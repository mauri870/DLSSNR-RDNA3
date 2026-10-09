#!/usr/bin/env python3
"""Bisection variants of one pipeline: the same shader with one #define removed at a time.

    make_variants.py OUT_DIR [pipeline]        (default pipeline: fswinimagepreds32nh)

Writes OUT_DIR/<pipeline>_without_<define>.spv. Defines the shader cannot do without (it stops compiling)
are skipped. Put the directory next to the probe as probe-variants/ and the probe tries every file.
On Adrenalin 26.9.2 all 38 variants of fswinimagepreds32nh crashed LLPC like the original, so the trigger is
not any one feature define; it is in the code they share.
"""
import json
import re
import subprocess
import sys
from pathlib import Path

R = Path(__file__).resolve().parents[3]


def main():
    if len(sys.argv) not in (2, 3):
        sys.exit(__doc__)
    out = Path(sys.argv[1])
    name = sys.argv[2] if len(sys.argv) == 3 else 'fswinimagepreds32nh'
    out.mkdir(parents=True, exist_ok=True)
    entry = json.loads((R / 'linux/shaders/rdna3/pipelines.json').read_text())['pipelines'][name]
    source = R / 'linux/shaders/rdna3' / entry['source']
    made = 0
    for d in entry['defines']:
        key = re.sub(r'[^A-Za-z0-9]+', '_', d).strip('_')
        r = subprocess.run([str(R / 'toolchain/glslang/bin/glslang'), '-V', '--target-env', 'vulkan1.3',
                            f'-I{R}/linux/shaders/rdna3/include'] + ['-D' + x for x in entry['defines'] if x != d]
                           + [str(source), '-o', str(out / f'{name}_without_{key}.spv')], capture_output=True, text=True)
        made += r.returncode == 0
    print(f'{made} of {len(entry["defines"])} variants written to {out}')


if __name__ == '__main__':
    main()
