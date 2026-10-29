#!/usr/bin/env bash
# DLSSNR-AMD-Vulkan installer (Linux)
#
#   bash install.sh <folder with the game's exe> [route] [--model <network>] [--renodx | --no-renodx | --wilsjo2]
#                   [--dll <nvngx_dlssnr.dll or .zip>] [--lumenite [<LumeniteFX zip>] | --no-lumenite]
#
# Unreal Engine games: the folder is <project>/Binaries/Win64 (the *-Shipping.exe), not the top folder with the launcher exe.
#
# Routes:
#   optiscaler             the game has a DLSS, FSR or XeSS option: OptiScaler + this project's NR plugin (64-bit only)
#   reshade-feeder         D3D10/11/12 games without a usable upscaler: ReShade + VORT motion vectors
#   reshade-feeder-vulkan  Vulkan games, or D3D9 games (DXVK turns D3D9 into Vulkan under Proton)
#   reshade-feeder-dx9     as reshade-feeder-vulkan, plus the depth direction for old D3D9 games
#   remove                 uninstall
# Without a route a menu is shown.
#
# If the package has no model file, point --dll at NVIDIA's nvngx_dlssnr.dll (310.9.16 or 310.8.0, or a
# zip containing it); the model is extracted from it during installation and kept in this
# package's dlssnr-amd/, so later installs from this package (into other games too) need no --dll.
# The 310.9.16 DLL holds two models, the new and the old one; the 310.8.0 DLL only the old one.
#
# All four networks are installed and can be switched in game at any time (the drop-down in NR Control or on
# ReShade's Add-ons page, or Ctrl+F11 to cycle). The choice below only sets the one a game starts with ([Network] Model):
# --model CC_SMALL_FP8                  the default network of 310.9.16 (default)
# --model CC_SMALL_FP8_EXPERIMENTAL     this project's experimental network: CC_SMALL_FP8 made faster;
#                                       its weights are made from CC_SMALL_FP8's model at install
# --model CC_SILVER_AARDWOLD            the network of 310.8.0
# --model CC_SILVER_AARDWOLD_INT4_MIXED CC_SILVER_AARDWOLD with part of the work in int4 and other lower precisions; faster, the picture differs somewhat
# Without it, the installer asks once; Enter = CC_SMALL_FP8 (not asked when not run in a terminal: CC_SMALL_FP8).
# The earlier options still work: --new-model = CC_SMALL_FP8, --old-model = CC_SILVER_AARDWOLD, --int4 = CC_SILVER_AARDWOLD_INT4_MIXED;
# --no-int4 is no longer needed (int4 mixed is always installed) and is ignored.
# int4 mixed needs its Vulkan layer in the launch options: the installer prints a second launch line with it.
#
# The processing of the picture before and after NR (the NR network itself is the same):
# OptiScaler route:
#   --renodx     the RenoDX style (default): processed the way RenoDX's DLSS5 add-on does it.
#   --wilsjo2    the wilsjo2 style: the original processing of OptiScaler-NR (wilsjo2), ported. --no-renodx means the same.
#   With neither, the installer asks once after the network; Enter = the RenoDX style (not asked when not run in a terminal: RenoDX).
#   Later NR Control (End in game) switches it at any time and can stack several layers.
# ReShade routes:
#   --renodx     the RenoDX style (default), settings in [RenoDX] (on the Add-ons page).
#   --wilsjo2    the wilsjo2 style: OptiScaler-NR's (wilsjo2) settings and names ([DlssNr]), this route's own processing. --no-renodx means the same.
#   With neither, the installer asks once after the network; Enter = the RenoDX style (not asked when not run in a terminal: RenoDX).
#   Later the Add-ons page can stack several layers, each with either style; without layers [RenoDX] Enabled, applied after a game restart.
# The old route name optiscaler-renodx still works and means optiscaler --renodx.
#
# --lumenite     (ReShade routes only) motion vectors from LumeniteFX Kernel (by Afzaal (Kaidō),
#                https://github.com/umar-afzaal/LumeniteFX, AGNYA licence). LumeniteFX is not in this package: the installer
#                downloads the version this package was tested with (f80e98d) from its author's GitHub and keeps it in this
#                package's lumenite/, so later installs do not download again; or give a LumeniteFX zip you downloaded
#                (Code > Download ZIP on its GitHub page) to install that version. The files are copied unchanged. On ReShade's Add-ons page (Motion vectors) you can switch back to VORT.
# --no-lumenite  do not install it (default): the VORT in this package. With neither, the ReShade routes ask once; Enter = no
#                (not asked when not run in a terminal: no).
#
# Every installed file is listed in dlssnr-amd-install.txt in the game folder; remove deletes by it.
set -euo pipefail
here=$(cd -- "$(dirname -- "$0")" && pwd)
# The package's models: bundled, or kept there by the first --dll install. Checked by SHA256.
# dlssnr.bin = CC_SILVER_AARDWOLD (310.8.0, also preset 1 of 310.9.16), dlssnr-small.bin = CC_SMALL_FP8 (310.9.16's default)
pkg_model=$here/dlssnr-amd/dlssnr.bin
pkg_small=$here/dlssnr-amd/dlssnr-small.bin
model_sha256=2b41c888cf4155b8958c665ba64018ab0bd25c85fc71a2b6db86d0d04d1f7fbd
small_sha256=7e966e2639dbec6596aa0bac3b8761137d2c40ca8cb4aca30df7ca10043c6e33
sha_ok() { [[ -f "$1" && "$(sha256sum -- "$1" | cut -d' ' -f1)" == "$2" ]]; }
model_ok() { sha_ok "$1" "$model_sha256"; }
small_ok() { sha_ok "$1" "$small_sha256"; }

