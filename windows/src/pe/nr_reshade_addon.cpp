// The fallback as a ReShade add-on, in DLSS5-Feeder's shape.
//
// For a game with no upscaler we can intercept there is no engine data, and the
// Present-time fallback had to estimate motion from the finished frame - which
// flickers wherever the picture is flat. DLSS5-Feeder solved the same problem
// for NVIDIA's runtime by not estimating at all: its companion effect,
// DLSS5_Feed.fx (MIT), reads a community motion-vector shader's output
// (qUINT, VORT, Launchpad, LumeniteFX), validates every vector against the
// previous frame, and hands over DLSS5_MV (pixels, prev = uv + mv),
// DLSS5_Depth (raw hardware depth) and DLSS5_Mask. All of that is ReShade
// effects and years of community tuning; none of it needs to be ours.
//
// So this add-on is Feeder's ReShade half with its NGX half replaced by our
// Session: on `reshade_render_technique` for DLSS5_Feed it takes the back
// buffer, the two guide textures and the depth orientation, and runs the
// network on the game's own device through the same DXVK / vkd3d-proton
// interop the standalone module uses. D3D10, D3D11 and D3D12 here, and Vulkan when
// ReShade is loaded as a Vulkan layer - which under Proton is also how a
// D3D9 game is reached, since its d3d9.dll is DXVK and ReShade then sees
// Vulkan rather than the SM3-only D3D9 backend. ReShade provides the events,
// the textures and the overlay, and it already runs under Proton.
//
// Layout: this file sits next to ReShade's own DLL in the game directory, with
// build/ and artifacts/ beside it, exactly like the standalone module. dlssnr-amd.ini
// is shared with it.
#include "nr_pe_bridge.hpp"
#include "nr_screenshot.hpp"
#include "nr_pe_config.hpp"
#include "nr_control_ui.hpp"
#include "nr_pe_interop.hpp"
#ifndef NR_BUILD_STAMP
#define NR_BUILD_STAMP "unknown"
#endif
#include "nr_pe_crash.hpp"
#include "nr_pe_log.hpp"
#include "nr_pe_session.hpp"
#include "nr_engine_panel.hpp"
#include "nr_pe_vkdevice.hpp"

#include <MinHook.h>
#include <d3dcommon.h>
#include <dxgi1_6.h>
#include <tlhelp32.h>

// ReShade's overlay draws through its own ImGui function table; the header
// must see imgui.h first, with the texture id type ReShade expects.
#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <map>
#include <memory>
#include <optional>
#include <type_traits>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

extern "C" __declspec(dllexport) const char* NAME = "DLSS 5 NR on AMD";
extern "C" __declspec(dllexport) const char* DESCRIPTION =
    "Runs the DLSS 5 neural-rendering network on AMD, on this frame, using the motion vectors "
    "DLSS5_Feed.fx produces from a community motion-vector shader. Enable a provider (VORT, "
    "qUINT, ...) above DLSS5_Feed in the effect list. Settings: dlssnr-amd.ini next to this add-on.";

// The generated thunks' loader (windows/build/build_package.sh): nothing pulls
// Vulkan in until a game has its own graphics stack up.
extern "C" void nr_vk_load(void);
extern "C" void* nr_vk_p_vkGetDeviceQueue;
extern "C" void* nr_vk_p_vkGetPhysicalDeviceQueueFamilyProperties;
extern "C" void* nr_vk_p_vkCmdClearColorImage;

// Calling a ReShade method that returns a struct, the way MSVC expects it.
//
// ReShade is built with MSVC and this add-on with mingw, and the two disagree
// on exactly one thing that matters here: how a member function returns a
// struct by value. On x86, MSVC always returns it through a hidden pointer
// passed as the first stack argument, with `this` in ECX and the callee
// popping the pointer with the arguments (`ret 0xc` for two arguments) - read
// out of ReShade32.dll 6.8.0 at find_technique, which is where Tomb Raider
// 2013 died. mingw returns an 8-byte struct in EDX:EAX, and for a larger one
// puts the hidden pointer in ECX and `this` on the stack. Neither matches, and
// no compiler flag makes them match (-fpcc-struct-return keeps ECX for the
// pointer). On x64 MSVC returns *every* struct from a member function through
// the hidden pointer, however small (`this` in RCX, pointer in RDX, the same
// as a free function taking (this, pointer, ...)); mingw returns an 8-byte one
// in RAX. An earlier version of this comment said the small case agreed on
// x64: it does not. ReShade64.dll 6.8.0's find_technique ends in
// `mov %rax,(%r15)` with r15 = RDX, and FFXIV (DX11 over DXVK, the first
// 64-bit game on this route) died there writing the handle over
// "DLSS5_Feed.fx" - the Tomb Raider fault again, on the other target.
//
// So the call is made explicitly: the vtable slot comes from the member
// function pointer (Itanium ABI: odd `ptr` = 1 + byte offset into the vtable,
// which is what mingw's g++ uses on every target), and the slot is invoked as
// a plain function `Ret* (this, Ret* out, args...)` in the convention that
// puts `this` first and the hidden pointer second - __thiscall on x86, the
// default on x64. Every struct-returning ReShade call in this file goes
// through here; a pointer-, integer- or void-returning one needs nothing.
namespace rs {
#if defined(__i386__)
#define RS_CALL __thiscall
#else
#define RS_CALL
#endif
template <class PMF> size_t slot(PMF pmf) {
    union { PMF p; struct { uintptr_t ptr; ptrdiff_t adj; } r; } u;
    u.p = pmf;
    if (!(u.r.ptr & 1u) || u.r.adj != 0) {
        nr::pe::log("[nr] FATAL: a ReShade method is not a plain virtual (ptr %p adj %ld)",
                    reinterpret_cast<void*>(u.r.ptr), long(u.r.adj));
        std::abort();
    }
    return (u.r.ptr - 1u) / sizeof(void*);
}
template <class Ret, class C, class... A, class... B>
Ret call(C* self, Ret (C::*pmf)(A...), B&&... b) {
    static_assert(std::is_class_v<Ret>, "only struct returns go through rs::call (an enum comes back in EAX/RAX)");
    using Fn = Ret* (RS_CALL*)(C*, Ret*, A...);
    const auto fn = reinterpret_cast<Fn>((*reinterpret_cast<void* const* const*>(self))[slot(pmf)]);
    Ret out{};
    fn(self, &out, static_cast<A>(b)...);
    return out;
}
template <class Ret, class C, class... A, class... B>
Ret call(const C* self, Ret (C::*pmf)(A...) const, B&&... b) {
    static_assert(std::is_class_v<Ret>, "only struct returns go through rs::call (an enum comes back in EAX/RAX)");
    using Fn = Ret* (RS_CALL*)(const C*, Ret*, A...);
    const auto fn = reinterpret_cast<Fn>((*reinterpret_cast<void* const* const*>(self))[slot(pmf)]);
    Ret out{};
    fn(self, &out, static_cast<A>(b)...);
    return out;
}
}  // namespace rs

namespace {

using namespace reshade::api;
using nr::pe::log;

constexpr const char* kEffect = "DLSS5_Feed.fx";
constexpr const char* kTechnique = "DLSS5_Feed";

std::mutex lock;
std::unique_ptr<nr::pe::Session> session;
nr::pe::Config config;
std::string config_path;
nr::pe::PreprocessSwitch prep_switch;   // [Preprocess] Enabled and its hotkey
bool vk_loaded = false;

// [RenoDX] Enabled as it was when the add-on loaded: the option is switched by restarting the game (its other
// keys follow the file while the game runs).
bool renodx_on = false;

// The one runtime we feed. A game usually has one; a second one (a proxy
// swapchain, a second window) is ignored until the first goes away.
effect_runtime* bound = nullptr;
effect_technique technique{};
effect_texture_variable mv_var{}, depth_var{};
// The engine-data panel's facts from the last frame fed (feed), drawn on the add-on page.
nr::pe::panel::Facts panel_facts;
bool need_reset = true;
// The bound effect runtime's presents, and their count when NR last ran a frame: frames between two NR frames
// without NR ([DlssNr] Enabled or RenoDX's NeuralUplift off, the technique off, frames declined) and the next NR
// frame starts its history afresh, as OptiScaler's features do when NR comes back on.
uint64_t presents = 0, presents_at_run = 0;
bool have_run = false;
// More presents than NR frames by up to this many is frame generation (a generated frame between rendered ones,
// several with multi-frame generation), not NR having been off.
constexpr uint64_t kResumeGap = 8;
bool missing_reported = false;
std::string last_status;
unsigned frames_run = 0, frames_declined = 0;

// The motion-vector source (VORT or LumeniteFX Kernel), below.
void motion_after_reload();
void motion_choice_changed();

const nr::Controls& settings() {
    const int motion_was = config.motion_vectors;
    if (!config_path.empty() && config.reload(config_path)) {
        if (config.motion_vectors != motion_was) {   // MotionVectors edited in the file: apply it at the next present
            motion_choice_changed();
            log("[nr] settings reloaded: MotionVectors %s", config.motion_vectors == 1 ? "lumenite" : "vort");
        }
        if (session) {
        session->set_history_strength(renodx_on ? 1.0f : config.history);
        session->set_white_point(config.white_point);
        session->set_max_passes(renodx_on ? 1u : uint32_t(config.controls.passes));
    }
        static const char* const transfers[] = {"matched residual", "edge-aware lighting + colour", "classic"};
        static const char* const scalers[] = {"bilinear", "catmullrom", "lanczos3", "fsr1"};
        log("[nr] settings reloaded: intensity %.2f tone %.2f structure %.2f skin %.2f mask %d history %.2f passes %d model_scale %.2f"
            " enlargement %s%s%s%s",
            config.controls.intensity, config.controls.local_tone, config.controls.local_structure,
            config.controls.skin_structure, int(config.controls.automatic_mask), config.history,
            config.controls.passes, config.model_scale, transfers[std::clamp(config.controls.transfer, 0, 2)],
            config.controls.transfer == nr::kEnlargeClassic ? " " : "",
            config.controls.transfer == nr::kEnlargeClassic ? scalers[std::clamp(config.controls.classic_scaler, 0, 3)] : "",
            config.controls.noise_every_frame ? "; NoiseEveryFrame on" : "");
        if (renodx_on)
            log("[nr] RenoDX-style option: [RenoDX] read (colour %.2f, transfer %.2f, resolution scale %.2f, detail "
                "stability %u)", config.renodx.values.color_strength, config.renodx.values.transfer_strength,
                config.renodx.values.resolution_scale, config.renodx.values.detail_stability);
    }
    if (session) session->set_model_scale(renodx_on ? 1.0f : config.model_scale);
    config.controls.preprocess = prep_switch.frame(config.preprocess);
    return config.controls;
}

nr::pe::Session& the_session() {
    if (!session) {
        session = std::make_unique<nr::pe::Session>(std::string{});
        // **No native compose on this route.** `native_compose` means "the host
        // resolves afterwards, so hand back the DLL's own tail and nothing
        // else", and the only host of ours that does is OptiScaler. Nothing
        // resolves after this add-on: it is its own consumer.
        //
        // (For the record, since an earlier version of this comment got it
        // wrong: `DLSS5_Feed.fx` produces motion vectors, depth and a mask and
        // nothing else - it composes nothing and depends on no consumer. In the
        // original Feeder the composition belongs to whichever *neural
        // consumer* is installed - Deep Fried Chicken, renodx-dlss5, or
        // OptiScaler_DLSSNR - and only the third of those does it the way this
        // project's OptiScaler route does.)
        //
        // Setting it here also silently clamped two shipped controls,
        // because RuntimeConfig derives them from it:
        //     max_passes  = native_compose ? 1 : kMaxPasses   -> passes 2..4 did nothing,
        //     model_scale = 1                                 -> Model Resolution did nothing
        // which is exactly what a user reported: identical pictures and
        // identical frame times at every pass count, and a resolution slider
        // that only ever rebuilt the network at full size. The composition
        // difference is 0.3% of mean luminance on a real frame (x0.9751 through
        // the transfer pass against x0.9718 native, measured 2026-09-17), so
        // nothing is given up by doing it the way this route needs.
        session->set_native_compose(false);
        session->set_history_strength(renodx_on ? 1.0f : config.history);
        session->set_white_point(config.white_point);
        session->set_model_scale(renodx_on ? 1.0f : config.model_scale);
        session->set_max_passes(renodx_on ? 1u : uint32_t(config.controls.passes));
    }
    return *session;
}

void report(const std::string& status) {
    // A background build's notice goes once the network it announced is in (the session's status is then empty).
    if (status.empty() && last_status.rfind("building the network", 0) == 0) { last_status.clear(); return; }
    if (status.empty() || status == last_status) return;
    last_status = status;
    log("[nr] %s", status.c_str());
    reshade::log::message(reshade::log::level::info, ("nr: " + status).c_str());
}


void resolve(effect_runtime* rt) {
    motion_after_reload();
    technique = rs::call(rt, &effect_runtime::find_technique, kEffect, kTechnique);
    mv_var = rs::call(rt, &effect_runtime::find_texture_variable, kEffect, "DLSS5_MV");
    depth_var = rs::call(rt, &effect_runtime::find_texture_variable, kEffect, "DLSS5_Depth");
    log("[nr] effect runtime %p: %s %s; DLSS5_MV %s, DLSS5_Depth %s", static_cast<void*>(rt),
        kTechnique, technique.handle ? "found" : "MISSING - install DLSS5_Feed.fx and a motion-vector provider",
        mv_var.handle ? "found" : "missing", depth_var.handle ? "found" : "missing");
    missing_reported = false;
}

// Which module a crash landed in. Written before anything else is tried, so a
// log that ends here says whether the fault was ours.
LPTOP_LEVEL_EXCEPTION_FILTER previous_filter = nullptr;
LONG WINAPI crash_filter(EXCEPTION_POINTERS* e) {
    const auto* rec = e ? e->ExceptionRecord : nullptr;
    void* at = rec ? rec->ExceptionAddress : nullptr;
    char module[MAX_PATH] = "?";
    HMODULE m = nullptr;
    if (at && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                 GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                 static_cast<LPCSTR>(at), &m) && m)
        GetModuleFileNameA(m, module, MAX_PATH);
    log("[nr] CRASH: exception 0x%08lX at %p in %s (module base %p)",
        rec ? rec->ExceptionCode : 0ul, at, module, static_cast<void*>(m));
    return previous_filter ? previous_filter(e) : EXCEPTION_CONTINUE_SEARCH;
}

void vulkan_selftest(VkDevice device);

