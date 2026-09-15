// B3 Stage 4 — the one verification leg no headless suite can do.
//
// `node` runs the threaded build (all 13 suites pass against build-thr), so the worker mesher and
// the worker column decoder get real headless regression coverage. What node CANNOT cover is that
// it has no cross-origin isolation and therefore no SharedArrayBuffer: every failure mode in the
// "browser API rejects a shared-memory view" class (see web/CLAUDE.md, and the
// `-Wpthreads-mem-growth` note in CMakeLists.txt) is invisible to it, and so is the real WebGL
// driver that prepareVBO() uploads into while two workers are running.
//
// So this asserts CORRECTNESS in a real browser, not performance:
//   1. the threaded build actually loaded -- cross-origin isolated, wasm memory really is a
//      SharedArrayBuffer, and the page did not silently fall back to build-st (eden-host.js does
//      that on purpose when isolation is missing, which would make everything below vacuous);
//   2. the engine frame loop is alive before AND after the burst -- the failure signature of both
//      black-canvas bugs this port has had is "DOM fine, rAF fine, engine frames stopped";
//   3. a real teleport burst drives the pool: chunks dispatched to workers, columns decoded on
//      workers, every job published, nothing left in flight or unpublished;
//   4. the geometry the worker pipeline produces is IDENTICAL to the single-threaded reference
//      recorded headless -- vertex counts depend only on block types and face visibility, so the
//      same teleports over the same deterministic default world must give the same totals;
//   5. no uncaught JS exception or console error happened at any point.
//
// Requires:  safaridriver -p 4599 &
//            node tools/serve.js <port>        (from web/ -- MUST be serve.js, for COOP/COEP)
// Usage:     node tools/safari-threaded-mesher-check.js 'http://localhost:8123/public/eden-st.html?build=thr'
//            node tools/safari-threaded-mesher-check.js --quit      (release the session)
'use strict';
const fs = require('fs');
const os = require('os');
const path = require('path');

const PORT = 4599;
const URL = process.argv[2];
const BASE = `http://localhost:${PORT}`;
const SESSION_FILE = path.join(os.tmpdir(), `eden-safari-session-${PORT}.txt`);

// The same five teleports headless-mesh-burst-probe.js uses, and the whole-window vertex totals
// build-relwdiag (single-threaded mesher, inline decode) produces for them. Recorded 2026-08-27.
const TARGETS = [
  [64700, 40, 65700], [65100, 40, 65350], [64300, 40, 66000],
  [65500, 40, 65100], [63900, 40, 66300],
];
const REFERENCE_VERTS = [1108392, 1017120, 1047420, 1080876, 897948];

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
  del: (s) => req('DELETE', `/session/${s}`),
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

const checks = [];
function check(ok, label, detail) {
  checks.push({ ok: !!ok, label, detail });
  console.log(`${ok ? 'PASS' : 'FAIL'}  ${label}${detail === undefined ? '' : `  — ${detail}`}`);
}

