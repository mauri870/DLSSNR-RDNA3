# DLSSNR-AMD

> **This is a fork that ports DLSSNR-AMD to RDNA3 (Radeon RX 7000) on Linux.**, see
> [README-RDNA3.md](README-RDNA3.md). The rest of this file describes the upstream project
> ([mochizuki0323/DLSSNR-AMD](https://github.com/mochizuki0323/DLSSNR-AMD)) for RDNA4 (RX 9000).

Runs the neural rendering (NR) model of NVIDIA DLSS 5 in games on AMD Radeon RX 9000 (RDNA4)
graphics cards.

- The network is reimplemented as Vulkan compute shaders.
- The model is not included. The installer extracts it from your own copy of `nvngx_dlssnr.dll`.
  Version 310.9.16 holds two models: the 310.9.16 new model, its default and faster, and the 310.8.0 old
  model, so from 310.9.16 alone the installer installs both. Since 0.0.6-linux-preview the Linux version
  runs the 310.9.16 new model by default; the 310.8.0 old model stays selectable (see
  [The model](#the-model)). The Windows version still runs the 310.8.0 old model only.
- The Linux build with the 310.9.16 new model, on an RX 9070 XT, checked against NVIDIA's own `nvngx_dlssnr.dll`
  310.9.16 run on an RTX 5090 through NGX, the same way as below. Single frames: PSNR 49.15 dB at 1080p,
  49.40 dB at 1440p and 50.59 dB at 4K, SSIM 0.997. Moving sequences: PSNR 50.6-52.3 dB at 1080p,
  51.2-52.4 dB at 1440p and 52.2-53.1 dB at 4K, SSIM 0.998, and the output changes from frame to frame by
  about as much as NVIDIA's. Method, pictures and data:
  [docs/ngx-verification/310.9.16](docs/ngx-verification/310.9.16/NGX-VERIFICATION.md).
- The Linux build with the 310.8.0 old model, on an RX 9070 XT, checked against NVIDIA's own `nvngx_dlssnr.dll`
  310.8.0 run on an RTX 5090 through NGX (the way a game calls it), with the same input and settings.
  Single frames: PSNR 45.56 dB at 1080p, 47.99 dB at 1440p and 49.06 dB at 4K (higher is closer; the input frame
  itself scores 26-29 dB), SSIM 0.996-0.997 (1 = identical); NVIDIA's DLL gives byte-identical output
  from run to run, so the difference is between the two implementations, not noise. Moving sequences
  with motion vectors and history (a still scene, a moving object, a panning camera, and NR after an
  upscaler; 10 frames each): PSNR 46.3-47.0 dB at 1080p, 50.1-50.9 dB at 1440p and 51.1-51.6 dB at 4K,
  SSIM 0.997-0.998, and the output changes from frame to frame by about as much as NVIDIA's. Method,
  pictures and data: [docs/ngx-verification/310.8.0](docs/ngx-verification/310.8.0/NGX-VERIFICATION.md).
- **Tested only on an RX 9070 XT.** Other cards are not guaranteed to work.
- **The Windows version is an experimental preview and has not been tested much.** Game crashes,
  driver resets and other unexpected problems can happen. With its default settings the network is
  slower than on Linux. With [ACO Mode](#windows-experimental-preview) it loads machine code that was
  made in advance with Mesa's ACO compiler (the Linux driver's compiler) and ships in the package, and
  comes close to Linux (see [Performance](#performance)). ACO Mode has been tested on one driver version only (AMD Software
  26.9.2); see [Windows](#windows-experimental-preview). Since 0.0.5-win-preview it has
  [int4 mixed](#int4-mixed-optional), the [RenoDX style](#renodx-style-optional) and
  [NR Control](#nr-control-optiscaler-route) as well.

This is an independent project. It is not affiliated with, endorsed by or supported by NVIDIA or
AMD. DLSS is a trademark of NVIDIA Corporation.

## Before you use it

This is an early project (version 0.0.x) and its testing is limited:

- one graphics card, one Linux desktop, Mesa 26.2 and GE-Proton;
- a small number of games, at a few resolutions and settings;

Other cards, drivers, Proton versions, distributions and games have not been tested. Expect problems:
games that fail to start or crash, visual artifacts, settings that do not behave as described, or
performance that differs from the numbers below. Problems are fixed as they are found, and the project
will keep changing, including in ways that break earlier setups.

- The installer puts DLLs into the game folder. Keep a backup of anything you care about; `remove`
  deletes what was installed.
- **Do not use it in online games with anti-cheat.** Injected DLLs can get an account banned.
- It is provided as is, without warranty (see [LICENSE](LICENSE)).

If something goes wrong, please open an issue with the game, the route, your card and driver, and
these logs:

- `dlssnr-amd.log` and `OptiScaler.log` or `ReShade.log` (whichever route you use), all in the game
  folder. On Windows, option 5 of `install.bat` puts them into one zip.
- On Linux, if you can, a Proton log as well: set the game's launch options in Steam to
  `PROTON_LOG=1 %command%`, run the game until the problem shows, and attach `steam-<appid>.log` from
  your home folder.

## Status

| Platform | Needs | State |
| --- | --- | --- |
| **Linux** (Steam / Proton) | Mesa 26.2 or newer, a recent Proton | The main version, used in games. |
| **Windows** | AMD Software 26.9.2 (tested) | **Experimental preview, not tested much.** Slower than Linux by default, close to it with ACO Mode; game crashes, driver resets and other problems can happen. |

Both need an RX 9000 series (RDNA4) card; older cards (RX 7000 and earlier) lack the FP8 matrix
instructions the network needs.

## Performance

310.9.16 new model (Linux, v0.0.6-linux-preview), GPU time of the network per frame on an RX 9070 XT,
**measured in game** (network only):

| | 1080p | 1440p | 4K |
| --- | --- | --- | --- |
| Linux, 310.9.16 new model | 3.27 ms | 5.53 ms | 12.67 ms |

310.8.0 old model, GPU time of the network per frame on an RX 9070 XT, **offline benchmark** (network
only), measured with v0.0.3 (the int4 mixed row with v0.0.4, the Windows ACO Mode row with v0.0.4-win-preview
on AMD Software 26.9.2). In games, the network-only time the log shows is lower than in this offline
benchmark:

| | 1080p | 1440p | 4K |
| --- | --- | --- | --- |
| Linux | 5.60 ms | 9.70 ms | 21.89 ms |
| Linux, [int4 mixed](#int4-mixed-optional) | 4.99 ms | 8.69 ms | 19.64 ms |
| Windows | 7.21 ms | 12.59 ms | 27.32 ms |
| Windows, [ACO Mode](#windows-experimental-preview) On | 5.70 ms | 9.96 ms | 22.49 ms |

Windows with ACO Mode On and int4 mixed runs at about the same speed as Linux with int4 mixed.

Running the model below the output resolution about doubles the frame rate. In the ReShade routes
that is the **Model resolution** setting. In the OptiScaler route it is mainly OptiScaler-NR's
**"Generate model before upscaler"** option, which runs the model on the game's render-resolution
frame before FSR4 upscales it; OptiScaler-NR has a **Model resolution** setting as well.

## Supported games

Linux and Windows have the same routes:

| Game uses | Route |
| --- | --- |
| **DirectX 12 / 11** | `optiscaler` if the game has a DLSS, FSR or XeSS option, otherwise `reshade-feeder` |
| **DirectX 10** | `reshade-feeder` |
| **DirectX 9** | `reshade-feeder-dx9` |
| **Vulkan** | `reshade-feeder-vulkan` |
| OpenGL | not supported |

- **`optiscaler`**: for games with a DLSS, FSR or XeSS option. Uses
  [OptiScaler-NR](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass) with this project as its
  DLSS-NR backend, and the game's own motion vectors. 64-bit games only.
- **`reshade-feeder`**: for other DirectX 10/11/12 games. Runs through ReShade; motion is estimated from
  the picture by a ReShade shader.
- **`reshade-feeder-vulkan` / `reshade-feeder-dx9`**: for Vulkan and DirectX 9 games. DirectX 9 games
  run on Vulkan through DXVK, and ReShade is loaded as a Vulkan layer underneath it.
  `reshade-feeder-dx9` also sets the depth direction old DirectX 9 games use.

These routes were called `reshade`, `vulkan` and `dx9` up to 0.0.4 on Linux and up to 0.0.4-win-preview
on Windows. Any route can also use the [RenoDX style](#renodx-style-optional).

32-bit games work on Linux (use the i686 package; not with `optiscaler`). The Windows version is
64-bit only.

## NR Control (OptiScaler route)

On the OptiScaler route, End opens NR Control, this project's own NR menu. On Linux it is a Vulkan layer
in the game folder (the installer's launch options name it); on Windows the NR DLL draws it over the
game's DirectX 11 or 12 swap chain, with nothing to set up. It always has the int4 mixed and Preprocess
switches.
Where the other NR settings are:

- RenoDX style on: also in NR Control - Intensity, Style, Local tone, Local structure, Skin structure,
  passes, composition, resolution, Adjust NR's look, Detail stability, encoding and screenshots. The NR
  settings in the OptiScaler menu are locked (changing them there has no effect); OptiScaler then only
  does upscaling and frame generation.
- RenoDX style off: on the DLSS Neural Rendering page of the OptiScaler menu (Insert), as before.

It also shows what NR is doing (frame and network sizes, passes, NR's cost split into the network and
the rest of the processing). On Linux it works on X11 (XWayland) and Wayland (`PROTON_ENABLE_WAYLAND=1`).
On Windows it is drawn in the swap chain's encoding (SDR, scRGB or HDR10). While closed it draws nothing
and submits no GPU work.

## RenoDX style (optional)

An install option (`--renodx` on Linux, a question or `-RenoDX on` in the Windows installer, or
`[RenoDX] Enabled = 1` in `dlssnr-amd.ini`, applied after a game restart): the processing around NR -
how the picture is handed to the network and how its result is put back - follows
[RenoDX's DLSS5 add-on](https://github.com/PEQHUB/RenoDX-DLSS5-Generic) v8.5.0-rc10, with RenoDX's own
shaders. The NR network itself is the same.

- Every NR setting is then in `[RenoDX]`, with RenoDX's names, defaults and ranges: in NR Control on the
  OptiScaler route (OptiScaler's own menu then only drives upscaling and frame generation), on the
  Add-ons page on the ReShade routes.
- The settings: Intensity, Style, Local tone, Local structure, Skin structure and the automatic mask;
  passes with their own settings; colour and transfer strength; the network's resolution with
  edge-aware enlargement; NR before or after the upscaler; Adjust NR's look; Detail stability; HDR and
  encoding (HDR10 on the ReShade routes too); NR screenshots (F5: the same frame without and with NR).
- Detail stability (Auto by default, as in RenoDX) costs extra GPU time; Off saves it.

## Preprocess (optional)

`[Preprocess]` in `dlssnr-amd.ini` in the game folder changes the picture the NR network is shown (exposure,
display curve, contrast, saturation), and so how NR edits the picture. It has two uses: a personal look in any
game, and fixing games that do not hand their exposure to the upscaler (below). The installer writes the
file; the ReShade routes also show the settings on the Add-ons page, and NR Control on the OptiScaler route
switches it on and off.

- **Enabled**: off by default; when off, none of it runs. Turned on, it starts from auto exposure and the
  filmic curve.
- **Exposure**:
  - `auto`: auto exposure after Unreal Engine's design, for games that compute their exposure but do not hand
    it to the upscaler.
  - `off`: the exposure stays as the game or OptiScaler set it.
  - `fixed`: `ExposureBias` alone.
- **ExposureBias** (EV, -8 to +8, decimals allowed): with `fixed` it is the whole gain; with `auto` it is added
  to what auto works out.
- **Curve, Contrast, Saturation**: change the picture NR is shown, and so how it edits the picture.
- **Hotkey** (Ctrl+F10 by default): switches the preprocess on and off for the current run, to compare on
  the same picture. It does not change the file.
- **Sound**: a short sound when it switches: two notes going up for on, going down for off. `Sound = 0`
  turns it off.

Some games hand the upscaler a picture without its exposure, and NR then works on a frame that is several
stops too dark: 007 First Light turns green and grainy with NR. Such games are fixed by the preprocess: set
`Enabled = 1` (its defaults, auto exposure and the filmic curve, are meant for this).

For a personal look in a game that gives its exposure properly, `auto` is not needed: use `Exposure = off`
(or `fixed` for a small shift) and change `Curve`, `Contrast` or `Saturation`. This departs from the original
look; it may be better or worse.

The first time the preprocess is turned on, NR rebuilds; a second or two of frames go without NR.
The file itself explains every setting.

## int4 mixed (optional)

A second network, faster than the default one (see Performance): part of its computation runs in int4,
a lower precision than the original network uses, so its picture differs somewhat from the default
network's. It exists for the 310.8.0 old model only. Both Linux packages and the Windows package have it; it
is chosen at install (on Linux after choosing the 310.8.0 old model).

On Windows it is choice 3 of the installer's network question (`-Network aco-int4-mixed`) and runs with
[ACO Mode](#windows-experimental-preview) only: the AMD driver's own compiler makes slow code for the
int4 kernels, so this version does not offer int4 mixed without ACO Mode. The rest of this section is
about Linux; the switching, the weights and their check work the same way on Windows.

- With the 310.8.0 old model the installer asks once (Enter = no), or pass `--int4` or `--no-int4` (`--int4`
  implies `--old-model`).
- With it, the Steam launch options the installer prints have `VK_ADD_LAYER_PATH` and
  `VK_INSTANCE_LAYERS` (a Vulkan layer in the game folder that int4 mixed needs; on the OptiScaler route
  they name NR Control's layer too). Copy the whole line.
- Once installed it is on. In game, Ctrl+F11 (or NR Control on the OptiScaler route) switches between
  int4 mixed and the default network, to compare them on the same picture; a switch builds the other network in the background, which takes a
  few seconds (`KeepBoth = 1` keeps both networks in video memory and makes switching instant). The
  settings are in `[Int4Mixed]` in `dlssnr-amd.ini`.
- With int4 mixed, NR takes much longer to take effect when a game starts. Until then the picture goes
  without NR; this does not mean int4 mixed is not working.
- Its weights are made during installation from your model and the tables in the package, and checked
  against a known SHA-256. The tables (`linux/data/int4/`) come from this project's own calibration on
  1,112 game frames and darkened copies of some of them. int4 mixed is the network these tables define;
  another calibration would give a different network.

## Why Vulkan

HIP would work too: ROCm supports RDNA4 and its matrix (WMMA) instructions. Vulkan fits this job
better:

- The network runs on the game's own Vulkan device and queue (DXVK / vkd3d-proton under Proton). The
  frame never leaves that device, and no second GPU context or cross-API synchronisation is needed.
- Nothing extra to install: Vulkan comes with the graphics driver. On Linux the HIP runtime
  (`libamdhip64`) is not part of Mesa and has to be installed separately (ROCm or the distribution's
  packages), plus a bridge to reach it from a game running in Proton (a Windows process). On Windows
  the AMD driver includes it (`amdhip64_7.dll` in current drivers).
- RDNA4's matrix instructions are available in Vulkan through `VK_KHR_cooperative_matrix`, both in
  Mesa (Linux) and in the AMD Windows driver.

How the routes work in detail, and where the code is: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Install (Linux)

Download a `DLSSNR-AMD-Vulkan-Linux-*-x86_64.tar.gz` (64-bit games) or `-i686.tar.gz` (32-bit games) from
the releases, unpack it and run:

```sh
bash install.sh "/path/to/steamapps/common/<game folder>" --dll /path/to/nvngx_dlssnr_2026_10_16.zip
```

The folder is the one holding the game's exe. The installer lists the routes, asks whether to use the
310.9.16 new model (Enter = yes; with the 310.8.0 old model it then asks whether to add
[int4 mixed](#int4-mixed-optional)) and whether to turn on the [RenoDX style](#renodx-style-optional),
extracts the model (below) and prints the Steam launch options to use. `--dll` is needed only for the first install:
the extracted model is kept and reused for every later one. Needs `bash` and `python3`. See
[linux/package/README.txt](linux/package/README.txt).

## Install (Windows)

Download `DLSSNR-AMD-Vulkan-Windows-*-preview-x86_64.zip` from the releases, unpack it and double-click
`install.bat`. Pick the game's exe (the one that actually runs, not a launcher), then the route, the
network (the default network compiled by the AMD driver, the default network as ACO machine code, or
[int4 mixed](#int4-mixed-optional)) and whether to use the [RenoDX style](#renodx-style-optional). The
first install asks for `nvngx_dlssnr.dll` 310.8.0 or a zip that contains it and extracts the model
(below); later installs from the same package do not ask again. Read
[windows/package/README.txt](windows/package/README.txt) and [Windows](#windows-experimental-preview)
first.

## The model

The weights are NVIDIA's and are not part of this project. They are extracted from your own copy of
`nvngx_dlssnr.dll`. Linux takes **version 310.9.16** or **310.8.0**. 310.9.16 already contains the 310.8.0
model, so with it both models are extracted and installed; 310.8.0 gives the 310.8.0 old model only. Use
310.9.16 when you can. Windows takes 310.8.0 only.

| File | Model |
| --- | --- |
| `dlssnr-amd/dlssnr-small.bin` | the 310.9.16 new model (310.9.16's default, faster); Linux default |
| `dlssnr-amd/dlssnr.bin` | the 310.8.0 old model (310.9.16 has it too) |

- Input: the DLL itself, or a zip that contains exactly one `nvngx_dlssnr.dll` (it may be in a
  subfolder).
- A DLL of any other version is refused.
- Every extracted entry (599 for the 310.8.0 old model, 255 for the 310.9.16 new model) is checked
  against known hashes; a model file is written only if every one matches. On Linux it takes about 40 seconds for 310.9.16 and 20
  seconds for 310.8.0, and needs `bash` and `python3`. The Windows package does the same with
  `model-tools\dlssnr_extract_model.exe` (no Python needed), in about a second.
- The extracted models are kept in the package's own `dlssnr-amd/`. Later installs from that package
  without `--dll` check their SHA256 and install them from there, so the extraction runs only once per
  package. With a new package, use `--dll` once more or copy those files over.
- Every model the package has is installed. Which one runs is `[Network] Model` in `dlssnr-amd.ini`
  (`small` = the 310.9.16 new model, `original` = the 310.8.0 old model), set at install
  (`--new-model` / `--old-model`, or the installer's question) and changeable afterwards; it takes effect
  after a game restart. If the 310.9.16 new model is chosen but not there (a 310.8.0 DLL), the 310.8.0 old
  model runs and the log says why.

Two ways to run it:

```sh
# while installing: the models go to <game folder>/dlssnr-amd/
bash install.sh "<game folder>" --dll nvngx_dlssnr_2026_10_16.zip

# on its own, into an existing folder: model-tools/ in the package, linux/package/model-tools/ in this repository
bash model-tools/extract_model.sh nvngx_dlssnr_2026_10_16.zip <folder>
```

On success it prints `599 entries, 140.9 MiB` for `dlssnr.bin` (147,756,560 bytes) and
`255 entries, 108.6 MiB` for `dlssnr-small.bin` (113,917,248 bytes). Put the files in
`<game folder>/dlssnr-amd/`, or pass them to a package build with `NR_MODEL=` and `NR_MODEL_SMALL=`.

On Windows, `install.bat` extracts the model by itself; to run the tool on its own:

```bat
model-tools\dlssnr_extract_model.exe nvngx_dlssnr.dll dlssnr.bin
```

It takes the DLL itself; unpack it from the zip first.

## Windows (experimental preview)

**The Windows version is an experimental preview and has not been tested much.** Game crashes, driver
resets and other unexpected problems can happen. It is built from the same code as the Linux version
and changed only where the AMD Windows driver needs it. It is updated less often than the Linux
version, and some releases may be Linux only.

DirectX 10/11/12 games keep the system's own D3D. NR runs on a Vulkan device of its own on the same
card, and each frame is handed between the two through shared textures and a shared fence, on the
GPU. DirectX 9 games run on Vulkan through DXVK, because D3D9 cannot share a frame with another API.

**ACO Mode.** `[Network] ACO Mode` in `dlssnr-amd.ini` chooses where the network's machine code comes
from. Off (`ACO Mode = 0`, the default): the AMD driver compiles the network. On (`ACO Mode = 1`): the
package carries the network as machine code made in advance with Mesa's ACO compiler (the Linux
driver's compiler), and the AMD driver loads that code instead of compiling the network. ACO's machine
code for this network runs faster than the AMD driver's (see Performance). It takes effect when the
game restarts.
**ACO Mode has been tested on one driver version only (AMD Software 26.9.2, RX 9070 XT). With other
drivers it may fail: game crashes, wrong pictures or driver resets. If that happens, turn it off.**
When ACO Mode cannot be used at all, NR runs as with it off and `dlssnr-amd.log` says why.
[int4 mixed](#int4-mixed-optional) needs ACO Mode On.

Known issues:

- With ACO Mode off it is slower than the Linux version (see Performance).
- The first time NR runs in a game, the network compiles for about a minute before it takes effect
  (on Linux about 10-20 seconds, with ACO Mode On a few seconds); this happens once for each game.
  Until then the picture looks as without NR, which does not mean the mod is not working: give it a
  minute.
- DirectX 9 games run entirely on DXVK, which changes their performance and behaviour; overlays such
  as Steam's may not show.
- In DirectX 10 games the game's depth buffer is not passed to NR yet (not implemented).
- NR Control has been tested over DirectX 12 swap chains only; the RenoDX style has not been tested in a
  DirectX 10 game.
- Games whose FSR runs in their own shaders never hand it to OptiScaler: pick the game's DLSS option
  instead, which OptiScaler offers.
- The network pauses itself when video or system memory runs short.
- With ACO Mode off the network is built by the AMD Windows driver's shader compiler, which rounds
  differently from the Linux version in places; on a 1080p test frame the two outputs are 48.5 dB
  PSNR apart.

## Build

Tested on Ubuntu 24.04. Everything, including the Windows DLLs, is cross-compiled on Linux.

```sh
sudo apt install git curl python3 cmake ninja-build mingw-w64 g++-multilib
bash fetch_deps.sh                          # pinned third-party pieces -> toolchain/, artifacts/ref/
bash linux/build/build_vulkan_loader.sh     # the patched Vulkan loader for the reshade-feeder-vulkan/-dx9 routes
NR_ARCH=x86_64 bash linux/build/build_package.sh
NR_ARCH=i686   bash linux/build/build_package.sh
```

The packages land in `linux/package/`. They contain no model; `install.sh --dll` extracts it. To put
models you extracted yourself into a package for your own use, set `NR_MODEL=/path/to/dlssnr.bin` (and
`NR_MODEL_SMALL=/path/to/dlssnr-small.bin` for the 310.9.16 new model).
Do not share packages that contain the model.

### Windows package

```sh
bash fetch_deps.sh --windows                # adds GE-Proton 11-7 (DXVK)
bash windows/build/build_package.sh
```

The result is `windows/package/DLSSNR-AMD-Vulkan-Windows-*-preview-x86_64.zip`; on Windows, run `install.bat` from it.
It contains no model; the installer extracts it on the first install. `NR_MODEL=/path/to/dlssnr.bin`
puts one into the package for your own use. Do not share packages that contain the model.

## Licence

MIT for the code in this repository, see [LICENSE](LICENSE). The packages also carry third-party
components under their own licences, listed in [THIRD_PARTY.md](THIRD_PARTY.md). NVIDIA's model
weights are NVIDIA's property and are neither included nor distributed.
