r"""Where the decompilation lives. Every tool takes it from here; set FNV_DECOMP to use another copy.

The default is the updated copy (2026-11-06, see docs/DECOMP_UPDATE.md); the earlier copy is E:\Games\fnv_decomp-main.
"""
import os

DECOMP_DIR = os.environ.get('FNV_DECOMP', r'E:\Games\fnv_decomp-main-updated')
