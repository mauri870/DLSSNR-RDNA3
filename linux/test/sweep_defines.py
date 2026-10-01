#!/usr/bin/env python3
"""Rebuild some pipelines of the RDNA3 network with other defines and time them, without the full check.

    python3 linux/test/sweep_defines.py --model dlssnr.bin fswinpds64,fswinpup64 NR_EXPAND_KLOOP=[[dont_unroll]]
    python3 linux/test/sweep_defines.py --model dlssnr.bin fswin32nh NR_FWAVES=4 --resolution 1080p

The named pipelines of pipelines.json are compiled with the extra defines (an extra define replaces the
table's define of the same name), copied over a copy of the built network, and nr_graph --per-layer prints
the time of those kernels at one resolution next to the unchanged build, run back to back. The picture is
not checked: a change that is worth keeping goes through check_rdna3.py. check_rdna3.py must have been run
once (it builds nr_graph, the plan and the input). Run it with the GPU otherwise idle.
"""
import argparse
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / "build" / "test"
NETWORK = ROOT / "build" / "linux" / "rdna3" / "network"
SWEEP = BUILD / "sweep" / "network"
PLANS = {"1080p": "plan_1080p.txt", "1440p": "plan_1440p.txt", "4k": "plan_4k.txt"}
EXTENT = {"1080p": (1920, 1080), "1440p": (2560, 1440), "4k": (3840, 2160)}


def per_layer(model, resolution, spv_dir):
    w, h = EXTENT[resolution]
    plan = BUILD / PLANS[resolution]
    if not plan.exists():
        plan.write_text(subprocess.run([str(BUILD / "mkplan"), str(w), str(h)], capture_output=True, text=True, check=True).stdout)
    blob = BUILD / f"in_{resolution}.f32"
    if not blob.exists():
        sys.exit(f"{blob} is missing: run check_rdna3.py --profile {resolution} once first")
    r = subprocess.run([str(BUILD / "nr_graph"), "--plan", str(plan), "--model-pack", str(model), "--spv-dir", str(spv_dir),
                        "--host-boundary", "--reuse", "--source-width", str(w), "--source-height", str(h),
                        "--in-image", str(blob), "--warmup", "3", "--repeats", "10", "--per-layer"],
                       capture_output=True, text=True, cwd=ROOT)
    times = {}
    for line in r.stdout.splitlines():
        m = re.match(r"(\S+)/(\S+)\s+\d+\s+(\d+)\s+([0-9.]+)\s+([0-9.]+)\s+[0-9.]+%", line)
        if m:
            times[m.group(2)] = (int(m.group(3)), float(m.group(4)))
    return times


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--model", required=True)
    ap.add_argument("--resolution", default="4k", choices=list(PLANS))
    ap.add_argument("pipelines", help="comma-separated names from pipelines.json")
    ap.add_argument("defines", nargs="*", help="NAME=value, or NAME for a bare define")
    args = ap.parse_args()
    table = json.loads((ROOT / "linux/shaders/rdna3/pipelines.json").read_text())["pipelines"]
    shutil.rmtree(SWEEP, ignore_errors=True)
    shutil.copytree(NETWORK, SWEEP)
    override = {d.split("=")[0] for d in args.defines}
    for name in args.pipelines.split(","):
        entry = table[name]
        defines = [d for d in entry["defines"] if d.split("=")[0] not in override] + args.defines
        r = subprocess.run([str(ROOT / "toolchain/glslang/bin/glslang"), "-V", "--target-env", "vulkan1.3",
                            "-I" + str(ROOT / "linux/shaders/rdna3/include")] + ["-D" + d for d in defines] +
                           [str(ROOT / "linux/shaders/rdna3" / entry["source"]), "-o", str(SWEEP / f"g_{name}.spv")],
                           capture_output=True, text=True)
        if r.returncode:
            sys.exit(r.stdout + r.stderr)
    model = Path(args.model).resolve()
    for label, directory in (("baseline", NETWORK), ("changed", SWEEP), ("baseline", NETWORK), ("changed", SWEEP)):
        times = per_layer(model, args.resolution, directory)
        row = "  ".join(f"{k} {times[k][1]:.0f} us x{times[k][0]}" for k in args.pipelines.split(",") if k in times)
        print(f"{label:>8}: {row}")


if __name__ == "__main__":
    main()
