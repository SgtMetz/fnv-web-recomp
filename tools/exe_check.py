"""Spike 1: fingerprint check of a FalloutNV.exe against the decomp's build/image_check.json.
Prints probe pass/fail and PE header facts only; never writes bytes of the image."""
import hashlib, json, struct, sys
from pathlib import Path
DECOMP = Path(sys.argv[2] if len(sys.argv) > 2 else r'E:\Games\fnv_decomp-main')
sys.path.insert(0, str(DECOMP / 'tools'))
from compare import Pe
pe = Pe(sys.argv[1])
spec = json.loads((DECOMP / 'build/image_check.json').read_text())
ok = 0
for addr, size, digest in spec['probes']:
    try: good = hashlib.sha256(pe.read(int(addr, 16), size)).hexdigest() == digest
    except ValueError as e: good = False
    ok += good
    print(f'{addr} {size:4d} {"ok" if good else "DIFF"}')
print(f'probes matched: {ok}/{len(spec["probes"])} (build_and_match.py needs >= 12)')
print('image base %#x' % pe.base)
for (rva, vs, rs, raw), n in zip(pe.sections, pe.names):
    print(f'  {n:8s} va={pe.base+rva:#010x} vsize={vs:#x} rawsize={rs:#x}')
d = pe.data; o = struct.unpack_from('<I', d, 0x3c)[0] + 24
ts = struct.unpack_from('<I', d, o - 24 + 8)[0]
reloc_rva, reloc_size = struct.unpack_from('<II', d, o + 96 + 5 * 8)
print('timestamp %#x, reloc dir rva=%#x size=%#x' % (ts, reloc_rva, reloc_size))
