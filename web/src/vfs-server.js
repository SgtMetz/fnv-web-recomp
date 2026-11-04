// I/O worker: owns every OPFS handle and answers synchronous file requests from engine threads (see vfs-proto.js).
//
// It presents OPFS with Windows semantics: drive-letter paths mapped through layout.MOUNTS, a process-wide current
// directory, case-insensitive names (the real case is kept for listings), FindFirstFile wildcards in NTFS order,
// and per-file last-write times. OPFS cannot set a file's time and a copied file gets the copy time, so the times
// come from meta/import.json (each file's time in the player's install) and meta/times.json (files the game wrote).
// The game's plugin load order depends on them (TESDataHandler 0x004624b0 sorts Data\*.esm by ftLastWriteTime).
//
// in: { type: 'init', root, shared: SharedArrayBuffer, memory: WebAssembly.Memory | SharedArrayBuffer }
// out: { type: 'ready', files, dirs, ms }  |  { type: 'error', message }
import { layout, OP, O, SEEK, ERR, F, S_POSTED, S_ANSWERED, FD_CACHEABLE, FD_OPEN } from './vfs-proto.js';
import { MOUNTS, CWD, IMPORT_META, TIMES_META, GAME } from './layout.js';
import { splitPath, foldName, compareNtfs, wildcardMatch } from './winpath.js';
import { opfsRoot } from './opfs.js';

class Node {
  constructor(name, kind, handle, parent) {
    this.name = name; this.kind = kind; this.handle = handle; this.parent = parent;
    this.children = kind === 'directory' ? new Map() : null;
    this.mtime = undefined;   // ms; undefined = ask OPFS
    this.access = null; this.mode = null; this.open = 0; this.dirty = false;
  }
  get path() { return this.parent ? (this.parent.parent ? `${this.parent.path}/${this.name}` : this.name) : ''; }
}

const fail = (code, msg) => Object.assign(new Error(msg || `win32 error ${code}`), { win32: code });
const domToWin32 = {
  NotFoundError: ERR.FILE_NOT_FOUND, TypeMismatchError: ERR.ACCESS_DENIED, NoModificationAllowedError: ERR.SHARING_VIOLATION,
  QuotaExceededError: ERR.DISK_FULL, InvalidModificationError: ERR.DIR_NOT_EMPTY, InvalidStateError: ERR.INVALID_HANDLE,
};

let rootNode, memory, cwd = CWD, timesDirty = null, sabReadOk = true;
let fdt = null;          // shared descriptor table (positions, generations, flags), see vfs-proto.js
const freeFds = [];      // lowest numbers handed out first; 0-2 are never used
const fds = new Map();
const times = {};   // node path -> ms (files written by the game)
const enc = new TextEncoder(), dec = new TextDecoder();
const mounts = MOUNTS.map((m) => ({ ...m, parts: splitPath(m.win).map(foldName) }));

const memBuffer = () => (memory instanceof SharedArrayBuffer ? memory : memory.buffer);

async function buildTree(dirHandle, node) {
  let files = 0, dirs = 0;
  for await (const [name, h] of dirHandle.entries()) {
    const child = new Node(name, h.kind, h, node);
    node.children.set(foldName(name), child);
    if (h.kind === 'directory') { dirs++; const c = await buildTree(h, child); files += c.files; dirs += c.dirs; }
    else files++;
  }
  return { files, dirs };
}

async function ensureDir(parent, name) {
  const key = foldName(name);
  let n = parent.children.get(key);
  if (!n) {
    n = new Node(name, 'directory', await parent.handle.getDirectoryHandle(name, { create: true }), parent);
    parent.children.set(key, n);
  }
  return n;
}

function lookupParts(parts) {
  let n = rootNode;
  for (const p of parts) {
    if (!n.children) return null;
    n = n.children.get(foldName(p));
    if (!n) return null;
  }
  return n;
}

