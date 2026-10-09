#!/usr/bin/env bash
# Build the Windows-side package (cross-compiled PE, run under Proton):
#   dlssnr-amd/          the network: model, shaders (linux/build/assemble_product_data.sh)
#   reshade/             the ReShade add-on route (Feeder-style motion vectors)
#   vulkan/              ReShade as a Vulkan layer, for Vulkan and D3D9 games
#   optiscaler/          OptiScaler (the official nightly, unmodified) and our NR plugin for it (x86_64 only)
#   model-tools/         rebuilds the model from nvngx_dlssnr.dll at install time
#   experimental/        makes CC_SMALL_FP8_EXPERIMENTAL's weights from the small model at install time
#   install.sh, README.txt
#
#   NR_ARCH=x86_64|i686 bash linux/build/build_package.sh [build dir]
#   NR_MODEL=dlssnr.bin put this model in the package (made from your own DLL by
#                       linux/package/model-tools/extract_model.sh); without it
#                       install.sh needs --dll
#   NR_MODEL_SMALL=dlssnr-small.bin  with NR_MODEL: the new model too (from the 310.9.16 DLL)
#   NR_GPU=rdna3|rdna4  the network to build (linux/build/arch/); default rdna4. An rdna3 package
#                       carries -rdna3 in its name
#   NR_VERSION=...      override the version (default: from the git tags, linux/build/version.sh)
#
# Packages also carry int4/, the int4 mixed option install.sh adds when asked for: the ReShade add-on built again
# with NR_INT4=1 (this script with NR_INT4=1 NR_PROGRAMS_ONLY=1), the int4 network, its data, the Vulkan layer and the
# install-time weights generator (build_int4.sh), and its dlssnr-amd.ini.
set -euo pipefail
model=${NR_MODEL:+$(realpath -- "$NR_MODEL")}
small=${NR_MODEL_SMALL:+$(realpath -- "$NR_MODEL_SMALL")}
cd -- "$(dirname -- "$0")/../.."
arch=${NR_ARCH:-x86_64}
case "$arch" in x86_64|i686) ;; *) echo "NR_ARCH must be x86_64 or i686" >&2; exit 2;; esac
if [[ "$arch" == i686 ]]; then default_out=artifacts/package-build/nr32; bits=32; else default_out=artifacts/package-build/nr; bits=64; fi
out=$(realpath -m -- "${1:-$default_out}")
case "$out" in "$(pwd)"/*) ;; *) echo 'build directory must be in the project' >&2; exit 2;; esac
mkdir -p -- "$out"

cxx=${NR_MINGW:-$arch-w64-mingw32-g++}
cc=${NR_MINGW_CC:-$arch-w64-mingw32-gcc}
command -v "$cxx" >/dev/null || { echo "no mingw cross compiler ($cxx)" >&2; exit 1; }
minhook=artifacts/ref/DLSS5-Feeder/external/minhook
reshade_inc=artifacts/ref/DLSS5-Feeder/external/reshade/include
for dir in "$minhook" "$reshade_inc" artifacts/ref/reshade-shaders artifacts/ref/vort_Shaders; do
    [[ -d "$dir" ]] || { echo "missing reference tree: $dir" >&2; exit 1; }
done
gpu=${NR_GPU:-rdna4}
[[ -f "linux/build/arch/$gpu.sh" ]] || { echo "NR_GPU must be rdna3 or rdna4" >&2; exit 2; }
source "linux/build/arch/$gpu.sh"

# ---- compile ----------------------------------------------------------------
common=(-std=c++17 -O2 -DNDEBUG -Itoolchain/Vulkan-Headers/include -Ilinux/src -Ilinux/src/core -Ilinux/src/layer -Ilinux/src/pe -I"$out")
[[ "${NR_INT4:-0}" == 1 ]] && common+=(-DNR_INT4=1)   # the int4 mixed build of the programs (int4/ below)
"$cxx" "${common[@]}" "${NR_PRODUCT_DEFINES[@]}" -c linux/src/core/nr_runtime.cpp -o "$out/nr_runtime.o"
"$cxx" "${common[@]}" -c linux/src/core/nr_native_plan.cpp -o "$out/nr_native_plan.o"
"$cxx" "${common[@]}" -c linux/src/core/nr_renodx.cpp -o "$out/nr_renodx.o"
"$cxx" "${common[@]}" -c linux/src/core/nr_wilsjo2.cpp -o "$out/nr_wilsjo2.o"   # its Stage: HUD FIX on this route
"$cxx" "${common[@]}" -c linux/src/pe/nr_pe_log.cpp -o "$out/log.o"
"$cxx" "${common[@]}" -c linux/src/pe/nr_pe_interop.cpp -o "$out/interop.o"
"$cxx" "${common[@]}" -c linux/src/pe/nr_vram_ledger.cpp -o "$out/vram_ledger.o"
"$cxx" "${common[@]}" -I"$minhook/include" -c linux/src/pe/nr_pe_vkdevice.cpp -o "$out/vkdevice.o"
"$cxx" "${common[@]}" -c linux/src/pe/nr_pe_session.cpp -o "$out/session.o"
"$cxx" "${common[@]}" -c linux/src/pe/nr_pe_config.cpp -o "$out/config.o"
"$cxx" "${common[@]}" -c linux/src/pe/nr_screenshot.cpp -o "$out/screenshot.o"
# ReShade's headers include <Windows.h>; the case shim maps that onto mingw's.
"$cxx" "${common[@]}" -Ilinux/src/pe/reshade_compat -I"$reshade_inc" -I"$minhook/include" \
    -Iartifacts/ref/DLSS5-Feeder/external/imgui -Ilinux/src/ui -c linux/src/pe/nr_reshade_addon.cpp -o "$out/reshade_addon.o"
for unit in hook buffer trampoline; do
    "$cc" -O2 -I"$minhook/include" -I"$minhook/src" -c "$minhook/src/$unit.c" -o "$out/mh_$unit.o"
done
hde=hde64; [[ "$arch" == i686 ]] && hde=hde32
"$cc" -O2 -I"$minhook/include" -I"$minhook/src" -c "$minhook/src/hde/$hde.c" -o "$out/mh_$hde.o"
ldflags=(-ld3d12 -ldxgi -ldwmapi -lgdi32 -lole32 -static -static-libgcc -static-libstdc++)
[[ "$arch" == i686 ]] && ldflags+=(-Wl,--kill-at)   # undecorated stdcall exports

# ---- Vulkan entry points ------------------------------------------------------
# The DLLs must not import vulkan-1.dll: a static import loads winevulkan when
# the DLL loads, before the game has set up its own graphics stack (Monster
# Hunter Wilds dies from that). Every vk* call goes through a pointer instead,
# filled by nr_vk_load() once the game has loaded Vulkan. The symbol list is
# whatever a trial link reports as undefined, so it cannot go stale.
runtime_objs=("$out/session.o" "$out/config.o" "$out/screenshot.o" "$out/interop.o" "$out/vram_ledger.o" "$out/log.o" "$out/vkdevice.o"
              "$out/nr_runtime.o" "$out/nr_native_plan.o" "$out/nr_renodx.o" "$out/nr_wilsjo2.o")
undefined=$({ "$cxx" -shared -o /dev/null "$out/reshade_addon.o" "${runtime_objs[@]}" \
    "$out"/mh_*.o "${ldflags[@]}" 2>&1 || true; } |
    grep -o "undefined reference to \`_*vk[A-Za-z0-9_@]*'" | sed "s/.*\`//; s/'//" | sort -u)
plain() { echo "$1" | sed 's/^_//; s/@[0-9]*$//'; }
{
    echo '// Generated by linux/build/build_package.sh.'
    echo '    .text'
    for sym in $undefined; do
        base=$(plain "$sym")
        if [[ "$arch" == i686 ]]; then
            label="_${sym#_}"
            printf '    .globl %s\n%s:\n    jmp *_nr_vk_p_%s\n' "$label" "$label" "$base"
        else
            printf '    .globl %s\n%s:\n    jmp *nr_vk_p_%s(%%rip)\n' "$sym" "$sym" "$base"
        fi
    done
    echo '    .data'
    for sym in $undefined; do
        base=$(plain "$sym")
        if [[ "$arch" == i686 ]]; then
            printf '    .globl _nr_vk_p_%s\n    .align 4\n_nr_vk_p_%s:\n    .long _nr_vk_unresolved\n' "$base" "$base"
        else
            printf '    .globl nr_vk_p_%s\n    .align 8\nnr_vk_p_%s:\n    .quad nr_vk_unresolved\n' "$base" "$base"
        fi
    done
} > "$out/vulkan_lazy.s"
{
    echo '// Generated by linux/build/build_package.sh.'
    echo '#include <windows.h>'
    echo 'long nr_vk_unresolved(void) { return -3; }   // VK_ERROR_INITIALIZATION_FAILED'
    for sym in $undefined; do printf 'extern void* nr_vk_p_%s;\n' "$(plain "$sym")"; done
    printf 'static const char* const nr_vk_names[] = {'
    for sym in $undefined; do printf '"%s", ' "$(plain "$sym")"; done
    echo '};'
    printf 'static void** const nr_vk_slots[] = {'
    for sym in $undefined; do printf '&nr_vk_p_%s, ' "$(plain "$sym")"; done
    echo '};'
    # device-level commands (their first parameter a VkDevice, VkQueue or VkCommandBuffer): the rest take the
    # loader's instance / physical-device objects and stay the loader's in nr_vk_device_direct
    instance_level='^vk(CreateInstance|DestroyInstance|EnumerateInstance.*|EnumeratePhysicalDevice.*|GetPhysicalDevice.*|CreateDevice|EnumerateDevice.*|GetInstanceProcAddr|GetDeviceProcAddr|Create.*SurfaceKHR|DestroySurfaceKHR|.*DebugUtils.*|.*DebugReport.*)$'
    printf 'static const unsigned char nr_vk_device_level[] = {'
    for sym in $undefined; do
        if [[ "$(plain "$sym")" =~ $instance_level ]]; then printf '0, '; else printf '1, '; fi
    done
    echo '};'
    cat <<'C'
#define NR_VK_COUNT (sizeof nr_vk_names / sizeof nr_vk_names[0])
static HMODULE nr_vk_from;
const char* nr_vk_source = "none";      /* the DLL the entry points came from, for the log */
int nr_vk_missing;                       /* entry points that DLL does not export (they return an error, or do nothing) */
const char* nr_vk_first_missing = "";
static int nr_vk_complete(HMODULE m) {
    for (unsigned i = 0; i < NR_VK_COUNT; ++i) if (!GetProcAddress(m, nr_vk_names[i])) return 0;
    return 1;
}
/* Safe to call again: it changes nothing unless the DLL to take the entry points from has changed.
   vkd3d-proton and DXVK call Wine's winevulkan.dll directly, never vulkan-1.dll, so the instance, device and command
   buffers they hand over are winevulkan's own objects. Proton's vulkan-1.dll only forwards to winevulkan.dll, which
   makes the two the same; with a native Khronos vulkan-1.dll (Proton's nativevulkanloader option, which Steam sets
   for some games) the loader takes those objects for its own and the first call crashes. So winevulkan.dll when it
   is in the process and exports every entry point; the winevulkan.dll forwarder of this package's Vulkan routes
   exports only a few and leaves those routes on vulkan-1.dll, the loader the game's device was made through. */
