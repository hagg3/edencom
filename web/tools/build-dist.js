#!/usr/bin/env node
// build-dist.js — audit row A10: materialise public/ into a real, deployable dist/ tree.
//
// THE PROBLEM: CMake's `file(CREATE_LINK)` points public/audio -> ../media and
// public/Eden.eden -> the repo root using ABSOLUTE paths (see web/CMakeLists.txt). That makes
// public/ correct to serve from THIS checkout with tools/serve.js, but not portable — copying
// public/ to another machine, or handing it to a plain static host that doesn't dereference
// symlinks, ships two broken links instead of a game. `.github/workflows/pages.yml` already
// solved this for the GitHub Pages deploy (`cp -RL public/. _site/public/` dereferences both
// links into real files, scoped to that one CI job) — this script is the same fix, generalised
// so it also works for a local `dist/`-style deploy to a plain static host outside this repo's
// own CI (the gap A10's original finding named as still open after the Pages fix).
//
// Deliberately mirrors the CI "Assemble site" step's shape (see pages.yml) rather than
// reinventing one — same directory layout, same index.html redirect — so `dist/` and the
// deployed Pages site are the same artifact and a difference between them is a real bug, not
// two independently-maintained assembly scripts drifting apart.
//
// Usage:
//   node tools/build-dist.js [--build=rel|st|thr] [--out=dist]
//     --build=rel (default) uses build-rel/ (Release — what pages.yml ships).
//     --build=st            uses build-st/ (Debug, EDEN_DIAGNOSTICS=ON — for a local smoke check
//                            of the assembled tree without a Release rebuild).
//     --build=thr           uses build-thr/ (the -pthread build, audit row 36/C1). Emits
//                            `?build=thr` in the index.html redirect, which is also what tells
//                            service-worker.js to synthesise COOP/COEP for that navigation (see
//                            the cross-origin-isolation note below). EDEN_THREADED still defaults
//                            OFF, so this is opt-in and dev-facing until off-thread meshing lands.
//
// Does NOT invoke emcc/cmake — it only copies files a build already produced. Run
// `cmake --build build-rel -j8` (or build-st) first if the chosen build dir is missing or stale.

'use strict';
const fs = require('fs');
const path = require('path');
const crypto = require('crypto');

const webRoot = path.resolve(__dirname, '..');

function argValue(name, fallback) {
  const prefix = `--${name}=`;
  const hit = process.argv.slice(2).find((a) => a.startsWith(prefix));
  return hit ? hit.slice(prefix.length) : fallback;
}

const buildKind = argValue('build', 'rel');
if (!['rel', 'st', 'thr'].includes(buildKind)) {
  console.error(`build-dist: --build must be "rel", "st" or "thr", got "${buildKind}"`);
  process.exit(1);
}
const buildDirName = `build-${buildKind}`;
const buildDir = path.join(webRoot, buildDirName);
const outDir = path.resolve(webRoot, argValue('out', 'dist'));

function requireFile(p, hint) {
  if (!fs.existsSync(p)) {
    console.error(`build-dist: missing ${path.relative(webRoot, p)}${hint ? ` (${hint})` : ''}`);
    console.error(`build-dist: run "cmake --build ${buildDirName} -j8" first.`);
    process.exit(1);
  }
}

requireFile(path.join(buildDir, 'eden.js'));
requireFile(path.join(buildDir, 'eden.wasm'));

console.log(`build-dist: assembling ${buildDirName} -> ${path.relative(webRoot, outDir)}/`);

fs.rmSync(outDir, { recursive: true, force: true });
fs.mkdirSync(path.join(outDir, 'public'), { recursive: true });
fs.mkdirSync(path.join(outDir, buildDirName), { recursive: true });

