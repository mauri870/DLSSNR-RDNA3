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
#include "nr_screenshot.hpp"
#include "nr_gpu_completion.hpp"
#include "nr_pe_config.hpp"
#include "nr_wilsjo2.hpp"
#include "nr_control_ui.hpp"
#include "nr_ini_edit.hpp"
#include "nr_reshade_layer_keys.hpp"
#include "nr_pe_interop.hpp"
#include "nr_pe_log.hpp"
#include "nr_pe_watchdog.hpp"
#include "nr_pe_session.hpp"
#include "nr_hud_capture.hpp"
#include "nr_engine_panel.hpp"
#include "nr_pe_vkdevice.hpp"
#include "nr_vram_ledger.hpp"
#include "nr_pe_events.hpp"

#include <MinHook.h>
#include <d3dcommon.h>
#include <dxgi1_6.h>
#include <tlhelp32.h>

// ReShade's overlay draws through its own ImGui function table; the header
// must see imgui.h first, with the texture id type ReShade expects.
#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>
// NR Control's pages (nr_control_pages.hpp) through ReShade's MSVC-built ImGui table: its ImVec2 returns the MSVC way
#define NR_IMGUI_MSVC_TABLE 1
#include "nr_control_pages.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
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
#include <unordered_map>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

extern "C" __declspec(dllexport) const char* NAME = "DLSS 5 NR on AMD";
extern "C" __declspec(dllexport) const char* DESCRIPTION =
    "Runs the DLSS 5 neural-rendering network on AMD, on this frame, using the motion vectors "
    "DLSS5_Feed.fx produces from a community motion-vector shader. Enable a provider (VORT, "
    "qUINT, ...) above DLSS5_Feed in the effect list. Settings: dlssnr-amd.ini next to this add-on.";

// The generated thunks' loader (linux/build/build_package.sh): nothing pulls
// Vulkan in until a game has its own graphics stack up.
extern "C" void nr_vk_load(void);
extern "C" void nr_vk_use_loader(int);
extern "C" void* nr_vk_p_vkGetDeviceQueue;
extern "C" void* nr_vk_p_vkGetPhysicalDeviceQueueFamilyProperties;
extern "C" void* nr_vk_p_vkCmdClearColorImage;
extern "C" int nr_vk_device_direct(void* device, int on);   // the driver's device functions while a device goes

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

// NR's own resources made through reshade::api (the frame copies: HUD FIX's, the PQ conversion's, the motion copies,
// Inspect's), in NR Control's video memory (nr_vram_ledger.hpp, "Frame copies"): D3D12 by its allocation size, D3D11 /
// D3D10 by their texels, counted out when the API destroys them; on the other APIs by their texels until unmake_resource.
bool make_resource(device* dev, const resource_desc& d, const subresource_data* init, resource_usage state, resource* out) {
    if (!dev->create_resource(d, init, state, out)) return false;
    switch (dev->get_api()) {
        case device_api::d3d12:
            nr::vram::track_d3d12(reinterpret_cast<ID3D12Device*>(dev->get_native()), reinterpret_cast<ID3D12Resource*>(out->handle),
                                  nr::vram::kCopies);
            break;
        case device_api::d3d11: nr::vram::track_d3d11(reinterpret_cast<ID3D11Resource*>(out->handle), nr::vram::kCopies); break;
        case device_api::d3d10: nr::vram::track_d3d10(reinterpret_cast<ID3D10Resource*>(out->handle), nr::vram::kCopies); break;
        default:
            if (d.heap == memory_heap::gpu_only)
                nr::vram::add_external(out->handle, d.type == resource_type::buffer ? d.buffer.size
                                       : uint64_t(format_slice_pitch(d.texture.format, format_row_pitch(d.texture.format, d.texture.width),
                                                                     d.texture.height)) * d.texture.depth_or_layers,
                                       nr::vram::kCopies);
            break;
    }
    return true;
}
void unmake_resource(device* dev, resource r) {
    nr::vram::forget_external(r.handle);
    dev->destroy_resource(r);
}
using nr::pe::log;

constexpr const char* kEffect = "DLSS5_Feed.fx";
constexpr const char* kTechnique = "DLSS5_Feed";

std::mutex lock;
std::unique_ptr<nr::pe::Session> session{};
// The device the session runs on (feed's; null: made by the Vulkan self-test). A game may make and destroy other devices
// meanwhile (Resident Evil Requiem makes a D3D11 one beside its D3D12 one): their destruction is not the session's.
device* session_device{};
nr::pe::Config config{};
std::string config_path;
nr::pe::PreprocessSwitch prep_switch;   // [Preprocess] Enabled and its hotkey
bool vk_loaded = false;

// [RenoDX] Enabled as it was when the add-on loaded: the option is switched by restarting the game (its other
// keys follow the file while the game runs).
bool renodx_on = false;

// The one runtime we feed. A game usually has one; a second one (a proxy
// swapchain, a second window) is ignored until the first goes away.
effect_runtime* bound{};
effect_technique technique{};
effect_texture_variable mv_var{};
effect_texture_variable depth_var{};
// The engine-data panel's facts from the last frame fed (feed), drawn on the add-on page.
nr::pe::panel::Facts panel_facts{};
bool need_reset{true};
bool hold_released{};   // [Inspect] HoldFrame let go this frame: the next frame resets (run_layers)
// The bound effect runtime's presents, and their count when NR last ran a frame: frames between two NR frames
// without NR ([DlssNr] Enabled or RenoDX's NeuralUplift off, the technique off, frames declined) and the next NR
// frame starts its history afresh, as OptiScaler's features do when NR comes back on.
uint64_t presents{};
uint64_t presents_at_run{};
bool have_run{};
// More presents than NR frames by up to this many is frame generation (a generated frame between rendered ones,
// several with multi-frame generation), not NR having been off.
constexpr uint64_t kResumeGap = 8;
bool missing_reported{};
std::string last_status{};
unsigned frames_run{};
unsigned frames_declined{};
// ---- the NR chain: [LayerN] sections in dlssnr-amd.ini (nr_pe_config.hpp read_chain), as on the .asi route ----------
// Each enabled layer is a run of its own on the back buffer, in layer order (on this route every layer runs at Present):
// its processing (RenoDX, or wilsjo2 = this route's own composition under the original's key names, layer_config), its
// network ([Network] Model, or Network = a codename), its own feature (history) and its own settings. Without any
// [LayerN] section the route runs as it always has, from [DlssNr] or [RenoDX].
struct AddonLayer {
    int number = 0;
    bool renodx = true;
    int network = -1;              // nr::NetworkChoice; -1: [Network] Model
    nr::pe::RenoDXConfig rdx;      // a RenoDX layer's keys
    nr::pe::Config cfg;            // a wilsjo2 layer's
    // The session's feature for it (its history, its RenoDX stage): one per number and processing, so a layer switched
    // to the other processing starts afresh and the old one's are released (chain_poll)
    uint64_t feature() const { return uint64_t(number) * 2u + (renodx ? 1u : 0u); }
};
nr::pe::ChainFile chain_file{};
nr::pe::ChainConfig chain{};
std::vector<AddonLayer> layers_now{};   // enabled, by number
bool layered{};                 // the file has [LayerN] sections

void view_layers(const std::vector<int>& numbers);
nr::control::LayerStatus* view_row(int number, bool make);

void chain_poll() {
    if (config_path.empty() || !chain_file.poll(config_path, chain)) return;
    const std::vector<AddonLayer> was = std::move(layers_now);
    layers_now.clear();
    if (!layered && chain.layered && session) {
        // The implicit first layer also owns named history. A new explicit chain starts it afresh.
        session->release_feature(2u);
        session->release_feature(3u);
    }
    layered = chain.layered;
    std::string said;
    for (const nr::pe::LayerConfig& l : chain.layers) {
        if (!l.enabled || !layered) continue;
        AddonLayer a;
        a.number = l.number;
        a.renodx = l.processing == nr::pe::Processing::kRenoDX;
        if (!l.network.empty()) {
            a.network = nr::network_choice(l.network);
            if (a.network < 0) log("[nr] dlssnr-amd.ini [Layer%d] Network = %s not known; [Network] Model", l.number, l.network.c_str());
        }
        if (l.placement != nr::pe::Placement::kPresent && l.placement != nr::pe::Placement::kAfter)
            log("[nr] dlssnr-amd.ini [Layer%d]: this route runs every layer at Present", l.number);
        if (a.renodx) a.rdx = l.renodx;
        else a.cfg = nr::pe::layer_config(l);
        // [Inspect]'s debug views, of the layer it chose (RenoDX layers: the stage's; wilsjo2 layers: the flow's
        // resolve, wilsjo2_resolve.comp)
        if (a.renodx && chain.inspect.layer == l.number) a.rdx.values.debug_view = chain.inspect.debug_view;
        if (!a.renodx && chain.inspect.layer == l.number) {
            a.cfg.controls.debug_view = chain.inspect.debug_view;
            a.cfg.controls.show_skin_mask = chain.inspect.skin_mask;
        }
        char one[96];
        std::snprintf(one, sizeof one, "%s%d %s%s%s", said.empty() ? "" : ", ", l.number, a.renodx ? "RenoDX" : "wilsjo2",
                      a.network >= 0 ? " " : "", a.network >= 0 ? nr::network_codename(a.network) : "");
        said += one;
        layers_now.push_back(std::move(a));
    }
    if (layered) nr::pe::events::note(nr::pe::events::kSettings, "NR chain: %s", said.empty() ? "no layer enabled" : said.c_str());
    if (layered) {   // the page's rows follow the file at once (a chain with no layer on runs no frame to do it)
        std::vector<int> numbers;
        for (const AddonLayer& l : layers_now) numbers.push_back(l.number);
        view_layers(numbers);
    }
    // A layer that went or runs another processing / network: its feature released, its costs measured anew
    for (const AddonLayer& o : was) {
        const auto n = std::find_if(layers_now.begin(), layers_now.end(), [&](const AddonLayer& l) { return l.number == o.number; });
        const bool same = n != layers_now.end() && n->renodx == o.renodx && n->network == o.network;
        if (same) {
            // Release this layer in every parked runtime too; returning to an earlier encoding must start fresh.
            if (session && !o.renodx && (n->cfg.controls.reversible_mode != o.cfg.controls.reversible_mode ||
                                        n->cfg.white_point != o.cfg.white_point))
                session->release_feature(o.feature());
            continue;
        }
        if (session) {
            if (n == layers_now.end() || n->renodx != o.renodx) session->release_feature(o.feature());
            session->reset_layer_timing(o.number);
        }
        if (nr::control::LayerStatus* r = view_row(o.number, false)) {
            r->net = nr::control::kNoNet;
            r->width = r->height = r->nr_width = r->nr_height = 0;
            r->passes = 0;
            r->gpu_ms = r->network_ms = 0.f;
            r->panel_lines = 0;
            r->updated = 0;
        }
    }
}

// The motion-vector source (VORT or LumeniteFX Kernel), below.
void motion_after_reload();
void motion_choice_changed();

const nr::Controls& settings() {
    chain_poll();
    const int motion_was = config.motion_vectors;
    if (!config_path.empty() && config.reload(config_path)) {
        if (config.motion_vectors != motion_was) {   // MotionVectors edited in the file: apply it at the next present
            motion_choice_changed();
            log("[nr] settings reloaded: MotionVectors %s", config.motion_vectors == 1 ? "lumenite" : "vort");
        }
        if (session) {
        session->set_history_strength(renodx_on ? 1.0f : config.history);
    }
        static const char* const transfers[] = {"matched residual", "edge-aware lighting + colour", "classic", "bilinear",
                                                "lighting + colour"};
        static const char* const scalers[] = {"bilinear", "catmullrom", "lanczos3", "fsr1"};
        log("[nr] settings reloaded: intensity %.2f tone %.2f structure %.2f skin %.2f mask %d history %.2f passes %d model_scale %.2f"
            " enlargement %s%s%s%s",
            config.controls.intensity, config.controls.local_tone, config.controls.local_structure,
            config.controls.skin_structure, int(config.controls.automatic_mask), config.history,
            config.controls.passes, config.model_scale, transfers[std::clamp(config.controls.transfer, 0, 4)],
            config.controls.transfer == nr::kEnlargeClassic ? " " : "",
            config.controls.transfer == nr::kEnlargeClassic ? scalers[std::clamp(config.controls.classic_scaler, 0, 3)] : "",
            config.controls.noise_every_frame ? "; NoiseEveryFrame on" : "");
        if (renodx_on)
            log("[nr] RenoDX-style option: [RenoDX] read (colour %.2f, transfer %.2f, resolution scale %.2f, detail "
                "stability %u)", config.renodx.values.color_strength, config.renodx.values.transfer_strength,
                config.renodx.values.resolution_scale, config.renodx.values.detail_stability);
    }
    config.controls.preprocess = prep_switch.frame(config.preprocess);
    return config.controls;
}

