#pragma once
#include <utility>
#include <vector>
#include <vulkan/vulkan.h>
#include "nr_native_plan.hpp"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace nr {

struct RuntimeCore;   // Runtime::share_for
struct ImageHeapRef;  // Runtime::image_heap_wanted
struct ActArenaRef;   // Runtime::act_arena
uint64_t act_arena_bytes(const ActArenaRef&);
const void* act_arena_id(const ActArenaRef&);
// A plan's working extent and the tallest frame (model) height it runs.
void plan_extent(const RuntimeCore& plan, uint32_t* work_w, uint32_t* work_h, uint32_t* source_h);
// The working extent a frame of width x height runs at with `model` and Model Resolution `scale` (the native plan's
// padding), and the tallest frame height with the same one at that width: a plan lowered there runs every height of
// it (RuntimeConfig::share). Up to ~10 ms: not on a render thread.
void working_extent(NetworkModel model, float scale, uint32_t width, uint32_t height, uint32_t* work_w, uint32_t* work_h);
uint32_t plan_height(NetworkModel model, float scale, uint32_t width, uint32_t height);
// The network memory a core runs in (Runtime::network_id of the runtimes built from it), and dropping its image heap
// (the images of runtimes built inside frames; those in it keep their part until they go): a network kept by a core
// alone - its weights and arenas, no runtime - holds no images.
const void* core_network(const RuntimeCore& core);
void release_image_heap(const RuntimeCore& core);
std::shared_ptr<ActArenaRef> core_act_arena(const RuntimeCore& core);

// One later pass's own model controls. Absent fields inherit pass 1's, except
// the tone, which the original's own hosts default to 0 on every pass after the
// first - a second pass that re-applies the tone control compounds it, measured
// at x1.052 -> x1.102 -> x1.136 over three passes.
struct PassControls {
    bool used = false;              // false: this pass inherits pass 1 entirely
    int style = 0;
    float intensity = 1.0f;
    float local_tone = 0.0f;
    float local_structure = 1.0f;
    float skin_structure = -1.0f;
    bool automatic_mask = true;
};

// [Preprocess] in dlssnr-amd.ini: what the network is shown is changed before
// it runs and its edit is mapped back by a bounded inverse or a ratio
// (linux/shaders/renodx-prep/prep_math.glsl: the RenoDX stage's and the wilsjo2 flow's Preprocess passes). Opt-in;
// the defaults below are what Enabled=1 starts from (exposure off, filmic curve), and each field has
// a value that leaves the frame as the host handed it over (exposure off,
// curve 0, contrast and saturation 1).
// Each curve's input scale that leaves mid grey (0.18) where it was, so the curves shape shadows and highlights and
// the exposure alone sets brightness (solved numerically, once for each curve); by Preprocess::curve.
inline constexpr float kPreprocessAnchor[7] = {1.0f, 1.0f, 1.2195122f, 2.9275228f, 1.0052344f, 0.7231708f, 0.8083602f};
struct Preprocess {
    bool enabled = false;
    // 0 off; 1 auto: a game engine's histogram auto exposure
    // (prep_math.glsl meter()), plus bias_ev; 2 fixed: bias_ev alone
    int exposure = 0;
    float bias_ev = 0.0f;        // -8..8
    // 0 none, 1 neutral, 2 reinhard, 3 filmic, 4 gt, 5 aces, 6 agx
    int curve = 3;
    float contrast = 1.0f;       // about mid grey, 0.5..2
    float saturation = 1.0f;     // 0.05..2
    // How the network's change comes back: through the bounded inverse of curve, contrast and saturation (true),
    // or as its ratio in the shown picture (false). Clipping, caps and storage can limit either result.
    bool inverse = true;
    // Does any of it change anything? False records nothing.
    bool active() const {
        return enabled && (exposure == 1 || (exposure == 2 && bias_ev != 0.0f) || curve != 0 ||
                           contrast != 1.0f || saturation != 1.0f);
    }
    bool operator==(const Preprocess& o) const {
        return enabled == o.enabled && exposure == o.exposure && bias_ev == o.bias_ev && curve == o.curve &&
               contrast == o.contrast && saturation == o.saturation && inverse == o.inverse;
    }
    bool operator!=(const Preprocess& o) const { return !(*this == o); }
};

