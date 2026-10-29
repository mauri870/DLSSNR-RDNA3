#!/usr/bin/env bash
# Download the third-party pieces the build uses, each pinned to the exact version the
# packages were built and tested with, into toolchain/ and artifacts/ref/ (neither is in git).
#
#   bash fetch_deps.sh             what the Linux package needs
#   bash fetch_deps.sh --windows   plus what the Windows package needs (GE-Proton 11-7 for DXVK,
#                                  and its licence)
#
# Already present entries are kept. Needs git, curl, tar and python3.
#
# Nothing from NVIDIA is downloaded: the model comes from your own nvngx_dlssnr.dll
# (linux/package/model-tools/extract_model.sh).
set -euo pipefail
cd -- "$(dirname -- "$0")"
windows=0
for a in "$@"; do
    case "$a" in --windows) windows=1;; *) echo "usage: fetch_deps.sh [--windows]" >&2; exit 2;; esac
done
mkdir -p toolchain artifacts/ref/downloads

# A git tree at one commit, without the history.
repo() {  # <dir> <url> <commit>
    local dir=$1 url=$2 commit=$3
    [[ -d "$dir" ]] && { echo "have $dir"; return; }
    echo "fetch $dir"
    git init -q "$dir"
    git -C "$dir" fetch -q --depth 1 "$url" "$commit"
    git -C "$dir" checkout -q FETCH_HEAD
}

# A file, checked against its SHA-256.
get() {  # <file> <url> <sha256>
    local file=$1 url=$2 sum=$3
    if [[ ! -f "$file" ]]; then
        echo "fetch $file"
        curl -fsSL --retry 3 -o "$file.part" "$url"
        mv -- "$file.part" "$file"
    fi
    echo "$sum  $file" | sha256sum -c --quiet - || { echo "checksum mismatch: $file" >&2; exit 1; }
}

# ---- build tools ------------------------------------------------------------------------------
repo toolchain/Vulkan-Headers https://github.com/KhronosGroup/Vulkan-Headers.git \
     e3b1eec08173d6b825cd3ac88c885a63b621504a                                      # v1.4.357
# The shaders are only byte-identical to the tested ones with this exact glslang.
get artifacts/ref/downloads/glslang-16.5.0-linux-x86_64-release.tar.gz \
    https://github.com/KhronosGroup/glslang/releases/download/16.5.0/glslang-16.5.0-linux-x86_64-release.tar.gz \
    b9b1f96acb898a62251b171f7695efcecfc206a530299054071919b06820f657
if [[ ! -x toolchain/glslang/bin/glslang ]]; then
    mkdir -p toolchain/glslang
    tar -C toolchain/glslang -xzf artifacts/ref/downloads/glslang-16.5.0-linux-x86_64-release.tar.gz
fi

# ---- sources and headers ----------------------------------------------------------------------
# Slang 2026.19 compiles the RenoDX-style option's shaders (linux/shaders/renodx, RenoDX's own HLSL); with this
# exact release they are the same SPIR-V bytes as the tested ones.
get artifacts/ref/downloads/slang-2026.19-linux-x86_64-glibc-2.28.tar.gz \
    https://github.com/shader-slang/slang/releases/download/v2026.19/slang-2026.19-linux-x86_64-glibc-2.28.tar.gz \
    bd7afcb0956dee9304f44ed115e9aaa7140704271b5c3294bcb237e3ae67adc7
if [[ ! -x toolchain/slang-2026.19/bin/slangc ]]; then
    mkdir -p toolchain/slang-2026.19
    tar -C toolchain/slang-2026.19 -xzf artifacts/ref/downloads/slang-2026.19-linux-x86_64-glibc-2.28.tar.gz
fi

# ---- sources ----------------------------------------------------------------------------------
# Dear ImGui (MIT) as OptiScaler-NR's source tree carries it, for NR Control (the OptiScaler route's NR menu: a
# Linux Vulkan layer, and on Windows drawn by the core DLL with the D3D11 / D3D12 backends): the files it compiles,
# checked, into toolchain/imgui-nr, with this project's imconfig.h.
repo artifacts/ref/OptiScaler-NR-src https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass.git \
     f45ccf3a761df91450959d325ac0166cba5364c9                                      # v0.8.91 source
if [[ ! -f toolchain/imgui-nr/imgui_impl_dx12.cpp ]]; then
    mkdir -p toolchain/imgui-nr
    for f in LICENSE.txt imgui.cpp imgui.h imgui_draw.cpp imgui_tables.cpp imgui_widgets.cpp imgui_internal.h \
             imgui_impl_vulkan.cpp imgui_impl_vulkan.h imgui_impl_dx11.cpp imgui_impl_dx11.h imgui_impl_dx12.cpp \
             imgui_impl_dx12.h imstb_rectpack.h imstb_textedit.h imstb_truetype.h; do
        cp -- "artifacts/ref/OptiScaler-NR-src/OptiScaler/include/imgui/$f" toolchain/imgui-nr/
    done
    printf '#pragma once\n// Project configuration. Standard stb fonts; no reference-project FreeType hooks.\n' \
        > toolchain/imgui-nr/imconfig.h
