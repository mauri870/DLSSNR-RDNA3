// See nr_pe_control.hpp.
#include "nr_pe_control.hpp"
#include "nr_pe_events.hpp"
#include "nr_control_shared.hpp"
#include "nr_pe_config.hpp"
#include "nr_pe_log.hpp"
#include "nr_screenshot.hpp"
#include "nr_vram_ledger.hpp"

#include <windows.h>
#include "MinHook.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

namespace nr::pe::control {
namespace {

nr::control::Shared* g_shared = nullptr;
std::atomic<bool> g_tried{false};

nr::control::Shared* shared() {
    if (g_shared || g_tried.exchange(true)) return g_shared;
    const std::string path = std::string(module_folder()) + "\\dlssnr-amd\\control.bin";
    HANDLE file = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return nullptr;
    HANDLE map = CreateFileMappingA(file, nullptr, PAGE_READWRITE, 0, sizeof(nr::control::Shared), nullptr);
    CloseHandle(file);
    if (!map) return nullptr;
    void* view = MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(nr::control::Shared));
    CloseHandle(map);
    if (!view) return nullptr;
    auto* s = static_cast<nr::control::Shared*>(view);
    if (s->magic != nr::control::kMagic || s->version != nr::control::kVersion) {
        std::memset(s, 0, sizeof *s);
        s->version = nr::control::kVersion;
        s->magic = nr::control::kMagic;
    }
    g_shared = s;
    return s;
}

// ---- Wayland: the game window's input for the menu ----
HWND g_window = nullptr;
WNDPROC g_previous = nullptr;

// While the menu is open the game gets none of the keyboard and mouse, as under ReShade's overlay: its window messages
// are dropped (window_proc), and what it would read directly is held still - the cursor position where it was when
// the menu opened, raw mouse and keyboard input empty, every key up. A game that hides the cursor and clips it (mouse
// look) has Wine's Wayland driver lock the pointer (only relative motion, the cursor stays where the game puts it), so
// the clip is lifted and the game's ClipCursor / SetCursorPos wait; its last clip is put back when the menu closes.
// Our own input thread (watch) still reads the real state.
using ClipCursorFn = BOOL(WINAPI*)(const RECT*);
using SetCursorPosFn = BOOL(WINAPI*)(int, int);
using GetCursorPosFn = BOOL(WINAPI*)(POINT*);
using GetCursorInfoFn = BOOL(WINAPI*)(CURSORINFO*);
using KeyStateFn = SHORT(WINAPI*)(int);
using GetKeyboardStateFn = BOOL(WINAPI*)(BYTE*);
using GetRawInputDataFn = UINT(WINAPI*)(HRAWINPUT, UINT, void*, UINT*, UINT);
using GetRawInputBufferFn = UINT(WINAPI*)(RAWINPUT*, UINT*, UINT);
ClipCursorFn g_real_clip = nullptr;
SetCursorPosFn g_real_setpos = nullptr;
GetCursorPosFn g_real_getpos = nullptr, g_real_physpos = nullptr;
GetCursorInfoFn g_real_cursorinfo = nullptr;
KeyStateFn g_real_async = nullptr, g_real_keystate = nullptr;
GetKeyboardStateFn g_real_keyboard = nullptr;
GetRawInputDataFn g_real_rawdata = nullptr;
GetRawInputBufferFn g_real_rawbuffer = nullptr;
std::mutex g_clip_mutex;
bool g_game_clipped = false;
RECT g_game_clip{};
POINT g_frozen{};
std::atomic<bool> g_menu_open{false};
std::atomic<DWORD> g_watch_thread{0};

bool held() { return g_menu_open.load() && GetCurrentThreadId() != g_watch_thread.load(); }

BOOL WINAPI clip_cursor(const RECT* r) {
    {
        std::lock_guard<std::mutex> lock(g_clip_mutex);
        g_game_clipped = r != nullptr;
        if (r) g_game_clip = *r;
    }
    return held() ? TRUE : g_real_clip(r);
}
BOOL WINAPI set_cursor_pos(int x, int y) { return held() ? TRUE : g_real_setpos(x, y); }
BOOL WINAPI get_cursor_pos(POINT* p) {
    if (!held() || !p) return g_real_getpos(p);
    *p = g_frozen;
    return TRUE;
}
BOOL WINAPI get_physical_cursor_pos(POINT* p) {
    if (!held() || !p) return g_real_physpos(p);
    *p = g_frozen;
    return TRUE;
}
BOOL WINAPI get_cursor_info(CURSORINFO* ci) {
    const BOOL ok = g_real_cursorinfo(ci);
    if (ok && ci && held()) ci->ptScreenPos = g_frozen;
    return ok;
}
SHORT WINAPI get_async_key_state(int vk) { return held() ? 0 : g_real_async(vk); }
SHORT WINAPI get_key_state(int vk) { return held() ? 0 : g_real_keystate(vk); }
BOOL WINAPI get_keyboard_state(BYTE* keys) {
    const BOOL ok = g_real_keyboard(keys);
    if (ok && keys && held()) std::memset(keys, 0, 256);
    return ok;
}
UINT WINAPI get_raw_input_data(HRAWINPUT h, UINT command, void* data, UINT* size, UINT header) {
    const UINT r = g_real_rawdata(h, command, data, size, header);
    if (held() && data && command == RID_INPUT && r != UINT(-1) && r >= sizeof(RAWINPUTHEADER)) {
        auto* in = static_cast<RAWINPUT*>(data);
        if (in->header.dwType == RIM_TYPEMOUSE) {
            in->data.mouse.lLastX = in->data.mouse.lLastY = 0;
            in->data.mouse.usButtonFlags = 0;
            in->data.mouse.usButtonData = 0;
        } else if (in->header.dwType == RIM_TYPEKEYBOARD) {
            in->data.keyboard.VKey = 0xFF;   // no key
            in->data.keyboard.MakeCode = 0;
            in->data.keyboard.Message = 0;
        }
    }
    return r;
}
UINT WINAPI get_raw_input_buffer(RAWINPUT* data, UINT* size, UINT header) {
    const UINT r = g_real_rawbuffer(data, size, header);   // read anyway: nothing held back is delivered later
    return data && held() && r != UINT(-1) ? 0u : r;
}

bool hook_input() {
    static bool tried = false, ok = false;
    if (tried) return ok;
    tried = true;
    HMODULE user32 = GetModuleHandleA("user32.dll");
    struct Hook { const char* name; void* detour; void** real; } hooks[] = {
        {"ClipCursor", reinterpret_cast<void*>(&clip_cursor), reinterpret_cast<void**>(&g_real_clip)},
        {"SetCursorPos", reinterpret_cast<void*>(&set_cursor_pos), reinterpret_cast<void**>(&g_real_setpos)},
        {"GetCursorPos", reinterpret_cast<void*>(&get_cursor_pos), reinterpret_cast<void**>(&g_real_getpos)},
        {"GetPhysicalCursorPos", reinterpret_cast<void*>(&get_physical_cursor_pos), reinterpret_cast<void**>(&g_real_physpos)},
        {"GetCursorInfo", reinterpret_cast<void*>(&get_cursor_info), reinterpret_cast<void**>(&g_real_cursorinfo)},
        {"GetAsyncKeyState", reinterpret_cast<void*>(&get_async_key_state), reinterpret_cast<void**>(&g_real_async)},
        {"GetKeyState", reinterpret_cast<void*>(&get_key_state), reinterpret_cast<void**>(&g_real_keystate)},
        {"GetKeyboardState", reinterpret_cast<void*>(&get_keyboard_state), reinterpret_cast<void**>(&g_real_keyboard)},
        {"GetRawInputData", reinterpret_cast<void*>(&get_raw_input_data), reinterpret_cast<void**>(&g_real_rawdata)},
        {"GetRawInputBuffer", reinterpret_cast<void*>(&get_raw_input_buffer), reinterpret_cast<void**>(&g_real_rawbuffer)},
    };
    const MH_STATUS init = MH_Initialize();
    ok = user32 && (init == MH_OK || init == MH_ERROR_ALREADY_INITIALIZED);
    for (const Hook& h : hooks) {
        void* at = ok ? reinterpret_cast<void*>(GetProcAddress(user32, h.name)) : nullptr;
        ok = at && MH_CreateHook(at, h.detour, h.real) == MH_OK && MH_EnableHook(at) == MH_OK;
        if (!ok) { log("[nr] NR Control: %s could not be hooked; the game may still see the mouse under the menu", h.name); break; }
    }
    return ok;
}

// The menu opened or closed: hold the game's input and lift its clip, or give both back.
void menu_changed(bool open) {
    if (!hook_input()) { g_menu_open.store(open); return; }
    if (open) {
        g_real_getpos(&g_frozen);
        g_menu_open.store(true);
        g_real_clip(nullptr);
    } else {
        g_menu_open.store(false);
        std::lock_guard<std::mutex> lock(g_clip_mutex);
        g_real_clip(g_game_clipped ? &g_game_clip : nullptr);
        g_real_setpos(g_frozen.x, g_frozen.y);   // the cursor back where the game left it
    }
}

LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    nr::control::Shared* s = g_shared;
    if (s && s->wayland) {
        if (msg == WM_MOUSEWHEEL) s->wheel += GET_WHEEL_DELTA_WPARAM(wp);
        if (msg == WM_CHAR && s->menu_open && wp >= 32) {
            s->chars[s->chars_written % 64] = uint32_t(wp);
            ++s->chars_written;
        }
        if (s->menu_open) {
            // The menu has the keyboard and mouse: the game's window gets none of it.
            if (msg == WM_INPUT) return DefWindowProcW(hwnd, msg, wp, lp);
            if ((msg >= WM_KEYFIRST && msg <= WM_KEYLAST) || (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST)) return 0;
        }
    }
    return CallWindowProcW(g_previous, hwnd, msg, wp, lp);
}

