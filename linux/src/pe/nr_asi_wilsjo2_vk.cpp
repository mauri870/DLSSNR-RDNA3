#include "nr_gpu_completion.hpp"
// The wilsjo2 style's per-frame flow on a Vulkan game (nr_asi_wilsjo2.hpp, the Vulkan section): the flow of
// nr_asi_wilsjo2.cpp step for step (section numbers as there; where a step
// says why, the D3D12 file says it at length), on the game's own command buffer. What differs is only what Vulkan
// makes differ: our surfaces are VkImages kept in GENERAL (read and written in it, so a D3D12 state change between
// uses is a memory barrier here, which every dispatch of the stage already records after itself), the game's images
// are read in the layouts the call hands them, and the model runs through nr::dlssnr::evaluate_vk_asi.
#include "nr_asi_wilsjo2.hpp"

#include "nr_pe_config.hpp"
#include "nr_pe_log.hpp"
#include "nr_pe_renodx_route.hpp"
#include "nr_pe_session.hpp"
#include "nr_vram_tag.hpp"
#include "nr_wilsjo2_flow.hpp"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace nr::asi {
namespace {

using nr::pe::log;
namespace w2 = nr::wilsjo2;

VkDevice g_device = VK_NULL_HANDLE;
VkPhysicalDevice g_physical = VK_NULL_HANDLE;
VkPhysicalDeviceMemoryProperties g_memory{};

// ---- our Vulkan surfaces ---------------------------------------------------------------------------------------
struct Surface {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0, height = 0;
    explicit operator bool() const { return image != VK_NULL_HANDLE; }
};

void destroy(Surface& s) {
    if (s.image) vkDestroyImage(g_device, s.image, nullptr);
    if (s.memory) vkFreeMemory(g_device, s.memory, nullptr);
    s = Surface{};
}

// Retired as the D3D12 surfaces are: released once 64 more runs went by AND 2 s passed.
nr::gpu::Uses gpu_uses;
struct Retired {
    nr::gpu::Uses uses;
    uint64_t at;
    ULONGLONG ms;
    Surface s;
};
std::vector<Retired> g_retired;
uint64_t g_clock = 0;
constexpr uint64_t kRetireRuns = 64;
constexpr ULONGLONG kRetireMs = 2000;

void retire(Surface& s) {
    if (s.image) g_retired.push_back({gpu_uses, g_clock + kRetireRuns, GetTickCount64() + kRetireMs, s});
    s = Surface{};
}

void tick() {
    gpu_uses.touch();
    ++g_clock;
    if (g_retired.empty()) return;
    const ULONGLONG now = GetTickCount64();
    for (size_t i = g_retired.size(); i-- > 0;)
        if (g_retired[i].at <= g_clock && g_retired[i].ms <= now && g_retired[i].uses.ready()) {
            destroy(g_retired[i].s);
            g_retired.erase(g_retired.begin() + long(i));
        }
}

// Spec 4.8 table: device-local, one mip, storage and sampled, in the frame's format; GENERAL from the first use (the
// layout change recorded into `cmd` here, before anything reads it).
Surface make_surface(VkCommandBuffer cmd, VkFormat format, uint32_t w, uint32_t h) {
    Surface s;
    if (!g_device || !format) return s;
    VkFormatProperties fp{};
    vkGetPhysicalDeviceFormatProperties(g_physical, format, &fp);
    const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    if ((fp.optimalTilingFeatures & need) != need) return s;
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D; ii.format = format; ii.extent = {w, h, 1}; ii.mipLevels = 1; ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
               VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(g_device, &ii, nullptr, &s.image) != VK_SUCCESS) { s.image = VK_NULL_HANDLE; return s; }
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(g_device, s.image, &req);
    uint32_t type = ~0u;
    for (uint32_t i = 0; i < g_memory.memoryTypeCount; ++i)
        if ((req.memoryTypeBits & (1u << i)) && (g_memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            type = i;
            break;
        }
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size; ai.memoryTypeIndex = type;
    {
        nr::vram::Scope tag(nr::vram::kWilsjo2);
        if (type == ~0u || vkAllocateMemory(g_device, &ai, nullptr, &s.memory) != VK_SUCCESS) {
            s.memory = VK_NULL_HANDLE;
            destroy(s);
            return s;
        }
    }
    if (vkBindImageMemory(g_device, s.image, s.memory, 0) != VK_SUCCESS) { destroy(s); return s; }
    s.format = format; s.width = w; s.height = h;
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = 0;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = s.image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
    return s;
}

w2::Image image_of(const Surface& s) {
    w2::Image i;
    i.image = s.image; i.format = s.format; i.width = s.width; i.height = s.height; i.layout = VK_IMAGE_LAYOUT_GENERAL;
    return i;
}
w2::Image image_of(const Wilsjo2VkImage& g) {
    w2::Image i;
    i.image = g.image; i.format = g.format; i.width = g.width; i.height = g.height; i.layout = g.layout;
    return i;
}
nr::dlssnr::VkImageRef ref_of(const Surface& s) {
    nr::dlssnr::VkImageRef r;
    r.image = uint64_t(reinterpret_cast<uintptr_t>(s.image));
    r.format = uint32_t(s.format); r.width = s.width; r.height = s.height;
    r.layout = uint32_t(VK_IMAGE_LAYOUT_GENERAL);
    r.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
              VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    return r;
}
nr::dlssnr::VkImageRef ref_of(const Wilsjo2VkImage& g) {   // a guide of the game's, read where it is
    nr::dlssnr::VkImageRef r;
    r.image = uint64_t(reinterpret_cast<uintptr_t>(g.image));
    r.format = uint32_t(g.format); r.width = g.width; r.height = g.height;
    r.layout = uint32_t(g.layout);
    r.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    return r;
}

// The programs, one set for the process (made on the game's device at first use).
std::unique_ptr<w2::Stage> g_stage;
// The Stage could not be made (its programs, memory): tried again 5 s later rather than never (a passing shortage does
// not keep wilsjo2 off for the run); said once, and once when it comes back.
struct StageRetry {
    ULONGLONG failed_ms = 0;
    bool said = false;
    bool blocked() const { return failed_ms && GetTickCount64() - failed_ms < 5000; }
    void fail(const char* what) {
        failed_ms = GetTickCount64();
        if (!said) { said = true; log("[nr] wilsjo2 style unavailable: %s (tried again every 5 s)", what); }
    }
    void made() {
        if (failed_ms) log("[nr] wilsjo2 style available again");
        failed_ms = 0;
    }
};
StageRetry g_stage_retry;
// NR_TEST_STAGE_FAIL=<n> (tests): the first n Stages are not made (a passing failure, retried)
bool stage_fail_test() {
    static int left = [] { const char* e = std::getenv("NR_TEST_STAGE_FAIL"); return e ? std::atoi(e) : 0; }();
    if (left <= 0) return false;
    --left;
    return true;
}

// `noise`: the layer's NoiseEveryFrame (ours), for frames without motion vectors
nr::dlssnr::Controls6 controls_of(const w2::PassControls& pc, bool noise = false, uint32_t proxy_curve = 0, bool capture = true) {
    nr::dlssnr::Controls6 c;
    c.intensity = pc.intensity;
    c.style = int(pc.style);
    c.local_structure = pc.local_structure;
    c.local_tone = pc.local_tone;
    c.skin_structure = pc.skin_structure;
    c.use_auto_mask = pc.auto_mask ? 1 : 0;
    c.noise_every_frame = noise ? 1 : 0;
    c.proxy_curve = int(proxy_curve);
    c.model_capture_allowed = capture;
    return c;
}

// The stage's dispatches recorded into the game's command buffer through the model's Vulkan session (its device;
// full memory barriers around them).
bool record(VkCommandBuffer cmd, const std::function<bool(VkCommandBuffer, nr::pe::Session&)>& fn) {
    bool ok = false;
    nr::dlssnr::with_vk_session([&](nr::pe::Session& session) {
        ok = session.record_raw_vk(cmd, [&](VkCommandBuffer vk, const HostDevice& host) {
            if (!g_stage) {
                try {
                    if (stage_fail_test()) throw std::runtime_error("NR_TEST_STAGE_FAIL");
                    g_stage = std::make_unique<w2::Stage>(host, std::string(nr::pe::module_folder()) +
                                                                    "\\dlssnr-amd\\shaders\\wilsjo2");
                    g_stage_retry.made();
                } catch (const std::exception& x) {
                    g_stage_retry.fail(x.what());
                    return false;
                }
            }
            return fn(vk, session);
        });
        if (g_stage) g_stage->end_recording();
    });
    return ok;
}

}  // namespace

