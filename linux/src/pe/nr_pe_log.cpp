#include "nr_pe_log.hpp"

#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>

namespace nr::pe {
namespace {

std::mutex log_lock;
HMODULE self_module = nullptr;

// Where this module is, by whichever route works. The loader's own handle first;
// deriving one from an address inside us second, because that is what the code
// used to do and it is right when it works; the executable's own folder last,
// which is where a drop-in module was copied to anyway.
std::string folder_of(HMODULE module) {
    char path[MAX_PATH]{};
    if (!GetModuleFileNameA(module, path, MAX_PATH)) return {};
    std::string s = path;
    const auto slash = s.find_last_of("\\/");
    return slash == std::string::npos ? std::string{} : s.substr(0, slash);
}

std::string resolve_folder() {
    HMODULE self = self_module;
    if (!self)
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&resolve_folder), &self);
    if (self) {
        std::string folder = folder_of(self);
        if (!folder.empty()) return folder;
    }
    // Last resort: the executable's own directory, which is where a drop-in
    // module was copied to in the first place.
    return folder_of(nullptr);
}

std::string resolve_path() {
    const std::string folder = resolve_folder();
    return folder.empty() ? std::string("dlssnr-amd.log")
                          : folder + "\\dlssnr-amd.log";
}

// [Log] in dlssnr-amd.ini beside this module, read once, before the first line: Enabled (0: no log
// at all) and ClearOnStart (1: the first module of this process to log starts the file afresh).
// Absent file, section or key: both on.
struct LogSettings { bool enabled = true, clear = true; };
LogSettings read_settings() {
    LogSettings s;
    const std::string folder = resolve_folder();
    FILE* f = std::fopen((folder.empty() ? std::string("dlssnr-amd.ini") : folder + "\\dlssnr-amd.ini").c_str(), "r");
    if (!f) return s;
    auto trim = [](std::string t) {
        const auto b = t.find_first_not_of(" \t\r\n"), e = t.find_last_not_of(" \t\r\n");
        return b == std::string::npos ? std::string{} : t.substr(b, e - b + 1);
    };
    auto lower = [](std::string t) { for (char& c : t) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a'); return t; };
    bool in_log = false;
    char line[512];
    while (std::fgets(line, sizeof line, f)) {
        std::string t = line;
        const auto comment = t.find_first_of(";#");
        if (comment != std::string::npos) t = t.substr(0, comment);
        t = trim(t);
        if (t.empty()) continue;
        if (t.front() == '[') { in_log = lower(t) == "[log]"; continue; }
        const auto eq = t.find('=');
        if (!in_log || eq == std::string::npos) continue;
        const std::string key = lower(trim(t.substr(0, eq))), v = lower(trim(t.substr(eq + 1)));
        const bool on = v == "1" || v == "true" || v == "on" || v == "yes";
        const bool off = v == "0" || v == "false" || v == "off" || v == "no";
        if (!on && !off) continue;
        if (key == "enabled") s.enabled = on;
        else if (key == "clearonstart") s.clear = on;
    }
    std::fclose(f);
    return s;
}

FILE* open_log() {
    const LogSettings s = read_settings();
    if (!s.enabled) return nullptr;
    // Several of our modules can live in one process (the NGX forwarder and the core); only the
    // first to log may start the file afresh, or it would erase what the other already wrote.
    char seen[4];
    const bool first = GetEnvironmentVariableA("NR_LOG_OPENED", seen, sizeof seen) == 0;
    if (first) SetEnvironmentVariableA("NR_LOG_OPENED", "1");
    return std::fopen(resolve_path().c_str(), s.clear && first ? "w" : "a");
}

FILE* file() {
    static FILE* handle = open_log();
    return handle;
}

}  // namespace

void set_module(void* module) { self_module = static_cast<HMODULE>(module); }

const char* module_folder() {
    static std::string folder = resolve_folder();
    return folder.c_str();
}

const char* log_path() {
    static std::string path = resolve_path();
    return path.c_str();
}

void log(const char* format, ...) {
    std::lock_guard<std::mutex> guard(log_lock);
    FILE* handle = file();
    if (!handle) return;
    va_list args; va_start(args, format);
    std::vfprintf(handle, format, args);
    va_end(args);
    std::fputc('\n', handle);
    std::fflush(handle);
}

}  // namespace nr::pe

// ---- the stall report (nr_pe_watchdog.hpp) -------------------------------------------------------------------------
#include "nr_pe_watchdog.hpp"

#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <cwchar>
#include <map>
#include <vector>

