// See nr_pe_menu.hpp.
#include "nr_vk_receipts.hpp"
#include "nr_number.hpp"
#include "nr_pe_menu.hpp"
#include "mingw_d3d12_compat.h"
#include "nr_control_shared.hpp"
#include "nr_ini_network.hpp"
#include "nr_control_pages.hpp"
#include "nr_control_ui.hpp"
#include "nr_ini_edit.hpp"
#include "nr_wilsjo2_keys.hpp"
#include "nr_menu_font.hpp"   // Inter, embedded by linux/build/build_asi.sh (embed_font.py)
#include "nr_pe_config.hpp"
#include "nr_asi_wilsjo2.hpp"
#include "nr_wilsjo2.hpp"
#include "nr_pe_control.hpp"
#include "nr_pe_log.hpp"
#include "nr_vram_ledger.hpp"

#include "MinHook.h"
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_dx12.h"
#include "imgui_impl_vulkan.h"
#include "menu_compose_vert.h"
#include "menu_compose_frag.h"
#include "nr_pe_vkdevice.hpp"

#include <windows.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// The ImGui D3D backends compile their shaders with D3DCompile; so do we. Resolved from d3dcompiler_47.dll on first
// use, so the DLL imports no shader compiler at load.
extern "C" HRESULT WINAPI D3DCompile(LPCVOID data, SIZE_T size, LPCSTR name, const D3D_SHADER_MACRO* defines,
                                     ID3DInclude* include, LPCSTR entry, LPCSTR target, UINT flags1, UINT flags2,
                                     ID3DBlob** code, ID3DBlob** errors) {
    using Fn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT,
                                UINT, ID3DBlob**, ID3DBlob**);
    static const Fn fn = [] {
        HMODULE m = LoadLibraryW(L"d3dcompiler_47.dll");
        return m ? reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(m, "D3DCompile"))) : nullptr;
    }();
    if (!fn) return E_NOTIMPL;
    return fn(data, size, name, defines, include, entry, target, flags1, flags2, code, errors);
}
// The D3D12 and DXGI entry points the menu (and ImGui's D3D12 backend, in its multi-viewport code, not used) name,
// taken from the modules already in the process when called: imported, they would load d3d12.dll from OptiScaler's
// DllMain (this module is loaded there) and bind dxgi.dll to OptiScaler itself (it is the game's dxgi.dll).
extern "C" HRESULT WINAPI D3D12SerializeRootSignature(const D3D12_ROOT_SIGNATURE_DESC* desc, D3D_ROOT_SIGNATURE_VERSION version,
                                                      ID3DBlob** blob, ID3DBlob** errors) {
    using Fn = HRESULT(WINAPI*)(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob**);
    HMODULE m = GetModuleHandleW(L"d3d12.dll");
    const Fn fn = m ? reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(m, "D3D12SerializeRootSignature"))) : nullptr;
    return fn ? fn(desc, version, blob, errors) : E_NOTIMPL;
}
extern "C" HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void** factory) {
    using Fn = HRESULT(WINAPI*)(REFIID, void**);
    HMODULE m = GetModuleHandleW(L"dxgi.dll");
    const Fn fn = m ? reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(m, "CreateDXGIFactory1"))) : nullptr;
    return fn ? fn(riid, factory) : E_NOTIMPL;
}
// The vendored Win32 backend's module handle (mingw_d3d12_compat.h); that backend is not linked here.
HMODULE dllModule = nullptr;