// One Vulkan game feature's wilsjo2 state at one placement: the flow's (nr_wilsjo2_flow.hpp) and the passes' models.
struct Wilsjo2VkOwner {
    bool capture_layer = false;
    bool at_present = false;   // wilsjo2_vk_owner_at_present
    w2::FlowState flow;
    std::vector<nr::dlssnr::Feature*> models;   // pass k's at [k]
};

Wilsjo2VkOwner* wilsjo2_vk_owner() { return new Wilsjo2VkOwner(); }
void wilsjo2_vk_owner_at_present(Wilsjo2VkOwner& o) { o.at_present = true; }

void wilsjo2_vk_tick() { tick(); }

void wilsjo2_vk_device(VkDevice device, VkPhysicalDevice physical) {
    if (g_device == device || !device || !physical) return;
    g_device = device;
    g_physical = physical;
    vkGetPhysicalDeviceMemoryProperties(physical, &g_memory);
}

void wilsjo2_vk_abandon_at_exit() {
    (void)g_stage.release();
    g_retired.clear();   // plain records (the surfaces they name are left)
}

void wilsjo2_vk_device_destroyed() {    for (auto& r : g_retired) destroy(r.s);
    g_retired.clear();
    g_stage.reset();
    g_stage_retry = StageRetry{};
    g_device = VK_NULL_HANDLE;
    g_physical = VK_NULL_HANDLE;
}