async function main() {
  if (process.argv[2] === '--quit') {
    try { const s = fs.readFileSync(SESSION_FILE, 'utf8').trim(); await wd.del(s); } catch {}
    try { fs.unlinkSync(SESSION_FILE); } catch {}
    console.log('session released');
    return;
  }
  if (!URL) {
    console.error("usage: node tools/safari-threaded-mesher-check.js 'http://localhost:PORT/public/eden-st.html?build=thr'");
    process.exit(1);
  }
  const s = await getSession();
  await wd.go(s, URL);
  await req('POST', `/session/${s}/timeouts`, { script: 180000 });

  // Trap errors before the runtime is even up, so a boot-time exception is caught too. Read C
  // strings with the port's own utf8() shape (walk HEAPU8, copy into a FRESH Uint8Array, then
  // TextDecoder): TextDecoder throws on a shared-memory view, and touching a Module runtime method
  // that is not exported aborts the engine permanently -- both are documented in web/CLAUDE.md and
  // both would be caused BY this probe rather than found by it.
  await wd.exec(s, `
    window.__edenErrors = [];
    window.addEventListener('error', (e) => window.__edenErrors.push(String(e.message || e)));
    window.addEventListener('unhandledrejection', (e) => window.__edenErrors.push('unhandledrejection: ' + String(e.reason)));
    window.__utf8 = function (ptr) {
      const H = Module.HEAPU8; let end = ptr; while (H[end] !== 0) end++;
      const copy = new Uint8Array(end - ptr); copy.set(H.subarray(ptr, end));
      return new TextDecoder('utf-8').decode(copy);
    };
    window.__frames = 0;
    const prev = Module.__edenFramePost;
    Module.__edenFramePost = function () { window.__frames++; if (prev) prev.apply(this, arguments); };
    return true;
  `);

  let ready = false;
  for (let i = 0; i < 90; i++) {
    if (await wd.exec(s, 'return !!(window.Module && Module.calledRun);')) { ready = true; break; }
    await new Promise(r => setTimeout(r, 1000));
  }
  check(ready, 'wasm runtime reached calledRun');
  if (!ready) return finish();

  // (1) Is this actually the threaded build? eden-host.js falls back to build-st when the page is
  // not cross-origin isolated, and says so -- without this check a green run below could be the
  // single-threaded build passing a threading test.
  const env = await wd.exec(s, `
    return {
      isolated: !!self.crossOriginIsolated,
      shared: (typeof SharedArrayBuffer !== 'undefined') && (Module.HEAPU8.buffer instanceof SharedArrayBuffer),
      build: (window.EDEN_BUILD_DIR || document.querySelector('script[src*="build-"]')?.src || 'unknown'),
    };
  `);
  check(env.isolated, 'page is cross-origin isolated (COOP/COEP)', JSON.stringify(env.build));
  check(env.shared, 'wasm memory is a SharedArrayBuffer (threaded build really loaded)');
  if (!env.shared) { console.log('\n  -> the page fell back to build-st; serve with tools/serve.js and use ?build=thr'); return finish(); }

  // (2) Enter a world.
  const enter = await wd.execAsync(s, `
    const done = arguments[0];
    (async () => {
      const inMenu = () => Module._eden_menu_active() === 1;
      const wait = (pred, ms) => new Promise((res) => {
        const t0 = Date.now();
        (function poll(){ if (pred()) return res(true); if (Date.now()-t0>ms) return res(false); setTimeout(poll, 50); })();
      });
      if (!(await wait(inMenu, 20000))) return done('menu never came up');
      Module._eden_menu_create_world();
      Module._eden_menu_set_pending_world_type(0);
      Module._eden_menu_play();
      if (!(await wait(() => !inMenu(), 60000))) return done('never reached PLAY');
      await new Promise(r => setTimeout(r, 3000));
      done('ok');
    })();
  `);
  check(enter === 'ok', 'created and entered a world', enter);
  if (enter !== 'ok') return finish();

  const framesBefore = await wd.exec(s, 'return window.__frames;');
  check(framesBefore > 0, 'engine frame loop is alive before the burst', `${framesBefore} frames`);

  // (3)+(4) Five teleports. Each one waits for the pool to go quiet rather than for a fixed time:
  // build-thr is -O0 and roughly 8x slower per chunk than release codegen, so a fixed settle that
  // is right headless is not right here.
  const pool = { dispatched: 0, published: 0, inlined: 0, stale: 0, decoded: 0 };
  const verts = [];
  let settleFailure = null;
  for (let i = 0; i < TARGETS.length; i++) {
    const [x, y, z] = TARGETS[i];
    const r = await wd.execAsync(s, `
      // executeAsyncScript appends the callback AFTER the user args, so it is last, not first.
      const done = arguments[arguments.length - 1];
      const [x, y, z] = [arguments[0], arguments[1], arguments[2]];
      (async () => {
        const geom = () => JSON.parse(window.__utf8(Module._eden_debug_terrain_geometry()));
        Module._eden_debug_mesh_timing_reset();
        Module._eden_console_teleport(x, y, z);
        // Settled = the pool is empty, nothing is holding an unpublished mesh, and the vertex
        // total has stopped moving for a few consecutive samples.
        let last = -1, stableFor = 0;
        const t0 = Date.now();
        while (Date.now() - t0 < 40000) {
          await new Promise(r => setTimeout(r, 250));
          const g = geom();
          const quiet = g.jobs_in_flight === 0 && g.unpublished === 0;
          if (quiet && g.rt_vertices === last) { if (++stableFor >= 4) break; }
          else stableFor = 0;
          last = g.rt_vertices;
        }
        const g = geom();
        const t = JSON.parse(window.__utf8(Module._eden_debug_mesh_timing()));
        done({ geom: g, timing: t, ms: Date.now() - t0 });
      })();
    `, [x, y, z]);
    verts.push(r.geom.rt_vertices);
    pool.dispatched += r.timing.dispatched; pool.published += r.timing.published;
    pool.inlined += r.timing.inlined; pool.stale += r.timing.stale;
    pool.decoded += r.timing.decodedColumns;
    if (r.geom.jobs_in_flight !== 0 || r.geom.unpublished !== 0) {
      settleFailure = `burst ${i}: jobs_in_flight=${r.geom.jobs_in_flight} unpublished=${r.geom.unpublished}`;
    }
    console.log(`  burst ${i}: ${r.geom.rt_vertices} verts, ${r.timing.dispatched} meshes dispatched, ` +
      `${r.timing.decodedColumns} columns decoded off-thread, settled in ${r.ms} ms`);
  }

  check(pool.dispatched > 0, 'the worker MESHER ran in a real browser', `${pool.dispatched} chunks dispatched`);
  check(pool.decoded > 0, 'the worker column DECODER ran in a real browser', `${pool.decoded} columns`);
  check(pool.published === pool.dispatched, 'every dispatched mesh was published',
    `${pool.published}/${pool.dispatched}, ${pool.inlined} fell back to inline, ${pool.stale} went stale`);
  check(!settleFailure, 'no job left in flight or unpublished after each burst', settleFailure || 'clean');

  const vertsMatch = verts.every((v, i) => v === REFERENCE_VERTS[i]);
  check(vertsMatch, 'geometry identical to the single-threaded reference',
    vertsMatch ? 'all 5 bursts' : `got [${verts}] want [${REFERENCE_VERTS}]`);

  const framesAfter = await wd.exec(s, 'return window.__frames;');
  check(framesAfter > framesBefore, 'engine frame loop still alive after the bursts',
    `${framesAfter - framesBefore} frames during the run`);

  // (5) Anything that threw, including from a shared-memory view handed to a browser API.
  const errs = await wd.exec(s, 'return window.__edenErrors;');
  check(errs.length === 0, 'no uncaught JS errors', errs.length ? JSON.stringify(errs.slice(0, 5)) : 'none');

  finish();
}

function finish() {
  const failed = checks.filter((c) => !c.ok);
  console.log(`\n${checks.length - failed.length}/${checks.length} checks passed`);
  process.exit(failed.length ? 1 : 0);
}

main().catch((e) => { console.error('ERROR:', e.message); process.exit(1); });
