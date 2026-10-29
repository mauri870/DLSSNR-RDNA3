#include "nr_pe_optifix.hpp"

#include "nr_pe_config.hpp"
#include "nr_pe_log.hpp"
#include "nr_screenshot.hpp"
#include "nr_pe_renodx_route.hpp"

#include <windows.h>

#include <climits>
#include <cstdint>
#include <cstdlib>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <initializer_list>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <d3d12.h>

#include "MinHook.h"

namespace nr::pe {

#if defined(_WIN64)
namespace {

struct Module {
    const wchar_t* name;
    uint8_t* base;
    size_t image;
};

// OptiScaler is dxgi.dll in both packaged routes; OptiScaler.dll covers a manual install.
bool find_module(Module* out) {
    static const wchar_t* const kModules[] = {L"dxgi.dll", L"OptiScaler.dll"};
    for (const wchar_t* name : kModules) {
        auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(name));
        if (!base) continue;
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) continue;
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) continue;
        *out = {name, base, nt->OptionalHeader.SizeOfImage};
        return true;
    }
    return false;
}

bool matches(const uint8_t* p, const uint8_t* pat, const char* mask, size_t n) {
    for (size_t k = 0; k < n; ++k)
        if (mask[k] != '?' && p[k] != pat[k]) return false;
    return true;
}

// Every executable-section position where `pat` matches and `accept` agrees. Only a unique hit is
// ever patched.
template <class Accept>
int find_unique(const Module& m, const uint8_t* pat, const char* mask, size_t n, Accept accept, uint8_t** hit) {
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(m.base + reinterpret_cast<IMAGE_DOS_HEADER*>(m.base)->e_lfanew);
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    int hits = 0;
    for (unsigned s = 0; s < nt->FileHeader.NumberOfSections; ++s, ++sec) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        uint8_t* p = m.base + sec->VirtualAddress;
        const size_t len = sec->Misc.VirtualSize;
        for (size_t i = 0; i + n <= len; ++i)
            if (matches(p + i, pat, mask, n) && accept(p + i)) { *hit = p + i; ++hits; }
    }
    return hits;
}

uint8_t* rel32_target(uint8_t* insn_end_minus4) {
    int32_t d;
    std::memcpy(&d, insn_end_minus4, 4);
    return insn_end_minus4 + 4 + d;
}

bool write_code(uint8_t* at, const void* bytes, size_t n) {
    DWORD old = 0;
    if (!VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &old)) return false;
    std::memcpy(at, bytes, n);
    VirtualProtect(at, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, n);
    return true;
}

// A signature written as hex bytes, "??" for any byte.
struct Sig {
    std::vector<uint8_t> bytes;
    std::string mask;
    explicit Sig(const char* text) {
        for (const char* p = text; *p;) {
            if (*p == ' ') { ++p; continue; }
            if (p[0] == '?') { bytes.push_back(0); mask += '?'; p += 2; continue; }
            bytes.push_back(static_cast<uint8_t>(std::strtoul(std::string(p, 2).c_str(), nullptr, 16)));
            mask += 'x';
            p += 2;
        }
    }
};

template <class Accept>
int find_unique(const Module& m, const Sig& sig, Accept accept, uint8_t** hit) {
    return find_unique(m, sig.bytes.data(), sig.mask.c_str(), sig.bytes.size(), accept, hit);
}

// Code bytes in fresh executable memory; null if it could not be allocated.
uint8_t* place_code(const uint8_t* code, size_t n) {
    auto* p = static_cast<uint8_t*>(VirtualAlloc(nullptr, n, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!p) return nullptr;
    std::memcpy(p, code, n);
    DWORD old = 0;
    VirtualProtect(p, n, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), p, n);
    return p;
}

// ---- 1. The device-extension query (see the header) ----------------------------------------------
void fix_extension_query(const Module& m) {
    // SupportedDeviceExtensions, OptiScaler-NR v0.8.4 (8802b2b4), MSVC x64:
    //   test rdx,rdx / je        getInstanceProcAddr == nullptr
    //   test r9,r9   / je        physicalDevice == VK_NULL_HANDLE
    //   lea rdx,[rip+name]       "vkEnumerateDeviceExtensionProperties"
    //   mov rcx,r8               instance            <- becomes xor ecx,ecx; nop
    //   call rax / mov rbp,rax / test rax,rax
    static const uint8_t pat[] = {
        0x48, 0x85, 0xD2, 0x0F, 0x84, 0, 0, 0, 0,
        0x4D, 0x85, 0xC9, 0x0F, 0x84, 0, 0, 0, 0,
        0x48, 0x8D, 0x15, 0, 0, 0, 0,
        0x49, 0x8B, 0xC8, 0xFF, 0xD0, 0x48, 0x8B, 0xE8, 0x48, 0x85, 0xC0};
    static const char mask[] = "xxxxx????xxxxx????xxx????xxxxxxxxxxx";
    static_assert(sizeof(pat) == sizeof(mask) - 1, "pattern and mask lengths");
    constexpr size_t kLea = 18, kMov = 25;
    static const char kName[] = "vkEnumerateDeviceExtensionProperties";

    uint8_t* hit = nullptr;
    const int hits = find_unique(m, pat, mask, sizeof(pat), [&](uint8_t* p) {
        const uint8_t* str = rel32_target(p + kLea + 3);
        return str >= m.base && str + sizeof(kName) <= m.base + m.image &&
               std::memcmp(str, kName, sizeof(kName)) == 0;
    }, &hit);
    if (hits != 1) {
        log("[nr] OptiScaler fix (device extensions): %d matching sites in %ls, nothing changed", hits, m.name);
        return;
    }
    static const uint8_t fix[] = {0x33, 0xC9, 0x90};   // xor ecx,ecx ; nop
    if (!write_code(hit + kMov, fix, sizeof(fix))) {
        log("[nr] OptiScaler fix (device extensions): VirtualProtect failed (%lu)", GetLastError());
        return;
    }
    log("[nr] OptiScaler fix: device-extension query no longer uses the stored VkInstance (%ls+0x%llx)",
        m.name, static_cast<unsigned long long>(hit + kMov - m.base));
}

// ---- 2. Finished Picture under DXVK / vkd3d-proton ------------------------------------------------
//
// wrapped_swapchain.cpp LocalPresent returns early on DXVK ("DXVK check, it's here because of
// upscaler time calculations"): it calls the real Present and returns before the native path's
//
//     if (cq && (fg == nullptr || !fg->IsActive() || fg->IsPaused()))
//         DlssNr::ApplyToFinishedPicture(pSwapChain, cq);
//
// so under Proton - every OptiScaler install of this project - [DlssNr] FinishedPicture never runs:
// the slots armed at the upscaler are never consumed and the option sits at "Waiting for the previous
// picture to finish.", with NR off. The fix gives the DXVK path the same call, under the same
// condition, before its Present, and changes nothing else (no D3D overlay, no GPU-time reads, the
// frame counter where it was).
//
// v0.8.4 (8802b2b4), register allocation of LocalPresent at that point: r15 = the swapchain,
// r14d/r13d = SyncInterval/Flags, r12b = willPresent, rdi = State::currentFG, [rbp+0x80] = cq.
// Both the DXVK branch and the native block are matched byte for byte (the native block is where
// ApplyToFinishedPicture's address and the FG field offsets come from), so any other build is left
// alone. v0.8.91 has its own layout below (finished_picture_0891). Each returns its number of matching
// sites; the fix is applied only at exactly one.
int finished_picture_084(const Module& m) {
    // test bl,bl (usesDxvk) / je native / the DXVK branch's first five instructions, then its
    // Present call: mov rax,[r15]; mov r9,[rbp-0x50]; mov r8d,r13d; mov edx,r14d; mov rcx,r15;
    // test r9,r9; jne +5; call [rax+0x40]
    static const uint8_t branch[] = {
        0x84, 0xDB, 0x0F, 0x84, 0, 0, 0, 0,
        0x49, 0x8B, 0x07, 0x4C, 0x8B, 0x4D, 0xB0, 0x45, 0x8B, 0xC5, 0x41, 0x8B, 0xD6, 0x49, 0x8B, 0xCF,
        0x4D, 0x85, 0xC9, 0x75, 0x05, 0xFF, 0x50, 0x40};
    static const char branch_mask[] = "xxxx????xxxxxxxxxxxxxxxxxxxxxxxx";
    static_assert(sizeof(branch) == sizeof(branch_mask) - 1, "pattern and mask lengths");
    // The native block the je lands on: willPresent, TickFrozenCheck, cq, the inlined FG
    // IsActive/IsPaused, mov rcx,r15; call ApplyToFinishedPicture.
    static const uint8_t native[] = {
        0x45, 0x84, 0xE4, 0x0F, 0x84, 0, 0, 0, 0,
        0xE8, 0, 0, 0, 0,
        0x48, 0x8B, 0x88, 0x98, 0x09, 0x00, 0x00, 0x48, 0x85, 0xC9, 0x74, 0x06, 0x48, 0x8B, 0x01, 0xFF, 0x50, 0x40,
        0x48, 0x8B, 0x95, 0x80, 0x00, 0x00, 0x00, 0x48, 0x85, 0xD2, 0x74, 0x45,
        0x48, 0x85, 0xFF, 0x74, 0x33,
        0x48, 0x8B, 0x47, 0x08, 0x48, 0x63, 0x48, 0x04,
        0x80, 0xBC, 0x39, 0x25, 0x02, 0x00, 0x00, 0x00, 0x75, 0x0A,
        0x80, 0xBC, 0x39, 0x18, 0x02, 0x00, 0x00, 0x00, 0x74, 0x17,
        0x48, 0x8B, 0x84, 0x39, 0x28, 0x02, 0x00, 0x00, 0x48, 0x85, 0xC0, 0x74, 0x17,
        0x48, 0x3B, 0x84, 0x39, 0x00, 0x02, 0x00, 0x00, 0x72, 0x0D,
        0x49, 0x8B, 0xCF, 0xE8, 0, 0, 0, 0};
    static const char native_mask[] =
        "xxxxx????"
        "x????"
        "xxxxxxxxxxxxxxxxxx"
        "xxxxxxxxxxxx"
        "xxxxx"
        "xxxxxxxx"
        "xxxxxxxxxx"
        "xxxxxxxxxx"
        "xxxxxxxxxxxxx"
        "xxxxxxxxxx"
        "xxxx????";
    static_assert(sizeof(native) == sizeof(native_mask) - 1, "pattern and mask lengths");

    uint8_t* site = nullptr;
    uint8_t* apply = nullptr;
    const int hits = find_unique(m, branch, branch_mask, sizeof(branch), [&](uint8_t* p) {
        uint8_t* target = rel32_target(p + 4);
        if (target < m.base || target + sizeof(native) > m.base + m.image) return false;
        if (!matches(target, native, native_mask, sizeof(native))) return false;
        apply = rel32_target(target + sizeof(native) - 4);
        return apply >= m.base && apply < m.base + m.image;
    }, &site);
    if (hits != 1) return hits;

    uint8_t* const patch = site + 8;          // the DXVK branch's first instruction
    constexpr size_t kCopied = 16;            // five position-independent instructions
    uint8_t* const back = patch + kCopied;    // test r9,r9

    // The stub: the native block's condition and call, then the five instructions it replaced.
    uint8_t stub[118];
    size_t n = 0;
    auto emit = [&](std::initializer_list<uint8_t> b) { for (uint8_t x : b) stub[n++] = x; };
    auto emit64 = [&](uint64_t v) { std::memcpy(stub + n, &v, 8); n += 8; };
    emit({0x45, 0x84, 0xE4, 0x74, 0x53});                               //  0 test r12b,r12b; jz skip
    emit({0x48, 0x8B, 0x95, 0x80, 0x00, 0x00, 0x00});                   //  5 mov rdx,[rbp+0x80]
    emit({0x48, 0x85, 0xD2, 0x74, 0x47});                               // 12 test rdx,rdx; jz skip
    emit({0x48, 0x85, 0xFF, 0x74, 0x33});                               // 17 test rdi,rdi; jz do
    emit({0x48, 0x8B, 0x47, 0x08, 0x48, 0x63, 0x48, 0x04});             // 22 fg's virtual-base offset
    emit({0x80, 0xBC, 0x39, 0x25, 0x02, 0x00, 0x00, 0x00, 0x75, 0x0A}); // 30 cmp; jne L1
    emit({0x80, 0xBC, 0x39, 0x18, 0x02, 0x00, 0x00, 0x00, 0x74, 0x17}); // 40 cmp; je do
    emit({0x48, 0x8B, 0x84, 0x39, 0x28, 0x02, 0x00, 0x00,               // 50 L1:
          0x48, 0x85, 0xC0, 0x74, 0x19});                               // 58 test rax,rax; jz skip
    emit({0x48, 0x3B, 0x84, 0x39, 0x00, 0x02, 0x00, 0x00, 0x72, 0x0F}); // 63 cmp; jb skip
    emit({0x49, 0x8B, 0xCF, 0x48, 0xB8});                               // 73 do: mov rcx,r15; mov rax,
    emit64(reinterpret_cast<uint64_t>(apply));                          //    ApplyToFinishedPicture
    emit({0xFF, 0xD0});                                                 // 86 call rax
    for (size_t k = 0; k < kCopied; ++k) stub[n++] = patch[k];          // 88 skip: the replaced code
    emit({0xFF, 0x25, 0x00, 0x00, 0x00, 0x00});                         // 104 jmp [rip+0]
    emit64(reinterpret_cast<uint64_t>(back));                           // 110
    if (n != sizeof(stub)) {
        log("[nr] OptiScaler fix (finished picture): stub is %zu bytes, expected %zu; nothing changed", n,
            sizeof(stub));
        return 1;
    }

    auto* code = static_cast<uint8_t*>(VirtualAlloc(nullptr, sizeof(stub), MEM_COMMIT | MEM_RESERVE,
                                                    PAGE_EXECUTE_READWRITE));
    if (!code) {
        log("[nr] OptiScaler fix (finished picture): VirtualAlloc failed (%lu)", GetLastError());
        return 1;
    }
    std::memcpy(code, stub, sizeof(stub));
    DWORD old = 0;
    VirtualProtect(code, sizeof(stub), PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), code, sizeof(stub));

    uint8_t jump[kCopied];
    jump[0] = 0xFF; jump[1] = 0x25; std::memset(jump + 2, 0, 4);        // jmp [rip+0]
    const uint64_t to = reinterpret_cast<uint64_t>(code);
    std::memcpy(jump + 6, &to, 8);
    jump[14] = jump[15] = 0x90;
    if (!write_code(patch, jump, sizeof(jump))) {
        log("[nr] OptiScaler fix (finished picture): VirtualProtect failed (%lu)", GetLastError());
        return 1;
    }
    log("[nr] OptiScaler fix: Finished Picture now runs on the DXVK present path (%ls+0x%llx, "
        "ApplyToFinishedPicture %ls+0x%llx)", m.name, static_cast<unsigned long long>(patch - m.base),
        m.name, static_cast<unsigned long long>(apply - m.base));
    return 1;
}