// Windows path -> OPFS components below the root, or null when no mount covers it.
function toParts(path) {
  let s = String(path).replace(/^\\\\\?\\/, '');
  if (/^[a-zA-Z]:/.test(s)) { /* absolute (or drive-relative, treated as absolute) */ }
  else if (/^[\\/]/.test(s)) s = cwd.slice(0, 2) + s;
  else s = cwd + '\\' + s;
  const parts = splitPath(s);
  for (const m of mounts) {
    if (parts.length >= m.parts.length && m.parts.every((x, i) => foldName(parts[i]) === x))
      return [m.dir, ...parts.slice(m.parts.length)];
  }
  return null;
}

function resolve(path) {
  const parts = toParts(path);
  if (!parts) throw fail(ERR.PATH_NOT_FOUND, `outside every mount: ${path}`);
  return { parts, node: lookupParts(parts) };
}

function parentOf(parts) {
  const dir = lookupParts(parts.slice(0, -1));
  if (!dir || dir.kind !== 'directory') throw fail(ERR.PATH_NOT_FOUND, `no folder for ${parts.join('/')}`);
  return { dir, name: parts[parts.length - 1] };
}

async function fileInfo(node) {
  if (node.kind === 'directory') return { size: 0, mtime: node.mtime ?? 0 };
  if (node.access) return { size: node.access.getSize(), mtime: node.mtime ?? Date.now() };
  const f = await node.handle.getFile();
  return { size: f.size, mtime: node.mtime ?? f.lastModified };
}

function touch(node) {
  node.mtime = Date.now();
  times[node.path] = node.mtime;
  if (!timesDirty) timesDirty = setTimeout(saveTimes, 300);
}

async function saveTimes() {
  timesDirty = null;
  const meta = await ensureDir(rootNode, 'meta');
  const h = await meta.handle.getFileHandle('times.json', { create: true });
  const w = await h.createWritable();
  await w.write(JSON.stringify(times));
  await w.close();
}

async function ensureAccess(node, write) {
  if (node.access && (!write || node.mode === 'readwrite')) return;
  release(node);
  const mode = write ? 'readwrite' : 'read-only';
  try { node.access = await node.handle.createSyncAccessHandle({ mode }); }
  catch (e) {
    if (e.name === 'NoModificationAllowedError') throw fail(ERR.SHARING_VIOLATION, `${node.path} is in use`);
    throw e;
  }
  node.mode = mode;
}

function release(node) {
  // flush() throws on a read-only handle (spec: NoModificationAllowedError), so only flush writable ones
  if (node.access) {
    if (node.mode === 'readwrite') node.access.flush();
    node.access.close(); node.access = null; node.mode = null;
  }
}

function fdOf(fd) {
  const f = fds.get(fd);
  if (!f) throw fail(ERR.INVALID_HANDLE, `bad fd ${fd}`);
  return f;
}

function readPath(slot, off = 0) {
  const len = slot.i32[F.SCRATCH_LEN];
  const bytes = slot.scratch.slice(0, len); // copy: TextDecoder refuses shared views
  const text = dec.decode(bytes);
  return off ? text.split('\0')[off] : text.split('\0')[0];
}

function writeOut(slot, text) {
  const b = enc.encode(text);
  if (b.length > slot.scratch.length) throw fail(ERR.INSUFFICIENT_BUFFER, `result needs ${b.length} bytes`);
  slot.scratch.set(b);
  slot.i32[F.SCRATCH_LEN] = b.length;
}

function ioRead(access, ptr, len, at) {
  const buf = memBuffer();
  if (ptr < 0 || ptr + len > buf.byteLength) throw fail(ERR.INVALID_PARAMETER, 'buffer outside memory');
  if (sabReadOk) {
    try { return access.read(new Uint8Array(buf, ptr, len), { at }); }
    catch (e) { if (e.name !== 'TypeError') throw e; sabReadOk = false; }
  }
  const tmp = new Uint8Array(len);
  const n = access.read(tmp, { at });
  new Uint8Array(buf, ptr, n).set(tmp.subarray(0, n));
  return n;
}

