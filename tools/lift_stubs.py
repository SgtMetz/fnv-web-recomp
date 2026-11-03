#!/usr/bin/env python3
"""Lift out-of-scope x86 code of FalloutNV.exe into C++ (Phase 1).

Two kinds of original code have no decomp source but are needed:
  * the 537 compiler-generated dynamic initializers in __xc_a..__xc_z (and the atexit thunks they register);
  * "gap" functions: game code between recipe functions that the decomp leaves out (e.g. 0x00aa78a0).
Each is translated into a C++ body over an explicit machine state (registers, flags, an emulated stack, an x87 stack,
XMM registers). Memory operands are real addresses (the data image is mapped at its original location); calls go to
relinked/CRT/lifted functions through fnv_lift_call (port/runtime/fnv_lift_runtime.cpp).

Every lifted function also gets a naked entry with the original calling interface, named fnv_f_<address> (and
fnv_c_<address>, the name unit objects use for out-of-scope code): it hands ecx/edx and the native argument words to
the body, and on return delivers eax/edx (and an x87 result), popping the bytes the original `ret N` popped.

The generated file is derived from the exe: it is written to out/link/ (git-ignored) and never committed.
Unsupported instructions become a logged trap at that address and are counted in the report.
"""
import re
import struct
import subprocess

OBJDUMP = r'C:\Program Files\LLVM\bin\llvm-objdump.exe'
LIFT_REGION = (0x00F34000, 0x00FDE58B)       # compiler-generated initializer/atexit code (EH stubs lie below)
REG32 = ['eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi']
REG16 = {'ax': 'eax', 'cx': 'ecx', 'dx': 'edx', 'bx': 'ebx', 'si': 'esi', 'di': 'edi', 'bp': 'ebp', 'sp': 'esp'}
REG8 = {'al': ('eax', 0), 'cl': ('ecx', 0), 'dl': ('edx', 0), 'bl': ('ebx', 0),
        'ah': ('eax', 8), 'ch': ('ecx', 8), 'dh': ('edx', 8), 'bh': ('ebx', 8)}
SIZES = {'byte': 8, 'word': 16, 'dword': 32, 'qword': 64, 'xmmword': 128, 'tbyte': 80}
LINE = re.compile(r'^\s*([0-9a-f]+):\s+((?:[0-9a-f]{2}\s)+)\s*([a-z][\w.]*)\s*(.*)$')
COND = {   # condition codes over the emulated flags
    'e': 's.zf', 'z': 's.zf', 'ne': '!s.zf', 'nz': '!s.zf', 's': 's.sf', 'ns': '!s.sf',
    'b': 's.cf', 'c': 's.cf', 'nae': 's.cf', 'ae': '!s.cf', 'nb': '!s.cf', 'nc': '!s.cf',
    'be': '(s.cf || s.zf)', 'na': '(s.cf || s.zf)', 'a': '(!s.cf && !s.zf)', 'nbe': '(!s.cf && !s.zf)',
    'l': '(s.sf != s.of)', 'nge': '(s.sf != s.of)', 'ge': '(s.sf == s.of)', 'nl': '(s.sf == s.of)',
    'le': '(s.zf || s.sf != s.of)', 'ng': '(s.zf || s.sf != s.of)', 'g': '(!s.zf && s.sf == s.of)',
    'nle': '(!s.zf && s.sf == s.of)', 'o': 's.of', 'no': '!s.of', 'p': 's.pf', 'pe': 's.pf', 'np': '!s.pf',
    'po': '!s.pf',
}


class Unsupported(Exception):
    pass


class TextCache:
    """Whole-.text disassembly of the exe, made once (llvm-objdump, ~15 s) and kept in out/cache/ keyed by the exe's
    SHA-256 (derived from the exe: git-ignored, never committed), with an address -> file offset index so a range is
    read without re-running objdump (one objdump per lifted function cost about a minute per link)."""

    def __init__(self, exe, text_range, cache_dir):
        import array
        import bisect
        import hashlib
        self.bisect = bisect
        key = hashlib.sha256(open(exe, 'rb').read()).hexdigest()[:16]
        self.lo, self.hi = text_range
        self.dis = cache_dir / ('text_%s.dis' % key)
        idx = cache_dir / ('text_%s.idx' % key)
        if not self.dis.exists():
            cache_dir.mkdir(parents=True, exist_ok=True)
            with open(self.dis, 'w') as f:
                subprocess.run([OBJDUMP, '-d', '-M', 'intel', '--start-address=%#x' % self.lo,
                                '--stop-address=%#x' % self.hi, exe], stdout=f, text=True)
        self.addrs, self.offs = array.array('I'), array.array('Q')
        if idx.exists():
            raw = idx.read_bytes()
            n = len(raw) // 12
            self.addrs.frombytes(raw[:n * 4])
            self.offs.frombytes(raw[n * 4:])
        else:
            pos, head = 0, re.compile(rb'^\s+([0-9a-f]+):')      # instruction lines start with whitespace
            with open(self.dis, 'rb') as f:
                for line in f:
                    m = head.match(line)
                    if m:
                        self.addrs.append(int(m.group(1), 16))
                        self.offs.append(pos)
                    pos += len(line)
            idx.write_bytes(self.addrs.tobytes() + self.offs.tobytes())

    def covers(self, lo, hi):
        return self.lo <= lo and hi <= self.hi

    def lines(self, lo, hi):
        k = self.bisect.bisect_left(self.addrs, lo)
        if k >= len(self.addrs):
            return []
        out = []
        with open(self.dis, 'rb') as f:
            f.seek(self.offs[k])
            for line in f:
                m = re.match(rb'^\s+([0-9a-f]+):', line)
                if m and int(m.group(1), 16) >= hi:
                    break
                out.append(line.decode('latin1').rstrip('\r\n'))
        return out


