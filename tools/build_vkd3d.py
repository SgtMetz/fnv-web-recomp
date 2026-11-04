#!/usr/bin/env python3
"""Apply tools/vkd3d_patches/*.patch to local/dl/vkd3d/vkd3d-1.19 and rebuild vkd3d-compiler.exe with zig cc.

The HLSL/preproc front end is stubbed (no bison/flex): d3dbc input does not use it.
usage: python tools/build_vkd3d.py [--reset]   (--reset re-extracts the pristine tarball first)
"""
import argparse, shutil, subprocess, sys, tarfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BASE = ROOT / 'local/dl/vkd3d'
SRC = BASE / 'vkd3d-1.19'
BUILD = BASE / 'build'
ZIG = ROOT / 'local/venv/Lib/site-packages/ziglang/zig.exe'
PATCHES = sorted((ROOT / 'tools/vkd3d_patches').glob('*.patch'))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--reset', action='store_true')
    a = ap.parse_args()
    if a.reset:
        shutil.rmtree(SRC, ignore_errors=True)
        with tarfile.open(BASE / 'vkd3d-1.19.tar.xz') as t:
            t.extractall(BASE)
        for p in PATCHES:
            print('patch', p.name)
            subprocess.run(['git', 'apply', '--whitespace=nowarn', '--directory=' + SRC.relative_to(ROOT).as_posix(), str(p)], cwd=ROOT, check=True)
    elif PATCHES:
        print('(not resetting: patches assumed applied; use --reset to re-apply)')
    sh = SRC / 'libs/vkd3d-shader'
    srcs = [sh / f for f in ('checksum.c', 'd3d_asm.c', 'd3dbc.c', 'dxbc.c', 'dxil.c', 'fx.c', 'glsl.c', 'hlsl.c',
                             'hlsl_codegen.c', 'hlsl_constant_ops.c', 'ir.c', 'msl.c', 'spirv.c', 'tpf.c',
                             'vkd3d_shader_main.c')]
    srcs += sorted((SRC / 'libs/vkd3d-common').glob('*.c'))
    srcs += [BUILD / 'hlsl_stubs.c', SRC / 'programs/vkd3d-compiler/main.c']
    cmd = [str(ZIG), 'cc', '-O2', '-g', '-w', '-DHAVE_CONFIG_H', '-D_GNU_SOURCE', '-DWINVER=0x0601',
           '-I', str(BUILD), '-I', str(SRC / 'include'), '-I', str(SRC / 'include/private'), '-I', str(sh),
           '-I', str(BASE / 'spirvh'), '-target', 'x86_64-windows-gnu'] + [str(s) for s in srcs] + \
          ['-o', str(BUILD / 'vkd3d-compiler.exe')]
    sys.exit(subprocess.run(cmd).returncode)


if __name__ == '__main__':
    main()
