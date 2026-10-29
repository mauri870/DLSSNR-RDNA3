#pragma once
// RenoDX-style processing ([RenoDX] in dlssnr-amd.ini): the frame is encoded for the network and the
// network's answer is put back onto the frame the way the RenoDX DLSS5 add-on (v8.5.0-rc10, MIT,
// renodx/LICENSE) does it on its D3D12 path, with its own shaders, unmodified. The network itself runs
// in between exactly as on every other route: the stage hands the runtime a proxy image and gets the
// network's answer back in an image of its own.
//
//   frame -> original (copy) -> [HDR: linearize, autoscale] then, per pass of the stack (1 unless the route
//   runs several model passes), the running image -> [HDR: v6 encode | SDR: legacy encode] -> proxy ->
//   network -> neural -> [look: Adjust NR's look, Detail stability, edge-aware upsampling] -> [HDR: v6
//   resolve | SDR: legacy decode, black-level restore] -> the next pass's running image; then [HDR: pedestal,
//   commit] -> the frame's output. A chain whose every pass is at zero strength leaves the frame as it is.
//
// Off by default; a runtime without a stage records exactly what it recorded before.
#include "nr_runtime.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace nr::renodx {

// RenoDX's own settings, under its own ini names (NR...), with its defaults (dlssnr.hpp namespace
// defaults). With the option on they are all of NR's settings: the route's own (OptiScaler's NR page,
// [DlssNr]) stop applying. Not here: what has no counterpart in this network (preset, UI correction), frame
// generation, and RenoDX's own diagnostics.
// The passes a frame runs: NRPasses, at most 4 as RenoDX's own, at most 30 with NRUnlockPasses (OptiScaler's ceiling).
inline constexpr uint32_t kMaxUnlockedPasses = 30;

struct Settings {
    bool nr_enabled = true;             // NeuralUplift: NR runs (off: the frame as it came)
    // The network's controls (DLSSNR.Intensity ...), pass 1's.
    float intensity = 1.0f;             // NRIntensity 0..2
    uint32_t style = 0;                 // NRStyle 0..2
    float local_tone = 1.0f;            // NRLocalTone 0..2
    float local_structure = 1.0f;       // NRLocalStructure 0..2
    float skin_structure = -1.0f;       // NRSkinStructure -1 (stock) .. 1
    bool auto_mask = false;             // NRAutoMask
    // The stack: passes 2..4 with their own controls once "Same as pass 1" is off (NRPass2FollowPass1 ...).
    uint32_t passes = 1;                // NRPasses 1..4 (1..30 with NRUnlockPasses)
    bool unlock_passes = false;         // NRUnlockPasses: ours, as OptiScaler's UnlockPasses (passes 5.. take pass 4's)
    float pass_intensity[3] = {1.0f, 1.0f, 1.0f};
    bool pass_follow[3] = {true, true, true};
    uint32_t pass_style[3] = {0, 0, 0};
    float pass_local_tone[3] = {1.0f, 1.0f, 1.0f};
    float pass_local_structure[3] = {1.0f, 1.0f, 1.0f};
    float pass_skin_structure[3] = {-1.0f, -1.0f, -1.0f};
    bool pass_auto_mask[3] = {false, false, false};
    // Where NR runs on the OptiScaler route (NRHookPoint): 0 after the upscaler, 1 before it (not with Ray
    // Reconstruction). The ReShade routes are RenoDX's Present hook point.
    uint32_t hook_point = 0;
    uint32_t present_guides = 0;        // NRPresentGuides: 0 optional, 1 required, 2 never (neutral guides)
    uint32_t depth_mode = 0;            // NRDepthMode: 0 the game's flag, 1 normal, 2 inverted
    float mvec_scale_x = 1.0f, mvec_scale_y = 1.0f;   // NRMVecScaleX / Y: the motion vectors scaled

