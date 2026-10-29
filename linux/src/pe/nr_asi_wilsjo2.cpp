#include "nr_gpu_completion.hpp"
// The wilsjo2 style's per-frame flow on the .asi route: see nr_asi_wilsjo2.hpp. Section numbers are those of
// our specification of the style (not published). Where the forked OptiScaler as the 0.0.7 route ran it differs from the
// specification by one of our OptiScaler fixes (nr_pe_optifix.cpp), the fix is what is followed, and said so below.
#include "nr_asi_wilsjo2.hpp"

#include "nr_pe_config.hpp"
#include "nr_pe_log.hpp"
#include "nr_pe_renodx_route.hpp"
#include "nr_pe_session.hpp"
#include "nr_vram_ledger.hpp"
#include "nr_wilsjo2_flow.hpp"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace nr::asi {
namespace {

using nr::pe::log;
namespace w2 = nr::wilsjo2;

// ---- our D3D12 surfaces ----------------------------------------------------------------------------------------
// Retired, not released at once: the GPU may still run a frame that uses them, in a command list the game has not
// even submitted yet. Released once 64 more runs went by AND 2 s passed (a run count alone goes by fast with several
// layers a frame, and a frame's lists are on the GPU within a few frames, far under 2 s).
nr::gpu::Uses gpu_uses;
struct Retired {
    nr::gpu::Uses uses;
    uint64_t at;
    ULONGLONG ms;
    ID3D12Resource* r;
};
std::vector<Retired> g_retired;
uint64_t g_clock = 0;
constexpr uint64_t kRetireRuns = 64;
constexpr ULONGLONG kRetireMs = 2000;

void retire(ID3D12Resource*& r) {
    if (r) g_retired.push_back({gpu_uses, g_clock + kRetireRuns, GetTickCount64() + kRetireMs, r});
    r = nullptr;
}

void tick() {
    gpu_uses.touch();
    ++g_clock;
    if (g_retired.empty()) return;
    const ULONGLONG now = GetTickCount64();
    g_retired.erase(std::remove_if(g_retired.begin(), g_retired.end(),
                                   [now](const Retired& e) {
                                       if (e.at > g_clock || e.ms > now || !e.uses.ready()) return false;
                                       e.r->Release();
                                       return true;
                                   }),
                    g_retired.end());
}

// Spec 4.8 table: default heap, one mip, UAV allowed, in the frame's format; UNORDERED_ACCESS between uses.
ID3D12Resource* make_surface(ID3D12Device* device, DXGI_FORMAT format, uint32_t w, uint32_t h) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w; d.Height = h; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.Format = format; d.SampleDesc.Count = 1;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ID3D12Resource* r = nullptr;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                               nullptr, IID_PPV_ARGS(&r))))
        return nullptr;
    nr::vram::track_d3d12(device, r, nr::vram::kWilsjo2);
    return r;
}

void transition(ID3D12GraphicsCommandList* cmd, ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    if (!r || from == to) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    cmd->ResourceBarrier(1, &b);
}

constexpr D3D12_RESOURCE_STATES kUav = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
constexpr D3D12_RESOURCE_STATES kSrv = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

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

w2::Image image_of(const nr::pe::Session::RawImage& r) {
    w2::Image i;
    i.image = r.image; i.format = r.format; i.width = r.width; i.height = r.height; i.layout = r.layout;
    return i;
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


}  // namespace

void wilsjo2_abandon_at_exit() { (void)g_stage.release(); }

// One OptiScaler feature's wilsjo2 state at one placement: the flow's (nr_wilsjo2_flow.hpp) and the passes' models
// (each pass its own feature, so its own history).
struct Wilsjo2Owner {
    bool capture_layer = false;
    bool at_present = false;   // wilsjo2_owner_at_present
    w2::FlowState flow;
    std::vector<nr::dlssnr::Feature*> models;      // pass k's at [k]
    std::map<ID3D12Resource*, bool> colour;        // our surfaces: colour-family (else read as a single-channel image)
};

Wilsjo2Owner* wilsjo2_owner() { return new Wilsjo2Owner(); }
void wilsjo2_owner_at_present(Wilsjo2Owner& o) { o.at_present = true; }

