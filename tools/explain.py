#!/usr/bin/env python3
"""Everything known about one original address, in one place: owner unit and its compile/overlay state, every
recipe name for it, the build's status (relinked / lifted / trap / CRT), the source line of its @fnv comment,
original callers and callees with their status, and the original next to the relinked disassembly, with call
targets mapped back to original addresses (fnv_f_/fnv_c_/fnv_d_ symbols through out/link/fnv_game.map).

usage: python tools/explain.py --exe <FalloutNV.exe> 0xADDR [--lines N]
       (an address inside a function explains the function holding it)
"""
import argparse
import bisect
import json
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import lift_stubs  # noqa: E402
import reach  # noqa: E402

ROOT = reach.ROOT
LINK = reach.LINK
OBJDUMP = r'C:\Program Files\LLVM\bin\llvm-objdump.exe'


def load_map():
    """[(dll address, symbol)] sorted, from lld's map file."""
    out = []
    for line in (LINK / 'fnv_game.map').read_text(errors='replace').splitlines():
        parts = line.split()
        if len(parts) >= 3 and ':' in parts[0] and re.match(r'^[0-9a-f]{16}$', parts[2]):
            out.append((int(parts[2], 16), parts[1]))
    out.sort()
    return out


def sym_label(sym):
    m = re.match(r'^fnv_[fcdt]_([0-9a-f]{8})', sym)
    return '%s (orig %#010x)' % (sym, int(m.group(1), 16)) if m else sym


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', required=True)
    ap.add_argument('address')
    ap.add_argument('--lines', type=int, default=80)
    a = ap.parse_args()
    p = reach.Program(a.exe)
    x = int(a.address, 16)
    f = p.function_of(x) or x
    end = p.end_of(f)
    print('== %#010x%s: function %#010x..%#010x (%d bytes)' % (x, '' if f == x else ' (inside)', f, end, end - f))

    # names and owner
    names = sorted({k for u in p.units for k, v in u.get('symbols', {}).items() if int(v, 0) == f} |
                   {fn['symbol'] for u in p.units for fn in u['functions'] if int(fn['address'], 16) == f})
    u = p.owner_unit.get(f)
    if u is not None:
        unit = p.units[u]
        rec = Path(ROOT / 'out/src' / ('%04d' % u) / (Path(unit['source']).name + '.overlay.json'))
        state = 'compiled' if u in p.compiled else 'NOT compiled'
        if rec.exists():
            r = json.loads(rec.read_text())
            state += ' (overlay: %s, rules %s)' % ('ok' if r['ok'] else 'failing',
                                                   sorted({c['rule'] for c in r['changes']}))
            if not r['ok'] and r.get('remaining'):
                state += '; first errors: %s' % [e for e, _ in r['remaining'][:3]]
        print('unit      #%d %s: %s' % (u, unit['source'], state))
        src = Path(reach.DECOMP) / unit['source']
        for k, line in enumerate(src.read_text(errors='replace').splitlines(), 1):
            if re.search(r'@fnv\s+0x0*%x\b' % f, line, re.I):
                print('source    %s:%d' % (src, k))
                break
    else:
        print('unit      none (outside the recipe)')
    print('names     %s' % (names[:8] or '-') + (' (+%d more)' % (len(names) - 8) if len(names) > 8 else ''))
    st = p.status(f)
    print('status    %s%s' % (st, ': lift failed: %s' % p.unsupported[f] if f in p.unsupported else ''))

    callers = sorted(p.pred.get(f, ()))
    print('callers   %d: %s' % (len(callers), ', '.join('%#x[%s]' % (c, p.status(c)) for c in callers[:12])))
    callees = sorted(p.succ.get(f, ()))
    print('callees   %d:' % len(callees))
    for c in callees:
        print('            %-60s %s' % (p.label(c)[:60], p.status(c)))

    # original disassembly
    cache = lift_stubs.text_cache(a.exe, (p.text['start'], p.text['end']))
    print('\n-- original %#010x..%#010x' % (f, end))
    for line in cache.lines(f, end)[:a.lines]:
        m = re.search(r'\t(?:call|jmp|j\w+)\s+0x([0-9a-f]+)', line)
        tag = ''
        if m:
            t = int(m.group(1), 16)
            if not f <= t < end:
                tag = '   ; %s [%s]' % (p.label(t).split(' ', 1)[-1][:50], p.status(t))
        print(line.split('<')[0].rstrip() + tag)

    # relinked disassembly
    mp = load_map() if (LINK / 'fnv_game.map').exists() else []
    addrs = [v for v, _ in mp]
    want = 'fnv_f_%08x' % f
    hit = next((k for k, (_, s) in enumerate(mp) if s == want), None)
    if hit is None:
        print('\n-- relinked: %s not in the map' % want)
        return
    lo = mp[hit][0]
    hi = next((v for v, _ in mp[hit + 1:] if v > lo), lo + 0x400)
    print('\n-- relinked %s at %#x..%#x' % (want, lo, hi))
    r = subprocess.run([OBJDUMP, '-d', '-M', 'intel', '--no-show-raw-insn', '--start-address=%#x' % lo,
                        '--stop-address=%#x' % hi, str(LINK / 'fnv_game.dll')], capture_output=True, text=True)
    shown = 0
    for line in r.stdout.splitlines():
        if not re.match(r'^\s+[0-9a-f]+:', line):
            continue
        m = re.search(r'\t(?:call|jmp|j\w+)\s+0x([0-9a-f]+)', line) or \
            re.search(r'(?:ptr \[|, |\t)0x([0-9a-f]{8})\]?', line)
        tag = ''
        if m:
            t = int(m.group(1), 16)
            k = bisect.bisect_right(addrs, t) - 1
            if k >= 0 and not lo <= t < hi and t - mp[k][0] < 0x10000:
                tag = '   ; %s%s' % (sym_label(mp[k][1]), '+%#x' % (t - mp[k][0]) if t != mp[k][0] else '')
        print(line.split('<')[0].rstrip() + tag)
        shown += 1
        if shown >= a.lines:
            break


if __name__ == '__main__':
    main()
