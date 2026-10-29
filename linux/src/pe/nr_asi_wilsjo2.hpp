#pragma once
// The wilsjo2 style on the .asi route (nr_asi_main.cpp): the style's per-frame flow
// (nr_wilsjo2_flow.hpp) on the game's command list - the frame encoded into the proxy (nr_wilsjo2.hpp, our programs), the
// model evaluated on it as the forked OptiScaler evaluated it (nr::dlssnr::evaluate_d3d12, the plain
// OptiScaler route's model call), the answer composed back onto the frame. One layer of the NR chain
// (nr_pe_config.hpp LayerConfig, Processing = wilsjo2); its settings are its own section's keys, under the fork's
// [DlssNr] key names.
//
// So far: one pass, working scale 1, no peripheral compression; white point manual (WhitePointSource 0), the game's
// exposure (1, the installer's choice) or the frame's own meter (3, automatic exposure); after or before the upscaler
// (RunBeforeSR).
#include "nr_dlssnr_model.hpp"
#include "nr_pe_config.hpp"
#include "nr_wilsjo2.hpp"

#include <d3d12.h>
#include <vulkan/vulkan.h>

#include <cstdint>

namespace nr::asi {

// What the game's evaluate says of the frame, as the .asi read it.
struct Wilsjo2Frame {
    ID3D12Resource *depth = nullptr, *motion = nullptr, *exposure = nullptr;
    D3D12_RESOURCE_STATES depth_state{}, motion_state{}, exposure_state{};
    uint32_t flags = 0;                 // DLSS.Feature.Create.Flags as known (0 unknown)
    bool hdr = false;                   // ColourIsLinearHdr (OptiScaler fixes 5 and 7)
    bool reset = false;
    float pre_exposure = 1.0f;          // DLSS.Pre.Exposure as given (sanitised by the flow)
    float mv_x = 1.0f, mv_y = 1.0f;     // the game's MV scale (non-finite or 0 -> 1)
    nr::dlssnr::Rect depth_rect, motion_rect;   // the guide regions
};

// One OptiScaler feature's wilsjo2 state at one placement: the surfaces, the model's feature, the reset latch.
struct Wilsjo2Owner;
Wilsjo2Owner* wilsjo2_owner();
// The owner of a layer placed at Present (its log says so, not "after the upscaler").
void wilsjo2_owner_at_present(Wilsjo2Owner& o);
// A run of layer `row` (its NR Control row) with spatial compression on, done (either API); whether that layer ran so
// since the preceding presentation (NR Control draws outlines for that presented frame).
void wilsjo2_note_spatial(int row, const nr::wilsjo2::SpatialLayout& layout);
nr::wilsjo2::SpatialLayout wilsjo2_spatial_layout(int row);
void wilsjo2_present_spatial(); // consume the runs since the preceding presentation
bool wilsjo2_spatial_running(int row);
void wilsjo2_release(Wilsjo2Owner*);
// The retired surfaces' clock: called at every evaluate and Present of the .asi (under its lock), so what a released
// owner retired is freed whether or not a wilsjo2 layer still runs.
void wilsjo2_tick();

// A wilsjo2 layer's settings from its section's keys (nr_pe_config.hpp LayerConfig::keys), its placement taking the
// place of RunBeforeSR; a key it does not know is logged once.
nr::wilsjo2::Settings wilsjo2_settings(const nr::pe::LayerConfig& layer);

// The common pipeline on `target` (the frame, UNORDERED_ACCESS, w x h, read and written in place).
// False when the frame was left as it came (a skip of the spec's: a feature made this frame, a surface that could not
// be made, ...).
bool wilsjo2_run(ID3D12GraphicsCommandList* cmd, ID3D12Device* device, Wilsjo2Owner& owner, const Wilsjo2Frame& f,
                 const nr::wilsjo2::Settings& settings, ID3D12Resource* target, uint32_t w, uint32_t h,
                 bool before_upscale);

// NR Control's comparison (wilsjo2_inspect.comp) on the D3D12 route: `before` and
// `after` (the frame without / with the compared layers, NON_PIXEL_SHADER_RESOURCE, the target's format and size) into
// `target` (UNORDERED_ACCESS), w x h. False when it could not be recorded (the target left as it was).
bool wilsjo2_inspect(ID3D12GraphicsCommandList* cmd, ID3D12Device* device, ID3D12Resource* before, ID3D12Resource* after,
                     ID3D12Resource* target, uint32_t w, uint32_t h, uint32_t compare, float split, float zoom, bool swap,
                     bool apply_model);

// HUD FIX at Present on D3D12 (wilsjo2_hudfix.comp), as wilsjo2_hudfix_vk: every resource UNORDERED_ACCESS; `hudless`
// null: `back`; `fresh`: `alpha` was just made (read as 0). `estimate_too` (source 2): the estimate's state follows this
// frame as well (`previous`; Auto composing from a HUD-less frame). False when not recorded.
struct HudFixD3D12 {
    ID3D12Resource *back = nullptr, *hudless = nullptr, *frame = nullptr, *alpha = nullptr, *stats = nullptr,
                   *previous = nullptr;
    // The estimate with the game's motion vectors (null: without): their texture (in NON_PIXEL_SHADER_RESOURCE), the
    // output pixels per unit, their region's origin and its texels per output pixel
    ID3D12Resource* motion = nullptr;
    float motion_scale_x = 1.f, motion_scale_y = 1.f, motion_step_x = 1.f, motion_step_y = 1.f;
    uint32_t motion_x = 0, motion_y = 0;
};
bool wilsjo2_hudfix(ID3D12GraphicsCommandList* cmd, ID3D12Device* device, const HudFixD3D12& r, uint32_t w, uint32_t h,
                    uint32_t source, bool show_mask, bool fresh, uint32_t parity, bool estimate_too = false);

// The finished picture's HDR10 conversion on `cmd`: to_linear: `source` (PQ BT.2020) -> `target` (linear
// BT.709 scRGB, RGBA16F); else `source` (linear) with `original` (the PQ picture) -> `target` (PQ, the original's
// format). `source` and `original` NON_PIXEL_SHADER_RESOURCE, `target` UNORDERED_ACCESS. False when not recorded.
bool wilsjo2_convert(ID3D12GraphicsCommandList* cmd, ID3D12Device* device, ID3D12Resource* source,
                     ID3D12Resource* original, ID3D12Resource* target, uint32_t w, uint32_t h, bool to_linear);

// ---- Vulkan games (nr_asi_wilsjo2_vk.cpp): the same flow on the game's own VkCommandBuffer ------------------------
// Step for step the flow above, with images of the game's (in the layouts the call hands them: the guides and the
// exposure SHADER_READ_ONLY_OPTIMAL, the target GENERAL) and surfaces of ours (GENERAL throughout); the model through
// nr::dlssnr::evaluate_vk_asi.
struct Wilsjo2VkImage {
    VkImage image = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0, height = 0;
    VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL;
    VkImageUsageFlags usage = 0;   // its usage where known (0: unknown)
};
struct Wilsjo2VkFrame {
    Wilsjo2VkImage depth, motion, exposure;   // exposure.image null: none
    uint32_t flags = 0;
    bool hdr = false;          // after the upscaler: the flags and the output's format
    bool hdr_before = false;   // before it: the flags and the colour's format (the image processed there)
    bool reset = false;
    float pre_exposure = 1.0f;
    float mv_x = 1.0f, mv_y = 1.0f;
    nr::dlssnr::Rect depth_rect, motion_rect;
};
struct Wilsjo2VkOwner;
Wilsjo2VkOwner* wilsjo2_vk_owner();
// As wilsjo2_owner_at_present: this owner runs a Present layer (its NR Control note says so).
void wilsjo2_vk_owner_at_present(Wilsjo2VkOwner& o);
void wilsjo2_vk_release(Wilsjo2VkOwner*);
// As wilsjo2_tick, for the Vulkan surfaces: called at every Vulkan evaluate of the .asi (under its lock).
void wilsjo2_vk_tick();
// The game's device the surfaces are made on (before the first run).
void wilsjo2_vk_device(VkDevice device, VkPhysicalDevice physical);
// The device goes (the game's vkDestroyDevice; every owner released before): the retired surfaces and the programs
// destroyed now, while it is still there.
void wilsjo2_vk_device_destroyed();
// The process is ending (see nr::dlssnr::abandon_at_exit): the stages are left as they are, not destroyed.
void wilsjo2_abandon_at_exit();
void wilsjo2_vk_abandon_at_exit();
bool wilsjo2_run_vk(VkCommandBuffer cmd, Wilsjo2VkOwner& owner, const Wilsjo2VkFrame& f, const nr::wilsjo2::Settings& settings,
                    const Wilsjo2VkImage& target, uint32_t w, uint32_t h, bool before_upscale);
// As wilsjo2_convert, on the Vulkan and D3D11 routes' Present layers: every image GENERAL. False when not recorded.
bool wilsjo2_convert_vk(VkCommandBuffer cmd, const Wilsjo2VkImage& source, const Wilsjo2VkImage* original,
                        const Wilsjo2VkImage& target, uint32_t w, uint32_t h, bool to_linear);
// An exact copy of `w` x `h` texels from (sx, sy) of `source` to (dx, dy) of `target` (wilsjo2_copy.comp), for the
// Vulkan route where a transfer copy cannot serve. `source` is read through `view` when given (the game's own: its mip
// level, array layer and channel order), else through a view of ours at `mip` / `layer` - as a storage image when
// `storage_read` (an image with storage use only; `target` is then ours). `target` (GENERAL) is written at its `mip` /
// `layer`. False when not recorded (the programs missing, a view not made).
struct VkCopySide {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0, height = 0;
    VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL;
    uint32_t mip = 0, layer = 0;
};
bool wilsjo2_copy_vk(VkCommandBuffer cmd, const VkCopySide& source, bool storage_read, uint32_t sx, uint32_t sy,
                     const VkCopySide& target, uint32_t dx, uint32_t dy, uint32_t w, uint32_t h);
// HUD FIX at Present (wilsjo2_hudfix.comp), on the Vulkan and D3D11 routes: every image GENERAL. `back` the frame as
// the game presented it (its copy, before the Present layers), `hudless` the same frame without its HUD (null image:
// `back`), `frame` the layers' answer on it, written over with the composition; `alpha` (RG16F: .r the HUD's alpha) and
// `stats` (R32_UINT, max(4, W/8) x 1 + H/8: the counts, the estimate's tile flags) the stream's; `fresh`: `alpha` and
// `stats` were just made (read as 0). `source`: 2 the difference mask of hudless against back, 4 the estimate
// (`previous`, the last frame's back, and alpha updated first), 0 / 1 a UI alpha / UI colour in `alpha`. `show_mask`:
// the alpha shown instead. `estimate_too` (source 2): the estimate's state follows this frame as well (`previous`).
// False when not recorded.
struct HudFixVkImages {
    Wilsjo2VkImage back, hudless, frame, alpha, stats, previous;
    // the estimate with the game's motion vectors (motion.image null: without), as HudFixD3D12
    Wilsjo2VkImage motion;
    float motion_scale_x = 1.f, motion_scale_y = 1.f, motion_step_x = 1.f, motion_step_y = 1.f;
    uint32_t motion_x = 0, motion_y = 0;
};
bool wilsjo2_hudfix_vk(VkCommandBuffer cmd, const HudFixVkImages& im, uint32_t w, uint32_t h, uint32_t source, bool show_mask,
                       bool fresh, uint32_t parity, bool estimate_too = false);
// NR Control's comparison on the Vulkan and D3D11 routes, as wilsjo2_inspect: `before` and `after` (GENERAL) shown into
// `target` (GENERAL, w x h). False when not recorded.
bool wilsjo2_inspect_vk(VkCommandBuffer cmd, const Wilsjo2VkImage& before, const Wilsjo2VkImage& after,
                        const Wilsjo2VkImage& target, uint32_t w, uint32_t h, uint32_t compare, float split, float zoom,
                        bool swap, bool apply_model);

}  // namespace nr::asi
