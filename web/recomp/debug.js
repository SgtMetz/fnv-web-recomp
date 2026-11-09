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

// ---- settings and frame-time experiments (how web/recomp/tuner.js's levers were measured) ----------------------------
// A game setting ("fGrassStartFadeDistance:Grass") is an object {vtable, value, name*} in .data: its address is found
// through its name string in .rdata. Most settings are read once at start-up, so writing one often changes nothing.
const settingAt = {};
export function setting(name) {
  if (settingAt[name] !== undefined) return settingAt[name];
  const M = window.Module, H8 = M.HEAPU8, H32 = M.HEAPU32, b = new TextEncoder().encode(name + '\0');
  let str = 0;
  outer: for (let a = 0xfdf000; a < 0x1182000; a++) {
    if (H8[a] !== b[0]) continue;
    for (let i = 1; i < b.length; i++) if (H8[a + i] !== b[i]) continue outer;
    str = a; break;
  }
  let at = 0;
  if (str) for (let p = 0x1183000 >>> 2; p < (0x1272000 >>> 2); p++) if (H32[p] === str) { at = p * 4 - 8; break; }
  return (settingAt[name] = at);
}
export const getF = (n) => setting(n) ? window.Module.HEAPF32[(setting(n) + 4) >>> 2] : null;
export const getI = (n) => setting(n) ? window.Module.HEAPU32[(setting(n) + 4) >>> 2] : null;
export const setF = (n, v) => { if (setting(n)) window.Module.HEAPF32[(setting(n) + 4) >>> 2] = v; };
export const setI = (n, v) => { if (setting(n)) window.Module.HEAPU32[(setting(n) + 4) >>> 2] = v; };
export const con = (line) => window.Module.ccall('fnv_console', null, ['string'], [line]);

// one measurement: game-thread ms, renderer-pump ms and draws per frame. The mouse nudge keeps the idle camera
// (which orbits the player after a while and changes what is in view) from starting.
export async function measure(ms = 2500) {
  const M = window.Module, H32 = M.HEAPU32, w = M._fnv_perf() >>> 2, r = window.renderer, p = window.fnvPump;
  M._fnv_mouse(3, 0, 0, -1, 0); await sleep(60); M._fnv_mouse(-3, 0, 0, -1, 0); await sleep(800);
  const read = () => [0, 1, 2].map((i) => H32[w + 2 * i] + H32[w + 2 * i + 1] * 4294967296);
  const a = read(), d0 = r.stats.draws, f0 = r.frames, b0 = p.busyMs;
  await sleep(ms);
  const b = read(), fr = b[0] - a[0], rf = r.frames - f0;
  return { fps: fr * 1000 / ms, gameMs: (b[1] - a[1] - (b[2] - a[2])) / fr / 1000, waitMs: (b[2] - a[2]) / fr / 1000,
    draws: (r.stats.draws - d0) / rf, pumpMs: (p.busyMs - b0) / rf };
}

// average of several measurements, as text
export async function average(rounds = 4, ms = 2500) {
  const s = { fps: 0, gameMs: 0, waitMs: 0, draws: 0, pumpMs: 0 };
  for (let i = 0; i < rounds; i++) { const m = await measure(ms); for (const k in s) s[k] += m[k] / rounds; }
  return `${s.fps.toFixed(1)} fps, game ${s.gameMs.toFixed(1)} ms, wait ${s.waitMs.toFixed(1)} ms, pump ${s.pumpMs.toFixed(1)} ms, ${s.draws.toFixed(0)} draws`;
}

// A/B: alternates two states (the machine's speed drifts by 10% or more between single measurements) and averages each
export async function ab(a, b, rounds = 3, settle = 3000) {
  const sum = [{ gameMs: 0, pumpMs: 0, draws: 0 }, { gameMs: 0, pumpMs: 0, draws: 0 }];
  for (let i = 0; i < rounds; i++)
    for (const [k, f] of [[0, a], [1, b]]) { f(); await sleep(settle); const m = await measure(); for (const x in sum[k]) sum[k][x] += m[x] / rounds; }
  a();
  return sum.map((s) => `game ${s.gameMs.toFixed(1)} ms, pump ${s.pumpMs.toFixed(1)} ms, ${s.draws.toFixed(0)} draws`).join('  ->  ');
}
