#pragma once
// <game>/dlssnr-amd/control.bin: what NR Control (the OptiScaler route's menu,
// linux/src/pe/nr_pe_menu.cpp) and our DLL inside the game (Windows code under Proton,
// linux/src/pe/nr_pe_control.cpp) share. Both map the same file (the DLL's file mapping is a shared mmap of it under
// Wine), so the page is common memory; each field has one writer. Fixed layout, the same for both compilers.
//   DLL -> menu: what NR is doing (its cost, sizes, the running network), refreshed about four times a second.
//   menu -> DLL: whether the menu draws on a Wayland surface (a layer cannot read the keyboard or mouse there) and
//   whether it is open (the DLL then keeps the game from getting the input).
//   DLL -> menu, on Wayland: the keyboard and mouse as the game's window sees them.
#include "nr_ini_network.hpp"
#include "nr_vram_tag.hpp"
#include <cstdint>

namespace nr::control {

inline constexpr uint32_t kMagic = 0x4C4F5243u;   // "CROL"
inline constexpr uint32_t kVersion = 13;
inline constexpr uint32_t kPanelLines = 10;
inline constexpr uint32_t kMaxLayers = 8;
// The engine-data line nr_engine_panel.hpp writes when the game gives no exposure and nothing covers it (its start;
// NR Control's [Preprocess] page reads the layers' lines for it).
inline constexpr char kNoExposureHint[] = "!The game gives no exposure.";
// A network field's "none (yet)": one past the networks (nr::NetworkChoice).
inline constexpr uint32_t kNoNet = nr::kNetworkChoices;

// One layer of the .asi's NR chain as it last ran, for NR Control's status (rows by layer number; 0: an empty row).
struct LayerStatus {
    uint32_t number;
    uint32_t processing;            // 0 RenoDX, 1 wilsjo2
    uint32_t placement;             // where it ran this frame: 0 after the upscaler, 1 before it, 2 at Present
    uint32_t state;                 // 0 not run yet, 1 ran, 2 its network is being built, 3 skipped (note says why)
    uint32_t net;                   // the network that ran (nr::NetworkChoice), kNoNet none yet
    uint32_t width, height;         // the frame it works on
    uint32_t nr_width, nr_height;   // the network's extent
    uint32_t passes;
    float fast_passes;              // 0 legacy, otherwise the effective fast strength
    uint32_t motion;                // this layer had motion vectors on its last run
    float gpu_ms, network_ms;       // its runtime's average a run: all of it, and the network's part
    uint32_t updated;               // GetTickCount (low 32 bits) of the last update
    char note[64];                  // why it was skipped, or what it waits for
    uint32_t panel_lines;           // the engine-data panel of this layer's runs (as Shared::panel)
    char panel[kPanelLines][192];
    uint32_t panel_at;              // GetTickCount (low 32 bits) of its last write
};

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
    uint32_t net_running;           // the network serving (nr::NetworkChoice), kNoNet none yet
    uint32_t preprocess;            // Preprocess on
    uint32_t building;              // a network is being built
    char state[96];                 // a line for the menu (empty: nothing to add)
    char screenshot[96];            // the NR screenshot's status (nr_screenshot.hpp)
    // menu -> DLL
    uint32_t wayland;               // the menu's swap chain is on a Wayland surface: send the input
    uint32_t menu_open;             // the menu is open: the game's window gets no keyboard or mouse input
    uint32_t screenshot_request;    // bumped by the menu's screenshot button
    uint32_t menu_seq;              // bumped by the menu on every Present (the DLL sees the layer is loaded)
    // DLL -> menu, Wayland only
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
    char net_why[nr::kNetworkChoices][96];
    // DLL -> menu: the network being built (nr::NetworkChoice, kNoNet none) and how far (0..1), for the OSD
    uint32_t building_net;
    float build_progress;
    // DLL -> menu: the engine-data panel (nr_engine_panel.hpp), one line each; '!' starts the hint
    uint32_t panel_lines;
    char panel[kPanelLines][192];
    // menu -> menu: bumped on every Present by NR Control drawn from our DLL (the .asi route, nr_pe_menu.cpp); the
    // Vulkan layer's menu steps aside while it moves
    uint32_t dll_menu;
    char build_phase[40];           // DLL -> menu: the build's stage in words ("creating pipelines 25/62, 8 s"), for the OSD
    // DLL -> menu (the .asi route): the NR chain's layers
    LayerStatus layers[kMaxLayers];
    // DLL -> menu: video memory in KiB. NR's own (nr_vram_ledger.hpp) by network (nr::NetworkChoice; kNoNet: not
    // a network's) and kind (nr::vram::Kind), and the process's use of the device-local heap, its budget and size
    // (VK_EXT_memory_budget: the game's memory included).
    uint32_t vram_known;
    uint32_t vram_kib[kNoNet + 1][nr::vram::kKinds];
    uint32_t vram_process_kib, vram_budget_kib, vram_heap_kib;
    // DLL -> menu (the .asi route): its build, and frame generation as measured (presents per game frame)
    char asi_build[48];
    char frame_gen[160];
    // DLL -> menu (the .asi route): HUD FIX ([HudFix]) - what it does now, and why each method cannot run here (by
    // Mode: 0 frame generation's HUD-less frame, 1 captured before the HUD, 2 estimated; "" it can)
    char hud_now[128];
    char hud_why[3][160];
};
static_assert(sizeof(Shared) % 4 == 0, "a plain array of words");

}  // namespace nr::control
