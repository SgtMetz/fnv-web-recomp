#!/usr/bin/env python3
"""Dump the first errors of every unit the overlay still cannot compile, with the offending source line, to
out/overlay_errors.txt, plus a cluster summary (first error message, normalised) to stdout. Used to pick the next
overlay rule.

usage: python tools/overlay_errors.py [--per-unit N]
"""
import argparse
import json
import re
import sys
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import overlay as o  # noqa: E402


def norm(msg):
    return re.sub(r'\b\d+\b', 'N', re.sub(r"'[^']*'", "'X'", msg))[:100]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--per-unit', type=int, default=3)
    a = ap.parse_args()
    units = json.loads((o.DECOMP / 'build/units.json').read_text())['units']
    still = set(json.loads((o.ROOT / 'out/overlay_report.json').read_text())['still_failing_units'])
    todo = [(i, u) for i, u in enumerate(units) if u['source'] in still]

    def run(iu):
        i, u = iu
        p = o.overlay_path(i, u)
        rec = json.loads(Path(str(p) + '.overlay.json').read_text())
        mode = next((c.get('mode') for c in rec['changes'] if c['rule'] == 'sse'), None)
        ok, diags = o.compile_unit(u, p, mode, modern=rec.get('modern', False))
        lines = p.read_text(errors='replace').splitlines()
        errs = []
        for d in diags:
            if d['kind'] != 'error':
                continue
            src = lines[d['line'] - 1].strip() if o.same_file(d['file'], p) and d['line'] <= len(lines) else \
                '(in %s)' % Path(d['file']).name
            errs.append((d['msg'], src))
        return u['source'], mode, errs

    with ThreadPoolExecutor(16) as ex:
        res = list(ex.map(run, todo))
    first, units_per = Counter(), Counter()
    out = []
    for src, mode, errs in sorted(res):
        out.append('== %s [sse=%s] %d errors' % (src, mode, len(errs)))
        for msg, line in errs[:a.per_unit]:
            out.append('   %s\n      | %s' % (msg[:160], line[:160]))
        if errs:
            first[norm(errs[0][0])] += 1
        for m in {norm(e[0]) for e in errs}:
            units_per[m] += 1
    (o.ROOT / 'out/overlay_errors.txt').write_text('\n'.join(out) + '\n')
    print('units:', len(res))
    print('-- by first error'); [print('%4d %s' % (n, m)) for m, n in first.most_common(20)]
    print('-- units containing'); [print('%4d %s' % (n, m)) for m, n in units_per.most_common(20)]


if __name__ == '__main__':
    main()
