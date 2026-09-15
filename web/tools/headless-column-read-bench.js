// headless-column-read-bench.js — B6 (ROADMAP Phase B): an isolated measurement of the shim's
// file-read path, i.e. NSFileHandle -readDataOfLength: as fmh_readColumnRawFromDefault drives it
// (a seek plus 8 reads per bundled-map column: a 2-byte length prefix and a payload, per RLE band).
//
// WHY NOT JUST USE headless-mesh-burst-probe.js: that probe measures this path inside a live
// teleport burst, sharing a run with meshing, worker scheduling, lighting and the block cache's
// warm-up. Its run-to-run spread on the column-read number is about +-20% — wider than the effect
// B6 is trying to move, so it can tell you the burst got better but not whether a change to the
// read path did anything. This drives eden_debug_bench_column_read() (DevConsole_web.mm) instead:
// same function, fixed column set, warm cache, nothing else running. Spread here is a few percent.
//
// Usage:
//   node tools/headless-column-read-bench.js [path/to/eden.js] [--side N] [--iters N]
// Defaults to ../build-relthr/eden.js. Needs an EDEN_DIAGNOSTICS build (the exports are gated).
'use strict';
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const argv = process.argv.slice(2);
const flag = (name, dflt) => {
    const i = argv.indexOf('--' + name);
    return i >= 0 && argv[i + 1] ? parseInt(argv[i + 1], 10) : dflt;
};
const positional = argv.filter((a, i) => !a.startsWith('--') && !(i > 0 && argv[i - 1].startsWith('--')));
const edenJsPath = path.resolve(positional[0] || path.join(__dirname, '..', 'build-relthr', 'eden.js'));
const SIDE = flag('side', 24);      // 24x24 = 576 columns, about half a bulk reload's window
const ITERS = flag('iters', 12);
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
            if (Date.now() - start > timeoutMs) { console.log(`  (timed out waiting for: ${label})`); return resolve(false); }
            setTimeout(poll, 5);
        };
        poll();
    });
}
const inMenu = () => global.Module._eden_menu_active() === 1;
const cstr = (ptr) => { let e = ptr; while (global.Module.HEAPU8[e] !== 0) e++;
    return Buffer.from(global.Module.HEAPU8.slice(ptr, e)).toString('utf8'); };

global.Module.postRun = global.Module.postRun || [];
global.Module.postRun.push(async () => {
    if (!(await waitUntil(inMenu, 15000, 'the menu'))) { console.log('FATAL: never reached the menu'); process.exit(1); }
    global.Module._eden_menu_create_world();
    global.Module._eden_menu_set_pending_world_type(0);
    global.Module._eden_menu_play();
    if (!(await waitUntil(() => !inMenu(), 60000, 'GAME_MODE_PLAY'))) { console.log('FATAL: never reached play'); process.exit(1); }
    await new Promise((r) => setTimeout(r, 2000));

    if (typeof global.Module._eden_debug_bench_column_read !== 'function') {
        console.log('FATAL: eden_debug_bench_column_read missing — build with -DEDEN_DIAGNOSTICS=ON');
        process.exit(1);
    }
    // A FIXED origin, not the resident window's. A new world spawns the player at a random-ish
    // place, so `fm->chunkOffsetX/Z` moves by tens of columns between runs — and since a column's
    // cost is proportional to how many RLE bytes it holds, that alone swings the result by more
    // than any change to the read path does. These coordinates sit inside the bundled map for
    // every world seed (asserted below via the hit count); pass --origin to move them.
    const ORIGIN_DEFAULT = [4080, 4080];
    const oi = argv.indexOf('--origin');
    const [cx0, cz0] = oi >= 0 ? argv[oi + 1].split(',').map(Number) : ORIGIN_DEFAULT;
    const stats = JSON.parse(cstr(global.Module._eden_console_world_stats()));
    void stats;
    const hits = global.Module._eden_debug_bench_column_hits(cx0, cz0, SIDE);
    if (!hits) { console.log(`FATAL: no bundled-map columns at (${cx0},${cz0}) — nothing to measure`); process.exit(1); }

    // Record sizes, so the timing can be read against how much data a column actually holds.
    const totalBytes = global.Module._eden_debug_bench_column_bytes(cx0, cz0, SIDE);
    const maxBytes = global.Module._eden_debug_bench_column_maxbytes();

    global.Module._eden_debug_bench_column_read(cx0, cz0, SIDE, 2);   // warm the block cache + JIT
    const runs = [];
    for (let i = 0; i < 5; i++) runs.push(global.Module._eden_debug_bench_column_read(cx0, cz0, SIDE, ITERS));
    runs.sort((a, b) => a - b);
    const median = runs[Math.floor(runs.length / 2)];
    const perColumnUs = (median * 1000) / (hits * ITERS);
    console.log(JSON.stringify({
        build: edenJsPath,
        origin: [cx0, cz0], side: SIDE, columnsPresent: hits, iters: ITERS,
        avg_record_bytes: Math.round(totalBytes / hits), max_record_bytes: maxBytes,
        runs_ms: runs.map((r) => +r.toFixed(2)),
        median_ms: +median.toFixed(2),
        spread_pct: +(((runs[runs.length - 1] - runs[0]) / median) * 100).toFixed(1),
        us_per_column: +perColumnUs.toFixed(2),
    }, null, 2));
    process.exit(0);
});
