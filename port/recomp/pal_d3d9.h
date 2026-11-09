// PAL Direct3D 9: the game's device as guest COM objects that keep D3D state and lockable CPU copies of resources,
// and send everything that changes the picture to a renderer through a command ring in shared memory
// (docs/RECOMP.md). The browser's renderer (web/recomp/renderer.js, WebGPU) consumes the ring on the page's main thread;
// in node no renderer is attached and packets are dropped.
#pragma once
#include "pal_com.h"

// command ring packets: [u32 op][u32 payload bytes][payload, padded to 4]
enum D3dOp : uint32_t
{
    OP_WRAP = 0xffff,           // continue at the start of the ring
    OP_CREATE_TEXTURE = 1,      // id, kind (0 2d, 1 cube, 2 volume, 3 render target, 4 depth surface), fmt, w, h, depth, levels, usage
    OP_DESTROY = 2,             // id
    OP_UPLOAD_TEXTURE = 3,      // id, face, level, x, y, w, h (pixels), bytes per row, row count, data
    OP_CREATE_BUFFER = 4,       // id, size, kind (0 vertex, 1 index16, 2 index32)
    OP_UPLOAD_BUFFER = 5,       // id, offset, size, data
    OP_CREATE_SHADER = 6,       // id, stage (0 vs, 1 ps), sha1[5], token bytes, tokens
    OP_CREATE_DECL = 7,         // id, count, D3DVERTEXELEMENT9[count]
    OP_RENDER_STATE = 8,        // state, value
    OP_SAMPLER_STATE = 9,       // sampler, type, value
    OP_SET_TEXTURE = 10,        // stage, id
    OP_SET_SHADER = 11,         // stage, id
    OP_SHADER_CONST = 12,       // stage, start register, count (vec4), floats
    OP_SET_STREAM = 13,         // stream, buffer id, offset, stride
    OP_SET_INDICES = 14,        // buffer id
    OP_SET_DECL = 15,           // decl id (0 = use FVF)
    OP_SET_FVF = 16,            // fvf
    OP_SET_TARGET = 17,         // index, texture id, face, level (id 0 = none; 1 = back buffer)
    OP_SET_DEPTH = 18,          // texture id (0 = none)
    OP_VIEWPORT = 19,           // x, y, w, h, minz, maxz (floats as bits)
    OP_SCISSOR = 20,            // left, top, right, bottom
    OP_CLEAR = 21,              // flags, color, z (float bits), stencil
    OP_DRAW = 22,               // primitive type, start vertex, primitive count
    OP_DRAW_INDEXED = 23,       // primitive type, base vertex, min index, vertex count, start index, primitive count
    OP_DRAW_UP = 24,            // primitive type, primitive count, stride, vertex bytes, data
    OP_DRAW_INDEXED_UP = 25,    // primitive type, min index, vertex count, primitive count, index32, stride, index bytes, vertex bytes, data
    OP_STRETCH = 26,            // src id, src face, src level, src rect[4], dst id, dst face, dst level, dst rect[4], filter
    OP_PRESENT = 27,            // frame number
    OP_STREAM_FREQ = 28,        // stream, setting
    OP_COLOR_FILL = 29,         // id, face, level, rect[4] (all -1 = whole), color
    OP_TEX_STAGE_STATE = 30,    // stage, type, value
    OP_RESIZE = 31,             // back buffer width, height
    OP_SET_TRANSFORM = 32,      // state, float[16]
};

// The renderer's view of the ring (exported to JavaScript): base address, size, write and read offsets, frames.
struct D3dRing
{
    uint32_t data, size;
    volatile uint32_t write, read;     // byte offsets; the renderer advances read
    volatile uint32_t frames_sent, frames_done;
    uint32_t enabled;                  // set by the host when a renderer consumes the ring
};
D3dRing* pal_d3d_ring();
void pal_d3d_enable_ring(uint32_t bytes);

const PalEntry* pal_d3d9_entries();    // Direct3DCreate9
const char* pal_d3d9_adapter_name();
const PalEntry* pal_d3dx_entries();    // pal_d3dx.cpp: the D3DX functions the game imports

// for D3DX (pal_d3dx.cpp): create a texture on a device object and fill a level
uint32_t pal_d3d_create_texture(uint32_t device, uint32_t kind, uint32_t w, uint32_t h, uint32_t depth,
                                uint32_t levels, uint32_t usage, uint32_t fmt, uint32_t pool);
void pal_d3d_fill_level(uint32_t texture, uint32_t face, uint32_t level, const uint8_t* data, uint32_t bytes);
uint32_t pal_d3d_format_bytes(uint32_t fmt, uint32_t w, uint32_t h, uint32_t* row_pitch, uint32_t* rows);