// The game's window: the largest visible top-level window of this process.
HWND game_window() {
    struct Find { HWND best = nullptr; long area = 0; } f;
    EnumWindows([](HWND h, LPARAM p) -> BOOL {
        auto& f = *reinterpret_cast<Find*>(p);
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        RECT r{};
        if (pid == GetCurrentProcessId() && IsWindowVisible(h) && GetClientRect(h, &r)) {
            const long area = long(r.right - r.left) * long(r.bottom - r.top);
            if (area > f.area) { f.area = area; f.best = h; }
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&f));
    return f.best;
}

void watch() {
    g_watch_thread.store(GetCurrentThreadId());
    for (;;) {
        nr::control::Shared* s = g_shared;
        const bool open = s && s->menu_open != 0;
        if (s && open != g_menu_open.load()) menu_changed(open);
        if (open && g_real_clip) {   // a clip set some other way while the menu is open
            RECT clip{}, screen{GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN), 0, 0};
            screen.right = screen.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
            screen.bottom = screen.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
            if (GetClipCursor(&clip) && !EqualRect(&clip, &screen)) g_real_clip(nullptr);
        }
        if (s) {   // for the X11 menu: its swap chain's X window need not be the one X gives the focus
            DWORD pid = 0;
            const HWND fg = GetForegroundWindow();
            if (fg) GetWindowThreadProcessId(fg, &pid);
            s->game_focused = fg && pid == GetCurrentProcessId() ? 1u : 0u;
        }
        if (s && s->wayland) {
            if (!g_window || !IsWindow(g_window)) {
                g_window = game_window();
                if (g_window) {
                    g_previous = reinterpret_cast<WNDPROC>(
                        SetWindowLongPtrW(g_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&window_proc)));
                    log("[nr] NR Control: its input comes from the game's window (the menu is drawn by our DLL, or by the layer on a Wayland surface)");
                }
            }
            if (g_window) {
                CURSORINFO ci{};
                ci.cbSize = sizeof ci;
                s->cursor_shown = GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING) && ci.hCursor ? 1u : 0u;
                uint32_t keys[8] = {};
                const bool focused = GetForegroundWindow() == g_window;
                for (int vk = 1; vk < 256; ++vk)
                    if (focused && (GetAsyncKeyState(vk) & 0x8000)) keys[vk / 32] |= 1u << (vk % 32);
                std::memcpy(s->keys, keys, sizeof keys);
                POINT p{};
                RECT r{};
                if (GetCursorPos(&p) && ScreenToClient(g_window, &p)) { s->mouse_x = p.x; s->mouse_y = p.y; }
                if (GetClientRect(g_window, &r)) { s->client_w = uint32_t(r.right); s->client_h = uint32_t(r.bottom); }
                s->buttons = (keys[0] & (1u << VK_LBUTTON) ? 1u : 0u) | (keys[0] & (1u << VK_RBUTTON) ? 2u : 0u) |
                             (keys[0] & (1u << VK_MBUTTON) ? 4u : 0u);
                ++s->input_seq;
            }
        }
        Sleep(s && (s->wayland || open) ? 4 : 50);
    }
}

}  // namespace

