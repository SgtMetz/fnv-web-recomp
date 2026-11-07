// PAL user group: user32 (one main window, its class, a message queue, window procedures called on the guest), gdi32,
// comctl32, shell32, ole32, winmm's mmio, and the libraries the browser build does without: WinSock (no network),
// Bink (no movies yet), XInput (no pad), the Steam/Galaxy wrapper (no client), DirectShow (CoCreateInstance fails).
#include "pal.h"
#include "pal_fs.h"
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <unistd.h>
#endif
#include <stdlib.h>

uint32_t pal_dshow_create(uint32_t clsid);   // pal_dshow.cpp: the DirectShow filter graph

#include <stdio.h>
#include <string.h>

#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace {
#define A(i) harg(s, (i))
#define RET(v, n) return hret(s, (uint32_t)(v), (n))
#define RETC(v) do { s.eax = (uint32_t)(v); s.esp += 4; return; } while (0)   // cdecl: the caller pops

struct Window
{
    uint32_t wndproc = 0, style = 0, exstyle = 0, userdata = 0;
    int x = 0, y = 0, w = 1280, h = 720;
    std::string cls, title;
};
std::mutex g_mx;
std::map<std::string, uint32_t> g_classes;     // class name (lower case) -> window procedure
std::map<uint32_t, Window> g_windows;          // HWND -> window
uint32_t g_next_hwnd = 0x00010010u;
uint32_t g_main_hwnd;
struct Msg { uint32_t hwnd, msg, wp, lp; };
std::deque<Msg> g_queue;
int g_cursor_count;
int g_screen_w = 1280, g_screen_h = 720;

std::string lower(const char* p)
{
    std::string s(p ? p : "");
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

uint32_t call_wndproc(FnvCpu& s, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp)
{
    uint32_t proc;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        auto it = g_windows.find(hwnd);
        proc = it == g_windows.end() ? 0 : it->second.wndproc;
    }
    if (!proc) return 0;
    uint32_t args[4] = {hwnd, msg, wp, lp};
    return fnv_guest_call(s, proc, args, 4);
}

