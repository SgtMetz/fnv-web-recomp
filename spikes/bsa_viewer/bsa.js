// BSA v104 (Fallout 3 / New Vegas) reader and DDS parser. Reads only the byte ranges it needs from a Source:
//   { size, read(offset, length) -> Promise<ArrayBuffer> }

export function fileSource(file) {
  return { size: file.size, name: file.name, read: (o, n) => file.slice(o, o + n).arrayBuffer() };
}

export function urlSource(url, size) {
  return {
    size, name: url,
    async read(o, n) {
      const r = await fetch(url, { headers: { Range: `bytes=${o}-${o + n - 1}` } });
      if (r.status !== 206) throw new Error(`range request failed: ${r.status}`);
      return r.arrayBuffer();
    },
  };
}

const ARCHIVE_DIRNAMES = 0x1, ARCHIVE_FILENAMES = 0x2, ARCHIVE_COMPRESSED = 0x4, ARCHIVE_EMBED_NAMES = 0x100;

export async function openBsa(src) {
  const h = new DataView(await src.read(0, 36));
  if (h.getUint32(0, true) !== 0x00415342) throw new Error('not a BSA');
  const version = h.getUint32(4, true);
  if (version !== 104) throw new Error(`unsupported BSA version ${version}`);
  const flags = h.getUint32(12, true), folderCount = h.getUint32(16, true), fileCount = h.getUint32(20, true);
  const folderNamesLen = h.getUint32(24, true), fileNamesLen = h.getUint32(28, true);
  if (!(flags & ARCHIVE_DIRNAMES) || !(flags & ARCHIVE_FILENAMES)) throw new Error('BSA without name tables');

  // folder records, then file record blocks (each prefixed by its folder name), then the file name block
  const recordsLen = folderCount * 16 + folderNamesLen + folderCount + fileCount * 16;
  const meta = new DataView(await src.read(36, recordsLen + fileNamesLen));
  const counts = [];
  for (let i = 0; i < folderCount; i++) counts.push(meta.getUint32(i * 16 + 8, true));
  const dec = new TextDecoder('latin1');
  let o = folderCount * 16;
  const files = [];
  for (const n of counts) {
    const len = meta.getUint8(o);
    const folder = dec.decode(new Uint8Array(meta.buffer, o + 1, len - 1));
    o += 1 + len;
    for (let j = 0; j < n; j++, o += 16) {
      files.push({ folder, size: meta.getUint32(o + 8, true), offset: meta.getUint32(o + 12, true) });
    }
  }
  const names = dec.decode(new Uint8Array(meta.buffer, o, fileNamesLen)).split('\0');
  files.forEach((f, i) => { f.path = `${f.folder}\\${names[i]}`; });
  return { src, version, flags, folderCount, fileCount, files };
}

async function inflate(bytes) {
  // zlib-wrapped deflate, as stored by BSA v104
  const ds = new Blob([bytes]).stream().pipeThrough(new DecompressionStream('deflate'));
  return new Uint8Array(await new Response(ds).arrayBuffer());
}

export async function readFile(bsa, f) {
  let size = f.size & 0x3fffffff;
  const compressed = !!(bsa.flags & ARCHIVE_COMPRESSED) !== !!(f.size & 0x40000000);
  let data = new Uint8Array(await bsa.src.read(f.offset, size));
  if (bsa.flags & ARCHIVE_EMBED_NAMES) data = data.subarray(1 + data[0]);
  if (!compressed) return data;
  const original = new DataView(data.buffer, data.byteOffset).getUint32(0, true);
  const out = await inflate(data.subarray(4));
  if (out.length !== original) throw new Error(`inflate size ${out.length} != ${original}`);
  return out;
}

// DDS -> { format, width, height, mips: [{width, height, data, bytesPerRow, rowsPerImage}] }
export function parseDds(bytes) {
  const v = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  if (v.getUint32(0, true) !== 0x20534444) throw new Error('not a DDS');
  const height = v.getUint32(12, true), width = v.getUint32(16, true);
  const mipCount = Math.max(1, v.getUint32(28, true));
  const pfFlags = v.getUint32(80, true), fourCC = v.getUint32(84, true), bits = v.getUint32(88, true);
  const rMask = v.getUint32(92, true), aMask = v.getUint32(104, true);
  const cc = String.fromCharCode(fourCC & 255, (fourCC >> 8) & 255, (fourCC >> 16) & 255, fourCC >>> 24);
  let format, block = 0, bpp = 0, expand = null;
  if (pfFlags & 0x4) {
    const bc = { DXT1: ['bc1-rgba-unorm', 8], DXT3: ['bc2-rgba-unorm', 16], DXT5: ['bc3-rgba-unorm', 16] }[cc];
    if (!bc) throw new Error(`unsupported fourCC ${cc}`);
    [format, block] = bc;
  } else if (bits === 32) {
    format = rMask === 0x00ff0000 ? 'bgra8unorm' : 'rgba8unorm'; bpp = 4;
    if (!aMask) expand = 'opaque32';
  } else if (bits === 24) {
    format = rMask === 0x00ff0000 ? 'bgra8unorm' : 'rgba8unorm'; bpp = 3; expand = 'rgb24';
  } else if (bits === 8) {
    format = 'rgba8unorm'; bpp = 1; expand = (pfFlags & 0x2) ? 'a8' : 'l8';
  } else throw new Error(`unsupported DDS pixel format (flags ${pfFlags.toString(16)}, ${bits} bpp)`);

  let o = 128;
  const mips = [];
  for (let i = 0, w = width, h = height; i < mipCount && o < bytes.length; i++) {
    let size, bytesPerRow, rows, data;
    if (block) {
      const bw = Math.max(1, Math.ceil(w / 4)), bh = Math.max(1, Math.ceil(h / 4));
      bytesPerRow = bw * block; rows = bh; size = bytesPerRow * bh;
      data = bytes.subarray(o, o + size);
      mips.push({ width: bw * 4, height: bh * 4, data, bytesPerRow, rowsPerImage: rows });
    } else {
      size = w * h * bpp;
      data = bytes.subarray(o, o + size);
      if (expand) data = expandPixels(data, w * h, expand);
      mips.push({ width: w, height: h, data, bytesPerRow: w * 4, rowsPerImage: h });
    }
    o += size;
    w = Math.max(1, w >> 1); h = Math.max(1, h >> 1);
  }
  return { format, fourCC: block ? cc : `${bits}bpp`, width, height, mips };
}

function expandPixels(src, n, kind) {
  const out = new Uint8Array(n * 4);
  for (let i = 0; i < n; i++) {
    if (kind === 'rgb24') { out.set(src.subarray(i * 3, i * 3 + 3), i * 4); out[i * 4 + 3] = 255; }
    else if (kind === 'opaque32') { out.set(src.subarray(i * 4, i * 4 + 3), i * 4); out[i * 4 + 3] = 255; }
    else if (kind === 'l8') { out[i * 4] = out[i * 4 + 1] = out[i * 4 + 2] = src[i]; out[i * 4 + 3] = 255; }
    else { out[i * 4] = out[i * 4 + 1] = out[i * 4 + 2] = 255; out[i * 4 + 3] = src[i]; }
  }
  return out;
}
