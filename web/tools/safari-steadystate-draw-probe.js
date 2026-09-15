#!/usr/bin/env node
// safari-steadystate-draw-probe.js — ROADMAP Phase B, step B2: measure the per-frame draw-call and
// GPU cost of NORMAL STEADY-STATE PLAY (walking around a already-meshed world), as opposed to the
// chunk-reload burst that B1 / the mesh-burst probe cover. This is the evidence B4 ("WebGL2 beyond
// ES1", roadmap row C2) needs: if fixed-function + per-draw-call overhead during ordinary play is
// already cheap, a GLSL/VAO rewrite buys little; if it is the frame's dominant cost, it is the win.
//
// Why a browser and not `node eden.js`: headless has no WebGL context, so eden_gl_glDrawArrays
// returns at its `!eden_gl_have_context()` guard BEFORE g_stats.draws++ — every _eden_gl_stat()
// counter is identically 0 under node. Real Safari over safaridriver with a REAL WebGL2 context is
// the only honest measurement. Modelled on tools/safari-objbatch-probe.js (same session reuse,
// same UTF8 helper, same gl-wrap trick) — that one builds a synthetic object scene on a FLAT
// world; this one walks a NORMAL world and touches no blocks.
//
// Build/serve (use build-relwdiag: -O2 like build-rel, EDEN_DIAGNOSTICS on for eden_console_*/
// eden_debug_* — build-st is -O0 and inflates every CPU number ~8x; see the mesh-burst probe's
// header). eden-host.js needs a `?build=relwdiag` branch while this runs (revert after):
//   emcmake cmake -B build-relwdiag -DCMAKE_BUILD_TYPE=Release -DEDEN_DIAGNOSTICS=ON -DEDEN_THREADED=OFF
//   cmake --build build-relwdiag -j8
//   safaridriver -p 4599 &
//   node tools/serve.js 8231
//   node tools/safari-steadystate-draw-probe.js 'http://localhost:8231/public/eden-st.html?build=relwdiag' [--walk]
//   node tools/safari-steadystate-draw-probe.js --quit
//
// Measured 2026-08-27 (see WORKING/STATUS.md §3): EXT_disjoint_timer_query_webgl2 is NOT exposed
// to a safaridriver-automated Safari, so gpuTimeElapsed_ms comes back unavailable — the rAF frame
// interval (CPU + GPU + vsync) is the fallback signal and it reads a flat vsync-locked 17 ms, so
// steady-state is not frame-bound on this hardware regardless. A real GPU-time number needs a
// manual Safari session with the query extension, or a Chrome pass.
'use strict';
const fs = require('fs');
const os = require('os');
const path = require('path');

const args = process.argv.slice(2);
const wantsQuit = args.includes('--quit');
const portArg = args.find(a => a.startsWith('--port='));
const sampleArg = args.find(a => a.startsWith('--samples='));
const PORT = portArg ? Number(portArg.split('=')[1]) : 4599;
const SAMPLES = sampleArg ? Number(sampleArg.split('=')[1]) : 90;
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
const median = (a) => { const b = a.slice().sort((x, y) => x - y); return b.length ? b[b.length >> 1] : 0; };
const pctl = (a, p) => { const b = a.slice().sort((x, y) => x - y); return b.length ? b[Math.min(b.length - 1, Math.floor(p / 100 * b.length))] : 0; };

const UTF8_HELPER = `
  window.__u8 = function(ptr){
    if(!ptr) return '';
    const h = Module.HEAPU8; let end = ptr;
    while (h[end]) end++;
    return new TextDecoder().decode(new Uint8Array(h.subarray(ptr, end)));
  };`;

