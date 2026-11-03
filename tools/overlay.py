#!/usr/bin/env python3
"""Phase 1: source-transform overlay for units clang-cl rejects.

The decomp tree is never edited. For each failing unit this writes a patched copy to out/src/<source> and a record of
what changed to out/src/<source>.overlay.json. Fixes are driven by clang's own diagnostics, applied one round at a
time until the unit compiles or no rule applies:

  sse        the unit brings its own MSVC-layout union __m128 / _mm_* declarations: keep clang's SSE headers out
             (port/compat/fnv_sse_block.h, force-included) so the unit's declarations stand alone; the intrinsics are
             supplied as real functions by port/compat/fnv_msvc_xmm.cpp
  dup        "conflicting types" / "different type" / linkage conflicts: rename the declaration and the uses after
             it to NAME__fnvdupK; the relink strips the marker, so the name still binds to its original address
  nsnew      operator new/delete declared or defined inside a namespace (MSVC extension): removed; global
             operator new binds to the game's allocator by address, placement new to the standard one

usage: python tools/overlay.py [--only TEXT] [--rounds N] [--jobs N]
"""
import argparse
import json
import os
import re
import subprocess
import sys
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import compile_census as cc  # noqa: E402

ROOT = cc.ROOT
DECOMP = Path(r'E:\Games\fnv_decomp-main')
OUT = ROOT / 'out/src'
SSE_BLOCK = ROOT / 'port/compat/fnv_sse_block.h'
MSVC_XMM = ROOT / 'port/compat/fnv_msvc_xmm.h'
OWN_M128 = re.compile(r'\b(union|struct)\s*(__declspec\((?:[^()]|\([^()]*\))*\)\s*|_CRT_ALIGN\(\d+\)\s*)*__m128\b')
DIAG = re.compile(r'^(?P<file>.*?)\((?P<line>\d+),(?P<col>\d+)\) ?:(?P<ranges>(?:\{\d+:\d+-\d+:\d+\})*):? ?'
                  r'(?P<kind>error|note): (?P<msg>.*)$')
RANGE = re.compile(r'\{(\d+):(\d+)-(\d+):(\d+)\}')
HELPERS = None   # set below (port/compat/fnv_overlay_helpers.h)
DUP_MSGS = ("conflicting types for '", "redeclaration of '", "declaration of '",
            "redefinition of '")
SSE_DEF = re.compile(r'union\s+(__declspec\([^)]*\)\s*|_CRT_ALIGN\(\d+\)\s*)*__m128\b|\bm128_(f32|u32|i32|u64)\b'
                     r'|\b_mm_\w+\s*\(')


FIXIT = re.compile(r'^fix-it:"(?P<file>.*?)":\{(?P<l1>\d+):(?P<c1>\d+)-(?P<l2>\d+):(?P<c2>\d+)\}:"(?P<text>.*)"$')
MODERN = ['-fms-compatibility-version=19.44', '/std:c++17']
HELPERS = ROOT / 'port/compat/fnv_overlay_helpers.h'


def compile_unit(unit, src, sse, obj='NUL', modern=False):
    cmd = cc.command(unit, str(DECOMP), obj)
    cmd[-1] = str(src)
    extra = ['/I' + str((DECOMP / unit['source']).parent), '/I' + str(SSE_BLOCK.parent),
             '/clang:-fdiagnostics-parseable-fixits', '/clang:-fdiagnostics-print-source-range-info',
             '/FI' + str(HELPERS)]
    if sse:
        extra.append('/FI' + str(SSE_BLOCK))
    if sse == 'types':
        extra.append('/FI' + str(MSVC_XMM))
    if modern:          # units that pull in the modern STL headers need the language mode those headers expect
        cmd = [c for c in cmd if not c.startswith('-fms-compatibility-version=')]
        extra += MODERN
    cmd[1:1] = extra
    r = subprocess.run(cmd, capture_output=True, text=True, errors='replace')
    diags = []
    for l in r.stderr.splitlines():
        m = DIAG.match(l)
        if m:
            diags.append(dict(m.groupdict(), line=int(m['line']), col=int(m['col']), fixits=[],
                              ranges=[tuple(map(int, r)) for r in RANGE.findall(m['ranges'] or '')]))
            continue
        f = FIXIT.match(l)
        if f and diags:
            diags[-1]['fixits'].append(dict(file=f['file'].replace('\\\\', '\\'), l1=int(f['l1']), c1=int(f['c1']),
                                             l2=int(f['l2']), c2=int(f['c2']), text=f['text']))
    return r.returncode == 0, diags


def same_file(a, b):
    try:
        return Path(a).resolve() == Path(b).resolve()
    except OSError:
        return False


def namespace_depths(lines):
    """Depth of enclosing `namespace X {` blocks at the start of each line (simple brace scanner)."""
    depth, stack, out = 0, [], []
    pending_ns = False
    for l in lines:
        out.append(sum(1 for k in stack if k))
        code = re.sub(r'//.*', '', l)
        for tok in re.finditer(r'\bnamespace\b(\s+\w+)?\s*(?=\{)|[{}]', code):
            t = tok.group(0)
            if t.startswith('namespace'):
                pending_ns = True
            elif t == '{':
                stack.append(pending_ns); pending_ns = False
            elif stack:
                stack.pop()
    return out


