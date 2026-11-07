#!/usr/bin/env python3
"""Recipe functions that leave through a tail `jmp` (no `ret` of their own) and whose definition's declared stack pop
differs from what the original effectively pops.

pop_check.py only sees functions with a `ret N`. A tail-jumping function (an adjustor thunk `sub [esp+4],N; jmp T`, a
COM forwarder `mov [esp+4],inner; jmp [vtbl+N]`) pops whatever its jump target pops. When the decomp declares it
`__cdecl` (the usual model for these thunks), clang compiles `call T; add esp,4; ret`, which pops nothing and, when T
really is `ret 4`, pops the argument twice and returns to the argument value (0x009fdd10, IPin::Release thunk).

Effective pop: a direct `jmp T` into another recipe function pops what T pops (followed through further tail jumps);
an indirect jump that first stores a new `this` into [esp+4] is a stdcall COM forwarder (pops at least 4); other
indirect jumps are unknown.

tools/link_build.py lifts the flagged definitions (lifted code passes the jump target's real pop through) and binds
references declared with another pop to popfix thunks.

usage: python tools/tail_pops.py  -> prints the flagged functions; out/tail_pop_mismatch.json
"""
from decomp_path import DECOMP_DIR
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import msmangle  # noqa: E402

DECOMP = Path(DECOMP_DIR)
LINE = re.compile(r'^\s*([0-9a-f]+):\s+(\S+)\s*(.*?)\s*(?:<.*>)?\s*$')
STORE_THIS = re.compile(r'^dword ptr \[esp \+ 0x4\], ')
ADJUSTOR = re.compile(r'@@[GHOPWX](?:[0-9]|[A-P]+@)(?=[A-Z])')


def scan():
    units = json.loads((DECOMP / 'build/units.json').read_text())['units']
    defs = {int(f['address'], 16): (f['symbol'], f['size']) for u in units for f in u['functions']}
    ends = sorted((a, a + s) for a, (_, s) in defs.items())
    insns = {a: [] for a in defs}
    k = 0
    for line in (ROOT / 'out/exe_text.asm').read_text(errors='replace').splitlines():
        m = LINE.match(line)
        if not m:
            continue
        a = int(m.group(1), 16)
        while k < len(ends) and ends[k][1] <= a:
            k += 1
        if k < len(ends) and ends[k][0] <= a:
            insns[ends[k][0]].append((a, m.group(2), m.group(3).strip()))

    own_memo = {}

    def own(f):
        if f not in own_memo:
            own_memo[f] = own_exit(f)
        return own_memo[f]

    def own_exit(f):
        """('ret', {N}), ('jmp', target), ('fwd', None) for a this-storing indirect jump, ('ind', None), or None"""
        body = [i for i in insns[f] if i[1] not in ('int3', 'nop')]
        rets = {int(o, 0) if o else 0 for _, mn, o in body if mn in ('ret', 'retn')}
        if rets:
            return 'ret', rets
        if not body or body[-1][1] != 'jmp':
            return None
        op = body[-1][2]
        if re.fullmatch(r'0x[0-9a-f]+', op):
            return 'jmp', int(op, 16)
        if any(mn in ('mov', 'sub', 'add') and STORE_THIS.match(o) for _, mn, o in body):
            return 'fwd', None
        return 'ind', None

    memo = {}

    def declared_pop(sym):
        try:
            d = msmangle.stack_pop(sym)
        except Exception:
            d = None
        if d is not None:
            return d
        # adjustor thunks (`@@WBE@AAXXZ`: public, this -= 0x14, __cdecl) are members like the virtual they adjust for
        for m in ADJUSTOR.finditer(sym):
            try:
                d = msmangle.stack_pop(sym[:m.start()] + '@@U' + sym[m.end():])
            except Exception:
                d = None
            if d is not None:
                return d
        return None

    def effective(f, depth=0):
        if f in memo:
            return memo[f]
        memo[f] = None                       # cycle guard
        r = own(f) if f in defs else None
        out = None
        if r is None:
            out = None
        elif r[0] == 'ret':
            out = next(iter(r[1])) if len(r[1]) == 1 else None
        elif r[0] == 'jmp':
            out = effective(r[1], depth + 1) if depth < 16 else None
        elif r[0] == 'fwd':
            out = 'fwd'
        memo[f] = out
        return out

    flagged = {}
    for f, (sym, _) in defs.items():
        r = own(f)
        if r is None or r[0] == 'ret':
            continue
        declared = declared_pop(sym)
        eff = effective(f)
        bad = (isinstance(eff, int) and declared is not None and declared != eff) or \
              (eff == 'fwd' and declared == 0)
        if bad:
            flagged[f] = {'symbol': sym, 'declared': declared, 'effective': eff, 'exit': r[0],
                          'target': '0x%08x' % r[1] if r[0] == 'jmp' else None}
    # references (any unit's name for a tail-jumping function) whose declared pop differs from the known effective
    # pop, in out/pop_mismatch.json form: compiled callers bind to popfix thunks like pop_check.py's findings
    refs = {}
    for u in units:
        for name, v in u.get('symbols', {}).items():
            a = int(v, 0)
            if a not in defs or own(a) is None or own(a)[0] == 'ret':
                continue
            eff = effective(a)
            d = declared_pop(name)
            if isinstance(eff, int) and d is not None and d != eff:
                refs.setdefault(a, {'actual': eff, 'declared': {}})['declared'][name] = d
    return flagged, refs


def main():
    flagged, refs = scan()
    (ROOT / 'out/tail_pop_mismatch.json').write_text(json.dumps({'0x%08x' % a: e for a, e in sorted(flagged.items())},
                                                                indent=0))
    print(len(refs), 'tail-jumping addresses referenced under a name with a different pop')
    for a, e in sorted(flagged.items()):
        print('0x%08x %-4s declared %-4s effective %-4s %s %s' % (a, e['exit'], e['declared'], e['effective'],
                                                                 e['target'] or '', e['symbol']))
    print(len(flagged), 'tail-jumping definitions whose declared pop differs from the original')


if __name__ == '__main__':
    main()
