#!/usr/bin/env python3
"""Does the RDNA3 build still compute the right picture?  Run before and after any kernel change.

    python3 linux/test/check_rdna3.py --model dlssnr.bin             # check against the golden
    python3 linux/test/check_rdna3.py --model dlssnr.bin --perf      # also the time per frame
    python3 linux/test/check_rdna3.py --model dlssnr.bin --update-golden   # record the current state

What it runs, on the first discrete GPU:
  1. the e4m3 quantiser and decoder of linux/shaders/rdna3 against the host's reference, exhaustively;
  2. the three single frames of docs/ngx-verification (1080p, 1440p, 4K) through nr::Runtime, each twice
     (a result that changes from run to run is a race, not a result);
  3. every activation value of the 1080p network, hashed (`nr_graph --value-stats`), which names the first
     layer to change when a picture does.

How a frame is judged:
  EXACT  the output is bit-identical to the golden. Required of any change that does not alter
         arithmetic: occupancy, scheduling, tile shape, memory layout, barrier placement.
  CLOSE  not identical, but within 60 dB of the golden and above the floors against NVIDIA's own output
         (PSNR 0.15 dB and SSIM 0.0005 under the golden's). The most a change may do that reorders a sum.
         Accepted only with --tolerance close, and the changed layers are listed so it is a decision.
  FAIL   anything else, and any frame that differs between two runs.

The golden is linux/test/golden/rdna3/: output hashes and scores (frames.json) and the per-layer hashes
of the 1080p network (layers-1080.txt). The golden pixels are the *_dlssnr-amd-rdna3.png files of
docs/ngx-verification. NVIDIA's weights are not in the repository: --model is a dlssnr.bin made by
linux/package/model-tools/extract_model.sh from your own nvngx_dlssnr.dll.

Needs g++, glslang, the Vulkan headers and loader, python3 with numpy and PIL.
"""
import argparse, hashlib, json, os, re, shutil, subprocess, sys
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / "build" / "test"
GOLDEN = ROOT / "linux" / "test" / "golden" / "rdna3"
DOCS = ROOT / "docs" / "ngx-verification"
FRAMES = {"1080p": (1920, 1080), "1440p": (2560, 1440), "4k": (3840, 2160)}
PNG_NAMES = {"1080p": "1920x1080", "1440p": "2560x1440", "4k": "3840x2160"}
PSNR_FLOOR_MARGIN, SSIM_FLOOR_MARGIN, CLOSE_TO_GOLDEN_DB = 0.15, 0.0005, 60.0


def run(cmd, **kw):
    r = subprocess.run(cmd, cwd=kw.pop("cwd", ROOT), capture_output=True, text=True, **kw)
    if r.returncode:
        sys.exit(f"failed: {' '.join(map(str, cmd))}\n{r.stdout}{r.stderr}")
    return r.stdout


def host_defines():
    out = run(["bash", "-c", 'source linux/build/arch/rdna3.sh; printf "%s\\n" "${NR_PRODUCT_DEFINES[@]}"'])
    return out.split()


# ---- picture metrics (the same as docs/ngx-verification/NGX-VERIFICATION.md) --------------------
def _blur(z, sigma=1.5):
    r = int(3.5 * sigma + 0.5)
    k = np.exp(-0.5 * (np.arange(-r, r + 1) / sigma) ** 2)
    k /= k.sum()
    for axis in (0, 1):
        pad = [(r, r) if i == axis else (0, 0) for i in range(2)]
        zp = np.pad(z, pad, mode="reflect")
        z = sum(k[i] * np.take(zp, range(i, i + z.shape[axis]), axis=axis) for i in range(2 * r + 1))
    return z


def ssim(a, b):
    a, b, out = a.astype(np.float64), b.astype(np.float64), []
    c1, c2 = (0.01 * 255) ** 2, (0.03 * 255) ** 2
    for c in range(3):
        x, y = a[..., c], b[..., c]
        mx, my = _blur(x), _blur(y)
        sxx, syy, sxy = _blur(x * x) - mx * mx, _blur(y * y) - my * my, _blur(x * y) - mx * my
        out.append((((2 * mx * my + c1) * (2 * sxy + c2)) / ((mx * mx + my * my + c1) * (sxx + syy + c2))).mean())
    return float(np.mean(out))


