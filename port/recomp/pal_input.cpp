// PAL input group: DirectInput8 as guest COM objects (pal_com.h). Keyboard and mouse report the host's input
// (pal_input_key / pal_input_mouse, fed by the browser later); with no input they report idle devices.
#include "pal_com.h"

#include <string.h>

#include <stdlib.h>

#include <chrono>
#include <deque>
#include <mutex>
#include <thread>

namespace {
#define A(i) harg(s, (i))
#define RET(v, n) return hret(s, (uint32_t)(v), (n))

struct Event { uint32_t ofs, data; };
struct Device
{
    bool mouse;
    std::deque<Event> events;      // buffered data (GetDeviceData)
};
std::mutex g_mx;
uint8_t g_keys[256];               // DIK_ scan codes, 0x80 = down
int32_t g_dx, g_dy, g_dz;          // mouse deltas since the last GetDeviceState
uint8_t g_buttons[8];
Device g_keyboard{false, {}}, g_mouse{true, {}};
uint32_t g_seq;

Device* dev(FnvCpu& s) { return (Device*)com_host(A(0)); }

// ---- IDirectInputDevice8A ----------------------------------------------------------------------------------------
void d_ok1(FnvCpu& s) { RET(0, 1); }     // Acquire, Unacquire, Poll (this only)
void d_ok2(FnvCpu& s) { RET(0, 2); }     // SetDataFormat, SetEventNotification
void d_ok3(FnvCpu& s) { RET(0, 3); }     // SetCooperativeLevel, SetProperty, GetProperty
void d_GetCapabilities(FnvCpu& s)
{
    uint32_t c = A(1);
    uint32_t size = M32(c);
    memset(gbuf(c + 4), 0, size > 4 ? size - 4 : 0);
    M32(c + 4) = 0x5;                                    // DIDC_ATTACHED | DIDC_POLLEDDEVICE
    M32(c + 8) = dev(s)->mouse ? 0x12 : 0x13;            // DI8DEVTYPE_MOUSE / KEYBOARD
    M32(c + 12) = dev(s)->mouse ? 3 : 0;                 // axes
    M32(c + 16) = dev(s)->mouse ? 8 : 256;               // buttons
    RET(0, 2);
}
void d_GetDeviceState(FnvCpu& s)
{
    uint32_t cb = A(1), out = A(2);
    std::lock_guard<std::mutex> lk(g_mx);
    memset(gbuf(out), 0, cb);
    if (!dev(s)->mouse)
        memcpy(gbuf(out), g_keys, cb < 256 ? cb : 256);
    else
    {
        M32(out) = (uint32_t)g_dx; M32(out + 4) = (uint32_t)g_dy;
        if (cb >= 12) M32(out + 8) = (uint32_t)g_dz;
        for (uint32_t i = 0; i < 8 && 12 + i < cb; ++i) M8(out + 12 + i) = g_buttons[i];
        g_dx = g_dy = g_dz = 0;
    }
    RET(0, 3);
}
void d_GetDeviceData(FnvCpu& s)
{
    uint32_t cbo = A(1), rg = A(2), pn = A(3), flags = A(4);
    std::lock_guard<std::mutex> lk(g_mx);
    Device* d = dev(s);
    uint32_t want = M32(pn), n = 0;
    while (n < want && n < d->events.size())
    {
        const Event& e = d->events[n];
        if (rg)
        {
            uint32_t p = rg + n * cbo;
            memset(gbuf(p), 0, cbo);
            M32(p) = e.ofs; M32(p + 4) = e.data; M32(p + 12) = ++g_seq;
        }
        ++n;
    }
    if (!(flags & 1))                                    // DIGDD_PEEK leaves them
        d->events.erase(d->events.begin(), d->events.begin() + n);
    M32(pn) = n;
    RET(0, 5);
}
void d_GetDeviceInfo(FnvCpu& s)
{
    uint32_t p = A(1), size = M32(p);
    memset(gbuf(p + 4), 0, size > 4 ? size - 4 : 0);
    M32(p + 36) = dev(s)->mouse ? 0x12 : 0x13;
    RET(0, 2);
}

// ---- IDirectInput8A ------------------------------------------------------------------------------------------------
void i_CreateDevice(FnvCpu& s)
{
    static ComClass* cls = com_class("IDirectInputDevice8A", {
        {"GetCapabilities", d_GetCapabilities}, {"SetProperty", d_ok3}, {"GetProperty", d_ok3},
        {"Acquire", d_ok1}, {"Unacquire", d_ok1}, {"GetDeviceState", d_GetDeviceState},
        {"GetDeviceData", d_GetDeviceData}, {"SetDataFormat", d_ok2}, {"SetEventNotification", d_ok2},
        {"SetCooperativeLevel", d_ok3}, {"GetDeviceInfo", d_GetDeviceInfo}, {"Poll", d_ok1}});
    uint32_t guid = A(1);
    bool mouse = M32(guid) == 0x6F1D2B60u;               // GUID_SysMouse (GUID_SysKeyboard is ...61)
    M32(A(2)) = com_new(cls, mouse ? &g_mouse : &g_keyboard);
    RET(0, 4);
}
void i_EnumDevices(FnvCpu& s) { RET(0, 5); }            // no game controllers to report
void i_GetDeviceStatus(FnvCpu& s) { RET(M32(A(1)) == 0x6F1D2B60u || M32(A(1)) == 0x6F1D2B61u ? 0 : 0x80040154u, 2); }

void p_DirectInput8Create(FnvCpu& s)
{
    static ComClass* cls = com_class("IDirectInput8A", {
        {"CreateDevice", i_CreateDevice}, {"EnumDevices", i_EnumDevices}, {"GetDeviceStatus", i_GetDeviceStatus}});
    M32(A(3)) = com_new(cls, nullptr);
    RET(0, 5);
}

const PalEntry kInput[] = {{"DirectInput8Create", p_DirectInput8Create}, {nullptr, nullptr}};
} // namespace

