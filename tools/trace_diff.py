#!/usr/bin/env python3
"""Compare two call traces of the recompiled game (FNV_CALL_TRACE builds: u32 function addresses in entry order, main
thread) and show where they first diverge, with the calls leading up to it named from the decomp.

usage: python tools/trace_diff.py out/recomp/trace_native.bin out/recomp/trace_web.bin [--context 40]
"""
import argparse
import array
import bisect
import json
from pathlib import Path

from decomp_path import DECOMP_DIR


def names():
    units = json.loads((Path(DECOMP_DIR) / 'build/units.json').read_text())['units']
    rows = sorted((int(f['address'], 16), f['symbol']) for u in units for f in u['functions'])
    return rows, [a for a, _ in rows]


def name_of(rows, keys, a):
    k = bisect.bisect_right(keys, a) - 1
    if k >= 0 and rows[k][0] == a:
        return rows[k][1][:90]
    return '(inside %s)' % rows[k][1][:70] if k >= 0 else '?'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('a')
    ap.add_argument('b')
    ap.add_argument('--context', type=int, default=40)
    args = ap.parse_args()
    ta, tb = array.array('I'), array.array('I')
    ta.frombytes(Path(args.a).read_bytes())
    tb.frombytes(Path(args.b).read_bytes())
    n = min(len(ta), len(tb))
    i = next((k for k in range(n) if ta[k] != tb[k]), n)
    print('%s: %d calls, %s: %d calls; identical for the first %d' % (args.a, len(ta), args.b, len(tb), i))
    rows, keys = names()
    for k in range(max(0, i - args.context), min(i + 8, max(len(ta), len(tb)))):
        x = ta[k] if k < len(ta) else None
        y = tb[k] if k < len(tb) else None
        mark = '  ' if x == y else '>>'
        print('%s %7d  %-10s %-10s %s' % (mark, k, '%08x' % x if x is not None else '-', '%08x' % y if y is not None else '-',
                                         name_of(rows, keys, x if x is not None else y)))


if __name__ == '__main__':
    main()
