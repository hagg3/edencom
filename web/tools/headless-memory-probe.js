// Measures what the wasm module ACTUALLY needs of the two hand-picked memory numbers in
// CMakeLists.txt — `-sSTACK_SIZE` and `-sINITIAL_MEMORY` — which is project-audit row 14 / E1
// ("96 MB initial ... and an 8 MB stack that CMakeLists.txt admits is unmeasured").
//
// WHY THIS IS A CHECKED-IN TOOL AND NOT A ONE-OFF: both numbers were picked the only way they
// could be at the time — the initial heap from a linker error message, the stack from "the iOS
// main thread had multi-MB, and 64 KB traps in Terrain::render". Neither was ever measured, and
// on mobile both are paid up front, in the one resource that gets a tab killed. Any future change
// to the resident window, the mesher's vertex buckets, or Terrain::render's ~380 KB local array
// moves these numbers, so the measurement has to be repeatable rather than a sentence in a doc.
//
// Usage: node tools/headless-memory-probe.js [path/to/eden.js] [--quiet]
//        (defaults to ../build-st/eden.js — see "WHICH BUILD" below)
//
// HOW THE STACK MEASUREMENT WORKS (stack painting). The wasm stack is an ordinary region of the
// single linear memory, [emscripten_stack_get_end(), emscripten_stack_get_base()), growing DOWN
// from base. In this build `end` is 0 — emsdk 3.1.x puts the stack at the BOTTOM of memory, below
// the static data, precisely so an overflow runs off into address 0 and traps instead of quietly
// eating globals. At `postRun` the C stack is unwound (main() has returned; the frame loop runs
// from timer callbacks), so essentially the whole region is dead. We fill it with a marker byte,
// run a full session — world create + load, several seconds of the real frame loop, quit, reload
// from the save file — then scan up from the low end for the first byte that is no longer the
// marker. That address is the deepest the stack pointer ever reached, so `base - address` is the
// peak stack usage, exactly. The first 12 bytes are left alone: address 0 holds the 'emsc' heap
// magic and the two words at 4/8 are the ASSERTIONS stack cookie (`writeStackCookie` in eden.js).
// Overwriting either aborts the run with a bogus "Stack overflow!".
//
// HOW THE HEAP MEASUREMENT WORKS. With -sALLOW_MEMORY_GROWTH the module starts at INITIAL_MEMORY
// and grows on demand, so `wasmMemory.buffer.byteLength` sampled over a session is the real total
// footprint — but only if INITIAL_MEMORY is small enough for growth to actually happen. Against
// the shipped 96 MB build the peak simply reads back as 96 MB and tells you nothing; that is the
// expected, honest answer, and the script says so. To get the real number, relink with a small
// initial heap and re-run; the linker itself prints the floor it will accept ("initial memory too
// small, N bytes needed", N = the top of static data). `peak_stack_bytes` is trustworthy either way.
//
// WHICH BUILD: the stack accessors (emscripten_stack_get_*) exist in build-st's JS glue but are
// not emitted in build-rel, so the stack half only runs against an ASSERTIONS/diagnostics build.
// Stack depth is a property of the C++ call graph, not of -O2, so build-st's peak is the number
// that matters — but -O0 frames are FATTER than -O2 frames (no inlining, every temporary spilled),
// so treat it as an upper bound and keep headroom rather than trimming to it exactly.
'use strict';
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const args = process.argv.slice(2);
const quiet = args.includes('--quiet');
const positional = args.filter((a) => !a.startsWith('--'));
const edenJsPath = path.resolve(positional[0] || path.join(__dirname, '..', 'build-st', 'eden.js'));
const edenDir = path.dirname(edenJsPath);

const PAINT = 0xa5;

// Intercept the module's own heap-growth requests BEFORE it instantiates. This is the only way to
// read the REAL demand: `wasmMemory.buffer.byteLength` reports the size after emscripten's
// geometric overshoot (growMemory asks for max(requested, old * 1.2)), so a naive reading of the
// buffer just walks a x1.2 ladder — 96 -> 115.25 -> 138.31 -> 166 MB — and every rung is an
// artefact of the previous rung, not a measurement. `emscripten_resize_heap(requestedSize)` is
// what sbrk actually asked for, in bytes, and it is a JS import the wasm calls; wrapping the
// imports object at instantiate time is what gets us at it (overwriting the top-level `var`
// afterwards would be too late — wasmImports has already captured the function reference).
const heapRequests = [];
for (const fn of ['instantiate', 'instantiateStreaming']) {
    if (typeof WebAssembly[fn] !== 'function') continue;
    const original = WebAssembly[fn].bind(WebAssembly);
    WebAssembly[fn] = (src, imports) => {
        const env = imports && imports.env;
        if (env && typeof env.emscripten_resize_heap === 'function') {
            const inner = env.emscripten_resize_heap;
            env.emscripten_resize_heap = (requestedSize) => {
                heapRequests.push(requestedSize >>> 0);
                return inner(requestedSize);
            };
        }
        return original(src, imports);
    };
}

