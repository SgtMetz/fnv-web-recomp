#!/usr/bin/env python3
"""Find functions whose compiled code changed between two decomp versions (for example after a rename-only update).

Both object folders hold clang objects built by compile_census.py / overlay.py (out/obj layout, NNNN_<stem>.obj, one
object per recipe unit). Each defined function is mapped to its original address through its unit's recipe names. For
every address compiled in both builds the code is compared with relocation fields masked, and each relocation's
target is compared by original address when the recipe knows that name. A rename-only change gives identical code; a
difference means a header or declaration change altered what clang generates (layouts, virtual slots, signatures),
which is where a relink regression comes from. A function that differs can be lifted from the original
(link_build.py lift_replace / FNV_FORCE_LIFT) until its source is fixed.

usage: python tools/decomp_diff.py OLD_OBJS OLD_UNITS_JSON NEW_OBJS NEW_UNITS_JSON [--out out/decomp_diff.json]
"""
import argparse
import json
import struct
import sys
from collections import Counter
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from link_build import CoffPatch  # noqa: E402

REL_TYPES_4 = {0x06, 0x07, 0x0B, 0x14}   # i386 DIR32, DIR32NB, SECREL, REL32: 4-byte fields


def load_units(path):
    units = json.loads(Path(path).read_text())['units']
    by_name = {}
    for u in units:
        for f in u['functions']:
            by_name.setdefault(f['symbol'], int(f['address'], 16))
        for k, v in u.get('symbols', {}).items():
            by_name.setdefault(k, int(v, 0))
    return units, by_name


def functions(args):
    """{address: (masked code bytes, relocation list)} of the recipe functions one object defines."""
    path, own, by_name = args
    p = CoffPatch(Path(path).read_bytes())
    syms = {i: s for i, s in p.symbols()}
    out = {}
    for i, s in syms.items():
        addr = own.get(s['name'])
        if addr is None or s['section'] <= 0 or s['type'] != 0x20:
            continue
        h = 20 + (s['section'] - 1) * 40
        raw_size, raw_ptr, rel_ptr = struct.unpack_from('<III', p.data, h + 16)
        nrel, = struct.unpack_from('<H', p.data, h + 32)
        # the function runs to the next function symbol in its section, or to the section end (/Gy: one per section)
        starts = sorted(x['value'] for x in syms.values() if x['section'] == s['section'] and x['type'] == 0x20)
        end = next((v for v in starts if v > s['value']), raw_size)
        code = bytearray(p.data[raw_ptr + s['value']:raw_ptr + end])
        rels = []
        for k in range(nrel):
            va, idx, typ = struct.unpack_from('<IIH', p.data, rel_ptr + k * 10)
            if not s['value'] <= va < end:
                continue
            o = va - s['value']
            if typ in REL_TYPES_4:
                code[o:o + 4] = b'\0\0\0\0'
            t = syms.get(idx, {}).get('name', '?')
            tgt = by_name.get(t)
            rels.append((o, typ, ('a%x' % tgt) if tgt is not None else ('sec' if t.startswith(('.', '$')) else 'ext')))
        out[addr] = (bytes(code), rels)
    return out


def collect(objdir, units, by_name, jobs):
    work = []
    for p in sorted(Path(objdir).glob('*.obj')):
        i = int(p.name[:4])
        if i < len(units):
            own = {f['symbol']: int(f['address'], 16) for f in units[i]['functions']}
            work.append((str(p), own, by_name))
    res = {}
    with ProcessPoolExecutor(jobs) as ex:
        for part in ex.map(functions, work, chunksize=8):
            res.update(part)
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('old_objs')
    ap.add_argument('old_units')
    ap.add_argument('new_objs')
    ap.add_argument('new_units')
    ap.add_argument('--out', default='out/decomp_diff.json')
    ap.add_argument('--jobs', type=int, default=None)
    a = ap.parse_args()
    ou, on = load_units(a.old_units)
    nu, nn = load_units(a.new_units)
    old = collect(a.old_objs, ou, on, a.jobs)
    new = collect(a.new_objs, nu, nn, a.jobs)
    both = sorted(set(old) & set(new))
    kinds, diff = Counter(), {}
    for addr in both:
        (oc, orl), (nc, nrl) = old[addr], new[addr]
        if oc == nc and orl == nrl:
            kinds['same'] += 1
            continue
        k = 'size' if len(oc) != len(nc) else ('code' if oc != nc else 'reloc-target')
        kinds[k] += 1
        diff['%#010x' % addr] = dict(kind=k, old_size=len(oc), new_size=len(nc))
    report = dict(old_functions=len(old), new_functions=len(new), compared=len(both), kinds=dict(kinds),
                  only_old=len(set(old) - set(new)), only_new=len(set(new) - set(old)), differ=diff,
                  new_only=['%#010x' % a for a in sorted(set(new) - set(old))])
    Path(a.out).write_text(json.dumps(report, indent=1))
    print(json.dumps({k: v for k, v in report.items() if k not in ('differ', 'new_only')}, indent=1))


if __name__ == '__main__':
    main()
