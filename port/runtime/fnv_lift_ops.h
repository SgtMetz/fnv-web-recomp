// Instruction helpers shared by the Phase 1 lifted bodies (fnv_lift.h) and the whole-game recompiler
// (port/recomp/fnv_recomp.h). They only use the field names of the machine state `s` (flags, x87, xmm).
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>
#if defined(__i386__) || defined(_M_IX86)
#include <xmmintrin.h>
#endif

inline uint32_t lift_mask(int bits) { return bits == 8 ? 0xffu : bits == 16 ? 0xffffu : 0xffffffffu; }
inline int lift_parity(uint32_t r) { r &= 0xff; r ^= r >> 4; r ^= r >> 2; r ^= r >> 1; return !(r & 1); }

#define LOGICF(r, bits) do { uint32_t m__ = lift_mask(bits), r__ = (uint32_t)(r) & m__; s.zf = r__ == 0; \
    s.sf = (int)((r__ >> ((bits) - 1)) & 1u); s.cf = 0; s.of = 0; s.pf = lift_parity(r__); } while (0)
#define ADDF(a, b, r, bits) do { uint32_t m__ = lift_mask(bits), a__ = (a) & m__, b__ = (b) & m__, r__ = (r) & m__; \
    int sh__ = (bits) - 1; s.zf = r__ == 0; s.sf = (int)((r__ >> sh__) & 1u); s.cf = r__ < a__; \
    s.of = (int)((~(a__ ^ b__) & (a__ ^ r__)) >> sh__ & 1u); s.pf = lift_parity(r__); } while (0)
#define SUBF(a, b, r, bits) do { uint32_t m__ = lift_mask(bits), a__ = (a) & m__, b__ = (b) & m__, r__ = (r) & m__; \
    int sh__ = (bits) - 1; s.zf = r__ == 0; s.sf = (int)((r__ >> sh__) & 1u); s.cf = a__ < b__; \
    s.of = (int)(((a__ ^ b__) & (a__ ^ r__)) >> sh__ & 1u); s.pf = lift_parity(r__); } while (0)

// x87 compare: C3 C2 C0 = 000 (>), 001 (<), 100 (=), 111 (unordered)
#define FCOM(a, b) do { double a__ = (a), b__ = (b); \
    if (a__ != a__ || b__ != b__) { s.c3 = s.c2 = s.c0 = 1; } \
    else { s.c3 = a__ == b__; s.c2 = 0; s.c0 = a__ < b__; } } while (0)
#define FSW() ((uint32_t)((s.c0 << 8) | (s.c2 << 10) | (((8 - s.depth) & 7) << 11) | (s.c3 << 14)))
// sahf: AH bits -> SF ZF AF PF CF (bit 7, 6, 4, 2, 0)
#define SAHF() do { uint32_t ah__ = (s.eax >> 8) & 0xffu; s.sf = (ah__ >> 7) & 1; s.zf = (ah__ >> 6) & 1; \
    s.pf = (ah__ >> 2) & 1; s.cf = ah__ & 1; } while (0)

enum { FOP_ADD, FOP_SUB, FOP_MUL, FOP_DIV, FOP_MIN, FOP_MAX };
// packed/scalar float op on n lanes (4 = ps, 1 = ss): d = d op src (min/max return src on NaN, as SSE does)
inline void SSEF(uint8_t* d, const uint8_t* src, int n, int op)
{
    float a[4], b[4];
    memcpy(a, d, 16);
    memcpy(b, src, n * 4);
    for (int i = 0; i < n; ++i)
    {
        switch (op)
        {
        case FOP_ADD: a[i] = a[i] + b[i]; break;
        case FOP_SUB: a[i] = a[i] - b[i]; break;
        case FOP_MUL: a[i] = a[i] * b[i]; break;
        case FOP_DIV: a[i] = a[i] / b[i]; break;
        case FOP_MIN: a[i] = a[i] < b[i] ? a[i] : b[i]; break;
        default: a[i] = a[i] > b[i] ? a[i] : b[i]; break;
        }
    }
    memcpy(d, a, 16);
}
// bitwise and / andnot / or / xor over 128 bits
inline void SSEB(uint8_t* d, const uint8_t* src, int op)
{
    for (int i = 0; i < 16; ++i)
        d[i] = op == 0 ? (d[i] & src[i]) : op == 1 ? ((uint8_t)~d[i] & src[i]) : op == 2 ? (d[i] | src[i]) : (d[i] ^ src[i]);
}

// shufps: lanes 0,1 from d, lanes 2,3 from src, selected by imm
inline void SHUFPS(uint8_t* d, const uint8_t* src, unsigned int imm)
{
    float a[4], b[4], r[4];
    memcpy(a, d, 16);
    memcpy(b, src, 16);
    r[0] = a[imm & 3];
    r[1] = a[(imm >> 2) & 3];
    r[2] = b[(imm >> 4) & 3];
    r[3] = b[(imm >> 6) & 3];
    memcpy(d, r, 16);
}