// v0.8.91 (f45ccf3a). The native block now also skips the call when XeFG composes the game's own
// picture (wrapped_swapchain.cpp, xeFgGamePicture):
//
//     if (cq && !xeFgGamePicture && (fg == nullptr || !fg->IsActive() || fg->IsPaused()))
//         DlssNr::ApplyToFinishedPicture(pSwapChain, cq);
//
// and the DXVK branch counts a presented frame itself, after its Present. Registers at the branch:
// r12 = the swapchain, r15d/r13d = SyncInterval/Flags, r14b = willPresent, rsi = State::currentFG,
// [rbp-0x50] = hWnd, [rbp+0x78] = cq. The stub is the native block's D3D12 condition and call,
// writing only rax, rcx and rdx, which the replaced instructions load again.
int finished_picture_0891(const Module& m) {
    // test bl,bl (usesDxvk) / je native / mov rax,[r12]; mov r9,[rbp-0x48]; mov r8d,r13d;
    // mov edx,r15d; mov rcx,r12; test r9,r9; jne +5; call [rax+0x40]
    static const Sig branch("84 DB 0F 84 ?? ?? ?? ?? 49 8B 04 24 4C 8B 4D B8 45 8B C5 41 8B D7 49 8B CC "
                            "4D 85 C9 75 05 FF 50 40");
    // The native block the je lands on: willPresent, TickFrozenCheck, xeFgGamePicture (State::Instance()
    // at +9, +26, +82, +97, +111), cq, the inlined FG IsActive/IsPaused, mov rcx,r12; call
    // ApplyToFinishedPicture.
    static const Sig native(
        "45 84 F6 0F 84 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B 98 98 09 00 00 48 85 DB 74 33 E8 ?? ?? ?? ?? "
        "48 8B 88 A0 09 00 00 4C 8B 03 48 85 C9 74 16 48 8B 41 08 48 63 50 04 8B 94 0A 1C 02 00 00 85 D2 "
        "0F 48 D7 EB 02 8B D7 48 8B CB 41 FF 50 40 48 85 F6 74 41 E8 ?? ?? ?? ?? 48 83 B8 C0 09 00 00 00 "
        "74 32 E8 ?? ?? ?? ?? 83 B8 A4 00 00 00 03 75 24 E8 ?? ?? ?? ?? 83 B8 10 06 00 00 00 75 16 48 8B "
        "06 48 8B CE FF 50 20 48 8B 7D B0 48 3B C7 75 08 B0 01 EB 06 48 8B 7D B0 32 C0 48 8B 55 78 48 85 "
        "D2 74 49 84 C0 75 45 48 85 F6 74 33 48 8B 46 08 48 63 48 04 80 BC 31 25 02 00 00 00 75 0A 80 BC "
        "31 18 02 00 00 00 74 17 48 8B 84 31 28 02 00 00 48 85 C0 74 17 48 3B 84 31 00 02 00 00 72 0D 49 "
        "8B CC E8 ?? ?? ?? ??");
    static const size_t kInstanceCalls[] = {9, 26, 82, 97, 111};

    uint8_t* site = nullptr;
    uint8_t* apply = nullptr;
    uint8_t* instance = nullptr;
    const int hits = find_unique(m, branch, [&](uint8_t* p) {
        uint8_t* target = rel32_target(p + 4);
        if (target < m.base || target + native.bytes.size() > m.base + m.image) return false;
        if (!matches(target, native.bytes.data(), native.mask.c_str(), native.bytes.size())) return false;
        uint8_t* const first = rel32_target(target + kInstanceCalls[0] + 1);
        for (size_t at : kInstanceCalls)
            if (rel32_target(target + at + 1) != first) return false;
        apply = rel32_target(target + native.bytes.size() - 4);
        instance = first;
        return apply >= m.base && apply < m.base + m.image && instance >= m.base && instance < m.base + m.image;
    }, &site);
    if (hits != 1) return hits;

    uint8_t* const patch = site + 8;          // the DXVK branch's first instruction
    constexpr size_t kCopied = 17;            // five position-independent instructions
    uint8_t* const back = patch + kCopied;    // test r9,r9

    // Assembled from the listing on the right; the two movabs immediates at 27 and 145.
    static const uint8_t kStub[] = {
        0x45, 0x84, 0xF6, 0x0F, 0x84, 0x92, 0x00, 0x00, 0x00,              //   0 test r14b,r14b; jz skip
        0x48, 0x83, 0x7D, 0x78, 0x00, 0x0F, 0x84, 0x87, 0x00, 0x00, 0x00,  //   9 cmp [rbp+0x78],0; jz skip
        0x48, 0x85, 0xF6, 0x74, 0x37,                                      //  20 test rsi,rsi; jz fgcheck
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xD0,                    //  25 call State::Instance
        0x48, 0x83, 0xB8, 0xC0, 0x09, 0x00, 0x00, 0x00, 0x74, 0x21,        //  37 currentFGSwapchain
        0x83, 0xB8, 0xA4, 0x00, 0x00, 0x00, 0x03, 0x75, 0x18,              //  47 activeFgOutput == XeFG
        0x83, 0xB8, 0x10, 0x06, 0x00, 0x00, 0x00, 0x75, 0x0F,              //  56 no swapchain interop
        0x48, 0x8B, 0x06, 0x48, 0x89, 0xF1, 0xFF, 0x50, 0x20,              //  65 fg->Hwnd()
        0x48, 0x3B, 0x45, 0xB0, 0x74, 0x4B,                                //  74 == hWnd: skip
        0x48, 0x85, 0xF6, 0x74, 0x33,                                      //  80 fgcheck: test rsi,rsi; jz do
        0x48, 0x8B, 0x46, 0x08, 0x48, 0x63, 0x48, 0x04,                    //  85 fg's virtual-base offset
        0x80, 0xBC, 0x31, 0x25, 0x02, 0x00, 0x00, 0x00, 0x75, 0x0A,        //  93 cmp; jne active
        0x80, 0xBC, 0x31, 0x18, 0x02, 0x00, 0x00, 0x00, 0x74, 0x17,        // 103 cmp; je do
        0x48, 0x8B, 0x84, 0x31, 0x28, 0x02, 0x00, 0x00,                    // 113 active:
        0x48, 0x85, 0xC0, 0x74, 0x1D,                                      // 121 test rax,rax; jz skip
        0x48, 0x3B, 0x84, 0x31, 0x00, 0x02, 0x00, 0x00, 0x72, 0x13,        // 126 cmp; jb skip
        0x48, 0x8B, 0x55, 0x78, 0x4C, 0x89, 0xE1,                          // 136 do: rdx = cq; rcx = r12
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xD0};                   // 143 call ApplyToFinishedPicture
    static_assert(sizeof(kStub) == 155, "fp0891.s is 155 bytes up to skip");
    uint8_t stub[sizeof(kStub) + kCopied + 14];
    std::memcpy(stub, kStub, sizeof(kStub));
    const uint64_t to_instance = reinterpret_cast<uint64_t>(instance), to_apply = reinterpret_cast<uint64_t>(apply),
                   to_back = reinterpret_cast<uint64_t>(back);
    std::memcpy(stub + 27, &to_instance, 8);
    std::memcpy(stub + 145, &to_apply, 8);
    std::memcpy(stub + sizeof(kStub), patch, kCopied);                  // skip: the replaced code
    uint8_t* const tail = stub + sizeof(kStub) + kCopied;
    tail[0] = 0xFF; tail[1] = 0x25; std::memset(tail + 2, 0, 4);        // jmp [rip+0]
    std::memcpy(tail + 6, &to_back, 8);

    uint8_t* const code = place_code(stub, sizeof(stub));
    if (!code) {
        log("[nr] OptiScaler fix (finished picture): VirtualAlloc failed (%lu)", GetLastError());
        return 1;
    }
    uint8_t jump[kCopied];
    jump[0] = 0xFF; jump[1] = 0x25; std::memset(jump + 2, 0, 4);        // jmp [rip+0]
    const uint64_t to = reinterpret_cast<uint64_t>(code);
    std::memcpy(jump + 6, &to, 8);
    std::memset(jump + 14, 0x90, kCopied - 14);
    if (!write_code(patch, jump, sizeof(jump))) {
        log("[nr] OptiScaler fix (finished picture): VirtualProtect failed (%lu)", GetLastError());
        return 1;
    }
    log("[nr] OptiScaler fix: Finished Picture now runs on the DXVK present path (%ls+0x%llx, "
        "ApplyToFinishedPicture %ls+0x%llx)", m.name, static_cast<unsigned long long>(patch - m.base),
        m.name, static_cast<unsigned long long>(apply - m.base));
    return 1;
}

void fix_finished_picture(const Module& m) {
    const int old_layout = finished_picture_084(m);
    if (old_layout == 1) return;
    const int new_layout = finished_picture_0891(m);
    if (new_layout == 1) return;
    log("[nr] OptiScaler fix (finished picture): %d/%d matching sites (v0.8.4/v0.8.91) in %ls, nothing changed",
        old_layout, new_layout, m.name);
}

// ---- 3. Window-sized swapchains are not overlays ------------------------------------------------
//
// DxgiFactoryHooks::CreateSwapChainForHwnd treats any swapchain under 100 pixels in either direction
// as an overlay's and does not wrap it. DXGI_SWAP_CHAIN_DESC1 allows 0 x 0 ("the size of the
// window"), and a game that asks for that (Helldivers 2) is left unwrapped: LocalPresent never runs,
// the frame counter never advances, and every NR feature waits forever for the frame after the one
// it was created in (DlssNr_Proxy.cpp: ready = submissionEpoch != creationEpoch) - features are
// created and nothing is ever evaluated. CreateSwapChain already resolves 0 x 0 to the window
// before the same test (ResolveWindowSizedSwapchain); ForHwnd does not (still so in v0.8.91).
//
// The two `jb overlay` of the test are redirected to stubs that send a 0 back to the normal path
// and anything from 1 to 99 on to the overlay block as before.
void* alloc_near(uint8_t* base, size_t image, size_t bytes) {
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const uintptr_t step = si.dwAllocationGranularity ? si.dwAllocationGranularity : 0x10000;
    const uintptr_t lo = reinterpret_cast<uintptr_t>(base), hi = lo + image;
    for (uintptr_t d = step; d < 0x70000000; d += step) {
        const uintptr_t up = ((hi + d) / step) * step;
        if (void* p = VirtualAlloc(reinterpret_cast<void*>(up), bytes, MEM_COMMIT | MEM_RESERVE,
                                   PAGE_EXECUTE_READWRITE)) return p;
        if (lo > d + step) {
            const uintptr_t down = ((lo - d) / step) * step;
            if (void* p = VirtualAlloc(reinterpret_cast<void*>(down), bytes, MEM_COMMIT | MEM_RESERVE,
                                       PAGE_EXECUTE_READWRITE)) return p;
        }
    }
    return nullptr;
}

