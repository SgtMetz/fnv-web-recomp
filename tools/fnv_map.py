#!/usr/bin/env python3
"""Map each compiled unit's function definitions to original addresses through the decomp's `// @fnv 0xADDR`
comments, independent of how the name mangles.

Exact recipe names stop matching wherever the port changes a declaration (SSE type unification, overlay renames)
or clang mangles differently from VC2008 (e.g. `AIAU` vs `AAU`). Here every unit is parsed with libclang (the
system LLVM's libclang.dll, the same version as clang-cl) using exactly the unit's clang-cl flags; for each function
definition in the main file, the comment block directly above it gives the address. A definition maps when that
block names exactly one address and the address is one of the unit's recipe functions.

Output: out/fnv_map.json  {unit index: {mangled name: "0xADDR"}} plus a summary on stdout.

usage: python tools/fnv_map.py [--jobs N] [--only TEXT]
"""
from decomp_path import DECOMP_DIR
import argparse
import json
import os
import re
import sys
from collections import Counter
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import compile_census as cc  # noqa: E402
import overlay as ov  # noqa: E402

ROOT = cc.ROOT
DECOMP = Path(DECOMP_DIR)
FNV = re.compile(r'//\s*@fnv\s+(0x[0-9a-fA-F]+)')
KINDS = None


def args_for(index, unit):
    """(source path, libclang args): the overlay copy and its extra flags when the unit went through the overlay."""
    src = ov.overlay_path(index, unit)
    rec_path = Path(str(src) + '.overlay.json')
    sse, modern = None, False
    if rec_path.exists():
        rec = json.loads(rec_path.read_text())
        if not rec.get('ok'):
            return None, None
        sse = next((c.get('mode') for c in rec['changes'] if c['rule'] == 'sse'), None)
        modern = rec.get('modern', False)
    else:
        src = DECOMP / unit['source']
    cmd = cc.command(unit, str(DECOMP), 'NUL')[1:-1]          # drop clang-cl.exe and the source path
    cmd = [c for c in cmd if c not in ('/c',) and not c.startswith('/Fo')]
    extra = ['/I' + str((DECOMP / unit['source']).parent), '/I' + str(ov.SSE_BLOCK.parent)]
    if sse:
        extra.append('/FI' + str(ov.SSE_BLOCK))
    if sse == 'types':
        extra.append('/FI' + str(ov.MSVC_XMM))
    if modern:
        cmd = [c for c in cmd if not c.startswith('-fms-compatibility-version=')]
        extra += ov.MODERN
    return src, ['--driver-mode=cl'] + extra + cmd


def init():
    global KINDS
    import clang.cindex as ci
    ci.Config.set_library_file(r'C:\Program Files\LLVM\bin\libclang.dll')
    K = ci.CursorKind
    KINDS = (K.FUNCTION_DECL, K.CXX_METHOD, K.CONSTRUCTOR, K.DESTRUCTOR, K.CONVERSION_FUNCTION)


def addresses_above(lines, line):
    """@fnv addresses in the comment block directly above 1-based `line` (blank lines allowed)."""
    out, i = [], line - 2
    while i >= 0:
        s = lines[i].strip()
        if not s:
            i -= 1
            continue
        if not s.startswith('//'):
            break
        m = FNV.search(s)
        if m:
            out.append(int(m.group(1), 16))
        i -= 1
    return out


def one(job):
    """Per unit, cached in out/cache/fnv_map/NNNN.json by the unit's object, source and arguments: unchanged units
    are not parsed again."""
    import hashlib
    index, unit = job
    src, args = args_for(index, unit)
    if src is None:
        return index, None, 'not-compiled'
    obj = next((ROOT / 'out/obj').glob('%04d_*.obj' % index), None)
    key = hashlib.sha1((obj.read_bytes() if obj else b'') + Path(src).read_bytes()
                       + json.dumps(args).encode()).hexdigest()
    cache = ROOT / 'out/cache/fnv_map' / ('%04d.json' % index)
    if cache.exists():
        c = json.loads(cache.read_text())
        if c['key'] == key:
            return index, c['mapping'], c['stats']
    index, out, stats = parse_one(index, unit, src, args)
    if out is not None:
        cache.parent.mkdir(parents=True, exist_ok=True)
        cache.write_text(json.dumps(dict(key=key, mapping=out, stats=stats)))
    return index, out, stats


def parse_one(index, unit, src, args):
    import clang.cindex as ci
    try:
        tu = ci.Index.create().parse(str(src), args=args)
    except ci.TranslationUnitLoadError:
        return index, None, 'parse-failed'
    lines = Path(src).read_text(errors='replace').splitlines()
    want = {int(f['address'], 16) for f in unit['functions']}
    src_res = str(Path(src).resolve()).lower()
    out, stats = {}, Counter()
    for c in tu.cursor.walk_preorder():
        if c.kind not in KINDS or not c.is_definition():
            continue
        f = c.location.file
        if f is None or str(Path(f.name).resolve()).lower() != src_res:
            continue
        # a method of a one-line local class (`struct X { void* operator new(...) {...} };`) sits on the line right
        # under another function's @fnv comment; it is nested, so the comment is not its own
        start = c.extent.start
        if '{' in lines[start.line - 1][:start.column - 1]:
            stats['nested-on-line'] += 1
            continue
        # template<> / return-type lines can start before the name; the extent start is the first token
        addrs = addresses_above(lines, start.line)
        if len(addrs) != 1:
            stats['no-single-fnv'] += 1
            continue
        if addrs[0] not in want:
            stats['fnv-not-in-recipe'] += 1
            continue
        out[c.mangled_name] = '0x%08x' % addrs[0]
        stats['mapped'] += 1
    return index, out, dict(stats)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--jobs', type=int, default=os.cpu_count())
    ap.add_argument('--only', default='')
    a = ap.parse_args()
    units = json.loads((DECOMP / 'build/units.json').read_text())['units']
    objs = {int(p.name[:4]) for p in (ROOT / 'out/obj').glob('*.obj')}
    jobs = [(i, u) for i, u in enumerate(units) if i in objs and a.only in u['source']]
    result, totals = {}, Counter()
    with ProcessPoolExecutor(a.jobs, initializer=init) as ex:
        for index, mapping, stats in ex.map(one, jobs, chunksize=4):
            if mapping is None:
                totals[stats] += 1
                continue
            result[index] = mapping
            totals.update(stats)
    # agreement with the recipe where names already match, and how many recipe functions it adds
    agree = disagree = added = 0
    for i, mapping in result.items():
        recipe = {f['symbol']: f['address'] for f in units[i]['functions']}
        for name, addr in mapping.items():
            if name in recipe:
                if int(recipe[name], 16) == int(addr, 16):
                    agree += 1
                else:
                    disagree += 1
        named = {int(v, 16) for v in recipe.values()} & {int(v, 16) for n, v in mapping.items() if n in recipe}
        added += len({int(v, 16) for v in mapping.values()} - named)
    (ROOT / 'out/fnv_map.json').write_text(json.dumps({str(k): v for k, v in result.items()}))
    print(json.dumps(dict(units=len(result), **totals, agree_with_recipe=agree, disagree=disagree,
                          addresses_only_via_map=added), indent=1))


if __name__ == '__main__':
    main()