namespace {
nr::control::LayerStatus* row_of(nr::control::Shared* s, int number, bool make) {
    if (!s || number <= 0) return nullptr;
    for (auto& r : s->layers)
        if (r.number == uint32_t(number)) return &r;
    if (!make) return nullptr;
    for (auto& r : s->layers)
        if (!r.number) { std::memset(&r, 0, sizeof r); r.number = uint32_t(number); r.net = nr::control::kNoNet; return &r; }
    return nullptr;   // more layers than rows: the rest are not shown
}
}  // namespace

// The .asi's chain as chain_layers last had it (g_chain_known: it was ever told): report() makes rows for these only,
// so a run of a layer just taken out cannot bring its row back.
std::mutex g_chain_lock;
std::vector<int> g_chain;
bool g_chain_known = false;
std::vector<int> g_frame_layers;
void present_layers() {
    nr::control::Shared* s = shared(); if (!s) s = page();
    std::lock_guard<std::mutex> g(g_chain_lock);
    for (auto& r : s->layers)
        if (r.number && r.state == 1 && std::find(g_frame_layers.begin(), g_frame_layers.end(), int(r.number)) == g_frame_layers.end()) {
            r.state = 3; r.gpu_ms = r.network_ms = 0.f;
            std::snprintf(r.note, sizeof r.note, "not run on this presented frame");
        }
    g_frame_layers.clear();
}


