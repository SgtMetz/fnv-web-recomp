// Automatic quality: holds the frame rate near a target (60 fps) by trading view distance, and on devices whose
// graphics are the limit, resolution. index.html starts it once the game is ready; debug.js has the tools the levers
// were measured with.
//
// What it changes
//   view distance (live)   The game's fade table: the distance at which objects, items and actors fade out, the same
//                          values its own Object/Item/Actor Fade sliders set. The sliders' settings are copied into a
//                          table at 0x011ad7b8 (FUN_004506b0 -> SetFloatAt, exe 1.4.0.525), and that table is what the
//                          game reads while it plays, so writing it takes effect within seconds. Measured on Doc
//                          Mitchell's porch, from the top preset (15) to the game's own Low preset (2.5): 1,515 -> 940
//                          draws a frame, the game thread 20.0 -> 15.9 ms, the renderer pump 9.4 -> 5.3 ms.
//   grass (live)           fGrassStartFadeDistance, off at the two lowest levels (a few dozen draws).
//   resolution (next start) 1280x720 -> 960x540 -> 854x480, only when the game keeps waiting for the renderer at the
//                          lowest level (the graphics side is the limit, which view distance cannot fix). The game
//                          picks its size at start-up, so the choice is stored and index.html applies it on the next
//                          visit through FNV_INI (port/recomp/pal_kernel.cpp). The canvas scales the picture up.
// Tried and left out, because writing them changed nothing while the game ran or at start-up: shadows, water
// reflections and refractions, depth of field, SetClipDist, and uGridsToLoad (the game keeps 5).
//
// How it decides (every 2 s, only while the world is on screen and the page is visible, and not in the first 30 s
// after the world appears: cells, textures and shaders are still loading in then, and the frame rate says nothing
// about the device)
//   down  three windows in a row under 92% of the target, unless the display itself is the cap
//   up    ten windows in a row at the target with 12% of the frame to spare; a level that had to be left again
//         within a minute is not retried for a while (doubling up to 15 minutes), so it does not flip back and forth
// The level and resolution are remembered in localStorage (fnv.tuner.*). ?tuner=off turns it off for a visit, the
// "Auto quality" box for good; ?target=30 changes the target.

const FADE_TABLE = 0x011ad7b8;                 // float[12]: 0,1,4,5,8,10 objects; 2 items; 3 actors
const OBJECT_SLOTS = [0, 1, 4, 5, 8, 10], ITEM_SLOT = 2, ACTOR_SLOT = 3;
const FADE_CAPS = [Infinity, 9, 6, 4, 2.5];    // 2.5 is the game's Low preset
const GRASS_OFF_FROM = 3;                      // index into FADE_CAPS from which grass is off
const RES_STEPS = [[1280, 720], [960, 540], [854, 480]];
const WINDOW_MS = 2000;
const WORLD_DRAWS = 300;                       // fewer draws a frame than this: a menu or a loading screen, no verdict
const GRACE_MS = 30000;                        // no verdict this long after the world appears: it is still loading in

const store = {
  get(k, d) { try { const v = localStorage.getItem('fnv.tuner.' + k); return v === null ? d : v; } catch { return d; } },
  set(k, v) { try { localStorage.setItem('fnv.tuner.' + k, String(v)); } catch { /* private mode */ } },
};
const query = new URLSearchParams(location.search);

export function tunerEnabled() {
  return query.get('tuner') !== 'off' && store.get('on', '1') !== '0';
}

// the start-up part: settings for FNV_INI ('' for none), from what earlier visits measured
export function startupIni() {
  if (!tunerEnabled()) return '';
  const step = Math.max(0, Math.min(RES_STEPS.length - 1, +store.get('res', 0) || 0));
  if (!step) return '';
  const [w, h] = RES_STEPS[step];
  return `iSize W:Display=${w};iSize H:Display=${h}`;
}

// a setting object {vtable, value, name*} in .data, found through its name string in .rdata (0 when absent)
function findSetting(M, name) {
  const H8 = M.HEAPU8, H32 = M.HEAPU32, b = new TextEncoder().encode(name + '\0');
  let str = 0;
  outer: for (let a = 0xfdf000; a < 0x1182000; a++) {
    if (H8[a] !== b[0]) continue;
    for (let i = 1; i < b.length; i++) if (H8[a + i] !== b[i]) continue outer;
    str = a; break;
  }
  if (!str) return 0;
  for (let p = 0x1183000 >>> 2; p < (0x1272000 >>> 2); p++) if (H32[p] === str) return p * 4 - 8;
  return 0;
}

