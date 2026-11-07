#!/usr/bin/env python3
"""Recipe functions whose original code takes an argument in eax/ebx/esi/edi (MSVC's register convention for
file-static helpers, e.g. 0xc7f4a0 out->eax self->esi count->edx). clang compiles the decomp's declaration with a
normal (or its own fastcc) convention, so such a function and its callers must be lifted from the original code.

The check is a straight-line scan from the entry to the first branch/call/ret: a register read before anything in
the function wrote it is an input. Pushes of callee-saved registers (prologue saves) are not reads.

usage: python tools/reg_inputs.py --exe <FalloutNV.exe>   (prints the flagged functions)
"""
from decomp_path import DECOMP_DIR
import re
from collections import defaultdict

import lift_stubs

ALIAS = {}
for r in ('eax', 'ebx', 'ecx', 'edx'):
    ALIAS[r] = r
    ALIAS[r[1:]] = r                  # ax
    ALIAS[r[1] + 'l'] = r             # al
    ALIAS[r[1] + 'h'] = r             # ah
for r in ('esi', 'edi', 'ebp', 'esp'):
    ALIAS[r] = r
    ALIAS[r[1:]] = r
WATCH = {'eax', 'ebx', 'esi', 'edi'}
REG = re.compile(r'\b(' + '|'.join(sorted(ALIAS, key=len, reverse=True)) + r')\b')
PURE_WRITE = {'mov', 'lea', 'movzx', 'movsx', 'pop', 'movd', 'movss', 'cvttss2si', 'cvtss2si'}


def regs(text):
    return {ALIAS[m] for m in REG.findall(text)}


def split_ops(ops):
    out, depth, cur = [], 0, ''
    for ch in ops:
        if ch == '[':
            depth += 1
        elif ch == ']':
            depth -= 1
        if ch == ',' and depth == 0:
            out.append(cur.strip())
            cur = ''
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


def effect(mnem, ops):
    """(registers read, registers written) by one instruction (32-bit names)."""
    parts = split_ops(ops)
    reads, writes = set(), set()
    if mnem == 'push':
        if parts and parts[0] not in ('ebx', 'esi', 'edi', 'ebp'):     # pushes of callee-saved registers are saves
            reads |= regs(parts[0])
    elif mnem == 'fnstsw' and ops == 'ax':
        writes.add('eax')
    elif mnem in ('cdq', 'cwde'):
        reads.add('eax')
        writes |= {'edx'} if mnem == 'cdq' else {'eax'}
    elif mnem in ('mul', 'div', 'idiv') or (mnem == 'imul' and len(parts) == 1):
        reads |= {'eax'} | regs(parts[0] if parts else '')
        writes |= {'eax', 'edx'}
    elif parts:
        dst, srcs = parts[0], parts[1:]
        for s_ in srcs:
            reads |= regs(s_)
        if '[' in dst:
            reads |= regs(dst)
        elif mnem in PURE_WRITE or mnem.startswith('set'):
            writes |= regs(dst)
        elif (mnem == 'or' and srcs == ['-0x1']) or (mnem == 'and' and srcs == ['0x0']):
            writes |= regs(dst)
        elif mnem in ('xor', 'sub', 'sbb') and len(srcs) == 1 and regs(srcs[0]) == regs(dst) and '[' not in srcs[0]:
            writes |= regs(dst)
            reads -= regs(dst)
        elif mnem in ('cmp', 'test') or not mnem.startswith('f'):
            reads |= regs(dst)
            if mnem not in ('cmp', 'test'):
                writes |= regs(dst)
    return reads, writes


def decode(lines):
    code = {}
    for line in lines:
        m = lift_stubs.LINE.match(line)
        if m:
            code[int(m.group(1), 16)] = (len(bytes.fromhex(m.group(2))), m.group(3),
                                          lift_stubs.COMMENT.sub('', lift_stubs.SYMBOL.sub('', m.group(4))).strip())
    return code


def inputs(lines, entry, watch=WATCH):
    """Registers in `watch` that some path from the entry reads before writing (paths end at a call or ret; each
    instruction is visited once). `push ecx` is not a read: MSVC uses it to reserve a 4-byte local."""
    return code_inputs(decode(lines), entry, watch)


