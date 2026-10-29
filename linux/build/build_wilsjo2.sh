#!/usr/bin/env bash
# The wilsjo2 style's shaders (linux/shaders/wilsjo2, ours, written from our specification of the style) ->
# SPIR-V with glslang, like every other pass of ours. Installed as dlssnr-amd\shaders\wilsjo2\wilsjo2_<mode>.spv.
#   build_wilsjo2.sh <out dir>        NR_GLSLANG overrides the compiler (default toolchain/glslang)
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
out=$(realpath -m -- "${1:?usage: build_wilsjo2.sh <out dir>}")
glslang=${NR_GLSLANG:-toolchain/glslang/bin/glslang}
src=linux/shaders/wilsjo2
mkdir -p -- "$out"
rm -f -- "$out"/wilsjo2_*.spv   # only this tree's programs (a reused folder may hold another build's)
log=$(mktemp "${TMPDIR:-/tmp}/wilsjo2.XXXXXX")
trap 'rm -f -- "$log"' EXIT
for k in encode resolve meter autoexposure finished clamp downsample os spatial inspect light; do
    "$glslang" -V --target-env vulkan1.3 -I"$src" "$src/wilsjo2_$k.comp" -o "$out/wilsjo2_$k.spv" > "$log" ||
        { cat "$log" >&2; exit 1; }
done
# the peripheral compression's guides (mode 101) as a shader of their own, as the original has them
"$glslang" -V --target-env vulkan1.3 -I"$src" -DWILSJO2_GUIDES "$src/wilsjo2_spatial.comp" -o "$out/wilsjo2_spatial_guides.spv" > "$log" ||
    { cat "$log" >&2; exit 1; }
# [Preprocess] on a wilsjo2 layer: the shared math (linux/shaders/renodx-prep/prep_math.glsl, RenoDX's too); the meter
# as a program of its own (one 16 x 16 group)
"$glslang" -V --target-env vulkan1.3 -I"$src" -Ilinux/shaders/renodx-prep "$src/wilsjo2_prep.comp" -o "$out/wilsjo2_prep.spv" > "$log" ||
    { cat "$log" >&2; exit 1; }
"$glslang" -V --target-env vulkan1.3 -I"$src" -Ilinux/shaders/renodx-prep -DMETER "$src/wilsjo2_prep.comp" -o "$out/wilsjo2_prep_meter.spv" > "$log" ||
    { cat "$log" >&2; exit 1; }
# Enlargement Classic's scaler (FSR 1 EASU from linux/shaders/passes/include/fsr1, AMD, MIT)
"$glslang" -V --target-env vulkan1.3 -I"$src" -Ilinux/shaders/passes/include "$src/wilsjo2_classic.comp" -o "$out/wilsjo2_classic.spv" > "$log" ||
    { cat "$log" >&2; exit 1; }
# Passes (fast): the RenoDX-style stage's programs (linux/shaders/renodx-fast, the same sources as renodx_st_*.spv), and
# the network's input in 16-bit float for them
for k in st_fb_in st_prep st_box st_apply; do
    "$glslang" -V --target-env vulkan1.3 -Ilinux/shaders/renodx-fast linux/shaders/renodx-fast/$k.comp -o "$out/wilsjo2_$k.spv" > "$log" ||
        { cat "$log" >&2; exit 1; }
done
"$glslang" -V --target-env vulkan1.3 -I"$src" "$src/wilsjo2_half_copy.comp" -o "$out/wilsjo2_half_copy.spv" > "$log" ||
    { cat "$log" >&2; exit 1; }
# the .asi's Vulkan copies a transfer cannot make: one file, two programs (the source sampled / read as storage)
"$glslang" -V --target-env vulkan1.3 -I"$src" "$src/wilsjo2_copy.comp" -o "$out/wilsjo2_copy.spv" > "$log" ||
    { cat "$log" >&2; exit 1; }
"$glslang" -V --target-env vulkan1.3 -I"$src" -DCOPY_LOAD "$src/wilsjo2_copy.comp" -o "$out/wilsjo2_copy_load.spv" > "$log" ||
    { cat "$log" >&2; exit 1; }
# HUD FIX: one file, four programs
for k in clear count estimate compose; do
    "$glslang" -V --target-env vulkan1.3 -I"$src" -DHUD_${k^^} "$src/wilsjo2_hudfix.comp" -o "$out/wilsjo2_hudfix_$k.spv" > "$log" ||
        { cat "$log" >&2; exit 1; }
done
echo "$out: $(ls "$out"/wilsjo2_*.spv | wc -l) wilsjo2 shaders"