game="" route="" dll=""
lumenite="" lumenite_zip=""
# the LumeniteFX commit this package was tested with (downloaded by --lumenite without a zip)
lumenite_commit=f80e98d716ad72e12af6f7baa727bcfeb4be5094
model=""        # the network a game starts with ([Network] Model)
renodx=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --dll) dll=${2:?--dll needs a file path}; shift 2;;
        --model) model=${2:?--model needs a network name}; shift 2
            case "$model" in CC_SMALL_FP8|CC_SMALL_FP8_EXPERIMENTAL|CC_SILVER_AARDWOLD|CC_SILVER_AARDWOLD_INT4_MIXED) ;;
                *) echo "--model takes CC_SMALL_FP8, CC_SMALL_FP8_EXPERIMENTAL, CC_SILVER_AARDWOLD or CC_SILVER_AARDWOLD_INT4_MIXED" >&2
                   exit 1;; esac;;
        --new-model) model=CC_SMALL_FP8; shift;;
        --old-model) model=CC_SILVER_AARDWOLD; shift;;
        --int4) model=CC_SILVER_AARDWOLD_INT4_MIXED; shift;;
        --no-int4) echo "Note: --no-int4 is no longer needed; int4 mixed is always installed (you do not have to use it)."; shift;;
        --renodx) renodx=1; shift;;
        --lumenite) lumenite=1
            if [[ $# -gt 1 && "$2" == *.zip ]]; then lumenite_zip=$2; shift; fi
            shift;;
        --no-lumenite) lumenite=0; shift;;
        --no-renodx|--wilsjo2) renodx=0; shift;;
        -h|--help) sed -n '2,54p' "$0"; exit 0;;
        *) if [[ -z "$game" ]]; then game=$1; elif [[ -z "$route" ]]; then route=$1;
           else echo "unexpected argument: $1" >&2; exit 1; fi; shift;;
    esac
done
[[ -n "$game" ]] || { sed -n '2,54p' "$0"; exit 1; }
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
    echo "  1) optiscaler             the game has a DLSS, FSR or XeSS option (OptiScaler, 64-bit only)"
    echo "  2) reshade-feeder         D3D10/11/12 game without a usable upscaler"
    echo "  3) reshade-feeder-vulkan  Vulkan game, or D3D9 game"
    echo "  4) reshade-feeder-dx9     as 3, plus the depth setting for old D3D9 games"
    echo "  5) remove                 uninstall"
    read -r -p "1-5: " pick
    case "$pick" in
        1) route=optiscaler;; 2) route=reshade-feeder;; 3) route=reshade-feeder-vulkan;;
        4) route=reshade-feeder-dx9;; 5) route=remove;;
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
    optiscaler) kind=optiscaler;;
    optiscaler-renodx) kind=optiscaler; route=optiscaler; [[ -z "$renodx" ]] && renodx=1;;
    reshade-feeder) kind=reshade;;
    reshade-feeder-vulkan) kind=vulkan;;
    reshade-feeder-dx9) kind=dx9;;
    reshade|vulkan|dx9)
        echo "unknown route: $route (this route is now called reshade-feeder$([[ "$route" == reshade ]] || echo "-$route"))" >&2; exit 1;;
    *) echo "unknown route: $route" >&2; exit 1;;
