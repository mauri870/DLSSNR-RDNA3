# RDNA3 on the AMD Windows driver: test kit and probe

The RDNA3 network (`linux/shaders/rdna3/`) is tested on RADV. This directory holds what is needed to find out
what the AMD Windows driver (its shader compiler is LLPC) does with the same SPIR-V. `windows/` is not ported to
RDNA3; this is the evidence-gathering step before that work.

## The kit

`build_kit.sh` cross-compiles (mingw-w64) a folder and zip that a person without tools can run. They unzip it, put
NVIDIA's `nvngx_dlssnr.dll` next to it and double-click `Run test.bat` (or run `nr_tester.exe`). The tester writes
one `results.txt`.

- It records the Windows version, the graphics card and driver, the memory, and the driver's limits and
  cooperative-matrix configurations (`probe.exe --report`).
- It makes the model from the user's DLL with `windows/package/model-tools` (the file is only read). The model is
  never in the kit.
- It runs the network on a 1080p frame four times, each with a time limit: layer by layer (`NR_NO_PERSIST`, no
  persistent kernels) and with them, each with barriers between dispatches and with tile counters (`NR_TCHAIN`). It
  compares the picture with RADV's (the reference is the committed `docs/ngx-verification` output) and times 1080p
  and 720p.
- Before the tests it runs the two exhaustive rounding self-checks (`e4m3_emul_test`, `e4m3_round_test`: every f32 and
  f16 input), which show whether the driver's compiler keeps the add-and-subtract e4m3 rounding.
- After them, `nr_graph --value-stats` runs the network layer by layer and the tester compares every activation's
  rms, largest value, share of zeros and count of NaN or saturated entries with `layers-reference-1080.txt` (made
  with RADV), then lists the first values that are off, by step and producer layer. That names the first wrong layer
  when the picture is wrong.
- When all four fail, `probe.exe` creates every pipeline of the network in its own process, plus the tiny shaders
  in `micro/` and `micro2/`, and prints a table: OK, or CRASH with the module and offset inside the driver.

```
bash windows/test/rdna3-probe/build_kit.sh [output dir]      # NR_UNROLL=0 keeps the shaders as built
```

Needs, on Linux or in WSL: mingw-w64, g++, patch, zip, python3 with numpy and PIL, `toolchain/glslang`
(glslang 16.5.0) and `toolchain/Vulkan-Headers`, both from `fetch_deps.sh`. The import library for `vulkan-1.dll` is
generated from the headers, so no Vulkan loader needs building. No GPU and no model are needed; the reference
picture is the RDNA3 output committed in `docs/ngx-verification`, and `NR_MODEL` makes it again with RADV.
`run_frame.exe` is built with `diag.patch` (a step trace and a crash report); the build keeps a copy with symbols in
`work/run_frame_symbols.exe` so a crash offset maps to a line with `x86_64-w64-mingw32-addr2line`.

Giving the kit to someone else to run: `INSTRUCTIONS-FOR-AI.md`.

## What it found (Adrenalin 26.9.2, RX 7900 XT)

- The driver has `VK_KHR_cooperative_matrix` with f16 x f16 -> f32, 16x16x16, subgroup scope, and no float8. Shared
  memory is 32 KB (RADV: 64 KB).
- LLPC dies inside `vkCreateComputePipelines` (access violation in `amdvlk64.dll`, in 0.1 to 0.6 s) for 40 of the 68
  pipelines: `attn`, `ffwd3`, `ffwd3w`, every `fswin*` and the temporal variants. The other 28 compile.
- Not the cause: LDS cooperative-matrix loads and stores (21 tiny shaders pass, 64 KB of LDS included), the
  subgroup-size struct, the pipeline cache, the float8 feature struct, the workgroup-layout feature, the memory model,
  `[[dont_unroll]]`, or any single `NR_*` define (38 variants of one pipeline all crash).
- The trigger: a fragment array element that is the destination of `coopMatMulAdd` through a dynamic index
  (`acc[n]`), whose component is then read (`acc[n][c]`). A tester working independently found the same rule.
  Constant indices avoid it; a dynamic component index and dynamic whole-fragment indices (`r[k]`) are fine.
- `unroll_network.py` makes every index constant with `windows/build/unroll_glsl.py`. All four rolled/unrolled pairs
  tried compile once unrolled, and on RADV the unrolled network gives the bit-identical picture at 2 to 4 % more time.

With the unrolled network, 92 of 101 shaders compile on that driver; the nine persistent kernels (`fswinp*`,
`fswinpds*`, `fswinpup*`) still fail: seven crash at one new site (`amdvlk64.dll+0x2b0fefb`) and `fswinp64/128` return
`VK_ERROR_INITIALIZATION_FAILED`. They share the work-claiming code: `atomicExchange`, module-level variables and
unsigned compares, which no passing shader has. The first two tests leave them out. Five passing shaders declare more
workgroup memory than the 32 KB the driver advertises (`fswin256`, `fswinds256`, `fswindsp256`, `fswinfusedup256` at
64 KB, `fswinfusedup128` at 36 KB); whether they run is not known yet.

First dispatch on that driver (kit built from `8f3bd55`): the layer-by-layer network runs to the end, with barriers and
with tile counters, with no hang or crash. It takes 27.4 ms at 1080p and 15.6 ms at 720p (RADV: 18.9 and 10.4 ms), and
the picture is wrong (9.8 and 10.9 dB against RADV's, 0 to 0.2 % of pixels identical). The persistent modes still crash.
The layer check in the next kit is there to name the first wrong layer.

## Files

- `build_kit.sh`, `kit-README.txt`, `Run test.bat`: the kit.
- `nr_tester.cpp`, `probe.cpp`, `probe-shapes.txt`: the tester, the probe and the pipeline shapes of a real run.
- `diag.patch`: step trace (`NRVK_TRACE`), crash report and the `NRVK_NO_*` switches, for `run_frame`.
- `unroll_network.py`, `make_variants.py`: the unrolled shader set; one-define-removed variants of a pipeline.
- `micro/`, `micro2/`: the tiny shaders, one construct each.
- `layers-reference-1080.txt`: the per-value statistics RADV gives for the 1080p frame (`NR_NO_PERSIST=1`,
  `--no-reuse`); regenerate it with `nr_graph --value-stats` after any change that alters the arithmetic.
