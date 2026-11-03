#!/usr/bin/env python3
"""Recipe functions whose original code takes an argument in eax/ebx/esi/edi (MSVC's register convention for
file-static helpers, e.g. 0xc7f4a0 out->eax self->esi count->edx). clang compiles the decomp's declaration with a
normal (or its own fastcc) convention, so such a function and its callers must be lifted from the original code.

The check is a straight-line scan from the entry to the first branch/call/ret: a register read before anything in
the function wrote it is an input. Pushes of callee-saved registers (prologue saves) are not reads.

usage: python tools/reg_inputs.py --exe <FalloutNV.exe>   (prints the flagged functions)
"""
import re

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


def inputs(lines, entry):
    """Registers in WATCH that some path from the entry reads before writing (paths end at a call or ret; each
    instruction is visited once)."""
    code = {}
    for line in lines:
        m = lift_stubs.LINE.match(line)
        if m:
            code[int(m.group(1), 16)] = (len(bytes.fromhex(m.group(2))), m.group(3),
                                          lift_stubs.COMMENT.sub('', lift_stubs.SYMBOL.sub('', m.group(4))).strip())
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
            found |= (reads & WATCH) - written
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


if __name__ == '__main__':
    import argparse
    import json
    from pathlib import Path
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', required=True)
    a = ap.parse_args()
    units = json.loads((Path(r'E:\Games\fnv_decomp-main') / 'build/units.json').read_text())['units']
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