esac
if [[ "$kind" == optiscaler && ! -d "$here/optiscaler" ]]; then
    echo "The OptiScaler route is only in the 64-bit package." >&2; exit 1
fi
model_src="" small_src=""
# the extracted model is kept in the package; when it cannot be (package folder not writable?) this install goes on and the next one needs --dll again
keep() {   # <extracted file> <place in the package>
    if cp -- "$1" "$2.part" && mv -f -- "$2.part" "$2"; then
        chmod 644 -- "$2"; echo "The model is kept in $2; later installs from this package need no --dll."; return 0
    fi
    rm -f -- "$2.part"; echo "Note: the model could not be kept in $2 (package folder not writable?); the next install needs --dll again." >&2; return 1
}
if [[ -n "$dll" ]] && ! { model_ok "$pkg_model" && small_ok "$pkg_small"; }; then
    model_tmp=$(mktemp -d)
    trap 'rm -rf -- "$model_tmp"; rm -f -- "$pkg_model.part" "$pkg_small.part"' EXIT
    echo "Extracting the model from $dll ..."
    bash "$here/model-tools/extract_model.sh" "$dll" "$model_tmp"
    model_ok "$model_tmp/dlssnr.bin" || { echo "The extracted model has the wrong checksum; nothing was installed." >&2; exit 1; }
    keep "$model_tmp/dlssnr.bin" "$pkg_model" || model_src=$model_tmp/dlssnr.bin
    if [[ -f "$model_tmp/dlssnr-small.bin" ]]; then
        small_ok "$model_tmp/dlssnr-small.bin" || { echo "The extracted CC_SMALL_FP8 model has the wrong checksum; nothing was installed." >&2; exit 1; }
        keep "$model_tmp/dlssnr-small.bin" "$pkg_small" || small_src=$model_tmp/dlssnr-small.bin
    fi
elif model_ok "$pkg_model"; then
    :   # the package has the model; it is installed with dlssnr-amd/
elif [[ -f "$pkg_model" ]]; then
    echo "$pkg_model is damaged or from another version; extract it again with --dll." >&2; exit 1
else
    echo "The package has no model file: point --dll at nvngx_dlssnr.dll (310.9.16 - both models - or 310.8.0) or its zip." >&2
    echo "Only the first time: the extracted model is kept in this package's dlssnr-amd/ and used by later installs." >&2
    exit 1
fi
if [[ -f "$pkg_small" ]] && ! small_ok "$pkg_small"; then
    echo "$pkg_small is damaged or from another version; delete it or extract it again with --dll." >&2; exit 1
fi
have_small=1
if [[ -z "$small_src" ]] && ! small_ok "$pkg_small"; then
    have_small=0
    echo "Note: the package has no model for CC_SMALL_FP8 (dlssnr-small.bin, in the 310.9.16 nvngx_dlssnr.dll); it is greyed out in game."
    echo "To add it, install again with --dll pointing at the 310.9.16 nvngx_dlssnr.dll or its zip."