// ---- user32 ------------------------------------------------------------------------------------------------------
void u_RegisterClassA(FnvCpu& s)
{
    uint32_t wc = A(0);
    const char* name = gstr(M32(wc + 36));
    std::lock_guard<std::mutex> lk(g_mx);
    g_classes[lower(name)] = M32(wc + 4);
    RET(0xc000 + g_classes.size(), 1);                       // an atom
}
void u_CreateWindowExA(FnvCpu& s)
{
    uint32_t a[12];
    for (int i = 0; i < 12; ++i) a[i] = A(i);
    Window w;
    w.exstyle = a[0];
    w.cls = gstr(a[1]) && (a[1] >> 16) ? gstr(a[1]) : "";
    w.title = a[2] ? gstr(a[2]) : "";
    w.style = a[3];
    w.x = (int)a[4]; w.y = (int)a[5];
    if ((int)a[6] > 0) w.w = (int)a[6];
    if ((int)a[7] > 0) w.h = (int)a[7];
    uint32_t hwnd;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        auto it = g_classes.find(lower(w.cls.c_str()));
        w.wndproc = it == g_classes.end() ? 0 : it->second;
        hwnd = g_next_hwnd += 0x10;
        g_windows[hwnd] = w;
        if (!g_main_hwnd && !a[8]) g_main_hwnd = hwnd;
    }
    char buf[200];
    snprintf(buf, sizeof buf, "[user] CreateWindowExA(\"%s\", \"%s\", %dx%d) -> %08x%c", w.cls.c_str(), w.title.c_str(),
             w.w, w.h, hwnd, 10);
    fnv_host_log(buf);
    // WM_CREATE with a CREATESTRUCTA, as CreateWindowEx sends it before returning
    uint32_t cs = (uint32_t)(uintptr_t)calloc(1, 48);
    M32(cs + 0) = a[11]; M32(cs + 4) = a[10]; M32(cs + 8) = a[9]; M32(cs + 12) = a[8];
    M32(cs + 16) = w.h; M32(cs + 20) = w.w; M32(cs + 24) = w.y; M32(cs + 28) = w.x;
    M32(cs + 32) = w.style; M32(cs + 36) = a[2]; M32(cs + 40) = a[1]; M32(cs + 44) = w.exstyle;
    call_wndproc(s, hwnd, 0x0001, 0, cs);
    free((void*)(uintptr_t)cs);
    RET(hwnd, 12);
}
void u_DestroyWindow(FnvCpu& s)
{
    call_wndproc(s, A(0), 0x0002, 0, 0);                     // WM_DESTROY
    std::lock_guard<std::mutex> lk(g_mx);
    g_windows.erase(A(0));
    RET(1, 1);
}
void u_DefWindowProcA(FnvCpu& s)
{
    if (A(1) == 0x0010)                                      // WM_CLOSE -> DestroyWindow
    {
        uint32_t hwnd = A(0);
        call_wndproc(s, hwnd, 0x0002, 0, 0);
    }
    RET(0, 4);
}
void u_PeekMessageA(FnvCpu& s)
{
    std::lock_guard<std::mutex> lk(g_mx);
    if (g_queue.empty()) RET(0, 5);
    Msg m = g_queue.front();
    if (A(4) & 1) g_queue.pop_front();                       // PM_REMOVE
    uint32_t p = A(0);
    M32(p) = m.hwnd; M32(p + 4) = m.msg; M32(p + 8) = m.wp; M32(p + 12) = m.lp; M32(p + 16) = 0; M32(p + 20) = 0; M32(p + 24) = 0;
    RET(1, 5);
}
void u_TranslateMessage(FnvCpu& s) { RET(1, 1); }
void u_DispatchMessageA(FnvCpu& s)
{
    uint32_t p = A(0);
    uint32_t r = call_wndproc(s, M32(p), M32(p + 4), M32(p + 8), M32(p + 12));
    RET(r, 1);
}
void u_SendMessageA(FnvCpu& s) { uint32_t r = call_wndproc(s, A(0), A(1), A(2), A(3)); RET(r, 4); }
void u_GetSystemMetrics(FnvCpu& s)
{
    switch (A(0))
    {
    case 0: case 78: RET(g_screen_w, 1);                     // SM_CXSCREEN, SM_CXVIRTUALSCREEN
    case 1: case 79: RET(g_screen_h, 1);
    case 80: RET(1, 1);                                      // SM_CMONITORS
    default: RET(0, 1);
    }
}
void u_AdjustWindowRect(FnvCpu& s) { RET(1, 3); }
void u_AdjustWindowRectEx(FnvCpu& s) { RET(1, 4); }
void u_GetClientRect(FnvCpu& s)
{
    std::lock_guard<std::mutex> lk(g_mx);
    auto it = g_windows.find(A(0));
    uint32_t r = A(1);
    M32(r) = 0; M32(r + 4) = 0;
    M32(r + 8) = it == g_windows.end() ? g_screen_w : it->second.w;
    M32(r + 12) = it == g_windows.end() ? g_screen_h : it->second.h;
    RET(1, 2);
}
void u_SetWindowPos(FnvCpu& s)
{
    std::lock_guard<std::mutex> lk(g_mx);
    auto it = g_windows.find(A(0));
    if (it != g_windows.end() && !(A(6) & 1) && (int)A(4) > 0) { it->second.w = (int)A(4); it->second.h = (int)A(5); }
    RET(1, 7);
}
void u_true1(FnvCpu& s) { RET(1, 1); }
void u_true2(FnvCpu& s) { RET(1, 2); }
void u_zero0(FnvCpu& s) { RET(0, 0); }
void u_zero1(FnvCpu& s) { RET(0, 1); }
void u_zero2(FnvCpu& s) { RET(0, 2); }
void u_GetActiveWindow(FnvCpu& s) { RET(g_main_hwnd, 0); }
void u_SetForegroundWindow(FnvCpu& s) { RET(1, 1); }
void u_ShowCursor(FnvCpu& s) { g_cursor_count += A(0) ? 1 : -1; RET(g_cursor_count, 1); }
void u_GetDoubleClickTime(FnvCpu& s) { RET(500, 0); }
void u_LoadIconA(FnvCpu& s) { RET(0x00020001, 2); }
void u_LoadCursorA(FnvCpu& s) { RET(0x00020003, 2); }
void u_GetWindowLongA(FnvCpu& s)
{
    std::lock_guard<std::mutex> lk(g_mx);
    auto it = g_windows.find(A(0));
    if (it == g_windows.end()) RET(0, 2);
    int32_t idx = (int32_t)A(1);
    RET(idx == -16 ? it->second.style : idx == -20 ? it->second.exstyle : idx == -4 ? it->second.wndproc :
        idx == -21 ? it->second.userdata : 0, 2);
}
void u_GetClassLongA(FnvCpu& s) { RET(0, 2); }
void u_GetClassNameA(FnvCpu& s)
{
    std::lock_guard<std::mutex> lk(g_mx);
    auto it = g_windows.find(A(0));
    std::string n = it == g_windows.end() ? "" : it->second.cls;
    strncpy(gbuf(A(1)), n.c_str(), A(2));
    RET(n.size(), 3);
}
void u_GetWindowTextA(FnvCpu& s)
{
    std::lock_guard<std::mutex> lk(g_mx);
    auto it = g_windows.find(A(0));
    std::string n = it == g_windows.end() ? "" : it->second.title;
    if (A(2)) { strncpy(gbuf(A(1)), n.c_str(), A(2) - 1); gbuf(A(1))[A(2) - 1] = 0; }
    RET(n.size(), 3);
}
void u_SetWindowTextA(FnvCpu& s)
{
    std::lock_guard<std::mutex> lk(g_mx);
    auto it = g_windows.find(A(0));
    if (it != g_windows.end()) it->second.title = gstr(A(1));
    RET(1, 2);
}
void u_SetWindowsHookExA(FnvCpu& s) { RET(0x00030001, 4); }  // a hook handle; the hook is never called
void u_CallNextHookEx(FnvCpu& s) { RET(0, 4); }
void u_EnumChildWindows(FnvCpu& s) { RET(1, 3); }
void u_EnumDisplayDevicesA(FnvCpu& s)
{
    if (A(0) || A(1) != 0) RET(0, 4);                        // one adapter
    uint32_t d = A(2);
    memset(gbuf(d + 4), 0, 420);
    strcpy(gbuf(d + 4), "\\\\.\\DISPLAY1");
    strcpy(gbuf(d + 36), "WebGPU");
    M32(d + 164) = 0x5;                                      // attached to desktop, primary
    RET(1, 4);
}
void u_GetWindow(FnvCpu& s) { RET(0, 2); }
void u_SendInput(FnvCpu& s) { RET(0, 3); }
void u_GetAsyncKeyState(FnvCpu& s) { RET(0, 1); }
void u_FindWindowA(FnvCpu& s) { RET(0, 2); }
void u_SwapMouseButton(FnvCpu& s) { RET(0, 1); }

