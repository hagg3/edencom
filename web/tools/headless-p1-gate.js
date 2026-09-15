// Checked-in version of the "headless logic check" recipe documented in web/CLAUDE.md and
// archive/PORT-STATUS-2026-08-13.md ("(cd build-st && node eden.js) — expect `[eden-gl] no canvas` then three
// `[eden-p1] tick N: World::update returned` lines; anything else is a regression"). Before this
// file, that check only existed as prose someone had to re-type from memory each session
// (perf-audit row #17 / §9's "no test harness in the repo" gap) — this makes it a runnable,
// asserting smoke test alongside headless-menu-flow-test.js and headless-lazy-world-test.js.
//
// Unlike those two, this does NOT use vm.runInThisContext + shared Module — it runs `node eden.js`
// exactly as a session would by hand, as a child process, and asserts against its stdout. That
// also means it exercises the real EDEN_DIAGNOSTICS-gated print path (row #12/Q7): this gate is
// only meaningful against a `build-st`-style tree (EDEN_DIAGNOSTICS=ON); pointed at `build-rel` it
// will correctly report the prints as absent, which is that build's intended behavior, not a bug.
//
// Usage: node tools/headless-p1-gate.js [path/to/build-dir]  (defaults to ../build-st)
'use strict';
const { spawn } = require('child_process');
const path = require('path');
const fs = require('fs');

const buildDir = path.resolve(process.argv[2] || path.join(__dirname, '..', 'build-st'));
const edenJsPath = path.join(buildDir, 'eden.js');

if (!fs.existsSync(edenJsPath)) {
    console.log('FAIL: no eden.js at', edenJsPath, '(build it first)');
    process.exit(1);
}

const NEEDED_TICKS = 3; // ticks 0, 1, 2 — matches the documented "three tick lines" gate
const TIMEOUT_MS = 20000;

const child = spawn('node', ['eden.js'], { cwd: buildDir });

let sawNoCanvas = false;
let ticksSeen = new Set();
let buf = '';
let finished = false;

function finish(ok, reason) {
    if (finished) return;
    finished = true;
    clearTimeout(timer);
    try { child.kill(); } catch (e) { /* already gone */ }
    if (ok) {
        console.log('PASS:', reason);
        console.log('ALL PASS');
        process.exit(0);
    } else {
        console.log('FAIL:', reason);
        console.log('--- captured output ---');
        console.log(buf);
        process.exit(1);
    }
}

function checkDone() {
    if (sawNoCanvas && ticksSeen.size >= NEEDED_TICKS) {
        finish(true, `saw headless-no-canvas line and ${ticksSeen.size} P1 tick(s) (0..${NEEDED_TICKS - 1})`);
    }
}

function onData(chunk) {
    buf += chunk;
    if (!sawNoCanvas && /\[eden-gl\] no canvas/.test(chunk)) sawNoCanvas = true;
    const re = /\[eden-p1\] tick (\d+): World::update returned/g;
    let m;
    while ((m = re.exec(chunk))) ticksSeen.add(Number(m[1]));
    checkDone();
}

child.stdout.on('data', onData);
child.stderr.on('data', onData);
child.on('exit', (code) => {
    if (!finished) finish(false, `process exited (code ${code}) before seeing the expected output`);
});

const timer = setTimeout(() => {
    finish(false, `timed out after ${TIMEOUT_MS}ms — saw no-canvas=${sawNoCanvas}, ticks=${[...ticksSeen].sort()}`);
}, TIMEOUT_MS);
