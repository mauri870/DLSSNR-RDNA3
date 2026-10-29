#pragma once
#include <vulkan/vulkan.h>
#include <vector>
// NR Control, the OptiScaler route's own NR menu (End), drawn by our DLL over the game's DXGI swap chain (the .asi route,
// nr_asi_main.cpp; ported from windows/src/pe/nr_pe_menu.cpp without its native-D3D12 parts): DXGI's
// Present is hooked in the system's dxgi.dll (below OptiScaler's own swap chain, after its menu and frame
// generation), and while the menu is open it is drawn with ImGui into an 8-bit image of its own and laid over the
// back buffer in the swap chain's encoding (SDR, scRGB, HDR10), on D3D11 and D3D12 swap chains. It edits the game's
// dlssnr-amd.ini - [RenoDX] (every NR setting with the RenoDX-style option on), [Preprocess], [Network] - which
// the DLL takes at once (nr_pe_config.hpp touch_config), and shows what NR is doing (nr_control_shared.hpp). Vulkan
// swap chains get the same menu from nr_pe_menu_vk.inl. OptiScaler's own menu (Insert) stays as it is.
//
// Test switches (no keyboard needed): NR_CONTROL_OPEN=1 starts it open; NR_CONTROL_AT=<n> makes the next two wait
// until it has been drawn n times; NR_CONTROL_SET=Section.Key=value,... writes once through the menu's ini path;
// NR_CONTROL_SHOT=1 presses its screenshot button once; NR_CONTROL_DUMP=<file> writes the first back buffer it
// drew into as a PPM (8-bit sRGB; HDR with 203 nits as white).

struct ID3D12Device;
struct ID3D12CommandQueue;
struct IDXGISwapChain;
struct ID3D12CommandList;
struct ID3D11Device;

namespace nr::pe::menu {

// Hooks DXGI's Present once per process (later calls do nothing), where dlssnr-amd\nr_present_probe.exe found it in a
// process of its own (nr_present_probe.cpp: a swap chain made in the game would be seen, and kept, by its hooks).
// Not from DllMain.
void install();
// Learns which queue a D3D12 swap chain presents on (DXVK's DXGI gives its device only): hooks ExecuteCommandLists of
// the queue class of `device` (the game's). Once per process; not from DllMain.
void watch_queues(ID3D12Device* device);
// Called on every Present of a D3D12 swap chain the menu sees, before it draws (whether or not it draws): the swap
// chain, the queue it presents on and its encoding as the menu judges it (0 SDR, 1 an sRGB format, 2 scRGB, 3 HDR10).
// The .asi's final-picture screenshot runs NR on the back buffer there.
using D3D12PresentHook = void (*)(IDXGISwapChain* swap_chain, ID3D12CommandQueue* queue, int encoding);
void on_d3d12_present(D3D12PresentHook hook);
// The same for a D3D11 swap chain (its device): the .asi's D3D11 Present layers run there when the game's own Present
// call was not reached.
using D3D11PresentHook = void (*)(IDXGISwapChain* swap_chain, ID3D11Device* device);
void on_d3d11_present(D3D11PresentHook hook);
// Called after every ExecuteCommandLists of the watched queue class (watch_queues), the game's and ours: the queue and
// the lists it was handed. The .asi's Present layers learn there where their guide copies were submitted.
using ExecuteObserver = void (*)(ID3D12CommandQueue* queue, unsigned count, ID3D12CommandList* const* lists);
void on_execute(ExecuteObserver observer);
// The colour space the game set on `swap_chain` (IDXGISwapChain3::SetColorSpace1, a DXGI_COLOR_SPACE_TYPE); false when
// it never set one.
bool colour_space_of(IDXGISwapChain* swap_chain, int* space);
// The process is ending (see nr::dlssnr::abandon_at_exit): the overlays are left as they are, not destroyed (their
// destructors wait for the GPU and release objects of modules detached already).
void abandon_at_exit();
// Native Vulkan Present: same Menu and input watcher as DXGI, its own Vulkan draw/compose passes.
const VkPresentInfoKHR* vulkan_present(VkInstance instance, VkPhysicalDevice physical, VkDevice device,
        VkQueue queue, uint32_t family, VkSwapchainKHR sc, VkFormat format, VkColorSpaceKHR space,
        VkExtent2D extent, const std::vector<VkImage>& images, const VkPresentInfoKHR* info);
void vulkan_forget(VkDevice device, VkSwapchainKHR sc = VK_NULL_HANDLE);

}  // namespace nr::pe::menu