// ---- gdi32, comctl32, shell32 --------------------------------------------------------------------------------------
void g_GetStockObject(FnvCpu& s) { RET(0x00040000 + A(0), 1); }
void c_InitCommonControlsEx(FnvCpu& s) { RET(1, 1); }
void c_ImageList_LoadImageA(FnvCpu& s) { RET(0, 7); }
void c_ImageList_Destroy(FnvCpu& s) { RET(1, 1); }
void sh_CommandLineToArgvW(FnvCpu& s)
{
    // argv = {"FalloutNV.exe"}: an array of pointers followed by the strings, freed by LocalFree in the game
    static uint16_t arg0[] = {'F', 'a', 'l', 'l', 'o', 'u', 't', 'N', 'V', '.', 'e', 'x', 'e', 0};
    uint32_t* argv = (uint32_t*)calloc(2, 4);
    argv[0] = (uint32_t)(uintptr_t)arg0;
    if (A(1)) M32(A(1)) = 1;
    RET((uintptr_t)argv, 2);
}
void sh_ShellExecuteA(FnvCpu& s) { RET(2, 6); }              // <= 32: failure, the game carries on

// ---- ole32 (DirectShow is not there: music players fall back) ---------------------------------------------------------
void o_CoInitialize(FnvCpu& s) { RET(0, 1); }
void o_CoInitializeEx(FnvCpu& s) { RET(0, 2); }
void o_CoUninitialize(FnvCpu& s) { RET(0, 0); }
void o_CoCreateInstance(FnvCpu& s)
{
    uint32_t obj = pal_dshow_create(A(0));                     // the DirectShow filter graph (music); nothing else
    if (A(4)) M32(A(4)) = obj;
    RET(obj ? 0 : 0x80040154u, 5);                           // REGDB_E_CLASSNOTREG
}
void o_CoFreeUnusedLibraries(FnvCpu& s) { RET(0, 0); }
void o_CoTaskMemAlloc(FnvCpu& s) { RET((uintptr_t)malloc(A(0) ? A(0) : 1), 1); }
void o_CoTaskMemFree(FnvCpu& s) { free((void*)(uintptr_t)A(0)); RET(0, 1); }

