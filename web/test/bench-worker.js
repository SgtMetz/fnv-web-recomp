// Micro-benchmark of OPFS sync access handle reads, without the VFS round trip: is the per-read cost in OPFS
// itself, in reading into a SharedArrayBuffer, or in the handle mode?
self.onmessage = async (ev) => {
  const { root: rootName, size = 16 << 20 } = ev.data;
  const top = await navigator.storage.getDirectory();
  const root = await top.getDirectoryHandle(rootName, { create: true });
  const fh = await root.getFileHandle('bench.bin', { create: true });
  let h = await fh.createSyncAccessHandle();
  const fill = new Uint8Array(1 << 20);
  for (let o = 0; o < size; o += fill.length) h.write(fill, { at: o });
  h.flush(); h.close();
  const res = {};
  const time = (label, fn, n) => { const a = performance.now(); for (let i = 0; i < n; i++) fn(i); res[label] = +((performance.now() - a) * 1000 / n).toFixed(2); };
  for (const mode of ['readwrite', 'read-only']) {
    try { h = await fh.createSyncAccessHandle({ mode }); } catch (e) { res[mode] = e.name; continue; }
    const ab = new Uint8Array(4096), sab = new Uint8Array(new SharedArrayBuffer(4096));
    time(`${mode} 16B ArrayBuffer us`, (i) => h.read(ab.subarray(0, 16), { at: (i * 16) % size }), 5000);
    let sabOk = true;
    try { h.read(sab.subarray(0, 16), { at: 0 }); } catch (e) { sabOk = e.name; }
    res[`${mode} SAB accepted`] = sabOk;
    if (sabOk === true) time(`${mode} 16B SAB us`, (i) => h.read(sab.subarray(0, 16), { at: (i * 16) % size }), 5000);
    let x = 7;
    time(`${mode} 4K random us`, () => { x = (x * 1103515245 + 12345) >>> 0; h.read(ab, { at: x % (size - 4096) }); }, 3000);
    time(`${mode} getSize us`, () => h.getSize(), 5000);
    h.close();
  }
  await root.removeEntry('bench.bin');
  self.postMessage(res);
};
