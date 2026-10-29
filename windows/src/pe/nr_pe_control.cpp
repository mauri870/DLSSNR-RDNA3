// See nr_pe_control.hpp.
#include "nr_pe_control.hpp"
#include "nr_control_shared.hpp"
#include "nr_pe_config.hpp"
#include "nr_pe_log.hpp"
#include "nr_screenshot.hpp"

#include <windows.h>
#include "MinHook.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>

namespace nr::pe::control {
namespace {
nr::control::Shared* shared();
}
nr::control::Shared* page() { return shared(); }
namespace {

nr::control::Shared* g_shared = nullptr;

// The menu is drawn by this DLL (nr_pe_menu.cpp), so the page is plain memory of the process.
nr::control::Shared* shared() {
    static nr::control::Shared* page = [] {
        auto* s = new nr::control::Shared{};
        s->version = nr::control::kVersion;
        s->magic = nr::control::kMagic;
        s->net_running = s->net_want = s->building_net = 3;   // none yet (nr::NetworkChoice)
        g_shared = s;
        return s;
    }();
    return page;
}

// ---- the game window's input for the menu ----
HWND g_window = nullptr;
WNDPROC g_previous = nullptr;

// While the menu is open the game gets none of the keyboard and mouse, as under ReShade's overlay: its window messages
// are dropped (window_proc), and what it would read directly is held still - the cursor position where it was when
// the menu opened, raw mouse and keyboard input empty, every key up. A game that hides the cursor and clips it (mouse
// look) keeps putting the cursor back in the middle of its window, so the clip is lifted and the game's ClipCursor /
// SetCursorPos wait; its last clip is put back when the menu closes.
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
    // Each on its own: one that fails leaves the others. Windows exports some of these as one function (its
    // GetPhysicalCursorPos is GetCursorPos): hooked once, which serves both.
    std::vector<void*> done;
    for (const Hook& h : hooks) {
        void* at = ok ? reinterpret_cast<void*>(GetProcAddress(user32, h.name)) : nullptr;
        if (!at) continue;
        if (std::find(done.begin(), done.end(), at) != done.end()) continue;
        if (MH_CreateHook(at, h.detour, h.real) == MH_OK && MH_EnableHook(at) == MH_OK) done.push_back(at);
        else log("[nr] NR Control: %s could not be hooked; the game may still see it under the menu", h.name);
    }
    // the cursor's own three, which closing the menu needs (where the game's clip and cursor are put back)
    ok = ok && g_real_clip && g_real_setpos && g_real_getpos;
    if (!g_real_physpos) g_real_physpos = g_real_getpos;
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
                    log("[nr] NR Control: the menu's input comes from the game's window");
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

void report(const Status& st) {
    static uint64_t last = 0;
    static std::once_flag started;
    static uint32_t screenshot_seen = 0;
    nr::control::Shared* s = shared();
    if (!s) return;
    std::call_once(started, [s] {
        screenshot_seen = s->screenshot_request;   // a press from before this run is not a request
        std::thread(watch).detach();
    });
    if (s->screenshot_request != screenshot_seen) {
        screenshot_seen = s->screenshot_request;
        nr::pe::screenshot::request();
    }
    const uint64_t now = GetTickCount64();
    if (now - last < 250) return;
    last = now;
    std::snprintf(s->screenshot, sizeof s->screenshot, "%s", nr::pe::screenshot::status());
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
    s->net_running = st.net_running < 0 ? 3u : uint32_t(st.net_running);
    s->net_want = st.net_want < 0 ? 3u : uint32_t(st.net_want);
    for (int n = 0; n < 3; ++n) {
        std::snprintf(s->net_why[n], sizeof s->net_why[n], "%s", st.net_why[n].c_str());
    }
    s->net_known = st.net_want >= 0;
    s->preprocess = st.preprocess;
    s->building = st.building;
    s->building_net = st.building && st.building_net >= 0 ? uint32_t(st.building_net) : 3u;
    s->build_progress = st.build_progress;
    const size_t lines = std::min<size_t>(st.panel.size(), nr::control::kPanelLines);
    for (size_t i = 0; i < lines; ++i) std::snprintf(s->panel[i], sizeof s->panel[i], "%s", st.panel[i].c_str());
    s->panel_lines = uint32_t(lines);
    ++s->status_seq;
}

}  // namespace nr::pe::control
