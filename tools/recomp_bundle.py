#!/usr/bin/env python3
"""Bundle the browser build for a server (server/README.md): everything server/serve.mjs --dist serves, in one folder.

    out/dist/
      index.html, credits.html, renderer.js, ...              the page (web/recomp/)
      build/fnv_web.js, build/fnv_web.wasm                     the Emscripten build (out/recomp/web_browser/)
      shader/<sha1>                                            each translated shader as the page fetches it (JSON)
      VERSION.json                                             what went in: commit, build time, wasm SHA-256
    plus .gz (and .br, via node) siblings of the larger files, which the server sends when the browser accepts them.

The bundle is derived from the player's copy of the game (the wasm is the recompiled exe, the shaders its translated
shaders), so it lives under out/ and is never committed. The game files themselves are not in it: the server reads
them from --game.

usage: python tools/recomp_bundle.py [--out out/dist]
"""
import argparse
import gzip
import hashlib
import json
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PAGE = ['index.html', 'credits.html', 'renderer.js', 'audio-worklet.js', 'debug.js', 'report.js', 'precheck.js', 'chat.js', 'tuner.js', 'favicon.ico', 'icon-192.png', 'apple-touch-icon.png']
BUILD = ['fnv_web.js', 'fnv_web.wasm']
BROTLI = r"""
const fs = require('fs'), zlib = require('zlib');
for (const f of process.argv.slice(1)) {
  // the strongest setting with a 16 MB window: the wasm is 13.6 MB instead of 17.2 MB at quality 9 (about 4 minutes,
  // once per bundle, against every player's first download)
  const out = zlib.brotliCompressSync(fs.readFileSync(f), { params: { [zlib.constants.BROTLI_PARAM_QUALITY]: 11,
    [zlib.constants.BROTLI_PARAM_LGWIN]: 24, [zlib.constants.BROTLI_PARAM_SIZE_HINT]: fs.statSync(f).size } });
  fs.writeFileSync(f + '.br', out);
}
"""


def gz(path: Path):
    with open(path, 'rb') as src, open(str(path) + '.gz', 'wb') as raw, \
            gzip.GzipFile(fileobj=raw, mode='wb', compresslevel=9, mtime=0) as dst:
        shutil.copyfileobj(src, dst)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', default=str(ROOT / 'out/dist'))
    ap.add_argument('--build-dir', type=Path, default=ROOT / 'out/recomp/web_browser')
    a = ap.parse_args()
    out = Path(a.out).resolve()
    t0 = time.time()
    build = a.build_dir.resolve()
    if build == out or out in build.parents:
        ap.error('--out cannot contain the build directory')
    for f in BUILD:
        if not (build / f).exists():
            sys.exit(f'missing {build / f}: build it with python -B tools/recomp_build_web.py --target browser')
    if out.exists():
        shutil.rmtree(out)
    (out / 'build').mkdir(parents=True)
    (out / 'shader').mkdir()
    for f in PAGE:
        shutil.copy2(ROOT / 'web/recomp' / f, out / f)
    # the live page: no log or debug status (web/recomp/index.html, html.dev); ?debug in the address shows them
    page = out / 'index.html'
    text = page.read_text(encoding='utf-8')
    if '/*FNV_DEV*/true' not in text:
        sys.exit('web/recomp/index.html has no /*FNV_DEV*/true switch')
    page.write_text(text.replace('/*FNV_DEV*/true', '/*FNV_DEV*/false'), encoding='utf-8', newline='')
    for f in BUILD:
        shutil.copy2(build / f, out / 'build' / f)

    # shaders: the same JSON the development server makes on request (server/serve.mjs)
    seen = set()
    for mf in sorted((ROOT / 'out/shader_manifest').glob('*.json')):
        pkg = mf.stem
        for name, info in json.loads(mf.read_text(encoding='utf-8')).items():
            sha = info['sha1']
            if sha in seen:
                continue
            seen.add(sha)
            wgsl = (ROOT / 'out/shaders' / pkg / info['wgsl']).read_text(encoding='utf-8')
            dst = out / 'shader' / sha
            dst.write_text(json.dumps({'name': pkg + '/' + name, 'info': info, 'wgsl': wgsl}), encoding='utf-8')
            if dst.stat().st_size > 1024:
                gz(dst)

    for f in [out / p for p in PAGE] + [out / 'build' / f for f in BUILD]:
        gz(f)
    big = [str(out / 'build' / f) for f in BUILD] + [str(out / p) for p in PAGE]
    if shutil.which('node'):
        subprocess.run(['node', '-e', BROTLI] + big, check=True)
    else:
        print('node not found: no .br files (gzip only)')

    commit = subprocess.run(['git', 'rev-parse', '--short', 'HEAD'], cwd=ROOT, capture_output=True, text=True).stdout.strip()
    dirty = subprocess.run(['git', 'status', '--porcelain', '--', 'port', 'web/recomp', 'server', 'tools'], cwd=ROOT,
                           capture_output=True, text=True).stdout.strip()
    wasm = out / 'build/fnv_web.wasm'
    version = {
        'commit': commit + ('+local changes' if dirty else ''),
        'built': time.strftime('%Y-%m-%dT%H:%M:%S'),
        'wasm_sha256': hashlib.sha256(wasm.read_bytes()).hexdigest(),
        'wasm_bytes': wasm.stat().st_size,
        # what the page counts as downloaded when the server compresses the wasm (web/recomp/index.html)
        'wasm_br_bytes': Path(str(wasm) + '.br').stat().st_size if Path(str(wasm) + '.br').exists() else None,
        'wasm_gz_bytes': Path(str(wasm) + '.gz').stat().st_size,
        'shaders': len(seen),
    }
    (out / 'VERSION.json').write_text(json.dumps(version, indent=2) + '\n', encoding='utf-8')
    size = lambda p: p.stat().st_size / 1e6
    br = Path(str(wasm) + '.br')
    print(f'bundle {out}: {len(seen)} shaders, wasm {size(wasm):.1f} MB (gzip {size(Path(str(wasm) + ".gz")):.1f} MB'
          + (f', brotli {size(br):.1f} MB' if br.exists() else '') + f') in {time.time() - t0:.0f} s')


if __name__ == '__main__':
    main()
