#!/usr/bin/env node
// safari-objbatch-probe.js — measures what Terrain.mm's per-frame object batches (doors, golden
// cubes, portal frames, portal swirls, flowers) actually cost the GL shim, in a REAL browser with a
// REAL WebGL2 context. This is the standing evidence behind audit row 23/E3.
//
// Why a browser and not `node eden.js`: headless has no canvas, so eden_gl_glDrawArrays returns at
// its `!eden_gl_have_context()` guard BEFORE `g_stats.draws++`. Every _eden_gl_stat() counter is
// therefore identically 0 under node, and no headless suite can measure this. Real Safari over
// safaridriver is the cheapest honest measurement available.
//
// What it does: creates a flat world, plays it, builds a deterministic test scene around the player
// out of doors / golden cubes / portals / flowers via the EDEN_DIAGNOSTICS `eden_console_setblock`
// export, then samples _eden_gl_stat(0..3) over a wall-clock window and reports per-frame medians.
// It also counts raw WebGL drawArrays/drawElements calls as a cross-check on stat(0).
//
// Requires Safari's Develop -> Allow Remote Automation, safaridriver running, and a server:
//   safaridriver -p 4599 &
//   node tools/serve.js 8231
//   node tools/safari-objbatch-probe.js 'http://localhost:8231/public/eden-st.html?build=st'
//   node tools/safari-objbatch-probe.js --quit        # release the reused Safari session
//
// Session reuse/teardown is deliberately identical to tools/safari-frame-check.js (Safari pairs
// with one WebDriver session at a time; the state file is keyed by driver port).

'use strict';
const fs = require('fs');
const os = require('os');
const path = require('path');

const args = process.argv.slice(2);
const wantsQuit = args.includes('--quit');
const noScene = args.includes('--no-scene');   // measure an empty flat world (control)
const portArg = args.find(a => a.startsWith('--port='));
const sampleArg = args.find(a => a.startsWith('--samples='));
const PORT = portArg ? Number(portArg.split('=')[1]) : 4599;
const SAMPLES = sampleArg ? Number(sampleArg.split('=')[1]) : 40;
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