// **What domain the back buffer is in, which this route had never asked.**
//
// The header on `D3D11Frame::upscaler_input` says a finished back buffer is
// display-referred "whatever its format", and for an SDR swapchain that is
// right. It is wrong for an HDR one: scRGB is linear light with 1.0 at SDR
// white, HDR10 is PQ over BT.2020. Feeding one of those in raw is the same fault the upscaler path
// once had - and there the picture came out dark with coloured blocks.
//
// ReShade knows: `swapchain::get_color_space()`. The effect runtime is not a
// swapchain, so it is taken from init_swapchain and kept.
color_space back_buffer_space = color_space::unknown;

// The RenoDX-style option at RenoDX's Present hook point (present_path.hpp), which is where this route runs: the
// back buffer's encoding from the colour space - 1 SDR, 2 HDR10 (PQ over BT.2020), 3 scRGB - with NRPresentEncoding
// able to force one the back buffer can carry; 0 = not served (HLG, unknown).
uint32_t present_encoding(const nr::renodx::Settings& s, DXGI_FORMAT f) {
    const bool float_format = f == DXGI_FORMAT_R16G16B16A16_FLOAT;
    const uint32_t automatic = back_buffer_space == color_space::srgb_nonlinear ? 1u
                               : back_buffer_space == color_space::hdr10_st2084 ? 2u
                               : back_buffer_space == color_space::extended_srgb_linear ? 3u : 0u;
    uint32_t e = s.present_encoding;
    if (e != 0 && automatic != 0 &&
        ((e == 2 && f != DXGI_FORMAT_R10G10B10A2_UNORM && !float_format) || (e == 3 && !float_format)))
        e = automatic;
    if (e == 0) e = automatic;
    if (e == 2 && f != DXGI_FORMAT_R10G10B10A2_UNORM && !float_format) return 0;
    if (e == 3 && !float_format) return 0;
    return e;
}

// QueryPresentSdrWhite: the Windows SDR content level of the display the swap chain is on, in nits, only while it
// runs HDR (1000 = 80 nits; 200 reads as 203); 0 when it cannot be read (no DXGI swap chain, Proton without it).
float query_sdr_white(effect_runtime* rt) {
    const device_api api = rt->get_device()->get_api();
    if (api != device_api::d3d10 && api != device_api::d3d11 && api != device_api::d3d12) return 0.f;
    auto* native = reinterpret_cast<IDXGISwapChain*>(rt->get_native());
    IDXGIOutput* output = nullptr;
    if (!native || FAILED(native->GetContainingOutput(&output)) || !output) return 0.f;
    DXGI_OUTPUT_DESC1 desc{};
    IDXGIOutput6* output6 = nullptr;
    const bool described = SUCCEEDED(output->QueryInterface(__uuidof(IDXGIOutput6), reinterpret_cast<void**>(&output6))) &&
                           SUCCEEDED(output6->GetDesc1(&desc));
    if (output6) output6->Release();
    output->Release();
    if (!described || desc.ColorSpace != DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) return 0.f;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    LONG result = ERROR_INSUFFICIENT_BUFFER;
    UINT32 path_count = 0;
    while (result == ERROR_INSUFFICIENT_BUFFER) {
        UINT32 mode_count = 0;
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count) != ERROR_SUCCESS) return 0.f;
        paths.resize(path_count);
        modes.resize(mode_count);
        result = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(), &mode_count, modes.data(), nullptr);
    }
    if (result != ERROR_SUCCESS) return 0.f;
    for (UINT32 i = 0; i < path_count; ++i) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = paths[i].sourceInfo.adapterId;
        source.header.id = paths[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS || wcscmp(source.viewGdiDeviceName, desc.DeviceName) != 0)
            continue;
        DISPLAYCONFIG_SDR_WHITE_LEVEL white{};
        white.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
        white.header.size = sizeof(white);
        white.header.adapterId = paths[i].targetInfo.adapterId;
        white.header.id = paths[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&white.header) != ERROR_SUCCESS) return 0.f;
        const float nits = float(white.SDRWhiteLevel) / 1000.f * 80.f;
        return nits == 200.f ? 203.f : nits;
    }
    return 0.f;
}

// A RenoDX add-on (renodx-*.addon*) other than this one in the process: its Game Brightness is the HDR white
// (present_path.hpp present_mod_white).
bool renodx_mod_loaded() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32W me{};
    me.dwSize = sizeof me;
    bool found = false;
    for (BOOL ok = Module32FirstW(snap, &me); ok && !found; ok = Module32NextW(snap, &me)) {
        std::wstring w = me.szModule;
        for (wchar_t& c : w) c = wchar_t(std::towlower(c));
        found = w.rfind(L"renodx-", 0) == 0 && w.find(L".addon") != std::wstring::npos;
    }
    CloseHandle(snap);
    return found;
}

// The HDR diffuse white, as RenoDX's Present path takes it (polled at most once a second: DisplayConfig can block):
// NRPresentWhiteNits; else a RenoDX mod's ToneMapGameNits ([renodx-preset1] with its tone mapper on, 48..10000,
// NRPresentWhiteSource 0 only); else the Windows SDR content level; else NRDiffuseWhiteNits.
float present_white(effect_runtime* rt, const nr::renodx::Settings& s) {
    static uint64_t polled = 0;
    static float windows = 0.f, mod = 0.f;
    const uint64_t now = GetTickCount64();
    if (polled == 0 || now - polled >= 1000) {
        polled = now;
        windows = query_sdr_white(rt);
        float tone_mapper = 0.f, game_nits = 0.f;
        mod = renodx_mod_loaded() && reshade::get_config_value(nullptr, "renodx-preset1", "ToneMapType", tone_mapper) &&
                      tone_mapper != 0.f &&
                      reshade::get_config_value(nullptr, "renodx-preset1", "ToneMapGameNits", game_nits) &&
                      game_nits >= 48.f && game_nits <= 10000.f
                  ? game_nits : 0.f;
    }
    const float m = s.present_white_source == 0 ? mod : 0.f;
    const float white = s.present_white_nits > 0.f ? s.present_white_nits : m > 0.f ? m : windows > 0.f ? windows
                                                                                                 : s.diffuse_white_nits;
    static float logged = -1.f;
    if (white != logged) {
        logged = white;
        log("[nr] RenoDX-style option: HDR diffuse white %.0f nits from %s", white,
            s.present_white_nits > 0.f ? "NRPresentWhiteNits" : m > 0.f ? "the RenoDX mod's Game Brightness"
            : windows > 0.f ? "the Windows SDR content brightness" : "NRDiffuseWhiteNits");
    }
    return white;
}

const char* space_name(color_space cs) {
    switch (cs) {
        case color_space::srgb_nonlinear: return "sRGB (display referred)";
        case color_space::extended_srgb_linear: return "scRGB (linear light, HDR)";
        case color_space::hdr10_st2084: return "HDR10 PQ";
        case color_space::hdr10_hlg: return "HDR10 HLG";
        default: return "unknown";
    }
}

// Model resolution below 100% reads the back buffer and writes the answer into it in place when the
// back buffer can be sampled and written as storage; otherwise it is copied out and back. Those two
// usages are asked for when the swap chain is created, only where they cannot make the creation fail:
// Vulkan swap chains without other view formats, in an 8-bit UNORM or RGBA16F format (the formats the
// answer is stored in directly). ReShade does not retry a creation that fails with the add-on's
// description, so nothing else is touched.
bool on_create_swapchain(device_api api, swapchain_desc& desc, void*) {
    // On Windows the D3D10/11/12 games run on the system's D3D and the bridge copies the frame, so only
    // the Vulkan layer routes (DX9 through DXVK, Vulkan games) use the back buffer in place.
    if (api != device_api::vulkan) return false;
    const format f = desc.back_buffer.texture.format;
    if (f != format::r8g8b8a8_unorm && f != format::b8g8r8a8_unorm && f != format::r16g16b16a16_float) return false;
    constexpr uint32_t kVkMutableFormat = 0x4;   // VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR
    if (api == device_api::vulkan && (desc.present_flags & kVkMutableFormat)) return false;
    const resource_usage want = resource_usage::shader_resource | resource_usage::unordered_access;
    if ((desc.back_buffer.usage & want) == want) return false;
    desc.back_buffer.usage |= want;
    log("[nr] swap chain: back buffers made sampled and storage (format %u)", unsigned(f));
    return true;
}

void on_init_swapchain(swapchain* sc, bool) {
    // **Called directly, NOT through rs::call.** That shim exists for MSVC's
    // hidden-pointer struct return and takes that path for any return type it
    // is given (now refused at compile time). `get_color_space` returns a plain enum, which
    // MSVC hands back in EAX, so routing it through the shim pushes an argument
    // the callee never pops - and __thiscall makes the callee clean the stack.
    // Half-Life 2 hung on the first swapchain, and the colour space read as
    // 216789112, which is not one of the five values it can be.
    const color_space cs = sc->get_color_space();
    if (cs == back_buffer_space) return;
    back_buffer_space = cs;
    log("[nr] swapchain colour space: %s (%u)", space_name(cs), unsigned(cs));
}

// ---------------------------------------------------------------------------
// NR_DEBUG_DUMP=1: what this route actually showed the model, and what it got
// back.
//
// The OptiScaler route has had this since 2026-09-16 and it is the only reason
// that side could be diagnosed at all - the picture a user describes is not a
// measurement, and a screenshot of the final frame cannot say whether the model
// or the path around it moved it. Written through ReShade's own abstraction, so
// one implementation covers D3D11, D3D12 and Vulkan.
//
// Sampled at evaluates 1, 2, 3 and then every 300th, overwriting: the first
// pairs are usually taken while the network is still building (a passthrough),
// and the file left behind should be from a frame the model really ran.
struct DebugDump {
    resource before{}, after{};
    bool pending = false;
    unsigned w = 0, h = 0;
    format fmt = format::unknown;
    uint64_t seen = 0;
};
DebugDump dump_state;

bool debug_dump_on() {
    static const int on = [] {
        char v[8] = {};
        const DWORD n = GetEnvironmentVariableA("NR_DEBUG_DUMP", v, sizeof v);
        const int yes = (n > 0 && v[0] == '1') ? 1 : 0;
        if (yes) log("[nr] NR_DEBUG_DUMP=1: the back buffer before and after the pass is written as PPM");
        return yes;
    }();
    return on != 0;
}

// ReShade's format enum takes DXGI's numbering for everything a swapchain uses,
// so this is the same table the model's readback has, minus the ones a back
// buffer cannot be.
unsigned dump_bpp(format f) {
    switch (f) {
        case format::r8g8b8a8_unorm: case format::r8g8b8a8_unorm_srgb: case format::r8g8b8a8_typeless:
        case format::b8g8r8a8_unorm: case format::b8g8r8a8_unorm_srgb: case format::b8g8r8a8_typeless:
        case format::b8g8r8x8_unorm: case format::b8g8r8x8_unorm_srgb: case format::b8g8r8x8_typeless:
        case format::r10g10b10a2_unorm: case format::r10g10b10a2_typeless: case format::r11g11b10_float: return 4;
        case format::r16g16b16a16_float: case format::r16g16b16a16_typeless: return 8;
        default: return 0;
    }
}

float dump_half(uint16_t h) {
    const unsigned s = (h >> 15) & 1u, e = (h >> 10) & 0x1Fu, m = h & 0x3FFu;
    float v;
    if (e == 0) v = std::ldexp((float)m, -24);
    else if (e == 31) v = m ? 0.0f : 1e30f;
    else v = std::ldexp((float)(m + 1024u), (int)e - 25);
    return s ? -v : v;
}

void dump_decode(const uint8_t* p, format f, float out[3]) {
    switch (f) {
        // A typeless back buffer (a Vulkan swapchain with mutable format, as DXVK makes them) holds the bits of
        // its UNORM view; a 16-bit one is scRGB, half floats.
        case format::r8g8b8a8_unorm: case format::r8g8b8a8_unorm_srgb: case format::r8g8b8a8_typeless:
            for (int i = 0; i < 3; ++i) out[i] = p[i] / 255.0f; break;
        case format::b8g8r8a8_unorm: case format::b8g8r8a8_unorm_srgb: case format::b8g8r8a8_typeless:
        case format::b8g8r8x8_unorm: case format::b8g8r8x8_unorm_srgb: case format::b8g8r8x8_typeless:
            out[0] = p[2] / 255.0f; out[1] = p[1] / 255.0f; out[2] = p[0] / 255.0f; break;
        case format::r10g10b10a2_unorm: case format::r10g10b10a2_typeless: {
            uint32_t v; std::memcpy(&v, p, 4);
            out[0] = (v & 0x3FFu) / 1023.0f; out[1] = ((v >> 10) & 0x3FFu) / 1023.0f;
            out[2] = ((v >> 20) & 0x3FFu) / 1023.0f; break;
        }
        case format::r11g11b10_float: {
            uint32_t v; std::memcpy(&v, p, 4);
            // 11/11/10 unsigned floats, 5-bit exponents, no sign.
            const unsigned r = v & 0x7FFu, g = (v >> 11) & 0x7FFu, b = (v >> 22) & 0x3FFu;
            auto f11 = [](unsigned x) { return x ? std::ldexp((float)((x & 0x3Fu) + 64u), (int)((x >> 6) & 0x1Fu) - 21) : 0.0f; };
            auto f10 = [](unsigned x) { return x ? std::ldexp((float)((x & 0x1Fu) + 32u), (int)((x >> 5) & 0x1Fu) - 20) : 0.0f; };
            out[0] = f11(r); out[1] = f11(g); out[2] = f10(b); break;
        }
        case format::r16g16b16a16_float: case format::r16g16b16a16_typeless: {
            uint16_t h[3]; std::memcpy(h, p, 6);
            for (int i = 0; i < 3; ++i) out[i] = dump_half(h[i]);
            break;
        }
        default: out[0] = out[1] = out[2] = 0.0f; break;
    }
}

