DLSSNR-AMD-Vulkan (Linux)
=========================

Runs the neural rendering (NR) model of DLSS 5 in games on AMD graphics cards, under Linux + Proton.
The network is reimplemented in Vulkan and runs on the game's own Vulkan device; NVIDIA's
runtime is neither needed nor called.

Requirements
------------
- RX 9000 series (RDNA4) graphics card
- Mesa 26.2 or newer (RADV driver)
- Proton: tested on GE-Proton 11-7
- python3 (the installer and the model extraction use it)

Install
-------
    bash install.sh "/path/to/steamapps/common/<game folder>" --dll /path/to/nvngx_dlssnr_310.8.0.zip

The folder is the one that holds the game's exe (the one that actually runs - for Unreal Engine
games it is <project>/Binaries/Win64, not the top folder with the launcher). The script lists
the routes; you can also name one after the folder:

    optiscaler  for games with a DLSS, FSR or XeSS option. OptiScaler does the upscaling and frame
                generation, this project is its DLSS-NR backend. 64-bit package only.
    reshade     for D3D10/11/12 games without a usable upscaler. ReShade + VORT provide
                the motion vectors.
    vulkan      Vulkan games, or D3D9 games (under Proton, DXVK turns D3D9 into Vulkan).
    dx9         as vulkan, plus the depth direction for old D3D9 games.
    remove      uninstall.

When it is done the script prints the line for the Steam launch options, for example:

    WINEDLLOVERRIDES="dxgi=n,b" %command%

Use the i686 package for 32-bit games and the x86_64 package for 64-bit games.

The model
---------
The weights are NVIDIA's and are not distributed with this project. They are extracted from your
own copy of nvngx_dlssnr.dll, and it must be version 310.8.0:

- give --dll either the DLL itself or a zip that contains exactly one nvngx_dlssnr.dll
  (it may sit in a subfolder of the zip);
- a DLL of any other version is refused before anything is installed;
- every one of the 599 extracted entries is checked against known hashes, and the model is
  written only if all of them match. This takes about 20 seconds.

The result is dlssnr-amd/dlssnr.bin in the game folder (147,756,560 bytes).

This is needed only once: the extracted model is also kept in this package's
dlssnr-amd/dlssnr.bin, and later installs from this package without --dll check its SHA256 and
install it from there. With a new package, use --dll once more, or copy that file over.

To make the model file without installing anything:

    bash model-tools/extract_model.sh /path/to/nvngx_dlssnr_310.8.0.zip dlssnr.bin

It prints "599 entries, 140.9 MiB" when it succeeds. The file can then be copied to
<game folder>/dlssnr-amd/dlssnr.bin by hand.

After installation the game folder has
--------------------------------------
    dlssnr-amd/                  model and shaders; the run-time pipeline cache goes here too
    dlssnr-amd-install.txt       the installation record, which remove deletes by
plus the route's own files (OptiScaler's or ReShade's DLLs, ini files, shaders).

Use
---
optiscaler: turn on DLSS, FSR or XeSS in the game; Insert opens the OptiScaler menu, the NR
            settings are on the DLSS Neural Rendering page.
            If the NR page keeps showing "Waiting for the upscaler to run", try
            [Spoofing] Dxgi=false in OptiScaler.ini (Dying Light: The Beast has needed it).
reshade / vulkan / dx9: Home opens ReShade, the settings are on the Add-ons page; the same
            settings are kept in dlssnr-amd.ini in the game folder and changes apply live.

Preprocess (optional, off by default): [Preprocess] in dlssnr-amd.ini in the game folder (the
optiscaler route writes the file on first start; the ReShade routes also show it on the Add-ons
page). It changes the picture the network is shown (exposure, display curve, contrast,
saturation), and so how NR edits the picture. Two uses: a personal look in any game (it departs
from the original look; the result may be better or worse), and games that do not hand their
exposure to the upscaler, which it fixes (see 007 First Light below). Ctrl+F10 switches it for
the current run, to compare. The file explains every setting.

Logs: dlssnr-amd.log (and OptiScaler.log or ReShade.log), all in the game folder.

Known issues
------------
- 32-bit games at 4K may fail on the first launch (the 32-bit address space runs out while
  ReShade compiles its shaders for the first time); the second launch works.
- 007 First Light (optiscaler route): with NR before or after the upscaler the picture turns
  dark, green and grainy. The game leaves exposure to the upscaler and hands over none, so the
  frame NR gets is far too dark. Turn on Preprocess (above); its defaults are meant for this.
  The older workarounds, lowering Paper white on the DLSS Neural Rendering page (about 0.06;
  [DlssNr] WhitePointScale) or [DlssNr] FinishedPicture=true, are no longer recommended.

Uninstall
---------
    bash install.sh "/path/to/<game folder>" remove