// Read a C string the export-independent, shared-memory-correct way (web/CLAUDE.md: never touch a
// Module runtime method that isn't in EXPORTED_RUNTIME_METHODS — it abort()s the engine from a read).
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
  if (!URL) { console.error("usage: node tools/safari-objbatch-probe.js <url> [--port=4599] [--samples=40] [--no-scene] [--quit]"); process.exit(1); }

  const s = await getSession();
  await wd.go(s, URL);
  await req('POST', `/session/${s}/timeouts`, { script: 120000 });

  let ready = false;
  for (let i = 0; i < 60; i++) {
    if (await wd.exec(s, 'return !!(window.Module && Module.calledRun && Module._eden_gl_stat);')) { ready = true; break; }
    await sleep(2000);
  }
  if (!ready) { console.log('RUNTIME NEVER READY (is this an EDEN_DIAGNOSTICS build-st?)'); process.exit(1); }
  await wd.exec(s, UTF8_HELPER + '\nreturn true;');

  // --- get in-world -------------------------------------------------------------------------
  await wd.exec(s, `
    const i = Module._eden_menu_create_world();
    Module._eden_menu_set_pending_world_type(1);   // flat: deterministic, no worldgen variance
    return { idx: i, play: Module._eden_menu_play() };`);

  let inWorld = false;
  for (let i = 0; i < 45; i++) {
    await sleep(1000);
    const st = await wd.exec(s, 'return __u8(Module._eden_debug_menu_state());');
    try { if (JSON.parse(st).loading === 0) { inWorld = true; break; } } catch {}
  }
  if (!inWorld) { console.log('NEVER REACHED IN-WORLD'); process.exit(1); }
  await sleep(2500);   // let the first full window mesh settle

  const pos = JSON.parse(await wd.exec(s, 'return __u8(Module._eden_debug_player_state());'));
  const px = Math.round(pos.pos[0]), py = Math.round(pos.pos[1]), pz = Math.round(pos.pos[2]);

  // --- build the test scene ------------------------------------------------------------------
  // Terrain APIs are (x, z, y) with y vertical, and eden_console_setblock keeps that order.
  // Doors and portals are two blocks: a base type plus the *_TOP the mesher extracts as a
  // StaticObject. Everything is placed inside the resident window, in front of the spawn.
  let scene = { doors: 0, cubes: 0, portals: 0, flowers: 0 };
  if (!noScene) {
    scene = await wd.exec(s, `
      const T = { DOOR1: 66, DOOR_TOP: 70, GOLDEN: 71, FLOWER: 73, PORTAL1: 75, PORTAL_TOP: 79 };
      const px = ${px}, py = ${py}, pz = ${pz};
      const n = { doors: 0, cubes: 0, portals: 0, flowers: 0 };
      for (let i = 0; i < 24; i++) {
        const x = px - 12 + i, z = pz + 6;
        Module._eden_console_setblock(x, z, py,     T.DOOR1);
        Module._eden_console_setblock(x, z, py + 1, T.DOOR_TOP);   n.doors++;
      }
      for (let i = 0; i < 24; i++) {
        const x = px - 12 + i, z = pz + 9;
        Module._eden_console_setblock(x, z, py,     T.PORTAL1);
        Module._eden_console_setblock(x, z, py + 1, T.PORTAL_TOP); n.portals++;
      }
      for (let i = 0; i < 24; i++) {
        Module._eden_console_setblock(px - 12 + i, pz + 3, py + 1, T.GOLDEN);  n.cubes++;
      }
      for (let i = 0; i < 24; i++) for (let j = 0; j < 6; j++) {
        Module._eden_console_setblock(px - 12 + i, pz + 12 + j, py, T.FLOWER); n.flowers++;
      }
      return n;`);
    await sleep(2500);   // remeshing + StaticObject re-extraction
  }

  // --- instrument the real WebGL2 context -----------------------------------------------------
  // Counting glBufferData/glBufferSubData BYTES is what actually shows this row's cost: the shim
  // re-uploads every client-side array into a streaming VBO on every draw, so an object batch pays
  // its vertex span once per enabled attribute per frame. Draw counts alone hide that entirely.
  //
  // getContext('webgl2') is safe HERE and only here: WebDriver's execute/sync runs in the page's
  // own main world, so it returns the engine's live context (tools/safari-frame-check.js has
  // depended on this since pass 57). The identical call from the Chrome extension's isolated world
  // returns a useless wrapper and can kill the context — see web/CLAUDE.md.
  await wd.exec(s, `
    const gl = document.getElementById('eden-canvas').getContext('webgl2');
    window.__glc = { draws: 0, bufData: 0, bufSub: 0, bytes: 0, frames: 0 };
    const c = window.__glc;
    const oa = gl.drawArrays.bind(gl), oe = gl.drawElements.bind(gl);
    const obd = gl.bufferData.bind(gl), obs = gl.bufferSubData.bind(gl);
    gl.drawArrays = function(){ c.draws++; return oa.apply(null, arguments); };
    gl.drawElements = function(){ c.draws++; return oe.apply(null, arguments); };
    // Emscripten's GL bindings use the WebGL2 5-argument overloads and pass the WHOLE heap view
    // (HEAPU8/HEAPF32) with srcOffset/length in ELEMENTS — so srcData.byteLength is the entire wasm
    // heap, not the upload. Measure 'length', in that view's element size.
    const span = (a, l) => (typeof a === 'number') ? a
                 : (l !== undefined && l !== null) ? l * (a.BYTES_PER_ELEMENT || 1)
                 : (a ? (a.byteLength || 0) : 0);
    gl.bufferData = function(t, a, u, o, l){
      c.bufData++; c.bytes += span(a, l);
      return obd.apply(null, arguments);
    };
    gl.bufferSubData = function(t, off, a, o, l){
      c.bufSub++; c.bytes += span(a, l);
      return obs.apply(null, arguments);
    };
    const prev = Module.__edenFramePost;
    Module.__edenFramePost = function(){ c.frames++; if (prev) return prev.apply(this, arguments); };
    return true;`);

  // --- sample ---------------------------------------------------------------------------------
  // stat(0..3) = draws / setup issued / setup elided / issued+elided, for the LAST COMPLETED frame.
  // Sampled on wall-clock timers: never _eden_debug_tick a live rAF-driven tab (web/CLAUDE.md).
  const rows = [];
  for (let i = 0; i < SAMPLES; i++) {
    rows.push(await wd.exec(s, `return [Module._eden_gl_stat(0), Module._eden_gl_stat(1),
                                       Module._eden_gl_stat(2), Module._eden_gl_stat(3)];`));
    await sleep(60);
  }
  const nonzero = rows.filter(r => r[0] > 0);
  if (!nonzero.length) { console.log('all samples read 0 draws — engine loop is not rendering'); process.exit(1); }

  const draws = median(nonzero.map(r => r[0]));
  const issued = median(nonzero.map(r => r[1]));
  const elided = median(nonzero.map(r => r[2]));
  const total = median(nonzero.map(r => r[3]));
  const c = await wd.exec(s, 'return window.__glc;');
  const f = Math.max(1, c.frames);

  console.log(JSON.stringify({
    scene: noScene ? 'empty flat world (control)' : scene,
    player: [px, py, pz],
    samples: nonzero.length + '/' + rows.length,
    shimPerFrameMedian: { draws, setupIssued: issued, setupElided: elided, setupTotal: total },
    setupPerDraw: +(issued / draws).toFixed(2),
    dirtyTrackingWin: +(total / issued).toFixed(2),
    webglPerFrame: {
      engineFrames: c.frames,
      draws: +(c.draws / f).toFixed(1),
      bufferDataCalls: +(c.bufData / f).toFixed(1),
      bufferSubDataCalls: +(c.bufSub / f).toFixed(1),
      uploadKB: +(c.bytes / f / 1024).toFixed(1),
    },
    uploadMBps: +(c.bytes / f / 1024 / 1024 * 60).toFixed(2),
  }, null, 1));
}

main().catch(e => { console.error('ERR', e.message); process.exit(1); });
