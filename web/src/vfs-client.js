// Synchronous file API for engine threads (workers only: it blocks in Atomics.wait). The browser platform layer's
// CreateFileA/ReadFile/SetFilePointer/FindFirstFileA/GetFileTime/MoveFileExA wrappers call these; errors carry the
// Win32 code for SetLastError. File bytes move between OPFS and `memory` (the wasm heap) without extra copies.
import { layout, OP, ERR, F, S_FREE, S_CLAIMED, S_POSTED, FD_CACHEABLE, FD_OPEN, SEEK } from './vfs-proto.js';

export class VfsError extends Error {
  constructor(op, code, path) {
    super(`${op}${path ? ` ${path}` : ''}: win32 error ${code}`);
    this.win32 = code;
  }
}

const enc = new TextEncoder(), dec = new TextDecoder();

export class VfsClient {
  // window: read-ahead size for small reads of install files (each thread keeps one window per descriptor).
  constructor(shared, memory, { window = 64 << 10 } = {}) {
    this.l = layout(shared);
    this.memory = memory;
    this.window = Math.min(window, this.l.scratchSize);
    this.windows = new Map(); // fd -> { gen, start, len, eof, buf }
    this.calls = 0; this.hits = 0; this.fills = 0;
  }

  get buffer() { return this.memory instanceof SharedArrayBuffer ? this.memory : this.memory.buffer; }

