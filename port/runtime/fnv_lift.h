// Machine state and helpers for code lifted by tools/lift_stubs.py (out-of-scope initializers, atexit thunks and
// gap functions of the original exe). Memory operands are real addresses: the data image is mapped at its original
// location. Each lifted body runs on its own emulated stack, seeded with a copy of the caller's native argument words.
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>

extern "C" void __cdecl fnv_trap(unsigned int address, const char* name);
extern "C" void __cdecl fnv_trap_soft(unsigned int address, const char* name);
extern "C" uint32_t __cdecl fnv_lift_tls_array(void);
extern "C" uint8_t __cdecl fnv_lift_code8(uint32_t addr);
// FNV_FPU_CHECK=1 (fnv_lift_runtime.cpp): the naked entries of lifted functions call these (lifted address pushed,
// all registers and the x87 stack preserved) to check that the native x87 stack is empty on entry and to note what
// the function leaves on it at exit; a stack that grew in between is logged with the last checkpoints.
extern "C" int fnv_fpu_check_on;   // nonzero until the first hook finds FNV_FPU_CHECK unset
extern "C" void fnv_fpu_entry_hook(void);
extern "C" void fnv_fpu_exit_hook(void);
#include <intrin.h>
#include <xmmintrin.h>

extern "C" {
struct FnvLiftCall
{
    unsigned int eax, edx, popped;
    int fpush;
    double st0;
    unsigned int in_edx, in_ebx, in_esi, in_edi;   // register inputs besides ecx (fastcall edx, compiler-internal
                                                   // conventions that pass values in ebx/esi/edi)
    unsigned int in_eax;                           // LTCG helpers also take an argument in eax
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
    uint32_t in_ebx, in_esi, in_edi;   // the caller's ebx/esi/edi at entry (filled by the naked entry)
    uint32_t in_eax;                   // and its eax (LTCG helpers take an argument there)
};

static const int kLiftArgWords = 32;   // native argument words copied in (return address included)
static const int kLiftStackWords = 8192; // emulated frame storage; covers large shader compiler frames

// Emulated frames come from a per-thread arena, not the native stack: game threads run on small stacks and lifted calls nest.
struct FnvLiftArena
{
    char* base;
    size_t top;
    size_t committed;
    size_t size;
};
FnvLiftArena& fnv_lift_arena();   // fnv_lift_runtime.cpp

struct FnvLiftState
{
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    int zf, sf, cf, of, pf;
    uint32_t fs0;                    // fs:[0] (SEH chain head) as seen by the lifted code
    int c0, c2, c3;                  // x87 condition codes from the last compare
    double st[8];
    int depth;                       // x87 stack depth (st[depth-1] is ST(0))
    alignas(16) uint8_t xmm[8][16];
    uint8_t mm[8][8];                // MMX registers
    uint32_t* stack;
    size_t arena_mark;

    FnvLiftState(uint32_t in_ecx, uint32_t in_edx, const uint32_t* in_args)
        : eax(0), ecx(in_ecx), edx(in_edx), ebx(0), ebp(0), esi(0), edi(0), zf(0), sf(0), cf(0), of(0), pf(0),
          fs0(0), c0(0), c2(0), c3(0), depth(0)
    {
        FnvLiftArena& a = fnv_lift_arena();
        arena_mark = a.top;
        stack = (uint32_t*)(a.base + a.top);
        a.top += kLiftStackWords * 4;
        // [esp] = return address, [esp+4..] = the caller's arguments
        uint32_t* top = &stack[kLiftStackWords - kLiftArgWords - 8];
        memcpy(top, in_args, kLiftArgWords * 4);
        esp = (uint32_t)(uintptr_t)top;
    }
    ~FnvLiftState() { fnv_lift_arena().top = arena_mark; }
    FnvLiftState(const FnvLiftState&) = delete;
};

#define LIFT_STATE(c, d, a, o) FnvLiftState s((c), (d), (a)); FnvLiftReturn* lift_out__ = (o); \
    s.ebx = lift_out__->in_ebx; s.esi = lift_out__->in_esi; s.edi = lift_out__->in_edi; s.eax = lift_out__->in_eax
#define RETURN(nbytes) do { lift_out__->eax = s.eax; lift_out__->edx = s.edx; lift_out__->fpush = s.depth > 0; \
    lift_out__->pop = (uint32_t)(nbytes); lift_out__->st0 = s.depth > 0 ? ST(0) : 0.0; return; } while (0)

