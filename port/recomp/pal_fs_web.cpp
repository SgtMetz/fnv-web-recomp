// pal_fs backend for the browser (pal_fs.h): synchronous requests from the game's worker threads to the web server
// (server/serve.mjs), which serves two roots: /game (the player's install, read-only) and /docs (the private copy of
// My Games\FalloutNV, writable). Built to stay usable with network latency:
//   - stat and list under /game come from one index of the install (/fs/index, fetched once per thread), not one
//     request each (start-up makes thousands);
//   - reads go in 1 MB blocks (256 KB in the phone version) through one block cache for all threads (in wasm
//     memory): a block another thread is already fetching is waited for, not fetched again;
//   - a read of an install file names the file's mtime (v=), so the browser's HTTP cache keeps it across sessions.
#include "pal.h"
#include "pal_fs.h"

#include <emscripten.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// one synchronous request, retried: a dropped or stalled connection (common on phones) must not kill or hang the game
// thread that asked. Each try has a timeout; a network error, a timeout or a server error (5xx, 408, 429) waits a
// little longer each time and tries again, and counts in g_pal_boot.retries (the loading bar shows it). Defined once
// per worker (fs_init below).
EM_JS(void, js_fs_init, (int mobile, uint32_t* retries), {
    // the phone version (FNV_ARCHIVES=mobile, web/recomp/index.html): /game/data/<name>.bsa is read from
    // /game/data/mobile/<name>.bsa when the index has one (tools/mobile_archives.py: smaller textures and sounds)
    globalThis.fnvMobile = !!mobile;
    globalThis.fnvRetries = retries;
    globalThis.fnvXhr = function (method, url, body, binary) {
        for (var attempt = 0; attempt < 40; attempt++) {
            var x = null;
            try {
                x = new XMLHttpRequest();
                x.open(method, url, false);
                if (binary) x.responseType = 'arraybuffer';
                x.timeout = 30000;
                x.send(body);
                var st = x.status;
                if (st != 0 && st < 500 && st != 408 && st != 429) return x;
            } catch (e) {
            }
            Atomics.add(HEAPU32, fnvRetries >>> 2, 1);
            if (attempt == 0 || attempt % 10 == 9)
                console.warn('[fs] ' + method + ' ' + url.substring(0, 160) + ': ' + (x && x.status ? 'status ' + x.status : 'no answer') + ', trying again');
            var until = Date.now() + Math.min(250 * (attempt + 1), 3000);
            while (Date.now() < until) {}
        }
        return { status: 0 };
    };
    // the install's index: keys are lowercased paths ("/game/data/falloutnv.esm"); f: [size, mtime], d: listings
    globalThis.fnvGameIndex = function () {
        if (globalThis.fnvIdx === undefined) {
            var x = fnvXhr('GET', '/fs/index', null, false);
            globalThis.fnvIdx = x.status == 200 ? JSON.parse(x.responseText) : null;
        }
        return globalThis.fnvIdx;
    };
    // index answer for a /game path: {file: [size, mtime]} / {dir: names} / {missing}, or null (ask the server)
    globalThis.fnvGameLookup = function (p) {
        // (no regex literals in EM_JS: its stringification breaks backslashes)
        var k = p.toLowerCase();
        while (k.length > 1 && k.charAt(k.length - 1) == '/') k = k.substring(0, k.length - 1);
        if (k != '/game' && k.indexOf('/game/') != 0) return null;
        var ix = fnvGameIndex();
        if (!ix) return null;
        if (fnvMobile && k.indexOf('/game/data/') == 0 && k.lastIndexOf('/') == 10 && k.substring(k.length - 4) == '.bsa'
            && ix.f['/game/data/mobile/' + k.substring(11)]) return { file: ix.f['/game/data/mobile/' + k.substring(11)], path: '/game/data/mobile/' + k.substring(11) };
        if (ix.f[k]) return { file: ix.f[k] };
        if (ix.d[k]) return { dir: ix.d[k] };
        return { missing: true };
    };
});

