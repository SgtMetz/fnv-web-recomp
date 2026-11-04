// BSA v104 (Fallout 3 / New Vegas) index reader, used by the importer to check copied archives and by tools that
// need to find a file without the engine (shader and video preprocessing). Grown from spikes/bsa_viewer/bsa.js.
//
// read(offset, length) may return a Uint8Array/ArrayBuffer or a Promise of one, so the same code runs over a File,
// an OPFS sync access handle, or HTTP range requests.

const MAGIC = 0x00415342; // "BSA\0"
export const FLAGS = { DIRNAMES: 0x1, FILENAMES: 0x2, COMPRESSED: 0x4, EMBED_NAMES: 0x100 };

const u8 = (x) => (x instanceof Uint8Array ? x : new Uint8Array(x));

export async function readBsaIndex(read, size) {
  const head = u8(await read(0, 36));
  if (head.length < 36) throw new Error('BSA: file too short');
  const h = new DataView(head.buffer, head.byteOffset, 36);
  if (h.getUint32(0, true) !== MAGIC) throw new Error('BSA: bad magic');
  const version = h.getUint32(4, true);
  if (version !== 104) throw new Error(`BSA: unsupported version ${version}`);
  const headerSize = h.getUint32(8, true), flags = h.getUint32(12, true);
  const folderCount = h.getUint32(16, true), fileCount = h.getUint32(20, true);
  const folderNamesLen = h.getUint32(24, true), fileNamesLen = h.getUint32(28, true), fileFlags = h.getUint32(32, true);
  if (headerSize !== 36) throw new Error(`BSA: header size ${headerSize}`);
  if (!(flags & FLAGS.DIRNAMES) || !(flags & FLAGS.FILENAMES)) throw new Error('BSA: no name tables');
  const recordsLen = folderCount * 16 + folderNamesLen + folderCount + fileCount * 16;
  if (36 + recordsLen + fileNamesLen > size) throw new Error('BSA: index runs past the end of the file');
  const meta = u8(await read(36, recordsLen + fileNamesLen));
  const v = new DataView(meta.buffer, meta.byteOffset, meta.byteLength);
  const dec = new TextDecoder('latin1');
  const folders = [];
  for (let i = 0; i < folderCount; i++) folders.push({ hash: v.getBigUint64(i * 16, true), count: v.getUint32(i * 16 + 8, true) });
  let o = folderCount * 16, total = 0;
  const files = [];
  for (const f of folders) {
    const len = v.getUint8(o);
    f.name = dec.decode(meta.subarray(o + 1, o + len)); // length includes the trailing NUL
    o += 1 + len;
    for (let j = 0; j < f.count; j++, o += 16) {
      const raw = v.getUint32(o + 8, true), offset = v.getUint32(o + 12, true);
      const stored = raw & 0x3fffffff;
      if (offset + stored > size) throw new Error(`BSA: file record ${files.length} runs past the end`);
      files.push({ folder: f.name, hash: v.getBigUint64(o, true), offset, stored,
                   compressed: !!(flags & FLAGS.COMPRESSED) !== !!(raw & 0x40000000) });
    }
    total += f.count;
  }
  if (total !== fileCount) throw new Error(`BSA: folders list ${total} files, header says ${fileCount}`);
  const names = dec.decode(meta.subarray(o, o + fileNamesLen)).split('\0');
  if (names.length < fileCount) throw new Error('BSA: file name table too short');
  files.forEach((f, i) => { f.name = names[i]; f.path = `${f.folder}\\${names[i]}`; });
  return { version, flags, fileFlags, folderCount, fileCount, files };
}

// Bytes of one file, inflated if needed. Needs DecompressionStream (all current browsers).
export async function readBsaFile(read, index, f) {
  let data = u8(await read(f.offset, f.stored));
  if (index.flags & FLAGS.EMBED_NAMES) data = data.subarray(1 + data[0]);
  if (!f.compressed) return data;
  const original = new DataView(data.buffer, data.byteOffset).getUint32(0, true);
  const ds = new Blob([data.subarray(4)]).stream().pipeThrough(new DecompressionStream('deflate'));
  const out = new Uint8Array(await new Response(ds).arrayBuffer());
  if (out.length !== original) throw new Error(`BSA: ${f.path} inflated to ${out.length}, expected ${original}`);
  return out;
}

// Writer for tests: builds a valid v104 archive (uncompressed, embedded names off) from { 'dir\\name': bytes }.
// Hashes are not the game's (the reader does not need them); folders and files are written sorted by name.
export function buildBsa(entries) {
  const enc = new TextEncoder();
  const byFolder = new Map();
  for (const [path, data] of Object.entries(entries)) {
    const i = path.lastIndexOf('\\');
    const folder = path.slice(0, i).toLowerCase(), name = path.slice(i + 1).toLowerCase();
    if (!byFolder.has(folder)) byFolder.set(folder, []);
    byFolder.get(folder).push({ name, data: u8(data) });
  }
  const folders = [...byFolder].sort(([a], [b]) => (a < b ? -1 : 1));
  const fileCount = folders.reduce((s, [, f]) => s + f.length, 0);
  const folderNamesLen = folders.reduce((s, [n]) => s + n.length + 1, 0);
  const fileNamesLen = folders.reduce((s, [, fs]) => s + fs.reduce((t, f) => t + f.name.length + 1, 0), 0);
  const recordsLen = folders.length * 16 + folderNamesLen + folders.length + fileCount * 16;
  let dataOffset = 36 + recordsLen + fileNamesLen;
  const dataLen = folders.reduce((s, [, fs]) => s + fs.reduce((t, f) => t + f.data.length, 0), 0);
  const out = new Uint8Array(dataOffset + dataLen);
  const v = new DataView(out.buffer);
  v.setUint32(0, MAGIC, true); v.setUint32(4, 104, true); v.setUint32(8, 36, true);
  v.setUint32(12, FLAGS.DIRNAMES | FLAGS.FILENAMES, true);
  v.setUint32(16, folders.length, true); v.setUint32(20, fileCount, true);
  v.setUint32(24, folderNamesLen, true); v.setUint32(28, fileNamesLen, true);
  let o = 36, hash = 1n;
  const fileBlocks = 36 + folders.length * 16;
  let fb = fileBlocks;
  for (const [name, fs] of folders) {
    v.setBigUint64(o, hash++, true); v.setUint32(o + 8, fs.length, true);
    v.setUint32(o + 12, fb + fileNamesLen, true); // offset field: like the game's, points past the name table
    o += 16;
    fb += 1 + name.length + 1 + fs.length * 16;
  }
  let names = 36 + recordsLen;
  for (const [name, fs] of folders) {
    out[o] = name.length + 1; out.set(enc.encode(name), o + 1); o += 1 + name.length + 1;
    for (const f of fs) {
      v.setBigUint64(o, hash++, true); v.setUint32(o + 8, f.data.length, true); v.setUint32(o + 12, dataOffset, true);
      out.set(f.data, dataOffset); dataOffset += f.data.length;
      o += 16;
      out.set(enc.encode(f.name), names); names += f.name.length + 1;
    }
  }
  return out;
}
