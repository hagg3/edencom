// Measures the worst-case main-thread block a teleport/warp causes, by triggering
// Terrain::prepareAndLoadGeometry's "count>140" bulk-reload path (Classes/Terrain.mm) and
// sampling the inter-timer gap the same way tools/headless-load-timing.js measures the initial
// world load — this is the other half of the same question, applied to the far larger burst a
// fast teleport can produce mid-session rather than the one-time boot load.
//
// WHY A SEPARATE SCRATCH BUILD (build-relwdiag, -DCMAKE_BUILD_TYPE=Release -DEDEN_DIAGNOSTICS=ON):
// eden_console_teleport is EDEN_DIAGNOSTICS-only (DevConsole_web.mm), which build-rel compiles
// out — but build-st is -O0, and a first pass through real Safari (see the tool's own commit
// history / RESUME-HERE) measured ~8x higher per-chunk mesh cost there than build-rel's -O2
// walking measurement, an apples-to-oranges gap, not a real difference. This combination was
// already established clean by pass 53 (project-audit-2026-07-30 row B1's investigation) — Release
// codegen with the diagnostics probes still linked in — so it is the fair way to get both a real
// -O2 number AND console access to trigger a burst deterministically.
//
// Usage:
//   emcmake cmake -B build-relwdiag -DCMAKE_BUILD_TYPE=Release -DEDEN_DIAGNOSTICS=ON -DEDEN_THREADED=OFF
//   cmake --build build-relwdiag -j8
//   node tools/headless-mesh-burst-probe.js [path/to/eden.js]   (defaults to ../build-relwdiag)
// build-relwdiag is a scratch tree, not checked in and not one of the three standard trees —
// delete it when done (RESUME-HERE's "scratch CMake dirs go directly under web/" gotcha).
'use strict';
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const argv = process.argv.slice(2);
const positional = argv.filter((a) => !a.startsWith('--'));
const edenJsPath = path.resolve(positional[0] || path.join(__dirname, '..', 'build-relwdiag', 'eden.js'));
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

const inMenu = () => global.Module._eden_menu_active() === 1;

function readTiming() {
    const ptr = global.Module._eden_debug_mesh_timing();
    let end = ptr;
    while (global.Module.HEAPU8[end] !== 0) end++;
    return JSON.parse(Buffer.from(global.Module.HEAPU8.slice(ptr, end)).toString('utf8'));
}

// Sample the worst inter-timer gap over `body()`, same technique as headless-load-timing.js —
// node's 1ms timer callback shares the thread with the fake-rAF loop, so a gap of N ms IS N ms of
// blocked main thread.
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
    // B3 Stage 2 needs a number for the thing B3 is actually FOR, and longest_block_ms is not it.
    // STATUS.md §3 item 2 characterised the remaining bulk-reload problem in real Safari as a
    // ~2 s SOFT DRAG — ~90 of ~148 frames at 20-30 ms — not a lockup; the worst frame was already
    // fixed by pass 70's frame-spreading. over_budget_ms is that drag as one figure: the total
    // wall-clock the main thread spent past the 60 fps budget during the burst, i.e. sum of
    // max(0, gap - 16.66). Halving it means the window fills in at 60 fps where it used to fill in
    // at 45, which is exactly the claim B3 has to make.
    const overBudget = gaps.reduce((a, g) => a + Math.max(0, g - 16.66), 0);
    return {
        label,
        longest_block_ms: gaps[0] || 0,
        blocks_over_16ms: gaps.filter((g) => g > 16.66).length,
        blocks_over_8ms: gaps.filter((g) => g > 8.33).length,
        over_budget_ms: +overBudget.toFixed(1),
    };
}

