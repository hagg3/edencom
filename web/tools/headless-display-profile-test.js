// Headless regression guard for audit rows D1 ("unpin the display constants") + D4 ("one build,
// two profiles") — src/seam/DisplayProfile_web.mm, and the Classes/ re-layout methods it drives
// (Hud::layoutForScreen, Menu::layoutForScreen, Input::screenMetricsChanged).
//
// WHY THIS EXISTS. Before D1, SCREEN_WIDTH/SCREEN_HEIGHT were compile-time constants, so "is the
// layout right?" was answerable by reading the source. Now they are derived from a window aspect
// and a UI-scale setting, and the HUD is laid out more than once per session. The failure mode that
// buys is silent and specific: a rect that does NOT follow a metrics change (because its arithmetic
// stayed in a constructor, or because a mutated file-static compounded on the second call) leaves a
// button drawn in one place and hit-tested in another. Nothing crashes and nothing logs. A desktop
// browser will not show it either, since half the affected chrome is touch-only.
//
// WHAT IT PINS
//   1. The classic profile is bit-exact. ui_scale 200% + Classic aspect must still produce
//      568x320 points and the exact HUD rects this port shipped with before D1 landed — that is the
//      audit's own mitigation ("keep the pinned profile as the default until verified on a real
//      phone") made checkable rather than asserted.
//   2. The derivation is right. Point height follows ui_scale, point width follows the viewport
//      aspect, P_ASPECT_RATIO is ALWAYS width/height (the stock EAGLView.mm bug this port must not
//      reproduce — it only recomputed it inside `if(IS_WIDESCREEN)`).
//   3. Re-layout actually happens, and is IDEMPOTENT. Setting the same metrics twice, and returning
//      to a previous size, must give identical rects — the marginLeft2-compounding trap.
//   4. Input's point space follows the engine's. A stale Input::scr_height flips every touch's Y.
//
// Usage: node tools/headless-display-profile-test.js [path/to/eden.js]  (defaults to ../build-st)
// Needs an EDEN_DIAGNOSTICS=ON tree (build-st) — eden_debug_display_state is compiled out of
// build-rel like every other probe in src/seam/DebugState_web.mm.
//
// Methodology note (web/CLAUDE.md "fast facts"): emscripten_set_main_loop is ALREADY running under
// node (fps=0 -> fakeRequestAnimationFrame, a real ~60 Hz setTimeout loop), so this script never
// calls _eden_debug_tick. It drives state and waits on wall-clock timers, same as the other
// harnesses in this directory.
'use strict';
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const positional = process.argv.slice(2).filter((a) => !a.startsWith('--'));
const edenJsPath = path.resolve(positional[0] || path.join(__dirname, '..', 'build-st', 'eden.js'));
const edenDir = path.dirname(edenJsPath);

global.require = require;
global.__dirname = edenDir;
global.__filename = edenJsPath;
global.Module = { print: () => {}, printErr: () => {} };

const cwdBefore = process.cwd();
process.chdir(edenDir);
vm.runInThisContext(fs.readFileSync(edenJsPath, 'utf8'), { filename: edenJsPath });
process.chdir(cwdBefore);

function utf8(ptr) {
    if (!ptr) return '';
    const heap = global.Module.HEAPU8;
    let end = ptr;
    while (heap[end] !== 0) end++;
    return Buffer.from(heap.buffer, heap.byteOffset + ptr, end - ptr).toString('utf8');
}

