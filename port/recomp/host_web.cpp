// Emscripten host for the whole-game recompiler, milestone 2 (docs/RECOMP.md): headless in node first, then the
// browser. There is no native code to escape to: every import is the PAL (pal_*.cpp), a web override below, or a
// generated stub (out/recomp/import_table.inc) that pops the right number of argument bytes and logs its first call.
//
// Memory: wasm32 linear memory is the guest address space. The player's exe is copied to its original addresses
// (0x00400000..), below everything Emscripten places (link with -sGLOBAL_BASE above the image end). Host functions get
// addresses above linear memory (0xF0000000..): they are dispatch keys, never dereferenced.
//
// usage (node): node fnv_web.js --exe <FalloutNV.exe> [--docs <folder>]
#include <emscripten.h>
#include <emscripten/stack.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <string>

#include "pal.h"
#include "pal_d3d9.h"
#include "pal_fs.h"
#include "pal_sound.h"

extern "C" char __global_base;                 // wasm-ld: start of Emscripten's static data

namespace {
struct ImportInfo { const char* dll; const char* name; const char* real; uint32_t arg_bytes; int cdecl_; };
const ImportInfo kImports[] = {
#include "../../out/recomp/import_table.inc"
};

FILE* g_log;
bool g_browser;                                  // in a browser (else node)
uint32_t g_image_lo, g_image_hi, g_entry;
std::string g_exe_path, g_docs;
uint32_t g_tls_start, g_tls_end, g_tls_zero, g_tls_index, g_tls_callbacks;
void logf(const char* fmt, ...);
uint32_t g_rdata_lo, g_rdata_hi;
std::string g_rdata_copy;
void report_rdata_changes()
{
    int n = 0;
    for (uint32_t a = g_rdata_lo; a + 4 <= g_rdata_hi && n < 24; a += 4)
    {
        uint32_t was, now = M32(a);
        memcpy(&was, g_rdata_copy.data() + (a - g_rdata_lo), 4);
        if (was != now)
        {
            logf("[web] .rdata changed at %08x: %08x -> %08x%c", a, was, now, 10);
            ++n;
        }
    }
    if (!n && !g_rdata_copy.empty()) logf("[web] .rdata unchanged%c", 10);
}
bool g_has_tls;

void logf(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log ? g_log : stderr, fmt, ap);
    va_end(ap);
    fflush(g_log ? g_log : stderr);
}

struct HostThread
{
    FnvCpu cpu;
    uint32_t teb, stack_lo, stack_hi;
};
thread_local HostThread* t_ht;
HostThread* g_main_ht;                          // the game's main thread, for fnv_dump_state after a wasm trap

uint32_t galloc(uint32_t size)
{
    void* p = aligned_alloc(65536, (size + 65535) & ~65535u);
    memset(p, 0, size);
    return (uint32_t)(uintptr_t)p;
}

HostThread* g_all_threads[64];                   // for fnv_dump_threads

HostThread* new_thread(uint32_t stack_size)
{
    // thread state in static storage, away from the heap the game allocates from (corruption hunting)
    static HostThread s_threads[64];                 // FnvCpu aligns the struct to 16
    static std::atomic<int> s_nthreads;
    int slot = s_nthreads++;
    HostThread* ht = slot < 64 ? &s_threads[slot] : (HostThread*)aligned_alloc(64, sizeof(HostThread));
    if (slot < 64) g_all_threads[slot] = ht;
    memset(ht, 0, sizeof(HostThread));
    ht->stack_lo = galloc(stack_size);
    ht->stack_hi = ht->stack_lo + stack_size;
    uint32_t teb = ht->teb = galloc(0x1000);
    M32(teb + 0x00) = 0xffffffffu;
    M32(teb + 0x04) = ht->stack_hi;
    M32(teb + 0x08) = ht->stack_lo;
    M32(teb + 0x18) = teb;
    M32(teb + 0x2c) = teb + 0x800;
    if (g_has_tls)
    {
        uint32_t len = g_tls_end - g_tls_start;
        uint32_t block = galloc(len + g_tls_zero + 16);
        memcpy((void*)(uintptr_t)block, (void*)(uintptr_t)g_tls_start, len);
        M32(teb + 0x800) = block;
    }
    ht->cpu.fs_base = teb;
    ht->cpu.esp = ht->stack_hi - 64;
    ht->cpu.host = ht;
    return ht;
}

