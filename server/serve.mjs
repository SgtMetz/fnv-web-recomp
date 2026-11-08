#!/usr/bin/env node
// The web server for the browser build of the recompiled game (docs/RECOMP.md, server/README.md). One program for
// both uses, so the game's file API never differs between them:
//
//   development (this machine)   node server/serve.mjs --dev
//     the page from web/recomp/, the build from out/recomp/web_browser/, shaders translated on request from out/,
//     plus the debugging endpoints (/snap screenshots, /ring.bin captures)
//   deployment (a server)        node server/serve.mjs --dist out/dist --game <install> --data <saves dir> ...
//     everything static from a bundle made by tools/recomp_bundle.py (precompressed, cache headers)
//
// Routes:
//   /                       the page (index.html), /build/ the Emscripten build, /shader/<sha1> translated WGSL
//   /fs/index               one index of the install (paths lowercased -> size, mtime; folder listings), so the
//                           browser answers stat/list itself instead of making thousands of requests
//   /fs/stat|list|read      GET: the game's file layer (port/recomp/pal_fs_web.cpp). Two roots: /game (the install,
//                           read-only, case-insensitive like Windows) and /docs (the private My Games copy, writable)
//   /fs/write|truncate|mkdir|remove|rename   POST, /docs only
//
// Options:
//   --dev                  development mode (above)
//   --dist <dir>           bundle to serve (deployment)
//   --game <dir>           the game install (default E:/Games/Fallout New Vegas, or FNV_GAME)
//   --data <dir>           the writable My Games copy (default out/fnv_docs)
//   --host <addr>          bind address (default 127.0.0.1; 0.0.0.0 exposes the server: see server/README.md)
//   --port <n>             port (default 8093)
//   --tls-cert/--tls-key   serve HTTPS directly (otherwise put a TLS reverse proxy in front for remote use)
//   --dlc                  load the DLC (FNV_DLC=1 too). Off by default: the game enables any plugin whose
//                          Data\<name>.NAM exists, so the server answers as if no .nam file existed
//   --delay <ms>           add latency to every /fs request (to test how the game behaves on a remote server)
//
// There is no access control yet. Every file the browser loads is derived from the player's own copy of the game,
// so a deployment must not be public (server/README.md).
import http from 'node:http';
import https from 'node:https';
import fs from 'node:fs';
import fsp from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const args = process.argv.slice(2);
const opt = (name, def) => { const i = args.indexOf(name); return i >= 0 ? args[i + 1] : def; };
const DEV = args.includes('--dev');
const DIST = opt('--dist') ? path.resolve(opt('--dist')) : null;
const GAME = path.resolve(opt('--game', process.env.FNV_GAME || 'E:/Games/Fallout New Vegas'));
const DATA = path.resolve(opt('--data', path.join(ROOT, 'out/fnv_docs')));
const HOST = opt('--host', '127.0.0.1');
const PORT = +opt('--port', 8093);
const DLC = args.includes('--dlc') || process.env.FNV_DLC === '1';
const DELAY = +opt('--delay', 0);
const TLS = opt('--tls-cert') && opt('--tls-key') ? { cert: fs.readFileSync(opt('--tls-cert')), key: fs.readFileSync(opt('--tls-key')) } : null;
if (!DEV && !DIST) {
  console.error('usage: node server/serve.mjs --dev | --dist <bundle> [--game <install>] [--data <dir>] [--host a] [--port n]');
  process.exit(2);
}
if (!fs.existsSync(path.join(GAME, 'FalloutNV.exe'))) { console.error(`no FalloutNV.exe in ${GAME} (--game)`); process.exit(2); }
fs.mkdirSync(DATA, { recursive: true });

const TYPES = { '.html': 'text/html; charset=utf-8', '.js': 'text/javascript', '.mjs': 'text/javascript', '.wasm': 'application/wasm',
  '.css': 'text/css', '.json': 'application/json', '.bin': 'application/octet-stream', '.png': 'image/png' };

