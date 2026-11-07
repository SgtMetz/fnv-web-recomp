// Native Windows host for the whole-game recompiler, milestone 1 (docs/RECOMP.md).
//
// The player's FalloutNV.exe is mapped at its original addresses (read/write, never executable) and its original
// entry point runs as recompiled code, so the game's own static CRT starts it. Guest pointers are host pointers in
// this build. Imports are the real Win32 functions, called through `native_escape` on the guest stack, and the bytes
// the callee pops come back the way they would on x86. Native code that calls a guest code address (window procedure,
// thread start, enumeration callbacks, DirectShow calling the game's own COM objects) faults on the non-executable
// guest page; the vectored handler sends it to callback_common, which runs the recompiled function.
//
// Safety for unattended runs (same rules as the Phase 1 loader): SHGetFolderPathA(CSIDL_PERSONAL) answers the private
// out/fnv_docs folder so the player's My Games\FalloutNV is never touched; FNV_BACKGROUND=1 keeps the window
// off-screen and unfocused; ShellExecuteA is blocked.
//
// usage: fnv_recomp.exe --exe <FalloutNV.exe>   (log: out/recomp/run.log)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fnv_recomp_host.h"
#include "pal.h"

namespace {
FILE* g_log;
uint32_t g_host_base;
uint32_t g_image_lo, g_image_hi;           // the mapped exe (guest image)
uint32_t g_entry;
char g_exe_path[MAX_PATH], g_docs[MAX_PATH];
bool g_bg;
IMAGE_TLS_DIRECTORY32 g_tls;
bool g_has_tls;
uint32_t g_import_addr[512];
const char* g_import_name[512];
int g_nimport;
bool g_trace_imports;
bool g_pal;                                 // FNV_PAL=1: imports the PAL implements go to it
const char* import_name(uint32_t a)
{
    for (int i = 0; i < g_nimport; ++i)
        if (g_import_addr[i] == a) return g_import_name[i];
    return nullptr;
}

struct HostThread
{
    FnvCpu cpu;
    uint32_t teb;
    uint32_t stack_lo, stack_hi;
    uint32_t pending_cb;                    // guest address native code tried to execute (set by the VEH)
    uint32_t escape_host_esp;               // host esp saved by the innermost native_escape (0: not in one)
    uint32_t host_stack_lo, host_stack_hi;  // the thread's own (native) stack, from its real TEB
};
thread_local HostThread* t_ht;

// clang-cl has __readfsdword but no __writefsdword on x86
extern "C" __declspec(naked) void __cdecl fs_write(uint32_t off, uint32_t v)
{
    __asm {
        mov ecx, [esp + 4]
        mov eax, [esp + 8]
        mov fs:[ecx], eax
        ret
    }
}
#define __writefsdword(off, v) fs_write((off), (v))

void logf(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log ? g_log : stderr, fmt, ap);
    va_end(ap);
    fflush(g_log ? g_log : stderr);
}

uint32_t galloc(uint32_t size, DWORD protect = PAGE_READWRITE)
{
    return (uint32_t)(uintptr_t)VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, protect);
}

// ---- threads: TEB block, guest stack, the original exe's static TLS -------------------------------------------
HostThread* g_threads[256];                // every guest thread, for crash reports
long g_nthreads;
void dump_all_threads(HostThread* except);
HostThread* new_thread(uint32_t stack_size)
{
    // FnvCpu holds SSE registers (alignas(16)); calloc only guarantees 8 bytes here
    HostThread* ht = (HostThread*)_aligned_malloc(sizeof(HostThread), 64);
    long slot = InterlockedIncrement(&g_nthreads) - 1;
    if (slot < 256) g_threads[slot] = ht;
    memset(ht, 0, sizeof(HostThread));
    ht->stack_lo = galloc(stack_size);
    ht->stack_hi = ht->stack_lo + stack_size;
    ht->teb = galloc(0x1000);
    uint32_t teb = ht->teb;
    M32(teb + 0x00) = 0xffffffffu;          // SEH chain: empty
    M32(teb + 0x04) = ht->stack_hi;
    M32(teb + 0x08) = ht->stack_lo;
    M32(teb + 0x18) = teb;                  // self
    M32(teb + 0x20) = GetCurrentProcessId();
    M32(teb + 0x24) = GetCurrentThreadId();
    M32(teb + 0x30) = (uint32_t)__readfsdword(0x30);   // the real PEB
    M32(teb + 0x2c) = teb + 0x800;          // TLS array: slot 0 = this thread's copy of the exe's .tls
    if (g_has_tls)
    {
        uint32_t len = g_tls.EndAddressOfRawData - g_tls.StartAddressOfRawData;
        uint32_t block = galloc(len + g_tls.SizeOfZeroFill + 16);
        memcpy((void*)(uintptr_t)block, (void*)(uintptr_t)g_tls.StartAddressOfRawData, len);
        M32(teb + 0x800) = block;
    }
    ht->host_stack_hi = __readfsdword(4);
    ht->host_stack_lo = __readfsdword(8);
    ht->cpu.fs_base = teb;
    ht->cpu.esp = ht->stack_hi - 64;
    ht->cpu.host = ht;
    return ht;
}

