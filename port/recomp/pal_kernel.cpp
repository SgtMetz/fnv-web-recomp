// PAL kernel group: handles and waitable objects, threads, TLS, critical sections, interlocked operations, time,
// files and directories, INI files, heaps, code pages, process information. Standard C++ only, so it builds for the
// native test host and for Emscripten. Struct layouts are written out (Win32 32-bit offsets).
#include "pal.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>

#include "pal_fs.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>


namespace {
int strcasecmp_portable(const char* a, const char* b);
// ---- last error -------------------------------------------------------------------------------------------------
thread_local uint32_t t_last_error;
void set_err(uint32_t e) { t_last_error = e; }

// ---- handles ----------------------------------------------------------------------------------------------------
enum ObjType { OBJ_FREE, OBJ_EVENT, OBJ_SEMAPHORE, OBJ_MUTEX, OBJ_THREAD, OBJ_FILE, OBJ_FIND, OBJ_HEAP, OBJ_STD };

struct Obj
{
    ObjType type = OBJ_FREE;
    // waitable state
    bool signaled = false, manual = false;
    int32_t count = 0, max = 0;
    uint32_t owner = 0, recursion = 0;            // mutex
    uint32_t exit_code = 259;                     // thread (STILL_ACTIVE)
    int suspend = 0;
    uint32_t tid = 0;
    uintptr_t os_thread = 0;                      // fnv_host_spawn's token
    int priority = 0;
    // file
    PalFile* f = nullptr;
    std::string path;
    // find
    std::vector<std::string> found;            // full paths
    size_t next = 0;
    int std_id = 0;
};

std::mutex g_mx;                                  // all handle state
std::condition_variable g_cv;                     // any waitable changed
std::vector<Obj*> g_objs;
std::map<std::string, uint32_t> g_named;

uint32_t new_handle(Obj* o)
{
    std::lock_guard<std::mutex> lk(g_mx);
    for (size_t i = 0; i < g_objs.size(); ++i)
        if (!g_objs[i])
        {
            g_objs[i] = o;
            return kPalHandleBase + 4u * (uint32_t)i;
        }
    g_objs.push_back(o);
    return kPalHandleBase + 4u * (uint32_t)(g_objs.size() - 1);
}

Obj* get(uint32_t h)                              // caller holds g_mx or the object is not shared
{
    if (h < kPalHandleBase || (h - kPalHandleBase) % 4) return nullptr;
    size_t i = (h - kPalHandleBase) / 4;
    return i < g_objs.size() ? g_objs[i] : nullptr;
}

std::atomic<uint32_t> g_next_tid{0x100};
thread_local uint32_t t_tid;
uint32_t cur_tid()
{
    if (!t_tid) t_tid = g_next_tid.fetch_add(4);
    return t_tid;
}

// ---- waits ------------------------------------------------------------------------------------------------------
bool ready(Obj* o, uint32_t tid)
{
    switch (o->type)
    {
    case OBJ_EVENT: return o->signaled;
    case OBJ_SEMAPHORE: return o->count > 0;
    case OBJ_MUTEX: return o->recursion == 0 || o->owner == tid;
    case OBJ_THREAD: return o->signaled;
    default: return true;                         // files, std handles: always signaled
    }
}
void consume(Obj* o, uint32_t tid)
{
    if (o->type == OBJ_EVENT && !o->manual) o->signaled = false;
    else if (o->type == OBJ_SEMAPHORE) --o->count;
    else if (o->type == OBJ_MUTEX) { o->owner = tid; ++o->recursion; }
}

uint32_t wait_many(uint32_t n, const uint32_t* hs, bool all, uint32_t ms)
{
    uint32_t tid = cur_tid();
    std::unique_lock<std::mutex> lk(g_mx);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    for (;;)
    {
        if (all)
        {
            bool ok = true;
            for (uint32_t i = 0; i < n && ok; ++i)
            {
                Obj* o = get(hs[i]);
                ok = !o || ready(o, tid);
            }
            if (ok)
            {
                for (uint32_t i = 0; i < n; ++i)
                    if (Obj* o = get(hs[i])) consume(o, tid);
                return 0;
            }
        }
        else
            for (uint32_t i = 0; i < n; ++i)
            {
                Obj* o = get(hs[i]);
                if (!o || ready(o, tid))
                {
                    if (o) consume(o, tid);
                    return i;                         // WAIT_OBJECT_0 + i
                }
            }
        if (ms == 0) return 0x102;                    // WAIT_TIMEOUT
        if (ms == 0xffffffffu)
            g_cv.wait(lk);
        else if (g_cv.wait_until(lk, deadline) == std::cv_status::timeout)
            return 0x102;
    }
}

// ---- strings and paths ------------------------------------------------------------------------------------------
std::string host_path(const char* p)
{
    std::string s(p ? p : "");
#ifndef _WIN32
    for (char& c : s)
        if (c == '\\') c = '/';
#endif
    return s;
}

bool wild_match(const char* pat, const char* str)    // * and ?, case-insensitive (FindFirstFile)
{
    if (!*pat) return !*str;
    if (*pat == '*')
    {
        if (!strcmp(pat, "*") || !strcmp(pat, "*.*")) return true;
        for (const char* s = str;; ++s)
        {
            if (wild_match(pat + 1, s)) return true;
            if (!*s) return false;
        }
    }
    if (!*str) return false;
    if (*pat != '?' && tolower((unsigned char)*pat) != tolower((unsigned char)*str)) return false;
    return wild_match(pat + 1, str + 1);
}

uint64_t to_filetime(int64_t unix_seconds) { return (uint64_t)(unix_seconds + 11644473600LL) * 10000000ull; }

void put_filetime(uint32_t p, uint64_t ft)
{
    if (p) { M32(p) = (uint32_t)ft; M32(p + 4) = (uint32_t)(ft >> 32); }
}

bool stat_path(const std::string& p, PalStat& st) { return pal_fs_stat(p.c_str(), &st); }

void fill_find(uint32_t d, const std::string& full)
{
    memset((void*)(uintptr_t)d, 0, 320);
    PalStat st;
    if (!stat_path(full, st)) return;
    M32(d) = st.dir ? 0x10u : 0x20u;
    uint64_t ft = to_filetime(st.mtime);
    put_filetime(d + 4, ft);
    put_filetime(d + 12, ft);
    put_filetime(d + 20, ft);
    uint64_t size = st.dir ? 0 : st.size;
    M32(d + 28) = (uint32_t)(size >> 32);
    M32(d + 32) = (uint32_t)size;
    size_t slash = full.find_last_of("\\/");
    std::string name = slash == std::string::npos ? full : full.substr(slash + 1);
    strncpy(gbuf(d + 44), name.c_str(), 259);
}

// ---- INI files --------------------------------------------------------------------------------------------------
struct Ini
{
    int64_t mtime = -1;
    std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>> sections;
};
std::mutex g_ini_mx;
std::unordered_map<std::string, Ini> g_ini;

std::string lower(std::string s)
{
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return s;
}
std::string trim(const std::string& s)
{
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

const Ini& load_ini(const std::string& path)
{
    PalStat st;
    int64_t mt = stat_path(path, st) ? st.mtime : -2;
    Ini& ini = g_ini[lower(path)];
    if (ini.mtime == mt) return ini;
    ini = Ini();
    ini.mtime = mt;
    PalFile* f = pal_fs_open(path.c_str(), "rb");
    if (!f) return ini;
    std::string text;
    char chunk[65536];
    for (size_t n; (n = pal_fs_read(chunk, sizeof chunk, f)) > 0;) text.append(chunk, n);
    pal_fs_close(f);
    size_t at = 0;
    while (at < text.size())
    {
        size_t eol = text.find('\n', at);
        std::string raw = text.substr(at, eol == std::string::npos ? std::string::npos : eol - at);
        at = eol == std::string::npos ? text.size() : eol + 1;
        const char* line = raw.c_str();
        std::string l = trim(line);
        if (l.empty() || l[0] == ';') continue;
        if (l[0] == '[')
        {
            size_t e = l.find(']');
            ini.sections.push_back({trim(l.substr(1, e == std::string::npos ? std::string::npos : e - 1)), {}});
            continue;
        }
        size_t eq = l.find('=');
        if (eq == std::string::npos || ini.sections.empty()) continue;
        ini.sections.back().second.push_back({trim(l.substr(0, eq)), trim(l.substr(eq + 1))});
    }
    return ini;
}

// copies a double-null list or a value the way GetPrivateProfileString does; returns the count it reports
uint32_t ini_copy(char* out, uint32_t size, const std::vector<std::string>& items, bool list)
{
    if (!size) return 0;
    if (!list)
    {
        const std::string& v = items.empty() ? std::string() : items[0];
        uint32_t n = (uint32_t)std::min<size_t>(v.size(), size - 1);
        memcpy(out, v.data(), n);
        out[n] = 0;
        return n;
    }
    uint32_t pos = 0;
    for (const std::string& it : items)
    {
        if (pos + it.size() + 2 > size) { if (size >= 2) { out[size - 2] = out[size - 1] = 0; } return size - 2; }
        memcpy(out + pos, it.c_str(), it.size() + 1);
        pos += (uint32_t)it.size() + 1;
    }
    out[pos] = 0;
    return pos;
}

bool ini_lookup(const char* file, const char* app, const char* key, std::vector<std::string>& out)
{
    std::lock_guard<std::mutex> lk(g_ini_mx);
    const Ini& ini = load_ini(host_path(file));
    if (!app)
    {
        for (auto& s : ini.sections) out.push_back(s.first);
        return true;
    }
    for (auto& s : ini.sections)
        if (!strcasecmp_portable(s.first.c_str(), app))
        {
            if (!key)
            {
                for (auto& kv : s.second) out.push_back(kv.first);
                return true;
            }
            for (auto& kv : s.second)
                if (!strcasecmp_portable(kv.first.c_str(), key))
                {
                    std::string v = kv.second;
                    if (v.size() >= 2 && (v[0] == '"' || v[0] == '\'') && v.back() == v[0]) v = v.substr(1, v.size() - 2);
                    out.push_back(v);
                    return true;
                }
        }
    return false;
}

// ---- heaps: size-prefixed blocks over malloc ---------------------------------------------------------------------
const uint32_t kHeapHeader = 16;
uint32_t heap_alloc(uint32_t size, bool zero)
{
    uint8_t* p = (uint8_t*)(zero ? calloc(1, size + kHeapHeader) : malloc(size + kHeapHeader));
    if (!p) return 0;
    memcpy(p, &size, 4);
    memcpy(p + 4, "FNVH", 4);
    return (uint32_t)(uintptr_t)(p + kHeapHeader);
}
bool heap_ours(uint32_t p) { return p && !memcmp((void*)(uintptr_t)(p - kHeapHeader + 4), "FNVH", 4); }
uint32_t heap_size(uint32_t p) { return M32(p - kHeapHeader); }
void heap_free(uint32_t p)
{
    if (heap_ours(p))
    {
        memset((void*)(uintptr_t)(p - kHeapHeader + 4), 0, 4);
        free((void*)(uintptr_t)(p - kHeapHeader));
    }
}

// ---- thread start -----------------------------------------------------------------------------------------------
thread_local jmp_buf* t_exit_jmp;
thread_local uint32_t t_exit_code;

std::chrono::steady_clock::time_point g_t0 = std::chrono::steady_clock::now();
// The clocks report an uptime of an hour plus the run time: the game compares tick counts against times it initialises
// to 0 (a near-zero GetTickCount let the loading-menu thread run ahead of start-up and crash on 0x126fac4)
const uint64_t kUptimeMs = 3600000;
uint32_t ticks() { return (uint32_t)(kUptimeMs + std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - g_t0).count()); }

} // namespace