def remove_decl(lines, ln):
    """Blank the declaration or definition starting at 1-based line ln; returns the number of lines blanked."""
    i = ln - 1
    text = lines[i]
    if '{' not in text and text.rstrip().endswith(';'):
        lines[i] = '// [overlay nsnew] ' + text
        return 1
    depth, j, seen = 0, i, False
    while j < len(lines):
        depth += lines[j].count('{') - lines[j].count('}')
        seen |= '{' in lines[j]
        if seen and depth <= 0:
            break
        j += 1
    for k in range(i, min(j + 1, len(lines))):
        lines[k] = '// [overlay nsnew] ' + lines[k]
    return j - i + 1


def scope_end(lines, start):
    """Last line (1-based) of the block that encloses line `start`: where brace depth first drops below its level.
    For a declaration at file scope this is the end of the file."""
    depth = 0
    for i in range(start - 1, len(lines)):
        code = re.sub(r'//.*', '', lines[i])
        for ch in code:
            if ch == '{':
                depth += 1
            elif ch == '}':
                depth -= 1
                if depth < 0:
                    return i + 1
    return len(lines)


def rename_from(lines, name, starts):
    """Rename `name` to name__fnvdupK from the K-th conflicting declaration until the next one, or until the block
    holding that declaration closes (block-scope `extern` declarations inside function bodies are common)."""
    pat = re.compile(r'(?<![\w$])' + re.escape(name) + r'(?![\w$])')
    starts = sorted(set(starts))
    for k, s in enumerate(starts):
        end = starts[k + 1] - 1 if k + 1 < len(starts) else len(lines)
        end = min(end, scope_end(lines, s))
        new = '%s__fnvdup%d' % (name, k + 1)
        for i in range(s - 1, end):
            if '#include' not in lines[i]:
                lines[i] = pat.sub(new, lines[i])


SIMD_TYPES = {'__m128': 'm128_', '__m128i': 'm128i_', '__m64': 'm64_'}
SIMD_DEF = re.compile(r'(\btypedef\s+)?\b(union|struct)\s*(__declspec\((?:[^()]|\([^()]*\))*\)\s*|_CRT_ALIGN\(\d+\)\s*)*'
                      r'(__m128i|__m128|__m64)\b\s*\{')


def unify_simd_types(text):
    """Collapse the unit's own copies of MSVC's SIMD unions onto the global compat types (fnv_msvc_xmm.h): a copy at
    file scope is dropped, a copy inside a namespace becomes `typedef ::T T;`. Only copies whose members are all the
    standard MSVC ones (m128_f32, ...) are touched. Returns (text, count, all_unified)."""
    out, pos, n, all_ok = [], 0, 0, True
    depth_at = namespace_depths(text.splitlines())
    for m in SIMD_DEF.finditer(text):
        if m.start() < pos:
            continue
        name = m.group(4)
        i, depth = m.end() - 1, 0
        while i < len(text):
            depth += {'{': 1, '}': -1}.get(text[i], 0)
            if depth == 0:
                break
            i += 1
        end = text.find(';', i) + 1
        body = text[m.end():i]
        members = re.findall(r'(\w+)\s*(?:\[[^\]]*\])?\s*;', body)
        if not members or not all(x.startswith(SIMD_TYPES[name]) for x in members):
            all_ok = False
            continue
        line = text.count('\n', 0, m.start())
        repl = ('typedef ::%s %s;' % (name, name)) if depth_at[line] else ''
        out.append(text[pos:m.start()])
        out.append('/* [overlay sse] own %s removed */ %s' % (name, repl) + '\n' * text.count('\n', m.start(), end))
        pos = end
        n += 1
    out.append(text[pos:])
    return ''.join(out), n, all_ok


FIXIT_OK = ("is a pointer; did you mean to use '->'", "template specialization requires 'template<>'")
STL_HEADERS = re.compile(r'[\\/](xtr1common|type_traits|xutility|xmemory|xstring|utility|limits|xtree|vector|string|'
                         r'memory|cstdint|cstddef|yvals|yvals_core|vcruntime\w*|xkeycheck\.h|new|exception)$', re.I)


def apply_fixits(lines, diags, src, record):
    """Clang's own fix-its for whitelisted diagnostics (same-line edits only, applied right to left)."""
    edits = {}
    for d in diags:
        if d['kind'] == 'error' and any(k in d['msg'] for k in FIXIT_OK):
            for f in d['fixits']:
                if f['l1'] == f['l2'] and same_file(f['file'], src):
                    edits.setdefault(f['l1'], []).append((f['c1'], f['c2'], f['text']))
    for ln, es in edits.items():
        line = lines[ln - 1]
        for c1, c2, text in sorted(set(es), reverse=True):
            line = line[:c1 - 1] + text + line[c2 - 1:]
        lines[ln - 1] = line
        record.append(dict(rule='fixit', line=ln, edits=len(es)))
    return bool(edits)