fi
int4=0; [[ -x "$here/int4/dlssnr-int4-weights" ]] && int4=1   # both the x86_64 and the i686 package have it
experimental=0; [[ -f "$here/experimental/dlssnr-small-experimental.delta" ]] && experimental=1
# the network a game starts with: all four are installed, this only sets [Network] Model in dlssnr-amd.ini
if [[ -z "$model" ]]; then
    model=CC_SMALL_FP8; [[ $have_small == 0 ]] && model=CC_SILVER_AARDWOLD
    if [[ -t 0 ]]; then
        echo
        echo "All four networks are installed and can be switched in game at any time (the drop-down in NR Control or on ReShade's Add-ons page, or Ctrl+F11 to cycle)."
        echo "Choose only the one a game starts with:"
        if [[ $have_small == 1 ]]; then
            echo "  1) CC_SMALL_FP8                    the default network of 310.9.16 (default)"
        else
            echo "  1) CC_SMALL_FP8                    not available: needs the 310.9.16 nvngx_dlssnr.dll (install again with --dll)"
        fi
        if [[ $have_small == 1 && $experimental == 1 ]]; then
            echo "  2) CC_SMALL_FP8_EXPERIMENTAL       this project's experimental network: 1) made faster"
        elif [[ $have_small == 1 ]]; then
            echo "  2) CC_SMALL_FP8_EXPERIMENTAL       not available: this package does not have it"
        else
            echo "  2) CC_SMALL_FP8_EXPERIMENTAL       not available: needs the 310.9.16 nvngx_dlssnr.dll (install again with --dll)"
        fi
        echo "  3) CC_SILVER_AARDWOLD              the network of 310.8.0"
        if [[ $int4 == 1 ]]; then
            echo "  4) CC_SILVER_AARDWOLD_INT4_MIXED   3) with part of the work in int4; faster, the picture differs somewhat"
        else
            echo "  4) CC_SILVER_AARDWOLD_INT4_MIXED   not available: this package has no int4 mixed"
        fi
        echo "Later: [Network] Model in dlssnr-amd.ini."
        def=1; [[ $have_small == 0 ]] && def=3
        read -r -p "Start with which one? [1/2/3/4, Enter = $def] " pick || pick=""
        case "${pick:-$def}" in
            1) model=CC_SMALL_FP8;; 2) model=CC_SMALL_FP8_EXPERIMENTAL;; 3) model=CC_SILVER_AARDWOLD;;
            4) model=CC_SILVER_AARDWOLD_INT4_MIXED;;
            *) echo "invalid choice, using $def.";;
        esac
    fi
fi
if [[ ($model == CC_SMALL_FP8 || $model == CC_SMALL_FP8_EXPERIMENTAL) && $have_small == 0 ]]; then
    echo "$model is not available (no CC_SMALL_FP8 model); the game starts with CC_SILVER_AARDWOLD."; model=CC_SILVER_AARDWOLD
fi
if [[ $model == CC_SILVER_AARDWOLD_INT4_MIXED && $int4 == 0 ]]; then
    echo "This package has no int4 mixed; games start with CC_SILVER_AARDWOLD."; model=CC_SILVER_AARDWOLD
fi
if [[ -z "$renodx" && "$kind" == optiscaler ]]; then
    renodx=1
    if [[ -t 0 ]]; then
        echo
        echo "The processing before and after NR (the NR network itself is the same):"
        echo "  1) RenoDX style: processed the way RenoDX's DLSS5 add-on does it (default)"
        echo "  2) wilsjo2 style: the original processing of OptiScaler-NR (wilsjo2), ported"
        echo "In game NR Control (End) switches it at any time and can stack several layers."
        read -r -p "Which one? [1/2, Enter = 1] " pick || pick=""
        case "${pick:-1}" in
            1) renodx=1;; 2) renodx=0;;
            *) echo "Not a choice; 1.";;
        esac
    fi
elif [[ -z "$renodx" ]]; then
    renodx=1
    if [[ -t 0 ]]; then
        echo
        echo "The processing before and after NR (the NR network itself is the same):"
        echo "  1) RenoDX style: processed the way RenoDX's DLSS5 add-on does it (default)"
        echo "  2) wilsjo2 style: OptiScaler-NR's (wilsjo2) settings and names ([DlssNr]), this route's own processing"
        echo "In game, ReShade's Add-ons page can stack several layers, each with either style."
        read -r -p "Which one? [1/2, Enter = 1] " pick || pick=""
        case "${pick:-1}" in
            1) renodx=1;; 2) renodx=0;;
            *) echo "Not a choice; 1.";;
        esac
    fi
fi
if [[ -z "$lumenite" ]]; then
    lumenite=0
    if [[ "$kind" != optiscaler && -t 0 ]]; then
        echo
        echo "LumeniteFX (optional): motion vectors from LumeniteFX Kernel (by Afzaal (Kaidō), https://github.com/umar-afzaal/LumeniteFX)."
        echo "  LumeniteFX is not in this package; with y it is downloaded from its author's GitHub (needs the internet) and installed unchanged."
        echo "  y: install and use LumeniteFX. On ReShade's Add-ons page (Motion vectors) you can switch back to VORT."
        echo "  N: do not install it; the VORT in this package, as before."
        read -r -p "Install LumeniteFX? [y/N] " pick || pick=""
        [[ "$pick" == y || "$pick" == Y ]] && lumenite=1
    fi
