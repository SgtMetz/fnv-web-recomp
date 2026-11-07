#!/usr/bin/env python3
"""Resolve host addresses of the recompiled build to symbols (out/recomp/fnv_recomp.map); f_XXXXXXXX names are the
original function at that address. usage: python tools/recomp_addr.py 0x30001ae4 [...]"""
import bisect
import re
import sys
from pathlib import Path

MAP = Path(__file__).resolve().parent.parent / 'out/recomp/fnv_recomp.map'


def load():
    rows = []
    for line in MAP.read_text(errors='replace').splitlines():
        m = re.match(r'^\s*[0-9a-f]{4}:[0-9a-f]{8}\s+(\S+)\s+([0-9a-f]{8,16})\s', line)
        if m:
            rows.append((int(m.group(2), 16), m.group(1)))
    rows.sort()
    return rows, [a for a, _ in rows]


def name(rows, keys, a):
    k = bisect.bisect_right(keys, a) - 1
    return '%s+%#x' % (rows[k][1], a - rows[k][0]) if k >= 0 else '?'


if __name__ == '__main__':
    rows, keys = load()
    for x in sys.argv[1:]:
        a = int(x, 16)
        print('%08x %s' % (a, name(rows, keys, a)))
