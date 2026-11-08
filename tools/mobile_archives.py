#!/usr/bin/env python3
"""Make the phone version's archives: smaller copies of the game's texture and sound BSAs (FNV, version 104).

  textures  DDS files keep only the mip levels at or below --cap pixels (default 256; 128 for normal maps and the
            distant-terrain tiles in textures/landscape/lod). Nothing is resampled: the top levels are cut off, so a
            2048 texture becomes its own 256 mip (1/64 of the bytes). Textures without mip levels, cube maps with
            odd layouts, volume textures, and everything under textures/interface and textures/fonts (menus and
            text) stay as they are.
  sounds    16-bit PCM WAVs at 32 kHz or more are halved in rate (neighbouring samples averaged): 48 kHz becomes
            24 kHz, 44.1 becomes 22.05. The game's DirectSound mixer (port/recomp/pal_dsound.cpp) plays any rate.
            WAVs with cue or loop chunks (sample positions) and the .ogg files stay as they are.

Every other file is copied unchanged, with the same compression. The output goes to out/mobile/ with the original
names. The browser file layer reads /game/data/mobile/<name>.bsa in place of /game/data/<name>.bsa when the page sets
FNV_ARCHIVES=mobile (port/recomp/pal_fs_web.cpp); server/serve.mjs serves out/mobile at /game/data/mobile. Derived
from the player's copy of the game: never committed.

usage: python -B tools/mobile_archives.py [--game "E:/Games/Fallout New Vegas"] [--cap 256] [--out out/mobile]
"""
import argparse
import array
import os
import struct
import sys
import time
import zlib
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ARCHIVES = ['Fallout - Textures.bsa', 'Fallout - Textures2.bsa', 'Fallout - Sound.bsa']
LOW_CAP = 128                 # normal maps and distant terrain
LOW_DIRS = (r'textures\landscape\lod',)
KEEP_DIRS = ('textures\\interface', 'textures\\fonts')
F_COMPRESSED, F_EMBED_NAMES = 0x4, 0x100
SIZE_TOGGLE = 0x40000000
BLOCK = {b'DXT1': 8, b'DXT2': 16, b'DXT3': 16, b'DXT4': 16, b'DXT5': 16, b'ATI1': 8, b'BC4U': 8, b'ATI2': 16, b'BC5U': 16}


def level_bytes(w, h, fourcc, bpp):
    if fourcc in BLOCK:
        return max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * BLOCK[fourcc]
    return w * h * bpp // 8


