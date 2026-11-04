// Runtime support for code lifted from small out-of-scope stubs of the original exe (the compiler-generated dynamic
// initializers and atexit thunks; see tools/lift_stubs.py). Lifted code keeps an explicit machine state and calls
// real (relinked or CRT) functions through fnv_lift_call, which runs the callee on the native stack with a copy of
// the emulated stack, then reports what the callee popped (stdcall/thiscall) and any x87 return value.
#include <windows.h>
#include "fnv_lift.h"

// Original .text cannot be mapped over the loader; indexed byte tables read this separate view.
static const uint8_t* fnv_original_text;
static uint32_t fnv_original_text_base, fnv_original_text_size;
extern "C" volatile long fnv_lift_yield_flag = 0;
static void __cdecl fnv_lift_no_yield(void) { fnv_lift_yield_flag = 0; }
extern "C" void (__cdecl* fnv_lift_yield_cb)(void) = fnv_lift_no_yield;
// the loader installs its thread-host yield (FNV_THREAD_HOST) and gets the flag's address to set
extern "C" __declspec(dllexport) volatile long* __cdecl fnv_lift_set_yield(void (__cdecl* cb)(void))
{
    if (cb) fnv_lift_yield_cb = cb;
    return &fnv_lift_yield_flag;
}

extern "C" void __cdecl fnv_lift_set_text(const uint8_t* text, uint32_t base, uint32_t size)
{
    fnv_original_text = text;
    fnv_original_text_base = base;
    fnv_original_text_size = size;
}
extern "C" uint8_t __cdecl fnv_lift_code8(uint32_t addr)
{
    if (!fnv_original_text || addr < fnv_original_text_base || addr - fnv_original_text_base >= fnv_original_text_size)
    {
        fnv_trap(addr, "lifted: original text byte outside image");
        return 0;
    }
    return fnv_original_text[addr - fnv_original_text_base];
}

static __declspec(thread) FnvLiftArena fnv_lift_tl_arena;
static const size_t kLiftMainArenaBytes = 96u << 20;      // the first thread that runs lifted code (the game's main thread)
static const size_t kLiftWorkerArenaBytes = 12u << 20;    // every other thread; address space is scarce in a 32-bit process
static volatile LONG fnv_lift_arena_count;

FnvLiftArena& fnv_lift_arena()
{
    FnvLiftArena& a = fnv_lift_tl_arena;
    if (!a.base)
    {
        a.size = InterlockedIncrement(&fnv_lift_arena_count) == 1 ? kLiftMainArenaBytes : kLiftWorkerArenaBytes;
        a.base = (char*)VirtualAlloc(nullptr, a.size, MEM_RESERVE, PAGE_READWRITE);
        if (!a.base)
        {
            a.size = 2u << 20;
            a.base = (char*)VirtualAlloc(nullptr, a.size, MEM_RESERVE, PAGE_READWRITE);
        }
        if (!a.base)
            __debugbreak();
        a.top = 0;
        a.committed = 0;
    }
    size_t need = a.top + kLiftStackWords * 4 + 64;
    if (need > a.size)
        __debugbreak();                                  // lifted calls nested deeper than the arena
    while (a.committed < need)
    {
        VirtualAlloc(a.base + a.committed, 1u << 20, MEM_COMMIT, PAGE_READWRITE);
        a.committed += 1u << 20;
    }
    return a;
}

// Thread-local storage for lifted original code. It does `mov eax,[__tls_index]` on the original slot (0 in the data
// image), `mov ecx, fs:[0x2c]`, `mov edx,[ecx+eax*4]`, then `[edx + original .tls offset]`. The DLL carries the
// original template in .tls$FNV (start marker fnv_tls_fnv_start), so fs:[0x2c] becomes a per-thread one-entry
// array holding this thread's address of that copy.
extern "C" __declspec(thread) char fnv_tls_fnv_start;
static __declspec(thread) void* fnv_lift_tls_slot;

extern "C" uint32_t __cdecl fnv_lift_tls_array(void)
{
    fnv_lift_tls_slot = &fnv_tls_fnv_start;
    return (uint32_t)(uintptr_t)&fnv_lift_tls_slot;
}

