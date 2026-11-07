// Interface between the portable recompiler runtime (fnv_recomp_rt.cpp) and a platform host (host_win32.cpp now,
// the browser host later).
#pragma once
#include "fnv_recomp.h"

typedef void (*HostFn)(FnvCpu& s);   // reads its arguments at esp+4, sets eax, pops like the function it replaces
static const uint32_t kHostFnMax = 4096;
static const uint32_t kGuestReturnSentinel = 0xfffffff0u;

// runtime (portable)
FnvFn fnv_lookup(uint32_t addr);
uint32_t fnv_function_count();
uint32_t fnv_host_register(const char* name, HostFn fn);   // returns the guest address of the host function
const char* fnv_host_name(uint32_t addr);
uint32_t fnv_host_current();                               // index of the host function being dispatched
uint32_t fnv_guest_call(FnvCpu& s, uint32_t fn, const uint32_t* args, int nargs);

// platform
uint32_t fnv_host_base();                                  // start of the reserved host-function address range
void fnv_host_unknown(FnvCpu& s, uint32_t addr);           // dispatch to an address nobody claims
void fnv_host_log(const char* text);
[[noreturn]] void fnv_host_fatal(FnvCpu& s, uint32_t addr, const char* what);

// stdcall helpers for host functions
inline uint32_t harg(FnvCpu& s, int i) { return M32(s.esp + 4u + 4u * (uint32_t)i); }
inline void hret(FnvCpu& s, uint32_t eax, int nargs) { s.eax = eax; s.esp += 4u + 4u * (uint32_t)nargs; }