namespace nr::pe::menu {
namespace {

void say(const std::string& s) { log("[nr] NR Control: %s", s.c_str()); }

template <class T> void release(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

nr::control::Shared* shared() { return nr::pe::control::page(); }

std::filesystem::path game_dir() { return std::filesystem::path(nr::pe::module_folder()); }

// dlssnr-amd.ini edited in place, a key table's values by section, the chain's global keys: nr_ini_edit.hpp (shared with
// the ReShade add-on's layer page)
using nr::ini_edit::Ini;
using nr::ini_edit::KeyValues;
using nr::ini_edit::chain_global;

// NR_CONTROL_AT=<n>: NR_CONTROL_SET, NR_CONTROL_SHOT and NR_CONTROL_DUMP wait until the menu has been drawn n times.
uint64_t control_at() {
    const char* e = std::getenv("NR_CONTROL_AT");
    return e ? std::strtoull(e, nullptr, 10) : 0;
}
uint64_t g_drawn = 0;   // frames the menu was drawn in
// NR_CONTROL_TRACE=1 (tests): every Present's menu outcome in the log - drawn (ImGui's vertex and list counts) or why not
void trace(const char* what, int vertices = -1, int lists = -1) {
    static const bool on = [] { const char* e = std::getenv("NR_CONTROL_TRACE"); return e && e[0] == '1'; }();
    if (!on) return;
    char t[160];
    std::snprintf(t, sizeof t, "menu trace: drawn %llu %s vertices %d lists %d", static_cast<unsigned long long>(g_drawn),
                  what, vertices, lists);
    say(t);
}
void trace_drawn() {
    const ImDrawData* d = ImGui::GetDrawData();
    trace("drawn", d ? d->TotalVtxCount : -1, d ? d->CmdListsCount : -1);
}

bool open_at_start() {
    const char* e = std::getenv("NR_CONTROL_OPEN");
    return e && e[0] == '1';
}

// ---- input: the game window's keyboard and mouse as the DLL reads them (nr_pe_control.cpp) ----
struct Input {
    bool open = open_at_start();
    uint32_t keys_before[8]{};
    int32_t wheel_seen = 0;
    uint32_t chars_seen = 0;
    bool primed = false;
    static ImGuiKey key_of(unsigned vk) {
        if (vk >= 'A' && vk <= 'Z') return ImGuiKey(ImGuiKey_A + int(vk - 'A'));
        if (vk >= '0' && vk <= '9') return ImGuiKey(ImGuiKey_0 + int(vk - '0'));
        switch (vk) {
            case 0x08: return ImGuiKey_Backspace; case 0x09: return ImGuiKey_Tab; case 0x0D: return ImGuiKey_Enter;
            case 0x1B: return ImGuiKey_Escape; case 0x20: return ImGuiKey_Space; case 0x21: return ImGuiKey_PageUp;
            case 0x22: return ImGuiKey_PageDown; case 0x23: return ImGuiKey_End; case 0x24: return ImGuiKey_Home;
            case 0x25: return ImGuiKey_LeftArrow; case 0x26: return ImGuiKey_UpArrow; case 0x27: return ImGuiKey_RightArrow;
            case 0x28: return ImGuiKey_DownArrow; case 0x2D: return ImGuiKey_Insert; case 0x2E: return ImGuiKey_Delete;
            case 0xA0: return ImGuiKey_LeftShift; case 0xA1: return ImGuiKey_RightShift;
            case 0xA2: return ImGuiKey_LeftCtrl; case 0xA3: return ImGuiKey_RightCtrl;
            case 0xA4: return ImGuiKey_LeftAlt; case 0xA5: return ImGuiKey_RightAlt;
            default: return ImGuiKey_None;
        }
    }
    // End opens and closes, Esc closes; the rest goes to ImGui while open. `width` x `height`: the back buffer.
    void update(ImGuiIO& io, uint32_t width, uint32_t height) {
        auto* s = shared();
        s->wayland = 1;
        auto down = [&](unsigned vk) { return (s->keys[vk / 32] >> (vk % 32)) & 1u; };
        auto was = [&](unsigned vk) { return (keys_before[vk / 32] >> (vk % 32)) & 1u; };
        if (!primed) {   // what was already held or typed before the first look is not input to the menu
            std::memcpy(keys_before, s->keys, sizeof keys_before);
            wheel_seen = s->wheel;
            chars_seen = s->chars_written;
            primed = true;
        }
        if (down(0x23) && !was(0x23)) open = !open;   // End
        if (open && down(0x1B) && !was(0x1B) && !io.WantTextInput) open = false;   // Esc
        s->menu_open = open ? 1 : 0;
        // the game hid its cursor (mouse look): the menu draws one
        io.MouseDrawCursor = open && !s->cursor_shown;
        if (open) {
            const float sx = s->client_w ? float(width) / float(s->client_w) : 1.f;
            const float sy = s->client_h ? float(height) / float(s->client_h) : 1.f;
            io.AddMousePosEvent(float(s->mouse_x) * sx, float(s->mouse_y) * sy);
            for (int b = 0; b < 3; ++b) io.AddMouseButtonEvent(b, (s->buttons >> b) & 1u);
            const int32_t wheel = s->wheel;
            if (wheel != wheel_seen) io.AddMouseWheelEvent(0.f, float(wheel - wheel_seen) / 120.f);
            for (unsigned vk = 8; vk < 256; ++vk)
                if (down(vk) != was(vk))
                    if (ImGuiKey k = key_of(vk); k != ImGuiKey_None) io.AddKeyEvent(k, down(vk) != 0);
            io.AddKeyEvent(ImGuiMod_Ctrl, down(0x11) != 0);
            io.AddKeyEvent(ImGuiMod_Shift, down(0x10) != 0);
            io.AddKeyEvent(ImGuiMod_Alt, down(0x12) != 0);
            for (uint32_t c = chars_seen; c != s->chars_written; ++c) io.AddInputCharacter(s->chars[c % 64]);
        }
        wheel_seen = s->wheel;
        chars_seen = s->chars_written;
        std::memcpy(keys_before, s->keys, sizeof keys_before);
    }
    void close() {
        open = false;
        shared()->menu_open = 0;
    }
};
Input g_input;   // one menu for the process, whichever swap chain shows it

// ---- the NR chain on this route (nr_control_pages.hpp ChainEditor): layers before or after the upscaler or at Present;
// a file without [LayerN] is one layer, as the .asi reads it: wilsjo2 with [RenoDX] Enabled = 0 given, else RenoDX
using nr::control_ui::chain_rows;
using nr::control_ui::draw_layer_status;
using nr::control_ui::draw_vram;
const nr::control_ui::ChainRoute kAsiChain = [] {
    nr::control_ui::ChainRoute r;
    r.chain_help = "Each layer is an NR pass of its own: its processing (RenoDX or wilsjo2), its network ([Network] "
                   "Model, or one of its own), where it runs, and its own settings below. Layers at one place run "
                   "in layer order; before the upscaler, then after it, then at Present. Each layer costs a network "
                   "run, and a layer with another network or size its own memory.";
    r.wilsjo2_about = "The OptiScaler-NR fork's processing around the network, ported.";
    r.wilsjo2_table = nr::wilsjo2_keys::kKeys;
    r.wilsjo2_count = nr::wilsjo2_keys::kCount;
    r.passes_help = "Passes. Model passes a frame, 1-3 (up to 30 with Unlock passes); each costs one more network "
                    "run. Only the last pass's answer is composed onto the frame.";
    r.pass_same_help = "On: pass 1's settings, with the local tone at 0 (the fork's rule for later passes). Off: "
                       "this pass's own settings below.";
    r.legacy_row = [](const Ini& ini) {
        nr::control_ui::ChainRow r;
        r.number = 1;
        const bool wilsjo2 = ini.get("RenoDX", "Enabled", "") == "0";
        r.processing = wilsjo2 ? "wilsjo2" : "RenoDX";
        const std::string hp = ini.get("RenoDX", "NRHookPoint", "2");
        const std::string rb = ini.get("wilsjo2", "RunBeforeSR", "auto");
        r.placement = wilsjo2 ? (rb == "1" || rb == "true" ? "before" : rb == "0" || rb == "false" ? "after" : "present")
                              : hp == "1" ? "before" : hp == "0" ? "after" : "present";
        return r;
    };
    r.legacy_source = [](const Ini& ini, const nr::control_ui::ChainRow& row) {
        return std::make_pair(&ini, std::string(row.processing == "wilsjo2" ? "wilsjo2" : "RenoDX"));
    };
    // The first change of a file without [LayerN] writes its one layer as [Layer1], its settings copied over
    r.materialize = [legacy_row = r.legacy_row](Ini& ini) {
        const nr::control_ui::ChainRow r = legacy_row(ini);
        const std::string sec = "Layer1";
        ini.set(sec, "Enabled", "1");
        ini.set(sec, "Processing", r.processing);
        ini.set(sec, "Placement", r.placement);
        if (r.processing == "RenoDX") {
            for (const auto& [k, v] : ini.keys("RenoDX"))
                if (!chain_global(k)) ini.set(sec, k, v);
        } else {
            for (const auto& [k, v] : ini.keys("wilsjo2")) {
                std::string lk = k;
                for (char& c : lk) c = char(std::tolower(static_cast<unsigned char>(c)));
                if (lk != "runbeforesr") ini.set(sec, k, v);
            }
        }
        say("the one layer of [RenoDX] / [wilsjo2] written as [Layer1]");
    };
    r.renodx_route = [](const std::string& placement) {
        return placement == "present" ? nr::control_ui::Route::kOptiScalerPresent : nr::control_ui::Route::kOptiScaler;
    };
    return r;
}();

// ---- the menu's content: the same as the Linux layer's ----
struct Menu {
    ImGuiContext* context{};
    std::chrono::steady_clock::time_point previous = std::chrono::steady_clock::now();
    Ini ini{game_dir() / "dlssnr-amd.ini"};
    KeyValues renodx;            // [RenoDX]: NR on / off and the screenshot keys (all layers)
    // the NR chain's list and the selected layer's page (nr_control_pages.hpp); the selected layer's number at start:
    // NR_CONTROL_LAYER=<n>, for tests
    nr::control_ui::ChainEditor chain{ini, kAsiChain, [] { const char* e = std::getenv("NR_CONTROL_LAYER"); return e ? std::atoi(e) : 0; }()};
    int64_t loaded = -1;

    Menu() {
        context = ImGui::CreateContext();
        ImGui::SetCurrentContext(context);
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.ConfigDebugHighlightIdConflicts = false;   // a developer aid, not a player's message
        apply_style(1.f);
    }

    // NR Control's look ([NRControl] Theme): 0 Midnight (blue), 1 Graphite (teal), 2 Plum (violet); roomy spacing; every
    // size times `scale`.
    static ImVec4 rgb(uint32_t hex, float a = 1.f) {
        return {float((hex >> 16) & 255) / 255.f, float((hex >> 8) & 255) / 255.f, float(hex & 255) / 255.f, a};
    }
    static void apply_style(float scale, int theme = 0) {
        struct Palette { uint32_t bg, panel, frame, frame_hover, header, header_hover, accent, accent_soft, text, dim, border; };
        static const Palette palettes[3] = {
            {0x14171D, 0x1B1F27, 0x232833, 0x2C3341, 0x222A38, 0x2D3A50, 0x5B9EFA, 0x34507A, 0xE8EBF0, 0x8B93A1, 0x2B313C},
            {0x111416, 0x181D20, 0x20272B, 0x293338, 0x1D2629, 0x24363A, 0x2EC4B6, 0x1F5C57, 0xE6EDEC, 0x86939A, 0x263034},
            {0x15131B, 0x1C1924, 0x26212F, 0x30293C, 0x252033, 0x33294A, 0xA78BFA, 0x4B3C78, 0xEDE9F5, 0x948CA6, 0x2F2A3A}};
        const Palette& p = palettes[std::clamp(theme, 0, 2)];
        ImGuiStyle st;
        ImGui::StyleColorsDark(&st);
        st.WindowRounding = 10; st.ChildRounding = 8; st.FrameRounding = 6; st.PopupRounding = 8;
        st.GrabRounding = 6; st.ScrollbarRounding = 8; st.TabRounding = 6;
        st.WindowPadding = {16, 14}; st.FramePadding = {10, 6}; st.ItemSpacing = {10, 8}; st.ItemInnerSpacing = {8, 6};
        st.IndentSpacing = 20; st.ScrollbarSize = 12; st.GrabMinSize = 12; st.SeparatorTextBorderSize = 2;
        st.WindowBorderSize = 1; st.ChildBorderSize = 1; st.FrameBorderSize = 0; st.WindowTitleAlign = {0.02f, 0.5f};
        ImVec4* c = st.Colors;
        c[ImGuiCol_Text] = rgb(p.text);
        c[ImGuiCol_TextDisabled] = rgb(p.dim);
        c[ImGuiCol_WindowBg] = rgb(p.bg, 0.97f);
        c[ImGuiCol_ChildBg] = rgb(p.panel, 1.f);
        c[ImGuiCol_PopupBg] = rgb(p.panel, 0.99f);
        c[ImGuiCol_Border] = rgb(p.border);
        c[ImGuiCol_FrameBg] = rgb(p.frame);
        c[ImGuiCol_FrameBgHovered] = rgb(p.frame_hover);
        c[ImGuiCol_FrameBgActive] = rgb(p.accent_soft);
        c[ImGuiCol_TitleBg] = rgb(p.bg);
        c[ImGuiCol_TitleBgActive] = rgb(p.header);
        c[ImGuiCol_TitleBgCollapsed] = rgb(p.bg);
        c[ImGuiCol_Header] = rgb(p.header);
        c[ImGuiCol_HeaderHovered] = rgb(p.header_hover);
        c[ImGuiCol_HeaderActive] = rgb(p.accent_soft);
        c[ImGuiCol_Button] = rgb(p.header);
        c[ImGuiCol_ButtonHovered] = rgb(p.header_hover);
        c[ImGuiCol_ButtonActive] = rgb(p.accent_soft);
        c[ImGuiCol_CheckMark] = rgb(p.accent);
        c[ImGuiCol_SliderGrab] = rgb(p.accent, 0.85f);
        c[ImGuiCol_SliderGrabActive] = rgb(p.accent);
        c[ImGuiCol_Separator] = rgb(p.border);
        c[ImGuiCol_SeparatorHovered] = rgb(p.accent_soft);
        c[ImGuiCol_ScrollbarBg] = rgb(p.bg, 0.f);
        c[ImGuiCol_ScrollbarGrab] = rgb(p.frame_hover);
        c[ImGuiCol_ScrollbarGrabHovered] = rgb(p.accent_soft);
        c[ImGuiCol_TableRowBg] = {0, 0, 0, 0};
        c[ImGuiCol_TableRowBgAlt] = {1, 1, 1, 0.03f};
        c[ImGuiCol_ResizeGrip] = rgb(p.accent, 0.2f);
        c[ImGuiCol_Tab] = rgb(p.frame);
        c[ImGuiCol_TabHovered] = rgb(p.header_hover);
        c[ImGuiCol_TabSelected] = rgb(p.accent_soft);
        st.ScaleAllSizes(scale);
        ImGui::GetStyle() = st;
    }
    static ImVec4 accent_of(int theme) {
        static const uint32_t accents[3] = {0x5B9EFA, 0x2EC4B6, 0xA78BFA};
        return rgb(accents[std::clamp(theme, 0, 2)]);
    }
    int theme() const {
        if (const char* e = std::getenv("NR_CONTROL_THEME")) return std::clamp(std::atoi(e), 0, 2);   // tests
        return std::clamp(nr::parse_integer<int>(ini.get("NRControl", "Theme", "0"), 0), 0, 2);
    }

    // NR Control's size: the font by the screen's height (17 px at 1080 lines) times [NRControl] MenuScale, every
    // spacing with it. True when the font was made again: the backend makes its font texture again at its next frame.
    float font_px = 0.f;
    int theme_now = -1;
    bool fonts_for(uint32_t height) {
        if (const int t = theme(); t != theme_now && font_px > 0.f) { theme_now = t; apply_style(font_px / 17.f, t); }
        float scale = nr::parse_float(ini.get("NRControl", "MenuScale", "1"), 1.0f);
        if (!(scale >= 0.5f && scale <= 3.f)) scale = 1.f;
        const float px = std::clamp(std::round(float(height) / 1080.f * 17.f * scale), 12.f, 72.f);
        if (px == font_px) return false;
        font_px = px;
        auto& io = ImGui::GetIO();
        io.Fonts->Clear();
        ImFontConfig cfg;
        cfg.FontDataOwnedByAtlas = false;   // the embedded font stays ours
        cfg.OversampleH = 2;
        cfg.OversampleV = 1;
        io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(kMenuFont), int(kMenuFontSize), px, &cfg);
        theme_now = theme();
        apply_style(px / 17.f, theme_now);
        return true;
    }
    ~Menu() { if (context) ImGui::DestroyContext(context); }

    // This frame's input; whether anything is to be drawn (the menu, or with the menu closed the OSD when it is due).
    // The caller has made `context` current.
    bool input(uint32_t width, uint32_t height) {
        ++shared()->menu_seq;
        ++shared()->dll_menu;   // the Vulkan layer's menu steps aside
        auto& io = ImGui::GetIO();
        const auto now = std::chrono::steady_clock::now();
        io.DeltaTime = std::clamp(std::chrono::duration<float>(now - previous).count(), .001f, .1f);
        previous = now;
        io.DisplaySize = {float(width), float(height)};
        g_input.update(io, width, height);
        const bool osd = osd_due(now);   // also re-reads the file once a second (the marks with it)
        nr::pe::control::present_layers();
        nr::asi::wilsjo2_present_spatial();
        read_marks(width, height);
        if (std::getenv("NR_SPATIAL_TRACE")) {
            static int previous = -1;
            if (previous != int(marks.outlines)) {
                previous = int(marks.outlines);
                nr::pe::log("[nr] spatial outlines: %s at presentation %u", previous ? "drawn" : "absent", shared()->menu_seq);
            }
        }
        osd_shown = osd;
        osd_only = !g_input.open && (osd || marks.any());
        return g_input.open || osd_only;
    }

    // [Network] OSD with the menu closed: while a switch builds, its progress, then the network now running for 3 s
    // (switch, the default); the running network all the time (always); nothing (off). The mode is read from the file
    // once a second.
    bool osd_only = false, osd_shown = false;
    int osd_mode = 1;
    // [Inspect]'s marks (nr_control_pages.hpp draw_inspect_marks), from the file with the OSD's mode
    nr::control_ui::InspectMarks marks;
    nr::pe::ChainFile marks_chain_file;
    nr::pe::ChainConfig marks_chain;
    void read_marks(uint32_t width, uint32_t height) {
        marks = nr::control_ui::inspect_marks(ini);
        const std::string v = ini.get("Inspect", "SpatialOutlines", "0");
        if (v != "1" && v != "true") return;
        marks_chain_file.poll(std::string(nr::pe::module_folder()) + "\\dlssnr-amd.ini", marks_chain);
        const int chosen = std::max(1, marks_chain.inspect.layer);   // the final picture: the first layer's
        for (const nr::pe::LayerConfig& l : marks_chain.layers) {
            if (!l.enabled || std::max(l.number, 1) != chosen || l.processing != nr::pe::Processing::kWilsjo2) continue;
            // The layout recorded for this presented frame, including fallback and resize decisions.
            marks.outlines = nr::control_ui::spatial_rects(nr::asi::wilsjo2_spatial_layout(chosen), marks.centre, marks.work);
            break;
        }
    }
    uint32_t osd_want = ~0u, osd_running = ~0u;
    std::chrono::steady_clock::time_point osd_until{}, osd_read{};
    bool osd_due(std::chrono::steady_clock::time_point now) {
        if (now - osd_read > std::chrono::seconds(1)) {
            osd_read = now;
            if (ini.refresh()) loaded = -1;   // the menu loads the rest when it opens
            const std::string m = ini.get("Network", "OSD", "switch");
            osd_mode = m == "off" ? 0 : m == "always" ? 2 : m == "cost" ? 3 : 1;

        }
        const nr::control::Shared* s = shared();
        if (!s || !s->route || !s->net_known) return false;
        // The .asi's chain: the networks its layers run, together (the page's net_running is the last layer's)
        const uint32_t running = chain_rows(s) ? layer_networks(s) : s->net_running < nr::control::kNoNet ? s->net_running + 1u : ~0u;
        if (s->net_want != osd_want || (running != ~0u && running != osd_running)) {
            if (osd_want != ~0u && osd_running != ~0u && (s->net_want != osd_want || switched(osd_running, running)))
                osd_until = now + std::chrono::seconds(3);
            osd_want = s->net_want;
            if (running != ~0u) osd_running = running;
        }
        if (osd_mode != 0 && switching(s)) return true;   // a build's bar, the first one too (nothing has run yet)
        if (osd_mode == 0 || running == ~0u) return false;
        return osd_mode >= 2 || switching(s) || now < osd_until;
    }
    // The layers' running networks as one number (4 bits a layer by row; ~0u: none has run yet).
    static uint32_t layer_networks(const nr::control::Shared* s) {
        uint32_t v = 0;
        bool any = false;
        for (uint32_t i = 0; i < nr::control::kMaxLayers; ++i) {
            const auto& r = s->layers[i];
            if (!r.number || r.net >= nr::control::kNoNet) continue;
            v |= (r.net + 1u) << (4 * i);
            any = true;
        }
        return any ? v : ~0u;
    }
    // A network that changed under a layer (or the one instance); a layer that only started or went is no switch.
    static bool switched(uint32_t before, uint32_t after) {
        if (before == ~0u || after == ~0u) return false;
        for (int i = 0; i < 8; ++i) {
            const uint32_t a = (before >> (4 * i)) & 15u, b = (after >> (4 * i)) & 15u;
            if (a && b && a != b) return true;
        }
        return false;
    }
    static bool switching(const nr::control::Shared* s) { return nr::control_ui::osd_switching(s); }
    void draw_osd() {
        const nr::control::Shared* s = shared();
        ImGui::SetNextWindowPos({24, 24});
        ImGui::SetNextWindowBgAlpha(0.65f);
        ImGui::Begin("##nr-osd", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
        nr::control_ui::draw_osd_lines(s, osd_mode);
        ImGui::End();
    }

    // The menu's draw data (or the OSD's alone), after the backend's NewFrame.
    void build(uint32_t height) {
        if (osd_only) {
            auto& io = ImGui::GetIO();
            const bool cursor = io.MouseDrawCursor;
            io.MouseDrawCursor = false;
            ImGui::NewFrame();
            if (osd_shown) draw_osd();
            nr::control_ui::draw_inspect_marks(marks);
            ImGui::Render();
            io.MouseDrawCursor = cursor;
            return;
        }
        if (ini.refresh() || loaded < 0) { renodx.load(ini); loaded = loaded < 0 ? 1 : loaded + 1; }   // layer page reloads too
        // tests: NR_CONTROL_SET=<section.key=value,...> once at NR_CONTROL_AT; or a schedule of such groups,
        // "<draw>@<section.key=value,...>;<draw>@..." (each written once the menu has been drawn that often)
        static size_t set_next = 0;
        if (const char* e = std::getenv("NR_CONTROL_SET")) {
            std::vector<std::pair<uint64_t, std::string>> groups;
            {
                std::stringstream all(e);
                std::string g;
                while (std::getline(all, g, ';')) {
                    const auto at = g.find('@');
                    if (at == std::string::npos) groups.push_back({control_at(), g});
                    else groups.push_back({std::strtoull(g.substr(0, at).c_str(), nullptr, 10), g.substr(at + 1)});
                }
            }
            if (set_next < groups.size() && g_drawn >= groups[set_next].first) {
                const std::string& items = groups[set_next++].second;
                std::stringstream ss(items);
                std::string item;
                while (std::getline(ss, item, ',')) {
                    const auto dot = item.find('.'), eq = item.find('=');
                    if (dot != std::string::npos && eq != std::string::npos && dot < eq)
                        ini.set(item.substr(0, dot), item.substr(dot + 1, eq - dot - 1), item.substr(eq + 1));
                }
                ini.save();
                renodx.load(ini);
                say("NR_CONTROL_SET written at menu frame " + std::to_string(g_drawn) + ": " + items);
            }
        }
        static bool shot_done = false;
        if (const char* e = std::getenv("NR_CONTROL_SHOT"); e && e[0] == '1' && !shot_done && g_drawn >= control_at()) {
            shot_done = true;
            ++shared()->screenshot_request;
            say("NR_CONTROL_SHOT: the screenshot button pressed at menu frame " + std::to_string(g_drawn));
        }
        ImGui::NewFrame();
        const float u = font_px > 0.f ? font_px / 17.f : 1.f;
        ImGui::SetNextWindowPos({24 * u, 24 * u}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize({900 * u, std::min(float(height) - 48.f * u, 900.f * u)}, ImGuiCond_FirstUseEver);
        bool open = true;
        if (ImGui::Begin("NR Control", &open)) draw();
        ImGui::End();
        nr::control_ui::draw_inspect_marks(marks);
        ImGui::Render();
        if (!open) g_input.close();
        ini.save();
    }

    // NR on / off and the screenshot keys, NR Control's for every layer ([RenoDX], as the OptiScaler route keeps them)
    void draw_globals(const nr::control::Shared* s) {
        nr::control_ui::draw_nr_switch(ini, renodx, [s] { if (s) ++const_cast<nr::control::Shared*>(s)->screenshot_request; },
                                       s && s->screenshot[0] ? std::string(s->screenshot) : std::string());
    }

    // NR Control itself: its size ([NRControl] MenuScale)
    // ---- Diagnostics: what NR did (its events, the last stop) and what it runs, in one text for a bug report
    // (nr_control_pages.hpp) ----
    std::string report_said;
    void copy_report(const nr::control::Shared* s) {
        const std::string text = nr::control_ui::report_text(s, "dlssnr-amd.asi",
                                                             std::string(nr::pe::module_folder()) + "\\dlssnr-amd.ini",
                                                             nr::pe::log_path());
        report_said = nr::control_ui::copy_report(text, std::string(nr::pe::module_folder()) + "\\dlssnr-amd\\nr-report.txt");
    }
    void draw_diagnostics(const nr::control::Shared* s) {
        static const bool diag_test = [] { const char* e = std::getenv("NR_CONTROL_DIAG_OPEN"); return e && e[0] == '1'; }();
        if (diag_test) { ImGui::SetNextItemOpen(true); ImGui::SetScrollHereY(0.f); }   // tests: open, in view
        if (!ImGui::CollapsingHeader("Diagnostics")) return;
        nr::control_ui::draw_events();
        if (nr::control_ui::draw_key(renodx, nr::control_ui::key_index("NROutputRect"))) {
            renodx.store(ini); ini.save();
        }

        if (s && s->asi_build[0]) ImGui::TextDisabled("dlssnr-amd.asi %.*s", int(sizeof s->asi_build), s->asi_build);
        if (s && s->frame_gen[0]) ImGui::Text("%.*s", int(strnlen(s->frame_gen, sizeof s->frame_gen)), s->frame_gen);   // ImGui takes a "%.*s" length as it is (no stop at the NUL)
        if (ImGui::Button("Copy a report")) copy_report(s);
        nr::control_ui::help("For a bug report: the layers as they run, their engine data, the video memory and its "
                             "residency, the events above, dlssnr-amd.ini and the last lines of dlssnr-amd.log, as text. "
                             "Copied to the clipboard and saved as dlssnr-amd\\nr-report.txt next to the game. Nothing "
                             "is sent anywhere.");
        if (!report_said.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", report_said.c_str()); }
    }

    void draw_menu_settings() {
        if (!ImGui::CollapsingHeader("NR Control")) return;
        static float pending = -1.f;
        float v = pending > 0.f ? pending : nr::parse_float(ini.get("NRControl", "MenuScale", "1"), 1.0f);
        if (!(v >= 0.5f && v <= 3.f)) v = 1.f;
        if (ImGui::SliderFloat("Menu size", &v, 0.75f, 2.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp)) pending = v;
        if (ImGui::IsItemDeactivatedAfterEdit() && pending > 0.f) {
            char t[16];
            std::snprintf(t, sizeof t, "%.2f", double(pending));
            ini.set("NRControl", "MenuScale", t);
            pending = -1.f;
        }
        nr::control_ui::help("[NRControl] MenuScale. The menu's text and controls, on top of the size the screen gives "
                             "them (17 px text at 1080 lines). Taken when the slider is let go.");
        static const char* const themes[] = {"Midnight (blue)", "Graphite (teal)", "Plum (violet)"};
        int t = std::clamp(nr::parse_integer<int>(ini.get("NRControl", "Theme", "0"), 0), 0, 2);
        if (ImGui::Combo("Colours", &t, themes, 3)) ini.set("NRControl", "Theme", std::to_string(t));
    }

    // The .asi's NR chain as it runs, the video memory: nr_control_pages.hpp

    void draw() {
        const nr::control::Shared* s = shared();
        if (s && s->route) {
            ImGui::Text("Neural Rendering: %s", s->nr_on ? "on" : "off");
            if (s->width) {
                const bool before = s->placement == 2 ||
                                    (s->placement == 0 && s->render_width == s->width && s->render_height == s->height);
                const uint32_t ow = s->output_width ? s->output_width : before ? 0u : s->width;
                const uint32_t oh = s->output_width ? s->output_height : before ? 0u : s->height;
                if (s->render_width && ow && (s->render_width != ow || s->render_height != oh))
                    ImGui::Text("Upscaling: %ux%u -> %ux%u (%.2fx)", s->render_width, s->render_height, ow, oh,
                                double(oh) / double(std::max(s->render_height, 1u)));
                else if (s->render_width && ow)
                    ImGui::Text("No upscaling: %ux%u", ow, oh);
                else if (s->render_width)
                    ImGui::Text("Render: %ux%u", s->render_width, s->render_height);
                if (!chain_rows(s)) {
                    const char* where = s->placement == 1 ? " (after the upscaler)" : s->placement == 2 ? " (before the upscaler)" : "";
                    ImGui::Text("NR on %ux%u%s, network %ux%u, %u pass%s", s->width, s->height, where, s->nr_width,
                                s->nr_height, s->passes, s->passes == 1 ? "" : "es");
                }
            }
            if (chain_rows(s)) {
                draw_layer_status(s);
                nr::control_ui::draw_flapping();
                nr::control_ui::draw_missing_data(s->layers, nr::control::kMaxLayers, &ini);
            }
            else ImGui::TextDisabled("No layer is on (the NR chain below): NR does not run.");
            if (s->net_running < nr::control::kNoNet && !chain_rows(s)) ImGui::Text("Network: %s", nr::network_codename(int(s->net_running)));
            if (s->building) ImGui::TextDisabled("Building a network...");
            if (s->state[0]) ImGui::TextWrapped("%s", s->state);
            if (s->frame_gen[0]) ImGui::Text("%.*s", int(strnlen(s->frame_gen, sizeof s->frame_gen)), s->frame_gen);   // ImGui takes a "%.*s" length as it is (no stop at the NUL)
            draw_vram(s);
            nr::control_ui::draw_residency();
        } else {
            ImGui::TextDisabled("Waiting for the game's upscaler: DLSS, FSR or XeSS in the game's settings (OptiScaler's "
                                "own menu, for the upscaler and frame generation: Insert).");
        }
        ImGui::Separator();
        chain.draw_chain(s);
        chain.draw_layer_page(s, loaded, accent_of(theme_now));
        // tests (NR_CONTROL_ENGINE_OPEN=1): the window scrolled to the layer page's end, its engine data in view
        static const bool engine_test = [] { const char* e = std::getenv("NR_CONTROL_ENGINE_OPEN"); return e && e[0] == '1'; }();
        if (engine_test) ImGui::SetScrollHereY(1.0f);
        if (const char* y = std::getenv("NR_CONTROL_SCROLL")) ImGui::SetScrollY(std::max(0.f, float(std::atof(y))));   // tests
        ImGui::Spacing();
        ImGui::SeparatorText("Global");
        if (std::getenv("NR_CONTROL_GLOBAL_OPEN")) ImGui::SetScrollHereY(0.f);   // tests
        nr::control_ui::draw_preprocess(ini);
        {   // HUD FIX: what it does now and why a method cannot run (the DLL's lines)
            nr::control_ui::HudFixMenu hm;
            if (s) {
                for (int i = 0; i < 3; ++i) hm.why[2 + i] = std::string(s->hud_why[i], strnlen(s->hud_why[i], sizeof s->hud_why[i]));
                hm.now = std::string(s->hud_now, strnlen(s->hud_now, sizeof s->hud_now));
            }
            nr::control_ui::draw_hudfix(ini, hm);
        }
        nr::control_ui::draw_network_options(ini, s, nr::control_ui::NetworkOptions{});
        // What the game hands NR, and who covers what it does not (the DLL's lines, nr_engine_panel.hpp)
        if (s && s->route && s->panel_lines && !chain_rows(s)) {
            if (const char* e = std::getenv("NR_CONTROL_GLOBAL_OPEN")) ImGui::SetNextItemOpen(e[0] != '0');   // tests
            if (ImGui::CollapsingHeader("Engine data")) {
                nr::control_ui::engine_legend();
                for (uint32_t i = 0; i < std::min(s->panel_lines, nr::control::kPanelLines); ++i) {
                    char line[sizeof s->panel[0] + 1];
                    std::snprintf(line, sizeof line, "%s", s->panel[i]);
                    nr::control_ui::engine_line(line);
                }
            }
        }
        draw_globals(s);
        draw_menu_settings();
        draw_diagnostics(s);
        // tests (NR_CONTROL_REPORT_AT=<draws>): the report made once, as its button would
        static int report_at = [] { const char* e = std::getenv("NR_CONTROL_REPORT_AT"); return e ? std::atoi(e) : 0; }();
        if (report_at > 0 && --report_at == 0) copy_report(s);
        // tests (NR_CONTROL_REPORT_EVERY=<draws>): the report appended to nr-report-series.txt beside dlssnr-amd.ini that often
        static const int report_every = [] { const char* e = std::getenv("NR_CONTROL_REPORT_EVERY"); return e ? std::atoi(e) : 0; }();
        static int draws = 0;
        if (report_every > 0 && ++draws % report_every == 0) {
            const std::string text = nr::control_ui::report_text(s, "dlssnr-amd.asi",
                                                                 std::string(nr::pe::module_folder()) + "\\dlssnr-amd.ini",
                                                                 nr::pe::log_path());
            if (FILE* f = std::fopen((std::string(nr::pe::module_folder()) + "\\nr-report-series.txt").c_str(), "ab")) {
                std::fprintf(f, "--- draw %d\n%s\n", draws, text.c_str());
                std::fclose(f);
            }
        }
        nr::control_ui::draw_inspect(ini, "What is compared. The final picture: the layers of the last place that runs "
                                          "(Present, else after the upscaler), all off against all on. A layer: its own "
                                          "input and output, where it runs (before the upscaler: the comparison is "
                                          "upscaled too).");
        nr::control_ui::draw_log(ini);
        ImGui::Separator();
        ImGui::TextDisabled("End: close. Changes are saved to dlssnr-amd.ini at once.");
    }
};

// ---- how the menu is laid over the back buffer ----
// The menu is drawn into an 8-bit image as for an SDR screen, then written into the back buffer in the swap chain's
// own encoding. mode 0: as it is (UNORM, SDR); 1: an _SRGB format (the hardware encodes, so linear here); 2: scRGB
// (linear, 1.0 = 80 nits; scale = white in those units); 3: HDR10 (BT.2020 primaries, PQ; scale = white / 10000
// nits). Blended over the game's colour with the menu's coverage; the game's alpha is kept. (menu_compose.frag of the
// Linux layer, in HLSL.)
const char kComposeHlsl[] = R"(
Texture2D<float4> menu : register(t0);
cbuffer Push : register(b0) { uint mode; float scale; };
float4 vs(uint id : SV_VertexID) : SV_Position {
    float2 p = float2((id << 1) & 2, id & 2);
    return float4(p * 2.0 - 1.0, 0.0, 1.0);
}
float3 srgb_to_linear(float3 c) {
    return c > 0.04045 ? pow((c + 0.055) / 1.055, 2.4) : c / 12.92;
}
float3 pq(float3 n) {
    const float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    float3 p = pow(max(n, 0.0), m1);
    return pow((c1 + c2 * p) / (1.0 + c3 * p), m2);
}
float4 ps(float4 pos : SV_Position) : SV_Target {
    float4 m = menu.Load(int3(int2(pos.xy), 0));
    if (m.a <= 0.0) discard;
    float3 c = saturate(m.rgb / m.a);
    if (mode == 1u) {
        c = srgb_to_linear(c);
    } else if (mode == 2u) {
        c = srgb_to_linear(c) * scale;
    } else if (mode == 3u) {
        float3 l = srgb_to_linear(c);
        float3 w = float3(dot(float3(0.627404, 0.329283, 0.043313), l),
                          dot(float3(0.069097, 0.919540, 0.011362), l),
                          dot(float3(0.016391, 0.088013, 0.895595), l));
        c = pq(w * scale);
    }
    return float4(c * m.a, m.a);
}
)";

struct Encoding { uint32_t mode; float scale; const char* name; };

std::mutex g_spaces_mutex;
std::unordered_map<IDXGISwapChain*, DXGI_COLOR_SPACE_TYPE> g_spaces;   // what the game set (SetColorSpace1)

// The swap chain's encoding: the colour space the game set if we saw it, else what its format and the display say
// (a float swap chain is scRGB; a 10-bit one on an HDR display taken as HDR10).
Encoding encoding_of(IDXGISwapChain* sc, DXGI_FORMAT format) {
    constexpr float kWhiteNits = 203.f;   // the menu's white in HDR (BT.2408's graphics white)
    bool known = false;
    DXGI_COLOR_SPACE_TYPE space = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    {
        std::lock_guard<std::mutex> lock(g_spaces_mutex);
        const auto it = g_spaces.find(sc);
        if (it != g_spaces.end()) { space = it->second; known = true; }
    }
    if (!known) {
        if (format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
            space = DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
        } else if (format == DXGI_FORMAT_R10G10B10A2_UNORM) {
            IDXGIOutput* out = nullptr;
            IDXGIOutput6* out6 = nullptr;
            DXGI_OUTPUT_DESC1 d{};
            if (SUCCEEDED(sc->GetContainingOutput(&out)) && out && SUCCEEDED(out->QueryInterface(IID_PPV_ARGS(&out6))) &&
                out6 && SUCCEEDED(out6->GetDesc1(&d)) && d.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020)
                space = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
            release(out6);
            release(out);
        }
    }
    if (space == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709) return {2, kWhiteNits / 80.f, "scRGB, white at 203 nits"};
    if (space == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) return {3, kWhiteNits / 10000.f, "HDR10 (PQ), white at 203 nits"};
    switch (format) {
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
            return {1, 1.f, "sRGB format"};
        default: return {0, 1.f, "SDR"};
    }
}

ID3DBlob* compile(const char* entry, const char* target) {
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    const HRESULT hr = D3DCompile(kComposeHlsl, sizeof kComposeHlsl - 1, "nr_menu_compose", nullptr, nullptr, entry,
                                  target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    std::string why = errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()) : "";
    release(errors);
    if (FAILED(hr) || !code) throw std::runtime_error("the composite shader " + std::string(entry) + " did not compile: " + why);
    return code;
}

void check(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        char t[160];
        std::snprintf(t, sizeof t, "%s failed (0x%08lx)", what, static_cast<unsigned long>(hr));
        throw std::runtime_error(t);
    }
}

// ---- NR_CONTROL_DUMP: one drawn back buffer as a PPM ----
struct Dump {
    std::string path;
    bool done = false;
};
Dump g_dump{[] { const char* e = std::getenv("NR_CONTROL_DUMP"); return std::string(e ? e : ""); }()};
bool dump_wanted() { return !g_dump.path.empty() && !g_dump.done && g_drawn > control_at(); }

float half_to_float(uint16_t h) {
    const uint32_t e = (h >> 10) & 31u, m = h & 1023u;
    const float v = e == 0 ? std::ldexp(float(m), -24) : e == 31 ? 65504.f : std::ldexp(float(m | 1024u), int(e) - 25);
    return (h & 0x8000u) ? -v : v;
}
// One pixel as 8-bit sRGB for a look: HDR is shown with 203 nits as white (clipped above).
void dump_pixel(const uint8_t* px, DXGI_FORMAT format, uint32_t mode, uint8_t rgb[3]) {
    float c[3];
    auto oetf = [](float v) {
        v = std::clamp(v, 0.f, 1.f);
        return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.f / 2.4f) - 0.055f;
    };
    auto pq_nits = [](float e) {
        const float m1 = 0.1593017578125f, m2 = 78.84375f, c1 = 0.8359375f, c2 = 18.8515625f, c3 = 18.6875f;
        const float p = std::pow(std::max(e, 0.f), 1.f / m2);
        return 10000.f * std::pow(std::max(p - c1, 0.f) / (c2 - c3 * p), 1.f / m1);
    };
    switch (format) {
        case DXGI_FORMAT_R16G16B16A16_FLOAT: {
            uint16_t h[3];
            std::memcpy(h, px, sizeof h);
            for (int i = 0; i < 3; ++i)
                c[i] = mode == 2 ? oetf(half_to_float(h[i]) * 80.f / 203.f) : std::clamp(half_to_float(h[i]), 0.f, 1.f);
            break;
        }
        case DXGI_FORMAT_R10G10B10A2_UNORM: {
            uint32_t w;
            std::memcpy(&w, px, 4);
            const float v[3] = {float(w & 1023u) / 1023.f, float((w >> 10) & 1023u) / 1023.f, float((w >> 20) & 1023u) / 1023.f};
            for (int i = 0; i < 3; ++i) c[i] = mode == 3 ? oetf(pq_nits(v[i]) / 203.f) : v[i];   // BT.2020 primaries kept
            break;
        }
        default: {
            const bool bgra = format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
                              format == DXGI_FORMAT_B8G8R8X8_UNORM || format == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
            for (int i = 0; i < 3; ++i) c[i] = px[bgra ? 2 - i : i] / 255.f;
        }
    }
    for (int i = 0; i < 3; ++i) rgb[i] = uint8_t(std::lround(std::clamp(c[i], 0.f, 1.f) * 255.f));
}
void dump_write(const uint8_t* data, uint32_t pitch, uint32_t w, uint32_t h, DXGI_FORMAT format, uint32_t mode) {
    g_dump.done = true;
    FILE* f = std::fopen(g_dump.path.c_str(), "wb");
    if (!f) { say("NR_CONTROL_DUMP: cannot write " + g_dump.path); return; }
    std::fprintf(f, "P6\n%u %u\n255\n", w, h);
    const size_t step = format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8 : 4;
    std::vector<uint8_t> row(size_t(w) * 3);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) dump_pixel(data + size_t(y) * pitch + x * step, format, mode, &row[size_t(x) * 3]);
        std::fwrite(row.data(), 1, row.size(), f);
    }
    std::fclose(f);
    say("drawn frame written to " + g_dump.path + " (DXGI format " + std::to_string(int(format)) + ", mode " +
        std::to_string(mode) + ")");
}