namespace {
int strcasecmp_portable(const char* a, const char* b)
{
    for (;; ++a, ++b)
    {
        int ca = tolower((unsigned char)*a), cb = tolower((unsigned char)*b);
        if (ca != cb || !ca) return ca - cb;
    }
}

#define A(i) harg(s, (i))
#define RET(v, n) return hret(s, (uint32_t)(v), (n))

// ---- errors, process -----------------------------------------------------------------------------------------
void k_GetLastError(FnvCpu& s) { RET(t_last_error, 0); }
void k_SetLastError(FnvCpu& s) { t_last_error = A(0); RET(0, 1); }
void k_GetCurrentThreadId(FnvCpu& s) { RET(cur_tid(), 0); }
void k_GetCurrentProcessId(FnvCpu& s) { RET(0x1234, 0); }
void k_GetCurrentThread(FnvCpu& s) { RET(0xfffffffeu, 0); }
void k_GetCurrentProcess(FnvCpu& s) { RET(0xffffffffu, 0); }
void k_IsDebuggerPresent(FnvCpu& s) { RET(0, 0); }

// ---- interlocked --------------------------------------------------------------------------------------------------
uint32_t* gp(uint32_t a) { return (uint32_t*)(uintptr_t)a; }
void k_InterlockedIncrement(FnvCpu& s) { RET(__atomic_add_fetch(gp(A(0)), 1u, __ATOMIC_SEQ_CST), 1); }
void k_InterlockedDecrement(FnvCpu& s) { RET(__atomic_sub_fetch(gp(A(0)), 1u, __ATOMIC_SEQ_CST), 1); }
void k_InterlockedExchange(FnvCpu& s) { RET(__atomic_exchange_n(gp(A(0)), A(1), __ATOMIC_SEQ_CST), 2); }
void k_InterlockedExchangeAdd(FnvCpu& s) { RET(__atomic_fetch_add(gp(A(0)), A(1), __ATOMIC_SEQ_CST), 2); }
void k_InterlockedCompareExchange(FnvCpu& s)
{
    uint32_t expected = A(2);
    __atomic_compare_exchange_n(gp(A(0)), &expected, A(1), false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    RET(expected, 3);                                 // the initial value, whether or not it was replaced
}

// ---- critical sections: DebugInfo (offset 0) holds the host mutex --------------------------------------------------
std::mutex g_cs_init;
std::recursive_mutex* cs_of(uint32_t cs)
{
    uint32_t m = __atomic_load_n(gp(cs), __ATOMIC_ACQUIRE);
    if (m && m != 0xffffffffu) return (std::recursive_mutex*)(uintptr_t)m;
    std::lock_guard<std::mutex> lk(g_cs_init);
    m = M32(cs);
    if (!m || m == 0xffffffffu)
    {
        m = (uint32_t)(uintptr_t) new std::recursive_mutex;
        __atomic_store_n(gp(cs), m, __ATOMIC_RELEASE);
    }
    return (std::recursive_mutex*)(uintptr_t)m;
}
void cs_init(uint32_t cs)
{
    memset((void*)(uintptr_t)cs, 0, 24);
    M32(cs + 12) = 0;                                 // OwningThread
    cs_of(cs);
}
void k_InitializeCriticalSection(FnvCpu& s) { cs_init(A(0)); RET(0, 1); }
void k_InitializeCriticalSectionAndSpinCount(FnvCpu& s) { cs_init(A(0)); RET(1, 2); }
void k_EnterCriticalSection(FnvCpu& s)
{
    uint32_t cs = A(0);
    cs_of(cs)->lock();
    M32(cs + 12) = cur_tid();
    ++M32(cs + 8);                                    // RecursionCount (the game never reads it, the CRT might)
    RET(0, 1);
}
void k_TryEnterCriticalSection(FnvCpu& s)
{
    uint32_t cs = A(0);
    if (!cs_of(cs)->try_lock()) RET(0, 1);
    M32(cs + 12) = cur_tid();
    ++M32(cs + 8);
    RET(1, 1);
}
void k_LeaveCriticalSection(FnvCpu& s)
{
    uint32_t cs = A(0);
    if (--M32(cs + 8) == 0) M32(cs + 12) = 0;
    cs_of(cs)->unlock();
    RET(0, 1);
}
void k_DeleteCriticalSection(FnvCpu& s)
{
    uint32_t cs = A(0), m = M32(cs);
    if (m && m != 0xffffffffu) delete (std::recursive_mutex*)(uintptr_t)m;
    M32(cs) = 0;
    RET(0, 1);
}

// ---- TLS ----------------------------------------------------------------------------------------------------------
const uint32_t kTlsSlots = 1088;
std::atomic<bool> g_tls_used[kTlsSlots];
thread_local uint32_t t_tls[kTlsSlots];
void k_TlsAlloc(FnvCpu& s)
{
    for (uint32_t i = 0; i < kTlsSlots; ++i)
    {
        bool f = false;
        if (g_tls_used[i].compare_exchange_strong(f, true)) RET(i, 0);
    }
    RET(0xffffffffu, 0);
}
void k_TlsFree(FnvCpu& s) { if (A(0) < kTlsSlots) g_tls_used[A(0)] = false; RET(1, 1); }
void k_TlsGetValue(FnvCpu& s) { set_err(0); RET(A(0) < kTlsSlots ? t_tls[A(0)] : 0, 1); }
void k_TlsSetValue(FnvCpu& s) { if (A(0) < kTlsSlots) t_tls[A(0)] = A(1); RET(1, 2); }

// ---- waitable objects -------------------------------------------------------------------------------------------
uint32_t named_or_new(const char* name, Obj* o, bool* existed)
{
    *existed = false;
    if (name && *name)
    {
        std::lock_guard<std::mutex> lk(g_mx);
        auto it = g_named.find(name);
        if (it != g_named.end()) { *existed = true; delete o; return it->second; }
    }
    uint32_t h = new_handle(o);
    if (name && *name)
    {
        std::lock_guard<std::mutex> lk(g_mx);
        g_named[name] = h;
    }
    return h;
}
void k_CreateEventA(FnvCpu& s)
{
    Obj* o = new Obj;
    o->type = OBJ_EVENT;
    o->manual = A(1) != 0;
    o->signaled = A(2) != 0;
    bool existed;
    uint32_t h = named_or_new(gstr(A(3)), o, &existed);
    set_err(existed ? 183 : 0);
    RET(h, 4);
}
void signal_event(uint32_t h, int how)               // 1 set, 0 reset, 2 pulse
{
    std::lock_guard<std::mutex> lk(g_mx);
    if (Obj* o = get(h))
        o->signaled = how != 0;
    g_cv.notify_all();
}
void k_SetEvent(FnvCpu& s) { signal_event(A(0), 1); RET(1, 1); }
void k_ResetEvent(FnvCpu& s) { signal_event(A(0), 0); RET(1, 1); }
void k_PulseEvent(FnvCpu& s) { signal_event(A(0), 1); signal_event(A(0), 0); RET(1, 1); }
void k_CreateSemaphoreA(FnvCpu& s)
{
    Obj* o = new Obj;
    o->type = OBJ_SEMAPHORE;
    o->count = (int32_t)A(1);
    o->max = (int32_t)A(2);
    bool existed;
    uint32_t h = named_or_new(gstr(A(3)), o, &existed);
    set_err(existed ? 183 : 0);
    RET(h, 4);
}
void k_ReleaseSemaphore(FnvCpu& s)
{
    std::lock_guard<std::mutex> lk(g_mx);
    Obj* o = get(A(0));
    if (!o) RET(0, 3);
    if (A(2)) M32(A(2)) = (uint32_t)o->count;
    if (o->count + (int32_t)A(1) > o->max) { set_err(298); RET(0, 3); }
    o->count += (int32_t)A(1);
    g_cv.notify_all();
    RET(1, 3);
}
void k_CreateMutexA(FnvCpu& s)
{
    Obj* o = new Obj;
    o->type = OBJ_MUTEX;
    if (A(1)) { o->owner = cur_tid(); o->recursion = 1; }
    bool existed;
    uint32_t h = named_or_new(gstr(A(2)), o, &existed);
    set_err(existed ? 183 : 0);
    RET(h, 3);
}
void k_ReleaseMutex(FnvCpu& s)
{
    std::lock_guard<std::mutex> lk(g_mx);
    Obj* o = get(A(0));
    if (!o || o->owner != cur_tid() || !o->recursion) RET(0, 1);
    if (--o->recursion == 0) o->owner = 0;
    g_cv.notify_all();
    RET(1, 1);
}
void k_WaitForSingleObject(FnvCpu& s)
{
    uint32_t h = A(0);
    bool ours;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        ours = get(h) != nullptr;
    }
    if (!ours)
    {
        int64_t r = fnv_host_wait_foreign(h, A(1));
        static int logged;
        if (logged++ < 20)
        {
            char buf[128];
            snprintf(buf, sizeof buf, "[pal] wait on a foreign handle %08x (%u ms) -> %08x from %08x%c", h, A(1),
                     (uint32_t)r, M32(s.esp), 10);
            fnv_host_log(buf);
        }
        if (r >= 0) RET((uint32_t)r, 2);
    }
    RET(wait_many(1, &h, false, A(1)), 2);
}
void k_WaitForSingleObjectEx(FnvCpu& s) { uint32_t h = A(0); RET(wait_many(1, &h, false, A(1)), 3); }
void k_WaitForMultipleObjects(FnvCpu& s)
{
    std::vector<uint32_t> hs(A(0));
    for (uint32_t i = 0; i < A(0); ++i) hs[i] = M32(A(1) + 4 * i);
    RET(wait_many(A(0), hs.data(), A(2) != 0, A(3)), 4);
}
void k_CloseHandle(FnvCpu& s)
{
    std::lock_guard<std::mutex> lk(g_mx);
    uint32_t h = A(0);
    Obj* o = get(h);
    if (!o) RET(fnv_host_close_foreign(h) ? 1 : 1, 1);
    if (o->type == OBJ_FILE && o->f) pal_fs_close(o->f);
    if (o->type == OBJ_THREAD && !o->signaled) RET(1, 1);   // the thread still uses its object: keep it until it ends
    for (auto it = g_named.begin(); it != g_named.end();)
        it = it->second == h ? g_named.erase(it) : std::next(it);
    g_objs[(h - kPalHandleBase) / 4] = nullptr;
    delete o;
    RET(1, 1);
}

// ---- threads --------------------------------------------------------------------------------------------------------
void k_CreateThread(FnvCpu& s)
{
    uint32_t fn = A(2), arg = A(3), flags = A(4), ptid = A(5);
    Obj* o = new Obj;
    o->type = OBJ_THREAD;
    o->suspend = (flags & 4) ? 1 : 0;              // CREATE_SUSPENDED
    o->tid = g_next_tid.fetch_add(4);
    uint32_t h = new_handle(o);
    struct Start { uint32_t h, fn, arg, tid; };
    o->os_thread = fnv_host_spawn([](void* p) {
        Start st = *(Start*)p;
        delete (Start*)p;
        uint32_t h = st.h, fn = st.fn, arg = st.arg;
        t_tid = st.tid;
        {
            std::unique_lock<std::mutex> lk(g_mx);
            g_cv.wait(lk, [h] { Obj* o = get(h); return !o || o->suspend <= 0; });
        }
        jmp_buf jb;
        t_exit_jmp = &jb;
        uint32_t code;
        if (setjmp(jb) == 0)
            code = fnv_host_thread_main(fn, arg);
        else
            code = t_exit_code;                     // ExitThread
        std::lock_guard<std::mutex> lk(g_mx);
        if (Obj* o = get(h)) { o->exit_code = code; o->signaled = true; }
        g_cv.notify_all();
    }, new Start{h, fn, arg, o->tid});
    if (ptid) M32(ptid) = o->tid;
    RET(h, 6);
}
void k_ResumeThread(FnvCpu& s)
{
    std::lock_guard<std::mutex> lk(g_mx);
    Obj* o = get(A(0));
    if (!o) RET(0xffffffffu, 1);
    int prev = o->suspend;
    if (o->suspend > 0) --o->suspend;
    g_cv.notify_all();
    RET(prev, 1);
}
void k_SuspendThread(FnvCpu& s) { RET(0, 1); }      // not used for real by the game (logged by a trap if it matters)
void k_ExitThread(FnvCpu& s)
{
    t_exit_code = A(0);
    if (t_exit_jmp) longjmp(*t_exit_jmp, 1);
    fnv_rtrap(s, 0, "ExitThread on a thread the PAL did not start");
}
void k_GetExitCodeThread(FnvCpu& s)
{
    std::lock_guard<std::mutex> lk(g_mx);
    Obj* o = get(A(0));
    if (A(1)) M32(A(1)) = o ? o->exit_code : 0;
    RET(o != nullptr, 2);
}
void k_true2(FnvCpu& s) { RET(1, 2); }               // SetThreadAffinityMask-like no-ops
// Priorities matter: the loading-menu thread lowers its own priority and polls state the main thread is still
// creating (0x126fac4); at equal priority it got there first
void k_SetThreadPriority(FnvCpu& s)
{
    uint32_t h = A(0);
    int prio = (int)A(1);
    if (h == 0xfffffffeu) { fnv_host_set_priority(0, prio); RET(1, 2); }
    std::lock_guard<std::mutex> lk(g_mx);
    Obj* o = get(h);
    if (!o || o->type != OBJ_THREAD) RET(0, 2);
    o->priority = prio;
    if (o->os_thread) fnv_host_set_priority(o->os_thread, prio);
    RET(1, 2);
}
void k_GetThreadPriorityReal(FnvCpu& s)
{
    std::lock_guard<std::mutex> lk(g_mx);
    Obj* o = get(A(0));
    RET(o ? (uint32_t)o->priority : 0u, 1);
}
void k_true1(FnvCpu& s) { RET(1, 1); }
void k_SetThreadIdealProcessor(FnvCpu& s) { RET(0, 2); }
void k_GetThreadPriority(FnvCpu& s) { RET(0, 1); }
void k_Sleep(FnvCpu& s)
{
    if (A(0) == 0) std::this_thread::yield();
    else std::this_thread::sleep_for(std::chrono::milliseconds(A(0)));
    RET(0, 1);
}
void k_SleepEx(FnvCpu& s) { std::this_thread::sleep_for(std::chrono::milliseconds(A(0))); RET(0, 2); }
void k_SwitchToThread(FnvCpu& s) { std::this_thread::yield(); RET(1, 0); }

// ---- time -----------------------------------------------------------------------------------------------------------
void k_GetTickCount(FnvCpu& s) { RET(ticks(), 0); }
void k_timeGetTime(FnvCpu& s) { RET(ticks(), 0); }
void k_QueryPerformanceCounter(FnvCpu& s)
{
    uint64_t t = kUptimeMs * 1000 + (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - g_t0).count();
    M32(A(0)) = (uint32_t)t;
    M32(A(0) + 4) = (uint32_t)(t >> 32);
    RET(1, 1);
}
void k_QueryPerformanceFrequency(FnvCpu& s) { M32(A(0)) = 1000000; M32(A(0) + 4) = 0; RET(1, 1); }
uint64_t now_filetime()
{
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    return (uint64_t)us * 10ull + 11644473600ull * 10000000ull;
}
void k_GetSystemTimeAsFileTime(FnvCpu& s) { put_filetime(A(0), now_filetime()); RET(0, 1); }
void filetime_to_systemtime(uint64_t ft, uint32_t st)
{
    int64_t secs = (int64_t)(ft / 10000000ull) - 11644473600LL;
    uint32_t ms = (uint32_t)((ft / 10000ull) % 1000);
    int64_t days = secs / 86400, rem = secs % 86400;
    if (rem < 0) { rem += 86400; --days; }
    // civil from days (Howard Hinnant)
    int64_t z = days + 719468, era = (z >= 0 ? z : z - 146096) / 146097;
    uint32_t doe = (uint32_t)(z - era * 146097), yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = (int64_t)yoe + era * 400;
    uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    uint32_t d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9;
    y += m <= 2;
    uint16_t v[8] = {(uint16_t)y, (uint16_t)m, (uint16_t)((days + 4) % 7 < 0 ? (days + 4) % 7 + 7 : (days + 4) % 7),
                     (uint16_t)d, (uint16_t)(rem / 3600), (uint16_t)(rem / 60 % 60), (uint16_t)(rem % 60), (uint16_t)ms};
    memcpy((void*)(uintptr_t)st, v, 16);
}
void k_FileTimeToSystemTime(FnvCpu& s)
{
    uint64_t ft = M32(A(0)) | ((uint64_t)M32(A(0) + 4) << 32);
    filetime_to_systemtime(ft, A(1));
    RET(1, 2);
}
void k_FileTimeToLocalFileTime(FnvCpu& s) { M32(A(1)) = M32(A(0)); M32(A(1) + 4) = M32(A(0) + 4); RET(1, 2); }
void k_CompareFileTime(FnvCpu& s)
{
    uint64_t a = M32(A(0)) | ((uint64_t)M32(A(0) + 4) << 32), b = M32(A(1)) | ((uint64_t)M32(A(1) + 4) << 32);
    RET(a < b ? 0xffffffffu : a > b ? 1u : 0u, 2);
}
void k_GetSystemTime(FnvCpu& s) { filetime_to_systemtime(now_filetime(), A(0)); RET(0, 1); }
void k_GetLocalTime(FnvCpu& s) { filetime_to_systemtime(now_filetime(), A(0)); RET(0, 1); }
void k_GetTimeZoneInformation(FnvCpu& s) { memset((void*)(uintptr_t)A(0), 0, 172); RET(0, 1); }

// ---- files ----------------------------------------------------------------------------------------------------------
uint32_t g_std[3];
void k_GetStdHandle(FnvCpu& s)
{
    int i = (int)(-10 - (int32_t)A(0));
    if (i < 0 || i > 2) RET(0xffffffffu, 1);
    if (!g_std[i])
    {
        Obj* o = new Obj;
        o->type = OBJ_STD;
        o->std_id = i;
        g_std[i] = new_handle(o);
    }
    RET(g_std[i], 1);
}
void k_GetFileType(FnvCpu& s)
{
    std::lock_guard<std::mutex> lk(g_mx);
    Obj* o = get(A(0));
    RET(!o ? 0 : o->type == OBJ_FILE ? 1 : 2, 1);
}
thread_local std::string t_last_media;
void k_CreateFileA(FnvCpu& s)
{
    std::string p = host_path(gstr(A(0)));
    uint32_t access = A(1), disp = A(4);
    PalStat st;
    bool exists = stat_path(p, st);
    if (exists && st.dir) { set_err(5); RET(0xffffffffu, 7); }
    const char* mode = "rb";
    if (access & 0x40000000u)                          // GENERIC_WRITE
    {
        switch (disp)
        {
        case 1: if (exists) { set_err(80); RET(0xffffffffu, 7); } mode = "w+b"; break;   // CREATE_NEW
        case 2: mode = "w+b"; break;                                                    // CREATE_ALWAYS
        case 3: if (!exists) { set_err(2); RET(0xffffffffu, 7); } mode = "r+b"; break;  // OPEN_EXISTING
        case 4: mode = exists ? "r+b" : "w+b"; break;                                   // OPEN_ALWAYS
        case 5: if (!exists) { set_err(2); RET(0xffffffffu, 7); } mode = "w+b"; break;  // TRUNCATE_EXISTING
        }
    }
    else if (!exists || disp == 1 || disp == 2)
    {
        if (!exists && (disp == 2 || disp == 4 || disp == 1))
            mode = "w+b";
        else { set_err(2); RET(0xffffffffu, 7); }
    }
    PalFile* f = pal_fs_open(p.c_str(), mode);
    if (!f) { set_err(exists ? 32 : 3); RET(0xffffffffu, 7); }
    if (p.size() > 4 && !strcasecmp_portable(p.c_str() + p.size() - 4, ".mp3")) t_last_media = pal_fs_resolve(p.c_str());
    Obj* o = new Obj;
    o->type = OBJ_FILE;
    o->f = f;
    o->path = p;
    set_err(exists && (disp == 2 || disp == 4) ? 183 : 0);
    RET(new_handle(o), 7);
}
Obj* file_of(uint32_t h)
{
    std::lock_guard<std::mutex> lk(g_mx);
    Obj* o = get(h);
    return o && (o->type == OBJ_FILE || o->type == OBJ_STD) ? o : nullptr;
}
void k_ReadFile(FnvCpu& s)
{
    Obj* o = file_of(A(0));
    if (!o || !o->f) { if (A(3)) M32(A(3)) = 0; set_err(6); RET(0, 5); }
    size_t n = pal_fs_read(gbuf(A(1)), A(2), o->f);
    if (A(3)) M32(A(3)) = (uint32_t)n;
    RET(1, 5);
}
void k_WriteFile(FnvCpu& s)
{
    Obj* o = file_of(A(0));
    if (!o) { set_err(6); RET(0, 5); }
    size_t n = A(2);
    if (o->type == OBJ_STD)
        fnv_host_log(std::string(gstr(A(1)), n).c_str());
    else
        n = pal_fs_write(gstr(A(1)), n, o->f);
    if (A(3)) M32(A(3)) = (uint32_t)n;
    RET(1, 5);
}
void k_SetFilePointer(FnvCpu& s)
{
    Obj* o = file_of(A(0));
    if (!o || !o->f) { set_err(6); RET(0xffffffffu, 4); }
    int64_t off = (int32_t)A(1);
    if (A(2)) off = (int64_t)(((uint64_t)M32(A(2)) << 32) | A(1));
    int whence = A(3) == 0 ? SEEK_SET : A(3) == 1 ? SEEK_CUR : SEEK_END;
    if (pal_fs_seek(o->f, off, whence)) { set_err(131); RET(0xffffffffu, 4); }
    int64_t pos = pal_fs_tell(o->f);
    if (A(2)) M32(A(2)) = (uint32_t)((uint64_t)pos >> 32);
    set_err(0);
    RET((uint32_t)pos, 4);
}
void k_GetFileSize(FnvCpu& s)
{
    Obj* o = file_of(A(0));
    if (!o || !o->f) { set_err(6); RET(0xffffffffu, 2); }
    int64_t cur = pal_fs_tell(o->f);
    pal_fs_seek(o->f, 0, SEEK_END);
    int64_t size = pal_fs_tell(o->f);
    pal_fs_seek(o->f, cur, SEEK_SET);
    if (A(1)) M32(A(1)) = (uint32_t)((uint64_t)size >> 32);
    RET((uint32_t)size, 2);
}
void k_GetFileSizeEx(FnvCpu& s)
{
    Obj* o = file_of(A(0));
    if (!o || !o->f) { set_err(6); RET(0, 2); }
    int64_t cur = pal_fs_tell(o->f);
    pal_fs_seek(o->f, 0, SEEK_END);
    int64_t size = pal_fs_tell(o->f);
    pal_fs_seek(o->f, cur, SEEK_SET);
    M32(A(1)) = (uint32_t)size;
    M32(A(1) + 4) = (uint32_t)((uint64_t)size >> 32);
    RET(1, 2);
}
void k_FlushFileBuffers(FnvCpu& s) { if (Obj* o = file_of(A(0))) if (o->f) pal_fs_flush(o->f); RET(1, 1); }
void k_SetEndOfFile(FnvCpu& s) { RET(1, 1); }        // only used after full rewrites here
void k_GetFileTime(FnvCpu& s)
{
    Obj* o = file_of(A(0));
    PalStat st;
    if (!o || !stat_path(o->path, st)) { set_err(6); RET(0, 4); }
    uint64_t ft = to_filetime(st.mtime);
    put_filetime(A(1), ft);
    put_filetime(A(2), ft);
    put_filetime(A(3), ft);
    RET(1, 4);
}
void k_GetFileAttributesA(FnvCpu& s)
{
    PalStat st;
    if (!stat_path(host_path(gstr(A(0))), st)) { set_err(2); RET(0xffffffffu, 1); }
    RET(st.dir ? 0x10u : 0x20u, 1);
}
void k_CreateDirectoryA(FnvCpu& s)
{
    std::string p = host_path(gstr(A(0)));
    PalStat st;
    if (stat_path(p, st)) { set_err(183); RET(0, 2); }
    RET(pal_fs_mkdir(p.c_str()), 2);
}
void k_DeleteFileA(FnvCpu& s) { RET(pal_fs_remove(host_path(gstr(A(0))).c_str()), 1); }
void k_MoveFileA(FnvCpu& s) { RET(pal_fs_rename(host_path(gstr(A(0))).c_str(), host_path(gstr(A(1))).c_str()), 2); }
void k_FindFirstFileA(FnvCpu& s)
{
    std::string pat = host_path(gstr(A(0)));
    size_t slash = pat.find_last_of("\\/");
    std::string dir = slash == std::string::npos ? "." : pat.substr(0, slash), name = slash == std::string::npos ? pat : pat.substr(slash + 1);
    Obj* o = new Obj;
    o->type = OBJ_FIND;
    if (name.find_first_of("*?") == std::string::npos)
    {
        PalStat st;
        if (stat_path(pat, st)) o->found.push_back(pat);
    }
    else
    {
        std::vector<std::string> names;
        pal_fs_list(dir.c_str(), names);
        for (const std::string& n : names)
            if (wild_match(name.c_str(), n.c_str()))
                o->found.push_back(dir + "/" + n);
    }
    if (o->found.empty()) { delete o; set_err(2); RET(0xffffffffu, 2); }
    fill_find(A(1), o->found[0]);
    o->next = 1;
    RET(new_handle(o), 2);
}
void k_FindNextFileA(FnvCpu& s)
{
    Obj* o;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        o = get(A(0));
    }
    if (!o || o->type != OBJ_FIND || o->next >= o->found.size()) { set_err(18); RET(0, 2); }
    fill_find(A(1), o->found[o->next++]);
    RET(1, 2);
}
void k_FindClose(FnvCpu& s)
{
    std::lock_guard<std::mutex> lk(g_mx);
    uint32_t h = A(0);
    if (Obj* o = get(h)) { g_objs[(h - kPalHandleBase) / 4] = nullptr; delete o; }
    RET(1, 1);
}

