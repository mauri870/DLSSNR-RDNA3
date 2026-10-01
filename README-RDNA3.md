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
- **Runs in a game** (Proton, ReShade route), see [In-game results](#in-game-results). It is too slow
  for native 4K: use a lower model resolution.

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
| 100 % | 3840x2160 | 57 ms | 49.02 dB |
| 75 % | 2880x1620 | 35 ms | 38.38 dB |
| 50 % | 1920x1080 | 16 ms | 32.86 dB |
| 37.5 % | 1440x810 | 11 ms | 30.93 dB |
| 25 % | 960x540 | 8 ms | 29.19 dB |

The PSNR column compares against NVIDIA's output at full resolution, so it measures how much of the
network's fine detail is lost, not how the frame looks; at 50 % the colour, contrast and edge treatment
of the full-resolution output are kept and fine texture is slightly softer. The network needs about
5.3 GB of video memory at 4K, twice what the RDNA4 build uses for the same values.

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

What the work taught, in the order it mattered:

- **Register spills were the main cost.** An f16 fragment takes eight VGPRs, twice what an FP8 fragment
  took on RDNA4, and the RDNA4 tile shapes sat at the 256-VGPR limit and spilled hundreds of scratch
  instructions each. Fewer fragments a wave (two waves a window, a smaller GEMM tile, a loop that the
  compiler must not unroll) removed the spills and gave 1.5 to 3 times on the kernels concerned. The
  ViT contraction ran at a third of its sibling's speed for the same arithmetic until its tile was fixed.
- **On this hardware matrix and vector instructions do not overlap**, not even within a wave: a kernel's
  time is its matrix cost plus its vector cost (a WMMA is about 36 cycles of SIMD time), and the C=32
  kernels are about 91 % busy on that sum. Occupancy stops mattering once the spills are gone, which is
  why the remaining gains are in cutting vector instructions, not in scheduling.
- **The persistent C=64/128/256 runs are better than per-layer dispatches** on this card too (8.6 ms
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

Kept here because each of these looks promising.

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
- Turning the e4m3 rounding off altogether is 40 % faster and costs 4.8 dB against NVIDIA: the rounding
  is part of the fidelity, not a cost to remove.

## What is left

Rough gains at 4K, none of them large:

- Swapping the two elements of a k pair per lane half so a B operand needs one permute and two packs:
  bit-identical, needs a weight repack in `nr_graph.cpp`, about 1 ms.
- The attention softmax epilogue still spills (about 0.3 ms); `vitattn` is vector-bound at about 44 % of
  the matrix peak (0.2 to 0.4 ms, with a risk to exactness).
- Host constants that gate better tiles: `gemmprojw` with two waves along N, `vitattn` with 64-token
  chunks, `ffwd3w` with two waves a workgroup (about 0.3 ms together).
- An f16 activation instead of the f32 one in the C=32 body would save about 0.5 ms and is a new golden
  (the three frames then score 45.50, 48.11 and 49.00 dB), so it is a decision, not a free change.

Past that the floor is the matrix work itself, about 38 ms at 4K. Native 4K at a playable frame rate is
out of reach on this card with this network; a model resolution of 50 % or lower is the setting to use.

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
frame through `nr::Runtime`, with a timing loop and a model scale), `mkplan`, `isa_stats.py`, and the two
exhaustive rounding tests. Run it with the GPU otherwise idle: a game running on the same card makes the
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

An earlier build, in the Gates of Hell scene it was first tried in (86 fps with neural rendering off),
gave 8 fps at 100 % and 23 fps at 50 % when its network took about 117 and 23 ms.
