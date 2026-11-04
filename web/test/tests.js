// Browser test run for the importer and the file layer. Uses its own OPFS root ("fnv-test") and removes it after,
// so a real import under "fnv" is never touched. Results go to the page and to window.__results.
// ?real=1 also imports a small subset of a real install from the dev server (web/serve.mjs --install ...), copies
// it into "fnv-test-real", checks it, and removes it again.
import { splitPath, foldName, compareNtfs, wildcardMatch, msToFiletime, filetimeToMs } from '../src/winpath.js';
import { planImport } from '../src/plan.js';
import { parseIni, getIni, setIni, serializeIni, latin1 } from '../src/ini.js';
import { buildBsa, readBsaIndex, readBsaFile } from '../src/bsa.js';
import { parseFos, listSaves, importSaves } from '../src/saves.js';
import { fromMemory, fromDevServer, rebase } from '../src/sources.js';
import { loadManifest, startCopy, finishImport, prepareStorage } from '../src/importer.js';
import { opfsRoot, removeRoot, readBytes, readText, readJson, walk } from '../src/opfs.js';
import { createShared } from '../src/vfs-proto.js';
import { MY_GAMES, PLUGINS_TXT } from '../src/layout.js';

const results = { checks: [], perf: {}, notes: [] };
window.__results = results;
const out = document.getElementById('out');
function check(name, ok, detail = '') {
  results.checks.push({ name, ok: !!ok, detail: String(detail) });
  const li = document.createElement('li');
  li.className = ok ? 'ok' : 'fail';
  li.textContent = `${ok ? 'PASS' : 'FAIL'}  ${name}${detail && !ok ? ` — ${detail}` : ''}`;
  out.append(li);
}
const note = (t) => { results.notes.push(t); const li = document.createElement('li'); li.className = 'note'; li.textContent = t; out.append(li); };
const eq = (a, b) => JSON.stringify(a) === JSON.stringify(b);

function rand(n, seed = 1) {
  const b = new Uint8Array(n);
  let x = seed >>> 0 || 1;
  for (let i = 0; i < n; i++) { x ^= x << 13; x ^= x >>> 17; x ^= x << 5; b[i] = x & 255; }
  return b;
}
function esm(n, seed) { const b = rand(n, seed); b.set([0x54, 0x45, 0x53, 0x34]); return b; } // "TES4"

function fosBytes({ name = 'Courier', location = 'Goodsprings', level = 3, plugins = ['FalloutNV.esm'], w = 8, h = 4 } = {}) {
  const parts = [];
  const enc = (s) => latin1.encode(s);
  const u32 = (x) => { const b = new Uint8Array(4); new DataView(b.buffer).setUint32(0, x, true); return b; };
  const u16 = (x) => { const b = new Uint8Array(2); new DataView(b.buffer).setUint16(0, x, true); return b; };
  const bar = new Uint8Array([0x7c]);
  const bstr = (s) => [u16(s.length), bar, enc(s)];
  const lang = new Uint8Array(64); lang.set(enc('ENGLISH'));
  const head = [u32(0x30), bar, lang, bar, u32(w), bar, u32(h), bar, u32(7), bar,
    ...bstr(name), bar, ...bstr('Drifter'), bar, u32(level), bar, ...bstr(location), bar, ...bstr('001.02.03'), bar];
  const headLen = head.reduce((s, x) => s + x.length, 0);
  const plug = [new Uint8Array([plugins.length]), bar, ...plugins.flatMap((p) => [...bstr(p), bar])];
  const plugLen = plug.reduce((s, x) => s + x.length, 0);
  parts.push(enc('FO3SAVEGAME'), u32(headLen), ...head, rand(w * h * 3, 9), new Uint8Array([27]), u32(plugLen), ...plug, rand(1000, 3));
  const total = parts.reduce((s, x) => s + x.length, 0);
  const b = new Uint8Array(total);
  let o = 0; for (const p of parts) { b.set(p, o); o += p.length; }
  return b;
}