// In-process interface shared by game adapters and the native menu.
// Controls::transfer
constexpr int kEnlargeMatched = 0, kEnlargeEdgeAware = 1, kEnlargeClassic = 2,
              // OptiScaler DLSS-NR's own two: its Transfer 0 (the model's pair sampled bilinearly, composed as at 100 %)
              // and Transfer 3 (lighting + colour carried as a resize field)
              kEnlargeBilinear = 3, kEnlargeField = 4;
// The ini's Transfer numbers, the same in [DlssNr] and every [LayerN] (OptiScaler DLSS-NR's 0 / 1 / 2, then this route's
// own two): 0 bilinear, 1 matched residual, 2 lighting + colour, 3 edge-aware lighting + colour, 4 classic
constexpr int kEnlargeFromIni[5] = {kEnlargeBilinear, kEnlargeMatched, kEnlargeField, kEnlargeEdgeAware, kEnlargeClassic};
constexpr int kEnlargeToIni[5] = {1, 3, 4, 0, 2};

struct ColourFrame;
// A run's settings. The runtime runs the network only and reads the network's controls (intensity, style, local tone
// and structure, skin structure, the mask, the noise seed, the history strength); the composition settings (debug view
// through scaling_downscaler, passes, per_pass, preprocess) are a wilsjo2 layer's on the ReShade route, which hands them to
// its processing (nr_reshade_addon.cpp w2_settings, nr_wilsjo2_flow.hpp).
struct Controls {
    bool enabled = true;
    bool apply_model = true;
    // NR Control's [Inspect] for this layer (wilsjo2_resolve.comp): DebugView 0 off, 1 the network's input, 2 its
    // answer, 3 its change x20 around grey; the skin weight.
    uint32_t debug_view = 0;
    bool show_skin_mask = false;
    float intensity = 1.0f;
    // The composition onto the frame (wilsjo2_resolve.comp), as OptiScaler DLSS-NR's resolve: Detail strength
    // (TransferStrength, 0..2), Colour strength (0..4, above 1 boosts saturation) and Highlight guard (MaxRatio, 1..8).
    float detail_strength = 1.0f;
    float colour_strength = 1.0f;
    float max_ratio = 2.0f;
    // Below 100% Model resolution, how the model's result is enlarged to the frame
    // (wilsjo2_resolve.comp), kEnlarge*: matched residual (the default), edge-aware
    // lighting + colour, classic. Per frame; nothing is rebuilt.
    int transfer = 0;
    // Classic's scaler: 0 bilinear, 1 Catmull-Rom, 2 Lanczos3, 3 FSR 1 EASU.
    int classic_scaler = 1;
    // Edge-aware: the enlargement weighted by the full-resolution proxy (else plain
    // bilinear), and the weight's range in stops. Not user settings.
    bool transfer_guided = true;
    float transfer_sigma = 0.25f;
    // OptiScaler DLSS-NR's SkinProtection (its resolve's last step, wilsjo2_resolve.comp): the composed picture's
    // detail (luminance) and colour kept by these amounts, 0..1, by how skin-coloured the frame's pixel is - Skin*
    // on skin, Environment* elsewhere. Off: nothing of it runs.
    bool skin_protection = false;
    float skin_detail = 1.0f, skin_colour = 1.0f, environment_detail = 1.0f, environment_colour = 1.0f;
    // OptiScaler DLSS-NR's ReversibleMode: the highlight curve a linear-light frame is shown to the network through
    // (wilsjo2_encode.comp: 0 the soft knee, 1 / 2 Neutwo, 3 / 4 the hybrid knee) and, in 2 and 4, the network's answer
    // decoded through it replacing the composition (wilsjo2_resolve.comp). ReplaceDetailStrength (0..2): in 2 and 4
    // below 100 % Model resolution, the frame's local contrast put back on that answer.
    int reversible_mode = 0;
    float replace_detail = 0.0f;
    // OptiScaler DLSS-NR's ScalingDownscaler, Model resolution above 100 % only: the filters between the frame's extent
    // and the network's (nr::wilsjo2::scaling_constants): 0 FSR 1 (a bilinear resample both ways, as OptiScaler runs it),
    // else the bicubic up filter and down filter 1 bicubic, 2 Catmull-Rom, 3 / 4 Lanczos 2 / 3, 5 / 6 Kaiser 2 / 3, 7 MAGIC.
    int scaling_downscaler = 4;
    // A wilsjo2 layer's passes (each one network run of the flow's): pass k+1 takes pass k's answer as its input,
    // composed once at the end against what the first pass saw. Cost is linear in the count.
    int passes = 1;
    int style = 0;
    float local_tone = 1.0f;
    float local_structure = 1.0f;
    float skin_structure = -1.0f;
    bool automatic_mask = true;
    // Per-pass overrides for passes 2..N; index 0 is pass 2. An entry with
    // `used == false`, or a pass past the end of this, inherits pass 1 with the
    // tone zeroed. Empty is exactly the behaviour that shipped before.
    std::vector<PassControls> per_pass{};
    Preprocess preprocess{};
    // A frame without motion vectors (Runtime::record): the pre block's noise seed moves every frame, as the DLL's
    // counter does whatever the motion vectors. Off (the default): the seed stays 0 - without a history to average
    // the noise, a moving seed shimmers on a still picture.
    bool noise_every_frame = false;
    // Seed 0 on every frame, a motion source or not (the RenoDX style's NRNoiseEveryFrame off); the DLL never does it.
    bool seed_fixed = false;
    // Optional layer timestamp writer at the first/last network dispatch (including model pre/post).
    std::function<void(VkCommandBuffer, bool)> network_timestamp;
    uint64_t feature = 0; // identity for non-temporal seed and Preprocess state
    float history_strength = -1.f; // negative: the session default; otherwise this layer's post blend
    bool model_capture_allowed = true; // an outer multipass host arms only its final pass
    // Temporal reuse of the network's edit (RuntimeConfig::reuse). 0 or 1: the network runs on every frame.
    // N >= 2: it runs on every Nth frame, and the frames between carry its last edit forward - the edit
    // (answer minus what the network was shown) looked up where the accumulated motion says each pixel was
    // when it was made - onto what the network would have been shown this frame. A frame with `reset`
    // always runs the network. Needs the temporal path - the engine's motion vectors, or the fallback estimator's
    // when there are none - and one pass, no preprocess.
    int reuse_every = 0;
    // The most the picture may have moved, in pixels of a 1080p-wide frame (scaled with the frame's width),
    // for the network to be skipped: a frame whose predicted accumulated motion is larger runs the network
    // instead. The motion is the mean of the previous frame's, times the frames since the network ran.
    float reuse_gate = 40.0f;
    // Optional readback of the last pass before and after its network, before composition.
    std::function<void(VkCommandBuffer, const ColourFrame&, const ColourFrame&, bool)> model_capture;
};

