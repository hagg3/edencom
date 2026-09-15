// safari-opfs-live.js — the browser leg of ROADMAP Phase C / C2 (the OPFS persistence backend).
// Plan: ../../WORKING/opfs-backend-plan.md §6.3.
//
// WHY THIS EXISTS. tools/headless-opfs-mirror-test.js covers everything above the byte sink (the
// op log, the delta property, populate) against the real engine — but node has no
// `navigator.storage` and `FileSystemSyncAccessHandle` is a Worker-only API, so NOTHING headless
// can touch the half of C2 that actually writes bytes: the worker, the sync access handles, the
// transferred buffers, and whether a save survives a real page reload. That is this file's job,
// and it is the same "a green headless suite is regression cover, not discovery" rule that
// tools/safari-threaded-mesher-check.js exists for.
//
// What it checks:
//   1. The OPFS backend is actually the one that won (`EdenStorage.backend() === 'opfs'`) — every
//      other check is vacuous if the page quietly fell back to IDBFS.
//   2. A world created and saved here is mirrored: per-save bytes written (EdenStorage.opfsStats())
//      are a small fraction of the world file, on the in-place save path.
//   3. After a REAL location.reload(), the world is still listed, still loads, and the block
//      placed before the reload reads back — i.e. the bytes went to OPFS and came back.
//   4. The engine frame loop is alive after all of it (the signature of both black-canvas bugs
//      this port has had is "DOM fine, rAF fine, engine frames stopped").
//
// Requires: safaridriver -p 4599 &   and   node tools/serve.js 8123   (from web/).
// Usage: node tools/safari-opfs-live.js 'http://localhost:8123/public/eden-st.html'
//        node tools/safari-opfs-live.js --quit        # release the reused WebDriver session
'use strict';
const fs = require('fs');
const os = require('os');
const path = require('path');

const PORT = 4599;
const ARG = process.argv[2];
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

let PASS = 0, FAIL = 0;
function check(name, cond, extra) {
  if (cond) { PASS++; console.log(`  ok   ${name}`); }
  else { FAIL++; console.log(`  FAIL ${name}${extra !== undefined ? '  -> ' + JSON.stringify(extra) : ''}`); }
}

async function waitRuntime(s) {
  for (let i = 0; i < 90; i++) {
    if (await wd.exec(s, 'return !!(window.Module && Module.calledRun && Module._eden_debug_menu_state);')) return true;
    await new Promise(r => setTimeout(r, 1000));
  }
  return false;
}

// NOTE (cost a run in an earlier pass): executeAsyncScript appends its callback AFTER your
// arguments, so it is arguments[arguments.length-1], never arguments[0].
const DRIVER_PRELUDE = `
  const M = window.Module;
  const utf8 = (ptr) => { let e = ptr; while (M.HEAPU8[e]) e++; return new TextDecoder().decode(new Uint8Array(M.HEAPU8.subarray(ptr, e))); };
  const menuState = () => JSON.parse(utf8(M._eden_debug_menu_state()));
  const listWorlds = () => JSON.parse(utf8(M._eden_storage_list_worlds()));
  const sleep = (ms) => new Promise(r => setTimeout(r, ms));
  const waitFor = async (pred, ms, label) => {
    const t0 = Date.now();
    for (;;) { let ok = false; try { ok = pred(); } catch (e) {}
      if (ok) return true;
      if (Date.now() - t0 > ms) { console.warn('timeout: ' + label); return false; }
      await sleep(50); }
  };
  const tapHud = async (which) => { M._eden_tap_hud_button_begin(which); await sleep(60); M._eden_tap_hud_button_end(which); await sleep(120); };
  const openMenu = async () => { if (M._eden_hud_in_menu() === 0) await tapHud(0); };
  const quitToMenu = async () => { await openMenu(); await tapHud(6); return waitFor(() => menuState().game_mode === 0, 10000, 'menu after quit'); };
  const saveInGame = async () => { await openMenu(); await tapHud(3); await sleep(400); };
`;