global.require = require;
global.__dirname = edenDir;
global.__filename = edenJsPath;
global.Module = { print: () => {}, printErr: () => {} };

const cwdBefore = process.cwd();
process.chdir(edenDir);
vm.runInThisContext(fs.readFileSync(edenJsPath, 'utf8'), { filename: edenJsPath });
process.chdir(cwdBefore);

const M = () => global.Module;
const log = (...a) => { if (!quiet) console.log(...a); };
const mb = (n) => (n / 1048576).toFixed(2) + ' MB';

function waitUntil(predicate, timeoutMs, label) {
    return new Promise((resolve) => {
        const start = Date.now();
        const poll = () => {
            let ok = false;
            try { ok = predicate(); } catch (e) { /* not ready */ }
            if (ok) return resolve(true);
            if (Date.now() - start > timeoutMs) {
                log(`  (timed out waiting for: ${label})`);
                return resolve(false);
            }
            setTimeout(poll, 5);
        };
        poll();
    });
}

const inMenu = () => M()._eden_menu_active() === 1;

async function quitToMenu() {
    // Same route the pause menu takes (which=0 opens the in-game menu, which=6 is "exit"); rexit
    // only lands while hud->inmenu. Matches headless-menu-flow-test.js / headless-load-timing.js.
    for (const which of [0, 6]) {
        M()._eden_tap_hud_button_begin(which);
        await new Promise((r) => setTimeout(r, 100));
        M()._eden_tap_hud_button_end(which);
        await new Promise((r) => setTimeout(r, 100));
    }
    return waitUntil(inMenu, 20000, 'back in the menu');
}

// The stack accessors are plain top-level `var`s in eden.js, NOT Module properties (and
// `Module.wasmExports` is itself gated behind EXPORTED_RUNTIME_METHODS — touching it aborts).
// Under vm.runInThisContext a top-level `var` lands on globalThis, which is how we reach them.
const stackFn = (name) => (typeof global[name] === 'function' ? global[name] : null);

function stackAccessorsAvailable() {
    try {
        const f = stackFn('_emscripten_stack_get_base');
        return !!f && f() > 0;
    } catch (e) {
        return false;
    }
}

// Bytes [0,4) are the address-zero heap magic and [4,12) the stack cookie — see writeStackCookie
// in eden.js. Skipping them costs nothing and keeps checkStackCookie from aborting the run.
const RESERVED_LOW = 12;

function paintStack(end, current) {
    // Stay a page clear of wherever the stack pointer happens to be right now so we never
    // scribble on a live frame.
    const from = end + RESERVED_LOW;
    const to = current - 4096;
    if (to <= from) throw new Error('stack region too small to paint');
    M().HEAPU8.fill(PAINT, from, to);
    return { from, to };
}

function scanHighWater(base, end) {
    const heap = M().HEAPU8;
    for (let a = end + RESERVED_LOW; a < base; a++) {
        if (heap[a] !== PAINT) return a;
    }
    return base; // nothing below the base was ever touched (impossible in practice)
}

