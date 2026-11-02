// Runtime support for code lifted from small out-of-scope stubs of the original exe (the compiler-generated dynamic
// initializers and atexit thunks; see tools/lift_stubs.py). Lifted code keeps an explicit machine state and calls
// real (relinked or CRT) functions through fnv_lift_call, which runs the callee on the native stack with a copy of
// the emulated stack, then reports what the callee popped (stdcall/thiscall) and any x87 return value.
#include "fnv_lift.h"

extern "C" {

// fn: callee; ecx: this/fastcall arg; args: emulated stack at the call (first argument first); out: results
__declspec(naked) void __cdecl fnv_lift_call(void* fn, unsigned int ecx, const unsigned int* args, FnvLiftCall* out)
{
    __asm {
        push ebp
        mov ebp, esp
        push ebx
        push esi
        push edi
        sub esp, 8                 // [ebp-0x14] x87 status before, [ebp-0x18] native esp before call
        fnstsw ax
        mov [ebp - 0x14], eax
        sub esp, 128               // kCopyWords * 4
        mov esi, [ebp + 16]        // args
        mov edi, esp
        mov ecx, 32
        cld
        rep movsd
        mov [ebp - 0x18], esp
        mov ecx, [ebp + 12]
        mov eax, [ebp + 8]
        call eax
        mov ebx, [ebp + 20]        // out
        mov [ebx], eax
        mov [ebx + 4], edx
        mov eax, esp
        sub eax, [ebp - 0x18]
        mov [ebx + 8], eax         // popped
        fnstsw ax
        mov ecx, [ebp - 0x14]
        shr eax, 11
        and eax, 7
        shr ecx, 11
        and ecx, 7
        xor edx, edx
        cmp eax, ecx               // TOP decreased by one => a value was pushed
        je no_push
        fstp qword ptr [ebx + 16]
        mov edx, 1
    no_push:
        mov [ebx + 12], edx
        lea esp, [ebp - 12]
        pop edi
        pop esi
        pop ebx
        pop ebp
        ret
    }
}

}