struct HostDevice {
    VkInstance instance{};
    VkPhysicalDevice physical{};
    VkDevice device{};
    VkQueue queue{};
    uint32_t queue_family{};
    // Vulkan layers receive inner physical-device handles. Route physical
    // queries through their next instance dispatch, not the outer loader.
    // Null for ordinary application-owned (outer-loader) handles.
    PFN_vkGetInstanceProcAddr physical_dispatch{};
    // The host API's queue lock, taken by the constructor around each of its
    // submits only. Empty: the caller serialises the whole construction.
    std::function<void()> queue_lock, queue_unlock;
};
// [Network] Residency offload: a network held by a core alone (no runtime, no frame of it in flight) moves its weights
// to RAM and releases its device memory (its weight buffer, its hold on the activation arena); reload_network brings
// them back before a runtime is built from the core, into `act` when it has the room (else a new arena with `slack`).
// The first recording then reloads the plan's tail and noise fields and zeroes the arena (retarget). Both submit (the
// host's queue lock): not on a render thread. False when there is nothing to do or it failed (the log says why).
bool offload_network(const HostDevice& host, const RuntimeCore& core);
// offload_network's halves: the read-back (any thread; nothing of the network changes) and the release (on the thread
// that records, once no frame of it is in flight).
bool download_network_weights(const HostDevice& host, const RuntimeCore& core, std::vector<uint8_t>& out);
void release_network(const RuntimeCore& core, std::vector<uint8_t>&& weights);
// Whether a core's network runs a frame of width x height (its working extent, the frame no taller than it was built
// for), as Runtime::share_for decides it.
bool core_runs(const RuntimeCore& core, uint32_t width, uint32_t height);
bool reload_network(const HostDevice& host, const RuntimeCore& core, const std::shared_ptr<ActArenaRef>& act, double slack);
bool network_offloaded(const RuntimeCore& core);
uint64_t network_ram_bytes(const RuntimeCore& core);

