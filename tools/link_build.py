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
import json
import re
import struct
import subprocess
import sys
from collections import Counter, defaultdict
from concurrent.futures import ProcessPoolExecutor
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
]
XC_TABLE = (0x00FDF4A4, 0x010113C0)     # __xc_a..__xc_z of FalloutNV.exe 1.4.0.525 (see port/loader/loader.cpp)
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
        """Point every relocation that names symbol `old` at symbol `new`."""
        n = 0
        for sec in range(1, self.nsec + 1):
            h = 20 + (sec - 1) * 40
            relptr, = struct.unpack_from('<I', self.data, h + 24)
            count, = struct.unpack_from('<H', self.data, h + 32)
            if self.section_flags(sec) & 0x01000000:            # IMAGE_SCN_LNK_NRELOC_OVFL
                count, = struct.unpack_from('<I', self.data, relptr)
            for k in range(count):
                o = relptr + k * 10
                if struct.unpack_from('<I', self.data, o + 4)[0] == old:
                    struct.pack_into('<I', self.data, o + 4, new)
                    n += 1
        return n

    def bytes(self):
        strings = self.strings + bytes(self.extra)
        added = bytes(getattr(self, 'added', b''))
        head = bytearray(self.data[:self.strptr])
        struct.pack_into('<I', head, 12, self.nsym + len(added) // 18)
        return bytes(head) + added + struct.pack('<I', len(strings)) + strings[4:]


CTX = {}


def init(ctx):
    CTX.update(ctx)


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
        return addr is not None and addr not in c['starts'] and not (c['text'][0] <= addr < c['text'][1])

    def bind(name):
        base = DUP.sub('', name)
        if base == '__tls_index':
            return base, 'crt'          # the loader writes the DLL's own TLS index into the CRT's variable
        addr = int(syms[base], 0) if base in syms else c['global_names'].get(base)
        if addr is None:
            addr = c['inferred'].get(base)          # bound by call-site correspondence (infer_job)
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
        if addr is not None and c['tls'][0] <= addr < c['tls'][1]:
            return 'fnv_t_%08x' % addr, 'tls'
        if addr is not None:
            if addr in c['starts']:
                if c['starts'][addr] not in c['compiled']:
                    traps['fnv_f_%08x' % addr] = (addr, base)
                    return 'fnv_f_%08x' % addr, 'func-missing'
                return 'fnv_f_%08x' % addr, 'func'
            if c['text'][0] <= addr < c['text'][1]:
                if base in c['libsyms']:
                    return base, 'crt'
                alias = crt_alias(base, addr, c['libsyms'])
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
        if sec > 0 and name in own and s['storage'] in (EXTERNAL, STATIC) and sec not in vsec:
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
                new, kind = bind(name)
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
    """Call-site correspondence for one object. For each relinked function F (original address A): the names its
    call relocations (REL32) use that do not bind yet, against A's original call targets that no bound call of F
    explains. One unbound name against one unexplained recipe target is a vote that the name is that function."""
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
    votes = []
    for sec, a in sec_fn.items():
        names, bound = set(), set()
        for _, si, kind in coff.sections[sec]['relocs']:
            if kind != 0x14:
                continue
            n = coff.symbols[si]['name']
            if coff.symbols[si]['section'] > 0:
                if n in own:
                    bound.add(own[n])
                continue
            base = DUP.sub('', n)
            addr = lookup(base, unit)
            if addr is not None:
                bound.add(addr)
            elif not (base in c['libsyms'] or base in c['compat'] or base in c['by_name']):
                names.add(base)
        if len(names) != 1:
            continue
        cand = c['orig_calls'].get(a, set()) - bound - {a}
        if len(cand) == 1:
            votes.append((next(iter(names)), next(iter(cand))))
    return votes


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
    r = subprocess.run([rp.NM, '--no-sort', str(path)], capture_output=True, text=True, errors='replace')
    ext, und = set(), set()
    for line in r.stdout.splitlines():
        parts = line.split()
        if len(parts) == 2 and parts[0] == 'U':
            und.add(parts[1])
        elif len(parts) >= 3 and parts[1] in 'TDBRCVW':
            ext.add(parts[2])
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
    struct.pack_into('<I', strings, 0, len(strings))
    data_ptr = 20 + 40
    symptr = data_ptr + len(body)
    header = struct.pack('<HHIIIHH', 0x14c, 1, 0, symptr, len(names) + 2, 0, 0)
    section = struct.pack('<8sIIIIIIHHI', b'.tls$FNV', 0, 0, len(body), data_ptr, 0, 0, 0, 0, 0xC0300040)
    Path(path).write_bytes(header + section + body + bytes(syms) + bytes(strings))


def write_addr_table(path, names):
    """COFF object with `fnv_addr_table`: {u32 original address, u32 relinked function} per fnv_f_<addr>, sorted by
    address and terminated by {0, 0}. It keeps every function alive under /OPT:REF and is what the loader uses to
    point the data image's vftables and callback tables at the relinked code."""
    names = sorted(names)
    n = len(names)
    body = bytearray()
    for name in names:
        body += struct.pack('<II', int(name[6:], 16), 0)
    body += bytes(8)
    relocs = bytearray()
    overflow = n + 1 > 0xffff
    if overflow:          # IMAGE_SCN_LNK_NRELOC_OVFL: the real count is in the first relocation's address field
        relocs += struct.pack('<IIH', n + 1, 0, 0)
    for k in range(n):
        relocs += struct.pack('<IIH', k * 8 + 4, 2 + k, 0x06)       # DIR32 to symbol 2+k
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


def write_traps(path, traps):
    lines = ['// generated by tools/link_build.py: logging stubs for unbound code',
             'extern "C" void __cdecl fnv_trap(unsigned int address, const char* name);']
    for k, (sym, (addr, name)) in enumerate(sorted(traps.items())):
        lines.append('extern "C" void t%d(void) __asm__("%s");' % (k, sym))
        lines.append('extern "C" void t%d(void) { fnv_trap(0x%08x, "%s"); }' % (k, addr, name.replace('\\', '\\\\')))
    Path(path).write_text('\n'.join(lines) + '\n')


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


def build_loader(vs, sdk):
    """FalloutNV_relinked.exe: fixed at the original image base 0x00400000 so its .fnvimg section reserves the
    original data range (see port/loader/loader.cpp)."""
    cl(ROOT / 'port/loader/loader.cpp', LINK / 'loader.obj')
    cmd = [str(LLVM / 'lld-link.exe'), '/nologo', '/machine:x86', '/subsystem:console', '/base:0x400000', '/fixed',
           '/dynamicbase:no', '/largeaddressaware', '/out:' + str(LINK / 'FalloutNV_relinked.exe'),
           '/libpath:' + str(vs), '/libpath:' + str(sdk / 'ucrt/x86'), '/libpath:' + str(sdk / 'um/x86'),
           str(LINK / 'loader.obj'), 'libcmt.lib', 'kernel32.lib']
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


def lift_initializers(exe, img, units, ctx):
    """Translate the __xc entries that are not decomp functions (and what they register with atexit) into
    out/link/lifted.cpp with tools/lift_stubs.py. Returns counts for the report."""
    import lift_stubs
    names = defaultdict(set)
    for u in units:
        for k, v in u.get('symbols', {}).items():
            names[int(v, 0)].add(k)

    def resolve(addr):
        if addr in ctx['starts']:
            return ('sym', 'fnv_f_%08x' % addr)
        if addr in ctx['crt_map']:
            return ('sym', ctx['crt_map'][addr])
        for n in names.get(addr, ()):
            alias = crt_alias(n, addr, ctx['libsyms'])
            if alias:
                return ('sym', alias)
        return None

    table = [img.u32(p) for p in range(XC_TABLE[0], XC_TABLE[1], 4)]
    entries = sorted({v for v in table if v and v not in ctx['starts']})
    text = next(s for s in img.sections if s['name'] == '.text')
    lifter = lift_stubs.Lifter(exe, resolve, (text['start'], text['end']))
    lifter.run(entries)
    (LINK / 'lifted.cpp').write_text(lifter.source())
    return dict(initializers=len(entries), lifted=len(lifter.lifted), unsupported=len(lifter.unsupported),
                unsupported_top=sorted(Counter(re.sub(r'0x[0-9a-f]+', 'N', v) for v in lifter.unsupported.values())
                                       .items(), key=lambda x: -x[1])[:15],
                unbound_code_pointers=len(lifter.traps))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', required=True)
    ap.add_argument('--jobs', type=int, default=16)
    a = ap.parse_args()
    units = json.loads((DECOMP / 'build/units.json').read_text())['units']
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
    with ProcessPoolExecutor(a.jobs, initializer=init, initargs=(ctx,)) as ex:
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
    # call-site correspondence against the original code (rule 4: the machine code is the authority)
    ctx['orig_calls'] = original_calls(img, units)
    ctx['inferred'] = {}
    infer_rounds = []
    for _ in range(4):
        votes = defaultdict(Counter)
        with ProcessPoolExecutor(a.jobs, initializer=init, initargs=(ctx,)) as ex:
            for vs in ex.map(infer_job, jobs, chunksize=16):
                for n, addr in vs:
                    votes[n][addr] += 1
        new = {n: next(iter(v)) for n, v in votes.items() if len(v) == 1 and n not in ctx['inferred']}
        conflicting = sum(1 for v in votes.values() if len(v) > 1)
        infer_rounds.append(dict(new=len(new), conflicting=conflicting))
        if not new:
            break
        ctx['inferred'].update(new)
    stats, traps = Counter(), {}
    with ProcessPoolExecutor(a.jobs, initializer=init, initargs=(ctx,)) as ex:
        for _, s, t in ex.map(patch_object, jobs, chunksize=16):
            stats.update(s)
            traps.update(t)
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
    lift_report = lift_initializers(a.exe, img, units, ctx)
    # port runtime/compat code: built first, so nothing it defines gets a trap stub
    compat_defined = set()
    for src, obj in COMPAT_SOURCES + [('out/link/lifted.cpp', 'lifted.obj')]:
        cl(ROOT / src, LINK / obj)
        ext, und = external_symbols(LINK / obj)
        compat_defined |= ext
        referenced |= und             # lifted code can name recipe functions no unit object defines
    for n in referenced - defined - data_names - tls_names - ctx['libsyms'] - ctx['compat'] - compat_defined - set(traps):
        m = re.match(r'fnv_[fc]_([0-9a-f]{8})$', n)
        traps[n] = (int(m.group(1), 16) if m else 0, n)
        stats['trap-never-defined'] += 1
    for n in compat_defined:
        traps.pop(n, None)
    write_traps(LINK / 'traps.cpp', traps)
    functions = {n for n in defined | compat_defined if re.match(r'fnv_f_[0-9a-f]{8}$', n)} | \
                {n for n in traps if re.match(r'fnv_f_[0-9a-f]{8}$', n)}
    write_addr_table(LINK / 'addr_table.obj', functions)
    cl(LINK / 'traps.cpp', LINK / 'traps.obj')

    vs = sorted(rp.VS.iterdir())[-1] / 'lib/x86'
    sdk = sorted(rp.KITS.iterdir())[-1]
    rsp = LINK / 'link.rsp'
    inputs = [j[2] for j in jobs] + [str(LINK / n) for n in ('abs.obj', 'tls.obj', 'addr_table.obj', 'traps.obj')] + \
             [str(LINK / obj) for _, obj in COMPAT_SOURCES] + [str(LINK / 'lifted.obj')]
    rsp.write_text('\n'.join('"%s"' % x for x in inputs) + '\n')
    cmd = [str(LLVM / 'lld-link.exe'), '/nologo', '/dll', '/machine:x86', '/base:0x20000000',
           '/dynamicbase:no', '/fixed', '/safeseh:no', '/errorlimit:0', '/export:fnv_addr_table,DATA', '/out:' + str(LINK / 'fnv_game.dll'),
           '/libpath:' + str(vs), '/libpath:' + str(sdk / 'ucrt/x86'), '/libpath:' + str(sdk / 'um/x86'),
           'libcmt.lib', 'libvcruntime.lib', 'libucrt.lib', 'libcpmt.lib', 'kernel32.lib', 'user32.lib',
           '@' + str(rsp)]
    r = subprocess.run(cmd, capture_output=True, text=True, errors='replace')
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
    report = dict(patch=dict(stats), inferred=dict(names=len(ctx['inferred']), rounds=infer_rounds), lift=lift_report, traps=len(traps), abs_symbols=len(data_names),
                  addr_table=len(functions),
                  link_exit=r.returncode,
                  undefined=len(undefined), duplicate=len(duplicate), other_errors=errs.most_common(20),
                  undefined_top=undefined.most_common(25), duplicate_top=duplicate.most_common(25))
    if r.returncode == 0:
        report['loader'] = build_loader(vs, sdk)
    (LINK / 'link_report.json').write_text(json.dumps(report, indent=1))
    print(json.dumps(report, indent=1))


if __name__ == '__main__':
    main()
