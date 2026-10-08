#!/usr/bin/env python3
"""Build the Linux network for one GPU architecture from linux/shaders/<arch>/pipelines.json.

    build_network.py <arch> [--out DIR] [--check DIR]

Writes (default build/linux/<arch>/network):
    g_<name>.spv + the four marker files   the network, what --spv-dir points at
    runtime/                                 the arch-neutral passes (linux/shaders/passes)
    temporal/                                the temporal variants and the motion estimator

The host must be compiled with the same constants: linux/build/arch/<arch>.sh.
--check DIR compares every network SPV and marker with DIR byte for byte and
lists the differences (exit 1 if any).
"""
import argparse
import json
import shutil
import subprocess
import sys
from pathlib import Path

R = Path(__file__).resolve().parents[2]
RUNTIME = ['runtime_alpha', 'runtime_encode', 'runtime_transfer', 'runtime_prep', 'runtime_depth', 'cascade_lograt', 'cascade_blur', 'cascade_feed',
           'runtime_reuse_store', 'runtime_reuse_motion', 'runtime_reuse_warp', 'runtime_reuse_stat']
MOTION = ['motion_luma', 'motion_estimate']


def glslang(arch, src, defines, out):
    cmd = [str(R / 'toolchain/glslang/bin/glslang'), '-V', '--target-env', 'vulkan1.3',
           '-I' + str(R / 'linux/shaders' / arch / 'include')]
    cmd += ['-D' + d for d in defines] + [str(src), '-o', str(out)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.exit(f'glslang failed on {src}:\n{r.stdout}{r.stderr}')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('arch')
    ap.add_argument('--out', type=Path)
    ap.add_argument('--check', type=Path)
    a = ap.parse_args()
    table = json.loads((R / 'linux/shaders' / a.arch / 'pipelines.json').read_text())
    out = a.out or R / 'build/linux' / a.arch / 'network'
    if out.exists():
        shutil.rmtree(out)
    (out / 'runtime').mkdir(parents=True)
    (out / 'temporal').mkdir()

    pipelines = table['pipelines']
    for name, e in pipelines.items():
        glslang(a.arch, R / 'linux/shaders' / a.arch / e['source'], e['defines'], out / f'g_{name}.spv')
    for name, text in table['markers'].items():
        (out / name).write_text('\n'.join(text) + '\n' if isinstance(text, list) else text + '\n')
    for name, v in table['variants'].items():
        base = pipelines[v['base']]
        glslang(a.arch, R / 'linux/shaders' / a.arch / base['source'], base['defines'] + v['add'],
                out / 'temporal' / f'{name}.spv')
    for k in MOTION:
        glslang(a.arch, R / 'linux/shaders/passes' / f'{k}.comp', [], out / 'temporal' / f'{k}.spv')
    shutil.copy2(out / 'shader-constants.txt', out / 'temporal' / 'shader-constants.txt')
    for k in RUNTIME:
        glslang(a.arch, R / 'linux/shaders/passes' / f'{k}.comp', [], out / 'runtime' / f'{k}.spv')
    print(f'{out}: {len(pipelines)} network pipelines, {len(table["variants"]) + len(MOTION)} temporal, '
          f'{len(RUNTIME)} runtime')

    if a.check:
        bad = [p.name for p in sorted(out.iterdir()) if p.is_file()
               if not (a.check / p.name).is_file() or (a.check / p.name).read_bytes() != p.read_bytes()]
        print('check against', a.check, ':', 'identical' if not bad else 'DIFFERENT: ' + ' '.join(bad))
        sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
