#!/usr/bin/env node
// browser-memory-probe.js — ROADMAP Phase M / M0: attribute the ~350 MB gap between measured
// wasm linear memory (~128-160 MB, headless) and reported RSS (~502 MB iPad / ~1.9 GB desktop),
// and decide whether the 2 GB shared-memory `maximum` is reserved-only or actually resident.
//
// Drives real macOS Safari over safaridriver (same WebKit as iOS Safari — the best proxy for the
// iPad Air 2 available in this checkout) through the fixed M0.3 scripted session, sampling every
// memory source on a schedule. The single most important reading is `vmmap --summary` on the
// com.apple.WebKit.WebContent process, which separates RESERVED (large VIRTUAL, small dirty) from
// COMMITTED (dirty) — that is the whole question about the shared-memory `maximum`.
//
// Build the matrix trees (Release codegen + EDEN_DIAGNOSTICS for eden_console_teleport):
//   emcmake cmake -B build-relwdiag       -DCMAKE_BUILD_TYPE=Release -DEDEN_DIAGNOSTICS=ON -DEDEN_THREADED=OFF
//   emcmake cmake -B build-relthr         -DCMAKE_BUILD_TYPE=Release -DEDEN_DIAGNOSTICS=ON -DEDEN_THREADED=ON
//   emcmake cmake -B build-relwdiag-cap512 ... -DCMAKE_EXE_LINKER_FLAGS="-sMAXIMUM_MEMORY=536870912"
//   emcmake cmake -B build-relthr-cap512   ... -DCMAKE_EXE_LINKER_FLAGS="-sMAXIMUM_MEMORY=536870912"
// eden-host.js needs a `?build=<dir>` branch for each while this runs (revert after) — or symlink
// the tree to one of the names eden-host.js already knows (relwdiag / relthr).
//
//   safaridriver -p 4599 &
//   node tools/serve.js 8232                       # MUST be serve.js — COOP/COEP for ?build=thr
//   node tools/browser-memory-probe.js 'http://localhost:8232/public/eden-st.html?build=relthr' --label=thr-uncapped
//   node tools/browser-memory-probe.js --quit
//
// Discard the FIRST run against any freshly built tree (cold .wasm inflates the first read).
'use strict';
const fs = require('fs');
const os = require('os');
const path = require('path');
const { execFileSync } = require('child_process');

const args = process.argv.slice(2);
const wantsQuit = args.includes('--quit');
const portArg = args.find(a => a.startsWith('--port='));
const labelArg = args.find(a => a.startsWith('--label='));
const outArg = args.find(a => a.startsWith('--out='));
const PORT = portArg ? Number(portArg.split('=')[1]) : 4599;
const LABEL = labelArg ? labelArg.split('=')[1] : 'run';
const URL = args.find(a => !a.startsWith('--'));

const BASE = `http://localhost:${PORT}`;
const SESSION_FILE = path.join(os.tmpdir(), `eden-safari-session-${PORT}.txt`);

async function req(method, p, body) {
  let r;
  try {
    r = await fetch(BASE + p, {
      method,
      headers: { 'Content-Type': 'application/json' },
      body: body === undefined ? undefined : JSON.stringify(body),
    });
  } catch (e) {
    throw new Error(`can't reach safaridriver at ${BASE} (start it: safaridriver -p ${PORT} &) — ${e.message}`);
  }
  const j = await r.json();
  if (j.value && j.value.error) throw new Error(j.value.error + ': ' + j.value.message);
  return j.value;
}
const wd = {
  newSession: () => req('POST', '/session', { capabilities: { alwaysMatch: { browserName: 'safari' } } }).then(v => v.sessionId),
  go: (s, url) => req('POST', `/session/${s}/url`, { url }),
  exec: (s, script, a = []) => req('POST', `/session/${s}/execute/sync`, { script, args: a }),
  quit: (s) => req('DELETE', `/session/${s}`),
};
async function getSession() {
  let s;
  try {
    s = fs.readFileSync(SESSION_FILE, 'utf8').trim();
    await wd.exec(s, 'return 1;');
  } catch {
    s = await wd.newSession();
    fs.writeFileSync(SESSION_FILE, s);
  }
  return s;
}
const sleep = (ms) => new Promise(r => setTimeout(r, ms));