nr::pe::Session& the_session() {
    if (!session) {
        // The network only: every layer's processing (RenoDX's stage, the wilsjo2 flow) composes its answer itself.
        session = std::make_unique<nr::pe::Session>(std::string{});
        session->set_history_strength(renodx_on ? 1.0f : config.history);
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
color_space back_buffer_space{color_space::unknown};
bool back_buffer_d3d10{};   // the swap chain's device is D3D10's (ReShade reports no colour space there)

// The RenoDX-style option at RenoDX's Present hook point (present_path.hpp), which is where this route runs: the
// back buffer's encoding from the colour space - 1 SDR, 2 HDR10 (PQ over BT.2020), 3 scRGB - with NRPresentEncoding
// able to force one the back buffer can carry; 0 = not served (HLG, unknown).
uint32_t present_encoding(const nr::renodx::Settings& s, DXGI_FORMAT f) {
    const bool float_format = f == DXGI_FORMAT_R16G16B16A16_FLOAT;
    // A D3D10 swap chain (ReShade 6.8 reports no colour space for D3D10) in an 8-bit or 10-bit format is SDR, as the
    // other processing takes it; a floating-point one, and an unknown colour space on any other API, stay not served.
    const bool unknown_sdr = back_buffer_space == color_space::unknown && back_buffer_d3d10 && !float_format &&
                             (f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
                              f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
                              f == DXGI_FORMAT_B8G8R8X8_UNORM || f == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB ||
                              f == DXGI_FORMAT_R10G10B10A2_UNORM);
    const uint32_t automatic = back_buffer_space == color_space::srgb_nonlinear || unknown_sdr ? 1u
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
// D3D10/D3D11 (DXVK's own back buffers) and Vulkan swap chains without other view formats, in an
// 8-bit UNORM or RGBA16F format (the formats the answer is stored in directly). ReShade does not retry
// a creation that fails with the add-on's description, so nothing else is touched.
bool on_create_swapchain(device_api api, swapchain_desc& desc, void*) {
    if (api != device_api::d3d10 && api != device_api::d3d11 && api != device_api::vulkan) return false;
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

void cap_swapchain(swapchain* sc, bool add);   // HUD FIX's capture (below)
void on_init_swapchain(swapchain* sc, bool) {
    cap_swapchain(sc, true);   // HUD FIX's capture looks for its back buffers
    // **Called directly, NOT through rs::call.** That shim exists for MSVC's
    // hidden-pointer struct return and takes that path for any return type it
    // is given (now refused at compile time). `get_color_space` returns a plain enum, which
    // MSVC hands back in EAX, so routing it through the shim pushes an argument
    // the callee never pops - and __thiscall makes the callee clean the stack.
    // Half-Life 2 hung on the first swapchain, and the colour space read as
    // 216789112, which is not one of the five values it can be.
    // A swap chain of another device than the one NR's session runs on (a game's helper device, as RE Engine makes a
    // D3D11 one beside its D3D12 one) does not decide the session's colour space.
    if (session_device && sc->get_device() != session_device) return;
    back_buffer_d3d10 = sc->get_device()->get_api() == device_api::d3d10;
    const color_space cs = sc->get_color_space();
    if (cs == back_buffer_space) return;
    // Colour-space changes can return to a parked runtime with the same image format.
    if (session) {
        for (const AddonLayer& l : layers_now) if (!l.renodx) session->release_feature(l.feature());
    }
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
DebugDump dump_state{};

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
    if (dump_state.before.handle) unmake_resource(dev, dump_state.before);
    if (dump_state.after.handle) unmake_resource(dev, dump_state.after);
    dump_state.before = dump_state.after = resource{};
    if (!dump_bpp(f)) {
        log("[nr] dump: format %u is not one this decoder knows", unsigned(f));
        return false;
    }
    resource_desc d(w, h, 1, 1, f, 1, memory_heap::gpu_to_cpu, resource_usage::copy_dest);
    if (!make_resource(dev, d, nullptr, resource_usage::copy_dest, &dump_state.before) ||
        !make_resource(dev, d, nullptr, resource_usage::copy_dest, &dump_state.after)) {
        log("[nr] dump: the staging pair could not be created");
        return false;
    }
    dump_state.w = w; dump_state.h = h; dump_state.fmt = f;
    return true;
}

// The effect normally records on ReShade's immediate list, which is submitted once. Flush before signalling:
// signalling first would only cover the application's earlier work, not these still-recorded commands.
struct EffectFence {
    device* dev{}; fence handle{}; uint64_t value{};
};
std::unordered_map<command_queue*, std::shared_ptr<EffectFence>> effect_fences;
nr::gpu::Uses effect_uses;
struct DeferredEffect {
    device* dev; nr::gpu::Uses uses; std::function<void()> destroy;
};
std::vector<DeferredEffect> effect_retired;
void reap_effect_resources(device* ending = nullptr) {
    for (auto i = effect_retired.begin(); i != effect_retired.end();) {
        if (i->dev == ending || i->uses.ready()) { i->destroy(); i = effect_retired.erase(i); }
        else ++i;
    }
}
void retire_effect_resource(device* dev, resource r) {
    if (r.handle) effect_retired.push_back({dev, effect_uses, [dev, r] { unmake_resource(dev, r); }});
}
void retire_effect_view(device* dev, resource_view v) {
    if (v.handle) effect_retired.push_back({dev, effect_uses, [dev, v] { dev->destroy_resource_view(v); }});
}
struct EffectRecording {
    command_queue* queue; command_list* list; bool immediate;
    nr::gpu::Ticket ticket;
    nr::gpu::Scope scope;
    EffectRecording(effect_runtime* rt, command_list* cl) : queue(rt->get_command_queue()), list(cl),
        immediate(queue->get_immediate_command_list() == cl),
        ticket(immediate ? std::make_shared<nr::gpu::Recording>() : nr::gpu::recording(cl)), scope(ticket) {
        effect_uses.touch(); reap_effect_resources();
    }
    ~EffectRecording() {
        if (!immediate) { ticket->submitted({}); return; } // No after-submit callback for application-owned lists.
        queue->flush_immediate_command_list();
        auto& f = effect_fences[queue];
        if (!f) {
            f = std::make_shared<EffectFence>(); f->dev = queue->get_device();
            f->dev->create_fence(0, fence_flags::none, &f->handle);
        }
        const uint64_t value = ++f->value;
        if (f->handle.handle && queue->signal(f->handle, value)) {
            const auto kept = f;
            ticket->submitted([kept, value] {
                return !kept->dev || kept->dev->get_completed_fence_value(kept->handle) >= value;
            });
        } else {
            // D3D9/10 backends without fence support still provide a real completion point.
            queue->wait_idle();
        }
        ticket->seal();
    }
};

// ---- [Inspect] on the ReShade route --------------------------------------------------------------------------------
// The comparison by copies of the back buffer alone, the same on D3D10, D3D11, D3D12 and Vulkan: ReShade copies regions
// of one size on every API (no scaling on D3D12, d3d12_impl_command_list.cpp copy_texture_region). So: ApplyModel off,
// wipe, swap and the divider (a two-pixel white column: a texture of its own cleared whole - ReShade clears no rects on
// D3D11 - and copied in) as on the .asi route (wilsjo2_inspect.comp); side by side
// with each half at 1:1, the frame's middle half in each (the .asi's Zoom 2: the half filled, its sides cropped).
struct InspectCopies {
    device* dev = nullptr;
    resource off{}, on{};   // copy_dest between uses
    resource white{};       // the divider: 2 x h, cleared white once (copy_source after)
    resource_view white_rtv{};
    bool white_ready = false;
    unsigned w = 0, h = 0;
    format fmt{};
};
InspectCopies inspect_copies{};

// Hold owns its input independently of comparison images and effect reloads.
struct InspectHold {
    device* dev = nullptr;
    resource colour{}, depth{}, motion{};
    resource_desc desc{};
    bool held = false, inverted = false, metadata = false, linear = false;
    int encoding = 0; float white = 80.f;
    int layer = 0;
    float sx = 1.f, sy = 1.f;
};
InspectHold inspect_input{};
void inspect_hold_release() {
    auto& h = inspect_input;
    if (h.dev) for (resource r : {h.colour, h.depth, h.motion})
        if (r.handle) retire_effect_resource(h.dev, r);
    h = InspectHold{};
}

void inspect_release() {
    if (inspect_copies.dev) {
        if (inspect_copies.white_rtv.handle) retire_effect_view(inspect_copies.dev, inspect_copies.white_rtv);
        for (resource* r : {&inspect_copies.off, &inspect_copies.on, &inspect_copies.white})
            if (r->handle) retire_effect_resource(inspect_copies.dev, *r);
    }
    inspect_copies = InspectCopies{};
}

bool inspect_ensure(device* dev, unsigned w, unsigned h, format f) {
    InspectCopies& c = inspect_copies;
    if (c.dev == dev && c.off.handle && c.w == w && c.h == h && c.fmt == f) return true;
    inspect_release();
    resource_desc d(w, h, 1, 1, f, 1, memory_heap::gpu_only, resource_usage::copy_source | resource_usage::copy_dest);
    if (!make_resource(dev, d, nullptr, resource_usage::copy_dest, &c.off) ||
        !make_resource(dev, d, nullptr, resource_usage::copy_dest, &c.on) ||
        !make_resource(dev, resource_desc(2, h, 1, 1, f, 1, memory_heap::gpu_only,
                                            resource_usage::render_target | resource_usage::copy_source),
                              nullptr, resource_usage::render_target, &c.white) ||
        !dev->create_resource_view(c.white, resource_usage::render_target,
                                   resource_view_desc(format_to_default_typed(f)), &c.white_rtv)) {
        static bool said = false;
        if (!said) { said = true; log("[nr] NR comparison: the copies of the back buffer (format %u) could not be made", unsigned(f)); }
        if (c.white_rtv.handle) retire_effect_view(dev, c.white_rtv);
        for (resource* r : {&c.off, &c.on, &c.white})
            if (r->handle) retire_effect_resource(dev, *r);
        c = InspectCopies{};
        return false;
    }
    c.dev = dev; c.w = w; c.h = h; c.fmt = f;
    return true;
}

// The back buffer (render_target) into `to` (copy_dest before and after).
void inspect_take(command_list* cl, resource colour, resource to) {
    const resource_usage rt = resource_usage::render_target, cs = resource_usage::copy_source;
    cl->barrier(1, &colour, &rt, &cs);
    cl->copy_texture_region(colour, 0, nullptr, to, 0, nullptr);
    cl->barrier(1, &colour, &cs, &rt);
}

// Freeze the layer's source images; history, noise and the meter continue to run.
bool inspect_hold(command_list* cl, resource colour, resource& depth, resource& motion_image,
                  bool& inverted, float& sx, float& sy) {
    auto& h = inspect_input;
    device* dev = cl->get_device();
    const auto desc = rs::call(dev, &device::get_resource_desc, colour);
    if (h.dev != dev || h.desc.texture.width != desc.texture.width ||
        h.desc.texture.height != desc.texture.height || h.desc.texture.format != desc.texture.format) {
        const int layer = h.layer;
        inspect_hold_release();
        h.dev = dev; h.desc = desc; h.layer = layer;
    }
    if (!h.held) {
        h.metadata = false;
        resource next[3]{};
        const resource sources[3] = {colour, depth, motion_image};
        bool made = true;
        for (int i = 0; i < 3; ++i) if (sources[i].handle) {
            auto d = rs::call(dev, &device::get_resource_desc, sources[i]);
            d.heap = memory_heap::gpu_only;
            d.usage = resource_usage::copy_source | resource_usage::copy_dest | resource_usage::shader_resource;
            d.flags = resource_flags::none;
            if (!make_resource(dev, d, nullptr, resource_usage::copy_dest, &next[i])) { made = false; break; }
        }
        if (!made) {
            for (resource r : next) if (r.handle) unmake_resource(dev, r);
            log("[nr] Hold: an input copy could not be made; the layer stays live");
            return false;
        }
        for (resource r : {h.colour, h.depth, h.motion}) retire_effect_resource(dev, r);
        h.colour = next[0]; h.depth = next[1]; h.motion = next[2];
        for (int i = 0; i < 3; ++i) if (sources[i].handle) {
            const resource_usage state = i ? resource_usage::shader_resource : resource_usage::render_target;
            const resource_usage kept = i ? resource_usage::shader_resource : resource_usage::copy_source;
            const auto cs = resource_usage::copy_source, cd = resource_usage::copy_dest;
            cl->barrier(1, &sources[i], &state, &cs);
            cl->copy_texture_region(sources[i], 0, nullptr, next[i], 0, nullptr);
            cl->barrier(1, &sources[i], &cs, &state);
            cl->barrier(1, &next[i], &cd, &kept);
        }
        h.inverted = inverted; h.sx = sx; h.sy = sy;
        h.held = true;
    } else {
        const auto rt = resource_usage::render_target, cd = resource_usage::copy_dest;
        cl->barrier(1, &colour, &rt, &cd);
        cl->copy_texture_region(h.colour, 0, nullptr, colour, 0, nullptr);
        cl->barrier(1, &colour, &cd, &rt);
    }
    depth = h.depth; motion_image = h.motion; inverted = h.inverted; sx = h.sx; sy = h.sy;
    return true;
}

bool inspect_fit_show(device* dev, command_list* cl, resource colour, resource off, const nr::pe::InspectConfig& ic,
                      unsigned w, unsigned h, format f);   // below, after HUD FIX
void inspect_fit_release(bool keep_stage);

// After the compared layers: the back buffer (render_target, `rtv` its view) shown as [Inspect] asks, from the copy
// taken before them (inspect_copies.off).
void inspect_show(command_list* cl, resource colour, resource_view rtv, const nr::pe::InspectConfig& ic, unsigned w,
                  unsigned h) {
    InspectCopies& c = inspect_copies;
    if (!c.off.handle || c.w != w || c.h != h) return;
    const resource_usage rt = resource_usage::render_target, cd = resource_usage::copy_dest, cs = resource_usage::copy_source;
    // regions of `from` (copy_dest) into the back buffer at `to_x`
    struct Part { resource from; unsigned x, to_x, width; };
    const auto put = [&](std::initializer_list<Part> parts) {
        cl->barrier(1, &colour, &rt, &cd);
        for (const Part& p : parts) {
            if (!p.width) continue;
            cl->barrier(1, &p.from, &cd, &cs);
            const subresource_box src{int32_t(p.x), 0, 0, int32_t(p.x + p.width), int32_t(h), 1};
            const subresource_box dst{int32_t(p.to_x), 0, 0, int32_t(p.to_x + p.width), int32_t(h), 1};
            cl->copy_texture_region(p.from, 0, &src, colour, 0, &dst, filter_mode::min_mag_mip_point);
            cl->barrier(1, &p.from, &cs, &cd);
        }
        cl->barrier(1, &colour, &cd, &rt);
    };
    const auto divider = [&](unsigned x) {   // the shader's: |uv.x - cut| < 1 / width, two columns at a whole cut
        if (x == 0 || x >= w) return;
        if (!c.white_ready) {
            const float white[4] = {1.f, 1.f, 1.f, 1.f};
            cl->clear_render_target_view(c.white_rtv, white);
            const resource_usage rt_ = resource_usage::render_target, cs_ = resource_usage::copy_source;
            cl->barrier(1, &c.white, &rt_, &cs_);
            c.white_ready = true;
        }
        const unsigned width = std::min(x + 1, w) - (x - 1);
        const subresource_box src{0, 0, 0, int32_t(width), int32_t(h), 1};
        const subresource_box dst{int32_t(x) - 1, 0, 0, int32_t(x - 1 + width), int32_t(h), 1};
        cl->barrier(1, &colour, &rt, &cd);
        cl->copy_texture_region(c.white, 0, &src, colour, 0, &dst, filter_mode::min_mag_mip_point);
        cl->barrier(1, &colour, &cd, &rt);
    };
    if (!ic.apply_model) {
        put({{c.off, 0, 0, w}});
        return;
    }
    if (ic.compare == 2) {
        const unsigned cut = unsigned(std::lround(std::clamp(ic.split, 0.f, 1.f) * float(w)));
        if (!ic.swap) put({{c.off, 0, 0, cut}});
        else put({{c.off, cut, cut, w - cut}});
        divider(cut);
    } else if (ic.compare == 1) {
        // Zoom below 2: the whole frame fitted in each half, a pass of its own (inspect_fit_show); else, or when that
        // cannot run, each half the frame's middle at 1:1 by copies
        if (ic.zoom < 1.999f && inspect_fit_show(cl->get_device(), cl, colour, c.off, ic, w, h, c.fmt)) return;
        inspect_take(cl, colour, c.on);
        const unsigned half = w / 2, q = w / 4;
        const resource left = ic.swap ? c.on : c.off, right = ic.swap ? c.off : c.on;
        put({{left, q, 0, half}, {right, half - q, half, w - half}});
        divider(half);
    }
}

// ---- HUD FIX ([HudFix], nr_pe_config.hpp): the game's HUD left as it drew it --------------------------------------
// Every layer of this route runs on the finished back buffer, HUD included. HUD FIX keeps the frame as it came before
// the layers and composes the layers' answer over it as NVIDIA's UICorrection does (Final = Back + (1 - a)(NR -
// Hudless)), as the .asi's Present layers do, with one of two methods:
//   Captured (D3D10/11/12): the frame copied where the game starts drawing its HUD (the capture below); the layers run
//            on that copy, the HUD is where the frame and the copy differ (wilsjo2_hudfix.comp HudMode 2);
//   Estimated: the HUD's alpha estimated from the frame and the last one (pixels that stay put while the picture around
//            them moves, HudMode 3); the layers run on the frame itself.
// Auto: Captured on the frames the capture holds, else Estimated (its state follows every frame either way). The images are made through ReShade (one description for every API, its barriers keep its
// tracking right); the dispatches are raw Vulkan on the session's device (record_raw on D3D12, record_raw_d3d11 on
// D3D10/11, record_raw_vk on Vulkan). Default off.
struct HudFixState {
    device* dev = nullptr;
    resource back[2]{};   // back[cur]: this frame before the layers; back[cur ^ 1]: the last frame's
    resource out{}, alpha{}, stats{};
    resource hudless{};   // the capture's copy
    unsigned w = 0, h = 0;
    format fmt{};
    uint32_t cur = 0, parity = 0;
    bool prev_valid = false, fresh = true, failed = false;
    uint64_t frame = 0, estimated_at = ~0ull;   // the estimate's last frame: after a gap it starts afresh
    std::unique_ptr<nr::wilsjo2::Stage> stage;
    bool stage_failed = false;
    std::string said;     // the last reason it did not run, logged once each
    std::mutex now_m;
    std::string now;      // what it did the last frame (the page's line)
};
HudFixState hudfix{};

// What HUD FIX does this frame.
struct HudPlan {
    bool run = false;        // compose over the layers' answer
    bool captured = false;   // from the capture (else the estimate)
    bool estimate = false;   // the estimate's state follows this frame
};

void hudfix_say(const std::string& why) {
    if (why == hudfix.said) return;
    hudfix.said = why;
    log("[nr] HUD FIX: %s", why.c_str());
}
void hudfix_now(const std::string& what) {
    std::lock_guard<std::mutex> g(hudfix.now_m);
    if (what == hudfix.now) return;
    static int said = 0;   // the log: its changes, the first 40
    if (said < 40) {
        ++said;
        log("[nr] HUD FIX now: %s%s", what.c_str(), said == 40 ? " (further changes not logged)" : "");
    }
    hudfix.now = what;
}

unsigned hudfix_stats_w(unsigned w) { return std::max(4u, (w + 7) / 8); }
unsigned hudfix_stats_h(unsigned h) { return 1 + 2 * ((h + 7) / 8); }

// One texture copy between `from` and `to` in the states they are kept in (ReShade's barriers around it).
void hudfix_copy(command_list* cl, resource from, resource_usage from_state, resource to, resource_usage to_state) {
    const resource_usage cs = resource_usage::copy_source, cd = resource_usage::copy_dest;
    cl->barrier(1, &from, &from_state, &cs);
    cl->barrier(1, &to, &to_state, &cd);
    cl->copy_texture_region(from, 0, nullptr, to, 0, nullptr);
    cl->barrier(1, &to, &cd, &to_state);
    cl->barrier(1, &from, &cs, &from_state);
}

// ---- HUD FIX's capture: where the game starts drawing its HUD over the finished scene ----------------------------
// The rules: nr_hud_capture.hpp (the .asi follows the same). Here the game's writes come from ReShade's events, a D3D12
// list's read when it is executed, D3D10/11's immediate context in its own order. D3D10/11 copy at the last scene ->
// overlay place of the last frame only (one copy a frame; the count of places is kept a frame). D3D12 lists are recorded
// on many threads, not in the GPU's order, so they copy at every place (kMaxCopies a frame at most). Not followed: a
// D3D11 deferred context's draws on the back buffer (the frame is not captured) and secondary lists. Vulkan snapshots
// before LOAD passes after an observed outside-pass command or reset. ReShade's begin event also represents next
// subpasses, so an end followed directly by another begin does not authorize a copy without that evidence.
using nr::hudcap::kLost;
using nr::hudcap::kScene;
using nr::hudcap::kOverlay;
using nr::hudcap::kCopy;
using CapRec = nr::hudcap::Rec;
struct CapList {
    uint64_t back = 0;                          // the back buffer bound here as a render target (else 0)
    bool dsv = false;                           // a depth-stencil view bound beside it
    uint64_t depth = 0, blend = 0, raster = 0;  // the pipelines bound for those stages (D3D12: its one; D3D10/11: states)
    viewport vp{};
    rect scissor{};
    bool in_pass = false;                       // no copy inside a render pass
    int8_t dynamic_depth = -2, dynamic_blend = -2;
    bool outside_pass = true;
    uint8_t last = 0;                           // its last write on a back buffer
    std::vector<CapRec> recs;                   // its writes on back buffers, in order
    void reset_bindings() {
        back = 0; dsv = false; depth = blend = raster = 0; vp = {}; scissor = {}; in_pass = false; dynamic_depth = dynamic_blend = -2; outside_pass = true;
    }
    void reset() {
        reset_bindings();
        last = 0;
        recs.clear();
    }
};
struct PipeFacts {
    bool depth_dynamic = false, blend_dynamic = false;
    int8_t depth = -1, blend = -1, scissor = -1;   // -1: not set by this pipeline (its default)
};
struct HudCapture {
    std::atomic<bool> armed{false};
    std::atomic<device*> dev{nullptr};             // the session's device: only its lists are followed
    std::atomic<int> api{0};                       // its device_api
    std::atomic<command_list*> immediate{nullptr}; // D3D10/11: the immediate context (feed's list)
    std::atomic<uint64_t> backs[8];                // its swap chains' back buffers
    std::atomic<uint32_t> back_w{0}, back_h{0};
    std::atomic<uint64_t> hudless{0};              // the copy's image (HudFixState::hudless), 0: none yet
    std::atomic<uint32_t> transitions{0}, copies{0};
    std::atomic<uint32_t> last_transition{0};      // D3D10/11: the last frame's last scene -> overlay place
    std::atomic<bool> inpass_overlay{false};
    std::atomic<bool> foreign{false};              // a back-buffer write from a list not followed in order
    std::string earlier_effect;                    // an effect ReShade ran on the frame before DLSS5_Feed this frame
    uint64_t fed_at = ~0ull;                       // the present DLSS5_Feed last ran at (the effects after it: not earlier)
    std::shared_mutex pipes_m;
    std::unordered_map<uint64_t, PipeFacts> pipes; // by pipeline (D3D10/11: a state object)
    std::mutex m;                                  // lists' map, frame, retired
    std::unordered_map<command_list*, CapList> lists;   // never erased (threads keep pointers to the entries)
    std::vector<CapRec> frame;                     // D3D12: the executed lists' writes, in the GPU's order
    std::vector<resource> retired;                 // D3D12 copies' earlier images (a recorded list may still copy into
                                                   // one): freed with the device
} cap;

bool cap_is_back(uint64_t h) {
    if (!h) return false;
    for (const auto& b : cap.backs)
        if (b.load(std::memory_order_relaxed) == h) return true;
    return false;
}
bool cap_vulkan() { return cap.api.load(std::memory_order_relaxed) == int(device_api::vulkan); }
bool cap_explicit() {
    const int a = cap.api.load(std::memory_order_relaxed);
    return a == int(device_api::d3d12) || a == int(device_api::vulkan);
}
// The session's device is D3D10, D3D11 or D3D12 (known from its first feed): the events are followed.
bool cap_followed() {
    const int a = cap.api.load(std::memory_order_relaxed);
    return a == int(device_api::d3d10) || a == int(device_api::d3d11) || a == int(device_api::d3d12) || a == int(device_api::vulkan);
}

// The list's state (a thread's last list is kept at hand: the events of one list come from one thread at a time).
CapList* cap_list(command_list* cl) {
    thread_local command_list* last_cl = nullptr;
    thread_local CapList* last_state = nullptr;
    if (cl == last_cl) return last_state;
    std::lock_guard<std::mutex> g(cap.m);
    CapList* s = &cap.lists[cl];
    last_cl = cl;
    last_state = s;
    return s;
}

PipeFacts cap_facts(uint64_t p) {
    std::shared_lock<std::shared_mutex> g(cap.pipes_m);
    const auto it = cap.pipes.find(p);
    if (it != cap.pipes.end()) return it->second;
    PipeFacts unknown;
    if (cap_vulkan()) unknown.depth=unknown.blend=-2;
    return unknown;
}

// A draw on the bound back buffer: a scene write or an overlay.
uint8_t cap_draw_kind(const CapList& s, uint32_t vertices, uint32_t instances, bool indirect) {
    nr::hudcap::Draw d;
    d.dsv = s.dsv;
    const PipeFacts depth = cap_facts(s.depth);
    d.depth = depth.depth_dynamic ? s.dynamic_depth : depth.depth;
    const PipeFacts blend = cap_facts(s.blend);
    d.blend = blend.blend_dynamic ? s.dynamic_blend : blend.blend;
    d.scissor_on = cap_explicit() || cap_facts(s.raster).scissor == 1;   // D3D12 always cuts by its scissor
    d.vp[0] = s.vp.x; d.vp[1] = s.vp.y; d.vp[2] = s.vp.width; d.vp[3] = s.vp.height;
    d.sc[0] = s.scissor.left; d.sc[1] = s.scissor.top; d.sc[2] = s.scissor.right; d.sc[3] = s.scissor.bottom;
    d.vertices = vertices; d.instances = instances; d.indirect = indirect;
    return nr::hudcap::draw_kind(d, cap.back_w.load(std::memory_order_relaxed), cap.back_h.load(std::memory_order_relaxed));
}

// A write of `kind` on back buffer `b` in `cl` (`state`: what D3D12 has it in for that write): first the copy, where
// an overlay follows a scene write (D3D12: also a list's first write, which may follow an earlier list's).
void cap_write(command_list* cl, CapList& s, uint64_t b, uint8_t kind, resource_usage state) {
    const bool explicit_api = cap_explicit();
    if (!explicit_api && cl != cap.immediate.load(std::memory_order_relaxed)) {
        cap.foreign.store(true, std::memory_order_relaxed);   // a deferred context: not in the immediate one's order
        return;
    }
    if (kind == kOverlay && s.last != kOverlay && (s.last == kScene || explicit_api)) {
        const uint32_t place = cap.transitions.fetch_add(1, std::memory_order_relaxed);
        const bool here = explicit_api || place >= cap.last_transition.load(std::memory_order_relaxed);
        const uint64_t h = cap.hudless.load(std::memory_order_acquire);
        if (here && h && !s.in_pass && cap.copies.fetch_add(1, std::memory_order_relaxed) < nr::hudcap::kMaxCopies) {
            hudfix_copy(cl, resource{b}, state, resource{h}, resource_usage::unordered_access);
            s.recs.push_back({kCopy, b, h});
        }
    }
    if (cap_vulkan() && s.in_pass && kind == kOverlay && s.last == kScene)
        cap.inpass_overlay.store(true, std::memory_order_relaxed);
    if (s.recs.size() >= nr::hudcap::kMaxRecs) {   // never read (no feed): kept short, the frame not trusted
        s.recs.clear();
        s.recs.push_back({kLost, b});
    }
    s.recs.push_back({kind, b});
    s.last = kind;
}

bool cap_full_box(const subresource_box* box) {
    return !box || (box->left == 0 && box->top == 0 && box->right >= cap.back_w.load(std::memory_order_relaxed) &&
                    box->bottom >= cap.back_h.load(std::memory_order_relaxed));
}

void on_cap_bind_targets(command_list* cl, uint32_t count, const resource_view* rtvs, resource_view dsv) {
    if (!cl) return;
    if (!cap_followed()) return;
    CapList* s = cap_list(cl);
    s->back = 0;
    s->dsv = dsv.handle != 0;
    device* d = cap.dev.load(std::memory_order_relaxed);
    if (!cap.armed.load(std::memory_order_relaxed) || !d || cl->get_device() != d) return;
    for (uint32_t i = 0; i < count; ++i) {
        if (!rtvs[i].handle) continue;
        const resource r = rs::call(d, &device::get_resource_from_view, rtvs[i]);
        if (cap_is_back(r.handle)) { s->back = r.handle; break; }
    }
}
bool on_cap_begin_pass(command_list* cl, uint32_t count, const render_pass_render_target_desc* rts,
                       const render_pass_depth_stencil_desc* ds, render_pass_flags flags) {
    if (!cl) return false;
    if (!cap_followed()) return false;
    CapList* s = cap_list(cl);
    s->back = 0;
    s->dsv = ds && ds->view.handle;
    const bool can_copy = s->outside_pass && flags == render_pass_flags::none;
    s->outside_pass = false;
    s->in_pass = true;
    device* d = cap.dev.load(std::memory_order_relaxed);
    if (!cap.armed.load(std::memory_order_relaxed) || !d || cl->get_device() != d) return false;
    for (uint32_t i = 0; i < count; ++i) {
        if (!rts[i].view.handle) continue;
        const resource r = rs::call(d, &device::get_resource_from_view, rts[i].view);
        if (!cap_is_back(r.handle)) continue;
        s->back = r.handle;
        if (cap_vulkan() && can_copy && rts[i].load_op == render_pass_load_op::load) {
            const uint64_t h=cap.hudless.load(std::memory_order_acquire);
            const auto desc=rs::call(d,&device::get_resource_desc,r);
            if (h && (desc.usage & resource_usage::copy_source)!=resource_usage::undefined &&
                cap.copies.fetch_add(1,std::memory_order_relaxed)<nr::hudcap::kMaxCopies) {
                hudfix_copy(cl,r,resource_usage::render_target,resource{h},resource_usage::unordered_access);
                s->recs.push_back({kCopy,r.handle,h});
            }
        }
        if (cap_vulkan() && flags != render_pass_flags::none) s->recs.push_back({nr::hudcap::kUnknown,r.handle});
        if (rts[i].load_op == render_pass_load_op::clear || (cap_vulkan() && rts[i].load_op == render_pass_load_op::discard)) cap_write(cl, *s, r.handle, kScene, resource_usage::render_target);
        break;
    }
    return false;
}
bool on_cap_end_pass(command_list* cl) {
    if (!cl) return false;
    if (!cap_followed()) return false;
    CapList* s = cap_list(cl);
    s->in_pass = false;
    s->outside_pass = false;   // this event may precede NextSubpass, not EndRenderPass
    s->back = 0;
    return false;
}
void on_cap_bind_pipeline(command_list* cl, pipeline_stage stages, pipeline p) {
    if (!cl) return;
    const uint32_t st = uint32_t(stages);
    constexpr uint32_t want = uint32_t(pipeline_stage::depth_stencil) | uint32_t(pipeline_stage::output_merger) |
                              uint32_t(pipeline_stage::rasterizer);
    if (!(st & want) || !cap_followed()) return;
    CapList* s = cap_list(cl);
    if (st & uint32_t(pipeline_stage::depth_stencil)) s->depth = p.handle;
    if (st & uint32_t(pipeline_stage::output_merger)) s->blend = p.handle;
    if (st & uint32_t(pipeline_stage::rasterizer)) s->raster = p.handle;
}
void on_cap_states(command_list* cl, uint32_t count, const dynamic_state* states, const uint32_t* values) {
    if (!cl) return;
    if (!cap_vulkan()) return;
    CapList* s = cap_list(cl);
    for (uint32_t i=0;i<count;++i)
        if (states[i] == dynamic_state::depth_enable) s->dynamic_depth = values[i] ? 1 : 0;
        else if (states[i] == dynamic_state::blend_enable) s->dynamic_blend = values[i] ? 1 : 0;
}
void on_cap_barrier(command_list* cl, uint32_t count, const resource* resources,
                    const resource_usage* before, const resource_usage* after) {
    if (!cl) return;
    if (!cl || !cap_vulkan() || !cap.armed.load(std::memory_order_relaxed) || cl->get_device()!=cap.dev.load()) return;
    CapList* s=cap_list(cl);
    if (s->in_pass) return;
    for(uint32_t i=0;i<count;++i)
        if(before[i]!=after[i]) s->outside_pass=true;   // a layout change cannot occur inside a render pass

}
void on_cap_viewports(command_list* cl, uint32_t first, uint32_t count, const viewport* v) {
    if (!cl) return;
    if (first == 0 && count && cap_followed()) cap_list(cl)->vp = v[0];
}
void on_cap_scissors(command_list* cl, uint32_t first, uint32_t count, const rect* r) {
    if (!cl) return;
    if (first == 0 && count && cap_followed()) cap_list(cl)->scissor = r[0];
}
bool on_cap_draw(command_list* cl, uint32_t vertices, uint32_t instances, uint32_t, uint32_t) {
    if (!cl) return false;
    if (!cap.armed.load(std::memory_order_relaxed)) return false;
    CapList* s = cap_list(cl);
    if (s->back) cap_write(cl, *s, s->back, cap_draw_kind(*s, vertices, instances, false), resource_usage::render_target);
    return false;
}
bool on_cap_draw_indexed(command_list* cl, uint32_t indices, uint32_t instances, uint32_t, int32_t, uint32_t) {
    if (!cl) return false;
    return on_cap_draw(cl, indices, instances, 0, 0);
}
bool on_cap_indirect(command_list* cl, indirect_command type, resource, uint64_t, uint32_t, uint32_t) {
    if (!cl) return false;
    if (!cap.armed.load(std::memory_order_relaxed) || (type != indirect_command::draw && type != indirect_command::draw_indexed))
        return false;
    CapList* s = cap_list(cl);
    if (s->back) cap_write(cl, *s, s->back, cap_draw_kind(*s, 0, 0, true), resource_usage::render_target);
    return false;
}
bool on_cap_clear(command_list* cl, resource_view rtv, const float*, uint32_t rects, const rect*) {
    if (!cl) return false;
    device* d = cap.dev.load(std::memory_order_relaxed);
    if (!cap.armed.load(std::memory_order_relaxed) || !d || cl->get_device() != d) return false;
    const resource r = rs::call(d, &device::get_resource_from_view, rtv);
    if (cap_is_back(r.handle))
        cap_write(cl, *cap_list(cl), r.handle, rects ? kOverlay : kScene, resource_usage::render_target);
    return false;
}
void cap_copy_into(command_list* cl, resource dest, bool whole, resource_usage state = resource_usage::copy_dest) {
    if (!cl) return;
    if (!cap.armed.load(std::memory_order_relaxed)) return;
    if (cap_vulkan()) cap_list(cl)->outside_pass=true;   // Vulkan copy/resolve commands are outside render passes
    if (!cap_is_back(dest.handle)) return;
    cap_write(cl, *cap_list(cl), dest.handle, whole ? kScene : kOverlay, state);
}
bool on_cap_copy_resource(command_list* cl, resource, resource dest) {
    if (!cl) return false; cap_copy_into(cl, dest, true); return false; }
bool on_cap_copy_texture(command_list* cl, resource, uint32_t, const subresource_box*, resource dest, uint32_t,
                         const subresource_box* dest_box, filter_mode) {
    if (!cl) return false;
    cap_copy_into(cl, dest, cap_full_box(dest_box));
    return false;
}
bool on_cap_copy_buffer(command_list* cl, resource, uint64_t, uint32_t, uint32_t, resource dest, uint32_t,
                        const subresource_box* dest_box) {
    if (!cl) return false;
    cap_copy_into(cl, dest, cap_full_box(dest_box));
    return false;
}
bool on_cap_resolve(command_list* cl, resource, uint32_t, const subresource_box* src_box, resource dest, uint32_t,
                    uint32_t x, uint32_t y, uint32_t, format) {
    if (!cl) return false;
    cap_copy_into(cl, dest, x == 0 && y == 0 && cap_full_box(src_box), resource_usage::resolve_dest);
    return false;
}
// D3D10/11's UpdateSubresource (a device's event): on the immediate context.
bool on_cap_update(device* d, const subresource_data&, resource dest, uint32_t, const subresource_box* box) {
    command_list* imm = cap.immediate.load(std::memory_order_relaxed);
    if (!imm || d != cap.dev.load(std::memory_order_relaxed) || cap_explicit()) return false;
    cap_copy_into(imm, dest, cap_full_box(box));
    return false;
}
void on_cap_execute(command_queue*, command_list* cl) {
    if (!cl) return;
    if (!cap_explicit()) return;
    CapList* s = cap_list(cl);
    std::lock_guard<std::mutex> g(cap.m);
    // its records stay until the list is reset: executed again, its copies run again
    if (cap.frame.size() < 16384) cap.frame.insert(cap.frame.end(), s->recs.begin(), s->recs.end());
}
void on_cap_execute_secondary(command_list*, command_list* secondary) {
    if (!secondary) return;
    if (!cap.armed.load(std::memory_order_relaxed)) return;
    // a bundle's (D3D12) or deferred context's (D3D10/11) writes are not followed: they make the frame not trusted
    CapList* s = cap_list(secondary);
    if (!s->recs.empty()) cap.foreign.store(true, std::memory_order_relaxed);
}
// D3D12's Reset: the list starts again. D3D10/11 (ClearState): its bindings only, the frame's writes stay.
void on_cap_reset_list(command_list* cl) {
    if (!cl) return;
    if (!cap_followed()) return;
    if (cap_explicit()) cap_list(cl)->reset();
    else cap_list(cl)->reset_bindings();
}
void on_cap_destroy_list(command_list* cl) {
    if (!cl) return;
    nr::gpu::reset(cl);
    std::lock_guard<std::mutex> g(cap.m);
    const auto it = cap.lists.find(cl);
    if (it != cap.lists.end()) it->second.reset();
}
void on_cap_init_pipeline(device* dev, pipeline_layout, uint32_t count, const pipeline_subobject* so, pipeline p) {
    PipeFacts f;
    if (dev->get_api()==device_api::vulkan) f.depth=f.blend=-2;
    bool any = false;
    for (uint32_t i = 0; i < count; ++i) {
        if (!so[i].data) continue;
        switch (so[i].type) {
        case pipeline_subobject_type::blend_state:
            f.blend = static_cast<const blend_desc*>(so[i].data)->blend_enable[0] ? 1 : 0; any = true; break;
        case pipeline_subobject_type::rasterizer_state:
            f.scissor = static_cast<const rasterizer_desc*>(so[i].data)->scissor_enable ? 1 : 0; any = true; break;
        case pipeline_subobject_type::depth_stencil_state:
            f.depth = static_cast<const depth_stencil_desc*>(so[i].data)->depth_enable ? 1 : 0; any = true; break;
        default: break;
        }
    }
    for (uint32_t i=0;i<count;++i) if (so[i].type==pipeline_subobject_type::dynamic_pipeline_states && so[i].data) {
        const auto* states=static_cast<const dynamic_state*>(so[i].data);
        for(uint32_t j=0;j<so[i].count;++j) {
            f.depth_dynamic |= states[j]==dynamic_state::depth_enable;
            f.blend_dynamic |= states[j]==dynamic_state::blend_enable;
        }
    }
    if (!any) return;
    std::unique_lock<std::shared_mutex> g(cap.pipes_m);
    cap.pipes[p.handle] = f;
}
void on_cap_destroy_pipeline(device*, pipeline p) {
    std::unique_lock<std::shared_mutex> g(cap.pipes_m);
    cap.pipes.erase(p.handle);
}
// The back buffers HUD FIX's capture looks for (every swap chain's: the session's device is known at its first feed).
void cap_swapchain(swapchain* sc, bool add) {
    device* d = sc->get_device();
    const uint32_t n = sc->get_back_buffer_count();
    for (uint32_t i = 0; i < n; ++i) {
        const resource r = rs::call(sc, &swapchain::get_back_buffer, i);
        if (!r.handle) continue;
        if (add && i == 0) {
            const resource_desc desc = rs::call(d, &device::get_resource_desc, r);
            cap.back_w.store(desc.texture.width);
            cap.back_h.store(desc.texture.height);
        }
        for (auto& b : cap.backs) {
            uint64_t want = add ? 0 : r.handle;
            if (add && b.load() == r.handle) break;
            if (b.compare_exchange_strong(want, add ? r.handle : 0)) break;
        }
    }
}
void on_cap_destroy_swapchain(swapchain* sc, bool) { cap_swapchain(sc, false); }

// The frame's verdict at its feed (`colour` the back buffer fed, nr_hud_capture.hpp verdict); its writes are taken (the
// next frame starts empty).
using CapVerdict = nr::hudcap::Verdict;
CapVerdict cap_verdict(uint64_t colour, std::string& why) {
    std::vector<CapRec> recs;
    {
        std::lock_guard<std::mutex> g(cap.m);
        if (cap_explicit()) recs.swap(cap.frame);
        else if (command_list* imm = cap.immediate.load()) {
            CapList& s = cap.lists[imm];
            recs.swap(s.recs);
            s.last = 0;
        }
    }
    const uint32_t places = cap.transitions.exchange(0);
    cap.last_transition.store(places ? places - 1 : 0);
    cap.copies.store(0);
    const std::string earlier = cap.earlier_effect;
    cap.earlier_effect.clear();
    cap.fed_at = presents;
    const CapVerdict v = nr::hudcap::verdict(recs, colour, cap.foreign.exchange(false), cap.hudless.load(), why);
    const bool inpass=cap.inpass_overlay.exchange(false);
    if (v==CapVerdict::kMissed && cap_vulkan() && inpass)
        why="the HUD followed scene draws without a capturable render-pass boundary";
    if (v == CapVerdict::kCaptured && !earlier.empty()) {
        why = earlier + " runs before DLSS5_Feed (it changes the frame after the copy): move DLSS5_Feed above it";
        return CapVerdict::kMissed;
    }
    return v;
}

// The images and the stage go (the device going, a new size or format): after the GPU is done with them. D3D12's
// capture image is kept until the device goes (`final`): a list the game recorded may still copy into it.
void hudfix_release(bool keep_stage, bool final = false) {
    HudFixState& H = hudfix;
    const bool any = H.back[0].handle || H.back[1].handle || H.out.handle || H.alpha.handle || H.stats.handle ||
                     H.hudless.handle || (!keep_stage && H.stage);
    if (any && session && !session->wait_idle()) {
        // NR's GPU work did not finish in time: nothing it may still use is freed (kept for the run; said once)
        static bool said = false;
        if (!said) { said = true; log("[nr] HUD FIX: NR's GPU work did not finish in time; its images are kept, not freed"); }
        cap.hudless.store(0);
        for (resource* r : {&H.back[0], &H.back[1], &H.out, &H.alpha, &H.stats, &H.hudless}) *r = {};
        if (!keep_stage) (void)H.stage.release();
        H.dev = nullptr;
        H.w = H.h = 0;
        H.prev_valid = false;
        H.fresh = true;
        return;
    }
    cap.hudless.store(0);
    if (H.dev && H.hudless.handle && !final && (H.dev->get_api() == device_api::d3d12 || H.dev->get_api() == device_api::vulkan)) {
        std::lock_guard<std::mutex> g(cap.m);
        cap.retired.push_back(H.hudless);
        H.hudless = {};
    }
    if (H.dev)
        for (resource* r : {&H.back[0], &H.back[1], &H.out, &H.alpha, &H.stats, &H.hudless})
            if (r->handle) retire_effect_resource(H.dev, *r);
    if (H.dev && final) {
        std::lock_guard<std::mutex> g(cap.m);
        for (const resource& r : cap.retired) retire_effect_resource(H.dev, r);
        cap.retired.clear();
    }
    for (resource* r : {&H.back[0], &H.back[1], &H.out, &H.alpha, &H.stats, &H.hudless}) *r = {};
    if (!keep_stage) { H.stage.reset(); H.stage_failed = false; }
    H.dev = nullptr;
    H.w = H.h = 0;
    H.prev_valid = false;
    H.fresh = true;
}

// The images for a w x h back buffer of format `f`, every one storage + sampled + copied, kept unordered_access.
bool hudfix_ensure(device* dev, unsigned w, unsigned h, format f) {
    HudFixState& H = hudfix;
    const format typed = format_to_default_typed(f, 0);   // a storage view: never sRGB (the copies move the bits)
    if (H.dev == dev && H.back[0].handle && H.w == w && H.h == h && H.fmt == typed) return true;
    if (H.failed && H.dev == nullptr && H.w == w && H.h == h && H.fmt == typed) return false;
    hudfix_release(H.stage && H.dev == dev);
    const resource_usage all = resource_usage::shader_resource | resource_usage::unordered_access |
                               resource_usage::copy_source | resource_usage::copy_dest;
    const auto make = [&](unsigned width, unsigned height, format fm, resource* r) {
        return make_resource(dev, resource_desc(width, height, 1, 1, fm, 1, memory_heap::gpu_only, all), nullptr,
                                    resource_usage::unordered_access, r);
    };
    if (!make(w, h, typed, &H.back[0]) || !make(w, h, typed, &H.back[1]) || !make(w, h, typed, &H.out) ||
        !make(w, h, typed, &H.hudless) || !make(w, h, format::r16g16b16a16_float, &H.alpha) ||
        !make(hudfix_stats_w(w), hudfix_stats_h(h), format::r32_uint, &H.stats)) {
        for (resource* r : {&H.back[0], &H.back[1], &H.out, &H.alpha, &H.stats, &H.hudless})
            if (r->handle) retire_effect_resource(dev, *r);
        for (resource* r : {&H.back[0], &H.back[1], &H.out, &H.alpha, &H.stats, &H.hudless}) *r = {};
        H.failed = true;
        H.w = w; H.h = h; H.fmt = typed;
        hudfix_say("its images (format " + std::to_string(unsigned(typed)) + ", storage) could not be made; it does not run");
        return false;
    }
    H.dev = dev; H.w = w; H.h = h; H.fmt = typed;
    H.failed = false;
    H.cur = 0; H.parity = 0;
    H.prev_valid = false;
    H.fresh = true;
    cap.hudless.store(H.hudless.handle, std::memory_order_release);
    return true;
}

// This frame's plan from [HudFix] Mode and the capture (`api` the device's; the capture is armed for the next frame).
HudPlan hudfix_plan(device* dev, command_list* cl, uint64_t colour) {
    const uint32_t mode = chain.hud.mode;
    const device_api api = dev->get_api();
    const bool can_capture = api == device_api::d3d10 || api == device_api::d3d11 || api == device_api::d3d12 || api == device_api::vulkan;
    const bool armed_before = cap.armed.load();
    cap.dev.store(dev);
    cap.api.store(int(api));
    if (api == device_api::d3d10 || api == device_api::d3d11) cap.immediate.store(cl);
    cap.armed.store(can_capture && (mode == 1 || mode == 3));
    HudPlan p;
    std::string why;
    CapVerdict v = CapVerdict::kMissed;
    if (armed_before && cap.armed.load()) v = cap_verdict(colour, why);
    else {
        why = "it starts with the next frame";
        if (armed_before) (void)cap_verdict(colour, why);   // switched off: its writes go
    }
    switch (mode) {
    case 1:   // Auto
        p.run = p.estimate = true;
        p.captured = v == CapVerdict::kCaptured;
        hudfix_now(p.captured ? "captured before the HUD"
                   : can_capture ? "estimated (captured: " + why + ")"
                                 : "estimated (Captured: unsupported graphics API)");
        break;
    case 3:   // Captured
        p.run = p.captured = v == CapVerdict::kCaptured;
        hudfix_now(!can_capture ? "nothing: Captured does not run on this graphics API"
                   : p.captured ? "captured before the HUD" : "nothing this frame: " + why);
        break;
    case 4:
        p.run = p.estimate = true;
        hudfix_now("estimated");
        break;
    case 2:
        hudfix_now("nothing: this route has no frame generation's frames");
        break;
    default:
        hudfix_now("off");
        break;
    }
    return p;
}

// Before the layers: the frame kept (the back buffer, render_target); for the capture, the copy put in its place.
bool hudfix_begin(device* dev, command_list* cl, resource colour, unsigned w, unsigned h, format f, const HudPlan& p) {
    const uint32_t mode = chain.hud.mode;
    if (mode != 1 && mode != 3 && mode != 4) {
        if (hudfix.back[0].handle) hudfix_release(true);   // switched off: the images go, the stage stays
        return false;
    }
    if (!hudfix_ensure(dev, w, h, f)) return false;
    HudFixState& H = hudfix;
    H.frame = presents;   // the Present's number: a gap of presents without the estimate is one (feed not run too)
    if (!p.run) return false;
    if (p.estimate && H.estimated_at + 1 != H.frame) {   // after a gap (off, Captured only): no stale confidence
        H.fresh = true;
        H.prev_valid = false;
    }
    const resource_usage rt = resource_usage::render_target, ua = resource_usage::unordered_access;
    hudfix_copy(cl, colour, rt, H.back[H.cur], ua);
    if (!H.prev_valid) hudfix_copy(cl, colour, rt, H.back[H.cur ^ 1], ua);   // the first: nothing moved yet
    if (p.captured) hudfix_copy(cl, H.hudless, ua, colour, rt);
    return true;
}

// The dispatches, the images as `image` gives them.
template <class ImageOf>
bool hudfix_record(VkCommandBuffer vk, const nr::HostDevice& host, const ImageOf& image, const HudPlan& p,
                   const nr::wilsjo2::Image& motion = {}) {
    HudFixState& H = hudfix;
    namespace w2 = nr::wilsjo2;
    if (!H.stage) {
        if (H.stage_failed) return false;
        try {
            H.stage = std::make_unique<w2::Stage>(host, std::string(nr::pe::module_folder()) + "\\dlssnr-amd\\shaders\\wilsjo2");
        } catch (const std::exception& x) {
            H.stage_failed = true;
            hudfix_say(std::string("its programs could not be made: ") + x.what());
            return false;
        }
    }
    const w2::Image back = image(H.back[H.cur]), prev = image(H.back[H.cur ^ 1]), out = image(H.out),
                    alpha = image(H.alpha), stats = image(H.stats), hudless = image(H.hudless);
    for (const w2::Image* i : {&back, &prev, &out, &alpha, &stats, &hudless})
        if (!i->image || !i->format) { hudfix_say("an image of its could not be seen in Vulkan; it does not run"); return false; }
    for (const w2::Image* i : {&out, &alpha, &stats})
        if (i->layout != VK_IMAGE_LAYOUT_GENERAL) {
            hudfix_say("an image of its is not in a storage layout (" + std::to_string(unsigned(i->layout)) + "); it does not run");
            return false;
        }
    const auto run = [&](uint32_t mode, uint32_t source, const w2::Image& t0, const w2::Image* t1, const w2::Image* t2,
                         const w2::Image* t3, const w2::Image& u0, uint32_t width, uint32_t height) {
        w2::Dispatch d;
        d.t0 = t0;
        if (t1) d.t1 = *t1;
        if (t2) d.t2 = *t2;
        if (t3) d.t3 = *t3;
        d.u0 = u0;
        d.u1 = stats;
        d.constants = w2::hudfix_constants(mode, width, height, source, chain.hud.show_mask, H.fresh, H.parity);
        if (mode == w2::kHudEstimate && motion.image && motion.format) {
            // the estimate with the motion vectors the layers get (DLSS5_MV: back-buffer pixels whatever its size,
            // prev = p + mv), read across the whole texture as the layers read it
            d.t2 = motion;
            w2::hudfix_motion(d.constants, 1.0f, 1.0f, 0u, 0u, float(motion.width) / float(width),
                              float(motion.height) / float(height));
        }
        H.stage->record(vk, d);
    };
    try {
        run(w2::kHudClear, 4, back, nullptr, nullptr, nullptr, out, 1, 1);
        if (p.estimate) {   // the estimate's state, this frame against the last
            run(w2::kHudCount, 4, back, &prev, nullptr, nullptr, out, H.w, H.h);
            run(w2::kHudEstimate, 4, back, &prev, nullptr, nullptr, alpha, H.w, H.h);
        }
        if (p.captured) {   // the frame against its copy before the HUD
            run(w2::kHudCount, 2, back, &hudless, nullptr, nullptr, out, H.w, H.h);
            run(w2::kHudCompose, 2, back, &hudless, &out, &alpha, out, H.w, H.h);
        } else {
            run(w2::kHudCompose, 4, back, &prev, &out, &alpha, out, H.w, H.h);
        }
    } catch (const std::exception& x) {
        hudfix_say(std::string("recording failed: ") + x.what());
        return false;
    }
    return true;
}

// After the layers: their answer (the back buffer) composed over the kept frame and put back; this frame becomes the
// estimate's last.
void hudfix_end(device* dev, command_list* cl, resource colour, const HudPlan& p, resource motion = {}) {
    HudFixState& H = hudfix;
    if (!H.back[0].handle || !session) return;
    const resource_usage rt = resource_usage::render_target, ua = resource_usage::unordered_access;
    hudfix_copy(cl, colour, rt, H.out, ua);
    bool recorded = false;
    switch (dev->get_api()) {
    case device_api::d3d10:
    case device_api::d3d11:
        recorded = session->record_raw_d3d11(reinterpret_cast<IUnknown*>(dev->get_native()),
            [&](VkCommandBuffer vk, const nr::HostDevice& host, const nr::pe::Session::RawLookup11& lookup) {
                const auto image = [&](resource r) {
                    const auto i = lookup(reinterpret_cast<IUnknown*>(r.handle));
                    nr::wilsjo2::Image o;
                    o.image = i.image; o.format = i.format; o.width = i.width; o.height = i.height; o.layout = i.layout;
                    return o;
                };
                return hudfix_record(vk, host, image, p, motion.handle && p.estimate ? image(motion) : nr::wilsjo2::Image{});
            });
        break;
    case device_api::d3d12:
        recorded = session->record_raw(reinterpret_cast<ID3D12Device*>(dev->get_native()),
            reinterpret_cast<ID3D12GraphicsCommandList*>(cl->get_native()),
            [&](VkCommandBuffer vk, const nr::HostDevice& host, const nr::pe::Session::RawLookup& lookup) {
                nr::wilsjo2::Image mv;
                if (motion.handle && p.estimate) {   // DLSS5_MV as the layers read it (pixel-shader resource)
                    const auto i = lookup(reinterpret_cast<ID3D12Resource*>(motion.handle), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, false);
                    mv.image = i.image; mv.format = i.format; mv.width = i.width; mv.height = i.height; mv.layout = i.layout;
                }
                return hudfix_record(vk, host, [&](resource r) {
                    const auto i = lookup(reinterpret_cast<ID3D12Resource*>(r.handle), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
                    nr::wilsjo2::Image o;
                    o.image = i.image; o.format = i.format; o.width = i.width; o.height = i.height; o.layout = i.layout;
                    return o;
                }, p, mv);
            });
        break;
    case device_api::vulkan:
        recorded = session->record_raw_vk(reinterpret_cast<VkCommandBuffer>(static_cast<uintptr_t>(cl->get_native())),
            [&](VkCommandBuffer vk, const nr::HostDevice& host) {
                nr::wilsjo2::Image mv;
                if (motion.handle && p.estimate) {   // DLSS5_MV as the layers read it
                    const resource_desc md = rs::call(dev, &device::get_resource_desc, motion);
                    mv.image = (VkImage)motion.handle;
                    mv.format = nr::pe::vulkan_format_of(static_cast<DXGI_FORMAT>(md.texture.format));
                    mv.width = md.texture.width; mv.height = md.texture.height;
                    mv.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                }
                return hudfix_record(vk, host, [&](resource r) {
                    const resource_desc d = rs::call(dev, &device::get_resource_desc, r);
                    nr::wilsjo2::Image o;
                    o.image = (VkImage)r.handle;
                    o.format = nr::pe::vulkan_format_of(static_cast<DXGI_FORMAT>(d.texture.format));
                    o.width = d.texture.width; o.height = d.texture.height;
                    o.layout = VK_IMAGE_LAYOUT_GENERAL;   // unordered_access, as ReShade's barriers leave it
                    return o;
                }, p, mv);
            });
        break;
    default:
        break;
    }
    if (H.stage) H.stage->end_recording();
    if (!recorded) {
        // the layers' answer stays as it is - unless it was made on the copy without the HUD: then the frame as the
        // game drew it goes back (its HUD with it), without NR this frame
        if (p.captured) hudfix_copy(cl, H.back[H.cur], ua, colour, rt);
        hudfix_now(std::string("nothing this frame: its composition could not be recorded") + (H.said.empty() ? "" : " (" + H.said + ")"));
        return;
    }
    hudfix_copy(cl, H.out, ua, colour, rt);
    H.said.clear();
    if (p.estimate) {
        H.fresh = false;
        H.parity ^= 1u;
        H.estimated_at = H.frame;
    }
    H.cur ^= 1u;
    H.prev_valid = true;
}

// One recording of our own passes as raw Vulkan through the session, on images made through ReShade (kept
// unordered_access): `rec(vk, host, image_of)` with image_of(resource) -> nr::wilsjo2::Image (D3D12 resources looked up
// in UNORDERED_ACCESS). As HUD FIX's recording (hudfix_end); for the comparison's fitted view and the HDR10 conversion.
template <class Rec>
bool raw_pass(device* dev, command_list* cl, Rec&& rec) {
    namespace w2 = nr::wilsjo2;
    switch (dev->get_api()) {
    case device_api::d3d10:
    case device_api::d3d11:
        return session->record_raw_d3d11(reinterpret_cast<IUnknown*>(dev->get_native()),
            [&](VkCommandBuffer vk, const nr::HostDevice& host, const nr::pe::Session::RawLookup11& lookup) {
                return rec(vk, host, [&](resource r) {
                    const auto i = lookup(reinterpret_cast<IUnknown*>(r.handle));
                    w2::Image o;
                    o.image = i.image; o.format = i.format; o.width = i.width; o.height = i.height; o.layout = i.layout;
                    return o;
                });
            });
    case device_api::d3d12:
        return session->record_raw(reinterpret_cast<ID3D12Device*>(dev->get_native()),
            reinterpret_cast<ID3D12GraphicsCommandList*>(cl->get_native()),
            [&](VkCommandBuffer vk, const nr::HostDevice& host, const nr::pe::Session::RawLookup& lookup) {
                return rec(vk, host, [&](resource r) {
                    const auto i = lookup(reinterpret_cast<ID3D12Resource*>(r.handle), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
                    w2::Image o;
                    o.image = i.image; o.format = i.format; o.width = i.width; o.height = i.height; o.layout = i.layout;
                    return o;
                });
            });
    case device_api::vulkan:
        return session->record_raw_vk(reinterpret_cast<VkCommandBuffer>(static_cast<uintptr_t>(cl->get_native())),
            [&](VkCommandBuffer vk, const nr::HostDevice& host) {
                return rec(vk, host, [&](resource r) {
                    const resource_desc d = rs::call(dev, &device::get_resource_desc, r);
                    w2::Image o;
                    o.image = (VkImage)r.handle;
                    o.format = nr::pe::vulkan_format_of(static_cast<DXGI_FORMAT>(d.texture.format));
                    o.width = d.texture.width; o.height = d.texture.height;
                    o.layout = VK_IMAGE_LAYOUT_GENERAL;   // unordered_access, as ReShade's barriers leave it
                    return o;
                });
            });
    default:
        return false;
    }
}

// ---- [Inspect] side by side, the whole frame in each half (Zoom below 2) ------------------------------------------
// Copies cannot scale (ReShade copies regions of one size on every API), so this one view is a pass of its own:
// wilsjo2_inspect.comp (as the .asi's comparison) recorded as raw Vulkan through the session, as HUD FIX's passes are,
// on images made through ReShade (storage + sampled + copied, kept unordered_access). Zoom 2 stays the copies'
// (inspect_show: the frame's middle half at 1:1, which is what the pass gives at 2 too).
struct InspectFit {
    device* dev = nullptr;
    resource off{}, on{}, out{};
    unsigned w = 0, h = 0;
    format fmt{};
    bool failed = false;
    std::unique_ptr<nr::wilsjo2::Stage> stage;
    bool stage_failed = false;
    std::string said;
};
InspectFit inspect_fit{};

void inspect_fit_say(const std::string& why) {
    if (why == inspect_fit.said) return;
    inspect_fit.said = why;
    log("[nr] NR comparison (side by side, fitted): %s", why.c_str());
}

void inspect_fit_release(bool keep_stage) {
    InspectFit& F = inspect_fit;
    const bool any = F.off.handle || F.on.handle || F.out.handle || (!keep_stage && F.stage);
    if (any && session && !session->wait_idle()) {   // NR's GPU work did not finish in time: nothing it may use is freed
        for (resource* r : {&F.off, &F.on, &F.out}) *r = {};
        if (!keep_stage) (void)F.stage.release();
        F.dev = nullptr;
        F.w = F.h = 0;
        return;
    }
    if (F.dev)
        for (resource* r : {&F.off, &F.on, &F.out})
            if (r->handle) retire_effect_resource(F.dev, *r);
    for (resource* r : {&F.off, &F.on, &F.out}) *r = {};
    if (!keep_stage) { F.stage.reset(); F.stage_failed = false; }
    F.dev = nullptr;
    F.w = F.h = 0;
}

bool inspect_fit_ensure(device* dev, unsigned w, unsigned h, format f) {
    InspectFit& F = inspect_fit;
    const format typed = format_to_default_typed(f, 0);   // a storage view: never sRGB (the copies move the bits)
    if (F.dev == dev && F.off.handle && F.w == w && F.h == h && F.fmt == typed) return true;
    if (F.failed && F.dev == nullptr && F.w == w && F.h == h && F.fmt == typed) return false;
    inspect_fit_release(F.stage && F.dev == dev);
    const resource_usage all = resource_usage::shader_resource | resource_usage::unordered_access |
                               resource_usage::copy_source | resource_usage::copy_dest;
    const auto make = [&](resource* r) {
        return make_resource(dev, resource_desc(w, h, 1, 1, typed, 1, memory_heap::gpu_only, all), nullptr,
                                    resource_usage::unordered_access, r);
    };
    if (!make(&F.off) || !make(&F.on) || !make(&F.out)) {
        for (resource* r : {&F.off, &F.on, &F.out}) { if (r->handle) retire_effect_resource(dev, *r); *r = {}; }
        F.failed = true;
        F.w = w; F.h = h; F.fmt = typed;
        inspect_fit_say("its images (format " + std::to_string(unsigned(typed)) + ", storage) could not be made");
        return false;
    }
    F.dev = dev; F.w = w; F.h = h; F.fmt = typed;
    F.failed = false;
    return true;
}

// The back buffer (render_target) shown side by side from `off` (inspect_copies.off, copy_dest) and itself, the whole
// frame in each half at Zoom `zoom`. False when the pass could not run (the caller shows the copies' view instead).
bool inspect_fit_show(device* dev, command_list* cl, resource colour, resource off, const nr::pe::InspectConfig& ic,
                      unsigned w, unsigned h, format f) {
    InspectFit& F = inspect_fit;
    if (!session || !inspect_fit_ensure(dev, w, h, f)) return false;
    const resource_usage rt = resource_usage::render_target, ua = resource_usage::unordered_access,
                         cd = resource_usage::copy_dest;
    hudfix_copy(cl, off, cd, F.off, ua);
    hudfix_copy(cl, colour, rt, F.on, ua);
    namespace w2 = nr::wilsjo2;
    const auto record = [&](VkCommandBuffer vk, const nr::HostDevice& host, const auto& image) {
        if (!F.stage) {
            if (F.stage_failed) return false;
            try {
                F.stage = std::make_unique<w2::Stage>(host, std::string(nr::pe::module_folder()) + "\\dlssnr-amd\\shaders\\wilsjo2");
            } catch (const std::exception& x) {
                F.stage_failed = true;
                inspect_fit_say(std::string("its program could not be made: ") + x.what());
                return false;
            }
        }
        w2::Dispatch d;
        d.t0 = image(F.off);
        d.t1 = image(F.on);
        d.u0 = image(F.out);
        for (const w2::Image* i : {&d.t0, &d.t1, &d.u0})
            if (!i->image || !i->format) { inspect_fit_say("an image of its could not be seen in Vulkan"); return false; }
        if (d.u0.layout != VK_IMAGE_LAYOUT_GENERAL) { inspect_fit_say("its output is not in a storage layout"); return false; }
        d.constants.mode = w2::kInspect;
        d.constants.width = w;
        d.constants.height = h;
        d.constants.compare_mode = 1u;
        d.constants.compare_zoom = std::clamp(ic.zoom, 1.0f, 2.0f);
        d.constants.compare_swap = ic.swap ? 1u : 0u;
        d.constants.apply_model = 1u;
        try {
            F.stage->record(vk, d);
        } catch (const std::exception& x) {
            inspect_fit_say(std::string("recording failed: ") + x.what());
            return false;
        }
        return true;
    };
    const bool recorded = raw_pass(dev, cl, record);
    if (F.stage) F.stage->end_recording();
    if (!recorded) return false;
    hudfix_copy(cl, F.out, ua, colour, rt);
    F.said.clear();
    return true;
}

// ---- HDR10 (PQ, BT.2020) swap chains for wilsjo2 layers --------------------------------------------------------------
// The wilsjo2 processing has no PQ decode, so as the .asi does at Present (wilsjo2_convert): the back buffer decoded into
// linear BT.709 in scRGB units (wilsjo2_finished.comp mode 20) on an RGBA16F image of ours, the layer run on that image
// as on an scRGB frame (its linear path), and the answer encoded back into PQ (mode 21; a non-finite pixel keeps the
// original) and copied into the back buffer. `lin` is kept render_target, as the back buffer the layer's run expects;
// `src` (the back buffer's copy) and `out` unordered_access.
struct PqState {
    device* dev = nullptr;
    resource src{}, lin{}, out{};
    resource_desc lin_desc{};
    unsigned w = 0, h = 0;
    format fmt{};
    bool failed = false;
    std::unique_ptr<nr::wilsjo2::Stage> stage;
    bool stage_failed = false;
    std::string said;
};
PqState pq{};

void pq_say(const std::string& why) {
    if (why == pq.said) return;
    pq.said = why;
    log("[nr] wilsjo2 on an HDR10 swap chain: %s", why.c_str());
}

void pq_release(bool keep_stage) {
    PqState& Q = pq;
    const bool any = Q.src.handle || Q.lin.handle || Q.out.handle || (!keep_stage && Q.stage);
    if (any && session && !session->wait_idle()) {   // NR's GPU work did not finish in time: nothing it may use is freed
        for (resource* r : {&Q.src, &Q.lin, &Q.out}) *r = {};
        if (!keep_stage) (void)Q.stage.release();
        Q.dev = nullptr;
        Q.w = Q.h = 0;
        return;
    }
    if (Q.dev)
        for (resource* r : {&Q.src, &Q.lin, &Q.out})
            if (r->handle) retire_effect_resource(Q.dev, *r);
    for (resource* r : {&Q.src, &Q.lin, &Q.out}) *r = {};
    if (!keep_stage) { Q.stage.reset(); Q.stage_failed = false; }
    Q.dev = nullptr;
    Q.w = Q.h = 0;
}

bool pq_ensure(device* dev, unsigned w, unsigned h, format f) {
    PqState& Q = pq;
    const format typed = format_to_default_typed(f, 0);
    if (Q.dev == dev && Q.src.handle && Q.w == w && Q.h == h && Q.fmt == typed) return true;
    if (Q.failed && Q.dev == nullptr && Q.w == w && Q.h == h && Q.fmt == typed) return false;
    pq_release(Q.stage && Q.dev == dev);
    const resource_usage all = resource_usage::shader_resource | resource_usage::unordered_access |
                               resource_usage::copy_source | resource_usage::copy_dest;
    const resource_desc lin(w, h, 1, 1, format::r16g16b16a16_float, 1, memory_heap::gpu_only,
                            all | resource_usage::render_target);
    const auto make = [&](const resource_desc& d, resource_usage state, resource* r) {
        return make_resource(dev, d, nullptr, state, r);
    };
    const resource_desc same(w, h, 1, 1, typed, 1, memory_heap::gpu_only, all);
    if (!make(same, resource_usage::unordered_access, &Q.src) || !make(lin, resource_usage::render_target, &Q.lin) ||
        !make(same, resource_usage::unordered_access, &Q.out)) {
        for (resource* r : {&Q.src, &Q.lin, &Q.out}) { if (r->handle) retire_effect_resource(dev, *r); *r = {}; }
        Q.failed = true;
        Q.w = w; Q.h = h; Q.fmt = typed;
        pq_say("its images (format " + std::to_string(unsigned(typed)) + " and RGBA16F) could not be made");
        return false;
    }
    Q.dev = dev; Q.w = w; Q.h = h; Q.fmt = typed; Q.lin_desc = lin;
    Q.failed = false;
    return true;
}

// One conversion pass (mode 20: src -> lin; 21: lin + src -> out), `lin` moved to unordered_access around it.
bool pq_pass(device* dev, command_list* cl, uint32_t mode) {
    PqState& Q = pq;
    namespace w2 = nr::wilsjo2;
    const resource_usage rt = resource_usage::render_target, ua = resource_usage::unordered_access;
    cl->barrier(1, &Q.lin, &rt, &ua);
    const bool recorded = raw_pass(dev, cl, [&](VkCommandBuffer vk, const nr::HostDevice& host, const auto& image) {
        if (!Q.stage) {
            if (Q.stage_failed) return false;
            try {
                Q.stage = std::make_unique<w2::Stage>(host, std::string(nr::pe::module_folder()) + "\\dlssnr-amd\\shaders\\wilsjo2");
            } catch (const std::exception& x) {
                Q.stage_failed = true;
                pq_say(std::string("its program could not be made: ") + x.what());
                return false;
            }
        }
        w2::Dispatch d;
        d.t0 = image(mode == w2::kFinishedToLinear ? Q.src : Q.lin);
        if (mode == w2::kFinishedToPq) d.t2 = image(Q.src);
        d.u0 = image(mode == w2::kFinishedToLinear ? Q.lin : Q.out);
        if (!d.t0.image || !d.t0.format || !d.u0.image || !d.u0.format || (mode == w2::kFinishedToPq && !d.t2.format)) {
            pq_say("an image of its could not be seen in Vulkan");
            return false;
        }
        d.constants.mode = mode;
        d.constants.width = Q.w;
        d.constants.height = Q.h;
        try {
            Q.stage->record(vk, d);
        } catch (const std::exception& x) {
            pq_say(std::string("recording failed: ") + x.what());
            return false;
        }
        return true;
    });
    if (Q.stage) Q.stage->end_recording();
    cl->barrier(1, &Q.lin, &ua, &rt);
    return recorded;
}

// Before the layer: the back buffer (render_target) decoded into `lin`. False: the layer cannot run on this frame.
bool pq_begin(device* dev, command_list* cl, resource colour, unsigned w, unsigned h, format f) {
    if (!session || !pq_ensure(dev, w, h, f)) return false;
    hudfix_copy(cl, colour, resource_usage::render_target, pq.src, resource_usage::unordered_access);
    return pq_pass(dev, cl, nr::wilsjo2::kFinishedToLinear);
}
// After it ran: `lin` encoded back and put into the back buffer. False: the back buffer is left as it was.
bool pq_end(device* dev, command_list* cl, resource colour) {
    if (!pq_pass(dev, cl, nr::wilsjo2::kFinishedToPq)) return false;
    hudfix_copy(cl, pq.out, resource_usage::unordered_access, colour, resource_usage::render_target);
    pq.said.clear();
    return true;
}

// ---- Wine's d3dcompiler_47 and `[fastopt]` ------------------------------------
//
// ReShade turns an effect's `[loop]` into `[fastopt]` in the HLSL it generates for shader model 4 and up
// (effect_codegen_hlsl.cpp), and Wine's d3dcompiler_47 (vkd3d-shader) rejects that attribute ("E5017 ...
// Unhandled attribute 'fastopt'") while it ignores `[loop]` itself. The effects in the package have their
// `[loop]` removed for it (linux/build/patch_reshade_effects.py); LumeniteFX is installed from its author's
// GitHub and is not changed. So when ReShade's own call to D3DCompile fails on `[fastopt]`, it is called again
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
    device* copies_device = nullptr;   // the device the copies were made on (destroyed only through it)
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
MotionSource motion{};

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
    if (dev != motion.copies_device) return;   // another device's: the copies are not its to destroy
    if (dev && motion.flow_copy.handle) retire_effect_resource(dev, motion.flow_copy);
    if (dev && motion.patch_copy.handle) retire_effect_resource(dev, motion.patch_copy);
    motion.flow_copy = motion.patch_copy = resource{};
    motion.copies_device = nullptr;
    motion.pending = false;
    motion.last_patches.clear();
}

// Set ReShade to `lumenite` or VORT: the provider definition (DLSS5_Feed.fx rebuilds) and the provider techniques.
void motion_apply(effect_runtime* rt, bool lumenite) {
    const effect_technique lum = rs::call(rt, &effect_runtime::find_technique, kLumeniteEffect, kLumeniteTechnique);
    const effect_technique vort = rs::call(rt, &effect_runtime::find_technique, kVortEffect, kVortTechnique);
    const int want = lumenite ? 3 : 2;
    // Coming from LumeniteFX: ours, or the preset's (install.sh --lumenite turns VORT off there).
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
            motion_fall_back(rt, "LumeniteFX is not installed (install.sh --lumenite installs it)");
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
        if (!make_resource(dev, fs, nullptr, resource_usage::copy_dest, &motion.flow_copy) ||
            !make_resource(dev, ps, nullptr, resource_usage::copy_dest, &motion.patch_copy)) {
            log("[nr] motion vectors: the check's staging textures could not be created; no data check");
            motion.copies_device = dev;
            motion_release(dev);
            motion.verified = true;   // do not try every frame
            return;
        }
        motion.copies_device = dev;
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
    if (!vk_loaded) { nr_vk_use_loader(1); nr_vk_load(); vk_loaded = true; }
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

void on_init_device(device* dev) {
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
void on_vulkan_device_created(VkDevice device) { vulkan_selftest(device); }

void on_destroy_device(device* dev) {
    nr::pe::watchdog::Phase phase("a device is destroyed");
    // Everything the session made lives on this device's VkDevice.
    std::lock_guard<std::mutex> guard(lock);
    const bool sessions_device = !session_device || dev == session_device;
    // ReShade's Vulkan layer no longer knows the device here (its vkDestroyDevice drops it before this event): what NR
    // frees of it with Vulkan's own functions goes through the driver's, not through the loader into ReShade's hooks
    // (which crash on a device they no longer know). Where those cannot be found, nothing is freed that way.
    // (only for the session's device: the entry points are every thread's, and another device's live session keeps
    // going through ReShade)
    const int direct = dev->get_api() == device_api::vulkan && sessions_device
                           ? nr_vk_device_direct(reinterpret_cast<void*>(uintptr_t(dev->get_native())), 1) : 0;
    if (direct > 0) log("[nr] NR's Vulkan objects of the device being destroyed freed through the driver's own functions");
    if (direct < 0)
        log("[nr] the Vulkan driver's own functions were not found: NR's objects of the device being destroyed are left "
            "to it, not freed");
    motion_release(dev);
    if (inspect_copies.dev == dev) inspect_release();
    if (inspect_input.dev == dev) inspect_hold_release();
    // the fitted comparison's and the HDR10 conversion's images are this device's, their programs the session's
    // device's (as HUD FIX's)
    if (sessions_device || inspect_fit.dev == dev) {
        if (direct < 0 && sessions_device) (void)inspect_fit.stage.release();
        inspect_fit_release(!sessions_device);
    }
    if (sessions_device || pq.dev == dev) {
        if (direct < 0 && sessions_device) (void)pq.stage.release();
        pq_release(!sessions_device);
    }
    // HUD FIX's images are this device's, its programs the session's device's
    if (sessions_device || hudfix.dev == dev) {
        if (direct < 0 && sessions_device) (void)hudfix.stage.release();   // Vulkan's own objects (the images are ReShade's)
        if (sessions_device) hudfix_release(false, true); else hudfix_release(true);
    }
    if (sessions_device) {
        cap.armed.store(false);
        cap.dev.store(nullptr);
        cap.immediate.store(nullptr);
        cap.api.store(0);
    }
    if (session && session_device && dev != session_device) {
        log("[nr] device %p (api 0x%x) is being destroyed; NR's session is on device %p and goes on",
            static_cast<void*>(dev), unsigned(dev->get_api()), static_cast<void*>(session_device));
        if (direct > 0) nr_vk_device_direct(nullptr, 0);
        return;
    }
    if (session) {
        log("[nr] the game's device is being destroyed; closing the session");
        if (direct < 0 || !session->wait_idle()) {
            // freeing would go into ReShade's hooks, or NR's GPU work did not finish: left to the device's end
            if (direct >= 0) log("[nr] NR's GPU work did not finish in time; its objects are left to the device's end");
            (void)session.release();
        } else {
            session.reset();
        }
    }
    if (direct > 0) nr_vk_device_direct(nullptr, 0);
    session_device = nullptr;
    reap_effect_resources(dev);
    for (auto i = effect_fences.begin(); i != effect_fences.end();) {
        if (i->second->dev != dev) { ++i; continue; }
        if (i->second->handle.handle) dev->destroy_fence(i->second->handle);
        i->second->dev = nullptr; i = effect_fences.erase(i);
    }

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

// ---- what the add-on's page shows (NR Control's shape, nr_control_shared.hpp; the add-on's own page, nothing shared
// with another process): the layers as they last ran, filled by the frames; the rest at each draw ----------------------
nr::control::Shared view{};

nr::control::LayerStatus* view_row(int number, bool make) {
    for (auto& r : view.layers)
        if (r.number == uint32_t(number)) return &r;
    if (!make) return nullptr;
    for (auto& r : view.layers)
        if (!r.number) { r = nr::control::LayerStatus{}; r.number = uint32_t(number); return &r; }
    return nullptr;   // more layers than rows: the page shows the first eight
}
// The layers this frame runs (by number; 1 for the route without [LayerN]): rows of others go.
void view_layers(const std::vector<int>& numbers) {
    for (auto& r : view.layers)
        if (r.number && std::find(numbers.begin(), numbers.end(), int(r.number)) == numbers.end()) r = nr::control::LayerStatus{};
}
// The engine-data lines of a layer's run (what feed found, the session's word on the run just made).
std::vector<std::string> panel_lines_for(bool renodx_layer, float white_point) {
    nr::pe::panel::Facts pf = panel_facts;
    pf.renodx = renodx_layer;
    pf.white_point = renodx_layer ? 0.f : white_point;
    pf.motion_source = motion.lumenite_active ? "LumeniteFX Kernel" : "VORT";
    if (session) {
        const nr::pe::Session::State st = session->state();
        pf.resizes = st.resizes; pf.carried = st.carried;
        pf.motion_without_depth = st.motion_without_depth;
        pf.renodx_known = renodx_layer && st.renodx_known;
        pf.renodx_hdr = st.renodx.hdr_mode; pf.renodx_absolute = st.renodx.absolute;
        pf.renodx_feed = st.renodx.feed; pf.renodx_divisor = st.renodx.divisor;
        pf.preprocess_ev = st.prep_ev;
    }
    return nr::pe::panel::lines(pf);
}
// What a run's result (run_one: 1 ran, 0 not this frame, -1 cannot be served) says of the layer: 1 ran, 2 its network is
// being built, 3 not running.
uint32_t run_state(int one) {
    if (one > 0) return 1u;
    return one == 0 && session && session->state().building ? 2u : 3u;
}
std::string run_note_text{};
const char* run_note(int one) {
    if (one > 0 || run_state(one) == 2u) return nullptr;
    if (one != 0) return last_status.c_str();
    // passed through: why, when the session says (waiting for video memory for this layer's network, ...)
    const nr::pe::Session::State st = session ? session->state() : nr::pe::Session::State{};
    run_note_text = st.passed && !st.status.empty() ? st.status : "not run this frame";
    return run_note_text.c_str();
}
// A layer's row after its run on this frame, or after it was left out (state 3, `note` why), as the .asi's layers
// report theirs (nr_pe_control.cpp report): the session's figures are the call's layer's (Session::set_call_layer).
struct SpatialRun { uint64_t frame; nr::wilsjo2::SpatialLayout layout; };
std::unordered_map<int, SpatialRun> spatial_at{};
std::unordered_map<int, uint64_t> layer_at{};
void note_layer(int number, bool wilsjo2, uint32_t state, const char* note, unsigned w, unsigned h, uint32_t passes,
                float white_point) {
    layer_at[number] = presents;
    if (wilsjo2 && state == 1 && session && session->spatial_active()) spatial_at[number] = {presents, session->spatial_layout()};
    else spatial_at.erase(number);
    nr::control::LayerStatus* r = view_row(number, true);
    if (!r) return;
    const uint64_t now = GetTickCount64();
    r->processing = wilsjo2 ? 1u : 0u;
    r->placement = 2u;   // every layer of this route runs at Present
    r->width = w; r->height = h;
    r->passes = passes;
    r->fast_passes = 0.f; r->motion = 0;
    std::snprintf(r->note, sizeof r->note, "%s", note ? note : "");
    if (session && state == 1) {
        const nr::pe::Session::State st = session->state();
        r->gpu_ms = session->gpu_ms(); r->network_ms = session->network_ms();
        if (!wilsjo2 && st.renodx_known) {
            r->fast_passes = st.renodx.fast_passes; r->motion = st.renodx.motion;
            if (st.renodx.fast_fallback) std::snprintf(r->note, sizeof r->note, "Extrapolation: no motion vectors");
        } else if (wilsjo2) {
            r->fast_passes = st.wilsjo2_fast_passes; r->motion = st.wilsjo2_motion ? 1u : 0u;
            if (st.wilsjo2_fast_fallback) std::snprintf(r->note, sizeof r->note, "Extrapolation: no motion vectors");
        }
        r->state = state;
        r->net = st.running_net < 0 ? nr::control::kNoNet : uint32_t(st.running_net);
        session->network_size(&r->nr_width, &r->nr_height);
    } else {
        r->state = state;
        r->gpu_ms = r->network_ms = 0.f;
    }
    r->updated = uint32_t(now);
    nr::pe::events::layer_state(number, r->state, r->note);
    if (uint32_t(now) - r->panel_at >= 250u) {   // its engine data, four times a second
        r->panel_at = uint32_t(now);
        const std::vector<std::string> lines = panel_lines_for(!wilsjo2, white_point);
        const size_t n = std::min<size_t>(lines.size(), nr::control::kPanelLines);
        for (size_t i = 0; i < n; ++i) std::snprintf(r->panel[i], sizeof r->panel[i], "%s", lines[i].c_str());
        r->panel_lines = uint32_t(n);
    }
}

// The NR chain's layers on this frame, in order (feed's run_one runs one of them). NRToggleKey / NeuralUplift switches
// all of them; [Preprocess] applies to each. The session runs each with its own network, feature and settings.
// `around(number, after)`: called before and after each layer's run ([Inspect] of one layer).
// `run_pq(controls, feature)`: a wilsjo2 layer's run on an HDR10 swap chain (-2: it cannot run there).
// A wilsjo2 layer's settings for the shared flow (nr_wilsjo2_flow.hpp, the one the .asi runs) from this route's own
// reading of the same keys (layer_config / [DlssNr]): `c` the layer's controls, `cfg` the rest. The white point is the
// layer's manual one (this route has no game exposure).
nr::wilsjo2::Settings w2_settings(const nr::Controls& c, const nr::pe::Config& cfg) {
    nr::wilsjo2::Settings s;
    s.passes = uint32_t(std::max(c.passes, 1));
    s.unlock_passes = cfg.unlock_passes;
    s.colour_limit = cfg.colour_limit;
    for (int n = 2; n <= nr::pe::kMaxPasses && n - 1 < int(nr::wilsjo2::kMaxPasses); ++n) {
        const nr::pe::PassOverride& o = cfg.pass[size_t(n) - 2];
        nr::wilsjo2::PassOverride& d = s.pass[size_t(n) - 1];
        if (o.style) { d.has_style = true; d.style = uint32_t(std::clamp(*o.style, 0, 2)); }
        if (o.intensity) { d.has_intensity = true; d.intensity = *o.intensity; }
        if (o.local_structure) { d.has_local_structure = true; d.local_structure = *o.local_structure; }
        if (o.local_tone) { d.has_local_tone = true; d.local_tone = *o.local_tone; }
        if (o.skin_structure) { d.has_skin_structure = true; d.skin_structure = *o.skin_structure; }
        if (o.automatic_mask) { d.has_auto_mask = true; d.auto_mask = *o.automatic_mask; }
    }
    s.style = uint32_t(std::clamp(c.style, 0, 2));
    s.intensity = c.intensity;
    s.local_structure = c.local_structure;
    s.local_tone = c.local_tone;
    s.skin_structure = c.skin_structure;
    s.auto_mask = c.automatic_mask;
    s.history_strength = cfg.history;
    s.noise_every_frame = c.noise_every_frame;
    s.debug_view = c.debug_view;
    s.show_skin_mask = c.show_skin_mask;
    s.transfer_strength = c.detail_strength;
    s.colour_strength = c.colour_strength;
    s.max_ratio = c.max_ratio;
    s.reversible_mode = uint32_t(std::clamp(c.reversible_mode, 0, 4));
    s.replace_detail_strength = c.replace_detail;
    s.transfer = uint32_t(nr::kEnlargeToIni[std::clamp(c.transfer, 0, 4)]);
    s.classic_scaler = uint32_t(std::clamp(c.classic_scaler, 0, 3));
    s.working_scale = cfg.model_scale;
    s.downscale = cfg.downscale;
    s.passes_mode = cfg.passes_mode;
    s.fast_method = cfg.fast_method;
    for (int m = 0; m < 3; ++m) {   // each method's own
        s.fast_passes[m] = cfg.fast_passes[m]; s.fast_halo_limit[m] = cfg.fast_halo_limit[m]; s.fast_fine[m] = cfg.fast_fine[m];
    }
    s.fast_edit_strength = cfg.fast_edit_strength;
    s.scaling_downscaler = uint32_t(std::clamp(c.scaling_downscaler, 0, 7));
    s.spatial = cfg.spatial.on;
    for (int i = 0; i < 2; ++i) {
        s.spatial_centre[i] = cfg.spatial.centre[i]; s.spatial_work[i] = cfg.spatial.work[i];
        s.spatial_offset[i] = cfg.spatial.offset[i]; s.spatial_shift[i] = cfg.spatial.shift[i];
    }
    s.white_point_scale = cfg.white_point;
    s.white_point_source = 0;
    s.skin_protection = c.skin_protection;
    s.skin_detail = c.skin_detail; s.skin_colour = c.skin_colour;
    s.environment_detail = c.environment_detail; s.environment_colour = c.environment_colour;
    return s;
}

template <class RunOne, class Around, class RunPq>
bool run_layers(effect_runtime* rt, nr::pe::Session& s, const nr::pe::Session::RenoDXFrame& base, const nr::Controls& controls,
                bool have_motion, DXGI_FORMAT format, unsigned w, unsigned h, RunOne&& run_one, Around&& around,
                RunPq&& run_pq) {
    bool ran = false;
    std::vector<int> numbers;
    for (const AddonLayer& l : layers_now) numbers.push_back(l.number);
    view_layers(numbers);
    if (!base.settings.nr_enabled) {
        for (const AddonLayer& l : layers_now)
            note_layer(l.number, !l.renodx, 3, "NR is off (toggle key)", w, h, 0, l.cfg.white_point);
        return false;
    }
    const bool hdr10 = back_buffer_space == color_space::hdr10_st2084 || back_buffer_space == color_space::hdr10_hlg;
    // [Inspect]'s debug view (or skin mask) of a layer shows that layer's own picture: the layers after it are left out
    // meanwhile (they would work on the debug picture), as on the .asi route
    const int shown = chain.inspect.layer > 0 && (chain.inspect.debug_view || chain.inspect.skin_mask) &&
                              std::any_of(layers_now.begin(), layers_now.end(),
                                          [](const AddonLayer& a) { return a.number == chain.inspect.layer; })
                          ? chain.inspect.layer : 0;
    for (const AddonLayer& l : layers_now) {
        if (shown && l.number > shown) {
            note_layer(l.number, !l.renodx, 3, ("not run while Layer " + std::to_string(shown) + "'s debug view is shown").c_str(),
                       w, h, 0, l.renodx ? 1.f : l.cfg.white_point);
            continue;
        }
        s.set_call_layer(l.number);
        s.set_call_network(l.network);
        int one = 0;
        if (l.renodx) {
            nr::pe::Session::RenoDXFrame f = base;
            f.settings = l.rdx.values;
            f.settings.nr_enabled = true;
            f.present = present_encoding(f.settings, format);
            if (!f.present) {
                report("NR chain layer " + std::to_string(l.number) + ": the RenoDX processing cannot serve this swap chain "
                       "(colour space " + std::string(space_name(back_buffer_space)) + ", format " +
                       std::to_string(unsigned(format)) + "); it is left out");
                note_layer(l.number, false, 3, "RenoDX cannot serve this swap chain", w, h, 0, 1.f);
                continue;
            }
            if (f.present != 1) f.present_white_nits = present_white(rt, f.settings);
            if (!have_motion && f.settings.present_guides == 1u) {   // NRPresentGuides = 1: needs DLSS5_Feed's vectors
                note_layer(l.number, false, 3, "NRPresentGuides = 1 needs DLSS5_Feed's motion vectors", w, h, 0, 1.f);
                continue;
            }
            s.set_history_strength(1.0f);
            around(l.number, false);
            one = run_one(controls, &f, l.feature());
            around(l.number, true);
            note_layer(l.number, false, run_state(one), run_note(one), w, h, nr::renodx::stack_passes(f.settings), 1.f);
        } else {
            if (back_buffer_space == color_space::hdr10_hlg) {
                report("NR chain layer " + std::to_string(l.number) + ": the swapchain is HLG, which the wilsjo2 "
                       "processing has no decode for; it is left out");
                note_layer(l.number, true, 3, "HLG swap chain: wilsjo2 has no decode for it", w, h, 0, l.cfg.white_point);
                continue;
            }
            nr::Controls c = l.cfg.controls;
            c.preprocess = controls.preprocess;
            // the wilsjo2 processing (nr_wilsjo2_flow.hpp) around a network-only run, as RenoDX's
            const nr::wilsjo2::Settings ws = w2_settings(c, l.cfg);
            nr::pe::Session::Wilsjo2Frame wf;
            wf.settings = &ws;
            wf.preprocess = controls.preprocess;
            s.set_history_strength(l.cfg.history);
            around(l.number, false);
            one = hdr10 ? run_pq(c, l.feature(), &wf) : run_one(c, nullptr, l.feature(), &wf);
            around(l.number, true);
            if (one == -2) {   // HDR10, its conversion could not run (said by pq_say): left out, the chain goes on
                note_layer(l.number, true, 3, "HDR10 swap chain: its PQ conversion could not run", w, h, 0, l.cfg.white_point);
                continue;
            }
            note_layer(l.number, true, run_state(one), run_note(one), w, h, uint32_t(c.passes), l.cfg.white_point);
        }
        if (one < 0) break;
        ran |= one > 0;
    }
    s.set_call_layer(0);
    s.set_call_network(-1);
    return ran;
}

void feed(effect_runtime* rt, command_list* cl, resource_view rtv) {
    device* dev = rt->get_device();
    resource_view mv_srv{}, mv_srgb{}, d_srv{}, d_srgb{};
    if (mv_var.handle) rt->get_texture_binding(mv_var, &mv_srv, &mv_srgb);
    if (depth_var.handle) rt->get_texture_binding(depth_var, &d_srv, &d_srgb);
    const resource colour = rs::call(dev, &device::get_resource_from_view, rtv);
    resource motion_image = mv_srv.handle ? rs::call(dev, &device::get_resource_from_view, mv_srv) : resource{};
    resource depth = d_srv.handle ? rs::call(dev, &device::get_resource_from_view, d_srv) : resource{};
    if (!colour.handle) return;
    EffectRecording recording(rt, cl);
    if (!motion_image.handle && !missing_reported) {
        missing_reported = true;
        report("DLSS5_Feed.fx rendered but its DLSS5_MV texture is not bound; the pass runs "
               "without motion (no history) until it is");
    }
    static bool traced = false;
    if (!traced) log("[nr] feed: colour %llx motion %llx depth %llx", (unsigned long long)colour.handle,
                     (unsigned long long)motion_image.handle, (unsigned long long)depth.handle);
    const resource_desc cd = rs::call(dev, &device::get_resource_desc, colour);
    const unsigned w = cd.texture.width, h_ = cd.texture.height;
    if (!traced) { traced = true; log("[nr] feed: back buffer %ux%u format %u, api %x", w, h_, unsigned(cd.texture.format), unsigned(dev->get_api())); }
    if (motion_image.handle) {
        const resource_desc md = rs::call(dev, &device::get_resource_desc, motion_image);
        if (md.texture.width != w || md.texture.height != h_) {
            report("DLSS5_MV is not the back buffer's size; its vectors are read across the whole texture, "
                   "in back-buffer pixels");
        }
    }
    // DLSS5_MV is in pixels at back-buffer size, prev = uv + mv: the consumer's
    // normalized units are one over the extent, and the sign is already ours.
    float sx = w ? 1.0f / float(w) : 1.0f, sy = h_ ? 1.0f / float(h_) : 1.0f;
    bool inverted = depth_is_reversed(rt);
    // A gap: the next frame NR runs starts its history afresh (said when it runs). The reset stays asked for until a
    // frame runs: frames passed through (a network building) do not consume it.
    const bool gap = have_run && presents > presents_at_run + kResumeGap;
    if (gap) need_reset = true;
    if (hold_released) { need_reset = true; hold_released = false; }
    bool reset = need_reset;   // also a hold let go below (this frame's runs read it)
    view.width = w; view.height = h_;
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
        pf.motion = motion_image.handle ? 1 : 0;
        pf.depth = depth.handle ? 1 : 0;
        pf.preprocess_exposure = config.controls.preprocess.active() ? config.controls.preprocess.exposure : 0;
        pf.white_point = renodx_on ? 0.f : config.white_point;
    }
    if (!vk_loaded) { nr_vk_use_loader(dev->get_api() == device_api::vulkan); nr_vk_load(); vk_loaded = true; log("[nr] feed: Vulkan entry points resolved"); }
    // An HDR10 back buffer is PQ (or HLG) over BT.2020, and nothing in this
    // pass decodes either. Running on it anyway would hand the model a picture
    // in a transfer function it has never seen and hand back a worse one, which
    // is a failure worth naming rather than a picture worth shipping.
    if (!layered) view_layers({1});   // the route without [LayerN]: its one layer
    // HDR10 (PQ) is decoded for the wilsjo2 processing around its run (run_pq); HLG is not.
    if (!layered && !renodx_on && back_buffer_space == color_space::hdr10_hlg) {
        report("the swapchain is HLG; this pass has no decode for it, so the frame is left alone - switch the game to "
               "SDR, scRGB or HDR10 (PQ)");
        note_layer(1, true, 3, "HLG swap chain: wilsjo2 has no decode for it", w, h_, 0, config.white_point);
        return;
    }
    const bool pq_frame = !renodx_on && back_buffer_space == color_space::hdr10_st2084;
    // scRGB: the back buffer is linear light with 1.0 at SDR white, so the
    // runtime has to encode it for the model exactly as it does an upscaler's
    // input. Without this the model is shown linear values as if they were
    // display-referred, which is what once made the upscaler path dark and
    // blocky - and on this route nothing downstream would bound it.
    const bool linear_hdr = back_buffer_space == color_space::extended_srgb_linear;
    const nr::Controls& controls = config.controls;
    bool ran = false;
    if (!session) session_device = dev;
    auto& s = the_session();
    s.set_call_layer(layered ? 0 : 1);
    // [RenoDX] Enabled at start: RenoDX's processing around the network (nr_renodx.hpp), its other keys as the file has
    // them now.
    nr::pe::Session::RenoDXFrame renodx_frame;
    renodx_frame.hdr_flag = linear_hdr;
    // DLSS5_MV can remain bound while its provider is disabled. Keep legacy's handling of that texture,
    // but do not offer temporal fast passes without the selected provider actually enabled.
    const int provider = feed_provider(rt);
    const effect_technique provider_technique = provider == 2
        ? rs::call(rt, &effect_runtime::find_technique, kVortEffect, kVortTechnique)
        : provider == 3 ? rs::call(rt, &effect_runtime::find_technique, kLumeniteEffect, kLumeniteTechnique)
                        : effect_technique{};
    renodx_frame.fast_motion_available = motion_image.handle && (provider == 2 || provider == 3
        ? provider_technique.handle && rt->get_technique_state(provider_technique) : provider != 0);
    renodx_frame.after_upscale = true;
    renodx_frame.settings = config.renodx.values;
    if (!layered && chain.inspect.layer == 1) renodx_frame.settings.debug_view = chain.inspect.debug_view;
    {   // NeuralUplift as NRToggleKey left it
        static nr::pe::NrToggle toggle;
        renodx_frame.settings.nr_enabled = toggle.frame(config.renodx);
    }
    {   // RenoDX's NR screenshot (nr_screenshot.hpp), taken by a RenoDX layer: this route's final picture is the presented frame
        static nr::pe::KeyPress screenshot_key;
        const auto& r = config.renodx;
        nr::pe::screenshot::configure({r.screenshot_source, r.screenshot_format, r.screenshot_layout,
                                       r.screenshot_jpeg_quality, r.screenshot_max_mb});
        if (screenshot_key.frame(r.screenshot_key)) nr::pe::screenshot::request();
        nr::pe::screenshot::tick();
    }
    renodx_frame.preprocess = controls.preprocess;
    const nr::pe::Session::RenoDXFrame* renodx = renodx_on ? &renodx_frame : nullptr;
    if (renodx && !layered) {
        renodx_frame.present = present_encoding(renodx_frame.settings, static_cast<DXGI_FORMAT>(cd.texture.format));
        if (!renodx_frame.present) {
            report("the RenoDX-style option cannot serve this swap chain (colour space " +
                   std::string(space_name(back_buffer_space)) + ", format " + std::to_string(unsigned(cd.texture.format)) +
                   "); the frame is left alone");
            note_layer(1, false, 3, "RenoDX cannot serve this swap chain", w, h_, 0, 1.f);
            return;
        }
        if (renodx_frame.present != 1) renodx_frame.present_white_nits = present_white(rt, renodx_frame.settings);
    }
    // NRPresentGuides: Required needs DLSS5_Feed's motion vectors; Optional runs on neutral ones without them, Never
    // always (Session::Impl::neutral_motion).
    if (renodx && !layered && !motion_image.handle && renodx_frame.settings.present_guides == 1u) {
        report("the RenoDX-style option needs DLSS5_Feed's motion vectors (NRPresentGuides = 1); the frame is left alone "
               "until they are bound");
        note_layer(1, false, 3, "NRPresentGuides = 1 needs DLSS5_Feed's motion vectors", w, h_, 0, 1.f);
        return;
    }
    // NR on / off (NeuralUplift as NRToggleKey left it) is every layer's, this route's one without [LayerN] too
    view.nr_on = renodx_frame.settings.nr_enabled ? 1u : 0u;
    if (!layered && !renodx && !renodx_frame.settings.nr_enabled) {
        note_layer(1, true, 3, "NR is off (toggle key)", w, h_, 0, config.white_point);
        return;
    }

    // The sampled pair: evaluates 1, 2, 3 and then every 300th, as the model's
    // readback does. Drain first, because the staging pair is reused and the
    // copy below would overwrite what is about to be read.
    // NR_DEBUG_DUMP_RUN=<n> (a measurement aid): also every 4th evaluation of the 32 from the n-th, for how steady a
    // still picture stays; NR_DEBUG_DUMP_COUNT / NR_DEBUG_DUMP_STEP change the 32 and the 4 (a run of final pictures
    // for a test that cannot read them itself: the Vulkan route's presented image).
    const auto env_number = [](const char* name, uint64_t fallback) {
        char v[16] = {};
        return GetEnvironmentVariableA(name, v, sizeof v) > 0 ? std::strtoull(v, nullptr, 10) : fallback;
    };
    static const uint64_t run_from = env_number("NR_DEBUG_DUMP_RUN", 0),
                          run_count = env_number("NR_DEBUG_DUMP_COUNT", 32),
                          run_step = std::max<uint64_t>(1, env_number("NR_DEBUG_DUMP_STEP", 4));
    const bool sample = debug_dump_on() &&
                        (++dump_state.seen <= 3 || dump_state.seen % 300 == 0 ||
                         (run_from && dump_state.seen >= run_from && dump_state.seen < run_from + run_count &&
                          (dump_state.seen - run_from) % run_step == 0));
    bool sampling = false;
    if (debug_dump_on()) {
        // the last pair's copies done before it is read: D3D12 keeps more than one frame in flight (a dump every
        // frame read its staging pair before the copy into it had run)
        if (dump_state.pending) rt->get_command_queue()->wait_idle();
        dump_drain(dev);
        if (sample && dump_ensure(dev, w, h_, cd.texture.format)) {
            const resource_usage rt_usage = resource_usage::render_target, cs = resource_usage::copy_source;
            cl->barrier(1, &colour, &rt_usage, &cs);
            cl->copy_texture_region(colour, 0, nullptr, dump_state.before, 0, nullptr);
            cl->barrier(1, &colour, &cs, &rt_usage);
            sampling = true;
        }
    }
    // One run of NR on the back buffer, in place: the route's one layer, or one layer of the NR chain ([LayerN], each its
    // own feature, so its own history). -1: the frame cannot be served at all (said by report()), else whether it ran.
    // What a run works on: the back buffer, or (a wilsjo2 layer on an HDR10 swap chain) pq.lin, linear light
    struct RunTarget { resource colour; resource_desc cd; bool linear; };
    RunTarget tgt{colour, cd, linear_hdr};
    bool hold_running = false;
    auto run_one = [&](const nr::Controls& controls, const nr::pe::Session::RenoDXFrame* renodx, uint64_t feature,
                       const nr::pe::Session::Wilsjo2Frame* w2f = nullptr) -> int {
        auto& held_input = inspect_input;
        nr::pe::Session::RenoDXFrame held_frame;
        const bool use_hold = hold_running && held_input.held;
        const bool live_linear = tgt.linear;
        struct RestoreLinear { bool& value; bool saved; ~RestoreLinear() { value = saved; } } restore_linear{tgt.linear, live_linear};
        if (use_hold) {
            if (!held_input.metadata) {
                held_input.linear = tgt.linear;
                if (renodx) { held_input.encoding = renodx->present; held_input.white = renodx->present_white_nits; }
                held_input.metadata = true;
            }
            tgt.linear = held_input.linear;
            if (renodx) {
                held_frame = *renodx; held_frame.hdr_flag = held_input.linear;
                held_frame.present = held_input.encoding; held_frame.present_white_nits = held_input.white;
                renodx = &held_frame;
            }
        }
        // a wilsjo2 layer: its flow, on linear light when the target is (scRGB, a decoded HDR10 frame)
        nr::pe::Session::Wilsjo2Frame w2frame;
        if (w2f) { w2frame = *w2f; w2frame.hdr = tgt.linear; w2frame.fast_motion = renodx_frame.fast_motion_available; }
        const nr::pe::Session::Wilsjo2Frame* const w2run = w2f ? &w2frame : nullptr;
        bool ran = false;
        switch (dev->get_api()) {
        // DXVK builds D3D10 on its D3D11: an ID3D10Device or ID3D10Texture2D answers QueryInterface with the
        // D3D11 object underneath (src/d3d10/d3d10_device.cpp, d3d10_texture.cpp), and the D3D11 path asks for
        // every interface it uses that way. So a D3D10 game takes the D3D11 path with ReShade's D3D10 natives.
        case device_api::d3d10:
        case device_api::d3d11: {
            nr::pe::Session::D3D11Frame f{};
            f.device = reinterpret_cast<IUnknown*>(dev->get_native());
            f.target = reinterpret_cast<IUnknown*>(tgt.colour.handle);
            f.motion = reinterpret_cast<IUnknown*>(motion_image.handle);
            f.depth = reinterpret_cast<IUnknown*>(depth.handle);
            f.depth_inverted = inverted;
            f.motion_scale_x = sx; f.motion_scale_y = sy;
            f.reset = reset;
            f.linear_hdr = tgt.linear;
            f.renodx = renodx;
            f.wilsjo2 = w2run;
            f.feature = feature;
            ran = s.run_d3d11(f, controls);
            break;
        }
        case device_api::d3d12: {
            auto* device = reinterpret_cast<ID3D12Device*>(dev->get_native());
            auto* list = reinterpret_cast<ID3D12GraphicsCommandList*>(cl->get_native());
            // ReShade renders its techniques with the back buffer as a render
            // target (DLSS5-Feeder moves it from exactly that state). Move it to
            // UAV through ReShade's own barrier so its tracking agrees with what
            // the pass does, and put it back after.
            const resource_usage from = resource_usage::render_target, to = resource_usage::unordered_access;
            cl->barrier(1, &tgt.colour, &from, &to);
            nr::pe::Session::EngineResources r{};
            r.motion = reinterpret_cast<ID3D12Resource*>(motion_image.handle);
            r.motion_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            r.depth = reinterpret_cast<ID3D12Resource*>(depth.handle);
            r.depth_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            r.depth_inverted = inverted;
            r.motion_scale_x = sx; r.motion_scale_y = sy;
            r.reset = reset;
            r.feature = feature;
            // The D3D12 path asks the question the other way round, and its default
            // said "not encoded" - so a float SDR back buffer used to be tone mapped
            // for no reason while D3D11 and Vulkan left an HDR one raw. One answer
            // for all three now.
            r.colour_encoded = !tgt.linear;
            r.wilsjo2 = w2run;
            ran = renodx ? s.run_renodx(device, list, reinterpret_cast<ID3D12Resource*>(tgt.colour.handle),
                                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, r, controls,
                                        reinterpret_cast<ID3D12Resource*>(tgt.colour.handle),
                                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, *renodx)
                         : s.run_after(device, list, reinterpret_cast<ID3D12Resource*>(tgt.colour.handle),
                                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS, r, controls);
            cl->barrier(1, &tgt.colour, &to, &from);
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
                return -1;
            }
            VkQueue queue{}; uint32_t family{};
            if (!nr::pe::vkdevice::queue_for(rs_device, &queue, &family)) {
                report("no graphics+compute queue of the game's was seen; nothing to upload the weights on");
                return -1;
            }
            s.set_vulkan_queue(queue, family);
            const auto cmd = reinterpret_cast<VkCommandBuffer>(static_cast<uintptr_t>(cl->get_native()));
            nr::pe::Session::VulkanFrame f{};
            // VkImage is a 64-bit non-dispatchable handle on both bitnesses; the
            // C-style cast is the one spelling valid for a pointer (x64) and an
            // integer (x86) alike.
            f.colour = (VkImage)tgt.colour.handle;
            f.colour_format = nr::pe::vulkan_colour_format_of(static_cast<DXGI_FORMAT>(tgt.cd.texture.format));
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
            f.colour_usage = vk_usage(tgt.cd.usage);
            if (motion_image.handle) {
                const resource_desc md = rs::call(dev, &device::get_resource_desc, motion_image);
                f.motion = (VkImage)motion_image.handle;
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
            f.linear_hdr = tgt.linear;
            if (f.colour_format == VK_FORMAT_UNDEFINED) {
                report("the back buffer's format is not one this pass accepts");
                return -1;
            }
            // ReShade renders techniques with the back buffer as a render target;
            // move it through ReShade's own barriers so its layout tracking and
            // ours agree: copy source for the pass's read, render target again
            // after. The runtime copies the frame out and the answer back itself
            // and leaves the image as it found it (VulkanFrame::in_place).
            const resource_usage rt = resource_usage::render_target, src = resource_usage::copy_source,
                                 dst = resource_usage::copy_dest;
            cl->barrier(1, &tgt.colour, &rt, &src);
            f.colour_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            f.in_place = true;
            f.renodx = renodx;
            f.wilsjo2 = w2run;
            f.feature = feature;
            const VkImage out = s.run_vulkan(h, cmd, f, controls);
            if (out == f.colour) {
                cl->barrier(1, &tgt.colour, &src, &rt);
                ran = true;
            } else if (out) {
                cl->barrier(1, &tgt.colour, &src, &dst);
                VkImageCopy region{};
                region.srcSubresource = region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                region.extent = {w, h_, 1};
                vkCmdCopyImage(cmd, out, VK_IMAGE_LAYOUT_GENERAL, f.colour, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
                cl->barrier(1, &tgt.colour, &dst, &rt);
                ran = true;
            } else {
                cl->barrier(1, &tgt.colour, &src, &rt);
            }
            break;
        }
        default:
            report("this graphics API is not handled by the ReShade add-on (D3D10, D3D11, D3D12 and Vulkan are)");
            return -1;
        }
        return ran ? 1 : 0;
    };
    // A wilsjo2 layer on an HDR10 swap chain: run on pq.lin (decoded before, encoded back after). -2: cannot run here.
    const auto run_pq = [&](const nr::Controls& c, uint64_t feature, const nr::pe::Session::Wilsjo2Frame* w2f) -> int {
        if (!pq_begin(dev, cl, colour, w, h_, cd.texture.format)) return -2;
        tgt = RunTarget{pq.lin, pq.lin_desc, true};
        const int one = run_one(c, nullptr, feature, w2f);
        tgt = RunTarget{colour, cd, linear_hdr};
        if (one > 0 && !pq_end(dev, cl, colour)) return -2;
        return one;
    };
    // [Inspect]: Layer 0 (the final picture: every layer runs at Present here) around all of them, Layer N around its
    // own run (the layer page's number; the route without [LayerN] is layer 1)
    const nr::pe::InspectConfig& ic = chain.inspect;
    const bool hold = ic.hold && ic.layer > 0;
    const bool ensured = (ic.compare || !ic.apply_model || hold) && inspect_ensure(dev, w, h_, cd.texture.format);
    // the fitted side by side's images go when it is not shown (its program stays)
    if (inspect_fit.off.handle && !(ic.compare == 1 && ic.apply_model && ic.zoom < 1.999f)) inspect_fit_release(true);
    // let go: the layers start their history again next frame (the fork returns to the live frame with a reset; this
    // route resets the whole frame's layers together)
    // a hold let go (also by a change of layer): this frame's run starts its history again - the first live frame
    if (inspect_input.held && (!ensured || !hold || inspect_input.layer != ic.layer)) reset = true;
    if (!ensured || !hold || inspect_input.layer != ic.layer) inspect_input.held = false;
    inspect_input.layer = ic.layer;
    const bool inspecting = ensured && (ic.compare || !ic.apply_model);
    const bool whole = inspecting && ic.layer == 0;
    if (whole) inspect_take(cl, colour, inspect_copies.off);
    bool shot_chain = false;
    const auto shot_frame = [&](bool after) {
        const resource_usage rt_state = resource_usage::render_target, ua = resource_usage::unordered_access;
        cl->barrier(1, &colour, &rt_state, &ua);
        raw_pass(dev, cl, [&](VkCommandBuffer cmd, const nr::HostDevice&, const auto& image) {
            const auto im = image(colour);
            if (after) s.capture_end(cmd, im.image, im.layout);
            else shot_chain = s.capture_begin(cmd, im.image, im.format, w, h_, im.layout,
                back_buffer_space == color_space::hdr10_st2084 ? 2 : back_buffer_space == color_space::extended_srgb_linear ? 1 : 0,
                "present");
            return true;
        });
        cl->barrier(1, &colour, &ua, &rt_state);
    };
    if (nr::pe::screenshot::wants_final()) shot_frame(false);
    struct ShotGuard { nr::pe::Session& s; bool& active; ~ShotGuard() { if (active) s.capture_abort(); } } shot_guard{s, shot_chain};
    // HUD FIX around every layer (composed before a final-picture comparison shows it)
    const HudPlan hud_plan = hudfix_plan(dev, cl, colour.handle);
    const bool hud_on = hudfix_begin(dev, cl, colour, w, h_, cd.texture.format, hud_plan);
    // An early return after the captured image was installed must restore the game's frame, including its HUD.
    struct HudRestore {
        command_list* cl;
        resource colour, back;
        bool active;
        ~HudRestore() {
            if (active) hudfix_copy(cl, back, resource_usage::unordered_access, colour, resource_usage::render_target);
        }
    } hud_restore{cl, colour, hudfix.back[hudfix.cur], hud_on && hud_plan.captured};
    const resource live_depth = depth, live_motion = motion_image;
    const bool live_inverted = inverted;
    const float live_sx = sx, live_sy = sy;
    const bool live_reset = reset;
    const auto around = [&](int number, bool after) {
        if (!ensured || ic.layer != number) return;
        if (!after && hold) {
            const bool was_held = inspect_input.held;
            hold_running = inspect_hold(cl, colour, depth, motion_image, inverted, sx, sy);
            if (hold_running && was_held) reset = false;
        }
        if (after) {
            hold_running = false;
            depth = live_depth; motion_image = live_motion; inverted = live_inverted;
            sx = live_sx; sy = live_sy; reset = live_reset;
        }
        if (!inspecting) return;
        if (!after) inspect_take(cl, colour, inspect_copies.off);
        else inspect_show(cl, colour, rtv, ic, w, h_);
    };
    if (!layered) {
        around(1, false);
        nr::Controls one_layer = controls;   // the route without [LayerN]: [Inspect]'s debug views of its one layer
        if (!renodx && chain.inspect.layer == 1) {
            one_layer.debug_view = chain.inspect.debug_view;
            one_layer.show_skin_mask = chain.inspect.skin_mask;
        }
        // Same identity as explicit Layer1; feature 0 must never share this runtime with named layers.
        const uint64_t feature = renodx ? 3u : 2u;
        // wilsjo2 from [DlssNr]: the shared flow around a network-only run (as a [LayerN] wilsjo2 layer)
        const nr::wilsjo2::Settings ws = w2_settings(one_layer, config);
        nr::pe::Session::Wilsjo2Frame wf;
        wf.settings = &ws;
        wf.preprocess = controls.preprocess;
        const int one = pq_frame ? run_pq(one_layer, feature, renodx ? nullptr : &wf)
                                 : run_one(one_layer, renodx, feature, renodx ? nullptr : &wf);
        around(1, true);
        if (one == -2) {   // HDR10, its conversion could not run (said by pq_say): the frame is left alone
            note_layer(1, true, 3, "HDR10 swap chain: its PQ conversion could not run", w, h_, 0, config.white_point);
            return;
        }
        if (renodx && !renodx_frame.settings.nr_enabled)   // RenoDX's stage passes the frame through
            note_layer(1, false, 3, "NR is off (toggle key)", w, h_, 0, 1.f);
        else
            note_layer(1, !renodx, run_state(one), run_note(one), w, h_,
                       renodx ? nr::renodx::stack_passes(renodx_frame.settings) : uint32_t(controls.passes), config.white_point);
        if (one < 0) return;
        ran = one > 0;
    } else {
        ran = run_layers(rt, s, renodx_frame, controls, motion_image.handle != 0, static_cast<DXGI_FORMAT>(cd.texture.format), w, h_,
                         run_one, around, run_pq);
    }
    if (hud_on) hudfix_end(dev, cl, colour, hud_plan, motion_image);
    hud_restore.active = false;
    if (shot_chain && ran) { shot_frame(true); shot_chain = false; }
    if (whole) inspect_show(cl, colour, rtv, ic, w, h_);
    if (sampling) {
        const resource_usage rt_usage = resource_usage::render_target, cs = resource_usage::copy_source;
        cl->barrier(1, &colour, &rt_usage, &cs);
        cl->copy_texture_region(colour, 0, nullptr, dump_state.after, 0, nullptr);
        cl->barrier(1, &colour, &cs, &rt_usage);
        dump_state.pending = true;
    }
    report(s.status());
    if (ran && gap)
        log("[nr] NR runs again after %llu frame(s) without it: history restarts",
            (unsigned long long)(presents - presents_at_run - 1));
    if (ran) { have_run = true; presents_at_run = presents; need_reset = false; }
    if (ran) ++frames_run; else ++frames_declined;
    if ((frames_run + frames_declined) % 240 == 0)
        log("[nr] frames: %u enhanced, %u declined; %.2f ms on the GPU (network %.2f, route %.2f)", frames_run,
            frames_declined, s.gpu_ms(), s.network_ms(), std::max(s.gpu_ms() - s.network_ms(), 0.0f));
}

// ---- the add-on's page, in ReShade's overlay (its Add-ons tab, under this add-on's name) ------------------------------
// NR Control's layout and parts (nr_control_pages.hpp, as the .asi route draws them): what runs and what it costs, the NR
// chain and the selected layer's page, then the keys for every layer. Every change is written into dlssnr-amd.ini in
// place (nr_ini_edit.hpp: comments and the other sections stay) and the frames take it from the file (settings(), the
// session's network switch), as on the .asi route.
const char* network_name(int n) { return n >= 0 ? nr::network_codename(n) : "?"; }

// The build's progress as a fraction and a phrase (nr::g_build_stage).
float build_fraction(const nr::pe::Session::State& st, char* text, size_t n) { return nr::pe::build_progress(st, text, n); }

// The route without [LayerN] running [DlssNr], as a wilsjo2 layer's keys ([LayerN] Processing = wilsjo2, read by
// layer_config: the original's names and numbers), from the file as the frames read it (config).
void dlssnr_as_layer(nr::ini_edit::Ini& ini, const std::string& sec) {
    const nr::Controls& c = config.controls;
    char v[64];
    const auto num = [&](const char* key, float x) { std::snprintf(v, sizeof v, "%g", double(x)); ini.set(sec, key, v); };
    const auto flag = [&](const char* key, bool b) { ini.set(sec, key, b ? "true" : "false"); };
    num("Style", float(c.style)); num("Intensity", c.intensity); num("LocalStructure", c.local_structure);
    num("LocalTone", c.local_tone); num("SkinStructure", c.skin_structure); flag("AutoMask", c.automatic_mask);
    num("TransferStrength", c.detail_strength); num("ColourStrength", c.colour_strength); num("MaxRatio", c.max_ratio);
    num("ReversibleMode", float(c.reversible_mode)); num("ReplaceDetailStrength", c.replace_detail);
    num("WorkingScale", config.model_scale); num("Downscale", float(config.downscale)); num("Transfer", float(nr::kEnlargeToIni[std::clamp(c.transfer, 0, 4)]));
    num("ClassicScaler", float(c.classic_scaler)); num("ScalingDownscaler", float(c.scaling_downscaler));
    flag("SpatialCompression", config.spatial.on);
    num("SpatialCenterX", config.spatial.centre[0]); num("SpatialCenterY", config.spatial.centre[1]);
    num("SpatialWorkX", config.spatial.work[0]); num("SpatialWorkY", config.spatial.work[1]);
    num("SpatialOffsetX", config.spatial.offset[0]); num("SpatialOffsetY", config.spatial.offset[1]);
    num("SpatialShiftX", config.spatial.shift[0]); num("SpatialShiftY", config.spatial.shift[1]);
    num("WhitePointScale", config.white_point);
    flag("SkinProtection", c.skin_protection); num("SkinDetail", c.skin_detail); num("SkinColour", c.skin_colour);
    num("EnvironmentDetail", c.environment_detail); num("EnvironmentColour", c.environment_colour);
    num("History", config.history); flag("NoiseEveryFrame", c.noise_every_frame); flag("ApplyModel", c.apply_model);
    num("Passes", float(c.passes)); flag("UnlockPasses", config.unlock_passes); flag("ColourLimit", config.colour_limit);
    ini.set(sec, "PassesMode", config.passes_mode == 1 ? "fast" : "legacy");
    num("FastMethod", float(config.fast_method));
    for (int m = 0; m < 3; ++m) {   // each method's own (Fast<Method>Passes / HaloLimit / Fine)
        static const char* const names[] = {"FastExtrapolation", "FastFeedback", "FastHybrid"};
        const std::string n = names[m];
        num((n + "Passes").c_str(), config.fast_passes[m]); flag((n + "HaloLimit").c_str(), config.fast_halo_limit[m]);
        num((n + "Fine").c_str(), config.fast_fine[m]);
    }
    num("FastEditStrength", config.fast_edit_strength);
    for (int n = 2; n <= nr::pe::kMaxPasses; ++n) {
        const nr::pe::PassOverride& o = config.pass[size_t(n) - 2];
        const std::string pre = "Pass" + std::to_string(n);
        if (o.style) num((pre + "Style").c_str(), float(*o.style));
        if (o.intensity) num((pre + "Intensity").c_str(), *o.intensity);
        if (o.local_structure) num((pre + "LocalStructure").c_str(), *o.local_structure);
        if (o.local_tone) num((pre + "LocalTone").c_str(), *o.local_tone);
        if (o.skin_structure) num((pre + "SkinStructure").c_str(), *o.skin_structure);
        if (o.automatic_mask) flag((pre + "AutoMask").c_str(), *o.automatic_mask);
    }
}

// The NR chain on this route (nr_control_pages.hpp ChainEditor): every layer runs at Present, on the frame ReShade hands
// over. A file without [LayerN] is the route as it runs: RenoDX ([RenoDX] Enabled = 1 when the game started), else
// wilsjo2 from [DlssNr]; its first change writes it as [Layer1].
nr::ini_edit::Ini legacy_page{std::filesystem::path{}};   // [DlssNr] as [Layer1] for the page; never saved
bool legacy_enabled(const nr::ini_edit::Ini& ini) {
    if (renodx_on) return true;
    std::string v = ini.get("DlssNr", "Enabled", "true");
    for (char& ch : v) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    return !(v == "0" || v == "false" || v == "off" || v == "no");
}
const nr::control_ui::ChainRoute kReShadeChain = [] {
    nr::control_ui::ChainRoute r;
    r.present_only = true;
    r.placement_default = "present";
    r.added_processing = "RenoDX";
    r.added_placement = "present";
    r.chain_help = "Each layer is an NR run of its own on the frame, in layer order: its processing (RenoDX, or wilsjo2 - "
                   "this route's own composition under OptiScaler DLSS-NR's settings), its network ([Network] Model, or one "
                   "of its own) and its own settings below. On this route every layer runs at Present, on the picture "
                   "ReShade hands over. Each layer costs a network run, and a layer with another network or size its own "
                   "memory. Removing every layer goes back to [DlssNr] / [RenoDX].";
    r.wilsjo2_about = "This route's own composition around the network, under OptiScaler DLSS-NR's settings and names.";
    r.wilsjo2_table = nr::reshade_layer_keys::kKeys;
    r.wilsjo2_count = nr::reshade_layer_keys::kCount;
    r.mask_label = "Auto skin mask";
    r.passes_help = "Passes. Model passes a frame, 1-3 (up to 30 with Unlock passes); each costs one more network run.";
    r.pass_same_help = "On: pass 1's settings, with the local tone at 0. Off: this pass's own settings below.";
    r.legacy_row = [](const nr::ini_edit::Ini& ini) {
        nr::control_ui::ChainRow row;
        row.number = 1;
        row.enabled = legacy_enabled(ini);
        row.processing = renodx_on ? "RenoDX" : "wilsjo2";
        row.placement = "present";
        return row;
    };
    r.legacy_source = [](const nr::ini_edit::Ini& ini, const nr::control_ui::ChainRow& row) {
        if (row.processing == "RenoDX") return std::make_pair(&ini, std::string("RenoDX"));
        legacy_page = nr::ini_edit::Ini{std::filesystem::path{}};
        dlssnr_as_layer(legacy_page, "Layer1");
        return std::make_pair(static_cast<const nr::ini_edit::Ini*>(&legacy_page), std::string("Layer1"));
    };
    r.materialize = [](nr::ini_edit::Ini& ini) {
        const std::string sec = "Layer1";
        ini.set(sec, "Enabled", legacy_enabled(ini) ? "1" : "0");
        ini.set(sec, "Processing", renodx_on ? "RenoDX" : "wilsjo2");
        ini.set(sec, "Placement", "present");
        if (renodx_on) {
            for (const auto& [k, v] : ini.keys("RenoDX"))
                if (!nr::ini_edit::chain_global(k)) ini.set(sec, k, v);
        } else {
            dlssnr_as_layer(ini, sec);
        }
        log("[nr] add-on page: the route's one layer (%s) written as [Layer1]", renodx_on ? "[RenoDX]" : "[DlssNr]");
    };
    r.renodx_route = [](const std::string&) { return nr::control_ui::Route::kReShade; };
    return r;
}();

std::unique_ptr<nr::ini_edit::Ini> page_ini{};
std::unique_ptr<nr::control_ui::ChainEditor> chain_editor{};
nr::ini_edit::KeyValues page_renodx{};   // [RenoDX]: NR on / off and the screenshot keys (all layers)
int64_t page_loaded{-1};
std::string report_said{};

// The page's facts besides the layers' rows (those come with the frames): the network, a build, the video memory.
void fill_view() {
    view.route = 1;
    if (session) {
        const nr::pe::Session::State st = session->state();
        view.net_running = st.running ? uint32_t(st.running_net) : nr::control::kNoNet;
        // [Network] Model as the file has it (the session's want_net is the last call's: a layer's own network)
        view.net_want = uint32_t(std::clamp(config.network.model, 0, nr::kNetworkChoices - 1));
        view.net_known = 1;
        for (int n = 0; n < nr::kNetworkChoices; ++n)
            std::snprintf(view.net_why[n], sizeof view.net_why[n], "%s", st.unavailable[n].c_str());
        view.building = st.building ? 1u : 0u;
        view.building_net = st.building_net >= 0 ? uint32_t(st.building_net) : nr::control::kNoNet;
        char phase[128];
        view.build_progress = st.building ? build_fraction(st, phase, sizeof phase) : 0.f;
        std::snprintf(view.build_phase, sizeof view.build_phase, "%s", st.building ? phase : "");
        view.gpu_ms = session->gpu_ms();
        view.network_ms = session->network_ms();
    }
    if (layered && layers_now.empty()) {   // no layer on: nothing of the last run's on the page
        view.gpu_ms = view.network_ms = 0.f;
        view.net_running = nr::control::kNoNet;
    }
    std::snprintf(view.state, sizeof view.state, "%s", last_status.c_str());
    std::snprintf(view.screenshot, sizeof view.screenshot, "%s", nr::pe::screenshot::status());
    static uint64_t vram_at = 0;   // four times a second (a driver query)
    if (const uint64_t now = GetTickCount64(); now - vram_at >= 250) {
        vram_at = now;
        uint64_t by[nr::vram::kOwners][nr::vram::kKinds];
        nr::vram::totals(by);
        for (int o = 0; o < nr::vram::kOwners; ++o)
            for (int k = 0; k < nr::vram::kKinds; ++k) view.vram_kib[o][k] = uint32_t(by[o][k] >> 10);
        uint64_t usage = 0, budget = 0, heap = 0;
        if (session && session->video_memory(&usage, &budget, &heap)) {
            view.vram_process_kib = uint32_t(usage >> 10);
            view.vram_budget_kib = uint32_t(budget >> 10);
            view.vram_heap_kib = uint32_t(heap >> 10);
        }
        view.vram_known = 1;
    }
}

void draw_page();
void draw_overlay(effect_runtime*) {
    nr::pe::watchdog::Phase phase("the add-on's page (ReShade overlay)");
    std::lock_guard<std::mutex> guard(lock);
    draw_page();
}
// NR_ADDON_PAGE_TRACE=1 (tests): the page's first three draws say each step in the log
void page_trace(const char* step) {
    static const bool on = [] { char v[4] = {}; return GetEnvironmentVariableA("NR_ADDON_PAGE_TRACE", v, 4) && v[0] == '1'; }();
    static int draws = 0;
    if (!on) return;
    if (std::strcmp(step, "begin") == 0) ++draws;
    if (draws <= 3) log("[nr] page trace %d: %s", draws, step);
}
// The page itself (the lock held)
void draw_page() {
    page_trace("begin");
    if (config_path.empty()) {
        ImGui::TextDisabled("dlssnr-amd.ini was not found next to the add-on.");
        return;
    }
    if (!page_ini) {
        page_ini = std::make_unique<nr::ini_edit::Ini>(config_path);
        chain_editor = std::make_unique<nr::control_ui::ChainEditor>(*page_ini, kReShadeChain);
    }
    if (page_ini->refresh() || page_loaded < 0) { page_renodx.load(*page_ini); page_loaded = page_loaded < 0 ? 1 : page_loaded + 1; }
    fill_view();
    page_trace("view");

    // What runs: NR on or off, the frame, the layers as they last ran and what they cost, a build, the video memory
    ImGui::Text("Neural Rendering: %s", view.nr_on ? "on" : "off");
    if (view.width)
        ImGui::Text("Frame: %ux%u, %s, %s", view.width, view.height, panel_facts.format.c_str(), space_name(back_buffer_space));
    page_trace("lines");
    static const int skip = [] { char v[8] = {}; return GetEnvironmentVariableA("NR_ADDON_PAGE_SKIP", v, 8) ? std::atoi(v) : 0; }();
    if (!(skip & 1)) {
        nr::control_ui::draw_layer_status(&view);
        nr::control_ui::draw_flapping();
        nr::control_ui::draw_missing_data(view.layers, nr::control::kMaxLayers, page_ini.get());
    }
    page_trace("layer status");
    if (session) {
        const nr::pe::Session::State st = session->state();
        if (st.building) {
            char phase[128];
            const float f = build_fraction(st, phase, sizeof phase);
            ImGui::Text("Building %s", st.building_net >= 0 ? network_name(st.building_net) : "the network");
            ImGui::ProgressBar(f, ImVec2(-1.0f, 0.0f), phase);
        }
    }
    if (!last_status.empty()) ImGui::TextWrapped("%s", last_status.c_str());
    ImGui::TextDisabled("DLSS5_Feed: %s; motion %s, depth %s. Frames: %u with NR, %u without",
                        technique.handle ? "found" : "MISSING", mv_var.handle ? "found" : "missing",
                        depth_var.handle ? "found" : "missing", frames_run, frames_declined);
    page_trace("feed line");
    if (!(skip & 2)) {
        nr::control_ui::draw_vram(&view);
        nr::control_ui::draw_residency();
    }
    page_trace("vram");
    ImGui::Separator();

    if (skip & 8) {   // tests: a table of the page's shape, nothing else
        if (ImGui::BeginTable("probe", 2, ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted("probe");
            ImGui::EndTable();
        }
        page_trace("probe table");
    }
    if (!(skip & 4)) chain_editor->draw_chain();
    page_trace("chain");
    chain_editor->draw_layer_page(&view, page_loaded, ImGui::GetStyleColorVec4(ImGuiCol_CheckMark));
    page_trace("layer page");

    ImGui::Spacing();
    ImGui::SeparatorText("Global");
    nr::control_ui::draw_preprocess(*page_ini);
    page_trace("Preprocess");
    {   // HUD FIX: this route has no frame generation's frames; Captured follows D3D10/11/12's draws
        nr::control_ui::HudFixMenu hm;
        hm.default_mode = 0;
        hm.why[2] = "this route sees the game's frames only, not frame generation's";
        {
            std::lock_guard<std::mutex> g(hudfix.now_m);
            hm.now = hudfix.now;
        }
        nr::control_ui::draw_hudfix(*page_ini, hm);
        page_trace("HUD FIX");
    }
    nr::control_ui::NetworkOptions net;
    net.resize_help = "[Network] Resize History. When the size of NR's frame changes (on this route: the game's resolution):\n"
                      "Off (default): NR's previous result is dropped and NR starts again at the new size, as NVIDIA's DLL does.\n"
                      "On: the previous result is scaled to the new size and kept.\n"
                      "It matters for dynamic resolution with NR before the upscaler (OptiScaler route); here sizes change rarely.";
    nr::control_ui::draw_network_options(*page_ini, &view, net);
    page_trace("network");
    // The ReShade effect that makes the motion vectors ([DlssNr] MotionVectors), applied at the next present
    if (ImGui::CollapsingHeader("Motion vectors", ImGuiTreeNodeFlags_DefaultOpen)) {
        static const char* const sources[] = {"VORT", "LumeniteFX Kernel"};
        static const char* const words[] = {"vort", "lumenite"};
        const bool installed = lumenite_on_disk();
        int chosen = -1;
        if (ImGui::BeginCombo("Source##motion", sources[std::clamp(config.motion_vectors, 0, 1)])) {
            if (ImGui::Selectable(sources[0], config.motion_vectors == 0)) chosen = 0;
            ImGui::BeginDisabled(!installed);
            if (ImGui::Selectable(sources[1], config.motion_vectors == 1)) chosen = 1;
            ImGui::EndDisabled();
            if (!installed && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("LumeniteFX is not installed. install.sh --lumenite installs it from its author's GitHub.");
            ImGui::EndCombo();
        }
        nr::control_ui::help("[DlssNr] MotionVectors. The ReShade effect that estimates the motion NR's history follows.\n"
                             "VORT comes with this package. LumeniteFX Kernel (by Afzaal) is installed separately,\n"
                             "with install.sh --lumenite, from its author's GitHub.\n"
                             "Switching makes ReShade compile its effects again (NR pauses until they are ready)\n"
                             "and restarts NR's history.");
        if (chosen >= 0) {   // choosing LumeniteFX again after it was not used retries it
            page_ini->set("DlssNr", "MotionVectors", words[chosen]);
            page_ini->save();
            config.motion_vectors = chosen;
            motion_choice_changed();
            log("[nr] motion vectors: %s chosen in the add-on page", sources[chosen]);
        }
        if (config.motion_vectors == 1 && motion.fell_back)
            ImGui::TextWrapped("LumeniteFX is not used this session: %s. VORT runs instead; choose LumeniteFX "
                               "again to retry.", motion.reason.c_str());
        else if (motion.lumenite_active)
            ImGui::TextDisabled("LumeniteFX Kernel%s", motion.verified ? ": its vectors follow the picture" : "");
    }
    page_trace("motion");
    nr::control_ui::draw_nr_switch(*page_ini, page_renodx, [] { nr::pe::screenshot::request(); }, nr::pe::screenshot::status());
    page_trace("NR on / off and screenshot");
    if (ImGui::CollapsingHeader("Diagnostics")) {
        nr::control_ui::draw_events();
        if (ImGui::Button("Copy a report")) {
            const std::string folder = nr::pe::module_folder();
            report_said = nr::control_ui::copy_report(
                nr::control_ui::report_text(&view, "dlssnr_amd.addon", config_path, nr::pe::log_path()),
                folder + "\\dlssnr-amd\\nr-report.txt");
        }
        nr::control_ui::help("For a bug report: the layers as they run, their engine data, the video memory and its "
                             "residency, the events above, dlssnr-amd.ini and the last lines of dlssnr-amd.log, as text. "
                             "Copied to the clipboard and saved as dlssnr-amd\\nr-report.txt next to the game. Nothing "
                             "is sent anywhere.");
        if (!report_said.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", report_said.c_str()); }
    }
    page_trace("Diagnostics");
    nr::control_ui::draw_inspect(*page_ini, "What is compared. The final picture: every layer (all run at Present on "
                                            "this route), all off against all on. A layer: its own input and output.");
    page_trace("Compare");
    nr::control_ui::draw_log(*page_ini);
    page_trace("Log");
    ImGui::Separator();
    ImGui::TextDisabled("Changes are saved to dlssnr-amd.ini at once.");
    page_ini->save();
    page_trace("end");
}

// ---- tests: the page drawn, changed and photographed without a keyboard (NR_ADDON_PAGE_AT=<present>) ----------------
// At that present of the bound runtime, counted from NR's first frame (1: when the add-on loads): NR_ADDON_PAGE_SET
// ("Section.Key=Value,...") is written through the page's file and NR_ADDON_PAGE_MATERIALIZE=1 writes the route's one
// layer as [Layer1], as the page's first change does; with
// NR_ADDON_PAGE_SHOTS=<n>, ReShade's overlay opens with the page in a window of its own (the left 60 % of the screen) and
// n screenshots are taken (ReShade's own, with the overlay: [SCREENSHOT] SaveOverlayShot = 1) of the page drawn in a
// window of its own over the left 60 % of the screen, scrolled down by most of a window each time.
struct PageTest {
    uint64_t at = 0;
    int shots = 0, scroll = 0;
    bool open = false;
    std::string set;
    bool materialize = false;
};
PageTest page_test = [] {
    PageTest p;
    char v[512] = {};
    if (GetEnvironmentVariableA("NR_ADDON_PAGE_AT", v, sizeof v)) p.at = std::strtoull(v, nullptr, 10);
    if (GetEnvironmentVariableA("NR_ADDON_PAGE_SHOTS", v, sizeof v)) p.shots = std::atoi(v);
    if (GetEnvironmentVariableA("NR_ADDON_PAGE_SET", v, sizeof v)) p.set = v;
    if (GetEnvironmentVariableA("NR_ADDON_PAGE_MATERIALIZE", v, sizeof v)) p.materialize = v[0] == '1';
    return p;
}();
// From the overlay event (the lock held), while the shots are being taken
void page_window() {
    if (page_test.shots <= 0 || !page_test.open) return;
    const ImVec2 d = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(d.x * 0.6f, d.y), ImGuiCond_Always);
    ImGui::SetNextWindowFocus();
    page_trace("window");
    if (ImGui::Begin("DLSSNR-AMD page (test)", nullptr,
                     ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse)) {
        ImGui::SetScrollY(float(page_test.scroll) * d.y * 0.85f);
        draw_page();
    }
    ImGui::End();
}
void page_test_writes() {
    if (!page_ini && !config_path.empty()) {
        page_ini = std::make_unique<nr::ini_edit::Ini>(config_path);
        chain_editor = std::make_unique<nr::control_ui::ChainEditor>(*page_ini, kReShadeChain);
    }
    if (page_ini) {
        page_ini->refresh();
        std::string item;
        std::stringstream ss(page_test.set);
        while (std::getline(ss, item, ',')) {
            const auto dot = item.find('.'), eq = item.find('=');
            if (dot != std::string::npos && eq != std::string::npos && dot < eq)
                page_ini->set(item.substr(0, dot), item.substr(dot + 1, eq - dot - 1), item.substr(eq + 1));
        }
        if (page_test.materialize) chain_editor->materialize();
        page_ini->save();
        log("[nr] add-on page test: NR_ADDON_PAGE_SET %s%s written at present %llu", page_test.set.c_str(),
            page_test.materialize ? " and the one layer materialized" : "", (unsigned long long)presents);
    }
}
// At each present of the bound runtime (the lock held)
void page_test_present(effect_runtime* rt) {
    // counted from NR's first frame (the effects are compiled and the page has something to show)
    static uint64_t first = 0;
    if (!page_test.at || !have_run) return;
    if (!first) first = presents;
    if (presents - first < page_test.at) return;
    const uint64_t since = presents - first - page_test.at;
    if (since == 0) {
        if (page_test.at > 1) page_test_writes();
        if (page_test.shots > 0) {
            page_test.open = true;
            rt->open_overlay(true, input_source::keyboard);
        }
        return;
    }
    // shot i: scrolled at 30 + 20 i, taken 10 presents later
    for (int i = 0; i < page_test.shots; ++i) {
        if (since == uint64_t(30 + 20 * i)) page_test.scroll = i;
        if (since == uint64_t(40 + 20 * i)) {
            char postfix[16];
            std::snprintf(postfix, sizeof postfix, "page%d", i);
            rt->save_screenshot(postfix);
            log("[nr] add-on page test: screenshot %s at present %llu", postfix, (unsigned long long)presents);
        }
    }
}

// Two seconds of "Preprocess: ON/OFF" in the corner after it switches, menu open or not. Drawn by
// ReShade on the finished picture, so nothing processes it.
void on_reshade_present(effect_runtime* rt) {
    nr::pe::watchdog::beat();   // before the lock: a frame that cannot take it is a stall too
    nr::pe::watchdog::Phase phase("ReShade present");
    std::lock_guard<std::mutex> guard(lock);
    if (rt != bound) return;
    ++presents;
    page_test_present(rt);
    motion_sync(rt);
    if (session) session->idle();   // NR off for a while: [Network] Residency
}

void on_reshade_overlay(effect_runtime*) {
    std::lock_guard<std::mutex> guard(lock);
    for (auto& r : view.layers)
        if (r.number && r.state == 1 && (!layer_at.count(int(r.number)) || layer_at[int(r.number)] != presents)) {
            r.state = 3; r.gpu_ms = r.network_ms = 0.f;
            std::snprintf(r.note, sizeof r.note, "not run on this presented frame");
        }
    {   // [Inspect]'s marks (nr_control_pages.hpp draw_inspect_marks): the comparison's tags, a wilsjo2 layer's outlines
        const nr::pe::InspectConfig& ic = chain.inspect;
        nr::control_ui::InspectMarks mk;
        mk.compare = ic.compare; mk.split = ic.split; mk.swap = ic.swap; mk.tags = ic.tags; mk.tag_scale = ic.tag_scale;
        if (ic.spatial_outlines) {
            const int chosen = std::max(1, ic.layer);   // the final picture: the first layer's
            const nr::pe::Config* c = nullptr;
            if (!layered) c = config.renodx.enabled ? nullptr : &config;
            else
                for (const AddonLayer& l : layers_now)
                    if (l.number == chosen && !l.renodx) c = &l.cfg;
            const ImVec2 ds = ImGui::GetIO().DisplaySize;
            // only while that layer runs (its row: not waiting, off or left out)
            const nr::control::LayerStatus* row = view_row(chosen, false);
            if (c && row && row->state == 1 && spatial_at.count(chosen) && spatial_at[chosen].frame == presents) {
                mk.outlines = nr::control_ui::spatial_rects(spatial_at[chosen].layout, mk.centre, mk.work);
            }
        }
        if (mk.any()) nr::control_ui::draw_inspect_marks(mk);
    }
    nr::pe::watchdog::Phase phase("ReShade overlay (OSD)");
    page_window();   // tests (NR_ADDON_PAGE_SHOTS)
    // [Network] OSD, as NR Control's (nr_control_pages.hpp draw_osd_lines): after a network switch (hotkey, menu or
    // ini) the build's progress, then the running network(s) for 3 s (switch, the default); always (always); always with
    // each layer's GPU time (cost); nothing (off). A layer that only started or went is no switch.
    if (session && config.network.osd != 0) {
        static uint32_t seen_want = ~0u, seen_running = ~0u;
        static std::chrono::steady_clock::time_point show_until{};
        fill_view();
        const auto now = std::chrono::steady_clock::now();
        uint32_t running = 0;   // the layers' running networks as one number, 4 bits a layer by row (0: none yet)
        for (uint32_t i = 0; i < nr::control::kMaxLayers; ++i) {
            const auto& r = view.layers[i];
            if (r.number && r.net < nr::control::kNoNet) running |= (r.net + 1u) << (4 * i);
        }
        if (running && (view.net_want != seen_want || running != seen_running)) {
            bool switched = false;
            for (int i = 0; i < 8 && seen_running != ~0u; ++i) {
                const uint32_t x = (seen_running >> (4 * i)) & 15u, y = (running >> (4 * i)) & 15u;
                switched |= x && y && x != y;
            }
            if (seen_want != ~0u && seen_running != ~0u && (view.net_want != seen_want || switched))
                show_until = now + std::chrono::seconds(3);
            seen_want = view.net_want;
            seen_running = running;
        }
        // a build shows its bar from the first one on (the game's first start: no network has run yet)
        if (nr::control_ui::osd_switching(&view) || (running && (now < show_until || config.network.osd >= 2))) {
            ImGui::SetNextWindowPos(ImVec2(24.0f, 64.0f));
            ImGui::SetNextWindowBgAlpha(0.65f);
            ImGui::Begin("##nr-osd", nullptr,
                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
            nr::control_ui::draw_osd_lines(&view, config.network.osd);
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
    nr::pe::watchdog::Phase phase("ReShade technique DLSS5_Feed (NR)");
    std::lock_guard<std::mutex> guard(lock);
    const nr::Controls& controls = settings();
    // With the RenoDX-style option NR's switch is [RenoDX] NeuralUplift ([DlssNr]'s does not apply).
    if (!controls.enabled && !renodx_on && !layered) {
        view.nr_on = 0;
        view_layers({1});
        note_layer(1, true, 3, "[DlssNr] Enabled is off", 0, 0, 0, config.white_point);
        return;
    }
    if (!bound) { bound = rt; resolve(rt); }
    if (rt != bound) return;
    if (technique.handle == 0) {
        // Re-ask once in a while: the effect may have been added after the
        // runtime initialised without a reload event we saw.
        static unsigned since = 0;
        if (++since % 300 == 0) resolve(rt);
        return;
    }
    if (t.handle != technique.handle) {
        // HUD FIX's capture: an effect before DLSS5_Feed changes the frame after the copy (the two are no longer of
        // one stage); the motion-vector providers write their own textures only
        if (cap.armed.load(std::memory_order_relaxed) && cap.earlier_effect.empty() && cap.fed_at != presents) {
            char fx[256] = {};
            size_t n = sizeof fx;
            rt->get_technique_effect_name(t, fx, &n);
            std::string f = fx;
            for (char& c : f) c = char(std::tolower(static_cast<unsigned char>(c)));
            static const char* const providers[] = {"vort_motion", "lumenite_kernel", "lumenite_quantmotion", "martysmods_launchpad",
                                                    "quint_motionvectors", "dh_uber_motion", "reshademotionestimation",
                                                    "dlss5_"};
            bool provider = false;
            for (const char* pv : providers) {
                std::string q = pv;
                for (char& c : q) c = char(std::tolower(static_cast<unsigned char>(c)));
                provider |= f.rfind(q, 0) == 0;
            }
            if (!provider) cap.earlier_effect = fx;
        }
        return;
    }
    {
        nr::pe::watchdog::Phase probe("motion-vector data check");
        motion_probe(rt, cl, rtv);
    }
    feed(rt, cl, rtv);
}

}  // namespace

// ReShade calls this before its FreeLibrary of the add-on (addon_manager.cpp unload_addons), outside the loader lock:
// the stall watcher, which holds this module while it runs, ends here, so the FreeLibrary unloads it as before (and
// DLL_PROCESS_DETACH takes the vulkan-1 hooks with it, DIRT 5).
extern "C" __declspec(dllexport) void AddonUninit(HMODULE, HMODULE) { nr::pe::watchdog::stop_and_wait(); }

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        nr::pe::set_module(module);
        nr::pe::set_hudfix_default(0);   // HUD FIX off by default on the ReShade route
        chain.hud.mode = 0;
        if (!reshade::register_addon(module)) return FALSE;
        previous_filter = SetUnhandledExceptionFilter(&crash_filter);
        log("[nr] ---------------------------------------------------------------");
        log("[nr] ReShade add-on loaded from %s", nr::pe::module_folder());
        nr::pe::watchdog::start();
        const std::string folder = nr::pe::module_folder();
        if (!folder.empty()) {
            config_path = folder + "\\dlssnr-amd.ini";
            config.load(config_path);
            log("[nr] settings file %s: enabled=%d intensity=%.2f history=%.2f", config_path.c_str(),
                int(config.controls.enabled), config.controls.intensity, config.history);
            renodx_on = config.renodx.enabled;
            if (renodx_on) log("[nr] RenoDX-style option on ([RenoDX] Enabled = 1 at start)");
            if (page_test.at == 1) page_test_writes();   // tests: before the first frame
        }
        // ReShade as a Vulkan layer loads add-ons inside its vkCreateInstance
        // hook: vulkan-1.dll is in the process and the game's vkCreateDevice
        // has not happened yet, which is exactly when the device watcher's
        // hooks have to go in. ReShade as dxgi.dll over DXVK/vkd3d-proton finds
        // vulkan-1 loaded as well, but takes its Vulkan handles from DXVK and has no use for
        // the watch - and must not get it: ReShade unloads the add-on when the game's first
        // (probe) device goes away and loads it again at another address, and a hook that
        // something else put on top of the watch's detours (it follows jumps) then writes its
        // saved bytes back into the second copy's code. Only the layer's own module name marks
        // the layer route.
        if (GetModuleHandleW(L"vulkan-1.dll") &&
            (GetModuleHandleW(L"ReShade64.dll") || GetModuleHandleW(L"ReShade32.dll"))) {
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
        // HUD FIX's capture: the game's writes on its back buffers
        reshade::register_event<reshade::addon_event::barrier>(on_cap_barrier);
        reshade::register_event<reshade::addon_event::bind_pipeline_states>(on_cap_states);
        reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(on_cap_bind_targets);
        reshade::register_event<reshade::addon_event::begin_render_pass>(on_cap_begin_pass);
        reshade::register_event<reshade::addon_event::end_render_pass>(on_cap_end_pass);
        reshade::register_event<reshade::addon_event::bind_pipeline>(on_cap_bind_pipeline);
        reshade::register_event<reshade::addon_event::bind_viewports>(on_cap_viewports);
        reshade::register_event<reshade::addon_event::bind_scissor_rects>(on_cap_scissors);
        reshade::register_event<reshade::addon_event::draw>(on_cap_draw);
        reshade::register_event<reshade::addon_event::draw_indexed>(on_cap_draw_indexed);
        reshade::register_event<reshade::addon_event::draw_or_dispatch_indirect>(on_cap_indirect);
        reshade::register_event<reshade::addon_event::clear_render_target_view>(on_cap_clear);
        reshade::register_event<reshade::addon_event::copy_resource>(on_cap_copy_resource);
        reshade::register_event<reshade::addon_event::copy_texture_region>(on_cap_copy_texture);
        reshade::register_event<reshade::addon_event::copy_buffer_to_texture>(on_cap_copy_buffer);
        reshade::register_event<reshade::addon_event::resolve_texture_region>(on_cap_resolve);
        reshade::register_event<reshade::addon_event::update_texture_region>(on_cap_update);
        reshade::register_event<reshade::addon_event::execute_command_list>(on_cap_execute);
        reshade::register_event<reshade::addon_event::execute_secondary_command_list>(on_cap_execute_secondary);
        reshade::register_event<reshade::addon_event::reset_command_list>(on_cap_reset_list);
        reshade::register_event<reshade::addon_event::destroy_command_list>(on_cap_destroy_list);
        reshade::register_event<reshade::addon_event::init_pipeline>(on_cap_init_pipeline);
        reshade::register_event<reshade::addon_event::destroy_pipeline>(on_cap_destroy_pipeline);
        reshade::register_event<reshade::addon_event::destroy_swapchain>(on_cap_destroy_swapchain);
        reshade::register_overlay(nullptr, draw_overlay);
        reshade::register_event<reshade::addon_event::reshade_overlay>(on_reshade_overlay);
    } else if (reason == DLL_PROCESS_DETACH) {
        // ReShade unloads add-ons when its last device is destroyed, and the
        // game goes on: the vulkan-1 hooks must not outlive this module. At
        // process exit (reserved != null) the other threads are gone and
        // nothing calls into vulkan-1 any more, so it is left alone.
        // At process exit the session is left as it is (its totals logged), not destroyed: the game's other threads
        // are gone, possibly inside vkd3d-proton's / DXVK's locks, and its destructor's GPU waits and queue locks
        // could wait for ever (the game would never end). The process's end frees it.
        if (reserved) {
            if (session) {
                session->log_totals();
                (void)session.release();
            }
            (void)hudfix.stage.release();
            (void)pq.stage.release();
            (void)inspect_fit.stage.release();
        }
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
        reshade::unregister_event<reshade::addon_event::init_device>(on_init_device);
        reshade::unregister_event<reshade::addon_event::create_swapchain>(on_create_swapchain);
        reshade::unregister_event<reshade::addon_event::init_swapchain>(on_init_swapchain);
        reshade::unregister_event<reshade::addon_event::init_effect_runtime>(on_init_effect_runtime);
        reshade::unregister_event<reshade::addon_event::destroy_effect_runtime>(on_destroy_effect_runtime);
        reshade::unregister_event<reshade::addon_event::reshade_reloaded_effects>(on_reloaded_effects);
        reshade::unregister_event<reshade::addon_event::reshade_render_technique>(on_render_technique);
        reshade::unregister_event<reshade::addon_event::reshade_present>(on_reshade_present);
        reshade::unregister_event<reshade::addon_event::destroy_device>(on_destroy_device);
        reshade::unregister_event<reshade::addon_event::barrier>(on_cap_barrier);
        reshade::unregister_event<reshade::addon_event::bind_pipeline_states>(on_cap_states);
        reshade::unregister_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(on_cap_bind_targets);
        reshade::unregister_event<reshade::addon_event::begin_render_pass>(on_cap_begin_pass);
        reshade::unregister_event<reshade::addon_event::end_render_pass>(on_cap_end_pass);
        reshade::unregister_event<reshade::addon_event::bind_pipeline>(on_cap_bind_pipeline);
        reshade::unregister_event<reshade::addon_event::bind_viewports>(on_cap_viewports);
        reshade::unregister_event<reshade::addon_event::bind_scissor_rects>(on_cap_scissors);
        reshade::unregister_event<reshade::addon_event::draw>(on_cap_draw);
        reshade::unregister_event<reshade::addon_event::draw_indexed>(on_cap_draw_indexed);
        reshade::unregister_event<reshade::addon_event::draw_or_dispatch_indirect>(on_cap_indirect);
        reshade::unregister_event<reshade::addon_event::clear_render_target_view>(on_cap_clear);
        reshade::unregister_event<reshade::addon_event::copy_resource>(on_cap_copy_resource);
        reshade::unregister_event<reshade::addon_event::copy_texture_region>(on_cap_copy_texture);
        reshade::unregister_event<reshade::addon_event::copy_buffer_to_texture>(on_cap_copy_buffer);
        reshade::unregister_event<reshade::addon_event::resolve_texture_region>(on_cap_resolve);
        reshade::unregister_event<reshade::addon_event::update_texture_region>(on_cap_update);
        reshade::unregister_event<reshade::addon_event::execute_command_list>(on_cap_execute);
        reshade::unregister_event<reshade::addon_event::execute_secondary_command_list>(on_cap_execute_secondary);
        reshade::unregister_event<reshade::addon_event::reset_command_list>(on_cap_reset_list);
        reshade::unregister_event<reshade::addon_event::destroy_command_list>(on_cap_destroy_list);
        reshade::unregister_event<reshade::addon_event::init_pipeline>(on_cap_init_pipeline);
        reshade::unregister_event<reshade::addon_event::destroy_pipeline>(on_cap_destroy_pipeline);
        reshade::unregister_event<reshade::addon_event::destroy_swapchain>(on_cap_destroy_swapchain);
        reshade::unregister_addon(module);
    }
    return TRUE;
}