def code_inputs(code, entry, watch):
    found, seen, todo = set(), set(), [(entry, frozenset())]
    while todo:
        a, written = todo.pop()
        while a in code and a not in seen:
            seen.add(a)
            n, mnem, ops = code[a]
            if mnem in ('call', 'ret', 'int3', 'hlt', 'ud2') or mnem.startswith(('loop', 'rep')):
                break
            if mnem.startswith('j'):
                try:
                    tgt = int(ops.split()[0], 16)
                except ValueError:
                    break                                   # indirect jump
                if mnem == 'jmp':
                    a = tgt
                    continue
                todo.append((tgt, written))
                a += n
                continue
            reads, writes = effect(mnem, ops)
            if mnem == 'push' and ops == 'ecx':
                reads = set()
            found |= (reads & watch) - written
            written = written | writes
            a += n
    return found


def flagged(exe, text_range, spans):
    """{address: sorted input registers} over `spans` ({address: size})."""
    cache = lift_stubs.text_cache(exe, text_range)
    out = {}
    for a, size in spans.items():
        r = inputs(cache.lines(a, a + size), a)
        if r:
            out[a] = sorted(r)
    return out


def passes_ecx(name):
    """True when a call through this decorated name loads ECX (thiscall member or fastcall), False when it cannot
    (free/static __cdecl/__stdcall, extern "C"), None when the name does not say."""
    if name.startswith('@'):
        return True                                   # extern "C" __fastcall
    if re.match(r'^_\w+(@\d+)?$', name):
        return False                                  # extern "C" __cdecl / __stdcall
    import msmangle
    r = msmangle.calling(name)
    if r is None:
        return None
    return r[0] in 'EFIJ'


def passes_edx(name):
    """True when a call through this decorated name loads EDX: __fastcall with a second register argument (a member
    __fastcall's first parameter, or two dword parameters of a free one). False otherwise, None when unknown."""
    m = re.match(r'^@\w+@(\d+)$', name)
    if m:
        return int(m.group(1)) >= 8
    if re.match(r'^_\w+(@\d+)?$', name):
        return False
    import msmangle
    r = msmangle.calling(name)
    if r is None:
        return None
    cc, p, member = r
    if cc not in 'IJ':
        return False
    try:
        p.ret()
        if p.peek() == 'X':
            return False
        n = 1 if member else 0
        while p.peek() not in ('@', 'Z', ''):
            if p.arg() != 4:
                return None
            n += 1
            if n >= 2:
                return True
        return False
    except msmangle.Fail:
        return None


def preserved_across_calls(exe, text_range, units, watch=('ecx', 'edx')):
    """Callees whose original callers read ECX/EDX right after the call, before writing it: the caller relies on the
    callee leaving that volatile register alone (MSVC LTCG knows the callee's register use). 0xd142b0
    (hkp3AxisSweep::addObject) sets EDX = node index, calls 0xd10b30, then stores `handle->m_id = edx`; the clang
    build of 0xd10b30 clobbers EDX, so m_id became a leftover pointer and the broadphase later indexed with it
    (0xd12040). Lifted callees keep the original register behaviour. A callee that returns a 64-bit value in EDX:EAX
    is also flagged, which only costs speed.

    Returns {callee: sorted registers} over direct calls in recipe functions (straight-line scan from the return
    address to the next branch/call/ret)."""
    cache = lift_stubs.text_cache(exe, text_range)
    spans = {int(f['address'], 16): f['size'] for u in units for f in u['functions']}
    out = defaultdict(set)
    for f, size in spans.items():
        code = decode(cache.lines(f, f + size))
        for a in sorted(code):
            n, mnem, ops = code[a]
            if mnem != 'call':
                continue
            try:
                t = int(ops.split()[0], 16)
            except ValueError:
                continue
            if t not in spans:
                continue
            written, x = set(), a + n
            while x in code:
                k, mn, op = code[x]
                if mn in ('call', 'ret', 'int3', 'hlt', 'ud2') or mn.startswith(('j', 'loop', 'rep')):
                    break
                r, w = effect(mn, op)
                if mn == 'push' and op == 'ecx':
                    r = set()                         # `push ecx` reserves a slot (`push ecx; fstp [esp]`)
                hit = (r & set(watch)) - written
                if hit:
                    out[t] |= hit
                written |= w
                if written >= set(watch):
                    break
                x += k
    return {t: sorted(r) for t, r in out.items()}


