// Called by generated trap stubs (out/link/traps.cpp) for code the relink cannot bind yet: functions in units that
// do not compile, references into out-of-scope original code, and names nothing defines. It logs and raises
// FNV_TRAP_EXCEPTION (address, name) so the loader can attribute it (e.g. skip one initializer and keep going);
// unhandled, the loader's crash filter reports it and the process ends.
#include <windows.h>
#include <stdio.h>

#define FNV_TRAP_EXCEPTION 0xE0F17E01u

extern "C" __declspec(noinline) void __cdecl fnv_trap(unsigned int address, const char* name)
{
    char buf[512];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "[fnv] trap: unbound %s 0x%08x (%s)\n",
                address ? "address" : "name", address, name ? name : "?");
    OutputDebugStringA(buf);
    fputs(buf, stderr);
    fflush(stderr);
    ULONG_PTR args[2] = {address, (ULONG_PTR)name};
    RaiseException(FNV_TRAP_EXCEPTION, EXCEPTION_NONCONTINUABLE, 2, args);
}
