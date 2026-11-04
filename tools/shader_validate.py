#!/usr/bin/env python3
"""Check that translated WGSL keeps each original shader's interface (Phase 0 follow-up, shader translation).

For every shader in out/shaders/<pkg>/*.dxbc that has a .wgsl.gen next to it, the d3dbc is disassembled with
vkd3d-compiler (-b d3d-asm) and compared with the WGSL:
  inputs    distinct input registers (dcl'd or read: vN, vPos, vFace) == entry-point parameters
  samplers  distinct samplers sN == texture bindings tN (and sampler bindings sN)
  consts    highest non-def constant register read + 1 <= size of the constant uniform array; none read => no array
  outputs   distinct output registers written (oPos oTn oDn oFog oPts oCn oDepth) == WGSL output variables oN
Optionally restrict with --names; prints a summary and the mismatches. Results never leave out/.

usage: python tools/shader_validate.py [out/shaders/shaderpackage013] [--limit N] [--names a,b,...]
"""
import argparse
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
VKD3D = ROOT / 'local/dl/vkd3d/build/vkd3d-compiler.exe'


def asm_facts(path):
    txt = subprocess.run([str(VKD3D), '-x', 'd3dbc', '-b', 'd3d-asm', str(path)], capture_output=True, text=True,
                         errors='replace').stdout
    ins, samplers, cmax, rel, outs, defs = set(), set(), -1, False, set(), set()
    kind = txt.split()[0][:2] if txt.strip() else '??'
    for line in txt.splitlines()[1:]:
        line = line.split('//')[0].strip()
        if not line:
            continue
        if line.startswith('def'):
            m = re.match(r'def[ib]? (c\d+|b\d+|i\d+)', line)
            if m and line.startswith('def '):
                defs.add(m.group(1))
            continue
        op, _, rest = line.partition(' ')
        ops = [o.strip() for o in re.split(r',(?![^\[]*\])', rest)]
        if op.startswith('dcl'):
            for o in ops:
                m = re.match(r'(s\d+)', o)
                if m:
                    samplers.add(m.group(1))
                m = re.match(r'(v\d+|vPos|vFace|t\d+)\b', o)
                if m:
                    ins.add(m.group(1))
                m = re.match(r'(o\w+)', o)
                if m:
                    outs.add(re.sub(r'\..*', '', m.group(1)))
            continue
        for i, o in enumerate(ops):
            o = o.lstrip('-!').strip()
            for m in re.finditer(r'\bs(\d+)\b', o):
                samplers.add('s' + m.group(1))
            for m in re.finditer(r'\b([vt])(\d+|Pos|Face)\b', o):
                ins.add(m.group(1) + m.group(2))
            for m in re.finditer(r'\bc(\d+)\b', o):
                if 'c' + m.group(1) not in defs:
                    cmax = max(cmax, int(m.group(1)))
            if re.search(r'\bc\[', o) or re.search(r'c\d+\[', o):
                rel = True
            if i == 0 and not op.startswith(('if', 'break', 'rep', 'loop', 'call', 'texkill', 'end', 'else', 'ret')):
                m = re.match(r'(o\w+?)(?:\.|$)', o)
                if m:
                    outs.add(m.group(1))
    return dict(kind=kind, defs=defs, ins=ins, samplers=samplers, cmax=cmax, rel=rel, outs=outs)


def wgsl_facts(path):
    t = path.read_text()
    m = re.search(r'\bfn main\((.*?)\)\s*(->|\{)', t, re.S)
    params = [p for p in re.split(r',\s*(?=@)', m.group(1).strip()) if p.strip()] if m else []
    cb = re.search(r'array<vec4<f32>, (\d+)u>', t)
    return dict(n_in=len(params), tex=set('s%s' % x for x in re.findall(r'\bvar t(\d+) :', t)),
                smp=set('s%s' % x for x in re.findall(r'\bvar s(\d+) :', t)), cb=int(cb.group(1)) if cb else 0,
                outs=set(re.findall(r'var<private> (o\w*) :', t)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dirs', nargs='*', default=[str(ROOT / 'out/shaders/shaderpackage013')])
    ap.add_argument('--limit', type=int, default=0)
    ap.add_argument('--names')
    a = ap.parse_args()
    bad, n = [], 0
    for d in map(Path, a.dirs):
        files = sorted(d.glob('*.dxbc'))
        if a.names:
            keep = a.names.split(',')
            files = [f for f in files if f.name[:-5] in keep]
        if a.limit:
            files = files[::max(1, len(files) // a.limit)]
        for f in files:
            w = f.with_suffix('.wgsl.gen')
            if not w.exists():
                continue
            n += 1
            A, W = asm_facts(f), wgsl_facts(w)
            problems = []
            if len(A['ins']) != W['n_in']:
                problems.append('inputs asm=%s wgsl_params=%d' % (sorted(A['ins']), W['n_in']))
            if A['samplers'] != W['tex']:
                problems.append('samplers asm=%s wgsl=%s' % (sorted(A['samplers']), sorted(W['tex'])))
            if A['cmax'] >= 0 or A['rel']:
                if not A['rel'] and W['cb'] < A['cmax'] + 1:
                    problems.append('consts max c%d but cb size %d' % (A['cmax'], W['cb']))
            elif W['cb']:
                problems.append('consts none read but cb size %d' % W['cb'])
            # vkd3d adds the D3D9 default diffuse (1,1,1,1) output to vertex shaders that don't write oD0
            extra = 1 if A['kind'] == 'vs' and 'oPos' in A['outs'] and 'oD0' not in A['outs'] else 0
            if len(A['outs']) + extra != len(W['outs']):
                problems.append('outputs asm=%s wgsl=%s' % (sorted(A['outs']), sorted(W['outs'])))
            if problems:
                bad.append((f.name[:-5], problems))
    print('checked %d shaders, %d with interface differences' % (n, len(bad)))
    for name, p in bad[:40]:
        print(' ', name, '; '.join(p))


if __name__ == '__main__':
    main()
