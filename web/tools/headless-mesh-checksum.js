// headless-mesh-checksum.js — Phase N Stage 1 criterion 2
// (WORKING/native-migration-plan-2026-09-04.md): the WEB half of the cross-target geometry
// comparison. Its native twin is `native/build/eden_native --headless --stage1`, and the two are
// written to run the SAME session so their outputs can be compared directly:
//
//     create a "Normal" 64z world  ->  teleport to a FIXED origin  ->  settle N frames
//     ->  print eden_debug_mesh_checksum()
//
// THE FIXED ORIGIN IS THE WHOLE DESIGN. A new world spawns the player at a hashed position, and a
// chunk's contents are a function of position, so a run that did not teleport would fingerprint
// something different every time and the comparison would be vacuous. "Normal" rather than "Flat"
// for the same reason in reverse: the default world streams from the bundled 2880x2880 Eden.eden,
// so the same coordinates yield the same blocks on every platform — where a flat world would
// compare equal for the trivial reason that it is empty.
//
// WHAT THE TWO NUMBERS MEAN (full note at eden_debug_mesh_checksum() in src/seam/DebugState_web.mm):
//   geom  position + texcoord. Pure functions of block type and face visibility. MUST match across
//         platforms; a mismatch is the finding.
//   full  the above plus baked vertex colours, i.e. lighting — which is propagated incrementally
//         and sliced across frames, so it only agrees after the same amount of settling. `geom`
//         matching while `full` differs means "same geometry, lighting still converging".
//
// Needs a diagnostics build (eden_console_teleport and the eden_debug_* exports are
// EDEN_DIAGNOSTICS-only): build-st works, and so does a Release+diagnostics tree.
//
// Usage:
//   node tools/headless-mesh-checksum.js [path/to/eden.js] [--at=X,Y,Z] [--settle-ms=N] [--height=64|256]
//   node tools/headless-mesh-checksum.js [path/to/eden.js] --import=<file.eden>
//
// `--import` is criterion 4's cross-target half: instead of creating a world, it injects an
// EXISTING .eden into the module's /documents before boot and plays THAT. Point it at a save the
// native build wrote (`eden_native --save-roundtrip` prints the path) and the checksum this prints
// must equal the one native printed for the same file at the same origin — which is what "the
// format is platform-independent" means operationally, as opposed to by design.
//
//   node tools/headless-mesh-checksum.js [path/to/eden.js] --export=<dir>
//
// `--export` is the OTHER direction of the same check: place one block, save through the real HUD
// save button, and copy the resulting .eden out of MEMFS to <dir>. Drop that file in ~/Documents
// and `eden_native --headless --stage1 --world=<its display name>` must fingerprint it identically.
'use strict';
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const argv = process.argv.slice(2);
const positional = argv.filter((a) => !a.startsWith('--'));
const flag = (name, dflt) => {
    const a = argv.find((x) => x.startsWith(`--${name}=`));
    return a ? a.split('=')[1] : dflt;
};
const edenJsPath = path.resolve(positional[0] || path.join(__dirname, '..', 'build-st', 'eden.js'));
const edenDir = path.dirname(edenJsPath);
// Same default the native harness uses (see eden_main_native.cpp's --at). Keep them in step: the
// comparison is only meaningful at one shared coordinate.
const AT = flag('at', '1440,40,1440').split(',').map(Number);
const SETTLE_MS = Number(flag('settle-ms', 8000));
const HEIGHT = Number(flag('height', 64));
const IMPORT = flag('import', null);
const EXPORT = flag('export', null);

global.require = require;
global.__dirname = edenDir;
global.__filename = edenJsPath;
global.Module = { print: () => {}, printErr: () => {} };

// The world file has to be in /documents BEFORE main() runs, because Menu::loadWorlds scans that
// directory once during World construction — the same reason public/eden-storage.js does its
// IndexedDB/OPFS populate in preRun rather than after.
if (IMPORT) {
    global.Module.preRun = global.Module.preRun || [];
    global.Module.preRun.push(() => {
        try { global.FS.mkdir('/documents'); } catch (e) { /* already there */ }
        global.FS.writeFile('/documents/' + path.basename(IMPORT),
                            new Uint8Array(fs.readFileSync(IMPORT)));
    });
}

const cwdBefore = process.cwd();
process.chdir(edenDir);
vm.runInThisContext(fs.readFileSync(edenJsPath, 'utf8'), { filename: edenJsPath });
process.chdir(cwdBefore);

// Read a C string the export-independent, shared-memory-correct way — web/CLAUDE.md's rule about
// never touching a Module runtime method that is not in EXPORTED_RUNTIME_METHODS (doing so calls
// abort() and silently kills the engine from a *read*).
function utf8(ptr) {
    if (!ptr) return '';
    const heap = global.Module.HEAPU8;
    let end = ptr;
    while (heap[end] !== 0) end++;
    return Buffer.from(heap.buffer, heap.byteOffset + ptr, end - ptr).toString('utf8');
}
const inMenu = () => global.Module._eden_menu_active() === 1;
const menuState = () => JSON.parse(utf8(global.Module._eden_debug_menu_state()));

