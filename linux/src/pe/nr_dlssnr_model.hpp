#pragma once
// The DLSS-NR model, once, for both of the DLLs that publish it.
//
// Two different OptiScaler lineages reach feature 18 by two different doors and neither is going
// away:
//
//   nvngx.dll_dlssnr.dll   Dagherbou's OptiScaler_DLSSNR (and the v0.2.0 release) loads this beside
//                          itself and calls dlssnr_call_* / dlssnr_vk_* -- linux/src/pe/nr_dlssnr_forwarder.cpp
//   _nvngx.dll             wilsjo2's fork from 0.8.1 on loads NO forwarder at all. It takes the NGX
//                          core's capability block, sets DLSSNR.* on it, and calls
//                          NVSDK_NGX_D3D12_CreateFeature(cmd, 18, params, &handle) -- linux/src/pe/nr_ngx_core.cpp
//
// Behind both doors the work is identical: one nr::pe::Session per graphics API, a Feature handle
// carrying the extent and the six controls, a seed copy of colour into output, and Session::run_after
// (D3D12) or Session::run_vulkan. That work lives here so the two entry points cannot drift -- the
// forwarder was the original and this file is its body, lifted out unchanged.
//
// The parameter block is written through its vtable in both cases, even from _nvngx.dll where the
// block is our own class: the write is what OptiScaler reads back to report what landed, and one code
// path for it is one thing that can be wrong. Reading is different -- see nr_ngx_core.cpp, which reads
// its own map directly.

#include "nr_ngx_abi.hpp"

#include <functional>

struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;

namespace nr::pe { class Session; }
namespace nr { struct Preprocess; }

