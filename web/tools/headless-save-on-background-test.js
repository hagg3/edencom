// Headless save-on-background check (Phase N Stage 4.2). Proves the new engine entry point
// `eden_app_will_background()` (src/seam/AppLifecycle_web.mm) both SAVES when it should and
// REFUSES when it should not.
//
// Why this test exists in this shape: the thing being added is a save that fires from a page
// lifecycle event, and a lifecycle event is precisely what a headless run cannot dispatch
// realistically. So this drives the ENGINE side directly — the same function public/eden-host.js's
// edenSaveOpenWorld() calls, reached through eden-storage.js's flushNow() — and asserts on VALUES
// that only a real save can produce: the on-disk block-data region growing past 32 KB, and then
// coming back byte-identical after a real quit/reload. ROADMAP's calibration list has a rule about
// this ("a gate that greps for a word is not a gate"); a status code alone would be that gate,
// which is why eden_app_background_save_count() is asserted alongside it and the file bytes are
// asserted alongside both.
//
// Usage: node tools/headless-save-on-background-test.js [path/to/eden.js]  (defaults to ../build-st/eden.js)
'use strict';
const fs = require('fs');
const path = require('path');
const vm = require('vm');

// Mirrors src/seam/AppLifecycle_web.h. Kept as named constants so a FAIL line says which wrong
// answer came back rather than an unexplained integer.
const BG_SAVED = 0, BG_NO_WORLD = 1, BG_BUSY = 2, BG_NO_ENGINE = -1;
const BG_NAME = { 0: 'SAVED', 1: 'NO_WORLD', 2: 'BUSY', '-1': 'NO_ENGINE' };

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

function menuState() { return JSON.parse(utf8(global.Module._eden_debug_menu_state())); }
function background() { return global.Module._eden_app_will_background(); }
function saveCount() { return global.Module._eden_app_background_save_count(); }
function lastStatus() { return global.Module._eden_app_background_last_status(); }

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
function checkStatus(name, got, want) {
    check(`${name} (got ${BG_NAME[got] !== undefined ? BG_NAME[got] : got}, want ${BG_NAME[want]})`, got === want);
}

function findWorldFile(displayName) {
    const list = JSON.parse(utf8(global.Module._eden_storage_list_worlds()));
    return list.find((w) => w.name === displayName) || null;
}

// Same region arithmetic (and the same reasoning about why the header and creature block are NOT
// comparable) as tools/headless-save-roundtrip-test.js — see that file's header.
const CREATURE_BLOCK_BYTES = 200 * 60;
const HEADER_BYTES = 192;
function readBlockDataRegion(fileBytes) {
    const dv = new DataView(fileBytes.buffer, fileBytes.byteOffset, fileBytes.byteLength);
    const lo = dv.getUint32(32, true);
    const hi = dv.getUint32(36, true);
    const directoryOffset = hi * 4294967296 + lo;
    const blockDataEnd = directoryOffset - CREATURE_BLOCK_BYTES;
    if (!(blockDataEnd > HEADER_BYTES) || !(blockDataEnd <= fileBytes.length)) {
        return { ok: false, directoryOffset, blockDataEnd };
    }
    return { ok: true, directoryOffset, blockDataEnd,
             bytes: Buffer.from(fileBytes.subarray(HEADER_BYTES, blockDataEnd)) };
}

function tapHud(which) {
    return new Promise((resolve) => {
        global.Module._eden_tap_hud_button_begin(which);
        setTimeout(() => {
            global.Module._eden_tap_hud_button_end(which);
            setTimeout(resolve, 100);
        }, 100);
    });
}
async function ensureMenuOpen() {
    if (global.Module._eden_hud_in_menu() === 0) await tapHud(0);
}

