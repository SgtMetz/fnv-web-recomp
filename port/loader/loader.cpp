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
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
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
const int kTraceRing = 4096;
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
LONG WINAPI on_crash(EXCEPTION_POINTERS* ep)
{
    g_tracing = 0;
    auto r = ep->ExceptionRecord;
    CONTEXT* ctx = ep->ContextRecord;
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
    for (int i = 0; i < 16 && !IsBadReadPtr(sp + i, 4); ++i)
        fprintf(g_log, "[loader]   raw-stack+%03x %08lx\n", i * 4, sp[i]);
    if (!g_byPtr.empty())
    {
        DWORD lo = (DWORD)g_byPtr.front().relinked, hi = (DWORD)g_byPtr.back().relinked + 0x10000;
        int shown = 0;
        for (int i = 0; i < 256 && shown < 16 && !IsBadReadPtr(sp + i, 4); ++i)
            if (sp[i] >= lo && sp[i] < hi)
            {
                fprintf(g_log, "[loader]   stack+%03x %08lx", i * 4, sp[i]);
                describe((void*)sp[i]);
                ++shown;
            }
    }
    g_tracing = 1; dump_trace();
    fflush(g_log);
    return EXCEPTION_EXECUTE_HANDLER;
}

// Raw original code addresses in the decomp source (e.g. `reinterpret_cast<void*>(0x00aff9d0)`) carry no symbol, so
// they are never rebound. With DEP, jumping there raises an execute fault; when the address is a relinked function,
// continue there instead (the return address is already on the stack). Each address is logged once, to be fixed at
// the source (the wasm build has no such safety net).
std::vector<DWORD> g_redirected;

LONG CALLBACK redirect_original_code(EXCEPTION_POINTERS* ep)
{
    auto r = ep->ExceptionRecord;
    if (r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || r->NumberParameters < 2 || r->ExceptionInformation[0] != 8)
        return EXCEPTION_CONTINUE_SEARCH;
    DWORD target = (DWORD)r->ExceptionAddress;
    void* f = relinked(target);
    if (!f)
        return EXCEPTION_CONTINUE_SEARCH;
    if (std::find(g_redirected.begin(), g_redirected.end(), target) == g_redirected.end())
    {
        g_redirected.push_back(target);
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

int main(int argc, char** argv)
{
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
    const char* exe = getenv("FNV_EXE") ? getenv("FNV_EXE") : "E:\\Games\\Fallout New Vegas\\FalloutNV.exe";
    const char* dll = "fnv_game.dll";
    for (int i = 1; i + 1 < argc; ++i)
    {
        if (!strcmp(argv[i], "--exe"))
            exe = argv[++i];
        else if (!strcmp(argv[i], "--dll"))
            dll = argv[++i];
    }
    SetUnhandledExceptionFilter(on_crash);

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
    if (!getenv("FNV_KEEP_THREAD_ATTACH"))
        DisableThreadLibraryCalls(game);
    LoadLibraryA("uxtheme.dll");
    auto table = (const AddrEntry*)GetProcAddress(game, "fnv_addr_table");
    if (!table)
        return fail("fnv_addr_table export missing");
    for (const AddrEntry* e = table; e->original; ++e)
        g_table.push_back(*e);
    g_byPtr = g_table;
    std::sort(g_byPtr.begin(), g_byPtr.end(), [](const AddrEntry& a, const AddrEntry& b) { return a.relinked < b.relinked; });
    fprintf(g_log, "[loader] %s at %p, %zu relinked functions\n", dllPath, (void*)game, g_table.size());
    AddVectoredExceptionHandler(1, redirect_original_code);
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
    size_t toFunc = 0, toThunk = 0;
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
                continue;
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
    fprintf(g_log, "[loader] calling WinMain\n");
    fflush(g_log);
    if (g_watchdogStart) SetEvent(g_watchdogStart);
    int rc = winMain(GetModuleHandleA(nullptr), nullptr, GetCommandLineA(), SW_SHOWDEFAULT);
    fprintf(g_log, "[loader] WinMain returned %d\n", rc);
    return rc;
}