// ---- D3D11 ----
struct D3D11Overlay {
    ID3D11Device* device{};
    ID3D11Device1* device1{};
    ID3D11DeviceContext* context{};
    ID3D11DeviceContext1* context1{};
    ID3DDeviceContextState* state{};   // our own pipeline state: the game's is swapped out and back whole
    ID3D11Texture2D* menu{};
    ID3D11RenderTargetView* menu_rtv{};
    ID3D11ShaderResourceView* menu_srv{};
    ID3D11VertexShader* vs{};
    ID3D11PixelShader* ps{};
    ID3D11BlendState* blend{};
    ID3D11RasterizerState* raster{};
    ID3D11Buffer* constants{};
    uint32_t width{}, height{};
    Menu menu_ui;
    bool backend = false;

    explicit D3D11Overlay(ID3D11Device* d) : device(d) {
        device->AddRef();
        device->GetImmediateContext(&context);
        check(device->QueryInterface(IID_PPV_ARGS(&device1)), "ID3D11Device1");
        check(context->QueryInterface(IID_PPV_ARGS(&context1)), "ID3D11DeviceContext1");
        const D3D_FEATURE_LEVEL level = device->GetFeatureLevel();
        const UINT flags = (device->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED)
                               ? D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED : 0;
        check(device1->CreateDeviceContextState(flags, &level, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device1), nullptr,
                                                &state), "CreateDeviceContextState");
        ID3DBlob* v = compile("vs", "vs_5_0");
        const HRESULT vr = device->CreateVertexShader(v->GetBufferPointer(), v->GetBufferSize(), nullptr, &vs);
        v->Release();
        check(vr, "the composite vertex shader");
        ID3DBlob* p = compile("ps", "ps_5_0");
        const HRESULT pr = device->CreatePixelShader(p->GetBufferPointer(), p->GetBufferSize(), nullptr, &ps);
        p->Release();
        check(pr, "the composite pixel shader");
        D3D11_BLEND_DESC bd{};
        auto& rt = bd.RenderTarget[0];
        rt.BlendEnable = TRUE;   // premultiplied over the game's colour; the game's alpha kept
        rt.SrcBlend = D3D11_BLEND_ONE; rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA; rt.BlendOp = D3D11_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D11_BLEND_ZERO; rt.DestBlendAlpha = D3D11_BLEND_ONE; rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        check(device->CreateBlendState(&bd, &blend), "the blend state");
        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
        check(device->CreateRasterizerState(&rd, &raster), "the rasterizer state");
        D3D11_BUFFER_DESC cd{};
        cd.ByteWidth = 16; cd.Usage = D3D11_USAGE_DEFAULT; cd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        check(device->CreateBuffer(&cd, nullptr, &constants), "the constant buffer");
        ImGui::SetCurrentContext(menu_ui.context);
        // ImGui's backend in our own state, so what it creates and binds never reaches the game's.
        ID3DDeviceContextState* game = nullptr;
        context1->SwapDeviceContextState(state, &game);
        backend = ImGui_ImplDX11_Init(device, context);
        context1->SwapDeviceContextState(game, nullptr);
        release(game);
        if (!backend) throw std::runtime_error("ImGui's D3D11 backend did not start");
    }
    ~D3D11Overlay() {
        ImGui::SetCurrentContext(menu_ui.context);
        if (backend) ImGui_ImplDX11_Shutdown();
        free_menu();
        release(constants); release(raster); release(blend); release(ps); release(vs);
        release(state); release(context1); release(context); release(device1); release(device);
    }
    void free_menu() { release(menu_srv); release(menu_rtv); release(menu); }
    void ensure_menu(uint32_t w, uint32_t h) {
        if (menu && w == width && h == height) return;
        free_menu();
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        check(device->CreateTexture2D(&td, nullptr, &menu), "the menu image");
        check(device->CreateRenderTargetView(menu, nullptr, &menu_rtv), "the menu's render target");
        check(device->CreateShaderResourceView(menu, nullptr, &menu_srv), "the menu's view");
        width = w; height = h;
    }

    void present(IDXGISwapChain* sc) {
        DXGI_SWAP_CHAIN_DESC desc{};
        check(sc->GetDesc(&desc), "GetDesc");
        const uint32_t w = desc.BufferDesc.Width, h = desc.BufferDesc.Height;
        if (!w || !h) return;
        ImGui::SetCurrentContext(menu_ui.context);
        if (!menu_ui.input(w, h)) { trace("closed"); return; }
        ID3D11Texture2D* back = nullptr;
        check(sc->GetBuffer(0, IID_PPV_ARGS(&back)), "GetBuffer");
        ID3DDeviceContextState* game = nullptr;
        context1->SwapDeviceContextState(state, &game);
        ID3D11RenderTargetView* rtv = nullptr;
        try {
            ensure_menu(w, h);
            if (menu_ui.fonts_for(h)) ImGui_ImplDX11_InvalidateDeviceObjects();
            ImGui_ImplDX11_NewFrame();
            menu_ui.build(h);
            const float clear[4] = {0, 0, 0, 0};
            context->ClearRenderTargetView(menu_rtv, clear);
            context->OMSetRenderTargets(1, &menu_rtv, nullptr);
            D3D11_VIEWPORT vp{0, 0, float(w), float(h), 0, 1};
            context->RSSetViewports(1, &vp);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            // over the back buffer
            check(device->CreateRenderTargetView(back, nullptr, &rtv), "the back buffer's render target");
            const Encoding enc = encoding_of(sc, desc.BufferDesc.Format);
            const struct { uint32_t mode; float scale; uint32_t pad[2]; } push{enc.mode, enc.scale, {}};
            context->UpdateSubresource(constants, 0, nullptr, &push, 0, 0);
            context->OMSetRenderTargets(1, &rtv, nullptr);
            context->OMSetBlendState(blend, nullptr, 0xffffffffu);
            context->OMSetDepthStencilState(nullptr, 0);
            context->RSSetState(raster);
            context->RSSetViewports(1, &vp);
            context->IASetInputLayout(nullptr);
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            context->VSSetShader(vs, nullptr, 0);
            context->PSSetShader(ps, nullptr, 0);
            context->PSSetShaderResources(0, 1, &menu_srv);
            context->PSSetConstantBuffers(0, 1, &constants);
            context->Draw(3, 0);
            ID3D11ShaderResourceView* none = nullptr;
            context->PSSetShaderResources(0, 1, &none);
            ++g_drawn;
            trace_drawn();
            if (dump_wanted()) dump(back, desc.BufferDesc.Format, enc.mode);
        } catch (...) {
            context->OMSetRenderTargets(0, nullptr, nullptr);
            release(rtv);
            context1->SwapDeviceContextState(game, nullptr);
            release(game);
            release(back);
            throw;
        }
        // Our context state keeps what is bound in it while the game's is current: a back buffer view left bound
        // there is a reference the game's ResizeBuffers fails on (window size, fullscreen switch).
        context->OMSetRenderTargets(0, nullptr, nullptr);
        release(rtv);
        context1->SwapDeviceContextState(game, nullptr);
        release(game);
        release(back);
    }
    void dump(ID3D11Texture2D* back, DXGI_FORMAT format, uint32_t mode) {
        D3D11_TEXTURE2D_DESC td{};
        back->GetDesc(&td);
        td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ; td.MiscFlags = 0;
        ID3D11Texture2D* staging = nullptr;
        if (FAILED(device->CreateTexture2D(&td, nullptr, &staging))) { g_dump.done = true; return; }
        context->CopyResource(staging, back);
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(context->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
            dump_write(static_cast<const uint8_t*>(m.pData), m.RowPitch, td.Width, td.Height, format, mode);
            context->Unmap(staging, 0);
        }
        g_dump.done = true;
        staging->Release();
    }
};