global.Module.postRun = global.Module.postRun || [];
global.Module.postRun.push(async () => {
    if (!(await waitUntil(inMenu, 15000, 'the menu to come up'))) {
        console.log('FATAL: never reached the menu');
        process.exit(1);
    }
    global.Module._eden_menu_create_world();
    global.Module._eden_menu_set_pending_world_type(0);
    global.Module._eden_menu_play();
    if (!(await waitUntil(() => !inMenu(), 60000, 'GAME_MODE_PLAY'))) {
        console.log('FATAL: never reached play');
        process.exit(1);
    }
    await new Promise((r) => setTimeout(r, 2000)); // let the initial-load churn settle

    // B6: the per-fread timing that produces the `ioMs` half of B1's split is OFF by default now.
    // It costs two emscripten_get_now() calls per -readDataOfLength:, and since B6 cut the reads
    // per column from 8 to 1 that is no longer the 16-crossings-per-column it was -- but it is
    // still measurement overhead sitting inside the thing being measured, so it stays opt-in.
    // Without --io-timing, ioMs/transportMs/cacheMs below read 0 and rleCpuMs collapses to readMs.
    const ioTiming = argv.includes('--io-timing');
    if (ioTiming && typeof global.Module._eden_debug_set_io_timing === 'function') {
        global.Module._eden_debug_set_io_timing(1);
    }

    const results = [];
    // Several teleports far enough apart that each one re-triggers a near-full-window reload
    // (Terrain.mm's `count>140` gate) — same coordinates used against the real-Safari run.
    const targets = [
        [64700, 40, 65700], [65100, 40, 65350], [64300, 40, 66000],
        [65500, 40, 65100], [63900, 40, 66300],
    ];
    // B1 (ROADMAP Phase B): the lazy Eden.eden node's cache/transport stats. No reset hook, so
    // snapshot-and-diff around each burst. fetchMs is the readRange() transport (fs.readSync
    // headless, sync XHR in-browser); requests/bytesFetched/blockMisses show whether a burst is
    // actually touching disk or replaying a warm cache.
    const fsStats = () => {
        const s = (global.Module.EdenWorldFS && global.Module.EdenWorldFS.stats) || {};
        return { requests: s.requests || 0, bytesFetched: s.bytesFetched || 0,
                 blockHits: s.blockHits || 0, blockMisses: s.blockMisses || 0,
                 evictions: s.evictions || 0, fetchMs: s.fetchMs || 0, fetchMsMax: s.fetchMsMax || 0 };
    };
    for (const [x, y, z] of targets) {
        global.Module._eden_debug_mesh_timing_reset();
        const fs0 = fsStats();
        let fillMs = 0;
        const r = await timedBlock(`teleport to (${x},${y},${z})`, async () => {
            const t0 = Date.now();
            global.Module._eden_console_teleport(x, y, z);
            // B3 Stage 2's honesty check. "Fewer over-budget frames" is a hollow win if the window
            // simply takes twice as long to fill — moving the work off the critical path is only
            // worth anything if the same work still LANDS in the same wall clock. Poll the mesh
            // counter and remember when it last moved: that is when the last of the burst's ~1296
            // chunks was meshed, i.e. when the frontier finished filling in.
            let lastCount = 0;
            const watch = setInterval(() => {
                const c = readTiming().meshCount;
                if (c > lastCount) { lastCount = c; fillMs = Date.now() - t0; }
            }, 10);
            // The bulk-reload gate needs two consecutive over-140 frames (Terrain.mm's
            // hit_load_counter) before it actually loads+meshes — give it real wall-clock frames
            // to reach that, then a little more for the mesh burst itself to finish.
            await new Promise((res) => setTimeout(res, 1500));
            clearInterval(watch);
        });
        r.fill_ms = fillMs;
        const timing = readTiming();
        const fs1 = fsStats();
        const worldfs = {
            requests: fs1.requests - fs0.requests,
            kbFetched: +((fs1.bytesFetched - fs0.bytesFetched) / 1024).toFixed(1),
            blockMisses: fs1.blockMisses - fs0.blockMisses,
            evictions: fs1.evictions - fs0.evictions,
            fetchMs: +(fs1.fetchMs - fs0.fetchMs).toFixed(3),
            fetchMsMax: +fs1.fetchMsMax.toFixed(3),
        };
        // readMs = NSFileHandle I/O (ioMs) + RLE-decode/transpose CPU. ioMs = transport (fetchMs)
        // + block-cache/coalesce overhead. Break it out so B1 has the layered number it asks for.
        // B3 Stage 3 split this. A column whose decode was deferred to a worker never enters
        // FileManager::readColumn, so `readMs` does not see it at all -- its main-thread half shows
        // up as `readRawMs` (the seek + fread of the raw RLE bytes) and its CPU half as `decodeMs`
        // on a worker. The figure to compare across builds is therefore mainThreadColumnMs, NOT
        // readMs: in a non-threaded build readRawMs and decodeMs are 0 and it reduces to the old
        // readMs. rleCpuMs keeps meaning "decode still being done on the main thread", which is
        // what should go to ~0 when Stage 3 is working.
        const mainThreadColumnMs = +(timing.readMs + (timing.readRawMs || 0)).toFixed(3);
        const rleCpuMs = +(timing.readMs - timing.ioMs).toFixed(3);
        const cacheMs = +(timing.ioMs - worldfs.fetchMs).toFixed(3);
        results.push({
            ...r, ...timing,
            meshMsPerChunk: timing.meshCount ? +(timing.meshMs / timing.meshCount).toFixed(4) : 0,
            mainThreadColumnMs,
            b1_breakdown: { readMs: +timing.readMs.toFixed(3), ioMs: +timing.ioMs.toFixed(3),
                            transportMs: worldfs.fetchMs, cacheMs, rleCpuMs, ...worldfs },
        });
    }

    console.log(JSON.stringify(results, null, 2));
    const worst = results.reduce((a, b) => (b.longest_block_ms > a.longest_block_ms ? b : a));
    console.log(`\nworst single main-thread block across ${results.length} teleport bursts: ` +
        `${worst.longest_block_ms} ms (${worst.meshCount} chunks meshed, ${worst.meshMs.toFixed(1)}ms of ` +
        `mesh CPU time, ${worst.uploadMs.toFixed(2)}ms of GL upload time) — build: ${edenJsPath}`);
    const tot = results.reduce((a, b) => ({
        readMs: a.readMs + b.readMs, ioMs: a.ioMs + b.ioMs,
        transportMs: a.transportMs + b.b1_breakdown.transportMs,
        rleCpuMs: a.rleCpuMs + b.b1_breakdown.rleCpuMs,
        readRawMs: a.readRawMs + (b.readRawMs || 0),
        decodeMs: a.decodeMs + (b.decodeMs || 0),
        mainThread: a.mainThread + b.mainThreadColumnMs,
        decodedCols: a.decodedCols + (b.decodedColumns || 0),
    }), { readMs: 0, ioMs: 0, transportMs: 0, rleCpuMs: 0, readRawMs: 0, decodeMs: 0,
          mainThread: 0, decodedCols: 0 });
    console.log(`column read ON THE MAIN THREAD, summed over ${results.length} bursts: ` +
        `${tot.mainThread.toFixed(1)} ms = readColumn ${tot.readMs.toFixed(1)} ` +
        `(fread I/O ${tot.ioMs.toFixed(1)}, of which transport ${tot.transportMs.toFixed(1)}; ` +
        `RLE decode/transpose still inline ${tot.rleCpuMs.toFixed(1)}) ` +
        `+ raw read for deferred columns ${tot.readRawMs.toFixed(1)}`);
    console.log(`column decode moved OFF the main thread: ${tot.decodeMs.toFixed(1)} ms of ` +
        `RLE decode/transpose on workers, over ${tot.decodedCols} columns`);
    process.exit(0);
});
