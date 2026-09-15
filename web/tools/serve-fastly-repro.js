#!/usr/bin/env node
// serve-fastly-repro.js — reproduces the GitHub Pages/Fastly quirk that produced a black canvas
// in deployed Safari (pass 57): when the client sends `Accept-Encoding: gzip`, Fastly answers
// from the GZIP representation of Eden.eden and applies the Range header to the COMPRESSED
// body, not the real 55 MB file. Identity/br clients get honest ranges against the real size.
// Everything else is a plain static server over `web/` — no compression, no ETag, this is a
// narrow repro server, not a replacement for tools/serve.js.
//
// Why this matters and why it's checked in rather than left as scratch: this is the only thing
// that reproduces the bug class "capability probe and real transport disagree about content
// encoding under a Range request" — Safari-only, deployed-site-only, and invisible to every other
// browser and to any local server that doesn't compress the .eden. It will be needed again for
// any future change to the lazy Eden.eden FS node's capability probe
// (src/seam/js/eden_default_world.pre.js). Pair with tools/safari-frame-check.js, which is the
// only way to confirm the engine's frame loop (not just the DOM) actually came up.
//
// Usage: node tools/serve-fastly-repro.js [port]
//   node tools/serve-fastly-repro.js 8199
//   open http://localhost:8199/public/eden-st.html?build=st   (or ?build=rel)

'use strict';
const http = require('http');
const fs = require('fs');
const path = require('path');
const zlib = require('zlib');

const ROOT = path.resolve(__dirname, '..'); // web/
const PORT = parseInt(process.argv[2], 10) || 8199;

const TYPES = {
  '.html': 'text/html', '.js': 'text/javascript', '.css': 'text/css',
  '.wasm': 'application/wasm', '.json': 'application/json', '.png': 'image/png',
  '.eden': 'application/octet-stream', '.mp3': 'audio/mpeg', '.caf': 'audio/x-caf',
  '.webmanifest': 'application/manifest+json',
};

// Compressed once per real file path and kept resident — this server only ever serves one large
// .eden, so an unbounded Map is fine; it exists for the length of one repro session.
const gzCache = new Map();

const server = http.createServer((req, res) => {
  const urlPath = decodeURIComponent(req.url.split('?')[0]);
  let file = path.join(ROOT, urlPath);
  let stat;
  try {
    stat = fs.statSync(file);
    if (stat.isDirectory()) { file = path.join(file, 'index.html'); stat = fs.statSync(file); }
  } catch { res.writeHead(404); res.end('Not found: ' + urlPath); return; }

  let real;
  try { real = fs.realpathSync(file); } catch { res.writeHead(404); res.end('Not found: ' + urlPath); return; }

  const ext = path.extname(real);
  const ct = TYPES[ext] || 'application/octet-stream';
  const ae = String(req.headers['accept-encoding'] || '');
  // The quirk: only Fastly's real behaviour for the big .eden, and only when gzip was offered
  // (br and identity clients get honest ranges against the real file, matching production).
  const quirk = ext === '.eden' && /\bgzip\b/.test(ae);

  let body, enc = null;
  if (quirk) {
    if (!gzCache.has(real)) gzCache.set(real, zlib.gzipSync(fs.readFileSync(real)));
    body = gzCache.get(real);
    enc = 'gzip';
  } else {
    body = fs.readFileSync(real);
  }

  const range = req.headers.range;
  const hdr = { 'Content-Type': ct, 'Accept-Ranges': 'bytes' };
  if (enc) hdr['Content-Encoding'] = enc;

  if (range) {
    const m = /bytes=(\d+)-(\d*)/.exec(range);
    if (m) {
      const start = Number(m[1]);
      if (start >= body.length) {                       // the 416 Safari was hitting in production
        hdr['Content-Range'] = `bytes */${body.length}`;
        res.writeHead(416, hdr); res.end(); return;
      }
      const end = m[2] === '' ? body.length - 1 : Math.min(Number(m[2]), body.length - 1);
      hdr['Content-Range'] = `bytes ${start}-${end}/${body.length}`;
      hdr['Content-Length'] = end - start + 1;
      res.writeHead(206, hdr); res.end(body.subarray(start, end + 1)); return;
    }
  }

  hdr['Content-Length'] = body.length;
  res.writeHead(200, hdr); res.end(body);
});

server.listen(PORT, () => {
  console.log(`[serve-fastly-repro] ${ROOT} -> http://localhost:${PORT}/  (gzip+Range quirk on .eden)`);
});