// ---- D3D12 ----
struct D3D12Overlay {
    static constexpr uint32_t kFrames = 3, kSrvs = 64;
    ID3D12Device* device{};
    ID3D12CommandQueue* queue{};   // the swap chain's: the menu goes in after the game's frame, before the Present
    ID3D12CommandAllocator* allocators[kFrames]{};
    uint64_t done_at[kFrames]{};
    ID3D12GraphicsCommandList* list{};
    ID3D12Fence* fence{};
    uint64_t fence_value{};
    HANDLE event{};
    uint32_t frame{};
    ID3D12DescriptorHeap* rtvs{};   // 0 the menu, 1 the back buffer
    ID3D12DescriptorHeap* srvs{};   // 0 the menu (the composite), 1.. ImGui's textures
    uint32_t rtv_step{}, srv_step{};
    std::vector<bool> srv_used = std::vector<bool>(kSrvs, false);
    ID3D12Resource* menu{};
    uint32_t width{}, height{};
    ID3D12RootSignature* root{};
    std::map<DXGI_FORMAT, ID3D12PipelineState*> pipelines;   // per back-buffer format
    ID3DBlob* vs_code{};
    ID3DBlob* ps_code{};
    Menu menu_ui;
    bool backend = false;

    D3D12Overlay(ID3D12Device* d, ID3D12CommandQueue* q) : device(d), queue(q) {
        device->AddRef();
        queue->AddRef();
        for (auto& a : allocators)
            check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a)), "a command allocator");
        check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators[0], nullptr, IID_PPV_ARGS(&list)),
              "the command list");
        list->Close();
        check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "the fence");
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = 2;
        check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvs)), "the render-target heap");
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = kSrvs;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&srvs)), "the shader-resource heap");
        rtv_step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        srv_step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        srv_used[0] = true;
        // the composite: the menu's SRV (t0) and two root constants (b0)
        D3D12_DESCRIPTOR_RANGE range{};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; range.NumDescriptors = 1;
        D3D12_ROOT_PARAMETER params[2]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable.NumDescriptorRanges = 1; params[0].DescriptorTable.pDescriptorRanges = &range;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[1].Constants.Num32BitValues = 2;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC rd{};
        rd.NumParameters = 2; rd.pParameters = params;
        ID3DBlob* blob = nullptr;
        ID3DBlob* errors = nullptr;
        const HRESULT sr = D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors);
        release(errors);
        check(sr, "the root signature's serialisation");
        const HRESULT rr = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root));
        blob->Release();
        check(rr, "the root signature");
        vs_code = compile("vs", "vs_5_0");
        ps_code = compile("ps", "ps_5_0");
        ImGui::SetCurrentContext(menu_ui.context);
        ImGui_ImplDX12_InitInfo init;
        init.Device = device;
        init.CommandQueue = queue;
        init.NumFramesInFlight = int(kFrames);
        init.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        init.DSVFormat = DXGI_FORMAT_UNKNOWN;
        init.UserData = this;
        init.SrvDescriptorHeap = srvs;
        init.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE* cpu,
                                       D3D12_GPU_DESCRIPTOR_HANDLE* gpu) {
            auto* o = static_cast<D3D12Overlay*>(info->UserData);
            for (uint32_t i = 1; i < kSrvs; ++i)
                if (!o->srv_used[i]) {
                    o->srv_used[i] = true;
                    *cpu = o->srv_cpu(i);
                    *gpu = o->srv_gpu(i);
                    return;
                }
            *cpu = o->srv_cpu(kSrvs - 1);   // out of slots: ImGui's few textures never get here
            *gpu = o->srv_gpu(kSrvs - 1);
        };
        init.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE cpu,
                                      D3D12_GPU_DESCRIPTOR_HANDLE) {
            auto* o = static_cast<D3D12Overlay*>(info->UserData);
            const uint32_t i = uint32_t((cpu.ptr - o->srvs->GetCPUDescriptorHandleForHeapStart().ptr) / o->srv_step);
            if (i < kSrvs) o->srv_used[i] = false;
        };
        backend = ImGui_ImplDX12_Init(&init);
        if (!backend) throw std::runtime_error("ImGui's D3D12 backend did not start");
    }
    ~D3D12Overlay() {
        if (!wait_all()) {   // the GPU may still run our last list: its objects are left as they are (kept, not freed)
            say("the menu's last list did not finish in 2 s; its objects are kept, not freed");
            return;
        }
        ImGui::SetCurrentContext(menu_ui.context);
        if (backend) ImGui_ImplDX12_Shutdown();
        for (auto& p : pipelines) p.second->Release();
        release(vs_code); release(ps_code); release(root); release(menu); release(srvs); release(rtvs);
        if (event) CloseHandle(event);
        release(fence); release(list);
        for (auto& a : allocators) release(a);
        release(queue); release(device);
    }
    D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu(uint32_t i) const {
        D3D12_CPU_DESCRIPTOR_HANDLE h = srvs->GetCPUDescriptorHandleForHeapStart();
        h.ptr += SIZE_T(i) * srv_step;
        return h;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu(uint32_t i) const {
        D3D12_GPU_DESCRIPTOR_HANDLE h = srvs->GetGPUDescriptorHandleForHeapStart();
        h.ptr += UINT64(i) * srv_step;
        return h;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE rtv(uint32_t i) const {
        D3D12_CPU_DESCRIPTOR_HANDLE h = rtvs->GetCPUDescriptorHandleForHeapStart();
        h.ptr += SIZE_T(i) * rtv_step;
        return h;
    }
    // Whether the GPU is past `value` (waited for up to 2 s): what it still runs is not reset or released when not.
    bool wait(uint64_t value) {
        if (!value || fence->GetCompletedValue() >= value) return true;
        if (SUCCEEDED(fence->SetEventOnCompletion(value, event))) WaitForSingleObject(event, 2000);
        if (fence->GetCompletedValue() >= value) return true;
        static bool said = false;
        if (!said) {
            said = true;
            log("[nr] NR Control: its last drawing is still on the GPU after 2 s; the menu skips Presents until it is done");
        }
        return false;
    }
    bool wait_all() { return !fence || wait(fence_value); }
    bool ensure_menu(uint32_t w, uint32_t h) {
        if (menu && w == width && h == height) return true;
        if (!wait_all()) return false;
        release(menu);
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; rd.Width = w; rd.Height = h; rd.DepthOrArraySize = 1;
        rd.MipLevels = 1; rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; rd.SampleDesc.Count = 1;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE clear{};
        clear.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                              &clear, IID_PPV_ARGS(&menu)), "the menu image");
        nr::vram::track_d3d12(device, menu, nr::vram::kMenu);
        device->CreateRenderTargetView(menu, nullptr, rtv(0));
        device->CreateShaderResourceView(menu, nullptr, srv_cpu(0));
        width = w; height = h;
        return true;
    }
    ID3D12PipelineState* pipeline(DXGI_FORMAT format) {
        const auto it = pipelines.find(format);
        if (it != pipelines.end()) return it->second;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = root;
        pd.VS = {vs_code->GetBufferPointer(), vs_code->GetBufferSize()};
        pd.PS = {ps_code->GetBufferPointer(), ps_code->GetBufferSize()};
        auto& rt = pd.BlendState.RenderTarget[0];
        rt.BlendEnable = TRUE;   // premultiplied over the game's colour; the game's alpha kept
        rt.SrcBlend = D3D12_BLEND_ONE; rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA; rt.BlendOp = D3D12_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D12_BLEND_ZERO; rt.DestBlendAlpha = D3D12_BLEND_ONE; rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        rt.LogicOp = D3D12_LOGIC_OP_NOOP;
        rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.SampleMask = UINT_MAX;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = format;
        pd.SampleDesc.Count = 1;
        ID3D12PipelineState* p = nullptr;
        check(device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&p)), "the composite pipeline");
        pipelines[format] = p;
        return p;
    }
    static void transition(ID3D12GraphicsCommandList* l, ID3D12Resource* r, D3D12_RESOURCE_STATES from,
                           D3D12_RESOURCE_STATES to) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = r;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = from;
        b.Transition.StateAfter = to;
        l->ResourceBarrier(1, &b);
    }

    void present(IDXGISwapChain* sc) {
        DXGI_SWAP_CHAIN_DESC desc{};
        check(sc->GetDesc(&desc), "GetDesc");
        const uint32_t w = desc.BufferDesc.Width, h = desc.BufferDesc.Height;
        if (!w || !h) return;
        ImGui::SetCurrentContext(menu_ui.context);
        if (!menu_ui.input(w, h)) { trace("closed"); return; }
        IDXGISwapChain3* sc3 = nullptr;
        check(sc->QueryInterface(IID_PPV_ARGS(&sc3)), "IDXGISwapChain3");
        const UINT index = sc3->GetCurrentBackBufferIndex();
        sc3->Release();
        ID3D12Resource* back = nullptr;
        check(sc->GetBuffer(index, IID_PPV_ARGS(&back)), "GetBuffer");
        struct Hold { ID3D12Resource*& r; ~Hold() { release(r); } } hold{back};
        if (!ensure_menu(w, h)) { trace("skipped: the menu image"); return; }
        const Encoding enc = encoding_of(sc, desc.BufferDesc.Format);
        ID3D12PipelineState* compose = pipeline(desc.BufferDesc.Format);
        if (!wait(done_at[frame])) { trace("skipped: the slot on the GPU"); return; }   // this slot's list is still on the GPU: no menu on this Present
        if (menu_ui.fonts_for(h)) {   // the old font texture may still be read by a frame on the GPU
            if (!wait_all()) { trace("skipped: the font"); return; }
            ImGui_ImplDX12_InvalidateDeviceObjects();
        }
        ImGui_ImplDX12_NewFrame();
        menu_ui.build(h);
        check(allocators[frame]->Reset(), "the command allocator's reset");
        check(list->Reset(allocators[frame], nullptr), "the command list's reset");
        // ImGui into the menu image, from transparent black
        transition(list, menu, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const float clear[4] = {0, 0, 0, 0};
        const D3D12_CPU_DESCRIPTOR_HANDLE menu_rtv = rtv(0), back_rtv = rtv(1);
        list->ClearRenderTargetView(menu_rtv, clear, 0, nullptr);
        list->OMSetRenderTargets(1, &menu_rtv, FALSE, nullptr);
        list->SetDescriptorHeaps(1, &srvs);
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), list);
        transition(list, menu, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        // over the back buffer, which is in PRESENT (COMMON) at a Present
        device->CreateRenderTargetView(back, nullptr, back_rtv);
        transition(list, back, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        list->OMSetRenderTargets(1, &back_rtv, FALSE, nullptr);
        D3D12_VIEWPORT vp{0, 0, float(w), float(h), 0, 1};
        D3D12_RECT scissor{0, 0, LONG(w), LONG(h)};
        list->RSSetViewports(1, &vp);
        list->RSSetScissorRects(1, &scissor);
        list->SetPipelineState(compose);
        list->SetGraphicsRootSignature(root);
        list->SetDescriptorHeaps(1, &srvs);
        list->SetGraphicsRootDescriptorTable(0, srv_gpu(0));
        const uint32_t push[2] = {enc.mode, 0};
        list->SetGraphicsRoot32BitConstant(1, push[0], 0);
        uint32_t scale_bits;
        std::memcpy(&scale_bits, &enc.scale, 4);
        list->SetGraphicsRoot32BitConstant(1, scale_bits, 1);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->DrawInstanced(3, 1, 0, 0);
        ++g_drawn;
        trace_drawn();
        // NR_CONTROL_DUMP: the drawn back buffer into a readback buffer
        ID3D12Resource* readback = nullptr;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
        const bool dumping = dump_wanted();
        if (dumping) {
            const D3D12_RESOURCE_DESC bd = back->GetDesc();
            UINT64 bytes = 0;
            device->GetCopyableFootprints(&bd, 0, 1, 0, &layout, nullptr, nullptr, &bytes);
            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC rd{};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = bytes; rd.Height = 1; rd.DepthOrArraySize = 1;
            rd.MipLevels = 1; rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                          nullptr, IID_PPV_ARGS(&readback)))) {
                transition(list, back, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
                D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
                dst.pResource = readback; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = layout;
                src.pResource = back; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex = 0;
                list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                transition(list, back, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
            } else {
                g_dump.done = true;
                transition(list, back, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
            }
        } else {
            transition(list, back, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        }
        check(list->Close(), "the command list's close");
        ID3D12CommandList* lists[] = {list};
        queue->ExecuteCommandLists(1, lists);
        // the slot's completion value set before the signal: if the signal fails, the fence never reaches it and the
        // slot's allocator is not reused (wait() times out) rather than reset under the list just executed
        done_at[frame] = fence_value + 1;
        check(queue->Signal(fence, ++fence_value), "the fence signal");
        frame = (frame + 1) % kFrames;
        if (readback) {
            if (!wait(fence_value)) {   // a test's dump only: not read, and the buffer left to the GPU (kept)
                say("NR_CONTROL_DUMP: the copy did not finish in 2 s; no dump");
                g_dump.done = true;
                return;
            }
            void* p = nullptr;
            const D3D12_RANGE range{0, SIZE_T(layout.Footprint.RowPitch) * h};
            if (SUCCEEDED(readback->Map(0, &range, &p))) {
                dump_write(static_cast<const uint8_t*>(p) + layout.Offset, layout.Footprint.RowPitch, w, h,
                           desc.BufferDesc.Format, enc.mode);
                const D3D12_RANGE none{0, 0};
                readback->Unmap(0, &none);
            }
            g_dump.done = true;
            readback->Release();
        }
    }
};

// ---- the queue a D3D12 swap chain presents on ----
// Under Proton DXVK's DXGI gives a D3D12 swap chain's device, not its queue: the queue is the direct queue this thread
// last executed command lists on (the frame's). ID3D12CommandQueue::ExecuteCommandLists of the game's queue class is
// hooked to know it (watch_queues).
using ExecuteFn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
ExecuteFn g_execute = nullptr;
std::atomic<ExecuteObserver> g_execute_observer{nullptr};   // on_execute
thread_local ID3D12CommandQueue* t_last_direct = nullptr;
thread_local ID3D12CommandQueue* t_last_seen = nullptr;
void STDMETHODCALLTYPE execute_hook(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists) {
    if (q != t_last_seen) {   // asked once per change of queue on this thread
        t_last_seen = q;
        if (q->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT) t_last_direct = q;
    }
    g_execute(q, n, lists);
    if (const ExecuteObserver observer = g_execute_observer.load()) observer(q, n, lists);
}
ID3D12CommandQueue* last_direct_queue() { return t_last_direct; }
std::atomic<D3D12PresentHook> g_d3d12_hook{nullptr};   // on_d3d12_present
std::atomic<D3D11PresentHook> g_d3d11_hook{nullptr};   // on_d3d11_present

// ---- the hooks ----
std::mutex g_mutex;   // one Present at a time draws the menu
std::atomic<uint64_t> g_presents{0};   // Presents that reached the hooks
// One overlay per D3D11 device / D3D12 queue that presents (each holds a reference to it): one not presented on for
// kIdle is let go, so a device the game replaced is not kept alive by the menu.
template <class T> struct Kept { std::unique_ptr<T> overlay; std::chrono::steady_clock::time_point used; };
std::map<void*, Kept<D3D11Overlay>> g_d3d11;
std::map<void*, Kept<D3D12Overlay>> g_d3d12;
constexpr std::chrono::seconds kIdle{3};
IDXGISwapChain* g_failed_for = nullptr;   // the swap chain the menu could not be drawn on (said once)
thread_local bool t_inside = false;

template <class T> void let_go_idle(std::map<void*, Kept<T>>& m, std::chrono::steady_clock::time_point now) {
    for (auto it = m.begin(); it != m.end();)
        if (now - it->second.used > kIdle) it = m.erase(it);
        else ++it;
}

#include "nr_pe_menu_vk.inl"

// The menu over this swap chain's back buffer, before its Present. Never throws.
void draw_on(IDXGISwapChain* sc, UINT flags) {
    ++g_presents;
    if (t_inside || (flags & DXGI_PRESENT_TEST)) return;
    t_inside = true;
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto now = std::chrono::steady_clock::now();
    // Refresh the presenting device before reaping idle overlays. A slow first frame or a pause must not
    // destroy its ImGui context on the very Present that is using it again.
    struct Reap { std::chrono::steady_clock::time_point now; ~Reap() {
        let_go_idle(g_d3d11, now); let_go_idle(g_d3d12, now);
    } } reap{now};
    const auto said = [sc](const char* api) {
        DXGI_SWAP_CHAIN_DESC desc{};
        sc->GetDesc(&desc);
        say(std::string("drawn over a ") + api + " swap chain, " + std::to_string(desc.BufferDesc.Width) + "x" +
            std::to_string(desc.BufferDesc.Height) + ", DXGI format " + std::to_string(int(desc.BufferDesc.Format)) +
            ": " + encoding_of(sc, desc.BufferDesc.Format).name);
    };
    try {
        ID3D11Device* d11 = nullptr;
        ID3D12CommandQueue* q12 = nullptr;
        ID3D12Device* d12dev = nullptr;
        // A D3D12 swap chain's device is the queue it presents on where DXGI hands that out; else (DXVK's DXGI) the
        // D3D12 device, and the queue is the direct queue this thread has just executed the frame on (watch_queues).
        if (FAILED(sc->GetDevice(IID_PPV_ARGS(&d11))) || !d11) {
            release(d11);
            if ((FAILED(sc->GetDevice(IID_PPV_ARGS(&q12))) || !q12) && SUCCEEDED(sc->GetDevice(IID_PPV_ARGS(&d12dev))) &&
                d12dev) {
                release(q12);
                if (ID3D12CommandQueue* last = last_direct_queue()) {
                    ID3D12Device* owner = nullptr;
                    if (SUCCEEDED(last->GetDevice(IID_PPV_ARGS(&owner))) && owner == d12dev) { q12 = last; q12->AddRef(); }
                    release(owner);
                }
                release(d12dev);
                if (!q12) { t_inside = false; return; }   // no frame executed on this thread yet: next Present
            }
        }
        if (d11) {
            auto& k = g_d3d11[d11];
            k.used = now;
            if (!k.overlay) {
                try {
                    k.overlay = std::make_unique<D3D11Overlay>(d11);
                } catch (...) {
                    d11->Release();
                    g_d3d11.erase(d11);
                    throw;
                }
                said("D3D11");
            }
            if (const D3D11PresentHook hook = g_d3d11_hook.load()) hook(sc, d11);
            d11->Release();
            k.overlay->present(sc);
        } else if (q12) {
            auto& k = g_d3d12[q12];
            k.used = now;
            if (!k.overlay) {
                ID3D12Device* d12 = nullptr;
                try {
                    if (FAILED(q12->GetDevice(IID_PPV_ARGS(&d12))) || !d12)
                        throw std::runtime_error("the swap chain's queue gives no device");
                    k.overlay = std::make_unique<D3D12Overlay>(d12, q12);
                } catch (...) {
                    release(d12);
                    q12->Release();
                    g_d3d12.erase(q12);
                    throw;
                }
                d12->Release();
                said("D3D12");
            }
            q12->Release();
            if (const D3D12PresentHook hook = g_d3d12_hook.load()) {
                DXGI_SWAP_CHAIN_DESC d{};
                if (SUCCEEDED(sc->GetDesc(&d))) hook(sc, q12, int(encoding_of(sc, d.BufferDesc.Format).mode));
            }
            k.overlay->present(sc);
        } else if (g_failed_for != sc) {
            g_failed_for = sc;
            IUnknown* u = nullptr;
            ID3D12Device* d = nullptr;
            const HRESULT hu = sc->GetDevice(IID_PPV_ARGS(&u)), hd = sc->GetDevice(IID_PPV_ARGS(&d));
            char t[200];
            std::snprintf(t, sizeof t, "a swap chain whose device is neither D3D11 nor a D3D12 queue (GetDevice: IUnknown "
                          "0x%08lx, ID3D12Device 0x%08lx); the menu is not drawn on it",
                          static_cast<unsigned long>(hu), static_cast<unsigned long>(hd));
            release(u);
            release(d);
            say(t);
        }
    } catch (const std::exception& e) {
        if (g_failed_for != sc) {
            g_failed_for = sc;
            say(std::string("the menu is not drawn on this swap chain: ") + e.what());
        }
    }
    t_inside = false;
}

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using SetColorSpace1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, DXGI_COLOR_SPACE_TYPE);
// A D3D11 and a D3D12 swap chain may be of different classes (they are under Proton): two of each at most.
PresentFn g_present[2]{};
Present1Fn g_present1[2]{};
SetColorSpace1Fn g_colour_space[2]{};

