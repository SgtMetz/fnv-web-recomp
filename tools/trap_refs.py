#!/usr/bin/env python3
"""Which relinked functions reference names nothing defines (name-only trap stubs in out/link/traps.cpp).

A compiled body that calls such a name skips that call at run time (the soft trap returns), so the code after it
works on state the call should have set up. Typical names are synthetic destructors and specializations of the
decomp's own model types: `delete pChanges` -> `??1InventoryChanges@...` (0x431890), a `NiPointer__fnvspec1`
constructor that leaves a NiPointer uninitialised (0x441c50). The original calls the real code, so
tools/link_build.py lifts these functions.

Works on the patched objects of the last link (out/link/obj/*.obj) and out/link/provided.json, so the result is a
generated input like out/pop_mismatch.json: run it after a link, then relink.
usage: python tools/trap_refs.py   -> out/trap_refs.json {"0xaddr": [names]}
"""
import json
import re
import struct
import sys
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LINK = ROOT / 'out/link'
FUNC = re.compile(r'^fnv_f_([0-9a-f]{8})(?:__fnvreplaced\d+)?$')     # replaced (lifted) bodies keep counting


def object_refs(path, wanted):
    """{function address: set(names in `wanted`)} for the function sections of one COFF object."""
    sys.path.insert(0, str(ROOT / 'tools'))
    from link_build import CoffPatch
    data = Path(path).read_bytes()
    p = CoffPatch(data)
    syms = dict(p.symbols())
    owners = defaultdict(set)
    starts = defaultdict(list)          # per section: (offset, is EH funclet) of every function symbol
    for s in syms.values():
        m = FUNC.match(s['name'])
        if m and s['section'] > 0:
            owners[s['section']].add(int(m.group(1), 16))
        if s['section'] > 0 and s['type'] == 0x20:
            starts[s['section']].append((s['value'], s['name'].startswith(('?dtor$', '?catch$', '?fin$'))))
    out = defaultdict(set)
    for sec, funcs in owners.items():
        h = 20 + (sec - 1) * 40
        relptr, = struct.unpack_from('<I', data, h + 24)
        count, = struct.unpack_from('<H', data, h + 32)
        marks = sorted(starts[sec])
        for k in range(count):
            off, sym, _ = struct.unpack_from('<IIH', data, relptr + k * 10)
            name = syms[sym]['name'] if sym in syms else None
            if name in wanted:
                # an unwind funclet (`?dtor$N@...`) only runs while an exception propagates: mark those references
                eh = next((e for v, e in reversed(marks) if v <= off), False)
                for f in funcs:
                    out[f].add(name + (' [eh]' if eh else ''))
    return out


def scan(link=LINK):
    provided = json.loads((link / 'provided.json').read_text())
    wanted = {sym for sym, (addr, _) in provided['traps'].items() if not addr}
    refs = defaultdict(set)
    for obj in sorted((link / 'obj').glob('*.obj')):
        for f, names in object_refs(obj, wanted).items():
            refs[f] |= names
    return refs


if __name__ == '__main__':
    refs = scan()
    (ROOT / 'out/trap_refs.json').write_text(json.dumps({'0x%08x' % f: sorted(n) for f, n in sorted(refs.items())},
                                                        indent=0))
    names = defaultdict(set)
    for f, ns in refs.items():
        for n in ns:
            names[n].add(f)
    print(len(refs), 'functions reference', len(names), 'name-only trap names')
    for f in (0x441c50, 0x431890, 0x4786e0):
        print(hex(f), sorted(refs.get(f, ())))
    kinds = defaultdict(int)
    for n in names:
        kinds['dtor' if n.startswith('??1') else 'ctor' if n.startswith('??0') else 'spec' if 'fnvspec' in n else 'other'] += 1
    print(dict(kinds))
