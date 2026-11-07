// pal_fs backend over stdio and std::filesystem (native test host, node). See pal_fs.h.
#include "pal_fs.h"

#include <stdio.h>
#include <sys/stat.h>

#include <stdlib.h>

#include <chrono>
#include <filesystem>

#ifdef _WIN32
#include <direct.h>
#define fseeko _fseeki64
#define ftello _ftelli64
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace {
std::string norm(const char* p)
{
    std::string s(p ? p : "");
#ifndef _WIN32
    for (char& c : s)
        if (c == '\\') c = '/';
#endif
    return s;
}
} // namespace

PalFile* pal_fs_open(const char* path, const char* mode) { return (PalFile*)fopen(norm(path).c_str(), mode); }
size_t pal_fs_read(void* buf, size_t n, PalFile* f) { return fread(buf, 1, n, (FILE*)f); }
size_t pal_fs_write(const void* buf, size_t n, PalFile* f) { return fwrite(buf, 1, n, (FILE*)f); }
int pal_fs_seek(PalFile* f, int64_t off, int whence) { return fseeko((FILE*)f, off, whence); }
int64_t pal_fs_tell(PalFile* f) { return ftello((FILE*)f); }
void pal_fs_flush(PalFile* f) { fflush((FILE*)f); }
void pal_fs_close(PalFile* f) { fclose((FILE*)f); }
bool pal_fs_stat(const char* path, PalStat* st)
{
#ifdef _WIN32
    struct _stat64 s;
    if (_stat64(norm(path).c_str(), &s)) return false;
#else
    struct stat s;
    if (stat(norm(path).c_str(), &s)) return false;
#endif
    st->dir = (s.st_mode & S_IFMT) == S_IFDIR;
    st->size = (uint64_t)s.st_size;
    st->mtime = (int64_t)s.st_mtime;
    return true;
}
bool pal_fs_list(const char* dir, std::vector<std::string>& names)
{
    static const bool log = getenv("FNV_FS_LOG") != nullptr;   // timing of directory scans (native vs wasm)
    static const auto t0 = std::chrono::steady_clock::now();
    if (log)
        fprintf(stderr, "[fs] %.2f list %s%c", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), dir, 10);
    std::error_code ec;
    for (auto& e : fs::directory_iterator(fs::path(norm(dir)), ec)) names.push_back(e.path().filename().string());
    return !ec;
}
bool pal_fs_mkdir(const char* path)
{
#ifdef _WIN32
    return _mkdir(norm(path).c_str()) == 0;
#else
    return mkdir(norm(path).c_str(), 0777) == 0;
#endif
}
bool pal_fs_remove(const char* path) { return remove(norm(path).c_str()) == 0; }
bool pal_fs_rename(const char* from, const char* to) { return rename(norm(from).c_str(), norm(to).c_str()) == 0; }
void pal_fs_chdir(const char* path)
{
    std::error_code ec;
    fs::current_path(fs::path(norm(path)), ec);
}
std::string pal_fs_cwd()
{
    std::error_code ec;
    return fs::current_path(ec).string();
}