void run_tls_callbacks(HostThread* ht, uint32_t reason)
{
    if (!g_has_tls || !g_tls.AddressOfCallBacks) return;
    for (uint32_t p = g_tls.AddressOfCallBacks; M32(p); p += 4)
    {
        uint32_t args[3] = {g_image_lo, reason, 0};
        fnv_guest_call(ht->cpu, M32(p), args, 3);
    }
}

void dump_all_threads(HostThread* except)
{
    for (long i = 0; i < g_nthreads && i < 256; ++i)
    {
        HostThread* ht = g_threads[i];
        if (!ht || ht == except) continue;
        char line[512];
        int pos = snprintf(line, sizeof line, "[recomp] other guest thread %ld (esp %08x):", i, ht->cpu.esp);
        int shown = 0;
        for (uint32_t p = ht->cpu.esp; p >= ht->stack_lo && p + 4 <= ht->stack_hi && shown < 8 && pos < 440; p += 4)
        {
            uint32_t v = M32(p);
            if (v >= g_image_lo + 0x1005 && v < 0x00fdf000u && M8(v - 5) == 0xe8)
            {
                pos += snprintf(line + pos, sizeof line - pos, " %08x", v);
                ++shown;
            }
        }
        logf("%s%c", line, 10);
    }
}

// ---- native escape: call a native function on the guest stack ---------------------------------------------------
struct NativeCall
{
    uint32_t fn, esp, eax, ecx, edx;        // 0 4 8 12 16
    uint32_t out_eax, out_edx, out_esp;     // 20 24 28
    uint32_t saved_esp;                     // 32
    uint32_t* host_esp_slot;                // 36
    uint32_t fpu;                           // 40: 1 when the callee returned a value in st0
    uint32_t pad;                           // 44
    double st0;                             // 48
};

extern "C" __declspec(naked) void __cdecl native_escape(NativeCall* c)
{
    __asm {
        push ebp
        mov ebp, esp
        push ebx
        push esi
        push edi
        mov ebx, [ebp + 8]
        mov [ebx + 32], esp
        mov eax, [ebx + 36]
        mov [eax], esp
        mov ecx, [ebx + 12]
        mov edx, [ebx + 16]
        mov eax, [ebx + 8]
        mov esp, [ebx + 4]
        call dword ptr [ebx]
        mov [ebx + 20], eax
        mov [ebx + 24], edx
        mov [ebx + 28], esp
        mov dword ptr [ebx + 40], 0
        fnstsw ax
        and eax, 0x3800
        jz no_st0
        fstp qword ptr [ebx + 48]
        mov dword ptr [ebx + 40], 1
    no_st0:
        mov esp, [ebx + 32]
        pop edi
        pop esi
        pop ebx
        pop ebp
        ret
    }
}

void native_call(FnvCpu& s, uint32_t fn)
{
    HostThread* ht = (HostThread*)s.host;
    NativeCall c = {};
    c.fn = fn;
    c.esp = s.esp + 4;                      // skip the guest return address: the callee's ret pops ours from that slot
    c.eax = s.eax;
    c.ecx = s.ecx;
    c.edx = s.edx;
    uint32_t prev = ht->escape_host_esp;
    c.host_esp_slot = &ht->escape_host_esp;
    // the native callee runs on the guest stack: the OS exception dispatcher only accepts SEH frames inside the
    // thread's recorded stack bounds, so a DLL that throws and catches internally needs them pointing there
    uint32_t base = __readfsdword(4), limit = __readfsdword(8);
    __writefsdword(4, ht->stack_hi);
    __writefsdword(8, ht->stack_lo);
    native_escape(&c);
    __writefsdword(4, base);
    __writefsdword(8, limit);
    ht->escape_host_esp = prev;
    s.eax = c.out_eax;
    s.edx = c.out_edx;
    s.esp = c.out_esp;                      // = guest esp + 4 + bytes the callee popped
    if (c.fpu)
        FPUSH(c.st0);
}

// ---- callbacks: native code calling a guest code address -------------------------------------------------------
struct CbArgs { uint32_t esp_entry, ecx, edx, fn, result, pop; };

void cb_body(void* p)
{
    CbArgs* a = (CbArgs*)p;
    HostThread* ht = t_ht;
    FnvCpu& s = ht->cpu;
    FnvCpu saved = s;
    // A fresh guest frame with the arguments copied in. Starting at the native return address would let guest pushes
    // run over fnv_cb_enter's own frame (it sits just below on the same stack): below that frame, or below the
    // guest's own esp when the native caller is not on the guest stack (an OS thread).
    uint32_t below = ht->escape_host_esp ? (uint32_t)(uintptr_t)a : s.esp;
    uint32_t frame = (below - 0x400 - 64 * 4) & ~15u;
    memcpy((void*)(uintptr_t)(frame + 4), (void*)(uintptr_t)(a->esp_entry + 4), 64 * 4);
    M32(frame) = kGuestReturnSentinel;
    a->esp_entry = frame;
    s.esp = frame;
    s.ecx = a->ecx;
    s.edx = a->edx;
    fnv_dispatch(s, a->fn);
    a->result = s.eax;
    a->pop = s.esp - a->esp_entry - 4;
    uint32_t edx = s.edx;
    s = saved;
    s.edx = edx;
}

