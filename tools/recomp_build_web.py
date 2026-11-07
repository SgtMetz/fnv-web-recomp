#!/usr/bin/env python3
"""Build the Emscripten version of the whole-game recompiler (milestone 2, docs/RECOMP.md): out/recomp/recomp_NN.cpp
(tools/recomp.py) and the portable runtime + PAL + web host, linked to out/recomp/web/fnv_web.js (+ .wasm). Node runs it
with direct access to the host filesystem (NODERAWFS). A shard whose source and headers are unchanged keeps its object.

usage: python tools/recomp_build_web.py [--jobs N]
"""
import argparse
import hashlib
import os
import subprocess
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / 'out/recomp'
EMSDK = ROOT / 'local/dl/emsdk'
EMXX = [os.environ.get('PYTHON', 'python'), str(EMSDK / 'upstream/emscripten/em++.py')]
ENV = dict(os.environ, EM_CONFIG=str(EMSDK / '.emscripten'))
HEADERS = [ROOT / 'port/recomp/fnv_recomp.h', ROOT / 'port/recomp/fnv_recomp_host.h', ROOT / 'port/runtime/fnv_lift_ops.h',
           ROOT / 'port/recomp/pal.h', ROOT / 'port/recomp/pal_com.h', OUT / 'import_table.inc', OUT / 'com_tables.inc',
           ROOT / 'port/recomp/pal_d3d9.h', OUT / 'd3d9_caps.inc']
PORT = ['fnv_recomp_rt.cpp', 'fnv_intrinsics.cpp', 'pal_kernel.cpp', 'pal_com.cpp', 'pal_user.cpp', 'pal_input.cpp',
        'pal_dsound.cpp', 'pal_d3d9.cpp', 'pal_d3dx.cpp', 'pal_dshow.cpp', 'host_web.cpp']
CFLAGS = [os.environ.get('FNV_WEB_OPT', '-O2'), '-fno-strict-aliasing', '-fwrapv', '-fno-delete-null-pointer-checks', '-fno-strict-float-cast-overflow', '-c', '-w', '-pthread', '-std=c++17', '-I' + str(ROOT / 'port/recomp')] +     (['-DFNV_CALL_TRACE'] if os.environ.get('FNV_CALL_TRACE') else [])
LDFLAGS = ['-O1', '-pthread', '-sGLOBAL_BASE=22020096', '-sINITIAL_MEMORY=3221225472', '-sMAXIMUM_MEMORY=3221225472', '-sSTACK_SIZE=64MB',
           '-sDEFAULT_PTHREAD_STACK_SIZE=4MB', '-sPTHREAD_POOL_SIZE=48', '-sNODERAWFS=1', '-sENVIRONMENT=node,worker',
           '-sEXIT_RUNTIME=1', '-sSUPPORT_LONGJMP=emscripten', '-sMALLOC=' + os.environ.get('FNV_WEB_MALLOC', 'dlmalloc'), '-sWASM_BIGINT=1',
           '-sERROR_ON_UNDEFINED_SYMBOLS=1', '--profiling-funcs',
           '--pre-js', str(ROOT / 'port/recomp/web_pre.js')]


def compile_one(args):
    src, obj, key = args
    stamp = Path(str(obj) + '.key')
    if obj.exists() and stamp.exists() and stamp.read_text() == key:
        return src, True, ''
    r = subprocess.run(EMXX + CFLAGS + [str(src), '-o', str(obj)], capture_output=True, text=True, errors='replace', env=ENV)
    if r.returncode:
        return src, False, (r.stdout + r.stderr)[-3000:]
    stamp.write_text(key)
    return src, True, ''


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--jobs', type=int, default=os.cpu_count())
    a = ap.parse_args()
    t0 = time.time()
    hdr = hashlib.sha1(b''.join(h.read_bytes() for h in HEADERS) + ' '.join(CFLAGS).encode()).hexdigest()
    srcs = [ROOT / 'port/recomp' / p for p in PORT] + sorted(OUT.glob('recomp_*.cpp'))
    objdir = OUT / 'web_obj'
    objdir.mkdir(parents=True, exist_ok=True)
    jobs = [(s, objdir / (s.stem + '.o'), hashlib.sha1(s.read_bytes()).hexdigest() + hdr) for s in srcs]
    failed = []
    with ThreadPoolExecutor(a.jobs) as ex:
        for src, ok, err in ex.map(compile_one, jobs):
            if not ok:
                failed.append(src)
                print('compile failed: %s\n%s' % (src, err))
    if failed:
        raise SystemExit('%d sources failed' % len(failed))
    print('compiled %d sources in %.0f s' % (len(jobs), time.time() - t0), flush=True)
    web = OUT / 'web'
    web.mkdir(exist_ok=True)
    out = web / 'fnv_web.js'
    r = subprocess.run(EMXX + LDFLAGS + [str(j[1]) for j in jobs] + ['-o', str(out)], capture_output=True, text=True,
                       errors='replace', env=ENV)
    if r.returncode:
        # the command line itself is huge: show the diagnostics, not the echo of the command
        lines = [l for l in (r.stdout + r.stderr).splitlines() if 'web_obj' not in l or l.startswith('wasm-ld')]
        print(chr(10).join(l[:300] for l in lines[:40]))
        raise SystemExit('link failed')
    wasm = web / 'fnv_web.wasm'
    print('linked %s (%.1f MB wasm) in %.0f s total' % (out, wasm.stat().st_size / 1e6, time.time() - t0))


if __name__ == '__main__':
    main()