#define M8(a) (*(uint8_t*)(uintptr_t)(a))
#define M16(a) (*(uint16_t*)(uintptr_t)(a))
#define M32(a) (*(uint32_t*)(uintptr_t)(a))
#define M64(a) (*(uint64_t*)(uintptr_t)(a))
#define MF(a) (*(float*)(uintptr_t)(a))
#define MD(a) (*(double*)(uintptr_t)(a))
#define X(i) (s.xmm[i])
#define MMX(i) (s.mm[i])
#define PUSH(v) do { uint32_t v__ = (uint32_t)(v); s.esp -= 4; M32(s.esp) = v__; } while (0)
#define POP() (s.esp += 4, M32(s.esp - 4))
#define ST(i) (s.st[s.depth - 1 - (i)])
#define FPUSH(v) do { double v__ = (v); s.st[s.depth++] = v__; } while (0)
#define FPOP() (s.depth--)

#include "fnv_lift_ops.h"

// The caller's emulated ebp, for the few lifted functions that read EBP on entry: CRT/SEH funclets the parent calls
// with `call` and that address the parent's frame (0xecaf0c, the finally block of __ehvec_copy_ctor 0xecaebb, reads
// [ebp-0x20]). The lifter loads s.ebp from here only in those bodies (LIFT_EBP_IN).
extern thread_local uint32_t fnv_lift_ebp_in;
// Called from lifted code (through fnv_lift_call, recognised by the return address) the parent frame is the
// lifted caller's emulated ebp; called from native code (the CRT unwinder runs __finally funclets such as 0xecf7ac
// with ebp = the parent's frame) it is the native caller's ebp, which the naked entry saved just below in_args.
extern "C" uint32_t __cdecl fnv_lift_call_return(void);
#define LIFT_EBP_IN() (s.ebp = in_args[0] == fnv_lift_call_return() ? fnv_lift_ebp_in : in_args[-1])

// Thread host time slice (port/loader/loader.cpp, FNV_THREAD_HOST): the loader's watchdog sets the flag when the
// running game thread has held the baton too long while others wait; lifted code hands it over at its next call
// (a spin loop that waits for another game thread would otherwise never let that thread run).
extern "C" volatile long fnv_lift_yield_flag;
extern "C" void (__cdecl* fnv_lift_yield_cb)(void);

inline void fnv_lift_do_call(FnvLiftState& s, void* fn, uint32_t args)
{
    if (fnv_lift_yield_flag)
        fnv_lift_yield_cb();
    FnvLiftCall r;
    fnv_lift_ebp_in = s.ebp;
    r.in_edx = s.edx;
    r.in_ebx = s.ebx;
    r.in_esi = s.esi;
    r.in_edi = s.edi;
    r.in_eax = s.eax;
    fnv_lift_call(fn, s.ecx, (const unsigned int*)(uintptr_t)args, &r);
    s.eax = r.eax;
    s.edx = r.edx;
    s.esp += r.popped;
    if (r.fpush)
        s.st[s.depth++] = r.st0;
}
#define CALL(fn) fnv_lift_do_call(s, (fn), s.esp)
#define CALL_AT(fn, args) fnv_lift_do_call(s, (fn), (args))
// tail jump: the target's `ret N` pops the caller's arguments, so this function returns with the same pop
#define TAILCALL(fn) do { uint32_t e0__ = s.esp; fnv_lift_do_call(s, (fn), e0__ + 4u); RETURN(s.esp - e0__); } while (0)
