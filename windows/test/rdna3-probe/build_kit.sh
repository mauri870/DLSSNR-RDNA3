#!/usr/bin/env bash
# Build the RDNA3 test kit for the AMD Windows driver: a folder (and zip) a person without tools can unzip and
# run with a double click. It runs the network once on a 1080p frame in two modes, compares the picture with
# the one RADV makes, times 720p and 1080p, and, when both runs fail, tries every shader in its own process
# and reports the driver's limits (windows/test/rdna3-probe/README.md).
#
#   bash windows/test/rdna3-probe/build_kit.sh [output dir]
#   NR_MODEL=dlssnr.bin bash windows/test/rdna3-probe/build_kit.sh [output dir]
#
# With neither variable set, the reference picture is the RDNA3 output committed in docs/ngx-verification
# (1920x1080_dlssnr-amd-rdna3.png), which is what RADV makes from the same frame. Nothing needs a GPU or a model.
# NR_MODEL        make the reference picture again on this machine with RADV (needs a GPU and the model). The model
#                 is never put in the kit; the tester makes its own from the user's nvngx_dlssnr.dll.
# NR_REFERENCE    use an existing RADV picture of the 1080p test frame instead (RGBA8, 8294400 bytes).
# NR_UNROLL=0     keep the shaders as they are (default 1: the pipelines LLPC crashes on, unrolled).
# NR_ZIP=0        do not zip the kit.
#
# Cross-compiled with mingw-w64, from Linux or WSL. Needs toolchain/glslang (glslang 16.5.0) and
# toolchain/Vulkan-Headers (both from fetch_deps.sh), mingw-w64, g++, patch, zip, and python3 with numpy and PIL.
# The import library for vulkan-1.dll is generated here from the Vulkan headers; no loader build is needed.
# The diagnostic patch (diag.patch) is applied to a copy of the sources; the tree is not touched.
set -euo pipefail
cd -- "$(dirname -- "$0")/../../.."
here=windows/test/rdna3-probe
out=$(realpath -m -- "${1:-artifacts/windows/rdna3-probe-kit}")
case "$out" in "$(pwd)"/*) ;; *) echo 'output directory must be in the project' >&2; exit 2;; esac
cxx=x86_64-w64-mingw32-g++
command -v "$cxx" >/dev/null || { echo "no mingw cross compiler ($cxx)" >&2; exit 1; }
glslang=toolchain/glslang/bin/glslang
for f in "$glslang" toolchain/Vulkan-Headers/include/vulkan/vulkan_core.h; do
    [[ -e "$f" ]] || { echo "missing: $f (bash fetch_deps.sh)" >&2; exit 1; }
done
for tool in x86_64-w64-mingw32-dlltool x86_64-w64-mingw32-strip g++ patch zip python3; do
    command -v "$tool" >/dev/null || { echo "missing tool: $tool" >&2; exit 1; }
done
python3 -c 'import numpy, PIL' 2>/dev/null || { echo 'python3 needs numpy and PIL (apt install python3-numpy python3-pil)' >&2; exit 1; }
version=$(bash linux/build/version.sh)
work="$out/work"
kit="$out/rdna3-windows-test-$version"
mkdir -p -- "$out"
rm -rf -- "${work:?}" "${kit:?}"
mkdir -p -- "$work" "$kit/dlssnr-amd" "$kit/model-tools" "$kit/source" "$kit/probe-micro" "$kit/probe-micro2"

# Import library for vulkan-1.dll: every vk* function the header declares. A symbol the loader does not export
# costs nothing unless something links against it.
{ echo 'LIBRARY vulkan-1.dll'; echo 'EXPORTS'
  grep -ohE 'VKAPI_CALL +vk[A-Za-z0-9_]+' toolchain/Vulkan-Headers/include/vulkan/vulkan_core.h | awk '{print $2}' | sort -u
} > "$work/vulkan-1.def"
x86_64-w64-mingw32-dlltool -d "$work/vulkan-1.def" -l "$work/libvulkan-1.dll.a"
implib="$work/libvulkan-1.dll.a"
defines=$(bash -c 'source linux/build/arch/rdna3.sh; printf "%s " "${NR_PRODUCT_DEFINES[@]}"')

# ---- the network ----------------------------------------------------------------------------------------
python3 linux/build/build_network.py rdna3 --out "$work/net" > /dev/null
cp -r -- "$work/net" "$work/net_plain"
if [[ "${NR_UNROLL:-1}" != 0 ]]; then python3 "$here/unroll_network.py" "$work/net"; fi
cp -r -- "$work/net" "$kit/dlssnr-amd/shaders"

# ---- test frames and the reference picture ---------------------------------------------------------------
python3 - "$kit" <<'PY'
import sys
import numpy as np
from PIL import Image
kit = sys.argv[1]
im = Image.open('docs/ngx-verification/single-frame-inputs/1920x1080.png').convert('RGBA')
np.array(im).tofile(f'{kit}/in_1080p.rgba8')
np.array(im.convert('RGB').resize((1280, 720), Image.LANCZOS).convert('RGBA')).tofile(f'{kit}/in_720p.rgba8')
PY
if [[ -n "${NR_REFERENCE:-}" ]]; then
    cp -- "$NR_REFERENCE" "$kit/radv_1080p.rgba8"
elif [[ -z "${NR_MODEL:-}" ]]; then
    python3 - "$kit" <<'PY'
import sys
import numpy as np
from PIL import Image
np.array(Image.open('docs/ngx-verification/single-frame-outputs/1920x1080_dlssnr-amd-rdna3.png').convert('RGBA')).tofile(sys.argv[1] + '/radv_1080p.rgba8')
PY
else
    g++ -std=c++20 -O1 -w $defines -Ilinux/src/core -Ilinux/test linux/test/run_frame.cpp linux/src/core/nr_runtime.cpp \
        linux/src/core/nr_native_plan.cpp -o "$work/run_frame_native" -lvulkan -lpthread
    mkdir -p -- "$work/ref/dlssnr-amd"
    cp -r -- "$work/net_plain" "$work/ref/dlssnr-amd/shaders"
    cp -- "$NR_MODEL" "$work/ref/dlssnr-amd/dlssnr.bin"
    "$work/run_frame_native" "$work/ref" "$kit/in_1080p.rgba8" 1920 1080 "$kit/radv_1080p.rgba8" > /dev/null
fi

# ---- the programs -----------------------------------------------------------------------------------------
# run_frame with the diagnostic patch: a step trace and a crash report, symbols kept in work/ for addr2line.
mkdir -p -- "$work/src/linux"
cp -r -- linux/src linux/test "$work/src/linux/"
patch -s -p1 -d "$work/src" < "$here/diag.patch"
common=(-std=c++20 -O2 -w -DNDEBUG $defines -Itoolchain/Vulkan-Headers/include -I"$work/src/linux/src/core" -I"$work/src/linux/test")
tail=("$implib" -static -static-libgcc -static-libstdc++)
"$cxx" "${common[@]}" -g -DNRVK_TRACE "$work/src/linux/test/run_frame.cpp" "$work/src/linux/src/core/nr_runtime.cpp" \
    "$work/src/linux/src/core/nr_native_plan.cpp" -o "$work/run_frame_symbols.exe" "${tail[@]}"
cp -- "$work/run_frame_symbols.exe" "$kit/run_frame.exe"
x86_64-w64-mingw32-strip -s "$kit/run_frame.exe"
"$cxx" "${common[@]}" -s -municode "$here/probe.cpp" -o "$kit/probe.exe" "${tail[@]}"
"$cxx" -std=c++17 -O2 -s -municode -static -static-libgcc -static-libstdc++ "$here/nr_tester.cpp" -o "$kit/nr_tester.exe" \
    -lbcrypt -ladvapi32
bash windows/package/model-tools/build_extract_model.sh "$work/extract" > /dev/null
cp -- "$work/extract/dlssnr_extract_model.exe" "$kit/model-tools/"

# ---- the probe's shaders --------------------------------------------------------------------------------
for f in "$here"/micro/*.comp; do
    "$glslang" -V --target-env vulkan1.3 "$f" -o "$kit/probe-micro/micro_$(basename "$f" .comp).spv" > /dev/null
done
for f in "$here"/micro2/*.comp; do
    "$glslang" -V --target-env vulkan1.3 "$f" -o "$kit/probe-micro2/micro2_$(basename "$f" .comp).spv" > /dev/null
done

# ---- assemble ---------------------------------------------------------------------------------------------
cp -- "$here/probe-shapes.txt" "$kit/"
cp -- "$here/Run test.bat" "$kit/"
sed "s/@VERSION@/$version/" "$here/kit-README.txt" > "$kit/README.txt"
cp -- "$here/nr_tester.cpp" "$here/probe.cpp" "$here/diag.patch" "$kit/source/"
if [[ "${NR_ZIP:-1}" != 0 ]]; then
    (cd -- "$out" && rm -f -- "rdna3-windows-test-$version.zip" && zip -qr "rdna3-windows-test-$version.zip" "rdna3-windows-test-$version")
    sha256sum -- "$out/rdna3-windows-test-$version.zip"
fi
echo "kit: $kit"
echo "symbols for crash offsets in run_frame.exe: $work/run_frame_symbols.exe (addr2line -f -C -e it ADDRESS, ADDRESS = 0x140000000 + offset)"