static int nr_vk_loader;
/* ReShade Vulkan handles belong to the loader, even when its Wine driver is also loaded. */
void nr_vk_use_loader(int yes) { nr_vk_loader = yes; }
void nr_vk_load(void) {
    HMODULE m = GetModuleHandleW(L"winevulkan.dll");
    int wine = !nr_vk_loader && m && nr_vk_complete(m);
    if (!wine) {
        m = GetModuleHandleW(L"vulkan-1.dll");
        if (!m) m = LoadLibraryW(L"vulkan-1.dll");
    }
    if (!m || m == nr_vk_from) return;
    nr_vk_from = m;
    nr_vk_source = wine ? "winevulkan.dll" : "vulkan-1.dll";
    nr_vk_missing = 0;
    nr_vk_first_missing = "";
    for (unsigned i = 0; i < NR_VK_COUNT; ++i) {
        void* p = (void*)GetProcAddress(m, nr_vk_names[i]);
        if (!p) {
            p = (void*)nr_vk_unresolved;
            if (!nr_vk_missing++) nr_vk_first_missing = nr_vk_names[i];
        }
        *nr_vk_slots[i] = p;
    }
}
/* ReShade's Vulkan layer drops a device from its table before it tells the add-ons the device is going (its
   vkDestroyDevice): a call of ours through the loader then lands in ReShade's hooks of that device and crashes there.
   While the add-on frees NR's objects of that device, its device-level entry points are the driver's own - the
   winevulkan.dll the loader took as its driver (not this package's forwarder of the same name: the one that exports
   the device functions itself). nr_vk_device_direct(0, 0) puts the loader's back. 1: switched, 0: nothing to switch
   (the entry points are the driver's already), -1: needed but the driver's functions were not found. */