    uint32_t codec_mode = 2;            // NRCodecMode: 0 classic, 1 anchored, 2 auto, 3 display
    uint32_t source_encoding = 0;       // NRSourceEncoding: 0 from the format, 1 SDR, 2 linear, 3 PQ
    float linear_unit_nits = 0.0f;      // NRLinearUnitNits: > 0 makes float HDR absolute
    float proxy_anchor_nits = 4.0f;     // NRProxyAnchor
    float paper_white_scale = 2.5375f;  // NRPaperWhiteScale
    float diffuse_white_nits = 203.0f;  // NRDiffuseWhiteNits
    float pq_calibration = 1.0f;        // NRPQCalibration
    float transfer_strength = 1.0f;     // NRTransferStrength
    float color_strength = 1.0f;        // NRColorStrength
    float chroma_clamp_stops = 1.0f;    // NRChromaClamp
    uint32_t transfer_mode = 0;         // NRTransferMode
    float style_highlight_guard = 0.0f; // NRStyleHighlightGuard
    uint32_t display_pedestal = 0;      // NRDisplayPedestal
    uint32_t sdr_black_restore = 0;     // NRSdrBlackRestore: 0 auto, 1 every block, 2 smooth
    bool neural_floor_guard = true;     // NRNeuralFloorGuard
    uint32_t feed_mode = 0;             // NRFeedMode: 0 auto, 1 the content meter, 2 the game's exposure
    uint32_t norm_governor = 2;         // NRNormGovernor
    bool norm_cut_aware = true;         // NRNormCutAware
    float norm_attack_stops = 4.0f;     // NRNormAttackStops
    float norm_release_stops = 0.5f;    // NRNormReleaseStops
    float norm_slew_stops = 0.02f;      // NRNormSlewStops
    uint32_t detail_stability = 0;      // NRDetailStability: 0 auto, 1 off, 2 always
    float resolution_scale = 1.0f;      // NRResolutionScale: the network at 0.33..1 of the frame
    bool follow_input_res = false;      // NRFollowInputRes: the network at the game's render resolution
    uint32_t look_upsample = 0;         // NRLookUpsample: 0 auto, 1 edge-aware, 2 classic, 3 area input
    // Adjust NR's look (look_stage.hpp Settings; every default the identity, NRLookMode off).
    bool look_mode = false;             // NRLookMode
    float look_strength = 1.0f;         // NRLookStrength 0..2
    float look_brighten = 1.0f;         // NRLookBrighten 0..2
    float look_darken = 1.0f;           // NRLookDarken 0..2
    float look_max_brighten = 0.0f;     // NRLookMaxBrighten 0..4 stops, 0 = no limit
    float look_max_darken = 0.0f;       // NRLookMaxDarken 0..4 stops, 0 = no limit
    float look_colour = 1.0f;           // NRLookColour 0..2
    float look_hue = 1.0f;              // NRLookHue 0..2
    float look_max_colour = 0.0f;       // NRLookMaxColour 0..2, 0 = no limit
    float look_shadows = 1.0f;          // NRLookShadows 0..2
    float look_midtones = 1.0f;         // NRLookMidtones 0..2
    float look_highlights = 1.0f;       // NRLookHighlights 0..2
    float look_tone = 1.0f;             // NRLookTone 0..2
    float look_detail = 1.0f;           // NRLookDetail 0..2
    float look_detail_radius = 1.0f;    // NRLookDetailRadius 0.25..4 (percent of the network's height)
    float look_halo = 0.0f;             // NRLookHalo 0..1
    float look_halo_bright = 0.0f;      // NRLookHaloBrightSide 0..1
    uint32_t look_stabilize = 0;        // NRLookStabilize: 0 off, 1 static, 2 along the motion vectors
    float look_stabilize_ms = 60.0f;    // NRLookStabilizeMs 10..250
    bool look_stabilize_detail = false; // NRLookStabilizeDetail
    // A stack of model passes: pass 2, 3, 4 (and later) resolve with their own strengths (NRPass2Transfer ...);
    // pass 1 takes NRTransferStrength / NRColorStrength.
    float pass_transfer[3] = {1.0f, 1.0f, 1.0f};   // NRPass2Transfer .. NRPass4Transfer 0..1
    float pass_color[3] = {1.0f, 1.0f, 1.0f};      // NRPass2Color .. NRPass4Color 0..1
    bool chained_history = true;        // NRChainedHistory: passes 2+ keep their history (off: they restart every frame)
    // NRSkinIndependent: with the Character mask on, Structure is sent at most 1 and a "stock" Skin as 1, so a
    // Structure change no longer moves faces (dlssnr.hpp SentSkinSteering). Applied to the network's controls.
    bool skin_independent = true;
    // RenoDX's Present hook point, which is what the ReShade routes are: the finished frame at Present.
    uint32_t present_encoding = 0;      // NRPresentEncoding: 0 the swapchain's colour space, 1 SDR, 2 HDR10, 3 scRGB
    float present_white_nits = 0.0f;    // NRPresentWhiteNits: 0 automatic
    uint32_t present_white_source = 0;  // NRPresentWhiteSource: 0 auto (a RenoDX mod's, else Windows'), 1 Windows'
};
inline uint32_t stack_passes(const Settings& s) {
    const uint32_t most = s.unlock_passes ? kMaxUnlockedPasses : 4u;
    return s.passes < 1u ? 1u : s.passes > most ? most : s.passes;
}

// RenoDX's ComputeNrWorkingResolution: the network's extent for a frame of `reference` size whose guides
// (depth, motion vectors) are `guide` size.
void working_resolution(const Settings& s, uint32_t reference_width, uint32_t reference_height, uint32_t guide_width,
                        uint32_t guide_height, uint32_t* nr_width, uint32_t* nr_height);
// RenoDX's kContractDebounceFrames: a working resolution the game's guides change to is taken once it has held
// this many frames (a settings change is taken at once).
inline constexpr uint32_t kContractDebounceFrames = 30;

