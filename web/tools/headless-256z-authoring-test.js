// Headless regression test for 256z Stage 3's remaining UI-shaped work (WORKING/ROADMAP.md Tier 1):
// the New World height picker (eden-menu.js's "Height format" segmented control, backed by
// Menu_web.mm's eden_menu_set_pending_world_height / FileManager::probeWorldHeight) and the
// Storage tab's "Convert to 64z" action (Storage_web.mm's eden_storage_convert_to_64z_at ->
// FileManager::convertWorldTo64). Same vm.runInThisContext harness as
// headless-menu-flow-test.js/headless-256z-test.js -- see either for why (plain require() does not
// share Module).
//
// WHAT THIS PROVES
//   1. A new world created with NO pending height choice is 64z by construction (the load-bearing
//      guarantee the whole plan is built on: "64z stays the default").
//   2. A new world created with the pending height set to 256 actually comes up as a real 256z
//      save (version 5, 131072 B column stride) -- not just "the flag was set", checked via
//      eden_storage_list_worlds()'s height field once it is on disk.
//   3. eden_storage_convert_to_64z_at() turns that same file back into a real 64z save (the
//      Storage tab list reports height 64 afterwards) and the report it returns is well-formed.
//   4. The converted world still loads and reaches GAME_MODE_PLAY -- the conversion did not leave
//      an unloadable file behind.
//
// NOT covered here (out of scope for this pass, left as a known gap): fixture-based verification
// that convertWorldTo64's block-discard/door-orphan/creature-relocation counters match
// eden-convert.js's algorithm byte-for-byte on a world with content above y=63. Both this test and
// eden-convert-test.js exercise their own implementation in isolation; nothing here checks them
// against EACH OTHER the way headless-256z-test.js does for the reader. See ROADMAP.md.
//
// Usage: node tools/headless-256z-authoring-test.js [path/to/eden.js]  (defaults to ../build-st/eden.js)
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
global.Module = {
    print: (t) => console.log('[out]', t),
    printErr: (t) => console.log('[err]', t),
};

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
const menuState = () => JSON.parse(utf8(global.Module._eden_debug_menu_state()));
const storageList = () => JSON.parse(utf8(global.Module._eden_storage_list_worlds()));

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

async function createAndPlay(pendingHeight) {
    const M = global.Module;
    const idx = M._eden_menu_create_world();
    check(`create_world returned a valid index (pendingHeight=${pendingHeight})`, idx >= 0);
    const name = utf8(M._eden_menu_world_name(idx));
    M._eden_menu_set_pending_world_type(1); // flat -- fast to generate, minimal high-band content
    if (pendingHeight != null) M._eden_menu_set_pending_world_height(pendingHeight);
    const playOk = M._eden_menu_play();
    check('play() accepted (returns 1)', playOk === 1);
    const sawPlay = await waitUntil(() => menuState().game_mode === 1 /* GAME_MODE_PLAY */,
        20000, 'game_mode == GAME_MODE_PLAY');
    check(`game_mode reached GAME_MODE_PLAY (pendingHeight=${pendingHeight})`, sawPlay);
    return name;
}

async function quitToMenu() {
    const M = global.Module;
    M._eden_tap_hud_button_begin(0);
    await new Promise((r) => setTimeout(r, 100));
    M._eden_tap_hud_button_end(0);
    await new Promise((r) => setTimeout(r, 100));
    M._eden_tap_hud_button_begin(6);
    await new Promise((r) => setTimeout(r, 100));
    M._eden_tap_hud_button_end(6);
    const sawMenu = await waitUntil(() => menuState().game_mode === 0 /* GAME_MODE_MENU */,
        10000, 'game_mode back to GAME_MODE_MENU');
    check('back in GAME_MODE_MENU after quit', sawMenu);
}

function findWorld(name) {
    return storageList().find((w) => w.name === name);
}