#include <tlhelp32.h>
static void* nr_vk_saved[NR_VK_COUNT];
static int nr_vk_direct_on;
int nr_vk_device_direct(void* device, int on) {
    if (!on) {
        if (nr_vk_direct_on)
            for (unsigned i = 0; i < NR_VK_COUNT; ++i) *nr_vk_slots[i] = nr_vk_saved[i];
        nr_vk_direct_on = 0;
        return 1;
    }
    if (nr_vk_direct_on) return 1;
    if (!device || nr_vk_from == GetModuleHandleW(L"winevulkan.dll")) return 0;
    HMODULE driver = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
    if (snap == INVALID_HANDLE_VALUE) return -1;
    MODULEENTRY32W me; me.dwSize = sizeof me;
    for (BOOL more = Module32FirstW(snap, &me); more && !driver; more = Module32NextW(snap, &me))
        if (!lstrcmpiW(me.szModule, L"winevulkan.dll") && GetProcAddress(me.hModule, "vkDestroyImage"))
            driver = me.hModule;
    CloseHandle(snap);
    if (!driver) return -1;
    typedef void* (__stdcall *gdpa_t)(void*, const char*);   /* PFN_vkGetDeviceProcAddr (VKAPI_CALL = __stdcall) */
    gdpa_t gdpa = (gdpa_t)(void*)GetProcAddress(driver, "vkGetDeviceProcAddr");
    if (!gdpa || !gdpa(device, "vkDestroyImage")) return -1;
    for (unsigned i = 0; i < NR_VK_COUNT; ++i) {
        nr_vk_saved[i] = *nr_vk_slots[i];
        void* p = nr_vk_device_level[i] ? gdpa(device, nr_vk_names[i]) : 0;
        if (p) *nr_vk_slots[i] = p;
    }
    nr_vk_direct_on = 1;
    return 1;
}
C
} > "$out/vulkan_lazy.c"
"$cc" -c "$out/vulkan_lazy.s" -o "$out/vulkan_lazy_s.o"
"$cc" -O2 -c "$out/vulkan_lazy.c" -o "$out/vulkan_lazy_c.o"
echo "vulkan entry points resolved at run time: $(echo "$undefined" | grep -c .)"
lazy=("$out/vulkan_lazy_s.o" "$out/vulkan_lazy_c.o")