void fix_window_sized_swapchain(const Module& m) {
    // test r14,r14 / je  (pDesc == nullptr), then the size test on pDesc (r14):
    //   lea r8,[r14+4]; cmp dword [r8],100; jb overlay; cmp dword [r14],100; jb overlay
    static const uint8_t pat[] = {
        0x4D, 0x85, 0xF6, 0x0F, 0x84, 0, 0, 0, 0,
        0x4D, 0x8D, 0x46, 0x04, 0x41, 0x83, 0x38, 0x64, 0x0F, 0x82, 0, 0, 0, 0,
        0x41, 0x83, 0x3E, 0x64, 0x0F, 0x82, 0, 0, 0, 0};
    static const char mask[] = "xxxxx????xxxxxxxxxx????xxxxxx????";
    static_assert(sizeof(pat) == sizeof(mask) - 1, "pattern and mask lengths");
    constexpr size_t kJbHeight = 17, kJbWidth = 27;   // the two 0F 82 rel32
    static const char kLog[] = "DxgiFactoryHooks::CreateSwapChainForHwnd Overlay call!";

    uint8_t* site = nullptr;
    uint8_t* overlay = nullptr;
    const int hits = find_unique(m, pat, mask, sizeof(pat), [&](uint8_t* p) {
        uint8_t* a = rel32_target(p + kJbHeight + 2);
        uint8_t* b = rel32_target(p + kJbWidth + 2);
        // Both lead to the same block, which starts by loading its log line.
        if (a != b || a < m.base || a + 7 > m.base + m.image || a[0] != 0x48 || a[1] != 0x8D || a[2] != 0x05)
            return false;
        const uint8_t* str = rel32_target(a + 3);
        if (str < m.base || str + sizeof(kLog) > m.base + m.image ||
            std::memcmp(str, kLog, sizeof(kLog) - 1) != 0) return false;
        overlay = a;
        return true;
    }, &site);
    if (hits != 1) {
        log("[nr] OptiScaler fix (window-sized swapchain): %d matching sites in %ls, nothing changed", hits,
            m.name);
        return;
    }
    uint8_t* const after_height = site + kJbHeight + 6;   // cmp dword [r14],100
    uint8_t* const after_width = site + kJbWidth + 6;     // the normal path

    // Two stubs, 34 bytes each:  cmp dword [reg],0 / jne +14 / jmp [rip] back / jmp [rip] overlay
    uint8_t stub[68];
    size_t n = 0;
    auto emit = [&](std::initializer_list<uint8_t> b) { for (uint8_t x : b) stub[n++] = x; };
    auto emit64 = [&](const void* v) { const uint64_t x = reinterpret_cast<uint64_t>(v); std::memcpy(stub + n, &x, 8); n += 8; };
    emit({0x41, 0x83, 0x38, 0x00, 0x75, 0x0E, 0xFF, 0x25, 0, 0, 0, 0}); emit64(after_height);   // Height
    emit({0xFF, 0x25, 0, 0, 0, 0}); emit64(overlay);
    emit({0x41, 0x83, 0x3E, 0x00, 0x75, 0x0E, 0xFF, 0x25, 0, 0, 0, 0}); emit64(after_width);    // Width
    emit({0xFF, 0x25, 0, 0, 0, 0}); emit64(overlay);
    if (n != sizeof(stub)) return;

    auto* code = static_cast<uint8_t*>(alloc_near(m.base, m.image, sizeof(stub)));
    auto rel = [](uint8_t* from_end, uint8_t* to, int32_t* out) {
        const int64_t d = to - from_end;
        if (d < INT32_MIN || d > INT32_MAX) return false;
        *out = static_cast<int32_t>(d);
        return true;
    };
    int32_t r_height = 0, r_width = 0;
    if (!code || !rel(site + kJbHeight + 6, code, &r_height) || !rel(site + kJbWidth + 6, code + 34, &r_width)) {
        if (code) VirtualFree(code, 0, MEM_RELEASE);
        log("[nr] OptiScaler fix (window-sized swapchain): no memory within reach of %ls, nothing changed", m.name);
        return;
    }
    std::memcpy(code, stub, sizeof(stub));
    DWORD old = 0;
    VirtualProtect(code, sizeof(stub), PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), code, sizeof(stub));
    if (!write_code(site + kJbHeight + 2, &r_height, 4) || !write_code(site + kJbWidth + 2, &r_width, 4)) {
        log("[nr] OptiScaler fix (window-sized swapchain): VirtualProtect failed (%lu)", GetLastError());
        return;
    }
    log("[nr] OptiScaler fix: a 0 x 0 (window-sized) swapchain is wrapped, not taken for an overlay (%ls+0x%llx)",
        m.name, static_cast<unsigned long long>(site - m.base));
}

// ---- 4. A depth/stencil depth is not cloned into a colour texture --------------------------------
//
// DlssNr_Dx12::State::TypedGuideFormat maps a typeless guide to a typed one, and ReadableGuide then
// creates a plain texture in that format (no depth-stencil flag) and CopyResource-s the game's guide
// into it. For a depth/stencil buffer the copy target is a shader-view format that is not a valid
// plain texture in D3D12: R24_UNORM_X8_TYPELESS for R24G8_TYPELESS (D24S8), R32_FLOAT_X8X24_TYPELESS
// for R32G8X24_TYPELESS (D32S8). Under vkd3d-proton that copy can hang the GPU: Helldivers 2 and
// Kingdom Come: Deliverance II (D24S8, "cloned a typeless guide as format 46") and S.T.A.L.K.E.R. 2
// (D32S8, "... as format 21", with our evaluate recording nothing) lost the device on NR's first
// frame. 007 First Light went through the same D32S8 copy without a hang.
//
// The fix returns both formats unchanged, so the depth is not treated as typeless and is handed on
// as it is: no clone, no copy. Our side reads the depth aspect of the game's own buffer
// (nr_pe_interop.cpp depth_stencil_format, runtime_depth.comp); the other guides are untouched.
void fix_depth_stencil_guide(const Module& m) {
    // TypedGuideFormat, v0.8.4: a jump table over (f - 9), eight `mov eax,<typed>; ret` cases and the
    // default `mov eax,edx; ret`. The R24G8_TYPELESS (44) case returns 46 (R24_UNORM_X8_TYPELESS).
    static const uint8_t pat[] = {
        0x8D, 0x42, 0xF7, 0x83, 0xF8, 0x2C, 0x77, 0x4F,
        0x4C, 0x8D, 0x05, 0, 0, 0, 0, 0x48, 0x98,
        0x41, 0x0F, 0xB6, 0x84, 0x00, 0, 0, 0, 0,
        0x41, 0x8B, 0x8C, 0x80, 0, 0, 0, 0, 0x49, 0x03, 0xC8, 0xFF, 0xE1,
        0xB8, 0x29, 0x00, 0x00, 0x00, 0xC3,    // R32_TYPELESS      -> R32_FLOAT
        0xB8, 0x38, 0x00, 0x00, 0x00, 0xC3,    // R16_TYPELESS      -> R16_UNORM
        0xB8, 0x2E, 0x00, 0x00, 0x00, 0xC3,    // R24G8_TYPELESS    -> R24_UNORM_X8_TYPELESS
        0xB8, 0x15, 0x00, 0x00, 0x00, 0xC3,    // R32G8X24_TYPELESS -> R32_FLOAT_X8X24_TYPELESS
        0xB8, 0x10, 0x00, 0x00, 0x00, 0xC3,
        0xB8, 0x22, 0x00, 0x00, 0x00, 0xC3,
        0xB8, 0x1C, 0x00, 0x00, 0x00, 0xC3,
        0xB8, 0x0A, 0x00, 0x00, 0x00, 0xC3,
        0x8B, 0xC2, 0xC3};                      // default: the format itself
    static const char mask[] =
        "xxxxxxxx" "xxx????xx" "xxxxx????" "xxxx????xxxxx"
        "xxxxxx" "xxxxxx" "xxxxxx" "xxxxxx" "xxxxxx" "xxxxxx" "xxxxxx" "xxxxxx" "xxx";
    static_assert(sizeof(pat) == sizeof(mask) - 1, "pattern and mask lengths");
    constexpr size_t kR24 = 39 + 12;            // the `mov eax,0x2e` of the R24G8 case
    constexpr size_t kR32 = 39 + 18;            // the `mov eax,0x15` of the R32G8X24 case

    uint8_t* hit = nullptr;
    const int hits = find_unique(m, pat, mask, sizeof(pat), [](uint8_t*) { return true; }, &hit);
    if (hits != 1) {
        log("[nr] OptiScaler fix (depth/stencil guide): %d matching sites in %ls, nothing changed", hits, m.name);
        return;
    }
    static const uint8_t fix[] = {0x8B, 0xC2, 0x90, 0x90, 0x90};   // mov eax,edx (the format) ; nops
    if (!write_code(hit + kR24, fix, sizeof(fix)) || !write_code(hit + kR32, fix, sizeof(fix))) {
        log("[nr] OptiScaler fix (depth/stencil guide): VirtualProtect failed (%lu)", GetLastError());
        return;
    }
    log("[nr] OptiScaler fix: a D24S8/D32S8 depth guide is passed on as it is, not copied into a colour "
        "texture (%ls+0x%llx)", m.name, static_cast<unsigned long long>(hit + kR24 - m.base));
}

// ---- 5. AutoExposure marks a float colour as linear HDR ----------------------------------------
//
// MakeDlssNrPass decides whether NR's input is linear HDR - and so whether OptiScaler encodes it
// (white point, soft knee, sRGB) and carries the edit back as a ratio - from the game's DLSS create
// flags alone: ColourIsLinearHdr = (flags & IsHDR) != 0, then cleared for non-float formats. Some
// games leave IsHDR clear yet hand over an unexposed scene-linear float buffer and ask the upscaler
// to expose it (AutoExposure): 007 First Light (R11G11B10) and Helldivers 2 (RGBA16, values up to
// ~560 in the readback) - the network, shown that raw, answered with coloured, flickering blocks.
// AutoExposure only has a meaning for HDR input (NVIDIA's DLSS documentation), so the test becomes
// (flags & (IsHDR | AutoExposure)) != 0; the float-format check after it is unchanged, so 8/10-bit
// frames are never affected. Kingdom Come: Deliverance II, which sets IsHDR, already took this path.
void fix_autoexposure_hdr(const Module& m) {
    // mov ecx,[rbp+0x170] (featureFlags); DepthInverted = flags>>3 & 1; MVLowRes = flags>>1 & 1;
    // and cl,1 ; mov [rbp+0x8d],cl  <- ColourIsLinearHdr ; lea rdx,[rip+"DLSS.Output"...]
    static const uint8_t pat[] = {
        0x8B, 0x8D, 0x70, 0x01, 0x00, 0x00, 0x8B, 0xC1, 0xC1, 0xE8, 0x03, 0x24, 0x01,
        0x88, 0x85, 0x81, 0x00, 0x00, 0x00, 0x8B, 0xC1, 0xD1, 0xE8, 0x24, 0x01,
        0x88, 0x85, 0xD4, 0x00, 0x00, 0x00,
        0x80, 0xE1, 0x01, 0x88, 0x8D, 0x8D, 0x00, 0x00, 0x00,
        0x48, 0x8D, 0x15};
    static const char mask[] = "xxxxxxxxxxxxx" "xxxxxxxxxxxx" "xxxxxx" "xxxxxxxxx" "xxx";
    static_assert(sizeof(pat) == sizeof(mask) - 1, "pattern and mask lengths");
    // v0.8.91: the same sequence, flags at [rbp+0x180], ColourIsLinearHdr at [rbp+0x6d]:
    // and cl,1 ; mov [rbp+0x6d],cl ; lea rdx,[rip+"DLSSD.Output"...]
    static const Sig pat_0891("8B 8D 80 01 00 00 8B C1 C1 E8 03 24 01 88 45 61 8B C1 D1 E8 24 01 "
                              "88 85 B4 00 00 00 80 E1 01 88 4D 6D 48 8D 15");
    size_t kAnd = 31, kLen = 9;                 // and cl,1 ; mov [rbp+0x8d],cl

    uint8_t* hit = nullptr;
    const int hits = find_unique(m, pat, mask, sizeof(pat), [](uint8_t*) { return true; }, &hit);
    if (hits != 1) {
        const int hits_0891 = find_unique(m, pat_0891, [](uint8_t*) { return true; }, &hit);
        if (hits_0891 != 1) {
            log("[nr] OptiScaler fix (AutoExposure HDR): %d/%d matching sites (v0.8.4/v0.8.91) in %ls, nothing "
                "changed", hits, hits_0891, m.name);
            return;
        }
        kAnd = 28; kLen = 6;                    // and cl,1 ; mov [rbp+0x6d],cl
    }
    uint8_t* const at = hit + kAnd;
    uint8_t* const back = at + kLen;
    // test cl,IsHDR|AutoExposure ; setne cl ; the site's own store of cl ; jmp back
    uint8_t stub[17] = {0xF6, 0xC1, 0x41, 0x0F, 0x95, 0xC1};
    const size_t store = kLen - 3, size = 6 + store + 5;
    std::memcpy(stub + 6, at + 3, store);
    stub[6 + store] = 0xE9;
    auto* code = static_cast<uint8_t*>(alloc_near(m.base, m.image, size));
    int32_t to_stub = 0, to_back = 0;
    auto rel = [](uint8_t* from_end, uint8_t* to, int32_t* out) {
        const int64_t d = to - from_end;
        if (d < INT32_MIN || d > INT32_MAX) return false;
        *out = static_cast<int32_t>(d);
        return true;
    };
    if (!code || !rel(at + 5, code, &to_stub) || !rel(code + size, back, &to_back)) {
        if (code) VirtualFree(code, 0, MEM_RELEASE);
        log("[nr] OptiScaler fix (AutoExposure HDR): no memory within reach of %ls, nothing changed", m.name);
        return;
    }
    std::memcpy(stub + size - 4, &to_back, 4);
    std::memcpy(code, stub, size);
    DWORD old = 0;
    VirtualProtect(code, size, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), code, size);
    uint8_t jump[9] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90};
    std::memcpy(jump + 1, &to_stub, 4);
    if (!write_code(at, jump, kLen)) {
        log("[nr] OptiScaler fix (AutoExposure HDR): VirtualProtect failed (%lu)", GetLastError());
        return;
    }
    log("[nr] OptiScaler fix: a float colour with AutoExposure is treated as linear HDR (%ls+0x%llx)", m.name,
        static_cast<unsigned long long>(at - m.base));
}

