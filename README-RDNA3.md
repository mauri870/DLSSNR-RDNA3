# DLSSNR-AMD on RDNA3 (RX 7000)

This branch runs the same neural rendering network as the main project on AMD Radeon RX 7000 cards
(RDNA3, gfx11), which have no FP8 matrix instructions. Linux and Proton only; `windows/` is not ported.
Everything in the [main README](README.md) that is not about the card still applies, including the
warnings: this is an early project, do not use it in online games with anti-cheat, and it comes
without warranty.

- **Tested only on an RX 7900 XTX** (Mesa 26.2.3, RADV). Other RDNA3 cards should work and are untested.
- Verified on single frames against NVIDIA's own output, with the frames and settings of
  [docs/ngx-verification](docs/ngx-verification/NGX-VERIFICATION.md): 45.47 dB at 1080p, 47.71 dB at
  1440p and 49.02 dB at 4K, against 45.56, 47.99 and 49.06 dB for the RDNA4 build. Moving sequences
  have not been run on RDNA3.

## What it costs

The network's time per frame on an RX 7900 XTX, one pass, measured from submit to completion
(`linux/test/check_rdna3.py --perf`):

| | 1080p | 1440p | 4K |
| --- | --- | --- | --- |
| RDNA3 (this branch) | 16 ms | 27 ms | 58 ms |
| RDNA3, first working version | 30 ms | 54 ms | 116 ms |
| RDNA4, RX 9070 XT (main README) | 5.6 ms | 9.7 ms | 21.9 ms |

