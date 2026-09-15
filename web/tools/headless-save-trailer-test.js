// Post-directory SIGN TRAILER round-trip test — the regression guard for the data-loss bug traced
// in WORKING/newformat256z-sign-trailer-2026-08-24.md and fixed as part of 256z Stage 3 / B5.
//
// WHAT THE BUG WAS. A 2026-08 game update ("NewFormat256z") appends in-game sign records INSIDE
// the chunk-directory region — after the real ColumnIndex rows, before EOF, every row tagged
// x = 0xffffffff so FileManager::readDirectory's twoToOne() gate maps it to "invalid, skip".
// Reading has always tolerated that. Writing did not: fwriteDirectory() rebuilds the directory
// from the `indexes` hashmap alone, and those rows were never put in it, so any save that
// rewrote the directory (i.e. any save that streamed a new column in — the common case) wrote a
// shorter directory over the top and destroyed every sign in the world.
//
// WHAT THIS TEST DOES. It doesn't need a NewFormat256z world: the trailer is a property of the
// directory region, not of the column layout, so an ordinary 64z world this engine wrote itself
// can carry one. The 192 bytes injected below are the REAL trailer lifted verbatim from the
// `quarry.eden` specimen (TESTERS/quarry-NewFormat256z.zip, 3.97 GB, one sign reading "test") —
// a 12-row `SGN1` wrapper + one 120-byte sign record, all rows 0xffffffff-tagged.
//
// The world is then re-loaded and edited in a column the directory does NOT yet contain, which is
// what forces writeDirectory=TRUE and makes the save rewrite the directory region. Before the fix
// the trailer is simply gone at that point; after it, it comes back byte-for-byte at the end.
//
// Usage: node tools/headless-save-trailer-test.js [path/to/eden.js]
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
const menuState = () => JSON.parse(utf8(global.Module._eden_debug_menu_state()));

function waitUntil(predicate, timeoutMs, label) {
    return new Promise((resolve) => {
        const start = Date.now();
        const poll = () => {
            let ok = false;
            try { ok = predicate(); } catch (e) {}
            if (ok) return resolve(true);
            if (Date.now() - start > timeoutMs) { console.log(`  (timed out: ${label})`); return resolve(false); }
            setTimeout(poll, 50);
        };
        poll();
    });
}
function tapHud(which) {
    return new Promise((resolve) => {
        global.Module._eden_tap_hud_button_begin(which);
        setTimeout(() => {
            global.Module._eden_tap_hud_button_end(which);
            setTimeout(resolve, 150);
        }, 100);
    });
}
async function ensureMenuOpen() { if (global.Module._eden_hud_in_menu() === 0) await tapHud(0); }
async function quitToMenu() {
    await ensureMenuOpen();
    await tapHud(6);
    return waitUntil(() => menuState().game_mode === 0, 10000, 'MENU');
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
    return waitUntil(() => menuState().game_mode === 1, 30000, `PLAY ${displayName}`);
}

let failures = 0;
function check(name, cond, detail) {
    if (cond) console.log('PASS:', name);
    else { console.log('FAIL:', name, detail === undefined ? '' : `(${detail})`); failures++; }
}

// The literal 192 bytes from quarry.eden's directory tail: 12 x 16-byte rows, each prefixed
// ff ff ff ff. Stripped of those tags the payload reads
//   "SGN1" len=132 | "SGN1" ver=1 count=1 | x=0xff84 y=0xfe2d z=0x20 a=4 b=9 c=1 | text "test"
const QUARRY_TRAILER = Buffer.from([
    0xff,0xff,0xff,0xff, 0x53,0x47,0x4e,0x31, 0x84,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
    0xff,0xff,0xff,0xff, 0x53,0x47,0x4e,0x31, 0x01,0x00,0x00,0x00, 0x01,0x00,0x00,0x00,
    0xff,0xff,0xff,0xff, 0x84,0xff,0x00,0x00, 0x2d,0xfe,0x00,0x00, 0x20,0x00,0x00,0x00,
    0xff,0xff,0xff,0xff, 0x04,0x00,0x00,0x00, 0x09,0x00,0x00,0x00, 0x01,0x00,0x00,0x00,
    0xff,0xff,0xff,0xff, 0x74,0x65,0x73,0x74, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
    0xff,0xff,0xff,0xff, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
    0xff,0xff,0xff,0xff, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
    0xff,0xff,0xff,0xff, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
    0xff,0xff,0xff,0xff, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
    0xff,0xff,0xff,0xff, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
    0xff,0xff,0xff,0xff, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
    0xff,0xff,0xff,0xff, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
]);

function directoryOffset(bytes) {
    const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    return dv.getUint32(36, true) * 4294967296 + dv.getUint32(32, true);
}
// Same gate as Classes/Util.mm's twoToOne(): anything outside 0..32767 (and (0,0) itself) is
// "invalid, skip" and is never admitted to the in-memory directory.
function splitDirectory(bytes) {
    const at = directoryOffset(bytes);
    const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    const rows = [];
    for (let o = at; o + 16 <= bytes.length; o += 16) {
        const x = dv.getInt32(o, true), z = dv.getInt32(o + 4, true);
        const valid = x >= 0 && z >= 0 && x < 32768 && z < 32768 && ((x << 15) + z) !== 0;
        rows.push({ o, valid });
    }
    let lastValid = -1;
    rows.forEach((r, i) => { if (r.valid) lastValid = i; });
    return {
        dirAt: at,
        rows: rows.length,
        validRows: rows.filter((r) => r.valid).length,
        trailer: lastValid + 1 < rows.length
            ? Buffer.from(bytes.buffer, bytes.byteOffset + at + (lastValid + 1) * 16, (rows.length - lastValid - 1) * 16)
            : Buffer.alloc(0),
        endsClean: at + rows.length * 16 === bytes.length,
    };
}

