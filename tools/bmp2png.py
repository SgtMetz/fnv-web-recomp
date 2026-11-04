#!/usr/bin/env python3
"""Convert 24-bit BMPs (out/d3d_dump from FNV_D3D_DUMP) to PNG without PIL.

usage: python tools/bmp2png.py in.bmp [more.bmp ...] [--out DIR] [--scale K]
  --scale K multiplies the 8-bit values by K (clamped) to make dark HDR targets visible."""
import struct
import sys
import zlib
from pathlib import Path


def bmp2png(src, dst, scale=1.0):
    b = Path(src).read_bytes()
    off, = struct.unpack_from('<I', b, 10)
    w, h = struct.unpack_from('<ii', b, 18)
    bpp, = struct.unpack_from('<H', b, 28)
    assert bpp == 24, bpp
    rowb = (w * 3 + 3) & ~3
    raw = bytearray()
    for y in range(abs(h)):
        sy = abs(h) - 1 - y if h > 0 else y
        row = b[off + sy * rowb: off + sy * rowb + w * 3]
        raw.append(0)
        px = bytearray(w * 3)
        px[0::3], px[1::3], px[2::3] = row[2::3], row[1::3], row[0::3]
        if scale != 1.0:
            px = bytearray(min(255, int(v * scale)) for v in px)
        raw += px

    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, abs(h), 8, 2, 0, 0, 0)) + \
        chunk(b'IDAT', zlib.compress(bytes(raw), 6)) + chunk(b'IEND', b'')
    Path(dst).write_bytes(png)


if __name__ == '__main__':
    args = sys.argv[1:]
    out = None
    scale = 1.0
    if '--out' in args:
        i = args.index('--out'); out = Path(args[i + 1]); del args[i:i + 2]
    if '--scale' in args:
        i = args.index('--scale'); scale = float(args[i + 1]); del args[i:i + 2]
    for a in args:
        p = Path(a)
        d = (out or p.parent) / (p.stem + '.png')
        bmp2png(p, d, scale)
        print(d)
