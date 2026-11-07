#!/usr/bin/env python3
"""Which original functions return a value (eax / st0) that some caller uses?

The decomp matched the original bytes, but many functions are declared `void` whose original leaves its last call's
result in eax, and a caller (often a function the decomp omits and the lifter reconstructs) reads it. clang does not
preserve that value. This tool reads the original code (llvm-objdump of .text) and, for every direct call, decides
whether the instructions after it read eax/ax/al/ah (or an x87 register) before overwriting it. A call that is
followed directly by `ret` passes the value through: it counts when the containing function's own result is used
(fixpoint over the recipe's function spans).

usage: python tools/ret_users.py --exe <FalloutNV.exe>   -> out/ret_used.json  {"0x00658c10": "eax" | "st0"}
"""
from decomp_path import DECOMP_DIR
import argparse
import bisect
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OBJDUMP = r'C:\Program Files\LLVM\bin\llvm-objdump.exe'
DECOMP = Path(DECOMP_DIR)

EAX = ('eax', 'ax', 'al', 'ah')
LINE = re.compile(r'^\s*([0-9a-f]+):\s+(\S+)\s*(.*?)\s*(?:<.*>)?\s*$')
X87_NO_READ = ('fld', 'fild', 'fldz', 'fld1', 'fldcw', 'fnstcw', 'fninit', 'fldpi', 'fnstsw', 'fnclex', 'fwait', 'fldl2e', 'fldln2')


def regs(text):
    return set(re.findall(r'\b(?:eax|ax|al|ah)\b', text))


def classify(mnem, ops):
    """'read' (eax value consumed), 'write' (clobbered/overwritten), 'stop' (control flow), or '' (irrelevant)."""
    m = mnem
    if m in ('ret', 'retn', 'jmp', 'int3') or (m.startswith('j') and m != 'jmp'):
        return 'stop'
    if m == 'call':
        return 'read' if regs(ops) else 'write'
    if m == 'cdq' or m == 'cwde' or m == 'cwd' or m == 'cbw':
        return 'read'
    if m in ('mul', 'div', 'idiv') or (m == 'imul' and ',' not in ops):
        return 'read'
    if m in ('stosd', 'stosb', 'stosw', 'rep'):
        return 'read'
    if m == 'sahf' or m == 'lahf':
        return 'read'
    parts = [p.strip() for p in re.split(r',(?![^\[]*\])', ops)] if ops else []
    uses = [bool(regs(p)) for p in parts]
    if not any(uses):
        if m == 'pop' and ops == 'eax':
            return 'write'
        return ''
    if m in ('mov', 'movzx', 'movsx', 'lea', 'movsxd'):
        dst = parts[0]
        if dst == 'eax':
            # `mov eax, [eax+4]` reads first
            return 'read' if len(parts) > 1 and regs(parts[1]) else 'write'
        return 'read'
    if m in ('xor', 'sub') and len(parts) == 2 and parts[0] == parts[1] == 'eax':
        return 'write'
    if m == 'pop':
        return 'write' if ops == 'eax' else ''
    return 'read'


def analyse(asm_path, units):
    calls = []                       # (call_addr, target, [following instrs])
    lines = []
    for line in asm_path.read_text(errors='replace').splitlines():
        m = LINE.match(line)
        if m:
            lines.append((int(m.group(1), 16), m.group(2), m.group(3)))
    addrs = [l[0] for l in lines]
    spans = sorted((int(f['address'], 16), int(f['address'], 16) + f['size'])
                   for u in units for f in u['functions'])
    starts = [s[0] for s in spans]
    used = {}                        # target -> 'eax'/'st0'
    tail = []                        # (target, containing function start): call directly followed by ret
    for i, (a, mn, ops) in enumerate(lines):
        if mn != 'call':
            continue
        t = re.match(r'0x([0-9a-f]+)', ops)
        if not t:
            continue
        target = int(t.group(1), 16)
        verdict = None
        for j in range(i + 1, min(i + 14, len(lines))):
            b = lines[j]
            mn2, ops2 = b[1], b[2]
            if mn2.startswith('f') and mn2 not in X87_NO_READ and not mn2.startswith('fl'):
                verdict = 'st0'
                break
            c = classify(mn2, ops2)
            if c == 'read':
                verdict = 'eax'
                break
            if c == 'write':
                break
            if c == 'stop':
                if mn2 in ('ret', 'retn') or mn2 == 'jmp':
                    verdict = 'tail'
                break
        if verdict in ('eax', 'st0'):
            used.setdefault(target, verdict)
        elif verdict == 'tail':
            k = bisect.bisect_right(starts, a) - 1
            if k >= 0 and spans[k][0] <= a < spans[k][1]:
                tail.append((target, spans[k][0]))
    changed = True
    while changed:
        changed = False
        for target, owner in tail:
            if owner in used and target not in used:
                used[target] = used[owner]
                changed = True
    return used


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', required=True)
    ap.add_argument('--out', default=str(ROOT / 'out/ret_used.json'))
    a = ap.parse_args()
    asm = ROOT / 'out/exe_text.asm'
    if not asm.exists() or asm.stat().st_mtime < Path(a.exe).stat().st_mtime:
        with asm.open('w') as f:
            subprocess.run([OBJDUMP, '-d', '--no-show-raw-insn', '--x86-asm-syntax=intel', '--section=.text', a.exe],
                           stdout=f, stderr=subprocess.DEVNULL, check=True)
    units = json.loads((DECOMP / 'build/units.json').read_text())['units']
    used = analyse(asm, units)
    Path(a.out).write_text(json.dumps({'0x%08x' % k: v for k, v in sorted(used.items())}, indent=0))
    print('%d functions have a caller that uses their result' % len(used))


if __name__ == '__main__':
    main()
