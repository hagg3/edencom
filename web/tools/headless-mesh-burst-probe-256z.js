// 256z variant of headless-mesh-burst-probe.js (ROADMAP.md Tier 0: "does the 64z frame-budget
// number hold at 4x scale before Stage 3 design work assumes it does").
//
// Same measurement (worst main-thread block across several bulk-reload teleports), but against a
// world converted to 256z first, using the same create->save->convert->reload recipe
// headless-256z-test.js uses (there is no way to create a 256z world directly at runtime).
//
// Usage: same scratch build as the 64z probe —
//   emcmake cmake -B build-relwdiag -DCMAKE_BUILD_TYPE=Release -DEDEN_DIAGNOSTICS=ON -DEDEN_THREADED=OFF
//   cmake --build build-relwdiag -j8
//   node tools/headless-mesh-burst-probe-256z.js [path/to/eden.js]   (defaults to ../build-relwdiag)
'use strict';
const fs = require('fs');
const os = require('os');
const path = require('path');
const vm = require('vm');
const { execFileSync } = require('child_process');

const positional = process.argv.slice(2).filter((a) => !a.startsWith('--'));
const edenJsPath = path.resolve(positional[0] || path.join(__dirname, '..', 'build-relwdiag', 'eden.js'));
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
const inMenu = () => global.Module._eden_menu_active() === 1;

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

function readTiming() {
    const ptr = global.Module._eden_debug_mesh_timing();
    let end = ptr;
    while (global.Module.HEAPU8[end] !== 0) end++;
    return JSON.parse(Buffer.from(global.Module.HEAPU8.slice(ptr, end)).toString('utf8'));
}
async function timedBlock(label, body) {
    let last = Date.now();
    const gaps = [];
    const sampler = setInterval(() => {
        const now = Date.now();
        gaps.push(now - last);
        last = now;
    }, 1);
    await body();
    clearInterval(sampler);
    gaps.sort((a, b) => b - a);
    return { label, longest_block_ms: gaps[0] || 0, blocks_over_16ms: gaps.filter((g) => g > 16.66).length };
}

global.Module.postRun = global.Module.postRun || [];
global.Module.postRun.push(async () => {
    if (!(await waitUntil(inMenu, 15000, 'the menu to come up'))) {
        console.log('FATAL: never reached the menu');
        process.exit(1);
    }

    // ---- create+save a normal 64z world so it streams real terrain from the bundled Eden.eden --
    const idx = global.Module._eden_menu_create_world();
    const displayName = utf8(global.Module._eden_menu_world_name(idx));
    global.Module._eden_menu_clear_pending_world_type();
    if (global.Module._eden_menu_play() !== 1) { console.log('FATAL: play() rejected'); process.exit(1); }
    if (!(await waitUntil(() => !inMenu(), 60000, 'GAME_MODE_PLAY'))) { console.log('FATAL: never reached play'); process.exit(1); }
    await new Promise((r) => setTimeout(r, 1500)); // let initial streaming settle
    await ensureMenuOpen();
    await tapHud(3); // rsave
    const wf = findWorldFile(displayName);
    if (!wf) { console.log('FATAL: saved world not discoverable'); process.exit(1); }
    if (!(await quitToMenu())) { console.log('FATAL: did not return to menu'); process.exit(1); }

    // ---- convert that save to 256z with the real offline converter ---------------------------
    const memPath = '/documents/' + wf.file;
    const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'eden256burst-'));
    const src64 = path.join(tmp, 'w64.eden');
    const out256 = path.join(tmp, 'w256.eden');
    fs.writeFileSync(src64, Buffer.from(global.FS.readFile(memPath)));
    execFileSync(process.execPath, [CONVERT, '--to-256', src64, '-o', out256, '--yes'], { encoding: 'utf8' });
    global.FS.writeFile(memPath, new Uint8Array(fs.readFileSync(out256)));

    if (!(await playWorldNamed(displayName))) { console.log('FATAL: converted 256z world did not reach PLAY'); process.exit(1); }
    await new Promise((r) => setTimeout(r, 2000)); // let the initial 256z load churn settle

    const results = [];
    // Same coordinates as the 64z burst probe — the point is a like-for-like comparison at 4x
    // the chunk height, not a different burst shape.
    const targets = [
        [64700, 40, 65700], [65100, 40, 65350], [64300, 40, 66000],
        [65500, 40, 65100], [63900, 40, 66300],
    ];
    for (const [x, y, z] of targets) {
        global.Module._eden_debug_mesh_timing_reset();
        const r = await timedBlock(`teleport to (${x},${y},${z})`, async () => {
            global.Module._eden_console_teleport(x, y, z);
            await new Promise((res) => setTimeout(res, 2500)); // 256z burst is ~4x the chunks; give it more real time
        });
        const timing = readTiming();
        results.push({ ...r, ...timing, meshMsPerChunk: timing.meshCount ? +(timing.meshMs / timing.meshCount).toFixed(4) : 0 });
    }

    console.log(JSON.stringify(results, null, 2));
    const worst = results.reduce((a, b) => (b.longest_block_ms > a.longest_block_ms ? b : a));
    console.log(`\n[256z] worst single main-thread block across ${results.length} teleport bursts: ` +
        `${worst.longest_block_ms} ms (${worst.meshCount} chunks meshed, ${worst.meshMs.toFixed(1)}ms of ` +
        `mesh CPU time, ${worst.uploadMs.toFixed(2)}ms of GL upload time) — build: ${edenJsPath}`);
    process.exit(0);
});