thread_local uint32_t fnv_lift_ebp_in;   // see fnv_lift.h

// the return address of fnv_lift_call's `call dword ptr [ebp+8]` (ff 55 08): a callee whose return address is this
// was called by lifted code (LIFT_EBP_IN)
extern "C" void __cdecl fnv_lift_call(void* fn, unsigned int ecx, const unsigned int* args, FnvLiftCall* out);
extern "C" uint32_t __cdecl fnv_lift_call_return(void)
{
    static uint32_t ret = 0;
    if (!ret)
    {
        const uint8_t* p = (const uint8_t*)&fnv_lift_call;
        for (int i = 0; i < 256; ++i)
            if (p[i] == 0xff && p[i + 1] == 0x55 && p[i + 2] == 0x08) { ret = (uint32_t)(uintptr_t)(p + i + 3); break; }
    }
    return ret;
}

// ---- FNV_FPU_CHECK=1: native x87 stack balance checks (see fnv_lift.h) ----
// Lifted bodies keep their own x87 stack, so the native one is touched only by compiled code, by each naked entry's
// `fld` of an st0 result and by fnv_lift_call's `fstp` of a native callee's result. Every lifted entry/exit and every
// native call from lifted code is a checkpoint: the expected native depth there is known (0 on entry, 0 or 1 at exit
// after the result is loaded, the caller then owning that value). A depth above the previous checkpoint's baseline
// means the code that ran in between (compiled code, or the caller of a lifted function that returned st0) left
// values on the x87 stack; that interval is logged once per (from, to) pair with this thread's recent checkpoints.
#include <stdio.h>
extern "C" int fnv_fpu_check_on = 1;   // 1: FNV_FPU_CHECK not read yet, 2: on, 0: off

struct FnvFpuEnv { uint16_t cw, r0, sw, r1, tw, r2; uint32_t fip, fcs, fdp, fds; };
struct FnvFpuMark { char kind; uint32_t addr, ret; int depth, baseline; };
static const int kFpuRing = 12;
static __declspec(thread) FnvFpuMark fnv_fpu_ring[kFpuRing];
static __declspec(thread) unsigned fnv_fpu_n;
static volatile LONG fnv_fpu_lock, fnv_fpu_reports;
static uint64_t fnv_fpu_seen[2048];
static unsigned fnv_fpu_nseen;