extern "C" __declspec(naked) void __cdecl run_on_stack(uint32_t top, void (*fn)(void*), void* arg)
{
    __asm {
        push ebp
        mov ebp, esp
        mov eax, [ebp + 8]
        mov ecx, [ebp + 12]
        mov edx, [ebp + 16]
        mov esp, eax
        push ebp
        push edx
        call ecx
        add esp, 4
        pop ebp
        mov esp, ebp
        pop ebp
        ret
    }
}

extern "C" uint64_t __cdecl fnv_cb_enter(uint32_t esp_entry, uint32_t ecx, uint32_t edx)
{
    if (!t_ht)
    {                                       // a thread the OS or a library created: give it a guest context
        t_ht = new_thread(1u << 20);
        run_tls_callbacks(t_ht, DLL_THREAD_ATTACH);
    }
    HostThread* ht = t_ht;
    CbArgs a = {esp_entry, ecx, edx, ht->pending_cb, 0, 0};
    if (ht->escape_host_esp)                // run the host code below the escaped host frames, not on the guest stack
    {
        uint32_t base = __readfsdword(4), limit = __readfsdword(8);
        __writefsdword(4, ht->host_stack_hi);   // host frames again: host stack bounds while the callback runs
        __writefsdword(8, ht->host_stack_lo);
        run_on_stack((ht->escape_host_esp - 0x200) & ~15u, cb_body, &a);
        __writefsdword(4, base);
        __writefsdword(8, limit);
    }
    else
        cb_body(&a);
    return ((uint64_t)a.pop << 32) | a.result;
}

extern "C" __declspec(naked) void callback_common(void)
{
    __asm {
        mov eax, esp
        push edx
        push ecx
        push eax
        call fnv_cb_enter
        add esp, 12
        pop ecx
        add esp, edx
        jmp ecx
    }
}

