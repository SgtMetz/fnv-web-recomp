#!/usr/bin/env python3
"""Compiled functions that call a function the decomp declares noreturn, although the original returns.

clang trusts `__declspec(noreturn)` and puts `int3` (or nothing) after the call; when the callee returns, the caller
runs into the trap. Found with 0xa9b4a0 (`vptr = ...; FUN_00aa0dc0();`, declared noreturn; the original tail-jumps to
0xaa0dc0, which returns): a breakpoint exception on the main thread right after player placement.

Scans the relinked DLL for `call rel32` immediately followed by 0xCC inside compiled (not lifted) recipe functions
and flags the caller when the callee is a recipe function (the recorded function sizes can end before the epilogue,
so a `ret` test is unreliable; lifting a caller of a genuinely noreturn function is harmless).
The result is cumulative like ctor_refs.json (a lifted caller no longer looks compiled).

usage: python tools/noreturn_scan.py  -> out/noreturn_callers.json {"0xcaller": ["0xcallee", ...]}
(reads the last link: out/link/fnv_game.map, fnv_game.dll, lifted.json, and out/exe_text.asm)
"""
from decomp_path import DECOMP_DIR
import bisect
import json
import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
from relink import Image  # noqa: E402
from ctor_refs import relinked_starts  # noqa: E402

DECOMP = Path(DECOMP_DIR)


def returning(spans):
    """Original recipe functions with a `ret` before the next function starts (straight from the disassembly)."""
    starts = sorted(spans)
    out = set()
    line_re = re.compile(r'^\s*([0-9a-f]+):\s+(ret|retn)\b')
    for line in (ROOT / 'out/exe_text.asm').read_text(errors='replace').splitlines():
        m = line_re.match(line)
        if m:
            x = int(m.group(1), 16)
            k = bisect.bisect_right(starts, x) - 1
            if k >= 0:      # up to the next function: recorded sizes can stop short of the epilogue (0xaa0dc0)
                out.add(starts[k])
    return out


def main():
    units = json.loads((DECOMP / 'build/units.json').read_text())['units']
    spans = {int(f['address'], 16): f['size'] for u in units for f in u['functions']}
    lifted = set(json.loads((ROOT / 'out/link/lifted.json').read_text())['lifted'])
    rel = relinked_starts()
    back = {v: k for k, v in rel.items()}
    starts = sorted(back)
    dll = Image(str(ROOT / 'out/link/fnv_game.dll'))
    t = next(s for s in dll.sections if s['name'] == '.text')
    code = dll.data[t['raw']:t['raw'] + t['rsize']]
    base = t['start']
    flagged = {}
    for i in range(len(code) - 6):
        if code[i] != 0xE8 or code[i + 5] != 0xCC:
            continue
        tgt = (base + i + 5 + struct.unpack_from('<i', code, i + 1)[0]) & 0xffffffff
        callee = back.get(tgt)
        if callee is None or callee not in spans:
            continue
        k = bisect.bisect_right(starts, base + i) - 1
        if k < 0:
            continue
        caller = back[starts[k]]
        if caller in lifted or caller not in spans:
            continue
        flagged.setdefault(caller, set()).add(callee)
    out_p = ROOT / 'out/noreturn_callers.json'
    if out_p.exists():
        for k_, v in json.loads(out_p.read_text()).items():
            flagged.setdefault(int(k_, 16), set()).update(int(x, 16) for x in v)
    out_p.write_text(json.dumps({'0x%08x' % c: sorted('0x%08x' % x for x in v) for c, v in sorted(flagged.items())},
                                indent=0))
    print('%d compiled functions call a returning function declared noreturn' % len(flagged))
    print('0xa9b4a0 flagged:', 0xa9b4a0 in flagged)


if __name__ == '__main__':
    main()
