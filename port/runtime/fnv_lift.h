// Machine state and helpers for code lifted by tools/lift_stubs.py (out-of-scope initializer and atexit stubs).
// Memory operands are real addresses: the data image is mapped at its original location.
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>

extern "C" void __cdecl fnv_trap(unsigned int address, const char* name);

extern "C" {
struct FnvLiftCall
{
    unsigned int eax, edx, popped;
    int fpush;
    double st0;
};
void __cdecl fnv_lift_call(void* fn, unsigned int ecx, const unsigned int* args, FnvLiftCall* out);
}

struct FnvLiftState
{
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    int zf, sf;
    uint32_t fs0;                    // fs:[0] (SEH chain head) as seen by the stub
    double st[8];
    int depth;                       // x87 stack depth (st[depth-1] is ST(0))
    alignas(16) uint8_t xmm[8][16];
    alignas(16) uint32_t stack[2048];

    FnvLiftState() : eax(0), ecx(0), edx(0), ebx(0), ebp(0), esi(0), edi(0), zf(0), sf(0), fs0(0), depth(0)
    {
        esp = (uint32_t)(uintptr_t)&stack[2040];
        esp -= 4;
        *(uint32_t*)(uintptr_t)esp = 0xFEEDF00Du;      // fake return address of the stub
    }
};

#define LIFT_STATE FnvLiftState s
#define M8(a) (*(uint8_t*)(uintptr_t)(a))
#define M16(a) (*(uint16_t*)(uintptr_t)(a))
#define M32(a) (*(uint32_t*)(uintptr_t)(a))
#define MF(a) (*(float*)(uintptr_t)(a))
#define MD(a) (*(double*)(uintptr_t)(a))
#define X(i) (s.xmm[i])
#define PUSH(v) do { uint32_t v__ = (uint32_t)(v); s.esp -= 4; M32(s.esp) = v__; } while (0)
#define POP() (s.esp += 4, M32(s.esp - 4))
#define FLAGS(v, bits) do { uint32_t f__ = (uint32_t)(v); \
    if ((bits) == 8) f__ &= 0xffu; else if ((bits) == 16) f__ &= 0xffffu; \
    s.zf = f__ == 0; s.sf = (int)((f__ >> ((bits) - 1)) & 1u); } while (0)
#define ST(i) (s.st[s.depth - 1 - (i)])
#define FPUSH(v) do { double v__ = (v); s.st[s.depth++] = v__; } while (0)
#define FPOP() (s.depth--)

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

inline void UNPCKLPS(uint8_t* d, const uint8_t* src)
{
    uint8_t t[16];
    memcpy(t, d, 4);
    memcpy(t + 4, src, 4);
    memcpy(t + 8, d + 4, 4);
    memcpy(t + 12, src + 4, 4);
    memcpy(d, t, 16);
}
