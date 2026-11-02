// Static server for the spike, bound to 127.0.0.1 only. With FNV_DATA set it also exposes that folder at
// /__dev_data/ (HEAD + Range only) so the page can be tested without the native folder picker. Dev use only.
import { createServer } from 'node:http';
import { createReadStream, statSync } from 'node:fs';
import { join, extname, normalize, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = fileURLToPath(new URL('.', import.meta.url));
const dataArg = process.env.FNV_DATA || process.argv[2];
const data = dataArg && resolve(dataArg);
const port = +(process.env.PORT || 8642);
const types = { '.html': 'text/html', '.js': 'text/javascript', '.mjs': 'text/javascript' };

createServer((req, res) => {
  const url = decodeURIComponent(new URL(req.url, 'http://x').pathname);
  let file;
  if (url.startsWith('/__dev_data/')) {
    if (!data) { res.writeHead(404).end(); return; }
    file = join(data, normalize(url.slice(12)).replace(/^([/\\])+/, ''));
    if (!file.startsWith(data)) { res.writeHead(403).end(); return; }
  } else {
    file = join(root, normalize(url === '/' ? '/index.html' : url));
    if (!file.startsWith(root) || !types[extname(file)]) { res.writeHead(404).end(); return; }
  }
  let st;
  try { st = statSync(file); } catch { res.writeHead(404).end(); return; }
  const headers = { 'Content-Type': types[extname(file)] || 'application/octet-stream', 'Accept-Ranges': 'bytes',
                    'Cache-Control': 'no-store' };
  const m = /bytes=(\d+)-(\d+)/.exec(req.headers.range || '');
  if (m) {
    const a = +m[1], b = Math.min(+m[2], st.size - 1);
    res.writeHead(206, { ...headers, 'Content-Range': `bytes ${a}-${b}/${st.size}`, 'Content-Length': b - a + 1 });
    createReadStream(file, { start: a, end: b }).pipe(res);
  } else {
    res.writeHead(200, { ...headers, 'Content-Length': st.size });
    if (req.method === 'HEAD') res.end(); else createReadStream(file).pipe(res);
  }
}).listen(port, '127.0.0.1', () => console.log(`bsa viewer on http://127.0.0.1:${port}/`));
