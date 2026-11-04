// Direct3D 9 model (Phase 2, docs/PHASE2_PLAN.md section 2). FNV_D3D9_MODEL=1 hands the game our own
// IDirect3D9 / IDirect3DDevice9 (generated forwarding classes in d3d9_model_gen.h) instead of the runtime's. The
// model keeps the device state the game sets in its own structure (render states, samplers, textures, shaders,
// shader constants, streams, targets, viewport, scissor) and counts each frame's work; for now every call is also
// forwarded to the real d3d9.dll, which draws. Phase 4 replaces the forwarding with a WebGPU backend reading this
// state; the frame comparison of the Phase 2 gate will record frames from here.
#pragma once
// included inside loader.cpp's namespace (g_log); <d3d9.h> is included at the top of loader.cpp

struct FnvDevice9;

struct D3d9ModelState
{
    DWORD renderState[256];
    DWORD samplerState[20][14];       // 16 pixel samplers + D3DDMAPSAMPLER and the 4 vertex samplers
    IDirect3DBaseTexture9* texture[20];
    IDirect3DVertexShader9* vs;
    IDirect3DPixelShader9* ps;
    float vsConst[256][4];
    float psConst[224][4];
    IDirect3DVertexBuffer9* stream[16];
    UINT streamOffset[16], streamStride[16];
    IDirect3DIndexBuffer9* indices;
    IDirect3DVertexDeclaration9* decl;
    DWORD fvf;
    IDirect3DSurface9* renderTarget[4];
    IDirect3DSurface9* depthStencil;
    D3DVIEWPORT9 viewport;
    RECT scissor;
    D3DPRESENT_PARAMETERS present;
    // this frame's work
    unsigned frame, draws, prims, clears, scenes, targetSwitches, stateSets, constSets;
    // resources the game created (counts)
    unsigned textures, vertexBuffers, indexBuffers, renderTargets, depthSurfaces, vertexShaders, pixelShaders;
};
static D3d9ModelState g_d3dm;

static DWORD model_sampler_index(DWORD s) { return s < 16 ? s : s == D3DDMAPSAMPLER ? 16 : s >= D3DVERTEXTEXTURESAMPLER0 ? 17 + (s - D3DVERTEXTEXTURESAMPLER0) % 3 : 19; }

