#!/usr/bin/env bash
# Build dlssnr_extract_model: the Windows .exe (static, mingw-w64) and a native
# Linux binary for testing.
#
#   bash windows/package/model-tools/build_extract_model.sh <output dir> [descriptor.json]
#
# Run from anywhere in the repository. dlssnr_extract_tables.hpp and dlssnr_extract_small_tables.hpp
# (next to this script) are regenerated first from linux/package/model-tools/: descriptor.json (or the
# one given) with model-files.{txt,sha256}, and descriptor-small.json with model-files-small.{txt,sha256}.
set -euo pipefail
here=$(cd -- "$(dirname -- "$0")" && pwd)
root=$(cd -- "$here/../../.." && pwd)
out=${1:?usage: build_extract_model.sh <output dir> [descriptor.json]}
desc=${2:-$root/linux/package/model-tools/descriptor.json}
mkdir -p -- "$out"
out=$(cd -- "$out" && pwd)

python3 "$here/gen_extract_tables.py" --descriptor "$desc" \
    --list "$root/linux/package/model-tools/model-files.txt" --sums "$root/linux/package/model-tools/model-files.sha256" \
    --out "$here/dlssnr_extract_tables.hpp"
python3 "$here/gen_extract_tables.py" --descriptor "$root/linux/package/model-tools/descriptor-small.json" \
    --list "$root/linux/package/model-tools/model-files-small.txt" --sums "$root/linux/package/model-tools/model-files-small.sha256" \
    --namespace small_tables --shared-types --out "$here/dlssnr_extract_small_tables.hpp"

flags=(-std=c++17 -O2 -Wall -Wextra)
g++ "${flags[@]}" -o "$out/dlssnr_extract_model" "$here/dlssnr_extract_model.cpp"
x86_64-w64-mingw32-g++ "${flags[@]}" -municode -static -static-libgcc -static-libstdc++ \
    -o "$out/dlssnr_extract_model.exe" "$here/dlssnr_extract_model.cpp"
x86_64-w64-mingw32-strip "$out/dlssnr_extract_model.exe"
ls -l -- "$out/dlssnr_extract_model" "$out/dlssnr_extract_model.exe"