thread_local int t_in_veh;
LONG WINAPI veh(EXCEPTION_POINTERS* e)
{
    if (t_in_veh) return EXCEPTION_CONTINUE_SEARCH;     // a fault while reporting one
    struct Guard { Guard() { ++t_in_veh; } ~Guard() { --t_in_veh; } } guard;
    EXCEPTION_RECORD* r = e->ExceptionRecord;
    CONTEXT* c = e->ContextRecord;
    // MSVC SetThreadName: the game raises it for a debugger and catches it with its own __except (guest SEH, not
    // dispatched yet); returning from RaiseException is what that handler amounts to
    if (r->ExceptionCode == 0x406D1388 && t_ht && t_ht->escape_host_esp)
        return EXCEPTION_CONTINUE_EXECUTION;
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->ExceptionInformation[0] == 8 && fnv_lookup(c->Eip))
    {
        if (!t_ht)
        {
            t_ht = new_thread(1u << 20);
            run_tls_callbacks(t_ht, DLL_THREAD_ATTACH);
        }
        t_ht->pending_cb = c->Eip;
        c->Eip = (DWORD)(uintptr_t)&callback_common;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (r->ExceptionCode == 0xE06D7363 && r->NumberParameters >= 3)
    {
        // MSVC throw: [1] = object, [2] = ThrowInfo {attributes, pmfnUnwind, pForwardCompat, pCatchableTypeArray};
        // the array's first CatchableType {properties, pType, ...} names the thrown type (TypeDescriptor name at +8)
        uint32_t ti = (uint32_t)r->ExceptionInformation[2], name = 0;
        if (ti >= g_image_lo && ti < g_image_hi)
        {
            uint32_t cta = M32(ti + 12), ct = M32(cta + 4);
            name = M32(ct + 4) + 8;
        }
        logf("[recomp] C++ exception %s thrown (guest SEH dispatch not implemented yet); params %lu: %08lx %08lx %08lx\n",
             name ? (const char*)(uintptr_t)name : "?", r->NumberParameters, (unsigned long)r->ExceptionInformation[0],
             (unsigned long)r->ExceptionInformation[1], (unsigned long)r->ExceptionInformation[2]);
        if (t_ht)
            for (uint32_t p = t_ht->cpu.esp, shown = 0; p + 4 <= t_ht->stack_hi && shown < 16; p += 4)
            {
                uint32_t v = M32(p);
                if (v >= g_image_lo + 0x1005 && v < 0x00fdf000u && M8(v - 5) == 0xe8)
                {
                    logf("[recomp]   guest stack +%04x: return to %08x\n", p - t_ht->cpu.esp, v);
                    ++shown;
                }
            }
    }
    else if (r->ExceptionCode == 0x406D1388 || r->ExceptionCode == 0x40010006 || r->ExceptionCode == 0x4001000A ||
             r->ExceptionCode == 0x000006BA || r->ExceptionCode == 0x80000003)
        ;                                   // thread naming, debug strings, RPC, breakpoints: routine first-chance events
    else
    {
        FnvCpu* s = t_ht ? &t_ht->cpu : nullptr;
        logf("[recomp] exception %08lx at %08lx (%s %08lx) host esp %08lx\n", r->ExceptionCode, c->Eip,
             r->NumberParameters > 1 ? (r->ExceptionInformation[0] == 1 ? "write" : r->ExceptionInformation[0] == 8 ? "exec" : "read") : "-",
             r->NumberParameters > 1 ? (unsigned long)r->ExceptionInformation[1] : 0ul, c->Esp);
        {
            auto modname = [](uint32_t a, char* out, int n) {
                HMODULE m = nullptr;
                out[0] = 0;
                if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                       (LPCSTR)(uintptr_t)a, &m) && m)
                {
                    char path[MAX_PATH];
                    GetModuleFileNameA(m, path, MAX_PATH);
                    const char* b = strrchr(path, '\\');
                    snprintf(out, n, "%s+%lx", b ? b + 1 : path, (unsigned long)(a - (uint32_t)(uintptr_t)m));
                }
                else
                    snprintf(out, n, "no module");
            };
            char m1[160], m2[160];
            uint32_t ret = IsBadReadPtr((void*)(uintptr_t)c->Esp, 4) ? 0 : M32(c->Esp);
            modname(c->Eip, m1, sizeof m1);
            modname(ret, m2, sizeof m2);
            logf("[recomp]   eip in %s; [esp] = %08x in %s\n", m1, ret, m2);
        }
        if (s)
            logf("[recomp]   guest eax=%08x ecx=%08x edx=%08x ebx=%08x esp=%08x ebp=%08x esi=%08x edi=%08x\n", s->eax,
                 s->ecx, s->edx, s->ebx, s->esp, s->ebp, s->esi, s->edi);
        // guest return addresses on the guest stack: the call chain in original addresses
        dump_all_threads(t_ht);
        if (s && s->esp >= t_ht->stack_lo && s->esp < t_ht->stack_hi - 4)
        {
            int shown = 0;
            for (uint32_t p = s->esp; p + 4 <= t_ht->stack_hi && shown < 24; p += 4)
            {
                uint32_t v = M32(p);
                if (v >= g_image_lo + 0x1005 && v < 0x00fdf000u && M8(v - 5) == 0xe8)
                {
                    logf("[recomp]   guest stack +%04x: return to %08x\n", p - s->esp, v);
                    ++shown;
                }
            }
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// ---- host functions (overrides of imports) ----------------------------------------------------------------------
void h_SHGetFolderPathA(FnvCpu& s)
{
    int csidl = (int)harg(s, 1);
    char* out = (char*)(uintptr_t)harg(s, 4);
    if ((csidl & 0xff) == CSIDL_PERSONAL && g_docs[0])
    {
        strcpy(out, g_docs);
        return hret(s, S_OK, 5);
    }
    return hret(s, SHGetFolderPathA((HWND)(uintptr_t)harg(s, 0), csidl, (HANDLE)(uintptr_t)harg(s, 2), harg(s, 3), out), 5);
}
void h_ShellExecuteA(FnvCpu& s)
{
    logf("[recomp] ShellExecuteA(%s) blocked\n", (const char*)(uintptr_t)harg(s, 2));
    hret(s, 2, 6);
}
void h_MessageBoxA(FnvCpu& s)
{
    logf("[recomp] MessageBoxA: %s | %s\n", (const char*)(uintptr_t)harg(s, 2), (const char*)(uintptr_t)harg(s, 1));
    hret(s, IDOK, 4);
}
void h_OutputDebugStringA(FnvCpu& s)
{
    logf("[dbgstr] %s", (const char*)(uintptr_t)harg(s, 0));
    hret(s, 0, 1);
}
void h_SetUnhandledExceptionFilter(FnvCpu& s) { hret(s, 0, 1); }
// FNV_GPA_NULL=1: GetProcAddress finds nothing (what the web host's stub does), to compare the CRT's fallback paths
void h_GetProcAddress(FnvCpu& s)
{
    if (getenv("FNV_GPA_NULL")) return hret(s, 0, 2);
    hret(s, (uint32_t)(uintptr_t)GetProcAddress((HMODULE)(uintptr_t)harg(s, 0), (LPCSTR)(uintptr_t)harg(s, 1)), 2);
}
void h_GetModuleHandleA(FnvCpu& s)
{
    const char* n = (const char*)(uintptr_t)harg(s, 0);
    if (!n || !_stricmp(n, "FalloutNV.exe"))
        return hret(s, g_image_lo, 1);
    hret(s, (uint32_t)(uintptr_t)GetModuleHandleA(n), 1);
}
void h_GetModuleFileNameA(FnvCpu& s)
{
    HMODULE m = (HMODULE)(uintptr_t)harg(s, 0);
    char* out = (char*)(uintptr_t)harg(s, 1);
    uint32_t n = harg(s, 2);
    if (!m || (uint32_t)(uintptr_t)m == g_image_lo)
    {
        lstrcpynA(out, g_exe_path, (int)n);
        return hret(s, (uint32_t)strlen(out), 3);
    }
    hret(s, GetModuleFileNameA(m, out, n), 3);
}
void log_guest_stack(const char* why)
{
    logf("[recomp] %s\n", why);
    if (t_ht)
        for (uint32_t p = t_ht->cpu.esp, shown = 0; p + 4 <= t_ht->stack_hi && shown < 16; p += 4)
        {
            uint32_t v = M32(p);
            if (v >= g_image_lo + 0x1005 && v < 0x00fdf000u && M8(v - 5) == 0xe8)
            {
                logf("[recomp]   guest stack +%04x: return to %08x\n", p - t_ht->cpu.esp, v);
                ++shown;
            }
        }
}
void h_ExitProcess(FnvCpu& s)
{
    char buf[64];
    snprintf(buf, sizeof buf, "ExitProcess(%u)", harg(s, 0));
    log_guest_stack(buf);
    ExitProcess(harg(s, 0));
}
void h_TerminateProcess(FnvCpu& s)
{
    char buf[96];
    snprintf(buf, sizeof buf, "TerminateProcess(%08x, %u)", harg(s, 0), harg(s, 1));
    log_guest_stack(buf);
    hret(s, TerminateProcess((HANDLE)(uintptr_t)harg(s, 0), harg(s, 1)), 2);
}
struct ThreadStart { uint32_t fn, arg; };
DWORD WINAPI thread_start(void* p)
{
    ThreadStart ts = *(ThreadStart*)p;
    free(p);
    t_ht = new_thread(4u << 20);
    run_tls_callbacks(t_ht, DLL_THREAD_ATTACH);
    uint32_t args[1] = {ts.arg};
    return fnv_guest_call(t_ht->cpu, ts.fn, args, 1);
}
void h_CreateThread(FnvCpu& s)
{
    ThreadStart* ts = (ThreadStart*)malloc(sizeof(ThreadStart));
    ts->fn = harg(s, 2);
    ts->arg = harg(s, 3);
    // 16 MB native stack reservation: the thread runs recompiled code (one host frame per guest call); the exe's
    // default is kept tiny so the initial thread's stack fits below the guest image at 0x00400000
    HANDLE h = CreateThread((LPSECURITY_ATTRIBUTES)(uintptr_t)harg(s, 0), 16u << 20, thread_start, ts,
                            (harg(s, 4) & CREATE_SUSPENDED) | STACK_SIZE_PARAM_IS_A_RESERVATION,
                            (LPDWORD)(uintptr_t)harg(s, 5));
    hret(s, (uint32_t)(uintptr_t)h, 6);
}
// no Galaxy/Steam client callbacks: the wrapper would call the game's callback objects through their vtables
void h_SteamAPI_RegisterCallback(FnvCpu& s) { hret(s, 0, 0); }   // cdecl: the caller pops
void h_SteamAPI_RunCallbacks(FnvCpu& s) { hret(s, 0, 0); }
void h_CreateWindowExA(FnvCpu& s)
{
    uint32_t a[12];
    for (int i = 0; i < 12; ++i) a[i] = harg(s, i);
    DWORD ex = a[0];
    int x = (int)a[4], y = (int)a[5];
    if (g_bg && !a[8])
    {
        x = GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN) + 64;
        y = GetSystemMetrics(SM_YVIRTUALSCREEN);
        ex = (ex | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE) & ~WS_EX_APPWINDOW;
    }
    HWND h = CreateWindowExA(ex, (LPCSTR)(uintptr_t)a[1], (LPCSTR)(uintptr_t)a[2], a[3], x, y, (int)a[6], (int)a[7],
                             (HWND)(uintptr_t)a[8], (HMENU)(uintptr_t)a[9], (HINSTANCE)(uintptr_t)a[10], (LPVOID)(uintptr_t)a[11]);
    logf("[recomp] CreateWindowExA -> %p%s\n", (void*)h, g_bg ? " (off-screen)" : "");
    hret(s, (uint32_t)(uintptr_t)h, 12);
}
void h_ShowWindow(FnvCpu& s)
{
    int cmd = (int)harg(s, 1);
    if (g_bg && cmd != SW_HIDE && cmd != SW_MINIMIZE) cmd = SW_SHOWNOACTIVATE;
    hret(s, ShowWindow((HWND)(uintptr_t)harg(s, 0), cmd), 2);
}
void h_SetForegroundWindow(FnvCpu& s)
{
    hret(s, g_bg ? TRUE : SetForegroundWindow((HWND)(uintptr_t)harg(s, 0)), 1);
}
// The game passes its own image base as the instance; DirectInput checks that it names a loaded module, and the
// mapped exe is not one. Every other argument goes through unchanged.
uint32_t g_real_DirectInput8Create;
void h_DirectInput8Create(FnvCpu& s)
{
    if (harg(s, 0) == g_image_lo)
        M32(s.esp + 4) = (uint32_t)(uintptr_t)GetModuleHandleA(nullptr);
    native_call(s, g_real_DirectInput8Create);
}
void h_VirtualAlloc(FnvCpu& s)
{
    uint32_t addr = harg(s, 0), size = harg(s, 1), type = harg(s, 2), prot = harg(s, 3);
    void* p = VirtualAlloc((void*)(uintptr_t)addr, size, type, prot);
    static int failures;
    if ((!p && failures++ < 6) || size >= (64u << 20) || (p && addr && (type & 0x2000)))
    {
        DWORD err = GetLastError();
        MEMORY_BASIC_INFORMATION mi = {};
        VirtualQuery((void*)(uintptr_t)addr, &mi, sizeof mi);
        logf("[recomp] VirtualAlloc(%08x, %u MB, %x, %x) -> %p%s (error %lu; region %p+%lx state %lx)\n", addr,
             size >> 20, type, prot, p, p ? "" : " FAILED", err, mi.BaseAddress, (unsigned long)mi.RegionSize,
             mi.State);
        if (failures == 1)                   // the address space at the first failure: allocations of 4 MB or more
            for (uint64_t a = 0x10000; a < 0x100000000ull;)
            {
                MEMORY_BASIC_INFORMATION q = {};
                if (!VirtualQuery((void*)(uintptr_t)a, &q, sizeof q)) break;
                if (q.State != MEM_FREE && q.RegionSize >= (4u << 20))
                    logf("[recomp]   map %08lx +%7lu KB %s alloc base %p\n", (unsigned long)a,
                         (unsigned long)(q.RegionSize >> 10), q.State == MEM_COMMIT ? "commit " : "reserve",
                         q.AllocationBase);
                a = (uint64_t)(uintptr_t)q.BaseAddress + q.RegionSize;
            }
    }
    hret(s, (uint32_t)(uintptr_t)p, 4);
}
void h_SetWindowPos(FnvCpu& s)
{
    UINT flags = harg(s, 6);
    if (g_bg) flags |= SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE;
    hret(s, SetWindowPos((HWND)(uintptr_t)harg(s, 0), (HWND)(uintptr_t)harg(s, 1), (int)harg(s, 2), (int)harg(s, 3),
                         (int)harg(s, 4), (int)harg(s, 5), flags), 7);
}