void run_tls_callbacks(HostThread* ht, uint32_t reason)
{
    if (!g_has_tls || !g_tls_callbacks) return;
    for (uint32_t p = g_tls_callbacks; M32(p); p += 4)
    {
        uint32_t args[3] = {g_image_lo, reason, 0};
        fnv_guest_call(ht->cpu, M32(p), args, 3);
    }
}

// ---- generic stub for imports nobody implements yet --------------------------------------------------------------
const ImportInfo* g_stub_of[kHostFnMax];
std::atomic<bool> g_stub_seen[kHostFnMax];
void stub(FnvCpu& s)
{
    const ImportInfo* im = g_stub_of[fnv_host_current()];
    if (!g_stub_seen[fnv_host_current()].exchange(true))
        logf("[stub] %s!%s called (returns 0)\n", im->dll, im->real);
    s.eax = 0;
    s.esp += 4 + (im->cdecl_ ? 0 : im->arg_bytes);
}

// ---- web overrides -------------------------------------------------------------------------------------------------
void w_SHGetFolderPathA(FnvCpu& s)
{
    strcpy(gbuf(harg(s, 4)), g_docs.c_str());
    hret(s, 0, 5);
}
// Modules: every DLL name gets a fake handle; GetProcAddress answers with the PAL's host function of that name (any
// group), or 0 for what the PAL does not have (the CRT then takes its fallbacks, as for FlsAlloc/EncodePointer)
uint32_t fake_module(const char* name)
{
    if (!name) return g_image_lo;
    uint32_t h = 2166136261u;
    for (const char* p = name; *p; ++p) h = (h ^ (uint32_t)tolower((unsigned char)*p)) * 16777619u;
    return 0x7e000000u + ((h & 0xff) << 16);
}
void w_GetModuleHandleA(FnvCpu& s) { hret(s, fake_module(gstr(harg(s, 0))), 1); }
void w_GetModuleHandleW(FnvCpu& s) { hret(s, harg(s, 0) ? 0x7e010000u : g_image_lo, 1); }
void w_LoadLibraryA(FnvCpu& s)
{
    logf("[web] LoadLibraryA(%s)\n", gstr(harg(s, 0)));
    hret(s, fake_module(gstr(harg(s, 0))), 1);
}
void w_FreeLibrary(FnvCpu& s) { hret(s, 1, 1); }
uint32_t lookup_export(const char* name);
void w_GetProcAddress(FnvCpu& s)
{
    uint32_t name = harg(s, 1);
    if (name < 0x10000) { hret(s, 0, 2); return; }                       // by ordinal: none
    uint32_t v = lookup_export(gstr(name));
    logf("[web] GetProcAddress(%s) -> %08x\n", gstr(name), v);
    hret(s, v, 2);
}
void w_GetModuleFileNameA(FnvCpu& s)
{
    uint32_t n = harg(s, 2);
    strncpy(gbuf(harg(s, 1)), g_exe_path.c_str(), n);
    hret(s, (uint32_t)std::min<size_t>(g_exe_path.size(), n), 3);
}
void w_MessageBoxA(FnvCpu& s)
{
    logf("[web] MessageBoxA: %s | %s\n", gstr(harg(s, 2)), gstr(harg(s, 1)));
    hret(s, 1, 4);
}
void w_ExitProcess(FnvCpu& s)
{
    logf("[web] ExitProcess(%u)\n", harg(s, 0));
    exit((int)harg(s, 0));
}
// Memory. Blocks come from the C heap and are recorded here; a commit succeeds only inside a block we handed out (it
// is memory already), reservations at a fixed address fail (as they did natively: the game's heaps probe addresses and
// fall back), and VirtualQuery answers like Windows. A lenient commit-anywhere let the heap set-up write into
// Emscripten's own data and stack (nondeterministic crashes; found with tools/trace_diff.py).
std::mutex g_vm_mx;
std::map<uint32_t, uint32_t> g_vm;               // base -> size
bool vm_owned(uint32_t addr, uint32_t size, uint32_t* base_out = nullptr, uint32_t* size_out = nullptr)
{
    auto it = g_vm.upper_bound(addr);
    if (it == g_vm.begin()) return false;
    --it;
    if (addr < it->first || (uint64_t)addr + size > (uint64_t)it->first + it->second) return false;
    if (base_out) *base_out = it->first;
    if (size_out) *size_out = it->second;
    return true;
}
// Fixed-address reservations: the game's heaps probe 16 MB-aligned addresses until one succeeds (natively one does,
// in a free hole). A 16 MB-aligned area set aside at start-up answers them, so the game takes the same path.
uint32_t g_fixed_lo, g_fixed_hi;
const uint32_t kFixedArea = 1280u << 20;      // 80 slots of 16 MB: the game reserves ~40 pools at start-up, one per slot
bool vm_overlaps(uint32_t addr, uint32_t size)
{
    auto it = g_vm.lower_bound(addr);
    if (it != g_vm.end() && it->first < addr + size) return true;
    if (it != g_vm.begin() && (--it, it->first + it->second > addr)) return true;
    return false;
}
void w_VirtualAlloc(FnvCpu& s)
{
    uint32_t addr = harg(s, 0), size = harg(s, 1), type = harg(s, 2);
    std::lock_guard<std::mutex> lk(g_vm_mx);
    if (addr)
    {
        bool commit_only = (type & 0x1000) && !(type & 0x2000);
        if (commit_only)
        {
            hret(s, vm_owned(addr, size) ? addr : 0, 4);
            return;
        }
        uint32_t rs = (size + 65535) & ~65535u;
        if (addr >= g_fixed_lo && (uint64_t)addr + rs <= g_fixed_hi && !(addr & 0xffff) && !vm_overlaps(addr, rs))
        {
            g_vm[addr] = rs;                       // fresh linear memory is zero: no memset (untouched pages stay free)
            logf("[web] fixed reservation %08x +%u KB granted\n", addr, rs >> 10);
            hret(s, addr, 4);
            return;
        }
        static uint32_t last_failed_size;
        if (rs != last_failed_size)
            logf("[web] fixed reservation of %u KB at %08x refused (more probes may follow)\n", rs >> 10, addr);
        last_failed_size = rs;
        hret(s, 0, 4);
        return;
    }
    uint32_t p = galloc(size);
    if (p) g_vm[p] = (size + 65535) & ~65535u;
    hret(s, p, 4);
}
void w_VirtualFree(FnvCpu& s)
{
    std::lock_guard<std::mutex> lk(g_vm_mx);
    uint32_t addr = harg(s, 0);
    if ((harg(s, 2) & 0x8000) && g_vm.count(addr))   // MEM_RELEASE of a whole block
    {
        g_vm.erase(addr);
        if (addr < g_fixed_lo || addr >= g_fixed_hi) free((void*)(uintptr_t)addr);
    }
    hret(s, 1, 3);
}
void w_VirtualQuery(FnvCpu& s)
{
    std::lock_guard<std::mutex> lk(g_vm_mx);
    uint32_t addr = harg(s, 0), out = harg(s, 1), base, size;
    memset(gbuf(out), 0, 28);
    if (vm_owned(addr, 1, &base, &size))
    {
        M32(out) = addr & ~4095u; M32(out + 4) = base; M32(out + 8) = 4;
        M32(out + 12) = base + size - (addr & ~4095u); M32(out + 16) = 0x1000; M32(out + 20) = 4; M32(out + 24) = 0x20000;
    }
    else if (addr >= g_image_lo && addr < g_image_hi)
    {
        M32(out) = addr & ~4095u; M32(out + 4) = g_image_lo; M32(out + 8) = 0x80;
        M32(out + 12) = g_image_hi - (addr & ~4095u); M32(out + 16) = 0x1000; M32(out + 20) = 4; M32(out + 24) = 0x1000000;
    }
    else
    {                                             // everything else is "in use" by someone: never offer it as free
        M32(out) = addr & ~4095u; M32(out + 12) = 4096; M32(out + 16) = 0x2000; M32(out + 24) = 0x20000;
    }
    hret(s, 28, 3);
}

