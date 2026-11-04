// Stands in for an engine thread: drives the synchronous VFS client the way the platform layer's Win32 wrappers
// will (CreateFileA/ReadFile/FindFirstFileA/MoveFileExA/DeleteFileA), against files the page imported.
// in:  { shared, memory, expect: { 'Data/x': { size, lastModified, head: number[] } }, plugins: [...], perf, name }
// out: { type: 'result', checks: [{ name, ok, detail }], perf }
import { VfsClient } from '../src/vfs-client.js';
import { O, SEEK, ERR } from '../src/vfs-proto.js';

self.onmessage = (ev) => {
  const out = { type: 'result', checks: [], perf: {} };
  try { run(ev.data, out); } catch (e) { out.checks.push({ name: 'uncaught', ok: false, detail: `${e.message}\n${e.stack}` }); }
  self.postMessage(out);
};

function run({ shared, memory, expect, esmOrder, perf, name, bigFile, base = 0 }, out) {
  const vfs = new VfsClient(shared, memory);
  // each thread works in its own part of the shared heap, as engine threads use their own buffers
  const heap = new Uint8Array(memory instanceof SharedArrayBuffer ? memory : memory.buffer, base);
  const io = { read: (fd, p, n, at) => vfs.read(fd, base + p, n, at), write: (fd, p, n, at) => vfs.write(fd, base + p, n, at) };
  const check = (label, ok, detail = '') => out.checks.push({ name: `${name}: ${label}`, ok: !!ok, detail: String(detail) });
  const code = (fn) => { try { fn(); return 0; } catch (e) { return e.win32 ?? -1; } };

  check('ping', vfs.ping() === 1);
  check('initial directory is the install', vfs.getcwd() === 'C:\\Games\\FalloutNV', vfs.getcwd());

  // FindFirstFileA("Data\\*.esm"): NTFS order and the install's last-write times, not the copy times
  const esms = vfs.list('Data\\*.esm');
  check('Data\\*.esm lists masters in NTFS order', JSON.stringify(esms.map((e) => e.name)) === JSON.stringify(esmOrder),
        esms.map((e) => e.name).join(', '));
  const badTimes = esms.filter((e) => expect['Data/' + e.name] && expect['Data/' + e.name].lastModified !== e.mtime);
  check('listed last-write times are the install\'s', badTimes.length === 0 && esms.length > 0,
        badTimes.map((e) => `${e.name}: ${e.mtime} != ${expect['Data/' + e.name].lastModified}`).join('; '));
  const all = vfs.list('C:\\Games\\FalloutNV\\Data\\*');
  check('"*" lists . and .. first', all[0]?.name === '.' && all[1]?.name === '..');
  check('wildcards are case-insensitive', vfs.list('data\\*.ESM').length === esms.length);

  // case-insensitive open, as the game does for "%s%s.NAM"
  const nam = vfs.open('C:\\GAMES\\falloutnv\\DATA\\DeadMoney.NAM', O.READ);
  io.read(nam.fd, 0, nam.size);
  check('mixed-case path opens DEADMONEY.NAM', new TextDecoder().decode(heap.slice(0, nam.size)) === 'Dead Money');
  vfs.close(nam.fd);

  // every expected file: size and first bytes, through relative paths with backslashes
  for (const [path, e] of Object.entries(expect)) {
    const st = vfs.stat(path.replace(/\//g, '\\'));
    if (!st || st.size !== e.size || st.mtime !== e.lastModified) { check(`stat ${path}`, false, JSON.stringify(st)); continue; }
    const f = vfs.open(path.replace(/\//g, '\\'), O.READ);
    const n = Math.min(e.head.length, f.size);
    const got = io.read(f.fd, 4096, n, 0);
    const same = got === n && e.head.every((b, i) => heap[4096 + i] === b);
    vfs.close(f.fd);
    if (!same) check(`content ${path}`, false, `read ${got} of ${n}`);
  }
  check(`stat + content of ${Object.keys(expect).length} imported files`, !out.checks.some((c) => !c.ok && /stat |content /.test(c.name)));

  // seek and sequential reads
  const big = vfs.open(bigFile.path, O.READ);
  check('open reports size', big.size === bigFile.size, big.size);
  check('seek end', vfs.seek(big.fd, 0, SEEK.END) === bigFile.size);
  vfs.seek(big.fd, 1000, SEEK.SET);
  io.read(big.fd, 0, 16);
  check('sequential read advances position', vfs.seek(big.fd, 0, SEEK.CUR) === 1016);
  check('read at end returns 0', io.read(big.fd, 0, 16, bigFile.size) === 0);
  check('negative seek fails with ERROR_NEGATIVE_SEEK', code(() => vfs.seek(big.fd, -5, SEEK.SET)) === ERR.NEGATIVE_SEEK);
  const iniFd = vfs.open('C:\\Users\\Player\\Documents\\My Games\\FalloutNV\\Fallout.ini', O.READ);
  check('install files are cacheable, others are not', big.cacheable && !iniFd.cacheable);
  vfs.close(iniFd.fd);

  // read-ahead window: many small reads (random and sequential) give the same bytes as one large read
  const REF = 1 << 20, REG = 512 << 10;
  const fills0 = vfs.fills;
  check('large read bypasses the window', io.read(big.fd, REF, REG, 0) === REG && vfs.fills === fills0);
  let mism = 0, x0 = 99;
  for (let i = 0; i < 3000 && !mism; i++) {
    x0 = (x0 * 1103515245 + 12345) >>> 0;
    const at = x0 % (REG - 3000), n = 1 + (x0 >>> 20) % 2000;
    io.read(big.fd, 8192, n, at);
    for (let k = 0; k < n; k++) if (heap[8192 + k] !== heap[REF + at + k]) { mism = at; break; }
  }
  check('random small reads match a large read', !mism, `first mismatch near ${mism}`);
  vfs.seek(big.fd, 0, SEEK.SET);
  let p = 0;
  while (p < REG - 4096 && !mism) {
    const n = 24 + (p % 977);
    const got = io.read(big.fd, 8192, n);
    for (let k = 0; k < got; k++) if (heap[8192 + k] !== heap[REF + p + k]) { mism = p + 1; break; }
    p += got;
  }
  check('sequential small reads match and advance the shared position', !mism && vfs.seek(big.fd, 0, SEEK.CUR) === p, `${mism} ${p}`);
  const tail = io.read(big.fd, 8192, 1000, bigFile.size - 300);
  check('window read across the end of file is short', tail === 300 && io.read(big.fd, 8192, 10, bigFile.size) === 0, tail);

  // a save written the way the game writes one: temp file, then rename over the old name
  const saves = 'C:\\Users\\Player\\Documents\\My Games\\FalloutNV\\Saves';
  const tmp = `${saves}\\${name}-save.fos.tmp`, fin = `${saves}\\${name} Save 1.fos`;
  const w = vfs.open(tmp, O.READ | O.WRITE | O.CREATE | O.TRUNC);
  for (let i = 0; i < 200000; i++) heap[i] = (i * 31 + 7) & 255;
  let wrote = 0;
  for (let o = 0; o < 200000; o += 65536) wrote += io.write(w.fd, o, Math.min(65536, 200000 - o));
  check('write 200000 bytes', wrote === 200000, wrote);
  check('unlink of an open file fails with ERROR_SHARING_VIOLATION', code(() => vfs.unlink(tmp)) === ERR.SHARING_VIOLATION);
  vfs.close(w.fd);
  const st = vfs.stat(tmp);
  check('written file: size and a fresh time', st && st.size === 200000 && Math.abs(Date.now() - st.mtime) < 60000, JSON.stringify(st));
  vfs.rename(tmp, fin, true);
  check('rename: old name gone', vfs.stat(tmp) === null);
  const listed = vfs.list(`${saves}\\*.fos`).map((e) => e.name);
  check('rename: new name listed', listed.includes(`${name} Save 1.fos`), listed.join(', '));
  const r = vfs.open(fin.toUpperCase(), O.READ);
  heap.fill(0, 300000, 500000);
  io.read(r.fd, 300000, 200000, 0);
  let same = true;
  for (let i = 0; i < 200000; i++) if (heap[300000 + i] !== ((i * 31 + 7) & 255)) { same = false; break; }
  check('read back what was written', same);
  vfs.close(r.fd);
  const w2 = vfs.open(tmp, O.WRITE | O.CREATE | O.TRUNC); vfs.close(w2.fd);
  check('rename onto an existing name without replace fails (ERROR_ALREADY_EXISTS)', code(() => vfs.rename(tmp, fin)) === ERR.ALREADY_EXISTS);
  vfs.unlink(tmp); vfs.unlink(fin);
  check('unlink', vfs.stat(fin) === null);

  // error codes the platform layer passes to SetLastError
  check('missing file: ERROR_FILE_NOT_FOUND', code(() => vfs.open('Data\\nope.esm', O.READ)) === ERR.FILE_NOT_FOUND);
  check('missing folder: ERROR_PATH_NOT_FOUND', code(() => vfs.open('Data\\nope\\x.dds', O.READ)) === ERR.PATH_NOT_FOUND);
  check('path outside every mount: ERROR_PATH_NOT_FOUND', code(() => vfs.open('D:\\elsewhere\\x', O.READ)) === ERR.PATH_NOT_FOUND);
  check('opening a folder: ERROR_ACCESS_DENIED', code(() => vfs.open('Data', O.READ)) === ERR.ACCESS_DENIED);
  check('create exclusive on existing: ERROR_FILE_EXISTS', code(() => vfs.open('Data\\FalloutNV.esm', O.WRITE | O.CREATE | O.EXCL)) === ERR.FILE_EXISTS);
  check('bad descriptor: ERROR_INVALID_HANDLE', code(() => io.read(99999, 0, 4)) === ERR.INVALID_HANDLE);
  const mk = `${saves}\\${name}-dir`;
  vfs.mkdir(mk);
  check('mkdir twice: ERROR_ALREADY_EXISTS', code(() => vfs.mkdir(mk)) === ERR.ALREADY_EXISTS);
  vfs.rmdir(mk);
  if (name === 't1') { // the current directory is process-wide, so only the solo run changes it
    vfs.chdir('Data');
    check('chdir + relative open', vfs.stat('FalloutNV.esm')?.size === expect['Data/FalloutNV.esm'].size);
    vfs.chdir('C:\\Games\\FalloutNV');
    check('chdir back', vfs.getcwd() === 'C:\\Games\\FalloutNV');
  }

  if (perf) {
    const t = (fn, n) => { const a = performance.now(); for (let i = 0; i < n; i++) fn(i); return (performance.now() - a) * 1000 / n; };
    out.perf.pingUs = t(() => vfs.ping(), 5000);
    const size = bigFile.size, MB = 1 << 20;
    const a = performance.now();
    let bytes = 0;
    for (let o = 0; o < size; o += MB) bytes += io.read(big.fd, 1 << 20, Math.min(MB, size - o), o);
    out.perf.seqMBps = bytes / MB / ((performance.now() - a) / 1000);
    let x = 12345;
    out.perf.rand4kUs = t(() => { x = (x * 1103515245 + 12345) >>> 0; io.read(big.fd, 1 << 20, 4096, x % (size - 4096)); }, 3000);
    out.perf.small16Us = t((i) => io.read(big.fd, 1 << 20, 16, (i * 16) % size), 5000);
    // an ESM-like pass: 24-byte record headers, then bodies of varying size, front to back
    vfs.seek(big.fd, 0, SEEK.SET);
    const e0 = performance.now();
    let ep = 0, reads = 0;
    while (ep < (16 << 20)) { const n = (reads & 1) ? 24 : 40 + ((reads * 7919) % 3000); ep += io.read(big.fd, 1 << 20, n); reads++; }
    const ems = performance.now() - e0;
    out.perf.esmPassMBps = ep / MB / (ems / 1000);
    out.perf.esmPassUsPerRead = ems * 1000 / reads;
    // the same small reads on a file the window does not cover (docs folder)
    const docs = 'C:\\Users\\Player\\Documents\\My Games\\FalloutNV\\bench.bin';
    const wf = vfs.open(docs, O.READ | O.WRITE | O.CREATE | O.TRUNC);
    io.write(wf.fd, 1 << 20, 1 << 20, 0); vfs.close(wf.fd);
    const rf = vfs.open(docs, O.READ);
    out.perf.uncached16Us = t((i) => io.read(rf.fd, 1 << 20, 16, (i * 16) % (1 << 20)), 2000);
    vfs.close(rf.fd); vfs.unlink(docs);
    const sa = performance.now();
    for (let i = 0; i < 1000; i++) vfs.stat('Data\\Fallout - Meshes.bsa');
    out.perf.statUs = (performance.now() - sa);
    out.perf.calls = vfs.calls; out.perf.windowHits = vfs.hits; out.perf.windowFills = vfs.fills;
  }
  vfs.close(big.fd);
}
