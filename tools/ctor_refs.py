#!/usr/bin/env python3
"""Compiled functions that skip a constructor the original runs.

The original constructs members and arrays through real constructors: a direct call, or the constructor's address
pushed for the CRT array helper (`push dtor; push ctor; push n; push size; push p; call __ehvec_ctor`). When the
decomp's type has no user constructor, clang generates an implicit one that only stores the vtable. 0xb9a530 (a
render pass group, `BSBatchRenderer__PassList passes[5]`) is the case found: the original constructs the 5 lists
with 0xb988c0 (vtable + head/tail/cache = 0); the clang build left head/count as the bytes of the freed music
file-name table the block reused ("mus_inc_day-059.ogg"), and ResetPassList (0xb63e90) followed `head` into the
string. Lifting such functions runs the original construction.

Flags a compiled (relinked, not lifted) recipe function F when its original references a constructor C (a name at C
starts with `??0`, and C is more than a bare vtable store: original size > 9 bytes) by `call C` or `push C`, and
F's relinked body references C neither by a rel32 call/jump nor as an absolute 32-bit value.

usage: python tools/ctor_refs.py --exe <FalloutNV.exe>  -> out/ctor_refs.json {"0xF": ["0xC", ...]}
(reads the last link: out/link/fnv_game.map, fnv_game.dll, lifted.json)
"""
from decomp_path import DECOMP_DIR
import argparse
import bisect
import json
import re
import struct
import sys
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
from relink import Image  # noqa: E402

DECOMP = Path(DECOMP_DIR)


def relinked_starts():
    """{original address: relinked start} from the map's fnv_f_ symbols, and the sorted relinked starts."""
    out = {}
    for l in (ROOT / 'out/link/fnv_game.map').read_text(errors='replace').splitlines():
        m = re.match(r'\s*0001:[0-9a-f]{8}\s+fnv_f_([0-9a-f]{8})\s+([0-9a-f]{8,16})\s', l)
        if m:
            out[int(m.group(1), 16)] = int(m.group(2), 16)
    return out


def effectful(ctors, spans, base):
    """The constructors whose original stores through a register other than ebp/esp (a member write), or calls one
    that does: a debug-built `return this` (0x6815c0, 1052 skipping callers) changes nothing when skipped."""
    store = re.compile(r'^(mov|movss|movsd|movq|movaps|movups|fstp|fst|and|or)\s+(byte|word|dword|qword|xmmword) ptr '
                       r'\[(?!ebp|esp)')
    body, callees = {}, {}
    cur = None
    line_re = re.compile(r'^\s*([0-9a-f]+):\s+(\S+)\s*(.*?)\s*(?:<.*>)?\s*$')
    starts = sorted(ctors)
    for line in (ROOT / 'out/exe_text.asm').read_text(errors='replace').splitlines():
        m = line_re.match(line)
        if not m:
            continue
        x = int(m.group(1), 16)
        k = bisect.bisect_right(starts, x) - 1
        if k < 0 or not starts[k] <= x < starts[k] + spans[starts[k]]:
            continue
        c = starts[k]
        mn, ops = m.group(2), m.group(3)
        if store.match('%s %s' % (mn, ops)):
            body[c] = True
        if mn == 'call':
            t = re.match(r'0x([0-9a-f]+)', ops)
            if t:
                callees.setdefault(c, set()).add(int(t.group(1), 16))
    out = {c for c in ctors if body.get(c)}
    changed = True
    while changed:
        changed = False
        for c in ctors - out:
            if callees.get(c, set()) & out:
                out.add(c)
                changed = True
    return out


def scan(exe):
    units = json.loads((DECOMP / 'build/units.json').read_text())['units']
    spans = {int(f['address'], 16): f['size'] for u in units for f in u['functions']}
    names = defaultdict(set)
    for u in units:
        for f in u['functions']:
            names[int(f['address'], 16)].add(f['symbol'])
        for n, v in u.get('symbols', {}).items():
            names[int(v, 0)].add(n)
    ctors = {a for a, ns in names.items() if a in spans and spans[a] > 9 and any(n.startswith('??0') for n in ns)}
    img = Image(exe)
    text = next(s for s in img.sections if s['name'] == '.text')
    code = img.data[text['raw']:text['raw'] + text['rsize']]
    base = text['start']
    ctors = effectful(ctors, spans, base)
    lifted = set(json.loads((ROOT / 'out/link/lifted.json').read_text())['lifted'])
    rel = relinked_starts()
    back = {v: k for k, v in rel.items()}
    starts = sorted(back)
    dll = Image(str(ROOT / 'out/link/fnv_game.dll'))
    dtext = next(s for s in dll.sections if s['name'] == '.text')
    dcode = dll.data[dtext['raw']:dtext['raw'] + dtext['rsize']]
    dbase = dtext['start']

    def orig_refs(f):
        out = set()
        o = f - base
        for k in range(o, o + spans[f] - 4):
            if code[k] == 0xE8:
                t = (base + k + 5 + struct.unpack_from('<i', code, k + 1)[0]) & 0xffffffff
            elif code[k] == 0x68:
                t = struct.unpack_from('<I', code, k + 1)[0]
            else:
                continue
            if t in ctors and t != f:
                out.add(t)
        return out

    def relinked_refs(f):
        s = rel[f]
        k = bisect.bisect_right(starts, s)
        e = starts[k] if k < len(starts) else s + 0x1000
        out = set()
        o = s - dbase
        for i in range(o, min(o + (e - s), len(dcode) - 4)):
            v = struct.unpack_from('<I', dcode, i)[0]
            if v in back:
                out.add(back[v])
            if dcode[i] in (0xE8, 0xE9):
                t = (dbase + i + 5 + struct.unpack_from('<i', dcode, i + 1)[0]) & 0xffffffff
                if t in back:
                    out.add(back[t])
        return out

    flagged = {}
    for f in spans:
        if f in lifted or f not in rel:
            continue
        want = orig_refs(f)
        if not want:
            continue
        missing = want - relinked_refs(f)
        if missing:
            flagged[f] = sorted(missing)
    return flagged


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', required=True)
    a = ap.parse_args()
    flagged = scan(a.exe)
    # cumulative: a flagged function is lifted by the next link, so it no longer looks compiled and would drop out
    # (and be compiled again by the link after). Keep earlier entries; delete the file to start over.
    out_p = ROOT / 'out/ctor_refs.json'
    if out_p.exists():
        for k, v in json.loads(out_p.read_text()).items():
            flagged.setdefault(int(k, 16), [int(x, 16) for x in v])
    out_p.write_text(json.dumps({'0x%08x' % f: ['0x%08x' % c for c in cs]
                                                         for f, cs in sorted(flagged.items())}, indent=0))
    print('%d compiled functions skip a constructor their original runs' % len(flagged))
    for f in sorted(flagged)[:30]:
        print('  0x%08x -> %s' % (f, ', '.join('0x%08x' % c for c in flagged[f])))
    print('0xb9a530 flagged:', 0xb9a530 in flagged)


if __name__ == '__main__':
    main()