namespace {
VkFormat vk_of(w2::Fmt f, VkFormat frame) {
    switch (f) {
        case w2::Fmt::kFrame: return frame;
        case w2::Fmt::kHalf: return VK_FORMAT_R16G16B16A16_SFLOAT;
        case w2::Fmt::kR32F: return VK_FORMAT_R32_SFLOAT;
        case w2::Fmt::kRG32F: return VK_FORMAT_R32G32_SFLOAT;
        case w2::Fmt::kF32: return VK_FORMAT_R32G32B32A32_SFLOAT;
        case w2::Fmt::kRG16F: return VK_FORMAT_R16G16_SFLOAT;
    }
    return frame;
}

// The flow's route on a Vulkan game (nr_wilsjo2_flow.hpp FlowIo): our surfaces in GENERAL throughout (a dispatch's
// barrier after it is all a change of use needs), the game's images in the layouts the call hands them, the stage's
// dispatches on the game's command buffer through the model's session, the model through evaluate_vk_asi.
class VkIo final : public w2::FlowIo {
public:
    VkIo(VkCommandBuffer cmd, Wilsjo2VkOwner& o, const Wilsjo2VkFrame* f, const Wilsjo2VkImage* target, bool before_upscale,
         uint32_t w, uint32_t h)
        : cmd_(cmd), o_(o), f_(f), target_(target), before_(before_upscale), w_(w), h_(h) {}
    w2::Surf game(const Wilsjo2VkImage& g) { return g.image ? w2::Surf{const_cast<Wilsjo2VkImage*>(&g)} : w2::Surf{}; }
    w2::Surf make(w2::Fmt format, uint32_t w, uint32_t h, bool) override {
        Surface s = make_surface(cmd_, vk_of(format, target_->format), w, h);
        if (!s) return {};
        return {new Surface(s)};
    }
    void retire(w2::Surf& s) override {
        if (auto* x = static_cast<Surface*>(s.h)) { ::nr::asi::retire(*x); delete x; }
        s = {};
    }
    void use(w2::Surf, w2::Access) override {}
    bool record(const Recorder& fn) override {
        return ::nr::asi::record(cmd_, [&](VkCommandBuffer vk, nr::pe::Session& session) {
            session_ = &session;
            const ImageOf image_of = [&](w2::Surf s) {
                if (!s) return w2::Image{};
                if (is_game(s)) return ::nr::asi::image_of(*static_cast<const Wilsjo2VkImage*>(s.h));
                return ::nr::asi::image_of(*static_cast<const Surface*>(s.h));
            };
            const bool ok = fn(vk, *g_stage, image_of);
            session_ = nullptr;
            return ok;
        });
    }
    bool has(w2::Mode mode) const override { return g_stage && g_stage->has(mode); }
    bool has_fast() const override { return !g_stage || g_stage->has_fast(); }
    bool stage_failed() const override { return g_stage_retry.blocked(); }
    w2::PassReady pass(uint32_t k, const w2::PassControls&, uint32_t mw, uint32_t mh) override {
        if (k < o_.models.size() && o_.models[k]) return w2::PassReady::kReady;
        int rc = 0;
        nr::dlssnr::Feature* m = nr::dlssnr::create_vk_asi(mw, mh, &rc);
        if (!m) {
            log("[nr] wilsjo2 style (Vulkan): pass %u's feature could not be made (0x%08X)", k + 1, unsigned(rc));
            return w2::PassReady::kFailed;
        }
        if (o_.models.size() <= k) o_.models.resize(k + 1, nullptr);
        o_.models[k] = m;
        return w2::PassReady::kMade;
    }
    void release_passes(size_t from) override {
        for (size_t k = from; k < o_.models.size(); ++k)
            if (o_.models[k]) nr::dlssnr::release_vk(o_.models[k]);
        o_.models.resize(std::min(from, o_.models.size()));
    }
    int network(const w2::NetCall& c) override {
        namespace route = nr::pe::renodx_route;
        route::begin_run(rf_);
        const auto rect = [](const w2::Rect& r) { nr::dlssnr::Rect o; o.x = r.x; o.y = r.y; o.width = r.width; o.height = r.height; return o; };
        const int result = nr::dlssnr::evaluate_vk_asi(cmd_, o_.models[c.pass], ref(c.input), ref(c.output), ref(c.depth),
                                                       ref(c.motion), c.width, c.height, rect(c.depth_rect), rect(c.motion_rect),
                                                       c.depth_inverted ? 1 : 0, c.reset ? 1 : 0,
                                                       controls_of(c.controls, c.noise_every_frame, c.proxy_curve, c.last),
                                                       c.mv_x, c.mv_y);
        route::end_run();
        return result == int(NVSDK_NGX_Result_Success) ? 0 : 1;
    }
    void passes_begin(const w2::FlowFacts& x) override {
        namespace route = nr::pe::renodx_route;
        style_.emplace(false);
        facts_.hdr = x.hdr; facts_.exposure = int(x.exposure); facts_.asked = x.asked; facts_.white = x.white;
        facts_.history = x.history; facts_.mode = x.mode; facts_.passes = x.passes;
        facts_.prep_outside = x.prep_outside; facts_.prep_missing = x.prep_missing;
        facts_.fast_passes = x.fast_passes; facts_.fast_fallback = x.fast_fallback;
        told_.emplace(&facts_);
        route::note_pass(f_->flags, 1.0f, f_->depth_rect.width, f_->depth_rect.height, w_, h_);
        rf_.before_upscale = before_;
        rf_.pre_exposure = f_->pre_exposure;
        if (f_->exposure.image) {
            rf_.vk_exposure = uint64_t(reinterpret_cast<uintptr_t>(f_->exposure.image));
            rf_.vk_exposure_format = uint32_t(f_->exposure.format);
            rf_.vk_exposure_width = f_->exposure.width;
            rf_.vk_exposure_height = f_->exposure.height;
        }
    }
    void passes_end() override { told_.reset(); style_.reset(); }
    void run_begin(VkCommandBuffer vk, const w2::Image& frame, bool hdr) override {
        session_->set_call_layer(nr::pe::renodx_route::call_layer());
        session_->outer_begin(vk);
        // F5 (nr_screenshot.hpp): the frame before this layer's NR, while a capture is armed
        o_.capture_layer = session_->capture_begin(vk, frame.image, frame.format, w_, h_, frame.layout, hdr ? 1 : 0,
                               o_.at_present ? "present" : before_ ? "nr-before-upscaler" : "nr-after-upscaler");
    }
    void run_end(VkCommandBuffer vk, const w2::Image& frame) override {
        session_->set_call_layer(nr::pe::renodx_route::call_layer());
        if (o_.capture_layer) { session_->capture_end(vk, frame.image, frame.layout); o_.capture_layer = false; }   // and after it
        session_->outer_end(vk);
    }
    void finish() override {
        if (o_.capture_layer) {
            nr::dlssnr::with_vk_session([](nr::pe::Session& s) { s.capture_abort(); });
            o_.capture_layer = false;
        }
    }
    void say(const char* text) override { log("[nr] wilsjo2 style (Vulkan): %s", text); }
    void spatial_ran(const w2::SpatialLayout& layout) override { wilsjo2_note_spatial(nr::pe::renodx_route::call_layer(), layout); }

private:
    bool is_game(w2::Surf s) const {
        return f_ && (s.h == target_ || s.h == &f_->depth || s.h == &f_->motion || s.h == &f_->exposure);
    }
    nr::dlssnr::VkImageRef ref(w2::Surf s) const {
        if (s.h == target_) {   // the frame itself (an SDR frame read where it is)
            nr::dlssnr::VkImageRef r;
            r.image = uint64_t(reinterpret_cast<uintptr_t>(target_->image));
            r.format = uint32_t(target_->format); r.width = target_->width; r.height = target_->height;
            r.layout = uint32_t(target_->layout); r.usage = target_->usage;
            return r;
        }
        if (is_game(s)) return ref_of(*static_cast<const Wilsjo2VkImage*>(s.h));
        return ref_of(*static_cast<const Surface*>(s.h));
    }
    VkCommandBuffer cmd_;
    Wilsjo2VkOwner& o_;
    const Wilsjo2VkFrame* f_;
    const Wilsjo2VkImage* target_;
    bool before_;
    uint32_t w_, h_;
    nr::pe::Session* session_ = nullptr;
    std::optional<nr::pe::renodx_route::CallStyle> style_;
    nr::pe::renodx_route::Wilsjo2Facts facts_;
    std::optional<nr::pe::renodx_route::CallWilsjo2> told_;
    nr::pe::renodx_route::Frame rf_;
};
}  // namespace