template <int N> HRESULT STDMETHODCALLTYPE present_hook(IDXGISwapChain* sc, UINT interval, UINT flags) {
    draw_on(sc, flags);
    return g_present[N](sc, interval, flags);
}
template <int N> HRESULT STDMETHODCALLTYPE present1_hook(IDXGISwapChain1* sc, UINT interval, UINT flags,
                                                         const DXGI_PRESENT_PARAMETERS* p) {
    draw_on(sc, flags);
    return g_present1[N](sc, interval, flags, p);
}
template <int N> HRESULT STDMETHODCALLTYPE colour_space_hook(IDXGISwapChain3* sc, DXGI_COLOR_SPACE_TYPE space) {
    const HRESULT hr = g_colour_space[N](sc, space);
    if (SUCCEEDED(hr)) {
        std::lock_guard<std::mutex> lock(g_spaces_mutex);
        g_spaces[sc] = space;
    }
    return hr;
}

// The methods as nr_present_probe.exe found them in a process of its own (see there), hooked in the same modules here.
void install_now() {
    const std::string folder = nr::pe::module_folder();
    std::wstring probe, out;
    {
        wchar_t w[MAX_PATH * 2] = {};
        MultiByteToWideChar(CP_ACP, 0, (folder + "\\dlssnr-amd\\nr_present_probe.exe").c_str(), -1, w, MAX_PATH * 2);
        probe = w;
        wchar_t tmp[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, tmp);
        out = std::wstring(tmp) + L"dlssnr-amd-present-" + std::to_wstring(GetCurrentProcessId()) + L".txt";
    }
    std::wstring command = L"\"" + probe + L"\" \"" + out + L"\"";
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(probe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        say("dlssnr-amd\\nr_present_probe.exe could not be started (error " + std::to_string(GetLastError()) + "); no menu");
        return;
    }
    const DWORD waited = WaitForSingleObject(pi.hProcess, 20000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    if (waited != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    std::ifstream in{std::filesystem::path(out)};
    std::vector<std::string> lines;
    for (std::string l; std::getline(in, l);) if (!l.empty()) lines.push_back(l);
    in.close();
    DeleteFileW(out.c_str());
    if (lines.empty()) {
        say("the swap-chain probe found nothing (exit " + std::to_string(code) + "); no menu");
        return;
    }
    const MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) { say("MinHook did not start; no menu"); return; }
    struct Target { const char* name; void* detour[2]; void** real[2]; int used; };
    Target targets[] = {
        {"Present", {reinterpret_cast<void*>(&present_hook<0>), reinterpret_cast<void*>(&present_hook<1>)},
         {reinterpret_cast<void**>(&g_present[0]), reinterpret_cast<void**>(&g_present[1])}, 0},
        {"Present1", {reinterpret_cast<void*>(&present1_hook<0>), reinterpret_cast<void*>(&present1_hook<1>)},
         {reinterpret_cast<void**>(&g_present1[0]), reinterpret_cast<void**>(&g_present1[1])}, 0},
        {"SetColorSpace1", {reinterpret_cast<void*>(&colour_space_hook<0>), reinterpret_cast<void*>(&colour_space_hook<1>)},
         {reinterpret_cast<void**>(&g_colour_space[0]), reinterpret_cast<void**>(&g_colour_space[1])}, 0},
    };
    std::vector<std::string> apis;
    std::vector<void*> hooked;
    for (const std::string& l : lines) {
        std::istringstream ls(l);
        std::string name, api, offset, bytes, path;
        ls >> name >> api >> offset >> bytes;
        std::getline(ls >> std::ws, path);
        if (std::find(apis.begin(), apis.end(), api) == apis.end()) apis.push_back(api);
        Target* t = nullptr;
        for (auto& c : targets) if (name == c.name) t = &c;
        if (!t || t->used == 2) continue;
        wchar_t wpath[MAX_PATH * 2] = {};
        MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wpath, MAX_PATH * 2);
        HMODULE module = GetModuleHandleW(wpath);
        if (!module) { say(name + " is in " + path + ", which this process has not loaded; not hooked"); continue; }
        auto* base = reinterpret_cast<uint8_t*>(module);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        const unsigned long long at = std::strtoull(offset.c_str(), nullptr, 16);
        if (at >= nt->OptionalHeader.SizeOfImage) { say(name + ": the probe's offset is outside the module; not hooked"); continue; }
        void* fn = base + at;
        if (std::find(hooked.begin(), hooked.end(), fn) != hooked.end()) continue;   // the same class for both APIs
        char here[33] = {};
        for (int i = 0; i < 16; ++i) std::snprintf(here + 2 * i, 3, "%02x", static_cast<const unsigned char*>(fn)[i]);
        if (bytes != here) say(name + " in " + path + " is already patched in this process (another hook); hooked over it");
        if (MH_CreateHook(fn, t->detour[t->used], t->real[t->used]) != MH_OK || MH_EnableHook(fn) != MH_OK) {
            say(name + " could not be hooked");
            continue;
        }
        ++t->used;
        hooked.push_back(fn);
    }
    std::string list;
    for (const auto& a : apis) list += (list.empty() ? "" : ", ") + a;
    if (!targets[0].used) { say("Present was not hooked; no menu"); return; }
    say("End opens the menu (DXGI's Present hooked; probed with " + list + " swap chains)");
    nr::pe::control::start_input();   // its keys from now on, not from NR's first evaluate
}

}  // namespace