struct Override { const char* name; HostFn fn; };
const Override kOverrides[] = {
    {"SHGetFolderPathA", h_SHGetFolderPathA}, {"ShellExecuteA", h_ShellExecuteA}, {"MessageBoxA", h_MessageBoxA},
    {"OutputDebugStringA", h_OutputDebugStringA}, {"SetUnhandledExceptionFilter", h_SetUnhandledExceptionFilter},
    {"GetProcAddress", h_GetProcAddress},
    {"GetModuleHandleA", h_GetModuleHandleA}, {"GetModuleFileNameA", h_GetModuleFileNameA},
    {"ExitProcess", h_ExitProcess}, {"TerminateProcess", h_TerminateProcess}, {"CreateThread", h_CreateThread},
    {"SteamAPI_RegisterCallback", h_SteamAPI_RegisterCallback}, {"SteamAPI_UnregisterCallback", h_SteamAPI_RegisterCallback},
    {"SteamAPI_RunCallbacks", h_SteamAPI_RunCallbacks},
    {"CreateWindowExA", h_CreateWindowExA}, {"ShowWindow", h_ShowWindow}, {"SetForegroundWindow", h_SetForegroundWindow},
    {"SetWindowPos", h_SetWindowPos}, {"VirtualAlloc", h_VirtualAlloc}, {"DirectInput8Create", h_DirectInput8Create},
};