global.Module.postRun = global.Module.postRun || [];
global.Module.postRun.push(async () => {
    const out = { eden_js: path.relative(path.join(__dirname, '..'), edenJsPath) };

    if (!(await waitUntil(inMenu, 20000, 'the menu to come up'))) {
        console.log('FATAL: never reached the menu');
        process.exit(1);
    }

    const haveStack = stackAccessorsAvailable();
    let base = 0, end = 0;
    if (haveStack) {
        base = stackFn('_emscripten_stack_get_base')();
        end = stackFn('_emscripten_stack_get_end')();
        // Layout here is [stack | static data | malloc heap] — see the header note; `end` is 0.
        out.stack_region = { end, base };
        out.stack_region_bytes = base - end;
    } else {
        log('note: emscripten_stack_get_* not exported by this build — skipping the stack half');
    }

    // Measured per PHASE rather than once for the session, because "which phase is deepest" is
    // the part that survives a change to the harness: menu-only is the shallow control, in-world
    // play is the frame loop (Terrain::render's ~380 KB objVertices local + the mesher), and the
    // reload leg is the save-file column read path. Repainting between phases is safe: we always
    // run from a timer callback with the C stack unwound, never nested inside a frame.
    const phases = {};
    const beginPhase = () => {
        if (haveStack) paintStack(end, stackFn('_emscripten_stack_get_current')());
    };
    const endPhase = (name) => {
        if (!haveStack) return;
        phases[name] = base - scanHighWater(base, end);
    };

    // Sample total linear memory throughout. With ALLOW_MEMORY_GROWTH this only ever rises.
    let peakBuffer = M().HEAP8.buffer.byteLength;
    const initialBuffer = peakBuffer;
    let growthEvents = 0;
    const sampler = setInterval(() => {
        const n = M().HEAP8.buffer.byteLength;
        if (n > peakBuffer) { peakBuffer = n; growthEvents++; }
    }, 4);

    // A full session: sit in the menu (control), generate a world from the bundled Eden.eden, play
    // it for a few seconds of real frame loop (this is what reaches Terrain::render and the mesher
    // — the deep frames), quit, then load the same world back from its save file (the other deep
    // path: column reads). Nothing here renders — there is no canvas under node — but every C++
    // frame above the GL shim still runs, and stack depth is a property of that call graph.
    beginPhase();
    await new Promise((r) => setTimeout(r, 1500));
    endPhase('menu_idle');

    beginPhase();
    const idx = M()._eden_menu_create_world();
    M()._eden_menu_set_pending_world_type(0); // normal — streams the bundled world
    M()._eden_menu_play();
    if (!(await waitUntil(() => !inMenu(), 60000, 'GAME_MODE_PLAY'))) {
        console.log('FATAL: first world load never reached play');
        process.exit(1);
    }
    endPhase('world_generate_and_load');

    beginPhase();
    await new Promise((r) => setTimeout(r, 4000)); // ~240 fake-rAF frames of the real frame loop
    endPhase('in_world_frame_loop');

    beginPhase();
    if (await quitToMenu()) {
        await new Promise((r) => setTimeout(r, 500));
        M()._eden_menu_select(idx >= 0 ? idx : 0);
        M()._eden_menu_play();
        if (await waitUntil(() => !inMenu(), 60000, 'GAME_MODE_PLAY (reload)')) {
            await new Promise((r) => setTimeout(r, 3000));
        }
        out.reloaded_from_save = true;
    } else {
        out.reloaded_from_save = false;
    }
    endPhase('save_quit_reload');
    clearInterval(sampler);

    if (haveStack) {
        out.peak_stack_bytes_by_phase = phases;
        const peak = Math.max(...Object.values(phases));
        out.peak_stack_bytes = peak;
        out.peak_stack_pct_of_region = +((100 * peak) / (base - end)).toFixed(1);
    }
    out.initial_memory_bytes = initialBuffer;
    out.peak_memory_bytes = peakBuffer;
    out.memory_growth_events = growthEvents;
    // The number that actually answers "what should -sINITIAL_MEMORY be": the largest size the
    // allocator ever asked for. Anything at or above it means the module never grows at all.
    out.peak_heap_demand_bytes = heapRequests.length ? Math.max(...heapRequests) : 0;
    out.heap_growth_requests = heapRequests.map((n) => +(n / 1048576).toFixed(2));

    console.log(JSON.stringify(out, null, 2));
    if (!quiet) {
        if (out.peak_stack_bytes !== undefined) {
            console.log(`\nstack: peak ${mb(out.peak_stack_bytes)} of ${mb(out.stack_region_bytes)} reserved` +
                ` (${out.peak_stack_pct_of_region}%)`);
        }
        console.log(`memory: buffer went ${mb(initialBuffer)} -> ${mb(peakBuffer)}` +
            ` (${growthEvents} growth event(s) seen after postRun)`);
        console.log(out.peak_heap_demand_bytes
            ? `real peak demand: ${mb(out.peak_heap_demand_bytes)} — an -sINITIAL_MEMORY at or above`
              + ` that never grows; the current build starts at ${mb(initialBuffer)}`
            : 'real peak demand: the module never asked to grow — -sINITIAL_MEMORY already covers'
              + ' this session');
    }
    process.exit(0);
});
