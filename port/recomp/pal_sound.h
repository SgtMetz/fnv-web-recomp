// The PAL's audio sink (pal_dsound.cpp): the DirectSound mixer writes 48 kHz stereo float frames into a ring in
// shared memory, and the page's AudioWorklet (web/recomp/audio-worklet.js) reads them. Counters are free-running frame
// counts; the ring index is counter % frames. The mixer keeps write - read at about `target` frames, so production
// follows consumption and the latency stays near target / 48 kHz.
#pragma once
#include <stdint.h>

static const uint32_t PAL_AUDIO_RATE = 48000;

struct PalAudioRing
{
    uint32_t data, frames;             // float[frames][2]
    volatile uint32_t write, read;     // frames produced / consumed
    uint32_t rate, target, enabled;
};
PalAudioRing* pal_sound_ring();
void pal_sound_enable(uint32_t frames, uint32_t target);
