# Serving the browser build

`server/serve.mjs` is the one web server for the recompiled game, both on this machine (`--dev`) and on a server
(`--dist`). It has no dependencies beyond Node 18 or newer.

## Read this first: what a deployment contains

Everything the browser loads comes from the player's own copy of Fallout: New Vegas:
- the 100 MB wasm is the recompiled `FalloutNV.exe`;
- the shaders are the game's own shaders, translated;
- the game files are the install itself.

**A deployment must not be publicly reachable.** There is no access control yet (by decision, 2026-11-08), so restrict
it by one of:
- a firewall or VPN (WireGuard, Tailscale);
- an authenticating reverse proxy (HTTP basic auth, an OAuth proxy) in front of it.

The server prints a warning whenever it listens beyond localhost.

## Build the bundle (on the build machine)

```bash
python -B tools/recomp.py --exe "E:/Games/Fallout New Vegas/FalloutNV.exe" --shards 64   # only after recompiler changes
python -B tools/recomp_build_web.py --target browser
python -B tools/recomp_bundle.py                       # -> out/dist (about 30 s)
```

`out/dist/` holds:
- the page;
- `build/` (wasm and loader);
- `shader/<sha1>`: one JSON per translated shader, 1,809 of them;
- `.br`/`.gz` siblings of each large file (the wasm is 11.9 MB as brotli);
- `VERSION.json`: the commit, build time and wasm SHA-256.

It is derived data, so it is git-ignored. Copy it to the server with rsync or scp, never through git.

## Run it (on the server)

The server needs:
- Node 18 or newer;
- the bundle;
- a copy of the game install (read-only to the server; only the `FalloutNV.exe` and `Data/` contents are read);
- a writable data folder for settings and saves.

```bash
node server/serve.mjs --dist /srv/fnv/dist --game "/srv/fnv/Fallout New Vegas" --data /srv/fnv/data \
    --host 127.0.0.1 --port 8093
```

| Option | Meaning |
| --- | --- |
| `--dist <dir>` | the bundle |
| `--game <dir>` | the install. Paths are matched case-insensitively, like on Windows, so a Linux file system is fine. |
| `--data <dir>` | the private `My Games\FalloutNV` copy: INIs, `Saves/`, and `FalloutNV/plugins.txt`. It is created if missing, and the game writes its INIs on first start. Back it up: it holds the saves. |
| `--host`, `--port` | bind address, by default `127.0.0.1:8093`. Keep the loopback address behind a reverse proxy. |
| `--tls-cert`, `--tls-key` | serve HTTPS directly (PEM files) |
| `--dlc` | load the DLC. It is off by default because the game enables any plugin with a `Data\<name>.NAM` file. |
| `--delay <ms>` | test only: add latency to every file request |

The server also needs:
- **HTTPS:** the game needs `SharedArrayBuffer`, which browsers allow only on cross-origin-isolated pages served
  over `https://` (or from `localhost`). The server sends the COOP/COEP/CORP headers; HTTPS comes from `--tls-*` or a
  proxy.
- **A WebGPU browser** for the players: Chrome or Edge 113 or newer on desktop.
- **Resources:** the server itself is light (static files plus range reads). The player's machine runs the whole game:
  3 GB of wasm memory, 64 threads, and WebGPU.

### Reverse proxy example (Caddy, HTTPS and a password)

```
fnv.example.net {
    basicauth {
        player <bcrypt hash from `caddy hash-password`>
    }
    reverse_proxy 127.0.0.1:8093
}
```

Caddy passes the cross-origin headers through unchanged. Make sure any proxy does too, and that it does not
recompress (or strip `Content-Encoding` from) responses that are already compressed.

### Keep it running (systemd)

```ini
[Unit]
Description=FNV recompiled (browser build)
After=network.target

[Service]
ExecStart=/usr/bin/node /srv/fnv/server/serve.mjs --dist /srv/fnv/dist --game "/srv/fnv/Fallout New Vegas" --data /srv/fnv/data
User=fnv
Restart=on-failure

[Install]
WantedBy=multi-user.target
```

## How the game talks to the server

The game reads files with synchronous requests from its own threads (`port/recomp/pal_fs_web.cpp`). Everything else
is static.

| Route | What |
| --- | --- |
| `/`, `/build/*` | the page and the build. They revalidate with an ETag (the names are not content-hashed); `304`s are cheap. |
| `/shader/<sha1>` | a translated shader, cached as immutable (it is named by its bytecode hash) |
| `/fs/index` | an index of the install, fetched once per game thread. `stat` and `list` under `/game` are answered from it in the browser. |
| `/fs/read?p&o&n&v` | a 1 MB block. Install reads carry the file's mtime (`v`), so the browser's HTTP cache keeps them across sessions. |
| `/fs/stat`, `/fs/list` | `/docs` only, in practice (the install is answered from the index) |
| `/fs/write`, `truncate`, `mkdir`, `remove`, `rename` | POST, `/docs` only. Writes to `/game` get `403`; paths outside the roots get `404`. |

Measured on 2026-11-08 with quick start (`?start=quick`), to the porch:

| | Requests | Time |
| --- | --- | --- |
| Before the index and the INI cache | about 7,000 | |
| Now | about 1,300, nearly all 1 MB reads | |
| Locally | | 39 s |
| With `--delay 40` | | 99 s (the main menu at 54 s) |

A first session reads about 950 distinct MB of the archives, so on a real link bandwidth dominates. Later sessions read
from the browser's HTTP cache, provided it is large enough.

Two knobs to tune:
- the block size `B` in `js_fs_read` (`pal_fs_web.cpp`);
- the per-thread cache of 96 MB.

## Known limits (not done yet)

- No access control (see above).
- One data folder for everyone: every visitor shares the same settings and saves. Per-user data would need accounts
  or the browser-side storage in `web/` (OPFS).
- Every game thread fetches its own copy of the index (about 42 KB) and keeps its own block cache. Concurrent
  threads sometimes fetch the same block twice: about a quarter of the reads in the measurement above.
- `debug.js` ships in the bundle, but its screenshot endpoint (`/snap`) exists only with `--dev`.

## Testing locally

The launch configurations in `.claude/launch.json` (each one is `node server/serve.mjs` with these options):

| Name | Options | Port |
| --- | --- | --- |
| `recomp` | `--dev` | 8093 |
| `recomp-remote` | `--dev --delay 40` | 8094 |
| `recomp-dist` | `--dist out/dist` | 8095 |
