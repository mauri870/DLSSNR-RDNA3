#!/usr/bin/env python3
"""Does the RDNA3 build still compute the small model's picture?  Run before and after any change to it.

    python3 linux/test/check_rdna3_small.py --model dlssnr-small.bin           # check against the golden
    python3 linux/test/check_rdna3_small.py --model dlssnr-small.bin --perf    # also the time per frame
    python3 linux/test/check_rdna3_small.py --model dlssnr-small.bin --update-golden

The small model is the 310.9.16 DLL's default (WEIGHTS_CC_SMALL_FP8: 31 blocks, 76 layers, the same layer
families and kernels as the 310.8.0 model). --model is the dlssnr-small.bin that
`linux/package/model-tools/extract_model.sh <nvngx_dlssnr.dll 310.9.16> <folder>` writes beside dlssnr.bin; it is
checked against the SHA-256 of the tested model first. The shaders are the default network's.

What it runs, on the first discrete GPU: the three single frames of docs/ngx-verification (1080p, 1440p, 4K)
through nr::Runtime, each twice, and every activation value of the 1080p network, hashed (nr_graph --value-stats).
Verdicts are check_rdna3.py's (EXACT, CLOSE, FAIL). The golden is linux/test/golden/rdna3/frames-small.json,
layers-small-1080.txt and the *_dlssnr-amd-rdna3.png files of docs/ngx-verification/310.9.16/single-frame-outputs.

The frames are scored against NVIDIA's own output of the 310.9.16 DLL (docs/ngx-verification/310.9.16).
"""
import argparse, hashlib, json, os, shutil, subprocess, sys
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_rdna3 as base
from check_rdna3 import BUILD, DOCS, FRAMES, GOLDEN, PNG_NAMES, ROOT, frame_inputs, host_defines, psnr, run, ssim

INST = BUILD / "inst-small"
DOCS_NEW = ROOT / "docs" / "ngx-verification" / "310.9.16"
MODEL_SHA256 = "7e966e2639dbec6596aa0bac3b8761137d2c40ca8cb4aca30df7ca10043c6e33"
PSNR_FLOOR_MARGIN, SSIM_FLOOR_MARGIN, CLOSE_TO_GOLDEN_DB = base.PSNR_FLOOR_MARGIN, base.SSIM_FLOOR_MARGIN, base.CLOSE_TO_GOLDEN_DB


def build(model):
    BUILD.mkdir(parents=True, exist_ok=True)
    digest = hashlib.sha256(model.read_bytes()).hexdigest()
    if digest != MODEL_SHA256:
        sys.exit(f"{model} is not the tested small model (SHA-256 {digest}, want {MODEL_SHA256}): make it with "
                 "extract_model.sh <nvngx_dlssnr.dll 310.9.16> <folder>")
    run([sys.executable, "linux/build/build_network.py", "rdna3"])
    cxx = ["g++", "-std=c++20", "-O1", "-w", "-Ilinux/src/core", "-Ilinux/test"] + host_defines()
    run(cxx + ["linux/test/run_frame.cpp", "linux/src/core/nr_runtime.cpp", "linux/src/core/nr_native_plan.cpp",
               "-o", str(BUILD / "run_frame_small"), "-lvulkan", "-lpthread"])
    run(cxx + ["linux/src/core/nr_graph.cpp", "-o", str(BUILD / "nr_graph_small"), "-lvulkan"])
    run(cxx + ["linux/test/mkplan.cpp", "linux/src/core/nr_native_plan.cpp", "-o", str(BUILD / "mkplan_small")])
    # the tree an installed small model has: the default network's shaders and dlssnr-small.bin
    data = INST / "dlssnr-amd"
    shutil.rmtree(INST, ignore_errors=True)
    data.mkdir(parents=True)
    shutil.copytree(ROOT / "build" / "linux" / "rdna3" / "network", data / "shaders")
    shutil.copyfile(model, data / "dlssnr-small.bin")


def run_frame(name, in_path, out_path, timed=0):
    w, h = FRAMES[name]
    cmd = [str(BUILD / "run_frame_small"), str(INST), str(in_path), str(w), str(h), str(out_path)]
    if timed:
        cmd.append(str(timed))
    out = run(cmd)
    if "small model" not in out:
        sys.exit("the runtime did not pick the small model:\n" + out)
    import re
    m = re.search(r"frame_ms ([0-9.]+)", out)
    return float(m.group(1)) if m else None