# ---- link ---------------------------------------------------------------------
"$cxx" -shared -o "$out/dlssnr_amd.addon$bits" "$out/reshade_addon.o" "${runtime_objs[@]}" \
    "${lazy[@]}" "$out"/mh_*.o "${ldflags[@]}"
[[ -n "${NR_PROGRAMS_ONLY:-}" ]] && exit 0

# ---- package ------------------------------------------------------------------
pkg="$out/package"
rm -rf -- "$pkg"; mkdir -p -- "$pkg"
bash linux/build/assemble_product_data.sh "$pkg" ${model:+--model "$model"} ${model:+${small:+--small-model "$small"}}

# model-tools/: what install.sh --dll runs to rebuild the model from the DLL.
mt="$pkg/model-tools"
mkdir -p -- "$mt"
cp -- linux/package/model-tools/* "$mt/"

# reshade/: copied into the game folder as-is (ReShade as dxgi.dll).
rs="$pkg/reshade"
mkdir -p -- "$rs/reshade-shaders/Shaders" "$rs/reshade-shaders/Textures"
cp -- "$out/dlssnr_amd.addon$bits" "$rs/"
cp -- artifacts/ref/DLSS5-Feeder/shaders/DLSS5_Feed.fx "$rs/reshade-shaders/Shaders/"
cp -- artifacts/ref/DLSS5-Feeder/LICENSE "$rs/reshade-shaders/Shaders/DLSS5_Feed-LICENSE.txt"
cp -- artifacts/ref/reshade-shaders/Shaders/ReShade.fxh artifacts/ref/reshade-shaders/Shaders/ReShadeUI.fxh \
      "$rs/reshade-shaders/Shaders/"
cp -r -- artifacts/ref/vort_Shaders/Shaders/. "$rs/reshade-shaders/Shaders/"
cp -r -- artifacts/ref/vort_Shaders/Textures/. "$rs/reshade-shaders/Textures/"
cp -- artifacts/ref/vort_Shaders/LICENSE "$rs/reshade-shaders/Shaders/vort-LICENSE.txt"
# Wine's d3dcompiler_47 (vkd3d-shader) rejects `[fastopt]` and nested unrolls;
# the shipped copies are patched for it.
python3 linux/build/patch_reshade_effects.py "$rs/reshade-shaders/Shaders"
[[ -f "artifacts/ref/reshade-6.8.0/ReShade$bits.dll" ]] || { echo "missing artifacts/ref/reshade-6.8.0/ReShade$bits.dll" >&2; exit 1; }
cp -- "artifacts/ref/reshade-6.8.0/ReShade$bits.dll" "$rs/dxgi.dll"
cp -- artifacts/ref/reshade-6.8.0/ReShade-LICENSE.md "$rs/ReShade-LICENSE.md"
# OverlayCollapsed: ReShade's built-in Generic Depth add-on folded in the Add-ons tab (its list can be very long); a
# built-in add-on named there starts folded
cat > "$rs/ReShade.ini" <<'INI'
[GENERAL]
EffectSearchPaths=.\reshade-shaders\Shaders\**
TextureSearchPaths=.\reshade-shaders\Textures\**
PresetPath=.\ReShadePreset.ini
PerformanceMode=0
[OVERLAY]
TutorialProgress=4
[ADDON]
AddonPath=.\
OverlayCollapsed=Generic Depth
INI
cat > "$rs/ReShadePreset.ini" <<'INI'
PreprocessorDefinitions=DLSS5_MV_PROVIDER=2,V_MV_MODE=1,V_ENABLE_MOT_BLUR=0,V_ENABLE_TAA=0
Techniques=vort_MotionEffects@vort_Motion.fx,DLSS5_Feed@DLSS5_Feed.fx
TechniqueSorting=vort_MotionEffects@vort_Motion.fx,DLSS5_Feed@DLSS5_Feed.fx
INI

# vulkan/: ReShade as a Vulkan layer, through the patched Khronos loader and a
# winevulkan.dll forwarder (linux/build/build_vulkan_loader.sh). Used for Vulkan
# games and for D3D9 games, whose d3d9 is DXVK under Proton.
vl=artifacts/vulkan-loader/$arch
[[ -f "$vl/vulkan-1.dll" && -f "$vl/winevulkan.dll" ]] || { echo "missing $vl (run linux/build/build_vulkan_loader.sh)" >&2; exit 1; }
vk="$pkg/vulkan"
mkdir -p -- "$vk/vk-override/implicit_layer" "$vk/vk-override/explicit_layer"
cp -- "$vl/vulkan-1.dll" "$vl/winevulkan.dll" "$vl/PATCHES.diff" "$vk/"
cp -- "$vl/LICENSE.txt" "$vk/Vulkan-Loader-LICENSE.txt"
cp -- "artifacts/ref/reshade-6.8.0/ReShade$bits.dll" "$vk/ReShade$bits.dll"
cat > "$vk/vk-override/nr-icd.json" <<'JSON'
{ "file_format_version": "1.0.0", "ICD": { "library_path": "C:\\windows\\system32\\winevulkan.dll", "api_version": "1.3.0" } }
JSON
touch "$vk/vk-override/explicit_layer/.keep"
cat > "$vk/vk-override/implicit_layer/ReShade$bits.json" <<JSON
{
  "file_format_version": "1.0.0",
  "layer": {
    "name": "VK_LAYER_reshade",
    "type": "GLOBAL",
    "library_path": "..\\\\..\\\\ReShade$bits.dll",
    "api_version": "1.3.0",
    "implementation_version": "1",
    "description": "ReShade",
    "functions": {
      "vkGetInstanceProcAddr": "vkGetInstanceProcAddr",
      "vkGetDeviceProcAddr": "vkGetDeviceProcAddr",
      "vkNegotiateLoaderLayerInterfaceVersion": "vkNegotiateLoaderLayerInterfaceVersion"
    },
    "disable_environment": { "DISABLE_RESHADE": "1" }
  }
}
JSON
# Old D3D9 games are not reversed-Z.
sed 's/^PreprocessorDefinitions=.*/&,RESHADE_DEPTH_INPUT_IS_REVERSED=0/' "$rs/ReShadePreset.ini" > "$vk/ReShadePreset-d3d9.ini"

