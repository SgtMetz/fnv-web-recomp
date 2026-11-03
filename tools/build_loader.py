#!/usr/bin/env python3
"""Rebuild only out/link/FalloutNV_relinked.exe from port/loader/loader.cpp (the DLL is untouched).

usage: python tools/build_loader.py"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import link_build as lb  # noqa: E402

vs = sorted(lb.rp.VS.iterdir())[-1] / 'lib/x86'
sdk = sorted(lb.rp.KITS.iterdir())[-1]
r = lb.build_loader(vs, sdk)
print(r['exit'], r['log'][-400:] if r['exit'] else 'ok')
sys.exit(r['exit'])
