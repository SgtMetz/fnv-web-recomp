#!/usr/bin/env python3
"""One iteration of the Phase 1 loop: overlay -> @fnv source map -> link -> startup run -> blocker summary.

The summary ranks what stops startup: the units (not compiled) whose functions the most initializers or WinMain
reach, with the trap/exception that blocked them. Logs: out/link/cycle_run.log, out/cycle_summary.json.

usage: python tools/cycle.py --exe <FalloutNV.exe> [--skip-overlay] [--timeout SECONDS]
"""
import argparse
import json
import re
import subprocess
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
VENV_PY = ROOT / 'local/venv/Scripts/python.exe'


def step(name, cmd, **kw):
    import time
    print('== %s' % name, flush=True)
    t0 = time.perf_counter()
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, errors='replace', **kw)
    print('   %s took %.1fs' % (name, time.perf_counter() - t0), flush=True)
    if r.returncode:
        print(r.stdout[-3000:], r.stderr[-3000:])
        raise SystemExit('%s failed' % name)
    return r.stdout


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', required=True)
    ap.add_argument('--skip-overlay', action='store_true')
    ap.add_argument('--timeout', type=int, default=300)
    a = ap.parse_args()
    if not a.skip_overlay:
        out = json.loads(step('overlay', [sys.executable, 'tools/overlay.py']))
        print('   overlay: %d of %d now compile' % (out['now_compile'], out['attempted']))
        step('fnv_map', [str(VENV_PY), 'tools/fnv_map.py'])
    step('link', [sys.executable, 'tools/link_build.py', '--exe', a.exe])
    rep = json.loads((ROOT / 'out/link/link_report.json').read_text())
    print('   link exit %s, %d functions, %d traps' % (rep['link_exit'], rep['addr_table'], rep['traps']))
    if rep['link_exit'] or rep.get('loader', {}).get('exit'):
        raise SystemExit('link failed: %s' % rep['other_errors'][:3])

    print('== run', flush=True)
    log = ROOT / 'out/link/cycle_run.log'
    # OpenAI model: stream startup output to disk so a waiting process can be diagnosed without stopping it.
    with log.open('w', encoding='utf-8', errors='replace') as stream:
        try:
            r = subprocess.run([str(ROOT / 'out/link/FalloutNV_relinked.exe'), '--exe', a.exe], cwd=ROOT / 'out/link',
                               stdout=stream, stderr=subprocess.STDOUT, timeout=a.timeout)
            code = r.returncode
        except subprocess.TimeoutExpired:
            code = 'timeout'
    text = log.read_text(encoding='utf-8', errors='replace')

    units = json.loads((Path(r'E:\Games\fnv_decomp-main') / 'build/units.json').read_text())['units']
    owner = {int(f['address'], 16): u['source'] for u in units for f in u['functions']}
    objs = {int(p.name[:4]) for p in (ROOT / 'out/obj').glob('*.obj')}
    compiled = {units[i]['source'] for i in objs}
    lines = text.splitlines()
    blockers, exceptions = Counter(), Counter()
    after_winmain = None
    for k, l in enumerate(lines):
        m = re.search(r'unbound 0x([0-9a-f]{8})', l)
        if '[init-blocked]' in l and m:
            blockers[int(m.group(1), 16)] += 1
        elif '[init-blocked]' in l and 'exception' in l:
            nxt = lines[k + 1] if k + 1 < len(lines) else ''
            f = re.search(r'fnv_f_([0-9a-f]{8})', nxt)
            exceptions[(re.search(r'exception (0x[0-9a-f]+)', l).group(1), f.group(1) if f else '?')] += 1
        if 'calling WinMain' in l:
            after_winmain = lines[k + 1:k + 8]
    by_unit = Counter()
    for addr, n in blockers.items():
        by_unit[owner.get(addr, '(not a recipe function: %#x)' % addr)] += n
    summary = dict(exit=code, init=[l for l in lines if l.startswith('[loader] initializers')],
                   blocked_by_unit=[[u, n, 'compiled' if u in compiled else 'not compiled'] for u, n in
                                    by_unit.most_common(25)],
                   init_exceptions=[[c, f, n] for (c, f), n in exceptions.most_common(10)],
                   winmain=after_winmain)
    (ROOT / 'out/cycle_summary.json').write_text(json.dumps(summary, indent=1))
    print(json.dumps(summary, indent=1))


if __name__ == '__main__':
    main()
