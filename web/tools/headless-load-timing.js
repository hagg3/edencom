// Measures how long the world load actually blocks the main thread — the standing evidence for
// project-audit row 9 / A6 ("pthread_create runs the world-load thread synchronously").
//
// WHY THIS IS A CHECKED-IN TOOL AND NOT A ONE-OFF: row 9 was written from reasoning, not
// measurement — "loading a world FREEZES the whole tab … on a slow phone with a large world this
// reads as a crash" — and it carried an Opus-5-high / M-effort recommendation on the strength of
// that sentence, plus a standing instruction not to start the threaded build (row 36) until it was
// fixed. When it was finally measured (2026-07-31) the block turned out to be ~20 ms, i.e. one
// dropped frame. That is the sort of claim that should never again be re-derived from prose, so it
// gets a tool.
//
// Usage:  node tools/headless-load-timing.js [path/to/eden.js]      (defaults to ../build-rel)
// Works against BOTH builds — it deliberately watches only `eden_menu_active()`, a normal export,
// rather than any EDEN_DIAGNOSTICS probe, so build-rel (what players actually run) can be measured.
//
// WHAT IT MEASURES, and the one thing it cannot:
//   * `longest_block_ms` — the largest gap between consecutive 1 ms timer callbacks while the load
//     runs. node's timers share the thread with the fake-rAF loop, so a tick that blocks for N ms
//     shows up as an N ms gap. This is exactly the quantity a browser tab would be unresponsive
//     for. It is the number that matters.
//   * `sync_range_requests_during_load` — how many SYNCHRONOUS XHRs the lazy Eden.eden FS node
//     would issue during the load on a range-capable host. Under node the backend is fs.readSync
//     so they cost nothing here, but in a browser each one is a full round trip of dead main
//     thread, and THAT is the part of row 9 that can still get genuinely slow. Note the deployed
//     site never takes this path (GitHub Pages ignores Range, so it uses the eager whole-file
//     fallback — audit row A11), which is why this is a dev-and-future-host concern, not a
//     player-facing one today.
//   * What it CANNOT measure: a real phone. Everything here is desktop CPU. Scale accordingly
//     (5-10x is the usual mobile-vs-desktop factor for this kind of decode-bound work) before
//     concluding anything about iOS Safari.
'use strict';
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const positional = process.argv.slice(2).filter((a) => !a.startsWith('--'));
const edenJsPath = path.resolve(positional[0] || path.join(__dirname, '..', 'build-rel', 'eden.js'));
const edenDir = path.dirname(edenJsPath);

global.require = require;
global.__dirname = edenDir;
global.__filename = edenJsPath;
global.Module = { print: () => {}, printErr: () => {} };

const cwdBefore = process.cwd();
process.chdir(edenDir);
vm.runInThisContext(fs.readFileSync(edenJsPath, 'utf8'), { filename: edenJsPath });
process.chdir(cwdBefore);

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

const requests = () =>
    (global.Module.EdenWorldFS && global.Module.EdenWorldFS.stats.requests) || 0;

// Runs `body()` while sampling the inter-timer gap, and reports the worst one.
async function timed(label, body) {
    let last = Date.now();
    const gaps = [];
    const sampler = setInterval(() => {
        const now = Date.now();
        gaps.push(now - last);
        last = now;
    }, 1);
    const rq0 = requests();
    const t0 = Date.now();
    const ok = await body();
    const total = Date.now() - t0;
    clearInterval(sampler);
    gaps.sort((a, b) => b - a);
    return {
        label,
        reached_play: ok,
        total_ms: total,
        longest_block_ms: gaps[0] || 0,
        blocks_over_10ms: gaps.filter((g) => g > 10).length,
        sync_range_requests_during_load: requests() - rq0,
    };
}

const inMenu = () => global.Module._eden_menu_active() === 1;

async function quitToMenu() {
    // Same route the pause menu takes: rexit (which=6) only lands while hud->inmenu, so open the
    // in-game menu (which=0) first. Matches headless-menu-flow-test.js.
    for (const which of [0, 6]) {
        global.Module._eden_tap_hud_button_begin(which);
        await new Promise((r) => setTimeout(r, 100));
        global.Module._eden_tap_hud_button_end(which);
        await new Promise((r) => setTimeout(r, 100));
    }
    return waitUntil(inMenu, 20000, 'back in the menu');
}

global.Module.postRun = global.Module.postRun || [];
global.Module.postRun.push(async () => {
    if (!(await waitUntil(inMenu, 15000, 'the menu to come up'))) {
        console.log('FATAL: never reached the menu');
        process.exit(1);
    }

    const idx = global.Module._eden_menu_create_world();
    global.Module._eden_menu_set_pending_world_type(0); // 0 = normal (streams the bundled world)
    const first = await timed('first load (create + generate from the bundled world)', () => {
        global.Module._eden_menu_play();
        return waitUntil(() => !inMenu(), 60000, 'GAME_MODE_PLAY');
    });
    console.log(JSON.stringify(first, null, 2));

    if (!(await quitToMenu())) { console.log('could not get back to the menu; stopping'); process.exit(0); }
    await new Promise((r) => setTimeout(r, 500));

    global.Module._eden_menu_select(idx >= 0 ? idx : 0);
    const second = await timed('second load (read the columns back from the save file)', () => {
        global.Module._eden_menu_play();
        return waitUntil(() => !inMenu(), 60000, 'GAME_MODE_PLAY');
    });
    console.log(JSON.stringify(second, null, 2));

    const worst = Math.max(first.longest_block_ms, second.longest_block_ms);
    console.log(`\nworst single main-thread block across both loads: ${worst} ms` +
        `  (${worst < 100 ? 'one or two dropped frames — NOT the multi-second freeze row 9 assumed'
                          : 'long enough to read as a hang; row 9 is live again, re-open it'})`);
    process.exit(0);
});