function waitUntil(predicate, timeoutMs, label) {
    return new Promise((resolve) => {
        const start = Date.now();
        const poll = () => {
            let ok = false;
            try { ok = predicate(); } catch (e) { /* not ready yet */ }
            if (ok) return resolve(true);
            if (Date.now() - start > timeoutMs) {
                console.log(`  (timed out waiting for: ${label})`);
                return resolve(false);
            }
            setTimeout(poll, 50);
        };
        poll();
    });
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

let failures = 0;
function check(name, cond, detail) {
    if (cond) console.log('PASS:', name);
    else { console.log('FAIL:', name, detail === undefined ? '' : `-- ${detail}`); failures++; }
}

const M = global.Module;
const state = () => JSON.parse(utf8(M._eden_debug_display_state()));

// Settings are addressed by INDEX (eden_settings_set takes one) — the schema is the key->index map.
let schema = null;
function settingIndex(key) {
    if (!schema) schema = JSON.parse(utf8(M._eden_settings_schema()));
    const row = schema.find((r) => r.key === key);
    if (!row) throw new Error(`no setting row named ${key}`);
    return row.i;
}
function setSetting(key, value) { M._eden_settings_set(settingIndex(key), value); }

// ui_scale option indexes: 0 Auto, 1 100%, 2 125%, 3 150%, 4 200%
// display_layout:          0 Auto, 1 Classic 16:9, 2 Adaptive
const UI_100 = 1, UI_125 = 2, UI_200 = 4;
const LAYOUT_CLASSIC = 1, LAYOUT_ADAPTIVE = 2;

const sameRect = (a, b) => a.length === b.length && a.every((v, i) => Math.abs(v - b[i]) < 0.01);

// The picker card (rpaintframe) is a fixed-size background for the colour/block grid. It has no
// hit-testing role, so the only thing that can go wrong is visual — and it goes wrong silently, in
// a mode the player has to open on purpose. Containment is the real invariant; asserting the card's
// coordinates instead would pass just as happily with the grid somewhere else entirely.
function checkGridInsideCard(label, s) {
    const [cx, cy, cw, ch] = s.rpaintframe;
    const rows = [s.block0, s.color0];
    // Only the TOP row of each grid is probed, which is the row nearest the card's top edge and the
    // one that escapes first; the grids extend downward from it toward the card's bottom edge.
    const ok = rows.every((r) => r[0] >= cx && r[1] >= cy &&
                                 r[0] + r[2] <= cx + cw && r[1] + r[3] <= cy + ch);
    check(`${label}: picker grid sits inside the picker card`, ok,
          `card=${JSON.stringify(s.rpaintframe)} block0=${JSON.stringify(s.block0)} ` +
          `color0=${JSON.stringify(s.color0)}`);
}

// The contract the GL shim's kPickViewport exists to hold, asserted at every point space this test
// visits. Util.mm's findWorldCoords scales a POINT-space tap by SCALE_* and unprojects it against
// whatever GL_VIEWPORT reports, so GL_VIEWPORT must be the point space times that same SCALE_* — it
// is deliberately NOT the real drawable (that decoupling is perf-audit item #6, and answering the
// drawable there is what once made mobile taps land left of the finger). SCALE_* is 2 on every
// profile this port produces. A wrong answer here misaims every click by a constant factor, which
// is invisible at the screen centre and grows toward the edges — exactly the bug a centre-of-screen
// smoke test cannot see.
function checkPickViewport(label, s) {
    check(`${label}: GL_VIEWPORT is the point space x2 (findWorldCoords contract)`,
          s.pick_viewport[0] === s.SCREEN_WIDTH * 2 && s.pick_viewport[1] === s.SCREEN_HEIGHT * 2,
          `${JSON.stringify(s.pick_viewport)} vs ${s.SCREEN_WIDTH}x${s.SCREEN_HEIGHT} x2`);
}

M.postRun = M.postRun || [];
M.postRun.push(async () => {
    const haveProbe = typeof M._eden_debug_display_state === 'function';
    check('eden_debug_display_state export present (EDEN_DIAGNOSTICS build)', haveProbe);
    if (!haveProbe) { console.log('\n(needs build-st; build-rel compiles the probes out)'); return finish(); }

    // The Hud only exists once the World has been constructed, which is a few frames into main().
    const ready = await waitUntil(() => state().hud === true, 20000, 'World/Hud constructed');
    check('World + Hud came up', ready);
    if (!ready) return finish();

    // ---- 1. The classic profile is bit-exact -------------------------------------------------
    // Force it rather than relying on the detected profile, so this leg means the same thing on any
    // machine: Classic aspect + ui_scale 200% is the definition of "what this port shipped".
    setSetting('display_layout', LAYOUT_CLASSIC);
    setSetting('ui_scale', UI_200);
    await sleep(100);
    const classic = state();
    check('classic: 568x320 points', classic.SCREEN_WIDTH === 568 && classic.SCREEN_HEIGHT === 320,
          `${classic.SCREEN_WIDTH}x${classic.SCREEN_HEIGHT}`);
    check('classic: P_ASPECT_RATIO is 1.775', Math.abs(classic.P_ASPECT_RATIO - 1.775) < 0.001,
          classic.P_ASPECT_RATIO);
    check('classic: IS_WIDESCREEN true', classic.IS_WIDESCREEN === 1);
    check('classic: Input point space matches the engine',
          classic.scr[0] === 568 && classic.scr[1] === 320, JSON.stringify(classic.scr));
    // The stock rects, computed by hand from Classes/Hud.mm's constants at 568x320:
    //   HUDR_X          = 568 - 45 - 13            = 510
    //   rmine.y         = 4*320/5 - 45 - 3         = 208
    //   rjumprender.x   = 510 - 17                 = 493   -> rjumphit.x = 492, w = 568-492 = 76
    //   rmenu.y         = 320 - 45                 = 275
    //   blockBounds[0]  = 7 + 15 + (4 + 57)        = 83   ; y = -3 + 320 - 10 - 15 - 38 = 254
    check('classic: rmine rect is the stock one', sameRect(classic.rmine, [510, 208, 45, 45]),
          JSON.stringify(classic.rmine));
    check('classic: rjumphit rect is the stock one', sameRect(classic.rjumphit, [492, 0, 76, 68]),
          JSON.stringify(classic.rjumphit));
    check('classic: rmenu rect is the stock one', sameRect(classic.rmenu, [0, 275, 45, 45]),
          JSON.stringify(classic.rmenu));
    check('classic: block picker cell 0 is the stock one',
          sameRect(classic.block0, [83, 254, 38, 38]), JSON.stringify(classic.block0));
    //   rburn.y  = 5*320/5 - 45 - 3               = 272   (flush against the top edge)
    //   rpaint.y = 2*320/5 - 45 - 3 + 5           = 85    (three 64-point steps below it)
    //   rpaintframe = (marginLeft2 4+57, marginVert+10) 402x282 = [61, 20, 402, 282]
    check('classic: mode column top button is the stock one',
          sameRect(classic.rburn, [510, 272, 45, 45]), JSON.stringify(classic.rburn));
    check('classic: mode column bottom button is the stock one',
          sameRect(classic.rpaint, [510, 85, 45, 45]), JSON.stringify(classic.rpaint));
    check('classic: picker card is the stock one',
          sameRect(classic.rpaintframe, [61, 20, 402, 282]), JSON.stringify(classic.rpaintframe));
    checkPickViewport('classic', classic);

    // ---- 2. The derivation ---------------------------------------------------------------------
    // Adaptive at a 16:10 desktop window. Point height comes from ui_scale (640 at 100%), width from
    // the viewport's aspect, rounded to an even number.
    setSetting('display_layout', LAYOUT_ADAPTIVE);
    setSetting('ui_scale', UI_100);
    M._eden_display_set_viewport(1600, 1000);
    await sleep(100);
    const wide = state();
    check('adaptive/100%: point height is 640', wide.SCREEN_HEIGHT === 640, wide.SCREEN_HEIGHT);
    check('adaptive/100%: point width follows the 1.6 viewport aspect',
          wide.SCREEN_WIDTH === 1024, wide.SCREEN_WIDTH);
    check('adaptive: P_ASPECT_RATIO is always width/height',
          Math.abs(wide.P_ASPECT_RATIO - wide.SCREEN_WIDTH / wide.SCREEN_HEIGHT) < 0.0005,
          wide.P_ASPECT_RATIO);
    check('adaptive: the page-facing aspect getter agrees',
          Math.abs(wide.aspect_x1000 / 1000 - 1.6) < 0.002, wide.aspect_x1000);
    check('adaptive: Input point space followed', wide.scr[0] === 1024 && wide.scr[1] === 640,
          JSON.stringify(wide.scr));
    // The right-hand mode column is anchored to the right edge, so it MUST have moved with the width.
    check('adaptive: right-anchored HUD column followed the width',
          Math.abs(wide.rmine[0] - (1024 - 45 - 13)) < 0.01, JSON.stringify(wide.rmine));
    // ...and the top-left menu icon with the height.
    check('adaptive: top-anchored HUD icon followed the height',
          Math.abs(wide.rmenu[1] - (640 - 45)) < 0.01, JSON.stringify(wide.rmenu));
    // The in-game menu card keeps its classic size and centres rather than stretching.
    checkPickViewport('adaptive/100%', wide);
    // THE COUPLING THAT BROKE FIRST IN A REAL BROWSER (2026-07-31): the picker card is drawn behind
    // the swatch/block grid, but the card was anchored to the BOTTOM of the screen by a constant Y
    // while the grid is anchored to the TOP via SCREEN_HEIGHT. They coincide at 320 points and
    // separate at every other height — the card slid out from under its own contents. Assert
    // containment, not a magic number: whatever the point space, the grid must sit inside the card.
    checkGridInsideCard('adaptive/100%', wide);
    // The four mode buttons keep the 64-point pitch of the 320-point layout instead of spreading
    // proportionally, and stay anchored to the top edge.
    // rpaint carries a stock +5 nudge on top of its three 64-point steps, hence the term.
    check('adaptive: mode column keeps its 64-point pitch (not proportional to height)',
          Math.abs((wide.rburn[1] - wide.rpaint[1] + 5) - 64 * 3) < 0.01,
          `rburn.y=${wide.rburn[1]} rpaint.y=${wide.rpaint[1]}`);
    check('adaptive: mode column stays anchored to the top edge',
          Math.abs(wide.rburn[1] - (wide.SCREEN_HEIGHT - 45 - 3)) < 0.01,
          `rburn.y=${wide.rburn[1]} of ${wide.SCREEN_HEIGHT}`);
    check('adaptive: in-game menu card kept its classic 268x240 size',
          Math.abs(wide.rmenuframe[2] - 268) < 0.01 && Math.abs(wide.rmenuframe[3] - 240) < 0.01,
          JSON.stringify(wide.rmenuframe));

    // ui_scale is what moves UI DENSITY: same window, larger scale, smaller point space, and every
    // absolutely-sized rect therefore covers a larger fraction of it.
    setSetting('ui_scale', UI_125);
    await sleep(100);
    const dense = state();
    check('ui_scale 125%: point height is 512', dense.SCREEN_HEIGHT === 512, dense.SCREEN_HEIGHT);
    check('ui_scale 125%: aspect is unchanged by the scale change',
          Math.abs(dense.P_ASPECT_RATIO - wide.P_ASPECT_RATIO) < 0.005,
          `${dense.P_ASPECT_RATIO} vs ${wide.P_ASPECT_RATIO}`);
    checkPickViewport('ui_scale 125%', dense);
    checkGridInsideCard('ui_scale 125%', dense);

    // ---- 3. Re-layout is idempotent, and reversible ---------------------------------------------
    setSetting('ui_scale', UI_100);
    M._eden_display_set_viewport(1600, 1000);
    await sleep(100);
    const again = state();
    check('idempotent: returning to a previous size reproduces its rects exactly',
          sameRect(again.rmine, wide.rmine) && sameRect(again.block0, wide.block0) &&
          sameRect(again.color0, wide.color0),
          `${JSON.stringify(again.block0)} vs ${JSON.stringify(wide.block0)}`);
    // The margin-compounding trap specifically: block0.x is the one carrying marginLeft2, which the
    // layout body MUTATES. A second pass that failed to reset it would drift right every time.
    M._eden_display_refresh();
    M._eden_display_refresh();
    await sleep(50);
    check('idempotent: two extra refreshes do not drift the margin-carrying rect',
          sameRect(state().block0, wide.block0),
          `${JSON.stringify(state().block0)} vs ${JSON.stringify(wide.block0)}`);

    // ---- 4. Back to classic, and the numbers must be the stock ones again -----------------------
    setSetting('display_layout', LAYOUT_CLASSIC);
    setSetting('ui_scale', UI_200);
    await sleep(100);
    const back = state();
    check('round trip: classic rects are bit-identical after a tour through adaptive',
          sameRect(back.rmine, classic.rmine) && sameRect(back.rjumphit, classic.rjumphit) &&
          sameRect(back.rmenu, classic.rmenu) && sameRect(back.block0, classic.block0) &&
          sameRect(back.color0, classic.color0),
          `${JSON.stringify(back.block0)} vs ${JSON.stringify(classic.block0)}`);

    // ---- 5. The profile is a real object, and drives the control chrome -------------------------
    // input_mode: 0 Auto, 1 Touch, 2 Keyboard+Mouse. Forcing it forces the profile.
    setSetting('input_mode', 1);
    await sleep(100);
    const touch = state();
    check('profile: forcing Touch selects the touch profile', touch.profile === 'touch',
          touch.profile);
    check('profile: touch profile turns the on-screen control chrome on',
          touch.touch_chrome === 1 && touch.use_joystick === 1,
          `chrome=${touch.touch_chrome} use_joystick=${touch.use_joystick}`);
    setSetting('input_mode', 2);
    await sleep(100);
    const desk = state();
    check('profile: forcing Keyboard+Mouse selects the desktop profile', desk.profile === 'desktop',
          desk.profile);
    check('profile: desktop profile turns the on-screen control chrome off',
          desk.touch_chrome === 0 && desk.use_joystick === 0,
          `chrome=${desk.touch_chrome} use_joystick=${desk.use_joystick}`);

    // Auto (0) + the profile's own defaults. The touch profile's layout row used to be Classic,
    // so this pair asserted "568x320 on ANY viewport" — the "changing nothing changes nothing"
    // promise. It is Adaptive since 2026-09-07 (a 4:3 iPad reported black bars, STATUS §2.0.12
    // bug 4), so the promise is now stated where it is actually about the shipped device: on a
    // 16:9 viewport, touch + Auto still reproduces 568x320 and the shipped HUD rects EXACTLY.
    setSetting('ui_scale', 0);
    setSetting('display_layout', 0);
    setSetting('input_mode', 1);
    M._eden_display_set_viewport(1136, 640);
    await sleep(100);
    const autoTouch = state();
    check('profile: Auto + touch on a 16:9 screen is still the shipped 568x320 layout',
          autoTouch.SCREEN_WIDTH === 568 && autoTouch.SCREEN_HEIGHT === 320,
          `${autoTouch.SCREEN_WIDTH}x${autoTouch.SCREEN_HEIGHT}`);
    check('profile: Auto + touch reproduces the shipped HUD rects',
          sameRect(autoTouch.rmine, classic.rmine) && sameRect(autoTouch.block0, classic.block0),
          JSON.stringify(autoTouch.block0));

    // ...and on a 4:3 screen it FILLS it instead of letterboxing to 16:9 — which is the bug the
    // device reported — while the CLASSIC-WIDTH FLOOR keeps the point space at least as wide as
    // the 568 the absolutely-sized HUD rects were drawn against. 1024/768 -> 568x426.
    M._eden_display_set_viewport(1024, 768);
    await sleep(100);
    const ipad = state();
    check('profile: Auto + touch on a 4:3 screen fills it rather than pinning 16:9',
          Math.abs(ipad.aspect_x1000 / 1000 - 4 / 3) < 0.005,
          `${ipad.SCREEN_WIDTH}x${ipad.SCREEN_HEIGHT} ar=${ipad.aspect_x1000}`);
    check('profile: the classic-width floor keeps the point space >= 568 wide',
          ipad.SCREEN_WIDTH >= 568,
          `${ipad.SCREEN_WIDTH}x${ipad.SCREEN_HEIGHT}`);
    // The floor has to hold at the narrowest aspect the clamp allows too (1.20), or a tall screen
    // walks the HUD off the right edge instead of a wide one.
    M._eden_display_set_viewport(800, 1200);
    await sleep(100);
    const tall = state();
    check('profile: the classic-width floor holds at the aspect clamp',
          tall.SCREEN_WIDTH >= 568, `${tall.SCREEN_WIDTH}x${tall.SCREEN_HEIGHT}`);

    M._eden_display_set_viewport(1600, 1000);
    await sleep(100);

    setSetting('input_mode', 2);
    await sleep(100);
    const autoDesk = state();
    // Tolerance is a full point of width, not a hair: the derived width is rounded to an EVEN
    // integer (the 2D passes project through glOrthof(0, SCREEN_WIDTH*2, ...)), so at a 512-point
    // height the aspect can legitimately land up to 2/512 off the window's own.
    check('profile: Auto + desktop takes the window aspect and the 125% scale',
          autoDesk.SCREEN_HEIGHT === 512 && Math.abs(autoDesk.aspect_x1000 / 1000 - 1.6) < 0.005,
          `${autoDesk.SCREEN_WIDTH}x${autoDesk.SCREEN_HEIGHT} ar=${autoDesk.aspect_x1000}`);

    // ---- 5b. The density floor (reported from live play, 2026-07-31) ----------------------------
    // ui_scale alone gives a FIXED point space, i.e. a UI that is a fixed FRACTION of the canvas —
    // so shrinking the window shrinks every HUD icon with it. Resizing a desktop browser to phone
    // proportions does not flip the touch profile (nothing about a resize makes the pointer
    // coarse), so the desktop profile's 512-point space stays in force and the mode buttons come
    // out at ~35 CSS px and keep going. The floor says one point is never smaller than one CSS
    // pixel, which is exactly the density the art was drawn at (iPhone-5 points ARE CSS pixels).
    setSetting('display_layout', LAYOUT_ADAPTIVE);
    setSetting('ui_scale', UI_125);
    M._eden_display_set_viewport(1600, 1000);
    await sleep(100);
    check('density floor: a large window is unaffected by it', state().SCREEN_HEIGHT === 512,
          state().SCREEN_HEIGHT);
    // 900x420 — a landscape phone-shaped box. Without the floor this would still be 512 points, so
    // a 45-point button would render at 45*420/512 = 37 CSS px.
    M._eden_display_set_viewport(900, 420);
    await sleep(100);
    const small = state();
    check('density floor: a phone-shaped window drops the point space to match the box',
          small.SCREEN_HEIGHT <= 420 && small.SCREEN_HEIGHT >= 418, small.SCREEN_HEIGHT);
    check('density floor: a HUD point is therefore never below one CSS pixel',
          420 / small.SCREEN_HEIGHT >= 0.999,
          `${(420 / small.SCREEN_HEIGHT).toFixed(3)} css px per point`);
    check('density floor: the layout still followed it (mode column re-anchored)',
          Math.abs(small.rburn[1] - (small.SCREEN_HEIGHT - 45 - 3)) < 0.01,
          `rburn.y=${small.rburn[1]} of ${small.SCREEN_HEIGHT}`);
    checkGridInsideCard('density floor', small);
    checkPickViewport('density floor', small);
    // The floor must not disturb the touch profile's bit-exact classic layout at any real phone
    // size: the shortest landscape phone viewport is 320 CSS px, which is exactly 320 points.
    setSetting('ui_scale', 0);
    setSetting('display_layout', 0);
    setSetting('input_mode', 1);
    M._eden_display_set_viewport(568, 320);
    await sleep(100);
    check('density floor: an iPhone-5-sized viewport still resolves to exactly 568x320',
          state().SCREEN_WIDTH === 568 && state().SCREEN_HEIGHT === 320,
          `${state().SCREEN_WIDTH}x${state().SCREEN_HEIGHT}`);
    setSetting('input_mode', 2);

    // ---- 6. The clamps ---------------------------------------------------------------------------
    // A portrait phone box must not hand the engine a portrait point space; it clamps, and the page
    // letterboxes to the clamped aspect (which is why the getter has to report the clamped value).
    M._eden_display_set_viewport(400, 900);
    await sleep(100);
    const portrait = state();
    check('clamp: a portrait viewport is clamped to the minimum landscape aspect',
          Math.abs(portrait.aspect_x1000 / 1000 - 1.20) < 0.01, portrait.aspect_x1000);
    M._eden_display_set_viewport(3840, 1080);
    await sleep(100);
    const ultrawide = state();
    check('clamp: a 32:9 viewport is clamped to the maximum aspect',
          Math.abs(ultrawide.aspect_x1000 / 1000 - 2.40) < 0.01, ultrawide.aspect_x1000);

    // ---- 6. ROADMAP Phase M / M5.2 — the low-memory overlay flag ---------------------------------
    // The page sets this before eden_settings_init() so DisplayProfile_web.mm's
    // kProfiles[EDEN_PROFILE_LOWMEM] row seeds a leaner video preset. The seeding itself is a
    // one-shot that has already fired by the time this test can toggle the flag (so it is a browser-
    // leg check), but the export contract the page depends on is guarded here.
    check('lowmem: the flag exports exist',
          typeof M._eden_set_low_memory === 'function' && typeof M._eden_low_memory === 'function',
          `${typeof M._eden_set_low_memory}/${typeof M._eden_low_memory}`);
    M._eden_set_low_memory(1);
    check('lowmem: eden_low_memory() follows eden_set_low_memory(1)', M._eden_low_memory() === 1,
          M._eden_low_memory());
    M._eden_set_low_memory(0);
    check('lowmem: eden_low_memory() follows eden_set_low_memory(0)', M._eden_low_memory() === 0,
          M._eden_low_memory());

    finish();
});

function finish() {
    console.log(failures === 0 ? '\nALL PASS' : `\n${failures} FAILURE(S)`);
    process.exit(failures === 0 ? 0 : 1);
}
