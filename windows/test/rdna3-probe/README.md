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
- It runs the network on a 1080p frame twice, with barriers between dispatches and with tile counters
  (`NR_TCHAIN`), each with a time limit, compares the picture with RADV's (the reference is the committed
  `docs/ngx-verification` output) and times 1080p and 720p.
- When both runs fail, `probe.exe` creates every pipeline of the network in its own process, plus the tiny shaders
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

Not yet known: whether the unrolled network dispatches and gives the right picture on LLPC, how fast it is, and
whether tile counters make progress there. That is what the kit's first two tests answer.

## Files

- `build_kit.sh`, `kit-README.txt`, `Run test.bat`: the kit.
- `nr_tester.cpp`, `probe.cpp`, `probe-shapes.txt`: the tester, the probe and the pipeline shapes of a real run.
- `diag.patch`: step trace (`NRVK_TRACE`), crash report and the `NRVK_NO_*` switches, for `run_frame`.
- `unroll_network.py`, `make_variants.py`: the unrolled shader set; one-define-removed variants of a pipeline.
- `micro/`, `micro2/`: the tiny shaders, one construct each.