struct RuntimeConfig {
    std::string root;
    std::string plan;  // empty: compiled native planner; optional file must match it
    std::string accumulation = "fp32";
    std::string adapter_shaders;  // empty: ROOT/build/runtime
    uint32_t width{}, height{};
    VkFormat colour_format = VK_FORMAT_R32G32B32A32_SFLOAT;
    // The network: the 310.8 model (dlssnr.bin) or the 310.9 small model
    // (dlssnr-small.bin). Same shaders; the plan and the weights differ.
    // -1 = as DLSSNR_MODEL / dlssnr-amd.ini [Network] Model say (small or original;
    // neither: small), 0 = the original model, 1 = the small model, 2 = the small
    // model's faster variant CC_SMALL_FP8_EXPERIMENTAL (its own weights and shaders,
    // dlssnr-small-experimental.bin and experimental/; without them, the small model).
    // The small model needs an installed tree with dlssnr-small.bin; without it, the
    // original model.
    int network = -1;
#if NR_INT4
    // The int4 mixed network: -1 = as DLSSNR_INT4 / dlssnr-amd.ini [Int4Mixed] Enabled say (start), 0 = the original
    // network, 1 = int4 mixed when installed and the device has VK_KHR_pipeline_binary ([Int4Mixed] Hotkey switches).
    int int4_mixed = -1;
#endif
    // The game's motion vectors read with point sampling at the colour's coordinate, as NVIDIA's DLL binds them
    // (MIN_MAG_MIP_POINT, the colour's own sampler; launch capture 2026-10-18). Bilinear blends two surfaces'
    // vectors at every silhouette when the vectors are at a lower resolution than the colour (after-SR edge band
    // 30.3 -> 43.9 dB vs NVIDIA with point). The NGX route sets it; the ReShade add-on's vectors (VORT, Lumenite)
    // are our own input and keep bilinear.
    bool motion_point = false;
    // The resize path: Runtime::share_for of a runtime at another source extent. Its network - lowered at the same
    // working extent (the native plan's padding), its arenas - is taken over, and only this runtime's images and
    // descriptor sets are made; null, or not matching (the log says so), builds one. Byte for byte the network a
    // build at this extent makes.
    std::shared_ptr<const RuntimeCore> share;
    // The lowering only, for Runtime::plan: no device memory, no images, nothing to record with.
    bool plan_only = false;
    // Built inside a recording, on the thread that records (with `share`: a frame at a new size runs the same frame):
    // nothing is submitted - the images' first layout transitions are recorded by the first recording - and anything
    // that would have to submit throws (Model Resolution below 100%), for the caller to build in the background.
    bool deferred = false;
    // The network memory's room, by area against this extent's own need (>= 1): plans of bigger working extents run
    // in it (Runtime::with_slot_of) up to that.
    double capacity = 1.0;
    // A whole build: another network's activation arena (Runtime::act_arena) to run in instead of allocating one, when
    // it has the room. The arena holds one frame's values only; a runtime records a zero fill before it runs in an
    // arena another one ran in last, so networks taking turns in it are exact, each turn costing a fill (the caller
    // keeps networks that run every frame apart). The build does not touch a taken arena (it may be in use); like two
    // runtimes of one network, its users record on one queue. `act_slack` (>= 1): the room a newly allocated one gets
    // beyond this build's need, for other networks to fit.
    std::shared_ptr<ActArenaRef> act_arena;
    double act_slack = 1.0;
    // Able to run Controls::reuse_every: allocates the stored edit and the motion accumulator and turns the
    // direct-sampling and direct-store paths off (a frame that skips the network has no post block to store
    // the answer, so the answer must exist as an image). Needs the temporal path (TemporalConfig::enable).
    // Off, the runtime is exactly what it was.
    bool reuse = false;
};