# optiscaler/ (x86_64 only): OptiScaler as released - the official nightly this package was tested with, unpacked from its
# archive, every file unmodified (optiscaler/release) - with its licence (GPL-3.0, from its source tree at that commit);
# and our NR as an OptiScaler plugin (optiscaler/asi): dlssnr-amd.asi, installed into OptiScaler\plugins, which runs NR
# on the game's own upscaler call and draws NR Control, and NR Control's swap-chain probe (into dlssnr-amd\; the
# wilsjo2 processing's shaders are in dlssnr-amd\shaders\wilsjo2 for every route, assemble_product_data.sh). The plugin
# is built with the int4 mixed network (it adds that network's device extensions itself: no Vulkan layer on this route).
if [[ "$arch" == x86_64 ]]; then
    opti_7z=${NR_OPTI_ARCHIVE:-artifacts/ref/downloads/OptiScaler_v10.0.0-pre1_20261028.7z}
    opti_license=${NR_OPTI_LICENSE:-artifacts/ref/optiscaler-nightly-20261028/LICENSE}
    [[ -f "$opti_7z" && -f "$opti_license" ]] || { echo "missing $opti_7z or $opti_license" >&2; exit 1; }
    if [[ -z "${NR_OPTI_ARCHIVE:-}" ]]; then   # the archive the package was tested with
        [[ "$(sha256sum -- "$opti_7z" | cut -d' ' -f1)" == e3a4f533d19a5818a821093f7dcbd9698b96b9b60dcb0e577c569a0b33c94a33 ]] ||
            { echo "$opti_7z is not the tested OptiScaler nightly-20261028" >&2; exit 1; }
    fi
    command -v 7z >/dev/null || { echo "7z is needed to unpack $opti_7z" >&2; exit 1; }
    rm -rf -- "$pkg/optiscaler"
    mkdir -p -- "$pkg/optiscaler/release" "$pkg/optiscaler/asi"
    7z x -y -o"$pkg/optiscaler/release" -- "$opti_7z" > /dev/null
    [[ -f "$pkg/optiscaler/release/OptiScaler.dll" && -f "$pkg/optiscaler/release/OptiScaler.ini" &&
       -d "$pkg/optiscaler/release/OptiScaler" ]] || { echo "$opti_7z is not laid out as OptiScaler v10" >&2; exit 1; }
    cp -- "$opti_license" "$pkg/optiscaler/OptiScaler_LICENSE.txt"
    bash linux/build/build_asi.sh artifacts/optiscaler/asi > /dev/null
    cp -- artifacts/optiscaler/asi/dlssnr-amd.asi artifacts/optiscaler/asi/nr_present_probe.exe "$pkg/optiscaler/asi/"
    # the notices of what dlssnr-amd.asi has compiled in: NR Control's font (Inter, SIL OFL 1.1) and Dear ImGui (MIT)
    cp -- "${NR_MENU_FONT_LICENSE:-toolchain/fonts/inter-4.1/LICENSE.txt}" "$pkg/optiscaler/asi/Inter-LICENSE.txt"
    cp -- "${NR_IMGUI_ROOT:-toolchain/imgui-nr}/LICENSE.txt" "$pkg/optiscaler/asi/DearImGui-LICENSE.txt"
    cmp -s <(cd artifacts/optiscaler/asi/shaders/wilsjo2 && sha256sum -- *.spv) <(cd "$pkg/dlssnr-amd/shaders/wilsjo2" && sha256sum -- *.spv) ||
        { echo "the plugin's wilsjo2 shaders are not dlssnr-amd/shaders/wilsjo2's" >&2; exit 1; }
    cp -- linux/package/optiscaler/patch_ini.py "$pkg/optiscaler/"
    # vulkan-1.dll for the game folder, its exports forwarded to winevulkan.dll (linux/vulkan-loader/vulkan1_forwarder.c)
    "$cc" -shared -o "$pkg/optiscaler/vulkan-1.dll" linux/vulkan-loader/vulkan1_forwarder.c \
        linux/vulkan-loader/vulkan1_forwarder.def
