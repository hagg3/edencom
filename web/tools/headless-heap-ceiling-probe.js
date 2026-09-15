// headless-heap-ceiling-probe.js — ROADMAP Phase M / M1 sizing.
//
// M1 sets -sMAXIMUM_MEMORY, which turns "the heap grew" into "the session aborted". The plan
// (WORKING/web-port-memory-plan.md §M1 "Sizing method") says the cap must be sized on the real
// ceiling, which is NOT the 64z number every other probe here reports:
//
//   step 1  peak linear memory on a 256z world under repeated reload bursts
//   step 3  a fragmentation torture run — 256z -> quit -> 64z -> quit -> 256z in ONE session,
//           because emmalloc never returns memory to the system and the world window is
//           reallocated at a different size on every world switch.
//
// The number it reports is `eden_debug_heap().peakSbrkTop` (src/seam/HeapProbe_web.mm) — linear
// memory the allocator has actually handed out, sampled on a 20 ms timer through the whole
// session so the peak is not missed between phases. `heapSize` (the WebAssembly.Memory length,
// i.e. peak + the growth policy's overshoot) is reported alongside because that, not sbrkTop, is
// what an -sMAXIMUM_MEMORY cap is actually compared against at growth time.
//
// Usage — needs a diagnostics build for _eden_console_teleport, same as the burst probes:
//   emcmake cmake -B build-relwdiag -DCMAKE_BUILD_TYPE=Release -DEDEN_DIAGNOSTICS=ON -DEDEN_THREADED=OFF
//   cmake --build build-relwdiag -j8
//   node tools/headless-heap-ceiling-probe.js [path/to/eden.js] [--cycles=N]
//   node tools/headless-heap-ceiling-probe.js [eden.js] [--cycles=N] [--torture=height|same256|same64]
// (build defaults to ../build-relwdiag; --cycles defaults to 2; --torture=height is the plan's
//  256z->64z->256z switch, same256/same64 are the same-world controls)
//
// Discard the first run against a freshly built tree (web/CLAUDE.md's cold-.wasm rule).
'use strict';
const fs = require('fs');
const os = require('os');
const path = require('path');
const vm = require('vm');
const { execFileSync } = require('child_process');

const argv = process.argv.slice(2);
const positional = argv.filter((a) => !a.startsWith('--'));
const cyclesArg = argv.find((a) => a.startsWith('--cycles='));
// Which world the torture run alternates BACK to. `height` (default) is the plan's 256z->64z->256z
// switch; `same256`/`same64` reload the SAME world every cycle, which is the control that says
// whether the growth is height-switch fragmentation or a plain per-load leak.
const tortureArg = (argv.find((a) => a.startsWith('--torture=')) || '--torture=height').split('=')[1];
const TORTURE_CYCLES = cyclesArg ? Math.max(1, parseInt(cyclesArg.split('=')[1], 10)) : 2;
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
const MB = (n) => +(n / (1024 * 1024)).toFixed(1);
const menuState = () => JSON.parse(utf8(global.Module._eden_debug_menu_state()));
const inMenu = () => global.Module._eden_menu_active() === 1;
const heap = () => JSON.parse(utf8(global.Module._eden_debug_heap()));

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
async function saveInPlace() {
    await ensureMenuOpen();
    await tapHud(3); // rsave
    await new Promise((r) => setTimeout(r, 500));
}
async function quitToMenu() {
    await ensureMenuOpen();
    await tapHud(6); // rexit
    return waitUntil(() => menuState().game_mode === 0, 20000, 'game_mode back to MENU');
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
    return waitUntil(() => menuState().game_mode === 1, 60000, `game_mode == PLAY (${displayName})`);
}
async function createAndSaveWorld() {
    const idx = global.Module._eden_menu_create_world();
    const displayName = utf8(global.Module._eden_menu_world_name(idx));
    global.Module._eden_menu_clear_pending_world_type();
    if (global.Module._eden_menu_play() !== 1) { console.log('FATAL: play() rejected'); process.exit(1); }
    if (!(await waitUntil(() => !inMenu(), 90000, 'GAME_MODE_PLAY'))) { console.log('FATAL: never reached play'); process.exit(1); }
    await new Promise((r) => setTimeout(r, 1500)); // let initial streaming settle
    await saveInPlace();
    if (!findWorldFile(displayName)) { console.log('FATAL: saved world not discoverable'); process.exit(1); }
    if (!(await quitToMenu())) { console.log('FATAL: did not return to menu'); process.exit(1); }
    return displayName;
}

