// Debugging helpers for web/recomp/index.html, loaded on demand from the console or tools:
//   const d = await import('/debug.js'); await d.snap(); await d.keys([0xd0, 0xd0, 0x1c]);
// snap() posts the back buffer to the dev server (out/recomp/snap.png); keys() taps DirectInput scan codes.
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

export async function snap(name) {
  const blob = await Promise.race([window.renderer.capture(), sleep(8000).then(() => null)]);
  if (!blob) return false;
  await fetch('/snap' + (name ? '?name=' + encodeURIComponent(name) : ''), { method: 'POST', body: blob });
  return true;
}

export async function keys(codes, gap = 400) {
  for (const c of codes) {
    window.Module._fnv_key(c, 1);
    await sleep(120);
    window.Module._fnv_key(c, 0);
    await sleep(gap);
  }
}

export async function mouse(dx, dy, steps = 10) {
  for (let i = 0; i < steps; i++) { window.Module._fnv_mouse(Math.round(dx / steps), Math.round(dy / steps), 0, -1, 0); await sleep(30); }
}

export async function click(button = 0) {
  window.Module._fnv_mouse(0, 0, 0, button, 1);
  await sleep(120);
  window.Module._fnv_mouse(0, 0, 0, button, 0);
}

export function status() {
  return document.getElementById('status').textContent;
}

// main menu -> New -> Yes (the menu takes about a minute to appear after the page loads). Load the page with
// ?start=normal: without it the page waits for the start-mode choice. (?start=quick needs none of this.)
export async function newGame(menuWaitMs = 65000) {
  await sleep(menuWaitMs);
  await keys([0xd0, 0xd0], 1200);
  await keys([0x1c], 2500);
  await keys([0x1c], 500);
}

// New Game into the world. DLC is off unless the server runs with --dlc; then pass popups = 6 to dismiss the
// DLC item pop-ups.
export async function intoGame(menuWaitMs = 65000, popups = 0) {
  await newGame(menuWaitMs);
  await sleep(20000);
  for (let i = 0; i < popups; i++) await keys([0x1c], 3500);
}

// sampling profile of the main game thread (Module._fnv_sample: the call sites on its guest stack, innermost first),
// for ms milliseconds: {samples, stacks: {"a b c": count}} with addresses in hex. Map them to functions with
// python tools/recomp_profile.py <file.json>.
export async function profile(ms = 10000, depth = 24) {
  const M = window.Module, buf = M._malloc(depth * 4), stacks = {};
  let samples = 0;
  const end = performance.now() + ms;
  while (performance.now() < end) {
    const n = M._fnv_sample(buf, depth);
    const k = Array.from(M.HEAPU32.subarray(buf >>> 2, (buf >>> 2) + n), (v) => v.toString(16)).join(' ');
    stacks[k] = (stacks[k] || 0) + 1;
    samples++;
    await sleep(1);
  }
  M._free(buf);
  return { samples, stacks };
}

// game-thread frame pacing over ms milliseconds (port/recomp/pal_d3d9.cpp dv_Present): fps, the frame time, and the
// part of it the game spends on its own work (the rest is waiting for the renderer)
export async function perf(ms = 5000) {
  const M = window.Module, w = M._fnv_perf() >>> 2;
  const read = () => [0, 1, 2].map((i) => M.HEAPU32[w + 2 * i] + M.HEAPU32[w + 2 * i + 1] * 4294967296);
  const a = read();
  await sleep(ms);
  const b = read(), frames = b[0] - a[0];
  return { fps: frames * 1000 / ms, frameMs: (b[1] - a[1]) / frames / 1000, gameMs: (b[1] - a[1] - (b[2] - a[2])) / frames / 1000,
    waitMs: (b[2] - a[2]) / frames / 1000 };
}

// JS profile of the page's thread (the renderer pump) for ms milliseconds, with the dev server's Document-Policy
// header: the self time of the top functions, as [name:line, percent]
export async function jsProfile(ms = 5000, top = 30) {
  const p = new Profiler({ sampleInterval: 1, maxBufferSize: 100000 });
  await sleep(ms);
  const t = await p.stop();
  const self = new Map();
  for (const s of t.samples) {
    const f = s.stackId == null ? null : t.frames[t.stacks[s.stackId].frameId];
    const k = f ? `${f.name || '(anon)'}:${f.line || 0}` : '(idle)';
    self.set(k, (self.get(k) || 0) + 1);
  }
  return [...self].sort((a, b) => b[1] - a[1]).slice(0, top).map(([k, n]) => [k, +(100 * n / t.samples.length).toFixed(1)]);
}
