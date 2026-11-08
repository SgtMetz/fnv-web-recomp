// FalloutNV_relinked loader (Phase 1, native).
//
// Layout: this exe is linked at the original image base 0x00400000 (fixed). Its .fnvimg section reserves the
// original data range, so nothing else (heaps, stacks) can take those addresses. At startup the loader:
//   1. reads the player's own FalloutNV.exe and copies .rdata/.data/.tls/CONST to their original VAs (.bss = 0);
//   2. loads fnv_game.dll (the relinked code, fixed base) and its fnv_addr_table {original address -> function};
//   3. walks the exe's .reloc table: every 32-bit word in the data range that points into the original .text is
//      repointed at the relinked function, or at a logging thunk when nothing was relinked there;
//   4. fills the original IAT slots in the data image with the real imports (Phase 2 swaps in the PAL here);
//   5. runs the original C++ initializer table (__xc_a..__xc_z) in its original order;
//   6. calls the relinked WinMain.
// Nothing from the exe is written to disk.
#include <windows.h>
#include <d3d9.h>
#include <shlobj.h>
#include <intrin.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <vector>
#include <algorithm>

namespace
{
// Facts about FalloutNV.exe 1.4.0.525 (checked against the exe's own headers at runtime where possible).
const DWORD kImageBase = 0x00400000;
const DWORD kXcA = 0x00FDF4A4;       // __xc_a: from _cinit (mov eax, __xc_a / push __xc_z / call _initterm)
const DWORD kXcZ = 0x010113C0;       // __xc_z
const DWORD kWinMain = 0x0086A850;   // called by __tmainCRTStartup after push 0x00400000
const DWORD kDataStart = 0x00FDF000;
const DWORD kDataEnd = 0x01274000;
}

#pragma section(".fnvimg", read, write)
__declspec(allocate(".fnvimg")) static unsigned char g_reserved[0x01000000];

namespace
{
struct AddrEntry
{
    DWORD original;
    void* relinked;
};

struct Pe
{
    std::vector<unsigned char> file;
    IMAGE_NT_HEADERS32* nt = nullptr;
    IMAGE_SECTION_HEADER* sec = nullptr;
    int nsec = 0;

    bool load(const char* path)
    {
        HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE)
            return false;
        DWORD size = GetFileSize(h, nullptr), got = 0;
        file.resize(size);
        BOOL ok = ReadFile(h, file.data(), size, &got, nullptr);
        CloseHandle(h);
        if (!ok || got != size)
            return false;
        auto dos = (IMAGE_DOS_HEADER*)file.data();
        nt = (IMAGE_NT_HEADERS32*)(file.data() + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.ImageBase != kImageBase)
            return false;
        sec = IMAGE_FIRST_SECTION(nt);
        nsec = nt->FileHeader.NumberOfSections;
        return true;
    }
    const IMAGE_SECTION_HEADER* section(const char* name) const
    {
        for (int i = 0; i < nsec; ++i)
            if (!strncmp((const char*)sec[i].Name, name, 8))
                return &sec[i];
        return nullptr;
    }
    // file bytes for an RVA (used only for headers/tables the data image does not cover)
    const unsigned char* at(DWORD rva) const
    {
        for (int i = 0; i < nsec; ++i)
            if (rva >= sec[i].VirtualAddress && rva < sec[i].VirtualAddress + sec[i].SizeOfRawData)
                return file.data() + sec[i].PointerToRawData + (rva - sec[i].VirtualAddress);
        return nullptr;
    }
};

std::vector<AddrEntry> g_table;           // sorted by original address
std::vector<AddrEntry> g_byPtr;           // sorted by relinked pointer (crash reports)
unsigned char* g_thunks = nullptr;
size_t g_thunkUsed = 0;
DWORD g_textStart, g_textEnd;
FILE* g_log = stderr;

void* relinked(DWORD original)
{
    auto it = std::lower_bound(g_table.begin(), g_table.end(), original,
                               [](const AddrEntry& e, DWORD a) { return e.original < a; });
    return (it != g_table.end() && it->original == original) ? it->relinked : nullptr;
}

const DWORD kTrapException = 0xE0F17E01u;   // raised by fnv_trap (DLL) and loader_unbound (address, name)

extern "C" void __cdecl loader_unbound(DWORD original)
{
    fprintf(g_log, "[loader] call into unbound original code at 0x%08lx\n", original);
    fflush(g_log);
    ULONG_PTR args[2] = {original, 0};
    RaiseException(kTrapException, EXCEPTION_NONCONTINUABLE, 2, args);
}

struct InitResult
{
    DWORD code;          // 0 = ran; kTrapException = hit unbound code; else the exception code
    DWORD detail;        // trap: original address; access violation: faulting data address
    void* where;         // exception address
    DWORD frames[6];     // return addresses from the EBP chain at the fault
};

// Guarded call without compiler SEH (clang-cl crashed on __try here): call_guarded saves the callee-saved
// registers, ESP and the fs:[0] SEH chain head; while a guarded call is active, a vectored handler turns a trap or
// hardware fault into a jump to guard_resume, which restores ESP and fs:[0] (dropping any SEH frames the abandoned
// code registered) and returns 1. C++ exceptions (0xE06D7363) are left to their normal handlers.
InitResult g_guard;
char g_guardDump[1024];
volatile LONG g_guardActive = 0;
volatile DWORD g_guardThread = 0;
DWORD g_guardEsp = 0, g_guardSeh = 0;

extern "C" __declspec(naked) int __cdecl call_guarded(void(__cdecl* f)(void))
{
    __asm {
        push ebx
        push esi
        push edi
        push ebp
        mov eax, fs:[0]
        mov g_guardSeh, eax
        mov g_guardEsp, esp
        mov eax, [esp + 20]
        call eax
        xor eax, eax
        pop ebp
        pop edi
        pop esi
        pop ebx
        ret
    }
}

extern "C" __declspec(naked) void guard_resume()
{
    __asm {
        mov esp, g_guardEsp
        mov eax, g_guardSeh
        mov fs:[0], eax
        mov eax, 1
        pop ebp
        pop edi
        pop esi
        pop ebx
        ret
    }
}

LONG CALLBACK guard_handler(EXCEPTION_POINTERS* ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    bool fault = code == kTrapException || code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION ||
                 code == EXCEPTION_PRIV_INSTRUCTION || code == EXCEPTION_INT_DIVIDE_BY_ZERO ||
                 code == EXCEPTION_STACK_OVERFLOW || code == EXCEPTION_ARRAY_BOUNDS_EXCEEDED;
    if (!g_guardActive || !fault || GetCurrentThreadId() != g_guardThread)
        return EXCEPTION_CONTINUE_SEARCH;
    g_guardActive = 0;   // IsBadReadPtr below faults internally; do not re-enter
    g_guard.code = code;
    g_guard.where = ep->ExceptionRecord->ExceptionAddress;
    g_guard.detail = ep->ExceptionRecord->NumberParameters >= 2
                         ? (DWORD)ep->ExceptionRecord->ExceptionInformation[code == kTrapException ? 0 : 1] : 0;
    DWORD* fp = (DWORD*)ep->ContextRecord->Ebp;
    for (int i = 0; i < 6; ++i)
    {
        g_guard.frames[i] = 0;
        if (!fp || IsBadReadPtr(fp, 8))
            break;
        g_guard.frames[i] = fp[1];
        fp = (DWORD*)fp[0];
    }
    {
        CONTEXT* c = ep->ContextRecord;
        DWORD* sp = (DWORD*)c->Esp;
        int o = sprintf(g_guardDump, "eax=%08lx ecx=%08lx edx=%08lx ebx=%08lx esi=%08lx edi=%08lx ebp=%08lx esp=%08lx stack:", c->Eax, c->Ecx, c->Edx, c->Ebx, c->Esi, c->Edi, c->Ebp, c->Esp);
        for (int i = 0; i < 16 && !IsBadReadPtr(sp + i, 4); ++i) o += sprintf(g_guardDump + o, " %08lx", sp[i]);
    }
    {   // stack scan for return addresses into the game DLL (frame-pointer chains are unreliable in lifted code)
        DWORD* sp = (DWORD*)ep->ContextRecord->Esp;
        int n = 0;
        for (int i = 0; i < 24 && n < 6; ++i)
        {
            if (IsBadReadPtr(sp + i, 4)) break;
            DWORD w = sp[i];
            if (w >= 0x20001000 && w < 0x22000000) g_guard.frames[n++] = w;
        }
        for (; n < 6; ++n) g_guard.frames[n] = 0;
    }
    g_guardActive = 0;
    ep->ContextRecord->Eip = (DWORD)&guard_resume;
    return EXCEPTION_CONTINUE_EXECUTION;
}

// One original initializer, guarded, so a single run reports every blocker.
InitResult run_initializer(void(__cdecl* f)(void))
{
    g_guard = InitResult{0, 0, nullptr, {0}};
    g_guardThread = GetCurrentThreadId();
    g_guardActive = 1;
    call_guarded(f);
    g_guardActive = 0;
    return g_guard;
}

// push imm32 ; call rel32 loader_unbound  (never returns)
void* thunk_for(DWORD original)
{
    unsigned char* t = g_thunks + g_thunkUsed;
    g_thunkUsed += 16;
    t[0] = 0x68;
    memcpy(t + 1, &original, 4);
    t[5] = 0xE8;
    LONG rel = (LONG)((unsigned char*)&loader_unbound - (t + 10));
    memcpy(t + 6, &rel, 4);
    memset(t + 10, 0xCC, 6);
    return t;
}

void describe(void* ip)
{
    {
        HMODULE m = nullptr;
        char path[MAX_PATH] = {};
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)ip, &m) && m &&
            GetModuleFileNameA(m, path, sizeof(path)))
        {
            const char* b = strrchr(path, 0x5c);
            fprintf(g_log, "[loader]   module %s+0x%lx\n", b ? b + 1 : path, (DWORD)((char*)ip - (char*)m));
        }
    }
    auto it = std::upper_bound(g_byPtr.begin(), g_byPtr.end(), ip,
                               [](void* p, const AddrEntry& e) { return p < e.relinked; });
    if (it != g_byPtr.begin())
    {
        --it;
        fprintf(g_log, "[loader]   in relinked fnv_f_%08lx + 0x%lx\n", it->original,
                (DWORD)((unsigned char*)ip - (unsigned char*)it->relinked));
    }
}

HANDLE g_watchdogStart = nullptr;

// OpenAI model: opt-in snapshot of the WinMain thread, taken without logging while it is suspended.
DWORD WINAPI startup_watchdog(void* arg)
{
    HANDLE thread = (HANDLE)arg;
    HANDLE thread0 = thread;
    WaitForSingleObject(g_watchdogStart, INFINITE);
    const char* ms = getenv("FNV_WATCHDOG_MS");
    Sleep(ms ? (DWORD)atoi(ms) : 2000);
    CONTEXT ctx = {};
    ctx.ContextFlags = CONTEXT_FULL;
    DWORD words[256] = {};
    SIZE_T bytes = 0;
    bool captured = false;
    DWORD lo = g_byPtr.empty() ? 0 : (DWORD)g_byPtr.front().relinked;
    DWORD hi = g_byPtr.empty() ? 0 : (DWORD)g_byPtr.back().relinked + 0x10000;
    if (SuspendThread(thread) != (DWORD)-1)
    {
        if (GetThreadContext(thread, &ctx))
        {
            captured = true;
            ReadProcessMemory(GetCurrentProcess(), (void*)ctx.Esp, words, sizeof(words), &bytes);
        }
        ResumeThread(thread);
    }
    if (!captured)
        fprintf(g_log, "[watchdog] could not capture WinMain thread\n");
    else
    {
        fprintf(g_log, "[watchdog] eip=%08lx esp=%08lx ebp=%08lx eax=%08lx ecx=%08lx edx=%08lx\n",
                ctx.Eip, ctx.Esp, ctx.Ebp, ctx.Eax, ctx.Ecx, ctx.Edx);
        if (ctx.Eip >= lo && ctx.Eip < hi) describe((void*)ctx.Eip);
        for (size_t i = 0, shown = 0; i < bytes / sizeof(DWORD) && shown < 24; ++i)
            if (words[i] >= lo && words[i] < hi)
            {
                fprintf(g_log, "[watchdog] stack+%03x %08lx", (unsigned)(i * 4), words[i]);
                describe((void*)words[i]);
                ++shown;
            }
    }
    // every other thread of the process, plus module-resolved return-address candidates on the main stack
    auto modname = [](DWORD addr, char* out, size_t n) {
        HMODULE m = nullptr;
        out[0] = 0;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)addr, &m) && m)
        {
            char path[MAX_PATH] = {};
            GetModuleFileNameA(m, path, sizeof(path));
            const char* base = strrchr(path, '\\');
            snprintf(out, n, "%s+0x%lx", base ? base + 1 : path, addr - (DWORD)m);
        }
    };
    if (captured)
    {
        char buf[300];
        {
            CRITICAL_SECTION* ll = *(CRITICAL_SECTION**)((char*)__readfsdword(0x30) + 0xA0);
            fprintf(g_log, "[watchdog] loader lock owner=%lu lockcount=%ld recursion=%ld (main tid=%lu)\n", (unsigned long)(ULONG_PTR)ll->OwningThread,
                    ll->LockCount, ll->RecursionCount, GetThreadId(thread0));
        }
        modname(ctx.Eip, buf, sizeof(buf));
        fprintf(g_log, "[watchdog] main eip in %s\n", buf);
        for (size_t i = 0, shown = 0; i < bytes / sizeof(DWORD) && shown < 40; ++i)
        {
            modname(words[i], buf, sizeof(buf));
            if (buf[0]) { fprintf(g_log, "[watchdog] main stack+%03x %08lx %s\n", (unsigned)(i * 4), words[i], buf); ++shown; }
        }
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        THREADENTRY32 te = {sizeof(te)};
        if (snap != INVALID_HANDLE_VALUE && Thread32First(snap, &te))
            do {
                if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == GetCurrentThreadId()) continue;
                HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID);
                if (!t) continue;
                CONTEXT c = {};
                c.ContextFlags = CONTEXT_FULL;
                if (SuspendThread(t) != (DWORD)-1)
                {
                    bool ok = GetThreadContext(t, &c);
                    DWORD w[256] = {};
                    SIZE_T got = 0;
                    if (ok) ReadProcessMemory(GetCurrentProcess(), (void*)c.Esp, w, sizeof(w), &got);
                    ResumeThread(t);
                    if (ok)
                    {
                        modname(c.Eip, buf, sizeof(buf));
                        fprintf(g_log, "[watchdog] thread %lu eip=%08lx %s\n", te.th32ThreadID, c.Eip, buf);
                        for (size_t i = 0, shown = 0; i < got / sizeof(DWORD) && shown < 12; ++i)
                        {
                            char m2[300];
                            modname(w[i], m2, sizeof(m2));
                            if (m2[0] && !strstr(m2, "FalloutNV_relinked"))
                                fprintf(g_log, "[watchdog]   t%lu stack+%03x %08lx %s\n", te.th32ThreadID, (unsigned)(i * 4), w[i], m2);

                            DWORD lo2 = g_byPtr.empty() ? 0 : (DWORD)g_byPtr.front().relinked, hi2 = g_byPtr.empty() ? 0 : (DWORD)g_byPtr.back().relinked + 0x10000;
                            if (w[i] >= lo2 && w[i] < hi2) { fprintf(g_log, "[watchdog]   t%lu stack+%03x %08lx", te.th32ThreadID, (unsigned)(i * 4), w[i]); describe((void*)w[i]); ++shown; }
                        }
                    }
                }
                CloseHandle(t);
            } while (Thread32Next(snap, &te));
        if (snap != INVALID_HANDLE_VALUE) CloseHandle(snap);
    }
    fflush(g_log);
    return 0;
}

// Debug aid (FNV_TRACE_AT=<original hex address>): break at a relinked function, then single-step, keeping the last
// kTraceRing EIPs. on_crash prints them (resolved to relinked functions), which shows what ran before a wild jump.
const int kTraceRing = 262144;
DWORD g_traceRing[kTraceRing];
DWORD g_traceRegs[kTraceRing][5];
volatile LONG g_traceIdx = 0;
volatile LONG g_tracing = 0;
unsigned char* g_traceAddr = nullptr;
unsigned char g_traceOrig = 0;
DWORD g_traceThread = 0;
struct EntryRec { DWORD orig, ecx, esp, ret; };
const int kEntryRing = 1024;
EntryRec g_entries[kEntryRing];
volatile LONG g_entryIdx = 0;
DWORD g_watchAddr = 0, g_watchPrev = 0;
struct WatchHit { LONG step; DWORD eip, from, to; };
WatchHit g_watchHits[64];
int g_watchCount = 0;

// FNV_BP=<hex original address>[,...]: log registers and arguments at each entry of the relinked function, keep running.
struct Bp { unsigned char* addr; unsigned char orig; DWORD original; int hits; };
Bp g_bps[32];
int g_bpCount = 0;
Bp* g_bpStepping[64];   // per thread id modulo 64: the breakpoint to re-arm after the single step

__declspec(thread) DWORD g_retStk[64];
__declspec(thread) DWORD g_retId[64];
__declspec(thread) int g_retN = 0;
extern "C" DWORD __cdecl bp_ret_log(DWORD eax, DWORD edx)
{
    int n = --g_retN;
    if (n < 0) { g_retN = 0; return 0; }
    fprintf(g_log, "[bp] %08lx returned eax=%08lx edx=%08lx\n", g_retId[n], eax, edx);
    fflush(g_log);
    return g_retStk[n];
}
__declspec(naked) void bp_ret_tramp()
{
    __asm {
        push eax
        push edx
        push edx
        push eax
        call bp_ret_log
        add esp, 8
        mov ecx, eax
        pop edx
        pop eax
        push ecx
        ret
    }
}