def check_frames(args, golden, record):
    verdicts, fresh, perf = {}, {}, {}
    for name in FRAMES:
        in_path, _ = frame_inputs(name)
        w, h = FRAMES[name]
        outs = []
        for i in range(2):
            o = BUILD / f"out_small_{name}_{i}.rgba8"
            ms = run_frame(name, in_path, o, timed=args.perf if (args.perf and i == 1) else 0)
            if ms is not None:
                perf[name] = ms
            outs.append(o.read_bytes())
        digest = hashlib.sha256(outs[0]).hexdigest()
        out = np.frombuffer(outs[0], np.uint8).reshape(h, w, 4)[..., :3]
        nv = np.array(Image.open(DOCS_NEW / "single-frame-outputs" / f"{PNG_NAMES[name]}_nvidia.png").convert("RGB"))
        scores = {"sha256": digest, "psnr_nvidia": round(psnr(out, nv), 2), "ssim_nvidia": round(ssim(out, nv), 5)}
        fresh[name] = scores
        line = f"{name:>6}: PSNR {scores['psnr_nvidia']:.2f} dB  SSIM {scores['ssim_nvidia']:.5f} vs NVIDIA"
        png = DOCS_NEW / "single-frame-outputs" / f"{PNG_NAMES[name]}_dlssnr-amd-rdna3.png"
        if outs[0] != outs[1]:
            verdicts[name] = "FAIL (two runs differ)"
        elif record:
            verdicts[name] = "recorded"
        elif name not in golden:
            verdicts[name] = "FAIL (no golden)"
        elif digest == golden[name]["sha256"]:
            verdicts[name] = "EXACT"
        else:
            db = psnr(out, np.array(Image.open(png).convert("RGB"))) if png.exists() else 0.0
            floor_ok = (scores["psnr_nvidia"] >= golden[name]["psnr_nvidia"] - PSNR_FLOOR_MARGIN and
                        scores["ssim_nvidia"] >= golden[name]["ssim_nvidia"] - SSIM_FLOOR_MARGIN)
            verdicts[name] = f"CLOSE ({db:.1f} dB from golden)" if db >= CLOSE_TO_GOLDEN_DB and floor_ok else \
                f"FAIL ({db:.1f} dB from golden, floors {'ok' if floor_ok else 'broken'})"
        print(f"{line}   {verdicts[name]}")
        if record:
            Image.fromarray(out).save(png)
    return verdicts, fresh, perf


def layer_hashes():
    w, h = FRAMES["1080p"]
    plan = BUILD / "plan_small_1080.txt"
    plan.write_text(run([str(BUILD / "mkplan_small"), str(w), str(h), "small"]))
    rgba = np.fromfile(BUILD / "in_1080p.rgba8", np.uint8).reshape(-1, 4).astype(np.float32) / 255
    blob = BUILD / "in_1080p.f32"
    rgba.tofile(blob)
    out = run([str(BUILD / "nr_graph_small"), "--plan", str(plan), "--model-pack", str(INST / "dlssnr-amd" / "dlssnr-small.bin"),
               "--spv-dir", str(INST / "dlssnr-amd" / "shaders"), "--host-boundary", "--no-reuse",
               "--source-width", str(w), "--source-height", str(h), "--in-image", str(blob), "--value-stats"])
    import re
    rows = []
    for line in out.splitlines():
        t = line.split()
        if len(t) > 8 and t[0].isdigit() and re.fullmatch(r"[0-9a-f]{1,16}", t[-1]):
            view = "twin" if "twin" in line else "arena"
            rows.append(f"{t[0]} {view} {t[2]} {t[-1]}")
    return rows


def check_layers(golden_rows, record):
    rows = layer_hashes()
    if record:
        return rows, None
    if not golden_rows:
        return rows, "no golden layer hashes"
    gold = {tuple(r.split()[:2]): r.split() for r in golden_rows}
    changed = [r.split() for r in rows if gold.get(tuple(r.split()[:2]), [None] * 4)[3] != r.split()[3]]
    if not changed:
        return rows, None
    first = changed[0]
    return rows, f"{len(changed)} of {len(rows)} values changed; first: key {first[0]} ({first[1]}) produced by {first[2]}"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--model", required=True, help="dlssnr-small.bin")
    ap.add_argument("--update-golden", action="store_true")
    ap.add_argument("--perf", type=int, nargs="?", const=20, default=0, help="also time N passes per frame (default 20)")
    ap.add_argument("--skip-build", action="store_true")
    args = ap.parse_args()
    os.chdir(ROOT)
    if not args.skip_build:
        build(Path(args.model).resolve())
    golden_file = GOLDEN / "frames-small.json"
    golden = json.loads(golden_file.read_text()) if golden_file.exists() and not args.update_golden else {}
    layers_file = GOLDEN / "layers-small-1080.txt"
    golden_rows = layers_file.read_text().split("\n") if layers_file.exists() and not args.update_golden else []
    verdicts, fresh, perf = check_frames(args, golden, args.update_golden)
    rows, layer_note = check_layers([r for r in golden_rows if r], args.update_golden)
    if args.update_golden:
        GOLDEN.mkdir(parents=True, exist_ok=True)
        golden_file.write_text(json.dumps(fresh, indent=1) + "\n")
        layers_file.write_text("\n".join(rows) + "\n")
        print(f"golden recorded in {GOLDEN} ({len(rows)} layer values)")
    else:
        print("layers: " + ("identical" if layer_note is None else layer_note))
    if perf:
        print("time per frame (network, wall): " + "  ".join(f"{k} {v:.1f} ms" for k, v in perf.items()))
    if args.update_golden:
        return 0
    bad = [v for v in verdicts.values() if v.startswith("FAIL")]
    close = [v for v in verdicts.values() if v.startswith("CLOSE")]
    if layer_note and not close:
        bad.append("FAIL (layers)")
    if bad or close:
        print("RESULT: FAIL" + ("  (a close picture is a new golden: --update-golden, with the reason in the commit)" if close and not bad else ""))
        return 1
    print("RESULT: PASS (exact)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