fi
(cd toolchain/imgui-nr && sha256sum -c --quiet -) <<EOF || { echo "checksum mismatch: toolchain/imgui-nr" >&2; exit 1; }
c80c5789748d955c4a650562baa0d750494e2c7128c2ca66aaebe2e3c4e198bf  LICENSE.txt
da877efff5dbfc130572e11881751e215e493f647515041195776b974b31dcae  imgui.cpp
9300189bd23f5a920019fb006bf45b297bb389bad24dfdf12768fd04bfa6fc50  imgui.h
4605107eb83713928a1e522b557d7ee22f9e77e5cfb4b09fe8e37c204456289d  imgui_draw.cpp
5c8c575481cc5352510ed55d4dbddfd458268857c1d3be55d8d0ae8c86ad13fe  imgui_tables.cpp
e4ac34e7bc9ef6509e41a1124dcdb460b995b7b56b64681ce924f384518b33e2  imgui_widgets.cpp
7ea0356920f6a8f9b7d85eeead141ad9eadac4f87fdde125262a1b29c8c59aa2  imgui_internal.h
c33a3f98fd7963e5508303f480a6d761198c8d22ad0609f4d23e0e3ff2af9e06  imgui_impl_vulkan.cpp
4723ad0de0d2dc23e6e44871ff5370d3e239e59dc7d2f20d69cae8c33e5266f5  imgui_impl_vulkan.h
63d2e5d0a358a40ded05e2e7d0a8d9c3b7e5eb7a34d352d90dbf3a9e549dc34a  imgui_impl_dx11.cpp
7c40511ddcebd12be1acad7752cb9873a7c621bb5a6b9c58be37500a86f4b7d2  imgui_impl_dx11.h
d4a6f8a59c3c3cc30b7bd05ca58b09f4cd612540980ba7b9e7309920e7aabb41  imgui_impl_dx12.cpp
c74ce2daf4901229511131d06e4d1d2d9f0b22a487e0987ec11a98510e62ef68  imgui_impl_dx12.h
2efa3d5f7d003c19743b15155dad9f46d9f9fc783a18893d703b431d3c990972  imstb_rectpack.h
10777411bf758908aadaf7d73d4710d6ba0464f332f0015db9d1444c19456e6a  imstb_textedit.h
87f08919ae8e7223451d0e9d06b62829075d3e920c9e6ad436db9d7e7427ffce  imstb_truetype.h
EOF
# DLSS5-Feeder (MIT): MinHook, the ReShade add-on headers and Dear ImGui under external/, and the
# DLSS5_Feed.fx effect the ReShade route ships.
repo artifacts/ref/DLSS5-Feeder https://github.com/jlrouzies-fr/DLSS5-Feeder.git \
     76a08db83652617cfe4192eeecf9e46bfc008382
repo artifacts/ref/reshade-shaders https://github.com/crosire/reshade-shaders.git \
     6db142b4b1a05c764222e5b0bd9a644b7ccfe1dc
# vort_Shaders (MIT): the motion vectors of the ReShade route.
repo artifacts/ref/vort_Shaders https://github.com/vortigern11/vort_Shaders.git \
     b410b9f0c0fbb83c8cb42164aaf1655fab386f4a
# The Vulkan loader source is cloned by linux/build/build_vulkan_loader.sh itself.

# ---- binaries the packages carry unmodified ---------------------------------------------------
# ReShade 6.8.0 with add-on support: the DLLs come out of the official installer, which is a zip.
get artifacts/ref/downloads/ReShade_Setup_6.8.0_Addon.exe \
    https://reshade.me/downloads/ReShade_Setup_6.8.0_Addon.exe \
    afe4c8f13048306307983b8b3d41d5bf00a86820440b0e57dea10950e1176445
rs=artifacts/ref/reshade-6.8.0
if [[ ! -f "$rs/ReShade64.dll" ]]; then
    mkdir -p "$rs"
    python3 - artifacts/ref/downloads/ReShade_Setup_6.8.0_Addon.exe "$rs" <<'PY'
import sys, zipfile
with zipfile.ZipFile(sys.argv[1]) as z:
    for name in ("ReShade32.dll", "ReShade64.dll"):
        open(f"{sys.argv[2]}/{name}", "wb").write(z.read(name))
PY
fi
sha256sum -c --quiet - <<EOF
da430e0a9c6eecefa0d1b27d05e16c426fb5d04e808b194d914eaac4b31bc0f8  $rs/ReShade32.dll
0cee63f9c9f13f3ac909c5b4903f4dbb4b719a7ab3b4f13b0deaf83c814b94f7  $rs/ReShade64.dll
EOF
get "$rs/ReShade-LICENSE.md" https://raw.githubusercontent.com/crosire/reshade/v6.8.0/LICENSE.md \
    237ded5b8344f820113efab1e65e91e1f159d9202c5b4856606a0590d3ffdab0
