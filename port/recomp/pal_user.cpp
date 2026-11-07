// PAL user group: user32 (one main window, its class, a message queue, window procedures called on the guest), gdi32,
// comctl32, shell32, ole32, winmm's mmio, and the libraries the browser build does without: WinSock (no network),
// Bink (no movies yet), XInput (no pad), the Steam/Galaxy wrapper (no client), DirectShow (CoCreateInstance fails).
#include "pal.h"

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

// ---- winmm mmio (WAV parsing for sounds: not available yet, callers treat it as a missing file) -------------------
void m_mmioOpenA(FnvCpu& s) { RET(0, 3); }
void m_mmioClose(FnvCpu& s) { RET(0, 2); }
void m_mmio_fail3(FnvCpu& s) { RET(0x105, 3); }              // MMIOERR_CANNOTREAD-like failures
void m_mmio_fail4(FnvCpu& s) { RET(0x105, 4); }
void m_mmioRead(FnvCpu& s) { RET(0xffffffffu, 3); }

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

// ---- libvorbisfile (cdecl): no Ogg decoding yet. Opening reports OV_ENOTVORBIS so the game skips the stream, and
// the queries answer a valid stereo 44.1 kHz description (the game divides by the rate). ----------------------------
uint32_t g_vorbis_info[8] = {0, 2, 44100, 0, 0, 0, 0, 0};    // vorbis_info: version, channels, rate, bitrates
void ov_open_callbacks_(FnvCpu& s) { RETC(0xFFFFFF7Cu); }   // -132
void ov_info_(FnvCpu& s) { RETC((uint32_t)(uintptr_t)g_vorbis_info); }
void ov_pcm_total_(FnvCpu& s) { s.edx = 0; RETC(0); }
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
    {"mmioOpenA", m_mmioOpenA}, {"mmioClose", m_mmioClose}, {"mmioDescend", m_mmio_fail4},
    {"mmioAscend", m_mmio_fail3}, {"mmioAdvance", m_mmio_fail3}, {"mmioGetInfo", m_mmio_fail3}, {"mmioRead", m_mmioRead},
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
    {"ov_read", ov_zero_}, {"ov_clear", ov_zero_}, {"ov_seekable", ov_zero_},
    {"SteamUtils", st_false}, {"D3DPERF_SetOptions", d_D3DPERF_SetOptions},
    {nullptr, nullptr}};
} // namespace

const PalEntry* pal_user_entries() { return kUser; }

void pal_user_set_screen(int w, int h) { g_screen_w = w; g_screen_h = h; }
void pal_user_post(uint32_t msg, uint32_t wp, uint32_t lp)
{
    std::lock_guard<std::mutex> lk(g_mx);
    g_queue.push_back({g_main_hwnd, msg, wp, lp});
}
uint32_t pal_user_main_window() { return g_main_hwnd; }
