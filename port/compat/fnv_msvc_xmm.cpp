// Portable scalar implementations of the SSE/MMX intrinsics declared in fnv_msvc_xmm.h, on the MSVC union layout.
// Semantics follow the Intel definitions (min/max return the second operand on NaN, compares produce all-ones
// lane masks). rsqrt/rcp are computed exactly rather than to the hardware's 12-bit approximation.
#include "fnv_sse_block.h"
#include "fnv_msvc_xmm.h"

#include <math.h>
#include <string.h>

namespace
{
inline __m128 v4(float a, float b, float c, float d)
{
    __m128 r;
    r.m128_f32[0] = a; r.m128_f32[1] = b; r.m128_f32[2] = c; r.m128_f32[3] = d;
    return r;
}
inline float mask(bool b)
{
    unsigned int u = b ? 0xffffffffu : 0u;
    float f;
    memcpy(&f, &u, 4);
    return f;
}
template <class F> inline __m128 map2(__m128 a, __m128 b, F f)
{
    return v4(f(a.m128_f32[0], b.m128_f32[0]), f(a.m128_f32[1], b.m128_f32[1]),
              f(a.m128_f32[2], b.m128_f32[2]), f(a.m128_f32[3], b.m128_f32[3]));
}
template <class F> inline __m128 ss(__m128 a, __m128 b, F f)
{
    a.m128_f32[0] = f(a.m128_f32[0], b.m128_f32[0]);
    return a;
}
template <class F> inline __m128 bits(__m128 a, __m128 b, F f)
{
    for (int i = 0; i < 4; ++i)
        a.m128_u32[i] = f(a.m128_u32[i], b.m128_u32[i]);
    return a;
}
template <class F> inline __m64 lanes16(__m64 a, __m64 b, F f)
{
    __m64 r;
    for (int i = 0; i < 4; ++i)
        r.m64_i16[i] = (short)f(a.m64_i16[i], b.m64_i16[i]);
    return r;
}
#ifndef _MSC_VER
unsigned int g_csr = 0x1f80;
#endif
}

