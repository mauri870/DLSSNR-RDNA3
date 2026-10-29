// See nr_pe_menu.hpp.
#include "nr_pe_menu.hpp"
#include "mingw_d3d12_compat.h"
#include "nr_control_shared.hpp"
#include "nr_ini_network.hpp"
#include "nr_control_ui.hpp"
#include "nr_pe_control.hpp"
#include "nr_pe_d3d12split.hpp"
#include "nr_pe_log.hpp"

#include "MinHook.h"
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_dx12.h"

#include <windows.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
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

// ---- dlssnr-amd.ini, edited in place: a value's line keeps its key and spacing, comments stay ----
class Ini {
  public:
    explicit Ini(std::filesystem::path p) : path_(std::move(p)) {}
    // Re-read when the file changed (the user's editor, the DLL writing defaults). True when it did.
    bool refresh() {
        std::error_code ec;
        const auto t = std::filesystem::last_write_time(path_, ec);
        if (ec) return false;
        const int64_t stamp = int64_t(t.time_since_epoch().count());
        if (stamp == stamp_ && !lines_.empty()) return false;
        std::ifstream f(path_);
        if (!f) return false;
        lines_.clear();
        std::string l;
        while (std::getline(f, l)) {
            if (!l.empty() && l.back() == '\r') l.pop_back();
            lines_.push_back(l);
        }
        stamp_ = stamp;
        return true;
    }
    bool has_section(const std::string& sec) const { return find_section(sec) >= 0; }
    std::string get(const std::string& sec, const std::string& key, const std::string& def) const {
        const int at = find_key(sec, key);
        if (at < 0) return def;
        const std::string& l = lines_[size_t(at)];
        std::string v = l.substr(l.find('=') + 1);
        const auto c = v.find_first_of(";#");
        if (c != std::string::npos) v = v.substr(0, c);
        return trim(v);
    }
    void set(const std::string& sec, const std::string& key, const std::string& value) {
        const int at = find_key(sec, key);
        if (at >= 0) {
            std::string& l = lines_[size_t(at)];
            const auto eq = l.find('=');
            const bool spaced = eq + 1 < l.size() && l[eq + 1] == ' ';
            l = l.substr(0, eq + 1) + (spaced ? " " : "") + value;
        } else {
            int s = find_section(sec);
            if (s < 0) {
                lines_.push_back("");
                lines_.push_back("[" + sec + "]");
                s = int(lines_.size()) - 1;
            }
            size_t end = size_t(s) + 1;
            while (end < lines_.size() && !(trim(lines_[end]).rfind("[", 0) == 0)) ++end;
            lines_.insert(lines_.begin() + std::ptrdiff_t(end), key + " = " + value);
        }
        dirty_ = true;
    }
    // Written whole through a temporary and a replacing move: the DLL never reads half a file.
    void save() {
        if (!dirty_) return;
        const auto tmp = path_.wstring() + L".nrcontrol";
        {
            std::ofstream f(std::filesystem::path(tmp), std::ios::trunc | std::ios::binary);
            for (const auto& l : lines_) f << l << "\r\n";
        }
        if (!MoveFileExW(tmp.c_str(), path_.wstring().c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            say("could not save dlssnr-amd.ini (error " + std::to_string(GetLastError()) + ")");
        dirty_ = false;
        std::error_code ec;
        const auto t = std::filesystem::last_write_time(path_, ec);
        if (!ec) stamp_ = int64_t(t.time_since_epoch().count());
    }

  private:
    static std::string trim(std::string s) {
        const auto a = s.find_first_not_of(" \t\r\n");
        const auto b = s.find_last_not_of(" \t\r\n");
        return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
    }
    static std::string lower(std::string s) {
        for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }
    int find_section(const std::string& sec) const {
        for (size_t i = 0; i < lines_.size(); ++i)
            if (lower(trim(lines_[i])) == "[" + lower(sec) + "]") return int(i);
        return -1;
    }
    int find_key(const std::string& sec, const std::string& key) const {
        const int s = find_section(sec);
        if (s < 0) return -1;
        for (size_t i = size_t(s) + 1; i < lines_.size(); ++i) {
            const std::string t = trim(lines_[i]);
            if (t.rfind("[", 0) == 0) break;
            if (t.empty() || t[0] == ';' || t[0] == '#') continue;
            const auto eq = t.find('=');
            if (eq != std::string::npos && lower(trim(t.substr(0, eq))) == lower(key)) return int(i);
        }
        return -1;
    }
    std::filesystem::path path_;
    std::vector<std::string> lines_;
    int64_t stamp_ = -1;
    bool dirty_ = false;
};

// [RenoDX]'s values for nr_control_ui.hpp, read from and written to the ini by the table's names.
struct RenoDXValues {
    std::vector<uint8_t> bools;
    std::vector<uint32_t> uints;
    std::vector<float> floats;
    std::vector<std::string> texts;
    bool& b(int i) { return reinterpret_cast<bool&>(bools[size_t(i)]); }
    uint32_t& u(int i) { return uints[size_t(i)]; }
    float& f(int i) { return floats[size_t(i)]; }
    std::string& s(int i) { return texts[size_t(i)]; }
    void load(const Ini& ini) {
        using namespace nr::renodx_keys;
        bools.assign(kCount, 0); uints.assign(kCount, 0); floats.assign(kCount, 0.f); texts.assign(kCount, {});
        for (int i = 0; i < kCount; ++i) {
            const Key& k = kKeys[i];
            const std::string v = ini.get("RenoDX", k.name, "");
            switch (k.kind) {
                case kBool: bools[size_t(i)] = v.empty() ? k.def != 0.f : (v == "1" || v == "true"); break;
                case kUint: uints[size_t(i)] = v.empty() ? uint32_t(k.def) : uint32_t(std::strtoul(v.c_str(), nullptr, 10)); break;
                case kFloat: floats[size_t(i)] = v.empty() ? k.def : std::strtof(v.c_str(), nullptr); break;
                case kText: texts[size_t(i)] = std::string(v); break;
            }
        }
    }
    void store(Ini& ini) const {
        using namespace nr::renodx_keys;
        for (int i = 0; i < kCount; ++i) {
            const Key& k = kKeys[i];
            char v[64];
            switch (k.kind) {
                case kBool: std::snprintf(v, sizeof v, "%d", bools[size_t(i)] ? 1 : 0); break;
                case kUint: std::snprintf(v, sizeof v, "%u", uints[size_t(i)]); break;
                case kFloat: std::snprintf(v, sizeof v, "%.*f", int(k.decimals), double(floats[size_t(i)])); break;
                case kText: std::snprintf(v, sizeof v, "%s", texts[size_t(i)].c_str()); break;
            }
            if (ini.get("RenoDX", k.name, "\x01") != v) ini.set("RenoDX", k.name, v);
        }
    }
};

// NR_CONTROL_AT=<n>: NR_CONTROL_SET, NR_CONTROL_SHOT and NR_CONTROL_DUMP wait until the menu has been drawn n times.
uint64_t control_at() {
    const char* e = std::getenv("NR_CONTROL_AT");
    return e ? std::strtoull(e, nullptr, 10) : 0;
}
uint64_t g_drawn = 0;   // frames the menu was drawn in

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

// ---- the menu's content: the same as the Linux layer's ----
struct Menu {
    ImGuiContext* context{};
    std::chrono::steady_clock::time_point previous = std::chrono::steady_clock::now();
    Ini ini{game_dir() / "dlssnr-amd.ini"};
    RenoDXValues renodx;
    int64_t loaded = -1;

    Menu() {
        context = ImGui::CreateContext();
        ImGui::SetCurrentContext(context);
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.ConfigDebugHighlightIdConflicts = false;   // a developer aid, not a player's message
        ImGui::StyleColorsDark();
        auto& style = ImGui::GetStyle();
        style.WindowRounding = 5;
        style.FrameRounding = 3;
        style.Colors[ImGuiCol_WindowBg] = {.07f, .08f, .10f, .96f};
    }
    ~Menu() { if (context) ImGui::DestroyContext(context); }

    // This frame's input; whether anything is to be drawn (the menu, or with the menu closed the OSD when it is due).
    // The caller has made `context` current.
    bool input(uint32_t width, uint32_t height) {
        ++shared()->menu_seq;
        auto& io = ImGui::GetIO();
        const auto now = std::chrono::steady_clock::now();
        io.DeltaTime = std::clamp(std::chrono::duration<float>(now - previous).count(), .001f, .1f);
        previous = now;
        io.DisplaySize = {float(width), float(height)};
        g_input.update(io, width, height);
        osd_only = !g_input.open && osd_due(now);
        return g_input.open || osd_only;
    }

    // [Network] OSD with the menu closed: while a switch builds, its progress, then the network now running for 3 s
    // (switch, the default); the running network all the time (always); nothing (off). The mode is read from the file
    // once a second.
    bool osd_only = false;
    int osd_mode = 1;
    uint32_t osd_want = ~0u, osd_running = ~0u;
    std::chrono::steady_clock::time_point osd_until{}, osd_read{};
    bool osd_due(std::chrono::steady_clock::time_point now) {
        if (now - osd_read > std::chrono::seconds(1)) {
            osd_read = now;
            if (ini.refresh()) loaded = -1;   // the menu loads the rest when it opens
            const std::string m = ini.get("Network", "OSD", "switch");
            osd_mode = m == "off" ? 0 : m == "always" ? 2 : 1;
        }
        const nr::control::Shared* s = shared();
        if (!s || !s->route || !s->net_known) return false;
        if (s->net_want != osd_want || (s->net_running < 3 && s->net_running != osd_running)) {
            if (osd_want != ~0u && osd_running != ~0u) osd_until = now + std::chrono::seconds(3);
            osd_want = s->net_want;
            if (s->net_running < 3) osd_running = s->net_running;
        }
        if (osd_mode == 0 || s->net_running >= 3) return false;
        return osd_mode == 2 || switching(s) || now < osd_until;
    }
    static bool switching(const nr::control::Shared* s) {
        return s->building && s->building_net < 3 && s->building_net != s->net_running;
    }
    void draw_osd() {
        const nr::control::Shared* s = shared();
        ImGui::SetNextWindowPos({24, 24});
        ImGui::SetNextWindowBgAlpha(0.65f);
        ImGui::Begin("##nr-osd", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
        if (switching(s))
            ImGui::Text("NR: switching to %s ... %.0f%%", nr::network_codename(int(s->building_net)),
                        double(s->build_progress) * 100.0);
        else
            ImGui::Text("NR: %s", s->nr_on ? nr::network_codename(int(s->net_running)) : "off");
        ImGui::End();
    }

    // A 0/1 key as a checkbox, written at once.
    void flag(const char* label, const char* section, const char* key, const char* def, const char* help) {
        bool on = ini.get(section, key, def) == "1";
        if (ImGui::Checkbox(label, &on)) ini.set(section, key, on ? "1" : "0");
        nr::control_ui::help(help);
    }

    // A hotkey as text, written when the field is let go (the DLL reads the file again and takes it at once).
    void hotkey_text(const char* label, const char* section, const char* help) {
        static std::unordered_map<std::string, std::string> editing;
        auto it = editing.find(label);
        std::string& t = it != editing.end() ? it->second : (editing[label] = ini.get(section, "Hotkey", ""));
        char buf[64];
        std::snprintf(buf, sizeof buf, "%s", t.c_str());
        if (ImGui::InputText(label, buf, sizeof buf)) t = buf;
        if (ImGui::IsItemDeactivatedAfterEdit()) { ini.set(section, "Hotkey", t); editing.erase(label); }
        else if (!ImGui::IsItemActive()) editing.erase(label);
        nr::control_ui::help(help);
    }

    // The menu's draw data (or the OSD's alone), after the backend's NewFrame.
    void build(uint32_t height) {
        if (osd_only) {
            auto& io = ImGui::GetIO();
            const bool cursor = io.MouseDrawCursor;
            io.MouseDrawCursor = false;
            ImGui::NewFrame();
            draw_osd();
            ImGui::Render();
            io.MouseDrawCursor = cursor;
            return;
        }
        if (ini.refresh() || loaded < 0) { renodx.load(ini); loaded = 1; }
        static bool set_done = false;
        if (const char* e = std::getenv("NR_CONTROL_SET"); e && !set_done && g_drawn >= control_at()) {
            set_done = true;
            std::stringstream ss(e);
            std::string item;
            while (std::getline(ss, item, ',')) {
                const auto dot = item.find('.'), eq = item.find('=');
                if (dot != std::string::npos && eq != std::string::npos && dot < eq)
                    ini.set(item.substr(0, dot), item.substr(dot + 1, eq - dot - 1), item.substr(eq + 1));
            }
            ini.save();
            renodx.load(ini);
            say("NR_CONTROL_SET written at menu frame " + std::to_string(g_drawn) + ": " + e);
        }
        static bool shot_done = false;
        if (const char* e = std::getenv("NR_CONTROL_SHOT"); e && e[0] == '1' && !shot_done && g_drawn >= control_at()) {
            shot_done = true;
            ++shared()->screenshot_request;
            say("NR_CONTROL_SHOT: the screenshot button pressed at menu frame " + std::to_string(g_drawn));
        }
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({24, 24}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize({560, std::min(float(height) - 48.f, 820.f)}, ImGuiCond_FirstUseEver);
        bool open = true;
        if (ImGui::Begin("NR Control", &open)) draw();
        ImGui::End();
        ImGui::Render();
        if (!open) g_input.close();
        ini.save();
    }

    void draw() {
        const nr::control::Shared* s = shared();
        const bool renodx_running = s && s->renodx;
        if (s && s->route) {
            ImGui::Text("Neural Rendering: %s%s", s->nr_on ? "on" : "off", renodx_running ? " (RenoDX style)" : "");
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
                const char* where = s->placement == 1 ? " (after the upscaler)" : s->placement == 2 ? " (before the upscaler)" : "";
                ImGui::Text("NR on %ux%u%s, network %ux%u, %u pass%s", s->width, s->height, where, s->nr_width,
                            s->nr_height, s->passes, s->passes == 1 ? "" : "es");
            }
            if (s->gpu_ms > 0.f && s->network_ms > 0.f) {
                ImGui::Text("NR cost: %.2f ms/frame", double(s->network_ms));
                ImGui::Text("%s: %.2f ms/frame   (total %.2f ms)", renodx_running ? "RenoDX processing" : "OptiScaler route",
                            double(std::max(s->gpu_ms - s->network_ms, 0.f)), double(s->gpu_ms));
            } else if (s->gpu_ms > 0.f) {
                ImGui::Text("NR cost: %.2f ms/frame", double(s->gpu_ms));
            } else {
                ImGui::TextUnformatted("NR cost: --");
            }
            if (s->net_running < 3) ImGui::Text("Network: %s", nr::network_codename(int(s->net_running)));
            if (s->building) ImGui::TextDisabled("Building a network...");
            if (s->state[0]) ImGui::TextWrapped("%s", s->state);
        } else {
            ImGui::TextDisabled("Waiting for NR to run (DLSS in the game's settings, OptiScaler's menu: Insert).");
        }
        const bool renodx_route = s && s->route ? renodx_running : ini.get("RenoDX", "Enabled", "0") == "1";
        if (!renodx_route) ImGui::TextDisabled("NR's other settings: OptiScaler's menu (Insert).");
        ImGui::Separator();
        if (renodx_route && nr::control_ui::draw_renodx(renodx, nr::control_ui::Route::kOptiScaler, renodx_route, [s] {
                if (ImGui::Button("Take a screenshot now") && s) ++const_cast<nr::control::Shared*>(s)->screenshot_request;
                if (s && s->screenshot[0]) ImGui::TextWrapped("Screenshot: %s", s->screenshot);
            }))
            renodx.store(ini);
        // [Network]: which network runs (the three by the DLL's codenames); one that cannot run in this run is
        // greyed out with the reason the DLL found (net_why), and the DLL skips it
        // (not "Network": the RenoDX table has a group of that name, and the same label is the same header)
        if (ImGui::CollapsingHeader("Network model and options", ImGuiTreeNodeFlags_DefaultOpen)) {
            const int file_model = std::max(0, nr::network_choice(ini.get("Network", "Model", "CC_SMALL_FP8")));
            const int shown = s && s->net_known && s->net_want < 3 ? int(s->net_want) : file_model;
            if (ImGui::BeginCombo("Network##model", nr::network_codename(shown))) {
                for (int n = 0; n < nr::kNetworkChoices; ++n) {
                    const char* why = s && s->net_known ? s->net_why[n] : "";
                    const bool ok = !why[0];
                    if (ImGui::Selectable(nr::network_codename(n), n == shown, ok ? 0 : ImGuiSelectableFlags_Disabled))
                        ini.set("Network", "Model", nr::network_codename(n));
                    if (!ok) { ImGui::SameLine(); ImGui::TextDisabled("- %s", why); }
                }
                ImGui::EndCombo();
            }
            nr::control_ui::help("[Network] Model. CC_SMALL_FP8: 310.9.16's default network, the fastest. CC_SILVER_AARDWOLD: "
                                 "310.8.0's network (310.9.16's preset 1). CC_SILVER_AARDWOLD_INT4_MIXED: that network with part "
                                 "of its arithmetic in int4, faster, a slightly different picture; needs ACO Mode. Switching "
                                 "builds the new network in the background; the running one serves until it is ready.");
            for (int n = 0; n < nr::kNetworkChoices; ++n)
                if (s && s->net_known && s->net_why[n][0]) ImGui::TextDisabled("%s: %s", nr::network_codename(n), s->net_why[n]);
            hotkey_text("Hotkey##net", "Network", "[Network] Hotkey: cycles through the networks that can run. Ctrl, Shift, "
                        "Alt and one key, e.g. Ctrl+F11 or Alt+F9; empty: no hotkey.");
            bool sound = ini.get("Network", "Sound", "1") == "1";
            if (ImGui::Checkbox("Sound##net", &sound)) ini.set("Network", "Sound", sound ? "1" : "0");
            nr::control_ui::help("[Network] Sound. A cue on every switch: one note CC_SMALL_FP8, two going down "
                                 "CC_SILVER_AARDWOLD, two going up CC_SILVER_AARDWOLD_INT4_MIXED.");
            static const char* const residencies[] = {"auto", "resident", "offload", "minimal"};
            const std::string r = ini.get("Network", "Residency", "auto");
            int ri = 0;
            for (int k = 0; k < 4; ++k) if (r == residencies[k]) ri = k;
            if (ImGui::Combo("Residency", &ri, residencies, 4)) ini.set("Network", "Residency", residencies[ri]);
            nr::control_ui::help("[Network] Residency: what is kept of a network not in use. The networks share one "
                                 "activation memory, so mostly their weights. auto (default): the weights stay in video "
                                 "memory, switching back is quick; moved to RAM when video memory runs short, freed when RAM "
                                 "does too. resident: kept whole, switching back is instant, the most video memory. offload: "
                                 "the weights in RAM, all video memory freed. minimal: everything freed, switching back builds "
                                 "anew. While the other network gets ready, the running one keeps serving. NR off for 10 s: "
                                 "offload and minimal do the same to the running network, auto only when video memory is short.");
            static const char* const osds[] = {"off", "switch", "always"};
            const std::string o = ini.get("Network", "OSD", "switch");
            int oi = 1;
            for (int k = 0; k < 3; ++k) if (o == osds[k]) oi = k;
            if (ImGui::Combo("OSD", &oi, osds, 3)) ini.set("Network", "OSD", osds[oi]);
            nr::control_ui::help("[Network] OSD: the running network in the corner of the picture. switch (default): while "
                                 "a switch builds, its progress; then the new network for 3 s. always: all the time. off: never.");
            flag("Style grade (Natural / Cinematic)", "Network", "Style Grade", "1",
                 "[Network] Style Grade. On (default): Natural and Cinematic are graded after the network as NVIDIA's DLL "
                 "does. Off: the picture these styles had up to 0.0.6, without that grade. Standard is not affected. "
                 "Takes effect at once.");
            flag("Motion without depth", "Network", "Motion Without Depth", "1",
                 "[Network] Motion Without Depth. Where the network's previous-frame lookup reads its motion vector.\n"
                 "On (default): the pixel's own vector, depth not used, as NVIDIA's DLL does.\n"
                 "Off: with depth, the vector of the nearest of the pixel and its four diagonal neighbours.\n"
                 "Only moving edges between frames with motion vectors and depth differ. Takes effect after a restart.");
            flag("Resize history", "Network", "Resize History", "0",
                 "[Network] Resize History. Only when NR runs before the upscaler and the game changes its render size\n"
                 "(dynamic resolution):\n"
                 "Off (default): NR's previous result is dropped and NR starts again at the new size, as NVIDIA's DLL does.\n"
                 "On: the previous result is scaled to the new size and kept. Steadier on our tests when the size changes\n"
                 "often, but not what NVIDIA's DLL does. Games with jittered input (most DLSS games) hide most of the\n"
                 "restart, so the default is off.");
            flag("ACO Mode", "Network", "ACO Mode", "1",
                 "[Network] ACO Mode. On (default): the network's machine code made in advance by the ACO compiler, loaded by "
                 "the AMD driver; when it cannot be used, the driver's compiler is used and dlssnr-amd.log says why. Off: the "
                 "AMD driver's own compiler. CC_SILVER_AARDWOLD_INT4_MIXED needs it on. Takes effect after a restart.");
        }
        // What the game hands NR, and who covers what it does not (the DLL's lines, nr_engine_panel.hpp)
        if (s && s->route && s->panel_lines && ImGui::CollapsingHeader("Engine data")) {
            nr::control_ui::engine_legend();
            for (uint32_t i = 0; i < std::min(s->panel_lines, nr::control::kPanelLines); ++i) {
                char line[sizeof s->panel[0] + 1];
                std::snprintf(line, sizeof line, "%s", s->panel[i]);
                nr::control_ui::engine_line(line);
            }
        }
        if (ImGui::CollapsingHeader("Preprocess")) {
            bool on = ini.get("Preprocess", "Enabled", "0") == "1";
            if (ImGui::Checkbox("Preprocess##on", &on)) ini.set("Preprocess", "Enabled", on ? "1" : "0");
            nr::control_ui::help("[Preprocess] Enabled. Changes the picture the network is shown.");
            static const char* const exposures[] = {"off", "auto", "fixed"};
            static const char* const curves[] = {"none", "neutral", "reinhard", "filmic", "gt", "aces", "agx"};
            auto combo = [&](const char* label, const char* key, const char* const* items, int n, const char* help) {
                const std::string v = ini.get("Preprocess", key, items[0]);
                int i = 0;
                for (int k = 0; k < n; ++k) if (v == items[k]) i = k;
                if (ImGui::Combo(label, &i, items, n)) ini.set("Preprocess", key, items[i]);
                nr::control_ui::help(help);
            };
            auto slider = [&](const char* label, const char* key, float lo, float hi, float def, const char* help) {
                static std::unordered_map<std::string, float> pending;
                const auto it = pending.find(key);
                float v = it != pending.end() ? it->second : std::strtof(ini.get("Preprocess", key, "").c_str(), nullptr);
                if (it == pending.end() && ini.get("Preprocess", key, "").empty()) v = def;
                if (ImGui::SliderFloat(label, &v, lo, hi, "%.2f", ImGuiSliderFlags_AlwaysClamp)) pending[key] = v;
                if (ImGui::IsItemDeactivatedAfterEdit() && pending.count(key)) {
                    char t[32];
                    std::snprintf(t, sizeof t, "%.2f", double(pending[key]));
                    ini.set("Preprocess", key, t);
                    pending.erase(key);
                }
                nr::control_ui::help(help);
            };
            combo("Exposure", "Exposure", exposures, 3, "[Preprocess] Exposure. auto: Unreal-style auto exposure; fixed: Exposure bias only.");
            slider("Exposure bias (EV)", "ExposureBias", -8.f, 8.f, 0.f, "[Preprocess] ExposureBias.");
            combo("Curve", "Curve", curves, 7, "[Preprocess] Curve. The display curve of the picture NR is shown.");
            slider("Contrast", "Contrast", 0.5f, 2.f, 1.f, "[Preprocess] Contrast.");
            slider("Saturation", "Saturation", 0.05f, 2.f, 1.f, "[Preprocess] Saturation.");
            hotkey_text("Hotkey##prep", "Preprocess", "[Preprocess] Hotkey: switches the whole preprocess on or off for "
                        "this run (not written to the file). Ctrl, Shift, Alt and one key; empty: no hotkey.");
            flag("Sound##prep", "Preprocess", "Sound", "1",
                 "[Preprocess] Sound. A cue when it switches: two notes going up = on, going down = off.");
        }
        // [Log]: read once when the game starts
        if (ImGui::CollapsingHeader("Log")) {
            flag("Write the log", "Log", "Enabled", "1",
                 "[Log] Enabled. dlssnr-amd.log in the game's folder. Takes effect after a restart.");
            flag("Clear on start", "Log", "ClearOnStart", "1",
                 "[Log] ClearOnStart. On: each start of the game begins a new log; off: appends to the last one. "
                 "Takes effect after a restart.");
        }
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
        if (!menu_ui.input(w, h)) return;
        ID3D11Texture2D* back = nullptr;
        check(sc->GetBuffer(0, IID_PPV_ARGS(&back)), "GetBuffer");
        ID3DDeviceContextState* game = nullptr;
        context1->SwapDeviceContextState(state, &game);
        ID3D11RenderTargetView* rtv = nullptr;
        try {
            ensure_menu(w, h);
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
        wait_all();
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
    void wait(uint64_t value) {
        if (!value || fence->GetCompletedValue() >= value) return;
        if (SUCCEEDED(fence->SetEventOnCompletion(value, event))) WaitForSingleObject(event, 2000);
    }
    void wait_all() { if (fence) wait(fence_value); }
    void ensure_menu(uint32_t w, uint32_t h) {
        if (menu && w == width && h == height) return;
        wait_all();
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
        device->CreateRenderTargetView(menu, nullptr, rtv(0));
        device->CreateShaderResourceView(menu, nullptr, srv_cpu(0));
        width = w; height = h;
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
        if (!menu_ui.input(w, h)) return;
        IDXGISwapChain3* sc3 = nullptr;
        check(sc->QueryInterface(IID_PPV_ARGS(&sc3)), "IDXGISwapChain3");
        const UINT index = sc3->GetCurrentBackBufferIndex();
        sc3->Release();
        ID3D12Resource* back = nullptr;
        check(sc->GetBuffer(index, IID_PPV_ARGS(&back)), "GetBuffer");
        struct Hold { ID3D12Resource*& r; ~Hold() { release(r); } } hold{back};
        ensure_menu(w, h);
        const Encoding enc = encoding_of(sc, desc.BufferDesc.Format);
        ID3D12PipelineState* compose = pipeline(desc.BufferDesc.Format);
        ImGui_ImplDX12_NewFrame();
        menu_ui.build(h);
        wait(done_at[frame]);
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
        check(queue->Signal(fence, ++fence_value), "the fence signal");
        done_at[frame] = fence_value;
        frame = (frame + 1) % kFrames;
        if (readback) {
            wait(fence_value);
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

// The menu over this swap chain's back buffer, before its Present. Never throws.
void draw_on(IDXGISwapChain* sc, UINT flags) {
    ++g_presents;
    if (t_inside || (flags & DXGI_PRESENT_TEST)) return;
    t_inside = true;
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto now = std::chrono::steady_clock::now();
    let_go_idle(g_d3d11, now);
    let_go_idle(g_d3d12, now);
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
        // A D3D12 swap chain's device is the queue it presents on where DXGI hands that out; else (DXVK's DXGI,
        // and the runtime's here) the D3D12 device, and the queue is the direct queue this thread has just executed
        // the frame on (nr_pe_d3d12split.hpp).
        if (FAILED(sc->GetDevice(IID_PPV_ARGS(&d11))) || !d11) {
            release(d11);
            if ((FAILED(sc->GetDevice(IID_PPV_ARGS(&q12))) || !q12) && SUCCEEDED(sc->GetDevice(IID_PPV_ARGS(&d12dev))) &&
                d12dev) {
                release(q12);
                if (ID3D12CommandQueue* last = nr::pe::split::last_direct_queue()) {
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
}

}  // namespace

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
