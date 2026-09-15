// Headless end-to-end check of the pass-44 DOM-menu backend (src/seam/Menu_web.mm) via node,
// following PORT-STATUS's documented vm.runInThisContext harness (plain require() does not share
// Module — see "Headless driving" there). Exercises exactly the flow public/eden-menu.js drives
// through the C exports, without a browser: create world -> set pending type (flat) -> play ->
// wait for GAME_MODE_PLAY -> quit to menu -> confirm the world is listed with its real name ->
// delete it. This is the engine-API-level confirmation pass 44 left owed ("actually creating+
// loading a world through the new UI end-to-end wasn't confirmed this pass") — it does NOT drive
// eden-menu.js's own DOM/JS, only the C backend it calls into.
//
// Usage: node tools/headless-menu-flow-test.js [path/to/eden.js]  (defaults to ../build-st/eden.js)
//
// Methodology note (see PORT-STATUS "Distilled hard-won knowledge" / web/CLAUDE.md "fast facts"):
// main()'s emscripten_set_main_loop is ALREADY running automatically under node (fps=0 falls back
// to MainLoop.fakeRequestAnimationFrame, a real setTimeout(~60Hz) loop — there is no document/rAF
// to throttle it, but it is not idle either). So this script does NOT call Module._eden_debug_tick
// at all — mixing manual ticks on top of an already-running loop is exactly the double-tick hazard
// that produced spurious crashes in a past pass. Instead it drives state changes and waits on real
// wall-clock timers, the same recipe used for a live browser tab.
'use strict';
const fs = require('fs');
const path = require('path');
const vm = require('vm');

// --fps-cap=N (30/45/60) turns the frame-rate cap on before the flow runs — the audit-row-A4
// regression leg. With the cap on, eden_main.cpp's gate render-skips roughly half of node's
// ~60 Hz fake-rAF ticks, and World::renderFrame(FALSE) is then the only thing still running the
// GAME_MODE_WAIT -> target_game_mode transition. If that side effect were ever lost again, the
// load below would sit in WAIT forever and 'game_mode reached GAME_MODE_PLAY' would time out.
const args = process.argv.slice(2);
const capArg = args.find((a) => a.startsWith('--fps-cap='));
const fpsCap = capArg ? parseInt(capArg.split('=')[1], 10) : 0;
const positional = args.filter((a) => !a.startsWith('--'));
const edenJsPath = path.resolve(positional[0] || path.join(__dirname, '..', 'build-st', 'eden.js'));
const edenDir = path.dirname(edenJsPath);

global.require = require;
global.__dirname = edenDir;
global.__filename = edenJsPath;
global.Module = {
    print: (t) => console.log('[out]', t),
    printErr: (t) => console.log('[err]', t),
};

const cwdBefore = process.cwd();
process.chdir(edenDir); // eden.js resolves eden.wasm/eden.data relative to cwd under node
const src = fs.readFileSync(edenJsPath, 'utf8');
vm.runInThisContext(src, { filename: edenJsPath });
process.chdir(cwdBefore);

function utf8(ptr) {
    if (!ptr) return '';
    const heap = global.Module.HEAPU8;
    let end = ptr;
    while (heap[end] !== 0) end++;
    return Buffer.from(heap.buffer, heap.byteOffset + ptr, end - ptr).toString('utf8');
}

function menuState() {
    return JSON.parse(utf8(global.Module._eden_debug_menu_state()));
}

