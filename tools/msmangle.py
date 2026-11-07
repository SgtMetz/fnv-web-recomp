#!/usr/bin/env python3
"""Bytes a function pops from the stack on return (`ret N`), from its MSVC-decorated name.

Used for trap stubs that have no original address (port/runtime/fnv_trap.cpp discovery mode): __cdecl pops nothing;
__thiscall/__stdcall/__fastcall pop their stack arguments, whose sizes come from the parameter types. Returns None
whenever a parameter's size is not certain (a class or union passed by value, unusual encodings), so a caller never
guesses. Checked against the original `ret N` of every recipe function (see `python tools/msmangle.py --exe ...`).
"""
from decomp_path import DECOMP_DIR
import re

STATIC = set('CDKLSTYZ')       # <class><cc><return>
MEMBER = set('ABEFIJMNQRUV')   # <class><this-cv><cc><return>
PRIM4 = set('CDEFGHIJKM')      # char/short/int/long (signed/unsigned), float: one stack slot
PRIM8 = set('NO')              # double, long double
EXT = {'D': 4, 'E': 4, 'F': 4, 'G': 4, 'H': 4, 'I': 4, 'J': 8, 'K': 8, 'N': 4, 'W': 4, 'S': 4, 'U': 4}


class Fail(Exception):
    pass


class Parser:
    def __init__(self, s):
        self.s, self.i = s, 0
        self.types = []                # back-reference table for multi-character argument types

    def peek(self, n=1):
        return self.s[self.i:self.i + n]

    def take(self, n=1):
        v = self.s[self.i:self.i + n]
        if len(v) < n:
            raise Fail('end')
        self.i += n
        return v

    # ---- names --------------------------------------------------------------------------------------------------
    def number(self):
        if self.peek() == '?':
            self.take()
        c = self.take()
        if c.isdigit():
            return
        while c != '@':
            if not 'A' <= c <= 'P':
                raise Fail('number')
            c = self.take()

    def fragment(self):
        c = self.peek()
        if c.isdigit():
            self.take()
            return
        if self.peek(2) == '?$':
            self.take(2)
            self.ident()
            saved = self.types
            self.types = []
            while self.peek() != '@':
                self.template_arg()
            self.take()
            self.types = saved
            return
        if self.peek(2) == '?A':        # anonymous namespace ?A0x...@
            self.take(2)
            self.ident()
            return
        if c == '?':
            raise Fail('nested name')
        self.ident()

    def ident(self):
        j = self.s.find('@', self.i)
        if j < 0:
            raise Fail('ident')
        self.i = j + 1

    def qualified(self):
        while self.peek() != '@':
            if not self.peek():
                raise Fail('name end')
            self.fragment()
        self.take()

    def template_arg(self):
        if self.peek() == '$':
            self.take()
            c = self.take()
            if c == '0':
                self.number()
            elif c in '1':
                self.take()          # '?'
                self.symbol_ref()
            elif c == '$':
                c2 = self.take()
                if c2 in 'QRAB':
                    self.cv()
                    self.type()
                elif c2 == 'V':
                    pass
                else:
                    raise Fail('template $$' + c2)
            else:
                raise Fail('template $' + c)
            return
        self.arg()

    def symbol_ref(self):
        # ?name@scope@@<type info>: give up (rare in parameter types)
        raise Fail('symbol template argument')

    # ---- types --------------------------------------------------------------------------------------------------
    def cv(self):
        while self.peek() == 'E':       # __ptr64
            self.take()
        c = self.take()
        if c not in 'ABCD':
            raise Fail('cv ' + c)

    def function_type(self):
        """After '6': <cc><return><params>Z."""
        self.take()                    # calling convention
        if self.peek() == '?':
            self.take(2)
        self.ret()
        self.params()
        if self.peek() == 'Z':
            self.take()

    def type(self):
        """One type; returns its size as a by-value argument (None: not known)."""
        c = self.take()
        if c in PRIM4:
            return 4
        if c in PRIM8:
            return 8
        if c == 'X':
            return 0
        if c == '_':
            k = self.take()
            if k not in EXT:
                raise Fail('_' + k)
            return EXT[k]
        if c in 'PQRS':
            if self.peek() == '6':
                self.take()
                self.function_type()
                return 4
            self.cv()
            if self.peek() == 'Y':
                self.array()
            else:
                self.type()
            return 4
        if c in 'AB':
            self.cv()
            if self.peek() == 'Y':
                self.array()
            else:
                self.type()
            return 4
        if c == '$':
            k = self.take(2)
            if k in ('$Q', '$R'):
                self.cv()
                self.type()
                return 4
            if k == '$C':
                self.cv()
                return self.type()
            raise Fail('$' + k)
        if c in 'TUV':
            self.qualified()
            return None
        if c == 'W':
            self.take()                # enum underlying type
            self.qualified()
            return 4
        if c == '?':
            self.cv()
            return self.type()
        raise Fail('type ' + c)

    def array(self):
        self.take()                    # Y
        self.number()
        # dimensions
        raise Fail('array')

    def arg(self):
        """A parameter type, with back-references (0-9) and recording of multi-character types."""
        c = self.peek()
        if c.isdigit():
            self.take()
            k = int(c)
            if k >= len(self.types):
                raise Fail('backref')
            return self.types[k]
        start = self.i
        size = self.type()
        if self.i - start > 1 and len(self.types) < 10:
            self.types.append(size)
        return size

    def ret(self):
        """Consume the return type; True when it is a class/struct/union returned by value."""
        if self.peek() == '@':         # constructors/destructors
            self.take()
            return False
        if self.peek(1) == '?':        # return type with cv (?A, ?B)
            self.take()
            self.cv()
        by_value_class = self.peek() in ('T', 'U', 'V')
        self.type()                    # not an argument: not in the back-reference table
        return by_value_class

    def params(self):
        """Sum of the parameter sizes (None when one is unknown); stops at the list terminator."""
        if self.peek() == 'X':
            self.take()
            return 0
        total = 0
        while True:
            c = self.peek()
            if c == '@':
                self.take()
                return total
            if c == 'Z':               # varargs
                self.take()
                return total
            if not c:
                raise Fail('params end')
            size = self.arg()
            total = None if total is None or size is None else total + size


