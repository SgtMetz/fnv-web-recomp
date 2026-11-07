// pal_fs backend for the browser (pal_fs.h): synchronous requests from the game's worker threads to the local dev
// server (tools/recomp_serve.mjs), which serves two roots: /game (the player's install, read-only) and /docs (the
// private copy of My Games\FalloutNV in out/fnv_docs, writable). Each worker keeps a read-ahead cache of 256 KB blocks;
// game files never leave the machine.
#include "pal_fs.h"

#include <emscripten.h>
#include <stdlib.h>
#include <string.h>

#include <mutex>

// one synchronous request, retried: a dropped connection must not kill the game thread that asked. Defined once per
// worker (fs_init below).
EM_JS(void, js_fs_init, (), {
    globalThis.fnvXhr = function (method, url, body, binary) {
        for (var attempt = 0; attempt < 20; attempt++) {
            try {
                var x = new XMLHttpRequest();
                x.open(method, url, false);
                if (binary) x.responseType = 'arraybuffer';
                x.send(body);
                return x;
            } catch (e) {
                var until = Date.now() + 250;
                while (Date.now() < until) {}
            }
        }
        return { status: 0 };
    };
});

EM_JS(int, js_fs_stat, (const char* path, uint32_t* out), {
    var x = fnvXhr('GET', '/fs/stat?p=' + encodeURIComponent(UTF8ToString(path)), null, false);
    if (x.status != 200) return 0;
    var j = JSON.parse(x.responseText);
    HEAPU32[out >> 2] = j.dir ? 1 : 0;
    HEAPU32[(out >> 2) + 1] = j.size % 4294967296;
    HEAPU32[(out >> 2) + 2] = Math.floor(j.size / 4294967296);
    HEAPU32[(out >> 2) + 3] = j.mtime;
    return 1;
});

EM_JS(int, js_fs_read, (const char* path, double off, int len, uint8_t* dst, double size), {
    var p = UTF8ToString(path);
    var B = 262144;
    var g = globalThis;
    if (!g.fnvCache) { g.fnvCache = new Map(); g.fnvCacheBytes = 0; }
    var done = 0;
    while (done < len) {
        var pos = off + done;
        if (pos >= size) break;
        var blk = Math.floor(pos / B);
        var key = p + '#' + blk;
        var data = g.fnvCache.get(key);
        if (!data) {
                    var want = Math.min(B, size - blk * B);
            var x = fnvXhr('GET', '/fs/read?p=' + encodeURIComponent(p) + '&o=' + (blk * B) + '&n=' + want, null, true);
            if (x.status != 200) break;
            data = new Uint8Array(x.response);
            g.fnvCache.set(key, data);
            g.fnvCacheBytes += data.length;
            if (g.fnvCacheBytes > 96 * 1048576) {          // drop the oldest blocks
                for (var k of g.fnvCache.keys()) {
                    g.fnvCacheBytes -= g.fnvCache.get(k).length;
                    g.fnvCache.delete(k);
                    if (g.fnvCacheBytes < 64 * 1048576) break;
                }
            }
        }
        var start = pos - blk * B;
        var n = Math.min(len - done, data.length - start);
        if (n <= 0) break;
        HEAPU8.set(data.subarray(start, start + n), dst + done);
        done += n;
    }
    return done;
});

EM_JS(int, js_fs_write, (const char* path, double off, int len, const uint8_t* src), {
    var p = UTF8ToString(path);
    var x = fnvXhr('POST', '/fs/write?p=' + encodeURIComponent(p) + '&o=' + off, HEAPU8.slice(src, src + len), false);
    if (globalThis.fnvCache)                                // the written range is stale in every block cache here
        for (var k of Array.from(globalThis.fnvCache.keys()))
            if (k.startsWith(p + '#')) { globalThis.fnvCacheBytes -= globalThis.fnvCache.get(k).length; globalThis.fnvCache.delete(k); }
    return x.status == 200 ? len : 0;
});

EM_JS(int, js_fs_list, (const char* path, char* out, int cap), {
    var x = fnvXhr('GET', '/fs/list?p=' + encodeURIComponent(UTF8ToString(path)), null, false);
    if (x.status != 200) return -1;
    var s = JSON.parse(x.responseText).join('\0') + '\0';
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
void fs_init()
{
    if (!t_init) { js_fs_init(); t_init = true; }
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

std::string pal_fs_resolve(const char* path) { return norm(path); }
void pal_fs_chdir(const char* path)
{
    std::string n = norm(path);
    std::lock_guard<std::mutex> lk(g_cwd_mx);
    g_cwd = n;
}

PalFile* pal_fs_open(const char* path, const char* mode)
{
    std::string p = norm(path);
    uint32_t st[4];
    bool exists = js_fs_stat(p.c_str(), st) && !st[0];
    bool write = strchr(mode, 'w') || strchr(mode, '+');
    if (mode[0] == 'w')
    {
        if (!js_fs_op("truncate", p.c_str(), nullptr)) return nullptr;
        return new PalFile{p, 0, 0, true};
    }
    if (!exists) return nullptr;
    return new PalFile{p, 0, (int64_t)st[1] + ((int64_t)st[2] << 32), write};
}
size_t pal_fs_read(void* buf, size_t n, PalFile* f)
{
    if (f->pos >= f->size) return 0;
    fs_init();
    int got = js_fs_read(f->path.c_str(), (double)f->pos, (int)n, (uint8_t*)buf, (double)f->size);
    f->pos += got;
    return (size_t)got;
}
size_t pal_fs_write(const void* buf, size_t n, PalFile* f)
{
    if (!f->writable) return 0;
    fs_init();
    int put = js_fs_write(f->path.c_str(), (double)f->pos, (int)n, (const uint8_t*)buf);
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
bool pal_fs_mkdir(const char* path) { return js_fs_op("mkdir", norm(path).c_str(), nullptr); }
bool pal_fs_remove(const char* path) { return js_fs_op("remove", norm(path).c_str(), nullptr); }
bool pal_fs_rename(const char* from, const char* to) { return js_fs_op("rename", norm(from).c_str(), norm(to).c_str()); }
std::string pal_fs_cwd()
{
    std::lock_guard<std::mutex> lk(g_cwd_mx);
    return g_cwd;
}