def psnr(a, b):
    m = ((a.astype(np.float64) - b.astype(np.float64)) ** 2).mean()
    return float("inf") if m == 0 else 10 * np.log10(255.0 ** 2 / m)


# ---- build ---------------------------------------------------------------------------------------
def build(model):
    BUILD.mkdir(parents=True, exist_ok=True)
    glslang = ROOT / "toolchain" / "glslang" / "bin" / "glslang"
    if not glslang.exists():
        found = shutil.which("glslang")
        if not found:
            sys.exit("no glslang: run fetch_deps.sh, or put glslang on PATH")
        glslang.parent.mkdir(parents=True, exist_ok=True)
        glslang.symlink_to(found)
        print(f"toolchain/glslang/bin/glslang -> {found} (fetch_deps.sh pins 16.5.0; this one may differ)")
    run([sys.executable, "linux/build/build_network.py", "rdna3"])
    defines = host_defines()
    inc = ["-Ilinux/src/core", "-Ilinux/test"]
    cxx = ["g++", "-std=c++20", "-O1", "-w"] + inc + defines
    run(cxx + ["linux/test/run_frame.cpp", "linux/src/core/nr_runtime.cpp", "linux/src/core/nr_native_plan.cpp",
               "-o", str(BUILD / "run_frame"), "-lvulkan", "-lpthread"])
    run(cxx + ["linux/test/mkplan.cpp", "linux/src/core/nr_native_plan.cpp", "-o", str(BUILD / "mkplan")])
    run(cxx + ["linux/src/core/nr_graph.cpp", "-o", str(BUILD / "nr_graph"), "-lvulkan"])
    run(cxx + ["linux/test/e4m3_emul_test.cpp", "-o", str(BUILD / "e4m3_emul_test"), "-lvulkan"])
    run([str(glslang), "-V", "--target-env", "vulkan1.3", "-Ilinux/shaders/rdna3/include",
         "linux/test/e4m3_emul_test.comp", "-o", str(BUILD / "e4m3_emul_test.spv")])
    inst = BUILD / "inst" / "dlssnr-amd"
    shutil.rmtree(inst, ignore_errors=True)
    inst.mkdir(parents=True)
    shutil.copytree(ROOT / "build" / "linux" / "rdna3" / "network", inst / "shaders")
    shutil.copyfile(model, inst / "dlssnr.bin")


# ---- the checks ----------------------------------------------------------------------------------
def check_quantiser():
    r = subprocess.run([str(BUILD / "e4m3_emul_test"), str(BUILD / "e4m3_emul_test.spv")], capture_output=True, text=True)
    print(r.stdout.strip() + ("" if r.returncode == 0 else "   FAIL"))
    return r.returncode == 0


def frame_inputs(name):
    w, h = FRAMES[name]
    png = DOCS / "single-frame-inputs" / f"{PNG_NAMES[name]}.png"
    rgba = np.array(Image.open(png).convert("RGBA"))
    assert rgba.shape == (h, w, 4)
    path = BUILD / f"in_{name}.rgba8"
    rgba.tofile(path)
    return path, rgba[..., :3]


def run_frame(name, in_path, out_path, timed=0):
    w, h = FRAMES[name]
    cmd = [str(BUILD / "run_frame"), str(BUILD / "inst"), str(in_path), str(w), str(h), str(out_path)]
    if timed:
        cmd.append(str(timed))
    out = run(cmd)
    m = re.search(r"frame_ms ([0-9.]+)", out)
    return float(m.group(1)) if m else None