fi
if [[ $lumenite == 1 && "$kind" == optiscaler ]]; then
    echo "--lumenite is for the ReShade routes only (the OptiScaler route uses the game's own motion vectors)." >&2; exit 1
fi
lumenite_src=""
if [[ $lumenite == 1 ]]; then
    # LumeniteFX is fetched and its interface checked first: if anything is wrong, nothing has been installed
    feed_fx=$here/reshade/reshade-shaders/Shaders/DLSS5_Feed.fx
    if [[ -n "$lumenite_zip" ]]; then
        [[ -f "$lumenite_zip" ]] || { echo "$lumenite_zip not found" >&2; exit 1; }
        lumenite_src=$lumenite_zip
    else
        lumenite_src=$here/lumenite/LumeniteFX-$lumenite_commit.zip
        if [[ ! -f "$lumenite_src" ]]; then
            url=https://github.com/umar-afzaal/LumeniteFX/archive/$lumenite_commit.zip
            echo "Downloading LumeniteFX from its author's GitHub ($url) ..."
            mkdir -p -- "$here/lumenite"
            if ! { curl -fsSL --retry 2 -o "$lumenite_src.part" "$url" 2>/dev/null ||
                   wget -q -O "$lumenite_src.part" "$url" 2>/dev/null; }; then
                rm -f -- "$lumenite_src.part"
                echo "The download failed; nothing was installed. On https://github.com/umar-afzaal/LumeniteFX, Code > Download ZIP" >&2
                echo "downloads it; then install with --lumenite <the downloaded zip>." >&2
                exit 1
            fi
            mv -f -- "$lumenite_src.part" "$lumenite_src"
        fi
    fi
    lumenite_got=$(python3 "$here/lumenite.py" check "$lumenite_src" "$feed_fx") ||
        { echo "This LumeniteFX cannot be used (the reason is above); nothing was installed." >&2; exit 1; }
    if [[ -n "$lumenite_got" && "$lumenite_got" != "$lumenite_commit" ]]; then
        echo "Note: this LumeniteFX is commit ${lumenite_got:0:7}, not ${lumenite_commit:0:7} this package was tested with. Its interface checks passed; installing it."
    fi
fi
int4_weights=0
if [[ $int4 == 1 ]]; then
    # int4 weights are not in the package: they are made here from this model and the package's restore tables, and
    # installed only if their SHA-256 matches. If that fails the other two networks are installed and int4 mixed is greyed out in game (with the reason).
    echo "Making the CC_SILVER_AARDWOLD_INT4_MIXED weights ..."
    int4_tmp=$(mktemp -d)
    trap 'rm -rf -- "$int4_tmp"; [[ -n "${model_tmp:-}" ]] && rm -rf -- "$model_tmp"; rm -f -- "$pkg_model.part" "$pkg_small.part"' EXIT
    if ! "$here/int4/dlssnr-int4-weights" "${model_src:-$pkg_model}" "$here/int4/dlssnr-amd/int4" "$int4_tmp/dlssnr-int4.bin" \
            > "$int4_tmp/log.txt" 2>&1; then
        cat "$int4_tmp/log.txt" >&2
        echo "Note: making the int4 mixed weights failed; it is not available in game. The other two networks are installed." >&2
    elif [[ "$(sha256sum -- "$int4_tmp/dlssnr-int4.bin" | cut -d' ' -f1)" != "$(cut -d' ' -f1 "$here/int4/dlssnr-int4.bin.sha256")" ]]; then
        echo "Note: the int4 mixed weights have the wrong checksum (model or package incomplete?); it is not available in game. The other two networks are installed." >&2
    else
        int4_weights=1
    fi
    if [[ $int4_weights == 0 && $model == CC_SILVER_AARDWOLD_INT4_MIXED ]]; then
        echo "Games start with CC_SILVER_AARDWOLD."; model=CC_SILVER_AARDWOLD
    fi