function ioWrite(access, ptr, len, at) {
  const buf = memBuffer();
  if (ptr < 0 || ptr + len > buf.byteLength) throw fail(ERR.INVALID_PARAMETER, 'buffer outside memory');
  if (sabReadOk) {
    try { return access.write(new Uint8Array(buf, ptr, len), { at }); }
    catch (e) { if (e.name !== 'TypeError') throw e; sabReadOk = false; }
  }
  return access.write(new Uint8Array(buf, ptr, len).slice(), { at });
}

const handlers = {
  [OP.PING]: async () => 1,

  [OP.OPEN]: async (s) => {
    const flags = s.i32[F.ARG + 1];
    const { parts, node: found } = resolve(readPath(s));
    let node = found;
    if (node && node.kind === 'directory') throw fail(ERR.ACCESS_DENIED, 'is a folder');
    if (node && (flags & O.CREATE) && (flags & O.EXCL)) throw fail(ERR.FILE_EXISTS);
    if (!node) {
      if (!(flags & O.CREATE)) {
        const dir = lookupParts(parts.slice(0, -1));
        throw fail(dir && dir.kind === 'directory' ? ERR.FILE_NOT_FOUND : ERR.PATH_NOT_FOUND);
      }
      const { dir, name } = parentOf(parts);
      node = new Node(name, 'file', await dir.handle.getFileHandle(name, { create: true }), dir);
      dir.children.set(foldName(name), node);
      touch(node);
    }
    await ensureAccess(node, !!(flags & (O.WRITE | O.TRUNC)));
    if (flags & O.TRUNC) { node.access.truncate(0); node.dirty = true; }
    if (flags & (O.WRITE | O.TRUNC)) {
      // someone writes a file other descriptors may be caching: drop their windows
      for (const [other, f] of fds) if (f.node === node) Atomics.add(fdt.gen, other, 1);
    }
    const fd = freeFds.pop();
    if (fd === undefined) throw fail(ERR.TOO_MANY_OPEN_FILES);
    fdt.pos[fd] = 0;
    Atomics.add(fdt.gen, fd, 1);
    const cacheable = !(flags & (O.WRITE | O.TRUNC)) && node.path.split('/')[0] === GAME;
    Atomics.store(fdt.flags, fd, FD_OPEN | (cacheable ? FD_CACHEABLE : 0));
    fds.set(fd, { node, flags, get pos() { return fdt.pos[fd]; }, set pos(v) { fdt.pos[fd] = v; } });
    node.open++;
    s.f64[0] = node.access.getSize();
    s.i32[F.ARG + 4] = cacheable ? FD_CACHEABLE : 0;
    return fd;
  },

  [OP.CLOSE]: async (s) => {
    const fd = s.i32[F.ARG], f = fdOf(fd);
    fds.delete(fd);
    Atomics.store(fdt.flags, fd, 0);
    Atomics.add(fdt.gen, fd, 1); // invalidates every thread's read-ahead window for this number
    freeFds.push(fd);
    const n = f.node;
    n.open--;
    if (n.dirty) { n.access.flush(); touch(n); n.dirty = false; }
    if (n.open === 0 && n.mode === 'readwrite') release(n); // let the page read saves and INIs again
    return 0;
  },

  [OP.READ]: async (s) => {
    const f = fdOf(s.i32[F.ARG]);
    if (!(f.flags & O.READ)) throw fail(ERR.ACCESS_DENIED, 'not opened for reading');
    const ptr = s.i32[F.ARG + 2] >>> 0, len = s.i32[F.ARG + 3] >>> 0, at = s.f64[0];
    const pos = at < 0 ? f.pos : at;
    const n = ioRead(f.node.access, ptr, len, pos);
    if (at < 0) f.pos = pos + n;
    return n;
  },

  // Read-ahead fill: up to len bytes at f64[0] into the slot's scratch (the caller copies them to its window).
  [OP.READ_SCRATCH]: async (s) => {
    const f = fdOf(s.i32[F.ARG]);
    if (!(f.flags & O.READ)) throw fail(ERR.ACCESS_DENIED, 'not opened for reading');
    const len = Math.min(s.i32[F.ARG + 3] >>> 0, s.scratch.length), at = s.f64[0];
    if (sabReadOk) {
      try { return f.node.access.read(s.scratch.subarray(0, len), { at }); }
      catch (e) { if (e.name !== 'TypeError') throw e; sabReadOk = false; }
    }
    const tmp = new Uint8Array(len);
    const n = f.node.access.read(tmp, { at });
    s.scratch.set(tmp.subarray(0, n));
    return n;
  },

  [OP.WRITE]: async (s) => {
    const f = fdOf(s.i32[F.ARG]);
    if (!(f.flags & O.WRITE)) throw fail(ERR.ACCESS_DENIED, 'not opened for writing');
    const ptr = s.i32[F.ARG + 2] >>> 0, len = s.i32[F.ARG + 3] >>> 0, at = s.f64[0];
    const pos = at < 0 ? f.pos : at;
    const n = ioWrite(f.node.access, ptr, len, pos);
    if (at < 0) f.pos = pos + n;
    f.node.dirty = true;
    return n;
  },

  [OP.SEEK]: async (s) => {
    const f = fdOf(s.i32[F.ARG]), whence = s.i32[F.ARG + 1], off = s.f64[0];
    const base = whence === SEEK.SET ? 0 : whence === SEEK.CUR ? f.pos : f.node.access.getSize();
    const pos = base + off;
    if (pos < 0) throw fail(ERR.NEGATIVE_SEEK, 'negative position');
    f.pos = pos;
    s.f64[0] = pos;
    return 0;
  },

  [OP.SIZE]: async (s) => { s.f64[0] = fdOf(s.i32[F.ARG]).node.access.getSize(); return 0; },

  [OP.TRUNCATE]: async (s) => {
    const f = fdOf(s.i32[F.ARG]);
    if (!(f.flags & O.WRITE)) throw fail(ERR.ACCESS_DENIED);
    f.node.access.truncate(s.f64[0]);
    f.node.dirty = true;
    return 0;
  },

  [OP.FLUSH]: async (s) => { const n = fdOf(s.i32[F.ARG]).node; if (n.mode === 'readwrite') n.access.flush(); return 0; },

  // result: 0 missing, 1 file, 2 folder; f64[0] size, f64[1] last-write time (ms)
  [OP.STAT]: async (s) => {
    const { node } = resolve(readPath(s));
    if (!node) return 0;
    const info = await fileInfo(node);
    s.f64[0] = info.size; s.f64[1] = info.mtime;
    return node.kind === 'directory' ? 2 : 1;
  },

  // FindFirstFileA: "<folder>\<pattern>". Scratch out: JSON [[name, isDir, size, mtime], ...] in NTFS order.
  [OP.LIST]: async (s) => {
    const parts = toParts(readPath(s));
    if (!parts || parts.length < 2) throw fail(ERR.PATH_NOT_FOUND);
    const pattern = parts.pop();
    const dir = lookupParts(parts);
    if (!dir || dir.kind !== 'directory') throw fail(ERR.PATH_NOT_FOUND);
    const rows = [];
    if (wildcardMatch(pattern, '.')) rows.push(['.', 1, 0, dir.mtime ?? 0], ['..', 1, 0, dir.parent?.mtime ?? 0]);
    const kids = [...dir.children.values()].filter((c) => wildcardMatch(pattern, c.name));
    kids.sort((a, b) => compareNtfs(a.name, b.name));
    for (const c of kids) {
      const info = await fileInfo(c);
      rows.push([c.name, c.kind === 'directory' ? 1 : 0, info.size, info.mtime]);
    }
    writeOut(s, JSON.stringify(rows));
    return rows.length;
  },

  [OP.MKDIR]: async (s) => {
    const { parts, node } = resolve(readPath(s));
    if (node) throw fail(ERR.ALREADY_EXISTS);
    const { dir, name } = parentOf(parts);
    const n = await ensureDir(dir, name);
    n.mtime = Date.now();
    return 0;
  },

  [OP.UNLINK]: async (s) => {
    const { node } = resolve(readPath(s));
    if (!node) throw fail(ERR.FILE_NOT_FOUND);
    if (node.kind === 'directory') throw fail(ERR.ACCESS_DENIED);
    if (node.open) throw fail(ERR.SHARING_VIOLATION, 'file is open');
    release(node);
    await node.parent.handle.removeEntry(node.name);
    node.parent.children.delete(foldName(node.name));
    delete times[node.path];
    return 0;
  },

  [OP.RMDIR]: async (s) => {
    const { node } = resolve(readPath(s));
    if (!node || node.kind !== 'directory') throw fail(ERR.PATH_NOT_FOUND);
    if (node.children.size) throw fail(ERR.DIR_NOT_EMPTY);
    await node.parent.handle.removeEntry(node.name);
    node.parent.children.delete(foldName(node.name));
    return 0;
  },

  // MoveFileExA: args[1] & 1 = MOVEFILE_REPLACE_EXISTING. Scratch: "from\0to".
  [OP.RENAME]: async (s) => {
    const replace = s.i32[F.ARG + 1] & 1;
    const from = resolve(readPath(s, 0)), toPath = readPath(s, 1);
    const to = resolve(toPath);
    if (!from.node) throw fail(ERR.FILE_NOT_FOUND);
    const n = from.node;
    if (n.open) throw fail(ERR.SHARING_VIOLATION, 'file is open');
    const { dir, name } = parentOf(to.parts);
    const sameEntry = to.node === n;
    if (to.node && !sameEntry) {
      if (!replace || to.node.kind === 'directory') throw fail(ERR.ALREADY_EXISTS);
      if (to.node.open) throw fail(ERR.ACCESS_DENIED);
      release(to.node);
      await dir.handle.removeEntry(to.node.name);
      dir.children.delete(foldName(to.node.name));
      delete times[to.node.path];
    }
    release(n);
    const oldPath = n.path;
    if (typeof n.handle.move === 'function') await n.handle.move(dir.handle, name);
    else if (n.kind === 'file') { // copy + delete for browsers without FileSystemHandle.move
      const data = await (await n.handle.getFile()).arrayBuffer();
      const h = await dir.handle.getFileHandle(name, { create: true });
      const w = await h.createWritable(); await w.write(data); await w.close();
      await n.parent.handle.removeEntry(n.name);
      n.handle = h;
    } else throw fail(ERR.ACCESS_DENIED, 'folder rename unsupported here');
    n.parent.children.delete(foldName(n.name));
    n.name = name; n.parent = dir;
    dir.children.set(foldName(name), n);
    if (times[oldPath] !== undefined) { delete times[oldPath]; times[n.path] = n.mtime; }
    else if (n.mtime !== undefined) times[n.path] = n.mtime;
    if (!timesDirty) timesDirty = setTimeout(saveTimes, 300);
    return 0;
  },

  [OP.SETTIME]: async (s) => {
    const { node } = resolve(readPath(s));
    if (!node) throw fail(ERR.FILE_NOT_FOUND);
    node.mtime = s.f64[0];
    times[node.path] = node.mtime;
    if (!timesDirty) timesDirty = setTimeout(saveTimes, 300);
    return 0;
  },

  [OP.CHDIR]: async (s) => {
    const p = readPath(s);
    const { node } = resolve(p);
    if (!node || node.kind !== 'directory') throw fail(ERR.PATH_NOT_FOUND);
    const abs = /^[a-zA-Z]:/.test(p) ? p : /^[\\/]/.test(p) ? cwd.slice(0, 2) + p : cwd + '\\' + p;
    cwd = splitPath(abs).join('\\');
    return 0;
  },

  [OP.GETCWD]: async (s) => { writeOut(s, cwd); return cwd.length; },
};

