// Copy worker: streams install files into OPFS through sync access handles (only workers get those, and they are
// the fast path). Resumable: a journal in meta/import-journal.json records each file's source identity (size, time)
// and whether it finished, so a reload continues where it stopped instead of starting over.
//
// in:  { type: 'start', root, items: [{ path, size, lastModified, reader: {file}|{url} }], chunk?, stopAfter? }
//      (stopAfter: test hook, cancel once that many bytes are copied)
//      { type: 'abort' }
// out: { type: 'progress', bytes, total, index, count, path }
//      { type: 'file', path, status: 'copied'|'resumed'|'skipped', ms }
//      { type: 'done', copied, skipped, bytes, ms }  |  { type: 'error', path, message, name }
import { opfsRoot, fileAt, readJson, writeJson } from './opfs.js';
import { readBsaIndex } from './bsa.js';

const JOURNAL = 'meta/import-journal.json';
let aborted = false;

self.onmessage = (ev) => {
  const m = ev.data;
  if (m.type === 'abort') aborted = true;
  else if (m.type === 'start') run(m).catch((e) => post({ type: 'error', path: e.path, message: e.message, name: e.name }));
};

const post = (m) => self.postMessage(m);

function chunkReader(reader, size, chunk) {
  if (reader.file) return (o) => reader.file.slice(o, Math.min(size, o + chunk)).arrayBuffer();
  return async (o) => {
    const end = Math.min(size, o + chunk) - 1;
    const r = await fetch(reader.url, { headers: { Range: `bytes=${o}-${end}` }, cache: 'no-store' });
    if (r.status !== 206 && !(r.status === 200 && o === 0 && end === size - 1)) throw new Error(`HTTP ${r.status}`);
    return r.arrayBuffer();
  };
}

// Cheap structure checks on a finished file, read back from OPFS.
function verify(path, handle, size) {
  const read = (o, n) => { const b = new Uint8Array(n); const got = handle.read(b, { at: o }); return b.subarray(0, got); };
  if (/\.bsa$/i.test(path)) return readBsaIndex(read, size).then((ix) => `${ix.fileCount} files`);
  if (/\.es[mp]$/i.test(path)) {
    const tag = new TextDecoder().decode(read(0, 4));
    if (tag !== 'TES4') throw new Error(`plugin does not start with TES4 (got ${JSON.stringify(tag)})`);
  }
  return Promise.resolve('');
}

async function run({ root: rootName, items, chunk = 8 << 20, stopAfter = Infinity }) {
  aborted = false;
  const t0 = performance.now();
  const root = await opfsRoot(rootName);
  const journal = await readJson(root, JOURNAL, {});
  const total = items.reduce((s, it) => s + it.size, 0);
  let bytes = 0, copied = 0, skipped = 0, lastPost = 0;
  const progress = (index, path, force) => {
    const now = performance.now();
    if (force || now - lastPost > 100) { lastPost = now; post({ type: 'progress', bytes, total, index, count: items.length, path }); }
  };
  for (let i = 0; i < items.length; i++) {
    const it = items[i];
    const t1 = performance.now();
    const id = { size: it.size, lastModified: it.lastModified };
    const j = journal[it.path];
    const same = j && j.size === id.size && j.lastModified === id.lastModified;
    let handle;
    try {
      const fh = await fileAt(root, 'game/' + it.path, true);
      handle = await fh.createSyncAccessHandle();
      let start = 0;
      const have = handle.getSize();
      if (same && j.done && have === it.size) {
        skipped++; bytes += it.size;
        post({ type: 'file', path: it.path, status: 'skipped', ms: 0 });
        progress(i, it.path);
        continue;
      }
      if (same && !j.done && have <= it.size) start = have - (have % chunk); // continue a partial copy
      handle.truncate(start);
      journal[it.path] = { ...id, done: false };
      await writeJson(root, JOURNAL, journal);
      bytes += start;
      const next = chunkReader(it.reader, it.size, chunk);
      let pending = start < it.size ? next(start) : null;
      for (let o = start; o < it.size;) {
        if (aborted || bytes >= stopAfter) throw Object.assign(new Error('import cancelled'), { name: 'AbortError' });
        const buf = new Uint8Array(await pending);
        if (!buf.length) throw new Error(`source ended at ${o} of ${it.size} bytes`);
        const after = o + buf.length;
        pending = after < it.size ? next(after) : null; // read ahead while this chunk is written
        const wrote = handle.write(buf, { at: o });
        if (wrote !== buf.length) throw new Error(`short write at ${o}`);
        o = after; bytes += buf.length;
        progress(i, it.path);
      }
      handle.flush();
      const got = handle.getSize();
      if (got !== it.size) throw new Error(`copied ${got} bytes, expected ${it.size}`);
      const check = await verify(it.path, handle, it.size);
      journal[it.path] = { ...id, done: true };
      await writeJson(root, JOURNAL, journal);
      copied++;
      post({ type: 'file', path: it.path, status: start ? 'resumed' : 'copied', ms: performance.now() - t1, check });
      progress(i, it.path, true);
    } catch (e) {
      e.path = it.path;
      throw e;
    } finally {
      handle?.close();
    }
  }
  post({ type: 'done', copied, skipped, bytes, ms: performance.now() - t0 });
}
