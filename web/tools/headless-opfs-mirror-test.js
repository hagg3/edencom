// headless-opfs-mirror-test.js — regression cover for the OPFS persistence backend
// (ROADMAP Phase C / C2, public/eden-opfs.js). Plan: ../../WORKING/opfs-backend-plan.md.
//
// THE PROBLEM THIS SOLVES. Real OPFS needs a browser: `navigator.storage.getDirectory()` does not
// exist under node, and `FileSystemSyncAccessHandle` exists in Workers only. So the backend was
// built with an INJECTABLE byte sink, and this test supplies a node-`fs` one —
// `fs.writeSync(fd, buf, 0, len, at)` is the same random-access partial write as the worker's
// `write(buf, {at})`. Everything above the sink (the op log: ordering, coalescing, truncate,
// rename-over-the-world-file, unlink, the per-batch size op, and the DELTA property C2 exists for)
// is therefore exercised for real, against the real engine's real save path. What it cannot cover
// is the worker/sync-handle half — that is tools/safari-opfs-live.js's job.
//
// What it asserts:
//   1. After every flush the mirror file is BYTE-IDENTICAL to FS.readFile() — on both save
//      strategies (below-threshold scratch-copy+rename, and B5's above-threshold in-place+journal
//      via _eden_debug_set_save_inplace_threshold(0)), across a world delete, and after an import.
//   2. An in-place save writes ORDERS OF MAGNITUDE less than the world file's size. This is the
//      whole row: IDBFS re-`put`s the entire file per autosave (279 MB on the Diane specimen),
//      this writes only the dirty columns + header + directory.
//   3. A second process, booting with nothing but the mirror directory, lists and loads that world
//      and reads back the block this test placed — i.e. the mirror is a real, loadable .eden and
//      the populate path works. (Phase 2, re-invoked automatically; --phase2 runs it by hand.)
//
// Usage: node tools/headless-opfs-mirror-test.js [path/to/eden.js]   (defaults to ../build-st/eden.js)
'use strict';
const fs = require('fs');
const os = require('os');
const path = require('path');
const vm = require('vm');
const { spawnSync } = require('child_process');

const args = process.argv.slice(2);
const positional = args.filter((a) => !a.startsWith('--'));
const edenJsPath = path.resolve(positional[0] || path.join(__dirname, '..', 'build-st', 'eden.js'));
const edenDir = path.dirname(edenJsPath);
const phase2 = args.includes('--phase2');
const dirArg = (args.find((a) => a.startsWith('--dir=')) || '').slice(6);
const MIRROR_DIR = dirArg || fs.mkdtempSync(path.join(os.tmpdir(), 'eden-opfs-'));
const EXPECT_ARG = (args.find((a) => a.startsWith('--expect=')) || '').slice(9);

let failures = 0;
function check(name, cond, extra) {
    if (cond) console.log('PASS:', name);
    else { console.log('FAIL:', name, extra !== undefined ? '— ' + extra : ''); failures++; }
}

// ---------------------------------------------------------------------------------------------
// The node-fs sink: the same three primitives the worker uses (partial write, truncate, rename).
// ---------------------------------------------------------------------------------------------
function makeNodeSink(dir) {
    fs.mkdirSync(dir, { recursive: true });
    const stats = { bytes: 0, ops: 0, batches: 0 };
    function full(p) { return path.join(dir, p); }
    function touch(p) { if (!fs.existsSync(p)) fs.closeSync(fs.openSync(p, 'w')); }
    return {
        stats,
        apply(ops) {
            stats.batches++;
            for (const o of ops) {
                const p = full(o.path);
                stats.ops++;
                switch (o.op) {
                    case 'mkdir': fs.mkdirSync(p, { recursive: true }); break;
                    case 'create': touch(p); break;
                    case 'write': {
                        touch(p);
                        const fd = fs.openSync(p, 'r+');
                        try { fs.writeSync(fd, o.data, 0, o.data.length, o.at); } finally { fs.closeSync(fd); }
                        stats.bytes += o.data.length;
                        break;
                    }
                    case 'truncate': touch(p); fs.truncateSync(p, o.size); break;
                    case 'unlink': fs.rmSync(p, { recursive: true, force: true }); break;
                    case 'rename': fs.renameSync(p, full(o.to)); break;
                    default: throw new Error('unknown op ' + o.op);
                }
            }
            return Promise.resolve({ bytes: stats.bytes, ops: ops.length });
        },
        readAll() {
            const out = [];
            for (const name of fs.readdirSync(dir)) {
                const st = fs.statSync(path.join(dir, name));
                if (!st.isFile()) continue;
                out.push({ name, bytes: new Uint8Array(fs.readFileSync(path.join(dir, name))) });
            }
            return Promise.resolve(out);
        },
        close() { return Promise.resolve(); }
    };
}

