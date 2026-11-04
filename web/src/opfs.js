// Small OPFS helpers shared by the page and the workers. Paths are '/'-separated and relative to a root folder.
import { splitPath } from './winpath.js';

export async function opfsRoot(name, create = true) {
  const top = await navigator.storage.getDirectory();
  return top.getDirectoryHandle(name, { create });
}

export async function dirAt(root, path, create = false) {
  let d = root;
  for (const part of splitPath(path)) d = await d.getDirectoryHandle(part, { create });
  return d;
}

export async function fileAt(root, path, create = false) {
  const parts = splitPath(path);
  const dir = await dirAt(root, parts.slice(0, -1).join('/'), create);
  return dir.getFileHandle(parts[parts.length - 1], { create });
}

export async function exists(root, path) {
  try { await fileAt(root, path); return true; } catch { return false; }
}

export async function readBytes(root, path) {
  const f = await (await fileAt(root, path)).getFile();
  return new Uint8Array(await f.arrayBuffer());
}

export async function readText(root, path, fallback = null) {
  try {
    const f = await (await fileAt(root, path)).getFile();
    return await f.text();
  } catch (e) {
    if (fallback !== null && e.name === 'NotFoundError') return fallback;
    throw e;
  }
}

export async function writeBytes(root, path, data) {
  const h = await fileAt(root, path, true);
  const w = await h.createWritable();
  await w.write(data);
  await w.close();
}

export async function readJson(root, path, fallback) {
  const text = await readText(root, path, '');
  return text ? JSON.parse(text) : fallback;
}

export async function writeJson(root, path, obj) {
  await writeBytes(root, path, new TextEncoder().encode(JSON.stringify(obj, null, 1)));
}

export async function remove(root, path, recursive = false) {
  const parts = splitPath(path);
  const dir = await dirAt(root, parts.slice(0, -1).join('/'));
  await dir.removeEntry(parts[parts.length - 1], { recursive });
}

// [{ path, kind, handle }] for everything below dir, depth-first.
export async function walk(dir, prefix = '') {
  const out = [];
  for await (const [name, h] of dir.entries()) {
    const path = prefix ? `${prefix}/${name}` : name;
    out.push({ path, kind: h.kind, handle: h });
    if (h.kind === 'directory') out.push(...await walk(h, path));
  }
  return out;
}

export async function removeRoot(name) {
  const top = await navigator.storage.getDirectory();
  try { await top.removeEntry(name, { recursive: true }); } catch (e) { if (e.name !== 'NotFoundError') throw e; }
}

export async function storageState() {
  const est = navigator.storage.estimate ? await navigator.storage.estimate() : {};
  const persisted = navigator.storage.persisted ? await navigator.storage.persisted() : false;
  return { quota: est.quota ?? 0, usage: est.usage ?? 0, persisted };
}
