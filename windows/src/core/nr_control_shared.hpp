#pragma once
// What NR Control (the OptiScaler route's menu, windows/src/pe/nr_pe_menu.cpp) and the rest of our DLL
// (windows/src/pe/nr_pe_control.cpp) share: one page of the process (the layout is the Linux build's, where the menu
// is a Vulkan layer and the page a file both map). Each field has one writer.
//   DLL -> menu: what NR is doing (its cost, sizes, the running network), refreshed about four times a second.
//   menu -> DLL: that the menu takes its input from the DLL (`wayland`, always set here) and whether it is open (the
//   DLL then keeps the game from getting the input).
//   DLL -> menu: the keyboard and mouse as the game's window sees them.
#include <cstdint>

namespace nr::control {

inline constexpr uint32_t kMagic = 0x4C4F5243u;   // "CROL"
inline constexpr uint32_t kVersion = 5;
inline constexpr uint32_t kPanelLines = 10;

struct Shared {
    uint32_t magic, version;
    // DLL -> menu
    uint32_t status_seq;            // bumped on every refresh
    uint32_t route;                 // 0 none yet, 1 OptiScaler (D3D12), 2 OptiScaler (Vulkan)
    uint32_t renodx;                // the RenoDX-style option runs
    uint32_t nr_on;                 // NR runs this frame (NeuralUplift, after NRToggleKey)
    float gpu_ms, network_ms;       // NR's whole cost and the network's part, GPU time
    uint32_t width, height;         // the frame NR works on
    uint32_t nr_width, nr_height;   // the network's extent
    uint32_t render_width, render_height;   // the game's render extent (the depth guide's), 0 unknown
    uint32_t placement;             // where NR runs: 0 not known, 1 after the upscaler, 2 before it
    uint32_t passes;                // model passes a frame
    uint32_t net_running;           // the network serving (nr::NetworkChoice), 3 none yet
    uint32_t preprocess;            // Preprocess on
    uint32_t building;              // a network is being built
    char state[96];                 // a line for the menu (empty: nothing to add)
    char screenshot[96];            // the NR screenshot's status (nr_screenshot.hpp)
    // menu -> DLL
    uint32_t wayland;               // the menu takes its input from the DLL: send it (always set on Windows)
    uint32_t menu_open;             // the menu is open: the game's window gets no keyboard or mouse input
    uint32_t screenshot_request;    // bumped by the menu's screenshot button
    uint32_t menu_seq;              // bumped by the menu on every Present (the DLL sees the layer is loaded)
    // DLL -> menu: the input
    uint32_t input_seq;
    int32_t mouse_x, mouse_y;       // in the window's client area
    uint32_t client_w, client_h;
    uint32_t buttons;               // bit 0 left, 1 right, 2 middle
    uint32_t cursor_shown;          // the game shows a cursor (else the menu draws its own)
    int32_t wheel;                  // WHEEL_DELTA units, accumulated (the menu takes the difference)
    uint32_t keys[8];               // Windows virtual-key codes down, 256 bits
    uint32_t chars[64];             // typed characters, a ring
    uint32_t chars_written;         // characters ever written (index chars_written % 64 is the next)
    // DLL -> menu
    uint32_t output_width, output_height;   // the upscaler's output (the game's DLSS evaluate), 0 unknown
    uint32_t game_focused;          // the foreground window is the game's (X11: the menu takes it as focus)
    // DLL -> menu: the network asked for ([Network] Model as the switch has it), and why each network cannot run in
    // this run ("" it can; before the DLL has looked: all empty, net_known 0)
    uint32_t net_want, net_known;
    char net_why[3][96];
    // DLL -> menu: the network being built (nr::NetworkChoice, 3 none) and how far (0..1), for the OSD
    uint32_t building_net;
    float build_progress;
    // DLL -> menu: the engine-data panel (nr_engine_panel.hpp), one line each; '!' starts the hint
    uint32_t panel_lines;
    char panel[kPanelLines][192];
    uint32_t reserved[11];
};
static_assert(sizeof(Shared) % 4 == 0, "a plain array of words");

}  // namespace nr::control
