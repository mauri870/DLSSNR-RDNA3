#!/usr/bin/env python3
"""Does the RDNA3 int4 mixed network still compute the right picture?  Run before and after any change to it.

    python3 linux/test/check_rdna3_int4.py --model dlssnr.bin             # build, then check against the golden
    python3 linux/test/check_rdna3_int4.py --model dlssnr.bin --perf      # also the time per frame, int4 and default
    python3 linux/test/check_rdna3_int4.py --model dlssnr.bin --update-golden

What it runs, on the first discrete GPU (the default network's checks are linux/test/check_rdna3.py):
  1. the install-time weights generator on the model: the file it writes has to be the one linux/data/int4/<tables>/
     dlssnr-int4.sha256 names (the tables are calibrated on the FP8 network, which this build reproduces);
  2. the three single frames of docs/ngx-verification (1080p, 1440p, 4K) through nr::Runtime with int4 mixed, each
     twice, judged as check_rdna3.py judges them: EXACT against the golden, or FAIL. int4 mixed is not the default
     network's picture on purpose (score 41.9 to 43.6 dB against NVIDIA's outputs, 45.5 to 49.0 for the default);
  3. every activation value of the 1080p network, hashed (nr_graph --value-stats), which names the first layer to
     change when a picture does;
  4. that every int4 pipeline of the network builds and its WMMAs are rewritten (a pipeline is made when a frame
     first needs it, so the frames alone leave the ones the persistent kernels replace untested).

The golden is linux/test/golden/rdna3/frames-int4.json and layers-int4-1080.txt; the golden pixels are the
*_dlssnr-amd-rdna3-int4.png files of docs/ngx-verification.
"""
import argparse, hashlib, json, os, re, shutil, subprocess, sys
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_rdna3 as base
from check_rdna3 import BUILD, DOCS, FRAMES, GOLDEN, PNG_NAMES, ROOT, frame_inputs, host_defines, psnr, run, ssim

INT4 = BUILD / "int4"
INST = BUILD / "inst-int4"
PSNR_FLOOR_MARGIN, SSIM_FLOOR_MARGIN, CLOSE_TO_GOLDEN_DB = base.PSNR_FLOOR_MARGIN, base.SSIM_FLOOR_MARGIN, base.CLOSE_TO_GOLDEN_DB


def build(model):
    BUILD.mkdir(parents=True, exist_ok=True)
    if not (ROOT / "build" / "linux" / "rdna3" / "network").is_dir():
        run([sys.executable, "linux/build/build_network.py", "rdna3"])
    env = dict(os.environ, NR_GPU="rdna3")
    r = subprocess.run(["bash", "linux/build/build_int4.sh", str(INT4)], cwd=ROOT, capture_output=True, text=True, env=env)
    if r.returncode:
        sys.exit(f"build_int4.sh failed:\n{r.stdout}{r.stderr}")
    cxx = ["g++", "-std=c++20", "-O1", "-w", "-DNR_INT4=1", "-Ilinux/src/core", "-Ilinux/test"] + host_defines()
    run(cxx + ["linux/test/run_frame.cpp", "linux/src/core/nr_runtime.cpp", "linux/src/core/nr_native_plan.cpp",
               "-o", str(BUILD / "run_frame_int4"), "-lvulkan", "-lpthread"])
    run(cxx + ["linux/src/core/nr_graph.cpp", "-o", str(BUILD / "nr_graph_int4"), "-lvulkan"])
    run(cxx + ["linux/test/mkplan.cpp", "linux/src/core/nr_native_plan.cpp", "-o", str(BUILD / "mkplan")])
    run(cxx + ["linux/test/int4_pipelines.cpp", "-o", str(BUILD / "int4_pipelines"), "-lvulkan"])
    # the tree an installed int4 mixed has: the default network, and dlssnr-amd/int4 beside the weights file
    data = INST / "dlssnr-amd"
    shutil.rmtree(INST, ignore_errors=True)
    data.mkdir(parents=True)
    shutil.copytree(ROOT / "build" / "linux" / "rdna3" / "network", data / "shaders")
    shutil.copytree(INT4 / "data", data / "int4")
    shutil.copyfile(model, data / "dlssnr.bin")
    log = run([str(INT4 / "dlssnr-int4-weights"), str(data / "dlssnr.bin"), str(data / "int4"), str(data / "dlssnr-int4.bin")])
    return log


def tables_name():
    return run(["bash", "-c", 'source linux/build/arch/rdna3.sh; echo $NR_INT4_DATA']).strip()


def check_weights():
    want = (ROOT / "linux" / "data" / "int4" / tables_name() / "dlssnr-int4.sha256").read_text().split()[0]
    got = hashlib.sha256((INST / "dlssnr-amd" / "dlssnr-int4.bin").read_bytes()).hexdigest()
    print(f"weights: {'identical to the shipped SHA-256' if got == want else 'FAIL (' + got[:16] + ' against ' + want[:16] + ')'}")
    return got == want


def check_pipelines():
    r = subprocess.run([str(BUILD / "int4_pipelines"), str(INST / "dlssnr-amd" / "int4" / "shaders")], capture_output=True, text=True)
    last = r.stdout.strip().splitlines()[-1] if r.stdout.strip() else r.stderr.strip()
    print(f"pipelines: {last}" + ("" if r.returncode == 0 else "   FAIL"))
    if r.returncode:
        print(r.stdout + r.stderr)
    return r.returncode == 0


