#!/usr/bin/env bash
# Lay out dlssnr-amd/, the folder the installed DLLs load everything from:
#
#   dlssnr-amd/dlssnr.bin             the old model's weights (only with --model)
#   dlssnr-amd/dlssnr-small.bin       the new model's weights (only with --small-model)
#   dlssnr-amd/shaders/               the network (linux/build/arch/rdna4.sh)
#   dlssnr-amd/shaders/runtime/       the passes around it
#   dlssnr-amd/shaders/temporal/      the temporal variants and motion estimator
#   dlssnr-amd/shaders/renodx/        the RenoDX-style option's shaders (RenoDX's own, MIT)
#
#   assemble_product_data.sh <parent dir> [--model dlssnr.bin [--small-model dlssnr-small.bin]]
set -euo pipefail
start=$PWD
cd -- "$(dirname -- "$0")/../.."
parent=${1:?usage: assemble_product_data.sh <parent dir> [--model dlssnr.bin [--small-model dlssnr-small.bin]]}
gpu=${NR_GPU:-rdna4}
[[ -f "linux/build/arch/$gpu.sh" ]] || { echo "NR_GPU must be rdna3 or rdna4" >&2; exit 2; }
source "linux/build/arch/$gpu.sh"
dst="$parent/dlssnr-amd"
rm -rf -- "$dst"; mkdir -p -- "$dst/shaders/runtime" "$dst/shaders/temporal"

python3 linux/build/build_network.py "$NR_GPU_ARCH" --out "$NR_PRODUCT_SPV" > /dev/null
cp -- "$NR_PRODUCT_SPV"/g_*.spv "$dst/shaders/"
for f in accumulation.txt swin-bias-storage.txt coherent-act.txt shader-constants.txt; do
    cp -- "$NR_PRODUCT_SPV/$f" "$dst/shaders/"
done
cp -- "$NR_PRODUCT_SPV"/runtime/* "$dst/shaders/runtime/"
cp -- "$NR_PRODUCT_SPV"/temporal/* "$dst/shaders/temporal/"
bash linux/build/build_renodx.sh "$dst/shaders/renodx" > /dev/null   # the RenoDX-style option
cp -- linux/shaders/renodx/LICENSE "$dst/shaders/renodx/RenoDX-LICENSE.txt"

# The weights are NVIDIA's and are never part of this repository: --model takes a dlssnr.bin and
# --small-model a dlssnr-small.bin made from your own nvngx_dlssnr.dll by
# linux/package/model-tools/extract_model.sh.
if [[ "${2:-}" == --model ]]; then
    model=$(cd -- "$start" && realpath -- "${3:?--model needs a dlssnr.bin}")
    [[ -f "$model" ]] || { echo "no model file: $model" >&2; exit 1; }
    cp -- "$model" "$dst/$NR_MODEL_NAME"
    if [[ "${4:-}" == --small-model ]]; then
        small=$(cd -- "$start" && realpath -- "${5:?--small-model needs a dlssnr-small.bin}")
        [[ -f "$small" ]] || { echo "no model file: $small" >&2; exit 1; }
        cp -- "$small" "$dst/dlssnr-small.bin"
    fi
fi
echo "$dst: $(find "$dst/shaders" -name '*.spv' | wc -l) shaders$([[ -f "$dst/$NR_MODEL_NAME" ]] && echo ", model included")"