namespace nr::pe::watchdog {
namespace {

// The steps threads are in (Phase): a fixed table, written without locks.
struct Slot {
    std::atomic<DWORD> tid{0};
    std::atomic<const char*> what{nullptr};
    std::atomic<ULONGLONG> since{0};
};
Slot g_slots[64];
thread_local Slot* t_slot = nullptr;

Slot* my_slot() {
    if (t_slot) return t_slot;
    const DWORD me = GetCurrentThreadId();
    for (Slot& s : g_slots) {
        DWORD free = 0;
        if (s.tid.compare_exchange_strong(free, me)) return t_slot = &s;
    }
    return nullptr;   // more threads in steps at once than slots: this one is not shown
}

std::atomic<ULONGLONG> g_last_beat{0};
std::atomic<unsigned long long> g_beats{0};
std::atomic<int> g_reports{0};
constexpr int kMaxReports = 4;
std::mutex g_thread_lock;      // the watching thread's start and stop
HANDLE g_stop = nullptr;       // the running watcher's stop event (null: none runs; g_thread_lock)
std::atomic<bool> g_running{false};   // a watcher runs (beat() reads it without the lock)
HANDLE g_thread = nullptr;     // the running watcher (for stop_and_wait)
bool g_closed = false;         // stop_and_wait ran: the module is about to be unloaded, nothing starts again

bool enabled() {
    static const bool on = [] {
        char v[8] = {};
        return !(GetEnvironmentVariableA("NR_WATCHDOG", v, sizeof v) && v[0] == '0');
    }();
    return on;
}
ULONGLONG stall_ms() {
    static const ULONGLONG ms = [] {
        char v[16] = {};
        const unsigned long long s = GetEnvironmentVariableA("NR_WATCHDOG_SECS", v, sizeof v) ? std::strtoull(v, nullptr, 10) : 0;
        return s >= 2 ? ULONGLONG(s) * 1000 : ULONGLONG(8000);
    }();
    return ms;
}

// A module and its code sections, for telling return addresses from other words on a stack. Found from the address
// itself (VirtualQuery: an image mapping's allocation base is the module), named by the mapped file
// (K32GetMappedFileNameW) - never through the loader (GetModuleFileName, GetModuleHandleEx, EnumProcessModules,
// GetProcAddress take the loader lock in Wine, and a freeze is exactly when a stuck thread may hold it).
struct Module {
    uintptr_t base = 0, end = 0;
    std::vector<std::pair<uintptr_t, uintptr_t>> code;
    std::string name;
    std::vector<std::pair<uint32_t, std::string>> exports;   // by address, read when first asked
    bool exports_read = false;
};
using MappedName = DWORD(WINAPI*)(HANDLE, LPVOID, LPWSTR, DWORD);
using Describe = HRESULT(WINAPI*)(HANDLE, PWSTR*);
MappedName g_mapped_name = nullptr;   // resolved when the watcher starts, not during a report
Describe g_describe = nullptr;

bool readable(uintptr_t a, size_t n) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof mbi) || mbi.State != MEM_COMMIT ||
        (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) || !mbi.Protect)
        return false;
    return a + n <= uintptr_t(mbi.BaseAddress) + mbi.RegionSize;
}

struct Modules {
    std::map<uintptr_t, Module> by_base;
    std::map<uintptr_t, uintptr_t> region_base;   // 64 KiB region -> its image's base (0: not an image)

    const Module* at(uintptr_t a) {
        if (a < 0x10000) return nullptr;
        const uintptr_t region = a >> 16;
        auto r = region_base.find(region);
        uintptr_t base = 0;
        if (r != region_base.end()) {
            base = r->second;
        } else {
            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof mbi) && mbi.Type == MEM_IMAGE)
                base = uintptr_t(mbi.AllocationBase);
            region_base[region] = base;
        }
        if (!base) return nullptr;
        auto it = by_base.find(base);
        if (it == by_base.end()) it = by_base.emplace(base, load(base)).first;
        const Module& m = it->second;
        return m.end && a >= m.base && a < m.end ? &m : nullptr;
    }
    Module* find(uintptr_t a) { return const_cast<Module*>(at(a)); }

    static Module load(uintptr_t base) {
        Module m;
        if (!readable(base, sizeof(IMAGE_DOS_HEADER))) return m;
        auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || !readable(base + dos->e_lfanew, sizeof(IMAGE_NT_HEADERS))) return m;
        auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return m;
        m.base = base;
        m.end = base + nt->OptionalHeader.SizeOfImage;
        const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections && readable(uintptr_t(sec), sizeof *sec); ++i, ++sec)
            if (sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)
                m.code.emplace_back(base + sec->VirtualAddress, base + sec->VirtualAddress + sec->Misc.VirtualSize);
        wchar_t path[MAX_PATH] = {};
        if (g_mapped_name && g_mapped_name(GetCurrentProcess(), reinterpret_cast<void*>(base), path, MAX_PATH)) {
            const wchar_t* slash = std::wcsrchr(path, L'\\');
            char n[MAX_PATH] = {};
            WideCharToMultiByte(CP_UTF8, 0, slash ? slash + 1 : path, -1, n, sizeof n - 1, nullptr, nullptr);
            m.name = n;
        }
        if (m.name.empty()) {
            char n[32];
            std::snprintf(n, sizeof n, "image@%p", reinterpret_cast<void*>(base));
            m.name = n;
        }
        return m;
    }
};