function unitTests() {
  check('splitPath handles \\, /, . and ..', eq(splitPath('C:\\Games\\.\\FalloutNV\\Data\\..\\Data//Meshes\\'), ['C:', 'Games', 'FalloutNV', 'Data', 'Meshes']));
  check('foldName: upcase, trailing dots/spaces ignored, ß kept', foldName('Dead Money.esm. ') === 'DEAD MONEY.ESM' && foldName('straße') === 'STRAßE');
  const names = ['TribalPack.esm', 'FalloutNV.esm', 'DeadMoney.esm', 'caravanpack.esm', 'FalloutNV_lang.esp', 'Fallout - Misc.bsa', 'GunRunnersArsenal.esm'];
  check('NTFS order (upcased binary)', eq([...names].sort(compareNtfs),
    ['caravanpack.esm', 'DeadMoney.esm', 'Fallout - Misc.bsa', 'FalloutNV.esm', 'FalloutNV_lang.esp', 'GunRunnersArsenal.esm', 'TribalPack.esm']));
  check('wildcards', wildcardMatch('*.esm', 'DeadMoney.ESM') && !wildcardMatch('*.esm', 'DeadMoney.esp') && wildcardMatch('*.*', 'README')
    && wildcardMatch('*', '.') && wildcardMatch('save?.fos', 'Save1.fos') && !wildcardMatch('save?.fos', 'Save12.fos')
    && wildcardMatch('Fallout - *.bsa', 'fallout - meshes.bsa') && wildcardMatch('a*b*c', 'aXXbYYc') && !wildcardMatch('a*b*c', 'aXXbYY'));
  const ms = Date.UTC(2017, 4, 15, 16, 41, 34);
  check('FILETIME round trip', filetimeToMs(msToFiletime(ms)) === ms && msToFiletime(0) === 116444736000000000n);

  const ini = parseIni('; comment\r\n[Display]\r\niSize W=1024\r\nbFull Screen=1\r\n\r\n[General]\r\nsLanguage=ENGLISH\r\n');
  setIni(ini, 'display', 'ISIZE W', '1280'); setIni(ini, 'Display', 'iSize H', '720'); setIni(ini, 'Audio', 'fVol', '1.0');
  const s = serializeIni(ini);
  check('INI: set keeps comments/order, case-insensitive', s === '; comment\r\n[Display]\r\niSize W=1280\r\nbFull Screen=1\r\niSize H=720\r\n\r\n[General]\r\nsLanguage=ENGLISH\r\n\r\n[Audio]\r\nfVol=1.0\r\n', JSON.stringify(s));
  check('INI: get', getIni(parseIni(s), 'GENERAL', 'slanguage') === 'ENGLISH' && getIni(parseIni(s), 'Display', 'missing') === undefined);

  const bsa = buildBsa({ 'meshes\\a.nif': rand(100, 1), 'meshes\\b.nif': rand(50, 2), 'textures\\c.dds': rand(300, 3) });
  return readBsaIndex((o, n) => bsa.subarray(o, o + n), bsa.length).then(async (ix) => {
    const f = ix.files.find((x) => x.path === 'textures\\c.dds');
    const data = await readBsaFile((o, n) => bsa.subarray(o, o + n), ix, f);
    check('BSA writer/reader round trip', ix.fileCount === 3 && ix.folderCount === 2 && eq([...data], [...rand(300, 3)]));
    let truncated = false;
    try { await readBsaIndex((o, n) => bsa.subarray(o, o + n), 40); } catch { truncated = true; }
    check('BSA reader rejects a truncated archive', truncated);

    const fos = parseFos(fosBytes({ plugins: ['FalloutNV.esm', 'DeadMoney.esm'] }));
    check('.fos header', fos.name === 'Courier' && fos.level === 3 && fos.location === 'Goodsprings' && eq(fos.plugins, ['FalloutNV.esm', 'DeadMoney.esm']) && fos.screenshot.length === 96);
    let bad = false;
    try { parseFos(new Uint8Array(100)); } catch { bad = true; }
    check('.fos: rejects non-saves', bad);
  });
}

