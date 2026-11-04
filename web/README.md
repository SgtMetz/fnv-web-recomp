# web/ — browser runtime shell (Phase 6 groundwork)

The page and workers that will host the wasm build. This folder holds the parts that do not depend on the relink:
importing the player's install into the browser, first-run settings, saves, and the synchronous file layer the
engine threads will call for their Win32 file APIs. No game data or executable-derived bytes live here.
`src/manifest.json` holds only file names and sizes of the reference install, so the importer can check an install.

## Run

```
node web/serve.mjs --port 8650 --install "E:/Games/Fallout New Vegas"
```

- `http://127.0.0.1:8650/` is the first-run page: browser support, folder picker (with a `webkitdirectory` fallback),
  import plan with "lite" choices, progress, saves (import/export/delete), and storage.
  `?dev=1` adds "Use dev install", which reads the `--install` folder over localhost instead of the native picker
  (automation cannot click the picker). `?root=<name>` uses another OPFS root, which keeps tests away from a real import.
- `http://127.0.0.1:8650/test/` runs the automated tests (synthetic install, its own OPFS root, removed afterwards).
  `?real=1` also copies a small slice of the real install (DLC masters, NAMs, Misc/Update BSAs, INIs: 81 MiB) into
  a throwaway root, checks it, and deletes it. Results are in `window.__results`.

The server binds 127.0.0.1 only, sends COOP/COEP (needed for SharedArrayBuffer), and never serves executables: only
`Data/**` and the INIs at the install root.

`python tools/web_manifest.py --install <dir>` regenerates `src/manifest.json`.

## OPFS layout (`src/layout.js`)

| OPFS path | Game sees |
|---|---|
| `fnv/game/` (`Fallout_default.ini`, presets, `Data/`) | `C:\Games\FalloutNV` (working directory) |
| `fnv/docs/My Games/FalloutNV/` (`Fallout.ini`, `FalloutPrefs.ini`, `Saves/`) | `CSIDL_PERSONAL` = `C:\Users\Player\Documents` |
| `fnv/localappdata/FalloutNV/plugins.txt` | `CSIDL_LOCAL_APPDATA` |
| `fnv/meta/import.json`, `times.json`, `import-journal.json` | (runtime only) |

## Pieces

- `src/sources.js`: directory handle, FileList, dev server and in-memory sources; `rebase()` accepts the install
  folder, its `Data` folder, or a folder above it.
- `src/plan.js`: checks a listing against the manifest. Missing required files are errors, a DLC is offered only when
  all its files exist, and size differences are warnings.
- `src/import-worker.js`: streams files into OPFS through sync access handles, reading ahead while writing. It can
  resume from a journal, and checks each BSA's index and each plugin's `TES4` tag once copied.
- `src/config.js`: `Fallout.ini` is a byte copy of `Fallout_default.ini`. `FalloutPrefs.ini` is the chosen preset plus
  windowed display settings. `plugins.txt` is created empty, as in the reference install, because DLC load through
  their `.NAM` files.
- `src/saves.js`: `.fos` header reader (name, level, place, play time, screenshot, plugin list), plus import, export
  and delete. Saves stay byte-identical PC saves.
- `src/vfs-*.js`: synchronous file layer. See below.

## Synchronous file layer

The game's file calls are synchronous, but OPFS opens files asynchronously. Engine threads therefore post requests
into slots of one SharedArrayBuffer and block in `Atomics.wait`. An I/O worker (`vfs-server.js`) holds every OPFS
handle and answers through `Atomics.waitAsync`. It copies bytes straight into the wasm heap; Chrome accepts shared
views in `read()`/`write()`. On top of OPFS it provides Windows behaviour:

- drive-letter paths through mounts, and one process-wide current directory;
- case-insensitive names: the game opens `DeadMoney.NAM` for `DEADMONEY.NAM` (`"%s%s.NAM"` in 0x004624b0);
- `FindFirstFileA` wildcards in NTFS order, with `.` and `..`;
- each file's original last-write time. OPFS cannot set a file's time, but TESDataHandler 0x004624b0 orders
  `Data\*.esm`/`*.esp` by `ftLastWriteTime`, so the times come from `meta/import.json`, and `meta/times.json` holds
  the times of files the game writes;
- Win32 error codes for `SetLastError`, process-wide descriptors and a shared file-pointer table.

Measured in the built-in browser on this machine (Windows):

| Operation | Cost |
|---|---|
| Slot round trip (ping) | 12–16 µs |
| Any OPFS sync-handle call, even `getSize()` | 130–200 µs |
| 16-byte read without the window | 178–193 µs |
| 16-byte read from the window | 0.34–0.39 µs |
| 1 MiB sequential reads | 780–840 MB/s |
| ESM-like pass (24-byte headers plus bodies) | ~200 MB/s, 3.7 µs per read |

Because every OPFS call has that fixed cost, small reads of install files (read-only, under `game/`) come from a
64 KiB read-ahead window kept by each engine thread. Seeks within the file never reach the I/O worker.

## Not done yet

- Hooking it up: the browser `pal_win32` (Phase 2/5) will call `VfsClient` from `CreateFileA`, `ReadFile`,
  `SetFilePointer`, `FindFirstFileA`, `GetFileTime` and `MoveFileExA` through EM_JS. That code does not exist yet.
- A full import has not run in the built-in browser: its quota is 2.6 GiB, and even the minimum import is 4.06 GiB
  (the full one is 8.86 GiB). The page detects this and refuses before copying. Desktop Chrome usually allows far
  more. The native picker path is the same code with a different source; it was not clicked, because automation
  cannot click it.
- Shader translation cache, Bink-to-WebM transcode, and the service worker (Phases 4–5).