void dump_write(device* dev, resource staging, const char* which) {
    subresource_data map{};
    if (!dev->map_texture_region(staging, 0, nullptr, map_access::read_only, &map)) {
        log("[nr] dump: %s could not be mapped", which);
        return;
    }
    const unsigned bpp = dump_bpp(dump_state.fmt);
    std::string path = nr::pe::log_path() ? nr::pe::log_path() : "";
    const size_t cut = path.find_last_of("\\/");
    path = (cut == std::string::npos) ? std::string() : path.substr(0, cut + 1);
    char name[64];
    std::snprintf(name, sizeof name, "nr-reshade-e%u-%s.ppm", (unsigned)dump_state.seen, which);
    path += name;
    FILE* fp = std::fopen(path.c_str(), "wb");
    double sum[3] = {0, 0, 0};
    if (fp) std::fprintf(fp, "P6\n%u %u\n255\n", dump_state.w, dump_state.h);
    std::vector<uint8_t> row((size_t)dump_state.w * 3);
    for (unsigned y = 0; y < dump_state.h; ++y) {
        const uint8_t* line = (const uint8_t*)map.data + (size_t)y * map.row_pitch;
        for (unsigned x = 0; x < dump_state.w; ++x) {
            float c[3];
            dump_decode(line + (size_t)x * bpp, dump_state.fmt, c);
            for (int i = 0; i < 3; ++i) {
                sum[i] += c[i];
                const float v = c[i] > 1.0f ? 1.0f : (c[i] > 0.0f ? c[i] : 0.0f);
                row[(size_t)x * 3 + i] = (uint8_t)(v * 255.0f + 0.5f);
            }
        }
        if (fp) std::fwrite(row.data(), 1, row.size(), fp);
    }
    if (fp) std::fclose(fp);
    dev->unmap_texture_region(staging, 0);
    const double n = double(dump_state.w) * dump_state.h;
    log("[nr] dump %s: mean RGB %.5f %.5f %.5f -> %s", which, sum[0] / n, sum[1] / n, sum[2] / n,
        fp ? path.c_str() : "(no file)");
}

// Both halves of the previous sampled pair, read one frame later so the copies
// have retired. Same rule as the model's readback, and for the same reason.
void dump_drain(device* dev) {
    if (!dump_state.pending) return;
    dump_state.pending = false;
    dump_write(dev, dump_state.before, "in");
    dump_write(dev, dump_state.after, "out");
}

bool dump_ensure(device* dev, unsigned w, unsigned h, format f) {
    if (dump_state.before.handle && dump_state.w == w && dump_state.h == h && dump_state.fmt == f)
        return true;
    if (dump_state.before.handle) dev->destroy_resource(dump_state.before);
    if (dump_state.after.handle) dev->destroy_resource(dump_state.after);
    dump_state.before = dump_state.after = resource{};
    if (!dump_bpp(f)) {
        log("[nr] dump: format %u is not one this decoder knows", unsigned(f));
        return false;
    }
    resource_desc d(w, h, 1, 1, f, 1, memory_heap::gpu_to_cpu, resource_usage::copy_dest);
    if (!dev->create_resource(d, nullptr, resource_usage::copy_dest, &dump_state.before) ||
        !dev->create_resource(d, nullptr, resource_usage::copy_dest, &dump_state.after)) {
        log("[nr] dump: the staging pair could not be created");
        return false;
    }
    dump_state.w = w; dump_state.h = h; dump_state.fmt = f;
    return true;
}

// ---- Wine's d3dcompiler_47 and `[fastopt]` ------------------------------------
//
// ReShade turns an effect's `[loop]` into `[fastopt]` in the HLSL it generates for shader model 4 and up
// (effect_codegen_hlsl.cpp), and Wine's d3dcompiler_47 (vkd3d-shader) rejects that attribute ("E5017 ...
// Unhandled attribute 'fastopt'") while it ignores `[loop]` itself. The effects in the package have their
// `[loop]` removed for it in the Linux package (the Windows package's are as published, for the system's
// compiler); LumeniteFX is installed from its author's GitHub and is not changed. So when ReShade's own call to D3DCompile fails on `[fastopt]`, it is called again
// with the attribute taken out of the generated HLSL - the same HLSL the effect without `[loop]` gives. Only
// ReShade's calls, only after such a failure; the game's own compiles and every compile that succeeds are untouched.
// MinHook is ours (linked into this module): the device watch (Vulkan) and the D3DCompile retry below use it.
bool minhook_initialized = false;
bool vk_watch = false;
bool minhook_start() {
    if (!minhook_initialized) {
        const MH_STATUS mh = MH_Initialize();
        minhook_initialized = mh == MH_OK || mh == MH_ERROR_ALREADY_INITIALIZED;
    }
    return minhook_initialized;
}

using D3DCompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const void*, void*, LPCSTR, LPCSTR, UINT, UINT,
                                      ID3DBlob**, ID3DBlob**);
D3DCompileFn real_d3dcompile = nullptr;
void* d3dcompile_target = nullptr;
HMODULE reshade_module = nullptr;
std::atomic<unsigned> fastopt_retries{0};

HRESULT WINAPI d3dcompile_hook(LPCVOID src, SIZE_T size, LPCSTR name, const void* defines, void* include,
                               LPCSTR entry, LPCSTR target, UINT flags1, UINT flags2, ID3DBlob** code,
                               ID3DBlob** errors) {
    void* const caller = __builtin_return_address(0);
    const HRESULT hr = real_d3dcompile(src, size, name, defines, include, entry, target, flags1, flags2, code, errors);
    if (SUCCEEDED(hr) || !src || !size || !errors || !*errors) return hr;
    HMODULE from = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCWSTR>(caller), &from) || from != reshade_module)
        return hr;
    const std::string message(static_cast<const char*>((*errors)->GetBufferPointer()), (*errors)->GetBufferSize());
    if (message.find("fastopt") == std::string::npos) return hr;
    static const std::string attribute = "[fastopt] ";   // as ReShade writes it
    const std::string_view text(static_cast<const char*>(src), size);
    std::string hlsl;
    hlsl.reserve(text.size());
    size_t removed = 0;
    for (size_t at = 0;;) {
        const size_t next = text.find(attribute, at);
        if (next == std::string_view::npos) { hlsl.append(text.substr(at)); break; }
        hlsl.append(text.substr(at, next - at));
        at = next + attribute.size();
        ++removed;
    }
    if (!removed) return hr;
    (*errors)->Release(); *errors = nullptr;
    if (code && *code) { (*code)->Release(); *code = nullptr; }
    const HRESULT again = real_d3dcompile(hlsl.data(), hlsl.size(), name, defines, include, entry, target, flags1,
                                          flags2, code, errors);
    const unsigned n = ++fastopt_retries;
    if (n == 1 || FAILED(again))
        log("[nr] d3dcompiler_47 rejected `[fastopt]` in ReShade's HLSL for %s; compiled again without it (%zu "
            "removed): %s", entry ? entry : "?", removed, SUCCEEDED(again) ? "ok" : "FAILED");
    return again;
}

