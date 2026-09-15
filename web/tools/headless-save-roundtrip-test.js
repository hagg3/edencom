// Headless save/load round-trip check (audit row I6): the highest-value coverage gap the audit's
// §9 named — nothing in the existing four suites (boot, menu flow, lazy world, gamepad) ever
// exercises FileManager's actual column encode/decode path end to end. This does, by proving an
// idempotence property rather than needing a golden fixture: save -> read the raw column BLOCK
// DATA bytes -> quit to menu -> reload the SAME world from that file -> save again -> the column
// bytes must be byte-identical, because nothing edited any block in between. Any regression in the
// RLE-less raw column write/read path (docs/eden-file-format.md's "block data" section — CC order,
// SIZEOF_COLUMN=32768, the (chunk_offset-192) % SIZEOF_COLUMN==0 invariant) flips this from a
// no-op to a diff.
//
// Deliberately does NOT diff the whole file: the 192-byte header carries live player
// position/yaw and the 200-slot creature block (docs/eden-file-format.md), both of which keep
// changing every frame the World is ticking (gravity settling, creature AI) even with no player
// input at all — comparing those would make this test flaky for reasons that have nothing to do
// with the save format. Only the append-only BLOCK DATA region (offset 192 up to
// directory_offset - 200*sizeof(EntityData), the creature block's own fixed size) is a fair
// target: it changes only when a block is actually placed/mined, which this test never does.
//
// Usage: node tools/headless-save-roundtrip-test.js [path/to/eden.js]  (defaults to ../build-st/eden.js)
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

function findWorldFile(displayName) {
    const list = JSON.parse(utf8(global.Module._eden_storage_list_worlds()));
    return list.find((w) => w.name === displayName) || null;
}

// -12000 = -(200 * sizeof(EntityData)), sizeof(EntityData) == 60 bytes per docs/eden-file-format.md.
const CREATURE_BLOCK_BYTES = 200 * 60;
const HEADER_BYTES = 192;