// ---- winmm mmio: RIFF/WAV parsing for the game's sounds ----------------------------------------------------------------
// A file opens whole into memory (by name through pal_fs, or from the caller's memory block with FOURCC_MEM); the
// handle keeps a read position. mmioGetInfo/mmioAdvance expose the whole file as the I/O buffer.
struct Mmio { uint32_t data = 0, size = 0, pos = 0; bool owned = false; };
std::mutex g_mmio_mx;
std::map<uint32_t, Mmio*> g_mmio;
uint32_t g_mmio_next = 0x6d000010;
Mmio* mmio(uint32_t h)
{
    std::lock_guard<std::mutex> lk(g_mmio_mx);
    auto it = g_mmio.find(h);
    return it == g_mmio.end() ? nullptr : it->second;
}
uint32_t fcc(const char* c) { return (uint32_t)c[0] | ((uint32_t)c[1] << 8) | ((uint32_t)c[2] << 16) | ((uint32_t)c[3] << 24); }

void m_mmioOpenA(FnvCpu& s)
{
    uint32_t name = A(0), info = A(1), flags = A(2);
    Mmio* m = new Mmio;
    static int logged = 0;
    if (info && M32(info + 4) == fcc("MEM "))
    {
        m->data = M32(info + 24);
        m->size = M32(info + 20);
    }
    else if (info && M32(info + 8))
    {   // the caller's own I/O procedure (the game reads WAVs out of its BSA archives this way): open, read it all,
        // close, then parse from memory. MMIOM_READ 0, MMIOM_OPEN 3, MMIOM_CLOSE 4; the proc keeps its state in the
        // MMIOINFO, so it gets a private copy as on Windows.
        uint32_t proc = M32(info + 8);
        uint32_t mi = (uint32_t)(uintptr_t)calloc(1, 72);
        memcpy(gbuf(mi), gbuf(info), 72);
        M32(mi + 44) = 0;
        uint32_t open_args[4] = {mi, 3, name, 0};
        if (fnv_guest_call(s, proc, open_args, 4) == 0)
        {
            std::string all;
            uint32_t chunk = (uint32_t)(uintptr_t)malloc(65536);
            for (;;)
            {
                uint32_t read_args[4] = {mi, 0, chunk, 65536};
                int32_t n = (int32_t)fnv_guest_call(s, proc, read_args, 4);
                if (n <= 0) break;
                all.append((const char*)(uintptr_t)chunk, (size_t)n);
            }
            free((void*)(uintptr_t)chunk);
            uint32_t close_args[4] = {mi, 4, 0, 0};
            fnv_guest_call(s, proc, close_args, 4);
            m->size = (uint32_t)all.size();
            m->data = (uint32_t)(uintptr_t)malloc(m->size ? m->size : 4);
            memcpy((void*)(uintptr_t)m->data, all.data(), all.size());
            m->owned = true;
        }
        free((void*)(uintptr_t)mi);
    }
    else if (name)
    {
        PalFile* f = pal_fs_open(gstr(name), "rb");
        if (f)
        {
            std::string all;
            char chunk[65536];
            for (size_t n; (n = pal_fs_read(chunk, sizeof chunk, f)) > 0;) all.append(chunk, n);
            pal_fs_close(f);
            m->size = (uint32_t)all.size();
            m->data = (uint32_t)(uintptr_t)malloc(m->size ? m->size : 4);
            memcpy((void*)(uintptr_t)m->data, all.data(), all.size());
            m->owned = true;
        }
    }
    if (logged < 8)
    {
        ++logged;
        char msg[300];
        snprintf(msg, sizeof msg, "[mmio] open %s info %08x fcc %08x ioproc %08x flags %08x -> %u bytes%c", name ? gstr(name) : "(null)",
                 info, info ? M32(info + 4) : 0, info ? M32(info + 8) : 0, flags, m->data ? m->size : 0, 10);
        fnv_host_log(msg);
    }
    if (!m->data) { delete m; if (info) M32(info + 12) = 257; RET(0, 3); }   // MMIOERR_FILENOTFOUND
    std::lock_guard<std::mutex> lk(g_mmio_mx);
    uint32_t h = g_mmio_next += 16;
    g_mmio[h] = m;
    RET(h, 3);
}
void m_mmioClose(FnvCpu& s)
{
    std::lock_guard<std::mutex> lk(g_mmio_mx);
    auto it = g_mmio.find(A(0));
    if (it != g_mmio.end())
    {
        if (it->second->owned) free((void*)(uintptr_t)it->second->data);
        delete it->second;
        g_mmio.erase(it);
    }
    RET(0, 2);
}
void m_mmioRead(FnvCpu& s)
{
    Mmio* m = mmio(A(0));
    if (!m) RET(0xffffffffu, 3);
    uint32_t n = A(2);
    if (m->pos >= m->size) n = 0;
    else if (n > m->size - m->pos) n = m->size - m->pos;
    memcpy(gbuf(A(1)), (const void*)(uintptr_t)(m->data + m->pos), n);
    m->pos += n;
    RET(n, 3);
}
// MMCKINFO: ckid, cksize, fccType, dwDataOffset, dwFlags
void m_mmioDescend(FnvCpu& s)
{
    Mmio* m = mmio(A(0));
    uint32_t ck = A(1), parent = A(2), flags = A(3);
    if (!m) RET(265, 4);
    uint32_t end = parent ? M32(parent + 12) + M32(parent + 4) : m->size;
    if (end > m->size) end = m->size;
    uint32_t want_id = (flags & 0x10) ? M32(ck) : (flags & 0x20) ? fcc("RIFF") : (flags & 0x40) ? fcc("LIST") : 0;
    uint32_t want_type = (flags & 0x60) ? M32(ck + 8) : 0;
    uint32_t pos = m->pos;
    while (pos + 8 <= end)
    {
        const uint8_t* d = (const uint8_t*)(uintptr_t)m->data;
        uint32_t id, size;
        memcpy(&id, d + pos, 4);
        memcpy(&size, d + pos + 4, 4);
        bool riff = id == fcc("RIFF") || id == fcc("LIST");
        uint32_t type = 0;
        if (riff && pos + 12 <= end) memcpy(&type, d + pos + 8, 4);
        if (!want_id || (id == want_id && (!want_type || type == want_type)))
        {
            M32(ck) = id;
            M32(ck + 4) = size;
            M32(ck + 8) = riff ? type : 0;
            M32(ck + 12) = pos + 8;
            M32(ck + 16) = 0;
            m->pos = pos + 8 + (riff ? 4 : 0);
            RET(0, 4);
        }
        pos += 8 + size + (size & 1);
    }
    RET(265, 4);                                                    // MMIOERR_CHUNKNOTFOUND
}
void m_mmioAscend(FnvCpu& s)
{
    Mmio* m = mmio(A(0));
    if (!m) RET(265, 3);
    uint32_t ck = A(1);
    uint32_t size = M32(ck + 4);
    m->pos = M32(ck + 12) + size + (size & 1);
    RET(0, 3);
}
// MMIOINFO: dwFlags 0, fccIOProc 4, pIOProc 8, wErrorRet 12, htask 16, cchBuffer 20, pchBuffer 24, pchNext 28,
// pchEndRead 32, pchEndWrite 36, lBufOffset 40, lDiskOffset 44, adwInfo 48, reserved 60/64, hmmio 68
void m_mmioGetInfo(FnvCpu& s)
{
    Mmio* m = mmio(A(0));
    if (!m) RET(258, 3);
    uint32_t i = A(1);
    memset(gbuf(i), 0, 72);
    M32(i + 20) = m->size;
    M32(i + 24) = m->data;
    M32(i + 28) = m->data + m->pos;
    M32(i + 32) = m->data + m->size;
    M32(i + 36) = m->data + m->size;
    M32(i + 44) = m->size;
    M32(i + 68) = A(0);
    RET(0, 3);
}
void m_mmioAdvance(FnvCpu& s)
{
    Mmio* m = mmio(A(0));
    if (!m) RET(258, 3);
    uint32_t i = A(1);
    if (i)
    {
        uint32_t next = M32(i + 28);
        if (next >= m->data && next <= m->data + m->size) m->pos = next - m->data;
        M32(i + 28) = m->data + m->pos;
        M32(i + 32) = m->data + m->size;
    }
    RET(0, 3);
}

