#!/usr/bin/env python3
"""Phase 1: patch every unit object and link the relinked game DLL.

Per object (out/obj/NNNN_*.obj -> out/link/obj/NNNN_*.obj):
  * recipe functions the unit defines (external or static) are renamed fnv_f_<addr> and made external, so callers
    and callees meet by address (this also removes the name collisions between units);
  * undefined names are bound through the unit's `symbols` map (then the global recipe map), after stripping the
    overlay's __fnvdupK marker:
        function start        -> fnv_f_<addr>
        data / IAT / other    -> fnv_d_<addr>   (absolute symbol at the original VA, in out/link/abs.obj)
        other code in .text   -> its CRT name when the CRT defines it, else fnv_c_<addr> (trap)
  * clang's own vftable/RTTI COMDAT sections (??_7, ??_R*) are marked IMAGE_SCN_LNK_REMOVE and their symbols made
    undefined, so references bind to the data image's vftables and type descriptors instead.
Generated: out/link/abs.obj (absolute data symbols), out/link/traps.cpp (logging stubs for everything unbound).
The link result and its unresolved/duplicate symbols are summarised in out/link/link_report.json.

usage: python tools/link_build.py --exe <FalloutNV.exe> [--jobs N]
"""
import argparse
import bisect
import hashlib
import json
import os
import re
import struct
import subprocess
import sys
from collections import Counter, defaultdict
from concurrent.futures import ProcessPoolExecutor, ThreadPoolExecutor
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, r'E:\Games\fnv_decomp-main\tools')
import relink_plan as rp  # noqa: E402
from relink import Image  # noqa: E402

ROOT = rp.ROOT
DECOMP = rp.DECOMP
LINK = ROOT / 'out/link'
LLVM = Path(r'C:\Program Files\LLVM\bin')
DUP = re.compile(r'__fnvdup\d+')
# Port runtime and compat sources linked into the game DLL (source, object name).
COMPAT_SOURCES = [
    ('port/runtime/fnv_trap.cpp', 'fnv_trap.obj'),
    ('port/compat/fnv_msvc_xmm.cpp', 'fnv_msvc_xmm.obj'),
    ('port/compat/fnv_vc2008_shims.cpp', 'fnv_vc2008_shims.obj'),
    ('port/compat/fnv_rtti.cpp', 'fnv_rtti.obj'),
    ('port/runtime/fnv_lift_runtime.cpp', 'fnv_lift_runtime.obj'),
    ('port/compat/fnv_crt_orig.cpp', 'fnv_crt_orig.obj'),
]
XC_TABLE = (0x00FDF4A4, 0x010113C0)     # __xc_a..__xc_z of FalloutNV.exe 1.4.0.525 (see port/loader/loader.cpp)
LIFT_SHARDS = 48                         # out/link/lifted_NN.cpp, compiled in parallel (one function's change rebuilds one)
LIFTED_OBJS = [('out/link/lifted_%02d.cpp' % k, 'lifted_%02d.obj' % k) for k in range(LIFT_SHARDS)]
# Units left out of the link (their functions become logging traps) until fixed, with the reason.
EXCLUDE = {
    'src/fallout_shared/tesscript.cpp':
        'declares the original __tls_index slot (DAT_0126fd98) as a __declspec(thread) variable; no correct binding '
        'exists in a clang-built DLL (SECREL to a non-TLS address)',
}
# a mangled function or variable whose own name is FUN_/DAT_<8 hex digits>, in any scope, or the plain C name
ADDR_NAME = re.compile(r'^(?:\?|_)(FUN|DAT)_([0-9a-fA-F]{8})(?:@|$)')
LNK_REMOVE = 0x800
SYM_ABSOLUTE = -1
EXTERNAL, STATIC, WEAK_EXTERNAL = 2, 3, 105