def drop_extern_c(lines, diags, src, record):
    """extern "C" functions declared in several namespaces with different types are one entity in C++ (VC2008
    accepted them). Dropping extern "C" from the ones inside a namespace makes each an ordinary namespaced function;
    the relink binds FUN_<addr>/DAT_<addr> names to their address whatever the namespace."""
    depth = namespace_depths(lines)
    hits = set()
    for d in diags:
        if not same_file(d['file'], src) or d['line'] > len(lines):
            continue          # a conflict reported inside an SDK header still has its note (handled below) here
        if d['kind'] == 'error' and not re.match(r"(conflicting types for|declaration of|redeclaration of|"
                                                 r"functions that differ only in their return type)", d['msg']):
            continue
        if d['kind'] == 'note' and 'previous declaration' not in d['msg']:
            continue
        i = d['line'] - 1
        if depth[i] and 'extern "C"' in lines[i]:
            hits.add(i)
    for i in sorted(hits):
        lines[i] = lines[i].replace('extern "C"', 'extern /* [overlay dropc] */', 1)
        record.append(dict(rule='dropc', line=i + 1))
    return bool(hits)


CONST_INIT = re.compile(r"cannot initialize (?:return object|a variable) of type '([^']+)' with an (?:rvalue|lvalue) "
                        r"of type 'const [^']+'")


def const_casts(lines, diags, src, record):
    """VC2008 let a const pointer (e.g. `cond ? p : ""`) initialise a non-const one; clang does not. Cast the
    expression to the target type named by the diagnostic, for `return EXPR;` and `T name = EXPR;` lines only."""
    changed = False
    for d in diags:
        a = ASSIGN_LITERAL.match(d['msg'])
        if a and d['kind'] == 'error' and same_file(d['file'], src):
            i = d['line'] - 1
            new = re.sub(r'^(\s*[^=;]+?=\s*)("(?:[^"\\]|\\.)*")(\s*;.*)$',
                         lambda r: '%s(%s)%s%s' % (r.group(1), a.group(1), r.group(2), r.group(3)), lines[i])
            if new != lines[i]:
                lines[i] = new
                record.append(dict(rule='constcast', line=d['line']))
                changed = True
            continue
        m = CONST_INIT.match(d['msg'])
        if d['kind'] != 'error' or not m or not same_file(d['file'], src):
            continue
        i, target = d['line'] - 1, m.group(1)
        line = lines[i]
        new = re.sub(r'^(\s*return\s+)(.+);(\s*(//.*)?)$', lambda r: '%s(%s)(%s);%s' % (r.group(1), target, r.group(2),
                                                                                  r.group(3)), line)
        if new == line:
            new = re.sub(r'^(\s*[\w:<>\s\*&]+?\b\w+\s*=\s*)(.+);(\s*(//.*)?)$',
                         lambda r: '%s(%s)(%s);%s' % (r.group(1), target, r.group(2), r.group(3)), line)
        if new != line:
            lines[i] = new
            record.append(dict(rule='constcast', line=d['line']))
            changed = True
    return changed


ASSIGN_LITERAL = re.compile(r"assigning to '([^']+)' from incompatible type 'const char\[\d+\]'")


def lvalue_wraps(lines, diags, src, record):
    """VC2008 bound temporaries to non-const references and allowed `(T&)rvalue`. Wrap the temporary in
    fnv_lvalue(...) (port/compat/fnv_overlay_helpers.h), using clang's source ranges: the argument for a binding
    error, the operand (last range) for a C-style cast. Single-line ranges only, applied right to left."""
    edits = {}
    for d in diags:
        if d['kind'] != 'error' or not same_file(d['file'], src) or not d['ranges']:
            continue
        if 'cannot bind to a temporary of type' in d['msg']:
            r = d['ranges'][0]
        elif 'C-style cast from rvalue to reference type' in d['msg'] or \
                'reinterpret_cast from rvalue to reference type' in d['msg']:
            r = d['ranges'][-1]
        else:
            continue
        l1, c1, l2, c2 = r
        # point insertions, so a range may span lines: '(' opener at the start, ')' at the end
        edits.setdefault(l1, set()).add((c1, 'fnv_lvalue('))
        edits.setdefault(l2, set()).add((c2, ')'))
    for ln, points in edits.items():
        line = lines[ln - 1]
        # right to left; at one column a closing ')' goes before an opening wrapper
        for col, text in sorted(points, key=lambda p: (p[0], p[1] != ')'), reverse=True):
            line = line[:col - 1] + text + line[col - 1:]
        lines[ln - 1] = line
        record.append(dict(rule='lvalue', line=ln, edits=len(points)))
    return bool(edits)


