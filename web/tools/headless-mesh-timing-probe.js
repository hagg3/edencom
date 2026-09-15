// Measures mesh-CPU cost vs VBO-upload cost per chunk during real streaming — the standing
// evidence for the design question WORKING/c1-threaded-build-handoff.md's §5 item 1 asks before
// any off-thread-meshing work starts: "nothing yet establishes what fraction of a frame meshing
// costs, or whether the bottleneck is meshing (CPU) or VBO upload (main-thread GL, which
// *cannot* move). Build the profile before committing to the design."
//
// WHY THIS IS A CHECKED-IN TOOL AND NOT A ONE-OFF: same reasoning as headless-load-timing.js and
// headless-memory-probe.js — a number this load-bearing for an XL-effort architecture decision
// should never be re-derived from a sentence in a doc.
//
// WHAT IT DOES: creates a world, then walks forward at a high speed multiplier for real wall-clock
// seconds (matching pass 47's "real 16-second flight across streaming boundaries" methodology —
// node's fake rAF loop is a genuine ~60 Hz setTimeout, so wall-clock time is real engine time), which
// crosses the toroidal window's streaming boundary repeatedly and forces sustained
// Terrain::prepareAndLoadGeometry / updateAllImportantChunks churn — precisely the condition an
// off-thread mesher would exist to smooth over. src/seam/MeshTiming_web.mm's --wrap of
// TerrainChunk::rebuild2()/prepareVBO() accumulates the CPU-mesh and GL-upload time separately;
// this script resets the counters after the initial load settles, then reads them back.
//
// Usage: node tools/headless-mesh-timing-probe.js [path/to/eden.js] [--seconds=20] [--speed=8]
//        (defaults to ../build-rel — the build players actually run; the export this tool reads
//        is deliberately NOT gated behind EDEN_DIAGNOSTICS, see MeshTiming_web.mm's header)
'use strict';
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const args = process.argv.slice(2);
const positional = args.filter((a) => !a.startsWith('--'));
const flag = (name, def) => {
    const hit = args.find((a) => a.startsWith(`--${name}=`));
    return hit ? Number(hit.split('=')[1]) : def;
};
const seconds = flag('seconds', 20);
const speed = flag('speed', 8);

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

const inMenu = () => global.Module._eden_menu_active() === 1;

function readTiming() {
    // Static C buffer — copy out immediately, same rule as every other utf8(ptr) reader in this
    // project (web/CLAUDE.md: never Module.UTF8ToString, walk HEAPU8 to the NUL ourselves).
    const ptr = global.Module._eden_debug_mesh_timing();
    let end = ptr;
    while (global.Module.HEAPU8[end] !== 0) end++;
    const bytes = global.Module.HEAPU8.slice(ptr, end);
    return JSON.parse(Buffer.from(bytes).toString('utf8'));
}

global.Module.postRun = global.Module.postRun || [];
global.Module.postRun.push(async () => {
    if (!(await waitUntil(inMenu, 15000, 'the menu to come up'))) {
        console.log('FATAL: never reached the menu');
        process.exit(1);
    }

    global.Module._eden_menu_create_world();
    global.Module._eden_menu_set_pending_world_type(0); // 0 = normal, streams the bundled world
    global.Module._eden_menu_play();
    if (!(await waitUntil(() => !inMenu(), 60000, 'GAME_MODE_PLAY'))) {
        console.log('FATAL: never reached play');
        process.exit(1);
    }

    // Let the initial-load churn (whatever prepareAndLoadGeometry does to fill the window around
    // spawn) settle and drain before the timed window starts — that is a one-time cost this probe
    // is not trying to measure (headless-load-timing.js already owns that number).
    await new Promise((r) => setTimeout(r, 2000));
    global.Module._eden_debug_mesh_timing_reset();

    console.log(`Walking forward at speed=${speed} for ${seconds}s real time (build: ${edenJsPath})...`);
    global.Module._eden_set_move_input(1, 0, speed);
    const resample = setInterval(() => {
        try { global.Module._eden_set_move_input(1, 0, speed); } catch (e) { /* ignore */ }
    }, 200); // re-assert in case setSpeed's effect decays without a fresh call — cheap to be safe

    const t0 = Date.now();
    await new Promise((r) => setTimeout(r, seconds * 1000));
    clearInterval(resample);
    global.Module._eden_set_move_input(0, 0, 0);
    const wallMs = Date.now() - t0;

    const timing = readTiming();
    const frameCount = Math.round(wallMs / (1000 / 60)); // nominal 60 Hz fake-rAF budget

    const report = {
        wall_ms: wallMs,
        nominal_frame_budget_ms: 1000 / 60,
        mesh: {
            calls: timing.meshCount,
            total_ms: Number(timing.meshMs.toFixed(2)),
            avg_ms_per_chunk: timing.meshCount ? Number((timing.meshMs / timing.meshCount).toFixed(4)) : 0,
            max_ms_single_chunk: timing.meshMsMax,
            ms_per_second_wall: Number((timing.meshMs / (wallMs / 1000)).toFixed(2)),
        },
        upload: {
            calls: timing.uploadCount,
            total_ms: Number(timing.uploadMs.toFixed(2)),
            avg_ms_per_chunk: timing.uploadCount ? Number((timing.uploadMs / timing.uploadCount).toFixed(4)) : 0,
            max_ms_single_chunk: timing.uploadMsMax,
            ms_per_second_wall: Number((timing.uploadMs / (wallMs / 1000)).toFixed(2)),
        },
    };
    console.log(JSON.stringify(report, null, 2));

    const meshShare = timing.meshMs / (timing.meshMs + timing.uploadMs || 1);
    console.log(`\nmesh is ${(meshShare * 100).toFixed(1)}% of (mesh+upload) time; ` +
        `upload is ${((1 - meshShare) * 100).toFixed(1)}%.`);
    console.log(`mesh cost averages ${report.mesh.ms_per_second_wall} ms of CPU work per real second ` +
        `(${(report.mesh.ms_per_second_wall / (1000 / 60) * 100 / 60).toFixed(2)}% of one 60fps frame ` +
        `budget's worth, amortised across the whole window) — upload averages ` +
        `${report.upload.ms_per_second_wall} ms/s the same way. Peak single-chunk cost ` +
        `(mesh ${report.mesh.max_ms_single_chunk.toFixed(2)}ms / upload ${report.upload.max_ms_single_chunk.toFixed(2)}ms) ` +
        `is what a bad frame actually pays if several land in the same tick.`);
    process.exit(0);
});
