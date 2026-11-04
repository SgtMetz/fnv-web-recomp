#!/usr/bin/env python3
"""Address-space map of the running relinked game: what reserves the 4 GB (a failed 5.6 MB texture allocation with
3.99 GB virtual / 0.7 GB private found address-space exhaustion).

usage: python tools/vmmap.py [--top N]
Groups regions by allocation base: type (image/mapped/private), state (committed/reserved), size; prints the totals
and the largest allocations, with module names for images."""
import argparse
import ctypes
import re
import subprocess
from collections import defaultdict
from ctypes import wintypes

k32 = ctypes.WinDLL('kernel32', use_last_error=True)
psapi = ctypes.WinDLL('psapi')


class MBI(ctypes.Structure):
    _fields_ = [('BaseAddress', ctypes.c_void_p), ('AllocationBase', ctypes.c_void_p), ('AllocationProtect', wintypes.DWORD),
                ('PartitionId', wintypes.WORD), ('RegionSize', ctypes.c_size_t), ('State', wintypes.DWORD),
                ('Protect', wintypes.DWORD), ('Type', wintypes.DWORD)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--top', type=int, default=25)
    a = ap.parse_args()
    out = subprocess.run(['tasklist', '/FI', 'IMAGENAME eq FalloutNV_relinked.exe', '/FO', 'CSV', '/NH'],
                         capture_output=True, text=True).stdout
    m = re.search(r'"FalloutNV_relinked.exe","(\d+)"', out)
    if not m:
        print('game not running')
        return
    h = k32.OpenProcess(0x0400 | 0x0010, False, int(m.group(1)))
    addr, mbi = 0, MBI()
    allocs = defaultdict(lambda: [0, 0, 0, ''])     # base -> [committed, reserved, type, name]
    totals = defaultdict(int)
    free_max, free_total = 0, 0
    while addr < 0x100000000 and k32.VirtualQueryEx(h, ctypes.c_void_p(addr), ctypes.byref(mbi), ctypes.sizeof(mbi)):
        size = mbi.RegionSize
        if mbi.State == 0x10000:                     # MEM_FREE
            free_total += size
            free_max = max(free_max, size)
        else:
            base = mbi.AllocationBase or 0
            e = allocs[base]
            e[0 if mbi.State == 0x1000 else 1] += size
            e[2] = mbi.Type
            kind = {0x1000000: 'image', 0x40000: 'mapped', 0x20000: 'private'}.get(mbi.Type, '?')
            totals[(kind, 'committed' if mbi.State == 0x1000 else 'reserved')] += size
        addr = (mbi.BaseAddress or 0) + size
    for base, e in allocs.items():
        if e[2] == 0x1000000:
            buf = ctypes.create_unicode_buffer(260)
            if psapi.GetMappedFileNameW(h, ctypes.c_void_p(base), buf, 260):
                e[3] = buf.value.rsplit('\\', 1)[-1]
    for k in sorted(totals):
        print('%-8s %-10s %7.0f MB' % (k[0], k[1], totals[k] / 2**20))
    print('free %7.0f MB, largest free block %.1f MB' % (free_total / 2**20, free_max / 2**20))
    sizes = defaultdict(lambda: [0, 0])
    for base, e in allocs.items():
        if e[2] == 0x20000:
            sizes[e[0] + e[1]][0] += 1
            sizes[e[0] + e[1]][1] += e[1]
    print('private allocation sizes (count x size, reserved part):')
    for sz, (n, res) in sorted(sizes.items(), key=lambda kv: -kv[0] * kv[1][0])[:12]:
        print('  %4d x %8.2f MB = %7.0f MB  (%.0f MB reserved only)' % (n, sz / 2**20, n * sz / 2**20, res / 2**20))
    print('largest allocations:')
    for base, e in sorted(allocs.items(), key=lambda kv: -(kv[1][0] + kv[1][1]))[:a.top]:
        kind = {0x1000000: 'image', 0x40000: 'mapped', 0x20000: 'private'}.get(e[2], '?')
        print('  %08x %8.1f MB (%6.1f committed) %-7s %s' % (base, (e[0] + e[1]) / 2**20, e[0] / 2**20, kind, e[3]))


if __name__ == '__main__':
    main()