fi
exp_weights=0
if [[ $experimental == 1 && $have_small == 1 ]]; then
    # CC_SMALL_FP8_EXPERIMENTAL's weights are not in the package: made here from CC_SMALL_FP8's model and the package's difference file,
    # installed only when their SHA-256 matches. When that fails the other networks are installed as usual and it is greyed out in game (with the reason).
    echo "Making CC_SMALL_FP8_EXPERIMENTAL's weights ..."
    exp_tmp=$(mktemp -d)
    trap 'rm -rf -- "$exp_tmp" ${int4_tmp:+"$int4_tmp"}; [[ -n "${model_tmp:-}" ]] && rm -rf -- "$model_tmp"; rm -f -- "$pkg_model.part" "$pkg_small.part"' EXIT
    if ! python3 "$here/experimental/experimental_weights.py" apply "${small_src:-$pkg_small}" \
            "$here/experimental/dlssnr-small-experimental.delta" "$exp_tmp/dlssnr-small-experimental.bin" > "$exp_tmp/log.txt" 2>&1; then
        cat "$exp_tmp/log.txt" >&2
        echo "Note: CC_SMALL_FP8_EXPERIMENTAL's weights could not be made; it is not available in game. The other networks are installed as usual." >&2
    elif [[ "$(sha256sum -- "$exp_tmp/dlssnr-small-experimental.bin" | cut -d' ' -f1)" != \
            "$(cut -d' ' -f1 "$here/experimental/dlssnr-small-experimental.bin.sha256")" ]]; then
        echo "Note: CC_SMALL_FP8_EXPERIMENTAL's weights have the wrong checksum (model or package incomplete?); it is not available in game. The other networks are installed as usual." >&2
    else
        exp_weights=1
    fi
fi
if [[ $exp_weights == 0 && $model == CC_SMALL_FP8_EXPERIMENTAL ]]; then
    echo "The game starts with CC_SMALL_FP8 instead."; model=CC_SMALL_FP8
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
[[ -n "$model_src" ]] && cp -- "$model_src" "$game/dlssnr-amd/dlssnr.bin"
[[ -n "$small_src" ]] && cp -- "$small_src" "$game/dlssnr-amd/dlssnr-small.bin"
[[ $exp_weights == 1 ]] && mv -- "$exp_tmp/dlssnr-small-experimental.bin" "$game/dlssnr-amd/dlssnr-small-experimental.bin"

case "$kind" in
    optiscaler)
        # OptiScaler as released (its files unmodified): OptiScaler.dll as dxgi.dll, its ini, its OptiScaler\ folder;
        # its licences go into that folder (a Licenses folder of the game's own is not touched); its setup scripts are
        # not needed. Our NR plugin into OptiScaler\plugins; NR Control's probe into dlssnr-amd\ (the wilsjo2 shaders are in it).
        rel=$here/optiscaler/release
        put_file "$rel/OptiScaler.dll" dxgi.dll
        put_file "$rel/OptiScaler.ini" OptiScaler.ini
        put_tree "$rel/OptiScaler" OptiScaler
        if [[ -d "$rel/Licenses" ]]; then mkdir -p -- "$game/OptiScaler/Licenses"; cp -r -- "$rel/Licenses"/. "$game/OptiScaler/Licenses/"; fi
        cp -- "$here/optiscaler/OptiScaler_LICENSE.txt" "$game/OptiScaler/Licenses/OptiScaler_LICENSE.txt"
        mkdir -p -- "$game/OptiScaler/plugins"
        cp -- "$here/optiscaler/asi/dlssnr-amd.asi" "$game/OptiScaler/plugins/dlssnr-amd.asi"
        cp -- "$here/optiscaler/asi/nr_present_probe.exe" "$game/dlssnr-amd/"
        # vulkan-1.dll forwarding to winevulkan.dll, as Proton's own: with Proton's native Vulkan loader (which Steam
        # turns on for some games) OptiScaler's menu would not show (linux/vulkan-loader/vulkan1_forwarder.c). A
        # vulkan-1.dll the game brings itself is left alone.
        if [[ -e "$game/vulkan-1.dll" ]]; then
            echo "Note: the game folder has a vulkan-1.dll of its own; it is left as it is."
        else
            put_file "$here/optiscaler/vulkan-1.dll" vulkan-1.dll
        fi
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
        [[ "$kind" == dx9 ]] && cp -- "$here/vulkan/ReShadePreset-d3d9.ini" "$game/ReShadePreset.ini"
        overrides="winevulkan=n,b;vulkan-1=n,b"
        ;;
