// Dump the local Direct3D 9 adapter's caps, identifier and display modes as a C include for the recompiler's PAL
// Direct3D 9 (port/recomp/pal_d3d9.cpp), so the game sees the answers a real device gives.
// build: clang-cl --target=i686-pc-windows-msvc /O1 d3d9_caps_dump.cpp d3d9.lib ; run: d3d9_caps_dump > out/recomp/d3d9_caps.inc
#include <d3d9.h>
#include <stdio.h>

static void bytes(const char* name, const void* p, size_t n)
{
    printf("static const unsigned char %s[%u] = {", name, (unsigned)n);
    for (size_t i = 0; i < n; ++i) printf("%s%u", i ? "," : "", ((const unsigned char*)p)[i]);
    printf("};\n");
}

int main()
{
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) return 1;
    D3DCAPS9 caps;
    d3d->GetDeviceCaps(0, D3DDEVTYPE_HAL, &caps);
    bytes("kCaps", &caps, sizeof caps);
    D3DADAPTER_IDENTIFIER9 id;
    d3d->GetAdapterIdentifier(0, 0, &id);
    bytes("kIdentifier", &id, sizeof id);
    D3DDISPLAYMODE cur;
    d3d->GetAdapterDisplayMode(0, &cur);
    bytes("kCurrentMode", &cur, sizeof cur);
    const D3DFORMAT fmts[] = {D3DFMT_X8R8G8B8, D3DFMT_A8R8G8B8, D3DFMT_R5G6B5, D3DFMT_A2R10G10B10};
    printf("struct CapsMode { unsigned fmt, w, h, refresh; };\nstatic const CapsMode kModes[] = {\n");
    for (D3DFORMAT f : fmts)
    {
        UINT n = d3d->GetAdapterModeCount(0, f);
        for (UINT i = 0; i < n; ++i)
        {
            D3DDISPLAYMODE m;
            if (d3d->EnumAdapterModes(0, f, i, &m) == D3D_OK)
                printf("{%u,%u,%u,%u},\n", (unsigned)f, m.Width, m.Height, m.RefreshRate);
        }
    }
    printf("};\n");
    // which texture/surface formats the device accepts for which usages (CheckDeviceFormat answers)
    printf("struct CapsFormat { unsigned usage, rtype, fmt; int ok; };\nstatic const CapsFormat kFormats[] = {\n");
    const DWORD usages[] = {0, D3DUSAGE_RENDERTARGET, D3DUSAGE_DEPTHSTENCIL, D3DUSAGE_DYNAMIC,
                            D3DUSAGE_QUERY_FILTER, D3DUSAGE_QUERY_POSTPIXELSHADER_BLENDING, D3DUSAGE_AUTOGENMIPMAP,
                            D3DUSAGE_QUERY_VERTEXTEXTURE};
    const D3DRESOURCETYPE types[] = {D3DRTYPE_SURFACE, D3DRTYPE_TEXTURE, D3DRTYPE_CUBETEXTURE, D3DRTYPE_VOLUMETEXTURE};
    for (unsigned f = 1; f < 200; ++f)
        for (DWORD u : usages)
            for (D3DRESOURCETYPE t : types)
                if (d3d->CheckDeviceFormat(0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, u, t, (D3DFORMAT)f) == D3D_OK)
                    printf("{%u,%u,%u,1},\n", (unsigned)u, (unsigned)t, f);
    const char* fourcc[] = {"DXT1", "DXT2", "DXT3", "DXT4", "DXT5", "ATI1", "ATI2", "INTZ", "RAWZ", "DF24", "DF16", "NULL"};
    for (const char* c : fourcc)
    {
        unsigned f = MAKEFOURCC(c[0], c[1], c[2], c[3]);
        for (DWORD u : usages)
            for (D3DRESOURCETYPE t : types)
                if (d3d->CheckDeviceFormat(0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, u, t, (D3DFORMAT)f) == D3D_OK)
                    printf("{%u,%u,%u,1},\n", (unsigned)u, (unsigned)t, f);
    }
    printf("{0,0,0,0}};\n");
    d3d->Release();
    return 0;
}
