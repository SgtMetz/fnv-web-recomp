#!/usr/bin/env node
// Frame-time run of the browser build in a real, visible browser window (web/recomp/bench.js does the recording).
// A hidden tab gets no animation frames and slow timers, so numbers from one say nothing about what a player sees:
// this opens its own window with background throttling off and drives it over the DevTools protocol.
//
//   node server/serve.mjs --dev --game <install>            (in another terminal)
//   node tools/bench_web.mjs [--seconds 45] [--fresh] [--query "pace=off"] [--label name] [--settle 20]
//
//   --fresh    a new browser profile: no compiled-shader cache and no HTTP cache (what a first visit is like)
//   --query    extra page parameters, e.g. "pace=off" or "FNV_INI=..."
//   --out      also write the summary to this JSON file
// Needs puppeteer-core (npm install --prefix local/bench puppeteer-core) and Edge or Chrome.
import fs from 'node:fs';
import path from 'node:path';
import { createRequire } from 'node:module';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const args = process.argv.slice(2);
const opt = (name, def) => { const i = args.indexOf(name); return i >= 0 ? args[i + 1] : def; };
const seconds = +opt('--seconds', 45), settle = +opt('--settle', 20);
const fresh = args.includes('--fresh');
const label = opt('--label', fresh ? 'fresh' : 'warm');
const url = `${opt('--url', 'http://localhost:8093/')}?start=quick&tuner=off${opt('--query') ? '&' + opt('--query') : ''}`;

const require = createRequire(path.join(ROOT, 'local/bench/'));
const puppeteer = require('puppeteer-core');
const exe = [process.env.BROWSER, 'C:/Program Files/Google/Chrome/Application/chrome.exe',
  'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe', 'C:/Program Files/Microsoft/Edge/Application/msedge.exe']
  .find((p) => p && fs.existsSync(p));
if (!exe) { console.error('no Chrome or Edge found (set BROWSER)'); process.exit(2); }
const profile = path.join(ROOT, 'local/bench', fresh ? 'profile-fresh' : 'profile');
if (fresh) fs.rmSync(profile, { recursive: true, force: true });

// Edge hands over to a second process and the first one exits, so the browser is started here and attached to
// through its debugging port, instead of puppeteer's own launcher (which takes that exit for a failed start).
const port = +opt('--debug-port', 9333);
const child = spawn(exe, [`--remote-debugging-port=${port}`, `--user-data-dir=${profile}`, '--window-size=1400,950', '--window-position=40,40',
  '--no-first-run', '--no-default-browser-check', '--disable-backgrounding-occluded-windows', '--disable-renderer-backgrounding',
  '--disable-background-timer-throttling', '--autoplay-policy=no-user-gesture-required', 'about:blank'], { detached: true, stdio: 'ignore' });