// ---- 6. A released feature outlives the GPU work recorded for it -------------------------------
//
// NVSDK_NGX_D3D12_ReleaseFeature erases the feature from Dx12Contexts at once, and with it the
// feature's shaders and their GpuTime_Dx12 timestamp heaps - whether or not the GPU has executed
// what that feature recorded. S.T.A.L.K.E.R. 2 destroys its first FSR context (ffxDestroyContext ->
// ReleaseFeature) right after presenting the first frame that used it: vkd3d-proton frees the query
// pools, the frame's own list then writes its timestamps into unmapped memory and waits on them
// (CmdCopyQueryPoolResults, WAIT_REG_MEM), and the GPU hangs. RADV_DEBUG=hang reported
// "Potential use-after-free" on a 6-timestamp pool freed in that ReleaseFeature, with the NR model
// recording nothing at all (NR_DEBUG_NOOP=1).
//
// The fix detours the exported EvaluateFeature and ReleaseFeature (the FSR/XeSS inputs call the
// same functions internally). After each evaluate a WriteBufferImmediate at the end of the game's
// list stores a serial in a readback buffer; a ReleaseFeature arriving before the GPU has written
// the feature's last serial is held, and carried out on a later NGX call once it has - or after
// kHoldMs, for a list that is never executed. Shutdown carries out everything still held first.
// The release only reads InHandle->Id, so a held release keeps its own copy of the handle.
namespace release_hold {

struct Handle { unsigned int id; };   // NVSDK_NGX_Handle
using EvaluateFn = int (*)(ID3D12GraphicsCommandList*, const Handle*, const void*, void*);
using ReleaseFn = int (*)(Handle*);
using ShutdownFn = int (*)();
using Shutdown1Fn = int (*)(ID3D12Device*);

constexpr uint32_t kSlots = 1024;          // one per evaluated feature, reused round robin
constexpr DWORD kHoldMs = 5000;

EvaluateFn real_evaluate;
ReleaseFn real_release;
ShutdownFn real_shutdown;
Shutdown1Fn real_shutdown1;

struct Mark { unsigned int id; uint32_t slot, serial; };
struct Held { Handle* handle; uint32_t slot, serial; DWORD since; };

std::mutex lock;
ID3D12Device* device;             // the device the marks live on (not owned)
ID3D12Resource* marks;
volatile uint32_t* written;       // the readback buffer, mapped for the process's life
D3D12_GPU_VIRTUAL_ADDRESS marks_va;
uint32_t serial, next_slot;
std::vector<Mark> live;           // the last mark of each feature that has been evaluated
std::vector<Held> held;
bool logged;

bool reached(uint32_t slot, uint32_t s) { return static_cast<int32_t>(written[slot] - s) >= 0; }

// Releases whose GPU work is done (or that waited long enough); `all` for shutdown.
std::vector<Handle*> take_ready(bool all) {
    std::vector<Handle*> out;
    const DWORD now = GetTickCount();
    for (size_t i = 0; i < held.size();) {
        const Held& h = held[i];
        if (all || reached(h.slot, h.serial) || now - h.since >= kHoldMs) {
            out.push_back(h.handle);
            held.erase(held.begin() + static_cast<long>(i));
        } else {
            ++i;
        }
    }
    return out;
}

void release_all(const std::vector<Handle*>& handles) {
    for (Handle* h : handles) {
        real_release(h);
        delete h;
    }
}

bool ensure_marks(ID3D12GraphicsCommandList* cmd) {
    ID3D12Device* dev = nullptr;
    if (FAILED(cmd->GetDevice(IID_PPV_ARGS(&dev))) || !dev) return false;
    dev->Release();                   // the list keeps its device alive
    if (dev == device) return marks != nullptr;
    // A new device: nothing recorded for the old one can be tracked any more.
    live.clear();
    if (marks) { marks->Release(); marks = nullptr; written = nullptr; }
    device = dev;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = kSlots * sizeof(uint32_t);
    desc.Height = 1; desc.DepthOrArraySize = 1; desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                            nullptr, IID_PPV_ARGS(&marks))))
        return false;
    void* p = nullptr;
    if (FAILED(marks->Map(0, nullptr, &p)) || !p) { marks->Release(); marks = nullptr; return false; }
    written = static_cast<volatile uint32_t*>(p);
    for (uint32_t i = 0; i < kSlots; ++i) written[i] = 0;
    marks_va = marks->GetGPUVirtualAddress();
    return true;
}

int evaluate(ID3D12GraphicsCommandList* cmd, const Handle* handle, const void* params, void* callback) {
    std::vector<Handle*> ready;
    {
        std::lock_guard<std::mutex> g(lock);
        ready = take_ready(false);
    }
    release_all(ready);
    const int result = real_evaluate(cmd, handle, params, callback);
    if (!cmd || !handle) return result;
    std::lock_guard<std::mutex> g(lock);
    ID3D12GraphicsCommandList2* list2 = nullptr;
    if (!ensure_marks(cmd) || FAILED(cmd->QueryInterface(IID_PPV_ARGS(&list2))) || !list2) return result;
    Mark* mark = nullptr;
    for (Mark& m : live)
        if (m.id == handle->id) mark = &m;
    if (!mark) {
        live.push_back({handle->id, next_slot++ % kSlots, 0});
        mark = &live.back();
    }
    mark->serial = ++serial;
    const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER p{marks_va + mark->slot * sizeof(uint32_t), mark->serial};
    const D3D12_WRITEBUFFERIMMEDIATE_MODE mode = D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_OUT;
    list2->WriteBufferImmediate(1, &p, &mode);
    list2->Release();
    return result;
}

int release(Handle* handle) {
    std::vector<Handle*> ready;
    {
        std::lock_guard<std::mutex> g(lock);
        ready = take_ready(false);
        if (handle) {
            for (size_t i = 0; i < live.size(); ++i) {
                if (live[i].id != handle->id) continue;
                const Mark m = live[i];
                live.erase(live.begin() + static_cast<long>(i));
                if (written && !reached(m.slot, m.serial)) {
                    held.push_back({new Handle{handle->id}, m.slot, m.serial, GetTickCount()});
                    if (!logged) {
                        logged = true;
                        log("[nr] OptiScaler fix: feature %u released before the GPU finished its work; "
                            "the release waits for it", handle->id);
                    }
                    handle = nullptr;
                }
                break;
            }
        }
    }
    release_all(ready);
    return handle ? real_release(handle) : 1;   // NVSDK_NGX_Result_Success
}

void flush_all() {
    std::vector<Handle*> all;
    {
        std::lock_guard<std::mutex> g(lock);
        all = take_ready(true);
        live.clear();
    }
    release_all(all);
}

int shutdown() { flush_all(); return real_shutdown(); }
int shutdown1(ID3D12Device* d) { flush_all(); return real_shutdown1(d); }

}  // namespace release_hold

void fix_release_hold(const Module& m) {
    auto* module = reinterpret_cast<HMODULE>(m.base);
    struct { const char* name; void* detour; void** original; } entries[] = {
        {"NVSDK_NGX_D3D12_EvaluateFeature", reinterpret_cast<void*>(&release_hold::evaluate),
         reinterpret_cast<void**>(&release_hold::real_evaluate)},
        {"NVSDK_NGX_D3D12_ReleaseFeature", reinterpret_cast<void*>(&release_hold::release),
         reinterpret_cast<void**>(&release_hold::real_release)},
        {"NVSDK_NGX_D3D12_Shutdown", reinterpret_cast<void*>(&release_hold::shutdown),
         reinterpret_cast<void**>(&release_hold::real_shutdown)},
        {"NVSDK_NGX_D3D12_Shutdown1", reinterpret_cast<void*>(&release_hold::shutdown1),
         reinterpret_cast<void**>(&release_hold::real_shutdown1)},
    };
    void* targets[4] = {};
    for (size_t i = 0; i < 4; ++i) {
        targets[i] = reinterpret_cast<void*>(GetProcAddress(module, entries[i].name));
        if (!targets[i]) {
            log("[nr] OptiScaler fix (held release): %ls exports no %s, nothing changed", m.name, entries[i].name);
            return;
        }
    }
    const MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
        log("[nr] OptiScaler fix (held release): MinHook would not start (%d), nothing changed", int(init));
        return;
    }
    for (size_t i = 0; i < 4; ++i) {
        if (MH_CreateHook(targets[i], entries[i].detour, entries[i].original) != MH_OK) {
            for (size_t k = 0; k < i; ++k) MH_RemoveHook(targets[k]);
            log("[nr] OptiScaler fix (held release): %s could not be hooked, nothing changed", entries[i].name);
            return;
        }
    }
    for (size_t i = 0; i < 4; ++i)
        if (MH_EnableHook(targets[i]) != MH_OK)
            log("[nr] OptiScaler fix (held release): %s could not be enabled", entries[i].name);
    log("[nr] OptiScaler fix: a released feature is kept until the GPU has run its work (%ls+0x%llx)", m.name,
        static_cast<unsigned long long>(static_cast<uint8_t*>(targets[1]) - m.base));
}

// ---- 7. Every float colour format counts as float ----------------------------------------------
//
// Right after the flag test fix 5 changes, MakeDlssNrPass clears ColourIsLinearHdr unless the output
// (or colour) is one of R32G32B32A32 typeless/float, R32G32B32 float, R16G16B16A16 typeless/float or
// R11G11B10 float (DlssNr_Pipeline_Dx12.cpp: a switch, compiled into a jump table over formats 1..26
// whose out-of-range side is the clearing store). R9G9B9E5_SHAREDEXP (67) is float too and is what
// RE Engine hands its upscaler: Resident Evil Requiem creates its FSR context with IsHdr set and a
// format-67 colour, and OptiScaler then logs "the game's DLSS colour space is already tone-mapped",
// sending unexposed scene-linear light to the network. R32G32B32_TYPELESS (5) is the only other
// float member left out. With both added the list is every DXGI float colour format there is.
//
// The range check in front of the table becomes a jump to a stub that sends 67 and 5 to the table's
// own "keep" target and does the original check for everything else; the table is untouched, so the
// answer for every other format is what it was.
void fix_float_formats(const Module& m) {
    // mov ecx,[rax+0x20] (Format) ; dec ecx ; cmp ecx,0x19 ; ja clear ; movsxd rax,ecx ;
    // lea rdx,[rip+image base] ; movzx eax,byte [rdx+rax+index] ; mov ecx,[rdx+rax*4+targets] ;
    // add rcx,rdx ; jmp rcx ; clear: mov byte [rbp+0x8d],0 (ColourIsLinearHdr) ; keep:
    static const uint8_t pat[] = {
        0x8B, 0x48, 0x20, 0xFF, 0xC9, 0x83, 0xF9, 0x19, 0x77, 0x1E,
        0x48, 0x63, 0xC1, 0x48, 0x8D, 0x15, 0, 0, 0, 0,
        0x0F, 0xB6, 0x84, 0x02, 0, 0, 0, 0,
        0x8B, 0x8C, 0x82, 0, 0, 0, 0,
        0x48, 0x03, 0xCA, 0xFF, 0xE1,
        0xC6, 0x85, 0x8D, 0x00, 0x00, 0x00, 0x00};
    static const char mask[] = "xxxxxxxxxx" "xxxxxx????" "xxxx????" "xxx????" "xxxxx" "xxxxxxx";
    static_assert(sizeof(pat) == sizeof(mask) - 1, "pattern and mask lengths");
    // v0.8.91: FormatCanHoldLinearHdr, the same table, now answering in al for
    // `ColourIsLinearHdr &= ...`: ... jmp rcx ; keep: mov al,1 ; jmp store ; clear: xor al,al ;
    // store: and [rbp+0x6d],al
    static const Sig pat_0891("8B 48 20 FF C9 83 F9 19 77 22 48 63 C1 48 8D 15 ?? ?? ?? ?? 0F B6 84 02 ?? ?? ?? ?? "
                              "8B 8C 82 ?? ?? ?? ?? 48 03 CA FF E1 B0 01 EB 02 32 C0 20 45 6D");
    constexpr size_t kCheck = 5, kLen = 5;      // cmp ecx,0x19 ; ja clear
    size_t kClear = 40, kKeep = sizeof(pat);

    uint8_t* hit = nullptr;
    const int hits = find_unique(m, pat, mask, sizeof(pat), [](uint8_t*) { return true; }, &hit);
    if (hits != 1) {
        const int hits_0891 = find_unique(m, pat_0891, [](uint8_t*) { return true; }, &hit);
        if (hits_0891 != 1) {
            log("[nr] OptiScaler fix (float formats): %d/%d matching sites (v0.8.4/v0.8.91) in %ls, nothing "
                "changed", hits, hits_0891, m.name);
            return;
        }
        kClear = 44; kKeep = 40;                // xor al,al / mov al,1
    }
    uint8_t* const at = hit + kCheck;
    uint8_t* const back = at + kLen;
    uint8_t* const clear = hit + kClear;
    uint8_t* const keep = hit + kKeep;
    uint8_t stub[32] = {0x83, 0xF9, 0x42,                        // cmp ecx,67-1 (R9G9B9E5_SHAREDEXP)
                        0x0F, 0x84, 0, 0, 0, 0,                  // je keep
                        0x83, 0xF9, 0x04,                        // cmp ecx,5-1 (R32G32B32_TYPELESS)
                        0x0F, 0x84, 0, 0, 0, 0,                  // je keep
                        0x83, 0xF9, 0x19,                        // cmp ecx,0x19 (the original check)
                        0x0F, 0x87, 0, 0, 0, 0,                  // ja clear
                        0xE9, 0, 0, 0, 0};                       // jmp back (the table)
    auto* code = static_cast<uint8_t*>(alloc_near(m.base, m.image, sizeof(stub)));
    auto rel = [](uint8_t* from_end, uint8_t* to, int32_t* out) {
        const int64_t d = to - from_end;
        if (d < INT32_MIN || d > INT32_MAX) return false;
        *out = static_cast<int32_t>(d);
        return true;
    };
    int32_t to_stub = 0, r[4] = {};
    if (!code || !rel(at + 5, code, &to_stub) || !rel(code + 9, keep, &r[0]) || !rel(code + 18, keep, &r[1]) ||
        !rel(code + 27, clear, &r[2]) || !rel(code + 32, back, &r[3])) {
        if (code) VirtualFree(code, 0, MEM_RELEASE);
        log("[nr] OptiScaler fix (float formats): no memory within reach of %ls, nothing changed", m.name);
        return;
    }
    std::memcpy(stub + 5, &r[0], 4);
    std::memcpy(stub + 14, &r[1], 4);
    std::memcpy(stub + 23, &r[2], 4);
    std::memcpy(stub + 28, &r[3], 4);
    std::memcpy(code, stub, sizeof(stub));
    DWORD old = 0;
    VirtualProtect(code, sizeof(stub), PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), code, sizeof(stub));
    uint8_t jump[kLen] = {0xE9, 0, 0, 0, 0};
    std::memcpy(jump + 1, &to_stub, 4);
    if (!write_code(at, jump, sizeof(jump))) {
        log("[nr] OptiScaler fix (float formats): VirtualProtect failed (%lu)", GetLastError());
        return;
    }
    log("[nr] OptiScaler fix: R9G9B9E5 and R32G32B32 typeless colour count as float for linear HDR (%ls+0x%llx)",
        m.name, static_cast<unsigned long long>(at - m.base));
}

