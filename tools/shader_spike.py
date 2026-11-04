#!/usr/bin/env python3
"""Shader translation spike (Phase 0, spike 4).

Unpacks a shaderpackageNNN.sdp (header: u32 version, u32 count, u32 payload size; then per entry a 256-byte name,
u32 size, D3D9 token stream) into out/shaders/<pkg>/ (git-ignored), then runs every shader through
  vkd3d-compiler -x d3dbc -b spirv-binary     (d3dbc -> SPIR-V)
  tint -f wgsl                                (SPIR-V -> WGSL)
and reports pass rates and bucketed failure reasons. Nothing derived from the shaders leaves out/.

Per-shader results (sha1 of the token stream, name, profile, stage, reason) go to out/shader_results/<pkg>.jsonl.
Arguments may be .sdp files or already-unpacked out/shaders/<pkg> directories (*.dxbc are the raw token streams).

usage: python tools/shader_spike.py <package.sdp|dir> [...] [--vkd3d EXE] [--tint EXE]
"""
import hashlib
import argparse
import json
import re
import struct
import subprocess
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
VKD3D = ROOT / 'local/dl/vkd3d/build/vkd3d-compiler.exe'
TINT = next((ROOT / 'local/dl/dawn').glob('*/bin/tint.exe'), None)


def unpack(sdp):
    d = sdp.read_bytes()
    version, count, size = struct.unpack_from('<III', d, 0)
    o, out = 12, []
    for _ in range(count):
        name = d[o:o + 0x100].split(b'\0')[0].decode()
        n, = struct.unpack_from('<I', d, o + 0x100)
        out.append((name, d[o + 0x104:o + 0x104 + n]))
        o += 0x104 + n
    assert o == len(d), (o, len(d))
    return version, out


def bucket(msg):
    msg = msg.strip().splitlines()
    first = next((l for l in msg if 'error' in l.lower()), msg[0] if msg else 'no message')
    first = re.sub(r'^.*?(error|E\d+):\s*', '', first, flags=re.I)
    first = re.sub(r'\b\d+\b', 'N', first)
    first = re.sub(r"'[^']*'", "'X'", first)
    return first[:120]


def profile(code):
    tok, = struct.unpack_from('<I', code, 0)
    kind = {0xFFFE: 'vs', 0xFFFF: 'ps'}.get(tok >> 16, '??')
    return '%s_%d_%d' % (kind, (tok >> 8) & 0xff, tok & 0xff)


def run(tools, path):
    spv, wgsl = path.with_suffix('.spv'), path.with_suffix('.wgsl.gen')
    r = subprocess.run([str(tools['vkd3d']), '-x', 'd3dbc', '-b', 'spirv-binary', '-o', str(spv), str(path)],
                       capture_output=True, text=True, errors='replace')
    if r.returncode or not spv.exists():
        return 'vkd3d', bucket(r.stderr + r.stdout)
    r = subprocess.run([str(tools['tint']), str(spv), '-f', 'wgsl', '--allow-non-uniform-derivatives', 'true', '-o', str(wgsl)],
                       capture_output=True, text=True, errors='replace')
    if r.returncode or not wgsl.exists():
        return 'tint', bucket(r.stderr + r.stdout)
    return 'ok', None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('sdp', nargs='+')
    ap.add_argument('--vkd3d', default=str(VKD3D))
    ap.add_argument('--tint', default=str(TINT))
    ap.add_argument('--jobs', type=int, default=16)
    a = ap.parse_args()
    tools = dict(vkd3d=a.vkd3d, tint=a.tint)
    summary = {}
    for sdp in map(Path, a.sdp):
        d = ROOT / 'out/shaders' / sdp.stem
        if sdp.is_dir():
            d = sdp
            version = 100
            shaders = [(f.name[:-5], f.read_bytes()) for f in sorted(d.glob('*.dxbc'))]
        else:
            version, shaders = unpack(sdp)
        d.mkdir(parents=True, exist_ok=True)
        paths, profiles, p_seen = [], Counter(), {}
        for name, code in shaders:
            if p_seen.get(name):
                name = '%s#%d' % (name, p_seen[name])
            p_seen[name] = p_seen.get(name, 0) + 1
            p = d / (name + '.dxbc')        # *.dxbc is git-ignored; content is legacy d3dbc
            p.write_bytes(code)
            paths.append(p)
            profiles[profile(code)] += 1
        with ThreadPoolExecutor(a.jobs) as ex:
            results = list(ex.map(lambda p: run(tools, p), paths))
        stages = Counter(r[0] for r in results)
        reasons = Counter((r[0], r[1]) for r in results if r[0] != 'ok')
        per_profile = Counter((profile(c), r[0]) for (n, c), r in zip(shaders, results))
        summary[d.name] = dict(version=version, shaders=len(shaders), profiles=dict(profiles),
                                 ok=stages['ok'], vkd3d_fail=stages['vkd3d'], tint_fail=stages['tint'],
                                 pass_rate=round(stages['ok'] / len(shaders), 4),
                                 by_profile={'%s/%s' % k: v for k, v in sorted(per_profile.items())},
                                 top_failures=[[s, m, n] for (s, m), n in reasons.most_common(12)],
                                 failed=[[n, r[0]] for (n, _), r in zip(shaders, results) if r[0] != 'ok'])
        rd = ROOT / 'out/shader_results'
        rd.mkdir(parents=True, exist_ok=True)
        with open(rd / (d.name + '.jsonl'), 'w') as f:
            for (n, c), r in zip(shaders, results):
                f.write(json.dumps(dict(hash=hashlib.sha1(c).hexdigest(), name=n, profile=profile(c),
                                        stage=r[0], reason=r[1])) + chr(10))
        s = summary[d.name]
        print('%s: %d shaders %s  ok=%d vkd3d_fail=%d tint_fail=%d' % (sdp.name, s['shaders'], s['profiles'],
                                                                        s['ok'], s['vkd3d_fail'], s['tint_fail']))
        for st, m, n in s['top_failures']:
            print('   %5d %-5s %s' % (n, st, m))
    out = ROOT / 'out/shader_spike.json'
    old = json.loads(out.read_text()) if out.exists() else {}
    old.update(summary)
    out.write_text(json.dumps(old, indent=1))


if __name__ == '__main__':
    main()
