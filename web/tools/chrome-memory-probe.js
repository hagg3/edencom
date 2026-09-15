#!/usr/bin/env node
// chrome-memory-probe.js — ROADMAP Phase V / V6: the CHROME half of the memory measurement
// `tools/browser-memory-probe.js` only ever did on Safari.
//
// WHY A SECOND PROBE AND NOT A FLAG ON THE FIRST ONE. The Safari probe drives safaridriver
// (WebDriver) and gets its process numbers from `vmmap`, which is a WebKit-shaped answer: one
// WebContent process per tab, GL buffers off in com.apple.WebKit.GPU, and a `maximum` reservation
// that WebKit commits lazily. Chromium answers all three questions differently and answers them
// with different instruments — a renderer process per site, a single shared GPU process, and
// `performance.measureUserAgentSpecificMemory()`, which is the ONLY thing that attributes JS/wasm
// bytes to a specific REALM (window vs. each Worker). That attribution is the entire point of V6:
//
//   * M1 (`-sMAXIMUM_MEMORY` 512 MB) is NOT a measured win — capping moved Safari's footprint by
//     ~nothing, because WebKit reserves address space rather than committing it. The live
//     hypothesis is that Blink accounts a shared `WebAssembly.Memory`'s `maximum` differently, and
//     that is what the "desktop Chromium ≈ 1.9 GB" report (2026-08-30) that STARTED Phase M was.
//   * M2 (`-sPTHREAD_POOL_SIZE` 8 -> 4, aimed at ~55 MB of idle Worker realms) did not move Safari
//     physFootprint at all (V4). If M2 was real, it was real in Blink, and a per-realm breakdown
//     is what shows it.
//
// So: same scripted session as the Safari probe, phase for phase, so the two are comparable.
//
//   node tools/serve.js 8241                     # MUST be serve.js — COOP/COEP; measureUserAgent-
//                                                # SpecificMemory() only exists on an isolated page
//   node tools/chrome-memory-probe.js 'http://localhost:8241/public/eden-st.html?build=relthr' --label=thr
//
// Needs the diagnostics trees (`eden_debug_heap`, `eden_debug_gl_buffer_bytes`,
// `eden_console_teleport`), i.e. build-relwdiag / build-relthr and their -cap512 siblings — the
// DEPLOYED Release builds strip those exports, which is why this runs against a local serve.js and
// not against hagg3.github.io. Discard the FIRST run against a freshly built tree (cold .wasm).
//
// Drives Chromium over CDP directly rather than through WebDriver: no chromedriver in this
// checkout, and CDP is also what gives `SystemInfo.getProcessInfo` — the per-process pid/type list
// that turns `ps` output into "this is the renderer, that is the GPU process", i.e. the task
// manager reading V6 asks for. `--browser=<path>` overrides the binary; it defaults to whatever
// Chromium-family browser is installed (Helium on this machine, then Chrome/Chromium/Edge/Brave).
//
// **macOS will block the spawn unless the terminal has App Management permission** (2026-09-03: it
// raised a "Terminal was prevented from modifying apps" alert, which is what a shell launching an
// installed .app binary looks like to TCC). Two ways round it, and the second needs no permission
// grant at all:
//   * System Settings -> Privacy & Security -> App Management -> enable the terminal, then run as
//     documented above; or
//   * start the browser yourself and point the probe at it:
//       open -na Helium --args --remote-debugging-port=9333 --user-data-dir=/tmp/eden-probe-profile
//       node tools/chrome-memory-probe.js '<url>' --attach=9333 --label=thr
//     `--attach` skips the spawn, leaves the browser open at the end, and uses whatever profile it
//     was started with — so use a THROWAWAY `--user-data-dir` as above. A warm everyday profile
//     carries a service worker, a saved world and possibly a remembered `eden.lowmem` verdict, each
//     of which changes what this measures.
'use strict';

const { execSync, spawn } = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');