def literal_param_casts(lines, diags, src, record):
    """VC2008 passed a string literal to a non-const pointer parameter (`f("")` with `void*`); cast the argument
    (clang's source range) to the parameter type named by the diagnostic. Right to left per line."""
    edits = {}
    for d in diags:
        m = re.match(r"cannot initialize a parameter of type '([^']+\*)' with an lvalue of type 'const char\[\d+\]'",
                     d['msg'])
        if d['kind'] != 'error' or not m or not same_file(d['file'], src) or not d['ranges']:
            continue
        l1, c1, _, _ = d['ranges'][0]
        edits.setdefault(l1, set()).add((c1, '(%s)' % m.group(1)))
    for ln, points in edits.items():
        line = lines[ln - 1]
        for col, text in sorted(points, reverse=True):
            line = line[:col - 1] + text + line[col - 1:]
        lines[ln - 1] = line
        record.append(dict(rule='literalcast', line=ln))
    return bool(edits)


def interlocked_addresses(lines, diags, src, record):
    """`&InterlockedX` used as a __stdcall function pointer: point it at the matching wrapper
    (port/compat/fnv_overlay_helpers.h); in clang the name is an intrinsic with no such address."""
    changed = False
    for d in diags:
        m = re.match(r"address of overloaded function '_?Interlocked(\w+)' does not match required type", d['msg'])
        if d['kind'] != 'error' or not m or not same_file(d['file'], src):
            continue
        i = d['line'] - 1
        new = re.sub(r'&\s*_?Interlocked%s\b' % m.group(1), '&fnv_Interlocked%s_stdcall' % m.group(1), lines[i])
        if new != lines[i]:
            lines[i] = new
            record.append(dict(rule='interlocked', line=d['line']))
            changed = True
    return changed


def inherited_member_specializations(lines, diags, src, record):
    """`template <> R C<A>::f(P) {...}` where f is inherited from a base of C (VC2008 accepted it as a definition at
    the recipe address): give the body a home in a helper struct deriving from C<A> that declares f, so `this` and the
    inherited members keep working. Its mangled name changes; the @fnv source map binds it to its address. Any
    forward declaration the specdecl rule added is dropped."""
    changed = False
    for d in diags:
        if d['kind'] != 'error' or not same_file(d['file'], src) or \
                not d['msg'].startswith('no function template matches function template specialization'):
            continue
        i = d['line'] - 1
        m = re.match(r'^(\s*)template\s*<\s*>\s*(.+?)\s+(\w+\s*<.*>)::(\w+)\s*\((.*)\)\s*(const)?\s*(;)?\s*(//.*)?$',
                     lines[i])
        if not m:
            continue
        if m.group(7):                       # a declaration (e.g. the specdecl line): drop it
            lines[i] = '// [overlay inherited-spec] ' + lines[i]
            changed = True
            continue
        # a `template <>` written on its own line above (comments may sit between) goes with the definition
        j = i - 1
        while j >= 0 and (not lines[j].strip() or lines[j].strip().startswith('//')):
            j -= 1
        if j >= 0 and re.match(r'^\s*template\s*<\s*>\s*$', lines[j]):
            lines[j] = '// [overlay inherited-spec] ' + lines[j]
        k = sum(1 for c in record if c['rule'] == 'inherited-spec') + 1
        helper = 'FnvSpecHelper%d' % k
        const = ' const' if m.group(6) else ''
        lines[i] = '%sstruct %s : %s { %s %s(%s)%s; };\n%s%s %s::%s(%s)%s' % (
            m.group(1), helper, m.group(3), m.group(2), m.group(4), m.group(5), const,
            m.group(1), m.group(2), helper, m.group(4), m.group(5), const)
        record.append(dict(rule='inherited-spec', line=d['line'], helper=helper))
        changed = True
    return changed


def exception_spec_and_delete(lines, diags, src, record):
    """Two VC2008 leniencies:
       * `throw()` in a function-pointer typedef: dropped (exception specifications have no effect on codegen here);
       * a class with only a placement `operator delete` under a virtual destructor: C++ deletes the destructor,
         VC2008 fell back to the global operator delete; declare a usual one that does exactly that."""
    changed = False
    for d in diags:
        if d['kind'] != 'error' or not same_file(d['file'], src):
            continue
        i = d['line'] - 1
        if d['msg'] == 'exception specifications are not allowed in typedefs':
            new = re.sub(r'\bthrow\s*\([^)]*\)', '', lines[i])
            if new != lines[i]:
                lines[i] = new
                record.append(dict(rule='typedef-throw', line=d['line']))
                changed = True
        elif re.match(r"deleted function '~\w+' cannot override a non-deleted function", d['msg']):
            j = i
            while j < len(lines) and '{' not in lines[j]:
                j += 1
            if j < len(lines) and 'operator delete(void* p) { ::operator delete(p); }' not in lines[j]:
                k = lines[j].index('{')
                lines[j] = lines[j][:k + 1] + ' public: void operator delete(void* p) { ::operator delete(p); } ' + \
                    lines[j][k + 1:]
                record.append(dict(rule='usual-delete', line=d['line']))
                changed = True
    return changed


