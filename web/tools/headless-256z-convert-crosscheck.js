// Cross-check: FileManager::convertWorldTo64 (the C++ port that backs the Settings -> Storage
// "Convert to 64z" button, Storage_web.mm -> Classes/FileManager.mm) vs. the reference algorithm
// in tools/eden-convert.js's `--to-64` direction, on a 256z world that has REAL content above
// y=63 — the gap headless-256z-authoring-test.js explicitly leaves open (see its header and
// ROADMAP.md Phase A "convertWorldTo64 counter cross-check").
//
// headless-256z-authoring-test.js only converts a freshly-created flat world that has never
// streamed a high band to disk, so its discard/orphan/relocate counters are all 0 and untested.
// headless-256z-test.js cross-checks the READER against eden-convert.js but never the 64z writer.
// This test closes that: it authors known content above the 64z ceiling, saves a real 256z file,
// then converts the SAME bytes both ways and asserts the two implementations report the same
// blocks-discarded / columns-affected / doors-orphaned / creatures-dropped counts.
//
// Usage: node tools/headless-256z-convert-crosscheck.js [path/to/eden.js]  (defaults to ../build-st/eden.js)
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
const M = () => global.Module;
const menuState = () => JSON.parse(utf8(M()._eden_debug_menu_state()));
const worldFormat = () => JSON.parse(utf8(M()._eden_debug_world_format()));