def run_frame(name, in_path, out_path, timed=0, int4=True):
    w, h = FRAMES[name]
    cmd = [str(BUILD / "run_frame_int4"), str(INST), str(in_path), str(w), str(h), str(out_path)]
    if timed:
        cmd.append(str(timed))
    out = run(cmd, env=dict(os.environ, DLSSNR_INT4="1" if int4 else "0"))
    m = re.search(r"frame_ms ([0-9.]+)", out)
    return float(m.group(1)) if m else None


def check_frames(args, golden, record):
    verdicts, fresh, perf, perf_default = {}, {}, {}, {}
    for name in FRAMES:
        in_path, _ = frame_inputs(name)
        w, h = FRAMES[name]
        outs = []
        for i in range(2):
            o = BUILD / f"out_int4_{name}_{i}.rgba8"
            ms = run_frame(name, in_path, o, timed=args.perf if (args.perf and i == 1) else 0)
            if ms is not None:
                perf[name] = ms
            outs.append(o.read_bytes())
        if args.perf:
            perf_default[name] = run_frame(name, in_path, BUILD / f"out_int4_default_{name}.rgba8", timed=args.perf, int4=False)
        digest = hashlib.sha256(outs[0]).hexdigest()
        out = np.frombuffer(outs[0], np.uint8).reshape(h, w, 4)[..., :3]
        nv = np.array(Image.open(DOCS / "single-frame-outputs" / f"{PNG_NAMES[name]}_nvidia.png").convert("RGB"))
        scores = {"sha256": digest, "psnr_nvidia": round(psnr(out, nv), 2), "ssim_nvidia": round(ssim(out, nv), 5)}
        fresh[name] = scores
        line = f"{name:>6}: PSNR {scores['psnr_nvidia']:.2f} dB  SSIM {scores['ssim_nvidia']:.5f} vs NVIDIA"
        png = DOCS / "single-frame-outputs" / f"{PNG_NAMES[name]}_dlssnr-amd-rdna3-int4.png"
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
    return verdicts, fresh, perf, perf_default


def layer_hashes():
    w, h = FRAMES["1080p"]
    plan = BUILD / "plan_1080.txt"
    plan.write_text(run([str(BUILD / "mkplan"), str(w), str(h)]))
    rgba = np.fromfile(BUILD / "in_1080p.rgba8", np.uint8).reshape(-1, 4).astype(np.float32) / 255
    blob = BUILD / "in_1080p.f32"
    rgba.tofile(blob)
    data = INST / "dlssnr-amd" / "int4"
    env = dict(os.environ, NR_I4_DIR=str(data / "vit"), NR_SWI4_DIR=str(data / "swin"),
               NR_I4_WEIGHTS=str(INST / "dlssnr-amd" / "dlssnr-int4.bin"))
    for line in (data / "settings.txt").read_text().splitlines():
        if line and not line.startswith("#") and "=" in line:
            k, v = line.split("=", 1)
            env[k] = v
    out = run([str(BUILD / "nr_graph_int4"), "--plan", str(plan), "--model-pack", str(INST / "dlssnr-amd" / "dlssnr.bin"),
               "--spv-dir", str(data / "shaders"), "--host-boundary", "--no-reuse",
               "--source-width", str(w), "--source-height", str(h), "--in-image", str(blob), "--value-stats"], env=env)
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
    ap.add_argument("--model", required=True, help="dlssnr.bin")
    ap.add_argument("--update-golden", action="store_true")
    ap.add_argument("--perf", type=int, nargs="?", const=20, default=0, help="also time N passes per frame (default 20)")
    ap.add_argument("--skip-build", action="store_true")
    args = ap.parse_args()
    os.chdir(ROOT)
    if not args.skip_build:
        build(Path(args.model).resolve())
    weights_ok = check_weights()
    pipelines_ok = check_pipelines()
    golden_file = GOLDEN / "frames-int4.json"
    golden = json.loads(golden_file.read_text()) if golden_file.exists() and not args.update_golden else {}
    layers_file = GOLDEN / "layers-int4-1080.txt"
    golden_rows = layers_file.read_text().split("\n") if layers_file.exists() and not args.update_golden else []
    verdicts, fresh, perf, perf_default = check_frames(args, golden, args.update_golden)
    rows, layer_note = check_layers([r for r in golden_rows if r], args.update_golden)
    if args.update_golden:
        GOLDEN.mkdir(parents=True, exist_ok=True)
        golden_file.write_text(json.dumps(fresh, indent=1) + "\n")
        layers_file.write_text("\n".join(rows) + "\n")
        print(f"golden recorded in {GOLDEN} ({len(rows)} layer values)")
    else:
        print("layers: " + ("identical" if layer_note is None else layer_note))
    if perf:
        print("time per frame (network, wall), int4: " + "  ".join(f"{k} {v:.1f} ms" for k, v in perf.items()))
        print("                                default: " + "  ".join(f"{k} {v:.1f} ms" for k, v in perf_default.items()))
    if args.update_golden:
        return 0
    bad = [v for v in verdicts.values() if v.startswith("FAIL")] + ([] if weights_ok else ["FAIL (weights)"]) + ([] if pipelines_ok else ["FAIL (pipelines)"])
    close = [v for v in verdicts.values() if v.startswith("CLOSE")]
    if layer_note:
        bad.append("FAIL (layers)") if not close else None
    if bad or close:
        print("RESULT: FAIL" + ("  (a close picture is a new golden: --update-golden, with the reason in the commit)" if close and not bad else ""))
        return 1
    print("RESULT: PASS (exact)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
