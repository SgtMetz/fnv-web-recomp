// Install sources. Each one lists files as entries { path, size, lastModified } with install-relative '/' paths, and
// gives the copy worker something it can read from: a File (directory picker, <input webkitdirectory>, synthetic) or
// a URL (the localhost-only dev server). Nothing leaves the machine: Files are read in place, and the dev source is
// served from 127.0.0.1.
import { splitPath, foldName } from './winpath.js';

// The player may pick the install folder, its Data folder, or a folder above the install. Find the install root by
// locating FalloutNV.esm and strip whatever lies above it, case-insensitively.
export function rebase(entries) {
  const esm = entries.find((e) => {
    const parts = splitPath(e.path);
    return foldName(parts[parts.length - 1]) === 'FALLOUTNV.ESM';
  });
  if (!esm) return { entries: [], root: null, reason: 'FalloutNV.esm was not found in the chosen folder' };
  const parts = splitPath(esm.path);
  const parent = parts.slice(0, -1);
  let prefix, addData;
  if (parent.length && foldName(parent[parent.length - 1]) === 'DATA') { prefix = parent.slice(0, -1); addData = false; }
  else { prefix = parent; addData = true; }  // the Data folder itself was picked
  const out = [];
  for (const e of entries) {
    const p = splitPath(e.path);
    if (p.length <= prefix.length || prefix.some((x, i) => foldName(x) !== foldName(p[i]))) continue;
    const rel = p.slice(prefix.length);
    out.push({ ...e, path: (addData ? ['Data', ...rel] : rel).join('/') });
  }
  return { entries: out, root: prefix.join('/') || '.', pickedData: addData };
}

// showDirectoryPicker(): walk the handle tree. Directories that cannot hold game files are skipped.
export async function fromDirectoryHandle(dir) {
  const entries = [];
  const SKIP = /^(__overlay|redist|_commonredist|directx|support)$/i;
  async function walk(handle, prefix, depth) {
    for await (const [name, child] of handle.entries()) {
      const path = prefix ? `${prefix}/${name}` : name;
      if (child.kind === 'directory') {
        if (depth < 6 && !SKIP.test(name)) await walk(child, path, depth + 1);
      } else {
        const file = await child.getFile();
        entries.push({ path, size: file.size, lastModified: file.lastModified, file });
      }
    }
  }
  await walk(dir, '', 0);
  return { kind: 'directory', name: dir.name, entries };
}

// <input type=file webkitdirectory> fallback. webkitRelativePath starts with the picked folder's own name.
export function fromFileList(files) {
  const entries = [];
  let name = '';
  for (const file of files) {
    const rel = file.webkitRelativePath || file.name;
    const parts = splitPath(rel);
    if (!name && parts.length > 1) name = parts[0];
    entries.push({ path: parts.slice(1).join('/') || file.name, size: file.size, lastModified: file.lastModified, file });
  }
  return { kind: 'filelist', name, entries };
}

// Dev server (web/serve.mjs --install ...): the same files over HTTP range requests, for automated tests.
export async function fromDevServer(base = '/__dev_install/') {
  const r = await fetch(base + '__list.json', { cache: 'no-store' });
  if (!r.ok) throw new Error(`dev install listing failed: HTTP ${r.status}`);
  const list = await r.json();
  const baseUrl = new URL(base, location.href).href;
  return {
    kind: 'dev', name: 'dev install',
    entries: list.map((e) => ({ ...e, url: baseUrl + e.path.split('/').map(encodeURIComponent).join('/') })),
  };
}

// Tests: { 'Data/FalloutNV.esm': Uint8Array | Blob, ... } with optional { lastModified } per path.
export function fromMemory(files, times = {}) {
  const entries = Object.entries(files).map(([path, data]) => {
    const blob = data instanceof Blob ? data : new Blob([data]);
    const lastModified = times[path] ?? Date.UTC(2017, 4, 15, 16, 41, 34);
    const file = new File([blob], splitPath(path).pop(), { lastModified });
    return { path, size: file.size, lastModified, file };
  });
  return { kind: 'memory', name: 'synthetic install', entries };
}

// What the copy worker gets for one entry (Files and strings both survive postMessage).
export function readerFor(entry) {
  if (entry.file) return { file: entry.file };
  if (entry.url) return { url: entry.url };
  throw new Error(`no reader for ${entry.path}`);
}
