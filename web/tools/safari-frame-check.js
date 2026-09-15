#!/usr/bin/env node
// safari-frame-check.js — "did the engine's frame loop actually start?", answered from outside
// the page. Grew out of pass 57's black-canvas bug: the DOM (loading overlay, JS menu, hotbar,
// eden-st.html's own rAF watchdog) all kept working over a WebGL2 context that never received a
// single GL call, because main() died before reaching emscripten_set_main_loop. Counting rAF
// frames or eyeballing the canvas cannot catch that class of bug — only counting ENGINE frames
// (Module.__edenFramePost, the per-frame hook audit row A8 unified everything onto) can. This is
// genuinely useful beyond that one bug: it's the only tool that answers "did main()'s loop
// register" for any future browser-only failure mode.
//
// Requires Safari's Develop -> Allow Remote Automation, and safaridriver running:
//   safaridriver -p 4599 &
//
// Usage:
//   node tools/safari-frame-check.js <url> [--port=4599] [--quit] [--wait-ms=3000]
//
//   node tools/safari-frame-check.js 'http://localhost:8123/public/eden-st.html?build=st'
//   node tools/safari-frame-check.js --quit            # tear down the reused session, no URL needed
//
// Session reuse: a session is expensive to spin up (real Safari launch), so one is kept alive
// across invocations in a state file under os.tmpdir(), keyed by driver port -- repeated runs
// against the same safaridriver reuse it instead of relaunching Safari each time. A dead/expired
// session is detected (a no-op script call fails) and silently replaced with a fresh one. Nothing
// here starts safaridriver itself: it holds a real Safari process, so this script only drives an
// already-running driver, and requires one to be told to actually quit it (--quit) rather than
// leaking a Safari window across an unattended chain of invocations.

'use strict';
const fs = require('fs');
const os = require('os');
const path = require('path');

const args = process.argv.slice(2);
const wantsQuit = args.includes('--quit');
const portArg = args.find(a => a.startsWith('--port='));
const waitArg = args.find(a => a.startsWith('--wait-ms='));
const PORT = portArg ? Number(portArg.split('=')[1]) : 4599;
const WAIT_MS = waitArg ? Number(waitArg.split('=')[1]) : 3000;
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
  exec: (s, script, args = []) => req('POST', `/session/${s}/execute/sync`, { script, args }),
  quit: (s) => req('DELETE', `/session/${s}`),
};

async function getSession() {
  let s;
  try {
    s = fs.readFileSync(SESSION_FILE, 'utf8').trim();
    await wd.exec(s, 'return 1;'); // dies loudly if the session expired or Safari was closed
  } catch {
    s = await wd.newSession();
    fs.writeFileSync(SESSION_FILE, s);
  }
  return s;
}

async function main() {
  if (wantsQuit) {
    if (!fs.existsSync(SESSION_FILE)) { console.log('no session on file for port ' + PORT); return; }
    const s = fs.readFileSync(SESSION_FILE, 'utf8').trim();
    try { await wd.quit(s); } catch (e) { console.log('quit failed (session may already be dead): ' + e.message); }
    fs.unlinkSync(SESSION_FILE);
    console.log('session torn down');
    return;
  }
  if (!URL) { console.error('usage: node tools/safari-frame-check.js <url> [--port=4599] [--quit]'); process.exit(1); }

  const s = await getSession();
  await wd.go(s, URL);
  await req('POST', `/session/${s}/timeouts`, { script: 120000 });

  let ready = false;
  for (let i = 0; i < 60; i++) {
    const r = await wd.exec(s, 'return !!(window.Module && Module.calledRun);');
    if (r) { ready = true; break; }
    await new Promise(r => setTimeout(r, 2000));
  }
  if (!ready) { console.log('RUNTIME NEVER READY'); return; }

  // Instrument engine frames (not rAF frames) and raw GL draw/clear calls on the live context.
  await wd.exec(s, `
    const prev = Module.__edenFramePost; window.__fp = 0;
    Module.__edenFramePost = function(){ window.__fp++; if (prev) return prev.apply(this, arguments); };
    const gl = document.getElementById('eden-canvas').getContext('webgl2');
    window.__gl = {draws:0, clears:0};
    const od = gl.drawElements.bind(gl), oa = gl.drawArrays.bind(gl), oc = gl.clear.bind(gl);
    gl.drawElements = function(){ window.__gl.draws++; return od.apply(null, arguments); };
    gl.drawArrays  = function(){ window.__gl.draws++; return oa.apply(null, arguments); };
    gl.clear       = function(){ window.__gl.clears++; return oc.apply(null, arguments); };
    return true;
  `);
  await new Promise(r => setTimeout(r, WAIT_MS));
  const out = await wd.exec(s, `return {
    engineFrames: window.__fp, gl: window.__gl,
    worldFS: Module.EdenWorldFS ? {mode: Module.EdenWorldFS.mode, size: Module.EdenWorldFS.size, degraded: !!Module.EdenWorldFS.degraded} : null,
    status: (document.getElementById('eden-status')||{}).textContent };`);
  console.log(JSON.stringify(out, null, 1));
  if (out.engineFrames === 0) {
    console.log('\nFAIL: engine frame loop never registered (main() likely died before emscripten_set_main_loop).');
    process.exitCode = 1;
  }
}

main().catch(e => { console.error('ERR', e.message); process.exit(1); });
