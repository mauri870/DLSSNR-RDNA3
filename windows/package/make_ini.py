#!/usr/bin/env python3
"""Default dlssnr-amd.ini files for the installer, written with the exact text the code writes (the format strings are
read out of windows/src/pe/nr_pe_config.cpp, so the two cannot drift): <out>/dlssnr-amd-optiscaler.ini ([Network],
[Preprocess], [RenoDX] and [Log], what the OptiScaler route reads), <out>/dlssnr-amd-optiscaler-renodx.ini (the same
with [RenoDX] Enabled = 1) and <out>/dlssnr-amd-addon.ini (the ReShade add-on's whole file). [Network] Model is
CC_SMALL_FP8 and ACO Mode 1; install.ps1 writes the network chosen at install.
usage: make_ini.py <nr_pe_config.cpp> <out dir>"""
import re, sys
from pathlib import Path

src = Path(sys.argv[1]).read_text(encoding="utf-8")
out = Path(sys.argv[2]); out.mkdir(parents=True, exist_ok=True)


def formats(body):
    """the string literals of every std::fprintf(f, "..." "...", ...) in body, joined and unescaped"""
    res = []
    for m in re.finditer(r'std::fprintf\(f,\s*((?:"(?:[^"\\]|\\.)*"\s*)+)', body):
        lits = re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(1))
        res.append("".join(lits).encode("utf-8").decode("unicode_escape").encode("latin-1").decode("utf-8"))
    return res


def body_of(sig):
    i = src.index(sig); j = src.index("\n}\n", i)
    return src[i:j]


# [Network] (nr_pe_config.hpp NetworkConfig defaults)
network = formats(body_of("void write_network("))[0] % ("CC_SMALL_FP8", "Ctrl+F11", 1, "auto", 1, 0, 1, "switch", 1)
prep = formats(body_of("void write_preprocess("))[0]
rdx = formats(body_of("void write_renodx("))[0]
logsec = formats(body_of("void write_log("))[0] % (1, 1)
save = formats(body_of("void Config::save("))
assert prep and len(save) == 8, len(save)   # [DlssNr], six Pass<N> lines (none by default), own keys

# defaults (nr_runtime.hpp Controls / Preprocess, nr_pe_config.hpp Config / PreprocessConfig)
preprocess = prep % (0, "auto", 0.0, "filmic", 1.0, 1.0, "Ctrl+F10", 1)
dlssnr = save[0] % ("true", "true", 1, "false", 1.0, 0, 1.0, 1.0, 1.0, -1.0, "true", 1.0, 1.0, 2.0, 0, "catmullrom")
own = save[-1] % (1.0, 1.0, "false", "vort")
# nr_renodx.hpp Settings: RenoDX's defaults
renodx_values = (1, 'F6', 0, 1.0, 0, 1.0, 1.0, -1.0, 0, 1, 1, 0, 1, 1.0, 1.0, 1.0, 1, 0, 1.0, 1.0, -1.0, 0, 1.0, 1.0, 1.0, 1, 0, 1.0, 1.0, -1.0, 0, 1.0, 1.0, 1.0, 1, 0, 1.0, 1.0, -1.0, 0, 1.0, 1.0, 1.0, 0, 0.0, 0, 1.0, 0, 0, 0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 0.0, 0.0, 0.0, 1.0, 1.0, 1.0, 0.0, 0.0, 0, 60.0, 0, 0, 0, 2, 1, 0, 0.0, 4.0, 2.5375, 203.0, 1.0, 0, 0, 0.0, 0, 2, 1, 4.0, 0.5, 0.02, 0, 0, 1.0, 1.0, 'F5', 0, 0, 0, 95, 10)
renodx = rdx % ((0,) + renodx_values)
renodx_on = rdx % ((1,) + renodx_values)

(out / "dlssnr-amd-optiscaler.ini").write_text(network + preprocess + renodx + logsec, encoding="utf-8")
(out / "dlssnr-amd-optiscaler-renodx.ini").write_text(network + preprocess + renodx_on + logsec, encoding="utf-8")
(out / "dlssnr-amd-addon.ini").write_text(dlssnr + own + "\n" + network + preprocess + renodx + logsec, encoding="utf-8")
print("wrote", out / "dlssnr-amd-optiscaler.ini", out / "dlssnr-amd-optiscaler-renodx.ini", out / "dlssnr-amd-addon.ini")