// Pointers reach EM_JS code as signed numbers, and memory is 3 GB: an address at or above 2 GB arrives negative, and
// HEAPU8.slice/set with it reads or writes the wrong place (slice counts from the end of memory). Every pointer used
// to index the heap here goes through `>>> 0` first. (UTF8ToString and stringToUTF8 already do it.) Before this,
// saves came out with their 4 KB CRT-buffered pieces zeroed (the buffer lives above 2 GB) and loading one never ended.
EM_JS(int, js_fs_stat, (const char* path, uint32_t* out), {
    var p = UTF8ToString(path);
    var o = (out >>> 0) >>> 2;
    var hit = fnvGameLookup(p);
    if (hit) {
        if (hit.missing) return 0;
        var size = hit.file ? hit.file[0] : 0;
        HEAPU32[o] = hit.dir ? 1 : 0;
        HEAPU32[o + 1] = size % 4294967296;
        HEAPU32[o + 2] = Math.floor(size / 4294967296);
        HEAPU32[o + 3] = hit.file ? hit.file[1] : 0;
        return 1;
    }
    var x = fnvXhr('GET', '/fs/stat?p=' + encodeURIComponent(p), null, false);
    if (x.status != 200) return 0;
    var j = JSON.parse(x.responseText);
    HEAPU32[o] = j.dir ? 1 : 0;
    HEAPU32[o + 1] = j.size % 4294967296;
    HEAPU32[o + 2] = Math.floor(j.size / 4294967296);
    HEAPU32[o + 3] = j.mtime;
    return 1;
});

// one block of a file, straight into dst (a buffer in the shared cache below); its length, or -1
EM_JS(int, js_fs_fetch, (const char* path, double off, int len, uint8_t* dst), {
    var p = UTF8ToString(path);
    var hit = fnvGameLookup(p);
    var ver = hit && hit.file ? '&v=' + hit.file[1] : '';
    if (hit && hit.path) p = hit.path;
    var x = fnvXhr('GET', '/fs/read?p=' + encodeURIComponent(p) + '&o=' + off + '&n=' + len + ver, null, true);
    if (x.status != 200) return -1;
    var data = new Uint8Array(x.response);
    var n = Math.min(len, data.length);
    HEAPU8.set(data.subarray(0, n), dst >>> 0);
    return n;
});

EM_JS(int, js_fs_write, (const char* path, double off, int len, const uint8_t* src), {
    var p = UTF8ToString(path);
    var x = fnvXhr('POST', '/fs/write?p=' + encodeURIComponent(p) + '&o=' + off, HEAPU8.slice(src >>> 0, (src >>> 0) + len), false);
    return x.status == 200 ? len : 0;
});

EM_JS(int, js_fs_list, (const char* path, char* out, int cap), {
    var p = UTF8ToString(path);
    var hit = fnvGameLookup(p);
    var names;
    if (hit) {
        if (!hit.dir) return -1;
        names = hit.dir;
    } else {
        var x = fnvXhr('GET', '/fs/list?p=' + encodeURIComponent(p), null, false);
        if (x.status != 200) return -1;
        names = JSON.parse(x.responseText);
    }
    var s = names.join('\0') + '\0';
    var need = lengthBytesUTF8(s) + 1;
    if (need > cap) return need;
    stringToUTF8(s, out, cap);
    return need;
});

EM_JS(int, js_fs_op, (const char* op, const char* a, const char* b), {
    var q = '/fs/' + UTF8ToString(op) + '?p=' + encodeURIComponent(UTF8ToString(a));
    if (b) q += '&q=' + encodeURIComponent(UTF8ToString(b));
    var x = fnvXhr('POST', q, null, false);
    return x.status == 200 ? 1 : 0;
});

struct PalFile { std::string path; int64_t pos, size; bool writable; };

namespace {
std::mutex g_cwd_mx;
std::string g_cwd = "/game";

// Windows path as the game writes it -> server path ("/game/...", "/docs/...")
thread_local bool t_init;
// the phone version (FNV_ARCHIVES=mobile): smaller archives, and smaller blocks (below), so the scattered small reads
// of start-up fetch less that nobody asked for
bool fs_mobile()
{
    static const bool m = [] { const char* a = getenv("FNV_ARCHIVES"); return a && !strcmp(a, "mobile"); }();
    return m;
}
void fs_init()
{
    if (!t_init) { js_fs_init(fs_mobile(), (uint32_t*)&g_pal_boot.retries); t_init = true; }
}

std::string norm(const char* p)
{
    fs_init();
    std::string s(p ? p : "");
    for (char& c : s)
        if (c == '\\') c = '/';
    if (s.size() >= 2 && s[1] == ':') s = s.substr(2);         // a drive letter means nothing here
    if (s.empty() || s[0] != '/')
    {
        std::lock_guard<std::mutex> lk(g_cwd_mx);
        s = g_cwd + "/" + s;
    }
    std::vector<std::string> parts;
    size_t i = 0;
    while (i <= s.size())
    {
        size_t j = s.find('/', i);
        if (j == std::string::npos) j = s.size();
        std::string part = s.substr(i, j - i);
        if (part == "..") { if (!parts.empty()) parts.pop_back(); }
        else if (!part.empty() && part != ".") parts.push_back(part);
        i = j + 1;
    }
    std::string out;
    for (auto& part : parts) out += "/" + part;
    return out.empty() ? "/" : out;
}
} // namespace