esac
[[ "$kind" != optiscaler ]] && { record dlssnr-amd.ini; record dlssnr-amd.log; record ReShade.log; }
if [[ $lumenite == 1 ]]; then
    # into reshade-shaders/ (the whole folder is recorded and removed with the rest); ReShadePreset.ini switched to LumeniteFX:
    # DLSS5_MV_PROVIDER=3, Lumenite_Kernel on (VORT off), sorted before DLSS5_Feed
    python3 "$here/lumenite.py" install "$lumenite_src" "$feed_fx" "$game/reshade-shaders"
    preset=$game/ReShadePreset.ini
    sed -i -e '/^PreprocessorDefinitions=/ s/DLSS5_MV_PROVIDER=2/DLSS5_MV_PROVIDER=3/' \
           -e '/^Techniques=/ s/vort_MotionEffects@vort_Motion\.fx/Lumenite_Kernel@lumenite_Kernel.fx/' \
           -e '/^TechniqueSorting=/ s/^TechniqueSorting=/TechniqueSorting=Lumenite_Kernel@lumenite_Kernel.fx,/' "$preset"
    grep -q '^PreprocessorDefinitions=.*DLSS5_MV_PROVIDER=3' "$preset" &&
        grep -q '^Techniques=Lumenite_Kernel@lumenite_Kernel.fx,DLSS5_Feed@DLSS5_Feed.fx' "$preset" ||
        { echo "ReShadePreset.ini could not be switched to LumeniteFX ($preset)." >&2; exit 1; }
fi
if [[ $int4 == 1 ]]; then
    # the programs that run all four networks (int4 mixed built in) replace the ReShade route's files of the same name just installed (the OptiScaler route's NR plugin runs all four as it is);
    # dlssnr-amd/int4: network, quantisers, settings, Vulkan layer (the recovery tables *.fix.bin, *.mu.bin stay in the package); the weights made go into dlssnr-amd/
    if [[ "$kind" != optiscaler ]]; then
        for f in "$here/int4/reshade"/*; do
            name=$(basename -- "$f")
            [[ -e "$game/$name" ]] && cp -- "$f" "$game/$name"
        done
    fi
    mkdir -p -- "$game/dlssnr-amd/int4"
    (cd -- "$here/int4/dlssnr-amd/int4" && find . -type f ! -name '*.fix.bin' ! -name '*.mu.bin' -print0 |
        while IFS= read -r -d '' f; do mkdir -p -- "$game/dlssnr-amd/int4/$(dirname -- "$f")"; cp -- "$f" "$game/dlssnr-amd/int4/$f"; done)
    [[ $int4_weights == 1 ]] && mv -- "$int4_tmp/dlssnr-int4.bin" "$game/dlssnr-amd/dlssnr-int4.bin"
fi
# dlssnr-amd.ini is put in place at install, with every setting and its description (a reinstall puts a fresh default)
ini_dir=$here/ini
if [[ "$kind" == optiscaler && $renodx == 1 ]]; then cp -- "$ini_dir/dlssnr-amd-optiscaler-renodx.ini" "$game/dlssnr-amd.ini"
elif [[ "$kind" == optiscaler ]]; then cp -- "$ini_dir/dlssnr-amd-optiscaler.ini" "$game/dlssnr-amd.ini"
else
    cp -- "$ini_dir/dlssnr-amd-addon.ini" "$game/dlssnr-amd.ini"
    # the RenoDX style: Enabled = 1 in the [RenoDX] section
    [[ $renodx == 1 ]] && sed -i '/^\[RenoDX\]/,/^\[/ s/^Enabled = 0$/Enabled = 1/' "$game/dlssnr-amd.ini"
fi
# LumeniteFX: MotionVectors = lumenite (the add-on sets ReShade up by it at every start)
if [[ $lumenite == 1 ]]; then
    sed -i 's/^MotionVectors=vort$/MotionVectors=lumenite/' "$game/dlssnr-amd.ini"
    grep -q '^MotionVectors=lumenite$' "$game/dlssnr-amd.ini" || { echo "dlssnr-amd.ini: could not set MotionVectors=lumenite." >&2; exit 1; }
fi
# the network a game starts with
sed -i "/^\[Network\]/,/^\[/ s/^Model = .*/Model = $model/" "$game/dlssnr-amd.ini"
grep -q "^Model = $model\$" "$game/dlssnr-amd.ini" || { echo "dlssnr-amd.ini: could not set Model = $model." >&2; exit 1; }

echo
if [[ "$kind" == optiscaler ]]; then
    echo "Installed the $route route into: $game ($([[ $renodx == 1 ]] && echo "RenoDX style" || echo "wilsjo2 style"))"
else
    echo "Installed the $route route into: $game$([[ $renodx == 1 ]] && echo " (RenoDX style)")"