function waitUntil(predicate, timeoutMs, label) {
    return new Promise((resolve) => {
        const start = Date.now();
        const poll = () => {
            let ok = false;
            try { ok = predicate(); } catch (e) { /* not ready */ }
            if (ok) return resolve(true);
            if (Date.now() - start > timeoutMs) {
                console.log(`  (timed out waiting for: ${label})`);
                return resolve(false);
            }
            setTimeout(poll, 5);
        };
        poll();
    });
}

global.Module.postRun = global.Module.postRun || [];
global.Module.postRun.push(async () => {
    if (!(await waitUntil(inMenu, 30000, 'the menu'))) { console.log('FAIL: never reached the menu'); process.exit(1); }

    // BEFORE anything is meshed. The per-vertex fingerprint is opt-in because it costs a pass over
    // every published vertex in prepareVBO(); a probe that turned it on afterwards would read
    // `hashed: 0` and a confident, meaningless agreement.
    global.Module._eden_debug_set_mesh_checksum(1);

    let idx;
    if (IMPORT) {
        // The imported file is already on disk; the menu discovered it during construction. Find it
        // by FILE name, not display name — the display name is the world's, the file name is what
        // the other target told us.
        const want = path.basename(IMPORT);
        idx = -1;
        for (let i = 0; i < global.Module._eden_menu_world_count(); i++) {
            if (utf8(global.Module._eden_menu_world_file(i)) === want) { idx = i; break; }
        }
        if (idx < 0) { console.log(`FAIL: imported world ${want} not listed by the menu`); process.exit(1); }
    } else {
        global.Module._eden_menu_set_pending_world_type(0);    // Normal, not Flat — see the header
        global.Module._eden_menu_set_pending_world_height(HEIGHT);
        idx = global.Module._eden_menu_create_world();
        if (idx < 0) { console.log('FAIL: create_world'); process.exit(1); }
    }
    global.Module._eden_menu_select(idx);
    if (global.Module._eden_menu_play() !== 1) { console.log('FAIL: menu_play refused'); process.exit(1); }
    if (!(await waitUntil(() => menuState().game_mode === 1, 90000, 'GAME_MODE_PLAY'))) {
        console.log('FAIL: never reached PLAY'); process.exit(1);
    }

    global.Module._eden_console_teleport(AT[0], AT[1], AT[2]);
    await new Promise((r) => setTimeout(r, SETTLE_MS));

    if (EXPORT) {
        // One deliberate edit first, so the exported file is proving that WRITTEN data survives the
        // trip rather than that an untouched file is unchanged. Terrain's own argument order is
        // (x, z, y) with y vertical — CLAUDE.md convention #1. Same edit the native harness makes.
        global.Module._eden_console_setblock(AT[0] + 2, AT[2] + 2, AT[1], 1);
        await new Promise((r) => setTimeout(r, 500));
        // Save through the REAL HUD button, as synthetic touches. Every `hud->` input flag is
        // re-derived from the live touch set each frame, so writing the flag directly is silently
        // overwritten (web/CLAUDE.md's fast facts — this has bitten the port three times).
        const tapHud = async (which) => {
            global.Module._eden_tap_hud_button_begin(which);
            await new Promise((r) => setTimeout(r, 120));
            global.Module._eden_tap_hud_button_end(which);
            await new Promise((r) => setTimeout(r, 120));
        };
        if (global.Module._eden_hud_in_menu() === 0) await tapHud(0);
        await tapHud(3);                                    // rsave
        await new Promise((r) => setTimeout(r, 2000));
        const file = utf8(global.Module._eden_menu_world_file(idx));
        const name = utf8(global.Module._eden_menu_world_name(idx));
        const out = path.join(EXPORT, file);
        fs.mkdirSync(EXPORT, { recursive: true });
        fs.writeFileSync(out, Buffer.from(global.FS.readFile('/documents/' + file)));
        console.log(`[eden-stage1] exported ${out}  (display name: ${name})`);
    }

    const geometry = utf8(global.Module._eden_debug_terrain_geometry());
    const checksum = utf8(global.Module._eden_debug_mesh_checksum());
    const format = utf8(global.Module._eden_debug_world_format());
    console.log(`build:    ${edenJsPath}`);
    console.log(`origin:   ${AT.join(',')}   settle: ${SETTLE_MS} ms   height: ${HEIGHT}`);
    console.log(`geometry  ${geometry}`);
    console.log(`checksum  ${checksum}`);
    console.log(`format    ${format}`);

    // Not an assertion: there is nothing checked-in to compare against, because the reference is
    // the OTHER TARGET's run. Print in the same shape the native harness prints so a diff is
    // literally a diff.
    console.log(`[eden-stage1] world${HEIGHT}.geometry ${geometry}`);
    console.log(`[eden-stage1] world${HEIGHT}.checksum ${checksum}`);
    process.exit(0);
});