# OptiScaler-NR (GPL-3.0), the Windows OptiScaler route's host, shipped as released: 0.8.91. 0.8.4 still works with
# the same core (NR_OPTI_ZIP=, windows/build/build_package.sh).
get artifacts/ref/downloads/OptiScaler-NR-v0.8.4.zip \
    https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass/releases/download/v0.8.4/OptiScaler-NR-v0.8.4.zip \
    8789912859882e66b3f3a1aa768db947da779dfd65225df69ea919052e73a2e4
get artifacts/ref/downloads/OptiScaler-NR-v0.8.91.zip \
    https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass/releases/download/v0.8.91/OptiScaler-NR-v0.8.91.zip \
    19a2852bb3f88e09075e3ccc66e0318c52e83a5901d9020a10384ae49d59ff77

# OptiScaler (GPL-3.0), the Linux OptiScaler route's host, shipped as released: the official nightly the Linux packages
# were tested with (OptiScaler v10.0.0-pre1, nightly-20261028), and its licence from its source tree at that nightly.
get artifacts/ref/downloads/OptiScaler_v10.0.0-pre1_20261028.7z \
    https://github.com/optiscaler/OptiScaler-nightly/releases/download/nightly-20261028/OptiScaler_v10.0.0-pre1_20261028.7z \
    e3a4f533d19a5818a821093f7dcbd9698b96b9b60dcb0e577c569a0b33c94a33
mkdir -p artifacts/ref/optiscaler-nightly-20261028
get artifacts/ref/optiscaler-nightly-20261028/LICENSE \
    https://raw.githubusercontent.com/optiscaler/OptiScaler/b19cfd29134ab9bc75d654b90f55fb05faca91b2/LICENSE \
    3972dc9744f6499f0f9b2dbf76696f2ae7ad8af9b23dde66d6af86c9dfb36986
# Inter 4.1 (SIL Open Font License 1.1), NR Control's font on the Linux OptiScaler route (embedded in dlssnr-amd.asi).
get artifacts/ref/downloads/Inter-4.1.zip https://github.com/rsms/inter/releases/download/v4.1/Inter-4.1.zip \
    9883fdd4a49d4fb66bd8177ba6625ef9a64aa45899767dde3d36aa425756b11e
if [[ ! -f toolchain/fonts/inter-4.1/Inter-Regular.ttf ]]; then
    mkdir -p toolchain/fonts/inter-4.1
    python3 - artifacts/ref/downloads/Inter-4.1.zip toolchain/fonts/inter-4.1 <<'PY'
import sys, zipfile
with zipfile.ZipFile(sys.argv[1]) as z:
    open(f"{sys.argv[2]}/Inter-Regular.ttf", "wb").write(z.read("extras/ttf/Inter-Regular.ttf"))
    open(f"{sys.argv[2]}/LICENSE.txt", "wb").write(z.read("LICENSE.txt"))
PY
fi
sha256sum -c --quiet - <<EOF
40d692fce188e4471e2b3cba937be967878f631ad3ebbbdcd587687c7ebe0c82  toolchain/fonts/inter-4.1/Inter-Regular.ttf
262481e844521b326f5ecd053e59b98c8b2da78c8ee1bdbb6e8174305e54935a  toolchain/fonts/inter-4.1/LICENSE.txt
EOF

# ---- Windows package only ---------------------------------------------------------------------
if [[ $windows == 1 ]]; then
    # DXVK as GE-Proton 11-7 ships it (PE build, it runs on Windows): the DX9 route's d3d9.dll and dxgi.dll.
    ge=GE-Proton11-7-x86_64
    if [[ ! -f "artifacts/ref/downloads/$ge.tar.gz" ]]; then
        echo "fetch $ge.tar.gz"
        curl -fsSL --retry 3 -o "artifacts/ref/downloads/$ge.tar.gz.part" \
            "https://github.com/GloriousEggroll/proton-ge-custom/releases/download/GE-Proton11-7/$ge.tar.gz"
        mv -- "artifacts/ref/downloads/$ge.tar.gz.part" "artifacts/ref/downloads/$ge.tar.gz"
    fi
    echo "7db87e9787e20c35cbdac26018431d5794626b626e4067b050684e45a88cc2ca229d7d263519eafb2e168cde5bef57611065d159d3685aaec152ccb9abe3073f  artifacts/ref/downloads/$ge.tar.gz" |
        sha512sum -c --quiet - || { echo "checksum mismatch: $ge.tar.gz" >&2; exit 1; }
    [[ -d "toolchain/$ge" ]] || tar -C toolchain -xzf "artifacts/ref/downloads/$ge.tar.gz"
    mkdir -p artifacts/ref/dxvk
    get artifacts/ref/dxvk/LICENSE \
        https://raw.githubusercontent.com/doitsujin/dxvk/601930949d111edbbcf9dd463948426d9f8f6ddd/LICENSE \
        a5cb1a6ded7d2d7e92d550ba28edd21be2d1d4044662b399887351023e30ce64
fi
echo "dependencies ready"
