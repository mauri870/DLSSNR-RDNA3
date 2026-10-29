#pragma once
// The wilsjo2 style: our re-implementation, from our written specification of it only, of the NR processing the
// OptiScaler-NR fork (wilsjo2, v0.8.91) does around the network - encode the frame into a display-like proxy for the
// network, compose the network's answer back onto the frame. This is the GPU half: the fork's main constant buffer
//, its passes as our own programs (linux/shaders/wilsjo2, glslang), and the host maths that fill the
// constants (8.4, 8.14). The per-frame flow (placement, surfaces, the model call between encode and
// resolve) is the host's, on the game's own command list (linux/src/pe/nr_asi_wilsjo2.cpp).
//
// Numeric rules: 32-bit float everywhere; values are rounded only where they are stored, by the format of
// the image they go into. The caller gives the images in the frame's own format, as the fork keeps them.
#include "nr_fast_passes.hpp"
#include "nr_runtime.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

namespace nr::wilsjo2 {

// The main constant buffer, 256 bytes, every field 4 bytes; pushed as the programs' push constants.
struct Constants {
    uint32_t mode = 0;
    float white_point = 0.f;
    uint32_t width = 0, height = 0;
    float transfer_strength = 0.f, colour_strength = 0.f;
    uint32_t debug_view = 0;
    float max_ratio = 0.f;
    uint32_t passthrough = 0;
    float mv_scale_x = 0.f, mv_scale_y = 0.f;
    uint32_t guide_width = 0, guide_height = 0;
    uint32_t compare_mode = 0;
    float compare_split = 0.f, compare_zoom = 0.f;
    uint32_t compare_swap = 0;
    uint32_t transfer = 0;
    float debug_scale = 0.f;
    uint32_t reversible_mode = 0;
    uint32_t apply_model = 0;
    uint32_t colour_limit = 0;   // ColourLimit (ours): the edit's colour bounded as RenoDX bounds it
    float residual_scale = 0.f;
    uint32_t skin_protection = 0, show_skin_mask = 0;
    float skin_detail = 0.f, skin_colour = 0.f, environment_detail = 0.f, environment_colour = 0.f;
    float residual_blend = 0.f;
    uint32_t residual_history_valid = 0, residual_motion_base_x = 0, residual_motion_base_y = 0;
    float replace_detail_strength = 0.f;
    float model_work_scale = 0.f;
    float residual_confidence_sensitivity = 0.f;
    uint32_t exposure_mode = 0;
    float pre_exposure = 0.f, exposure_trim = 0.f, exposure_protection = 0.f;
    uint32_t exposure_anchor_count = 0, exposure_source_width = 0, exposure_source_height = 0, exposure_padding = 0;
    float exposure_anchors[16] = {};
    uint32_t padding[4] = {};
};
static_assert(sizeof(Constants) == 256, "the fork's constant buffer is 256 bytes");

// Mode numbers: the ones recorded so far; the others follow with the stages that need them.
enum Mode : uint32_t { kEncode = 0, kResolve = 1, kDownsample = 2, kMeter = 3, kClamp = 8, kHalfCopy = 9, kAutoExposure = 11,
                      // ours: an exact copy of a region (wilsjo2_copy.comp), the .asi's Vulkan route where a transfer copy
                      // cannot serve: t0 read (12) or u1 read as a storage image (13) into u0
                      kCopy = 12, kCopyLoad = 13,
                      // ours: the finished picture's HDR10 conversion (the fork's separate finished-colour modes 0 / 1)
                      kFinishedToLinear = 20, kFinishedToPq = 21,
                      // ours: OptiScaler's output-scaling filters for WorkingScale above 1
                      kOsUp = 30, kOsDown = 31, kOsEasu = 32,
                      // ours: Enlargement Classic's scaler (wilsjo2_classic.comp)
                      kClassicUp = 34,
                      // ours: NR Control's comparison of two frames, any processing's (wilsjo2_inspect.comp)
                      kInspect = 40,
                      // ours: [Preprocess] on the layer itself (wilsjo2_light.comp, wilsjo2_prep.comp)
                      kLight = 60, kPrepMeter = 61, kPrepForward = 62, kPrepBack = 63,
                      // ours: HUD FIX at Present, any processing's (wilsjo2_hudfix.comp)
                      kHudClear = 50, kHudCount = 51, kHudEstimate = 52, kHudCompose = 53,
                      // peripheral compression: pack the proxy, pack the guides, unpack the pair
                      kSpatialPack = 100, kSpatialGuides = 101, kSpatialUnpack = 102 };

// The settings the per-frame flow uses, under the fork's [DlssNr] key names, as the [wilsjo2] section of
// dlssnr-amd.ini gives them; defaults as the 0.0.7 installer left the fork's (its keys at auto, WhitePointSource 1).
// A later pass's own values (keys Pass2... to Pass30...); each unset one inherits pass 1's.
struct PassOverride {
    bool has_preset = false, has_style = false, has_intensity = false, has_local_structure = false,
         has_local_tone = false, has_skin_structure = false, has_auto_mask = false;
    uint32_t preset = 0, style = 0;
    float intensity = 1.0f, local_structure = 1.0f, local_tone = 0.0f, skin_structure = -1.0f;
    bool auto_mask = true;
};
inline constexpr uint32_t kMaxPasses = 30;   // the ceiling with UnlockPasses

struct Settings {
    bool run_before_sr = false;              // RunBeforeSR
    uint32_t passes = 1;                     // Passes
    bool unlock_passes = false;              // UnlockPasses
    // ColourLimit (ours, default on; off = the fork's composition): the colour part of the network's edit bounded to
    // +/-1 stop per pass as RenoDX's resolve bounds it (ChromaClampStops, rc10), and the lower MaxRatio guard's extra
    // light added along the original colour, not the answer's (red / orange face edges, HL2 and KCD2).
    bool colour_limit = true;
    // Passes (fast) (ours, as a RenoDX layer's NRPassesMode / NRFastPasses / NRFastMethod / NRFastHaloLimit): one network
    // pass strengthened after it (nr_fast_passes.hpp), instead of Passes network passes
    uint32_t passes_mode = 0;                // PassesMode: 0 legacy, 1 fast
    uint32_t fast_method = 0;                // FastMethod: 0 Extrapolation, 1 Feedback, 2 Hybrid, 3 Edit strength
    // Each method's own, [0] Extrapolation, [1] Feedback, [2] Hybrid (Fast<Method>Passes / HaloLimit / Fine)
    float fast_passes[3] = {3.0f, 3.0f, 3.0f};       // 1..8, referred to legacy pass counts
    bool fast_halo_limit[3] = {false, false, false};
    float fast_fine[3] = {0.0f, 1.0f, 1.0f};         // 0..2
    float fast_edit_strength = 2.0f;         // FastEditStrength 0..4 (method 3)
    PassOverride pass[kMaxPasses];           // [k]: pass k's (k >= 1; key prefix Pass{k+1})
    uint32_t preset = 0;                     // Preset
    uint32_t style = 0;                      // Style
    float intensity = 1.0f;                  // Intensity
    float local_structure = 1.0f;            // LocalStructure
    float local_tone = 1.0f;                 // LocalTone
    float skin_structure = -1.0f;            // SkinStructure
    bool auto_mask = true;                   // AutoMask
    float history_strength = 1.0f;           // History (ours): post-block previous-frame blend
    bool noise_every_frame = true;           // NoiseEveryFrame (ours): frames without motion vectors, a new seed each
    // Not keys of the layer: NR Control's [Inspect] for the layer it chose (DebugView, SkinMask) - the fork's debug
    // views of its resolve: 1 the network's input, 2 its answer, 3 its change x20 around grey; the skin weight.
    uint32_t debug_view = 0;
    bool show_skin_mask = false;
    float transfer_strength = 1.0f;          // TransferStrength
    float colour_strength = 1.0f;            // ColourStrength
    float max_ratio = 2.0f;                  // MaxRatio
    uint32_t reversible_mode = 0;            // ReversibleMode
    float replace_detail_strength = 0.0f;    // ReplaceDetailStrength
    uint32_t transfer = 1;                   // Transfer: 0 bilinear, 1 matched residual, 2 lighting + colour, 3 Edge-aware,
                                             // 4 Classic (ours, both: the ReShade route's two enlargements)
    uint32_t classic_scaler = 1;             // ClassicScaler (Classic): 0 bilinear, 1 Catmull-Rom, 2 Lanczos3, 3 FSR 1
    uint32_t downscale = 0;                  // Downscale (below 100 %): 0 area after encoding, 1 bilinear before encoding
    float working_scale = 1.0f;              // WorkingScale (the model's resolution scale)
    uint32_t scaling_downscaler = 4;         // ScalingDownscaler (WorkingScale above 1)
    bool spatial = false;                    // SpatialCompression (peripheral compression)
    float spatial_centre[2] = {80.0f, 80.0f};   // SpatialCenterX / Y, percent
    float spatial_work[2] = {90.0f, 90.0f};     // SpatialWorkX / Y
    float spatial_offset[2] = {0.0f, 0.0f};     // SpatialOffsetX / Y
    float spatial_shift[2] = {0.0f, 0.0f};      // SpatialShiftX / Y
    float white_point_scale = 1.0f;          // WhitePointScale
    uint32_t white_point_source = 1;         // WhitePointSource (0 manual, 1 the game's exposure, 3 automatic)
    float white_point_trim = 1.0f;           // WhitePointTrim
    std::string exposure_trim_anchors;       // ExposureTrimAnchors
    float auto_exposure_trim = 5.0f;                  // AutoExposureTrim
    float auto_exposure_highlight_protection = 0.0f;  // AutoExposureHighlightProtection
    std::string auto_exposure_trim_anchors;           // AutoExposureTrimAnchors
    bool skin_protection = false;            // SkinProtection
    float skin_detail = 1.0f, skin_colour = 1.0f;                 // SkinDetail, SkinColour
    float environment_detail = 1.0f, environment_colour = 1.0f;   // EnvironmentDetail, EnvironmentColour
};
// The settings and source availability that choose a layer's input encoding. Measured exposure values may move
// every frame; those are scene adaptation, not a change of encoding settings.
struct EncodingState {
    bool valid = false, hdr = false;
    uint32_t mode = 0, source = 0, exposure_mode = 0;
    float scale = 1.f, trim = 1.f, auto_trim = 5.f, protection = 0.f;
    std::string anchors, auto_anchors;
    bool update(const Settings& s, bool is_hdr, uint32_t effective_exposure_mode) {
        const bool changed = !valid || hdr != is_hdr || mode != s.reversible_mode || source != s.white_point_source ||
            exposure_mode != effective_exposure_mode || scale != s.white_point_scale || trim != s.white_point_trim ||
            auto_trim != s.auto_exposure_trim || protection != s.auto_exposure_highlight_protection ||
            anchors != s.exposure_trim_anchors || auto_anchors != s.auto_exposure_trim_anchors;
        valid = true; hdr = is_hdr; mode = s.reversible_mode; source = s.white_point_source;
        exposure_mode = effective_exposure_mode; scale = s.white_point_scale; trim = s.white_point_trim;
        auto_trim = s.auto_exposure_trim; protection = s.auto_exposure_highlight_protection;
        anchors = s.exposure_trim_anchors; auto_anchors = s.auto_exposure_trim_anchors;
        return changed;
    }
};
// One key of the section into `s` (case-insensitive, as on the ReShade route); false for an unknown key. "auto" or an
// empty value leaves the default.
bool parse_key(Settings& s, const std::string& key, const std::string& value);

// The model's per-pass controls for pass 0, sanitised.
struct PassControls {
    uint32_t preset = 0, style = 0;
    float intensity = 1.0f, local_structure = 1.0f, local_tone = 1.0f, skin_structure = -1.0f;
    bool auto_mask = true;
};
PassControls pass0_controls(const Settings& s);
// Pass k's: pass 1's values with Pass{k+1}'s overrides (the preset only for passes 2 and 3), the local tone
// 0 for later passes unless set; sanitised.
PassControls pass_controls(const Settings& s, uint32_t k);
// Passes requested: Passes clamped to [1, 3], [1, 30] with UnlockPasses.
uint32_t passes_requested(const Settings& s);
// WorkingScale sanitised (non-finite 1, clamped to [0.25, 2]) and the work extent.
float working_scale(const Settings& s);
void work_extent(float scale, uint32_t width, uint32_t height, uint32_t* work_width, uint32_t* work_height);
// The same with the layer's Downscale: bilinear before encoding rounds below 100 % to a multiple of 8 (never above).
void work_extent(const Settings& s, float scale, uint32_t width, uint32_t height, uint32_t* work_width, uint32_t* work_height);

// Trim anchors: "point:trim" tokens, at most 8 kept, sorted, duplicates resolved; into the constants.
void parse_anchors(const std::string& text, Constants& c);

// The exposure constants of a frame, shared by the meter, the automatic exposure, the encode and the
// resolve. `measure`: the measuring branch runs (linear HDR, and source 3, or source 1 with a valid game exposure
// texture); otherwise everything stays zero (ExposureMode 0).
void exposure_constants(const Settings& s, bool measure, float frame_pre_exposure, uint32_t width, uint32_t height,
                        Constants& c);
// The meter's constants (mode 3: 64 x 64 groups, one a tile) and the automatic exposure's (mode 11, one
// group), on top of the exposure constants in `c`.
Constants meter_constants(Constants c);
Constants auto_exposure_constants(Constants c);
// The encode's constants and the resolve's, on top of the exposure constants in `c`.
Constants encode_constants(const Settings& s, Constants c, bool passthrough, uint32_t width, uint32_t height);
Constants resolve_constants(const Settings& s, Constants c, bool passthrough, uint32_t width, uint32_t height,
                            float work_scale);
// Peripheral compression: the layout of a frame W x H at working scale g. Inactive: ordinary NR.
struct SpatialAxis {
    float scale = 1.0f, half_band = 0.0f, band_centre = 0.0f, work_centre = 0.0f;
    float half_span[2] = {}, periphery[2] = {}, allotted[2] = {};
    float centre_frac[2] = {}, work_frac[2] = {}, compression[2] = {1.0f, 1.0f}, edge_slope[2] = {1.0f, 1.0f};
};
struct SpatialLayout {
    bool active = false;
    uint32_t native_w = 0, native_h = 0; // W, H
    uint32_t ord_w = 0, ord_h = 0;       // the ordinary extent (W, H at g)
    uint32_t model_w = 0, model_h = 0;   // the packed model extent
    SpatialAxis axis[2];
    // its identity (settings, W, H, g, active) for change detection
    std::string signature;
};
// spatial_layout() and spatial_constants() (the spatial passes' 256-byte block; rects (x, y, w, h) of the
// guides): below, inline.

// ---- peripheral compression ---------------------------------------------------------------------------
// Inline: the ReShade route's runtime (nr_runtime.cpp) runs the same layout without linking the rest of the style.
namespace spatial_detail {
inline bool finite(float v) { return std::isfinite(v); }
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
// step 3: the shift range of an axis from its fractions
inline void shift_limits(float cf, float wf, float of, float* lo, float* hi) {
    const float half = cf / 2.0f, bc = 0.5f + of;
    const float periphery[2] = {std::max(0.0f, bc - half), std::max(0.0f, 1.0f - bc - half)};
    const float budget = std::max(0.0f, wf - cf);
    const int narrow = periphery[0] <= periphery[1] ? 0 : 1;
    float base[2];
    base[narrow] = std::min(budget / 2.0f, periphery[narrow]);
    base[1 - narrow] = std::min(budget - base[narrow], periphery[1 - narrow]);
    *lo = -100.0f * std::max(0.0f, std::min(base[1], periphery[0] - base[0]));
    *hi = 100.0f * std::max(0.0f, std::min(base[0], periphery[1] - base[1]));
}
inline bool axis_valid(float c, float w, float o, float h) {
    if (!finite(c) || !finite(w) || !finite(o) || !finite(h)) return false;
    if (!(c > 0.0f) || w < 25.0f || w > 100.0f || !(c < w)) return false;
    const float max_offset = std::max(0.0f, (100.0f - c) * 0.5f - 0.5f);
    if (std::fabs(o) > max_offset + 1e-4f) return false;
    float lo = 0.0f, hi = 0.0f;
    shift_limits(c * 0.01f, w * 0.01f, o * 0.01f, &lo, &hi);
    return h >= lo - 1e-4f && h <= hi + 1e-4f;
}
// step 4, in double precision
inline uint32_t even_extent(uint32_t n, double f) {
    if (f == 1.0) return n;
    double v = double(n) * f;
    if (std::fabs(v - std::round(v)) <= 1e-6 * std::max(1.0, std::fabs(v))) v = std::round(v);
    uint32_t r = uint32_t(std::ceil(v));
    if (r & 1u) ++r;
    if (f < 1.0) r = std::min(r, n & ~1u);
    return std::max(2u, r);
}
// step 6, f32: N native, R raw work, M model extent; cf / of / sf the centre, offset and shift fractions
inline SpatialAxis build_axis(float N, float R, float M, float cf, float of, float sf) {
    SpatialAxis a;
    a.scale = M / R;
    a.half_band = cf * N / 2.0f;
    a.band_centre = (0.5f + of) * N;
    a.half_span[0] = a.band_centre;
    a.half_span[1] = N - a.band_centre;
    for (int i = 0; i < 2; ++i) a.periphery[i] = std::max(0.0f, a.half_span[i] - a.half_band);
    const float budget = std::max(0.0f, R - 2.0f * a.half_band);
    const int narrow = a.periphery[0] <= a.periphery[1] ? 0 : 1;
    float base[2];
    base[narrow] = std::min(budget / 2.0f, a.periphery[narrow]);
    base[1 - narrow] = std::min(budget - base[narrow], a.periphery[1 - narrow]);
    const float min_shift = -std::max(0.0f, std::min(base[1], a.periphery[0] - base[0]));
    const float max_shift = std::max(0.0f, std::min(base[0], a.periphery[1] - base[1]));
    const float shift = clampf(sf * N, min_shift, max_shift);
    a.allotted[0] = clampf(base[0] - shift, 0.0f, a.periphery[0]);
    a.allotted[1] = clampf(base[1] + shift, 0.0f, a.periphery[1]);
    if (R == M) {
        const float translation = a.half_band + a.allotted[0] - a.band_centre;
        for (const float cand : {std::round(translation), std::floor(translation), std::ceil(translation)}) {
            const float work_centre = a.band_centre + cand;
            const float left = work_centre - a.half_band, right = R - work_centre - a.half_band;
            if (left >= -1e-3f && right >= -1e-3f && left <= a.periphery[0] + 1e-3f && right <= a.periphery[1] + 1e-3f) {
                a.allotted[0] = clampf(left, 0.0f, a.periphery[0]);
                a.allotted[1] = clampf(right, 0.0f, a.periphery[1]);
                break;
            }
        }
    }
    // as the original's build (fast floating point) computes them: one reciprocal of the half span for both fractions
    for (int i = 0; i < 2; ++i) {
        const float inv = 1.0f / a.half_span[i];
        a.centre_frac[i] = a.half_band * inv;
        a.work_frac[i] = (a.half_band + a.allotted[i]) * inv;
        a.compression[i] = a.periphery[i] > 0.0f ? a.allotted[i] / a.periphery[i] : 1.0f;
        a.edge_slope[i] = a.compression[i] * a.compression[i];
    }
    a.work_centre = (a.half_band + a.allotted[0]) * a.scale;
    return a;
}
}  // namespace spatial_detail

inline SpatialLayout spatial_layout(const Settings& s, uint32_t W, uint32_t H, float g) {
    SpatialLayout l;
    l.native_w = W;
    l.native_h = H;
    // percentages become fractions as x * 0.01f (f32) and x * .01 (double): x / 100 rounds differently
    const float og = spatial_detail::finite(g) ? spatial_detail::clampf(g, 0.25f, 2.0f) : 1.0f;
    l.ord_w = std::max(1u, uint32_t(std::lround(float(W) * og)));
    l.ord_h = std::max(1u, uint32_t(std::lround(float(H) * og)));
    l.model_w = l.ord_w;
    l.model_h = l.ord_h;
    char sig[256];
    std::snprintf(sig, sizeof sig, "%d %g %g %g %g %g %g %g %g %u %u %g", s.spatial ? 1 : 0, double(s.spatial_centre[0]),
                  double(s.spatial_centre[1]), double(s.spatial_work[0]), double(s.spatial_work[1]),
                  double(s.spatial_offset[0]), double(s.spatial_offset[1]), double(s.spatial_shift[0]),
                  double(s.spatial_shift[1]), W, H, double(g));
    const auto done = [&](bool active) {
        l.active = active;
        if (!active) { l.model_w = l.ord_w; l.model_h = l.ord_h; }
        l.signature = std::string(sig) + (active ? " on" : " off");
        return l;
    };
    // step 2
    if (!s.spatial || W < 2 || H < 2 || !spatial_detail::finite(g) || g < 0.25f || g > 2.0f) return done(false);
    for (int i = 0; i < 2; ++i)
        if (!spatial_detail::axis_valid(s.spatial_centre[i], s.spatial_work[i], s.spatial_offset[i], s.spatial_shift[i])) return done(false);
    if (s.spatial_work[0] == 100.0f && s.spatial_work[1] == 100.0f) return done(false);
    // step 4
    const uint32_t raw_w = spatial_detail::even_extent(W, double(s.spatial_work[0]) * .01);
    const uint32_t raw_h = spatial_detail::even_extent(H, double(s.spatial_work[1]) * .01);
    const uint32_t model_w = spatial_detail::even_extent(W, double(g) * double(s.spatial_work[0]) * .01);
    const uint32_t model_h = spatial_detail::even_extent(H, double(g) * double(s.spatial_work[1]) * .01);
    // step 5
    if (float(model_w) < float(W) * 0.25f || float(model_h) < float(H) * 0.25f ||
        s.spatial_centre[0] * 0.01f >= float(raw_w) / float(W) || s.spatial_centre[1] * 0.01f >= float(raw_h) / float(H))
        return done(false);
    // step 6
    const uint32_t N[2] = {W, H}, R[2] = {raw_w, raw_h}, M[2] = {model_w, model_h};
    for (int i = 0; i < 2; ++i)
        l.axis[i] = spatial_detail::build_axis(float(N[i]), float(R[i]), float(M[i]), s.spatial_centre[i] * 0.01f,
                               s.spatial_offset[i] * 0.01f, s.spatial_shift[i] * 0.01f);
    // step 7
    for (const SpatialAxis& a : l.axis)
        for (int i = 0; i < 2; ++i)
            if (a.periphery[i] > 0.0f && a.allotted[i] < 1.0f) return done(false);
    l.model_w = model_w;
    l.model_h = model_h;
    return done(true);
}

// NGX scales turn guide values into active-guide pixels. Spatial packing needs native pixels (ordinary NR keeps the
// fork's work-size factor).
inline float native_motion_scale(float ngx_scale, uint32_t native_extent, uint32_t guide_extent) {
    return guide_extent ? ngx_scale * (float(native_extent) / float(guide_extent)) : ngx_scale;
}

inline Constants spatial_constants(const SpatialLayout& l, Mode mode, uint32_t width, uint32_t height, const float depth_rect[4],
                            const float motion_rect[4], float mv_x, float mv_y) {
    // by offset
    struct Block {
        uint32_t mode; float unused; uint32_t width, height;
        float native_size[4];                       // 16: W, H, model W, model H
        float fracs[4], minimums[4];                // 32, 48: not read by the programs
        uint32_t options[4];                        // 64: 2, 1, 1, 0
        float band_centre[4];                       // 80: band centre x, y; work centre x, y
        float side_neg[4], side_pos[4];             // 96, 112: half span x, y; centre fraction x, y
        float side_work_neg[4], side_work_pos[4];   // 128, 144: work fraction x, y; compression x, y
        float edge_slope[4];                        // 160: negative x, y; positive x, y
        float work_scale[4];                        // 176: scale x, y
        float depth_rect[4], motion_rect[4];        // 192, 208
        float motion_scale[2];                      // 224
        uint32_t padding[6];
    } b{};
    static_assert(sizeof(Block) == 256, "256 bytes");
    b.mode = uint32_t(mode);
    b.width = width;
    b.height = height;
    const SpatialAxis& x = l.axis[0];
    const SpatialAxis& y = l.axis[1];
    b.native_size[0] = float(l.native_w); b.native_size[1] = float(l.native_h);
    b.native_size[2] = float(l.model_w); b.native_size[3] = float(l.model_h);
    b.options[0] = 2; b.options[1] = 1; b.options[2] = 1; b.options[3] = 0;
    b.band_centre[0] = x.band_centre; b.band_centre[1] = y.band_centre;
    b.band_centre[2] = x.work_centre; b.band_centre[3] = y.work_centre;
    b.side_neg[0] = x.half_span[0]; b.side_neg[1] = y.half_span[0]; b.side_neg[2] = x.centre_frac[0]; b.side_neg[3] = y.centre_frac[0];
    b.side_pos[0] = x.half_span[1]; b.side_pos[1] = y.half_span[1]; b.side_pos[2] = x.centre_frac[1]; b.side_pos[3] = y.centre_frac[1];
    b.side_work_neg[0] = x.work_frac[0]; b.side_work_neg[1] = y.work_frac[0];
    b.side_work_neg[2] = x.compression[0]; b.side_work_neg[3] = y.compression[0];
    b.side_work_pos[0] = x.work_frac[1]; b.side_work_pos[1] = y.work_frac[1];
    b.side_work_pos[2] = x.compression[1]; b.side_work_pos[3] = y.compression[1];
    b.edge_slope[0] = x.edge_slope[0]; b.edge_slope[1] = y.edge_slope[0];
    b.edge_slope[2] = x.edge_slope[1]; b.edge_slope[3] = y.edge_slope[1];
    b.work_scale[0] = x.scale; b.work_scale[1] = y.scale;
    for (int i = 0; i < 4; ++i) { b.depth_rect[i] = depth_rect[i]; b.motion_rect[i] = motion_rect[i]; }
    b.motion_scale[0] = mv_x; b.motion_scale[1] = mv_y;
    Constants c;
    std::memcpy(&c, &b, sizeof c);
    return c;
}


// WorkingScale above 1: the filter of ScalingDownscaler between the frame's extent and the work extent.
// `up`: the proxy to the work extent (FSR1 EASU for 0, else the bicubic upsampler); else the answer back to the
// frame's extent (FSR1 EASU for 0, else the downsampler of that number).
// (inline: nr_runtime.cpp, the ReShade route's supersampling, uses it without linking the rest)
inline Constants scaling_constants(uint32_t scaler, bool up, uint32_t src_w, uint32_t src_h, uint32_t dst_w, uint32_t dst_h) {
    Constants c;
    c.width = dst_w;
    c.height = dst_h;
    if (scaler == 0) {
        // FsrEasuCon (AMD FidelityFX FSR 1, MIT) of the source as viewport and resource, to the destination, its four
        // words in ExposureAnchors 0..15 (the shader reads them back as bits)
        const auto rcp = [](float v) { return 1.0f / v; };
        const float iw = float(src_w), ih = float(src_h), ow = float(dst_w), oh = float(dst_h);
        const float words[16] = {iw * rcp(ow), ih * rcp(oh), 0.5f * iw * rcp(ow) - 0.5f, 0.5f * ih * rcp(oh) - 0.5f,
                                 rcp(iw), rcp(ih), 1.0f * rcp(iw), -1.0f * rcp(ih),
                                 -1.0f * rcp(iw), 2.0f * rcp(ih), 1.0f * rcp(iw), 2.0f * rcp(ih),
                                 0.0f * rcp(iw), 4.0f * rcp(ih), 0.0f, 0.0f};
        std::memcpy(c.exposure_anchors, words, sizeof words);
        c.mode = kOsEasu;
        return c;
    }
    c.mode = up ? kOsUp : kOsDown;
    c.debug_view = std::min(scaler, 7u);
    return c;
}

// [Preprocess] on a wilsjo2 layer (wilsjo2_prep.comp's push block, in the constants' 256 bytes). `kind`: what the proxy
// is - 0 HDR (the encode's curve + sRGB), 1 SDR (sRGB), 2 light as it is (a float frame not flagged HDR). `reset`: the
// meter starts again (Preprocess just turned on or changed); `dt`: seconds since the layer's last metered frame.
inline Constants prep_constants(Mode mode, const nr::Preprocess& pp, uint32_t w, uint32_t h, uint32_t kind, uint32_t rmode,
                                bool reset, float dt) {
    struct {
        uint32_t mode; float unused; uint32_t width, height, flags, curve;
        float bias, contrast, saturation, k, cap, dt;
        uint32_t rmode, kind;
    } p{};
    const uint32_t curve = uint32_t(std::clamp(pp.curve, 0, 6));
    p.mode = mode; p.width = w; p.height = h;
    p.flags = (pp.exposure == 1 ? 1u : 0u) | (kind == 0u ? 2u : 0u) | (reset ? 8u : 0u) | (pp.inverse ? 0u : 16u);
    p.curve = curve;
    p.bias = pp.exposure == 0 ? 0.0f : std::clamp(pp.bias_ev, -8.0f, 8.0f);
    p.contrast = std::clamp(pp.contrast, 0.5f, 2.0f);
    p.saturation = std::clamp(pp.saturation, 0.05f, 2.0f);
    p.k = kPreprocessAnchor[curve];
    p.cap = 4.0f;
    p.dt = dt;
    p.rmode = rmode <= 4u ? rmode : 0u;
    p.kind = kind;
    Constants c;
    std::memcpy(&c, &p, sizeof p);
    return c;
}

// An image a pass reads or writes: the caller's (the game's or its own), in `layout` while the pass records.
struct Image {
    VkImage image{};
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width{}, height{};
    VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL;
    // read slots: the caller's own view to read through (its mip level, array layer and channel order), else one of
    // ours; ours are made at `mip` / `layer`
    VkImageView view{};
    uint32_t mip = 0, layer = 0;
    explicit operator bool() const { return image != VK_NULL_HANDLE; }
};

// One dispatch of the fork's: its images by slot, its constants; Width x Height in 8x8 groups (the
// meter: Width x Height groups). A read slot left empty is given t0, an empty u1 is given u0.
struct Dispatch {
    Image t0, t1, t2, t3, u0, u1;
    Constants constants{};
};

// HUD FIX's constants (wilsjo2_hudfix.comp): `mode` (kHud*), the extent, the source (0 a UI alpha, 1 a UI colour's
// alpha, 2 the difference mask against a HUD-less frame, 4 the estimate), RenoDX's limits (present_path.hpp
// kPresentPreFgEpsilon 1/510, kPresentPreFgHudShare 0.25, the residual form as its Auto), the estimate's share of
// changed pixels for a moving frame (0.25), ShowMask.
// `fresh`: the estimate's state image is new (its contents read as 0).
// `parity`: the estimate's tile-flag set read this frame (the other one is written; the caller flips it a frame).
Constants hudfix_constants(uint32_t mode, uint32_t w, uint32_t h, uint32_t source, bool show_mask, bool fresh = false,
                           uint32_t parity = 0);
// The estimate's motion vectors (t2) into its constants: output pixels per unit, their region's origin, texels per
// output pixel
void hudfix_motion(Constants& c, float scale_x, float scale_y, uint32_t x, uint32_t y, float step_x, float step_y);
// kCopy / kCopyLoad's constants: `w` x `h` texels from (sx, sy) of the source to (dx, dy) of u0
Constants copy_constants(Mode mode, uint32_t sx, uint32_t sy, uint32_t dx, uint32_t dy, uint32_t w, uint32_t h);

class Stage {
public:
    // `shaders`: the folder holding wilsjo2_<mode>.spv.
    Stage(const HostDevice&, const std::string& shaders);
    ~Stage();
    Stage(const Stage&) = delete;
    Stage& operator=(const Stage&) = delete;

    // Records the dispatch, with a barrier after it so the next one (or anything after) sees its writes. Throws
    // std::runtime_error when it cannot (a mode this stage does not have, an image it cannot view).
    void record(VkCommandBuffer, const Dispatch&);
    // Once per recording that used the stage: views and descriptor sets made for it are freed once 64 later
    // recordings have passed (the GPU may still be running it).
    void end_recording();
    // Whether this stage has the program of `mode` (the optional ones: [Preprocess], the comparison, HUD FIX).
    bool has(Mode mode) const;
    // Passes (fast)'s programs (nr_fast_passes.hpp; wilsjo2_st_*.spv, the RenoDX-style stage's st_* sources): optional,
    // each with its own bindings (sampled images and storage images by binding number) and push block. Records one
    // dispatch of gx x gy groups, with a barrier after it. Throws as record() does.
    bool has_fast() const;
    struct FastImage { uint32_t binding; Image image; };
    void record_fast(VkCommandBuffer, uint32_t program, const FastImage* images, uint32_t count, const void* push,
                     uint32_t bytes, uint32_t gx, uint32_t gy);

    struct Impl;
private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace nr::wilsjo2
