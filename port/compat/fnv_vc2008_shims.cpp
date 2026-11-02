// VC2008 CRT entry points the decomp references under their old names, forwarded to the modern CRT.
#include <exception>

// VC2008 declared terminate() at global scope (?terminate@@YAXXZ); the modern CRT only has std::terminate.
extern "C" void fnv_vc2008_terminate(void) __asm__("?terminate@@YAXXZ");
extern "C" void fnv_vc2008_terminate(void)
{
    std::terminate();
}