// Before ReShade compiles its effects (it does so once its effect runtime is up, after the device): ReShade loads
// D3DCompiler_47.dll by name under Wine (dll_main.cpp DliNotifyHook2), so this is the module it gets.
void hook_d3dcompile() {
    if (d3dcompile_target || !GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version")) return;
    reshade_module = reshade::internal::get_reshade_module_handle();
    const HMODULE compiler = LoadLibraryW(L"D3DCompiler_47.dll");
    void* const fn = compiler ? reinterpret_cast<void*>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
    if (!fn || !reshade_module) { log("[nr] d3dcompiler_47: not found; no `[fastopt]` retry"); return; }
    if (!minhook_start() || MH_CreateHook(fn, reinterpret_cast<void*>(&d3dcompile_hook),
                                          reinterpret_cast<void**>(&real_d3dcompile)) != MH_OK ||
        MH_EnableHook(fn) != MH_OK) {
        log("[nr] d3dcompiler_47: the `[fastopt]` retry could not be installed");
        return;
    }
    d3dcompile_target = fn;
}

void unhook_d3dcompile() {
    if (!d3dcompile_target) return;
    MH_DisableHook(d3dcompile_target);
    MH_RemoveHook(d3dcompile_target);
    d3dcompile_target = nullptr;
}

// ---- the motion-vector source: VORT (shipped) or LumeniteFX Kernel ---------------------------------------------
//
// dlssnr-amd.ini MotionVectors chooses; this applies it to ReShade - DLSS5_Feed.fx's DLSS5_MV_PROVIDER (2 VORT, 3
// LumeniteFX Kernel) and which provider technique is on - and checks every launch that LumeniteFX still offers what
// DLSS5_Feed reads: its effect compiled, its technique on and before DLSS5_Feed, its flow texture Kernel::tFlow the one
// DLSS5_Feed samples, and vectors that are not all zero while the picture moves (nor NaN, nor beyond the screen).
// LumeniteFX is installed from its author's GitHub and may be a newer version than the one tested; whenever a check
// fails this session goes back to VORT and the add-on page and dlssnr-amd.log say why. The file keeps the choice, so
// the next launch tries again. With VORT chosen nothing here touches ReShade unless it is switching away from
// LumeniteFX.
constexpr const char* kVortEffect = "vort_Motion.fx";
constexpr const char* kVortTechnique = "vort_MotionEffects";
constexpr const char* kLumeniteEffect = "lumenite_Kernel.fx";
constexpr const char* kLumeniteTechnique = "Lumenite_Kernel";
constexpr const char* kLumeniteFlow = "V__Kernel__tFlow";   // Kernel::tFlow under ReShade's unique name ('V' + "::Kernel::" + name, ':' -> '_')

struct MotionSource {
    bool sync_due = true;      // apply the file's choice at the next present
    bool checks_due = false;   // the effects were reloaded: check LumeniteFX
    bool reloading = false;    // a definition changed: the checks wait for ReShade's reload (the effects are the old ones)
    bool lumenite_active = false;   // ReShade is set to LumeniteFX (by us)
    bool fell_back = false;    // a check failed this session
    std::string reason;
    // The data check: every 60th DLSS5_Feed frame (every 600th once the vectors were seen moving) the flow texture
    // and four 32x32 patches of the frame are copied out, and read at the next sample.
    resource flow_copy{}, patch_copy{};
    resource_desc flow_desc{};
    format patch_format = format::unknown;
    bool pending = false;
    uint64_t frames = 0;
    std::vector<uint8_t> last_patches;
    unsigned moving = 0, zero_moving = 0, bad_in_row = 0, samples = 0;
    bool verified = false;     // vectors seen moving with the picture
    std::string probe_failure; // from the data check, applied at the next present
    // Since when DLSS5_Feed has been missing with LumeniteFX set (ReShade compiles every effect again after a switch).
    std::chrono::steady_clock::time_point missing_since{};
};
MotionSource motion;

bool lumenite_on_disk() {
    const std::string folder = nr::pe::module_folder();
    if (folder.empty()) return false;
    const DWORD a = GetFileAttributesA((folder + "\\reshade-shaders\\Shaders\\lumenite_Kernel.fx").c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

int feed_provider(effect_runtime* rt) {
    char value[16] = {};
    if (rt->get_preprocessor_definition_for_effect(kEffect, "DLSS5_MV_PROVIDER", value) ||
        rt->get_preprocessor_definition("DLSS5_MV_PROVIDER", value))
        return std::atoi(value);
    return 0;
}

// Forget the samples in flight (the copies themselves retire with their frame).
void motion_forget() {
    motion.pending = false;
    motion.last_patches.clear();
}

// The staging pair, with the device idle or going away.
void motion_release(device* dev) {
    if (dev && motion.flow_copy.handle) dev->destroy_resource(motion.flow_copy);
    if (dev && motion.patch_copy.handle) dev->destroy_resource(motion.patch_copy);
    motion.flow_copy = motion.patch_copy = resource{};
    motion.pending = false;
    motion.last_patches.clear();
}

// Set ReShade to `lumenite` or VORT: the provider definition (DLSS5_Feed.fx rebuilds) and the provider techniques.
void motion_apply(effect_runtime* rt, bool lumenite) {
    const effect_technique lum = rs::call(rt, &effect_runtime::find_technique, kLumeniteEffect, kLumeniteTechnique);
    const effect_technique vort = rs::call(rt, &effect_runtime::find_technique, kVortEffect, kVortTechnique);
    const int want = lumenite ? 3 : 2;
    // Coming from LumeniteFX: ours, or the preset's (install.bat (LumeniteFX: y) turns VORT off there).
    const bool from_lumenite = motion.lumenite_active || feed_provider(rt) == 3;
    if (feed_provider(rt) != want) {
        rt->set_preprocessor_definition("DLSS5_MV_PROVIDER", lumenite ? "3" : "2");
        rt->reload_effect_next_frame(kEffect);
        motion.reloading = true;
        // A preset definition: ReShade compiles every effect again (runtime_api.cpp set_preprocessor_definition_for_effect).
        log("[nr] motion vectors: DLSS5_MV_PROVIDER=%d (%s); ReShade compiles its effects again", want,
            lumenite ? "LumeniteFX Kernel" : "VORT");
    }
    if (lum.handle && rt->get_technique_state(lum) != lumenite) rt->set_technique_state(lum, lumenite);
    // VORT is switched on only when coming from LumeniteFX; with VORT chosen it is left as the preset has it.
    if (vort.handle && (lumenite || from_lumenite) && rt->get_technique_state(vort) == lumenite)
        rt->set_technique_state(vort, !lumenite);
    if (motion.lumenite_active != lumenite) {
        motion.lumenite_active = lumenite;
        motion_forget();
        motion.moving = motion.zero_moving = motion.bad_in_row = motion.samples = 0;
        motion.verified = false;
        motion.frames = 0;
        need_reset = true;
    }
    motion.checks_due = lumenite;
}

void motion_fall_back(effect_runtime* rt, const std::string& why) {
    motion.fell_back = true;
    motion.reason = why;
    log("[nr] motion vectors: LumeniteFX is not used this session - %s; VORT instead", why.c_str());
    reshade::log::message(reshade::log::level::warning, ("nr: LumeniteFX not used - " + why).c_str());
    motion_apply(rt, false);
}

// At a present, outside effect rendering: the file's choice, then the checks after a reload.
void motion_sync(effect_runtime* rt) {
    if (!rt) return;
    if (!technique.handle) {   // still compiling, or DLSS5_Feed.fx failed to
        const auto now = std::chrono::steady_clock::now();
        if (motion.missing_since == std::chrono::steady_clock::time_point{}) motion.missing_since = now;
        if (motion.lumenite_active && !motion.fell_back && now - motion.missing_since > std::chrono::seconds(180))
            motion_fall_back(rt, "DLSS5_Feed.fx did not come up with LumeniteFX as its provider in 3 minutes (ReShade.log)");
        return;
    }
    motion.missing_since = {};
    if (!motion.probe_failure.empty()) {
        const std::string why = std::move(motion.probe_failure);
        motion.probe_failure.clear();
        motion_fall_back(rt, why);
        return;
    }
    const bool want = config.motion_vectors == 1 && !motion.fell_back;
    if (motion.sync_due) {
        motion.sync_due = false;
        if (want && !lumenite_on_disk() &&
            !rs::call(rt, &effect_runtime::find_technique, kLumeniteEffect, kLumeniteTechnique).handle) {
            motion_fall_back(rt, "LumeniteFX is not installed (install.bat (LumeniteFX: y) installs it)");
            return;
        }
        const bool set = feed_provider(rt) == 3;   // ReShade is on LumeniteFX (ours or the preset's)
        if (want != motion.lumenite_active || want != set) motion_apply(rt, want);
        else if (want) motion.checks_due = true;
    }
    if (!motion.checks_due || !motion.lumenite_active || motion.reloading) return;
    if (feed_provider(rt) != 3) return;
    motion.checks_due = false;
    const effect_technique lum = rs::call(rt, &effect_runtime::find_technique, kLumeniteEffect, kLumeniteTechnique);
    if (!lum.handle) {
        unsigned others = 0;   // a compiled lumenite_Kernel.fx without the technique of that name
        rt->enumerate_techniques(kLumeniteEffect, [](effect_runtime*, effect_technique, void* user) {
            ++*static_cast<unsigned*>(user);
        }, &others);
        motion_fall_back(rt, others ? "lumenite_Kernel.fx has no technique Lumenite_Kernel (a LumeniteFX version this "
                                      "package does not know?)"
                             : lumenite_on_disk() ? "lumenite_Kernel.fx did not compile (ReShade.log has the error)"
                                                  : "lumenite_Kernel.fx was not found");
        return;
    }
    if (!rt->get_technique_state(lum)) rt->set_technique_state(lum, true);
    if (!rt->get_technique_state(lum)) { motion_fall_back(rt, "the Lumenite_Kernel technique cannot be turned on"); return; }
    // Before DLSS5_Feed in the effect order, or DLSS5_Feed reads the previous frame's flow.
    std::vector<effect_technique> order;
    rt->enumerate_techniques(nullptr, [](effect_runtime*, effect_technique t, void* user) {
        static_cast<std::vector<effect_technique>*>(user)->push_back(t);
    }, &order);
    const auto at = [&](effect_technique t) {
        return std::find_if(order.begin(), order.end(), [&](effect_technique o) { return o.handle == t.handle; });
    };
    if (at(lum) != order.end() && at(technique) != order.end() && at(lum) > at(technique)) {
        order.erase(at(lum));
        order.insert(at(technique), lum);
        rt->reorder_techniques(order.size(), order.data());
        log("[nr] motion vectors: Lumenite_Kernel moved before DLSS5_Feed in the effect order");
    }
    const effect_texture_variable mine = rs::call(rt, &effect_runtime::find_texture_variable, kLumeniteEffect, kLumeniteFlow);
    const effect_texture_variable feeds = rs::call(rt, &effect_runtime::find_texture_variable, kEffect, kLumeniteFlow);
    if (!mine.handle || !feeds.handle || mine.handle != feeds.handle) {
        motion_fall_back(rt, std::string("LumeniteFX's flow texture Kernel::tFlow is ") +
                                 (!mine.handle ? "not in lumenite_Kernel.fx" : "not the one DLSS5_Feed.fx reads") +
                                 " (a LumeniteFX version this package does not know?)");
        return;
    }
    log("[nr] motion vectors: LumeniteFX Kernel - compiled, on, before DLSS5_Feed, Kernel::tFlow shared");
}

void motion_choice_changed() {
    motion.sync_due = true;
    motion.fell_back = false;
    motion.reason.clear();
    motion.probe_failure.clear();
}

void motion_after_reload() {
    motion_forget();
    motion.reloading = false;
    motion.checks_due = motion.lumenite_active;
}

// One flow sample: false while undecided, and a reason when LumeniteFX's vectors fail.
std::string motion_read(device* dev) {
    motion.pending = false;
    std::vector<uint8_t> patches;
    {
        subresource_data m{};
        if (!dev->map_texture_region(motion.patch_copy, 0, nullptr, map_access::read_only, &m)) {
            log("[nr] motion vectors: the data check's copy could not be read; no data check");
            motion.verified = true;
            return {};
        }
        const unsigned bpp = dump_bpp(motion.patch_format);
        patches.resize(size_t(4 * 32) * 32 * bpp);
        for (unsigned y = 0; y < 32; ++y)
            std::memcpy(patches.data() + size_t(y) * 4 * 32 * bpp, static_cast<const uint8_t*>(m.data) + size_t(y) * m.row_pitch,
                        size_t(4 * 32) * bpp);
        dev->unmap_texture_region(motion.patch_copy, 0);
    }
    const bool first = motion.last_patches.empty();
    const bool moving = !first && patches != motion.last_patches;
    motion.last_patches.swap(patches);
    subresource_data m{};
    if (!dev->map_texture_region(motion.flow_copy, 0, nullptr, map_access::read_only, &m)) {
        log("[nr] motion vectors: the data check's copy could not be read; no data check");
        motion.verified = true;
        return {};
    }
    const unsigned w = motion.flow_desc.texture.width, h = motion.flow_desc.texture.height;
    uint64_t nonzero = 0, bad = 0;
    for (unsigned y = 0; y < h; ++y) {
        const uint8_t* line = static_cast<const uint8_t*>(m.data) + size_t(y) * m.row_pitch;
        for (unsigned x = 0; x < w; ++x) {
            uint16_t v[2]; std::memcpy(v, line + size_t(x) * 4, 4);
            if ((v[0] & 0x7FFFu) || (v[1] & 0x7FFFu)) ++nonzero;
            const float a = dump_half(v[0]), b = dump_half(v[1]);
            // Delta UV (DLSS5_Feed.fx ProviderMV): beyond 1.5 screens, or NaN/Inf, is no vector.
            if (((v[0] >> 10) & 0x1Fu) == 31 || ((v[1] >> 10) & 0x1Fu) == 31 || std::fabs(a) > 1.5f || std::fabs(b) > 1.5f) ++bad;
        }
    }
    dev->unmap_texture_region(motion.flow_copy, 0);
    const uint64_t texels = uint64_t(w) * h;
    if (++motion.samples <= 3)
        log("[nr] motion vectors: data check sample %u: %llu of %llu vectors non-zero, %llu unusable, picture %s",
            motion.samples, (unsigned long long)nonzero, (unsigned long long)texels, (unsigned long long)bad,
            first ? "not compared yet" : moving ? "moving" : "still");
    if (texels && bad * 100 > texels) {
        if (++motion.bad_in_row >= 3)
            return "LumeniteFX's flow is NaN or beyond the screen on more than 1% of the frame (three samples running)";
    } else {
        motion.bad_in_row = 0;
    }
    if (moving && !motion.verified) {
        ++motion.moving;
        if (nonzero) {
            motion.verified = true;
            log("[nr] motion vectors: LumeniteFX's flow moves with the picture (%llu of %llu vectors non-zero)",
                (unsigned long long)nonzero, (unsigned long long)texels);
        } else if (++motion.zero_moving >= 10) {
            return "LumeniteFX's flow stayed all zero on 10 sampled frames while the picture moved";
        }
    }
    return {};
}

// Why a data check was skipped, once per cause.
void motion_skip(const char* why) {
    static std::string last;
    if (last != why) { last = why; log("[nr] motion vectors: data check skipped - %s", why); }
}

// In DLSS5_Feed's technique, before the pass reads the vectors: queue the copies, read the previous ones.
void motion_probe(effect_runtime* rt, command_list* cl, resource_view rtv) {
    if (!motion.lumenite_active || motion.checks_due || motion.fell_back) return;
    // NR_DEBUG_MOTION_EVERY=<n> (a test aid): sample every n-th frame until the vectors are verified, not every 60th.
    static const uint64_t first_every = [] {
        char v[16] = {};
        const uint64_t n = GetEnvironmentVariableA("NR_DEBUG_MOTION_EVERY", v, sizeof v) > 0 ? std::strtoull(v, nullptr, 10) : 0;
        return n >= 2 ? n : uint64_t(60);
    }();
    const uint64_t every = motion.verified ? 600 : first_every;
    if (++motion.frames % every != 0) return;
    device* dev = rt->get_device();
    if (motion.pending) {
        const std::string why = motion_read(dev);
        if (!why.empty()) { motion.probe_failure = why; motion.fell_back = true; return; }
    }
    const effect_texture_variable var = rs::call(rt, &effect_runtime::find_texture_variable, kEffect, kLumeniteFlow);
    resource_view srv{}, srgb{};
    if (var.handle) rt->get_texture_binding(var, &srv, &srgb);
    const resource flow = srv.handle ? rs::call(dev, &device::get_resource_from_view, srv) : resource{};
    const resource colour = rs::call(dev, &device::get_resource_from_view, rtv);
    if (!flow.handle || !colour.handle) { motion_skip(!flow.handle ? "Kernel::tFlow has no resource" : "no back buffer"); return; }
    const resource_desc fd = rs::call(dev, &device::get_resource_desc, flow);
    const resource_desc cd = rs::call(dev, &device::get_resource_desc, colour);
    if (fd.texture.format != format::r16g16_float) {   // what DLSS5_Feed.fx declares Kernel::tFlow as
        log("[nr] motion vectors: Kernel::tFlow is format %u, not RG16F; no data check", unsigned(fd.texture.format));
        motion.verified = true;
        motion.frames = 0;
        return;
    }
    if (!dump_bpp(cd.texture.format) || cd.texture.width < 64 || cd.texture.height < 64 || cd.texture.samples > 1) {
        char why[96];
        std::snprintf(why, sizeof why, "back buffer format %u, %ux%u, %u samples", unsigned(cd.texture.format),
                      cd.texture.width, cd.texture.height, unsigned(cd.texture.samples));
        motion_skip(why);
        return;
    }
    if (!motion.flow_copy.handle || motion.flow_desc.texture.width != fd.texture.width ||
        motion.flow_desc.texture.height != fd.texture.height || motion.flow_desc.texture.format != fd.texture.format ||
        motion.patch_format != cd.texture.format) {
        if (motion.flow_copy.handle) rt->get_command_queue()->wait_idle();
        motion_release(dev);
        const resource_desc fs(fd.texture.width, fd.texture.height, 1, 1, fd.texture.format, 1, memory_heap::gpu_to_cpu,
                               resource_usage::copy_dest);
        const resource_desc ps(4 * 32, 32, 1, 1, cd.texture.format, 1, memory_heap::gpu_to_cpu, resource_usage::copy_dest);
        if (!dev->create_resource(fs, nullptr, resource_usage::copy_dest, &motion.flow_copy) ||
            !dev->create_resource(ps, nullptr, resource_usage::copy_dest, &motion.patch_copy)) {
            log("[nr] motion vectors: the check's staging textures could not be created; no data check");
            motion_release(dev);
            motion.verified = true;   // do not try every frame
            return;
        }
        motion.flow_desc = fd;
        motion.patch_format = cd.texture.format;
    }
    const resource_usage sr = resource_usage::shader_resource, rtu = resource_usage::render_target,
                         cs = resource_usage::copy_source;
    cl->barrier(1, &flow, &sr, &cs);
    cl->copy_texture_region(flow, 0, nullptr, motion.flow_copy, 0, nullptr);
    cl->barrier(1, &flow, &cs, &sr);
    cl->barrier(1, &colour, &rtu, &cs);
    const unsigned w = cd.texture.width, h = cd.texture.height;
    const unsigned px[4] = {w / 4, 3 * w / 4, w / 4, 3 * w / 4}, py[4] = {h / 4, h / 4, 3 * h / 4, 3 * h / 4};
    for (unsigned i = 0; i < 4; ++i) {
        const subresource_box src{px[i] - 16, py[i] - 16, 0, px[i] + 16, py[i] + 16, 1};
        const subresource_box dst{i * 32, 0, 0, i * 32 + 32, 32, 1};
        cl->copy_texture_region(colour, 0, &src, motion.patch_copy, 0, &dst);
    }
    cl->barrier(1, &colour, &cs, &rtu);
    motion.pending = true;
}

void on_init_effect_runtime(effect_runtime* rt) {
    // Before any call into ReShade: if the log ends on this line, the fault is
    // in the first virtual call below, not in anything of ours.
    log("[nr] effect runtime %p initialising", static_cast<void*>(rt));
    std::lock_guard<std::mutex> guard(lock);

    if (bound && rt != bound) {
        log("[nr] a second effect runtime %p appeared; %p stays bound", static_cast<void*>(rt),
            static_cast<void*>(bound));
        return;
    }
    bound = rt;
    resolve(rt);
    need_reset = true;
}

void on_reloaded_effects(effect_runtime* rt) {
    std::lock_guard<std::mutex> guard(lock);
    if (bound && rt != bound) return;
    bound = rt;
    resolve(rt);
    // A reload recompiles the provider, which writes zero vectors until its
    // own history refills; do not smear history built on those forward.
    need_reset = true;
}

void on_destroy_effect_runtime(effect_runtime* rt) {
    std::lock_guard<std::mutex> guard(lock);
    if (rt != bound) return;
    bound = nullptr;
    technique = {}; mv_var = {}; depth_var = {};
}

// The Vulkan self-test: with no display to present on (a headless box), the
// effect runtime never renders a technique, so NR_RESHADE_SELFTEST=1 runs the
// whole Vulkan path once on a scratch image at device creation and logs the
// result. The command buffer and submit are ours; the submit goes through
// vulkan-1.dll's export, i.e. through ReShade's vkQueueSubmit hook and its
// queue lock.
void vulkan_selftest(VkDevice rs_device) {
    char flag[8] = {};
    if (!GetEnvironmentVariableA("NR_RESHADE_SELFTEST", flag, sizeof flag) || flag[0] != '1') return;
    std::lock_guard<std::mutex> guard(lock);
    log("[nr] selftest: begin");
    if (!vk_loaded) { nr_vk_load(); vk_loaded = true; }
    auto h = nr::pe::vkdevice::handles();
    h.device = rs_device;
    log("[nr] selftest: handles instance %p physical %p device %p", static_cast<void*>(h.instance),
        static_cast<void*>(h.physical), static_cast<void*>(h.device));
    {
        log("[nr] selftest: thunks vkGetDeviceQueue %p vkGetPhysicalDeviceQueueFamilyProperties %p vkCmdClearColorImage %p",
            nr_vk_p_vkGetDeviceQueue, nr_vk_p_vkGetPhysicalDeviceQueueFamilyProperties, nr_vk_p_vkCmdClearColorImage);
    }
    VkQueue queue{}; uint32_t family{};
    if (!h.valid() || !nr::pe::vkdevice::queue_for(rs_device, &queue, &family)) {
        log("[nr] selftest: FAILED - handles or queue missing");
        return;
    }
    log("[nr] selftest: queue %p family %u", static_cast<void*>(queue), family);
    auto& s = the_session();
    s.set_vulkan_queue(queue, family);
    const uint32_t w = 640, hh = 480;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D; ci.format = VK_FORMAT_B8G8R8A8_UNORM; ci.extent = {w, hh, 1};
    ci.mipLevels = ci.arrayLayers = 1; ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    VkImage image{}; VkDeviceMemory memory{};
    if (vkCreateImage(h.device, &ci, nullptr, &image) != VK_SUCCESS) { log("[nr] selftest: FAILED - image"); return; }
    VkMemoryRequirements req{}; vkGetImageMemoryRequirements(h.device, image, &req);
    VkPhysicalDeviceMemoryProperties mem{}; vkGetPhysicalDeviceMemoryProperties(h.physical, &mem);
    uint32_t type = mem.memoryTypeCount;
    for (uint32_t i = 0; i < mem.memoryTypeCount; ++i)
        if ((req.memoryTypeBits & (1u << i)) && (mem.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) { type = i; break; }
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = req.size; ai.memoryTypeIndex = type;
    if (type == mem.memoryTypeCount || vkAllocateMemory(h.device, &ai, nullptr, &memory) != VK_SUCCESS ||
        vkBindImageMemory(h.device, image, memory, 0) != VK_SUCCESS) { log("[nr] selftest: FAILED - memory"); return; }
    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pi.queueFamilyIndex = family;
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VkCommandPool pool{}; vkCreateCommandPool(h.device, &pi, nullptr, &pool);
    VkCommandBufferAllocateInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; bi.commandPool = pool;
    bi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; bi.commandBufferCount = 1;
    VkCommandBuffer cmd{}; vkAllocateCommandBuffers(h.device, &bi, &cmd);
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence fence{}; vkCreateFence(h.device, &fi, nullptr, &fence);
    nr::Controls controls = config.controls; controls.enabled = true;
    // Frame 1 starts the background build; keep submitting until it is ready.
    VkImage out = VK_NULL_HANDLE;
    for (int attempt = 0; attempt < 600 && !out; ++attempt) {
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        vkResetCommandBuffer(cmd, 0); vkBeginCommandBuffer(cmd, &begin);
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = attempt ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.image = image;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.srcAccessMask = 0; b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        VkClearColorValue clear{}; clear.float32[0] = 0.5f; clear.float32[1] = 0.4f; clear.float32[2] = 0.3f; clear.float32[3] = 1.0f;
        vkCmdClearColorImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &b.subresourceRange);
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        nr::pe::Session::VulkanFrame f{};
        f.colour = image; f.colour_format = VK_FORMAT_B8G8R8A8_UNORM; f.width = w; f.height = hh;
        f.colour_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        out = s.run_vulkan(h, cmd, f, controls);
        vkEndCommandBuffer(cmd);
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
        vkResetFences(h.device, 1, &fence);
        const VkResult r = vkQueueSubmit(queue, 1, &si, fence);
        vkWaitForFences(h.device, 1, &fence, VK_TRUE, ~0ull);
        if (r != VK_SUCCESS) { log("[nr] selftest: FAILED - submit %d", int(r)); break; }
        if (!out) { if (attempt % 50 == 0) log("[nr] selftest: waiting (%s)", s.status().c_str()); Sleep(100); }
    }
    log("[nr] selftest: %s (%s)", out ? "OK - the network ran on the game's Vulkan device" : "FAILED", s.status().c_str());
    s.wait_idle();
    vkDestroyFence(h.device, fence, nullptr); vkDestroyCommandPool(h.device, pool, nullptr);
    vkDestroyImage(h.device, image, nullptr); vkFreeMemory(h.device, memory, nullptr);
}

// The bridge's own Vulkan device (nr_pe_bridge.hpp) passes through ReShade too when ReShade is injected
// as dxgi.dll: it is not a game's device and none of these events are about the game.
bool own_device(device* dev) {
    if (dev->get_api() != device_api::vulkan) return false;
    if (nr::pe::bridge::creating_device()) return true;
    return nr::pe::bridge::is_own_device(reinterpret_cast<VkDevice>(static_cast<uintptr_t>(dev->get_native())));
}

void on_init_device(device* dev) {
    if (own_device(dev)) return;
    // On Vulkan this fires from inside the game's vkCreateDevice, after
    // ReShade has wrapped the device's queues (its vkQueueSubmit hook needs
    // that) and before the game has asked for any: the self-test asks itself.
    log("[nr] device %p initialised, api 0x%x", static_cast<void*>(dev), unsigned(dev->get_api()));
    const device_api api = dev->get_api();
    if (api == device_api::d3d10 || api == device_api::d3d11 || api == device_api::d3d12) hook_d3dcompile();
}

// After the game's vkCreateDevice has returned: the loader has set its
// dispatch on the handle, so device-level entry points work on it. Inside
// the call (where ReShade's init_device fires) they do not - the first word
// of the handle is still the ICD's marker, and vkGetDeviceQueue jumps into it.
void on_vulkan_device_created(VkDevice device) {
    if (nr::pe::bridge::creating_device() || nr::pe::bridge::is_own_device(device)) return;
    vulkan_selftest(device);
}

void on_destroy_device(device* dev) {
    if (own_device(dev)) return;
    // Everything the session made lives on this device's VkDevice.
    std::lock_guard<std::mutex> guard(lock);
    motion_release(dev);
    if (session) {
        log("[nr] the game's device is being destroyed; closing the session");
        session->wait_idle();
        session.reset();
    }
    (void)dev;
}

bool depth_is_reversed(effect_runtime* rt) {
    // ReShade's own convention for the depth buffer it detected, as the effect
    // sees it. Modern engines are reversed-Z; ReShade's default definition is 1.
    char value[16] = {};
    if (rt->get_preprocessor_definition_for_effect(kEffect, "RESHADE_DEPTH_INPUT_IS_REVERSED", value) ||
        rt->get_preprocessor_definition("RESHADE_DEPTH_INPUT_IS_REVERSED", value))
        return std::atoi(value) != 0;
    return true;
}

void feed(effect_runtime* rt, command_list* cl, resource_view rtv) {
    device* dev = rt->get_device();
    resource_view mv_srv{}, mv_srgb{}, d_srv{}, d_srgb{};
    if (mv_var.handle) rt->get_texture_binding(mv_var, &mv_srv, &mv_srgb);
    if (depth_var.handle) rt->get_texture_binding(depth_var, &d_srv, &d_srgb);
    const resource colour = rs::call(dev, &device::get_resource_from_view, rtv);
    resource motion = mv_srv.handle ? rs::call(dev, &device::get_resource_from_view, mv_srv) : resource{};
    const resource depth = d_srv.handle ? rs::call(dev, &device::get_resource_from_view, d_srv) : resource{};
    if (!colour.handle) return;
    if (!motion.handle && !missing_reported) {
        missing_reported = true;
        report("DLSS5_Feed.fx rendered but its DLSS5_MV texture is not bound; the pass runs "
               "without motion (no history) until it is");
    }
    static bool traced = false;
    if (!traced) log("[nr] feed: colour %llx motion %llx depth %llx", (unsigned long long)colour.handle,
                     (unsigned long long)motion.handle, (unsigned long long)depth.handle);
    const resource_desc cd = rs::call(dev, &device::get_resource_desc, colour);
    const unsigned w = cd.texture.width, h_ = cd.texture.height;
    if (!traced) { traced = true; log("[nr] feed: back buffer %ux%u format %u, api %x", w, h_, unsigned(cd.texture.format), unsigned(dev->get_api())); }
    if (motion.handle) {
        const resource_desc md = rs::call(dev, &device::get_resource_desc, motion);
        if (md.texture.width != w || md.texture.height != h_) {
            report("DLSS5_MV is not the back buffer's size; its vectors are read across the whole texture, "
                   "in back-buffer pixels");
        }
    }
    // DLSS5_MV is in pixels at back-buffer size, prev = uv + mv: the consumer's
    // normalized units are one over the extent, and the sign is already ours.
    const float sx = w ? 1.0f / float(w) : 1.0f, sy = h_ ? 1.0f / float(h_) : 1.0f;
    const bool inverted = depth_is_reversed(rt);
    if (have_run && presents > presents_at_run + kResumeGap) {
        if (!need_reset)
            log("[nr] NR runs again after %llu frame(s) without it: history restarts",
                (unsigned long long)(presents - presents_at_run - 1));
        need_reset = true;
    }
    // Consumed only by a frame that ran: a reload that comes before a frame the pass turns away still
    // resets the first frame that does run.
    const bool reset = need_reset;
    {   // the engine-data panel
        auto& pf = panel_facts;
        pf.reshade = true;
        pf.renodx = renodx_on;
        pf.format = nr::pe::panel::dxgi_format_name(uint32_t(cd.texture.format));
        pf.hdr = back_buffer_space == color_space::srgb_nonlinear ? 0
                 : back_buffer_space == color_space::extended_srgb_linear ? 3
                 : back_buffer_space == color_space::hdr10_st2084 ? 2 : -1;
        pf.hdr_why = "the swap chain's colour space";
        if (renodx_on && config.renodx.values.present_encoding != 0) pf.hdr_why = "NRPresentEncoding";
        pf.motion = motion.handle ? 1 : 0;
        pf.depth = depth.handle ? 1 : 0;
        pf.preprocess_exposure = config.controls.preprocess.active() ? config.controls.preprocess.exposure : 0;
        pf.white_point = renodx_on ? 0.f : config.white_point;
    }
    if (!vk_loaded) { nr_vk_load(); vk_loaded = true; log("[nr] feed: Vulkan entry points resolved"); }
    // An HDR10 back buffer is PQ (or HLG) over BT.2020, and nothing in this
    // pass decodes either. Running on it anyway would hand the model a picture
    // in a transfer function it has never seen and hand back a worse one, which
    // is a failure worth naming rather than a picture worth shipping.
    if (!renodx_on && (back_buffer_space == color_space::hdr10_st2084 || back_buffer_space == color_space::hdr10_hlg)) {
        report("the swapchain is HDR10 (PQ/HLG); this pass has no decode for it yet, so the frame is "
               "left alone - switch the game to SDR or scRGB HDR");
        return;
    }
    // scRGB: the back buffer is linear light with 1.0 at SDR white, so the
    // runtime has to encode it for the model exactly as it does an upscaler's
    // input. Without this the model is shown linear values as if they were
    // display-referred, which is what once made the upscaler path dark and
    // blocky - and on this route nothing downstream would bound it.
    const bool linear_hdr = back_buffer_space == color_space::extended_srgb_linear;
    const nr::Controls& controls = config.controls;
    bool ran = false;
    auto& s = the_session();
    // [RenoDX] Enabled at start: RenoDX's processing around the network (nr_renodx.hpp), its other keys as the file has
    // them now.
    nr::pe::Session::RenoDXFrame renodx_frame;
    renodx_frame.hdr_flag = linear_hdr;
    renodx_frame.after_upscale = true;
    renodx_frame.settings = config.renodx.values;
    {   // NeuralUplift as NRToggleKey left it
        static nr::pe::NrToggle toggle;
        renodx_frame.settings.nr_enabled = toggle.frame(config.renodx);
    }
    if (renodx_on) {   // RenoDX's NR screenshot (nr_screenshot.hpp): this route's final picture is the presented frame
        static nr::pe::KeyPress screenshot_key;
        const auto& r = config.renodx;
        nr::pe::screenshot::configure({r.screenshot_source, r.screenshot_format, r.screenshot_layout,
                                       r.screenshot_jpeg_quality, r.screenshot_max_mb});
        if (screenshot_key.frame(r.screenshot_key)) nr::pe::screenshot::request();
        nr::pe::screenshot::tick();
    }
    renodx_frame.preprocess = controls.preprocess;
    const nr::pe::Session::RenoDXFrame* renodx = renodx_on ? &renodx_frame : nullptr;
    if (renodx) {
        renodx_frame.present = present_encoding(renodx_frame.settings, static_cast<DXGI_FORMAT>(cd.texture.format));
        if (!renodx_frame.present) {
            report("the RenoDX-style option cannot serve this swap chain (colour space " +
                   std::string(space_name(back_buffer_space)) + ", format " + std::to_string(unsigned(cd.texture.format)) +
                   "); the frame is left alone");
            return;
        }
        if (renodx_frame.present != 1) renodx_frame.present_white_nits = present_white(rt, renodx_frame.settings);
    }
    // NRPresentGuides: Required needs DLSS5_Feed's motion vectors; Optional runs on neutral ones without them, Never
    // always (Session::Impl::neutral_motion).
    if (renodx && !motion.handle && renodx_frame.settings.present_guides == 1u) {
        report("the RenoDX-style option needs DLSS5_Feed's motion vectors (NRPresentGuides = 1); the frame is left alone "
               "until they are bound");
        return;
    }

    // The sampled pair: evaluates 1, 2, 3 and then every 300th, as the model's
    // readback does. Drain first, because the staging pair is reused and the
    // copy below would overwrite what is about to be read.
    // NR_DEBUG_DUMP_RUN=<n> (a measurement aid): also every 4th evaluation of the 32 from the n-th, for how steady a
    // still picture stays.
    static const uint64_t run_from = [] {
        char v[16] = {};
        return GetEnvironmentVariableA("NR_DEBUG_DUMP_RUN", v, sizeof v) > 0 ? std::strtoull(v, nullptr, 10) : 0ull;
    }();
    const bool sample = debug_dump_on() &&
                        (++dump_state.seen <= 3 || dump_state.seen % 300 == 0 ||
                         (run_from && dump_state.seen >= run_from && dump_state.seen < run_from + 32 &&
                          (dump_state.seen - run_from) % 4 == 0));
    bool sampling = false;
    if (debug_dump_on()) {
        dump_drain(dev);
        if (sample && dump_ensure(dev, w, h_, cd.texture.format)) {
            const resource_usage rt_usage = resource_usage::render_target, cs = resource_usage::copy_source;
            cl->barrier(1, &colour, &rt_usage, &cs);
            cl->copy_texture_region(colour, 0, nullptr, dump_state.before, 0, nullptr);
            cl->barrier(1, &colour, &cs, &rt_usage);
            sampling = true;
        }
    }
    // DXVK builds D3D10 on its D3D11: an ID3D10Device or ID3D10Texture2D answers QueryInterface with the
    // D3D11 object underneath (src/d3d10/d3d10_device.cpp, d3d10_texture.cpp), and the D3D11 path asks for
    // every interface it uses that way. So a D3D10 game on DXVK takes the D3D11 path with ReShade's D3D10
    // natives; the native D3D10 runtime (whose device has no D3D11 underneath) takes the bridge below.
    device_api api = dev->get_api();
    if (api == device_api::d3d10) {
        IUnknown* d11 = nullptr;
        if (SUCCEEDED(reinterpret_cast<IUnknown*>(dev->get_native())->QueryInterface(__uuidof(ID3D11Device),
                                                                                      reinterpret_cast<void**>(&d11))) &&
            d11) {
            d11->Release();
            api = device_api::d3d11;
        }
    }
    switch (api) {
    case device_api::d3d11: {
        nr::pe::Session::D3D11Frame f{};
        f.device = reinterpret_cast<IUnknown*>(dev->get_native());
        f.target = reinterpret_cast<IUnknown*>(colour.handle);
        f.motion = reinterpret_cast<IUnknown*>(motion.handle);
        f.depth = reinterpret_cast<IUnknown*>(depth.handle);
        f.depth_inverted = inverted;
        f.motion_scale_x = sx; f.motion_scale_y = sy;
        f.reset = reset;
        f.linear_hdr = linear_hdr;
        f.renodx = renodx;
        ran = s.run_d3d11(f, controls);
        break;
    }
    case device_api::d3d10: {
        // The native D3D10 runtime (ReShade as dxgi.dll): the bridge with keyed mutexes.
        nr::pe::Session::D3D11Frame f{};
        f.device = reinterpret_cast<IUnknown*>(dev->get_native());
        f.target = reinterpret_cast<IUnknown*>(colour.handle);
        f.motion = reinterpret_cast<IUnknown*>(motion.handle);
        f.depth = reinterpret_cast<IUnknown*>(depth.handle);
        f.depth_inverted = inverted;
        f.motion_scale_x = sx; f.motion_scale_y = sy;
        f.reset = reset;
        f.linear_hdr = linear_hdr;
        f.renodx = renodx;
        ran = s.run_d3d10(f, controls);
        break;
    }
    case device_api::d3d12: {
        auto* device = reinterpret_cast<ID3D12Device*>(dev->get_native());
        if (s.bridged(device)) {
            // The native runtime: ReShade's effect list is its own immediate list, so the frame is
            // handed over at queue level - flush it, copy in, our queue, copy out - and nothing is
            // recorded into a list ReShade keeps state for.
            command_queue* queue = rt->get_command_queue();
            nr::pe::Session::D3D12QueueFrame f{};
            f.device = device;
            f.queue = reinterpret_cast<ID3D12CommandQueue*>(queue->get_native());
            f.flush = [queue] { queue->flush_immediate_command_list(); };
            f.target = reinterpret_cast<ID3D12Resource*>(colour.handle);
            f.target_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
            // ReShade's shader_resource usage is both shader-resource states.
            f.motion = reinterpret_cast<ID3D12Resource*>(motion.handle);
            f.motion_state = D3D12_RESOURCE_STATES(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            f.depth = reinterpret_cast<ID3D12Resource*>(depth.handle);
            f.depth_state = D3D12_RESOURCE_STATES(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            f.depth_inverted = inverted;
            f.motion_scale_x = sx; f.motion_scale_y = sy;
            f.reset = reset;
            f.linear_hdr = linear_hdr;
            f.renodx = renodx;
            ran = s.run_d3d12_queue(f, controls);
            break;
        }
        auto* list = reinterpret_cast<ID3D12GraphicsCommandList*>(cl->get_native());
        // ReShade renders its techniques with the back buffer as a render
        // target (DLSS5-Feeder moves it from exactly that state). Move it to
        // UAV through ReShade's own barrier so its tracking agrees with what
        // the pass does, and put it back after.
        const resource_usage from = resource_usage::render_target, to = resource_usage::unordered_access;
        cl->barrier(1, &colour, &from, &to);
        nr::pe::Session::EngineResources r{};
        r.motion = reinterpret_cast<ID3D12Resource*>(motion.handle);
        r.motion_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        r.depth = reinterpret_cast<ID3D12Resource*>(depth.handle);
        r.depth_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        r.depth_inverted = inverted;
        r.motion_scale_x = sx; r.motion_scale_y = sy;
        r.reset = reset;
        // The D3D12 path asks the question the other way round, and its default
        // said "not encoded" - so a float SDR back buffer used to be tone mapped
        // for no reason while D3D11 and Vulkan left an HDR one raw. One answer
        // for all three now.
        r.colour_encoded = !linear_hdr;
        ran = renodx ? s.run_renodx(device, list, reinterpret_cast<ID3D12Resource*>(colour.handle),
                                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS, r, controls,
                                    reinterpret_cast<ID3D12Resource*>(colour.handle),
                                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS, *renodx)
                     : s.run_after(device, list, reinterpret_cast<ID3D12Resource*>(colour.handle),
                                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS, r, controls);
        cl->barrier(1, &colour, &to, &from);
        break;
    }
    case device_api::vulkan: {
        // ReShade as a Vulkan layer: its natives are the raw Vulkan handles.
        // The device/instance/queue come from watching vulkan-1.dll (the
        // add-on loads inside ReShade's vkCreateInstance, before the game's
        // vkCreateDevice), and the command list is the one ReShade records
        // its techniques into, so the pass lands in order with them.
        auto h = nr::pe::vkdevice::handles();
        const auto rs_device = reinterpret_cast<VkDevice>(static_cast<uintptr_t>(dev->get_native()));
        h.device = rs_device;   // ReShade's word on the device; the watcher supplies the rest
        if (!h.valid()) {
            report("the game's Vulkan instance/physical device were not seen (the add-on has to be loaded by "
                   "ReShade's Vulkan layer before the game creates its device)");
            return;
        }
        VkQueue queue{}; uint32_t family{};
        if (!nr::pe::vkdevice::queue_for(rs_device, &queue, &family)) {
            report("no graphics+compute queue of the game's was seen; nothing to upload the weights on");
            return;
        }
        s.set_vulkan_queue(queue, family);
        const auto cmd = reinterpret_cast<VkCommandBuffer>(static_cast<uintptr_t>(cl->get_native()));
        nr::pe::Session::VulkanFrame f{};
        // VkImage is a 64-bit non-dispatchable handle on both bitnesses; the
        // C-style cast is the one spelling valid for a pointer (x64) and an
        // integer (x86) alike.
        f.colour = (VkImage)colour.handle;
        f.colour_format = nr::pe::vulkan_colour_format_of(static_cast<DXGI_FORMAT>(cd.texture.format));
        f.width = w; f.height = h_;
        // ReShade's word on the image's usage, in Vulkan's terms (the runtime reads and writes a
        // sampled + storage back buffer in place).
        const auto vk_usage = [](resource_usage u) {
            VkImageUsageFlags v = 0;
            if ((u & resource_usage::shader_resource) != 0) v |= VK_IMAGE_USAGE_SAMPLED_BIT;
            if ((u & resource_usage::unordered_access) != 0) v |= VK_IMAGE_USAGE_STORAGE_BIT;
            if ((u & resource_usage::copy_source) != 0) v |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            if ((u & resource_usage::copy_dest) != 0) v |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            return v;
        };
        f.colour_usage = vk_usage(cd.usage);
        if (motion.handle) {
            const resource_desc md = rs::call(dev, &device::get_resource_desc, motion);
            f.motion = (VkImage)motion.handle;
            f.motion_format = nr::pe::vulkan_format_of(static_cast<DXGI_FORMAT>(md.texture.format));
            f.motion_width = md.texture.width; f.motion_height = md.texture.height;
            f.motion_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            f.motion_usage = vk_usage(md.usage);
        }
        if (depth.handle) {
            const resource_desc dd = rs::call(dev, &device::get_resource_desc, depth);
            f.depth = (VkImage)depth.handle;
            f.depth_format = nr::pe::vulkan_format_of(static_cast<DXGI_FORMAT>(dd.texture.format));
            f.depth_width = dd.texture.width; f.depth_height = dd.texture.height;
            f.depth_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            f.depth_usage = vk_usage(dd.usage);
        }
        f.depth_inverted = inverted;
        f.motion_scale_x = sx; f.motion_scale_y = sy;
        f.reset = reset;
        f.linear_hdr = linear_hdr;
        if (f.colour_format == VK_FORMAT_UNDEFINED) {
            report("the back buffer's format is not one this pass accepts");
            return;
        }
        // ReShade renders techniques with the back buffer as a render target;
        // move it through ReShade's own barriers so its layout tracking and
        // ours agree: copy source for the pass's read, render target again
        // after. The runtime copies the frame out and the answer back itself
        // and leaves the image as it found it (VulkanFrame::in_place).
        const resource_usage rt = resource_usage::render_target, src = resource_usage::copy_source,
                             dst = resource_usage::copy_dest;
        cl->barrier(1, &colour, &rt, &src);
        f.colour_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        f.in_place = true;
        f.renodx = renodx;
        const VkImage out = s.run_vulkan(h, cmd, f, controls);
        if (out == f.colour) {
            cl->barrier(1, &colour, &src, &rt);
            ran = true;
        } else if (out) {
            cl->barrier(1, &colour, &src, &dst);
            VkImageCopy region{};
            region.srcSubresource = region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.extent = {w, h_, 1};
            vkCmdCopyImage(cmd, out, VK_IMAGE_LAYOUT_GENERAL, f.colour, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            cl->barrier(1, &colour, &dst, &rt);
            ran = true;
        } else {
            cl->barrier(1, &colour, &src, &rt);
        }
        break;
    }
    default:
        report("this graphics API is not handled by the ReShade add-on (D3D10, D3D11, D3D12 and Vulkan are)");
        return;
    }
    if (sampling) {
        const resource_usage rt_usage = resource_usage::render_target, cs = resource_usage::copy_source;
        cl->barrier(1, &colour, &rt_usage, &cs);
        cl->copy_texture_region(colour, 0, nullptr, dump_state.after, 0, nullptr);
        cl->barrier(1, &colour, &cs, &rt_usage);
        dump_state.pending = true;
    }
    report(s.status());
    if (ran) { need_reset = false; have_run = true; presents_at_run = presents; }
    if (ran) ++frames_run; else ++frames_declined;
    if ((frames_run + frames_declined) % 240 == 0)
        log("[nr] frames: %u enhanced, %u declined; %.2f ms on the GPU (network %.2f, route %.2f); %s", frames_run,
            frames_declined, s.gpu_ms(), s.network_ms(), std::max(s.gpu_ms() - s.network_ms(), 0.0f),
            s.vram_line().c_str());
}

// The settings panel, in ReShade's own overlay under this add-on's name. The
// controls, labels, ranges and defaults are OptiScaler DLSS-NR's model page
// (its DlssNr_Menu.cpp / DlssNr_MenuModel.cpp); every change is written back to
// dlssnr-amd.ini. History is this project's own.
void help_marker(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", text);
}

// A slider that commits on release, with a Reset button. `inherit` (a later
// pass) resets to "not set", which inherits pass 1.
// [RenoDX]'s values for nr_control_ui.hpp, by its key table's index (nr_pe_config.cpp renodx_field).
struct RenoDXAccess {
    nr::pe::RenoDXConfig& r;
    bool& b(int i) { return *static_cast<bool*>(nr::pe::renodx_field(r, i)); }
    uint32_t& u(int i) { return *static_cast<uint32_t*>(nr::pe::renodx_field(r, i)); }
    float& f(int i) { return *static_cast<float*>(nr::pe::renodx_field(r, i)); }
    std::string& s(int i) { return *static_cast<std::string*>(nr::pe::renodx_field(r, i)); }
};

// A hotkey as text, committed when the field is let go; true then.
bool hotkey_text(const char* label, std::string& value) {
    static std::map<ImGuiID, std::string> editing;
    const ImGuiID id = ImGui::GetID(label);
    auto it = editing.find(id);
    std::string& t = it != editing.end() ? it->second : (editing[id] = value);
    char buf[64];
    std::snprintf(buf, sizeof buf, "%s", t.c_str());
    if (ImGui::InputText(label, buf, sizeof buf)) t = buf;
    if (ImGui::IsItemDeactivatedAfterEdit()) { value = t; editing.erase(id); return true; }
    if (!ImGui::IsItemActive()) editing.erase(id);
    return false;
}

template <class Value>
bool deferred_slider(const char* label, Value& value, float shown, float mn, float mx, float reset_to,
                     bool inherit, const char* help) {
    static std::map<ImGuiID, float> pending;
    const ImGuiID id = ImGui::GetID(label);
    const auto it = pending.find(id);
    float v = it != pending.end() ? it->second : shown;
    bool changed = false;
    if (ImGui::SliderFloat(label, &v, mn, mx, "%.2f")) pending[id] = v;
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        const auto done = pending.find(id);
        if (done != pending.end()) { value = std::clamp(done->second, mn, mx); pending.erase(done); changed = true; }
    }
    ImGui::SameLine();
    if (ImGui::SmallButton((std::string("Reset##") + label).c_str())) {
        if constexpr (std::is_same_v<Value, std::optional<float>>) { if (inherit) value.reset(); else value = reset_to; }
        else value = reset_to;
        pending.erase(id);
        changed = true;
    }
    if (help) help_marker(help);
    return changed;
}

bool model_sliders(float& intensity, float& structure, float& tone, float& skin) {
    bool changed = false;
    changed |= deferred_slider("Intensity", intensity, intensity, 0.0f, 2.0f, 1.0f, false, "Enhancement strength. 1 = default.");
    changed |= deferred_slider("Local structure", structure, structure, 0.0f, 2.0f, 1.0f, false, "Fine detail and local contrast. 1 = default.");
    changed |= deferred_slider("Local tone", tone, tone, 0.0f, 2.0f, 1.0f, false, "Broad lighting changes. Later passes default to 0.");
    changed |= deferred_slider("Skin structure", skin, skin, -1.0f, 2.0f, -1.0f, false, "Skin detail. -1 follows Local structure.");
    return changed;
}

const char* network_name(int n) { return n >= 0 ? nr::network_codename(n) : "?"; }

// The build's progress as a fraction and a phrase (nr::g_build_stage).
float build_fraction(const nr::pe::Session::State& st, char* text, size_t n) { return nr::pe::build_progress(st, text, n); }

// The top of the add-on page: which network runs, what it costs, and a build in progress.
void draw_state() {
    const nr::pe::Session::State st = session ? session->state() : nr::pe::Session::State{};
    char line[256];
    if (st.running) {
        const char* kind = network_name(st.running_net);
        ImGui::Text("Network: %s   model %ux%u", kind, st.model_w, st.model_h);
    } else {
        ImGui::TextUnformatted("Network: none (frames pass through)");
    }
    // The network itself, then this route's own work around it (input, enlargement and composition,
    // copies); together the whole, timed as before.
    const float ms = session ? session->gpu_ms() : 0.0f;
    const float net = session ? session->network_ms() : 0.0f;
    if (ms > 0.0f && net > 0.0f) {
        ImGui::Text("NR cost: %.2f ms/frame", net);
        ImGui::Text("ReShade route: %.2f ms/frame   (total %.2f ms)", std::max(ms - net, 0.0f), ms);
    } else if (ms > 0.0f) {
        ImGui::Text("NR cost: %.2f ms/frame", ms);
    } else {
        ImGui::TextUnformatted("NR cost: --");
    }
    if (st.building) {
        char phase[128];
        const float f = build_fraction(st, phase, sizeof phase);
        std::snprintf(line, sizeof line, "Building %s", st.building_net >= 0 ? network_name(st.building_net) : "the network");
        ImGui::TextUnformatted(line);
        ImGui::ProgressBar(f, ImVec2(-1.0f, 0.0f), phase);
    }
    if (!last_status.empty() && !st.running) ImGui::TextUnformatted(last_status.c_str());
    ImGui::Separator();
}

void draw_overlay(effect_runtime*) {
    std::lock_guard<std::mutex> guard(lock);
    auto& c = config.controls;
    bool changed = false;

    draw_state();

    // With the RenoDX-style option every NR setting is [RenoDX]'s (the same menu as NR Control on the OptiScaler
    // routes, nr_control_ui.hpp); [DlssNr]'s do not apply then.
    if (renodx_on) {
        RenoDXAccess access{config.renodx};
        changed |= nr::control_ui::draw_renodx(access, nr::control_ui::Route::kReShade, true, [] {
            if (ImGui::Button("Take a screenshot now")) nr::pe::screenshot::request();
            ImGui::TextWrapped("Screenshot: %s", nr::pe::screenshot::status());
        });
    } else {
        changed |= ImGui::Checkbox("Enable Neural Rendering", &c.enabled);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Enable NR processing.");
        changed |= ImGui::Checkbox("Apply model", &c.apply_model);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Show or hide the NR effect. The model still runs when hidden.\n"
                              "Disable Enable Neural Rendering to stop its GPU cost.");

        {   // Model resolution: rebuilds the network, so it commits on release.
            static int pending = -1;
            int percent = pending >= 0 ? pending : int(std::lround(config.model_scale * 100.0f));
            if (ImGui::SliderInt("Model resolution", &percent, 25, 100, "%d%%")) pending = percent;
            if (ImGui::IsItemDeactivatedAfterEdit() && pending >= 0) {
                config.model_scale = float(std::clamp(pending, 25, 100)) / 100.0f;
                pending = -1;
                changed = true;
            }
            help_marker("50% halves width and height. 100% uses the full input size.");
            // Below 100%: how the model's result is enlarged to the frame (nr::kEnlarge*). Per frame, nothing rebuilds.
            static const char* const transfers[] = {"Matched residual", "Edge-aware lighting + colour", "Classic"};
            changed |= ImGui::Combo("Enlargement", &c.transfer, transfers, 3);
            help_marker("Below 100% model resolution: how the model's result is enlarged to the frame.\n"
                        "Matched residual (default): the model's change, enlarged, added to the full-size frame.\n"
                        "Edge-aware lighting + colour: the change in light and in colour enlarged separately,\n"
                        "weighted by the full-size frame's edges, then applied to the full-size frame.\n"
                        "Classic: the model's picture enlarged with the scaler below.");
            if (c.transfer == nr::kEnlargeClassic) {
                static const char* const scalers[] = {"Bilinear", "Catmull-Rom", "Lanczos3", "FSR 1"};
                changed |= ImGui::Combo("Classic scaler", &c.classic_scaler, scalers, 4);
            }
        }

        {   // Model passes
            const int limit = config.pass_limit();
            static int passes = 1;
            static bool editing = false;
            if (!editing) passes = std::clamp(c.passes, 1, limit);
            const ImVec4 text = ImGui::GetStyleColorVec4(ImGuiCol_Text);
            const float b = std::max({text.x, text.y, text.z});
            ImGui::PushStyleColor(ImGuiCol_Text, passes == 1 ? ImVec4(b * 0.35f, b * 0.75f, b * 0.45f, text.w)
                                                             : ImVec4(b * 0.80f, b * 0.35f, b * 0.32f, text.w));
            ImGui::SliderInt("##Model passes", &passes, 1, limit, "%d", ImGuiSliderFlags_AlwaysClamp);
            editing = ImGui::IsItemActive();
            if (ImGui::IsItemDeactivatedAfterEdit()) { c.passes = std::clamp(passes, 1, limit); changed = true; }
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::TextUnformatted("Model passes");
            if (ImGui::Checkbox("Unlock up to 10 passes", &config.unlock_passes)) {
                c.passes = std::clamp(c.passes, 1, config.pass_limit());
                changed = true;
            }
        }

        static const char* const styles[] = {"Standard", "Natural", "Cinematic"};
        static const char* const inherited_styles[] = {"Auto", "Standard", "Natural", "Cinematic"};
        if (ImGui::TreeNodeEx("Pass 1", ImGuiTreeNodeFlags_DefaultOpen)) {
            changed |= ImGui::Combo("Style", &c.style, styles, 3);
            changed |= model_sliders(c.intensity, c.local_structure, c.local_tone, c.skin_structure);
            changed |= ImGui::Checkbox("Auto skin mask", &c.automatic_mask);
            help_marker("Model-based skin selection.");
            ImGui::TreePop();
        }
        for (int n = 2; n <= c.passes; ++n) {
            char head[16];
            std::snprintf(head, sizeof head, "Pass %d", n);
            ImGui::PushID(n);
            if (ImGui::TreeNodeEx(head, ImGuiTreeNodeFlags_DefaultOpen)) {
                auto& o = config.pass[size_t(n) - 2];
                int style = o.style ? *o.style + 1 : 0;
                if (ImGui::Combo("Style", &style, inherited_styles, 4)) {
                    if (style == 0) o.style.reset(); else o.style = style - 1;
                    changed = true;
                }
                changed |= deferred_slider("Intensity", o.intensity, o.intensity.value_or(c.intensity), 0.0f, 2.0f, 0, true, "Enhancement strength. 1 = default.");
                changed |= deferred_slider("Local structure", o.local_structure, o.local_structure.value_or(c.local_structure), 0.0f, 2.0f, 0, true, "Fine detail and local contrast. 1 = default.");
                changed |= deferred_slider("Local tone", o.local_tone, o.local_tone.value_or(0.0f), 0.0f, 2.0f, 0, true, "Broad lighting changes. Later passes default to 0.");
                changed |= deferred_slider("Skin structure", o.skin_structure, o.skin_structure.value_or(c.skin_structure), -1.0f, 2.0f, 0, true, "Skin detail. -1 follows Local structure.");
                bool mask = o.automatic_mask.value_or(c.automatic_mask);
                if (ImGui::Checkbox("Auto skin mask", &mask)) { o.automatic_mask = mask; changed = true; }
                ImGui::SameLine();
                if (ImGui::SmallButton("Reset##mask")) { o.automatic_mask.reset(); changed = true; }
                ImGui::TreePop();
            }
            ImGui::PopID();
        }

        if (ImGui::TreeNodeEx("Apply edit", ImGuiTreeNodeFlags_DefaultOpen)) {
            changed |= ImGui::SliderFloat("Detail strength", &c.detail_strength, 0.0f, 2.0f, "%.2f");
            ImGui::SameLine();
            if (ImGui::SmallButton("Reset##detail")) { c.detail_strength = 1.0f; changed = true; }
            help_marker("0 = no detail change. 1 = normal.");
            changed |= ImGui::SliderFloat("Colour strength", &c.colour_strength, 0.0f, 4.0f, "%.2f");
            ImGui::SameLine();
            if (ImGui::SmallButton("Reset##colour")) { c.colour_strength = 1.0f; changed = true; }
            help_marker("0 = game colours. 1 = model colours. Above 1 boosts saturation.");
            changed |= ImGui::SliderFloat("Highlight guard", &c.max_ratio, 1.0f, 8.0f, "%.1fx");
            ImGui::SameLine();
            if (ImGui::SmallButton("Reset##guard")) { c.max_ratio = 2.0f; changed = true; }
            help_marker("Limit pixel brightening and darkening.");
            ImGui::TreePop();
        }
        // This project's own: the post block's previous-frame blend, the linear-light white point, the noise seed.
        changed |= deferred_slider("History (previous frame blend)", config.history, config.history, 0.0f, 1.0f, 1.0f, false, nullptr);
        changed |= deferred_slider("White point", config.white_point, config.white_point, 0.01f, 100.0f, 1.0f, false,
                                   "[DlssNr] WhitePoint. Linear-light (floating-point HDR) frames: the value shown to the\n"
                                   "network as white. 1 = default.");
        changed |= ImGui::Checkbox("New noise every frame", &c.noise_every_frame);
        help_marker("[DlssNr] NoiseEveryFrame. Frames without motion vectors (DLSS5_MV not bound): a new noise seed for\n"
                    "the network every frame, as NVIDIA's DLL does. With no previous result to average it out, a still\n"
                    "picture shimmers slightly. Off (default): the seed stays fixed.");
    }

    // This project's own: [Preprocess]. Off is the upstream behaviour.
    // [Network]: which network runs (the three by the DLL's codenames); one that cannot run in this run is greyed out
    // with the reason. The switch writes Model to the file; the other keys are saved with the rest of it.
    // (not "Network": the RenoDX table has a group of that name, and the same label is the same node)
    if (session && ImGui::TreeNodeEx("Network model and options", ImGuiTreeNodeFlags_DefaultOpen)) {
        const nr::pe::Session::State st = session->state();
        const int shown = st.want_net >= 0 ? st.want_net : config.network.model;
        if (ImGui::BeginCombo("Network##model", network_name(shown))) {
            for (int n = 0; n < nr::kNetworkChoices; ++n) {
                const std::string& why = st.unavailable[n];
                if (ImGui::Selectable(network_name(n), n == shown, why.empty() ? 0 : ImGuiSelectableFlags_Disabled)) {
                    session->set_network(n);
                    config.network.model = n;
                }
                if (!why.empty()) { ImGui::SameLine(); ImGui::TextDisabled("- %s", why.c_str()); }
            }
            ImGui::EndCombo();
        }
        help_marker("CC_SMALL_FP8: 310.9.16's default network, the fastest.\n"
                    "CC_SILVER_AARDWOLD: 310.8.0's network (310.9.16's preset 1).\n"
                    "CC_SILVER_AARDWOLD_INT4_MIXED: that network with part of its arithmetic in int4, faster, a slightly "
                    "different picture.\nSwitching builds the new network in the background; the running one serves until "
                    "it is ready.");
        for (int n = 0; n < nr::kNetworkChoices; ++n)
            if (!st.unavailable[n].empty()) ImGui::TextDisabled("%s: %s", network_name(n), st.unavailable[n].c_str());
        if (hotkey_text("Hotkey##net", config.network.hotkey) && !config_path.empty()) config.save(config_path);
        help_marker("[Network] Hotkey: cycles through the networks that can run.\n"
                    "Ctrl, Shift, Alt and one key, e.g. Ctrl+F11 or Alt+F9; empty: no hotkey.");
        bool sound = config.network.sound;
        if (ImGui::Checkbox("Sound on switch", &sound)) {
            config.network.sound = sound;
            if (!config_path.empty()) config.save(config_path);
        }
        static const char* const residencies[] = {"auto", "resident", "offload", "minimal"};
        int ri = std::clamp(config.network.residency, 0, 3);
        if (ImGui::Combo("Residency", &ri, residencies, 4)) {
            config.network.residency = ri;
            if (!config_path.empty()) config.save(config_path);
        }
        help_marker("What is kept of a network not in use (the networks share one activation memory: mostly weights).\n"
                    "auto (default): weights in video memory, switching back is quick; to RAM when video memory runs\n"
                    "short, freed when RAM does too. resident: kept whole, instant, the most video memory.\n"
                    "offload: weights in RAM, video memory freed. minimal: everything freed, switching back builds anew.\n"
                    "The running network serves until the other is ready. NR off for 10 s: offload and minimal do the\n"
                    "same to the running network, auto only when video memory is short.");
        static const char* const osds[] = {"off", "switch", "always"};
        int oi = std::clamp(config.network.osd, 0, 2);
        if (ImGui::Combo("OSD", &oi, osds, 3)) {
            config.network.osd = oi;
            if (!config_path.empty()) config.save(config_path);
        }
        help_marker("[Network] OSD: the running network in the corner of the picture.\n"
                    "switch (default): while a switch builds, its progress; then the new network for 3 s.\n"
                    "always: all the time. off: never.");
        bool grade = config.network.style_grade;
        if (ImGui::Checkbox("Style grade (Natural / Cinematic)", &grade)) {
            config.network.style_grade = grade;
            if (!config_path.empty()) config.save(config_path);
        }
        help_marker("[Network] Style Grade. On (default): Natural and Cinematic are graded after the network as\n"
                    "NVIDIA's DLL does. Off: the picture these styles had up to 0.0.6, without that grade.\n"
                    "Standard is not affected. Takes effect at once.");
        bool mwd = config.network.motion_without_depth;
        if (ImGui::Checkbox("Motion without depth", &mwd)) {
            config.network.motion_without_depth = mwd;
            if (!config_path.empty()) config.save(config_path);
        }
        help_marker("[Network] Motion Without Depth. Where the network's previous-frame lookup reads its motion vector.\n"
                    "On (default): the pixel's own vector, depth not used, as NVIDIA's DLL does.\n"
                    "Off: with depth (DLSS5_Feed), the vector of the nearest of the pixel and its four diagonal neighbours.\n"
                    "Only moving edges differ. Takes effect after a restart.");
        bool rh = config.network.resize_history;
        if (ImGui::Checkbox("Resize history", &rh)) {
            config.network.resize_history = rh;
            if (!config_path.empty()) config.save(config_path);
        }
        help_marker("[Network] Resize History. When the size of NR's frame changes (on this route: the game's resolution):\n"
                    "Off (default): NR's previous result is dropped and NR starts again at the new size, as NVIDIA's DLL does.\n"
                    "On: the previous result is scaled to the new size and kept.\n"
                    "It matters for dynamic resolution with NR before the upscaler (OptiScaler route); here sizes change rarely.");
        bool aco = config.network.aco;
        if (ImGui::Checkbox("ACO Mode", &aco)) {
            config.network.aco = aco;
            if (!config_path.empty()) config.save(config_path);
        }
        help_marker("[Network] ACO Mode. On (default): the network's machine code made in advance by the ACO compiler,\n"
                    "loaded by the AMD driver; when it cannot be used, the driver's compiler is used and dlssnr-amd.log\n"
                    "says why. Off: the AMD driver's own compiler. CC_SILVER_AARDWOLD_INT4_MIXED needs it on.\n"
                    "Takes effect after a restart.");
        ImGui::TreePop();
    }

    if (ImGui::TreeNodeEx("Preprocess", 0)) {
        auto& p = config.preprocess.values;
        changed |= ImGui::Checkbox("Enable preprocess", &p.enabled);
        help_marker("Changes the picture the network is shown, and so how NR edits the picture.\n"
                    "Off = upstream. The first time it is turned on, NR rebuilds (a second or two).");
        if (!config.preprocess.hotkey.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s: %s", config.preprocess.hotkey.c_str(), prep_switch.on() ? "on" : "off");
        }
        static const char* const exposures[] = {"Off (upstream)", "Auto", "Fixed"};
        static const char* const curves[] = {"None (upstream)", "Neutral", "Reinhard", "Filmic", "GT", "ACES", "AgX"};
        changed |= ImGui::Combo("Exposure", &p.exposure, exposures, 3);
        help_marker("Auto exposure after Unreal Engine's design, for games that give none;\n"
                    "Exposure bias is added on top. Fixed uses the bias alone. When the game\n"
                    "gives its exposure and you want your own look, use Off.");
        changed |= deferred_slider("Exposure bias (EV)", p.bias_ev, p.bias_ev, -8.0f, 8.0f, 0.0f, false,
                                   "Each EV doubles or halves what the network sees. 0 = upstream.\n"
                                   "Fixed: the whole gain. Auto: added to what auto measured.");
        changed |= ImGui::Combo("Curve", &p.curve, curves, 7);
        help_marker("The display curve of the picture NR is shown. Test the effect yourself;\n"
                    "the hotkey compares on the same picture.");
        changed |= deferred_slider("Contrast", p.contrast, p.contrast, 0.5f, 2.0f, 1.0f, false,
                                   "Of the picture NR is shown, about mid grey. 1 = upstream.");
        changed |= deferred_slider("Saturation", p.saturation, p.saturation, 0.05f, 2.0f, 1.0f, false,
                                   "Of the picture NR is shown, luminance kept. 1 = upstream.");
        changed |= hotkey_text("Hotkey##prep", config.preprocess.hotkey);
        help_marker("[Preprocess] Hotkey: switches the whole preprocess on or off for this run (not written to the file).\n"
                    "Ctrl, Shift, Alt and one key; empty: no hotkey.");
        changed |= ImGui::Checkbox("Sound##prep", &config.preprocess.sound);
        help_marker("[Preprocess] Sound. A cue when it switches: two notes going up = on, going down = off.");
        ImGui::TreePop();
    }

    if (ImGui::TreeNodeEx("Log", 0)) {   // [Log]: read once when the game starts
        changed |= ImGui::Checkbox("Write the log", &config.log_enabled);
        help_marker("[Log] Enabled. dlssnr-amd.log in the game's folder. Takes effect after a restart.");
        changed |= ImGui::Checkbox("Clear on start", &config.log_clear);
        help_marker("[Log] ClearOnStart. On: each start of the game begins a new log; off: appends to the last one.\n"
                    "Takes effect after a restart.");
        ImGui::TreePop();
    }

    // [RenoDX] Enabled while the option is off (it is switched by restarting the game).
    if (!renodx_on && ImGui::CollapsingHeader("RenoDX style")) {
        RenoDXAccess access{config.renodx};
        changed |= nr::control_ui::draw_renodx(access, nr::control_ui::Route::kReShade, false);
    }

    if (changed) {
        config.resolve();
        if (!config_path.empty()) config.save(config_path);
        if (session) {
            session->set_history_strength(renodx_on ? 1.0f : config.history);
            session->set_model_scale(renodx_on ? 1.0f : config.model_scale);
            session->set_white_point(config.white_point);
            session->set_max_passes(renodx_on ? 1u : uint32_t(c.passes));
        }
    }
    if (ImGui::TreeNodeEx("Motion vectors", ImGuiTreeNodeFlags_DefaultOpen)) {   // MotionVectors, applied at the next present
        static const char* const sources[] = {"VORT", "LumeniteFX Kernel"};
        const bool installed = lumenite_on_disk();
        int chosen = -1;
        if (ImGui::BeginCombo("Source##motion", sources[std::clamp(config.motion_vectors, 0, 1)])) {
            if (ImGui::Selectable(sources[0], config.motion_vectors == 0)) chosen = 0;
            ImGui::BeginDisabled(!installed);
            if (ImGui::Selectable(sources[1], config.motion_vectors == 1)) chosen = 1;
            ImGui::EndDisabled();
            if (!installed && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("LumeniteFX is not installed. install.bat (LumeniteFX: y) installs it from its author's GitHub.");
            ImGui::EndCombo();
        }
        help_marker("The ReShade effect that estimates the motion NR's history follows.\n"
                    "VORT comes with this package. LumeniteFX Kernel (by Afzaal) is installed separately,\n"
                    "with install.bat (LumeniteFX: y), from its author's GitHub.\n"
                    "Switching makes ReShade compile its effects again (NR pauses until they are ready)\n"
                    "and restarts NR's history.");
        if (chosen >= 0) {   // choosing LumeniteFX again after it was not used retries it
            config.motion_vectors = chosen;
            if (!config_path.empty()) config.save(config_path);
            motion_choice_changed();
            log("[nr] motion vectors: %s chosen in the add-on page", sources[chosen]);
        }
        if (config.motion_vectors == 1 && motion.fell_back)
            ImGui::TextWrapped("LumeniteFX is not used this session: %s. VORT runs instead; choose LumeniteFX "
                               "again to retry.", motion.reason.c_str());
        else if (motion.lumenite_active)
            ImGui::TextDisabled("LumeniteFX Kernel%s", motion.verified ? ": its vectors follow the picture" : "");
        ImGui::TreePop();
    }

    if (ImGui::TreeNodeEx("Engine data", 0)) {   // what the game hands NR here (nr_engine_panel.hpp)
        nr::pe::panel::Facts pf = panel_facts;
        pf.motion_source = motion.lumenite_active ? "LumeniteFX Kernel" : "VORT";
        if (session) {
            const nr::pe::Session::State st = session->state();
            pf.resizes = st.resizes; pf.carried = st.carried;
            pf.motion_without_depth = st.motion_without_depth;
            pf.renodx_known = st.renodx_known;
            pf.renodx_hdr = st.renodx.hdr_mode; pf.renodx_absolute = st.renodx.absolute;
            pf.renodx_feed = st.renodx.feed; pf.renodx_divisor = st.renodx.divisor;
            pf.preprocess_ev = st.prep_ev;
        }
        if (pf.format.empty()) ImGui::TextDisabled("Not known until NR runs.");
        else {
            nr::control_ui::engine_legend();
            for (const std::string& l : nr::pe::panel::lines(pf)) nr::control_ui::engine_line(l);
        }
        ImGui::TreePop();
    }

    ImGui::Separator();
    ImGui::TextUnformatted("Status");
    char line[256];
    std::snprintf(line, sizeof line, "DLSS5_Feed: %s; motion %s, depth %s",
                  technique.handle ? "found" : "MISSING", mv_var.handle ? "found" : "missing",
                  depth_var.handle ? "found" : "missing");
    ImGui::TextUnformatted(line);
    std::snprintf(line, sizeof line, "frames: %u enhanced, %u declined", frames_run, frames_declined);
    ImGui::TextUnformatted(line);
    // The cost, the way OptiScaler's overlay reports its own: the GPU's own
    // timestamps around the work, so it is what the pass adds to the frame and
    // not what the call took on the CPU. Dashes until the first pair is back.
    if (!last_status.empty()) ImGui::TextUnformatted(last_status.c_str());
}

// Two seconds of "Preprocess: ON/OFF" in the corner after it switches, menu open or not. Drawn by
// ReShade on the finished picture, so nothing processes it.
void on_reshade_present(effect_runtime* rt) {
    std::lock_guard<std::mutex> guard(lock);
    if (rt != bound) return;
    ++presents;
    motion_sync(rt);
    if (session) session->idle();   // NR off for a while: [Network] Residency
}

void on_reshade_overlay(effect_runtime*) {
    std::lock_guard<std::mutex> guard(lock);
    // [Network] OSD: after a network switch (hotkey, menu or ini) the build's progress, then the running network for
    // 3 s (switch, the default); always the running network (always); nothing (off).
    if (session && config.network.osd != 0) {
        static int seen_want = -2, seen_running = -2;
        static std::chrono::steady_clock::time_point show_until{};
        const nr::pe::Session::State st = session->state();
        const auto now = std::chrono::steady_clock::now();
        if (st.want_net >= 0 && (st.want_net != seen_want || (st.running && st.running_net != seen_running))) {
            if (seen_want != -2 && seen_running >= 0) show_until = now + std::chrono::seconds(3);
            seen_want = st.want_net;
            if (st.running) seen_running = st.running_net;
        }
        const bool switching = st.building && st.building_net >= 0 && st.running && st.building_net != st.running_net;
        if (switching || now < show_until || config.network.osd == 2) {
            ImGui::SetNextWindowPos(ImVec2(24.0f, 64.0f));
            ImGui::SetNextWindowBgAlpha(0.65f);
            ImGui::Begin("##nr-osd", nullptr,
                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
            if (switching) {
                char phase[128];
                const float f = build_fraction(st, phase, sizeof phase);
                ImGui::Text("NR: switching to %s ... %.0f%%", network_name(st.building_net), f * 100.0f);
            } else {
                ImGui::Text("NR: %s", st.running ? network_name(st.running_net) : "off");
            }
            ImGui::End();
        }
    }
    if (prep_switch.since_switch() > 2.0) return;
    ImGui::SetNextWindowPos(ImVec2(24.0f, 24.0f));
    ImGui::SetNextWindowBgAlpha(0.65f);
    ImGui::Begin("##nr-preprocess", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    ImGui::Text("NR Preprocess: %s", prep_switch.on() ? "ON" : "OFF");
    ImGui::End();
}

void on_render_technique(effect_runtime* rt, effect_technique t, command_list* cl,
                         resource_view rtv, resource_view /*rtv_srgb*/) {
    std::lock_guard<std::mutex> guard(lock);
    const nr::Controls& controls = settings();
    // With the RenoDX-style option NR's switch is [RenoDX] NeuralUplift ([DlssNr]'s does not apply).
    if (!controls.enabled && !renodx_on) return;
    if (!bound) { bound = rt; resolve(rt); }
    if (rt != bound) return;
    if (technique.handle == 0) {
        // Re-ask once in a while: the effect may have been added after the
        // runtime initialised without a reload event we saw.
        static unsigned since = 0;
        if (++since % 300 == 0) resolve(rt);
        return;
    }
    if (t.handle != technique.handle) return;
    motion_probe(rt, cl, rtv);
    feed(rt, cl, rtv);
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        nr::pe::set_module(module);
        if (!reshade::register_addon(module)) return FALSE;
        previous_filter = SetUnhandledExceptionFilter(&crash_filter);
        nr::pe::install_crash_watch();
        log("[nr] ---------------------------------------------------------------");
        log("[nr] ReShade add-on loaded from %s (build %s)", nr::pe::module_folder(), NR_BUILD_STAMP);
        const std::string folder = nr::pe::module_folder();
        if (!folder.empty()) {
            config_path = folder + "\\dlssnr-amd.ini";
            config.load(config_path);
            // [Network] ACO Mode: decided before any device of ours (or a game's we add features to) is made.
            nr::pe::configure_network(folder);
            log("[nr] settings file %s: enabled=%d intensity=%.2f history=%.2f", config_path.c_str(),
                int(config.controls.enabled), config.controls.intensity, config.history);
            renodx_on = config.renodx.enabled;
            if (renodx_on) log("[nr] RenoDX-style option on ([RenoDX] Enabled = 1 at start)");
        }
        // ReShade as a Vulkan layer loads add-ons inside its vkCreateInstance
        // hook: vulkan-1.dll is in the process and the game's vkCreateDevice
        // has not happened yet, which is exactly when the device watcher's
        // hooks have to go in. ReShade as dxgi.dll over DXVK/vkd3d-proton finds
        // vulkan-1 loaded as well; the hooks are harmless there, as long as
        // they leave with the add-on (DLL_PROCESS_DETACH below).
        if (GetModuleHandleW(L"vulkan-1.dll")) {
            if (minhook_start()) {
                vk_watch = true;
                log("[nr] vulkan-1.dll is loaded: watching the game's device creation (%s)",
                    nr::pe::vkdevice::install() ? "hooks installed" : "hooks FAILED");
                nr::pe::vkdevice::set_device_callback(on_vulkan_device_created);
            }
        }
        reshade::register_event<reshade::addon_event::init_device>(on_init_device);
        reshade::register_event<reshade::addon_event::create_swapchain>(on_create_swapchain);
        reshade::register_event<reshade::addon_event::init_swapchain>(on_init_swapchain);
        reshade::register_event<reshade::addon_event::init_effect_runtime>(on_init_effect_runtime);
        reshade::register_event<reshade::addon_event::destroy_effect_runtime>(on_destroy_effect_runtime);
        reshade::register_event<reshade::addon_event::reshade_reloaded_effects>(on_reloaded_effects);
        reshade::register_event<reshade::addon_event::reshade_render_technique>(on_render_technique);
        reshade::register_event<reshade::addon_event::reshade_present>(on_reshade_present);
        reshade::register_event<reshade::addon_event::destroy_device>(on_destroy_device);
        reshade::register_overlay(nullptr, draw_overlay);
        reshade::register_event<reshade::addon_event::reshade_overlay>(on_reshade_overlay);
    } else if (reason == DLL_PROCESS_DETACH) {
        // ReShade unloads add-ons when its last device is destroyed, and the
        // game goes on: the vulkan-1 hooks must not outlive this module. At
        // process exit (reserved != null) the other threads are gone and
        // nothing calls into vulkan-1 any more, so it is left alone.
        if (minhook_initialized && !reserved) {
            unhook_d3dcompile();
            if (vk_watch) nr::pe::vkdevice::uninstall();
            MH_Uninitialize();
            minhook_initialized = false;
            vk_watch = false;
        }
        reshade::unregister_overlay(nullptr, draw_overlay);
        reshade::unregister_event<reshade::addon_event::reshade_overlay>(on_reshade_overlay);
        // First: the filter lives in code that is about to be unmapped.
        SetUnhandledExceptionFilter(previous_filter);
        nr::pe::remove_crash_watch();
        reshade::unregister_event<reshade::addon_event::init_device>(on_init_device);
        reshade::unregister_event<reshade::addon_event::create_swapchain>(on_create_swapchain);
        reshade::unregister_event<reshade::addon_event::init_swapchain>(on_init_swapchain);
        reshade::unregister_event<reshade::addon_event::init_effect_runtime>(on_init_effect_runtime);
        reshade::unregister_event<reshade::addon_event::destroy_effect_runtime>(on_destroy_effect_runtime);
        reshade::unregister_event<reshade::addon_event::reshade_reloaded_effects>(on_reloaded_effects);
        reshade::unregister_event<reshade::addon_event::reshade_render_technique>(on_render_technique);
        reshade::unregister_event<reshade::addon_event::reshade_present>(on_reshade_present);
        reshade::unregister_event<reshade::addon_event::destroy_device>(on_destroy_device);
        reshade::unregister_addon(module);
    }
    return TRUE;
}