// ---- 8. A size change re-creates the feature -------------------------------------------------
//
// The D3D12 path throws its NR feature away whenever the frame's size changes, in two places, and a new feature is not
// evaluated in the frame that creates it (it waits for the creation's submission). With the game's dynamic resolution
// and NR before the upscaler that is every size change: a size that moves every frame ran NR on ~3% of frames
// (858 features made, 9 evaluated). NVIDIA's DLL evaluates one feature at any
// size, and a frame at a new size is that of a new feature there (measured on an RTX 5090); this backend does the same
// per evaluation (nr_pe_session.cpp, Impl::resized). Each evaluation carries its own size (DLSSNR.Width/Height and
// the subrects are set every frame), so the feature is kept:
//
//  a. DlssNr::Proxy::Context::Prepare retires it when the settings, the device, the width or the height differ from
//     its creation's: cmp [r14+device],rdi/rsi ; jne retire ; cmp [r14+0x3c],ebx (width) ; jne retire ;
//     cmp [r14+0x40],ecx (height) ; je keep ; retire: ... The width jne becomes two nops and the height je a jmp.
//  b. DlssNr_Dx12::State::PrepareRunModels (v0.8.91) calls RetryAfterFailure on every pass's model when the
//     resolution, the placement or the tuning changed, then parks the frame-sized scratch textures on a resolution or
//     placement change. The models' loop goes through a stub that runs it only for a placement change, or a tuning
//     change with a feature made (the original condition without the resolution); the textures are still parked and
//     made again at the new size, and the frame still starts the history afresh (nr.reset).
// Settings, device, placement and tuning changes still re-create the feature.
void fix_feature_resize(const Module& m) {
    static const Sig pat_0891("41 38 46 38 75 15 49 39 BE 98 00 00 00 75 0C 41 39 5E 3C 75 06 41 39 4E 40 74 08 "
                              "49 8B CE E8");
    static const Sig pat_084("41 38 46 38 75 12 49 39 76 50 75 0C 41 39 5E 3C 75 06 41 39 4E 40 74 08 49 8B CE E8");
    uint8_t* hit = nullptr;
    size_t width_at = 19;                       // the jne after the width compare (v0.8.91)
    const int hits = find_unique(m, pat_0891, [](uint8_t*) { return true; }, &hit);
    bool v0891 = hits == 1;
    if (!v0891) {
        const int hits_084 = find_unique(m, pat_084, [](uint8_t*) { return true; }, &hit);
        if (hits_084 != 1) {
            log("[nr] OptiScaler fix (size change): %d/%d matching sites (v0.8.91/v0.8.4) in %ls, nothing changed",
                hits, hits_084, m.name);
            return;
        }
        width_at = 16;
    }
    // b first: without it a changes nothing (the whole model state is reset before Prepare sees it).
    uint8_t* loop = nullptr;
    if (v0891) {
        // mov ecx,eax (tuning matches) ; cmp byte [rsp+0x41],0 (resolution changed) ; jne models ;
        // test r13b,r13b (placement changed) ; jne models ; mov rax,[rsi+8] ; cmp qword [rax],0 ; je skip ;
        // test ecx,ecx ; jne skip ; models: lea r14,[rsi+8] ; lea r15,[r14+0xf0] ; cmp r14,r15 ; je done ;
        // mov rcx,[r14] ; call RetireState
        static const Sig loop_0891("8B C8 80 7C 24 41 00 75 1B 45 84 ED 75 16 48 8B 46 08 48 83 38 00 0F 84 ?? ?? ?? ?? "
                                   "85 C9 0F 85 ?? ?? ?? ?? 4C 8D 76 08 4D 8D BE F0 00 00 00 4D 3B F7 74 11 49 8B 0E E8");
        const int n = find_unique(m, loop_0891, [](uint8_t*) { return true; }, &loop);
        if (n != 1) {
            log("[nr] OptiScaler fix (size change): %d matching sites for the models' reset in %ls, nothing changed",
                n, m.name);
            return;
        }
    } else {
        log("[nr] OptiScaler fix (size change): OptiScaler v0.8.4 resets its models on every size change elsewhere; "
            "nothing changed");
        return;
    }
    constexpr size_t kModels = 36, kAfterLeas = 47, kDone = 69;
    uint8_t* const at = loop + kModels;
    // test r13b,r13b ; jnz retire ; mov rax,[rsi+8] ; cmp qword [rax],0 ; je skip ; test ecx,ecx ; je retire ;
    // skip: jmp done ; retire: lea r14,[rsi+8] ; lea r15,[r14+0xf0] ; jmp after the leas
    uint8_t stub[40] = {0x45, 0x84, 0xED, 0x75, 0x13, 0x48, 0x8B, 0x46, 0x08, 0x48, 0x83, 0x38, 0x00, 0x74, 0x04,
                        0x85, 0xC9, 0x74, 0x05, 0xE9, 0, 0, 0, 0, 0x4C, 0x8D, 0x76, 0x08,
                        0x4D, 0x8D, 0xBE, 0xF0, 0x00, 0x00, 0x00, 0xE9, 0, 0, 0, 0};
    auto* code = static_cast<uint8_t*>(alloc_near(m.base, m.image, sizeof(stub)));
    auto rel = [](uint8_t* from_end, uint8_t* to, int32_t* out) {
        const int64_t d = to - from_end;
        if (d < INT32_MIN || d > INT32_MAX) return false;
        *out = static_cast<int32_t>(d);
        return true;
    };
    int32_t to_stub = 0, to_done = 0, to_back = 0;
    if (!code || !rel(at + 5, code, &to_stub) || !rel(code + 24, loop + kDone, &to_done) ||
        !rel(code + 40, loop + kAfterLeas, &to_back)) {
        if (code) VirtualFree(code, 0, MEM_RELEASE);
        log("[nr] OptiScaler fix (size change): no memory within reach of %ls, nothing changed", m.name);
        return;
    }
    std::memcpy(stub + 20, &to_done, 4);
    std::memcpy(stub + 36, &to_back, 4);
    std::memcpy(code, stub, sizeof(stub));
    DWORD old = 0;
    VirtualProtect(code, sizeof(stub), PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), code, sizeof(stub));
    uint8_t jump[11] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
    std::memcpy(jump + 1, &to_stub, 4);
    static const uint8_t nops[] = {0x90, 0x90}, jmp[] = {0xEB};
    if (!write_code(at, jump, sizeof(jump)) || !write_code(hit + width_at, nops, sizeof(nops)) ||
        !write_code(hit + width_at + 6, jmp, sizeof(jmp))) {
        log("[nr] OptiScaler fix (size change): VirtualProtect failed (%lu)", GetLastError());
        return;
    }
    log("[nr] OptiScaler fix: the NR feature is kept across frame sizes (%ls+0x%llx, +0x%llx)", m.name,
        static_cast<unsigned long long>(hit + width_at - m.base), static_cast<unsigned long long>(at - m.base));
}