// Same burst coordinates as the mesh burst probes — the point is the memory a bulk reload costs,
// and these are the teleports whose reload volume is already characterised.
const TARGETS = [
    [64700, 40, 65700], [65100, 40, 65350], [64300, 40, 66000],
    [65500, 40, 65100], [63900, 40, 66300],
];
async function burstRun(dwellMs) {
    for (const [x, y, z] of TARGETS) {
        global.Module._eden_console_teleport(x, y, z);
        await new Promise((r) => setTimeout(r, dwellMs));
    }
}

// The peak is sample-based (HeapProbe_web.mm updates its high-water on every call), so poll it
// continuously rather than only at phase boundaries.
let sampler = null;
const phases = [];
function mark(label) {
    const h = heap();
    phases.push({ phase: label, sbrkTop_MB: MB(h.sbrkTop), peakSbrkTop_MB: MB(h.peakSbrkTop), heapSize_MB: MB(h.heapSize) });
    console.log(`  ${label.padEnd(34)} sbrkTop ${String(MB(h.sbrkTop)).padStart(6)} MB   peak ${String(MB(h.peakSbrkTop)).padStart(6)} MB   heapSize ${String(MB(h.heapSize)).padStart(6)} MB`);
}

global.Module.postRun = global.Module.postRun || [];
global.Module.postRun.push(async () => {
    if (!(await waitUntil(inMenu, 30000, 'the menu to come up'))) {
        console.log('FATAL: never reached the menu');
        process.exit(1);
    }
    sampler = setInterval(() => { try { heap(); } catch (e) { /* during teardown */ } }, 20);

    const cap = heap().heapMax;
    console.log(`build: ${edenJsPath}`);
    console.log(`-sMAXIMUM_MEMORY cap (emscripten_get_heap_max): ${MB(cap)} MB\n`);
    mark('menu, pre-world');

    // ---- two real worlds: one stays 64z, one becomes the 256z ceiling ------------------------
    console.log('[phase 1] create the 64z control world');
    const name64 = await createAndSaveWorld();
    mark('after 64z create+save+quit');

    console.log('[phase 2] create + convert the 256z world');
    const name256 = await createAndSaveWorld();
    const wf = findWorldFile(name256);
    const memPath = '/documents/' + wf.file;
    const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'edenheapceil-'));
    const src64 = path.join(tmp, 'w64.eden');
    const out256 = path.join(tmp, 'w256.eden');
    fs.writeFileSync(src64, Buffer.from(global.FS.readFile(memPath)));
    execFileSync(process.execPath, [CONVERT, '--to-256', src64, '-o', out256, '--yes'], { encoding: 'utf8' });
    global.FS.writeFile(memPath, new Uint8Array(fs.readFileSync(out256)));
    mark('after 256z convert (in menu)');

    // ---- step 1: the 256z ceiling under repeated reload bursts -------------------------------
    console.log('\n[step 1] 256z ceiling — 5 teleport bursts + a save');
    if (!(await playWorldNamed(name256))) { console.log('FATAL: 256z world did not reach PLAY'); process.exit(1); }
    await new Promise((r) => setTimeout(r, 2000));
    mark('256z loaded, settled');
    await burstRun(2500);
    mark('256z after 5 bursts');
    await saveInPlace();
    mark('256z after save');
    const ceiling256 = heap().peakSbrkTop;

    // ---- step 3: the fragmentation torture run ----------------------------------------------
    // 256z -> quit -> 64z -> quit -> 256z, in ONE session. emmalloc does not return memory to the
    // system, and the world window arrays are reallocated at a different size on each switch, so
    // if the cap has to absorb fragmentation this is where it shows up.
    // TWO cycles, not one: the question a cap has to answer is whether the fragmentation cost is
    // a one-off step (a cap can absorb it) or grows per switch (a cap turns a long session into an
    // abort, and the real bug is fragmentation, not the flag).
    console.log(`\n[step 3] fragmentation torture (--torture=${tortureArg}) — ${TORTURE_CYCLES} cycles in one session`);
    const cycleWorld = tortureArg === 'same64' ? name64 : name256;
    const cycleLabel = tortureArg === 'same64' ? '64z' : '256z';
    for (let cycle = 1; cycle <= TORTURE_CYCLES; cycle++) {
        if (!(await quitToMenu())) { console.log('FATAL: did not return to menu after 256z'); process.exit(1); }
        mark(`torture c${cycle}: quit 256z`);
        const alt = tortureArg === 'height' ? name64 : cycleWorld;
        const altLabel = tortureArg === 'height' ? '64z' : `${cycleLabel}(alt)`;
        if (!(await playWorldNamed(alt))) { console.log(`FATAL: ${altLabel} world did not reach PLAY`); process.exit(1); }
        await new Promise((r) => setTimeout(r, 1500));
        await burstRun(tortureArg === 'same64' ? 1200 : 2500);
        mark(`torture c${cycle}: ${altLabel} + bursts`);
        if (!(await quitToMenu())) { console.log(`FATAL: did not return to menu after ${altLabel}`); process.exit(1); }
        mark(`torture c${cycle}: quit ${altLabel}`);
        if (!(await playWorldNamed(cycleWorld))) { console.log(`FATAL: ${cycleLabel} world did not reload`); process.exit(1); }
        await new Promise((r) => setTimeout(r, 2000));
        await burstRun(2500);
        mark(`torture c${cycle}: ${cycleLabel} again + bursts`);
    }
    await saveInPlace();
    mark('torture: 256z save');

    clearInterval(sampler);
    const final = heap();
    console.log('\n' + JSON.stringify({ build: edenJsPath, heapMax_MB: MB(cap), phases,
        ceiling_256z_peakSbrkTop_MB: MB(ceiling256),
        session_peakSbrkTop_MB: MB(final.peakSbrkTop),
        session_final_heapSize_MB: MB(final.heapSize) }, null, 2));
    console.log(`\n[M1 sizing] 256z peak linear memory: ${MB(ceiling256)} MB` +
        `  ·  whole-session peak (incl. fragmentation torture): ${MB(final.peakSbrkTop)} MB` +
        `  ·  final heapSize (peak + growth overshoot): ${MB(final.heapSize)} MB`);
    console.log(`[M1 sizing] headroom left under the ${MB(cap)} MB cap: ${MB(cap - final.peakSbrkTop)} MB`);
    // M6 (2026-09-02) fixed the ~22 MB-per-world-load leak this probe found, so a torture run
    // should now be FLAT: peak is bounded by the biggest world the session opens, not by how many
    // it opened. A rising sbrkTop across the torture phases is the regression to look for.
    const tortureStart = phases.find((p) => p.phase.startsWith('torture c1: quit'));
    const tortureEnd = phases[phases.length - 1];
    if (tortureStart) {
        const drift = +(tortureEnd.sbrkTop_MB - tortureStart.sbrkTop_MB).toFixed(1);
        console.log(`[M6] sbrkTop drift across the ${TORTURE_CYCLES}-cycle torture run: ${drift} MB` +
            `  (${drift <= 2 ? 'FLAT — no per-load leak' : 'RISING — the per-load leak is back'})`);
    }
    console.log(`[M1 sizing] 1.5x the session peak = ${MB(final.peakSbrkTop * 1.5)} MB` +
        `  ·  rounded up to a 16 MB growth step = ${MB(Math.ceil(final.peakSbrkTop * 1.5 / (16 * 1024 * 1024)) * 16 * 1024 * 1024)} MB`);
    process.exit(0);
});