void chain_layers(const std::vector<int>& numbers) {
    nr::control::Shared* s = shared();
    if (!s) s = page();
    {
        std::lock_guard<std::mutex> g(g_chain_lock);
        g_chain = numbers;
        g_chain_known = true;
    }
    for (auto& r : s->layers)
        if (r.number && std::find(numbers.begin(), numbers.end(), int(r.number)) == numbers.end()) std::memset(&r, 0, sizeof r);
    for (int n : numbers) row_of(s, n, true);
    if (numbers.empty()) {   // no layer runs: nothing of the last run's stays on the page (no report() comes now)
        s->gpu_ms = s->network_ms = 0.f;
        s->width = s->height = s->nr_width = s->nr_height = 0;
        s->render_width = s->render_height = s->output_width = s->output_height = 0;
        s->passes = 0;
        s->net_running = nr::control::kNoNet;
        s->building = 0;
        s->building_net = nr::control::kNoNet;
        s->panel_lines = 0;
        ++s->status_seq;
    }
}

void layer_changed(int number) {
    nr::control::Shared* s = shared();
    if (!s) s = page();
    nr::control::LayerStatus* r = row_of(s, number, false);
    if (!r) return;
    r->net = nr::control::kNoNet;
    r->width = r->height = r->nr_width = r->nr_height = 0;
    r->passes = 0;
    r->gpu_ms = r->network_ms = 0.f;
    r->panel_lines = 0;
    r->updated = 0;
}

void asi_facts(const char* build, const char* frame_gen) {
    nr::control::Shared* s = shared();
    if (!s) s = page();
    if (build) std::snprintf(s->asi_build, sizeof s->asi_build, "%s", build);
    if (frame_gen) std::snprintf(s->frame_gen, sizeof s->frame_gen, "%s", frame_gen);
}

void hud_facts(const char* now, const char* why_game, const char* why_captured, const char* why_estimated) {
    nr::control::Shared* s = shared();
    if (!s) s = page();
    if (now) std::snprintf(s->hud_now, sizeof s->hud_now, "%s", now);
    const char* why[3] = {why_game, why_captured, why_estimated};
    for (int i = 0; i < 3; ++i)
        if (why[i]) std::snprintf(s->hud_why[i], sizeof s->hud_why[i], "%s", why[i]);
}

