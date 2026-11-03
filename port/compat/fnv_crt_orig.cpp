// Small pieces of the original VC2008 CRT that lifted original code calls, reimplemented against the data image.
#include <stdint.h>
#include <stdio.h>

extern "C" void __cdecl fnv_trap(unsigned int address, const char* name);
extern "C" void __cdecl fnv_trap_soft(unsigned int address, const char* name);

static void __cdecl cookie_mismatch(void)
{
    static volatile long reported = 0;
    if (__sync_fetch_and_add(&reported, 1) < 5)
        fnv_trap_soft(0x00EC408Cu, "security cookie mismatch in lifted code (continuing)");
}

// __security_check_cookie (0x00EC408C): original code XORs its frame with the original CRT's cookie, which lives in
// the data image at 0x011C16BC, so the check compares against that same word. Like the original it takes the value in
// ecx and must preserve every other register: callers return the result of their last call in eax right after it.
extern "C" __declspec(naked) void __fastcall fnv_check_cookie_orig(uintptr_t cookie)
{
    __asm {
        cmp ecx, dword ptr ds:[0x011C16BC]
        jne bad
        ret
    bad:
        push eax
        push edx
        push ecx
        call cookie_mismatch
        pop ecx
        pop edx
        pop eax
        ret
    }
}

// _invalid_parameter (0x00EC7C30): the original would end in _invoke_watson. Log the first few and return, so the CRT
// function that detected the bad argument fails with EINVAL as it does when a handler returns.
extern "C" void __cdecl fnv_invalid_parameter_orig(const void*, const void*, const void*, unsigned int, uintptr_t)
{
    static volatile long reported = 0;
    if (__sync_fetch_and_add(&reported, 1) < 5)
        fnv_trap_soft(0x00EC7C30u, "original CRT _invalid_parameter (continuing)");
}

// __RTDynamicCast (0x00EC43FB) with a log of failed casts on non-null objects: their vtable's RTTI says why
// (an object built with a clang vftable carries clang's type names, not the original ones).
extern "C" void* __cdecl __RTDynamicCast(void* inptr, long vfdelta, void* src, void* dst, int isref);
extern "C" void* __cdecl fnv_rtdynamiccast_orig(void* inptr, long vfdelta, void* src, void* dst, int isref)
{
    void* r = __RTDynamicCast(inptr, vfdelta, src, dst, isref);
    static volatile long reported = 0;
    if (!r && inptr && __sync_fetch_and_add(&reported, 1) < 40)
    {
        uintptr_t vt = *(uintptr_t*)inptr;
        uintptr_t col = vt ? *(uintptr_t*)(vt - 4) : 0;
        const char* have = col ? (const char*)(*(uintptr_t*)(col + 12) + 8) : "?";
        const char* want = (const char*)((uintptr_t)dst + 8);
        char buf[256];
        _snprintf(buf, sizeof(buf) - 1, "dynamic_cast failed: object %p vtable %p is %s, wanted %s", inptr, (void*)vt, have, want);
        fprintf(stdout, "[fnv] %s\n", buf);
        fflush(stdout);
    }
    return r;
}