fi

# dlssnr-amd.ini, put in place at install (the text the programs write themselves, linux/package/make_ini.py)
python3 linux/package/make_ini.py linux/src/pe/nr_pe_config.cpp "$pkg/ini" > /dev/null

# int4/: the int4 mixed option. The int4 build of the programs that differ, dlssnr-amd/int4 (network,
# quantisers, recovery tables, settings, layer), the weights generator and the SHA-256 of what it must make from the
# model (linux/data/int4/<NR_INT4_DATA>/dlssnr-int4.sha256, checked by install.sh).
# i686: no OptiScaler route, so only the add-on; the layer comes in both widths (build_int4.sh NR_LAYER32).
if [[ "$arch" == x86_64 || "$arch" == i686 ]]; then
    i4="$pkg/int4"
    NR_INT4=1 NR_PROGRAMS_ONLY=1 bash linux/build/build_package.sh "$out/int4"
    layer32=0; [[ "$arch" == i686 ]] && layer32=1
    NR_LAYER32=$layer32 bash linux/build/build_int4.sh "$out/int4/parts" > /dev/null
    mkdir -p -- "$i4/reshade" "$i4/dlssnr-amd"
    cp -- "$out/int4/dlssnr_amd.addon$bits" "$i4/reshade/"
    cp -r -- "$out/int4/parts/data" "$i4/dlssnr-amd/int4"
    cp -- "$out/int4/parts/dlssnr-int4-weights" "$i4/"
    cp -- "linux/data/int4/$NR_INT4_DATA/dlssnr-int4.sha256" "$i4/dlssnr-int4.bin.sha256"
