#!/usr/bin/env python3
"""Compiled functions that copy a decomp type with a user-declared destructor through its implicit copy.

The decomp often models a smart pointer or owning handle as a struct with a destructor (release/free) and no copy
constructor. Wherever the code copies one, clang uses the implicit bitwise copy: no AddRef, then the copy's
destructor releases the original's reference. 0x4bc320 (BSRenderedTexture::GetTexture) is the case found:
`(this ? m_textures[i] : GO(0))` makes a temporary copy of the texture NiPointer and destroys it, so each call
dropped a reference; the render target was freed while in use and its memory reused by an animation object
(main-thread call through a two-slot vtable into the string after it, "ANIM"). The original takes the member's
address and copies nothing.

clang reports these with -Wdeprecated-copy-with-dtor ("definition of implicit copy constructor/assignment
operator for 'T' is deprecated because it has a user-declared destructor"), once per class per translation unit,
with a note at the first use. The first use is mapped to its @fnv function exactly; because later uses in the
same unit are not reported, every other @fnv function of that unit whose body names T is flagged too (lifting an
extra function only costs speed).

usage: python tools/copy_dtor_scan.py [--jobs N] [--only substr]  -> out/copy_dtor.json
       {"units": {source: {type: [first-use lines]}}, "functions": {"0xaddr": [types]}}
"""
import argparse
import json
import os
import re
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import compile_census as cc  # noqa: E402
import overlay  # noqa: E402

DECOMP = Path(r'E:\Games\fnv_decomp-main')
WARN = re.compile(r"^(?P<file>.*?)\((?P<line>\d+),\d+\) ?: warning: definition of implicit copy "
                  r"(?:constructor|assignment operator) for '(?P<type>[^']+)' is deprecated")
NOTE = re.compile(r"^(?P<file>.*?)\((?P<line>\d+),\d+\) ?: note: in implicit copy "
                  r"(?:constructor|assignment operator) for '(?P<type>[^']+)' first required here")
FNV = re.compile(r'^\s*//\s*@fnv\s+(0x[0-9a-fA-F]+)')


def unit_command(index, unit):
    """(command, source path) as the build compiles the unit: the overlay copy when one compiles, else the decomp."""
    obj = 'NUL'
    rec_p = overlay.overlay_path(index, unit)
    rec_p = rec_p.parent / (rec_p.name + '.overlay.json')
    cmd = cc.command(unit, str(DECOMP), obj)
    src = DECOMP / unit['source']
    if rec_p.exists():
        rec = json.loads(rec_p.read_text())
        if rec.get('ok'):
            src = overlay.overlay_path(index, unit)
            sse = next((c.get('mode') for c in rec.get('changes', []) if c.get('rule') == 'sse'), None)
            extra = ['/I' + str((DECOMP / unit['source']).parent), '/I' + str(overlay.SSE_BLOCK.parent),
                     '/FI' + str(overlay.HELPERS)]
            if sse:
                extra.append('/FI' + str(overlay.SSE_BLOCK))
            if sse == 'types':
                extra.append('/FI' + str(overlay.MSVC_XMM))
            if rec.get('modern'):
                cmd = [c for c in cmd if not c.startswith('-fms-compatibility-version=')]
                extra += overlay.MODERN
            cmd[1:1] = extra
    cmd[-1] = str(src)
    cmd = [c for c in cmd if c not in ('-w', '/c') and not c.startswith('/Fo')]
    cmd += ['/Zs', '-Wdeprecated-copy-with-dtor']
    return cmd, src


def scan_unit(index, unit):
    cmd, src = unit_command(index, unit)
    r = subprocess.run(cmd, capture_output=True, text=True, errors='replace')
    hits = {}
    for l in r.stderr.splitlines():
        m = NOTE.match(l)
        if m and Path(m['file']).name.lower() == src.name.lower():
            hits.setdefault(m['type'], []).append(int(m['line']))
        elif WARN.match(l):
            hits.setdefault(WARN.match(l)['type'], [])
    return unit['source'], src, hits


def functions_of(src):
    """[(address, first line, last line)] of the @fnv functions in a source file (body = up to the next @fnv)."""
    lines = Path(src).read_text(errors='replace').splitlines()
    marks = [(i + 1, int(m.group(1), 16)) for i, l in enumerate(lines) for m in [FNV.match(l)] if m]
    out = []
    for k, (ln, a) in enumerate(marks):
        end = marks[k + 1][0] - 1 if k + 1 < len(marks) else len(lines)
        out.append((a, ln, end))
    return out, lines


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--jobs', type=int, default=os.cpu_count())
    ap.add_argument('--only', default='')
    a = ap.parse_args()
    units = json.loads((DECOMP / 'build/units.json').read_text())['units']
    built = {int(p.name[:4]) for p in (ROOT / 'out/obj').glob('*.obj')}
    todo = [(i, u) for i, u in enumerate(units) if i in built and a.only in u['source']]
    with ThreadPoolExecutor(a.jobs) as ex:
        results = list(ex.map(lambda iu: scan_unit(*iu), todo))
    report_units, funcs = {}, {}
    for source, src, hits in results:
        if not hits:
            continue
        report_units[source] = hits
        fs, lines = functions_of(src)
        for t, first in hits.items():
            short = t.split('::')[-1].split('<')[0]
            pat = re.compile(r'\b' + re.escape(short) + r'\b')
            for addr, lo, hi in fs:
                if any(lo <= f <= hi for f in first) or any(pat.search(lines[j - 1]) for j in range(lo, hi + 1)):
                    funcs.setdefault('0x%08x' % addr, set()).add(t)
    out = dict(units=report_units, functions={k: sorted(v) for k, v in sorted(funcs.items())})
    (ROOT / 'out/copy_dtor.json').write_text(json.dumps(out, indent=1))
    print('%d units copy %d types with user-declared destructors; %d functions flagged' %
          (len(report_units), sum(len(h) for h in report_units.values()), len(funcs)))
    print('0x004bc320 flagged:', '0x004bc320' in funcs)


if __name__ == '__main__':
    main()
