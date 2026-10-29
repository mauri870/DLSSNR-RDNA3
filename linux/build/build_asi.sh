#!/usr/bin/env bash
# dlssnr-amd.asi: NR as a plugin of upstream OptiScaler (linux/src/pe/nr_asi_main.cpp), cross-compiled to PE, x86_64
# only (OptiScaler's builds are). It is the OptiScaler route's model with an entry of its own: the objects are the
# ones linux/build/build_optiscaler_nr.sh builds for _nvngx.dll (same flags, same generated Vulkan thunks), less the
# NGX exports and the OptiScaler-NR fixes (nr_ngx_core.cpp, nr_pe_optifix.cpp).
#
#   build_asi.sh [build dir]     (default artifacts/optiscaler/asi; the objects are built there first)
# NR_INT4=1 (the default, as the package's OptiScaler route core: the int4 mixed network installed beside the others)
# or 0.
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."

out=$(realpath -m -- "${1:-artifacts/optiscaler/asi}")
case "$out" in "$(pwd)"/*) ;; *) echo 'build directory must be in the project' >&2; exit 2;; esac
mkdir -p -- "$out"

int4=${NR_INT4:-1}

cxx=${NR_MINGW:-x86_64-w64-mingw32-g++}
objdump=${NR_OBJDUMP:-x86_64-w64-mingw32-objdump}
minhook=artifacts/ref/DLSS5-Feeder/external/minhook
stamp=${NR_STAMP:-$(git rev-parse --short HEAD 2>/dev/null || cat .git-commit 2>/dev/null || echo unknown)}
python3 linux/build/generate_device_chain.py toolchain/Vulkan-Headers "$out/nr_device_chain_sizes.hpp" > /dev/null
common=(-std=c++17 -O2 -DNDEBUG -DNR_BUILD_STAMP="\"$stamp\"" -Itoolchain/Vulkan-Headers/include -Ilinux/src -Ilinux/src/core
        -Ilinux/src/layer -Ilinux/src/pe -I"$out")
[[ "$int4" == 1 ]] && common+=(-DNR_INT4=1)
"$cxx" "${common[@]}" -I"$minhook/include" -c linux/src/pe/nr_asi_main.cpp -o "$out/asi_main.o"
# The wilsjo2 style (nr_asi_wilsjo2.hpp): its flow and its programs; the shaders go to dlssnr-amd\shaders\wilsjo2.
"$cxx" "${common[@]}" -c linux/src/pe/nr_asi_wilsjo2.cpp -o "$out/asi_wilsjo2.o"
"$cxx" "${common[@]}" -c linux/src/pe/nr_asi_wilsjo2_vk.cpp -o "$out/asi_wilsjo2_vk.o"
"$cxx" "${common[@]}" -c linux/src/core/nr_wilsjo2.cpp -o "$out/nr_wilsjo2.o"
# HUD FIX's capture on D3D12 (nr_asi_hudcap.hpp): hooks of the game's command lists
"$cxx" "${common[@]}" -I"$minhook/include" -c linux/src/pe/nr_asi_hudcap.cpp -o "$out/asi_hudcap.o"
bash linux/build/build_wilsjo2.sh "$out/shaders/wilsjo2" > /dev/null
# NR Control (End), drawn by the DLL over the game's swap chain: ImGui with its D3D11 and D3D12 backends, as the
# Windows build (windows/build/build_optiscaler_nr.sh) - the same ImGui.
imgui=${NR_IMGUI_ROOT:-toolchain/imgui-nr}
[[ -f "$imgui/imgui_impl_dx12.cpp" ]] || { echo "missing $imgui (ImGui with its D3D backends)" >&2; exit 1; }
# NR Control's font: Inter (SIL Open Font License 1.1, toolchain/fonts/inter-4.1/LICENSE.txt), embedded
font=${NR_MENU_FONT:-toolchain/fonts/inter-4.1/Inter-Regular.ttf}
[[ -f "$font" ]] || { echo "missing $font (NR Control's font)" >&2; exit 1; }
python3 linux/build/embed_font.py "$font" "$out/nr_menu_font.hpp" kMenuFont
glslang=${NR_GLSLANG:-toolchain/glslang/bin/glslang}
"$glslang" -V --target-env vulkan1.0 --vn kMenuComposeVert linux/shaders/control/menu_compose.vert -o "$out/menu_compose_vert.h" > /dev/null
"$glslang" -V --target-env vulkan1.0 --vn kMenuComposeFrag linux/shaders/control/menu_compose.frag -o "$out/menu_compose_frag.h" > /dev/null
"$cxx" "${common[@]}" -I"$minhook/include" -I"$imgui" -Ilinux/src/ui -c linux/src/pe/nr_pe_menu.cpp -o "$out/menu.o"
for unit in imgui imgui_draw imgui_tables imgui_widgets imgui_impl_dx11 imgui_impl_vulkan; do
    "$cxx" -std=c++17 -O2 -DNDEBUG -Itoolchain/Vulkan-Headers/include -I"$imgui" -c "$imgui/$unit.cpp" -o "$out/im_$unit.o"
done
"$cxx" -std=c++17 -O2 -DNDEBUG -I"$imgui" -include linux/src/pe/mingw_d3d12_compat.h -c "$imgui/imgui_impl_dx12.cpp" \
    -o "$out/im_imgui_impl_dx12.o"
# nr_present_probe.exe (dlssnr-amd\): finds DXGI's swap-chain methods for NR Control in a process of its own.
"$cxx" -std=c++17 -O2 -DNDEBUG -municode linux/src/pe/nr_present_probe.cpp -o "$out/nr_present_probe.exe" \
    -ldxgi -static -static-libgcc -static-libstdc++

# Include the menu/backend in the Vulkan symbol probe: their graphics calls are not in the compute runtime.
NR_ARCH=x86_64 NR_INT4=$int4 NR_ASI_VK_OBJECTS=1 bash linux/build/build_optiscaler_nr.sh "$out" > "$out/build_optiscaler_nr.log"

shared_objs=("$out/model.o" "$out/session.o" "$out/config.o" "$out/interop.o" "$out/vram_ledger.o" "$out/log.o" "$out/vkdevice.o"
             "$out/nr_runtime.o" "$out/nr_native_plan.o" "$out/nr_renodx.o" "$out/renodx_route.o" "$out/control.o"
             "$out/screenshot.o" "$out"/mh_*.o)
"$cxx" -shared -o "$out/dlssnr-amd.asi" "$out/asi_main.o" "$out/asi_wilsjo2.o" "$out/asi_wilsjo2_vk.o" "$out/nr_wilsjo2.o" "$out/asi_hudcap.o" "$out/menu.o" "$out"/im_*.o "${shared_objs[@]}" "$out/vulkan_lazy_s.o" \
    "$out/vulkan_lazy_c.o" -ld3d12 -ldxgi -lole32 -lversion -static -static-libgcc -static-libstdc++

echo "built $out/dlssnr-amd.asi ($(stat -c %s "$out/dlssnr-amd.asi") bytes), $out/nr_present_probe.exe and $out/shaders/wilsjo2"
echo "exports: $("$objdump" -p "$out/dlssnr-amd.asi" | grep -c 'InitializeASI')  imports: $("$objdump" -p "$out/dlssnr-amd.asi" | grep 'DLL Name' | awk '{print $3}' | tr '\n' ' ')"