void wilsjo2_vk_release(Wilsjo2VkOwner* o) {
    if (!o) return;
    VkIo io(VK_NULL_HANDLE, *o, nullptr, nullptr, false, 0, 0);
    w2::flow_release(io, o->flow);
    delete o;
}

bool wilsjo2_run_vk(VkCommandBuffer cmd, Wilsjo2VkOwner& o, const Wilsjo2VkFrame& f, const w2::Settings& s,
                    const Wilsjo2VkImage& target, uint32_t w, uint32_t h, bool before_upscale) {
    gpu_uses.touch();
    o.capture_layer = false;
    if (!target.image || !f.depth.image || !f.motion.image || !g_device) return false;
    VkIo io(cmd, o, &f, &target, before_upscale, w, h);
    w2::FlowFrame ff;
    ff.target = io.game(target);
    ff.width = w; ff.height = h;
    switch (target.format) {
        case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_A8B8G8R8_UNORM_PACK32:
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32: case VK_FORMAT_A2R10G10B10_UNORM_PACK32: case VK_FORMAT_R16G16B16A16_UNORM:
            ff.unorm = true;
            break;
        default:
            break;
    }
    // the frame read and written in GENERAL as a sampled and storage image, at the frame's extent
    const VkImageUsageFlags rw_usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    ff.direct_ok = target.layout == VK_IMAGE_LAYOUT_GENERAL && (target.usage & rw_usage) == rw_usage &&
                   target.width == w && target.height == h;
    ff.depth = io.game(f.depth);
    ff.motion = io.game(f.motion);
    {
        char g[192];
        std::snprintf(g, sizeof g, "%d:%ux%u:%d:%ux%u:%u,%u,%u,%u:%u,%u,%u,%u", int(f.depth.format), f.depth.width,
                      f.depth.height, int(f.motion.format), f.motion.width, f.motion.height, f.depth_rect.x, f.depth_rect.y,
                      f.depth_rect.width, f.depth_rect.height, f.motion_rect.x, f.motion_rect.y, f.motion_rect.width,
                      f.motion_rect.height);
        ff.guide_signature = g;
    }
    ff.depth_rect = {f.depth_rect.x, f.depth_rect.y, f.depth_rect.width, f.depth_rect.height};
    ff.motion_rect = {f.motion_rect.x, f.motion_rect.y, f.motion_rect.width, f.motion_rect.height};
    ff.exposure = io.game(f.exposure);
    ff.exposure_valid = f.exposure.image != VK_NULL_HANDLE;   // any size, as the fork binds it: texel (0, 0) is read
    ff.flags = f.flags;
    ff.hdr = before_upscale ? f.hdr_before : f.hdr;   // the image processed decides
    ff.reset = f.reset;
    ff.pre_exposure = f.pre_exposure;
    ff.mv_x = f.mv_x; ff.mv_y = f.mv_y;
    ff.preprocess = nr::dlssnr::preprocess_settings();
    ff.before_upscale = before_upscale;
    ff.at_present = o.at_present;
    return w2::flow_run(io, o.flow, ff, s, uint64_t(target.format));
}