const args = process.argv.slice(2);
const URL_ARG = args.find((a) => !a.startsWith('--'));
const arg = (n, d) => { const h = args.find((a) => a.startsWith(`--${n}=`)); return h ? h.slice(n.length + 3) : d; };
const LABEL = arg('label', 'run');
const OUT = arg('out', null);
const ATTACH = parseInt(arg('attach', ''), 10) || null;   // attach to a browser already listening
const PORT = ATTACH || parseInt(arg('port', '9333'), 10);
const KEEP = args.includes('--keep-open') || !!ATTACH;    // never close a browser we did not start

const BROWSER_CANDIDATES = [
  '/Applications/Helium.app/Contents/MacOS/Helium',
  '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome',
  '/Applications/Chromium.app/Contents/MacOS/Chromium',
  '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge',
  '/Applications/Brave Browser.app/Contents/MacOS/Brave Browser',
];
const BROWSER = arg('browser', BROWSER_CANDIDATES.find((p) => fs.existsSync(p)));

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// --- CDP ---------------------------------------------------------------------------------------
// One websocket to the BROWSER target, flat sessions for the page. Node 22+ has a global
// WebSocket, so this needs no dependency — deliberately, like every other tool in here.
class CDP {
  constructor(ws) { this.ws = ws; this.id = 0; this.pending = new Map(); this.closed = false;
    ws.addEventListener('message', (ev) => {
      const msg = JSON.parse(ev.data);
      if (msg.id && this.pending.has(msg.id)) {
        const { resolve, reject } = this.pending.get(msg.id);
        this.pending.delete(msg.id);
        msg.error ? reject(new Error(msg.method + ': ' + JSON.stringify(msg.error))) : resolve(msg.result);
      }
    });
    ws.addEventListener('close', () => { this.closed = true; });
  }
  static async connect(url) {
    const ws = new WebSocket(url);
    await new Promise((res, rej) => {
      ws.addEventListener('open', res, { once: true });
      ws.addEventListener('error', () => rej(new Error('cdp: websocket failed: ' + url)), { once: true });
    });
    return new CDP(ws);
  }
  send(method, params, sessionId) {
    const id = ++this.id;
    return new Promise((resolve, reject) => {
      this.pending.set(id, { resolve, reject });
      this.ws.send(JSON.stringify(sessionId ? { id, method, params, sessionId } : { id, method, params }));
    });
  }
}

// Evaluate in the page and return the value. `awaitPromise` covers measureUserAgentSpecificMemory()
// and every `await`-shaped phase step below. A thrown page-side exception comes back as a string
// rather than killing the run — a probe that dies on one bad sample loses the whole session.
async function evalPage(cdp, sid, expr, ms = 120000) {
  const r = await Promise.race([
    cdp.send('Runtime.evaluate', {
      expression: `(async () => { ${expr} })()`,
      awaitPromise: true, returnByValue: true,
    }, sid),
    sleep(ms).then(() => ({ __timeout: true })),
  ]);
  if (r.__timeout) return { error: 'timeout after ' + ms + 'ms' };
  if (r.exceptionDetails) return { error: String(r.exceptionDetails.exception && r.exceptionDetails.exception.description || r.exceptionDetails.text) };
  return r.result && r.result.value;
}

// --- process memory ------------------------------------------------------------------------------
// The Chromium equivalent of the Safari probe's `vmmap`: SystemInfo.getProcessInfo names each pid,
// `ps` gives its RSS. Reported per TYPE, because the interesting split is renderer (wasm linear
// memory + JS) vs GPU process (where the ~22 MB of GL buffers actually lives, exactly as on
// WebKit) vs the browser process itself.
function rssKB(pid) {
  try { return parseInt(execSync(`ps -o rss= -p ${pid}`).toString().trim(), 10) || 0; } catch (e) { return 0; }
}
async function processMemory(cdp) {
  let info;
  try { info = await cdp.send('SystemInfo.getProcessInfo', {}); } catch (e) { return { error: String(e.message) }; }
  const byType = {}; const all = [];
  for (const p of info.processInfo || []) {
    const mb = +(rssKB(p.id) / 1024).toFixed(1);
    if (!mb) continue;                       // process has gone away between the two calls
    byType[p.type] = +((byType[p.type] || 0) + mb).toFixed(1);
    all.push({ pid: p.id, type: p.type, rssMB: mb });
  }
  const totalMB = +all.reduce((a, x) => a + x.rssMB, 0).toFixed(1);
  return { byType, totalMB, all };
}

