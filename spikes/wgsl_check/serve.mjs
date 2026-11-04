// Local-only server for the WGSL browser check. Serves the page plus the git-ignored translated shaders under
// out/shaders/<pkg>/*.wgsl.gen. Binds 127.0.0.1 only. usage: node spikes/wgsl_check/serve.mjs [port]
import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
const here = path.dirname(fileURLToPath(import.meta.url));
const shaders = path.resolve(here, '../../out/shaders');
const port = Number(process.argv[2] || 8660);
http.createServer((req, res) => {
  const u = new URL(req.url, 'http://x');
  try {
    if (u.pathname === '/') { res.setHeader('content-type', 'text/html'); return res.end(fs.readFileSync(path.join(here, 'index.html'))); }
    if (u.pathname === '/list') {
      const out = {};
      for (const p of fs.readdirSync(shaders)) {
        const fs_ = fs.readdirSync(path.join(shaders, p)).filter(f => f.endsWith('.wgsl.gen'));
        if (fs_.length) out[p] = fs_;
      }
      res.setHeader('content-type', 'application/json'); return res.end(JSON.stringify(out));
    }
    const m = u.pathname.match(/^\/f\/([\w]+)\/([^/]+\.wgsl\.gen)$/);
    if (m) { res.setHeader('content-type', 'text/plain'); return res.end(fs.readFileSync(path.join(shaders, m[1], decodeURIComponent(m[2])))); }
  } catch (e) { res.statusCode = 500; return res.end(String(e)); }
  res.statusCode = 404; res.end('not found');
}).listen(port, '127.0.0.1', () => console.log('http://127.0.0.1:' + port + '/'));