// ---- INI ------------------------------------------------------------------------------------------------------------
void k_GetPrivateProfileStringA(FnvCpu& s)
{
    const char *app = gstr(A(0)), *key = gstr(A(1)), *def = gstr(A(2)), *file = gstr(A(5));
    char* out = gbuf(A(3));
    uint32_t size = A(4);
    std::vector<std::string> items;
    bool list = !app || !key;
    if (!file || !ini_lookup(file, app, key, items))
    {
        items.clear();
        if (!list) items.push_back(def ? trim(def) : std::string());
    }
    RET(ini_copy(out, size, items, list), 6);
}
void k_GetPrivateProfileIntA(FnvCpu& s)
{
    std::vector<std::string> items;
    if (!ini_lookup(gstr(A(3)), gstr(A(0)), gstr(A(1)), items) || items.empty()) RET(A(2), 4);
    RET((uint32_t)strtol(items[0].c_str(), nullptr, 0), 4);
}
void k_WritePrivateProfileStringA(FnvCpu& s) { RET(1, 4); }   // settings writes are dropped (private copy anyway)

// ---- heaps ------------------------------------------------------------------------------------------------------------
void k_HeapCreate(FnvCpu& s) { Obj* o = new Obj; o->type = OBJ_HEAP; RET(new_handle(o), 3); }
void k_GetProcessHeap(FnvCpu& s)
{
    static uint32_t h = [] { Obj* o = new Obj; o->type = OBJ_HEAP; return new_handle(o); }();
    RET(h, 0);
}
void k_HeapDestroy(FnvCpu& s) { RET(1, 1); }
void k_HeapAlloc(FnvCpu& s) { RET(heap_alloc(A(2), (A(1) & 8) != 0), 3); }
void k_HeapFree(FnvCpu& s) { heap_free(A(2)); RET(1, 3); }
void k_HeapSize(FnvCpu& s) { RET(heap_ours(A(2)) ? heap_size(A(2)) : 0xffffffffu, 3); }
void k_HeapReAlloc(FnvCpu& s)
{
    uint32_t p = A(2), size = A(3);
    if (!p) RET(heap_alloc(size, (A(1) & 8) != 0), 4);
    uint32_t old = heap_ours(p) ? heap_size(p) : 0;
    if (A(1) & 0x10) RET(size <= old ? p : 0, 4);       // HEAP_REALLOC_IN_PLACE_ONLY
    uint32_t q = heap_alloc(size, (A(1) & 8) != 0);
    if (!q) RET(0, 4);
    memcpy((void*)(uintptr_t)q, (void*)(uintptr_t)p, std::min(old, size));
    heap_free(p);
    RET(q, 4);
}
void k_HeapValidate(FnvCpu& s) { RET(1, 3); }
void k_LocalAlloc(FnvCpu& s) { RET(heap_alloc(A(1), (A(0) & 0x40) != 0), 2); }
void k_LocalFree(FnvCpu& s) { heap_free(A(0)); RET(0, 1); }
void k_GlobalAlloc(FnvCpu& s) { RET(heap_alloc(A(1), (A(0) & 0x40) != 0), 2); }
void k_GlobalFree(FnvCpu& s) { heap_free(A(0)); RET(0, 1); }

