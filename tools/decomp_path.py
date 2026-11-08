r"""Where the decompilation lives. Every tool takes it from here; set FNV_DECOMP to use another copy.

The default is the updated copy (2026-11-06). The earlier copy (E:\Games\fnv_decomp-main)
was deleted on 2026-11-08.
"""
import os

DECOMP_DIR = os.environ.get('FNV_DECOMP', r'E:\Games\fnv_decomp-main-updated')
