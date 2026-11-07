// PAL sound group: DirectSound 8 as guest COM objects. Buffers are real memory whose play cursor advances in real time
// (the game's streaming threads poll it), so music and sound logic run as on Windows; nothing is played yet. A mixer
// can read the buffers later (pal_dsound_mix).
#include "pal_com.h"

#include <string.h>

#include <chrono>
#include <mutex>

namespace {
#define A(i) harg(s, (i))
#define RET(v, n) return hret(s, (uint32_t)(v), (n))

uint64_t now_ms()
{
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct Buffer
{
    uint32_t data = 0, size = 0, flags = 0;
    uint32_t rate = 22050, channels = 2, bits = 16, avg = 88200, block = 4;
    int32_t volume = 0, pan = 0;
    uint32_t freq = 0;
    bool playing = false, looping = false, primary = false;
    uint64_t started = 0;         // time of Play, for the play cursor
    uint32_t start_pos = 0;
    uint32_t obj = 0;             // the guest object (8 interface) and its 3D / notify views
    uint32_t obj3d = 0, notify = 0;
    float pos3d[3] = {0, 0, 0};
};
std::mutex g_mx;

Buffer* buf(FnvCpu& s) { return (Buffer*)com_host(A(0)); }

uint32_t play_pos(Buffer* b)
{
    if (!b->playing || !b->size) return b->start_pos;
    uint32_t rate = b->freq ? b->freq * b->block : b->avg;
    uint64_t bytes = (now_ms() - b->started) * rate / 1000 + b->start_pos;
    if (!b->looping && bytes >= b->size)
    {
        b->playing = false;
        b->start_pos = 0;
        return 0;
    }
    return (uint32_t)(bytes % b->size) & ~(b->block - 1);
}

// ---- IDirectSoundBuffer(8) ------------------------------------------------------------------------------------------
void b_QueryInterface(FnvCpu& s);
void b_GetCaps(FnvCpu& s)
{
    uint32_t c = A(1);
    M32(c + 4) = buf(s)->flags; M32(c + 8) = buf(s)->size; M32(c + 12) = 0; M32(c + 16) = 0;
    RET(0, 2);
}
void b_GetCurrentPosition(FnvCpu& s)
{
    std::lock_guard<std::mutex> lk(g_mx);
    Buffer* b = buf(s);
    uint32_t p = play_pos(b);
    if (A(1)) M32(A(1)) = p;
    if (A(2)) M32(A(2)) = b->size ? (p + b->avg / 50) % b->size : 0;   // write cursor ~20 ms ahead
    RET(0, 3);
}
void b_GetFormat(FnvCpu& s)
{
    Buffer* b = buf(s);
    if (A(1) && A(2) >= 16)
    {
        uint32_t w = A(1);
        memset(gbuf(w), 0, A(2) < 18 ? A(2) : 18);
        *(uint16_t*)gbuf(w) = 1; *(uint16_t*)gbuf(w + 2) = (uint16_t)b->channels; M32(w + 4) = b->rate;
        M32(w + 8) = b->avg; *(uint16_t*)gbuf(w + 12) = (uint16_t)b->block; *(uint16_t*)gbuf(w + 14) = (uint16_t)b->bits;
    }
    if (A(3)) M32(A(3)) = 18;
    RET(0, 4);
}
void b_GetVolume(FnvCpu& s) { M32(A(1)) = (uint32_t)buf(s)->volume; RET(0, 2); }
void b_GetPan(FnvCpu& s) { M32(A(1)) = (uint32_t)buf(s)->pan; RET(0, 2); }
void b_GetFrequency(FnvCpu& s) { M32(A(1)) = buf(s)->freq ? buf(s)->freq : buf(s)->rate; RET(0, 2); }
void b_GetStatus(FnvCpu& s)
{
    std::lock_guard<std::mutex> lk(g_mx);
    Buffer* b = buf(s);
    play_pos(b);
    M32(A(1)) = (b->playing ? 1u : 0u) | (b->playing && b->looping ? 4u : 0u);
    RET(0, 2);
}
void b_Lock(FnvCpu& s)
{
    Buffer* b = buf(s);
    uint32_t off = A(1), bytes = A(2), flags = A(7);
    if (flags & 2) { off = 0; bytes = b->size; }                        // DSBLOCK_ENTIREBUFFER
    if (flags & 1) { std::lock_guard<std::mutex> lk(g_mx); off = (play_pos(b) + b->avg / 50) % (b->size ? b->size : 1); }
    if (!b->size || bytes > b->size) RET(0x88780032u, 8);               // DSERR_INVALIDPARAM
    off %= b->size;
    uint32_t first = bytes <= b->size - off ? bytes : b->size - off;
    M32(A(3)) = b->data + off;
    M32(A(4)) = first;
    if (A(5)) M32(A(5)) = first < bytes ? b->data : 0;
    if (A(6)) M32(A(6)) = bytes - first;
    RET(0, 8);
}
void b_Unlock(FnvCpu& s) { RET(0, 5); }
void b_Play(FnvCpu& s)
{
    std::lock_guard<std::mutex> lk(g_mx);
    Buffer* b = buf(s);
    if (!b->playing) { b->started = now_ms(); }
    b->playing = true;
    b->looping = (A(3) & 1) != 0;                                        // DSBPLAY_LOOPING
    RET(0, 4);
}
void b_Stop(FnvCpu& s)
{
    std::lock_guard<std::mutex> lk(g_mx);
    Buffer* b = buf(s);
    b->start_pos = play_pos(b);
    b->playing = false;
    RET(0, 1);
}
void b_SetCurrentPosition(FnvCpu& s)
{
    std::lock_guard<std::mutex> lk(g_mx);
    Buffer* b = buf(s);
    b->start_pos = b->size ? A(1) % b->size : 0;
    b->started = now_ms();
    RET(0, 2);
}
void b_SetFormat(FnvCpu& s)
{
    Buffer* b = buf(s);
    uint32_t w = A(1);
    b->channels = *(uint16_t*)gbuf(w + 2); b->rate = M32(w + 4); b->avg = M32(w + 8);
    b->block = *(uint16_t*)gbuf(w + 12); b->bits = *(uint16_t*)gbuf(w + 14);
    RET(0, 2);
}
void b_SetVolume(FnvCpu& s) { buf(s)->volume = (int32_t)A(1); RET(0, 2); }
void b_SetPan(FnvCpu& s) { buf(s)->pan = (int32_t)A(1); RET(0, 2); }
void b_SetFrequency(FnvCpu& s) { buf(s)->freq = A(1); RET(0, 2); }
void b_Restore(FnvCpu& s) { RET(0, 1); }

// ---- IDirectSound3DBuffer / 3DListener / Notify (state only) -------------------------------------------------------
void d3_ok2(FnvCpu& s) { RET(0, 2); }
void d3_ok3(FnvCpu& s) { RET(0, 3); }
void d3_ok5(FnvCpu& s) { RET(0, 5); }
void d3_ok1(FnvCpu& s) { RET(0, 1); }
void d3_SetPosition(FnvCpu& s) { RET(0, 5); }                           // x, y, z, apply
void d3_GetAllParameters(FnvCpu& s) { uint32_t p = A(1); uint32_t size = M32(p); memset(gbuf(p + 4), 0, size > 4 ? size - 4 : 0); RET(0, 2); }
void n_SetNotificationPositions(FnvCpu& s) { RET(0, 3); }

ComClass* buffer_class()
{
    static ComClass* cls = com_class("IDirectSoundBuffer8", {
        {"QueryInterface", b_QueryInterface}, {"GetCaps", b_GetCaps}, {"GetCurrentPosition", b_GetCurrentPosition},
        {"GetFormat", b_GetFormat}, {"GetVolume", b_GetVolume}, {"GetPan", b_GetPan}, {"GetFrequency", b_GetFrequency},
        {"GetStatus", b_GetStatus}, {"Lock", b_Lock}, {"Unlock", b_Unlock}, {"Play", b_Play}, {"Stop", b_Stop},
        {"SetCurrentPosition", b_SetCurrentPosition}, {"SetFormat", b_SetFormat}, {"SetVolume", b_SetVolume},
        {"SetPan", b_SetPan}, {"SetFrequency", b_SetFrequency}, {"Restore", b_Restore}});
    return cls;
}

void b_QueryInterface(FnvCpu& s)
{
    Buffer* b = buf(s);
    uint32_t iid = M32(A(1)), out = A(2);
    if (iid == 0x279AFA86u || iid == 0x279AFA84u)                       // IDirectSound3DBuffer / 3DListener
    {
        static ComClass* c3b = com_class("IDirectSound3DBuffer", {
            {"GetAllParameters", d3_GetAllParameters}, {"SetPosition", d3_SetPosition}, {"SetAllParameters", d3_ok3},
            {"SetConeAngles", d3_ok5}, {"SetConeOrientation", d3_ok5}, {"SetConeOutsideVolume", d3_ok3},
            {"SetMaxDistance", d3_ok3}, {"SetMinDistance", d3_ok3}, {"SetMode", d3_ok3}, {"SetVelocity", d3_ok5}});
        static ComClass* c3l = com_class("IDirectSound3DListener", {
            {"GetAllParameters", d3_GetAllParameters}, {"SetPosition", d3_SetPosition}, {"SetAllParameters", d3_ok3},
            {"SetDistanceFactor", d3_ok3}, {"SetDopplerFactor", d3_ok3}, {"SetOrientation", [](FnvCpu& s) { hret(s, 0, 8); }},
            {"SetRolloffFactor", d3_ok3}, {"SetVelocity", d3_ok5}, {"CommitDeferredSettings", d3_ok1}});
        if (!b->obj3d) b->obj3d = com_new(iid == 0x279AFA86u ? c3b : c3l, b);
        M32(out) = b->obj3d;
        RET(0, 3);
    }
    if (iid == 0xb0210783u)                                              // IDirectSoundNotify
    {
        static ComClass* cn = com_class("IDirectSoundNotify", {{"SetNotificationPositions", n_SetNotificationPositions}});
        if (!b->notify) b->notify = com_new(cn, b);
        M32(out) = b->notify;
        RET(0, 3);
    }
    M32(out) = A(0);                                                     // IDirectSoundBuffer / 8: itself
    RET(0, 3);
}

// ---- IDirectSound8 ------------------------------------------------------------------------------------------------------
void s_CreateSoundBuffer(FnvCpu& s)
{
    uint32_t d = A(1);
    Buffer* b = new Buffer;
    b->flags = M32(d + 4);
    b->primary = (b->flags & 1) != 0;
    b->size = b->primary ? 65536 : M32(d + 8);
    uint32_t w = M32(d + 16);
    if (w)
    {
        b->channels = *(uint16_t*)gbuf(w + 2); b->rate = M32(w + 4); b->avg = M32(w + 8);
        b->block = *(uint16_t*)gbuf(w + 12); b->bits = *(uint16_t*)gbuf(w + 14);
    }
    if (!b->block) b->block = 4;
    if (!b->avg) b->avg = b->rate * b->block;
    b->data = (uint32_t)(uintptr_t)calloc(1, b->size ? b->size : 4);
    b->obj = com_new(buffer_class(), b);
    M32(A(2)) = b->obj;
    RET(0, 4);
}
void s_DuplicateSoundBuffer(FnvCpu& s)
{
    Buffer* o = (Buffer*)com_host(A(1));
    Buffer* b = new Buffer(*o);
    b->playing = false; b->start_pos = 0; b->obj3d = b->notify = 0;
    b->obj = com_new(buffer_class(), b);
    M32(A(2)) = b->obj;
    RET(0, 3);
}
void s_GetCaps(FnvCpu& s) { uint32_t c = A(1); uint32_t size = M32(c); memset(gbuf(c + 4), 0, size > 4 ? size - 4 : 0); M32(c + 4) = 0xf5f; RET(0, 2); }
void s_SetCooperativeLevel(FnvCpu& s) { RET(0, 3); }
void s_GetSpeakerConfig(FnvCpu& s) { M32(A(1)) = 4; RET(0, 2); }       // DSSPEAKER_STEREO
void s_ok1(FnvCpu& s) { RET(0, 1); }
void s_ok2(FnvCpu& s) { RET(0, 2); }

void p_DirectSoundCreate8(FnvCpu& s)
{
    static ComClass* cls = com_class("IDirectSound8", {
        {"CreateSoundBuffer", s_CreateSoundBuffer}, {"DuplicateSoundBuffer", s_DuplicateSoundBuffer},
        {"GetCaps", s_GetCaps}, {"SetCooperativeLevel", s_SetCooperativeLevel}, {"GetSpeakerConfig", s_GetSpeakerConfig},
        {"SetSpeakerConfig", s_ok2}, {"Compact", s_ok1}, {"Initialize", s_ok2}, {"VerifyCertification", s_ok2}});
    M32(A(1)) = com_new(cls, nullptr);
    RET(0, 3);
}

const PalEntry kSound[] = {{"DirectSoundCreate8", p_DirectSoundCreate8}, {"DirectSoundCreate", p_DirectSoundCreate8},
                           {nullptr, nullptr}};
} // namespace

const PalEntry* pal_sound_entries() { return kSound; }
