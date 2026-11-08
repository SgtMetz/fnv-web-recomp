#!/usr/bin/env python3
"""Relink census (Phase 0, spike 2).

Resolves every external symbol reference in the decomp's build/units.json to an address, then classifies that
address against the functions the decomp defines and the sections of the original exe:

  resolved      address is the start of a function some unit defines (the linker can alias the name to it)
  import        address is an IAT slot of the exe's import directory (named DLL!function)
  data          address lies in .rdata/.data/.tls/CONST (lives in the data image)
  code-interior address is in .text but not a function start; sub-classified as
                  in-function (inside a defined function body), eh-tail, or gap (out-of-scope code: CRT, thunks, ...)
  other         anything else (outside every section)

Only addresses, names and counts are written (to out/, git-ignored). No bytes of the exe are written.

usage: python tools/relink.py --exe <FalloutNV.exe> [--decomp <decomp folder>] [--out out/relink_census.json]
"""
from decomp_path import DECOMP_DIR
import argparse
import bisect
import json
import struct
import sys
from collections import Counter, defaultdict
from pathlib import Path


class Image:
    def __init__(self, path):
        d = self.data = Path(path).read_bytes()
        pe, = struct.unpack_from('<I', d, 0x3c)
        _, count, _, _, _, opts, _ = struct.unpack_from('<HHIIIHH', d, pe + 4)
        opt = pe + 24
        self.base, = struct.unpack_from('<I', d, opt + 28)
        self.dirs = [struct.unpack_from('<II', d, opt + 96 + i * 8) for i in range(16)]
        self.sections = []
        for i in range(count):
            name, vsize, rva, rsize, raw = struct.unpack_from('<8sIIII', d, opt + opts + i * 40)
            self.sections.append(dict(name=name.rstrip(b'\0').decode(), start=self.base + rva,
                                      end=self.base + rva + max(vsize, rsize), vsize=vsize, rva=rva, raw=raw, rsize=rsize))

    def section_of(self, va):
        for s in self.sections:
            if s['start'] <= va < s['end']:
                return s['name']
        return None

    def off(self, rva):
        for s in self.sections:
            if s['rva'] <= rva < s['rva'] + s['rsize']:
                return s['raw'] + rva - s['rva']
        raise ValueError(hex(rva))

    def cstr(self, rva):
        o = self.off(rva)
        return self.data[o:self.data.index(b'\0', o)].decode('latin1')

    def imports(self):
        """{iat_va: 'dll!name'}"""
        rva, size = self.dirs[1]
        out = {}
        o = self.off(rva)
        while True:
            oft, _, _, name_rva, ft = struct.unpack_from('<IIIII', self.data, o)
            if not name_rva:
                break
            dll = self.cstr(name_rva).lower()
            look = oft or ft
            i = 0
            while True:
                v, = struct.unpack_from('<I', self.data, self.off(look) + i * 4)
                if not v:
                    break
                fn = ('#%d' % (v & 0xffff)) if v & 0x80000000 else self.cstr(v + 2)
                out[self.base + ft + i * 4] = '%s!%s' % (dll, fn)
                i += 1
            o += 20
        return out

    def relocs(self):
        """VAs of every HIGHLOW fixup."""
        rva, size = self.dirs[5]
        o, end, out = self.off(rva), self.off(rva) + size, []
        while o < end:
            page, block = struct.unpack_from('<II', self.data, o)
            if not block:
                break
            for i in range((block - 8) // 2):
                e, = struct.unpack_from('<H', self.data, o + 8 + i * 2)
                if e >> 12 == 3:
                    out.append(self.base + page + (e & 0xfff))
            o += block
        return out

    def u32(self, va):
        for s in self.sections:
            if s['start'] <= va < s['start'] + s['rsize']:
                return struct.unpack_from('<I', self.data, s['raw'] + va - s['start'])[0]
        return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', required=True)
    ap.add_argument('--decomp', default=DECOMP_DIR)
    ap.add_argument('--out', default='out/relink_census.json')
    a = ap.parse_args()

    recipe = json.loads((Path(a.decomp) / 'build/units.json').read_text())
    units = recipe['units']
    img = Image(a.exe)
    imports = img.imports()
    text = next(s for s in img.sections if s['name'] == '.text')

    starts = {}                          # address -> (symbol, size, unit)
    for u in units:
        for f in u['functions']:
            starts[int(f['address'], 16)] = (f['symbol'], f['size'], u['source'])
    order = sorted(starts)
    tails = sorted((int(t['address'], 16), t['size']) for u in units for t in u.get('eh_tails', []))
    tail_starts = [t[0] for t in tails]

    def containing(addr, keys, sizes):
        i = bisect.bisect_right(keys, addr) - 1
        return i >= 0 and addr < keys[i] + sizes(keys[i], i)

    def classify(addr):
        if addr in starts:
            return 'resolved', None
        if addr in imports:
            return 'import', None
        sec = img.section_of(addr)
        if sec == '.text':
            if containing(addr, order, lambda k, i: starts[k][1]):
                return 'code-interior', 'in-function'
            if containing(addr, tail_starts, lambda k, i: tails[i][1]):
                return 'code-interior', 'eh-tail'
            return 'code-interior', 'gap'
        if sec in ('.rdata', '.data', '.tls', 'CONST'):
            return 'data', sec
        return 'other', sec

    refs, distinct = Counter(), defaultdict(set)
    sub = Counter()
    name_conflicts = defaultdict(set)
    imp_used = Counter()
    interior = Counter()
    other_samples = []
    for u in units:
        for name, v in u.get('symbols', {}).items():
            addr = int(v, 0)
            name_conflicts[name].add(addr)
            kind, detail = classify(addr)
            refs[kind] += 1
            distinct[kind].add(addr)
            if detail:
                sub[(kind, detail)] += 1
            if kind == 'import':
                imp_used[imports[addr]] += 1
            if kind == 'code-interior':
                interior[(addr, detail, name)] += 1
            if kind == 'other' and len(other_samples) < 30:
                other_samples.append((hex(addr), name, u['source']))

    gap_addrs = sorted({k[0] for k in interior if k[1] == 'gap'})
    # fixups: how many data words point at code, and of those how many at known function starts
    rel = img.relocs()
    data_to_code = data_to_fn = 0
    data_to_code_nonfn = Counter()
    for va in rel:
        sec = img.section_of(va)
        if sec in ('.rdata', '.data'):
            t = img.u32(va)
            if t is not None and text['start'] <= t < text['end']:
                data_to_code += 1
                if t in starts:
                    data_to_fn += 1
                else:
                    data_to_code_nonfn[classify(t)[1]] += 1

    total_bytes = sum(s for _, s, _ in starts.values())
    report = dict(
        units=len(units), functions=len(starts), function_bytes=total_bytes,
        text_bytes=text['vsize'], text_coverage=round(total_bytes / text['vsize'], 4),
        references=sum(refs.values()), by_kind=dict(refs),
        distinct_by_kind={k: len(v) for k, v in distinct.items()},
        sub_kinds={'%s/%s' % k: v for k, v in sub.items()},
        names_with_multiple_addresses=sum(1 for v in name_conflicts.values() if len(v) > 1),
        imports_in_exe=len(imports), imports_referenced=len(imp_used),
        imports_unreferenced=sorted(set(imports.values()) - set(imp_used)),
        code_interior_distinct=len({k[0] for k in interior}),
        code_interior_gap_distinct=len(gap_addrs),
        relocations_total=len(rel),
        data_words_pointing_into_text=data_to_code, of_which_function_starts=data_to_fn,
        of_which_not_function_starts=dict(data_to_code_nonfn),
        other_samples=other_samples,
        data_range=[hex(min(distinct['data'])), hex(max(distinct['data']))] if distinct['data'] else None,
    )
    out = Path(a.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    detail = dict(report, imports_used=dict(imp_used.most_common()),
                  code_interior=[[hex(k[0]), k[1], k[2], n] for k, n in sorted(interior.items())])
    out.write_text(json.dumps(detail, indent=1))
    print(json.dumps({k: v for k, v in report.items() if k != 'imports_unreferenced'}, indent=1))
    print('imports unreferenced by any unit:', len(report['imports_unreferenced']))


if __name__ == '__main__':
    sys.exit(main())