// ---- The RenoDX-style option (nr_pe_optifix.hpp, renodx_route) --------------------------------
namespace renodx_impl {

// DlssNrConstants (DlssNr_Common.h, 256 bytes, layout fixed by its static_asserts and the shader cbuffer).
constexpr size_t kConstants = 256, kMode = 0, kWhitePoint = 4, kWidth = 8, kHeight = 12, kPassthrough = 32, kReversibleMode = 76,
                 kSkinProtection = 92;
constexpr uint32_t kEncode = 0, kResolve = 1, kDownsample = 2, kClampProxy = 8, kNeutwoReplace = 2;

// The encode, resolve and clamp as this route wants them (nr_pe_renodx_route.hpp), into `copy`.
// The encode's own decision, for the engine-data panel (renodx_route::encode_seen): read only.
void see_encode(const uint8_t* constants) {
    uint32_t mode = 0, passthrough = 0;
    float white = 0.f;
    std::memcpy(&mode, constants + kMode, 4);
    if (mode != kEncode) return;
    std::memcpy(&passthrough, constants + kPassthrough, 4);
    std::memcpy(&white, constants + kWhitePoint, 4);
    renodx_route::note_encode_seen(passthrough == 0, white);
}

void rewrite(const uint8_t* constants, uint32_t mode, uint8_t* copy) {
    std::memcpy(copy, constants, kConstants);
    see_encode(constants);
    if (mode == kClampProxy) {
        // Between two model passes: the same-size Downsample is a plain copy (dlssnr.hlsl "Nothing to do when
        // the sizes already agree"), where ClampProxy would cut a linear frame at 1.
        std::memcpy(copy + kMode, &kDownsample, 4);
        return;
    }
    if (mode == kEncode) {
        uint32_t passthrough = 0, width = 0, height = 0;
        std::memcpy(&passthrough, constants + kPassthrough, 4);
        std::memcpy(&width, constants + kWidth, 4);
        std::memcpy(&height, constants + kHeight, 4);
        renodx_route::note_encode(passthrough == 0, width, height);
    } else {
        const uint32_t off = 0;
        std::memcpy(copy + kSkinProtection, &off, 4);
    }
    const uint32_t one = 1;
    std::memcpy(copy + kPassthrough, &one, 4);
    std::memcpy(copy + kReversibleMode, &kNeutwoReplace, 4);
}

// bool DlssNr_Dx12::DispatchPass(cmd, const DlssNrConstants&, source, model, original, motion, previous edit,
// target, keep, uint32_t* immutable slot)
using DispatchPass = bool (*)(void*, void*, const uint8_t*, void*, void*, void*, void*, void*, void*, void*, uint32_t*);
DispatchPass real_dispatch_pass = nullptr;

// The encode skipped when the model can read and write the frame itself (nr_pe_renodx_route.hpp set_direct): inside
// State::Run, a frame OptiScaler holds in UNORDERED_ACCESS between its encode and resolve (one it can write), readable
// by shaders and at the encode's extent; and only with every one of OptiScaler's NR settings locked (find_locks: its
// debug views, comparison, frame hold, passes and model resolution would read the copies). NR_RENODX_DIRECT=0 keeps
// OptiScaler's two copies.
std::atomic<bool> g_all_locked{false};
bool direct_frame(const uint8_t* constants, void* source) {
    static const bool off = [] { const char* e = std::getenv("NR_RENODX_DIRECT"); return e && e[0] == '0'; }();
    if (off || !g_all_locked.load() || !source || !renodx_route::current_frame().known) return false;
    uint32_t width = 0, height = 0;
    std::memcpy(&width, constants + kWidth, 4);
    std::memcpy(&height, constants + kHeight, 4);
    const D3D12_RESOURCE_DESC d = static_cast<ID3D12Resource*>(source)->GetDesc();
    return d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.Width == width && d.Height == height &&
           d.DepthOrArraySize == 1 && d.SampleDesc.Count == 1 && (d.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) &&
           !(d.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE);
}

bool dispatch_pass(void* self, void* cmd, const uint8_t* constants, void* source, void* model, void* original,
                   void* motion, void* previous, void* target, void* keep, uint32_t* slot) {
    uint32_t mode = 0;
    std::memcpy(&mode, constants + kMode, 4);
    if (mode != kEncode && mode != kResolve && mode != kClampProxy)
        return real_dispatch_pass(self, cmd, constants, source, model, original, motion, previous, target, keep, slot);
    // The frame is the model's to read and write: no encode copy, and no resolve to put the answer back (the frame
    // already holds it, or is untouched where NR did not run).
    if (mode == kResolve && renodx_route::direct()) return true;
    if (mode == kEncode && direct_frame(constants, source)) {
        alignas(256) uint8_t noted[kConstants];
        rewrite(constants, mode, noted);   // OptiScaler's judgement and extent noted as for a real encode
        renodx_route::set_direct(source);
        static std::atomic<bool> said{false};
        if (!said.exchange(true))
            log("[nr] RenoDX-style option: OptiScaler's encode and resolve skipped, NR reads and writes the frame itself");
        return true;
    }
    // The pass copies the constants into its own buffer before it returns, so a copy on this stack will do.
    alignas(256) uint8_t copy[kConstants];
    rewrite(constants, mode, copy);
    return real_dispatch_pass(self, cmd, copy, source, model, original, motion, previous, target, keep, slot);
}

// void DlssNr_Dx12::State::Run(cmd, colour, depth, motion, output, const DlssNrFrameInfo& frame, queue)
using StateRun = void (*)(void*, void*, void*, void*, void*, void*, const uint8_t*, void*);
StateRun real_state_run = nullptr;
// DlssNrFrameInfo (DlssNr_Common.h), offsets as v0.8.91 reads them (State::Run 0x1d/0x1e, EncodeInput 0x3c/0x48).
constexpr size_t kFrameBeforeUpscale = 0x1e, kFrameFinished = 0x20, kFrameRayReconstruction = 0x2d,
                 kFramePreExposure = 0x3c, kFrameExposureTexture = 0x40, kFrameExposureState = 0x48;
// The game's last NR frame ran with Ray Reconstruction (RenoDX keeps NR after the upscaler then: RR's input is noisy).
std::atomic<bool> g_ray_reconstruction{false};

// NR_OPTI_PROF=1 (measurement only): GPU time of each State::Run - OptiScaler's own NR steps with the model call
// inside - from D3D12 timestamps on its list, read back 8 calls later, averaged in the log every 300 calls.
class OptiProf {
public:
    static OptiProf& get() { static OptiProf p; return p; }
    static bool wanted() {
        static const bool on = [] { const char* e = std::getenv("NR_OPTI_PROF"); return e && e[0] == '1'; }();
        return on;
    }
    void begin(void* cmd, void* queue) {
        list_ = static_cast<ID3D12GraphicsCommandList*>(cmd);
        if (!list_ || !ready(queue)) { list_ = nullptr; return; }
        const uint32_t s = slot_ % kRing;
        if (slot_ >= kRing) take(s);
        list_->EndQuery(heap_, D3D12_QUERY_TYPE_TIMESTAMP, 2 * s);
    }
    void end() {
        if (!list_) return;
        const uint32_t s = slot_ % kRing;
        list_->EndQuery(heap_, D3D12_QUERY_TYPE_TIMESTAMP, 2 * s + 1);
        list_->ResolveQueryData(heap_, D3D12_QUERY_TYPE_TIMESTAMP, 2 * s, 2, readback_, 16ull * s);
        ++slot_;
        list_ = nullptr;
    }

private:
    static constexpr uint32_t kRing = 16;
    bool ready(void* queue) {
        if (failed_) return false;
        if (heap_) return true;
        failed_ = true;
        if (!queue) return false;
        freq_ = 0;
        if (FAILED(static_cast<ID3D12CommandQueue*>(queue)->GetTimestampFrequency(&freq_)) || !freq_) return false;
        ID3D12Device* device = nullptr;
        if (FAILED(list_->GetDevice(IID_PPV_ARGS(&device))) || !device) return false;
        D3D12_QUERY_HEAP_DESC q{};
        q.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        q.Count = 2 * kRing;
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = 16ull * kRing;
        rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        const bool ok = SUCCEEDED(device->CreateQueryHeap(&q, IID_PPV_ARGS(&heap_))) &&
                        SUCCEEDED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                                  D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                                  IID_PPV_ARGS(&readback_))) &&
                        SUCCEEDED(readback_->Map(0, nullptr, reinterpret_cast<void**>(&mapped_)));
        device->Release();
        if (!ok) { log("[nr] NR_OPTI_PROF: the timestamp queries could not be created"); return false; }
        std::memset(mapped_, 0, 16ull * kRing);
        failed_ = false;
        log("[nr] NR_OPTI_PROF: timing OptiScaler's DlssNr_Dx12::State::Run (%llu ticks/s)",
            static_cast<unsigned long long>(freq_));
        return true;
    }
    // The pair this slot held kRing calls ago; zeroed after reading so a pair the GPU never wrote is not counted.
    void take(uint32_t s) {
        const uint64_t a = mapped_[2 * s], b = mapped_[2 * s + 1];
        mapped_[2 * s] = mapped_[2 * s + 1] = 0;
        if (!a || b <= a) return;
        sum_ms_ += 1000.0 * static_cast<double>(b - a) / static_cast<double>(freq_);
        if (++count_ == 300) {
            log("[nr] NR_OPTI_PROF: State::Run %.3f ms GPU a call (average of %u)", sum_ms_ / count_, count_);
            sum_ms_ = 0;
            count_ = 0;
        }
    }
    ID3D12GraphicsCommandList* list_ = nullptr;
    ID3D12QueryHeap* heap_ = nullptr;
    ID3D12Resource* readback_ = nullptr;
    uint64_t* mapped_ = nullptr;
    uint64_t freq_ = 0, slot_ = 0;
    uint32_t count_ = 0;
    double sum_ms_ = 0;
    bool failed_ = false;
};

void state_run(void* self, void* cmd, void* colour, void* depth, void* motion, void* output, const uint8_t* frame,
               void* queue) {
    renodx_route::Frame f;
    f.before_upscale = frame[kFrameBeforeUpscale] != 0;
    f.finished_picture = frame[kFrameFinished] != 0;
    g_ray_reconstruction.store(frame[kFrameRayReconstruction] != 0);
    std::memcpy(&f.pre_exposure, frame + kFramePreExposure, 4);
    std::memcpy(&f.exposure, frame + kFrameExposureTexture, sizeof(void*));
    std::memcpy(&f.exposure_state, frame + kFrameExposureState, 4);
    renodx_route::begin_run(f);
    if (OptiProf::wanted()) OptiProf::get().begin(cmd, queue);
    real_state_run(self, cmd, colour, depth, motion, output, frame, queue);
    if (OptiProf::wanted()) OptiProf::get().end();
    renodx_route::end_run();
}

// ShaderPass_Dx12 MakeDlssNrPass(shader, device, list, NVSDK_NGX_Parameter* game, bool before, unsigned flags,
// queue, bool interop, bool ray reconstruction, uint64_t epoch): returned through a hidden first pointer.
using MakePass = void* (*)(void*, void*, void*, void*, void*, uint64_t, uint64_t, void*, uint64_t, uint64_t, uint64_t);
MakePass real_make_pass = nullptr;

void* make_pass(void* result, void* shader, void* device, void* list, void* game, uint64_t before, uint64_t flags,
                void* queue, uint64_t interop, uint64_t ray, uint64_t epoch) {
    // DLSS.Exposure.Scale from the game's own parameter block: Get(float*) at its vtable slot 14 (nr_ngx_slots.hpp,
    // MSVC's reversed overloads), as RenoDX reads it beside DLSS.Pre.Exposure.
    // And the sizes OptiScaler's menu shows: the render extent and the output, Get(unsigned int*) at slot 12.
    float scale = 1.0f;
    uint32_t size[6] = {};   // subrect width, height, Width, Height, OutWidth, OutHeight
    if (game) {
        using GetFloat = int (*)(void*, const char*, float*);
        auto get = reinterpret_cast<GetFloat>((*reinterpret_cast<void***>(game))[14]);
        float v = 0.0f;
        if (get && get(game, "DLSS.Exposure.Scale", &v) == 1 && v > 0.0f) scale = v;
        using GetUInt = int (*)(void*, const char*, unsigned int*);
        auto get_uint = reinterpret_cast<GetUInt>((*reinterpret_cast<void***>(game))[12]);
        const char* names[6] = {"DLSS.Render.Subrect.Dimensions.Width", "DLSS.Render.Subrect.Dimensions.Height",
                                "Width", "Height", "OutWidth", "OutHeight"};
        for (int i = 0; i < 6 && get_uint; ++i) {
            unsigned int u = 0;
            if (get_uint(game, names[i], &u) == 1) size[i] = u;
        }
    }
    const bool subrect = size[0] && size[1];
    renodx_route::note_pass(uint32_t(flags), scale, subrect ? size[0] : size[2], subrect ? size[1] : size[3], size[4],
                            size[5]);
    return real_make_pass(result, shader, device, list, game, before, flags, queue, interop, ray, epoch);
}

// OptiScaler's Vulkan path. bool DlssNr_Vk::Dispatch(cmd, const DlssNrConstants&, threads x, y, source, model, original,
// motion, target, keep, source layout, motion layout, bool finished colour, uint32_t* slot): the same rewrites as
// DispatchPass for its encode, resolve and clamp, never for the finished-colour (residual) program.
using VkDispatch = bool (*)(void*, void*, const uint8_t*, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                            uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint32_t*);
VkDispatch real_vk_dispatch = nullptr;

bool vk_dispatch(void* self, void* cmd, const uint8_t* constants, uint64_t tx, uint64_t ty, uint64_t source,
                 uint64_t model, uint64_t original, uint64_t motion, uint64_t target, uint64_t keep, uint64_t source_layout,
                 uint64_t motion_layout, uint64_t finished, uint32_t* slot) {
    uint32_t mode = 0;
    std::memcpy(&mode, constants + kMode, 4);
    if ((finished & 0xFF) != 0 || (mode != kEncode && mode != kResolve && mode != kClampProxy))
        return real_vk_dispatch(self, cmd, constants, tx, ty, source, model, original, motion, target, keep,
                                source_layout, motion_layout, finished, slot);
    alignas(256) uint8_t copy[kConstants];
    rewrite(constants, mode, copy);
    return real_vk_dispatch(self, cmd, copy, tx, ty, source, model, original, motion, target, keep, source_layout,
                            motion_layout, finished, slot);
}

// bool DlssNr::ModelVk::Impl::Evaluate(cmd, colour, depth, motion, target, const DlssNrFrameInfo_Vk& frame, instance,
// physical, device, input layout): the frame for the model's evaluate inside it. DlssNrFrameInfo_Vk is
// DlssNrFrameInfo followed by the exposure's VkImageInfo (v0.8.91: ImageView 0x70, Image 0x78, Format 0x94,
// Width/Height 0x98/0x9c - the `== 1` tests in this function).
using VkEvaluate = bool (*)(void*, void*, void*, void*, void*, void*, const uint8_t*, void*, void*, void*, uint64_t);
VkEvaluate real_vk_evaluate = nullptr;
constexpr size_t kVkExposureView = 0x70, kVkExposureImage = 0x78, kVkExposureFormat = 0x94, kVkExposureWidth = 0x98,
                 kVkExposureHeight = 0x9c;

bool vk_evaluate(void* self, void* cmd, void* colour, void* depth, void* motion, void* target, const uint8_t* frame,
                 void* instance, void* physical, void* device, uint64_t layout) {
    renodx_route::Frame f;
    f.before_upscale = frame[kFrameBeforeUpscale] != 0;
    f.finished_picture = frame[kFrameFinished] != 0;
    g_ray_reconstruction.store(frame[kFrameRayReconstruction] != 0);
    std::memcpy(&f.pre_exposure, frame + kFramePreExposure, 4);
    uint64_t view = 0;
    std::memcpy(&view, frame + kVkExposureView, 8);
    if (view) {
        std::memcpy(&f.vk_exposure, frame + kVkExposureImage, 8);
        std::memcpy(&f.vk_exposure_format, frame + kVkExposureFormat, 4);
        std::memcpy(&f.vk_exposure_width, frame + kVkExposureWidth, 4);
        std::memcpy(&f.vk_exposure_height, frame + kVkExposureHeight, 4);
    }
    renodx_route::begin_run(f);
    const bool ran = real_vk_evaluate(self, cmd, colour, depth, motion, target, frame, instance, physical, device, layout);
    renodx_route::end_run();
    return ran;
}

