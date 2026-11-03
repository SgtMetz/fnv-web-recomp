// Runtime support for code lifted from small out-of-scope stubs of the original exe (the compiler-generated dynamic
// initializers and atexit thunks; see tools/lift_stubs.py). Lifted code keeps an explicit machine state and calls
// real (relinked or CRT) functions through fnv_lift_call, which runs the callee on the native stack with a copy of
// the emulated stack, then reports what the callee popped (stdcall/thiscall) and any x87 return value.
#include <windows.h>
#include "fnv_lift.h"

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

extern "C" {

// fn: callee; ecx: this/fastcall arg; args: emulated stack at the call (first argument first); out: results
__declspec(naked) void __cdecl fnv_lift_call(void* fn, unsigned int ecx, const unsigned int* args, FnvLiftCall* out)
{
    __asm {
        push ebp
        mov ebp, esp
        push ebx
        push esi
        push edi
        sub esp, 8                 // [ebp-0x14] x87 status before, [ebp-0x18] native esp before call
        fnstsw ax
        mov [ebp - 0x14], eax
        sub esp, 128               // kCopyWords * 4
        mov esi, [ebp + 16]        // args
        mov edi, esp
        mov ecx, 32
        cld
        rep movsd
        mov [ebp - 0x18], esp
        mov eax, [ebp + 20]        // out: register inputs at +24..+36
        mov edx, [eax + 24]
        mov esi, [eax + 32]
        mov edi, [eax + 36]
        mov ebx, [eax + 28]
        mov ecx, [ebp + 12]
        mov eax, [ebp + 8]
        call eax
        mov ebx, [ebp + 20]        // out
        mov [ebx], eax
        mov [ebx + 4], edx
        mov eax, esp
        sub eax, [ebp - 0x18]
        mov [ebx + 8], eax         // popped
        fnstsw ax
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
