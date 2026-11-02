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
DIAG = re.compile(r'^(?P<file>.*?)\((?P<line>\d+),(?P<col>\d+)\) ?: (?P<kind>error|note): (?P<msg>.*)$')
DUP_MSGS = ("conflicting types for '", "redeclaration of '", "declaration of '",
            "redefinition of '")
SSE_DEF = re.compile(r'union\s+(__declspec\([^)]*\)\s*|_CRT_ALIGN\(\d+\)\s*)*__m128\b|\bm128_(f32|u32|i32|u64)\b'
                     r'|\b_mm_\w+\s*\(')


def compile_unit(unit, src, sse):
    cmd = cc.command(unit, str(DECOMP), 'NUL')
    cmd[-1] = str(src)
    extra = ['/I' + str((DECOMP / unit['source']).parent), '/I' + str(SSE_BLOCK.parent)]
    if sse:
        extra.append('/FI' + str(SSE_BLOCK))
    if sse == 'types':
        extra.append('/FI' + str(MSVC_XMM))
    cmd[1:1] = extra
    r = subprocess.run(cmd, capture_output=True, text=True, errors='replace')
    diags = []
    for l in r.stderr.splitlines():
        m = DIAG.match(l)
        if m:
            diags.append(dict(m.groupdict(), line=int(m['line']), col=int(m['col'])))
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


def fix_round(lines, diags, src, record):
    changed = False
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
    lines = text.splitlines()
    record = [dict(rule='sse', mode=sse, unified=unified)] if sse else []
    first_errors = None
    for r in range(rounds):
        out.write_text('\n'.join(lines) + '\n')
        ok, diags = compile_unit(unit, out, sse)
        errs = [d for d in diags if d['kind'] == 'error']
        if first_errors is None:
            first_errors = len(errs)
        if ok:
            break
        if not fix_round(lines, diags, out, record):
            break
    (out.parent / (out.name + '.overlay.json')).write_text(json.dumps(dict(source=unit['source'], ok=ok,
                                                                          rounds=r + 1, changes=record), indent=1))
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
