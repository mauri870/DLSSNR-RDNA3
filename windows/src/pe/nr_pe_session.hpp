#pragma once
// One neural-rendering session bound to a game's D3D12 device, running at the
// game's *render* resolution.
//
// Placement, which is the whole point (docs and the neural-upstream reference):
//
//     internal render res -> NR -> the game's DLSS/FSR upscale -> output
//
// rather than the official `internal -> upscale -> NR at output resolution`.
// The network is same-resolution - it enhances, it does not upscale - so running
// it on the smaller image is what makes it affordable, and the upscaler still
// does the job it was going to do.
//
// The network is SPIR-V and the game is D3D12, which on Proton is not a problem:
// vkd3d-proton hands out the command list's VkCommandBuffer and each resource's
// VkImage, so the pass records into the game's own command stream, in order,
// immediately before the upscale it is feeding.
#include "nr_pe_interop.hpp"
#include "nr_runtime.hpp"
#include "nr_renodx.hpp"
#include "nr_ini_network.hpp"
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>

namespace nr::pe {

class Session {
  public:
    // `root` is the directory holding build/ and artifacts/; empty means "next
    // to this module", the same rule the Vulkan layer uses so that nothing has
    // to be configured through the environment.
    explicit Session(std::string root);
    ~Session();

    // The post block's history strength, 0..1; see Config::history. Applies to
    // the running network at once and to any network built later.
    void set_history_strength(float);
    // The white point for linear-light colour buffers; see Config::white_point.
    void set_white_point(float);
    // Model Resolution, 0.25..1: the network's extent as a fraction of the
    // frame's. Changing it rebuilds the network in the background.
    // The pass ceiling a network is built for. Below what shipped is ignored,
    // so 1..4 never rebuilds; above it the next frame rebuilds.
    void set_max_passes(uint32_t);
    void set_model_scale(float);
    // The DLL's own composition only (RuntimeConfig::native_compose): the host
    // does its resolve after us. Set before the first frame; the OptiScaler
    // route sets it, the legacy module does not.
    void set_native_compose(bool);
    // RuntimeConfig::motion_point: the game's motion vectors point-sampled as NVIDIA's DLL does. Set before the first
    // frame; the NGX (OptiScaler) route sets it.
    void set_motion_point(bool);

    // Run the network on `colour` and return the resource to hand the upscaler
    // in its place. Returns null when the pass could not run, in which case the
    // caller must leave the game's parameters exactly as it found them.
    //
    // `motion` may be null: without it the original does not consume history
    // either, and the pass still runs as a
    // spatial enhancement. `reset` is DLSSNR.Reset for this frame.
    // Everything one call of an upscaler carries that the network can use. A
    // struct rather than a parameter list because the list had reached nine and
    // the next thing to add - depth - would have made it eleven, in an order
    // nobody could keep straight across four call sites.
    // A rectangle of a guide texture that actually holds this frame's data.
    struct Subrect {
        uint32_t x{}, y{}, width{}, height{};
    };