// A small install with the real names, odd case, and the reference install's timestamp pattern.
function syntheticInstall() {
  const T = (h, m, s) => Date.UTC(2017, 4, 15, h, m, s);
  const bsa = (seed) => buildBsa({ 'meshes\\x.nif': rand(2000, seed), 'textures\\y.dds': rand(3000, seed + 1) });
  const files = {
    'FalloutNV.exe': rand(1000, 99), 'Fallout_default.ini': latin1.encode('[General]\r\nsLanguage=ENGLISH\r\n[Display]\r\niSize W=1024\r\n'),
    'high.ini': latin1.encode('[Display]\r\niMultiSample=4\r\niShadowMapResolution=1024\r\n'),
    'Data/FalloutNV.esm': esm(5000, 1), 'Data/Fallout - Meshes.bsa': bsa(10), 'Data/Fallout - Textures.bsa': bsa(20),
    'Data/Fallout - Textures2.bsa': bsa(30), 'Data/Fallout - Sound.bsa': bsa(40), 'Data/Fallout - Misc.bsa': bsa(50),
    'Data/Update.bsa': bsa(60), 'Data/Fallout - Voices1.bsa': buildBsa({ 'sound\\voice\\v.ogg': rand(24 << 20, 70) }),
    'Data/Shaders/shaderpackage002.sdp': rand(4000, 80), 'Data/Shaders/shaderpackage013.sdp': rand(4000, 81),
    'Data/DeadMoney.esm': esm(3000, 2), 'Data/DEADMONEY.NAM': latin1.encode('Dead Money'),
    'Data/DeadMoney - Main.bsa': bsa(90), 'Data/DeadMoney - Sounds.bsa': bsa(95),
    'Data/GunRunnersArsenal.esm': esm(1000, 3), // incomplete DLC: no NAM, no BSAs
    'Data/Music/Base/MUS_Base.mp3': rand(5000, 100), 'Data/Sound/songs/radionv/song.mp3': rand(5000, 101),
    'Data/mymod.esp': esm(100, 4),
  };
  const times = {
    'Data/FalloutNV.esm': T(16, 41, 36), 'Data/DeadMoney.esm': T(16, 41, 34), 'Data/GunRunnersArsenal.esm': T(16, 41, 46),
  };
  // the player picked the folder above the install: paths start with "Fallout New Vegas/"
  const prefixed = Object.fromEntries(Object.entries(files).map(([k, v]) => [`Fallout New Vegas/${k}`, v]));
  const prefTimes = Object.fromEntries(Object.entries(times).map(([k, v]) => [`Fallout New Vegas/${k}`, v]));
  return { src: fromMemory(prefixed, prefTimes), files };
}

