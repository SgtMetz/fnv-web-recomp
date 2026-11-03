#!/usr/bin/env python3
"""Map relinked DLL addresses (hex, from the loader log) to function names: python tools/addr2fn.py 204fada2 ..."""
import bisect
import re
import sys
from pathlib import Path

rows = []
for l in (Path(__file__).resolve().parent.parent / 'out/link/fnv_game.map').read_text(errors='replace').splitlines():
    m = re.match(r'\s*(?:\d+:)?\s*0001:[0-9a-f]{8}\s+(\S+)\s+([0-9a-f]{8,16})\s', l)
    if m:
        rows.append((int(m.group(2), 16), m.group(1)))
rows.sort()
keys = [r[0] for r in rows]
for a in sys.argv[1:]:
    v = int(a, 16)
    k = bisect.bisect_right(keys, v) - 1
    print('%08x  %s +0x%x' % (v, rows[k][1][:90], v - rows[k][0]) if k >= 0 else a)
