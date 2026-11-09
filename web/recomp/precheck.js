// Device precheck (web/recomp/index.html): before the game loads, check that this browser and device can run it, say
// why in a popup when they cannot (or may struggle), and estimate how long loading will take. A classic script loaded
// in <head>, so it runs whether or not the game's module gets far.
//
// Blockers (the game cannot start): no WebAssembly, no shared wasm memory (wasm threads), the page not cross-origin
// isolated (no SharedArrayBuffer), no WebGPU or no WebGPU adapter, a 32-bit browser (the game needs 2-3 GB of wasm
// memory).
// Warnings (it may start, but badly): little device memory, a software (fallback) GPU, no BC texture compression
// (textures are decoded on the CPU), few CPU cores, a slow CPU, a slow or metered connection, no AudioWorklet (no
// sound), a browser other than Chrome or Edge.
//
// The load estimate: the wasm download and compile, then the game's own start-up (the kilobytes it fetched last time
// on this browser, or the defaults in index.html), timed with a short CPU benchmark, a measured download speed (a few
// seconds of the wasm, not cached) and the round-trip time. When this browser has started the game before, the time it
// took then is used instead. index.html shows the estimate on the start panel (fnvPrecheck.estimate).
//
// window.fnvPrecheck = { ready: Promise<result>, result, estimate(quality, mode) -> {sec, low, high, text, last} }.
// ?precheck in the address always shows the popup.
(() => {
  'use strict';
  const query = new URLSearchParams(location.search);
  const FORCE = query.has('precheck');

  // kilobytes fetched until ready, the same defaults as index.html (DEFAULT_KB)
  const DEFAULT_KB = { low: { normal: 91000, quick: 358000 }, high: { normal: 141000, quick: 1073000 } };
  const WASM_MB = 13.6;               // brotli'd wasm, when VERSION.json does not say
  // calibration: a desktop with a Ryzen-class CPU (bench 1.0) took 27 s of CPU for the 360 MB a quick start reads,
  // and a few seconds to compile the wasm
  const CPU_SEC_PER_MB = 0.07, COMPILE_SEC = 6, REF_BENCH_MS = 21;

  const result = { blockers: [], warnings: [], info: {}, done: false };
  const block = (title, why) => result.blockers.push({ title, why });
  const warn = (title, why) => result.warnings.push({ title, why });
  const timeout = (p, ms, v = null) => Promise.race([Promise.resolve(p).catch(() => v), new Promise((r) => setTimeout(() => r(v), ms))]);

  // a fixed amount of integer and float work; the best of a few runs, in ms (smaller is faster)
  function bench() {
    let best = Infinity, sink = 0;
    for (let run = 0; run < 4; run++) {
      const t = performance.now();
      let a = 1, f = 1.0001;
      for (let i = 0; i < 4e6; i++) { a = Math.imul(a ^ i, 0x9e3779b1) + (a >>> 7); f = f * 1.0000001 + (a & 7) * 1e-9; }
      sink += a + f;
      best = Math.min(best, performance.now() - t);
    }
    window.__fnvBenchSink = sink;    // keeps the loop from being optimised away
    return best;
  }

  // download speed in MB/s of compressed bytes: up to 2.5 s or 8 MB of the wasm, uncached, from its first byte
  async function measureDownload(version) {
    const ctl = new AbortController();
    try {
      const t0 = performance.now();
      const res = await timeout(fetch('/build/fnv_web.wasm', { cache: 'no-store', signal: ctl.signal }), 6000);
      if (!res || !res.ok || !res.body) return null;
      const enc = res.headers.get('content-encoding');
      const scale = enc && enc !== 'identity' && version && version.wasm_bytes
        ? ((enc === 'br' ? version.wasm_br_bytes : enc === 'gzip' ? version.wasm_gz_bytes : 0) || version.wasm_bytes) / version.wasm_bytes : 1;
      const reader = res.body.getReader();
      const tFirst = performance.now();
      let got = 0;
      for (;;) {
        const { done, value } = await reader.read();
        if (done) break;
        got += value.byteLength;
        if (performance.now() - tFirst > 2500 || got * scale > 8 * 1048576) break;
      }
      ctl.abort();
      const sec = (performance.now() - tFirst) / 1000;
      result.info.ttfbMs = Math.round(tFirst - t0);
      // a fast connection can finish the 8 MB in a few milliseconds: the timer's resolution sets a floor
      return got * scale > 1048576 || sec > 0.5 ? Math.min(200, (got * scale / 1048576) / Math.max(sec, 0.02)) : null;
    } catch { ctl.abort(); return null; }
  }

  async function measureRtt() {
    const times = [];
    for (let i = 0; i < 3; i++) {
      const t = performance.now();
      const r = await timeout(fetch('favicon.ico', { method: 'HEAD', cache: 'no-store' }), 3000);
      if (r) times.push(performance.now() - t);
    }
    times.sort((a, b) => a - b);
    return times.length ? times[Math.floor(times.length / 2)] : null;
  }

  async function run() {
    const n = navigator, info = result.info;

    // ---- the browser --------------------------------------------------------------------------------------------------
    const ua = n.userAgent;
    let uaData = null;
    if (n.userAgentData) uaData = await timeout(n.userAgentData.getHighEntropyValues(['bitness', 'architecture', 'platform', 'mobile']), 1500);
    info.mobile = uaData ? !!uaData.mobile : /Android|iPhone|iPad|iPod|Mobile/i.test(ua);
    const chromium = !!(n.userAgentData && n.userAgentData.brands && n.userAgentData.brands.some((b) => /Chrom|Edge/i.test(b.brand)));

    if (typeof WebAssembly !== 'object') {
      block('WebAssembly is turned off', 'The game is compiled to WebAssembly, and this browser has it disabled or does not support it. Use a recent Chrome or Edge.');
    } else {
      try { new WebAssembly.Memory({ initial: 1, maximum: 1, shared: true }); }
      catch { block('No shared WebAssembly memory', 'The game runs on many threads at once, which needs shared WebAssembly memory. This browser does not offer it. Use a recent Chrome or Edge.'); }
    }
    if (!window.crossOriginIsolated || typeof SharedArrayBuffer === 'undefined') {
      block('The page is not isolated', window.parent !== window
        ? 'The game is shown inside another page, which keeps it from using the browser features it needs. Open the game on its own page.'
        : 'The browser did not turn on the security isolation the game needs for its threads (SharedArrayBuffer). Reload the page; if this keeps happening, an extension or a privacy setting may be blocking it.');
    }
    if (uaData && uaData.bitness === '32') {
      block('32-bit browser', 'The game needs about 3 GB of memory for itself, more than a 32-bit browser can give one page. Install the 64-bit version of your browser.');
    }
    if (typeof AudioWorkletNode === 'undefined') warn('No sound', 'This browser has no AudioWorklet, so the game will run without sound.');
    if (!chromium) warn('Untested browser', 'The game is tested in Chrome and Edge. Other browsers may fail to start it or run it slowly.');

    // ---- the GPU ------------------------------------------------------------------------------------------------------
    info.bc = true;
    if (!window.isSecureContext) {
      block('Not a secure page', 'WebGPU only works on https:// pages. Open the game at its https:// address.');
    } else if (!n.gpu) {
      block('No WebGPU', 'The game draws with WebGPU, and this browser does not have it. Use a recent Chrome or Edge on a computer, or Chrome on Android. On Linux, WebGPU may need to be turned on in chrome://flags.');
    } else {
      const adapter = await timeout(n.gpu.requestAdapter({ powerPreference: 'high-performance' }), 5000);
      if (!adapter) {
        block('No graphics adapter', 'This browser has WebGPU, but it would not give the game a graphics card. Your graphics driver may be too old or blocked by the browser: update it, and check that hardware acceleration is on in the browser\'s settings.');
      } else {
        const ai = adapter.info || {};
        info.gpu = [ai.vendor, ai.architecture, ai.description].filter(Boolean).join(' ') || 'unknown';
        info.bc = adapter.features.has('texture-compression-bc');
        if (adapter.isFallbackAdapter || ai.isFallbackAdapter) {
          warn('Software graphics', 'The browser is drawing with the CPU instead of a graphics card, so the game will be very slow. Turn on hardware acceleration in the browser\'s settings, or update your graphics driver.');
        }
        if (!info.bc) warn('Slower textures', 'This graphics chip cannot read the game\'s compressed textures directly, so they are unpacked on the CPU. The game works, but loading takes longer. Low quality helps.');
      }
    }

    // ---- the device ---------------------------------------------------------------------------------------------------
    info.cores = n.hardwareConcurrency || 0;
    info.memoryGB = n.deviceMemory || 0;          // Chrome rounds this, and reports at most 8
    if (info.memoryGB && info.memoryGB < 4) {
      warn('Little memory', `This device reports about ${info.memoryGB} GB of memory. The game needs about 3 GB for itself, so the browser may close the tab while it loads. Choose Low quality and close other tabs and apps.`);
    } else if (info.mobile && info.memoryGB && info.memoryGB < 8) {
      warn('Phone memory', 'Phones and tablets often close a tab that uses as much memory as this game. Low quality uses much less; close other apps first.');
    }
    if (info.cores && info.cores < 4) warn('Few CPU cores', `This device has ${info.cores} CPU threads. The game uses several at once, so it will load and run slowly.`);
    info.benchMs = +bench().toFixed(1);
    info.cpuFactor = Math.max(0.5, info.benchMs / REF_BENCH_MS) * (info.cores && info.cores < 4 ? 1.5 : 1);
    if (info.cpuFactor > 3) warn('Slow CPU', 'This device\'s processor tested several times slower than a typical desktop\'s. Expect a long load and a low frame rate; Low quality helps.');

    // ---- the connection -----------------------------------------------------------------------------------------------
    const c = n.connection;
    if (c && c.saveData) warn('Data saver is on', 'The game downloads hundreds of megabytes (Low quality) to over a gigabyte (High). Use Wi-Fi if your data is limited.');
    const version = await timeout(fetch('VERSION.json', { cache: 'no-store' }).then((r) => (r.ok ? r.json() : null)), 3000);
    info.wasmMB = version && version.wasm_br_bytes ? version.wasm_br_bytes / 1048576 : WASM_MB;
    // measuring competes with the game's own download, so only while the start panel is still waiting
    const measured = query.get('start') ? null : await measureDownload(version);
    info.rttMs = Math.round((await measureRtt()) || (c && c.rtt) || 100);
    info.mbps = measured ? +measured.toFixed(2) : c && c.downlink ? +(c.downlink / 8).toFixed(2) : 2;
    info.bandwidthMeasured = !!measured;
    if (info.mbps < 0.6) warn('Slow connection', `The download measured about ${(info.mbps * 8).toFixed(1)} Mbit/s. The game fetches a lot of data while it loads, so this will take a long time; Low quality downloads about a third as much.`);
    if (info.rttMs > 400) warn('High latency', `Each request to the server takes about ${info.rttMs} ms, and the game makes over a thousand of them while it loads.`);
  }

  // seconds until the game is ready, with a range, for a quality (low/high) and a start mode (normal/quick)
  function estimate(quality = 'high', mode = 'normal') {
    const info = result.info;
    const lastKey = `fnv.boot.sec.${mode}.${quality}`;
    const last = +localStorage.getItem(lastKey) || 0;
    if (!result.done && !last) return null;
    let sec;
    if (last) sec = last;
    else {
      const kb = +localStorage.getItem('fnv.boot.kb.' + mode + (quality === 'low' ? '.mobile' : '')) || DEFAULT_KB[quality][mode];
      const dataMB = kb / 1024, fetches = dataMB * 4;
      const cpu = info.cpuFactor || 1;
      sec = info.wasmMB / info.mbps                                    // the wasm
        + COMPILE_SEC * cpu                                            // compiling it
        + dataMB / info.mbps                                           // the game's data
        + fetches * info.rttMs / 1000 / 6                              // round trips (several in flight)
        + dataMB * CPU_SEC_PER_MB * cpu * (info.bc ? 1 : 1.6);        // the game's own loading work
    }
    const low = sec * (last ? 0.8 : 0.7), high = sec * (last ? 1.25 : 1.5);
    const fmt = (s) => s < 90 ? `${Math.max(5, Math.round(s / 5) * 5)} s` : `${Math.round(s / 60)} min`;
    const a = fmt(low), b = fmt(high), unit = (x) => x.split(' ')[1];
    const text = a === b ? `about ${a}` : unit(a) === unit(b) ? `about ${a.split(' ')[0]}–${b}` : `about ${a} to ${b}`;
    return { sec: Math.round(sec), low: Math.round(low), high: Math.round(high), text, last: !!last };
  }

  // ---- the popup --------------------------------------------------------------------------------------------------------
  const CSS = `
  #fnvPrecheck { max-width: 560px; width: calc(100% - 32px); padding: 18px 22px; color: var(--fg, #d8d2c4); background: #14130f;
    border: 1px solid #3a3528; border-radius: 6px; font: 13px/1.45 system-ui, sans-serif; }
  #fnvPrecheck::backdrop { background: rgba(0, 0, 0, .75); }
  #fnvPrecheck h2 { margin: 0 0 6px; font-size: 16px; color: var(--accent, #e0a040); font-weight: 600; }
  #fnvPrecheck.blocked h2 { color: #e77; }
  #fnvPrecheck > p { margin: 0 0 10px; color: var(--dim, #8a8478); }
  #fnvPrecheck ul { margin: 8px 0 0; padding: 0; list-style: none; }
  #fnvPrecheck li { margin: 0 0 10px; padding: 8px 10px; border-left: 3px solid #e0a040; background: #1b1913; }
  #fnvPrecheck li.blocker { border-left-color: #d55; }
  #fnvPrecheck li b { display: block; color: var(--fg, #d8d2c4); }
  #fnvPrecheck li span { color: var(--dim, #8a8478); }
  #fnvPrecheck .eta { margin: 12px 0 0; color: var(--fg, #d8d2c4); }
  #fnvPrecheck .eta small { display: block; color: var(--dim, #8a8478); }
  #fnvPrecheck .row { display: flex; gap: 10px; justify-content: flex-end; margin-top: 14px; flex-wrap: wrap; }
  #fnvPrecheck button { font: inherit; color: inherit; background: #1e1c16; border: 1px solid #3a3528; border-radius: 4px; padding: 6px 14px; cursor: pointer; }
  #fnvPrecheck button.primary { border-color: var(--accent, #e0a040); }
  #fnvPrecheck button:hover { border-color: var(--accent, #e0a040); }`;

  const esc = (s) => String(s).replace(/[&<>"]/g, (ch) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' })[ch]);

  function etaHtml() {
    const qual = localStorage.getItem('fnv.quality') || (result.info.mobile ? 'low' : 'high');
    const e = estimate(qual, 'normal');
    if (!e) return '';
    const i = result.info;
    const how = e.last ? 'how long it took last time on this browser'
      : `your connection (${i.bandwidthMeasured ? 'measured ' : ''}about ${(i.mbps * 8).toFixed(1)} Mbit/s, ${i.rttMs} ms) and a quick test of your processor`;
    return `<p class="eta">Estimated load time to the main menu at ${qual === 'low' ? 'Low' : 'High'} quality: <b>${esc(e.text)}</b>
      <small>From ${esc(how)}.</small></p>`;
  }

  function popup() {
    const blocked = result.blockers.length > 0;
    if (!blocked && !result.warnings.length && !FORCE) return;
    // a warning the player already accepted on this browser is not shown again (a blocker always is)
    const sig = result.warnings.map((w) => w.title).join('|');
    if (!blocked && !FORCE && localStorage.getItem('fnv.precheck.ok') === sig) return;
    const style = document.createElement('style');
    style.textContent = CSS;
    document.head.appendChild(style);
    const dlg = document.createElement('dialog');
    dlg.id = 'fnvPrecheck';
    if (blocked) dlg.classList.add('blocked');
    const items = [...result.blockers.map((b) => ({ ...b, cls: 'blocker' })), ...result.warnings.map((w) => ({ ...w, cls: 'warning' }))];
    dlg.innerHTML = `
      <h2>${blocked ? 'This device can\'t run the game' : items.length ? 'This device may have trouble running the game' : 'This device can run the game'}</h2>
      <p>${blocked ? 'We checked your browser and device before loading, and found:' : items.length ? 'It should start, but we found:' : 'Everything the game needs is here.'}</p>
      <ul>${items.map((it) => `<li class="${it.cls}"><b>${esc(it.title)}</b><span>${esc(it.why)}</span></li>`).join('')}</ul>
      ${blocked ? '' : etaHtml()}
      <div class="row">
        ${blocked ? '<button type="button" data-a="report">Report a problem</button><button type="button" data-a="close">Try anyway</button>'
          : '<button type="button" class="primary" data-a="ok">Continue</button>'}
      </div>`;
    document.body.appendChild(dlg);
    dlg.addEventListener('keydown', (e) => e.stopPropagation());
    dlg.addEventListener('click', (e) => {
      const a = e.target.closest('button') && e.target.closest('button').dataset.a;
      if (!a) return;
      if (a === 'ok' && !FORCE) localStorage.setItem('fnv.precheck.ok', sig);
      dlg.close();
      if (a === 'report' && window.fnvReport) window.fnvReport.open();
    });
    if (document.pointerLockElement) document.exitPointerLock();
    dlg.showModal();
    dlg.querySelector('button').focus();
  }

  const api = window.fnvPrecheck = { result, estimate, ready: null };
  api.ready = (async () => {
    try { await run(); } catch (e) { result.error = String(e && e.stack || e); }
    result.done = true;
    window.fnvReport && window.fnvReport.add('P', 'precheck: ' + JSON.stringify({ blockers: result.blockers.map((b) => b.title),
      warnings: result.warnings.map((w) => w.title), ...result.info }));
    window.dispatchEvent(new CustomEvent('fnv-precheck', { detail: result }));
    if (document.readyState === 'loading') await new Promise((r) => document.addEventListener('DOMContentLoaded', r));
    popup();
    return result;
  })();
})();