void layer_ran(int number, uint32_t processing, uint32_t placement, uint32_t state, const char* note) {
    {   // a layer the chain no longer has (a Present that waited for its slot while the chain changed): no row again
        std::lock_guard<std::mutex> g(g_chain_lock);
        if (g_chain_known && std::find(g_chain.begin(), g_chain.end(), number) == g_chain.end()) return;
        if (std::find(g_frame_layers.begin(),g_frame_layers.end(),number)==g_frame_layers.end()) g_frame_layers.push_back(number);
    }
    nr::control::Shared* s = shared();
    if (!s) s = page();
    nr::control::LayerStatus* r = row_of(s, number, true);
    if (!r) return;
    if (r->processing != processing || r->placement != placement) {   // another processing / place: the old readings go
        const uint32_t n = r->number;
        std::memset(r, 0, sizeof *r);
        r->number = n;
        r->net = nr::control::kNoNet;
    }
    r->processing = processing;
    r->placement = placement;
    r->state = state;
    if (state != 1u) r->gpu_ms = r->network_ms = 0.f;
    std::snprintf(r->note, sizeof r->note, "%s", note ? note : "");
    r->updated = uint32_t(GetTickCount64());
    // a skip with no run after it is a state (the run's own result comes through report()); the place noted before a
    // run (state 1, "not recorded yet") is not
    if (state == 3u && std::strcmp(r->note, "not recorded yet") != 0) nr::pe::events::layer_state(number, state, r->note);
}

namespace {
std::once_flag g_watch_started;
uint32_t g_screenshot_seen = 0;
void start_watch_on(nr::control::Shared* s) {
    std::call_once(g_watch_started, [s] {
        g_screenshot_seen = s->screenshot_request;   // a press from before this run is not a request
        std::thread(watch).detach();
    });
}
}  // namespace

void start_input() {
    if (nr::control::Shared* s = shared()) start_watch_on(s);
}