def calling(name):
    """(calling convention letter, parser positioned at the return type) or None."""
    if not name.startswith('?'):
        return None
    p = Parser(name)
    p.take()
    try:
        if p.peek(2) == '?$':
            p.fragment()
        elif p.peek() == '?':          # operators, ctors, dtors: ?0 ?1 ?_E ...
            p.take()
            if p.peek() == '_':
                p.take()
            p.take()
        else:
            p.ident()
        p.qualified()
        kind = p.take()
        if kind in MEMBER:
            p.cv()
        elif kind not in STATIC:
            return None
        cc = p.take()
        return cc, p, kind in MEMBER
    except Fail:
        return None


def stack_pop(name):
    """`ret N` for a decorated function name, or None when unknown."""
    m = re.match(r'^_\w+@(\d+)$', name)
    if m:
        return int(m.group(1))                # extern "C" __stdcall
    if re.match(r'^_[A-Za-z_]\w*$', name):
        return 0                              # extern "C" __cdecl
    r = calling(name)
    if r is None:
        return None
    cc, p, member = r
    if cc in 'AB':                            # __cdecl
        return 0
    if cc not in 'EFGHIJ':                    # thiscall E/F, stdcall G/H, fastcall I/J
        return None
    try:
        sret = p.ret()
        total = p.params()
    except Fail:
        return None
    if total is None:
        return None
    if sret:
        # a member function returns every class by value through a hidden pointer argument; other functions
        # return small POD types in registers, which the name alone does not tell
        if not member:
            return None
        total += 4
    if member and cc in 'GH':
        total += 4                            # __stdcall member (COM methods): `this` is on the stack too
    if cc in 'IJ':
        return None                           # fastcall: which arguments go in ecx/edx depends on their types
    return total


if __name__ == '__main__':
    # validation: every recipe function whose original returns with one `ret N`
    import argparse
    import json
    import sys
    from collections import Counter
    from pathlib import Path
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    sys.path.insert(0, DECOMP_DIR + r'\tools')
    import link_build
    from relink import Image
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', required=True)
    a = ap.parse_args()
    img = Image(a.exe)
    text = next(s for s in img.sections if s['name'] == '.text')
    units = json.loads((Path(DECOMP_DIR) / 'build/units.json').read_text())['units']
    funcs = {int(f['address'], 16): (f['symbol'], int(f['address'], 16) + f['size']) for u in units
             for f in u['functions']}
    pops = link_build.original_pops(a.exe, text, {k: v[1] for k, v in funcs.items()})
    c, bad = Counter(), []
    for addr, n in pops.items():
        got = stack_pop(funcs[addr][0])
        if got is None:
            c['unknown'] += 1
        elif got == n:
            c['agree'] += 1
        else:
            c['disagree'] += 1
            bad.append((funcs[addr][0], n, got))
    print(dict(c))
    for b in bad[:15]:
        print(b)
