#!/usr/bin/env python3
"""LumeniteFX Kernel for the ReShade routes, from a LumeniteFX zip (its author's GitHub; install.sh --lumenite).

  lumenite.py check   <LumeniteFX zip> <DLSS5_Feed.fx>
  lumenite.py install <LumeniteFX zip> <DLSS5_Feed.fx> <reshade-shaders dir>

Nothing of LumeniteFX is in this package (its AGNYA licence allows propagation only through its author's links), and
nothing of it is changed: the files are copied as they are in the zip. What is copied is what lumenite_Kernel.fx
needs - Shaders/lumenite_Kernel.fx, Shaders/include/*.fxh, Textures/* - and the licence and notice; the other
LumeniteFX effects are left out, since ReShade would compile every effect it finds.

The check is of the interface, not of the version, so a newer LumeniteFX installs as long as it still offers what
this package uses: lumenite_Kernel.fx with technique Lumenite_Kernel, every file it includes, and the flow textures
DLSS5_Feed.fx reads (Kernel::tFlow, Kernel::tConfidence) declared as DLSS5_Feed.fx declares them. The add-on checks the
rest in the game (nr_reshade_addon.cpp)."""
import re
import sys
import zipfile
from pathlib import Path


def fail(message):
    print(message, file=sys.stderr)
    sys.exit(1)


def entries(zip_path):
    """{'Shaders/lumenite_Kernel.fx': ZipInfo, ...}: the paths below the repository's top folder"""
    try:
        z = zipfile.ZipFile(zip_path)
    except (OSError, zipfile.BadZipFile) as e:
        fail(f"{zip_path} is not a zip that can be opened: {e}")
    found = {}
    for info in z.infolist():
        if info.is_dir():
            continue
        name = info.filename.replace("\\", "/")
        m = re.search(r"(?:^|/)((?:Shaders|Textures)/.+|LICENSE\.md|NOTICE)$", name)
        if m and "/" not in name[: m.start(1)].rstrip("/"):   # at most one folder above (the repository's)
            found.setdefault(m.group(1), info)
    return z, found


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def normal(text):
    return re.sub(r"\s+", "", text)


def check(zip_path, feed_path):
    z, found = entries(zip_path)
    if "Shaders/lumenite_Kernel.fx" not in found:
        fail("the zip has no Shaders/lumenite_Kernel.fx (not a LumeniteFX zip?)")
    kernel = strip_comments(z.read(found["Shaders/lumenite_Kernel.fx"]).decode("utf-8", "replace"))
    if not re.search(r"\btechnique\s+Lumenite_Kernel\b", kernel):
        fail("lumenite_Kernel.fx has no technique Lumenite_Kernel (this LumeniteFX version does not work with this package)")
    for inc in re.findall(r'#\s*include\s+"([^"]+)"', kernel):
        inc = inc.lstrip("./")
        if inc in ("ReShade.fxh", "ReShadeUI.fxh"):
            continue   # ReShade's own headers, in the package
        if "Shaders/" + inc not in found:
            fail(f"{inc}, included by lumenite_Kernel.fx, is not in the zip")
    # The textures DLSS5_Feed.fx re-declares for provider 3 (LumeniteFX Kernel) must be declared the same way.
    feed = Path(feed_path).read_text(encoding="utf-8", errors="replace")
    m = re.search(r"#elif\s+DLSS5_MV_PROVIDER\s*==\s*3\b(.*?)#elif", feed, flags=re.S)
    if not m:
        fail(f"{feed_path} has no DLSS5_MV_PROVIDER == 3 part")
    wanted = re.findall(r"texture2D\s+\w+\s*\{[^}]*\}\s*;", strip_comments(m.group(1)))
    if not wanted or "namespace Kernel" not in m.group(1):
        fail(f"the LumeniteFX part of {feed_path} is not as expected")
    if not re.search(r"\bnamespace\s+Kernel\b", kernel):
        fail("lumenite_Kernel.fx has no namespace Kernel (this LumeniteFX version does not work with this package)")
    flat = normal(kernel)
    for decl in wanted:
        if normal(decl) not in flat:
            fail(f"{decl.split()[1]} in lumenite_Kernel.fx differs from what DLSS5_Feed.fx reads (this LumeniteFX version does not work with this package)")
    commit = z.comment.decode("ascii", "replace").strip() if z.comment else ""
    return z, found, commit


def install(zip_path, feed_path, dest):
    z, found, commit = check(zip_path, feed_path)
    dest = Path(dest)
    shaders, textures = dest / "Shaders", dest / "Textures"
    picked = {"Shaders/lumenite_Kernel.fx": shaders / "lumenite_Kernel.fx"}
    for name in found:
        if name.startswith("Shaders/include/") and name.endswith(".fxh") and name.count("/") == 2:
            picked[name] = shaders / "include" / name.split("/")[-1]
        elif name.startswith("Textures/") and name.count("/") == 1:
            picked[name] = textures / name.split("/")[-1]
    if "LICENSE.md" in found:
        picked["LICENSE.md"] = shaders / "LumeniteFX-LICENSE.md"
    if "NOTICE" in found:
        picked["NOTICE"] = shaders / "LumeniteFX-NOTICE.txt"
    for name, out in picked.items():
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_bytes(z.read(found[name]))
    print(f"LumeniteFX{(' ' + commit[:7]) if commit else ''}: {len(picked)} files installed into {dest}")


if __name__ == "__main__":
    if len(sys.argv) == 4 and sys.argv[1] == "check":
        _, _, commit = check(sys.argv[2], sys.argv[3])
        print(commit)
    elif len(sys.argv) == 5 and sys.argv[1] == "install":
        install(sys.argv[2], sys.argv[3], sys.argv[4])
    else:
        fail(__doc__)