// ---- WinSock: no network -------------------------------------------------------------------------------------------
void w_WSAStartup(FnvCpu& s) { RET(10091, 2); }             // WSASYSNOTREADY
void w_WSAGetLastError(FnvCpu& s) { RET(10091, 0); }
void w_fail_n(FnvCpu& s, int n) { hret(s, 0xffffffffu, n); }
void w_socket(FnvCpu& s) { w_fail_n(s, 3); }
void w_err1(FnvCpu& s) { w_fail_n(s, 1); }
void w_err2(FnvCpu& s) { w_fail_n(s, 2); }
void w_err3(FnvCpu& s) { w_fail_n(s, 3); }
void w_err4(FnvCpu& s) { w_fail_n(s, 4); }
void w_err5(FnvCpu& s) { w_fail_n(s, 5); }
void w_htons(FnvCpu& s) { uint32_t v = A(0) & 0xffff; RET(((v >> 8) | (v << 8)) & 0xffff, 1); }
void w_inet_addr(FnvCpu& s) { RET(0xffffffffu, 1); }
void w_inet_ntoa(FnvCpu& s) { static char z[] = "0.0.0.0"; RET((uintptr_t)z, 1); }
void w_gethostbyname(FnvCpu& s) { RET(0, 1); }
void w_gethostname(FnvCpu& s) { if (A(1)) strncpy(gbuf(A(0)), "localhost", A(1)); RET(0, 2); }
void w_FDIsSet(FnvCpu& s) { RET(0, 2); }