bool in_code(const Module& m, uintptr_t a, uintptr_t* section_start) {
    for (const auto& [b, e] : m.code)
        if (a >= b && a < e) { *section_start = b; return true; }
    return false;
}

// Whether the bytes before `a` end in a call instruction (a return address, not any word that points into code).
bool after_call(uintptr_t a, uintptr_t section_start) {
    if (a < section_start + 7 || !readable(a - 7, 7)) return false;
    const auto* p = reinterpret_cast<const uint8_t*>(a);
    if (p[-5] == 0xE8) return true;                                            // call rel32
    if (p[-6] == 0xFF && (p[-5] == 0x15 || (p[-5] & 0xF8) == 0x90)) return true;   // call [rip+d32] / [reg+d32]
    if (p[-7] == 0xFF && (p[-6] == 0x94 || (p[-6] == 0x14 && p[-5] == 0x25))) return true;
    if (p[-2] == 0xFF && ((p[-1] & 0xF8) == 0xD0 || (p[-1] & 0xF8) == 0x10)) return true;   // call reg / [reg]
    if (p[-3] == 0xFF && ((p[-2] & 0xF8) == 0x50 || p[-2] == 0x14)) return true;           // call [reg+d8] / [sib]
    if (p[-4] == 0xFF && p[-3] == 0x54) return true;                                       // call [sib+d8]
    if (p[-3] == 0x41 && p[-2] == 0xFF && ((p[-1] & 0xF8) == 0xD0 || (p[-1] & 0xF8) == 0x10)) return true;
    if (p[-4] == 0x41 && p[-3] == 0xFF && (p[-2] & 0xF8) == 0x50) return true;
    if (p[-7] == 0x41 && p[-6] == 0xFF && (p[-5] & 0xF8) == 0x90) return true;
    return false;
}

void read_exports(Module& m) {
    m.exports_read = true;
    const uintptr_t base = m.base;
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dir.VirtualAddress || !dir.Size || !readable(base + dir.VirtualAddress, sizeof(IMAGE_EXPORT_DIRECTORY))) return;
    auto* ex = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + dir.VirtualAddress);
    auto* names = reinterpret_cast<const uint32_t*>(base + ex->AddressOfNames);
    auto* ordinals = reinterpret_cast<const uint16_t*>(base + ex->AddressOfNameOrdinals);
    auto* functions = reinterpret_cast<const uint32_t*>(base + ex->AddressOfFunctions);
    if (!readable(uintptr_t(names), size_t(ex->NumberOfNames) * 4) ||
        !readable(uintptr_t(ordinals), size_t(ex->NumberOfNames) * 2) ||
        !readable(uintptr_t(functions), size_t(ex->NumberOfFunctions) * 4))
        return;
    for (uint32_t i = 0; i < ex->NumberOfNames && i < 100000; ++i) {
        if (ordinals[i] >= ex->NumberOfFunctions) continue;
        const uint32_t rva = functions[ordinals[i]];
        if (rva >= dir.VirtualAddress && rva < dir.VirtualAddress + dir.Size) continue;   // a forwarder
        if (!readable(base + names[i], 1)) continue;
        m.exports.emplace_back(rva, reinterpret_cast<const char*>(base + names[i]));
    }
    std::sort(m.exports.begin(), m.exports.end());
}