export function startTuner({ renderer, pump, log = () => {} }) {
  const M = window.Module;
  const target = Math.max(15, Math.min(240, +query.get('target') || 60));
  const t = window.fnvTuner = {
    enabled: tunerEnabled(), target, level: 0, levels: 1, label: '', fps: 0, gameMs: 0, waitMs: 0, pumpMs: 0, displayHz: 0,
    resStep: Math.max(0, Math.min(RES_STEPS.length - 1, +store.get('res', 0) || 0)), history: [],
    setEnabled(on) {
      t.enabled = !!on; store.set('on', on ? 1 : 0);
      if (!on) { apply(0); store.set('level', 0); store.set('res', 0); t.label = ''; }
      else lowRun = highRun = 0;
    },
  };
  if (!M || !M._fnv_perf || !M.HEAPF32) { t.enabled = false; return t; }
  const F32 = () => M.HEAPF32, H32 = () => M.HEAPU32;      // the heap views are replaced when memory grows
  const T = FADE_TABLE >>> 2;
  const sane = (v) => Number.isFinite(v) && v >= 0.1 && v <= 100;
  // the table as this exe build lays it out: every object slot holds the same sane value
  const tableOk = () => { const f = F32(); return OBJECT_SLOTS.every((i) => sane(f[T + i]) && f[T + i] === f[T + OBJECT_SLOTS[0]]) && sane(f[T + ITEM_SLOT]) && sane(f[T + ACTOR_SLOT]); };
  if (!tableOk()) { log('tuner: the fade table is not where this build expects it; automatic quality is off'); t.enabled = false; return t; }
  const grassAt = findSetting(M, 'fGrassStartFadeDistance:Grass');

  // the game's own values (what level 0 restores), and the levels below them that change anything
  let orig, levels, written = null, grassOrig = grassAt ? F32()[(grassAt + 4) >>> 2] : 0;
  function readOrig() {
    const f = F32();
    orig = { objects: f[T + OBJECT_SLOTS[0]], items: f[T + ITEM_SLOT], actors: f[T + ACTOR_SLOT] };
    levels = [];
    for (let i = 0; i < FADE_CAPS.length; i++) {
      const cap = FADE_CAPS[i];
      const l = { objects: Math.min(orig.objects, cap), items: Math.min(orig.items, cap), actors: Math.min(orig.actors, cap),
        grass: i < GRASS_OFF_FROM || !(grassOrig > 0) };
      const last = levels[levels.length - 1];
      if (!last || last.objects !== l.objects || last.items !== l.items || last.actors !== l.actors || last.grass !== l.grass) levels.push(l);
    }
    t.levels = levels.length;
  }
  function apply(level) {
    t.level = Math.max(0, Math.min(levels.length - 1, level));
    const l = levels[t.level], f = F32();
    for (const i of OBJECT_SLOTS) f[T + i] = l.objects;
    f[T + ITEM_SLOT] = l.items; f[T + ACTOR_SLOT] = l.actors;
    if (grassAt && grassOrig > 0) f[(grassAt + 4) >>> 2] = l.grass ? grassOrig : 0;
    written = l;
    t.label = levels.length > 1 && t.level ? `view distance ${levels.length - t.level}/${levels.length}` : '';
  }
  // the game rewrote the table (its settings menu, a loaded game): those are the player's values now
  function resync() {
    const f = F32();
    if (written && f[T + OBJECT_SLOTS[0]] === written.objects && f[T + ITEM_SLOT] === written.items && f[T + ACTOR_SLOT] === written.actors) return;
    if (!tableOk()) return;
    if (grassAt && written && written.grass) grassOrig = f[(grassAt + 4) >>> 2];
    readOrig();
    apply(t.level);
  }
  readOrig();
  apply(t.enabled ? +store.get('level', 0) || 0 : 0);
  if (t.level) log(`tuner: starting at ${t.label} (remembered from the last visit)`);

  // display rate: animation frames per window (none while the page is hidden)
  let rafCount = 0;
  const onRaf = () => { rafCount++; requestAnimationFrame(onRaf); };
  requestAnimationFrame(onRaf);

  const perf = () => { const h = H32(), w = M._fnv_perf() >>> 2; return [0, 1, 2].map((i) => h[w + 2 * i] + h[w + 2 * i + 1] * 4294967296); };
  let last = { t: performance.now(), p: perf(), draws: renderer.stats.draws, frames: renderer.frames, busy: pump.busyMs || 0, paced: pump.pacedMs || 0, raf: 0 };
  let lowRun = 0, highRun = 0, boundRun = 0, settle = 2, steppedUpAt = 0, worldSince = 0;
  const holdUntil = [], holdMs = [];          // per level: when stepping up from it may be tried again
  const note = (m) => { t.history.push(`${new Date().toISOString().slice(11, 19)} ${m}`); if (t.history.length > 40) t.history.shift(); log('tuner: ' + m); };

  function tick() {
    const now = performance.now();
    const cur = { t: now, p: perf(), draws: renderer.stats.draws, frames: renderer.frames, busy: pump.busyMs || 0, paced: pump.pacedMs || 0, raf: rafCount };
    const dt = cur.t - last.t, frames = cur.p[0] - last.p[0], shown = cur.frames - last.frames;
    const prev = last;
    last = cur;
    if (!t.enabled || document.hidden || dt > WINDOW_MS * 2 || shown < 1) { lowRun = highRun = boundRun = 0; worldSince = 0; return; }
    // the world is on screen (a hitch of a second or two while it loads in still counts as on screen)
    const inWorld = (cur.draws - prev.draws) / shown >= WORLD_DRAWS;
    if (!inWorld) worldSince = 0;
    else if (!worldSince) worldSince = now;
    if (frames < 4 || shown < 4) { lowRun = highRun = boundRun = 0; return; }
    const fps = frames * 1000 / dt, frameMs = (cur.p[1] - prev.p[1]) / frames / 1000, waitMs = (cur.p[2] - prev.p[2]) / frames / 1000;
    const gameMs = frameMs - waitMs, pumpMs = (cur.busy - prev.busy) / shown, draws = (cur.draws - prev.draws) / shown;
    const hz = (cur.raf - prev.raf) * 1000 / dt;
    Object.assign(t, { fps, gameMs, waitMs, pumpMs, displayHz: hz });
    resync();
    if (settle > 0) { settle--; return; }
    if (!inWorld || now - worldSince < GRACE_MS) { lowRun = highRun = boundRun = 0; return; }
    const budget = 1000 / target;
    // the display (or the browser's frame pacing) is the cap: the game waits for it and nothing here would help
    const displayCapped = hz > 10 && hz < target * 0.95 && fps >= hz * 0.92;
    const low = fps < target * 0.92 && !displayCapped;
    // the game spends a good part of each frame waiting for the renderer: the graphics side is the limit. (Not the
    // time the page holds a finished frame back for its place in the cadence: that is the frame pacing, index.html.)
    const pacedMs = (cur.paced - prev.paced) / shown;
    const rendererBound = low && waitMs - pacedMs > frameMs * 0.25;
    // one level costs the game thread roughly 5-8% (20% over the four), so 12% in hand is room for one
    const spare = !low && gameMs < budget * 0.88 && pumpMs < budget * 0.7;
    lowRun = low ? lowRun + 1 : 0;
    highRun = spare ? highRun + 1 : 0;
    boundRun = rendererBound ? boundRun + 1 : 0;

    if (lowRun >= 3) {
      lowRun = 0;
      if (t.level < levels.length - 1) {
        // a level that was just tried and did not hold: wait longer before trying it again
        if (steppedUpAt && now - steppedUpAt < 60000) {
          holdMs[t.level + 1] = Math.min(900000, (holdMs[t.level + 1] || 60000) * 2);
          holdUntil[t.level + 1] = now + holdMs[t.level + 1];
        }
        steppedUpAt = 0;
        apply(t.level + 1); store.set('level', t.level); settle = 2;
        note(`${fps.toFixed(0)} fps (game ${gameMs.toFixed(1)} ms, renderer ${pumpMs.toFixed(1)} ms): ${t.label}`);
      } else if (boundRun >= 3 && t.resStep < RES_STEPS.length - 1 && +store.get('res', 0) === t.resStep) {
        store.set('res', t.resStep + 1);
        const [w, h] = RES_STEPS[t.resStep + 1];
        note(`${fps.toFixed(0)} fps at the lowest view distance, waiting ${waitMs.toFixed(1)} ms a frame for the renderer: ${w}x${h} from the next start`);
        t.pendingRes = `${w}x${h}`;
      }
    } else if (highRun >= 10 && t.level > 0 && !(holdUntil[t.level] > now)) {
      highRun = 0;
      apply(t.level - 1); store.set('level', t.level); settle = 2; steppedUpAt = now;
      note(`${fps.toFixed(0)} fps with time to spare (game ${gameMs.toFixed(1)} ms): ${t.label || 'full view distance'}`);
    }
  }
  setInterval(tick, WINDOW_MS);
  return t;
}