struct Override { const char* name; HostFn fn; };
const Override kWeb[] = {
    {"SHGetFolderPathA", w_SHGetFolderPathA}, {"GetModuleHandleA", w_GetModuleHandleA},
    {"GetModuleHandleW", w_GetModuleHandleW}, {"GetModuleFileNameA", w_GetModuleFileNameA},
    {"LoadLibraryA", w_LoadLibraryA}, {"FreeLibrary", w_FreeLibrary}, {"GetProcAddress", w_GetProcAddress},
    {"MessageBoxA", w_MessageBoxA}, {"ExitProcess", w_ExitProcess}, {"VirtualAlloc", w_VirtualAlloc},
    {"VirtualFree", w_VirtualFree}, {"VirtualQuery", w_VirtualQuery},
};

uint32_t lookup_export(const char* name)
{
    static std::mutex mx;
    static std::map<std::string, uint32_t> done;
    std::lock_guard<std::mutex> lk(mx);
    auto it = done.find(name);
    if (it != done.end()) return it->second;
    uint32_t v = 0;
    for (const PalEntry* grp : {pal_kernel_entries(), pal_user_entries(), pal_input_entries(), pal_sound_entries(),
                                pal_d3d9_entries(), pal_d3dx_entries()})
        for (const PalEntry* p = grp; p->name && !v; ++p)
            if (!strcmp(p->name, name)) v = fnv_host_register(p->name, p->fn);
    for (const Override& o : kWeb)
        if (!v && !strcmp(o.name, name)) v = fnv_host_register(o.name, o.fn);
    done[name] = v;
    return v;
}

