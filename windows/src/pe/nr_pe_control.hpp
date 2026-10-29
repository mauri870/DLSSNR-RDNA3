#pragma once
// Our side of NR Control (the OptiScaler route's menu, drawn by this DLL over the game's swap chain: nr_pe_menu.cpp),
// through the page nr_control_shared.hpp lays out: what NR is doing, for the menu to show, and the game window's
// keyboard and mouse for it, kept from the game while the menu is open.
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
    std::string net_why[3];                // why each network cannot run in this run ("" it can)
    int building_net = -1;                 // the network being built, -1 none
    float build_progress = 0.f;            // 0..1 (nr_pe_session.hpp build_progress)
    std::vector<std::string> panel;        // the engine-data panel's lines (nr_engine_panel.hpp)
};
// From each evaluate; written about four times a second. The first call starts the input watcher.
void report(const Status& s);
// The page (never null).
nr::control::Shared* page();

}  // namespace nr::pe::control
