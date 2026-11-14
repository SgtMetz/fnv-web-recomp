// PAL Direct3D 9 (pal_d3d9.h): the device, its resources and the command ring.
#include "pal_d3d9.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#ifdef __EMSCRIPTEN__
#include <emscripten/threading.h>
#endif

#include "../../out/recomp/d3d9_caps.inc"

uint32_t g_device_obj;          // the game's device (one per game)
// frame pacing (web/recomp/debug.js perf()): frames, microseconds between Presents, microseconds of that spent
// waiting for the renderer. Frame time minus wait is the game thread's own work per frame.
volatile uint64_t g_present_stats[3];
extern "C" uint64_t* fnv_present_stats() { return (uint64_t*)g_present_stats; }

namespace {
#define A(i) harg(s, (i))
#define RET(v, n) return hret(s, (uint32_t)(v), (n))
const uint32_t D3DERR_NOTAVAILABLE = 0x8876086Au, D3DERR_INVALIDCALL = 0x8876086Cu;

// ---- command ring --------------------------------------------------------------------------------------------------
D3dRing g_ring;
std::mutex g_ring_mx;

void ring_wait_space(uint32_t need)
{
    for (;;)
    {
        uint32_t w = g_ring.write, r = __atomic_load_n(&g_ring.read, __ATOMIC_ACQUIRE);
        uint32_t used = w >= r ? w - r : g_ring.size - (r - w);
        if (g_ring.size - used > need + 64) return;
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
}

void emit(uint32_t op, std::initializer_list<uint32_t> words, const void* data = nullptr, uint32_t len = 0)
{
    if (!g_ring.enabled) return;
    uint32_t payload = (uint32_t)words.size() * 4 + ((len + 3) & ~3u);
    uint32_t total = 8 + payload;
    std::lock_guard<std::mutex> lk(g_ring_mx);
    if (total + 64 > g_ring.size) return;                  // too big for the ring: dropped (logged by the renderer as a gap)
    ring_wait_space(total + 8);
    uint32_t w = g_ring.write;
    if (w + total + 8 > g_ring.size)
    {                                                      // not enough room before the end: wrap
        ring_wait_space(total + 8 + (g_ring.size - w));
        M32(g_ring.data + w) = OP_WRAP;
        M32(g_ring.data + w + 4) = 0;
        w = 0;
        // the reader must have left the start: it is behind the old write position, which is >= total here
        while (__atomic_load_n(&g_ring.read, __ATOMIC_ACQUIRE) <= total + 8 &&
               __atomic_load_n(&g_ring.read, __ATOMIC_ACQUIRE) != g_ring.write)
            std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
    uint32_t p = g_ring.data + w;
    M32(p) = op;
    M32(p + 4) = payload;
    uint32_t q = p + 8;
    for (uint32_t v : words) { M32(q) = v; q += 4; }
    if (len) memcpy((void*)(uintptr_t)q, data, len);
    __atomic_store_n(&g_ring.write, w + total, __ATOMIC_RELEASE);
}

uint32_t fbits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

// ---- formats ---------------------------------------------------------------------------------------------------------
uint32_t fourcc(const char* c) { return (uint32_t)c[0] | ((uint32_t)c[1] << 8) | ((uint32_t)c[2] << 16) | ((uint32_t)c[3] << 24); }
bool is_block(uint32_t f, uint32_t* block_bytes)
{
    if (f == fourcc("DXT1") || f == fourcc("ATI1")) { *block_bytes = 8; return true; }
    if (f == fourcc("DXT2") || f == fourcc("DXT3") || f == fourcc("DXT4") || f == fourcc("DXT5") || f == fourcc("ATI2"))
    { *block_bytes = 16; return true; }
    return false;
}
uint32_t bytes_per_pixel(uint32_t f)
{
    switch (f)
    {
    case 28: case 50: case 41: case 52: case 27: return 1;
    case 23: case 24: case 25: case 26: case 51: case 111: case 80: case 70: case 60: case 81: case 29: case 30: return 2;
    case 20: return 3;
    case 36: case 113: case 115: case 64: return 8;
    case 116: return 16;
    default: return 4;
    }
}
} // namespace

uint32_t pal_d3d_format_bytes(uint32_t fmt, uint32_t w, uint32_t h, uint32_t* row_pitch, uint32_t* rows)
{
    uint32_t bb;
    if (is_block(fmt, &bb))
    {
        uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
        if (row_pitch) *row_pitch = bw * bb;
        if (rows) *rows = bh;
        return bw * bb * bh;
    }
    uint32_t bpp = bytes_per_pixel(fmt);
    if (row_pitch) *row_pitch = w * bpp;
    if (rows) *rows = h;
    return w * bpp * h;
}

namespace {
// ---- resources --------------------------------------------------------------------------------------------------------
std::atomic<uint32_t> g_next_id{16};
struct Device;

struct Level { uint32_t mem = 0, bytes = 0, pitch = 0, rows = 0; bool locked = false; uint32_t lx = 0, ly = 0, lw = 0, lh = 0; };
struct Tex                                  // textures, and standalone surfaces (render targets, depth, offscreen)
{
    uint32_t id, kind, fmt, w, h, depth, levels, usage, pool;
    uint32_t obj = 0;                       // guest object (texture or, standalone, the surface)
    std::vector<Level> lv;                  // faces * levels
    std::vector<uint32_t> surf;             // guest surface objects per face*level (lazily)
    Device* dev;
    uint32_t faces() const { return kind == 1 ? 6 : 1; }
    uint32_t lw(uint32_t l) const { return w >> l ? w >> l : 1; }
    uint32_t lh(uint32_t l) const { return h >> l ? h >> l : 1; }
    uint32_t ld(uint32_t l) const { return depth >> l ? depth >> l : 1; }
};
struct Surf { Tex* tex; uint32_t face, level; };
struct Buf { uint32_t id, size, kind, usage, pool, fvf, fmt, mem; bool locked = false; uint32_t lo = 0, lsize = 0; uint32_t obj; Device* dev; };
struct Shader { uint32_t id, stage, tokens, bytes; uint32_t obj; Device* dev; };
struct Decl { uint32_t id, count, mem; uint32_t obj; Device* dev; };
struct Query { uint32_t type; uint32_t obj; Device* dev; };
struct Block { uint32_t obj; Device* dev; };

struct Device
{
    uint32_t obj = 0, d3d = 0, hwnd = 0, behavior = 0;
    uint32_t bb_w = 1280, bb_h = 720, bb_fmt = 22;
    uint32_t pp[14];
    Tex* backbuffer = nullptr; uint32_t bb_surf = 0;
    Tex* autodepth = nullptr; uint32_t ad_surf = 0;
    uint32_t swapchain = 0;
    uint32_t rs[256] = {};
    uint32_t ss[20][14] = {};
    uint32_t tss[8][33] = {};
    uint32_t tex[20] = {};                  // guest texture objects by stage (0-15, D3DVERTEXTEXTURESAMPLER0.. -> 16-19)
    uint32_t rt[4] = {}, ds = 0;            // guest surface objects
    uint32_t vs = 0, ps = 0, decl = 0, fvf = 0, ib = 0;
    uint32_t stream[16] = {}, stream_off[16] = {}, stream_stride[16] = {}, stream_freq[16] = {};
    float vsf[256 * 4] = {}, psf[224 * 4] = {};
    int32_t vsi[16 * 4] = {}, psi[16 * 4] = {};
    uint32_t vsb[16] = {}, psb[16] = {};
    uint32_t viewport[6] = {};
    uint32_t scissor[4] = {};
    uint8_t material[68] = {};
    std::map<uint32_t, std::vector<uint8_t>> transforms, lights;
    std::map<uint32_t, uint32_t> light_enable;
    float clip[6][4] = {};
    uint32_t frame = 0;
    bool recording = false;
};

Device* dev_of(uint32_t obj) { return (Device*)com_host(obj); }
uint32_t tex_id_of_surface(uint32_t sobj, uint32_t* face, uint32_t* level)
{
    if (!sobj) { *face = *level = 0; return 0; }
    Surf* sf = (Surf*)com_host(sobj);
    *face = sf->face;
    *level = sf->level;
    return sf->tex->id;
}

ComClass* surface_class();
ComClass* texture_class(uint32_t kind);
uint32_t surface_object(Tex* t, uint32_t face, uint32_t level)
{
    uint32_t i = face * t->levels + level;
    if (!t->surf[i]) t->surf[i] = com_new(surface_class(), new Surf{t, face, level});
    return t->surf[i];
}

Tex* new_tex(Device* d, uint32_t kind, uint32_t w, uint32_t h, uint32_t depth, uint32_t levels, uint32_t usage, uint32_t fmt,
             uint32_t pool)
{
    if (!levels)
    {
        levels = 1;
        for (uint32_t m = w > h ? w : h; m > 1; m >>= 1) ++levels;
    }
    Tex* t = new Tex{g_next_id.fetch_add(1), kind, fmt, w ? w : 1, h ? h : 1, depth ? depth : 1, levels, usage, pool};
    t->dev = d;
    t->lv.resize(t->faces() * levels);
    t->surf.resize(t->faces() * levels);
    emit(OP_CREATE_TEXTURE, {t->id, kind, fmt, t->w, t->h, t->depth, levels, usage});
    return t;
}

uint32_t level_mem(Tex* t, uint32_t face, uint32_t level)
{
    Level& L = t->lv[face * t->levels + level];
    if (!L.mem)
    {
        uint32_t pitch, rows;
        uint32_t bytes = pal_d3d_format_bytes(t->fmt, t->lw(level), t->lh(level), &pitch, &rows) * t->ld(level);
        L.mem = (uint32_t)(uintptr_t)calloc(1, bytes ? bytes : 4);
        pal_mem_add(PAL_MEM_TEX, bytes);
        L.bytes = bytes; L.pitch = pitch; L.rows = rows;
    }
    return L.mem;
}

void upload_level(Tex* t, uint32_t face, uint32_t level, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    Level& L = t->lv[face * t->levels + level];
    if (!L.mem) return;
    uint32_t bb;
    bool block = is_block(t->fmt, &bb);
    if (block) { x &= ~3u; y &= ~3u; w = (w + 3) & ~3u; h = (h + 3) & ~3u; }
    uint32_t lw = t->lw(level), lh = t->lh(level);
    if (x >= lw || y >= lh) return;
    if (x + w > lw) w = lw - x;
    if (y + h > lh) h = lh - y;
    uint32_t row_bytes, rows;
    pal_d3d_format_bytes(t->fmt, w, h, &row_bytes, &rows);
    uint32_t first_row = block ? y / 4 : y, col = block ? (x / 4) * bb : x * bytes_per_pixel(t->fmt);
    uint32_t slices = t->ld(level);
    std::vector<uint8_t> data((size_t)row_bytes * rows * slices);
    for (uint32_t z = 0; z < slices; ++z)
        for (uint32_t r = 0; r < rows; ++r)
            memcpy(&data[((size_t)z * rows + r) * row_bytes],
                   (const void*)(uintptr_t)(L.mem + z * L.pitch * L.rows + (first_row + r) * L.pitch + col), row_bytes);
    emit(OP_UPLOAD_TEXTURE, {t->id, face, level, x, y, w, h, row_bytes, rows * slices}, data.data(), (uint32_t)data.size());
}

void destroy_tex(Tex* t)
{
    emit(OP_DESTROY, {t->id});
    for (Level& L : t->lv)
        if (L.mem) { free((void*)(uintptr_t)L.mem); pal_mem_add(PAL_MEM_TEX, -(int64_t)L.bytes); }
    t->lv.clear();
}

// ---- IUnknown-style release with destruction --------------------------------------------------------------------------
uint32_t release(uint32_t obj) { return M32(obj + 8) ? --M32(obj + 8) : 0; }

// ---- surfaces -----------------------------------------------------------------------------------------------------------
Surf* surf(FnvCpu& s) { return (Surf*)com_host(A(0)); }
void sf_Release(FnvCpu& s)
{
    Surf* sf = surf(s);
    uint32_t r = release(A(0));
    if (!r && sf->tex->kind >= 3) destroy_tex(sf->tex);     // standalone surfaces own their resource
    RET(r, 1);
}
void sf_GetDesc(FnvCpu& s)
{
    Surf* sf = surf(s);
    Tex* t = sf->tex;
    uint32_t d = A(1);
    M32(d) = t->fmt; M32(d + 4) = 1; M32(d + 8) = t->usage; M32(d + 12) = t->pool; M32(d + 16) = 0; M32(d + 20) = 0;
    M32(d + 24) = t->lw(sf->level); M32(d + 28) = t->lh(sf->level);
    RET(0, 2);
}
void lock_rect(Tex* t, uint32_t face, uint32_t level, uint32_t out, uint32_t rect, uint32_t flags)
{
    uint32_t mem = level_mem(t, face, level);
    Level& L = t->lv[face * t->levels + level];
    uint32_t x = 0, y = 0, w = t->lw(level), h = t->lh(level);
    if (rect) { x = M32(rect); y = M32(rect + 4); w = M32(rect + 8) - x; h = M32(rect + 12) - y; }
    L.locked = !(flags & 0x10); L.lx = x; L.ly = y; L.lw = w; L.lh = h;      // D3DLOCK_READONLY: no upload
    uint32_t bb;
    uint32_t off = is_block(t->fmt, &bb) ? (y / 4) * L.pitch + (x / 4) * bb : y * L.pitch + x * bytes_per_pixel(t->fmt);
    M32(out) = L.pitch;
    M32(out + 4) = mem + off;
}
void unlock_rect(Tex* t, uint32_t face, uint32_t level)
{
    Level& L = t->lv[face * t->levels + level];
    if (L.locked && t->kind != 4) upload_level(t, face, level, L.lx, L.ly, L.lw, L.lh);
    L.locked = false;
}
void sf_LockRect(FnvCpu& s) { Surf* sf = surf(s); lock_rect(sf->tex, sf->face, sf->level, A(1), A(2), A(3)); RET(0, 4); }
void sf_UnlockRect(FnvCpu& s) { Surf* sf = surf(s); unlock_rect(sf->tex, sf->face, sf->level); RET(0, 1); }
void sf_GetContainer(FnvCpu& s)
{
    Surf* sf = surf(s);
    if (sf->tex->kind < 3) { M32(A(2)) = sf->tex->obj; ++M32(sf->tex->obj + 8); RET(0, 3); }
    M32(A(2)) = 0;
    RET(0x80004002u, 3);
}
void any_GetDevice(FnvCpu& s);
void sf_GetDC(FnvCpu& s) { RET(D3DERR_INVALIDCALL, 2); }
void sf_ReleaseDC(FnvCpu& s) { RET(D3DERR_INVALIDCALL, 2); }
void res_GetType_surface(FnvCpu& s) { RET(1, 1); }
void res_ok1(FnvCpu& s) { RET(0, 1); }
void res_zero1(FnvCpu& s) { RET(0, 1); }
void res_ok2(FnvCpu& s) { RET(0, 2); }
void res_ok3(FnvCpu& s) { RET(0, 3); }
void res_ok5(FnvCpu& s) { RET(0, 5); }

ComClass* surface_class()
{
    static ComClass* c = com_class("IDirect3DSurface9", {
        {"Release", sf_Release}, {"GetDevice", any_GetDevice}, {"SetPrivateData", res_ok5}, {"GetPrivateData", [](FnvCpu& s) { hret(s, 0x88760866u, 4); }},
        {"FreePrivateData", res_ok2}, {"SetPriority", res_zero1}, {"GetPriority", res_zero1}, {"PreLoad", res_ok1},
        {"GetType", res_GetType_surface}, {"GetContainer", sf_GetContainer}, {"GetDesc", sf_GetDesc},
        {"LockRect", sf_LockRect}, {"UnlockRect", sf_UnlockRect}, {"GetDC", sf_GetDC}, {"ReleaseDC", sf_ReleaseDC}});
    return c;
}

// ---- textures (2D, cube, volume) --------------------------------------------------------------------------------------------
Tex* tex(FnvCpu& s) { return (Tex*)com_host(A(0)); }
void tx_Release(FnvCpu& s)
{
    uint32_t r = release(A(0));
    if (!r) destroy_tex(tex(s));
    RET(r, 1);
}
void tx_GetLevelCount(FnvCpu& s) { RET(tex(s)->levels, 1); }
void tx_GetType(FnvCpu& s) { Tex* t = tex(s); RET(t->kind == 1 ? 5 : t->kind == 2 ? 4 : 3, 1); }
void tx_GetLevelDesc(FnvCpu& s)
{
    Tex* t = tex(s);
    uint32_t l = A(1), d = A(2);
    if (l >= t->levels) RET(D3DERR_INVALIDCALL, 3);
    M32(d) = t->fmt; M32(d + 4) = t->kind == 2 ? 2 : 1; M32(d + 8) = t->usage; M32(d + 12) = t->pool;
    if (t->kind == 2) { M32(d + 16) = t->lw(l); M32(d + 20) = t->lh(l); M32(d + 24) = t->ld(l); }
    else { M32(d + 16) = 0; M32(d + 20) = 0; M32(d + 24) = t->lw(l); M32(d + 28) = t->lh(l); }
    RET(0, 3);
}
void tx_GetSurfaceLevel(FnvCpu& s)
{
    Tex* t = tex(s);
    if (A(1) >= t->levels) RET(D3DERR_INVALIDCALL, 3);
    uint32_t o = surface_object(t, 0, A(1));
    ++M32(o + 8);
    M32(A(2)) = o;
    RET(0, 3);
}
void tx_GetCubeMapSurface(FnvCpu& s)
{
    Tex* t = tex(s);
    if (A(2) >= t->levels || A(1) > 5) RET(D3DERR_INVALIDCALL, 4);
    uint32_t o = surface_object(t, A(1), A(2));
    ++M32(o + 8);
    M32(A(3)) = o;
    RET(0, 4);
}
void tx_LockRect(FnvCpu& s) { Tex* t = tex(s); if (A(1) >= t->levels) RET(D3DERR_INVALIDCALL, 5); lock_rect(t, 0, A(1), A(2), A(3), A(4)); RET(0, 5); }
void tx_UnlockRect(FnvCpu& s) { Tex* t = tex(s); if (A(1) < t->levels) unlock_rect(t, 0, A(1)); RET(0, 2); }
void cu_LockRect(FnvCpu& s) { Tex* t = tex(s); if (A(2) >= t->levels) RET(D3DERR_INVALIDCALL, 6); lock_rect(t, A(1), A(2), A(3), A(4), A(5)); RET(0, 6); }
void cu_UnlockRect(FnvCpu& s) { Tex* t = tex(s); if (A(2) < t->levels) unlock_rect(t, A(1), A(2)); RET(0, 3); }
void vt_LockBox(FnvCpu& s)
{
    Tex* t = tex(s);
    uint32_t l = A(1);
    if (l >= t->levels) RET(D3DERR_INVALIDCALL, 5);
    uint32_t mem = level_mem(t, 0, l);
    Level& L = t->lv[l];
    L.locked = !(A(4) & 0x10); L.lx = 0; L.ly = 0; L.lw = t->lw(l); L.lh = t->lh(l);
    M32(A(2)) = L.pitch; M32(A(2) + 4) = L.pitch * L.rows; M32(A(2) + 8) = mem;
    RET(0, 5);
}
void vt_UnlockBox(FnvCpu& s) { Tex* t = tex(s); if (A(1) < t->levels) unlock_rect(t, 0, A(1)); RET(0, 2); }
void tx_SetLOD(FnvCpu& s) { RET(0, 2); }
void tx_GenerateMipSubLevels(FnvCpu& s) { RET(0, 1); }
void tx_AddDirtyRect(FnvCpu& s) { RET(0, 2); }
void tx_AddDirtyRect3(FnvCpu& s) { RET(0, 3); }
void tx_SetAutoGen(FnvCpu& s) { RET(0, 2); }
void tx_GetAutoGen(FnvCpu& s) { RET(2, 1); }

ComClass* texture_class(uint32_t kind)
{
    static ComClass* c2 = com_class("IDirect3DTexture9", {
        {"Release", tx_Release}, {"GetDevice", any_GetDevice}, {"SetPrivateData", res_ok5}, {"FreePrivateData", res_ok2},
        {"SetPriority", res_zero1}, {"GetPriority", res_zero1}, {"PreLoad", res_ok1}, {"GetType", tx_GetType},
        {"SetLOD", tx_SetLOD}, {"GetLOD", res_zero1}, {"GetLevelCount", tx_GetLevelCount},
        {"SetAutoGenFilterType", tx_SetAutoGen}, {"GetAutoGenFilterType", tx_GetAutoGen},
        {"GenerateMipSubLevels", tx_GenerateMipSubLevels}, {"GetLevelDesc", tx_GetLevelDesc},
        {"GetSurfaceLevel", tx_GetSurfaceLevel}, {"LockRect", tx_LockRect}, {"UnlockRect", tx_UnlockRect},
        {"AddDirtyRect", tx_AddDirtyRect}});
    static ComClass* cc = com_class("IDirect3DCubeTexture9", {
        {"Release", tx_Release}, {"GetDevice", any_GetDevice}, {"SetPrivateData", res_ok5}, {"FreePrivateData", res_ok2},
        {"SetPriority", res_zero1}, {"GetPriority", res_zero1}, {"PreLoad", res_ok1}, {"GetType", tx_GetType},
        {"SetLOD", tx_SetLOD}, {"GetLOD", res_zero1}, {"GetLevelCount", tx_GetLevelCount},
        {"SetAutoGenFilterType", tx_SetAutoGen}, {"GetAutoGenFilterType", tx_GetAutoGen},
        {"GenerateMipSubLevels", tx_GenerateMipSubLevels}, {"GetLevelDesc", tx_GetLevelDesc},
        {"GetCubeMapSurface", tx_GetCubeMapSurface}, {"LockRect", cu_LockRect}, {"UnlockRect", cu_UnlockRect},
        {"AddDirtyRect", tx_AddDirtyRect3}});
    static ComClass* cv = com_class("IDirect3DVolumeTexture9", {
        {"Release", tx_Release}, {"GetDevice", any_GetDevice}, {"SetPrivateData", res_ok5}, {"FreePrivateData", res_ok2},
        {"SetPriority", res_zero1}, {"GetPriority", res_zero1}, {"PreLoad", res_ok1}, {"GetType", tx_GetType},
        {"SetLOD", tx_SetLOD}, {"GetLOD", res_zero1}, {"GetLevelCount", tx_GetLevelCount},
        {"SetAutoGenFilterType", tx_SetAutoGen}, {"GetAutoGenFilterType", tx_GetAutoGen},
        {"GenerateMipSubLevels", tx_GenerateMipSubLevels}, {"GetLevelDesc", tx_GetLevelDesc},
        {"LockBox", vt_LockBox}, {"UnlockBox", vt_UnlockBox}, {"AddDirtyBox", tx_AddDirtyRect}});
    return kind == 1 ? cc : kind == 2 ? cv : c2;
}

// ---- buffers --------------------------------------------------------------------------------------------------------------
Buf* bufo(FnvCpu& s) { return (Buf*)com_host(A(0)); }
void bf_Release(FnvCpu& s)
{
    Buf* b = bufo(s);
    uint32_t r = release(A(0));
    if (!r) { emit(OP_DESTROY, {b->id}); free((void*)(uintptr_t)b->mem); b->mem = 0; pal_mem_add(PAL_MEM_BUF, -(int64_t)b->size); }
    RET(r, 1);
}
void bf_Lock(FnvCpu& s)
{
    Buf* b = bufo(s);
    uint32_t off = A(1), size = A(2), flags = A(4);
    if (!size || off + size > b->size) { size = b->size - (off < b->size ? off : 0); }
    b->locked = !(flags & 0x10);
    b->lo = off; b->lsize = size;
    M32(A(3)) = b->mem + off;
    RET(0, 5);
}
void bf_Unlock(FnvCpu& s)
{
    Buf* b = bufo(s);
    if (b->locked && b->lsize)
        emit(OP_UPLOAD_BUFFER, {b->id, b->lo, b->lsize}, (const void*)(uintptr_t)(b->mem + b->lo), b->lsize);
    b->locked = false;
    RET(0, 1);
}
void bf_GetDesc(FnvCpu& s)
{
    Buf* b = bufo(s);
    uint32_t d = A(1);
    M32(d) = b->fmt; M32(d + 4) = b->kind ? 7 : 6; M32(d + 8) = b->usage; M32(d + 12) = b->pool; M32(d + 16) = b->size;
    if (!b->kind) M32(d + 20) = b->fvf;
    RET(0, 2);
}
void bf_GetTypeV(FnvCpu& s) { RET(6, 1); }
void bf_GetTypeI(FnvCpu& s) { RET(7, 1); }
ComClass* buffer_class(bool index)
{
    static ComClass* cv = com_class("IDirect3DVertexBuffer9", {
        {"Release", bf_Release}, {"GetDevice", any_GetDevice}, {"SetPrivateData", res_ok5}, {"FreePrivateData", res_ok2},
        {"SetPriority", res_zero1}, {"GetPriority", res_zero1}, {"PreLoad", res_ok1}, {"GetType", bf_GetTypeV},
        {"Lock", bf_Lock}, {"Unlock", bf_Unlock}, {"GetDesc", bf_GetDesc}});
    static ComClass* ci = com_class("IDirect3DIndexBuffer9", {
        {"Release", bf_Release}, {"GetDevice", any_GetDevice}, {"SetPrivateData", res_ok5}, {"FreePrivateData", res_ok2},
        {"SetPriority", res_zero1}, {"GetPriority", res_zero1}, {"PreLoad", res_ok1}, {"GetType", bf_GetTypeI},
        {"Lock", bf_Lock}, {"Unlock", bf_Unlock}, {"GetDesc", bf_GetDesc}});
    return index ? ci : cv;
}

// ---- shaders, declarations, queries, state blocks, swap chain -------------------------------------------------------------
void sh_GetFunction(FnvCpu& s)
{
    Shader* sh = (Shader*)com_host(A(0));
    if (A(1)) memcpy(gbuf(A(1)), (const void*)(uintptr_t)sh->tokens, sh->bytes);
    M32(A(2)) = sh->bytes;
    RET(0, 3);
}
void sh_Release(FnvCpu& s)
{
    Shader* sh = (Shader*)com_host(A(0));
    uint32_t r = release(A(0));
    if (!r) emit(OP_DESTROY, {sh->id});
    RET(r, 1);
}
void dc_GetDeclaration(FnvCpu& s)
{
    Decl* d = (Decl*)com_host(A(0));
    if (A(1)) memcpy(gbuf(A(1)), (const void*)(uintptr_t)d->mem, (d->count + 1) * 8);
    M32(A(2)) = d->count + 1;
    RET(0, 3);
}
void q_GetType(FnvCpu& s) { RET(((Query*)com_host(A(0)))->type, 1); }
void q_GetDataSize(FnvCpu& s)
{
    uint32_t t = ((Query*)com_host(A(0)))->type;
    RET(t == 10 ? 8 : t == 11 ? 4 : t == 12 ? 8 : 4, 1);
}
void q_Issue(FnvCpu& s) { RET(0, 2); }
void q_GetData(FnvCpu& s)
{
    Query* q = (Query*)com_host(A(0));
    uint32_t p = A(1), size = A(2);
    if (p && size)
    {
        memset(gbuf(p), 0, size);
        if (q->type == 9) M32(p) = 100000;                          // occlusion: visible
        else if (q->type == 8) M32(p) = 1;                          // event: done
        else if (q->type == 12 && size >= 8) M32(p) = 1000000;      // timestamp frequency
    }
    RET(0, 4);
}
void sb_ok(FnvCpu& s) { RET(0, 1); }
void any_GetDevice(FnvCpu& s)
{
    // one device per game: every resource answers with it
    M32(A(1)) = g_device_obj;
    ++M32(g_device_obj + 8);
    RET(0, 2);
}
} // namespace

namespace {
Device* g_dev;

// ---- device -------------------------------------------------------------------------------------------------------------
Device* D(FnvCpu& s) { return dev_of(A(0)); }
void dv_TestCooperativeLevel(FnvCpu& s) { RET(0, 1); }
void dv_GetAvailableTextureMem(FnvCpu& s) { RET(1024u << 20, 1); }
void dv_GetDirect3D(FnvCpu& s) { Device* d = D(s); M32(A(1)) = d->d3d; ++M32(d->d3d + 8); RET(0, 2); }
void dv_GetDeviceCaps(FnvCpu& s) { memcpy(gbuf(A(1)), kCaps, sizeof kCaps); RET(0, 2); }
void dv_GetDisplayMode(FnvCpu& s)
{
    Device* d = D(s);
    uint32_t m = A(2);
    M32(m) = d->bb_w; M32(m + 4) = d->bb_h; M32(m + 8) = 60; M32(m + 12) = 22;
    RET(0, 3);
}
void dv_GetCreationParameters(FnvCpu& s)
{
    Device* d = D(s);
    uint32_t p = A(1);
    M32(p) = 0; M32(p + 4) = 1; M32(p + 8) = d->hwnd; M32(p + 12) = d->behavior;
    RET(0, 2);
}
void dv_SetCursorProperties(FnvCpu& s) { RET(0, 4); }
void dv_SetCursorPosition(FnvCpu& s) { RET(0, 4); }
void dv_ShowCursor(FnvCpu& s) { RET(0, 2); }
void dv_GetSwapChain(FnvCpu& s) { Device* d = D(s); M32(A(2)) = d->swapchain; ++M32(d->swapchain + 8); RET(0, 3); }
void dv_GetNumberOfSwapChains(FnvCpu& s) { RET(1, 1); }
void make_backbuffer(Device* d)
{
    d->bb_w = d->pp[0] ? d->pp[0] : 1280;
    d->bb_h = d->pp[1] ? d->pp[1] : 720;
    d->bb_fmt = d->pp[2] ? d->pp[2] : 22;
    if (!d->backbuffer)
    {
        d->backbuffer = new Tex{1, 3, d->bb_fmt, d->bb_w, d->bb_h, 1, 1, 1, 0};
        d->backbuffer->dev = d;
        d->backbuffer->lv.resize(1);
        d->backbuffer->surf.resize(1);
        d->bb_surf = surface_object(d->backbuffer, 0, 0);
    }
    d->backbuffer->w = d->bb_w;
    d->backbuffer->h = d->bb_h;
    emit(OP_RESIZE, {d->bb_w, d->bb_h});
    if (d->pp[9] && !d->autodepth)                            // EnableAutoDepthStencil
    {
        d->autodepth = new_tex(d, 4, d->bb_w, d->bb_h, 1, 1, 2, d->pp[10], 0);
        d->ad_surf = surface_object(d->autodepth, 0, 0);
        ++M32(d->ad_surf + 8);
    }
    d->rt[0] = d->bb_surf;
    d->ds = d->ad_surf;
    emit(OP_SET_TARGET, {0, 1, 0, 0});
    emit(OP_SET_DEPTH, {d->autodepth ? d->autodepth->id : 0});
    d->viewport[0] = 0; d->viewport[1] = 0; d->viewport[2] = d->bb_w; d->viewport[3] = d->bb_h;
    d->viewport[4] = fbits(0.f); d->viewport[5] = fbits(1.f);
}
void dv_Reset(FnvCpu& s)
{
    Device* d = D(s);
    memcpy(d->pp, gbuf(A(1)), 56);
    make_backbuffer(d);
    RET(0, 2);
}
void dv_Present(FnvCpu& s)
{
    Device* d = D(s);
    ++d->frame;
    if (g_ring.enabled)
    {
        emit(OP_PRESENT, {d->frame});
        __atomic_add_fetch(&g_ring.frames_sent, 1, __ATOMIC_RELEASE);
        // stay at most one frame ahead of the renderer: the page releases one frame per screen refresh
        // (web/recomp/index.html pace()), and every frame queued beyond that is 17 ms of late input
        auto t0 = std::chrono::steady_clock::now();
        // (woken by the page the moment it shows a frame: a timed sleep here can overrun by a whole operating
        // system timer tick, 15.6 ms on Windows, which is a late frame)
        for (;;)
        {
            uint32_t done = __atomic_load_n(&g_ring.frames_done, __ATOMIC_ACQUIRE);
            if (g_ring.frames_sent - done <= 1) break;
#ifdef __EMSCRIPTEN__
            emscripten_futex_wait(&g_ring.frames_done, done, 4.0);
#else
            std::this_thread::sleep_for(std::chrono::microseconds(500));
#endif
        }
        auto t1 = std::chrono::steady_clock::now();
        static auto last = t1;
        g_present_stats[0]++;
        g_present_stats[1] += (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(t1 - last).count();
        g_present_stats[2] += (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
        last = t1;
    }
    if (d->frame == 1 || d->frame % 600 == 0)
    {
        char buf[96];
        snprintf(buf, sizeof buf, "[d3d9] frame %u presented%c", d->frame, 10);
        fnv_host_log(buf);
    }
    RET(0, 5);
}
void dv_GetBackBuffer(FnvCpu& s) { Device* d = D(s); M32(A(4)) = d->bb_surf; ++M32(d->bb_surf + 8); RET(0, 5); }
void dv_GetRasterStatus(FnvCpu& s) { M32(A(2)) = 0; M32(A(2) + 4) = 0; RET(0, 3); }
void dv_SetDialogBoxMode(FnvCpu& s) { RET(0, 2); }
void dv_SetGammaRamp(FnvCpu& s) { RET(0, 4); }
void dv_GetGammaRamp(FnvCpu& s)
{
    uint16_t* r = (uint16_t*)gbuf(A(2));
    for (int c = 0; c < 3; ++c)
        for (int i = 0; i < 256; ++i) r[c * 256 + i] = (uint16_t)(i * 257);
    RET(0, 3);
}
uint32_t new_texture_obj(Device* d, uint32_t kind, uint32_t w, uint32_t h, uint32_t depth, uint32_t levels, uint32_t usage,
                         uint32_t fmt, uint32_t pool)
{
    Tex* t = new_tex(d, kind, w, h, depth, levels, usage, fmt, pool);
    t->obj = com_new(texture_class(kind), t);
    return t->obj;
}
void dv_CreateTexture(FnvCpu& s)
{
    Device* d = D(s);
    uint32_t usage = A(4);
    M32(A(7)) = new_texture_obj(d, (usage & 1) ? 0 : 0, A(1), A(2), 1, (usage & 0x400) ? 0 : A(3), usage, A(5), A(6));
    if (A(8)) M32(A(8)) = 0;
    RET(0, 9);
}
void dv_CreateVolumeTexture(FnvCpu& s)
{
    Device* d = D(s);
    M32(A(8)) = new_texture_obj(d, 2, A(1), A(2), A(3), A(4), A(5), A(6), A(7));
    RET(0, 10);
}
void dv_CreateCubeTexture(FnvCpu& s)
{
    Device* d = D(s);
    M32(A(6)) = new_texture_obj(d, 1, A(1), A(1), 1, A(2), A(3), A(4), A(5));
    RET(0, 8);
}
uint32_t new_buffer(Device* d, uint32_t size, uint32_t kind, uint32_t usage, uint32_t fvf, uint32_t fmt, uint32_t pool)
{
    Buf* b = new Buf{g_next_id.fetch_add(1), size, kind, usage, pool, fvf, fmt, 0};
    b->dev = d;
    b->mem = (uint32_t)(uintptr_t)calloc(1, size ? size : 4);
    pal_mem_add(PAL_MEM_BUF, size);
    b->obj = com_new(buffer_class(kind != 0), b);
    emit(OP_CREATE_BUFFER, {b->id, size, kind});
    return b->obj;
}
void dv_CreateVertexBuffer(FnvCpu& s) { M32(A(5)) = new_buffer(D(s), A(1), 0, A(2), A(3), 100, A(4)); RET(0, 7); }
void dv_CreateIndexBuffer(FnvCpu& s)
{
    uint32_t fmt = A(3);
    M32(A(5)) = new_buffer(D(s), A(1), fmt == 102 ? 2 : 1, A(2), 0, fmt, A(4));
    RET(0, 7);
}
uint32_t standalone_surface(Device* d, uint32_t kind, uint32_t w, uint32_t h, uint32_t fmt, uint32_t usage, uint32_t pool)
{
    Tex* t = new_tex(d, kind, w, h, 1, 1, usage, fmt, pool);
    t->obj = surface_object(t, 0, 0);
    return t->obj;
}
void dv_CreateRenderTarget(FnvCpu& s) { M32(A(7)) = standalone_surface(D(s), 3, A(1), A(2), A(3), 1, 0); RET(0, 9); }
void dv_CreateDepthStencilSurface(FnvCpu& s) { M32(A(7)) = standalone_surface(D(s), 4, A(1), A(2), A(3), 2, 0); RET(0, 9); }
void dv_CreateOffscreenPlainSurface(FnvCpu& s) { M32(A(5)) = standalone_surface(D(s), 0, A(1), A(2), A(3), 0, A(4)); RET(0, 7); }
void copy_surface_data(uint32_t src, uint32_t dst)
{
    Surf* a = (Surf*)com_host(src);
    Surf* b = (Surf*)com_host(dst);
    uint32_t ma = level_mem(a->tex, a->face, a->level), mb = level_mem(b->tex, b->face, b->level);
    Level& La = a->tex->lv[a->face * a->tex->levels + a->level];
    Level& Lb = b->tex->lv[b->face * b->tex->levels + b->level];
    memcpy((void*)(uintptr_t)mb, (void*)(uintptr_t)ma, La.bytes < Lb.bytes ? La.bytes : Lb.bytes);
    upload_level(b->tex, b->face, b->level, 0, 0, b->tex->lw(b->level), b->tex->lh(b->level));
}
void dv_UpdateSurface(FnvCpu& s) { copy_surface_data(A(1), A(3)); RET(0, 5); }
void dv_UpdateTexture(FnvCpu& s)
{
    Tex* a = (Tex*)com_host(A(1));
    Tex* b = (Tex*)com_host(A(2));
    for (uint32_t f = 0; f < a->faces() && f < b->faces(); ++f)
        for (uint32_t l = 0; l < a->levels && l < b->levels; ++l)
        {
            uint32_t ma = level_mem(a, f, l), mb = level_mem(b, f, l);
            Level& La = a->lv[f * a->levels + l];
            memcpy((void*)(uintptr_t)mb, (void*)(uintptr_t)ma, La.bytes);
            upload_level(b, f, l, 0, 0, b->lw(l), b->lh(l));
        }
    RET(0, 3);
}
void dv_GetRenderTargetData(FnvCpu& s) { RET(0, 3); }       // read-back: the system-memory copy keeps its contents
void dv_GetFrontBufferData(FnvCpu& s) { RET(0, 3); }
void rect_words(uint32_t r, uint32_t out[4])
{
    if (r) { out[0] = M32(r); out[1] = M32(r + 4); out[2] = M32(r + 8); out[3] = M32(r + 12); }
    else out[0] = out[1] = out[2] = out[3] = 0xffffffffu;
}
void dv_StretchRect(FnvCpu& s)
{
    uint32_t sf, sl, df, dl, sr[4], dr[4];
    uint32_t si = tex_id_of_surface(A(1), &sf, &sl), di = tex_id_of_surface(A(3), &df, &dl);
    rect_words(A(2), sr);
    rect_words(A(4), dr);
    emit(OP_STRETCH, {si, sf, sl, sr[0], sr[1], sr[2], sr[3], di, df, dl, dr[0], dr[1], dr[2], dr[3], A(5)});
    RET(0, 6);
}
void dv_ColorFill(FnvCpu& s)
{
    uint32_t f, l, r[4];
    uint32_t id = tex_id_of_surface(A(1), &f, &l);
    rect_words(A(2), r);
    emit(OP_COLOR_FILL, {id, f, l, r[0], r[1], r[2], r[3], A(3)});
    RET(0, 4);
}
void dv_SetRenderTarget(FnvCpu& s)
{
    Device* d = D(s);
    uint32_t i = A(1);
    if (i > 3) RET(D3DERR_INVALIDCALL, 3);
    uint32_t o = A(2);
    if (o) ++M32(o + 8);
    if (d->rt[i]) release(d->rt[i]);
    d->rt[i] = o;
    uint32_t f, l, id = tex_id_of_surface(o, &f, &l);
    emit(OP_SET_TARGET, {i, id, f, l});
    if (i == 0 && o)
    {                                                          // D3D9 resets the viewport to the new target
        Surf* sf = (Surf*)com_host(o);
        d->viewport[0] = 0; d->viewport[1] = 0; d->viewport[2] = sf->tex->lw(sf->level); d->viewport[3] = sf->tex->lh(sf->level);
        d->viewport[4] = fbits(0.f); d->viewport[5] = fbits(1.f);
        emit(OP_VIEWPORT, {d->viewport[0], d->viewport[1], d->viewport[2], d->viewport[3], d->viewport[4], d->viewport[5]});
    }
    RET(0, 3);
}
void dv_GetRenderTarget(FnvCpu& s)
{
    Device* d = D(s);
    uint32_t o = A(1) < 4 ? d->rt[A(1)] : 0;
    M32(A(2)) = o;
    if (!o) RET(0x8876086Cu, 3);
    ++M32(o + 8);
    RET(0, 3);
}
void dv_SetDepthStencilSurface(FnvCpu& s)
{
    Device* d = D(s);
    uint32_t o = A(1);
    if (o) ++M32(o + 8);
    if (d->ds) release(d->ds);
    d->ds = o;
    uint32_t f, l;
    emit(OP_SET_DEPTH, {tex_id_of_surface(o, &f, &l)});
    RET(0, 2);
}
void dv_GetDepthStencilSurface(FnvCpu& s)
{
    Device* d = D(s);
    M32(A(1)) = d->ds;
    if (!d->ds) RET(0x8876086Cu, 2);
    ++M32(d->ds + 8);
    RET(0, 2);
}
void dv_ok1(FnvCpu& s) { RET(0, 1); }
void dv_Clear(FnvCpu& s) { emit(OP_CLEAR, {A(3), A(4), A(5), A(6)}); RET(0, 7); }
void dv_SetTransform(FnvCpu& s)
{
    Device* d = D(s);
    std::vector<uint8_t>& m = d->transforms[A(1)];
    m.assign(gbuf(A(2)), gbuf(A(2)) + 64);
    emit(OP_SET_TRANSFORM, {A(1)}, gbuf(A(2)), 64);
    RET(0, 3);
}
void dv_GetTransform(FnvCpu& s)
{
    Device* d = D(s);
    auto it = d->transforms.find(A(1));
    if (it == d->transforms.end())
    {
        float id[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        memcpy(gbuf(A(2)), id, 64);
    }
    else memcpy(gbuf(A(2)), it->second.data(), 64);
    RET(0, 3);
}
void dv_MultiplyTransform(FnvCpu& s) { RET(0, 3); }
void dv_SetViewport(FnvCpu& s)
{
    Device* d = D(s);
    memcpy(d->viewport, gbuf(A(1)), 24);
    emit(OP_VIEWPORT, {d->viewport[0], d->viewport[1], d->viewport[2], d->viewport[3], d->viewport[4], d->viewport[5]});
    RET(0, 2);
}
void dv_GetViewport(FnvCpu& s) { memcpy(gbuf(A(1)), D(s)->viewport, 24); RET(0, 2); }
void dv_SetMaterial(FnvCpu& s) { memcpy(D(s)->material, gbuf(A(1)), 68); RET(0, 2); }
void dv_GetMaterial(FnvCpu& s) { memcpy(gbuf(A(1)), D(s)->material, 68); RET(0, 2); }
void dv_SetLight(FnvCpu& s) { D(s)->lights[A(1)].assign(gbuf(A(2)), gbuf(A(2)) + 104); RET(0, 3); }
void dv_GetLight(FnvCpu& s)
{
    auto& l = D(s)->lights;
    auto it = l.find(A(1));
    if (it == l.end()) RET(D3DERR_INVALIDCALL, 3);
    memcpy(gbuf(A(2)), it->second.data(), 104);
    RET(0, 3);
}
void dv_LightEnable(FnvCpu& s) { D(s)->light_enable[A(1)] = A(2); RET(0, 3); }
void dv_GetLightEnable(FnvCpu& s) { M32(A(2)) = D(s)->light_enable[A(1)]; RET(0, 3); }
void dv_SetClipPlane(FnvCpu& s) { if (A(1) < 6) memcpy(D(s)->clip[A(1)], gbuf(A(2)), 16); RET(0, 3); }
void dv_GetClipPlane(FnvCpu& s) { if (A(1) < 6) memcpy(gbuf(A(2)), D(s)->clip[A(1)], 16); RET(0, 3); }
void dv_SetRenderState(FnvCpu& s)
{
    Device* d = D(s);
    uint32_t st = A(1);
    if (st < 256)
    {
        if (d->rs[st] != A(2) || true) emit(OP_RENDER_STATE, {st, A(2)});
        d->rs[st] = A(2);
    }
    RET(0, 3);
}
void dv_GetRenderState(FnvCpu& s) { Device* d = D(s); M32(A(2)) = A(1) < 256 ? d->rs[A(1)] : 0; RET(0, 3); }
uint32_t new_block(Device* d)
{
    static ComClass* c = com_class("IDirect3DStateBlock9", {
        {"GetDevice", any_GetDevice}, {"Capture", sb_ok}, {"Apply", sb_ok}});
    Block* b = new Block{0, d};
    b->obj = com_new(c, b);
    return b->obj;
}
void dv_CreateStateBlock(FnvCpu& s) { M32(A(2)) = new_block(D(s)); RET(0, 3); }
void dv_BeginStateBlock(FnvCpu& s) { D(s)->recording = true; RET(0, 1); }
void dv_EndStateBlock(FnvCpu& s) { D(s)->recording = false; M32(A(1)) = new_block(D(s)); RET(0, 2); }
void dv_SetClipStatus(FnvCpu& s) { RET(0, 2); }
void dv_GetClipStatus(FnvCpu& s) { memset(gbuf(A(1)), 0, 24); RET(0, 2); }
uint32_t stage_index(uint32_t st) { return st >= 257 && st <= 260 ? 16 + (st - 257) : st < 16 ? st : 0xffffffffu; }
void dv_GetTexture(FnvCpu& s)
{
    Device* d = D(s);
    uint32_t i = stage_index(A(1));
    uint32_t o = i < 20 ? d->tex[i] : 0;
    M32(A(2)) = o;
    if (o) ++M32(o + 8);
    RET(0, 3);
}
void dv_SetTexture(FnvCpu& s)
{
    Device* d = D(s);
    uint32_t i = stage_index(A(1));
    if (i >= 20) RET(D3DERR_INVALIDCALL, 3);
    uint32_t o = A(2);
    if (o) ++M32(o + 8);
    if (d->tex[i]) { uint32_t old = d->tex[i]; if (!release(old)) { Tex* t = (Tex*)com_host(old); destroy_tex(t); } }
    d->tex[i] = o;
    emit(OP_SET_TEXTURE, {i, o ? ((Tex*)com_host(o))->id : 0});
    RET(0, 3);
}
void dv_GetTextureStageState(FnvCpu& s) { Device* d = D(s); M32(A(3)) = A(1) < 8 && A(2) < 33 ? d->tss[A(1)][A(2)] : 0; RET(0, 4); }
void dv_SetTextureStageState(FnvCpu& s)
{
    Device* d = D(s);
    if (A(1) < 8 && A(2) < 33) { d->tss[A(1)][A(2)] = A(3); emit(OP_TEX_STAGE_STATE, {A(1), A(2), A(3)}); }
    RET(0, 4);
}
void dv_GetSamplerState(FnvCpu& s) { Device* d = D(s); uint32_t i = stage_index(A(1)); M32(A(3)) = i < 20 && A(2) < 14 ? d->ss[i][A(2)] : 0; RET(0, 4); }
void dv_SetSamplerState(FnvCpu& s)
{
    Device* d = D(s);
    uint32_t i = stage_index(A(1));
    if (i < 20 && A(2) < 14) { d->ss[i][A(2)] = A(3); emit(OP_SAMPLER_STATE, {i, A(2), A(3)}); }
    RET(0, 4);
}
void dv_ValidateDevice(FnvCpu& s) { if (A(1)) M32(A(1)) = 1; RET(0, 2); }
void dv_ok3(FnvCpu& s) { RET(0, 3); }
void dv_ok2(FnvCpu& s) { RET(0, 2); }
void dv_zero1(FnvCpu& s) { RET(0, 1); }
void dv_SetScissorRect(FnvCpu& s)
{
    Device* d = D(s);
    memcpy(d->scissor, gbuf(A(1)), 16);
    emit(OP_SCISSOR, {d->scissor[0], d->scissor[1], d->scissor[2], d->scissor[3]});
    RET(0, 2);
}
void dv_GetScissorRect(FnvCpu& s) { memcpy(gbuf(A(1)), D(s)->scissor, 16); RET(0, 2); }
uint32_t prim_vertices(uint32_t type, uint32_t count)
{
    switch (type)
    {
    case 1: return count;                // point list
    case 2: return count * 2;            // line list
    case 3: return count + 1;            // line strip
    case 4: return count * 3;            // triangle list
    case 5: case 6: return count + 2;    // strip, fan
    default: return 0;
    }
}
void dv_DrawPrimitive(FnvCpu& s) { emit(OP_DRAW, {A(1), A(2), A(3)}); RET(0, 4); }
void dv_DrawIndexedPrimitive(FnvCpu& s) { emit(OP_DRAW_INDEXED, {A(1), A(2), A(3), A(4), A(5), A(6)}); RET(0, 7); }
void dv_DrawPrimitiveUP(FnvCpu& s)
{
    uint32_t type = A(1), count = A(2), data = A(3), stride = A(4);
    uint32_t bytes = prim_vertices(type, count) * stride;
    emit(OP_DRAW_UP, {type, count, stride, bytes}, (const void*)(uintptr_t)data, bytes);
    // D3D9 unsets stream 0 after an UP draw
    Device* d = D(s);
    d->stream[0] = 0; d->stream_off[0] = 0; d->stream_stride[0] = 0;
    RET(0, 5);
}
void dv_DrawIndexedPrimitiveUP(FnvCpu& s)
{
    uint32_t type = A(1), minidx = A(2), nverts = A(3), count = A(4), idx = A(5), fmt = A(6), verts = A(7), stride = A(8);
    uint32_t isz = fmt == 102 ? 4 : 2;
    uint32_t ibytes = prim_vertices(type, count) * isz, vbytes = (minidx + nverts) * stride;
    std::vector<uint8_t> data(((ibytes + 3) & ~3u) + vbytes);
    memcpy(data.data(), (const void*)(uintptr_t)idx, ibytes);
    memcpy(data.data() + ((ibytes + 3) & ~3u), (const void*)(uintptr_t)verts, vbytes);
    emit(OP_DRAW_INDEXED_UP, {type, minidx, nverts, count, isz == 4, stride, ibytes, vbytes}, data.data(), (uint32_t)data.size());
    Device* d = D(s);
    d->stream[0] = 0; d->ib = 0;
    RET(0, 9);
}
void dv_ProcessVertices(FnvCpu& s) { RET(D3DERR_INVALIDCALL, 7); }
void dv_CreateVertexDeclaration(FnvCpu& s)
{
    static ComClass* c = com_class("IDirect3DVertexDeclaration9", {{"GetDevice", any_GetDevice}, {"GetDeclaration", dc_GetDeclaration}});
    uint32_t e = A(1), n = 0;
    while (M16(e + n * 8) != 0xff && n < 64) ++n;
    Decl* d = new Decl{g_next_id.fetch_add(1), n, 0, 0, D(s)};
    d->mem = (uint32_t)(uintptr_t)malloc((n + 1) * 8);
    memcpy((void*)(uintptr_t)d->mem, gbuf(e), (n + 1) * 8);
    d->obj = com_new(c, d);
    emit(OP_CREATE_DECL, {d->id, n}, gbuf(e), n * 8);
    M32(A(2)) = d->obj;
    RET(0, 3);
}
void dv_SetVertexDeclaration(FnvCpu& s)
{
    Device* d = D(s);
    d->decl = A(1);
    if (A(1)) d->fvf = 0;
    emit(OP_SET_DECL, {A(1) ? ((Decl*)com_host(A(1)))->id : 0});
    RET(0, 2);
}
void dv_GetVertexDeclaration(FnvCpu& s) { Device* d = D(s); M32(A(1)) = d->decl; if (d->decl) ++M32(d->decl + 8); RET(0, 2); }
void dv_SetFVF(FnvCpu& s) { Device* d = D(s); d->fvf = A(1); d->decl = 0; emit(OP_SET_FVF, {A(1)}); RET(0, 2); }
void dv_GetFVF(FnvCpu& s) { M32(A(1)) = D(s)->fvf; RET(0, 2); }

// SHA-1 of a shader's token stream through its end token (the browser's WGSL bundle is keyed by it)
void sha1(const uint8_t* p, uint32_t n, uint32_t out[5])
{
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    std::vector<uint8_t> m(p, p + n);
    uint64_t bits = (uint64_t)n * 8;
    m.push_back(0x80);
    while (m.size() % 64 != 56) m.push_back(0);
    for (int i = 7; i >= 0; --i) m.push_back((uint8_t)(bits >> (i * 8)));
    for (size_t c = 0; c < m.size(); c += 64)
    {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = ((uint32_t)m[c + 4 * i] << 24) | ((uint32_t)m[c + 4 * i + 1] << 16) | ((uint32_t)m[c + 4 * i + 2] << 8) | m[c + 4 * i + 3];
        for (int i = 16; i < 80; ++i) { uint32_t v = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16]; w[i] = (v << 1) | (v >> 31); }
        uint32_t a = h[0], b = h[1], cc = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i)
        {
            uint32_t f, k;
            if (i < 20) { f = (b & cc) | (~b & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ cc ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & cc) | (b & d) | (cc & d); k = 0x8F1BBCDC; }
            else { f = b ^ cc ^ d; k = 0xCA62C1D6; }
            uint32_t t = ((a << 5) | (a >> 27)) + f + e + k + w[i];
            e = d; d = cc; cc = (b << 30) | (b >> 2); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += cc; h[3] += d; h[4] += e;
    }
    memcpy(out, h, 20);
}
uint32_t new_shader(Device* d, uint32_t stage, uint32_t tokens)
{
    static ComClass* cv = com_class("IDirect3DVertexShader9", {{"Release", sh_Release}, {"GetDevice", any_GetDevice}, {"GetFunction", sh_GetFunction}});
    static ComClass* cp = com_class("IDirect3DPixelShader9", {{"Release", sh_Release}, {"GetDevice", any_GetDevice}, {"GetFunction", sh_GetFunction}});
    uint32_t n = 1;
    while (n < (1u << 18) && M32(tokens + (n - 1) * 4) != 0x0000FFFFu) ++n;
    Shader* sh = new Shader{g_next_id.fetch_add(1), stage, 0, n * 4, 0, d};
    sh->tokens = (uint32_t)(uintptr_t)malloc(n * 4);
    memcpy((void*)(uintptr_t)sh->tokens, gbuf(tokens), n * 4);
    uint32_t h[5];
    sha1((const uint8_t*)gbuf(tokens), n * 4, h);
    sh->obj = com_new(stage ? cp : cv, sh);
    emit(OP_CREATE_SHADER, {sh->id, stage, h[0], h[1], h[2], h[3], h[4], n * 4}, gbuf(tokens), n * 4);
    return sh->obj;
}
void dv_CreateVertexShader(FnvCpu& s) { M32(A(2)) = new_shader(D(s), 0, A(1)); RET(0, 3); }
void dv_CreatePixelShader(FnvCpu& s) { M32(A(2)) = new_shader(D(s), 1, A(1)); RET(0, 3); }
void set_shader(FnvCpu& s, uint32_t stage)
{
    Device* d = D(s);
    (stage ? d->ps : d->vs) = A(1);
    emit(OP_SET_SHADER, {stage, A(1) ? ((Shader*)com_host(A(1)))->id : 0});
}
void dv_SetVertexShader(FnvCpu& s) { set_shader(s, 0); RET(0, 2); }
void dv_SetPixelShader(FnvCpu& s) { set_shader(s, 1); RET(0, 2); }
void dv_GetVertexShader(FnvCpu& s) { Device* d = D(s); M32(A(1)) = d->vs; if (d->vs) ++M32(d->vs + 8); RET(0, 2); }
void dv_GetPixelShader(FnvCpu& s) { Device* d = D(s); M32(A(1)) = d->ps; if (d->ps) ++M32(d->ps + 8); RET(0, 2); }
void const_f(FnvCpu& s, uint32_t stage, bool set)
{
    Device* d = D(s);
    float* arr = stage ? d->psf : d->vsf;
    uint32_t max = stage ? 224 : 256, start = A(1), count = A(3);
    if (start >= max) return;
    if (start + count > max) count = max - start;
    if (set)
    {
        memcpy(arr + start * 4, gbuf(A(2)), count * 16);
        emit(OP_SHADER_CONST, {stage, start, count}, gbuf(A(2)), count * 16);
    }
    else memcpy(gbuf(A(2)), arr + start * 4, count * 16);
}
void dv_SetVSConstF(FnvCpu& s) { const_f(s, 0, true); RET(0, 4); }
void dv_GetVSConstF(FnvCpu& s) { const_f(s, 0, false); RET(0, 4); }
void dv_SetPSConstF(FnvCpu& s) { const_f(s, 1, true); RET(0, 4); }
void dv_GetPSConstF(FnvCpu& s) { const_f(s, 1, false); RET(0, 4); }
void const_ib(FnvCpu& s, void* arr, uint32_t elem, bool set)
{
    uint32_t start = A(1), count = A(3);
    if (start + count > 16) return;
    if (set) memcpy((uint8_t*)arr + start * elem, gbuf(A(2)), count * elem);
    else memcpy(gbuf(A(2)), (uint8_t*)arr + start * elem, count * elem);
}
void dv_SetVSConstI(FnvCpu& s) { const_ib(s, D(s)->vsi, 16, true); RET(0, 4); }
void dv_GetVSConstI(FnvCpu& s) { const_ib(s, D(s)->vsi, 16, false); RET(0, 4); }
void dv_SetVSConstB(FnvCpu& s) { const_ib(s, D(s)->vsb, 4, true); RET(0, 4); }
void dv_GetVSConstB(FnvCpu& s) { const_ib(s, D(s)->vsb, 4, false); RET(0, 4); }
void dv_SetPSConstI(FnvCpu& s) { const_ib(s, D(s)->psi, 16, true); RET(0, 4); }
void dv_GetPSConstI(FnvCpu& s) { const_ib(s, D(s)->psi, 16, false); RET(0, 4); }
void dv_SetPSConstB(FnvCpu& s) { const_ib(s, D(s)->psb, 4, true); RET(0, 4); }
void dv_GetPSConstB(FnvCpu& s) { const_ib(s, D(s)->psb, 4, false); RET(0, 4); }
void dv_SetStreamSource(FnvCpu& s)
{
    Device* d = D(s);
    uint32_t i = A(1);
    if (i >= 16) RET(D3DERR_INVALIDCALL, 5);
    d->stream[i] = A(2); d->stream_off[i] = A(3); d->stream_stride[i] = A(4);
    emit(OP_SET_STREAM, {i, A(2) ? ((Buf*)com_host(A(2)))->id : 0, A(3), A(4)});
    RET(0, 5);
}
void dv_GetStreamSource(FnvCpu& s)
{
    Device* d = D(s);
    uint32_t i = A(1) < 16 ? A(1) : 0;
    M32(A(2)) = d->stream[i]; if (d->stream[i]) ++M32(d->stream[i] + 8);
    M32(A(3)) = d->stream_off[i]; M32(A(4)) = d->stream_stride[i];
    RET(0, 5);
}
void dv_SetStreamSourceFreq(FnvCpu& s)
{
    Device* d = D(s);
    if (A(1) < 16) { d->stream_freq[A(1)] = A(2); emit(OP_STREAM_FREQ, {A(1), A(2)}); }
    RET(0, 3);
}
void dv_GetStreamSourceFreq(FnvCpu& s) { M32(A(2)) = A(1) < 16 ? D(s)->stream_freq[A(1)] : 1; RET(0, 3); }
void dv_SetIndices(FnvCpu& s)
{
    Device* d = D(s);
    d->ib = A(1);
    emit(OP_SET_INDICES, {A(1) ? ((Buf*)com_host(A(1)))->id : 0});
    RET(0, 2);
}
void dv_GetIndices(FnvCpu& s) { Device* d = D(s); M32(A(1)) = d->ib; if (d->ib) ++M32(d->ib + 8); RET(0, 2); }
void dv_CreateQuery(FnvCpu& s)
{
    static ComClass* c = com_class("IDirect3DQuery9", {
        {"GetDevice", any_GetDevice}, {"GetType", q_GetType}, {"GetDataSize", q_GetDataSize}, {"Issue", q_Issue},
        {"GetData", q_GetData}});
    uint32_t type = A(1);
    if (!A(2)) RET(type == 8 || type == 9 || type == 10 || type == 11 || type == 12 ? 0 : D3DERR_NOTAVAILABLE, 3);
    Query* q = new Query{type, 0, D(s)};
    q->obj = com_new(c, q);
    M32(A(2)) = q->obj;
    RET(0, 3);
}
void dv_Release(FnvCpu& s) { RET(release(A(0)), 1); }
void dv_EvictManagedResources(FnvCpu& s) { RET(0, 1); }
void dv_CreateAdditionalSwapChain(FnvCpu& s) { RET(D3DERR_NOTAVAILABLE, 3); }
void dv_SetSoftwareVertexProcessing(FnvCpu& s) { RET(0, 2); }
void dv_GetSoftwareVertexProcessing(FnvCpu& s) { RET(0, 1); }
void dv_SetNPatchMode(FnvCpu& s) { RET(0, 2); }
void dv_GetNPatchMode(FnvCpu& s) { RET(0, 1); }

// ---- swap chain -----------------------------------------------------------------------------------------------------
void sc_Present(FnvCpu& s) { RET(0, 6); }
void sc_GetBackBuffer(FnvCpu& s) { M32(A(3)) = g_dev->bb_surf; ++M32(g_dev->bb_surf + 8); RET(0, 4); }
void sc_GetPresentParameters(FnvCpu& s) { memcpy(gbuf(A(1)), g_dev->pp, 56); RET(0, 2); }
void sc_GetDisplayMode(FnvCpu& s)
{
    uint32_t m = A(1);
    M32(m) = g_dev->bb_w; M32(m + 4) = g_dev->bb_h; M32(m + 8) = 60; M32(m + 12) = 22;
    RET(0, 2);
}

ComClass* device_class()
{
    static ComClass* c = com_class("IDirect3DDevice9", {
        {"Release", dv_Release}, {"TestCooperativeLevel", dv_TestCooperativeLevel},
        {"GetAvailableTextureMem", dv_GetAvailableTextureMem}, {"EvictManagedResources", dv_EvictManagedResources},
        {"GetDirect3D", dv_GetDirect3D}, {"GetDeviceCaps", dv_GetDeviceCaps}, {"GetDisplayMode", dv_GetDisplayMode},
        {"GetCreationParameters", dv_GetCreationParameters}, {"SetCursorProperties", dv_SetCursorProperties},
        {"SetCursorPosition", dv_SetCursorPosition}, {"ShowCursor", dv_ShowCursor},
        {"CreateAdditionalSwapChain", dv_CreateAdditionalSwapChain}, {"GetSwapChain", dv_GetSwapChain},
        {"GetNumberOfSwapChains", dv_GetNumberOfSwapChains}, {"Reset", dv_Reset}, {"Present", dv_Present},
        {"GetBackBuffer", dv_GetBackBuffer}, {"GetRasterStatus", dv_GetRasterStatus},
        {"SetDialogBoxMode", dv_SetDialogBoxMode}, {"SetGammaRamp", dv_SetGammaRamp}, {"GetGammaRamp", dv_GetGammaRamp},
        {"CreateTexture", dv_CreateTexture}, {"CreateVolumeTexture", dv_CreateVolumeTexture},
        {"CreateCubeTexture", dv_CreateCubeTexture}, {"CreateVertexBuffer", dv_CreateVertexBuffer},
        {"CreateIndexBuffer", dv_CreateIndexBuffer}, {"CreateRenderTarget", dv_CreateRenderTarget},
        {"CreateDepthStencilSurface", dv_CreateDepthStencilSurface}, {"UpdateSurface", dv_UpdateSurface},
        {"UpdateTexture", dv_UpdateTexture}, {"GetRenderTargetData", dv_GetRenderTargetData},
        {"GetFrontBufferData", dv_GetFrontBufferData}, {"StretchRect", dv_StretchRect}, {"ColorFill", dv_ColorFill},
        {"CreateOffscreenPlainSurface", dv_CreateOffscreenPlainSurface}, {"SetRenderTarget", dv_SetRenderTarget},
        {"GetRenderTarget", dv_GetRenderTarget}, {"SetDepthStencilSurface", dv_SetDepthStencilSurface},
        {"GetDepthStencilSurface", dv_GetDepthStencilSurface}, {"BeginScene", dv_ok1}, {"EndScene", dv_ok1},
        {"Clear", dv_Clear}, {"SetTransform", dv_SetTransform}, {"GetTransform", dv_GetTransform},
        {"MultiplyTransform", dv_MultiplyTransform}, {"SetViewport", dv_SetViewport}, {"GetViewport", dv_GetViewport},
        {"SetMaterial", dv_SetMaterial}, {"GetMaterial", dv_GetMaterial}, {"SetLight", dv_SetLight},
        {"GetLight", dv_GetLight}, {"LightEnable", dv_LightEnable}, {"GetLightEnable", dv_GetLightEnable},
        {"SetClipPlane", dv_SetClipPlane}, {"GetClipPlane", dv_GetClipPlane}, {"SetRenderState", dv_SetRenderState},
        {"GetRenderState", dv_GetRenderState}, {"CreateStateBlock", dv_CreateStateBlock},
        {"BeginStateBlock", dv_BeginStateBlock}, {"EndStateBlock", dv_EndStateBlock},
        {"SetClipStatus", dv_SetClipStatus}, {"GetClipStatus", dv_GetClipStatus}, {"GetTexture", dv_GetTexture},
        {"SetTexture", dv_SetTexture}, {"GetTextureStageState", dv_GetTextureStageState},
        {"SetTextureStageState", dv_SetTextureStageState}, {"GetSamplerState", dv_GetSamplerState},
        {"SetSamplerState", dv_SetSamplerState}, {"ValidateDevice", dv_ValidateDevice},
        {"SetPaletteEntries", dv_ok3}, {"GetPaletteEntries", dv_ok3}, {"SetCurrentTexturePalette", dv_ok2},
        {"GetCurrentTexturePalette", dv_ok2}, {"SetScissorRect", dv_SetScissorRect}, {"GetScissorRect", dv_GetScissorRect},
        {"SetSoftwareVertexProcessing", dv_SetSoftwareVertexProcessing},
        {"GetSoftwareVertexProcessing", dv_GetSoftwareVertexProcessing}, {"SetNPatchMode", dv_SetNPatchMode},
        {"GetNPatchMode", dv_GetNPatchMode}, {"DrawPrimitive", dv_DrawPrimitive},
        {"DrawIndexedPrimitive", dv_DrawIndexedPrimitive}, {"DrawPrimitiveUP", dv_DrawPrimitiveUP},
        {"DrawIndexedPrimitiveUP", dv_DrawIndexedPrimitiveUP}, {"ProcessVertices", dv_ProcessVertices},
        {"CreateVertexDeclaration", dv_CreateVertexDeclaration}, {"SetVertexDeclaration", dv_SetVertexDeclaration},
        {"GetVertexDeclaration", dv_GetVertexDeclaration}, {"SetFVF", dv_SetFVF}, {"GetFVF", dv_GetFVF},
        {"CreateVertexShader", dv_CreateVertexShader}, {"SetVertexShader", dv_SetVertexShader},
        {"GetVertexShader", dv_GetVertexShader}, {"SetVertexShaderConstantF", dv_SetVSConstF},
        {"GetVertexShaderConstantF", dv_GetVSConstF}, {"SetVertexShaderConstantI", dv_SetVSConstI},
        {"GetVertexShaderConstantI", dv_GetVSConstI}, {"SetVertexShaderConstantB", dv_SetVSConstB},
        {"GetVertexShaderConstantB", dv_GetVSConstB}, {"SetStreamSource", dv_SetStreamSource},
        {"GetStreamSource", dv_GetStreamSource}, {"SetStreamSourceFreq", dv_SetStreamSourceFreq},
        {"GetStreamSourceFreq", dv_GetStreamSourceFreq}, {"SetIndices", dv_SetIndices}, {"GetIndices", dv_GetIndices},
        {"CreatePixelShader", dv_CreatePixelShader}, {"SetPixelShader", dv_SetPixelShader},
        {"GetPixelShader", dv_GetPixelShader}, {"SetPixelShaderConstantF", dv_SetPSConstF},
        {"GetPixelShaderConstantF", dv_GetPSConstF}, {"SetPixelShaderConstantI", dv_SetPSConstI},
        {"GetPixelShaderConstantI", dv_GetPSConstI}, {"SetPixelShaderConstantB", dv_SetPSConstB},
        {"GetPixelShaderConstantB", dv_GetPSConstB}, {"CreateQuery", dv_CreateQuery}});
    return c;
}

// ---- IDirect3D9 ---------------------------------------------------------------------------------------------------------
void d9_GetAdapterCount(FnvCpu& s) { RET(1, 1); }
void d9_GetAdapterIdentifier(FnvCpu& s) { memcpy(gbuf(A(3)), kIdentifier, sizeof kIdentifier); RET(0, 4); }
void d9_GetAdapterModeCount(FnvCpu& s)
{
    uint32_t n = 0;
    for (const CapsMode& m : kModes) n += m.fmt == A(2);
    RET(n, 3);
}
void d9_EnumAdapterModes(FnvCpu& s)
{
    uint32_t i = 0;
    for (const CapsMode& m : kModes)
        if (m.fmt == A(2) && i++ == A(3))
        {
            uint32_t o = A(4);
            M32(o) = m.w; M32(o + 4) = m.h; M32(o + 8) = m.refresh; M32(o + 12) = m.fmt;
            RET(0, 5);
        }
    RET(D3DERR_INVALIDCALL, 5);
}
void d9_GetAdapterDisplayMode(FnvCpu& s) { memcpy(gbuf(A(2)), kCurrentMode, sizeof kCurrentMode); RET(0, 3); }
void d9_CheckDeviceType(FnvCpu& s) { RET(0, 6); }
void d9_CheckDeviceFormat(FnvCpu& s)
{
    uint32_t usage = A(4), rtype = A(5), fmt = A(6);
    for (const CapsFormat& f : kFormats)
        if (f.ok && f.fmt == fmt && f.rtype == rtype && f.usage == usage) RET(0, 7);
    // depth formats are queried as surfaces with usage DEPTHSTENCIL; anything not in the table is not available
    RET(D3DERR_NOTAVAILABLE, 7);
}
void d9_CheckDeviceMultiSampleType(FnvCpu& s)
{
    if (A(6)) M32(A(6)) = 1;
    RET(A(5) == 0 ? 0 : D3DERR_NOTAVAILABLE, 7);                 // no multisampling (yet)
}
void d9_CheckDepthStencilMatch(FnvCpu& s) { RET(0, 6); }
void d9_CheckDeviceFormatConversion(FnvCpu& s) { RET(0, 5); }
void d9_GetDeviceCaps(FnvCpu& s) { memcpy(gbuf(A(3)), kCaps, sizeof kCaps); RET(0, 4); }
void d9_GetAdapterMonitor(FnvCpu& s) { RET(0x00050001, 2); }
void d9_CreateDevice(FnvCpu& s)
{
    Device* d = new Device;
    g_dev = d;
    d->d3d = A(0);
    d->hwnd = A(3);
    d->behavior = A(4);
    memcpy(d->pp, gbuf(A(5)), 56);
    d->obj = com_new(device_class(), d);
    g_device_obj = d->obj;
    static ComClass* sc = com_class("IDirect3DSwapChain9", {
        {"Present", sc_Present}, {"GetBackBuffer", sc_GetBackBuffer}, {"GetPresentParameters", sc_GetPresentParameters},
        {"GetDisplayMode", sc_GetDisplayMode}, {"GetDevice", any_GetDevice}});
    d->swapchain = com_new(sc, d);
    // D3D9 defaults that differ from zero
    d->rs[7] = 1;            // ZENABLE
    d->rs[8] = 3;            // FILLMODE solid
    d->rs[9] = 2;            // SHADEMODE gouraud
    d->rs[14] = 1;           // ZWRITEENABLE
    d->rs[19] = 2;           // SRCBLEND one
    d->rs[20] = 1;           // DESTBLEND zero
    d->rs[22] = 3;           // CULLMODE ccw
    d->rs[23] = 4;           // ZFUNC lessequal
    d->rs[25] = 8;           // ALPHAFUNC always
    d->rs[168] = 0xf;        // COLORWRITEENABLE
    d->rs[171] = 1;          // BLENDOP add
    for (int i = 0; i < 20; ++i)
    {
        d->ss[i][1] = 1; d->ss[i][2] = 1; d->ss[i][3] = 1;           // address wrap
        d->ss[i][5] = 1; d->ss[i][6] = 1;                            // mag/min point
        d->ss[i][10] = 1;                                            // max anisotropy
    }
    for (int i = 0; i < 16; ++i) d->stream_freq[i] = 1;
    char buf[160];
    snprintf(buf, sizeof buf, "[d3d9] CreateDevice: %ux%u format %u, %s, behaviour %08x%c", d->pp[0], d->pp[1], d->pp[2],
             d->pp[8] ? "windowed" : "full screen", d->behavior, 10);
    fnv_host_log(buf);
    make_backbuffer(d);
    M32(A(6)) = d->obj;
    RET(0, 7);
}

void p_Direct3DCreate9(FnvCpu& s)
{
    static ComClass* c = com_class("IDirect3D9", {
        {"GetAdapterCount", d9_GetAdapterCount}, {"GetAdapterIdentifier", d9_GetAdapterIdentifier},
        {"GetAdapterModeCount", d9_GetAdapterModeCount}, {"EnumAdapterModes", d9_EnumAdapterModes},
        {"GetAdapterDisplayMode", d9_GetAdapterDisplayMode}, {"CheckDeviceType", d9_CheckDeviceType},
        {"CheckDeviceFormat", d9_CheckDeviceFormat}, {"CheckDeviceMultiSampleType", d9_CheckDeviceMultiSampleType},
        {"CheckDepthStencilMatch", d9_CheckDepthStencilMatch},
        {"CheckDeviceFormatConversion", d9_CheckDeviceFormatConversion}, {"GetDeviceCaps", d9_GetDeviceCaps},
        {"GetAdapterMonitor", d9_GetAdapterMonitor}, {"CreateDevice", d9_CreateDevice}});
    RET(com_new(c, nullptr), 1);
}

const PalEntry kD3d9[] = {{"Direct3DCreate9", p_Direct3DCreate9}, {nullptr, nullptr}};
} // namespace

const PalEntry* pal_d3d9_entries() { return kD3d9; }
const char* pal_d3d9_adapter_name()
{
    // D3DADAPTER_IDENTIFIER9 starts with Driver[512], followed by Description[512].
    return reinterpret_cast<const char*>(kIdentifier + 512);
}
D3dRing* pal_d3d_ring() { return &g_ring; }
void pal_d3d_enable_ring(uint32_t bytes)
{
    g_ring.size = bytes;
    g_ring.data = (uint32_t)(uintptr_t)calloc(1, bytes);
    g_ring.write = g_ring.read = 0;
    g_ring.enabled = 1;
}
uint32_t pal_d3d_create_texture(uint32_t device, uint32_t kind, uint32_t w, uint32_t h, uint32_t depth, uint32_t levels,
                                uint32_t usage, uint32_t fmt, uint32_t pool)
{
    return new_texture_obj(dev_of(device), kind, w, h, depth, levels, usage, fmt, pool);
}
void pal_d3d_fill_level(uint32_t texture, uint32_t face, uint32_t level, const uint8_t* data, uint32_t bytes)
{
    Tex* t = (Tex*)com_host(texture);
    if (level >= t->levels || face >= t->faces()) return;
    uint32_t mem = level_mem(t, face, level);
    Level& L = t->lv[face * t->levels + level];
    memcpy((void*)(uintptr_t)mem, data, bytes < L.bytes ? bytes : L.bytes);
    upload_level(t, face, level, 0, 0, t->lw(level), t->lh(level));
}