const ImportInfo* import_info(const char* dll, const char* name)
{
    for (const ImportInfo& im : kImports)
        if (!strcasecmp(im.dll, dll) && !strcmp(im.name, name)) return &im;
    return nullptr;
}

// ---- mapping the exe -----------------------------------------------------------------------------------------------
bool map_exe(const char* path)
{
    PalFile* f = pal_fs_open(path, "rb");
    if (!f) { logf("[web] cannot open %s\n", path); return false; }
    std::string file;
    char chunk[1 << 16];
    for (size_t n; (n = pal_fs_read(chunk, sizeof chunk, f)) > 0;) file.append(chunk, n);
    pal_fs_close(f);
    const uint8_t* d = (const uint8_t*)file.data();
    uint32_t pe = *(uint32_t*)(d + 0x3c), opt = pe + 24;
    uint16_t nsec = *(uint16_t*)(d + pe + 6), optsize = *(uint16_t*)(d + pe + 20);
    uint32_t base = *(uint32_t*)(d + opt + 28), img = *(uint32_t*)(d + opt + 56), hdrs = *(uint32_t*)(d + opt + 60);
    if (base + img > (uint32_t)(uintptr_t)&__global_base)
    {
        logf("[web] the exe (%08x-%08x) overlaps Emscripten's data at %p: link with a higher GLOBAL_BASE\n", base,
             base + img, (void*)&__global_base);
        return false;
    }
    memset((void*)(uintptr_t)base, 0, img);
    memcpy((void*)(uintptr_t)base, d, hdrs);
    for (int i = 0; i < nsec; ++i)
    {
        const uint8_t* sh = d + opt + optsize + 40 * i;
        uint32_t vsize = *(uint32_t*)(sh + 8), rva = *(uint32_t*)(sh + 12), rsize = *(uint32_t*)(sh + 16), raw = *(uint32_t*)(sh + 20);
        memcpy((void*)(uintptr_t)(base + rva), d + raw, std::min(rsize, vsize ? vsize : rsize));
    }
    g_image_lo = base;
    g_image_hi = base + img;
    g_entry = base + *(uint32_t*)(d + opt + 16);

    uint32_t tls_rva = *(uint32_t*)(d + opt + 96 + 9 * 8);
    if (tls_rva)
    {
        uint32_t t = base + tls_rva;
        g_tls_start = M32(t); g_tls_end = M32(t + 4); g_tls_index = M32(t + 8); g_tls_callbacks = M32(t + 12);
        g_tls_zero = M32(t + 16);
        g_has_tls = true;
        M32(g_tls_index) = 0;
    }
    uint32_t imp = base + *(uint32_t*)(d + opt + 96 + 1 * 8);
    int n = 0, pal = 0, web = 0, stubs = 0;
    for (uint32_t desc = imp; M32(desc + 12); desc += 20)
    {
        const char* dll = gstr(base + M32(desc + 12));
        uint32_t names = base + (M32(desc) ? M32(desc) : M32(desc + 16)), slots = base + M32(desc + 16);
        for (int k = 0; M32(names + 4 * k); ++k, ++n)
        {
            uint32_t e = M32(names + 4 * k);
            char ord[16];
            const char* nm;
            if (e & 0x80000000u) { snprintf(ord, sizeof ord, "#%u", e & 0xffff); nm = ord; }
            else nm = gstr(base + e + 2);
            const ImportInfo* im = import_info(dll, nm);
            const char* real = im ? im->real : nm;
            uint32_t v = 0;
            for (const PalEntry* grp : {pal_kernel_entries(), pal_user_entries(), pal_input_entries(), pal_sound_entries(),
                                         pal_d3d9_entries(), pal_d3dx_entries()})
                for (const PalEntry* p = grp; p->name && !v; ++p)
                    if (!strcmp(p->name, real)) { v = fnv_host_register(p->name, p->fn); ++pal; }
            for (const Override& o : kWeb)
                if (!v && !strcmp(o.name, real)) { v = fnv_host_register(o.name, o.fn); ++web; }
            if (!v)
            {
                if (!im) { logf("[web] no argument size for %s!%s\n", dll, nm); return false; }
                v = fnv_host_register(im->real, stub);
                g_stub_of[(v - fnv_host_base()) / 16] = im;
                ++stubs;
            }
            M32(slots + 4 * k) = v;
        }
    }
    // keep a copy of .rdata (after the import table is filled): a stop reports any word that changed (corruption hunting)
    g_rdata_lo = 0x00fdf000u;
    g_rdata_hi = 0x01183000u;
    g_rdata_copy.assign((const char*)(uintptr_t)g_rdata_lo, g_rdata_hi - g_rdata_lo);
    logf("[web] exe mapped at %08x-%08x, entry %08x, %d imports: %d PAL, %d web, %d stubs; TLS %s\n", g_image_lo,
         g_image_hi, g_entry, n, pal, web, stubs, g_has_tls ? "yes" : "no");
    return true;
}