def explicit_constructor_calls(lines, diags, src, record):
    """`X::X();` as a statement in a member of X (VC2008 ran the constructor on this): placement-construct this."""
    changed = False
    for d in diags:
        m = re.match(r"qualified reference to '(\w+)' is a constructor name rather than a type", d['msg'])
        if d['kind'] != 'error' or not m or not same_file(d['file'], src):
            continue
        i, x = d['line'] - 1, m.group(1)
        new = re.sub(r'^(\s*)(?:this->)?%s::%s\s*\(\s*\)\s*;' % (x, x), r'\1fnv_construct_in_place(this);', lines[i])
        if new != lines[i]:
            lines[i] = new
            record.append(dict(rule='ctor-call', line=d['line']))
            changed = True
    return changed


def declaration_start(lines, i):
    """0-based index of the first line of the namespace-scope declaration that contains line i."""
    ns = namespace_depths(lines)
    total, depth = [], 0
    for l in lines:                      # brace depth at the start of each line
        total.append(depth)
        code = re.sub(r'//.*', '', l)
        depth += code.count('{') - code.count('}')
    j = i
    while j > 0 and total[j] > ns[j]:    # walk out of function/class bodies
        j -= 1
    # then back over the declaration's own leading lines (return type, template<>, attributes)
    while j > 0:
        prev = lines[j - 1].strip()
        if not prev or prev.startswith(('//', '#', '/*')) or prev.endswith((';', '}', '{')) or total[j - 1] > ns[j - 1]:
            break
        j -= 1
    return j


def specialization_before_use(lines, diags, src, record):
    """A member specialisation defined after the template was instantiated (VC2008 still used it everywhere):
    declare it ahead of the declaration holding the first use, which clang's note names. Inserts a line, so it runs
    alone in its round (the caller re-diagnoses)."""
    for k, d in enumerate(diags):
        if d['kind'] != 'error' or not same_file(d['file'], src) or \
                not re.match(r"explicit specialization of '\w+' after instantiation", d['msg']):
            continue
        note = next((n for n in diags[k + 1:k + 4] if n['kind'] == 'note' and 'first required here' in n['msg']
                     and same_file(n['file'], src)), None)
        defn = lines[d['line'] - 1].strip()
        m = re.match(r'^(template\s*<\s*>\s*)?(.+?\))\s*(const)?\s*(\{.*)?$', defn)
        if note is None or not m:
            continue
        decl = 'template <> %s%s;  // [overlay specdecl]' % (m.group(2), ' const' if m.group(3) else '')
        j = declaration_start(lines, note['line'] - 1)
        lines[j] = decl + '\n' + lines[j]
        if not m.group(1):
            lines[d['line'] - 1] = 'template <> ' + lines[d['line'] - 1].lstrip()
        record.append(dict(rule='specdecl', line=d['line'], at=j + 1))
        return True
    return False


def incomplete_if_not_exists(lines, diags, src, record):
    """`__if_not_exists(X::~X)` on a class that is only forward-declared means "not defined yet" (VC2008: true);
    clang rejects the incomplete type. Test a name that is never declared, so the block is kept."""
    changed = False
    for d in diags:
        m = re.match(r"incomplete type '(\w+)' named in nested name specifier", d['msg'])
        if d['kind'] != 'error' or not m or not same_file(d['file'], src):
            continue
        i, x = d['line'] - 1, m.group(1)
        new = re.sub(r'__if_not_exists\s*\(\s*%s::~%s\s*\)' % (x, x), '__if_not_exists(__fnv_never_declared_%s)' % x,
                     lines[i])
        if new != lines[i]:
            lines[i] = new
            record.append(dict(rule='ifexists-incomplete', line=d['line']))
            changed = True
    return changed


