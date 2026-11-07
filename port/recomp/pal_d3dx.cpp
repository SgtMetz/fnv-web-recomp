// PAL D3DX group: the D3DX 9 functions the game imports. Math as D3DX defines it (row vectors, row-major matrices),
// textures from DDS files in memory onto the PAL device (pal_d3d9.h), ID3DXBuffer, and the shader entry points
// (profiles answered; run-time assembly/compilation fails cleanly: the game's shaders come precompiled from its packages).
#include "pal_d3d9.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <atomic>
#include <vector>

namespace {
#define A(i) harg(s, (i))
#define RET(v, n) return hret(s, (uint32_t)(v), (n))
const uint32_t D3DXERR_INVALIDDATA = 0x88760B59u;

float* F(uint32_t p) { return (float*)(uintptr_t)p; }
float fa(FnvCpu& s, int i) { uint32_t u = A(i); float f; memcpy(&f, &u, 4); return f; }

void mul(float* o, const float* a, const float* b)
{
    float r[16];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            r[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j] + a[i * 4 + 3] * b[12 + j];
    memcpy(o, r, 64);
}
void transpose(float* o, const float* a)
{
    float r[16];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) r[i * 4 + j] = a[j * 4 + i];
    memcpy(o, r, 64);
}
void x_MatrixMultiply(FnvCpu& s) { mul(F(A(0)), F(A(1)), F(A(2))); RET(A(0), 3); }
void x_MatrixMultiplyTranspose(FnvCpu& s) { float r[16]; mul(r, F(A(1)), F(A(2))); transpose(F(A(0)), r); RET(A(0), 3); }
void x_MatrixTranspose(FnvCpu& s) { transpose(F(A(0)), F(A(1))); RET(A(0), 2); }
void x_MatrixInverse(FnvCpu& s)
{
    const float* m = F(A(2));
    float inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (A(1)) *F(A(1)) = det;
    if (det == 0) RET(0, 3);
    for (float& v : inv) v /= det;
    memcpy(F(A(0)), inv, 64);
    RET(A(0), 3);
}
void x_MatrixRotationYawPitchRoll(FnvCpu& s)
{
    float y = fa(s, 1), p = fa(s, 2), r = fa(s, 3);
    float cy = cosf(y), sy = sinf(y), cp = cosf(p), sp = sinf(p), cr = cosf(r), sr = sinf(r);
    float* o = F(A(0));
    // D3DX: roll about z, then pitch about x, then yaw about y (row vectors)
    o[0] = cr * cy + sr * sp * sy; o[1] = sr * cp; o[2] = cr * -sy + sr * sp * cy; o[3] = 0;
    o[4] = -sr * cy + cr * sp * sy; o[5] = cr * cp; o[6] = sr * sy + cr * sp * cy; o[7] = 0;
    o[8] = cp * sy; o[9] = -sp; o[10] = cp * cy; o[11] = 0;
    o[12] = 0; o[13] = 0; o[14] = 0; o[15] = 1;
    RET(A(0), 4);
}
void x_PlaneNormalize(FnvCpu& s)
{
    const float* p = F(A(1));
    float l = sqrtf(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
    float* o = F(A(0));
    float k = l ? 1.f / l : 0.f;
    o[0] = p[0] * k; o[1] = p[1] * k; o[2] = p[2] * k; o[3] = p[3] * k;
    RET(A(0), 2);
}
void vec4_mul(float* o, const float* v, const float* m)
{
    float r[4];
    for (int j = 0; j < 4; ++j) r[j] = v[0] * m[j] + v[1] * m[4 + j] + v[2] * m[8 + j] + v[3] * m[12 + j];
    memcpy(o, r, 16);
}
void x_PlaneTransform(FnvCpu& s) { vec4_mul(F(A(0)), F(A(1)), F(A(2))); RET(A(0), 3); }
void x_Vec4Transform(FnvCpu& s) { vec4_mul(F(A(0)), F(A(1)), F(A(2))); RET(A(0), 3); }
void x_Vec3Normalize(FnvCpu& s)
{
    const float* v = F(A(1));
    float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    float* o = F(A(0));
    float k = l ? 1.f / l : 0.f;
    o[0] = v[0] * k; o[1] = v[1] * k; o[2] = v[2] * k;
    RET(A(0), 2);
}
void x_Vec3TransformCoord(FnvCpu& s)
{
    const float* v = F(A(1));
    float in[4] = {v[0], v[1], v[2], 1}, r[4];
    vec4_mul(r, in, F(A(2)));
    float* o = F(A(0));
    float w = r[3] ? 1.f / r[3] : 0.f;
    o[0] = r[0] * w; o[1] = r[1] * w; o[2] = r[2] * w;
    RET(A(0), 3);
}
void x_Vec3TransformNormal(FnvCpu& s)
{
    const float* v = F(A(1));
    float in[4] = {v[0], v[1], v[2], 0}, r[4];
    vec4_mul(r, in, F(A(2)));
    float* o = F(A(0));
    o[0] = r[0]; o[1] = r[1]; o[2] = r[2];
    RET(A(0), 3);
}

// ---- ID3DXBuffer ----------------------------------------------------------------------------------------------------
struct XBuf { uint32_t data, size; };
void xb_GetBufferPointer(FnvCpu& s) { RET(((XBuf*)com_host(A(0)))->data, 1); }
void xb_GetBufferSize(FnvCpu& s) { RET(((XBuf*)com_host(A(0)))->size, 1); }
uint32_t new_xbuffer(uint32_t size)
{
    static ComClass* c = com_class_custom("ID3DXBuffer", {
        {"QueryInterface", 12, nullptr}, {"AddRef", 4, nullptr}, {"Release", 4, nullptr},
        {"GetBufferPointer", 4, xb_GetBufferPointer}, {"GetBufferSize", 4, xb_GetBufferSize}});
    XBuf* b = new XBuf{(uint32_t)(uintptr_t)calloc(1, size ? size : 4), size};
    return com_new(c, b);
}
void x_CreateBuffer(FnvCpu& s) { M32(A(1)) = new_xbuffer(A(0)); RET(0, 2); }

// ---- DDS ------------------------------------------------------------------------------------------------------------
struct Dds { uint32_t w, h, depth, mips, fmt, kind; bool rgb24; const uint8_t* data; uint32_t size; };
uint32_t fourcc(const char* c) { return (uint32_t)c[0] | ((uint32_t)c[1] << 8) | ((uint32_t)c[2] << 16) | ((uint32_t)c[3] << 24); }
bool parse_dds(const uint8_t* p, uint32_t n, Dds& d)
{
    if (n < 128 || memcmp(p, "DDS ", 4)) return false;
    auto u = [&](uint32_t o) { uint32_t v; memcpy(&v, p + o, 4); return v; };
    d.h = u(12); d.w = u(16); d.depth = (u(8) & 0x800000) ? u(24) : 1; d.mips = u(28) ? u(28) : 1;
    uint32_t pf_flags = u(80), cc = u(84), bits = u(88), rm = u(92), gm = u(96), bm = u(100), am = u(104);
    uint32_t caps2 = u(112);
    d.kind = (caps2 & 0x200) ? 1 : (caps2 & 0x200000) ? 2 : 0;
    d.rgb24 = false;
    if (pf_flags & 4) d.fmt = cc;                                         // DDPF_FOURCC (DXTn, or a D3DFORMAT number)
    else if (pf_flags & 0x40)                                             // DDPF_RGB
    {
        if (bits == 32) d.fmt = am ? (rm == 0xff0000 ? 21 : 32) : (rm == 0xff0000 ? 22 : 33);
        else if (bits == 24) { d.fmt = 22; d.rgb24 = true; }
        else if (bits == 16) d.fmt = rm == 0xf800 ? 23 : am == 0x8000 ? 25 : am == 0xf000 ? 26 : 24;
        else return false;
    }
    else if (pf_flags & 0x20000) d.fmt = bits == 16 ? 51 : 50;            // DDPF_LUMINANCE
    else if (pf_flags & 0x2) d.fmt = 28;                                  // DDPF_ALPHA
    else return false;
    (void)gm; (void)bm;
    d.data = p + 128;
    d.size = n - 128;
    return true;
}
void x_GetImageInfoFromFileInMemory(FnvCpu& s)
{
    Dds d;
    if (!parse_dds((const uint8_t*)gstr(A(0)), A(1), d)) RET(D3DXERR_INVALIDDATA, 3);
    uint32_t o = A(2);
    if (o)
    {
        M32(o) = d.w; M32(o + 4) = d.h; M32(o + 8) = d.depth; M32(o + 12) = d.mips; M32(o + 16) = d.fmt;
        M32(o + 20) = d.kind == 1 ? 5 : d.kind == 2 ? 4 : 3; M32(o + 24) = 4;   // D3DXIFF_DDS
    }
    RET(0, 3);
}
uint32_t texture_from_dds(uint32_t device, const uint8_t* p, uint32_t n, uint32_t want_kind, uint32_t* out)
{
    Dds d;
    if (!parse_dds(p, n, d)) return D3DXERR_INVALIDDATA;
    uint32_t tex = pal_d3d_create_texture(device, d.kind, d.w, d.h, d.depth, d.mips, 0, d.fmt, 1);
    const uint8_t* q = d.data;
    const uint8_t* end = p + n;
    uint32_t faces = d.kind == 1 ? 6 : 1;
    std::vector<uint8_t> conv;
    for (uint32_t f = 0; f < faces; ++f)
        for (uint32_t l = 0; l < d.mips; ++l)
        {
            uint32_t w = d.w >> l ? d.w >> l : 1, h = d.h >> l ? d.h >> l : 1, dd = d.depth >> l ? d.depth >> l : 1;
            uint32_t bytes;
            if (d.rgb24)
            {
                bytes = w * h * 3 * dd;
                if (q + bytes > end) break;
                conv.resize((size_t)w * h * dd * 4);
                for (uint32_t i = 0; i < w * h * dd; ++i)
                {
                    conv[i * 4] = q[i * 3]; conv[i * 4 + 1] = q[i * 3 + 1]; conv[i * 4 + 2] = q[i * 3 + 2]; conv[i * 4 + 3] = 255;
                }
                pal_d3d_fill_level(tex, f, l, conv.data(), (uint32_t)conv.size());
            }
            else
            {
                bytes = pal_d3d_format_bytes(d.fmt, w, h, nullptr, nullptr) * dd;
                if (q + bytes > end) break;
                pal_d3d_fill_level(tex, f, l, q, bytes);
            }
            q += bytes;
        }
    (void)want_kind;
    *out = tex;
    return 0;
}
void x_CreateTextureFromFileInMemory(FnvCpu& s)
{
    uint32_t t = 0, r = texture_from_dds(A(0), (const uint8_t*)gstr(A(1)), A(2), 0, &t);
    M32(A(3)) = t;
    RET(r, 4);
}
void x_CreateCubeTextureFromFileInMemory(FnvCpu& s)
{
    uint32_t t = 0, r = texture_from_dds(A(0), (const uint8_t*)gstr(A(1)), A(2), 1, &t);
    M32(A(3)) = t;
    RET(r, 4);
}
void x_CreateVolumeTextureFromFileInMemory(FnvCpu& s)
{
    uint32_t t = 0, r = texture_from_dds(A(0), (const uint8_t*)gstr(A(1)), A(2), 2, &t);
    M32(A(3)) = t;
    RET(r, 4);
}
void x_LoadSurfaceFromSurface(FnvCpu& s) { RET(0, 8); }
void x_SaveTextureToFileA(FnvCpu& s) { RET(0x80004005u, 4); }

// ---- shaders ---------------------------------------------------------------------------------------------------------
std::atomic<bool> g_logged_compile;
void x_GetVertexShaderProfile(FnvCpu& s) { static char p[] = "vs_3_0"; RET((uintptr_t)p, 1); }
void x_GetPixelShaderProfile(FnvCpu& s) { static char p[] = "ps_3_0"; RET((uintptr_t)p, 1); }
void shader_fail(FnvCpu& s, int nargs, int out_shader, int out_errors)
{
    if (!g_logged_compile.exchange(true)) fnv_host_log("[d3dx] run-time shader assembly/compilation requested: not available\n");
    if (out_shader >= 0 && A(out_shader)) M32(A(out_shader)) = 0;
    if (out_errors >= 0 && A(out_errors)) M32(A(out_errors)) = 0;
    hret(s, 0x80004005u, nargs);
}
void x_AssembleShader(FnvCpu& s) { shader_fail(s, 7, 5, 6); }
void x_AssembleShaderFromFileA(FnvCpu& s) { shader_fail(s, 6, 4, 5); }
void x_CompileShader(FnvCpu& s) { if (A(9)) M32(A(9)) = 0; shader_fail(s, 10, 7, 8); }
void x_CompileShaderFromFileA(FnvCpu& s) { if (A(8)) M32(A(8)) = 0; shader_fail(s, 9, 6, 7); }
void x_GetShaderConstantTable(FnvCpu& s) { M32(A(1)) = 0; RET(0x80004005u, 2); }

const PalEntry kD3dx[] = {
    {"D3DXMatrixMultiply", x_MatrixMultiply}, {"D3DXMatrixMultiplyTranspose", x_MatrixMultiplyTranspose},
    {"D3DXMatrixTranspose", x_MatrixTranspose}, {"D3DXMatrixInverse", x_MatrixInverse},
    {"D3DXMatrixRotationYawPitchRoll", x_MatrixRotationYawPitchRoll}, {"D3DXPlaneNormalize", x_PlaneNormalize},
    {"D3DXPlaneTransform", x_PlaneTransform}, {"D3DXVec4Transform", x_Vec4Transform},
    {"D3DXVec3Normalize", x_Vec3Normalize}, {"D3DXVec3TransformCoord", x_Vec3TransformCoord},
    {"D3DXVec3TransformNormal", x_Vec3TransformNormal}, {"D3DXCreateBuffer", x_CreateBuffer},
    {"D3DXGetImageInfoFromFileInMemory", x_GetImageInfoFromFileInMemory},
    {"D3DXCreateTextureFromFileInMemory", x_CreateTextureFromFileInMemory},
    {"D3DXCreateCubeTextureFromFileInMemory", x_CreateCubeTextureFromFileInMemory},
    {"D3DXCreateVolumeTextureFromFileInMemory", x_CreateVolumeTextureFromFileInMemory},
    {"D3DXLoadSurfaceFromSurface", x_LoadSurfaceFromSurface}, {"D3DXSaveTextureToFileA", x_SaveTextureToFileA},
    {"D3DXGetVertexShaderProfile", x_GetVertexShaderProfile}, {"D3DXGetPixelShaderProfile", x_GetPixelShaderProfile},
    {"D3DXAssembleShader", x_AssembleShader}, {"D3DXAssembleShaderFromFileA", x_AssembleShaderFromFileA},
    {"D3DXCompileShader", x_CompileShader}, {"D3DXCompileShaderFromFileA", x_CompileShaderFromFileA},
    {"D3DXGetShaderConstantTable", x_GetShaderConstantTable},
    {nullptr, nullptr}};
} // namespace

const PalEntry* pal_d3dx_entries() { return kD3dx; }
