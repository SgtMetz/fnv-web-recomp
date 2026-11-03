#!/usr/bin/env python3
"""Compiled recipe functions with a bool/char/short parameter that the original reads as a full dword.

The decomp sometimes declares a pointer/int parameter as `unsigned char` or `bool`; it matched under MSVC because the
original only forwards the stack slot. clang zero-extends the byte, so a forwarded pointer is truncated. Found with
0x00a52b00 (NiBSplineBasisData::CreateClone(CloningProcess&) declared `CreateSplineObj(unsigned char)`): the cloning
pointer reached NiObject::CopyMembers (0x00a5d450) as one byte and a worker thread crashed in the hash insert.

Detection, per definition: stack offsets of the narrow parameters from the decoration (msmangle), then a linear walk of
the original body tracking esp depth (push/pop, sub/add esp, callee `ret N` from the original); a `dword ptr` read of
the slot through esp or an ebp frame flags it. Lifting a false positive only costs speed.

usage: python tools/narrow_params.py  -> prints counts and the first flagged functions
"""
import bisect
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import msmangle  # noqa: E402
from pop_check import original_pops, LINE  # noqa: E402

NARROW = set('CDEFG')
MEM = re.compile(r'dword ptr \[(esp|ebp) \+ (0x[0-9a-f]+|\d+)\]')


def narrow_slots(name):
    """Entry-esp offsets ([esp+N] at the first instruction) of the narrow parameters, or None."""
    r = msmangle.calling(name)
    if r is None:
        return None
    cc, p, member = r
    if cc not in 'ABEFGH':                    # cdecl, thiscall, stdcall; fastcall placement depends on types
        return None
    try:
        sret = p.ret()
        off = 4
        if member and cc not in 'EF':
            off += 4                          # `this` on the stack
        if sret and member:
            off += 4
        elif sret:
            return None
        slots = []
        if p.peek() == 'X':
            return []
        while True:
            c = p.peek()
            if c in ('@', 'Z', ''):
                break
            narrow = c in NARROW or (c == '_' and p.peek(2) == '_N')
            size = p.arg()
            if size is None:
                return None
            if narrow:
                slots.append(off)
            off += max(size, 4)
        return slots
    except msmangle.Fail:
        return None


def scan(units):
    defs = {int(f['address'], 16): (f['symbol'], f['size']) for u in units for f in u['functions']}
    want = {}
    for a, (sym, size) in defs.items():
        s = narrow_slots(sym)
        if s:
            want[a] = (set(s), size)
    pops = original_pops(units)
    starts = sorted(want)
    flagged = {}
    cur, depth, frame, known = None, 0, False, True
    for line in (ROOT / 'out/exe_text.asm').read_text(errors='replace').splitlines():
        m = LINE.match(line)
        if not m:
            continue
        x = int(m.group(1), 16)
        k = bisect.bisect_right(starts, x) - 1
        if k < 0 or not (starts[k] <= x < starts[k] + want[starts[k]][1]):
            cur = None
            continue
        if starts[k] != cur:
            cur, depth, frame, known = starts[k], 0, False, True
        if cur in flagged:
            continue
        mn, ops = m.group(2), m.group(3)
        slots = want[cur][0]
        # reads only: `mov reg, dword ptr [slot]`, `push dword ptr [slot]`, `cmp/test dword ptr [slot], …`. Writes and
        # x87 loads/stores are MSVC reusing the argument slot as scratch.
        reads = ((mn == 'mov' and re.match(r'^e[a-z]{2}, dword ptr', ops)) or mn == 'push' or
                 (mn in ('cmp', 'test') and ops.startswith('dword ptr')))
        for base, n in MEM.findall(ops) if reads else ():
            n = int(n, 0)
            if base == 'ebp' and frame and n - 4 in slots:
                flagged[cur] = (x, line.strip())
            elif base == 'esp' and known and n - depth in slots:
                flagged[cur] = (x, line.strip())
        if mn == 'push':
            depth += 4
            if ops == 'ebp' and depth == 4:
                frame = 'pending'
        elif mn == 'pop':
            depth -= 4
        elif mn == 'mov' and ops == 'ebp, esp' and frame == 'pending':
            frame = True
        elif mn in ('sub', 'add') and ops.startswith('esp, '):
            try:
                v = int(ops[5:], 0)
                depth += v if mn == 'sub' else -v
            except ValueError:
                known = False
        elif mn == 'call':
            t = re.match(r'0x([0-9a-f]+)', ops)
            ps = pops.get(int(t.group(1), 16)) if t else None
            if ps and len(ps) == 1:
                depth -= next(iter(ps))
            else:
                known = False                 # unknown pop (indirect call): stop trusting esp offsets
        elif mn in ('ret', 'retn', 'jmp'):
            pass
    return flagged


def main():
    units = json.loads((Path(r'E:\Games\fnv_decomp-main') / 'build/units.json').read_text())['units']
    flagged = scan(units)
    print('%d functions read a narrow-declared parameter as a dword' % len(flagged))
    for a in sorted(flagged)[:40]:
        print('  0x%08x  %s' % (a, flagged[a][1]))


if __name__ == '__main__':
    main()