// ---- the shared block cache ------------------------------------------------------------------------------------
namespace {
// 1 MB, or 256 KB in the phone version
int64_t block_size() { static const int64_t b = fs_mobile() ? 256 << 10 : 1 << 20; return b; }
constexpr size_t kCacheMax = 256u << 20, kCacheLow = 192u << 20;
struct Block { std::vector<uint8_t> data; bool ready = false, failed = false; uint64_t used = 0; };
std::mutex g_bc_mx;
std::condition_variable g_bc_cv;
std::unordered_map<std::string, std::shared_ptr<Block>> g_blocks;
size_t g_bc_bytes;
uint64_t g_bc_tick;

void bc_evict_locked()
{
    if (g_bc_bytes <= kCacheMax) return;
    std::vector<std::pair<uint64_t, std::string>> order;
    for (auto& kv : g_blocks)
        if (kv.second->ready) order.emplace_back(kv.second->used, kv.first);
    std::sort(order.begin(), order.end());
    for (auto& o : order)
    {
        if (g_bc_bytes <= kCacheLow) break;
        auto it = g_blocks.find(o.second);
        g_bc_bytes -= it->second->data.size();
        g_blocks.erase(it);
    }
}

// a path's blocks are stale (it was written, truncated, removed or renamed)
void bc_invalidate(const std::string& path)
{
    std::string pre = path + '#';
    std::lock_guard<std::mutex> lk(g_bc_mx);
    for (auto it = g_blocks.begin(); it != g_blocks.end();)
    {
        if (it->first.compare(0, pre.size(), pre) == 0)
        {
            if (it->second->ready) g_bc_bytes -= it->second->data.size();
            it = g_blocks.erase(it);
        }
        else ++it;
    }
}

std::shared_ptr<Block> bc_get(const std::string& path, int64_t blk, int64_t size)
{
    std::string key = path + '#' + std::to_string(blk);
    std::unique_lock<std::mutex> lk(g_bc_mx);
    for (;;)
    {
        auto it = g_blocks.find(key);
        if (it == g_blocks.end()) break;
        std::shared_ptr<Block> b = it->second;
        if (b->ready)
        {
            b->used = ++g_bc_tick;
            return b->failed ? nullptr : b;
        }
        g_bc_cv.wait(lk);                                  // another thread is fetching it
    }
    auto b = std::make_shared<Block>();
    g_blocks[key] = b;
    lk.unlock();
    int64_t want = std::min(block_size(), size - blk * block_size());
    b->data.resize((size_t)want);
    {
        // start-up progress: name the file before the fetch, so a slow one shows what is loading
        size_t n = std::min(path.size(), sizeof g_pal_boot.file - 1);
        memcpy(g_pal_boot.file, path.data(), n);
        g_pal_boot.file[n] = 0;
    }
    int got = js_fs_fetch(path.c_str(), (double)(blk * block_size()), (int)want, b->data.data());
    lk.lock();
    auto it = g_blocks.find(key);
    bool listed = it != g_blocks.end() && it->second == b;
    if (got < 0)
    {
        b->failed = true;
        if (listed) g_blocks.erase(it);                    // a later read tries again
    }
    else
    {
        b->data.resize((size_t)got);
        if (listed) g_bc_bytes += (size_t)got;
        g_pal_boot.fetches++;
        g_pal_boot.kb += (uint32_t)((got + 1023) / 1024);
    }
    b->ready = true;
    b->used = ++g_bc_tick;
    bc_evict_locked();
    lk.unlock();
    g_bc_cv.notify_all();
    return b->failed ? nullptr : b;
}
} // namespace

