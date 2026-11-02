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
import relink_plan as rp  # noqa: E402
from relink import Image  # noqa: E402

ROOT = rp.ROOT
DECOMP = rp.DECOMP
LINK = ROOT / 'out/link'
LLVM = Path(r'C:\Program Files\LLVM\bin')
DUP = re.compile(r'__fnvdup\d+')
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

    def bytes(self):
        strings = self.strings + bytes(self.extra)
        return bytes(self.data[:self.strptr]) + struct.pack('<I', len(strings)) + strings[4:]


CTX = {}


def init(ctx):
    CTX.update(ctx)


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
    own = {f['symbol']: int(f['address'], 16) for f in unit['functions']}
    syms = unit.get('symbols', {})
    p = CoffPatch(Path(src).read_bytes())
    entries = list(p.symbols())
    vsec = bound_vftable_sections(entries, index)
    for sec in vsec:
        p.flag_section(sec, LNK_REMOVE)
    stats, traps = Counter(), {}

    def bind(name):
        base = DUP.sub('', name)
        if base == '__tls_index':
            return base, 'crt'          # the loader writes the DLL's own TLS index into the CRT's variable
        addr = int(syms[base], 0) if base in syms else c['global_names'].get(base)
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
        elif s['storage'] == WEAK_EXTERNAL and name.startswith('??_E'):
            # clang's vector-deleting-dtor alias: only its (discarded) vftables use it
            p.rename(i, '%s__fnvlocal%d' % (name, index))
            stats['weak-vdtor-local'] += 1
        elif sec > 0 and s['storage'] == EXTERNAL and name in c['dups'] and c['dups'][name] != index:
            # another object owns this name (or it binds by address): keep this copy object-local
            p.rename(i, '%s__fnvlocal%d' % (name, index))
            p.set(i, storage=STATIC)
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
    Path(dst).write_bytes(p.bytes())
    return index, stats, traps


def external_defs(job):
    """Externally defined names of an object, minus its own recipe functions and clang's vftable/RTTI sections."""
    index, src, _ = job
    own = {f['symbol'] for f in CTX['units'][index]['functions']}
    p = CoffPatch(Path(src).read_bytes())
    entries = list(p.symbols())
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
           '-w', '/I' + str(ROOT / 'port/compat'), '/imsvc' + str(vs / 'include')]
    cmd += ['/imsvc' + str(sdk / p) for p in ('ucrt', 'shared', 'um')]
    r = subprocess.run(cmd + ['/Fo' + str(obj), str(src)], capture_output=True, text=True)
    if r.returncode:
        raise SystemExit('compile failed: %s\n%s' % (src, r.stdout[-2000:] + r.stderr[-2000:]))


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
    objs = {int(p.name[:4]): p for p in (ROOT / 'out/obj').glob('*.obj')}
    with ProcessPoolExecutor(a.jobs) as ex:
        tables = dict(zip(objs, ex.map(rp.nm, objs.values(), chunksize=32)))
    by_name = set().union(*(d for d, _ in tables.values()))
    compat = {'_' + n for n in re.findall(r'\b(_mm_\w+)\s*\(', (ROOT / 'port/compat/fnv_msvc_xmm.h').read_text())}
    ctx = dict(units=units, starts={int(f['address'], 16): i for i, u in enumerate(units) for f in u['functions']},
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
    for n in referenced - defined - data_names - tls_names - ctx['libsyms'] - ctx['compat'] - set(traps):
        m = re.match(r'fnv_[fc]_([0-9a-f]{8})$', n)
        traps[n] = (int(m.group(1), 16) if m else 0, n)
        stats['trap-never-defined'] += 1
    traps.pop('?terminate@@YAXXZ', None)
    write_traps(LINK / 'traps.cpp', traps)
    functions = {n for n in defined if re.match(r'fnv_f_[0-9a-f]{8}$', n)} |                 {n for n in traps if re.match(r'fnv_f_[0-9a-f]{8}$', n)}
    write_addr_table(LINK / 'addr_table.obj', functions)
    cl(LINK / 'traps.cpp', LINK / 'traps.obj')
    cl(ROOT / 'port/runtime/fnv_trap.cpp', LINK / 'fnv_trap.obj')
    cl(ROOT / 'port/compat/fnv_msvc_xmm.cpp', LINK / 'fnv_msvc_xmm.obj')
    cl(ROOT / 'port/compat/fnv_vc2008_shims.cpp', LINK / 'fnv_vc2008_shims.obj')

    vs = sorted(rp.VS.iterdir())[-1] / 'lib/x86'
    sdk = sorted(rp.KITS.iterdir())[-1]
    rsp = LINK / 'link.rsp'
    inputs = [j[2] for j in jobs] + [str(LINK / n) for n in ('abs.obj', 'tls.obj', 'addr_table.obj', 'traps.obj', 'fnv_trap.obj',
                                                            'fnv_msvc_xmm.obj', 'fnv_vc2008_shims.obj')]
    rsp.write_text('\n'.join('"%s"' % x for x in inputs) + '\n')
    cmd = [str(LLVM / 'lld-link.exe'), '/nologo', '/dll', '/noentry', '/machine:x86', '/base:0x20000000',
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
    report = dict(patch=dict(stats), traps=len(traps), abs_symbols=len(data_names), addr_table=len(functions),
                  link_exit=r.returncode,
                  undefined=len(undefined), duplicate=len(duplicate), other_errors=errs.most_common(20),
                  undefined_top=undefined.most_common(25), duplicate_top=duplicate.most_common(25))
    (LINK / 'link_report.json').write_text(json.dumps(report, indent=1))
    print(json.dumps(report, indent=1))


if __name__ == '__main__':
    main()