void abandon_at_exit() {
    for (auto& [k, v] : g_vulkan) (void)v.release();
    for (auto& [k, v] : g_d3d11) (void)v.overlay.release();
    for (auto& [k, v] : g_d3d12) (void)v.overlay.release();
}


const VkPresentInfoKHR* vulkan_present(VkInstance instance, VkPhysicalDevice physical, VkDevice device,
        VkQueue queue, uint32_t family, VkSwapchainKHR sc, VkFormat format, VkColorSpaceKHR space,
        VkExtent2D extent, const std::vector<VkImage>& images, const VkPresentInfoKHR* info) {
    std::lock_guard<std::mutex> lock(g_mutex);
    static thread_local VkPresentInfoKHR result;
    if (!info || info->swapchainCount != 1 || !info->pImageIndices || info->pImageIndices[0] >= images.size()) return nullptr;
    auto& o = g_vulkan[sc];
    bool submitted_frame = false;
    try {
        nr::pe::control::start_input();
        if (!o) {
            o = std::make_unique<VulkanOverlay>();
            o->init(instance,physical,device,queue,family,format,space,extent,images);
            say("drawn over a Vulkan swap chain, " + std::to_string(extent.width) + "x" + std::to_string(extent.height) +
                ": " + o->encoding.name);
        }
        if (o->failed || !o->input()) return nullptr;
        // A shared menu image and ImGui upload buffers: finish our prior draw before changing either.
        if (o->last && vkWaitForFences(device,1,&o->last,VK_TRUE,2000000000ull)!=VK_SUCCESS) return nullptr;
        const uint32_t index=info->pImageIndices[0];
        auto& f=o->submits[index];
        if (vkWaitForFences(device,1,&f.fence,VK_TRUE,2000000000ull)!=VK_SUCCESS) return nullptr;
        vk_check(vkResetCommandBuffer(f.cmd,0),"menu reset");
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};bi.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vk_check(vkBeginCommandBuffer(f.cmd,&bi),"menu begin");
        o->record(f.cmd,index);
        ++g_drawn;
        const bool dump=vk_dump_record(*o,f.cmd,index);
        vk_check(vkEndCommandBuffer(f.cmd),"menu end");
        std::vector<VkPipelineStageFlags> waits(info->waitSemaphoreCount,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};si.waitSemaphoreCount=info->waitSemaphoreCount;
        si.pWaitSemaphores=info->pWaitSemaphores;si.pWaitDstStageMask=waits.data();si.commandBufferCount=1;si.pCommandBuffers=&f.cmd;
        si.signalSemaphoreCount=1;si.pSignalSemaphores=&f.done;
        vk_check(vkResetFences(device,1,&f.fence),"menu fence reset");
        const bool held=nr::pe::vkdevice::lock_queue_held(queue);
        const VkResult submitted=vkQueueSubmit(queue,1,&si,f.fence);
        nr::pe::vkdevice::unlock_queue_held(queue,held);
        vk_check(submitted,"menu submit");
        o->last=f.fence;
        result=*info;result.waitSemaphoreCount=1;result.pWaitSemaphores=&f.done;
        submitted_frame = true;
        if (dump && vkWaitForFences(device,1,&f.fence,VK_TRUE,2000000000ull)==VK_SUCCESS) {
            const uint32_t bpp=format==VK_FORMAT_R16G16B16A16_SFLOAT?8:4;
            dump_write(static_cast<const uint8_t*>(o->mapped),extent.width*bpp,extent.width,extent.height,
                       dump_format(format),o->encoding.mode);
            g_dump.done=true;
        }
        return &result;
    } catch (const std::exception& e) {
        if(o)o->failed=true;
        say(std::string("Vulkan menu: ")+e.what());
        return submitted_frame ? &result : nullptr;
    }
}

