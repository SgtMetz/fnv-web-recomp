#!/usr/bin/env python3
"""Phase 1: extract the data image from your own FalloutNV.exe (this machine only; output is git-ignored).

Writes local/data_image.bin covering [first data section VA, end of the last one) with .rdata/.data/.tls/CONST at
their original offsets. Bytes past each section's raw size (.bss) stay zero. local/data_image.json records the base,
size and section spans. Rule 1: never commit, upload or share these files.

usage: python tools/data_image.py --exe <FalloutNV.exe>
"""
import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from relink import Image  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
SECTIONS = ('.rdata', '.data', '.tls', 'CONST')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', required=True)
    a = ap.parse_args()
    img = Image(a.exe)
    secs = [s for s in img.sections if s['name'] in SECTIONS]
    base = min(s['start'] for s in secs)
    end = max(s['start'] + max(s['vsize'], s['rsize']) for s in secs)
    end = (end + 0xfff) & ~0xfff
    out = bytearray(end - base)
    spans = []
    for s in secs:
        raw = img.data[s['raw']:s['raw'] + min(s['rsize'], max(s['vsize'], 1))]
        out[s['start'] - base:s['start'] - base + len(raw)] = raw
        spans.append(dict(name=s['name'], va=hex(s['start']), vsize=hex(s['vsize']), raw=hex(len(raw))))
    dst = ROOT / 'local'
    dst.mkdir(exist_ok=True)
    (dst / 'data_image.bin').write_bytes(bytes(out))
    meta = dict(base=hex(base), size=hex(len(out)), sections=spans)
    (dst / 'data_image.json').write_text(json.dumps(meta, indent=1))
    print(json.dumps(meta, indent=1))


if __name__ == '__main__':
    main()