async function importTests(manifest) {
  const ROOT = 'fnv-test';
  await removeRoot(ROOT);
  const { src, files } = syntheticInstall();
  const rb = rebase(src.entries);
  check('rebase finds the install below the picked folder', rb.root === 'Fallout New Vegas' && rb.entries.some((e) => e.path === 'Data/FalloutNV.esm'), rb.root);
  const plan = planImport(manifest, rb.entries, { video: true });
  check('plan: no errors with every required file', plan.errors.length === 0, plan.errors.join('; '));
  const g = Object.fromEntries(plan.groups.map((x) => [x.id, x]));
  check('plan: DLC with all files is offered', g.deadmoney.available && g.deadmoney.selected);
  check('plan: incomplete DLC is not offered, with a warning', !g.gunrunners.available && plan.warnings.some((w) => /Gun Runners/.test(w)));
  check('plan: size differences are warnings', plan.warnings.some((w) => /reference install/.test(w)));
  check('plan: executables and mods are not copied', !plan.copy.some((e) => /\.exe$|mymod/i.test(e.path)) && plan.unclaimed === 2, plan.unclaimed);
  check('plan: missing optional video is just not installed', !g.video.available && plan.info.some((t) => /Intro video/.test(t)));
  const lite = planImport(manifest, rb.entries, { voices: false, deadmoney: false });
  check('lite plan drops voices and DLC', !lite.copy.some((e) => /Voices1|DeadMoney/i.test(e.path)) && lite.totalBytes < plan.totalBytes - (24 << 20));
  const missing = planImport(manifest, rb.entries.filter((e) => !/Update\.bsa$/.test(e.path)));
  check('plan: a missing required file is an error', missing.errors.some((e) => /Update\.bsa/.test(e)));

  const st = await prepareStorage(ROOT, plan);
  note(`storage: quota ${(st.quota / 2 ** 30).toFixed(1)} GiB, used ${(st.usage / 2 ** 20).toFixed(0)} MiB, persist() granted: ${st.persisted}`);
  check('storage estimate is available', st.quota > 0);

  // cancel part-way through the 24 MiB voices archive, then resume
  const order = [...plan.copy].sort((a, b) => (/Voices1/.test(a.path) ? -1 : /Voices1/.test(b.path) ? 1 : 0));
  const first = { ...plan, copy: order };
  const job = startCopy(ROOT, first, {}, { chunk: 1 << 20, stopAfter: 6 << 20 });
  let abortName = '';
  try { await job.promise; } catch (e) { abortName = e.name; }
  check('cancel stops the copy', abortName === 'AbortError', abortName);
  const statuses = {};
  const t0 = performance.now();
  const done = await startCopy(ROOT, first, { file: (m) => { statuses[m.path] = m.status; } }, { chunk: 1 << 20 }).promise;
  results.perf.syntheticCopyMs = performance.now() - t0;
  check('resume continues the partial file', statuses['Data/Fallout - Voices1.bsa'] === 'resumed', statuses['Data/Fallout - Voices1.bsa']);
  check('every planned file copied', done.copied === plan.copy.length, `${done.copied}/${plan.copy.length}`);
  const again = await startCopy(ROOT, plan, {}).promise;
  check('a second import skips finished files', again.skipped === plan.copy.length && again.copied === 0, JSON.stringify(again));

  const root = await opfsRoot(ROOT);
  let same = true;
  for (const e of plan.copy) {
    const got = await readBytes(root, 'game/' + e.path);
    const want = files[e.path];
    if (got.length !== want.length || got.some((b, i) => b !== want[i])) { same = false; check(`bytes of ${e.path}`, false); }
  }
  check('copied bytes equal the source', same);

  const r = await finishImport(ROOT, plan, src, { preset: 'high', width: 1600, height: 900 });
  const meta = await readJson(root, 'meta/import.json', null);
  check('import.json keeps the install\'s file times', meta.files['Data/DeadMoney.esm'].lastModified === Date.UTC(2017, 4, 15, 16, 41, 34));
  const ini = await readBytes(root, `${MY_GAMES}/Fallout.ini`);
  check('Fallout.ini is a byte copy of Fallout_default.ini', eq([...ini], [...files['Fallout_default.ini']]));
  const prefs = parseIni(await readText(root, `${MY_GAMES}/FalloutPrefs.ini`));
  check('FalloutPrefs.ini = preset + browser display settings',
    getIni(prefs, 'Display', 'iShadowMapResolution') === '1024' && getIni(prefs, 'Display', 'bFull Screen') === '0'
    && getIni(prefs, 'Display', 'iSize W') === '1600' && getIni(prefs, 'Display', 'iSize H') === '900');
  check('plugins.txt is created empty', (await readText(root, PLUGINS_TXT)) === '');
  const r2 = await finishImport(ROOT, plan, src, { preset: 'low' });
  check('re-import keeps existing settings', r2.seeded.length === 0 && r.seeded.length === 3, JSON.stringify(r2.seeded));

  // saves: import (with a non-save and a duplicate), list
  const sv = await importSaves(root, [
    new File([fosBytes({ name: 'Six', level: 12 })], 'Save 3   Six  Primm  01 02 03.fos'),
    new File([new Uint8Array(10)], 'broken.fos'), new File(['x'], 'notes.txt'),
  ]);
  const sv2 = await importSaves(root, [new File([fosBytes()], 'SAVE 3   six  primm  01 02 03.FOS')]);
  check('save import validates and refuses duplicates', eq(sv.map((x) => x.status), ['imported', 'rejected', 'skipped']) && sv2[0].status === 'exists');
  const list = await listSaves(root);
  check('save list reads headers', list.length === 1 && list[0].info.name === 'Six' && list[0].info.level === 12);
  return { root, plan, files, ROOT };
}