// Real files, not the absolute-path symlinks CMake created for THIS checkout — this is the
// script's actual reason to exist (see the header comment for why public/audio and
// public/Eden.eden are symlinks). NOT fs.cpSync's `dereference` option: that flag only resolves
// the copy's TOP-LEVEL source if it is itself a symlink — it does not dereference symlinks
// encountered while walking a directory recursively (confirmed against Node 26; `cp -RL`, which
// pages.yml uses, has no such limitation). copyDereferenced() below is the manual walk that
// actually behaves like `cp -RL`.
function copyDereferenced(src, dst) {
  const real = fs.existsSync(src) ? fs.realpathSync(src) : src;
  const st = fs.statSync(real);
  if (st.isDirectory()) {
    fs.mkdirSync(dst, { recursive: true });
    for (const entry of fs.readdirSync(real)) {
      copyDereferenced(path.join(real, entry), path.join(dst, entry));
    }
  } else {
    fs.copyFileSync(real, dst);
  }
}
copyDereferenced(path.join(webRoot, 'public'), path.join(outDir, 'public'));

const serviceWorker = path.join(webRoot, 'service-worker.js');
if (fs.existsSync(serviceWorker)) {
  // Registered from eden-st.html as '../service-worker.js' (resolved against the site root) —
  // pages.yml's own comment on this same copy explains why it can't just live under public/.
  fs.copyFileSync(serviceWorker, path.join(outDir, 'service-worker.js'));
}

for (const f of ['eden.js', 'eden.wasm', 'eden.data']) {
  const src = path.join(buildDir, f);
  if (fs.existsSync(src)) fs.copyFileSync(src, path.join(outDir, buildDirName, f));
}

// Audit row 15/B6: content-hash eden.wasm/eden.js/eden.data and the public/*.js + *.css that
// eden-st.html loads, so a static host (or tools/serve.js, pointed at THIS tree) can serve them
// `immutable, max-age=31536000` without ever risking a stale hit — a rebuild that changes bytes
// mints a new filename, never the same URL with different content. The HTML stays unhashed on
// purpose (the row's own ask, "keep the HTML uncached"): it's the one file every load must
// re-fetch to discover which hashed assets are current. Deliberately scoped to files eden-st.html
// or service-worker.js reference by literal name — this is exactly the surface those two files'
// own text needs rewriting for; audio/images aren't part of this row.
//
// NOTE ON THE ACTUAL GITHUB PAGES DEPLOY: Pages serves through Fastly with a fixed
// Cache-Control it does not let repo content override (no `_headers` support like Netlify/
// Cloudflare Pages) — so on that host this step's real benefit is narrower than "immutable
// max-age": a hashed URL is *correct* to cache for as long as any intermediate (browser, Fastly's
// own edge cache) chooses to, because the content behind it never changes, whereas an unhashed
// URL needs revalidation every time whatever TTL Pages picks expires even when nothing changed.
// The `immutable, max-age=31536000` header is real and honoured when this dist/ tree is served by
// `tools/serve.js` (which special-cases the hashed-filename pattern) or any host that does pass
// through custom headers.
//
// ---- THE SAME LIMITATION ALMOST BLOCKED THE THREADED BUILD (audit row 36/C1, passes 63+65) ----
// A `-DEDEN_THREADED=ON` build's wasm memory is a SharedArrayBuffer, and browsers only expose
// SharedArrayBuffer to a cross-origin-ISOLATED document — which requires the DOCUMENT's own
// response to carry
//     Cross-Origin-Opener-Policy: same-origin
//     Cross-Origin-Embedder-Policy: require-corp
// `tools/serve.js` sends both, so the threaded build has been testable locally since pass 63.
// GitHub Pages cannot send either, for exactly the reason spelled out above — no `_headers`, no
// way to set response headers from repo content. Two options were weighed (pass 63) and the
// decision (2026-08-04) was:
//
//   1. NOT TAKEN: move the deploy to a host that passes custom headers (Cloudflare Pages/Netlify
//      `_headers`, or any real server). Least clever, but it is a hosting migration — new URL
//      unless the domain moves too, and new infra to operate, to buy a capability one file can
//      provide.
//   2. TAKEN, and implemented in pass 65: the `coi-serviceworker` pattern. The service worker
//      synthesises both headers on the navigation response, and the page reloads once so the
//      document is created from an isolated response. Merged into this port's EXISTING
//      `service-worker.js` (two registrations cannot share a scope) and driven by
//      `public/eden-coi.js`, which registers, pings the controller to confirm it can isolate,
//      reloads exactly once, and otherwise fails closed to the single-threaded build.
//
// So a `--build=thr` dist IS deployable to Pages: the index.html redirect below emits
// `?build=thr`, which is the URL the SW isolates. Verify a threaded dist by serving it with the
// real headers withheld — `node tools/build-dist.js --build=thr && node tools/serve.js 8123 dist
// --no-coi` — which is what the deployed host actually looks like.
//
// Off-thread meshing (B3) landed 2026-08-27 and as of 2026-08-30 `.github/workflows/pages.yml`
// ships the threaded Release build as the deployed default (`?build=thr`), with the single-threaded
// Release tree still built and shipped as the isolation-failed fallback. `EDEN_THREADED` still
// defaults OFF at the CMake level (audit row A1) so `build-st` stays single-threaded locally.
function hashFile(filePath) {
  return crypto.createHash('sha256').update(fs.readFileSync(filePath)).digest('hex').slice(0, 10);
}

