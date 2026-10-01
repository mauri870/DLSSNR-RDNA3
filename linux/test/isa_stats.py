#!/usr/bin/env python3
"""Per-pipeline instruction mix, peak VGPRs and scratch (spill) instructions from a RADV shader dump.

    RADV_DEBUG=shaders <program that creates the pipelines> > isa.txt 2>&1
    python3 linux/test/isa_stats.py isa.txt [name,name,...]

check_rdna3.py --isa does both and names the pipelines (in creation order: the noise-field pipeline
first, then each kernel at its first use in the frame plan, which nr_graph --wiring lists).

Columns: instruction count, v_wmma, other vector ALU, scalar ALU, LDS, vector memory, scratch (a spill
or a private array), the highest VGPR the code touches (256 is the limit of a wave), and the number of
vector ALU instructions per WMMA.
"""
import collections
import re
import sys


def stats(text, names=()):
    rows = []
    for i, section in enumerate(text.split('shader: MESA_SHADER_COMPUTE')[1:]):
        disasm = section.split('disasm:')[-1]
        mnemonics = [l.split()[0] for l in disasm.splitlines() if re.match(r'\t[vsdgb][a-z0-9_]*\s', l)]
        c = collections.Counter()
        for m in mnemonics:
            if m.startswith('v_wmma'): c['wmma'] += 1
            elif m.startswith('v_'): c['valu'] += 1
            elif m.startswith('s_'): c['salu'] += 1
            elif m.startswith('ds_'): c['ds'] += 1
            elif m.startswith('scratch_'): c['scratch'] += 1
            elif m.startswith(('buffer_', 'global_', 'flat_')): c['vmem'] += 1
        top = 0
        for m in re.finditer(r'\bv\[?(\d+)(?::(\d+))?\]?', disasm):
            top = max(top, int(m.group(2) or m.group(1)))
        rows.append((names[i] if i < len(names) else f'#{i}', len(mnemonics), c, top + 1))
    return rows


def render(rows):
    out = [f"{'kernel':<22}{'instr':>7}{'wmma':>6}{'valu':>7}{'salu':>6}{'ds':>5}{'vmem':>6}{'scratch':>8}{'vgpr':>6}  valu/wmma"]
    for name, n, c, vgpr in rows:
        out.append(f"{name:<22}{n:>7}{c['wmma']:>6}{c['valu']:>7}{c['salu']:>6}{c['ds']:>5}{c['vmem']:>6}"
                   f"{c['scratch']:>8}{vgpr:>6}  {c['valu'] / max(c['wmma'], 1):7.1f}")
    return '\n'.join(out)


if __name__ == '__main__':
    print(render(stats(open(sys.argv[1]).read(), sys.argv[2].split(',') if len(sys.argv) > 2 else ())))
