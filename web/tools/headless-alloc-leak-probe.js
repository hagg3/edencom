// headless-alloc-leak-probe.js — ROADMAP Phase M / M6: the ~22 MB-per-world-load leak.
//
// tools/headless-heap-ceiling-probe.js established the symptom (sbrkTop climbs ~22 MB per world
// load, at both world heights, with no plateau) but reports only sbrkTop, which cannot tell a leak
// from fragmentation — emmalloc never returns memory to the system, so both look identical from
// the heap top. This probe is the bisect harness the ROADMAP row asks for:
//
//   * it runs the cheapest possible reproducer — create ONE 64z world, then load/quit it N times,
//     no teleport bursts — so a cycle costs ~4 s instead of ~25 s;
//   * at every menu boundary it reads `eden_debug_alloc()` (emmalloc's live-bytes statistic,
//     src/seam/HeapProbe_web.mm) alongside sbrkTop. `live` flat + sbrkTop climbing = fragmentation;
//     both climbing = a real leak;
//   * with EDEN_ALLOC_TRACE=ON in the build it also dumps the per-size-class live-allocation
//     histogram (`eden_debug_alloc_histogram()`), and diffs cycle N against cycle N-1 — which size
//     class grows by ~22 MB is the actual bisect.
//
// Usage (needs a diagnostics build for _eden_console_teleport-free driving; build-relwdiag works):
//   node tools/headless-alloc-leak-probe.js [path/to/eden.js] [--cycles=N] [--burst]
'use strict';
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const argv = process.argv.slice(2);
const positional = argv.filter((a) => !a.startsWith('--'));
const cyclesArg = argv.find((a) => a.startsWith('--cycles='));
const CYCLES = cyclesArg ? Math.max(1, parseInt(cyclesArg.split('=')[1], 10)) : 6;
const WITH_BURST = argv.includes('--burst');
const stacksArg = argv.find((a) => a.startsWith('--stacks='));
// Capture a callstack for every allocation >= this many bytes (0 = off). Costs a JS stack capture
// per qualifying allocation, so raise it if a run gets slow.
const STACK_MIN = stacksArg ? parseInt(stacksArg.split('=')[1], 10) : 4096;
const topArg = argv.find((a) => a.startsWith('--top='));
const STACK_TOP = topArg ? parseInt(topArg.split('=')[1], 10) : 12;
const minArg = argv.find((a) => a.startsWith('--min-delta='));
// Don't report a call site unless it grew by at least this many bytes in the cycle.
const REPORT_MIN = minArg ? parseInt(minArg.split('=')[1], 10) : 32 * 1024;
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

function utf8(ptr) {
    if (!ptr) return '';
    const heap = global.Module.HEAPU8;
    let end = ptr;
    while (heap[end] !== 0) end++;
    return Buffer.from(heap.buffer, heap.byteOffset + ptr, end - ptr).toString('utf8');
}
const MB = (n) => +(n / (1024 * 1024)).toFixed(2);
const menuState = () => JSON.parse(utf8(global.Module._eden_debug_menu_state()));
const inMenu = () => global.Module._eden_menu_active() === 1;
const heap = () => JSON.parse(utf8(global.Module._eden_debug_heap()));
const alloc = () => JSON.parse(utf8(global.Module._eden_debug_alloc()));

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
const TARGETS = [
    [64700, 40, 65700], [65100, 40, 65350], [64300, 40, 66000],
];
async function burstRun(dwellMs) {
    for (const [x, y, z] of TARGETS) {
        global.Module._eden_console_teleport(x, y, z);
        await new Promise((r) => setTimeout(r, dwellMs));
    }
}

function mark(label) {
    const h = heap();
    const a = alloc();
    console.log(`  ${label.padEnd(26)} sbrkTop ${String(MB(h.sbrkTop)).padStart(7)} MB   live ${String(MB(a.live)).padStart(7)} MB   free ${String(MB(a.freeDyn)).padStart(7)} MB`);
    return { label, sbrkTop: h.sbrkTop, live: a.live, freeDyn: a.freeDyn };
}
const hasTrace = () => typeof global.Module._eden_debug_alloc_histogram === 'function';
const histogram = () => (hasTrace() ? JSON.parse(utf8(global.Module._eden_debug_alloc_histogram())) : null);
const phases = () => (hasTrace() ? JSON.parse(utf8(global.Module._eden_debug_alloc_phases())) : null);
function stacks() {
    if (!hasTrace()) return [];
    const n = global.Module._eden_debug_alloc_stack_count();
    const out = [];
    for (let i = 0; i < n; i++) {
        out.push({
            bytes: global.Module._eden_debug_alloc_stack_bytes(i) >>> 0,
            live: global.Module._eden_debug_alloc_stack_live(i) >>> 0,
            text: utf8(global.Module._eden_debug_alloc_stack_text(i)),
        });
    }
    return out;
}
function setPhase(name) {
    if (!hasTrace()) return;
    const buf = global.Module._malloc(64);
    global.Module.stringToUTF8(name, buf, 64);
    global.Module._eden_alloc_trace_phase(buf);
    global.Module._free(buf);
}

let prevBuckets = null;
function reportBuckets(label) {
    const h = histogram();
    if (!h) return;
    if (prevBuckets) {
        const rows = [];
        for (const k of Object.keys(h.buckets)) {
            const dB = h.buckets[k].bytes - ((prevBuckets[k] && prevBuckets[k].bytes) || 0);
            const dN = h.buckets[k].count - ((prevBuckets[k] && prevBuckets[k].count) || 0);
            if (Math.abs(dB) > 64 * 1024) rows.push({ k, dMB: MB(dB), dN });
        }
        rows.sort((x, y) => y.dMB - x.dMB);
        if (rows.length) {
            console.log(`    size-class delta vs previous (${label}):  ` +
                rows.map((r) => `${r.k}B ${r.dMB >= 0 ? '+' : ''}${r.dMB}MB/${r.dN >= 0 ? '+' : ''}${r.dN}`).join('   '));
        }
    }
    prevBuckets = h.buckets;
}