// ---------------------------------------------------------------------------------------------
// Boot
// ---------------------------------------------------------------------------------------------
const opfsSrc = fs.readFileSync(path.join(__dirname, '..', 'public', 'eden-opfs.js'), 'utf8');
vm.runInThisContext(opfsSrc, { filename: 'eden-opfs.js' });

const sink = makeNodeSink(MIRROR_DIR);
let fsTypeObj = null;

global.require = require;
global.__dirname = edenDir;
global.__filename = edenJsPath;
global.Module = {
    print: () => {},
    printErr: (t) => { if (/error|abort|Error/.test(t)) console.log('[err]', t); },
    preRun: [() => {
        try { global.FS.mkdir('/documents'); } catch (e) {}
        fsTypeObj = global.EdenOPFS.fsType(sink, {});
        global.FS.mount(fsTypeObj, {}, '/documents');
        global.Module.addRunDependency('eden-opfs-populate');
        global.FS.syncfs(true, (err) => {
            if (err) console.log('[opfs] populate failed:', err);
            global.Module.removeRunDependency('eden-opfs-populate');
        });
    }],
};

const cwdBefore = process.cwd();
process.chdir(edenDir);
vm.runInThisContext(fs.readFileSync(edenJsPath, 'utf8'), { filename: edenJsPath });
process.chdir(cwdBefore);

// ---------------------------------------------------------------------------------------------
// Engine driving helpers (same shapes as tools/headless-save-roundtrip-test.js)
// ---------------------------------------------------------------------------------------------
function utf8(ptr) {
    if (!ptr) return '';
    const heap = global.Module.HEAPU8;
    let end = ptr;
    while (heap[end] !== 0) end++;
    return Buffer.from(heap.buffer, heap.byteOffset + ptr, end - ptr).toString('utf8');
}
function menuState() { return JSON.parse(utf8(global.Module._eden_debug_menu_state())); }
function listWorlds() { return JSON.parse(utf8(global.Module._eden_storage_list_worlds())); }
function waitUntil(predicate, timeoutMs, label) {
    return new Promise((resolve) => {
        const start = Date.now();
        const poll = () => {
            let ok = false;
            try { ok = predicate(); } catch (e) {}
            if (ok) return resolve(true);
            if (Date.now() - start > timeoutMs) { console.log('  (timed out waiting for: ' + label + ')'); return resolve(false); }
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
            setTimeout(resolve, 100);
        }, 100);
    });
}
async function ensureMenuOpen() {
    if (global.Module._eden_hud_in_menu() === 0) await tapHud(0);
}
function flush() {
    return new Promise((resolve, reject) => {
        global.FS.syncfs(false, (err) => (err ? reject(err) : resolve()));
    });
}
// FS.syncfs(false) walks every mount, so this is the same call — the argument only documents
// which mount the caller cares about.
function flushPath() { return flush(); }

// The property every phase checks: MEMFS (what the engine believes) === the mirror (what would
// survive a reload), for every file under the mount, with no extra files on either side.
function compareMirror(label) {
    const memNames = global.FS.readdir('/documents').filter((n) => n !== '.' && n !== '..')
        .filter((n) => { try { return global.FS.isFile(global.FS.stat('/documents/' + n).mode); } catch (e) { return false; } })
        .sort();
    const diskNames = fs.readdirSync(MIRROR_DIR)
        .filter((n) => fs.statSync(path.join(MIRROR_DIR, n)).isFile()).sort();
    check(label + ': same set of files in MEMFS and the mirror',
        JSON.stringify(memNames) === JSON.stringify(diskNames),
        'memfs=' + JSON.stringify(memNames) + ' mirror=' + JSON.stringify(diskNames));
    let allSame = true, detail = '';
    for (const n of memNames) {
        if (diskNames.indexOf(n) < 0) { allSame = false; detail = n + ' missing from mirror'; continue; }
        const a = Buffer.from(global.FS.readFile('/documents/' + n));
        const b = fs.readFileSync(path.join(MIRROR_DIR, n));
        if (a.length !== b.length) { allSame = false; detail = n + ': ' + a.length + ' B in MEMFS vs ' + b.length + ' B mirrored'; continue; }
        if (Buffer.compare(a, b) !== 0) {
            allSame = false;
            let i = 0; while (i < a.length && a[i] === b[i]) i++;
            detail = n + ': first differing byte at ' + i;
        }
    }
    check(label + ': every mirrored file is byte-identical to MEMFS', allSame, detail);
}