// ---- the install: an index, and case-insensitive lookup ---------------------------------------------------------
// The game was written for Windows and asks for paths in any case; servers usually have case-sensitive file systems.
// Index keys are lowercased game paths ("/game/data/falloutnv.esm").
const hiddenName = (name) => !DLC && /\.nam$/i.test(name);           // a DLC marker the game must not see
const gameFiles = new Map();    // key -> {real, size, mtime}
const gameDirs = new Map();     // key -> [names as on disk]
function indexGame(dir, key) {
  const names = [];
  for (const e of fs.readdirSync(dir, { withFileTypes: true })) {
    if (e.isFile() && hiddenName(e.name)) continue;
    names.push(e.name);
    const real = path.join(dir, e.name), k = key + '/' + e.name.toLowerCase();
    if (e.isDirectory()) indexGame(real, k);
    else if (e.isFile()) { const st = fs.statSync(real); gameFiles.set(k, { real, size: st.size, mtime: Math.floor(st.mtimeMs / 1000) }); }
  }
  gameDirs.set(key, names);
}
indexGame(GAME, '/game');
const gameIndexJson = JSON.stringify({
  f: Object.fromEntries([...gameFiles].map(([k, v]) => [k, [v.size, v.mtime]])),
  d: Object.fromEntries(gameDirs),
});

// /docs: case-insensitive too, resolved segment by segment (it changes while the game runs, so no index)
function resolveDocs(rel, forCreate) {
  let cur = DATA;
  const parts = rel.split('/').filter(Boolean);
  for (let i = 0; i < parts.length; i++) {
    const part = parts[i];
    if (part === '..' || part === '.') return null;
    let next = path.join(cur, part);
    if (!fs.existsSync(next)) {
      let match = null;
      try { match = fs.readdirSync(cur).find((n) => n.toLowerCase() === part.toLowerCase()); } catch {}
      if (match) next = path.join(cur, match);
      else if (!forCreate) return null;
    }
    cur = next;
  }
  return cur;
}

// a game path -> {root: 'game'|'docs', host path or null, the game index entry}
function locate(p, forCreate) {
  const m = /^\/(game|docs)(\/.*)?$/i.exec(p || '');
  if (!m) return null;
  const rest = m[2] || '/';
  if (m[1].toLowerCase() === 'game') {
    const key = ('/game' + rest).toLowerCase().replace(/\/+$/, '') || '/game';
    const f = gameFiles.get(key);
    return { root: 'game', key, file: f, dir: gameDirs.get(key), real: f ? f.real : null };
  }
  return { root: 'docs', real: resolveDocs(rest, forCreate) };
}

function body(req, limit) {
  return new Promise((ok, fail) => {
    const parts = []; let n = 0;
    req.on('data', (c) => { n += c.length; if (n > limit) { req.destroy(); fail(new Error('body too large')); } else parts.push(c); });
    req.on('end', () => ok(Buffer.concat(parts))); req.on('error', fail);
  });
}

// ---- the game's file API ------------------------------------------------------------------------------------------
const seen = new Set();
const rate = { n: 0, bytes: 0, t: Date.now(), ops: {} };
const readBlocks = new Set();     // distinct (file, offset) reads since start: repeats mean the browser cache missed
setInterval(() => {
  if (!rate.n) return;
  console.log(`[fs] ${rate.n} requests, ${(rate.bytes / 1e6).toFixed(1)} MB in ${((Date.now() - rate.t) / 1000).toFixed(0)} s: ${JSON.stringify(rate.ops)}; ${readBlocks.size} distinct read blocks so far`);
  rate.n = 0; rate.bytes = 0; rate.t = Date.now(); rate.ops = {};
}, 10000).unref();