    struct EngineResources {
        // Which NGX feature this frame belongs to; see create_feature. Zero is
        // "the session's own single temporal state", which is what the game
        // module's one-pass hooks use.
        uint64_t feature{};
        ID3D12Resource* colour{};
        D3D12_RESOURCE_STATES colour_state{D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
        ID3D12Resource* motion{};
        D3D12_RESOURCE_STATES motion_state{D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
        // Optional. Depth never becomes a feature; it only chooses where the
        // motion vector is sampled, which is what keeps a background vector off
        // a foreground pixel at a silhouette.
        ID3D12Resource* depth{};
        D3D12_RESOURCE_STATES depth_state{D3D12_RESOURCE_STATE_DEPTH_READ};
        bool depth_inverted{true};   // reversed-Z, which is what modern engines use
        // How much of the depth and motion textures the game actually rendered into, and where.
        //
        // Not the same thing as how big they are. A game with dynamic resolution allocates its guides
        // once at the largest size it will ever need and renders into a corner of them; sizing the
        // guide from the resource hands the network the stale margin as well and calls it scene.
        // Zero width or height means "the caller did not say", and the allocation is used.
        //
        // Base and extent are both honoured (apply_guide_subrect in nr_pe_session.cpp).
        Subrect depth_subrect{};
        Subrect motion_subrect{};
        float motion_scale_x{1.0f}, motion_scale_y{1.0f};
        bool reset{};
        // The colour has already been encoded by something upstream, so a float
        // format here is NOT scene-referred linear light and must not be encoded
        // a second time. OptiScaler's DLSS-NR pass tone-maps its proxy before it
        // ever reaches a model, so its path sets this; other callers leave it
        // false and keep judging by format alone.
        bool colour_encoded{};
    };

    ID3D12Resource* run(ID3D12Device* device, ID3D12GraphicsCommandList* list,
                        const EngineResources& resources, const Controls& controls);

    // The post-upscale placement: the upscaler has already written its output at
    // display resolution and the network runs over that, in place. This is where
    // the official pipeline puts it, and it is the more expensive of the two -
    // the network's cost tracks the resolution it runs at. Engine motion vectors
    // still come from the same call, at render resolution; the shader samples
    // them by normalized coordinate, so the resolutions need not match.
    // `input` (optional): read the colour from there and write the answer into
    // `output`, which then needs no seed copy of it. Returns false without
    // recording anything when that is not possible (the caller seeds and runs in
    // place instead): different formats or extents, or the model not applied.
    bool run_after(ID3D12Device* device, ID3D12GraphicsCommandList* list,
                   ID3D12Resource* output, D3D12_RESOURCE_STATES output_state,
                   const EngineResources& resources, const Controls& controls,
                   ID3D12Resource* input = nullptr, D3D12_RESOURCE_STATES input_state = D3D12_RESOURCE_STATE_COMMON);

    // The RenoDX-style option on the OptiScaler route ([RenoDX], nr_renodx.hpp): `input` (the frame as
    // OptiScaler hands it over, see nr_pe_optifix.hpp renodx_route) goes through RenoDX's processing around
    // the network into `output`. One stage per feature (each holds RenoDX's per-stream state: the autoscale
    // divisor, the Detail stability history). Returns false without recording anything while the network
    // builds or when the frame cannot be taken (the caller then copies the input over, which OptiScaler
    // outputs as it is).
    struct RenoDXFrame {
        bool hdr_flag = false;        // the game's colour is HDR (OptiScaler's own judgement)
        bool after_upscale = true;    // NR runs on the upscaler's output
        renodx::Settings settings{};
        // The game's exposure as DLSS was given it (OptiScaler route only): the exposure texture and its state
        // during the evaluation, DLSS.Pre.Exposure, DLSS.Exposure.Scale, the AutoExposure create flag.
        ID3D12Resource* exposure = nullptr;
        D3D12_RESOURCE_STATES exposure_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        float pre_exposure = 1.0f, exposure_scale = 1.0f;
        bool auto_exposure = false, create_flags_known = true;
        // OptiScaler's Vulkan path: the game's exposure image, in GENERAL (no image: none).
        VkImage vk_exposure = VK_NULL_HANDLE;
        VkFormat vk_exposure_format = VK_FORMAT_UNDEFINED;
        uint32_t vk_exposure_width = 0, vk_exposure_height = 0;
        Preprocess preprocess{};      // [Preprocess] this frame (it runs inside RenoDX's processing, not the network's)
        // The ReShade routes are RenoDX's Present hook point: the encoding (1 SDR, 2 HDR10, 3 scRGB; 0 = the OptiScaler
        // route's DLSS-output rules) and the HDR diffuse white in nits.
        uint32_t present = 0;
        float present_white_nits = 203.0f;
        // OptiScaler's finished picture (its present path; with the option on only for RenoDX's NR screenshot), and
        // whether this frame carries no NR from an inline evaluate - the final picture a screenshot may take.
        bool finished = false;
        bool clean = true;
    };
    bool run_renodx(ID3D12Device* device, ID3D12GraphicsCommandList* list,
                    ID3D12Resource* output, D3D12_RESOURCE_STATES output_state,
                    const EngineResources& resources, const Controls& controls,
                    ID3D12Resource* input, D3D12_RESOURCE_STATES input_state, const RenoDXFrame& frame);

    // Is this D3D12 device the native runtime (the network then runs on a Vulkan device of its own,
    // nr_pe_bridge.hpp) rather than vkd3d-proton? Decided on the first device seen; NR_BRIDGE=1 takes
    // the bridge under vkd3d-proton too.
    bool bridged(ID3D12Device* device);

    // A D3D12 frame handed over at queue level rather than inside a command list: the ReShade route,
    // whose effect list is ReShade's own immediate list and can be flushed. In place on `target`.
    // Only for a bridged device.
    struct D3D12QueueFrame {
        ID3D12Device* device{};
        ID3D12CommandQueue* queue{};
        // Hands everything recorded so far to `queue` (ReShade: flush_immediate_command_list).
        std::function<void()> flush;
        ID3D12Resource* target{};
        D3D12_RESOURCE_STATES target_state{D3D12_RESOURCE_STATE_RENDER_TARGET};
        ID3D12Resource* motion{};
        D3D12_RESOURCE_STATES motion_state{D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE};
        ID3D12Resource* depth{};
        D3D12_RESOURCE_STATES depth_state{D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE};
        bool depth_inverted{true};
        float motion_scale_x{1.0f}, motion_scale_y{1.0f};
        bool reset{};
        bool linear_hdr{};   // as D3D11Frame::linear_hdr
        // [RenoDX] Enabled at start (D3D11Frame::renodx).
        const RenoDXFrame* renodx{};
    };
    bool run_d3d12_queue(const D3D12QueueFrame& frame, const Controls& controls);

    // D3D11, through DXVK. There is no command list to record into, so this one
    // owns a command buffer, records into it, and submits it on DXVK's own queue
    // between a flush and the queue lock - the sequence DXVK documents for
    // exactly this.
    //
    // The result goes back into `target` itself rather than into an image of our
    // own. D3D11 offers no way to hand the upscaler a different texture - there
    // is no parameter to rebind and no way to wrap a VkImage as an
    // ID3D11Texture2D - so an out-of-place pass here would run, cost its
    // milliseconds, and be read by nobody. In place is what makes this path real.
    // The game keeps its handle; the pixels behind it change.
    struct D3D11Frame {
        IUnknown* device{};
        IUnknown* target{};   // read and written; the upscaler's input, or its output
        IUnknown* motion{};   // engine motion vectors; null runs the spatial pass
        IUnknown* depth{};    // optional; chooses where the motion vector is read
        bool depth_inverted{true};
        float motion_scale_x{1.0f}, motion_scale_y{1.0f};
        bool reset{};
        // No engine data at all - the Present fallback. Runs the estimator's
        // temporal path instead, and must never be reported as engine data.
        bool estimate_motion{};
        // `target` is an upscaler's input: scene-referred linear light when its
        // format is a float one, and then the runtime encodes it. A finished
        // back buffer - the fallback's, or ReShade's - is never that, whatever
        // its format, so this stays false there.
        bool upscaler_input{};
        // ... unless the swapchain says otherwise. An HDR back buffer IS linear
        // light (scRGB), and the caller is the only one who can know: the format
        // cannot tell an SDR float buffer from an HDR one, and ReShade can
        // (`swapchain::get_color_space`). Set it and the runtime encodes the
        // frame for the model exactly as it does an upscaler's input.
        bool linear_hdr{};
        // The RenoDX-style option: RenoDX's processing around the network instead (needs `motion`).
        const RenoDXFrame* renodx{};
    };
    bool run_d3d11(const D3D11Frame& frame, const Controls& controls);
    // The same for a D3D10 game on the native runtime (`device` an ID3D10Device), through the bridge
    // with keyed mutexes; D3D10 has neither fences nor compute shaders, so depth is not read.
    bool run_d3d10(const D3D11Frame& frame, const Controls& controls);

    // The fallback: no upscaler, so no engine data. The colour is the finished
    // back buffer - already upscaled, already carrying the game's UI - and the
    // motion vectors come from the estimator rather than the engine. Strictly
    // worse than the upscaler hook, and the only thing such a game offers.
    // Returns true when the pass was recorded into the list.
    bool run_present(ID3D12Device* device, ID3D12GraphicsCommandList* list,
                     ID3D12Resource* back_buffer, unsigned width, unsigned height,
                     const Controls& controls);

    // The same pass for a Vulkan game, where the upscaler hands over a
    // VkCommandBuffer and VkImages directly and there is no D3D12 in the way.
    // The device handles come from the upscaler's own context creation.
    struct VulkanFrame {
        uint64_t feature{};   // as EngineResources::feature
        VkImage colour{}; VkImageLayout colour_layout{VK_IMAGE_LAYOUT_UNDEFINED};
        VkFormat colour_format{VK_FORMAT_UNDEFINED};
        uint32_t width{}, height{};
        VkImage motion{}; VkImageLayout motion_layout{VK_IMAGE_LAYOUT_UNDEFINED};
        VkFormat motion_format{VK_FORMAT_UNDEFINED};
        uint32_t motion_width{}, motion_height{};
        VkImage depth{}; VkImageLayout depth_layout{VK_IMAGE_LAYOUT_UNDEFINED};
        VkFormat depth_format{VK_FORMAT_UNDEFINED};
        uint32_t depth_width{}, depth_height{};
        // The guides' usage as far as the caller knows it (0: not known). A sampled motion-vector image is
        // read in place by the network, as the DLL reads it; without SAMPLED it is blitted into a field of ours.
        VkImageUsageFlags motion_usage{}, depth_usage{};
        // As EngineResources: the part of each guide the game rendered into. The *_width/_height above
        // stay the allocation, so both numbers are available.
        Subrect depth_subrect{};
        Subrect motion_subrect{};
        bool depth_inverted{true};
        float motion_scale_x{1.0f}, motion_scale_y{1.0f};
        bool reset{};
        bool upscaler_input{};   // as D3D11Frame::upscaler_input
        bool linear_hdr{};       // as D3D11Frame::linear_hdr
        // The caller's output (optional): the network reads `colour` in place and writes
        // here, and run_vulkan returns it - no copy into an image of ours and back.
        // `output_usage` / `colour_usage` as far as the caller's descriptors say.
        VkImage output{}; VkImageLayout output_layout{VK_IMAGE_LAYOUT_GENERAL};
        VkImageUsageFlags output_usage{}, colour_usage{};
        // The answer back into `colour` itself (copied out and back by the runtime, so `colour`
        // needs transfer source and destination usage, and is left in colour_layout); run_vulkan
        // then returns `colour`. Otherwise the answer is in an image of ours.
        bool in_place{};
        // The RenoDX-style option (needs `motion`): RenoDX's processing around the network, from `colour` into
        // `output` (or back into `colour` when there is no output or in_place is set).
        const RenoDXFrame* renodx{};
    };
    // A Vulkan game's own queue. The two D3D runtimes have an interop object to
    // ask for one; a Vulkan game has nothing of the sort, so the queue is learned
    // by watching the game create it. Without this the network cannot upload its
    // weights and the Vulkan path declines with that reason.
    void set_vulkan_queue(VkQueue queue, uint32_t family);

    // Returns the image to hand the upscaler in place of the game's colour, or
    // null when the pass could not run. With VulkanFrame::output set it is that
    // image when the answer went straight into it.
    VkImage run_vulkan(const DeviceHandles& handles, VkCommandBuffer cmd,
                       const VulkanFrame& frame, const Controls& controls);
    // The universal fallback: the frame exactly as it is about to be shown, in
    // whatever API the game used - under Proton all of them arrive here as a
    // Vulkan swapchain image. No engine data, so the estimator supplies motion
    // and the result is never reported as the engine's.
    //
    // Ordering is the whole difficulty and it is handled here: the pass waits on
    // the semaphores the game's present was going to wait on, and signals one of
    // ours for the present to wait on instead. `signalled` is that semaphore.
    //
    // `slot` is the swapchain image index and `slots` how many there are, and
    // they are not bookkeeping. A binary semaphore may have exactly one pending
    // signal-wait pair at a time, and the present that waits on ours is still
    // outstanding when the next frame arrives - so one semaphore reused every
    // frame is undefined, and shows up as a black screen and then a crash.
    // Everything per-frame here is per image index instead: semaphore, fence and
    // command buffer.
    bool run_present_vulkan(const DeviceHandles& handles, VkQueue queue, uint32_t family,
                            VkImage image, VkFormat format, uint32_t width, uint32_t height,
                            uint32_t slot, uint32_t slots,
                            const VkSemaphore* wait, uint32_t wait_count,
                            VkSemaphore* signalled, bool temporal, const Controls& controls);

    // The view onto the image run_vulkan returns. NGX takes a resource
    // descriptor, not a bare image, so the caller needs both.
    VkImageView vulkan_output_view() const;

    // ---- features ------------------------------------------------------
    //
    // A feature is what NVIDIA's DLL hands back from CreateFeature: an object
    // with its own temporal state, created and destroyed freely while the
    // network behind it stays put. OptiScaler depends on that - one feature per
    // model pass, two extents alive at once, everything torn down and rebuilt
    // whenever a control moves - so this session gives each one an id and each
    // id its own history inside every runtime it is evaluated against.
    //
    // Creating one costs nothing here: the images are allocated by the runtime
    // the first time the feature is actually evaluated, because until then
    // there may be no runtime and no extent to size them from.
    uint64_t create_feature();
    // Never waits. The runtime retires the feature's images and destroys them a
    // fixed number of later recordings on.
    void release_feature(uint64_t feature);

    // Is the network still being built in the background? A caller must treat
    // this as "pass the frame through", NOT as a failure: it is the normal
    // state for the first second of a session, and reporting it as a failure is
    // what makes a host disable the feature for good.
    bool building() const;
    // Has something gone wrong that will not fix itself? This, and only this, is
    // a failure.
    bool failed() const;

    // Wait for any work this session has in flight. Called before anything it
    // may still be reading is destroyed - a retiring swapchain above all, whose
    // images go away while our last submit can still be referencing them.
    void wait_idle();

    // Why the last run declined, for the log and the overlay. Empty while the
    // pass is running normally.
    const std::string& status() const;

    // What the pass costs on the GPU, in milliseconds - the number OptiScaler's
    // own overlay shows for DLSS-NR, measured the same way: a timestamp pair
    // around the work, not a clock around the call. Smoothed; zero until the
    // first result is back, and zero on a queue with no timestamp support.
    // With the RenoDX-style option: its whole stage a frame (every pass and its own work).
    float gpu_ms() const;
    // The network's part of gpu_ms (every pass); gpu_ms - network_ms is the route's own work.
    float network_ms() const;
    // The running network's extent (0 x 0 before one is running).
    void network_size(uint32_t* width, uint32_t* height) const;

    // What a host's UI shows: the network running and the one being built.
    struct State {
        bool running = false;           // a network is serving frames
        int running_net = -1;           // the network serving (nr::NetworkChoice), -1 none
        uint32_t model_w = 0, model_h = 0;
        int want_net = -1;              // [Network] Model: what the switch asks for
        std::string unavailable[nr::kNetworkChoices];   // why each network cannot run in this run ("" it can)
        bool building = false;
        int building_net = -1;
        float building_seconds = 0.0f;
        int stage = 0;                  // nr::g_build_stage while building
        uint32_t pipes_done = 0, pipes_total = 0;
        // For the engine-data panel: size changes of a feature (each restarts its history, or carries it over with
        // [Network] Resize History = 1: `carried`), the RenoDX-style option's last frame, [Network] Motion Without Depth.
        uint64_t resizes = 0, carried = 0;
        bool renodx_known = false;
        renodx::Stage::Facts renodx{};
        bool motion_without_depth = true;
        float prep_ev = NAN;   // the running network's Preprocess auto exposure (smoothed EV), NaN not metered
    };
    State state() const;
    // [Network] Model from the host's UI, as the hotkey does (the switch writes Model to the ini).
    void set_network(int n);
    // Between evaluations, from the host's own clock (with whatever lock its evaluations hold): after a while with no
    // evaluation (NR off), [Network] Residency frees what it would free of an inactive network, the running one included,
    // and networks retired since are freed (their frames are long done).
    void idle();

    // The largest device-local heap as the driver's memory budget sees it, this
    // process's use against its budget, in MB; "" before a device is known. For
    // the periodic log line: the Windows driver refuses allocations at the budget
    // instead of spilling, and the game is the one that crashes when it does.
    std::string vram_line() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// How far a build is (0..1) by its stage, and the stage in words into `text` (the menus' progress bar, the OSD).
inline float build_progress(const Session::State& st, char* text, size_t n) {
    float f = 0.02f;
    const char* what = "starting";
    switch (st.stage) {
        case 1: f = 0.05f; what = "planning"; break;
        case 2: f = 0.20f; what = "reading the weights"; break;
        case 3: f = 0.35f; what = "uploading the weights"; break;
        case 4: f = 0.40f + 0.55f * (st.pipes_total ? float(st.pipes_done) / float(st.pipes_total) : 0.0f);
                what = "creating pipelines"; break;
        case 5: f = 0.97f; what = "finishing"; break;
        default: break;
    }
    if (st.stage == 4) std::snprintf(text, n, "%s %u/%u, %.0f s", what, st.pipes_done, st.pipes_total, st.building_seconds);
    else std::snprintf(text, n, "%s, %.0f s", what, st.building_seconds);
    return f;
}

}  // namespace nr::pe