bool wilsjo2_convert_vk(VkCommandBuffer cmd, const Wilsjo2VkImage& source, const Wilsjo2VkImage* original,
                        const Wilsjo2VkImage& target, uint32_t w, uint32_t h, bool to_linear) {
    if (g_stage_retry.blocked() || (!to_linear && !original)) return false;
    return record(cmd, [&](VkCommandBuffer vk, nr::pe::Session&) {
        w2::Dispatch d;
        d.t0 = image_of(source);
        if (!to_linear) d.t2 = image_of(*original);
        d.u0 = image_of(target);
        d.constants.mode = to_linear ? w2::kFinishedToLinear : w2::kFinishedToPq;
        d.constants.width = w;
        d.constants.height = h;
        if (!g_stage->has(w2::Mode(d.constants.mode))) return false;   // the conversion's program missing
        g_stage->record(vk, d);
        return true;
    });
}

bool wilsjo2_copy_vk(VkCommandBuffer cmd, const VkCopySide& source, bool storage_read, uint32_t sx, uint32_t sy,
                     const VkCopySide& target, uint32_t dx, uint32_t dy, uint32_t w, uint32_t h) {
    if (g_stage_retry.blocked() || !source.image || !target.image || !w || !h) return false;
    const auto image = [](const VkCopySide& c) {
        w2::Image i;
        i.image = c.image; i.view = c.view; i.format = c.format; i.width = c.width; i.height = c.height;
        i.layout = c.layout; i.mip = c.mip; i.layer = c.layer;
        return i;
    };
    return record(cmd, [&](VkCommandBuffer vk, nr::pe::Session&) {
        const w2::Mode mode = storage_read ? w2::kCopyLoad : w2::kCopy;
        if (!g_stage->has(mode)) {
            static bool said = false;
            if (!said) { said = true; log("[nr] Vulkan route: the copy programs (wilsjo2_copy*.spv) are missing; a copy a transfer cannot make does not happen"); }
            return false;
        }
        w2::Dispatch d;
        d.u0 = image(target);
        if (storage_read) {
            d.u1 = image(source);
            d.u1.view = VK_NULL_HANDLE;   // a storage view of ours (the game's view may not allow storage)
            d.t0 = d.u0;                  // bound, not read
            d.t0.view = VK_NULL_HANDLE;
        } else {
            d.t0 = image(source);
        }
        d.constants = w2::copy_constants(mode, sx, sy, dx, dy, w, h);
        try {
            g_stage->record(vk, d);
        } catch (const std::exception& x) {
            log("[nr] Vulkan route: a copy could not be recorded: %s", x.what());
            return false;
        }
        return true;
    });
}