async function fsApi(req, res, op, q) {
  rate.n++; rate.ops[op] = (rate.ops[op] || 0) + 1;
  if (DELAY) await new Promise((r) => setTimeout(r, DELAY));
  if (op === 'index') {
    res.writeHead(200, { 'content-type': 'application/json', 'cache-control': 'no-cache' }).end(gameIndexJson);
    return;
  }
  if (op === 'read') { rate.bytes += +q.get('n') || 0; readBlocks.add(q.get('p') + '@' + q.get('o')); }
  if (DEV) { const key = op + ' ' + q.get('p'); if (!seen.has(key)) { seen.add(key); console.log('[fs] ' + key); } }
  const write = req.method === 'POST';
  const loc = locate(q.get('p'), write);
  if (!loc || (write && loc.root !== 'docs')) { res.writeHead(403).end(); return; }
  try {
    if (op === 'stat') {
      if (loc.root === 'game') {
        if (loc.file) return res.writeHead(200, { 'content-type': 'application/json' }).end(JSON.stringify({ dir: false, size: loc.file.size, mtime: loc.file.mtime }));
        if (loc.dir) return res.writeHead(200, { 'content-type': 'application/json' }).end(JSON.stringify({ dir: true, size: 0, mtime: 0 }));
        return res.writeHead(404).end();
      }
      if (!loc.real) return res.writeHead(404).end();
      const st = await fsp.stat(loc.real);
      res.writeHead(200, { 'content-type': 'application/json' }).end(JSON.stringify({ dir: st.isDirectory(), size: st.size, mtime: Math.floor(st.mtimeMs / 1000) }));
    } else if (op === 'list') {
      const names = loc.root === 'game' ? loc.dir : loc.real && await fsp.readdir(loc.real);   // before writeHead: missing -> 404
      if (!names) return res.writeHead(404).end();
      res.writeHead(200, { 'content-type': 'application/json' }).end(JSON.stringify(names));
    } else if (op === 'read') {
      if (!loc.real) return res.writeHead(404).end();
      const o = +q.get('o') || 0, n = Math.min(+q.get('n') || 0, 64 << 20);
      const fh = await fsp.open(loc.real, 'r');
      const buf = Buffer.alloc(n);
      const { bytesRead } = await fh.read(buf, 0, n, o);
      await fh.close();
      // install files never change while the server runs; a read that names the file's mtime (v=) can be cached
      const cache = loc.root === 'game' && q.get('v') ? 'public, max-age=31536000, immutable' : 'no-store';
      res.writeHead(200, { 'content-type': 'application/octet-stream', 'cache-control': cache }).end(buf.subarray(0, bytesRead));
    } else if (op === 'write' && write) {
      const data = await body(req, 64 << 20);
      const fh = await fsp.open(loc.real, fs.existsSync(loc.real) ? 'r+' : 'w+');
      await fh.write(data, 0, data.length, +q.get('o') || 0);
      await fh.close();
      res.writeHead(200).end();
    } else if (op === 'truncate' && write) {
      await fsp.mkdir(path.dirname(loc.real), { recursive: true });
      await fsp.writeFile(loc.real, Buffer.alloc(0));
      res.writeHead(200).end();
    } else if (op === 'mkdir' && write) {
      await fsp.mkdir(loc.real);
      res.writeHead(200).end();
    } else if (op === 'remove' && write) {
      if (!loc.real) return res.writeHead(404).end();
      await fsp.rm(loc.real);
      res.writeHead(200).end();
    } else if (op === 'rename' && write) {
      const to = locate(q.get('q'), true);
      if (!to || to.root !== 'docs' || !loc.real || !to.real) { res.writeHead(403).end(); return; }
      await fsp.rename(loc.real, to.real);
      res.writeHead(200).end();
    } else res.writeHead(400).end();
  } catch (e) {
    if (!res.headersSent) res.writeHead(404).end(String(e.code || e.message || e));
    else res.destroy();
  }
}

// ---- static files ---------------------------------------------------------------------------------------------------
// The bundle's build/ files are not content-hashed, so they revalidate (ETag, 304) instead of a long max-age; a
// shader is named by its bytecode's SHA-1 and never changes. Precompressed .br/.gz siblings are sent when accepted.
async function serveFile(req, res, file, { type, cache = 'no-cache' } = {}) {
  let st;
  try { st = await fsp.stat(file); } catch { res.writeHead(404).end('not found'); return; }
  if (st.isDirectory()) return serveFile(req, res, path.join(file, 'index.html'), { cache });
  const etag = `"${st.size.toString(36)}-${Math.floor(st.mtimeMs).toString(36)}"`;
  const headers = { 'content-type': type || TYPES[path.extname(file)] || 'application/octet-stream', 'cache-control': cache, etag, vary: 'accept-encoding' };
  if (req.headers['if-none-match'] === etag) { res.writeHead(304, headers).end(); return; }
  let send = file, size = st.size;
  const accept = String(req.headers['accept-encoding'] || '');
  for (const [enc, ext] of [['br', '.br'], ['gzip', '.gz']]) {
    if (!accept.includes(enc)) continue;
    try { const cs = await fsp.stat(file + ext); send = file + ext; size = cs.size; headers['content-encoding'] = enc; break; } catch {}
  }
  headers['content-length'] = size;
  res.writeHead(200, headers);
  if (req.method === 'HEAD') return res.end();
  fs.createReadStream(send).on('error', () => res.destroy()).pipe(res);
}

