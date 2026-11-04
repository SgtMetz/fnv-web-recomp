#!/usr/bin/env python3
"""Snapshot the threads of the running relinked game (a hang, a black screen) without stopping it:
python tools/thread_dump.py [--all] [--words N]

For each thread: EIP/ESP/EBP and the stack words that point into fnv_game.dll, resolved against
out/link/fnv_game.map (nearest symbol, as addr2fn.py). Only the main thread (the oldest) unless --all.
Needs a 64-bit Python (uses Wow64GetThreadContext on the 32-bit game)."""
import argparse
import bisect
import ctypes
import re
import subprocess
from ctypes import wintypes
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
k32 = ctypes.WinDLL('kernel32', use_last_error=True)
TH32CS_SNAPTHREAD = 4
THREAD_ALL = 0x1FFFFF
PROCESS_VM_READ = 0x10 | 0x400


class THREADENTRY32(ctypes.Structure):
    _fields_ = [('dwSize', wintypes.DWORD), ('cntUsage', wintypes.DWORD), ('th32ThreadID', wintypes.DWORD),
                ('th32OwnerProcessID', wintypes.DWORD), ('tpBasePri', wintypes.LONG), ('tpDeltaPri', wintypes.LONG),
                ('dwFlags', wintypes.DWORD)]


class WOW64_CONTEXT(ctypes.Structure):   # only the integer registers are read; the rest is padding
    _fields_ = [('ContextFlags', wintypes.DWORD), ('dr', wintypes.DWORD * 6), ('fp', ctypes.c_byte * 112),
                ('SegGs', wintypes.DWORD), ('SegFs', wintypes.DWORD), ('SegEs', wintypes.DWORD), ('SegDs', wintypes.DWORD),
                ('Edi', wintypes.DWORD), ('Esi', wintypes.DWORD), ('Ebx', wintypes.DWORD), ('Edx', wintypes.DWORD),
                ('Ecx', wintypes.DWORD), ('Eax', wintypes.DWORD), ('Ebp', wintypes.DWORD), ('Eip', wintypes.DWORD),
                ('SegCs', wintypes.DWORD), ('EFlags', wintypes.DWORD), ('Esp', wintypes.DWORD), ('SegSs', wintypes.DWORD),
                ('ext', ctypes.c_byte * 512)]


class FILETIME(ctypes.Structure):
    _fields_ = [('lo', wintypes.DWORD), ('hi', wintypes.DWORD)]


def load_map():
    rows = []
    for l in (ROOT / 'out/link/fnv_game.map').read_text(errors='replace').splitlines():
        m = re.match(r'\s*(?:\d+:)?\s*0001:[0-9a-f]{8}\s+(\S+)\s+([0-9a-f]{8,16})\s', l)
        if m:
            rows.append((int(m.group(2), 16), m.group(1)))
    rows.sort()
    return rows, [r[0] for r in rows]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--all', action='store_true')
    ap.add_argument('--words', type=int, default=2048)
    a = ap.parse_args()
    out = subprocess.run(['tasklist', '/FI', 'IMAGENAME eq FalloutNV_relinked.exe', '/FO', 'CSV', '/NH'],
                         capture_output=True, text=True).stdout
    m = re.search(r'"FalloutNV_relinked.exe","(\d+)"', out)
    if not m:
        print('game not running')
        return
    pid = int(m.group(1))
    rows, keys = load_map()
    lo, hi = keys[0] & ~0xfff, keys[-1] + 0x100000

    def name(v):
        k = bisect.bisect_right(keys, v) - 1
        return '%s +0x%x' % (rows[k][1][:90], v - rows[k][0]) if k >= 0 else '?'

    proc = k32.OpenProcess(PROCESS_VM_READ, False, pid)
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)
    te = THREADENTRY32()
    te.dwSize = ctypes.sizeof(te)
    tids = []
    ok = k32.Thread32First(snap, ctypes.byref(te))
    while ok:
        if te.th32OwnerProcessID == pid:
            tids.append(te.th32ThreadID)
        ok = k32.Thread32Next(snap, ctypes.byref(te))
    k32.CloseHandle(snap)
    threads = []
    for tid in tids:
        h = k32.OpenThread(THREAD_ALL, False, tid)
        c, e, kt, ut = FILETIME(), FILETIME(), FILETIME(), FILETIME()
        k32.GetThreadTimes(h, ctypes.byref(c), ctypes.byref(e), ctypes.byref(kt), ctypes.byref(ut))
        threads.append(((c.hi << 32) | c.lo, tid, h, ((ut.hi << 32) | ut.lo) / 1e7))
    threads.sort()
    print('pid %d, %d threads; main thread %d' % (pid, len(threads), threads[0][1]))
    for i, (_, tid, h, cpu) in enumerate(threads if a.all else threads[:1]):
        k32.Wow64SuspendThread(h)
        ctx = WOW64_CONTEXT()
        ctx.ContextFlags = 0x10003   # WOW64_CONTEXT_CONTROL | INTEGER
        got = k32.Wow64GetThreadContext(h, ctypes.byref(ctx))
        buf = (wintypes.DWORD * a.words)()
        n = ctypes.c_size_t()
        if got:
            k32.ReadProcessMemory(proc, ctypes.c_void_p(ctx.Esp), buf, ctypes.sizeof(buf), ctypes.byref(n))
        k32.ResumeThread(h)
        if not got:
            print('thread %d: no context (%d)' % (tid, ctypes.get_last_error()))
            continue
        print('thread %d%s user cpu %.1fs  eip=%08x esp=%08x ebp=%08x eax=%08x ecx=%08x' %
              (tid, ' (main)' if i == 0 else '', cpu, ctx.Eip, ctx.Esp, ctx.Ebp, ctx.Eax, ctx.Ecx))
        if lo <= ctx.Eip < hi:
            print('  eip   %08x  %s' % (ctx.Eip, name(ctx.Eip)))
        shown = 0
        for w in range(n.value // 4):
            v = buf[w]
            if lo <= v < hi and shown < 70:
                print('  sp+%04x %08x  %s' % (4 * w, v, name(v)))
                shown += 1
        k32.CloseHandle(h)


if __name__ == '__main__':
    main()