def rename_namespaced_specializations(lines, diags, src, record):
    """`template <> class T<ARGS> {...}` inside a namespace specialises a global template (VC2008 allowed it; C++
    requires global scope, and the body uses names of the enclosing namespace). Turn it into a plain class
    T__fnvspecK: the declaration, its constructors/destructor inside the body, and every `T<ARGS>` from the
    declaration to the end of the enclosing namespace. Functions whose mangling changes bind through the @fnv map."""
    changed = False
    depth = namespace_depths(lines)
    for d in diags:
        m = re.match(r"class template specialization of '(\w+)' must occur at global scope", d['msg'])
        if d['kind'] != 'error' or not m or not same_file(d['file'], src):
            continue
        i, t = d['line'] - 1, m.group(1)
        decl = re.match(r'^(\s*)template\s*<\s*>\s*(class|struct)\s+%s\s*<(.*)>(.*)$' % t, lines[i])
        if not decl or not depth[i]:
            continue
        args = decl.group(3)
        k = sum(1 for c in record if c['rule'] == 'nsspec') + 1
        new = '%s__fnvspec%d' % (t, k)
        lines[i] = '%s%s %s%s' % (decl.group(1), decl.group(2), new, decl.group(4))
        # body extent
        level, j, opened = 0, i, False
        while j < len(lines):
            code = re.sub(r'//.*', '', lines[j])
            level += code.count('{') - code.count('}')
            opened |= '{' in code
            if opened and level <= 0:
                break
            j += 1
        for b in range(i + 1, min(j + 1, len(lines))):
            lines[b] = re.sub(r'(~?)\b%s\s*\(' % t, lambda r: r.group(1) + new + '(', lines[b])
        # uses across the whole enclosing namespace (VC2008 used the specialisation even where it came after a
        # use); a forward declaration on the namespace's opening line keeps earlier by-value parameters valid
        end = scope_end(lines, i + 1)
        start = i
        while start > 0 and not (depth[start - 1] == depth[i] - 1 and re.search(r'\bnamespace\b', lines[start - 1])):
            start -= 1
        use = re.compile(r'\b%s\s*<\s*%s\s*>' % (t, r'\s*'.join(re.escape(x) for x in re.split(r'\s+', args.strip()))))
        for b in range(start, end):
            lines[b] = use.sub(new, lines[b])
        if start > 0:
            lines[start - 1] += ' %s %s;' % (decl.group(2), new)
        record.append(dict(rule='nsspec', name=t, args=args.strip(), line=d['line'], new=new))
        changed = True
    return changed


def drop_sdk_redeclarations(lines, diags, src, record):
    """The unit redeclares a Windows SDK function with its own types; clang reports the clash inside the SDK header,
    with a note pointing at the unit's declaration. Comment out that one-line declaration so the SDK's is used."""
    changed = False
    for k, d in enumerate(diags):
        if d['kind'] != 'error' or same_file(d['file'], src) or not d['msg'].startswith('conflicting types for'):
            continue
        for n in diags[k + 1:k + 4]:
            if n['kind'] == 'note' and 'previous declaration' in n['msg'] and same_file(n['file'], src):
                i = n['line'] - 1
                if lines[i].rstrip().endswith(';') and '{' not in lines[i] and not lines[i].lstrip().startswith('//'):
                    lines[i] = '// [overlay sdkdup] ' + lines[i]
                    record.append(dict(rule='sdkdup', line=n['line']))
                    changed = True
                break
    return changed


def spec_conversions(lines, diags, src, record):
    """A renamed namespaced specialisation (T__fnvspecK) meets code declared outside the namespace that still uses
    the global T<ARGS>. Both are the same object layout (the original was one type): reinterpret the operand."""
    edits = {}
    for d in diags:
        m = re.match(r"no viable conversion from '([^']+)' to '([^']+)'", d['msg'])
        if d['kind'] != 'error' or not m or not same_file(d['file'], src) or not d['ranges']:
            continue
        if '__fnvspec' not in m.group(1) and '__fnvspec' not in m.group(2):
            continue
        target = re.sub(r"\s*\(aka.*$", '', m.group(2))
        l1, c1, l2, c2 = d['ranges'][0]
        if l1 == l2:
            edits.setdefault(l1, set()).add((c1, c2, target))
    for ln, spans in edits.items():
        line = lines[ln - 1]
        for c1, c2, target in sorted(spans, reverse=True):
            line = line[:c1 - 1] + '(*(%s*)&(' % target + line[c1 - 1:c2 - 1] + '))' + line[c2 - 1:]
        lines[ln - 1] = line
        record.append(dict(rule='specconv', line=ln))
    return bool(edits)


def incomplete_dynamic_casts(lines, diags, src, record):
    """dynamic_cast<T*>(e) where T is only forward-declared here: cast by decorated name instead
    (fnv_dyncast, port/compat/fnv_rtti.cpp). Global-namespace class names only."""
    changed = False
    for d in diags:
        m = re.match(r"'(\w+)' is an incomplete type", d['msg'])
        if d['kind'] != 'error' or not m or not same_file(d['file'], src):
            continue
        i, t = d['line'] - 1, m.group(1)
        new = re.sub(r'\bdynamic_cast\s*<\s*%s\s*\*\s*>\s*\(' % t, 'fnv_dyncast<%s>(".?AV%s@@", ' % (t, t), lines[i])
        if new != lines[i]:
            lines[i] = new
            record.append(dict(rule='dyncast-name', line=d['line'], type=t))
            changed = True
    return changed