void* game_main(void*)
{
    t_ht = g_main_ht = new_thread(16u << 20);
    run_tls_callbacks(t_ht, 1);
    logf("[web] calling the original entry point %08x\n", g_entry);
    uint32_t r = fnv_guest_call(t_ht->cpu, g_entry, nullptr, 0);
    logf("[web] entry point returned %u\n", r);
    exit((int)r);
}
} // namespace

// ---- runtime and PAL hooks -------------------------------------------------------------------------------------------
uint32_t fnv_host_base() { return 0xf0000000u; }
void fnv_host_log(const char* text) { logf("%s", text); }
void fnv_host_unknown(FnvCpu& s, uint32_t addr) { fnv_rtrap(s, addr, "dispatch to an address with no function"); }
[[noreturn]] void fnv_host_fatal(FnvCpu& s, uint32_t addr, const char* what)
{
    report_rdata_changes();
    logf("[web] stopping: %s at %08x (guest esp %08x)\n", what, addr, s.esp);
    for (int i = 0; i < 16 && s.esp + 4u * i + 16u <= 0x80000000u; i += 4)
        logf("[web]   [esp+%02x] %08x %08x %08x %08x\n", 4 * i, M32(s.esp + 4 * i), M32(s.esp + 4 * i + 4),
             M32(s.esp + 4 * i + 8), M32(s.esp + 4 * i + 12));
    if (t_ht)
        for (uint32_t p = s.esp, shown = 0; p >= t_ht->stack_lo && p + 4 <= t_ht->stack_hi && shown < 24; p += 4)
        {
            uint32_t v = M32(p);
            if (v >= g_image_lo + 0x1005 && v < 0x00fdf000u && M8(v - 5) == 0xe8)
            {
                logf("[web]   guest stack +%04x: return to %08x\n", p - s.esp, v);
                ++shown;
            }
        }
    exit(3);
}
uint32_t fnv_host_thread_main(uint32_t fn, uint32_t arg)
{
    t_ht = new_thread(4u << 20);
    run_tls_callbacks(t_ht, 2);
    uint32_t args[1] = {arg};
    return fnv_guest_call(t_ht->cpu, fn, args, 1);
}
uintptr_t fnv_host_spawn(void (*fn)(void*), void* arg)
{
    struct P { void (*fn)(void*); void* arg; };
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 8u << 20);
    pthread_t th;
    pthread_create(&th, &at, [](void* p) -> void* {
        P q = *(P*)p;
        delete (P*)p;
        q.fn(q.arg);
        return nullptr;
    }, new P{fn, arg});
    pthread_detach(th);
    return 0;
}
void fnv_host_set_priority(uintptr_t, int) {}
int64_t fnv_host_wait_foreign(uint32_t, uint32_t) { return -1; }
bool fnv_host_close_foreign(uint32_t) { return false; }
uint32_t fnv_host_image_base() { return g_image_lo; }
const char* fnv_host_exe_path() { return g_exe_path.c_str(); }
uint64_t fnv_host_rdtsc(void)
{
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() * 3u;
}
void fnv_host_cpuid(int out[4], int leaf, int)
{
    memset(out, 0, 16);
    if (leaf == 0) { out[0] = 1; memcpy(&out[1], "Genu", 4); memcpy(&out[3], "ineI", 4); memcpy(&out[2], "ntel", 4); }
    else if (leaf == 1) { out[0] = 0x000006f2; out[3] = (1 << 0) | (1 << 4) | (1 << 15) | (1 << 23) | (1 << 25) | (1 << 26); }
}