// --- macOS process memory: the readings headless cannot give -------------------------------------
// WebKit splits its work across several com.apple.WebKit.WebContent processes; the one hosting our
// tab is whichever has the largest footprint once a world is loaded. We sample the whole set and
// keep the max, and also run `vmmap --summary` on it for the reserved-vs-dirty split.
function webContentPids() {
  try {
    return execFileSync('pgrep', ['-f', 'com.apple.WebKit.WebContent.xpc'], { encoding: 'utf8' })
      .trim().split(/\s+/).filter(Boolean);
  } catch { return []; }
}
function rssMB(pid) {
  try {
    const out = execFileSync('ps', ['-o', 'rss=', '-p', pid], { encoding: 'utf8' }).trim();
    const kb = parseInt(out, 10);
    return Number.isFinite(kb) ? kb / 1024 : null;
  } catch { return null; }
}
const footprintMB = rssMB;
function vmmapSummary(pid) {
  try {
    const out = execFileSync('vmmap', ['--summary', pid], { encoding: 'utf8', maxBuffer: 64 * 1024 * 1024 });
    const grab = (re) => { const m = out.match(re); return m ? m[1].trim() : null; };
    return {
      physFootprint: grab(/Physical footprint:\s+([\d.]+\s*[KMG]?)/),
      physFootprintPeak: grab(/Physical footprint \(peak\):\s+([\d.]+\s*[KMG]?)/),
      raw: out.split('\n').slice(0, 60).join('\n'),
    };
  } catch (e) { return { error: e.message.split('\n')[0] }; }
}
function allWebContent() {
  const map = {};
  for (const pid of webContentPids()) {
    const f = rssMB(pid);
    if (f != null) map[pid] = +f.toFixed(1);
  }
  return map;
}
function bestWebContent() {
  const map = allWebContent();
  let best = { pid: null, footprintMB: 0 };
  for (const [pid, f] of Object.entries(map)) if (f > best.footprintMB) best = { pid, footprintMB: f };
  best.all = map;
  return best;
}

const UTF8_HELPER = `
  window.__u8 = function(ptr){
    if(!ptr) return '';
    const h = Module.HEAPU8; let end = ptr;
    while (h[end]) end++;
    return new TextDecoder().decode(new Uint8Array(h.subarray(ptr, end)));
  };`;

// One in-browser sample: everything readable from JS.
async function sample(s, phase) {
  const js = await wd.exec(s, `
    const out = { phase: ${JSON.stringify(phase)} };
    out.wasmBytes = Module.HEAPU8.byteLength;
    try { out.heap = JSON.parse(__u8(Module._eden_debug_heap())); } catch(e){ out.heap = String(e); }
    try { out.glbuf = JSON.parse(__u8(Module._eden_debug_gl_buffer_bytes())); } catch(e){ out.glbuf = String(e); }
    try {
      const cv = document.getElementById('eden-canvas');
      out.canvas = cv ? { w: cv.width, h: cv.height, px: cv.width * cv.height } : null;
    } catch(e){ out.canvas = String(e); }
    out.dpr = window.devicePixelRatio;
    try { out.jsHeap = performance.memory ? performance.memory.usedJSHeapSize : null; } catch(e){ out.jsHeap = null; }
    try { out.crossOriginIsolated = self.crossOriginIsolated; } catch(e){}
    return out;`);
  const wc = bestWebContent();
  js.webContent = wc;
  // vmmap is captured once at the end on the identified tab pid, not per-sample (it costs seconds).
  js._t = new Date().toISOString();
  return js;
}

