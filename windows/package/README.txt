DLSSNR-AMD-Vulkan (Windows, experimental preview)
=================================================

Runs the neural rendering (NR) model of DLSS 5 in games on AMD graphics cards. The network is
reimplemented in Vulkan; NVIDIA's runtime is neither needed nor called.

This is an experimental preview and has not been tested much: game crashes, driver resets and
other unexpected problems can happen. It is slower than the Linux version and updated less
often, and some releases may be Linux only. If something breaks, uninstall with option 4 of
install.bat.

On Windows the game itself is D3D11/D3D12 and has no Vulkan device, so the installer puts DXVK
and vkd3d-proton into the game folder: the game runs on Vulkan and NR shares its device.

Requirements
------------
- RX 9000 series (RDNA4) graphics card, AMD driver 25.10 or newer
- 64-bit games
- Do not use it in online games with anti-cheat; start games with EasyAntiCheat with EAC off

Install
-------
Double-click install.bat, pick the game's exe (the one that actually runs - for Unreal Engine
games it is under Binaries\Win64, not the launcher outside), then pick a route:

  1) OptiScaler   DX12 games with a DLSS, FSR or XeSS option (DX12 only)
  2) ReShade      other DX10/11/12 or Vulkan games, including DX11 games with a DLSS option
  3) ReShade      old DX9 games
  4) Uninstall
  5) Collect logs when something goes wrong: writes a zip to the desktop to attach to an issue

You can also drag the game's exe onto install.bat. Games under Program Files ask for
administrator rights.

Model
-----
The package does not contain NVIDIA's model. The first install asks for nvngx_dlssnr.dll
(version 310.8.0 only) or a zip that contains it. The installer extracts the model with
model-tools\dlssnr_extract_model.exe (it only reads the weight data; the DLL is never loaded or
run), checks every entry against known hashes and writes dlssnr-amd\dlssnr.bin only if all
match. The model stays in this package, so later installs from it (into other games too) do not
ask again; with a newer package, choose the DLL once more or copy that file over.

Use
---
OptiScaler: turn on DLSS (or FSR / XeSS) in the game's graphics settings. Insert opens the
            OptiScaler menu, the NR settings are on the DLSS Neural Rendering page. The
            upscaler defaults to XeSS.
            If the NR page keeps showing "Waiting for the upscaler to run", set
            [Spoofing] Dxgi=false in OptiScaler.ini and try again.
ReShade:    Home opens ReShade, the settings are on the Add-ons page; the same settings are
            kept in dlssnr-amd.ini in the game folder and changes apply live.

Preprocess (optional, off by default): [Preprocess] in dlssnr-amd.ini in the game folder (the
OptiScaler route writes the file on first start; ReShade also shows it on the Add-ons page).
It changes the picture the network is shown (exposure, display curve, contrast, saturation),
and so how NR edits the picture. Two uses: a personal look in any game (it departs from the
original look; the result may be better or worse), and games that do not hand their exposure
to the upscaler, which it fixes (007 First Light turns green and grainy with NR otherwise).
Ctrl+F10 switches it for the current run, to compare. The file explains every setting.

The first time NR runs in a game the network has to compile; it takes effect after about a
minute (much longer than on Linux, where it takes 10-20 seconds). Until then the picture looks
as without NR, which does not mean the mod is not working: give it a minute. After that it is
cached. This happens once for each game.

Files put into the game folder
------------------------------
    dlssnr-amd\                  model and shaders
    d3d12.dll d3d12core.dll      vkd3d-proton
    vulkan-1.dll                 Vulkan loader (never calls DXGI; in the ReShade route it loads
                                 ReShade from the game folder)
    d3d11.dll d3d10core.dll dxgi.dll d3d9.dll   DXVK (in the OptiScaler route dxgi is
                                 OptiScaler, DXVK's dxgi is renamed dxgi-dxvk.dll, and
                                 dxgi-original.dll hands the graphics driver's own calls to
                                 the system DXGI)
    dlssnr-amd-install.txt       the installation record, which uninstall deletes by
plus the route's own files. Files of the same name already in the game folder are first moved
to dlssnr-amd-backup\ and put back on uninstall.

Logs: dlssnr-amd.log, and OptiScaler.log or ReShade.log, all in the game folder.

Known limits
------------
- The whole game runs on DXVK / vkd3d-proton; the frame rate can differ from native D3D.
- In the ReShade route, overlays such as Steam's may not show; in the OptiScaler route
  overlays are turned off (otherwise the game hangs).
- DX11 games cannot use the OptiScaler route: for DX11 games OptiScaler hands the picture to
  the system's D3D12, and on Windows DXVK's picture cannot be shared that way. Use the ReShade
  route.
- Games whose FSR runs in their own shaders never hand it to OptiScaler: pick the game's DLSS
  option instead, which OptiScaler offers.
- When video memory or system memory (including virtual memory) runs short, the network pauses
  by itself and the picture returns to normal until memory frees up. Set the page file to
  "System managed size".
- 64-bit package only.

Third-party components
----------------------
DXVK (zlib licence) and vkd3d-proton (LGPL-2.1) come from GE-Proton 11-7; the versions are in
version.txt in their folders. Sources: https://github.com/doitsujin/dxvk ,
https://github.com/HansKristian-Work/vkd3d-proton . The licence files of ReShade 6.8.0,
OptiScaler-NR 0.8.4, the Vulkan Loader (with a description of the changes), vort_Shaders and
DLSS5-Feeder are in their folders.
