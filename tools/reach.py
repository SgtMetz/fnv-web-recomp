#!/usr/bin/env python3
"""Phase 1 worklist in one pass: everything startup can reach in the original program that the build does not
provide yet, instead of finding one blocker per run.

The original call graph (machine code is the authority, rule 4) comes from the cached whole-.text disassembly
(tools/lift_stubs.py TextCache): direct call/jmp/jcc targets, plus code addresses the code itself references (the
exe's .reloc fixups inside .text: callbacks, WndProcs, thread entries). Function bounds are the recipe's; code
outside every recipe function (CRT, gaps) is cut at the next known entry. Roots:

  tier A   WinMain and the __xc initializer table (what runs at startup, through direct calls)
  tier B   additionally every code address stored in .rdata/.data (vftables, callback tables): a conservative
           bound on what virtual and indirect calls can reach

Each reachable address gets the build's status from out/link/provided.json and out/link/lifted.json (written by
tools/link_build.py): relinked, lifted, crt (bound to the modern CRT: not expanded further), or a problem:
  trap-unit-not-compiled   recipe function whose unit does not compile (and the lifter could not stand in)
  trap-not-emitted         recipe function in a compiled unit that clang did not emit, not liftable
  trap-code                other code the relinked objects call (fnv_c_ trap)
  lift-unbound             code pointer in lifted code with no binding (lift_trap_)
  loader-thunk-recipe/gap  code address stored in data that nothing built defines (the loader's trap thunk)
A problem's lift failure reason (unsupported instruction) is attached when the lifter tried it. Problems are
expanded too, so the worklist also covers what lies behind them. `Program` is shared with tools/explain.py.

Output: out/reach.json (summary, per-unit worklist, every problem with depth and a root path) and a summary on stdout.

usage: python tools/reach.py --exe <FalloutNV.exe> [--out out/reach.json]
"""
import argparse
import array
import bisect
import json
import pickle
import re
import sys
from collections import Counter, defaultdict, deque
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, r'E:\Games\fnv_decomp-main\tools')
import lift_stubs  # noqa: E402
from relink import Image  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
DECOMP = Path(r'E:\Games\fnv_decomp-main')
LINK = ROOT / 'out/link'
WINMAIN = 0x0086A850
XC_TABLE = (0x00FDF4A4, 0x010113C0)
CRT_REGION = (0x00EC3000, 0x00EDE000)
PROBLEMS = ('trap-unit-not-compiled', 'trap-not-emitted', 'trap-code', 'lift-unbound', 'loader-thunk-recipe',
            'loader-thunk-gap')
BRANCH = re.compile(rb'^ *([0-9a-f]+):[^\t\n]*\t(call|jmp|j[a-z]{1,3})\s+0x([0-9a-f]+)', re.M)


def branches(exe, text):
    """(sources, targets, is_call) arrays of every direct branch in .text, cached next to the disassembly."""
    cache = lift_stubs.text_cache(exe, (text['start'], text['end']))
    path = cache.dis.with_suffix('.branches.pickle')
    if path.exists():
        return pickle.loads(path.read_bytes())
    src, tgt, call = array.array('I'), array.array('I'), array.array('B')
    for m in BRANCH.finditer(cache.dis.read_bytes()):
        src.append(int(m.group(1), 16))
        tgt.append(int(m.group(3), 16))
        call.append(m.group(2) == b'call')
    out = (src, tgt, call)
    path.write_bytes(pickle.dumps(out))
    return out


