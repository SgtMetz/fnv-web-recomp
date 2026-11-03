#!/usr/bin/env python3
"""Functions whose original code leaves a value in eax / st0 at `ret`, judged from the instruction right before the
epilogue (after pops, `mov esp, ebp`, `leave`, `add esp, N`): a call (the result passes through), or an instruction that
writes eax / pushes an x87 value. Indirect callers (vtables) cannot be seen by tools/ret_users.py; this covers them.

usage: python tools/ret_hint.py  -> out/ret_hint.json {"0x00d5f970": "eax" | "st0"}
"""
import bisect
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DECOMP = Path(r'E:\Games\fnv_decomp-main')
LINE = re.compile(r'^\s*([0-9a-f]+):\s+(\S+)\s*(.*?)\s*(?:<.*>)?\s*$')
NOISE_MN = ('pop', 'leave', 'nop', 'int3')


def producing(mn, ops):
    if mn == 'call':
        return 'eax'
    if mn.startswith('f') and mn in ('fld', 'fld1', 'fldz', 'fild', 'fldpi', 'fldl2e', 'fldln2'):
        return 'st0'
    parts = [p.strip() for p in re.split(r',(?![^\[]*\])', ops)] if ops else []
    dst = parts[0] if parts else ''
    if dst in ('eax', 'ax', 'al') and mn in ('mov', 'movzx', 'movsx', 'lea', 'xor', 'or', 'and', 'add', 'sub', 'sbb', 'neg',
                                              'not', 'shl', 'shr', 'sar', 'imul', 'inc', 'dec', 'cmovne', 'cmove', 'setne', 'sete',
                                              'setl', 'setg', 'setb', 'seta', 'setle', 'setge', 'setbe', 'setae', 'sets', 'setns'):
        return 'eax'
    if mn.startswith('set') and dst in ('al',):
        return 'eax'
    if mn == 'pop' and ops.strip() == 'eax':
        return 'eax'
    return None


def main():
    units = json.loads((DECOMP / 'build/units.json').read_text())['units']
    spans = sorted((int(f['address'], 16), int(f['address'], 16) + f['size']) for u in units for f in u['functions'])
    starts = [s[0] for s in spans]
    lines = []
    for line in (ROOT / 'out/exe_text.asm').read_text(errors='replace').splitlines():
        m = LINE.match(line)
        if m:
            lines.append((int(m.group(1), 16), m.group(2), m.group(3)))
    out = {}
    for i, (a, mn, ops) in enumerate(lines):
        if mn not in ('ret', 'retn'):
            continue
        k = bisect.bisect_right(starts, a) - 1
        if k < 0 or not (spans[k][0] <= a < spans[k][1]):
            continue
        j = i - 1
        while j >= 0:
            m2, o2 = lines[j][1], lines[j][2]
            noise = m2 in NOISE_MN and not (m2 == 'pop' and o2.strip() == 'eax')
            noise = noise or (m2 == 'mov' and re.fullmatch(r'esp, ebp', o2.strip())) or (m2 == 'add' and o2.startswith('esp,')) \
                or (m2 == 'sub' and o2.startswith('esp,') is False and False)
            if not noise:
                break
            j -= 1
        if j < 0 or lines[j][0] < spans[k][0]:
            continue
        p = producing(lines[j][1], lines[j][2])
        if p:
            fn = spans[k][0]
            if out.get(fn) != 'eax':
                out[fn] = p if fn not in out else ('eax' if 'eax' in (p, out[fn]) else p)
    (ROOT / 'out/ret_hint.json').write_text(json.dumps({'0x%08x' % k: v for k, v in sorted(out.items())}))
    print(len(out), 'functions with a value-producing instruction before ret')


if __name__ == '__main__':
    main()
