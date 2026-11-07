#!/usr/bin/env python3
"""Virtual-call inventory (Phase 0, spike 3).

Parses every translation unit of the decomp with libclang (MSVC i686 target, mingw-w64 SDK headers from the ziglang
wheel) and counts:

  classes: definitions that declare a C++ `virtual` method (directly or through a polymorphic base), and definitions
           that carry an explicit vtable pointer field (m_ppVirtualMethodTable, vtbl, vftable, m_pVtbl, ...)
  call sites (deduplicated by source location):
    cxx-virtual   x->Method() where Method is virtual and the call is not qualified (compiler emits a vptr load)
    field-vtable  calls through an explicit vtable field  (obj->m_ppVirtualMethodTable[n])(...)
    raw-vtable    calls through a cast of the object      (*(Fn**)obj)[n](...)
    other-fptr    any other call through a function pointer

usage: python tools/vcall_census.py [--decomp DIR] [--jobs N] [--limit N] [--out out/vcall_census.json]
"""
from decomp_path import DECOMP_DIR
import argparse
import json
import multiprocessing as mp
import os
import re
import sys
from collections import Counter, defaultdict
from pathlib import Path

import clang.cindex as ci
import ziglang

K = ci.CursorKind
ZIG = Path(ziglang.__file__).parent / 'lib'
VT_FIELDS = re.compile(r'^(m_ppVirtualMethodTable|vtbl|vftable|m_pVtbl|_vtable|vtable|__vfptr|m_vtbl|pVtbl|vptr|m_pVTable)$', re.I)
RAW_CAST = re.compile(r'\*\s*\*\s*\)')

ARGS = None
DECOMP = None


def args_for(decomp):
    return ['-x', 'c++', '--target=i686-pc-windows-msvc', '-fms-extensions', '-fms-compatibility',
            '-fms-compatibility-version=15.00', '-fdelayed-template-parsing', '-ferror-limit=0', '-w',
            '-nostdinc', '-nostdinc++',
            '-isystem', str(ZIG / 'include'), '-isystem', str(ZIG / 'libcxx/include'),
            '-isystem', str(ZIG / 'libc/include/any-windows-any'),
            '-D_WIN32', '-D_M_IX86=600', '-DWIN32', '-D_LIBCPP_HAS_NO_THREADS', '-D_LIBCPP_HARDENING_MODE_DEFAULT=_LIBCPP_HARDENING_MODE_NONE',
            '-I', str(Path(decomp) / 'include')]


def init(decomp):
    global ARGS, DECOMP
    DECOMP = str(Path(decomp).resolve()).lower()
    ARGS = args_for(decomp)


def ours(cur):
    f = cur.location.file
    return f is not None and str(Path(f.name).resolve()).lower().startswith(DECOMP)


def loc(cur):
    l = cur.location
    return '%s:%d:%d' % (Path(l.file.name).resolve().as_posix().lower(), l.line, l.column)


def callee_tokens(call):
    kids = list(call.get_children())
    if not kids:
        return ''
    return ' '.join(t.spelling for t in kids[0].get_tokens())


FN_KINDS = {ci.TypeKind.FUNCTIONPROTO, ci.TypeKind.FUNCTIONNOPROTO}


def is_fptr_callee(call):
    """True when the callee expression has pointer-to-function (or member-function-pointer) type."""
    kids = list(call.get_children())
    if not kids:
        return False
    t = kids[0].type.get_canonical()
    if t.kind == ci.TypeKind.POINTER:
        return t.get_pointee().get_canonical().kind in FN_KINDS
    return t.kind in FN_KINDS or t.kind == ci.TypeKind.MEMBERPOINTER