// --- the sample ----------------------------------------------------------------------------------
// Deliberately the same field names as browser-memory-probe.js's sample() so the two runs can sit
// in one table, plus the two things only Blink can answer.
const UTF8 = `
  const __u8 = (ptr) => { const h = Module.HEAPU8; let e = ptr; while (h[e]) e++;
    return new TextDecoder().decode(new Uint8Array(h.subarray(ptr, e))); };`;

async function sample(cdp, sid, phase, deep) {
  const js = await evalPage(cdp, sid, `${UTF8}
    const out = { phase: ${JSON.stringify(phase)} };
    out.wasmBytes = Module.HEAPU8.byteLength;
    out.shared = Module.HEAPU8.buffer.constructor.name;
    try { out.heap = JSON.parse(__u8(Module._eden_debug_heap())); } catch(e){ out.heap = String(e); }
    try { out.glbuf = JSON.parse(__u8(Module._eden_debug_gl_buffer_bytes())); } catch(e){ out.glbuf = String(e); }
    const cv = document.getElementById('eden-canvas');
    out.canvas = cv ? { w: cv.width, h: cv.height } : null;
    out.dpr = window.devicePixelRatio;
    out.jsHeap = performance.memory ? performance.memory.usedJSHeapSize : null;
    out.crossOriginIsolated = self.crossOriginIsolated;
    // The realm breakdown — the reading this whole tool exists for. Only on an isolated page, and
    // it can take seconds (it waits for a GC), so it is opt-in per phase.
    if (${!!deep} && self.crossOriginIsolated && performance.measureUserAgentSpecificMemory) {
      try {
        const m = await performance.measureUserAgentSpecificMemory();
        out.uaMemBytes = m.bytes;
        const byScope = {}, byType = {};
        for (const b of m.breakdown) {
          for (const t of (b.types || [])) byType[t] = (byType[t] || 0) + b.bytes;
          const scope = (b.attribution && b.attribution[0] && (b.attribution[0].scope || b.attribution[0].url)) || 'unattributed';
          byScope[scope] = (byScope[scope] || 0) + b.bytes;
        }
        out.uaByType = byType; out.uaByScope = byScope;
      } catch (e) { out.uaMem = String(e); }
    }
    return out;`);
  const s = (js && !js.error) ? js : { phase, error: js && js.error };
  s.proc = await processMemory(cdp);
  s._t = new Date().toISOString();
  return s;
}

const mb = (b) => (b / 1048576).toFixed(0);