namespace {
std::mutex g_spatial_lock;
std::map<int, w2::SpatialLayout> g_spatial_ran, g_spatial_present;   // layer row -> its last run with spatial compression
}  // namespace
void wilsjo2_note_spatial(int row, const w2::SpatialLayout& layout) {
    std::lock_guard<std::mutex> g(g_spatial_lock);
    g_spatial_ran[std::max(row, 1)] = layout;
}
void wilsjo2_present_spatial() {
    std::lock_guard<std::mutex> g(g_spatial_lock);
    g_spatial_present.swap(g_spatial_ran);
    g_spatial_ran.clear();
}
w2::SpatialLayout wilsjo2_spatial_layout(int row) {
    std::lock_guard<std::mutex> g(g_spatial_lock);
    const auto it = g_spatial_present.find(std::max(row, 1));
    return it == g_spatial_present.end() ? w2::SpatialLayout{} : it->second;
}
bool wilsjo2_spatial_running(int row) {
    std::lock_guard<std::mutex> g(g_spatial_lock);
    return g_spatial_present.count(std::max(row, 1)) != 0;
}

void wilsjo2_tick() { tick(); }

namespace {
// The flow's route on D3D12 (nr_wilsjo2_flow.hpp FlowIo): our surfaces are committed resources in UNORDERED_ACCESS
// between uses, read as NON_PIXEL_SHADER_RESOURCE; the game's guides read in NON_PIXEL_SHADER_RESOURCE and given back in
// their states; the stage's dispatches recorded into the game's list through the model's session (record_raw); the
// model through nr::dlssnr::evaluate_d3d12.
DXGI_FORMAT dxgi_of(w2::Fmt f, DXGI_FORMAT frame) {
    switch (f) {
        case w2::Fmt::kFrame: return frame;
        case w2::Fmt::kHalf: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case w2::Fmt::kR32F: return DXGI_FORMAT_R32_FLOAT;
        case w2::Fmt::kRG32F: return DXGI_FORMAT_R32G32_FLOAT;
        case w2::Fmt::kF32: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case w2::Fmt::kRG16F: return DXGI_FORMAT_R16G16_FLOAT;
    }
    return frame;
}

class D3d12Io final : public w2::FlowIo {
public:
    D3d12Io(ID3D12GraphicsCommandList* cmd, ID3D12Device* device, Wilsjo2Owner& o, const Wilsjo2Frame* f,
            ID3D12Resource* target, bool before_upscale, uint32_t w, uint32_t h)
        : cmd_(cmd), device_(device), o_(o), f_(f), before_(before_upscale), w_(w), h_(h) {
        if (target) { frame_ = target->GetDesc().Format; track(target, kUav, true); }
        if (f) {
            if (f->depth) track(f->depth, f->depth_state, false);
            if (f->motion && f->motion != f->depth) track(f->motion, f->motion_state, false);
            if (f->exposure && !states_.count(f->exposure)) track(f->exposure, f->exposure_state, false);
        }
    }
    w2::Surf make(w2::Fmt format, uint32_t w, uint32_t h, bool colour) override {
        ID3D12Resource* r = make_surface(device_, dxgi_of(format, frame_), w, h);
        if (!r) return {};
        o_.colour[r] = colour;
        return {r};
    }
    void retire(w2::Surf& s) override {
        auto* r = static_cast<ID3D12Resource*>(s.h);
        if (r) { o_.colour.erase(r); states_.erase(r); }
        ::nr::asi::retire(r);
        s = {};
    }
    void use(w2::Surf s, w2::Access a) override {
        auto* r = static_cast<ID3D12Resource*>(s.h);
        if (!r) return;
        auto& st = state(r);
        const D3D12_RESOURCE_STATES want = a == w2::Access::kRead ? kSrv : kUav;
        transition(cmd_, r, st.now, want);
        st.now = want;
    }
    bool record(const Recorder& fn) override {
        bool ok = false;
        nr::dlssnr::with_d3d12_session([&](nr::pe::Session& session) {
            session_ = &session;
            ok = session.record_raw(device_, cmd_, [&](VkCommandBuffer vk, const HostDevice& host,
                                                       const nr::pe::Session::RawLookup& lookup) {
                if (!g_stage) {   // the programs, on the game's device, at the first frame
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
                const ImageOf image_of = [&](w2::Surf s) {
                    auto* r = static_cast<ID3D12Resource*>(s.h);
                    if (!r) return w2::Image{};
                    const auto& st = state(r);
                    return ::nr::asi::image_of(lookup(r, st.now, st.colour));
                };
                return fn(vk, *g_stage, image_of);
            });
            if (g_stage) g_stage->end_recording();
            session_ = nullptr;
        });
        return ok;
    }
    bool has(w2::Mode mode) const override { return g_stage && g_stage->has(mode); }
    bool has_fast() const override { return !g_stage || g_stage->has_fast(); }
    bool stage_failed() const override { return g_stage_retry.blocked(); }
    w2::PassReady pass(uint32_t k, const w2::PassControls& pc, uint32_t mw, uint32_t mh) override {
        if (k < o_.models.size() && o_.models[k]) return w2::PassReady::kReady;
        int rc = 0;
        nr::dlssnr::Feature* m = nr::dlssnr::create_d3d12(device_, cmd_, nullptr, mw, mh, int(pc.preset), 1, controls_of(pc), &rc);
        if (!m) {
            log("[nr] wilsjo2 style: pass %u's feature could not be made (0x%08X): %s", k + 1, unsigned(rc),
                nr::dlssnr::last_error());
            return w2::PassReady::kFailed;
        }
        if (o_.models.size() <= k) o_.models.resize(k + 1, nullptr);
        o_.models[k] = m;
        return w2::PassReady::kMade;
    }
    void release_passes(size_t from) override {
        for (size_t k = from; k < o_.models.size(); ++k)
            if (o_.models[k]) nr::dlssnr::release_d3d12(o_.models[k]);
        o_.models.resize(std::min(from, o_.models.size()));
    }
    int network(const w2::NetCall& c) override {
        namespace route = nr::pe::renodx_route;
        route::begin_run(rf_);
        const auto rect = [](const w2::Rect& r) { nr::dlssnr::Rect o; o.x = r.x; o.y = r.y; o.width = r.width; o.height = r.height; return o; };
        const int result = nr::dlssnr::evaluate_d3d12(
            cmd_, o_.models[c.pass], nullptr, static_cast<ID3D12Resource*>(c.input.h), static_cast<ID3D12Resource*>(c.depth.h),
            static_cast<ID3D12Resource*>(c.motion.h), static_cast<ID3D12Resource*>(c.output.h), c.width, c.height,
            rect(c.depth_rect), rect(c.motion_rect), c.depth_inverted ? 1 : 0, c.reset ? 1 : 0,
            controls_of(c.controls, c.noise_every_frame, c.proxy_curve, c.last), c.mv_x, c.mv_y);
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
        rf_.exposure = f_->exposure;
        rf_.exposure_state = uint32_t(f_->exposure_state);
    }
    void passes_end() override { told_.reset(); style_.reset(); }
    void run_begin(VkCommandBuffer vk, const w2::Image& frame, bool hdr) override {
        session_->set_call_layer(nr::pe::renodx_route::call_layer());   // the run's whole time, for NR Control
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
        for (auto& [r, st] : states_) { transition(cmd_, r, st.now, st.home); st.now = st.home; }
        if (o_.capture_layer) {
            nr::dlssnr::with_d3d12_session([](nr::pe::Session& s) { s.capture_abort(); });
            o_.capture_layer = false;
        }
    }
    void say(const char* text) override { log("[nr] wilsjo2 style: %s", text); }
    void spatial_ran(const w2::SpatialLayout& layout) override { wilsjo2_note_spatial(nr::pe::renodx_route::call_layer(), layout); }
    DXGI_FORMAT frame_format() const { return frame_; }

private:
    struct State { D3D12_RESOURCE_STATES now, home; bool colour; };
    void track(ID3D12Resource* r, D3D12_RESOURCE_STATES s, bool colour) { states_[r] = State{s, s, colour}; }
    State& state(ID3D12Resource* r) {
        auto it = states_.find(r);
        if (it == states_.end()) {   // one of ours: made in UNORDERED_ACCESS, kept there between runs
            const auto c = o_.colour.find(r);
            it = states_.emplace(r, State{kUav, kUav, c == o_.colour.end() || c->second}).first;
        }
        return it->second;
    }
    ID3D12GraphicsCommandList* cmd_;
    ID3D12Device* device_;
    Wilsjo2Owner& o_;
    const Wilsjo2Frame* f_;
    bool before_;
    uint32_t w_, h_;
    DXGI_FORMAT frame_ = DXGI_FORMAT_UNKNOWN;
    std::map<ID3D12Resource*, State> states_;
    nr::pe::Session* session_ = nullptr;
    std::optional<nr::pe::renodx_route::CallStyle> style_;
    nr::pe::renodx_route::Wilsjo2Facts facts_;
    std::optional<nr::pe::renodx_route::CallWilsjo2> told_;
    nr::pe::renodx_route::Frame rf_;
};
}  // namespace

void wilsjo2_release(Wilsjo2Owner* o) {
    if (!o) return;
    D3d12Io io(nullptr, nullptr, *o, nullptr, nullptr, false, 0, 0);
    w2::flow_release(io, o->flow);
    delete o;
}

w2::Settings wilsjo2_settings(const nr::pe::LayerConfig& layer) {
    w2::Settings s;
    for (const auto& [key, value] : layer.keys) {
        std::string k = key;
        for (char& c : k) c = char(std::tolower(static_cast<unsigned char>(c)));
        if (k == "runbeforesr") continue;   // the layer's Placement says it
        if (!w2::parse_key(s, key, value)) {
            static std::set<std::string> said;
            if (said.insert(key).second)
                log("[nr] wilsjo2 processing: dlssnr-amd.ini [Layer%d] %s = %s not understood", layer.number, key.c_str(),
                    value.c_str());
        }
    }
    s.run_before_sr = layer.placement == nr::pe::Placement::kBefore;
    return s;
}

bool wilsjo2_run(ID3D12GraphicsCommandList* cmd, ID3D12Device* device, Wilsjo2Owner& o, const Wilsjo2Frame& f,
                 const w2::Settings& s, ID3D12Resource* target, uint32_t w, uint32_t h, bool before_upscale) {
    gpu_uses.touch();
    o.capture_layer = false;
    if (!target || !f.depth || !f.motion) return false;
    D3d12Io io(cmd, device, o, &f, target, before_upscale, w, h);
    w2::FlowFrame ff;
    ff.target = {target};
    ff.width = w; ff.height = h;
    const DXGI_FORMAT format = io.frame_format();
    switch (format) {
        case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8X8_UNORM:
        case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R16G16B16A16_UNORM:
            ff.unorm = true;
            break;
        default:
            break;
    }
    ff.direct_ok = true;   // D3D12: the frame is a UAV the stage reads and writes at its extent
    ff.depth = {f.depth};
    ff.motion = {f.motion};
    {
        const D3D12_RESOURCE_DESC dd = f.depth->GetDesc(), md = f.motion->GetDesc();
        char g[192];
        std::snprintf(g, sizeof g, "%u:%llux%u:%u:%llux%u:%u,%u,%u,%u:%u,%u,%u,%u", unsigned(dd.Format),
                      (unsigned long long)dd.Width, dd.Height, unsigned(md.Format), (unsigned long long)md.Width, md.Height,
                      f.depth_rect.x, f.depth_rect.y, f.depth_rect.width, f.depth_rect.height, f.motion_rect.x,
                      f.motion_rect.y, f.motion_rect.width, f.motion_rect.height);
        ff.guide_signature = g;
    }
    ff.depth_rect = {f.depth_rect.x, f.depth_rect.y, f.depth_rect.width, f.depth_rect.height};
    ff.motion_rect = {f.motion_rect.x, f.motion_rect.y, f.motion_rect.width, f.motion_rect.height};
    if (f.exposure) {
        ff.exposure = {f.exposure};
        const D3D12_RESOURCE_DESC ed = f.exposure->GetDesc();
        // Any size, as the fork binds it: the programs read its texel (0, 0) only (DL:TB's is 2 x 1, R32_TYPELESS,
        // read as float by the lookup). A 2-D single-sample texture is what a sampler2D can be bound to.
        ff.exposure_valid = ed.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && ed.SampleDesc.Count == 1 &&
                            ed.DepthOrArraySize == 1;
    }
    ff.flags = f.flags;
    ff.hdr = f.hdr;
    ff.reset = f.reset;
    ff.pre_exposure = f.pre_exposure;
    ff.mv_x = f.mv_x; ff.mv_y = f.mv_y;
    ff.preprocess = nr::dlssnr::preprocess_settings();
    ff.before_upscale = before_upscale;
    ff.at_present = o.at_present;
    return w2::flow_run(io, o.flow, ff, s, uint64_t(format));
}

bool wilsjo2_convert(ID3D12GraphicsCommandList* cmd, ID3D12Device* device, ID3D12Resource* source,
                     ID3D12Resource* original, ID3D12Resource* target, uint32_t w, uint32_t h, bool to_linear) {
    if (g_stage_retry.blocked()) return false;
    bool recorded = false;
    nr::dlssnr::with_d3d12_session([&](nr::pe::Session& session) {
        recorded = session.record_raw(device, cmd, [&](VkCommandBuffer vk, const HostDevice& host,
                                                       const nr::pe::Session::RawLookup& lookup) {
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
            w2::Dispatch d;
            d.t0 = image_of(lookup(source, kSrv, true));
            if (!to_linear) d.t2 = image_of(lookup(original, kSrv, true));
            d.u0 = image_of(lookup(target, kUav, true));
            if (!d.t0.format || !d.u0.format || (!to_linear && !d.t2.format)) return false;
            d.constants.mode = to_linear ? w2::kFinishedToLinear : w2::kFinishedToPq;
            d.constants.width = w;
            d.constants.height = h;
            if (!g_stage->has(w2::Mode(d.constants.mode))) return false;   // the conversion's program missing
            g_stage->record(vk, d);
            return true;
        });
        if (g_stage) g_stage->end_recording();
    });
    return recorded;
}

bool wilsjo2_hudfix(ID3D12GraphicsCommandList* cmd, ID3D12Device* device, const HudFixD3D12& r, uint32_t w, uint32_t h,
                    uint32_t source, bool show_mask, bool fresh, uint32_t parity, bool estimate_too) {
    if (g_stage_retry.blocked()) return false;
    bool recorded = false;
    nr::dlssnr::with_d3d12_session([&](nr::pe::Session& session) {
        recorded = session.record_raw(device, cmd, [&](VkCommandBuffer vk, const HostDevice& host,
                                                       const nr::pe::Session::RawLookup& lookup) {
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
            const w2::Image back = image_of(lookup(r.back, kUav, true)), frame = image_of(lookup(r.frame, kUav, true)),
                            alpha = image_of(lookup(r.alpha, kUav, true)), stats = image_of(lookup(r.stats, kUav, true));
            const w2::Image ref = source == 4 ? image_of(lookup(r.previous, kUav, true))
                                  : r.hudless ? image_of(lookup(r.hudless, kUav, true)) : back;
            const bool warm = estimate_too && source == 2 && r.previous;
            const w2::Image motion = r.motion ? image_of(lookup(r.motion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, false))
                                              : w2::Image{};
            const w2::Image prev = warm ? image_of(lookup(r.previous, kUav, true)) : ref;
            if (!back.format || !frame.format || !alpha.format || !stats.format || !ref.format || !prev.format) return false;
            uint32_t src = source;
            const auto run = [&](uint32_t mode, const w2::Image& t0, const w2::Image* t1, const w2::Image* t2,
                                 const w2::Image* t3, const w2::Image& u0, uint32_t width, uint32_t height) {
                w2::Dispatch d;
                d.t0 = t0;
                if (t1) d.t1 = *t1;
                if (t2) d.t2 = *t2;
                if (t3) d.t3 = *t3;
                d.u0 = u0;
                d.u1 = stats;
                d.constants = w2::hudfix_constants(mode, width, height, src, show_mask, fresh, parity);
                if (mode == w2::kHudEstimate && motion.format) {   // the estimate with the game's motion vectors
                    d.t2 = motion;
                    w2::hudfix_motion(d.constants, r.motion_scale_x, r.motion_scale_y, r.motion_x, r.motion_y,
                                      r.motion_step_x, r.motion_step_y);
                }
                g_stage->record(vk, d);
            };
            try {
                run(w2::kHudClear, back, nullptr, nullptr, nullptr, frame, 1, 1);
                if (warm) {   // the estimate's state, this frame against the last
                    src = 4;
                    run(w2::kHudCount, back, &prev, nullptr, nullptr, frame, w, h);
                    run(w2::kHudEstimate, back, &prev, nullptr, nullptr, alpha, w, h);
                    src = source;
                }
                if (source == 2 || source == 4) run(w2::kHudCount, back, &ref, nullptr, nullptr, frame, w, h);
                if (source == 4) run(w2::kHudEstimate, back, &ref, nullptr, nullptr, alpha, w, h);
                run(w2::kHudCompose, back, &ref, &frame, &alpha, frame, w, h);
            } catch (const std::exception& x) {
                log("[nr] HUD FIX: %s", x.what());
                return false;
            }
            return true;
        });
        if (g_stage) g_stage->end_recording();
    });
    return recorded;
}

bool wilsjo2_inspect(ID3D12GraphicsCommandList* cmd, ID3D12Device* device, ID3D12Resource* before, ID3D12Resource* after,
                     ID3D12Resource* target, uint32_t w, uint32_t h, uint32_t compare, float split, float zoom, bool swap,
                     bool apply_model) {
    if (g_stage_retry.blocked()) return false;
    bool recorded = false;
    nr::dlssnr::with_d3d12_session([&](nr::pe::Session& session) {
        recorded = session.record_raw(device, cmd, [&](VkCommandBuffer vk, const HostDevice& host,
                                                       const nr::pe::Session::RawLookup& lookup) {
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
            w2::Dispatch d;
            d.t0 = image_of(lookup(before, kSrv, true));
            d.t1 = image_of(lookup(after, kSrv, true));
            d.u0 = image_of(lookup(target, kUav, true));
            if (!d.t0.format || !d.t1.format || !d.u0.format) return false;
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
        if (g_stage) g_stage->end_recording();
    });
    return recorded;
}

}  // namespace nr::asi
