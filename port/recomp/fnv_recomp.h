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

// call: the original return address goes on the guest stack, then the callee's host function runs; its `ret N`
// pops that address and N argument bytes. Direct calls name the function; indirect ones dispatch on the address.
#define RCALL(fn, ret) do { PUSH(ret); fn(s); } while (0)
#define RCALLI(target, ret) do { uint32_t t__ = (uint32_t)(target); PUSH(ret); fnv_dispatch(s, t__); } while (0)
// tail jump: the target's ret returns for both
#define RJMP(fn) do { fn(s); return; } while (0)
#define TAILCALL(target) do { fnv_dispatch(s, (uint32_t)(uintptr_t)(target)); return; } while (0)
#define RETURN(nbytes) do { s.esp += 4u + (uint32_t)(nbytes); return; } while (0)

// traps emitted by the Phase 1 code paths the recompiler reuses
#define fnv_trap(a, w) fnv_rtrap(s, (a), (w))
#define fnv_trap_soft(a, w) fnv_rtrap_soft((a), (w))