def small_msvc_leniencies(lines, diags, src, record):
    """One-line VC2008 leniencies:
       * `T* x = fnv_dyncast<D>(...)` where D is incomplete (so not known to derive from T): cast through void*;
       * `class ::X;` (a qualified forward declaration): dropped, the global X is declared elsewhere;
       * a string literal passed to a `void *` parameter: cast the literals on that line."""
    changed = False
    for k, d in enumerate(diags):
        if d['kind'] != 'error' or not same_file(d['file'], src):
            continue
        i = d['line'] - 1
        line = lines[i]
        m = re.match(r"cannot initialize a variable of type '([^']+)' with an rvalue of type '[^']+'", d['msg'])
        if m and 'fnv_dyncast<' in line:
            new = line.replace('= fnv_dyncast<', '= (%s)(void*)fnv_dyncast<' % m.group(1), 1)
        elif 'forward declaration of class cannot have a nested name specifier' in d['msg'] and \
                re.match(r'^\s*(class|struct)\s+::\w+\s*;\s*$', line):
            new = '// [overlay fwd] ' + line
        elif d['msg'].startswith('no matching function for call to') and any(
                n['kind'] == 'note' and re.search(r"no known conversion from 'const char\[\d+\]' to 'void \*'", n['msg'])
                for n in diags[k + 1:k + 6]):
            new = re.sub(r'(?<![\w)])("(?:[^"\\]|\\.)*")', r'(void*)\1', line)
        else:
            continue
        if new != line:
            lines[i] = new
            record.append(dict(rule='leniency', line=d['line']))
            changed = True
    return changed


def sdk_conflict_renames(lines, diags, src, record):
    """The unit declares a Windows API function itself (simplified types) at file scope and includes <windows.h>
    only later; clang reports the clash inside the SDK header, with a note at the unit's declaration. Rename the
    function from that declaration up to the next system #include to NAME__fnvdupK: uses before the include keep
    the unit's types, later ones the SDK's, and the relink strips the marker (__imp__NAME@N binds the IAT slot)."""
    changed = False
    for k, d in enumerate(diags):
        m = re.match(r"conflicting types for '(\w+)'", d['msg'])
        if d['kind'] != 'error' or not m or same_file(d['file'], src):
            continue
        note = next((n for n in diags[k + 1:k + 4] if n['kind'] == 'note' and 'previous declaration' in n['msg']
                     and same_file(n['file'], src)), None)
        if note is None:
            continue
        name, start = m.group(1), note['line']
        # an SDK include nested in a namespace (seen in the decomp) declares into that namespace, so it does not
        # end the region; only one at file scope does
        depth = namespace_depths(lines)
        end = next((j for j in range(start, len(lines)) if re.match(r'\s*#\s*include\s*<', lines[j]) and not depth[j]),
                   len(lines))
        kth = sum(1 for c in record if c['rule'] == 'sdkrename') + 1
        pat = re.compile(r'(?<![\w$])%s(?![\w$])' % re.escape(name))
        for j in range(start - 1, end):
            if not lines[j].lstrip().startswith('#'):
                lines[j] = pat.sub('%s__fnvdup%d' % (name, 90 + kth), lines[j])
        record.append(dict(rule='sdkrename', name=name, lines=[start, end]))
        changed = True
    return changed


def fix_round(lines, diags, src, record):
    # rules that insert lines put a newline inside one entry; re-split so list indices match the file's line numbers
    lines[:] = '\n'.join(lines).split('\n')
    if drop_extern_c(lines, diags, src, record):
        return True                        # re-diagnose before any rename rule touches these names
    if specialization_before_use(lines, diags, src, record):
        return True                        # it inserted a line: re-diagnose before line-based rules run
    if inherited_member_specializations(lines, diags, src, record):
        return True                        # it inserted a line too
    changed = apply_fixits(lines, diags, src, record)
    changed |= const_casts(lines, diags, src, record)
    changed |= lvalue_wraps(lines, diags, src, record)
    changed |= literal_param_casts(lines, diags, src, record)
    changed |= interlocked_addresses(lines, diags, src, record)
    changed |= exception_spec_and_delete(lines, diags, src, record)
    changed |= explicit_constructor_calls(lines, diags, src, record)
    changed |= incomplete_if_not_exists(lines, diags, src, record)
    changed |= rename_namespaced_specializations(lines, diags, src, record)
    changed |= spec_conversions(lines, diags, src, record)
    changed |= incomplete_dynamic_casts(lines, diags, src, record)
    changed |= small_msvc_leniencies(lines, diags, src, record)
    changed |= sdk_conflict_renames(lines, diags, src, record)
    ours = [d for d in diags if same_file(d['file'], src)]
    # qualify: N::X where N doesn't declare X but the global scope does (VC2008 resolved these)
    for d in ours:
        m = re.match(r"no member named '(\w+)' in namespace '[\w:]+'; did you mean simply '\1'\?", d['msg'])
        if m and d['kind'] == 'error':
            i = d['line'] - 1
            new = re.sub(r'(?<![\w:])(?:\w+::)+' + re.escape(m.group(1)) + r'\b', m.group(1), lines[i])
            if new != lines[i]:
                lines[i] = new
                record.append(dict(rule='qualify', name=m.group(1), line=d['line']))
                changed = True
    # nsnew
    for d in ours:
        if d['kind'] == 'error' and re.search(r"'operator (new|delete)(\[\])?' cannot be declared inside a namespace",
                                              d['msg']):
            if not lines[d['line'] - 1].lstrip().startswith('// [overlay'):
                n = remove_decl(lines, d['line'])
                record.append(dict(rule='nsnew', line=d['line'], lines=n))
                changed = True
    # dup: group by name, rename from each conflicting declaration onwards
    groups = {}
    for d in ours:
        if d['kind'] != 'error':
            continue
        m = re.match(r"(?:conflicting types for|redeclaration of|declaration of|redefinition of) '([A-Za-z_]\w*)'",
                     d['msg'])
        if m and ('conflict' in d['msg'] or 'different' in d['msg'] or 'redefinition' in d['msg']
                  or 'linkage' in d['msg']):
            groups.setdefault(m.group(1), []).append(d['line'])
    for name, starts in groups.items():
        if name.startswith('operator') or name in ('__m128', '__m128i', '__m128d'):
            continue
        rename_from(lines, name, starts)
        record.append(dict(rule='dup', name=name, lines=sorted(set(starts))))
        changed = True
    return changed