// ---- Bink: no movies yet (BinkOpen fails, the game skips them) -------------------------------------------------------
void b_BinkOpen(FnvCpu& s) { RET(0, 2); }
void b_BinkSetSoundSystem(FnvCpu& s) { RET(1, 2); }
void b_BinkOpenDirectSound(FnvCpu& s) { RET(0, 1); }
void b_void1(FnvCpu& s) { RET(0, 1); }
void b_void2(FnvCpu& s) { RET(0, 2); }
void b_BinkCopyToBufferRect(FnvCpu& s) { RET(0, 11); }

// ---- XInput: no pad -------------------------------------------------------------------------------------------------
void x_XInputGetState(FnvCpu& s) { RET(1167, 2); }          // ERROR_DEVICE_NOT_CONNECTED
void x_XInputSetState(FnvCpu& s) { RET(1167, 2); }

// ---- Steam API wrapper (cdecl): no client; interface getters return null ----------------------------------------
void st_false(FnvCpu& s) { RETC(0); }

// ---- libvorbisfile (cdecl): Ogg Vorbis voices ----------------------------------------------------------------------------
// ov_open_callbacks reads the whole stream through the game's own callbacks; in the browser the page decodes it
// (decodeAudioData, web/recomp/index.html fnvDecode) into 16-bit PCM at the page's 48 kHz, which ov_read then serves.
// Without a decoder (node) opening reports OV_ENOTVORBIS and the game skips the line. ov_info always answers a valid
// description (the game divides by the rate).
struct OvFile
{
    uint32_t pcm = 0, frames = 0, channels = 2, rate = 44100, pos = 0;   // pcm: int16 interleaved (page malloc)
    uint32_t info = 0;                                                  // guest vorbis_info
    uint32_t datasource = 0, close_fn = 0;
};
std::mutex g_ov_mx;
std::map<uint32_t, OvFile*> g_ov;
bool g_ov_decoder;
uint32_t g_vorbis_info[8] = {0, 2, 44100, 0, 0, 0, 0, 0};    // vorbis_info: version, channels, rate, bitrates
OvFile* ov(uint32_t vf)
{
    std::lock_guard<std::mutex> lk(g_ov_mx);
    auto it = g_ov.find(vf);
    return it == g_ov.end() ? nullptr : it->second;
}