def shrink_dds(d: bytes, cap: int) -> bytes:
    """the DDS with its top mip levels dropped until both sides are <= cap, or d itself if it cannot be done"""
    if len(d) < 128 or d[:4] != b'DDS ':
        return d
    flags, h, w, _, depth, mips = struct.unpack_from('<IIIIII', d, 8)
    pf_flags, fourcc, bpp = struct.unpack_from('<I4sI', d, 80)
    caps2 = struct.unpack_from('<I', d, 112)[0]
    if mips <= 1 or max(w, h) <= cap or caps2 & 0x200000 or fourcc == b'DX10':
        return d
    if pf_flags & 0x4:                      # FOURCC
        if fourcc not in BLOCK:
            return d
    elif pf_flags & 0x40 or pf_flags & 0x20000 or pf_flags & 0x2:   # RGB, luminance, alpha
        fourcc = None
        if bpp % 8:
            return d
    else:
        return d
    faces = 6 if caps2 & 0x200 else 1
    if faces == 6 and (caps2 & 0xfc00) != 0xfc00:
        return d
    sizes = [level_bytes(max(1, w >> i), max(1, h >> i), fourcc, bpp) for i in range(mips)]
    if 128 + faces * sum(sizes) != len(d):
        return d                             # an unexpected layout: leave it alone
    drop = 0
    while drop < mips - 1 and max(max(1, w >> drop), max(1, h >> drop)) > cap:
        drop += 1
    if not drop:
        return d
    hdr = bytearray(d[:128])
    nw, nh = max(1, w >> drop), max(1, h >> drop)
    struct.pack_into('<III', hdr, 12, nh, nw, sizes[drop] if fourcc else nw * bpp // 8)
    struct.pack_into('<I', hdr, 28, mips - drop)
    out = [bytes(hdr)]
    face = sum(sizes)
    skip = sum(sizes[:drop])
    for f in range(faces):
        out.append(d[128 + f * face + skip:128 + (f + 1) * face])
    return b''.join(out)


def shrink_wav(d: bytes) -> bytes:
    """a 16-bit PCM WAV of 32 kHz or more at half its rate, or d itself"""
    if len(d) < 44 or d[:4] != b'RIFF' or d[8:12] != b'WAVE' or sys.byteorder != 'little':
        return d
    chunks, at = [], 12
    while at + 8 <= len(d):
        cid, n = d[at:at + 4], struct.unpack_from('<I', d, at + 4)[0]
        chunks.append([cid, d[at + 8:at + 8 + n]])
        at += 8 + n + (n & 1)
    ids = [c[0] for c in chunks]
    if b'fmt ' not in ids or b'data' not in ids or b'cue ' in ids or b'smpl' in ids:
        return d
    fmt = bytearray(chunks[ids.index(b'fmt ')][1])
    tag, ch, rate, avg, block, bits = struct.unpack_from('<HHIIHH', fmt, 0)
    if tag != 1 or bits != 16 or ch not in (1, 2) or rate < 32000:
        return d
    pcm = chunks[ids.index(b'data')][1]
    a = array.array('h')
    a.frombytes(pcm[:len(pcm) // (2 * ch * 2) * (2 * ch * 2)])     # whole pairs of frames
    out = array.array('h', bytes(len(a)))[:len(a) // 2]
    for c in range(ch):
        x, y = a[c::2 * ch], a[c + ch::2 * ch]
        out[c::ch] = array.array('h', [(p + q) >> 1 for p, q in zip(x, y)])
    struct.pack_into('<II', fmt, 4, rate // 2, avg // 2)
    chunks[ids.index(b'fmt ')][1] = bytes(fmt)
    chunks[ids.index(b'data')][1] = out.tobytes()
    body = b''.join(cid + struct.pack('<I', len(c)) + c + (bytes(1) if len(c) & 1 else b'') for cid, c in chunks)
    return b'RIFF' + struct.pack('<I', 4 + len(body)) + b'WAVE' + body


def convert(job):
    """one file's stored bytes -> new stored bytes (same compression and name prefix)"""
    raw, compressed, prefix_len, path, cap = job
    if path.endswith('.dds') and not path.startswith(KEEP_DIRS):
        if path.endswith('_n.dds') or path.startswith(LOW_DIRS):
            cap = min(cap, LOW_CAP)
        shrink = lambda d: shrink_dds(d, cap)
    elif path.endswith('.wav'):
        shrink = shrink_wav
    else:
        return raw, False
    prefix, body = raw[:prefix_len], raw[prefix_len:]
    data = zlib.decompress(body[4:]) if compressed else body
    new = shrink(data)
    if new is data:
        return raw, False
    if compressed:
        return prefix + struct.pack('<I', len(new)) + zlib.compress(new, 6), True
    return prefix + new, True


def jobs_for(f, files, flags, cap):
    for rec in files:
        f.seek(rec['offset'])
        raw = f.read(rec['size'])
        prefix_len = 1 + raw[0] if flags & F_EMBED_NAMES else 0
        yield raw, rec['compressed'], prefix_len, rec['path'], cap


def shrink_archive(src: Path, dst: Path, cap: int, pool):
    with open(src, 'rb') as f:
        magic, ver, off, flags, nfold, nfile, foldlen, filelen, fileflags = struct.unpack('<4sIIIIIIII', f.read(36))
        if magic != b'BSA\0' or ver != 104:
            sys.exit(f'{src}: not an FNV archive (version {ver})')
        folders = [struct.unpack('<QII', f.read(16)) for _ in range(nfold)]
        files = []                     # in record order, each with the position of its record in the header
        for _, count, _ in folders:
            name = f.read(f.read(1)[0])[:-1].decode('cp1252') if flags & 1 else ''
            for _ in range(count):
                pos = f.tell()
                h, size, offset = struct.unpack('<QII', f.read(16))
                comp = bool(flags & F_COMPRESSED) != bool(size & SIZE_TOGGLE)
                files.append({'rec': pos, 'size': size & ~SIZE_TOGGLE, 'toggle': size & SIZE_TOGGLE,
                              'offset': offset, 'compressed': comp, 'folder': name.lower()})
        names = f.read(filelen).split(b'\0') if flags & 2 else []
        for rec, nm in zip(files, names):
            rec['path'] = rec['folder'] + '\\' + nm.decode('cp1252').lower()
        data_start = min(r['offset'] for r in files)
        f.seek(0)
        header = bytearray(f.read(data_start))
        order = sorted(files, key=lambda r: r['offset'])
        tmp = dst.with_suffix('.tmp')
        changed = 0
        with open(tmp, 'wb') as o:
            o.write(header)            # rewritten below with the new sizes and offsets
            at = data_start
            for i in range(0, len(order), 512):        # in batches, so the whole archive is never in memory
                part = order[i:i + 512]
                for rec, (new, did) in zip(part, pool.map(convert, list(jobs_for(f, part, flags, cap)), chunksize=8)):
                    struct.pack_into('<II', header, rec['rec'] + 8, len(new) | rec['toggle'], at)
                    o.write(new)
                    at += len(new)
                    changed += did
            o.seek(0)
            o.write(header)
    os.replace(tmp, dst)
    return len(files), changed


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--game', default=os.environ.get('FNV_GAME', 'E:/Games/Fallout New Vegas'))
    ap.add_argument('--cap', type=int, default=256)
    ap.add_argument('--out', default=str(ROOT / 'out/mobile'))
    a = ap.parse_args()
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    with ProcessPoolExecutor() as pool:
        for name in ARCHIVES:
            t0 = time.time()
            src, dst = Path(a.game) / 'Data' / name, out / name
            n, changed = shrink_archive(src, dst, a.cap, pool)
            print(f'{name}: {src.stat().st_size / 1e6:.0f} MB -> {dst.stat().st_size / 1e6:.0f} MB '
                  f'({changed} of {n} files shrunk, {time.time() - t0:.0f} s)', flush=True)


if __name__ == '__main__':
    main()
