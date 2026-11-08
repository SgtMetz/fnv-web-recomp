#!/usr/bin/env node
// Local dev server for the browser build of the whole-game recompiler (docs/RECOMP.md, milestone 3).
// Binds to 127.0.0.1 only: the player's game files are read from their own disk and never leave the machine.
//
//   /                  web/recomp/ (index.html, renderer.js, replay.html)
//   /build/            out/recomp/web_browser/ (fnv_web.js, .wasm) - the Emscripten browser build
//   /fs/stat|list|read GET, /fs/write|truncate|mkdir|remove|rename POST - the game's file layer (pal_fs_web.cpp)
//                      roots: /game -> the install (read-only), /docs -> out/fnv_docs (the private My Games copy)
//   /shader/<sha1>     the translated WGSL and its manifest entry for a D3D9 shader (out/shaders, out/shader_manifest)
//   /ring.bin          a captured command ring (FNV_RING_DUMP) for replay.html
//
// usage: node tools/recomp_serve.mjs [--game "E:/Games/Fallout New Vegas"] [--port 8093] [--dlc]
//
// DLC is off by default. The game turns a plugin on by itself when Data\<name>.NAM exists (how the Steam DLC and the
// pre-order packs install), so without --dlc (or FNV_DLC=1) the server answers as if no .nam file existed and only
// FalloutNV.esm loads. The install is never changed.
import http from 'node:http';
import fs from 'node:fs';
import fsp from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const args = process.argv.slice(2);
const opt = (name, def) => { const i = args.indexOf(name); return i >= 0 ? args[i + 1] : def; };
const GAME = path.resolve(opt('--game', process.env.FNV_GAME || 'E:/Games/Fallout New Vegas'));
const DOCS = path.resolve(ROOT, 'out/fnv_docs');
const PORT = +opt('--port', 8093);
const DLC = args.includes('--dlc') || process.env.FNV_DLC === '1';
// a DLC marker the game must not see: /game/Data/<name>.nam
const hiddenDlc = (p) => !DLC && /^\/game\/data\/[^/]+\.nam$/i.test(p || '');
const STATIC = [['/build/', path.join(ROOT, 'out/recomp/web_browser')], ['/', path.join(ROOT, 'web/recomp')]];
const TYPES = { '.html': 'text/html', '.js': 'text/javascript', '.mjs': 'text/javascript', '.wasm': 'application/wasm',
  '.css': 'text/css', '.json': 'application/json', '.bin': 'application/octet-stream' };

// sha1 -> {pkg, name, info}: every translated shader
const shaders = new Map();
for (const f of fs.readdirSync(path.join(ROOT, 'out/shader_manifest'))) {
  if (!f.endsWith('.json')) continue;
  const pkg = f.slice(0, -5);
  const m = JSON.parse(fs.readFileSync(path.join(ROOT, 'out/shader_manifest', f), 'utf8'));
  for (const [name, info] of Object.entries(m)) if (!shaders.has(info.sha1)) shaders.set(info.sha1, { pkg, name, info });
}

// a game path (/game/..., /docs/...) -> a host path inside that root, or null
function hostPath(p, write) {
  const m = /^\/(game|docs)(\/.*)?$/i.exec(p || '');
  if (!m) return null;
  const root = m[1].toLowerCase() === 'game' ? GAME : DOCS;
  if (write && root !== DOCS) return null;
  const full = path.resolve(root, '.' + (m[2] || '/'));
  if (full !== root && !full.startsWith(root + path.sep)) return null;
  return full;
}

function body(req) {
  return new Promise((ok, fail) => { const parts = []; req.on('data', (c) => parts.push(c)); req.on('end', () => ok(Buffer.concat(parts))); req.on('error', fail); });
}