static void model_Present(FnvDevice9*, CONST RECT*, CONST RECT*, HWND, CONST RGNDATA*)
{
    D3d9ModelState& m = g_d3dm;
    if (++m.frame % 600 == 0)
    {
        fprintf(g_log, "[d3d9model] frame %u: scenes %u draws %u prims %u clears %u target switches %u state sets %u "
                       "constant sets %u; resources: textures %u VB %u IB %u RT %u DS %u VS %u PS %u\n",
                m.frame, m.scenes, m.draws, m.prims, m.clears, m.targetSwitches, m.stateSets, m.constSets, m.textures,
                m.vertexBuffers, m.indexBuffers, m.renderTargets, m.depthSurfaces, m.vertexShaders, m.pixelShaders);
        fflush(g_log);
    }
    m.draws = m.prims = m.clears = m.scenes = m.targetSwitches = m.stateSets = m.constSets = 0;
}
static void model_BeginScene(FnvDevice9*) { ++g_d3dm.scenes; }
static void model_EndScene(FnvDevice9*) {}
static void model_Clear(FnvDevice9*, DWORD, CONST D3DRECT*, DWORD, D3DCOLOR, float, DWORD) { ++g_d3dm.clears; }
static void model_SetRenderTarget(FnvDevice9*, DWORD i, IDirect3DSurface9* s)
{
    if (i < 4) g_d3dm.renderTarget[i] = s;
    ++g_d3dm.targetSwitches;
}
static void model_SetDepthStencilSurface(FnvDevice9*, IDirect3DSurface9* s) { g_d3dm.depthStencil = s; }
static void model_SetViewport(FnvDevice9*, CONST D3DVIEWPORT9* v) { if (v) g_d3dm.viewport = *v; }
static void model_SetRenderState(FnvDevice9*, D3DRENDERSTATETYPE st, DWORD v)
{
    if ((unsigned)st < 256) g_d3dm.renderState[st] = v;
    ++g_d3dm.stateSets;
}
static void model_SetSamplerState(FnvDevice9*, DWORD s, D3DSAMPLERSTATETYPE t, DWORD v)
{
    if ((unsigned)t < 14) g_d3dm.samplerState[model_sampler_index(s)][t] = v;
    ++g_d3dm.stateSets;
}
static void model_SetTexture(FnvDevice9*, DWORD s, IDirect3DBaseTexture9* t) { g_d3dm.texture[model_sampler_index(s)] = t; }
static void model_SetVertexShader(FnvDevice9*, IDirect3DVertexShader9* s) { g_d3dm.vs = s; }
static void model_SetPixelShader(FnvDevice9*, IDirect3DPixelShader9* s) { g_d3dm.ps = s; }
static void model_SetVertexShaderConstantF(FnvDevice9*, UINT r, CONST float* v, UINT n)
{
    if (v && r < 256) memcpy(g_d3dm.vsConst[r], v, (r + n <= 256 ? n : 256 - r) * 16);
    ++g_d3dm.constSets;
}
static void model_SetPixelShaderConstantF(FnvDevice9*, UINT r, CONST float* v, UINT n)
{
    if (v && r < 224) memcpy(g_d3dm.psConst[r], v, (r + n <= 224 ? n : 224 - r) * 16);
    ++g_d3dm.constSets;
}
static void model_SetStreamSource(FnvDevice9*, UINT i, IDirect3DVertexBuffer9* b, UINT off, UINT stride)
{
    if (i < 16) { g_d3dm.stream[i] = b; g_d3dm.streamOffset[i] = off; g_d3dm.streamStride[i] = stride; }
}
static void model_SetIndices(FnvDevice9*, IDirect3DIndexBuffer9* b) { g_d3dm.indices = b; }
static void model_SetVertexDeclaration(FnvDevice9*, IDirect3DVertexDeclaration9* d) { g_d3dm.decl = d; }
static void model_SetFVF(FnvDevice9*, DWORD f) { g_d3dm.fvf = f; }
static void model_SetScissorRect(FnvDevice9*, CONST RECT* r) { if (r) g_d3dm.scissor = *r; }
static void model_StretchRect(FnvDevice9*, IDirect3DSurface9*, CONST RECT*, IDirect3DSurface9*, CONST RECT*, D3DTEXTUREFILTERTYPE) {}
static void model_DrawPrimitive(FnvDevice9*, D3DPRIMITIVETYPE, UINT, UINT n) { ++g_d3dm.draws; g_d3dm.prims += n; }
static void model_DrawIndexedPrimitive(FnvDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT n) { ++g_d3dm.draws; g_d3dm.prims += n; }
static void model_DrawPrimitiveUP(FnvDevice9*, D3DPRIMITIVETYPE, UINT n, CONST void*, UINT) { ++g_d3dm.draws; g_d3dm.prims += n; }
static void model_DrawIndexedPrimitiveUP(FnvDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT n, CONST void*, D3DFORMAT, CONST void*, UINT)
{
    ++g_d3dm.draws;
    g_d3dm.prims += n;
}
static void model_CreateTexture(FnvDevice9*, UINT, UINT, UINT, DWORD usage, D3DFORMAT, D3DPOOL, IDirect3DTexture9**, HANDLE*)
{
    ++g_d3dm.textures;
    if (usage & D3DUSAGE_RENDERTARGET) ++g_d3dm.renderTargets;
}
static void model_CreateVertexBuffer(FnvDevice9*, UINT, DWORD, DWORD, D3DPOOL, IDirect3DVertexBuffer9**, HANDLE*) { ++g_d3dm.vertexBuffers; }
static void model_CreateIndexBuffer(FnvDevice9*, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DIndexBuffer9**, HANDLE*) { ++g_d3dm.indexBuffers; }
static void model_CreateRenderTarget(FnvDevice9*, UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD, BOOL, IDirect3DSurface9**, HANDLE*) { ++g_d3dm.renderTargets; }
static void model_CreateDepthStencilSurface(FnvDevice9*, UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD, BOOL, IDirect3DSurface9**, HANDLE*) { ++g_d3dm.depthSurfaces; }
static void model_CreateVertexShader(FnvDevice9*, CONST DWORD*, IDirect3DVertexShader9**) { ++g_d3dm.vertexShaders; }
static void model_CreatePixelShader(FnvDevice9*, CONST DWORD*, IDirect3DPixelShader9**) { ++g_d3dm.pixelShaders; }
static void model_Reset(FnvDevice9*, D3DPRESENT_PARAMETERS* pp) { if (pp) g_d3dm.present = *pp; }

#include "d3d9_model_gen.h"

static IDirect3DDevice9* model_wrap_device(IDirect3DDevice9* real, D3DPRESENT_PARAMETERS* pp)
{
    if (pp) g_d3dm.present = *pp;
    fprintf(g_log, "[d3d9model] device %p wrapped by the model (%ux%u, format %d, windowed %d)\n", (void*)real,
            pp ? pp->BackBufferWidth : 0, pp ? pp->BackBufferHeight : 0, pp ? (int)pp->BackBufferFormat : 0,
            pp ? pp->Windowed : 0);
    fflush(g_log);
    return new FnvDevice9(real);
}

typedef IDirect3D9*(WINAPI* D3DCreate9Real)(UINT);
static D3DCreate9Real g_modelRealCreate9;
static IDirect3D9* WINAPI model_Direct3DCreate9(UINT sdk)
{
    IDirect3D9* real = g_modelRealCreate9(sdk);
    return real ? new FnvDirect3D9(real) : nullptr;
}