function hashedName(basename, hash) {
  const ext = path.extname(basename);
  return basename.slice(0, -ext.length) + '.' + hash + ext;
}

// rewrites: Map<original literal string found in eden-st.html/service-worker.js, replacement>
const rewrites = new Map();

for (const f of ['eden.js', 'eden.wasm', 'eden.data']) {
  const src = path.join(outDir, buildDirName, f);
  if (!fs.existsSync(src)) continue;
  const hashed = hashedName(f, hashFile(src));
  fs.renameSync(src, path.join(outDir, buildDirName, hashed));
  rewrites.set(`'${f}'`, `'${hashed}'`); // matches the EDEN_ASSET_MAP entries below
}

const htmlPath = path.join(outDir, 'public', 'eden-st.html');
let html = fs.readFileSync(htmlPath, 'utf8');

// eden.js/eden.wasm/eden.data are requested by BARE filename from inside eden.js's own generated
// code (locateFile) or by the page's own `edenScript.src` line — never by a literal that also
// matches an unrelated string, so rewriting the EDEN_ASSET_MAP={} initialiser is enough; the
// consuming code (locateFile, edenScript.src, both in eden-st.html) already reads through it.
//
// DONE HERE, BEFORE public/*.js ARE HASHED, and searched for rather than assumed — pass 65 found
// this rewrite silently doing nothing. Audit row 28/C5 moved the declaration out of eden-st.html
// into public/eden-host.js when it split the inline script, and this step still targeted the
// HTML, so every dist assembled after that split renamed the build files to hashed names and then
// asked for the ORIGINAL names: a guaranteed 404 on eden.js and a black canvas, with nothing in
// the build output saying so. (The GitHub Pages deploy was never affected — .github/workflows/
// pages.yml assembles the site itself and does no hashing.) Two things keep it fixed: patch
// whichever file actually declares it, and HARD-FAIL when that count isn't exactly 1. Order also
// matters: patching a public/*.js AFTER hashing it would leave a content-hash name that no longer
// matches the content, which is the one invariant the whole scheme rests on.
const ASSET_MAP_DECL = 'const EDEN_ASSET_MAP = {};';
const assetMapEntries = ['eden.js', 'eden.wasm', 'eden.data']
  .filter((f) => rewrites.has(`'${f}'`))
  .map((f) => `'${f}': ${rewrites.get(`'${f}'`)}`)
  .join(', ');
const ASSET_MAP_PATCHED = `const EDEN_ASSET_MAP = {${assetMapEntries}};`;

