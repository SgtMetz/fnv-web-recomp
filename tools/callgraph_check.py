#!/usr/bin/env python3
"""Call-graph diff between the original exe and the relinked game DLL (Phase 1 verification).

For every relinked function: the direct call/jmp targets in the original (E8/E9 rel32 inside the recipe's function
bounds) against those in fnv_game.dll (inside [start, next relinked start)), mapped back to original addresses
through fnv_addr_table. A relinked function that calls a recipe function its original never calls is a likely
binding error (clang inlining can only remove calls, and calls to CRT/compat/trap code are not compared).

usage: python tools/callgraph_check.py --exe <FalloutNV.exe> [--only 0xADDR,...] [--out out/callgraph_diff.json]
"""
import argparse
import bisect
import json
import struct
import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from relink import Image  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent


def calls(data, base, start, end):
    """Targets of E8/E9 rel32 in [start, end) of a mapped image (naive byte scan; rel32 targets must look sane)."""
    out = set()
    for va in range(start, end - 4):
        op = data(va, 1)[0]
        if op in (0xE8, 0xE9):
            tgt = (va + 5 + struct.unpack('<i', data(va + 1, 4))[0]) & 0xffffffff
            out.add(tgt)
    return out


def reader(img):
    def read(va, n):
        for s in img.sections:
            if s['start'] <= va < s['start'] + s['rsize']:
                o = s['raw'] + va - s['start']
                return img.data[o:o + n]
        return b'\0' * n
    return read


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', required=True)
    ap.add_argument('--dll', default=str(ROOT / 'out/link/fnv_game.dll'))
    ap.add_argument('--only', default='')
    ap.add_argument('--out', default='out/callgraph_diff.json')
    a = ap.parse_args()
    exe, dll = Image(a.exe), Image(a.dll)
    rexe, rdll = reader(exe), reader(dll)
    units = json.loads((Path(r'E:\Games\fnv_decomp-main') / 'build/units.json').read_text())['units']
    size = {int(f['address'], 16): f['size'] for u in units for f in u['functions']}

    # fnv_addr_table: exported data, {original, relinked} pairs until {0, 0}
    exp_rva = dll.dirs[0][0]
    ed = rdll(dll.base + exp_rva, 40)
    nfun, nnames, afun, aname, aord = struct.unpack_from('<IIIII', ed, 20)
    tab_rva = struct.unpack('<I', rdll(dll.base + afun, 4))[0]
    table, p = {}, dll.base + tab_rva
    while True:
        orig, ptr = struct.unpack('<II', rdll(p, 8))
        if not orig:
            break
        table[orig] = ptr
        p += 8
    back = {}
    for o, ptr in table.items():
        back.setdefault(ptr, o)
    ptrs = sorted(back)

    only = {int(x, 16) for x in a.only.split(',') if x}
    suspicious, checked = {}, 0
    for orig, ptr in sorted(table.items()):
        if orig not in size or (only and orig not in only):
            continue
        k = bisect.bisect_right(ptrs, ptr)
        end = ptrs[k] if k < len(ptrs) else ptr + 0x1000
        if end - ptr > 0x10000:
            continue
        o_calls = {t for t in calls(rexe, exe.base, orig, orig + size[orig]) if t in size}
        r_calls = {back[t] for t in calls(rdll, dll.base, ptr, end) if t in back and back[t] in size}
        extra = r_calls - o_calls - {orig}
        checked += 1
        if extra:
            suspicious[hex(orig)] = sorted(hex(x) for x in extra)
    report = dict(checked=checked, suspicious=len(suspicious),
                  top_targets=Counter(t for v in suspicious.values() for t in v).most_common(20),
                  sample=dict(list(suspicious.items())[:30]))
    Path(a.out).write_text(json.dumps(dict(report, all=suspicious), indent=1))
    print(json.dumps(report, indent=1))


if __name__ == '__main__':
    main()