static bool fnv_fpu_enabled()
{
    if (fnv_fpu_check_on == 1)
    {
        char v[8] = {0};
        bool on = GetEnvironmentVariableA("FNV_FPU_CHECK", v, sizeof(v)) && v[0] == '1';
        if (on)
        {
            fprintf(stderr, "[fpu] FNV_FPU_CHECK on: checking native x87 stack balance at lifted entries/exits\n");
            fflush(stderr);
        }
        fnv_fpu_check_on = on ? 2 : 0;
    }
    return fnv_fpu_check_on == 2;
}
// fnstenv masks all x87 exceptions after storing; fldenv puts the state back exactly
static void fnv_fpu_env(FnvFpuEnv* e)
{
    __asm {
        mov eax, e
        fnstenv [eax]
        fldenv [eax]
    }
}
static int fnv_fpu_depth(const FnvFpuEnv& e)
{
    int n = 0;
    for (int i = 0; i < 8; ++i)
        n += ((e.tw >> (2 * i)) & 3) != 3;
    return n;
}
static const FnvFpuMark* fnv_fpu_last() { return fnv_fpu_n ? &fnv_fpu_ring[(fnv_fpu_n - 1) % kFpuRing] : nullptr; }
static void fnv_fpu_mark(char kind, uint32_t addr, uint32_t ret, int depth, int baseline)
{
    FnvFpuMark& m = fnv_fpu_ring[fnv_fpu_n++ % kFpuRing];
    m.kind = kind; m.addr = addr; m.ret = ret; m.depth = depth; m.baseline = baseline;
}
static bool fnv_fpu_once(uint64_t key)
{
    while (InterlockedCompareExchange(&fnv_fpu_lock, 1, 0)) {}
    bool fresh = true;
    for (unsigned i = 0; i < fnv_fpu_nseen && fresh; ++i)
        fresh = fnv_fpu_seen[i] != key;
    if (fresh && fnv_fpu_nseen < 2048)
        fnv_fpu_seen[fnv_fpu_nseen++] = key;
    InterlockedExchange(&fnv_fpu_lock, 0);
    return fresh && InterlockedIncrement(&fnv_fpu_reports) <= 300;
}
static const char* fnv_fpu_kind(char k)
{
    return k == 'E' ? "entry of lifted" : k == 'X' ? "exit of lifted" : k == 'P' ? "exit (st0 pushed) of lifted"
         : "native call from lifted code to";
}
static void fnv_fpu_report(const char* what, char kind, uint32_t addr, uint32_t ret, const FnvFpuEnv& e, int depth)
{
    const FnvFpuMark* p = fnv_fpu_last();
    LONG nrep = fnv_fpu_reports;
    fprintf(stderr, "[fpu] %s: %s 0x%08x (ret %08x) x87 depth %d top %u tags %04x tid %lu; previous checkpoint: ",
            what, fnv_fpu_kind(kind), addr, ret, depth, (e.sw >> 11) & 7, e.tw, GetCurrentThreadId());
    if (p)
        fprintf(stderr, "%s 0x%08x (ret %08x) depth %d baseline %d\n", fnv_fpu_kind(p->kind), p->addr, p->ret,
                p->depth, p->baseline);
    else
        fprintf(stderr, "none\n");
    if (nrep <= 20)   // context for the first reports: this thread's recent checkpoints, oldest first
    {
        unsigned n = fnv_fpu_n < (unsigned)kFpuRing ? fnv_fpu_n : (unsigned)kFpuRing;
        for (unsigned i = fnv_fpu_n - n; i < fnv_fpu_n; ++i)
        {
            const FnvFpuMark& m = fnv_fpu_ring[i % kFpuRing];
            fprintf(stderr, "[fpu]    ... %c 0x%08x ret %08x depth %d baseline %d\n", m.kind, m.addr, m.ret, m.depth,
                    m.baseline);
        }
    }
    fflush(stderr);
}
// the previous checkpoint's baseline vs the depth now: growth = something in between left values on the stack
static void fnv_fpu_check_growth(char kind, uint32_t addr, uint32_t ret, const FnvFpuEnv& e, int depth)
{
    const FnvFpuMark* p = fnv_fpu_last();
    int base = p ? p->baseline : 0;
    if (depth > base || (!p && depth))
    {
        uint64_t key = ((uint64_t)(p ? p->ret : 0) << 32) ^ ((uint64_t)(p ? p->addr : 0) << 16) ^ addr ^ ((uint64_t)kind << 60);
        if (fnv_fpu_once(key))
            fnv_fpu_report("x87 stack grew", kind, addr, ret, e, depth);
    }
}

extern "C" void __cdecl fnv_fpu_entry_c(uint32_t addr, uint32_t ret)
{
    if (!fnv_fpu_enabled())
        return;
    FnvFpuEnv e;
    fnv_fpu_env(&e);
    int depth = fnv_fpu_depth(e);
    fnv_fpu_check_growth('E', addr, ret, e, depth);
    if (depth && fnv_fpu_once(((uint64_t)1 << 62) ^ addr))
        fnv_fpu_report("x87 stack not empty", 'E', addr, ret, e, depth);
    fnv_fpu_mark('E', addr, ret, depth, depth);
}
extern "C" void __cdecl fnv_fpu_exit_c(uint32_t addr, uint32_t ret, uint32_t fpush)
{
    if (!fnv_fpu_enabled())
        return;
    FnvFpuEnv e;
    fnv_fpu_env(&e);
    int depth = fnv_fpu_depth(e);
    const FnvFpuMark* p = fnv_fpu_last();
    int base = p ? p->baseline : 0;
    if (depth != base + (fpush ? 1 : 0) && fnv_fpu_once(((uint64_t)2 << 62) ^ addr))
        fnv_fpu_report(fpush ? "exit depth != baseline + 1" : "exit depth != baseline", fpush ? 'P' : 'X', addr, ret, e,
                       depth);
    // the caller owns a pushed result: the baseline after it pops it is one less
    fnv_fpu_mark(fpush ? 'P' : 'X', addr, ret, depth, depth - (fpush ? 1 : 0) < 0 ? 0 : depth - (fpush ? 1 : 0));
}
// fnv_lift_call, after the native callee returned (before it pops a pushed result)
extern "C" void __cdecl fnv_fpu_call_c(uint32_t fn, uint32_t ret, uint32_t sw_before, uint32_t sw_after)
{
    if (!fnv_fpu_enabled())
        return;
    FnvFpuEnv e;
    fnv_fpu_env(&e);
    int depth = fnv_fpu_depth(e);
    unsigned pushed = ((sw_before >> 11) - (sw_after >> 11)) & 7;
    if (pushed > 1 && fnv_fpu_once(((uint64_t)3 << 62) ^ fn))
        fnv_fpu_report(pushed == 7 ? "native callee popped the x87 stack" : "native callee pushed more than one value",
                       'C', fn, ret, e, depth);
    else
        fnv_fpu_check_growth('C', fn, ret, e, depth - (int)pushed);
    int base = depth - (pushed == 1 ? 1 : 0);
    fnv_fpu_mark('C', fn, ret, depth, base < 0 ? 0 : base);
}

