// Frame-time benchmark for web/recomp/index.html: what a player feels as stutter is the slow frames, not the
// average, so this records every frame and reports percentiles, the worst frame and how many ran over 33 ms, the
// way a scripted flyover is judged. Each slow frame carries what the page did in it, so the cause can be named.
//
//   const b = await import('/bench.js');
//   await b.run({ seconds: 60 })          walks forward and turns, then returns the summary (also window.fnvBench)
//   b.install(); ... b.summary()          record while playing by hand
//
// Per frame (Present to Present, on the page's thread):
//   dt        the time since the previous frame reached the canvas
//   game      the game thread's own work for a frame (port/recomp/pal_d3d9.cpp dv_Present), wait: its time held back
//   pipe      createRenderPipeline calls and their time (a new shader/state combination, compiled on the spot)
//   module    createShaderModule calls and their time
//   bg        createBindGroup calls and their time
//   tex       texture uploads: calls, megabytes, time (the copy out of the ring included)
//   buf       buffer uploads: megabytes, time
//   shaderWait  time a draw waited for its translated shader to arrive (/shader/<sha1>)
//   submit    queue.submit calls and their time; pass: render passes begun; wb/wt: queue.writeBuffer/writeTexture calls
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

let st = null;
const blank = () => ({ pipeN: 0, pipeMs: 0, modN: 0, modMs: 0, bgN: 0, bgMs: 0, texN: 0, texMB: 0, texMs: 0, bufMB: 0, bufMs: 0,
  shaderWaitMs: 0, subN: 0, subMs: 0, draws: 0, destroyed: 0, passN: 0, wbN: 0, wbMB: 0, wtN: 0 });

function timed(obj, name, note) {
  const orig = obj[name];
  obj[name] = function (...a) {
    const t0 = performance.now();
    const r = orig.apply(this, a);
    note(performance.now() - t0, a, r);
    return r;
  };
  return () => { obj[name] = orig; };
}

export function install() {
  if (st) return st;
  const r = window.renderer, dev = r.dev, M = window.Module;
  const perfAt = M._fnv_perf() >>> 2;
  const perf = () => [0, 1, 2].map((i) => M.HEAPU32[perfAt + 2 * i] + M.HEAPU32[perfAt + 2 * i + 1] * 4294967296);
  st = { frames: [], cur: blank(), last: performance.now(), lastPerf: perf(), lastDraws: r.stats.draws, undo: [], marks: [] };
  const c = () => st.cur;
  st.undo.push(timed(dev, 'createRenderPipeline', (ms) => { c().pipeN++; c().pipeMs += ms; }));
  st.undo.push(timed(dev, 'createShaderModule', (ms) => { c().modN++; c().modMs += ms; }));
  st.undo.push(timed(dev, 'createBindGroup', (ms) => { c().bgN++; c().bgMs += ms; }));
  st.undo.push(timed(dev.queue, 'submit', (ms) => { c().subN++; c().subMs += ms; }));
  // passes, and the queue writes behind the uploads (a frame's constants and dynamic vertices go up as one write
  // per submit, so writes far above submits mean resource uploads)
  st.undo.push(timed(GPUCommandEncoder.prototype, 'beginRenderPass', () => { c().passN++; }));
  st.undo.push(timed(dev.queue, 'writeBuffer', (ms, a) => { c().wbN++; c().wbMB += (a[4] ?? (a[2].byteLength || 0)) / 1048576; }));
  st.undo.push(timed(dev.queue, 'writeTexture', () => { c().wtN++; }));
  st.undo.push(timed(r, 'uploadTexture', (ms, a) => { c().texN++; c().texMB += a[1].length / 1048576; c().texMs += ms; }));
  st.undo.push(timed(r, 'uploadBuffer', (ms, a) => { c().bufMB += a[2] / 1048576; c().bufMs += ms; }));
  st.undo.push(timed(r, 'destroy', () => { c().destroyed++; }));
  // a draw that returns a promise is waiting for its shaders
  st.undo.push(timed(r, 'draw', (ms, a, ret) => {
    if (ret && ret.then) { const t0 = performance.now(), cur = c(); ret.then(() => { cur.shaderWaitMs += performance.now() - t0; }); }
  }));
  st.undo.push(timed(r, 'present', () => {
    const now = performance.now(), p = perf(), f = st.cur;
    const frames = Math.max(1, p[0] - st.lastPerf[0]);
    f.dt = now - st.last;
    f.game = (p[1] - st.lastPerf[1] - (p[2] - st.lastPerf[2])) / frames / 1000;
    f.wait = (p[2] - st.lastPerf[2]) / frames / 1000;
    f.draws = r.stats.draws - st.lastDraws;
    f.t = now;
    st.frames.push(f);
    st.cur = blank(); st.last = now; st.lastPerf = p; st.lastDraws = r.stats.draws;
  }));
  return st;
}

export function uninstall() { if (st) { for (const u of st.undo.reverse()) u(); st = null; } }
export function reset() { if (st) { st.frames = []; st.marks = []; st.cur = blank(); st.last = performance.now(); } }