function bytesWritten() { return sink.stats.bytes; }

// ---------------------------------------------------------------------------------------------
global.Module.postRun = global.Module.postRun || [];
global.Module.postRun.push(async () => {
    const ok = await waitUntil(() => !menuState().error, 5000, 'World/Menu to exist');
    if (!ok) { console.log('FATAL: engine never reached a usable menu'); process.exit(1); }

    if (phase2) return runPhase2();

    console.log('mirror directory:', MIRROR_DIR);

    // ---- 0. the mount/unmount/remount dance the IndexedDB -> OPFS migration performs ----------
    // eden-storage.js's first-OPFS-boot path mounts IDBFS at /documents, reads, unmounts, then
    // mounts the OPFS type at the same path. None of that is reachable from node (no indexedDB),
    // but the FS mechanics it depends on are, and they are the part that would fail silently.
    try {
        const scratch = makeNodeSink(path.join(MIRROR_DIR, '..', path.basename(MIRROR_DIR) + '-mig'));
        global.FS.mkdir('/migtest');
        global.FS.mount(global.MEMFS, {}, '/migtest');
        global.FS.writeFile('/migtest/a.bin', new Uint8Array([1, 2, 3]));
        check('migration dance: a plain MEMFS mount reads back what it wrote',
            global.FS.readFile('/migtest/a.bin').length === 3);
        global.FS.unmount('/migtest');
        global.FS.mount(global.EdenOPFS.fsType(scratch, {}), {}, '/migtest');
        check('migration dance: unmount then re-mount a different type at the same path works',
            !global.FS.analyzePath('/migtest/a.bin').exists);
        global.FS.writeFile('/migtest/b.bin', new Uint8Array([4, 5, 6, 7]));
        await flushPath('/migtest');
        check('migration dance: the re-mounted backend records and mirrors writes',
            fs.existsSync(path.join(MIRROR_DIR, '..', path.basename(MIRROR_DIR) + '-mig', 'b.bin')));
        global.FS.unmount('/migtest');
    } catch (e) {
        check('migration dance: mount/unmount/remount', false, e.message);
    }

    // ---- 1. create + first (below-threshold) save -------------------------------------------
    const idx = global.Module._eden_menu_create_world();
    const displayName = utf8(global.Module._eden_menu_world_name(idx));
    global.Module._eden_menu_clear_pending_world_type();
    check('play() accepted', global.Module._eden_menu_play() === 1);
    const sawPlay = await waitUntil(() => menuState().game_mode === 1, 20000, 'game_mode == PLAY');
    if (!sawPlay) { console.log('FATAL: world never loaded'); process.exit(1); }
    await new Promise((r) => setTimeout(r, 1500));

    const ps = JSON.parse(utf8(global.Module._eden_debug_player_state()));
    const editX = Math.round(ps.pos[0]), editZ = Math.round(ps.pos[2]);
    const editY = Math.max(0, Math.round(ps.pos[1]) - 3);
    check('setblock accepted', global.Module._eden_console_setblock(editX, editZ, editY, 1) === 1);

    // Dirty a 10x10 grid of DIFFERENT columns (CHUNK_SIZE=16 apart, well inside the 288x288-block
    // resident window) so the world file is a few MB rather than 45 KB. Without this the delta
    // measurement below is meaningless: the save's fixed costs — the creature block, the directory
    // and B5's rollback journal — are all O(column count), and on a toy world they dwarf the file
    // itself, which says nothing about the 279 MB case this row exists for.
    let dirtied = 0;
    for (let i = -5; i < 5; i++) {
        for (let j = -5; j < 5; j++) {
            if (global.Module._eden_console_setblock(editX + 16 * i, editZ + 16 * j, editY, 1) === 1) dirtied++;
        }
    }
    check('dirtied a grid of columns to make a multi-MB world', dirtied >= 90, dirtied + ' of 100');

    let before = bytesWritten();
    await ensureMenuOpen();
    await tapHud(3);                       // rsave
    await flush();
    const scratchSaveBytes = bytesWritten() - before;
    compareMirror('below-threshold save');

    const wf = listWorlds().find((w) => w.name === displayName);
    check('world file listed after first save', !!wf);
    if (!wf) { console.log(failures + ' FAILURE(S)'); process.exit(1); }
    const worldSize = global.FS.stat('/documents/' + wf.file).size;
    console.log(`  world file ${wf.file}: ${worldSize.toLocaleString()} B`);
    console.log(`  below-threshold save mirrored ${scratchSaveBytes.toLocaleString()} B`);

    // ---- 2. the in-place (B5) save path — the one C2 is about --------------------------------
    // Below the threshold the ENGINE itself rewrites the whole file into a .savetmp scratch copy,
    // so no persistence layer can make that save a delta; C1 established that cost is fine at 64z.
    // Above it the engine writes only dirty columns + creature block + directory, and this is
    // where IDBFS's whole-file re-put was throwing the delta away.
    global.Module._eden_debug_set_save_inplace_threshold(0);
    check('threshold forced to 0 (in-place path)',
        global.Module._eden_debug_get_save_inplace_threshold() === 0);
    check('setblock #2 accepted',
        global.Module._eden_console_setblock(editX, editZ, Math.max(0, editY - 1), 1) === 1);

    before = bytesWritten();
    await ensureMenuOpen();
    await tapHud(3);
    await flush();
    const inplaceBytes = bytesWritten() - before;
    compareMirror('in-place save');
    console.log(`  in-place save mirrored ${inplaceBytes.toLocaleString()} B ` +
        `(${(100 * inplaceBytes / worldSize).toFixed(1)}% of the ${worldSize.toLocaleString()} B world)`);
    check('an in-place save mirrors a small fraction of the world file, not all of it',
        inplaceBytes > 0 && inplaceBytes < worldSize / 10,
        inplaceBytes + ' B of ' + worldSize + ' B');
    check('the in-place save mirrored less than the below-threshold one',
        inplaceBytes < scratchSaveBytes, inplaceBytes + ' vs ' + scratchSaveBytes);

    // ---- 3. a third save with nothing edited: the steady-state autosave cost ------------------
    before = bytesWritten();
    await ensureMenuOpen();
    await tapHud(3);
    await flush();
    const idleBytes = bytesWritten() - before;
    compareMirror('no-edit in-place save');
    console.log(`  no-edit in-place save mirrored ${idleBytes.toLocaleString()} B`);
    check('a no-edit in-place save mirrors far less than the world file', idleBytes < worldSize / 10,
        idleBytes + ' B');

    // ---- 3b. what C3's dirty-column journalling costs per save --------------------------------
    // ROADMAP C3 restored full atomicity to the in-place path by journalling the pre-image of
    // every column the save overwrites. Under IDBFS that cost was unmeasurable (the whole file was
    // re-put regardless); with this backend underneath it is exactly the number below, and it is
    // the number the row was blocked on. Both saves dirty ONE column, so the difference is one
    // column record (32,768 B at 64z, 131,072 B at 256z) plus a 24-byte record header.
    global.Module._eden_debug_set_save_journal_columns(0);
    check('setblock #3 accepted',
        global.Module._eden_console_setblock(editX, editZ, Math.max(0, editY - 2), 1) === 1);
    before = bytesWritten();
    await ensureMenuOpen();
    await tapHud(3);
    await flush();
    const noJournalBytes = bytesWritten() - before;
    compareMirror('in-place save, dirty-column journalling OFF');

    global.Module._eden_debug_set_save_journal_columns(1);
    check('setblock #4 accepted',
        global.Module._eden_console_setblock(editX, editZ, Math.max(0, editY - 3), 1) === 1);
    before = bytesWritten();
    await ensureMenuOpen();
    await tapHud(3);
    await flush();
    const journalBytes = bytesWritten() - before;
    compareMirror('in-place save, dirty-column journalling ON');
    console.log(`  one-dirty-column save: ${noJournalBytes.toLocaleString()} B without column ` +
        `journalling, ${journalBytes.toLocaleString()} B with it ` +
        `(+${(journalBytes - noJournalBytes).toLocaleString()} B, ` +
        `${(100 * journalBytes / noJournalBytes - 100).toFixed(0)}%)`);
    check('full atomicity costs at most one extra copy of the columns the save was writing anyway',
        journalBytes > noJournalBytes && journalBytes < 2.5 * noJournalBytes,
        `${journalBytes} vs ${noJournalBytes}`);
    check('...and the in-place save is still a small fraction of the world file',
        journalBytes < worldSize / 10, `${journalBytes} B of ${worldSize} B`);

    // ---- 4. quit, reload from MEMFS, save again ----------------------------------------------
    await ensureMenuOpen();
    await tapHud(6);                       // rexit
    const sawMenu = await waitUntil(() => menuState().game_mode === 0, 10000, 'back to MENU');
    check('back in the menu after quit', sawMenu);
    await flush();
    compareMirror('after quit-to-menu');

    // ---- 5. import a file through the same mount (the Storage tab's path) ---------------------
    const imported = new Uint8Array(4096);
    for (let i = 0; i < imported.length; i++) imported[i] = i & 0xff;
    global.FS.writeFile('/documents/Imported.bin', imported);
    await flush();
    compareMirror('after an import-shaped whole-file write');

    // ---- 6. delete propagates ----------------------------------------------------------------
    global.FS.unlink('/documents/Imported.bin');
    await flush();
    compareMirror('after a delete');
    check('the deleted file is gone from the mirror too',
        !fs.existsSync(path.join(MIRROR_DIR, 'Imported.bin')));

    // ---- 7. no scratch files left behind in the mirror ----------------------------------------
    const leftovers = fs.readdirSync(MIRROR_DIR).filter((n) => /\.savetmp|\.savejrnl/.test(n));
    check('no .savetmp/.savejrnl leftovers in the mirror', leftovers.length === 0, leftovers.join(','));

    // ---- 8. phase 2: a fresh process boots from the mirror alone ------------------------------
    console.log('--- phase 2: booting a fresh process from the mirror directory ---');
    const expect = [wf.file, String(editX), String(editZ), String(editY)].join(',');
    const r = spawnSync(process.execPath, [__filename, edenJsPath, '--phase2',
        '--dir=' + MIRROR_DIR, '--expect=' + expect], { encoding: 'utf8' });
    process.stdout.write((r.stdout || '').split('\n').map((l) => (l ? '  ' + l : l)).join('\n'));
    if (r.stderr && r.stderr.trim()) console.log('  [phase2 stderr]', r.stderr.trim().slice(0, 400));
    check('phase 2 (populate from the mirror, load the world, read the edited block) passed',
        r.status === 0);

    console.log(failures === 0 ? 'ALL PASS' : failures + ' FAILURE(S)');
    if (!dirArg) {
        fs.rmSync(MIRROR_DIR, { recursive: true, force: true });
        fs.rmSync(MIRROR_DIR + '-mig', { recursive: true, force: true });
    }
    process.exit(failures === 0 ? 0 : 1);
});