child.unref();
let browser = null;
for (let i = 0; i < 60 && !browser; i++) {
  await new Promise((r) => setTimeout(r, 500));
  try { browser = await puppeteer.connect({ browserURL: `http://127.0.0.1:${port}`, defaultViewport: null }); } catch { /* not up yet */ }
}
if (!browser) { console.error('the browser did not open its debugging port'); process.exit(2); }
try {
  const page = (await browser.pages())[0] || await browser.newPage();
  const errors = [];
  page.on('pageerror', (e) => errors.push(String(e).slice(0, 300)));
  const consoleTail = [];
  page.on('console', (m) => { consoleTail.push(`${m.type()}: ${m.text().slice(0, 240)}`); if (consoleTail.length > 400) consoleTail.shift(); });
  const t0 = Date.now();
  await page.goto(url, { waitUntil: 'domcontentloaded' });
  await page.waitForFunction(() => window.fnvBoot && window.fnvBoot.ready && window.fnvBoot.stage >= 4, { timeout: 240000, polling: 500 });
  const loadS = (Date.now() - t0) / 1000;
  await new Promise((r) => setTimeout(r, settle * 1000));          // the porch: let the first cells finish loading in
  // The run itself happens with the DevTools connection closed (--attached keeps it open): an attached debugger
  // sees every request of every game thread, and that is not how a player runs the page.
  await page.evaluate((seconds, opts) => {
    window.fnvBenchResult = null;
    const wantGameProfile = opts.gameProfile; window.fnvBenchRun = (async () => {
    // the display's refresh interval, from animation frames
    const ts = [];
    await new Promise((res) => { const f = (t) => { ts.push(t); if (ts.length < 60) requestAnimationFrame(f); else res(); }; requestAnimationFrame(f); setTimeout(res, 3000); });
    const gaps = ts.slice(1).map((t, i) => t - ts[i]).sort((a, b) => a - b);
    const b = await import('/bench.js');
    const s = await b.run({ seconds });
    const st = b.install(), fr = st.frames, hist = {};
    for (const f of fr) { const k = Math.min(100, Math.floor(f.dt / 4) * 4); hist[k] = (hist[k] || 0) + 1; }
    const g = fr.map((f) => f.game).sort((x, y) => x - y), q = (p) => Math.round(g[Math.min(g.length - 1, Math.floor(g.length * p))] * 10) / 10;
    // is the game still producing frames after the run? (a walk can end in a stall, which the frame times alone hide)
    const f0 = window.renderer.frames;
    await new Promise((res) => setTimeout(res, 2000));
    const alive = window.renderer.frames - f0;
    const logEl = document.getElementById('log');
    // stalled: where every game thread is, and the command ring's counters (data, size, write, read, sent, done)
    let ringState = null;
    if (alive < 10) {
      try { window.Module._fnv_dump_threads(); } catch (e) { /* not exported */ }
      const M = window.Module, r = M._fnv_ring() >>> 2;
      ringState = { ring: Array.from(M.HEAPU32.subarray(r, r + 7)), boot: JSON.parse(JSON.stringify(window.fnvBoot)), pump: { ...window.fnvPump } };
      await new Promise((res) => setTimeout(res, 1500));
    }
    const logTail = logEl ? [logEl.textContent.slice(-12000)] : [];
    // where the page's own thread spends its time (the dev server's Document-Policy header allows the JS profiler)
    let js = null;
    try { const d = await import('/debug.js'); window.Module._fnv_key(0x11, 1); js = await d.jsProfile(6000, 16); window.Module._fnv_key(0x11, 0); } catch (e) { js = String(e); }
    // --gameprofile: what the game's main thread is running, sampled while walking (tools/recomp_profile.py reads it)
    let gameProfile = null;
    if (wantGameProfile) { const d = await import('/debug.js'); window.Module._fnv_key(0x11, 1); gameProfile = await d.profile(15000, 24); window.Module._fnv_key(0x11, 0); }
    const info = await navigator.gpu.requestAdapter().then((a) => a && a.info ? `${a.info.vendor} ${a.info.architecture} ${a.info.description || ''}` : '');
    return window.fnvBenchResult = { refreshMs: gaps.length ? Math.round(gaps[gaps.length >> 1] * 100) / 100 : null, rafSeen: ts.length, summary: s,
      game: { p50: q(0.5), p95: q(0.95), p99: q(0.99), max: q(1) }, dtHist: hist, pump: { ...window.fnvPump }, framesAfterRun2s: alive, logTail, ringState, js, pace: window.fnvPace ? JSON.parse(JSON.stringify(window.fnvPace)) : null, dtSeq: fr.slice(200, 520).map((f) => Math.round(f.dt)).join(' '), gameProfile,
      pipelines: window.renderer.pipelines.size, adapter: info, visible: document.visibilityState, status: document.getElementById('status').textContent };
      })().catch((e) => { window.fnvBenchResult = { failed: String(e && e.stack || e) }; });
  }, seconds, { gameProfile: args.includes('--gameprofile') });
  const extra = args.includes('--gameprofile') ? 26 : 8;
  if (!args.includes('--attached')) {
    await browser.disconnect();
    await new Promise((r) => setTimeout(r, (seconds + extra) * 1000));
    browser = await puppeteer.connect({ browserURL: `http://127.0.0.1:${port}`, defaultViewport: null });
  }
  const page2 = (await browser.pages()).find((p) => p.url().startsWith(url.split('?')[0])) || page;
  await page2.waitForFunction(() => window.fnvBenchResult, { timeout: (seconds + 60) * 1000, polling: 500 });
  const result = await page2.evaluate(() => window.fnvBenchResult);
  if (result.failed) throw new Error(result.failed);
  if (result.gameProfile) { fs.writeFileSync(path.join(ROOT, 'out/bench', label + '-gameprofile.json'), JSON.stringify(result.gameProfile)); console.log(`  game-thread profile: ${result.gameProfile.samples} samples -> out/bench/${label}-gameprofile.json`); delete result.gameProfile; }
  const out = { label, url, browser: path.basename(exe), loadS: Math.round(loadS * 10) / 10, ...result, errors };
  const s = out.summary;
  console.log(`[${label}] loaded in ${out.loadS} s; display ${out.refreshMs} ms/refresh (${out.rafSeen} animation frames seen); ${out.adapter}`);
  console.log(`  ${s.frames} frames in ${s.seconds} s = ${s.fps} fps | ms avg ${s.ms.avg} p50 ${s.ms.p50} p95 ${s.ms.p95} p99 ${s.ms.p99} worst ${s.ms.worst}`);
  console.log(`  frames over 33 ms: ${s.over[33]}, over 50 ms: ${s.over[50]}, over 100 ms: ${s.over[100]} | game thread ms p50 ${out.game.p50} p95 ${out.game.p95} p99 ${out.game.p99} max ${out.game.max}`);
  console.log(`  slow-frame time ${s.slowTimeMs} ms, explained (ms): ${JSON.stringify(s.blameMs)}`);
  console.log(`  totals: ${JSON.stringify(s.totals)} | pump ${JSON.stringify(out.pump)}`);
  if (out.pace) console.log(`  pacing: ${JSON.stringify(out.pace)}`);
  console.log(`  per frame: ${JSON.stringify(s.perFrame)}`);
  console.log(`  page thread (JS self time %): ${JSON.stringify(out.js)}`);
  console.log('  frame-time histogram (ms: frames): ' + Object.entries(out.dtHist).map(([k, v]) => `${k}:${v}`).join(' '));
  console.log('  worst frames:');
  for (const f of s.worst) console.log('   ' + JSON.stringify(f));
  if (errors.length) console.log('  page errors: ' + errors.slice(0, 5).join(' | '));
  if (s.seconds < seconds * 0.9 || out.framesAfterRun2s < 10) {
    console.log(`  STALLED: frames cover ${s.seconds} s of a ${seconds} s run; ${out.framesAfterRun2s} frames in the 2 s after it. Log tail:`);
    console.log('    ' + JSON.stringify(out.ringState));
    for (const l of out.logTail) console.log(l.slice(-9000).split('[web] thread').join('\n[web] thread'));
    for (const l of consoleTail.slice(-12)) console.log('    console ' + l);
  }
  if (opt('--out')) fs.writeFileSync(opt('--out'), JSON.stringify(out, null, 1));
} finally {
  await browser.close();
}
