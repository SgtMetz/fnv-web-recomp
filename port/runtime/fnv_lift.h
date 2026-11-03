// Machine state and helpers for code lifted by tools/lift_stubs.py (out-of-scope initializers, atexit thunks and
// gap functions of the original exe). Memory operands are real addresses: the data image is mapped at its original
// location. Each lifted body runs on its own emulated stack, seeded with a copy of the caller's native argument words.
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>

extern "C" void __cdecl fnv_trap(unsigned int address, const char* name);
extern "C" uint32_t __cdecl fnv_lift_tls_array(void);
#include <intrin.h>

extern "C" {
struct FnvLiftCall
{
    unsigned int eax, edx, popped;
    int fpush;
    double st0;
};
void __cdecl fnv_lift_call(void* fn, unsigned int ecx, const unsigned int* args, FnvLiftCall* out);
}

// What a lifted body hands back to its naked entry (layout used by the entry's asm: eax, edx, fpush, pad, st0).
struct FnvLiftReturn
{
    uint32_t eax, edx;
    int32_t fpush;
    uint32_t pop;
    double st0;
};

static const int kLiftArgWords = 32;   // native argument words copied in (return address included)
static const int kLiftStackWords = 8192; // emulated frame storage; covers large shader compiler frames

struct FnvLiftState
{
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    int zf, sf, cf, of, pf;
    uint32_t fs0;                    // fs:[0] (SEH chain head) as seen by the lifted code
    int c0, c2, c3;                  // x87 condition codes from the last compare
    double st[8];
    int depth;                       // x87 stack depth (st[depth-1] is ST(0))
    alignas(16) uint8_t xmm[8][16];
    alignas(16) uint32_t stack[kLiftStackWords];

    FnvLiftState(uint32_t in_ecx, uint32_t in_edx, const uint32_t* in_args)
        : eax(0), ecx(in_ecx), edx(in_edx), ebx(0), ebp(0), esi(0), edi(0), zf(0), sf(0), cf(0), of(0), pf(0),
          fs0(0), c0(0), c2(0), c3(0), depth(0)
    {
        // [esp] = return address, [esp+4..] = the caller's arguments
        uint32_t* top = &stack[kLiftStackWords - kLiftArgWords - 8];
        memcpy(top, in_args, kLiftArgWords * 4);
        esp = (uint32_t)(uintptr_t)top;
    }
};

#define LIFT_STATE(c, d, a, o) FnvLiftState s((c), (d), (a)); FnvLiftReturn* lift_out__ = (o)
#define RETURN(nbytes) do { lift_out__->eax = s.eax; lift_out__->edx = s.edx; lift_out__->fpush = s.depth > 0; \
    lift_out__->pop = (uint32_t)(nbytes); lift_out__->st0 = s.depth > 0 ? ST(0) : 0.0; return; } while (0)

#define M8(a) (*(uint8_t*)(uintptr_t)(a))
#define M16(a) (*(uint16_t*)(uintptr_t)(a))
#define M32(a) (*(uint32_t*)(uintptr_t)(a))
#define M64(a) (*(uint64_t*)(uintptr_t)(a))
#define MF(a) (*(float*)(uintptr_t)(a))
#define MD(a) (*(double*)(uintptr_t)(a))
#define X(i) (s.xmm[i])
#define PUSH(v) do { uint32_t v__ = (uint32_t)(v); s.esp -= 4; M32(s.esp) = v__; } while (0)
#define POP() (s.esp += 4, M32(s.esp - 4))
#define ST(i) (s.st[s.depth - 1 - (i)])
#define FPUSH(v) do { double v__ = (v); s.st[s.depth++] = v__; } while (0)
#define FPOP() (s.depth--)

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

inline void fnv_lift_do_call(FnvLiftState& s, void* fn, uint32_t args)
{
    FnvLiftCall r;
    fnv_lift_call(fn, s.ecx, (const unsigned int*)(uintptr_t)args, &r);
    s.eax = r.eax;
    s.edx = r.edx;
    s.esp += r.popped;
    if (r.fpush)
        s.st[s.depth++] = r.st0;
}
#define CALL(fn) fnv_lift_do_call(s, (fn), s.esp)
#define CALL_AT(fn, args) fnv_lift_do_call(s, (fn), (args))

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

inline void UNPCKLPS(uint8_t* d, const uint8_t* src)
{
    uint8_t t[16];
    memcpy(t, d, 4);
    memcpy(t + 4, src, 4);
    memcpy(t + 8, d + 4, 4);
    memcpy(t + 12, src + 4, 4);
    memcpy(d, t, 16);
}
