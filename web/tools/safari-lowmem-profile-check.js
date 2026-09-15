// safari-lowmem-profile-check.js — ROADMAP Phase V / V5: the three browser-side behaviours of
// Phase M / M5 (low-memory device profile) that a headless suite structurally cannot reach.
//
// Drives real Safari over safaridriver (same pattern as tools/safari-opfs-live.js). Checks:
//
//   1. M5.1  a REMEMBERED threaded-load failure downgrades ?build=thr to build-st before the
//            threaded tree is ever requested (localStorage['eden.lowmem']='1' -> build-st/eden.js
//            is what loads, and a toast/status says why). This is the "every subsequent visit is
//            one fast single-threaded boot" half of M5.1. The 45 s timer FIRING on a real OOM is
//            V9 (iPad Air 2) — it cannot be provoked on a desktop that loads build-thr in ~2 s.
//
//   2. M5.2  ?lowmem=1 seeds the low-memory VIDEO preset as the default for the three video rows
//            (dpr_cap=1x/idx0, render_scale=75%/idx1, fps_cap=45/idx2 — kProfiles[low-mem] in
//            DisplayProfile_web.mm), and eden_low_memory() reads back true. A player override still
//            wins (not tested here — fresh profile has no stored rows).
//
//   3. M5.3  a 256z world created with eden_low_memory() set is REFUSED at load: World::loadWorld
//            bails via eden_report_load_failure(name,"TALL_WORLD_LOW_MEM") before allocateMemory(),
//            game_mode stays 0, and eden-loaderror.js's buildTallWorld() dialog is on screen.
//
// Requires: safaridriver -p 4599 &   and   node tools/serve.js 8123   (from web/).
// Usage: node tools/safari-lowmem-profile-check.js 'http://localhost:8123/public/eden-st.html'
//        node tools/safari-lowmem-profile-check.js --quit
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

async function waitRuntime(s, ms = 90000) {
  const t0 = Date.now();
  while (Date.now() - t0 < ms) {
    if (await wd.exec(s, 'return !!(window.Module && Module.calledRun && Module._eden_debug_menu_state);')) return true;
    await new Promise(r => setTimeout(r, 1000));
  }
  return false;
}

const PRELUDE = `
  const M = window.Module;
  const utf8 = (ptr) => { let e = ptr; while (M.HEAPU8[e]) e++; return new TextDecoder().decode(new Uint8Array(M.HEAPU8.subarray(ptr, e))); };
  const menuState = () => JSON.parse(utf8(M._eden_debug_menu_state()));
  const sleep = (ms) => new Promise(r => setTimeout(r, ms));
  const waitFor = async (pred, ms, label) => {
    const t0 = Date.now();
    for (;;) { let ok = false; try { ok = pred(); } catch (e) {}
      if (ok) return true;
      if (Date.now() - t0 > ms) { console.warn('timeout: ' + label); return false; }
      await sleep(50); }
  };
`;