_TEXT_CACHE = {}


def text_cache(exe, text_range):
    from pathlib import Path
    if exe not in _TEXT_CACHE:
        try:
            _TEXT_CACHE[exe] = TextCache(exe, text_range, Path(__file__).resolve().parent.parent / 'out/cache')
        except OSError:
            _TEXT_CACHE[exe] = None
    return _TEXT_CACHE[exe]


def num(s):
    s = s.strip()
    neg = s.startswith('-')
    v = int(s.lstrip('-'), 0)
    return -v if neg else v


class Lifter:
    def __init__(self, exe, resolve_code, text_range, gap_end=None, allow=None):
        """resolve_code(addr) -> ('sym', decorated) | None for an original code address.
        gap_end(addr) -> end of the gap function at addr (next recipe start); allow(addr) -> may it be lifted."""
        self.exe = exe
        self.resolve_code = resolve_code
        self.text = text_range
        self.gap_end = gap_end or (lambda a: a + 0x400)
        self.allow = allow or (lambda a: LIFT_REGION[0] <= a < LIFT_REGION[1])
        self.ins = {}
        self.ranges = []
        self.externs = {}
        self.traps = set()
        self.unsupported = {}
        self.lifted = {}          # address -> body source
        self.ret_pop = {}         # address -> bytes popped by its ret
        self.queue = []
        self.disassemble(LIFT_REGION[0], LIFT_REGION[1])

    # ---- disassembly ---------------------------------------------------------------------------------------------
    def disassemble(self, lo, hi, force=False):
        if not force and any(a <= lo and hi <= b for a, b in self.ranges):
            return
        cache = text_cache(self.exe, self.text)
        if not force and cache and cache.covers(lo, hi):
            lines = cache.lines(lo, hi)
        else:
            lines = subprocess.run([OBJDUMP, '-d', '-M', 'intel', '--start-address=%#x' % lo,
                                    '--stop-address=%#x' % hi, self.exe], capture_output=True, text=True).stdout
            lines = lines.splitlines()
        for l in lines:
            m = LINE.match(l)
            if m:
                raw = bytes.fromhex(re.sub(r'\s', '', m.group(2)))
                ops = re.sub(r'\s*#.*$', '', re.sub(r'\s*<[^>]*>\s*$', '', m.group(4))).strip()
                mnem = m.group(3)
                if mnem in ('rep', 'repe', 'repne', 'lock'):
                    part = ops.split(None, 1)
                    if not part:                    # objdump printed the prefix on a line of its own
                        mnem, ops = 'nop', ''
                    else:
                        mnem, ops = (mnem + ' ' + part[0]) if mnem != 'lock' else part[0], \
                            (part[1] if len(part) > 1 else '')
                self.ins[int(m.group(1), 16)] = (len(raw), raw, mnem, ops)
        self.ranges.append((lo, hi))

    # ---- code pointers ------------------------------------------------------------------------------------------
    def code_ptr(self, addr):
        r = self.resolve_code(addr)
        if r and r[0] == 'sym':
            ident = 'X_' + re.sub(r'\W', '_', r[1])[:60] + '_%08x' % addr
            self.externs[ident] = r[1]
            return '(void*)&%s' % ident
        if self.allow(addr):
            if addr not in self.lifted and addr not in self.queue:
                self.queue.append(addr)
            return '(void*)&entry_%08x' % addr
        self.traps.add(addr)
        return '(void*)&lift_trap_%08x' % addr

    def is_code(self, v):
        return self.text[0] <= (v & 0xffffffff) < self.text[1]

    # ---- operands -----------------------------------------------------------------------------------------------
    def mem_addr(self, inner):
        inner = re.sub(r'^\w+:', '', inner.strip())
        expr = []
        for sign, t in re.findall(r'([+-]?)\s*([^+-]+)', inner.replace(' ', '')):
            sign = sign or '+'
            if '*' in t:
                r, sc = t.split('*')
                if r not in REG32:
                    r, sc = sc, r
                expr.append('%s s.%s*%s' % (sign, r, sc))
            elif t in REG32:
                expr.append('%s s.%s' % (sign, t))
            else:
                expr.append('%s %#xu' % (sign, num(t) & 0xffffffff))
        return '(uint32_t)(0u %s)' % ' '.join(expr)

    def parse(self, op):
        op = op.strip()
        m = re.match(r'^(?:(byte|word|dword|qword|xmmword|tbyte) ptr )?(\w+:)?\[(.*)\]$', op)
        if m:
            if m.group(2) == 'fs:':
                off = num(m.group(3).strip())
                if off == 0:
                    return ('fs', 32, 's.fs0')       # SEH chain head: private to the lifted call
                return ('fsr', 32, off)              # other TEB fields: read-only (fs:[0x2c] = TLS array)
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
        if k == 'fsr':
            return 'fnv_lift_tls_array()' if e == 0x2c else '(uint32_t)__readfsdword(%#x)' % e
        if k == 'reg16':
            return '(s.%s & 0xffffu)' % e
        if k == 'reg8':
            r, sh = REG8[e]
            return '((s.%s >> %d) & 0xffu)' % (r, sh)
        if k == 'imm':
            if self.is_code(e) and size == 32:
                return '(uint32_t)(uintptr_t)%s' % self.code_ptr(e & 0xffffffff)
            mask = {8: 0xff, 16: 0xffff}.get(size, 0xffffffff)
            return '%#xu' % (e & mask)
        if k == 'mem':
            return {8: 'M8(%s)', 16: 'M16(%s)', 32: 'M32(%s)'}[size] % e
        raise Unsupported('read ' + k)

    def code_jump_table(self, addr, entry, index_reg, jump_addr):
        """Read bounded absolute jump-table entries embedded in the original .text section."""
        if not self.is_code(addr):
            return None
        ins = sorted((a for a in self.ins if entry <= a < jump_addr), reverse=True)
        indices = None
        # Some compiler CRT tables are indexed by a register masked immediately before dispatch.
        for a in ins:
            _, _, mnem, ops = self.ins[a]
            if mnem == 'and' and ops.startswith(index_reg + ', '):
                try:
                    limit = num(ops.split(',', 1)[1])
                    if 0 <= limit <= 255:
                        indices = list(range(limit + 1))
                        break
                except ValueError:
                    pass
        # A short-length path can subtract N and branch on borrow, then use the wrapped index to address
        # the preceding N jump-table entries (a common VC CRT memcpy/memmove layout).
        if indices is None:
            for k, a in enumerate(ins[:-1]):
                branch = self.ins[a]
                sub = self.ins[ins[k + 1]]
                if branch[2] in ('jb', 'jc', 'jnae') and num(branch[3].split()[0]) == jump_addr and \
                        sub[2] == 'sub' and sub[3].startswith(index_reg + ', '):
                    try:
                        n = num(sub[3].split(',', 1)[1])
                        if 1 <= n <= 16:
                            indices = list(range(-n, 0))
                    except ValueError:
                        pass
                    if indices is not None:
                        break
        # Otherwise use a nearby compare against the same index register.
        if indices is None:
            for a in ins:
                _, _, mnem, ops = self.ins[a]
                if mnem == 'cmp' and ops.startswith(index_reg + ', '):
                    try:
                        limit = num(ops.split(',', 1)[1])
                        if 0 <= limit <= 255:
                            indices = list(range(limit + 1))
                            break
                    except ValueError:
                        pass
        if not hasattr(self, '_exe_sections'):
            data = open(self.exe, 'rb').read()
            pe, = struct.unpack_from('<I', data, 0x3c)
            _, count, _, _, _, opt_size, _ = struct.unpack_from('<HHIIIHH', data, pe + 4)
            opt = pe + 24
            base, = struct.unpack_from('<I', data, opt + 28)
            sec = opt + opt_size
            self._exe_sections = []
            for i in range(count):
                _, _, rva, raw_size, raw, *_ = struct.unpack_from('<8sIIIIIIHHI', data, sec + i * 40)
                self._exe_sections.append((base + rva, base + rva + raw_size, raw))
            self._exe_data = data
        if not indices:
            # Some dispatches load the selector from a local, so its bound is not adjacent to the jump.
            # Accept a consecutive run of code pointers and stop at the first non-code word.
            indices = []
            for i in range(256):
                va = addr + i * 4
                match = next(((lo, hi, raw) for lo, hi, raw in self._exe_sections
                              if lo <= va and va + 4 <= hi), None)
                if match is None:
                    break
                lo, _, raw = match
                target, = struct.unpack_from('<I', self._exe_data, raw + va - lo)
                if not self.is_code(target):
                    break
                indices.append(i)
            if len(indices) < 2:
                return None
        values = []
        for i in indices:
            va = (addr + i * 4) & 0xffffffff
            match = next(((lo, hi, raw) for lo, hi, raw in self._exe_sections if lo <= va and va + 4 <= hi), None)
            if match is None:
                return None
            lo, _, raw = match
            target, = struct.unpack_from('<I', self._exe_data, raw + va - lo)
            values.append((i, target if self.is_code(target) else None))
        return values if any(target is not None for _, target in values) else None
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
    def stmt(self, addr, n, raw, mnem, ops, labels, entry, end):
        args = [a for a in re.split(r',(?![^\[]*\])', ops) if a.strip()] if ops else []
        branch = mnem in ('call', 'jmp') or (mnem.startswith('j') and mnem[1:] in COND)
        P = [self.parse(a) for a in args] if not branch else []

        if mnem in ('nop', 'int3', 'wait', 'fwait'):
            return ''
        if mnem in ('mov', 'movzx', 'movsx'):
            src = self.read(P[1])
            if mnem == 'movsx':
                src = '(uint32_t)(int32_t)(int%d_t)(%s)' % (P[1][1], src)
            return self.write(P[0], src)
        if mnem == 'lea':
            return self.write(P[0], P[1][2])
        if mnem == 'xchg':
            return '{ uint32_t t_ = %s; %s %s }' % (self.read(P[0]), self.write(P[0], self.read(P[1])), self.write(P[1], 't_'))
        if mnem == 'push':
            return 'PUSH(%s);' % self.read(P[0], 32)
        if mnem == 'pop':
            return self.write(P[0], 'POP()')
        if mnem in ('add', 'sub', 'cmp'):
            bits = P[0][1]
            a, b = self.read(P[0]), self.read(P[1], bits)
            fl = 'ADDF' if mnem == 'add' else 'SUBF'
            st = '{ uint32_t a_ = %s, b_ = %s, r_ = a_ %s b_; %s(a_, b_, r_, %d);' % (a, b, '+' if mnem == 'add' else '-', fl, bits)
            return st + (' %s }' % self.write(P[0], 'r_') if mnem != 'cmp' else ' }')
        if mnem in ('and', 'or', 'xor', 'test'):
            bits = P[0][1]
            op = {'and': '&', 'test': '&', 'or': '|', 'xor': '^'}[mnem]
            st = '{ uint32_t r_ = (%s) %s (%s); LOGICF(r_, %d);' % (self.read(P[0]), op, self.read(P[1], bits), bits)
            return st + (' %s }' % self.write(P[0], 'r_') if mnem != 'test' else ' }')
        if mnem in ('shl', 'shr', 'sar', 'sal'):
            a, b = self.read(P[0]), (self.read(P[1]) if len(P) > 1 else '1u')
            v = {'shl': '(uint32_t)(a_ << c_)', 'sal': '(uint32_t)(a_ << c_)', 'shr': '(uint32_t)(a_ >> c_)',
                 'sar': '(uint32_t)((int32_t)a_ >> c_)'}[mnem]
            return '{ uint32_t a_ = %s, c_ = (%s) & 31u; if (c_) { uint32_t r_ = %s; LOGICF(r_, %d); %s } }' % (
                a, b, v, P[0][1], self.write(P[0], 'r_'))
        if mnem == 'imul' and len(P) in (2, 3):
            a, b = (self.read(P[1]), self.read(P[2])) if len(P) == 3 else (self.read(P[0]), self.read(P[1]))
            return '{ int64_t p_ = (int64_t)(int32_t)(%s) * (int32_t)(%s); %s s.cf = s.of = p_ != (int32_t)p_; }' % (
                a, b, self.write(P[0], '(uint32_t)p_'))
        if mnem in ('mul', 'imul', 'div', 'idiv') and len(P) == 1:
            v = self.read(P[0])
            return {
                'mul': '{ uint64_t p_ = (uint64_t)s.eax * (uint32_t)(%s); s.eax = (uint32_t)p_; s.edx = (uint32_t)(p_ >> 32); s.cf = s.of = s.edx != 0; }',
                'imul': '{ int64_t p_ = (int64_t)(int32_t)s.eax * (int32_t)(%s); s.eax = (uint32_t)p_; s.edx = (uint32_t)((uint64_t)p_ >> 32); }',
                'div': '{ uint64_t n_ = ((uint64_t)s.edx << 32) | s.eax; uint32_t d_ = (%s); s.eax = (uint32_t)(n_ / d_); s.edx = (uint32_t)(n_ %% d_); }',
                'idiv': '{ int64_t n_ = (int64_t)(((uint64_t)s.edx << 32) | s.eax); int32_t d_ = (int32_t)(%s); s.eax = (uint32_t)(int32_t)(n_ / d_); s.edx = (uint32_t)(int32_t)(n_ %% d_); }',
            }[mnem] % v
        if mnem in ('neg', 'not', 'inc', 'dec'):
            a, bits = self.read(P[0]), P[0][1]
            if mnem == 'not':
                return self.write(P[0], '~(%s)' % a)
            if mnem == 'neg':
                return '{ uint32_t b_ = %s, r_ = 0u - b_; SUBF(0u, b_, r_, %d); %s }' % (a, bits, self.write(P[0], 'r_'))
            # inc/dec leave CF alone
            return '{ int cf_ = s.cf; uint32_t a_ = %s, r_ = a_ %s 1u; %s(a_, 1u, r_, %d); s.cf = cf_; %s }' % (
                a, '+' if mnem == 'inc' else '-', 'ADDF' if mnem == 'inc' else 'SUBF', bits, self.write(P[0], 'r_'))
        if mnem in ('sbb', 'adc'):
            bits = P[0][1]
            a, b = self.read(P[0]), self.read(P[1], bits)
            if mnem == 'sbb':
                return '{ uint32_t a_ = %s, b_ = (%s) + (uint32_t)s.cf, r_ = a_ - b_; SUBF(a_, b_, r_, %d); %s }' % (
                    a, b, bits, self.write(P[0], 'r_'))
            return '{ uint32_t a_ = %s, b_ = (%s) + (uint32_t)s.cf, r_ = a_ + b_; ADDF(a_, b_, r_, %d); %s }' % (
                a, b, bits, self.write(P[0], 'r_'))
        if mnem.startswith('set') and mnem[3:] in COND:
            return self.write(P[0], '(%s) ? 1u : 0u' % COND[mnem[3:]])
        if mnem.startswith('cmov') and mnem[4:] in COND:
            return 'if (%s) { %s }' % (COND[mnem[4:]], self.write(P[0], self.read(P[1])))
        if mnem == 'cdq':
            return 's.edx = (s.eax & 0x80000000u) ? 0xffffffffu : 0u;'
        if mnem == 'pushfd':
            return 'PUSH(((uint32_t)s.cf) | 2u | ((uint32_t)s.pf << 2) | ((uint32_t)s.zf << 6) | ((uint32_t)s.sf << 7) | 0x200u | ((uint32_t)s.of << 11) | lift_flags_id());'
        if mnem == 'popfd':
            return '{ uint32_t f_ = POP(); s.cf = f_ & 1; s.pf = (f_ >> 2) & 1; s.zf = (f_ >> 6) & 1; s.sf = (f_ >> 7) & 1; s.of = (f_ >> 11) & 1; lift_flags_id() = f_ & 0x200000u; }'
        if mnem == 'cpuid':
            return '{ int r_[4]; __cpuidex(r_, (int)s.eax, (int)s.ecx); s.eax = r_[0]; s.ebx = r_[1]; s.ecx = r_[2]; s.edx = r_[3]; }'
        if mnem == 'rdtsc':
            return '{ uint64_t t_ = __rdtsc(); s.eax = (uint32_t)t_; s.edx = (uint32_t)(t_ >> 32); }'
        if mnem.startswith('prefetch'):
            return ''
        if mnem == 'shufps':
            d, s_, imm = P[0], P[1], P[2][2]
            src = 'X(%s)' % s_[2] if s_[0] == 'xmm' else '(const uint8_t*)(uintptr_t)%s' % s_[2]
            return 'SHUFPS(X(%s), %s, %du);' % (d[2], src, imm & 0xff)
        if mnem in ('fiadd', 'fimul', 'fisub', 'fisubr', 'fidiv', 'fidivr') and P and P[0][0] == 'mem':
            v = '(double)(int%d_t)M%d(%s)' % (P[0][1], P[0][1], P[0][2])
            f = {'fiadd': 'ST(0) + %s', 'fimul': 'ST(0) * %s', 'fisub': 'ST(0) - %s', 'fisubr': '%s - ST(0)',
                 'fidiv': 'ST(0) / %s', 'fidivr': '%s / ST(0)'}[mnem]
            return 'ST(0) = %s;' % (f % v)
        if mnem == 'cwde':
            return 's.eax = (uint32_t)(int32_t)(int16_t)s.eax;'
        if mnem == 'leave':
            return 's.esp = s.ebp; s.ebp = POP();'
        if mnem == 'frndint':
            return 'ST(0) = nearbyint(ST(0));'
        if mnem == 'sahf':
            return 'SAHF();'
        if mnem == 'jmp' and ops.strip() in REG32:
            return 'TAILCALL((void*)(uintptr_t)s.%s);' % ops.strip()
        if mnem == 'jmp' and ops.strip().startswith('dword ptr'):
            operand = self.parse(ops.strip())
            if operand[0] == 'mem':
                base = re.search(r'0x[0-9a-f]+u', operand[2])
                if base:
                    table_addr = int(base.group(0)[:-1], 16)
                    index = re.search(r's\.(eax|ecx|edx|ebx|esi|edi|ebp)\*4', operand[2])
                    targets = self.code_jump_table(table_addr, entry, index.group(1), addr) if index else None
                    if targets:
                        cases = []
                        for i, target in targets:
                            if target is None:
                                action = 'fnv_trap(0x%08xu, "lifted: invalid embedded jump-table entry"); RETURN(0);' % table_addr
                            elif entry <= target < end:
                                if target not in self.ins:
                                    self.disassemble(target, min(target + 0x100, end), force=True)
                                labels.add(target)
                                action = 'goto L_%08x;' % target
                            else:
                                action = 'TAILCALL(%s);' % self.code_ptr(target)
                            cases.append('case %d: %s' % (i & 0xffffffff, action))
                        cases = ' '.join(cases)
                        return 'switch (s.%s) { %s default: fnv_trap(0x%08xu, "lifted: invalid code jump-table index"); } RETURN(0);' % (index.group(1), cases, table_addr)
            return 'TAILCALL((void*)(uintptr_t)%s);' % self.read(operand)
        if mnem.startswith('j') and (mnem == 'jmp' or mnem[1:] in COND):
            tgt = num(ops.split()[0])
            if entry <= tgt < end and tgt not in self.stub_starts - {entry}:
                self.disassemble(entry, end)
                if tgt in self.ins:
                    labels.add(tgt)
                    return ('if (%s) goto L_%08x;' % (COND[mnem[1:]], tgt)) if mnem != 'jmp' else 'goto L_%08x;' % tgt
            if mnem != 'jmp':
                target = self.resolve_code(tgt)
                if (target and target[0] == 'sym') or self.is_code(tgt):
                    return 'if (%s) { TAILCALL(%s); }' % (
                        COND[mnem[1:]], self.code_ptr(tgt))
                raise Unsupported('conditional jump out of function')
            return 'TAILCALL(%s);' % self.code_ptr(tgt)       # tail call
        if mnem == 'call':
            o = ops.strip()
            if o.startswith('dword ptr'):
                return 'CALL((void*)(uintptr_t)%s);' % self.read(self.parse(o))
            if o in REG32:
                return 'CALL((void*)(uintptr_t)s.%s);' % o
            target = num(o.split()[0])
            # OpenAI model: these VC9 SEH helpers establish/restore the *caller* frame.
            # A normal lifted CALL creates an independent state and loses EAX, EBP,
            # ESP and fs:[0]. Emulate the call and helper on the current state.
            if target == 0x00ece255:
                return ('PUSH(0x%08xu); PUSH(s.eax); PUSH(s.fs0); '
                        's.eax = s.esp + 0xcu; '
                        '{ uint32_t a_ = s.esp, b_ = M32(s.esp + 0xcu), r_ = a_ - b_; '
                        'SUBF(a_, b_, r_, 32); s.esp = r_; } '
                        'PUSH(s.ebx); PUSH(s.esi); PUSH(s.edi); M32(s.eax) = s.ebp; '
                        's.ebp = s.eax; s.eax = M32(0x011c16bcu) ^ s.ebp; '
                        'LOGICF(s.eax, 32); PUSH(s.eax); PUSH(M32(s.ebp - 4u)); '
                        'M32(s.ebp - 4u) = 0xffffffffu; s.eax = s.ebp - 0xcu; '
                        's.fs0 = s.eax; (void)POP();') % (addr + n)
            if target == 0x00ece2be:
                return ('PUSH(0x%08xu); s.ecx = M32(s.ebp - 0xcu); s.fs0 = s.ecx; '
                        's.ecx = POP(); s.edi = POP(); s.edi = POP(); s.esi = POP(); '
                        's.ebx = POP(); s.esp = s.ebp; s.ebp = POP(); '
                        'PUSH(s.ecx); (void)POP();') % (addr + n)
            if target == 0x00ed1714:      # __SEH_prolog4: same shape as 0xece255 with a handler/scope table and frame size
                return ('PUSH(0x%08xu); PUSH(0x00ecc8e0u); PUSH(s.fs0); '
                        's.eax = M32(s.esp + 0x10u); M32(s.esp + 0x10u) = s.ebp; s.ebp = s.esp + 0x10u; '
                        '{ uint32_t a_ = s.esp, b_ = s.eax, r_ = a_ - b_; SUBF(a_, b_, r_, 32); s.esp = r_; } '
                        'PUSH(s.ebx); PUSH(s.esi); PUSH(s.edi); '
                        's.eax = M32(0x011c16bcu); M32(s.ebp - 4u) ^= s.eax; s.eax ^= s.ebp; LOGICF(s.eax, 32); '
                        'PUSH(s.eax); M32(s.ebp - 0x18u) = s.esp; PUSH(M32(s.ebp - 8u)); '
                        's.eax = M32(s.ebp - 4u); M32(s.ebp - 4u) = 0xfffffffeu; M32(s.ebp - 8u) = s.eax; '
                        's.eax = s.ebp - 0x10u; s.fs0 = s.eax; (void)POP();') % (addr + n)
            if target == 0x00ed1759:      # __SEH_epilog4
                return ('PUSH(0x%08xu); s.ecx = M32(s.ebp - 0x10u); s.fs0 = s.ecx; '
                        's.ecx = POP(); s.edi = POP(); s.edi = POP(); s.esi = POP(); '
                        's.ebx = POP(); s.esp = s.ebp; s.ebp = POP(); '
                        'PUSH(s.ecx); (void)POP();') % (addr + n)
            resolved = self.resolve_code(target)
            if resolved and any(name in resolved[1].lower() for name in ('chkstk', 'alloca_probe')):
                return 's.esp -= s.eax;'
            return 'CALL(%s);' % self.code_ptr(target)
        if mnem == 'ret':
            pop = num(ops) if ops else 0
            prev = self.ret_pop.setdefault(entry, pop)
            if prev != pop:
                self.ret_pop[entry] = max(prev, pop)
            return 'RETURN(%d);' % pop
        if mnem in ('rep stosd', 'rep movsd', 'rep stosb', 'rep movsb'):
            w = 4 if mnem.endswith('d') else 1
            m = 'M32' if w == 4 else 'M8'
            body = ('%s(s.edi) = s.eax; s.edi += %d;' % (m, w)) if 'stos' in mnem else \
                ('%s(s.edi) = %s(s.esi); s.edi += %d; s.esi += %d;' % (m, m, w, w))
            return 'while (s.ecx) { %s s.ecx--; }' % body
        # ---- x87 (register forms decoded from the opcode bytes) ----
        if raw[0] in (0xD8, 0xDC, 0xDE) and len(raw) == 2 and raw[1] >= 0xC0:
            i, grp = raw[1] & 7, (raw[1] >> 3) & 7
            if grp in (2, 3):         # fcom/fcomp st(i) (D8), fcompp (DE D9)
                return 'FCOM(ST(0), ST(%d));%s' % (i, {(0xD8, 3): ' FPOP();', (0xDE, 3): ' FPOP(); FPOP();'}.get((raw[0], grp), ''))
            if raw[0] == 0xD8:
                f = {0: 'ST(0) + ST(%d)', 1: 'ST(0) * ST(%d)', 4: 'ST(0) - ST(%d)', 5: 'ST(%d) - ST(0)',
                     6: 'ST(0) / ST(%d)', 7: 'ST(%d) / ST(0)'}[grp]
                return 'ST(0) = %s;' % (f % i)
            f = {0: 'ST(%d) + ST(0)', 1: 'ST(%d) * ST(0)', 4: 'ST(0) - ST(%d)', 5: 'ST(%d) - ST(0)',
                 6: 'ST(0) / ST(%d)', 7: 'ST(%d) / ST(0)'}[grp]
            return 'ST(%d) = %s;%s' % (i, f % i, ' FPOP();' if raw[0] == 0xDE else '')
        if raw[0] == 0xD9 and len(raw) == 2 and raw[1] >= 0xC0:
            b = raw[1]
            if 0xC0 <= b <= 0xC7:
                return '{ double v_ = ST(%d); FPUSH(v_); }' % (b & 7)
            if 0xC8 <= b <= 0xCF:
                return '{ double v_ = ST(0); ST(0) = ST(%d); ST(%d) = v_; }' % (b & 7, b & 7)
            return {0xE0: 'ST(0) = -ST(0);', 0xE1: 'ST(0) = fabs(ST(0));', 0xE4: 'FCOM(ST(0), 0.0);',
                    0xE8: 'FPUSH(1.0);', 0xEE: 'FPUSH(0.0);', 0xFA: 'ST(0) = sqrt(ST(0));',
                    0xFB: '{ double v_ = ST(0); ST(0) = sin(v_); FPUSH(cos(v_)); }', 0xFE: 'ST(0) = sin(ST(0));', 0xFF: 'ST(0) = cos(ST(0));'}.get(b) or self._unsup(mnem, ops)
        if raw[0] == 0xDD and len(raw) == 2 and raw[1] >= 0xC0:
            b = raw[1]
            if 0xD0 <= b <= 0xD7:
                return 'ST(%d) = ST(0);' % (b & 7)
            if 0xD8 <= b <= 0xDF:
                return 'ST(%d) = ST(0); FPOP();' % (b & 7)
            if 0xE0 <= b <= 0xEF:     # fucom/fucomp st(i)
                return 'FCOM(ST(0), ST(%d));%s' % (b & 7, ' FPOP();' if b >= 0xE8 else '')
            return self._unsup(mnem, ops)
        if raw[0] == 0xDA and raw[1:2] == b'\xe9':
            return 'FCOM(ST(0), ST(1)); FPOP(); FPOP();'          # fucompp
        if raw[0] == 0xDF and raw[1:2] == b'\xe0':
            return 's.eax = (s.eax & 0xffff0000u) | FSW();'       # fnstsw ax
        if mnem in ('fnstsw', 'fstsw') and P and P[0][0] == 'mem':
            return 'M16(%s) = (uint16_t)FSW();' % P[0][2]
        if mnem in ('fnstcw', 'fstcw') and P and P[0][0] == 'mem':
            return 'M16(%s) = 0x027fu;' % P[0][2]
        if mnem == 'fldcw':
            return ''                                              # rounding/precision control is not modelled
        if mnem in ('fld', 'fild', 'fst', 'fstp', 'fistp', 'fist', 'fadd', 'fmul', 'fsub', 'fsubr', 'fdiv', 'fdivr',
                    'fcom', 'fcomp'):
            o = P[0]
            if o[0] != 'mem':
                return self._unsup(mnem, ops)
            if mnem == 'fild':
                return 'FPUSH((double)(int%d_t)M%d(%s));' % (o[1], o[1], o[2])
            if mnem in ('fist', 'fistp'):
                return 'M%d(%s) = (uint%d_t)(int%d_t)lrint(ST(0));%s' % (o[1], o[2], o[1], o[1],
                                                                        ' FPOP();' if mnem == 'fistp' else '')
            load = {32: 'MF(%s)', 64: 'MD(%s)'}.get(o[1])
            if load is None:
                return self._unsup(mnem, ops)
            v = load % o[2]
            if mnem == 'fld':
                return 'FPUSH(%s);' % v
            if mnem in ('fcom', 'fcomp'):
                return 'FCOM(ST(0), %s);%s' % (v, ' FPOP();' if mnem == 'fcomp' else '')
            if mnem in ('fst', 'fstp'):
                store = {32: 'MF(%s) = (float)ST(0);', 64: 'MD(%s) = ST(0);'}[o[1]] % o[2]
                return store + (' FPOP();' if mnem == 'fstp' else '')
            f = {'fadd': 'ST(0) + %s', 'fmul': 'ST(0) * %s', 'fsub': 'ST(0) - %s', 'fsubr': '%s - ST(0)',
                 'fdiv': 'ST(0) / %s', 'fdivr': '%s / ST(0)'}[mnem]
            return 'ST(0) = %s;' % (f % v)
        # ---- SSE ----
        if mnem in ('addps', 'subps', 'mulps', 'divps', 'addss', 'subss', 'mulss', 'divss', 'minps', 'maxps'):
            d, s_ = P[0], P[1]
            src = 'X(%s)' % s_[2] if s_[0] == 'xmm' else '(const uint8_t*)(uintptr_t)%s' % s_[2]
            op = mnem[:3]
            return 'SSEF(X(%s), %s, %d, %s);' % (d[2], src, 1 if mnem.endswith('ss') else 4,
                                                {'add': 'FOP_ADD', 'sub': 'FOP_SUB', 'mul': 'FOP_MUL', 'div': 'FOP_DIV',
                                                 'min': 'FOP_MIN', 'max': 'FOP_MAX'}[op])
        if mnem in ('andps', 'andnps', 'orps', 'xorps'):
            d, s_ = P[0], P[1]
            src = 'X(%s)' % s_[2] if s_[0] == 'xmm' else '(const uint8_t*)(uintptr_t)%s' % s_[2]
            return 'SSEB(X(%s), %s, %d);' % (d[2], src, ('andps', 'andnps', 'orps', 'xorps').index(mnem))
        if mnem in ('movaps', 'movups', 'movss', 'unpcklps'):
            d, s_ = P[0], P[1]
            src = 'X(%s)' % s_[2] if s_[0] == 'xmm' else '(const uint8_t*)(uintptr_t)%s' % s_[2]
            if mnem in ('movaps', 'movups'):
                if d[0] == 'xmm':
                    return 'memcpy(X(%s), %s, 16);' % (d[2], src)
                return 'memcpy((uint8_t*)(uintptr_t)%s, X(%s), 16);' % (d[2], s_[2])
            if mnem == 'movss':
                if d[0] == 'xmm' and s_[0] == 'mem':
                    return 'memset(X(%s), 0, 16); memcpy(X(%s), %s, 4);' % (d[2], d[2], src)
                if d[0] == 'mem':
                    return 'memcpy((void*)(uintptr_t)%s, X(%s), 4);' % (d[2], s_[2])
                return 'memcpy(X(%s), X(%s), 4);' % (d[2], s_[2])
            return 'UNPCKLPS(X(%s), %s);' % (d[2], src)
        if mnem in ('sqrtss', 'rsqrtss', 'rcpps', 'unpckhps') and len(P) == 2:
            d, s_ = P[0], P[1]
            src = 'X(%s)' % s_[2] if s_[0] == 'xmm' else '(const uint8_t*)(uintptr_t)%s' % s_[2]
            return '%s(X(%s), %s);' % (mnem.upper(), d[2], src)
        if re.match(r'cmp(eq|lt|le|unord|neq|nlt|nle|ord)(ps|ss)$', mnem) and len(P) == 2:
            pred = ('eq', 'lt', 'le', 'unord', 'neq', 'nlt', 'nle', 'ord').index(re.match(r'cmp([a-z]+)(ps|ss)$', mnem).group(1))
            d, s_ = P[0], P[1]
            src = 'X(%s)' % s_[2] if s_[0] == 'xmm' else '(const uint8_t*)(uintptr_t)%s' % s_[2]
            return 'CMPPS(X(%s), %s, %d, %d);' % (d[2], src, pred, 1 if mnem.endswith('ss') else 4)
        if mnem == 'movmskps' and len(P) == 2:
            return self.write(P[0], 'MOVMSKPS(X(%s))' % P[1][2])
        if mnem == 'cvtsi2ss' and len(P) == 2:
            return '{ float f_ = (float)(int32_t)(%s); memcpy(X(%s), &f_, 4); }' % (self.read(P[1]), P[0][2])
        if mnem == 'bswap' and len(P) == 1:
            return self.write(P[0], '__builtin_bswap32(%s)' % self.read(P[0]))
        if mnem in ('rol', 'ror') and 1 <= len(P) <= 2:
            cnt = self.read(P[1]) if len(P) > 1 else '1u'
            fn = 'LIFT_ROL' if mnem == 'rol' else 'LIFT_ROR'
            return self.write(P[0], '%s(%s, %s, %d)' % (fn, self.read(P[0]), cnt, P[0][1]))
        if mnem == 'movlhps' and len(P) == 2:
            return 'memcpy(X(%s) + 8, X(%s), 8);' % (P[0][2], P[1][2])
        if mnem == 'rsqrtps' and len(P) == 2:
            src = 'X(%s)' % P[1][2] if P[1][0] == 'xmm' else '(const uint8_t*)(uintptr_t)%s' % P[1][2]
            return '{ float a_[4]; memcpy(a_, %s, 16); for (int i_ = 0; i_ < 4; ++i_) a_[i_] = 1.0f / sqrtf(a_[i_]); memcpy(X(%s), a_, 16); }' % (src, P[0][2])
        if mnem == 'fsincos':
            return '{ double v_ = ST(0); ST(0) = sin(v_); FPUSH(cos(v_)); }'
        return self._unsup(mnem, ops)

    def _unsup(self, mnem, ops):
        raise Unsupported('%s %s' % (mnem, ops))

    # ---- one function -------------------------------------------------------------------------------------------
    def lift(self, entry):
        end = entry + 0x2000 if LIFT_REGION[0] <= entry < LIFT_REGION[1] else self.gap_end(entry)
        self.disassemble(entry, end)
        body, labels, todo, seen = {}, set(), [entry], set()
        while todo:
            a = todo.pop()
            while a in self.ins and a not in seen and a < end:
                seen.add(a)
                n, raw, mnem, ops = self.ins[a]
                try:
                    st = self.stmt(a, n, raw, mnem, ops, labels, entry, end)
                except Unsupported as e:
                    self.unsupported[a] = str(e)
                    st = 'fnv_trap(0x%08xu, "lifted: unsupported instruction"); RETURN(0);' % a
                body[a] = st
                for t in labels:
                    if t not in seen and t not in todo:
                        todo.append(t)
                if mnem == 'ret' or 'RETURN(' in st:
                    break
                a += n
        lines = ['static void body_%08x(uint32_t in_ecx, uint32_t in_edx, const uint32_t* in_args, FnvLiftReturn* out)'
                 % entry, '{', '    LIFT_STATE(in_ecx, in_edx, in_args, out);']
        for a in sorted(body):
            if a in labels:
                lines.append('L_%08x:' % a)
            lines.append('    %s  // %08x %s %s' % (body[a], a, self.ins[a][2], self.ins[a][3][:60]))
        lines.append('    RETURN(0);')
        lines.append('}')
        self.lifted[entry] = '\n'.join(lines)

    def run(self, entries):
        self.stub_starts = set(entries)
        self.queue = list(entries)
        while self.queue:
            e = self.queue.pop(0)
            if e not in self.lifted:
                self.lift(e)

    def source(self):
        return self.shards(1)[0]

    def shards(self, n):
        """The lifted code as n translation units (compiled in parallel): each carries every declaration and the
        static trap helpers, and defines its own share of the functions."""
        head = ['// Generated by tools/lift_stubs.py from the player\'s FalloutNV.exe: do not commit (rule 1).',
                '#include "fnv_lift.h"', '']
        for ident, sym in sorted(self.externs.items()):
            head.append('extern "C" void %s(void) __asm__("%s");' % (ident, sym))
        for a in sorted(self.lifted):
            head.append('extern "C" void entry_%08x(void) __asm__("fnv_f_%08x");' % (a, a))
        for a in sorted(self.traps):
            head.append('static void lift_trap_%08x(void) { fnv_trap(0x%08xu, "lifted: unbound code pointer"); }'
                        % (a, a))
        head.append('')
        funcs = sorted(self.lifted)
        out = []
        for k in range(n):
            body = []
            for a in funcs[k::n]:
                body.append(self.lifted[a])
                body.append(LIFT_ENTRY.format(a=a, pop=self.ret_pop.get(a, 0)))
            out.append('\n'.join(head + body) + '\n')
        return out


# Native entry: the original interface (ecx/edx, stack arguments, eax/edx/st0 result, `ret N`) around the body.
LIFT_ENTRY = '''extern "C" __declspec(naked) void entry_{a:08x}(void)
{{
    __asm {{
        push ebp
        mov ebp, esp
        sub esp, 24
        lea eax, [ebp - 24]
        push eax
        lea eax, [ebp + 4]
        push eax
        push edx
        push ecx
        call body_{a:08x}
        add esp, 16
        cmp dword ptr [ebp - 16], 0
        je no_fpu
        fld qword ptr [ebp - 8]
    no_fpu:
        mov ecx, [ebp - 12]
        mov eax, [ebp + 4]
        mov [ebp + ecx + 4], eax
        mov eax, [ebp - 24]
        mov edx, [ebp - 20]
        mov esp, ebp
        pop ebp
        add esp, ecx
        ret
    }}
}}
extern "C" __declspec(naked) void alias_c_{a:08x}(void) __asm__("fnv_c_{a:08x}");
extern "C" __declspec(naked) void alias_c_{a:08x}(void) {{ __asm jmp entry_{a:08x} }}
'''