class CoffPatch:
    """Minimal i386 COFF symbol-table rewriter: rename, externalise, undefine symbols; flag sections."""

    def __init__(self, data):
        self.data = bytearray(data)
        _, self.nsec, _, self.symptr, self.nsym, opt, _ = struct.unpack_from('<HHIIIHH', data)
        assert opt == 0
        self.strptr = self.symptr + self.nsym * 18
        strlen, = struct.unpack_from('<I', data, self.strptr)
        assert self.strptr + strlen == len(data), 'string table is not at the end of the object'
        self.strings = bytes(data[self.strptr:self.strptr + strlen])
        self.extra = bytearray()

    def symbols(self):
        i = 0
        while i < self.nsym:
            o = self.symptr + i * 18
            raw, value, sec, typ, sc, aux = struct.unpack_from('<8sIhHBB', self.data, o)
            if raw[:4] == b'\0\0\0\0':
                off, = struct.unpack_from('<I', raw, 4)
                name = self.strings[off:self.strings.index(b'\0', off)].decode('latin1')
            else:
                name = raw.rstrip(b'\0').decode('latin1')
            yield i, dict(name=name, value=value, section=sec, type=typ, storage=sc, aux=aux)
            i += 1 + aux

    def section_flags(self, index):
        return struct.unpack_from('<I', self.data, 20 + (index - 1) * 40 + 36)[0]

    def section_name(self, index):
        return bytes(self.data[20 + (index - 1) * 40:20 + (index - 1) * 40 + 8]).rstrip(b'\0').decode('latin1')

    def flag_section(self, index, flag):
        o = 20 + (index - 1) * 40 + 36
        struct.pack_into('<I', self.data, o, self.section_flags(index) | flag)

    def rename(self, i, name):
        off = len(self.strings) + len(self.extra)
        self.extra += name.encode('latin1') + b'\0'
        struct.pack_into('<II', self.data, self.symptr + i * 18, 0, off)

    def set(self, i, section=None, value=None, storage=None):
        o = self.symptr + i * 18
        if value is not None:
            struct.pack_into('<I', self.data, o + 8, value)
        if section is not None:
            struct.pack_into('<h', self.data, o + 12, section)
        if storage is not None:
            struct.pack_into('<B', self.data, o + 16, storage)

    def drop_includes(self, names):
        """Blank (same length) /INCLUDE:name directives in .drectve for symbols this patch renamed: the linker would
        otherwise demand the old name. Returns how many were dropped."""
        n = 0
        for sec in range(1, self.nsec + 1):
            if self.section_name(sec) != '.drectve':
                continue
            h = 20 + (sec - 1) * 40
            size, raw = struct.unpack_from('<II', self.data, h + 16)
            text = bytes(self.data[raw:raw + size]).decode('latin1')
            for m in re.finditer(r'/INCLUDE:("?)([^" ]+)\1', text, re.I):
                if m.group(2) in names:
                    self.data[raw + m.start():raw + m.end()] = b' ' * (m.end() - m.start())
                    n += 1
        return n

    def add_symbol(self, name, storage=EXTERNAL):
        """Append an undefined symbol (after every existing one, so no index moves); returns its index."""
        off = len(self.strings) + len(self.extra)
        self.extra += name.encode('latin1') + b'\0'
        self.added = getattr(self, 'added', bytearray())
        index = self.nsym + len(self.added) // 18
        self.added += struct.pack('<IIIhHBB', 0, off, 0, 0, 0, storage, 0)
        return index

    def retarget(self, old, new):
        """Point every relocation that names symbol `old` at symbol `new` (applied in one pass by bytes())."""
        self.pending = getattr(self, 'pending', {})
        self.pending[old] = new

    def apply_retargets(self):
        pending = getattr(self, 'pending', None)
        if not pending:
            return
        for sec in range(1, self.nsec + 1):
            h = 20 + (sec - 1) * 40
            relptr, = struct.unpack_from('<I', self.data, h + 24)
            count, = struct.unpack_from('<H', self.data, h + 32)
            if self.section_flags(sec) & 0x01000000:            # IMAGE_SCN_LNK_NRELOC_OVFL
                count, = struct.unpack_from('<I', self.data, relptr)
            for k, (sym,) in enumerate(struct.iter_unpack('<4xI2x', self.data[relptr:relptr + count * 10])):
                if sym in pending:
                    struct.pack_into('<I', self.data, relptr + k * 10 + 4, pending[sym])
        self.pending = {}

    def bytes(self):
        self.apply_retargets()
        strings = self.strings + bytes(self.extra)
        added = bytes(getattr(self, 'added', b''))
        head = bytearray(self.data[:self.strptr])
        struct.pack_into('<I', head, 12, self.nsym + len(added) // 18)
        return bytes(head) + added + struct.pack('<I', len(strings)) + strings[4:]


CTX = {}


def init(ctx):
    CTX.update(ctx)


def init_from_file(path):
    import pickle
    CTX.update(pickle.loads(Path(path).read_bytes()))


def ctx_pool(ctx, jobs):
    """A worker pool whose workers start with `ctx`: written once to out/cache and read by each worker (sending
    the ~40 MB context through every worker's pipe cost ~9 s per pool)."""
    import pickle
    path = ROOT / 'out/cache/ctx_pool.pickle'
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(pickle.dumps(ctx, protocol=pickle.HIGHEST_PROTOCOL))
    return ProcessPoolExecutor(jobs, initializer=init_from_file, initargs=(str(path),))


CRT_REGION = (0x00EC3000, 0x00EDE000)     # the statically linked VC2008 CRT in FalloutNV.exe


def crt_alias(name, addr, libsyms):
    """Modern CRT symbol for a decomp name of a VC2008 CRT function, by name only, inside the CRT region:
    `__malloc` (extern "C" _malloc) -> `_malloc`, `?_strlen@@YAIPBD@Z` -> `_strlen`. FUN_ names never match."""
    if not CRT_REGION[0] <= addr < CRT_REGION[1] or 'FUN_' in name:
        return None
    m = re.match(r'^\?(_*\w+?)@@', name)
    base = m.group(1) if m else re.sub(r'@\d+$', '', name)
    for cand in (base, base[1:] if base.startswith('_') else None, '_' + base):
        if cand and cand in libsyms:
            return cand
    return None


def own_functions(index, defined=()):
    """{mangled name: address} of the recipe functions a unit defines: the recipe's own names plus the @fnv source
    map (tools/fnv_map.py), which binds definitions whose mangling changed (overlay renames, SSE unification, clang
    vs VC2008 mangling differences)."""
    own = {f['symbol']: int(f['address'], 16) for f in CTX['units'][index]['functions']}
    mapped = CTX['fnv_map'].get(str(index), {})
    # a unit can carry two definitions marked with one address (a readable copy and the matched shell), and a few
    # @fnv comments sit above the wrong definition: the recipe's name wins whenever the object defines it;
    # otherwise the first mapped name per address
    taken = {own[n] for n in own if n in mapped or n in defined}
    for name, addr in sorted(mapped.items()):
        a = int(addr, 16)
        if name not in own and a not in taken:
            own[name] = a
            taken.add(a)
    return own


def bound_vftable_sections(entries, index):
    """clang's own vftable sections whose ??_7 name binds to an address: those are replaced by the data image's
    vftables. Vftables with no address, and all RTTI, are kept (MSVC's __RTDynamicCast and EH compare type
    descriptors by pointer or by decorated name, so clang's descriptors agree with the original ones)."""
    syms = CTX['units'][index].get('symbols', {})
    return {s['section'] for _, s in entries
            if s['section'] > 0 and s['name'].startswith('??_7')
            and (s['name'] in syms or s['name'] in CTX['global_names'])}


def patch_object(job):
    index, src, dst = job
    c = CTX
    unit = c['units'][index]
    syms = unit.get('symbols', {})
    p = CoffPatch(Path(src).read_bytes())
    entries = list(p.symbols())
    own = own_functions(index, {s['name'] for _, s in entries if s['section'] > 0})
    vsec = bound_vftable_sections(entries, index)
    for sec in vsec:
        p.flag_section(sec, LNK_REMOVE)
    stats, traps = Counter(), {}
    renamed = set()        # names whose definition this patch renamed (their /INCLUDE directives go too)
    # clang's per-unit initializer lists: the loader runs the original __xc table instead, in its original order
    for sec in range(1, p.nsec + 1):
        if p.section_name(sec) == '.CRT$XCU':
            p.flag_section(sec, LNK_REMOVE)
            stats['crt-xcu-stripped'] += 1

    def bound_data(name):
        addr = int(syms[name], 0) if name in syms else c['global_names'].get(name)
        if addr is None:
            addr = c['literal_bind'].get(name)
        return addr is not None and addr not in c['starts'] and not (c['text'][0] <= addr < c['text'][1])

    def bind(name, pf=False):
        base = DUP.sub('', name)
        if base == '__tls_index':
            return base, 'crt'          # the loader writes the DLL's own TLS index into the CRT's variable
        addr = int(syms[base], 0) if base in syms else c['global_names'].get(base)
        if addr is None:
            addr = c['inferred'].get(base)          # bound by call-site correspondence (infer_job)
        if addr is None:
            addr = c['literal_bind'].get(base)      # string literal at its original location (infer_job)
        if addr is None and base.startswith('__imp_?'):
            # an import the overlay turned into a namespaced C++ declaration (dropc): bind to Name's IAT slot
            ident = re.match(r'__imp_\?(\w+)@', base)
            if ident:
                addr = c['iat_by_name'].get(ident.group(1))
        if addr is None:
            # FUN_<addr> / DAT_<addr> name the address they live at, whatever namespace the overlay left them in
            m = ADDR_NAME.match(base)
            if m:
                a = int(m.group(2), 16)
                if (m.group(1) == 'FUN' and (a in c['starts'] or a in c['crt_map'])) or \
                        (m.group(1) == 'DAT' and not (c['text'][0] <= a < c['text'][1])):
                    addr = a
        if addr is not None and addr in c['crt_map']:
            return c['crt_map'][addr], 'crt-mapped'      # port/compat/crt_map.json
        if addr == 0x00A9A730:
            # OpenAI Phase 1: original 0xa9a730 is a tail JMP to 0xa5e9d0; the decomp emits CALL+INT3 and drops args.
            return 'fnv_f_00a5e9d0', 'tail-jump-alias'
        if addr is not None and c['tls'][0] <= addr < c['tls'][1]:
            return 'fnv_t_%08x' % addr, 'tls'
        if addr is not None:
            if addr in c['starts']:
                if c['starts'][addr] not in c['compiled']:
                    traps['fnv_f_%08x' % addr] = (addr, base)
                    return 'fnv_f_%08x' % addr, 'func-missing'
                fix = c['popfix'].get(addr, {}).get(base) if pf else None
                if fix:
                    return 'fnv_pf_%08x_%d' % (addr, fix), 'func-popfix'
                return 'fnv_f_%08x' % addr, 'func'
            if c['text'][0] <= addr < c['text'][1]:
                if base in c['libsyms']:
                    return base, 'crt'
                alias = crt_alias(base, addr, c['libsyms'])
                for other in ([] if alias else sorted(c['names_at'].get(addr, ()))):
                    alias = crt_alias(other, addr, c['libsyms'])      # another unit's name for the address
                    if alias:
                        break
                if alias:
                    return alias, 'crt-alias'
                traps['fnv_c_%08x' % addr] = (addr, base)
                return 'fnv_c_%08x' % addr, 'code-other'
            return 'fnv_d_%08x' % addr, 'data'
        if base in c['libsyms'] or base in c['compat'] or base in c['by_name']:
            return base, 'by-name'
        traps[base] = (0, base)
        return base, 'unresolved'

    for i, s in entries:
        name, sec = s['name'], s['section']
        if sec > 0 and name in own and s['storage'] in (EXTERNAL, STATIC) and sec not in vsec \
                and own[name] == 0x00B00A80 and c['addr_owner'].get(own[name]) == index:
            # OpenAI Phase 1: retain the source implementation behind the null-safe ABI wrapper.
            p.rename(i, 'fnv_f_00b00a80__fnvorig')
            p.set(i, storage=EXTERNAL)
            stats['tasklet-null-safe-original'] += 1
        elif sec > 0 and name in own and s['storage'] in (EXTERNAL, STATIC) and sec not in vsec \
                and own[name] in c['lift_replace']:
            # OpenAI model: route local relocations to the canonical lifted implementation too.
            p.retarget(i, p.add_symbol('fnv_f_%08x' % own[name]))
            renamed.add(name)
            p.rename(i, 'fnv_f_%08x__fnvreplaced%d' % (own[name], index))
            p.set(i, storage=STATIC)
            stats['replaced-by-lifted'] += 1

        elif sec > 0 and name in own and s['storage'] in (EXTERNAL, STATIC) and sec not in vsec:
            if c['addr_owner'].get(own[name]) == index:
                p.rename(i, 'fnv_f_%08x' % own[name])
                p.set(i, storage=EXTERNAL)
                stats['defined-recipe'] += 1
            else:
                p.rename(i, 'fnv_f_%08x__fnvlocal%d' % (own[name], index))
                p.set(i, storage=STATIC)
                stats['defined-recipe-second-copy'] += 1
        elif sec > 0 and s['storage'] in (EXTERNAL, STATIC) and s['type'] != 0x20 and sec not in vsec \
                and not name.startswith('.') and bound_data(name):
            # a global the unit defines but the original keeps at a fixed address (e.g. the objects the generated
            # initializers construct): bind every use to the data image instead of the DLL's own copy
            # The definition itself stays (COMDAT and associative sections remain consistent) as an unreferenced
            # local; every relocation is retargeted to a new undefined symbol bound to the original address.
            new, kind = bind(name)
            p.retarget(i, p.add_symbol(new))
            renamed.add(name)
            p.rename(i, '%s__fnvdead%d' % (name, index))
            p.set(i, storage=STATIC)
            stats['data-definition-bound-' + kind] += 1
        elif sec > 0 and s['type'] == 0x20 and s['storage'] in (EXTERNAL, STATIC) and sec not in vsec \
                and name not in own and name in syms and int(syms[name], 0) in c['starts']:
            # a stub body the unit defines for a function its recipe binds elsewhere (VC2008's call went to that
            # address): keep the stub as an unreferenced local and send every call to the real function
            new, kind = bind(name)
            p.retarget(i, p.add_symbol(new))
            renamed.add(name)
            p.rename(i, '%s__fnvstub%d' % (name, index))
            p.set(i, storage=STATIC)
            stats['stub-definition-bound'] += 1
        elif s['storage'] == WEAK_EXTERNAL and name.startswith('??_E'):
            # clang's vector-deleting-dtor alias: only its (discarded) vftables use it
            p.rename(i, '%s__fnvlocal%d' % (name, index))
            stats['weak-vdtor-local'] += 1
        elif sec > 0 and s['storage'] == EXTERNAL and name in c['dups'] and c['dups'][name] != index:
            # another object owns this name (or it binds by address): keep this copy object-local
            p.rename(i, '%s__fnvlocal%d' % (name, index))
            p.set(i, storage=STATIC)
            renamed.add(name)
            stats['dup-made-local'] += 1
        elif sec in vsec and s['storage'] == EXTERNAL:
            new, kind = bind(name)
            p.rename(i, new)
            p.set(i, section=0, value=0)
            stats['vftable-symbol-' + kind] += 1
        elif sec == 0 and s['storage'] == EXTERNAL and s['value'] == 0:
            if name in own:
                new, kind = 'fnv_f_%08x' % own[name], 'func'
            else:
                new, kind = bind(name, pf=True)
            if new != name:
                p.rename(i, new)
            stats[kind] += 1
    stats['drectve-include-dropped'] += p.drop_includes(renamed)
    Path(dst).write_bytes(p.bytes())
    return index, stats, traps


def lookup(name, unit):
    """Address a name binds to, without side effects (None when unbound or bound by name only)."""
    c = CTX
    base = DUP.sub('', name)
    syms = unit.get('symbols', {})
    addr = int(syms[base], 0) if base in syms else c['global_names'].get(base)
    if addr is None:
        addr = c['inferred'].get(base)
    if addr is None:
        m = ADDR_NAME.match(base)
        if m and m.group(1) == 'FUN' and int(m.group(2), 16) in c['starts']:
            addr = int(m.group(2), 16)
    return addr


def infer_job(job):
    """Call-site constraints for one object. For each relinked function F (original address A): the names its call
    relocations (REL32) use that do not bind, and A's original call targets (recipe functions or CRT code) that no
    bound call of F explains. Solved across all objects by solve_call_sites."""
    from compare import Coff
    index, src, _ = job
    c = CTX
    unit = c['units'][index]
    coff = Coff(src)
    defined = {s['name'] for s in coff.symbols.values() if s['section'] > 0}
    own = own_functions(index, defined)
    sec_fn = {}
    for s in coff.symbols.values():
        if s['section'] > 0 and s['name'] in own:
            sec_fn.setdefault(s['section'], own[s['name']])
    for s in coff.symbols.values():
        # a unit's own copy of a function another unit owns (inline/COMDAT) is still that original's code
        if s['section'] > 0 and s['section'] not in sec_fn and s['type'] == 0x20 and s['name'] not in own:
            addr = lookup(s['name'], unit)
            if addr in c['starts']:
                sec_fn[s['section']] = addr
    constraints, literal_votes = [], []
    for sec, a in sec_fn.items():
        names, bound = set(), set()
        # F's calls, plus those of local non-recipe helpers it reaches: VC2008 inlined such helpers (e.g. a decomp
        # `inline` allocator wrapper) into the original, so their callees are among F's original call targets
        todo, seen, relocs = [sec], {sec}, []
        while todo:
            s_ = todo.pop()
            for _, si, kind in coff.sections[s_]['relocs']:
                sym = coff.symbols[si]
                if kind == 0x14 and sym['section'] > 0 and sym['name'] not in own and sym['section'] not in seen \
                        and sym['section'] not in sec_fn:
                    seen.add(sym['section'])
                    todo.append(sym['section'])
                relocs.append((si, kind))
        for si, kind in relocs:
            sym = coff.symbols[si]
            if kind == 0x06 and sym['section'] > 0 and sym['name'].startswith('??_C@'):
                # a string literal F uses: candidates are the data addresses the original F references (exe .reloc)
                # whose bytes are that literal, terminator included
                lit = literal_bytes(coff, sym)
                if lit:
                    cand = frozenset(t for t in c['orig_data_refs'].get(a, ()) if image_bytes(t, len(lit)) == lit)
                    literal_votes.append((sym['name'], cand))
                continue
            if kind != 0x14:
                continue
            n = sym['name']
            if sym['section'] > 0:
                if n in own:
                    bound.add(own[n])
                continue
            base = DUP.sub('', n)
            addr = lookup(base, unit)
            if addr is not None:
                bound.add(addr)
            elif not (base in c['libsyms'] or base in c['compat'] or base in c['by_name']):
                names.add(base)
        if not names:
            continue
        cand = {t for t in c['orig_calls'].get(a, set()) - bound - {a}
                if t in c['starts'] or CRT_REGION[0] <= t < CRT_REGION[1]}
        constraints.append((a, frozenset(names), frozenset(cand)))
    return constraints, literal_votes


def literal_bytes(coff, sym):
    """Bytes of a string literal symbol (`??_C@_0...` narrow, `??_C@_1...` wide), terminator included."""
    data = coff.sections[sym['section']]['data']
    off, unit = sym['value'], (2 if sym['name'].startswith('??_C@_1') else 1)
    end = off
    while end + unit <= len(data) and data[end:end + unit] != b'\0' * unit:
        end += unit
    return bytes(data[off:end + unit]) if end + unit <= len(data) else None


def image_bytes(va, n):
    base, data = CTX['image']
    o = va - base
    return bytes(data[o:o + n]) if 0 <= o and o + n <= len(data) else None


def member_kind(name):
    """Special-member kind of a decorated name: ctor, dtor, deleting dtors, new/delete, or a plain function."""
    m = re.match(r'^\?\?(_[A-Z]|[0-9A-Z])', name)
    return m.group(1) if m else 'fn'


def solve_call_sites(constraints, names_at=None):
    """Bind names from call-site constraints. A name's target must be among the unexplained original targets of
    every function that calls it, so its candidates are the intersection over its callers; a single survivor binds
    it. Bound names and their targets drop out of every constraint and the solve repeats. Names whose intersection
    is empty (clang calls something VC2008 inlined) stay unbound rather than guessed."""
    inferred, rounds = {}, []
    while True:
        cand = {}
        for _, names, targets in constraints:
            rest = names - inferred.keys()
            if not rest:
                continue
            free = targets - {inferred[n] for n in names if n in inferred}
            for n in rest:
                cand[n] = cand[n] & free if n in cand else set(free)
        # one target cannot be two names: when a caller's leftover target is the only candidate of more than one
        # of its unbound names (e.g. a constructor and a destructor whose EH call the original makes elsewhere),
        # none of them binds through it
        blocked = set()
        for _, names, targets in constraints:
            rest = [n for n in names - inferred.keys() if n in cand and len(cand[n]) == 1]
            seen_t = defaultdict(list)
            for n in rest:
                seen_t[next(iter(cand[n]))].append(n)
            for ns in seen_t.values():
                if len(ns) > 1:
                    blocked.update(ns)
        new = {n: next(iter(s)) for n, s in cand.items() if len(s) == 1 and n not in blocked}
        if names_at:
            # a name binds only where the recipe's names for that address are of the same kind (a destructor
            # never binds to an address the recipe knows only as a constructor)
            new = {n: t for n, t in new.items()
                   if not names_at.get(t) or member_kind(n) in {member_kind(x) for x in names_at[t]}}
        rounds.append(dict(new=len(new), empty=sum(1 for s in cand.values() if not s),
                           ambiguous=sum(1 for s in cand.values() if len(s) > 1)))
        if not new:
            return inferred, rounds
        inferred.update(new)


def external_defs(job):
    """Externally defined names of an object, minus its own recipe functions and clang's vftable/RTTI sections."""
    index, src, _ = job
    p = CoffPatch(Path(src).read_bytes())
    entries = list(p.symbols())
    own = set(own_functions(index, {s['name'] for _, s in entries if s['section'] > 0}))
    vsec = bound_vftable_sections(entries, index)
    # COMDAT selection per section, from the section symbol's aux record (byte 14); only NODUPLICATES (1) collides
    selection = {}
    for i, s in entries:
        if s['storage'] == STATIC and s['aux'] and s['value'] == 0 and s['section'] > 0 \
                and s['name'] == p.section_name(s['section']) and p.section_flags(s['section']) & 0x1000:
            selection[s['section']] = p.data[p.symptr + (i + 1) * 18 + 14]
    return index, [(s['name'], selection.get(s['section'], 0)) for _, s in entries
                   if s['section'] > 0 and s['storage'] == EXTERNAL and s['name'] not in own
                   and s['section'] not in vsec]


def external_symbols(path):
    """(externally defined, undefined) names of an object; static definitions cannot satisfy other objects."""
    ext, und = set(), set()
    for letter, name in rp.nm_lines(path):
        if letter == 'U':
            und.add(name)
        elif letter in 'TDBRCVW':
            ext.add(name)
    return ext, und


def write_abs_obj(path, names):
    """COFF object with one absolute external symbol per fnv_d_<addr>."""
    syms, strings = bytearray(), bytearray(b'\0\0\0\0')
    for n in names:
        off = len(strings)
        strings += n.encode() + b'\0'
        syms += struct.pack('<IIIhHBB', 0, off, int(n[6:], 16), SYM_ABSOLUTE, 0, EXTERNAL, 0)
    struct.pack_into('<I', strings, 0, len(strings))
    header = struct.pack('<HHIIIHH', 0x14c, 0, 0, 20, len(names), 0, 0)
    Path(path).write_bytes(header + bytes(syms) + bytes(strings))


def write_tls_obj(path, names, tls):
    """COFF object holding the original .tls template (from the git-ignored local/data_image.bin) in section
    .tls$FNV, which sorts between the CRT's .tls start marker and .tls$ZZZ, with fnv_t_<addr> at each variable's
    offset. SECREL references then resolve against the DLL's own TLS block."""
    meta = json.loads((ROOT / 'local/data_image.json').read_text())
    image = (ROOT / 'local/data_image.bin').read_bytes()
    start = tls[0] - int(meta['base'], 16)
    body = image[start:start + (tls[1] - tls[0])]
    body += bytes(-len(body) % 4)
    strings = bytearray(b'\0\0\0\0')
    syms = bytearray(struct.pack('<8sIhHBB', b'.tls$FNV', 0, 1, 0, STATIC, 1))
    syms += struct.pack('<IHHIHBBH', len(body), 0, 0, 0, 0, 0, 0, 0)      # section aux record
    for n in names:
        off = len(strings)
        strings += n.encode() + b'\0'
        syms += struct.pack('<IIIhHBB', 0, off, int(n[6:], 16) - tls[0], 1, 0, EXTERNAL, 0)
    # start marker: lifted original code reaches the original TLS layout through it (fnv_lift_tls_array)
    off = len(strings)
    strings += b'_fnv_tls_fnv_start\0'
    syms += struct.pack('<IIIhHBB', 0, off, 0, 1, 0, EXTERNAL, 0)
    struct.pack_into('<I', strings, 0, len(strings))
    data_ptr = 20 + 40
    symptr = data_ptr + len(body)
    header = struct.pack('<HHIIIHH', 0x14c, 1, 0, symptr, len(names) + 3, 0, 0)
    section = struct.pack('<8sIIIIIIHHI', b'.tls$FNV', 0, 0, len(body), data_ptr, 0, 0, 0, 0, 0xC0300040)
    Path(path).write_bytes(header + section + body + bytes(syms) + bytes(strings))


def write_addr_table(path, names):
    """COFF object with `fnv_addr_table`: {u32 original address, u32 relinked function} per fnv_f_<addr>, sorted by
    address and terminated by {0, 0}. It keeps every function alive under /OPT:REF and is what the loader uses to
    point the data image's vftables and callback tables at the relinked code."""
    names = sorted(names)
    n = len(names)
    name_index = {name: i for i, name in enumerate(names)}
    body = bytearray()
    for name in names:
        body += struct.pack('<II', int(name[6:], 16), 0)
    body += bytes(8)
    relocs = bytearray()
    overflow = n + 1 > 0xffff
    if overflow:          # IMAGE_SCN_LNK_NRELOC_OVFL: the real count is in the first relocation's address field
        relocs += struct.pack('<IIH', n + 1, 0, 0)
    for k, name in enumerate(names):
        target = 'fnv_f_00a5e9d0' if name == 'fnv_f_00a9a730' else name
        relocs += struct.pack('<IIH', k * 8 + 4, 2 + name_index[target], 0x06)  # DIR32 to function symbol
    strings = bytearray(b'\0\0\0\0')
    syms = bytearray(struct.pack('<8sIhHBB', b'.rdata', 0, 1, 0, STATIC, 1))
    syms += struct.pack('<IHHIHBBH', len(body), min(n + overflow, 0xffff), 0, 0, 0, 0, 0, 0)
    for name in names:
        off = len(strings)
        strings += name.encode() + b'\0'
        syms += struct.pack('<IIIhHBB', 0, off, 0, 0, 0x20, EXTERNAL, 0)
    off = len(strings)
    strings += b'_fnv_addr_table\0'          # C decoration on x86 (the export name is fnv_addr_table)
    syms += struct.pack('<IIIhHBB', 0, off, 0, 1, 0, EXTERNAL, 0)
    struct.pack_into('<I', strings, 0, len(strings))
    data_ptr = 20 + 40
    reloc_ptr = data_ptr + len(body)
    symptr = reloc_ptr + len(relocs)
    flags = 0x40300040 | (0x01000000 if overflow else 0)                # initialized read-only data, align 4
    header = struct.pack('<HHIIIHH', 0x14c, 1, 0, symptr, n + 3, 0, 0)
    section = struct.pack('<8sIIIIIIHHI', b'.rdata', 0, 0, len(body), data_ptr, reloc_ptr, 0,
                          0xffff if overflow else n, 0, flags)
    Path(path).write_bytes(header + section + bytes(body) + bytes(relocs) + bytes(syms) + bytes(strings))


def write_traps(path, traps, pops=None):
    """Trap stubs. `pops` {symbol: N}: the original function returns with `ret N` (from its code, or from its
    decorated name: tools/msmangle.py), so in discovery mode (port/runtime/fnv_trap.cpp) the stub can return 0
    with the original stack cleanup instead of stopping."""
    pops = pops or {}
    lines = ['// generated by tools/link_build.py: logging stubs for unbound code',
             'extern "C" void __cdecl fnv_trap(unsigned int address, const char* name);',
             'extern "C" void __cdecl fnv_trap_soft(unsigned int address, const char* name);']
    for k, (sym, (addr, name)) in enumerate(sorted(traps.items())):
        lines.append('extern "C" void t%d(void) __asm__("%s");' % (k, sym))
        text = name.replace('\\', '\\\\')
        if sym in pops:
            lines.append('static void b%d(void) { fnv_trap_soft(0x%08x, "%s"); }' % (k, addr, text))
            lines.append('extern "C" __declspec(naked) void t%d(void) { __asm { call b%d } __asm { xor eax, eax } '
                         '__asm { ret %d } }' % (k, k, pops[sym]))
        else:
            lines.append('extern "C" void t%d(void) { fnv_trap(0x%08x, "%s"); }' % (k, addr, text))
    Path(path).write_text('\n'.join(lines) + '\n')


def original_pops(exe, text, ranges):
    """{address: N} for the functions in `ranges` ({address: end}) whose every `ret` in the original pops N bytes
    (no ret, or mixed sizes: left out). From the cached whole-.text disassembly."""
    import lift_stubs
    cache = lift_stubs.text_cache(exe, (text['start'], text['end']))
    out = {}
    for a, end in ranges.items():
        sizes = set()
        for line in cache.lines(a, end):
            m = re.search(r'\tret\s*(0x[0-9a-f]+|\d+)?\s*$', line)
            if m:
                sizes.add(int(m.group(1), 0) if m.group(1) else 0)
        if len(sizes) == 1:
            out[a] = sizes.pop()
    return out


def cl(src, obj):
    vs = sorted(rp.VS.iterdir())[-1]
    sdk = sorted(Path(r'C:\Program Files (x86)\Windows Kits\10\Include').iterdir())[-1]
    cmd = [str(LLVM / 'clang-cl.exe'), '--target=i686-pc-windows-msvc', '/nologo', '/c', '/O2', '/MT', '/EHsc',
           '-w', '/I' + str(ROOT / 'port/compat'), '/I' + str(ROOT / 'port/runtime'),
           '/imsvc' + str(vs / 'include')]
    cmd += ['/imsvc' + str(sdk / p) for p in ('ucrt', 'shared', 'um')]
    r = subprocess.run(cmd + ['/Fo' + str(obj), str(src)], capture_output=True, text=True)
    if r.returncode:
        raise SystemExit('compile failed: %s\n%s' % (src, r.stdout[-2000:] + r.stderr[-2000:]))


def port_headers_hash():
    """Hash of the port's own headers: a change to any of them rebuilds every cached port object."""
    h = hashlib.sha1()
    for p in sorted((ROOT / 'port').rglob('*.h')):
        h.update(p.name.encode() + p.read_bytes())
    return h.hexdigest()


def cl_cached(job):
    """cl() unless the object exists and was built from the same source text and port headers. Returns True when
    it compiled."""
    src, obj, headers = job
    key = hashlib.sha1(Path(src).read_bytes() + headers.encode()).hexdigest()
    stamp = Path(str(obj) + '.key')
    if Path(obj).exists() and stamp.exists() and stamp.read_text() == key:
        return False
    cl(src, obj)
    stamp.write_text(key)
    return True


def write_if_changed(path, text):
    """Write text unless the file already holds it (keeps the mtime, so cached objects stay valid)."""
    path = Path(path)
    if not path.exists() or path.read_text() != text:
        path.write_text(text)


def cached(name, inputs, compute):
    """compute() memoised in out/cache/<name>_<key>.pickle, keyed by the sha1 of `inputs` (bytes)."""
    import pickle
    key = hashlib.sha1(inputs).hexdigest()[:16]
    path = ROOT / 'out/cache' / ('%s_%s.pickle' % (name, key))
    if path.exists():
        return pickle.loads(path.read_bytes())
    value = compute()
    path.parent.mkdir(parents=True, exist_ok=True)
    for old in path.parent.glob(name + '_*.pickle'):
        old.unlink()
    path.write_bytes(pickle.dumps(value, protocol=pickle.HIGHEST_PROTOCOL))
    return value


def build_loader(vs, sdk):
    """FalloutNV_relinked.exe: fixed at the original image base 0x00400000 so its .fnvimg section reserves the
    original data range (see port/loader/loader.cpp)."""
    cl(ROOT / 'port/loader/loader.cpp', LINK / 'loader.obj')
    cmd = [str(LLVM / 'lld-link.exe'), '/nologo', '/machine:x86', '/subsystem:console', '/base:0x400000', '/fixed',
           '/dynamicbase:no', '/largeaddressaware', '/out:' + str(LINK / 'FalloutNV_relinked.exe'),
           '/libpath:' + str(vs), '/libpath:' + str(sdk / 'ucrt/x86'), '/libpath:' + str(sdk / 'um/x86'),
           str(LINK / 'loader.obj'), 'libcmt.lib', 'kernel32.lib', 'user32.lib']
    r = subprocess.run(cmd, capture_output=True, text=True, errors='replace')
    return dict(exit=r.returncode, log=(r.stdout + r.stderr)[-2000:])


def original_calls(img, units):
    """{recipe function address: set of direct call/jump targets in the original}, EH tails included."""
    text = next(s for s in img.sections if s['name'] == '.text')
    code = img.data[text['raw']:text['raw'] + text['rsize']]
    base = text['start']
    spans = defaultdict(list)
    for u in units:
        for f in u['functions']:
            spans[int(f['address'], 16)].append((int(f['address'], 16), f['size']))
        for e in u.get('eh_tails', []):
            spans[int(e['parent'], 16)].append((int(e['address'], 16), e['size']))
    out = {}
    for fn, ranges in spans.items():
        targets = set()
        for start, size in ranges:
            o = start - base
            for k in range(o, o + size - 4):
                if code[k] in (0xE8, 0xE9):
                    targets.add((base + k + 5 + struct.unpack_from('<i', code, k + 1)[0]) & 0xffffffff)
        out[fn] = targets
    return out


STATIC_CLASS = set('CDKLSTYZ')       # MS mangling: static members / globals: <class><cc><return>
MEMBER_CLASS = set('ABEFIJMNQRUV')   # instance members: <class><this-cv><cc><return>


def return_kind(name):
    """'void' / 'value' from a decorated function name's return-type code, or None (ctor/dtor/unknown)."""
    if not name.startswith('?') or name.startswith(('??_', '??$')):
        return None
    i = name.find('@@')
    t = name[i + 2:] if i >= 0 else ''
    if not t:
        return None
    r = t[2:3] if t[0] in STATIC_CLASS else t[3:4] if t[0] in MEMBER_CLASS else ''
    if not r or r == '@':
        return None
    return 'void' if r == 'X' else 'value'


def void_mismatches(units):
    """Recipe functions defined as returning void while other units call the same address expecting a value. The
    decomp matched because the original leaves its last call's result in eax; clang does not, so callers read
    garbage (e.g. 0x00AA13E0, NiMemObject::operator new). The machine code is the authority: these are lifted from
    the original instead."""
    defs = {int(f['address'], 16): f['symbol'] for u in units for f in u['functions']}
    wants_value = set()
    for u in units:
        for k, v in u.get('symbols', {}).items():
            a = int(v, 0)
            if a in defs and return_kind(k) == 'value':
                wants_value.add(a)
    undecorated = {a for a, d in defs.items() if return_kind(d) is None and a in wants_value}
    src_void = source_void_returns(units, undecorated) if undecorated else set()
    out = {a for a, d in defs.items() if (return_kind(d) == 'void' or a in src_void) and a in wants_value}
    # machine-code evidence (tools/ret_users.py): a caller in the original reads eax/st0 after the call, or the call
    # is a tail pass-through of such a function. A `void` definition cannot supply that value.
    used = ret_used()
    if used:
        cand = {a for a in defs if a in used and a not in out}
        decl_void = {a for a in cand if return_kind(defs[a]) == 'void'}
        decl_void |= source_void_returns(units, {a for a in cand if return_kind(defs[a]) is None})
        out |= decl_void
    # indirect callers (vtable slots) are invisible to ret_users: judge from the instruction before each `ret`
    hint_p = ROOT / 'out/ret_hint.json'
    if hint_p.exists() and not os.environ.get('FNV_NO_RETHINT'):
        hint = {int(k, 16) for k in json.loads(hint_p.read_text())}
        cand2 = {a for a in defs if a in hint and a not in out}
        hv = {a for a in cand2 if return_kind(defs[a]) == 'void'}
        hv |= source_void_returns(units, {a for a in cand2 if return_kind(defs[a]) is None})
        out |= hv
    return out


def ret_used():
    p = ROOT / 'out/ret_used.json'
    if not p.exists():
        return {}
    return {int(k, 16): v for k, v in json.loads(p.read_text()).items()}


def leak_callers(units, orig_calls, owner):
    """Recipe functions (with an owning unit) whose unit references some callee under a name whose declared pop differs
    from the callee's original `ret N`, and which call that callee in the original code."""
    p = ROOT / 'out/pop_mismatch.json'
    if not p.exists():
        return set()
    mm = {int(a, 16): e for a, e in json.loads(p.read_text()).items()}
    out = set()
    for u in units:
        s = {int(v, 0) for n, v in u.get('symbols', {}).items()
             if int(v, 0) in mm and n in mm[int(v, 0)]['declared']}
        if not s:
            continue
        for f in u['functions']:
            fa = int(f['address'], 16)
            if orig_calls.get(fa, set()) & s and owner.get(fa) is not None:
                out.add(fa)
    return out


# Placeholder virtual methods the decomp declares on a by-value member (so clang calls them directly): dispatch through
# the object's vtable at the given byte offset instead.
VDISPATCH = {}      # filled per link: unresolved placeholder virtual names -> vtable byte offset
# Virtuals of abstract bases that the recipe binds to addresses which are not those functions (the original slots are
# _purecall): clang calls them directly on by-value members, so they dispatch through the object's vtable instead.
VSLOTS = {'?Init@BaseFormComponent@@UAEXXZ': 0, '?Free@BaseFormComponent@@UAEXXZ': 4,
          '?CopyFromBase@BaseFormComponent@@UAEXPAV1@@Z': 8, '?CompareWithBase@BaseFormComponent@@UAE_NPAV1@@Z': 12,
          '?SetModelFilePath@TESModel@@UAEXPBD@Z': 0x18}       # TESModel slot 6 (see tescasinochips.h)


def vdispatch_offset(name):
    """Byte offset in the vtable for a placeholder virtual (`?v04@Cls@@UAE...` = offset 4 when a multiple of 4,
    `?Name_v3@Cls@@UAE...` = slot 3), else None. Only thiscall virtuals (`UAE`)."""
    if name in VSLOTS:
        return VSLOTS[name]
    m = re.match(r'^\?(\w*?)@[^@]*(?:@[^@]*)*@@UAE', name)
    if not m:
        return None
    head = m.group(1)
    k = re.fullmatch(r'v([0-9a-f]{2})', head)
    if k and int(k.group(1), 16) % 4 == 0:
        return int(k.group(1), 16)
    k = re.search(r'_v(\d+)$', head)
    if k:
        return int(k.group(1)) * 4
    return None




def vtable_slots(path):
    """{virtual method name: byte offset} from the clang vftables (`??_7Class@@6B@`, primary only) an object defines."""
    p = CoffPatch(Path(path).read_bytes())
    syms = dict(p.symbols())
    out = {}
    for i, s in syms.items():
        if not (s['name'].startswith('??_7') and s['name'].endswith('@@6B@')) or s['section'] <= 0:
            continue
        h = 20 + (s['section'] - 1) * 40
        relptr, = struct.unpack_from('<I', p.data, h + 24)
        count, = struct.unpack_from('<H', p.data, h + 32)
        for off, sym in struct.iter_unpack('<II2x', p.data[relptr:relptr + count * 10]):
            name = syms[sym]['name'] if sym in syms else ''
            if off >= s['value'] and '@@UAE' in name and not name.startswith('??'):
                out.setdefault(name, set()).add(off - s['value'])
    return out


def declared_slots():
    """{(class, method): byte offset} for classes the decomp declares without a base class: the order of their
    `virtual` declarations is the vtable order (a virtual destructor takes one slot). Overloaded names are left out.
    Covers shell classes whose vtable clang never emits (no key function)."""
    head = re.compile(r'\b(?:struct|class)\s+(\w+)\s*\{')
    virt = re.compile(r'\bvirtual\b[^;{(]*?(~?\w+)\s*\(')
    found = defaultdict(set)
    for path in DECOMP.joinpath('src').rglob('*'):
        if path.suffix not in ('.cpp', '.h'):
            continue
        text = path.read_text(errors='replace')
        for m in head.finditer(text):
            depth, k, stmt, names = 1, m.end(), [], []
            start = k
            while k < len(text) and depth:
                c = text[k]
                if c == '{':
                    if depth == 1:
                        stmt.append(text[start:k])
                    depth += 1
                elif c == '}':
                    depth -= 1
                    if depth == 1:
                        start = k + 1
                elif c == ';' and depth == 1:
                    stmt.append(text[start:k])
                    start = k + 1
                k += 1
            for st in stmt:
                v = virt.search(st)
                if v:
                    names.append(v.group(1))
            if not names:
                continue
            counts = Counter(names)
            for i, n in enumerate(names):
                if counts[n] == 1:
                    found[(m.group(1), n)].add(i * 4)
    return {k: next(iter(v)) for k, v in found.items() if len(v) == 1}


def popfix_map():
    """{address: {name: declared pop}} for references whose declaration pops more than the original callee does
    (tools/pop_check.py). clang-compiled callers assume the declared pop; those names bind to adjusting thunks."""
    p = ROOT / 'out/pop_mismatch.json'
    if not p.exists():
        return {}
    out = {}
    for a, e in json.loads(p.read_text()).items():
        for name, d in e['declared'].items():
            if d != e['actual'] and (d - e['actual']) % 4 == 0 and e['actual'] % 4 == 0 and d % 4 == 0 and d >= 0:
                out.setdefault(int(a, 16), {})[name] = d
    return out


def write_popfix(path, popfix, actual):
    """Naked thunks: the real callee pops `actual` bytes; the caller was compiled to expect `declared`. The thunk
    parks the return address in the first unpopped argument slot, calls the callee, and returns with the rest."""
    lines = ['// generated by tools/link_build.py (popfix)']
    seen = set()
    for addr, names in sorted(popfix.items()):
        for d in sorted(set(names.values())):
            if (addr, d) in seen:
                continue
            seen.add((addr, d))
            a = actual[addr]
            lines.append('extern "C" void pf_target_%08x_%d(void) __asm__("fnv_f_%08x");' % (addr, d, addr))
            lines.append('extern "C" __declspec(naked) void pf_%08x_%d(void) __asm__("fnv_pf_%08x_%d");' % (addr, d, addr, d))
            if d > a:
                body = ('__asm { pop eax } __asm { mov [esp + %d], eax } __asm { call pf_target_%08x_%d } __asm { ret %d }'
                        % (a, addr, d, d - a - 4))
            else:
                # the callee pops more than the caller pushed: slide the return address and arguments down by the
                # difference, so the callee's `ret` leaves esp exactly where the caller expects it
                pad = a - d
                body = '__asm { sub esp, %d } ' % pad
                for i in range(1 + a // 4):      # the callee's own argument bytes: real ones when the caller pushed more
                    body += '__asm { mov eax, [esp + %d] } __asm { mov [esp + %d], eax } ' % (pad + 4 * i, 4 * i)
                body += '__asm { jmp pf_target_%08x_%d }' % (addr, d)
            lines.append('extern "C" __declspec(naked) void pf_%08x_%d(void) { %s }' % (addr, d, body))
    for k_, (name_, off_) in enumerate(sorted(VDISPATCH.items())):
        lines.append('extern "C" __declspec(naked) void vd_%d(void) __asm__("%s");' % (k_, name_))
        lines.append('extern "C" __declspec(naked) void vd_%d(void) { __asm { mov eax, [ecx] } __asm { jmp dword ptr [eax + %d] } }' % (k_, off_))
    Path(path).write_text(chr(10).join(lines) + chr(10))
    return len(seen)


def source_void_returns(units, wanted):
    """Addresses in `wanted` whose definition (the line after its `// @fnv 0x...` comment in the decomp source) is
    declared `void`. C-linkage names (`_FUN_00658c10`) carry no return code, so the source decides."""
    out = set()
    by_unit = {}
    for u in units:
        for f in u['functions']:
            a = int(f['address'], 16)
            if a in wanted:
                by_unit.setdefault(u['source'], set()).add(a)
    for source, addrs in by_unit.items():
        try:
            lines = (DECOMP / source).read_text(errors='replace').splitlines()
        except OSError:
            continue
        for k, line in enumerate(lines):
            m = re.match(r'\s*//\s*@fnv\s+0x([0-9a-fA-F]{8})', line)
            if not m or int(m.group(1), 16) not in addrs:
                continue
            j = k + 1
            while j < len(lines) and (not lines[j].strip() or lines[j].lstrip().startswith('//')):
                j += 1
            decl = re.sub(r'(extern\s+"C"|static|inline|__forceinline|__declspec\([^)]*\)|virtual)', ' ', lines[j] if j < len(lines) else '')
            if re.match(r'\s*void(?!\s*\*)', decl):
                out.add(int(m.group(1), 16))
    return out


def original_data_refs(img, units):
    """{recipe function address: set of absolute data addresses it references}, from the exe's .reloc fixups that
    lie inside the function (EH tails included)."""
    spans = sorted((int(f['address'], 16), f['size']) for u in units for f in u['functions'])
    spans += sorted((int(e['address'], 16), e['size'], int(e['parent'], 16)) for u in units
                    for e in u.get('eh_tails', []))
    owner = sorted((s[0], s[0] + s[1], s[2] if len(s) > 2 else s[0]) for s in spans)
    starts = [o[0] for o in owner]
    data = [(s['start'], s['end']) for s in img.sections if s['name'] in ('.rdata', '.data')]
    out = defaultdict(set)
    for va in img.relocs():
        k = bisect.bisect_right(starts, va) - 1
        if k < 0 or not owner[k][0] <= va < owner[k][1]:
            continue
        v = img.u32(va)
        if v is not None and any(a <= v < b for a, b in data):
            out[owner[k][2]].add(v)
    return out


def lift_initializers(exe, img, units, ctx, traps, defined, referenced=()):
    """Translate the __xc entries that are not decomp functions (and what they register with atexit) into
    out/link/lifted.cpp with tools/lift_stubs.py. Returns counts for the report."""
    import lift_stubs
    names = defaultdict(set)
    for u in units:
        for k, v in u.get('symbols', {}).items():
            names[int(v, 0)].add(k)

    sizes0 = {int(f['address'], 16): f['size'] for u in units for f in u['functions']}
    dyn_allow, dyn_bound = set(), {}
    text = next(s for s in img.sections if s['name'] == '.text')
    spans = sorted((int(f['address'], 16), f['size']) for u in units for f in u['functions'])
    starts = [s for s, _ in spans]

    def inside_recipe(addr):
        k = bisect.bisect_right(starts, addr) - 1
        return k >= 0 and addr < spans[k][0] + spans[k][1]

    def gap_end(addr):
        k = bisect.bisect_right(starts, addr)
        return min(starts[k] if k < len(starts) else addr + 0x2000, addr + 0x2000)

    def resolve(addr):
        if addr in ctx['starts']:
            if 'fnv_f_%08x' % addr in defined or addr in ctx['lift_replace']:
                return ('sym', 'fnv_f_%08x' % addr)
            # a recipe function nothing defines that lifted code calls: lift it as well (transitive fallback)
            dyn_allow.add(addr)
            dyn_bound[addr] = addr + sizes0[addr]
            return None
        if addr in ctx['crt_map']:
            return ('sym', ctx['crt_map'][addr])
        for n in sorted(names.get(addr, ())):     # sorted: the generated code must not depend on hash order
            alias = crt_alias(n, addr, ctx['libsyms'])
            if alias:
                return ('sym', alias)
        if text['start'] <= addr < text['end'] and not inside_recipe(addr):
            dyn_allow.add(addr)
            dyn_bound[addr] = gap_end(addr)
        return None

    table = [img.u32(p) for p in range(XC_TABLE[0], XC_TABLE[1], 4)]
    entries = sorted({v for v in table if v and v not in ctx['starts']})
    # game code between recipe functions that relinked code calls (fnv_c_ traps below the CRT): lifted too
    gaps = sorted(a for a, _ in traps.values() if a and text['start'] <= a < text['end'] and a not in ctx['starts']
                  and a not in ctx['crt_map'] and not inside_recipe(a))
    allow = set(gaps)
    lifter = lift_stubs.Lifter(exe, resolve, (text['start'], text['end']), gap_end=gap_end,
                               allow=lambda x: lift_stubs.LIFT_REGION[0] <= x < lift_stubs.LIFT_REGION[1] or x in allow
                               or x in dyn_allow)
    sizes = {s: n for s, n in spans}
    replace = sorted(ctx['lift_replace'])
    startup_targets_path = ROOT / 'out/startup_lift_targets.json'
    startup_targets = {int(x, 16) for x in json.loads(startup_targets_path.read_text())} if startup_targets_path.exists() else set()
    # OpenAI model Phase 1: runtime renderer callback reached through an original data pointer.
    startup_targets.add(0x00E7CD50)
    # fallback: recipe functions something references (or the initializer table names) that the relinked build does
    # not define (units that do not compile yet, functions clang never emitted): lifted from the original code
    missing = sorted({int(n[6:], 16) for n in set(traps) | {r for r in referenced if r not in defined}
                      if re.match(r'fnv_f_[0-9a-f]{8}$', n) and int(n[6:], 16) in sizes} |
                     {v for v in table if v in sizes and 'fnv_f_%08x' % v not in defined} |
                     {v for v in startup_targets if v in sizes})
    # code pointers stored in the data image (vtables, callback tables) that nothing defines: lifted too, so a virtual
    # call into a unit that does not compile yet runs the original code instead of a logging trap
    data_ptrs = set()
    for va_ in img.relocs():
        if 0x00FDF000 <= va_ < 0x01274000:
            v_ = img.u32(va_)
            if v_ and text['start'] <= v_ < text['end']:
                data_ptrs.add(v_)
    ptr_missing = sorted(v for v in data_ptrs if v in sizes and 'fnv_f_%08x' % v not in defined)
    ptr_other = sorted(v for v in data_ptrs if v not in sizes and not inside_recipe(v) and v not in ctx['crt_map'])
    missing = sorted(set(missing) | set(ptr_missing))
    missing = [m for m in missing if m not in ctx['lift_replace']]
    allow.update(ptr_other)
    allow.update(replace)
    allow.update(missing)
    bounded = dict((x, x + sizes[x]) for x in replace + missing)
    lifter.gap_end = lambda x: bounded[x] if x in bounded else dyn_bound[x] if x in dyn_bound else gap_end(x)
    # the original CRT's startup steps (the loader calls them before the initializers; __tmainCRTStartup itself is not run):
    # _heap_init, _mtinit (FLS index + main thread's _tiddata), _ioinit, and _cinit's first helper
    crt_startup = [0x00EDDB6A, 0x00ED1327, 0x00ED7E18, 0x00ED248E]
    crt_startup += [v for v in (img.u32(p_) for p_ in range(0x010113C4, 0x010113E8, 4)) if v]   # __xi_a..__xi_z (C initializers)
    allow.update(crt_startup)
    if os.environ.get('FNV_PROFILE_LIFT'):          # cProfile of the lifter -> out/cache/lift.prof
        import cProfile
        cProfile.runctx('lifter.run(roots)', globals(), dict(lifter=lifter, roots=entries + gaps + replace + missing + crt_startup + ptr_other),
                        str(ROOT / 'out/cache/lift.prof'))
    else:
        lifter.run(entries + gaps + replace + missing + crt_startup + ptr_other)
    for k, src in enumerate(lifter.shards(LIFT_SHARDS)):
        # OpenAI Phase 1: keep the recovered GetTaskletValue entry behind its null-safe ABI wrapper.
        if 'static void body_00b00a80' in src:
            src = src.replace('extern "C" __declspec(naked) void entry_00b00a80(void)',
                              'extern "C" __declspec(naked) void entry_00b00a80_lift(void) '
                              '__asm__("fnv_f_00b00a80__fnvlift");\n'
                              'extern "C" __declspec(naked) void entry_00b00a80_lift(void)')
        write_if_changed(LINK / ('lifted_%02d.cpp' % k), src)
    for g in gaps:                     # now defined by lifted.obj
        traps.pop('fnv_c_%08x' % g, None)
    for m in lifter.lifted:            # every lifted recipe address is now defined by lifted.obj
        traps.pop('fnv_f_%08x' % m, None)
    (LINK / 'lifted.json').write_text(json.dumps(dict(
        lifted=sorted(lifter.lifted), unsupported={'%#010x' % k: v for k, v in sorted(lifter.unsupported.items())},
        unbound_code_pointers=sorted(lifter.traps))))
    return dict(initializers=len(entries), gap_functions=len(gaps), replaced=len(replace), missing=len(missing),
                transitive=len(dyn_allow - set(missing)),
                lifted=len(lifter.lifted), cache_hits=lifter.cache_hits, resynced=getattr(lifter, 'resynced', 0),
                unsupported=len(lifter.unsupported),
                unsupported_top=sorted(Counter(re.sub(r'0x[0-9a-f]+', 'N', v) for v in lifter.unsupported.values())
                                       .items(), key=lambda x: -x[1])[:15],
                unbound_code_pointers=len(lifter.traps))


TIMES = []
_T0 = [None]


def tick(stage):
    """Record the seconds since the previous checkpoint under `stage` (printed and written to the report)."""
    import time
    now = time.perf_counter()
    if _T0[0] is not None:
        TIMES.append((stage, round(now - _T0[0], 1)))
        print('[time] %-28s %6.1fs' % (stage, now - _T0[0]), file=sys.stderr, flush=True)
    _T0[0] = now


def main():
    tick('start')
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', required=True)
    ap.add_argument('--jobs', type=int, default=os.cpu_count())
    a = ap.parse_args()
    units = json.loads((DECOMP / 'build/units.json').read_text())['units']
    for u in units:
        for n in VSLOTS:
            u.get('symbols', {}).pop(n, None)
    img = Image(a.exe)
    text = next(s for s in img.sections if s['name'] == '.text')
    seen = defaultdict(set)
    for u in units:
        for k, v in u.get('symbols', {}).items():
            seen[k].add(int(v, 0))
        for f in u['functions']:
            seen[f['symbol']].add(int(f['address'], 16))
    objs = {int(p.name[:4]): p for p in (ROOT / 'out/obj').glob('*.obj')
            if units[int(p.name[:4])]['source'] not in EXCLUDE}
    with ProcessPoolExecutor(a.jobs) as ex:
        tables = dict(zip(objs, ex.map(rp.nm, objs.values(), chunksize=32)))
    by_name = set().union(*(d for d, _ in tables.values()))
    compat = {'_' + n for n in re.findall(r'\b(_mm_\w+)\s*\(', (ROOT / 'port/compat/fnv_msvc_xmm.h').read_text())}
    fnv_map = json.loads((ROOT / 'out/fnv_map.json').read_text()) if (ROOT / 'out/fnv_map.json').exists() else {}
    crt_map = {int(k, 16): v['symbol'] for k, v in
               json.loads((ROOT / 'port/compat/crt_map.json').read_text())['map'].items()}
    iat_by_name = {}
    for va, full in img.imports().items():
        iat_by_name.setdefault(full.split('!', 1)[1], va)
    ctx = dict(units=units, fnv_map=fnv_map, crt_map=crt_map, iat_by_name=iat_by_name, starts={int(f['address'], 16): i for i, u in enumerate(units) for f in u['functions']},
               compiled=set(objs), text=(text['start'], text['end']), libsyms=rp.library_symbols(),
               global_names={k: next(iter(v)) for k, v in seen.items() if len(v) == 1},
               by_name=by_name, compat=compat,
               tls=next((t['start'], t['start'] + t['vsize']) for t in img.sections if t['name'] == '.tls'))

    (LINK / 'obj').mkdir(parents=True, exist_ok=True)
    jobs = [(i, str(p), str(LINK / 'obj' / p.name)) for i, p in sorted(objs.items())]
    # names defined (non-COMDAT) by more than one object: owner -1 (bound by address: every copy goes local) or the
    # first definer
    # A name collides when it has several definers and at least one is non-COMDAT (0) or NODUPLICATES (1).
    definers, strict = defaultdict(list), set()
    with ctx_pool(ctx, a.jobs) as ex:
        for index, names in ex.map(external_defs, jobs, chunksize=16):
            for n, sel in names:
                definers[n].append(index)
                if sel in (0, 1):
                    strict.add(n)
    ctx['dups'] = {n: (-1 if n in ctx['global_names'] else min(v)) for n, v in definers.items()
                   if len(v) > 1 and n in strict}
    # a few addresses are listed by two units built from one source with different flags: the first one owns it
    owner = {}
    for i, u in enumerate(units):
        if i in objs:
            for f in u['functions']:
                owner.setdefault(int(f['address'], 16), i)
    ctx['addr_owner'] = owner
    names_by_addr = defaultdict(set)
    for u in units:
        for k, v in u.get('symbols', {}).items():
            a_ = int(v, 0)
            if CRT_REGION[0] <= a_ < CRT_REGION[1]:
                names_by_addr[a_].add(k)
    ctx['names_at'] = dict(names_by_addr)
    # OpenAI model Phase 1: a32c70's source prototype is bool, but original callers pass a 32-bit Node pointer.
    # Lift the original body so its full-width argument reaches a31910 instead of being truncated to 0x20.
    # OpenAI model Phase 1: the source constructor dispatches through a mismatched vtable slot; the original calls 00482c80 directly.
    # 0x52bd40/0x52b8f0: TESImageSpaceModifier::ResetFormData/ctor (the compiled version leaks 4 bytes per loop iteration).
    # 0xc7f4a0 and its callers 0xc7f4e0/0xc7f500: an MSVC register-convention helper (eax/esi/edx), see hkvector4.cpp.
    # 0xd32ae0: the compiled source reads past its declared arguments and optimizes to a bare return.
    ctx['lift_replace'] = {a for a in void_mismatches(units) if owner.get(a) is not None} | {0x00A32C70, 0x00482C50, 0x0052BD40, 0x0052B8F0,
        0x00C7F4A0, 0x00C7F4E0, 0x00C7F500, 0x00D32AE0,
        0x00540C50}  # Land texture ctor: original calls ResetFormData directly, compiled virtual slot is null.
    ctx['popfix'] = {} if os.environ.get('FNV_NO_POPFIX') else popfix_map()
    for tok_ in os.environ.get('FNV_FORCE_LIFT', '').split(','):       # A/B tests: lift these recipe functions from the original
        if tok_.strip():
            ctx['lift_replace'].add(int(tok_, 16))
    # call-site correspondence against the original code (rule 4: the machine code is the authority)
    tick('symbol tables + dup scan')
    exe_key = img.data + (DECOMP / 'build/units.json').read_bytes()
    ctx['orig_calls'] = cached('orig_calls', exe_key, lambda: original_calls(img, units))
    ctx['orig_data_refs'] = cached('orig_data_refs', exe_key, lambda: original_data_refs(img, units))
    # Functions that call a target through a declaration whose stack pop disagrees with the original callee: the
    # original relies on the difference (leaked pushes become later arguments), clang code cannot. Lift them.
    if not os.environ.get('FNV_NO_LEAKLIFT'):
        ctx['lift_replace'] |= leak_callers(units, ctx['orig_calls'], owner)
    # MSVC register-convention helpers (arguments in eax/ebx/esi/edi, tools/reg_inputs.py) and their direct callers:
    # clang cannot reproduce that convention, lifted code passes every register
    if not os.environ.get('FNV_NO_REGLIFT'):
        import reg_inputs
        spans_ = {int(f['address'], 16): f['size'] for u in units for f in u['functions']}
        reg_in = cached('reg_inputs', exe_key + (ROOT / 'tools/reg_inputs.py').read_bytes(),
                        lambda: reg_inputs.flagged(a.exe, (text['start'], text['end']), spans_))
        reg_callers = {fn for fn, tg in ctx['orig_calls'].items() if tg & set(reg_in)}
        ctx['lift_replace'] |= {x for x in set(reg_in) | reg_callers if owner.get(x) is not None}
        stats_reg = (len(reg_in), len(reg_callers))
    else:
        stats_reg = (0, 0)
    meta = json.loads((ROOT / 'local/data_image.json').read_text())
    ctx['image'] = (int(meta['base'], 16), (ROOT / 'local/data_image.bin').read_bytes())
    ctx['inferred'], ctx['literal_bind'] = {}, {}
    constraints, lit_cand = [], {}
    with ctx_pool(ctx, a.jobs) as ex:
        for cs, lv in ex.map(infer_job, jobs, chunksize=16):
            constraints += cs
            for n, cand in lv:
                lit_cand[n] = lit_cand[n] & cand if n in lit_cand else set(cand)
    # a literal binds to the original location when every function using it agrees on exactly one address
    ctx['literal_bind'] = {n: next(iter(s)) for n, s in lit_cand.items() if len(s) == 1}
    literal_report = dict(referenced=len(lit_cand), bound=len(ctx['literal_bind']),
                          no_match=sum(1 for s in lit_cand.values() if not s),
                          ambiguous=sum(1 for s in lit_cand.values() if len(s) > 1))
    del ctx['image']                   # not needed by the patch workers
    names_at = defaultdict(set)
    for u in units:
        for f in u['functions']:
            names_at[int(f['address'], 16)].add(f['symbol'])
        for k, v in u.get('symbols', {}).items():
            names_at[int(v, 0)].add(k)
    tick('exe analysis + constraints')
    ctx['inferred'], infer_rounds = solve_call_sites(constraints, names_at)
    # The vtable name uses struct/unsigned aliases while the recipe defines the same MSVC slot with
    # class/enum parameter types. Both are one-word references, and the original virtual call is thiscall.
    renderer_method = '?CreateRendererData@Ni2DBuffer@@UAE_NABUNiPixelFormat@@I@Z'
    if 0x00A8F200 in ctx['starts']:
        ctx['inferred'].setdefault(renderer_method, 0x00A8F200)
    # NiMalloc (0xaa1070) under a namespaced declaration the recipe never maps (unit blk_00ec0000_od_ob1_variant)
    ctx['inferred'].setdefault('?NiMalloc@fnv_00ec20c0@@YAPAXI@Z', 0x00AA1070)
    tick('solve call sites')
    (LINK / 'inferred.json').write_text(json.dumps({n: '%#010x' % v for n, v in sorted(ctx['inferred'].items())},
                                                    indent=0))
    stats, traps = Counter(), {}
    stats['reg-input-functions'], stats['reg-input-callers'] = stats_reg
    if os.environ.get('FNV_DUMP_PATCH_CTX'):           # for profiling patch_object outside the pool
        import pickle
        (ROOT / 'out/cache/patch_ctx.pickle').write_bytes(pickle.dumps((ctx, jobs)))
    with ctx_pool(ctx, a.jobs) as ex:
        for _, s, t in ex.map(patch_object, jobs, chunksize=16):
            stats.update(s)
            traps.update(t)
    tick('patch objects')
    # every absolute data symbol any patched object now references, and every name still left undefined by all
    # patched objects, libraries and compat code (recipe functions clang never emitted, by-name copies made local)
    data_names, referenced, defined = set(), set(), set()
    with ProcessPoolExecutor(a.jobs) as ex:
        for ext, und in ex.map(external_symbols, [j[2] for j in jobs], chunksize=32):
            data_names |= {n for n in und if n.startswith('fnv_d_')}
            referenced |= und
            defined |= ext
    write_abs_obj(LINK / 'abs.obj', sorted(data_names))
    tls_names = {n for n in referenced if n.startswith('fnv_t_')}
    write_tls_obj(LINK / 'tls.obj', sorted(tls_names), ctx['tls'])
    # out-of-scope compiler-generated initializers (and the atexit thunks they register), lifted to C++
    tick('symbols of patched objects')
    lift_report = lift_initializers(a.exe, img, units, ctx, traps, defined, referenced)
    tick('lift (generate lifted.cpp)')
    # port runtime/compat code: built first, so nothing it defines gets a trap stub
    compat_defined = set()
    builds = COMPAT_SOURCES + LIFTED_OBJS
    headers = port_headers_hash()
    with ThreadPoolExecutor(os.cpu_count()) as ex:
        built = sum(ex.map(cl_cached, [(ROOT / src, LINK / obj, headers) for src, obj in builds]))
    stats['port-objects-rebuilt'] = built
    for src, obj in builds:
        ext, und = external_symbols(LINK / obj)
        compat_defined |= ext
        referenced |= und             # lifted code can name recipe functions no unit object defines
    # defined by tls.obj, or by the toolchain for implicit TLS (lld/CRT), never trap stubs
    generated = {'_fnv_tls_fnv_start', '__tls_array', '__tls_index', '__tls_used', '___tls_array'}
    # slots of every virtual in clang's own vftables: a direct call to an unbound virtual dispatches through the
    # object's vtable (agreeing names only)
    slot_sets = defaultdict(set)
    with ProcessPoolExecutor(a.jobs) as ex:
        for d_ in ex.map(vtable_slots, [j[1] for j in jobs], chunksize=32):
            for n_, v_ in d_.items():
                slot_sets[n_] |= v_
    slots_ = {n_: next(iter(v_)) for n_, v_ in slot_sets.items() if len(v_) == 1}
    stats['vtable-slot-names'] = len(slots_)
    decl_slots = cached('declared_slots', (DECOMP / 'build/units.json').read_bytes() + Path(__file__).read_bytes(),
                        declared_slots)
    for n_ in set(traps) | set(referenced):
        if n_ not in defined and (n_ not in traps or traps[n_][0] == 0):
            off_ = vdispatch_offset(n_)
            if off_ is None and n_ in slots_:
                off_ = slots_[n_]
            m_ = re.match(r'^\?(\w+)@(\w+)@', n_) if off_ is None and '@@UAE' in n_ else None
            if m_ and (m_.group(2), m_.group(1)) in decl_slots:
                off_ = decl_slots[(m_.group(2), m_.group(1))]
                stats['virtual-slots-from-declarations'] += 1
            if off_ is not None:
                VDISPATCH[n_] = off_
    for n_ in VDISPATCH:
        traps.pop(n_, None)
    stats['virtual-placeholders-dispatched'] = len(VDISPATCH)
    popfix_names = {n for n in referenced if n.startswith('fnv_pf_')} | set(VDISPATCH)
    for n in referenced - defined - data_names - tls_names - generated - ctx['libsyms'] - ctx['compat'] - compat_defined - set(traps) - popfix_names:
        m = re.match(r'fnv_[fc]_([0-9a-f]{8})$', n)
        traps[n] = (int(m.group(1), 16) if m else 0, n)
        stats['trap-never-defined'] += 1
    for n in compat_defined:
        traps.pop(n, None)
    import reach
    sizes = {int(f['address'], 16): f['size'] for u in units for f in u['functions']}
    _, tgt, is_call = reach.branches(a.exe, text)
    # outside the recipe, a function ends at the next recipe function or original call target
    starts_sorted = sorted(set(sizes) | {t for t, c in zip(tgt, is_call) if c})

    def trap_end(x):
        if x in sizes:
            return x + sizes[x]
        k = bisect.bisect_right(starts_sorted, x)
        return min(starts_sorted[k] if k < len(starts_sorted) else x + 0x1000, x + 0x1000)
    import msmangle
    trap_addrs = {addr for addr, _ in traps.values() if addr and text['start'] <= addr < text['end']}
    by_addr = original_pops(a.exe, text, {x: trap_end(x) for x in trap_addrs})
    pops = {}
    for sym, (addr, name) in traps.items():
        n = by_addr.get(addr) if addr else msmangle.stack_pop(DUP.sub('', name))
        if n is not None and not sym.startswith('fnv_d_'):
            pops[sym] = n
    write_traps(LINK / 'traps.cpp', traps, pops)
    pf_actual = {int(a, 16): e['actual'] for a, e in json.loads((ROOT / 'out/pop_mismatch.json').read_text()).items()}         if (ROOT / 'out/pop_mismatch.json').exists() else {}
    pf_needed = {}
    for n in popfix_names:
        m_ = re.match(r'fnv_pf_([0-9a-f]{8})_(\d+)$', n)
        if not m_:
            continue
        pf_needed.setdefault(int(m_.group(1), 16), {})[n] = int(m_.group(2))
    stats['popfix-thunks'] = write_popfix(LINK / 'popfix.cpp', pf_needed, pf_actual)
    stats['traps-soft-capable'] = len(pops)
    # what the build provides per original address (tools/reach.py, tools/explain.py)
    (LINK / 'provided.json').write_text(json.dumps(dict(
        relinked=sorted(int(n[6:], 16) for n in defined if re.match(r'fnv_f_[0-9a-f]{8}$', n)),
        traps={s: [addr, name] for s, (addr, name) in sorted(traps.items())})))
    functions = {n for n in defined | compat_defined if re.match(r'fnv_f_[0-9a-f]{8}$', n)} | \
                {n for n in traps if re.match(r'fnv_f_[0-9a-f]{8}$', n)}
    write_addr_table(LINK / 'addr_table.obj', functions)
    tick('compat + lifted compile')
    cl_cached((LINK / 'traps.cpp', LINK / 'traps.obj', headers))
    cl_cached((LINK / 'popfix.cpp', LINK / 'popfix.obj', headers))

    vs = sorted(rp.VS.iterdir())[-1] / 'lib/x86'
    sdk = sorted(rp.KITS.iterdir())[-1]
    rsp = LINK / 'link.rsp'
    inputs = [j[2] for j in jobs] + [str(LINK / n) for n in ('abs.obj', 'tls.obj', 'addr_table.obj', 'traps.obj', 'popfix.obj')] + \
             [str(LINK / obj) for _, obj in COMPAT_SOURCES + LIFTED_OBJS]
    rsp.write_text('\n'.join('"%s"' % x for x in inputs) + '\n')
    cmd = [str(LLVM / 'lld-link.exe'), '/nologo', '/dll', '/machine:x86', '/base:0x20000000',
           '/dynamicbase:no', '/fixed', '/safeseh:no', '/errorlimit:0', '/export:fnv_addr_table,DATA', '/export:fnv_lift_set_text', '/out:' + str(LINK / 'fnv_game.dll'),
           '/map:' + str(LINK / 'fnv_game.map'),
           '/libpath:' + str(vs), '/libpath:' + str(sdk / 'ucrt/x86'), '/libpath:' + str(sdk / 'um/x86'),
           'libcmt.lib', 'libvcruntime.lib', 'libucrt.lib', 'libcpmt.lib', 'legacy_stdio_definitions.lib',
           'kernel32.lib', 'user32.lib',
           '@' + str(rsp)]
    tick('traps compile')
    r = subprocess.run(cmd, capture_output=True, text=True, errors='replace')
    tick('lld-link')
    log = r.stdout + r.stderr
    (LINK / 'link.log').write_text(log)
    errs = Counter()
    undefined, duplicate = Counter(), Counter()
    for line in log.splitlines():
        m = re.match(r'.*error: (undefined symbol|duplicate symbol): (.*)', line)
        if m:
            (undefined if m.group(1).startswith('undef') else duplicate)[m.group(2).strip()] += 1
        elif 'error:' in line:
            errs[re.sub(r"'[^']*'|\S+\.obj|0x[0-9a-f]+", 'X', line.split('error:', 1)[1].strip())[:110]] += 1
    report = dict(patch=dict(stats), literals=literal_report, inferred=dict(names=len(ctx['inferred']), rounds=infer_rounds), lift=lift_report, traps=len(traps), abs_symbols=len(data_names),
                  addr_table=len(functions),
                  link_exit=r.returncode,
                  undefined=len(undefined), duplicate=len(duplicate), other_errors=errs.most_common(20),
                  undefined_top=undefined.most_common(25), duplicate_top=duplicate.most_common(25))
    if r.returncode == 0:
        report['loader'] = build_loader(vs, sdk)
        tick('loader build')
    report['times'] = TIMES
    (LINK / 'link_report.json').write_text(json.dumps(report, indent=1))
    print(json.dumps(report, indent=1))


if __name__ == '__main__':
    main()