async function main() {
  if (ARG === '--quit') {
    try { await wd.del(fs.readFileSync(SESSION_FILE, 'utf8').trim()); } catch (e) {}
    try { fs.unlinkSync(SESSION_FILE); } catch (e) {}
    console.log('session released');
    return;
  }
  if (!ARG) { console.error("usage: node tools/safari-lowmem-profile-check.js <url-to-eden-st.html> | --quit"); process.exit(1); }
  const u = new URL(ARG);
  const base = u.origin + u.pathname;
  const s = await getSession();
  await req('POST', `/session/${s}/timeouts`, { script: 120000 });

  // ---- clean slate: no stored settings, no remembered downgrade -----------------------------
  await wd.go(s, base);
  if (!(await waitRuntime(s))) { console.log('RUNTIME NEVER READY (clean load)'); process.exit(1); }
  await wd.exec(s, `try { localStorage.clear(); } catch(e){}`);

  // ============================================================================================
  // 1. M5.1 — a remembered threaded-load failure downgrades ?build=thr before requesting it
  // ============================================================================================
  console.log('\n--- M5.1: remembered-downgrade path ---');
  // The generation key must match eden-host.js's EDEN_LOWMEM_GEN or the flag is (correctly)
  // discarded as a verdict from an older build — see edenExpireStaleLowMemVerdict(). Read it out of
  // the page rather than hard-coding it here, so bumping the constant does not silently make this
  // section test the expiry path instead of the downgrade path.
  await wd.exec(s, `
    localStorage.setItem('eden.lowmem','1');
    localStorage.setItem('eden.lowmem.gen',
      typeof EDEN_LOWMEM_GEN === 'string' ? EDEN_LOWMEM_GEN : '');   // a top-level const is NOT on window
    return localStorage.getItem('eden.lowmem.gen');`);
  await wd.go(s, base + '?build=thr');
  if (!(await waitRuntime(s))) { console.log('RUNTIME NEVER READY (?build=thr w/ eden.lowmem)'); process.exit(1); }
  const dg = await wd.exec(s, `
    const scripts = [...document.querySelectorAll('script[src]')].map(x => x.getAttribute('src'));
    return {
      edenJs: scripts.find(x => /eden\\.js/.test(x)) || null,
      shared: (typeof Module !== 'undefined' && Module.HEAPU8 && Module.HEAPU8.buffer &&
               Module.HEAPU8.buffer.constructor && Module.HEAPU8.buffer.constructor.name) || null,
      status: (document.getElementById('eden-status') || {}).textContent || '',
      bodyText: document.body.innerText.slice(0, 4000),
      lowmemFlag: (Module._eden_low_memory ? Module._eden_low_memory() : -1),
    };`);
  check('?build=thr with a remembered failure loads build-st, not build-thr',
        /build-st\//.test(dg.edenJs || ''), dg.edenJs);
  check('wasm memory is a plain ArrayBuffer (single-threaded)', dg.shared === 'ArrayBuffer', dg.shared);
  check('the downgrade reason is surfaced on the page (status line or toast)',
        /single-threaded/i.test(dg.status + ' ' + dg.bodyText), { status: dg.status });
  check('the remembered downgrade also sets the low-memory engine flag', dg.lowmemFlag === 1, dg.lowmemFlag);

  // ============================================================================================
  // 2. M5.2 — ?lowmem=1 seeds the low-memory video preset as the three video-row defaults
  // ============================================================================================
  console.log('\n--- M5.2: low-memory video preset seeding ---');
  await wd.exec(s, `try { localStorage.clear(); } catch(e){}`);
  await wd.go(s, base + '?lowmem=1');
  if (!(await waitRuntime(s))) { console.log('RUNTIME NEVER READY (?lowmem=1)'); process.exit(1); }
  const preset = await wd.execAsync(s, PRELUDE + `
    const done = arguments[arguments.length - 1];
    (async () => {
      // the profile-default seeder runs from eden_apply_input_profile(); give it a beat
      await sleep(1500);
      M._eden_settings_init();
      const schema = JSON.parse(utf8(M._eden_settings_schema()));
      const val = (key) => { const row = schema.find(r => r.key === key); return row ? M._eden_settings_get(row.i) : null; };
      done({
        lowmem: M._eden_low_memory ? M._eden_low_memory() : -1,
        dpr_cap: val('dpr_cap'), render_scale: val('render_scale'), fps_cap: val('fps_cap'),
      });
    })();`);
  check('eden_low_memory() reads true under ?lowmem=1', preset.lowmem === 1, preset);
  check('dpr_cap default seeded to 1x (index 0)', preset.dpr_cap === 0, preset.dpr_cap);
  check('render_scale default seeded to 75% (index 1)', preset.render_scale === 1, preset.render_scale);
  check('fps_cap default seeded to the low-mem cap (index 2)', preset.fps_cap === 2, preset.fps_cap);

  // ============================================================================================
  // 3. M5.3 — a 256z world is refused when eden_low_memory() is set
  // ============================================================================================
  console.log('\n--- M5.3: 256z refusal on a low-memory device ---');
  await wd.exec(s, `try { localStorage.clear(); } catch(e){}`);
  await wd.go(s, base + '?lowmem=1');
  if (!(await waitRuntime(s))) { console.log('RUNTIME NEVER READY (?lowmem=1, refusal)'); process.exit(1); }
  const refuse = await wd.execAsync(s, PRELUDE + `
    const done = arguments[arguments.length - 1];
    (async () => {
      if (!(await waitFor(() => menuState().game_mode === 0, 20000, 'menu'))) return done({ error: 'no menu' });
      const idx = M._eden_menu_create_world();
      M._eden_menu_clear_pending_world_type();
      M._eden_menu_set_pending_world_type(0);
      M._eden_menu_set_pending_world_height(256);   // New Dawn 256z
      M._eden_menu_play();
      // it must NOT reach play; wait long enough to be sure, then look for the dialog
      const reached = await waitFor(() => menuState().game_mode === 1, 12000, 'play (should NOT happen)');
      await sleep(500);
      const dlg = document.getElementById('eden-loaderror-msg');
      done({
        reachedPlay: reached,
        loadErrorOpen: window.EdenLoadError ? window.EdenLoadError.isOpen() : null,
        dialogText: dlg ? dlg.textContent : (document.body.innerText.match(/needs more memory[\\s\\S]{0,200}/i) || [null])[0],
        bodyHasConvert: /Convert to 64z|64z/i.test(document.body.innerText),
      });
    })();`);
  check('a 256z world does NOT reach play on a low-memory device', refuse.reachedPlay === false, refuse);
  check('the load-error dialog is open', refuse.loadErrorOpen === true, refuse);
  check('the dialog explains it is a memory problem / points at Convert to 64z',
        /memory/i.test(refuse.dialogText || '') || refuse.bodyHasConvert, refuse);

  console.log(`\n==== ${PASS} passed, ${FAIL} failed ====`);
  process.exit(FAIL ? 1 : 0);
}
main().catch(e => { console.error(e); process.exit(1); });
