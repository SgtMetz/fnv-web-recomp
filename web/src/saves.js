// PC save files (.fos), kept byte-for-byte in docs/My Games/FalloutNV/Saves so they move freely between the
// browser and a PC install. The header reader below is for the save list only (name, level, place, picture); the
// game reads and writes the files itself.
//
// Header layout, checked against three saves from the reference install (version 0x30, English):
//   "FO3SAVEGAME"  u32 headerSize (bytes from offset 15 to the screenshot)  u32 version  '|'
//   char language[64] '|'  u32 width '|'  u32 height '|'  u32 saveNumber '|'
//   bstr name '|'  bstr title '|'  u32 level '|'  bstr location '|'  bstr playtime '|'
//   RGB screenshot (width*height*3)  u8 formVersion  u32 pluginInfoSize  u8 pluginCount '|'  (bstr plugin '|')*
// where bstr = u16 length '|' bytes. The plugin list is the save's load order.
import { dirAt, fileAt, readBytes, writeBytes, remove } from './opfs.js';
import { SAVES } from './layout.js';
import { foldName } from './winpath.js';

const MAGIC = 'FO3SAVEGAME';

export function parseFos(bytes, { screenshot = true } = {}) {
  const b = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
  const v = new DataView(b.buffer, b.byteOffset, b.byteLength);
  const dec = new TextDecoder('latin1');
  if (b.length < 32 || dec.decode(b.subarray(0, 11)) !== MAGIC) throw new Error('not a Fallout save (no FO3SAVEGAME)');
  let o = 11;
  const need = (n) => { if (o + n > b.length) throw new Error(`save header truncated at ${o}`); };
  const u8 = () => { need(1); return b[o++]; };
  const u16 = () => { need(2); const x = v.getUint16(o, true); o += 2; return x; };
  const u32 = () => { need(4); const x = v.getUint32(o, true); o += 4; return x; };
  const bar = () => { need(1); if (b[o] !== 0x7c) throw new Error(`expected '|' at ${o}`); o++; };
  const bstr = () => { const n = u16(); bar(); need(n); const s = dec.decode(b.subarray(o, o + n)); o += n; return s; };

  const headerSize = u32();
  const version = u32(); bar();
  need(64);
  const language = dec.decode(b.subarray(o, o + 64)).replace(/\0.*$/s, ''); o += 64; bar();
  const width = u32(); bar();
  const height = u32(); bar();
  const saveNumber = u32(); bar();
  const name = bstr(); bar();
  const title = bstr(); bar();
  const level = u32(); bar();
  const location = bstr(); bar();
  const playtime = bstr(); bar();
  const shotAt = 15 + headerSize;
  if (o !== shotAt) throw new Error(`header ends at ${o}, headerSize says ${shotAt}`);
  if (width > 4096 || height > 4096) throw new Error(`implausible screenshot size ${width}x${height}`);
  o += width * height * 3;
  const formVersion = u8();
  const pluginInfoSize = u32();
  const pluginStart = o;
  const count = u8(); bar();
  const plugins = [];
  for (let i = 0; i < count; i++) { plugins.push(bstr()); bar(); }
  if (o - pluginStart !== pluginInfoSize) throw new Error(`plugin list is ${o - pluginStart} bytes, header says ${pluginInfoSize}`);
  const out = { version, language, saveNumber, name, title, level, location, playtime, formVersion, plugins, width, height };
  if (screenshot) out.screenshot = b.subarray(shotAt, shotAt + width * height * 3);
  return out;
}

// RGB screenshot -> ImageData (for a <canvas> thumbnail).
export function screenshotImage(info) {
  const { width, height, screenshot } = info;
  const rgba = new Uint8ClampedArray(width * height * 4);
  for (let i = 0, j = 0; i < width * height; i++, j += 3) {
    rgba[i * 4] = screenshot[j]; rgba[i * 4 + 1] = screenshot[j + 1]; rgba[i * 4 + 2] = screenshot[j + 2]; rgba[i * 4 + 3] = 255;
  }
  return new ImageData(rgba, width, height);
}

// [{ file, size, lastModified, info | error }] newest first.
export async function listSaves(root) {
  const dir = await dirAt(root, SAVES, true);
  const out = [];
  for await (const [name, h] of dir.entries()) {
    if (h.kind !== 'file' || !/\.fos$/i.test(name)) continue;
    const f = await h.getFile();
    const row = { file: name, size: f.size, lastModified: f.lastModified };
    try { row.info = parseFos(new Uint8Array(await f.slice(0, 2 << 20).arrayBuffer())); }
    catch (e) { row.error = e.message; }
    out.push(row);
  }
  return out.sort((a, b) => b.lastModified - a.lastModified);
}

// Copy picked/dropped .fos files in. Existing names (compared as Windows does) are skipped unless replace is set.
export async function importSaves(root, files, { replace = false } = {}) {
  const dir = await dirAt(root, SAVES, true);
  const have = new Map();
  for await (const [name] of dir.entries()) have.set(foldName(name), name);
  const result = [];
  for (const file of files) {
    if (!/\.fos$/i.test(file.name)) { result.push({ file: file.name, status: 'skipped', reason: 'not a .fos file' }); continue; }
    const bytes = new Uint8Array(await file.arrayBuffer());
    try { parseFos(bytes, { screenshot: false }); }
    catch (e) { result.push({ file: file.name, status: 'rejected', reason: e.message }); continue; }
    const existing = have.get(foldName(file.name));
    if (existing && !replace) { result.push({ file: file.name, status: 'exists' }); continue; }
    const name = existing ?? file.name;
    await writeBytes(root, `${SAVES}/${name}`, bytes);
    have.set(foldName(name), name);
    result.push({ file: name, status: existing ? 'replaced' : 'imported' });
  }
  return result;
}

export async function saveBlob(root, name) {
  return new Blob([await readBytes(root, `${SAVES}/${name}`)], { type: 'application/octet-stream' });
}

// Export: a native "save as" dialog where available, else a download link.
export async function exportSave(root, name) {
  const blob = await saveBlob(root, name);
  if (window.showSaveFilePicker) {
    try {
      const h = await window.showSaveFilePicker({ suggestedName: name, types: [{ description: 'Fallout save', accept: { 'application/octet-stream': ['.fos'] } }] });
      const w = await h.createWritable(); await w.write(blob); await w.close();
      return 'saved';
    } catch (e) { if (e.name === 'AbortError') return 'cancelled'; }
  }
  const a = Object.assign(document.createElement('a'), { href: URL.createObjectURL(blob), download: name });
  document.body.append(a); a.click(); a.remove();
  setTimeout(() => URL.revokeObjectURL(a.href), 10000);
  return 'downloaded';
}

export async function deleteSave(root, name) {
  await remove(root, `${SAVES}/${name}`);
  await fileAt(root, `${SAVES}/${name}`).then(() => { throw new Error('delete failed'); }, () => {});
}