std::string where(Modules& mods, uintptr_t a) {
    char buf[200];
    Module* m = mods.find(a);
    if (!m) { std::snprintf(buf, sizeof buf, "%p", reinterpret_cast<void*>(a)); return buf; }
    const uint32_t rva = uint32_t(a - m->base);
    if (!m->exports_read) read_exports(*m);
    auto it = std::upper_bound(m->exports.begin(), m->exports.end(), rva,
                               [](uint32_t x, const std::pair<uint32_t, std::string>& e) { return x < e.first; });
    if (it != m->exports.begin() && rva - (it - 1)->first < 0x10000)
        std::snprintf(buf, sizeof buf, "%s+0x%x (%s+0x%x)", m->name.c_str(), rva, (it - 1)->second.c_str(), rva - (it - 1)->first);
    else
        std::snprintf(buf, sizeof buf, "%s+0x%x", m->name.c_str(), rva);
    return buf;
}

std::string thread_name(HANDLE h) {
    if (!g_describe) return {};
    PWSTR w = nullptr;
    if (FAILED(g_describe(h, &w)) || !w) return {};
    char n[128] = {};
    WideCharToMultiByte(CP_UTF8, 0, w, -1, n, sizeof n - 1, nullptr, nullptr);
    LocalFree(w);
    return n;
}

constexpr size_t kStackCopy = 256 * 1024;
uint8_t g_stack[kStackCopy];   // one thread's stack at a time, copied while it is suspended (nothing allocated then)

void report(const char* why) {
    if (g_reports.fetch_add(1) >= kMaxReports) return;
    log("[nr] watchdog: %s; every thread of the process follows (stacks as return addresses found on them)", why);
    const ULONGLONG now = GetTickCount64();
    for (Slot& s : g_slots) {
        const char* what = s.what.load();
        if (what) log("[nr] watchdog: thread %lu is in NR's step \"%s\" for %llu ms", (unsigned long)s.tid.load(), what,
                      (unsigned long long)(now - s.since.load()));
    }
    Modules mods;
    std::vector<DWORD> tids;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        THREADENTRY32 te{};
        te.dwSize = sizeof te;
        for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
            if (te.th32OwnerProcessID == GetCurrentProcessId() && te.th32ThreadID != GetCurrentThreadId())
                tids.push_back(te.th32ThreadID);
        CloseHandle(snap);
    }
    tids.reserve(tids.size());
    std::vector<uintptr_t> frames;
    frames.reserve(64);
    for (DWORD tid : tids) {
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
        if (!h) continue;
        const std::string name = thread_name(h);
        if (SuspendThread(h) == DWORD(-1)) { CloseHandle(h); continue; }
        // suspended: no allocation, no lock, until ResumeThread
        CONTEXT ctx;
        std::memset(&ctx, 0, sizeof ctx);
        ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        const bool have = GetThreadContext(h, &ctx) != 0;
#if defined(_WIN64)
        const uintptr_t sp = have ? uintptr_t(ctx.Rsp) : 0, ip = have ? uintptr_t(ctx.Rip) : 0;
#else
        const uintptr_t sp = have ? uintptr_t(ctx.Esp) : 0, ip = have ? uintptr_t(ctx.Eip) : 0;
#endif
        size_t copied = 0;
        MEMORY_BASIC_INFORMATION mbi{};
        if (sp && VirtualQuery(reinterpret_cast<void*>(sp), &mbi, sizeof mbi) && mbi.State == MEM_COMMIT &&
            !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
            const uintptr_t end = uintptr_t(mbi.BaseAddress) + mbi.RegionSize;
            copied = std::min<size_t>(end - sp, kStackCopy);
            std::memcpy(g_stack, reinterpret_cast<void*>(sp), copied);
        }
        ResumeThread(h);
        CloseHandle(h);
        frames.clear();
        for (size_t o = 0; o + sizeof(uintptr_t) <= copied && frames.size() < 40; o += sizeof(uintptr_t)) {
            uintptr_t v;
            std::memcpy(&v, g_stack + o, sizeof v);
            const Module* m = mods.at(v);
            uintptr_t section = 0;
            if (m && in_code(*m, v, &section) && after_call(v, section)) frames.push_back(v);
        }
        std::string line = "[nr] watchdog: thread " + std::to_string(tid) + (name.empty() ? "" : " \"" + name + "\"") +
                           " at " + (have ? where(mods, ip) : std::string("? (no context)"));
        for (size_t i = 0; i < frames.size(); ++i) {
            line += (i ? " < " : "; called from ") + where(mods, frames[i]);
            if (line.size() > 900) { log("%s", line.c_str()); line = "[nr] watchdog:   ..."; }
        }
        log("%s", line.c_str());
    }
    log("[nr] watchdog: end of the report");
}