// Requests from all slots are handled one at a time: OPFS handle creation is async, and two interleaved opens of
// the same file would race for its access handle.
let queue = Promise.resolve();
function serialize(fn) { const p = queue.then(fn, fn); queue = p.catch(() => {}); return p; }

async function serveSlot(slot) {
  const st = slot.i32;
  for (;;) {
    let v;
    while ((v = Atomics.load(st, F.STATE)) !== S_POSTED) {
      const w = Atomics.waitAsync(st, F.STATE, v);
      if (w.async) await w.value;
    }
    await serialize(async () => {
      let result = 0, error = 0;
      try {
        const h = handlers[st[F.OP]];
        if (!h) throw fail(ERR.INVALID_PARAMETER, `unknown op ${st[F.OP]}`);
        result = await h(slot);
      } catch (e) {
        error = e.win32 ?? domToWin32[e.name] ?? ERR.GEN_FAILURE;
        result = -1;
        if (!e.win32) console.warn('[vfs]', e);
      }
      st[F.RESULT] = result;
      st[F.ERROR] = error;
      Atomics.store(st, F.STATE, S_ANSWERED);
      Atomics.notify(st, F.STATE);
    });
  }
}

async function init({ root: rootName, shared, memory: mem }) {
  const t0 = performance.now();
  memory = mem;
  const rootHandle = await opfsRoot(rootName);
  rootNode = new Node('', 'directory', rootHandle, null);
  const counts = await buildTree(rootHandle, rootNode);
  for (const m of MOUNTS) await ensureDir(rootNode, m.dir);
  const docs = lookupParts(['docs']);
  await ensureDir(await ensureDir(docs, 'My Games'), 'FalloutNV');
  // last-write times: the install's (import.json), then the game's own writes (times.json)
  const readMeta = async (path) => {
    const n = lookupParts(splitPath(path));
    if (!n) return null;
    try { return JSON.parse(await (await n.handle.getFile()).text()); } catch { return null; }
  };
  const imp = await readMeta(IMPORT_META);
  let timed = 0;
  for (const [path, info] of Object.entries(imp?.files ?? {})) {
    const n = lookupParts([GAME, ...splitPath(path)]);
    if (n && info.lastModified) { n.mtime = info.lastModified; timed++; }
  }
  for (const [path, ms] of Object.entries((await readMeta(TIMES_META)) ?? {})) {
    const n = lookupParts(splitPath(path));
    if (n) { n.mtime = ms; times[n.path] = ms; timed++; }
  }
  const { slots, fd, maxFds } = layout(shared);
  fdt = fd;
  for (let i = maxFds - 1; i >= 3; i--) freeFds.push(i);
  for (const s of slots) serveSlot(s);
  self.postMessage({ type: 'ready', files: counts.files, dirs: counts.dirs, timed, ms: performance.now() - t0 });
}

// Close every handle so the page (or another tab) can read, replace or delete the files. Pending requests finish
// first; the worker should be terminated after 'closed'.
async function shutdown() {
  await serialize(async () => {
    const stack = [rootNode];
    while (stack.length) {
      const n = stack.pop();
      if (n.children) stack.push(...n.children.values());
      else release(n);
    }
    fds.clear();
    if (timesDirty) { clearTimeout(timesDirty); await saveTimes(); }
  });
  self.postMessage({ type: 'closed' });
}

self.onmessage = (ev) => {
  const t = ev.data?.type;
  if (t === 'init') init(ev.data).catch((e) => self.postMessage({ type: 'error', message: e.message }));
  else if (t === 'shutdown') shutdown().catch((e) => self.postMessage({ type: 'error', message: e.message }));
};