// ---- mapping the exe ----------------------------------------------------------------------------------------------
bool map_exe(const char* path)
{
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) { logf("[recomp] cannot open %s\n", path); return false; }
    DWORD size = GetFileSize(f, nullptr), got = 0;
    unsigned char* file = (unsigned char*)malloc(size);
    ReadFile(f, file, size, &got, nullptr);
    CloseHandle(f);
    auto nt = (IMAGE_NT_HEADERS32*)(file + ((IMAGE_DOS_HEADER*)file)->e_lfanew);
    uint32_t base = nt->OptionalHeader.ImageBase, img = nt->OptionalHeader.SizeOfImage;
    void* p = VirtualAlloc((void*)(uintptr_t)base, img, MEM_COMMIT, PAGE_READWRITE);
    if (!p) p = VirtualAlloc((void*)(uintptr_t)base, img, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (p != (void*)(uintptr_t)base) { logf("[recomp] cannot map the exe at %08x (%lu)\n", base, GetLastError()); return false; }
    memcpy(p, file, nt->OptionalHeader.SizeOfHeaders);
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i)
        memcpy((char*)p + sec[i].VirtualAddress, file + sec[i].PointerToRawData,
               min(sec[i].SizeOfRawData, sec[i].Misc.VirtualSize ? sec[i].Misc.VirtualSize : sec[i].SizeOfRawData));
    g_image_lo = base;
    g_image_hi = base + img;
    g_entry = base + nt->OptionalHeader.AddressOfEntryPoint;
    auto& td = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
    if (td.VirtualAddress)
    {
        g_tls = *(IMAGE_TLS_DIRECTORY32*)(uintptr_t)(base + td.VirtualAddress);
        g_has_tls = true;
        M32(g_tls.AddressOfIndex) = 0;
    }
    // imports: the real functions, or a host override
    auto& id = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    int n = 0, over = 0, missing = 0, pal = 0;
    for (auto d = (IMAGE_IMPORT_DESCRIPTOR*)(uintptr_t)(base + id.VirtualAddress); d->Name; ++d)
    {
        const char* dll = (const char*)(uintptr_t)(base + d->Name);
        HMODULE m = LoadLibraryA(dll);
        auto names = (IMAGE_THUNK_DATA32*)(uintptr_t)(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        auto slots = (IMAGE_THUNK_DATA32*)(uintptr_t)(base + d->FirstThunk);
        for (int k = 0; names[k].u1.AddressOfData; ++k, ++n)
        {
            const char* nm = (names[k].u1.Ordinal & IMAGE_ORDINAL_FLAG32) ? (const char*)(uintptr_t)(names[k].u1.Ordinal & 0xffff)
                                                                          : (const char*)(uintptr_t)(base + names[k].u1.AddressOfData + 2);
            uint32_t v = 0;
            const char* excl = getenv("FNV_PAL_EXCLUDE");   // comma-separated names kept native (bisecting)
            bool excluded = false;
            if (excl && !(names[k].u1.Ordinal & IMAGE_ORDINAL_FLAG32))
            {
                const char* hit = strstr(excl, nm);
                size_t len = strlen(nm);
                excluded = hit && (hit == excl || hit[-1] == ',') && (hit[len] == 0 || hit[len] == ',');
            }
            if (g_pal && !excluded && !(names[k].u1.Ordinal & IMAGE_ORDINAL_FLAG32))
                for (const PalEntry* e = pal_kernel_entries(); e->name && !v; ++e)
                    if (!strcmp(e->name, nm))
                    {
                        v = fnv_host_register(e->name, e->fn);
                        ++pal;
                    }
            if (!v && !(names[k].u1.Ordinal & IMAGE_ORDINAL_FLAG32) && getenv("FNV_INPUT_SCRIPT") && !strcmp(nm, "DirectInput8Create"))
                for (const PalEntry* e = pal_input_entries(); e->name && !v; ++e)   // scripted input: the PAL devices
                    if (!strcmp(e->name, nm)) v = fnv_host_register(e->name, e->fn);
            if (!v && !(names[k].u1.Ordinal & IMAGE_ORDINAL_FLAG32))
                for (const Override& o : kOverrides)
                    if (!strcmp(o.name, nm))
                    {
                        if (!strcmp(nm, "DirectInput8Create") && m)
                            g_real_DirectInput8Create = (uint32_t)(uintptr_t)GetProcAddress(m, nm);
                        v = fnv_host_register(o.name, o.fn);
                        ++over;
                    }
            if (!v && m)
                v = (uint32_t)(uintptr_t)GetProcAddress(m, nm);
            if (!v)
            {
                ++missing;
                logf("[recomp] import missing: %s!%s\n", dll, (names[k].u1.Ordinal & IMAGE_ORDINAL_FLAG32) ? "#ordinal" : nm);
            }
            slots[k].u1.Function = v;
            if (v && !(names[k].u1.Ordinal & IMAGE_ORDINAL_FLAG32) && g_nimport < 512)
            {
                g_import_addr[g_nimport] = v;
                g_import_name[g_nimport++] = nm;
            }
        }
    }
    logf("[recomp] exe mapped at %08x-%08x, entry %08x, %d imports (%d PAL, %d host overrides, %d missing), TLS %s\n",
         g_image_lo, g_image_hi, g_entry, n, pal, over, missing, g_has_tls ? "yes" : "no");
    free(file);
    return true;
}

