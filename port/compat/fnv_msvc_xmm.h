/* MSVC-layout SSE/MMX types and the intrinsics the decomp uses, as ordinary extern "C" functions.
 *
 * The decomp was written against VC2008's <xmmintrin.h>: __m128 is a union with m128_f32[] etc., and code reads
 * those members directly. clang's __m128 is a vector type, so units that use the members cannot see clang's
 * headers. tools/overlay.py force-includes fnv_sse_block.h (which keeps clang's SSE headers out) and, for units
 * that do not define the unions themselves, this header. Implementations: fnv_msvc_xmm.cpp (portable scalar C, so
 * the same code serves the wasm build). */
#pragma once

#ifndef FNV_MSVC_XMM_TYPES
#define FNV_MSVC_XMM_TYPES

typedef union __declspec(align(8)) __m64 {
    unsigned __int64 m64_u64;
    float m64_f32[2];
    __int8 m64_i8[8];
    __int16 m64_i16[4];
    __int32 m64_i32[2];
    __int64 m64_i64;
    unsigned __int8 m64_u8[8];
    unsigned __int16 m64_u16[4];
    unsigned __int32 m64_u32[2];
} __m64;

typedef union __declspec(align(16)) __m128 {
    float m128_f32[4];
    unsigned __int64 m128_u64[2];
    __int8 m128_i8[16];
    __int16 m128_i16[8];
    __int32 m128_i32[4];
    __int64 m128_i64[2];
    unsigned __int8 m128_u8[16];
    unsigned __int16 m128_u16[8];
    unsigned __int32 m128_u32[4];
} __m128;

typedef union __declspec(align(16)) __m128i {
    __int8 m128i_i8[16];
    __int16 m128i_i16[8];
    __int32 m128i_i32[4];
    __int64 m128i_i64[2];
    unsigned __int8 m128i_u8[16];
    unsigned __int16 m128i_u16[8];
    unsigned __int32 m128i_u32[4];
    unsigned __int64 m128i_u64[2];
} __m128i;

#endif

#define _MM_SHUFFLE(fp3, fp2, fp1, fp0) (((fp3) << 6) | ((fp2) << 4) | ((fp1) << 2) | (fp0))
#define _MM_HINT_T0 1
#define _MM_HINT_T1 2
#define _MM_HINT_T2 3
#define _MM_HINT_NTA 0

#ifdef __cplusplus
extern "C" {
#endif

/* arithmetic */
__m128 _mm_add_ps(__m128, __m128);
__m128 _mm_add_ss(__m128, __m128);
__m128 _mm_sub_ps(__m128, __m128);
__m128 _mm_sub_ss(__m128, __m128);
__m128 _mm_mul_ps(__m128, __m128);
__m128 _mm_mul_ss(__m128, __m128);
__m128 _mm_div_ps(__m128, __m128);
__m128 _mm_div_ss(__m128, __m128);
__m128 _mm_sqrt_ps(__m128);
__m128 _mm_sqrt_ss(__m128);
__m128 _mm_rsqrt_ps(__m128);
__m128 _mm_rsqrt_ss(__m128);
__m128 _mm_rcp_ps(__m128);
__m128 _mm_min_ps(__m128, __m128);
__m128 _mm_max_ps(__m128, __m128);

/* logic */
__m128 _mm_and_ps(__m128, __m128);
__m128 _mm_andnot_ps(__m128, __m128);
__m128 _mm_or_ps(__m128, __m128);
__m128 _mm_xor_ps(__m128, __m128);

/* compare */
__m128 _mm_cmpeq_ps(__m128, __m128);
__m128 _mm_cmpeq_ss(__m128, __m128);
__m128 _mm_cmplt_ps(__m128, __m128);
__m128 _mm_cmple_ps(__m128, __m128);
__m128 _mm_cmpgt_ps(__m128, __m128);
__m128 _mm_cmpge_ps(__m128, __m128);
int _mm_movemask_ps(__m128);

/* shuffle / move */
__m128 _mm_shuffle_ps(__m128, __m128, unsigned int);
__m128 _mm_unpackhi_ps(__m128, __m128);
__m128 _mm_unpacklo_ps(__m128, __m128);
__m128 _mm_movehl_ps(__m128, __m128);
__m128 _mm_movelh_ps(__m128, __m128);
__m128 _mm_move_ss(__m128, __m128);

/* set / load / store */
__m128 _mm_setzero_ps(void);
__m128 _mm_set_ss(float);
__m128 _mm_set_ps1(float);
__m128 _mm_set1_ps(float);
__m128 _mm_set_ps(float, float, float, float);
__m128 _mm_setr_ps(float, float, float, float);
__m128 _mm_load_ss(float const*);
__m128 _mm_load_ps(float const*);
__m128 _mm_loadu_ps(float const*);
__m128 _mm_load_ps1(float const*);
__m128 _mm_load1_ps(float const*);
void _mm_store_ss(float*, __m128);
void _mm_store_ps(float*, __m128);
void _mm_stream_ps(float*, __m128);
float _mm_cvtss_f32(__m128);
int _mm_cvtss_si32(__m128);
void _mm_prefetch(char const*, int);
void _mm_setcsr(unsigned int);
unsigned int _mm_getcsr(void);

/* SSE2 integer */
__m128i _mm_setzero_si128(void);
__m128i _mm_set1_epi16(short);
__m128i _mm_cvtsi32_si128(int);
__m128i _mm_load_si128(__m128i const*);
__m128i _mm_loadu_si128(__m128i const*);
__m128i _mm_srli_epi32(__m128i, int);
__m128i _mm_slli_epi32(__m128i, int);
__m128i _mm_unpacklo_epi16(__m128i, __m128i);
__m128i _mm_unpackhi_epi16(__m128i, __m128i);
__m128i _mm_adds_epu16(__m128i, __m128i);
__m128 _mm_cvtepi32_ps(__m128i);
__m128 _mm_castsi128_ps(__m128i);

/* MMX */
__m64 _mm_setzero_si64(void);
__m64 _mm_set_pi32(int, int);
__m64 _mm_add_pi16(__m64, __m64);
__m64 _mm_sub_pi16(__m64, __m64);
__m64 _mm_cmpgt_pi16(__m64, __m64);
__m64 _mm_cmpeq_pi16(__m64, __m64);
__m64 _mm_and_si64(__m64, __m64);
__m64 _mm_srli_si64(__m64, int);
__m64 _mm_srli_pi16(__m64, int);
__m64 _mm_slli_pi16(__m64, int);
__m64 _mm_unpacklo_pi16(__m64, __m64);
__m64 _mm_unpacklo_pi32(__m64, __m64);
void _mm_empty(void);

#ifdef __cplusplus
}
#endif