namespace nr::dlssnr {

// The six controls the NGX surface carries, in the order every caller passes them. Defaults are
// nr::Controls's own, so a caller that reads nothing from the block still gets a sane model.
struct Controls6 {
    float intensity = 1.0f;
    int style = 0;
    float local_structure = 1.0f;
    float local_tone = 1.0f;
    float skin_structure = -1.0f;
    int use_auto_mask = 1;
    int noise_every_frame = 0;   // ours (a wilsjo2 layer's NoiseEveryFrame): nr::Controls::noise_every_frame
    // ours: the highlight curve the colour was encoded with (a wilsjo2 layer's ReversibleMode), for Preprocess's undo:
    // nr::Controls::reversible_mode
    int proxy_curve = 0;
    bool model_capture_allowed = true;
};

// A guide subrect: where in the allocation this frame's data is, and how much of it.
// Base and extent are both honoured, as NVIDIA's DLL does -- nr_pe_session.cpp apply_guide_subrect.
struct Rect {
    unsigned int x = 0, y = 0, width = 0, height = 0;
};

// One live feature. Opaque: a handle to the caller, a struct to this file.
struct Feature;

// ---------------------------------------------------------------------------------------------
// The caller's parameter block.
// ---------------------------------------------------------------------------------------------

// Which vtable slot floats live in. Defaults to MSVC's 6 (nr_ngx_abi.hpp), which is both what our own
// block publishes and where NVIDIA's driver block keeps them.
void set_float_slot(int slot);
// Write a float through an arbitrary slot, so a host can find the right one by probing.
void probe_float(void* params, const char* name, float value, int slot);

// What the reference writes at create, and again at every evaluate. Nothing here reads these back:
// the controls arrive as arguments. They are written because OptiScaler reads them back to report
// what landed, and a block answering 0xBAD00000 to all of them looks like a broken integration.
void write_create_keys(void* params, unsigned int width, unsigned int height, int preset,
                       int ui_correction, const Controls6& controls);
void write_evaluate_keys(void* params, const Controls6& controls, bool reset, int depth_inverted);

// Why the last create or evaluate declined, in words; empty when nothing has.
const char* last_error();

// ---------------------------------------------------------------------------------------------
// Direct3D 12, through vkd3d-proton.
// ---------------------------------------------------------------------------------------------

// Returns null on failure, and writes the NGX result code into *out_result either way.
Feature* create_d3d12(ID3D12Device* device, ID3D12GraphicsCommandList* cmd, void* params,
                      unsigned int width, unsigned int height, int preset, int ui_correction,
                      const Controls6& controls, int* out_result);

// NGX result codes, except 0 for "bad arguments", which is what the reference forwarder answers and
// what the forwarder ABI's callers test for.
int evaluate_d3d12(ID3D12GraphicsCommandList* cmd, Feature* feature, void* params,
                   ID3D12Resource* color, ID3D12Resource* depth, ID3D12Resource* motion,
                   ID3D12Resource* output, unsigned int width, unsigned int height,
                   const Rect& depth_rect, const Rect& motion_rect, int depth_inverted, int reset,
                   const Controls6& controls, float mv_scale_x, float mv_scale_y);

void release_d3d12(Feature* feature);

// The frame's keys, once a frame: [RenoDX] NRToggleKey (NR on / off for this run) and the screenshot keys. True while NR
// is on. The OptiScaler route's evaluate does it itself; the .asi's NR chain calls it once before its layers.
bool frame_keys();
// NR on / off as the last frame_keys left it (no keys read): for the .asi's Present layers.
bool nr_on_now();

// The .asi route (nr_asi_main.cpp, upstream OptiScaler): the D3D12 session keeps the caller's command-list state
// across the network's recording (Session::set_keep_caller_state). Before the first create.
void keep_caller_state();

// The .asi's wilsjo2 style (nr_asi_wilsjo2.cpp): its own passes around the model's evaluate are recorded through the
// D3D12 session (Session::record_raw), under the lock every evaluate holds.
void with_d3d12_session(const std::function<void(nr::pe::Session&)>& fn);
// [Preprocess] as this frame has it (dlssnr-amd.ini, re-read at most once a second, and its hotkey) - for a wilsjo2
// layer that runs it itself (it then sets Wilsjo2Facts::prep_outside, and the network's runtime does not run it).
nr::Preprocess preprocess_settings();

// ---------------------------------------------------------------------------------------------
// Vulkan.
// ---------------------------------------------------------------------------------------------

// Learns the device and looks for a queue to upload the weights on. Returns an NGX result code;
// FAIL_UnableToInitializeFeature means no queue is known, which is the documented decline.
int vk_init(void* instance, void* physical_device, void* device);
// After vk_init: the lock every submit of the Vulkan session takes on its queue (the .asi's D3D11 route: DXVK's
// submission-queue lock), in place of the per-queue one.
void vk_queue_lock(const std::function<void()>& lock, const std::function<void()>& unlock);
// The game destroys the Vulkan device the session runs on (the .asi's Vulkan route, from its vkDestroyDevice detour,
// before the real call; every feature of the session already released): the session goes with everything it made
// while the device is still there, and a later vk_init may start one on another device.
void vk_device_destroyed();
// The process is ending (DLL_PROCESS_DETACH with a non-null reserved): its other threads are gone, perhaps inside
// vkd3d-proton's / DXVK's locks, and the modules loaded after this one (d3d12, winevulkan) are detached already. The
// sessions are left as they are (their totals logged), not destroyed: a destructor's GPU wait or queue lock could
// wait for ever, and the game would never end.
void abandon_at_exit();
// Whether vk_init has succeeded, i.e. whether a create can be attempted at all.
bool vk_ready();

Feature* create_vk(void* cmd_buffer, void* params, unsigned int width, unsigned int height,
                   int preset, int ui_correction, const Controls6& controls, int* out_result);

// The resources are NVSDK_NGX_Resource_VK*; buffers are declined rather than reinterpreted.
int evaluate_vk(void* cmd_buffer, Feature* feature, void* params, const void* color,
                const void* depth, const void* motion, const void* output, unsigned int width,
                unsigned int height, const Rect& depth_rect, const Rect& motion_rect,
                int depth_inverted, int reset, const Controls6& controls, float mv_scale_x,
                float mv_scale_y);

void release_vk(Feature* feature);

// The .asi on a Vulkan game (nr_asi_main.cpp, the exported NVSDK_NGX_VULKAN_EvaluateFeature): an image as the caller
// holds it during the call. `image` / `format` are VkImage / VkFormat, `layout` / `usage` VkImageLayout /
// VkImageUsageFlags (the hpp names no Vulkan type).
struct VkImageRef {
    uint64_t image = 0;
    uint32_t format = 0, width = 0, height = 0;
    uint32_t layout = 1;   // VK_IMAGE_LAYOUT_GENERAL
    uint32_t usage = 0;
};
// A feature for the .asi's Vulkan route (the session's queue as vk_init found it): as create_vk, without NGX keys.
Feature* create_vk_asi(unsigned int width, unsigned int height, int* out_result);
// One run of NR recorded into the game's command buffer `cmd`: `colour` read, `output` written (the same image: in
// place, in its layout). The RenoDX style when renodx_route says so for this call (CallStyle, begin_run, note_encode),
// else the network alone, as NVIDIA's DLL composes (the wilsjo2 style's model pass). `depth` / `motion` with image 0:
// none. An NGX result; a frame NR did not run on is a Success with `output` unchanged when it is `colour`, else seeded
// with the colour.
int evaluate_vk_asi(void* cmd, Feature* feature, const VkImageRef& colour, const VkImageRef& output,
                    const VkImageRef& depth, const VkImageRef& motion, unsigned int width, unsigned int height,
                    const Rect& depth_rect, const Rect& motion_rect, int depth_inverted, int reset,
                    const Controls6& controls, float mv_scale_x, float mv_scale_y, bool* ran_out = nullptr);
// The Vulkan session, for the .asi's wilsjo2 style on Vulkan (its own passes recorded beside the model's), under the
// lock every evaluate holds.
void with_vk_session(const std::function<void(nr::pe::Session&)>& fn);
// A command buffer the Vulkan session recorded into was not submitted: its timing is not read (Session::discard_timing;
// nothing when there is no session yet).
void discard_vk_timing();
// A layer of the .asi's chain changed (processing, placement, network) or went: Session::reset_layer_timing on every
// session there is (none is made for it).
void layer_changed(int layer);

}  // namespace nr::dlssnr