// Phase 2 runs in its own process with an empty MEMFS: everything it sees came out of the mirror
// through the backend's populate path.
async function runPhase2() {
    const [wantFile, xs, zs, ys] = EXPECT_ARG.split(',');
    const x = +xs, z = +zs, y = +ys;
    const worlds = listWorlds();
    check('phase2: the mirrored world is listed', worlds.some((w) => w.file === wantFile),
        JSON.stringify(worlds.map((w) => w.file)));
    const count = global.Module._eden_menu_world_count();
    let idx = -1;
    for (let i = 0; i < count; i++) {
        const nm = utf8(global.Module._eden_menu_world_name(i));
        const match = worlds.find((w) => w.file === wantFile);
        if (match && nm === match.name) { idx = i; break; }
    }
    check('phase2: the mirrored world is in the menu list', idx >= 0);
    if (idx >= 0) {
        global.Module._eden_menu_select(idx);
        check('phase2: play() accepted', global.Module._eden_menu_play() === 1);
        const played = await waitUntil(() => menuState().game_mode === 1, 20000, 'PLAY');
        check('phase2: the mirrored world loads', played);
        if (played) {
            await new Promise((r) => setTimeout(r, 800));
            const b = global.Module._eden_console_getblock(x, z, y);
            check('phase2: the block saved in phase 1 is present (type != 0/-1)', b > 0, 'got ' + b);
        }
    }
    console.log(failures === 0 ? 'PHASE2 ALL PASS' : failures + ' PHASE2 FAILURE(S)');
    process.exit(failures === 0 ? 0 : 1);
}