// ---- strings and code pages -----------------------------------------------------------------------------------------
void k_lstrcpyA(FnvCpu& s) { strcpy(gbuf(A(0)), gstr(A(1))); RET(A(0), 2); }
void k_lstrcatA(FnvCpu& s) { strcat(gbuf(A(0)), gstr(A(1))); RET(A(0), 2); }
void k_lstrlenA(FnvCpu& s) { RET(A(0) ? strlen(gstr(A(0))) : 0, 1); }
void k_lstrcmpiA(FnvCpu& s) { RET((uint32_t)strcasecmp_portable(gstr(A(0)), gstr(A(1))), 2); }
void k_GetACP(FnvCpu& s) { RET(1252, 0); }
void k_GetOEMCP(FnvCpu& s) { RET(437, 0); }
void k_IsValidCodePage(FnvCpu& s) { RET(1, 1); }
void k_GetCPInfo(FnvCpu& s)
{
    uint32_t p = A(1);
    memset((void*)(uintptr_t)p, 0, 20);
    M32(p) = 1;                                          // MaxCharSize
    M8(p + 4) = '?';                                     // DefaultChar
    RET(1, 2);
}
void k_MultiByteToWideChar(FnvCpu& s)
{
    const uint8_t* src = (const uint8_t*)gstr(A(2));
    int32_t n = (int32_t)A(3);
    if (n < 0) n = (int32_t)strlen((const char*)src) + 1;
    uint32_t dst = A(4), cap = A(5);
    if (!cap) RET(n, 6);
    int32_t k = std::min<int32_t>(n, (int32_t)cap);
    for (int32_t i = 0; i < k; ++i) *(uint16_t*)(uintptr_t)(dst + 2 * i) = src[i];
    RET(k, 6);
}
void k_WideCharToMultiByte(FnvCpu& s)
{
    const uint16_t* src = (const uint16_t*)(uintptr_t)A(2);
    int32_t n = (int32_t)A(3);
    if (n < 0) { n = 0; while (src[n]) ++n; ++n; }
    uint32_t dst = A(4), cap = A(5);
    if (!cap) RET(n, 8);
    int32_t k = std::min<int32_t>(n, (int32_t)cap);
    for (int32_t i = 0; i < k; ++i) M8(dst + i) = src[i] < 256 ? (uint8_t)src[i] : '?';
    RET(k, 8);
}
void k_LCMapStringW(FnvCpu& s)
{
    uint32_t flags = A(1);
    const uint16_t* src = (const uint16_t*)(uintptr_t)A(2);
    int32_t n = (int32_t)A(3);
    if (n < 0) { n = 0; while (src[n]) ++n; ++n; }
    uint32_t dst = A(4), cap = A(5);
    if (!cap) RET(n, 6);
    int32_t k = std::min<int32_t>(n, (int32_t)cap);
    for (int32_t i = 0; i < k; ++i)
    {
        uint16_t c = src[i];
        if ((flags & 0x100) && c < 128) c = (uint16_t)tolower(c);
        if ((flags & 0x200) && c < 128) c = (uint16_t)toupper(c);
        *(uint16_t*)(uintptr_t)(dst + 2 * i) = c;
    }
    RET(k, 6);
}
void k_GetStringTypeW(FnvCpu& s)
{
    const uint16_t* src = (const uint16_t*)(uintptr_t)A(1);
    int32_t n = (int32_t)A(2);
    if (n < 0) { n = 0; while (src[n]) ++n; ++n; }
    uint16_t* out = (uint16_t*)(uintptr_t)A(3);
    for (int32_t i = 0; i < n; ++i)
    {
        int c = src[i] < 256 ? src[i] : 0;
        uint16_t t = 0;
        if (isupper(c)) t |= 1;
        if (islower(c)) t |= 2;
        if (isdigit(c)) t |= 4;
        if (isspace(c)) t |= 8;
        if (ispunct(c)) t |= 0x10;
        if (iscntrl(c)) t |= 0x20;
        if (c == ' ' || c == '\t') t |= 0x40;
        if (isxdigit(c)) t |= 0x80;
        if (isalpha(c)) t |= 0x100;
        out[i] = t;
    }
    RET(1, 4);
}