char g_bpRing[64][200];
volatile LONG g_bpRingIdx = 0;
LONG CALLBACK bp_handler(EXCEPTION_POINTERS* ep)
{
    auto r = ep->ExceptionRecord;
    if (r->ExceptionCode == EXCEPTION_BREAKPOINT)
    {
        for (int i = 0; i < g_bpCount; ++i)
            if (g_bps[i].addr == r->ExceptionAddress)
            {
                Bp& b = g_bps[i];
                CONTEXT* c = ep->ContextRecord;
                {
                    DWORD* sp3 = (DWORD*)c->Esp;
                    LONG k3 = InterlockedIncrement(&g_bpRingIdx) - 1;
                    snprintf(g_bpRing[k3 & 63], sizeof(g_bpRing[0]), "%08lx hit %d ecx=%08lx edx=%08lx ret=%08lx args %08lx %08lx %08lx %08lx", b.original, b.hits + 1,
                             c->Ecx, c->Edx, sp3[0], sp3[1], sp3[2], sp3[3], sp3[4]);
                }
                // FNV_BP_FROM=N: log hits N..N+39 (default 1): a crash at hit 199 is past the first 40
                static const LONG bpFrom = getenv("FNV_BP_FROM") ? atol(getenv("FNV_BP_FROM")) : 1;
                ++b.hits;
                if (b.hits >= bpFrom && b.hits < bpFrom + 40)
                {
                    DWORD* sp = (DWORD*)c->Esp;
                    // FNV_BP_LIST=headoff,nextoff: walk the singly linked list whose head pointer is at
                    // [ecx+headoff], following [node+nextoff], printing each node until an unreadable one
                    if (const char* lw = getenv("FNV_BP_LIST"))
                    {
                        char* e2;
                        DWORD ho = strtoul(lw, &e2, 16), no = *e2 == 0x2c ? strtoul(e2 + 1, nullptr, 16) : 0;
                        DWORD node = IsBadReadPtr((void*)(c->Ecx + ho), 4) ? 0 : *(DWORD*)(c->Ecx + ho);
                        for (int k = 0; node && k < 400; ++k)
                        {
                            if (IsBadReadPtr((void*)node, 16)) { fprintf(g_log, "[bp]     list #%d %08lx unreadable\n", k, node); break; }
                            DWORD* v = (DWORD*)node;
                            fprintf(g_log, "[bp]     list #%d %08lx: %08lx %08lx %08lx %08lx\n", k, node, v[0], v[1], v[2], v[3]);
                            node = *(DWORD*)(node + no);
                        }
                    }
                    fprintf(g_log, "[bp] %08lx hit %d thread %lu eax=%08lx ecx=%08lx edx=%08lx esp=%08lx ret=%08lx args %08lx %08lx %08lx %08lx\n", b.original, b.hits,
                            GetCurrentThreadId(), c->Eax, c->Ecx, c->Edx, c->Esp, sp[0], sp[1], sp[2], sp[3], sp[4]);
                    // x87 state: TOP (status bits 11-13) and the tag word (2 bits per register, 3 = empty; FFFF = clean)
                    fprintf(g_log, "[bp]     fpu status %04lx (top %lu) tags %04lx\n", c->FloatSave.StatusWord & 0xffff,
                            (c->FloatSave.StatusWord >> 11) & 7, c->FloatSave.TagWord & 0xffff);
                    if (c->Ecx > 0x10000 && !IsBadReadPtr((void*)c->Ecx, 4))   // object chain: ecx -> *ecx -> vtable entries
                    {
                        DWORD o1 = *(DWORD*)c->Ecx;
                        for (int lvl = 0; lvl < 2 && o1 > 0x10000 && !IsBadReadPtr((void*)o1, 16); ++lvl)
                        {
                            DWORD* v = (DWORD*)o1;
                            fprintf(g_log, "[bp]     level%d %08lx: %08lx %08lx %08lx %08lx\n", lvl, o1, v[0], v[1], v[2], v[3]);
                            o1 = v[0];
                        }
                    }
                    for (int ai = -1; ai < 4; ++ai)   // strings reachable from ecx and the first stack arguments
                    {
                        DWORD v = ai < 0 ? c->Ecx : sp[1 + ai];
                        if (v < 0x10000 || IsBadReadPtr((void*)v, 24)) continue;
                        const char* s = (const char*)v;
                        int len = 0;
                        while (len < 60 && s[len] >= 0x20 && s[len] < 0x7f) ++len;
                        if (len >= 4) fprintf(g_log, "[bp]     %s -> \"%.*s\"\n", ai < 0 ? "ecx" : "arg", len, s);
                    }
                    // FNV_BP_MEM=path[,path...]: dump memory reachable from the entry state. A path is a base
                    // (ecx, edx, eax, esi, edi, a0..a3 = stack arguments) followed by "/hex" steps, each meaning
                    // p = *(p + hex), "+hex"/"-hex" steps (p += / -= hex), and an optional ":n" word count
                    // (default 8, max 32), e.g. "ecx/8/c:4" or "a0-14:12".
                    if (const char* mem = getenv("FNV_BP_MEM"))
                        for (const char* q = mem; q && *q;)
                        {
                            DWORD p = 0;
                            if (q[0] == 'a' && q[1] >= '0' && q[1] <= '3') p = sp[1 + (q[1] - '0')];
                            else if (!strncmp(q, "ecx", 3)) p = c->Ecx;
                            else if (!strncmp(q, "edx", 3)) p = c->Edx;
                            else if (!strncmp(q, "eax", 3)) p = c->Eax;
                            else if (!strncmp(q, "esi", 3)) p = c->Esi;
                            else if (!strncmp(q, "edi", 3)) p = c->Edi;
                            const char* e = q;
                            while (*e && *e != 0x2c && *e != '/' && *e != '+' && *e != '-' && *e != ':') ++e;
                            bool ok = true;
                            while (*e == '/' || *e == '+' || *e == '-')
                            {
                                char* end;
                                DWORD off = (DWORD)strtoul(e + 1, &end, 16);
                                char op = *e;
                                e = end;
                                if (op != '/') { p = op == '+' ? p + off : p - off; continue; }
                                if (IsBadReadPtr((void*)(p + off), 4)) { ok = false; break; }
                                p = *(DWORD*)(p + off);
                            }
                            int n = 8;
                            if (*e == ':') { char* end; n = (int)strtoul(e + 1, &end, 10); e = end; }
                            const char* pe = e;
                            while (*pe && *pe != 0x2c) ++pe;
                            fprintf(g_log, "[bp]     mem %.*s = %08lx:", (int)(pe - q), q, p);
                            for (int k = 0; ok && k < n && k < 32; ++k)
                            {
                                if (IsBadReadPtr((void*)(p + 4 * k), 4)) break;
                                fprintf(g_log, " %08lx", ((DWORD*)p)[k]);
                            }
                            fprintf(g_log, ok ? "\n" : " (unreadable)\n");
                            q = *pe ? pe + 1 : nullptr;
                        }
                    fflush(g_log);
                }
                if (b.hits <= 40 && g_retN < 64 && !getenv("FNV_BP_NORET"))
                {
                    DWORD* sp2 = (DWORD*)c->Esp;
                    g_retStk[g_retN] = sp2[0]; g_retId[g_retN] = b.original; ++g_retN;
                    sp2[0] = (DWORD)&bp_ret_tramp;
                }
                DWORD old;
                VirtualProtect(b.addr, 1, PAGE_EXECUTE_READWRITE, &old);
                *b.addr = b.orig;
                VirtualProtect(b.addr, 1, old, &old);
                g_bpStepping[GetCurrentThreadId() & 63] = &b;
                c->EFlags |= 0x100;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
    }
    else if (r->ExceptionCode == EXCEPTION_SINGLE_STEP)
    {
        Bp*& s = g_bpStepping[GetCurrentThreadId() & 63];
        if (s)
        {
            DWORD old;
            VirtualProtect(s->addr, 1, PAGE_EXECUTE_READWRITE, &old);
            *s->addr = 0xCC;
            VirtualProtect(s->addr, 1, old, &old);
            s = nullptr;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

LONG CALLBACK trace_handler(EXCEPTION_POINTERS* ep)
{
    auto r = ep->ExceptionRecord;
    if (r->ExceptionCode == EXCEPTION_BREAKPOINT && g_traceAddr && r->ExceptionAddress == g_traceAddr)
    {
        DWORD old;
        VirtualProtect(g_traceAddr, 1, PAGE_EXECUTE_READWRITE, &old);
        *g_traceAddr = g_traceOrig;
        VirtualProtect(g_traceAddr, 1, old, &old);
        if (const char* w = getenv("FNV_TRACE_WATCH")) { g_watchAddr = (DWORD)strtoul(w, nullptr, 16); g_watchPrev = *(DWORD*)g_watchAddr; }
        g_tracing = 1;
        g_traceThread = GetCurrentThreadId();
        ep->ContextRecord->EFlags |= 0x100;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (r->ExceptionCode == EXCEPTION_SINGLE_STEP && g_tracing && GetCurrentThreadId() == g_traceThread)
    {
        LONG k = g_traceIdx++;
        {
            // function entry? (EIP equals a relinked function start)
            auto it = std::lower_bound(g_byPtr.begin(), g_byPtr.end(), (void*)ep->ContextRecord->Eip, [](const AddrEntry& e, void* q) { return e.relinked < q; });
            if (it != g_byPtr.end() && it->relinked == (void*)ep->ContextRecord->Eip)
            {
                EntryRec& e = g_entries[(g_entryIdx++) & (kEntryRing - 1)];
                e.orig = it->original; e.ecx = ep->ContextRecord->Ecx; e.esp = ep->ContextRecord->Esp;
                e.ret = IsBadReadPtr((void*)ep->ContextRecord->Esp, 4) ? 0 : *(DWORD*)ep->ContextRecord->Esp;
            }
        }
        if (g_watchAddr && !IsBadReadPtr((void*)g_watchAddr, 4))
        {
            DWORD v = *(DWORD*)g_watchAddr;
            if (v != g_watchPrev)
            {
                if (g_watchCount < 64) g_watchHits[g_watchCount++] = WatchHit{k, ep->ContextRecord->Eip, g_watchPrev, v};
                g_watchPrev = v;
            }
        }
        auto c = ep->ContextRecord;
        g_traceRing[k & (kTraceRing - 1)] = c->Eip;
        g_traceRegs[k & (kTraceRing - 1)][0] = c->Eax; g_traceRegs[k & (kTraceRing - 1)][1] = c->Ecx;
        g_traceRegs[k & (kTraceRing - 1)][2] = c->Edx; g_traceRegs[k & (kTraceRing - 1)][3] = c->Esp;
        g_traceRegs[k & (kTraceRing - 1)][4] = c->Ebp;
        ep->ContextRecord->EFlags |= 0x100;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (r->ExceptionCode == EXCEPTION_SINGLE_STEP) { fprintf(g_log, "[trace] stray single-step at %p on thread %lu (tracing=%ld, tracer thread %lu)\n", r->ExceptionAddress, GetCurrentThreadId(), (long)g_tracing, g_traceThread); fflush(g_log); }
    return EXCEPTION_CONTINUE_SEARCH;
}

void dump_trace()
{
    if (!g_tracing) return;
    g_tracing = 0;
    LONG n = g_traceIdx;
    LONG count = n < kTraceRing ? n : kTraceRing;
    fprintf(g_log, "[trace] %ld steps recorded; last %ld (oldest first, function changes only):\n", (long)n, (long)count);
    DWORD prevFn = 0;
    for (LONG i = n - count; i < n; ++i)
    {
        DWORD ip = g_traceRing[i & (kTraceRing - 1)];
        auto it = std::upper_bound(g_byPtr.begin(), g_byPtr.end(), (void*)ip, [](void* p, const AddrEntry& e) { return p < e.relinked; });
        DWORD fn = 0, off = 0;
        if (it != g_byPtr.begin()) { --it; fn = it->original; off = ip - (DWORD)it->relinked; }
        if (fn != prevFn || i == n - 1)
        {
            if (off < 0x2000) fprintf(g_log, "[trace]   %08lx fnv_f_%08lx+0x%lx\n", ip, fn, off);
            else fprintf(g_log, "[trace]   %08lx (outside table)\n", ip);
            prevFn = fn;
        }
    }
    for (int w = 0; w < g_watchCount; ++w)
        fprintf(g_log, "[trace] watch %08lx changed at step %ld (eip after the write: %08lx): %08lx -> %08lx\n", g_watchAddr, (long)g_watchHits[w].step, g_watchHits[w].eip, g_watchHits[w].from, g_watchHits[w].to);
    if (g_watchAddr) fprintf(g_log, "[trace] watch %08lx now %08lx (hits %d)\n", g_watchAddr, *(DWORD*)g_watchAddr, g_watchCount);
    {
        LONG m = g_entryIdx, cnt = m < kEntryRing ? m : kEntryRing, from = m - (cnt < 120 ? cnt : 120);
        fprintf(g_log, "[trace] last function entries (original address, ecx, esp, return address):\n");
        for (LONG i = from; i < m; ++i)
        {
            EntryRec& e = g_entries[i & (kEntryRing - 1)];
            fprintf(g_log, "[trace]   enter %08lx ecx=%08lx esp=%08lx ret=%08lx\n", e.orig, e.ecx, e.esp, e.ret);
        }
    }
    if (getenv("FNV_TRACE_FULL"))
        if (FILE* ff = fopen("trace_full.txt", "w"))
        {
            for (LONG i = n - count; i < n; ++i)
            {
                auto& g = g_traceRegs[i & (kTraceRing - 1)];
                fprintf(ff, "%08lx eax=%08lx ecx=%08lx edx=%08lx esp=%08lx ebp=%08lx\n", g_traceRing[i & (kTraceRing - 1)], g[0], g[1], g[2], g[3], g[4]);
            }
            fclose(ff);
        }
    fprintf(g_log, "[trace] last 48 steps in detail:\n");
    for (LONG i = n - 48 > n - count ? n - 48 : n - count; i < n; ++i)
    {
        auto& g = g_traceRegs[i & (kTraceRing - 1)];
        fprintf(g_log, "[trace]   %08lx eax=%08lx ecx=%08lx edx=%08lx esp=%08lx ebp=%08lx\n", g_traceRing[i & (kTraceRing - 1)], g[0], g[1], g[2], g[3], g[4]);
    }
}
LPTOP_LEVEL_EXCEPTION_FILTER WINAPI ignore_set_filter(LPTOP_LEVEL_EXCEPTION_FILTER) { return nullptr; }
void log_exit_stack(const char* what, DWORD code)
{
    DWORD* sp = (DWORD*)_AddressOfReturnAddress();
    fprintf(g_log, "[loader] %s(0x%08lx) on thread %lu, caller stack:\n", what, code, GetCurrentThreadId());
    int shown = 0;
    for (int i = 0; i < 4000 && shown < 30 && !IsBadReadPtr(sp + i, 4); ++i)
        if (sp[i] >= 0x20001000 && sp[i] < 0x24000000) { fprintf(g_log, "[loader]   +%x %08lx", i * 4, sp[i]); describe((void*)sp[i]); ++shown; }
    fflush(g_log);
}
static int g_fileLog = -1;
HANDLE WINAPI wrap_CreateFileA(LPCSTR n, DWORD acc, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD fl, HANDLE tmpl)
{
    HANDLE h = CreateFileA(n, acc, share, sa, disp, fl, tmpl);
    DWORD e = GetLastError();
    if (g_fileLog < 0) g_fileLog = getenv("FNV_FILE_LOG") ? 1 : 0;
    if (g_fileLog) { fprintf(g_log, "[file] open %s acc=%08lx disp=%lu -> %s (err %lu)\n", n, acc, disp, h == INVALID_HANDLE_VALUE ? "FAIL" : "ok", e); fflush(g_log); }
    SetLastError(e);
    return h;
}
DWORD WINAPI wrap_GetFileAttributesA(LPCSTR n)
{
    DWORD r = GetFileAttributesA(n);
    DWORD e = GetLastError();
    if (g_fileLog < 0) g_fileLog = getenv("FNV_FILE_LOG") ? 1 : 0;
    if (g_fileLog) { fprintf(g_log, "[file] attrs %s -> %08lx\n", n, r); fflush(g_log); }
    SetLastError(e);
    return r;
}
HANDLE WINAPI wrap_FindFirstFileA(LPCSTR n, LPWIN32_FIND_DATAA fd)
{
    HANDLE h = FindFirstFileA(n, fd);
    DWORD e = GetLastError();
    if (g_fileLog < 0) g_fileLog = getenv("FNV_FILE_LOG") ? 1 : 0;
    if (g_fileLog) { fprintf(g_log, "[file] find %s -> %s\n", n, h == INVALID_HANDLE_VALUE ? "none" : fd->cFileName); fflush(g_log); }
    SetLastError(e);
    return h;
}
int WINAPI wrap_MessageBoxA(HWND, LPCSTR text, LPCSTR caption, UINT type)
{
    fprintf(g_log, "[msgbox] %s: %s\n", caption ? caption : "", text ? text : "");
    if (const char* dc = getenv("FNV_DUMP_MSGBOX"))   // "hexaddr:words[,hexaddr:words...]"
        for (const char* q = dc; q && *q;)
        {
            DWORD addr = (DWORD)strtoul(q, nullptr, 16);
            const char* c = strchr(q, ':');
            int words = c ? atoi(c + 1) : 8;
            for (int i = 0; i < words; ++i)
                if (!IsBadReadPtr((void*)(addr + i * 4), 4))
                    fprintf(g_log, "[msgdump] %08lx: %08lx\n", addr + i * 4, *(DWORD*)(addr + i * 4));
            q = strchr(q, ',');
            if (q) ++q;
        }
    fflush(g_log);
    return IDOK;
}
VOID WINAPI wrap_OutputDebugStringA(LPCSTR s)
{
    fprintf(g_log, "[dbgstr] %s", s ? s : "");
    fflush(g_log);
}
// SHGetFolderPathA(CSIDL_PERSONAL): give the game a private copy of "My Games\FalloutNV" (seeded from the real one) so
// experiments never touch the player's INI files. FNV_REAL_DOCS=1 uses the real folder.
static char g_docsRoot[MAX_PATH];
HRESULT WINAPI wrap_SHGetFolderPathA(HWND hwnd, int csidl, HANDLE tok, DWORD flags, LPSTR out)
{
    if ((csidl & 0xff) == CSIDL_PERSONAL && !getenv("FNV_REAL_DOCS") && g_docsRoot[0])
    {
        strcpy(out, g_docsRoot);
        return S_OK;
    }
    typedef HRESULT(WINAPI * Fn)(HWND, int, HANDLE, DWORD, LPSTR);
    static Fn real = (Fn)GetProcAddress(LoadLibraryA("shell32.dll"), "SHGetFolderPathA");
    return real(hwnd, csidl, tok, flags, out);
}
HINSTANCE WINAPI wrap_ShellExecuteA(HWND, LPCSTR op, LPCSTR file, LPCSTR params, LPCSTR dir, INT show)
{
    fprintf(g_log, "[loader] ShellExecuteA(%s, %s, %s) blocked\n", op ? op : "", file ? file : "", params ? params : "");
    fflush(g_log);
    return (HINSTANCE)2;   // <= 32: failure, the game carries on
}
// FNV_INPUT=script (or FNV_INPUT_FILE=path): scripted DirectInput for unattended runs, off unless set. The game's
// DirectInput8Create import is wrapped; CreateDevice, Acquire, GetDeviceState and GetDeviceData of the devices it
// creates are patched so the script's keys/mouse are merged into what the real devices report, and a lost or
// unacquired device (window in the background) reads as idle instead of failing. Script: ';'-separated steps
// "delay_ms:verb:args", delay counted from the previous step (the first from the first poll):
//   key:<DIK hex>[,hold ms]   move:<dx>,<dy>   click[:hold ms]   log:<text>
struct FnvDiObj { DWORD ofs, data, time, seq; DWORD app; };    // DIDEVICEOBJECTDATA (callers pass cbObjectData)
struct InStep { DWORD delay; char verb; int a, b; char text[48]; };
std::vector<InStep> g_in;
size_t g_inPos = 0;
DWORD g_inNext = 0, g_inSeq = 0;
bool g_inStarted = false;
void* g_inKbd = nullptr;
void* g_inMouse = nullptr;
BYTE g_inKeys[256];
DWORD g_inKeyUp[256];
int g_inDx = 0, g_inDy = 0;
bool g_inBtn = false;
DWORD g_inBtnUp = 0;
static const char* g_dumpTag;          // FNV_D3D_DUMP=<non-numeric tag>: set by the D3D hook
static volatile LONG g_dumpTagHit;
std::vector<FnvDiObj> g_inKbdEv, g_inMouseEv;
CRITICAL_SECTION g_inLock;
typedef HRESULT(WINAPI* DiCreateFn)(HINSTANCE, DWORD, REFIID, void**, void*);
typedef HRESULT(WINAPI* DiCreateDevFn)(void*, REFGUID, void**, void*);
typedef HRESULT(WINAPI* DiAcquireFn)(void*);
typedef HRESULT(WINAPI* DiStateFn)(void*, DWORD, void*);
typedef HRESULT(WINAPI* DiDataFn)(void*, DWORD, FnvDiObj*, DWORD*, DWORD);
DiCreateFn g_realDiCreate;
DiCreateDevFn g_realCreateDev;
typedef HRESULT(WINAPI* DiCoopFn)(void*, HWND, DWORD);
typedef HRESULT(WINAPI* DiFormatFn)(void*, const DWORD*);
struct DevReal { void** vtbl; DiAcquireFn acquire; DiStateFn state; DiDataFn data; DiCoopFn coop; DiFormatFn format; };
bool g_bg = false;      // FNV_BACKGROUND=1: off-screen, never activated, real keyboard/mouse ignored
bool g_inLog = false;   // FNV_INPUT_LOG=1: log cooperative level and Acquire/GetDeviceState results as they change
const char* dev_name(void* self);
void in_log_hr(void* self, const char* what, HRESULT hr, HRESULT* last)
{
    if (!g_inLog || hr == *last) return;
    *last = hr;
    fprintf(g_log, "[input] %s %s -> %08lx (foreground %p)\n", dev_name(self), what, (unsigned long)hr, (void*)GetForegroundWindow());
    fflush(g_log);
}
DevReal g_devReal[4];
int g_devRealN = 0;
const GUID kSysKeyboard = {0x6F1D2B61, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
const GUID kSysMouse = {0x6F1D2B60, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};

void input_parse(const char* s)
{
    while (s && *s)
    {
        while (*s == ';' || *s == ' ' || *s == 9) ++s;
        if (!*s) break;
        if (*s == '#') { s = strchr(s, ';'); continue; }    // comment to the end of the line
        InStep st = {};
        char* e;
        st.delay = strtoul(s, &e, 10);
        if (*e != ':') break;
        const char* v = e + 1;
        const char* arg = strchr(v, ':');
        const char* end = strchr(v, ';');
        if (!end) end = v + strlen(v);
        if (arg && arg > end) arg = nullptr;
        st.verb = *v;
        if (st.verb == 'k' && arg) { st.a = (int)strtoul(arg + 1, &e, 16); st.b = *e == 0x2c ? atoi(e + 1) : 100; }
        else if (st.verb == 'm' && arg) { st.a = (int)strtol(arg + 1, &e, 10); st.b = *e == 0x2c ? atoi(e + 1) : 0; }
        else if (st.verb == 'c') st.a = arg ? atoi(arg + 1) : 100;
        else if (st.verb == 'l' && arg) _snprintf_s(st.text, sizeof(st.text), _TRUNCATE, "%.*s", (int)(end - arg - 1), arg + 1);
        if (st.verb == 't' && arg)   // type:<text> -> one key per character, 150 ms apart (console commands)
        {
            static const char* row1 = "qwertyuiop", *row2 = "asdfghjkl", *row3 = "zxcvbnm";
            DWORD delay = st.delay;
            for (const char* c = arg + 1; c < end; ++c)
            {
                char ch = (char)tolower((unsigned char)*c);
                int dik = 0;
                if (const char* p = strchr(row1, ch); p && ch) dik = 0x10 + (int)(p - row1);
                else if (const char* p2 = strchr(row2, ch); p2 && ch) dik = 0x1E + (int)(p2 - row2);
                else if (const char* p3 = strchr(row3, ch); p3 && ch) dik = 0x2C + (int)(p3 - row3);
                else if (ch >= '1' && ch <= '9') dik = 0x02 + (ch - '1');
                else if (ch == '0') dik = 0x0B;
                else if (ch == ' ') dik = 0x39;
                else if (ch == '-') dik = 0x0C;
                else if (ch == '.') dik = 0x34;
                else if (ch == '`') dik = 0x29;   // console key
                if (!dik) continue;
                InStep k = {};
                k.delay = delay; k.verb = 'k'; k.a = dik; k.b = 60;
                g_in.push_back(k);
                delay = 150;
            }
            s = *end ? end + 1 : nullptr;
            continue;
        }
        g_in.push_back(st);
        s = *end ? end + 1 : nullptr;
    }
}

void input_event(std::vector<FnvDiObj>& q, DWORD ofs, DWORD data)
{
    FnvDiObj o = {ofs, data, GetTickCount(), ++g_inSeq, 0};
    q.push_back(o);
}

void input_tick()   // under g_inLock
{
    DWORD now = GetTickCount();
    if (!g_inStarted)
    {
        g_inStarted = true;
        g_inNext = now + (g_in.empty() ? 0 : g_in[0].delay);
    }
    while (g_inPos < g_in.size() && (LONG)(now - g_inNext) >= 0)
    {
        const InStep& st = g_in[g_inPos];
        fprintf(g_log, "[input] step %u %c %d %d %s\n", (unsigned)g_inPos, st.verb, st.a, st.b, st.text);
        fflush(g_log);
        if (st.verb == 'l' && g_dumpTag && *g_dumpTag && !strncmp(st.text, g_dumpTag, strlen(g_dumpTag)))
            g_dumpTagHit = 1;   // FNV_D3D_DUMP=<tag>: arms the D3D pass dump (see ds_dump_*)
        if (st.verb == 'k' && st.a > 0 && st.a < 256)
        {
            g_inKeys[st.a] = 0x80;
            g_inKeyUp[st.a] = now + st.b;
            input_event(g_inKbdEv, st.a, 0x80);
        }
        else if (st.verb == 'm')
        {
            g_inDx += st.a;
            g_inDy += st.b;
            if (st.a) input_event(g_inMouseEv, 0, (DWORD)st.a);
            if (st.b) input_event(g_inMouseEv, 4, (DWORD)st.b);
        }
        else if (st.verb == 'c')
        {
            g_inBtn = true;
            g_inBtnUp = now + st.a;
            input_event(g_inMouseEv, 12, 0x80);
        }
        if (++g_inPos < g_in.size()) g_inNext = now + g_in[g_inPos].delay;
    }
    for (int k = 1; k < 256; ++k)
        if (g_inKeys[k] && (LONG)(now - g_inKeyUp[k]) >= 0)
        {
            g_inKeys[k] = 0;
            input_event(g_inKbdEv, k, 0);
        }
    if (g_inBtn && (LONG)(now - g_inBtnUp) >= 0)
    {
        g_inBtn = false;
        input_event(g_inMouseEv, 12, 0);
    }
}

DevReal* dev_real(void* self)
{
    void** vt = *(void***)self;
    for (int i = 0; i < g_devRealN; ++i)
        if (g_devReal[i].vtbl == vt) return &g_devReal[i];
    return nullptr;
}

const char* dev_name(void* self) { return self == g_inKbd ? "keyboard" : self == g_inMouse ? "mouse" : "device"; }

HRESULT WINAPI in_acquire(void* self)
{
    DevReal* r = dev_real(self);
    HRESULT hr = r ? r->acquire(self) : 0;
    static HRESULT last[2] = {1, 1};
    in_log_hr(self, "Acquire", hr, &last[self == g_inMouse]);
    if (g_in.empty() && !g_bg) return hr;   // no script: the game sees the real result
    return hr < 0 ? 0 : hr;
}

HRESULT WINAPI in_state(void* self, DWORD cb, void* data)
{
    DevReal* r = dev_real(self);
    HRESULT hr = r ? r->state(self, cb, data) : -1;
    static HRESULT last[2] = {1, 1};
    in_log_hr(self, "GetDeviceState", hr, &last[self == g_inMouse]);
    if (g_in.empty() && !g_bg) return hr;
    if (hr < 0 || g_bg) { memset(data, 0, cb); hr = 0; }
    EnterCriticalSection(&g_inLock);
    input_tick();
    if (self == g_inKbd && cb >= 256)
        for (int k = 0; k < 256; ++k) ((BYTE*)data)[k] |= g_inKeys[k];
    else if (self == g_inMouse && cb >= 16)
    {
        ((LONG*)data)[0] += g_inDx;
        ((LONG*)data)[1] += g_inDy;
        g_inDx = g_inDy = 0;
        if (g_inBtn) ((BYTE*)data)[12] |= 0x80;
    }
    LeaveCriticalSection(&g_inLock);
    return hr;
}

HRESULT WINAPI in_data(void* self, DWORD cb, FnvDiObj* rg, DWORD* inout, DWORD flags)
{
    DevReal* r = dev_real(self);
    DWORD cap = inout ? *inout : 0;
    HRESULT hr = r ? r->data(self, cb, rg, inout, flags) : -1;
    static HRESULT last[2] = {1, 1};
    in_log_hr(self, "GetDeviceData", hr, &last[self == g_inMouse]);
    if (g_in.empty() && !g_bg) return hr;
    if ((hr < 0 || g_bg) && inout) *inout = 0;
    if (hr < 0 || g_bg) hr = 0;
    EnterCriticalSection(&g_inLock);
    input_tick();
    std::vector<FnvDiObj>* q = self == g_inKbd ? &g_inKbdEv : self == g_inMouse ? &g_inMouseEv : nullptr;
    if (q && rg && inout && cb >= 16)
    {
        size_t used = 0;
        while (*inout < cap && used < q->size())
        {
            FnvDiObj* o = (FnvDiObj*)((BYTE*)rg + *inout * cb);
            memset(o, 0, cb);
            o->ofs = (*q)[used].ofs; o->data = (*q)[used].data; o->time = (*q)[used].time; o->seq = (*q)[used].seq;
            ++*inout;
            ++used;
        }
        if (!(flags & 1))   // DIGDD_PEEK leaves the events queued
            q->erase(q->begin(), q->begin() + used);
    }
    LeaveCriticalSection(&g_inLock);
    return hr;
}

void patch_slot(void** vt, int slot, void* fn)
{
    DWORD old;
    VirtualProtect(&vt[slot], 4, PAGE_READWRITE, &old);
    vt[slot] = fn;
    VirtualProtect(&vt[slot], 4, old, &old);
}

HRESULT WINAPI in_coop(void* self, HWND h, DWORD flags)
{
    DevReal* r = dev_real(self);
    HRESULT hr = r ? r->coop(self, h, flags) : -1;
    fprintf(g_log, "[input] %s SetCooperativeLevel(hwnd %p, flags %lx) -> %08lx\n", dev_name(self), (void*)h,
            (unsigned long)flags, (unsigned long)hr);
    fflush(g_log);
    return hr;
}

HRESULT WINAPI in_format(void* self, const DWORD* f)   // DIDATAFORMAT: dwSize, dwObjSize, dwFlags, dwDataSize, dwNumObjs, rgodf
{
    DevReal* r = dev_real(self);
    HRESULT hr = r ? r->format(self, f) : -1;
    fprintf(g_log, "[input] %s SetDataFormat(%p: size %lu objsize %lu flags %lx datasize %lu objs %lu rgodf %08lx) -> %08lx\n",
            dev_name(self), (void*)f, f ? f[0] : 0, f ? f[1] : 0, f ? f[2] : 0, f ? f[3] : 0, f ? f[4] : 0, f ? f[5] : 0,
            (unsigned long)hr);
    if (f && f[5] && f[4])
        for (DWORD i = 0; i < f[4] && i < 4; ++i)
        {
            const DWORD* o = (const DWORD*)f[5] + 4 * i;   // DIOBJECTDATAFORMAT: pguid, dwOfs, dwType, dwFlags
            fprintf(g_log, "[input]   obj %lu pguid %08lx ofs %lu type %08lx flags %lx\n", i, o[0], o[1], o[2], o[3]);
        }
    fflush(g_log);
    return hr;
}

HRESULT WINAPI in_create_device(void* self, REFGUID guid, void** dev, void* outer)
{
    HRESULT hr = g_realCreateDev(self, guid, dev, outer);
    if (hr < 0 || !dev || !*dev) return hr;
    bool kbd = !memcmp(&guid, &kSysKeyboard, sizeof(GUID)), mouse = !memcmp(&guid, &kSysMouse, sizeof(GUID));
    if (kbd) g_inKbd = *dev;
    if (mouse) g_inMouse = *dev;
    if (!dev_real(*dev) && g_devRealN < 4)
    {
        void** vt = *(void***)*dev;
        DevReal& r = g_devReal[g_devRealN++];
        r.vtbl = vt;
        r.acquire = (DiAcquireFn)vt[7];
        r.state = (DiStateFn)vt[9];
        r.data = (DiDataFn)vt[10];
        r.coop = (DiCoopFn)vt[13];
        r.format = (DiFormatFn)vt[11];
        patch_slot(vt, 11, (void*)&in_format);
        patch_slot(vt, 13, (void*)&in_coop);
        patch_slot(vt, 7, (void*)&in_acquire);
        patch_slot(vt, 9, (void*)&in_state);
        patch_slot(vt, 10, (void*)&in_data);
    }
    fprintf(g_log, "[input] device %p created (%s)\n", *dev, kbd ? "keyboard" : mouse ? "mouse" : "other");
    fflush(g_log);
    return hr;
}

HRESULT WINAPI wrap_DirectInput8Create(HINSTANCE inst, DWORD ver, REFIID iid, void** out, void* outer)
{
    HRESULT hr = g_realDiCreate(inst, ver, iid, out, outer);
    if (hr >= 0 && out && *out && !g_realCreateDev)
    {
        void** vt = *(void***)*out;
        g_realCreateDev = (DiCreateDevFn)vt[3];
        patch_slot(vt, 3, (void*)&in_create_device);
        fprintf(g_log, "[input] DirectInput8Create wrapped, %u scripted steps\n", (unsigned)g_in.size());
        fflush(g_log);
    }
    return hr;
}

bool input_init()
{
    const char* s = getenv("FNV_INPUT");
    static std::vector<char> file;
    if (const char* path = getenv("FNV_INPUT_FILE"))
        if (FILE* f = fopen(path, "rb"))
        {
            int c;
            while ((c = fgetc(f)) != EOF) file.push_back(c == 10 || c == 13 ? ';' : (char)c);
            file.push_back(0);
            fclose(f);
            s = file.data();
        }
    g_inLog = getenv("FNV_INPUT_LOG") != nullptr;
    if (!s && !g_inLog && !g_bg) return false;
    InitializeCriticalSection(&g_inLock);
    input_parse(s);
    return true;
}

// The player closing the window (title-bar X, Alt+F4) reaches DefWindowProcA as SC_CLOSE / WM_CLOSE and then
// DestroyWindow. Log it, and mark a later crash as shutdown rather than gameplay.
volatile LONG g_userClosed = 0;
LRESULT WINAPI wrap_DefWindowProcA(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_CLOSE || (msg == WM_SYSCOMMAND && (wp & 0xfff0) == SC_CLOSE))
        if (!InterlockedExchange(&g_userClosed, 1))
        {
            fprintf(g_log, "[loader] window closed by the user (%s)\n", msg == WM_CLOSE ? "WM_CLOSE" : "SC_CLOSE");
            fflush(g_log);
        }
    return DefWindowProcA(h, msg, wp, lp);
}
BOOL WINAPI wrap_DestroyWindow(HWND h)
{
    fprintf(g_log, "[loader] DestroyWindow(%p)%s\n", (void*)h, g_userClosed ? " after the user closed the window" : "");
    fflush(g_log);
    return DestroyWindow(h);
}

// FNV_BACKGROUND=1: the window has no taskbar button and is never activated, so a run does not take focus from what
// the user is doing. It stays on-screen but fully transparent and click-through (a layered window at alpha 1/255):
// an off-screen window is not composited and PrintWindow (tools/screenshot.ps1) then captured only black frames.
// FNV_BACKGROUND=offscreen restores the old placement right of every monitor. The game keeps running unfocused
// through bAlwaysActive=1 in the private INI.
HWND WINAPI wrap_CreateWindowExA(DWORD ex, LPCSTR cls, LPCSTR title, DWORD style, int x, int y, int w, int h, HWND parent,
                                 HMENU menu, HINSTANCE inst, LPVOID param)
{
    const char* bg = getenv("FNV_BACKGROUND");
    bool offscreen = bg && !strcmp(bg, "offscreen");
    if (g_bg && !parent)
    {
        if (offscreen)
        {
            x = GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN) + 64;
            y = GetSystemMetrics(SM_YVIRTUALSCREEN);
        }
        ex = (ex | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | (offscreen ? 0 : WS_EX_LAYERED | WS_EX_TRANSPARENT)) &
             ~WS_EX_APPWINDOW;
    }
    HWND hw = CreateWindowExA(ex, cls, title, style, x, y, w, h, parent, menu, inst, param);
    if (g_bg && !parent)
    {
        if (!offscreen && hw)
            SetLayeredWindowAttributes(hw, 0, 1, LWA_ALPHA);
        fprintf(g_log, "[loader] FNV_BACKGROUND: window %p %s at %d,%d\n", (void*)hw,
                offscreen ? "created off-screen" : "transparent and click-through", x, y);
        fflush(g_log);
    }
    return hw;
}
BOOL WINAPI wrap_ShowWindow(HWND h, int cmd)
{
    if (g_bg && cmd != SW_HIDE && cmd != SW_MINIMIZE) cmd = SW_SHOWNOACTIVATE;
    return ShowWindow(h, cmd);
}
BOOL WINAPI wrap_SetForegroundWindow(HWND h) { return g_bg ? TRUE : SetForegroundWindow(h); }
BOOL WINAPI wrap_SetWindowPos(HWND h, HWND after, int x, int y, int cx, int cy, UINT flags)
{
    if (g_bg) flags |= SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE;
    return SetWindowPos(h, after, x, y, cx, cy, flags);
}

// With the game's MemoryManager routed to the CRT heap (FNV_SYSHEAP, default), the memory heaps still reserved
// their pools at startup (0xaa65b0 probes base addresses 16 MB apart for four 514-577 MB pools, never committed).
// They took 2.1 GB of the 4 GB address space, and a 5.6 MB texture allocation failed in the world (a null memset).
// Reserve-only requests of 256 MB or more are capped at 16 MB while the system heap is on (logged once).
bool g_sysheap = true;
LPVOID WINAPI wrap_VirtualAlloc(LPVOID addr, SIZE_T size, DWORD type, DWORD protect)
{
    if (g_sysheap && type == MEM_RESERVE && size >= (256u << 20))
    {
        static volatile LONG logged = 0;
        if (!InterlockedExchange(&logged, 1))
        {
            fprintf(g_log, "[loader] VirtualAlloc reserves of %lu MB from %p capped at 16 MB (FNV_SYSHEAP)\n",
                    (unsigned long)(size >> 20), _ReturnAddress());
            fflush(g_log);
        }
        size = 16u << 20;
    }
    return VirtualAlloc(addr, size, type, protect);
}

// FNV_D3D_STATS=1: count what the game asks Direct3D 9 to do and log it every 120 frames (draw calls, primitives,
// draws with no vertex/pixel shader bound, shaders created, BeginScene count, Clear calls). With HDR off the world
// view is black, with HDR on white (tone mapping of an empty scene): this says whether the world issues draws at all.
typedef HRESULT(WINAPI* D3DFn)(void*);
typedef void*(WINAPI* D3DCreate9Fn)(UINT);
typedef HRESULT(WINAPI* D3DCreateDevFn)(void*, UINT, DWORD, HWND, DWORD, void*, void**);
typedef HRESULT(WINAPI* D3DPresentFn)(void*, const RECT*, const RECT*, HWND, const void*);
typedef HRESULT(WINAPI* D3DDrawFn)(void*, DWORD, DWORD, DWORD);
typedef HRESULT(WINAPI* D3DDrawIdxFn)(void*, DWORD, INT, UINT, UINT, UINT, UINT);
typedef HRESULT(WINAPI* D3DDrawUPFn)(void*, DWORD, UINT, const void*, UINT);
typedef HRESULT(WINAPI* D3DDrawIdxUPFn)(void*, DWORD, UINT, UINT, UINT, const void*, DWORD, const void*, UINT);
typedef HRESULT(WINAPI* D3DSetShFn)(void*, void*);
typedef HRESULT(WINAPI* D3DCreateShFn)(void*, const DWORD*, void**);
typedef HRESULT(WINAPI* D3DClearFn)(void*, DWORD, const void*, DWORD, DWORD, float, DWORD);
static D3DCreate9Fn g_realD3DCreate9;
static D3DCreateDevFn g_realCreateDevice;
static D3DPresentFn g_realPresent;
static D3DFn g_realBeginScene;
static D3DDrawFn g_realDraw;
static D3DDrawIdxFn g_realDrawIdx;
static D3DDrawUPFn g_realDrawUP;
static D3DDrawIdxUPFn g_realDrawIdxUP;
static D3DSetShFn g_realSetVS, g_realSetPS;
static D3DCreateShFn g_realCreateVS, g_realCreatePS;
static D3DClearFn g_realClear;
typedef HRESULT(WINAPI* D3DSetConstFn)(void*, UINT, const float*, UINT);
static D3DSetConstFn g_realSetVSConst;
typedef HRESULT(WINAPI* D3DSetTexFn)(void*, DWORD, void*);
typedef HRESULT(WINAPI* D3DSetRTFn)(void*, DWORD, void*);
static D3DSetTexFn g_realSetTexture;
static D3DSetRTFn g_realSetRT;
static void* g_dsTex0;
static void* g_dsTex[8];
static void* g_dsVS;
static void* g_dsPS;
static void fpu_trap_where(const char* what, DWORD a);
static volatile LONG g_dsNoTex0, g_dsRTsets, g_dsRTnull;
static HRESULT WINAPI ds_SetTexture(void* d, DWORD st, void* t)
{
    if (st == 0) g_dsTex0 = t;
    if (st < 8) g_dsTex[st] = t;
    return g_realSetTexture(d, st, t);
}

// FNV_D3D_DUMP=<frame number> or =<input log tag> (e.g. OK3; then the 20th frame with >= 200 draws after the tag,
// FNV_D3D_DUMP_SKIP=n changes 20): during that one frame every SetRenderTarget / SetDepthStencilSurface / Clear /
// StretchRect is logged ([dump] lines, with the game caller), the first draws after each colour-target switch log
// the key render states, viewport, scissor, shaders and textures, and each colour target is read back when the
// frame switches away from it (and at Present, and StretchRect destinations) to out/d3d_dump/NN_<w>x<h>_<fmt>.bmp
// with per-channel mean/min/max.
struct DsDesc { DWORD Format, Type, Usage, Pool, MultiSampleType, MultiSampleQuality; UINT Width, Height; };
struct DsLocked { INT Pitch; void* pBits; };
struct DsViewport { DWORD X, Y, Width, Height; float MinZ, MaxZ; };
template <class R, class... A> static R ds_vcall(void* o, int slot, A... a)
{
    typedef R(WINAPI * F)(void*, A...);
    return ((F)(*(void***)o)[slot])(o, a...);
}
typedef HRESULT(WINAPI* D3DStretchFn)(void*, void*, const RECT*, void*, const RECT*, DWORD);
typedef HRESULT(WINAPI* D3DSetDSFn)(void*, void*);
typedef HRESULT(WINAPI* D3DSetRSFn)(void*, DWORD, DWORD);
typedef HRESULT(WINAPI* D3DSetVPFn)(void*, const DsViewport*);
typedef HRESULT(WINAPI* D3DSetScFn)(void*, const RECT*);
static D3DStretchFn g_realStretch;
static D3DSetDSFn g_realSetDS;
static D3DSetRSFn g_realSetRS;
static D3DSetVPFn g_realSetVP;
static D3DSetScFn g_realSetSc;
static D3DFn g_realEndScene;
static DWORD g_rsShadow[256];
static DsViewport g_vpShadow;
static RECT g_scShadow;
static int g_dumpState;              // 0 off, 1 armed, 2 dumping this frame, 3 done
static LONG g_dumpFrame = -1, g_dumpHeavy, g_dumpSkip = 20, g_frameDraws;
static int g_dumpSeg, g_dumpSegDraws, g_dumpDetail, g_dumpFileN;
static void* g_dumpRT;               // colour target 0 of the current segment (not referenced)
static char g_dumpDir[MAX_PATH];
static void ds_rs_defaults()
{
    g_rsShadow[7] = 1; g_rsShadow[14] = 1; g_rsShadow[19] = 2; g_rsShadow[20] = 1; g_rsShadow[22] = 3;
    g_rsShadow[23] = 4; g_rsShadow[25] = 8; g_rsShadow[56] = 8; g_rsShadow[168] = 0xf; g_rsShadow[171] = 1;
}
static const char* ds_fmt(DWORD f)
{
    static char buf[16];
    switch (f)
    {
    case 21: return "A8R8G8B8";
    case 22: return "X8R8G8B8";
    case 23: return "R5G6B5";
    case 31: return "A2B10G10R10";
    case 35: return "A2R10G10B10";
    case 36: return "A16B16G16R16";
    case 75: return "D24S8";
    case 77: return "D24X8";
    case 80: return "D16";
    case 111: return "R16F";
    case 112: return "G16R16F";
    case 113: return "A16B16G16R16F";
    case 114: return "R32F";
    case 115: return "G32R32F";
    case 116: return "A32B32G32R32F";
    }
    if (f > 0xffff)
    {
        memcpy(buf, &f, 4);
        buf[4] = 0;
        return buf;
    }
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "fmt%lu", f);
    return buf;
}
static float g_psC[32][4];          // pixel shader constants c0-c31 as last set, and who set them
static void* g_psCaller[32];
static void ds_where(DWORD a)   // a return address: relinked function (original address) or raw
{
    DWORD lo = g_byPtr.empty() ? 0 : (DWORD)g_byPtr.front().relinked;
    DWORD hi = g_byPtr.empty() ? 0 : (DWORD)g_byPtr.back().relinked + 0x10000;
    if (a >= lo && a < hi) fpu_trap_where("caller", a);
    else fprintf(g_log, " caller %08lx", a);
}
static bool ds_desc(void* s, DsDesc* d)
{
    memset(d, 0, sizeof(*d));
    return s && ds_vcall<HRESULT>(s, 12, d) >= 0;
}
static void ds_log_surf(const char* what, void* s)
{
    DsDesc d;
    if (!s) fprintf(g_log, " %s null", what);
    else if (ds_desc(s, &d))
        fprintf(g_log, " %s %p %ux%u %s usage %lx pool %lu ms %lu", what, s, d.Width, d.Height, ds_fmt(d.Format), d.Usage,
                d.Pool, d.MultiSampleType);
    else fprintf(g_log, " %s %p (no desc)", what, s);
}
static float ds_h2f(WORD h)
{
    DWORD s = (h & 0x8000u) << 16, e = (h >> 10) & 0x1f, m = h & 0x3ff, f;
    if (e == 0)
    {
        if (!m) f = s;
        else
        {
            e = 113;
            while (!(m & 0x400)) { m <<= 1; --e; }
            f = s | (e << 23) | ((m & 0x3ff) << 13);
        }
    }
    else if (e == 31) f = s | 0x7f800000u | (m << 13);
    else f = s | ((e + 112) << 23) | (m << 13);
    float r;
    memcpy(&r, &f, 4);
    return r;
}
static bool ds_pixel(DWORD fmt, const BYTE* p, float* c, int* nch)   // -> RGBA floats
{
    c[0] = c[1] = c[2] = 0; c[3] = 1;
    switch (fmt)
    {
    case 21: case 22:
        c[2] = p[0] / 255.f; c[1] = p[1] / 255.f; c[0] = p[2] / 255.f; c[3] = fmt == 21 ? p[3] / 255.f : 1; *nch = 4; return true;
    case 35: { DWORD v = *(const DWORD*)p; c[2] = (v & 1023) / 1023.f; c[1] = ((v >> 10) & 1023) / 1023.f;
               c[0] = ((v >> 20) & 1023) / 1023.f; c[3] = (v >> 30) / 3.f; *nch = 4; return true; }
    case 23: { WORD v = *(const WORD*)p; c[2] = (v & 31) / 31.f; c[1] = ((v >> 5) & 63) / 63.f; c[0] = (v >> 11) / 31.f; *nch = 3; return true; }
    case 111: c[0] = ds_h2f(*(const WORD*)p); *nch = 1; return true;
    case 112: c[0] = ds_h2f(((const WORD*)p)[0]); c[1] = ds_h2f(((const WORD*)p)[1]); *nch = 2; return true;
    case 113: for (int i = 0; i < 4; ++i) c[i] = ds_h2f(((const WORD*)p)[i]); *nch = 4; return true;
    case 114: c[0] = *(const float*)p; *nch = 1; return true;
    case 115: c[0] = ((const float*)p)[0]; c[1] = ((const float*)p)[1]; *nch = 2; return true;
    case 116: for (int i = 0; i < 4; ++i) c[i] = ((const float*)p)[i]; *nch = 4; return true;
    }
    return false;
}
static int ds_bpp(DWORD fmt)
{
    switch (fmt)
    {
    case 21: case 22: case 35: case 112: case 114: return 4;
    case 23: case 111: return 2;
    case 113: case 115: return 8;
    case 116: return 16;
    }
    return 0;
}
static void ds_dump_surface(void* dev, void* src, const char* why)
{
    if (!src || g_dumpFileN >= 120) return;
    DsDesc d;
    if (!ds_desc(src, &d)) return;
    int n = ++g_dumpFileN;
    fprintf(g_log, "[dump]   readback #%02d (%s) of %p %ux%u %s:", n, why, src, d.Width, d.Height, ds_fmt(d.Format));
    if (!ds_bpp(d.Format)) { fprintf(g_log, " format not handled\n"); return; }
    void* rd = src;
    void* tmp = nullptr;
    void* sys = nullptr;
    HRESULT hr;
    if (d.MultiSampleType)
    {
        hr = ds_vcall<HRESULT>(dev, 28, d.Width, d.Height, d.Format, (DWORD)0, (DWORD)0, (BOOL)FALSE, &tmp, (HANDLE*)nullptr);
        if (hr < 0 || (hr = g_realStretch(dev, src, nullptr, tmp, nullptr, 0)) < 0)
        {
            fprintf(g_log, " MSAA resolve failed %08lx\n", hr);
            if (tmp) ds_vcall<ULONG>(tmp, 2);
            return;
        }
        rd = tmp;
    }
    hr = ds_vcall<HRESULT>(dev, 36, d.Width, d.Height, d.Format, (DWORD)2 /* SYSTEMMEM */, &sys, (HANDLE*)nullptr);
    if (hr >= 0) hr = ds_vcall<HRESULT>(dev, 32, rd, sys);
    DsLocked lr = {};
    if (hr >= 0) hr = ds_vcall<HRESULT>(sys, 13, &lr, (const RECT*)nullptr, (DWORD)0x10 /* READONLY */);
    if (hr < 0)
    {
        fprintf(g_log, " readback failed %08lx\n", hr);
        if (sys) ds_vcall<ULONG>(sys, 2);
        if (tmp) ds_vcall<ULONG>(tmp, 2);
        return;
    }
    UINT w = d.Width, h = d.Height, rowb = (w * 3 + 3) & ~3u;
    std::vector<BYTE> bmp(54 + rowb * h);
    BYTE* hd = bmp.data();
    hd[0] = 'B'; hd[1] = 'M';
    *(DWORD*)(hd + 2) = (DWORD)bmp.size(); *(DWORD*)(hd + 10) = 54; *(DWORD*)(hd + 14) = 40;
    *(LONG*)(hd + 18) = (LONG)w; *(LONG*)(hd + 22) = (LONG)h; *(WORD*)(hd + 26) = 1; *(WORD*)(hd + 28) = 24;
    *(DWORD*)(hd + 34) = rowb * h;
    double sum[4] = {}, mn[4], mx[4];
    for (int i = 0; i < 4; ++i) { mn[i] = 1e30; mx[i] = -1e30; }
    unsigned long nonfinite = 0;
    int nch = 4, bpp = ds_bpp(d.Format);
    for (UINT y = 0; y < h; ++y)
    {
        const BYTE* row = (const BYTE*)lr.pBits + (size_t)y * lr.Pitch;
        BYTE* out = hd + 54 + (size_t)(h - 1 - y) * rowb;
        for (UINT x = 0; x < w; ++x)
        {
            float c[4];
            ds_pixel(d.Format, row + x * bpp, c, &nch);
            for (int i = 0; i < 4; ++i)
            {
                if (!(c[i] == c[i]) || c[i] > 1e30f || c[i] < -1e30f) { ++nonfinite; c[i] = 0; }
                sum[i] += c[i];
                if (c[i] < mn[i]) mn[i] = c[i];
                if (c[i] > mx[i]) mx[i] = c[i];
            }
            float r = c[0], g = nch >= 2 ? c[1] : c[0], b = nch >= 3 ? c[2] : nch == 1 ? c[0] : 0;
            out[x * 3 + 0] = (BYTE)(b <= 0 ? 0 : b >= 1 ? 255 : b * 255 + 0.5f);
            out[x * 3 + 1] = (BYTE)(g <= 0 ? 0 : g >= 1 ? 255 : g * 255 + 0.5f);
            out[x * 3 + 2] = (BYTE)(r <= 0 ? 0 : r >= 1 ? 255 : r * 255 + 0.5f);
        }
    }
    ds_vcall<HRESULT>(sys, 14);
    ds_vcall<ULONG>(sys, 2);
    if (tmp) ds_vcall<ULONG>(tmp, 2);
    double np = (double)w * h;
    static const char* ch = "RGBA";
    for (int i = 0; i < nch; ++i)
        fprintf(g_log, " %c mean %.4g min %.4g max %.4g", ch[i], sum[i] / np, mn[i], mx[i]);
    if (nonfinite) fprintf(g_log, " nonfinite %lu", nonfinite);
    char path[MAX_PATH];
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\%02d_%ux%u_%s.bmp", g_dumpDir, n, w, h, ds_fmt(d.Format));
    if (FILE* f = fopen(path, "wb"))
    {
        fwrite(bmp.data(), 1, bmp.size(), f);
        fclose(f);
        fprintf(g_log, " -> %s\n", path);
    }
    else fprintf(g_log, " (cannot write %s)\n", path);
    fflush(g_log);
}
static void* ds_cur_rt(void* dev)
{
    void* s = nullptr;
    if (ds_vcall<HRESULT>(dev, 38, (DWORD)0, &s) < 0) return nullptr;
    if (s) ds_vcall<ULONG>(s, 2);   // the device keeps its own reference
    return s;
}
static void ds_seg_begin(void* dev)
{
    g_dumpRT = ds_cur_rt(dev);
    ++g_dumpSeg;
    g_dumpSegDraws = g_dumpDetail = 0;
    void* ds = nullptr;
    if (ds_vcall<HRESULT>(dev, 40, &ds) >= 0 && ds) ds_vcall<ULONG>(ds, 2);
    fprintf(g_log, "[dump] segment %d:", g_dumpSeg);
    ds_log_surf("RT0", g_dumpRT);
    ds_log_surf("DS", ds);
    fputc('\n', g_log);
}
static void ds_seg_end(void* dev, const char* why)
{
    fprintf(g_log, "[dump] segment %d ends (%s): %d draws\n", g_dumpSeg, why, g_dumpSegDraws);
    if (g_dumpRT) ds_dump_surface(dev, g_dumpRT, why);
}
static DWORD ds_rs(void* dev, DWORD s)
{
    DWORD v;
    if (ds_vcall<HRESULT>(dev, 58, s, &v) >= 0) return v;
    return g_rsShadow[s & 255];
}
static void ds_dump_draw(void* dev, const char* kind, UINT prims, void* ret)
{
    ++g_dumpSegDraws;
    if (g_dumpDetail >= 4) return;
    ++g_dumpDetail;
    DsViewport vp = g_vpShadow;
    ds_vcall<HRESULT>(dev, 48, &vp);
    RECT sc = g_scShadow;
    ds_vcall<HRESULT>(dev, 76, &sc);
    fprintf(g_log, "[dump]   %s #%d prims %u: Z %lu func %lu write %lu | blend %lu src %lu dst %lu op %lu sepA %lu | atest %lu "
                   "ref %lu func %lu | cwrite %lx cull %lu stencil %lu srgb %lu | scissor %lu (%ld,%ld)-(%ld,%ld) | vp %lu,%lu %lux%lu z %g..%g"
                   " | VS %p PS %p tex %p %p %p %p",
            kind, g_dumpSegDraws, prims, ds_rs(dev, 7), ds_rs(dev, 23), ds_rs(dev, 14), ds_rs(dev, 27), ds_rs(dev, 19),
            ds_rs(dev, 20), ds_rs(dev, 171), ds_rs(dev, 206), ds_rs(dev, 15), ds_rs(dev, 24), ds_rs(dev, 25),
            ds_rs(dev, 168), ds_rs(dev, 22), ds_rs(dev, 52), ds_rs(dev, 194), ds_rs(dev, 174), sc.left, sc.top, sc.right,
            sc.bottom, vp.X, vp.Y, vp.Width, vp.Height, vp.MinZ, vp.MaxZ, g_dsVS, g_dsPS, g_dsTex[0], g_dsTex[1],
            g_dsTex[2], g_dsTex[3]);
    for (int i = 0; i < 4; ++i)   // a texture's level-0 surface: matches an RT0 / StretchRect destination above
    {
        void* t = g_dsTex[i];
        void* s = nullptr;
        if (t && ds_vcall<DWORD>(t, 10) == 3 /* D3DRTYPE_TEXTURE */ && ds_vcall<HRESULT>(t, 18, (UINT)0, &s) >= 0 && s)
        {
            fprintf(g_log, " [t%d surf %p]", i, s);
            ds_vcall<ULONG>(s, 2);
        }
    }
    ds_where((DWORD)ret);
    fputc('\n', g_log);
    for (int r = 0; r < 32; ++r)
        if (g_psCaller[r])
        {
            fprintf(g_log, "[dump]     PS c%d = %g %g %g %g", r, g_psC[r][0], g_psC[r][1], g_psC[r][2], g_psC[r][3]);
            ds_where((DWORD)g_psCaller[r]);
            fputc('\n', g_log);
        }
}
// Image space state behind the final HDR / cinematic pass: the ImageSpaceManager singleton ([0x011f91ac]) as floats
// (blended parameters +0x14, multiplicative/additive modifier sets +0xb4/+0x14c, tint/fade accumulators +0x214..+0x25c)
// and the active image space modifier instances (TES [0x011dea10] +0x6c, a BSSimpleList of instance pointers).
// The intro quests that own the white/black image space modifiers (VCG00 stage 0 applies WhiteISFX and
// MQ08AllBlackedOutISFX, its quest script timer walks 90 -> 95 -> 100, stage 100 removes the blackout and starts
// VCG01, whose stage 0 removes WhiteISFX): flags, delay, current stage and the quest script variables.
static void ds_dump_quests()
{
    typedef void*(__cdecl * LookupFn)(DWORD);
    static LookupFn lookup = (LookupFn)relinked(0x004839c0);
    if (!lookup) return;
    static const DWORD ids[] = {0x102037, 0x104c1c};
    for (DWORD id : ids)
    {
        BYTE* q = (BYTE*)lookup(id);
        if (!q || IsBadReadPtr(q, 0x70)) { fprintf(g_log, "[quest] %06lx not found\n", id); continue; }
        fprintf(g_log, "[quest] %06lx %p type %02x flags %02x delay %g stage %u script %08lx evlist %08lx |",
                id, q, q[4], q[0x3c], *(float*)(q + 0x40), q[0x60], *(DWORD*)(q + 0x1c), *(DWORD*)(q + 0x5c));
        DWORD ev = *(DWORD*)(q + 0x5c);
        if (ev && !IsBadReadPtr((void*)ev, 0x14))
        {
            DWORD node = ((DWORD*)ev)[3];   // tList<ScriptVar>: {var, next}; ScriptVar {id, next, double}
            for (int k = 0; node && k < 12 && !IsBadReadPtr((void*)node, 8); ++k, node = ((DWORD*)node)[1])
            {
                DWORD v = ((DWORD*)node)[0];
                if (v && !IsBadReadPtr((void*)v, 16)) fprintf(g_log, " v%lu=%g", ((DWORD*)v)[0], *(double*)(v + 8));
            }
        }
        fputc('\n', g_log);
    }
}
static void ds_dump_ism(const char* when)
{
    ds_dump_quests();
    fprintf(g_log, "[dump] image space state (%s): modifier time %g\n", when, *(volatile float*)0x011c96fc);
    DWORD ism = *(DWORD*)0x011f91ac;
    if (ism && !IsBadReadPtr((void*)ism, 0x280))
        for (DWORD o = 0; o < 0x280; o += 0x20)
        {
            fprintf(g_log, "[dump]   ism+%03lx:", o);
            for (DWORD k = 0; k < 0x20; k += 4)
            {
                DWORD v = *(DWORD*)(ism + o + k);
                float f;
                memcpy(&f, &v, 4);
                if (v && (v >> 23 & 0xff) > 0x60 && (v >> 23 & 0xff) < 0x98) fprintf(g_log, " %g", f);
                else fprintf(g_log, " %lx", v);
            }
            fputc('\n', g_log);
        }
    DWORD tes = *(DWORD*)0x011dea10;
    if (!tes || IsBadReadPtr((void*)(tes + 0x6c), 8)) return;
    DWORD node = tes + 0x6c;
    for (int k = 0; node && k < 32 && !IsBadReadPtr((void*)node, 8); ++k, node = ((DWORD*)node)[1])
    {
        DWORD inst = ((DWORD*)node)[0];
        if (!inst || IsBadReadPtr((void*)inst, 0x40)) { fprintf(g_log, "[dump]   imod #%d %08lx\n", k, inst); continue; }
        fprintf(g_log, "[dump]   imod #%d %08lx:", k, inst);
        for (int w = 0; w < 16; ++w)
        {
            DWORD v = ((DWORD*)inst)[w];
            float f;
            memcpy(&f, &v, 4);
            if (v && (v >> 23 & 0xff) > 0x60 && (v >> 23 & 0xff) < 0x98) fprintf(g_log, " %g", f);
            else fprintf(g_log, " %lx", v);
        }
        for (int w = 1; w < 16; ++w)   // form pointers in the instance: their form ids
        {
            DWORD p = ((DWORD*)inst)[w];
            if (p > 0x10000 && !IsBadReadPtr((void*)p, 0x10) && *(DWORD*)p >= 0x01000000 && *(DWORD*)p < 0x01100000)
                fprintf(g_log, " [w%d vt %08lx id %08lx type %02x]", w, *(DWORD*)p, ((DWORD*)p)[3], ((BYTE*)p)[4]);
        }
        fputc('\n', g_log);
    }
    fflush(g_log);
}
static HRESULT WINAPI ds_SetRT(void* d, DWORD i, void* s)
{
    InterlockedIncrement(&g_dsRTsets);
    if (!s && i == 0) InterlockedIncrement(&g_dsRTnull);
    if (g_dumpState != 2) return g_realSetRT(d, i, s);
    fprintf(g_log, "[dump] SetRenderTarget(%lu)", i);
    ds_log_surf("->", s);
    ds_where((DWORD)_ReturnAddress());
    fputc('\n', g_log);
    if (i == 0 && s != g_dumpRT) ds_seg_end(d, "target switch");
    HRESULT hr = g_realSetRT(d, i, s);
    if (i == 0 && s != g_dumpRT) ds_seg_begin(d);
    return hr;
}
static HRESULT WINAPI ds_SetDS(void* d, void* s)
{
    if (g_dumpState == 2)
    {
        fprintf(g_log, "[dump] SetDepthStencilSurface");
        ds_log_surf("->", s);
        ds_where((DWORD)_ReturnAddress());
        fputc('\n', g_log);
    }
    return g_realSetDS(d, s);
}
static HRESULT WINAPI ds_Stretch(void* d, void* src, const RECT* sr, void* dst, const RECT* dr, DWORD filt)
{
    if (g_dumpState != 2) return g_realStretch(d, src, sr, dst, dr, filt);
    fprintf(g_log, "[dump] StretchRect");
    ds_log_surf("src", src);
    if (sr) fprintf(g_log, " (%ld,%ld)-(%ld,%ld)", sr->left, sr->top, sr->right, sr->bottom);
    ds_log_surf("dst", dst);
    if (dr) fprintf(g_log, " (%ld,%ld)-(%ld,%ld)", dr->left, dr->top, dr->right, dr->bottom);
    fprintf(g_log, " filter %lu", filt);
    ds_where((DWORD)_ReturnAddress());
    HRESULT hr = g_realStretch(d, src, sr, dst, dr, filt);
    fprintf(g_log, " -> %08lx\n", hr);
    DsDesc dd;
    if (hr >= 0 && ds_desc(dst, &dd) && (dd.Usage & 1)) ds_dump_surface(d, dst, "StretchRect dst");
    return hr;
}
static HRESULT WINAPI ds_SetRS(void* d, DWORD s, DWORD v) { g_rsShadow[s & 255] = v; return g_realSetRS(d, s, v); }
static HRESULT WINAPI ds_SetVP(void* d, const DsViewport* v) { if (v) g_vpShadow = *v; return g_realSetVP(d, v); }
static HRESULT WINAPI ds_SetSc(void* d, const RECT* r) { if (r) g_scShadow = *r; return g_realSetSc(d, r); }
static HRESULT WINAPI ds_EndScene(void* d)
{
    if (g_dumpState == 2) fprintf(g_log, "[dump] EndScene (segment %d, %d draws so far)\n", g_dumpSeg, g_dumpSegDraws);
    return g_realEndScene(d);
}
static volatile LONG g_dsConstNaN, g_dsConstSets, g_dsConstZeroWVP;
static float g_dsC0[16];
static volatile LONG g_dsC0Taken;
static volatile LONG g_dsPSNaN, g_dsNaNLogged;
// FNV_FPU_TRAP=1 (with FNV_D3D_STATS=1): x87 invalid-operation exceptions are unmasked on the render thread while
// game code runs (re-armed when each hooked D3D call returns, masked again while D3D itself runs). A stack fault (a
// pop of an empty register: a caller storing an st0 result its callee never pushed, or a push onto a full stack)
// then traps; the handler logs the faulting instruction (FPU last-instruction pointer) once per site with the
// relinked code pointers on the stack, masks the exception and lets the game continue.
static int g_fpuTrap = -1;
static DWORD g_fpuTrapSeen[256];
static volatile LONG g_fpuTrapN, g_fpuTrapLock;
static void fpu_trap_where(const char* what, DWORD a)
{
    auto it = std::upper_bound(g_byPtr.begin(), g_byPtr.end(), (void*)a,
                               [](void* p, const AddrEntry& e) { return p < e.relinked; });
    if (it != g_byPtr.begin())
    {
        --it;
        fprintf(g_log, " %s %08lx (fnv_f_%08lx+0x%lx)", what, a, it->original, a - (DWORD)it->relinked);
    }
    else
        fprintf(g_log, " %s %08lx", what, a);
}
static LONG CALLBACK fpu_trap_handler(EXCEPTION_POINTERS* ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != STATUS_FLOAT_STACK_CHECK && code != STATUS_FLOAT_INVALID_OPERATION)
        return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = ep->ContextRecord;
    BYTE* fx = (c->ContextFlags & CONTEXT_EXTENDED_REGISTERS) == CONTEXT_EXTENDED_REGISTERS ? c->ExtendedRegisters : nullptr;
    DWORD fip = fx ? *(DWORD*)(fx + 8) : c->FloatSave.ErrorOffset;
    DWORD sw = fx ? *(WORD*)(fx + 2) : c->FloatSave.StatusWord;
    DWORD op = fx ? *(WORD*)(fx + 6) : (c->FloatSave.ErrorSelector >> 16) & 0x7ff;
    while (InterlockedCompareExchange(&g_fpuTrapLock, 1, 0)) {}
    bool fresh = true;
    for (LONG i = 0; i < g_fpuTrapN && fresh; ++i)
        fresh = g_fpuTrapSeen[i] != fip;
    if (fresh && g_fpuTrapN < 256)
        g_fpuTrapSeen[g_fpuTrapN++] = fip;
    if (fresh && g_fpuTrapN <= 64)
    {
        fprintf(g_log, "[fputrap] %s (%s) thread %lu sw %04lx tags %04lx opcode %03lx:",
                code == STATUS_FLOAT_STACK_CHECK ? "x87 stack fault" : "x87 invalid operation",
                code == STATUS_FLOAT_STACK_CHECK ? ((sw & 0x200) ? "overflow" : "underflow") : "operand",
                GetCurrentThreadId(), sw, (DWORD)c->FloatSave.TagWord & 0xffff, op);
        fpu_trap_where("fip", fip);
        fpu_trap_where("trapped at", c->Eip);
        fputc('\n', g_log);
        DWORD* sp = (DWORD*)c->Esp;
        DWORD lo = g_byPtr.empty() ? 0 : (DWORD)g_byPtr.front().relinked;
        DWORD hi = g_byPtr.empty() ? 0 : (DWORD)g_byPtr.back().relinked + 0x10000;
        int shown = 0;
        fprintf(g_log, "[fputrap]   stack:");
        for (int i = 0; i < 512 && shown < 8 && !IsBadReadPtr(sp + i, 4); ++i)
            if (sp[i] >= lo && sp[i] < hi)
            {
                fpu_trap_where("", sp[i]);
                ++shown;
            }
        fputc('\n', g_log);
        fflush(g_log);
    }
    InterlockedExchange(&g_fpuTrapLock, 0);
    // mask IM and drop the pending exception: the trapping instruction then runs normally (the faulting one was
    // suppressed, as an unmasked invalid operation leaves its destination and TOP unchanged)
    c->FloatSave.ControlWord |= 1;
    c->FloatSave.StatusWord &= ~0x80ffu;
    if (fx)
    {
        *(WORD*)fx |= 1;
        *(WORD*)(fx + 2) &= ~0x80ff;
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}
static bool fpu_trap_on()
{
    if (g_fpuTrap < 0)
    {
        g_fpuTrap = getenv("FNV_FPU_TRAP") && *getenv("FNV_FPU_TRAP") == '1';
        if (g_fpuTrap)
        {
            AddVectoredExceptionHandler(1, fpu_trap_handler);
            fprintf(g_log, "[fputrap] FNV_FPU_TRAP=1: x87 invalid-operation exceptions unmasked on the render thread\n");
        }
    }
    return g_fpuTrap > 0;
}
static void fpu_mask()     // entering a hooked D3D call (fldcw raises any pending unmasked exception first)
{
    if (!fpu_trap_on()) return;
    unsigned short cw;
    __asm { fnstcw cw }
    if (!(cw & 1)) { cw |= 1; __asm { fldcw cw } }
}
static void fpu_unmask()   // back to game code
{
    if (!fpu_trap_on()) return;
    unsigned short cw;
    __asm { fnstcw cw }
    if (cw & 1) { cw &= ~1; __asm { fnclex } __asm { fldcw cw } }
}
static void ds_nan_note(const char* kind, UINT reg, const float* v, UINT n)
{
    if (InterlockedIncrement(&g_dsNaNLogged) > 12) return;
    unsigned short env[14];   // fnstenv: control, status, tag words first (x87 stack depth at the bad upload)
    __asm { fnstenv env }
    __asm { fldcw env }
    char buf[400];
    int k = _snprintf_s(buf, sizeof(buf), _TRUNCATE, "[d3d]   NaN/inf %s constants c%u (x%u), caller %p, fpu status %04x top %u tags %04x:",
                        kind, reg, n, _ReturnAddress(), env[2], (env[2] >> 11) & 7, env[4]);
    for (UINT i = 0; i < n * 4 && i < 16 && k > 0 && k < 360; ++i) k += _snprintf_s(buf + k, sizeof(buf) - k, _TRUNCATE, " %g", v[i]);
    fprintf(g_log, "%s\n", buf);
}
static bool ds_bad(const float* v, UINT n)
{
    for (UINT i = 0; i < n * 4; ++i)
        if (v[i] != v[i] || v[i] > 1e30f || v[i] < -1e30f) return true;
    return false;
}
static D3DSetConstFn g_realSetPSConst;
static HRESULT WINAPI ds_SetPSConst(void* d, UINT reg, const float* v, UINT n)
{
    fpu_mask();
    if (ds_bad(v, n)) { InterlockedIncrement(&g_dsPSNaN); ds_nan_note("PS", reg, v, n); }
    for (UINT i = 0; i < n && reg + i < 32; ++i)
    {
        memcpy(g_psC[reg + i], v + 4 * i, 16);
        g_psCaller[reg + i] = _ReturnAddress();
    }
    HRESULT hr = g_realSetPSConst(d, reg, v, n);
    fpu_unmask();
    return hr;
}
static HRESULT WINAPI ds_SetVSConst(void* d, UINT reg, const float* v, UINT n)
{
    fpu_mask();
    InterlockedIncrement(&g_dsConstSets);
    if (ds_bad(v, n)) { InterlockedIncrement(&g_dsConstNaN); ds_nan_note("VS", reg, v, n); }
    if (reg == 0 && n >= 4)
    {
        bool zero = true;
        for (int i = 0; i < 16; ++i) zero = zero && v[i] == 0.0f;
        if (zero) InterlockedIncrement(&g_dsConstZeroWVP);
        if (!InterlockedExchange(&g_dsC0Taken, 1)) memcpy(g_dsC0, v, sizeof(g_dsC0));
    }
    HRESULT hr = g_realSetVSConst(d, reg, v, n);
    fpu_unmask();
    return hr;
}
static volatile LONG g_dsDraws, g_dsPrims, g_dsNoVS, g_dsNoPS, g_dsScenes, g_dsVSmade, g_dsPSmade, g_dsVSfail, g_dsPSfail,
    g_dsClears, g_dsFrames;
static void ds_draw(void* d, const char* kind, UINT prims, void* ret)
{
    ++g_frameDraws;
    if (g_dumpState == 2) ds_dump_draw(d, kind, prims, ret);
    InterlockedIncrement(&g_dsDraws);
    InterlockedExchangeAdd(&g_dsPrims, (LONG)prims);
    if (!g_dsVS) InterlockedIncrement(&g_dsNoVS);
    if (!g_dsPS) InterlockedIncrement(&g_dsNoPS);
    if (!g_dsTex0) InterlockedIncrement(&g_dsNoTex0);
}
static HRESULT WINAPI ds_Present(void* d, const RECT* a, const RECT* b, HWND w, const void* r)
{
    fpu_mask();
    LONG f = InterlockedIncrement(&g_dsFrames);
    if (g_dumpState == 2)
    {
        fprintf(g_log, "[dump] Present (frame %ld, %ld draws)\n", f, g_frameDraws);
        ds_seg_end(d, "Present");
        ds_dump_ism("Present");
        fprintf(g_log, "[dump] frame %ld done: %d segments, %d readbacks\n", f, g_dumpSeg, g_dumpFileN);
        fflush(g_log);
        g_dumpState = 3;
    }
    else if (g_dumpState == 1)
    {
        bool go = false;
        if (g_dumpFrame >= 0) go = f + 1 == g_dumpFrame;
        else if (g_dumpTagHit && g_frameDraws >= 200) go = ++g_dumpHeavy >= g_dumpSkip;
        if (go)
        {
            g_dumpState = 2;
            fprintf(g_log, "[dump] FNV_D3D_DUMP: dumping frame %ld (previous frame had %ld draws) to %s\n", f + 1, g_frameDraws, g_dumpDir);
            ds_dump_ism("frame start");
            ds_seg_begin(d);
        }
    }
    g_frameDraws = 0;
    if (f % 120 == 0)
    {
        unsigned short env[14];   // x87 stack at Present: compiled callers keep it empty across calls
        __asm { fnstenv env }
        __asm { fldenv env }
        fprintf(g_log, "[d3d] x87 at Present: status %04x top %u tags %04x; image space modifier time (0x011c96fc) %g\n",
                env[2], (env[2] >> 11) & 7, env[4], *(volatile float*)0x011c96fc);
        if (g_dumpState) ds_dump_quests();
        fprintf(g_log, "[d3d] frames %ld: last 120 -> scenes %ld draws %ld prims %ld (no VS %ld, no PS %ld) clears %ld; "
                       "shaders made VS %ld (fail %ld) PS %ld (fail %ld)\n",
                f, InterlockedExchange(&g_dsScenes, 0), InterlockedExchange(&g_dsDraws, 0), InterlockedExchange(&g_dsPrims, 0),
                InterlockedExchange(&g_dsNoVS, 0), InterlockedExchange(&g_dsNoPS, 0), InterlockedExchange(&g_dsClears, 0),
                g_dsVSmade, g_dsVSfail, g_dsPSmade, g_dsPSfail);
        fprintf(g_log, "[d3d]   VS constants: %ld sets, %ld with NaN/inf, %ld all-zero c0-c3; sample c0-c3 %g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g\n",
                InterlockedExchange(&g_dsConstSets, 0), InterlockedExchange(&g_dsConstNaN, 0),
                InterlockedExchange(&g_dsConstZeroWVP, 0), g_dsC0[0], g_dsC0[1], g_dsC0[2], g_dsC0[3], g_dsC0[4], g_dsC0[5],
                g_dsC0[6], g_dsC0[7], g_dsC0[8], g_dsC0[9], g_dsC0[10], g_dsC0[11], g_dsC0[12], g_dsC0[13], g_dsC0[14],
                g_dsC0[15]);
        InterlockedExchange(&g_dsC0Taken, 0);
        fprintf(g_log, "[d3d]   draws with no texture on stage 0: %ld; SetRenderTarget calls %ld (null RT0 %ld); PS consts with NaN/inf %ld\n",
                InterlockedExchange(&g_dsNoTex0, 0), InterlockedExchange(&g_dsRTsets, 0), InterlockedExchange(&g_dsRTnull, 0),
                InterlockedExchange(&g_dsPSNaN, 0));
        fflush(g_log);
    }
    HRESULT hr = g_realPresent(d, a, b, w, r);
    fpu_unmask();
    return hr;
}
static HRESULT WINAPI ds_BeginScene(void* d)
{
    InterlockedIncrement(&g_dsScenes);
    if (g_dumpState == 2)
    {
        fprintf(g_log, "[dump] BeginScene");
        ds_where((DWORD)_ReturnAddress());
        fputc('\n', g_log);
    }
    return g_realBeginScene(d);
}
static HRESULT WINAPI ds_Draw(void* d, DWORD t, DWORD s, DWORD n) { ds_draw(d, "Draw", n, _ReturnAddress()); return g_realDraw(d, t, s, n); }
static HRESULT WINAPI ds_DrawIdx(void* d, DWORD t, INT b, UINT mi, UINT nv, UINT si, UINT n)
{
    ds_draw(d, "DrawIdx", n, _ReturnAddress());
    return g_realDrawIdx(d, t, b, mi, nv, si, n);
}
static HRESULT WINAPI ds_DrawUP(void* d, DWORD t, UINT n, const void* v, UINT s) { ds_draw(d, "DrawUP", n, _ReturnAddress()); return g_realDrawUP(d, t, n, v, s); }
static HRESULT WINAPI ds_DrawIdxUP(void* d, DWORD t, UINT mi, UINT nv, UINT n, const void* i, DWORD f, const void* v, UINT s)
{
    ds_draw(d, "DrawIdxUP", n, _ReturnAddress());
    return g_realDrawIdxUP(d, t, mi, nv, n, i, f, v, s);
}
static HRESULT WINAPI ds_SetVS(void* d, void* s) { g_dsVS = s; return g_realSetVS(d, s); }
static HRESULT WINAPI ds_SetPS(void* d, void* s) { g_dsPS = s; return g_realSetPS(d, s); }
static HRESULT WINAPI ds_CreateVS(void* d, const DWORD* f, void** o)
{
    HRESULT hr = g_realCreateVS(d, f, o);
    InterlockedIncrement(hr < 0 ? &g_dsVSfail : &g_dsVSmade);
    return hr;
}
static HRESULT WINAPI ds_CreatePS(void* d, const DWORD* f, void** o)
{
    HRESULT hr = g_realCreatePS(d, f, o);
    InterlockedIncrement(hr < 0 ? &g_dsPSfail : &g_dsPSmade);
    return hr;
}
static HRESULT WINAPI ds_Clear(void* d, DWORD n, const void* r, DWORD fl, DWORD c, float z, DWORD s)
{
    InterlockedIncrement(&g_dsClears);
    if (g_dumpState == 2)
    {
        fprintf(g_log, "[dump] Clear(rects %lu, flags%s%s%s, color %08lx, z %g, stencil %lu) in segment %d after %d draws",
                n, (fl & 1) ? " TARGET" : "", (fl & 2) ? " ZBUFFER" : "", (fl & 4) ? " STENCIL" : "", c, z, s, g_dumpSeg,
                g_dumpSegDraws);
        if (n && r) { const RECT* q = (const RECT*)r; fprintf(g_log, " rect0 (%ld,%ld)-(%ld,%ld)", q->left, q->top, q->right, q->bottom); }
        ds_where((DWORD)_ReturnAddress());
        fputc('\n', g_log);
    }
    return g_realClear(d, n, r, fl, c, z, s);
}
template <class F> static void ds_patch(void** vt, int slot, F& real, void* fn)
{
    real = (F)vt[slot];
    DWORD old;
    VirtualProtect(&vt[slot], 4, PAGE_READWRITE, &old);
    vt[slot] = fn;
    VirtualProtect(&vt[slot], 4, old, &old);
}
static HRESULT WINAPI ds_CreateDevice(void* d3d, UINT ad, DWORD type, HWND w, DWORD fl, void* pp, void** out)
{
    HRESULT hr = g_realCreateDevice(d3d, ad, type, w, fl, pp, out);
    if (hr >= 0 && out && *out && !g_realPresent)
    {
        void** vt = *(void***)*out;
        ds_patch(vt, 17, g_realPresent, (void*)&ds_Present);
        ds_patch(vt, 41, g_realBeginScene, (void*)&ds_BeginScene);
        ds_patch(vt, 43, g_realClear, (void*)&ds_Clear);
        ds_patch(vt, 81, g_realDraw, (void*)&ds_Draw);
        ds_patch(vt, 82, g_realDrawIdx, (void*)&ds_DrawIdx);
        ds_patch(vt, 83, g_realDrawUP, (void*)&ds_DrawUP);
        ds_patch(vt, 84, g_realDrawIdxUP, (void*)&ds_DrawIdxUP);
        ds_patch(vt, 91, g_realCreateVS, (void*)&ds_CreateVS);
        ds_patch(vt, 92, g_realSetVS, (void*)&ds_SetVS);
        ds_patch(vt, 94, g_realSetVSConst, (void*)&ds_SetVSConst);
        ds_patch(vt, 65, g_realSetTexture, (void*)&ds_SetTexture);
        ds_patch(vt, 109, g_realSetPSConst, (void*)&ds_SetPSConst);
        ds_patch(vt, 37, g_realSetRT, (void*)&ds_SetRT);
        ds_patch(vt, 106, g_realCreatePS, (void*)&ds_CreatePS);
        ds_patch(vt, 107, g_realSetPS, (void*)&ds_SetPS);
        ds_patch(vt, 34, g_realStretch, (void*)&ds_Stretch);
        ds_patch(vt, 39, g_realSetDS, (void*)&ds_SetDS);
        ds_patch(vt, 42, g_realEndScene, (void*)&ds_EndScene);
        ds_patch(vt, 47, g_realSetVP, (void*)&ds_SetVP);
        ds_patch(vt, 57, g_realSetRS, (void*)&ds_SetRS);
        ds_patch(vt, 75, g_realSetSc, (void*)&ds_SetSc);
        ds_rs_defaults();
        if (const char* e = getenv("FNV_D3D_DUMP"))
        {
            if (*e >= '0' && *e <= '9') g_dumpFrame = atol(e);
            else g_dumpTag = e;
            if (const char* k = getenv("FNV_D3D_DUMP_SKIP")) g_dumpSkip = atol(k);
            if (const char* dir = getenv("FNV_D3D_DUMP_DIR")) _snprintf_s(g_dumpDir, sizeof(g_dumpDir), _TRUNCATE, "%s", dir);
            else
            {
                GetModuleFileNameA(nullptr, g_dumpDir, MAX_PATH);   // out/link/FalloutNV_relinked.exe -> out/d3d_dump
                if (char* sl = strrchr(g_dumpDir, '\\')) *sl = 0;
                if (char* sl = strrchr(g_dumpDir, '\\')) *sl = 0;
                strcat_s(g_dumpDir, "\\d3d_dump");
            }
            CreateDirectoryA(g_dumpDir, nullptr);
            g_dumpState = 1;
            fprintf(g_log, "[d3d] FNV_D3D_DUMP=%s armed (dir %s)\n", e, g_dumpDir);
        }
        fprintf(g_log, "[d3d] FNV_D3D_STATS: device %p wrapped\n", *out);
        fflush(g_log);
    }
    return hr;
}
static void* WINAPI wrap_Direct3DCreate9(UINT sdk);
// Gamebryo's NiDX9Renderer loads d3d9.dll and looks up Direct3DCreate9 itself
#include "d3d9_model.h"
FARPROC WINAPI wrap_GetProcAddress(HMODULE m, LPCSTR name)
{
    FARPROC f = GetProcAddress(m, name);
    if (f && (ULONG_PTR)name > 0xffff && !strcmp(name, "Direct3DCreate9") && getenv("FNV_D3D9_MODEL") &&
        *getenv("FNV_D3D9_MODEL") == '1')
    {   // the game gets the Direct3D 9 model (d3d9_model.h), which forwards to the real runtime
        g_modelRealCreate9 = (D3DCreate9Real)f;
        return (FARPROC)&model_Direct3DCreate9;
    }
    if (f && (ULONG_PTR)name > 0xffff && !strcmp(name, "Direct3DCreate9") && getenv("FNV_D3D_STATS"))
    {
        g_realD3DCreate9 = (D3DCreate9Fn)f;
        return (FARPROC)&wrap_Direct3DCreate9;
    }
    return f;
}
static void* WINAPI wrap_Direct3DCreate9(UINT sdk)
{
    void* d3d = g_realD3DCreate9(sdk);
    if (d3d && !g_realCreateDevice)
        ds_patch(*(void***)d3d, 16, g_realCreateDevice, (void*)&ds_CreateDevice);
    return d3d;
}

// ---- FNV_THREAD_HOST=1: game threads run one at a time (idea from nv-rs's thread host) ----
// Every thread the game creates and the main thread hold a baton while running game code. The baton is a fair
// ticket lock (arrival order). Switch points are the blocking calls: waits with a timeout, Sleep (also Sleep(0),
// the game's spin-lock yield), a contended EnterCriticalSection, thread exit. The real kernel objects keep their
// meaning: a thread gives the baton back, blocks in the real call, then queues for the baton again. Each game
// thread keeps its OS thread, so TLS (lift arenas, Havok thread memory, the original's __tls slots) is unchanged.
bool g_th = false;
static volatile LONG g_thNext = 0, g_thServing = 0, g_thThreads = 0, g_thSwitches = 0;
static __declspec(thread) int t_thGame = 0, t_thHold = 0;
static volatile ULONGLONG g_thSince = 0;   // when the current holder took the baton
static volatile long* g_thYieldFlag = nullptr;   // fnv_game.dll's fnv_lift_yield_flag
static void th_acquire()
{
    LONG my = InterlockedIncrement(&g_thNext) - 1;
    for (;;)
    {
        LONG cur = g_thServing;
        if (cur == my) break;
        WaitOnAddress(&g_thServing, &cur, sizeof(cur), INFINITE);
    }
    t_thHold = 1;
    g_thSince = GetTickCount64();
}
static void th_release()
{
    t_thHold = 0;
    InterlockedIncrement(&g_thSwitches);
    InterlockedIncrement(&g_thServing);
    WakeByAddressAll((PVOID)&g_thServing);
}
// release around a blocking call, only on a game thread that holds the baton
struct ThBlock
{
    bool held;
    ThBlock() : held(g_th && t_thGame && t_thHold) { if (held) th_release(); }
    ~ThBlock() { if (held) th_acquire(); }
};
// time slice: lifted code calls this at its next call once the watchdog set the flag (fnv_lift.h)
static void __cdecl th_yield()
{
    if (g_thYieldFlag) *g_thYieldFlag = 0;
    if (!(g_th && t_thGame && t_thHold) || g_thNext - g_thServing <= 1) return;
    th_release();
    th_acquire();
}
static DWORD WINAPI th_watchdog(LPVOID)
{
    for (;;)
    {
        Sleep(2);
        if (g_thYieldFlag && g_thNext - g_thServing > 1 && GetTickCount64() - g_thSince > 20)
            *g_thYieldFlag = 1;
    }
}
struct ThStart { LPTHREAD_START_ROUTINE fn; LPVOID arg; };
static DWORD WINAPI th_trampoline(LPVOID p)
{
    ThStart s = *(ThStart*)p;
    HeapFree(GetProcessHeap(), 0, p);
    t_thGame = 1;
    InterlockedIncrement(&g_thThreads);
    th_acquire();
    DWORD r = s.fn(s.arg);
    if (t_thHold) th_release();
    return r;
}
HANDLE WINAPI wrap_CreateThread(LPSECURITY_ATTRIBUTES sa, SIZE_T stack, LPTHREAD_START_ROUTINE fn, LPVOID arg, DWORD flags,
                                LPDWORD id)
{
    if (!g_th) return CreateThread(sa, stack, fn, arg, flags, id);
    ThStart* s = (ThStart*)HeapAlloc(GetProcessHeap(), 0, sizeof(ThStart));
    s->fn = fn;
    s->arg = arg;
    return CreateThread(sa, stack, th_trampoline, s, flags, id);
}
VOID WINAPI wrap_ExitThread(DWORD code)
{
    if (g_th && t_thGame && t_thHold) th_release();
    ExitThread(code);
}
VOID WINAPI wrap_Sleep(DWORD ms) { ThBlock b; Sleep(ms); }
DWORD WINAPI wrap_WaitForSingleObject(HANDLE h, DWORD ms)
{
    if (!ms) return WaitForSingleObject(h, 0);
    ThBlock b;
    return WaitForSingleObject(h, ms);
}
DWORD WINAPI wrap_WaitForMultipleObjects(DWORD n, const HANDLE* hs, BOOL all, DWORD ms)
{
    if (!ms) return WaitForMultipleObjects(n, hs, all, 0);
    ThBlock b;
    return WaitForMultipleObjects(n, hs, all, ms);
}
VOID WINAPI wrap_EnterCriticalSection(LPCRITICAL_SECTION cs)
{
    if (!(g_th && t_thGame && t_thHold)) { EnterCriticalSection(cs); return; }
    if (TryEnterCriticalSection(cs)) return;
    ThBlock b;   // owned by another game thread: let it run until it leaves the section
    EnterCriticalSection(cs);
}

VOID WINAPI wrap_ExitProcess(UINT code) { log_exit_stack("ExitProcess", code); ExitProcess(code); }
BOOL WINAPI wrap_TerminateProcess(HANDLE h, UINT code) { if (h == GetCurrentProcess()) log_exit_stack("TerminateProcess", code); return TerminateProcess(h, code); }

DWORD g_mainThreadId = 0;
LONG WINAPI on_crash(EXCEPTION_POINTERS* ep)
{
    if (const char* dc = getenv("FNV_DUMP_CRASH"))   // "hexaddr:words[,hexaddr:words...]"
    {
        for (const char* q = dc; q && *q;)
        {
            // "*hexaddr+hexoff:words" dereferences hexaddr first (follow a pointer that changes between runs)
            const char* q2 = q;
            int derefs = 0;
            while (*q2 == '*') { ++derefs; ++q2; }
            char* endp = nullptr;
            DWORD addr = (DWORD)strtoul(q2, &endp, 16);
            for (int d = 0; d < derefs && !IsBadReadPtr((void*)addr, 4); ++d)
            {
                addr = *(DWORD*)addr;
                if (endp && *endp == '+') addr += (DWORD)strtoul(endp + 1, &endp, 16);
            }
            const char* c = strchr(q, 0x3a);
            int words = c ? atoi(c + 1) : 8;
            for (int i = 0; i < words; ++i)
                if (!IsBadReadPtr((void*)(addr + i * 4), 4))
                    fprintf(g_log, "[crashdump] %08lx: %08lx\n", addr + i * 4, *(DWORD*)(addr + i * 4));
            q = strchr(q, 0x2c);
            if (q) ++q;
        }
    }
    g_tracing = 0;
    auto r = ep->ExceptionRecord;
    CONTEXT* ctx = ep->ContextRecord;
    fprintf(g_log, "[loader] crash on thread %lu (process main thread %lu)%s\n", GetCurrentThreadId(), g_mainThreadId,
            g_userClosed ? " during shutdown: the user closed the window" : "");
    fprintf(g_log, "[loader] exception 0x%08lx at %p (access %s 0x%08lx)\n", r->ExceptionCode, r->ExceptionAddress,
            r->NumberParameters >= 2 ? (r->ExceptionInformation[0] ? "write" : "read") : "-",
            r->NumberParameters >= 2 ? (DWORD)r->ExceptionInformation[1] : 0);
    fprintf(g_log, "[loader]   regs eax=%08lx ecx=%08lx edx=%08lx ebx=%08lx esi=%08lx edi=%08lx esp=%08lx ebp=%08lx\n",
            ctx->Eax, ctx->Ecx, ctx->Edx, ctx->Ebx, ctx->Esi, ctx->Edi, ctx->Esp, ctx->Ebp);
    describe(r->ExceptionAddress);
    DWORD* ebp = (DWORD*)ep->ContextRecord->Ebp;
    for (int i = 0; i < 12 && ebp && !IsBadReadPtr(ebp, 8); ++i, ebp = (DWORD*)ebp[0])
    {
        fprintf(g_log, "[loader]   frame %d ret %08lx", i, ebp[1]);
        describe((void*)ebp[1]);
        fputc('\n', g_log);
    }
    // optimised code keeps no frame pointer: also list stack words that point into relinked code
    DWORD* sp = (DWORD*)ctx->Esp;
    // Keep the raw top of the faulting stack; caller arguments can themselves be bad code pointers.
    for (int i = 0; i < 120 && !IsBadReadPtr(sp + i, 4); ++i)
        fprintf(g_log, "[loader]   raw-stack+%03x %08lx\n", i * 4, sp[i]);
    if (!g_byPtr.empty())
    {
        DWORD lo = (DWORD)g_byPtr.front().relinked, hi = (DWORD)g_byPtr.back().relinked + 0x10000;
        int shown = 0;
        for (int i = 0; i < 8192 && shown < 64 && !IsBadReadPtr(sp + i, 4); ++i)
            if (sp[i] >= lo && sp[i] < hi)
            {
                fprintf(g_log, "[loader]   stack+%03x %08lx", i * 4, sp[i]);
                describe((void*)sp[i]);
                ++shown;
            }
    }
    if (g_bpRingIdx > 0)
    {
        LONG n3 = g_bpRingIdx, from = n3 > 64 ? n3 - 64 : 0;
        fprintf(g_log, "[bpring] last %ld breakpoint hits before the crash:\n", (long)(n3 - from));
        for (LONG i = from; i < n3; ++i) fprintf(g_log, "[bpring] %s\n", g_bpRing[i & 63]);
    }
    g_tracing = 1; dump_trace();
    fflush(g_log);
    return EXCEPTION_EXECUTE_HANDLER;
}

// Raw original code addresses in the decomp source (e.g. `reinterpret_cast<void*>(0x00aff9d0)`) carry no symbol, so
// they are never rebound. With DEP, jumping there raises an execute fault; when the address is a relinked function,
// continue there instead (the return address is already on the stack). Each address is logged once, to be fixed at
// the source (the wasm build has no such safety net).
// Logged-once set of redirected targets. Lock-free and heap-free: this handler runs on whichever thread faults, and
// several AI worker threads start in original code at once after New Game; an unsynchronised std::vector here
// corrupted the process heap (c0000374, and a memcpy past a reallocated buffer in push_back).
volatile LONG g_redirected[1024];
volatile LONG g_redirectedN = 0;

LONG CALLBACK redirect_original_code(EXCEPTION_POINTERS* ep)
{
    auto r = ep->ExceptionRecord;
    if (r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || r->NumberParameters < 2 || r->ExceptionInformation[0] != 8)
        return EXCEPTION_CONTINUE_SEARCH;
    DWORD target = (DWORD)r->ExceptionAddress;
    void* f = relinked(target);
    if (!f)
        return EXCEPTION_CONTINUE_SEARCH;
    bool seen = false;
    for (LONG i = 0, n = g_redirectedN; i < n && i < 1024 && !seen; ++i)
        seen = (DWORD)g_redirected[i] == target;
    if (!seen)
    {
        LONG k = InterlockedIncrement(&g_redirectedN) - 1;
        if (k < 1024) InterlockedExchange(&g_redirected[k], (LONG)target);
        fprintf(g_log, "[loader] redirected a call to original code 0x%08lx -> relinked\n", target);
    }
    ep->ContextRecord->Eip = (DWORD)f;
    return EXCEPTION_CONTINUE_EXECUTION;
}

int fail(const char* what)
{
    fprintf(g_log, "[loader] error: %s (GetLastError %lu)\n", what, GetLastError());
    return 1;
}
} // namespace

// The relinked DLL has its own import table (compiled decomp code calls Win32 by name): apply the same wrappers there.
static void patch_dll_imports(HMODULE dll)
{
    struct W { const char* name; void* fn; };
    const W wraps[] = {{"CreateFileA", (void*)&wrap_CreateFileA}, {"GetFileAttributesA", (void*)&wrap_GetFileAttributesA},
                       {"MessageBoxA", (void*)&wrap_MessageBoxA}, {"OutputDebugStringA", (void*)&wrap_OutputDebugStringA},
                       {"ExitProcess", (void*)&wrap_ExitProcess}, {"TerminateProcess", (void*)&wrap_TerminateProcess},
                       {"SHGetFolderPathA", (void*)&wrap_SHGetFolderPathA}, {"SetUnhandledExceptionFilter", (void*)&ignore_set_filter},
                       {"VirtualAlloc", (void*)&wrap_VirtualAlloc}, {"GetProcAddress", (void*)&wrap_GetProcAddress},
                       {"CreateThread", (void*)&wrap_CreateThread}, {"ExitThread", (void*)&wrap_ExitThread},
                       {"Sleep", (void*)&wrap_Sleep}, {"WaitForSingleObject", (void*)&wrap_WaitForSingleObject},
                       {"WaitForMultipleObjects", (void*)&wrap_WaitForMultipleObjects},
                       {"EnterCriticalSection", (void*)&wrap_EnterCriticalSection}};
    auto base = (unsigned char*)dll;
    auto nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    int patched = 0;
    for (auto d = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); dir.VirtualAddress && d->Name; ++d)
    {
        auto names = (IMAGE_THUNK_DATA32*)(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        auto slots = (IMAGE_THUNK_DATA32*)(base + d->FirstThunk);
        for (int k = 0; names[k].u1.AddressOfData; ++k)
        {
            if (names[k].u1.Ordinal & IMAGE_ORDINAL_FLAG32) continue;
            const char* nm = (const char*)(base + names[k].u1.AddressOfData) + 2;
            if (!strcmp(nm, "Direct3DCreate9") && getenv("FNV_D3D_STATS"))   // the renderer is compiled code
            {
                if (!g_realD3DCreate9) g_realD3DCreate9 = (D3DCreate9Fn)slots[k].u1.Function;
                DWORD old;
                VirtualProtect(&slots[k], 4, PAGE_READWRITE, &old);
                slots[k].u1.Function = (DWORD)&wrap_Direct3DCreate9;
                VirtualProtect(&slots[k], 4, old, &old);
                ++patched;
            }
            for (const W& w : wraps)
                if (!strcmp(nm, w.name))
                {
                    DWORD old;
                    VirtualProtect(&slots[k], 4, PAGE_READWRITE, &old);
                    slots[k].u1.Function = (DWORD)w.fn;
                    VirtualProtect(&slots[k], 4, old, &old);
                    ++patched;
                }
        }
    }
    fprintf(g_log, "[loader] fnv_game.dll imports wrapped: %d\n", patched);
}

// FNV_SYSHEAP (default on; FNV_SYSHEAP=0 disables): route the game's MemoryManager (Allocate 0xaa3e40, Deallocate
// 0xaa4060, Reallocate 0xaa4150, Size 0xaa44c0) to 16-byte-aligned CRT heap blocks, like the community heap
// replacers do. The relinked pool allocator hands out overlapping small blocks; this takes it out of the picture.
static int g_heapZero = -1;   // FNV_HEAP_ZERO=1: zero-fill new blocks (A/B for reads of uninitialized heap)
static void* __fastcall sysheap_alloc(void*, void*, size_t size)
{
    void* p = _aligned_malloc(size ? size : 1, 16);
    if (g_heapZero < 0) g_heapZero = getenv("FNV_HEAP_ZERO") && *getenv("FNV_HEAP_ZERO") == '1';
    if (g_heapZero && p) memset(p, 0, size ? size : 1);
    return p;
}
static void __fastcall sysheap_free(void*, void*, void* p) { if (p) _aligned_free(p); }
static void* __fastcall sysheap_realloc(void*, void*, void* p, size_t size)
{
    if (!p) return _aligned_malloc(size ? size : 1, 16);
    if (!size) { _aligned_free(p); return nullptr; }
    return _aligned_realloc(p, size, 16);
}
static size_t __fastcall sysheap_size(void*, void*, const void* p) { return p ? _aligned_msize((void*)p, 16, 0) : 0; }

// FNV_HEAPCHECK=1 (with the system heap): each block gets a 16-byte header {magic, size, allocating caller} and 16
// guard bytes after it. A free checks both and logs overflows (with the bytes written past the end), double frees and
// frees of foreign pointers, naming the allocating and freeing callers (relinked DLL addresses: tools/addr2fn.py).
struct HcHead { DWORD magic, size, caller, caller2; };
// the first two game-DLL return addresses above `sp`: lifted callers reach the allocator through fnv_lift_call, so
// the second one is usually the lifted body that asked
static void hc_callers(DWORD* sp, DWORD* c1, DWORD* c2)
{
    *c1 = *c2 = 0;
    for (int i = 0; i < 96 && !IsBadReadPtr(sp + i, 4); ++i)
        if (sp[i] >= 0x20001000 && sp[i] < 0x24000000)
        {
            if (!*c1) *c1 = sp[i];
            else if (sp[i] != *c1) { *c2 = sp[i]; return; }
        }
}
const DWORD kHcLive = 0x4C4C4548u, kHcFreed = 0x44454546u;
static volatile LONG g_hcReports = 0;
static void hc_report(const char* what, const HcHead* h, const void* p, DWORD freer)
{
    if (InterlockedIncrement(&g_hcReports) > 40) return;
    char buf[400];
    int n = wsprintfA(buf, "[heapcheck] %s block %08lx size %lu alloc-callers %08lx %08lx free-caller %08lx thread %lu",
                      what, (DWORD)p, h ? h->size : 0, h ? h->caller : 0, h ? h->caller2 : 0, freer, GetCurrentThreadId());
    if (h && h->magic == kHcLive)
    {
        const unsigned char* t = (const unsigned char*)p + h->size;
        n += wsprintfA(buf + n, " tail:");
        for (int i = 0; i < 16; ++i) n += wsprintfA(buf + n, " %02x", t[i]);
        n += wsprintfA(buf + n, " \"");
        for (int i = 0; i < 16; ++i) buf[n++] = (t[i] >= 0x20 && t[i] < 0x7f) ? (char)t[i] : '.';
        buf[n++] = '"';
    }
    buf[n++] = '\r'; buf[n++] = '\n';
    DWORD w; WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), buf, n, &w, nullptr);
}
// FNV_HEAPCHECK_EVERY=N (default 2048): validate the whole process heap every N allocator calls; at the first
// failure, walk it and print the last entries before the broken one (a guarded block's header names its allocator)
static volatile LONG g_hcCalls = 0, g_hcBroken = 0;
static void hc_walk_report(LONG call, DWORD* sp)
{
    DWORD c1, c2;
    hc_callers(sp, &c1, &c2);
    fprintf(g_log, "[heapcheck] process heap invalid at allocator call %ld (current callers %08lx %08lx, thread %lu)\n",
            call, c1, c2, GetCurrentThreadId());
    PROCESS_HEAP_ENTRY e = {}, prev[4] = {};
    int n = 0, steps = 0;
    HANDLE heap = GetProcessHeap();
    HeapLock(heap);
    __try
    {
        while (HeapWalk(heap, &e) && steps < 50000000)
        {
            ++steps;
            if ((e.wFlags & PROCESS_HEAP_ENTRY_BUSY) && !HeapValidate(heap, 0, e.lpData))
            {
                fprintf(g_log, "[heapcheck] first invalid busy entry %p size %lu\n", e.lpData, e.cbData);
                break;
            }
            prev[n++ & 3] = e;
        }
        fprintf(g_log, "[heapcheck] walk ended after %d entries (error %lu)\n", steps, GetLastError());
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        fprintf(g_log, "[heapcheck] walk faulted after %d entries\n", steps);
    }
    HeapUnlock(heap);
    for (int k = 0; k < 4 && k < n; ++k)
    {
        const PROCESS_HEAP_ENTRY& p = prev[(n - 1 - k) & 3];
        const HcHead* h = (const HcHead*)p.lpData;   // _aligned_malloc puts its own word first: probe a few offsets
        fprintf(g_log, "[heapcheck]   entry -%d: %p size %lu flags %x", k, p.lpData, p.cbData, p.wFlags);
        for (int o = 0; o <= 32 && p.cbData > 32; o += 4)
        {
            const HcHead* hh = (const HcHead*)((const unsigned char*)p.lpData + o);
            if (hh->magic == kHcLive || hh->magic == kHcFreed)
            {
                fprintf(g_log, " guarded size %lu alloc-callers %08lx %08lx%s", hh->size, hh->caller, hh->caller2,
                        hh->magic == kHcFreed ? " (freed)" : "");
                break;
            }
        }
        const unsigned char* t = (const unsigned char*)p.lpData + (p.cbData > 32 ? p.cbData - 32 : 0);
        fprintf(g_log, " end:");
        for (int i = 0; i < 32 && i < (int)p.cbData; ++i) fprintf(g_log, "%02x", t[i]);
        fprintf(g_log, "\n");
        (void)h;
    }
    fflush(g_log);
}
static void hc_tick(DWORD* sp)
{
    static const LONG every = getenv("FNV_HEAPCHECK_EVERY") ? atol(getenv("FNV_HEAPCHECK_EVERY")) : 2048;
    LONG c = InterlockedIncrement(&g_hcCalls);
    if (g_hcBroken || every <= 0 || c % every) return;
    if (!HeapValidate(GetProcessHeap(), 0, nullptr) && !InterlockedExchange(&g_hcBroken, 1))
        hc_walk_report(c, sp);
}
static void* hc_alloc(size_t size, DWORD* sp)
{
    hc_tick(sp);
    DWORD caller, caller2;
    hc_callers(sp, &caller, &caller2);
    unsigned char* b = (unsigned char*)_aligned_malloc(size + 32, 16);
    if (!b) return nullptr;
    HcHead* h = (HcHead*)b;
    h->magic = kHcLive; h->size = (DWORD)size; h->caller = caller; h->caller2 = caller2;
    memset(b + 16 + size, 0xFD, 16);
    return b + 16;
}
static bool hc_check_free(void* p, DWORD freer)
{
    HcHead* h = (HcHead*)((unsigned char*)p - 16);
    if (IsBadReadPtr(h, 16)) { hc_report("free of a foreign pointer", nullptr, p, freer); return false; }
    if (h->magic == kHcFreed) { hc_report("double free", h, p, freer); return false; }
    if (h->magic != kHcLive) { hc_report("free with a damaged header (underrun or foreign)", h, p, freer); return false; }
    const unsigned char* t = (const unsigned char*)p + h->size;
    for (int i = 0; i < 16; ++i)
        if (t[i] != 0xFD) { hc_report("overflow past the end", h, p, freer); break; }
    h->magic = kHcFreed;
    _aligned_free(h);
    return true;
}
static void* __fastcall hc_sys_alloc(void*, void*, size_t size) { return hc_alloc(size ? size : 1, (DWORD*)_AddressOfReturnAddress()); }
static void __fastcall hc_sys_free(void*, void*, void* p)
{
    hc_tick((DWORD*)_AddressOfReturnAddress());
    if (p) hc_check_free(p, (DWORD)_ReturnAddress());
}
static void* __fastcall hc_sys_realloc(void*, void*, void* p, size_t size)
{
    DWORD caller = (DWORD)_ReturnAddress();
    DWORD* sp = (DWORD*)_AddressOfReturnAddress();
    if (!p) return hc_alloc(size ? size : 1, sp);
    if (!size) { hc_check_free(p, caller); return nullptr; }
    HcHead* h = (HcHead*)((unsigned char*)p - 16);
    void* n = hc_alloc(size, sp);
    if (n) memcpy(n, p, h->magic == kHcLive && h->size < size ? h->size : size);
    hc_check_free(p, caller);
    return n;
}
static size_t __fastcall hc_sys_size(void*, void*, const void* p)
{
    return p ? ((const HcHead*)((const unsigned char*)p - 16))->size : 0;
}

// FNV_SKIP_INTRO=1: the New Game intro is played by the script command PlayBink (0x005d15d0) through
// BSMovieThread 0x00ec2320 -> BSPlaybackFile::Play 0x00ec0e00 (thiscall: name, 7 bools, float; ret 0x24), which blocks
// the main thread until the movie ends or a key skips it. For names containing "intro" this returns false without
// playing, the original's own path when InitPlayback cannot open the file. Other movies play normally.
typedef unsigned char(__fastcall* MoviePlayFn)(void*, void*, const char*, bool, bool, bool, bool, bool, bool, bool, float);
static MoviePlayFn g_moviePlay;
static unsigned char __fastcall skip_intro_play(void* self, void* edx, const char* name, bool b1, bool b2, bool b3,
                                                bool b4, bool b5, bool b6, bool b7, float f)
{
    char low[MAX_PATH] = {};
    for (int i = 0; name && name[i] && i < MAX_PATH - 1; ++i)
        low[i] = (char)tolower((unsigned char)name[i]);
    if (strstr(low, "intro"))
    {
        fprintf(g_log, "[loader] movie skipped (FNV_SKIP_INTRO): %s\n", name);
        fflush(g_log);
        return 0;
    }
    fprintf(g_log, "[loader] movie plays: %s\n", name ? name : "(null)");
    fflush(g_log);
    return g_moviePlay(self, edx, name, b1, b2, b3, b4, b5, b6, b7, f);
}

static void detour(DWORD original, void* to)
{
    unsigned char* f = (unsigned char*)relinked(original);
    if (!f) { fprintf(g_log, "[loader] detour: 0x%08lx not in the build\n", original); return; }
    DWORD old;
    VirtualProtect(f, 5, PAGE_EXECUTE_READWRITE, &old);
    f[0] = 0xE9;
    *(LONG*)(f + 1) = (LONG)((unsigned char*)to - (f + 5));
    VirtualProtect(f, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), f, 5);
}

