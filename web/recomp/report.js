// "Report a problem" (web/recomp/index.html): what the player says went wrong, sent together with everything needed to
// find the cause, to the address the server keeps (the page never knows it). A classic script loaded in <head>, before
// the game's module, so it records from the first moment and still works when the game itself never starts (no
// WebGPU, a failed download, a crash while compiling).
//
// Recorded all along:
//   the log      every line the game, the renderer and the page print (Module.print/printErr through the page's
//                log()), console.*, uncaught errors and rejections, failed resource loads, and page events (focus,
//                visibility, pointer lock, full screen, resize, online/offline, WebGPU device loss). Each line has
//                its time since the page opened. The first 3,000 lines and the last 12,000 are kept.
//   samples      every 5 s: frames, fps, the start-up state, the wasm's and JavaScript's memory, the pump's busy time
//                (the last 30 minutes)
// Gathered when the report is sent:
//   the browser, the device, the GPU (WebGPU adapter info, features and limits; the WebGL renderer string), the
//   connection, storage, the build (VERSION.json), the page's settings, the start-up state (window.fnvBoot), the
//   renderer, sound and pump state, the game's present timings, where every game thread is (fnv_dump_threads) and
//   samples of the main thread's stack, the player's position and cell, the saves in this tab; a screenshot of the
//   game (optional) and the newest save (optional).
//
// It is sent as multipart form data to the URL in <meta name="fnv-report">: the development server keeps it in
// out/reports/ (server/serve.mjs); a deployment points the meta at its own receiver. When sending fails, the player
// can download the same report as one file.
(() => {
  'use strict';
  const T0 = performance.now(), OPENED = new Date();
  const HEAD = 3000, TAIL = 12000, LINE_MAX = 4000;
  const head = [], tail = new Array(TAIL);
  let tailN = 0, total = 0;
  const errors = [];          // the last 300 error lines, for the summary
  const ERRORISH = /error|fail|abort|exception|trap|unreachable|out of memory|cannot|invalid|lost|timed? ?out|assert/i;
  const stamp = () => ((performance.now() - T0) / 1000).toFixed(2).padStart(9);
  function add(kind, text) {
    text = String(text);
    if (text.length > LINE_MAX) text = text.slice(0, LINE_MAX) + ` …(+${text.length - LINE_MAX} chars)`;
    const line = `${stamp()} ${kind} ${text}`;
    total++;
    if (head.length < HEAD) head.push(line); else tail[tailN++ % TAIL] = line;
    // the game's stderr ('2') is mostly ordinary logging: only lines that read as failures count as errors
    if (kind === 'E' || kind === 'X' || (kind === '2' && ERRORISH.test(text))) { errors.push(line); if (errors.length > 300) errors.shift(); }
  }
  function logText() {
    const n = Math.min(tailN, TAIL), first = tailN > TAIL ? tailN % TAIL : 0;
    const rest = [];
    for (let i = 0; i < n; i++) rest.push(tail[(first + i) % TAIL]);
    const dropped = total - head.length - n;
    return head.join('\n') + (dropped > 0 ? `\n…… ${dropped} lines left out ……\n` : '\n') + rest.join('\n') + '\n';
  }
  const describe = (v) => {
    if (v instanceof Error) return (v.stack && v.stack.includes(v.message) ? v.stack : `${v.name}: ${v.message}\n${v.stack || ''}`).trim();
    if (typeof v === 'string') return v;
    try { return JSON.stringify(v); } catch { return String(v); }
  };

  // ---- recording ------------------------------------------------------------------------------------------------------
  // kinds: '-' game/page output, '2' the game's stderr, 'E' an error, 'X' uncaught error, 'C' console, 'P' page event
  for (const level of ['log', 'info', 'warn', 'error', 'debug']) {
    const orig = console[level].bind(console);
    console[level] = (...a) => {
      try { add(level === 'error' ? 'E' : 'C', `console.${level}: ${a.map(describe).join(' ')}`); } catch {}
      orig(...a);
    };
  }
  addEventListener('error', (e) => {
    const t = e.target;
    if (t && t !== window && (t.src || t.href)) add('X', `failed to load <${t.tagName.toLowerCase()}> ${t.src || t.href}`);
    else add('X', `uncaught: ${e.error ? describe(e.error) : e.message} (${e.filename || '?'}:${e.lineno || 0}:${e.colno || 0})`);
  }, true);
  addEventListener('unhandledrejection', (e) => add('X', 'unhandled rejection: ' + describe(e.reason)));
  addEventListener('securitypolicyviolation', (e) => add('E', `blocked by policy ${e.violatedDirective}: ${e.blockedURI}`));
  document.addEventListener('visibilitychange', () => add('P', 'visibility: ' + document.visibilityState));
  addEventListener('focus', () => add('P', 'window focus'));
  addEventListener('blur', () => add('P', 'window blur'));
  addEventListener('online', () => add('P', 'online'));
  addEventListener('offline', () => add('P', 'offline'));
  addEventListener('pagehide', () => add('P', 'pagehide'));
  let resizeT = 0;
  addEventListener('resize', () => { clearTimeout(resizeT); resizeT = setTimeout(() => add('P', `resize ${innerWidth}x${innerHeight} @${devicePixelRatio}`), 300); });
  document.addEventListener('pointerlockchange', () => add('P', 'pointer lock: ' + (document.pointerLockElement ? 'on' : 'off')));
  document.addEventListener('pointerlockerror', () => add('E', 'pointer lock refused'));
  document.addEventListener('fullscreenchange', () => add('P', 'full screen: ' + (document.fullscreenElement ? 'on' : 'off')));
  // long main-thread stalls (the page's thread runs the renderer: a stall there is a frozen picture)
  try {
    new PerformanceObserver((l) => { for (const e of l.getEntries()) if (e.duration >= 400) add('P', `long task ${e.duration.toFixed(0)} ms`); })
      .observe({ type: 'longtask', buffered: true });
  } catch {}

  // samples: the last 360 (30 minutes at one per 5 s)
  const samples = [];
  let lastFrames = 0, lastT = performance.now();
  setInterval(() => {
    try {
      const now = performance.now(), r = window.renderer, b = window.fnvBoot, M = window.Module;
      const frames = r ? r.frames : 0;
      const s = { t: +((now - T0) / 1000).toFixed(1), frames, fps: +((frames - lastFrames) * 1000 / (now - lastT)).toFixed(1) };
      lastFrames = frames; lastT = now;
      if (b) Object.assign(s, { phase: b.phase, stage: b.stage, kb: b.kb, fetches: b.fetches, retries: b.retries, ready: b.ready });
      if (r && r.stats) Object.assign(s, { draws: r.stats.draws, skipped: r.stats.skipped, pipelines: r.pipelines && r.pipelines.size });
      if (M && M.HEAPU8) s.heapMB = Math.round(M.HEAPU8.length / 1048576);
      if (performance.memory) s.jsMB = Math.round(performance.memory.usedJSHeapSize / 1048576);
      if (window.fnvPump) { s.pumpBusyMs = Math.round(window.fnvPump.busyMs || 0); s.packets = window.fnvPump.packets; }
      if (window.fnvAudio) { s.audio = window.fnvAudio.ctx ? window.fnvAudio.ctx.state : 'none'; s.underruns = window.fnvAudio.underruns; }
      s.vis = document.visibilityState[0];
      samples.push(s);
      if (samples.length > 360) samples.shift();
    } catch {}
  }, 5000);

  // ---- gathering ------------------------------------------------------------------------------------------------------
  const timeout = (p, ms, v = null) => Promise.race([Promise.resolve(p).catch((e) => ({ error: describe(e) })), new Promise((r) => setTimeout(() => r(v), ms))]);
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
  const safe = (f) => { try { return f(); } catch (e) { return { error: describe(e) }; } };
  const plainLimits = (l) => { const o = {}; for (const k in l) if (typeof l[k] === 'number') o[k] = l[k]; return o; };
  const hex = (v) => '0x' + (v >>> 0).toString(16).padStart(8, '0');

  async function gpuInfo() {
    const g = { webgpu: !!navigator.gpu };
    if (window.fnvGpu) g.used = window.fnvGpu;            // the adapter the renderer took (index.html)
    else if (navigator.gpu) {
      const a = await timeout(navigator.gpu.requestAdapter({ powerPreference: 'high-performance' }), 4000);
      if (a && !a.error) g.adapter = { info: safe(() => ({ ...a.info, vendor: a.info.vendor, architecture: a.info.architecture, device: a.info.device, description: a.info.description })),
        features: [...a.features], limits: plainLimits(a.limits), fallback: a.isFallbackAdapter };
      else g.adapter = a || 'no adapter (timed out)';
    }
    g.preferredFormat = safe(() => navigator.gpu && navigator.gpu.getPreferredCanvasFormat());
    g.webgl = safe(() => {
      const c = document.createElement('canvas'), gl = c.getContext('webgl2') || c.getContext('webgl');
      if (!gl) return 'none';
      const ext = gl.getExtension('WEBGL_debug_renderer_info');
      const out = { version: gl.getParameter(gl.VERSION), vendor: ext ? gl.getParameter(ext.UNMASKED_VENDOR_WEBGL) : gl.getParameter(gl.VENDOR),
        renderer: ext ? gl.getParameter(ext.UNMASKED_RENDERER_WEBGL) : gl.getParameter(gl.RENDERER), maxTexture: gl.getParameter(gl.MAX_TEXTURE_SIZE) };
      gl.getExtension('WEBGL_lose_context')?.loseContext();
      return out;
    });
    return g;
  }

  async function browserInfo() {
    const n = navigator, c = n.connection;
    const ua = { userAgent: n.userAgent, platform: n.platform, language: n.language, languages: n.languages,
      hardwareConcurrency: n.hardwareConcurrency, deviceMemoryGB: n.deviceMemory, maxTouchPoints: n.maxTouchPoints,
      pointerCoarse: matchMedia('(pointer: coarse)').matches, cookieEnabled: n.cookieEnabled, pdfViewer: n.pdfViewerEnabled,
      timeZone: Intl.DateTimeFormat().resolvedOptions().timeZone, tzOffsetMin: new Date().getTimezoneOffset() };
    if (n.userAgentData) ua.uaData = await timeout(n.userAgentData.getHighEntropyValues(['platform', 'platformVersion', 'architecture', 'bitness', 'model', 'fullVersionList', 'mobile']), 2000);
    const screenInfo = { width: screen.width, height: screen.height, availWidth: screen.availWidth, availHeight: screen.availHeight,
      colorDepth: screen.colorDepth, dpr: devicePixelRatio, inner: [innerWidth, innerHeight], orientation: screen.orientation && screen.orientation.type };
    const conn = c ? { effectiveType: c.effectiveType, downlinkMbps: c.downlink, rttMs: c.rtt, saveData: c.saveData, type: c.type } : null;
    const storage = n.storage && n.storage.estimate ? await timeout(n.storage.estimate().then((e) => ({ usageMB: Math.round(e.usage / 1048576), quotaMB: Math.round(e.quota / 1048576) })), 2000) : null;
    const caps = { crossOriginIsolated, isSecureContext, sharedArrayBuffer: typeof SharedArrayBuffer !== 'undefined',
      atomicsWait: typeof Atomics !== 'undefined' && typeof Atomics.wait === 'function', audioWorklet: typeof AudioWorkletNode !== 'undefined',
      offscreenCanvas: typeof OffscreenCanvas !== 'undefined', wasmThreads: safe(() => WebAssembly.validate(new Uint8Array([0, 97, 115, 109, 1, 0, 0, 0, 5, 4, 1, 3, 1, 1]))),
      opfs: !!(n.storage && n.storage.getDirectory) };
    const memory = performance.memory ? { usedMB: Math.round(performance.memory.usedJSHeapSize / 1048576), totalMB: Math.round(performance.memory.totalJSHeapSize / 1048576),
      limitMB: Math.round(performance.memory.jsHeapSizeLimit / 1048576) } : null;
    const nav = safe(() => { const e = performance.getEntriesByType('navigation')[0]; return e ? { type: e.type, domContentLoadedMs: Math.round(e.domContentLoadedEventEnd), loadMs: Math.round(e.loadEventEnd), transferKB: Math.round(e.transferSize / 1024) } : null; });
    return { ua, screen: screenInfo, connection: conn, storage, caps, memory, navigation: nav, online: n.onLine };
  }

  // page resources: the slowest, the failed (HTTP status where the browser reports it) and totals by type
  function resources() {
    const all = performance.getEntriesByType('resource');
    const byType = {};
    for (const e of all) {
      const t = (byType[e.initiatorType] = byType[e.initiatorType] || { n: 0, kb: 0, ms: 0 });
      t.n++; t.kb += Math.round((e.transferSize || 0) / 1024); t.ms += Math.round(e.duration);
    }
    const short = (e) => ({ url: e.name.replace(location.origin, ''), ms: Math.round(e.duration), kb: Math.round((e.transferSize || 0) / 1024), status: e.responseStatus, type: e.initiatorType });
    return { count: all.length, byType, failed: all.filter((e) => e.responseStatus >= 400).slice(-40).map(short),
      slowest: [...all].sort((a, b) => b.duration - a.duration).slice(0, 25).map(short) };
  }

  // the game: wasm state, present timings, threads, the main thread's stack, the player
  async function gameInfo() {
    const M = window.Module;
    if (!M) return { module: 'none' };
    const g = { runtime: !!(window.fnvBoot && window.fnvBoot.runtime), heapMB: M.HEAPU8 ? Math.round(M.HEAPU8.length / 1048576) : 0 };
    if (!M.HEAPU32 || !g.runtime) return g;
    const H32 = M.HEAPU32, H8 = M.HEAPU8, F32 = M.HEAPF32, len = H8.length;
    const ok = (p, n = 4) => p >= 0x400000 && p + n <= len;
    const u32 = (p) => ok(p) ? H32[p >>> 2] : 0;
    g.present = safe(() => {
      if (!M._fnv_perf) return null;
      const p = M._fnv_perf() >>> 0, v = new BigUint64Array(H8.buffer, p, 3), n = Number(v[0]);
      return { presents: n, avgFrameMs: n ? +(Number(v[1]) / n / 1000).toFixed(2) : 0, avgPresentMs: n ? +(Number(v[2]) / n / 1000).toFixed(2) : 0 };
    });
    // the main game thread's stack, five times 200 ms apart (call sites, innermost first; tools/recomp_addr.py)
    g.mainStack = await safe(async () => {
      if (!M._fnv_sample) return null;
      const out = [], buf = M._malloc(256) >>> 0;
      for (let i = 0; i < 5; i++) {
        const n = M._fnv_sample(buf, 64);
        out.push(Array.from(M.HEAPU32.subarray(buf >>> 2, (buf >>> 2) + n), hex).join(' '));
        await sleep(200);
      }
      M._free(buf);
      return out;
    });
    // every game thread's return addresses: fnv_dump_threads prints them to the log ("[web] thread N: ...")
    g.threads = await safe(async () => {
      if (!M._fnv_dump_threads) return null;
      const before = total;
      M._fnv_dump_threads();
      await sleep(300);
      return (total - before) + ' lines in the log ([web] thread)';
    });
    // the player: PlayerCharacter* at 0x011dea3c (TESForm: refID +0x0c; TESObjectREFR: base +0x20, rotation +0x24,
    // position +0x30, parentCell +0x40; TESObjectCELL: worldspace +0xc0)
    g.player = safe(() => {
      const pc = u32(0x011dea3c);
      if (!ok(pc, 0x48)) return 'no player yet';
      const w = pc >>> 2, f = (o) => +F32[w + o / 4].toFixed(1);
      const p = { ref: hex(u32(pc + 0x0c)), pos: [f(0x30), f(0x34), f(0x38)], rotZ: +(F32[w + 0x2c / 4] * 180 / Math.PI).toFixed(0) };
      const cell = u32(pc + 0x40);
      if (ok(cell, 0xc4)) {
        p.cell = hex(u32(cell + 0x0c));
        const ws = u32(cell + 0xc0);
        p.worldspace = ok(ws, 0x10) ? hex(u32(ws + 0x0c)) : 'none (interior)';
      }
      return p;
    });
    return g;
  }

  function pageState() {
    const b = window.fnvBoot, r = window.renderer, a = window.fnvAudio, pump = window.fnvPump;
    const copy = (o) => safe(() => JSON.parse(JSON.stringify(o, (k, v) => (typeof v === 'function' ? undefined : v))));
    const st = {
      url: location.href, referrer: document.referrer, title: document.title, embedded: window.parent !== window,
      status: safe(() => document.getElementById('status').textContent), visibility: document.visibilityState,
      focus: document.hasFocus(), pointerLock: !!document.pointerLockElement, fullscreen: !!document.fullscreenElement,
      startPanelOpen: safe(() => !document.getElementById('start').hidden), savesMsg: safe(() => document.getElementById('savesMsg').textContent),
      bootDetail: safe(() => document.getElementById('bootDetail').textContent),
      uptimeSec: +((performance.now() - T0) / 1000).toFixed(1), opened: OPENED.toISOString(),
      settings: safe(() => { const o = {}; for (let i = 0; i < localStorage.length; i++) { const k = localStorage.key(i); if (k.startsWith('fnv.')) o[k] = localStorage.getItem(k); } return o; }),
      env: window.Module && window.Module.fnvEnv, args: window.Module && window.Module.arguments,
    };
    st.boot = copy(b);
    st.pump = copy(pump);
    st.tuner = copy(window.fnvTuner);       // automatic quality (tuner.js): level, last frame times, what it changed
    if (a) st.audio = safe(() => ({ state: a.ctx ? a.ctx.state : 'none', sampleRate: a.ctx && a.ctx.sampleRate, baseLatency: a.ctx && a.ctx.baseLatency,
      peak: a.peak, underruns: a.underruns, decodes: a.decodes, tracks: a.tracks ? [...a.tracks.values()].map((t) => ({ path: t.path, playing: t.playing, duration: t.duration })) : [] }));
    if (r) st.renderer = safe(() => ({ frames: r.frames, stats: r.stats, pipelines: r.pipelines && r.pipelines.size,
      textures: r.textures && r.textures.size, warnings: r.warned ? [...r.warned].slice(-100) : [],
      canvas: [r.canvas ? r.canvas.width : 0, r.canvas ? r.canvas.height : 0] }));
    st.precheck = safe(() => window.fnvPrecheck ? { blockers: window.fnvPrecheck.result.blockers.map((b) => b.title),
      warnings: window.fnvPrecheck.result.warnings.map((w) => w.title), info: window.fnvPrecheck.result.info } : null);
    st.saves = safe(() => window.fnvSaves ? window.fnvSaves.list().map((f) => ({ name: f.path.replace(/^.*\//, ''), size: f.size, mtime: f.mtime })) : null);
    return st;
  }

  // the back buffer as a JPEG (at most 1280 wide), or null
  async function screenshot() {
    const r = window.renderer;
    if (!r || !r.frames || !r.capture) return null;
    const png = await timeout(r.capture(), 6000);
    if (!png || png.error) return null;
    const img = await createImageBitmap(png);
    const scale = Math.min(1, 1280 / img.width), c = document.createElement('canvas');
    c.width = Math.round(img.width * scale); c.height = Math.round(img.height * scale);
    c.getContext('2d').drawImage(img, 0, 0, c.width, c.height);
    return new Promise((res) => c.toBlob(res, 'image/jpeg', 0.85));
  }

  function summaryText(rep) {
    const L = [];
    const b = rep.page.boot || {}, ua = rep.browser.ua || {}, gpu = rep.gpu || {};
    const brands = ua.uaData && ua.uaData.fullVersionList ? ua.uaData.fullVersionList.filter((x) => !/Not.?A.?Brand/i.test(x.brand)).map((x) => `${x.brand} ${x.version}`).join(', ') : '';
    L.push(`Browser: ${brands || ua.userAgent}`);
    // Windows reports platformVersion 13 and up for Windows 11
    const win11 = ua.uaData && ua.uaData.platform === 'Windows' && parseInt(ua.uaData.platformVersion) >= 13 ? ' (Windows 11)' : '';
    if (ua.uaData) L.push(`OS: ${ua.uaData.platform} ${ua.uaData.platformVersion || ''}${win11} ${ua.uaData.architecture || ''}${ua.uaData.bitness ? '-' + ua.uaData.bitness : ''}${ua.uaData.model ? ' model ' + ua.uaData.model : ''}${ua.uaData.mobile ? ' (mobile)' : ''}`);
    L.push(`Device: ${ua.hardwareConcurrency} threads, ${ua.deviceMemoryGB || '?'} GB, screen ${rep.browser.screen.width}x${rep.browser.screen.height} @${rep.browser.screen.dpr}, touch ${ua.maxTouchPoints}`);
    const ad = gpu.used || gpu.adapter || {}, info = ad.info || {};
    L.push(`GPU: ${gpu.webgpu ? [info.vendor, info.architecture, info.device, info.description].filter(Boolean).join(' / ') || 'WebGPU (no adapter info)' : 'no WebGPU'}; WebGL ${gpu.webgl && gpu.webgl.renderer || '-'}`);
    L.push(`Page: ${rep.page.url}  up ${rep.page.uptimeSec} s, ${rep.page.visibility}, isolated ${rep.browser.caps.crossOriginIsolated}`);
    L.push(`Start-up: quality ${b.quality}, mode ${b.mode}, phase ${b.phase}, stage ${b.stage}, ready ${b.ready}, failed ${b.failed}${b.failText ? ' (' + b.failText + ')' : ''}, stalled ${b.stalled}, ${b.kb} KB in ${b.fetches} fetches, ${b.retries} retries`);
    L.push(`Status bar: ${rep.page.status}`);
    const s = rep.samples[rep.samples.length - 1];
    if (s) L.push(`Last sample: ${s.fps} fps, frame ${s.frames}, heap ${s.heapMB} MB, JS ${s.jsMB} MB`);
    if (rep.game.player && rep.game.player.cell) L.push(`Player: cell ${rep.game.player.cell}, worldspace ${rep.game.player.worldspace}, at ${rep.game.player.pos.join(', ')}`);
    if (rep.game.present && rep.game.present.presents) L.push(`Frames: ${rep.game.present.presents} presented, ${rep.game.present.avgFrameMs} ms average`);
    if (rep.build && typeof rep.build === 'object') L.push(`Build: ${rep.build.built || rep.build.version || JSON.stringify(rep.build).slice(0, 120)}`);
    L.push(`Log: ${rep.logLines} lines, ${rep.errorCount} error lines`);
    const errs = errors.slice(-20);
    if (errs.length) { L.push('', 'Last errors:'); for (const e of errs) L.push('  ' + e.slice(0, 300)); }
    return L.join('\n');
  }

  async function gather(opts) {
    add('P', 'report: gathering');
    const id = OPENED.toISOString().slice(0, 10).replace(/-/g, '') + '-' + Math.random().toString(36).slice(2, 8).toUpperCase();
    const [browser, gpu, game, build, shot] = await Promise.all([
      timeout(browserInfo(), 5000, { error: 'timed out' }), timeout(gpuInfo(), 6000, { error: 'timed out' }),
      timeout(gameInfo(), 6000, { error: 'timed out' }),
      timeout(fetch('VERSION.json', { cache: 'no-store' }).then((r) => (r.ok ? r.json() : 'HTTP ' + r.status)), 3000),
      opts.screenshot ? timeout(screenshot(), 8000) : null]);
    let save = null;
    if (opts.save && window.fnvSaves) save = safe(() => { const f = window.fnvSaves.list()[0]; if (!f) return null; const bytes = window.fnvSaves.read(f.path); return bytes ? { name: f.path.replace(/^.*\//, ''), bytes } : null; });
    if (save && save.error) save = null;
    const rep = { id, sent: new Date().toISOString(), description: opts.description, contact: opts.contact || '',
      page: pageState(), browser, gpu, game, build, resources: safe(resources), samples: samples.slice(),
      logLines: total, errorCount: errors.length, errors: errors.slice() };
    rep.summary = safe(() => summaryText(rep));
    add('P', 'report: gathered ' + id);
    return { rep, log: logText(), shot: shot && !shot.error ? shot : null, save };
  }

  // ---- confetti, when a report is sent -------------------------------------------------------------------------------
  // A burst from the middle of the screen. The canvas is a popover, so it draws above the modal dialog (the top layer);
  // without popovers it goes inside the dialog. Smaller when the system asks for reduced motion.
  function confetti(host) {
    const few = matchMedia('(prefers-reduced-motion: reduce)').matches ? 0.4 : 1;
    const c = document.createElement('canvas');
    c.style.cssText = 'position:fixed;inset:0;width:100vw;height:100vh;margin:0;padding:0;border:0;background:transparent;pointer-events:none;z-index:2147483647;overflow:visible';
    const popover = typeof c.showPopover === 'function';
    if (popover) { c.setAttribute('popover', 'manual'); document.body.appendChild(c); c.showPopover(); }
    else (host || document.body).appendChild(c);
    const dpr = Math.min(2, devicePixelRatio || 1), W = innerWidth, H = innerHeight;
    c.width = W * dpr; c.height = H * dpr;
    const g = c.getContext('2d');
    g.scale(dpr, dpr);
    const colors = ['#e0a040', '#ffd27a', '#f25f5c', '#70c1b3', '#5aa9e6', '#b388eb', '#9bd35a', '#ffffff'];
    const parts = [];
    const burst = (x, y, n, speed) => {
      for (let i = 0; i < n; i++) {
        const a = Math.random() * Math.PI * 2, v = speed * (0.35 + Math.random() * 0.65);
        parts.push({ x, y, vx: Math.cos(a) * v, vy: Math.sin(a) * v - speed * 0.35, w: 6 + Math.random() * 6, h: 3 + Math.random() * 5,
          r: Math.random() * Math.PI, vr: (Math.random() - 0.5) * 0.4, color: colors[(Math.random() * colors.length) | 0],
          wobble: Math.random() * 10, round: Math.random() < 0.25 });
      }
    };
    const s = Math.min(W, H) / 45;
    burst(W / 2, H / 2, Math.round(220 * few), s);                     // the explosion
    setTimeout(() => { burst(W * 0.2, H * 0.55, Math.round(70 * few), s * 0.8); burst(W * 0.8, H * 0.55, Math.round(70 * few), s * 0.8); }, 180);
    const t0 = performance.now(), LIFE = 3800;
    let last = t0;
    const frame = (now) => {
      const dt = Math.min(2.5, (now - last) / 16.7), age = now - t0;
      last = now;
      g.clearRect(0, 0, W, H);
      g.globalAlpha = age > LIFE - 900 ? Math.max(0, (LIFE - age) / 900) : 1;
      for (const p of parts) {
        p.vx *= Math.pow(0.985, dt); p.vy = p.vy * Math.pow(0.985, dt) + 0.28 * dt;
        p.x += (p.vx + Math.sin((age / 180) + p.wobble) * 0.6) * dt; p.y += p.vy * dt; p.r += p.vr * dt;
        g.save(); g.translate(p.x, p.y); g.rotate(p.r); g.fillStyle = p.color;
        if (p.round) { g.beginPath(); g.arc(0, 0, p.h * 0.7, 0, Math.PI * 2); g.fill(); }
        else g.fillRect(-p.w / 2, -p.h / 2 * Math.abs(Math.cos(p.r * 2)), p.w, p.h * Math.abs(Math.cos(p.r * 2)) + 0.5);
        g.restore();
      }
      if (age < LIFE) requestAnimationFrame(frame);
      else { if (popover) try { c.hidePopover(); } catch {} c.remove(); }
    };
    requestAnimationFrame(frame);
  }

  // ---- the dialog -----------------------------------------------------------------------------------------------------
  const CSS = `
  #fnvReport { max-width: 560px; width: calc(100% - 32px); padding: 18px 22px; color: var(--fg, #d8d2c4); background: #14130f;
    border: 1px solid #3a3528; border-radius: 6px; font: 13px/1.45 system-ui, sans-serif; }
  #fnvReport::backdrop { background: rgba(0, 0, 0, .7); }
  #fnvReport h2 { margin: 0 0 6px; font-size: 16px; color: var(--accent, #e0a040); font-weight: 600; }
  #fnvReport p { margin: 0 0 10px; color: var(--dim, #8a8478); }
  #fnvReport label { display: block; margin: 10px 0 4px; }
  #fnvReport textarea, #fnvReport input[type=email] { box-sizing: border-box; width: 100%; font: inherit; color: inherit; background: #0d0d0d;
    border: 1px solid #3a3528; border-radius: 4px; padding: 8px; }
  #fnvReport textarea { min-height: 130px; resize: vertical; }
  #fnvReport textarea:focus, #fnvReport input:focus { border-color: var(--accent, #e0a040); outline: none; }
  #fnvReport .check { display: flex; gap: 8px; align-items: baseline; margin: 6px 0 0; color: var(--dim, #8a8478); }
  #fnvReport details { margin: 10px 0 0; color: var(--dim, #8a8478); }
  #fnvReport .row { display: flex; gap: 10px; justify-content: flex-end; align-items: center; margin-top: 14px; flex-wrap: wrap; }
  #fnvReport .row .msg { flex: 1 1 200px; color: var(--dim, #8a8478); }
  #fnvReport .row .msg.bad { color: #e77; } #fnvReport .row .msg.good { color: #9c6; }
  #fnvReport button { font: inherit; color: inherit; background: #1e1c16; border: 1px solid #3a3528; border-radius: 4px; padding: 6px 14px; cursor: pointer; }
  #fnvReport button.primary { border-color: var(--accent, #e0a040); }
  #fnvReport button:hover:not(:disabled) { border-color: var(--accent, #e0a040); }
  #fnvReport button:disabled { opacity: .5; cursor: default; }
  #fnvReport [hidden] { display: none; }`;
  const HTML = `
  <form method="dialog">
    <h2>Report a problem</h2>
    <p>Say what went wrong. The game's log and details about your browser and device are sent with it, so it can be fixed.</p>
    <label for="fnvRepText">What happened? What were you doing just before?</label>
    <textarea id="fnvRepText" required minlength="5" maxlength="20000" placeholder="For example: after the loading screen leaving Doc Mitchell's house the picture froze, but the sound kept playing."></textarea>
    <label for="fnvRepContact">Your email, if you'd like a reply (optional)</label>
    <input id="fnvRepContact" type="email" maxlength="200" autocomplete="email" placeholder="you@example.com">
    <div class="check"><input type="checkbox" id="fnvRepShot" checked><label for="fnvRepShot" style="margin:0">Include a screenshot of the game</label></div>
    <div class="check" id="fnvRepSaveRow"><input type="checkbox" id="fnvRepSave" checked><label for="fnvRepSave" style="margin:0">Include my newest save (helps to repeat the problem)</label></div>
    <details><summary>What is sent</summary>
      Your description and email (if given); the game's log from this visit; your browser, operating system, screen, graphics card and
      connection type; the game's settings and loading state; where you are in the game; and, if ticked, the screenshot and save.
      Nothing else from your computer, and no passwords or files besides the save.</details>
    <div class="row">
      <span class="msg" id="fnvRepMsg" role="status"></span>
      <button type="button" id="fnvRepDownload" hidden>Download report</button>
      <button type="button" id="fnvRepCancel">Cancel</button>
      <button type="submit" id="fnvRepSend" class="primary" value="send">Send report</button>
    </div>
  </form>`;

  function ui() {
    const btn = document.getElementById('reportIssue');
    if (!btn) return;
    const style = document.createElement('style');
    style.textContent = CSS;
    document.head.appendChild(style);
    const dlg = document.createElement('dialog');
    dlg.id = 'fnvReport';
    dlg.innerHTML = HTML;
    document.body.appendChild(dlg);
    const $ = (id) => document.getElementById(id);
    const text = $('fnvRepText'), contact = $('fnvRepContact'), shotBox = $('fnvRepShot'), saveBox = $('fnvRepSave'), saveRow = $('fnvRepSaveRow');
    const msg = $('fnvRepMsg'), send = $('fnvRepSend'), cancel = $('fnvRepCancel'), dl = $('fnvRepDownload');
    const endpoint = (document.querySelector('meta[name="fnv-report"]') || {}).content || '/report';
    let last = null, sending = false;
    const say = (m, cls = '') => { msg.textContent = m; msg.className = 'msg ' + cls; };
    const open = () => {
      if (document.pointerLockElement) document.exitPointerLock();
      const hasSave = !!(window.fnvSaves && safe(() => window.fnvSaves.list().length) > 0);
      saveRow.hidden = !hasSave;
      if (!sending) { say(''); dl.hidden = true; send.disabled = false; }
      add('P', 'report: dialog opened');
      dlg.showModal();
      text.focus();
    };
    btn.addEventListener('click', open);
    window.fnvReport.open = open;
    cancel.addEventListener('click', () => dlg.close());
    // keys typed here must not reach the game (its handlers are on the canvas, but be sure)
    dlg.addEventListener('keydown', (e) => e.stopPropagation());
    dl.addEventListener('click', async () => {
      if (!last) return;
      const b64 = async (blob) => blob ? btoa(Array.from(new Uint8Array(await blob.arrayBuffer()), (c) => String.fromCharCode(c)).join('')) : null;
      const file = { ...last.rep, log: last.log, screenshotJpegBase64: await b64(last.shot),
        save: last.save ? { name: last.save.name, base64: await b64(new Blob([last.save.bytes])) } : null };
      const url = URL.createObjectURL(new Blob([JSON.stringify(file, null, 1)], { type: 'application/json' }));
      const a = document.createElement('a');
      a.href = url; a.download = `fnv-report-${last.rep.id}.json`;
      document.body.appendChild(a); a.click(); a.remove();
      setTimeout(() => URL.revokeObjectURL(url), 60000);
    });
    dlg.querySelector('form').addEventListener('submit', async (e) => {
      e.preventDefault();
      if (sending) return;
      const description = text.value.trim();
      if (description.length < 5) { say('Please describe the problem first.', 'bad'); text.focus(); return; }
      sending = true; send.disabled = true; dl.hidden = true;
      say('Gathering the details…');
      try {
        last = await gather({ description, contact: contact.value.trim(), screenshot: shotBox.checked, save: saveBox.checked && !saveRow.hidden });
        say('Sending…');
        const form = new FormData();
        form.append('id', last.rep.id);
        form.append('description', description);
        form.append('contact', last.rep.contact);
        form.append('summary', typeof last.rep.summary === 'string' ? last.rep.summary : '');
        form.append('report', new Blob([JSON.stringify(last.rep, null, 1)], { type: 'application/json' }), 'report.json');
        form.append('log', new Blob([last.log], { type: 'text/plain' }), 'log.txt');
        if (last.shot) form.append('screenshot', last.shot, 'screenshot.jpg');
        if (last.save) form.append('save', new Blob([last.save.bytes], { type: 'application/octet-stream' }), last.save.name);
        const ctl = new AbortController(), t = setTimeout(() => ctl.abort(), 60000);
        let r;
        try { r = await fetch(endpoint, { method: 'POST', body: form, signal: ctl.signal, mode: 'cors', credentials: 'omit' }); }
        finally { clearTimeout(t); }
        let answer = {};
        try { answer = await r.json(); } catch {}
        if (!r.ok || !answer.ok) throw new Error(answer.error || `the server answered ${r.status}`);
        add('P', 'report: sent ' + last.rep.id);
        say(`Sent, thank you. Reference ${answer.id || last.rep.id}.`, 'good');
        safe(() => confetti(dlg));
        text.value = '';
        send.disabled = true;
        setTimeout(() => { if (dlg.open && !sending) dlg.close(); }, 4000);
      } catch (err) {
        add('E', 'report: sending failed: ' + describe(err));
        say(`Could not send it (${err.name === 'AbortError' ? 'timed out' : err.message}). Try again, or download the report and send the file another way.`, 'bad');
        send.disabled = false;
        dl.hidden = !last;
      }
      sending = false;
    });
    dlg.addEventListener('close', () => { const c = document.getElementById('screen'); if (c) c.focus(); });
  }

  window.fnvReport = { add, logText, gather, errors, samples, confetti, open: () => {} };
  add('P', `page opened ${OPENED.toISOString()} ${location.href}`);
  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', ui); else ui();
})();