// SDR encoded RGB, source-sized and upright. Accepts RGBA32F and 8-bit RGBA/BGRA
// UNORM or SRGB. For SRGB images, copy the encoded bits through a private UNORM
// transfer image; do not apply a transfer function or require mutable game views.
// Image must have TRANSFER_SRC|TRANSFER_DST usage, one colour mip/layer/sample,
// and belong to the host queue family. Format must match RuntimeConfig.
struct ColourFrame {
    VkImage image{};
    VkFormat format = VK_FORMAT_R32G32B32A32_SFLOAT;
    uint32_t width{}, height{};
    VkImageLayout before = VK_IMAGE_LAYOUT_GENERAL;
    VkImageLayout after = VK_IMAGE_LAYOUT_GENERAL;
    VkPipelineStageFlags before_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkAccessFlags before_access = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    VkPipelineStageFlags after_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkAccessFlags after_access = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    // What the image may be used for, when the caller knows. Zero = unknown.
    // With VK_IMAGE_USAGE_SAMPLED_BIT the engine path samples the colour and
    // depth in place instead of copying them (same values, no copy).
    VkImageUsageFlags usage = 0;
};

// Explicit engine/integration-provided mask. Linear RGBA32F, TRANSFER_SRC,
// one mip/layer/sample, host queue family. Preserve its image/layout/contents.
// R masks the NR1 edit before application strength; G/B condition tone/structure.
// Alpha unused. Subrect width/height zero select the full texture independently.
// This does not acquire a mask from a game or generate a character segmentation.
struct ControlMaskConfig {
    // Both zero disable the optional allocation/pipelines. Size changes require
    // reconstruction, independently of colour extent. Existing RuntimeConfig ABI
    // remains intact; callers opt in through the separate constructor overload.
    uint32_t width{}, height{};
};

struct ControlMaskFrame {
    ColourFrame texture;
    uint32_t x{}, y{}, width{}, height{};
};

// Opt-in temporal path. Allocates the fallback motion estimator's luma
// pyramids and flow field, plus the history image, and selects the temporal
// pre/post pipeline variants. With `enable` false nothing is allocated and the
// runtime records exactly what it records today.
//
// This is the consumer side of the motion provider architecture. The
// estimator is the *fallback* provider: a
// game that exposes engine motion vectors must use those instead, and when that
// path exists it fills the same motion texture rather than replacing any of
// this. An estimate never reports itself as engine data.
struct TemporalConfig {
    bool enable = false;
    // The original's post block blends the reprojected previous output into the
    // current one under a weight the network emits, scaled by a device scalar
    // (its parameter block's +104, clamped to [0, 1]): the model's blend_scale,
    // 0.7397. This multiplies it; 1 is the original. 0 disables the output-side
    // blend and leaves only the pre block's history features.
    float history_strength = 1.0f;
    // Variant SPIR-V directory; empty selects ROOT/build/linux/rdna4/network/temporal.
    // These are variants, not production binaries: the temporal build
    // verifies the shipping pre/post SPIR-V are unchanged when it builds them.
    std::string shaders;
};

// Per-frame temporal state. `reset` is `DLSSNR.Reset`, one of the three terms
// of the original's gate: it suppresses history
// for this frame only. The other two terms - the first-frame latch and whether
// a motion source exists at all - the runtime knows and resolves itself.
struct TemporalFrame {
    bool reset = false;
    // Which feature's temporal history this frame belongs to; see
    // Runtime::release_feature. Zero is the runtime's own single history, which
    // is what every caller that does not know about features gets.
    uint64_t feature = 0;
};

struct RecordResult {
    uint32_t network_dispatches{};
    bool applied{};
    bool reused{};   // the answer came from the stored edit; the network did not run
    bool reuse_gated{};   // reuse was asked for and the motion was too large: the network ran
    float effective_skin{}, effective_background{};

};

