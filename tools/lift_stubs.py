#!/usr/bin/env python3
"""Lift small out-of-scope stubs of FalloutNV.exe into C++ (Phase 1).

The original C++ initializer table (__xc_a..__xc_z) holds 537 compiler-generated dynamic initializers that are not
decomp functions (e.g. the one constructing the MemoryManager), plus the atexit thunks they register. They are short
straight-line x86 (about 30 opcodes in all). This translates each one into a C++ function over an explicit machine
state: memory operands address real memory (the data image is mapped at its original addresses), and calls go to
relinked/CRT functions through fnv_lift_call (port/runtime/fnv_lift_runtime.cpp). Each lifted stub is defined as
fnv_f_<original address>, so it joins fnv_addr_table and the loader calls it from the original table.

The generated file is derived from the exe: it is written to out/link/ (git-ignored) and never committed.
Unsupported instructions become a logged trap at that address, and are counted in the report.
"""
import re
import struct
import subprocess
from pathlib import Path

OBJDUMP = r'C:\Program Files\LLVM\bin\llvm-objdump.exe'
LIFT_REGION = (0x00F34000, 0x00FDE58B)       # compiler-generated initializer/atexit code (EH stubs lie below)
REG32 = ['eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi']
REG16 = {'ax': 'eax', 'cx': 'ecx', 'dx': 'edx', 'bx': 'ebx', 'si': 'esi', 'di': 'edi', 'bp': 'ebp', 'sp': 'esp'}
REG8 = {'al': ('eax', 0), 'cl': ('ecx', 0), 'dl': ('edx', 0), 'bl': ('ebx', 0),
        'ah': ('eax', 8), 'ch': ('ecx', 8), 'dh': ('edx', 8), 'bh': ('ebx', 8)}
SIZES = {'byte': 8, 'word': 16, 'dword': 32, 'qword': 64, 'xmmword': 128, 'tbyte': 80}
LINE = re.compile(r'^\s*([0-9a-f]+):\s+((?:[0-9a-f]{2}\s)+)\s*([a-z][\w.]*)\s*(.*)$')


class Unsupported(Exception):
    pass


def disassemble(exe, lo, hi):
    out = subprocess.run([OBJDUMP, '-d', '-M', 'intel', '--start-address=%#x' % lo, '--stop-address=%#x' % hi, exe],
                         capture_output=True, text=True).stdout
    ins = {}
    for l in out.splitlines():
        m = LINE.match(l)
        if m:
            raw = bytes.fromhex(re.sub(r'\s', '', m.group(2)))
            ops = re.sub(r'\s*#.*$', '', re.sub(r'\s*<[^>]*>\s*$', '', m.group(4))).strip()
            mnem = m.group(3)
            if mnem in ('rep', 'repe', 'repne'):        # "rep stosd dword ptr es:[edi], eax"
                part = ops.split(None, 1)
                mnem, ops = mnem + ' ' + part[0], (part[1] if len(part) > 1 else '')
            ins[int(m.group(1), 16)] = (len(raw), raw, mnem, ops)
    return ins


def num(s):
    s = s.strip()
    neg = s.startswith('-')
    v = int(s.lstrip('-'), 0)
    return -v if neg else v


