#pragma once
// NR Control, the OptiScaler route's own NR menu (End), drawn by our DLL over the game's DXGI swap chain: DXGI's
// Present is hooked in the system's dxgi.dll (below OptiScaler's own swap chain, after its menu and frame
// generation), and while the menu is open it is drawn with ImGui into an 8-bit image of its own and laid over the
// back buffer in the swap chain's encoding (SDR, scRGB, HDR10), on D3D11 and D3D12 swap chains. It edits the game's
// dlssnr-amd.ini - [RenoDX] (every NR setting with the RenoDX-style option on), [Preprocess], [Network] - which
// the DLL re-reads about once a second, and shows what NR is doing (nr_control_shared.hpp). The same menu as the
// Linux build's Vulkan layer (linux/src/layer/nr_control_layer.cpp). OptiScaler's own menu (Insert) stays as it is.
//
// Test switches (no keyboard needed): NR_CONTROL_OPEN=1 starts it open; NR_CONTROL_AT=<n> makes the next two wait
// until it has been drawn n times; NR_CONTROL_SET=Section.Key=value,... writes once through the menu's ini path;
// NR_CONTROL_SHOT=1 presses its screenshot button once; NR_CONTROL_DUMP=<file> writes the first back buffer it
// drew into as a PPM (8-bit sRGB; HDR with 203 nits as white).

namespace nr::pe::menu {

// Hooks DXGI's Present once per process (later calls do nothing), where dlssnr-amd\nr_present_probe.exe found it in a
// process of its own (nr_present_probe.cpp: a swap chain made in the game would be seen, and kept, by its hooks).
// Not from DllMain.
void install();

}  // namespace nr::pe::menu
