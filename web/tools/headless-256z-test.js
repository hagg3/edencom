// Headless 256z ("New Dawn") read/play/save check — the regression test for Stage 2 of
// WORKING/256z-format-backport-plan-2026-08-05.md (runtime, per-world world height).
//
// WHY IT IS SHAPED LIKE THIS. There is no small checked-in 256z fixture and there deliberately
// isn't one: a 256z column record is 131,072 bytes, so even an 18x18 world is 42 MB. Instead the
// test MAKES one at run time out of a world this engine itself just wrote — create a normal 64z
// world, edit a block in it, save it, then run the real `tools/eden-convert.js --to-256` over the
// saved bytes and hand the result back to the engine. That gets three things a synthetic fixture
// would not: the converter and the engine are checked against each other rather than both against
// my idea of the format; the 64z content is known exactly, so "did the low bands survive the
// conversion" is checkable; and the fixture tracks the save format automatically if it ever moves.
//
// WHAT IT PROVES
//   1. A version-5 header makes the engine open the world at height 256 — 16 bands, a 131,072 B
//      column record — and a version-4 one does not (the same world, one load earlier).
//   2. The creature-slot count is DERIVED FROM THE FILE, not the version (400 here, because that
//      is what the converter wrote).
//   3. Block data below y=64 survives the round trip: the block placed before conversion reads
//      back at the same coordinate after it.
//   4. Space above the old ceiling reads as AIR, not as a neighbouring column's bytes — the
//      failure mode a wrong stride produces, and the one that looks like corrupt terrain.
//   5. A block placed at y=200 survives save -> quit -> reload. That exercises saveColumn at the
//      256z stride and readColumn's band loop at 16, which is the whole point of Stage 2.
//   6. The save preserved the file's own version 5 (B4) instead of stamping FILE_VERSION 4, which
//      is what would silently re-label a tall world as a short one.
//
// Usage: node tools/headless-256z-test.js [path/to/eden.js]   (defaults to ../build-st/eden.js)
'use strict';
const fs = require('fs');
const os = require('os');
const path = require('path');
const vm = require('vm');
const { execFileSync } = require('child_process');

const positional = process.argv.slice(2).filter((a) => !a.startsWith('--'));
const edenJsPath = path.resolve(positional[0] || path.join(__dirname, '..', 'build-st', 'eden.js'));
const edenDir = path.dirname(edenJsPath);
const CONVERT = path.join(__dirname, 'eden-convert.js');

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
const worldFormat = () => JSON.parse(utf8(global.Module._eden_debug_world_format()));

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

let failures = 0;
function check(name, cond, detail) {
    if (cond) console.log('PASS:', name);
    else { console.log('FAIL:', name, detail === undefined ? '' : `(${detail})`); failures++; }
}

function findWorldFile(displayName) {
    const list = JSON.parse(utf8(global.Module._eden_storage_list_worlds()));
    return list.find((w) => w.name === displayName) || null;
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
async function quitToMenu() {
    await ensureMenuOpen();
    await tapHud(6); // rexit
    return waitUntil(() => menuState().game_mode === 0, 10000, 'game_mode back to MENU');
}
async function playWorldNamed(displayName) {
    const count = global.Module._eden_menu_world_count();
    let idx = -1;
    for (let i = 0; i < count; i++) {
        if (utf8(global.Module._eden_menu_world_name(i)) === displayName) { idx = i; break; }
    }
    if (idx < 0) return false;
    global.Module._eden_menu_select(idx);
    if (global.Module._eden_menu_play() !== 1) return false;
    return waitUntil(() => menuState().game_mode === 1, 30000, `game_mode == PLAY (${displayName})`);
}

// header fields we assert on directly (docs/eden-file-format.md)
const headerVersion = (bytes) => new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength).getInt32(92, true);
function directoryOffset(bytes) {
    const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    return dv.getUint32(36, true) * 4294967296 + dv.getUint32(32, true);
}
// Every column record must start on a clean stride from the header — the same invariant
// FileManager::saveColumn asserts with (chunk_offset-192) % SIZEOF_COLUMN == 0.
function columnOffsetsAligned(bytes, colBytes) {
    const dirAt = directoryOffset(bytes);
    const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    let n = 0;
    for (let at = dirAt; at + 16 <= bytes.length; at += 16) {
        const off = dv.getUint32(at + 12, true) * 4294967296 + dv.getUint32(at + 8, true);
        if ((off - 192) % colBytes !== 0) return { ok: false, n, off };
        n++;
    }
    return { ok: n > 0, n };
}

