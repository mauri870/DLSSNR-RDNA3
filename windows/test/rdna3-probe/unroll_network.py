#!/usr/bin/env python3
"""Replace the pipelines the AMD Windows driver's compiler (LLPC) crashes on by fully unrolled copies.

    unroll_network.py NETWORK_DIR

NETWORK_DIR is a built RDNA3 network (python3 linux/build/build_network.py rdna3 --out DIR). Every pipeline
built from fswin_t.comp, attn.comp or ffwd3_t.comp, and every temporal variant of them, is preprocessed with
glslang, loses its [[dont_unroll]] attributes (windows/build/unroll_glsl.py does not know them), is unrolled
with windows/build/unroll_glsl.py and compiled again; the result replaces the file in NETWORK_DIR.

Why: on Adrenalin 26.9.2 LLPC dies in vkCreateComputePipelines (access violation inside amdvlk64.dll) on a
fragment array element that is the destination of coopMatMulAdd through a dynamic index and whose component
is read afterwards. Constant indices avoid it, and unrolling makes every index constant. On RADV the unrolled
network gives the bit-identical picture and costs 2 to 4 % more time.
"""
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

R = Path(__file__).resolve().parents[3]
GLSLANG = R / 'toolchain/glslang/bin/glslang'
SHADERS = R / 'linux/shaders/rdna3'
SOURCES = {'fswin_t.comp', 'attn.comp', 'ffwd3_t.comp'}


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    net = Path(sys.argv[1])
    table = json.loads((SHADERS / 'pipelines.json').read_text())
    jobs = []
    for name, e in table['pipelines'].items():
        if e['source'] in SOURCES:
            jobs.append((net / f'g_{name}.spv', e['source'], e['defines']))
    for name, v in table['variants'].items():
        base = table['pipelines'][v['base']]
        if base['source'] in SOURCES:
            jobs.append((net / 'temporal' / f'{name}.spv', base['source'], base['defines'] + v['add']))
    for out, source, defines in jobs:
        pre = out.with_suffix('.pre.comp')
        r = subprocess.run([str(GLSLANG), '-E', f'-I{SHADERS}/include'] + ['-D' + d for d in defines] + [str(SHADERS / source)],
                           capture_output=True, text=True)
        if r.returncode:
            sys.exit(f'preprocessing {source} for {out.name} failed:\n{r.stderr}')
        pre.write_text(re.sub(r'\[\[\s*dont_unroll\s*\]\]\s*', '', r.stdout))
        subprocess.run([sys.executable, str(R / 'windows/build/unroll_glsl.py'), str(pre), str(pre)], check=True,
                       stdout=subprocess.DEVNULL)
        tmp = out.with_suffix('.unrolled')
        c = subprocess.run([str(GLSLANG), '-V', '--target-env', 'vulkan1.3', str(pre), '-o', str(tmp)],
                           capture_output=True, text=True)
        if c.returncode:
            sys.exit(f'compiling the unrolled {out.name} failed:\n{c.stdout}{c.stderr}')
        shutil.move(tmp, out)
        pre.unlink()
    print(f'{len(jobs)} pipelines unrolled in {net}')


if __name__ == '__main__':
    main()