def check_frames(args, golden, record):
    verdicts, fresh = {}, {}
    perf = {}
    for name in FRAMES:
        in_path, _ = frame_inputs(name)
        w, h = FRAMES[name]
        outs = []
        for i in range(2):
            o = BUILD / f"out_{name}_{i}.rgba8"
            ms = run_frame(name, in_path, o, timed=args.perf if (args.perf and i == 1) else 0)
            if ms is not None:
                perf[name] = ms
            outs.append(o.read_bytes())
        digest = hashlib.sha256(outs[0]).hexdigest()
        out = np.frombuffer(outs[0], np.uint8).reshape(h, w, 4)[..., :3]
        nv = np.array(Image.open(DOCS / "single-frame-outputs" / f"{PNG_NAMES[name]}_nvidia.png").convert("RGB"))
        scores = {"sha256": digest, "psnr_nvidia": round(psnr(out, nv), 2), "ssim_nvidia": round(ssim(out, nv), 5)}
        fresh[name] = scores
        line = f"{name:>6}: PSNR {scores['psnr_nvidia']:.2f} dB  SSIM {scores['ssim_nvidia']:.5f} vs NVIDIA"
        if outs[0] != outs[1]:
            verdicts[name] = "FAIL (two runs differ)"
        elif record:
            verdicts[name] = "recorded"
        elif name not in golden:
            verdicts[name] = "FAIL (no golden)"
        elif digest == golden[name]["sha256"]:
            verdicts[name] = "EXACT"
        else:
            ref = np.array(Image.open(DOCS / "single-frame-outputs" / f"{PNG_NAMES[name]}_dlssnr-amd-rdna3.png").convert("RGB"))
            db = psnr(out, ref)
            floor_ok = (scores["psnr_nvidia"] >= golden[name]["psnr_nvidia"] - PSNR_FLOOR_MARGIN and
                        scores["ssim_nvidia"] >= golden[name]["ssim_nvidia"] - SSIM_FLOOR_MARGIN)
            verdicts[name] = f"CLOSE ({db:.1f} dB from golden)" if db >= CLOSE_TO_GOLDEN_DB and floor_ok else \
                f"FAIL ({db:.1f} dB from golden, floors {'ok' if floor_ok else 'broken'})"
        print(f"{line}   {verdicts[name]}")
        if record:
            Image.fromarray(out).save(DOCS / "single-frame-outputs" / f"{PNG_NAMES[name]}_dlssnr-amd-rdna3.png")
    return verdicts, fresh, perf


def layer_hashes():
    w, h = FRAMES["1080p"]
    plan = BUILD / "plan_1080.txt"
    plan.write_text(run([str(BUILD / "mkplan"), str(w), str(h)]))
    rgba = np.fromfile(BUILD / "in_1080p.rgba8", np.uint8).reshape(-1, 4).astype(np.float32) / 255
    blob = BUILD / "in_1080p.f32"
    rgba.tofile(blob)
    out = run([str(BUILD / "nr_graph"), "--plan", str(plan), "--model-pack", str(BUILD / "inst/dlssnr-amd/dlssnr.bin"),
               "--spv-dir", str(ROOT / "build/linux/rdna3/network"), "--host-boundary", "--no-reuse",
               "--source-width", str(w), "--source-height", str(h), "--in-image", str(blob), "--value-stats"])
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
    ap.add_argument("--tolerance", choices=["exact", "close"], default="exact")
    ap.add_argument("--skip-build", action="store_true")
    args = ap.parse_args()
    os.chdir(ROOT)
    if not args.skip_build:
        build(Path(args.model).resolve())
    quantiser_ok = check_quantiser()
    golden_file = GOLDEN / "frames.json"
    golden = json.loads(golden_file.read_text()) if golden_file.exists() and not args.update_golden else {}
    layers_file = GOLDEN / "layers-1080.txt"
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
    bad = [v for v in verdicts.values() if v.startswith("FAIL")] + ([] if quantiser_ok else ["FAIL (quantiser)"])
    close = [v for v in verdicts.values() if v.startswith("CLOSE")]
    if bad or (close and args.tolerance == "exact"):
        print("RESULT: FAIL" + ("  (rerun with --tolerance close to accept a reordered sum)" if close and not bad else ""))
        return 1
    print("RESULT: PASS" + (" (close)" if close else " (exact)"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