struct ControlMaskResult {
    RecordResult frame;
    bool consumed{};
};

struct TemporalResult {
    RecordResult frame;
    // The original's gate, as it resolved for this frame: the first-frame latch
    // was set, `reset` was clear, and a motion source was available. False means
    // the history features held the current colour, which is what the original
    // does in the same situation - not a degraded mode of our own.
    bool history_consumed{};
    // Provenance, for the UI. "none", "estimated" or - once the upscaler
    // interception exists - "engine". Never report an estimate as engine data.
    const char* motion_provider = "none";
};

// One frame's worth of engine-provided resources, in the caller's own Vulkan
// device. This is the *engine* motion provider, and
// the point of the whole provider split: the consumer below is identical to the
// fallback's, so neural rendering runs on real motion vectors when a game has
// them and on an estimate when it does not, with nothing else changing.
//
// Colour is the pre-upscale, render-resolution image an upscaler is about to
// consume. Running here is what "pre-upscale neural rendering" means: the
// network sees the frame before the upscaler and before the UI, which is where
// the original runs and is not where a Present-time adapter can reach.
struct EngineFrame {
    ColourFrame colour;        // render-resolution, modified in place
    // Backward motion vectors. Any 2-channel format; `motion_scale` converts
    // the texture's units into normalized screen units, exactly as the
    // original's MVecScaleX/Y do.
    ColourFrame motion;
    float motion_scale_x = 1.0f, motion_scale_y = 1.0f;
    // The motion texture's whole allocation when `motion.width/height` name a
    // region of it (a subrect); zero: the same extent.
    uint32_t motion_texture_width = 0, motion_texture_height = 0;
    // Where the motion and depth regions start in their textures (the subrect
    // bases, in texels); zero: the origin. `width/height` are the regions'.
    uint32_t motion_x = 0, motion_y = 0, depth_x = 0, depth_y = 0;
    // The engine's depth buffer, at render resolution. Optional, and it is never
    // a feature of its own: the original samples depth at the centre and four
    // diagonals and reads the motion vector at whichever of the five is nearest
    // the camera (the pre PTX at :339-378). On a
    // silhouette that is what stops a background vector being used for a
    // foreground pixel. Without it the motion vector is read at the pixel
    // itself, which is what the original does when the game passes no depth.
    ColourFrame depth;
    // Reversed-Z, which is what most modern engines use: nearest is the largest
    // value rather than the smallest. Getting this backwards picks the furthest
    // of the five taps and makes silhouettes worse than no depth at all.
    bool depth_inverted = false;
    // DLSSNR.Reset: suppresses history for this frame only.
    bool reset = false;
    // As TemporalFrame::feature: the independent temporal state this frame
    // belongs to. A host that keeps several NGX features alive on one network -
    // OptiScaler runs one per pass - gives each its own id, and each then has
    // its own history, latch and parity exactly as it would with NVIDIA's DLL,
    // where a feature IS the object that owns them.
    uint64_t feature = 0;
    // where the answer goes when it is not `colour` itself (same format and
    // extent, one pass, the model applied). `colour` is then only read, and the
    // host's seed copy of the colour into the target is not needed.
    ColourFrame target;
};

struct EngineResult {
    RecordResult frame;
    bool history_consumed{};
    const char* motion_provider = "engine";
};

// Diagnostics, off by default: every ~600 recordings of an engine frame, read back what the
// network is handed (colour, the game's motion vectors, depth) and what it hands back, score it
// on a background thread and log the result; the first `pictures` captures also as PNGs in
// `folder`. See nr_input_check.hpp.
void set_input_check(bool on, const std::string& folder, int pictures = 4);

std::shared_ptr<ImageHeapRef> allocate_image_heap(const HostDevice& host, uint64_t region_bytes);
// Whether a runtime built inside a frame from `core` at width x height takes its images from the heap of the memory
// it runs in (else memory of its own).
bool image_heap_fits(const RuntimeCore& core, uint32_t width, uint32_t height);