// ---- process information --------------------------------------------------------------------------------------------
void k_GetCommandLineA(FnvCpu& s) { static char cl[] = "\"FalloutNV.exe\""; RET((uintptr_t)cl, 0); }
void k_GetCommandLineW(FnvCpu& s) { static uint16_t cl[] = {'F', 'a', 'l', 'l', 'o', 'u', 't', 'N', 'V', '.', 'e', 'x', 'e', 0}; RET((uintptr_t)cl, 0); }
void k_GetStartupInfoA(FnvCpu& s) { memset((void*)(uintptr_t)A(0), 0, 68); M32(A(0)) = 68; RET(0, 1); }
void k_GetEnvironmentStringsW(FnvCpu& s) { static uint16_t env[2] = {0, 0}; RET((uintptr_t)env, 0); }
void k_GetEnvironmentStringsA(FnvCpu& s) { static char env[2] = {0, 0}; RET((uintptr_t)env, 0); }
void k_GetEnvironmentVariableA(FnvCpu& s) { set_err(203); RET(0, 3); }
void k_SetHandleCount(FnvCpu& s) { RET(A(0), 1); }
void k_GetVersionExA(FnvCpu& s)
{
    uint32_t p = A(0);
    M32(p + 4) = 6; M32(p + 8) = 1; M32(p + 12) = 7601; M32(p + 16) = 2;   // Windows 7 SP1, NT platform
    strcpy(gbuf(p + 20), "Service Pack 1");
    RET(1, 1);
}
void k_GetVersion(FnvCpu& s) { RET(0x1db10106u, 0); }
void k_GetSystemInfo(FnvCpu& s)
{
    uint32_t p = A(0);
    memset((void*)(uintptr_t)p, 0, 36);
    M32(p + 4) = 4096;                                  // page size
    M32(p + 8) = 0x10000; M32(p + 12) = 0x7ffeffff;     // application address range
    M32(p + 16) = 0xf;                                  // active processor mask
    M32(p + 20) = 4;                                    // processors
    M32(p + 24) = 586;
    M32(p + 28) = 0x10000;                              // allocation granularity
    RET(0, 1);
}
void k_GlobalMemoryStatusEx(FnvCpu& s)
{
    uint32_t p = A(0);
    uint64_t v[7] = {4ull << 30, 3ull << 30, 8ull << 30, 6ull << 30, 2ull << 30, 1ull << 30, 0};
    M32(p + 4) = 25;
    memcpy((void*)(uintptr_t)(p + 8), v, sizeof v);
    RET(1, 1);
}
void k_GlobalMemoryStatus(FnvCpu& s)
{
    uint32_t p = A(0);
    uint32_t v[8] = {32, 25, 0x7fffffff, 0x60000000, 0x7fffffff, 0x60000000, 0x7ffe0000, 0x40000000};
    memcpy((void*)(uintptr_t)p, v, sizeof v);
    RET(0, 1);
}
void k_GetUserNameA(FnvCpu& s) { strcpy(gbuf(A(0)), "Player"); if (A(1)) M32(A(1)) = 7; RET(1, 2); }
void k_GetComputerNameA(FnvCpu& s) { strcpy(gbuf(A(0)), "PC"); if (A(1)) M32(A(1)) = 2; RET(1, 2); }
void k_GetCurrentDirectoryA(FnvCpu& s)
{
    std::string d = pal_fs_cwd();
    if (A(0) <= d.size()) RET(d.size() + 1, 2);
    strcpy(gbuf(A(1)), d.c_str());
    RET(d.size(), 2);
}
void k_RaiseException(FnvCpu& s)
{
    if (A(0) == 0x406D1388u) RET(0, 4);                   // SetThreadName for a debugger: nobody listens
    char buf[128];
    snprintf(buf, sizeof buf, "RaiseException(%08x): guest SEH dispatch is not implemented yet", A(0));
    fnv_rtrap(s, M32(s.esp), buf);
}
void k_OutputDebugStringA(FnvCpu& s) { fnv_host_log("[dbgstr] "); fnv_host_log(gstr(A(0))); RET(0, 1); }
void k_RegOpenKeyExA(FnvCpu& s) { RET(2, 5); }          // ERROR_FILE_NOT_FOUND: no registry
void k_RegQueryValueExA(FnvCpu& s) { RET(2, 6); }
void k_RegCloseKey(FnvCpu& s) { RET(0, 1); }
void k_RegCreateKeyExA(FnvCpu& s) { RET(5, 9); }
void k_RegSetValueExA(FnvCpu& s) { RET(5, 6); }