void vulkan_forget(VkDevice device, VkSwapchainKHR sc) {
    std::lock_guard<std::mutex> lock(g_mutex);
    for(auto it=g_vulkan.begin();it!=g_vulkan.end();) {
        auto& o=it->second;
        if(!o || (o->device==device && (!sc || it->first==sc))) {
            // Finish the menu commands before releasing their images and views.
            if(o) {
                const bool held=nr::pe::vkdevice::lock_queue_held(o->queue);
                const VkResult idle=vkQueueWaitIdle(o->queue);
                nr::pe::vkdevice::unlock_queue_held(o->queue,held);
                if(idle!=VK_SUCCESS) (void)o.release();
            }
            it=g_vulkan.erase(it);
        } else ++it;
    }
    nr::pe::receipts::collect();
}

void watch_queues(ID3D12Device* device) {
    static std::once_flag once;
    std::call_once(once, [device] {
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ID3D12CommandQueue* q = nullptr;
        if (!device || FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&q))) || !q) {
            say("no queue to learn the swap chains' queue from; no menu over D3D12 swap chains");
            return;
        }
        // IUnknown 3, ID3D12Object 4, ID3D12DeviceChild 1, ID3D12CommandQueue: UpdateTileMappings, CopyTileMappings,
        // ExecuteCommandLists
        void* fn = (*reinterpret_cast<void***>(q))[10];
        q->Release();
        const MH_STATUS init = MH_Initialize();
        if ((init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) ||
            MH_CreateHook(fn, reinterpret_cast<void*>(&execute_hook), reinterpret_cast<void**>(&g_execute)) != MH_OK ||
            MH_EnableHook(fn) != MH_OK)
            say("ExecuteCommandLists could not be hooked; no menu over D3D12 swap chains");
    });
}