extern "C" {

__m128 _mm_add_ps(__m128 a, __m128 b) { return map2(a, b, [](float x, float y) { return x + y; }); }
__m128 _mm_add_ss(__m128 a, __m128 b) { return ss(a, b, [](float x, float y) { return x + y; }); }
__m128 _mm_sub_ps(__m128 a, __m128 b) { return map2(a, b, [](float x, float y) { return x - y; }); }
__m128 _mm_sub_ss(__m128 a, __m128 b) { return ss(a, b, [](float x, float y) { return x - y; }); }
__m128 _mm_mul_ps(__m128 a, __m128 b) { return map2(a, b, [](float x, float y) { return x * y; }); }
__m128 _mm_mul_ss(__m128 a, __m128 b) { return ss(a, b, [](float x, float y) { return x * y; }); }
__m128 _mm_div_ps(__m128 a, __m128 b) { return map2(a, b, [](float x, float y) { return x / y; }); }
__m128 _mm_div_ss(__m128 a, __m128 b) { return ss(a, b, [](float x, float y) { return x / y; }); }
__m128 _mm_sqrt_ps(__m128 a) { return map2(a, a, [](float x, float) { return sqrtf(x); }); }
__m128 _mm_sqrt_ss(__m128 a) { return ss(a, a, [](float x, float) { return sqrtf(x); }); }
__m128 _mm_rsqrt_ps(__m128 a) { return map2(a, a, [](float x, float) { return 1.0f / sqrtf(x); }); }
__m128 _mm_rsqrt_ss(__m128 a) { return ss(a, a, [](float x, float) { return 1.0f / sqrtf(x); }); }
__m128 _mm_rcp_ps(__m128 a) { return map2(a, a, [](float x, float) { return 1.0f / x; }); }
__m128 _mm_min_ps(__m128 a, __m128 b) { return map2(a, b, [](float x, float y) { return x < y ? x : y; }); }
__m128 _mm_max_ps(__m128 a, __m128 b) { return map2(a, b, [](float x, float y) { return x > y ? x : y; }); }

__m128 _mm_and_ps(__m128 a, __m128 b) { return bits(a, b, [](unsigned x, unsigned y) { return x & y; }); }
__m128 _mm_andnot_ps(__m128 a, __m128 b) { return bits(a, b, [](unsigned x, unsigned y) { return ~x & y; }); }
__m128 _mm_or_ps(__m128 a, __m128 b) { return bits(a, b, [](unsigned x, unsigned y) { return x | y; }); }
__m128 _mm_xor_ps(__m128 a, __m128 b) { return bits(a, b, [](unsigned x, unsigned y) { return x ^ y; }); }

__m128 _mm_cmpeq_ps(__m128 a, __m128 b) { return map2(a, b, [](float x, float y) { return mask(x == y); }); }
__m128 _mm_cmpeq_ss(__m128 a, __m128 b) { return ss(a, b, [](float x, float y) { return mask(x == y); }); }
__m128 _mm_cmplt_ps(__m128 a, __m128 b) { return map2(a, b, [](float x, float y) { return mask(x < y); }); }
__m128 _mm_cmple_ps(__m128 a, __m128 b) { return map2(a, b, [](float x, float y) { return mask(x <= y); }); }
__m128 _mm_cmpgt_ps(__m128 a, __m128 b) { return map2(a, b, [](float x, float y) { return mask(x > y); }); }
__m128 _mm_cmpge_ps(__m128 a, __m128 b) { return map2(a, b, [](float x, float y) { return mask(x >= y); }); }
int _mm_movemask_ps(__m128 a)
{
    int r = 0;
    for (int i = 0; i < 4; ++i)
        r |= (int)(a.m128_u32[i] >> 31) << i;
    return r;
}

__m128 _mm_shuffle_ps(__m128 a, __m128 b, unsigned int imm)
{
    return v4(a.m128_f32[imm & 3], a.m128_f32[(imm >> 2) & 3], b.m128_f32[(imm >> 4) & 3], b.m128_f32[(imm >> 6) & 3]);
}
__m128 _mm_unpackhi_ps(__m128 a, __m128 b) { return v4(a.m128_f32[2], b.m128_f32[2], a.m128_f32[3], b.m128_f32[3]); }
__m128 _mm_unpacklo_ps(__m128 a, __m128 b) { return v4(a.m128_f32[0], b.m128_f32[0], a.m128_f32[1], b.m128_f32[1]); }
__m128 _mm_movehl_ps(__m128 a, __m128 b) { return v4(b.m128_f32[2], b.m128_f32[3], a.m128_f32[2], a.m128_f32[3]); }
__m128 _mm_movelh_ps(__m128 a, __m128 b) { return v4(a.m128_f32[0], a.m128_f32[1], b.m128_f32[0], b.m128_f32[1]); }
__m128 _mm_move_ss(__m128 a, __m128 b) { a.m128_f32[0] = b.m128_f32[0]; return a; }

__m128 _mm_setzero_ps(void) { return v4(0, 0, 0, 0); }
__m128 _mm_set_ss(float a) { return v4(a, 0, 0, 0); }
__m128 _mm_set_ps1(float a) { return v4(a, a, a, a); }
__m128 _mm_set1_ps(float a) { return v4(a, a, a, a); }
__m128 _mm_set_ps(float e3, float e2, float e1, float e0) { return v4(e0, e1, e2, e3); }
__m128 _mm_setr_ps(float e0, float e1, float e2, float e3) { return v4(e0, e1, e2, e3); }
__m128 _mm_load_ss(float const* p) { return v4(p[0], 0, 0, 0); }
__m128 _mm_load_ps(float const* p) { return v4(p[0], p[1], p[2], p[3]); }
__m128 _mm_loadu_ps(float const* p) { return v4(p[0], p[1], p[2], p[3]); }
__m128 _mm_load_ps1(float const* p) { return v4(p[0], p[0], p[0], p[0]); }
__m128 _mm_load1_ps(float const* p) { return v4(p[0], p[0], p[0], p[0]); }
void _mm_store_ss(float* p, __m128 a) { p[0] = a.m128_f32[0]; }
void _mm_store_ps(float* p, __m128 a) { memcpy(p, a.m128_f32, 16); }
void _mm_stream_ps(float* p, __m128 a) { memcpy(p, a.m128_f32, 16); }
float _mm_cvtss_f32(__m128 a) { return a.m128_f32[0]; }
int _mm_cvtss_si32(__m128 a) { return (int)lrintf(a.m128_f32[0]); }
#ifndef _MSC_VER
// clang-cl treats these three as builtins (real prefetch / MXCSR access); other targets (wasm) get these
void _mm_prefetch(char const*, int) {}
void _mm_setcsr(unsigned int v) { g_csr = v; }       // recorded only: the port does not depend on MXCSR modes
unsigned int _mm_getcsr(void) { return g_csr; }
#endif

__m128i _mm_setzero_si128(void) { __m128i r; memset(&r, 0, 16); return r; }
__m128i _mm_set1_epi16(short v) { __m128i r; for (int i = 0; i < 8; ++i) r.m128i_i16[i] = v; return r; }
__m128i _mm_cvtsi32_si128(int v) { __m128i r; memset(&r, 0, 16); r.m128i_i32[0] = v; return r; }
__m128i _mm_load_si128(__m128i const* p) { return *p; }
__m128i _mm_loadu_si128(__m128i const* p) { __m128i r; memcpy(&r, p, 16); return r; }
__m128i _mm_srli_epi32(__m128i a, int n)
{
    for (int i = 0; i < 4; ++i)
        a.m128i_u32[i] = (unsigned)n > 31 ? 0 : a.m128i_u32[i] >> n;
    return a;
}
__m128i _mm_slli_epi32(__m128i a, int n)
{
    for (int i = 0; i < 4; ++i)
        a.m128i_u32[i] = (unsigned)n > 31 ? 0 : a.m128i_u32[i] << n;
    return a;
}
__m128i _mm_unpacklo_epi16(__m128i a, __m128i b)
{
    __m128i r;
    for (int i = 0; i < 4; ++i) { r.m128i_i16[2 * i] = a.m128i_i16[i]; r.m128i_i16[2 * i + 1] = b.m128i_i16[i]; }
    return r;
}
__m128i _mm_unpackhi_epi16(__m128i a, __m128i b)
{
    __m128i r;
    for (int i = 0; i < 4; ++i) { r.m128i_i16[2 * i] = a.m128i_i16[4 + i]; r.m128i_i16[2 * i + 1] = b.m128i_i16[4 + i]; }
    return r;
}
__m128i _mm_adds_epu16(__m128i a, __m128i b)
{
    for (int i = 0; i < 8; ++i)
    {
        unsigned s = (unsigned)a.m128i_u16[i] + b.m128i_u16[i];
        a.m128i_u16[i] = (unsigned short)(s > 0xffff ? 0xffff : s);
    }
    return a;
}
__m128 _mm_cvtepi32_ps(__m128i a)
{
    return v4((float)a.m128i_i32[0], (float)a.m128i_i32[1], (float)a.m128i_i32[2], (float)a.m128i_i32[3]);
}
__m128 _mm_castsi128_ps(__m128i a) { __m128 r; memcpy(&r, &a, 16); return r; }

__m64 _mm_setzero_si64(void) { __m64 r; r.m64_u64 = 0; return r; }
__m64 _mm_set_pi32(int i1, int i0) { __m64 r; r.m64_i32[0] = i0; r.m64_i32[1] = i1; return r; }
__m64 _mm_add_pi16(__m64 a, __m64 b) { return lanes16(a, b, [](int x, int y) { return x + y; }); }
__m64 _mm_sub_pi16(__m64 a, __m64 b) { return lanes16(a, b, [](int x, int y) { return x - y; }); }
__m64 _mm_cmpgt_pi16(__m64 a, __m64 b) { return lanes16(a, b, [](int x, int y) { return x > y ? -1 : 0; }); }
__m64 _mm_cmpeq_pi16(__m64 a, __m64 b) { return lanes16(a, b, [](int x, int y) { return x == y ? -1 : 0; }); }
__m64 _mm_and_si64(__m64 a, __m64 b) { a.m64_u64 &= b.m64_u64; return a; }
__m64 _mm_srli_si64(__m64 a, int n) { a.m64_u64 = (unsigned)n > 63 ? 0 : a.m64_u64 >> n; return a; }
__m64 _mm_srli_pi16(__m64 a, int n)
{
    for (int i = 0; i < 4; ++i)
        a.m64_u16[i] = (unsigned short)((unsigned)n > 15 ? 0 : a.m64_u16[i] >> n);
    return a;
}
__m64 _mm_slli_pi16(__m64 a, int n)
{
    for (int i = 0; i < 4; ++i)
        a.m64_u16[i] = (unsigned short)((unsigned)n > 15 ? 0 : a.m64_u16[i] << n);
    return a;
}
__m64 _mm_unpacklo_pi16(__m64 a, __m64 b)
{
    __m64 r;
    r.m64_i16[0] = a.m64_i16[0]; r.m64_i16[1] = b.m64_i16[0]; r.m64_i16[2] = a.m64_i16[1]; r.m64_i16[3] = b.m64_i16[1];
    return r;
}
__m64 _mm_unpacklo_pi32(__m64 a, __m64 b) { __m64 r; r.m64_i32[0] = a.m64_i32[0]; r.m64_i32[1] = b.m64_i32[0]; return r; }
void _mm_empty(void) {}

}
