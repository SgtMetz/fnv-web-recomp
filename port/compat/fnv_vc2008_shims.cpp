// VC2008 CRT entry points the decomp references under their old names, forwarded to the modern CRT.
#include <exception>
#include <windows.h>

// VC2008 declared terminate() at global scope (?terminate@@YAXXZ); the modern CRT only has std::terminate.
extern "C" void fnv_vc2008_terminate(void) __asm__("?terminate@@YAXXZ");
extern "C" void fnv_vc2008_terminate(void)
{
    std::terminate();
}
// OpenAI-authored Phase 1 shim: the recovered ShaderValue layout has one 32-bit filter value;
// its default constructor initializes that value to zero.
extern "C" void* __thiscall fnv_shader_value_ctor(void* self) __asm__("??0ShaderValue@fnv_00bc58a0@@QAE@XZ");
extern "C" void* __thiscall fnv_shader_value_ctor(void* self)
{
    *(unsigned int*)self = 0;
    return self;
}

// OpenAI-authored Phase 1 shim: the recovered startup caller can pass a null GetTaskletValue output.
// Preserve the original implementation for calls with a valid destination.
extern "C" bool __thiscall fnv_tasklet_value_original(void* self, void* result)
    __asm__("fnv_f_00b00a80__fnvorig");
extern "C" bool __thiscall fnv_tasklet_value(void* self, void* result)
    __asm__("fnv_f_00b00a80");
extern "C" bool __thiscall fnv_tasklet_value(void* self, void* result)
{
    if (!result)
        return false;
    MEMORY_BASIC_INFORMATION mbi = {};
    if (!VirtualQuery(result, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT)
        return false;
    DWORD protect = mbi.Protect & 0xff;
    if (protect != PAGE_READWRITE && protect != PAGE_WRITECOPY &&
        protect != PAGE_EXECUTE_READWRITE && protect != PAGE_EXECUTE_WRITECOPY)
        return false;
    return fnv_tasklet_value_original(self, result);
}
