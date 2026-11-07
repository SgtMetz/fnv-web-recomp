#!/usr/bin/env python3
"""Whole-game recompiler: every function of FalloutNV.exe as portable C++ (docs/RECOMP.md).

Reuses the Phase 1 lifter's instruction translation (tools/lift_stubs.py) and changes the call boundary. All
functions share one FnvCpu per thread and a real guest stack: `call` pushes the original return address and calls
the target's host function, `ret N` pops it plus N bytes and returns, indirect calls/jumps dispatch on the original
address. Code addresses stay original values everywhere (immediates, jump tables, the data image), and `fs:` is
memory at the thread's TEB block.

Entries: every recipe function start (build/units.json), every relocation target in .text whose fixup lies outside
.text (vtables, EH tables, callback tables), code addresses used as immediates, the exe entry point, and every
direct call/jump target found while lifting.

Output (derived from the player's exe: git-ignored, never committed): out/recomp/recomp_NN.cpp, out/recomp/report.json.

usage: python tools/recomp.py --exe <FalloutNV.exe> [--shards 64] [--only 0xADDR,...]
"""
import argparse
import bisect
import json
import re
import struct
import sys
import time
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import lift_stubs as ls  # noqa: E402
from decomp_path import DECOMP_DIR  # noqa: E402
from relink import Image  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / 'out/recomp'
# CRT helpers whose register/x87 conventions the Phase 1 lowering handles inline (by the decomp's name)
INLINE_HELPERS = re.compile(r'^(ftol2_sse|ftol2|ftol|allshl|aullshr|allshr|ci(acos|asin|atan|cos|sin|tan|sqrt|log|'
                            r'log10|exp|cosh|sinh|tanh|pow|fmod|atan2))$|chkstk|alloca_probe')


# The exe's CRT math functions (addresses from port/compat/crt_map.json): called inline as C math, because their
# bodies select SSE2 implementations full of instructions nothing else in the game uses
CRT_MATH = {0x00ec6054: ('sqrt', 1), 0x00ec6cde: ('fabs', 1), 0x00ec9010: ('atan', 1), 0x00ec9130: ('fmod', 2),
            0x00ec9610: ('asin', 1), 0x00ec9ac0: ('pow', 2), 0x00ec9e10: ('ceil', 1), 0x00ec9f30: ('cos', 1),
            0x00eca060: ('sin', 1), 0x00eca190: ('acos', 1), 0x00eca590: ('tan', 1), 0x00eca920: ('log', 1),
            0x00ecae30: ('exp', 1), 0x00ecbc80: ('log10', 1)}


# CRT memory/string primitives with host implementations (port/recomp/fnv_intrinsics.cpp), by address from
# port/compat/crt_map.json: the recompiled function only calls the host one and returns (cdecl)
INTRINSICS = {0x00ec44d0: 'memmove', 0x00ec7230: 'memmove', 0x00ec61c0: 'memset', 0x00ec4835: 'memcmp',
              0x00ec6130: 'strlen', 0x00ec6370: 'strcpy', 0x00ec6380: 'strcat', 0x00ec6da0: 'strcmp',
              0x00ec7690: 'strchr', 0x00ec6e30: 'strrchr', 0x00ec7750: 'strstr', 0x00ec8a19: 'strncmp',
              0x00ecc7b0: 'strncpy', 0x00ecbf20: 'strpbrk', 0x00ecdb00: 'wcslen'}


