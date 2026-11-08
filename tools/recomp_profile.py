#!/usr/bin/env python3
"""Read a sampling profile of the browser build's main game thread (web/recomp/debug.js profile()) as functions.

Each sample is the call sites on the guest stack, innermost first. The function running is the target of the
innermost call when that is a direct call (E8 rel32); otherwise it is shown as "indirect from <caller>". Prints the
leaf functions (where the time is) and the inclusive totals (time under each function), with decomp names.

usage: python tools/recomp_profile.py <profile.json> [--exe FalloutNV.exe] [--top 40]
"""
import argparse
import bisect
import json
import struct
import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from decomp_path import DECOMP_DIR  # noqa: E402
from relink import Image  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('profile')
    ap.add_argument('--exe', default='E:/Games/Fallout New Vegas/FalloutNV.exe')
    ap.add_argument('--top', type=int, default=40)
    a = ap.parse_args()
    prof = json.loads(Path(a.profile).read_text())
    img = Image(a.exe)
    units = json.loads((Path(DECOMP_DIR) / 'build/units.json').read_text())['units']
    names = {}
    for u in units:
        for f in u['functions']:
            names.setdefault(int(f['address'], 16), f['symbol'])
    starts = sorted(names)
    imports = img.imports()

    def func_of(addr):
        k = bisect.bisect_right(starts, addr) - 1
        return starts[k] if k >= 0 else 0

    def label(addr):
        return '%08x %s' % (addr, names.get(addr, '?')[:90])

    leaf, incl = Counter(), Counter()
    total = 0
    for key, n in prof['stacks'].items():
        sites = [int(x, 16) for x in key.split()] if key else []
        total += n
        if not sites:
            leaf['(no guest stack)'] += n
            continue
        inner = sites[0]
        if (img.u32(inner - 5) or 0) & 0xff == 0xe8:
            callee = (inner + struct.unpack('<i', struct.pack('<I', img.u32(inner - 4)))[0]) & 0xffffffff
            top = label(callee)
        elif ((img.u32(inner - 6) or 0) & 0xffff) == 0x15ff and img.u32(inner - 4) in imports:
            top = 'import ' + imports[img.u32(inner - 4)]
        else:
            top = 'indirect from ' + label(func_of(inner))
        leaf[top] += n
        seen = {top}
        for s in sites:
            f = label(func_of(s))
            if f not in seen:
                seen.add(f)
                incl[f] += n
        for f in seen:
            if f == top:
                incl[f] += n
    print('%d samples' % total)
    print('\n-- leaf (self) --')
    for k, n in leaf.most_common(a.top):
        print('%5.1f%%  %s' % (100.0 * n / total, k))
    print('\n-- inclusive --')
    for k, n in incl.most_common(a.top):
        print('%5.1f%%  %s' % (100.0 * n / total, k))


if __name__ == '__main__':
    main()
