// Machine state and calling convention for the whole-game recompiler (tools/recomp.py, docs/RECOMP.md).
// Every original function 0xXXXXXXXX becomes `void f_XXXXXXXX(FnvCpu& s)`. Guest memory is host memory at the
// original addresses (native: the process address space; wasm32: linear memory from 0), guest stacks included, so a
// guest pointer is used directly. The same generated code builds for native x86 (clang-cl) and for Emscripten.
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>

struct FnvCpu
{
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    int zf, sf, cf, of, pf, df;
    int c0, c2, c3;                  // x87 condition codes from the last compare
    double st[8];
    int depth;                       // x87 stack depth (st[depth-1] is ST(0))
    alignas(16) uint8_t xmm[8][16];
    uint8_t mm[8][8];
    uint32_t fs_base;                // guest address of this thread's TEB block (fs:[0] = SEH chain head)
    void* host;                     // the host's per-thread data (fnv_recomp_rt.cpp)
};

typedef void (*FnvFn)(FnvCpu&);

extern "C" {
void fnv_dispatch(FnvCpu& s, uint32_t addr);                  // indirect call/jump to a guest address
void fnv_rtrap(FnvCpu& s, uint32_t addr, const char* what);   // unrecoverable: report and stop the guest
void fnv_rtrap_soft(uint32_t addr, const char* what);         // report once, continue
void fnv_register_functions(const uint32_t* addrs, const FnvFn* fns, uint32_t n);   // one table per shard
}

// Call trace (compile with -DFNV_CALL_TRACE): every recompiled function records its address on entry, so two builds
// (native and wasm) can be compared until they diverge (fnv_recomp_rt.cpp writes the main thread's sequence).
#ifdef FNV_CALL_TRACE
extern "C" void fnv_trace_enter(uint32_t addr);
#define FNV_ENTER(a) fnv_trace_enter(a)
#else
#define FNV_ENTER(a) ((void)0)
#endif

#define M8(a) (*(uint8_t*)(uintptr_t)(uint32_t)(a))
#define M16(a) (*(uint16_t*)(uintptr_t)(uint32_t)(a))
#define M32(a) (*(uint32_t*)(uintptr_t)(uint32_t)(a))
#define M64(a) (*(uint64_t*)(uintptr_t)(uint32_t)(a))
#define MF(a) (*(float*)(uintptr_t)(uint32_t)(a))
#define MD(a) (*(double*)(uintptr_t)(uint32_t)(a))
#define X(i) (s.xmm[i])
#define MMX(i) (s.mm[i])
#define PUSH(v) do { uint32_t v__ = (uint32_t)(v); s.esp -= 4; M32(s.esp) = v__; } while (0)
#define POP() (s.esp += 4, M32(s.esp - 4))
#define ST(i) (s.st[s.depth - 1 - (i)])
#define FPUSH(v) do { double v__ = (v); s.st[s.depth++] = v__; } while (0)
#define FPOP() (s.depth--)

#include "../runtime/fnv_lift_ops.h"

// x86 intrinsics the lifted code uses (rdtsc 908x, cpuid 6x, MXCSR 8x): native on x86, emulated elsewhere
#if defined(__i386__) || defined(_M_IX86)
#include <intrin.h>
#include <xmmintrin.h>
#else
uint64_t fnv_host_rdtsc(void);                          // host clock in "cycles"
void fnv_host_cpuid(int out[4], int leaf, int sub);     // the CPU the game should see (SSE2, no newer)
inline uint64_t __rdtsc(void) { return fnv_host_rdtsc(); }
inline void __cpuidex(int out[4], int leaf, int sub) { fnv_host_cpuid(out, leaf, sub); }
inline uint32_t& fnv_mxcsr() { static thread_local uint32_t v = 0x1f80u; return v; }
inline uint32_t _mm_getcsr(void) { return fnv_mxcsr(); }
inline void _mm_setcsr(uint32_t v) { fnv_mxcsr() = v; }
#endif

