#!/usr/bin/env python3
"""Compiled recipe functions in which clang emitted a trap instruction (int3 / ud2) or that fall off their end.

The decomp is written to make MSVC reproduce the original bytes, and a few functions get there through undefined
behavior (an uninitialized local read, evaluation order pinned with volatile stores; 0xaa1420
NiMemObject::operator new[] after the 2026-11-06 update). Clang folds such paths to `unreachable` and emits int3/ud2,
so the relinked function traps where the original runs, or ends without a return (0xaa1420 runs into the next
section's padding). A trap also follows a call clang believes never returns;
lifting those is harmless (conservative). link_build lifts every function listed here from the original.

usage: python tools/clang_traps.py [--objs out/obj]  -> out/clang_traps.json ["0xaddr", ...]
"""
import argparse
import json
import re
import subprocess
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

from decomp_path import DECOMP_DIR

ROOT = Path(__file__).resolve().parent.parent
OBJDUMP = r'C:\Program Files\LLVM\bin\llvm-objdump.exe'
LABEL = re.compile(r'^[0-9a-f]+ <(.+)>:$')
TRAP = re.compile(r'\t(int3|ud2)\b')


def scan(args):
    path, own = args
    out = subprocess.run([OBJDUMP, '-d', '--no-show-raw-insn', path], capture_output=True, text=True,
                         errors='replace').stdout
    hits, cur, body = set(), None, []

    def close():
        # alignment padding after the body (nop / int3 between functions that share a section) is not code
        while body and (body[-1].startswith('nop') or body[-1] == 'int3'):
            body.pop()
        if cur is None or not body:
            return
        # a trap inside the body, or a body that does not end in a return, jump or call (it falls off its end; a final
        # call is the cookie check / noreturn layout the earlier build already ran)
        if any(m in ('int3', 'ud2') for m in body) or not re.match(r'(ret|jmp|call)', body[-1]):
            hits.add(cur)
    for line in out.splitlines():
        m = LABEL.match(line)
        if m:
            close()
            cur, body = own.get(m.group(1)), []
        elif cur is not None:
            ins = re.match(r'^\s+[0-9a-f]+:\s+(\w+)', line)
            if ins:
                body.append(ins.group(1))
    close()
    return hits


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--objs', default=str(ROOT / 'out/obj'))
    ap.add_argument('--out', default=str(ROOT / 'out/clang_traps.json'))
    a = ap.parse_args()
    units = json.loads((Path(DECOMP_DIR) / 'build/units.json').read_text())['units']
    work = []
    for p in sorted(Path(a.objs).glob('*.obj')):
        i = int(p.name[:4])
        if i < len(units):
            work.append((str(p), {f['symbol']: int(f['address'], 16) for f in units[i]['functions']}))
    found = set()
    with ThreadPoolExecutor() as ex:
        for h in ex.map(scan, work):
            found |= h
    Path(a.out).write_text(json.dumps(['%#010x' % x for x in sorted(found)], indent=0))
    print('%d compiled functions contain int3/ud2' % len(found), '0xaa1420:', 0xaa1420 in found)


if __name__ == '__main__':
    main()