// FNV_RING_DUMP=<file>: consume the D3D9 command ring here and write the packets to a file (renderer development:
// web/recomp/replay.html plays it back); FNV_RING_FRAMES limits it to that many presents
void start_ring_dump(const char* path)
{
    pal_d3d_enable_ring(64u << 20);
    static FILE* f = fopen(path, "wb");
    static int limit = getenv("FNV_RING_FRAMES") ? atoi(getenv("FNV_RING_FRAMES")) : 600;
    fnv_host_spawn([](void*) {
        D3dRing* r = pal_d3d_ring();
        int frames = 0;
        for (;;)
        {
            uint32_t rd = r->read, w = __atomic_load_n(&r->write, __ATOMIC_ACQUIRE);
            if (rd == w) { usleep(200); continue; }
            uint32_t op = M32(r->data + rd), len = M32(r->data + rd + 4);
            if (op == OP_WRAP) { __atomic_store_n(&r->read, 0u, __ATOMIC_RELEASE); continue; }
            if (f && frames < limit) fwrite((void*)(uintptr_t)(r->data + rd), 1, 8 + len, f);
            if (op == OP_PRESENT) __atomic_add_fetch(&r->frames_done, 1, __ATOMIC_RELEASE);
            if (op == OP_PRESENT && ++frames == limit && f) { fclose(f); f = nullptr; logf("[web] ring dump done%c", 10); }
            __atomic_store_n(&r->read, rd + 8 + len, __ATOMIC_RELEASE);
        }
    }, nullptr);
}

// the page's hooks (web/recomp/index.html): the command ring, and input as DirectInput sees it
extern "C" EMSCRIPTEN_KEEPALIVE uint32_t fnv_ring() { return (uint32_t)(uintptr_t)pal_d3d_ring(); }
extern "C" EMSCRIPTEN_KEEPALIVE uint32_t fnv_audio() { return (uint32_t)(uintptr_t)pal_sound_ring(); }
extern "C" EMSCRIPTEN_KEEPALIVE void fnv_key(int dik, int down) { pal_input_key(dik, down != 0); }
// debugging: Module.ccall('fnv_console', null, ['string'], ['player.additem f 100']) runs a console line
extern "C" EMSCRIPTEN_KEEPALIVE void fnv_console(const char* line) { pal_console_line(line); }
extern "C" EMSCRIPTEN_KEEPALIVE void fnv_mouse(int dx, int dy, int dz, int button, int down) { pal_input_mouse(dx, dy, dz, button, down != 0); }