const PalEntry kKernel[] = {
    {"GetLastError", k_GetLastError}, {"SetLastError", k_SetLastError},
    {"GetCurrentThreadId", k_GetCurrentThreadId}, {"GetCurrentProcessId", k_GetCurrentProcessId},
    {"GetCurrentThread", k_GetCurrentThread}, {"GetCurrentProcess", k_GetCurrentProcess},
    {"IsDebuggerPresent", k_IsDebuggerPresent},
    {"InterlockedIncrement", k_InterlockedIncrement}, {"InterlockedDecrement", k_InterlockedDecrement},
    {"InterlockedExchange", k_InterlockedExchange}, {"InterlockedExchangeAdd", k_InterlockedExchangeAdd},
    {"InterlockedCompareExchange", k_InterlockedCompareExchange},
    {"InitializeCriticalSection", k_InitializeCriticalSection},
    {"InitializeCriticalSectionAndSpinCount", k_InitializeCriticalSectionAndSpinCount},
    {"EnterCriticalSection", k_EnterCriticalSection}, {"TryEnterCriticalSection", k_TryEnterCriticalSection},
    {"LeaveCriticalSection", k_LeaveCriticalSection}, {"DeleteCriticalSection", k_DeleteCriticalSection},
    {"TlsAlloc", k_TlsAlloc}, {"TlsFree", k_TlsFree}, {"TlsGetValue", k_TlsGetValue}, {"TlsSetValue", k_TlsSetValue},
    {"CreateEventA", k_CreateEventA}, {"SetEvent", k_SetEvent}, {"ResetEvent", k_ResetEvent}, {"PulseEvent", k_PulseEvent},
    {"CreateSemaphoreA", k_CreateSemaphoreA}, {"ReleaseSemaphore", k_ReleaseSemaphore},
    {"CreateMutexA", k_CreateMutexA}, {"ReleaseMutex", k_ReleaseMutex},
    {"WaitForSingleObject", k_WaitForSingleObject}, {"WaitForSingleObjectEx", k_WaitForSingleObjectEx},
    {"WaitForMultipleObjects", k_WaitForMultipleObjects}, {"CloseHandle", k_CloseHandle},
    {"CreateThread", k_CreateThread}, {"ResumeThread", k_ResumeThread}, {"SuspendThread", k_SuspendThread},
    {"ExitThread", k_ExitThread}, {"GetExitCodeThread", k_GetExitCodeThread},
    {"SetThreadPriority", k_SetThreadPriority}, {"SetThreadAffinityMask", k_true2}, {"SetThreadIdealProcessor", k_SetThreadIdealProcessor},
    {"GetThreadPriority", k_GetThreadPriorityReal}, {"SetPriorityClass", k_true2},
    {"Sleep", k_Sleep}, {"SleepEx", k_SleepEx}, {"SwitchToThread", k_SwitchToThread},
    {"GetTickCount", k_GetTickCount}, {"timeGetTime", k_timeGetTime},
    {"QueryPerformanceCounter", k_QueryPerformanceCounter}, {"QueryPerformanceFrequency", k_QueryPerformanceFrequency},
    {"GetSystemTimeAsFileTime", k_GetSystemTimeAsFileTime}, {"FileTimeToSystemTime", k_FileTimeToSystemTime},
    {"FileTimeToLocalFileTime", k_FileTimeToLocalFileTime}, {"CompareFileTime", k_CompareFileTime},
    {"GetSystemTime", k_GetSystemTime}, {"GetLocalTime", k_GetLocalTime}, {"GetTimeZoneInformation", k_GetTimeZoneInformation},
    {"GetStdHandle", k_GetStdHandle}, {"GetFileType", k_GetFileType}, {"CreateFileA", k_CreateFileA},
    {"ReadFile", k_ReadFile}, {"WriteFile", k_WriteFile}, {"SetFilePointer", k_SetFilePointer},
    {"GetFileSize", k_GetFileSize}, {"GetFileSizeEx", k_GetFileSizeEx}, {"FlushFileBuffers", k_FlushFileBuffers},
    {"SetEndOfFile", k_SetEndOfFile}, {"GetFileTime", k_GetFileTime}, {"GetFileAttributesA", k_GetFileAttributesA},
    {"CreateDirectoryA", k_CreateDirectoryA}, {"DeleteFileA", k_DeleteFileA}, {"MoveFileA", k_MoveFileA},
    {"FindFirstFileA", k_FindFirstFileA}, {"FindNextFileA", k_FindNextFileA}, {"FindClose", k_FindClose},
    {"GetPrivateProfileStringA", k_GetPrivateProfileStringA}, {"GetPrivateProfileIntA", k_GetPrivateProfileIntA},
    {"WritePrivateProfileStringA", k_WritePrivateProfileStringA},
    {"HeapCreate", k_HeapCreate}, {"GetProcessHeap", k_GetProcessHeap}, {"HeapDestroy", k_HeapDestroy},
    {"HeapAlloc", k_HeapAlloc}, {"HeapFree", k_HeapFree}, {"HeapSize", k_HeapSize}, {"HeapReAlloc", k_HeapReAlloc},
    {"HeapValidate", k_HeapValidate}, {"LocalAlloc", k_LocalAlloc}, {"LocalFree", k_LocalFree},
    {"GlobalAlloc", k_GlobalAlloc}, {"GlobalFree", k_GlobalFree},
    {"lstrcpyA", k_lstrcpyA}, {"lstrcatA", k_lstrcatA}, {"lstrlenA", k_lstrlenA}, {"lstrcmpiA", k_lstrcmpiA},
    {"GetACP", k_GetACP}, {"GetOEMCP", k_GetOEMCP}, {"IsValidCodePage", k_IsValidCodePage}, {"GetCPInfo", k_GetCPInfo},
    {"MultiByteToWideChar", k_MultiByteToWideChar}, {"WideCharToMultiByte", k_WideCharToMultiByte},
    {"LCMapStringW", k_LCMapStringW}, {"GetStringTypeW", k_GetStringTypeW},
    {"GetCommandLineA", k_GetCommandLineA}, {"GetCommandLineW", k_GetCommandLineW}, {"GetStartupInfoA", k_GetStartupInfoA},
    {"GetEnvironmentStringsW", k_GetEnvironmentStringsW}, {"GetEnvironmentStrings", k_GetEnvironmentStringsA},
    {"FreeEnvironmentStringsW", k_true1}, {"FreeEnvironmentStringsA", k_true1},
    {"GetEnvironmentVariableA", k_GetEnvironmentVariableA}, {"SetHandleCount", k_SetHandleCount},
    {"GetVersionExA", k_GetVersionExA}, {"GetVersion", k_GetVersion}, {"GetSystemInfo", k_GetSystemInfo},
    {"GlobalMemoryStatusEx", k_GlobalMemoryStatusEx}, {"GlobalMemoryStatus", k_GlobalMemoryStatus},
    {"GetUserNameA", k_GetUserNameA}, {"GetComputerNameA", k_GetComputerNameA},
    {"GetCurrentDirectoryA", k_GetCurrentDirectoryA}, {"RaiseException", k_RaiseException},
    {"OutputDebugStringA", k_OutputDebugStringA},
    {"RegOpenKeyExA", k_RegOpenKeyExA}, {"RegQueryValueExA", k_RegQueryValueExA}, {"RegCloseKey", k_RegCloseKey},
    {"RegCreateKeyExA", k_RegCreateKeyExA}, {"RegSetValueExA", k_RegSetValueExA},
    {nullptr, nullptr}};
} // namespace

const char* pal_media_last_open() { return t_last_media.c_str(); }
const PalEntry* pal_kernel_entries() { return kKernel; }