std::string pal_fs_resolve(const char* path) { return norm(path); }
void pal_fs_chdir(const char* path)
{
    std::string n = norm(path);
    std::lock_guard<std::mutex> lk(g_cwd_mx);
    g_cwd = n;
}

static std::atomic<uint64_t> g_changes;
uint64_t pal_fs_change_count() { return g_changes.load(); }
PalFile* pal_fs_open(const char* path, const char* mode)
{
    if (strpbrk(mode, "wa+")) ++g_changes;
    std::string p = norm(path);
    uint32_t st[4];
    bool exists = js_fs_stat(p.c_str(), st) && !st[0];
    bool write = strchr(mode, 'w') || strchr(mode, '+');
    if (mode[0] == 'w')
    {
        bool ok = js_fs_op("truncate", p.c_str(), nullptr);
        bc_invalidate(p);
        if (!ok) return nullptr;
        return new PalFile{p, 0, 0, true};
    }
    if (!exists) return nullptr;
    return new PalFile{p, 0, (int64_t)st[1] + ((int64_t)st[2] << 32), write};
}
size_t pal_fs_read(void* buf, size_t n, PalFile* f)
{
    if (f->pos >= f->size) return 0;
    fs_init();
    size_t done = 0;
    while (done < n && f->pos < f->size)
    {
        int64_t blk = f->pos / block_size();
        std::shared_ptr<Block> b = bc_get(f->path, blk, f->size);
        if (!b) break;
        int64_t start = f->pos - blk * block_size();
        int64_t k = std::min<int64_t>((int64_t)(n - done), (int64_t)b->data.size() - start);
        if (k <= 0) break;
        memcpy((uint8_t*)buf + done, b->data.data() + start, (size_t)k);
        done += (size_t)k;
        f->pos += k;
    }
    return done;
}
size_t pal_fs_write(const void* buf, size_t n, PalFile* f)
{
    ++g_changes;
    if (!f->writable) return 0;
    fs_init();
    int put = js_fs_write(f->path.c_str(), (double)f->pos, (int)n, (const uint8_t*)buf);
    bc_invalidate(f->path);
    f->pos += put;
    if (f->pos > f->size) f->size = f->pos;
    return (size_t)put;
}
int pal_fs_seek(PalFile* f, int64_t off, int whence)
{
    int64_t base = whence == 0 ? 0 : whence == 1 ? f->pos : f->size;
    if (base + off < 0) return -1;
    f->pos = base + off;
    return 0;
}
int64_t pal_fs_tell(PalFile* f) { return f->pos; }
void pal_fs_flush(PalFile*) {}
void pal_fs_close(PalFile* f) { delete f; }
bool pal_fs_stat(const char* path, PalStat* st)
{
    uint32_t r[4];
    if (!js_fs_stat(norm(path).c_str(), r)) return false;
    st->dir = r[0] != 0;
    st->size = (uint64_t)r[1] | ((uint64_t)r[2] << 32);
    st->mtime = r[3];
    return true;
}
bool pal_fs_list(const char* dir, std::vector<std::string>& names)
{
    std::string p = norm(dir);
    std::vector<char> buf(65536);
    int need = js_fs_list(p.c_str(), buf.data(), (int)buf.size());
    if (need < 0) return false;
    if (need > (int)buf.size())
    {
        buf.resize(need);
        need = js_fs_list(p.c_str(), buf.data(), (int)buf.size());
    }
    for (const char* q = buf.data(); *q; q += strlen(q) + 1) names.push_back(q);
    return true;
}
bool pal_fs_mkdir(const char* path) { ++g_changes; return js_fs_op("mkdir", norm(path).c_str(), nullptr); }
bool pal_fs_remove(const char* path)
{
    ++g_changes;
    std::string p = norm(path);
    bool ok = js_fs_op("remove", p.c_str(), nullptr);
    bc_invalidate(p);
    return ok;
}
bool pal_fs_rename(const char* from, const char* to)
{
    ++g_changes;
    std::string a = norm(from), b = norm(to);
    bool ok = js_fs_op("rename", a.c_str(), b.c_str());
    bc_invalidate(a);
    bc_invalidate(b);
    return ok;
}
std::string pal_fs_cwd()
{
    std::lock_guard<std::mutex> lk(g_cwd_mx);
    return g_cwd;
}
