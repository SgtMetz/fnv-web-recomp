// PAL sound group: DirectSound 8 as guest COM objects. Buffers are real memory. Without an audio sink (node, native
// tests) the play cursor advances in real time, so music and sound logic run as on Windows and nothing is heard.
// With a sink (the browser: pal_sound_enable), a mixer thread mixes every playing buffer into a 48 kHz stereo float
// ring that web/recomp/audio-worklet.js plays; the play cursors then follow the mixer, so streaming stays in step with
// what is heard. Volume, pan, frequency and 3D distance attenuation are applied; 3D panning is not.
#include "pal_com.h"
#include "pal_sound.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

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
    uint32_t* data_refs = nullptr; // buffers sharing `data` (DuplicateSoundBuffer); the last Release frees it
    uint32_t rate = 22050, channels = 2, bits = 16, avg = 88200, block = 4;
    int32_t volume = 0, pan = 0;
    uint32_t freq = 0;
    bool playing = false, looping = false, primary = false;
    uint64_t started = 0;         // time of Play, for the play cursor
    uint32_t start_pos = 0;
    uint32_t obj = 0;             // the guest object (8 interface) and its 3D / notify views
    uint32_t obj3d = 0, notify = 0;
    float pos3d[3] = {0, 0, 0};
    float min_dist = 1, max_dist = 1e9f;
    uint32_t mode3d = 0;          // DS3DMODE_NORMAL 0, HEADRELATIVE 1, DISABLE 2
    bool is3d = false, dead = false;
    double mixpos = 0;            // mixer read position in frames
};
std::mutex g_mx;
std::vector<Buffer*> g_buffers;   // every secondary buffer, for the mixer
float g_listener[3] = {0, 0, 0};
PalAudioRing g_audio;             // the sink ring (pal_sound.h)

Buffer* buf(FnvCpu& s) { return (Buffer*)com_host(A(0)); }