async function main() {
  if (ARG === '--quit') {
    try { await wd.del(fs.readFileSync(SESSION_FILE, 'utf8').trim()); } catch (e) {}
    try { fs.unlinkSync(SESSION_FILE); } catch (e) {}
    console.log('session released');
    return;
  }
  if (!ARG) { console.error("usage: node tools/safari-opfs-live.js <url> | --quit"); process.exit(1); }
  const s = await getSession();
  await wd.go(s, ARG);
  await req('POST', `/session/${s}/timeouts`, { script: 180000 });
  if (!(await waitRuntime(s))) { console.log('RUNTIME NEVER READY'); process.exit(1); }

  const env = await wd.exec(s, `return {
    backend: window.EdenStorage && EdenStorage.backend ? EdenStorage.backend() : 'no-EdenStorage',
    opfsApi: !!(navigator.storage && navigator.storage.getDirectory),
    stats: window.EdenStorage && EdenStorage.opfsStats ? EdenStorage.opfsStats() : null
  };`);
  console.log('--- environment ---');
  check('the browser exposes the OPFS API', env.opfsApi, env);
  check('the OPFS backend is the one in use (not an IDBFS fallback)', env.backend === 'opfs', env);
  if (env.backend !== 'opfs') {
    console.log('  everything below would be vacuous — stopping.');
    console.log(`\n==== ${PASS} passed, ${FAIL} failed ====`);
    process.exit(1);
  }

  const setup = await wd.execAsync(s, DRIVER_PRELUDE + `
    const done = arguments[arguments.length - 1];
    (async () => {
      const out = { checks: [] };
      const ck = (n, c, e) => out.checks.push([n, !!c, e]);
      if (!(await waitFor(() => menuState().game_mode === 0, 20000, 'menu'))) return done({ error: 'no menu' });

      const idx = M._eden_menu_create_world();
      const name = utf8(M._eden_menu_world_name(idx));
      M._eden_menu_clear_pending_world_type();
      M._eden_menu_set_pending_world_type(0);   // answer the world-type modal up front, like eden-menu.js does
      M._eden_menu_play();
      if (!(await waitFor(() => menuState().game_mode === 1, 40000, 'play'))) return done({ error: 'never played' });
      await sleep(2500);

      // Dirty a grid of real columns so the world file is a few MB — the delta ratio below is
      // meaningless on a 45 KB toy world (the journal and directory are O(columns), not O(size)).
      const ps = JSON.parse(utf8(M._eden_debug_player_state()));
      const ex = Math.round(ps.pos[0]), ez = Math.round(ps.pos[2]), ey = Math.max(2, Math.round(ps.pos[1]) - 3);
      for (let i = -5; i < 5; i++) for (let j = -5; j < 5; j++) M._eden_console_setblock(ex + 16*i, ez + 16*j, ey, 1);
      await saveInGame();
      await new Promise((res) => FS.syncfs(false, res));

      M._eden_debug_set_save_inplace_threshold(0);           // B5's in-place path — what C2 is for
      M._eden_console_setblock(ex, ez, ey - 1, 7);
      const before = EdenStorage.opfsStats().bytesWritten;
      await saveInGame();
      await new Promise((res) => FS.syncfs(false, res));
      const stats = EdenStorage.opfsStats();
      const ent = listWorlds().find(w => w.name === name) || {};
      out.worldBytes = ent.bytes || 0;
      out.saveBytes = stats.bytesWritten - before;
      out.lastMs = stats.lastMs;
      ck('an in-place save mirrored a small fraction of the world file',
         out.saveBytes > 0 && out.worldBytes > 0 && out.saveBytes < out.worldBytes / 10,
         { saveBytes: out.saveBytes, worldBytes: out.worldBytes });
      ck('the flush was fast (main thread only sliced and posted the delta)', stats.lastMs < 50, stats);
      ck('no flush errors', stats.errors === 0, stats);

      await quitToMenu();
      await new Promise((res) => FS.syncfs(false, res));
      out.name = name; out.edit = [ex, ez, ey - 1];
      done(out);
    })().catch(e => done({ error: String(e), stack: e && e.stack }));
  `);
  console.log('\n--- pre-reload ---');
  if (setup.error) { console.log('SETUP ERROR:', setup.error, setup.stack || ''); process.exit(1); }
  (setup.checks || []).forEach(([n, c, e]) => check(n, c, c ? undefined : e));
  console.log(`  world "${setup.name}" ${setup.worldBytes.toLocaleString()} B; ` +
              `in-place save mirrored ${setup.saveBytes.toLocaleString()} B in ${(setup.lastMs || 0).toFixed(1)} ms`);

  await wd.exec(s, 'location.reload(); return true;');
  await new Promise(r => setTimeout(r, 2000));
  if (!(await waitRuntime(s))) { console.log('RUNTIME NEVER READY AFTER RELOAD'); process.exit(1); }
  console.log('  page reloaded, runtime ready again');

  const after = await wd.execAsync(s, DRIVER_PRELUDE + `
    const done = arguments[arguments.length - 1];
    (async () => {
      const out = { checks: [] };
      const ck = (n, c, e) => out.checks.push([n, !!c, e]);
      ck('still on the OPFS backend after the reload', EdenStorage.backend() === 'opfs', EdenStorage.backend());
      if (!(await waitFor(() => menuState().game_mode === 0, 25000, 'menu post-reload'))) return done({ error: 'no menu post-reload' });
      const list = listWorlds();
      const name = ${JSON.stringify(setup.name)};
      const [ex, ez, ey] = ${JSON.stringify(setup.edit)};
      const i = list.findIndex(w => w.name === name);
      ck('the world survived the reload (populated back out of OPFS)', i >= 0, list.map(w => w.name));
      if (i < 0) return done(out);
      M._eden_menu_select(i);
      M._eden_menu_play();
      if (!(await waitFor(() => menuState().game_mode === 1, 40000, 'play post-reload'))) {
        ck('the reloaded world plays', false); return done(out);
      }
      await sleep(1500);
      ck('the block written by the in-place save is there (==7)', M._eden_console_getblock(ex, ez, ey) === 7,
         { got: M._eden_console_getblock(ex, ez, ey), at: [ex, ez, ey] });
      // Frame-loop liveness: count ENGINE frames, not rAF frames — Module.__edenFramePost is the
      // per-frame hook eden_frame_tick() calls, and it is the only signal that distinguishes "the
      // engine is running" from "the DOM overlay painted over a dead GL context".
      const prev = M.__edenFramePost; window.__fp = 0;
      M.__edenFramePost = function () { window.__fp++; if (prev) return prev.apply(this, arguments); };
      await sleep(700);
      ck('the engine frame loop is alive after all of it', window.__fp > 0, { frames: window.__fp });
      done(out);
    })().catch(e => done({ error: String(e), stack: e && e.stack }));
  `);
  console.log('\n--- post-reload ---');
  if (after.error) console.log('POST-RELOAD ERROR:', after.error, after.stack || '');
  (after.checks || []).forEach(([n, c, e]) => check(n, c, c ? undefined : e));

  console.log(`\n==== ${PASS} passed, ${FAIL} failed ====`);
  process.exit(FAIL ? 1 : 0);
}
main().catch((e) => { console.error('ERR', e.message); process.exit(1); });