global.Module.postRun = global.Module.postRun || [];
global.Module.postRun.push(async () => {
    const haveWorldAndMenu = await waitUntil(() => !menuState().error, 5000, 'World/Menu to exist');
    check('World/Menu exist after main()', haveWorldAndMenu);
    if (!haveWorldAndMenu) { console.log('FATAL, aborting'); process.exit(1); }

    // ---------------------------------------------------------------------------------------
    // 1. On the main menu: refuse, and do not count a save.
    // ---------------------------------------------------------------------------------------
    checkStatus('backgrounding on the main menu refuses', background(), BG_NO_WORLD);
    check('no save was counted on the main menu', saveCount() === 0);
    check('eden_app_background_last_status() agrees with the return value',
        lastStatus() === BG_NO_WORLD);

    // ---------------------------------------------------------------------------------------
    // 2. Enter a world, edit one block, and background: this must write.
    // ---------------------------------------------------------------------------------------
    const idx = global.Module._eden_menu_create_world();
    check('create_world returned a valid index', idx >= 0);
    const displayName = utf8(global.Module._eden_menu_world_name(idx));
    check('created world has a non-empty display name', displayName.length > 0);

    global.Module._eden_menu_clear_pending_world_type();
    check('play() accepted (returns 1)', global.Module._eden_menu_play() === 1);

    // While the load is in flight the answer must be BUSY, never SAVED — a save here would write a
    // header for a world that is not fully in memory. Best-effort: the poll can legitimately miss
    // the window on a fast machine, so a miss is reported, not failed.
    // The game_mode re-check inside the poll is load-bearing: the world can reach PLAY between two
    // of this timer's ticks, and a background() call after that point legitimately SAVES — which
    // would then be counted against the "nothing was saved while loading" assertion below and make
    // this test fail on its own timing rather than on the engine's behaviour.
    let sawBusy = false;
    const busyPoll = setInterval(() => {
        if (menuState().game_mode === 1) return;
        if (background() === BG_BUSY) sawBusy = true;
    }, 25);

    const sawPlay1 = await waitUntil(() => menuState().game_mode === 1, 20000, 'game_mode == PLAY');
    clearInterval(busyPoll);
    check('game_mode reached PLAY after create+play', sawPlay1);
    if (!sawPlay1) { console.log(failures, 'FAILURE(S)'); process.exit(1); }
    console.log(sawBusy
        ? 'PASS: backgrounding mid-load answered BUSY'
        : '  (note: never sampled the engine mid-load — no evidence either way, not a failure)');
    check('nothing was saved while the world was still loading', saveCount() === 0);

    await new Promise((r) => setTimeout(r, 1500)); // let Terrain::update stream/mesh a little

    // A pristine world has no block data of its own to write (FileManager::saveColumn skips any
    // column whose `modified` flag is FALSE), so without a real edit this test would "pass" on an
    // empty region. Same trick, and the same reasoning, as headless-save-roundtrip-test.js.
    const playerState = JSON.parse(utf8(global.Module._eden_debug_player_state()));
    const [px, py, pz] = playerState.pos;
    const editX = Math.round(px), editZ = Math.round(pz), editY = Math.max(0, Math.round(py) - 3);
    check('eden_console_setblock accepted (returns 1)',
        global.Module._eden_console_setblock(editX, editZ, editY, 1) === 1);

    checkStatus('backgrounding in a loaded world saves', background(), BG_SAVED);
    check('exactly one background save has been counted', saveCount() === 1);

    const worldFile = findWorldFile(displayName);
    check('world file exists on disk after the background save', !!worldFile);
    if (!worldFile) { console.log(failures, 'FAILURE(S)'); process.exit(1); }

    const region1 = readBlockDataRegion(global.FS.readFile('/documents/' + worldFile.file));
    check('background save: directory_offset points past a real block-data region', region1.ok);
    check('background save: at least one full column (>= 32 KB) reached the file',
        region1.ok && region1.bytes.length >= 32768);

    // ---------------------------------------------------------------------------------------
    // 3. A second background in a row is safe. Both lifecycle events fire on a real tab close,
    //    so this is not a hypothetical — it is the shipped path.
    // ---------------------------------------------------------------------------------------
    checkStatus('a second consecutive background still saves', background(), BG_SAVED);
    check('the second background save was counted', saveCount() === 2);
    const region2 = readBlockDataRegion(global.FS.readFile('/documents/' + worldFile.file));
    check('back-to-back background saves leave the block data byte-identical',
        region1.ok && region2.ok && region1.bytes.equals(region2.bytes));

    // ---------------------------------------------------------------------------------------
    // 4. Quit to the menu: refuse again, and the count must not move.
    // ---------------------------------------------------------------------------------------
    await ensureMenuOpen();
    await tapHud(6); // rexit
    const sawMenu = await waitUntil(() => menuState().game_mode === 0, 10000, 'game_mode back to MENU');
    check('back in GAME_MODE_MENU after quit', sawMenu);
    checkStatus('backgrounding after quitting to the menu refuses again', background(), BG_NO_WORLD);
    check('quitting to the menu did not count a background save', saveCount() === 2);

    // ---------------------------------------------------------------------------------------
    // 5. The bytes the background save wrote are a real, loadable world — reload it and prove the
    //    block data survives the round trip unchanged. This is the assertion that makes the whole
    //    test about persistence rather than about a return code.
    // ---------------------------------------------------------------------------------------
    const count = global.Module._eden_menu_world_count();
    let reloadIdx = -1;
    for (let i = 0; i < count; i++) {
        if (utf8(global.Module._eden_menu_world_name(i)) === displayName) { reloadIdx = i; break; }
    }
    check('the background-saved world is listed by name in the menu', reloadIdx >= 0);
    if (reloadIdx < 0) { console.log(failures, 'FAILURE(S)'); process.exit(1); }

    global.Module._eden_menu_select(reloadIdx);
    check('play() accepted on reload', global.Module._eden_menu_play() === 1);
    const sawPlay2 = await waitUntil(() => menuState().game_mode === 1, 20000, 'game_mode == PLAY (reload)');
    check('game_mode reached PLAY after reloading the background-saved world', sawPlay2);
    if (!sawPlay2) { console.log(failures, 'FAILURE(S)'); process.exit(1); }

    checkStatus('backgrounding the reloaded world saves', background(), BG_SAVED);
    check('the reload-then-background save was counted', saveCount() === 3);
    const region3 = readBlockDataRegion(global.FS.readFile('/documents/' + worldFile.file));
    check('reloaded world: block-data region is still valid', region3.ok);
    check('block data is BYTE-IDENTICAL across background-save -> reload -> background-save',
        region1.ok && region3.ok && region1.bytes.equals(region3.bytes));

    console.log(failures === 0 ? 'ALL PASS' : `${failures} FAILURE(S)`);
    process.exit(failures === 0 ? 0 : 1);
});