function waitUntil(predicate, timeoutMs, label) {
    return new Promise((resolve) => {
        const start = Date.now();
        const poll = () => {
            let ok = false;
            try { ok = predicate(); } catch (e) { /* not ready */ }
            if (ok) return resolve(true);
            if (Date.now() - start > timeoutMs) { console.log(`  (timed out: ${label})`); return resolve(false); }
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

function tapHud(which) {
    return new Promise((resolve) => {
        M()._eden_tap_hud_button_begin(which);
        setTimeout(() => { M()._eden_tap_hud_button_end(which); setTimeout(resolve, 100); }, 100);
    });
}
async function ensureMenuOpen() { if (M()._eden_hud_in_menu() === 0) await tapHud(0); }
async function quitToMenu() {
    await ensureMenuOpen();
    await tapHud(6);
    return waitUntil(() => menuState().game_mode === 0, 10000, 'game_mode back to MENU');
}
function findWorldFile(displayName) {
    return JSON.parse(utf8(M()._eden_storage_list_worlds())).find((w) => w.name === displayName) || null;
}
async function playWorldNamed(displayName) {
    const count = M()._eden_menu_world_count();
    let idx = -1;
    for (let i = 0; i < count; i++) if (utf8(M()._eden_menu_world_name(i)) === displayName) { idx = i; break; }
    if (idx < 0) return false;
    M()._eden_menu_select(idx);
    if (M()._eden_menu_play() !== 1) return false;
    return waitUntil(() => menuState().game_mode === 1, 30000, `PLAY (${displayName})`);
}

// Parse the CLI reporter's `--to-64` output (tools/eden-convert.js lines ~470-481).
function parseCliReport(stdout) {
    const grab = (re) => { const m = stdout.match(re); return m ? m.slice(1).map((s) => Number(String(s).replace(/,/g, ''))) : null; };
    const blocks = grab(/non-air blocks\s+([\d,]+) destroyed, across (\d+) of (\d+) columns/);
    const doors = grab(/orphaned doors\s+(\d+) cleared/);
    const creat = grab(/creatures\s+(\d+) dropped, (\d+) relocated into free slots, (\d+) lost to overflow/);
    return {
        blocksDiscarded: blocks ? blocks[0] : NaN,
        columnsAffected: blocks ? blocks[1] : NaN,
        doorsOrphaned: doors ? doors[0] : NaN,
        creaturesDropped: creat ? creat[0] : NaN,
        creaturesRelocated: creat ? creat[1] : NaN,
        creaturesOverflow: creat ? creat[2] : NaN,
    };
}

const TYPE_BRICK = 13, TYPE_DOOR1 = 66, TYPE_DOOR_TOP = 70;

global.Module.postRun = global.Module.postRun || [];
global.Module.postRun.push(async () => {
    if (!(await waitUntil(() => !menuState().error, 5000, 'World/Menu'))) { console.log('FATAL'); process.exit(1); }

    // ---- 1. a real 256z world (height picker), flat so high bands start empty -----------------
    const idx = M()._eden_menu_create_world();
    const displayName = utf8(M()._eden_menu_world_name(idx));
    M()._eden_menu_set_pending_world_type(1);       // flat
    M()._eden_menu_set_pending_world_height(256);
    check('play() accepted', M()._eden_menu_play() === 1);
    check('reached PLAY', await waitUntil(() => menuState().game_mode === 1, 30000, 'PLAY'));
    const fmt = worldFormat();
    check('world really opened at height 256', fmt.height === 256, JSON.stringify(fmt));
    await new Promise((r) => setTimeout(r, 1500)); // let columns stream/mesh

    // ---- 2. author known content above the 64z ceiling --------------------------------------
    const st = JSON.parse(utf8(M()._eden_debug_player_state()));
    const [px, , pz] = st.pos;
    const bx = Math.round(px), bz = Math.round(pz);

    // A 6x6 lattice of pillars, 20 blocks apart so each lands in a distinct 16-wide file column.
    const HIGH_YS = [64, 90, 150, 220];
    let placed = 0;
    const cells = [];
    for (let gx = 0; gx < 6; gx++) {
        for (let gz = 0; gz < 6; gz++) {
            const x = bx + (gx - 3) * 20, z = bz + (gz - 3) * 20;
            cells.push([x, z]);
            for (const y of HIGH_YS) { M()._eden_console_setblock(x, z, y, TYPE_BRICK); placed++; }
        }
    }
    // Three orphaned-door setups on separate columns: DOOR1 at y=63 (retained), DOOR_TOP at y=64
    // (cut) -> each is +1 discarded block, +1 affected column, +1 orphaned door.
    const doorCells = [[bx + 3 * 20, bz], [bx, bz + 3 * 20], [bx - 3 * 20, bz - 1 * 20]];
    for (const [x, z] of doorCells) {
        M()._eden_console_setblock(x, z, 63, TYPE_DOOR1);
        M()._eden_console_setblock(x, z, 64, TYPE_DOOR_TOP);
    }
    check('a high-band block reads back before saving',
        M()._eden_console_getblock(bx, bz, 220) === TYPE_BRICK,
        M()._eden_console_getblock(bx, bz, 220));

    await tapHud(3); // rsave
    await new Promise((r) => setTimeout(r, 300));
    const wf = findWorldFile(displayName);
    check('world file discoverable after the 256z save', !!wf);
    if (!wf) { console.log('FATAL'); process.exit(1); }
    if (!(await quitToMenu())) { console.log('FATAL: no return to menu'); process.exit(1); }

    // ---- 3. reference conversion: tools/eden-convert.js --to-64 over the saved bytes ---------
    const memPath = '/documents/' + wf.file;
    const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'eden256xcheck-'));
    const src256 = path.join(tmp, 'src256.eden');
    const out64 = path.join(tmp, 'out64.eden');
    fs.writeFileSync(src256, Buffer.from(global.FS.readFile(memPath)));
    const cliOut = execFileSync(process.execPath, [CONVERT, '--to-64', src256, '-o', out64, '--yes'], { encoding: 'utf8' });
    const ref = parseCliReport(cliOut);
    console.log('eden-convert.js --to-64 report:', JSON.stringify(ref));
    check('the CLI reported a non-trivial discard (sanity: content really landed above y=63)',
        Number.isFinite(ref.blocksDiscarded) && ref.blocksDiscarded > 0, JSON.stringify(ref));
    check('the CLI reported the orphaned doors', ref.doorsOrphaned === doorCells.length, ref.doorsOrphaned);

    // ---- 4. engine conversion: FileManager::convertWorldTo64 over the same world -------------
    const worlds = JSON.parse(utf8(M()._eden_storage_list_worlds()));
    const wIndex = worlds.findIndex((w) => w.name === displayName);
    check('the 256z world resolves to a storage-list index', wIndex >= 0);
    const eng = JSON.parse(utf8(M()._eden_storage_convert_to_64z_at(wIndex)));
    console.log('FileManager::convertWorldTo64 report:', JSON.stringify(eng));
    check('convertWorldTo64 reported ok', eng.ok === true, JSON.stringify(eng));

    // ---- 5. the two implementations must agree ----------------------------------------------
    check(`blocksDiscarded matches  (engine ${eng.blocksDiscarded} == ref ${ref.blocksDiscarded})`,
        eng.blocksDiscarded === ref.blocksDiscarded);
    check(`columnsAffected matches  (engine ${eng.columnsAffected} == ref ${ref.columnsAffected})`,
        eng.columnsAffected === ref.columnsAffected);
    check(`doorsOrphaned matches    (engine ${eng.doorsOrphaned} == ref ${ref.doorsOrphaned})`,
        eng.doorsOrphaned === ref.doorsOrphaned);
    check(`creaturesDropped matches (engine ${eng.creaturesDropped} == ref ${ref.creaturesDropped})`,
        eng.creaturesDropped === ref.creaturesDropped);
    check(`creaturesRelocated matches (engine ${eng.creaturesRelocated} == ref ${ref.creaturesRelocated})`,
        eng.creaturesRelocated === ref.creaturesRelocated);
    check(`creaturesOverflow matches (engine ${eng.creaturesOverflow} == ref ${ref.creaturesOverflow})`,
        eng.creaturesOverflow === ref.creaturesOverflow);

    // and the engine's own view of what it authored, as an independent floor on the numbers
    check('engine blocksDiscarded >= the bricks placed above y=63 plus the door tops',
        eng.blocksDiscarded >= placed + doorCells.length, `${eng.blocksDiscarded} vs ${placed + doorCells.length}`);

    fs.rmSync(tmp, { recursive: true, force: true });
    console.log(failures === 0 ? '\nALL CHECKS PASSED' : `\n${failures} CHECK(S) FAILED`);
    process.exit(failures === 0 ? 0 : 1);
});
