// One-off live-browser check (ROADMAP.md Tier 0): does the frame-budgeted bulk-chunk-reload
// (Terrain.mm's BULK_RELOAD_CHUNK_BUDGET, teleport/warp/fast-walk path) actually *feel* okay in a
// real browser, not just pass headless timing? Reuses safari-frame-check.js's safaridriver session
// plumbing. Drives a real teleport and records the browser's own rAF frame deltas around it (what
// the player's eyes actually see, screen-paint-to-screen-paint) rather than re-measuring the
// engine's internal main-thread-block number headless-mesh-burst-probe.js already covers.
//
// Requires safaridriver -p 4599 & and node tools/serve.js <port> running from web/.
// Usage: node tools/safari-chunk-reload-feel-check.js 'http://localhost:8123/public/eden-st.html'
'use strict';
const fs = require('fs');
const os = require('os');
const path = require('path');

const PORT = 4599;
const URL = process.argv[2];
const BASE = `http://localhost:${PORT}`;
const SESSION_FILE = path.join(os.tmpdir(), `eden-safari-session-${PORT}.txt`);

async function req(method, p, body) {
  const r = await fetch(BASE + p, {
    method, headers: { 'Content-Type': 'application/json' },
    body: body === undefined ? undefined : JSON.stringify(body),
  });
  const j = await r.json();
  if (j.value && j.value.error) throw new Error(j.value.error + ': ' + j.value.message);
  return j.value;
}
const wd = {
  newSession: () => req('POST', '/session', { capabilities: { alwaysMatch: { browserName: 'safari' } } }).then(v => v.sessionId),
  go: (s, url) => req('POST', `/session/${s}/url`, { url }),
  exec: (s, script, args = []) => req('POST', `/session/${s}/execute/sync`, { script, args }),
  execAsync: (s, script, args = []) => req('POST', `/session/${s}/execute/async`, { script, args }),
};
async function getSession() {
  let s;
  try { s = fs.readFileSync(SESSION_FILE, 'utf8').trim(); await wd.exec(s, 'return 1;'); }
  catch { s = await wd.newSession(); fs.writeFileSync(SESSION_FILE, s); }
  return s;
}

async function main() {
  if (!URL) { console.error('usage: node tools/safari-chunk-reload-feel-check.js <url>'); process.exit(1); }
  const s = await getSession();
  await wd.go(s, URL);
  await req('POST', `/session/${s}/timeouts`, { script: 120000 });

  let ready = false;
  for (let i = 0; i < 60; i++) {
    if (await wd.exec(s, 'return !!(window.Module && Module.calledRun);')) { ready = true; break; }
    await new Promise(r => setTimeout(r, 1000));
  }
  if (!ready) { console.log('RUNTIME NEVER READY'); process.exit(1); }

  // Create + enter a fresh world (mirrors headless-menu-flow-test.js's exports), let it settle.
  const enter = await wd.execAsync(s, `
    const done = arguments[0];
    (async () => {
      const inMenu = () => Module._eden_menu_active() === 1;
      const wait = (pred, ms) => new Promise((res) => {
        const t0 = Date.now();
        (function poll(){ if (pred()) return res(true); if (Date.now()-t0>ms) return res(false); setTimeout(poll, 50); })();
      });
      if (!(await wait(inMenu, 15000))) return done('menu never came up');
      Module._eden_menu_create_world();
      Module._eden_menu_set_pending_world_type(0);
      Module._eden_menu_play();
      if (!(await wait(() => !inMenu(), 30000))) return done('never reached PLAY');
      await new Promise(r => setTimeout(r, 2500));
      done('ok');
    })();
  `);
  console.log('enter world:', enter);
  if (enter !== 'ok') process.exit(1);

  // Hook rAF to record real screen-paint-to-screen-paint deltas, teleport, then read them back.
  await wd.exec(s, `
    window.__deltas = [];
    window.__last = performance.now();
    window.__raf = (window.__raf || 0);
    function tick(t) { window.__deltas.push(t - window.__last); window.__last = t; requestAnimationFrame(tick); }
    requestAnimationFrame(tick);
    return true;
  `);
  await new Promise(r => setTimeout(r, 500));
  await wd.exec(s, `window.__deltas = []; Module._eden_console_teleport(64700, 40, 65700); return true;`);
  await new Promise(r => setTimeout(r, 2500));
  const deltas = await wd.exec(s, `return window.__deltas;`);

  const sorted = [...deltas].sort((a, b) => b - a);
  const over16 = deltas.filter((d) => d > 16.66).length;
  const over33 = deltas.filter((d) => d > 33.3).length; // dropped-below-30fps frame
  console.log(JSON.stringify({
    frames: deltas.length,
    worst_ms: sorted[0],
    top5_ms: sorted.slice(0, 5),
    frames_over_16ms: over16,
    frames_over_33ms: over33,
  }, null, 2));
}
main().catch((e) => { console.error('ERR', e.message); process.exit(1); });