let prevStacks = null;
function firstEngineFrame(text) {
    // The captured stack is newest-first and always starts with the tracer + emmalloc frames;
    // report the deepest few frames below them, which is the actual allocation site.
    const lines = text.split('\n').map((l) => l.trim()).filter(Boolean)
        .filter((l) => !/getCallstack|emscripten_get_callstack|capture_stack|note_alloc|__wrap|\bmalloc\b|\bcalloc\b|\brealloc\b|operator new|_Znw|_Zna/.test(l));
    return lines.slice(0, 4).join('  <-  ');
}
function reportStacks(label, limit) {
    const cur = stacks();
    if (!cur.length) return;
    const rows = cur.map((s, i) => ({
        i, text: s.text, bytes: s.bytes, live: s.live,
        dBytes: prevStacks && prevStacks[i] ? s.bytes - prevStacks[i].bytes : s.bytes,
        dLive: prevStacks && prevStacks[i] ? s.live - prevStacks[i].live : s.live,
    })).filter((r) => r.dBytes > REPORT_MIN);
    rows.sort((a, b) => b.dBytes - a.dBytes);
    if (rows.length) {
        console.log(`    leaked-per-callsite delta (${label}), top ${Math.min(limit, rows.length)}:`);
        for (const r of rows.slice(0, limit)) {
            console.log(`      +${String(MB(r.dBytes)).padStart(6)} MB  +${String(r.dLive).padStart(4)} allocs   ${firstEngineFrame(r.text)}`);
        }
    }
    prevStacks = cur;
}

global.Module.postRun = global.Module.postRun || [];
global.Module.postRun.push(async () => {
    if (!(await waitUntil(inMenu, 30000, 'the menu to come up'))) {
        console.log('FATAL: never reached the menu');
        process.exit(1);
    }
    console.log(`build: ${edenJsPath}`);
    console.log(`cycles: ${CYCLES}   bursts: ${WITH_BURST ? 'yes' : 'no'}   alloc trace: ${hasTrace() ? `yes (stacks >= ${STACK_MIN} B)` : 'no (build without EDEN_ALLOC_TRACE)'}\n`);
    mark('menu, pre-world');

    // one 64z world, created and saved once — every later cycle just re-loads it
    const idx = global.Module._eden_menu_create_world();
    const name = utf8(global.Module._eden_menu_world_name(idx));
    global.Module._eden_menu_clear_pending_world_type();
    if (global.Module._eden_menu_play() !== 1) { console.log('FATAL: play() rejected'); process.exit(1); }
    if (!(await waitUntil(() => !inMenu(), 90000, 'GAME_MODE_PLAY'))) { console.log('FATAL: never reached play'); process.exit(1); }
    await new Promise((r) => setTimeout(r, 1500));
    await saveInPlace();
    if (!(await quitToMenu())) { console.log('FATAL: did not return to menu'); process.exit(1); }
    if (hasTrace() && STACK_MIN > 0) global.Module._eden_alloc_trace_stacks(STACK_MIN);
    const base = mark('after create+save+quit');
    reportBuckets('baseline');
    reportStacks('baseline', 1);

    const rows = [];
    let prev = base;
    for (let c = 1; c <= CYCLES; c++) {
        if (!(await playWorldNamed(name))) { console.log('FATAL: world did not reach PLAY'); process.exit(1); }
        await new Promise((r) => setTimeout(r, 1200));
        if (WITH_BURST) await burstRun(1200);
        if (!(await quitToMenu())) { console.log('FATAL: did not return to menu'); process.exit(1); }
        await new Promise((r) => setTimeout(r, 200));
        const m = mark(`cycle ${c}: back at menu`);
        reportBuckets(`cycle ${c}`);
        reportStacks(`cycle ${c}`, STACK_TOP);
        rows.push({ cycle: c, sbrkTop_MB: MB(m.sbrkTop), live_MB: MB(m.live),
            dSbrk_MB: MB(m.sbrkTop - prev.sbrkTop), dLive_MB: MB(m.live - prev.live) });
        prev = m;
    }

    console.log('\ncycle   sbrkTop      dSbrk      live      dLive');
    for (const r of rows) {
        console.log(`  ${String(r.cycle).padStart(2)}   ${String(r.sbrkTop_MB).padStart(8)}   ${String(r.dSbrk_MB).padStart(8)}   ${String(r.live_MB).padStart(8)}   ${String(r.dLive_MB).padStart(8)}`);
    }
    const dS = rows.slice(1).reduce((a, r) => a + r.dSbrk_MB, 0) / Math.max(1, rows.length - 1);
    const dL = rows.slice(1).reduce((a, r) => a + r.dLive_MB, 0) / Math.max(1, rows.length - 1);
    console.log(`\n[M6] mean per-load growth after the first cycle: sbrkTop ${dS.toFixed(2)} MB   live ${dL.toFixed(2)} MB`);
    const ph = phases();
    if (ph) console.log(`[M6] live bytes by phase tag: ${Object.entries(ph).map(([k, v]) => `${k}=${MB(v.bytes)}MB/${v.count}`).join('  ')}`);
    console.log(dL > 4
        ? '[M6] live allocations grow per load — this is a LEAK, not fragmentation.'
        : '[M6] live allocations are flat — the growth is fragmentation/high-water, not leaked objects.');
    process.exit(0);
});
