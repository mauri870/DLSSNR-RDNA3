#!/usr/bin/env bash
# DLSSNR-AMD-Vulkan installer (Linux)
#
#   bash install.sh <folder with the game's exe> [route] [--int4 | --no-int4] [--dll <nvngx_dlssnr.dll or .zip>]
#
# Unreal Engine games: the folder is <project>/Binaries/Win64 (the *-Shipping.exe), not the top
# folder with the launcher exe.
#
# Routes:
#   optiscaler  the game has a DLSS, FSR or XeSS option: OptiScaler, with this project as its DLSS-NR backend (64-bit only)
#   reshade     D3D10/11/12 games without a usable upscaler: ReShade + VORT motion vectors
#   vulkan      Vulkan games, or D3D9 games (DXVK turns D3D9 into Vulkan under Proton)
#   dx9         as vulkan, plus the depth direction for old D3D9 games
#   remove      uninstall
# Without a route a menu is shown.
#
# If the package has no model file, point --dll at NVIDIA's nvngx_dlssnr.dll (310.8.0, or a
# zip containing it); the model is extracted from it during installation and kept in this
# package's dlssnr-amd/, so later installs from this package (into other games too) need no --dll.
#
# --int4     also install int4 mixed: part of the network runs in int4 and other lower precisions; faster, the
#            picture differs somewhat. On by default; Ctrl+F11 in game switches between int4 mixed and the default network.
# --no-int4  do not install int4 mixed: the default network only, no int4 mixed option and no switching.
#            To use int4 mixed later, run the installer again and choose it (it uninstalls and installs again).
# With neither, the installer asks once; Enter = do not install (not asked when not run in a terminal: not installed).
#
# Every installed file is listed in dlssnr-amd-install.txt in the game folder; remove deletes by it.
set -euo pipefail
here=$(cd -- "$(dirname -- "$0")" && pwd)
model_name=dlssnr.bin
# The package's model: bundled, or kept there by the first --dll install. Checked by SHA256.
pkg_model=$here/dlssnr-amd/$model_name
model_sha256=2b41c888cf4155b8958c665ba64018ab0bd25c85fc71a2b6db86d0d04d1f7fbd
model_ok() { [[ -f "$1" && "$(sha256sum -- "$1" | cut -d' ' -f1)" == "$model_sha256" ]]; }

game="" route="" dll=""
int4=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --dll) dll=${2:?--dll needs a file path}; shift 2;;
        --int4) int4=1; shift;;
        --no-int4) int4=0; shift;;
        -h|--help) sed -n '2,27p' "$0"; exit 0;;
        *) if [[ -z "$game" ]]; then game=$1; elif [[ -z "$route" ]]; then route=$1;
           else echo "unexpected argument: $1" >&2; exit 1; fi; shift;;
    esac
done
[[ -n "$game" ]] || { sed -n '2,27p' "$0"; exit 1; }
[[ -d "$game" ]] || { echo "no such folder: $game" >&2; exit 1; }
game=$(cd -- "$game" && pwd)
manifest="$game/dlssnr-amd-install.txt"
if compgen -G "$game/*/Binaries/Win64/*.exe" > /dev/null; then
    echo "Note: this looks like the top folder of an Unreal Engine game; install into <project>/Binaries/Win64 (the *-Shipping.exe) instead." >&2
fi
bits=64; [[ -f "$here/reshade/dlssnr_amd.addon32" ]] && bits=32

exe_bits=$(python3 - "$game" <<'PY' 2>/dev/null || true
import sys, pathlib, struct
seen = set()
for exe in pathlib.Path(sys.argv[1]).glob("*.exe"):
    try:
        b = exe.read_bytes()
        pe = struct.unpack_from("<I", b, 0x3C)[0]
        seen.add({0x14C: "32", 0x8664: "64"}.get(struct.unpack_from("<H", b, pe + 4)[0], "?"))
    except Exception:
        pass
print(" ".join(sorted(seen)))
PY
)

if [[ -z "$route" ]]; then
    echo
    echo "This is the $bits-bit package; the game folder's exe is ${exe_bits:-unknown}-bit. Choose a route:"
    echo "  1) optiscaler  the game has a DLSS, FSR or XeSS option (OptiScaler, 64-bit only)"
    echo "  2) reshade     D3D10/11/12 game without a usable upscaler"
    echo "  3) vulkan      Vulkan game, or D3D9 game"
    echo "  4) dx9         as 3, plus the depth setting for old D3D9 games"
    echo "  5) remove      uninstall"
    read -r -p "1-5: " pick
    case "$pick" in
        1) route=optiscaler;; 2) route=reshade;; 3) route=vulkan;; 4) route=dx9;; 5) route=remove;;
        *) echo "invalid choice" >&2; exit 1;;
    esac
fi

record() { printf '%s\n' "$1" >> "$manifest"; }
put_file() { cp -- "$1" "$game/$2"; record "$2"; }
put_tree() { mkdir -p -- "$game/$2"; cp -r -- "$1"/. "$game/$2/"; record "$2/"; }