extern "C" {

// called from the naked lifted entries with the lifted address pushed; ebp is the entry's frame ([ebp+4] = its caller)
__declspec(naked) void fnv_fpu_entry_hook(void)
{
    __asm {
        pushad
        pushfd
        push dword ptr [ebp + 4]
        push dword ptr [esp + 44]  // 4 (ret) + 32 (pushad) + 4 (pushfd) + 4 (pushed ret)
        call fnv_fpu_entry_c
        add esp, 8
        popfd
        popad
        ret 4
    }
}
__declspec(naked) void fnv_fpu_exit_hook(void)
{
    __asm {
        pushad
        pushfd
        push dword ptr [ebp - 32]  // FnvLiftReturn.fpush
        push dword ptr [ebp + 4]
        push dword ptr [esp + 48]
        call fnv_fpu_exit_c
        add esp, 12
        popfd
        popad
        ret 4
    }
}

// fn: callee; ecx: this/fastcall arg; args: emulated stack at the call (first argument first); out: results
__declspec(naked) void __cdecl fnv_lift_call(void* fn, unsigned int ecx, const unsigned int* args, FnvLiftCall* out)
{
    __asm {
        push ebp
        mov ebp, esp
        push ebx
        push esi
        push edi
        sub esp, 8                 // [ebp-0x14] x87 status before, [ebp-0x10] native esp before call
        fnstsw ax
        mov [ebp - 0x14], eax
        sub esp, 128               // kCopyWords * 4
        mov esi, [ebp + 16]        // args
        mov edi, esp
        mov ecx, 32
        cld
        rep movsd
        mov [ebp - 0x10], esp
        mov eax, [ebp + 20]        // out: register inputs at +24..+40
        mov edx, [eax + 24]
        mov esi, [eax + 32]
        mov edi, [eax + 36]
        mov ebx, [eax + 28]
        mov eax, [eax + 40]
        mov ecx, [ebp + 12]
        call dword ptr [ebp + 8]
        mov ebx, [ebp + 20]        // out
        mov [ebx], eax
        mov [ebx + 4], edx
        mov eax, esp
        sub eax, [ebp - 0x10]
        mov [ebx + 8], eax         // popped
        fnstsw ax
        cmp dword ptr [fnv_fpu_check_on], 0
        je no_check
        push eax
        push eax                   // x87 status after
        push dword ptr [ebp - 0x14]   // and before
        push dword ptr [ebp + 4]   // where in the lifted body
        push dword ptr [ebp + 8]   // callee
        call fnv_fpu_call_c
        add esp, 16
        pop eax
        mov ebx, [ebp + 20]
    no_check:
        mov ecx, [ebp - 0x14]
        shr eax, 11
        and eax, 7
        shr ecx, 11
        and ecx, 7
        xor edx, edx
        cmp eax, ecx               // TOP decreased by one => a value was pushed
        je no_push
        fstp qword ptr [ebx + 16]
        mov edx, 1
    no_push:
        mov [ebx + 12], edx
        lea esp, [ebp - 12]
        pop edi
        pop esi
        pop ebx
        pop ebp
        ret
    }
}

}
