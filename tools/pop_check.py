#!/usr/bin/env python3
"""Referenced names whose declared stack pop (from the MSVC decoration) differs from the callee's original `ret N`.

Compiled callers assume the callee pops what their declaration says; the original callee pops what its code does. The
decomp tolerates a difference (MSVC restores esp from ebp), clang-compiled callers do not.

usage: python tools/pop_check.py  -> out/pop_mismatch.json {"0xaddr": {"actual": N, "declared": {name: N}}}
"""
from decomp_path import DECOMP_DIR
import bisect
import json
import re
import sys
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import msmangle  # noqa: E402

DECOMP = Path(DECOMP_DIR)
LINE = re.compile(r'^\s*([0-9a-f]+):\s+(\S+)\s*(.*?)\s*(?:<.*>)?\s*$')


def original_pops(units):
    spans = sorted((int(f['address'], 16), f['size']) for u in units for f in u['functions'])
    starts = [s[0] for s in spans]
    pops = defaultdict(set)
    for line in (ROOT / 'out/exe_text.asm').read_text(errors='replace').splitlines():
        m = LINE.match(line)
        if not m or m.group(2) not in ('ret', 'retn'):
            continue
        a = int(m.group(1), 16)
        k = bisect.bisect_right(starts, a) - 1
        if k >= 0 and spans[k][0] <= a < spans[k][0] + spans[k][1]:
            ops = m.group(3).strip()
            pops[spans[k][0]].add(int(ops, 0) if ops else 0)
    return pops


def main():
    units = json.loads((DECOMP / 'build/units.json').read_text())['units']
    pops = original_pops(units)
    out = {}
    stats = defaultdict(int)
    for u in units:
        for name, v in u.get('symbols', {}).items():
            a = int(v, 0)
            if a not in pops or len(pops[a]) != 1:
                continue
            actual = next(iter(pops[a]))
            try:
                declared = msmangle.stack_pop(name)
            except Exception:
                declared = None
            if declared is None:
                stats['unknown'] += 1
                continue
            if declared == actual:
                stats['agree'] += 1
                continue
            stats['disagree'] += 1
            e = out.setdefault('0x%08x' % a, {'actual': actual, 'declared': {}})
            e['declared'][name] = declared
    (ROOT / 'out/pop_mismatch.json').write_text(json.dumps(out, indent=0))
    print(dict(stats), len(out), 'addresses')


if __name__ == '__main__':
    main()
