#pragma once
// HUD FIX's capture on the .asi route (nr_hud_capture.hpp's rules): the game's commands followed through hooks of their
// functions, the back buffer copied where its HUD starts. Armed while [HudFix] Mode is Auto or Captured.
//   D3D12: the device's pipeline and render-target-view creation (from the game's swap chain on), the lists' draws,
//          copies and bindings and the queue's ExecuteCommandLists (from the first armed frame); idle while not armed.
//   D3D11: the immediate context's draws, copies and bindings (from the first armed frame); a deferred context's draws
//          are not seen.
//   Vulkan (a native Vulkan game's device): its image views, framebuffers, render passes and pipelines (from its
//          vkCreateDevice), the command buffers' render passes, draws, copies and bindings (from the first armed frame),
//          their submits in order. The copy can only be made outside a render pass: before each pass on a swap-chain
//          image that keeps its contents - a HUD drawn in the scene's own pass is not captured.
#include <d3d11.h>
#include <d3d12.h>
#include "nr_gpu_completion.hpp"
#include <dxgi1_4.h>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>
#include <vector>

namespace nr::asi::hudcap {

// The game's swap chain was made on `device` (its back buffers' views and the pipelines made from now on are followed
// once the capture is armed; `armed_now`: [HudFix] Mode is Auto or Captured already - the device's hooks go in at once).
void d3d12_device(ID3D12Device* device, bool armed_now);

// At the game's Present, before the layers: armed or not this frame (the hooks go in at the first armed one; `queue` the
// one the game presents on), and the verdict on the frame's writes for `back` (its swap chain's current buffer, `format`
// NR's writable twin of its format).
// `hudless`: the copy (UNORDERED_ACCESS, `format`, w x h) when captured. `why`: why not.
enum class Result { kOff, kCaptured, kClean, kMissed };
// Frame generation's HUD-less frame this frame (D3D12): what the game handed its frame generation before its Present -
// FSR 3.1's ffxConfigure (HUDLessColor), XeSS-FG's tag (XEFG_SWAPCHAIN_RES_HUDLESS_COLOR), DLSS-G's (Streamline's
// kBufferTypeHUDLessColor tag, NGX's DLSSG.HUDLess at its evaluate: fg_note_dlssg). Handed over while the game's Present
// runs (OptiScaler's own frame generation, inside its Present hook) it would be a frame late: not taken (the capture
// serves those games).
struct Fg {
    bool seen = false;             // this frame's, usable: `copy` holds it, or (copied == false) `game` in `state` does
    ID3D12Resource* copy = nullptr;   // ours (UNORDERED_ACCESS, the back buffer's writable format, w x h)
    bool copied = false;           // already copied into `copy` (on the game's list, when it was handed over)
    ID3D12Resource* game = nullptr;   // the game's resource, valid until the present
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    bool substitute = false;       // NR's answer may be written back into `game` (frame generation reads it at the present)
    const char* source = "";
    std::string why;               // not seen: why
};
struct Frame {
    Result result = Result::kOff;
    ID3D12Resource* hudless = nullptr;
    std::string why;
    bool broken = false;   // the capture cannot run in this game at all (its hooks): greyed in the menu, `why`
    Fg fg;
};
Frame d3d12_present(ID3D12Device* device, IDXGISwapChain* sc, ID3D12CommandQueue* queue, ID3D12Resource* back,
                    DXGI_FORMAT format, uint32_t w, uint32_t h, bool armed, bool fg_armed = false);
// After every ExecuteCommandLists of the game's (NR Control's queue hook calls the .asi's observer, which calls this;
// the capture hooks the queue itself only where that hook is not there): the lists' writes in the GPU's order.
nr::gpu::Ticket d3d12_recording(ID3D12GraphicsCommandList* list);
bool vk_lifetime(VkDevice device);
void d3d12_executed(unsigned count, ID3D12CommandList* const* lists);
// NGX's DLSS-G evaluate (the .asi's NVSDK_NGX_D3D12_EvaluateFeature hook): its DLSSG.HUDLess (COPY_DEST there, as
// OptiScaler takes it), copied on its command list.
void fg_note_dlssg(ID3D12GraphicsCommandList* list, ID3D12Resource* hudless);
// The game's Present call runs (the .asi's Present detours, around the original): frame generation's hand-overs then
// are OptiScaler's own (a frame late), not taken.
struct PresentCall {
    PresentCall();
    ~PresentCall();
};

// D3D11, as d3d12_present: `back` the swap chain's buffer, `format` NR's writable twin. `hudless`: the copy (a texture
// of `format`, w x h, shader resource + unordered access) when captured.
struct Frame11 {
    Result result = Result::kOff;
    ID3D11Texture2D* hudless = nullptr;
    std::string why;
    bool broken = false;
};
Frame11 d3d11_present(ID3D11Device* device, ID3D11Texture2D* back, DXGI_FORMAT format, uint32_t w, uint32_t h, bool armed);

// Vulkan: the game's device was made (`translator`: by DXVK or vkd3d-proton - not followed: their games have their
// D3D capture). Its creations are followed from now on.
void vk_device(VkDevice device, bool translator);
// What the game's device's vkCreateImage / vkCreateImageView said (followed from vk_device, not on a translator's
// device): an image's usage (false: not seen), whether a view reorders its channels (false: identity or not seen). The
// .asi's Vulkan route chooses its copies by them (a transfer copy where the image allows one).
bool vk_image_usage(VkImage image, VkImageUsageFlags* usage);
bool vk_view_reordered(VkImageView view);
// The game's queue submits (the .asi's queue hooks): its command buffers in submit order.
void vk_submitted(uint32_t n, const VkCommandBuffer* buffers);
void vk_submitted2(uint32_t n, const VkSubmitInfo2* submits);
// At a native Vulkan game's Present, before the layers: `images` its swap chain's, `image` the one presented, `hudless`
// the image the copies go into (ours: GENERAL, the swap chain's format, w x h; null: none this frame).
struct FrameVk {
    Result result = Result::kOff;
    std::string why;
    bool broken = false;
};
FrameVk vk_present(VkDevice device, const std::vector<VkImage>& images, VkImage image, VkImage hudless, uint32_t w, uint32_t h,
                   bool armed);

// The .asi records its own commands on this thread (its Present layers): the hooks pass them through unseen.
struct Own {
    Own();
    ~Own();
};

}  // namespace nr::asi::hudcap
