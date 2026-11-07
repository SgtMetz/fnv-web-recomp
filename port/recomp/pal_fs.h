// File access under the PAL (pal_kernel.cpp: CreateFileA, FindFirstFileA, INI files...). Two backends:
//   pal_fs_std.cpp  stdio + std::filesystem (native test host, node with NODERAWFS)
//   pal_fs_web.cpp  the browser: synchronous range requests to the local dev server (tools/recomp_serve.mjs), from
//                   the game's worker threads, with a read-ahead cache
// Paths are as the game spells them (Windows separators allowed); backends normalise them.
#pragma once
#include <stdint.h>
#include <stddef.h>

#include <string>
#include <vector>

struct PalFile;
struct PalStat { bool dir; uint64_t size; int64_t mtime; };   // mtime: seconds since 1970

PalFile* pal_fs_open(const char* path, const char* mode);      // fopen modes: rb, r+b, w+b
size_t pal_fs_read(void* buf, size_t n, PalFile* f);
size_t pal_fs_write(const void* buf, size_t n, PalFile* f);
int pal_fs_seek(PalFile* f, int64_t off, int whence);          // 0 on success
int64_t pal_fs_tell(PalFile* f);
void pal_fs_flush(PalFile* f);
void pal_fs_close(PalFile* f);
bool pal_fs_stat(const char* path, PalStat* st);
bool pal_fs_list(const char* dir, std::vector<std::string>& names);   // entry names, no . / ..
bool pal_fs_mkdir(const char* path);
bool pal_fs_remove(const char* path);
bool pal_fs_rename(const char* from, const char* to);
std::string pal_fs_cwd();
void pal_fs_chdir(const char* path);
std::string pal_fs_resolve(const char* path);   // the backend's own spelling (web: the server path)