const seen = new Set();
const rate = { n: 0, bytes: 0, t: Date.now(), ops: {} };
setInterval(() => {
  if (!rate.n) return;
  console.log(`[fs] ${rate.n} requests, ${(rate.bytes / 1e6).toFixed(1)} MB in ${((Date.now() - rate.t) / 1000).toFixed(0)} s: ${JSON.stringify(rate.ops)}`);
  rate.n = 0; rate.bytes = 0; rate.t = Date.now(); rate.ops = {};
}, 10000).unref();
async function fsApi(req, res, op, q) {
  rate.n++; rate.ops[op] = (rate.ops[op] || 0) + 1;
  if (op === 'read') rate.bytes += +q.get('n') || 0;
  const key = op + ' ' + q.get('p');
  if (!seen.has(key)) { seen.add(key); console.log('[fs] ' + key); }
  const write = req.method === 'POST';
  if (hiddenDlc(q.get('p'))) { res.writeHead(404).end('ENOENT'); return; }
  const p = hostPath(q.get('p'), write);
  if (!p) { res.writeHead(403).end(); return; }
  try {
    if (op === 'stat') {
      const st = await fsp.stat(p);
      res.writeHead(200, { 'content-type': 'application/json' }).end(JSON.stringify({ dir: st.isDirectory(), size: st.size, mtime: Math.floor(st.mtimeMs / 1000) }));
    } else if (op === 'list') {
      let names = await fsp.readdir(p);                    // before writeHead: a missing folder must answer 404
      if (!DLC && /^\/game\/data\/?$/i.test(q.get('p'))) names = names.filter((n) => !/\.nam$/i.test(n));
      res.writeHead(200, { 'content-type': 'application/json' }).end(JSON.stringify(names));
    } else if (op === 'read') {
      const o = +q.get('o') || 0, n = Math.min(+q.get('n') || 0, 64 << 20);
      const fh = await fsp.open(p, 'r');
      const buf = Buffer.alloc(n);
      const { bytesRead } = await fh.read(buf, 0, n, o);
      await fh.close();
      res.writeHead(200, { 'content-type': 'application/octet-stream' }).end(buf.subarray(0, bytesRead));
    } else if (op === 'write' && write) {
      const data = await body(req);
      const fh = await fsp.open(p, fs.existsSync(p) ? 'r+' : 'w+');
      await fh.write(data, 0, data.length, +q.get('o') || 0);
      await fh.close();
      res.writeHead(200).end();
    } else if (op === 'truncate' && write) {
      await fsp.mkdir(path.dirname(p), { recursive: true });
      await fsp.writeFile(p, Buffer.alloc(0));
      res.writeHead(200).end();
    } else if (op === 'mkdir' && write) {
      await fsp.mkdir(p);
      res.writeHead(200).end();
    } else if (op === 'remove' && write) {
      await fsp.rm(p);
      res.writeHead(200).end();
    } else if (op === 'rename' && write) {
      const to = hostPath(q.get('q'), true);
      if (!to) { res.writeHead(403).end(); return; }
      await fsp.rename(p, to);
      res.writeHead(200).end();
    } else res.writeHead(400).end();
  } catch (e) {
    if (!res.headersSent) res.writeHead(404).end(String(e.code || e));
    else res.destroy();
  }
}

async function serveFile(res, file, req) {
  try {
    const st = await fsp.stat(file);
    if (st.isDirectory()) return serveFile(res, path.join(file, 'index.html'), req);
    res.writeHead(200, { 'content-type': TYPES[path.extname(file)] || 'application/octet-stream', 'content-length': st.size,
      'cache-control': 'no-cache' });
    if (req.method === 'HEAD') return res.end();
    fs.createReadStream(file).on('error', () => res.destroy()).pipe(res);
  } catch {
    res.writeHead(404).end('not found');
  }
}

const server = http.createServer(async (req, res) => {
  // cross-origin isolation: SharedArrayBuffer (wasm threads) needs it
  res.setHeader('Cross-Origin-Opener-Policy', 'same-origin');
  res.setHeader('Cross-Origin-Embedder-Policy', 'require-corp');
  res.setHeader('Cross-Origin-Resource-Policy', 'same-origin');
  const url = new URL(req.url, 'http://localhost');
  const p = decodeURIComponent(url.pathname);
  if (p.startsWith('/fs/')) return fsApi(req, res, p.slice(4), url.searchParams);
  if (p.startsWith('/shader/')) {
    const s = shaders.get(p.slice(8));
    if (!s) { res.writeHead(404).end(); return; }
    const wgsl = await fsp.readFile(path.join(ROOT, 'out/shaders', s.pkg, s.info.wgsl), 'utf8');
    res.writeHead(200, { 'content-type': 'application/json', 'cache-control': 'max-age=3600' }).end(JSON.stringify({ name: s.pkg + '/' + s.name, info: s.info, wgsl }));
    return;
  }
  if (p === '/snap' && req.method === 'POST') {           // a debugging screenshot from the page
    const name = (url.searchParams.get('name') || 'snap').replace(/[^\w-]/g, '');
    await fsp.writeFile(path.join(ROOT, 'out/recomp', name + '.png'), await body(req));
    res.writeHead(200).end();
    return;
  }
  if (p === '/ring.bin') return serveFile(res, path.join(ROOT, 'out/recomp/ring.bin'), req);
  if (p.includes('..')) { res.writeHead(400).end(); return; }
  for (const [prefix, dir] of STATIC) if (p.startsWith(prefix)) return serveFile(res, path.join(dir, p.slice(prefix.length)), req);
  res.writeHead(404).end();
});
server.listen(PORT, '127.0.0.1', () => console.log(`recomp server on http://localhost:${PORT}/ (game ${GAME}, ${shaders.size} shaders, DLC ${DLC ? 'on' : 'off'})`));
// a failed request (client gone mid-stream, a file error) must not take the game's file server down
process.on('uncaughtException', (e) => console.error('[serve] ' + (e.stack || e)));
process.on('unhandledRejection', (e) => console.error('[serve] ' + (e && e.stack || e)));