void ov_open_callbacks_(FnvCpu& s)
{
    uint32_t ds = A(0), vf = A(1), initial = A(2), ibytes = A(3), read_fn = A(4), close_fn = A(6);
    if (!g_ov_decoder || !read_fn) RETC(0xFFFFFF7Cu);                  // OV_ENOTVORBIS (-132)
    std::string all;
    if (initial && ibytes) all.append((const char*)(uintptr_t)initial, ibytes);
    uint32_t chunk = (uint32_t)(uintptr_t)malloc(65536);
    for (;;)
    {
        uint32_t args[4] = {chunk, 1, 65536, ds};
        uint32_t n = fnv_guest_call(s, read_fn, args, 4);
        if (!n || n > 65536) break;
        all.append((const char*)(uintptr_t)chunk, n);
    }
    free((void*)(uintptr_t)chunk);
#ifdef __EMSCRIPTEN__
    // request: data, bytes, done, pcm, frames, channels, rate, ok
    uint32_t* req = (uint32_t*)calloc(8, 4);
    req[0] = (uint32_t)(uintptr_t)all.data();
    req[1] = (uint32_t)all.size();
    MAIN_THREAD_ASYNC_EM_ASM({ if (globalThis.fnvDecode) globalThis.fnvDecode($0); else HEAPU32[(($0 >>> 0) >>> 2) + 2] = 1; }, req);
    for (int i = 0; i < 20000 && !__atomic_load_n(&req[2], __ATOMIC_ACQUIRE); ++i) usleep(500);
    bool ok = __atomic_load_n(&req[2], __ATOMIC_ACQUIRE) && req[7];
    OvFile* f = nullptr;
    if (ok)
    {
        f = new OvFile;
        f->pcm = req[3]; f->frames = req[4]; f->channels = req[5]; f->rate = req[6];
        f->datasource = ds; f->close_fn = close_fn;
        f->info = (uint32_t)(uintptr_t)calloc(8, 4);
        M32(f->info + 4) = f->channels;
        M32(f->info + 8) = f->rate;
    }
    free(req);
    {
        char m[200];
        snprintf(m, sizeof m, "[vorbis] open vf %08x: %u bytes -> %s, %u frames %u ch %u Hz%c", vf, (unsigned)all.size(),
                 f ? "ok" : "failed", f ? f->frames : 0, f ? f->channels : 0, f ? f->rate : 0, 10);
        fnv_host_log(m);
    }
    if (!f) RETC(0xFFFFFF7Cu);
    std::lock_guard<std::mutex> lk(g_ov_mx);
    g_ov[vf] = f;
    RETC(0);
#else
    RETC(0xFFFFFF7Cu);
#endif
}
void ov_info_(FnvCpu& s)
{
    OvFile* f = ov(A(0));
    RETC(f ? f->info : (uint32_t)(uintptr_t)g_vorbis_info);
}
void ov_pcm_total_(FnvCpu& s)
{
    OvFile* f = ov(A(0));
    s.edx = 0;
    RETC(f ? f->frames : 0);
}
// ov_read(vf, buffer, length, bigendianp, word, sgned, bitstream): 16-bit little-endian signed samples
void ov_read_(FnvCpu& s)
{
    OvFile* f = ov(A(0));
    if (A(6)) M32(A(6)) = 0;
    if (!f) RETC(0);
    uint32_t total = f->frames * f->channels * 2;
    uint32_t n = A(2) & ~1u;
    if (f->pos >= total) RETC(0);
    if (n > total - f->pos) n = total - f->pos;
    memcpy(gbuf(A(1)), (const void*)(uintptr_t)(f->pcm + f->pos), n);
    f->pos += n;
    RETC(n);
}
void ov_clear_(FnvCpu& s)
{
    OvFile* f = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_ov_mx);
        auto it = g_ov.find(A(0));
        if (it != g_ov.end()) { f = it->second; g_ov.erase(it); }
    }
    if (f)
    {
        if (f->close_fn) { uint32_t args[1] = {f->datasource}; fnv_guest_call(s, f->close_fn, args, 1); }
        free((void*)(uintptr_t)f->pcm);
        free((void*)(uintptr_t)f->info);
        delete f;
    }
    RETC(0);
}
void ov_seekable_(FnvCpu& s) { RETC(ov(A(0)) ? 1 : 0); }     // decoded whole: the game sizes its buffer from ov_pcm_total only when seekable
void ov_zero_(FnvCpu& s) { RETC(0); }

// ---- d3d9 module function ---------------------------------------------------------------------------------------------
void d_D3DPERF_SetOptions(FnvCpu& s) { RET(0, 1); }