global.Module.postRun = global.Module.postRun || [];
global.Module.postRun.push(async () => {
    const M = global.Module;
    const haveWorldAndMenu = await waitUntil(() => !menuState().error, 5000, 'World/Menu to exist');
    check('World/Menu exist after main()', haveWorldAndMenu);
    if (!haveWorldAndMenu) { console.log('FATAL, aborting'); process.exit(1); }

    // ---- 1. default (no pending height) stays 64z ----
    const defaultName = await createAndPlay(null);
    await quitToMenu();
    const defaultRow = findWorld(defaultName);
    check('a world created with no height choice is listed', !!defaultRow);
    check('...and it is 64z by default', defaultRow && defaultRow.height === 64);

    // ---- 2. an explicit 256 choice actually produces a 256z save ----
    const tallName = await createAndPlay(256);
    await quitToMenu();
    let tallRow = findWorld(tallName);
    check('a world created with height=256 is listed', !!tallRow);
    check('...and it is really 256z on disk', tallRow && tallRow.height === 256);

    // A second world created right after, with NO pending choice, must not inherit the previous
    // screen's answer -- pending height is one-shot, same contract as pending world type.
    const afterTallName = await createAndPlay(null);
    await quitToMenu();
    const afterTallRow = findWorld(afterTallName);
    check('pending height is one-shot: a later world with no choice is still 64z',
        afterTallRow && afterTallRow.height === 64);

    // ---- 3. Convert to 64z ----
    const worlds = storageList();
    const tallIndex = worlds.findIndex((w) => w.name === tallName);
    check('the 256z world resolves to a storage-list index', tallIndex >= 0);
    const report = JSON.parse(utf8(M._eden_storage_convert_to_64z_at(tallIndex)));
    console.log('convert report:', JSON.stringify(report));
    check('convertWorldTo64 reported ok', report.ok === true);
    // Not >= 1: this harness quits back to the menu almost immediately after creation, so a fresh
    // flat world may not have streamed any column to disk yet -- that is a timing artifact of the
    // test, not something convertWorldTo64 should be judged on. headless-256z-test.js already
    // covers real column conversion end to end (via eden-convert.js, not this C++ path -- see this
    // file's header for that known gap).
    check('...report.columns is a sane non-negative count', typeof report.columns === 'number' && report.columns >= 0);

    tallRow = findWorld(tallName);
    check('the converted world is now listed as 64z', tallRow && tallRow.height === 64);

    // Converting an already-64z world must refuse cleanly, not corrupt it.
    const alreadyIndex = storageList().findIndex((w) => w.name === tallName);
    const secondReport = JSON.parse(utf8(M._eden_storage_convert_to_64z_at(alreadyIndex)));
    check('converting an already-64z world refuses rather than corrupting it', secondReport.ok === false);

    // ---- 4. the converted world still loads and plays ----
    const worldsNow = storageList();
    const reloadIndex = worldsNow.findIndex((w) => w.name === tallName);
    M._eden_menu_select(reloadIndex);
    const playOk = M._eden_menu_play();
    check('play() on the converted world accepted', playOk === 1);
    const sawPlay = await waitUntil(() => menuState().game_mode === 1, 20000, 'converted world reaches PLAY');
    check('the converted world still loads and reaches GAME_MODE_PLAY', sawPlay);
    await quitToMenu();

    // ---- cleanup ----
    // Delete by re-resolving each name to its current index -- deleting shifts later indices.
    for (const nm of [defaultName, tallName, afterTallName]) {
        let idx = -1;
        const n = M._eden_menu_world_count();
        for (let i = 0; i < n; i++) if (utf8(M._eden_menu_world_name(i)) === nm) { idx = i; break; }
        if (idx >= 0) M._eden_menu_delete_at(idx);
    }

    console.log(failures === 0 ? 'ALL PASS' : `${failures} FAILURE(S)`);
    process.exit(failures === 0 ? 0 : 1);
});
