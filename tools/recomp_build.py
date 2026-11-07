#!/usr/bin/env python3
"""Build the native whole-game recompiler host (milestone 1, docs/RECOMP.md): compile out/recomp/recomp_NN.cpp
(tools/recomp.py) and port/recomp/*.cpp with clang-cl, link out/recomp/fnv_recomp.exe. A shard whose source and
headers are unchanged keeps its object.

usage: python tools/recomp_build.py [--jobs N] [--opt /O2]
"""
import argparse
import hashlib
import os
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import relink_plan as rp  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / 'out/recomp'
LLVM = Path(r'C:\Program Files\LLVM\bin')
HEADERS = [ROOT / 'port/recomp/fnv_recomp.h', ROOT / 'port/recomp/fnv_recomp_host.h', ROOT / 'port/runtime/fnv_lift_ops.h',
           ROOT / 'port/recomp/pal.h']


def includes():
    vs = sorted(rp.VS.iterdir())[-1]
    sdk = sorted(Path(r'C:\Program Files (x86)\Windows Kits\10\Include').iterdir())[-1]
    return ['/imsvc' + str(vs / 'include')] + ['/imsvc' + str(sdk / p) for p in ('ucrt', 'shared', 'um')]


def compile_one(args):
    src, obj, opt, key = args
    stamp = Path(str(obj) + '.key')
    if obj.exists() and stamp.exists() and stamp.read_text() == key:
        return src, True, ''
    cmd = [str(LLVM / 'clang-cl.exe'), '--target=i686-pc-windows-msvc', '/nologo', '/c', opt, '/MT', '/EHsc', '-w', '/std:c++17',
           '/Gy', '/I' + str(ROOT / 'port/recomp')] + includes() +           (['/DFNV_CALL_TRACE'] if os.environ.get('FNV_CALL_TRACE') else []) + ['/Fo' + str(obj), str(src)]
    r = subprocess.run(cmd, capture_output=True, text=True, errors='replace')
    if r.returncode:
        return src, False, (r.stdout + r.stderr)[-3000:]
    stamp.write_text(key)
    return src, True, ''


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--jobs', type=int, default=os.cpu_count())
    ap.add_argument('--opt', default='/O2')
    a = ap.parse_args()
    t0 = time.time()
    hdr = hashlib.sha1(b''.join(h.read_bytes() for h in HEADERS) + a.opt.encode() +
                       (b'trace' if os.environ.get('FNV_CALL_TRACE') else b'')).hexdigest()
    srcs = [ROOT / 'port/recomp/fnv_recomp_rt.cpp', ROOT / 'port/recomp/host_win32.cpp', ROOT / 'port/recomp/fnv_intrinsics.cpp', ROOT / 'port/recomp/pal_kernel.cpp'] + sorted(OUT.glob('recomp_*.cpp'))
    objdir = OUT / 'obj'
    objdir.mkdir(parents=True, exist_ok=True)
    jobs = [(s, objdir / (s.stem + '.obj'), a.opt, hashlib.sha1(s.read_bytes()).hexdigest() + hdr) for s in srcs]
    failed = []
    with ThreadPoolExecutor(a.jobs) as ex:
        for src, ok, err in ex.map(compile_one, jobs):
            if not ok:
                failed.append(src)
                print('compile failed: %s\n%s' % (src, err))
    if failed:
        raise SystemExit('%d sources failed' % len(failed))
    print('compiled %d sources in %.0f s' % (len(jobs), time.time() - t0), flush=True)
    exe = OUT / 'fnv_recomp.exe'
    cmd = [str(LLVM / 'lld-link.exe'), '/nologo', '/machine:x86', '/subsystem:console', '/base:0x30000000', '/fixed',
           '/dynamicbase:no', '/largeaddressaware', '/nxcompat', '/stack:0x40000', '/opt:ref',
           '/map:' + str(OUT / 'fnv_recomp.map'), '/out:' + str(exe)] + [str(j[1]) for j in jobs] + \
          ['libcmt.lib', 'kernel32.lib', 'user32.lib', 'shell32.lib', 'gdi32.lib', 'advapi32.lib']
    vs = sorted(rp.VS.iterdir())[-1]
    sdk = sorted(Path(r'C:\Program Files (x86)\Windows Kits\10\Lib').iterdir())[-1]
    cmd += ['/libpath:' + str(vs / 'lib/x86'), '/libpath:' + str(sdk / 'ucrt/x86'), '/libpath:' + str(sdk / 'um/x86')]
    r = subprocess.run(cmd, capture_output=True, text=True, errors='replace')
    print((r.stdout + r.stderr)[-3000:])
    if r.returncode:
        raise SystemExit('link failed')
    print('linked %s (%.1f MB) in %.0f s total' % (exe, exe.stat().st_size / 1e6, time.time() - t0))


if __name__ == '__main__':
    main()