// The guest's address range must be free before anything else lands in it: run ourselves suspended with it reserved.
int relaunch(int argc, char** argv)
{
    char self[MAX_PATH];
    GetModuleFileNameA(nullptr, self, MAX_PATH);
    STARTUPINFOA si = {sizeof si};
    PROCESS_INFORMATION pi = {};
    SetEnvironmentVariableA("FNV_RECOMP_CHILD", "1");
    if (!CreateProcessA(self, GetCommandLineA(), nullptr, nullptr, TRUE, CREATE_SUSPENDED, nullptr, nullptr, &si, &pi))
    {
        fprintf(stderr, "[recomp] relaunch failed (%lu)\n", GetLastError());
        return 1;
    }
    void* r = VirtualAllocEx(pi.hProcess, (void*)0x00400000, 0x0100D000, MEM_RESERVE, PAGE_READWRITE);
    if (!r) fprintf(stderr, "[recomp] could not reserve the guest range in the child (%lu)\n", GetLastError());
    ResumeThread(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    fprintf(stderr, "[recomp] game process exited with %08lx\n", code);
    return (int)code;
}
} // namespace

uint32_t fnv_host_base() { return g_host_base; }

// ---- PAL hooks ----------------------------------------------------------------------------------------------------
uint32_t fnv_host_thread_main(uint32_t fn, uint32_t arg)
{
    t_ht = new_thread(4u << 20);
    run_tls_callbacks(t_ht, DLL_THREAD_ATTACH);
    uint32_t args[1] = {arg};
    return fnv_guest_call(t_ht->cpu, fn, args, 1);
}
uintptr_t fnv_host_spawn(void (*fn)(void*), void* arg)
{
    struct P { void (*fn)(void*); void* arg; };
    HANDLE h = CreateThread(nullptr, 16u << 20, [](void* p) -> DWORD {
        P q = *(P*)p;
        delete (P*)p;
        q.fn(q.arg);
        return 0;
    }, new P{fn, arg}, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    return (uintptr_t)h;                    // kept open: the PAL sets priorities through it
}
void fnv_host_set_priority(uintptr_t token, int priority)
{
    SetThreadPriority(token ? (HANDLE)token : GetCurrentThread(), priority);
}
int64_t fnv_host_wait_foreign(uint32_t h, uint32_t ms) { return WaitForSingleObject((HANDLE)(uintptr_t)h, ms); }
bool fnv_host_close_foreign(uint32_t h) { return CloseHandle((HANDLE)(uintptr_t)h) != 0; }
uint32_t fnv_host_image_base() { return g_image_lo; }
const char* fnv_host_exe_path() { return g_exe_path; }
void fnv_host_log(const char* text) { logf("%s", text); }

void fnv_host_unknown(FnvCpu& s, uint32_t addr)
{
    if (addr == kGuestReturnSentinel)
    {
        logf("[recomp] dispatch to the return sentinel (a guest function jumped to its return address)\n");
        fnv_host_fatal(s, addr, "sentinel");
    }
    if (addr >= g_image_lo && addr < g_image_hi)
        fnv_rtrap(s, addr, "no recompiled function at this guest code address");
    if (!g_trace_imports)
        return native_call(s, addr);
    uint32_t ret = M32(s.esp), a0 = M32(s.esp + 4), a1 = M32(s.esp + 8), a2 = M32(s.esp + 12);
    native_call(s, addr);
    const char* n = import_name(addr);
    char modbuf[96];
    if (!n)
    {                                       // a COM method or a GetProcAddress result: name its module
        HMODULE m = nullptr;
        char path[MAX_PATH] = "";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)(uintptr_t)addr, &m) && m)
            GetModuleFileNameA(m, path, MAX_PATH);
        const char* b = strrchr(path, '\\');
        snprintf(modbuf, sizeof modbuf, "native:%s+%lx", b ? b + 1 : "?", (unsigned long)(addr - (uint32_t)(uintptr_t)m));
        n = modbuf;
    }
    char buf[200];
    snprintf(buf, sizeof buf, "[import] %s(%08x, %08x, %08x) -> %08x (from %08x, last error %lu)\n", n ? n : "native",
             a0, a1, a2, s.eax, ret, GetLastError());
    fnv_host_log(buf);
}

