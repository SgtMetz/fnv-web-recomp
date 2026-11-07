// Plays the game's mixed sound (port/recomp/pal_sound.h PalAudioRing) from the wasm heap. The page posts the heap's
// SharedArrayBuffer and the ring's address; each 128-frame quantum takes what the mixer has written and advances the
// ring's read counter, which paces the mixer. Every ~0.5 s it reports the level back (the page shows it).
class FnvAudio extends AudioWorkletProcessor {
  constructor() {
    super();
    this.u32 = null;
    this.peak = 0; this.frames = 0; this.under = 0;
    this.port.onmessage = (e) => {
      this.u32 = new Uint32Array(e.data.buffer);
      this.f32 = new Float32Array(e.data.buffer);
      this.ring = e.data.ring >>> 2;
    };
  }
  process(inputs, outputs) {
    const out = outputs[0], L = out[0], R = out[1] || out[0];
    const u = this.u32;
    if (!u) return true;
    const b = this.ring;
    const data = u[b] >>> 2, frames = u[b + 1];
    if (!data || !frames) return true;
    const w = Atomics.load(u, b + 2), r = Atomics.load(u, b + 3);
    const avail = (w - r) >>> 0;
    const n = Math.min(L.length, avail);
    for (let i = 0; i < n; i++) {
      const at = ((r + i) >>> 0) % frames;
      const l = this.f32[data + 2 * at], rr = this.f32[data + 2 * at + 1];
      L[i] = l; R[i] = rr;
      const a = Math.abs(l) > Math.abs(rr) ? Math.abs(l) : Math.abs(rr);
      if (a > this.peak) this.peak = a;
    }
    for (let i = n; i < L.length; i++) { L[i] = 0; R[i] = 0; }
    if (n < L.length) this.under++;
    Atomics.store(u, b + 3, (r + n) >>> 0);
    this.frames += L.length;
    if (this.frames >= 24000) {
      this.port.postMessage({ peak: this.peak, underruns: this.under });
      this.frames = 0; this.peak = 0;
    }
    return true;
  }
}
registerProcessor('fnv-audio', FnvAudio);
