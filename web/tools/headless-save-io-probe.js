// Save-path I/O profiler — the measurement WORKING/ROADMAP.md Tier 1 / 256z Stage 3 B5
// ("the atomic-save whole-file copy must go") asks for before any option is chosen.
//
// Asserts nothing. It wraps Emscripten's FS layer (every C fread/fwrite/lseek in the engine
// bottoms out there) and reports, per file path, how many bytes one FileManager::saveWorld()
// actually reads and writes. Byte counts are deterministic; the wall-clock numbers next to them
// are Debug-build MEMFS and should only be read as ratios.
//
// Usage: node tools/headless-save-io-probe.js [path/to/eden.js] [--specimen <file.eden>]
'use strict';
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const argv = process.argv.slice(2);
const positional = argv.filter((a) => !a.startsWith('--'));
const specIdx = argv.indexOf('--specimen');
const specimen = specIdx >= 0 ? argv[specIdx + 1] : null;
const edenJsPath = path.resolve(positional[0] || path.join(__dirname, '..', 'build-st', 'eden.js'));
const edenDir = path.dirname(edenJsPath);

global.require = require;
global.__dirname = edenDir;
global.__filename = edenJsPath;
global.Module = {
    print: () => {}, printErr: () => {},
    // The specimen must be on disk BEFORE main() runs: Menu::loadWorlds reads /documents exactly
    // once at boot, so a file dropped in postRun would never be listed.
    preRun: [function () {
        if (!specimen) return;
        try { global.FS.mkdir('/documents'); } catch (e) {}
        const bytes = fs.readFileSync(path.resolve(specimen));
        global.FS.writeFile('/documents/IOProbe.eden', new Uint8Array(bytes));
        console.log(`specimen ${path.basename(specimen)} -> /documents/IOProbe.eden (${bytes.length.toLocaleString()} B)`);
    }],
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

// ---- the FS probe -------------------------------------------------------------------------
let acct = null;
function base(p) {
    if (p && typeof p === 'object') p = p.path || (p.node && FS.getPath ? FS.getPath(p.node) : null);
    return typeof p === 'string' ? p.replace(/^.*\//, '') : '(anon)';
}
function note(kind, streamOrPath, len) {
    if (!acct) return;
    const p = base(typeof streamOrPath === 'string' ? streamOrPath : (streamOrPath && streamOrPath.path));
    const row = acct.files[p] || (acct.files[p] = { read: 0, write: 0, ops: 0 });
    row[kind] += (len | 0);
    row.ops++;
}
function installProbe() {
    const FS = global.FS;
    const oRead = FS.read.bind(FS), oWrite = FS.write.bind(FS);
    const oRename = FS.rename.bind(FS), oUnlink = FS.unlink.bind(FS), oTruncate = FS.truncate.bind(FS);
    FS.read = function (stream, buffer, offset, length, position) {
        const n = oRead(stream, buffer, offset, length, position); note('read', stream, n); return n;
    };
    FS.write = function (stream, buffer, offset, length, position, canOwn) {
        const n = oWrite(stream, buffer, offset, length, position, canOwn); note('write', stream, n); return n;
    };
    FS.rename = function (a, b) { if (acct) acct.renames.push(base(a) + ' -> ' + base(b)); return oRename(a, b); };
    FS.unlink = function (a) { if (acct) acct.unlinks.push(base(a)); return oUnlink(a); };
    FS.truncate = function (a, l) { if (acct) acct.truncates.push(base(a) + ' @' + l); return oTruncate(a, l); };
    global.FS = FS;
}
function startAcct() { acct = { files: {}, renames: [], unlinks: [], truncates: [], t0: Date.now() }; }
function stopAcct(label, worldFile) {
    const a = acct; acct = null;
    const size = worldFile ? global.FS.stat('/documents/' + worldFile).size : 0;
    let read = 0, write = 0;
    for (const k of Object.keys(a.files)) { read += a.files[k].read; write += a.files[k].write; }
    console.log(`\n=== ${label} ===`);
    console.log(`world file size after save: ${size.toLocaleString()} B`);
    console.log(`wall clock (Debug MEMFS):   ${Date.now() - a.t0} ms  (includes harness sleeps)`);
    console.log(`TOTAL read ${read.toLocaleString()} B   write ${write.toLocaleString()} B` +
        (size ? `   = ${(read / size).toFixed(2)}x / ${(write / size).toFixed(2)}x the file` : ''));
    for (const k of Object.keys(a.files).sort((x, y) => (a.files[y].read + a.files[y].write) - (a.files[x].read + a.files[x].write))) {
        const r = a.files[k];
        console.log(`   ${k.padEnd(34)} read ${String(r.read).padStart(12)}  write ${String(r.write).padStart(12)}  ops ${r.ops}`);
    }
    if (a.renames.length) console.log('   renames:  ', a.renames.join(', '));
    if (a.unlinks.length) console.log('   unlinks:  ', a.unlinks.join(', '));
    if (a.truncates.length) console.log('   truncates:', a.truncates.join(', '));
    return { size, read, write };
}
function findWorldFile(displayName) {
    const list = JSON.parse(utf8(global.Module._eden_storage_list_worlds()));
    return list.find((w) => w.name === displayName) || null;
}

global.Module.postRun = global.Module.postRun || [];
global.Module.postRun.push(async () => {
    installProbe();
    await waitUntil(() => !menuState().error, 5000, 'World/Menu');

    let displayName, worldFile;
    if (specimen) {
        worldFile = 'IOProbe.eden';
        const list = JSON.parse(utf8(global.Module._eden_storage_list_worlds()));
        const ent = list.find((w) => w.file === worldFile);
        if (!ent) { console.log('specimen not listed by storage; aborting'); process.exit(1); }
        displayName = ent.name;
        const count = global.Module._eden_menu_world_count();
        let idx = -1;
        for (let i = 0; i < count; i++) {
            if (utf8(global.Module._eden_menu_world_name(i)) === displayName) { idx = i; break; }
        }
        if (idx < 0) { console.log('specimen not in menu list; aborting'); process.exit(1); }
        global.Module._eden_menu_select(idx);
        if (global.Module._eden_menu_play() !== 1) { console.log('play refused'); process.exit(1); }
    } else {
        const idx = global.Module._eden_menu_create_world();
        displayName = utf8(global.Module._eden_menu_world_name(idx));
        global.Module._eden_menu_clear_pending_world_type();
        global.Module._eden_menu_play();
    }
    const played = await waitUntil(() => menuState().game_mode === 1, 60000, 'PLAY');
    if (!played) { console.log('never reached PLAY'); process.exit(1); }
    console.log('world format:', JSON.stringify(worldFormat()));
    await new Promise((r) => setTimeout(r, 2500));

    // save #1 — first save of the session (creates the file if new)
    await ensureMenuOpen();
    startAcct();
    await tapHud(3);
    if (!worldFile) worldFile = (findWorldFile(displayName) || {}).file;
    stopAcct('save #1 (first save of session)', worldFile);

    // save #2 — nothing changed since save #1: the pure overhead of the save path
    await ensureMenuOpen();
    startAcct();
    await tapHud(3);
    stopAcct('save #2 (no edits since #1)', worldFile);

    // save #3 — one block edited
    const st = JSON.parse(utf8(global.Module._eden_debug_player_state()));
    const [px, py, pz] = st.pos;
    global.Module._eden_console_setblock(Math.round(px), Math.round(pz), Math.max(1, Math.round(py) - 3), 13);
    await ensureMenuOpen();
    startAcct();
    await tapHud(3);
    stopAcct('save #3 (one block edited)', worldFile);

    process.exit(0);
});
