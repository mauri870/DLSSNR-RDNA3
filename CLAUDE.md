# Working on DLSSNR-AMD

Notes for contributors and coding assistants. README.md covers what the project is and how to build it.

## Layout rules

- `linux/` is the reference implementation. `windows/` is a separate copy for the AMD Windows driver
  (its shader compiler, LLPC) and is changed on its own. The two trees share no files on purpose.
- A workaround for the Windows driver goes into `windows/` only. A change goes into `linux/` only if
  it helps Linux. When a fix applies to both, make it in both, deliberately.
- Build scripts `cd` to the repository root themselves; run them from anywhere. Everything under
  `toolchain/`, `artifacts/` and `build/` is downloaded or generated and never committed.

## Things that must stay true

- **No NVIDIA files.** Never commit `nvngx_dlssnr.dll`, the weights, `dlssnr.bin`, tensors extracted
  from the DLL, or packages that contain them. The model reaches users only through
  `linux/package/model-tools/extract_model.sh` run on their own DLL. CI checks that packages carry no model.
- **Host and shaders agree.** The C++ host is compiled with the defines in `*/build/arch/rdna4.sh`,
  the shaders with those in `*/shaders/rdna4/pipelines.json`; `shader-constants.txt` ties them together
  and the runtime refuses a shader folder that disagrees. Change both sides together.
- **Precision floor.** Keep at least the original network's precision at every operation: FP8 only
  where the original uses FP8, FP16/FP32 elsewhere. No new lower-precision boundary, no truncated
  inputs or lookup tables. Changing the order of a reduction changes the picture too - it is not a free
  reordering.
- **Weight packing.** A new kernel that reads weights must be added to the host's weight packing
  (`nr_graph.cpp`); otherwise it runs on unpacked data and its results are meaningless.
- **Reproducible shaders.** `fetch_deps.sh` pins glslang 16.5.0; with it `build_network.py` produces the
  same SPIR-V bytes every time. After a change that should not alter the arithmetic (refactor,
  comments, scheduling of independent work), compare the `.spv` files before and after.

## RDNA3

- `linux/shaders/rdna3/` is the RDNA3 network (e4m3 values held as FP16, see README-RDNA3.md); `rdna4/` is not
  touched by it. Build scripts take `NR_GPU=rdna3`; the host is gated by `NR_ARCH_RDNA3`.
- Run `python3 linux/test/check_rdna3.py --model dlssnr.bin` before and after any change to it. A change that
  leaves the arithmetic alone must be EXACT; a change that does not is a new golden and needs a reason in the
  commit. Time kernels with the GPU idle.
- A raw 32-bit view of an arena (counters, sync words, scale tables) goes at alias binding 16 plus the arena's
  binding, never next to an e4m3 view: the e4m3 bindings read the FP16 twin buffer.

## Performance work on RDNA4 (RADV / ACO)

Measured facts worth knowing before changing kernels:

- FP8 WMMA and VALU instructions do not overlap on RDNA4, not even across waves. Frame time is roughly
  WMMA time + VALU time + load time + idle time; saving VALU instructions saves time directly.
- ACO turns per-tile guards and uniform-looking branches it cannot prove uniform into phi copies and
  extra VGPRs; a "cheap" skip can cost occupancy. Count VGPRs and waves in the ISA
  (`RADV_DEBUG=shaders`) before and after.
- Wave arbitration is oldest-first: in persistent kernels, the second workgroup on a CU runs much slower
  than the first.
- Keep every LDS `coopMatLoad` stride and offset 16-byte aligned: the Windows compiler reads fragments
  with 8-byte-aligned paired loads and otherwise reads unwritten padding.
- Measure frame time over many frames and several runs at the same clocks (the log's per-frame GPU
  time); a single fastest run is not a result.

## Versions and releases

- The version comes from git tags only (`*/build/version.sh`): tag `v0.0.1` builds as `0.0.1`, later
  commits as `0.0.1-3-g1a2b3c4`. Package names and the DLLs' log lines carry it.
- Releasing: `git tag v0.0.2 && git push origin v0.0.2`. CI (.github/workflows/packages.yml) builds
  the Linux packages and the Windows package and publishes a GitHub release. For a Linux-only release,
  set the repository variable `WINDOWS_PACKAGE` to `off` first (`gh variable set WINDOWS_PACKAGE --body off`).

## Style

- Comments and user-facing text in English. Comments explain why (the measurement, the trap), not
  what the next line does.
- No internal version tags or references to files that are not in this repository.
- Package text (`*/package/README.txt`, installer messages) stays short and plain.
