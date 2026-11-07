// Portable part of the whole-game recompiler runtime (docs/RECOMP.md): the address -> function table that indirect
// calls dispatch through, host functions, traps. Platform code (host_win32.cpp, later the browser host) supplies
// fnv_host_unknown() for addresses that are neither lifted nor host functions, and fnv_host_log().
#include "fnv_recomp.h"
#include "fnv_recomp_host.h"

#include <stdio.h>
#include <stdlib.h>

namespace {
// open-addressing table, power-of-two size, key 0 = empty (no function lives at address 0)
uint32_t* g_keys;
FnvFn* g_vals;
uint32_t g_mask, g_count;
HostFn g_host[kHostFnMax];
const char* g_host_names[kHostFnMax];
uint32_t g_host_n;
bool g_trace_host = getenv("FNV_TRACE_IMPORTS") != nullptr;

inline uint32_t hash(uint32_t a) { a ^= a >> 15; a *= 0x2c1b3c6du; a ^= a >> 12; return a; }

void grow()
{
    uint32_t n = g_mask ? (g_mask + 1) * 2 : 1u << 18;
    uint32_t* keys = (uint32_t*)calloc(n, 4);
    FnvFn* vals = (FnvFn*)calloc(n, sizeof(FnvFn));
    for (uint32_t i = 0; g_mask && i <= g_mask; ++i)
        if (g_keys[i])
        {
            uint32_t j = hash(g_keys[i]) & (n - 1);
            while (keys[j]) j = (j + 1) & (n - 1);
            keys[j] = g_keys[i];
            vals[j] = g_vals[i];
        }
    free(g_keys);
    free(g_vals);
    g_keys = keys;
    g_vals = vals;
    g_mask = n - 1;
}
} // namespace

extern "C" void fnv_register_functions(const uint32_t* addrs, const FnvFn* fns, uint32_t n)
{
    for (uint32_t i = 0; i < n; ++i)
    {
        if ((g_count + 1) * 2 > g_mask)
            grow();
        uint32_t j = hash(addrs[i]) & g_mask;
        while (g_keys[j] && g_keys[j] != addrs[i]) j = (j + 1) & g_mask;
        if (!g_keys[j]) ++g_count;
        g_keys[j] = addrs[i];
        g_vals[j] = fns[i];
    }
}

FnvFn fnv_lookup(uint32_t addr)
{
    if (!g_mask) return nullptr;
    uint32_t j = hash(addr) & g_mask;
    while (g_keys[j])
    {
        if (g_keys[j] == addr) return g_vals[j];
        j = (j + 1) & g_mask;
    }
    return nullptr;
}

uint32_t fnv_function_count() { return g_count; }

uint32_t fnv_host_register(const char* name, HostFn fn)
{
    if (g_host_n >= kHostFnMax) { fnv_host_log("[recomp] too many host functions\n"); abort(); }
    g_host[g_host_n] = fn;
    g_host_names[g_host_n] = name;
    return fnv_host_base() + 16u * g_host_n++;
}

const char* fnv_host_name(uint32_t addr)
{
    uint32_t i = (addr - fnv_host_base()) / 16u;
    return i < g_host_n ? g_host_names[i] : "?";
}

extern "C" void fnv_dispatch(FnvCpu& s, uint32_t addr)
{
    if (FnvFn f = fnv_lookup(addr))
        return f(s);
    uint32_t hb = fnv_host_base();
    if (addr - hb < 16u * g_host_n && (addr - hb) % 16u == 0)
    {
        if (!g_trace_host)
            return g_host[(addr - hb) / 16u](s);
        uint32_t ret = M32(s.esp), a0 = M32(s.esp + 4), a1 = M32(s.esp + 8), a2 = M32(s.esp + 12);
        g_host[(addr - hb) / 16u](s);
        char buf[200];
        snprintf(buf, sizeof buf, "[host] %s(%08x, %08x, %08x) -> %08x (from %08x)%c", g_host_names[(addr - hb) / 16u], a0, a1,
                 a2, s.eax, ret, 10);
        fnv_host_log(buf);
        return;
    }
    fnv_host_unknown(s, addr);
}

extern "C" void fnv_rtrap(FnvCpu& s, uint32_t addr, const char* what)
{
    char buf[256];
    snprintf(buf, sizeof buf, "[recomp] trap at %08x: %s (eax=%08x ecx=%08x edx=%08x esp=%08x ebp=%08x)\n", addr, what,
             s.eax, s.ecx, s.edx, s.esp, s.ebp);
    fnv_host_log(buf);
    fnv_host_fatal(s, addr, what);
}

extern "C" void fnv_rtrap_soft(uint32_t addr, const char* what)
{
    static uint32_t seen[256];
    static uint32_t nseen;
    for (uint32_t i = 0; i < nseen; ++i)
        if (seen[i] == addr) return;
    if (nseen < 256) seen[nseen++] = addr;
    char buf[256];
    snprintf(buf, sizeof buf, "[recomp] warning at %08x: %s\n", addr, what);
    fnv_host_log(buf);
}

// Host -> guest call: arguments are pushed right to left (stdcall/cdecl), then a sentinel return address. Returns
// eax; the callee's pop (stdcall) or the caller's cleanup (cdecl) are both handled by restoring esp.
uint32_t fnv_guest_call(FnvCpu& s, uint32_t fn, const uint32_t* args, int nargs)
{
    uint32_t esp0 = s.esp;
    for (int i = nargs - 1; i >= 0; --i)
        PUSH(args[i]);
    PUSH(kGuestReturnSentinel);
    fnv_dispatch(s, fn);
    s.esp = esp0;
    return s.eax;
}
