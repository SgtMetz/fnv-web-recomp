// Small pieces of the original VC2008 CRT that lifted original code calls, reimplemented against the data image.
#include <stdint.h>

extern "C" void __cdecl fnv_trap(unsigned int address, const char* name);

// __security_check_cookie (0x00EC408C): original code XORs its frame with the original CRT's cookie, which lives in
// the data image at 0x011C16BC, so the check compares against that same word.
extern "C" void __fastcall fnv_check_cookie_orig(uintptr_t cookie)
{
    if (cookie != *(const volatile uint32_t*)0x011C16BCu)
        fnv_trap(0x00EC408Cu, "security cookie mismatch in lifted code");
}
