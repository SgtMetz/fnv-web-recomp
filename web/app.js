// First-run page: capability checks, install import, saves, storage.
import { fromDirectoryHandle, fromFileList, fromDevServer, rebase } from './src/sources.js';
import { planImport } from './src/plan.js';
import { loadManifest, prepareStorage, startCopy, finishImport, importStatus } from './src/importer.js';
import { opfsRoot, removeRoot, storageState } from './src/opfs.js';
import { listSaves, importSaves, exportSave, deleteSave, screenshotImage } from './src/saves.js';
import { ROOT } from './src/layout.js';

const $ = (id) => document.getElementById(id);
const params = new URLSearchParams(location.search);
const rootName = params.get('root') || ROOT;
const fmt = (n) => (n >= 2 ** 30 ? `${(n / 2 ** 30).toFixed(2)} GiB` : n >= 2 ** 20 ? `${(n / 2 ** 20).toFixed(1)} MiB` : `${(n / 1024).toFixed(0)} KiB`);
const el = (tag, props = {}, ...kids) => { const e = Object.assign(document.createElement(tag), props); e.append(...kids); return e; };

let manifest, source, plan, copyJob;

async function capabilities() {
  const caps = [];
  const add = (ok, label, note = '') => caps.push({ ok, label, note });
  add(!!navigator.storage?.getDirectory, 'Origin-private file system (OPFS)');
  const worker = await new Promise((res) => {
    try {
      const w = new Worker(new URL('./src/caps-worker.js', import.meta.url), { type: 'module' });
      w.onmessage = (ev) => { w.terminate(); res(ev.data); };
      w.onerror = () => { w.terminate(); res({}); };
    } catch { res({}); }
  });
  add(!!worker.syncAccess, 'Sync access handles (worker file I/O)');
  add(self.crossOriginIsolated === true, 'Cross-origin isolation (SharedArrayBuffer)', self.crossOriginIsolated ? '' : 'serve with COOP/COEP');
  add(typeof Atomics?.waitAsync === 'function', 'Atomics.waitAsync');
  add(!!window.showDirectoryPicker, 'Folder picker', window.showDirectoryPicker ? '' : 'fallback picker will be used');
  let gpu = false, bc = false;
  if (navigator.gpu) {
    try { const a = await navigator.gpu.requestAdapter(); gpu = !!a; bc = !!a?.features.has('texture-compression-bc'); } catch {}
  }
  add(gpu, 'WebGPU');
  add(bc, 'BC texture compression (DXT)');
  add(!!navigator.keyboard?.lock, 'Keyboard Lock');
  add('requestPointerLock' in Element.prototype, 'Pointer Lock');
  $('cap-list').replaceChildren(...caps.map((c) => el('li', { className: c.ok ? 'ok' : 'no', textContent: c.label + (c.note ? ` (${c.note})` : '') })));
  return caps;
}

async function refreshStatus() {
  const meta = await importStatus(rootName);
  const st = await storageState();
  $('import-status').textContent = meta
    ? `Imported ${fmt(meta.bytes)} on ${new Date(meta.importedAt).toLocaleString()} from ${meta.source.name}: ${meta.groups.join(', ')}.`
    : 'No game files imported yet. Choose the folder that contains FalloutNV.exe (or its Data folder).';
  $('storage-detail').textContent = `This site uses ${fmt(st.usage)} of ${fmt(st.quota)} available. ` +
    (st.persisted ? 'Storage is persistent (the browser will not clear it on its own).' : 'Storage is not marked persistent yet; importing asks for it.');
}

function showPlan() {
  $('plan').hidden = false;
  const rows = plan.groups.map((g) => {
    const box = el('input', { type: 'checkbox', checked: g.selected, disabled: g.required || !g.available });
    box.addEventListener('change', () => { choices[g.id] = box.checked; replan(); });
    const state = g.required ? 'required' : !g.available ? (g.files.length ? 'incomplete' : 'not installed') : g.mismatched.length ? 'differs from reference' : 'ok';
    return el('tr', {}, el('td', {}, box), el('td', { textContent: g.label }), el('td', { className: 'num', textContent: g.files.length ? fmt(g.bytes) : '—' }), el('td', { textContent: state }));
  });
  $('group-rows').replaceChildren(...rows);
  const msgs = [
    ...plan.errors.map((t) => el('li', { className: 'error', textContent: t })),
    ...plan.warnings.map((t) => el('li', { className: 'warning', textContent: t })),
    ...plan.info.map((t) => el('li', { className: 'info', textContent: t })),
  ];
  $('messages').replaceChildren(...msgs);
  $('start').disabled = plan.errors.length > 0;
  $('storage').textContent = `Selected: ${fmt(plan.totalBytes)}`;
}

const choices = {};
function replan() {
  plan = planImport(manifest, source.entries, choices);
  showPlan();
}

async function useSource(raw) {
  const rb = rebase(raw.entries);
  if (!rb.root) { $('import-status').textContent = rb.reason; return; }
  source = { ...raw, entries: rb.entries };
  manifest ??= await loadManifest();
  replan();
}