class Recompiler(ls.Lifter):
    def __init__(self, exe, text, starts, sizes, names):
        self.starts = starts                  # sorted function starts (recipe)
        self.sizes = sizes                    # start -> size
        self.names = names                    # address -> decomp name (helpers only need it)
        super().__init__(exe, self._resolve, text, gap_end=self._end, allow=lambda a: text[0] <= a < text[1])

    # ---- context ------------------------------------------------------------------------------------------------
    def _resolve(self, addr):
        n = self.names.get(addr)
        if n and INLINE_HELPERS.search(n.lstrip('_?@').split('@')[0].lower()):
            return ('sym', n)
        return None

    def _end(self, a):
        """End of the code an entry at `a` may reach without a call: its recipe function, else the next start."""
        k = bisect.bisect_right(self.starts, a) - 1
        if k >= 0 and self.starts[k] <= a < self.starts[k] + self.sizes[self.starts[k]]:
            return self.starts[k] + self.sizes[self.starts[k]]
        nxt = self.starts[k + 1] if k + 1 < len(self.starts) else self.text[1]
        return min(nxt, a + 0x10000)

    def lift(self, entry):
        if entry in INTRINSICS:
            self.lifted[entry] = ('void fnv_i_%s(FnvCpu& s);\nvoid f_%08x(FnvCpu& s) { fnv_i_%s(s); s.esp += 4; }  '
                                  '// CRT %s (host)' % (INTRINSICS[entry], entry, INTRINSICS[entry], INTRINSICS[entry]))
            return
        # the base class bounds initializer-region entries by +0x2000; here every entry uses its function's end
        end = self._end(entry)
        hit = self.cache.get((entry, end))
        if hit and self.replay(entry, hit):
            self.cache_new[(entry, end)] = hit
            self.cache_hits += 1
            return
        self._log = []
        before = set(self.unsupported)
        self.lift_uncached(entry, end)
        unsupported = {a: v for a, v in self.unsupported.items() if a not in before}
        self.cache_new[(entry, end)] = (self.lifted[entry], self.ret_pop.get(entry), unsupported, self._log)
        self._log = None

    # ---- code addresses stay original ---------------------------------------------------------------------------
    def code_ptr_kind(self, addr):
        return '0x%08xu' % addr, None

    def code_ptr(self, addr):
        if self._log is not None:
            self._log.append(('c', addr, '0x%08xu' % addr))
        self.want(addr)
        return '0x%08xu' % addr

    def want(self, addr):
        if self.text[0] <= addr < self.text[1] and addr not in self.lifted and addr not in self.queued:
            self.queue.append(addr)
            self.queued.add(addr)

    # ---- operands -----------------------------------------------------------------------------------------------
    def parse(self, op):
        m = re.match(r'^(?:(byte|word|dword|qword|xmmword|tbyte) ptr )?fs:\[(.*)\]$', op.strip())
        if m:                                   # the thread's TEB block in guest memory
            return ('mem', ls.SIZES[m.group(1) or 'dword'], '(uint32_t)(s.fs_base + %s)' % self.mem_addr(m.group(2)))
        return super().parse(op)

    def read(self, o, size=None):
        k, sz, e = o
        size = size or sz
        if k == 'imm':
            if size == 32 and self.is_code(e & 0xffffffff):
                self.code_ptr(e & 0xffffffff)   # a code address handed to data: lift it as an entry
            return '%#xu' % (e & {8: 0xff, 16: 0xffff}.get(size, 0xffffffff))
        if k == 'mem':                          # .text is mapped at its original address: byte tables are memory
            if e == '(uint32_t)(0u + 0x1270a64u)':
                return '0u'                     # the CRT's "use SSE2 math" flag: always take the x87 paths
            return {8: 'M8(%s)', 16: 'M16(%s)', 32: 'M32(%s)'}[size] % e
        return super().read(o, size)

    # ---- the call boundary ----------------------------------------------------------------------------------------
    def extra(self, mnem, ops):
        """Instructions the Phase 1 lifter never needed (they live in CRT code it did not lift). None: not handled."""
        args = [a for a in re.split(r',(?![^\[]*\])', ops) if a.strip()] if ops else []
        if mnem == 'rep ret':                              # AMD's two-byte return
            return 'RETURN(0);'
        if mnem == 'std':
            return 's.df = 1;'
        if mnem == 'cld':
            return 's.df = 0;'
        if mnem in ('clc', 'stc', 'cmc'):
            return {'clc': 's.cf = 0;', 'stc': 's.cf = 1;', 'cmc': 's.cf = !s.cf;'}[mnem]
        m = re.match(r'^(rep )?(movs|stos)([bwd])$', mnem)
        if m and 'xmm' not in ops:                         # string moves and stores, honoring the direction flag
            w = {'b': 1, 'w': 2, 'd': 4}[m.group(3)]
            mm = {1: 'M8', 2: 'M16', 4: 'M32'}[w]
            step = '(s.df ? -%du : %du)' % (w, w)
            if m.group(2) == 'movs':
                one = '%s(s.edi) = %s(s.esi); s.edi += %s; s.esi += %s;' % (mm, mm, step, step)
            else:
                one = '%s(s.edi) = (%s)s.eax; s.edi += %s;' % (mm, {1: 'uint8_t', 2: 'uint16_t', 4: 'uint32_t'}[w], step)
            return ('while (s.ecx) { %s s.ecx--; }' % one) if m.group(1) else one
        m = re.match(r'^(repne|repe) (scas|cmps)b$', mnem)
        if m:
            cond = 's.zf' if m.group(1) == 'repne' else '!s.zf'
            if m.group(2) == 'scas':
                one = 'uint32_t a_ = s.eax & 0xffu, b_ = M8(s.edi), r_ = a_ - b_; SUBF(a_, b_, r_, 8); s.edi += (s.df ? -1u : 1u);'
            else:
                one = ('uint32_t a_ = M8(s.esi), b_ = M8(s.edi), r_ = a_ - b_; SUBF(a_, b_, r_, 8); '
                       's.esi += (s.df ? -1u : 1u); s.edi += (s.df ? -1u : 1u);')
            return 'while (s.ecx) { %s s.ecx--; if (%s) break; }' % (one, cond)
        if mnem == 'xlatb':
            return 's.eax = (s.eax & ~0xffu) | M8(s.ebx + (s.eax & 0xffu));'
        if mnem == 'pushal':
            return '{ uint32_t e_ = s.esp; PUSH(s.eax); PUSH(s.ecx); PUSH(s.edx); PUSH(s.ebx); PUSH(e_); PUSH(s.ebp); PUSH(s.esi); PUSH(s.edi); }'
        if mnem in ('rcr', 'rcl') and 1 <= len(args) <= 2:
            P = [self.parse(a) for a in args]
            bits = P[0][1]
            cnt = self.read(P[1]) if len(P) > 1 else '1u'
            if mnem == 'rcr':
                step = 'nc_ = v_ & 1u; v_ = (v_ >> 1) | ((uint32_t)s.cf << %d); s.cf = (int)nc_;' % (bits - 1)
            else:
                step = 'nc_ = (v_ >> %d) & 1u; v_ = ((v_ << 1) | (uint32_t)s.cf) & %#xu; s.cf = (int)nc_;' % (
                    bits - 1, {8: 0xff, 16: 0xffff, 32: 0xffffffff}[bits])
            return '{ uint32_t v_ = %s, nc_, c_ = (%s) & 31u; while (c_--) { %s } %s }' % (
                self.read(P[0]), cnt, step, self.write(P[0], 'v_'))
        if mnem in ('movlpd', 'movlps', 'movhpd', 'movhps') and len(args) == 2:
            d, s_ = self.parse(args[0]), self.parse(args[1])
            off = 0 if mnem.startswith('movl') else 8
            if d[0] == 'xmm':
                return 'memcpy(X(%s) + %d, (const uint8_t*)(uintptr_t)%s, 8);' % (d[2], off, s_[2])
            return 'memcpy((uint8_t*)(uintptr_t)%s, X(%s) + %d, 8);' % (d[2], s_[2], off)
        if mnem in ('andpd', 'andnpd', 'orpd', 'xorpd') and len(args) == 2:
            d, s_ = self.parse(args[0]), self.parse(args[1])
            src = 'X(%s)' % s_[2] if s_[0] == 'xmm' else '(const uint8_t*)(uintptr_t)%s' % s_[2]
            return 'SSEB(X(%s), %s, %d);' % (d[2], src, ('andpd', 'andnpd', 'orpd', 'xorpd').index(mnem))
        if mnem == 'unpcklpd' and len(args) == 2:
            d, s_ = self.parse(args[0]), self.parse(args[1])
            src = 'X(%s)' % s_[2] if s_[0] == 'xmm' else '(const uint8_t*)(uintptr_t)%s' % s_[2]
            return '{ uint8_t t_[8]; memcpy(t_, %s, 8); memcpy(X(%s) + 8, t_, 8); }' % (src, d[2])
        if mnem == 'cvtdq2ps' and len(args) == 2:
            d, s_ = self.parse(args[0]), self.parse(args[1])
            src = 'X(%s)' % s_[2] if s_[0] == 'xmm' else '(const uint8_t*)(uintptr_t)%s' % s_[2]
            return ('{ int32_t i_[4]; float f_[4]; memcpy(i_, %s, 16); for (int k_ = 0; k_ < 4; ++k_) f_[k_] = (float)i_[k_]; '
                    'memcpy(X(%s), f_, 16); }') % (src, d[2])
        if mnem == 'movd' and len(args) == 2 and re.match(r'^xmm\d$', args[0].strip()):
            s_ = self.parse(args[1])
            return '{ uint32_t v_ = %s; memset(X(%s), 0, 16); memcpy(X(%s), &v_, 4); }' % (
                self.read(s_), args[0].strip()[3:], args[0].strip()[3:])
        if mnem in ('movntps', 'movntdq') and len(args) == 2:
            d, s_ = self.parse(args[0]), self.parse(args[1])
            return 'memcpy((uint8_t*)(uintptr_t)%s, X(%s), 16);' % (d[2], s_[2])
        if mnem == 'popal':
            return ('s.edi = POP(); s.esi = POP(); s.ebp = POP(); (void)POP(); s.ebx = POP(); s.edx = POP(); '
                    's.ecx = POP(); s.eax = POP();')
        if mnem in ('fldenv', 'frstor') and len(args) == 1:
            return ''                              # the environment the CRT saved a moment before: nothing to restore
        if mnem in ('psrlq', 'psllq') and len(args) == 2 and re.match(r'^xmm\d$', args[1].strip()):
            d, c = args[0].strip()[3:], args[1].strip()[3:]
            op = '>>' if mnem == 'psrlq' else '<<'
            return ('{ uint64_t q_[2], n_; memcpy(q_, X(%s), 16); memcpy(&n_, X(%s), 8); '
                    'q_[0] = n_ < 64 ? q_[0] %s n_ : 0; q_[1] = n_ < 64 ? q_[1] %s n_ : 0; memcpy(X(%s), q_, 16); }') % (
                d, c, op, op, d)
        if mnem == 'pshufd' and len(args) == 3:
            d, s_ = self.parse(args[0]), self.parse(args[1])
            imm = ls.num(args[2].strip()) & 0xff
            src = 'X(%s)' % s_[2] if s_[0] == 'xmm' else '(const uint8_t*)(uintptr_t)%s' % s_[2]
            sel = ', '.join('a_[%d]' % ((imm >> (2 * i)) & 3) for i in range(4))
            return '{ uint32_t a_[4]; memcpy(a_, %s, 16); uint32_t r_[4] = {%s}; memcpy(X(%s), r_, 16); }' % (src, sel, d[2])
        if mnem in ('fnstenv', 'fstenv', 'fnsave', 'fsave') and len(args) == 1:
            d = self.parse(args[0])
            size = 28 if 'env' in mnem else 108
            st = ('{ uint8_t* e_ = (uint8_t*)(uintptr_t)%s; memset(e_, 0, %d); uint16_t cw_ = 0x027f, sw_ = (uint16_t)FSW(), '
                  'tw_ = s.depth ? 0 : 0xffff; memcpy(e_, &cw_, 2); memcpy(e_ + 4, &sw_, 2); memcpy(e_ + 8, &tw_, 2); }') % (d[2], size)
            return st + (' s.depth = 0;' if 'save' in mnem else '')
        return None

    def stmt(self, addr, n, raw, mnem, ops, labels, entry, end):
        x = self.extra(mnem, ops)
        if x is not None:
            return x
        if mnem == 'call':
            o, nxt = ops.strip(), addr + n
            if o.startswith('dword ptr') or o.startswith('fs:') or o in ls.REG32:
                tgt = ('s.' + o) if o in ls.REG32 else self.read(self.parse(o))
                return 'RCALLI(%s, 0x%08xu);' % (tgt, nxt)
            target = ls.num(o.split()[0])
            m = CRT_MATH.get(target)
            if m:                               # cdecl double math: arguments at [esp], result in st0
                fn, nargs = m
                xs = ', '.join('MD(s.esp + %du)' % (8 * i) for i in range(nargs))
                return 'FPUSH(%s(%s));' % (fn, xs)
            if self.resolve_code(target):       # x87/edx:eax helpers lowered inline by the base class
                return super().stmt(addr, n, raw, mnem, ops, labels, entry, end)
            if not self.is_code(target):
                return 'fnv_trap(0x%08xu, "recomp: call outside .text"); RETURN(0);' % addr
            self.code_ptr(target)
            return 'RCALL(f_%08x, 0x%08xu);' % (target, nxt)
        st = super().stmt(addr, n, raw, mnem, ops, labels, entry, end)
        # tail jumps to a known address become direct calls (TAILCALL keeps dispatching for computed targets)
        return re.sub(r'TAILCALL\(0x([0-9a-f]{8})u\)', lambda m: 'RJMP(f_%s)' % m.group(1), st)

    def lift_uncached(self, entry, end):
        """The base walk, stopping after any unconditional jump as well as ret (the code after one is reachable only
        through a label, which is queued): without it a walk runs on into the next function."""
        self.disassemble(entry, end)
        body, labels, todo, seen = {}, set(), [entry], set()
        queued, queued_n = set(), 0
        while todo:
            a = todo.pop()
            if a not in self.ins and entry <= a < end:
                self.resync(a, end)
            while a in self.ins and a not in seen and a < end:
                seen.add(a)
                n, raw, mnem, ops = self.ins[a]
                try:
                    st = self.stmt(a, n, raw, mnem, ops, labels, entry, end)
                except ls.Unsupported as e:
                    self.unsupported[a] = str(e)
                    st = 'fnv_trap(0x%08xu, "recomp: unsupported instruction"); return;' % a
                body[a] = st
                if len(labels) != queued_n:
                    for t in labels - queued:
                        queued.add(t)
                        if t not in seen:
                            todo.append(t)
                    queued_n = len(labels)
                if mnem in ('ret', 'jmp') or 'RETURN(' in st or 'return;' in st:
                    break
                a += n
                if a not in self.ins and a < end:
                    self.resync(a, end)
        lines = ['void f_%08x(FnvCpu& s)' % entry, '{', '    FNV_ENTER(0x%08xu);' % entry]
        for a in sorted(body):
            if a in labels:
                lines.append('L_%08x:' % a)
            lines.append('    %s  // %08x %s %s' % (body[a], a, self.ins[a][2], self.ins[a][3][:60]))
        lines.append('    fnv_trap(0x%08xu, "recomp: fell off the end of the lifted range");' % entry)
        lines.append('}')
        self.lifted[entry] = '\n'.join(lines)

    # ---- cache and output -----------------------------------------------------------------------------------------
    def cache_path(self):
        import hashlib
        h = hashlib.sha1(Path(ls.__file__).read_bytes() + Path(__file__).read_bytes())
        with open(self.exe, 'rb') as f:
            h.update(hashlib.sha256(f.read()).digest())
        return ROOT / 'out/cache' / ('recomp_%s.pickle' % h.hexdigest()[:16])

    def run(self, entries, use_cache=True):
        import pickle
        path = self.cache_path() if use_cache else None
        if path and path.exists():
            self.cache = pickle.loads(path.read_bytes())
        self.stub_starts = set()                # no stub boundaries: every function is lifted
        self.queue = sorted(entries, reverse=True)
        self.queued = set(entries)
        done = 0
        t0 = time.time()
        while self.queue:
            e = self.queue.pop()
            if e not in self.lifted:
                self.lift(e)
                done += 1
                if done % 5000 == 0:
                    print('  %d lifted, %d queued, %.0f s' % (done, len(self.queue), time.time() - t0), flush=True)
        if path and self.cache_new != self.cache:
            path.parent.mkdir(parents=True, exist_ok=True)
            for old in path.parent.glob('recomp_*.pickle'):
                old.unlink()
            path.write_bytes(pickle.dumps(self.cache_new, protocol=pickle.HIGHEST_PROTOCOL))

    def shards(self, n):
        groups = [[] for _ in range(n)]
        for a in sorted(self.lifted):
            groups[zlib.crc32(struct.pack('<I', a)) % n].append(a)
        ref = re.compile(r'\bf_([0-9a-f]{8})\b')
        out = []
        for k, funcs in enumerate(groups):
            body = '\n'.join(self.lifted[a] for a in funcs)
            used = sorted(set(ref.findall(body)) - {'%08x' % a for a in funcs})
            head = ['// Generated by tools/recomp.py from the player\'s FalloutNV.exe: do not commit (rule 1).',
                    '#include "fnv_recomp.h"', '']
            head += ['void f_%s(FnvCpu& s);' % a for a in used]
            head += ['void f_%08x(FnvCpu& s);' % a for a in funcs]
            tail = ['', 'static const uint32_t kAddrs[] = {%s};' % ', '.join('0x%08xu' % a for a in funcs),
                    'static const FnvFn kFns[] = {%s};' % ', '.join('f_%08x' % a for a in funcs),
                    'static struct Reg { Reg() { fnv_register_functions(kAddrs, kFns, %d); } } reg_;' % len(funcs)]
            out.append('\n'.join(head) + '\n' + body + '\n' + '\n'.join(tail) + '\n')
        return out