[[noreturn]] void fnv_host_fatal(FnvCpu& s, uint32_t addr, const char* what)
{
    logf("[recomp] stopping: %s at %08x (guest esp %08x)\n", what, addr, s.esp);
    if (t_ht)
        for (uint32_t p = s.esp, shown = 0; p >= t_ht->stack_lo && p < t_ht->stack_hi && shown < 24; p += 4)
        {
            uint32_t v = M32(p);
            if (v >= g_image_lo + 0x1000 && v < 0x00fdf000u && M8(v - 5) == 0xe8)
            {
                logf("[recomp]   guest stack +%04x: return to %08x\n", p - s.esp, v);
                ++shown;
            }
        }
    ExitProcess(3);
}

int main(int argc, char** argv)
{
    if (!getenv("FNV_RECOMP_CHILD"))
        return relaunch(argc, argv);
    const char* exe = nullptr;
    for (int i = 1; i + 1 < argc; ++i)
        if (!strcmp(argv[i], "--exe")) exe = argv[i + 1];
    char logpath[MAX_PATH], dir[MAX_PATH];
    GetModuleFileNameA(nullptr, dir, MAX_PATH);
    *strrchr(dir, '\\') = 0;
    snprintf(logpath, sizeof logpath, "%s\\run.log", dir);
    g_log = fopen(logpath, "w");
    snprintf(g_docs, sizeof g_docs, "%s\\..\\fnv_docs", dir);    // out/fnv_docs, the private INI copy
    if (getenv("FNV_REAL_DOCS")) g_docs[0] = 0;
    g_bg = getenv("FNV_BACKGROUND") != nullptr;
    g_trace_imports = getenv("FNV_TRACE_IMPORTS") != nullptr;
    g_pal = getenv("FNV_PAL") != nullptr;
    if (const char* sc = getenv("FNV_INPUT_SCRIPT")) pal_input_script(sc);
    if (!exe) { logf("usage: fnv_recomp.exe --exe <FalloutNV.exe>\n"); return 2; }
    GetFullPathNameA(exe, MAX_PATH, g_exe_path, nullptr);
    char gamedir[MAX_PATH];
    strcpy(gamedir, g_exe_path);
    *strrchr(gamedir, '\\') = 0;
    SetCurrentDirectoryA(gamedir);
    SetDllDirectoryA(gamedir);
    g_host_base = (uint32_t)(uintptr_t)VirtualAlloc(nullptr, kHostFnMax * 16, MEM_RESERVE, PAGE_NOACCESS);
    logf("[recomp] %u recompiled functions, host functions at %08x, docs %s%s\n", fnv_function_count(), g_host_base,
         g_docs[0] ? g_docs : "(real)", g_bg ? ", background" : "");
    AddVectoredExceptionHandler(1, veh);
    atexit([] { logf("[recomp] process exiting\n"); });
    if (!map_exe(g_exe_path)) return 1;
    // The game's main thread gets its own 64 MB native stack (recompiled calls nest one host frame per guest call);
    // the exe's default stays small, because every thread the game and its libraries create gets the default
    // reservation, and 64 MB each filled the address space the game's heaps probe for.
    HANDLE th = CreateThread(nullptr, 64u << 20, [](void*) -> DWORD {
        t_ht = new_thread(16u << 20);
        run_tls_callbacks(t_ht, DLL_PROCESS_ATTACH);
        logf("[recomp] calling the original entry point %08x\n", g_entry);
        uint32_t r = fnv_guest_call(t_ht->cpu, g_entry, nullptr, 0);
        logf("[recomp] entry point returned %u\n", r);
        return r;
    }, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    WaitForSingleObject(th, INFINITE);
    DWORD code = 0;
    GetExitCodeThread(th, &code);
    return (int)code;
}