fi
# experimental/: what install.sh makes CC_SMALL_FP8_EXPERIMENTAL's weights with - the difference of its fitted weights
# from the small model's (linux/data/experimental: each changed entry XOR the small model's, so it holds none of
# NVIDIA's weights), the program that applies it, and the SHA-256 of the result. Its shaders are in
# dlssnr-amd/experimental/.
mkdir -p -- "$pkg/experimental"
cp -- linux/data/experimental/dlssnr-small-experimental.delta "$pkg/experimental/"
cp -- linux/build/experimental_weights.py "$pkg/experimental/"
cp -- linux/data/experimental/dlssnr-small-experimental.sha256 "$pkg/experimental/dlssnr-small-experimental.bin.sha256"
cp -- linux/package/install.sh linux/package/README.txt linux/package/lumenite.py "$pkg/"
chmod +x "$pkg/install.sh" "$mt/extract_model.sh"

# ---- archive ------------------------------------------------------------------
version=$(bash linux/build/version.sh)
archive="DLSSNR-AMD-Vulkan-Linux-$version-$arch$([[ "$gpu" == rdna4 ]] || echo "-$gpu").tar.gz"
tar -C "$pkg" -czf "$out/$archive" .
# The finished package goes beside the installer sources in linux/package/ (not in git).
mv -f -- "$out/$archive" "linux/package/$archive"   # one copy only; package-build/ is scratch
du -sh -- "$pkg"
sha256sum -- "linux/package/$archive"