That is about 2.7 times the RDNA4 cost. The part of the frame the game itself needs is on top of this.
Running the network at a lower resolution than the frame is the lever for 4K: `model_scale` in
`dlssnr-amd.ini` ("Model resolution" on the add-on's page in the game, 25 to 100 %) runs the network on a
smaller copy of the frame and carries its edit back onto the full-resolution frame. At 4K:

| Model resolution | Network extent (about) | Time | PSNR vs NVIDIA's full-resolution output |
| --- | --- | --- | --- |
| 100 % | 3840x2160 | 58 ms | 49.02 dB |
| 75 % | 2880x1620 | 35 ms | 39.39 dB |
| 50 % | 1920x1080 | 16 ms | 35.29 dB |
| 37.5 % | 1440x810 | 11 ms | 33.83 dB |
| 25 % | 960x540 | 8 ms | 32.66 dB |

The edit of a network run below the frame's size is stronger than the full-size run's (at half size its RMS
is about a third higher, and it is contrast, not detail), so the transfer pass scales it by
`0.49 + 0.52 * model_scale`; that fit is worth 1.0 dB at 75 %, 2.4 dB at 50 %, 2.9 dB at 37.5 % and 3.5 dB at
25 %, and costs no time. The rest of the loss is the network itself: a perfectly band-limited copy of the full-size
edit would score 46 to 47 dB at 50 %, so the loss is in what the network does at the smaller size, not in the upsampling.

The PSNR column compares against NVIDIA's output at full resolution, so it measures how much of the
network's fine detail is lost, not how the frame looks; at 50 % the colour, contrast and edge treatment
of the full-resolution output are kept and fine texture is slightly softer. The network needs about
5.3 GB of video memory at 4K, twice what the RDNA4 build uses for the same values.

The matrix work itself is about 38 ms at 4K. Native 4K at a playable frame rate is
out of reach on this card with this network; a model resolution of 50 % or lower is the setting to use.

## Companion project

[dlssnr-pytorch](https://github.com/mauri870/dlssnr-pytorch) is an independent PyTorch implementation of the
same network. It reads the weights out of your own copy of `nvngx_dlssnr.dll` and reproduces NVIDIA's output on
the frames of [docs/ngx-verification](docs/ngx-verification/NGX-VERIFICATION.md) to 45.5 to 49.1 dB. It served
here as a second implementation to check this one against, and as the place to run the experiments that need
a network that can be modified: the gain of the edit at a reduced model resolution, the upper bound set by a
band-limited edit, which blocks the picture depends on, what an int8 or low-rank network would lose, and what
replacing a layer would cost. Where both ran, they agree: the 50 % model resolution frame scores 35.29 dB
against NVIDIA in the PyTorch test that fitted the gain and in the Vulkan build.

## How it works on a card without FP8

The network is e4m3 (FP8) at every operation boundary. Every e4m3 value is exactly an FP16 value, so the
RDNA3 build holds them as FP16 and multiplies them in the FP16 matrix instructions with FP32
accumulation. The products are the ones the FP8 instructions form. What the FP8 hardware did in a
conversion (round to nearest even, saturate at 448) is done in ALU instructions: a constant whose ulp is
the e4m3 step is added and subtracted, which the adder rounds to nearest even by construction. It is
checked against a bit-level rounding over every f32 bit pattern and every pair of f16 patterns
(`linux/test/e4m3_round_test`), and against the reference over every f16 input. A NaN input is not
preserved; none reaches a quantiser in the network.

The RDNA4 shaders are not touched: the RDNA3 network is `linux/shaders/rdna3/`, built with
`linux/build/arch/rdna3.sh`. Two properties of gfx11 had to be measured, and the RDNA4 shaders assumed
neither: an accumulator component `c` of lane `l` is element `(2c + l/16, l%16)`, and an A or B fragment
holds sixteen components per lane, so an accumulator cannot be copied component by component into the
next stage's operand. The host keeps an FP16 twin of the activation arena and of the weights next to the
byte arenas, indexed like them, and binds it to every binding whose block holds e4m3 data. Raw 32-bit
views of an arena (tile counters, sync words, scale tables) live at alias binding 16 plus the arena's
binding and always get the arena itself.

## What made it faster

Everything below is verified with `linux/test/check_rdna3.py` and, except where stated, the pictures are
bit-identical before and after. 4K network time, ms:

| Step | Before | After |
| --- | --- | --- |
| first working version | | 116 |
| e4m3 rounding by a magic constant instead of about sixteen ALU instructions a value | 116 | 84 |
| C=32 kernels on two waves a window instead of one, the MLP streamed through its hidden fragments, the stage-1 k loop kept rolled at C=64 and C=128, the C=256 upsample projection k-outermost | 84 | 62 |
| ViT contraction GEMM and C=512 attention in 256 VGPRs | 62 | 58 |

What mattered, most important first:

- Register spills were the main cost. An f16 fragment takes eight VGPRs, twice what an FP8 fragment
  took on RDNA4, and the RDNA4 tile shapes sat at the 256-VGPR limit and spilled hundreds of scratch
  instructions each. Fewer fragments a wave (two waves a window, a smaller GEMM tile, a loop that the
  compiler must not unroll) removed the spills and gave 1.5 to 3 times on the kernels concerned. The
  ViT contraction ran at a third of its sibling's speed for the same arithmetic until its tile was fixed.
- On this hardware matrix and vector instructions do not overlap, not even within a wave: a kernel's
  time is its matrix cost plus its vector cost (a WMMA is about 36 cycles of SIMD time), and the C=32
  kernels are about 91 % busy on that sum. Occupancy stops mattering once the spills are gone, which is
  why the remaining gains are in cutting vector instructions, not in scheduling.
- The persistent C=64/128/256 runs are faster than per-layer dispatches on this card too (8.6 ms
  against about 12.4 ms for the C=64 layers).

## Where the time goes now, and the floor

The network is about 3.5 TFLOP at 4K. At the 123 TFLOPS matrix peak that is 28 ms, and 38 ms at a
realistic 75 %, against 58 ms now. By family at 4K (ms of the frame, share of the matrix peak reached):

| Kernels | ms | Peak reached |
| --- | --- | --- |
| C=256 persistent runs (fswinpds256, fswinpup256) | 9.6 | about 73 % |
| C=128 persistent runs | 8.6 | about 63 % |
| C=64 persistent runs | 8.0 | about 44 % |
| C=32 layers and the image pre/post blocks (the same body over 4x the windows) | 17.5 | about 50 % |
| ViT and C=512 GEMMs, attention, FFN | 11 | 45 to 90 % |

The C=64 runs are limited by LDS occupancy (8 KB a wave, four waves a SIMD) and cannot be raised exactly:
both 64-token buffers are live in the same stage and every wave reads both.

## What did not work

Each of these looked promising and did not pay off.

- Quantising an f32 accumulator through f16 (a packed rounding) in place of rounding it directly: no
  faster, and the same fidelity against NVIDIA.
- Raising workgroup counts or the straggler fraction of the persistent runs: residency is limited by
  LDS, not by the count. Rolling more k loops than the stage-1 one: within 1 %, and rolling the
  hidden-fragment loop is 8 to 10 % slower because indexing a fragment array by the loop variable sends
  it to scratch.
- Four waves a window at C=32; moving the expand hidden tile through LDS (VALU 7 % lower, but the
  register count rises and the kernel is 10 % slower); removing the hi-lane selects entirely (0 %).
- A weight-cache-friendlier record layout (1.4 %); the memory system is not the limit anywhere that was
  measured.
- Hiding vector work behind matrix work by scheduling: a microbenchmark with half the workgroups doing
  only WMMA and half only vector ALU takes 11.9 ms, where running them one after the other takes 11.8 ms
  and a real overlap would take about 6.3 ms. The two serialise across waves as well as within one.
- Turning the e4m3 rounding off. Keeping the values at float16 in every kernel that quantises
  (`NR_QUANT_F16_ONLY=1`) takes the 4K frame from 59.3 to 53.9 ms (9 %) and the score against NVIDIA from
  49.0 to 38.5 dB; dropping the clamp to 448 as well gives 53.3 ms and the same score. At 50 % model
  resolution it saves 1.5 ms for 0.25 dB. The rounding is part of the fidelity, and since it was reduced to
  a few instructions it is a small part of the time.
- INT8 matrix instructions instead of FP16: on RDNA3 they are not faster. `linux/test/wmma_rate/run.sh` times
  16x16x16 cooperative-matrix multiplies on an RX 7900 XTX and gets 131 to 135 TFLOPS in f16 and 132 to 140 TOPS
  in int8, so int8 would only change the memory traffic, and a simulated int8 network loses 1 to 2.6 dB
  against NVIDIA.
- Other tile shapes for the `gemmprojw` GEMM that keep the host's 64x128 tile: the best gains 0.15 ms on
  one of its two uses and loses 0.4 ms on the other. The frame time itself varies by about 0.1 ms from
  run to run.

## What is left

Gains still available at 4K, none of them large:

- Swapping the two elements of a k pair per lane half so a B operand needs one permute and two packs:
  bit-identical, needs a weight repack in `nr_graph.cpp`, about 1 ms.
- The attention softmax epilogue still spills (about 0.3 ms), and `vitattn` is vector-bound at about 44 % of
  the matrix peak.
- Skipping the e4m3 rounding in selected kernels. `NR_QUANT_F16_ONLY=1` (off by default, in
  `linux/shaders/rdna3/include/e4m3_emul.glsl`) keeps the clamp to 448 and stops rounding onto the e4m3
  grid. Built into the post block, `fswindsp32nh`, `fswinfusedup32nh`, `fswin32` and `fswinpup64` it takes the
  4K frame from 60.2 to 58.5 ms and the score against NVIDIA from 49.02 to 48.38 dB. The picture changes,
  so using it means a new golden; no pipeline uses it.
- An f16 activation instead of the f32 one in the C=32 body would save about 0.5 ms and is a new golden
  (the three frames then score 45.50, 48.11 and 49.00 dB), so it is a decision, not a free change.

## Getting the model

The weights are NVIDIA's and are not in this repository. The installer extracts them from
`nvngx_dlssnr.dll`. A zip that contains it can be downloaded from
<https://github.com/yumlevi/renodx-dlss-installer/releases/download/latest/streamline.zip>
(`streamline/nvngx_dlssnr.dll` in the zip). It is a different build of the DLL from 310.8.0, but its
weights are byte for byte the ones this project is tested with, and the installer only accepts a DLL
whose extracted model matches the pinned hashes. Do not commit the zip or anything made from it.

## Building

Everything that goes into a package is cross-compiled on Linux. Needed: git, curl, tar, python3, cmake,
ninja, g++ and the mingw-w64 cross compiler (`x86_64-w64-mingw32-g++`; `i686-w64-mingw32-g++` as well for a
32-bit package). On Ubuntu: `sudo apt install git curl python3 cmake ninja-build mingw-w64 g++`. This
branch was built and tested on an Arch-based system with Mesa 26.2.3. Do not substitute your distribution's
glslang: `fetch_deps.sh` puts glslang 16.5.0 into `toolchain/`, the build uses that one, and the SPIR-V is
reproducible only with it.

```
git clone <this repository> && cd DLSSNR-AMD && git checkout rdna3
bash fetch_deps.sh                        # pinned third-party sources and glslang 16.5.0 -> toolchain/, artifacts/ref/
bash linux/build/build_vulkan_loader.sh   # the patched Vulkan loader the vulkan and dx9 routes ship
NR_GPU=rdna3 bash linux/build/build_package.sh
```

`build_package.sh` builds the network shaders (`linux/build/build_network.py rdna3`), the host code, the
ReShade add-on, the OptiScaler DLLs and the model tools, and writes
`linux/package/DLSSNR-AMD-Vulkan-Linux-<version>-x86_64-rdna3.tar.gz`. The version comes from the git tags
(`linux/build/version.sh`; `-dirty` is appended when the tree has changes). `toolchain/`, `artifacts/` and
`build/` hold downloaded and generated files and are never committed.

- `NR_GPU=rdna3` selects the RDNA3 network: the host is compiled with the defines in
  `linux/build/arch/rdna3.sh` and the shaders with `linux/shaders/rdna3/pipelines.json`. They have to
  agree (`shader-constants.txt` ties them together) and the runtime refuses a shader folder that does not.
  Without it the build is the RDNA4 one.
- The package carries no model; `install.sh --dll` extracts it from your own DLL. To build one with a model
  you extracted yourself, for your own use, set `NR_MODEL=/path/to/dlssnr.bin`. Do not share it.
- `NR_ARCH=i686` builds the 32-bit package for 32-bit games. It has no OptiScaler route and has not been
  built or run on RDNA3.
- Only the network shaders: `python3 linux/build/build_network.py rdna3 --out build/linux/rdna3/network`.
- `windows/` is not part of this port.

## Installing

Unpack the package, then point the installer at the folder with the game's exe and at the zip:

```
bash install.sh "<folder with the game's exe>" reshade --dll streamline.zip
```

The routes are the main project's: `optiscaler` for games with a DLSS, FSR or XeSS option, `reshade` for
D3D10/11/12 games without one, `vulkan` for Vulkan and D3D9 games. The installer prints the Steam launch
options for the route (`WINEDLLOVERRIDES="dxgi=n,b" %command%` for `reshade`) and how to remove it.
Mesa 26.2 or newer and GE-Proton 11-7 are the tested versions. Logs are `dlssnr-amd.log` and
`ReShade.log` in the game folder. Set the model resolution on the add-on's page (Home opens ReShade, then
Add-ons) or with `model_scale` in `dlssnr-amd.ini`.

## Checking a change

```
python3 linux/test/check_rdna3.py --model dlssnr.bin             # build, then check against the golden
python3 linux/test/check_rdna3.py --model dlssnr.bin --perf      # also the time per frame
python3 linux/test/check_rdna3.py --model dlssnr.bin --profile   # and the time of every kernel at 4K
python3 linux/test/check_rdna3.py --model dlssnr.bin --isa       # and instruction mix, VGPRs, spills
```

It builds the network and host, runs the e4m3 quantiser against the reference over every input, runs the
three NVIDIA frames through the runtime twice each, and hashes every activation value of the 1080p
network. A change that does not alter the arithmetic (tile shape, occupancy, register allocation,
scheduling) must come out EXACT; one that reorders a sum may come out CLOSE (`--tolerance close`);
anything else fails and the first changed layer is named. `--update-golden` records a new golden, which is
a decision to make and explain in the commit. The tools it uses are in `linux/test/`: `run_frame` (one
frame through `nr::Runtime`, with a timing loop and a model scale), `mkplan`, `isa_stats.py`, `sweep_defines.py`,
the two exhaustive rounding tests and `wmma_rate/` (the matrix throughput of the card). Run it with the GPU otherwise idle: a game running on the same card makes the
timings meaningless.

## In-game results

Call to Arms - Gates of Hell: Ostfront (Direct3D 11 under Proton, ReShade route with the VORT motion
vectors and generic depth), 3840x2160 with every setting at maximum, RX 7900 XTX, Mesa 26.2.3, this
build:

| Setting | Frame rate | Frame time | Added by the network | Offline, same setting |
| --- | --- | --- | --- | --- |
| Neural rendering off | 112 fps | 8.9 ms | | |
| Model resolution 100 % | 15 fps | 66.7 ms | 57.8 ms | 57.5 ms |
| Model resolution 50 % | 37 fps | 27.0 ms | 18.1 ms | 16.4 ms |

The time the network adds in the game is what `check_rdna3.py --perf` and `run_frame` measure offline, to
within a millisecond or two, so a millisecond saved in the harness is a millisecond in the game. That
also gives the frame rates to expect from the other settings in this game, which have not been run:
about 45 fps at 37.5 % (8.9 + 11 ms) and about 60 fps at 25 % (8.9 + 8 ms); 75 % would be about 23 fps.
The add-on finds the motion vectors and the depth buffer and reports its own GPU cost, which agrees.
