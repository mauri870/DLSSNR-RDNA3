#pragma once
// Our side of NR Control (the OptiScaler route's menu, drawn by the plugin: nr_pe_menu.cpp),
// through <game>/dlssnr-amd/control.bin (nr_control_shared.hpp): what NR is doing, for the menu to show; and, while
// the menu draws on a Wayland surface, the game window's keyboard and mouse for it (a layer cannot read them there),
// kept from the game while the menu is open.
#include "nr_ini_network.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace nr::control { struct Shared; }

namespace nr::pe::control {

struct Status {
    uint32_t route = 1;             // 1 OptiScaler D3D12, 2 OptiScaler Vulkan
    bool renodx = false, nr_on = true, preprocess = false, building = false;
    float gpu_ms = 0.f, network_ms = 0.f;
    uint32_t width = 0, height = 0, nr_width = 0, nr_height = 0, passes = 1;
    uint32_t render_width = 0, render_height = 0;   // the game's render extent (the depth guide's), 0 unknown
    uint32_t placement = 0;         // 0 not known, 1 after the upscaler, 2 before it (OptiScaler's frame info)
    uint32_t output_width = 0, output_height = 0;   // the upscaler's output (the game's DLSS evaluate), 0 unknown
    int net_running = -1, net_want = -1;   // nr::NetworkChoice, -1 none / not known
    std::string net_why[nr::kNetworkChoices];              // why each network cannot run in this run ("" it can)
    int building_net = -1;                 // the network being built, -1 none
    float build_progress = 0.f;            // 0..1 (nr_pe_session.hpp build_progress)
    std::string build_phase;               // its stage in words
    std::vector<std::string> panel;        // the engine-data panel's lines (nr_engine_panel.hpp)
    int layer = 0;                         // the .asi's NR-chain layer this run was (its row), 0 none
    float fast_passes = 0.f;
    bool motion = false, fast_fallback = false;
    bool waiting = false;                  // the run passed its frame through, not building (`waiting_why`)
    std::string waiting_why;
    bool vram_known = false;               // the process's device-local heap: use, budget, size (Session::video_memory)
    uint64_t vram_usage = 0, vram_budget = 0, vram_heap = 0;
};
// From each evaluate; written about four times a second. The first call starts the Wayland input watcher.
void report(const Status& s);
// The input watcher (End, the menu's keys and mouse) started without waiting for NR's first evaluate: NR Control drawn
// by this DLL calls it once it is installed, so the menu opens while no layer runs (all off, a game still in its menus).
void start_input();
// The page, for NR Control drawn by this DLL (nr_pe_menu.cpp, the .asi route): control.bin as mapped, or - where it
// cannot be mapped - a page of this process the menu alone sees. Never null.
nr::control::Shared* page();
// The .asi's NR chain, for NR Control's status: the layers there are now (rows of others are emptied), and one layer's
// row at a run - what ran where, or why it did not (`state` as nr::control::LayerStatus, `note` the reason).
void chain_layers(const std::vector<int>& numbers);
void layer_ran(int number, uint32_t processing, uint32_t placement, uint32_t state, const char* note);
// A layer's setup changed (processing, placement, network): its row's readings (sizes, passes, network, costs, engine
// data) go until it runs as it is now.
void layer_changed(int number);
void present_layers(); // clear running rows with no recording since the preceding presentation
// The .asi's build and its frame-generation line, for NR Control (and its report).
void asi_facts(const char* build, const char* frame_gen);
// HUD FIX's line for NR Control: what it does now (null: unchanged) and why Mode 2 / 3 / 4 cannot run (null: unchanged,
// "" it can).
void hud_facts(const char* now, const char* why_game, const char* why_captured, const char* why_estimated);

}  // namespace nr::pe::control
