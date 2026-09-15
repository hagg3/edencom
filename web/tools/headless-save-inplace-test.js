// In-place save + rollback-journal test — the regression guard for 256z Stage 3 / B5
// ("the atomic-save whole-file copy must go"), WORKING/256z-format-backport-plan-2026-08-05.md.
//
// WHAT B5 CHANGED. Below Classes/Constants.h's g_save_inplace_threshold a save still runs on a
// whole-file scratch copy committed by one rename (pass 37's atomicity, unchanged). At or above
// it — where that copy is O(file size) in time AND in peak memory, which a multi-gigabyte 256z
// world cannot pay — the save writes straight into the world file, and a small journal holding
// the header plus the file's tail from (directory_offset - creature block) stands in for the
// copy. Recovery runs from FileManager::probeWorldHeight, i.e. before anything reads the header.
//
// WHAT C3 ADDED (2026-09-02, section 4b). B5's journal covered only the region an APPEND can
// destroy, so a crash could still leave one dirty column half-old/half-new — the file loaded, the
// directory was valid, but a chunk of terrain was garbage. The journal now also carries a
// pre-image record per column the save overwrites at its existing offset. Section 4b proves that
// both ways: with the records the torn column is repaired byte-for-byte, and with
// eden_debug_set_save_journal_columns(0) (B5's exact behaviour) the identical damage survives.
//
// WHY IT LOOKS LIKE THIS. Picking the path by file size would make the in-place path untestable
// without a several-hundred-megabyte fixture, so eden_debug_set_save_inplace_threshold() forces
// it (0 = always in place). And a crash cannot be staged from a headless harness — a JS throw
// under a synchronous engine write kills the whole engine (web/CLAUDE.md) — so the interrupted
// state is reconstructed instead, from a REAL journal the engine itself wrote: FS.unlink is
// wrapped to snapshot the journal at the instant the save commits by deleting it. The bytes fed
// back to the recovery path are therefore the engine's own, not a hand-built fixture.
//
// Usage: node tools/headless-save-inplace-test.js [path/to/eden.js]
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
const exists = (p) => { try { global.FS.stat(p); return true; } catch (e) { return false; } };
const read = (p) => Buffer.from(global.FS.readFile(p));

// Snapshot every journal the engine deletes — that delete IS the save's commit point, so the
// bytes captured here are exactly what a crash one instant earlier would have left behind.
let lastJournal = null;
(function wrapUnlink() {
    const orig = global.FS.unlink.bind(global.FS);
    global.FS.unlink = function (p) {
        if (typeof p === 'string' && p.endsWith('.savejrnl') && exists(p)) lastJournal = read(p);
        return orig(p);
    };
})();

// Journal layout — FileManager.mm's SaveJournalHeader. Kept in sync by hand; the magic/version
// asserts below fail loudly if it ever drifts.
const JOURNAL_HEADER_BYTES = 40 + 192;   // magic/version/reserved/3 u64s, then WorldFileHeader
const JOURNAL_COLUMN_BYTES = 24;         // SaveJournalColumn: magic[8] + offset u64 + length u64
function parseJournal(buf) {
    if (!buf || buf.length < 40) return null;
    return {
        magic: buf.slice(0, 7).toString('ascii'),
        version: buf.readUInt32LE(8),
        origLength: Number(buf.readBigUInt64LE(16)),
        regionOffset: Number(buf.readBigUInt64LE(24)),
        regionLength: Number(buf.readBigUInt64LE(32)),
        worldHeader: buf.slice(40, 40 + 192),
        totalLength: buf.length,
    };
}
// C3's phase-2 records, which follow the region: one per column the save overwrote in place,
// holding that column's original bytes. Parsed with the same stop-at-the-first-bad-record rule
// FileManager::recoverInterruptedSave uses, so a drift in either shows up as a count mismatch.
function parseJournalColumns(buf, j) {
    const recs = [];
    let p = JOURNAL_HEADER_BYTES + j.regionLength;
    while (p + JOURNAL_COLUMN_BYTES <= buf.length) {
        if (buf.slice(p, p + 7).toString('ascii') !== 'EDNJCOL') break;
        const offset = Number(buf.readBigUInt64LE(p + 8));
        const length = Number(buf.readBigUInt64LE(p + 16));
        if (length <= 0 || p + JOURNAL_COLUMN_BYTES + length > buf.length) break;
        recs.push({ offset, length, payloadAt: p + JOURNAL_COLUMN_BYTES });
        p += JOURNAL_COLUMN_BYTES + length;
    }
    return recs;
}
function directoryOffset(bytes) {
    const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    return dv.getUint32(36, true) * 4294967296 + dv.getUint32(32, true);
}

