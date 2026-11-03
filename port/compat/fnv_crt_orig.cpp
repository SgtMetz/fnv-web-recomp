// Small pieces of the original VC2008 CRT that lifted original code calls, reimplemented against the data image.
#include <stdint.h>

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