const PalEntry* pal_input_entries() { return kInput; }

// FNV_INPUT_SCRIPT="ms:dik,ms:dik,..." (hex scan codes, delays from the previous step, the first from start-up): key
// taps for unattended runs, the same in the native host and the browser
void pal_input_script(const char* script)
{
    std::string sc(script);
    std::thread([sc]() {
        size_t i = 0;
        while (i < sc.size())
        {
            size_t comma = sc.find(',', i);
            std::string step = sc.substr(i, comma == std::string::npos ? std::string::npos : comma - i);
            i = comma == std::string::npos ? sc.size() : comma + 1;
            size_t colon = step.find(':');
            if (colon == std::string::npos) continue;
            std::this_thread::sleep_for(std::chrono::milliseconds(atoi(step.c_str())));
            int dik = (int)strtol(step.c_str() + colon + 1, nullptr, 16);
            pal_input_key(dik, true);
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            pal_input_key(dik, false);
        }
    }).detach();
}

// host input (browser): DirectInput scan code (DIK_*) down/up, mouse movement and buttons
void pal_input_key(int dik, bool down)
{
    std::lock_guard<std::mutex> lk(g_mx);
    g_keys[dik & 255] = down ? 0x80 : 0;
    g_keyboard.events.push_back({(uint32_t)(dik & 255), down ? 0x80u : 0u});
}
void pal_input_mouse(int dx, int dy, int dz, int button, bool down)
{
    std::lock_guard<std::mutex> lk(g_mx);
    g_dx += dx; g_dy += dy; g_dz += dz;
    if (dx) g_mouse.events.push_back({0, (uint32_t)dx});
    if (dy) g_mouse.events.push_back({4, (uint32_t)dy});
    if (dz) g_mouse.events.push_back({8, (uint32_t)dz});
    if (button >= 0 && button < 8)
    {
        g_buttons[button] = down ? 0x80 : 0;
        g_mouse.events.push_back({12u + (uint32_t)button, down ? 0x80u : 0u});
    }
}