let assetMapSites = 0;
if (html.includes(ASSET_MAP_DECL)) {           // where it lived before audit row 28/C5
  html = html.replace(ASSET_MAP_DECL, ASSET_MAP_PATCHED);
  assetMapSites++;
}
for (const f of fs.readdirSync(path.join(outDir, 'public')).filter((n) => n.endsWith('.js'))) {
  const p = path.join(outDir, 'public', f);    // where it lives now: eden-host.js
  const src = fs.readFileSync(p, 'utf8');
  if (!src.includes(ASSET_MAP_DECL)) continue;
  fs.writeFileSync(p, src.replace(ASSET_MAP_DECL, ASSET_MAP_PATCHED));
  assetMapSites++;
}
if (assetMapSites !== 1) {
  console.error(`build-dist: expected exactly ONE "${ASSET_MAP_DECL}" declaration across ` +
    `public/eden-st.html and public/*.js, found ${assetMapSites}.`);
  console.error('build-dist: without it the content-hashed build files are unreachable and the ' +
    'assembled tree would 404 on eden.js. Refusing to emit a broken dist.');
  process.exit(1);
}

// public/*.js and public/*.css referenced by literal src="…"/href="…" — hash only what's actually
// linked, so nothing under public/audio or public/*.png is touched.
const assetRefRe = /(src|href)="([\w-]+\.(?:js|css))"/g;
let m;
const seen = new Map(); // basename -> hashed name, so a file referenced twice gets one hash
while ((m = assetRefRe.exec(html))) {
  const basename = m[2];
  if (seen.has(basename)) continue;
  const src = path.join(outDir, 'public', basename);
  if (!fs.existsSync(src)) continue;
  seen.set(basename, hashedName(basename, hashFile(src)));
}
for (const [basename, hashed] of seen) {
  fs.renameSync(path.join(outDir, 'public', basename), path.join(outDir, 'public', hashed));
  rewrites.set(`"${basename}"`, `"${hashed}"`);
}

for (const [literal, replacement] of rewrites) {
  if (literal.startsWith('"')) html = html.split(literal).join(replacement); // src=/href= rewrites
}
fs.writeFileSync(htmlPath, html);

const swPath = path.join(outDir, 'service-worker.js');
if (fs.existsSync(swPath)) {
  let sw = fs.readFileSync(swPath, 'utf8');
  for (const [basename, hashed] of seen) {
    sw = sw.split(`public/${basename}`).join(`public/${hashed}`);
  }
  fs.writeFileSync(swPath, sw);
}

const buildFileCount = ['eden.js', 'eden.wasm', 'eden.data'].filter((f) => rewrites.has(`'${f}'`)).length;
console.log(`build-dist: content-hashed ${buildFileCount + seen.size} asset file(s)`);

// The redirect carries the build selector because eden-host.js's DEFAULT is build-st/ — a dist
// assembled from any other build serves nothing at that path, so a bare /public/eden-st.html in a
// rel or thr tree would 404 on eden.js. For --build=thr the parameter does double duty: it is
// also the signal service-worker.js keys cross-origin isolation off (wantsIsolation()).
const buildParam = buildKind === 'st' ? '' : `?build=${buildKind}`;
fs.writeFileSync(
  path.join(outDir, 'index.html'),
  `<!doctype html>\n<meta charset="utf-8">\n` +
    `<meta http-equiv="refresh" content="0; url=public/eden-st.html${buildParam}">\n` +
    `<p>Loading Eden: World Builder… if nothing happens, ` +
    `<a href="public/eden-st.html${buildParam}">click here</a>.</p>\n`
);

console.log(`build-dist: done. Serve ${path.relative(webRoot, outDir)}/ with any static file`);
console.log('build-dist: server that supports HTTP Range requests (tools/serve.js does; a host');
console.log('build-dist: that does not, like GitHub Pages, silently falls back to the eager');
console.log('build-dist: whole-file Eden.eden path instead of lazy byte-range loading).');