int main(int argc, char** argv)
{
    g_browser = EM_ASM_INT({ return typeof process === 'undefined' || !process.versions || !process.versions.node ? 1 : 0; });
    const char* exe = nullptr;
    const char* docs = nullptr;
    for (int i = 1; i + 1 < argc; ++i)
    {
        if (!strcmp(argv[i], "--exe")) exe = argv[i + 1];
        if (!strcmp(argv[i], "--docs")) docs = argv[i + 1];
        if (!strcmp(argv[i], "--log")) g_log = fopen(argv[i + 1], "w");   // a file: node writes it synchronously
    }
    if (!g_log) g_log = stdout;
    if (!exe) { logf("usage: fnv_web --exe <FalloutNV.exe> [--docs <folder>]\n"); return 2; }
    g_exe_path = exe;
    for (char& c : g_exe_path) if (c == '\\') c = '/';
    g_docs = docs ? docs : "fnv_docs";
    logf("[web] %u recompiled functions; C stack %08x-%08x, static data from %p, heap from %p\n", fnv_function_count(),
         (uint32_t)emscripten_stack_get_end(), (uint32_t)emscripten_stack_get_base(), (void*)&__global_base, sbrk(0));
    if (!map_exe(g_exe_path.c_str())) return 1;
    g_fixed_lo = (uint32_t)(uintptr_t)aligned_alloc(16u << 20, kFixedArea);
    g_fixed_hi = g_fixed_lo + kFixedArea;
    logf("[web] fixed-address reservation area %08x-%08x\n", g_fixed_lo, g_fixed_hi);
    std::string dir = g_exe_path.substr(0, g_exe_path.find_last_of('/'));
    pal_fs_chdir(dir.c_str());
    if (g_browser)
    {
        pal_d3d_enable_ring(128u << 20);                    // web/recomp/renderer.js consumes it on the page's thread
        pal_sound_enable(16384, 4096);                      // web/recomp/audio-worklet.js plays it (~85 ms ahead)
        pal_dshow_enable_sink();                            // music: the page plays the tracks the graphs run
        pal_vorbis_enable_decoder();                        // voices: the page decodes Ogg Vorbis
    }
    else if (chdir(dir.c_str())) logf("[web] cannot enter %s\n", dir.c_str());
    if (const char* rd = getenv("FNV_RING_DUMP")) start_ring_dump(rd);
    if (const char* sc = getenv("FNV_INPUT_SCRIPT")) pal_input_script(sc);
    if (const char* st = getenv("FNV_START")) if (!strcmp(st, "quick")) pal_start_quick();
    if (const char* w = getenv("FNV_START_DELAY")) usleep(1000u * (unsigned)atoi(w));   // race test: let the pool load
    // node: the game's main thread runs on the runtime's main thread (blocking is allowed there), so a wasm trap
    // reaches the uncaughtException hook (pre-js) while the instance is intact and fnv_dump_state can report it
    game_main(nullptr);
    return 0;
}

// debugging (page console: Module._fnv_dump_threads()): where every game thread is, as return addresses on its stack
extern "C" EMSCRIPTEN_KEEPALIVE void fnv_dump_threads()
{
    for (int i = 0; i < 64; ++i)
    {
        HostThread* ht = g_all_threads[i];
        if (!ht) continue;
        FnvCpu& s = ht->cpu;
        std::string line = "[web] thread " + std::to_string(i) + ":";
        char b[16];
        for (uint32_t p = s.esp, shown = 0; p >= ht->stack_lo && p + 4 <= ht->stack_hi && shown < 14; p += 4)
        {
            uint32_t v = M32(p);
            if (v >= g_image_lo + 0x1005 && v < 0x00fdf000u && M8(v - 5) == 0xe8)
            {
                snprintf(b, sizeof b, " %08x", v);
                line += b;
                ++shown;
            }
        }
        logf("%s%c", line.c_str(), 10);
    }
}

extern "C" EMSCRIPTEN_KEEPALIVE void fnv_dump_state()
{
    report_rdata_changes();
    HostThread* ht = g_main_ht;
    if (!ht) return;
    FnvCpu& s = ht->cpu;
    logf("[web] state after the trap: eax=%08x ecx=%08x edx=%08x ebx=%08x esp=%08x ebp=%08x esi=%08x edi=%08x"
         " (guest stack %08x-%08x)%c", s.eax, s.ecx, s.edx, s.ebx, s.esp, s.ebp, s.esi, s.edi, ht->stack_lo, ht->stack_hi, 10);
    for (uint32_t p = s.esp, shown = 0; p >= ht->stack_lo && p + 4 <= ht->stack_hi && shown < 24; p += 4)
    {
        uint32_t v = M32(p);
        if (v >= g_image_lo + 0x1005 && v < 0x00fdf000u && M8(v - 5) == 0xe8)
        {
            logf("[web]   guest stack +%04x: return to %08x%c", p - s.esp, v, 10);
            ++shown;
        }
    }
}
