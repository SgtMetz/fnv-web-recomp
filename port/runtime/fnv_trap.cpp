// Called by generated trap stubs (out/link/traps.cpp) for code the relink cannot bind yet: functions in units that
// do not compile, references into out-of-scope original code, and names nothing defines. Logs once and exits so a
// run shows exactly which binding is missing.
#include <windows.h>
#include <stdio.h>

extern "C" __declspec(noinline) void __cdecl fnv_trap(unsigned int address, const char* name)
{
    char buf[512];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "[fnv] trap: unbound %s 0x%08x (%s)\n",
                address ? "address" : "name", address, name ? name : "?");
    OutputDebugStringA(buf);
    fputs(buf, stderr);
    fflush(stderr);
    ExitProcess(0xF17E0000u | (address & 0xffff));
}