fi
echo "Networks installed: $([[ $have_small == 1 ]] && echo "CC_SMALL_FP8, ")$([[ $exp_weights == 1 ]] && echo "CC_SMALL_FP8_EXPERIMENTAL, ")CC_SILVER_AARDWOLD$([[ $int4_weights == 1 ]] && echo ", CC_SILVER_AARDWOLD_INT4_MIXED"); the game starts with $model."
echo "Switch the network in game at any time: the drop-down in $([[ "$kind" == optiscaler ]] && echo "NR Control (End)" || echo "ReShade's Add-ons page"), or Ctrl+F11 to cycle; settings: [Network] in dlssnr-amd.ini."
# the Vulkan layer in the launch options: only int4 mixed on the ReShade routes needs it (in the main launch line only when it is the starting network, else printed as a line of its own);
# the OptiScaler route needs no layer (the NR plugin draws NR Control itself and adds the device extensions int4 mixed needs)
launch() {   # <with the int4 layer: 0/1>
    local paths=() names=()
    [[ $1 == 1 && "$kind" != optiscaler ]] && { paths+=("$game/dlssnr-amd/int4/layer"); names+=(VK_LAYER_DLSSNR_int4); }
    if [[ ${#paths[@]} -gt 0 ]]; then
        echo "VK_ADD_LAYER_PATH=\"$(IFS=:; echo "${paths[*]}")\" VK_INSTANCE_LAYERS=$(IFS=:; echo "${names[*]}") WINEDLLOVERRIDES=\"$overrides\" %command%"
    else
        echo "WINEDLLOVERRIDES=\"$overrides\" %command%"
    fi
}
main_int4=0; [[ $model == CC_SILVER_AARDWOLD_INT4_MIXED ]] && main_int4=1
echo "Steam launch options:  $(launch $main_int4)"
if [[ $int4_weights == 1 && $main_int4 == 0 && "$kind" != optiscaler ]]; then
    echo "To use CC_SILVER_AARDWOLD_INT4_MIXED in game, use this line instead (every network can be switched to):"
    echo "                 $(launch 1)"
fi
if [[ "$kind" == optiscaler ]]; then
    echo "In game, turn on DLSS, FSR or XeSS in the game's settings. End opens NR Control (DirectX 11 / 12 and Vulkan games: every NR setting: processing, network, stacked layers);"
    echo "Insert opens the OptiScaler menu (upscaling and frame generation only)."
    echo "If NR Control keeps waiting for the game's upscaler, set [Spoofing] Dxgi=false in OptiScaler.ini and try again."
    echo "If colours turn green or grainy after NR (007 First Light, for example), try [Preprocess] in dlssnr-amd.ini, or press Ctrl+F10 in the game."
else
    echo "In the game, Home opens ReShade; the settings are on the Add-ons page, or edit dlssnr-amd.ini directly."
    if [[ $lumenite == 1 ]]; then
        echo "Motion vectors: LumeniteFX Kernel (by Afzaal (Kaidō), https://github.com/umar-afzaal/LumeniteFX, AGNYA licence;"
        echo "  not part of this project, installed unchanged from its author's GitHub). Motion vectors on the Add-ons page switches back to VORT;"
        echo "  when LumeniteFX cannot be used in game, VORT runs that time and dlssnr-amd.log says why."
    fi
fi
[[ "$kind" == optiscaler ]] &&
    echo "F5 takes screenshots (the same frame without and with NR) into NR Screenshots in the game folder."
if [[ "$kind" != optiscaler ]]; then
    if [[ $renodx == 1 ]]; then
        echo "Processing before and after NR: the RenoDX style ([RenoDX] Enabled = 1 in dlssnr-amd.ini), settings on the Add-ons page (that is [RenoDX])."
    else
        echo "Processing before and after NR: the wilsjo2 style ([RenoDX] Enabled = 0), settings on the Add-ons page (that is [DlssNr])."
    fi
    echo "The Add-ons page can stack several layers ([LayerN]), each RenoDX or wilsjo2. F5 takes screenshots (the same frame without and with NR) into NR Screenshots in the game folder."
fi
echo "Logs: dlssnr-amd.log$([[ "$kind" == optiscaler ]] && echo ", OptiScaler.log" || echo ", ReShade.log")"
echo "Uninstall:  bash install.sh \"$game\" remove"
