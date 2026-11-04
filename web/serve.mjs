// Static server for the web runtime, bound to 127.0.0.1 only.
//
// It sends the cross-origin-isolation headers (COOP/COEP) the runtime needs for SharedArrayBuffer and Atomics.wait,
// which the synchronous file layer (src/vfs-*.js) and, later, the pthread build use.
//
// Dev only: with --install <dir> it also exposes a read-only view of a local install at /__dev_install/, so the
// importer can be tested without the native folder picker (which automation cannot click). Only files the importer
// would copy are served (everything under Data/ and the INIs at the install root); executables are never served.
//   GET /__dev_install/__list.json   -> [{path, size, lastModified}]
//   GET/HEAD /__dev_install/<path>   -> file bytes, Range supported
//
// Usage: node web/serve.mjs [--port 8650] [--install "E:/Games/Fallout New Vegas"]
import { createServer } from 'node:http';
import { createReadStream, statSync, readdirSync } from 'node:fs';
import { join, extname, normalize, resolve, relative, sep } from 'node:path';
import { fileURLToPath } from 'node:url';

const args = process.argv.slice(2);
const opt = (name) => { const i = args.indexOf(name); return i >= 0 ? args[i + 1] : undefined; };
const root = fileURLToPath(new URL('.', import.meta.url));
const install = opt('--install') || process.env.FNV_INSTALL;
const installRoot = install && resolve(install);
const port = +(opt('--port') || process.env.PORT || 8650);

const types = {
  '.html': 'text/html; charset=utf-8', '.js': 'text/javascript; charset=utf-8', '.mjs': 'text/javascript; charset=utf-8',
  '.css': 'text/css; charset=utf-8', '.json': 'application/json; charset=utf-8', '.wasm': 'application/wasm',
  '.svg': 'image/svg+xml', '.png': 'image/png', '.ico': 'image/x-icon', '.txt': 'text/plain; charset=utf-8',
};
const isolation = {
  'Cross-Origin-Opener-Policy': 'same-origin',
  'Cross-Origin-Embedder-Policy': 'require-corp',
  'Cross-Origin-Resource-Policy': 'same-origin',
};

function servable(rel) {
  const p = rel.replace(/\\/g, '/');
  if (/\.(exe|dll|lnk|dat|msg|zip|hashdb|info|script)$/i.test(p)) return false;
  return /^data\//i.test(p) || /^[^/]+\.ini$/i.test(p);
}

function listInstall() {
  const out = [];
  const walk = (dir) => {
    for (const e of readdirSync(dir, { withFileTypes: true })) {
      const full = join(dir, e.name);
      if (e.isDirectory()) walk(full);
      else if (e.isFile()) {
        const rel = relative(installRoot, full).split(sep).join('/');
        if (!servable(rel)) continue;
        const st = statSync(full);
        out.push({ path: rel, size: st.size, lastModified: Math.round(st.mtimeMs) });
      }
    }
  };
  walk(installRoot);
  return out;
}

function sendFile(req, res, file, extra = {}) {
  let st;
  try { st = statSync(file); } catch { res.writeHead(404, isolation).end(); return; }
  if (!st.isFile()) { res.writeHead(404, isolation).end(); return; }
  const headers = { ...isolation, 'Content-Type': types[extname(file).toLowerCase()] || 'application/octet-stream',
                    'Accept-Ranges': 'bytes', 'Cache-Control': 'no-store',
                    'Last-Modified': new Date(st.mtimeMs).toUTCString(), ...extra };
  const m = /^bytes=(\d+)-(\d*)$/.exec(req.headers.range || '');
  if (m) {
    const a = +m[1], b = Math.min(m[2] === '' ? st.size - 1 : +m[2], st.size - 1);
    if (a > b) { res.writeHead(416, { ...headers, 'Content-Range': `bytes */${st.size}` }).end(); return; }
    res.writeHead(206, { ...headers, 'Content-Range': `bytes ${a}-${b}/${st.size}`, 'Content-Length': b - a + 1 });
    if (req.method === 'HEAD') res.end(); else createReadStream(file, { start: a, end: b }).pipe(res);
  } else {
    res.writeHead(200, { ...headers, 'Content-Length': st.size });
    if (req.method === 'HEAD') res.end(); else createReadStream(file).pipe(res);
  }
}

createServer((req, res) => {
  if (req.method !== 'GET' && req.method !== 'HEAD') { res.writeHead(405, isolation).end(); return; }
  let url;
  try { url = decodeURIComponent(new URL(req.url, 'http://x').pathname); } catch { res.writeHead(400).end(); return; }
  if (url.startsWith('/__dev_install/')) {
    if (!installRoot) { res.writeHead(404, isolation).end(); return; }
    const rel = url.slice('/__dev_install/'.length);
    if (rel === '__list.json') {
      const body = JSON.stringify(listInstall());
      res.writeHead(200, { ...isolation, 'Content-Type': types['.json'], 'Cache-Control': 'no-store' }).end(body);
      return;
    }
    const file = resolve(installRoot, normalize(rel).replace(/^([/\\])+/, ''));
    const back = relative(installRoot, file);
    if (back.startsWith('..') || !servable(back)) { res.writeHead(403, isolation).end(); return; }
    sendFile(req, res, file);
    return;
  }
  const file = resolve(root, '.' + normalize(url.endsWith('/') ? url + 'index.html' : url));
  if (relative(root, file).startsWith('..') || !types[extname(file).toLowerCase()]) {
    res.writeHead(404, isolation).end(); return;
  }
  sendFile(req, res, file);
}).listen(port, '127.0.0.1', () => {
  console.log(`web runtime on http://127.0.0.1:${port}/` + (installRoot ? ` (dev install: ${installRoot})` : ''));
});
