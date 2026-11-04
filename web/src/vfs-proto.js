// Shared-memory protocol between engine threads (synchronous callers) and the I/O worker (vfs-server.js).
//
// Why: the game calls CreateFileA/ReadFile/FindFirstFileA and expects an answer before it continues, but OPFS only
// opens files asynchronously (getFileHandle, createSyncAccessHandle return promises). So engine threads post a
// request into a slot of one SharedArrayBuffer and block in Atomics.wait; the I/O worker, which holds every OPFS
// handle, answers with Atomics.waitAsync/notify and copies file bytes straight into the engine's memory.
//
// One SharedArrayBuffer holds a header, a descriptor table and SLOTS request slots. Any thread may use any free
// slot (claimed with a compare-exchange), so threads need no registration. Descriptors and the current directory
// are process-wide, as Windows handles and the Win32 current directory are.
//
// Descriptor table (MAX_FDS entries): f64 position (the Win32 file pointer, shared by every thread), i32 generation
// (bumped when the number is reused, so per-thread read caches can tell), i32 flags (FD_OPEN; FD_CACHEABLE: opened read-only
// under the install folder, whose files the game never writes; engine threads may serve small reads of it from
// their own read-ahead window instead of a round trip).
//
// Measured on the dev machine (built-in browser, Windows): an OPFS sync-handle call costs 130-200 us whatever its
// size (even getSize), and the slot round trip about 16 us. Small reads are therefore batched by the read-ahead
// window, and seeks inside the file are done by the caller without the I/O worker.
//
// Slot layout (bytes):        0  i32 state      (0 free, 1 claimed, 2 request posted, 3 answered)
//                             4  i32 op
//                             8  i32 args[8]    fd, flags, ptr, len, ...
//                            40  i32 result
//                            44  i32 error      Win32 error code (GetLastError value), 0 on success
//                            48  f64 nums[4]    64-bit offsets, sizes, times (exact below 2^53)
//                            80  i32 scratchLen
//                            96  scratch[SCRATCH] UTF-8 paths in, small results out (stat, listings, read-ahead)

export const MAGIC = 0x56464e47; // "GNFV", v2: descriptor table
export const HEADER = 64;
export const SLOT_HEADER = 96;
export const DEFAULT_SLOTS = 16;
export const DEFAULT_SCRATCH = 256 << 10;
export const DEFAULT_MAX_FDS = 4096;
export const FD_CACHEABLE = 1, FD_OPEN = 2;

export const S_FREE = 0, S_CLAIMED = 1, S_POSTED = 2, S_ANSWERED = 3;

export const OP = {
  OPEN: 1, CLOSE: 2, READ: 3, WRITE: 4, SEEK: 5, SIZE: 6, TRUNCATE: 7, FLUSH: 8,
  STAT: 9, LIST: 10, MKDIR: 11, UNLINK: 12, RMDIR: 13, RENAME: 14, SETTIME: 15, CHDIR: 16, GETCWD: 17,
  PING: 18, READ_SCRATCH: 19,
};

// OPEN flags. The platform layer maps CreateFileA's access and disposition onto these.
export const O = { READ: 1, WRITE: 2, CREATE: 4, TRUNC: 8, EXCL: 16 };
export const SEEK = { SET: 0, CUR: 1, END: 2 };

// Win32 error codes, so the platform layer can hand them to SetLastError unchanged.
export const ERR = {
  FILE_NOT_FOUND: 2, PATH_NOT_FOUND: 3, ACCESS_DENIED: 5, INVALID_HANDLE: 6, SHARING_VIOLATION: 32,
  FILE_EXISTS: 80, INVALID_PARAMETER: 87, DISK_FULL: 112, INSUFFICIENT_BUFFER: 122, DIR_NOT_EMPTY: 145,
  ALREADY_EXISTS: 183, NO_MORE_FILES: 18, GEN_FAILURE: 31, NEGATIVE_SEEK: 131, TOO_MANY_OPEN_FILES: 4,
};

const align = (n) => (n + 63) & ~63;

export function layout(sab) {
  const head = new Int32Array(sab, 0, HEADER / 4);
  if (head[0] !== MAGIC) throw new Error('not a VFS shared buffer (or an older layout)');
  const slots = head[1], scratch = head[2], maxFds = head[4], stride = SLOT_HEADER + scratch;
  const fdBase = HEADER;
  const pos = new Float64Array(sab, fdBase, maxFds);
  const gen = new Int32Array(sab, fdBase + maxFds * 8, maxFds);
  const flags = new Int32Array(sab, fdBase + maxFds * 12, maxFds);
  const slotBase = fdBase + align(maxFds * 16);
  const out = [];
  for (let i = 0; i < slots; i++) {
    const base = slotBase + i * stride;
    out.push({
      index: i,
      i32: new Int32Array(sab, base, 24),
      f64: new Float64Array(sab, base + 48, 4),
      scratch: new Uint8Array(sab, base + SLOT_HEADER, scratch),
    });
  }
  return { head, slots: out, scratchSize: scratch, maxFds, fd: { pos, gen, flags } };
}

export function createShared({ slots = DEFAULT_SLOTS, scratch = DEFAULT_SCRATCH, maxFds = DEFAULT_MAX_FDS } = {}) {
  const sab = new SharedArrayBuffer(HEADER + align(maxFds * 16) + slots * (SLOT_HEADER + scratch));
  const head = new Int32Array(sab, 0, HEADER / 4);
  head[0] = MAGIC; head[1] = slots; head[2] = scratch; head[4] = maxFds;
  return sab;
}

// Slot field indexes (Int32 units). Header: [3] slot-release counter.
export const F = { STATE: 0, OP: 1, ARG: 2, RESULT: 10, ERROR: 11, SCRATCH_LEN: 20 };