def ecx_free_callers(exe, text_range, units):
    return reg_free_callers(exe, text_range, units, 'ecx', passes_ecx)


def edx_free_callers(exe, text_range, units):
    """EDX-input functions reached through a name that does not load EDX (0xdae220, a Havok manifold helper taking
    the manifold in EDX, called by 0xdaf1e0 through a cdecl name: the crash read [0+3] after New Game)."""
    return reg_free_callers(exe, text_range, units, 'edx', passes_edx)


def reg_free_callers(exe, text_range, units, reg, passes):
    """ECX-input functions reached through a name that does not load ECX (0x9fdf20 CAsyncRequest::Complete is
    thiscall, but 0x9fdfe0's unit calls it as `int __stdcall FUN_009fdf20(void)`; the original caller relies on
    ECX = &request surviving the previous call, an LTCG convention). clang never loads ECX for such a call.

    Returns (callees, sites): callees {address: definition passes ECX?} whose original reads ECX before writing it,
    sites [(caller, callee, original caller wrote ECX since its previous call)] for direct calls whose caller's unit
    names the callee only through ECX-less names."""
    cache = lift_stubs.text_cache(exe, text_range)
    spans, defsym, unit_of = {}, {}, {}
    for k, u in enumerate(units):
        for f in u['functions']:
            a = int(f['address'], 16)
            spans[a], defsym[a], unit_of[a] = f['size'], f['symbol'], k
    code = {a: decode(cache.lines(a, a + s)) for a, s in spans.items()}
    reads = {a for a in spans if code_inputs(code[a], a, {reg})}
    names = [defaultdict(set) for _ in units]                 # per unit: callee address -> names it uses
    for k, u in enumerate(units):
        for n, v in u.get('symbols', {}).items():
            if int(v, 0) in reads:
                names[k][int(v, 0)].add(n)
    sites = []
    for f, c in code.items():
        set_ecx = False
        for a in sorted(c):
            n, mnem, ops = c[a]
            if mnem == 'call':
                try:
                    t = int(ops.split()[0], 16)
                except ValueError:
                    t = None
                if t in reads and t != f:
                    used = names[unit_of[f]].get(t) or ({defsym[t]} if unit_of.get(t) == unit_of[f] else set())
                    kinds = {passes(x) for x in used}
                    if False in kinds:
                        sites.append((f, t, set_ecx))
                set_ecx = False                               # ECX/EDX are volatile across calls
                continue
            if reg in effect(mnem, ops)[1]:
                set_ecx = True
    callees = {t: passes(defsym[t]) for _, t, _ in sites}
    return callees, sites


if __name__ == '__main__':
    import argparse
    import json
    from pathlib import Path
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', required=True)
    ap.add_argument('--ecx', action='store_true', help='ECX-input callees reached through ECX-less names')
    a = ap.parse_args()
    units = json.loads((Path(DECOMP_DIR) / 'build/units.json').read_text())['units']
    if a.ecx:
        callees, sites = ecx_free_callers(a.exe, (0x00401000, 0x00FDE58B), units)
        print(len(callees), 'callees,', len({s[0] for s in sites}), 'callers,', len(sites), 'sites;',
              sum(1 for s in sites if not s[2]), 'sites where the original caller does not set ECX first;',
              sum(1 for v in callees.values() if v is False), 'callees defined without ECX')
        for f, t, w in sorted(sites)[:400]:
            print('%08x -> %08x %s def-ecx=%s' % (f, t, 'sets-ecx' if w else 'INHERITS-ECX', callees[t]))
        raise SystemExit
    spans = {int(f['address'], 16): f['size'] for u in units for f in u['functions']}
    f = flagged(a.exe, (0x00401000, 0x00FDE58B), spans)
    print(len(f), 'of', len(spans))
    for k in (0xc7f4a0, 0xcb2550, 0xc7f4e0):
        print(hex(k), f.get(k))
    from collections import Counter
    print(Counter(tuple(v) for v in f.values()).most_common(10))
    import random
    random.seed(1)
    for k in random.sample(sorted(k for k, v in f.items() if v == ['eax']), 6):
        print(hex(k))
