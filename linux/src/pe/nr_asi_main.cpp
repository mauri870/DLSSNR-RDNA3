// dlssnr-amd.asi: our NR as a plugin of upstream OptiScaler, which loads every *.asi in <game>\OptiScaler\plugins
// ([Plugins] LoadAsiPlugins=true) from its own DllMain and calls their InitializeASI there. OptiScaler stays as it
// is and does the upscaling and frame generation; NR lives in its calls:
//   - OptiScaler's exported NVSDK_NGX_D3D12_EvaluateFeature is the one function every input of it reaches: the
//     game's own DLSS calls, and OptiScaler's FSR2 / FSR3 / FidelityFX API / XeSS inputs, which call the export
//     (checked in the binary: every call site is a direct call, none inlined). Detoured, it gives the game's
//     command list and parameter block before and after OptiScaler's upscaler: NR runs on the colour before it
//     ([RenoDX] NRHookPoint = 1) or on the output after it (0, the default), on the same command list, or at the
//     game's Present (2). The NGX Vulkan and D3D11 exports the same way for Vulkan and D3D11 games (their sections).
//   - CreateFeature / ReleaseFeature tell the features apart: the export also serves frame generation (DLSSG
//     replacement handles), and NR runs on SuperSampling and Ray Reconstruction only. OptiScaler's FSR / FidelityFX
//     inputs make their features through an inlined copy of CreateFeature, so a handle first seen in an evaluate
//     that OptiScaler itself makes is one of those (they make nothing but SuperSampling).
//   - NR runs in one of two styles, chosen per frame by [RenoDX] Enabled in dlssnr-amd.ini (no key: 1):
//       1: the RenoDX style, the RenoDX-style processing of the OptiScaler route as it is (nr_dlssnr_model.cpp
//          evaluate_d3d12 with the RenoDX-style option on): what that route's OptiScaler hook told it about the frame
//          (nr_pe_renodx_route.hpp) is worked out here from the game's own block, as the forked OptiScaler worked it
//          out; placement by [RenoDX] NRHookPoint.
//       0: the wilsjo2 style, our re-implementation of the forked OptiScaler's own NR processing around the model
//          (nr_asi_wilsjo2.hpp); its settings in [wilsjo2], placement by
//          [wilsjo2] RunBeforeSR.
//     Each style keeps its own model features (a switch starts the history again).
//   - The network's Vulkan commands keep the command list's D3D12 state (Session::set_keep_caller_state): this
//     module records outside OptiScaler's own state restore.
//   - NR Control (End) is drawn by this module over the game's swap chain (nr_pe_menu.cpp), the Vulkan layer's menu
//     stepping aside while it does.
//   - With the int4 network allowed (DLSSNR_INT4 not 0), VK_KHR_pipeline_binary and VK_KHR_maintenance5 are added to
//     the game's Vulkan device as the int4 Vulkan layer adds them (nr_int4_layer.cpp): this module is loaded before
//     the game makes its D3D12 device, so vkd3d-proton's vkCreateDevice can be wrapped where it gets it.
// Every file stays where the OptiScaler route keeps it: the module folder is OptiScaler's (dlssnr-amd.ini,
// dlssnr-amd\, dlssnr-amd.log, control.bin for NR Control).
#include "MinHook.h"
#include "nr_asi_wilsjo2.hpp"
#include "nr_device_features.hpp"
#include "nr_dlssnr_model.hpp"
#include "nr_pe_session.hpp"
#include "nr_ini_network.hpp"
#include "nr_ngx_abi.hpp"
#include "nr_pe_config.hpp"
#include "nr_pe_control.hpp"
#include "nr_asi_hudcap.hpp"
#include "nr_pe_interop.hpp"
#include "nr_pe_vkdevice.hpp"
#include "nr_pe_log.hpp"
#include "nr_pe_menu.hpp"
#include "nr_pe_events.hpp"
#include "nr_pe_watchdog.hpp"
#include "nr_pe_renodx_route.hpp"
#include "nr_present_cadence.hpp"
#include "nr_screenshot.hpp"
#include "nr_test_fault.hpp"
#include "nr_vram_ledger.hpp"
#include "nr_vk_receipts.hpp"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#ifndef NR_BUILD_STAMP
#define NR_BUILD_STAMP "unknown"
#endif

namespace {

using nr::pe::log;
using nr::pe::test_fault;
using nr::dlssnr::Feature;
using nr::dlssnr::Rect;

HMODULE g_self = nullptr;
HMODULE g_opti = nullptr;   // OptiScaler: the module that called InitializeASI

// The DLSS flags (NVSDK_NGX_DLSS_Feature_Flags) this module reads.
constexpr uint32_t kFlagIsHdr = 1u << 0, kFlagMvLowRes = 1u << 1, kFlagDepthInverted = 1u << 3,
                   kFlagAutoExposure = 1u << 6;
constexpr int kFeatureSuperSampling = 1, kFeatureFrameGeneration = 11, kFeatureRayReconstruction = 13;
constexpr int kEngineUnreal = 1;   // NVSDK_NGX_EngineType

// ---- the states OptiScaler holds the game's resources in during its evaluate --------------------------------------
// As its upscalers assume them (upscalers/*Feature_Dx12.cpp, OptiScaler c4668477; to check with every OptiScaler
// update): [Hotfix] <Name>ResourceBarrier in OptiScaler.ini when it is set; its per-game defaults
// (misc/GameConfigs.cpp); for an Unreal Engine game the colour in RENDER_TARGET and the motion vectors in
// UNORDERED_ACCESS; otherwise NON_PIXEL_SHADER_RESOURCE, and the output in UNORDERED_ACCESS. OptiScaler's FSR3 input
// takes the colour's state from the game's FSR3 call, which this module does not see: NON_PIXEL_SHADER_RESOURCE is
// assumed there. vkd3d-proton keeps every image in one layout on a device with VK_KHR_unified_image_layouts (RADV),
// so a wrong guess costs synchronisation at most, and the network's recording is fenced by full barriers.
struct States {
    D3D12_RESOURCE_STATES color = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    D3D12_RESOURCE_STATES depth = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    D3D12_RESOURCE_STATES motion = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    D3D12_RESOURCE_STATES exposure = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    D3D12_RESOURCE_STATES output = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
};
struct IniBarriers { int color = -1, depth = -1, motion = -1, exposure = -1, output = -1; };   // -1: not set
IniBarriers g_ini;
bool g_unreal_by_exe = false;            // OptiScaler's own judgement of the executable, mirrored
bool g_game_colour_readable = false;     // its per-game defaults: colour stays NON_PIXEL_SHADER_RESOURCE
bool g_game_motion_readable = false;     // ... motion vectors too
std::atomic<int> g_ngx_engine{-1};       // the engine the game told NGX (Init with a project id)

std::wstring lower(std::wstring s) {
    for (wchar_t& c : s) c = wchar_t(std::towlower(c));
    return s;
}
bool contains_any(const std::wstring& s, std::initializer_list<const wchar_t*> words) {
    for (const wchar_t* w : words)
        if (s.find(w) != std::wstring::npos) return true;
    return false;
}

// OptiScaler's Util.cpp game-engine check, for the one thing it decides here (Unity first, then Unreal).
void judge_executable() {
    wchar_t path[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH * 2);
    const std::wstring full = path;
    const size_t slash = full.find_last_of(L"\\/");
    const std::wstring exe = lower(slash == std::wstring::npos ? full : full.substr(slash + 1));
    const std::wstring dir = slash == std::wstring::npos ? std::wstring() : full.substr(0, slash);
    auto version_string = [&](const wchar_t* name) -> std::wstring {
        DWORD handle = 0;
        const DWORD size = GetFileVersionInfoSizeW(path, &handle);
        if (!size) return {};
        std::vector<uint8_t> data(size);
        if (!GetFileVersionInfoW(path, 0, size, data.data())) return {};
        struct Translation { WORD language, codepage; }* tr = nullptr;
        UINT n = 0;
        if (!VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<LPVOID*>(&tr), &n) || !tr ||
            n < sizeof(Translation))
            return {};
        wchar_t query[128];
        std::swprintf(query, 128, L"\\StringFileInfo\\%04x%04x\\%ls", tr->language, tr->codepage, name);
        LPWSTR value = nullptr;
        UINT len = 0;
        if (VerQueryValueW(data.data(), query, reinterpret_cast<LPVOID*>(&value), &len) && value && len) return value;
        return {};
    };
    const std::wstring product = lower(version_string(L"ProductName")), copyright = lower(version_string(L"LegalCopyright")),
                       company = lower(version_string(L"CompanyName")), description = lower(version_string(L"FileDescription"));
    const bool unity = GetFileAttributesW((dir + L"\\UnityPlayer.dll").c_str()) != INVALID_FILE_ATTRIBUTES ||
                       GetFileAttributesW((dir + L"\\GameAssembly.dll").c_str()) != INVALID_FILE_ATTRIBUTES ||
                       contains_any(product, {L"unity"}) || contains_any(company, {L"unity"}) ||
                       contains_any(copyright, {L"unity"}) || contains_any(description, {L"unity"});
    if (unity) return;
    auto ends = [&](const wchar_t* tail) {
        const size_t t = std::wcslen(tail);
        return exe.size() >= t && exe.compare(exe.size() - t, t, tail) == 0;
    };
    const bool shipping = ends(L"-win64-shipping.exe") || ends(L"-wingdk-shipping.exe") || ends(L"-windows-shipping.exe") ||
                          ends(L"-win64-test.exe") || ends(L"-win64-debug.exe") || ends(L"-windows-test.exe") ||
                          ends(L"-windows-debug.exe");
    bool binaries = false;
    {
        const size_t a = dir.find_last_of(L"\\/");
        if (a != std::wstring::npos) {
            const std::wstring platform = lower(dir.substr(a + 1)), parent = dir.substr(0, a);
            const size_t b = parent.find_last_of(L"\\/");
            const std::wstring bins = lower(b == std::wstring::npos ? parent : parent.substr(b + 1));
            binaries = bins == L"binaries" && platform.rfind(L"win", 0) == 0;
        }
    }
    g_unreal_by_exe = contains_any(product, {L"unreal"}) || contains_any(company, {L"epic games"}) ||
                      contains_any(copyright, {L"epic games", L"unreal"}) || contains_any(description, {L"unreal"}) ||
                      shipping || binaries;
    // misc/GameConfigs.cpp: the Unreal games whose colour (and motion vectors) OptiScaler keeps in
    // NON_PIXEL_SHADER_RESOURCE, and the games it takes for Unreal whatever their executable says.
    const std::string name(exe.begin(), exe.end());
    auto ue = [&](const char* project) {
        return name == std::string(project) + "-win64-shipping.exe" || name == std::string(project) + "-wingdk-shipping.exe";
    };
    for (const char* p : {"returnal", "hellbladegame", "outriders", "medium", "sackboy", "pumpkinjack", "dungeonhaven", "pal7"})
        if (ue(p)) g_game_colour_readable = g_game_motion_readable = true;
    if (ue("avowed")) g_game_colour_readable = true;
    if (name == "observersystemredux.exe") g_game_colour_readable = g_game_motion_readable = true;
    if (name == "gwt.exe" || name == "ff7rebirth_.exe" || name == "soulstice.exe") g_unreal_by_exe = true;
}

// [Hotfix] *ResourceBarrier in OptiScaler.ini beside OptiScaler (auto / absent: not set).
void read_ini_barriers() {
    char ini[MAX_PATH + 32];
    std::snprintf(ini, sizeof ini, "%s\\OptiScaler.ini", nr::pe::module_folder());
    auto get = [&](const char* key) {
        char v[32] = {};
        GetPrivateProfileStringA("Hotfix", key, "", v, sizeof v, ini);
        return v[0] >= '0' && v[0] <= '9' ? std::atoi(v) : -1;
    };
    g_ini.color = get("ColorResourceBarrier");
    g_ini.depth = get("DepthResourceBarrier");
    g_ini.motion = get("MVResourceBarrier");
    g_ini.exposure = get("ExposureResourceBarrier");
    g_ini.output = get("OutputResourceBarrier");
}

States expected_states() {
    States s;
    const bool unreal = g_ngx_engine.load() == kEngineUnreal || g_unreal_by_exe;
    auto pick = [](int ini, bool game_readable, bool unreal_case, D3D12_RESOURCE_STATES unreal_state,
                   D3D12_RESOURCE_STATES plain) {
        if (ini >= 0) return D3D12_RESOURCE_STATES(ini);
        if (game_readable) return D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        return unreal_case ? unreal_state : plain;
    };
    s.color = pick(g_ini.color, g_game_colour_readable, unreal, D3D12_RESOURCE_STATE_RENDER_TARGET, s.color);
    s.motion = pick(g_ini.motion, g_game_motion_readable, unreal, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, s.motion);
    s.depth = pick(g_ini.depth, false, false, s.depth, s.depth);
    s.exposure = pick(g_ini.exposure, false, false, s.exposure, s.exposure);
    s.output = pick(g_ini.output, false, false, s.output, s.output);
    return s;
}

void transition(ID3D12GraphicsCommandList* cmd, ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    if (!r || from == to) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    cmd->ResourceBarrier(1, &b);
}

// A feature whose create was not seen (made before this module was loaded, or through an entry the hooks do not see):
// taken by what its evaluate carries, as the 0.0.7 route ran NR on every upscaling evaluate of OptiScaler's -
// DLSS-G's back buffer: frame generation; Ray Reconstruction's own inputs: Ray Reconstruction; else SuperSampling (a
// call without a colour and an output is not run on: the frame readers decline it). `has(name)`: the call carries it.
template <class Has> int unseen_feature(Has has) {
    if (has("DLSSG.Backbuffer")) return kFeatureFrameGeneration;
    for (const char* k : {"DLSSD.Color", "DLSSD.DiffuseAlbedo", "DLSSD.SpecularAlbedo", "GBuffer.DiffuseAlbedo",
                          "GBuffer.SpecularAlbedo"})
        if (has(k)) return kFeatureRayReconstruction;
    return kFeatureSuperSampling;
}
const char* feature_name(int f) {
    return f == kFeatureSuperSampling ? "SuperSampling" : f == kFeatureRayReconstruction ? "Ray Reconstruction"
           : f == kFeatureFrameGeneration ? "frame generation" : "another feature";
}

// ---- the game's parameter block --------------------------------------------------------------------------------
// A resource as OptiScaler reads it: Get(ID3D12Resource**), else Get(void**); `as_resource` says which answered.
ID3D12Resource* get_resource(const NVSDK_NGX_Parameter* p, const char* name, bool* as_resource = nullptr) {
    ID3D12Resource* r = nullptr;
    if (p->Get(name, &r) == NVSDK_NGX_Result_Success && r) {
        if (as_resource) *as_resource = true;
        return r;
    }
    void* v = nullptr;
    if (p->Get(name, &v) == NVSDK_NGX_Result_Success && v) {
        if (as_resource) *as_resource = false;
        return static_cast<ID3D12Resource*>(v);
    }
    return nullptr;
}
unsigned get_uint(const NVSDK_NGX_Parameter* p, const char* name, unsigned fallback = 0) {
    unsigned u = 0;
    if (p->Get(name, &u) == NVSDK_NGX_Result_Success) return u;
    int i = 0;
    if (p->Get(name, &i) == NVSDK_NGX_Result_Success) return unsigned(i);
    return fallback;
}
bool get_float(const NVSDK_NGX_Parameter* p, const char* name, float* out) {
    float f = 0.f;
    if (p->Get(name, &f) != NVSDK_NGX_Result_Success) return false;
    *out = f;
    return true;
}

// The colour formats the forked OptiScaler took as able to hold linear HDR (DlssNr_Pipeline_Dx12.h), with fix 7 of the
// 0.0.7 OptiScaler route (nr_pe_optifix.cpp): R9G9B9E5 - what RE Engine games such as Resident Evil Requiem hand their
// upscaler - and R32G32B32 typeless are float too. Both styles use this list.
bool float_colour(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        case DXGI_FORMAT_R32G32B32A32_FLOAT: case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        case DXGI_FORMAT_R32G32B32_FLOAT: case DXGI_FORMAT_R11G11B10_FLOAT:
        case DXGI_FORMAT_R9G9B9E5_SHAREDEXP: case DXGI_FORMAT_R32G32B32_TYPELESS:
            return true;
        default:
            return false;
    }
}

// ---- OptiScaler's features --------------------------------------------------------------------------------------
// One layer of the NR chain on one OptiScaler feature: its own model features and surfaces, nothing shared with any
// other layer.
struct LayerState {
    nr::pe::Processing processing = nr::pe::Processing::kRenoDX;
    Feature* nr[2] = {};                    // RenoDX: the model's feature for each placement: 0 after, 1 before
    nr::asi::Wilsjo2Owner* w2[2] = {};      // wilsjo2: its state for each placement
};
void release_layer(LayerState& l) {
    for (Feature*& f : l.nr)
        if (f) { nr::dlssnr::release_d3d12(f); f = nullptr; }
    for (nr::asi::Wilsjo2Owner*& o : l.w2)
        if (o) { nr::asi::wilsjo2_release(o); o = nullptr; }
}

// What the game's DLSS evaluate carries, for NR.
struct Frame {
    ID3D12Resource *color = nullptr, *output = nullptr, *depth = nullptr, *motion = nullptr, *exposure = nullptr;
    bool color_as_resource = true;
    const char* color_key = "Color";
    uint32_t color_x = 0, color_y = 0, render_w = 0, render_h = 0;
    uint32_t subrect_w = 0, subrect_h = 0;   // DLSS.Render.Subrect.Dimensions as given (0: not given)
    uint32_t out_x = 0, out_y = 0, out_w = 0, out_h = 0;
    Rect depth_rect, motion_rect;
    uint32_t flags = 0;
    bool flags_known = false;
    bool reset = false;
    float pre_exposure = 1.0f, exposure_scale = 1.0f, mv_x = 1.0f, mv_y = 1.0f;
};


// [Inspect] HoldFrame: one input of the chosen layer, kept (UNORDERED_ACCESS between uses) and given to it every frame.
struct HoldCopy {
    ID3D12Resource* guides[3] = {};
    Frame input;
    States input_states;
    nr::asi::Wilsjo2Frame w2;
    bool inputs_ready = false;
    uint32_t encoding = 0; float white = 0.f;
    ID3D12Resource* tex = nullptr;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    bool held = false;
    int layer = -1;          // the layer's row the frame was held for (another layer takes a frame of its own)
    ULONGLONG used_ms = 0;   // its last use; a pause alone does not replace the held input
};
// [Inspect] HoldFrame let go: the layer's history starts again at its next run (the fork returns to the live frame
// with a reset), so the held frame does not linger in it. g_lock.
std::set<int> g_hold_released;
bool hold_released(int row) { return g_hold_released.erase(row) != 0; }
// The sizes of a feature's create block (upstream RenoDX dlssnr.hpp, KCD2: an evaluate may carry the render size as
// OutWidth while the create block and the output resource say the display size; the SDK's evaluate helper never
// writes OutWidth): Width / Height and OutWidth / OutHeight, 0 not given.
struct CreateSizes { uint32_t in_w = 0, in_h = 0, out_w = 0, out_h = 0; };
CreateSizes create_sizes(const NVSDK_NGX_Parameter* p) {
    CreateSizes c;
    if (!p) return c;
    unsigned int v = 0;
    if (p->Get("Width", &v) == NVSDK_NGX_Result_Success) c.in_w = v;
    if (p->Get("Height", &v) == NVSDK_NGX_Result_Success) c.in_h = v;
    if (p->Get("OutWidth", &v) == NVSDK_NGX_Result_Success) c.out_w = v;
    if (p->Get("OutHeight", &v) == NVSDK_NGX_Result_Success) c.out_h = v;
    return c;
}
// NROutputRect: Auto uses create, then evaluate, then resource size. Declared uses evaluate then resource;
// Resource uses the allocation. Auto logs a disagreement between the create and evaluate sizes.
uint32_t output_rect_mode();
void output_size(const NVSDK_NGX_Parameter* p, const CreateSizes& c, uint32_t res_w, uint32_t res_h, uint32_t* w,
                 uint32_t* h) {
    unsigned int ew = 0, eh = 0;
    p->Get("OutWidth", &ew);
    p->Get("OutHeight", &eh);
    const uint32_t mode = output_rect_mode();
    if (mode == 2) { *w = res_w; *h = res_h; return; }
    if (mode == 1) { *w = ew ? ew : res_w; *h = eh ? eh : res_h; return; }
    if (c.out_w && c.out_h) {
        *w = c.out_w; *h = c.out_h;
        static int said = 0;
        if ((ew || eh) && (ew != c.out_w || eh != c.out_h) && said < 4) {
            ++said;
            log("[nr] the game's evaluate says its output is %ux%u, its create said %ux%u: the create's is taken (the "
                "output resource is %ux%u)", ew, eh, c.out_w, c.out_h, res_w, res_h);
        }
    } else {
        *w = ew ? ew : res_w;
        *h = eh ? eh : res_h;
    }
}

struct Tracked {
    int feature = -1;              // NVSDK_NGX_Feature; -1 not seen at create
    bool flags_known = false;
    uint32_t flags = 0;            // DLSS.Feature.Create.Flags
    CreateSizes create;            // its create block's sizes
    std::map<int, LayerState> layers;   // by layer number ([LayerN]; 0 the one layer of a file without any)
    uint64_t chain_version = 0;         // the chain the layers were last matched against
    ID3D12Resource* copy = nullptr;            // before the upscaler: the colour NR has run on, handed to it
    D3D12_RESOURCE_STATES copy_state = D3D12_RESOURCE_STATE_COMMON;
    ID3D12Resource* out_copy = nullptr;        // after the upscaler, for an output subrect: its region
    D3D12_RESOURCE_STATES out_copy_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;   // UNORDERED_ACCESS between uses
    // [Inspect]: the output before and after the compared layers (NON_PIXEL_SHADER_RESOURCE between uses)
    ID3D12Resource* inspect_off = nullptr;
    ID3D12Resource* inspect_on = nullptr;
    D3D12_RESOURCE_STATES inspect_off_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                          inspect_on_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    HoldCopy hold;                             // [Inspect] HoldFrame: the chosen layer's input, held
    uint64_t evaluates = 0;
    bool said = false;
};
std::mutex g_lock;
std::unordered_map<uint32_t, Tracked> g_features;

using PFN_Create = NVSDK_NGX_Result(NR_NGX_MSABI*)(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature,
                                                  NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
using PFN_Evaluate = NVSDK_NGX_Result(NR_NGX_MSABI*)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*,
                                                    NVSDK_NGX_Parameter*, void*);
using PFN_Release = NVSDK_NGX_Result(NR_NGX_MSABI*)(NVSDK_NGX_Handle*);
using PFN_InitProjectId = NVSDK_NGX_Result(NR_NGX_MSABI*)(const char*, int, const char*, const wchar_t*,
                                                         ID3D12Device*, uintptr_t, uintptr_t);
PFN_Create real_create = nullptr;
PFN_Evaluate real_evaluate = nullptr;
PFN_Release real_release = nullptr;
PFN_InitProjectId real_init_project_id = nullptr, real_init_with_project_id = nullptr;

// Our textures are retired, not released: the GPU may still read one for a frame in flight, in a command list the game
// has not even submitted yet. Released once 64 more evaluates went by AND 2 s passed (under g_lock; the evaluates are
// the clock, not the releases: OptiScaler releasing many features at once would otherwise run the count out at once).
nr::gpu::Uses g_d12_uses, g_native_uses;
struct RetiredResource {
    uint64_t at;
    ULONGLONG ms;
    ID3D12Resource* r;
    nr::gpu::Uses uses;
};
std::vector<RetiredResource> g_retired_resources;
uint64_t g_evaluate_clock = 0;

void free_retired_resources() {
    const ULONGLONG now = GetTickCount64();
    g_retired_resources.erase(std::remove_if(g_retired_resources.begin(), g_retired_resources.end(),
                                             [now](const RetiredResource& e) {
                                                 if (e.at > g_evaluate_clock || e.ms > now || !e.uses.ready()) return false;
                                                 e.r->Release();
                                                 return true;
                                             }),
                              g_retired_resources.end());
}

// The clock of both retired lists (ours and the wilsjo2 style's): every evaluate and every Present of the Present path.
void tick_retired() {
    g_d12_uses.touch();
    ++g_evaluate_clock;
    if (!g_retired_resources.empty()) free_retired_resources();
    nr::asi::wilsjo2_tick();
}

void release_resource(ID3D12Resource*& r) {
    if (r) g_retired_resources.push_back({g_evaluate_clock + 64, GetTickCount64() + 2000, r, g_d12_uses});
    r = nullptr;
}

void forget(Tracked& t) {
    for (auto& [n, l] : t.layers) release_layer(l);
    t.layers.clear();
    release_resource(t.copy);
    release_resource(t.out_copy);
    release_resource(t.inspect_off);
    release_resource(t.inspect_on);
    release_resource(t.hold.tex);
    for (auto& r : t.hold.guides) release_resource(r);
    t.hold.inputs_ready = false;
    if (t.hold.held) g_hold_released.insert(t.hold.layer);
    t.hold.held = false;
}

NVSDK_NGX_Result NR_NGX_MSABI hk_create(ID3D12GraphicsCommandList* cmd, NVSDK_NGX_Feature feature,
                                        NVSDK_NGX_Parameter* p, NVSDK_NGX_Handle** out) {
    const NVSDK_NGX_Result r = real_create(cmd, feature, p, out);
    if (r == NVSDK_NGX_Result_Success && out && *out) {
        std::lock_guard<std::mutex> guard(g_lock);
        Tracked& t = g_features[(*out)->Id];
        forget(t);
        t = Tracked{};
        t.feature = int(feature);
        t.create = create_sizes(p);
        if (p) {
            int flags = 0;
            if (p->Get("DLSS.Feature.Create.Flags", &flags) == NVSDK_NGX_Result_Success) {
                t.flags = uint32_t(flags);
                t.flags_known = true;
            }
        }
        log("[nr] OptiScaler feature %u made: NGX feature %d, flags 0x%x%s", (*out)->Id, int(feature), t.flags,
            t.flags_known ? "" : " (not given)");
    }
    return r;
}

NVSDK_NGX_Result NR_NGX_MSABI hk_release(NVSDK_NGX_Handle* h) {
    if (h) {
        std::lock_guard<std::mutex> guard(g_lock);
        auto it = g_features.find(h->Id);
        if (it != g_features.end()) {
            forget(it->second);
            g_features.erase(it);
        }
    }
    return real_release(h);
}

NVSDK_NGX_Result NR_NGX_MSABI hk_init_project_id(const char* project, int engine, const char* version,
                                                 const wchar_t* path, ID3D12Device* device, uintptr_t a, uintptr_t b) {
    g_ngx_engine.store(engine);
    return real_init_project_id(project, engine, version, path, device, a, b);
}
NVSDK_NGX_Result NR_NGX_MSABI hk_init_with_project_id(const char* project, int engine, const char* version,
                                                      const wchar_t* path, ID3D12Device* device, uintptr_t a,
                                                      uintptr_t b) {
    g_ngx_engine.store(engine);
    return real_init_with_project_id(project, engine, version, path, device, a, b);
}

// [RenoDX] for the placement (NRHookPoint), re-read when the file changes.
// ---- the NR chain ([LayerN] in dlssnr-amd.ini, nr_pe_config.hpp) ----------------------------------------------------
// The enabled layers as this frame runs them, re-read when the file changes (at once after NR Control saved it).
struct Layer {
    nr::pe::LayerConfig cfg;
    nr::wilsjo2::Settings w2;          // a wilsjo2 layer's settings, parsed from its keys
    int network = -1;                  // its Network (nr::NetworkChoice); -1: [Network] Model
};
// A layer's row in NR Control's status. The one layer of a file without [LayerN] is its Layer 1, as NR Control shows it.
int row_number(const Layer& l) { return std::max(l.cfg.number, 1); }
struct Chain {
    uint32_t output_rect = 1;
    uint64_t version = 0;
    std::vector<Layer> layers;         // enabled, by number
    nr::pe::InspectConfig inspect;     // [Inspect]: NR Control's comparison
    nr::pe::HudFixConfig hud;          // [HudFix]
};
// Under g_lock. A snapshot: an evaluate keeps the chain it started with while it lets go of the lock.
std::shared_ptr<const Chain> chain_now() {
    static nr::pe::ChainFile file;
    static nr::pe::ChainConfig config = [] {   // no file: one RenoDX layer at its defaults
        nr::pe::ChainConfig c;
        c.layers.emplace_back();
        return c;
    }();
    static std::shared_ptr<const Chain> current;
    if (!file.poll(std::string(nr::pe::module_folder()) + "\\dlssnr-amd.ini", config) && current) return current;
    auto next = std::make_shared<Chain>();
    Chain& chain = *next;
    chain.version = current ? current->version : 0;
    std::string said;
    for (const nr::pe::LayerConfig& l : config.layers) {
        if (!l.enabled) continue;
        Layer x{l, {}, -1};
        if (l.processing == nr::pe::Processing::kWilsjo2) {
            x.w2 = nr::asi::wilsjo2_settings(l);
            if (config.inspect.layer == std::max(l.number, 1)) {   // [Inspect]'s debug views, of the layer it chose
                x.w2.debug_view = config.inspect.debug_view;
                x.w2.show_skin_mask = config.inspect.skin_mask;
            }
        } else if (config.inspect.layer == std::max(l.number, 1)) {
            x.cfg.renodx.values.debug_view = config.inspect.debug_view;
        }
        if (!l.network.empty()) {
            x.network = nr::network_choice(l.network);
            if (x.network < 0) log("[nr] dlssnr-amd.ini [Layer%d] Network = %s not known; [Network] Model", l.number,
                                   l.network.c_str());
        }
        char one[96];
        std::snprintf(one, sizeof one, "%s%d %s %s%s%s", said.empty() ? "" : ", ", l.number,
                      nr::pe::processing_name(l.processing), nr::pe::placement_name(l.placement),
                      x.network >= 0 ? " " : "", x.network >= 0 ? nr::network_codename(x.network) : "");
        said += one;
        chain.layers.push_back(std::move(x));
    }
    chain.inspect = config.inspect;
    chain.hud = config.hud;
    chain.output_rect = config.output_rect;
    ++chain.version;
    std::vector<int> numbers;
    for (const Layer& l : chain.layers) numbers.push_back(row_number(l));
    // [Inspect]'s debug view (or skin mask) of a layer shows that layer's own picture: the layers that run after it
    // (later placements, then higher numbers at its own) are left out meanwhile - they would work on the debug picture
    struct Back { int row; uint32_t processing, placement; };
    std::vector<Back> held_back;
    if (config.inspect.layer > 0 && (config.inspect.debug_view || config.inspect.skin_mask)) {
        const auto order = [](const Layer& l) {
            const int p = l.cfg.placement == nr::pe::Placement::kBefore ? 0 : l.cfg.placement == nr::pe::Placement::kAfter ? 1 : 2;
            return p * 1000 + row_number(l);
        };
        const auto chosen = std::find_if(chain.layers.begin(), chain.layers.end(),
                                         [&](const Layer& l) { return row_number(l) == config.inspect.layer; });
        if (chosen != chain.layers.end()) {
            const int at = order(*chosen);
            std::vector<Layer> kept;
            for (Layer& l : chain.layers) {
                if (order(l) > at)
                    held_back.push_back({row_number(l), l.cfg.processing == nr::pe::Processing::kWilsjo2 ? 1u : 0u,
                                         l.cfg.placement == nr::pe::Placement::kBefore ? 1u
                                         : l.cfg.placement == nr::pe::Placement::kPresent ? 2u : 0u});
                else kept.push_back(std::move(l));
            }
            chain.layers = std::move(kept);
        }
    }
    nr::pe::control::chain_layers(numbers);
    for (const Back& b : held_back) {
        char note[96];
        std::snprintf(note, sizeof note, "not run while Layer %d's debug view is shown", config.inspect.layer);
        nr::pe::control::layer_ran(b.row, b.processing, b.placement, 3, note);
    }
    // A layer that went, or runs another processing, placement or network now: its costs are measured anew
    if (current)
        for (const Layer& was : current->layers) {
            const auto now = std::find_if(chain.layers.begin(), chain.layers.end(),
                                          [&](const Layer& l) { return row_number(l) == row_number(was); });
            if (now != chain.layers.end() && now->cfg.processing == was.cfg.processing &&
                now->cfg.placement == was.cfg.placement && now->network == was.network)
                continue;
            nr::dlssnr::layer_changed(row_number(was));
            nr::pe::control::layer_changed(row_number(was));
        }
    nr::pe::events::note(nr::pe::events::kSettings, "NR chain%s: %s",
                         config.layered ? "" : " (one layer, from [RenoDX] / [wilsjo2])", said.empty() ? "no layer enabled" : said.c_str());
    current = next;
    return current;
}
uint32_t output_rect_mode() { return chain_now()->output_rect; }


// The feature's layer states against the chain: a layer gone, disabled or of another processing now lets go of its own.
void match_layers(Tracked& t, const Chain& chain) {
    if (t.chain_version == chain.version) return;
    t.chain_version = chain.version;
    for (auto it = t.layers.begin(); it != t.layers.end();) {
        const auto keep = std::find_if(chain.layers.begin(), chain.layers.end(), [&](const Layer& l) {
            return l.cfg.number == it->first && l.cfg.processing == it->second.processing;
        });
        if (keep != chain.layers.end()) { ++it; continue; }
        release_layer(it->second);
        it = t.layers.erase(it);
    }
}

// NR Control's row of a layer: where it runs this frame, or why it does not (nr::control::LayerStatus).
void layer_note(const Layer& l, uint32_t placement, uint32_t state, const char* note) {
    nr::pe::control::layer_ran(row_number(l), l.cfg.processing == nr::pe::Processing::kWilsjo2 ? 1u : 0u, placement, state,
                               note);
}

LayerState& layer_state(Tracked& t, const Layer& l) {
    LayerState& s = t.layers[l.cfg.number];
    s.processing = l.cfg.processing;
    return s;
}

// A same-format texture NR runs on (UNORDERED_ACCESS), made again when the format or size changes.
bool ensure_texture(ID3D12Device* device, ID3D12Resource*& tex, D3D12_RESOURCE_STATES& state, DXGI_FORMAT format,
                    uint32_t w, uint32_t h) {
    if (tex) {
        const D3D12_RESOURCE_DESC d = tex->GetDesc();
        if (d.Format == format && d.Width == w && d.Height == h) return true;
        release_resource(tex);
    }
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w; d.Height = h; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.Format = format; d.SampleDesc.Count = 1;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (test_fault("d12_resource") ||
        FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                               nullptr, IID_PPV_ARGS(&tex)))) {
        tex = nullptr;
        return false;
    }
    nr::vram::track_d3d12(device, tex, nr::vram::kCopies);
    state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    return true;
}

void copy_region(ID3D12GraphicsCommandList* cmd, ID3D12Resource* dst, uint32_t dx, uint32_t dy, ID3D12Resource* src,
                 uint32_t sx, uint32_t sy, uint32_t w, uint32_t h) {
    D3D12_TEXTURE_COPY_LOCATION d{}, s{};
    d.pResource = dst; d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    s.pResource = src; s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    const D3D12_BOX box{sx, sy, 0, sx + w, sy + h, 1};
    cmd->CopyTextureRegion(&d, dx, dy, 0, &s, &box);
}


bool read_frame(const NVSDK_NGX_Parameter* p, const Tracked& t, Frame& f) {
    // DLSS-D (Ray Reconstruction) blocks may name them DLSSD.*, as the forked OptiScaler reads them.
    f.color = get_resource(p, "Color", &f.color_as_resource);
    if (!f.color && (f.color = get_resource(p, "DLSSD.Color", &f.color_as_resource))) f.color_key = "DLSSD.Color";
    f.output = get_resource(p, "Output");
    if (!f.output) f.output = get_resource(p, "DLSSD.Output");
    f.depth = get_resource(p, "Depth");
    if (!f.depth) f.depth = get_resource(p, "DLSSD.Depth");
    f.motion = get_resource(p, "MotionVectors");
    if (!f.motion) f.motion = get_resource(p, "DLSSD.MotionVectors");
    f.exposure = get_resource(p, "ExposureTexture");
    if (!f.color || !f.output || !f.depth || !f.motion) return false;
    f.flags_known = t.flags_known;
    f.flags = t.flags;
    if (!f.flags_known) {
        int flags = 0;
        if (p->Get("DLSS.Feature.Create.Flags", &flags) == NVSDK_NGX_Result_Success) {
            f.flags = uint32_t(flags);
            f.flags_known = true;
        }
    }
    const D3D12_RESOURCE_DESC cd = f.color->GetDesc(), od = f.output->GetDesc();
    f.render_w = f.subrect_w = get_uint(p, "DLSS.Render.Subrect.Dimensions.Width");
    f.render_h = f.subrect_h = get_uint(p, "DLSS.Render.Subrect.Dimensions.Height");
    if (!f.render_w || !f.render_h) {   // the evaluate's Width / Height, else the create block's, else the colour's
        f.render_w = get_uint(p, "Width", t.create.in_w ? t.create.in_w : uint32_t(cd.Width));
        f.render_h = get_uint(p, "Height", t.create.in_h ? t.create.in_h : cd.Height);
    }
    f.color_x = get_uint(p, "DLSS.Input.Color.Subrect.Base.X");
    f.color_y = get_uint(p, "DLSS.Input.Color.Subrect.Base.Y");
    f.out_x = get_uint(p, "DLSS.Output.Subrect.Base.X");
    f.out_y = get_uint(p, "DLSS.Output.Subrect.Base.Y");
    output_size(p, t.create, uint32_t(od.Width), od.Height, &f.out_w, &f.out_h);
    if (!f.out_w || !f.out_h) { f.out_w = uint32_t(od.Width); f.out_h = od.Height; }
    // a region that does not fit the resource: clamped to it
    if (f.out_x >= od.Width || f.out_y >= od.Height) { f.out_x = f.out_y = 0; }
    f.out_w = std::min<uint32_t>(f.out_w, uint32_t(od.Width) - f.out_x);
    f.out_h = std::min<uint32_t>(f.out_h, od.Height - f.out_y);
    // The guides as the forked OptiScaler gave them to NR: depth at the render extent from its own base, the motion
    // vectors from theirs at the render extent, or at the output's when the game says they are at display size.
    f.depth_rect = {get_uint(p, "DLSS.Input.Depth.Subrect.Base.X"), get_uint(p, "DLSS.Input.Depth.Subrect.Base.Y"),
                    f.render_w, f.render_h};
    const bool low_res_mv = !f.flags_known || (f.flags & kFlagMvLowRes) != 0;
    f.motion_rect = {get_uint(p, "DLSS.Input.MV.SubrectBase.X"), get_uint(p, "DLSS.Input.MV.SubrectBase.Y"),
                     low_res_mv ? f.render_w : f.out_w, low_res_mv ? f.render_h : f.out_h};
    if (!low_res_mv && f.motion) {   // display-size vectors declared that the resource cannot hold: the part it holds (0.0.7's)
        const D3D12_RESOURCE_DESC md = f.motion->GetDesc();
        if (f.motion_rect.x + f.motion_rect.width > md.Width || f.motion_rect.y + f.motion_rect.height > md.Height) {
            static int said = 0;
            if (said++ < 2)
                log("[nr] the game says its motion vectors are at the output's size (%ux%u) but they are %llux%u: the part "
                    "they cover is read", f.motion_rect.width, f.motion_rect.height, (unsigned long long)md.Width, md.Height);
            f.motion_rect.x = std::min<uint32_t>(f.motion_rect.x, uint32_t(md.Width));
            f.motion_rect.y = std::min<uint32_t>(f.motion_rect.y, md.Height);
            f.motion_rect.width = std::min<uint32_t>(f.motion_rect.width, uint32_t(md.Width) - f.motion_rect.x);
            f.motion_rect.height = std::min<uint32_t>(f.motion_rect.height, md.Height - f.motion_rect.y);
        }
    }
    f.reset = get_uint(p, "Reset") != 0;
    if (!get_float(p, "DLSS.Pre.Exposure", &f.pre_exposure) || !(f.pre_exposure > 1e-6f) || !std::isfinite(f.pre_exposure)) f.pre_exposure = 1.0f;
    if (!get_float(p, "DLSS.Exposure.Scale", &f.exposure_scale) || !(f.exposure_scale > 0.0f) || !std::isfinite(f.exposure_scale)) f.exposure_scale = 1.0f;
    if (!get_float(p, "MV.Scale.X", &f.mv_x) || !std::isfinite(f.mv_x) || f.mv_x == 0.0f) f.mv_x = 1.0f;
    if (!get_float(p, "MV.Scale.Y", &f.mv_y) || !std::isfinite(f.mv_y) || f.mv_y == 0.0f) f.mv_y = 1.0f;
    return true;
}

// A RenoDX layer on `target` (UNORDERED_ACCESS, in place), as the OptiScaler route's RenoDX-style option runs it, with
// the layer's own settings.
// ---- [Inspect] on the D3D12 route ------------------------------------------------------------------------------------
// The layers of one placement compared: [*a, *b] of `runs`. Layer 0: all of them, when this placement is the last
// that runs (`final_here`) - with ApplyModel off at every placement (no layer's answer anywhere); Layer N: layer N
// alone, wherever it runs.
bool inspect_span(const nr::pe::InspectConfig& ic, const std::vector<const Layer*>& runs, bool final_here, size_t* a,
                  size_t* b) {
    if (!(ic.compare || !ic.apply_model) || runs.empty()) return false;
    if (ic.layer == 0) {
        if (!final_here && ic.apply_model) return false;
        *a = 0; *b = runs.size() - 1;
        return true;
    }
    for (size_t i = 0; i < runs.size(); ++i)
        if (row_number(*runs[i]) == ic.layer) { *a = *b = i; return true; }
    return false;
}
// The comparison's frame formats: what its shader can store (no sRGB, no shared exponent); said once otherwise.
bool inspect_format(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        case DXGI_FORMAT_R32G32B32A32_FLOAT: case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        case DXGI_FORMAT_R11G11B10_FLOAT:
        case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_TYPELESS:
            return true;
        default: {
            static DXGI_FORMAT said = DXGI_FORMAT_UNKNOWN;
            if (said != f) { said = f; log("[nr] NR comparison: frames in DXGI format %d cannot be shown in it", int(f)); }
            return false;
        }
    }
}
// `frame` (UNORDERED_ACCESS, whole) copied into `tex` (made in its format and size; NON_PIXEL_SHADER_RESOURCE after).
bool inspect_copy(ID3D12GraphicsCommandList* cmd, ID3D12Device* device, ID3D12Resource*& tex, D3D12_RESOURCE_STATES& st,
                  ID3D12Resource* frame) {
    const D3D12_RESOURCE_DESC d = frame->GetDesc();
    if (!ensure_texture(device, tex, st, d.Format, uint32_t(d.Width), d.Height)) return false;
    transition(cmd, frame, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    transition(cmd, tex, st, D3D12_RESOURCE_STATE_COPY_DEST);
    cmd->CopyResource(tex, frame);
    transition(cmd, frame, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    transition(cmd, tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    st = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    return true;
}
// After the compared layers: `frame` (UNORDERED_ACCESS) shown as [Inspect] asks - the frame before them (ApplyModel
// off), or the comparison of before (`off`, taken by inspect_copy) and now.
void inspect_show(ID3D12GraphicsCommandList* cmd, ID3D12Device* device, const nr::pe::InspectConfig& ic, ID3D12Resource* off,
                  D3D12_RESOURCE_STATES& off_state, ID3D12Resource*& on, D3D12_RESOURCE_STATES& on_state,
                  ID3D12Resource* frame) {
    if (!off) return;
    const D3D12_RESOURCE_DESC d = frame->GetDesc();
    if (!ic.apply_model) {
        transition(cmd, off, off_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
        transition(cmd, frame, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        cmd->CopyResource(frame, off);
        transition(cmd, frame, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        transition(cmd, off, D3D12_RESOURCE_STATE_COPY_SOURCE, off_state);
        return;
    }
    if (!inspect_copy(cmd, device, on, on_state, frame)) return;
    nr::asi::wilsjo2_inspect(cmd, device, off, on, frame, uint32_t(d.Width), d.Height, ic.compare, ic.split, ic.zoom,
                             ic.swap, true);
}

// [Inspect] HoldFrame, before the chosen layer runs on `frame` (UNORDERED_ACCESS, whole): the first time (or after a
// change of size or format) its input is kept, after that the kept one is put in its place. Off: let go.
void hold_frame(ID3D12GraphicsCommandList* cmd, ID3D12Device* device, const nr::pe::InspectConfig& ic, HoldCopy& h,
                ID3D12Resource* frame, int row) {
    if (!ic.hold) {
        if (h.held) g_hold_released.insert(h.layer);
        h.held = false;
        return;
    }
    const ULONGLONG now = GetTickCount64();
    if (h.held && (h.layer != row)) {   // another layer, or a pause: the old one back to live
        g_hold_released.insert(h.layer);
        h.held = false;
    }
    h.layer = row;
    h.used_ms = now;
    const D3D12_RESOURCE_DESC d = frame->GetDesc();
    if (h.held && h.tex) {
        const D3D12_RESOURCE_DESC k = h.tex->GetDesc();
        if (k.Format != d.Format || k.Width != d.Width || k.Height != d.Height) { g_hold_released.insert(h.layer); h.held = false; }
    }
    if (!h.held) {
        h.inputs_ready = false;
        if (!ensure_texture(device, h.tex, h.state, d.Format, uint32_t(d.Width), d.Height)) return;
        transition(cmd, frame, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        transition(cmd, h.tex, h.state, D3D12_RESOURCE_STATE_COPY_DEST);
        cmd->CopyResource(h.tex, frame);
        transition(cmd, frame, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        transition(cmd, h.tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        h.state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        h.held = true;
        return;
    }
    transition(cmd, h.tex, h.state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    transition(cmd, frame, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    cmd->CopyResource(frame, h.tex);
    transition(cmd, frame, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    transition(cmd, h.tex, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    h.state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
}
// The run index of the layer [Inspect] chose (its Layer), or -1.
long chosen_index(const nr::pe::InspectConfig& ic, const std::vector<const Layer*>& runs) {
    if (ic.layer <= 0) return -1;
    for (size_t i = 0; i < runs.size(); ++i)
        if (row_number(*runs[i]) == ic.layer) return long(i);
    return -1;
}

void run_renodx(ID3D12GraphicsCommandList* cmd, LayerState& ls, const Layer& layer, int placement, const Frame& f,
                ID3D12Resource* target, uint32_t w, uint32_t h, const States& st) {
    ID3D12Device* device = nullptr;
    if (FAILED(cmd->GetDevice(IID_PPV_ARGS(&device))) || !device) return;
    Feature*& feature = ls.nr[placement];
    if (!feature) {
        int rc = 0;
        feature = nr::dlssnr::create_d3d12(device, cmd, nullptr, w, h, 0, 1, nr::dlssnr::Controls6{}, &rc);
        if (!feature) {
            log("[nr] NR feature could not be made (0x%08X): %s", unsigned(rc), nr::dlssnr::last_error());
            device->Release();
            return;
        }
    }
    // What the forked OptiScaler's hooks told the model (nr_pe_renodx_route.hpp), from the game's own block: the
    // colour is linear HDR when the game says IsHDR (or asks for AutoExposure, fix 5 of the OptiScaler route) and the
    // output (else the colour) is a float format.
    const DXGI_FORMAT authority = f.output->GetDesc().Format;
    const bool hdr = f.flags_known && (f.flags & (kFlagIsHdr | kFlagAutoExposure)) != 0 && float_colour(authority);
    namespace route = nr::pe::renodx_route;
    route::note_pass(f.flags_known ? f.flags : 0u, f.exposure_scale, f.render_w, f.render_h, f.out_w, f.out_h);
    route::Frame rf;
    rf.before_upscale = placement == 1;
    rf.pre_exposure = f.pre_exposure;
    rf.exposure = f.exposure;
    rf.exposure_state = uint32_t(st.exposure);
    route::begin_run(rf);
    route::note_encode(hdr, w, h);
    route::set_direct(target);
    // The guides in the state the model reads them in, as the forked OptiScaler held them for it.
    transition(cmd, f.depth, st.depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (f.motion != f.depth) transition(cmd, f.motion, st.motion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    {
        route::CallStyle style(true);
        route::CallSettings settings(&layer.cfg.renodx.values);
        route::CallNetwork network(layer.network);
        route::CallLayer number(row_number(layer));
        nr::dlssnr::evaluate_d3d12(cmd, feature, nullptr, target, f.depth, f.motion, target, w, h, f.depth_rect,
                                   f.motion_rect, (f.flags & kFlagDepthInverted) ? 1 : 0, f.reset || hold_released(row_number(layer)) ? 1 : 0,
                                   nr::dlssnr::Controls6{}, f.mv_x, f.mv_y);
    }
    if (f.motion != f.depth) transition(cmd, f.motion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, st.motion);
    transition(cmd, f.depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, st.depth);
    route::end_run();
    device->Release();
}

// ---- RenoDX's NR screenshot of the final picture -----------------------------------------------------------------
// As the OptiScaler route moves NR to OptiScaler's finished picture while one is armed (nr_pe_optifix.cpp lock_all,
// the fork's FinishedPicture): no NR in the evaluate then, which instead keeps copies of the game's depth and motion
// vectors and what the frame says of them; at the Present (NR Control's D3D12 Present hook, nr_pe_menu.hpp) NR runs on
// the back buffer with them, on a command list of ours on the queue it presents on, until the session has taken the
// pair (nr_pe_session.cpp: a clean finished picture, the third in a row).
struct Late {
    bool armed = false;                          // guides and frame taken at an evaluate since the last Present
    ID3D12Resource *depth = nullptr, *motion = nullptr;
    Frame f;                                     // the evaluate's frame (its resources are not used at the Present)
    Feature* nr = nullptr;
    ID3D12Resource* target = nullptr;            // the back buffer copied here, NR in place, copied back
    D3D12_RESOURCE_STATES target_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    ID3D12Device* device = nullptr;
    ID3D12CommandAllocator* allocators[3] = {};
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    uint64_t fence_value = 0, done_at[3] = {};
    uint32_t slot = 0;
    HANDLE event = nullptr;
};
Late g_late;

bool clone_of(ID3D12Device* device, ID3D12Resource*& clone, ID3D12Resource* source) {
    D3D12_RESOURCE_DESC d = source->GetDesc();
    if (clone) {
        const D3D12_RESOURCE_DESC c = clone->GetDesc();
        if (c.Format == d.Format && c.Width == d.Width && c.Height == d.Height && c.Flags == d.Flags) return true;
        release_resource(clone);
    }
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    d.Alignment = 0;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                               IID_PPV_ARGS(&clone)))) {
        clone = nullptr;
        return false;
    }
    nr::vram::track_d3d12(device, clone, nr::vram::kCopies);
    return true;
}

// The source metadata and guide textures belong to the held input, not the live game's next frame.
bool hold_inputs(ID3D12GraphicsCommandList* cmd, ID3D12Device* device, HoldCopy& h,
                 Frame& f, States& st, nr::asi::Wilsjo2Frame& wf) {
    if (!h.held) return false;
    if (!h.inputs_ready) {
        for (auto& r : h.guides) release_resource(r);
        ID3D12Resource* src[] = {f.depth, f.motion, f.exposure};
        const D3D12_RESOURCE_STATES from[] = {st.depth, st.motion, st.exposure};
        // Allocate the complete set before recording any copy.
        for (int i = 0; i < 3; ++i)
            if (src[i] && !clone_of(device, h.guides[i], src[i])) { h.held = false; return false; }
        for (int i = 0; i < 3; ++i) if (src[i]) {
            transition(cmd, src[i], from[i], D3D12_RESOURCE_STATE_COPY_SOURCE);
            cmd->CopyResource(h.guides[i], src[i]);
            transition(cmd, src[i], D3D12_RESOURCE_STATE_COPY_SOURCE, from[i]);
            transition(cmd, h.guides[i], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        h.input = f; h.input_states = st; h.w2 = wf;
        h.input.depth = h.w2.depth = h.guides[0];
        h.input.motion = h.w2.motion = h.guides[1];
        h.input.exposure = h.w2.exposure = h.guides[2];
        h.input_states.depth = h.input_states.motion = h.input_states.exposure = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        h.w2.depth_state = h.w2.motion_state = h.w2.exposure_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        h.inputs_ready = true;
    }
    auto* color = f.color; auto* output = f.output;
    f = h.input; st = h.input_states; wf = h.w2;
    f.color = color; f.output = output;
    // Reset is an event: consume it once, then let temporal state evolve on the stored input.
    h.input.reset = h.w2.reset = false;
    return true;
}


// At an evaluate while a final-picture screenshot is armed: the guides copied where their states are known.
void late_capture(ID3D12GraphicsCommandList* cmd, ID3D12Device* device, const Frame& f, const States& st) {
    if (!clone_of(device, g_late.depth, f.depth) || !clone_of(device, g_late.motion, f.motion)) {
        nr::pe::screenshot::fail("unsupported_present", "the game's depth or motion vectors could not be copied");
        return;
    }
    const std::pair<ID3D12Resource*, std::pair<ID3D12Resource*, D3D12_RESOURCE_STATES>> pairs[2] = {
        {g_late.depth, {f.depth, st.depth}}, {g_late.motion, {f.motion, st.motion}}};
    for (const auto& pr : pairs) {
        transition(cmd, pr.second.first, pr.second.second, D3D12_RESOURCE_STATE_COPY_SOURCE);
        cmd->CopyResource(pr.first, pr.second.first);
        transition(cmd, pr.second.first, D3D12_RESOURCE_STATE_COPY_SOURCE, pr.second.second);
    }
    g_late.f = f;
    g_late.armed = true;
}

// A typed format the back buffer's copy can be written in (an sRGB format cannot be a UAV).
DXGI_FORMAT writable(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8X8_UNORM;
        default: return f;
    }
}

void late_present(IDXGISwapChain* sc, ID3D12CommandQueue* queue, int encoding) {
    if (!nr::pe::screenshot::wants_final()) return;
    std::lock_guard<std::mutex> guard(g_lock);
    if (!g_late.armed) return;
    g_late.armed = false;
    ID3D12Device* device = nullptr;
    if (FAILED(queue->GetDevice(IID_PPV_ARGS(&device))) || !device) return;
    struct Dev { ID3D12Device* d; ~Dev() { d->Release(); } } hold{device};
    if (g_late.device != device) {   // the queue's device: our list, allocators and fence on it
        if (g_late.list) { g_late.list->Release(); g_late.list = nullptr; }
        for (auto*& a : g_late.allocators) if (a) { a->Release(); a = nullptr; }
        if (g_late.fence) { g_late.fence->Release(); g_late.fence = nullptr; }
        for (auto*& a : g_late.allocators)
            if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a)))) return;
        if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_late.allocators[0], nullptr,
                                             IID_PPV_ARGS(&g_late.list))) ||
            FAILED(g_late.list->Close()) || FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_late.fence))))
            return;
        if (!g_late.event) g_late.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        g_late.device = device;
        g_late.fence_value = 0;
        for (auto& v : g_late.done_at) v = 0;
    }
    IDXGISwapChain3* sc3 = nullptr;
    if (FAILED(sc->QueryInterface(IID_PPV_ARGS(&sc3))) || !sc3) return;
    const UINT index = sc3->GetCurrentBackBufferIndex();
    sc3->Release();
    ID3D12Resource* back = nullptr;
    if (FAILED(sc->GetBuffer(index, IID_PPV_ARGS(&back))) || !back) return;
    struct Back { ID3D12Resource* r; ~Back() { r->Release(); } } back_hold{back};
    const D3D12_RESOURCE_DESC bd = back->GetDesc();
    const uint32_t w = uint32_t(bd.Width), h = bd.Height;
    if (!ensure_texture(device, g_late.target, g_late.target_state, writable(bd.Format), w, h)) return;
    if (!g_late.nr) {
        int rc = 0;
        g_late.nr = nr::dlssnr::create_d3d12(device, g_late.list, nullptr, w, h, 0, 1, nr::dlssnr::Controls6{}, &rc);
        if (!g_late.nr) return;
    }
    const uint32_t slot = g_late.slot;
    if (g_late.fence->GetCompletedValue() < g_late.done_at[slot]) {
        ResetEvent(g_late.event);
        if (SUCCEEDED(g_late.fence->SetEventOnCompletion(g_late.done_at[slot], g_late.event)))
            WaitForSingleObject(g_late.event, 5000);
        if (g_late.fence->GetCompletedValue() < g_late.done_at[slot]) return;   // its slot still on the GPU
    }
    if (FAILED(g_late.allocators[slot]->Reset()) || FAILED(g_late.list->Reset(g_late.allocators[slot], nullptr))) return;
    ID3D12GraphicsCommandList* l = g_late.list;
    nr::gpu::reset(l);
    nr::gpu::Scope recording(nr::asi::hudcap::d3d12_recording(l));
    g_d12_uses.touch();
    const Frame& f = g_late.f;
    transition(l, back, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
    transition(l, g_late.target, g_late.target_state, D3D12_RESOURCE_STATE_COPY_DEST);
    l->CopyResource(g_late.target, back);
    transition(l, g_late.target, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    transition(l, g_late.depth, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition(l, g_late.motion, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    // The forked OptiScaler's finished picture: linear HDR for scRGB and HDR10 swap chains, no exposure texture, no
    // create flags (its MakeDlssNrPass does not run for it).
    namespace route = nr::pe::renodx_route;
    route::Frame rf;
    rf.finished_picture = true;
    route::begin_run(rf);
    route::note_encode(encoding == 2 || encoding == 3, w, h);
    route::set_direct(g_late.target);
    {
        route::CallStyle style(true);
        nr::dlssnr::evaluate_d3d12(l, g_late.nr, nullptr, g_late.target, g_late.depth, g_late.motion, g_late.target, w,
                                   h, f.depth_rect, f.motion_rect, (f.flags & kFlagDepthInverted) ? 1 : 0,
                                   f.reset ? 1 : 0, nr::dlssnr::Controls6{}, f.mv_x, f.mv_y);
    }
    route::end_run();
    transition(l, g_late.depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    transition(l, g_late.motion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    transition(l, g_late.target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    transition(l, back, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    l->CopyResource(back, g_late.target);
    transition(l, back, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    transition(l, g_late.target, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    g_late.target_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    if (FAILED(l->Close())) return;
    ID3D12CommandList* lists[] = {l};
    queue->ExecuteCommandLists(1, lists);
    queue->Signal(g_late.fence, ++g_late.fence_value);
    g_late.done_at[slot] = g_late.fence_value;
    g_late.slot = (slot + 1) % 3;
}

// ---- the Present placement: RenoDX's Present hook point -------------------------------------------------------------
// RenoDX layers placed at Present run as RenoDX's Present hook point runs them (RenoDX's
// present_path.hpp, v8.5.0-rc10): on the back buffer at the swap chain's Present - after the game drew everything,
// UI included - on a command list of ours from a ring of eight on the queue it presents on, the processing the ReShade
// routes run (Session::RenoDXFrame::present). Their guides are the game's motion vectors and depth, copied at its
// evaluate (the game may reuse them by the Present); the Present takes the newest copy that is submitted and ordered
// before it - on the presenting queue, or already complete on another - of the back buffer's aspect and at most 8
// presents or 250 ms old. Without one (or NRPresentGuides = 2) neutral guides: no motion, no depth (RenoDX's).
// The encoding comes from the colour space the game set (SetColorSpace1; never set: scRGB for an FP16 back buffer,
// else sRGB - ReShade 6.8.0's d3d12::swapchain_impl::get_color_space, which RenoDX reads), NRPresentEncoding forcing one
// the back buffer can carry; a swap chain
// that cannot be served (HLG, an unknown space, a format that cannot carry it) leaves those layers off, their rows
// saying why, until it can - never in another place (RenoDX falls back to its Upscaled hook point; Present
// never does). Frame generation's cadence below; not yet: the composition around the game's HUD (HUD FIX).
constexpr uint32_t kPresentRing = 8;                 // present_path.hpp kPresentRing
constexpr DWORD kPresentSlotWaitMs = 100;            // kPresentSlotWaitMs
constexpr uint32_t kPresentGuideSets = 4;            // kPresentGuideSets
constexpr uint64_t kPresentGuideMaxAge = 8;          // kPresentGuideMaxAge (presents)
constexpr ULONGLONG kPresentGuideMaxAgeMs = 250;     // kPresentGuideMaxAgeNs

struct GuideSet {
    ID3D12Resource *depth = nullptr, *motion = nullptr;   // copies, in COPY_DEST between uses
    Frame f;                                   // the evaluate's contract (its resources are not used at the Present)
    nr::asi::Wilsjo2Frame w2;                  // the same for the wilsjo2 processing (its own guide regions)
    uint64_t serial = 0;                       // 0: empty
    uint64_t captured_at = 0;                  // presents seen at the capture
    ULONGLONG captured_ms = 0;
    uint32_t out_w = 0, out_h = 0;             // the upscaler's output (the aspect a back buffer must have)
    uint64_t read_value = 0;                   // the ring's fence value of the last Present that read it
    // submission (g_capture_lock): the game's list it was recorded on until that list is executed; then the queue it
    // ran on and our tracker's signal after it
    ID3D12CommandList* list = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12Fence* fence = nullptr;
    uint64_t value = 0;
};
struct PresentLayer {                          // a Present layer's own
    Feature* nr = nullptr;                     // RenoDX: its model feature at the back buffer's extent
    nr::asi::Wilsjo2Owner* w2 = nullptr;       // wilsjo2: its state (the common pipeline's)
    bool w2_reset = true;                      // wilsjo2: the last finished picture did not run
    uint32_t w = 0, h = 0;
    bool real_guides = false;                  // its last run had the game's guides (a change restarts its history)
    uint64_t consumed = 0;                     // the guide set it last ran on (a new one carries the game's Reset)
    uint64_t consumed_present = 0;             // wilsjo2: the Present that took it (g_present.presents then)
    nr::pe::Processing processing = nr::pe::Processing::kRenoDX;   // what it was made for
};
struct PresentPath {
    GuideSet sets[kPresentGuideSets];
    // [Inspect]: the back buffer's copy before and after the compared Present layers (NON_PIXEL_SHADER_RESOURCE)
    ID3D12Resource* inspect_off = nullptr;
    ID3D12Resource* inspect_on = nullptr;
    D3D12_RESOURCE_STATES inspect_off_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                          inspect_on_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    HoldCopy hold;                             // [Inspect] HoldFrame of a Present layer
    uint64_t serial = 0, presents = 0;
    ID3D12Device* device = nullptr;            // the ring's
    ID3D12CommandAllocator* allocators[kPresentRing] = {};
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    uint64_t submitted = 0, slot_value[kPresentRing] = {};
    HANDLE event = nullptr;
    bool slot_wait_off = false;
    ID3D12Resource* target = nullptr;          // the back buffer's copy NR runs on, in place
    D3D12_RESOURCE_STATES target_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    // wilsjo2's HDR10 finished picture: the linear BT.709 copy it runs on and the re-encoded one (UNORDERED_ACCESS)
    ID3D12Resource *linear = nullptr, *encoded = nullptr;
    // HUD FIX (wilsjo2_hudfix.comp): the frame before the layers, the last one (the estimate's), the HUD's alpha and
    // confidence (R16G16B16A16_FLOAT), the counts and the estimate's tile flags (R32_UINT, hud_stats_size);
    // UNORDERED_ACCESS between uses
    ID3D12Resource *hud_back = nullptr, *hud_prev = nullptr, *hud_alpha = nullptr, *hud_stats = nullptr;
    D3D12_RESOURCE_STATES hud_back_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                          hud_prev_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                          hud_alpha_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                          hud_stats_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    bool hud_prev_valid = false, hud_fresh = true, hud_said = false;
    uint32_t hud_parity = 0;
    uint64_t hud_estimated_at = ~0ull;   // the estimate's last Present: after a gap it starts afresh
    D3D12_RESOURCE_STATES linear_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS, encoded_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    std::map<int, PresentLayer> layers;        // by layer number
    std::atomic<bool> unusable{false};         // this swap chain cannot be served: the layers run after the upscaler
    std::atomic<bool> unusable_w2{false};      // the same for wilsjo2 layers (its finished picture's formats)
    std::atomic<uint64_t> served_ms{0};        // the last Present that ran its layers (GetTickCount64)
    uint64_t wanted_since_ms = 0;              // since when evaluates have Present layers (0: none), under g_lock
    std::atomic<bool> wanted{false};           // the chain has RenoDX layers at Present (evaluates copy guides)
    std::atomic<uint64_t> wanted_checked_ms{0};   // when a Present last let present_layers look at the chain (below)
    std::atomic<uint64_t> evaluate_ms{0};         // the last game evaluate (its frame_keys); 0: none yet
    std::atomic<bool> held{false};             // `layers` is not empty
    // Frame generation (present_path.hpp PresentStream): presents a game frame has, from the cadence of fresh captures
    // against presents, or declared by the game's DLSS-G evaluates
    nr::pe::present::Cadence cadence;
    uint32_t multiplier = 1;
    uint32_t back_w = 0, back_h = 0;           // the stream's back buffer (the captures its cadence counts)
    uint64_t cadence_captures = 0;
    uint32_t cadence_source = 0;               // the feature the last counted capture came from
    ULONGLONG cadence_source_ms = 0, cadence_ambiguous_until_ms = 0;
    struct Declaration { uint32_t w = 0, h = 0, multiplier = 0; ULONGLONG ms = 0; uint64_t presents = 0; } declaration;
    std::atomic<ULONGLONG> top_ms{0};          // the game's own swap chain served the layers then (game_present)
    uint64_t chain_version = 0;
};
PresentPath g_present;                         // under g_lock, but the submission fields of `sets`
std::mutex g_capture_lock;                     // GuideSet's submission fields; never held across ExecuteCommandLists
std::map<ID3D12CommandQueue*, std::pair<ID3D12Fence*, uint64_t>> g_trackers;   // our fence on each queue (g_capture_lock)

bool aspect_matches(uint32_t sw, uint32_t sh, uint32_t tw, uint32_t th) {   // present_path.hpp PresentAspectMatches
    return sw && sh && tw && th && std::abs(double(sw) * th - double(tw) * sh) <= 0.01 * double(tw) * sh;
}

bool present_layer(const Layer& l) { return l.cfg.placement == nr::pe::Placement::kPresent; }
// HUD FIX's counts image (wilsjo2_hudfix.comp): its counts in row 0, a texel an 8 x 8 tile below
uint32_t hud_stats_w(uint32_t w) { return std::max(4u, (w + 7) / 8); }
uint32_t hud_stats_h(uint32_t h) { return 1 + 2 * ((h + 7) / 8); }   // two tile-flag sets

// HUD FIX's line for NR Control (nr_pe_control.hpp hud_facts), from a Present that reached its layers (`layers`: some
// run there). `follows`: the capture follows this API (D3D12, D3D11, Vulkan); `captured` this frame, else `why` not.
void hud_report(bool layers, bool follows = false, bool captured = false, const std::string& why = std::string(),
                bool fg_api = false, bool fg = false, const std::string& fg_why = std::string(), const char* fg_source = "",
                bool broken = false) {
    const bool d3d12 = follows;
    // greyed while it cannot run: frame generation's HUD-less frame not handed over for 2 s; the capture's hooks failed
    static ULONGLONG fg_seen = 0, fg_watched = 0;   // a hand-over last seen; watched since (Auto / Game armed it)
    const ULONGLONG now_ms = GetTickCount64();
    const uint32_t mode_now = chain_now() ? chain_now()->hud.mode : 0;
    if (!(mode_now == 1 || mode_now == 2)) fg_watched = 0;
    else if (!fg_watched) fg_watched = now_ms;
    if (fg) fg_seen = now_ms;
    // greyed only once it was watched for 2 s and nothing came (off: it is not watched, so not greyed)
    const bool fg_absent = fg_api && fg_watched && now_ms - fg_watched > 2000 && (!fg_seen || now_ms - fg_seen > 2000);
    const std::shared_ptr<const Chain> c = chain_now();
    const uint32_t mode = c ? c->hud.mode : 0;
    const char* const no_game = "frame generation's HUD-less frame is read for D3D12 games only";
    const char* const no_capture = "the capture before the HUD does not follow this game's API";
    std::string now = mode == 0 ? "off"
                      : !layers ? "nothing: no layer runs at Present"
                      : mode == 4 ? "estimated"
                      : fg && (mode == 1 || mode == 2) ? std::string("frame generation's HUD-less frame (") + fg_source + ")"
                      : mode == 2 ? (fg_api ? "nothing this frame: " + fg_why : std::string("nothing: ") + no_game)
                      : captured  ? "captured before the HUD"
                      : mode == 1 ? (d3d12 ? "estimated (captured: " + why + ")" : std::string("estimated"))
                      : d3d12     ? "nothing this frame: " + why : std::string("nothing: ") + no_capture;
    static std::string said;   // the log: its changes, the first 40
    static int changes = 0;
    if (now != said && changes < 40) {
        ++changes;
        log("[nr] HUD FIX now: %s%s", now.c_str(), changes == 40 ? " (further changes not logged)" : "");
        said = now;
    }
    const std::string why_game = !fg_api ? std::string(no_game)
                                 : fg_absent ? (fg_why.empty() ? std::string("the game hands its frame generation no HUD-less frame") : fg_why)
                                             : std::string();
    const std::string why_capture = !d3d12 ? std::string(no_capture) : broken ? why : std::string();
    nr::pe::control::hud_facts(now.c_str(), why_game.c_str(), why_capture.c_str(), "");
}
bool renodx_layer(const Layer& l) { return l.cfg.processing == nr::pe::Processing::kRenoDX; }

// At a game evaluate, while the chain has Present layers: its motion vectors and depth copied into the oldest set no
// Present on the GPU still reads (none free: no copy this frame).
void capture_guides(ID3D12GraphicsCommandList* cmd, ID3D12Device* device, const Frame& f, const States& st,
                    const nr::asi::Wilsjo2Frame& wf, uint32_t feature) {
    GuideSet* target = nullptr;
    const uint64_t done = g_present.fence ? g_present.fence->GetCompletedValue() : ~0ull;
    // the set a Present read last is kept (present_path.hpp: a capture in flight must not leave every candidate
    // unusable - neutral guides and a history reset at the next Present)
    const GuideSet* last_read = nullptr;
    for (const GuideSet& s : g_present.sets)
        if (s.serial && s.read_value && (!last_read || s.read_value > last_read->read_value)) last_read = &s;
    for (GuideSet& s : g_present.sets) {
        if (s.read_value > done && done != ~0ull) continue;   // a Present on the GPU still reads it
        if (&s == last_read) continue;
        if (!target || s.serial < target->serial) target = &s;
    }
    if (!target) return;
    for (ID3D12Resource* r : {f.depth, f.motion}) {
        const D3D12_RESOURCE_DESC d = r->GetDesc();
        if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.DepthOrArraySize != 1 || d.SampleDesc.Count != 1) return;
    }
    if (!clone_of(device, target->depth, f.depth) || !clone_of(device, target->motion, f.motion)) {
        target->serial = 0;
        return;
    }
    const std::pair<ID3D12Resource*, std::pair<ID3D12Resource*, D3D12_RESOURCE_STATES>> pairs[2] = {
        {target->depth, {f.depth, st.depth}}, {target->motion, {f.motion, st.motion}}};
    for (const auto& pr : pairs) {
        transition(cmd, pr.second.first, pr.second.second, D3D12_RESOURCE_STATE_COPY_SOURCE);
        cmd->CopyResource(pr.first, pr.second.first);
        transition(cmd, pr.second.first, D3D12_RESOURCE_STATE_COPY_SOURCE, pr.second.second);
    }
    target->f = f;
    target->w2 = wf;
    target->serial = ++g_present.serial;
    target->captured_at = g_present.presents;
    target->captured_ms = GetTickCount64();
    target->out_w = f.out_w;
    target->out_h = f.out_h;
    // The stream's cadence counts captures of its shape; two features feeding it at once are ambiguous for a second
    // (StampPresentCapture: two renders must not read as half the frame generation)
    if (aspect_matches(f.out_w, f.out_h, g_present.back_w, g_present.back_h)) {
        if (g_present.cadence_source && g_present.cadence_source != feature &&
            target->captured_ms - g_present.cadence_source_ms <= kPresentGuideMaxAgeMs)
            g_present.cadence_ambiguous_until_ms = target->captured_ms + 1000;
        g_present.cadence_source = feature;
        g_present.cadence_source_ms = target->captured_ms;
        ++g_present.cadence_captures;
    }
    std::lock_guard<std::mutex> guard(g_capture_lock);
    target->list = cmd;
    target->queue = nullptr;
    target->fence = nullptr;
    target->value = 0;
}

// After every ExecuteCommandLists (nr_pe_menu.hpp on_execute): a guide set recorded on one of these lists is submitted
// on this queue; our fence on it is signalled after them, so a Present on another queue can tell when they ran.
void on_execute(ID3D12CommandQueue* queue, unsigned count, ID3D12CommandList* const* lists) {
    nr::asi::hudcap::d3d12_executed(count, lists);   // HUD FIX's capture: the writes in the GPU's order
    std::lock_guard<std::mutex> guard(g_capture_lock);
    bool any = false;
    for (GuideSet& s : g_present.sets) {
        if (!s.list) continue;
        for (unsigned i = 0; i < count; ++i)
            if (lists[i] == s.list) { s.list = nullptr; s.queue = queue; any = true; break; }
    }
    std::vector<nr::gpu::Ticket> tickets;
    for (unsigned i = 0; i < count; ++i) if (auto t = nr::gpu::find(lists[i])) tickets.push_back(std::move(t));
    if (!any && tickets.empty()) return;
    auto& tracker = g_trackers[queue];
    if (!tracker.first) {
        ID3D12Device* device = nullptr;
        if (FAILED(queue->GetDevice(IID_PPV_ARGS(&device))) || !device) {
            for (const auto& t : tickets) t->submitted({}); return;
        }
        if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&tracker.first)))) tracker.first = nullptr;
        device->Release();
        if (!tracker.first) { for (const auto& t : tickets) t->submitted({}); return; }
    }
    const uint64_t value = ++tracker.second;
    const bool signalled = SUCCEEDED(queue->Signal(tracker.first, value));
    tracker.first->AddRef();
    const auto fence = std::shared_ptr<ID3D12Fence>(tracker.first, [](ID3D12Fence* f) { f->Release(); });
    for (const auto& t : tickets) {
        if (signalled) t->submitted([fence, value] { return fence->GetCompletedValue() >= value; });
        else t->submitted({});
    }
    for (GuideSet& s : g_present.sets)
        if (s.queue == queue && !s.fence) { s.fence = tracker.first; s.value = tracker.second; }
}

// The Windows SDR content level of the display the swap chain is on, in nits, only while it runs HDR (1000 = 80 nits;
// 200 reads as 203); 0 when it cannot be read (RenoDX's QueryPresentSdrWhite, as the ReShade route reads it).
float sdr_white(IDXGISwapChain* sc) {
    IDXGIOutput* output = nullptr;
    if (FAILED(sc->GetContainingOutput(&output)) || !output) return 0.f;
    DXGI_OUTPUT_DESC1 desc{};
    IDXGIOutput6* output6 = nullptr;
    const bool described = SUCCEEDED(output->QueryInterface(IID_PPV_ARGS(&output6))) && SUCCEEDED(output6->GetDesc1(&desc));
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

bool ensure_ring(ID3D12Device* device) {
    if (g_present.device == device && g_present.list) return true;
    if (g_present.list) { g_present.list->Release(); g_present.list = nullptr; }
    for (auto*& a : g_present.allocators) if (a) { a->Release(); a = nullptr; }
    if (g_present.fence) { g_present.fence->Release(); g_present.fence = nullptr; }
    g_present.device = nullptr;
    for (auto*& a : g_present.allocators)
        if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a)))) return false;
    if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_present.allocators[0], nullptr,
                                         IID_PPV_ARGS(&g_present.list))) ||
        FAILED(g_present.list->Close()) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_present.fence))))
        return false;
    if (!g_present.event) g_present.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_present.device = device;
    g_present.submitted = 0;
    for (auto& v : g_present.slot_value) v = 0;
    return true;
}

// A RenoDX mod's Game Brightness (present_path.hpp present_mod_white), as the ReShade route reads it: a renodx-*.addon
// loaded in the process and its [renodx-preset1] ToneMapGameNits (48..10000) with its tone mapper on, read from the
// ReShade.ini beside the ReShade module (else the game's folder). 0: none. Polled at most once a second.
float renodx_mod_white() {
    static ULONGLONG polled = 0;
    static float white = 0.f;
    const ULONGLONG now = GetTickCount64();
    if (polled && now - polled < 1000) return white;
    polled = now;
    white = 0.f;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
    if (snap == INVALID_HANDLE_VALUE) return white;
    MODULEENTRY32W me{};
    me.dwSize = sizeof me;
    bool mod = false;
    std::wstring reshade_dir;
    for (BOOL ok = Module32FirstW(snap, &me); ok; ok = Module32NextW(snap, &me)) {
        std::wstring n = me.szModule;
        for (wchar_t& c : n) c = wchar_t(std::towlower(c));
        mod |= n.rfind(L"renodx-", 0) == 0 && n.find(L".addon") != std::wstring::npos;
        if (n == L"reshade64.dll" || n == L"reshade32.dll" || GetProcAddress(me.hModule, "ReShadeRegisterAddon")) {
            std::wstring path = me.szExePath;
            reshade_dir = path.substr(0, path.find_last_of(L"\\/"));
        }
    }
    CloseHandle(snap);
    if (!mod) return white;
    if (reshade_dir.empty()) {
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        reshade_dir = exe;
        reshade_dir = reshade_dir.substr(0, reshade_dir.find_last_of(L"\\/"));
    }
    const std::wstring ini = reshade_dir + L"\\ReShade.ini";
    wchar_t type[32] = {}, nits[32] = {};
    GetPrivateProfileStringW(L"renodx-preset1", L"ToneMapType", L"0", type, 32, ini.c_str());
    GetPrivateProfileStringW(L"renodx-preset1", L"ToneMapGameNits", L"0", nits, 32, ini.c_str());
    const float t = std::wcstof(type, nullptr), g = std::wcstof(nits, nullptr);
    if (t != 0.f && g >= 48.f && g <= 10000.f) white = g;
    return white;
}
// The HDR diffuse white of a RenoDX Present layer, as RenoDX takes it: NRPresentWhiteNits; else the RenoDX mod's Game
// Brightness (NRPresentWhiteSource 0 only); else the Windows SDR content level (`windows`, 0 unknown); else
// NRDiffuseWhiteNits.
float present_white_of(const nr::renodx::Settings& rs, float windows) {
    const float mod = rs.present_white_source == 0 ? renodx_mod_white() : 0.f;
    return rs.present_white_nits > 0.f ? rs.present_white_nits : mod > 0.f ? mod : windows > 0.f ? windows
                                                                                        : rs.diffuse_white_nits;
}

// The back buffer's encoding for a layer (present_path.hpp RunPresentPath): 1 SDR, 2 HDR10, 3 scRGB, 0 not served.
uint32_t present_encoding(const nr::renodx::Settings& s, int space, bool space_set, DXGI_FORMAT format) {
    const bool float_format = format == DXGI_FORMAT_R16G16B16A16_FLOAT;
    if (!space_set) space = float_format ? int(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709) : int(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
    const uint32_t automatic = space == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709 ? 1u
                               : space == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ? 2u
                               : space == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709 ? 3u : 0u;
    uint32_t e = s.present_encoding;
    if (e != 0 && automatic != 0 &&
        ((e == 2 && format != DXGI_FORMAT_R10G10B10A2_UNORM && !float_format) || (e == 3 && !float_format)))
        e = automatic;   // one the back buffer cannot carry takes Auto's
    if (e == 0) e = automatic;
    if (e == 2) return format == DXGI_FORMAT_R10G10B10A2_UNORM || float_format ? 2u : 0u;
    if (e == 3) return float_format ? 3u : 0u;
    return e == 1 ? 1u : 0u;
}

uint32_t w2_present_encoding(int space, bool space_set, DXGI_FORMAT format);

// At the swap chain's Present (NR Control's D3D12 Present hook): the RenoDX layers placed there, in layer order, on the
// back buffer.
// `top`: at the Present of the game's own swap chain (game_present_before), before OptiScaler's or the game's frame
// generation - every present there is a game frame, whatever DLSS-G declares; else at the system's Present, which
// generated frames reach too.

// F5 (the NR screenshot) at Present on D3D12: the chain's own pair - the frame as the game presents it (before HUD FIX
// and the layers) and the final one (after the last layer and HUD FIX's composition), every processing's - read back
// once our list is done. The request is taken here (screenshot::recorded), so no stage takes a pair of its own run.
struct PresentShot {
    ID3D12Resource* rb[2] = {};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    uint32_t w = 0, h = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    bool ok = false, done = false;
    bool submitted = false;   // our list with the copies was executed: the buffers are freed only once it is done
    bool prepare(ID3D12Device* device, ID3D12Resource* target) {
        const D3D12_RESOURCE_DESC d = target->GetDesc();
        if (!nr::pe::screenshot::readable(nr::pe::vulkan_format_of(d.Format))) {
            nr::pe::screenshot::fail("unsupported_present", "the frame's format cannot be read back");
            return false;
        }
        UINT64 total = 0;
        device->GetCopyableFootprints(&d, 0, 1, 0, &fp, nullptr, nullptr, &total);
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = total; bd.Height = 1; bd.DepthOrArraySize = 1;
        bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        for (auto*& r : rb)
            if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                       IID_PPV_ARGS(&r))))
                r = nullptr;
        if (!rb[0] || !rb[1]) {
            nr::pe::screenshot::abandon("allocation_failed", "the readback buffers could not be allocated");
            return false;
        }
        w = uint32_t(d.Width); h = d.Height; format = d.Format;
        nr::pe::screenshot::recorded();
        return ok = true;
    }
    void copy(ID3D12GraphicsCommandList* l, ID3D12Resource* target, int i) {
        if (!ok) return;
        transition(l, target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
        dst.pResource = rb[i]; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = fp;
        src.pResource = target; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        transition(l, target, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    // `hdr`: 0 SDR, 1 scRGB (1.0 = 80 nits), 2 PQ; `layers` the chain's
    void deliver(ID3D12Fence* fence, uint64_t value, uint8_t hdr, size_t layers) {
        if (!ok) return;
        if (fence->GetCompletedValue() < value) {
            HANDLE e = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (e && SUCCEEDED(fence->SetEventOnCompletion(value, e))) WaitForSingleObject(e, 5000);
            if (e) CloseHandle(e);
        }
        if (fence->GetCompletedValue() < value) {   // not done in time: the buffers are kept (the GPU may still write them)
            rb[0] = rb[1] = nullptr;
            return;   // ~PresentShot abandons the request
        }
        void* m[2] = {};
        const D3D12_RANGE all{0, SIZE_T(fp.Footprint.RowPitch) * h};
        if (FAILED(rb[0]->Map(0, &all, &m[0])) || FAILED(rb[1]->Map(0, &all, &m[1]))) return;
        nr::pe::screenshot::Pair pair;
        pair.width = w; pair.height = h; pair.row_pitch = fp.Footprint.RowPitch;
        pair.format = nr::pe::vulkan_format_of(format);
        pair.hdr_mode = hdr; pair.linear_unit_nits = 80.0f;
        pair.pre = m[0]; pair.post = m[1];
        pair.bytes = uint64_t(fp.Footprint.RowPitch) * h;
        pair.meta = "path=present\ncomparison=final-picture-before-after-nr\nlayers=" + std::to_string(layers) +
                    "\nwidth=" + std::to_string(w) + "\nheight=" + std::to_string(h) + "\nsource_format=" +
                    std::to_string(int(pair.format)) + "\nhdr_mode=" + std::to_string(hdr) + "\n";
        struct Owner { ID3D12Resource* rb[2]; };
        pair.owner = new Owner{{rb[0], rb[1]}};
        pair.release = [](void* o) {
            auto* own = static_cast<Owner*>(o);
            for (ID3D12Resource* r : own->rb) { r->Unmap(0, nullptr); r->Release(); }
            delete own;
        };
        rb[0] = rb[1] = nullptr;
        done = true;
        nr::pe::screenshot::deliver(std::move(pair));
    }
    ~PresentShot() {
        // executed without a proof it finished (a failed Signal): kept; never executed: freed
        if (!submitted) for (ID3D12Resource* r : rb) if (r) r->Release();
        if (ok && !done) nr::pe::screenshot::abandon("capture_failed", "the Present's pair did not complete");
    }
};

void present_layers(IDXGISwapChain* sc, ID3D12CommandQueue* queue, bool top) {
    nr::pe::watchdog::Phase phase("Present layers");
    const nr::asi::hudcap::Own own;   // its commands pass HUD FIX's capture hooks unseen
    std::unique_lock<std::mutex> guard(g_lock);
    tick_retired();
    ++g_present.presents;
    const std::shared_ptr<const Chain> chain = chain_now();
    std::vector<const Layer*> layers;
    for (const Layer& l : chain->layers)
        if (present_layer(l)) layers.push_back(&l);
    g_present.wanted = !layers.empty();
    // no game evaluate for a second (the game in its menus, Present layers alone): the frame's keys here (NR on / off,
    // the screenshot keys), as an evaluate takes them
    if (!layers.empty() && GetTickCount64() - g_present.evaluate_ms.load() > 1000) nr::dlssnr::frame_keys();
    if (g_present.chain_version != chain->version) {   // layers gone or changed processing let go of their features
        g_present.chain_version = chain->version;
        for (auto it = g_present.layers.begin(); it != g_present.layers.end();) {
            // kept: the same layer at Present with the same processing (a layer switched RenoDX <-> wilsjo2 starts
            // afresh: the other style's feature and state go, its guide bookkeeping with them)
            const bool keep = std::any_of(layers.begin(), layers.end(), [&](const Layer* l) {
                return l->cfg.number == it->first && l->cfg.processing == it->second.processing;
            });
            if (keep) { ++it; continue; }
            if (it->second.nr) nr::dlssnr::release_d3d12(it->second.nr);
            if (it->second.w2) nr::asi::wilsjo2_release(it->second.w2);
            it = g_present.layers.erase(it);
        }
    }
    g_present.held = !g_present.layers.empty();
    if (layers.empty()) {
        nr::asi::hudcap::d3d12_present(nullptr, nullptr, nullptr, nullptr, DXGI_FORMAT_UNKNOWN, 0, 0, false);
        hud_report(false, true);
        return;
    }
    ID3D12Device* device = nullptr;
    if (FAILED(queue->GetDevice(IID_PPV_ARGS(&device))) || !device) return;
    struct Dev { ID3D12Device* d; ~Dev() { d->Release(); } } hold{device};
    IDXGISwapChain3* sc3 = nullptr;
    if (FAILED(sc->QueryInterface(IID_PPV_ARGS(&sc3))) || !sc3) return;
    const UINT index = sc3->GetCurrentBackBufferIndex();
    sc3->Release();
    ID3D12Resource* back = nullptr;
    if (FAILED(sc->GetBuffer(index, IID_PPV_ARGS(&back))) || !back) return;
    struct Back { ID3D12Resource* r; ~Back() { r->Release(); } } back_hold{back};
    const D3D12_RESOURCE_DESC bd = back->GetDesc();
    const uint32_t w = uint32_t(bd.Width), h = bd.Height;
    const DXGI_FORMAT format = writable(bd.Format);
    int space = 0;
    const bool space_set = nr::pe::menu::colour_space_of(sc, &space);
    // The white for HDR (polled at most once a second: DisplayConfig can block)
    static ULONGLONG polled = 0;
    static float windows_white = 0.f;
    if (const ULONGLONG now = GetTickCount64(); !polled || now - polled >= 1000) {
        polled = now;
        windows_white = sdr_white(sc);
    }
    struct Run { const Layer* layer; uint32_t encoding; float white; };
    std::vector<Run> runs;
    bool unusable = false;
    const bool plain = bd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && bd.DepthOrArraySize == 1 && bd.SampleDesc.Count == 1;
    // wilsjo2's finished picture (the fork's colour space defaults to ours): scRGB needs RGBA16F, HDR10 (PQ)
    // and SDR R8G8B8A8_UNORM or R10G10B10A2_UNORM; anything else is refused. 1 SDR, 2 PQ, 3 scRGB, 0 refused.
    const int w2_space = space_set ? space
                         : int(bd.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ? DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709
                                                                          : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
    // (an 8-bit BGRA back buffer too, as on D3D11 / Vulkan - one rule, w2_present_encoding)
    const uint32_t w2_encoding = plain ? w2_present_encoding(space, space_set, bd.Format) : 0u;
    static uint32_t w2_said = ~0u;
    bool w2_any = false;
    for (const Layer* l : layers) {
        if (!renodx_layer(*l)) {
            w2_any = true;
            if (w2_encoding != w2_said) {
                w2_said = w2_encoding;
                if (!w2_encoding)
                    log("[nr] wilsjo2 finished picture: this screen colour format is not supported (colour space %d, "
                        "format %u)", w2_space, unsigned(bd.Format));
            }
            if (w2_encoding) runs.push_back({l, w2_encoding, 203.0f});
            else {
                g_present.layers[l->cfg.number].w2_reset = true;
                layer_note(*l, 2, 3, "Present cannot serve this swap chain's colour format (wilsjo2)");
            }
            continue;
        }
        const nr::renodx::Settings& rs = l->cfg.renodx.values;
        const uint32_t e = plain ? present_encoding(rs, space, space_set, format) : 0u;
        if (!e) {
            unusable = true;
            layer_note(*l, 2, 3, "Present cannot serve this swap chain's colour format");
            continue;
        }
        runs.push_back({l, e, present_white_of(rs, windows_white)});
    }
    g_present.unusable_w2 = w2_any && !w2_encoding;
    if (unusable != g_present.unusable.exchange(unusable)) {
        if (unusable)
            log("[nr] Present layers: this swap chain cannot be served (colour space %d%s, format %u); the RenoDX ones do "
                "not run until it can (never in another place)", space, space_set ? "" : " (not set)", unsigned(bd.Format));
        else
            log("[nr] Present layers: the swap chain can be served again");
    }
    g_present.served_ms = GetTickCount64();   // a Present of the game's reached its layers (evaluates: Present is alive)
    // HUD FIX's capture (nr_asi_hudcap.hpp): armed for Auto and Captured, its verdict on this frame
    const uint32_t hud_mode = chain->hud.mode;
    // and frame generation's HUD-less frame (Auto's first choice, Game): what the game handed it this frame
    const nr::asi::hudcap::Frame hud_cap = nr::asi::hudcap::d3d12_present(
        device, sc, queue, back, format, w, h, !runs.empty() && (hud_mode == 1 || hud_mode == 3),
        !runs.empty() && (hud_mode == 1 || hud_mode == 2));
    const bool hud_fg = hud_cap.fg.seen && hud_cap.fg.copy && (hud_mode == 1 || hud_mode == 2);
    const bool hud_captured = hud_fg || (hud_cap.result == nr::asi::hudcap::Result::kCaptured && hud_cap.hudless);
    ID3D12Resource* const hud_hudless = hud_fg ? hud_cap.fg.copy : hud_cap.hudless;
    hud_report(!runs.empty(), true, hud_captured && !hud_fg, hud_cap.why, true, hud_fg, hud_cap.fg.why, hud_cap.fg.source,
               hud_cap.broken);
    if (runs.empty()) return;
    // Frame generation (present_path.hpp RunPresentPath): a matching DLSS-G declaration is authoritative; without one
    // the cadence of presents against fresh captures, over windows (nr_present_cadence.hpp)
    const ULONGLONG now_ms = GetTickCount64();
    g_present.back_w = w;
    g_present.back_h = h;
    const auto fresh = [&](const GuideSet& gs) {
        return gs.serial && aspect_matches(gs.out_w, gs.out_h, w, h) &&
               (g_present.presents - gs.captured_at <= kPresentGuideMaxAge * g_present.multiplier ||
                now_ms - gs.captured_ms <= kPresentGuideMaxAgeMs);
    };   // (the age in presents: generated ones included, where they reach this hook)
    const auto& decl = g_present.declaration;
    const uint32_t declared = decl.multiplier && decl.w == w && decl.h == h &&
                                      (now_ms - decl.ms <= 1000 || g_present.presents - decl.presents <= 8 * decl.multiplier)
                                  ? decl.multiplier : 0u;
    const Run* first_renodx = nullptr;
    for (const Run& run : runs)
        if (renodx_layer(*run.layer)) { first_renodx = &run; break; }
    const auto mode = static_cast<nr::pe::present::FgSource>(
        first_renodx ? first_renodx->layer->cfg.renodx.values.present_fg_source : 0u);
    const bool any_fresh = std::any_of(std::begin(g_present.sets), std::end(g_present.sets), fresh);
    if (g_present.cadence.observe(int64_t(now_ms) * 1'000'000, g_present.cadence_captures,
                                  now_ms >= g_present.cadence_ambiguous_until_ms && any_fresh, declared, mode)) {
        const uint32_t m = g_present.cadence.multiplier;
        const bool flipped = (m == 1) != (g_present.multiplier == 1);
        g_present.multiplier = m;
        const std::string what = !flipped ? "now " + std::to_string(m) + "x"
                                 : m > 1    ? "detected (" + std::to_string(m) + "x)" : std::string("no longer detected");
        log("[nr] Present layers: %.3f presents per game frame: frame generation %s; source=%s; RenoDX layers run %s",
            double(g_present.cadence.cadence), what.c_str(),
            g_present.cadence.declared ? "DLSSG.MultiFrameCount"
            : g_present.cadence.source == nr::pe::present::FgSource::kLegacyTiming ? "legacy timing" : "stable timing",
            top ? "at the game's own Present: once a game frame, frame generation made from their picture"
            : first_renodx && first_renodx->layer->cfg.renodx.values.present_frames == 1
                ? "on the first present after each game frame (NRPresentFrames=1)"
                : m > 1 ? "on every present, motion scaled by 1/multiplier (NRPresentFrames=0)"
                        : "on every present (NRPresentFrames=0)");
    }
    // The presents a game frame has HERE: one at the game's own Present (generated frames never pass it)
    const uint32_t multiplier = top ? 1u : std::max(g_present.multiplier, 1u);
    if (!ensure_ring(device) || !ensure_texture(device, g_present.target, g_present.target_state, format, w, h)) return;
    // The ring's next slot, waited for (bounded) while a Present of eight ago still runs
    const uint32_t slot = uint32_t((g_present.submitted + 1) % kPresentRing);
    uint64_t done = g_present.fence->GetCompletedValue();
    if (done < g_present.slot_value[slot]) {
        if (g_present.slot_wait_off) return;
        // the event may still hold an older wait's signal (one that timed out and completed later): the fence's own
        // value, read after the wake, is what says the slot is free
        ResetEvent(g_present.event);
        if (FAILED(g_present.fence->SetEventOnCompletion(g_present.slot_value[slot], g_present.event))) return;
        ID3D12Fence* const fence_was = g_present.fence;
        ID3D12Resource* const target_was = g_present.target;
        const uint64_t waited_for = g_present.slot_value[slot], submitted_was = g_present.submitted;
        guard.unlock();
        const bool completed = WaitForSingleObject(g_present.event, kPresentSlotWaitMs) == WAIT_OBJECT_0;
        guard.lock();
        // the lock was let go: the ring and the copy may have been made anew meanwhile (another device, a resize), or
        // another Present took the slot: this one leaves the frame alone
        if (g_present.device != device || !g_present.list || !g_present.target || g_present.fence != fence_was ||
            g_present.target != target_was || g_present.slot_value[slot] != waited_for || g_present.submitted != submitted_was)
            return;
        done = g_present.fence->GetCompletedValue();
        if (!completed || done < waited_for) {
            g_present.slot_wait_off = true;
            log("[nr] Present layers: a command list slot was still on the GPU after %u ms; that Present keeps the game's "
                "image, and Presents stop waiting until a slot frees on its own", unsigned(kPresentSlotWaitMs));
            return;
        }
    }
    g_present.slot_wait_off = false;
    // Which guide sets this Present may read: submitted and ordered before it (on its queue, or complete on another)
    bool ordered[kPresentGuideSets] = {};
    {
        std::lock_guard<std::mutex> cg(g_capture_lock);
        for (uint32_t k = 0; k < kPresentGuideSets; ++k) {
            const GuideSet& gs = g_present.sets[k];
            ordered[k] = gs.serial && !gs.list && gs.queue &&
                         (gs.queue == queue || (gs.fence && gs.fence->GetCompletedValue() >= gs.value));
        }
    }
    // RenoDX's guides: the newest fresh one of the back buffer's aspect
    GuideSet* guides = nullptr;
    for (uint32_t k = 0; k < kPresentGuideSets; ++k) {
        GuideSet& gs = g_present.sets[k];
        if (!ordered[k] || !fresh(gs) || (guides && gs.serial < guides->serial)) continue;
        guides = &gs;
    }
    ID3D12GraphicsCommandList* l = g_present.list;
    if (FAILED(g_present.allocators[slot]->Reset()) || FAILED(l->Reset(g_present.allocators[slot], nullptr))) return;
    nr::gpu::reset(l);
    nr::gpu::Scope recording(nr::asi::hudcap::d3d12_recording(l));
    g_d12_uses.touch();
    transition(l, back, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
    transition(l, g_present.target, g_present.target_state, D3D12_RESOURCE_STATE_COPY_DEST);
    l->CopyResource(g_present.target, back);
    transition(l, g_present.target, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    PresentShot shot;   // F5: the frame as the game presents it
    if (nr::pe::screenshot::wants_final() && shot.prepare(device, g_present.target)) shot.copy(l, g_present.target, 0);
    // The guide sets read by this Present, shader-readable for it and back to COPY_DEST after
    std::vector<GuideSet*> readable;
    auto read = [&](GuideSet* gs) {
        if (!gs || std::find(readable.begin(), readable.end(), gs) != readable.end()) return;
        transition(l, gs->depth, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        transition(l, gs->motion, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        readable.push_back(gs);
    };
    bool ran = false;
    namespace route = nr::pe::renodx_route;
    // [Inspect]: the Present layers compared (the final picture is theirs: Present is the last placement), the frame
    // taken before the first of them and shown after the last
    std::vector<const Layer*> run_layers;
    for (const Run& run : runs) run_layers.push_back(run.layer);
    size_t ia = 0, ib = 0;
    const bool inspect = inspect_span(chain->inspect, run_layers, true, &ia, &ib) && inspect_format(format);
    const long held = chosen_index(chain->inspect, run_layers);
    if (held < 0 && g_present.hold.held) {   // the hold moved off these layers: the held one back to live
        g_hold_released.insert(g_present.hold.layer);
        g_present.hold.held = false;
    }
    // HUD FIX: the frame before the layers kept; composed after the last of them (before a final-picture comparison
    // shows it). Captured (Auto's first choice): the layers run on the copy made before the HUD, the difference of the
    // two is the HUD (source 2; Auto keeps the estimate's state too); else the estimate (Auto, Estimated).
    const uint32_t hud_source = hud_captured ? 2u : hud_mode == 1 || hud_mode == 4 ? 4u : 0u;   // hud_captured: FG's too
    const bool hud_estimate = hud_source == 4 || (hud_captured && hud_mode == 1);
    bool hud = false;
    if (hud_source && !runs.empty()) {
        PresentPath& P = g_present;
        const auto desc_of = [](ID3D12Resource* r) { return r ? r->GetDesc() : D3D12_RESOURCE_DESC{}; };
        if (desc_of(P.hud_back).Format != format || desc_of(P.hud_back).Width != w || desc_of(P.hud_back).Height != h)
            P.hud_prev_valid = false;
        if (desc_of(P.hud_alpha).Width != w || desc_of(P.hud_alpha).Height != h) P.hud_fresh = true;
        if (hud_estimate && P.hud_estimated_at + 1 != P.presents) {   // after a gap: no stale confidence
            P.hud_fresh = true;
            P.hud_prev_valid = false;
        }
        hud = ensure_texture(device, P.hud_back, P.hud_back_state, format, w, h) &&
              ensure_texture(device, P.hud_prev, P.hud_prev_state, format, w, h) &&
              ensure_texture(device, P.hud_alpha, P.hud_alpha_state, DXGI_FORMAT_R16G16B16A16_FLOAT, w, h) &&
              ensure_texture(device, P.hud_stats, P.hud_stats_state, DXGI_FORMAT_R32_UINT, hud_stats_w(w), hud_stats_h(h));
        if (!hud && !P.hud_said) { P.hud_said = true; log("[nr] HUD FIX: its textures could not be made; it does not run"); }
        const auto copy = [&](ID3D12Resource* from, ID3D12Resource* to) {
            transition(l, from, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            transition(l, to, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
            l->CopyResource(to, from);
            transition(l, to, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            transition(l, from, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        };
        if (hud) {
            copy(P.target, P.hud_back);
            if (!P.hud_prev_valid) { copy(P.target, P.hud_prev); P.hud_prev_valid = true; }
            if (hud_fg && !hud_cap.fg.copied) {   // frame generation's HUD-less frame, valid until the present: copied now
                transition(l, hud_cap.fg.game, hud_cap.fg.state, D3D12_RESOURCE_STATE_COPY_SOURCE);
                transition(l, hud_cap.fg.copy, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
                l->CopyResource(hud_cap.fg.copy, hud_cap.fg.game);
                transition(l, hud_cap.fg.copy, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                transition(l, hud_cap.fg.game, D3D12_RESOURCE_STATE_COPY_SOURCE, hud_cap.fg.state);
            }
            if (hud_captured) copy(hud_hudless, P.target);   // the layers run on the frame without its HUD
        }
    }
    const auto hud_end = [&] {
        PresentPath& P = g_present;
        nr::asi::HudFixD3D12 r;
        r.back = P.hud_back; r.frame = P.target; r.alpha = P.hud_alpha; r.stats = P.hud_stats; r.previous = P.hud_prev;
        r.hudless = hud_captured ? hud_hudless : nullptr;
        // the estimate with the game's motion vectors: the newest fresh guides of the back buffer's aspect, their units
        // in output pixels (low-resolution vectors: by the output's size over the render size) per present
        if (hud_estimate && guides && guides->f.out_w && guides->f.render_w) {
            read(guides);
            const Frame& gf = guides->f;
            const bool low = (gf.flags & kFlagMvLowRes) != 0;
            const float sx = low ? float(gf.out_w) / float(gf.render_w) : 1.f, sy = low ? float(gf.out_h) / float(gf.render_h) : 1.f;
            const float per = 1.0f / float(multiplier);
            r.motion = guides->motion;
            r.motion_scale_x = gf.mv_x * sx * per * float(w) / float(gf.out_w);
            r.motion_scale_y = gf.mv_y * sy * per * float(h) / float(gf.out_h);
            r.motion_x = gf.motion_rect.x; r.motion_y = gf.motion_rect.y;
            r.motion_step_x = float(gf.motion_rect.width) / float(w);
            r.motion_step_y = float(gf.motion_rect.height) / float(h);
        }
        if (hud_fg && hud_cap.fg.substitute) {   // NR's answer on it handed to frame generation in its place
            transition(l, P.target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            transition(l, hud_cap.fg.game, hud_cap.fg.state, D3D12_RESOURCE_STATE_COPY_DEST);
            l->CopyResource(hud_cap.fg.game, P.target);
            transition(l, hud_cap.fg.game, D3D12_RESOURCE_STATE_COPY_DEST, hud_cap.fg.state);
            transition(l, P.target, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        if (!nr::asi::wilsjo2_hudfix(l, device, r, w, h, hud_source, chain->hud.show_mask, P.hud_fresh, P.hud_parity,
                                     hud_estimate)) {
            if (hud_captured) {   // the layers ran on the copy without the HUD: the frame as the game drew it goes back
                transition(l, P.hud_back, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
                transition(l, P.target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
                l->CopyResource(P.target, P.hud_back);
                transition(l, P.target, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                transition(l, P.hud_back, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            }
            if (hud_fg && hud_cap.fg.substitute && hud_cap.fg.copy) {   // and frame generation gets its own frame back
                transition(l, hud_cap.fg.copy, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
                transition(l, hud_cap.fg.game, hud_cap.fg.state, D3D12_RESOURCE_STATE_COPY_DEST);
                l->CopyResource(hud_cap.fg.game, hud_cap.fg.copy);
                transition(l, hud_cap.fg.game, D3D12_RESOURCE_STATE_COPY_DEST, hud_cap.fg.state);
                transition(l, hud_cap.fg.copy, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            }
            nr::pe::control::hud_facts("nothing this frame: its composition could not be recorded", nullptr, nullptr, nullptr);
            return;
        }
        if (hud_estimate) {
            P.hud_fresh = false;
            P.hud_parity ^= 1u;
            P.hud_estimated_at = P.presents;
        }
        // this frame's kept frame becomes the next one's previous by swapping the two textures' roles (the next
        // frame overwrites hud_back first) instead of copying it into hud_prev; both rest in UNORDERED_ACCESS
        std::swap(P.hud_back, P.hud_prev);
        std::swap(P.hud_back_state, P.hud_prev_state);
        ran = true;
    };
    for (size_t k = 0; k < runs.size(); ++k) {
        Run run = runs[k];
        const bool holding = long(k) == held && chain->inspect.hold;
        if (long(k) == held) hold_frame(l, device, chain->inspect, g_present.hold, g_present.target, row_number(*run.layer));
        auto& hi = g_present.hold;
        if (holding && hi.held) {
            if (!hi.inputs_ready) { hi.encoding = run.encoding; hi.white = run.white; }
            else { run.encoding = hi.encoding; run.white = hi.white; }
        }
        GuideSet held_set;
        const auto frozen = [&](GuideSet* live, nr::asi::Wilsjo2Frame wf) -> GuideSet* {
            if (!holding || !hi.held) return live;
            Frame f = live ? live->f : Frame{};
            f.color = f.output = g_present.target;
            f.depth = wf.depth; f.motion = wf.motion; f.exposure = wf.exposure;
            States st; st.depth = wf.depth_state; st.motion = wf.motion_state; st.exposure = wf.exposure_state;
            if (!hold_inputs(l, device, hi, f, st, wf)) return live;
            held_set.f = f; held_set.w2 = wf;
            held_set.depth = f.depth; held_set.motion = f.motion;
            held_set.serial = live ? live->serial : 0;
            return &held_set;
        };

        if (inspect && k == ia) inspect_copy(l, device, g_present.inspect_off, g_present.inspect_off_state, g_present.target);
        [&] {
        const Layer& layer = *run.layer;
        PresentLayer& pl = g_present.layers[layer.cfg.number];
        pl.processing = layer.cfg.processing;
        g_present.held = true;
        if (!renodx_layer(layer)) {
            // wilsjo2's finished picture: the newest capture of this or the previous frame (with frame
            // generation: of the last game frame's presents), of exactly the back buffer's size, each capture once
            GuideSet* g = nullptr;
            for (uint32_t k = 0; k < kPresentGuideSets; ++k) {
                GuideSet& gs = g_present.sets[k];
                if (!ordered[k] || gs.serial <= pl.consumed ||
                    g_present.presents - gs.captured_at > uint64_t(multiplier) ||
                    gs.out_w != w || gs.out_h != h || (g && gs.serial < g->serial))
                    continue;
                g = &gs;
            }
            if (!nr::dlssnr::nr_on_now()) {   // NRToggleKey, as the layers at the upscaler take it
                pl.w2_reset = true;
                layer_note(layer, 2, 3, "NR is off (toggle key)");
                return;
            }
            if (!g && holding && hi.inputs_ready) {
                held_set.f = hi.input; held_set.w2 = hi.w2;
                held_set.depth = hi.guides[0]; held_set.motion = hi.guides[1];
                g = &held_set;
            }
            if (!g) {
                // A Present of frame generation between two game frames: the last one was taken a moment ago (one
                // capture, one run): nothing to say, the row stays as it is
                if (pl.consumed && g_present.presents - pl.consumed_present <= multiplier + 1u)
                    return;
                pl.w2_reset = true;
                layer_note(layer, 2, 3, "waits for a game frame of the back buffer's size");
                return;
            }
            if (g != &held_set) read(g);
            pl.consumed = g->serial;
            pl.consumed_present = g_present.presents;
            nr::asi::Wilsjo2Frame wf = g->w2;
            wf.depth = g->depth; wf.motion = g->motion; wf.exposure = nullptr;
            wf.depth_state = wf.motion_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            wf.hdr = run.encoding != 1;
            wf.pre_exposure = 1.0f;
            if (auto* h = frozen(g, wf); h == &held_set) wf = h->w2;
            wf.reset = wf.reset || pl.w2_reset || hold_released(row_number(layer));
            // HDR: the absolute display encodings' 203-nit white in 80-nit scRGB units, no exposure measurement
            nr::wilsjo2::Settings ws = layer.w2;
            if (wf.hdr) { ws.white_point_scale = 203.0f / 80.0f; ws.white_point_source = 0; }
            if (!pl.w2) { pl.w2 = nr::asi::wilsjo2_owner(); nr::asi::wilsjo2_owner_at_present(*pl.w2); }
            route::CallNetwork network(layer.network);
            route::CallLayer number(row_number(layer));
            // the row before the run: what the model says of it during the run (passed through, building) stays
            layer_note(layer, 2, 3, "not recorded yet");
            bool done_run = false;
            if (run.encoding == 2) {   // PQ: through linear BT.709 scRGB and back
                if (ensure_texture(device, g_present.linear, g_present.linear_state, DXGI_FORMAT_R16G16B16A16_FLOAT, w, h) &&
                    ensure_texture(device, g_present.encoded, g_present.encoded_state, format, w, h)) {
                    transition(l, g_present.target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                    if (nr::asi::wilsjo2_convert(l, device, g_present.target, nullptr, g_present.linear, w, h, true)) {
                        done_run = nr::asi::wilsjo2_run(l, device, *pl.w2, wf, ws, g_present.linear, w, h, false);
                        if (done_run) {
                            transition(l, g_present.linear, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                            if (nr::asi::wilsjo2_convert(l, device, g_present.linear, g_present.target, g_present.encoded, w, h, false)) {
                                transition(l, g_present.encoded, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
                                transition(l, g_present.target, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
                                l->CopyResource(g_present.target, g_present.encoded);
                                transition(l, g_present.target, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                                transition(l, g_present.encoded, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                            }
                            transition(l, g_present.linear, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                        }
                    }
                    transition(l, g_present.target, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                }
            } else {
                done_run = nr::asi::wilsjo2_run(l, device, *pl.w2, wf, ws, g_present.target, w, h, false);
            }
            pl.w2_reset = !done_run;
            if (!done_run) layer_note(layer, 2, 3, "its run could not be recorded (see the log)");
            ran |= done_run;
            return;
        }
        const nr::renodx::Settings& rs = layer.cfg.renodx.values;
        // NRPresentGuides: 2 never the game's; 1 required (none: this layer leaves the frame alone); 0 optional
        GuideSet* g = rs.present_guides == 2u ? nullptr : guides;
        if (g) read(g);
        nr::asi::Wilsjo2Frame hw;
        if (g) { hw = g->w2; hw.depth = g->depth; hw.motion = g->motion; }
        hw.exposure = nullptr;
        hw.depth_state = hw.motion_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        g = frozen(g, hw);
        if (!g && rs.present_guides == 1u) {
            layer_note(layer, 2, 3, "waits for the game's depth and motion (NRPresentGuides)");
            return;
        }
        const bool new_frame = g && g->serial != pl.consumed;
        if (!holding && rs.present_frames == 1u && g && !new_frame) return;   // NRPresentFrames = 1: once per game frame (as it ran)
        // Every present of a generated run moves 1/multiplier of a game frame (NRPresentFrames = 0)
        const float share = g && rs.present_frames == 0u ? 1.0f / float(multiplier) : 1.0f;
        if (pl.nr && (pl.w != w || pl.h != h)) { nr::dlssnr::release_d3d12(pl.nr); pl.nr = nullptr; }
        if (!pl.nr) {
            int rc = 0;
            pl.nr = nr::dlssnr::create_d3d12(device, l, nullptr, w, h, 0, 1, nr::dlssnr::Controls6{}, &rc);
            if (!pl.nr) { layer_note(layer, 2, 3, "its NR could not be made (see the log)"); return; }
            pl.w = w; pl.h = h;
        }
        layer_note(layer, 2, 3, "not recorded yet");   // it runs now
        bool reset = (new_frame && g->f.reset) || hold_released(row_number(layer));   // the game's Reset rides the first Present of its frame only
        if ((g != nullptr) != pl.real_guides) { pl.real_guides = g != nullptr; reset = true; }
        route::Frame rf;
        rf.present = run.encoding;
        rf.present_white_nits = run.white;
        route::begin_run(rf);
        route::note_encode(run.encoding == 3, w, h);
        route::set_direct(g_present.target);
        {
            route::CallStyle style(true);
            route::CallSettings settings(&rs);
            route::CallNetwork network(layer.network);
            route::CallLayer number(row_number(layer));
            const Rect whole{0, 0, w, h};
            nr::dlssnr::evaluate_d3d12(l, pl.nr, nullptr, g_present.target, g ? g->depth : nullptr,
                                       g ? g->motion : nullptr, g_present.target, w, h, g ? g->f.depth_rect : whole,
                                       g ? g->f.motion_rect : whole, g ? ((g->f.flags & kFlagDepthInverted) ? 1 : 0) : 1,
                                       reset ? 1 : 0, nr::dlssnr::Controls6{}, g ? g->f.mv_x * share : 1.f,
                                       g ? g->f.mv_y * share : 1.f);
        }
        route::end_run();
        if (g) pl.consumed = g->serial;
        ran = true;
            }();
        if (hud && k + 1 == runs.size()) hud_end();
        if (inspect && k == ib)
            inspect_show(l, device, chain->inspect, g_present.inspect_off, g_present.inspect_off_state, g_present.inspect_on,
                         g_present.inspect_on_state, g_present.target);
    }
    for (GuideSet* gs : readable) {
        transition(l, gs->depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        transition(l, gs->motion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    }
    shot.copy(l, g_present.target, 1);   // F5: the final picture
    transition(l, g_present.target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    transition(l, back, D3D12_RESOURCE_STATE_COPY_SOURCE, ran ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_PRESENT);
    if (ran) {
        l->CopyResource(back, g_present.target);
        transition(l, back, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    }
    transition(l, g_present.target, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    g_present.target_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    if (FAILED(l->Close())) return;
    const uint64_t value = ++g_present.submitted;
    for (GuideSet* gs : readable) gs->read_value = value;
    guard.unlock();   // our own ExecuteCommandLists comes through on_execute
    ID3D12CommandList* lists[] = {l};
    queue->ExecuteCommandLists(1, lists);
    shot.submitted = true;
    const HRESULT signalled = queue->Signal(g_present.fence, value);
    std::lock_guard<std::mutex> relock(g_lock);
    static bool said_signal = false;
    if (FAILED(signalled) && !said_signal) {   // (a lost device): the slot waits time out, and Presents stop waiting
        said_signal = true;
        log("[nr] Present layers: the queue's Signal failed (0x%08lX)", (unsigned long)signalled);
    }
    g_present.slot_value[slot] = value;
    if (SUCCEEDED(signalled)) {   // F5's pair, once our list is done
        const uint8_t hdr = w2_space == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709 ? 1 : w2_space == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ? 2 : 0;
        shot.deliver(g_present.fence, value, hdr, runs.size());
    }
}

// NR Control's D3D12 Present hook (the system's DXGI, under OptiScaler): the Present layers when the game's own swap
// chain did not serve them (game_present), else the final-picture screenshot's late NR.
// NR_DEBUG_PRESENT_DUMP=<folder> (measurement only): every picture the swap chain shows, as it reaches the system's
// Present - generated frames included - copied to the CPU (the Present waits for it) and written raw, rows packed, as
// p_NNNNN.raw; a line per Present in dlssnr-amd.log. For comparing what frame generation shows between builds.
void debug_present_dump(IDXGISwapChain* sc, ID3D12CommandQueue* queue) {
    static const std::string folder = [] {
        char b[MAX_PATH] = {};
        return GetEnvironmentVariableA("NR_DEBUG_PRESENT_DUMP", b, MAX_PATH) ? std::string(b) : std::string();
    }();
    if (folder.empty()) return;
    static uint64_t n = 0, seen = 0;
    static const uint64_t from = [] { char b[32] = {}; return GetEnvironmentVariableA("NR_DEBUG_PRESENT_FROM", b, 32) ? std::strtoull(b, nullptr, 10) : 0ull; }();
    if (seen++ < from || n >= 60) return;   // 60 presents from NR_DEBUG_PRESENT_FROM on
    static ID3D12Device* device = nullptr;
    static ID3D12CommandAllocator* allocator = nullptr;
    static ID3D12GraphicsCommandList* list = nullptr;
    static ID3D12Fence* fence = nullptr;
    static uint64_t value = 0;
    static ID3D12Resource* readback = nullptr;
    static uint64_t readback_size = 0;
    if (!device) {
        if (FAILED(queue->GetDevice(IID_PPV_ARGS(&device))) ||
            FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
            FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(&list))) ||
            FAILED(list->Close()) || FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
            return;
        CreateDirectoryA(folder.c_str(), nullptr);
    }
    IDXGISwapChain3* sc3 = nullptr;
    if (FAILED(sc->QueryInterface(IID_PPV_ARGS(&sc3))) || !sc3) return;
    const UINT index = sc3->GetCurrentBackBufferIndex();
    sc3->Release();
    ID3D12Resource* back = nullptr;
    if (FAILED(sc->GetBuffer(index, IID_PPV_ARGS(&back))) || !back) return;
    const D3D12_RESOURCE_DESC bd = back->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT rows = 0;
    UINT64 row_bytes = 0, total = 0;
    device->GetCopyableFootprints(&bd, 0, 1, 0, &fp, &rows, &row_bytes, &total);
    if (total > readback_size) {
        if (readback) readback->Release();
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = total; rd.Height = 1; rd.DepthOrArraySize = 1;
        rd.MipLevels = 1; rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        readback = nullptr;
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                   IID_PPV_ARGS(&readback)))) { back->Release(); readback_size = 0; return; }
        readback_size = total;
    }
    allocator->Reset();
    list->Reset(allocator, nullptr);
    transition(list, back, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
    dst.pResource = readback; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = fp;
    src.pResource = back; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    transition(list, back, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
    list->Close();
    back->Release();
    ID3D12CommandList* lists[] = {list};
    queue->ExecuteCommandLists(1, lists);
    queue->Signal(fence, ++value);
    if (fence->GetCompletedValue() < value) {
        HANDLE e = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (e && SUCCEEDED(fence->SetEventOnCompletion(value, e))) WaitForSingleObject(e, 5000);
        if (e) CloseHandle(e);
    }
    if (fence->GetCompletedValue() < value) return;   // not done: nothing read (the copy may still be written)
    uint8_t* m = nullptr;
    if (FAILED(readback->Map(0, nullptr, reinterpret_cast<void**>(&m))) || !m) return;
    char path[MAX_PATH + 32];
    std::snprintf(path, sizeof path, "%s\\p_%05llu.raw", folder.c_str(), static_cast<unsigned long long>(n));
    if (FILE* f = std::fopen(path, "wb")) {
        for (UINT y = 0; y < rows; ++y) std::fwrite(m + fp.Offset + size_t(y) * fp.Footprint.RowPitch, 1, size_t(row_bytes), f);
        std::fclose(f);
    }
    D3D12_RANGE none{0, 0};
    readback->Unmap(0, &none);
    log("[nr] debug present dump %llu: %ux%u format %u", static_cast<unsigned long long>(n), unsigned(bd.Width), bd.Height,
        unsigned(bd.Format));
    ++n;
}

// Frame generation as NR Control shows it: every present the system's DXGI sees (generated ones included) against the
// game's frames (its upscaler's evaluates), over a second; and the game's own DLSS-G when it declares one. A
// measurement, said as one: a game that presents twice a frame for other reasons reads the same.
std::atomic<uint64_t> g_fg_frames{0};
void frame_generation_line() {
    static uint64_t presents = 0, frames_at = 0;
    static ULONGLONG since = 0;
    ++presents;
    const ULONGLONG now = GetTickCount64();
    if (!since) { since = now; frames_at = g_fg_frames.load(); return; }
    if (now - since < 1000) return;
    const uint64_t frames = g_fg_frames.load() - frames_at;
    char line[160];
    const auto& decl = g_present.declaration;
    if (decl.multiplier > 1 && now - decl.ms < 2000)
        std::snprintf(line, sizeof line, "Frame generation: the game's DLSS-G, %ux (its own word)", decl.multiplier);
    else if (!frames)
        std::snprintf(line, sizeof line, "Frame generation: not known (no game frame in the last second)");
    else {
        const double r = double(presents) / double(frames);
        std::snprintf(line, sizeof line, "Frame generation: %.2f presents per game frame%s", r,
                      r >= 1.8 ? " - frame generation is likely on" : r <= 1.2 ? " - none seen" : "");
    }
    nr::pe::control::asi_facts(nullptr, line);
    presents = 0;
    since = now;
    frames_at = g_fg_frames.load();
}

// Whether this Present goes into present_layers: while the chain has Present layers, every one; else twice a second,
// so a Present layer turned on in NR Control runs without waiting for a game evaluate (the game in its menus, an
// upscaler not called yet: Present layers need none).
bool present_due() {
    if (g_present.wanted.load()) return true;
    const uint64_t now = GetTickCount64();
    if (now - g_present.wanted_checked_ms.load() < 500) return false;
    g_present.wanted_checked_ms = now;
    return true;
}

void on_present(IDXGISwapChain* sc, ID3D12CommandQueue* queue, int encoding) {
    nr::pe::watchdog::beat();
    nr::pe::watchdog::Phase phase("the system's Present (NR Control's hook)");
    frame_generation_line();
    const bool served_above = GetTickCount64() - g_present.top_ms.load() < 1000;
    if (!served_above && (present_due() || g_present.held.load()))   // held: features to let go of
        present_layers(sc, queue, false);
    if (!g_present.wanted.load()) late_present(sc, queue, encoding);
    debug_present_dump(sc, queue);   // what is shown, after whatever ran here
}

// ---- the Present layers before frame generation: the swap chain the game itself holds ------------------------------
// NR Control's Present hook sits in the system's DXGI, under OptiScaler's swap chain: with OptiScaler's frame generation
// on, the frames it generates were made from the picture without the Present layers by then, and those frames would be
// shown without them. So the layers run at the Present of the swap chain the game holds (OptiScaler's frame-generation
// proxy while FG is on), before OptiScaler's own Present work: once a game frame, and the generated frames are made
// from that picture - as the forked OptiScaler ran its finished picture inside its FG present hook before generating,
// and as RenoDX's pre-FG composition runs NR once a game frame before DLSS-G. Found through the game's dxgi.dll
// (OptiScaler as dxgi.dll): CreateDXGIFactory*, its factories' CreateSwapChain*, and the swap chains returned to the
// game (only the outermost creation: the ones frame generation makes inside it are under OptiScaler); every function
// hooked once. MinHook patches the function itself; each hooked function gets a detour of its own (an instance of the
// templates below) holding its own original, written by MH_CreateHook before the hook is enabled - so a call that
// arrives the moment the hook goes live finds it, and so does one through an object whose vtable someone else (an
// overlay) has since pointed elsewhere.
std::mutex g_sc_lock;
std::unordered_map<IDXGISwapChain*, ID3D12CommandQueue*> g_game_swapchains;   // the game's, with its queue (AddRef)
std::unordered_map<IDXGISwapChain*, ID3D11Device*> g_game_swapchains11;       // a D3D11 game's, with its device (AddRef)
void d11_present_layers(IDXGISwapChain* sc, ID3D11Device* device);   // the D3D11 section
bool d11_present_due();
std::atomic<ULONGLONG> g_d11_top_ms{0};   // the game's own D3D11 swap chain served the layers then
thread_local int t_creating = 0;

enum DetourKind { kDetourPresent, kDetourPresent1, kDetourCreate, kDetourCreateHwnd, kDetourCreateCore, kDetourCreateComp,
                  kDetourKinds };
constexpr int kDetoursPerKind = 8;   // functions of one kind: one per class of swap chain / factory the game makes
void* g_detour_original[kDetourKinds][kDetoursPerKind] = {};   // never cleared: a hook is never removed
int g_detours_used[kDetourKinds] = {};                         // g_sc_lock
std::unordered_map<void*, int> g_hooked_functions;             // hooked (or not hookable) function -> instance, -1 not
                                                                // hookable, -2 hooked by NR Control (g_sc_lock)

void game_present_before(IDXGISwapChain* sc) {
    ID3D12CommandQueue* queue = nullptr;
    ID3D11Device* d11 = nullptr;
    {
        std::lock_guard<std::mutex> guard(g_sc_lock);
        const auto it = g_game_swapchains.find(sc);
        if (it != g_game_swapchains.end()) queue = it->second;
        const auto it11 = g_game_swapchains11.find(sc);
        if (it11 != g_game_swapchains11.end()) { d11 = it11->second; d11->AddRef(); }
    }
    // NR_PRESENT_UNDER=1 (measurement only): leave the layers to NR Control's Present hook, under OptiScaler (and after
    // its frame generation), for comparing the two places
    static const bool under = [] { char b[8] = {}; return GetEnvironmentVariableA("NR_PRESENT_UNDER", b, 8) && b[0] == '1'; }();
    if (d11) {
        if (!under && d11_present_due()) {
            d11_present_layers(sc, d11);
            g_d11_top_ms = GetTickCount64();
        }
        d11->Release();
        return;
    }
    if (!queue || under || !present_due()) return;
    present_layers(sc, queue, true);
    g_present.top_ms = GetTickCount64();
}

void adopt_swapchain(IUnknown* device, IDXGISwapChain* sc);

template <int K>
struct ScDetours {
    template <class Fn>
    static Fn original(DetourKind kind) { return reinterpret_cast<Fn>(g_detour_original[kind][K]); }

    static HRESULT STDMETHODCALLTYPE present(IDXGISwapChain* sc, UINT sync, UINT flags) {
        nr::pe::watchdog::beat();
        if (!(flags & DXGI_PRESENT_TEST)) game_present_before(sc);
        using Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
        const nr::asi::hudcap::PresentCall call;   // frame generation's hand-overs now: OptiScaler's own
        return original<Fn>(kDetourPresent)(sc, sync, flags);
    }
    static HRESULT STDMETHODCALLTYPE present1(IDXGISwapChain1* sc, UINT sync, UINT flags,
                                              const DXGI_PRESENT_PARAMETERS* params) {
        nr::pe::watchdog::beat();
        if (!(flags & DXGI_PRESENT_TEST)) game_present_before(sc);
        using Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
        const nr::asi::hudcap::PresentCall call;   // frame generation's hand-overs now: OptiScaler's own
        return original<Fn>(kDetourPresent1)(sc, sync, flags, params);
    }
    static HRESULT STDMETHODCALLTYPE create(IDXGIFactory* f, IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc,
                                            IDXGISwapChain** out) {
        using Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
        ++t_creating;
        const HRESULT hr = original<Fn>(kDetourCreate)(f, device, desc, out);
        --t_creating;
        if (SUCCEEDED(hr) && out && !t_creating) adopt_swapchain(device, *out);
        return hr;
    }
    static HRESULT STDMETHODCALLTYPE create_hwnd(IDXGIFactory2* f, IUnknown* device, HWND hwnd,
                                                 const DXGI_SWAP_CHAIN_DESC1* desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs,
                                                 IDXGIOutput* output, IDXGISwapChain1** out) {
        using Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
                                               const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
        ++t_creating;
        const HRESULT hr = original<Fn>(kDetourCreateHwnd)(f, device, hwnd, desc, fs, output, out);
        --t_creating;
        if (SUCCEEDED(hr) && out && !t_creating) adopt_swapchain(device, *out);
        return hr;
    }
    static HRESULT STDMETHODCALLTYPE create_core(IDXGIFactory2* f, IUnknown* device, IUnknown* window,
                                                 const DXGI_SWAP_CHAIN_DESC1* desc, IDXGIOutput* output,
                                                 IDXGISwapChain1** out) {
        using Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*,
                                               IDXGIOutput*, IDXGISwapChain1**);
        ++t_creating;
        const HRESULT hr = original<Fn>(kDetourCreateCore)(f, device, window, desc, output, out);
        --t_creating;
        if (SUCCEEDED(hr) && out && !t_creating) adopt_swapchain(device, *out);
        return hr;
    }
    static HRESULT STDMETHODCALLTYPE create_comp(IDXGIFactory2* f, IUnknown* device, const DXGI_SWAP_CHAIN_DESC1* desc,
                                                 IDXGIOutput* output, IDXGISwapChain1** out) {
        using Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*,
                                               IDXGISwapChain1**);
        ++t_creating;
        const HRESULT hr = original<Fn>(kDetourCreateComp)(f, device, desc, output, out);
        --t_creating;
        if (SUCCEEDED(hr) && out && !t_creating) adopt_swapchain(device, *out);
        return hr;
    }
    static void* of(DetourKind kind) {
        switch (kind) {
            case kDetourPresent: return reinterpret_cast<void*>(&present);
            case kDetourPresent1: return reinterpret_cast<void*>(&present1);
            case kDetourCreate: return reinterpret_cast<void*>(&create);
            case kDetourCreateHwnd: return reinterpret_cast<void*>(&create_hwnd);
            case kDetourCreateCore: return reinterpret_cast<void*>(&create_core);
            case kDetourCreateComp: return reinterpret_cast<void*>(&create_comp);
            default: return nullptr;
        }
    }
};

void* detour_of(DetourKind kind, int k) {
    switch (k) {
        case 0: return ScDetours<0>::of(kind);
        case 1: return ScDetours<1>::of(kind);
        case 2: return ScDetours<2>::of(kind);
        case 3: return ScDetours<3>::of(kind);
        case 4: return ScDetours<4>::of(kind);
        case 5: return ScDetours<5>::of(kind);
        case 6: return ScDetours<6>::of(kind);
        case 7: return ScDetours<7>::of(kind);
        default: return nullptr;
    }
}
static_assert(kDetoursPerKind == 8, "detour_of lists eight instances");

// The function in `object`'s vtable `slot`, hooked with the next free detour of `kind` (once per function). False when
// it runs unhooked.
bool hook_slot(void* object, int slot, DetourKind kind) {
    void* fn = (*reinterpret_cast<void***>(object))[slot];
    std::lock_guard<std::mutex> guard(g_sc_lock);
    if (const auto it = g_hooked_functions.find(fn); it != g_hooked_functions.end()) return it->second != -1;
    if (kind == kDetourPresent || kind == kDetourPresent1) {
        // A Present outside OptiScaler (no wrapper of its between the game and the system's DXGI) is NR Control's to
        // hook (nr_pe_menu.cpp, from its own thread: hooking it here too would race it); its hook calls on_present /
        // on_present11, the same place.
        HMODULE owner = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCWSTR>(fn), &owner);
        if (owner != g_opti) {
            g_hooked_functions[fn] = -2;
            return true;
        }
    }
    if (g_detours_used[kind] >= kDetoursPerKind) {
        g_hooked_functions[fn] = -1;
        log("[nr] Present layers: more than %d DXGI functions of one kind (slot %d); this one runs unhooked",
            kDetoursPerKind, slot);
        return false;
    }
    const int k = g_detours_used[kind];
    const MH_STATUS created = MH_CreateHook(fn, detour_of(kind, k), &g_detour_original[kind][k]);
    g_hooked_functions[fn] = -1;
    if (created == MH_ERROR_ALREADY_CREATED) {   // the system's Present, hooked by NR Control: the same place
        g_hooked_functions[fn] = -2;
        return true;
    }
    if (created != MH_OK) {
        log("[nr] Present layers: a DXGI function could not be hooked (slot %d, %s); they run under OptiScaler's Present",
            slot, MH_StatusToString(created));
        return false;
    }
    ++g_detours_used[kind];   // this instance's original is set (MinHook wrote it) and stays this function's
    if (const MH_STATUS e = MH_EnableHook(fn); e != MH_OK) {
        MH_RemoveHook(fn);
        log("[nr] Present layers: a DXGI function could not be hooked (slot %d, %s); they run under OptiScaler's Present",
            slot, MH_StatusToString(e));
        return false;
    }
    g_hooked_functions[fn] = k;
    return true;
}

// NR Control, drawn by this module over the game's swap chain (nr_pe_menu.hpp), once: from the game's first swap chain
// (adopt_swapchain: End works while the game is still in its menus, before it calls an upscaler) or its first evaluate.
// Not in DllMain.
void menu_once(ID3D12Device* device) {
    static std::once_flag menu;
    std::call_once(menu, [device] {
        nr::pe::menu::watch_queues(device);
        nr::pe::menu::on_d3d12_present(&on_present);
        nr::pe::menu::on_execute(&on_execute);
        nr::pe::menu::install();
    });
}

// The D3D11 menu, and NR Control's DXGI Present hook calling on_present11 (the Present layers when the game's own call
// was not reached: no OptiScaler wrapper between), once.
void on_present11(IDXGISwapChain* sc, ID3D11Device* device);
void menu_once11() {
    static std::once_flag menu;
    std::call_once(menu, [] {
        nr::pe::menu::on_d3d11_present(&on_present11);
        nr::pe::menu::install();
    });
}
void on_present11(IDXGISwapChain* sc, ID3D11Device* device) {
    nr::pe::watchdog::beat();
    frame_generation_line();
    if (GetTickCount64() - g_d11_top_ms.load() < 1000) return;   // served at the game's own call
    {   // a swap chain of the game's (adopt_swapchain), not one OptiScaler or an overlay made; while none was seen
        // being made (a creation path not hooked), every one
        std::lock_guard<std::mutex> guard(g_sc_lock);
        if (!g_game_swapchains11.empty() && !g_game_swapchains11.count(sc)) return;
    }
    if (d11_present_due()) d11_present_layers(sc, device);
}

void adopt_swapchain(IUnknown* device, IDXGISwapChain* sc) {
    ID3D12CommandQueue* queue = nullptr;
    if (!device || !sc) return;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&queue))) || !queue) {   // a D3D11 (D3D10) game's: its menu now
        ID3D11Device* d11 = nullptr;
        if (FAILED(device->QueryInterface(IID_PPV_ARGS(&d11))) || !d11) { nr::pe::menu::install(); return; }
        menu_once11();
        {
            std::lock_guard<std::mutex> guard(g_sc_lock);
            ID3D11Device*& slot = g_game_swapchains11[sc];
            if (slot) slot->Release();
            slot = d11;
        }
        bool hooked = hook_slot(sc, 8, kDetourPresent);
        IDXGISwapChain1* sc1 = nullptr;
        if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc1))) && sc1) {
            hooked = hook_slot(sc1, 22, kDetourPresent1) && hooked;
            sc1->Release();
        }
        log("[nr] Present layers: the game's D3D11 swap chain %p found%s", static_cast<void*>(sc),
            hooked ? "; they run at its Present, before OptiScaler's" : ", but its Present is not hooked (above)");
        return;
    }
    {
        ID3D12Device* d = nullptr;
        if (SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&d))) && d) {
            menu_once(d);
            // HUD FIX's capture: the device's render-target views and pipelines followed from now on
            const std::shared_ptr<const Chain> c = chain_now();
            nr::asi::hudcap::d3d12_device(d, c && (c->hud.mode == 1 || c->hud.mode == 3));
            d->Release();
        }
    }
    {
        std::lock_guard<std::mutex> guard(g_sc_lock);
        ID3D12CommandQueue*& slot = g_game_swapchains[sc];
        if (slot) slot->Release();
        slot = queue;
    }
    bool hooked = hook_slot(sc, 8, kDetourPresent);
    IDXGISwapChain1* sc1 = nullptr;
    if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc1))) && sc1) {
        hooked = hook_slot(sc1, 22, kDetourPresent1) && hooked;
        sc1->Release();
    }
    if (hooked)
        log("[nr] Present layers: the game's swap chain %p (presenting on queue %p) found; they run at its Present, "
            "before OptiScaler's", static_cast<void*>(sc), static_cast<void*>(queue));
    else
        log("[nr] Present layers: the game's swap chain %p found, but its Present is not hooked (above)",
            static_cast<void*>(sc));
}

void adopt_factory(void* factory) {
    if (!factory) return;
    IDXGIFactory* f = nullptr;
    if (SUCCEEDED(static_cast<IUnknown*>(factory)->QueryInterface(IID_PPV_ARGS(&f))) && f) {
        hook_slot(f, 10, kDetourCreate);
        f->Release();
    }
    IDXGIFactory2* f2 = nullptr;
    if (SUCCEEDED(static_cast<IUnknown*>(factory)->QueryInterface(IID_PPV_ARGS(&f2))) && f2) {
        hook_slot(f2, 15, kDetourCreateHwnd);
        hook_slot(f2, 16, kDetourCreateCore);
        hook_slot(f2, 24, kDetourCreateComp);
        f2->Release();
    }
}
using CreateFactoryFn = HRESULT(WINAPI*)(REFIID, void**);
using CreateFactory2Fn = HRESULT(WINAPI*)(UINT, REFIID, void**);
CreateFactoryFn real_create_factory = nullptr, real_create_factory1 = nullptr;
CreateFactory2Fn real_create_factory2 = nullptr;
HRESULT WINAPI hk_create_factory(REFIID riid, void** out) {
    const HRESULT hr = real_create_factory(riid, out);
    if (SUCCEEDED(hr) && out && !t_creating) adopt_factory(*out);
    return hr;
}
HRESULT WINAPI hk_create_factory1(REFIID riid, void** out) {
    const HRESULT hr = real_create_factory1(riid, out);
    if (SUCCEEDED(hr) && out && !t_creating) adopt_factory(*out);
    return hr;
}
HRESULT WINAPI hk_create_factory2(UINT flags, REFIID riid, void** out) {
    const HRESULT hr = real_create_factory2(flags, riid, out);
    if (SUCCEEDED(hr) && out && !t_creating) adopt_factory(*out);
    return hr;
}

// From InitializeASI: the factory creation of the dxgi.dll the game loads (OptiScaler itself when it is dxgi.dll).
void hook_game_dxgi() {
    HMODULE dxgi = GetModuleHandleW(L"dxgi.dll");
    if (!dxgi) {
        log("[nr] Present layers: no dxgi.dll loaded with OptiScaler; they run under OptiScaler's Present (after its "
            "frame generation)");
        return;
    }
    const std::pair<const char*, std::pair<void*, void**>> entries[] = {
        {"CreateDXGIFactory", {reinterpret_cast<void*>(&hk_create_factory), reinterpret_cast<void**>(&real_create_factory)}},
        {"CreateDXGIFactory1", {reinterpret_cast<void*>(&hk_create_factory1), reinterpret_cast<void**>(&real_create_factory1)}},
        {"CreateDXGIFactory2", {reinterpret_cast<void*>(&hk_create_factory2), reinterpret_cast<void**>(&real_create_factory2)}}};
    int hooked = 0;
    for (const auto& [name, hk] : entries) {
        void* fn = reinterpret_cast<void*>(GetProcAddress(dxgi, name));
        if (fn && MH_CreateHook(fn, hk.first, hk.second) == MH_OK && MH_EnableHook(fn) == MH_OK) ++hooked;
    }
    log("[nr] Present layers: %d of 3 DXGI factory entry points hooked in the game's dxgi.dll", hooked);
}

// ---- the wilsjo2 style (nr_asi_wilsjo2.hpp) ------------------------------------------------------------------------

// Spec 4.5: `wanted` of an allocation from an origin, clipped to what the allocation has (0: all of it).
Rect guide_subrect(ID3D12Resource* r, uint32_t want_w, uint32_t want_h, uint32_t x, uint32_t y) {
    const D3D12_RESOURCE_DESC d = r->GetDesc();
    const uint32_t aw = uint32_t(d.Width), ah = d.Height;
    Rect out;
    out.x = std::min(x, aw);
    out.y = std::min(y, ah);
    const uint32_t vw = aw - out.x, vh = ah - out.y;
    out.width = std::min(want_w ? want_w : vw, vw);
    out.height = std::min(want_h ? want_h : vh, vh);
    return out;
}

nr::asi::Wilsjo2Frame wilsjo2_frame(const NVSDK_NGX_Parameter* p, const Frame& f, const States& st) {
    nr::asi::Wilsjo2Frame w;
    w.depth = f.depth; w.motion = f.motion; w.exposure = f.exposure;
    w.depth_state = st.depth; w.motion_state = st.motion; w.exposure_state = st.exposure;
    w.flags = f.flags_known ? f.flags : 0u;
    // 4.4: IsHDR (or AutoExposure, OptiScaler fix 5) and the authority - the output, else the colour - a float format
    const DXGI_FORMAT authority = (f.output ? f.output : f.color)->GetDesc().Format;
    w.hdr = f.flags_known && (f.flags & (kFlagIsHdr | kFlagAutoExposure)) != 0 && float_colour(authority);
    w.reset = f.reset;
    w.pre_exposure = f.pre_exposure;
    w.mv_x = std::isfinite(f.mv_x) && f.mv_x != 0.0f ? f.mv_x : 1.0f;
    w.mv_y = std::isfinite(f.mv_y) && f.mv_y != 0.0f ? f.mv_y : 1.0f;
    // 4.5, with the render subrect as given; motion vectors at the render extent when the game says so (or, its create
    // not seen, as OptiScaler's own FSR / FidelityFX inputs have them), else at the output's
    // (display-size vectors the resource cannot hold: the part it holds, guide_subrect - 0.0.7's GuideSubrect)
    const bool low_res_mv = !f.flags_known || (f.flags & kFlagMvLowRes) != 0;
    w.depth_rect = guide_subrect(f.depth, f.subrect_w, f.subrect_h, get_uint(p, "DLSS.Input.Depth.Subrect.Base.X"),
                                 get_uint(p, "DLSS.Input.Depth.Subrect.Base.Y"));
    w.motion_rect = guide_subrect(f.motion, low_res_mv ? f.subrect_w : f.out_w, low_res_mv ? f.subrect_h : f.out_h,
                                  get_uint(p, "DLSS.Input.MV.SubrectBase.X"), get_uint(p, "DLSS.Input.MV.SubrectBase.Y"));
    return w;
}

// The wilsjo2 processing's own conditions for running before the upscaler: the colour at origin 0,
// one mip, a valid active extent (a 2-D, one-sample, one-slice allocation of 1..16384 a side; the render subrect both
// 0 = all of it, one 0 or larger = invalid). Else that layer runs after the upscaler. `aw` x `ah`: its active extent.
bool wilsjo2_before_ok(const Frame& f, uint32_t* aw, uint32_t* ah) {
    const D3D12_RESOURCE_DESC cd = f.color->GetDesc();
    const uint32_t cw = uint32_t(cd.Width), ch = cd.Height;
    *aw = f.subrect_w; *ah = f.subrect_h;
    if (!*aw && !*ah) { *aw = cw; *ah = ch; }
    const bool active_ok = cd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && cd.SampleDesc.Count == 1 &&
                           cd.DepthOrArraySize == 1 && cw >= 1 && ch >= 1 && cw <= 16384 && ch <= 16384 && *aw && *ah &&
                           *aw <= cw && *ah <= ch;
    return !f.color_x && !f.color_y && cd.MipLevels == 1 && active_ok;
}

thread_local int t_inside = 0;   // OptiScaler's own nested calls of the export pass straight through

NVSDK_NGX_Result NR_NGX_MSABI hk_evaluate(ID3D12GraphicsCommandList* cmd, const NVSDK_NGX_Handle* h,
                                          NVSDK_NGX_Parameter* p, void* callback) {
    if (t_inside || !cmd || !h || !p) return real_evaluate(cmd, h, p, callback);
    struct Inside { Inside() { ++t_inside; } ~Inside() { --t_inside; } } inside;
    nr::gpu::Scope recording(nr::asi::hudcap::d3d12_recording(cmd));
    nr::pe::watchdog::Phase phase("OptiScaler's D3D12 evaluate (NR)");
    {   // HUD FIX: DLSS-G's HUD-less frame (its evaluate through NGX), for the frame-generation method
        ID3D12Resource* hudless = nullptr;
        if (p->Get("DLSSG.HUDLess", &hudless) == NVSDK_NGX_Result_Success && hudless)
            nr::asi::hudcap::fg_note_dlssg(cmd, hudless);
    }

    std::unique_lock<std::mutex> guard(g_lock);
    tick_retired();
    Tracked& t = g_features[h->Id];
    ++t.evaluates;
    if (t.feature < 0) {
        // Not seen at create: one of OptiScaler's own FSR / FidelityFX features (its inlined CreateFeature:
        // SuperSampling), or a feature made before this module was loaded (by what the call carries)
        HMODULE caller = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(__builtin_return_address(0)), &caller);
        t.feature = caller == g_opti ? kFeatureSuperSampling
                                     : unseen_feature([p](const char* n) { return get_resource(p, n) != nullptr; });
        log("[nr] OptiScaler feature %u first seen at an evaluate from %s: taken as %s", h->Id,
            caller == g_opti ? "OptiScaler's own input" : "the game (its create not seen; by what the call carries)",
            feature_name(t.feature));
    }
    if (t.feature == kFeatureFrameGeneration) {
        // DLSS-G's declared rate for the Present layers (present_path.hpp ObservePresentFrameGeneration): NGX counts the
        // generated frames (1 = 2x); an absent key or paused rendering declares nothing
        if (ID3D12Resource* back = get_resource(p, "DLSSG.Backbuffer")) {
            const uint32_t generated = get_uint(p, "DLSSG.MultiFrameCount");
            if (generated >= 1 && generated < 8 && get_uint(p, "DLSSG.NotRenderingGameFrames") == 0) {
                const D3D12_RESOURCE_DESC d = back->GetDesc();
                g_present.declaration = {uint32_t(d.Width), d.Height, generated + 1, GetTickCount64(), g_present.presents};
            }
        }
        guard.unlock();
        return real_evaluate(cmd, h, p, callback);
    }
    if (t.feature != kFeatureSuperSampling && t.feature != kFeatureRayReconstruction) {
        guard.unlock();
        return real_evaluate(cmd, h, p, callback);
    }
    Frame f;
    if (!read_frame(p, t, f)) {
        if (!t.said) {
            t.said = true;
            log("[nr] OptiScaler feature %u: no colour, output, depth or motion vectors in its evaluate; NR does not run",
                h->Id);
        }
        guard.unlock();
        return real_evaluate(cmd, h, p, callback);
    }
    const std::shared_ptr<const Chain> chain = chain_now();
    match_layers(t, *chain);
    const States st = expected_states();
    ID3D12Device* device = nullptr;
    if (FAILED(cmd->GetDevice(IID_PPV_ARGS(&device))) || !device) {
        guard.unlock();
        return real_evaluate(cmd, h, p, callback);
    }
    struct Dev { ID3D12Device* d; ~Dev() { d->Release(); } } hold{device};
    menu_once(device);   // NR Control (above), if no swap chain of the game's was seen first
    ++g_fg_frames;   // a game frame (NR Control's frame-generation line)
    g_present.evaluate_ms = GetTickCount64();
    const bool renodx_only = std::all_of(chain->layers.begin(), chain->layers.end(), [](const Layer& l) {
        return l.cfg.processing == nr::pe::Processing::kRenoDX;
    });
    if (renodx_only && nr::pe::screenshot::wants_final() && !g_present.wanted.load()) {
        // NR moves to the final picture until the screenshot has its pair (late_present); layers at Present take
        // the pair there (PresentShot)
        late_capture(cmd, device, f, st);
        guard.unlock();
        return real_evaluate(cmd, h, p, callback);
    }
    const bool nr_on = nr::dlssnr::frame_keys();

    // The layers by placement, in layer order. Before the upscaler with Ray Reconstruction too (RenoDX does not; players
    // found NR before it fine). A wilsjo2 layer: before only where its own conditions allow, and
    // with an output subrect away from the origin not at all.
    uint32_t w2_aw = 0, w2_ah = 0;
    const bool w2_before_ok = wilsjo2_before_ok(f, &w2_aw, &w2_ah);
    std::vector<const Layer*> before, after;
    bool before_renodx = false;
    // Layers at Present run there (present_layers) and nowhere else (never before / after the upscaler in its
    // place - the picture there differs too much). Here only their state when Present does not serve them: no Present
    // of the game's has reached them for a second since they were wanted (its swap chain not found), or Present cannot
    // serve this swap chain for their processing.
    bool any_present = false;
    for (const Layer& l : chain->layers) any_present |= present_layer(l);
    const uint64_t now_ms = GetTickCount64();
    if (!any_present) g_present.wanted_since_ms = 0;
    else if (!g_present.wanted_since_ms) g_present.wanted_since_ms = now_ms;
    const uint64_t served = g_present.served_ms.load();
    const bool alive = !any_present || now_ms - g_present.wanted_since_ms < 1000 ||
                       (served >= g_present.wanted_since_ms && now_ms - served < 1000);
    const bool at_present = alive && !g_present.unusable.load(), at_present_w2 = alive && !g_present.unusable_w2.load();
    bool present_wanted = false, present_takes = false;   // Present layers at all; some run at Present now
    for (const Layer& l : chain->layers) {
        const bool wilsjo2 = l.cfg.processing == nr::pe::Processing::kWilsjo2;
        if (present_layer(l)) {
            present_wanted = true;
            if (wilsjo2 ? at_present_w2 : at_present) present_takes = true;
            else if (!alive) layer_note(l, 2, 3, "waits for a Present of the game's (its swap chain was not found)");
            else layer_note(l, 2, 3, "Present cannot serve this swap chain's colour format");
            continue;
        }
        if (wilsjo2 && (f.out_x || f.out_y)) {
            layer_note(l, 0, 3, "the output is a region away from the corner");
            continue;
        }
        const bool want_before = l.cfg.placement == nr::pe::Placement::kBefore;
        const bool b = want_before && (!wilsjo2 || w2_before_ok);
        (b ? before : after).push_back(&l);
        before_renodx |= b && !wilsjo2;
        const char* note = want_before && !b ? "after the upscaler: this frame does not allow before" : "";
        layer_note(l, b ? 1u : 0u, wilsjo2 && !nr_on ? 3u : 1u, wilsjo2 && !nr_on ? "NR is off (toggle key)" : note);
    }
    const nr::asi::Wilsjo2Frame wf = wilsjo2_frame(p, f, st);
    g_present.wanted = present_wanted;
    if (present_wanted) capture_guides(cmd, device, f, st, wf, h->Id);

    NVSDK_NGX_Result r;
    if (!before.empty()) {
        // The colour's region copied into a texture of our own, the layers on it in order, and that texture handed to
        // the upscaler in the colour's place (at base 0, in the colour's state); the game's colour is never written.
        // The region: the render region from the colour's base (RenoDX's), or for wilsjo2 layers alone their active
        // extent at origin 0.
        const uint32_t rx = before_renodx ? f.color_x : 0u, ry = before_renodx ? f.color_y : 0u;
        const uint32_t rw = before_renodx ? f.render_w : w2_aw, rh = before_renodx ? f.render_h : w2_ah;
        const DXGI_FORMAT format = f.color->GetDesc().Format;
        if (!ensure_texture(device, t.copy, t.copy_state, format, rw, rh)) {
            // the layers before the upscaler cannot run this frame: the upscaler and the layers after it still do
            // (0.0.7 ran its after-SR NR on its own)
            static bool said = false;
            if (!said) { said = true; log("[nr] the colour's copy could not be made; the layers before the upscaler skip such frames, the layers after it run"); }
            guard.unlock();
            r = real_evaluate(cmd, h, p, callback);
            goto after_layers;
        }
        transition(cmd, f.color, st.color, D3D12_RESOURCE_STATE_COPY_SOURCE);
        transition(cmd, t.copy, t.copy_state, D3D12_RESOURCE_STATE_COPY_DEST);
        copy_region(cmd, t.copy, 0, 0, f.color, rx, ry, rw, rh);
        transition(cmd, f.color, D3D12_RESOURCE_STATE_COPY_SOURCE, st.color);
        transition(cmd, t.copy, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        // [Inspect]: the final picture's comparison is here when nothing runs after the upscaler or at Present (the
        // comparison then goes through the upscaler); a chosen layer's wherever it runs
        size_t ia = 0, ib = 0;
        const bool inspect = inspect_span(chain->inspect, before, after.empty() && !present_takes, &ia, &ib) &&
                             inspect_format(format);
        const long held = chosen_index(chain->inspect, before);
        const Frame live_frame = f; const States live_states = st; const auto live_wf = wf;
        for (size_t k = 0; k < before.size(); ++k) {
            Frame f = live_frame; States st = live_states; auto wf = live_wf;
            const Layer* l = before[k];
            if (long(k) == held) hold_frame(cmd, device, chain->inspect, t.hold, t.copy, row_number(*l));
            if (long(k) == held && chain->inspect.hold) hold_inputs(cmd, device, t.hold, f, st, wf);
            if (inspect && k == ia) inspect_copy(cmd, device, t.inspect_off, t.inspect_off_state, t.copy);
            LayerState& ls = layer_state(t, *l);
            if (l->cfg.processing == nr::pe::Processing::kRenoDX) {
                run_renodx(cmd, ls, *l, 1, f, t.copy, rw, rh, st);
            } else if (nr_on) {
                if (!ls.w2[1]) ls.w2[1] = nr::asi::wilsjo2_owner();
                nr::pe::renodx_route::CallNetwork network(l->network);
                nr::pe::renodx_route::CallLayer number(row_number(*l));
                nr::asi::Wilsjo2Frame wr = wf;
                wr.reset = wr.reset || hold_released(row_number(*l));
                nr::asi::wilsjo2_run(cmd, device, *ls.w2[1], wr, l->w2, t.copy, rw, rh, true);
            }
            if (inspect && k == ib)
                inspect_show(cmd, device, chain->inspect, t.inspect_off, t.inspect_off_state, t.inspect_on,
                             t.inspect_on_state, t.copy);
        }
        transition(cmd, t.copy, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, st.color);
        t.copy_state = st.color;
        ID3D12Resource* copy = t.copy;
        guard.unlock();
        if (f.color_as_resource) p->Set(f.color_key, copy); else p->Set(f.color_key, static_cast<void*>(copy));
        const bool moved = rx || ry;
        if (moved) {
            p->Set("DLSS.Input.Color.Subrect.Base.X", 0u);
            p->Set("DLSS.Input.Color.Subrect.Base.Y", 0u);
        }
        r = real_evaluate(cmd, h, p, callback);
        if (f.color_as_resource) p->Set(f.color_key, f.color); else p->Set(f.color_key, static_cast<void*>(f.color));
        if (moved) {
            p->Set("DLSS.Input.Color.Subrect.Base.X", f.color_x);
            p->Set("DLSS.Input.Color.Subrect.Base.Y", f.color_y);
        }
    } else {
        guard.unlock();
        r = real_evaluate(cmd, h, p, callback);
    }
after_layers:
    if (r != NVSDK_NGX_Result_Success) return r;
    if (after.empty()) return r;

    // After the upscaler: the layers on its output, in place, on the same command list.
    guard.lock();
    const auto it = g_features.find(h->Id);   // released meanwhile on another thread: nothing to run on
    if (it == g_features.end()) return r;
    Tracked& t2 = it->second;
    match_layers(t2, *chain);
    const D3D12_RESOURCE_DESC od = f.output->GetDesc();
    const bool whole = !f.out_x && !f.out_y && od.Width == f.out_w && od.Height == f.out_h;
    transition(cmd, f.output, st.output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    // [Inspect]: the layers compared here, the output taken before the first of them and shown after the last
    size_t ia = 0, ib = 0;
    const bool inspect = inspect_span(chain->inspect, after, !present_takes, &ia, &ib) &&
                         inspect_format(od.Format);
    const long held = chosen_index(chain->inspect, after);
    const Frame live_frame = f; const States live_states = st; const auto live_wf = wf;
    for (size_t k = 0; k < after.size(); ++k) {
            Frame f = live_frame; States st = live_states; auto wf = live_wf;
        const Layer* l = after[k];
        if (long(k) == held) hold_frame(cmd, device, chain->inspect, t2.hold, f.output, row_number(*l));
            if (long(k) == held && chain->inspect.hold) hold_inputs(cmd, device, t2.hold, f, st, wf);
        if (inspect && k == ia) inspect_copy(cmd, device, t2.inspect_off, t2.inspect_off_state, f.output);
        [&] {
        LayerState& ls = layer_state(t2, *l);
        if (l->cfg.processing == nr::pe::Processing::kWilsjo2) {
            // on the whole output resource (the native extent)
            if (!nr_on) return;
            if (!ls.w2[0]) ls.w2[0] = nr::asi::wilsjo2_owner();
            nr::pe::renodx_route::CallNetwork network(l->network);
            nr::pe::renodx_route::CallLayer number(row_number(*l));
            nr::asi::Wilsjo2Frame wr = wf;
            wr.reset = wr.reset || hold_released(row_number(*l));
            // the processing reads and writes the output where it is; an output it cannot (no unordered access, or
            // denied as a shader resource) goes through a copy of our own and back, as 0.0.7's fork did with its own
            // intermediate
            const bool in_place = (od.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) &&
                                  !(od.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE);
            if (in_place) {
                nr::asi::wilsjo2_run(cmd, device, *ls.w2[0], wr, l->w2, f.output, uint32_t(od.Width), od.Height, false);
            } else if (ensure_texture(device, t2.out_copy, t2.out_copy_state, od.Format, uint32_t(od.Width), od.Height)) {
                static bool said = false;
                if (!said) { said = true; log("[nr] wilsjo2 after the upscaler: the output cannot be read and written in place (flags 0x%X); through a copy", unsigned(od.Flags)); }
                transition(cmd, f.output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
                transition(cmd, t2.out_copy, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
                copy_region(cmd, t2.out_copy, 0, 0, f.output, 0, 0, uint32_t(od.Width), od.Height);
                transition(cmd, t2.out_copy, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                nr::asi::wilsjo2_run(cmd, device, *ls.w2[0], wr, l->w2, t2.out_copy, uint32_t(od.Width), od.Height, false);
                transition(cmd, t2.out_copy, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
                transition(cmd, f.output, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
                copy_region(cmd, f.output, 0, 0, t2.out_copy, 0, 0, uint32_t(od.Width), od.Height);
                transition(cmd, f.output, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                transition(cmd, t2.out_copy, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            }
        } else if (whole) {
            run_renodx(cmd, ls, *l, 0, f, f.output, f.out_w, f.out_h, st);
        } else if (ensure_texture(device, t2.out_copy, t2.out_copy_state, od.Format, f.out_w, f.out_h)) {
            // An output subrect: its region out, NR, and back
            transition(cmd, f.output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            transition(cmd, t2.out_copy, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
            copy_region(cmd, t2.out_copy, 0, 0, f.output, f.out_x, f.out_y, f.out_w, f.out_h);
            transition(cmd, t2.out_copy, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            run_renodx(cmd, ls, *l, 0, f, t2.out_copy, f.out_w, f.out_h, st);
            transition(cmd, t2.out_copy, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            transition(cmd, f.output, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            copy_region(cmd, f.output, f.out_x, f.out_y, t2.out_copy, 0, 0, f.out_w, f.out_h);
            transition(cmd, f.output, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            transition(cmd, t2.out_copy, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        }();
        if (inspect && k == ib)
            inspect_show(cmd, device, chain->inspect, t2.inspect_off, t2.inspect_off_state, t2.inspect_on,
                         t2.inspect_on_state, f.output);
    }
    transition(cmd, f.output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, st.output);
    return r;
}

// ---- the game's Vulkan devices: the int4 network's extensions, and what a Vulkan game's NR needs to know -----------
// winevulkan's vkGetInstanceProcAddr wrapped: the game's (or vkd3d-proton's / DXVK's) vkCreateDevice goes through
// hk_create_device, which adds VK_KHR_pipeline_binary and VK_KHR_maintenance5 for the int4 network (DLSSNR_INT4 not 0)
// and records every device made (its instance, physical device and queue families) for the Vulkan route below.
struct SeenDevice {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    std::vector<uint32_t> families;
    std::vector<uint32_t> counts;   // queues made of each family (as `families`)
    bool present_fences = false;
    bool translator = false;   // made by vkd3d-proton or DXVK (a D3D device's, e.g. OptiScaler's own D3D12 one)
};
// Whether `address` (a vkCreateDevice caller) is in vkd3d-proton's or DXVK's modules.
bool in_translator(void* address) {
    HMODULE m = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(address), &m) || !m)
        return false;
    for (const wchar_t* name : {L"d3d12core.dll", L"d3d12.dll", L"d3d11.dll", L"dxgi.dll", L"d3d10core.dll", L"d3d9.dll"})
        if (HMODULE t = GetModuleHandleW(name); t && t == m) return true;
    return false;
}
std::mutex g_devices_lock;
std::vector<SeenDevice> g_devices;                       // the devices made, oldest first
// The device NGX's Vulkan init was given (OptiScaler's NVSDK_NGX_VULKAN_Init forms): the device of a game
// whose vkCreateDevice ran before this module was loaded (0.0.7 took its device from there) - g_devices_lock
SeenDevice g_ngx_vk;
// NR_TEST_DEVICE_UNSEEN=1 (tests): the game's vkCreateDevice taken as not seen - the route's device and the present
// queue's family looked up as for a device made before this module was loaded
bool test_device_unseen() {
    static const bool on = [] { const char* e = std::getenv("NR_TEST_DEVICE_UNSEEN"); return e && e[0] == '1'; }();
    return on;
}
// NGX's Vulkan init: the instance, physical device and device the game gives NGX, kept for a device whose creation was
// not seen (the first five / seven parameters are the same in every SDK's form; the rest passed through as they are).
using PFN_VkInit = NVSDK_NGX_Result(NR_NGX_MSABI*)(unsigned long long, const wchar_t*, VkInstance, VkPhysicalDevice, VkDevice,
                                                   uintptr_t, uintptr_t, uintptr_t, uintptr_t);
using PFN_VkInitProject = NVSDK_NGX_Result(NR_NGX_MSABI*)(const char*, int, const char*, const wchar_t*, VkInstance,
                                                          VkPhysicalDevice, VkDevice, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
// [0] NVSDK_NGX_VULKAN_Init, [1] _Init_Ext, [2] _Init_Ext2 (where OptiScaler's other forms end up); project forms:
// [0] _Init_ProjectID, [1] _Init_ProjectID_Ext. (10-27: the hook named "_Init_with_ProjectID", an export no NGX has -
// the chaos / fallback gate NR_TEST_DEVICE_UNSEEN found NR not running.)
PFN_VkInit real_vk_init[3] = {};
PFN_VkInitProject real_vk_init_project[2] = {};
void note_ngx_vk(VkInstance instance, VkPhysicalDevice physical, VkDevice device) {
    if (!instance || !physical || !device) return;
    std::lock_guard<std::mutex> guard(g_devices_lock);
    g_ngx_vk.instance = instance; g_ngx_vk.physical = physical; g_ngx_vk.device = device;
}
template <int K>
NVSDK_NGX_Result NR_NGX_MSABI hk_vk_init(unsigned long long app, const wchar_t* path, VkInstance instance, VkPhysicalDevice physical,
                                        VkDevice device, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d) {
    note_ngx_vk(instance, physical, device);
    return real_vk_init[K](app, path, instance, physical, device, a, b, c, d);
}
template <int K>
NVSDK_NGX_Result NR_NGX_MSABI hk_vk_init_project(const char* project, int engine, const char* version, const wchar_t* path,
                                                VkInstance instance, VkPhysicalDevice physical, VkDevice device, uintptr_t a,
                                                uintptr_t b, uintptr_t c, uintptr_t d) {
    g_ngx_engine.store(engine);
    note_ngx_vk(instance, physical, device);
    return real_vk_init_project[K](project, engine, version, path, instance, physical, device, a, b, c, d);
}

std::atomic<VkInstance> g_create_instance{VK_NULL_HANDLE};   // the instance vkCreateDevice was last asked of
PFN_vkGetInstanceProcAddr real_gipa = nullptr;
std::atomic<PFN_vkCreateDevice> g_next_create_device{nullptr};
PFN_vkEnumerateDeviceExtensionProperties g_enumerate = nullptr;
PFN_vkGetPhysicalDeviceFeatures2 g_features2 = nullptr;

bool int4_wanted() {
    const char* e = std::getenv("DLSSNR_INT4");
    return !(e && e[0] == '0' && !e[1]);
}

// A native Vulkan game calls winevulkan.dll's exported vkCreateDevice (through vulkan-1.dll) rather than asking
// vkGetInstanceProcAddr: that export is detoured too, its instance found from the physical device
// (vkEnumeratePhysicalDevices' export detoured, which instance listed it). A call that came through the wrapped
// pointer and reaches the export goes straight on (t_in_create).
PFN_vkCreateDevice real_export_create = nullptr;
PFN_vkEnumeratePhysicalDevices real_enumerate_physical = nullptr;
std::unordered_map<VkPhysicalDevice, VkInstance> g_physical_instance;   // g_devices_lock
thread_local int t_in_create = 0;

VkResult create_device(PFN_vkCreateDevice create, VkInstance instance, VkPhysicalDevice phys, const VkDeviceCreateInfo* ci,
                       const VkAllocationCallbacks* a, VkDevice* out, void* caller) {
    nr::pe::watchdog::Phase phase("the game's vkCreateDevice (int4 extensions)");
    struct Inside { Inside() { ++t_in_create; } ~Inside() { --t_in_create; } } inside;
    VkDeviceCreateInfo patched = *ci;
    std::unique_ptr<nr::DeviceFeatures> features;
    std::vector<const char*> ext;
    bool added = false;
    if (int4_wanted() && g_enumerate && g_features2) try {
        uint32_t n = 0;
        g_enumerate(phys, nullptr, &n, nullptr);
        std::vector<VkExtensionProperties> have(n);
        g_enumerate(phys, nullptr, &n, have.data());
        auto offered = [&](const char* e) {
            for (auto& x : have) if (!std::strcmp(x.extensionName, e)) return true;
            return false;
        };
        VkPhysicalDeviceMaintenance5FeaturesKHR m5{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR};
        VkPhysicalDevicePipelineBinaryFeaturesKHR pb{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_BINARY_FEATURES_KHR, &m5};
        VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &pb};
        if (offered(VK_KHR_PIPELINE_BINARY_EXTENSION_NAME) && offered(VK_KHR_MAINTENANCE_5_EXTENSION_NAME)) {
            g_features2(phys, &f2);
            if (pb.pipelineBinaries && m5.maintenance5) {
                features = std::make_unique<nr::DeviceFeatures>(ci->pNext);
                features->get<VkPhysicalDevicePipelineBinaryFeaturesKHR>(
                    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_BINARY_FEATURES_KHR)->pipelineBinaries = VK_TRUE;
                if (auto* v14 = features->find<VkPhysicalDeviceVulkan14Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES))
                    v14->maintenance5 = VK_TRUE;
                else
                    features->get<VkPhysicalDeviceMaintenance5FeaturesKHR>(
                        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR)->maintenance5 = VK_TRUE;
                for (uint32_t i = 0; i < ci->enabledExtensionCount; ++i) ext.push_back(ci->ppEnabledExtensionNames[i]);
                for (const char* e : {VK_KHR_PIPELINE_BINARY_EXTENSION_NAME, VK_KHR_MAINTENANCE_5_EXTENSION_NAME}) {
                    bool on = false;
                    for (auto x : ext) on |= !std::strcmp(x, e);
                    if (!on) ext.push_back(e);
                }
                patched.pNext = features->head;
                patched.enabledExtensionCount = uint32_t(ext.size());
                patched.ppEnabledExtensionNames = ext.data();
                added = true;
            }
        }
    } catch (...) {
        patched = *ci;
        added = false;
    }
    log("[nr] the game's vkCreateDevice: %s (asking)", added ? "VK_KHR_pipeline_binary and VK_KHR_maintenance5 added for the "
                                                                "int4 network" : "left as it is (the int4 network's extensions not offered)");
    VkResult r = create(phys, &patched, a, out);
    if (r != VK_SUCCESS && added) {   // the device as the game asked for it, rather than no device at all
        log("[nr] the game's vkCreateDevice with the int4 extensions returned %d; made again without them", int(r));
        patched = *ci;
        r = create(phys, ci, a, out);
    }
    log("[nr] the game's vkCreateDevice returned %d", int(r));
    if (r == VK_SUCCESS && out && *out) {
        SeenDevice d;
        d.physical = phys;
        d.device = *out;
        d.translator = in_translator(caller);
        bool maintenance = false;
        for (uint32_t i = 0; i < patched.enabledExtensionCount; ++i)
            maintenance |= !std::strcmp(patched.ppEnabledExtensionNames[i], VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME) ||
                           !std::strcmp(patched.ppEnabledExtensionNames[i], VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME);
        for (auto* p = static_cast<const VkBaseInStructure*>(patched.pNext); p; p = p->pNext)
            if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT)
                d.present_fences = maintenance && reinterpret_cast<const VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT*>(p)->swapchainMaintenance1;

        for (uint32_t i = 0; i < ci->queueCreateInfoCount; ++i) {
            d.families.push_back(ci->pQueueCreateInfos[i].queueFamilyIndex);
            d.counts.push_back(ci->pQueueCreateInfos[i].queueCount);
        }
        nr::asi::hudcap::vk_device(*out, d.translator);   // HUD FIX's capture: its creations followed (not a translator's)
        std::lock_guard<std::mutex> guard(g_devices_lock);
        // the instance that listed this physical device (vkEnumeratePhysicalDevices), else the one the wrapped
        // vkGetInstanceProcAddr was last asked of
        const auto it = g_physical_instance.find(phys);
        d.instance = it != g_physical_instance.end() ? it->second : instance;
        g_devices.push_back(std::move(d));
    }
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL hk_create_device(VkPhysicalDevice phys, const VkDeviceCreateInfo* ci,
                                                const VkAllocationCallbacks* a, VkDevice* out) {
    return create_device(g_next_create_device.load(), g_create_instance.load(), phys, ci, a, out, __builtin_return_address(0));
}
VKAPI_ATTR VkResult VKAPI_CALL hk_export_create_device(VkPhysicalDevice phys, const VkDeviceCreateInfo* ci,
                                                       const VkAllocationCallbacks* a, VkDevice* out) {
    if (t_in_create) return real_export_create(phys, ci, a, out);
    return create_device(real_export_create, VK_NULL_HANDLE, phys, ci, a, out, __builtin_return_address(0));
}
// The game's vkDestroyDevice (winevulkan's export, detoured beside vkCreateDevice): the device leaves the record, and
// when NR's Vulkan route runs on it everything of ours on it goes first, while it is still there (vk_route_teardown).
void vk_route_teardown(VkDevice device);
void end_receipts(VkDevice device);
PFN_vkDestroyDevice real_destroy_device = nullptr;
bool g_destroy_hooked = false;
void forget_queues(VkDevice device);   // the game's queue records (below, with their hooks)
VKAPI_ATTR void VKAPI_CALL hk_destroy_device(VkDevice device, const VkAllocationCallbacks* a) {
    if (device) {
        nr::pe::menu::vulkan_forget(device);
        vk_route_teardown(device);
        forget_queues(device);
        nr::pe::vkdevice::forget_device(device);
        std::lock_guard<std::mutex> guard(g_devices_lock);
        g_devices.erase(std::remove_if(g_devices.begin(), g_devices.end(), [device](const SeenDevice& d) { return d.device == device; }),
                        g_devices.end());
    }
    end_receipts(device);
    real_destroy_device(device, a);
}

VKAPI_ATTR VkResult VKAPI_CALL hk_enumerate_physical(VkInstance instance, uint32_t* n, VkPhysicalDevice* list) {
    const VkResult r = real_enumerate_physical(instance, n, list);
    if ((r == VK_SUCCESS || r == VK_INCOMPLETE) && list && n) {
        std::lock_guard<std::mutex> guard(g_devices_lock);
        for (uint32_t i = 0; i < *n; ++i) g_physical_instance[list[i]] = instance;
    }
    return r;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL hk_gipa(VkInstance instance, const char* name) {
    const PFN_vkVoidFunction f = real_gipa(instance, name);
    if (f && name && !std::strcmp(name, "vkCreateDevice")) {
        g_next_create_device.store(reinterpret_cast<PFN_vkCreateDevice>(f));
        if (instance) g_create_instance.store(instance);
        return reinterpret_cast<PFN_vkVoidFunction>(&hk_create_device);
    }
    return f;
}

// The queues the game takes (winevulkan's vkGetDeviceQueue / vkGetDeviceQueue2, which its vkGetDeviceProcAddr hands
// out too): device, queue -> family, so a present queue's family is known for a device whose vkCreateDevice ran before
// this module was loaded. A device's records go with it (hk_destroy_device: a handle may be made again).
struct QueueRecord { VkDevice device; VkQueue queue; uint32_t family; };
std::mutex g_queue_records_lock;
std::vector<QueueRecord> g_queue_records;
PFN_vkGetDeviceQueue real_get_queue = nullptr;
PFN_vkGetDeviceQueue2 real_get_queue2 = nullptr;
void record_queue(VkDevice device, VkQueue queue, uint32_t family) {
    if (!device || !queue) return;
    std::lock_guard<std::mutex> guard(g_queue_records_lock);
    for (QueueRecord& r : g_queue_records)
        if (r.device == device && r.queue == queue) { r.family = family; return; }
    g_queue_records.push_back({device, queue, family});
}
bool recorded_family(VkDevice device, VkQueue queue, uint32_t* family) {
    std::lock_guard<std::mutex> guard(g_queue_records_lock);
    for (const QueueRecord& r : g_queue_records)
        if (r.device == device && r.queue == queue) { *family = r.family; return true; }
    return false;
}
void forget_queues(VkDevice device) {
    std::lock_guard<std::mutex> guard(g_queue_records_lock);
    g_queue_records.erase(std::remove_if(g_queue_records.begin(), g_queue_records.end(),
                                         [device](const QueueRecord& r) { return r.device == device; }),
                          g_queue_records.end());
}
VKAPI_ATTR void VKAPI_CALL hk_get_queue(VkDevice device, uint32_t family, uint32_t index, VkQueue* out) {
    real_get_queue(device, family, index, out);
    if (out) record_queue(device, *out, family);
}
VKAPI_ATTR void VKAPI_CALL hk_get_queue2(VkDevice device, const VkDeviceQueueInfo2* info, VkQueue* out) {
    real_get_queue2(device, info, out);
    if (out && info) record_queue(device, *out, info->queueFamilyIndex);
}

void hook_device_creation() {
    if (!int4_wanted()) log("[nr] DLSSNR_INT4=0: the game's Vulkan device is left as it is (no int4 extensions)");
    HMODULE m = GetModuleHandleW(L"winevulkan.dll");
    if (!m) m = LoadLibraryW(L"winevulkan.dll");
    if (!m) {
        log("[nr] no winevulkan.dll: the int4 network's device extensions are not added");
        return;
    }
    g_enumerate = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
        reinterpret_cast<void*>(GetProcAddress(m, "vkEnumerateDeviceExtensionProperties")));
    g_features2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
        reinterpret_cast<void*>(GetProcAddress(m, "vkGetPhysicalDeviceFeatures2")));
    void* gipa = reinterpret_cast<void*>(GetProcAddress(m, "vkGetInstanceProcAddr"));
    if (!gipa || MH_CreateHook(gipa, reinterpret_cast<void*>(&hk_gipa), reinterpret_cast<void**>(&real_gipa)) != MH_OK ||
        MH_EnableHook(gipa) != MH_OK)
        log("[nr] winevulkan.dll's vkGetInstanceProcAddr could not be hooked: the int4 network's device extensions are "
            "not added");
    void* create = reinterpret_cast<void*>(GetProcAddress(m, "vkCreateDevice"));
    void* enumerate = reinterpret_cast<void*>(GetProcAddress(m, "vkEnumeratePhysicalDevices"));
    void* destroy = reinterpret_cast<void*>(GetProcAddress(m, "vkDestroyDevice"));
    g_destroy_hooked = destroy && MH_CreateHook(destroy, reinterpret_cast<void*>(&hk_destroy_device),
                                                reinterpret_cast<void**>(&real_destroy_device)) == MH_OK &&
                       MH_EnableHook(destroy) == MH_OK;
    if (!g_destroy_hooked)
        log("[nr] winevulkan.dll's vkDestroyDevice could not be hooked: NR does not run on Vulkan games (its objects "
            "would outlive the game's device)");
    if (!create || !enumerate ||
        MH_CreateHook(enumerate, reinterpret_cast<void*>(&hk_enumerate_physical),
                      reinterpret_cast<void**>(&real_enumerate_physical)) != MH_OK ||
        MH_EnableHook(enumerate) != MH_OK ||
        MH_CreateHook(create, reinterpret_cast<void*>(&hk_export_create_device), reinterpret_cast<void**>(&real_export_create)) != MH_OK ||
        MH_EnableHook(create) != MH_OK)
        log("[nr] winevulkan.dll's exported vkCreateDevice could not be hooked: a Vulkan game calling it directly gets no "
            "int4 extensions and no NR");
    // the game's queues (optional: without them a present queue the device's creation did not show is looked up
    // as before)
    for (auto [name, detour, real] : {std::tuple{"vkGetDeviceQueue", reinterpret_cast<void*>(&hk_get_queue), reinterpret_cast<void**>(&real_get_queue)},
                                      std::tuple{"vkGetDeviceQueue2", reinterpret_cast<void*>(&hk_get_queue2), reinterpret_cast<void**>(&real_get_queue2)}}) {
        void* fn = reinterpret_cast<void*>(GetProcAddress(m, name));
        if (!fn || MH_CreateHook(fn, detour, real) != MH_OK || MH_EnableHook(fn) != MH_OK)
            log("[nr] winevulkan.dll's %s could not be hooked: the game's queues are known from its vkCreateDevice only", name);
    }
}

// ---- Vulkan games --------------------------------------------------------------------------------------------------
// OptiScaler's Vulkan inputs (FSR2 / FidelityFX API / XeSS on Vulkan, and the game's own DLSS) all reach the exported
// NVSDK_NGX_VULKAN_EvaluateFeature (inputs/*_Vk.cpp, OptiScaler c4668477), as the D3D12 ones reach the D3D12 export:
// detoured the same way, NR recorded into the game's command buffer through the model's Vulkan session
// (nr_dlssnr_model.cpp evaluate_vk_asi), as the 0.0.7 route's forked OptiScaler ran it in IFeature_Vk. The device and
// the queue families it was made with come from the game's vkCreateDevice (hk_create_device above); the network's
// builds submit onto the game's queue, so from the first Vulkan evaluate on the game's queue calls take the same
// per-queue lock as ours (nr_pe_vkdevice.hpp enable_queue_locks). During the call the inputs are in
// SHADER_READ_ONLY_OPTIMAL and the output in GENERAL (NGX's Vulkan contract, how OptiScaler's upscalers take them).
// Present layers run at the game's vkQueuePresentKHR (the Vulkan Present section below), never here. The wilsjo2 style
// runs its own Vulkan flow (nr_asi_wilsjo2_vk.cpp). NR Control is the Vulkan layer's menu, not drawn here.
using PFN_VkCreate1 = NVSDK_NGX_Result(NR_NGX_MSABI*)(VkDevice, VkCommandBuffer, NVSDK_NGX_Feature, NVSDK_NGX_Parameter*,
                                                     NVSDK_NGX_Handle**);
using PFN_VkCreate = NVSDK_NGX_Result(NR_NGX_MSABI*)(VkCommandBuffer, NVSDK_NGX_Feature, NVSDK_NGX_Parameter*,
                                                    NVSDK_NGX_Handle**);
using PFN_VkEvaluate = NVSDK_NGX_Result(NR_NGX_MSABI*)(VkCommandBuffer, const NVSDK_NGX_Handle*, NVSDK_NGX_Parameter*, void*);
PFN_VkCreate1 real_vk_create1 = nullptr;
PFN_VkCreate real_vk_create = nullptr;
PFN_VkEvaluate real_vk_evaluate = nullptr;
PFN_Release real_vk_release = nullptr;

// Our own image on the game's device (the colour NR runs on before the upscaler, an output subrect's region), with
// the view and the NGX description OptiScaler's upscaler takes it through.
struct OwnImage {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0, height = 0;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    NVSDK_NGX_Resource_VK ngx{};
    uint32_t shared[2] = {~0u, ~0u};   // a CONCURRENT image's two queue families (ensure_xfer_image)
};
struct RetiredImage { uint64_t at; ULONGLONG ms; OwnImage im; uint64_t ring; nr::gpu::Uses uses; };
// As release_resource: 64 more Vulkan evaluates AND 2 s, AND every submission of our Present ring made before it went
// done (its fences: the proof for what we submit ourselves; time stays the rule for the game's command buffers) (g_lock)
std::vector<RetiredImage> g_retired_images;
uint64_t vk_ring_submits();                      // the Present ring's submissions so far (below)
bool vk_ring_done_through(uint64_t submission);  // every one of them up to `submission` has completed
uint64_t g_vk_clock = 0;

struct VkRoute {
    bool tried = false, ready = false;
    ULONGLONG retry_ms = 0;   // the session could not start: tried again from then (0: not to be retried)
    SeenDevice dev;
    VkPhysicalDeviceMemoryProperties memory{};
} g_vk;   // g_lock
bool g_vk_is_dxvk = false;   // the session's device is DXVK's (a D3D11 game, the D3D11 section below)

void destroy_image(OwnImage& im) {
    if (im.view) vkDestroyImageView(g_vk.dev.device, im.view, nullptr);
    if (im.image) vkDestroyImage(g_vk.dev.device, im.image, nullptr);
    if (im.memory) vkFreeMemory(g_vk.dev.device, im.memory, nullptr);
    im = OwnImage{};
}
void retire_image(OwnImage& im) {
    if (im.image) g_retired_images.push_back({g_vk_clock + 64, GetTickCount64() + 2000, im, vk_ring_submits(), g_native_uses});
    im = OwnImage{};
}
void tick_vk_retired() {
    g_native_uses.touch();
    ++g_vk_clock;
    if (g_retired_images.empty()) return;
    const ULONGLONG now = GetTickCount64();
    for (size_t i = g_retired_images.size(); i-- > 0;)
        if (g_retired_images[i].at <= g_vk_clock && g_retired_images[i].ms <= now &&
            vk_ring_done_through(g_retired_images[i].ring) && g_retired_images[i].uses.ready()) {
            destroy_image(g_retired_images[i].im);
            g_retired_images.erase(g_retired_images.begin() + long(i));
        }
}

bool depth_format(VkFormat f) {
    switch (f) {
        case VK_FORMAT_D16_UNORM: case VK_FORMAT_X8_D24_UNORM_PACK32: case VK_FORMAT_D32_SFLOAT:
        case VK_FORMAT_D16_UNORM_S8_UINT: case VK_FORMAT_D24_UNORM_S8_UINT: case VK_FORMAT_D32_SFLOAT_S8_UINT:
            return true;
        default:
            return false;
    }
}
// `im` at this format and extent (made again otherwise; its contents are not kept), storage + sampled + transfer.
// `guide`: a copy of a game's guide (depth or motion) - sampled + transfer only, a depth format's view of its depth.
bool ensure_image(OwnImage& im, VkFormat format, uint32_t w, uint32_t h, bool guide = false) {
    if (im.image && im.format == format && im.width == w && im.height == h) return true;
    retire_image(im);
    const VkDevice dev = g_vk.dev.device;
    VkFormatProperties fp{};
    vkGetPhysicalDeviceFormatProperties(g_vk.dev.physical, format, &fp);
    const VkFormatFeatureFlags need = (guide ? 0u : VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                      VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    const VkImageAspectFlags aspect = guide && depth_format(format) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    if ((fp.optimalTilingFeatures & need) != need) {
        static VkFormat said = VK_FORMAT_UNDEFINED;
        if (said != format) {
            said = format;
            log("[nr] Vulkan route: format %d cannot be written by a compute shader here; NR does not run on it", int(format));
        }
        return false;
    }
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D; ii.format = format; ii.extent = {w, h, 1}; ii.mipLevels = 1; ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = (guide ? 0u : VK_IMAGE_USAGE_STORAGE_BIT) | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
               VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    OwnImage n;
    if (vkCreateImage(dev, &ii, nullptr, &n.image) != VK_SUCCESS) return false;
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(dev, n.image, &req);
    uint32_t type = ~0u;
    for (uint32_t i = 0; i < g_vk.memory.memoryTypeCount; ++i)
        if ((req.memoryTypeBits & (1u << i)) && (g_vk.memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            type = i;
            break;
        }
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size; ai.memoryTypeIndex = type;
    {
        nr::vram::Scope tag(nr::vram::kCopies);
        if (type == ~0u || test_fault("image_alloc") || vkAllocateMemory(dev, &ai, nullptr, &n.memory) != VK_SUCCESS) {
            destroy_image(n);
            return false;
        }
    }
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = n.image; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = format;
    vi.subresourceRange = {aspect, 0, 1, 0, 1};
    if (vkBindImageMemory(dev, n.image, n.memory, 0) != VK_SUCCESS || vkCreateImageView(dev, &vi, nullptr, &n.view) != VK_SUCCESS) {
        destroy_image(n);
        return false;
    }
    n.format = format; n.width = w; n.height = h;
    n.ngx.Type = NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW;
    n.ngx.ReadWrite = !guide;
    auto& info = n.ngx.Resource.ImageViewInfo;
    info.ImageView = reinterpret_cast<void*>(n.view);
    info.Image = reinterpret_cast<void*>(n.image);
    info.SubresourceRange.aspectMask = aspect;
    info.SubresourceRange.levelCount = 1;
    info.SubresourceRange.layerCount = 1;
    info.Format = uint32_t(format);
    info.Width = w; info.Height = h;
    im = n;
    return true;
}

void vk_barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to, bool depth = false) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = from; b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VkImageAspectFlags(depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT), 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
}

void vk_copy(VkCommandBuffer cmd, VkImage src, uint32_t sx, uint32_t sy, VkImage dst, uint32_t dx, uint32_t dy, uint32_t w,
             uint32_t h) {
    VkImageCopy c{};
    c.srcSubresource = c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.srcOffset = {int32_t(sx), int32_t(sy), 0};
    c.dstOffset = {int32_t(dx), int32_t(dy), 0};
    c.extent = {w, h, 1};
    vkCmdCopyImage(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
}

// ---- the game's images where a transfer copy or a view of ours cannot take them -------------------------------------
// 0.0.7's Vulkan NR read the game's images through the views the call hands (their mip level, array layer and channel
// order) with compute copies (sampled use only). Here a transfer copy and our own views (mip 0, layer 0) serve where the
// image allows them, and our copy program (wilsjo2_copy.comp) through the game's view where it does not.
// A view a transfer copy and a view of ours take as it is: mip 0, layer 0, channels in their own order (a view whose
// vkCreateImageView was not seen: taken as in order).
bool plain_view(const NVSDK_NGX_Resource_VK* r) {
    const auto& v = r->Resource.ImageViewInfo;
    return !v.SubresourceRange.baseMipLevel && !v.SubresourceRange.baseArrayLayer &&
           !nr::asi::hudcap::vk_view_reordered(reinterpret_cast<VkImageView>(v.ImageView));
}
// The image's usage from its vkCreateImage (hudcap), else `given` (DXVK's on D3D11; 0: not known).
VkImageUsageFlags seen_usage(const NVSDK_NGX_Resource_VK* r, VkImageUsageFlags given = 0) {
    // NR_TEST_USAGE_UNKNOWN=1 (tests): as if no vkCreateImage had been seen
    static const bool unknown = [] { const char* e = std::getenv("NR_TEST_USAGE_UNKNOWN"); return e && e[0] == '1'; }();
    if (unknown) return given;
    VkImageUsageFlags u = 0;
    if (r && nr::asi::hudcap::vk_image_usage(reinterpret_cast<VkImage>(r->Resource.ImageViewInfo.Image), &u)) return u;
    return given;
}
// `r` used where it is for every use in `need`: a plain view, and its usage allows them. Usage not known (its
// vkCreateImage not seen): only what NGX's contract promises every resource - sampled use (the upscaler's output's
// storage use is the caller's to add) - so a transfer copy is never made on an image that may not allow it (0.0.7 read
// the game's images by compute copies only).
bool usable_in_place(const NVSDK_NGX_Resource_VK* r, VkImageUsageFlags usage, VkImageUsageFlags need) {
    return plain_view(r) && (usage ? (usage & need) == need : (VK_IMAGE_USAGE_SAMPLED_BIT & need) == need);
}
VkImageAspectFlags layout_aspect(VkFormat f) {   // a barrier's aspects (a depth-stencil format: both)
    if (!depth_format(f)) return VK_IMAGE_ASPECT_COLOR_BIT;
    const bool stencil = f == VK_FORMAT_D16_UNORM_S8_UINT || f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT;
    return VK_IMAGE_ASPECT_DEPTH_BIT | (stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0u);
}
void vk_barrier_at(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to, VkImageAspectFlags aspect,
                   uint32_t mip, uint32_t layer) {
    if (from == to) return;
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = from; b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {aspect, mip, 1, layer, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
}
nr::asi::VkCopySide copy_side(const OwnImage& im, VkImageLayout layout) {
    nr::asi::VkCopySide c;
    c.image = im.image; c.format = im.format; c.width = im.width; c.height = im.height; c.layout = layout;
    return c;
}
nr::asi::VkCopySide copy_side(const NVSDK_NGX_Resource_VK* r, VkImageLayout layout, bool through_view) {
    const auto& v = r->Resource.ImageViewInfo;
    nr::asi::VkCopySide c;
    c.image = reinterpret_cast<VkImage>(v.Image);
    c.view = through_view ? reinterpret_cast<VkImageView>(v.ImageView) : VK_NULL_HANDLE;
    c.format = VkFormat(v.Format); c.width = v.Width; c.height = v.Height; c.layout = layout;
    c.mip = v.SubresourceRange.baseMipLevel; c.layer = v.SubresourceRange.baseArrayLayer;
    return c;
}
// The region (sx, sy, w x h) of the game's image `r` (in `layout`, left in it; `usage` as seen_usage gives it) into
// `dst` at its origin, `dst` made for it and left in `dst_end`. A transfer copy where `r` allows one; else our copy
// program through the game's view (sampled, as NGX's inputs are), or as a storage image when the image has storage use
// and no sampled use (an upscaler's output) - into a 32-bit float image for a depth format. `wide`: a format our program
// cannot write is taken into 32-bit float RGBA (a guide's copy; not the colour or the output, kept in their format).
bool copy_in(VkCommandBuffer cmd, const NVSDK_NGX_Resource_VK* r, VkImageLayout layout, VkImageUsageFlags usage,
             uint32_t sx, uint32_t sy, uint32_t w, uint32_t h, OwnImage& dst, VkImageLayout dst_end, bool wide = false,
             bool written = false) {
    const auto& v = r->Resource.ImageViewInfo;
    const VkFormat format = VkFormat(v.Format);
    const VkImage game = reinterpret_cast<VkImage>(v.Image);
    const bool depth = depth_format(format);
    if (plain_view(r) && (usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) {   // (usage not known: the program, as 0.0.7)
        if (!ensure_image(dst, format, w, h, depth)) return false;
        const VkImageAspectFlags aspect = depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
        vk_barrier_at(cmd, game, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, layout_aspect(format), 0, 0);
        vk_barrier_at(cmd, dst.image, dst.layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, aspect, 0, 0);
        VkImageCopy c{};
        c.srcSubresource = c.dstSubresource = {aspect, 0, 0, 1};
        c.srcOffset = {int32_t(sx), int32_t(sy), 0};
        c.extent = {w, h, 1};
        vkCmdCopyImage(cmd, game, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        vk_barrier_at(cmd, game, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, layout, layout_aspect(format), 0, 0);
        vk_barrier_at(cmd, dst.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, dst_end, aspect, 0, 0);
        dst.layout = dst_end;
        return true;
    }
    // usage not known: NGX's contract - an input is sampled, the upscaler's output (`written`) a storage image (read
    // with imageLoad: it need not be sampled)
    const bool sampled = usage ? (usage & VK_IMAGE_USAGE_SAMPLED_BIT) != 0 : !written;
    if (!sampled && !(usage ? (usage & VK_IMAGE_USAGE_STORAGE_BIT) != 0 : written)) return false;
    const VkFormat to = depth ? VK_FORMAT_R32_SFLOAT : format;
    if (!ensure_image(dst, to, w, h) && !(wide && ensure_image(dst, VK_FORMAT_R32G32B32A32_SFLOAT, w, h))) return false;
    static bool said = false;
    if (!said) {
        said = true;
        log("[nr] Vulkan route: a game image is read through its own view (%s) by our copy program",
            !plain_view(r) ? "a mip level, array layer or channel order of its own" : usage ? "no transfer use"
                                                                                           : "its usage not known");
    }
    vk_barrier_at(cmd, dst.image, dst.layout, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0);
    const bool ok = nr::asi::wilsjo2_copy_vk(cmd, copy_side(r, layout, sampled), !sampled, sx, sy,
                                             copy_side(dst, VK_IMAGE_LAYOUT_GENERAL), 0, 0, w, h);
    vk_barrier_at(cmd, dst.image, VK_IMAGE_LAYOUT_GENERAL, dst_end, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0);
    dst.layout = dst_end;
    return ok;
}
// `src` (ours, GENERAL; w x h from its origin) back into the game's image `r` at (dx, dy) (in `layout`, left in it): a
// transfer copy where `r` allows one, else our copy program into a storage view of ours at the view's mip level and
// array layer (an upscaler's output has storage use).
bool copy_out(VkCommandBuffer cmd, const OwnImage& src, const NVSDK_NGX_Resource_VK* r, VkImageLayout layout,
              VkImageUsageFlags usage, uint32_t dx, uint32_t dy, uint32_t w, uint32_t h) {
    const auto& v = r->Resource.ImageViewInfo;
    const VkImage game = reinterpret_cast<VkImage>(v.Image);
    const VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    if (plain_view(r) && (usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {   // (usage not known: the storage store, NGX's
                                                                         // contract for an upscaler's output)
        vk_barrier_at(cmd, src.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, aspect, 0, 0);
        vk_barrier_at(cmd, game, layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, aspect, 0, 0);
        vk_copy(cmd, src.image, 0, 0, game, dx, dy, w, h);
        vk_barrier_at(cmd, game, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, layout, aspect, 0, 0);
        vk_barrier_at(cmd, src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, aspect, 0, 0);
        return true;
    }
    if (usage && !(usage & VK_IMAGE_USAGE_STORAGE_BIT)) return false;
    const auto side = copy_side(r, VK_IMAGE_LAYOUT_GENERAL, false);
    vk_barrier_at(cmd, game, layout, VK_IMAGE_LAYOUT_GENERAL, aspect, side.mip, side.layer);
    const bool ok = nr::asi::wilsjo2_copy_vk(cmd, copy_side(src, VK_IMAGE_LAYOUT_GENERAL), false, 0, 0, side, dx, dy, w, h);
    vk_barrier_at(cmd, game, VK_IMAGE_LAYOUT_GENERAL, layout, aspect, side.mip, side.layer);
    return ok;
}

// The game's queue calls with the per-queue lock our submits take too. Two detour instances a call: [0] winevulkan's
// export, [1] the address the device's vkGetDeviceProcAddr gives when it is another one (vkQueueSubmit2KHR has no
// export). vkDeviceWaitIdle takes the lock of the queue our builds submit on (it needs every queue of the device).
struct QueueHold {
    VkQueue q;
    bool held;
    explicit QueueHold(VkQueue queue) : q(queue), held(nr::pe::vkdevice::lock_queue_held(queue)) {}
    ~QueueHold() { nr::pe::vkdevice::unlock_queue_held(q, held); }
};
PFN_vkQueueSubmit real_queue_submit[2] = {};
PFN_vkQueueSubmit2 real_queue_submit2[2] = {}, real_queue_submit2_khr[2] = {};
PFN_vkQueueWaitIdle real_queue_wait_idle[2] = {};
PFN_vkQueuePresentKHR real_queue_present[2] = {};
PFN_vkQueueBindSparse real_queue_bind_sparse[2] = {};
PFN_vkDeviceWaitIdle real_device_wait_idle[2] = {};
// The device and queue NR's Vulkan session runs on (the Vulkan route's, or DXVK's for the D3D11 route; null: none).
std::atomic<VkDevice> g_route_device{VK_NULL_HANDLE};
std::atomic<VkQueue> g_route_queue{VK_NULL_HANDLE};
std::atomic<uint32_t> g_route_family{~0u};   // g_route_queue's family (vkdevice::queue_for's answer; D3D11: DXVK's)
namespace receipts = nr::pe::receipts;
PFN_vkResetFences real_reset_fences[2] = {};
PFN_vkDestroyFence real_destroy_fence[2] = {};
std::atomic<bool> g_fence_hooks{false};
thread_local unsigned t_submit_depth = 0;
struct SubmitDepth { SubmitDepth() { ++t_submit_depth; } ~SubmitDepth() { --t_submit_depth; } };

template <int I> VKAPI_ATTR VkResult VKAPI_CALL hk_reset_fences(VkDevice d, uint32_t n, const VkFence* f) {
    std::lock_guard<std::recursive_mutex> guard(receipts::mutex);
    for (uint32_t i = 0; i < n; ++i) receipts::invalidate(d, f[i]);
    return real_reset_fences[I](d, n, f);
}
template <int I> VKAPI_ATTR void VKAPI_CALL hk_destroy_fence(VkDevice d, VkFence f, const VkAllocationCallbacks* a) {
    std::lock_guard<std::recursive_mutex> guard(receipts::mutex);
    receipts::invalidate(d, f);
    real_destroy_fence[I](d, f, a);
}
void end_receipts(VkDevice device) { receipts::end(device); }

// When the game's reset/destroy entry points are covered, its fence supplies the receipt. With no game fence,
// attach a pooled one to the original submit. Only an uncovered game fence requires another submit.
template<class Submit> VkResult tracked_submit(VkQueue q, VkFence game, const std::vector<nr::gpu::Ticket>& tickets,
        const std::vector<VkSemaphore>& sems, Submit submit, PFN_vkQueueSubmit fallback) {
    const VkDevice device = g_route_device.load();
    if (!device || (tickets.empty() && !receipts::watched(device, uint32_t(sems.size()), sems.data()))) return submit(game);
    const bool extra = game && !g_fence_hooks.load();
    auto receipt = receipts::acquire(device, extra ? VK_NULL_HANDLE : game);
    const VkResult result = submit(game ? game : receipt ? receipt->fence : VK_NULL_HANDLE);
    if (result != VK_SUCCESS) { receipts::cancel(receipt); return result; }
    if (extra && (!receipt || !fallback || fallback(q, 0, nullptr, receipt->fence) != VK_SUCCESS)) {
        receipts::cancel(receipt); receipt.reset();
    }
    receipts::submitted(receipt);
    for (const auto& t : tickets) t->submitted(receipt ? std::function<bool()>([receipt] { return receipts::done(receipt); }) : std::function<bool()>{});
    for (VkSemaphore sem : sems) receipts::used(device, sem, receipt);
    return result;
}
template <int I> VKAPI_ATTR VkResult VKAPI_CALL hk_queue_submit(VkQueue q, uint32_t n, const VkSubmitInfo* s, VkFence f) {
    if (t_submit_depth) return real_queue_submit[I](q, n, s, f);
    SubmitDepth depth;
    QueueHold hold(q);
    std::vector<nr::gpu::Ticket> tickets;
    std::vector<VkSemaphore> sems;
    for (uint32_t i = 0; s && i < n; ++i) {
        nr::asi::hudcap::vk_submitted(s[i].commandBufferCount, s[i].pCommandBuffers);
        for (uint32_t j = 0; j < s[i].commandBufferCount; ++j)
            if (auto t = nr::gpu::find(s[i].pCommandBuffers[j])) tickets.push_back(std::move(t));
        for (uint32_t j = 0; j < s[i].waitSemaphoreCount; ++j) sems.push_back(s[i].pWaitSemaphores[j]);
        for (uint32_t j = 0; j < s[i].signalSemaphoreCount; ++j) sems.push_back(s[i].pSignalSemaphores[j]);
    }
    return tracked_submit(q, f, tickets, sems, [&](VkFence fence) { return real_queue_submit[I](q, n, s, fence); }, real_queue_submit[I]);
}
VkResult tracked_submit2(PFN_vkQueueSubmit2 submit, PFN_vkQueueSubmit fallback, VkQueue q, uint32_t n, const VkSubmitInfo2* s, VkFence f) {
    if (t_submit_depth) return submit(q, n, s, f);
    SubmitDepth depth;
    QueueHold hold(q);
    nr::asi::hudcap::vk_submitted2(n, s);
    std::vector<nr::gpu::Ticket> tickets;
    std::vector<VkSemaphore> sems;
    for (uint32_t i = 0; s && i < n; ++i) {
        for (uint32_t j = 0; j < s[i].commandBufferInfoCount; ++j)
            if (auto t = nr::gpu::find(s[i].pCommandBufferInfos[j].commandBuffer)) tickets.push_back(std::move(t));
        for (uint32_t j = 0; j < s[i].waitSemaphoreInfoCount; ++j) sems.push_back(s[i].pWaitSemaphoreInfos[j].semaphore);
        for (uint32_t j = 0; j < s[i].signalSemaphoreInfoCount; ++j) sems.push_back(s[i].pSignalSemaphoreInfos[j].semaphore);
    }
    return tracked_submit(q, f, tickets, sems, [&](VkFence fence) { return submit(q, n, s, fence); }, fallback);
}
template <int I> VKAPI_ATTR VkResult VKAPI_CALL hk_queue_submit2(VkQueue q, uint32_t n, const VkSubmitInfo2* s, VkFence f) {
    return tracked_submit2(real_queue_submit2[I], real_queue_submit[I] ? real_queue_submit[I] : real_queue_submit[0], q, n, s, f);
}
template <int I> VKAPI_ATTR VkResult VKAPI_CALL hk_queue_submit2_khr(VkQueue q, uint32_t n, const VkSubmitInfo2* s, VkFence f) {
    return tracked_submit2(real_queue_submit2_khr[I], real_queue_submit[I] ? real_queue_submit[I] : real_queue_submit[0], q, n, s, f);
}
template <int I> VKAPI_ATTR VkResult VKAPI_CALL hk_queue_wait_idle(VkQueue q) {
    QueueHold hold(q);
    return real_queue_wait_idle[I](q);
}
// The Present layers of a native Vulkan game from the lowest present hook (null: the present as it came); see the
// Vulkan Present section.
thread_local VkResult t_present_failure = VK_SUCCESS;
const VkPresentInfoKHR* vk_present_bottom(VkQueue q, const VkPresentInfoKHR* p, void* caller);
VkResult present_receipt(PFN_vkQueuePresentKHR present, VkQueue q, const VkPresentInfoKHR* p);
template <int I> VKAPI_ATTR VkResult VKAPI_CALL hk_queue_present(VkQueue q, const VkPresentInfoKHR* p) {
    nr::pe::watchdog::beat();   // a Vulkan game's frame (it has no DXGI Present)
    t_present_failure = VK_SUCCESS;
    if (const VkPresentInfoKHR* mine = vk_present_bottom(q, p, __builtin_return_address(0))) p = mine;
    if (t_present_failure < 0) return t_present_failure;
    QueueHold hold(q);
    return present_receipt(real_queue_present[I], q, p);
}
template <int I> VKAPI_ATTR VkResult VKAPI_CALL hk_queue_bind_sparse(VkQueue q, uint32_t n, const VkBindSparseInfo* b, VkFence f) {
    QueueHold hold(q);
    return real_queue_bind_sparse[I](q, n, b, f);
}
template <int I> VKAPI_ATTR VkResult VKAPI_CALL hk_device_wait_idle(VkDevice d) {
    // the lock of the queue our builds submit on, for that device only (another device's queues are not ours)
    QueueHold hold(d && d == g_route_device.load() ? g_route_queue.load() : VK_NULL_HANDLE);
    return real_device_wait_idle[I](d);
}

// `device`: the game's. 2 when every submit and present call the game can reach takes the lock, 1 when some cannot (a
// hook that did not take, or a submit / present entry missing), 0 when the command buffers' lifetime cannot be followed
// (our objects in the game's command buffers would be freed blind).
int hook_queue_calls(VkDevice device) {
    if (!nr::asi::hudcap::vk_lifetime(device)) return 0;
    HMODULE m = GetModuleHandleW(L"winevulkan.dll");
    if (!m) return false;
    const auto gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(reinterpret_cast<void*>(GetProcAddress(m, "vkGetDeviceProcAddr")));
    struct Entry { const char* name; void* detour[2]; void** real[2]; bool needed; };
#define NR_QUEUE_ENTRY(name, hk, real, needed)                                                                          \
    Entry{name, {reinterpret_cast<void*>(&hk<0>), reinterpret_cast<void*>(&hk<1>)},                                     \
          {reinterpret_cast<void**>(&real[0]), reinterpret_cast<void**>(&real[1])}, needed}
    const Entry entries[] = {NR_QUEUE_ENTRY("vkQueueSubmit", hk_queue_submit, real_queue_submit, true),
                             NR_QUEUE_ENTRY("vkQueueSubmit2", hk_queue_submit2, real_queue_submit2, true),
                             NR_QUEUE_ENTRY("vkQueueSubmit2KHR", hk_queue_submit2_khr, real_queue_submit2_khr, false),
                             NR_QUEUE_ENTRY("vkQueueWaitIdle", hk_queue_wait_idle, real_queue_wait_idle, false),
                             NR_QUEUE_ENTRY("vkQueuePresentKHR", hk_queue_present, real_queue_present, true),
                             NR_QUEUE_ENTRY("vkQueueBindSparse", hk_queue_bind_sparse, real_queue_bind_sparse, false),
                             NR_QUEUE_ENTRY("vkDeviceWaitIdle", hk_device_wait_idle, real_device_wait_idle, false),
                             NR_QUEUE_ENTRY("vkResetFences", hk_reset_fences, real_reset_fences, false),
                             NR_QUEUE_ENTRY("vkDestroyFence", hk_destroy_fence, real_destroy_fence, false)};
#undef NR_QUEUE_ENTRY
    int hooked = 0, addresses = 0;
    bool covered = true, fences_covered = true;
    for (const Entry& e : entries) {
        void* exported = reinterpret_cast<void*>(GetProcAddress(m, e.name));
        void* asked = gdpa && device ? reinterpret_cast<void*>(gdpa(device, e.name)) : nullptr;
        void* targets[2] = {exported, asked && asked != exported ? asked : nullptr};
        bool any = false, missed = false;
        for (int i = 0; i < 2; ++i) {
            if (!targets[i]) continue;
            ++addresses;
            const MH_STATUS c = MH_CreateHook(targets[i], e.detour[i], e.real[i]);
            // ALREADY_CREATED: vkQueuePresentKHR's export, hooked at start (hook_present_beat), or a second route's
            // start; enabled again to be sure (ERROR_ENABLED: it is)
            const MH_STATUS en = c == MH_OK || c == MH_ERROR_ALREADY_CREATED ? MH_EnableHook(targets[i]) : c;
            if (en == MH_OK || en == MH_ERROR_ENABLED) {
                any = true;
                ++hooked;
            } else {
                missed = true;
                log("[nr] Vulkan route: %s (%p) could not be hooked", e.name, targets[i]);
            }
        }
        // a call the device has must take the lock; vkQueueSubmit / Submit2 / QueuePresentKHR must exist and take it
        if (missed || (e.needed && !any)) covered = false;
        if (!std::strcmp(e.name, "vkResetFences") || !std::strcmp(e.name, "vkDestroyFence")) fences_covered &= any && !missed;
    }
    g_fence_hooks.store(fences_covered);
    // NR_TEST_QUEUE_PARTIAL=1 (tests): as if a call were not covered (the hooks stay as made)
    static const bool partial = [] { const char* e = std::getenv("NR_TEST_QUEUE_PARTIAL"); return e && e[0] == '1'; }();
    if (partial) covered = false;
    log("[nr] Vulkan route: %d of %d queue-call addresses take the queue's lock%s", hooked, addresses,
        covered ? "" : " - a submit or present call is not covered");
    return covered ? 2 : 1;
}

// At start: winevulkan's vkQueuePresentKHR, so a Vulkan game's frames tell the watchdog the game runs (it has no DXGI
// Present); it takes the queue's lock only once the locks are on (hook_queue_calls counts it as hooked then).
void hook_swapchains(HMODULE winevulkan);
void hook_present_beat() {
    HMODULE m = GetModuleHandleW(L"winevulkan.dll");
    if (m) hook_swapchains(m);
    void* fn = m ? reinterpret_cast<void*>(GetProcAddress(m, "vkQueuePresentKHR")) : nullptr;
    if (!fn || MH_CreateHook(fn, reinterpret_cast<void*>(&hk_queue_present<0>), reinterpret_cast<void**>(&real_queue_present[0])) != MH_OK ||
        MH_EnableHook(fn) != MH_OK)
        log("[nr] winevulkan.dll's vkQueuePresentKHR could not be hooked: a Vulkan game's frames are not seen by the "
            "stall watch");
}

// The first Vulkan evaluate: the game's device (the newest one made; OptiScaler's own CreateFeature1 says which when
// it was seen), its queue, the model's Vulkan session. Once; false from then on when it could not be done.
bool vk_route_ready(VkDevice wanted) {
    if (g_vk_is_dxvk) return false;   // one Vulkan session a process: it serves the D3D11 route here
    if (!g_destroy_hooked) return false;   // said at start
    if (g_vk.tried) {
        if (g_vk.ready || !g_vk.retry_ms || GetTickCount64() < g_vk.retry_ms) return g_vk.ready;
        g_vk.tried = false;   // the session's start failed: once more (5 s later)
        g_vk.dev = SeenDevice{};
    }
    bool from_ngx = false;
    {
        std::lock_guard<std::mutex> guard(g_devices_lock);
        // the device OptiScaler's CreateFeature1 named; else the newest one the game made itself (not one of
        // vkd3d-proton's or DXVK's: OptiScaler's own D3D12 device for an upscaler bridged to D3D12); else the newest.
        const bool unseen = test_device_unseen();
        for (auto it = g_devices.rbegin(); !unseen && it != g_devices.rend() && !g_vk.dev.device; ++it)
            if (wanted ? it->device == wanted : !it->translator) g_vk.dev = *it;
        if (!unseen && !g_vk.dev.device && !wanted && !g_devices.empty()) g_vk.dev = g_devices.back();
        // its creation not seen (this module loaded after it): the device NGX's Vulkan init was given, its queue the
        // first of a graphics family (the game's queues are not known: the one nearly every game makes)
        if (!g_vk.dev.device && g_ngx_vk.device && (!wanted || wanted == g_ngx_vk.device)) {
            g_vk.dev = g_ngx_vk;
            nr::pe::vulkan_entry_points();
            uint32_t n = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(g_vk.dev.physical, &n, nullptr);
            std::vector<VkQueueFamilyProperties> fam(n);
            vkGetPhysicalDeviceQueueFamilyProperties(g_vk.dev.physical, &n, fam.data());
            for (uint32_t i = 0; i < n && g_vk.dev.families.empty(); ++i)
                if (fam[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { g_vk.dev.families = {i}; g_vk.dev.counts = {1}; }
            static bool said = false;
            if (!said) {
                said = true;
                log("[nr] Vulkan route: the game's device was not seen being made; the one NGX's Vulkan init was given is "
                    "taken (%p), its queue the first of family %d", static_cast<void*>(g_vk.dev.device),
                    g_vk.dev.families.empty() ? -1 : int(g_vk.dev.families[0]));
            }
            if (g_vk.dev.families.empty()) g_vk.dev = SeenDevice{};
            else from_ngx = true;
        }
    }
    // HUD FIX's capture and the images' usage from now on (the creation hooks a seen device got at its vkCreateDevice)
    if (from_ngx) nr::asi::hudcap::vk_device(g_vk.dev.device, false);
    if (!g_vk.dev.device || !g_vk.dev.instance || !g_vk.dev.physical) {
        // asked again at the next evaluate (a device made later, or the named one recorded later): said once
        static bool said = false;
        if (!said) {
            said = true;
            log("[nr] Vulkan route: the game's Vulkan device was not seen being made (winevulkan.dll's vkCreateDevice / "
                "vkGetInstanceProcAddr); NR does not run on it");
        }
        g_vk.dev = SeenDevice{};
        return false;
    }
    g_vk.tried = true;
    nr::pe::vulkan_entry_points();
    vkGetPhysicalDeviceMemoryProperties(g_vk.dev.physical, &g_vk.memory);
    nr::pe::vkdevice::enable_queue_locks();   // before the hooks: no queue call enters unlocked and leaves locked
    static int coverage = -1;   // the hooks once (a retry keeps them)
    if (coverage < 0) coverage = hook_queue_calls(g_vk.dev.device);
    if (coverage == 0) {
        log("[nr] Vulkan route: the game's command buffers cannot be followed (their lifetime calls could not be "
            "hooked); NR does not run on this Vulkan game");
        return false;
    }
    if (coverage == 1) {   // as 0.0.7 (no lock at all): the covered calls wait for our submits, the others do not
        static bool said = false;
        if (!said) {
            said = true;
            log("[nr] Vulkan route: not every queue call of the game takes the lock our background submits take; NR runs "
                "(as 0.0.7, which had no such lock) - a game calling an uncovered one at the same moment as our build "
                "could collide");
        }
    }
    nr::pe::vkdevice::note_device(g_vk.dev.instance, g_vk.dev.physical, g_vk.dev.device, g_vk.dev.families);
    nr::asi::wilsjo2_vk_device(g_vk.dev.device, g_vk.dev.physical);
    const int rc = test_fault("vk_init") ? int(NVSDK_NGX_Result_Fail)
                                         : nr::dlssnr::vk_init(g_vk.dev.instance, g_vk.dev.physical, g_vk.dev.device);
    {
        VkQueue queue = VK_NULL_HANDLE;
        uint32_t family = 0;
        nr::pe::vkdevice::queue_for(g_vk.dev.device, &queue, &family);
        g_route_queue.store(queue);
        g_route_family.store(queue ? family : ~0u);
        g_route_device.store(g_vk.dev.device);
    }
    if (rc != int(NVSDK_NGX_Result_Success)) {
        static bool said = false;
        if (!said) {
            said = true;
            log("[nr] Vulkan route: the model's Vulkan session could not start (0x%08X); NR does not run - tried again "
                "every 5 s", unsigned(rc));
        }
        g_vk.retry_ms = GetTickCount64() + 5000;
        return false;
    }
    g_vk.ready = true;
    g_vk.retry_ms = 0;
    log("[nr] Vulkan route: NR runs in OptiScaler's Vulkan evaluate (device %p)", static_cast<void*>(g_vk.dev.device));
    return true;
}

struct VkLayerState {
    nr::pe::Processing processing = nr::pe::Processing::kRenoDX;
    Feature* nr[2] = {};                    // the model's feature for each placement: 0 after, 1 before
    nr::asi::Wilsjo2VkOwner* w2[2] = {};    // wilsjo2: its state for each placement
};
void vk_layer_release(VkLayerState& l) {
    for (Feature*& f : l.nr)
        if (f) { nr::dlssnr::release_vk(f); f = nullptr; }
    for (nr::asi::Wilsjo2VkOwner*& o : l.w2)
        if (o) { nr::asi::wilsjo2_vk_release(o); o = nullptr; }
}
// What the game's Vulkan evaluate carries for NR (as Frame / read_frame for D3D12; the extents are the resources').
struct VkFrame {
    const NVSDK_NGX_Resource_VK *color = nullptr, *output = nullptr, *depth = nullptr, *motion = nullptr, *exposure = nullptr;
    const char* color_key = "Color";
    uint32_t color_x = 0, color_y = 0, render_w = 0, render_h = 0, out_x = 0, out_y = 0, out_w = 0, out_h = 0;
    uint32_t subrect_w = 0, subrect_h = 0;   // DLSS.Render.Subrect.Dimensions as given (0: not given)
    Rect depth_rect, motion_rect;
    uint32_t flags = 0;
    bool flags_known = false, reset = false;
    float pre_exposure = 1.0f, exposure_scale = 1.0f, mv_x = 1.0f, mv_y = 1.0f;
    // the layouts the images are in during the call: NGX's Vulkan contract, or what DXVK says (a D3D11 game)
    VkImageLayout color_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, output_layout = VK_IMAGE_LAYOUT_GENERAL,
                  depth_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  motion_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  exposure_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    // the images' usage when known (DXVK says it on D3D11, the game's vkCreateImage on Vulkan; 0: NGX's contract
    // assumed, vk_ref)
    VkImageUsageFlags output_usage = 0, depth_usage = 0, motion_usage = 0, color_usage = 0, exposure_usage = 0;
};

struct VkTracked {
    OwnImage hold_guides[3];
    VkFrame hold_input;
    nr::asi::Wilsjo2VkFrame hold_w2;
    bool hold_inputs_ready = false;
    uint32_t hold_encoding = 0; float hold_white = 0.f;

    int feature = -1;
    VkDevice device = VK_NULL_HANDLE;   // the device OptiScaler's CreateFeature1 named (null: not said)
    bool flags_known = false;
    uint32_t flags = 0;
    CreateSizes create;                 // its create block's sizes (as Tracked)
    std::map<int, VkLayerState> layers;
    uint64_t chain_version = 0;
    OwnImage copy, out_copy;
    OwnImage guides[3];                 // depth, motion, exposure where read through their views (vk_plain_guides)
    OwnImage out_whole;                 // the output where it cannot be used in place (run_after_vk)
    OwnImage inspect_off, inspect_on;   // [Inspect]: the frame before the compared layers, and after them
    OwnImage hold;                      // [Inspect] HoldFrame: the chosen layer's input, held (`held`)
    bool held = false;
    int hold_layer = -1;                // as HoldCopy: the row it was held for, its last use
    ULONGLONG hold_ms = 0;
    bool said = false;
};
std::unordered_map<uint32_t, VkTracked> g_vk_features;   // g_lock
VkDevice g_vk_created_on = VK_NULL_HANDLE;                // the device OptiScaler's last CreateFeature1 named

void vk_forget(VkTracked& t) {
    for (auto& [n, l] : t.layers) vk_layer_release(l);
    t.layers.clear();
    retire_image(t.copy);
    retire_image(t.out_copy);
    for (auto& im : t.guides) retire_image(im);
    retire_image(t.out_whole);
    retire_image(t.inspect_off);
    retire_image(t.inspect_on);
    retire_image(t.hold);
    for (auto& im : t.hold_guides) retire_image(im);
    t.hold_inputs_ready = false;
    if (t.held) g_hold_released.insert(t.hold_layer);
    t.held = false;
}

void vk_present_forget();
void vk_route_teardown(VkDevice device) {
    std::lock_guard<std::mutex> guard(g_lock);
    if (g_vk_created_on == device) g_vk_created_on = VK_NULL_HANDLE;
    // features made on that device: never served again
    for (auto it = g_vk_features.begin(); it != g_vk_features.end();)
        if (it->second.device == device) { vk_forget(it->second); it = g_vk_features.erase(it); } else ++it;
    if (g_vk_is_dxvk || !g_vk.tried || device != g_vk.dev.device) return;
    if (!g_vk.ready) {   // the route had picked it and failed to start: another device may be tried again; whatever
                         // the start made on it before it failed goes with it
        nr::asi::wilsjo2_vk_device_destroyed();
        nr::dlssnr::vk_device_destroyed();
        g_route_device.store(VK_NULL_HANDLE);
        g_route_queue.store(VK_NULL_HANDLE);
        g_vk = VkRoute{};
        return;
    }
    log("[nr] Vulkan route: the game destroys device %p, NR's session runs on it: everything of NR's on it goes now",
        static_cast<void*>(device));
    {   // its queues idle (our builds submit on the game's queue: under its lock)
        QueueHold hold(g_route_queue.load());
        // winevulkan's own (the detour's trampoline when it is hooked: this lock is taken already)
        PFN_vkDeviceWaitIdle wait = real_device_wait_idle[0];
        if (!wait)
            if (HMODULE m = GetModuleHandleW(L"winevulkan.dll"))
                wait = reinterpret_cast<PFN_vkDeviceWaitIdle>(reinterpret_cast<void*>(GetProcAddress(m, "vkDeviceWaitIdle")));
        if (wait) wait(device);
    }
    // features of other devices stay known (a later route on their device serves them); only their state on this
    // device goes (they have none unless the route ran them: it runs features of its own device only)
    for (auto& [id, t] : g_vk_features) vk_forget(t);
    vk_present_forget();   // the Present layers' state and images, our present command buffers
    for (auto& r : g_retired_images) destroy_image(r.im);
    g_retired_images.clear();
    nr::asi::wilsjo2_vk_device_destroyed();
    nr::dlssnr::vk_device_destroyed();
    g_route_device.store(VK_NULL_HANDLE);
    g_route_queue.store(VK_NULL_HANDLE);
    g_vk = VkRoute{};   // a later Vulkan evaluate may start the route on the game's next device
}

void vk_track(NVSDK_NGX_Handle** out, NVSDK_NGX_Feature feature, NVSDK_NGX_Parameter* p, VkDevice device) {
    std::lock_guard<std::mutex> guard(g_lock);
    VkTracked& t = g_vk_features[(*out)->Id];
    vk_forget(t);
    t = VkTracked{};
    t.feature = int(feature);
    t.device = device;
    t.create = create_sizes(p);
    if (device) g_vk_created_on = device;
    int flags = 0;
    if (p && p->Get("DLSS.Feature.Create.Flags", &flags) == NVSDK_NGX_Result_Success) {
        t.flags = uint32_t(flags);
        t.flags_known = true;
    }
    log("[nr] OptiScaler Vulkan feature %u made: NGX feature %d, flags 0x%x%s", (*out)->Id, int(feature), t.flags,
        t.flags_known ? "" : " (not given)");
}
NVSDK_NGX_Result NR_NGX_MSABI hk_vk_create1(VkDevice dev, VkCommandBuffer cmd, NVSDK_NGX_Feature feature,
                                           NVSDK_NGX_Parameter* p, NVSDK_NGX_Handle** out) {
    const NVSDK_NGX_Result r = real_vk_create1(dev, cmd, feature, p, out);
    if (r == NVSDK_NGX_Result_Success && out && *out) vk_track(out, feature, p, dev);
    return r;
}
NVSDK_NGX_Result NR_NGX_MSABI hk_vk_create(VkCommandBuffer cmd, NVSDK_NGX_Feature feature, NVSDK_NGX_Parameter* p,
                                          NVSDK_NGX_Handle** out) {
    const NVSDK_NGX_Result r = real_vk_create(cmd, feature, p, out);
    if (r == NVSDK_NGX_Result_Success && out && *out) vk_track(out, feature, p, VK_NULL_HANDLE);
    return r;
}
NVSDK_NGX_Result NR_NGX_MSABI hk_vk_release(NVSDK_NGX_Handle* h) {
    if (h) {
        std::lock_guard<std::mutex> guard(g_lock);
        auto it = g_vk_features.find(h->Id);
        if (it != g_vk_features.end()) { vk_forget(it->second); g_vk_features.erase(it); }
    }
    return real_vk_release(h);
}

// An NGX Vulkan resource as the model takes it.
// `usage`: the image's usage when known (DXVK says it on D3D11), else 0: what NGX's contract implies is taken
// (sampled + transfer, storage when written) - an NGX Vulkan resource does not carry it.
nr::dlssnr::VkImageRef vk_ref(const NVSDK_NGX_Resource_VK* r, VkImageLayout layout, bool written, VkImageUsageFlags usage = 0) {
    nr::dlssnr::VkImageRef out;
    if (!r || r->Type != NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW || !r->Resource.ImageViewInfo.Image) return out;
    const auto& i = r->Resource.ImageViewInfo;
    out.image = uint64_t(reinterpret_cast<uintptr_t>(i.Image));
    out.format = i.Format; out.width = i.Width; out.height = i.Height;
    out.layout = uint32_t(layout);
    out.usage = usage ? usage
                      : VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                            (written || r->ReadWrite ? VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT : 0u);
    return out;
}
nr::dlssnr::VkImageRef own_ref(const OwnImage& im) {
    nr::dlssnr::VkImageRef out;
    out.image = uint64_t(reinterpret_cast<uintptr_t>(im.image));
    out.format = im.format; out.width = im.width; out.height = im.height;
    out.layout = uint32_t(VK_IMAGE_LAYOUT_GENERAL);
    out.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    return out;
}
const NVSDK_NGX_Resource_VK* vk_resource(const NVSDK_NGX_Parameter* p, const char* name, const char* rr_name) {
    void* v = nullptr;
    if (p->Get(name, &v) == NVSDK_NGX_Result_Success && v) return static_cast<const NVSDK_NGX_Resource_VK*>(v);
    if (rr_name && p->Get(rr_name, &v) == NVSDK_NGX_Result_Success && v) return static_cast<const NVSDK_NGX_Resource_VK*>(v);
    return nullptr;
}
bool vk_float(uint32_t f) {
    switch (VkFormat(f)) {
        case VK_FORMAT_R16G16B16A16_SFLOAT: case VK_FORMAT_R32G32B32A32_SFLOAT: case VK_FORMAT_R32G32B32_SFLOAT:
        case VK_FORMAT_R16G16B16_SFLOAT:   // as 0.0.7's Vulkan NR (FormatCanHoldLinearHdr)
        case VK_FORMAT_B10G11R11_UFLOAT_PACK32: case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32:
            return true;
        default:
            return false;
    }
}


bool read_frame_dims(const NVSDK_NGX_Parameter* p, VkFrame& f, const CreateSizes& create);
// OptiScaler.ini [InitFlags]' overrides of the game's DLSS flags (AutoExposure, HDR, DepthInverted, DisplayResolution:
// true / false, else the game's), as OptiScaler's Vulkan feature applies them and 0.0.7's Vulkan NR got them with it.
// Read again every 5 s: a change made in OptiScaler's menu reaches NR once it is saved (Save Settings) - the menu's
// unsaved values live only inside OptiScaler (a plugin of the official OptiScaler has no way to them).
// Not taken: OptiScaler's [OutputScaling] rule (Enabled with render size = display size: low-resolution vectors). It is
// for its upscaler, whose target is then the display size times the multiplier; NR works on the game's own output, the
// display size the vectors are given in - with dynamic resolution the rule would scale them wrongly for NR.
uint32_t optiscaler_init_flags(uint32_t flags) {
    struct Key { const char* name; uint32_t bit; bool set_when_true; };
    static const Key kKeys[] = {{"AutoExposure", kFlagAutoExposure, true}, {"HDR", kFlagIsHdr, true},
                                {"DepthInverted", kFlagDepthInverted, true}, {"DisplayResolution", kFlagMvLowRes, false}};
    static int forced[4] = {-1, -1, -1, -1};   // -1 the game's, 0 false, 1 true
    static ULONGLONG read = 0;
    const ULONGLONG now = GetTickCount64();
    if (!read || now - read >= 5000) {
        read = now;
        char path[MAX_PATH + 32];
        std::snprintf(path, sizeof path, "%s\\OptiScaler.ini", nr::pe::module_folder());
        for (int i = 0; i < 4; ++i) {
            char v[16] = {};
            GetPrivateProfileStringA("InitFlags", kKeys[i].name, "", v, sizeof v, path);
            forced[i] = _stricmp(v, "true") == 0 ? 1 : _stricmp(v, "false") == 0 ? 0 : -1;
        }
    }
    for (int i = 0; i < 4; ++i) {
        if (forced[i] < 0) continue;
        const bool on = (forced[i] == 1) == kKeys[i].set_when_true;   // DisplayResolution true = not low-res vectors
        flags = on ? (flags | kKeys[i].bit) : (flags & ~kKeys[i].bit);
    }
    return flags;
}
bool read_vk_frame(const NVSDK_NGX_Parameter* p, const VkTracked& t, VkFrame& f) {
    f.color = vk_resource(p, "Color", "DLSSD.Color");
    void* probe = nullptr;
    if (p->Get("Color", &probe) != NVSDK_NGX_Result_Success || !probe) f.color_key = "DLSSD.Color";
    f.output = vk_resource(p, "Output", "DLSSD.Output");
    f.depth = vk_resource(p, "Depth", "DLSSD.Depth");
    f.motion = vk_resource(p, "MotionVectors", "DLSSD.MotionVectors");
    f.exposure = vk_resource(p, "ExposureTexture", nullptr);
    for (const NVSDK_NGX_Resource_VK* r : {f.color, f.output, f.depth, f.motion})
        if (!r || r->Type != NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW || !r->Resource.ImageViewInfo.Image) return false;
    if (f.exposure && (f.exposure->Type != NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW || !f.exposure->Resource.ImageViewInfo.Image))
        f.exposure = nullptr;
    // The inputs' layouts stay SHADER_READ_ONLY_OPTIMAL whatever their ReadWrite says: ReadWrite is the image's
    // storage (UAV) use, not its layout - OptiScaler's own upscalers take every input as COMPUTE_READ (FFXFeature_Vk:
    // FFX_API_RESOURCE_STATE_COMPUTE_READ, ReadWrite only adds the UAV usage), so a game it runs keeps them there.
    // (10-27: taking ReadWrite as GENERAL, as the 0.0.7 fork did for depth / motion, read the stand-ins' storage-usage
    // guides from the wrong layout: gates/vkinputs.sh failed.)
    // a view at another mip level or array layer, or with its channels reordered, is read through that view (copy_in,
    // vk_plain_guides); the images' usage where the game's vkCreateImage was seen
    f.color_usage = seen_usage(f.color);
    f.output_usage = seen_usage(f.output);
    f.depth_usage = seen_usage(f.depth);
    f.motion_usage = seen_usage(f.motion);
    f.exposure_usage = seen_usage(f.exposure);
    f.flags_known = t.flags_known;
    f.flags = t.flags;
    if (!f.flags_known) {
        int flags = 0;
        if (p->Get("DLSS.Feature.Create.Flags", &flags) == NVSDK_NGX_Result_Success) { f.flags = uint32_t(flags); f.flags_known = true; }
    }
    if (f.flags_known) f.flags = optiscaler_init_flags(f.flags);
    return read_frame_dims(p, f, t.create);
}
// The extents, regions and scalars of the call (as read_frame does on D3D12; the extents are the images').
bool read_frame_dims(const NVSDK_NGX_Parameter* p, VkFrame& f, const CreateSizes& create) {
    const auto& ci = f.color->Resource.ImageViewInfo;
    const auto& oi = f.output->Resource.ImageViewInfo;
    f.render_w = f.subrect_w = get_uint(p, "DLSS.Render.Subrect.Dimensions.Width");
    f.render_h = f.subrect_h = get_uint(p, "DLSS.Render.Subrect.Dimensions.Height");
    if (!f.render_w || !f.render_h) {   // as read_frame: the evaluate's, the create block's, the colour's
        f.render_w = get_uint(p, "Width", create.in_w ? create.in_w : ci.Width);
        f.render_h = get_uint(p, "Height", create.in_h ? create.in_h : ci.Height);
    }
    f.color_x = get_uint(p, "DLSS.Input.Color.Subrect.Base.X");
    f.color_y = get_uint(p, "DLSS.Input.Color.Subrect.Base.Y");
    f.out_x = get_uint(p, "DLSS.Output.Subrect.Base.X");
    f.out_y = get_uint(p, "DLSS.Output.Subrect.Base.Y");
    output_size(p, create, oi.Width, oi.Height, &f.out_w, &f.out_h);
    if (!f.out_w || !f.out_h) { f.out_w = oi.Width; f.out_h = oi.Height; }
    if (f.out_x >= oi.Width || f.out_y >= oi.Height) { f.out_x = f.out_y = 0; }
    f.out_w = std::min<uint32_t>(f.out_w, oi.Width - f.out_x);
    f.out_h = std::min<uint32_t>(f.out_h, oi.Height - f.out_y);
    f.depth_rect = {get_uint(p, "DLSS.Input.Depth.Subrect.Base.X"), get_uint(p, "DLSS.Input.Depth.Subrect.Base.Y"), f.render_w,
                    f.render_h};
    const bool low_res_mv = !f.flags_known || (f.flags & kFlagMvLowRes) != 0;
    f.motion_rect = {get_uint(p, "DLSS.Input.MV.SubrectBase.X"), get_uint(p, "DLSS.Input.MV.SubrectBase.Y"),
                     low_res_mv ? f.render_w : f.out_w, low_res_mv ? f.render_h : f.out_h};
    if (!low_res_mv && f.motion) {   // display-size vectors declared that the image cannot hold: the part it holds (0.0.7's)
        const auto& mi = f.motion->Resource.ImageViewInfo;
        if (f.motion_rect.x + f.motion_rect.width > mi.Width || f.motion_rect.y + f.motion_rect.height > mi.Height) {
            static int said = 0;
            if (said++ < 2)
                log("[nr] the game says its motion vectors are at the output's size (%ux%u) but they are %ux%u: the part "
                    "they cover is read", f.motion_rect.width, f.motion_rect.height, mi.Width, mi.Height);
            f.motion_rect.x = std::min<uint32_t>(f.motion_rect.x, mi.Width);
            f.motion_rect.y = std::min<uint32_t>(f.motion_rect.y, mi.Height);
            f.motion_rect.width = std::min<uint32_t>(f.motion_rect.width, mi.Width - f.motion_rect.x);
            f.motion_rect.height = std::min<uint32_t>(f.motion_rect.height, mi.Height - f.motion_rect.y);
        }
    }
    f.reset = get_uint(p, "Reset") != 0;
    if (!get_float(p, "DLSS.Pre.Exposure", &f.pre_exposure) || !(f.pre_exposure > 1e-6f) || !std::isfinite(f.pre_exposure)) f.pre_exposure = 1.0f;
    if (!get_float(p, "DLSS.Exposure.Scale", &f.exposure_scale) || !(f.exposure_scale > 0.0f) || !std::isfinite(f.exposure_scale)) f.exposure_scale = 1.0f;
    if (!get_float(p, "MV.Scale.X", &f.mv_x) || !std::isfinite(f.mv_x) || f.mv_x == 0.0f) f.mv_x = 1.0f;
    if (!get_float(p, "MV.Scale.Y", &f.mv_y) || !std::isfinite(f.mv_y) || f.mv_y == 0.0f) f.mv_y = 1.0f;
    return true;
}

// The depth, motion vectors and exposure as every layer reads them where they are (sampled, and a transfer source for
// the copies some take): where the call's view or the image does not allow that (another mip level or array layer,
// reordered channels, no such use), read through the view into images of ours (copy_in) the frame uses instead, in
// SHADER_READ_ONLY_OPTIMAL. False when depth or motion cannot be read (the frame is not run on); an exposure that
// cannot is left out.
bool vk_plain_guides(VkCommandBuffer cmd, VkTracked& t, VkFrame& f) {
    const NVSDK_NGX_Resource_VK** r[3] = {&f.depth, &f.motion, &f.exposure};
    VkImageLayout* layout[3] = {&f.depth_layout, &f.motion_layout, &f.exposure_layout};
    VkImageUsageFlags* usage[3] = {&f.depth_usage, &f.motion_usage, &f.exposure_usage};
    for (int i = 0; i < 3; ++i) {
        if (!*r[i]) continue;
        if (usable_in_place(*r[i], *usage[i], VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) continue;
        const auto& v = (*r[i])->Resource.ImageViewInfo;
        if (!copy_in(cmd, *r[i], *layout[i], *usage[i], 0, 0, v.Width, v.Height, t.guides[i],
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, true)) {
            static bool said[3] = {};
            if (!said[i]) {
                said[i] = true;
                log("[nr] Vulkan route: the %s could not be read through its view; %s", i == 0 ? "depth" : i == 1 ? "motion vectors" : "exposure texture",
                    i == 2 ? "NR runs without it" : "NR does not run on such frames");
            }
            if (i < 2) return false;
            f.exposure = nullptr;
            continue;
        }
        *r[i] = &t.guides[i].ngx;
        *layout[i] = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        *usage[i] = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;   // ours
    }
    return true;
}

// The wilsjo2 processing's view of the frame (as wilsjo2_frame does on D3D12): the guides and the exposure where the
// call has them (SHADER_READ_ONLY_OPTIMAL), the guide regions clipped to the images.
nr::asi::Wilsjo2VkImage w2_image(const NVSDK_NGX_Resource_VK* r, VkImageLayout layout) {
    nr::asi::Wilsjo2VkImage out;
    if (!r || r->Type != NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW || !r->Resource.ImageViewInfo.Image) return out;
    const auto& i = r->Resource.ImageViewInfo;
    out.image = reinterpret_cast<VkImage>(i.Image);
    out.format = VkFormat(i.Format);
    out.width = i.Width; out.height = i.Height;
    out.layout = layout;
    return out;
}
Rect vk_guide_subrect(const NVSDK_NGX_Resource_VK* r, uint32_t want_w, uint32_t want_h, uint32_t x, uint32_t y) {
    const uint32_t aw = r->Resource.ImageViewInfo.Width, ah = r->Resource.ImageViewInfo.Height;
    Rect out;
    out.x = std::min(x, aw);
    out.y = std::min(y, ah);
    const uint32_t vw = aw - out.x, vh = ah - out.y;
    out.width = std::min(want_w ? want_w : vw, vw);
    out.height = std::min(want_h ? want_h : vh, vh);
    return out;
}
nr::asi::Wilsjo2VkFrame wilsjo2_vk_frame(const NVSDK_NGX_Parameter* p, const VkFrame& f) {
    nr::asi::Wilsjo2VkFrame w;
    w.depth = w2_image(f.depth, f.depth_layout);
    w.motion = w2_image(f.motion, f.motion_layout);
    w.exposure = w2_image(f.exposure, f.exposure_layout);
    w.flags = f.flags_known ? f.flags : 0u;
    // 4.4: IsHDR (or AutoExposure, OptiScaler fix 5) and the authority, a float format: after the upscaler the output
    // (else the colour), before it the colour - the image processed there (0.0.7's Vulkan NR: the colour's format;
    // a float colour with a UNORM output is linear light before the upscaler)
    const uint32_t authority = (f.output ? f.output : f.color)->Resource.ImageViewInfo.Format;
    const bool says_hdr = f.flags_known && (f.flags & (kFlagIsHdr | kFlagAutoExposure)) != 0;
    w.hdr = says_hdr && vk_float(authority);
    w.hdr_before = says_hdr && vk_float((f.color ? f.color : f.output)->Resource.ImageViewInfo.Format);
    w.reset = f.reset;
    w.pre_exposure = f.pre_exposure;
    w.mv_x = std::isfinite(f.mv_x) && f.mv_x != 0.0f ? f.mv_x : 1.0f;
    w.mv_y = std::isfinite(f.mv_y) && f.mv_y != 0.0f ? f.mv_y : 1.0f;
    // (display-size vectors the image cannot hold: the part it holds, vk_guide_subrect - 0.0.7's GuideSubrect)
    const bool low_res_mv = !f.flags_known || (f.flags & kFlagMvLowRes) != 0;
    w.depth_rect = vk_guide_subrect(f.depth, f.subrect_w, f.subrect_h, get_uint(p, "DLSS.Input.Depth.Subrect.Base.X"),
                                    get_uint(p, "DLSS.Input.Depth.Subrect.Base.Y"));
    w.motion_rect = vk_guide_subrect(f.motion, low_res_mv ? f.subrect_w : f.out_w, low_res_mv ? f.subrect_h : f.out_h,
                                     get_uint(p, "DLSS.Input.MV.SubrectBase.X"), get_uint(p, "DLSS.Input.MV.SubrectBase.Y"));
    return w;
}
// As wilsjo2_before_ok: the colour at origin 0 and a valid active extent (`aw` x `ah`).
bool wilsjo2_vk_before_ok(const VkFrame& f, uint32_t* aw, uint32_t* ah) {
    const auto& ci = f.color->Resource.ImageViewInfo;
    const uint32_t cw = ci.Width, ch = ci.Height;
    *aw = f.subrect_w; *ah = f.subrect_h;
    if (!*aw && !*ah) { *aw = cw; *ah = ch; }
    const bool active_ok = cw >= 1 && ch >= 1 && cw <= 16384 && ch <= 16384 && *aw && *ah && *aw <= cw && *ah <= ch;
    // the colour is read through its view (copy_in): any mip level, array layer or level count
    return !f.color_x && !f.color_y && active_ok;
}

// A RenoDX layer on `target` (GENERAL, in place), as run_renodx does on D3D12.
void run_renodx_vk(VkCommandBuffer cmd, VkLayerState& ls, const Layer& layer, int placement, const VkFrame& f,
                   const nr::dlssnr::VkImageRef& target, uint32_t w, uint32_t h) {
    Feature*& feature = ls.nr[placement];
    if (!feature) {
        int rc = 0;
        feature = nr::dlssnr::create_vk_asi(w, h, &rc);
        if (!feature) { log("[nr] Vulkan route: NR feature could not be made (0x%08X)", unsigned(rc)); return; }
    }
    // the image processed decides (before the upscaler the colour, as 0.0.7's Vulkan NR)
    const bool hdr = f.flags_known && (f.flags & (kFlagIsHdr | kFlagAutoExposure)) != 0 &&
                     vk_float((placement == 1 ? f.color : f.output)->Resource.ImageViewInfo.Format);
    namespace route = nr::pe::renodx_route;
    route::note_pass(f.flags_known ? f.flags : 0u, f.exposure_scale, f.render_w, f.render_h, f.out_w, f.out_h);
    route::Frame rf;
    rf.before_upscale = placement == 1;
    rf.pre_exposure = f.pre_exposure;
    const bool exposure = f.exposure && f.exposure->Type == NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW &&
                          f.exposure->Resource.ImageViewInfo.Image;
    if (exposure) {   // read in GENERAL by the RenoDX stage: moved there for the call, and back
        const auto& e = f.exposure->Resource.ImageViewInfo;
        rf.vk_exposure = uint64_t(reinterpret_cast<uintptr_t>(e.Image));
        rf.vk_exposure_format = e.Format; rf.vk_exposure_width = e.Width; rf.vk_exposure_height = e.Height;
        vk_barrier(cmd, reinterpret_cast<VkImage>(e.Image), f.exposure_layout, VK_IMAGE_LAYOUT_GENERAL);
    }
    route::begin_run(rf);
    route::note_encode(hdr, w, h);
    {
        route::CallStyle style(true);
        route::CallSettings settings(&layer.cfg.renodx.values);
        route::CallNetwork network(layer.network);
        route::CallLayer number(row_number(layer));
        nr::dlssnr::evaluate_vk_asi(cmd, feature, target, target,
                                    vk_ref(f.depth, f.depth_layout, false, f.depth_usage),
                                    vk_ref(f.motion, f.motion_layout, false, f.motion_usage), w, h, f.depth_rect,
                                    f.motion_rect, (f.flags & kFlagDepthInverted) ? 1 : 0, f.reset || hold_released(row_number(layer)) ? 1 : 0,
                                    nr::dlssnr::Controls6{}, f.mv_x, f.mv_y);
    }
    route::end_run();
    if (exposure)
        vk_barrier(cmd, reinterpret_cast<VkImage>(f.exposure->Resource.ImageViewInfo.Image), VK_IMAGE_LAYOUT_GENERAL,
                   f.exposure_layout);
}

// The layers of the NR chain by placement for one evaluate (as hk_evaluate sorts them on D3D12): the feature's layer
// states matched to the chain first (as match_layers). `who` names the games, for NR Control's notes.
struct VkPlan {
    std::shared_ptr<const Chain> chain;   // the chain the pointers below are into, held for the whole evaluate
    std::vector<const Layer*> before, after;
    bool before_renodx = false;
    bool present = false;                 // the chain has Present layers (the route's Present path runs them)
    bool present_takes = false;           // and that path is alive: they run after this evaluate (D3D12's present_takes)
    uint32_t w2_aw = 0, w2_ah = 0;
};
struct VkPresentPath;
bool vk_present_alive(const VkPresentPath& P);
// `present`: the route's Present path (its layers are left to it; while it has not run, their rows say what they wait
// for).
void plan_layers_vk(VkTracked& t, const VkFrame& f, bool nr_on, const char* who, VkPlan& plan, const VkPresentPath& present) {
    plan.chain = chain_now();
    const std::shared_ptr<const Chain>& chain = plan.chain;
    if (t.chain_version != chain->version) {   // as match_layers
        t.chain_version = chain->version;
        for (auto it = t.layers.begin(); it != t.layers.end();) {
            const bool keep = std::any_of(chain->layers.begin(), chain->layers.end(), [&](const Layer& l) {
                return l.cfg.number == it->first && l.cfg.processing == it->second.processing;
            });
            if (keep) { ++it; continue; }
            vk_layer_release(it->second);
            it = t.layers.erase(it);
        }
    }
    // The layers by placement, as on D3D12 (hk_evaluate): before Ray Reconstruction too; a wilsjo2 layer before only
    // where its own conditions allow, and with an output subrect away from the origin not at all. Present
    // layers run at the game's present only (never here).
    const bool w2_before_ok = wilsjo2_vk_before_ok(f, &plan.w2_aw, &plan.w2_ah);
    for (const Layer& l : chain->layers) {
        const bool wilsjo2 = l.cfg.processing == nr::pe::Processing::kWilsjo2;
        if (present_layer(l)) {
            plan.present = true;
            if (vk_present_alive(present)) plan.present_takes = true;
            else layer_note(l, 2, 3, "waits for a Present of the game's");
            continue;
        }
        if (wilsjo2 && (f.out_x || f.out_y)) {
            layer_note(l, 0, 3, "the output is a region away from the corner");
            continue;
        }
        const bool want_before = l.cfg.placement == nr::pe::Placement::kBefore;
        const bool b = want_before && (!wilsjo2 || w2_before_ok);
        (b ? plan.before : plan.after).push_back(&l);
        plan.before_renodx |= b && !wilsjo2;
        const char* note = want_before && !b ? "after the upscaler: this frame does not allow before" : "";
        layer_note(l, b ? 1u : 0u, wilsjo2 && !nr_on ? 3u : 1u, wilsjo2 && !nr_on ? "NR is off (toggle key)" : note);
    }
}

// ---- [Inspect] on the Vulkan and D3D11 routes (as inspect_copy / inspect_show on D3D12) -----------------------------
// `frame` (GENERAL, whole, w x h) copied into `im` (made in its format and size; GENERAL after).
bool vk_inspect_copy(VkCommandBuffer cmd, OwnImage& im, VkImage frame, VkFormat format, uint32_t w, uint32_t h) {
    if (!ensure_image(im, format, w, h)) return false;
    vk_barrier(cmd, frame, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    vk_barrier(cmd, im.image, im.layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vk_copy(cmd, frame, 0, 0, im.image, 0, 0, w, h);
    vk_barrier(cmd, frame, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    vk_barrier(cmd, im.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    im.layout = VK_IMAGE_LAYOUT_GENERAL;
    return true;
}
// [Inspect] HoldFrame before the chosen layer runs on `frame` (GENERAL, whole), as hold_frame on D3D12.
void vk_hold_frame(VkCommandBuffer cmd, const nr::pe::InspectConfig& ic, VkTracked& t, VkImage frame, VkFormat format,
                   uint32_t w, uint32_t h, int row) {
    if (!ic.hold) {
        if (t.held) g_hold_released.insert(t.hold_layer);
        t.held = false;
        return;
    }
    const ULONGLONG now = GetTickCount64();
    if (t.held && (t.hold_layer != row || t.hold.format != format || t.hold.width != w ||
                   t.hold.height != h)) {   // another layer, a pause, a new size: the old one back to live
        g_hold_released.insert(t.hold_layer);
        t.held = false;
    }
    t.hold_layer = row;
    t.hold_ms = now;
    if (!t.held) { t.hold_inputs_ready = false; t.held = vk_inspect_copy(cmd, t.hold, frame, format, w, h); return; }
    vk_barrier(cmd, t.hold.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    vk_barrier(cmd, frame, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vk_copy(cmd, t.hold.image, 0, 0, frame, 0, 0, w, h);
    vk_barrier(cmd, frame, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    vk_barrier(cmd, t.hold.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
}

bool vk_hold_inputs(VkCommandBuffer cmd, VkTracked& t, VkFrame& f, nr::asi::Wilsjo2VkFrame& wf) {
    if (!t.held) return false;
    if (!t.hold_inputs_ready) {
        const NVSDK_NGX_Resource_VK* src[] = {f.depth, f.motion, f.exposure};
        const VkImageLayout layout[] = {f.depth_layout, f.motion_layout, f.exposure_layout};
        for (auto& im : t.hold_guides) retire_image(im);
        for (int i = 0; i < 3; ++i) if (src[i] && src[i]->Resource.ImageViewInfo.Image) {
            const auto& d = src[i]->Resource.ImageViewInfo;
            if (!ensure_image(t.hold_guides[i], VkFormat(d.Format), d.Width, d.Height, true)) { t.held = false; return false; }
        }
        for (int i = 0; i < 3; ++i) if (t.hold_guides[i].image) {
            auto& dst = t.hold_guides[i];
            const auto& d = src[i]->Resource.ImageViewInfo;
            const auto image = reinterpret_cast<VkImage>(d.Image);
            const VkImageAspectFlags aspect = depth_format(dst.format) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
            VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            b.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT; b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            b.oldLayout = layout[i]; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.image = image;
            const bool stencil = dst.format == VK_FORMAT_D16_UNORM_S8_UINT || dst.format == VK_FORMAT_D24_UNORM_S8_UINT ||
                                 dst.format == VK_FORMAT_D32_SFLOAT_S8_UINT;
            b.subresourceRange = {aspect | (stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0u), 0, 1, 0, 1};
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
            VkImageMemoryBarrier out = b; out.srcAccessMask = 0; out.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            out.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; out.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; out.image = dst.image;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &out);
            VkImageCopy copy{}; copy.srcSubresource = copy.dstSubresource = {aspect, 0, 0, 1}; copy.extent = {dst.width, dst.height, 1};
            vkCmdCopyImage(cmd, image, b.newLayout, dst.image, out.newLayout, 1, &copy);
            b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT; b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            std::swap(b.oldLayout, b.newLayout);
            out.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; out.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            out.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; out.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            const VkImageMemoryBarrier back[] = {b, out};
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 2, back);
            dst.layout = out.newLayout;
        }
        t.hold_input = f; t.hold_w2 = wf;
        auto& hf = t.hold_input; auto& hw = t.hold_w2;
        hf.depth = t.hold_guides[0].image ? &t.hold_guides[0].ngx : nullptr;
        hf.motion = t.hold_guides[1].image ? &t.hold_guides[1].ngx : nullptr;
        hf.exposure = t.hold_guides[2].image ? &t.hold_guides[2].ngx : nullptr;
        hf.depth_layout = hf.motion_layout = hf.exposure_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        nr::asi::Wilsjo2VkImage* wi[] = {&hw.depth, &hw.motion, &hw.exposure};
        for (int i = 0; i < 3; ++i) {
            const auto& im = t.hold_guides[i];
            *wi[i] = {im.image, im.format, im.width, im.height, im.layout,
                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
        }
        hf.depth_usage = hf.motion_usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        t.hold_inputs_ready = true;
    }
    const auto* color = f.color; const auto* output = f.output;
    f = t.hold_input; wf = t.hold_w2; f.color = color; f.output = output;
    t.hold_input.reset = t.hold_w2.reset = false;
    return true;
}

nr::asi::Wilsjo2VkImage w2_of(const OwnImage& im) {
    nr::asi::Wilsjo2VkImage g;
    g.image = im.image; g.format = im.format; g.width = im.width; g.height = im.height; g.layout = VK_IMAGE_LAYOUT_GENERAL;
    g.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
              VK_IMAGE_USAGE_TRANSFER_DST_BIT;   // ensure_image's (not a guide's)
    return g;
}
// After the compared layers: `frame` (GENERAL) shown as [Inspect] asks - the frame before them (ApplyModel off), or
// the comparison of before (t.inspect_off, taken by vk_inspect_copy) and now.
void vk_inspect_show(VkCommandBuffer cmd, const nr::pe::InspectConfig& ic, VkTracked& t, VkImage frame, VkFormat format,
                     uint32_t w, uint32_t h) {
    OwnImage& off = t.inspect_off;
    if (!off.image || off.format != format || off.width != w || off.height != h) return;
    if (!ic.apply_model) {
        vk_barrier(cmd, off.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        vk_barrier(cmd, frame, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vk_copy(cmd, off.image, 0, 0, frame, 0, 0, w, h);
        vk_barrier(cmd, frame, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
        vk_barrier(cmd, off.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
        return;
    }
    if (!vk_inspect_copy(cmd, t.inspect_on, frame, format, w, h)) return;
    nr::asi::Wilsjo2VkImage target;
    target.image = frame; target.format = format; target.width = w; target.height = h;
    target.layout = VK_IMAGE_LAYOUT_GENERAL;
    nr::asi::wilsjo2_inspect_vk(cmd, w2_of(off), w2_of(t.inspect_on), target, w, h, ic.compare, ic.split, ic.zoom,
                                ic.swap, true);
}

void run_after_one(VkCommandBuffer cmd, VkTracked& t2, const Layer& layer, const VkFrame& f,
                   const nr::asi::Wilsjo2VkFrame& wf, bool nr_on, VkImage output, bool whole);

// The layers after the upscaler on its output (f.output, in f.output_layout), in place, recorded into `cmd`.
// `ic`: [Inspect] (the comparison spans these layers when they are the last that run, or holds the chosen one).
// `final_here`: no layer runs at Present after these (the comparison of the final picture is theirs).
void run_after_vk(VkCommandBuffer cmd, VkTracked& t2, const std::vector<const Layer*>& after, const VkFrame& f,
                  const nr::asi::Wilsjo2VkFrame& wf, bool nr_on, const nr::pe::InspectConfig& ic, bool final_here) {
    // The layers read and write the output where it is (sampled, storage; transfer for the copies some make): where
    // its view or its usage does not allow that, they run on our copy of it, read through its view, written back
    // after them (0.0.7's after-SR NR ran on an image of its own too).
    const VkImageUsageFlags in_place = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!usable_in_place(f.output, f.output_usage, in_place)) {
        const auto& gi = f.output->Resource.ImageViewInfo;
        static bool said = false;
        if (!copy_in(cmd, f.output, f.output_layout, f.output_usage, 0, 0, gi.Width, gi.Height, t2.out_whole,
                     VK_IMAGE_LAYOUT_GENERAL, false, true)) {
            if (!said) { said = true; log("[nr] Vulkan route: the upscaler's output cannot be read here; the layers after it do not run"); }
            return;
        }
        if (!said) { said = true; log("[nr] Vulkan route: the layers after the upscaler run on a copy of its output (its view or usage does not allow them in place)"); }
        VkFrame g = f;
        g.output = &t2.out_whole.ngx;
        g.output_layout = VK_IMAGE_LAYOUT_GENERAL;
        g.output_usage = in_place;   // ours (ensure_image: storage, sampled, transfer both ways). Not 0: 0 is "not
                                     // known" (usable_in_place: sampled only) - the copy was taken for not in place
                                     // again, and the layers never ran (10-27, vkinputs mip1 / nosrc after the upscaler)
        run_after_vk(cmd, t2, after, g, wf, nr_on, ic, final_here);
        if (!copy_out(cmd, t2.out_whole, f.output, f.output_layout, f.output_usage, 0, 0, gi.Width, gi.Height)) {
            static bool said_back = false;
            if (!said_back) { said_back = true; log("[nr] Vulkan route: the layers' answer could not be written back into the upscaler's output"); }
        }
        return;
    }
    const auto& oi = f.output->Resource.ImageViewInfo;
    const VkImage output = reinterpret_cast<VkImage>(oi.Image);
    const bool whole = !f.out_x && !f.out_y && oi.Width == f.out_w && oi.Height == f.out_h;
    size_t ia = 0, ib = 0;
    const bool inspect = nr_on && inspect_span(ic, after, final_here, &ia, &ib) && f.output_layout == VK_IMAGE_LAYOUT_GENERAL;
    const long held = f.output_layout == VK_IMAGE_LAYOUT_GENERAL ? chosen_index(ic, after) : -1;
    const VkFrame live_frame = f; const auto live_wf = wf;
    for (size_t k = 0; k < after.size(); ++k) {
        VkFrame f = live_frame; auto wf = live_wf;
        const Layer* l = after[k];
        if (long(k) == held) vk_hold_frame(cmd, ic, t2, output, VkFormat(oi.Format), oi.Width, oi.Height, row_number(*l));
        if (long(k) == held && ic.hold) vk_hold_inputs(cmd, t2, f, wf);
        if (inspect && k == ia) vk_inspect_copy(cmd, t2.inspect_off, output, VkFormat(oi.Format), oi.Width, oi.Height);
        run_after_one(cmd, t2, *l, f, wf, nr_on, output, whole);
        if (inspect && k == ib) vk_inspect_show(cmd, ic, t2, output, VkFormat(oi.Format), oi.Width, oi.Height);
    }
}

// One layer of run_after_vk.
void run_after_one(VkCommandBuffer cmd, VkTracked& t2, const Layer& layer, const VkFrame& f,
                   const nr::asi::Wilsjo2VkFrame& wf, bool nr_on, VkImage output, bool whole) {
    const auto& oi = f.output->Resource.ImageViewInfo;
    const Layer* l = &layer;
    {
        VkLayerState& ls = t2.layers[l->cfg.number];
        ls.processing = l->cfg.processing;
        if (l->cfg.processing == nr::pe::Processing::kWilsjo2) {
            // on the whole output image (the native extent)
            if (!nr_on) return;
            if (!ls.w2[0]) ls.w2[0] = nr::asi::wilsjo2_vk_owner();
            nr::pe::renodx_route::CallNetwork network(l->network);
            nr::pe::renodx_route::CallLayer number(row_number(*l));
            nr::asi::Wilsjo2VkImage target;
            target.image = output; target.format = VkFormat(oi.Format); target.width = oi.Width; target.height = oi.Height;
            target.layout = f.output_layout;
            // what the output is taken to allow wherever its usage is not known (vk_ref's, as the RenoDX layers read it)
            target.usage = VkImageUsageFlags(vk_ref(f.output, f.output_layout, true, f.output_usage).usage);
            nr::asi::Wilsjo2VkFrame wr = wf;
            wr.reset = wr.reset || hold_released(row_number(*l));
            nr::asi::wilsjo2_run_vk(cmd, *ls.w2[0], wr, l->w2, target, oi.Width, oi.Height, false);
        } else if (whole) {
            run_renodx_vk(cmd, ls, *l, 0, f, vk_ref(f.output, f.output_layout, true, f.output_usage), f.out_w, f.out_h);
        } else if (ensure_image(t2.out_copy, VkFormat(oi.Format), f.out_w, f.out_h)) {
            vk_barrier(cmd, output, f.output_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            vk_barrier(cmd, t2.out_copy.image, t2.out_copy.layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            vk_copy(cmd, output, f.out_x, f.out_y, t2.out_copy.image, 0, 0, f.out_w, f.out_h);
            vk_barrier(cmd, t2.out_copy.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
            run_renodx_vk(cmd, ls, *l, 0, f, own_ref(t2.out_copy), f.out_w, f.out_h);
            vk_barrier(cmd, t2.out_copy.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            vk_barrier(cmd, output, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            vk_copy(cmd, t2.out_copy.image, 0, 0, output, f.out_x, f.out_y, f.out_w, f.out_h);
            vk_barrier(cmd, output, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, f.output_layout);
            vk_barrier(cmd, t2.out_copy.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
            t2.out_copy.layout = VK_IMAGE_LAYOUT_GENERAL;
        }
    }
}

void run_before_one(VkCommandBuffer cmd, VkTracked& t, const Layer& layer, const VkFrame& f,
                    const nr::asi::Wilsjo2VkFrame& wf, bool nr_on, const nr::dlssnr::VkImageRef& copy, uint32_t rw,
                    uint32_t rh);

// The layers before the upscaler on `copy` (our image or texture with the colour's region, GENERAL, rw x rh).
// `ic`: [Inspect], `final_here`: no layer runs after these (the comparison of the final picture is theirs).
void run_before_vk(VkCommandBuffer cmd, VkTracked& t, const std::vector<const Layer*>& before, const VkFrame& f,
                   const nr::asi::Wilsjo2VkFrame& wf, bool nr_on, const nr::dlssnr::VkImageRef& copy, uint32_t rw,
                   uint32_t rh, const nr::pe::InspectConfig& ic, bool final_here) {
    const VkImage frame = reinterpret_cast<VkImage>(uintptr_t(copy.image));
    const VkFormat format = VkFormat(copy.format);
    size_t ia = 0, ib = 0;
    const bool inspect = nr_on && inspect_span(ic, before, final_here, &ia, &ib) &&
                         VkImageLayout(copy.layout) == VK_IMAGE_LAYOUT_GENERAL;
    const long held = VkImageLayout(copy.layout) == VK_IMAGE_LAYOUT_GENERAL ? chosen_index(ic, before) : -1;
    const VkFrame live_frame = f; const auto live_wf = wf;
    for (size_t k = 0; k < before.size(); ++k) {
        VkFrame f = live_frame; auto wf = live_wf;
        const Layer* l = before[k];
        if (long(k) == held) vk_hold_frame(cmd, ic, t, frame, format, rw, rh, row_number(*l));
        if (long(k) == held && ic.hold) vk_hold_inputs(cmd, t, f, wf);
        if (inspect && k == ia) vk_inspect_copy(cmd, t.inspect_off, frame, format, rw, rh);
        run_before_one(cmd, t, *l, f, wf, nr_on, copy, rw, rh);
        if (inspect && k == ib) vk_inspect_show(cmd, ic, t, frame, format, rw, rh);
    }
}

// One layer of run_before_vk.
void run_before_one(VkCommandBuffer cmd, VkTracked& t, const Layer& layer, const VkFrame& f,
                    const nr::asi::Wilsjo2VkFrame& wf, bool nr_on, const nr::dlssnr::VkImageRef& copy, uint32_t rw,
                    uint32_t rh) {
    const Layer* l = &layer;
    {
        VkLayerState& ls = t.layers[l->cfg.number];
        ls.processing = l->cfg.processing;
        if (l->cfg.processing == nr::pe::Processing::kRenoDX) {
            run_renodx_vk(cmd, ls, *l, 1, f, copy, rw, rh);
        } else if (nr_on) {
            if (!ls.w2[1]) ls.w2[1] = nr::asi::wilsjo2_vk_owner();
            nr::pe::renodx_route::CallNetwork network(l->network);
            nr::pe::renodx_route::CallLayer number(row_number(*l));
            nr::asi::Wilsjo2VkImage target;
            target.image = reinterpret_cast<VkImage>(uintptr_t(copy.image)); target.format = VkFormat(copy.format);
            target.width = rw; target.height = rh; target.layout = VkImageLayout(copy.layout);
            target.usage = VkImageUsageFlags(copy.usage);
            nr::asi::Wilsjo2VkFrame wr = wf;
            wr.reset = wr.reset || hold_released(row_number(*l));
            nr::asi::wilsjo2_run_vk(cmd, *ls.w2[1], wr, l->w2, target, rw, rh, true);
        }
    }
}

// ---- Present layers on the model's Vulkan session (D3D11 games through DXVK, native Vulkan games) ------------------
// The rules of present_layers (D3D12) on a frame of ours in GENERAL: the back buffer's (or swap-chain image's) copy,
// the layers placed at Present run on it in layer order, the copy goes back. Guides: copies of the game's depth and
// motion vectors taken at its evaluates (VkGuideSet), the newest fresh one of the frame's aspect for RenoDX (none:
// neutral guides, NRPresentGuides as on D3D12), the newest not yet used of exactly the frame's size for wilsjo2.
// These presents are the game's own (before OptiScaler's frame generation, where it has one): one game frame
// a present, so no cadence (D3D12's present_layers counts DLSS-G's generated presents; none reach these).
struct VkGuideSet {
    nr::dlssnr::VkImageRef depth, motion;   // as the present reads them (filled by the route before the run)
    nr::asi::Wilsjo2VkFrame w2;             // the wilsjo2 view (its depth / motion = the images above)
    Rect depth_rect, motion_rect;
    bool inverted = false, reset = false;
    float mv_x = 1.0f, mv_y = 1.0f;
    uint32_t render_w = 0, render_h = 0;    // the render extent, and whether the vectors are at it (HUD FIX's estimate)
    bool low_res_mv = true;
    uint64_t serial = 0;                    // 0: empty
    uint64_t captured_at = 0;               // presents seen at the capture
    ULONGLONG captured_ms = 0;
    uint32_t out_w = 0, out_h = 0;          // the upscaler's output
};
struct VkPresentLayer {
    uint32_t w = 0, h = 0;                  // RenoDX: its feature's extent
    bool real_guides = false;
    uint64_t consumed = 0, consumed_present = 0;
    bool w2_reset = true;
};
struct VkPresentPath {
    const char* who = "";                   // the games, for the log
    VkTracked vk;                           // the layers' features (nr[0], w2[0]) and the [Inspect] images
    std::map<int, VkPresentLayer> layers;
    VkGuideSet sets[kPresentGuideSets];
    uint64_t presents = 0, serial = 0, chain_version = 0;
    OwnImage frame, hudless;                // this swap chain's copies
    OwnImage stage;                         // the blits' way in and out of an *_SRGB swap chain (its UNORM twin)
    OwnImage xfer;                          // the copy bridge's picture (a swap chain not shared with NR's family)
    bool said_xfer = false;
    OwnImage linear, encoded;               // wilsjo2's HDR10 finished picture (as D3D12's)
    // HUD FIX (wilsjo2_hudfix.comp): the frame before the layers, the last one (the estimate's), the HUD's alpha and
    // confidence (RG16F), the counts (R32_UINT 4 x 1)
    OwnImage hud_back, hud_prev, hud_alpha, hud_stats;
    bool hud_prev_valid = false, hud_fresh = true;
    bool hud_said = false;
    uint32_t hud_parity = 0;
    uint64_t hud_estimated_at = ~0ull;   // the estimate's last Present: after a gap it starts afresh
    std::atomic<bool> wanted{false};        // the chain has Present layers (evaluates copy guides)
    std::atomic<uint64_t> served_ms{0};     // a present of the game's last reached the layers
    std::atomic<uint64_t> checked_ms{0};    // present_due's
    std::atomic<uint64_t> top_ms{0};        // served at the game's own call (the lower hook then leaves it)
    bool said_blit = false;                 // the blits' fallback said (vk_present_layers)
    bool unusable = false;
    explicit VkPresentPath(const char* games) : who(games) {}
};
VkPresentPath g_vk_present{"Vulkan games"};    // native Vulkan games (vk_present_layers), under g_lock
VkPresentPath g_d11_present{"D3D11 games"};    // D3D11 games (d11_present_layers), under g_lock

// As present_due (D3D12): while the chain has Present layers every present, else twice a second.
bool vk_present_due(VkPresentPath& P) {
    if (P.wanted.load()) return true;
    const uint64_t now = GetTickCount64();
    if (now - P.checked_ms.load() < 500) return false;
    P.checked_ms = now;
    return true;
}
// Whether this route's Present is alive (a present of the game's reached the layers within a second): the evaluates
// leave the Present layers' rows to it; else they say what the layers wait for.
bool vk_present_alive(const VkPresentPath& P) { return GetTickCount64() - P.served_ms.load() <= 1000; }

// The oldest set no present still reads (the route's command buffers are ordered on one queue: none is), for a new
// capture; its serial etc. set by the caller once the copies are recorded.
VkGuideSet& vk_guide_slot(VkPresentPath& P) {
    VkGuideSet* t = &P.sets[0];
    for (VkGuideSet& s : P.sets)
        if (s.serial < t->serial) t = &s;
    return *t;
}
void vk_guide_stamp(VkPresentPath& P, VkGuideSet& g, const VkFrame& f) {
    g.depth_rect = f.depth_rect;
    g.motion_rect = f.motion_rect;
    g.inverted = (f.flags & kFlagDepthInverted) != 0;
    g.reset = f.reset;
    g.mv_x = f.mv_x; g.mv_y = f.mv_y;
    g.render_w = f.render_w; g.render_h = f.render_h;
    g.low_res_mv = !f.flags_known || (f.flags & kFlagMvLowRes) != 0;
    g.serial = ++P.serial;
    g.captured_at = P.presents;
    g.captured_ms = GetTickCount64();
    g.out_w = f.out_w; g.out_h = f.out_h;
}

// The layers of the chain placed at Present (layer order); layers gone or switched processing let go of theirs.
std::vector<const Layer*> vk_present_plan(VkPresentPath& P, const std::shared_ptr<const Chain>& chain) {
    std::vector<const Layer*> layers;
    for (const Layer& l : chain->layers)
        if (present_layer(l)) layers.push_back(&l);
    P.wanted = !layers.empty();
    if (P.chain_version != chain->version) {
        P.chain_version = chain->version;
        for (auto it = P.vk.layers.begin(); it != P.vk.layers.end();) {
            const bool keep = std::any_of(layers.begin(), layers.end(), [&](const Layer* l) {
                return l->cfg.number == it->first && l->cfg.processing == it->second.processing;
            });
            if (keep) { ++it; continue; }
            vk_layer_release(it->second);
            P.layers.erase(it->first);
            it = P.vk.layers.erase(it);
        }
    }
    return layers;
}

struct VkPresentRun { const Layer* layer; uint32_t encoding; float white; };
// The back buffer's (or swap-chain image's) encodings, in DXGI's terms (as present_layers on D3D12): RenoDX's by present_encoding, wilsjo2's finished
// picture 1 SDR, 2 PQ, 3 scRGB, 0 refused - an 8-bit BGRA back buffer taken as its RGBA twin (the stage
// writes it through a view of its own format).
uint32_t w2_present_encoding(int space, bool space_set, DXGI_FORMAT format) {
    const int s = space_set ? space
                            : int(format == DXGI_FORMAT_R16G16B16A16_FLOAT ? DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709
                                                                          : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
    const bool b8or10 = format == DXGI_FORMAT_R8G8B8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM ||
                        format == DXGI_FORMAT_R10G10B10A2_UNORM;
    return s == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709 ? (format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 3u : 0u)
           : s == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ? (b8or10 ? 2u : 0u)
           : s == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709 ? (b8or10 ? 1u : 0u) : 0u;
}

// The runs for `layers` on a back buffer of this encoding (rows of the refused ones say so).
std::vector<VkPresentRun> present_runs(const std::vector<const Layer*>& layers, int space, bool space_set,
                                       DXGI_FORMAT format, float windows_white, bool plain, bool* unusable) {
    std::vector<VkPresentRun> runs;
    const uint32_t w2e = plain ? w2_present_encoding(space, space_set, format) : 0u;
    for (const Layer* l : layers) {
        if (!renodx_layer(*l)) {
            if (w2e) runs.push_back({l, w2e, 203.0f});
            else layer_note(*l, 2, 3, "Present cannot serve this swap chain's colour format (wilsjo2)");
            continue;
        }
        const nr::renodx::Settings& rs = l->cfg.renodx.values;
        const uint32_t e = plain ? present_encoding(rs, space, space_set, format) : 0u;
        if (!e) {
            *unusable = true;
            layer_note(*l, 2, 3, "Present cannot serve this swap chain's colour format");
            continue;
        }
        runs.push_back({l, e, present_white_of(rs, windows_white)});
    }
    return runs;
}

// `frame` (ours, GENERAL, w x h, `format`): the runs on it in order, recorded into `cmd`. True when a layer changed it.
// HUD FIX on these routes: the estimate (no HUD-less frame of the game's, no capture before its HUD here) - Mode Auto
// or Estimated; Game / Captured run nothing (NR Control greys them out with the reason). 0: off.
uint32_t vk_hud_source(const nr::pe::HudFixConfig& h) { return h.mode == 1 || h.mode == 4 ? 4u : 0u; }
// This Present's HUD-less frame (HUD FIX's capture, nr_asi_hudcap.hpp; image null: none): the layers run on it and the
// composition is against it (source 2); Auto keeps the estimate's state too.
struct VkHudless {
    nr::asi::Wilsjo2VkImage image;   // its layout as handed (copied from, left so)
    bool estimate_too = false;
};

// The frame before the layers kept for HUD FIX (the images made, the alpha cleared when new).
bool vk_hud_begin(VkCommandBuffer cmd, VkPresentPath& P, VkImage frame, VkFormat format, uint32_t w, uint32_t h,
                  const VkHudless& hl) {
    if (!P.hud_alpha.image || P.hud_alpha.width != w || P.hud_alpha.height != h) P.hud_fresh = true;
    if ((!hl.image.image || hl.estimate_too) && P.hud_estimated_at + 1 != P.presents) {   // after a gap: afresh
        P.hud_fresh = true;
        P.hud_prev_valid = false;
    }
    if (P.hud_back.format != format || P.hud_back.width != w || P.hud_back.height != h) P.hud_prev_valid = false;
    if (!ensure_image(P.hud_back, format, w, h) || !ensure_image(P.hud_prev, format, w, h) ||
        !ensure_image(P.hud_alpha, VK_FORMAT_R16G16B16A16_SFLOAT, w, h) ||
        !ensure_image(P.hud_stats, VK_FORMAT_R32_UINT, hud_stats_w(w), hud_stats_h(h))) {
        if (!P.hud_said) { P.hud_said = true; log("[nr] HUD FIX (%s): its images could not be made; it does not run", P.who); }
        return false;
    }
    for (OwnImage* im : {&P.hud_back, &P.hud_prev, &P.hud_alpha, &P.hud_stats})
        if (im->layout != VK_IMAGE_LAYOUT_GENERAL) {
            vk_barrier(cmd, im->image, im->layout, VK_IMAGE_LAYOUT_GENERAL);
            im->layout = VK_IMAGE_LAYOUT_GENERAL;
        }
    const auto copy = [&](VkImage from, VkImage to) {
        vk_barrier(cmd, from, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        vk_barrier(cmd, to, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vk_copy(cmd, from, 0, 0, to, 0, 0, w, h);
        vk_barrier(cmd, to, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
        vk_barrier(cmd, from, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    };
    copy(frame, P.hud_back.image);
    if (!P.hud_prev_valid) { copy(frame, P.hud_prev.image); P.hud_prev_valid = true; }   // the first: nothing moved
    if (hl.image.image) {   // the layers run on the frame without its HUD
        const VkImageLayout at = hl.image.layout;
        vk_barrier(cmd, hl.image.image, at, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        vk_barrier(cmd, frame, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vk_copy(cmd, hl.image.image, 0, 0, frame, 0, 0, w, h);
        vk_barrier(cmd, frame, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
        vk_barrier(cmd, hl.image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, at);
    }
    return true;
}
// After the layers: the composition into `frame`, then this frame becomes the estimate's last.
void vk_hud_end(VkCommandBuffer cmd, VkPresentPath& P, const nr::pe::HudFixConfig& hud, VkImage frame, VkFormat format,
                uint32_t w, uint32_t h, const VkHudless& hl, const VkGuideSet* guides, uint32_t multiplier) {
    nr::asi::HudFixVkImages im;
    // the estimate with the game's motion vectors (as on D3D12): the guides this Present reads, their units in output
    // pixels per present
    if ((!hl.image.image || hl.estimate_too) && guides && guides->motion.image && guides->out_w && guides->render_w) {
        const VkGuideSet& g = *guides;
        const float sx = g.low_res_mv ? float(g.out_w) / float(g.render_w) : 1.f;
        const float sy = g.low_res_mv ? float(g.out_h) / float(g.render_h) : 1.f;
        const float per = 1.0f / float(std::max(multiplier, 1u));
        im.motion.image = reinterpret_cast<VkImage>(g.motion.image);
        im.motion.format = VkFormat(g.motion.format);
        im.motion.width = g.motion.width; im.motion.height = g.motion.height;
        im.motion.layout = VkImageLayout(g.motion.layout);
        im.motion_scale_x = g.mv_x * sx * per * float(w) / float(g.out_w);
        im.motion_scale_y = g.mv_y * sy * per * float(h) / float(g.out_h);
        im.motion_x = g.motion_rect.x; im.motion_y = g.motion_rect.y;
        im.motion_step_x = float(g.motion_rect.width) / float(w);
        im.motion_step_y = float(g.motion_rect.height) / float(h);
    }
    im.back = w2_of(P.hud_back);
    im.previous = w2_of(P.hud_prev);
    im.alpha = w2_of(P.hud_alpha);
    im.stats = w2_of(P.hud_stats);
    im.frame.image = frame; im.frame.format = format; im.frame.width = w; im.frame.height = h;
    im.frame.layout = VK_IMAGE_LAYOUT_GENERAL;
    if (hl.image.image) im.hudless = hl.image;
    const uint32_t source = hl.image.image ? 2u : 4u;
    const bool estimate = source == 4 || hl.estimate_too;
    if (!nr::asi::wilsjo2_hudfix_vk(cmd, im, w, h, source, hud.show_mask, P.hud_fresh, P.hud_parity, hl.estimate_too)) {
        if (hl.image.image) {   // the layers ran on the copy without the HUD: the frame as the game drew it goes back
            vk_barrier(cmd, P.hud_back.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            vk_barrier(cmd, frame, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            vk_copy(cmd, P.hud_back.image, 0, 0, frame, 0, 0, w, h);
            vk_barrier(cmd, frame, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
            vk_barrier(cmd, P.hud_back.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
        }
        nr::pe::control::hud_facts("nothing this frame: its composition could not be recorded", nullptr, nullptr, nullptr);
        return;
    }
    if (estimate) {
        P.hud_fresh = false;
        P.hud_parity ^= 1u;
        P.hud_estimated_at = P.presents;
    }
    // the roles swapped instead of the copy (as on D3D12); both rest in GENERAL
    std::swap(P.hud_back, P.hud_prev);
}

bool vk_present_run(VkCommandBuffer cmd, VkPresentPath& P, const std::shared_ptr<const Chain>& chain,
                    const std::vector<VkPresentRun>& runs, VkImage frame, VkFormat format, uint32_t w, uint32_t h,
                    const VkHudless& hudless = VkHudless{}) {
    namespace route = nr::pe::renodx_route;
    // HUD FIX: the frame before the layers kept; the composition after the last of them (before a final-picture
    // comparison shows it)
    bool shot_chain = false;
    if (nr::pe::screenshot::wants_final() && !runs.empty())
        nr::dlssnr::with_vk_session([&](nr::pe::Session& s) {
            const uint8_t hdr = runs.front().encoding == 2 ? 2 : runs.front().encoding == 1 ? 0 : 1;
            shot_chain = s.capture_begin(cmd, frame, format, w, h, VK_IMAGE_LAYOUT_GENERAL, hdr, "present");
        });
    const bool hud = (vk_hud_source(chain->hud) != 0 || hudless.image.image) && !runs.empty() &&
                     vk_hud_begin(cmd, P, frame, format, w, h, hudless);
    const ULONGLONG now = GetTickCount64();
    const auto fresh = [&](const VkGuideSet& gs) {
        return gs.serial && aspect_matches(gs.out_w, gs.out_h, w, h) &&
               (P.presents - gs.captured_at <= kPresentGuideMaxAge || now - gs.captured_ms <= kPresentGuideMaxAgeMs);
    };
    VkGuideSet* guides = nullptr;
    for (VkGuideSet& gs : P.sets)
        if (fresh(gs) && (!guides || gs.serial > guides->serial)) guides = &gs;
    nr::dlssnr::VkImageRef target;
    target.image = uint64_t(reinterpret_cast<uintptr_t>(frame));
    target.format = uint32_t(format); target.width = w; target.height = h;
    target.layout = uint32_t(VK_IMAGE_LAYOUT_GENERAL);
    target.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                   VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    nr::asi::Wilsjo2VkImage w2_target;
    w2_target.image = frame; w2_target.format = format; w2_target.width = w; w2_target.height = h;
    w2_target.layout = VK_IMAGE_LAYOUT_GENERAL;
    w2_target.usage = VkImageUsageFlags(target.usage);
    std::vector<const Layer*> run_layers;
    for (const VkPresentRun& r : runs) run_layers.push_back(r.layer);
    size_t ia = 0, ib = 0;
    const bool inspect = inspect_span(chain->inspect, run_layers, true, &ia, &ib);
    const long held = chosen_index(chain->inspect, run_layers);
    if (held < 0 && P.vk.held) { g_hold_released.insert(P.vk.hold_layer); P.vk.held = false; }
    bool ran = false;
    for (size_t k = 0; k < runs.size(); ++k) {
        VkPresentRun run = runs[k];
        const bool holding = long(k) == held && chain->inspect.hold;
        const Layer& layer = *run.layer;
        if (long(k) == held) vk_hold_frame(cmd, chain->inspect, P.vk, frame, format, w, h, row_number(layer));
        auto& hi = P.vk;
        if (holding && hi.held) {
            if (!hi.hold_inputs_ready) { hi.hold_encoding = run.encoding; hi.hold_white = run.white; }
            else { run.encoding = hi.hold_encoding; run.white = hi.hold_white; }
        }
        VkGuideSet held_set;
        const auto frozen = [&](VkGuideSet* live, nr::asi::Wilsjo2VkFrame wf) -> VkGuideSet* {
            if (!holding || !hi.held) return live;
            NVSDK_NGX_Resource_VK resources[3] = {};
            VkFrame f;
            const nr::asi::Wilsjo2VkImage images[] = {wf.depth, wf.motion, wf.exposure};
            for (int i = 0; i < 3; ++i) {
                auto& r = resources[i]; r.Type = NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW;
                auto& d = r.Resource.ImageViewInfo;
                d.Image = reinterpret_cast<void*>(images[i].image); d.Format = images[i].format;
                d.Width = images[i].width; d.Height = images[i].height;
            }
            f.depth = &resources[0]; f.motion = &resources[1]; f.exposure = &resources[2];
            f.depth_layout = wf.depth.layout; f.motion_layout = wf.motion.layout; f.exposure_layout = wf.exposure.layout;
            f.depth_rect = live ? live->depth_rect : Rect{0, 0, w, h};
            f.motion_rect = live ? live->motion_rect : Rect{0, 0, w, h};
            f.flags = (!live || live->inverted) ? kFlagDepthInverted : 0u;
            f.mv_x = live ? live->mv_x : 1.f; f.mv_y = live ? live->mv_y : 1.f;
            f.reset = live && live->reset;
            if (!vk_hold_inputs(cmd, hi, f, wf)) return live;
            held_set.w2 = wf; held_set.depth = own_ref(hi.hold_guides[0]); held_set.motion = own_ref(hi.hold_guides[1]);
            held_set.depth.layout = held_set.motion.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            held_set.depth_rect = f.depth_rect; held_set.motion_rect = f.motion_rect;
            held_set.inverted = (f.flags & kFlagDepthInverted) != 0; held_set.reset = f.reset;
            held_set.mv_x = f.mv_x; held_set.mv_y = f.mv_y; held_set.serial = live ? live->serial : 0;
            return &held_set;
        };

        if (inspect && k == ia) vk_inspect_copy(cmd, P.vk.inspect_off, frame, format, w, h);
        VkPresentLayer& pl = P.layers[layer.cfg.number];
        VkLayerState& ls = P.vk.layers[layer.cfg.number];
        ls.processing = layer.cfg.processing;
        [&] {
            if (!renodx_layer(layer)) {
                if (!nr::dlssnr::nr_on_now()) {   // NRToggleKey, as the layers at the upscaler take it
                    pl.w2_reset = true;
                    layer_note(layer, 2, 3, "NR is off (toggle key)");
                    return;
                }
                VkGuideSet* g = nullptr;
                for (VkGuideSet& gs : P.sets) {
                    if (!gs.serial || gs.serial <= pl.consumed || P.presents - gs.captured_at > 1u || gs.out_w != w ||
                        gs.out_h != h || (g && gs.serial < g->serial))
                        continue;
                    g = &gs;
                }
                if (!g && holding && hi.hold_inputs_ready) g = frozen(nullptr, hi.hold_w2);
                if (!g) {
                    if (pl.consumed && P.presents - pl.consumed_present <= 2u) return;   // one capture, one run
                    pl.w2_reset = true;
                    layer_note(layer, 2, 3, "waits for a game frame of the back buffer's size");
                    return;
                }
                pl.consumed = g->serial;
                pl.consumed_present = P.presents;
                nr::asi::Wilsjo2VkFrame wf = g->w2;
                wf.exposure = nr::asi::Wilsjo2VkImage{};
                wf.hdr = run.encoding != 1;
                wf.pre_exposure = 1.0f;
                if (auto* h = frozen(g, wf); h == &held_set) wf = h->w2;
                wf.reset = wf.reset || pl.w2_reset || hold_released(row_number(layer));
                nr::wilsjo2::Settings ws = layer.w2;
                if (wf.hdr) { ws.white_point_scale = 203.0f / 80.0f; ws.white_point_source = 0; }
                if (!ls.w2[0]) { ls.w2[0] = nr::asi::wilsjo2_vk_owner(); nr::asi::wilsjo2_vk_owner_at_present(*ls.w2[0]); }
                route::CallNetwork network(layer.network);
                route::CallLayer number(row_number(layer));
                layer_note(layer, 2, 3, "not recorded yet");   // before the run: the model's word during it stays (as on D3D12)
                bool done = false;
                if (run.encoding == 2) {   // PQ: through linear BT.709 scRGB and back
                    if (ensure_image(P.linear, VK_FORMAT_R16G16B16A16_SFLOAT, w, h) && ensure_image(P.encoded, format, w, h)) {
                        for (OwnImage* im : {&P.linear, &P.encoded})
                            if (im->layout != VK_IMAGE_LAYOUT_GENERAL) {
                                vk_barrier(cmd, im->image, im->layout, VK_IMAGE_LAYOUT_GENERAL);
                                im->layout = VK_IMAGE_LAYOUT_GENERAL;
                            }
                        if (nr::asi::wilsjo2_convert_vk(cmd, w2_target, nullptr, w2_of(P.linear), w, h, true) &&
                            nr::asi::wilsjo2_run_vk(cmd, *ls.w2[0], wf, ws, w2_of(P.linear), w, h, false) &&
                            nr::asi::wilsjo2_convert_vk(cmd, w2_of(P.linear), &w2_target, w2_of(P.encoded), w, h, false)) {
                            vk_barrier(cmd, P.encoded.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                            vk_barrier(cmd, frame, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                            vk_copy(cmd, P.encoded.image, 0, 0, frame, 0, 0, w, h);
                            vk_barrier(cmd, frame, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
                            vk_barrier(cmd, P.encoded.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
                            done = true;
                        }
                    }
                } else {
                    done = nr::asi::wilsjo2_run_vk(cmd, *ls.w2[0], wf, ws, w2_target, w, h, false);
                }
                pl.w2_reset = !done;
                if (!done) layer_note(layer, 2, 3, "its run could not be recorded (see the log)");
                ran |= done;
                return;
            }
            const nr::renodx::Settings& rs = layer.cfg.renodx.values;
            // NRPresentGuides: 2 never the game's; 1 required (none: this layer leaves the frame alone); 0 optional
            VkGuideSet* g = rs.present_guides == 2u ? nullptr : guides;
            auto hw = g ? g->w2 : nr::asi::Wilsjo2VkFrame{};
            hw.exposure = {};
            g = frozen(g, hw);
            if (!g && rs.present_guides == 1u) {
                layer_note(layer, 2, 3, "waits for the game's depth and motion (NRPresentGuides)");
                return;
            }
            const bool new_frame = g && g->serial != pl.consumed;
            if (!holding && rs.present_frames == 1u && g && !new_frame) return;   // NRPresentFrames = 1: once per game frame
            Feature*& feature = ls.nr[0];
            if (feature && (pl.w != w || pl.h != h)) { nr::dlssnr::release_vk(feature); feature = nullptr; }
            if (!feature) {
                int rc = 0;
                feature = nr::dlssnr::create_vk_asi(w, h, &rc);
                if (!feature) {
                    log("[nr] Present layers (%s): NR feature could not be made (0x%08X)", P.who, unsigned(rc));
                    layer_note(layer, 2, 3, "its NR feature could not be made (see the log)");
                    return;
                }
                pl.w = w; pl.h = h;
            }
            layer_note(layer, 2, 3, "not recorded yet");
            bool reset = (new_frame && g->reset) || hold_released(row_number(layer));   // the game's Reset: its frame's first present only
            if ((g != nullptr) != pl.real_guides) { pl.real_guides = g != nullptr; reset = true; }
            route::Frame rf;
            rf.present = run.encoding;
            rf.present_white_nits = run.white;
            route::begin_run(rf);
            route::note_encode(run.encoding == 3, w, h);
            bool recorded_run = false;
            {
                route::CallStyle style(true);
                route::CallSettings settings(&rs);
                route::CallNetwork network(layer.network);
                route::CallLayer number(row_number(layer));
                const Rect whole{0, 0, w, h};
                nr::dlssnr::evaluate_vk_asi(cmd, feature, target, target, g ? g->depth : nr::dlssnr::VkImageRef{},
                                            g ? g->motion : nr::dlssnr::VkImageRef{}, w, h, g ? g->depth_rect : whole,
                                            g ? g->motion_rect : whole, g ? (g->inverted ? 1 : 0) : 1, reset ? 1 : 0,
                                            nr::dlssnr::Controls6{}, g ? g->mv_x : 1.f, g ? g->mv_y : 1.f, &recorded_run);
            }
            route::end_run();
            if (g) pl.consumed = g->serial;
            ran |= recorded_run;
        }();
        if (hud && k + 1 == runs.size()) vk_hud_end(cmd, P, chain->hud, frame, format, w, h, hudless, guides, 1u);   // (no frame-generation scaling here, as the RenoDX runs)
        if (inspect && k == ib) vk_inspect_show(cmd, chain->inspect, P.vk, frame, format, w, h);
    }
    if (shot_chain) nr::dlssnr::with_vk_session([&](nr::pe::Session& s) {
        if (ran) s.capture_end(cmd, frame, VK_IMAGE_LAYOUT_GENERAL); else s.capture_abort();
    });
    return ran || inspect || held >= 0 || hud;   // a held frame is put back too
}

// ---- Present layers on native Vulkan games -------------------------------------------------------------------------
// At the game's vkQueuePresentKHR, before OptiScaler's own present work (its menu, its frame generation: the frames it
// generates are then made from the picture with the layers, as on D3D12). OptiScaler detours winevulkan's
// vkQueuePresentKHR (hooks/Vulkan_Hooks.cpp HookDevice) after this module hooked it at start (hook_present_beat), so
// its detour runs first; once it is there (seen at the first present that reaches ours), its detour gets one of ours
// in front: a thunk that calls vk_present_front and jumps on with the caller's own return address, which OptiScaler's
// detour reads (IsDxgiBackedCaller: DXVK's and vkd3d-proton's presents; FromFGOutput: its frame generation's). Without
// OptiScaler's detour the lowest hook runs the layers itself (vk_present_bottom). Only the game's presents: not
// DXVK's or vkd3d-proton's (D3D11 / D3D12 games: their DXGI Present serves them), not OptiScaler's or a frame
// generator's. Our command buffer waits for the present's semaphores, copies the swap-chain image into an image of
// ours, runs the layers (vk_present_run), copies it back and signals a semaphore of ours, which the present then
// waits for instead. The swap chains are seen being made at winevulkan (under OptiScaler: the real ones), which adds
// the transfer usage the copies need where the surface allows it (as ReShade does).
struct PresentSignals {
    VkDevice device{};
    std::vector<VkSemaphore> images;
};

struct SwapRec {
    VkDevice device = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkExtent2D extent{};
    VkImageUsageFlags usage = 0;
    std::shared_ptr<VkPresentPath> path;
    std::shared_ptr<PresentSignals> signals;
    VkSharingMode sharing = VK_SHARING_MODE_EXCLUSIVE;
    std::vector<uint32_t> families;
    std::vector<VkImage> images;   // as the game sees them (asked through the present path's own entry), at first use
};
std::mutex g_swap_lock;
std::unordered_map<VkSwapchainKHR, SwapRec> g_swaps;
VkSwapchainKHR g_swap_latest = VK_NULL_HANDLE;
PFN_vkCreateSwapchainKHR real_create_swapchain = nullptr;
PFN_vkDestroySwapchainKHR real_destroy_swapchain = nullptr;

VkResult present_receipt(PFN_vkQueuePresentKHR present, VkQueue q, const VkPresentInfoKHR* p) {
    static thread_local bool inside = false;
    if (inside || !p) return present(q, p);
    struct Inside { bool& v; Inside(bool& b) : v(b) { v = true; } ~Inside() { v = false; } } depth(inside);
    const VkDevice device = g_route_device.load();
    receipts::collect();
    if (!device || !receipts::watched(device, p->waitSemaphoreCount, p->pWaitSemaphores)) return present(q, p);
    bool enabled = false;
    { std::lock_guard<std::mutex> guard(g_devices_lock);
      for (const auto& d : g_devices) if (d.device == device) enabled = d.present_fences; }
    const VkSwapchainPresentFenceInfoEXT* supplied = nullptr;
    for (auto* b = static_cast<const VkBaseInStructure*>(p->pNext); b; b = b->pNext)
        if (b->sType == VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT)
            supplied = reinterpret_cast<const VkSwapchainPresentFenceInfoEXT*>(b);
    receipts::Token receipt;
    VkPresentInfoKHR copy = *p;
    VkSwapchainPresentFenceInfoEXT fences{VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT};
    std::vector<VkFence> handles;
    if (enabled && supplied && g_fence_hooks.load()) {
        for (uint32_t i = 0; i < supplied->swapchainCount; ++i) if (supplied->pFences[i]) {
            receipt = receipts::acquire(device, supplied->pFences[i]); break;
        }
    } else if (enabled && !supplied && p->swapchainCount) {
        receipt = receipts::acquire(device);
        if (receipt) {
            handles.resize(p->swapchainCount); handles[0] = receipt->fence;
            fences.pNext = p->pNext; fences.swapchainCount = p->swapchainCount; fences.pFences = handles.data();
            copy.pNext = &fences;
        }
    }
    const VkResult result = present(q, &copy);
    // Some present errors still enqueue waits. Keep an unsignalled receipt until device teardown.
    receipts::submitted(receipt);
    for (uint32_t i = 0; i < p->waitSemaphoreCount; ++i) receipts::used(device, p->pWaitSemaphores[i], receipt);
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL hk_create_swapchain(VkDevice device, const VkSwapchainCreateInfoKHR* ci,
                                                   const VkAllocationCallbacks* a, VkSwapchainKHR* out) {
    VkSwapchainCreateInfoKHR c = *ci;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    {
        std::lock_guard<std::mutex> guard(g_devices_lock);
        for (const SeenDevice& d : g_devices) if (d.device == device) physical = d.physical;
    }
    VkSurfaceCapabilitiesKHR caps{};
    static const auto surface_caps = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>(reinterpret_cast<void*>(
        GetProcAddress(GetModuleHandleW(L"winevulkan.dll"), "vkGetPhysicalDeviceSurfaceCapabilitiesKHR")));
    if (physical && surface_caps && surface_caps(physical, ci->surface, &caps) == VK_SUCCESS)
        c.imageUsage |= caps.supportedUsageFlags & (VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    VkResult r = real_create_swapchain(device, &c, a, out);
    if (r != VK_SUCCESS && c.imageUsage != ci->imageUsage) {   // as the game asked, rather than none
        c = *ci;
        r = real_create_swapchain(device, &c, a, out);
    }
    if (r == VK_SUCCESS && out && *out) {
        std::lock_guard<std::mutex> guard(g_swap_lock);
        SwapRec& rec = g_swaps[*out];
        rec = SwapRec{};
        rec.device = device; rec.format = c.imageFormat; rec.space = c.imageColorSpace; rec.extent = c.imageExtent;
        rec.usage = c.imageUsage; rec.sharing = c.imageSharingMode;
        if (c.queueFamilyIndexCount) rec.families.assign(c.pQueueFamilyIndices, c.pQueueFamilyIndices + c.queueFamilyIndexCount);
        rec.path = std::make_shared<VkPresentPath>("Vulkan games");
        rec.signals = std::make_shared<PresentSignals>(); rec.signals->device = device;
        g_swap_latest = *out;
    }
    return r;
}
void retire_present_path(VkPresentPath& P);
VKAPI_ATTR void VKAPI_CALL hk_destroy_swapchain(VkDevice device, VkSwapchainKHR sc, const VkAllocationCallbacks* a) {
    {
        std::lock_guard<std::mutex> route_guard(g_lock);
        std::lock_guard<std::mutex> guard(g_swap_lock);
        const auto it = g_swaps.find(sc);
        if (it != g_swaps.end()) {
            if (it->second.path) retire_present_path(*it->second.path);
            if (it->second.signals) for (VkSemaphore sem : it->second.signals->images) receipts::retire(device, sem);
        }
        g_swaps.erase(sc);
        if (g_swap_latest == sc) g_swap_latest = VK_NULL_HANDLE;
    }
    nr::pe::menu::vulkan_forget(device, sc);
    real_destroy_swapchain(device, sc, a);
}
void hook_swapchains(HMODULE m) {
    void* create = reinterpret_cast<void*>(GetProcAddress(m, "vkCreateSwapchainKHR"));
    void* destroy = reinterpret_cast<void*>(GetProcAddress(m, "vkDestroySwapchainKHR"));
    const bool ok = create && destroy &&
                    MH_CreateHook(create, reinterpret_cast<void*>(&hk_create_swapchain),
                                  reinterpret_cast<void**>(&real_create_swapchain)) == MH_OK &&
                    MH_CreateHook(destroy, reinterpret_cast<void*>(&hk_destroy_swapchain),
                                  reinterpret_cast<void**>(&real_destroy_swapchain)) == MH_OK &&
                    MH_EnableHook(create) == MH_OK && MH_EnableHook(destroy) == MH_OK;
    if (!ok) log("[nr] winevulkan.dll's swap-chain calls could not be hooked: no Present layers on Vulkan games");
}

// Whether a present call comes from the game (not from DXVK / vkd3d-proton, OptiScaler, this module, a frame
// generator or Streamline). The last module asked is kept (one call per present).
// A module left out by its name only as a frame generator or Streamline (ffx, fidelityfx, sl., nvngx, libxess) through
// which every present has come for 2 s - no present from the game itself meanwhile - is the game's present path (a
// game whose presents all go through Streamline's interposer, say): taken as the game's from then on, said once (0.0.7
// served every present).
std::atomic<ULONGLONG> g_game_present_ms{0}, g_wrapped_since_ms{0};
std::mutex g_wrapper_lock;
std::vector<HMODULE> g_wrappers;   // promoted (g_wrapper_lock)
bool game_caller(void* address) {
    HMODULE m = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCWSTR>(address), &m) || !m)
        return true;   // code outside any module (generated): the game's
    static thread_local HMODULE last = nullptr;
    static thread_local bool last_game = false, last_wrapper = false;
    const ULONGLONG now = GetTickCount64();
    const auto served = [&](bool game, bool wrapper) {
        if (game) { g_game_present_ms = now; g_wrapped_since_ms = 0; return true; }
        if (!wrapper) return false;
        {
            std::lock_guard<std::mutex> g(g_wrapper_lock);
            if (std::find(g_wrappers.begin(), g_wrappers.end(), m) != g_wrappers.end()) { g_game_present_ms = now; return true; }
        }
        ULONGLONG since = g_wrapped_since_ms.load();
        if (!since) { g_wrapped_since_ms = now; return false; }
        if (now - since < 2000 || now - g_game_present_ms.load() < 2000) return false;
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(m, path, MAX_PATH);
        {
            std::lock_guard<std::mutex> g(g_wrapper_lock);
            if (std::find(g_wrappers.begin(), g_wrappers.end(), m) == g_wrappers.end()) g_wrappers.push_back(m);
        }
        log("[nr] Vulkan presents: every present for 2 s came through %ls; taken as the game's present path", path);
        g_game_present_ms = now;
        return true;
    };
    if (m == last) return served(last_game, last_wrapper);
    bool game = m != g_opti && m != g_self && !in_translator(address), wrapper = false;
    if (game) {
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(m, path, MAX_PATH);
        std::wstring name = path;
        const size_t slash = name.find_last_of(L"\\/");
        if (slash != std::wstring::npos) name = name.substr(slash + 1);
        for (wchar_t& c : name) c = wchar_t(std::towlower(c));
        // by name too: DXVK's / vkd3d-proton's DLLs share their names with OptiScaler's proxy (in_translator finds the
        // first module of a name only)
        for (const wchar_t* not_game : {L"fidelityfx", L"ffx", L"sl.", L"nvngx", L"libxess", L"winevulkan", L"dxgi", L"d3d9",
                                        L"d3d10", L"d3d11", L"d3d12", L"dxvk", L"vkd3d"})
            if (name.find(not_game) != std::wstring::npos) game = false;
        if (!game) {   // a frame generator's or Streamline's only (not a translator's name): may be the game's path
            wrapper = true;
            for (const wchar_t* translator : {L"winevulkan", L"dxgi", L"d3d9", L"d3d10", L"d3d11", L"d3d12", L"dxvk", L"vkd3d"})
                if (name.find(translator) != std::wstring::npos) wrapper = false;
        }
    }
    last = m;
    last_game = game;
    last_wrapper = wrapper;
    return served(game, wrapper);
}

// Command buffers are reused after their submit fence. Present wait semaphores belong to swap-chain images:
// reacquiring that image and waiting for the game's rendering proves that its earlier presentation consumed them.
struct VkPresentRing {
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t family = ~0u;
    VkCommandPool pool = VK_NULL_HANDLE;
    static constexpr int kSlots = 8;
    VkCommandBuffer cmd[kSlots] = {};
    VkFence fence[kSlots] = {};
    uint64_t ticket[kSlots] = {};   // the submission number each slot's fence stands for (0: none)
    uint64_t submits = 0;
    int slot = 0;
    bool said_wait = false;
};
VkPresentRing g_vk_ring;   // g_lock
// A command buffer of ours recorded with the session's timing and not submitted: that timing is not read
// (Session::discard_timing).
void discard_session_timing() { nr::dlssnr::discard_vk_timing(); }
uint64_t vk_ring_submits() { return g_vk_ring.submits; }
bool vk_ring_done_through(uint64_t submission) {
    const VkPresentRing& R = g_vk_ring;
    if (!R.device) return true;   // no ring (made anew or gone after the device idled)
    for (int s = 0; s < VkPresentRing::kSlots; ++s)
        if (R.ticket[s] && R.ticket[s] <= submission && R.fence[s] &&
            vkWaitForFences(R.device, 1, &R.fence[s], VK_TRUE, 0) != VK_SUCCESS)   // not signalled yet
            return false;
    return true;
}
OwnImage g_vk_frame;       // the swap-chain image's copy the layers run on
OwnImage g_vk_hudless;     // HUD FIX's capture: the swap-chain image as it was before the game's HUD (GENERAL)
OwnImage g_vk_guides[kPresentGuideSets][2];   // [set][0 depth, 1 motion]: the evaluates' copies

// `device_going`: the device is destroyed next (vk_present_forget, after vk_route_teardown idled it) - the ring goes
// whatever its wait says, no handle of that device is kept.
void vk_ring_destroy(bool device_going = false) {
    VkPresentRing& R = g_vk_ring;
    if (!R.device) return;
    {   // its submissions done first (fences and command buffers in flight are not destroyed; at most 2 s)
        VkFence f[VkPresentRing::kSlots];
        uint32_t n = 0;
        for (int i = 0; i < VkPresentRing::kSlots; ++i)
            if (R.fence[i] && R.ticket[i]) f[n++] = R.fence[i];
        const VkResult w = !n ? VK_SUCCESS : test_fault("ring_wait") ? VK_TIMEOUT
                                                                       : vkWaitForFences(R.device, n, f, VK_TRUE, 2000000000ull);
        if (w != VK_SUCCESS && w != VK_ERROR_DEVICE_LOST && !device_going) {
            // not done in time: nothing of it destroyed (the ring stays; a new one is not made for another queue)
            static bool said = false;
            if (!said) { said = true; log("[nr] Present layers (Vulkan): our ring's work did not finish in 2 s; it is kept"); }
            return;
        }
        if (w != VK_SUCCESS) {   // a lost device's work never finishes: its objects may go (and must, if it is going)
            static bool said = false;
            if (!said) { said = true; log("[nr] Present layers (Vulkan): our ring's wait returned %d; the ring goes", int(w)); }
        }
    }
    for (int i = 0; i < VkPresentRing::kSlots; ++i) {
        if (R.fence[i]) vkDestroyFence(R.device, R.fence[i], nullptr);
    }
    if (R.pool) vkDestroyCommandPool(R.device, R.pool, nullptr);   // frees its command buffers
    const uint64_t submits = R.submits;   // the next ring numbers on from here (retired images compare against them)
    R = VkPresentRing{};
    R.submits = submits;
}
uint32_t queue_family(VkDevice device, VkQueue queue) {
    // the queues the game took (hk_get_queue), then the ones its vkCreateDevice asked for (each asked of the device:
    // only queues it was made with - vkGetDeviceQueue's rule), then NR's own queue
    uint32_t family = ~0u;
    if (!test_device_unseen() && recorded_family(device, queue, &family)) return family;
    {
        std::lock_guard<std::mutex> guard(g_devices_lock);
        for (const SeenDevice& d : g_devices) {
            if (test_device_unseen()) break;
            if (d.device != device) continue;
            for (size_t i = 0; i < d.families.size() && family == ~0u; ++i)
                for (uint32_t k = 0; k < (i < d.counts.size() ? d.counts[i] : 1u); ++k) {
                    VkQueue q = VK_NULL_HANDLE;
                    vkGetDeviceQueue(device, d.families[i], k, &q);
                    if (q == queue) { family = d.families[i]; break; }
                }
        }
    }
    if (family == ~0u && device && device == g_route_device.load() && queue == g_route_queue.load()) family = g_route_family.load();
    if (family != ~0u) record_queue(device, queue, family);
    return family;
}
// All NR work stays on the session's queue, so its graph arenas never change queue-family ownership.
bool vk_ring_for(VkDevice device, VkQueue queue) {
    VkPresentRing& R = g_vk_ring;
    if (R.device == device && R.queue == queue && R.pool) return true;
    const uint32_t family = queue_family(device, queue);
    if (family == ~0u) return false;
    if (R.device && R.family == family && R.device == device) { R.queue = queue; return true; }
    if (R.device) {   // another family: the old ring once it is idle (still busy: kept, and no layers this Present)
        vk_ring_destroy();
        if (R.device) return false;
    }
    R.device = device; R.queue = queue; R.family = family;
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = family;
    bool made = vkCreateCommandPool(device, &pci, nullptr, &R.pool) == VK_SUCCESS;
    if (made) {
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = R.pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = VkPresentRing::kSlots;
        made = vkAllocateCommandBuffers(device, &ai, R.cmd) == VK_SUCCESS;
    }
    for (int i = 0; made && i < VkPresentRing::kSlots; ++i) {
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        made = vkCreateFence(device, &fi, nullptr, &R.fence[i]) == VK_SUCCESS;
    }
    if (!made) {
        log("[nr] Present layers (Vulkan games): our command buffers could not be made on the present's queue family %u",
            family);
        vk_ring_destroy();
        return false;
    }
    return true;
}

// An exclusive swap-chain image presented on another family visits the NR family with paired ownership
// barriers. Only these bridge command buffers touch it on the present family; graph resources stay on NR's queue.
struct PresentBridge {
    VkDevice device{}; VkQueue queue{}; uint32_t family = ~0u;
    VkCommandPool pool{}; VkCommandBuffer cmd[2]{};
    VkSemaphore to_nr{}, from_nr{}; VkFence fence{};
};
std::map<std::pair<VkQueue, int>, PresentBridge> present_bridges;
void ownership(VkCommandBuffer cmd, VkImage image, uint32_t from, uint32_t to, bool acquire) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    b.srcQueueFamilyIndex = from; b.dstQueueFamilyIndex = to; b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.srcAccessMask = acquire ? 0u : VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = acquire ? VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT : 0u;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &b);
}
PresentBridge* prepare_bridge(VkDevice device, VkQueue queue, uint32_t family, uint32_t nr_family,
                              int slot, VkImage image) {
    auto& b = present_bridges[{queue, slot}];
    if (!b.pool) {
        b.device = device; b.queue = queue; b.family = family;
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; pi.queueFamilyIndex = family;
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 2;
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        const bool made = vkCreateCommandPool(device, &pi, nullptr, &b.pool) == VK_SUCCESS &&
                          (ai.commandPool = b.pool, vkAllocateCommandBuffers(device, &ai, b.cmd) == VK_SUCCESS) &&
                          vkCreateSemaphore(device, &si, nullptr, &b.to_nr) == VK_SUCCESS &&
                          vkCreateSemaphore(device, &si, nullptr, &b.from_nr) == VK_SUCCESS && !test_fault("bridge_make") &&
                          vkCreateFence(device, &fi, nullptr, &b.fence) == VK_SUCCESS;
        if (!made) {   // nothing half made is kept: the next present makes it all again
            if (b.fence) vkDestroyFence(device, b.fence, nullptr);
            if (b.to_nr) vkDestroySemaphore(device, b.to_nr, nullptr);
            if (b.from_nr) vkDestroySemaphore(device, b.from_nr, nullptr);
            if (b.pool) vkDestroyCommandPool(device, b.pool, nullptr);   // (its command buffers with it)
            present_bridges.erase({queue, slot});
            return nullptr;
        }
    }
    if (!b.fence || vkWaitForFences(device, 1, &b.fence, VK_TRUE, 2000000000ull) != VK_SUCCESS) return nullptr;
    if (!image) return &b;   // the copy bridge records its own (prepare_copy_bridge)
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    for (int i = 0; i < 2; ++i) {
        if (vkResetCommandBuffer(b.cmd[i], 0) != VK_SUCCESS || vkBeginCommandBuffer(b.cmd[i], &bi) != VK_SUCCESS) return nullptr;
        ownership(b.cmd[i], image, i ? nr_family : family, i ? family : nr_family, i != 0);
        if (vkEndCommandBuffer(b.cmd[i]) != VK_SUCCESS) return nullptr;
    }
    return &b;
}
// The copy bridge (a CONCURRENT swap chain that leaves NR's family out): `xfer` made shared by the present family and
// NR's (transfer only), at rest in GENERAL.
bool ensure_xfer_image(OwnImage& im, VkFormat format, uint32_t w, uint32_t h, uint32_t family, uint32_t nr_family) {
    // (shared by exactly these two families: another present family makes it again)
    if (im.image && im.format == format && im.width == w && im.height == h && im.shared[0] == family &&
        im.shared[1] == nr_family)
        return true;
    retire_image(im);
    const VkDevice dev = g_vk.dev.device;
    const uint32_t families[2] = {family, nr_family};
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D; ii.format = format; ii.extent = {w, h, 1}; ii.mipLevels = 1; ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (family != nr_family) {   // (one family - NR_TEST_PRESENT_XFER only: CONCURRENT needs two different ones)
        ii.sharingMode = VK_SHARING_MODE_CONCURRENT; ii.queueFamilyIndexCount = 2; ii.pQueueFamilyIndices = families;
    }
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    OwnImage n;
    if (test_fault("xfer_image") || vkCreateImage(dev, &ii, nullptr, &n.image) != VK_SUCCESS) return false;
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(dev, n.image, &req);
    uint32_t type = ~0u;
    for (uint32_t i = 0; i < g_vk.memory.memoryTypeCount; ++i)
        if ((req.memoryTypeBits & (1u << i)) && (g_vk.memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            type = i;
            break;
        }
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size; ai.memoryTypeIndex = type;
    {
        nr::vram::Scope tag(nr::vram::kCopies);
        if (type == ~0u || test_fault("image_alloc") || vkAllocateMemory(dev, &ai, nullptr, &n.memory) != VK_SUCCESS) {
            destroy_image(n);
            return false;
        }
    }
    if (vkBindImageMemory(dev, n.image, n.memory, 0) != VK_SUCCESS) { destroy_image(n); return false; }
    n.format = format; n.width = w; n.height = h;
    n.shared[0] = family; n.shared[1] = nr_family;
    im = n;
    return true;
}
// Its two command buffers on the present queue: the swap chain's picture into `xfer` before our work, back after it
PresentBridge* prepare_copy_bridge(VkDevice device, VkQueue queue, uint32_t family, int slot, VkImage image, OwnImage& xfer,
                                   uint32_t w, uint32_t h) {
    PresentBridge* b = prepare_bridge(device, queue, family, family, slot, VK_NULL_HANDLE);
    if (!b) return nullptr;
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    for (int i = 0; i < 2; ++i) {
        if (vkResetCommandBuffer(b->cmd[i], 0) != VK_SUCCESS || vkBeginCommandBuffer(b->cmd[i], &bi) != VK_SUCCESS) return nullptr;
        const VkCommandBuffer c = b->cmd[i];
        if (i == 0) {
            vk_barrier(c, image, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            vk_barrier(c, xfer.image, xfer.layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            vk_copy(c, image, 0, 0, xfer.image, 0, 0, w, h);
            vk_barrier(c, xfer.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
            vk_barrier(c, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        } else {
            vk_barrier(c, image, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            vk_barrier(c, xfer.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            vk_copy(c, xfer.image, 0, 0, image, 0, 0, w, h);
            vk_barrier(c, xfer.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
            vk_barrier(c, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        }
        if (vkEndCommandBuffer(c) != VK_SUCCESS) return nullptr;
    }
    xfer.layout = VK_IMAGE_LAYOUT_GENERAL;
    return b;
}
void destroy_present_bridges(VkDevice device) {
    for (auto i = present_bridges.begin(); i != present_bridges.end();) {
        auto& b = i->second;
        if (b.device != device) { ++i; continue; }
        if (b.fence) vkDestroyFence(device, b.fence, nullptr);
        if (b.to_nr) vkDestroySemaphore(device, b.to_nr, nullptr);
        if (b.from_nr) vkDestroySemaphore(device, b.from_nr, nullptr);
        if (b.pool) vkDestroyCommandPool(device, b.pool, nullptr);
        i = present_bridges.erase(i);
    }
}

// The swap chain's encoding in DXGI's terms, for present_encoding / w2_present_encoding: its format's DXGI twin and
// its colour space (always set on Vulkan).
DXGI_FORMAT dxgi_twin(VkFormat f) {
    switch (f) {
        case VK_FORMAT_R16G16B16A16_SFLOAT: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return DXGI_FORMAT_R10G10B10A2_UNORM;
        // the same encodings with red and blue the other way round (the frame is worked in its own format; the twin
        // only names the encoding): X11 / Wayland's usual 10-bit swap chains, as 0.0.7's Vulkan Present took them
        case VK_FORMAT_A2R10G10B10_UNORM_PACK32: return DXGI_FORMAT_R10G10B10A2_UNORM;
        case VK_FORMAT_R16G16B16A16_UNORM: return DXGI_FORMAT_R16G16B16A16_UNORM;
        case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case VK_FORMAT_A8B8G8R8_UNORM_PACK32: case VK_FORMAT_A8B8G8R8_SRGB_PACK32: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
        default: return DXGI_FORMAT_UNKNOWN;
    }
}
int dxgi_space(VkColorSpaceKHR s) {
    switch (s) {
        case VK_COLOR_SPACE_SRGB_NONLINEAR_KHR: return int(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
        case VK_COLOR_SPACE_HDR10_ST2084_EXT: return int(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
        case VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT: return int(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709);
        default: return -1;   // not served
    }
}
VkFormat unorm_twin(VkFormat f) {   // a storage-writable view of the same bytes
    switch (f) {
        case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
        case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
        case VK_FORMAT_A8B8G8R8_SRGB_PACK32: return VK_FORMAT_A8B8G8R8_UNORM_PACK32;
        default: return f;
    }
}
bool vk_format_has(VkFormat f, VkFormatFeatureFlags need) {
    VkFormatProperties fp{};
    vkGetPhysicalDeviceFormatProperties(g_vk.dev.physical, f, &fp);
    return (fp.optimalTilingFeatures & need) == need;
}
// The Present layers' frame where the swap chain's format (its UNORM twin) cannot be the layers' (not one they know, or
// not one our programs can store into): a wider one of the same meaning, the swap chain's picture taken in and given
// back by blits (0.0.7's Vulkan finished picture went through RGBA16F the same way) - 16-bit UNORM for UNORM formats
// (code values kept: a blit between UNORM formats converts the normalised value), 16-bit float for float ones;
// VK_FORMAT_UNDEFINED for anything else (SNORM, integer: not a picture of a known meaning).
VkFormat present_blit_format(VkFormat f) {
    switch (unorm_twin(f)) {
        case VK_FORMAT_R16G16B16A16_SFLOAT: case VK_FORMAT_B10G11R11_UFLOAT_PACK32: case VK_FORMAT_R32G32B32A32_SFLOAT:
        case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32:
            return VK_FORMAT_R16G16B16A16_SFLOAT;
        case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_A8B8G8R8_UNORM_PACK32:
        case VK_FORMAT_A2R10G10B10_UNORM_PACK32: case VK_FORMAT_A2B10G10R10_UNORM_PACK32: case VK_FORMAT_R16G16B16A16_UNORM:
        case VK_FORMAT_R5G6B5_UNORM_PACK16: case VK_FORMAT_B5G6R5_UNORM_PACK16: case VK_FORMAT_R5G5B5A1_UNORM_PACK16:
        case VK_FORMAT_B5G5R5A1_UNORM_PACK16: case VK_FORMAT_A1R5G5B5_UNORM_PACK16: case VK_FORMAT_R4G4B4A4_UNORM_PACK16:
        case VK_FORMAT_B4G4R4A4_UNORM_PACK16:
            return VK_FORMAT_R16G16B16A16_UNORM;
        default:
            return VK_FORMAT_UNDEFINED;
    }
}
void vk_blit(VkCommandBuffer cmd, VkImage src, VkImage dst, uint32_t w, uint32_t h) {   // TRANSFER_SRC -> TRANSFER_DST
    VkImageBlit b{};
    b.srcSubresource = b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    b.srcOffsets[1] = b.dstOffsets[1] = {int32_t(w), int32_t(h), 1};
    vkCmdBlitImage(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b,
                   VK_FILTER_NEAREST);
}

// NR_DEBUG_PRESENT_DUMP=<folder> (measurement only, as on D3D12): the swap-chain image as our command buffer leaves it
// (after the Present layers), copied to the CPU (the present waits for it) and written raw, rows packed, p_NNNNN.raw;
// 60 presents from NR_DEBUG_PRESENT_FROM on.
struct VkDump {
    std::string folder;
    uint64_t from = 0, seen = 0, written = 0;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    void* mapped = nullptr;
    VkDump() {
        char b[MAX_PATH] = {};
        if (GetEnvironmentVariableA("NR_DEBUG_PRESENT_DUMP", b, MAX_PATH)) folder = b;
        char f[32] = {};
        if (GetEnvironmentVariableA("NR_DEBUG_PRESENT_FROM", f, 32)) from = std::strtoull(f, nullptr, 10);
        if (!folder.empty()) CreateDirectoryA(folder.c_str(), nullptr);
    }
    bool wanted() { return !folder.empty() && seen++ >= from && written < 60; }
    bool ensure(VkDevice d, VkDeviceSize bytes) {
        if (buffer && size >= bytes) return true;
        if (buffer) { vkUnmapMemory(d, memory); vkDestroyBuffer(d, buffer, nullptr); vkFreeMemory(d, memory, nullptr); buffer = VK_NULL_HANDLE; }
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = bytes; bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (vkCreateBuffer(d, &bi, nullptr, &buffer) != VK_SUCCESS) { buffer = VK_NULL_HANDLE; return false; }
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(d, buffer, &req);
        const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        uint32_t type = ~0u;
        for (uint32_t i = 0; i < g_vk.memory.memoryTypeCount; ++i)
            if ((req.memoryTypeBits & (1u << i)) && (g_vk.memory.memoryTypes[i].propertyFlags & want) == want) { type = i; break; }
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size; ai.memoryTypeIndex = type;
        if (type == ~0u || vkAllocateMemory(d, &ai, nullptr, &memory) != VK_SUCCESS || vkBindBufferMemory(d, buffer, memory, 0) != VK_SUCCESS ||
            vkMapMemory(d, memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
            vkDestroyBuffer(d, buffer, nullptr); buffer = VK_NULL_HANDLE;
            return false;
        }
        size = bytes;
        return true;
    }
};
VkDump g_vk_dump;

const VkPresentInfoKHR* vk_present_one(VkQueue queue, const VkPresentInfoKHR* info) {
    nr::pe::watchdog::Phase phase("Present layers (Vulkan)");
    static thread_local VkPresentInfoKHR t_info;
    static thread_local VkSemaphore t_done;
    std::unique_lock<std::mutex> guard(g_lock);
    const nr::asi::hudcap::Own own;   // its commands pass HUD FIX's capture hooks unseen
    if (!info || info->swapchainCount != 1) return nullptr;
    const VkSwapchainKHR sc = info->pSwapchains[0];
    SwapRec rec;
    bool known = false;
    {
        std::lock_guard<std::mutex> sg(g_swap_lock);
        auto it = g_swaps.find(sc);
        known = it != g_swaps.end();
        if (!known) it = g_swaps.find(g_swap_latest);
        if (it == g_swaps.end()) return nullptr;
        rec = it->second;
    }
    if (!rec.path || !rec.signals) return nullptr;
    if (!known) {
        // A frame generator's unobserved chain may use the same format, but owns different images and waits.
        rec.path = std::make_shared<VkPresentPath>("Vulkan games");
        rec.signals = std::make_shared<PresentSignals>(); rec.signals->device = rec.device;
        rec.images.clear();
        std::lock_guard<std::mutex> sg(g_swap_lock);
        g_swaps[sc] = rec; known = true;
    }
    VkPresentPath& P = *rec.path;
    P.presents = g_vk_present.presents;
    for (size_t i = 0; i < kPresentGuideSets; ++i) P.sets[i] = g_vk_present.sets[i];
    tick_vk_retired();
    nr::asi::wilsjo2_vk_tick();
    const std::shared_ptr<const Chain> chain = chain_now();
    const std::vector<const Layer*> layers = vk_present_plan(P, chain);
    g_vk_present.wanted = P.wanted.load();
    if (layers.empty() && !g_vk_is_dxvk) {
        nr::asi::hudcap::vk_present(VK_NULL_HANDLE, {}, VK_NULL_HANDLE, VK_NULL_HANDLE, 0, 0, false);
        hud_report(false, true);
    }
    if (layers.empty() || g_vk_is_dxvk) return nullptr;
    if (GetTickCount64() - g_present.evaluate_ms.load() > 1000) nr::dlssnr::frame_keys();   // as on D3D12
    // HUD FIX's capture (nr_asi_hudcap.hpp): its verdict once our command buffer has begun (below); a Present that
    // leaves before it (or whose command buffer is not submitted) disarms it, so no copy of it outlives the layers
    const uint32_t hud_mode = chain->hud.mode;
    struct HudDisarm {
        bool on = true;
        ~HudDisarm() {
            if (!on) return;
            nr::asi::hudcap::vk_present(VK_NULL_HANDLE, {}, VK_NULL_HANDLE, VK_NULL_HANDLE, 0, 0, false);
            hud_report(false, true);
        }
    } hud_disarm;
    const auto note_all = [&](const char* why) {
        for (const Layer* l : layers) layer_note(*l, 2, 3, why);
        P.served_ms = GetTickCount64(); g_vk_present.served_ms = P.served_ms.load();
    };
    if (!vk_route_ready(rec.device) || g_vk.dev.device != rec.device) {
        note_all("NR cannot run on this Vulkan device (see the log)");
        return nullptr;
    }
    if (rec.images.empty() || !known) {
        // winevulkan's export, through OptiScaler's detour where it has one: the images the game presents
        static const auto get_images = reinterpret_cast<PFN_vkGetSwapchainImagesKHR>(reinterpret_cast<void*>(
            GetProcAddress(GetModuleHandleW(L"winevulkan.dll"), "vkGetSwapchainImagesKHR")));
        uint32_t n = 0;
        if (get_images) get_images(rec.device, sc, &n, nullptr);
        rec.images.resize(n);
        if (!n || get_images(rec.device, sc, &n, rec.images.data()) < 0) rec.images.clear();
        if (known) { std::lock_guard<std::mutex> sg(g_swap_lock); g_swaps[sc].images = rec.images; }
    }
    const uint32_t index = info->pImageIndices ? info->pImageIndices[0] : ~0u;
    if (index >= rec.images.size()) { note_all("its swap-chain images could not be read"); return nullptr; }
    const VkImage image = rec.images[index];
    auto& signals = rec.signals->images;
    while (signals.size() < rec.images.size()) {
        VkSemaphoreCreateInfo ci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}; VkSemaphore sem{};
        if (vkCreateSemaphore(rec.device, &ci, nullptr, &sem) != VK_SUCCESS) return nullptr;
        receipts::watch(rec.device, sem);
        signals.push_back(sem);
    }
    const VkSemaphore done = signals[index];
    const uint32_t w = rec.extent.width, h = rec.extent.height;
    // The layers' frame: the swap chain's format (its UNORM twin for *_SRGB) copied in and out where the layers know it and
    // our programs store into it (the fast way); else a wider format by blits (present_blit_format), an *_SRGB swap
    // chain's bits copied into P.stage (its UNORM twin) first so the blits keep the encoded values.
    // NR_TEST_PRESENT_BLIT=1 (tests): the blits for every format.
    static const bool force_blit = [] { const char* e = std::getenv("NR_TEST_PRESENT_BLIT"); return e && e[0] == '1'; }();
    const VkFormat own_twin = unorm_twin(rec.format);
    const VkFormatFeatureFlags storable = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                          VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    const bool direct = !force_blit && dxgi_twin(rec.format) != DXGI_FORMAT_UNKNOWN && vk_format_has(own_twin, storable);
    const bool via_stage = !direct && own_twin != rec.format;
    const VkFormatFeatureFlags blits = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
    const VkFormat ff = direct ? own_twin
                        : present_blit_format(rec.format) != VK_FORMAT_UNDEFINED &&
                                  vk_format_has(present_blit_format(rec.format), storable | blits) &&
                                  vk_format_has(via_stage ? own_twin : rec.format, blits)
                              ? present_blit_format(rec.format) : VK_FORMAT_UNDEFINED;
    if (!direct && ff != VK_FORMAT_UNDEFINED && !P.said_blit) {
        P.said_blit = true;
        log("[nr] Present layers (Vulkan): the swap chain's format %d is taken in and out by blits through format %d%s",
            int(rec.format), int(ff), force_blit ? " (NR_TEST_PRESENT_BLIT)" : "");
    }
    const DXGI_FORMAT twin = dxgi_twin(direct || ff == VK_FORMAT_UNDEFINED ? rec.format : ff);
    const int space = dxgi_space(rec.space);
    bool unusable = false;
    const std::vector<VkPresentRun> runs = present_runs(layers, space, true, twin, 0.f, twin != DXGI_FORMAT_UNKNOWN && space >= 0,
                                                        &unusable);
    if (unusable != P.unusable) {
        P.unusable = unusable;
        log(unusable ? "[nr] Present layers (Vulkan): this swap chain cannot be served (colour space %d, format %d); the "
                       "RenoDX ones do not run until it can (never in another place)"
                     : "[nr] Present layers (Vulkan): the swap chain can be served again (colour space %d, format %d)",
            int(rec.space), int(rec.format));
    }
    P.served_ms = GetTickCount64(); g_vk_present.served_ms = P.served_ms.load();
    if (runs.empty()) return nullptr;
    const VkQueue work_queue = g_route_queue.load();
    const uint32_t present_family = queue_family(rec.device, queue), nr_family = queue_family(rec.device, work_queue);
    if (present_family == ~0u || nr_family == ~0u) { note_all("the present queue's family is not known"); return nullptr; }
    // A swap chain shared (CONCURRENT) by families that leave NR's out: the present queue copies its picture into P.xfer
    // (shared by both families) and back around our work (0.0.7 worked on the present queue itself); the swap chain's
    // image is not touched by NR's queue. NR_TEST_PRESENT_XFER=1 (tests): this way for every swap chain.
    static const bool force_xfer = [] { const char* e = std::getenv("NR_TEST_PRESENT_XFER"); return e && e[0] == '1'; }();
    const bool copy_bridge = force_xfer || (rec.sharing == VK_SHARING_MODE_CONCURRENT && present_family != nr_family &&
                                            std::find(rec.families.begin(), rec.families.end(), nr_family) == rec.families.end());
    const VkImageUsageFlags copies = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    const char* refused = (rec.usage & copies) != copies ? "its swap-chain images cannot be copied (their usage)"
                          : ff == VK_FORMAT_UNDEFINED || !ensure_image(P.frame, ff, w, h) ||
                                    (via_stage && !ensure_image(P.stage, own_twin, w, h, true))
                              ? "Present cannot serve this swap chain's colour format"
                          : !vk_ring_for(rec.device, work_queue) ? "our command buffers could not be made (see the log)" : nullptr;
    if (refused) {
        for (const VkPresentRun& r : runs) layer_note(*r.layer, 2, 3, refused);
        return nullptr;
    }
    VkPresentRing& R = g_vk_ring;
    const int s = R.slot;
    if (!R.fence[s]) {   // a failed submit's fence that could not be made again: made here, or no layers this Present
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        const VkResult e = test_fault("present_fence") ? VK_ERROR_OUT_OF_DEVICE_MEMORY : vkCreateFence(R.device, &fi, nullptr, &R.fence[s]);
        if (e != VK_SUCCESS) {
            R.fence[s] = VK_NULL_HANDLE;
            for (const VkPresentRun& r : runs) layer_note(*r.layer, 2, 3, "our command buffers could not be made (see the log)");
            static bool said = false;
            if (!said) { said = true; log("[nr] Present layers (Vulkan): our fence could not be made (%d); tried again at the next Present", int(e)); }
            return nullptr;
        }
    }
    if (const VkResult w = test_fault("present_wait") ? VK_ERROR_DEVICE_LOST
                                                       : vkWaitForFences(R.device, 1, &R.fence[s], VK_TRUE, 2000000000ull);
        w != VK_SUCCESS) {
        if (!R.said_wait) {
            R.said_wait = true;
            if (w == VK_TIMEOUT)
                log("[nr] Present layers (Vulkan): our previous command buffer did not finish in 2 s; presents go on "
                    "without the layers until it does");
            else
                log("[nr] Present layers (Vulkan): waiting for our previous command buffer returned %d; presents go on "
                    "without the layers", int(w));
        }
        return nullptr;
    }
    R.said_wait = false;
    PresentBridge* bridge = nullptr;
    if (copy_bridge) {
        if (!ensure_xfer_image(P.xfer, rec.format, w, h, present_family, nr_family) ||
            !(bridge = prepare_copy_bridge(rec.device, queue, present_family, s, image, P.xfer, w, h))) {
            note_all("the swap chain is not shared with NR's queue family and its copy for NR could not be made");
            return nullptr;
        }
        if (!P.said_xfer) {
            P.said_xfer = true;
            log("[nr] Present layers (Vulkan): the swap chain is not shared with NR's queue family (%u); the present "
                "queue's family (%u) copies its picture for NR and back%s", nr_family, present_family,
                force_xfer ? " (NR_TEST_PRESENT_XFER)" : "");
        }
    } else if (present_family != nr_family && rec.sharing == VK_SHARING_MODE_EXCLUSIVE) {
        bridge = prepare_bridge(rec.device, queue, present_family, nr_family, s, image);
        if (!bridge) { note_all("the present queue's ownership bridge is not ready"); return nullptr; }
    }
    // the picture NR's command buffer reads and writes: the swap chain's image (at rest PRESENT_SRC), or its copy
    const VkImage pic = copy_bridge ? P.xfer.image : image;
    const VkImageLayout pic_rest = copy_bridge ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    const VkCommandBuffer cmd = R.cmd[s];
    if (vkResetCommandBuffer(cmd, 0) != VK_SUCCESS || vkBeginCommandBuffer(cmd, &bi) != VK_SUCCESS) return nullptr;
    nr::gpu::reset(cmd);
    nr::gpu::Scope recording(nr::gpu::recording(cmd));
    g_native_uses.touch();
    if (bridge && !copy_bridge) ownership(cmd, image, present_family, nr_family, true);
    const VkImageLayout frame_layout_was = P.frame.layout, stage_layout_was = P.stage.layout;
    vk_barrier(cmd, pic, pic_rest, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    vk_barrier(cmd, P.frame.image, P.frame.layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    if (direct) {
        vk_copy(cmd, pic, 0, 0, P.frame.image, 0, 0, w, h);
    } else {   // (the blits: the swap chain's picture, through its UNORM twin for *_SRGB, into the wider frame)
        VkImage from = pic;
        if (via_stage) {
            vk_barrier(cmd, P.stage.image, P.stage.layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            vk_copy(cmd, pic, 0, 0, P.stage.image, 0, 0, w, h);
            vk_barrier(cmd, P.stage.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            P.stage.layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            from = P.stage.image;
        }
        vk_blit(cmd, from, P.frame.image, w, h);
    }
    vk_barrier(cmd, P.frame.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    P.frame.layout = VK_IMAGE_LAYOUT_GENERAL;
    // HUD FIX's capture: armed for Auto and Captured (only where the swap chain's images can be copied: it copies from
    // them in the game's command buffers), copying into P.hudless - made GENERAL here, before the game's next
    // command buffers (they run after this one) copy into it; its verdict on this frame
    const bool hud_armed = (hud_mode == 1 || hud_mode == 3) && (rec.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    const bool hud_image = hud_armed && ensure_image(P.hudless, unorm_twin(rec.format), w, h);
    const VkImageLayout hud_layout_was = P.hudless.layout;
    if (hud_image && P.hudless.layout != VK_IMAGE_LAYOUT_GENERAL) {
        vk_barrier(cmd, P.hudless.image, P.hudless.layout, VK_IMAGE_LAYOUT_GENERAL);
        P.hudless.layout = VK_IMAGE_LAYOUT_GENERAL;
    }
    const nr::asi::hudcap::FrameVk hud_cap = nr::asi::hudcap::vk_present(
        rec.device, rec.images, image, hud_image ? P.hudless.image : VK_NULL_HANDLE, w, h, hud_armed);
    const bool hud_captured = hud_image && hud_cap.result == nr::asi::hudcap::Result::kCaptured;
    hud_report(!runs.empty(), true, hud_captured, hud_cap.why, false, false, std::string(), "", hud_cap.broken);
    // not submitted: the layouts this command buffer would have set are not there, and the capture is disarmed
    const auto unsubmitted = [&] {
        P.hudless.layout = hud_layout_was;
        P.frame.layout = frame_layout_was;
        P.stage.layout = stage_layout_was;
        hud_disarm.on = true;
        discard_session_timing();   // its timing queries were never reset on the GPU
    };
    hud_disarm.on = false;
    VkHudless hudless;
    if (hud_captured) {
        hudless.image = w2_of(P.hudless);
        hudless.estimate_too = hud_mode == 1;
    }
    const bool changed = vk_present_run(cmd, P, chain, runs, P.frame.image, ff, w, h, hudless);
    if (changed) {
        vk_barrier(cmd, P.frame.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        vk_barrier(cmd, pic, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        if (direct) {
            vk_copy(cmd, P.frame.image, 0, 0, pic, 0, 0, w, h);
        } else if (via_stage) {
            vk_barrier(cmd, P.stage.image, P.stage.layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            vk_blit(cmd, P.frame.image, P.stage.image, w, h);
            vk_barrier(cmd, P.stage.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            P.stage.layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            vk_copy(cmd, P.stage.image, 0, 0, pic, 0, 0, w, h);
        } else {
            vk_blit(cmd, P.frame.image, pic, w, h);
        }
        vk_barrier(cmd, pic, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, pic_rest);
        vk_barrier(cmd, P.frame.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    } else {
        vk_barrier(cmd, pic, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, pic_rest);
    }
    const uint32_t bpp = rec.format == VK_FORMAT_R16G16B16A16_SFLOAT ? 8u : 4u;
    const bool dump = g_vk_dump.wanted() && g_vk_dump.ensure(R.device, VkDeviceSize(w) * h * bpp);
    if (dump) {
        vk_barrier(cmd, pic, pic_rest, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy c{};
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.imageExtent = {w, h, 1};
        vkCmdCopyImageToBuffer(cmd, pic, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_vk_dump.buffer, 1, &c);
        vk_barrier(cmd, pic, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, pic_rest);
    }
    if (bridge && !copy_bridge) ownership(cmd, image, nr_family, present_family, false);
    if (vkEndCommandBuffer(cmd) != VK_SUCCESS || vkResetFences(R.device, 1, &R.fence[s]) != VK_SUCCESS) {
        unsubmitted();
        return nullptr;
    }
    std::vector<VkPipelineStageFlags> stages(info->waitSemaphoreCount, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = info->waitSemaphoreCount;
    si.pWaitSemaphores = info->pWaitSemaphores;
    si.pWaitDstStageMask = stages.data();
    si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
    si.signalSemaphoreCount = 1; si.pSignalSemaphores = &done;
    R.ticket[s] = ++R.submits;   // under g_lock (a failed submit re-makes the fence signalled: then done at once)
    guard.unlock();   // our submit takes the queue's lock (hk_queue_submit), as the game's do
    VkResult r = VK_SUCCESS;
    bool work_submitted = false, released = false;
    const VkPipelineStageFlags all = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    if (bridge) {
        VkSubmitInfo release{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        release.waitSemaphoreCount = si.waitSemaphoreCount; release.pWaitSemaphores = si.pWaitSemaphores;
        release.pWaitDstStageMask = si.pWaitDstStageMask;
        release.commandBufferCount = 1; release.pCommandBuffers = &bridge->cmd[0];
        release.signalSemaphoreCount = 1; release.pSignalSemaphores = &bridge->to_nr;
        r = vkQueueSubmit(queue, 1, &release, VK_NULL_HANDLE);
        released = r == VK_SUCCESS;
        si.waitSemaphoreCount = 1; si.pWaitSemaphores = &bridge->to_nr; si.pWaitDstStageMask = &all;
        si.pSignalSemaphores = &bridge->from_nr;
    }
    if (r == VK_SUCCESS) {
        r = test_fault("present_submit") ? VK_ERROR_OUT_OF_DEVICE_MEMORY : vkQueueSubmit(work_queue, 1, &si, R.fence[s]);
        work_submitted = r == VK_SUCCESS;
        if (!work_submitted && bridge && !copy_bridge) {   // the exclusive image's ownership went to NR's family with the
            // release: only NR's work gives it back - once more (a failed submit leaves everything it names as it was)
            r = test_fault("present_submit") ? VK_ERROR_OUT_OF_DEVICE_MEMORY : vkQueueSubmit(work_queue, 1, &si, R.fence[s]);
            work_submitted = r == VK_SUCCESS;
        }
    }
    if (r == VK_SUCCESS && bridge) {
        VkSubmitInfo acquire{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        acquire.waitSemaphoreCount = 1; acquire.pWaitSemaphores = &bridge->from_nr; acquire.pWaitDstStageMask = &all;
        acquire.commandBufferCount = 1; acquire.pCommandBuffers = &bridge->cmd[1];
        acquire.signalSemaphoreCount = 1; acquire.pSignalSemaphores = &done;
        r = vkResetFences(rec.device, 1, &bridge->fence);
        if (r == VK_SUCCESS) r = vkQueueSubmit(queue, 1, &acquire, bridge->fence);
    }
    if (r != VK_SUCCESS && released) {   // a bridge step went without the bridge's fence: its queues drained before its
                                         // command buffers are recorded again (rare: the failure path only)
        vkQueueWaitIdle(queue);
        if (work_submitted && work_queue != queue) vkQueueWaitIdle(work_queue);
    }
    guard.lock();
    if (r != VK_SUCCESS) {
        // Never destroy the fence of NR work that was successfully submitted before the error. A failed submit leaves
        // its semaphores as they were (the spec): what decides is whether anything that did go consumed the waits.
        if (!work_submitted) {
            vkDestroyFence(R.device, R.fence[s], nullptr);
            VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            if (test_fault("present_fence") || vkCreateFence(R.device, &fi, nullptr, &R.fence[s]) != VK_SUCCESS)
                R.fence[s] = VK_NULL_HANDLE;
            unsubmitted();
        }
        static bool said = false;
        if (!released) {   // the present's own waits untouched: it goes on as the game made it, without the layers
            if (!said) { said = true; log("[nr] Present layers (Vulkan): our submit returned %d; that present goes on without the layers", int(r)); }
            return nullptr;
        }
        if (copy_bridge) {   // the swap-chain image itself untouched (its copy is NR's): presented once the bridge's
                             // last submitted step is done
            if (!said) { said = true; log("[nr] Present layers (Vulkan): our submit returned %d; that present goes on without the layers", int(r)); }
            t_info = *info;
            t_info.waitSemaphoreCount = 1;
            t_done = work_submitted ? bridge->from_nr : bridge->to_nr;
            t_info.pWaitSemaphores = &t_done;
            return &t_info;
        }
        // The exclusive bridge stopped half way (NR's work not taken twice, or the way back not): the image's ownership
        // is not the present family's. Reported to the caller; the bridge (its semaphores may be left signalled, its
        // queues drained above) is made anew.
        if (!said) { said = true; log("[nr] Present layers (Vulkan): our submit returned %d; reported to the caller", int(r)); }
        t_present_failure = r;
        for (auto i = present_bridges.begin(); i != present_bridges.end(); ++i)
            if (&i->second == bridge) {
                PresentBridge& b = i->second;
                vkDestroyFence(b.device, b.fence, nullptr);
                vkDestroySemaphore(b.device, b.to_nr, nullptr);
                vkDestroySemaphore(b.device, b.from_nr, nullptr);
                vkDestroyCommandPool(b.device, b.pool, nullptr);
                present_bridges.erase(i);
                break;
            }
        return nullptr;
    }
    if (dump && vkWaitForFences(R.device, 1, &R.fence[s], VK_TRUE, 5000000000ull) == VK_SUCCESS) {
        char path[MAX_PATH + 32];
        std::snprintf(path, sizeof path, "%s\\p_%05llu.raw", g_vk_dump.folder.c_str(),
                      static_cast<unsigned long long>(g_vk_dump.written));
        if (FILE* f = std::fopen(path, "wb")) {
            std::fwrite(g_vk_dump.mapped, 1, size_t(w) * h * bpp, f);
            std::fclose(f);
        }
        log("[nr] debug present dump %llu: %ux%u VkFormat %d (Vulkan)", static_cast<unsigned long long>(g_vk_dump.written),
            w, h, int(rec.format));
        ++g_vk_dump.written;
    }
    R.slot = (s + 1) % VkPresentRing::kSlots;
    t_info = *info;
    t_info.waitSemaphoreCount = 1;
    t_done = done;
    t_info.pWaitSemaphores = &t_done;
    return &t_info;
}

// Each served image consumes the preceding wait chain. Keep the original Present's pNext/results and all images,
// including unsupported ones; only its final wait list changes.
const VkPresentInfoKHR* vk_present_layers(VkQueue queue, const VkPresentInfoKHR* info) {
    if (!info || !info->pSwapchains || !info->pImageIndices) return nullptr;
    static thread_local VkPresentInfoKHR result;
    static thread_local VkSemaphore done;
    { std::lock_guard<std::mutex> guard(g_lock); ++g_vk_present.presents; }
    std::vector<VkSemaphore> waits;
    if (info->waitSemaphoreCount) waits.assign(info->pWaitSemaphores, info->pWaitSemaphores + info->waitSemaphoreCount);
    bool changed = false;
    for (uint32_t i = 0; i < info->swapchainCount; ++i) {
        VkPresentInfoKHR one = *info;
        one.pNext = nullptr; one.pResults = nullptr; one.swapchainCount = 1;
        one.pSwapchains = info->pSwapchains + i; one.pImageIndices = info->pImageIndices + i;
        one.waitSemaphoreCount = uint32_t(waits.size()); one.pWaitSemaphores = waits.data();
        if (const auto* served = vk_present_one(queue, &one)) {
            waits.assign(served->pWaitSemaphores, served->pWaitSemaphores + served->waitSemaphoreCount);
            changed = true;
        }
        if (t_present_failure < 0) return nullptr;
    }
    if (!changed) return nullptr;
    result = *info; done = waits.front(); result.waitSemaphoreCount = 1; result.pWaitSemaphores = &done;
    return &result;
}

void retire_present_path(VkPresentPath& P) {
    vk_forget(P.vk); P.layers.clear();
    for (OwnImage* im : {&P.frame, &P.stage, &P.xfer, &P.hudless, &P.linear, &P.encoded, &P.hud_back, &P.hud_prev, &P.hud_alpha,
                         &P.hud_stats})
        retire_image(*im);
    for (auto& set : P.sets) set = VkGuideSet{};
    P.hud_prev_valid = false;
}

// The device goes (vk_route_teardown, its queues idle): everything of the Present layers on it.
void vk_present_forget() {
    VkPresentPath& P = g_vk_present;
    vk_forget(P.vk);
    P.layers.clear();
    for (VkGuideSet& g : P.sets) g = VkGuideSet{};
    retire_image(P.linear);
    retire_image(P.encoded);
    for (OwnImage* im : {&P.hud_back, &P.hud_prev, &P.hud_alpha, &P.hud_stats}) retire_image(*im);
    P.hud_prev_valid = false;
    retire_image(g_vk_frame);
    retire_image(g_vk_hudless);
    for (auto& set : g_vk_guides) for (OwnImage& im : set) retire_image(im);
    vk_ring_destroy(true);
    destroy_present_bridges(g_vk.dev.device);
    std::lock_guard<std::mutex> sg(g_swap_lock);
    for (auto& [sc, rec] : g_swaps) { if (rec.path) retire_present_path(*rec.path); rec.images.clear(); }
    for (auto& [sc, rec] : g_swaps) if (rec.device == g_vk.dev.device && rec.signals) {
        for (VkSemaphore sem : rec.signals->images) receipts::retire(rec.device, sem);
        rec.signals->images.clear();
    }
}

// At a Vulkan evaluate while the chain has Present layers: its depth and motion copied (in the game's command buffer,
// before the upscaler) into the oldest guide set, kept SHADER_READ_ONLY_OPTIMAL.
void vk_capture_guides(VkCommandBuffer cmd, const VkFrame& f, const nr::asi::Wilsjo2VkFrame& wf) {
    VkPresentPath& P = g_vk_present;
    VkGuideSet& g = vk_guide_slot(P);
    const size_t k = size_t(&g - P.sets);
    const NVSDK_NGX_Resource_VK* src[2] = {f.depth, f.motion};
    const VkImageLayout layout[2] = {f.depth_layout, f.motion_layout};
    for (int i = 0; i < 2; ++i) {
        const auto& v = src[i]->Resource.ImageViewInfo;
        if (!ensure_image(g_vk_guides[k][i], VkFormat(v.Format), v.Width, v.Height, true)) { g.serial = 0; return; }
    }
    for (int i = 0; i < 2; ++i) {
        const auto& v = src[i]->Resource.ImageViewInfo;
        OwnImage& im = g_vk_guides[k][i];
        const VkFormat fmt = VkFormat(v.Format);
        const VkImageAspectFlags aspect = depth_format(fmt)
            ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT) |
                  (fmt == VK_FORMAT_D16_UNORM_S8_UINT || fmt == VK_FORMAT_D24_UNORM_S8_UINT || fmt == VK_FORMAT_D32_SFLOAT_S8_UINT
                       ? VK_IMAGE_ASPECT_STENCIL_BIT : 0)
            : VK_IMAGE_ASPECT_COLOR_BIT;
        const auto barrier = [&](VkImage img, VkImageLayout from, VkImageLayout to) {
            VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            b.srcAccessMask = b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            b.oldLayout = from; b.newLayout = to;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = img;
            b.subresourceRange = {aspect, 0, 1, 0, 1};
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr,
                                 0, nullptr, 1, &b);
        };
        const VkImage game = reinterpret_cast<VkImage>(v.Image);
        barrier(game, layout[i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        barrier(im.image, im.layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageCopy c{};
        const VkImageAspectFlags copy_aspect = depth_format(fmt) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
        c.srcSubresource = c.dstSubresource = {copy_aspect, 0, 0, 1};
        c.extent = {v.Width, v.Height, 1};
        vkCmdCopyImage(cmd, game, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, im.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        barrier(game, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, layout[i]);
        barrier(im.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        im.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    vk_guide_stamp(P, g, f);
    const auto ref = [](const OwnImage& im) {
        nr::dlssnr::VkImageRef r;
        r.image = uint64_t(reinterpret_cast<uintptr_t>(im.image));
        r.format = uint32_t(im.format); r.width = im.width; r.height = im.height;
        r.layout = uint32_t(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        r.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        return r;
    };
    g.depth = ref(g_vk_guides[k][0]);
    g.motion = ref(g_vk_guides[k][1]);
    g.w2 = wf;
    for (int i = 0; i < 2; ++i) {
        nr::asi::Wilsjo2VkImage& wi = i ? g.w2.motion : g.w2.depth;
        const OwnImage& im = g_vk_guides[k][i];
        wi.image = im.image; wi.format = im.format; wi.width = im.width; wi.height = im.height;
        wi.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    g.w2.exposure = nr::asi::Wilsjo2VkImage{};
}

// ---- the front hook: in front of OptiScaler's vkQueuePresentKHR detour -------------------------------------------
std::atomic<int> g_front{0};   // 0 not decided, 1 in front of OptiScaler's detour, 2 none: the lowest hook serves
}  // namespace
extern "C" {
void* nr_vk_front_original = nullptr;   // OptiScaler's detour, past ours (MinHook's trampoline)
void* nr_vk_front_present(VkQueue queue, const VkPresentInfoKHR* info, void* caller);
void nr_vk_front_thunk();
}
#if defined(__x86_64__)
// rcx queue, rdx info; the caller's return address at [rsp]. Calls nr_vk_front_present(queue, info, caller) and jumps
// to OptiScaler's detour with rdx = what it returned and the stack as the caller left it.
asm(R"(
    .text
    .globl nr_vk_front_thunk
    .def nr_vk_front_thunk; .scl 2; .type 32; .endef
    .seh_proc nr_vk_front_thunk
nr_vk_front_thunk:
    push %rcx
    .seh_pushreg %rcx
    push %rdx
    .seh_pushreg %rdx
    sub $0x28, %rsp
    .seh_stackalloc 0x28
    .seh_endprologue
    mov 0x38(%rsp), %r8
    call nr_vk_front_present
    add $0x28, %rsp
    pop %rdx
    pop %rcx
    test %rax, %rax
    js 1f
    mov %rax, %rdx
    jmp *nr_vk_front_original(%rip)
1:
    ret
    .seh_endproc
)");
#endif
namespace {

// The function `fn` jumps to (jmp rel8 / rel32 / [rip+disp32]), followed up to 6 times, stopping in a module's code
// that is no jump.
void* jump_target(void* fn) {
    auto* p = static_cast<uint8_t*>(fn);
    for (int i = 0; i < 6 && p; ++i) {
        if (p[0] == 0xE9) p = p + 5 + *reinterpret_cast<int32_t*>(p + 1);
        else if (p[0] == 0xEB) p = p + 2 + int8_t(p[1]);
        else if (p[0] == 0xFF && p[1] == 0x25) p = *reinterpret_cast<uint8_t**>(p + 6 + *reinterpret_cast<int32_t*>(p + 2));
        else break;
    }
    return p;
}

void decide_front(VkQueue queue) {
    static std::once_flag once;
    std::call_once(once, [queue] {
        (void)queue;
        g_front = 2;
#if defined(__x86_64__)
        HMODULE wv = GetModuleHandleW(L"winevulkan.dll");
        void* fns[2] = {wv ? reinterpret_cast<void*>(GetProcAddress(wv, "vkQueuePresentKHR")) : nullptr, nullptr};
        {
            VkDevice d = VK_NULL_HANDLE;
            { std::lock_guard<std::mutex> guard(g_swap_lock); if (const auto it = g_swaps.find(g_swap_latest); it != g_swaps.end()) d = it->second.device; }
            const auto gdpa = wv ? reinterpret_cast<PFN_vkGetDeviceProcAddr>(reinterpret_cast<void*>(GetProcAddress(wv, "vkGetDeviceProcAddr"))) : nullptr;
            if (d && gdpa) fns[1] = reinterpret_cast<void*>(gdpa(d, "vkQueuePresentKHR"));
        }
        for (void* fn : fns) {
            if (!fn) continue;
            void* target = jump_target(fn);
            HMODULE owner = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               static_cast<LPCWSTR>(target), &owner);
            if (owner != g_opti || !g_opti) continue;
            if (MH_CreateHook(target, reinterpret_cast<void*>(&nr_vk_front_thunk), &nr_vk_front_original) == MH_OK &&
                MH_EnableHook(target) == MH_OK) {
                g_front = 1;
                log("[nr] Present layers (Vulkan games): in front of OptiScaler's vkQueuePresentKHR (%p)", target);
            } else {
                log("[nr] Present layers (Vulkan games): OptiScaler's vkQueuePresentKHR (%p) could not be hooked; the "
                    "layers run under it (after its frame generation)", target);
            }
            return;
        }
        log("[nr] Present layers (Vulkan games): no OptiScaler detour on vkQueuePresentKHR; the layers run at "
            "winevulkan's");
#endif
    });
}

// The menu also draws while no Present NR layer runs (including all layers off).
const VkPresentInfoKHR* vk_present_menu(VkQueue queue, const VkPresentInfoKHR* info) {
    if (!info || info->swapchainCount != 1) return nullptr;
    const VkSwapchainKHR sc = info->pSwapchains[0];
    SwapRec rec;
    SeenDevice dev;
    uint32_t family = ~0u;
    {
        std::lock_guard<std::mutex> guard(g_lock);
        {
            std::lock_guard<std::mutex> sg(g_swap_lock);
            auto it=g_swaps.find(sc);
            if(it==g_swaps.end())return nullptr;
            rec=it->second;
        }
        if (!(rec.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) || !vk_route_ready(rec.device)) return nullptr;
        dev=g_vk.dev;
        if(dev.device!=rec.device || dev.translator)return nullptr;
        for(size_t i=0;i<dev.families.size() && family==~0u;++i)
            for(uint32_t j=0;j<(i<dev.counts.size()?dev.counts[i]:1u);++j) {
                VkQueue q{};vkGetDeviceQueue(dev.device,dev.families[i],j,&q);
                if(q==queue){family=dev.families[i];break;}
            }
        if(family==~0u)return nullptr;
        uint32_t count=0;
        vkGetPhysicalDeviceQueueFamilyProperties(dev.physical,&count,nullptr);
        std::vector<VkQueueFamilyProperties> properties(count);
        vkGetPhysicalDeviceQueueFamilyProperties(dev.physical,&count,properties.data());
        if(family>=count || !(properties[family].queueFlags&VK_QUEUE_GRAPHICS_BIT))return nullptr;
        if(rec.images.empty()) {
            static const auto get=reinterpret_cast<PFN_vkGetSwapchainImagesKHR>(reinterpret_cast<void*>(
                GetProcAddress(GetModuleHandleW(L"winevulkan.dll"),"vkGetSwapchainImagesKHR")));
            count=0;if(!get || get(rec.device,sc,&count,nullptr)!=VK_SUCCESS || !count)return nullptr;
            rec.images.resize(count);
            if(get(rec.device,sc,&count,rec.images.data())!=VK_SUCCESS)return nullptr;
            std::lock_guard<std::mutex> sg(g_swap_lock);g_swaps[sc].images=rec.images;
        }
    }
    const nr::asi::hudcap::Own own;
    return nr::pe::menu::vulkan_present(dev.instance,dev.physical,dev.device,queue,family,sc,rec.format,rec.space,
                                       rec.extent,rec.images,info);
}

thread_local int t_present_inside = 0;
// A present that came through OptiScaler's detour (served in front of it) reaches here from OptiScaler's module and is
// left alone; one that did not (no detour of OptiScaler's, or the game calling an address it did not detour) is served
// here.
const VkPresentInfoKHR* vk_present_bottom(VkQueue q, const VkPresentInfoKHR* p, void* caller) {
    if (!p || t_present_inside) return nullptr;
    if (g_front.load() == 0) decide_front(q);
    if (!game_caller(caller)) return nullptr;
    struct Inside { Inside() { ++t_present_inside; } ~Inside() { --t_present_inside; } } inside;
    const VkPresentInfoKHR* layers = (g_vk_present.wanted.load() || vk_present_due(g_vk_present)) ? vk_present_layers(q, p) : nullptr;
    if (t_present_failure < 0) return nullptr;
    const VkPresentInfoKHR* menu = vk_present_menu(q, layers ? layers : p);
    return menu ? menu : layers;
}
}  // namespace
void* nr_vk_front_present(VkQueue queue, const VkPresentInfoKHR* info, void* caller) {
    t_present_failure = VK_SUCCESS;
    if (!info || t_present_inside || !game_caller(caller)) return const_cast<VkPresentInfoKHR*>(info);
    nr::pe::watchdog::beat();
    struct Inside { Inside() { ++t_present_inside; } ~Inside() { --t_present_inside; } } inside;
    const VkPresentInfoKHR* mine = (g_vk_present.wanted.load() || vk_present_due(g_vk_present)) ? vk_present_layers(queue, info) : nullptr;
    if (t_present_failure < 0) return reinterpret_cast<void*>(intptr_t(t_present_failure));
    if (const VkPresentInfoKHR* menu = vk_present_menu(queue, mine ? mine : info)) mine = menu;
    return const_cast<VkPresentInfoKHR*>(mine ? mine : info);
}
namespace {

thread_local int t_vk_inside = 0;

NVSDK_NGX_Result NR_NGX_MSABI hk_vk_evaluate(VkCommandBuffer cmd, const NVSDK_NGX_Handle* h, NVSDK_NGX_Parameter* p,
                                             void* callback) {
    if (t_vk_inside || !cmd || !h || !p) return real_vk_evaluate(cmd, h, p, callback);
    struct Inside { Inside() { ++t_vk_inside; } ~Inside() { --t_vk_inside; } } inside;
    nr::gpu::Scope recording(nr::gpu::recording(cmd));
    nr::pe::watchdog::Phase phase("OptiScaler's Vulkan evaluate (NR)");
    std::unique_lock<std::mutex> guard(g_lock);
    tick_vk_retired();
    nr::asi::wilsjo2_vk_tick();
    VkTracked& t = g_vk_features[h->Id];
    if (t.feature < 0) {
        HMODULE caller = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(__builtin_return_address(0)), &caller);
        t.feature = caller == g_opti ? kFeatureSuperSampling : unseen_feature([p](const char* n) {
            void* v = nullptr;
            return p->Get(n, &v) == NVSDK_NGX_Result_Success && v;
        });
        log("[nr] OptiScaler Vulkan feature %u first seen at an evaluate from %s: taken as %s", h->Id,
            caller == g_opti ? "OptiScaler's own input" : "the game (its create not seen; by what the call carries)",
            feature_name(t.feature));
    }
    if (t.feature != kFeatureSuperSampling && t.feature != kFeatureRayReconstruction) {
        guard.unlock();
        return real_vk_evaluate(cmd, h, p, callback);
    }
    VkFrame f;
    if (!read_vk_frame(p, t, f)) {
        if (!t.said) {
            t.said = true;
            log("[nr] OptiScaler Vulkan feature %u: no colour, output, depth or motion vector image in its evaluate; NR "
                "does not run", h->Id);
        }
        guard.unlock();
        return real_vk_evaluate(cmd, h, p, callback);
    }
    if (!vk_route_ready(t.device ? t.device : g_vk_created_on)) {
        guard.unlock();
        return real_vk_evaluate(cmd, h, p, callback);
    }
    if (t.device && t.device != g_vk.dev.device) {   // a feature on another device than the session's: not served
        if (!t.said) {
            t.said = true;
            log("[nr] OptiScaler Vulkan feature %u is on device %p, NR's session on %p; NR does not run on it", h->Id,
                static_cast<void*>(t.device), static_cast<void*>(g_vk.dev.device));
        }
        guard.unlock();
        return real_vk_evaluate(cmd, h, p, callback);
    }
    const bool nr_on = nr::dlssnr::frame_keys();
    VkPlan plan;
    plan_layers_vk(t, f, nr_on, "Vulkan games", plan, g_vk_present);
    g_vk_present.wanted = plan.present;
    g_present.evaluate_ms = GetTickCount64();
    std::vector<const Layer*>& before = plan.before;
    std::vector<const Layer*>& after = plan.after;
    bool before_renodx = plan.before_renodx;
    const uint32_t w2_aw = plan.w2_aw, w2_ah = plan.w2_ah;
    if ((!before.empty() || !after.empty() || plan.present) && !vk_plain_guides(cmd, t, f)) {
        guard.unlock();
        return real_vk_evaluate(cmd, h, p, callback);
    }
    const nr::asi::Wilsjo2VkFrame wf = wilsjo2_vk_frame(p, f);
    if (plan.present) {
        vk_capture_guides(cmd, f, wf);
        if (g_front.load() == 0) decide_front(VK_NULL_HANDLE);   // the game may present through an address of its own
    }
    ++g_fg_frames;
    NVSDK_NGX_Result r;
    if (!before.empty()) {
        // The colour's region copied into our own image, the layers on it in order, handed to the upscaler in the
        // colour's place (SHADER_READ_ONLY_OPTIMAL, base 0); the game's colour is never written. The region: the
        // render region from the colour's base (RenoDX's), or for wilsjo2 layers alone their active extent at origin 0.
        const uint32_t rx = before_renodx ? f.color_x : 0u, ry = before_renodx ? f.color_y : 0u;
        const uint32_t rw = before_renodx ? f.render_w : w2_aw, rh = before_renodx ? f.render_h : w2_ah;
        // the colour as its view gives it (a transfer copy where the image allows one, else our copy program: 0.0.7's)
        if (!copy_in(cmd, f.color, f.color_layout, f.color_usage, rx, ry, rw, rh, t.copy, VK_IMAGE_LAYOUT_GENERAL)) {
            // the layers before the upscaler cannot run this frame (said by copy_in's caller once): the upscaler and the
            // layers after it still do (0.0.7 ran its after-SR NR on its own)
            static bool said = false;
            if (!said) { said = true; log("[nr] Vulkan route: the colour could not be read for the layers before the upscaler; they skip such frames, the layers after it run"); }
            guard.unlock();
            r = real_vk_evaluate(cmd, h, p, callback);
        } else {
        run_before_vk(cmd, t, before, f, wf, nr_on, own_ref(t.copy), rw, rh, plan.chain->inspect,
                      after.empty() && !plan.present_takes);
        vk_barrier(cmd, t.copy.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        t.copy.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        void* copy = &t.copy.ngx;
        guard.unlock();
        p->Set(f.color_key, copy);
        const bool moved = rx || ry;
        if (moved) { p->Set("DLSS.Input.Color.Subrect.Base.X", 0u); p->Set("DLSS.Input.Color.Subrect.Base.Y", 0u); }
        r = real_vk_evaluate(cmd, h, p, callback);
        p->Set(f.color_key, static_cast<void*>(const_cast<NVSDK_NGX_Resource_VK*>(f.color)));
        if (moved) { p->Set("DLSS.Input.Color.Subrect.Base.X", f.color_x); p->Set("DLSS.Input.Color.Subrect.Base.Y", f.color_y); }
        }
    } else {
        guard.unlock();
        r = real_vk_evaluate(cmd, h, p, callback);
    }
    if (r != NVSDK_NGX_Result_Success) return r;
    if (after.empty()) return r;
    // After the upscaler: the layers on its output (GENERAL), in place, on the same command buffer.
    guard.lock();
    const auto it = g_vk_features.find(h->Id);
    if (it == g_vk_features.end()) return r;
    VkTracked& t2 = it->second;
    run_after_vk(cmd, t2, after, f, wf, nr_on, plan.chain->inspect, !plan.present_takes);
    return r;
}

// ---- D3D11 games ---------------------------------------------------------------------------------------------------
// OptiScaler's D3D11 inputs (the game's DLSS, FSR 2 / XeSS on D3D11) reach the exported NVSDK_NGX_D3D11_EvaluateFeature;
// detoured the same way. D3D11 has no command list to record into: under DXVK a D3D11 game is a Vulkan game whose
// command buffer is ours - recorded with the Vulkan route's layers above (the model's Vulkan session on DXVK's device),
// submitted on DXVK's queue after its pending work (FlushRenderingCommands, then under LockSubmissionQueue: DXVK's
// documented interop, the ReShade route's D3D11 path). The images are DXVK's, in the layouts DXVK keeps them in; the
// upscaler's output is moved to GENERAL for our work and back. Before the upscaler: the colour's region copied by the
// game's context into a texture of ours, the layers on it, that texture handed to the upscaler in the colour's place.
// The 0.0.7 route ran NR for D3D11 games inside the forked OptiScaler's D3D11-on-D3D12 features; here every D3D11
// upscaler of OptiScaler's is served. Present layers: at the game's DXGI Present (d11_present_layers below), never here.
using PFN_D11Create = NVSDK_NGX_Result(NR_NGX_MSABI*)(ID3D11DeviceContext*, NVSDK_NGX_Feature, NVSDK_NGX_Parameter*,
                                                     NVSDK_NGX_Handle**);
using PFN_D11Evaluate = NVSDK_NGX_Result(NR_NGX_MSABI*)(ID3D11DeviceContext*, const NVSDK_NGX_Handle*, NVSDK_NGX_Parameter*,
                                                       void*);
PFN_D11Create real_d11_create = nullptr;
PFN_D11Evaluate real_d11_evaluate = nullptr;
PFN_Release real_d11_release = nullptr;

struct D11Route {
    bool tried = false, ready = false;
    ID3D11Device* device = nullptr;            // the game's (a reference held)
    IDXGIVkInteropDevice* interop = nullptr;   // DXVK's (a reference held)
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t family = 0;
    VkCommandPool pool = VK_NULL_HANDLE;
    static constexpr int kRing = 4;
    VkCommandBuffer cmd[kRing] = {};
    VkFence fence[kRing] = {};
    // the D3D11 objects a slot's command buffer reads or writes, a reference each until its fence is waited for: DXVK
    // keeps them for its own work only, and a game may let a guide go right after the evaluate
    std::vector<IUnknown*> held[kRing];
    int slot = 0;
    bool said_wait = false;
    VkResult last_error = VK_SUCCESS;   // our last command buffer's failure (d11_route_failed)
    bool recording = false;             // d11_begin gave a command buffer that d11_submit has not taken yet
    int failures = 0;                   // in a row
    ULONGLONG retry_ms = 0;             // stopped on its failures: started again from then (0: not)
    const void* tried_on = nullptr;     // the device the route was last started on (compared only, no reference)
} g_d11;   // g_lock

void d11_hold(IUnknown* r) {
    if (!r) return;
    r->AddRef();
    g_d11.held[g_d11.slot].push_back(r);
}

// Our textures (the colour's copy), released as the D3D12 ones: 64 more D3D11 evaluates AND 2 s.
struct RetiredTexture { uint64_t at; ULONGLONG ms; ID3D11Texture2D* t; };
std::vector<RetiredTexture> g_d11_retired;
uint64_t g_d11_clock = 0;
void retire_texture(ID3D11Texture2D*& t) {
    if (t) g_d11_retired.push_back({g_d11_clock + 64, GetTickCount64() + 2000, t});
    t = nullptr;
}
void tick_d11_retired() {
    ++g_d11_clock;
    const ULONGLONG now = GetTickCount64();
    for (size_t i = g_d11_retired.size(); i-- > 0;)
        if (g_d11_retired[i].at <= g_d11_clock && g_d11_retired[i].ms <= now) {
            g_d11_retired[i].t->Release();
            g_d11_retired.erase(g_d11_retired.begin() + long(i));
        }
}

struct D11Tracked {
    VkTracked vk;                         // the layers' states (the Vulkan route's)
    ID3D11Texture2D* copy = nullptr;      // the colour's region, before the upscaler
};
std::unordered_map<uint32_t, D11Tracked> g_d11_features;   // g_lock

void d11_forget(D11Tracked& t) {
    vk_forget(t.vk);
    retire_texture(t.copy);
}

bool d11_route_release(const char* why);
// The first D3D11 evaluate: DXVK's Vulkan device and queue under the game's D3D11 device, the model's Vulkan session on
// them, our command buffers. Made again when the game evaluates on another D3D11 device for 30 evaluates in a row (it
// made a new one: after a device loss, or a first device that was only a probe), and 5 s after the route stopped on
// its failures (d11_route_failed); a device that is not DXVK's is not asked again.
bool d11_route_ready(ID3D11Device* device) {
    static int other = 0;   // evaluates in a row on a device other than the route's
    if (g_d11.tried) {
        if (g_d11.ready && device == g_d11.device) { other = 0; return true; }
        const bool moved = device != (g_d11.device ? static_cast<const void*>(g_d11.device) : g_d11.tried_on);
        if (moved) {
            if (g_d11.ready && ++other < 30) return false;
        } else if (g_d11.ready || !g_d11.retry_ms || GetTickCount64() < g_d11.retry_ms) {
            return false;
        }
        other = 0;
        if (!d11_route_release(moved ? "the game evaluates on another D3D11 device; NR starts on it"
                                     : "starting again after its command buffers failed"))
            return false;
    }
    g_d11.tried = true;
    g_d11.tried_on = device;
    if (g_vk.tried) {
        log("[nr] D3D11 route: the model's Vulkan session already serves a Vulkan device; NR does not run on D3D11 here");
        return false;
    }
    nr::pe::vulkan_entry_points();
    IDXGIVkInteropDevice* interop = nullptr;
    const nr::pe::DeviceHandles hd = nr::pe::d3d11_handles(device, &interop);
    if (!hd.valid() || !interop) {
        if (interop) interop->Release();
        log("[nr] D3D11 route: no Vulkan device under this D3D11 device (not DXVK); NR does not run on D3D11");
        return false;
    }
    interop->GetSubmissionQueue(&g_d11.queue, &g_d11.family);
    if (!g_d11.queue) {
        interop->Release();
        log("[nr] D3D11 route: DXVK gave no submission queue; NR does not run on D3D11 (tried again in 5 s)");
        g_d11.retry_ms = GetTickCount64() + 5000;
        return false;
    }
    g_vk.tried = g_vk.ready = true;
    g_vk_is_dxvk = true;
    g_vk.dev.instance = hd.instance; g_vk.dev.physical = hd.physical; g_vk.dev.device = hd.device;
    g_vk.dev.families = {g_d11.family};
    vkGetPhysicalDeviceMemoryProperties(hd.physical, &g_vk.memory);
    nr::pe::vkdevice::enable_queue_locks();   // before the hooks, as on the Vulkan route
    hook_queue_calls(hd.device);   // DXVK's submissions take its own lock too, which our submits take (d11_submit)
    nr::pe::vkdevice::note_device(hd.instance, hd.physical, hd.device, g_vk.dev.families);
    nr::asi::wilsjo2_vk_device(hd.device, hd.physical);
    const int rc = test_fault("d11_init") ? int(NVSDK_NGX_Result_Fail) : nr::dlssnr::vk_init(hd.instance, hd.physical, hd.device);
    g_route_queue.store(g_d11.queue);
    g_route_family.store(g_d11.family);
    g_route_device.store(hd.device);
    // the builds' uploads under DXVK's own submission-queue lock, as the ReShade route's D3D11 path takes it (DXVK's
    // submissions: its lock, then the per-queue one inside our detour of vkQueueSubmit - the same order)
    if (rc == int(NVSDK_NGX_Result_Success))
        nr::dlssnr::vk_queue_lock([interop] { interop->LockSubmissionQueue(); }, [interop] { interop->ReleaseSubmissionQueue(); });
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = g_d11.family;
    bool made = rc == int(NVSDK_NGX_Result_Success) && !test_fault("d11_pool") &&
                vkCreateCommandPool(hd.device, &pci, nullptr, &g_d11.pool) == VK_SUCCESS;
    if (made) {
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = g_d11.pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = D11Route::kRing;
        made = vkAllocateCommandBuffers(hd.device, &ai, g_d11.cmd) == VK_SUCCESS;
        for (int i = 0; made && i < D11Route::kRing; ++i) {
            VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            made = vkCreateFence(hd.device, &fi, nullptr, &g_d11.fence[i]) == VK_SUCCESS;
        }
    }
    if (!made) {
        log("[nr] D3D11 route: the model's Vulkan session or our command buffers could not be made on DXVK's device "
            "(0x%08X); NR does not run - tried again in 5 s", unsigned(rc));
        g_d11.retry_ms = GetTickCount64() + 5000;   // what was made goes with d11_route_release before the next try
        interop->Release();
        return false;
    }
    device->AddRef();
    g_d11.device = device;
    g_d11.interop = interop;
    g_d11.ready = true;
    log("[nr] D3D11 route: NR runs in OptiScaler's D3D11 evaluate (DXVK device %p, queue family %u)",
        static_cast<void*>(hd.device), g_d11.family);
    return true;
}

void d11_route_failed(bool at_once = false);

// One of our command buffers, recording (its last use finished: the ring is 4 deep and each is waited for, at most 2 s).
VkCommandBuffer d11_begin() {
    if (g_d11.recording) discard_session_timing();   // the last one was begun and never submitted
    g_d11.recording = false;
    const int s = g_d11.slot;
    if (!g_d11.fence[s]) {   // a failed submit's fence that could not be made again (d11_submit): made here; else this
        // frame goes on without NR's work and the next tries again (a failure like a failed submit: 30 in a row stop NR
        // on D3D11 until it starts again in 5 s - a stress test: stopping at the first one rebuilt the network each time)
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        const VkResult e = test_fault("d11_fence") ? VK_ERROR_OUT_OF_DEVICE_MEMORY : vkCreateFence(g_vk.dev.device, &fi, nullptr, &g_d11.fence[s]);
        if (e != VK_SUCCESS) {
            g_d11.fence[s] = VK_NULL_HANDLE;
            static bool said = false;
            if (!said) { said = true; log("[nr] D3D11 route: our fence could not be made (%d); that frame goes on without NR's work", int(e)); }
            g_d11.last_error = e;
            d11_route_failed();
            return VK_NULL_HANDLE;
        }
    }
    const VkResult w = test_fault("d11_wait") ? VK_ERROR_DEVICE_LOST : test_fault("d11_timeout") ? VK_TIMEOUT
                       : vkWaitForFences(g_vk.dev.device, 1, &g_d11.fence[s], VK_TRUE, 2000000000ull);
    if (w == VK_TIMEOUT) {
        if (!g_d11.said_wait) {
            g_d11.said_wait = true;
            log("[nr] D3D11 route: our previous command buffer did not finish in 2 s; NR skips frames until it does");
        }
        return VK_NULL_HANDLE;
    }
    if (w != VK_SUCCESS) {   // device lost, out of memory: NR on D3D11 stops (again in 5 s, or on the game's new device)
        log("[nr] D3D11 route: waiting for our previous command buffer returned %d", int(w));
        g_d11.last_error = w;
        d11_route_failed(true);
        return VK_NULL_HANDLE;
    }
    for (IUnknown* r : g_d11.held[s]) r->Release();   // its last use is done
    g_d11.held[s].clear();
    // the fence is reset last: a failure before it leaves the slot signalled (never waited on for nothing)
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VkResult e = vkResetCommandBuffer(g_d11.cmd[s], 0);
    const char* step = "reset";
    if (e == VK_SUCCESS) { e = vkBeginCommandBuffer(g_d11.cmd[s], &bi); step = "begin"; }
    if (e == VK_SUCCESS) { e = vkResetFences(g_vk.dev.device, 1, &g_d11.fence[s]); step = "fence reset"; }
    if (e != VK_SUCCESS) {
        log("[nr] D3D11 route: our command buffer's %s returned %d", step, int(e));
        g_d11.last_error = e;
        d11_route_failed();
        return VK_NULL_HANDLE;
    }
    nr::gpu::reset(g_d11.cmd[s]);
    g_d11.recording = true;
    return g_d11.cmd[s];
}
// Submitted after everything the game's context has recorded so far. False when it was not (the frame then goes on
// without what was recorded: an in-place change of ours never ran).
bool d11_submit(VkCommandBuffer cmd) {
    const int s = g_d11.slot;
    g_d11.recording = false;
    {   // DXVK's later work on what we read or wrote (guide copies, its own images) waits for ours: it does not know of it
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0,
                             nullptr, 0, nullptr);
    }
    VkResult r = vkEndCommandBuffer(cmd);
    const char* step = "end";
    if (r == VK_SUCCESS) {
        g_d11.interop->FlushRenderingCommands();
        g_d11.interop->LockSubmissionQueue();
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
        r = !g_d11.fence[s] ? VK_ERROR_INITIALIZATION_FAILED : test_fault("d11_submit") ? VK_ERROR_OUT_OF_DEVICE_MEMORY
                                                                                     : vkQueueSubmit(g_d11.queue, 1, &si, g_d11.fence[s]);
        step = "submit on DXVK's queue";
        g_d11.interop->ReleaseSubmissionQueue();
    }
    if (r != VK_SUCCESS) {
        static bool said = false;
        if (!said) { said = true; log("[nr] D3D11 route: our command buffer's %s returned %d", step, int(r)); }
        // the fence stays unsignalled: re-made signalled so the slot is not waited on for ever
        vkDestroyFence(g_vk.dev.device, g_d11.fence[s], nullptr);
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (test_fault("d11_fence") || vkCreateFence(g_vk.dev.device, &fi, nullptr, &g_d11.fence[s]) != VK_SUCCESS)
            g_d11.fence[s] = VK_NULL_HANDLE;
    }
    g_d11.slot = (s + 1) % D11Route::kRing;
    if (r == VK_SUCCESS) g_d11.failures = 0;
    else { g_d11.last_error = r; discard_session_timing(); }
    return r == VK_SUCCESS;
}

// Our submit failed: what was recorded (histories, first layout changes of new surfaces) never ran, so nothing of
// the route can be trusted from here: NR stops on D3D11 for this run, said once.
// A failure of our command buffer: that frame goes on without NR's work; the route stops for the run only when the device
// is lost or 30 in a row failed (0.0.7's D3D11 NR, through OptiScaler's D3D12 bridge, had no such latch of its own).
void d11_route_failed(bool at_once) {
    if (!g_d11.ready) return;
    if (!at_once && g_d11.last_error != VK_ERROR_DEVICE_LOST && ++g_d11.failures < 30) {
        static bool said = false;
        if (!said) { said = true; log("[nr] D3D11 route: our command buffer failed (%d); that frame goes on without NR's work", int(g_d11.last_error)); }
        return;
    }
    g_d11.ready = false;
    g_d11.retry_ms = GetTickCount64() + 5000;
    log("[nr] D3D11 route: our command buffers keep failing (%d); NR stops on D3D11, started again in 5 s (or on the "
        "game's new device)", int(g_d11.last_error));
}

// A D3D11 resource of the call as DXVK has it: the NGX Vulkan description the layers read, and its layout.
struct D11Image {
    ID3D11Resource* res = nullptr;
    bool as_resource = true;
    NVSDK_NGX_Resource_VK ngx{};
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageUsageFlags usage = 0;
    bool ok() const { return ngx.Resource.ImageViewInfo.Image != nullptr; }
};
ID3D11Resource* d11_resource(const NVSDK_NGX_Parameter* p, const char* name, bool* as_resource) {
    ID3D11Resource* r = nullptr;
    if (p->Get(name, &r) == NVSDK_NGX_Result_Success && r) { *as_resource = true; return r; }
    void* v = nullptr;
    if (p->Get(name, &v) == NVSDK_NGX_Result_Success && v) { *as_resource = false; return static_cast<ID3D11Resource*>(v); }
    return nullptr;
}
bool d11_image(ID3D11Resource* r, D11Image& out) {
    out.res = r;
    if (!r) return false;
    const nr::pe::ResourceHandle h = nr::pe::d3d11_image(r, g_d11.device);
    if (!h.usable()) return false;
    out.layout = h.layout;
    out.usage = h.usage;
    out.ngx.Type = NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW;
    out.ngx.ReadWrite = (h.usage & VK_IMAGE_USAGE_STORAGE_BIT) != 0;
    auto& i = out.ngx.Resource.ImageViewInfo;
    i.Image = reinterpret_cast<void*>(h.image);
    i.SubresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    i.SubresourceRange.levelCount = 1;
    i.SubresourceRange.layerCount = 1;
    i.Format = uint32_t(h.format);
    i.Width = h.width; i.Height = h.height;
    return true;
}

// ---- Present layers on D3D11 games ---------------------------------------------------------------------------------
// At the Present of the game's swap chain (game_present_before, or NR Control's DXGI hook when the game's own call was
// not reached): the back buffer copied by the game's immediate context into a texture of ours, the Present layers on
// it in our command buffer on DXVK's queue (vk_present_run), the copy back by the context - the order of the layers
// before the upscaler above (copy, FlushRenderingCommands + our submit, the context's next work). NR Control and
// OptiScaler's menu are drawn after (under this call). Guides: the depth and motion textures copied by the context at
// each D3D11 evaluate while the chain has Present layers, into textures cloned from them.
ID3D11Texture2D* g_d11_guides[kPresentGuideSets][2] = {};   // [set][0 depth, 1 motion], g_lock
ID3D11Texture2D* g_d11_target = nullptr;                    // the back buffer's copy

// `copy` a texture like `src` (its format, extent, mips, bind flags), made again when it differs. False: none.
bool d11_clone(ID3D11Texture2D*& copy, ID3D11Resource* src) {
    ID3D11Texture2D* s = nullptr;
    if (!src || FAILED(src->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&s))) || !s) return false;
    D3D11_TEXTURE2D_DESC d{};
    s->GetDesc(&d);
    s->Release();
    if (d.ArraySize != 1 || d.SampleDesc.Count != 1) return false;
    d.Usage = D3D11_USAGE_DEFAULT; d.CPUAccessFlags = 0; d.MiscFlags = 0;
    if (copy) {
        D3D11_TEXTURE2D_DESC have{};
        copy->GetDesc(&have);
        if (have.Format == d.Format && have.Width == d.Width && have.Height == d.Height && have.MipLevels == d.MipLevels &&
            have.BindFlags == d.BindFlags)
            return true;
        retire_texture(copy);
    }
    if (FAILED(g_d11.device->CreateTexture2D(&d, nullptr, &copy))) { copy = nullptr; return false; }
    return true;
}

// At a D3D11 evaluate while the chain has Present layers: its depth and motion copied (by the game's context, before
// the upscaler) into the oldest guide set.
void d11_capture_guides(ID3D11DeviceContext* ctx, const D11Image& depth, const D11Image& motion, const VkFrame& f) {
    VkGuideSet& g = vk_guide_slot(g_d11_present);
    const size_t k = size_t(&g - g_d11_present.sets);
    if (!d11_clone(g_d11_guides[k][0], depth.res) || !d11_clone(g_d11_guides[k][1], motion.res)) { g.serial = 0; return; }
    ctx->CopyResource(g_d11_guides[k][0], depth.res);
    ctx->CopyResource(g_d11_guides[k][1], motion.res);
    vk_guide_stamp(g_d11_present, g, f);
    g.w2 = nr::asi::Wilsjo2VkFrame{};
    g.w2.flags = f.flags_known ? f.flags : 0u;
    g.w2.reset = f.reset;
    g.w2.mv_x = std::isfinite(f.mv_x) && f.mv_x != 0.0f ? f.mv_x : 1.0f;
    g.w2.mv_y = std::isfinite(f.mv_y) && f.mv_y != 0.0f ? f.mv_y : 1.0f;
    // the guide regions as wilsjo2_vk_frame clips them
    // (display-size vectors the image cannot hold: the part it holds, as wilsjo2_vk_frame)
    const bool low_res_mv = !f.flags_known || (f.flags & kFlagMvLowRes) != 0;
    g.w2.depth_rect = vk_guide_subrect(f.depth, f.subrect_w, f.subrect_h, f.depth_rect.x, f.depth_rect.y);
    g.w2.motion_rect = vk_guide_subrect(f.motion, low_res_mv ? f.subrect_w : f.out_w, low_res_mv ? f.subrect_h : f.out_h,
                                        f.motion_rect.x, f.motion_rect.y);
}

// Everything of the D3D11 route's on DXVK's device goes, the device's references too (g_lock held), so the route can
// start on another device or again on this one: once our submitted work there is done (false when it does not finish
// in 2 s - asked again later). `why` is said.
bool d11_route_release(const char* why) {
    if (g_d11.interop) g_d11.interop->FlushRenderingCommands();
    for (int i = 0; i < D11Route::kRing; ++i) {   // (a lost device has nothing running any more)
        const VkResult w = g_d11.fence[i] ? vkWaitForFences(g_vk.dev.device, 1, &g_d11.fence[i], VK_TRUE, 2000000000ull) : VK_SUCCESS;
        if (w != VK_SUCCESS && w != VK_ERROR_DEVICE_LOST) return false;
    }
    // Session teardown also joins pending builders. No record is reusable after its pool is destroyed.
    for (auto cmd : g_d11.cmd) if (cmd) nr::gpu::reset(cmd);
    for (auto& [id, t] : g_d11_features) d11_forget(t);
    retire_present_path(g_d11_present);
    g_d11_present.chain_version = 0;
    for (auto& set : g_d11_guides) for (auto& r : set) retire_texture(r);
    retire_texture(g_d11_target);
    if (g_vk_is_dxvk) {
        nr::dlssnr::vk_device_destroyed();
        nr::asi::wilsjo2_vk_device_destroyed();
        end_receipts(g_vk.dev.device);
        for (auto& r : g_retired_images) destroy_image(r.im);
        g_retired_images.clear();
    }
    for (auto& r : g_d11_retired) r.t->Release();
    g_d11_retired.clear();
    for (int i = 0; i < D11Route::kRing; ++i) {
        for (IUnknown* r : g_d11.held[i]) r->Release();
        if (g_d11.fence[i]) vkDestroyFence(g_vk.dev.device, g_d11.fence[i], nullptr);
    }
    if (g_d11.pool) vkDestroyCommandPool(g_vk.dev.device, g_d11.pool, nullptr);
    if (g_d11.interop) g_d11.interop->Release();
    if (g_d11.device) g_d11.device->Release();
    g_d11 = D11Route{};
    if (g_vk_is_dxvk) {
        g_vk = VkRoute{};
        g_vk_is_dxvk = false;
        g_route_device.store(VK_NULL_HANDLE); g_route_queue.store(VK_NULL_HANDLE);
    }
    log("[nr] D3D11 route: %s", why);
    return true;
}

// A helper/off-screen device may evaluate first. The device whose swap chain actually presents gets the route;
// finish our submitted work before moving the single Vulkan session and its private images to that device.
bool d11_select_present_device(ID3D11Device* device) {
    if (!g_d11.device || g_d11.device == device) return true;
    if (!g_vk_is_dxvk || !g_d11.interop) return false;
    return d11_route_release("the presenting device changed; NR follows its frame");
}

bool d11_present_due() { return vk_present_due(g_d11_present); }

void d11_present_layers(IDXGISwapChain* sc, ID3D11Device* device) {
    nr::pe::watchdog::Phase phase("Present layers (D3D11)");
    std::unique_lock<std::mutex> guard(g_lock);
    const nr::asi::hudcap::Own own;   // its commands pass HUD FIX's capture hooks unseen
    VkPresentPath& P = g_d11_present;
    ++P.presents;
    tick_d11_retired();
    tick_vk_retired();
    nr::asi::wilsjo2_vk_tick();
    const std::shared_ptr<const Chain> chain = chain_now();
    const std::vector<const Layer*> layers = vk_present_plan(P, chain);
    if (!d11_select_present_device(device)) return;
    if (layers.empty()) {
        nr::asi::hudcap::d3d11_present(nullptr, nullptr, DXGI_FORMAT_UNKNOWN, 0, 0, false);
        hud_report(false, true);
        return;
    }
    if (GetTickCount64() - g_present.evaluate_ms.load() > 1000) nr::dlssnr::frame_keys();   // as on D3D12
    if (!d11_route_ready(device)) {
        for (const Layer* l : layers) layer_note(*l, 2, 3, "NR cannot run on this D3D11 device (see the log)");
        P.served_ms = GetTickCount64();
        return;
    }
    ID3D11Texture2D* back = nullptr;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back))) || !back) return;
    struct Back { ID3D11Texture2D* t; ~Back() { t->Release(); } } back_hold{back};
    D3D11_TEXTURE2D_DESC bd{};
    back->GetDesc(&bd);
    const DXGI_FORMAT format = writable(bd.Format);
    const uint32_t w = bd.Width, h = bd.Height;
    const bool plain = bd.ArraySize == 1 && bd.SampleDesc.Count == 1 && bd.MipLevels == 1;
    int space = 0;
    const bool space_set = nr::pe::menu::colour_space_of(sc, &space);
    static ULONGLONG polled = 0;
    static float windows_white = 0.f;
    if (const ULONGLONG now = GetTickCount64(); !polled || now - polled >= 1000) { polled = now; windows_white = sdr_white(sc); }
    bool unusable = false;
    const std::vector<VkPresentRun> runs = present_runs(layers, space, space_set, format, windows_white, plain, &unusable);
    if (unusable != P.unusable) {
        P.unusable = unusable;
        log(unusable ? "[nr] Present layers (D3D11): this swap chain cannot be served (colour space %d%s, format %u); the "
                       "RenoDX ones do not run until it can (never in another place)"
                     : "[nr] Present layers (D3D11): the swap chain can be served again (colour space %d%s, format %u)",
            space, space_set ? "" : " (not set)", unsigned(bd.Format));
    }
    P.served_ms = GetTickCount64();
    // HUD FIX's capture (nr_asi_hudcap.hpp): armed for Auto and Captured, its verdict on this frame
    const uint32_t hud_mode = chain->hud.mode;
    const nr::asi::hudcap::Frame11 hud_cap = nr::asi::hudcap::d3d11_present(
        device, back, format, w, h, !runs.empty() && (hud_mode == 1 || hud_mode == 3));
    const bool hud_captured = hud_cap.result == nr::asi::hudcap::Result::kCaptured && hud_cap.hudless;
    hud_report(!runs.empty(), true, hud_captured, hud_cap.why, false, false, std::string(), "", hud_cap.broken);
    if (runs.empty()) return;
    // the back buffer's copy, a texture NR can write (an sRGB back buffer: its UNORM twin; CopyResource takes the pair)
    D3D11_TEXTURE2D_DESC have{};
    if (g_d11_target) g_d11_target->GetDesc(&have);
    if (!g_d11_target || have.Format != format || have.Width != w || have.Height != h) {
        retire_texture(g_d11_target);
        D3D11_TEXTURE2D_DESC d{};
        d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = format;
        d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        if (FAILED(g_d11.device->CreateTexture2D(&d, nullptr, &g_d11_target))) {
            g_d11_target = nullptr;
            static DXGI_FORMAT said = DXGI_FORMAT_UNKNOWN;
            if (said != format) { said = format; log("[nr] Present layers (D3D11): the back buffer's copy (DXGI format %d) could not be made", int(format)); }
            for (const VkPresentRun& r : runs) layer_note(*r.layer, 2, 3, "Present cannot serve this swap chain's colour format");
            return;
        }
    }
    D11Image target;
    if (!d11_image(g_d11_target, target)) return;
    // the guide sets as DXVK has them now
    for (uint32_t k = 0; k < kPresentGuideSets; ++k) {
        VkGuideSet& g = P.sets[k];
        if (!g.serial) continue;
        D11Image d, m;
        if (!d11_image(g_d11_guides[k][0], d) || !d11_image(g_d11_guides[k][1], m)) { g.serial = 0; continue; }
        g.depth = vk_ref(&d.ngx, d.layout, false, d.usage);
        g.motion = vk_ref(&m.ngx, m.layout, false, m.usage);
        g.w2.depth = w2_image(&d.ngx, d.layout);
        g.w2.motion = w2_image(&m.ngx, m.layout);
    }
    ID3D11DeviceContext* ctx = nullptr;
    device->GetImmediateContext(&ctx);
    if (!ctx) return;
    struct Ctx { ID3D11DeviceContext* c; ~Ctx() { c->Release(); } } ctx_hold{ctx};
    ctx->CopyResource(g_d11_target, back);
    const VkCommandBuffer cmd = d11_begin();
    if (!cmd) return;
    nr::gpu::Scope recording(nr::gpu::recording(cmd));
    g_native_uses.touch();
    d11_hold(g_d11_target);
    for (auto& set : g_d11_guides) for (ID3D11Texture2D* t : set) d11_hold(t);
    const VkImage ti = reinterpret_cast<VkImage>(target.ngx.Resource.ImageViewInfo.Image);
    const VkFormat tf = VkFormat(target.ngx.Resource.ImageViewInfo.Format);
    if (target.layout != VK_IMAGE_LAYOUT_GENERAL) vk_barrier(cmd, ti, target.layout, VK_IMAGE_LAYOUT_GENERAL);
    VkHudless hudless;   // Captured: the frame copied before its HUD, as DXVK has it now
    D11Image hl;
    if (hud_captured && d11_image(hud_cap.hudless, hl)) {
        d11_hold(hud_cap.hudless);
        hudless.image = w2_image(&hl.ngx, hl.layout);
        hudless.estimate_too = hud_mode == 1;
    }
    // Captured with no copy this frame: the layers' answer as it is (no estimate)
    const bool changed = vk_present_run(cmd, P, chain, runs, ti, tf, w, h, hudless);
    if (target.layout != VK_IMAGE_LAYOUT_GENERAL) vk_barrier(cmd, ti, VK_IMAGE_LAYOUT_GENERAL, target.layout);
    if (!d11_submit(cmd)) { d11_route_failed(); return; }
    if (changed) ctx->CopyResource(back, g_d11_target);
}

NVSDK_NGX_Result NR_NGX_MSABI hk_d11_create(ID3D11DeviceContext* ctx, NVSDK_NGX_Feature feature, NVSDK_NGX_Parameter* p,
                                           NVSDK_NGX_Handle** out) {
    const NVSDK_NGX_Result r = real_d11_create(ctx, feature, p, out);
    if (r == NVSDK_NGX_Result_Success && out && *out) {
        std::lock_guard<std::mutex> guard(g_lock);
        D11Tracked& t = g_d11_features[(*out)->Id];
        d11_forget(t);
        t = D11Tracked{};
        t.vk.feature = int(feature);
        t.vk.create = create_sizes(p);
        int flags = 0;
        if (p && p->Get("DLSS.Feature.Create.Flags", &flags) == NVSDK_NGX_Result_Success) {
            t.vk.flags = uint32_t(flags);
            t.vk.flags_known = true;
        }
        log("[nr] OptiScaler D3D11 feature %u made: NGX feature %d, flags 0x%x%s", (*out)->Id, int(feature), t.vk.flags,
            t.vk.flags_known ? "" : " (not given)");
    }
    return r;
}
NVSDK_NGX_Result NR_NGX_MSABI hk_d11_release(NVSDK_NGX_Handle* h) {
    if (h) {
        std::lock_guard<std::mutex> guard(g_lock);
        auto it = g_d11_features.find(h->Id);
        if (it != g_d11_features.end()) { d11_forget(it->second); g_d11_features.erase(it); }
    }
    return real_d11_release(h);
}

thread_local int t_d11_inside = 0;

NVSDK_NGX_Result NR_NGX_MSABI hk_d11_evaluate(ID3D11DeviceContext* ctx, const NVSDK_NGX_Handle* h, NVSDK_NGX_Parameter* p,
                                             void* callback) {
    if (t_d11_inside || !ctx || !h || !p) return real_d11_evaluate(ctx, h, p, callback);
    struct Inside { Inside() { ++t_d11_inside; } ~Inside() { --t_d11_inside; } } inside;
    nr::pe::watchdog::Phase phase("OptiScaler's D3D11 evaluate (NR)");
    std::unique_lock<std::mutex> guard(g_lock);
    tick_d11_retired();
    tick_vk_retired();
    nr::asi::wilsjo2_vk_tick();
    D11Tracked& t = g_d11_features[h->Id];
    if (t.vk.feature < 0) {
        HMODULE caller = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(__builtin_return_address(0)), &caller);
        t.vk.feature = caller == g_opti ? kFeatureSuperSampling : unseen_feature([p](const char* n) {
            bool as_resource = false;
            return d11_resource(p, n, &as_resource) != nullptr;
        });
        log("[nr] OptiScaler D3D11 feature %u first seen at an evaluate from %s: taken as %s", h->Id,
            caller == g_opti ? "OptiScaler's own input" : "the game (its create not seen; by what the call carries)",
            feature_name(t.vk.feature));
    }
    if (t.vk.feature != kFeatureSuperSampling && t.vk.feature != kFeatureRayReconstruction) {
        guard.unlock();
        return real_d11_evaluate(ctx, h, p, callback);
    }
    ID3D11Device* device = nullptr;
    ctx->GetDevice(&device);
    // NR Control, drawn by this module over the game's D3D11 swap chain (nr_pe_menu.hpp's D3D11 overlay), from the
    // first frame NR sees, as on D3D12
    menu_once11();
    const bool ready = device && d11_route_ready(device);
    if (device) device->Release();
    if (!ready) {
        guard.unlock();
        return real_d11_evaluate(ctx, h, p, callback);
    }
    // The call's resources (DLSS-D blocks may name them DLSSD.*), as DXVK has them.
    D11Image colour, output, depth, motion, exposure;
    VkFrame f;
    bool got = true;
    {
        ID3D11Resource* c = d11_resource(p, "Color", &colour.as_resource);
        if (!c && (c = d11_resource(p, "DLSSD.Color", &colour.as_resource))) f.color_key = "DLSSD.Color";
        bool unused = true;
        ID3D11Resource* o = d11_resource(p, "Output", &unused);
        if (!o) o = d11_resource(p, "DLSSD.Output", &unused);
        ID3D11Resource* d = d11_resource(p, "Depth", &unused);
        if (!d) d = d11_resource(p, "DLSSD.Depth", &unused);
        ID3D11Resource* m = d11_resource(p, "MotionVectors", &unused);
        if (!m) m = d11_resource(p, "DLSSD.MotionVectors", &unused);
        ID3D11Resource* e = d11_resource(p, "ExposureTexture", &unused);
        got = d11_image(c, colour) && d11_image(o, output) && d11_image(d, depth) && d11_image(m, motion);
        if (e && d11_image(e, exposure)) {
            // a typeless exposure (DXVK makes it an integer image of that size, its format changeable) read as the
            // float it holds, as the 0.0.7 route's D3D12 view of it (R32_TYPELESS as R32_FLOAT)
            ID3D11Texture2D* tex = nullptr;
            if (SUCCEEDED(e->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex))) && tex) {
                D3D11_TEXTURE2D_DESC ed{};
                tex->GetDesc(&ed);
                tex->Release();
                auto& fmt = exposure.ngx.Resource.ImageViewInfo.Format;
                if (ed.Format == DXGI_FORMAT_R32_TYPELESS) fmt = uint32_t(VK_FORMAT_R32_SFLOAT);
                else if (ed.Format == DXGI_FORMAT_R16_TYPELESS) fmt = uint32_t(VK_FORMAT_R16_SFLOAT);
            }
        }
    }
    if (!got) {
        if (!t.vk.said) {
            t.vk.said = true;
            log("[nr] OptiScaler D3D11 feature %u: no colour, output, depth or motion vector texture DXVK can show; NR "
                "does not run", h->Id);
        }
        guard.unlock();
        return real_d11_evaluate(ctx, h, p, callback);
    }
    f.color = &colour.ngx; f.output = &output.ngx; f.depth = &depth.ngx; f.motion = &motion.ngx;
    f.exposure = exposure.ok() ? &exposure.ngx : nullptr;
    f.color_layout = colour.layout; f.output_layout = output.layout; f.depth_layout = depth.layout;
    f.motion_layout = motion.layout; f.exposure_layout = exposure.layout;
    f.output_usage = output.usage; f.depth_usage = depth.usage; f.motion_usage = motion.usage;
    f.flags_known = t.vk.flags_known;
    f.flags = t.vk.flags;
    if (!f.flags_known) {
        int flags = 0;
        if (p->Get("DLSS.Feature.Create.Flags", &flags) == NVSDK_NGX_Result_Success) { f.flags = uint32_t(flags); f.flags_known = true; }
    }
    read_frame_dims(p, f, t.vk.create);
    const bool nr_on = nr::dlssnr::frame_keys();
    VkPlan plan;
    plan_layers_vk(t.vk, f, nr_on, "D3D11 games", plan, g_d11_present);
    g_d11_present.wanted = plan.present;
    g_present.evaluate_ms = GetTickCount64();
    if (plan.present) d11_capture_guides(ctx, depth, motion, f);
    const nr::asi::Wilsjo2VkFrame wf = wilsjo2_vk_frame(p, f);
    ++g_fg_frames;
    NVSDK_NGX_Result r;
    if (!plan.before.empty()) {
        // The colour's region into a texture of ours by the game's context, the layers on it in our command buffer,
        // that texture handed to the upscaler in the colour's place (base 0); the game's colour is never written.
        const uint32_t rx = plan.before_renodx ? f.color_x : 0u, ry = plan.before_renodx ? f.color_y : 0u;
        const uint32_t rw = plan.before_renodx ? f.render_w : plan.w2_aw, rh = plan.before_renodx ? f.render_h : plan.w2_ah;
        ID3D11Texture2D* src = nullptr;
        D3D11_TEXTURE2D_DESC cd{};
        if (SUCCEEDED(colour.res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&src))) && src) {
            src->GetDesc(&cd);
            src->Release();
        }
        D3D11_TEXTURE2D_DESC have{};
        if (t.copy) t.copy->GetDesc(&have);
        if (!t.copy || have.Format != cd.Format || have.Width != rw || have.Height != rh) {
            retire_texture(t.copy);
            D3D11_TEXTURE2D_DESC d{};
            d.Width = rw; d.Height = rh; d.MipLevels = 1; d.ArraySize = 1; d.Format = cd.Format;
            d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT;
            d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
            if (!cd.Format || FAILED(g_d11.device->CreateTexture2D(&d, nullptr, &t.copy))) {
                t.copy = nullptr;
                static bool said = false;
                if (!said) { said = true; log("[nr] D3D11 route: the colour's copy (DXGI format %d) could not be made; NR "
                                              "before the upscaler does not run", int(cd.Format)); }
            }
        }
        D11Image copy;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        if (t.copy && d11_image(t.copy, copy)) {
            const D3D11_BOX box{rx, ry, 0, rx + rw, ry + rh, 1};
            ctx->CopySubresourceRegion(t.copy, 0, 0, 0, 0, colour.res, 0, &box);
            cmd = d11_begin();
        }
        if (cmd) {
            nr::gpu::Scope recording(nr::gpu::recording(cmd));
            g_native_uses.touch();
            for (IUnknown* r : {static_cast<IUnknown*>(t.copy), static_cast<IUnknown*>(depth.res),
                                static_cast<IUnknown*>(motion.res), static_cast<IUnknown*>(exposure.res)})
                d11_hold(r);
            const VkImage ci = reinterpret_cast<VkImage>(copy.ngx.Resource.ImageViewInfo.Image);
            if (copy.layout != VK_IMAGE_LAYOUT_GENERAL) vk_barrier(cmd, ci, copy.layout, VK_IMAGE_LAYOUT_GENERAL);
            nr::dlssnr::VkImageRef ref;
            ref.image = uint64_t(reinterpret_cast<uintptr_t>(ci));
            ref.format = copy.ngx.Resource.ImageViewInfo.Format; ref.width = rw; ref.height = rh;
            ref.layout = uint32_t(VK_IMAGE_LAYOUT_GENERAL);
            ref.usage = copy.usage;
            run_before_vk(cmd, t.vk, plan.before, f, wf, nr_on, ref, rw, rh, plan.chain->inspect,
                          plan.after.empty() && !plan.present_takes);
            if (copy.layout != VK_IMAGE_LAYOUT_GENERAL) vk_barrier(cmd, ci, VK_IMAGE_LAYOUT_GENERAL, copy.layout);
            if (!d11_submit(cmd)) d11_route_failed();   // the copy holds the colour as it came
            ID3D11Texture2D* mine = t.copy;
            guard.unlock();
            if (colour.as_resource) p->Set(f.color_key, static_cast<ID3D11Resource*>(mine));
            else p->Set(f.color_key, static_cast<void*>(mine));
            const bool moved = rx || ry;
            if (moved) { p->Set("DLSS.Input.Color.Subrect.Base.X", 0u); p->Set("DLSS.Input.Color.Subrect.Base.Y", 0u); }
            r = real_d11_evaluate(ctx, h, p, callback);
            if (colour.as_resource) p->Set(f.color_key, colour.res); else p->Set(f.color_key, static_cast<void*>(colour.res));
            if (moved) { p->Set("DLSS.Input.Color.Subrect.Base.X", f.color_x); p->Set("DLSS.Input.Color.Subrect.Base.Y", f.color_y); }
        } else {
            guard.unlock();
            r = real_d11_evaluate(ctx, h, p, callback);
        }
    } else {
        guard.unlock();
        r = real_d11_evaluate(ctx, h, p, callback);
    }
    if (r != NVSDK_NGX_Result_Success) return r;
    if (plan.after.empty()) return r;
    // After the upscaler: the layers on its output in our command buffer, behind the upscaler's work.
    guard.lock();
    if (!g_d11.ready) return r;   // the route stopped (a failure before the upscaler, or another thread's)
    const auto it = g_d11_features.find(h->Id);
    if (it == g_d11_features.end()) return r;
    D11Tracked& t2 = it->second;
    const VkCommandBuffer cmd = d11_begin();
    if (!cmd) return r;
    nr::gpu::Scope recording(nr::gpu::recording(cmd));
    g_native_uses.touch();
    for (IUnknown* res : {static_cast<IUnknown*>(output.res), static_cast<IUnknown*>(depth.res),
                          static_cast<IUnknown*>(motion.res), static_cast<IUnknown*>(exposure.res)})
        d11_hold(res);
    const VkImage oi = reinterpret_cast<VkImage>(output.ngx.Resource.ImageViewInfo.Image);
    const VkImageLayout kept = f.output_layout;
    if (kept != VK_IMAGE_LAYOUT_GENERAL) vk_barrier(cmd, oi, kept, VK_IMAGE_LAYOUT_GENERAL);
    f.output_layout = VK_IMAGE_LAYOUT_GENERAL;
    run_after_vk(cmd, t2.vk, plan.after, f, wf, nr_on, plan.chain->inspect, !plan.present_takes);
    if (kept != VK_IMAGE_LAYOUT_GENERAL) vk_barrier(cmd, oi, VK_IMAGE_LAYOUT_GENERAL, kept);
    if (!d11_submit(cmd)) d11_route_failed();
    return r;
}

bool hook(const char* name, void* detour, void** real, bool required) {
    void* fn = reinterpret_cast<void*>(GetProcAddress(g_opti, name));
    if (!fn) {
        if (required) log("[nr] OptiScaler exports no %s", name);
        return false;
    }
    const MH_STATUS c = MH_CreateHook(fn, detour, real);
    const MH_STATUS e = c == MH_OK ? MH_EnableHook(fn) : c;
    if (e != MH_OK) log("[nr] %s could not be hooked (%s)", name, MH_StatusToString(e));
    return e == MH_OK;
}

}  // namespace

// Called by OptiScaler from its DllMain, under the loader lock: hooks only.
extern "C" __declspec(dllexport) void InitializeASI() {
    static bool done = false;
    if (done) return;
    done = true;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(__builtin_return_address(0)), &g_opti);
    if (!g_opti) g_opti = GetModuleHandleW(nullptr);
    nr::pe::set_module(g_opti);   // dlssnr-amd.ini, dlssnr-amd\, the log and control.bin sit beside OptiScaler
    {   // and RenoDX's NR screenshots go to <game>\NR Screenshots, as on the OptiScaler route
        wchar_t folder[MAX_PATH * 2] = {};
        MultiByteToWideChar(CP_ACP, 0, nr::pe::module_folder(), -1, folder, MAX_PATH * 2);
        nr::pe::screenshot::set_folder(std::wstring(folder) + L"\\NR Screenshots");
    }
    char opti[MAX_PATH] = {};
    GetModuleFileNameA(g_opti, opti, MAX_PATH);
    log("[nr] dlssnr-amd.asi %s loaded by %s", NR_BUILD_STAMP, opti);
    nr::pe::control::asi_facts(NR_BUILD_STAMP, nullptr);
    nr::pe::watchdog::start();   // its thread runs once OptiScaler's DllMain has returned
    judge_executable();
    read_ini_barriers();
    nr::dlssnr::keep_caller_state();   // the RenoDX style is said per evaluate (renodx_route::CallStyle), not set here
    const MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
        log("[nr] MinHook could not start (%s): NR does not run", MH_StatusToString(init));
        return;
    }
    const bool ok = hook("NVSDK_NGX_D3D12_EvaluateFeature", reinterpret_cast<void*>(&hk_evaluate),
                         reinterpret_cast<void**>(&real_evaluate), true) &&
                    hook("NVSDK_NGX_D3D12_CreateFeature", reinterpret_cast<void*>(&hk_create),
                         reinterpret_cast<void**>(&real_create), true) &&
                    hook("NVSDK_NGX_D3D12_ReleaseFeature", reinterpret_cast<void*>(&hk_release),
                         reinterpret_cast<void**>(&real_release), true);
    // Vulkan games (the Vulkan route above): optional, OptiScaler exports them all
    const bool vk_ok = hook("NVSDK_NGX_VULKAN_EvaluateFeature", reinterpret_cast<void*>(&hk_vk_evaluate),
                            reinterpret_cast<void**>(&real_vk_evaluate), false) &&
                       hook("NVSDK_NGX_VULKAN_ReleaseFeature", reinterpret_cast<void*>(&hk_vk_release),
                            reinterpret_cast<void**>(&real_vk_release), false);
    hook("NVSDK_NGX_VULKAN_CreateFeature1", reinterpret_cast<void*>(&hk_vk_create1), reinterpret_cast<void**>(&real_vk_create1),
         false);
    hook("NVSDK_NGX_VULKAN_CreateFeature", reinterpret_cast<void*>(&hk_vk_create), reinterpret_cast<void**>(&real_vk_create),
         false);
    if (!vk_ok) log("[nr] OptiScaler's Vulkan evaluate could not be hooked: no NR on Vulkan games");
    hook("NVSDK_NGX_VULKAN_Init", reinterpret_cast<void*>(&hk_vk_init<0>), reinterpret_cast<void**>(&real_vk_init[0]), false);
    hook("NVSDK_NGX_VULKAN_Init_Ext", reinterpret_cast<void*>(&hk_vk_init<1>), reinterpret_cast<void**>(&real_vk_init[1]), false);
    hook("NVSDK_NGX_VULKAN_Init_Ext2", reinterpret_cast<void*>(&hk_vk_init<2>), reinterpret_cast<void**>(&real_vk_init[2]), false);
    hook("NVSDK_NGX_VULKAN_Init_ProjectID", reinterpret_cast<void*>(&hk_vk_init_project<0>),
         reinterpret_cast<void**>(&real_vk_init_project[0]), false);
    hook("NVSDK_NGX_VULKAN_Init_ProjectID_Ext", reinterpret_cast<void*>(&hk_vk_init_project<1>),
         reinterpret_cast<void**>(&real_vk_init_project[1]), false);
    // D3D11 games (the D3D11 route above): optional too
    const bool d11_ok = hook("NVSDK_NGX_D3D11_EvaluateFeature", reinterpret_cast<void*>(&hk_d11_evaluate),
                             reinterpret_cast<void**>(&real_d11_evaluate), false) &&
                        hook("NVSDK_NGX_D3D11_ReleaseFeature", reinterpret_cast<void*>(&hk_d11_release),
                             reinterpret_cast<void**>(&real_d11_release), false);
    hook("NVSDK_NGX_D3D11_CreateFeature", reinterpret_cast<void*>(&hk_d11_create), reinterpret_cast<void**>(&real_d11_create),
         false);
    if (!d11_ok) log("[nr] OptiScaler's D3D11 evaluate could not be hooked: no NR on D3D11 games");
    hook("NVSDK_NGX_D3D12_Init_ProjectID", reinterpret_cast<void*>(&hk_init_project_id),
         reinterpret_cast<void**>(&real_init_project_id), false);
    hook("NVSDK_NGX_D3D12_Init_with_ProjectID", reinterpret_cast<void*>(&hk_init_with_project_id),
         reinterpret_cast<void**>(&real_init_with_project_id), false);
    hook_device_creation();
    hook_present_beat();
    hook_game_dxgi();
    log("[nr] %s (Unreal Engine by the executable: %s)", ok ? "NR runs in OptiScaler's D3D12 evaluate"
                                                             : "OptiScaler's D3D12 evaluate could not be hooked: NR does not run",
        g_unreal_by_exe ? "yes" : "no");
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = module;
        DisableThreadLibraryCalls(module);
    } else if (reason == DLL_PROCESS_DETACH && reserved) {
        // The process is ending (before this module's static destructors run): the game's other threads are gone,
        // possibly inside vkd3d-proton's / DXVK's locks, and d3d12 / winevulkan were detached before us (loaded after
        // us). Nothing of NR's GPU state is destroyed or waited for now - that could wait for ever and the game would
        // never end; the process's end frees it all.
        nr::dlssnr::abandon_at_exit();
        nr::asi::wilsjo2_abandon_at_exit();
        nr::asi::wilsjo2_vk_abandon_at_exit();
        nr::pe::menu::abandon_at_exit();
    }
    return TRUE;
}
