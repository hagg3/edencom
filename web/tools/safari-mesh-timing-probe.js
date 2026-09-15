#!/usr/bin/env node
// safari-mesh-timing-probe.js — the real-GL-context half of the row 36/C1 measurement question.
// node headless-mesh-timing-probe.js gives an honest mesh-CPU number (pure C++, no GL) but its
// upload number is worthless: eden_gl_glBufferData/SubData return at their !eden_gl_have_context()
// guard under node (no canvas), so TerrainChunk::prepareVBO()'s wrapped time is measuring "how long
// it takes to no-op" rather than a real upload — same class of gap as safari-objbatch-probe.js's
// reason for existing.
//
// What it does: creates a world in a REAL Safari tab (real WebGL2 context), walks forward for a
// wall-clock window the same way headless-mesh-timing-probe.js does (eden_set_move_input, forcing
// sustained streaming/remeshing), then reads src/seam/MeshTiming_web.mm's
// eden_debug_mesh_timing() — same export, same --wrap, only the upload half is now measuring a
// real GL driver instead of a stub.
//
// Requires Safari's Develop -> Allow Remote Automation, safaridriver running, and a server:
//   safaridriver -p 4599 &
//   node tools/serve.js 8231
//   node tools/safari-mesh-timing-probe.js 'http://localhost:8231/public/eden-st.html?build=rel' --seconds=20 --speed=8
//   node tools/safari-mesh-timing-probe.js --quit
'use strict';
const fs = require('fs');
const os = require('os');
const path = require('path');

const args = process.argv.slice(2);
const wantsQuit = args.includes('--quit');
const portArg = args.find(a => a.startsWith('--port='));
const secArg = args.find(a => a.startsWith('--seconds='));
const speedArg = args.find(a => a.startsWith('--speed='));
const PORT = portArg ? Number(portArg.split('=')[1]) : 4599;
const SECONDS = secArg ? Number(secArg.split('=')[1]) : 20;
const SPEED = speedArg ? Number(speedArg.split('=')[1]) : 8;
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
  if (!URL) { console.error("usage: node tools/safari-mesh-timing-probe.js <url> [--port=4599] [--seconds=20] [--speed=8] [--quit]"); process.exit(1); }

  const s = await getSession();
  await wd.go(s, URL);
  await req('POST', `/session/${s}/timeouts`, { script: 120000 });

  let ready = false;
  for (let i = 0; i < 60; i++) {
    if (await wd.exec(s, 'return !!(window.Module && Module.calledRun && Module._eden_debug_mesh_timing);')) { ready = true; break; }
    await sleep(2000);
  }
  if (!ready) { console.log('RUNTIME NEVER READY (or eden_debug_mesh_timing not linked — rebuild)'); process.exit(1); }
  await wd.exec(s, UTF8_HELPER + '\nreturn true;');

  await wd.exec(s, `
    const i = Module._eden_menu_create_world();
    Module._eden_menu_set_pending_world_type(0);   // normal: streams the bundled world
    return { idx: i, play: Module._eden_menu_play() };`);

  let inWorld = false;
  for (let i = 0; i < 45; i++) {
    await sleep(1000);
    const active = await wd.exec(s, 'return Module._eden_menu_active();');
    if (active === 0) { inWorld = true; break; }
  }
  if (!inWorld) { console.log('NEVER REACHED IN-WORLD'); process.exit(1); }

  const framesBefore = await wd.exec(s, `
    window.__frames = 0;
    const prev = Module.__edenFramePost;
    Module.__edenFramePost = function(){ window.__frames++; if (prev) return prev.apply(this, arguments); };
    return window.__frames;`);

  // Force Keyboard+Mouse (input_mode=2) — Auto-detect never saw a real touch/mouse event in this
  // scripted session, and if it resolves to the Touch profile, Classes/Joystick.mm's "no active
  // touch" branch calls setSpeed(zero,0) every frame AFTER eden_set_move_input's call in the same
  // Hud::update-before-Player::preupdate ordering (Input_web.mm's own header comment documents
  // this exact stomp), silently cancelling movement — confirmed live: without this, 20s of
  // eden_set_move_input produced zero streaming and zero mesh calls.
  await wd.exec(s, `
    const schema = JSON.parse(__u8(Module._eden_settings_schema()));
    const idx = schema.findIndex(x => x.key === 'input_mode');
    if (idx >= 0) Module._eden_settings_set(idx, 2);
    return idx;`);

  await sleep(2500); // let the initial-load churn settle, same as the headless version
  await wd.exec(s, 'Module._eden_debug_mesh_timing_reset(); window.__frames = 0; return true;');

  console.log(`Walking forward at speed=${SPEED} for ${SECONDS}s real time in real Safari (${URL})...`);
  // Player::setSpeed's effect decays fast (Classes/Player.mm's per-frame ~0.9x ground damping —
  // see src/seam/Movement_web.mm's header), so a sparse reassertion (originally 1/s here) barely
  // moves the player between calls: measured 0.38 units from one call, then nothing until the
  // next. The real input path (public/eden-input.js) reasserts every rAF frame; a WebDriver round
  // trip can't go that fast, but every ~150ms keeps velocity from bottoming out between calls —
  // confirmed by comparing against the headless probe's 200ms interval, which DID produce sustained
  // movement and real streaming.
  const t0 = Date.now();
  let elapsed = 0;
  while (elapsed < SECONDS * 1000) {
    await wd.exec(s, `Module._eden_set_move_input(1, 0, ${SPEED}); return true;`);
    await sleep(150);
    elapsed = Date.now() - t0;
  }
  await wd.exec(s, 'Module._eden_set_move_input(0, 0, 0); return true;');
  const wallMs = Date.now() - t0;

  const timing = JSON.parse(await wd.exec(s, 'return __u8(Module._eden_debug_mesh_timing());'));
  const frames = await wd.exec(s, 'return window.__frames;');

  const meshShare = timing.meshMs / (timing.meshMs + timing.uploadMs || 1);
  const report = {
    engine: 'real Safari, real WebGL2 context',
    wall_ms: wallMs,
    engine_frames: frames,
    fps: +(frames / (wallMs / 1000)).toFixed(1),
    mesh: {
      calls: timing.meshCount,
      total_ms: +timing.meshMs.toFixed(2),
      avg_ms_per_chunk: timing.meshCount ? +(timing.meshMs / timing.meshCount).toFixed(4) : 0,
      max_ms_single_chunk: timing.meshMsMax,
      ms_per_second_wall: +(timing.meshMs / (wallMs / 1000)).toFixed(2),
    },
    upload: {
      calls: timing.uploadCount,
      total_ms: +timing.uploadMs.toFixed(2),
      avg_ms_per_chunk: timing.uploadCount ? +(timing.uploadMs / timing.uploadCount).toFixed(4) : 0,
      max_ms_single_chunk: timing.uploadMsMax,
      ms_per_second_wall: +(timing.uploadMs / (wallMs / 1000)).toFixed(2),
    },
    mesh_share_pct: +(meshShare * 100).toFixed(1),
    upload_share_pct: +((1 - meshShare) * 100).toFixed(1),
  };
  console.log(JSON.stringify(report, null, 2));
}

main().catch(e => { console.error('ERR', e.message); process.exit(1); });