global.Module.postRun = global.Module.postRun || [];
global.Module.postRun.push(async () => {
    await waitUntil(() => !menuState().error, 5000, 'World/Menu');
    check('the save-strategy threshold defaults to 16 MiB',
        global.Module._eden_debug_get_save_inplace_threshold() === 16 * 1024 * 1024,
        global.Module._eden_debug_get_save_inplace_threshold());

    // ---- 1. a world saved the ordinary (below-threshold) way --------------------------------
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
    await tapHud(3);

    const ent = JSON.parse(utf8(global.Module._eden_storage_list_worlds())).find((w) => w.name === displayName);
    check('world file exists after the first save', !!ent);
    if (!ent) { console.log(`${failures} FAILURE(S)`); process.exit(1); }
    const memPath = '/documents/' + ent.file;
    check('the below-threshold save used the scratch copy (a .savetmp.bak slot exists)',
        exists(memPath + '.savetmp.bak'));
    check('...and left no journal behind', !exists(memPath + '.savejrnl'));

    // ---- 2. same world, forced onto the in-place path ---------------------------------------
    global.Module._eden_debug_set_save_inplace_threshold(0);
    check('threshold now forces the in-place path',
        global.Module._eden_debug_get_save_inplace_threshold() === 0);

    const FAR = 80; // a column the directory does not contain yet -> forces a directory rewrite
    global.Module._eden_console_setblock(bx + FAR, bz + FAR, by, 13);
    const beforeFirstInPlace = read(memPath);
    lastJournal = null;
    await ensureMenuOpen();
    await tapHud(3);
    const good = read(memPath);
    check('the in-place save grew the file (it appended a new column record)',
        good.length > beforeFirstInPlace.length, `${beforeFirstInPlace.length} -> ${good.length}`);

    check('the in-place save left NO scratch copy behind', !exists(memPath + '.savetmp'));
    // Not just "didn't make one": it reclaims the stale one the last below-threshold save left,
    // which is a full second copy of the world that nothing above the threshold ever refreshes.
    check('the in-place save reclaimed the stale whole-file backup slot', !exists(memPath + '.savetmp.bak'));
    check('...and any older <world>.bak slot with it', !exists(memPath + '.bak'));
    check('the in-place save removed its journal on commit', !exists(memPath + '.savejrnl'));
    check('a journal was actually written and then committed', !!lastJournal);
    if (!lastJournal) { console.log(`${failures} FAILURE(S)`); process.exit(1); }

    const j = parseJournal(lastJournal);
    check('journal magic/version are what FileManager.mm writes',
        j.magic === 'EDNJRNL' && j.version === 2, `${j.magic} v${j.version}`);
    check('journal covers the file tail exactly (region_offset + region_length == orig_length)',
        j.regionOffset + j.regionLength === j.origLength, JSON.stringify(j).slice(0, 200));
    check('journal is O(directory), not O(file): its payload is a small fraction of the world',
        j.totalLength < good.length, `${j.totalLength} B journal vs ${good.length} B world`);
    check('the journalled header is the PREVIOUS save\'s (an older directory_offset)',
        directoryOffset(j.worldHeader) < directoryOffset(good),
        `${directoryOffset(j.worldHeader)} vs ${directoryOffset(good)}`);

    // ---- 3. the in-place save is still a correct save ---------------------------------------
    check('back in MENU', await quitToMenu());
    check('the in-place-saved world reloads', await playWorldNamed(displayName));
    await new Promise((r) => setTimeout(r, 1500));
    check('the block edited before the in-place save is still there',
        global.Module._eden_console_getblock(bx, bz, by) === 13,
        global.Module._eden_console_getblock(bx, bz, by));
    check('so is the one in the far column the in-place save had to append',
        global.Module._eden_console_getblock(bx + FAR, bz + FAR, by) === 13,
        global.Module._eden_console_getblock(bx + FAR, bz + FAR, by));
    check('back in MENU (2nd)', await quitToMenu());

    // ---- 4. roll back an interrupted in-place save -------------------------------------------
    // Staged against a save made HERE rather than against step 2's, because quitting to the menu
    // is itself a save: `lastJournal` and the file on disk have both moved on since then. The
    // damage is staged with NO world loaded, which is the only state real recovery ever runs in
    // (FileManager::probeWorldHeight, before the load) — recovering under a live world would
    // leave the engine's in-memory directory pointing past the rolled-back file's end.
    check('the world plays again for the rollback leg', await playWorldNamed(displayName));
    await new Promise((r) => setTimeout(r, 1500));
    const FAR2 = -80;
    global.Module._eden_console_setblock(bx + FAR2, bz + FAR2, by, 13);
    // The exact bytes the exit-save is about to start overwriting. This — not the file that save
    // produces — is what rolling it back has to reproduce.
    const beforeSave = read(memPath);
    lastJournal = null;
    check('back in MENU (3rd)', await quitToMenu());
    const afterSave = read(memPath);
    const j4 = parseJournal(lastJournal);
    check('quitting performed an in-place save with its own journal', !!j4);
    if (!j4) { console.log(`${failures} FAILURE(S)`); process.exit(1); }
    check('the exit save grew the file (it appended a new column record)',
        afterSave.length > beforeSave.length, `${beforeSave.length} -> ${afterSave.length}`);
    check('its journal records the pre-save length', j4.origLength === beforeSave.length,
        `${j4.origLength} vs ${beforeSave.length}`);

    // What a crash between the journal and the commit leaves on disk: the destructive part of the
    // save (everything at or above region_offset) half-written, the journal still present.
    const damaged = Buffer.from(afterSave);
    damaged.fill(0xcd, j4.regionOffset);                 // scribble over the structural tail...
    const truncated = damaged.slice(0, j4.regionOffset + Math.floor(j4.regionLength / 2)); // ...stop mid-write
    global.FS.writeFile(memPath, new Uint8Array(truncated));
    global.FS.writeFile(memPath + '.savejrnl', new Uint8Array(lastJournal));
    check('the staged file really is damaged', !read(memPath).equals(afterSave));

    // eden_storage_list_worlds -> FileManager::probeWorldHeight -> recoverInterruptedSave
    JSON.parse(utf8(global.Module._eden_storage_list_worlds()));
    const restored = read(memPath);
    check('an interrupted in-place save is rolled back to the last COMPLETE save, byte for byte',
        restored.equals(beforeSave), `${restored.length} B vs ${beforeSave.length} B expected`);
    check('recovery consumed the journal', !exists(memPath + '.savejrnl'));
    check('the rolled-back world still plays', await playWorldNamed(displayName));
    await new Promise((r) => setTimeout(r, 1500));
    check('it still has the block from the earlier, completed save',
        global.Module._eden_console_getblock(bx, bz, by) === 13,
        global.Module._eden_console_getblock(bx, bz, by));
    check('and not the one the rolled-back save had appended',
        global.Module._eden_console_getblock(bx + FAR2, bz + FAR2, by) !== 13,
        global.Module._eden_console_getblock(bx + FAR2, bz + FAR2, by));
    check('back in MENU (4th)', await quitToMenu());

    // ---- 4b. C3: a TORN DIRTY COLUMN is rolled back too --------------------------------------
    // This is the gap B5 shipped with and C3 closes. Section 4 only proves the structural tail
    // (creature block + directory + append) survives a crash; a column overwritten at its own
    // existing offset used to be left half-old/half-new with nothing able to repair it. The
    // journal now carries a pre-image record per such column, and the two legs below are the
    // whole claim: with the records the damage is repaired byte-for-byte, and with them turned
    // off (eden_debug_set_save_journal_columns(0), i.e. B5's exact behaviour) the identical
    // damage survives recovery. Without the second leg the first proves nothing — the tail
    // restore alone could be doing the work.
    check('dirty-column journalling is ON by default',
        global.Module._eden_debug_get_save_journal_columns() === 1);

    check('the world plays again for the torn-column leg', await playWorldNamed(displayName));
    await new Promise((r) => setTimeout(r, 1500));
    // (bx,bz) is the spawn column — already in the directory since step 1's save — so editing it
    // makes the next save OVERWRITE it in place rather than append it.
    global.Module._eden_console_setblock(bx, bz, by + 1, 13);
    const beforeTorn = read(memPath);
    lastJournal = null;
    check('back in MENU (5th)', await quitToMenu());
    const jt = parseJournal(lastJournal);
    const recs = parseJournalColumns(lastJournal, jt);
    check('the save journalled at least one dirty column pre-image', recs.length > 0,
        `${recs.length} records`);
    if (!recs.length) { console.log(`${failures} FAILURE(S)`); process.exit(1); }
    check('every column record sits strictly below the phase-1 region (no double coverage)',
        recs.every((r) => r.offset + r.length <= jt.regionOffset),
        JSON.stringify(recs.map((r) => [r.offset, r.length])).slice(0, 200));
    check('each record holds the column\'s bytes as they were BEFORE the save',
        recs.every((r) => lastJournal.slice(r.payloadAt, r.payloadAt + r.length)
            .equals(beforeTorn.slice(r.offset, r.offset + r.length))));

    // What a crash mid-column-write leaves: the last complete save, with one column half
    // overwritten. Nothing structural is damaged — this file loads fine, it just has garbage
    // terrain in it, which is exactly why B5's journal could not detect or repair it.
    function stageTornColumn(base, rec) {
        const torn = Buffer.from(base);
        torn.fill(0xcd, rec.offset, rec.offset + Math.floor(rec.length / 2));
        global.FS.writeFile(memPath, new Uint8Array(torn));
        global.FS.writeFile(memPath + '.savejrnl', new Uint8Array(lastJournal));
    }
    stageTornColumn(beforeTorn, recs[0]);
    check('the staged torn column really is damaged', !read(memPath).equals(beforeTorn));
    JSON.parse(utf8(global.Module._eden_storage_list_worlds()));   // -> recoverInterruptedSave
    check('an interrupted in-place save no longer tears a dirty column — it is rolled back byte for byte',
        read(memPath).equals(beforeTorn));
    check('recovery consumed the journal (torn-column leg)', !exists(memPath + '.savejrnl'));

    // The control: same damage, same recovery, journal written B5-style with no column records.
    global.Module._eden_debug_set_save_journal_columns(0);
    check('dirty-column journalling can be turned off',
        global.Module._eden_debug_get_save_journal_columns() === 0);
    check('the world plays again for the control leg', await playWorldNamed(displayName));
    await new Promise((r) => setTimeout(r, 1500));
    global.Module._eden_console_setblock(bx, bz, by + 2, 13);
    const beforeControl = read(memPath);
    lastJournal = null;
    check('back in MENU (6th)', await quitToMenu());
    const jc = parseJournal(lastJournal);
    const recsOff = parseJournalColumns(lastJournal, jc);
    check('with the lever off the journal carries no column records (B5\'s exact behaviour)',
        recsOff.length === 0, `${recsOff.length} records`);
    stageTornColumn(beforeControl, recs[0]);
    JSON.parse(utf8(global.Module._eden_storage_list_worlds()));
    check('...and the identical damage SURVIVES recovery, which is the gap C3 closes',
        !read(memPath).equals(beforeControl));
    global.Module._eden_debug_set_save_journal_columns(1);
    global.FS.writeFile(memPath, new Uint8Array(beforeControl));   // undo the control's damage
    try { global.FS.unlink(memPath + '.savejrnl'); } catch (e) {}

    // ---- 5. a truncated journal must be discarded, not applied -------------------------------
    const untouched = read(memPath);
    global.FS.writeFile(memPath + '.savejrnl', new Uint8Array(lastJournal.slice(0, 30)));
    JSON.parse(utf8(global.Module._eden_storage_list_worlds()));
    check('a journal too short to be complete is discarded and the world file left alone',
        read(memPath).equals(untouched) && !exists(memPath + '.savejrnl'));

    // ---- 6. deleting a world takes its journal with it ---------------------------------------
    global.FS.writeFile(memPath + '.savejrnl', new Uint8Array(lastJournal));
    const worlds = JSON.parse(utf8(global.Module._eden_storage_list_worlds()));
    // (the listing above already ran recovery, so re-stage the journal after it)
    global.FS.writeFile(memPath + '.savejrnl', new Uint8Array(lastJournal));
    const at = worlds.findIndex((w) => w.file === ent.file);
    check('the world is still listed', at >= 0);
    global.Module._eden_storage_delete_world_at(at);
    check('deleting a world deletes its rollback journal too (a stale one would "recover" the next world of the same name)',
        !exists(memPath + '.savejrnl'));

    console.log(failures === 0 ? 'ALL PASS' : `${failures} FAILURE(S)`);
    process.exit(failures === 0 ? 0 : 1);
});