// The function a code address belongs to: its .pdata entry, following chained unwind info to the primary one.
uint8_t* function_start(uint8_t* at) {
    DWORD64 image = 0;
    PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(at), &image, nullptr);
    for (int guard = 0; f && guard < 16; ++guard) {
        const uint8_t* unwind = reinterpret_cast<const uint8_t*>(image + f->UnwindData);
        if (!((unwind[0] >> 3) & 0x4)) return reinterpret_cast<uint8_t*>(image + f->BeginAddress);   // UNW_FLAG_CHAININFO
        const unsigned codes = unwind[2];
        f = reinterpret_cast<PRUNTIME_FUNCTION>(const_cast<uint8_t*>(unwind) + 4 + ((codes + 1u) & ~1u) * 2);
    }
    return nullptr;
}

// ---- OptiScaler's own NR settings, locked while the option runs (its menu then only drives the upscaler and frame
// generation). OptiScaler keeps them in Config (Config.h), each a CustomOptional<T>: std::optional<T>, the default,
// the ini's value and a volatile flag. A lock is what its own set_volatile_value does - the value in use is ours,
// the one its Save writes stays the player's (a menu change sets it again, and the next lock takes it as the ini's).
// Found by Config::Save's code for each key:  call Config::Instance; lea rcx,[rax+field] ... lea r8,[rip+key];
// lea rdx,[rip+"DlssNr"].
struct Lock { const char* key; char kind; int value; uint32_t field; bool found; };   // kind: b bool, u/i 4 bytes, f float
Lock g_locks[] = {
    {"Enabled", 'b', 1}, {"RunBeforeSR", 'b', 0}, {"FinishedPicture", 'b', 0}, {"HdrTransfer", 'b', 0},
    {"ResidualAcrossRR", 'b', 0}, {"ApplyModel", 'b', 1}, {"HoldFrame", 'b', 0}, {"ToggleKey", 'i', -1},
    {"Passes", 'u', 1}, {"DebugView", 'u', 0}, {"Compare", 'u', 0}, {"CompareTags", 'b', 0}, {"WorkingScale", 'f', 1},
    {"SpatialCompression", 'b', 0}, {"SpatialShowCenter", 'b', 0}, {"SpatialShowWork", 'b', 0},
    {"ShowSkinMask", 'b', 0}, {"SkinProtection", 'b', 0}, {"DeferredDLSS", 'b', 0},
};
uint8_t* g_config = nullptr;

// set_volatile_value(value) on the CustomOptional<T> at `f` (MSVC x64: bool fields 6 bytes - value, engaged,
// default, ini value, ini engaged, volatile; 4-byte ones 24 - value, engaged at 4, default 8, ini 12 / 16, volatile 20).
template <class T>
void lock_field(uint8_t* f, T value) {
    constexpr size_t kHas = sizeof(T) == 1 ? 1 : 4, kIni = sizeof(T) == 1 ? 3 : 12, kVolatile = sizeof(T) == 1 ? 5 : 20;
    if (!f[kVolatile]) {   // the value in use now (the player's or the file's) becomes what its Save writes
        std::memcpy(f + kIni, f, sizeof(T));
        f[kIni + kHas] = f[kHas];
        f[kVolatile] = 1;
    }
    T now;
    std::memcpy(&now, f, sizeof(T));
    if (!f[kHas] || std::memcmp(&now, &value, sizeof(T)) != 0) {
        std::memcpy(f, &value, sizeof(T));
        f[kHas] = 1;
    }
}

void lock_all(uint32_t hook_point) {
    // RenoDX's NR screenshot of the final picture: NR moves to OptiScaler's finished picture until both copies are
    // recorded, as RenoDX moves it to Present for the capture (OptiScaler then runs no NR inline).
    const bool screenshot = screenshot::wants_final();
    for (Lock& l : g_locks) {
        if (!l.found) continue;
        uint8_t* f = g_config + l.field;
        int v = l.value;
        if (std::strcmp(l.key, "RunBeforeSR") == 0) v = hook_point == 1 && !g_ray_reconstruction.load() && !screenshot ? 1 : 0;
        if (std::strcmp(l.key, "FinishedPicture") == 0) v = screenshot ? 1 : 0;
        switch (l.kind) {
            case 'b': lock_field<uint8_t>(f, uint8_t(v)); break;
            case 'u': lock_field<uint32_t>(f, uint32_t(v)); break;
            case 'i': lock_field<int32_t>(f, int32_t(v)); break;
            case 'f': lock_field<float>(f, float(v)); break;
        }
    }
}

void find_locks(const Module& m, IMAGE_NT_HEADERS* nt) {
    auto data_strings = [&](const char* s) {   // every NUL-terminated copy of `s` outside code
        std::vector<uint8_t*> out;
        const size_t n = std::strlen(s) + 1;
        IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
            if (sec->Characteristics & IMAGE_SCN_MEM_EXECUTE) continue;
            uint8_t* p = m.base + sec->VirtualAddress;
            for (size_t k = 1; k + n <= sec->Misc.VirtualSize; ++k)
                if (p[k - 1] == 0 && std::memcmp(p + k, s, n) == 0) out.push_back(p + k);
        }
        return out;
    };
    const auto section = data_strings("DlssNr");
    auto in = [](const std::vector<uint8_t*>& v, uint8_t* p) { return std::find(v.begin(), v.end(), p) != v.end(); };
    // One pass over the code: every  lea r8,[rip+key]; lea rdx,[rip+"DlssNr"]  (key string -> where, how many).
    std::vector<std::pair<uint8_t*, uint8_t*>> pairs;
    {
        IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
            if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
            uint8_t* p = m.base + sec->VirtualAddress;
            for (size_t k = 0; k + 14 <= sec->Misc.VirtualSize; ++k)
                if (p[k] == 0x4C && p[k + 1] == 0x8D && p[k + 2] == 0x05 && p[k + 7] == 0x48 && p[k + 8] == 0x8D &&
                    p[k + 9] == 0x15 && in(section, rel32_target(p + k + 10)))
                    pairs.push_back({rel32_target(p + k + 3), p + k});
        }
    }
    uint8_t* instance = nullptr;
    int found = 0;
    for (Lock& l : g_locks) {
        const auto key = data_strings(l.key);
        uint8_t* at = nullptr;
        int hits = 0;
        for (const auto& [target, where] : pairs)
            if (in(key, target)) { at = where; ++hits; }
        if (hits != 1) continue;
        for (uint8_t* b = at - 12; b > at - 0x80; --b)
            if (b[0] == 0xE8 && b[5] == 0x48 && b[6] == 0x8D && b[7] == 0x88) {
                uint8_t* fn = rel32_target(b + 1);
                if (instance && fn != instance) break;
                instance = fn;
                int32_t field = 0;
                std::memcpy(&field, b + 8, 4);
                l.field = uint32_t(field);
                l.found = true;
                ++found;
                break;
            }
    }
    if (!instance) {
        log("[nr] RenoDX-style option: OptiScaler's NR settings not found in %ls; its menu's NR page keeps applying",
            m.name);
        return;
    }
    g_config = reinterpret_cast<uint8_t*>(reinterpret_cast<void* (*)()>(instance)());
    std::string missing;
    for (const Lock& l : g_locks)
        if (!l.found) missing += std::string(missing.empty() ? "" : ", ") + l.key;
    log("[nr] RenoDX-style option: OptiScaler's NR settings locked while it runs (%d of %zu: Enabled on, Passes 1, "
        "Model resolution 100%%, Peripheral compression / Finished Picture / Deferred / debug views off, RunBeforeSR "
        "from NRHookPoint)%s%s", found, sizeof g_locks / sizeof g_locks[0], missing.empty() ? "" : "; not found: ",
        missing.c_str());
    // Kept locked while the game runs: OptiScaler's menu may set any of them at any time.
    std::thread([all = missing.empty()] {
        RenoDXFile file;
        RenoDXConfig cfg;
        const std::string ini = std::string(module_folder()) + "\\dlssnr-amd.ini";
        read_renodx(ini, cfg);
        for (;;) {
            file.poll(ini, cfg);
            lock_all(cfg.values.hook_point);
            if (all) g_all_locked.store(true);
            Sleep(screenshot::armed() ? 5 : 50);
        }
    }).detach();
}

// The function whose code holds the one reference to the string `s` (unique in the image), or null.
uint8_t* string_function(const Module& m, IMAGE_NT_HEADERS* nt, const char* s) {
    static const uint8_t kLea[] = {0x48, 0x8D, 0x05, 0, 0, 0, 0};
    static const char kLeaMask[] = "???????";
    const size_t n = std::strlen(s) + 1;
    uint8_t* at = nullptr;
    int count = 0;
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (sec->Characteristics & IMAGE_SCN_MEM_EXECUTE) continue;
        uint8_t* p = m.base + sec->VirtualAddress;
        for (size_t k = 0; k + n <= sec->Misc.VirtualSize; ++k)
            if (std::memcmp(p + k, s, n) == 0) { at = p + k; ++count; }
    }
    uint8_t* ref = nullptr;
    if (count != 1 || find_unique(m, kLea, kLeaMask, sizeof kLea, [&](uint8_t* q) {
            return (q[0] == 0x48 || q[0] == 0x4C) && q[1] == 0x8D && (q[2] & 0xC7) == 0x05 && rel32_target(q + 3) == at;
        }, &ref) != 1)
        return nullptr;
    return function_start(ref);
}