  #claim() {
    const head = this.l.head;
    for (;;) {
      const gen = Atomics.load(head, 3);
      for (const s of this.l.slots) {
        if (Atomics.compareExchange(s.i32, F.STATE, S_FREE, S_CLAIMED) === S_FREE) return s;
      }
      Atomics.wait(head, 3, gen, 50); // every slot busy: wait for a release
    }
  }

  #release(s) {
    Atomics.store(s.i32, F.STATE, S_FREE);
    const head = this.l.head;
    Atomics.add(head, 3, 1);
    Atomics.notify(head, 3, 1);
  }

  // Run one request. `fill(slot)` writes arguments; returns { result, f64 } copied out before the slot is freed.
  #call(op, name, path, fill) {
    const s = this.#claim();
    try {
      s.i32[F.OP] = op;
      if (path !== undefined) {
        const b = enc.encode(path);
        if (b.length > s.scratch.length) throw new VfsError(name, ERR.INVALID_PARAMETER, 'path too long');
        s.scratch.set(b);
        s.i32[F.SCRATCH_LEN] = b.length;
      }
      if (fill) fill(s);
      Atomics.store(s.i32, F.STATE, S_POSTED);
      Atomics.notify(s.i32, F.STATE);
      while (Atomics.load(s.i32, F.STATE) === S_POSTED) Atomics.wait(s.i32, F.STATE, S_POSTED);
      this.calls++;
      const err = s.i32[F.ERROR];
      if (err) throw new VfsError(name, err, path);
      return { result: s.i32[F.RESULT], f64: [s.f64[0], s.f64[1]] };
    } finally {
      this.#release(s);
    }
  }

  #callOut(op, name, path, fill, take) {
    // like #call, but reads the scratch result (text, or take(slot, result) for bytes) before the slot is released
    const s = this.#claim();
    try {
      s.i32[F.OP] = op;
      const b = enc.encode(path ?? '');
      s.scratch.set(b);
      s.i32[F.SCRATCH_LEN] = b.length;
      if (fill) fill(s);
      Atomics.store(s.i32, F.STATE, S_POSTED);
      Atomics.notify(s.i32, F.STATE);
      while (Atomics.load(s.i32, F.STATE) === S_POSTED) Atomics.wait(s.i32, F.STATE, S_POSTED);
      this.calls++;
      const err = s.i32[F.ERROR];
      if (err) throw new VfsError(name, err, path);
      if (take) { take(s, s.i32[F.RESULT]); return { result: s.i32[F.RESULT] }; }
      return { result: s.i32[F.RESULT], text: dec.decode(s.scratch.slice(0, s.i32[F.SCRATCH_LEN])) };
    } finally {
      this.#release(s);
    }
  }

  ping() { return this.#call(OP.PING, 'ping').result; }

  // flags: vfs-proto O.* bits. Returns { fd, size }.
  open(path, flags) {
    const r = this.#call(OP.OPEN, 'open', path, (s) => { s.i32[F.ARG + 1] = flags; });
    return { fd: r.result, size: r.f64[0], cacheable: !!(Atomics.load(this.l.fd.flags, r.result) & FD_CACHEABLE) };
  }

  close(fd) {
    this.windows.delete(fd);
    this.#call(OP.CLOSE, 'close', undefined, (s) => { s.i32[F.ARG] = fd; });
  }

  #fdFlags(fd) { return fd > 0 && fd < this.l.maxFds ? Atomics.load(this.l.fd.flags, fd) : 0; }

  // at < 0: use and advance the descriptor's position (ReadFile); at >= 0: positioned read (OVERLAPPED offset).
  // Small reads of install files come from this thread's read-ahead window; the rest go to the I/O worker.
  read(fd, ptr, len, at = -1) {
    if ((this.#fdFlags(fd) & FD_CACHEABLE) && len <= this.window >> 1) {
      const pos = at < 0 ? this.l.fd.pos[fd] : at;
      const n = this.#windowRead(fd, ptr, len, pos);
      if (at < 0) this.l.fd.pos[fd] = pos + n;
      return n;
    }
    return this.#call(OP.READ, 'read', undefined, (s) => {
      s.i32[F.ARG] = fd; s.i32[F.ARG + 2] = ptr; s.i32[F.ARG + 3] = len; s.f64[0] = at;
    }).result;
  }

  #windowRead(fd, ptr, len, pos) {
    const gen = Atomics.load(this.l.fd.gen, fd);
    let w = this.windows.get(fd);
    const covered = w && w.gen === gen && pos >= w.start && (pos + len <= w.start + w.len || (w.eof && pos <= w.start + w.len));
    if (!covered) {
      if (!w || w.buf.length !== this.window) w = { buf: new Uint8Array(this.window) };
      const want = this.window;
      let got = 0;
      this.#callOut(OP.READ_SCRATCH, 'read', undefined, (s) => {
        s.i32[F.ARG] = fd; s.i32[F.ARG + 3] = want; s.f64[0] = pos;
      }, (s, result) => { got = result; w.buf.set(s.scratch.subarray(0, result)); });
      Object.assign(w, { gen, start: pos, len: got, eof: got < want });
      this.windows.set(fd, w);
      this.fills++;
    } else this.hits++;
    const off = pos - w.start, n = Math.max(0, Math.min(len, w.len - off));
    if (n) new Uint8Array(this.buffer, ptr, n).set(w.buf.subarray(off, off + n));
    return n;
  }

  write(fd, ptr, len, at = -1) {
    return this.#call(OP.WRITE, 'write', undefined, (s) => {
      s.i32[F.ARG] = fd; s.i32[F.ARG + 2] = ptr; s.i32[F.ARG + 3] = len; s.f64[0] = at;
    }).result;
  }

  // SetFilePointerEx. SET and CUR only touch the shared position table; END needs the size from the I/O worker.
  seek(fd, offset, whence) {
    if (whence !== SEEK.END) {
      if (!(this.#fdFlags(fd) & FD_OPEN)) throw new VfsError('seek', ERR.INVALID_HANDLE);
      const pos = (whence === SEEK.SET ? 0 : this.l.fd.pos[fd]) + offset;
      if (pos < 0) throw new VfsError('seek', ERR.NEGATIVE_SEEK);
      this.l.fd.pos[fd] = pos;
      return pos;
    }
    return this.#call(OP.SEEK, 'seek', undefined, (s) => { s.i32[F.ARG] = fd; s.i32[F.ARG + 1] = whence; s.f64[0] = offset; }).f64[0];
  }

  size(fd) { return this.#call(OP.SIZE, 'size', undefined, (s) => { s.i32[F.ARG] = fd; }).f64[0]; }
  truncate(fd, size) { this.#call(OP.TRUNCATE, 'truncate', undefined, (s) => { s.i32[F.ARG] = fd; s.f64[0] = size; }); }
  flush(fd) { this.#call(OP.FLUSH, 'flush', undefined, (s) => { s.i32[F.ARG] = fd; }); }

  // null when missing, else { dir, size, mtime } (mtime in ms since 1970; see winpath.msToFiletime)
  stat(path) {
    const r = this.#call(OP.STAT, 'stat', path);
    return r.result ? { dir: r.result === 2, size: r.f64[0], mtime: r.f64[1] } : null;
  }

  // FindFirstFileA pattern ("Data\\*.esm"). Returns [{ name, dir, size, mtime }] in NTFS order.
  list(pattern) {
    const r = this.#callOut(OP.LIST, 'list', pattern);
    return JSON.parse(r.text).map(([name, dir, size, mtime]) => ({ name, dir: !!dir, size, mtime }));
  }

  mkdir(path) { this.#call(OP.MKDIR, 'mkdir', path); }
  unlink(path) { this.#call(OP.UNLINK, 'unlink', path); }
  rmdir(path) { this.#call(OP.RMDIR, 'rmdir', path); }
  rename(from, to, replace = false) {
    this.#call(OP.RENAME, 'rename', `${from}\0${to}`, (s) => { s.i32[F.ARG + 1] = replace ? 1 : 0; });
  }
  settime(path, ms) { this.#call(OP.SETTIME, 'settime', path, (s) => { s.f64[0] = ms; }); }
  chdir(path) { this.#call(OP.CHDIR, 'chdir', path); }
  getcwd() { return this.#callOut(OP.GETCWD, 'getcwd', '').text; }
}
