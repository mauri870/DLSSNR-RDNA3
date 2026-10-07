#!/usr/bin/env python3
"""Build the Linux network for one GPU architecture from linux/shaders/<arch>/pipelines.json.

    build_network.py <arch> [--int4] [--out DIR] [--check DIR]

Writes (default build/linux/<arch>/network, with --int4 build/linux/<arch>/network-int4):
    g_<name>.spv + the four marker files   the network, what --spv-dir points at
    runtime/                                 the arch-neutral passes (linux/shaders/passes)
    temporal/                                the temporal variants and the motion estimator

The host must be compiled with the same constants: linux/build/arch/<arch>.sh.
--int4: the int4 mixed network (pipelines.json with pipelines-int4.json applied; the host built with NR_INT4=1):
the network and temporal/ (the runtime passes are the default network's), the host's g_<name>.spv.iu4 markers and
hs-pipelines.txt (the pipelines built with the hard-swish, for the host's guard).
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
RUNTIME = ['runtime_alpha', 'runtime_encode', 'runtime_transfer', 'runtime_upscale', 'runtime_prep', 'runtime_depth', 'cascade_lograt', 'cascade_blur', 'cascade_feed',
           'runtime_downscale', 'runtime_taps']
# runtime passes built from another pass's source with defines: name -> (source, defines)
RUNTIME_VARIANTS = {'runtime_transfer_store': ('runtime_transfer', ['NR_STORE_NATIVE'])}
MOTION = ['motion_luma', 'motion_estimate']


def glslang(arch, src, defines, out):
    cmd = [str(R / 'toolchain/glslang/bin/glslang'), '-V', '--target-env', 'vulkan1.3',
           '-I' + str(R / 'linux/shaders' / arch / 'include'), '-I' + str(R / 'linux/shaders/passes/include')]
    cmd += ['-D' + d for d in defines] + [str(src), '-o', str(out)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.exit(f'glslang failed on {src}:\n{r.stdout}{r.stderr}')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('arch')
    ap.add_argument('--int4', action='store_true')
    ap.add_argument('--out', type=Path)
    ap.add_argument('--check', type=Path)
    a = ap.parse_args()
    table = json.loads((R / 'linux/shaders' / a.arch / 'pipelines.json').read_text())
    if a.int4:
        i4 = json.loads((R / 'linux/shaders' / a.arch / 'pipelines-int4.json').read_text())
        default = table['pipelines']
        def derive(e, c):
            return dict(e, defines=[x for x in e['defines'] if x not in c.get('remove', [])] + c.get('add', []))
        table['pipelines'] = {k: derive(e, i4['change'].get(k, {})) for k, e in default.items()}
        table['pipelines'].update({k: derive(default[c['base']], c) for k, c in i4['new'].items()})
    out = a.out or R / 'build/linux' / a.arch / ('network-int4' if a.int4 else 'network')
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
    if a.int4:
        for name in i4['iu4']:
            (out / f'g_{name}.spv.iu4').write_text('0\n')
        hs = sorted(k for k, e in pipelines.items() if 'NR_ACT_HS=2' in e['defines'])
        (out / 'hs-pipelines.txt').write_text('\n'.join(hs) + '\n')
        (out / 'runtime').rmdir()
    else:
        for k in RUNTIME:
            glslang(a.arch, R / 'linux/shaders/passes' / f'{k}.comp', [], out / 'runtime' / f'{k}.spv')
        for k, (src, defines) in RUNTIME_VARIANTS.items():
            glslang(a.arch, R / 'linux/shaders/passes' / f'{src}.comp', defines, out / 'runtime' / f'{k}.spv')
        # runtime_upscale.spv carries AMD FidelityFX Super Resolution 1 (MIT): its notice goes with it
        shutil.copy2(R / 'linux/shaders/passes/include/fsr1/LICENSE.txt', out / 'runtime' / 'FidelityFX-FSR1-LICENSE.txt')
    print(f'{out}: {len(pipelines)} network pipelines, {len(table["variants"]) + len(MOTION)} temporal, '
          f'{0 if a.int4 else len(RUNTIME) + len(RUNTIME_VARIANTS)} runtime')

    if a.check:
        bad = [p.name for p in sorted(out.iterdir()) if p.is_file()
               if not (a.check / p.name).is_file() or (a.check / p.name).read_bytes() != p.read_bytes()]
        print('check against', a.check, ':', 'identical' if not bad else 'DIFFERENT: ' + ' '.join(bad))
        sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
