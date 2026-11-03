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

// Discovery mode (environment FNV_DISCOVERY=1): stubs whose original function has one known `ret N` call this
// instead; it logs each address once and returns, and the stub returns 0 with the original stack cleanup, so one
// run walks past every such gap and logs all of them. Without FNV_DISCOVERY it is fnv_trap.
extern "C" __declspec(noinline) void __cdecl fnv_trap_soft(unsigned int address, const char* name)
{
    static int discovery = -1;
    if (discovery < 0) {
        char v[8] = {0};
        discovery = GetEnvironmentVariableA("FNV_DISCOVERY", v, sizeof(v)) && v[0] == '1';
    }
    if (!discovery)
        fnv_trap(address, name);
    // once per address, or per name for name-only stubs (address 0): keying those by address alone logged only the
    // first one and hid every later skipped name
    static unsigned int seen[4096];
    static unsigned int nseen = 0;
    unsigned int key = address ? address : (unsigned int)(unsigned long)name;
    for (unsigned int i = 0; i < nseen; i++)
        if (seen[i] == key)
            return;
    if (nseen < 4096)
        seen[nseen++] = key;
    fprintf(stderr, "[fnv] discover: skipped %s 0x%08x (%s)\n", address ? "address" : "name", address,
            name ? name : "?");
    fflush(stderr);
}