class Runtime {
public:
    // Caller enables the same device features/extensions as nrvk::Context::create.
    // Initialization uploads weights on host.queue; serialize it with host submits.
    Runtime(const HostDevice&, const RuntimeConfig&);
    Runtime(const HostDevice&, const RuntimeConfig&, const ControlMaskConfig&);
    Runtime(const HostDevice&, const RuntimeConfig&, const ControlMaskConfig&, const TemporalConfig&);
    ~Runtime();
    bool recordings_done() const;
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    // Records only: no submit, wait, readback, image resizing or host allocation.
    // Caller supplies a recording command buffer on queue_family, synchronizes
    // incoming work and serializes uses of this runtime's scratch arena.
    // Changes to controls affect this recording, including intensity 0/1/2.
    // Wait for all uses before destruction. The mask overload consumes explicit
    // caller resources; neither overload acquires game guides or supplies temporal
    // history, depth/motion integration or a Present hook by itself.
    RecordResult record(VkCommandBuffer, const ColourFrame&, const Controls&);
    // The same, for a frame the caller has a Reset flag for (DLSSNR.Reset): with Controls::noise_every_frame the
    // seed starts again from 0 on it, as the DLL's counter does.
    RecordResult record(VkCommandBuffer, const ColourFrame&, const Controls&, bool reset);
    ControlMaskResult record(VkCommandBuffer, const ColourFrame&, const Controls&, const ControlMaskFrame*);
    // Named rather than overloaded: `record(cmd, frame, controls, nullptr)` must
    // keep meaning what it already means to every existing caller.
    // Requires a runtime built with TemporalConfig::enable.
    TemporalResult record_temporal(VkCommandBuffer, const ColourFrame&, const Controls&,
                                   const TemporalFrame&);
    // The engine provider: real motion vectors, on the pre-upscale colour image.
    // Requires a runtime built with TemporalConfig::enable, whose extent matches
    // the render resolution rather than the display one.
    EngineResult record_engine(VkCommandBuffer, const EngineFrame&, const Controls&);
    // whether record_engine can take EngineFrame::target with these controls
    // (the model applied, one pass). A host asks before it skips its own seed copy.
    bool takes_target(const Controls&) const;
    // Drop a feature's temporal state (EngineFrame::feature). The images are not
    // freed here: the caller's last use of them may still be executing and this
    // is called from a render thread, so they are retired and destroyed a fixed
    // number of later recordings on, which is the same reasoning as OptiScaler's
    // own 32-evaluate parking of a retired feature. Never blocks.
    void release_feature(uint64_t feature);
    // Every feature's next frame runs as a new feature's (no history read, the noise seed from 0), as after a
    // Reset. For a network that served nothing for a while and is taken up again (Session::select_runtime).
    void forget_history();
    // How many features currently hold temporal state, for the log.
    uint32_t live_features() const;

