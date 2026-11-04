// Import orchestration on the page: storage checks, the copy worker, then meta/import.json and first-run config.
import { opfsRoot, readJson, writeJson, storageState } from './opfs.js';
import { readerFor } from './sources.js';
import { seedConfig } from './config.js';
import { IMPORT_META } from './layout.js';

export async function loadManifest() {
  const r = await fetch(new URL('./manifest.json', import.meta.url));
  if (!r.ok) throw new Error(`manifest: HTTP ${r.status}`);
  return r.json();
}

// Bytes still to copy, from the journal of an earlier, interrupted import.
export async function remainingBytes(rootName, plan) {
  const root = await opfsRoot(rootName);
  const journal = await readJson(root, 'meta/import-journal.json', {});
  let left = 0;
  for (const e of plan.copy) {
    const j = journal[e.path];
    if (!(j && j.done && j.size === e.size && j.lastModified === e.lastModified)) left += e.size;
  }
  return left;
}

export async function prepareStorage(rootName, plan) {
  const persisted = navigator.storage.persist ? await navigator.storage.persist() : false;
  const st = await storageState();
  const left = await remainingBytes(rootName, plan);
  const free = st.quota - st.usage;
  return { ...st, persisted, left, free, enough: free >= left };
}

// Runs the copy worker. Resolves with the worker's 'done' message; rejects on error or cancel.
// handlers: { progress(m), file(m) }; returns { promise, cancel() }.
export function startCopy(rootName, plan, handlers = {}, { chunk, stopAfter } = {}) {
  const worker = new Worker(new URL('./import-worker.js', import.meta.url), { type: 'module' });
  const items = plan.copy.map((e) => ({ path: e.path, size: e.size, lastModified: e.lastModified, reader: readerFor(e) }));
  const promise = new Promise((resolve, reject) => {
    worker.onmessage = (ev) => {
      const m = ev.data;
      if (m.type === 'progress') handlers.progress?.(m);
      else if (m.type === 'file') handlers.file?.(m);
      else if (m.type === 'done') { worker.terminate(); resolve(m); }
      else if (m.type === 'error') {
        worker.terminate();
        reject(Object.assign(new Error(m.path ? `${m.path}: ${m.message}` : m.message), { name: m.name }));
      }
    };
    worker.onerror = (ev) => { worker.terminate(); reject(new Error(ev.message || 'copy worker failed to start')); };
  });
  worker.postMessage({ type: 'start', root: rootName, items, chunk, stopAfter });
  return { promise, cancel: () => worker.postMessage({ type: 'abort' }) };
}

// After a successful copy: record what is in OPFS and where it came from, then seed the INIs.
export async function finishImport(rootName, plan, source, options = {}) {
  const root = await opfsRoot(rootName);
  const files = {};
  for (const e of plan.copy) files[e.path] = { size: e.size, lastModified: e.lastModified };
  const meta = {
    format: 1,
    importedAt: new Date().toISOString(),
    source: { kind: source.kind, name: source.name },
    groups: plan.groups.filter((g) => g.selected).map((g) => g.id),
    plugins: plan.plugins,
    bytes: plan.totalBytes,
    warnings: plan.warnings,
    files,
  };
  await writeJson(root, IMPORT_META, meta);
  const seeded = await seedConfig(root, options);
  return { meta, seeded };
}

export async function importStatus(rootName) {
  try {
    const root = await opfsRoot(rootName, false);
    return await readJson(root, IMPORT_META, null);
  } catch { return null; }
}