void report(const Status& st) {
    static uint64_t last = 0;
    uint32_t& screenshot_seen = g_screenshot_seen;
    nr::control::Shared* s = shared();
    if (!s) return;
    start_watch_on(s);
    if (s->screenshot_request != screenshot_seen) {
        screenshot_seen = s->screenshot_request;
        nr::pe::screenshot::request();
    }
    const uint64_t now = GetTickCount64();
    {   // a run of a layer the chain no longer has (a frame in flight at the change): dropped, its row stays emptied
        std::lock_guard<std::mutex> g(g_chain_lock);
        if (g_chain_known && st.layer > 0 && std::find(g_chain.begin(), g_chain.end(), st.layer) == g_chain.end()) return;
        if (std::find(g_frame_layers.begin(),g_frame_layers.end(),st.layer)==g_frame_layers.end()) g_frame_layers.push_back(st.layer);
    }
    if (nr::control::LayerStatus* r = row_of(s, st.layer, true)) {   // a layer's row: every run
        r->state = !st.nr_on ? 3u : st.building && st.gpu_ms <= 0.f ? 2u : st.waiting ? 3u : 1u;
        if (!st.nr_on) std::snprintf(r->note, sizeof r->note, "NR is off (toggle key)");
        else if (st.waiting) std::snprintf(r->note, sizeof r->note, "%s", st.waiting_why.c_str());
        else r->note[0] = 0;
        r->net = st.net_running < 0 ? nr::control::kNoNet : uint32_t(st.net_running);
        r->width = st.width; r->height = st.height;
        r->nr_width = st.nr_width; r->nr_height = st.nr_height;
        r->passes = st.passes;
        r->fast_passes = st.fast_passes; r->motion = st.motion;
        if (r->state == 1u && st.fast_fallback)
            std::snprintf(r->note, sizeof r->note, "Extrapolation: no motion vectors");
        r->gpu_ms = st.gpu_ms; r->network_ms = st.network_ms;
        r->updated = uint32_t(now);
        nr::pe::events::layer_state(st.layer, r->state, r->note);
        if (uint32_t(now) - r->panel_at >= 250u) {   // its engine data, four times a second
            r->panel_at = uint32_t(now);
            const size_t lines = std::min<size_t>(st.panel.size(), nr::control::kPanelLines);
            for (size_t i = 0; i < lines; ++i) std::snprintf(r->panel[i], sizeof r->panel[i], "%s", st.panel[i].c_str());
            r->panel_lines = uint32_t(lines);
        }
    }
    if (st.vram_known) {
        s->vram_process_kib = uint32_t(st.vram_usage >> 10);
        s->vram_budget_kib = uint32_t(st.vram_budget >> 10);
        s->vram_heap_kib = uint32_t(st.vram_heap >> 10);
    }
    if (now - last < 250) return;
    last = now;
    {   // NR's own video memory, by network and kind
        nr::vram::install_vulkan();   // again, should the entry points have been loaded anew
        uint64_t by[nr::vram::kOwners][nr::vram::kKinds];
        nr::vram::totals(by);
        for (int o = 0; o < nr::vram::kOwners; ++o)
            for (int k = 0; k < nr::vram::kKinds; ++k) s->vram_kib[o][k] = uint32_t(by[o][k] >> 10);
        s->vram_known = 1;
    }
    std::snprintf(s->screenshot, sizeof s->screenshot, "%s", nr::pe::screenshot::status());
    // NR Control is a Vulkan layer the launch options load: no Present from it 10 s after NR first ran = not loaded.
    static uint64_t first = now;
    static uint32_t seen_seq = s->menu_seq;
    static bool told = false;
    if (s->menu_seq != seen_seq) told = true;   // it is there
    if (s->dll_menu) told = true;               // the .asi draws NR Control itself (a DXGI swap chain): no layer needed
    if (!told && now - first > 10000) {
        told = true;
        // the launch line is read on the Linux side: the folder as a Linux path (Wine's own mapping of any drive;
        // without it Z: is / and other drives stay as they are)
        std::string game = std::string(module_folder()) + "\\dlssnr-amd\\control\\layer";
        using UnixName = char*(__cdecl*)(const wchar_t*);
        const auto unix_name = reinterpret_cast<UnixName>(reinterpret_cast<void*>(
            GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "wine_get_unix_file_name")));
        wchar_t wide[1024];
        char* mapped = nullptr;
        if (unix_name && MultiByteToWideChar(CP_ACP, 0, game.c_str(), -1, wide, 1024) > 0) mapped = unix_name(wide);
        if (mapped) {
            game = mapped;
            HeapFree(GetProcessHeap(), 0, mapped);
        } else {
            if (game.size() > 2 && (game[0] == 'Z' || game[0] == 'z') && game[1] == ':') game.erase(0, 2);
            for (char& c : game) if (c == '\\') c = '/';
        }
        log("[nr] NR Control (End) is not loaded: a Vulkan game needs VK_ADD_LAYER_PATH=\"%s\" "
            "and VK_LAYER_DLSSNR_control in VK_INSTANCE_LAYERS in its launch options (install.sh prints the whole line)",
            game.c_str());
    }
    s->route = st.route;
    s->renodx = st.renodx;
    s->nr_on = st.nr_on;
    s->gpu_ms = st.gpu_ms;
    s->network_ms = st.network_ms;
    s->width = st.width; s->height = st.height;
    s->nr_width = st.nr_width; s->nr_height = st.nr_height;
    s->render_width = st.render_width; s->render_height = st.render_height;
    s->placement = st.placement;
    s->output_width = st.output_width; s->output_height = st.output_height;
    s->passes = st.passes;
    s->net_running = st.net_running < 0 ? nr::control::kNoNet : uint32_t(st.net_running);
    s->net_want = st.net_want < 0 ? nr::control::kNoNet : uint32_t(st.net_want);
    for (int n = 0; n < nr::kNetworkChoices; ++n) {
        std::snprintf(s->net_why[n], sizeof s->net_why[n], "%s", st.net_why[n].c_str());
    }
    s->net_known = st.net_want >= 0;
    s->preprocess = st.preprocess;
    s->building = st.building;
    s->building_net = st.building && st.building_net >= 0 ? uint32_t(st.building_net) : nr::control::kNoNet;
    s->build_progress = st.build_progress;
    std::snprintf(s->build_phase, sizeof s->build_phase, "%s", st.build_phase.c_str());
    const size_t lines = std::min<size_t>(st.panel.size(), nr::control::kPanelLines);
    for (size_t i = 0; i < lines; ++i) std::snprintf(s->panel[i], sizeof s->panel[i], "%s", st.panel[i].c_str());
    s->panel_lines = uint32_t(lines);
    ++s->status_seq;
}

nr::control::Shared* page() {
    if (nr::control::Shared* s = shared()) return s;
    static nr::control::Shared* own = [] {
        auto* s = new nr::control::Shared{};
        s->version = nr::control::kVersion;
        s->magic = nr::control::kMagic;
        s->net_running = s->net_want = s->building_net = nr::control::kNoNet;   // none yet
        return s;
    }();
    return own;
}

}  // namespace nr::pe::control