uint32_t play_pos(Buffer* b)
{
    if (g_audio.enabled && !b->primary)                                  // the mixer owns the cursor
        return b->size ? ((uint32_t)b->mixpos * b->block) % b->size : 0;
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
    static int logged = 0;
    if (logged < 3)
    {
        ++logged;
        char m[200];
        snprintf(m, sizeof m, "[dsound] Play %08x: %u Hz %u ch %u bit, %u bytes, vol %d, flags %08x, 3d %d%c", A(0), b->rate,
                 b->channels, b->bits, b->size, b->volume, b->flags, b->is3d ? 1 : 0, 10);
        fnv_host_log(m);
    }
    if (!b->playing) { b->started = now_ms(); b->mixpos = b->block ? b->start_pos / b->block : 0; }
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
    b->mixpos = b->block ? b->start_pos / b->block : 0;
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
float fl(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
void d3_SetPosition(FnvCpu& s)                                           // x, y, z, apply
{
    Buffer* b = buf(s);
    float* dst = b->primary ? g_listener : b->pos3d;
    dst[0] = fl(A(1)); dst[1] = fl(A(2)); dst[2] = fl(A(3));
    RET(0, 5);
}
void d3_SetMinDistance(FnvCpu& s) { buf(s)->min_dist = fl(A(1)); RET(0, 3); }
void d3_SetMaxDistance(FnvCpu& s) { buf(s)->max_dist = fl(A(1)); RET(0, 3); }
void d3_SetMode(FnvCpu& s) { buf(s)->mode3d = A(1); RET(0, 3); }
void d3_GetAllParameters(FnvCpu& s) { uint32_t p = A(1); uint32_t size = M32(p); memset(gbuf(p + 4), 0, size > 4 ? size - 4 : 0); RET(0, 2); }
void n_SetNotificationPositions(FnvCpu& s) { RET(0, 3); }

void b_Release(FnvCpu& s)
{
    uint32_t o = A(0);
    uint32_t r = com_refs(o) ? com_refs(o) - 1 : 0;
    M32(o + 8) = r;
    if (!r)
    {
        std::lock_guard<std::mutex> lk(g_mx);              // the mixer reads `data` under this lock
        Buffer* b = buf(s);
        b->playing = false;
        b->dead = true;
        // Free the samples once no duplicate shares them. Every sound the game plays is a buffer: keeping them filled
        // the 3 GB heap in a few minutes of play (problem report 20261113-IQTFGH, Aborted(OOM)).
        if (b->data_refs && !--*b->data_refs)
        {
            free((void*)(uintptr_t)b->data);
            pal_mem_add(PAL_MEM_SOUND, -(int64_t)b->size);
            delete b->data_refs;
        }
        b->data_refs = nullptr;
        b->data = 0;
        b->size = 0;                                       // a stray call after the release sees an empty buffer
    }
    RET(r, 1);
}

ComClass* buffer_class()
{
    static ComClass* cls = com_class("IDirectSoundBuffer8", {
        {"QueryInterface", b_QueryInterface}, {"Release", b_Release}, {"GetCaps", b_GetCaps}, {"GetCurrentPosition", b_GetCurrentPosition},
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
            {"SetMaxDistance", d3_SetMaxDistance}, {"SetMinDistance", d3_SetMinDistance}, {"SetMode", d3_SetMode},
            {"SetVelocity", d3_ok5}});
        static ComClass* c3l = com_class("IDirectSound3DListener", {
            {"GetAllParameters", d3_GetAllParameters}, {"SetPosition", d3_SetPosition}, {"SetAllParameters", d3_ok3},
            {"SetDistanceFactor", d3_ok3}, {"SetDopplerFactor", d3_ok3}, {"SetOrientation", [](FnvCpu& s) { hret(s, 0, 8); }},
            {"SetRolloffFactor", d3_ok3}, {"SetVelocity", d3_ok5}, {"CommitDeferredSettings", d3_ok1}});
        if (!b->obj3d) b->obj3d = com_new(iid == 0x279AFA86u ? c3b : c3l, b);
        if (iid == 0x279AFA86u) b->is3d = true;
        M32(out) = b->obj3d;
        ++M32(b->obj3d + 8);
        RET(0, 3);
    }
    if (iid == 0xb0210783u)                                              // IDirectSoundNotify
    {
        static ComClass* cn = com_class("IDirectSoundNotify", {{"SetNotificationPositions", n_SetNotificationPositions}});
        if (!b->notify) b->notify = com_new(cn, b);
        M32(out) = b->notify;
        ++M32(b->notify + 8);
        RET(0, 3);
    }
    M32(out) = A(0);                                                     // IDirectSoundBuffer / 8: itself
    ++M32(A(0) + 8);                                                     // COM: QueryInterface adds a reference
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
    b->data_refs = new uint32_t(1);
    pal_mem_add(PAL_MEM_SOUND, b->size);
    b->obj = com_new(buffer_class(), b);
    if (!b->primary) { std::lock_guard<std::mutex> lk(g_mx); g_buffers.push_back(b); }
    M32(A(2)) = b->obj;
    RET(0, 4);
}
void s_DuplicateSoundBuffer(FnvCpu& s)
{
    Buffer* o = (Buffer*)com_host(A(1));
    std::lock_guard<std::mutex> lk(g_mx);
    Buffer* b = new Buffer(*o);
    b->playing = false; b->start_pos = 0; b->obj3d = b->notify = 0;
    b->dead = false; b->mixpos = 0;
    if (b->data_refs) ++*b->data_refs;                   // shares the original's samples
    b->obj = com_new(buffer_class(), b);
    g_buffers.push_back(b);
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

// ---- mixer ---------------------------------------------------------------------------------------------------------------
float db_gain(int32_t hundredths) { return hundredths >= 0 ? 1.f : hundredths <= -10000 ? 0.f : powf(10.f, hundredths / 2000.f); }

float sample(Buffer* b, uint32_t frame, uint32_t ch)
{
    const uint8_t* d = (const uint8_t*)(uintptr_t)b->data + frame * b->block;
    uint32_t c = b->channels > 1 ? ch : 0;
    if (b->bits == 8) return (d[c] - 128) / 128.f;
    return ((const int16_t*)d)[c] / 32768.f;
}

void mix_chunk(float* out, uint32_t n)
{
    memset(out, 0, n * 8);
    std::lock_guard<std::mutex> lk(g_mx);
    // released buffers leave the mix list (their samples were freed by the last Release; the small Buffer and guest
    // object stay, so a stray call on a released object finds an empty buffer)
    g_buffers.erase(std::remove_if(g_buffers.begin(), g_buffers.end(), [](Buffer* b) { return b->dead; }), g_buffers.end());
    for (Buffer* b : g_buffers)
    {
        if (!b->playing || b->dead || !b->size || !b->block || (b->bits != 8 && b->bits != 16)) continue;
        uint32_t frames = b->size / b->block;
        float g = db_gain(b->volume), gl = g, gr = g;
        if (b->pan > 0) gl *= db_gain(-b->pan); else if (b->pan < 0) gr *= db_gain(b->pan);
        if (b->is3d && b->mode3d != 2)
        {
            float dx = b->pos3d[0], dy = b->pos3d[1], dz = b->pos3d[2];
            if (b->mode3d == 0) { dx -= g_listener[0]; dy -= g_listener[1]; dz -= g_listener[2]; }
            float dist = sqrtf(dx * dx + dy * dy + dz * dz);
            float att = dist > b->max_dist ? 0.f : dist <= b->min_dist ? 1.f : b->min_dist / dist;
            gl *= att; gr *= att;
        }
        if (gl < 1e-4f && gr < 1e-4f && !b->looping)
        {                                                                // inaudible: only advance
            b->mixpos += (double)(b->freq ? b->freq : b->rate) / PAL_AUDIO_RATE * n;
            if (b->mixpos >= frames) { b->playing = false; b->start_pos = 0; b->mixpos = 0; }
            continue;
        }
        double step = (double)(b->freq ? b->freq : b->rate) / PAL_AUDIO_RATE;
        double pos = b->mixpos;
        for (uint32_t i = 0; i < n; ++i)
        {
            uint32_t f = (uint32_t)pos;
            if (f >= frames)
            {
                if (!b->looping) { b->playing = false; b->start_pos = 0; pos = 0; break; }
                pos -= frames; f = (uint32_t)pos;
            }
            out[2 * i] += sample(b, f, 0) * gl;
            out[2 * i + 1] += sample(b, f, 1) * gr;
            pos += step;
        }
        if (b->playing && b->looping && pos >= frames) pos = fmod(pos, frames);
        b->mixpos = pos;
    }
}

void mixer_thread()
{
    const uint32_t chunk = 256;
    std::vector<float> tmp(chunk * 2);
    uint32_t last_read = 0;
    uint64_t last_change = now_ms(), last_tick = now_ms();
    for (;;)
    {
        uint32_t w = __atomic_load_n(&g_audio.write, __ATOMIC_ACQUIRE), r = __atomic_load_n(&g_audio.read, __ATOMIC_ACQUIRE);
        uint64_t t = now_ms();
        if (r != last_read) { last_read = r; last_change = t; }
        else if (t - last_change > 150 && t > last_tick)
        {                                   // no consumer (audio not started yet, page hidden): keep real time ourselves
            uint32_t adv = (uint32_t)((t - last_tick) * PAL_AUDIO_RATE / 1000);
            r = w - r < adv ? w : r + adv;
            __atomic_store_n(&g_audio.read, r, __ATOMIC_RELEASE);
            last_read = r;
        }
        last_tick = t;
        if (w - r + chunk > g_audio.target)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        mix_chunk(tmp.data(), chunk);
        float* ring = (float*)(uintptr_t)g_audio.data;
        for (uint32_t i = 0; i < chunk; ++i)
        {
            uint32_t at = (w + i) % g_audio.frames;
            float l = tmp[2 * i], rr = tmp[2 * i + 1];
            ring[2 * at] = l > 1.f ? 1.f : l < -1.f ? -1.f : l;
            ring[2 * at + 1] = rr > 1.f ? 1.f : rr < -1.f ? -1.f : rr;
        }
        __atomic_store_n(&g_audio.write, w + chunk, __ATOMIC_RELEASE);
    }
}

const PalEntry kSound[] = {{"DirectSoundCreate8", p_DirectSoundCreate8}, {"DirectSoundCreate", p_DirectSoundCreate8},
                           {nullptr, nullptr}};
} // namespace

const PalEntry* pal_sound_entries() { return kSound; }

PalAudioRing* pal_sound_ring() { return &g_audio; }
void pal_sound_enable(uint32_t frames, uint32_t target)
{
    g_audio.frames = frames;
    g_audio.data = (uint32_t)(uintptr_t)calloc(frames, 8);
    g_audio.target = target;
    g_audio.rate = PAL_AUDIO_RATE;
    g_audio.write = g_audio.read = 0;
    g_audio.enabled = 1;
    std::thread(mixer_thread).detach();
}

// debugging (the page's console: Module._fnv_sound_debug()): every playing buffer to the log
extern "C"
#ifdef __EMSCRIPTEN__
__attribute__((used, visibility("default")))
#endif
void fnv_sound_debug()
{
    std::lock_guard<std::mutex> lk(g_mx);
    char m[256];
    snprintf(m, sizeof m, "[dsound] %u buffers, listener %.0f %.0f %.0f, ring w %u r %u%c", (unsigned)g_buffers.size(),
             g_listener[0], g_listener[1], g_listener[2], g_audio.write, g_audio.read, 10);
    fnv_host_log(m);
    for (Buffer* b : g_buffers)
        if (b->playing && !b->dead)
        {
            snprintf(m, sizeof m, "[dsound]   %08x %u Hz %u ch %u bit %u bytes vol %d pan %d freq %u loop %d 3d %d mode %u pos %.0f %.0f %.0f min %.1f max %.1f at %.0f%c",
                     b->obj, b->rate, b->channels, b->bits, b->size, b->volume, b->pan, b->freq, b->looping, b->is3d, b->mode3d,
                     b->pos3d[0], b->pos3d[1], b->pos3d[2], b->min_dist, b->max_dist, b->mixpos, 10);
            fnv_host_log(m);
        }
}
