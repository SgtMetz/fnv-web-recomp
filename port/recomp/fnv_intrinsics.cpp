// Host implementations of the exe's CRT memory and string primitives (addresses: tools/recomp.py INTRINSICS, from
// port/compat/crt_map.json). They replace the recompiled bodies: native memmove is far faster than lifted x86, and the
// originals dispatch through unrolled jump tables. All are cdecl: arguments at esp+4, result in eax; the caller pops.
// "C" locale semantics, as the game's CRT runs without setlocale.
#include "fnv_recomp.h"

#include <ctype.h>
#include <wchar.h>

#define A(i) M32(s.esp + 4u + 4u * (i))
#define P(i) ((char*)(uintptr_t)A(i))
#define CP(i) ((const char*)(uintptr_t)A(i))
#define R(v) (s.eax = (uint32_t)(uintptr_t)(v))

void fnv_i_memmove(FnvCpu& s) { memmove(P(0), CP(1), A(2)); R(A(0)); }
void fnv_i_memset(FnvCpu& s) { memset(P(0), (int)(A(1) & 0xff), A(2)); R(A(0)); }
void fnv_i_memcmp(FnvCpu& s) { int r = memcmp(CP(0), CP(1), A(2)); R(r < 0 ? -1 : r > 0 ? 1 : 0); }
void fnv_i_strlen(FnvCpu& s) { R(strlen(CP(0))); }
void fnv_i_strcpy(FnvCpu& s) { strcpy(P(0), CP(1)); R(A(0)); }
void fnv_i_strcat(FnvCpu& s) { strcat(P(0), CP(1)); R(A(0)); }
void fnv_i_strcmp(FnvCpu& s) { int r = strcmp(CP(0), CP(1)); R(r < 0 ? -1 : r > 0 ? 1 : 0); }
void fnv_i_strncmp(FnvCpu& s) { int r = strncmp(CP(0), CP(1), A(2)); R(r < 0 ? -1 : r > 0 ? 1 : 0); }
void fnv_i_strncpy(FnvCpu& s) { strncpy(P(0), CP(1), A(2)); R(A(0)); }
void fnv_i_strchr(FnvCpu& s) { const char* r = strchr(CP(0), (int)(A(1) & 0xff)); R(r); }
void fnv_i_strrchr(FnvCpu& s) { const char* r = strrchr(CP(0), (int)(A(1) & 0xff)); R(r); }
void fnv_i_strstr(FnvCpu& s) { const char* r = strstr(CP(0), CP(1)); R(r); }
void fnv_i_strpbrk(FnvCpu& s) { const char* r = strpbrk(CP(0), CP(1)); R(r); }
void fnv_i_wcslen(FnvCpu& s)
{
    const uint16_t* p = (const uint16_t*)(uintptr_t)A(0);
    uint32_t n = 0;
    while (p[n]) ++n;
    R(n);
}
