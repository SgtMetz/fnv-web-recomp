#!/usr/bin/env python3
"""Phase 1, step 1: compile every decomp unit with clang-cl (i686 MSVC ABI) and bucket the failures.

Each unit keeps its recipe flags (/O2, /Od /Oy-, /EHsc, /GS, /Gy, /GF, /Ob1 ...). clang-cl is told to behave like
VC2008 (-fms-compatibility-version=15.00) and uses the installed VS 2022 CRT + Windows SDK headers. Objects go to
out/obj/ (git-ignored). Sources in the decomp are never modified; port-side fixes go in a forced-include header
(port/compat/fnv_clang_compat.h) when one exists.

usage: python tools/compile_census.py [--decomp DIR] [--jobs N] [--only TEXT] [--limit N]
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

ROOT = Path(__file__).resolve().parent.parent
CLANG_CL = r'C:\Program Files\LLVM\bin\clang-cl.exe'
VS = Path(r'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC')
KITS = Path(r'C:\Program Files (x86)\Windows Kits\10\Include')
COMPAT = ROOT / 'port/compat/fnv_clang_compat.h'


def includes():
    msvc = sorted(VS.iterdir())[-1] / 'include'
    sdk = sorted(KITS.iterdir())[-1]
    return [msvc] + [sdk / p for p in ('ucrt', 'shared', 'um')]


def command(unit, decomp, obj):
    flags = [f for f in unit['flags'] if f not in ('/MT',)]
    # FNV_MSVER / FNV_STD override the language mode (measured: 19.44 + C++17 changes almost nothing)
    cmd = [CLANG_CL, '--target=i686-pc-windows-msvc', '/c', '/nologo', '/MT',
           os.environ.get('FNV_MSVER', '-fms-compatibility-version=15.00'),
           '-w', '-Wno-everything', '-ferror-limit=0', '/Zc:dllexportInlines-',
           '/clang:-fcheck-new'] + flags    # VC2008 kept the null check after `new (p) T`; clang drops it for placement new
    if os.environ.get('FNV_STD'):       # clang-cl's default for 15.00 compiles more units than an explicit /std:c++14
        cmd.append(os.environ['FNV_STD'])
    for inc in includes():
        cmd.append('/imsvc' + str(inc))
    cmd.append('/I' + str(Path(decomp) / 'include'))
    if COMPAT.exists():
        cmd.append('/FI' + str(COMPAT))
    return cmd + ['/Fo' + str(obj), str(Path(decomp) / unit['source'])]


def bucket(err):
    lines = [l for l in err.splitlines() if ': error:' in l or 'fatal error' in l]
    if not lines:
        return ['(no error line) ' + err.strip()[:100]]
    out = []
    for l in lines:
        m = re.sub(r"^.*?error: ", '', l)
        m = re.sub(r"'[^']*'", "'X'", m)
        m = re.sub(r'\b\d+\b', 'N', m)
        out.append(m[:110])
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--decomp', default=r'E:\Games\fnv_decomp-main')
    ap.add_argument('--jobs', type=int, default=os.cpu_count())
    ap.add_argument('--only', default='')
    ap.add_argument('--limit', type=int, default=0)
    ap.add_argument('--out', default='out/compile_census.json')
    a = ap.parse_args()
    units = json.loads((Path(a.decomp) / 'build/units.json').read_text())['units']
    units = [u for u in units if a.only in u['source']]
    if a.limit:
        units = units[:a.limit]
    objdir = ROOT / 'out/obj'
    objdir.mkdir(parents=True, exist_ok=True)

    def one(i_u):
        i, u = i_u
        obj = objdir / ('%04d_%s.obj' % (i, Path(u['source']).stem))
        r = subprocess.run(command(u, a.decomp, obj), capture_output=True, text=True, errors='replace')
        ok = r.returncode == 0 and obj.exists()
        return u, ok, [] if ok else bucket(r.stderr + r.stdout)

    results = []
    with ThreadPoolExecutor(a.jobs) as ex:
        for n, res in enumerate(ex.map(one, enumerate(units))):
            results.append(res)
            if n % 250 == 0:
                print('%d/%d' % (n, len(units)), file=sys.stderr, flush=True)
    ok = sum(1 for _, k, _ in results if k)
    reasons = Counter(m for _, k, errs in results if not k for m in errs)
    first = Counter(errs[0] for _, k, errs in results if not k)
    fn_ok = sum(len(u['functions']) for u, k, _ in results if k)
    report = dict(units=len(results), compiled=ok, failed=len(results) - ok,
                  functions_in_compiled_units=fn_ok, functions_total=sum(len(u['functions']) for u, _, _ in results),
                  top_first_errors=first.most_common(25), top_errors=reasons.most_common(25),
                  failed_units=[u['source'] for u, k, _ in results if not k])
    Path(a.out).write_text(json.dumps(report, indent=1))
    print(json.dumps({k: v for k, v in report.items() if k != 'failed_units'}, indent=1))


if __name__ == '__main__':
    main()