void install(const Module& m) {
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(m.base + reinterpret_cast<IMAGE_DOS_HEADER*>(m.base)->e_lfanew);
    // 1. The string only DlssNr_Dx12::DispatchCompute logs (its name is part of it).
    static const char kText[] = "DlssNr_Dx12::DispatchCompute [{0}] Failed to create a constants buffer";
    uint8_t* text = nullptr;
    int texts = 0;
    {
        IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
            if (sec->Characteristics & IMAGE_SCN_MEM_EXECUTE) continue;
            uint8_t* p = m.base + sec->VirtualAddress;
            for (size_t k = 0; k + sizeof kText <= sec->Misc.VirtualSize; ++k)
                if (std::memcmp(p + k, kText, sizeof kText) == 0) { text = p + k; ++texts; }
        }
    }
    // 2. The one lea r64, [rip + text], inside DispatchCompute.
    static const uint8_t kLea[] = {0x48, 0x8D, 0x05, 0, 0, 0, 0};
    static const char kLeaMask[] = "???????";   // any lea r64,[rip+d32]; the target decides
    uint8_t* lea = nullptr;
    const int leas = !text ? 0 : find_unique(m, kLea, kLeaMask, sizeof kLea, [&](uint8_t* p) {
        return (p[0] == 0x48 || p[0] == 0x4C) && p[1] == 0x8D && (p[2] & 0xC7) == 0x05 && rel32_target(p + 3) == text;
    }, &lea);
    uint8_t* compute = leas == 1 ? function_start(lea) : nullptr;
    // 3. DispatchPass's call of it: r9 = _pipelineState ([rdi+0x50]), r8 = the constants, rdx = the list,
    //    rcx = this; then the call.
    static const uint8_t kCall[] = {0x4C, 0x8B, 0x4F, 0x50, 0x4C, 0x8B, 0xC5, 0x49, 0x8B, 0xD6, 0x48, 0x8B, 0xCF,
                                    0xE8, 0, 0, 0, 0};
    static const char kCallMask[] = "xxxxxxxxxxxxxx????";
    uint8_t* call = nullptr;
    const int calls = !compute ? 0 : find_unique(m, kCall, kCallMask, sizeof kCall,
                                                 [&](uint8_t* p) { return rel32_target(p + 14) == compute; }, &call);
    // 4. DispatchPass itself, by its prologue.
    static const uint8_t kPrologue[] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x68, 0x10, 0x48,
                                        0x89, 0x70, 0x18, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x70};
    uint8_t* pass = calls == 1 ? function_start(call) : nullptr;
    if (!pass || std::memcmp(pass, kPrologue, sizeof kPrologue) != 0) {
        log("[nr] RenoDX-style option: OptiScaler's DispatchPass not found in %ls (string %d, lea %d, call %d), "
            "the option stays off", m.name, texts, leas, calls);
        return;
    }
    // 5. State::Run: the function holding its three own strings, by its prologue.
    auto unique_string_function = [&](const char* s) { return string_function(m, nt, s); };
    static const uint8_t kRunPrologue[] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41,
                                           0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x81, 0xEC, 0x70, 0x0C, 0x00, 0x00};
    uint8_t* run = unique_string_function("the pre-SR active colour size is invalid");
    if (!run || run != unique_string_function("depth or motion-vector subrect is empty") ||
        std::memcmp(run, kRunPrologue, sizeof kRunPrologue) != 0) {
        log("[nr] RenoDX-style option: OptiScaler's DlssNr_Dx12::State::Run not found in %ls, the option stays off", m.name);
        return;
    }
    // 6. MakeDlssNrPass: the function around the flag reads fix 5 also matches (the part of them fix 5 leaves as it
    //    is), by its prologue.
    static const Sig kFlagReads("8B 8D 80 01 00 00 8B C1 C1 E8 03 24 01 88 45 61 8B C1 D1 E8 24 01 88 85 B4 00 00 00");
    static const uint8_t kMakePrologue[] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x70, 0x18, 0x48, 0x89,
                                            0x78, 0x20, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57};
    uint8_t* reads = nullptr;
    uint8_t* make = find_unique(m, kFlagReads, [](uint8_t*) { return true; }, &reads) == 1 ? function_start(reads) : nullptr;
    if (!make || std::memcmp(make, kMakePrologue, sizeof kMakePrologue) != 0) {
        log("[nr] RenoDX-style option: OptiScaler's MakeDlssNrPass not found in %ls, the option stays off", m.name);
        return;
    }
    // 7. The Vulkan path's two: DlssNr_Vk::Dispatch (its "nothing to write" string) and ModelVk::Impl::Evaluate
    //    (its "the game's buffer is" string), each by its prologue.
    static const uint8_t kVkDispatchPrologue[] = {0x40, 0x53, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56,
                                                  0x48, 0x81, 0xEC, 0x00, 0x01, 0x00, 0x00};
    static const uint8_t kVkEvaluatePrologue[] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54,
                                                  0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x81, 0xEC, 0xF0, 0x0A, 0x00, 0x00};
    uint8_t* vk_pass = unique_string_function("DlssNr_Vk::Dispatch DLSS-NR Vulkan pass: a dispatch with nothing to write");
    uint8_t* vk_eval = unique_string_function(
        "DlssNr::ModelVk::Impl::Evaluate DLSS-NR Vulkan: the game's buffer is {} (flag {}, format {}), depth {}");
    if (!vk_pass || !vk_eval || std::memcmp(vk_pass, kVkDispatchPrologue, sizeof kVkDispatchPrologue) != 0 ||
        std::memcmp(vk_eval, kVkEvaluatePrologue, sizeof kVkEvaluatePrologue) != 0) {
        log("[nr] RenoDX-style option: OptiScaler's Vulkan NR functions not found in %ls, the option stays off", m.name);
        return;
    }
    const MH_STATUS init = MH_Initialize();
    struct { uint8_t* at; void* detour; void** real; } hooks[] = {
        {pass, reinterpret_cast<void*>(&dispatch_pass), reinterpret_cast<void**>(&real_dispatch_pass)},
        {run, reinterpret_cast<void*>(&state_run), reinterpret_cast<void**>(&real_state_run)},
        {make, reinterpret_cast<void*>(&make_pass), reinterpret_cast<void**>(&real_make_pass)},
        {vk_pass, reinterpret_cast<void*>(&vk_dispatch), reinterpret_cast<void**>(&real_vk_dispatch)},
        {vk_eval, reinterpret_cast<void*>(&vk_evaluate), reinterpret_cast<void**>(&real_vk_evaluate)},
    };
    constexpr size_t kHooks = sizeof hooks / sizeof hooks[0];
    bool ok = init == MH_OK || init == MH_ERROR_ALREADY_INITIALIZED;
    size_t made = 0;
    for (; ok && made < kHooks; ++made)
        ok = MH_CreateHook(hooks[made].at, hooks[made].detour, hooks[made].real) == MH_OK;
    if (!ok) {
        for (size_t k = 0; k + 1 < made; ++k) MH_RemoveHook(hooks[k].at);
        log("[nr] RenoDX-style option: OptiScaler could not be hooked, the option stays off");
        return;
    }
    for (auto& h : hooks)
        if (MH_EnableHook(h.at) != MH_OK) {
            for (auto& g : hooks) { MH_DisableHook(g.at); MH_RemoveHook(g.at); }
            log("[nr] RenoDX-style option: OptiScaler's hooks could not be enabled, the option stays off");
            return;
        }
    renodx_route::set_installed();
    find_locks(m, nt);
    log("[nr] RenoDX-style option: OptiScaler hands the frame over and takes the answer back as they are "
        "(%ls: DispatchPass +0x%llx, State::Run +0x%llx, MakeDlssNrPass +0x%llx, DlssNr_Vk::Dispatch +0x%llx, "
        "ModelVk::Impl::Evaluate +0x%llx)", m.name,
        static_cast<unsigned long long>(pass - m.base), static_cast<unsigned long long>(run - m.base),
        static_cast<unsigned long long>(make - m.base), static_cast<unsigned long long>(vk_pass - m.base),
        static_cast<unsigned long long>(vk_eval - m.base));
}

// The plain route's read-only twins of dispatch_pass / vk_dispatch: what OptiScaler's encode used, nothing changed.
bool observe_dispatch_pass(void* self, void* cmd, const uint8_t* constants, void* source, void* model, void* original,
                           void* motion, void* previous, void* target, void* keep, uint32_t* slot) {
    see_encode(constants);
    return real_dispatch_pass(self, cmd, constants, source, model, original, motion, previous, target, keep, slot);
}
bool observe_vk_dispatch(void* self, void* cmd, const uint8_t* constants, uint64_t tx, uint64_t ty, uint64_t source,
                         uint64_t model, uint64_t original, uint64_t motion, uint64_t target, uint64_t keep,
                         uint64_t source_layout, uint64_t motion_layout, uint64_t finished, uint32_t* slot) {
    if ((finished & 0xFF) == 0) see_encode(constants);
    return real_vk_dispatch(self, cmd, constants, tx, ty, source, model, original, motion, target, keep, source_layout,
                            motion_layout, finished, slot);
}

// DlssNr_Dx12::DispatchPass, by the string only DispatchCompute logs, its one lea, DispatchPass's call of it and the
// prologue (the same steps install() takes); null when any step does not find exactly one.
uint8_t* find_dispatch_pass(const Module& m, IMAGE_NT_HEADERS* nt) {
    static const char kText[] = "DlssNr_Dx12::DispatchCompute [{0}] Failed to create a constants buffer";
    uint8_t* text = nullptr;
    int texts = 0;
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (sec->Characteristics & IMAGE_SCN_MEM_EXECUTE) continue;
        uint8_t* p = m.base + sec->VirtualAddress;
        for (size_t k = 0; k + sizeof kText <= sec->Misc.VirtualSize; ++k)
            if (std::memcmp(p + k, kText, sizeof kText) == 0) { text = p + k; ++texts; }
    }
    if (texts != 1) return nullptr;
    static const uint8_t kLea[] = {0x48, 0x8D, 0x05, 0, 0, 0, 0};
    static const char kLeaMask[] = "???????";
    uint8_t* lea = nullptr;
    if (find_unique(m, kLea, kLeaMask, sizeof kLea, [&](uint8_t* p) {
            return (p[0] == 0x48 || p[0] == 0x4C) && p[1] == 0x8D && (p[2] & 0xC7) == 0x05 && rel32_target(p + 3) == text;
        }, &lea) != 1)
        return nullptr;
    uint8_t* compute = function_start(lea);
    static const uint8_t kCall[] = {0x4C, 0x8B, 0x4F, 0x50, 0x4C, 0x8B, 0xC5, 0x49, 0x8B, 0xD6, 0x48, 0x8B, 0xCF,
                                    0xE8, 0, 0, 0, 0};
    static const char kCallMask[] = "xxxxxxxxxxxxxx????";
    uint8_t* call = nullptr;
    if (!compute || find_unique(m, kCall, kCallMask, sizeof kCall, [&](uint8_t* p) { return rel32_target(p + 14) == compute; },
                                &call) != 1)
        return nullptr;
    static const uint8_t kPrologue[] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x68, 0x10, 0x48,
                                        0x89, 0x70, 0x18, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x70};
    uint8_t* pass = function_start(call);
    return pass && std::memcmp(pass, kPrologue, sizeof kPrologue) == 0 ? pass : nullptr;
}

// The plain OptiScaler route: OptiScaler's frame for the menus' engine-data panel (renodx_route::current_frame: where NR
// runs, the game's exposure texture, its DLSS create flags and sizes). Read-only detours on State::Run, MakeDlssNrPass and
// ModelVk::Impl::Evaluate - the frame and the game's parameters are read, nothing is changed - each where it is found;
// with NR_OPTI_PROF=1 State::Run's also takes its timestamps. DispatchPass and DlssNr_Vk::Dispatch are watched for what
// the encode used (renodx_route::encode_seen), their constants passed on untouched.
void install_observer(const Module& m) {
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(m.base + reinterpret_cast<IMAGE_DOS_HEADER*>(m.base)->e_lfanew);
    static const uint8_t kRunPrologue[] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41,
                                           0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x81, 0xEC, 0x70, 0x0C, 0x00, 0x00};
    uint8_t* run = string_function(m, nt, "the pre-SR active colour size is invalid");
    if (!run || run != string_function(m, nt, "depth or motion-vector subrect is empty") ||
        std::memcmp(run, kRunPrologue, sizeof kRunPrologue) != 0)
        run = nullptr;
    static const Sig kFlagReads("8B 8D 80 01 00 00 8B C1 C1 E8 03 24 01 88 45 61 8B C1 D1 E8 24 01 88 85 B4 00 00 00");
    static const uint8_t kMakePrologue[] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x70, 0x18, 0x48, 0x89,
                                            0x78, 0x20, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57};
    uint8_t* reads = nullptr;
    uint8_t* make = find_unique(m, kFlagReads, [](uint8_t*) { return true; }, &reads) == 1 ? function_start(reads) : nullptr;
    if (make && std::memcmp(make, kMakePrologue, sizeof kMakePrologue) != 0) make = nullptr;
    static const uint8_t kVkEvaluatePrologue[] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54,
                                                  0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x81, 0xEC, 0xF0, 0x0A, 0x00, 0x00};
    uint8_t* vk_eval = string_function(
        m, nt, "DlssNr::ModelVk::Impl::Evaluate DLSS-NR Vulkan: the game's buffer is {} (flag {}, format {}), depth {}");
    if (vk_eval && std::memcmp(vk_eval, kVkEvaluatePrologue, sizeof kVkEvaluatePrologue) != 0) vk_eval = nullptr;
    const MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
        log("[nr] engine data: MinHook could not start, OptiScaler's frame is not read");
        return;
    }
    uint8_t* pass = find_dispatch_pass(m, nt);
    static const uint8_t kVkDispatchPrologue[] = {0x40, 0x53, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56,
                                                  0x48, 0x81, 0xEC, 0x00, 0x01, 0x00, 0x00};
    uint8_t* vk_pass = string_function(m, nt, "DlssNr_Vk::Dispatch DLSS-NR Vulkan pass: a dispatch with nothing to write");
    if (vk_pass && std::memcmp(vk_pass, kVkDispatchPrologue, sizeof kVkDispatchPrologue) != 0) vk_pass = nullptr;
    struct { uint8_t* at; void* detour; void** real; const char* name; } hooks[] = {
        {pass, reinterpret_cast<void*>(&observe_dispatch_pass), reinterpret_cast<void**>(&real_dispatch_pass), "DispatchPass"},
        {vk_pass, reinterpret_cast<void*>(&observe_vk_dispatch), reinterpret_cast<void**>(&real_vk_dispatch),
         "DlssNr_Vk::Dispatch"},
        {run, reinterpret_cast<void*>(&state_run), reinterpret_cast<void**>(&real_state_run), "State::Run"},
        {make, reinterpret_cast<void*>(&make_pass), reinterpret_cast<void**>(&real_make_pass), "MakeDlssNrPass"},
        {vk_eval, reinterpret_cast<void*>(&vk_evaluate), reinterpret_cast<void**>(&real_vk_evaluate),
         "ModelVk::Impl::Evaluate"},
    };
    std::string made, missed;
    for (auto& h : hooks) {
        const bool ok = h.at && MH_CreateHook(h.at, h.detour, h.real) == MH_OK &&
                        (MH_EnableHook(h.at) == MH_OK || (MH_RemoveHook(h.at), false));
        std::string& list = ok ? made : missed;
        if (!list.empty()) list += ", ";
        list += h.name;
    }
    log("[nr] engine data: OptiScaler's frame read through %s%s%s (%ls; nothing in it is changed)",
        made.empty() ? "nothing" : made.c_str(), missed.empty() ? "" : "; not found: ", missed.c_str(), m.name);
}

}  // namespace renodx_impl

}  // namespace
#endif

void fix_optiscaler() {
#if defined(_WIN64)
    if (const char* e = std::getenv("NR_OPTISCALER_FIX"); e && e[0] == '0') {
        log("[nr] OptiScaler fixes: off (NR_OPTISCALER_FIX=0)");
        return;
    }
    Module m{};
    if (!find_module(&m)) {
        log("[nr] OptiScaler fixes: no OptiScaler module found, nothing changed");
        return;
    }
    fix_extension_query(m);
    fix_finished_picture(m);
    fix_window_sized_swapchain(m);
    fix_depth_stencil_guide(m);
    fix_autoexposure_hdr(m);
    fix_release_hold(m);
    fix_float_formats(m);
    fix_feature_resize(m);
    // The RenoDX-style option, only when it is on at start (nothing of it is installed otherwise).
    RenoDXConfig renodx{};
    if (read_renodx(std::string(module_folder()) + "\\dlssnr-amd.ini", renodx) && renodx.enabled)
        renodx_impl::install(m);
    else
        renodx_impl::install_observer(m);   // read-only, for the engine-data panel (and NR_OPTI_PROF's timestamps)
#endif
}

}  // namespace nr::pe