global.Module.postRun = global.Module.postRun || [];
global.Module.postRun.push(async () => {
    const ready = await waitUntil(() => !menuState().error, 5000, 'World/Menu to exist');
    check('World/Menu exist after main()', ready);
    if (!ready) { console.log('FATAL, aborting'); process.exit(1); }

    // ---- 1. a normal 64z world, with one known block edit in it -----------------------------
    const idx = global.Module._eden_menu_create_world();
    const displayName = utf8(global.Module._eden_menu_world_name(idx));
    global.Module._eden_menu_clear_pending_world_type();
    check('play() accepted for the new 64z world', global.Module._eden_menu_play() === 1);
    check('game_mode reached PLAY (64z)', await waitUntil(() => menuState().game_mode === 1, 30000, 'PLAY'));

    const fmt64 = worldFormat();
    check('a freshly created world opens at height 64', fmt64.height === 64, JSON.stringify(fmt64));
    check('...with 4 bands and a 32768 B column record',
        fmt64.bands === 4 && fmt64.column_bytes === 32768, JSON.stringify(fmt64));

    await new Promise((r) => setTimeout(r, 1500)); // let some columns stream/mesh

    const st = JSON.parse(utf8(global.Module._eden_debug_player_state()));
    const [px, py, pz] = st.pos;
    const bx = Math.round(px), bz = Math.round(pz), by = Math.max(1, Math.round(py) - 3);
    const MARK_LOW = 13;   // TYPE_BRICK — distinctive, and nothing generates it near a player
    global.Module._eden_console_setblock(bx, bz, by, MARK_LOW);
    check('marker block placed below the player reads back before saving',
        global.Module._eden_console_getblock(bx, bz, by) === MARK_LOW);

    await ensureMenuOpen();
    await tapHud(3); // rsave
    const wf = findWorldFile(displayName);
    check('world file discoverable after the 64z save', !!wf);
    if (!wf) { console.log(`${failures} FAILURE(S)`); process.exit(1); }
    check('back in MENU after quitting the 64z world', await quitToMenu());

    // ---- 2. convert that very file to 256z with the real offline converter -------------------
    const memPath = '/documents/' + wf.file;
    const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'eden256-'));
    const src64 = path.join(tmp, 'w64.eden');
    const out256 = path.join(tmp, 'w256.eden');
    fs.writeFileSync(src64, Buffer.from(global.FS.readFile(memPath)));
    const conv = execFileSync(process.execPath, [CONVERT, '--to-256', src64, '-o', out256, '--yes'],
        { encoding: 'utf8' });
    check('eden-convert.js --to-256 reported a 131072 B column record', /131,?072/.test(conv), conv.trim());
    const bytes256 = fs.readFileSync(out256);
    check('converted file carries header version 5', headerVersion(bytes256) === 5, headerVersion(bytes256));
    global.FS.writeFile(memPath, new Uint8Array(bytes256));

    // ---- 3. the engine opens it natively at 256 ---------------------------------------------
    check('the converted world plays', await playWorldNamed(displayName));
    const fmt256 = worldFormat();
    check('a version-5 world opens at height 256', fmt256.height === 256, JSON.stringify(fmt256));
    check('...with 16 bands and a 131072 B column record',
        fmt256.bands === 16 && fmt256.column_bytes === 131072, JSON.stringify(fmt256));
    check('...and 400 creature slots DERIVED from the file, not assumed from the version',
        fmt256.creature_slots === 400, JSON.stringify(fmt256));
    check('...with the toroidal window and its stride grown to match',
        fmt256.xz_stride === 288 * 256 && fmt256.t_blocks === 288 * 288 * 256, JSON.stringify(fmt256));

    await new Promise((r) => setTimeout(r, 1500));
    check('the marker block placed at 64z is still there after the conversion',
        global.Module._eden_console_getblock(bx, bz, by) === MARK_LOW,
        global.Module._eden_console_getblock(bx, bz, by));
    // The bands the converter appended are air. If the column stride were wrong, this would be
    // some neighbouring column's terrain instead — the signature failure of a mis-strided read.
    let garbage = 0;
    for (let y = 70; y < 250; y += 7) {
        if (global.Module._eden_console_getblock(bx, bz, y) !== 0) garbage++;
    }
    check('everything above the old 64-block ceiling reads as AIR, not a neighbour\'s bytes',
        garbage === 0, `${garbage} non-air samples`);
    check('y=255 is inside the world (the old build answered -1 for anything above 63)',
        global.Module._eden_console_getblock(bx, bz, 255) === 0,
        global.Module._eden_console_getblock(bx, bz, 255));

    // ---- 4. author at height, save, reload --------------------------------------------------
    const HIGH_Y = 200, MARK_HIGH = 74; // TYPE_STEEL
    global.Module._eden_console_setblock(bx, bz, HIGH_Y, MARK_HIGH);
    check('a block can be placed at y=200 and reads back immediately',
        global.Module._eden_console_getblock(bx, bz, HIGH_Y) === MARK_HIGH);

    await ensureMenuOpen();
    await tapHud(3); // rsave
    const saved = Buffer.from(global.FS.readFile(memPath));
    check('the 256z save kept the file\'s OWN version 5 (B4: it used to stamp 4)',
        headerVersion(saved) === 5, headerVersion(saved));
    const align = columnOffsetsAligned(saved, 131072);
    check('every column in the saved directory sits on a clean 131072 B stride',
        align.ok, JSON.stringify(align));

    check('back in MENU after quitting the 256z world', await quitToMenu());
    check('the 256z world reloads', await playWorldNamed(displayName));
    const fmt256b = worldFormat();
    check('it still opens at height 256 after being saved by this build', fmt256b.height === 256,
        JSON.stringify(fmt256b));
    await new Promise((r) => setTimeout(r, 1500));
    check('the y=200 block survived save -> quit -> reload',
        global.Module._eden_console_getblock(bx, bz, HIGH_Y) === MARK_HIGH,
        global.Module._eden_console_getblock(bx, bz, HIGH_Y));
    check('and the low-band marker is still there too',
        global.Module._eden_console_getblock(bx, bz, by) === MARK_LOW,
        global.Module._eden_console_getblock(bx, bz, by));

    try { fs.rmSync(tmp, { recursive: true, force: true }); } catch (e) { /* best effort */ }
    console.log(failures === 0 ? 'ALL PASS' : `${failures} FAILURE(S)`);
    process.exit(failures === 0 ? 0 : 1);
});
