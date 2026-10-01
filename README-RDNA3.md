# DLSSNR-AMD on RDNA3 (RX 7000)

This branch runs the same neural rendering network as the main project on AMD Radeon RX 7000 cards
(RDNA3, gfx11), which have no FP8 matrix instructions. Linux and Proton only; `windows/` is not ported.
Everything in the [main README](README.md) that is not about the card still applies, including the
warnings: this is an early project, do not use it in online games with anti-cheat, and it comes
without warranty.

- **Tested only on an RX 7900 XTX** (Mesa 26.2.3, RADV). Other RDNA3 cards should work and are untested.
- **Verified on single frames** against NVIDIA's own output, with the frames and settings of
  [docs/ngx-verification](docs/ngx-verification/NGX-VERIFICATION.md): 45.47 dB at 1080p, 47.71 dB at
  1440p and 49.02 dB at 4K, against 45.56, 47.99 and 49.06 dB for the RDNA4 build. Moving sequences
  have not been run on RDNA3.
- **Games:** the package for this branch is built and installable. In-game results are listed at the end
  of this file as they are checked.

## What it costs

The network's time per frame on an RX 7900 XTX, one pass, measured from submit to completion
(`linux/test/check_rdna3.py --perf`):

| | 1080p | 1440p | 4K |
| --- | --- | --- | --- |
| RDNA3 (this branch) | 17 ms | 30 ms | 63 ms |
| RDNA4, RX 9070 XT (main README) | 5.6 ms | 9.7 ms | 21.9 ms |

That is about three times the RDNA4 cost, and optimisation for the card is under way. Running the
network at a lower resolution than the frame is the lever that exists today: `model_scale` in
`dlssnr-amd.ini` ("Model Resolution" on the add-on's page in the game, 25 to 100 %) runs the network on a
smaller copy of the frame and carries its edit back onto the full-resolution frame. At 4K:

| Model resolution | Network extent (about) | Time | PSNR vs NVIDIA's full-resolution output |
| --- | --- | --- | --- |
| 100 % | 3840x2160 | 63 ms | 49.02 dB |
| 75 % | 2880x1620 | 38 ms | 38.38 dB |
| 50 % | 1920x1080 | 18 ms | 32.86 dB |
| 37.5 % | 1440x810 | 12 ms | 30.93 dB |

The PSNR column compares against NVIDIA's output at full resolution, so it measures how much of the
network's fine detail is lost, not how the frame looks. The network needs about 5.3 GB of video memory
at 4K, twice what the RDNA4 build uses for the same values.

## How it works on a card without FP8

The network is e4m3 (FP8) at every operation boundary. Every e4m3 value is exactly an FP16 value, so the
RDNA3 build holds them as FP16 and multiplies them in the FP16 matrix instructions with FP32
accumulation. The products are the ones the FP8 instructions form; what the FP8 hardware did in a
conversion (round to nearest even, saturate at 448) is done in ALU instructions, and was checked against
the reference over every input. The RDNA4 shaders are not touched: the RDNA3 network is
`linux/shaders/rdna3/`, built with `linux/build/arch/rdna3.sh`. The host keeps an FP16 twin of the
activation arena and of the weights next to the byte arenas (see the commit that adds it).

## Getting the model

The weights are NVIDIA's and are not in this repository. The installer extracts them from
`nvngx_dlssnr.dll`. A zip that contains it can be downloaded from
<https://github.com/yumlevi/renodx-dlss-installer/releases/download/latest/streamline.zip>
(`streamline/nvngx_dlssnr.dll` in the zip). It is a different build of the DLL from 310.8.0, but its
weights are byte for byte the ones this project is tested with, and the installer only accepts a DLL
whose extracted model matches the pinned hashes. Do not commit the zip or anything made from it.

## Building the package

```
bash fetch_deps.sh                       # pinned third-party sources and glslang 16.5.0
NR_GPU=rdna3 bash linux/build/build_optiscaler_nr.sh
bash linux/build/build_vulkan_loader.sh
NR_GPU=rdna3 bash linux/build/build_package.sh
```

This needs a mingw-w64 cross compiler (`x86_64-w64-mingw32-g++`). The package is written to
`linux/package/DLSSNR-AMD-Vulkan-Linux-<version>-x86_64-rdna3.tar.gz`, without the model.

## Installing

Unpack the package, then point the installer at the folder with the game's exe and at the zip:

```
bash install.sh "<folder with the game's exe>" reshade --dll streamline.zip
```

The routes are the main project's: `optiscaler` for games with a DLSS, FSR or XeSS option, `reshade` for
D3D10/11/12 games without one, `vulkan` for Vulkan and D3D9 games. The installer prints the Steam launch
options for the route (`WINEDLLOVERRIDES="dxgi=n,b" %command%` for `reshade`) and how to remove it.
Mesa 26.2 or newer and GE-Proton 11-7 are the tested versions. Logs are `dlssnr-amd.log` and
`ReShade.log` in the game folder.

## Checking a change

`python3 linux/test/check_rdna3.py --model dlssnr.bin` builds the network and host, runs the e4m3
quantiser against the reference over every input, runs the three NVIDIA frames through the runtime twice
each, and hashes every activation value of the 1080p network. A change that does not alter the arithmetic
must come out EXACT; one that reorders a sum may come out CLOSE (`--tolerance close`); anything else
fails and the first changed layer is named. `--update-golden` records a new golden, which is a decision to
make and explain in the commit.

## In-game results

None yet.
