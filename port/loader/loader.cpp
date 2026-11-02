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

extern "C" void __cdecl loader_unbound(DWORD original)
{
    fprintf(g_log, "[loader] call into unbound original code at 0x%08lx\n", original);
    fflush(g_log);
    ExitProcess(0xF17D0000u | (original & 0xffff));
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
    auto it = std::upper_bound(g_byPtr.begin(), g_byPtr.end(), ip,
                               [](void* p, const AddrEntry& e) { return p < e.relinked; });
    if (it != g_byPtr.begin())
    {
        --it;
        fprintf(g_log, "[loader]   in relinked fnv_f_%08lx + 0x%lx\n", it->original,
                (DWORD)((unsigned char*)ip - (unsigned char*)it->relinked));
    }
}

LONG WINAPI on_crash(EXCEPTION_POINTERS* ep)
{
    auto r = ep->ExceptionRecord;
    fprintf(g_log, "[loader] exception 0x%08lx at %p (access %s 0x%08lx)\n", r->ExceptionCode, r->ExceptionAddress,
            r->NumberParameters >= 2 ? (r->ExceptionInformation[0] ? "write" : "read") : "-",
            r->NumberParameters >= 2 ? (DWORD)r->ExceptionInformation[1] : 0);
    describe(r->ExceptionAddress);
    DWORD* ebp = (DWORD*)ep->ContextRecord->Ebp;
    for (int i = 0; i < 12 && ebp && !IsBadReadPtr(ebp, 8); ++i, ebp = (DWORD*)ebp[0])
    {
        fprintf(g_log, "[loader]   frame %d ret %08lx", i, ebp[1]);
        describe((void*)ebp[1]);
        fputc('\n', g_log);
    }
    fflush(g_log);
    return EXCEPTION_EXECUTE_HANDLER;
}

int fail(const char* what)
{
    fprintf(g_log, "[loader] error: %s (GetLastError %lu)\n", what, GetLastError());
    return 1;
}
} // namespace

int main(int argc, char** argv)
{
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
    strcpy_s(gameDir, exe);
    if (char* slash = strrchr(gameDir, '\\'))
        *slash = 0;
    SetCurrentDirectoryA(gameDir);
    SetDllDirectoryA(gameDir);
    HMODULE game = LoadLibraryA(dllPath);
    if (!game)
        return fail("cannot load fnv_game.dll");
    auto table = (const AddrEntry*)GetProcAddress(game, "fnv_addr_table");
    if (!table)
        return fail("fnv_addr_table export missing");
    for (const AddrEntry* e = table; e->original; ++e)
        g_table.push_back(*e);
    g_byPtr = g_table;
    std::sort(g_byPtr.begin(), g_byPtr.end(), [](const AddrEntry& a, const AddrEntry& b) { return a.relinked < b.relinked; });
    fprintf(g_log, "[loader] %s at %p, %zu relinked functions\n", dllPath, (void*)game, g_table.size());

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

    // 5. original C++ initializers, in their original order
    size_t ran = 0, skipped = 0;
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
        f();
        ++ran;
    }
    fprintf(g_log, "[loader] initializers: %zu ran, %zu skipped (not relinked)\n", ran, skipped);
    fflush(g_log);

    // 6. WinMain
    auto winMain = (int(__stdcall*)(HINSTANCE, HINSTANCE, LPSTR, int))relinked(kWinMain);
    if (!winMain)
        return fail("WinMain not relinked");
    fprintf(g_log, "[loader] calling WinMain\n");
    fflush(g_log);
    int rc = winMain(GetModuleHandleA(nullptr), nullptr, GetCommandLineA(), SW_SHOWDEFAULT);
    fprintf(g_log, "[loader] WinMain returned %d\n", rc);
    return rc;
}