bool wilsjo2_hudfix_vk(VkCommandBuffer cmd, const HudFixVkImages& im, uint32_t w, uint32_t h, uint32_t source,
                       bool show_mask, bool fresh, uint32_t parity, bool estimate_too) {
    if (g_stage_retry.blocked()) return false;
    return record(cmd, [&](VkCommandBuffer vk, nr::pe::Session&) {
        const Wilsjo2VkImage& ref = source == 4 ? im.previous : im.hudless.image ? im.hudless : im.back;
        const bool warm = estimate_too && source == 2 && im.previous.image;
        uint32_t src = source;
        const auto run = [&](uint32_t mode, const Wilsjo2VkImage& t0, const Wilsjo2VkImage* t1, const Wilsjo2VkImage* t2,
                             const Wilsjo2VkImage* t3, const Wilsjo2VkImage& u0, uint32_t width, uint32_t height) {
            w2::Dispatch d;
            d.t0 = image_of(t0);
            if (t1) d.t1 = image_of(*t1);
            if (t2) d.t2 = image_of(*t2);
            if (t3) d.t3 = image_of(*t3);
            d.u0 = image_of(u0);
            d.u1 = image_of(im.stats);
            d.constants = w2::hudfix_constants(mode, width, height, src, show_mask, fresh, parity);
            if (mode == w2::kHudEstimate && im.motion.image) {   // the estimate with the game's motion vectors
                d.t2 = image_of(im.motion);
                w2::hudfix_motion(d.constants, im.motion_scale_x, im.motion_scale_y, im.motion_x, im.motion_y,
                                  im.motion_step_x, im.motion_step_y);
            }
            g_stage->record(vk, d);
        };
        try {
            run(w2::kHudClear, im.back, nullptr, nullptr, nullptr, im.frame, 1, 1);
            if (warm) {   // the estimate's state, this frame against the last
                src = 4;
                run(w2::kHudCount, im.back, &im.previous, nullptr, nullptr, im.frame, w, h);
                run(w2::kHudEstimate, im.back, &im.previous, nullptr, nullptr, im.alpha, w, h);
                src = source;
            }
            if (source == 2 || source == 4) run(w2::kHudCount, im.back, &ref, nullptr, nullptr, im.frame, w, h);
            if (source == 4) run(w2::kHudEstimate, im.back, &im.previous, nullptr, nullptr, im.alpha, w, h);
            run(w2::kHudCompose, im.back, &ref, &im.frame, &im.alpha, im.frame, w, h);
        } catch (const std::exception& x) {
            log("[nr] HUD FIX: %s", x.what());
            return false;
        }
        return true;
    });
}

bool wilsjo2_inspect_vk(VkCommandBuffer cmd, const Wilsjo2VkImage& before, const Wilsjo2VkImage& after,
                        const Wilsjo2VkImage& target, uint32_t w, uint32_t h, uint32_t compare, float split, float zoom,
                        bool swap, bool apply_model) {
    if (g_stage_retry.blocked()) return false;
    return record(cmd, [&](VkCommandBuffer vk, nr::pe::Session&) {
        w2::Dispatch d;
        d.t0 = image_of(before);
        d.t1 = image_of(after);
        d.u0 = image_of(target);
        d.constants.mode = w2::kInspect;
        d.constants.width = w;
        d.constants.height = h;
        d.constants.compare_mode = compare;
        d.constants.compare_split = split;
        d.constants.compare_zoom = zoom;
        d.constants.compare_swap = swap ? 1u : 0u;
        d.constants.apply_model = apply_model ? 1u : 0u;
        try {
            g_stage->record(vk, d);
        } catch (const std::exception& x) {
            log("[nr] NR comparison: %s", x.what());
            return false;
        }
        return true;
    });
}

}  // namespace nr::asi
