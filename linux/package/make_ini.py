#!/usr/bin/env python3
"""Default dlssnr-amd.ini files for the installer, written with the exact text and the default values the code has:
the format strings are read out of src/pe/nr_pe_config.cpp and every value out of the structs' member defaults
(nr_pe_config.hpp, core/nr_runtime.hpp, core/nr_renodx.hpp), so neither can drift from what the programs write.
<out>/dlssnr-amd-optiscaler.ini ([Network], [Preprocess], [RenoDX], [wilsjo2], [Log] and [Reuse], what the OptiScaler route
reads), <out>/dlssnr-amd-optiscaler-renodx.ini (the same with [RenoDX] Enabled = 1) and <out>/dlssnr-amd-addon.ini (the
ReShade add-on's whole file). [Network] Model is the default network; install.sh writes the one chosen at install.
usage: make_ini.py <nr_pe_config.cpp> <out dir>"""
import re, sys
from pathlib import Path

cfg_cpp = Path(sys.argv[1])
src = cfg_cpp.read_text(encoding="utf-8")
out = Path(sys.argv[2]); out.mkdir(parents=True, exist_ok=True)
srcdir = cfg_cpp.parent.parent   # linux/src
headers = "\n".join((srcdir / p).read_text(encoding="utf-8") for p in
                    ("pe/nr_pe_config.hpp", "core/nr_runtime.hpp", "core/nr_renodx.hpp", "core/nr_ini_network.hpp"))


def body_of(sig):
    i = src.index(sig); j = src.index("\n}\n", i)
    return src[i:j]


def formats(body):
    """(format text, argument expressions) of every std::fprintf(f, "..." "...", ...) in body"""
    res = []
    for m in re.finditer(r'std::fprintf\(f,\s*((?:"(?:[^"\\]|\\.)*"\s*)+)(.*?)\);', body, re.S):
        lits = re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(1))
        text = "".join(lits).encode("utf-8").decode("unicode_escape").encode("latin-1").decode("utf-8")
        res.append((text, split_args(m.group(2).strip().lstrip(",").strip())))
    return res


def split_args(s):
    args, depth, cur, quote = [], 0, "", False
    for ch in s:
        if ch == '"': quote = not quote
        if not quote and ch in "([{": depth += 1
        if not quote and ch in ")]}": depth -= 1
        if ch == "," and depth == 0 and not quote: args.append(cur.strip()); cur = ""
        else: cur += ch
    return args + [cur.strip()] if cur.strip() else args


# ---- member defaults of the structs ------------------------------------------------------------------------------------
constants = {m.group(1): int(m.group(2)) for m in re.finditer(r"\b(k\w+)\s*=\s*(-?\d+)\b", headers)}


def literal(v):
    v = v.strip()
    if v in ("true", "false"): return v == "true"
    if v.startswith('"'): return v[1:-1]
    if v.startswith("{"): return [literal(x) for x in split_args(v[1:-1])]
    v = re.sub(r"^(?:nr::)+", "", v)
    if v in constants: return constants[v]
    m = re.fullmatch(r"(-?[\d.]+(?:e-?\d+)?)[fFuU]?", v)
    if not m: raise ValueError("member default not understood: " + v)
    return float(m.group(1)) if any(c in m.group(1) for c in ".e") else int(m.group(1))


def struct(name):
    """the members of `struct name { ... }` (the first one in the headers) with their defaults; members of struct
    type (T x{}) as nested dicts"""
    m = re.search(r"\bstruct " + re.escape(name) + r"\s*\{", headers)
    if not m: raise KeyError("struct " + name)
    i, depth, j = m.end(), 1, m.end()
    while depth:
        depth += {"{": 1, "}": -1}.get(headers[j], 0); j += 1
    return struct_body(headers[i:j - 1])


def struct_body(body):
    members = {}
    body = re.sub(r"//[^\n]*", "", body)   # comments first: they hold ';', braces and parentheses
    # function bodies dropped ("...) const {...}" -> ";"); nested struct definitions with a member name kept apart
    top, n = "", 0
    while n < len(body):
        ch = body[n]
        if ch == "{":
            d, e = 1, n + 1
            while d:
                d += {"{": 1, "}": -1}.get(body[e], 0); e += 1
            head = top.rstrip()
            sm = re.search(r"\bstruct (\w+)\s*$", head)
            if sm:   # struct T { ... } name;
                nm = re.match(r"\s*(\w+)\s*;", body[e:])
                inner = body[n + 1:e - 1]
                members[nm.group(1)] = struct_body(inner)
                top = head[:sm.start()]; n = e + nm.end(); continue
            if re.search(r"(\)|\bconst|\bnoexcept|\boverride)$", head):   # a function body
                top = head + ";"; n = e; continue
            top += body[n:e]; n = e; continue
        top += ch; n += 1
    for st in top.split(";"):
        st = re.sub(r"//[^\n]*", "", st).strip()
        st = re.sub(r"^(?:#\w+[^\n]*\n\s*)+", "", st)
        if not st or "(" in st or st.startswith(("using", "static", "friend", "struct", "enum", "private", "public")):
            continue
        st = re.sub(r"^(?:private|public):\s*", "", st)
        mt = re.match(r"((?:const\s+)?[\w:<>]+(?:\s*<[^>]*>)?)\s+(.*)$", st, re.S)
        if not mt: continue
        typ, rest = mt.group(1), mt.group(2)
        for decl in split_args(rest):
            dm = re.fullmatch(r"(\w+)(\[\w+\])?\s*(?:=\s*(.+)|\{\})", decl.strip(), re.S)
            if not dm: continue
            if dm.group(3) is not None: members[dm.group(1)] = literal(dm.group(3))
            else:
                t = typ.split("::")[-1]
                try: members[dm.group(1)] = struct(t)
                except KeyError: pass   # a standard type ({} = zero), not written
    return members