async function runImport() {
  const st = await prepareStorage(rootName, plan);
  $('storage').textContent = `Selected: ${fmt(plan.totalBytes)} · to copy: ${fmt(st.left)} · free: ${fmt(st.free)} · persistent: ${st.persisted ? 'yes' : 'no'}`;
  if (!st.enough) { $('messages').prepend(el('li', { className: 'error', textContent: `Not enough browser storage: ${fmt(st.left)} needed, ${fmt(st.free)} free.` })); return; }
  $('start').disabled = true; $('cancel').hidden = false; $('progress').hidden = false;
  const t0 = performance.now();
  copyJob = startCopy(rootName, plan, {
    progress: (m) => {
      $('bar').value = m.total ? m.bytes / m.total : 1;
      const rate = m.bytes / Math.max(1, (performance.now() - t0) / 1000);
      $('progress-text').textContent = `${fmt(m.bytes)} of ${fmt(m.total)} · file ${m.index + 1}/${m.count} · ${fmt(rate)}/s · ${m.path}`;
    },
    file: (m) => $('log').append(el('li', { textContent: `${m.status} ${m.path}${m.check ? ` (${m.check})` : ''}${m.ms ? ` ${(m.ms / 1000).toFixed(1)} s` : ''}` })),
  });
  try {
    const done = await copyJob.promise;
    const r = await finishImport(rootName, plan, source, { preset: $('preset').value, width: 1280, height: 720 });
    $('progress-text').textContent = `Done: ${done.copied} copied, ${done.skipped} already present, ${fmt(done.bytes)} in ${(done.ms / 1000).toFixed(1)} s. Wrote ${r.seeded.length ? r.seeded.join(', ') : 'no new settings (kept existing)'}.`;
  } catch (e) {
    $('progress-text').textContent = e.name === 'AbortError' ? 'Cancelled. Importing again continues where it stopped.' : `Import failed: ${e.message}`;
  } finally {
    $('start').disabled = false; $('cancel').hidden = true; copyJob = null;
    refreshStatus(); refreshSaves();
  }
}

async function refreshSaves() {
  const root = await opfsRoot(rootName);
  const saves = await listSaves(root);
  if (!saves.length) { $('save-list').replaceChildren(el('li', { className: 'muted', textContent: 'No saves yet.' })); return; }
  $('save-list').replaceChildren(...saves.map((s) => {
    const li = el('li');
    if (s.info) {
      const c = el('canvas', { width: s.info.width, height: s.info.height });
      c.getContext('2d').putImageData(screenshotImage(s.info), 0, 0);
      li.append(c);
    }
    const i = s.info;
    li.append(el('div', { className: 'meta' },
      el('strong', { textContent: i ? `${i.name}, level ${i.level}` : s.file }),
      el('div', { className: 'muted', textContent: i ? `${i.location} · played ${i.playtime.replace(/^0*(\d+)\.(\d+)\.(\d+)$/, '$1h $2m')} · ${i.plugins.length} plugins` : `unreadable: ${s.error}` }),
      el('div', { className: 'muted', textContent: `${s.file} · ${fmt(s.size)} · ${new Date(s.lastModified).toLocaleString()}` })));
    const exp = el('button', { type: 'button', className: 'secondary', textContent: 'Export' });
    exp.onclick = () => exportSave(root, s.file);
    const del = el('button', { type: 'button', className: 'danger', textContent: 'Delete' });
    del.onclick = async () => { if (confirm(`Delete ${s.file} from this browser? Export it first if you want to keep it.`)) { await deleteSave(root, s.file); refreshSaves(); } };
    li.append(el('div', { className: 'actions' }, exp, del));
    return li;
  }));
}

$('pick').onclick = async () => {
  try { await useSource(await fromDirectoryHandle(await window.showDirectoryPicker({ id: 'fnv-install', mode: 'read' }))); }
  catch (e) { if (e.name !== 'AbortError') $('import-status').textContent = `Could not read the folder: ${e.message}`; }
};
$('pick').hidden = !window.showDirectoryPicker;
$('pick-fallback').onchange = (ev) => useSource(fromFileList(ev.target.files));
if (params.has('dev')) { $('pick-dev').hidden = false; $('pick-dev').onclick = async () => useSource(await fromDevServer()); }
$('start').onclick = runImport;
$('cancel').onclick = () => copyJob?.cancel();
$('save-import').onchange = async (ev) => {
  const r = await importSaves(await opfsRoot(rootName), ev.target.files);
  ev.target.value = '';
  alert(r.map((x) => `${x.file}: ${x.status}${x.reason ? ` (${x.reason})` : ''}`).join('\n'));
  refreshSaves();
};
$('wipe').onclick = async () => {
  if (!confirm('Remove every imported game file, setting and save from this browser? Export saves first to keep them.')) return;
  await removeRoot(rootName);
  location.reload();
};

capabilities();
refreshStatus();
refreshSaves();