function waitUntil(predicate, timeoutMs, label) {
    return new Promise((resolve) => {
        const start = Date.now();
        const poll = () => {
            let ok = false;
            try { ok = predicate(); } catch (e) { console.log('  (predicate threw:', e.message, ')'); }
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

let failures = 0;
function check(name, cond) {
    if (cond) { console.log('PASS:', name); }
    else { console.log('FAIL:', name); failures++; }
}

global.Module.postRun = global.Module.postRun || [];
global.Module.postRun.push(async () => {
    // main() has returned; World/Menu are constructed and the fake-rAF loop is already ticking.
    const haveWorldAndMenu = await waitUntil(() => !menuState().error, 5000, 'World/Menu to exist');
    check('World/Menu exist after main()', haveWorldAndMenu);
    if (!haveWorldAndMenu) { console.log('FATAL, aborting'); process.exit(1); }

    if (fpsCap > 0) {
        const schema = JSON.parse(utf8(global.Module._eden_settings_schema()));
        const rows = Array.isArray(schema) ? schema : schema.settings;
        const fpsIdx = rows.findIndex((r) => r.key === 'fps_cap');
        const optIdx = ['0', '30', '45', '60'].indexOf(String(fpsCap));
        check('fps_cap row found in the settings schema', fpsIdx >= 0);
        check(`--fps-cap=${fpsCap} is one of the enum's options`, optIdx > 0);
        global.Module._eden_settings_set(fpsIdx, optIdx);
        check(`frame-rate cap is live at ${fpsCap} fps`,
            global.Module._eden_get_fps_cap() === fpsCap);
    }

    const before = global.Module._eden_menu_world_count();
    console.log('world_count before:', before);

    const idx = global.Module._eden_menu_create_world();
    check('create_world returned a valid index', idx >= 0);

    const nameAfterCreate = utf8(global.Module._eden_menu_world_name(idx));
    console.log('created world name:', JSON.stringify(nameAfterCreate));
    check('created world has a non-empty display name', nameAfterCreate.length > 0);
    check('world_count incremented by 1', global.Module._eden_menu_world_count() === before + 1);

    global.Module._eden_menu_set_pending_world_type(1); // flat
    const playOk = global.Module._eden_menu_play();
    check('play() accepted (returns 1)', playOk === 1);

    const sawPlay = await waitUntil(() => menuState().game_mode === 1 /* GAME_MODE_PLAY */,
        20000, 'game_mode == GAME_MODE_PLAY');
    check('game_mode reached GAME_MODE_PLAY after create+play', sawPlay);
    if (!sawPlay) console.log('last menu state:', JSON.stringify(menuState()));

    // Quit back to the menu the same way the pause menu's "Quit to Menu" does: rexit (which=6)
    // only takes effect while hud->inmenu is true (Input_web.mm), so open the in-game menu
    // (which=0, rmenu) first, same as eden-pausemenu.js's open()/close() do.
    global.Module._eden_tap_hud_button_begin(0);
    await new Promise((r) => setTimeout(r, 100));
    global.Module._eden_tap_hud_button_end(0);
    await new Promise((r) => setTimeout(r, 100));
    console.log('hud->inmenu after rmenu tap:', global.Module._eden_hud_in_menu());

    global.Module._eden_tap_hud_button_begin(6);
    await new Promise((r) => setTimeout(r, 100));
    global.Module._eden_tap_hud_button_end(6);

    const sawMenu = await waitUntil(() => menuState().game_mode === 0 /* GAME_MODE_MENU */,
        10000, 'game_mode back to GAME_MODE_MENU');
    check('back in GAME_MODE_MENU after quit', sawMenu);

    // Confirm the created world is now listed with its real name (the item pass 44 closed).
    const countAfterQuit = global.Module._eden_menu_world_count();
    let foundIdx = -1;
    for (let i = 0; i < countAfterQuit; i++) {
        if (utf8(global.Module._eden_menu_world_name(i)) === nameAfterCreate) { foundIdx = i; break; }
    }
    check('created+played world is listed by its real name after returning to menu', foundIdx >= 0);

    if (foundIdx >= 0) {
        const delOk = global.Module._eden_menu_delete_at(foundIdx);
        check('delete_at() accepted', delOk === 1);
        const countAfterDelete = global.Module._eden_menu_world_count();
        check('world_count decremented after delete', countAfterDelete === countAfterQuit - 1);
    }

    console.log(failures === 0 ? 'ALL PASS' : `${failures} FAILURE(S)`);
    process.exit(failures === 0 ? 0 : 1);
});
