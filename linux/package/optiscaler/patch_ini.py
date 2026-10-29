#!/usr/bin/env python3
"""Rewrite the keys our install needs in OptiScaler's own OptiScaler.ini.

Key names and sections were read from the bundled OptiScaler's OptiScaler.ini and Config.cpp (nightly-20261020,
artifacts/ref/optiscaler-nightly-20261020; nightly-20261028's OptiScaler.ini has the same [Plugins] LoadAsiPlugins). Nothing is added that is not already in the file: every key here ships as
`<key>=auto` and is rewritten in place, so a key renamed upstream shows up as an error rather than as a silently
ignored line.
"""

import os
import re
import sys


def wanted_keys(game: str) -> dict:
    return {
        # OptiScaler loads the .asi plugins in its plugins folder (OptiScaler\plugins) only when asked to; ours is the
        # whole of NR on this route.
        ("Plugins", "LoadAsiPlugins"): "true",
        # [Spoofing] is left at the release's own `auto` values (Dxgi: true on AMD/Intel; the reported GPU: an NVIDIA
        # card, so the game offers DLSS). Dxgi is a per-game, per-system setting: if NR Control keeps waiting for the
        # game's upscaler, Dxgi=false is the thing to try (install.sh and the package README say so).
        # [Upscalers] Dx12Upscaler is left at auto: OptiScaler's own choice, nothing to do with Neural Rendering.
    }


def optional_keys() -> dict:
    """Keys rewritten where the release's ini has them, and not missed where it does not."""
    return {}


def main(game: str) -> int:
    path = os.path.join(game, "OptiScaler.ini")
    required = wanted_keys(game)
    wanted = {**required, **optional_keys()}
    # the file's own line endings kept (the release's ini has CRLF): only the rewritten lines change
    raw = open(path, "rb").read().decode("utf-8", errors="surrogateescape")
    newline = "\r\n" if "\r\n" in raw else "\n"
    lines = raw.split(newline)

    section = None
    seen = set()
    for i, line in enumerate(lines):
        header = re.match(r"\s*\[([^\]]+)\]", line)
        if header:
            section = header.group(1)
            continue
        # Live keys only. A commented-out line is documentation, and turning one into a setting would
        # both duplicate the real key and enable something nobody asked for.
        entry = re.match(r"\s*([A-Za-z0-9_]+)\s*=", line)
        if not entry or section is None:
            continue
        key = (section, entry.group(1))
        if key in wanted and key not in seen:
            seen.add(key)
            lines[i] = f"{entry.group(1)}={wanted[key]}"

    open(path, "wb").write(newline.join(lines).encode("utf-8", errors="surrogateescape"))

    for (s, k), v in sorted(wanted.items()):
        if (s, k) in seen or (s, k) in required:
            print(f"  {' ' if (s, k) in seen else '!'} [{s}] {k}={v}")

    missing = sorted(k for k in required if k not in seen)
    if missing:
        print(f"  ! not found in {path}; add them by hand: {missing}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} <game-dir>", file=sys.stderr)
        raise SystemExit(2)
    raise SystemExit(main(sys.argv[1]))