void on_d3d12_present(D3D12PresentHook hook) { g_d3d12_hook.store(hook); }
void on_d3d11_present(D3D11PresentHook hook) { g_d3d11_hook.store(hook); }
void on_execute(ExecuteObserver observer) { g_execute_observer.store(observer); }
bool colour_space_of(IDXGISwapChain* swap_chain, int* space) {
    std::lock_guard<std::mutex> lock(g_spaces_mutex);
    const auto it = g_spaces.find(swap_chain);
    if (it == g_spaces.end()) {
        // NR_TEST_COLOUR_SPACE=<DXGI_COLOR_SPACE_TYPE> (tests): a swap chain the game set no colour space on is taken
        // as in that one (a test display whose surface refuses HDR colour spaces)
        static const int forced = [] { const char* e = std::getenv("NR_TEST_COLOUR_SPACE"); return e ? std::atoi(e) : -1; }();
        if (forced < 0) return false;
        *space = forced;
        return true;
    }
    *space = int(it->second);
    return true;
}

void install() {
    static std::once_flag once;
    std::call_once(once, [] {
        // Its own thread: the probe process takes a moment, which must not be spent inside the game's frame.
        std::thread([] {
            try {
                install_now();
            } catch (const std::exception& e) {
                say(std::string("no menu: ") + e.what());
            }
        }).detach();
    });
}

}  // namespace nr::pe::menu