global.Module.postRun = global.Module.postRun || [];
global.Module.postRun.push(async () => {
    await waitUntil(() => !menuState().error, 5000, 'World/Menu');

    // ---- 1. an ordinary world with a real save behind it ------------------------------------
    const idx = global.Module._eden_menu_create_world();
    const displayName = utf8(global.Module._eden_menu_world_name(idx));
    global.Module._eden_menu_clear_pending_world_type();
    global.Module._eden_menu_play();
    check('game_mode reached PLAY', await waitUntil(() => menuState().game_mode === 1, 30000, 'PLAY'));
    await new Promise((r) => setTimeout(r, 1500));

    const st = JSON.parse(utf8(global.Module._eden_debug_player_state()));
    const [px, py, pz] = st.pos;
    const bx = Math.round(px), bz = Math.round(pz), by = Math.max(1, Math.round(py) - 3);
    global.Module._eden_console_setblock(bx, bz, by, 13);
    await ensureMenuOpen();
    await tapHud(3); // rsave

    const list = JSON.parse(utf8(global.Module._eden_storage_list_worlds()));
    const ent = list.find((w) => w.name === displayName);
    check('world file exists after the first save', !!ent);
    if (!ent) { console.log(`${failures} FAILURE(S)`); process.exit(1); }
    const memPath = '/documents/' + ent.file;
    check('back in MENU', await quitToMenu());

    // ---- 2. graft the real quarry sign trailer onto its directory ---------------------------
    const before = Buffer.from(global.FS.readFile(memPath));
    const d0 = splitDirectory(before);
    check('the fresh save has no trailer of its own', d0.trailer.length === 0, d0.trailer.length);
    check('the fresh save ends exactly where its directory does', d0.endsClean,
        `${d0.dirAt} + ${d0.rows}*16 vs ${before.length}`);
    const grafted = Buffer.concat([before, QUARRY_TRAILER]);
    global.FS.writeFile(memPath, new Uint8Array(grafted));
    const dG = splitDirectory(grafted);
    check('the grafted file parses as <real rows> + a 192 B trailer',
        dG.validRows === d0.validRows && dG.trailer.equals(QUARRY_TRAILER),
        `${dG.validRows} valid rows, ${dG.trailer.length} B trailer`);

    // ---- 3. reload and edit a column the directory does NOT contain -------------------------
    // That is what sets writeDirectory=TRUE and makes the save rebuild the directory region --
    // without it the trailer would survive by accident (nothing overwrites it) and this test
    // would pass against the buggy build too.
    check('the grafted world still plays', await playWorldNamed(displayName));
    await new Promise((r) => setTimeout(r, 1500));
    const FAR = 80; // > 4 chunks away, so it is a different column record
    global.Module._eden_console_setblock(bx + FAR, bz + FAR, by, 13);
    check('a block placed in a far column reads back',
        global.Module._eden_console_getblock(bx + FAR, bz + FAR, by) === 13);
    await ensureMenuOpen();
    await tapHud(3); // rsave

    // ---- 4. the trailer must have survived, byte for byte -----------------------------------
    const after = Buffer.from(global.FS.readFile(memPath));
    const dA = splitDirectory(after);
    check('the save really did rewrite the directory (more column rows than before)',
        dA.validRows > dG.validRows, `${dG.validRows} -> ${dA.validRows}`);
    check('the sign trailer survived the directory rewrite, byte for byte',
        dA.trailer.equals(QUARRY_TRAILER),
        `${dA.trailer.length} B: ${dA.trailer.slice(0, 16).toString('hex')}`);
    check('the trailer sits immediately after the column rows, at the very end of the file',
        dA.endsClean, `${dA.dirAt} + ${dA.rows}*16 vs ${after.length}`);

    // ---- 5. and again, so a re-captured trailer is not a one-shot ---------------------------
    check('back in MENU (2nd)', await quitToMenu());
    check('the world plays a third time', await playWorldNamed(displayName));
    await new Promise((r) => setTimeout(r, 1500));
    global.Module._eden_console_setblock(bx - FAR, bz - FAR, by, 13);
    await ensureMenuOpen();
    await tapHud(3);
    const after2 = Buffer.from(global.FS.readFile(memPath));
    const dA2 = splitDirectory(after2);
    check('the trailer survives a SECOND directory rewrite (it is re-captured on every read)',
        dA2.trailer.equals(QUARRY_TRAILER) && dA2.validRows > dA.validRows,
        `${dA.validRows} -> ${dA2.validRows}, ${dA2.trailer.length} B trailer`);

    console.log(failures === 0 ? 'ALL PASS' : `${failures} FAILURE(S)`);
    process.exit(failures === 0 ? 0 : 1);
});