// ---- more SSE / integer helpers ----
// x87 80-bit extended values in memory (fld/fstp tbyte)
inline double LIFT_LD80(const uint8_t* p)
{
    uint64_t m; uint16_t se;
    memcpy(&m, p, 8); memcpy(&se, p + 8, 2);
    int e = se & 0x7fff;
    double sign = (se & 0x8000) ? -1.0 : 1.0;
    if (e == 0 && m == 0) return sign * 0.0;
    if (e == 0x7fff) return (m << 1) ? NAN : sign * INFINITY;
    return sign * ldexp((double)m, e - 16383 - 63);
}
inline void LIFT_ST80(uint8_t* p, double v)
{
    uint64_t m = 0; uint16_t se = 0;
    if (v != v) { m = 0xc000000000000000ull; se = 0x7fff; }
    else
    {
        if (signbit(v)) { se = 0x8000; v = -v; }
        if (isinf(v)) { m = 0x8000000000000000ull; se |= 0x7fff; }
        else if (v != 0)
        {
            int e; double f = frexp(v, &e);              // v = f * 2^e, f in [0.5, 1)
            m = (uint64_t)ldexp(f, 64);
            se |= (uint16_t)(e - 1 + 16383);
        }
    }
    memcpy(p, &m, 8); memcpy(p + 8, &se, 2);
}
inline uint32_t& lift_flags_id() { static uint32_t v = 0x200000u; return v; }   // EFLAGS.ID: toggling it is how CRT code probes for CPUID
inline void SQRTSS(uint8_t* d, const uint8_t* src) { float f; memcpy(&f, src, 4); f = sqrtf(f); memcpy(d, &f, 4); }
inline void RSQRTSS(uint8_t* d, const uint8_t* src) { float f; memcpy(&f, src, 4); f = 1.0f / sqrtf(f); memcpy(d, &f, 4); }
inline void RCPPS(uint8_t* d, const uint8_t* src)
{
    float a[4];
    memcpy(a, src, 16);
    for (int i = 0; i < 4; ++i) a[i] = 1.0f / a[i];
    memcpy(d, a, 16);
}
inline void UNPCKHPS(uint8_t* d, const uint8_t* src)
{
    uint8_t t[16];
    memcpy(t, d + 8, 4);
    memcpy(t + 4, src + 8, 4);
    memcpy(t + 8, d + 12, 4);
    memcpy(t + 12, src + 12, 4);
    memcpy(d, t, 16);
}
// pred: 0 eq, 1 lt, 2 le, 3 unord, 4 neq, 5 nlt, 6 nle, 7 ord
inline void CMPPS(uint8_t* d, const uint8_t* src, int pred, int lanes)
{
    float a[4], b[4];
    uint32_t r[4];
    memcpy(a, d, 16);
    memcpy(b, src, lanes * 4);
    for (int i = 0; i < lanes; ++i)
    {
        bool nan = a[i] != a[i] || b[i] != b[i], v = false;
        switch (pred)
        {
        case 0: v = a[i] == b[i]; break;
        case 1: v = a[i] < b[i]; break;
        case 2: v = a[i] <= b[i]; break;
        case 3: v = nan; break;
        case 4: v = !(a[i] == b[i]); break;
        case 5: v = !(a[i] < b[i]); break;
        case 6: v = !(a[i] <= b[i]); break;
        default: v = !nan; break;
        }
        r[i] = v ? 0xffffffffu : 0u;
    }
    memcpy(d, r, lanes * 4);
}
inline uint32_t MOVMSKPS(const uint8_t* src)
{
    uint32_t w[4], m = 0;
    memcpy(w, src, 16);
    for (int i = 0; i < 4; ++i) m |= (w[i] >> 31) << i;
    return m;
}
inline uint32_t LIFT_ROL(uint32_t v, unsigned c, int bits) { c &= 31; uint32_t m = lift_mask(bits); v &= m; c %= bits; return c ? (((v << c) | (v >> (bits - c))) & m) : v; }
inline uint32_t LIFT_ROR(uint32_t v, unsigned c, int bits) { c &= 31; uint32_t m = lift_mask(bits); v &= m; c %= bits; return c ? (((v >> c) | (v << (bits - c))) & m) : v; }
inline void UNPCKLPS(uint8_t* d, const uint8_t* src)
{
    uint8_t t[16];
    memcpy(t, d, 4);
    memcpy(t + 4, src, 4);
    memcpy(t + 8, d + 4, 4);
    memcpy(t + 12, src + 4, 4);
    memcpy(d, t, 16);
}