// detour() that keeps the original callable: copies whole prologue instructions (the forms clang emits: push reg,
// mov ebp,esp, sub esp,imm, push imm) to a trampoline ending in a jump back. nullptr if the prologue is not one of them.
static void* detour_keep(DWORD original, void* to)
{
    unsigned char* f = (unsigned char*)relinked(original);
    if (!f) { fprintf(g_log, "[loader] detour: 0x%08lx not in the build\n", original); return nullptr; }
    int n = 0;
    while (n < 5)
    {
        unsigned char b = f[n];
        int len = (b >= 0x50 && b <= 0x57) ? 1
                  : ((b == 0x89 && f[n + 1] == 0xe5) || (b == 0x8b && f[n + 1] == 0xec) || b == 0x6a) ? 2
                  : (b == 0x83 && f[n + 1] == 0xec) ? 3
                  : b == 0x68 ? 5
                  : (b == 0x81 && f[n + 1] == 0xec) ? 6 : 0;
        if (!len)
        {
            fprintf(g_log, "[loader] detour: 0x%08lx prologue byte %02x at +%d not handled, not hooked\n", original, b, n);
            return nullptr;
        }
        n += len;
    }
    unsigned char* t = (unsigned char*)VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    memcpy(t, f, n);
    t[n] = 0xE9;
    *(LONG*)(t + n + 1) = (LONG)((f + n) - (t + n + 5));
    detour(original, to);
    return t;
}