const pct = (sorted, p) => sorted.length ? sorted[Math.min(sorted.length - 1, Math.floor(sorted.length * p))] : 0;
const r1 = (x) => Math.round(x * 10) / 10;

// the summary of the frames recorded so far; slow frames are those over `slowMs`
export function summary(slowMs = 33.4, top = 12) {
  if (!st || !st.frames.length) return null;
  const fr = st.frames, dts = fr.map((f) => f.dt).sort((a, b) => a - b);
  const total = dts.reduce((a, b) => a + b, 0);
  const slow = fr.filter((f) => f.dt > slowMs);
  const sum = (arr, k) => arr.reduce((a, f) => a + f[k], 0);
  // which part of each slow frame is explained by what, in total milliseconds
  const blame = { pipe: sum(slow, 'pipeMs'), module: sum(slow, 'modMs'), bindGroup: sum(slow, 'bgMs'), texture: sum(slow, 'texMs'),
    buffer: sum(slow, 'bufMs'), shaderWait: sum(slow, 'shaderWaitMs'), submit: sum(slow, 'subMs'),
    game: slow.reduce((a, f) => a + Math.max(0, f.game - 16.7), 0) };
  for (const k in blame) blame[k] = r1(blame[k]);
  const line = (f) => ({ at: r1((f.t - fr[0].t) / 1000), dt: r1(f.dt), game: r1(f.game), wait: r1(f.wait), draws: f.draws,
    pipe: f.pipeN ? `${f.pipeN} / ${r1(f.pipeMs)} ms` : '', module: f.modN ? `${f.modN} / ${r1(f.modMs)} ms` : '',
    bg: f.bgN ? `${f.bgN} / ${r1(f.bgMs)} ms` : '', tex: f.texN ? `${f.texN} / ${r1(f.texMB)} MB / ${r1(f.texMs)} ms` : '',
    buf: f.bufMB > 0.01 ? `${r1(f.bufMB)} MB / ${r1(f.bufMs)} ms` : '', shaderWait: r1(f.shaderWaitMs), submit: r1(f.subMs), destroyed: f.destroyed });
  return {
    frames: fr.length, seconds: r1(total / 1000), fps: r1(fr.length * 1000 / total),
    ms: { avg: r1(total / fr.length), p50: r1(pct(dts, 0.5)), p95: r1(pct(dts, 0.95)), p99: r1(pct(dts, 0.99)), worst: r1(dts[dts.length - 1]) },
    over: { 33: fr.filter((f) => f.dt > 33.4).length, 50: fr.filter((f) => f.dt > 50).length, 100: fr.filter((f) => f.dt > 100).length },
    slowTimeMs: r1(sum(slow, 'dt')), blameMs: blame,
    totals: { pipelines: sum(fr, 'pipeN'), pipelineMs: r1(sum(fr, 'pipeMs')), modules: sum(fr, 'modN'), bindGroups: sum(fr, 'bgN'), bindGroupMs: r1(sum(fr, 'bgMs')),
      textureMB: r1(sum(fr, 'texMB')), textureMs: r1(sum(fr, 'texMs')), gameAvg: r1(sum(fr, 'game') / fr.length), drawsAvg: Math.round(sum(fr, 'draws') / fr.length) },
    perFrame: { passes: r1(sum(fr, 'passN') / fr.length), submits: r1(sum(fr, 'subN') / fr.length), submitMs: r1(sum(fr, 'subMs') / fr.length * 100) / 100,
      writeBuffers: r1(sum(fr, 'wbN') / fr.length), writeBufferKB: Math.round(sum(fr, 'wbMB') * 1024 / fr.length), writeTextures: r1(sum(fr, 'wtN') / fr.length),
      bindGroupsMade: r1(sum(fr, 'bgN') / fr.length), maxPasses: Math.max(...fr.map((f) => f.passN)), maxSubmits: Math.max(...fr.map((f) => f.subN)) },
    worst: [...fr].sort((a, b) => b.dt - a.dt).slice(0, top).map(line),
    marks: st.marks,
  };
}

// A scripted run: walk forward (W) and turn the camera steadily, which keeps new cells, textures and shader
// combinations arriving, the case that stutters. `turn` is mouse counts per step (30 ms).
export async function run({ seconds = 60, walk = true, turn = 6, slowMs = 33.4 } = {}) {
  install(); reset();
  const M = window.Module, end = performance.now() + seconds * 1000;
  if (walk) M._fnv_key(0x11, 1);
  try {
    let i = 0;
    while (performance.now() < end) {
      // turn one way for 10 s, then back, so the walk covers ground instead of circling
      const dir = Math.floor(i / 333) % 2 ? -1 : 1;
      if (turn) M._fnv_mouse(dir * turn, 0, 0, -1, 0);
      i++;
      await sleep(30);
    }
  } finally {
    if (walk) M._fnv_key(0x11, 0);
  }
  return (window.fnvBench = summary(slowMs));
}