class Program:
    """The original program's functions and call graph, and the current build's status for each address."""

    def __init__(self, exe):
        self.exe = exe
        img = self.img = Image(exe)
        text = self.text = next(s for s in img.sections if s['name'] == '.text')
        in_text = self.in_text = lambda v: text['start'] <= v < text['end']  # noqa: E731
        units = self.units = json.loads((DECOMP / 'build/units.json').read_text())['units']
        self.compiled = {int(p.name[:4]) for p in (ROOT / 'out/obj').glob('*.obj')}

        # function bounds: recipe spans (EH tails belong to their parent), other code cut at the next entry
        self.span, self.owner_unit, self.name = {}, {}, {}
        pieces = []                                   # (start, end, function)
        for i, u in enumerate(units):
            for f in u['functions']:
                s = int(f['address'], 16)
                self.span[s] = s + f['size']
                self.owner_unit.setdefault(s, i)
                self.name.setdefault(s, f['symbol'])
                pieces.append((s, s + f['size'], s))
            for e in u.get('eh_tails', []):
                s = int(e['address'], 16)
                pieces.append((s, s + e['size'], int(e['parent'], 16)))
        pieces.sort()
        self.pieces, self.piece_starts = pieces, [p[0] for p in pieces]

        src, tgt, _ = branches(exe, text)
        relocs = sorted(img.relocs())
        code_refs = [(va, img.u32(va)) for va in relocs if in_text(va) and img.u32(va) is not None
                     and in_text(img.u32(va))]
        data_secs = [(s['start'], s['end']) for s in img.sections if s['name'] in ('.rdata', '.data')]
        self.data_code = sorted({v for va in relocs if any(lo <= va < hi for lo, hi in data_secs)
                                 for v in [img.u32(va)] if v is not None and in_text(v)})
        self.data_code_set = set(self.data_code)
        self.xc = sorted({v for v in (img.u32(p) for p in range(XC_TABLE[0], XC_TABLE[1], 4)) if v})
        # entries of code outside the recipe: branch targets, referenced code addresses, roots
        entries = set(self.span) | {t for t in tgt if in_text(t)} | {v for _, v in code_refs} | \
            self.data_code_set | set(self.xc) | {WINMAIN}
        self.others = sorted(e for e in entries if e not in self.span)
        self.entries = sorted(entries)

        # the original call graph (and its reverse)
        self.succ, self.pred = defaultdict(set), defaultdict(set)
        for va, t in list(zip(src, tgt)) + code_refs:
            if in_text(t):
                f = self.function_of(va)
                if f is not None and t != f and not (f in self.span and f <= t < self.span[f]):
                    self.succ[f].add(t)
                    self.pred[t].add(f)
        self.load_build()

    def function_of(self, va):
        """The function an instruction address belongs to: the recipe piece holding it, else the nearest entry
        before it with no recipe piece in between (None: code no known entry leads to)."""
        k = bisect.bisect_right(self.piece_starts, va) - 1
        for j in (k, k - 1):
            if j >= 0 and self.pieces[j][0] <= va < self.pieces[j][1]:
                return self.pieces[j][2]
        e = bisect.bisect_right(self.others, va) - 1
        if e < 0 or (k >= 0 and self.pieces[k][0] > self.others[e]):
            return None
        return self.others[e]

    def end_of(self, f):
        """End of function f: its recipe span, else the next known entry."""
        if f in self.span:
            return self.span[f]
        k = bisect.bisect_right(self.entries, f)
        return self.entries[k] if k < len(self.entries) else f + 0x100

    def load_build(self):
        provided = json.loads((LINK / 'provided.json').read_text())
        lifted_info = json.loads((LINK / 'lifted.json').read_text())
        self.relinked = set(provided['relinked'])
        self.lifted = set(lifted_info['lifted'])
        self.unsupported = {int(k, 16): v for k, v in lifted_info['unsupported'].items()}
        self.unbound = set(lifted_info['unbound_code_pointers'])
        self.trap_f, self.trap_c = {}, {}
        for sym, (addr, nm) in provided['traps'].items():
            if sym.startswith('fnv_f_'):
                self.trap_f[addr] = nm
            elif sym.startswith('fnv_c_'):
                self.trap_c[addr] = nm

    def status(self, x):
        if x in self.lifted:
            return 'lifted'
        if x in self.relinked:
            return 'relinked'
        if x in self.trap_f:
            return 'trap-unit-not-compiled' if self.owner_unit.get(x) not in self.compiled else 'trap-not-emitted'
        if x in self.trap_c:
            return 'trap-code'
        if x in self.unbound:
            return 'lift-unbound'
        if CRT_REGION[0] <= x < CRT_REGION[1]:
            return 'crt'
        if x in self.data_code_set:
            # stored in the data image (vftable slot, callback table) but nothing built defines it: the loader
            # points the slot at a trap thunk. EH funclets and labels inside a function are only named by the
            # original EH tables (FuncInfo), which relinked code never uses.
            if x in self.span:
                return 'loader-thunk-recipe'
            k = bisect.bisect_right(self.piece_starts, x) - 1
            inside = any(j >= 0 and self.pieces[j][0] < x < self.pieces[j][1] for j in (k, k - 1))
            return 'original-eh-only' if inside else 'loader-thunk-gap'
        return 'unreferenced'          # not called by anything built: inlined by clang, or behind a problem

    def label(self, x):
        return '%#010x %s' % (x, self.name.get(x) or self.trap_f.get(x) or self.trap_c.get(x) or '')

    def reach(self):
        """Breadth-first from the roots, tier A then tier B: ({address: depth}, {address: parent}, {address: tier})."""
        depth, parent, tier = {}, {}, {}
        for t, roots in (('A', [WINMAIN] + self.xc), ('B', self.data_code)):
            queue = deque()
            for r in roots:
                if r not in depth:
                    depth[r], parent[r], tier[r] = 0, None, t
                    queue.append(r)
            while queue:
                x = queue.popleft()
                if self.status(x) == 'crt':
                    continue                  # replaced by the modern CRT: its internals are not ours
                for y in sorted(self.succ.get(x, ())):
                    if y not in depth:
                        depth[y], parent[y], tier[y] = depth[x] + 1, x, t
                        queue.append(y)
        return depth, parent, tier


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', required=True)
    ap.add_argument('--out', default=str(ROOT / 'out/reach.json'))
    a = ap.parse_args()
    p = Program(a.exe)
    depth, parent, tier = p.reach()

    def path(x):
        out = []
        while x is not None and len(out) < 12:
            out.append(p.label(x))
            x = parent[x]
        return out[::-1]

    status = {x: p.status(x) for x in depth}
    counts = {t: Counter(status[x] for x in depth if tier[x] == t) for t in 'AB'}
    problems = sorted((x for x in depth if status[x] in PROBLEMS), key=lambda x: (tier[x], depth[x], x))
    by_unit = defaultdict(lambda: dict(A=0, B=0, min_depth=99, functions=[]))
    for x in problems:
        u = p.owner_unit.get(x)
        key = p.units[u]['source'] if u is not None else '(outside the recipe: %s)' % status[x]
        e = by_unit[key]
        e[tier[x]] += 1
        e['min_depth'] = min(e['min_depth'], depth[x])
        e['functions'].append('%#010x' % x)
        e['compiled'] = u in p.compiled if u is not None else None
    worklist = sorted(by_unit.items(), key=lambda kv: (-kv[1]['A'], kv[1]['min_depth'], -kv[1]['B']))
    reasons = Counter(re.sub(r'0x[0-9a-f]+', 'N', p.unsupported[x]) for x in problems if x in p.unsupported)
    report = dict(
        roots=dict(winmain='%#010x' % WINMAIN, initializers=len(p.xc), data_code_pointers=len(p.data_code)),
        graph=dict(functions=len(p.succ), edges=sum(len(v) for v in p.succ.values())),
        tier_A=dict(counts['A']), tier_B=dict(counts['B']),
        problems=dict(A=sum(1 for x in problems if tier[x] == 'A'), B=sum(1 for x in problems if tier[x] == 'B')),
        lift_failure_reasons=reasons.most_common(25),
        worklist=[dict(unit=k, **{kk: vv for kk, vv in v.items() if kk != 'functions'},
                       functions=v['functions'][:20]) for k, v in worklist],
        problems_list=[dict(address='%#010x' % x, status=status[x], tier=tier[x], depth=depth[x],
                            lift_failure=p.unsupported.get(x), path=path(x)) for x in problems])
    Path(a.out).write_text(json.dumps(report, indent=1))
    print(json.dumps({k: report[k] for k in ('roots', 'graph', 'tier_A', 'tier_B', 'problems')}, indent=1))
    print('top of the worklist (unit: tier A / tier B problems, nearest depth):')
    for k, v in worklist[:25]:
        print('  %-70s %4d %5d  d%-2d %s' % (k[:70], v['A'], v['B'], v['min_depth'],
                                            '' if v.get('compiled') is None else
                                            'compiled' if v['compiled'] else 'NOT COMPILED'))


if __name__ == '__main__':
    main()