int main(int argc, char** argv)
{
    g_sysheap = !(getenv("FNV_SYSHEAP") && !strcmp(getenv("FNV_SYSHEAP"), "0"));
    if (getenv("FNV_THREAD_HOST") && *getenv("FNV_THREAD_HOST") == '1')
    {   // the main thread holds the baton from the start: game threads created during init wait for it
        g_th = true;
        t_thGame = 1;
        th_acquire();
    }
    g_bg = getenv("FNV_BACKGROUND") && *getenv("FNV_BACKGROUND") && strcmp(getenv("FNV_BACKGROUND"), "0");   // 1 or offscreen
    if (getenv("FNV_STARTUP_WATCHDOG") && *getenv("FNV_STARTUP_WATCHDOG") == '1')
    {
        g_watchdogStart = CreateEventA(nullptr, TRUE, FALSE, nullptr);
        HANDLE thread = nullptr;
        if (g_watchdogStart && DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &thread, 0, FALSE,
                            DUPLICATE_SAME_ACCESS))
        {
            HANDLE watcher = CreateThread(nullptr, 0, startup_watchdog, thread, 0, nullptr);
            if (watcher) CloseHandle(watcher);
            else { CloseHandle(thread); fprintf(g_log, "[watchdog] CreateThread failed\n"); }
        }
        else fprintf(g_log, "[watchdog] DuplicateHandle failed\n");
    }
    {   // private copy of the player's "My Games\FalloutNV" (see wrap_SHGetFolderPathA)
        char exeDir[MAX_PATH];
        GetModuleFileNameA(nullptr, exeDir, sizeof(exeDir));
        char* slash = strrchr(exeDir, 0x5c);
        if (slash) *slash = 0;
        slash = strrchr(exeDir, 0x5c);
        if (slash) *slash = 0;                              // out/link -> out
        snprintf(g_docsRoot, sizeof(g_docsRoot), "%s/fnv_docs", exeDir);
        char dir[MAX_PATH], src[MAX_PATH], dst[MAX_PATH], real[MAX_PATH];
        snprintf(dir, sizeof(dir), "%s/My Games/FalloutNV", g_docsRoot);
        CreateDirectoryA(g_docsRoot, nullptr);
        snprintf(dst, sizeof(dst), "%s/My Games", g_docsRoot);
        CreateDirectoryA(dst, nullptr);
        CreateDirectoryA(dir, nullptr);
        snprintf(dst, sizeof(dst), "%s/Saves", dir);
        CreateDirectoryA(dst, nullptr);
        typedef HRESULT(WINAPI * Fn)(HWND, int, HANDLE, DWORD, LPSTR);
        Fn get = (Fn)GetProcAddress(LoadLibraryA("shell32.dll"), "SHGetFolderPathA");
        if (get && get(nullptr, CSIDL_PERSONAL, nullptr, 0, real) == S_OK)
            for (const char* name : {"Fallout.ini", "FalloutPrefs.ini"})
            {
                snprintf(src, sizeof(src), "%s/My Games/FalloutNV/%s", real, name);
                snprintf(dst, sizeof(dst), "%s/%s", dir, name);
                if (!getenv("FNV_KEEP_DOCS")) { SetFileAttributesA(dst, FILE_ATTRIBUTE_NORMAL); DeleteFileA(dst); }   // reseed every run
                if (GetFileAttributesA(dst) == INVALID_FILE_ATTRIBUTES)
                {
                    CopyFileA(src, dst, TRUE);
                    SetFileAttributesA(dst, FILE_ATTRIBUTE_NORMAL);
                    if (!strcmp(name, "FalloutPrefs.ini") && !getenv("FNV_FULLSCREEN"))
                    {   // a fullscreen mode must match a display mode exactly; test windowed
                        WritePrivateProfileStringA("Display", "bFull Screen", "0", dst);
                        WritePrivateProfileStringA("Display", "iSize W", "1280", dst);
                        WritePrivateProfileStringA("Display", "iSize H", "720", dst);
                    }
                }
                if (g_bg)
                    WritePrivateProfileStringA("General", "bAlwaysActive", "1", dst);   // keep running unfocused
                if (getenv("FNV_NO_HDR") && *getenv("FNV_NO_HDR") == '1')   // A/B test for the all-white world view
                    WritePrivateProfileStringA("BlurShaderHDR", "bDoHighDynamicRange", "0", dst);
            }
    }
    const char* exe = getenv("FNV_EXE") ? getenv("FNV_EXE") : "E:\\Games\\Fallout New Vegas\\FalloutNV.exe";
    const char* dll = "fnv_game.dll";
    for (int i = 1; i + 1 < argc; ++i)
    {
        if (!strcmp(argv[i], "--exe"))
            exe = argv[++i];
        else if (!strcmp(argv[i], "--dll"))
            dll = argv[++i];
    }
    if (getenv("FNV_VEH_LOG"))   // first-chance exception log written with raw WriteFile (safe on any thread, any lock state)
        AddVectoredExceptionHandler(1, [](EXCEPTION_POINTERS* ep) -> LONG {
            static volatile LONG count = 0;
            if (InterlockedIncrement(&count) > 200) return EXCEPTION_CONTINUE_SEARCH;
            char buf[256];
            int n = wsprintfA(buf, "[veh] thread %lu code %08lx at %08lx esp %08lx info %08lx %08lx\r\n", GetCurrentThreadId(), ep->ExceptionRecord->ExceptionCode,
                              (DWORD)ep->ExceptionRecord->ExceptionAddress, ep->ContextRecord->Esp,
                              ep->ExceptionRecord->NumberParameters > 0 ? (DWORD)ep->ExceptionRecord->ExceptionInformation[0] : 0,
                              ep->ExceptionRecord->NumberParameters > 1 ? (DWORD)ep->ExceptionRecord->ExceptionInformation[1] : 0);
            DWORD w; WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), buf, n, &w, nullptr);
            DWORD code = ep->ExceptionRecord->ExceptionCode;
            if ((code == 0xC0000005 || code == 0xC0000374) && !IsBadReadPtr((void*)ep->ContextRecord->Esp, 128))
            {   // c0000374 (heap corruption) ends the process without the crash handler: this is the only stack
                DWORD* sp = (DWORD*)ep->ContextRecord->Esp;
                n = wsprintfA(buf, "[veh]   stack %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx\r\n", sp[0], sp[1], sp[2], sp[3], sp[4], sp[5], sp[6], sp[7]);
                WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), buf, n, &w, nullptr);
                int shown = 0;
                for (int i = 8; i < 3000 && shown < (code == 0xC0000374 ? 40 : 14) && !IsBadReadPtr(sp + i, 4); ++i)
                    if (sp[i] >= 0x20001000 && sp[i] < 0x24000000)
                    {
                        n = wsprintfA(buf, "[veh]   dll word +%x = %08lx\r\n", i * 4, sp[i]);
                        WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), buf, n, &w, nullptr);
                        ++shown;
                    }
                if (code == 0xC0000374)   // a native thread: name the module of each code-looking stack word
                {
                    int named = 0;
                    for (int i = 0; i < 1500 && named < 30 && !IsBadReadPtr(sp + i, 4); ++i)
                    {
                        HMODULE m = nullptr;
                        MEMORY_BASIC_INFORMATION mbi;
                        if (sp[i] < 0x10000 || !VirtualQuery((void*)sp[i], &mbi, sizeof(mbi)) ||
                            !(mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)))
                            continue;
                        char name[MAX_PATH] = "?";
                        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                               (LPCSTR)sp[i], &m))
                            GetModuleFileNameA(m, name, sizeof(name));
                        const char* base = strrchr(name, 0x5c);
                        n = wsprintfA(buf, "[veh]   code +%x = %08lx %s+%lx\r\n", i * 4, sp[i], base ? base + 1 : name,
                                      sp[i] - (DWORD)m);
                        WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), buf, n, &w, nullptr);
                        ++named;
                    }
                }
            }
            return EXCEPTION_CONTINUE_SEARCH;
        });
    // MSVC SetThreadName (RaiseException 0x406D1388 inside __try/__except) and debug-print exceptions: harmless without a debugger
    AddVectoredExceptionHandler(0, [](EXCEPTION_POINTERS* ep) -> LONG {
        DWORD c = ep->ExceptionRecord->ExceptionCode;
        return (c == 0x406D1388 || c == 0x40010006 || c == 0x4001000A) ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
    });
    SetUnhandledExceptionFilter(on_crash);
    { ULONG guarantee = 128 * 1024; SetThreadStackGuarantee(&guarantee); }   // let the crash handler run after a stack overflow

    if ((DWORD)g_reserved > kDataStart || (DWORD)g_reserved + sizeof(g_reserved) < kDataEnd)
        return fail("reserved section does not cover the original data range");

    Pe pe;
    if (!pe.load(exe))
        return fail("cannot read FalloutNV.exe (or wrong image base)");
    const IMAGE_SECTION_HEADER* text = pe.section(".text");
    g_textStart = kImageBase + text->VirtualAddress;
    g_textEnd = g_textStart + text->Misc.VirtualSize;

    // 1. data image
    memset((void*)kDataStart, 0, kDataEnd - kDataStart);
    for (const char* name : {".rdata", ".data", ".tls", "CONST"})
    {
        const IMAGE_SECTION_HEADER* s = pe.section(name);
        if (!s)
            return fail("missing data section");
        DWORD va = kImageBase + s->VirtualAddress;
        DWORD n = min(s->SizeOfRawData, max(s->Misc.VirtualSize, 1ul));
        if (va < kDataStart || va + max(n, s->Misc.VirtualSize) > kDataEnd)
            return fail("data section outside the reserved range");
        memcpy((void*)va, pe.file.data() + s->PointerToRawData, n);
    }
    fprintf(g_log, "[loader] data image mapped at 0x%08lx..0x%08lx\n", kDataStart, kDataEnd);

    // 2. game DLL: change to the install folder so the game finds Data\ and its own DLLs
    char gameDir[MAX_PATH], dllPath[MAX_PATH];
    GetFullPathNameA(dll, MAX_PATH, dllPath, nullptr);
    GetFullPathNameA(exe, MAX_PATH, gameDir, nullptr);       // also normalises '/' to '\'
    if (char* slash = strrchr(gameDir, '\\'))
        *slash = 0;
    SetCurrentDirectoryA(gameDir);
    SetDllDirectoryA(gameDir);
    // log first-chance faults while the DLL initialises (DllMain/CRT init failures report only error 1114)
    PVOID loadVeh = AddVectoredExceptionHandler(1, [](EXCEPTION_POINTERS* ep) -> LONG {
        DWORD code = ep->ExceptionRecord->ExceptionCode;
        if (code != 0xE06D7363 && code != 0x406D1388)
            fprintf(g_log, "[loader] during DLL load: exception 0x%08lx at %p (data 0x%08lx)\n", code,
                    ep->ExceptionRecord->ExceptionAddress,
                    ep->ExceptionRecord->NumberParameters >= 2 ? (DWORD)ep->ExceptionRecord->ExceptionInformation[1] : 0);
        return EXCEPTION_CONTINUE_SEARCH;
    });
    HMODULE game = LoadLibraryA(dllPath);
    RemoveVectoredExceptionHandler(loadVeh);
    if (!game)
        return fail("cannot load fnv_game.dll");
    if (g_th)
    {   // time slice for the thread host: the DLL's lifted calls check a flag the watchdog sets
        using SetYield = volatile long* (__cdecl*)(void (__cdecl*)(void));
        if (auto sy = (SetYield)GetProcAddress(game, "fnv_lift_set_yield"))
        {
            g_thYieldFlag = sy(&th_yield);
            CloseHandle(CreateThread(nullptr, 0, th_watchdog, nullptr, 0, nullptr));
            fprintf(g_log, "[loader] FNV_THREAD_HOST=1: game threads run one at a time (20 ms time slice)\n");
        }
    }
    using SetLiftText = void (__cdecl*)(const unsigned char*, DWORD, DWORD);
    auto setLiftText = (SetLiftText)GetProcAddress(game, "fnv_lift_set_text");
    if (!setLiftText || text->PointerToRawData + text->SizeOfRawData > pe.file.size())
        return fail("cannot initialize lifted original text view");
    setLiftText(pe.file.data() + text->PointerToRawData, g_textStart, text->SizeOfRawData);
    if (!getenv("FNV_KEEP_THREAD_ATTACH"))
        DisableThreadLibraryCalls(game);
    LoadLibraryA("uxtheme.dll");
    patch_dll_imports(game);
    auto table = (const AddrEntry*)GetProcAddress(game, "fnv_addr_table");
    if (!table)
        return fail("fnv_addr_table export missing");
    for (const AddrEntry* e = table; e->original; ++e)
        g_table.push_back(*e);
    g_byPtr = g_table;
    std::sort(g_byPtr.begin(), g_byPtr.end(), [](const AddrEntry& a, const AddrEntry& b) { return a.relinked < b.relinked; });
    fprintf(g_log, "[loader] %s at %p, %zu relinked functions\n", dllPath, (void*)game, g_table.size());
    AddVectoredExceptionHandler(1, redirect_original_code);
    if (!getenv("FNV_SYSHEAP") || strcmp(getenv("FNV_SYSHEAP"), "0"))
    {
        bool hc = getenv("FNV_HEAPCHECK") && *getenv("FNV_HEAPCHECK") == '1';
        detour(0x00AA3E40, hc ? (void*)&hc_sys_alloc : (void*)&sysheap_alloc);
        detour(0x00AA4060, hc ? (void*)&hc_sys_free : (void*)&sysheap_free);
        detour(0x00AA4150, hc ? (void*)&hc_sys_realloc : (void*)&sysheap_realloc);
        detour(0x00AA44C0, hc ? (void*)&hc_sys_size : (void*)&sysheap_size);
        if (hc) fprintf(g_log, "[loader] FNV_HEAPCHECK=1: guarded blocks, checked on free\n");
        fprintf(g_log, "[loader] MemoryManager routed to the CRT heap (FNV_SYSHEAP=0 to keep the game's pools)\n");
    }
    if (const char* si = getenv("FNV_SKIP_INTRO"); si && *si == '1')
    {
        g_moviePlay = (MoviePlayFn)detour_keep(0x00EC0E00, (void*)&skip_intro_play);
        fprintf(g_log, "[loader] FNV_SKIP_INTRO=1: intro movies are skipped%s\n", g_moviePlay ? "" : " (hook NOT installed)");
    }
    if (const char* bp = getenv("FNV_BP"))
    {
        for (const char* q = bp; q && *q && g_bpCount < 32;)
        {
            DWORD orig = (DWORD)strtoul(q, nullptr, 16);
            if (void* f = relinked(orig))
            {
                Bp& b = g_bps[g_bpCount++];
                b.addr = (unsigned char*)f; b.orig = *b.addr; b.original = orig; b.hits = 0;
                DWORD old;
                VirtualProtect(b.addr, 1, PAGE_EXECUTE_READWRITE, &old);
                *b.addr = 0xCC;
                VirtualProtect(b.addr, 1, old, &old);
                fprintf(g_log, "[loader] bp armed at original 0x%08lx (%p)\n", orig, f);
            }
            q = strchr(q, 0x2c);
            if (q) ++q;
        }
        AddVectoredExceptionHandler(1, bp_handler);
    }
    if (const char* at = getenv("FNV_TRACE_AT"))
    {
        DWORD orig = (DWORD)strtoul(at, nullptr, 16);
        if (void* f = relinked(orig))
        {
            g_traceAddr = (unsigned char*)f;
            g_traceOrig = *g_traceAddr;
            DWORD old;
            VirtualProtect(g_traceAddr, 1, PAGE_EXECUTE_READWRITE, &old);
            *g_traceAddr = 0xCC;
            VirtualProtect(g_traceAddr, 1, old, &old);
            AddVectoredExceptionHandler(1, trace_handler);
            fprintf(g_log, "[loader] trace armed at original 0x%08lx (%p)\n", orig, f);
        }
    }

    // 3. code pointers in data, from the exe's own relocation table
    g_thunks = (unsigned char*)VirtualAlloc(nullptr, 1 << 22, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    std::vector<std::pair<DWORD, void*>> thunkCache;
    auto& rdir = pe.nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    const unsigned char* rp = pe.at(rdir.VirtualAddress);
    const unsigned char* rend = rp + rdir.Size;
    size_t toFunc = 0, toThunk = 0, diFormats = 0;
    std::vector<DWORD> unbound;
    while (rp < rend)
    {
        auto block = (const IMAGE_BASE_RELOCATION*)rp;
        if (!block->SizeOfBlock)
            break;
        auto entries = (const WORD*)(block + 1);
        size_t n = (block->SizeOfBlock - sizeof(*block)) / 2;
        for (size_t i = 0; i < n; ++i)
        {
            if ((entries[i] >> 12) != IMAGE_REL_BASED_HIGHLOW)
                continue;
            DWORD va = kImageBase + block->VirtualAddress + (entries[i] & 0xfff);
            if (va < kDataStart || va + 4 > kDataEnd)
                continue;
            DWORD v = *(DWORD*)va;
            if (v < g_textStart || v >= g_textEnd)
                continue;                                     // data -> data stays as is
            if (va >= kDataStart + 20 && ((DWORD*)va)[-5] == 0x18 && ((DWORD*)va)[-4] == 0x10 &&
                v + 16 * ((DWORD*)va)[-1] <= g_textEnd)
            {   // DIDATAFORMAT.rgodf: dinput8.lib's c_dfDI* object tables were linked into .text. They are data, so
                // point at the original bytes instead of a lifted "function" (Acquire failed with DIERR_INVALIDPARAM)
                *(const void**)va = pe.file.data() + text->PointerToRawData + (v - g_textStart);
                ++diFormats;
                continue;
            }
            if (void* f = relinked(v))
            {
                *(void**)va = f;
                ++toFunc;
            }
            else
            {
                unbound.push_back(v);
                ++toThunk;
            }
        }
        rp += block->SizeOfBlock;
    }
    std::sort(unbound.begin(), unbound.end());
    unbound.erase(std::unique(unbound.begin(), unbound.end()), unbound.end());
    // second pass for the unbound targets: one thunk per distinct original address
    rp = pe.at(rdir.VirtualAddress);
    while (rp < rend)
    {
        auto block = (const IMAGE_BASE_RELOCATION*)rp;
        if (!block->SizeOfBlock)
            break;
        auto entries = (const WORD*)(block + 1);
        size_t n = (block->SizeOfBlock - sizeof(*block)) / 2;
        for (size_t i = 0; i < n; ++i)
        {
            if ((entries[i] >> 12) != IMAGE_REL_BASED_HIGHLOW)
                continue;
            DWORD va = kImageBase + block->VirtualAddress + (entries[i] & 0xfff);
            if (va < kDataStart || va + 4 > kDataEnd)
                continue;
            DWORD v = *(DWORD*)va;
            if (v < g_textStart || v >= g_textEnd)
                continue;                                     // (DIDATAFORMAT tables were redirected above)
            size_t k = std::lower_bound(unbound.begin(), unbound.end(), v) - unbound.begin();
            if (k < unbound.size() && unbound[k] == v)
            {
                while (thunkCache.size() <= k)
                    thunkCache.push_back({unbound[thunkCache.size()], nullptr});
                if (!thunkCache[k].second)
                    thunkCache[k].second = thunk_for(v);
                *(void**)va = thunkCache[k].second;
            }
        }
        rp += block->SizeOfBlock;
    }
    fprintf(g_log, "[loader] DirectInput data formats pointed at the original object tables: %zu\n", diFormats);
    fprintf(g_log, "[loader] code pointers in data: %zu -> relinked, %zu -> thunks (%zu distinct)\n", toFunc, toThunk,
            unbound.size());

    // 4. imports into the original IAT slots
    auto& idir = pe.nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    size_t imported = 0, missing = 0;
    for (auto d = (const IMAGE_IMPORT_DESCRIPTOR*)pe.at(idir.VirtualAddress); d->Name; ++d)
    {
        const char* dllName = (const char*)pe.at(d->Name);
        HMODULE m = LoadLibraryA(dllName);
        auto names = (const DWORD*)pe.at(d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk);
        for (DWORD k = 0; names[k]; ++k)
        {
            DWORD* slot = (DWORD*)(kImageBase + d->FirstThunk + 4 * k);
            FARPROC f = nullptr;
            if (m)
                f = (names[k] & IMAGE_ORDINAL_FLAG32)
                        ? GetProcAddress(m, (LPCSTR)(ULONG_PTR)(names[k] & 0xffff))
                        : GetProcAddress(m, (const char*)pe.at(names[k]) + 2);
            if (f && !(names[k] & IMAGE_ORDINAL_FLAG32) && !strcmp((const char*)pe.at(names[k]) + 2, "SetUnhandledExceptionFilter") &&
                !getenv("FNV_GAME_CRASH_FILTER"))
                f = (FARPROC)&ignore_set_filter;     // keep the loader's crash report; the game's filter would hide it
            if (f && !(names[k] & IMAGE_ORDINAL_FLAG32) && !strcmp((const char*)pe.at(names[k]) + 2, "ExitProcess")) f = (FARPROC)&wrap_ExitProcess;
            if (f && !(names[k] & IMAGE_ORDINAL_FLAG32) && !strcmp((const char*)pe.at(names[k]) + 2, "CreateFileA")) f = (FARPROC)&wrap_CreateFileA;
            if (f && !(names[k] & IMAGE_ORDINAL_FLAG32) && !strcmp((const char*)pe.at(names[k]) + 2, "ShellExecuteA") && !getenv("FNV_ALLOW_SHELL")) f = (FARPROC)&wrap_ShellExecuteA;
            if (f && !(names[k] & IMAGE_ORDINAL_FLAG32) && !strcmp((const char*)pe.at(names[k]) + 2, "SHGetFolderPathA")) f = (FARPROC)&wrap_SHGetFolderPathA;
            if (f && !(names[k] & IMAGE_ORDINAL_FLAG32) && !strcmp((const char*)pe.at(names[k]) + 2, "MessageBoxA")) f = (FARPROC)&wrap_MessageBoxA;
            if (f && !(names[k] & IMAGE_ORDINAL_FLAG32) && !strcmp((const char*)pe.at(names[k]) + 2, "OutputDebugStringA")) f = (FARPROC)&wrap_OutputDebugStringA;
            if (f && !(names[k] & IMAGE_ORDINAL_FLAG32) && !strcmp((const char*)pe.at(names[k]) + 2, "GetFileAttributesA")) f = (FARPROC)&wrap_GetFileAttributesA;
            if (f && !(names[k] & IMAGE_ORDINAL_FLAG32) && !strcmp((const char*)pe.at(names[k]) + 2, "FindFirstFileA")) f = (FARPROC)&wrap_FindFirstFileA;
            if (f && !(names[k] & IMAGE_ORDINAL_FLAG32) && !strcmp((const char*)pe.at(names[k]) + 2, "TerminateProcess")) f = (FARPROC)&wrap_TerminateProcess;
            if (f && !(names[k] & IMAGE_ORDINAL_FLAG32) && !strcmp((const char*)pe.at(names[k]) + 2, "DefWindowProcA")) f = (FARPROC)&wrap_DefWindowProcA;
            if (f && !(names[k] & IMAGE_ORDINAL_FLAG32) && !strcmp((const char*)pe.at(names[k]) + 2, "DestroyWindow")) f = (FARPROC)&wrap_DestroyWindow;
            if (f && !(names[k] & IMAGE_ORDINAL_FLAG32) && !strcmp((const char*)pe.at(names[k]) + 2, "CreateWindowExA")) f = (FARPROC)&wrap_CreateWindowExA;
            if (f && !(names[k] & IMAGE_ORDINAL_FLAG32) && !strcmp((const char*)pe.at(names[k]) + 2, "ShowWindow")) f = (FARPROC)&wrap_ShowWindow;
            if (f && !(names[k] & IMAGE_ORDINAL_FLAG32) && !strcmp((const char*)pe.at(names[k]) + 2, "SetForegroundWindow")) f = (FARPROC)&wrap_SetForegroundWindow;
            if (f && !(names[k] & IMAGE_ORDINAL_FLAG32) && !strcmp((const char*)pe.at(names[k]) + 2, "SetWindowPos")) f = (FARPROC)&wrap_SetWindowPos;
            if (f && !(names[k] & IMAGE_ORDINAL_FLAG32) && !strcmp((const char*)pe.at(names[k]) + 2, "VirtualAlloc")) f = (FARPROC)&wrap_VirtualAlloc;
            if (f && !(names[k] & IMAGE_ORDINAL_FLAG32) && !strcmp((const char*)pe.at(names[k]) + 2, "GetProcAddress")) f = (FARPROC)&wrap_GetProcAddress;
            if (f && !(names[k] & IMAGE_ORDINAL_FLAG32))   // thread host (no-ops unless FNV_THREAD_HOST=1)
            {
                const char* nm2 = (const char*)pe.at(names[k]) + 2;
                if (!strcmp(nm2, "CreateThread")) f = (FARPROC)&wrap_CreateThread;
                else if (!strcmp(nm2, "ExitThread")) f = (FARPROC)&wrap_ExitThread;
                else if (!strcmp(nm2, "Sleep")) f = (FARPROC)&wrap_Sleep;
                else if (!strcmp(nm2, "WaitForSingleObject")) f = (FARPROC)&wrap_WaitForSingleObject;
                else if (!strcmp(nm2, "WaitForMultipleObjects")) f = (FARPROC)&wrap_WaitForMultipleObjects;
                else if (!strcmp(nm2, "EnterCriticalSection")) f = (FARPROC)&wrap_EnterCriticalSection;
            }
            if (f && !(names[k] & IMAGE_ORDINAL_FLAG32) && !strcmp((const char*)pe.at(names[k]) + 2, "Direct3DCreate9") &&
                getenv("FNV_D3D_STATS"))
            {
                g_realD3DCreate9 = (D3DCreate9Fn)f;
                f = (FARPROC)&wrap_Direct3DCreate9;
            }
            if (f && !(names[k] & IMAGE_ORDINAL_FLAG32) && !strcmp((const char*)pe.at(names[k]) + 2, "DirectInput8Create") && input_init())
            {
                g_realDiCreate = (DiCreateFn)f;
                f = (FARPROC)&wrap_DirectInput8Create;
            }
            if (f)
            {
                *slot = (DWORD)f;
                ++imported;
            }
            else
            {
                *slot = (DWORD)thunk_for((DWORD)slot);
                ++missing;
                fprintf(g_log, "[loader] import not found: %s!%s\n", dllName,
                        (names[k] & IMAGE_ORDINAL_FLAG32) ? "#ordinal" : (const char*)pe.at(names[k]) + 2);
            }
        }
    }
    fprintf(g_log, "[loader] imports: %zu resolved, %zu missing\n", imported, missing);

    // 5. original C++ initializers, in their original order. Each runs under SEH: one that reaches unbound code or
    //    faults is logged with the original address it was blocked on, and startup continues, so one run lists
    //    every blocker. FNV_STRICT_INIT=1 stops at the first one instead.
    bool strict = getenv("FNV_STRICT_INIT") && *getenv("FNV_STRICT_INIT") == '1';
    PVOID veh = AddVectoredExceptionHandler(1, guard_handler);
    // The original CRT's startup (__tmainCRTStartup) is not run; its state is what the lifted CRT code the game still
    // uses (per-thread data, errno, stdio) depends on. FNV_NO_CRT_STARTUP=1 skips it.
    if (!getenv("FNV_NO_CRT_STARTUP"))
    {
        // _heap_init: the original creates its own CRT heap (and VC9 makes it terminate on corruption). The relinked build
        // mixes the original CRT's allocator with the new one, so both use the process heap.
        *(DWORD*)0x012705BC = (DWORD)GetProcessHeap();
        *(DWORD*)0x01270948 = 1;
        struct Step { DWORD addr; int arg; bool hasArg; const char* name; };
        const Step steps[] = {{0x00ED1327, 0, false, "_mtinit"},
                              {0x00ED7E18, 0, false, "_ioinit"}, {0x00ED248E, 0, false, "_cinit helper"}};
        for (const Step& s : steps)
        {
            void* f = relinked(s.addr);
            if (!f)
            {
                fprintf(g_log, "[loader] CRT startup: %s (0x%08lx) not in the build\n", s.name, s.addr);
                continue;
            }
            g_guardThread = GetCurrentThreadId();
            g_guardActive = 1;
            g_guard = InitResult{0, 0, nullptr, {0}};
            int rv = 0;
            struct Caller { static int __cdecl call(void* fn, int arg, bool has) { return has ? ((int(__cdecl*)(int))fn)(arg) : ((int(__cdecl*)(void))fn)(); } };
            // run under the same guard as the initializers: copy of run_initializer for a function with a result
            InitResult r;
            {
                static int s_arg, s_has; static void* s_fn; static int s_rv;
                s_arg = s.arg; s_has = s.hasArg; s_fn = f;
                r = run_initializer([]() { s_rv = Caller::call(s_fn, s_arg, s_has != 0); });
                rv = s_rv;
            }
            g_guardActive = 0;
            fprintf(g_log, "[loader] CRT startup: %s -> %d%s\n", s.name, rv, r.code ? " (faulted)" : "");
        }
        // _cinit: the C initializer table (__xi_a..__xi_z), before the C++ one
        for (DWORD p = 0x010113C4; p < 0x010113E8; p += 4)
        {
            auto f = *(int(__cdecl**)(void))p;
            if (!f)
                continue;
            InitResult r = run_initializer((void(__cdecl*)(void))f);
            if (r.code)
                fprintf(g_log, "[loader] CRT startup: C initializer at 0x%08lx faulted\n", p);
    }
        // keep the original CRT on its x87 / non-SSE2 paths (its SSE2 libm and memset variants use instructions
        // the lifter does not translate)
        *(DWORD*)0x01270A64 = 0;
        *(DWORD*)0x01270A6C = 0;
    }
    size_t ran = 0, skipped = 0, blocked = 0;
    for (DWORD p = kXcA; p < kXcZ; p += 4)
    {
        auto f = *(void(__cdecl**)(void))p;
        if (!f)
            continue;
        if ((unsigned char*)f >= g_thunks && (unsigned char*)f < g_thunks + g_thunkUsed)
        {
            ++skipped;                                        // initializer outside the decomp's scope
            continue;
        }
        if (const char* skip = getenv("FNV_SKIP_INIT"))
        {
            auto it2 = std::lower_bound(g_byPtr.begin(), g_byPtr.end(), (void*)f,
                                        [](const AddrEntry& e, void* q) { return e.relinked < q; });
            char tag[16];
            sprintf(tag, "%08lx", (it2 != g_byPtr.end() && it2->relinked == (void*)f) ? it2->original : 0ul);
            if (strstr(skip, tag)) { ++skipped; continue; }
        }
        InitResult r = strict ? InitResult{0, 0, nullptr, {0}} : run_initializer(f);
        if (strict)
            f();
        if (r.code)
        {
            ++blocked;
            auto it = std::lower_bound(g_byPtr.begin(), g_byPtr.end(), (void*)f,
                                       [](const AddrEntry& e, void* q) { return e.relinked < q; });
            DWORD orig = (it != g_byPtr.end() && it->relinked == (void*)f) ? it->original : 0;
            if (r.code == kTrapException)
                fprintf(g_log, "[init-blocked] initializer 0x%08lx (slot 0x%08lx): unbound 0x%08lx\n", orig, p,
                        r.detail);
            else
            {
                fprintf(g_log, "[init-blocked] initializer 0x%08lx (slot 0x%08lx): exception 0x%08lx at %p "
                               "(data 0x%08lx)\n", orig, p, r.code, r.where, r.detail);
                fprintf(g_log, "[loader]   %s\n", g_guardDump);
                describe(r.where);
                for (int i = 0; i < 6 && r.frames[i]; ++i)
                {
                    fprintf(g_log, "[loader]   from %08lx", r.frames[i]);
                    describe((void*)r.frames[i]);
                }
            }
            continue;
        }
        ++ran;
    }
    RemoveVectoredExceptionHandler(veh);
    fprintf(g_log, "[loader] initializers: %zu ran, %zu blocked, %zu skipped (not relinked)\n", ran, blocked,
            skipped);
    fflush(g_log);

    // debug: FNV_PROBE_ALLOC=1 calls the relinked allocators and prints what they return, then exits
    if (getenv("FNV_PROBE_ALLOC") && *getenv("FNV_PROBE_ALLOC") == '1')
    {
        auto niNew = (void*(__cdecl*)(unsigned int))relinked(0x00AA13E0);          // NiMemObject::operator new
        auto niMalloc = (void*(__cdecl*)(unsigned int))relinked(0x00AA1070);       // NiMalloc
        auto mmAlloc = (void*(__fastcall*)(void*, void*, unsigned int))relinked(0x00AA3E40); // MemoryManager::Allocate
        for (unsigned int size : {0x1Cu, 0x40u, 0x400u, 0x10000u})
        {
            void* a = niNew ? niNew(size) : nullptr;
            void* b = niMalloc ? niMalloc(size) : nullptr;
            void* c = mmAlloc ? mmAlloc((void*)0x011F6238, nullptr, size) : nullptr;
            fprintf(g_log, "[probe] size %#x: NiNew %p NiMalloc %p MemoryManager::Allocate %p\n", size, a, b, c);
        }
        fflush(g_log);
        return 0;
    }

    // 6. WinMain
    auto winMain = (int(__stdcall*)(HINSTANCE, HINSTANCE, LPSTR, int))relinked(kWinMain);
    if (!winMain)
        return fail("WinMain not relinked");
    if (const char* d = getenv("FNV_DUMP"))   // "hexaddr:words" - words of the data image just before WinMain
    {
        DWORD addr = (DWORD)strtoul(d, nullptr, 16);
        const char* c = strchr(d, ':');
        int words = c ? atoi(c + 1) : 8;
        for (int i = 0; i < words; ++i)
            fprintf(g_log, "[dump] %08lx: %08lx\n", addr + i * 4, *(DWORD*)(addr + i * 4));
    }
    g_mainThreadId = GetCurrentThreadId();
    fprintf(g_log, "[loader] calling WinMain\n");
    fflush(g_log);
    if (g_watchdogStart) SetEvent(g_watchdogStart);
    int rc = winMain(GetModuleHandleA(nullptr), nullptr, GetCommandLineA(), SW_SHOWDEFAULT);
    fprintf(g_log, "[loader] WinMain returned %d\n", rc);
    return rc;
}
