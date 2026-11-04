#!/usr/bin/env python3
"""Per-shader interface manifest for the D3D9 -> WebGPU shim (reads what shader_spike.py produced).

For each out/shaders/<pkg>/<name>.wgsl.gen writes an entry to out/shader_manifest/<pkg>.json (git-ignored):
  stage, profile, sha1 (of the d3dbc token stream), wgsl file,
  inputs   [{name, location|builtin, type, interp, semantic?}]   (vertex inputs carry the d3d semantic from the dcl)
  outputs  [{name, location|builtin, type}]
  bindings {samplers: [{n, texture_binding, sampler_binding, dim}], constants: {binding, vec4_count} | null}
The binding numbers are the real @binding values in the WGSL; constants are one vec4 array indexed by D3D register.

usage: python tools/shader_manifest.py [out/shaders/<pkg> ...]   (default: all)
"""
import hashlib
import json
import re
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
VKD3D = ROOT / 'local/dl/vkd3d/build/vkd3d-compiler.exe'
ATTR = r'@(?:location\((\d+)u\)|builtin\((\w+)\))(?:\s+@interpolate\(([^)]*)\))?'


def members(text):
    out = []
    for m in re.finditer(ATTR + r'\s+(\w+)\s*:\s*([\w<>, ]+?)\s*(?:,|\)|$)', text, re.M):
        loc, bi, interp, name, ty = m.groups()
        e = dict(name=name, type=ty.strip())
        e.update(location=int(loc)) if loc is not None else e.update(builtin=bi)
        if interp:
            e['interp'] = interp
        out.append(e)
    return out


def profile(code):
    tok, = struct.unpack_from('<I', code, 0)
    return '%s_%d_%d' % ({0xFFFE: 'vs', 0xFFFF: 'ps'}.get(tok >> 16, '??'), (tok >> 8) & 0xff, tok & 0xff)


def semantics(dxbc):
    txt = subprocess.run([str(VKD3D), '-x', 'd3dbc', '-b', 'd3d-asm', str(dxbc)], capture_output=True, text=True,
                         errors='replace').stdout
    return {int(m.group(2)): m.group(1) for m in re.finditer(r'^dcl_(\w+?)(?:_\w+)? v(\d+)', txt, re.M)} \
        if txt.startswith('vs') else {}


def entry(dxbc):
    w = dxbc.with_suffix('.wgsl.gen')
    t = w.read_text()
    code = dxbc.read_bytes()
    prof = profile(code)
    m = re.search(r'\bfn main\((.*?)\)\s*(->\s*(.*?))?\s*\{', t, re.S)
    ins = members(m.group(1))
    if prof.startswith('vs'):
        sem = semantics(dxbc)
        for i in ins:
            if i.get('location') in sem:
                i['semantic'] = sem[i['location']]
    ret = (m.group(3) or '').strip()
    if ret.startswith('tint_symbol'):
        outs = members(re.search(r'struct %s \{(.*?)\n\}' % re.escape(ret), t, re.S).group(1))
    elif ret:
        mm = re.match(ATTR + r'\s+(.*)', ret)
        outs = [dict(name='o0', type=mm.group(4).strip(), **({'location': int(mm.group(1))} if mm.group(1) else {'builtin': mm.group(2)}))]
    else:
        outs = []
    textures = {int(n): (int(b), d) for b, n, d in re.findall(r'@binding\((\d+)u\) var t(\d+) : texture_(\w+)<', t)}
    samplers = {int(n): int(b) for b, n in re.findall(r'@binding\((\d+)u\) var s(\d+) : sampler', t)}
    cb = re.search(r'@binding\((\d+)u\) var<uniform> \w+ : (\w+)', t)
    consts = None
    if cb:
        n = re.search(r'array<vec4<f32>, (\d+)u>', t)
        consts = dict(binding=int(cb.group(1)), vec4_count=int(n.group(1)))
    return dict(stage='vertex' if prof.startswith('vs') else 'fragment', profile=prof,
                sha1=hashlib.sha1(code).hexdigest(), wgsl=w.name, inputs=ins, outputs=outs,
                bindings=dict(samplers=[dict(n=n, texture_binding=textures[n][0], sampler_binding=samplers.get(n),
                                             dim=textures[n][1]) for n in sorted(textures)], constants=consts))


def main():
    dirs = [Path(a) for a in sys.argv[1:]] or sorted((ROOT / 'out/shaders').glob('shaderpackage*'))
    od = ROOT / 'out/shader_manifest'
    od.mkdir(parents=True, exist_ok=True)
    for d in dirs:
        man = {}
        for f in sorted(d.glob('*.dxbc')):
            if f.with_suffix('.wgsl.gen').exists():
                man[f.name[:-5]] = entry(f)
        (od / (d.name + '.json')).write_text(json.dumps(man, indent=1))
        print(d.name, len(man))


if __name__ == '__main__':
    main()