function readBlockDataRegion(fileBytes) {
    const dv = new DataView(fileBytes.buffer, fileBytes.byteOffset, fileBytes.byteLength);
    // directory_offset: unsigned long long at header offset 32, little-endian (doc-confirmed).
    const lo = dv.getUint32(32, true);
    const hi = dv.getUint32(36, true);
    const directoryOffset = hi * 4294967296 + lo;
    const blockDataEnd = directoryOffset - CREATURE_BLOCK_BYTES;
    if (!(blockDataEnd > HEADER_BYTES) || !(blockDataEnd <= fileBytes.length)) {
        return { ok: false, directoryOffset, blockDataEnd };
    }
    return {
        ok: true, directoryOffset, blockDataEnd,
        bytes: fileBytes.subarray(HEADER_BYTES, blockDataEnd),
    };
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

// which=3 (rsave)'s real handler (Hud::handlePickMenu, Classes/Hud.mm) closes hud->inmenu itself
// after acting — a genuine tap(0) toggle would then RE-open it instead of closing it, unlike
// headless-menu-flow-test.js's simpler open-once-then-exit flow which never saves in between. Poll
// the real flag rather than assuming a fixed number of toggles.
async function ensureMenuOpen() {
    if (global.Module._eden_hud_in_menu() === 0) await tapHud(0);
}

global.Module.postRun = global.Module.postRun || [];
global.Module.postRun.push(async () => {
    const haveWorldAndMenu = await waitUntil(() => !menuState().error, 5000, 'World/Menu to exist');
    check('World/Menu exist after main()', haveWorldAndMenu);
    if (!haveWorldAndMenu) { console.log('FATAL, aborting'); process.exit(1); }

    const idx = global.Module._eden_menu_create_world();
    check('create_world returned a valid index', idx >= 0);
    const displayName = utf8(global.Module._eden_menu_world_name(idx));
    check('created world has a non-empty display name', displayName.length > 0);

    // Normal generator (not flat) — real terrain variety exercises more of the column encode path
    // than an all-one-type flat world would (same choice headless-lazy-world-test.js makes).
    global.Module._eden_menu_clear_pending_world_type();
    const playOk = global.Module._eden_menu_play();
    check('play() accepted (returns 1)', playOk === 1);

    const sawPlay1 = await waitUntil(() => menuState().game_mode === 1, 20000, 'game_mode == PLAY (1st load)');
    check('game_mode reached PLAY after create+play', sawPlay1);
    if (!sawPlay1) { console.log(failures, 'FAILURE(S)'); process.exit(1); }

    // Give Terrain::update a few real ticks to stream/mesh some columns before the edit below.
    await new Promise((r) => setTimeout(r, 1500));

    // A pristine, never-edited world has NO block data of its own to round-trip — unmodified
    // terrain streams from the bundled Eden.eden by seed (docs/eden-file-format.md), and
    // FileManager::saveColumn skips any column whose chunk->modified flag is still FALSE. So
    // there must be at least one real edit before the first save, or directory_offset never
    // advances past the header and this test would trivially "pass" on an empty region.
    // eden_console_setblock (DevConsole_web.mm, EDEN_DIAGNOSTICS-gated, audit row I6) reaches the
    // same Terrain::updateChunks dirty-marking path a real build/mine click does, without needing
    // a camera raycast.
    const playerState = JSON.parse(utf8(global.Module._eden_debug_player_state()));
    const [px, py, pz] = playerState.pos; // eden_debug_player_state's pos is [x,y,z], Vector.y up
    const editX = Math.round(px), editZ = Math.round(pz),
          editY = Math.max(0, Math.round(py) - 3); // a few blocks under the player's feet
    const setOk = global.Module._eden_console_setblock(editX, editZ, editY, 1 /* TYPE_DIRT-ish, any non-zero id */);
    check('eden_console_setblock accepted (returns 1)', setOk === 1);

    await ensureMenuOpen();
    await tapHud(3); // rsave

    const worldFile1 = findWorldFile(displayName);
    check('world file discoverable via eden_storage_list_worlds after first save', !!worldFile1);
    if (!worldFile1) { console.log(failures, 'FAILURE(S)'); process.exit(1); }

    const bytes1 = global.FS.readFile('/documents/' + worldFile1.file);
    const region1 = readBlockDataRegion(bytes1);
    check('first save: directory_offset points past a real block-data region', region1.ok);
    check('first save: block-data region is non-trivial (> 32 KB, i.e. at least one column)',
        region1.ok && region1.bytes.length >= 32768);

    // Quit to menu (same sequence as headless-menu-flow-test.js), then reload the SAME world file.
    await ensureMenuOpen();
    await tapHud(6); // rexit

    const sawMenu = await waitUntil(() => menuState().game_mode === 0, 10000, 'game_mode back to MENU');
    check('back in GAME_MODE_MENU after quit', sawMenu);

    const count = global.Module._eden_menu_world_count();
    let reloadIdx = -1;
    for (let i = 0; i < count; i++) {
        if (utf8(global.Module._eden_menu_world_name(i)) === displayName) { reloadIdx = i; break; }
    }
    check('created world is listed by name after quitting to menu', reloadIdx >= 0);
    if (reloadIdx < 0) { console.log(failures, 'FAILURE(S)'); process.exit(1); }

    global.Module._eden_menu_select(reloadIdx);
    const playOk2 = global.Module._eden_menu_play();
    check('play() accepted on reload', playOk2 === 1);

    const sawPlay2 = await waitUntil(() => menuState().game_mode === 1, 20000, 'game_mode == PLAY (2nd load)');
    check('game_mode reached PLAY after reload', sawPlay2);
    if (!sawPlay2) { console.log(failures, 'FAILURE(S)'); process.exit(1); }

    // No further block edits happen anywhere in this script — the only way column bytes can
    // differ now is a genuine decode/re-encode regression.
    await ensureMenuOpen();
    await tapHud(3); // rsave

    const worldFile2 = findWorldFile(displayName);
    check('world file still discoverable after second save', !!worldFile2);
    const bytes2 = global.FS.readFile('/documents/' + (worldFile2 ? worldFile2.file : worldFile1.file));
    const region2 = readBlockDataRegion(bytes2);
    check('second save: directory_offset points past a real block-data region', region2.ok);

    if (region1.ok && region2.ok) {
        check('block-data region is the same length across the reload/re-save round trip',
            region1.bytes.length === region2.bytes.length);
        const same = region1.bytes.length === region2.bytes.length &&
            Buffer.compare(Buffer.from(region1.bytes), Buffer.from(region2.bytes)) === 0;
        check('block-data bytes are BYTE-IDENTICAL after a no-edit save -> reload -> save round trip', same);
        if (!same && region1.bytes.length === region2.bytes.length) {
            let firstDiff = -1;
            for (let i = 0; i < region1.bytes.length; i++) {
                if (region1.bytes[i] !== region2.bytes[i]) { firstDiff = i; break; }
            }
            console.log('  first differing byte at column-data offset', firstDiff);
        }
    }

    console.log(failures === 0 ? 'ALL PASS' : `${failures} FAILURE(S)`);
    process.exit(failures === 0 ? 0 : 1);
});