// Ask the I/O worker to close its handles (else OPFS keeps the files locked), then stop it.
async function stopServer(server) {
  await new Promise((res) => { server.onmessage = (ev) => ev.data.type === 'closed' && res(); server.postMessage({ type: 'shutdown' }); });
  server.terminate();
}

async function vfsTests({ plan, files, ROOT }, label = 'vfs', esmOrder = ['DeadMoney.esm', 'FalloutNV.esm']) {
  const shared = createShared({ slots: 8 });
  const memory = new WebAssembly.Memory({ initial: 128, maximum: 256, shared: true }); // 8 MiB, like a wasm heap
  const server = new Worker(new URL('../src/vfs-server.js', import.meta.url), { type: 'module' });
  const ready = await new Promise((res, rej) => {
    server.onmessage = (ev) => (ev.data.type === 'ready' ? res(ev.data) : rej(new Error(ev.data.message)));
    server.onerror = (e) => rej(new Error(e.message));
    server.postMessage({ type: 'init', root: ROOT, shared, memory });
  });
  note(`${label}: I/O worker indexed ${ready.files} files / ${ready.dirs} folders in ${ready.ms.toFixed(0)} ms, ${ready.timed} with recorded times`);
  const expect = {};
  for (const e of plan.copy) {
    const bytes = files[e.path];
    expect[e.path] = { size: e.size, lastModified: e.lastModified, head: [...bytes.subarray(0, 64)] };
  }
  const big = plan.copy.reduce((a, b) => (b.size > a.size ? b : a));
  const run = (name, perf, base = 0) => new Promise((res) => {
    const w = new Worker(new URL('./engine-worker.js', import.meta.url), { type: 'module' });
    w.onmessage = (ev) => { w.terminate(); res(ev.data); };
    w.onerror = (e) => { w.terminate(); res({ checks: [{ name: `${name}: worker`, ok: false, detail: e.message }], perf: {} }); };
    w.postMessage({ shared, memory, expect, esmOrder, perf, name, base, bigFile: { path: big.path.replace(/\//g, '\\'), size: big.size } });
  });
  const solo = await run('t1', true);
  for (const c of solo.checks) check(`${label} ${c.name}`, c.ok, c.detail);
  results.perf[label] = solo.perf;
  // two threads at once: both must pass (slot sharing, interleaved requests)
  const pair = await Promise.all([run('t2', false, 3 << 20), run('t3', false, 5 << 20)]);
  check(`${label}: two engine threads at once`, pair.every((p) => p.checks.every((c) => c.ok)),
    pair.flatMap((p) => p.checks.filter((c) => !c.ok).map((c) => `${c.name} ${c.detail}`)).join(', '));
  await stopServer(server);
}

async function realSubsetTests(manifest) {
  // A small slice of the player's real install (no voices/textures/meshes): masters, NAMs, the two smallest BSAs,
  // INIs. Copied into fnv-test-real and removed afterwards. Only runs with ?real=1 and the dev server.
  const ROOT = 'fnv-test-real';
  await removeRoot(ROOT);
  let src;
  try { src = await fromDevServer(); } catch (e) { note(`real subset skipped: ${e.message}`); return; }
  const rb = rebase(src.entries);
  const full = planImport(manifest, rb.entries);
  check('real install: plan has no errors', full.errors.length === 0, full.errors.join('; '));
  check('real install: no size warnings against the manifest', full.warnings.length === 0, full.warnings.join('; '));
  note(`real install: full import ${(full.totalBytes / 2 ** 30).toFixed(2)} GiB, ${full.copy.length} files; minimum ${(planImport(manifest, rb.entries, Object.fromEntries(manifest.groups.map((g) => [g.id, false]))).totalBytes / 2 ** 30).toFixed(2)} GiB`);
  const keep = /(^|\/)(Fallout - Misc\.bsa|Update\.bsa|[^/]+\.(esm|esp|nam)|[^/]+\.ini)$/i;
  const subset = { ...full, copy: full.copy.filter((e) => keep.test(e.path) && !/FalloutNV\.esm$/i.test(e.path)) };
  subset.totalBytes = subset.copy.reduce((s, e) => s + e.size, 0);
  const t0 = performance.now();
  const logs = [];
  const done = await startCopy(ROOT, subset, { file: (m) => logs.push(`${m.path}: ${m.check}`) }).promise;
  const ms = performance.now() - t0;
  results.perf.realSubset = { files: done.copied, MiB: subset.totalBytes / 2 ** 20, ms, MBps: subset.totalBytes / 2 ** 20 / (ms / 1000) };
  check(`real subset copied and verified (${done.copied} files, ${(subset.totalBytes / 2 ** 20).toFixed(1)} MiB)`, done.copied === subset.copy.length);
  note(`real subset: ${logs.filter((l) => /files$/.test(l)).join('; ')}`);
  await finishImport(ROOT, subset, src, { preset: 'VeryHigh' });
  // FindFirstFile over the real masters: NTFS order and the install's own times
  const shared = createShared({ slots: 4 });
  const memory = new SharedArrayBuffer(4 << 20);
  const server = new Worker(new URL('../src/vfs-server.js', import.meta.url), { type: 'module' });
  await new Promise((res, rej) => { server.onmessage = (ev) => (ev.data.type === 'ready' ? res() : rej(new Error(ev.data.message))); server.postMessage({ type: 'init', root: ROOT, shared, memory }); });
  const listing = await new Promise((res) => {
    const w = new Worker(URL.createObjectURL(new Blob([
      `import { VfsClient } from '${new URL('../src/vfs-client.js', import.meta.url).href}';
       self.onmessage = (ev) => { const v = new VfsClient(ev.data.shared, ev.data.memory); self.postMessage(v.list('Data\\\\*.esm')); };`],
      { type: 'text/javascript' })), { type: 'module' });
    w.onmessage = (ev) => { w.terminate(); res(ev.data); };
    w.postMessage({ shared, memory });
  });
  await stopServer(server);
  const want = rb.entries.filter((e) => /^Data\/[^/]+\.esm$/i.test(e.path) && !/FalloutNV\.esm$/i.test(e.path));
  const ok = listing.length === want.length && listing.every((l) => want.find((w) => w.path === 'Data/' + l.name)?.lastModified === l.mtime);
  check('real masters: listed with the install\'s times', ok, JSON.stringify(listing.map((l) => [l.name, new Date(l.mtime).toISOString()])));
  results.realListing = listing.map((l) => [l.name, new Date(l.mtime).toISOString()]);
  await removeRoot(ROOT);
}

async function main() {
  const params = new URLSearchParams(location.search);
  check('page is cross-origin isolated', self.crossOriginIsolated === true);
  await unitTests();
  const manifest = await loadManifest();
  check('manifest loads', manifest.groups.length === 16);
  const ctx = await importTests(manifest);
  await vfsTests(ctx);
  if (params.has('real')) await realSubsetTests(manifest);
  if (!params.has('keep')) await removeRoot('fnv-test');
  const failed = results.checks.filter((c) => !c.ok);
  results.done = true;
  results.summary = `${results.checks.length - failed.length}/${results.checks.length} passed`;
  document.getElementById('summary').textContent = results.summary + (failed.length ? ` — ${failed.length} failed` : '');
  document.getElementById('perf').textContent = JSON.stringify(results.perf, null, 1);
}

main().catch((e) => {
  check('test run', false, `${e.message}\n${e.stack}`);
  results.done = true;
  results.summary = 'crashed';
  document.getElementById('summary').textContent = `crashed: ${e.message}`;
});
