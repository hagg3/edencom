// One-shot live-browser characterization of the perceived-stutter gap during a bulk chunk reload
// (ROADMAP.md Tier 0 follow-up / STATUS.md §3 item 2). The feel-check established the gap exists
// (real Safari worst-frame 134ms vs. ~30ms headless main-thread block); this run produces the
// number that decides Tier 2: is the extra cost (a) GL upload / browser compositing outside what
// headless-mesh-burst-probe.js measures, or (b) fill-order not smoothing perceived motion.
//
// Method: same teleport targets as headless-mesh-burst-probe.js, but in real Safari against
// build-relwdiag (real WebGL driver + real compositor). Per teleport we read BOTH:
//   - the browser's own rAF frame deltas (what the eye sees)
//   - eden_debug_mesh_timing() (real-driver mesh CPU / GL upload / column-read ms, reset per burst)
// so mesh+upload+read can be subtracted from the observed block. Whatever is left is compositing /
// GPU-pipeline stall not attributable to engine CPU.
//
// Requires: safaridriver -p 4599 &   and   node tools/serve.js 8123   (from web/), plus a built
// build-relwdiag/ and the temporary ?build=relwdiag branch in public/eden-host.js.
// Usage: node tools/safari-chunk-reload-characterize.js 'http://localhost:8123/public/eden-st.html?build=relwdiag'
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

const TARGETS = [
  [64700, 40, 65700], [65100, 40, 65350], [64300, 40, 66000],
  [65500, 40, 65100], [63900, 40, 66300],
];

async function main() {
  if (!URL) { console.error('usage: node tools/safari-chunk-reload-characterize.js <url>'); process.exit(1); }
  const s = await getSession();
  await wd.go(s, URL);
  await req('POST', `/session/${s}/timeouts`, { script: 120000 });

  let ready = false;
  for (let i = 0; i < 60; i++) {
    if (await wd.exec(s, 'return !!(window.Module && Module.calledRun);')) { ready = true; break; }
    await new Promise(r => setTimeout(r, 1000));
  }
  if (!ready) { console.log('RUNTIME NEVER READY'); process.exit(1); }

  const build = await wd.exec(s, 'return (document.currentScript||{}).src || (window.EDEN_BUILD_DIR||"?");');
  const haveTiming = await wd.exec(s, 'return typeof Module._eden_debug_mesh_timing === "function" && typeof Module._eden_console_teleport === "function";');
  console.log('runtime ready; mesh-timing+teleport exports present:', haveTiming);
  if (!haveTiming) { console.log('FATAL: build lacks diagnostics exports — served the wrong build?'); process.exit(1); }

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
      await new Promise(r => setTimeout(r, 3000));
      done('ok');
    })();
  `);
  console.log('enter world:', enter);
  if (enter !== 'ok') process.exit(1);

  // Persistent rAF hook — records {t, dt} for every frame so we can slice per-teleport windows.
  await wd.exec(s, `
    window.__frames = [];
    window.__last = performance.now();
    (function tick(t){ window.__frames.push({t, dt: t - window.__last}); window.__last = t; requestAnimationFrame(tick); })();
    return true;
  `);

  const results = [];
  for (const [x, y, z] of TARGETS) {
    await new Promise(r => setTimeout(r, 400));
    const mark = await wd.exec(s, `
      Module._eden_debug_mesh_timing_reset();
      const m = performance.now();
      window.__frames.length = 0;
      Module._eden_console_teleport(${x}, ${y}, ${z});
      return m;
    `);
    await new Promise(r => setTimeout(r, 2500));
    const raw = await wd.exec(s, `
      const ptr = Module._eden_debug_mesh_timing();
      let end = ptr; while (Module.HEAPU8[end] !== 0) end++;
      const timing = JSON.parse(new TextDecoder().decode(Module.HEAPU8.slice(ptr, end)));
      return { timing, frames: window.__frames.slice() };
    `);
    const dts = raw.frames.map(f => f.dt);
    const sorted = [...dts].sort((a, b) => b - a);
    const sum = dts.reduce((a, b) => a + b, 0);
    results.push({
      target: [x, y, z],
      frames: dts.length,
      window_ms: +sum.toFixed(1),
      worst_frame_ms: +(sorted[0] || 0).toFixed(1),
      top5_frame_ms: sorted.slice(0, 5).map(v => +v.toFixed(1)),
      frames_over_16ms: dts.filter(d => d > 16.66).length,
      frames_over_33ms: dts.filter(d => d > 33.3).length,
      meshMs: +raw.timing.meshMs.toFixed(1),
      meshMsMax: +raw.timing.meshMsMax.toFixed(2),
      meshCount: raw.timing.meshCount,
      uploadMs: +raw.timing.uploadMs.toFixed(1),
      uploadMsMax: +raw.timing.uploadMsMax.toFixed(2),
      uploadCount: raw.timing.uploadCount,
      readMs: +raw.timing.readMs.toFixed(1),
      readMsMax: +raw.timing.readMsMax.toFixed(2),
      readCount: raw.timing.readCount,
    });
  }

  console.log(JSON.stringify(results, null, 2));

  // Aggregate: for the worst teleport, how much of the observed "lost time" (frames beyond a
  // 16.6ms budget) is explained by engine CPU (mesh+read) vs. unattributed (upload path stall /
  // compositing)?
  const worst = results.reduce((a, b) => (b.worst_frame_ms > a.worst_frame_ms ? b : a));
  const lost = worst.top5_frame_ms.reduce((a, b) => a + Math.max(0, b - 16.66), 0);
  console.log('\n=== worst teleport:', JSON.stringify(worst.target), '===');
  console.log(`worst frame ${worst.worst_frame_ms}ms, ${worst.frames_over_33ms} frames <30fps, ${worst.frames_over_16ms} frames <60fps`);
  console.log(`engine CPU this burst: mesh ${worst.meshMs}ms (max chunk ${worst.meshMsMax}), read ${worst.readMs}ms, GL upload ${worst.uploadMs}ms (max ${worst.uploadMsMax})`);
  console.log(`headless-mesh-burst-probe.js measured ~0.4ms total GL upload for the same burst — compare uploadMs above.`);
  process.exit(0);
}
main().catch((e) => { console.error('ERR', e.message); process.exit(1); });