remove_installed() {
    [[ -f "$manifest" ]] || { echo "$manifest not found, nothing to uninstall."; return; }
    while IFS= read -r entry; do
        [[ -n "$entry" ]] || continue
        case "$entry" in /*|..*|*/..*) echo "skipping suspicious entry: $entry" >&2; continue;; esac
        rm -rf -- "$game/$entry"
    done < "$manifest"
    rm -f -- "$manifest"
    echo "Uninstalled from $game."
}

case "$route" in
    remove) remove_installed; exit 0;;
    optiscaler|reshade|vulkan|dx9) ;;
    *) echo "unknown route: $route" >&2; exit 1;;
esac
if [[ "$route" == optiscaler && ! -d "$here/optiscaler" ]]; then
    echo "The OptiScaler route is only in the 64-bit package." >&2; exit 1
fi
if [[ -z "$int4" ]]; then
    int4=0
    if [[ -d "$here/int4" && -t 0 ]]; then
        echo
        echo "int4 mixed (optional): part of the network runs in int4 and other lower precisions; faster, the picture differs somewhat."
        echo "  y: install it. On by default; Ctrl+F11 in game switches between int4 mixed and the default network."
        echo "  N: do not install it. The default network only, no int4 mixed option and no switching; to use it later, run the installer again and choose y."
        read -r -p "Install int4 mixed? [y/N] " pick
        [[ "$pick" == y || "$pick" == Y ]] && int4=1
    fi
fi
if [[ $int4 == 1 && ! -x "$here/int4/dlssnr-int4-weights" ]]; then
    echo "This package has no int4 mixed." >&2; exit 1
fi
model_src=""
if [[ -f "$pkg_model" ]] && model_ok "$pkg_model"; then
    :   # the package has the model; it is installed with dlssnr-amd/
elif [[ -n "$dll" ]]; then
    model_tmp=$(mktemp)
    trap 'rm -f -- "$model_tmp" "$pkg_model.part"' EXIT
    echo "Extracting the model from $dll ..."
    bash "$here/model-tools/extract_model.sh" "$dll" "$model_tmp"
    if model_ok "$model_tmp" && cp -- "$model_tmp" "$pkg_model.part" && mv -f -- "$pkg_model.part" "$pkg_model"; then
        chmod 644 -- "$pkg_model"
        echo "The model is kept in $pkg_model; later installs from this package need no --dll."
    else
        rm -f -- "$pkg_model.part"
        echo "Note: the model could not be kept in $pkg_model (package folder not writable?); the next install needs --dll again." >&2
        model_src=$model_tmp
    fi
elif [[ -f "$pkg_model" ]]; then
    echo "$pkg_model is damaged or from another version; extract it again with --dll." >&2; exit 1
else
    echo "The package has no model file: point --dll at nvngx_dlssnr.dll (310.8.0) or its zip." >&2
    echo "Only the first time: the extracted model is kept in this package's dlssnr-amd/ and used by later installs." >&2
    exit 1
fi
if [[ $int4 == 1 ]]; then
    # The int4 weights are not in the package: made here from this model and the package's recovery tables, installed
    # only when their SHA-256 matches
    echo "Making the int4 mixed weights ..."
    int4_tmp=$(mktemp -d)
    trap 'rm -rf -- "$int4_tmp"; [[ -n "${model_tmp:-}" ]] && rm -f -- "$model_tmp" "$pkg_model.part"' EXIT
    "$here/int4/dlssnr-int4-weights" "${model_src:-$pkg_model}" "$here/int4/dlssnr-amd/int4" "$int4_tmp/dlssnr-int4.bin" \
        > "$int4_tmp/log.txt" 2>&1 || { cat "$int4_tmp/log.txt" >&2; echo "Making the int4 mixed weights failed; nothing was installed." >&2; exit 1; }
    if [[ "$(sha256sum -- "$int4_tmp/dlssnr-int4.bin" | cut -d' ' -f1)" != "$(cut -d' ' -f1 "$here/int4/dlssnr-int4.bin.sha256")" ]]; then
        echo "The int4 mixed weights have the wrong checksum (model or package incomplete?); nothing was installed." >&2; exit 1
    fi
fi
if [[ -f "$manifest" ]]; then
    echo "Found a previous installation, removing it first."
    remove_installed
fi
if [[ -n "$exe_bits" && "$exe_bits" != "?" && "$exe_bits" != *"$bits"* ]]; then
    echo "Note: this is the $bits-bit package but the game exe is $exe_bits-bit; DLLs of the other bitness will not load." >&2
fi

: > "$manifest"; record "dlssnr-amd-install.txt"
put_tree "$here/dlssnr-amd" dlssnr-amd
[[ -n "$model_src" ]] && cp -- "$model_src" "$game/dlssnr-amd/$model_name"