async function main() {
  if (wantsQuit) {
    if (!fs.existsSync(SESSION_FILE)) { console.log('no session on file for port ' + PORT); return; }
    const s = fs.readFileSync(SESSION_FILE, 'utf8').trim();
    try { await wd.quit(s); } catch (e) { console.log('quit failed (session may already be dead): ' + e.message); }
    fs.unlinkSync(SESSION_FILE);
    console.log('session torn down');
    return;
  }
  if (!URL) { console.error('usage: node tools/safari-steadystate-draw-probe.js <url> [--port=4599] [--samples=90] [--quit]'); process.exit(1); }

  const s = await getSession();
  await wd.go(s, URL);
  await req('POST', `/session/${s}/timeouts`, { script: 120000 });

  let ready = false;
  for (let i = 0; i < 60; i++) {
    if (await wd.exec(s, 'return !!(window.Module && Module.calledRun && Module._eden_gl_stat && Module._eden_debug_mesh_timing);')) { ready = true; break; }
    await sleep(2000);
  }
  if (!ready) { console.log('RUNTIME NEVER READY (is this an EDEN_DIAGNOSTICS -O2 build served at this URL?)'); process.exit(1); }
  await wd.exec(s, UTF8_HELPER + '\nreturn true;');

  // --- get in-world: a NORMAL world (type 0), not flat ------------------------------------------
  await wd.exec(s, `
    const i = Module._eden_menu_create_world();
    Module._eden_menu_set_pending_world_type(0);
    return { idx: i, play: Module._eden_menu_play() };`);

  let inWorld = false;
  for (let i = 0; i < 45; i++) {
    await sleep(1000);
    const st = await wd.exec(s, 'return __u8(Module._eden_debug_menu_state());');
    try { if (JSON.parse(st).loading === 0) { inWorld = true; break; } } catch {}
  }
  if (!inWorld) { console.log('NEVER REACHED IN-WORLD'); process.exit(1); }
  await sleep(4000);   // let the initial full-window mesh + lighting settle

  const pos = JSON.parse(await wd.exec(s, 'return __u8(Module._eden_debug_player_state());'));

  // --- instrument the real WebGL2 context ------------------------------------------------------
  // Per-frame: raw drawArrays/drawElements count, bufferData/bufferSubData bytes (the shim
  // re-streams every client array per draw — doors/portals/flowers/breakage), and a GPU TIME_ELAPSED
  // query spanning the draw work of each frame (EXT_disjoint_timer_query_webgl2 where available —
  // Safari has shipped it since 16.4). getContext('webgl2') is safe under WebDriver execute/sync
  // (page's own main world) — see safari-objbatch-probe.js.
  await wd.exec(s, `
    const gl = document.getElementById('eden-canvas').getContext('webgl2');
    const ext = gl.getExtension('EXT_disjoint_timer_query_webgl2');
    const c = window.__glc = { frames: 0, draws: 0, bufData: 0, bufSub: 0, bytes: 0,
                               gpuMs: [], gpuSupported: !!ext, drawnThisFrame: 0, inFrame: false, q: null, pending: [] };
    const oa = gl.drawArrays.bind(gl), oe = gl.drawElements.bind(gl);
    const obd = gl.bufferData.bind(gl), obs = gl.bufferSubData.bind(gl);
    const span = (a, l) => (typeof a === 'number') ? a
                 : (l !== undefined && l !== null) ? l * (a.BYTES_PER_ELEMENT || 1)
                 : (a ? (a.byteLength || 0) : 0);
    function beginFrameQuery(){
      if (!ext || c.inFrame) return;
      c.q = gl.createQuery();
      gl.beginQuery(ext.TIME_ELAPSED_EXT, c.q);
      c.inFrame = true;
    }
    function endFrameQuery(){
      if (!ext || !c.inFrame) return;
      gl.endQuery(ext.TIME_ELAPSED_EXT);
      c.pending.push(c.q); c.q = null; c.inFrame = false;
      // drain any finished queries
      for (let i = c.pending.length - 1; i >= 0; i--) {
        const q = c.pending[i];
        if (gl.getQueryParameter(q, gl.QUERY_RESULT_AVAILABLE)) {
          const disjoint = gl.getParameter(ext.GPU_DISJOINT_EXT);
          if (!disjoint) c.gpuMs.push(gl.getQueryParameter(q, gl.QUERY_RESULT) / 1e6);
          gl.deleteQuery(q); c.pending.splice(i, 1);
        }
      }
    }
    gl.drawArrays = function(){ c.draws++; c.drawnThisFrame++; beginFrameQuery(); return oa.apply(null, arguments); };
    gl.drawElements = function(){ c.draws++; c.drawnThisFrame++; beginFrameQuery(); return oe.apply(null, arguments); };
    gl.bufferData = function(t,a,u,o,l){ c.bufData++; c.bytes += span(a,l); return obd.apply(null, arguments); };
    gl.bufferSubData = function(t,off,a,o,l){ c.bufSub++; c.bytes += span(a,l); return obs.apply(null, arguments); };
    const prev = Module.__edenFramePost;
    Module.__edenFramePost = function(){ endFrameQuery(); c.frames++; c.drawnThisFrame = 0; if (prev) return prev.apply(this, arguments); };
    return true;`);

  // --- optionally walk forward during the window ----------------------------------------------
  // --walk re-asserts eden_set_move_input every sample, because Player::processInput re-derives
  // the walk force from hud touch state every frame and stomps a one-shot call (web/CLAUDE.md:
  // "any hud-> input flag is re-derived from touches every frame"). Default (no --walk) measures
  // a STATIONARY observer in a fully-meshed normal world — which is the steady-state B2 wants,
  // free of the stream bursts that are the B1/mesh-burst regime.
  const walk = args.includes('--walk');

  const rows = [];
  await wd.exec(s, `window.__raf = { t: [], last: null };
    (function tick(ts){ const r = window.__raf; if (r.last!=null) r.t.push(ts - r.last); r.last = ts; requestAnimationFrame(tick); })();
    return true;`);

  for (let i = 0; i < SAMPLES; i++) {
    rows.push(await wd.exec(s, `${walk ? 'Module._eden_set_move_input(1.0,0.0,1.0);' : ''}
                               return [Module._eden_gl_stat(0), Module._eden_gl_stat(1),
                                       Module._eden_gl_stat(2), Module._eden_gl_stat(3),
                                       __u8(Module._eden_debug_mesh_timing())];`));
    await sleep(60);
  }

  await wd.exec(s, `try { Module._eden_set_move_input(0.0, 0.0, 1.0); } catch(e){}
    const r = window.__raf; window.__rafDeltas = r.t.slice(); return true;`);

  const rafT = await wd.exec(s, 'return window.__rafDeltas || [];');
  const c = await wd.exec(s, 'return { frames: __glc.frames, draws: __glc.draws, bufData: __glc.bufData, bufSub: __glc.bufSub, bytes: __glc.bytes, gpuMs: __glc.gpuMs, gpuSupported: __glc.gpuSupported };');

  const nz = rows.filter(r => r[0] > 0);
  if (!nz.length) { console.log('all samples read 0 draws — engine loop is not rendering'); process.exit(1); }

  // Separate "steady" samples (little/no meshing happening) from frames where a stream burst
  // overlapped, using the mesh-timing readout's meshCount delta between consecutive samples.
  let prevMesh = null;
  const steadyDraws = [], burstDraws = [];
  for (const r of nz) {
    let mc = 0; try { mc = JSON.parse(r[4]).meshCount || 0; } catch {}
    const delta = prevMesh == null ? 0 : mc - prevMesh; prevMesh = mc;
    (delta > 20 ? burstDraws : steadyDraws).push(r);
  }
  const use = steadyDraws.length >= 5 ? steadyDraws : nz;

  const f = Math.max(1, c.frames);
  const out = {
    build: URL,
    player: pos.pos.map(v => Math.round(v)),
    samples: `${nz.length}/${rows.length} nonzero, ${steadyDraws.length} steady / ${burstDraws.length} stream-burst`,
    shimPerFrame_steady: {
      drawCalls: median(use.map(r => r[0])),
      setupIssued: median(use.map(r => r[1])),
      setupElided: median(use.map(r => r[2])),
      setupPerDraw: +(median(use.map(r => r[1])) / median(use.map(r => r[0]))).toFixed(2),
      dirtyTrackingWin: +(median(use.map(r => r[3])) / Math.max(1, median(use.map(r => r[1])))).toFixed(2),
    },
    webglPerFrame_wholeWindow: {
      engineFrames: c.frames,
      rawDraws: +(c.draws / f).toFixed(1),
      bufferDataCalls: +(c.bufData / f).toFixed(1),
      bufferSubDataCalls: +(c.bufSub / f).toFixed(1),
      uploadKB: +(c.bytes / f / 1024).toFixed(1),
    },
    gpuTimeElapsed_ms: c.gpuSupported ? {
      samples: c.gpuMs.length,
      median: +median(c.gpuMs).toFixed(3),
      p90: +pctl(c.gpuMs, 90).toFixed(3),
      max: +Math.max(0, ...c.gpuMs).toFixed(3),
    } : 'EXT_disjoint_timer_query_webgl2 not available',
    rafFrameInterval_ms: {
      samples: rafT.length,
      median: +median(rafT).toFixed(2),
      p90: +pctl(rafT, 90).toFixed(2),
      max: +Math.max(0, ...rafT).toFixed(2),
    },
  };
  console.log(JSON.stringify(out, null, 1));
}

main().catch(e => { console.error('ERR', e.message); process.exit(1); });