# the tables the writers index
def table(name):
    m = re.search(re.escape(name) + r"\[\d*\]\s*=\s*\{([^}]*)\}", src + headers)
    return [literal(x) for x in split_args(m.group(1))]


codenames = {constants[k]: v for k, v in re.findall(r"n == (k\w+) \? \"(\w+)\"", headers)}
tables = {"kEnlargeToIni": table("kEnlargeToIni"), "kClassicScalers": table("kClassicScalers"),
          "kExposures": table("kExposures"), "kCurves": table("kCurves")}
funcs = {"nr::network_codename": lambda n: codenames[n],
         "residency_name": lambda r: re.search(r"residency_name.*?names\[4\] = \{([^}]*)\}", src, re.S)
         and literal(split_args(re.search(r"residency_name.*?names\[4\] = \{([^}]*)\}", src, re.S).group(1))[r]),
         "osd_name": lambda o: literal(split_args(re.search(r"osd_name.*?names\[\] = \{([^}]*)\}", src, re.S).group(1))[o])}


def value(expr, scope):
    e = expr.strip()
    if e.endswith(".c_str()"): e = e[:-8]
    m = re.fullmatch(r"(.+?)\s*\?\s*(.+?)\s*:\s*(.+)", e)
    if m:
        c = m.group(1).strip()
        cm = re.fullmatch(r"(.+?)\s*==\s*(.+)", c)
        cond = value(cm.group(1), scope) == literal(cm.group(2)) if cm else bool(value(c, scope))
        return value(m.group(2) if cond else m.group(3), scope)
    m = re.fullmatch(r"(?:double|float|int|uint32_t)\((.+)\)", e)   # a cast: the value itself
    if m: return value(m.group(1), scope)
    m = re.fullmatch(r"flag\((.+)\)", e)
    if m: return "true" if value(m.group(1), scope) else "false"
    m = re.fullmatch(r"std::clamp\((.+?),\s*(-?\d+),\s*(-?\d+)\)", e)
    if m:
        v = value(m.group(1), scope)
        assert int(m.group(2)) <= v <= int(m.group(3)), (e, v)
        return v
    m = re.fullmatch(r"(\w+)\[(.+)\]", e)
    if m and m.group(1) in tables: return tables[m.group(1)][value(m.group(2), scope)]
    m = re.fullmatch(r"([\w:]+)\((.+)\)", e)
    if m and m.group(1) in funcs: return funcs[m.group(1)](value(m.group(2), scope))
    if re.fullmatch(r"-?[\d.]+[fF]?|\"[^\"]*\"|true|false", e): return literal(e)
    path = re.findall(r"\w+|\[\d+\]", e)
    v = scope
    for p in path:
        v = v[int(p[1:-1])] if p.startswith("[") else v[p]
    return v


def render(sig, scope, n=None):
    fs = formats(body_of(sig))
    return [text % tuple(value(a, scope) for a in args) if args else text for text, args in (fs if n is None else fs[:n])]


config = struct("Config")
renodx_cfg = config["renodx"]
network = render("void write_network(", {"n": config["network"]})[0]
preprocess = render("void write_preprocess(", {"p": config["preprocess"], "v": config["preprocess"]["values"]})[0]
w2 = formats(body_of("void write_wilsjo2("))[0][0]   # no values: every key at auto
logsec = render("void write_log(", {"enabled": config["log_enabled"], "clear": config["log_clear"]})[0]
reusesec = render("void write_reuse(", {"every": config["controls"]["reuse_every"], "gate": config["controls"]["reuse_gate"]})[0]
inspect = render("void write_inspect(", {"i": struct("InspectConfig")})[0]   # every tool off
hudfix = render("void write_hudfix(", {"h": struct("HudFixConfig")})[0]   # auto
hudfix_addon = render("void write_hudfix(", {"h": dict(struct("HudFixConfig"), mode=0)})[0]   # off on the ReShade route
save = formats(body_of("void Config::save("))
assert len(save) == 8, len(save)   # [DlssNr], six Pass<N> lines (none by default), own keys
scope = dict(config, k=config["controls"], spatial=config["spatial"])
dlssnr = save[0][0] % tuple(value(a, scope) for a in save[0][1])
own = save[-1][0] % tuple(value(a, scope) for a in save[-1][1])   # MotionVectors: install.sh --lumenite writes lumenite


def renodx_section(enabled):
    r = dict(renodx_cfg, enabled=enabled)
    return "".join(render("void write_renodx(", {"r": r, "v": r["values"]}))


(out / "dlssnr-amd-optiscaler.ini").write_text(network + preprocess + renodx_section(False) + w2 + inspect + hudfix + logsec + reusesec,
                                                encoding="utf-8")
(out / "dlssnr-amd-optiscaler-renodx.ini").write_text(network + preprocess + renodx_section(True) + w2 + inspect + hudfix + logsec + reusesec,
                                                      encoding="utf-8")
(out / "dlssnr-amd-addon.ini").write_text(dlssnr + own + "\n" + network + preprocess + renodx_section(False) + inspect + hudfix_addon + logsec + reusesec,
                                          encoding="utf-8")
print("wrote", out / "dlssnr-amd-optiscaler.ini", out / "dlssnr-amd-optiscaler-renodx.ini", out / "dlssnr-amd-addon.ini")
