// One live-browser session closing the three owed 256z-Stage-3 checks headless node cannot do
// (STATUS.md §3 items 1 & 3, ROADMAP Tier 1 "Follow-up queued from B5"):
//   A. New-World "Height format" picker actually creates a 256z world in a real browser.
//   B. Settings -> Storage "Convert to 64z" runs against that world and it re-opens at 64z.
//   C. An in-place save (threshold forced to 0) round-trips through IndexedDB across a real
//      page reload -- the leg a prior Safari run couldn't complete and headless node can't
//      (no `indexedDB`).
//
// Runs against build-st (default page build; EDEN_DIAGNOSTICS=ON so the console/debug exports
// used below are present). Requires: safaridriver -p 4599 &   and   node tools/serve.js 8123.
// Usage: node tools/safari-256z-authoring-live.js 'http://localhost:8123/public/eden-st.html'
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

// All the engine-driving helpers run inside one execute/async call so they share a closure.
const DRIVER_PRELUDE = `
  const M = window.Module;
  const utf8 = (ptr) => { let e = ptr; while (M.HEAPU8[e]) e++; return new TextDecoder().decode(M.HEAPU8.slice(ptr, e)); };
  const menuState = () => JSON.parse(utf8(M._eden_debug_menu_state()));
  const worldFormat = () => JSON.parse(utf8(M._eden_debug_world_format()));
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
  if (!URL) { console.error('usage: node tools/safari-256z-authoring-live.js <url>'); process.exit(1); }
  const s = await getSession();
  await wd.go(s, URL);
  await req('POST', `/session/${s}/timeouts`, { script: 120000 });
  if (!(await waitRuntime(s))) { console.log('RUNTIME NEVER READY'); process.exit(1); }
  const idb = await wd.exec(s, 'return typeof indexedDB !== "undefined" && !!(window.FS);');
  console.log('runtime ready; indexedDB + FS present:', idb);

  // ---- TEST A + B: create a 256z world via the height picker, save it, convert it to 64z. ----
  const ab = await wd.execAsync(s, DRIVER_PRELUDE + `
    const done = arguments[arguments.length - 1];
    (async () => {
      const out = { checks: [] };
      const ck = (n, c, e) => out.checks.push([n, !!c, e]);
      if (!(await waitFor(() => menuState().game_mode === 0, 15000, 'menu'))) return done({ error: 'no menu' });

      const idx = M._eden_menu_create_world();
      const nameA = utf8(M._eden_menu_world_name(idx));
      M._eden_menu_clear_pending_world_type();
      M._eden_menu_set_pending_world_type(0);
      M._eden_menu_set_pending_world_height(256);          // <-- what the "Height format" segmented control calls
      M._eden_menu_play();
      if (!(await waitFor(() => menuState().game_mode === 1, 30000, 'play A'))) return done({ error: 'A never played' });
      await sleep(2000);

      const fmt = worldFormat();
      ck('A: height picker produced a 256z world (height==256)', fmt.height === 256, fmt);
      ck('A: derived sizes followed height (16 bands, 131072 B column)', fmt.bands === 16 && fmt.column_bytes === 131072, fmt);

      // Edit a block well below y=63 so the 64z conversion in test B keeps it.
      const ps = JSON.parse(utf8(M._eden_debug_player_state()));
      const [px, py, pz] = ps.pos;
      const ex = Math.round(px), ez = Math.round(pz), ey = Math.max(2, Math.round(py) - 4);
      const setOk = M._eden_console_setblock(ex, ez, ey, 13);
      ck('A: console setblock accepted in the 256z world', setOk === 1);
      await saveInGame();

      const listed = listWorlds();
      const entA = listed.find(w => w.name === nameA);
      ck('A: 256z world listed by eden_storage_list_worlds after save', !!entA, listed);
      await quitToMenu();

      // ---- TEST B: convert-to-64z. Index is the position in the storage-list scan. ----
      const list2 = listWorlds();
      const bIdx = list2.findIndex(w => w.name === nameA);
      ck('B: world still in storage list before convert', bIdx >= 0);
      let report = null, convErr = null;
      try { report = JSON.parse(utf8(M._eden_storage_convert_to_64z_at(bIdx))); }
      catch (e) { convErr = String(e); }
      ck('B: convert_to_64z_at returned a report (no throw)', !!report && !convErr, report || convErr);
      out.convertReport = report;

      // Re-open the converted world.
      const list3 = listWorlds();
      const cIdx = list3.findIndex(w => w.name === nameA);
      M._eden_menu_select(cIdx);
      const playOk = M._eden_menu_play();
      if (!(await waitFor(() => menuState().game_mode === 1, 30000, 'play converted'))) return done({ error: 'converted world never played', out });
      await sleep(1500);
      const fmt2 = worldFormat();
      ck('B: converted world re-opens at 64z (height==64)', fmt2.height === 64, fmt2);
      ck('B: converted world has 64z sizes (4 bands, 32768 B column)', fmt2.bands === 4 && fmt2.column_bytes === 32768, fmt2);
      const kept = M._eden_console_getblock(ex, ez, ey);
      ck('B: block edited below y=63 survived the conversion', kept === 13, { got: kept, at: [ex, ez, ey] });
      await quitToMenu();

      out.nameA = nameA;
      done(out);
    })().catch(e => done({ error: String(e), stack: e && e.stack }));
  `);
  if (ab.error) { console.log('TEST A/B ERROR:', ab.error, ab.stack || ''); }
  console.log('\n--- TEST A + B ---');
  (ab.checks || []).forEach(([n, c, e]) => check(n, c, c ? undefined : e));
  if (ab.convertReport) console.log('  convert-to-64z report:', JSON.stringify(ab.convertReport));

  // ---- TEST C: in-place save round-trips through IndexedDB across a real page reload. ----
  const setup = await wd.execAsync(s, DRIVER_PRELUDE + `
    const done = arguments[arguments.length - 1];
    (async () => {
      const out = { checks: [] };
      if (!(await waitFor(() => menuState().game_mode === 0, 15000, 'menu C'))) return done({ error: 'no menu C' });
      const idx = M._eden_menu_create_world();
      const nameC = utf8(M._eden_menu_world_name(idx));
      M._eden_menu_clear_pending_world_type();
      M._eden_menu_set_pending_world_type(0);
      M._eden_menu_play();
      if (!(await waitFor(() => menuState().game_mode === 1, 30000, 'play C'))) return done({ error: 'C never played' });
      await sleep(2000);

      const before = M._eden_debug_get_save_inplace_threshold();
      M._eden_debug_set_save_inplace_threshold(0);              // force the in-place path for any save
      const now = M._eden_debug_get_save_inplace_threshold();
      out.checks.push(['C: threshold forced to 0 (in-place path)', now === 0, { before, now }]);

      const ps = JSON.parse(utf8(M._eden_debug_player_state()));
      const [px, py, pz] = ps.pos;
      const ex = Math.round(px), ez = Math.round(pz), ey = Math.max(2, Math.round(py) - 3);
      M._eden_console_setblock(ex, ez, ey, 7);
      await saveInGame();                                        // in-place save + .savejrnl
      const memPath = '/documents/' + (listWorlds().find(w => w.name === nameC) || {}).file;
      out.checks.push(['C: in-place save left no whole-file scratch copy', !FS.analyzePath(memPath + '.savetmp.bak').exists, memPath]);
      await quitToMenu();                                        // quit = another in-place save

      // Force IDBFS -> IndexedDB flush and WAIT for it before reloading.
      await new Promise((res) => FS.syncfs(false, res));
      out.nameC = nameC; out.edit = [ex, ez, ey];
      done(out);
    })().catch(e => done({ error: String(e), stack: e && e.stack }));
  `);
  console.log('\n--- TEST C (pre-reload) ---');
  if (setup.error) { console.log('TEST C SETUP ERROR:', setup.error, setup.stack || ''); process.exit(1); }
  (setup.checks || []).forEach(([n, c, e]) => check(n, c, c ? undefined : e));
  console.log(`  created "${setup.nameC}", edit at ${JSON.stringify(setup.edit)} = 7, flushed to IndexedDB`);

  // Real reload.
  await wd.exec(s, 'location.reload(); return true;');
  await new Promise(r => setTimeout(r, 2000));
  if (!(await waitRuntime(s))) { console.log('RUNTIME NEVER READY AFTER RELOAD'); process.exit(1); }
  console.log('  page reloaded, runtime ready again');

  const after = await wd.execAsync(s, DRIVER_PRELUDE + `
    const done = arguments[arguments.length - 1];
    (async () => {
      const out = { checks: [] };
      if (!(await waitFor(() => menuState().game_mode === 0, 20000, 'menu post-reload'))) return done({ error: 'no menu post-reload' });
      const list = listWorlds();
      const nameC = ${JSON.stringify(setup.nameC)};
      const [ex, ez, ey] = ${JSON.stringify(setup.edit)};
      const cIdx = list.findIndex(w => w.name === nameC);
      out.checks.push(['C: world present in storage list after reload (survived IndexedDB round-trip)', cIdx >= 0, list.map(w => w.name)]);
      if (cIdx < 0) return done(out);
      M._eden_menu_select(cIdx);
      M._eden_menu_play();
      if (!(await waitFor(() => menuState().game_mode === 1, 30000, 'play post-reload'))) { out.checks.push(['C: reloaded world plays', false]); return done(out); }
      await sleep(1500);
      const got = M._eden_console_getblock(ex, ez, ey);
      out.checks.push(['C: block from the in-place save persisted across the reload (==7)', got === 7, { got, at: [ex, ez, ey] }]);
      done(out);
    })().catch(e => done({ error: String(e), stack: e && e.stack }));
  `);
  console.log('\n--- TEST C (post-reload) ---');
  if (after.error) console.log('TEST C POST-RELOAD ERROR:', after.error, after.stack || '');
  (after.checks || []).forEach(([n, c, e]) => check(n, c, c ? undefined : e));

  console.log(`\n==== ${PASS} passed, ${FAIL} failed ====`);
  process.exit(FAIL ? 1 : 0);
}
main().catch((e) => { console.error('ERR', e.message); process.exit(1); });