def entries_of(img, units, text):
    starts = sorted({int(f['address'], 16) for u in units for f in u['functions']})
    sizes = {}
    for u in units:
        for f in u['functions']:
            a = int(f['address'], 16)
            sizes[a] = max(sizes.get(a, 0), f['size'])
    ent = set(starts)
    pe = struct.unpack_from('<I', img.data, 0x3c)[0]
    pe_entry = img.base + struct.unpack_from('<I', img.data, pe + 0x28)[0]
    ent.add(pe_entry)
    from_data = 0
    for site in img.relocs():
        if text[0] <= site < text[1]:
            continue                            # immediates are found while lifting; jump tables are labels
        v = img.u32(site)
        if v is not None and text[0] <= v < text[1]:
            ent.add(v)
            from_data += 1
    return starts, sizes, ent, pe_entry, from_data


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', required=True)
    ap.add_argument('--shards', type=int, default=64)
    ap.add_argument('--only', default='', help='comma-separated entries (a quick partial build for testing)')
    ap.add_argument('--no-cache', action='store_true')
    a = ap.parse_args()
    t0 = time.time()
    img = Image(a.exe)
    tsec = next(s for s in img.sections if s['name'] == '.text')
    text = (tsec['start'], tsec['start'] + tsec['vsize'])
    units = json.loads((Path(DECOMP_DIR) / 'build/units.json').read_text())['units']
    names = {}
    for u in units:
        for f in u['functions']:
            names.setdefault(int(f['address'], 16), f['symbol'])
        for k, v in u.get('symbols', {}).items():
            names.setdefault(int(v, 0), k)
    starts, sizes, ent, pe_entry, from_data = entries_of(img, units, text)
    if a.only:
        ent = {int(x, 16) for x in a.only.split(',')}
    print('entries: %d (%d recipe starts, %d from data fixups), entry point %#x' %
          (len(ent), len(starts), from_data, pe_entry), flush=True)
    rc = Recompiler(a.exe, text, starts, sizes, names)
    rc.run(sorted(ent), use_cache=not a.no_cache)
    OUT.mkdir(parents=True, exist_ok=True)
    for old in OUT.glob('recomp_*.cpp'):
        old.unlink()
    for k, src in enumerate(rc.shards(a.shards)):
        (OUT / ('recomp_%02d.cpp' % k)).write_text(src)
    from collections import Counter
    unsup = Counter(v.split(' ')[0] for v in rc.unsupported.values())
    report = dict(entries=len(ent), lifted=len(rc.lifted), traps=len(rc.traps), unsupported=len(rc.unsupported),
                  unsupported_top=unsup.most_common(20), cache_hits=rc.cache_hits, entry_point='%#x' % pe_entry,
                  seconds=round(time.time() - t0))
    (OUT / 'report.json').write_text(json.dumps(report, indent=1))
    print(json.dumps(report, indent=1))


if __name__ == '__main__':
    main()