struct StageConfig {
    std::string shaders;              // the directory holding renodx_*.spv
    uint32_t width{}, height{};       // the frame
    uint32_t nr_width{}, nr_height{}; // the network's extent (the proxy and its answer)
    VkFormat frame_format = VK_FORMAT_UNDEFINED;
    // The game marked its colour as HDR (DLSS IsHDR). Decides R10G10B10A2 (PQ or SDR) and promotes an
    // 8-bit frame, as RenoDX's FrameHdrMode does; a float frame is linear light either way.
    bool hdr_flag = false;
    // The Present hook point (the ReShade routes): the frame's encoding as RenoDX's Present path decides it -
    // 1 SDR (legacy codec), 2 HDR10 (PQ over BT.2020, absolute), 3 scRGB (linear, 80 nits = 1.0, absolute). 0: the
    // DLSS output path, decided by the format and hdr_flag.
    uint32_t present = 0;
    // The settings at creation: NRSourceEncoding / NRLinearUnitNits decide relative or absolute units, which a stage
    // keeps for its life (a change between them takes a new stage, made with the new settings).
    Settings settings{};
};

struct StageFrame {
    ColourFrame colour;     // read
    ColourFrame output;     // written; may be the same image as `colour`
    bool reset = false;     // DLSSNR.Reset
    bool after_upscale = true;
    // The game's motion vectors, for Detail stability: the region (x, y, motion.width/height) of the
    // texture and the scale that turns a texel into normalised screen units (EngineFrame's convention).
    ColourFrame motion;
    uint32_t motion_x = 0, motion_y = 0;
    float motion_scale_x = 0.0f, motion_scale_y = 0.0f;
    // The game's exposure as DLSS was given it (RenoDX's feed v2): the exposure texture (texel 0,0 is read),
    // DLSS.Pre.Exposure and DLSS.Exposure.Scale, and whether the game created its DLSS with AutoExposure
    // (then the texture is not used). No image: there is none.
    ColourFrame exposure;
    float pre_exposure = 1.0f, exposure_scale = 1.0f;
    bool auto_exposure = false;
    // Whether the game's create flags were seen (auto_exposure means something). Unseen is no evidence for the
    // texture, and Auto keeps the content meter (RenoDX's "any absent or ambiguous signal keeps v1").
    bool create_flags_known = true;
    // [Preprocess] with this mode (docs: the RenoDX-style option and Preprocess): on HDR its forward half runs before
    // RenoDX's encode and RenoDX's N/P resolve takes it out again; on SDR, whose answer RenoDX takes whole, its own
    // undo runs on the answer. Its exposure, when on, replaces RenoDX's divisor (1); its curve, when set, RenoDX's.
    Preprocess preprocess{};
    // The Present hook point: the HDR diffuse white in nits (RenoDX's: NRPresentWhiteNits, a RenoDX mod's Game
    // Brightness, the Windows SDR content level, NRDiffuseWhiteNits).
    float present_white_nits = 203.0f;
};

class Stage {
public:
    Stage(const HostDevice&, const StageConfig&);
    ~Stage();
    Stage(const Stage&) = delete;
    Stage& operator=(const Stage&) = delete;

    // The settings for the next frame (read once per frame).
    void set(const Settings&);
    // 0 SDR (legacy codec), 1 linear HDR, 2 PQ: what this frame format and flag resolve to.
    uint8_t hdr_mode() const;

    // The network for one pass of the stack: reads `proxy`, writes its answer into `neural` (both
    // StageConfig::nr_width x nr_height, RGBA16F); `reset` restarts that pass's history.
    using Network = std::function<void(uint32_t pass, const ColourFrame& proxy, const ColourFrame& neural, bool reset)>;
    // The frame through every pass of the stack (Settings::passes) into StageFrame::output. Returns how many passes
    // ran the network (0: NR off, or every pass at zero strength - the frame copied through as it is).
    uint32_t record(VkCommandBuffer, const StageFrame&, const Network&);

    // What the last record() decided about the frame, for the menus' engine-data panel.
    struct Facts {
        uint8_t hdr_mode = 0;        // 0 SDR, 1 linear HDR, 2 PQ
        bool absolute = false;       // absolute units: the frame's own nits, no divisor
        bool prep_exposure = false;  // [Preprocess]'s exposure replaced the divisor
        int feed = -1;               // the divisor's feed: -1 none (SDR, absolute, Preprocess), 0 the content meter,
                                     // 1 the game's exposure texture, 2 fixed 1
        int exposure_texture = -1;   // -1 not looked at, 0 none, 1 readable, 2 not a format the feed reads
        // The divisor the encode applied to the linear frame (the scale texel's .r, read back every 32nd frame a few
        // frames late); 0 not known (SDR, absolute units, not read yet).
        float divisor = 0.f;
    };
    Facts facts() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace nr::renodx