case "$route" in
    optiscaler)
        tmp=$(mktemp -d)
        python3 "$here/optiscaler/extract_release.py" "$here"/optiscaler/OptiScaler*.zip "$tmp" > /dev/null
        rm -f -- "$tmp/!! EXTRACT ALL FILES TO GAME FOLDER !!" "$tmp/setup_windows.bat" "$tmp/setup_linux.sh" \
                 "$tmp/nvngx.dll_dlssnr.dll"
        mv -- "$tmp/OptiScaler.dll" "$tmp/dxgi.dll"
        for f in "$tmp"/*; do
            name=$(basename -- "$f")
            if [[ -d "$f" ]]; then put_tree "$f" "$name"; else put_file "$f" "$name"; fi
        done
        rm -rf -- "$tmp"
        for f in nvngx.dll_dlssnr.dll nvngx_dlssnr.dll dlssnr_core.dll; do put_file "$here/optiscaler/$f" "$f"; done
        python3 "$here/optiscaler/patch_ini.py" "$game"
        record OptiScaler.log; record dlssnr-amd.log; record dlssnr-amd.ini
        overrides="dxgi=n,b"
        ;;
    reshade)
        for f in "$here"/reshade/*; do
            name=$(basename -- "$f")
            if [[ -d "$f" ]]; then put_tree "$f" "$name"; else put_file "$f" "$name"; fi
        done
        overrides="dxgi=n,b"
        ;;
    vulkan|dx9)
        for f in "$here"/reshade/* "$here"/vulkan/*; do
            name=$(basename -- "$f")
            [[ "$name" == dxgi.dll || "$name" == ReShadePreset-d3d9.ini ]] && continue
            if [[ -d "$f" ]]; then put_tree "$f" "$name"; else put_file "$f" "$name"; fi
        done
        [[ "$route" == dx9 ]] && cp -- "$here/vulkan/ReShadePreset-d3d9.ini" "$game/ReShadePreset.ini"
        overrides="winevulkan=n,b;vulkan-1=n,b"
        ;;
esac
[[ "$route" != optiscaler ]] && { record dlssnr-amd.ini; record dlssnr-amd.log; record ReShade.log; }
if [[ $int4 == 1 ]]; then
    # The int4 mixed programs replace the route's files of the same name just installed; dlssnr-amd/int4: network,
    # quantisers, settings, Vulkan layer (the recovery tables *.fix.bin, *.mu.bin stay in the package); the weights
    # made above go into dlssnr-amd/
    d=reshade; [[ "$route" == optiscaler ]] && d=optiscaler
    for f in "$here/int4/$d"/*; do
        name=$(basename -- "$f")
        [[ -e "$game/$name" ]] && cp -- "$f" "$game/$name"
    done
    mkdir -p -- "$game/dlssnr-amd/int4"
    (cd -- "$here/int4/dlssnr-amd/int4" && find . -type f ! -name '*.fix.bin' ! -name '*.mu.bin' -print0 |
        while IFS= read -r -d '' f; do mkdir -p -- "$game/dlssnr-amd/int4/$(dirname -- "$f")"; cp -- "$f" "$game/dlssnr-amd/int4/$f"; done)
    mv -- "$int4_tmp/dlssnr-int4.bin" "$game/dlssnr-amd/dlssnr-int4.bin"
fi
# dlssnr-amd.ini is put in place at install, with every setting and its description (a reinstall puts a fresh default)
ini_dir=$here/ini; [[ $int4 == 1 ]] && ini_dir=$here/int4/ini
if [[ "$route" == optiscaler ]]; then cp -- "$ini_dir/dlssnr-amd-optiscaler.ini" "$game/dlssnr-amd.ini"
else cp -- "$ini_dir/dlssnr-amd-addon.ini" "$game/dlssnr-amd.ini"; fi

echo
echo "Installed the $route route into: $game"
if [[ $int4 == 1 ]]; then
    echo "Steam launch options:  VK_ADD_LAYER_PATH=\"$game/dlssnr-amd/int4/layer\" VK_INSTANCE_LAYERS=VK_LAYER_DLSSNR_int4 WINEDLLOVERRIDES=\"$overrides\" %command%"
    echo "VK_ADD_LAYER_PATH and VK_INSTANCE_LAYERS in the launch options are needed by int4 mixed; copy the whole line."
    echo "int4 mixed is on by default; Ctrl+F11 in game switches to the default network and back. Settings: [Int4Mixed] in dlssnr-amd.ini, applied when saved."
else
echo "Steam launch options:  WINEDLLOVERRIDES=\"$overrides\" %command%"
fi
if [[ "$route" == optiscaler ]]; then
    echo "In the game, turn on DLSS, FSR or XeSS in its settings; Insert opens the OptiScaler menu, DLSS Neural Rendering has its own page."
    echo "If the NR page keeps showing 'Waiting for the upscaler to run', set [Spoofing] Dxgi=false in OptiScaler.ini and try again."
    echo "If colours turn green or grainy after NR (007 First Light, for example), try [Preprocess] in dlssnr-amd.ini, or press Ctrl+F10 in the game."
else
    echo "In the game, Home opens ReShade; the settings are on the Add-ons page, or edit dlssnr-amd.ini directly."
fi
echo "Logs: dlssnr-amd.log$([[ "$route" == optiscaler ]] && echo ", OptiScaler.log" || echo ", ReShade.log")"
echo "Uninstall:  bash install.sh \"$game\" remove"