    // The history image, after the last recorded frame completed: the blended
    // image the model wrote, alpha = the blend weight it asked for. For tests
    // and diagnostics; `image` is null when the runtime has no temporal path.
    // The image belongs to the runtime and stays in `layout` for its whole
    // life; a caller may copy from it and must not destroy it.
    struct TemporalHistoryView {
        VkImage image{}; VkImageView view{}; uint32_t width{}, height{};
        VkFormat format{}; VkImageLayout layout{};
    };
    TemporalHistoryView temporal_history() const;
    // The extent the network runs at (the frame's).
    uint32_t model_width() const;
    uint32_t model_height() const;
    // The model this runtime runs (RuntimeConfig::network resolved).
    NetworkModel network() const;
    // Whether it runs CC_SMALL_FP8_EXPERIMENTAL (network() is then NetworkModel::Small).
    bool experimental() const;
    // What a runtime at width x height (the frame's extent, as RuntimeConfig::width/height) with this one's
    // configuration otherwise can take over from this one (RuntimeConfig::share): null when its working extent
    // differs, or the frame is taller than this one's network was built for.
    std::shared_ptr<const RuntimeCore> share_for(uint32_t width, uint32_t height) const;
    // The network this runtime runs on (its arenas): the same for runtimes built from one another's (share_for).
    const void* network_id() const;
    // Its activation arena (shared with other networks when built with RuntimeConfig::act_arena), and its identity.
    std::shared_ptr<ActArenaRef> act_arena() const;
    const void* act_id() const;
    // Why it runs another model or network than RuntimeConfig asked for (the small model's file missing, int4 mixed
    // without what it needs); empty when it runs what was asked.
    const std::string& network_note() const;
    // [Network] Motion Without Depth as this runtime read it at construction (true: the depth is not read).
    bool motion_without_depth() const;
    // A runtime built inside a recording (RuntimeConfig::deferred) whose images another one has taken over since:
    // never recorded again; drop it.
    bool stale() const;
    // The image heap the runtimes built inside frames in this runtime's network memory take turns in: the region size
    // still wanted (0: it has one big enough, or nothing to size it from). allocate_image_heap makes one (any thread:
    // it is a long allocation), adopt_image_heap hands it over on the thread that records.
    uint64_t image_heap_wanted() const;
    void adopt_image_heap(const std::shared_ptr<ImageHeapRef>& heap);
    // Built inside a frame without the heap: its images took memory of their own.
    bool owns_image_memory() const;
    // The resize path's option ([Network] Resize History = 1, off by default: NVIDIA starts a feature's history afresh
    // at a new size): the history `feature` holds in `from` (a runtime at the size before), scaled bilinearly into the
    // one this runtime's next recording of the feature reads, which then consumes it as its own - the noise counter
    // goes on too. Recorded into `cmd` before that recording. False, nothing recorded: one model pass only, and
    // `from` must hold a history for the feature.
    bool carry_history(VkCommandBuffer cmd, const Runtime& from, uint64_t feature);
    // A runtime built with RuntimeConfig::plan_only: its lowering, for another runtime's network memory
    // (with_slot_of). Any runtime: its own lowering.
    std::shared_ptr<const RuntimeCore> plan() const;
    // `plan` running in this runtime's network memory (RuntimeConfig::share): null when the memory is too small for it,
    // or its weights or build arguments differ. The memory is made ready for it in the recording (zeroed arena, the
    // plan's records and noise field), whenever another plan used it last.
    std::shared_ptr<const RuntimeCore> with_slot_of(const std::shared_ptr<const RuntimeCore>& plan) const;

#if NR_INT4
    // Whether this runtime runs the int4 mixed network (asked for and everything it needs present).
    bool int4_mixed() const;
#endif
    // Change the post block's history strength (TemporalConfig::history_strength)
    // between frames. Takes effect on the next recording; no-op without a
    // temporal path.
    void set_history_strength(float);
    // Natural and Cinematic graded after the post block as NVIDIA's DLL does (true, the default), or not (false: the
    // picture these styles had before the grade was ported). Per frame; nothing is rebuilt.
    void set_style_grade(bool);

    // **What the pass costs on the GPU**, from a timestamp pair around the work
    // this runtime records - everything between the first barrier and the last,
    // which is the number a player comparing it against their frame time wants.
    // It is not the frame time and not a CPU measurement: the recording is
    // submitted inside somebody else's command buffer on two of the three
    // routes, so wall-clock around the call measures the caller, not us.
    //
    // Read from a small ring several recordings behind, never waiting, so
    // asking costs nothing and a result that is not back yet leaves the last one
    // standing. Zero until the first pair comes back, and zero for the whole
    // life of a device whose queue family reports no timestamp bits.
    float last_gpu_ms() const;
    // The same, smoothed over recent frames - what to put in a UI, because the
    // instantaneous number moves too much to read.
    float average_gpu_ms() const;
    // The network's share of average_gpu_ms (every pass's dispatches), the same average; the rest is
    // the work around it (input, composition, copies). Zero until the first result is back.
    float average_network_ms() const;
    // The recordings since the timing ring was last read may not all have reached the GPU (a submit of the command
    // buffer they were in failed: their queries were never reset). No slot is read until the ring has gone round again.
    void discard_timing();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    // One body behind the three entry points, so the mask and temporal paths
    // cannot drift apart in barriers or ordering.
    ControlMaskResult record_all(VkCommandBuffer, const ColourFrame&, const Controls&,
                                 const ControlMaskFrame*, const TemporalFrame*, bool*,
                                 const EngineFrame*);
};
}  // namespace nr