const PalEntry kUser[] = {
    {"RegisterClassA", u_RegisterClassA}, {"CreateWindowExA", u_CreateWindowExA}, {"DestroyWindow", u_DestroyWindow},
    {"DefWindowProcA", u_DefWindowProcA}, {"PeekMessageA", u_PeekMessageA}, {"TranslateMessage", u_TranslateMessage},
    {"DispatchMessageA", u_DispatchMessageA}, {"SendMessageA", u_SendMessageA}, {"GetSystemMetrics", u_GetSystemMetrics},
    {"AdjustWindowRect", u_AdjustWindowRect}, {"AdjustWindowRectEx", u_AdjustWindowRectEx},
    {"GetClientRect", u_GetClientRect}, {"SetWindowPos", u_SetWindowPos}, {"ShowWindow", u_true2},
    {"UpdateWindow", u_true1}, {"GetActiveWindow", u_GetActiveWindow}, {"SetForegroundWindow", u_SetForegroundWindow},
    {"ShowCursor", u_ShowCursor}, {"GetDoubleClickTime", u_GetDoubleClickTime}, {"LoadIconA", u_LoadIconA},
    {"LoadCursorA", u_LoadCursorA}, {"GetWindowLongA", u_GetWindowLongA}, {"GetClassLongA", u_GetClassLongA},
    {"GetClassNameA", u_GetClassNameA}, {"GetWindowTextA", u_GetWindowTextA}, {"SetWindowTextA", u_SetWindowTextA},
    {"SetWindowsHookExA", u_SetWindowsHookExA}, {"CallNextHookEx", u_CallNextHookEx},
    {"UnhookWindowsHookEx", u_true1}, {"EnumChildWindows", u_EnumChildWindows},
    {"EnumDisplayDevicesA", u_EnumDisplayDevicesA}, {"GetWindow", u_GetWindow}, {"SendInput", u_SendInput},
    {"GetAsyncKeyState", u_GetAsyncKeyState}, {"FindWindowA", u_FindWindowA}, {"SwapMouseButton", u_SwapMouseButton},
    {"GetStockObject", g_GetStockObject}, {"InitCommonControlsEx", c_InitCommonControlsEx},
    {"ImageList_LoadImageA", c_ImageList_LoadImageA}, {"ImageList_Destroy", c_ImageList_Destroy},
    {"CommandLineToArgvW", sh_CommandLineToArgvW}, {"ShellExecuteA", sh_ShellExecuteA},
    {"CoInitialize", o_CoInitialize}, {"CoInitializeEx", o_CoInitializeEx}, {"CoUninitialize", o_CoUninitialize},
    {"CoCreateInstance", o_CoCreateInstance}, {"CoFreeUnusedLibraries", o_CoFreeUnusedLibraries},
    {"CoTaskMemAlloc", o_CoTaskMemAlloc}, {"CoTaskMemFree", o_CoTaskMemFree},
    {"mmioOpenA", m_mmioOpenA}, {"mmioClose", m_mmioClose}, {"mmioDescend", m_mmioDescend},
    {"mmioAscend", m_mmioAscend}, {"mmioAdvance", m_mmioAdvance}, {"mmioGetInfo", m_mmioGetInfo}, {"mmioRead", m_mmioRead},
    {"WSAStartup", w_WSAStartup}, {"WSAGetLastError", w_WSAGetLastError}, {"socket", w_socket},
    {"closesocket", w_err1}, {"accept", w_err3}, {"bind", w_err3}, {"connect", w_err3}, {"listen", w_err2},
    {"ioctlsocket", w_err3}, {"recv", w_err4}, {"send", w_err4}, {"select", w_err5}, {"setsockopt", w_err5},
    {"WSAAsyncSelect", w_err4}, {"htons", w_htons}, {"ntohs", w_htons}, {"inet_addr", w_inet_addr},
    {"inet_ntoa", w_inet_ntoa}, {"gethostbyname", w_gethostbyname}, {"gethostname", w_gethostname},
    {"__WSAFDIsSet", w_FDIsSet},
    {"BinkOpen", b_BinkOpen}, {"BinkSetSoundSystem", b_BinkSetSoundSystem}, {"BinkOpenDirectSound", b_BinkOpenDirectSound},
    {"BinkClose", b_void1}, {"BinkDoFrame", b_void1}, {"BinkNextFrame", b_void1}, {"BinkWait", b_void1},
    {"BinkPause", b_void2}, {"BinkCopyToBufferRect", b_BinkCopyToBufferRect},
    {"XInputGetState", x_XInputGetState}, {"XInputSetState", x_XInputSetState},
    {"SteamAPI_Init", st_false}, {"SteamAPI_IsSteamRunning", st_false}, {"SteamAPI_RegisterCallback", st_false},
    {"SteamAPI_UnregisterCallback", st_false}, {"SteamAPI_RunCallbacks", st_false}, {"SteamAPI_Shutdown", st_false},
    {"SteamApps", st_false}, {"SteamFriends", st_false}, {"SteamUser", st_false}, {"SteamUserStats", st_false},
    {"ov_open_callbacks", ov_open_callbacks_}, {"ov_info", ov_info_}, {"ov_pcm_total", ov_pcm_total_},
    {"ov_read", ov_read_}, {"ov_clear", ov_clear_}, {"ov_seekable", ov_seekable_},
    {"SteamUtils", st_false}, {"D3DPERF_SetOptions", d_D3DPERF_SetOptions},
    {nullptr, nullptr}};
} // namespace

const PalEntry* pal_user_entries() { return kUser; }
void pal_vorbis_enable_decoder() { g_ov_decoder = true; }

void pal_user_set_screen(int w, int h) { g_screen_w = w; g_screen_h = h; }
void pal_user_post(uint32_t msg, uint32_t wp, uint32_t lp)
{
    std::lock_guard<std::mutex> lk(g_mx);
    g_queue.push_back({g_main_hwnd, msg, wp, lp});
}
uint32_t pal_user_main_window() { return g_main_hwnd; }
