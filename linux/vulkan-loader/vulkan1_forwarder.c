/* vulkan-1.dll forwarder for the OptiScaler route's game folder: every export forwards to winevulkan.dll, the way
   Proton's own vulkan-1.dll does. Proton's nativevulkanloader option (vulkan-1=n, which Steam sets for some games)
   otherwise loads a Khronos vulkan-1.dll from system32, while vkd3d-proton and DXVK keep calling winevulkan.dll
   directly: OptiScaler hooks vulkan-1.dll's exports to draw its menu over D3D12 games and never sees a call. Wine
   takes a native vulkan-1.dll from the game folder before system32's. The list of exports is Proton's (the same 252
   in GE-Proton 11-7, 11-9 and proton-cachyos 11.0). This module has no code. */
#include <windows.h>
BOOL WINAPI DllMain(HINSTANCE h, DWORD r, LPVOID p) { (void)h; (void)r; (void)p; return TRUE; }
