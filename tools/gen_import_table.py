#!/usr/bin/env python3
"""Argument sizes of every import of FalloutNV.exe, for host stubs that must pop exactly what the real function pops.

stdcall sizes come from the Windows SDK import libraries (decorated names `_Name@N`); D3DX, the ordinal imports
(WinSock, XInput, DirectSound) and the cdecl libraries (the Steam API wrapper, libvorbisfile) are written out here from
their public prototypes. Output: out/recomp/import_table.inc (derived from the player's import list, git-ignored):
  {"dll", "name", arg_bytes, cdecl},

usage: python tools/gen_import_table.py --exe <FalloutNV.exe>
"""
import argparse
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from relink import Image  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
NM = r'C:\Program Files\LLVM\bin\llvm-nm.exe'
SDK_LIBS = ['kernel32.Lib', 'User32.Lib', 'Gdi32.Lib', 'AdvAPI32.Lib', 'shell32.lib', 'ComCtl32.Lib', 'Ole32.Lib',
            'WinMM.Lib', 'WSock32.Lib', 'dinput8.lib', 'dsound.lib', 'd3d9.lib', 'xinput.lib']
# d3dx9 (stdcall, d3dx9*.h): argument count
D3DX = {'D3DXAssembleShader': 7, 'D3DXAssembleShaderFromFileA': 6, 'D3DXCompileShader': 10,
        'D3DXCompileShaderFromFileA': 9, 'D3DXCreateBuffer': 2, 'D3DXCreateCubeTextureFromFileInMemory': 4,
        'D3DXCreateTextureFromFileInMemory': 4, 'D3DXCreateVolumeTextureFromFileInMemory': 4,
        'D3DXGetImageInfoFromFileInMemory': 3, 'D3DXGetPixelShaderProfile': 1, 'D3DXGetShaderConstantTable': 2,
        'D3DXGetVertexShaderProfile': 1, 'D3DXLoadSurfaceFromSurface': 8, 'D3DXMatrixInverse': 3,
        'D3DXMatrixMultiply': 3, 'D3DXMatrixMultiplyTranspose': 3, 'D3DXMatrixRotationYawPitchRoll': 4,
        'D3DXMatrixTranspose': 2, 'D3DXPlaneNormalize': 2, 'D3DXPlaneTransform': 3, 'D3DXSaveTextureToFileA': 4,
        'D3DXVec3Normalize': 2, 'D3DXVec3TransformCoord': 3, 'D3DXVec3TransformNormal': 3, 'D3DXVec4Transform': 3}
# ordinal imports: (dll, ordinal) -> (name, argument count)
ORDINALS = {
    ('wsock32.dll', 1): ('accept', 3), ('wsock32.dll', 2): ('bind', 3), ('wsock32.dll', 3): ('closesocket', 1),
    ('wsock32.dll', 4): ('connect', 3), ('wsock32.dll', 5): ('getpeername', 3), ('wsock32.dll', 6): ('getsockname', 3),
    ('wsock32.dll', 7): ('getsockopt', 5), ('wsock32.dll', 8): ('htonl', 1), ('wsock32.dll', 9): ('htons', 1),
    ('wsock32.dll', 10): ('inet_addr', 1), ('wsock32.dll', 11): ('inet_ntoa', 1), ('wsock32.dll', 12): ('ioctlsocket', 3),
    ('wsock32.dll', 13): ('listen', 2), ('wsock32.dll', 14): ('ntohl', 1), ('wsock32.dll', 15): ('ntohs', 1),
    ('wsock32.dll', 16): ('recv', 4), ('wsock32.dll', 17): ('recvfrom', 6), ('wsock32.dll', 18): ('select', 5),
    ('wsock32.dll', 19): ('send', 4), ('wsock32.dll', 20): ('sendto', 6), ('wsock32.dll', 21): ('setsockopt', 5),
    ('wsock32.dll', 22): ('shutdown', 2), ('wsock32.dll', 23): ('socket', 3), ('wsock32.dll', 51): ('gethostbyaddr', 3),
    ('wsock32.dll', 52): ('gethostbyname', 1), ('wsock32.dll', 57): ('gethostname', 2),
    ('wsock32.dll', 101): ('WSAAsyncSelect', 4), ('wsock32.dll', 111): ('WSAGetLastError', 0),
    ('wsock32.dll', 115): ('WSAStartup', 2), ('wsock32.dll', 116): ('WSACleanup', 0), ('wsock32.dll', 151): ('__WSAFDIsSet', 2),
    ('xinput1_3.dll', 2): ('XInputGetState', 2), ('xinput1_3.dll', 3): ('XInputSetState', 2),
    ('xinput1_3.dll', 4): ('XInputGetCapabilities', 3), ('dsound.dll', 1): ('DirectSoundCreate', 3),
    ('dsound.dll', 11): ('DirectSoundCreate8', 3)}
CDECL_DLLS = ('galaxywrp.dll', 'libvorbisfile.dll')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', required=True)
    a = ap.parse_args()
    sdk = sorted(Path(r'C:\Program Files (x86)\Windows Kits\10\Lib').iterdir())[-1] / 'um/x86'
    sizes = {}
    for lib in SDK_LIBS:
        out = subprocess.run([NM, str(sdk / lib)], capture_output=True, text=True, errors='replace').stdout
        for m in re.finditer(r'__imp__([A-Za-z0-9_]+)@(\d+)', out):
            sizes.setdefault(m.group(1), int(m.group(2)))
    rows, missing = [], []
    for dll_name in sorted(set(Image(a.exe).imports().values())):
        dll, name = dll_name.split('!', 1)
        dll = dll.lower()
        cdecl = dll in CDECL_DLLS
        if name.startswith('#'):
            hit = ORDINALS.get((dll, int(name[1:])))
            if not hit:
                missing.append(dll_name)
                continue
            real, n = hit
            rows.append((dll, name, real, 4 * n, False))
            continue
        m = re.match(r'^_(\w+)@(\d+)$', name)                 # decorated already (binkw32)
        if m:
            rows.append((dll, name, m.group(1), int(m.group(2)), False))
        elif cdecl:
            rows.append((dll, name, name, 0, True))
        elif name in D3DX:
            rows.append((dll, name, name, 4 * D3DX[name], False))
        elif name in sizes:
            rows.append((dll, name, name, sizes[name], False))
        else:
            missing.append(dll_name)
    if missing:
        raise SystemExit('no argument size for: %s' % ', '.join(missing))
    out = ROOT / 'out/recomp/import_table.inc'
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text('// Generated by tools/gen_import_table.py: do not edit or commit.\n' +
                   ''.join('{"%s", "%s", "%s", %d, %d},\n' % (d, n, r, b, int(c)) for d, n, r, b, c in rows))
    print('%d imports -> %s' % (len(rows), out))


if __name__ == '__main__':
    main()
