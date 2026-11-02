#!/usr/bin/env python3
"""Phase 1: symbol inventory and link plan for the relinked game.

Reads every object in out/obj (one per unit, from compile_census.py / overlay.py) with llvm-nm and classifies:

  defined recipe functions   symbols a unit must define (units.json `functions`); collisions between objects counted
  undefined references, after stripping the overlay's __fnvdupK marker:
    func         bound by the unit's `symbols` map to the start of a recipe function
    func-missing ... whose unit does not compile yet (the link will use a logging trap)
    data         bound to .rdata/.data/.tls/CONST, IAT slots included (absolute symbol at the original address;
                 the loader fills IAT slots and the data image)
    code-other   bound into .text but not a function start (CRT, EH stubs, thunks)
    by-name      not in the recipe, defined by name in some object (inline/COMDAT code, helpers)
    crt          resolved by name from the MSVC/UCRT/Windows libraries
    unresolved   none of the above

usage: python tools/relink_plan.py --exe <FalloutNV.exe> [--out out/relink_plan.json]
"""
import argparse
import bisect
import json
import re
import subprocess
import sys
from collections import Counter, defaultdict
from concurrent.futures import ProcessPoolExecutor, ThreadPoolExecutor
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, r'E:\Games\fnv_decomp-main\tools')
from relink import Image  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
DECOMP = Path(r'E:\Games\fnv_decomp-main')
NM = r'C:\Program Files\LLVM\bin\llvm-nm.exe'
VS = Path(r'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC')
KITS = Path(r'C:\Program Files (x86)\Windows Kits\10\Lib')
DUP = re.compile(r'__fnvdup\d+')
LIBS = ['libcmt.lib', 'libvcruntime.lib', 'libcpmt.lib', 'oldnames.lib', 'libucrt.lib', 'kernel32.lib', 'user32.lib',
        'advapi32.lib', 'gdi32.lib', 'shell32.lib', 'ole32.lib', 'winmm.lib']


def nm(path):
    r = subprocess.run([NM, '--no-sort', str(path)], capture_output=True, text=True, errors='replace')
    defined, undefined = set(), set()
    for line in r.stdout.splitlines():
        parts = line.split()
        if len(parts) == 2 and parts[0] == 'U':
            undefined.add(parts[1])
        elif len(parts) >= 3 and parts[1] in 'TDBRCVtdbr':
            defined.add(parts[2])       # lowercase: static functions/data, globalised by the link step
    return defined, undefined


def library_symbols():
    msvc = sorted(VS.iterdir())[-1] / 'lib/x86'
    sdk = sorted(KITS.iterdir())[-1]
    out = set()
    for lib in LIBS:
        for d in (msvc, sdk / 'ucrt/x86', sdk / 'um/x86'):
            if (d / lib).exists():
                out |= nm(d / lib)[0]
                break
    return out


def live_undefined(path):
    """Undefined names referenced by relocations outside clang's own vftable/RTTI sections. Those sections are
    discarded in the relink (the data image's vftables are used), so names only they reference need no binding."""
    from compare import Coff
    c = Coff(path)
    vsec = {s['section'] for s in c.symbols.values() if s['section'] > 0 and s['name'].startswith(('??_7', '??_R'))}
    live = set()
    for i, sec in enumerate(c.sections):
        if sec is None or i in vsec:
            continue
        for _, si, _ in sec['relocs']:
            if c.symbols[si]['section'] == 0:
                live.add(c.symbols[si]['name'])
    return live


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', required=True)
    ap.add_argument('--out', default='out/relink_plan.json')
    a = ap.parse_args()
    units = json.loads((DECOMP / 'build/units.json').read_text())['units']
    img = Image(a.exe)
    text = next(s for s in img.sections if s['name'] == '.text')

    starts = {int(f['address'], 16): i for i, u in enumerate(units) for f in u['functions']}
    seen = defaultdict(set)      # every name the recipe binds, across all units -> addresses
    for u in units:
        for k, v in u.get('symbols', {}).items():
            seen[k].add(int(v, 0))
        for f in u['functions']:
            seen[f['symbol']].add(int(f['address'], 16))
    global_names = {k: next(iter(v)) for k, v in seen.items() if len(v) == 1}
    objs = {int(p.name[:4]): p for p in (ROOT / 'out/obj').glob('*.obj')}
    compiled = set(objs)
    with ThreadPoolExecutor(16) as ex:
        tables = dict(zip(objs, ex.map(nm, objs.values())))
    with ProcessPoolExecutor(16) as ex:
        live = set().union(*ex.map(live_undefined, objs.values(), chunksize=32))
    libsyms = library_symbols()
    by_name = defaultdict(set)
    for i, (d, _) in tables.items():
        for s in d:
            by_name[s].add(i)

    defined_recipe, collisions = 0, Counter()
    for i in compiled:
        names = {f['symbol'] for f in units[i]['functions']}
        missing = names - tables[i][0]
        defined_recipe += len(names) - len(missing)
        for n in names & tables[i][0]:
            if len(by_name[n]) > 1:
                collisions[n] = len(by_name[n])

    vftables_defined = sum(1 for d, _ in tables.values() for n in d if n.startswith('??_7'))
    kinds, distinct = Counter(), defaultdict(set)
    unresolved = Counter()
    missing_targets = Counter()
    for i in compiled:
        syms = units[i].get('symbols', {})
        own = {f['symbol'] for f in units[i]['functions']}
        for s in tables[i][1]:
            base = DUP.sub('', s)
            if base in own:
                continue
            addr = int(syms[base], 0) if base in syms else global_names.get(base)
            if addr is not None:
                if addr in starts:
                    k = 'func' if starts[addr] in compiled else 'func-missing'
                    if k == 'func-missing':
                        missing_targets[addr] += 1
                elif text['start'] <= addr < text['end']:
                    k = 'code-other'
                elif img.section_of(addr) in ('.rdata', '.data', '.tls', 'CONST'):
                    k = 'data'
                else:
                    k = 'other-addr'
                distinct[k].add(addr)
            elif base in by_name:
                k = 'by-name'
            elif base in libsyms or s in libsyms:
                k = 'crt'
            elif s not in live:
                k = 'vftable-only'      # only clang's discarded vftables/RTTI reference it
            else:
                k = 'unresolved'
                unresolved[base] += 1
            kinds[k] += 1

    missing_units = [i for i in range(len(units)) if i not in compiled]
    report = dict(
        objects=len(compiled), units=len(units),
        recipe_functions_defined=defined_recipe,
        recipe_functions_total=sum(len(u['functions']) for u in units),
        recipe_functions_in_missing_units=sum(len(units[i]['functions']) for i in missing_units),
        recipe_bytes_in_missing_units=sum(f['size'] for i in missing_units for f in units[i]['functions']),
        defined_name_collisions=len(collisions),
        undefined_refs=dict(kinds), undefined_distinct_addresses={k: len(v) for k, v in distinct.items()},
        missing_functions_referenced=len(missing_targets),
        unresolved_distinct=len(unresolved), unresolved_top=unresolved.most_common(40),
        library_symbols=len(libsyms), vftables_defined_by_objects=vftables_defined,
    )
    Path(a.out).write_text(json.dumps(dict(report, collisions=sorted(collisions),
                                           unresolved=sorted(unresolved)), indent=1))
    print(json.dumps(report, indent=1))


if __name__ == '__main__':
    main()