async function main() {
  if (!URL_ARG || (!BROWSER && !ATTACH)) {
    console.error('usage: node tools/chrome-memory-probe.js <url> [--label=x] [--attach=9333] [--browser=path] [--port=9333] [--out=f.json] [--keep-open]');
    if (!BROWSER) console.error('no Chromium-family browser found; pass --browser=<path> or start one yourself and use --attach=<port>');
    process.exit(1);
  }

  // A dedicated profile directory, always fresh: a warm profile carries a service worker, an
  // OPFS/IndexedDB copy of a world and a remembered `eden.lowmem` verdict, every one of which
  // changes what this measures. (Under --attach the caller owns the profile — see the header.)
  let child = null;
  if (!ATTACH) {
    const profile = fs.mkdtempSync(path.join(os.tmpdir(), 'eden-chrome-probe-'));
    child = spawn(BROWSER, [
      `--remote-debugging-port=${PORT}`, `--user-data-dir=${profile}`,
      '--no-first-run', '--no-default-browser-check', '--disable-background-timer-throttling',
      '--disable-backgrounding-occluded-windows', '--disable-renderer-backgrounding',
      'about:blank',
    ], { stdio: 'ignore', detached: false });
    console.log(`[chrome-probe] launched ${BROWSER} — profile ${profile}`);
  }

  let version = null;
  for (let i = 0; i < (ATTACH ? 6 : 60) && !version; i++) {
    await sleep(500);
    try { version = await (await fetch(`http://127.0.0.1:${PORT}/json/version`)).json(); } catch (e) { /* not up yet */ }
  }
  if (!version) {
    console.error(ATTACH
      ? `nothing is listening on 127.0.0.1:${PORT} — start the browser with --remote-debugging-port=${PORT} first (see this file's header)`
      : 'browser never opened a debugging port (macOS App Management permission? see this file\'s header)');
    if (child) child.kill();
    process.exit(1);
  }
  console.log(`[chrome-probe] connected to ${version.Browser}`);

  const cdp = await CDP.connect(version.webSocketDebuggerUrl);
  const { targetId } = await cdp.send('Target.createTarget', { url: 'about:blank' });
  const { sessionId: sid } = await cdp.send('Target.attachToTarget', { targetId, flatten: true });
  await cdp.send('Page.enable', {}, sid);
  await cdp.send('Runtime.enable', {}, sid);
  await cdp.send('Page.navigate', { url: URL_ARG }, sid);

  let ready = false;
  for (let i = 0; i < 90; i++) {
    await sleep(2000);
    const r = await evalPage(cdp, sid, 'return !!(window.Module && Module.calledRun && Module._eden_debug_heap && Module._eden_debug_gl_buffer_bytes && Module._eden_menu_create_world);', 10000);
    if (r === true) { ready = true; break; }
  }
  if (!ready) {
    console.log('RUNTIME NEVER READY — is this a diagnostics tree (build-relwdiag/build-relthr) and is serve.js sending COOP/COEP?');
    if (!KEEP) child.kill();
    process.exit(1);
  }

  const samples = [];
  const grab = async (phase, deep) => {
    const x = await sample(cdp, sid, phase, deep);
    samples.push(x);
    const p = x.proc || {};
    console.log(`  [${phase}] wasm=${mb(x.wasmBytes)}MB sbrkPeak=${x.heap ? mb(x.heap.peakSbrkTop) : '?'}MB ` +
      `glbuf=${x.glbuf ? (x.glbuf.bytes / 1048576).toFixed(1) : '?'}MB/${x.glbuf ? x.glbuf.count : '?'} ` +
      `js=${x.jsHeap ? mb(x.jsHeap) : '?'}MB ua=${x.uaMemBytes ? mb(x.uaMemBytes) + 'MB' : '-'} ` +
      `rss{${Object.entries(p.byType || {}).map(([k, v]) => k + ':' + v).join(' ')}} total=${p.totalMB}MB`);
    return x;
  };

  // Phase-for-phase with browser-memory-probe.js — the two are only comparable if they stay so.
  await grab('post_runtime_init', true);
  await sleep(1500);
  await grab('post_menu', true);

  for (let i = 0; i < 30; i++) { if (await evalPage(cdp, sid, 'return Module._eden_menu_active()===1;', 10000) === true) break; await sleep(1000); }
  await evalPage(cdp, sid, `window.__idx = Module._eden_menu_create_world();
    Module._eden_menu_set_pending_world_type(0);
    return Module._eden_menu_play();`);
  let inWorld = false;
  for (let i = 0; i < 90; i++) {
    await sleep(1000);
    const gm = await evalPage(cdp, sid, `${UTF8} try { return JSON.parse(__u8(Module._eden_debug_menu_state())).game_mode; } catch(e){ return -1; }`, 10000);
    if (gm === 1) { inWorld = true; break; }
  }
  if (!inWorld) console.log('NEVER REACHED IN-WORLD');
  await sleep(4000);
  await grab('post_world_load', true);

  for (let i = 0; i < 15; i++) { await evalPage(cdp, sid, 'try{Module._eden_set_move_input(1.0,0.0,1.0);}catch(e){} return 1;', 10000); await sleep(2000); }
  await evalPage(cdp, sid, 'try{Module._eden_set_move_input(0,0,1);}catch(e){} return 1;');
  await grab('pre_burst', true);

  await evalPage(cdp, sid, `try { Module._eden_debug_gl_buffer_bytes_reset(); } catch(e){}
    try { Module._eden_console_teleport(64700, 40, 65700); } catch(e){ return String(e); }
    return 1;`);
  await sleep(1200);
  await grab('mid_burst', false);
  await sleep(4000);
  await grab('post_burst', false);
  await sleep(15000);
  await grab('steady_state', true);

  await evalPage(cdp, sid, 'for (const w of [0,6]) { Module._eden_tap_hud_button_begin(w); } return 1;');
  await sleep(400);
  await evalPage(cdp, sid, 'for (const w of [0,6]) { Module._eden_tap_hud_button_end(w); } return 1;');
  for (let i = 0; i < 20; i++) { await sleep(500); if (await evalPage(cdp, sid, 'return Module._eden_menu_active()===1;', 10000) === true) break; }
  await grab('post_quit', true);

  await evalPage(cdp, sid, `Module._eden_menu_select(window.__idx >= 0 ? window.__idx : 0);
    return Module._eden_menu_play();`);
  for (let i = 0; i < 60; i++) {
    await sleep(1000);
    const gm = await evalPage(cdp, sid, `${UTF8} try { return JSON.parse(__u8(Module._eden_debug_menu_state())).game_mode; } catch(e){ return -1; }`, 10000);
    if (gm === 1) break;
  }
  await sleep(4000);
  await grab('post_reload', true);

  // --- report ---
  const peakTotal = Math.max(...samples.map((x) => (x.proc && x.proc.totalMB) || 0));
  const idle = samples.find((x) => x.phase === 'post_menu') || samples[0];
  console.log(`\n=== ${LABEL} — ${URL_ARG} ===`);
  console.log(`  isolated=${samples[0].crossOriginIsolated} wasmBuffer=${samples[0].shared}`);
  console.log('  phase                wasm  sbrkPeak  glbufPeak      jsHeap    uaMem   rssTotal');
  for (const x of samples) {
    console.log(`  ${x.phase.padEnd(18)} ${mb(x.wasmBytes).padStart(4)}MB ${(x.heap ? mb(x.heap.peakSbrkTop) : '?').padStart(7)}MB ` +
      `${(x.glbuf ? (x.glbuf.peakBytes / 1048576).toFixed(1) : '?').padStart(7)}MB/${String(x.glbuf ? x.glbuf.peakCount : '?').padStart(4)} ` +
      `${(x.jsHeap ? mb(x.jsHeap) : '?').padStart(6)}MB ${(x.uaMemBytes ? mb(x.uaMemBytes) : '-').padStart(6)}MB ` +
      `${String((x.proc && x.proc.totalMB) || '?').padStart(8)}MB`);
  }
  console.log(`  browser-wide RSS: idle(menu) ${idle.proc && idle.proc.totalMB}MB  peak ${peakTotal}MB`);
  const deep = samples.filter((x) => x.uaByScope);
  if (deep.length) {
    const last = deep[deep.length - 1];
    console.log(`  measureUserAgentSpecificMemory by realm (${last.phase}):`);
    for (const [k, v] of Object.entries(last.uaByScope).sort((a, b) => b[1] - a[1])) {
      console.log(`    ${mb(v).padStart(5)}MB  ${k}`);
    }
  }

  const out = OUT || path.join(os.tmpdir(), `eden-chrome-mem-${LABEL}.json`);
  fs.writeFileSync(out, JSON.stringify({ label: LABEL, url: URL_ARG, browser: version.Browser, samples }, null, 2));
  console.log('  wrote ' + out);

  if (!KEEP) { try { await cdp.send('Browser.close', {}); } catch (e) { child.kill(); } }

  // The CDP websocket (and, without --attach, the browser child) keep the event loop alive, so a
  // bare return here hangs the process forever after the report is written — which is exactly what
  // happened the first time this tool was run for real (V6, 2026-09-04). Close out explicitly.
  try { cdp.ws.close(); } catch (e) { /* already gone */ }
  process.exit(0);
}

main().catch((e) => { console.error(e); process.exit(1); });