// development: shaders straight from the translation output (out/shader_manifest, out/shaders)
const devShaders = new Map();
if (DEV) {
  for (const f of fs.readdirSync(path.join(ROOT, 'out/shader_manifest'))) {
    if (!f.endsWith('.json')) continue;
    const pkg = f.slice(0, -5);
    const m = JSON.parse(fs.readFileSync(path.join(ROOT, 'out/shader_manifest', f), 'utf8'));
    for (const [name, info] of Object.entries(m)) if (!devShaders.has(info.sha1)) devShaders.set(info.sha1, { pkg, name, info });
  }
}
const DEV_STATIC = [['/build/', path.join(ROOT, 'out/recomp/web_browser')], ['/', path.join(ROOT, 'web/recomp')]];

async function handle(req, res) {
  // cross-origin isolation: SharedArrayBuffer (wasm threads) needs it
  res.setHeader('Cross-Origin-Opener-Policy', 'same-origin');
  res.setHeader('Cross-Origin-Embedder-Policy', 'require-corp');
  res.setHeader('Cross-Origin-Resource-Policy', 'same-origin');
  res.setHeader('X-Content-Type-Options', 'nosniff');
  if (DEV) res.setHeader('Document-Policy', 'js-profiling');   // debug.js jsProfile(): the JS Self-Profiling API
  const url = new URL(req.url, 'http://localhost');
  let p;
  try { p = decodeURIComponent(url.pathname); } catch { res.writeHead(400).end(); return; }
  if (p.startsWith('/fs/')) return fsApi(req, res, p.slice(4), url.searchParams);
  if (p.includes('..') || p.includes('\0')) { res.writeHead(400).end(); return; }
  if (req.method !== 'GET' && req.method !== 'HEAD' && !(DEV && p === '/snap')) { res.writeHead(405).end(); return; }
  if (p.startsWith('/shader/')) {
    const sha = p.slice(8);
    if (!/^[0-9a-f]{40}$/.test(sha)) { res.writeHead(404).end(); return; }
    if (DIST) return serveFile(req, res, path.join(DIST, 'shader', sha), { type: 'application/json', cache: 'public, max-age=31536000, immutable' });
    const s = devShaders.get(sha);
    if (!s) { res.writeHead(404).end(); return; }
    const wgsl = await fsp.readFile(path.join(ROOT, 'out/shaders', s.pkg, s.info.wgsl), 'utf8');
    res.writeHead(200, { 'content-type': 'application/json', 'cache-control': 'no-cache' }).end(JSON.stringify({ name: s.pkg + '/' + s.name, info: s.info, wgsl }));
    return;
  }
  if (DEV && p === '/snap' && req.method === 'POST') {     // a debugging screenshot from the page (debug.js snap)
    const name = (url.searchParams.get('name') || 'snap').replace(/[^\w-]/g, '');
    await fsp.writeFile(path.join(ROOT, 'out/recomp', name + '.png'), await body(req, 64 << 20));
    res.writeHead(200).end();
    return;
  }
  if (DEV && p === '/ring.bin') return serveFile(req, res, path.join(ROOT, 'out/recomp/ring.bin'));
  if (DIST) return serveFile(req, res, path.join(DIST, p === '/' ? 'index.html' : p.slice(1)));
  for (const [prefix, dir] of DEV_STATIC) if (p.startsWith(prefix)) return serveFile(req, res, path.join(dir, p.slice(prefix.length)));
  res.writeHead(404).end();
}

const server = TLS ? https.createServer(TLS, handle) : http.createServer(handle);
server.listen(PORT, HOST, () => {
  const scheme = TLS ? 'https' : 'http';
  const shown = HOST === '0.0.0.0' || HOST === '::' ? 'localhost' : HOST;
  console.log(`recomp server on ${scheme}://${shown}:${PORT}/ (${DEV ? 'development' : 'bundle ' + DIST}; game ${GAME}, ` +
    `${gameFiles.size} files indexed; data ${DATA}; DLC ${DLC ? 'on' : 'off'}${DELAY ? `; +${DELAY} ms per /fs request` : ''})`);
  const loopback = HOST === '127.0.0.1' || HOST === 'localhost' || HOST === '::1';
  if (!loopback) {
    console.warn('[serve] WARNING: listening beyond this machine with no access control. The build and the game files are');
    console.warn('[serve] derived from your copy of the game and must not be public: firewall it or put an authenticating proxy in front.');
    if (!TLS) console.warn('[serve] Browsers allow SharedArrayBuffer (needed here) only on https:// or localhost: use --tls-cert/--tls-key or a TLS proxy.');
  }
});
// a failed request (client gone mid-stream, a file error) must not take the game's file server down
process.on('uncaughtException', (e) => console.error('[serve] ' + (e.stack || e)));
process.on('unhandledRejection', (e) => console.error('[serve] ' + (e && e.stack || e)));