def obj_path(index, unit):
    """Same naming as compile_census.py, so out/obj holds one object per unit whichever tool built it."""
    return str(ROOT / 'out/obj' / ('%04d_%s.obj' % (index, Path(unit['source']).stem)))


def overlay_path(index, unit):
    """One folder per unit: a few sources are built by more than one unit (with different flags)."""
    return OUT / ('%04d' % index) / Path(unit['source']).name


def process(index, unit, rounds):
    orig = DECOMP / unit['source']
    text = orig.read_text(errors='replace')
    # 'types': the global MSVC unions come from fnv_msvc_xmm.h (the unit's own standard copies are unified onto
    # them); 'own': the unit keeps a nonstandard definition of its own and only clang's headers are blocked
    sse = None
    unified = 0
    if SSE_DEF.search(text):
        text, unified, all_ok = unify_simd_types(text)
        # nonstandard copies that remain are fine inside a namespace; only one at file scope blocks the global types
        depth = namespace_depths(text.splitlines())
        global_own = any(not depth[text.count('\n', 0, m.start())] for m in OWN_M128.finditer(text))
        sse = 'own' if global_own else 'types'
    out = overlay_path(index, unit)
    out.parent.mkdir(parents=True, exist_ok=True)
    record = [dict(rule='sse', mode=sse, unified=unified)] if sse else []
    # __if_not_exists(T::T) ("class T is not declared yet") errors in clang when T is undeclared; test T itself
    text, n = re.subn(r'__if_not_exists\s*\(\s*(::)?(\w+)::\2\s*\)', r'__if_not_exists(\1\2)', text)
    if n:
        record.append(dict(rule='ifexists', count=n))
    lines = text.splitlines()
    modern = False
    first_errors = None
    for r in range(rounds):
        out.write_text('\n'.join(lines) + '\n')
        ok, diags = compile_unit(unit, out, sse, obj_path(index, unit), modern)
        errs = [d for d in diags if d['kind'] == 'error']
        if first_errors is None:
            first_errors = len(errs)
        if ok:
            break
        changed = False
        if not modern and any(STL_HEADERS.search(Path(d['file']).name and d['file']) or 'char16_t' in d['msg']
                              for d in errs):
            modern = True
            record.append(dict(rule='modern'))
            changed = True
        if not fix_round(lines, diags, out, record) and not changed:
            break
    (out.parent / (out.name + '.overlay.json')).write_text(json.dumps(dict(source=unit['source'], ok=ok,
                                                                          rounds=r + 1, modern=modern,
                                                                          changes=record), indent=1))
    remaining = Counter(re.sub(r"'[^']*'", "'X'", d['msg'])[:100] for d in diags if d['kind'] == 'error')
    return unit['source'], ok, record, remaining


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--only', default='')
    ap.add_argument('--rounds', type=int, default=6)
    ap.add_argument('--jobs', type=int, default=os.cpu_count())
    ap.add_argument('--failed', default='out/compile_census.json')
    a = ap.parse_args()
    units = json.loads((DECOMP / 'build/units.json').read_text())['units']
    failed = set(json.loads((ROOT / a.failed).read_text())['failed_units'])
    todo = [(i, u) for i, u in enumerate(units) if u['source'] in failed and a.only in u['source']]
    with ThreadPoolExecutor(a.jobs) as ex:
        results = list(ex.map(lambda iu: process(iu[0], iu[1], a.rounds), todo))
    fixed = sum(1 for _, ok, _, _ in results if ok)
    rules = Counter(c['rule'] for _, ok, rec, _ in results if ok for c in rec)
    rest = Counter()
    first = Counter()
    for _, ok, _, rem in results:
        if not ok:
            rest.update(rem)
            if rem:
                first[next(iter(rem))] += 1
    report = dict(attempted=len(results), now_compile=fixed, still_failing=len(results) - fixed,
                  rules_used_in_fixed_units=dict(rules), remaining_errors=rest.most_common(30),
                  still_failing_units=[s for s, ok, _, _ in results if not ok])
    (ROOT / 'out/overlay_report.json').write_text(json.dumps(report, indent=1))
    print(json.dumps({k: v for k, v in report.items() if k != 'still_failing_units'}, indent=1))


if __name__ == '__main__':
    main()