def walk(tu):
    classes = {}            # name -> dict(virtual, vtfield, bases, loc)
    calls = {}              # loc -> kind
    stack = [tu.cursor]
    while stack:
        c = stack.pop()
        k = c.kind
        if k in (K.STRUCT_DECL, K.CLASS_DECL, K.CLASS_TEMPLATE) and c.is_definition() and ours(c):
            name = c.type.spelling or c.spelling
            info = classes.setdefault(name, dict(virtual=False, vtfield=None, bases=[], loc=loc(c)))
            for ch in c.get_children():
                if ch.kind in (K.CXX_METHOD, K.DESTRUCTOR) and ch.is_virtual_method():
                    info['virtual'] = True
                elif ch.kind == K.FIELD_DECL and VT_FIELDS.match(ch.spelling) and info['vtfield'] is None:
                    info['vtfield'] = ch.spelling
                elif ch.kind == K.CXX_BASE_SPECIFIER:
                    info['bases'].append(ch.type.get_canonical().spelling)
        elif k == K.CALL_EXPR and ours(c):
            ref = c.referenced
            key = loc(c)
            if ref is not None and ref.kind in (K.CXX_METHOD, K.DESTRUCTOR) and ref.is_virtual_method():
                toks = callee_tokens(c)
                calls[key] = 'cxx-virtual-qualified' if '::' in toks.split('(')[0] else 'cxx-virtual'
            elif ref is None or ref.kind not in (K.FUNCTION_DECL, K.CXX_METHOD, K.CONSTRUCTOR, K.DESTRUCTOR,
                                                 K.FUNCTION_TEMPLATE, K.CONVERSION_FUNCTION):
                toks = callee_tokens(c)
                if not toks or toks.startswith(('_mm_', '__builtin')) or not is_fptr_callee(c):
                    pass        # intrinsics, functional casts/temporaries, unresolved expressions
                elif any(VT_FIELDS.match(t) for t in toks.split()):
                    calls[key] = 'field-vtable'
                elif RAW_CAST.search(toks) and '[' in toks:
                    calls[key] = 'raw-vtable'
                elif '[' in toks:
                    calls[key] = 'subscript-fptr'     # vt[n](...) through a local holding a loaded vtable
                else:
                    calls[key] = 'other-fptr'
        stack.extend(c.get_children())
    return classes, calls


def one(src):
    idx = ci.Index.create()
    try:
        tu = idx.parse(src, args=ARGS, options=ci.TranslationUnit.PARSE_SKIP_FUNCTION_BODIES * 0)
    except ci.TranslationUnitLoadError as e:
        return src, None, None, -1
    errors = sum(1 for d in tu.diagnostics if d.severity >= ci.Diagnostic.Error)
    classes, calls = walk(tu)
    return src, classes, calls, errors


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--decomp', default=DECOMP_DIR)
    ap.add_argument('--jobs', type=int, default=os.cpu_count())
    ap.add_argument('--limit', type=int, default=0)
    ap.add_argument('--out', default='out/vcall_census.json')
    a = ap.parse_args()
    units = json.loads((Path(a.decomp) / 'build/units.json').read_text())['units']
    srcs = sorted({str(Path(a.decomp) / u['source']) for u in units})
    if a.limit:
        srcs = srcs[:a.limit]
    classes, calls, errs, failed = {}, {}, {}, []
    with mp.Pool(a.jobs, initializer=init, initargs=(a.decomp,)) as pool:
        for i, (src, cl, ca, ne) in enumerate(pool.imap_unordered(one, srcs, chunksize=4)):
            if cl is None:
                failed.append(src); continue
            errs[src] = ne
            for n, v in cl.items():
                old = classes.get(n)
                if old is None:
                    classes[n] = v
                else:
                    old['virtual'] |= v['virtual']
                    old['vtfield'] = old['vtfield'] or v['vtfield']
                    old['bases'] = sorted(set(old['bases']) | set(v['bases']))
            calls.update(ca)
            if i % 200 == 0:
                print('%d/%d' % (i, len(srcs)), file=sys.stderr, flush=True)

    # polymorphism through bases
    def poly(n, seen=()):
        c = classes.get(n)
        if c is None or n in seen:
            return False
        return c['virtual'] or any(poly(b, seen + (n,)) for b in c['bases'])
    polymorphic = {n for n in classes if poly(n)}
    explicit = {n for n, c in classes.items() if c['vtfield']}
    both = polymorphic & explicit
    kinds = Counter(calls.values())
    errfree = sum(1 for v in errs.values() if v == 0)
    report = dict(
        translation_units=len(srcs), parsed=len(errs), failed=len(failed),
        tus_without_errors=errfree, total_parse_errors=sum(errs.values()),
        class_definitions=len(classes), polymorphic_classes=len(polymorphic),
        declaring_virtual_directly=sum(1 for c in classes.values() if c['virtual']),
        explicit_vtable_field_classes=len(explicit), both=len(both),
        vtable_field_names=dict(Counter(c['vtfield'] for c in classes.values() if c['vtfield'])),
        call_sites=dict(kinds), call_sites_total=sum(kinds.values()),
    )
    print(json.dumps(report, indent=1))
    Path(a.out).parent.mkdir(parents=True, exist_ok=True)
    Path(a.out).write_text(json.dumps(dict(report, worst_tus=sorted(errs.items(), key=lambda x: -x[1])[:40],
                                           failed_tus=failed, polymorphic=sorted(polymorphic),
                                           explicit=sorted(explicit)), indent=1))


if __name__ == '__main__':
    main()