struct WatchStart { HANDLE stop; HMODULE self; };
DWORD WINAPI watch(void* param) {
    // the module was pinned by start() before this thread existed; let go by FreeLibraryAndExitThread at the end
    const WatchStart ws = *static_cast<WatchStart*>(param);
    delete static_cast<WatchStart*>(param);
    HANDLE stop = ws.stop;
    HMODULE self = ws.self;
    // what a report needs from the loader, asked now (a report must not: a stuck thread may hold the loader lock)
    if (HMODULE k32 = GetModuleHandleW(L"kernel32.dll")) {
        g_mapped_name = reinterpret_cast<MappedName>(reinterpret_cast<void*>(GetProcAddress(k32, "K32GetMappedFileNameW")));
        g_describe = reinterpret_cast<Describe>(reinterpret_cast<void*>(GetProcAddress(k32, "GetThreadDescription")));
    }
    const ULONGLONG started = GetTickCount64();
    bool said_none = false, stalled = false;
    // Frames stop while the game is not in front (minimised, the desktop in front): no stall then. The time without a
    // frame counts from the later of the last frame and the last second the game was not in front.
    ULONGLONG not_front = 0;
    const auto in_front = [] {   // no window in front known: taken as in front (headless desktops)
        DWORD pid = 0;
        const HWND w = GetForegroundWindow();
        return !w || !GetWindowThreadProcessId(w, &pid) || pid == GetCurrentProcessId();
    };
    while (WaitForSingleObject(stop, 1000) == WAIT_TIMEOUT) {
        if (g_reports.load() >= kMaxReports) break;
        const ULONGLONG now = GetTickCount64();
        if (!in_front()) not_front = now;
        const unsigned long long beats = g_beats.load();
        if (!beats) {
            if (!said_none && now - started >= 60000) {
                said_none = true;
                report("no frame (Present) in the minute since NR started");
            }
            continue;
        }
        const ULONGLONG gap = now - std::max(g_last_beat.load(), not_front);
        if (gap < stall_ms()) { stalled = false; continue; }
        if (stalled) continue;
        stalled = true;
        char why[128];
        std::snprintf(why, sizeof why, "no frame (Present) for %llu ms, after %llu frames", (unsigned long long)gap, beats);
        report(why);
    }
    {
        std::lock_guard<std::mutex> guard(g_thread_lock);
        if (g_stop == stop) { g_stop = nullptr; g_running.store(false); }
    }
    CloseHandle(stop);
    if (self) FreeLibraryAndExitThread(self, 0);
    return 0;
}

}  // namespace

void start() {
    if (!enabled() || g_reports.load() >= kMaxReports) return;
    std::lock_guard<std::mutex> guard(g_thread_lock);
    if (g_stop || g_closed) return;
    HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stop) return;
    // the module pinned here, before the thread exists: an unload right after start() cannot take the code from under
    // a thread that has not run yet (the thread lets it go as it ends)
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&watch), &self);
    auto* ws = new WatchStart{stop, self};
    HANDLE t = CreateThread(nullptr, 0, &watch, ws, 0, nullptr);
    if (!t) {
        delete ws;
        CloseHandle(stop);
        if (self) FreeLibrary(self);
        return;
    }
    if (g_thread) CloseHandle(g_thread);
    g_thread = t;
    g_stop = stop;
    g_running.store(true);
}

void stop_and_wait() {
    HANDLE t = nullptr;
    {
        std::lock_guard<std::mutex> guard(g_thread_lock);
        if (g_stop) SetEvent(g_stop);   // the watcher closes it
        g_stop = nullptr;
        g_running.store(false);
        g_closed = true;
        t = g_thread;
        g_thread = nullptr;
    }
    if (!t) return;
    // It ends within a second, or a report's length. Bounded: a watcher that does not end keeps the module loaded
    // (ReShade then says the add-on "was not unregistered" and keeps it), which is better than a game that hangs here.
    WaitForSingleObject(t, 3000);
    CloseHandle(t);
}

void beat() {
    g_last_beat.store(GetTickCount64());
    g_beats.fetch_add(1);
    if (!g_running.load()) start();
}

Phase::Phase(const char* what) : previous(nullptr) {
    Slot* s = my_slot();
    if (!s) return;
    previous = s->what.load();
    s->since.store(GetTickCount64());
    s->what.store(what);
}

Phase::~Phase() {
    Slot* s = t_slot;
    if (!s) return;
    s->what.store(previous);
    if (!previous) { s->tid.store(0); t_slot = nullptr; }
}

}  // namespace nr::pe::watchdog