// Registers in locals. A recompiled function body names its machine state `s`, which is an FnvLocal over the
// thread's FnvCpu (`s_`): the integer registers and the flags the body uses are plain locals, so the compiler keeps
// them in registers. Through the FnvCpu they would be reloaded after every guest store, since guest memory may alias
// anything (the build has no strict aliasing). The locals go back to the FnvCpu before anything else can see it (a
// call, a return, a tail jump, a trap) and are reloaded after a call returns. M (tools/recomp.py) says which fields
// the body uses: bit 0-7 eax..edi, bit 8 zf/sf/cf/of/pf, bit 9 df. A field outside M is never touched, so the
// FnvCpu's value passes through unchanged. x87, SSE, MMX and fs_base stay in the FnvCpu (references).
template <unsigned M>
struct FnvLocal
{
    FnvCpu& c;
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    int zf, sf, cf, of, pf, df;
    int &c0, &c2, &c3;
    double (&st)[8];
    int& depth;
    uint8_t (&xmm)[8][16];
    uint8_t (&mm)[8][8];
    uint32_t& fs_base;
    explicit FnvLocal(FnvCpu& c_)
        : c(c_), c0(c_.c0), c2(c_.c2), c3(c_.c3), st(c_.st), depth(c_.depth), xmm(c_.xmm), mm(c_.mm), fs_base(c_.fs_base)
    {
        in();
    }
    inline __attribute__((always_inline)) void in()
    {
        if (M & 1) eax = c.eax;
        if (M & 2) ecx = c.ecx;
        if (M & 4) edx = c.edx;
        if (M & 8) ebx = c.ebx;
        if (M & 16) esp = c.esp;
        if (M & 32) ebp = c.ebp;
        if (M & 64) esi = c.esi;
        if (M & 128) edi = c.edi;
        if (M & 256) { zf = c.zf; sf = c.sf; cf = c.cf; of = c.of; pf = c.pf; }
        if (M & 512) df = c.df;
    }
    inline __attribute__((always_inline)) void out()
    {
        if (M & 1) c.eax = eax;
        if (M & 2) c.ecx = ecx;
        if (M & 4) c.edx = edx;
        if (M & 8) c.ebx = ebx;
        if (M & 16) c.esp = esp;
        if (M & 32) c.ebp = ebp;
        if (M & 64) c.esi = esi;
        if (M & 128) c.edi = edi;
        if (M & 256) { c.zf = zf; c.sf = sf; c.cf = cf; c.of = of; c.pf = pf; }
        if (M & 512) c.df = df;
    }
};
inline FnvCpu& fnv_c(FnvCpu& s) { return s; }
inline void fnv_out(FnvCpu&) {}
inline void fnv_in(FnvCpu&) {}
template <unsigned M> inline FnvCpu& fnv_c(FnvLocal<M>& s) { return s.c; }
template <unsigned M> inline __attribute__((always_inline)) void fnv_out(FnvLocal<M>& s) { s.out(); }
template <unsigned M> inline __attribute__((always_inline)) void fnv_in(FnvLocal<M>& s) { s.in(); }

// call: the original return address goes on the guest stack, then the callee's host function runs; its `ret N`
// pops that address and N argument bytes. Direct calls name the function; indirect ones dispatch on the address.
#define RCALL(fn, ret) do { PUSH(ret); fnv_out(s); fn(fnv_c(s)); fnv_in(s); } while (0)
#define RCALLI(target, ret) do { uint32_t t__ = (uint32_t)(target); PUSH(ret); fnv_out(s); fnv_dispatch(fnv_c(s), t__); fnv_in(s); } while (0)
// tail jump: the target's ret returns for both
#define RJMP(fn) do { fnv_out(s); fn(fnv_c(s)); return; } while (0)
#define TAILCALL(target) do { uint32_t t__ = (uint32_t)(uintptr_t)(target); fnv_out(s); fnv_dispatch(fnv_c(s), t__); return; } while (0)
#define RETURN(nbytes) do { s.esp += 4u + (uint32_t)(nbytes); fnv_out(s); return; } while (0)

// traps emitted by the Phase 1 code paths the recompiler reuses
#define fnv_trap(a, w) (fnv_out(s), fnv_rtrap(fnv_c(s), (a), (w)))
#define fnv_trap_soft(a, w) fnv_rtrap_soft((a), (w))