class Lifter:
    def __init__(self, exe, resolve_code, text_range):
        """resolve_code(addr) -> ('sym', decorated) | ('lift', None) | None, for an original code address."""
        self.exe = exe
        self.resolve_code = resolve_code
        self.text = text_range
        self.ins = disassemble(exe, LIFT_REGION[0], LIFT_REGION[1])
        self.order = sorted(self.ins)
        self.externs = {}          # C identifier -> decorated symbol
        self.traps = set()         # code addresses with no binding (called/pushed): lift_trap_<addr>
        self.unsupported = {}      # original address -> instruction text
        self.lifted = {}           # stub address -> C++ body
        self.queue = []

    # ---- code pointers ------------------------------------------------------------------------------------------
    def code_ptr(self, addr):
        """C expression (void*) for an original code address."""
        r = self.resolve_code(addr)
        if r and r[0] == 'sym':
            ident = 'X_' + re.sub(r'\W', '_', r[1])[:60] + '_%08x' % addr
            self.externs[ident] = r[1]
            return '(void*)&%s' % ident
        if LIFT_REGION[0] <= addr < LIFT_REGION[1] and addr in self.ins:
            if addr not in self.lifted and addr not in self.queue:
                self.queue.append(addr)
            return '(void*)&lifted_%08x' % addr
        self.traps.add(addr)
        return '(void*)&lift_trap_%08x' % addr

    def is_code(self, v):
        return self.text[0] <= (v & 0xffffffff) < self.text[1]

    # ---- operands -----------------------------------------------------------------------------------------------
    def mem_addr(self, inner):
        inner = re.sub(r'^\w+:', '', inner.strip())              # segment prefix (es:, ds:)
        terms, expr = re.findall(r'([+-]?)\s*([^+-]+)', inner.replace(' ', '')), []
        for sign, t in terms:
            sign = sign or '+'
            if '*' in t:
                r, sc = t.split('*')
                if r not in REG32:              # objdump also prints the scale first: [4*ecx]
                    r, sc = sc, r
                expr.append('%s s.%s*%s' % (sign, r, sc))
            elif t in REG32:
                expr.append('%s s.%s' % (sign, t))
            else:
                expr.append('%s %su' % (sign, hex(num(t)) if not t.startswith('0x') else t))
        return '(uint32_t)(0u %s)' % ' '.join(expr)

    def parse(self, op):
        """-> (kind, size, expr) kind in reg/reg8/reg16/mem/imm/st/xmm"""
        op = op.strip()
        m = re.match(r'^(?:(byte|word|dword|qword|xmmword|tbyte) ptr )?(\w+:)?\[(.*)\]$', op)
        if m:
            if m.group(2) == 'fs:':
                # SEH frame registration in a stub (fs:[0]): a private per-call variable (stubs do not unwind)
                if m.group(3).strip() not in ('0', '0x0'):
                    raise Unsupported('fs: offset')
                return ('fs', 32, 's.fs0')
            return ('mem', SIZES[m.group(1) or 'dword'], self.mem_addr(m.group(3)))
        if op in REG32:
            return ('reg', 32, 's.' + op)
        if op in REG16:
            return ('reg16', 16, REG16[op])
        if op in REG8:
            return ('reg8', 8, op)
        if re.match(r'^xmm\d$', op):
            return ('xmm', 128, op[3:])
        if re.match(r'^st(\(\d\))?$', op):
            return ('st', 80, op)
        if re.match(r'^-?(0x[0-9a-f]+|\d+)$', op):
            return ('imm', 32, num(op))
        raise Unsupported('operand ' + op)

    def read(self, o, size=None):
        k, sz, e = o
        size = size or sz
        if k in ('reg', 'fs'):
            return e
        if k == 'reg16':
            return '(s.%s & 0xffffu)' % e
        if k == 'reg8':
            r, sh = REG8[e]
            return '((s.%s >> %d) & 0xffu)' % (r, sh)
        if k == 'imm':
            if self.is_code(e) and size == 32:
                return '(uint32_t)(uintptr_t)%s' % self.code_ptr(e & 0xffffffff)
            return '%#xu' % (e & 0xffffffff)
        if k == 'mem':
            return {8: 'M8(%s)', 16: 'M16(%s)', 32: 'M32(%s)'}[size] % e
        raise Unsupported('read ' + k)

    def write(self, o, val):
        k, sz, e = o
        if k in ('reg', 'fs'):
            return '%s = (uint32_t)(%s);' % (e, val)
        if k == 'reg16':
            return 's.%s = (s.%s & 0xffff0000u) | ((%s) & 0xffffu);' % (e, e, val)
        if k == 'reg8':
            r, sh = REG8[e]
            return 's.%s = (s.%s & ~(0xffu << %d)) | (((%s) & 0xffu) << %d);' % (r, r, sh, val, sh)
        if k == 'mem':
            return {8: 'M8(%s) = (uint8_t)(%s);', 16: 'M16(%s) = (uint16_t)(%s);',
                    32: 'M32(%s) = (uint32_t)(%s);'}[sz] % (e, val)
        raise Unsupported('write ' + k)

    # ---- one instruction ------------------------------------------------------------------------------------------
    def stmt(self, addr, n, raw, mnem, ops, labels, entry):
        args = [a for a in re.split(r',(?![^\[]*\])', ops) if a.strip()] if ops else []
        P = [self.parse(a) for a in args] if mnem not in ('call', 'jmp') and not mnem.startswith('j') else []
        nxt = addr + n

        def flags(v, size=32):
            return 'FLAGS(%s, %d);' % (v, size)

        if mnem == 'nop' or mnem == 'int3':
            return ''
        if mnem in ('mov', 'movzx', 'movsx'):
            src = self.read(P[1])
            if mnem == 'movsx':
                src = '(uint32_t)(int32_t)(int%d_t)(%s)' % (P[1][1], src)
            return self.write(P[0], src)
        if mnem == 'lea':
            return self.write(P[0], P[1][2])
        if mnem == 'push':
            return 'PUSH(%s);' % self.read(P[0], 32)
        if mnem == 'pop':
            return self.write(P[0], 'POP()')
        if mnem in ('add', 'sub', 'and', 'or', 'xor', 'imul', 'shl', 'shr', 'sar'):
            if mnem == 'imul' and len(P) != 2:
                raise Unsupported('imul form')
            a, b = self.read(P[0]), self.read(P[1])
            op = {'add': '+', 'sub': '-', 'and': '&', 'or': '|', 'xor': '^', 'imul': '*', 'shl': '<<',
                  'shr': '>>'}.get(mnem)
            v = '(uint32_t)((int32_t)(%s) >> (%s))' % (a, b) if mnem == 'sar' else '(uint32_t)((%s) %s (%s))' % (a, op, b)
            return '{ uint32_t r_ = %s; %s %s }' % (v, self.write(P[0], 'r_'), flags('r_', P[0][1]))
        if mnem in ('neg', 'not', 'inc', 'dec'):
            a = self.read(P[0])
            v = {'neg': '0u - (%s)', 'not': '~(%s)', 'inc': '(%s) + 1u', 'dec': '(%s) - 1u'}[mnem] % a
            return '{ uint32_t r_ = %s; %s %s }' % (v, self.write(P[0], 'r_'), flags('r_', P[0][1]))
        if mnem in ('cmp', 'test'):
            v = ('(uint32_t)((%s) - (%s))' if mnem == 'cmp' else '(uint32_t)((%s) & (%s))') % (
                self.read(P[0]), self.read(P[1], P[0][1]))
            return flags(v, P[0][1])
        if mnem in ('sete', 'setz', 'setne', 'setnz', 'sets', 'setns'):
            c = {'sete': 's.zf', 'setz': 's.zf', 'setne': '!s.zf', 'setnz': '!s.zf', 'sets': 's.sf',
                 'setns': '!s.sf'}[mnem]
            return self.write(P[0], '(%s) ? 1u : 0u' % c)
        if mnem == 'cdq':
            return 's.edx = (s.eax & 0x80000000u) ? 0xffffffffu : 0u;'
        if mnem == 'jmp' and ops.strip() in REG32:
            return 'CALL_AT((void*)(uintptr_t)s.%s, s.esp + 4u); return;' % ops.strip()
        if mnem == 'jmp' and ops.strip().startswith('dword ptr'):
            return 'CALL_AT((void*)(uintptr_t)%s, s.esp + 4u); return;' % self.read(self.parse(ops.strip()))
        if mnem.startswith('j'):
            tgt = num(ops.split()[0]) if ops.split() else None
            cond = {'je': 's.zf', 'jz': 's.zf', 'jne': '!s.zf', 'jnz': '!s.zf', 'js': 's.sf', 'jns': '!s.sf',
                    'jmp': None}
            if mnem not in cond:
                raise Unsupported(mnem)
            if ops.strip().startswith('dword ptr'):
                raise Unsupported('indirect jmp')
            if entry <= tgt < entry + 0x2000 and tgt in self.ins and not (mnem == 'jmp' and tgt in self.stub_starts):
                labels.add(tgt)
                return ('if (%s) goto L_%08x;' % (cond[mnem], tgt)) if cond[mnem] else 'goto L_%08x;' % tgt
            if mnem != 'jmp':
                raise Unsupported('conditional jump out of stub')
            # tail call: the callee's arguments sit above the (fake) return address
            return 'CALL_AT(%s, s.esp + 4u); return;' % self.code_ptr(tgt)
        if mnem == 'call':
            o = ops.strip()
            if o.startswith('dword ptr'):
                return 'CALL((void*)(uintptr_t)%s);' % self.read(self.parse(o))
            if o in REG32:
                return 'CALL((void*)(uintptr_t)s.%s);' % o
            return 'CALL(%s);' % self.code_ptr(num(o.split()[0]))
        if mnem == 'ret':
            return 'return;'
        if mnem in ('rep stosd', 'rep movsd'):
            body = 'M32(s.edi) = s.eax; s.edi += 4;' if 'stos' in mnem else 'M32(s.edi) = M32(s.esi); s.edi += 4; s.esi += 4;'
            return 'while (s.ecx) { %s s.ecx--; }' % body
        # ---- x87 (register forms decoded from the opcode bytes) ----
        if raw[0] in (0xD8, 0xDC, 0xDE) and len(raw) == 2 and raw[1] >= 0xC0:
            i, grp = raw[1] & 7, (raw[1] >> 3) & 7         # 0 add,1 mul,4/5 sub,6/7 div (see table below)
            if raw[0] == 0xD8:      # st0 = st0 op st(i)
                f = {0: 'ST(0) + ST(%d)', 1: 'ST(0) * ST(%d)', 4: 'ST(0) - ST(%d)', 5: 'ST(%d) - ST(0)',
                     6: 'ST(0) / ST(%d)', 7: 'ST(%d) / ST(0)'}[grp]
                return 'ST(0) = %s;' % (f % i)
            # DC / DE: st(i) = st(i) op st0 (DC E0 fsubr, E8 fsub, F0 fdivr, F8 fdiv), DE pops afterwards
            f = {0: 'ST(%d) + ST(0)', 1: 'ST(%d) * ST(0)', 4: 'ST(0) - ST(%d)', 5: 'ST(%d) - ST(0)',
                 6: 'ST(0) / ST(%d)', 7: 'ST(%d) / ST(0)'}[grp]
            return 'ST(%d) = %s;%s' % (i, f % i, ' FPOP();' if raw[0] == 0xDE else '')
        if raw[0] == 0xD9 and len(raw) == 2 and raw[1] >= 0xC0:
            b = raw[1]
            if 0xC0 <= b <= 0xC7:
                return '{ double v_ = ST(%d); FPUSH(v_); }' % (b & 7)
            if 0xC8 <= b <= 0xCF:
                return '{ double v_ = ST(0); ST(0) = ST(%d); ST(%d) = v_; }' % (b & 7, b & 7)
            return {0xE0: 'ST(0) = -ST(0);', 0xE1: 'ST(0) = fabs(ST(0));', 0xE8: 'FPUSH(1.0);',
                    0xEE: 'FPUSH(0.0);', 0xFA: 'ST(0) = sqrt(ST(0));'}.get(b) or self._unsup(mnem, ops)
        if raw[0] == 0xDD and len(raw) == 2 and raw[1] >= 0xC0:
            b = raw[1]
            if 0xD0 <= b <= 0xD7:
                return 'ST(%d) = ST(0);' % (b & 7)
            if 0xD8 <= b <= 0xDF:
                return 'ST(%d) = ST(0); FPOP();' % (b & 7)
            return self._unsup(mnem, ops)
        if mnem in ('fld', 'fild', 'fst', 'fstp', 'fistp', 'fist', 'fadd', 'fmul', 'fsub', 'fsubr', 'fdiv', 'fdivr'):
            o = P[0]
            if o[0] != 'mem':
                return self._unsup(mnem, ops)
            load = {32: 'MF(%s)', 64: 'MD(%s)'}.get(o[1])
            if mnem == 'fild':
                return 'FPUSH((double)(int%d_t)M%d(%s));' % (o[1], o[1], o[2])
            if mnem in ('fist', 'fistp'):
                return 'M%d(%s) = (uint%d_t)(int%d_t)lrint(ST(0));%s' % (o[1], o[2], o[1], o[1],
                                                                        ' FPOP();' if mnem == 'fistp' else '')
            if load is None:
                return self._unsup(mnem, ops)
            v = load % o[2]
            if mnem == 'fld':
                return 'FPUSH(%s);' % v
            if mnem in ('fst', 'fstp'):
                store = {32: 'MF(%s) = (float)ST(0);', 64: 'MD(%s) = ST(0);'}[o[1]] % o[2]
                return store + (' FPOP();' if mnem == 'fstp' else '')
            f = {'fadd': 'ST(0) + %s', 'fmul': 'ST(0) * %s', 'fsub': 'ST(0) - %s', 'fsubr': '%s - ST(0)',
                 'fdiv': 'ST(0) / %s', 'fdivr': '%s / ST(0)'}[mnem]
            return 'ST(0) = %s;' % (f % v)
        # ---- SSE ----
        if mnem in ('movaps', 'movups', 'movss', 'unpcklps', 'xorps', 'mulps', 'addps', 'shufps'):
            d, s_ = P[0], P[1]
            if mnem in ('movaps', 'movups'):
                if d[0] == 'xmm':
                    src = 'X(%s)' % s_[2] if s_[0] == 'xmm' else '(const uint8_t*)(uintptr_t)%s' % s_[2]
                    return 'memcpy(X(%s), %s, 16);' % (d[2], src)
                return 'memcpy((uint8_t*)(uintptr_t)%s, X(%s), 16);' % (d[2], s_[2])
            if mnem == 'movss':
                if d[0] == 'xmm' and s_[0] == 'mem':
                    return 'memset(X(%s), 0, 16); memcpy(X(%s), (const void*)(uintptr_t)%s, 4);' % (d[2], d[2], s_[2])
                if d[0] == 'mem':
                    return 'memcpy((void*)(uintptr_t)%s, X(%s), 4);' % (d[2], s_[2])
                return 'memcpy(X(%s), X(%s), 4);' % (d[2], s_[2])
            if mnem == 'unpcklps':
                src = 'X(%s)' % s_[2] if s_[0] == 'xmm' else '(const uint8_t*)(uintptr_t)%s' % s_[2]
                return 'UNPCKLPS(X(%s), %s);' % (d[2], src)
            return self._unsup(mnem, ops)
        return self._unsup(mnem, ops)

    def _unsup(self, mnem, ops):
        raise Unsupported('%s %s' % (mnem, ops))

    # ---- one stub -----------------------------------------------------------------------------------------------
    def lift(self, entry):
        """Linear sweep from entry to ret/tail jump, following intra-stub branch targets."""
        body, labels, todo, seen = {}, set(), [entry], set()
        while todo:
            a = todo.pop()
            while a in self.ins and a not in seen:
                seen.add(a)
                n, raw, mnem, ops = self.ins[a]
                try:
                    st = self.stmt(a, n, raw, mnem, ops, labels, entry)
                except Unsupported as e:
                    self.unsupported[a] = str(e)
                    st = 'fnv_trap(0x%08xu, "lifted: unsupported instruction"); return;' % a
                body[a] = st
                if mnem == 'ret' or (mnem == 'jmp' and 'goto' not in st) or 'return;' in st and mnem != 'call':
                    break
                for t in list(labels):
                    if t not in seen and t not in todo:
                        todo.append(t)
                a += n
        lines = ['extern "C" void lifted_%08x(void)' % entry, '{', '    LIFT_STATE;']
        for a in sorted(body):
            if a in labels:
                lines.append('L_%08x:' % a)
            lines.append('    %s  // %08x %s %s' % (body[a], a, self.ins[a][2], self.ins[a][3][:60]))
        lines.append('}')
        self.lifted[entry] = '\n'.join(lines)

    def run(self, entries):
        self.stub_starts = set(entries)
        self.queue = list(entries)
        while self.queue:
            e = self.queue.pop(0)
            if e not in self.lifted and e in self.ins:
                self.lift(e)

    def source(self):
        out = ['// Generated by tools/lift_stubs.py from the player\'s FalloutNV.exe: do not commit (rule 1).',
               '#include "fnv_lift.h"', '']
        for ident, sym in sorted(self.externs.items()):
            out.append('extern "C" void %s(void) __asm__("%s");' % (ident, sym))
        for a in sorted(self.lifted):     # exact undecorated name, like every relinked function
            out.append('extern "C" void lifted_%08x(void) __asm__("fnv_f_%08x");' % (a, a))
        for a in sorted(self.traps):
            out.append('static void lift_trap_%08x(void) { fnv_trap(0x%08xu, "lifted: unbound code pointer"); }' % (a, a))
        out.append('')
        out += [self.lifted[a] + '\n' for a in sorted(self.lifted)]
        return '\n'.join(out)
