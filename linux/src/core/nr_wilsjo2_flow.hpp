#pragma once
// The wilsjo2 style's per-frame flow (section numbers below are those of our written specification of the style, not
// published): the frame encoded into the proxy, brought to the model's extent, [Preprocess] around the network, the
// network's passes, the answer brought back and resolved onto the frame - one flow for every route. What a route does
// differently is only how it holds images, records commands and calls the network: a FlowIo of its own
// (nr_asi_wilsjo2.cpp D3D12 resources, nr_asi_wilsjo2_vk.cpp the game's Vulkan images, the ReShade route's session).
// The arithmetic is in the programs (nr_wilsjo2.hpp Stage); this file only orders them.
#include "nr_wilsjo2.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

namespace nr::wilsjo2 {

// A surface of the route's: one of its own textures or one of the game's, by an opaque handle.
struct Surf {
    void* h = nullptr;
    explicit operator bool() const { return h != nullptr; }
    bool operator==(const Surf& o) const { return h == o.h; }
    bool operator!=(const Surf& o) const { return h != o.h; }
};
// The formats the flow makes surfaces in: the frame's own, 16-bit float RGBA, R32F, RG32F, 32-bit float RGBA.
enum class Fmt : uint8_t { kFrame, kHalf, kR32F, kRG32F, kF32, kRG16F };
// How the next step touches a surface: read by a shader (D3D12: NON_PIXEL_SHADER_RESOURCE) or written (UNORDERED_ACCESS).
enum class Access : uint8_t { kRead, kWrite };

struct Rect { uint32_t x = 0, y = 0, width = 0, height = 0; };

// One network pass: its input and answer (the route's surfaces), the guides, the model's extent and controls.
struct NetCall {
    uint32_t pass = 0;
    Surf input, output, depth, motion;
    uint32_t width = 0, height = 0;
    Rect depth_rect, motion_rect;
    Rect motion_texture_rect;   // the game's motion region (the MV scale's extent, also with peripheral compression)
    bool depth_inverted = false, reset = false, last = false;
    PassControls controls;
    bool noise_every_frame = false;
    uint32_t proxy_curve = 0;   // the layer's ReversibleMode (the network's proxy curve)
    float mv_x = 1.0f, mv_y = 1.0f;
};

// What NR Control is told of a run (the route forwards it).
struct FlowFacts {
    bool hdr = false;
    uint32_t exposure = 0;      // 0 manual, 1 the game's exposure, 3 automatic
    uint32_t asked = 0;         // WhitePointSource as set
    float white = 1.0f, history = 1.0f;
    uint32_t mode = 0, passes = 0;
    bool prep_outside = false, prep_missing = false;
    float fast_passes = 0.f;    // Passes (fast) running: its strength (0: legacy passes)
    bool fast_fallback = false; // Feedback / Hybrid asked for, Extrapolation run (no motion vectors / peripheral compression)
};

// A pass's model on the route: ready, made this frame (the frame is left as it came), or not to be had.
enum class PassReady : uint8_t { kReady, kMade, kFailed };

class FlowIo {
public:
    virtual ~FlowIo() = default;
    // A surface of ours, `colour`: a colour-family image (else a single-channel / meter one); null when it cannot be made.
    virtual Surf make(Fmt format, uint32_t w, uint32_t h, bool colour = true) = 0;
    virtual void retire(Surf& s) = 0;
    // Before the next step touches `s` so (D3D12: a state transition; Vulkan images stay GENERAL: nothing).
    virtual void use(Surf s, Access a) = 0;
    // Dispatches of the stage: `fn(cmd, stage, image_of)`, image_of(s) the surface as the step reads or writes it now
    // (format VK_FORMAT_UNDEFINED: not to be had). False: nothing was recorded.
    using ImageOf = std::function<Image(Surf)>;
    using Recorder = std::function<bool(VkCommandBuffer, Stage&, const ImageOf&)>;
    virtual bool record(const Recorder& fn) = 0;
    // Whether the stage (made at the first record) has the program of `mode`; false before it was made.
    virtual bool has(Mode mode) const = 0;
    // Whether Passes (fast)'s programs are there (true before the stage was made: they ship with it).
    virtual bool has_fast() const = 0;
    virtual bool stage_failed() const = 0;
    // Pass k's model for `pc` at the model extent: made if missing (kMade: the frame is left as it came).
    virtual PassReady pass(uint32_t k, const PassControls& pc, uint32_t model_w, uint32_t model_h) = 0;
    // The passes from `from` on go (their models released).
    virtual void release_passes(size_t from) = 0;
    // The network on one pass; 0 when it ran.
    virtual int network(const NetCall& call) = 0;
    // Around the passes: what NR Control is told, and the run's state for the model calls.
    virtual void passes_begin(const FlowFacts&) {}
    virtual void passes_end() {}
    // Inside the first and the last record of a run: the run's timing, the NR screenshot's before / after.
    virtual void run_begin(VkCommandBuffer, const Image& /*frame*/, bool /*hdr*/) {}
    virtual void run_end(VkCommandBuffer, const Image& /*frame*/) {}
    // Every exit of a run: the route's states back (D3D12: everything to its state at the call), an open capture
    // abandoned.
    virtual void finish() {}
    virtual void say(const char* text) = 0;
    // A run with peripheral compression done (NR Control's outlines).
    virtual void spatial_ran(const SpatialLayout&) {}
};

// What the route knows of the frame.
struct FlowFrame {
    Surf target;                  // the frame, read and written in place (Access::kWrite at the call)
    uint32_t width = 0, height = 0;
    bool unorm = false;           // the frame's format is a UNORM one
    bool direct_ok = false;       // the frame can be read where it is (format, layout, usage, extent: the route says)
    Surf depth, motion;           // the game's guides
    std::string guide_signature;  // their formats, sizes and regions (spatial compression resets on a change)
    Rect depth_rect, motion_rect;
    Surf exposure;                // the game's 1 x 1 exposure (empty: none)
    bool exposure_valid = false;  // it is a 2-D 1 x 1 single-sample texture
    uint32_t flags = 0;           // DLSS.Feature.Create.Flags as known
    bool hdr = false, reset = false;
    float pre_exposure = 1.0f, mv_x = 1.0f, mv_y = 1.0f;
    nr::Preprocess preprocess{};  // [Preprocess] this frame
    bool before_upscale = false, at_present = false;
    bool fast_motion = true;      // Passes (fast) Feedback / Hybrid may use the motion vectors (the ReShade route: their
                                  // provider runs; a texture can stay bound with it off)
};

// One layer's state on one route: its surfaces, the passes' controls, the latch.
struct FlowState {
    Surf proxy, keep, answer, scratch, clamp, small, native;
    Surf small_raw, small_keep;   // Downscale "bilinear before encoding": the frame at the work extent, its kept copy
    Surf classic;                 // Enlargement Classic: the answer enlarged to the frame's extent
    // Passes (fast) (nr_fast_passes.hpp): the network's input and answer in 16-bit float at the model's extent, the
    // fast work's images (by nr::fast::Image; kP, kN, kMotion unused) and its feedback history
    Surf f_in, f_out;
    Surf f_net, f_ans;            // a float frame passed through: the network's input and answer (light), apart from the
                                  // fast work's bounded-domain pair f_in / f_out
    Surf fast[nr::fast::kImageCount];
    nr::fast::History fast_history;
    bool half = false;            // the model's images in 16-bit float (peripheral compression or Passes (fast))
    int fast_said = -1;
    float scale = 1.0f;
    Surf sp_colour, sp_depth, sp_motion, sp_proxy, sp_answer, sp_proxy_native, sp_answer_native;
    bool spatial = false;
    std::string spatial_signature, guide_signature;
    bool spatial_requested = false, spatial_failed = false;
    uint32_t model_w = 0, model_h = 0;
    Surf meter, exposure;
    uint32_t w = 0, h = 0;
    std::vector<PassControls> built;   // pass k's controls, as its model was made
    std::vector<bool> fresh, failed;
    bool latch = true;
    int said = -1;
    Surf p_light, p_light_m, p_in, p_out, p_scratch, p_clamp, p_m, p_state;
    bool prep_was_on = false;
    nr::Preprocess prep_last{};
    uint32_t prep_kind = 0, prep_rmode = 0;
    EncodingState encoding;
    std::chrono::steady_clock::time_point prep_metered{};
    uint64_t frame_format_key = 0;   // the route's frame format (a change remakes everything)
};

inline bool same_controls(const PassControls& a, const PassControls& b) {
    return a.preset == b.preset && a.style == b.style && a.intensity == b.intensity &&
           a.local_structure == b.local_structure && a.local_tone == b.local_tone &&
           a.skin_structure == b.skin_structure && a.auto_mask == b.auto_mask;
}

// Passes (fast) as the layer asks for it (one legacy pass is the network's own answer).
// The chosen method's own setting (Fast<Method>...): [0] Extrapolation, [1] Feedback, [2] Hybrid.
inline uint32_t fast_slot(const Settings& s) { return std::min(s.fast_method, 2u); }
inline bool fast_requested(const Settings& s) {   // Edit strength (method 3): at any strength but 1
    return s.passes_mode == 1 && (s.fast_method == 3 ? s.fast_edit_strength != 1.f : s.fast_passes[fast_slot(s)] > 1.f);
}
inline nr::fast::Settings fast_settings(const Settings& s) {
    nr::fast::Settings f;
    f.passes = s.fast_passes[fast_slot(s)];
    f.method = s.fast_method;
    f.halo_limit = s.fast_halo_limit[fast_slot(s)];
    f.fine = s.fast_fine[fast_slot(s)];
    f.edit_strength = s.fast_edit_strength;
    return f;
}

inline void flow_release(FlowIo& io, FlowState& o) {
    io.release_passes(0);
    o.built.clear(); o.fresh.clear(); o.failed.clear();
    io.retire(o.f_in); io.retire(o.f_out); io.retire(o.f_net); io.retire(o.f_ans);
    for (Surf& r : o.fast) io.retire(r);
    o.fast_history = {};
    for (Surf* s : {&o.small_raw, &o.small_keep, &o.classic,
                    &o.scratch, &o.clamp, &o.small, &o.native, &o.sp_colour, &o.sp_depth, &o.sp_motion, &o.sp_proxy,
                    &o.sp_answer, &o.sp_proxy_native, &o.sp_answer_native, &o.proxy, &o.keep, &o.answer, &o.meter,
                    &o.exposure, &o.p_light, &o.p_light_m, &o.p_in, &o.p_out, &o.p_scratch, &o.p_clamp, &o.p_m,
                    &o.p_state})
        io.retire(*s);
}

namespace flow_detail {
inline void once(FlowIo& io, int& said, int what, const char* text) {
    if (said == what) return;
    said = what;
    io.say(text);
}
// Passes (fast)'s dispatches through the stage, its images resolved to the flow's surfaces.
class FastRecorder final : public nr::fast::Dispatcher {
public:
    FastRecorder(VkCommandBuffer cmd, Stage& stage, const FlowIo::ImageOf& image_of, const FlowState& o, Surf motion)
        : cmd_(cmd), stage_(stage), image_of_(image_of), o_(o), motion_(motion) {}
    bool dispatch(nr::fast::Program p, const nr::fast::Binding* b, uint32_t count, const void* push, uint32_t bytes,
                  uint32_t gx, uint32_t gy) override {
        Stage::FastImage images[8];
        if (count > 8) return false;
        for (uint32_t i = 0; i < count; ++i) {
            const nr::fast::Image which = b[i].image;
            const Surf s = which == nr::fast::kP ? o_.f_in : which == nr::fast::kN ? o_.f_out
                         : which == nr::fast::kMotion ? motion_ : o_.fast[which];
            images[i] = {b[i].binding, image_of_(s)};
            if (!images[i].image.format) return false;
        }
        stage_.record_fast(cmd_, p, images, count, push, bytes, gx, gy);
        return true;
    }
private:
    VkCommandBuffer cmd_;
    Stage& stage_;
    const FlowIo::ImageOf& image_of_;
    const FlowState& o_;
    Surf motion_;
};
inline void release_fast(FlowIo& io, FlowState& o) {
    io.retire(o.f_in); io.retire(o.f_out); io.retire(o.f_net); io.retire(o.f_ans);
    for (Surf& r : o.fast) io.retire(r);
    o.fast_history = {};
}
inline void release_from(FlowIo& io, FlowState& o, size_t from) {
    io.release_passes(from);
    o.built.resize(std::min(from, o.built.size()));
    o.fresh.resize(o.built.size());
    o.failed.resize(o.built.size());
}
}  // namespace flow_detail

// The model's extent for a frame of w x h (4.8 steps 5 / 6), as flow_run will take it: the work extent, or the packed
// one with peripheral compression (which needs both guides and is off once it failed for this layout).
inline void flow_model_extent(const Settings& s, uint32_t w, uint32_t h, const FlowState& o, bool guides, uint32_t* mw,
                              uint32_t* mh, bool* spatial_out = nullptr) {
    const float scale = working_scale(s);
    uint32_t work_w = w, work_h = h;
    work_extent(s, scale, w, h, &work_w, &work_h);
    work_w = std::max(work_w, 1u);
    work_h = std::max(work_h, 1u);
    const SpatialLayout layout = spatial_layout(s, w, h, scale);
    const bool failed = layout.signature == o.spatial_signature && o.spatial_failed;
    const bool spatial = layout.active && !failed && guides;
    *mw = spatial ? layout.model_w : work_w;
    *mh = spatial ? layout.model_h : work_h;
    if (spatial_out) *spatial_out = spatial;
}
// Whether the model's images (the network's input and answer) are 16-bit float: with peripheral compression, and with
// Passes (fast) where its programs are there.
inline bool flow_model_half(const Settings& s, bool spatial, bool fast_programs) {
    return spatial || (fast_requested(s) && fast_programs);
}
// The format of the network's input and answer as flow_run gives them (a route whose network is made for one format
// asks before the run): Passes (fast) 16-bit float, else [Preprocess] (`prep`: active and its programs there) 32-bit
// float, else peripheral compression 16-bit float, else the frame's.
inline Fmt flow_network_format(const Settings& s, bool spatial, bool fast_programs, bool prep) {
    if (fast_requested(s) && fast_programs) return Fmt::kHalf;
    if (prep) return Fmt::kF32;
    return spatial ? Fmt::kHalf : Fmt::kFrame;
}

// The common pipeline on f.target (w x h, in place). `format_key`: the route's frame format (any value; a change
// remakes the surfaces and the passes). False when the frame was left as it came.
inline bool flow_run(FlowIo& io, FlowState& o, const FlowFrame& f, const Settings& s, uint64_t format_key) {
    using flow_detail::once;
    struct Finish { FlowIo& io; ~Finish() { io.finish(); } } finish{io};
    const uint32_t w = f.width, h = f.height;
    if (!w || !h || !f.target) return false;
    // 4.8 step 2: the guide regions (4.5) must not be empty. (A route may have no guides at all - the ReShade route
    // without a motion-vector source: the network runs without them, peripheral compression is off.)
    if ((f.depth && (!f.depth_rect.width || !f.depth_rect.height)) || (f.motion && (!f.motion_rect.width || !f.motion_rect.height))) {
        once(io, o.said, 1, "a guide region is empty; NR skips the frame");
        return false;
    }
    // Step 3: the game's Reset.
    if (f.reset) o.latch = true;
    // Steps 5 / 6: the work extent from WorkingScale (4.8 step 5), and the peripheral compression's layout (8.10).
    const float scale = working_scale(s);
    const bool super = scale > 1.0f;
    uint32_t work_w = w, work_h = h;
    work_extent(s, scale, w, h, &work_w, &work_h);
    work_w = std::max(work_w, 1u);
    work_h = std::max(work_h, 1u);
    const bool reduced = work_w != w || work_h != h;
    const SpatialLayout layout = spatial_layout(s, w, h, scale);
    if (layout.signature != o.spatial_signature) {   // 4.8 step 6: a changed layout either side requested resets
        if (s.spatial || o.spatial_requested) o.latch = true;
        o.spatial_signature = layout.signature;
        o.spatial_requested = s.spatial;
        o.spatial_failed = false;
    }
    const bool spatial = layout.active && !o.spatial_failed && f.depth && f.motion;
    if (spatial && o.guide_signature != f.guide_signature) {   // the fork's spatial signature counts the guides' too
        if (!o.guide_signature.empty()) o.latch = true;
        o.guide_signature = f.guide_signature;
    }
    const uint32_t model_w = spatial ? layout.model_w : work_w, model_h = spatial ? layout.model_h : work_h;
    // Surfaces (5.3): made again, with the history restarted, when the extent, the scale or the format changes. A
    // format, scale or peripheral-compression change also retires the features; a size change alone keeps them, the
    // model extent following the frame's (OptiScaler fix 8: each evaluate carries its own size). The proxy and the kept
    // original at the frame's extent, the rest at the work extent; the model's images in the frame's format, fp16 with
    // peripheral compression (4.8 table).
    // Passes (fast): the network's input and answer in 16-bit float as with peripheral compression (its work edits them
    // in place in that format).
    const bool half = flow_model_half(s, spatial, io.has_fast());
    const Fmt model_format = half ? Fmt::kHalf : Fmt::kFrame;
    if (o.w != w || o.h != h || o.frame_format_key != format_key || o.scale != scale || o.spatial != spatial ||
        o.half != half || o.model_w != model_w || o.model_h != model_h || !o.proxy || !o.keep || !o.answer) {
        if (o.frame_format_key != format_key || o.scale != scale || o.spatial != spatial || o.half != half)
            flow_detail::release_from(io, o, 0);
        flow_detail::release_fast(io, o);
        for (Surf* r : {&o.proxy, &o.keep, &o.answer, &o.scratch, &o.clamp, &o.small, &o.native, &o.sp_colour, &o.sp_depth,
                        &o.sp_motion, &o.sp_proxy, &o.sp_answer, &o.sp_proxy_native, &o.sp_answer_native, &o.p_light,
                        &o.p_light_m, &o.p_in, &o.p_out, &o.p_scratch, &o.p_clamp, &o.p_m, &o.small_raw, &o.small_keep,
                        &o.classic})
            io.retire(*r);
        o.proxy = io.make(Fmt::kFrame, w, h);
        o.keep = io.make(Fmt::kFrame, w, h);
        o.answer = io.make(model_format, model_w, model_h);
        bool made = o.proxy && o.keep && o.answer;
        if (spatial) {
            o.sp_colour = io.make(Fmt::kHalf, model_w, model_h);
            o.sp_depth = io.make(Fmt::kR32F, model_w, model_h);
            o.sp_motion = io.make(Fmt::kRG32F, model_w, model_h);
            o.sp_proxy = io.make(Fmt::kHalf, layout.ord_w, layout.ord_h);
            o.sp_answer = io.make(Fmt::kHalf, layout.ord_w, layout.ord_h);
            made = made && o.sp_colour && o.sp_depth && o.sp_motion && o.sp_proxy && o.sp_answer;
            if (super) {
                o.sp_proxy_native = io.make(Fmt::kHalf, w, h);
                o.sp_answer_native = io.make(Fmt::kHalf, w, h);
                made = made && o.sp_proxy_native && o.sp_answer_native;
            }
        } else {
            if (reduced) made = made && (o.small = io.make(Fmt::kFrame, work_w, work_h));
            if (super) made = made && (o.native = io.make(Fmt::kFrame, w, h));   // outputNative
        }
        o.w = w; o.h = h; o.frame_format_key = format_key; o.scale = scale; o.spatial = spatial; o.half = half;
        o.model_w = model_w; o.model_h = model_h;
        o.latch = true;
        if (!made) {
            for (Surf* r : {&o.proxy, &o.keep, &o.answer, &o.small, &o.native, &o.sp_colour, &o.sp_depth, &o.sp_motion,
                            &o.sp_proxy, &o.sp_answer, &o.sp_proxy_native, &o.sp_answer_native})
                io.retire(*r);
            o.w = o.h = 0;
            once(io, o.said, 2, "its textures could not be made; NR skips the frame");
            return false;
        }
    }
    // Steps 4 / 7 (5.1, 5.3, 9.4): a model for every requested pass. Any pass's settings changed: every one is made
    // again (with a reset); fewer passes: the ones past them go. At most one is made a frame, in pass order, and a
    // frame that made one is left as it came; a pass whose model could not be made ends the chain there (pass 0: NR
    // does not run).
    const uint32_t requested = passes_requested(s);
    for (size_t k = 0; k < o.built.size(); ++k)
        if (!o.failed[k] && !same_controls(pass_controls(s, uint32_t(k)), o.built[k])) {
            flow_detail::release_from(io, o, 0);
            o.latch = true;
            break;
        }
    if (o.built.size() > requested) flow_detail::release_from(io, o, requested);
    if (requested == 1) { io.retire(o.scratch); io.retire(o.clamp); }
    uint32_t ready = 0;
    for (uint32_t k = 0; k < requested; ++k) {
        if (k < o.failed.size() && o.failed[k]) break;
        const PassControls pc = k < o.built.size() ? o.built[k] : pass_controls(s, k);
        const PassReady r = io.pass(k, pc, model_w, model_h);
        if (r == PassReady::kFailed) {
            if (o.built.size() <= k) { o.built.resize(k + 1); o.fresh.resize(k + 1); o.failed.resize(k + 1); }
            o.failed[k] = true;
            if (k == 0) {
                // the packed model of spatial compression could not be made: the whole picture from the next frame
                if (spatial) {
                    o.spatial_failed = true;
                    o.failed[k] = false;
                    once(io, o.said, 12, "the spatial compression's model could not be made; NR runs on the whole picture");
                }
                return false;
            }
            break;
        }
        if (r == PassReady::kReady && o.built.size() <= k) {   // a route whose passes need no making (the first use)
            o.built.resize(k + 1); o.fresh.resize(k + 1); o.failed.resize(k + 1);
            o.built[k] = pc;
            o.fresh[k] = true;
        }
        if (r == PassReady::kMade) {
            if (o.built.size() <= k) { o.built.resize(k + 1); o.fresh.resize(k + 1); o.failed.resize(k + 1); }
            o.built[k] = pc;
            o.fresh[k] = true;
            o.failed[k] = false;
            return false;
        }
        ++ready;
    }
    if (!ready) return false;
    if (ready > 1 && (!o.scratch || !o.clamp)) {   // the textures between passes, made at first use
        if (!o.scratch) o.scratch = io.make(model_format, model_w, model_h);
        if (!o.clamp) o.clamp = io.make(model_format, model_w, model_h);
        if (!o.scratch || !o.clamp) {
            once(io, o.said, 7, "the textures between passes could not be made; one pass runs");
            ready = 1;
        }
    }
    // 8.1: the exposure source, on a linear HDR frame only. WhitePointSource 1 measures from a valid game exposure
    // texture; 3 meters the frame itself (8.3); anything else is 0 (the manual white point, WhitePointScale).
    const bool passthrough = !f.hdr;
    // An SDR frame (passthrough) in a UNORM format at the frame's extent: the encode would store the frame's bytes into
    // the proxy and the kept original. The network and the resolve read the frame itself instead (the resolve reads no
    // neighbour of them at this extent), the encode does not run. NR_W2_DIRECT=0: as before.
    static const bool w2_direct_off = [] { const char* e = std::getenv("NR_W2_DIRECT"); return e && e[0] == '0'; }();
    const bool direct = !w2_direct_off && passthrough && f.unorm && f.direct_ok && !spatial && !reduced && !super;
    { static bool said = false; if (direct && !said) { said = true; io.say("the SDR frame read where it is (no encode)"); } }
    bool measure = false;
    const bool automatic = s.white_point_source == 3;
    if (!passthrough && s.white_point_source == 1 && f.exposure) measure = f.exposure_valid;
    if (!passthrough && automatic) {
        // 4.8 table: the meter (64 x 64) and the exposure (1 x 1), 32-bit float RGBA, kept for the owner's life
        if (!o.meter) o.meter = io.make(Fmt::kF32, 64, 64, false);
        if (!o.exposure) o.exposure = io.make(Fmt::kF32, 1, 1, false);
        measure = o.meter && o.exposure;
        if (!measure) once(io, o.said, 3, "the automatic exposure's textures could not be made; the manual white point is used");
    }
    // The 1 x 1 exposure the encode and the resolve read (t3): ours for source 3, the game's for source 1.
    const Surf exposure_texture = automatic ? o.exposure : f.exposure;

    if (io.stage_failed()) return false;

    // Step 8: the encode (8.5) - the frame into the proxy and the kept original.
    Constants exposure{};
    io.use(f.target, Access::kWrite);
    io.use(o.proxy, Access::kWrite);
    io.use(o.keep, Access::kWrite);
    if (automatic && o.meter) io.use(o.meter, Access::kWrite);
    const bool encoded = io.record([&](VkCommandBuffer vk, Stage& stage, const FlowIo::ImageOf& image_of) {
        Dispatch d;
        d.t0 = image_of(f.target);
        d.u0 = image_of(o.proxy);
        d.u1 = image_of(o.keep);
        if (!d.t0.format || !d.u0.format || !d.u1.format) return false;
        bool measured = measure;
        if (measured) {
            d.t3 = image_of(exposure_texture);
            if (!d.t3.format) { measured = false; d.t3 = {}; }   // 8.1: a measure that cannot run is ExposureMode 0
        }
        exposure_constants(s, measured, f.pre_exposure, w, h, exposure);
        io.run_begin(vk, d.t0, f.hdr);
        if (measured && automatic && !stage.has(kAutoExposure)) {   // its program missing: the manual white point
            measured = false;
            d.t3 = {};
            exposure_constants(s, false, f.pre_exposure, w, h, exposure);
        }
        if (measured && automatic) {
            // 8.3: the meter over the frame the encode reads, then the automatic exposure from its tiles
            Dispatch m;
            m.t0 = d.t0;
            m.u0 = image_of(o.meter);
            Dispatch a;
            a.u0 = d.t3;
            a.t0 = m.u0;
            if (!m.u0.format) {   // 8.1: the measuring dispatches cannot run: ExposureMode 0 for the frame
                exposure = Constants{};
                d.t3 = {};
            } else {
                m.constants = meter_constants(exposure);
                stage.record(vk, m);
                a.constants = auto_exposure_constants(exposure);
                stage.record(vk, a);
            }
        }
        d.constants = encode_constants(s, exposure, passthrough, w, h);
        if (!direct) stage.record(vk, d);
        return true;
    });
    if (!encoded) {
        once(io, o.said, 4, "the frame or its textures could not be read or written (format); NR skips the frame");
        return false;
    }
    if (o.encoding.update(s, f.hdr, exposure.exposure_mode)) o.latch = true;
    // [Preprocess] on this layer: run here from the frame's light, as the RenoDX-style layer runs it, and not in the
    // network's runtime from the encoded proxy (which cannot give the light back: its highlight curve divides by the
    // peak channel, it is stored in the frame's format, and a float frame the game does not flag HDR is passed through
    // as light while the runtime would take it for the encode's curve + sRGB). What the proxy is: HDR (the encode's
    // curve + sRGB), an SDR frame in display encoding, or light as it is.
    const nr::Preprocess& pp = f.preprocess;
    const uint32_t prep_kind = passthrough ? (f.unorm ? 1u : 2u) : 0u;
    const bool light_m_needed = spatial || reduced;
    bool prep = pp.active() && io.has(kPrepBack);
    if (prep) {
        if (!o.p_light) o.p_light = io.make(Fmt::kF32, w, h);
        if (light_m_needed && !o.p_light_m) o.p_light_m = io.make(Fmt::kF32, model_w, model_h);
        for (Surf* r : {&o.p_in, &o.p_out, &o.p_m})
            if (!*r) *r = io.make(Fmt::kF32, model_w, model_h);
        if (ready > 1) for (Surf* r : {&o.p_scratch, &o.p_clamp})
            if (!*r) *r = io.make(Fmt::kF32, model_w, model_h);
        if (!o.p_state) o.p_state = io.make(Fmt::kF32, 1, 1);
        prep = o.p_light && (!light_m_needed || o.p_light_m) && o.p_in && o.p_out && o.p_m && o.p_state &&
               (ready < 2 || (o.p_scratch && o.p_clamp));
    }
    // [Preprocess]'s programs or textures unavailable: NR runs without it (not the runtime's proxy-domain Preprocess,
    // which would bring back the encoding errors this layer's Preprocess avoids).
    const bool prep_missing = pp.active() && !prep;
    if (prep_missing) once(io, o.said, 14, "[Preprocess]'s programs or textures are unavailable; NR runs without it");
    const bool prep_reset = prep && (o.latch || !o.prep_was_on || pp != o.prep_last || prep_kind != o.prep_kind ||
                                     s.reversible_mode != o.prep_rmode);
    if (prep != o.prep_was_on || prep_reset) o.latch = true;   // the network is shown another picture: history restarts
    const auto prep_now = std::chrono::steady_clock::now();
    const float prep_dt = prep && o.prep_was_on && pp.exposure == 1
                              ? std::chrono::duration<float>(prep_now - o.prep_metered).count() : 0.0f;
    o.prep_was_on = prep;
    if (prep) {
        o.prep_last = pp; o.prep_kind = prep_kind; o.prep_rmode = s.reversible_mode;
        if (pp.exposure == 1) o.prep_metered = prep_now;
    }

    // Steps 10 - 13 and section 9: the model on the proxy, into the answer; the guides as the game gave them.
    io.use(direct ? f.target : o.proxy, Access::kRead);
    if (f.depth) io.use(f.depth, Access::kRead);
    if (f.motion) io.use(f.motion, Access::kRead);
    const float depth_rect[4] = {float(f.depth_rect.x), float(f.depth_rect.y), float(f.depth_rect.width),
                                 float(f.depth_rect.height)};
    const float motion_rect[4] = {float(f.motion_rect.x), float(f.motion_rect.y), float(f.motion_rect.width),
                                  float(f.motion_rect.height)};
    // spatial packing works in native pixels of the active guide region
    const float native_mv_x = native_motion_scale(f.mv_x, w, f.motion_rect.width);
    const float native_mv_y = native_motion_scale(f.mv_y, h, f.motion_rect.height);
    // Step 11: with peripheral compression the network is shown the packed proxy and packed guides (8.12, modes 100
    // and 101); a pass that cannot run falls back to ordinary NR for this layout, the frame left as it came.
    const auto spatial_fallback = [&](const char* why) {
        o.spatial_failed = true;
        o.latch = true;
        once(io, o.said, 12, why);
    };
    if (spatial) {
        io.use(o.sp_colour, Access::kWrite);
        io.use(o.sp_depth, Access::kWrite);
        io.use(o.sp_motion, Access::kWrite);
        const bool packed = io.record([&](VkCommandBuffer vk, Stage& stage, const FlowIo::ImageOf& image_of) {
            Dispatch c;
            c.t0 = image_of(o.proxy);
            c.u0 = image_of(o.sp_colour);
            Dispatch g;
            g.t0 = c.t0;
            g.t1 = image_of(f.depth);
            g.t2 = image_of(f.motion);
            g.u0 = image_of(o.sp_depth);
            g.u1 = image_of(o.sp_motion);
            if (!c.t0.format || !c.u0.format || !g.t1.format || !g.t2.format || !g.u0.format || !g.u1.format) return false;
            c.constants = spatial_constants(layout, kSpatialPack, model_w, model_h, depth_rect, motion_rect, native_mv_x, native_mv_y);
            g.constants = spatial_constants(layout, kSpatialGuides, model_w, model_h, depth_rect, motion_rect, native_mv_x, native_mv_y);
            if (!stage.has(Mode(c.constants.mode)) || !stage.has(Mode(g.constants.mode))) return false;   // its programs missing
            stage.record(vk, c);
            stage.record(vk, g);
            return true;
        });
        if (!packed) {
            spatial_fallback("the peripheral compression's packing could not run; it is off for this layout");
            return false;
        }
        for (Surf r : {o.sp_colour, o.sp_depth, o.sp_motion}) io.use(r, Access::kRead);
    }
    // Downscale "bilinear before encoding" (ours: the ReShade route's way, a choice on both): below the frame's extent
    // the frame is resampled bilinearly to the work extent and encoded there, instead of the full proxy area-resampled.
    const bool bilinear_first = !spatial && reduced && !super && s.downscale == 1;
    if (bilinear_first) {
        if (!o.small_raw) o.small_raw = io.make(Fmt::kFrame, work_w, work_h);
        if (!o.small_keep) o.small_keep = io.make(Fmt::kFrame, work_w, work_h);
        if (!o.small_raw || !o.small_keep) {
            once(io, o.said, 17, "the bilinear downscale's textures could not be made; NR skips the frame");
            return false;
        }
        io.use(o.small_raw, Access::kWrite);
        io.use(o.small_keep, Access::kWrite);
        io.use(o.small, Access::kWrite);
        const bool resampled = io.record([&](VkCommandBuffer vk, Stage& stage, const FlowIo::ImageOf& image_of) {
            Dispatch r;
            r.t0 = image_of(f.target);
            r.u0 = image_of(o.small_raw);
            if (!r.t0.format || !r.u0.format) return false;
            r.constants.mode = kDownsample;
            r.constants.width = work_w;
            r.constants.height = work_h;
            r.constants.compare_mode = 1;   // bilinear
            stage.record(vk, r);
            Dispatch e;
            e.t0 = r.u0;
            e.u0 = image_of(o.small);
            e.u1 = image_of(o.small_keep);
            if (!e.u0.format || !e.u1.format) return false;
            if (exposure.exposure_mode) {
                e.t3 = image_of(exposure_texture);
                if (!e.t3.format) return false;
            }
            e.constants = encode_constants(s, exposure, passthrough, work_w, work_h);
            stage.record(vk, e);
            return true;
        });
        if (!resampled) {
            once(io, o.said, 10, "the proxy could not be resampled to the work extent; NR skips the frame");
            return false;
        }
        io.use(o.small, Access::kRead);
    }
    // Step 9: otherwise the model is shown the proxy at the work extent: below the frame's, area-resampled (mode 2,
    // 8.6); above it, enlarged by the up filter of ScalingDownscaler (8.7).
    if (!spatial && reduced && !bilinear_first) {
        io.use(o.small, Access::kWrite);
        const bool resampled = io.record([&](VkCommandBuffer vk, Stage& stage, const FlowIo::ImageOf& image_of) {
            Dispatch d;
            d.t0 = image_of(o.proxy);
            d.u0 = image_of(o.small);
            if (!d.t0.format || !d.u0.format) return false;
            if (super) {
                d.constants = scaling_constants(s.scaling_downscaler, true, w, h, work_w, work_h);
            } else {
                d.constants.mode = kDownsample;
                d.constants.width = work_w;
                d.constants.height = work_h;
            }
            if (!stage.has(Mode(d.constants.mode))) return false;   // supersampling's programs missing
            stage.record(vk, d);
            return true;
        });
        if (!resampled) {
            once(io, o.said, 10, "the proxy could not be resampled to the work extent; NR skips the frame");
            return false;
        }
        io.use(o.small, Access::kRead);
    }
    const Surf model_input = direct ? f.target : spatial ? o.sp_colour : reduced ? o.small : o.proxy;
    // [Preprocess]: the frame's light (as the encode reads the frame), brought to the model's extent the way the proxy
    // was, the meter over it (Exposure auto), and the network's input from it.
    const Surf light_m = light_m_needed ? o.p_light_m : o.p_light;
    if (prep) {
        for (Surf r : {o.p_light, o.p_light_m, o.p_state, o.p_in}) if (r) io.use(r, Access::kWrite);
        const bool prepped = io.record([&](VkCommandBuffer vk, Stage& stage, const FlowIo::ImageOf& image_of) {
            Dispatch l;
            l.t0 = image_of(f.target);
            if (exposure.exposure_mode) l.t3 = image_of(exposure_texture);
            l.u0 = image_of(o.p_light);
            if (!l.t0.format || !l.u0.format || (exposure.exposure_mode && !l.t3.format)) return false;
            l.constants = encode_constants(s, exposure, passthrough, w, h);
            l.constants.mode = kLight;
            l.constants.padding[0] = prep_kind == 1u ? 1u : 0u;
            stage.record(vk, l);
            if (light_m_needed) {
                Dispatch r;
                r.t0 = l.u0;
                r.u0 = image_of(o.p_light_m);
                if (!r.u0.format) return false;
                if (spatial)
                    r.constants = spatial_constants(layout, kSpatialPack, model_w, model_h, depth_rect, motion_rect,
                                                    native_mv_x, native_mv_y);
                else if (super)
                    r.constants = scaling_constants(s.scaling_downscaler, true, w, h, work_w, work_h);
                else {
                    r.constants.mode = kDownsample;
                    r.constants.width = work_w;
                    r.constants.height = work_h;
                    if (bilinear_first) r.constants.compare_mode = 1;   // the light resampled as the proxy was
                }
                if (!stage.has(Mode(r.constants.mode))) return false;
                stage.record(vk, r);
            }
            const Image lm = image_of(light_m), st = image_of(o.p_state);
            if (!lm.format || !st.format) return false;
            if (pp.exposure == 1) {
                Dispatch m;
                m.t0 = lm; m.u0 = lm; m.u1 = st;
                m.constants = prep_constants(kPrepMeter, pp, model_w, model_h, prep_kind, s.reversible_mode, prep_reset, prep_dt);
                stage.record(vk, m);
            }
            Dispatch fw;
            fw.t0 = lm; fw.u1 = st;
            fw.u0 = image_of(o.p_in);
            if (!fw.u0.format) return false;
            fw.constants = prep_constants(kPrepForward, pp, model_w, model_h, prep_kind, s.reversible_mode, prep_reset, prep_dt);
            stage.record(vk, fw);
            return true;
        });
        if (!prepped) {
            once(io, o.said, 15, "[Preprocess] could not be recorded; NR skips the frame");
            return false;
        }
        io.use(o.p_in, Access::kRead);
    }
    const Surf net_input = prep ? o.p_in : model_input;
    const Surf clamp_image = prep ? o.p_clamp : o.clamp;
    // Passes (fast) (nr_fast_passes.hpp; one network pass, passes_requested): the pass's input copied to 16-bit float
    // (rounded as the network rounds it: the network is shown the same values), with Feedback / Hybrid the last frame's
    // edit moved by the motion vectors into that copy, and after the network its answer (16-bit float, model_format)
    // strengthened in place. Feedback / Hybrid need the game's motion vectors and run as Extrapolation without them and
    // with peripheral compression (whose packed vectors are of another picture).
    const nr::fast::Settings fs = fast_settings(s);
    const bool fast_programs = io.has_fast() && io.has(kHalfCopy);
    const bool fast_on = fast_requested(s) && fast_programs;
    if (!fast_on && (o.f_in || o.f_out)) flow_detail::release_fast(io, o);
    if (fast_requested(s) && !fast_programs) {
        once(io, o.fast_said, 1, "Passes (fast)'s programs are unavailable; one network pass runs");
        // the first frame only (the stage was not made when the model's images were chosen 16-bit float): the network's
        // input and answer differ in format; from the next frame they are the frame's
        if (half && !spatial) return false;
    }
    const bool history_method = fs.method == 1 || fs.method == 2;   // Feedback / Hybrid (Edit strength: none)
    const bool feedback = fast_on && history_method && f.motion && f.fast_motion && !spatial;
    // The fast work's arithmetic takes a display-encoded picture in [0, 1]. A float frame passed through as it is (no
    // HDR flag; without [Preprocess], whose input is its own) can go above 1: the fast work then runs in a bounded domain
    // (wilsjo2_half_copy.comp DebugView 1 / 2, the hybrid curve: identity to 0.75) and the network on the light - its
    // input f_net (as it came, or the fed input brought back), its answer f_ans (brought into the domain for the fast
    // work and back after it).
    const bool fast_map = fast_on && passthrough && !f.unorm && !prep;
    if (fast_on && !fast_map) { io.retire(o.f_net); io.retire(o.f_ans); }
    if (fast_on) {
        const uint32_t lw = nr::fast::low(model_w), lh = nr::fast::low(model_h);
        if (!o.f_in) o.f_in = io.make(Fmt::kHalf, model_w, model_h);
        if (!o.f_out) o.f_out = io.make(Fmt::kHalf, model_w, model_h);
        if (fast_map && !o.f_net) o.f_net = io.make(Fmt::kHalf, model_w, model_h);
        if (fast_map && !o.f_ans) o.f_ans = io.make(Fmt::kHalf, model_w, model_h);
        bool made = o.f_in && o.f_out && (!fast_map || (o.f_net && o.f_ans));
        for (uint32_t i = nr::fast::kFbAdd; i < nr::fast::kImageCount; ++i) {
            const auto image = nr::fast::Image(i);
            if (nr::fast::feedback_image(image) && !history_method) {   // Extrapolation / Edit strength: no feedback's add or history
                if (o.fast[i]) io.retire(o.fast[i]);
                continue;
            }
            // at the model's extent kFbAdd RGBA16F, kFineH RG16F; else RGBA32F at quarter size
            const bool full = i <= nr::fast::kFineH;
            const Fmt fmt = image == nr::fast::kFineH ? Fmt::kRG16F : full ? Fmt::kHalf : Fmt::kF32;
            if (!o.fast[i]) o.fast[i] = io.make(fmt, full ? model_w : lw, full ? model_h : lh);
            made = made && o.fast[i];
        }
        if (!made) {
            once(io, o.fast_said, 2, "Passes (fast)'s textures could not be made; NR skips the frame");
            return false;
        }
        if (history_method && !feedback)
            once(io, o.fast_said, spatial ? 3 : 4, spatial ? "Passes (fast): Feedback / Hybrid run as Extrapolation with peripheral compression"
                                                           : "Passes (fast): no motion vectors; Feedback / Hybrid run as Extrapolation");
        else if (o.fast_said > 0)
            o.fast_said = 0;
        // the history restarts where the network's does, and is not kept without feedback
        if (o.latch || o.fresh.empty() || o.fresh[0] || !feedback) o.fast_history.valid = false;
        nr::fast::Motion motion;
        if (feedback) {
            motion.base[0] = f.motion_rect.x; motion.base[1] = f.motion_rect.y;
            motion.grid[0] = f.motion_rect.width; motion.grid[1] = f.motion_rect.height;
            // the NGX scale gives pixels of the vectors' region; the fast work moves the network's pixels
            motion.scale[0] = f.mv_x * float(model_w) / float(std::max(f.motion_rect.width, 1u));
            motion.scale[1] = f.mv_y * float(model_h) / float(std::max(f.motion_rect.height, 1u));
        }
        io.use(net_input, Access::kRead);
        io.use(o.f_in, Access::kWrite);
        io.use(o.f_out, Access::kWrite);
        if (fast_map) io.use(o.f_net, Access::kWrite);
        const bool fed = io.record([&](VkCommandBuffer vk, Stage& stage, const FlowIo::ImageOf& image_of) {
            const auto copy = [&](Surf from, Surf to, uint32_t way) {
                Dispatch c;
                c.t0 = image_of(from);
                c.u0 = image_of(to);
                if (!c.t0.format || !c.u0.format) return false;
                c.constants.mode = kHalfCopy;
                c.constants.width = model_w;
                c.constants.height = model_h;
                c.constants.debug_view = way;
                stage.record(vk, c);
                return true;
            };
            if (!copy(net_input, o.f_in, fast_map ? 1u : 0u)) return false;
            flow_detail::FastRecorder d(vk, stage, image_of, o, f.motion);
            if (!nr::fast::feed(d, fs, model_w, model_h, motion, o.fast_history, feedback)) return false;
            // the network's own input: the light as it came, or with the feedback brought back from the domain
            return !fast_map || copy(feedback ? o.f_in : net_input, o.f_net, feedback ? 2u : 0u);
        });
        if (!fed) {
            once(io, o.fast_said, 5, "Passes (fast) could not be recorded; NR skips the frame");
            return false;
        }
    }
    // the network's guides (9.1): the packed ones at the model extent (motion already in its pixels), else the game's
    const Surf model_depth = spatial ? o.sp_depth : f.depth;
    const Surf model_motion = spatial ? o.sp_motion : f.motion;
    const Rect packed_rect{0, 0, model_w, model_h};
    NetCall call;
    call.depth = model_depth;
    call.motion = model_motion;
    call.width = model_w; call.height = model_h;
    call.depth_rect = spatial ? packed_rect : f.depth_rect;
    call.motion_texture_rect = f.motion_rect;
    call.motion_rect = spatial ? packed_rect : f.motion_rect;
    call.depth_inverted = (f.flags & (1u << 3)) != 0;
    call.mv_x = spatial ? 1.0f : f.mv_x * (float(work_w) / float(w));
    call.mv_y = spatial ? 1.0f : f.mv_y * (float(work_h) / float(h));
    call.noise_every_frame = s.noise_every_frame;
    call.proxy_curve = s.reversible_mode;
    Surf final_answer;   // the last pass's answer (9.3)
    bool all_ran = true;
    {
        FlowFacts facts;
        facts.hdr = !passthrough;
        facts.exposure = !exposure.exposure_mode ? 0 : automatic ? 3 : 1;
        facts.asked = s.white_point_source;
        facts.white = s.white_point_scale;
        facts.history = s.history_strength;
        facts.mode = s.reversible_mode;
        facts.passes = ready;
        facts.prep_outside = prep || prep_missing;
        facts.prep_missing = prep_missing;
        facts.fast_passes = fast_on ? (fs.method == 3u ? fs.edit_strength : fs.passes) : 0.f;
        facts.fast_fallback = fast_on && history_method && !feedback;
        io.passes_begin(facts);
        for (uint32_t k = 0; k < ready; ++k) {
            // pass 0 reads the proxy; a later pass the previous answer clamped (mode 8, 8.8) into passClamp.
            // Answers alternate output / passScratch.
            const Surf input = fast_on ? (fast_map ? o.f_net : o.f_in) : k == 0 ? net_input : clamp_image;
            const Surf output = fast_on ? (fast_map ? o.f_ans : o.f_out)
                                        : k % 2 == 0 ? (prep ? o.p_out : o.answer) : (prep ? o.p_scratch : o.scratch);
            if (fast_on) io.use(input, Access::kRead);
            if (k > 0) {
                io.use(final_answer, Access::kRead);
                io.use(clamp_image, Access::kWrite);
                const Surf previous = final_answer;
                const bool clamped = io.record([&](VkCommandBuffer vk, Stage& stage, const FlowIo::ImageOf& image_of) {
                    Dispatch d;
                    d.t0 = image_of(previous);
                    d.u0 = image_of(clamp_image);
                    if (!d.t0.format || !d.u0.format) return false;
                    d.constants.mode = kClamp;
                    d.constants.width = model_w;
                    d.constants.height = model_h;
                    stage.record(vk, d);
                    return true;
                });
                io.use(final_answer, Access::kWrite);
                if (!clamped) {   // 9.3: the chain stops at the last answer, the reset latch stays set
                    all_ran = false;
                    break;
                }
                io.use(clamp_image, Access::kRead);
            }
            io.use(output, Access::kWrite);
            call.pass = k;
            call.input = input;
            call.output = output;
            call.reset = o.latch || o.fresh[k];
            call.controls = o.built[k];
            call.last = k + 1 == ready;
            const int result = io.network(call);
            if (k > 0) io.use(clamp_image, Access::kWrite);
            if (result != 0) {
                // a model pass failed: no answer this frame (the fork: no resolve); the packed model of spatial
                // compression failing: the whole picture from the next frame
                all_ran = false;
                final_answer = Surf{};
                o.latch = true;   // its history starts again with the next run
                if (spatial) {
                    o.spatial_failed = true;
                    once(io, o.said, 13, "the spatial compression's model did not run; NR runs on the whole picture");
                }
                break;
            }
            o.fresh[k] = false;
            final_answer = output;
        }
        io.passes_end();
    }
    if (!final_answer) {
        once(io, o.said, 5, "the model's evaluate failed; the frame is left as it came");
        return false;   // no answer: the latch stays set (step 13), nothing is composed
    }
    if (fast_on) {   // the answer strengthened in place; with feedback the input given back and the edit kept
        io.use(o.f_in, Access::kWrite);
        io.use(o.f_out, Access::kWrite);
        if (fast_map) io.use(o.f_ans, Access::kWrite);
        const bool applied = io.record([&](VkCommandBuffer vk, Stage& stage, const FlowIo::ImageOf& image_of) {
            const auto copy = [&](Surf from, Surf to, uint32_t way) {
                Dispatch c;
                c.t0 = image_of(from);
                c.u0 = image_of(to);
                if (!c.t0.format || !c.u0.format) return false;
                c.constants.mode = kHalfCopy;
                c.constants.width = model_w;
                c.constants.height = model_h;
                c.constants.debug_view = way;
                stage.record(vk, c);
                return true;
            };
            if (fast_map && !copy(o.f_ans, o.f_out, 1u)) return false;   // the answer into the domain
            flow_detail::FastRecorder d(vk, stage, image_of, o, f.motion);
            if (!nr::fast::apply(d, fs, model_w, model_h, o.fast_history, feedback)) return false;
            return !fast_map || copy(o.f_out, o.f_ans, 2u);              // and the strengthened answer back
        });
        if (!applied) {
            o.fast_history.valid = false;
            once(io, o.fast_said, 6, "Passes (fast)'s work could not be recorded; the frame is left as it came");
            return false;
        }
    }
    if (all_ran) o.latch = false;   // 5.2: cleared once every pass of the frame ran
    // [Preprocess]: the network's change taken back into the proxy's domain (M = P + E(p + d) - E(p), P the proxy at
    // the model's extent); everything after works on M as on an answer. With NR Control's debug view of this layer the
    // view shows the network's own input and answer instead (the resolve is given that pair).
    const bool debug_raw = prep && s.debug_view >= 1u && s.debug_view <= 3u;
    if (prep && !debug_raw) {
        io.use(o.p_m, Access::kWrite);
        io.use(o.p_state, Access::kWrite);
        const Surf answer = final_answer;
        const bool backed = io.record([&](VkCommandBuffer vk, Stage& stage, const FlowIo::ImageOf& image_of) {
            Dispatch b;
            b.t0 = image_of(o.p_in);
            b.t1 = image_of(answer);
            b.t2 = image_of(light_m);
            b.t3 = image_of(model_input);
            b.u0 = image_of(o.p_m);
            b.u1 = image_of(o.p_state);
            if (!b.t0.format || !b.t1.format || !b.t2.format || !b.t3.format || !b.u0.format || !b.u1.format) return false;
            b.constants = prep_constants(kPrepBack, pp, model_w, model_h, prep_kind, s.reversible_mode, prep_reset, prep_dt);
            stage.record(vk, b);
            return true;
        });
        if (!backed) {
            once(io, o.said, 16, "[Preprocess]'s change could not be taken back; the frame is left as it came");
            return false;
        }
        final_answer = o.p_m;
    }

    // Enlargement Classic (ours, Transfer 4): below the frame's extent the answer enlarged to it by its scaler; the
    // resolve composes that picture (its Transfer 4 branch). Without the program: the bilinear pair, composed so.
    if (s.transfer == 4 && reduced && !spatial && !super && !debug_raw && io.has(kClassicUp)) {
        if (!o.classic) o.classic = io.make(Fmt::kFrame, w, h);
        if (o.classic) {
            io.use(final_answer, Access::kRead);
            io.use(o.classic, Access::kWrite);
            const Surf answer = final_answer;
            const bool enlarged = io.record([&](VkCommandBuffer vk, Stage& stage, const FlowIo::ImageOf& image_of) {
                Dispatch d;
                d.t0 = image_of(answer);
                d.u0 = image_of(o.classic);
                if (!d.t0.format || !d.u0.format) return false;
                d.constants.mode = kClassicUp;
                d.constants.width = w;
                d.constants.height = h;
                d.constants.debug_view = std::min(s.classic_scaler, 3u);
                stage.record(vk, d);
                return true;
            });
            io.use(final_answer, Access::kWrite);
            if (enlarged) final_answer = o.classic;
        }
    }

    // Step 15: above the frame's extent the answer goes back to it through the down filter of ScalingDownscaler
    // (outputNative), and the resolve pair is (the native proxy, that); if the filter cannot run, the pair stays at the
    // work extent.
    Surf resolve_proxy = debug_raw ? o.p_in : model_input;
    Surf resolve_answer = final_answer;
    if (spatial) {
        // Step 14: the packed proxy and the answer unpacked to the ordinary extent (mode 102); above WorkingScale 1 both
        // then go to the frame's extent through the down filter (step 15). Either failing: ordinary NR from now on for
        // this layout, this frame left as it came.
        io.use(final_answer, Access::kRead);
        io.use(o.sp_proxy, Access::kWrite);
        io.use(o.sp_answer, Access::kWrite);
        const Surf answer = final_answer;
        const bool unpacked = io.record([&](VkCommandBuffer vk, Stage& stage, const FlowIo::ImageOf& image_of) {
            Dispatch d;
            d.t0 = image_of(debug_raw ? o.p_in : o.sp_colour);
            d.t1 = image_of(answer);
            d.u0 = image_of(o.sp_proxy);
            d.u1 = image_of(o.sp_answer);
            if (!d.t0.format || !d.t1.format || !d.u0.format || !d.u1.format) return false;
            d.constants = spatial_constants(layout, kSpatialUnpack, layout.ord_w, layout.ord_h, depth_rect, motion_rect,
                                            native_mv_x, native_mv_y);
            if (!stage.has(Mode(d.constants.mode))) return false;
            stage.record(vk, d);
            return true;
        });
        io.use(final_answer, Access::kWrite);
        if (!unpacked) {
            spatial_fallback("the peripheral compression's unpacking could not run; it is off for this layout");
            return false;
        }
        resolve_proxy = o.sp_proxy;
        resolve_answer = o.sp_answer;
        if (super) {
            io.use(o.sp_proxy, Access::kRead);
            io.use(o.sp_answer, Access::kRead);
            io.use(o.sp_proxy_native, Access::kWrite);
            io.use(o.sp_answer_native, Access::kWrite);
            const bool down = io.record([&](VkCommandBuffer vk, Stage& stage, const FlowIo::ImageOf& image_of) {
                Dispatch p, a;
                p.t0 = image_of(o.sp_proxy);
                p.u0 = image_of(o.sp_proxy_native);
                a.t0 = image_of(o.sp_answer);
                a.u0 = image_of(o.sp_answer_native);
                if (!p.t0.format || !p.u0.format || !a.t0.format || !a.u0.format) return false;
                p.constants = a.constants = scaling_constants(s.scaling_downscaler, false, layout.ord_w, layout.ord_h, w, h);
                if (!stage.has(Mode(p.constants.mode))) return false;
                stage.record(vk, p);
                stage.record(vk, a);
                return true;
            });
            io.use(o.sp_proxy, Access::kWrite);
            io.use(o.sp_answer, Access::kWrite);
            if (!down) {
                spatial_fallback("the peripheral compression's pair could not be brought to the frame's size; it is off "
                                 "for this layout");
                return false;
            }
            resolve_proxy = o.sp_proxy_native;
            resolve_answer = o.sp_answer_native;
        }
    } else if (super && !debug_raw) {
        io.use(final_answer, Access::kRead);
        io.use(o.native, Access::kWrite);
        const Surf answer = final_answer;
        const bool down = io.record([&](VkCommandBuffer vk, Stage& stage, const FlowIo::ImageOf& image_of) {
            Dispatch d;
            d.t0 = image_of(answer);
            d.u0 = image_of(o.native);
            if (!d.t0.format || !d.u0.format) return false;
            d.constants = scaling_constants(s.scaling_downscaler, false, work_w, work_h, w, h);
            if (!stage.has(Mode(d.constants.mode))) return false;
            stage.record(vk, d);
            return true;
        });
        io.use(final_answer, Access::kWrite);
        if (down) {
            resolve_proxy = o.proxy;
            resolve_answer = o.native;
        } else {
            once(io, o.said, 11, "the answer could not be brought back to the frame's size; it is composed at the work size");
        }
    }

    // Step 17: the resolve (8.13, constants 8.14) of the last answer into the frame, in place (the original is the kept
    // copy), against the pass-0 input (the proxy). Direct: the frame is the proxy and the original, read and written.
    io.use(resolve_proxy, Access::kRead);
    io.use(resolve_answer, Access::kRead);
    io.use(o.keep, Access::kRead);
    io.use(f.target, Access::kWrite);
    const bool resolved = io.record([&](VkCommandBuffer vk, Stage& stage, const FlowIo::ImageOf& image_of) {
        Dispatch d;
        d.t0 = image_of(direct && !debug_raw ? f.target : resolve_proxy);   // the pair (4.8 steps 14 / 15)
        d.t1 = image_of(resolve_answer);
        d.t2 = image_of(direct ? f.target : o.keep);
        if (exposure.exposure_mode) d.t3 = image_of(exposure_texture);
        d.u0 = image_of(f.target);
        if (!d.t0.format || !d.t1.format || !d.t2.format || !d.u0.format) return false;
        if (exposure.exposure_mode && !d.t3.format) return false;
        d.constants = resolve_constants(s, exposure, passthrough, w, h, scale);
        stage.record(vk, d);
        io.run_end(vk, d.u0);
        return true;
    });
    if (!resolved) {
        once(io, o.said, 6, "the resolve could not be recorded; the frame is left as it came");
        return false;
    }
    if (o.spatial) io.spatial_ran(layout);
    if (o.said != 0)
        once(io, o.said, 0, f.at_present ? "running at Present" : f.before_upscale ? "running before the upscaler" : "running after the upscaler");
    return true;
}

}  // namespace nr::wilsjo2
