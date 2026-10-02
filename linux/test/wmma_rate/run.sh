#!/usr/bin/env bash
# Matrix throughput of the GPU, f16 and int8 coopmat 16x16x16 (RDNA3: the two run at the same rate).
# Two iteration counts per type, so the pipeline-creation time cancels in the difference.
set -euo pipefail
cd -- "$(dirname -- "$0")/../../.."
out=build/test/wmma_rate; mkdir -p "$out"
mk() { n=$1; shift; { echo '#version 450'; for d in "$@"; do echo "#define ${d/=/ }"; done; tail -n +2 linux/test/wmma_rate/wmma_rate.comp; } > "$out/t_$n.comp"
       toolchain/glslang/bin/glslang -V --target-env vulkan1.3 "$out/t_$n.comp" -o "$out/$n.spv"; }
mk f1 ITERS=5000; mk f2 ITERS=20000; mk i1 I8 ITERS=5000; mk i2 I8 ITERS=20000
g++ -O1 -Itoolchain/Vulkan-Headers/include linux/test/wmma_rate/wmma_rate.cpp -o "$out/wmma_rate" -lvulkan
cd "$out" && ./wmma_rate