async function main() {
  if (wantsQuit) {
    if (!fs.existsSync(SESSION_FILE)) { console.log('no session on file for port ' + PORT); return; }
    const s = fs.readFileSync(SESSION_FILE, 'utf8').trim();
    try { await wd.quit(s); } catch (e) { console.log('quit failed: ' + e.message); }
    fs.unlinkSync(SESSION_FILE);
    console.log('session torn down');
    return;
  }
  if (!URL) { console.error('usage: node tools/browser-memory-probe.js <url> [--label=x] [--port=4599] [--out=file.json] [--quit]'); process.exit(1); }

  const s = await getSession();
  await wd.go(s, URL);
  await req('POST', `/session/${s}/timeouts`, { script: 120000 });

  let ready = false;
  for (let i = 0; i < 60; i++) {
    if (await wd.exec(s, 'return !!(window.Module && Module.calledRun && Module._eden_debug_heap && Module._eden_debug_gl_buffer_bytes && Module._eden_menu_create_world);')) { ready = true; break; }
    await sleep(2000);
  }
  if (!ready) { console.log('RUNTIME NEVER READY (EDEN_DIAGNOSTICS -O2 build with the M0 exports at this URL?)'); process.exit(1); }
  await wd.exec(s, UTF8_HELPER + '\nreturn true;');

  const samples = [];
  const grab = async (phase) => { const x = await sample(s, phase); samples.push(x);
    console.log(`  [${phase}] wasm=${(x.wasmBytes/1048576).toFixed(0)}MB heapSize=${(x.heap.heapSize/1048576).toFixed(0)} sbrkPeak=${(x.heap.peakSbrkTop/1048576).toFixed(0)} glbuf=${(x.glbuf.bytes/1048576).toFixed(1)}MB/${x.glbuf.count} (peak ${(x.glbuf.peakBytes/1048576).toFixed(1)}/${x.glbuf.peakCount}, ${x.glbuf.creates}c/${x.glbuf.deletes}d) footprint=${x.webContent.footprintMB}MB`);
    return x; };

  await grab('post_runtime_init');

  // menu
  await sleep(1500);
  await grab('post_menu');

  // create + load bundled world (type 0)
  for (let i = 0; i < 30; i++) { if (await wd.exec(s, 'return Module._eden_menu_active()===1;')) break; await sleep(1000); }
  await wd.exec(s, `window.__idx = Module._eden_menu_create_world();
    Module._eden_menu_set_pending_world_type(0);
    return Module._eden_menu_play();`);
  let inWorld = false;
  for (let i = 0; i < 90; i++) {
    await sleep(1000);
    const gm = await wd.exec(s, 'try { return JSON.parse(__u8(Module._eden_debug_menu_state())).game_mode; } catch(e){ return -1; }');
    if (gm === 1) { inWorld = true; break; }
  }
  if (!inWorld) { console.log('NEVER REACHED IN-WORLD'); }
  await sleep(4000); // settle initial mesh + lighting
  await grab('post_world_load');

  // 30 s of frame loop with movement
  for (let i = 0; i < 15; i++) { await wd.exec(s, 'try{Module._eden_set_move_input(1.0,0.0,1.0);}catch(e){} return 1;'); await sleep(2000); }
  await wd.exec(s, 'try{Module._eden_set_move_input(0,0,1);}catch(e){} return 1;');
  await grab('pre_burst');

  // one teleport reload burst
  await wd.exec(s, `try { Module._eden_debug_gl_buffer_bytes_reset(); } catch(e){}
    try { Module._eden_console_teleport(64700, 40, 65700); } catch(e){ return String(e); }
    return 1;`);
  await sleep(1200);
  await grab('mid_burst');
  await sleep(4000);
  await grab('post_burst');

  // 30 s more
  await sleep(15000);
  await grab('steady_state');

  // save + quit to menu
  await wd.exec(s, `for (const w of [0,6]) { Module._eden_tap_hud_button_begin(w); }
    return 1;`);
  await sleep(400);
  await wd.exec(s, `for (const w of [0,6]) { Module._eden_tap_hud_button_end(w); } return 1;`);
  for (let i = 0; i < 20; i++) { await sleep(500); if (await wd.exec(s, 'return Module._eden_menu_active()===1;')) break; }
  await grab('post_quit');

  // reload from the save
  await wd.exec(s, `Module._eden_menu_select(window.__idx >= 0 ? window.__idx : 0);
    return Module._eden_menu_play();`);
  for (let i = 0; i < 60; i++) {
    await sleep(1000);
    const gm = await wd.exec(s, 'try { return JSON.parse(__u8(Module._eden_debug_menu_state())).game_mode; } catch(e){ return -1; }');
    if (gm === 1) break;
  }
  await sleep(4000);
  await grab('post_reload');

  // Identify the WebContent process hosting our tab: the one present across the run whose RSS
  // grew the most between the menu and the loaded world (our wasm heap goes 96->128MB there).
  const bySample = samples.map(x => (x.webContent && x.webContent.all) || {});
  const menuS = bySample[1] || {}, loadS = bySample[2] || {};
  // The tab hosting our wasm is the WebContent process with the largest RSS once the world is
  // loaded (wasm linear alone is 128-160MB; GPU/network helper WebContents stay under ~100MB).
  // Cross-check with growth from the menu phase.
  let tabPid = null, bestRss = -1;
  for (const pid of Object.keys(loadS)) {
    if (loadS[pid] > bestRss) { bestRss = loadS[pid]; tabPid = pid; }
  }
  const bestGrowth = tabPid ? bestRss - (menuS[tabPid] || 0) : 0;
  const tabSeries = bySample.map((m, i) => ({ phase: samples[i].phase, rssMB: tabPid ? (m[tabPid] ?? null) : null }));
  let tabVmmap = null;
  if (tabPid) tabVmmap = vmmapSummary(tabPid);

  const result = { label: LABEL, url: URL, when: new Date().toISOString(),
    tabPid, tabGrowthMB: +bestGrowth.toFixed(1), tabSeries, tabVmmap, samples };
  const outFile = outArg ? outArg.split('=')[1] : path.join(os.tmpdir(), `eden-mem-${LABEL}-${Date.now()}.json`);
  fs.writeFileSync(outFile, JSON.stringify(result, null, 1));
  console.log('\nwrote ' + outFile);

  // headline table
  const mb = (n) => (n / 1048576).toFixed(0);
  console.log(`\n${LABEL}  (tab WebContent pid ${tabPid}, grew ${bestGrowth.toFixed(1)}MB at world load):`);
  for (let i = 0; i < samples.length; i++) {
    const x = samples[i];
    console.log(`  ${x.phase.padEnd(20)} wasm ${mb(x.wasmBytes).padStart(4)}MB  sbrkPeak ${mb(x.heap.peakSbrkTop).padStart(4)}MB  glbuf ${(x.glbuf.peakBytes/1048576).toFixed(1).padStart(6)}MB/${String(x.glbuf.peakCount).padStart(4)}  tabRSS ${String(tabSeries[i].rssMB).padStart(6)}MB`);
  }
  if (tabVmmap) console.log(`  vmmap physFootprint ${tabVmmap.physFootprint} (peak ${tabVmmap.physFootprintPeak})\n--- vmmap --summary (head) ---\n${tabVmmap.raw || tabVmmap.error}`);
}
main().catch(e => { console.error('ERR', e.message); process.exit(1); });
