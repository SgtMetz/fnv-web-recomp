// Portable platform layer for the recompiled game (docs/RECOMP.md): the Win32 imports implemented in standard C++
// (threads, mutexes, files), so the same code serves the native test host (FNV_PAL=1) and the Emscripten build.
// Guest pointers are host pointers in both (native: the process; wasm32: linear memory).
#pragma once
#include "fnv_recomp_host.h"

struct PalEntry
{
    const char* name;   // the import's name as the exe's import table spells it
    HostFn fn;
};

// The implemented imports, terminated by {nullptr, nullptr}. Groups: pal_kernel.cpp (more to come).
const PalEntry* pal_kernel_entries();
const PalEntry* pal_user_entries();     // pal_user.cpp: user32, gdi32, comctl32, shell32, ole32, mmio, WinSock, Bink, XInput, Steam
const PalEntry* pal_input_entries();    // pal_input.cpp: DirectInput8
void pal_input_script(const char* script);                       // FNV_INPUT_SCRIPT key taps
void pal_input_key(int dik, bool down);                         // host input: DIK_* scan code
void pal_input_mouse(int dx, int dy, int dz, int button, bool down);   // button -1: movement only
// pal_start.cpp: FNV_START=quick begins a new game outside Doc Mitchell's house with the default character
void pal_start_quick();
void pal_start_tick(FnvCpu& s);         // main thread, once per keyboard poll
void pal_console_line(const char* line);   // run a console line on the main thread at the next poll (debugging)

// Start-up progress, for the page's loading bar (host_web.cpp fnv_boot). The game thread writes it and the page
// only reads it, so a torn read just shows a stale value for one poll.
//   stage   kBoot* below, only ever increases
//   fetches blocks the file layer fetched from the server (pal_fs_web.cpp; cache hits are not counted)
//   kb      kilobytes in those blocks
//   file    the file of the last fetch, NUL-terminated
enum { kBootMain = 1, kBootMenu = 2, kBootWorld = 3, kBootPorch = 4 };
struct PalBoot { volatile uint32_t stage, fetches, kb, reserved; char file[112]; };
extern PalBoot g_pal_boot;
void pal_boot_stage(uint32_t stage);   // raises g_pal_boot.stage to `stage` (never lowers it)
const PalEntry* pal_sound_entries();
// music: the last .mp3 this thread opened through CreateFileA (pal_kernel.cpp). The game's music thread opens the
// track with its own reader right before it builds the DirectShow graph (pal_dshow.cpp), which plays that file.
const char* pal_media_last_open();
void pal_dshow_enable_sink();
void pal_vorbis_enable_decoder();      // browser: ov_* decode through the page (pal_user.cpp)            // browser: graphs drive web/recomp/index.html's music player    // pal_dsound.cpp: DirectSound 8
const PalEntry* pal_d3d9_entries();     // pal_d3d9.cpp: Direct3DCreate9
const PalEntry* pal_d3dx_entries();     // pal_d3dx.cpp: D3DX
// pal_user.cpp: the host's screen size, input messages for the main window, its handle
void pal_user_set_screen(int w, int h);
void pal_user_post(uint32_t msg, uint32_t wp, uint32_t lp);
uint32_t pal_user_main_window();

// Host services the PAL needs (the host provides them):
//   run guest code `fn(arg)` as a new guest thread, on the calling OS thread (it sets up the TEB, stack, TLS)
uint32_t fnv_host_thread_main(uint32_t fn, uint32_t arg);
//   start an OS thread with a native stack big enough for recompiled code (std::thread cannot set one)
//   (returns an OS thread token for fnv_host_set_priority, 0 if the platform has none)
uintptr_t fnv_host_spawn(void (*fn)(void*), void* arg);
//   Win32 priority -15..15 for a thread from fnv_host_spawn (0 token = the calling thread); no-op where unsupported
void fnv_host_set_priority(uintptr_t token, int priority);
//   a handle the PAL did not create (native test host: made by a real DLL): wait on it / close it; -1 = unknown
int64_t fnv_host_wait_foreign(uint32_t h, uint32_t ms);
bool fnv_host_close_foreign(uint32_t h);
//   the guest's own module (its image base), its path, the private documents folder
uint32_t fnv_host_image_base();
const char* fnv_host_exe_path();

// Guest-visible handle values: multiples of 4 from here (Win32 pseudo handles -1/-2 stay distinct)
static const uint32_t kPalHandleBase = 0x00100000u;

inline const char* gstr(uint32_t p) { return (const char*)(uintptr_t)p; }
inline char* gbuf(uint32_t p) { return (char*)(uintptr_t)p; }
