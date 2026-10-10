// headless-emod-store-test.js — Stage S / S.3: EdenWorldStore + the streaming .eden -> .emod
// converter, run inside the wasm build under node. Drives the EDEN_DIAGNOSTICS-only export
// eden_emod_selftest_web (Classes/EdenWorldStoreSelftest.cpp) — the SAME gates `eden_native
// --emod-selftest` runs on macOS/Linux/Windows:
//   - the checked-in fixture pack (web/tools/fixtures/emod/, written by `emod.py fixtures-pack`):
//     every fixture stream-converted to the exact bytes emod.py wrote (SHA-256 in expected.txt),
//     and emod.py's multi-batch / zlib / raw .emod files read to emod.py's state digest. Matching
//     the same hashes here as on the three desktop CI legs IS the cross-target byte gate (S.3
//     gate 4) for the wasm target;
//   - synthetic worlds, refused inputs, the writer, the damaged-record fallback, crash injection
//     at every kill point of a batch (strided in --quick), the corrupt gate (+ its CRC-off
//     control), and open time on a log.
// The pack is copied into MEMFS (/emod-fixtures) first; the work directory is MEMFS too.
//
// Only meaningful against an EDEN_DIAGNOSTICS=ON tree (build-st): the export is compiled out
// otherwise. build-st is -O0, so the default here is --quick (crash points strided by 23, a
// 3,000-column open-time log); pass --full for every byte.
//
// Usage: node tools/headless-emod-store-test.js [path/to/eden.js] [--full]
'use strict';
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const positional = process.argv.slice(2).filter((a) => !a.startsWith('--'));
const FULL = process.argv.includes('--full');
const edenJsPath = path.resolve(positional[0] || path.join(__dirname, '..', 'build-st', 'eden.js'));
const edenDir = path.dirname(edenJsPath);
const packDir = path.join(__dirname, 'fixtures', 'emod');

if (!fs.existsSync(edenJsPath)) {
    console.log('FAIL: no eden.js at', edenJsPath, '(build it first)');
    process.exit(1);
}

global.require = require;
global.__dirname = edenDir;
global.__filename = edenJsPath;
const lines = [];
global.Module = {
    print: (t) => { lines.push(t); if (/^\[eden-emod\]/.test(t)) console.log(t); },
    printErr: (t) => { if (/^\[eden-emod\]/.test(t)) console.log(t); },
};

const cwdBefore = process.cwd();
process.chdir(edenDir);
vm.runInThisContext(fs.readFileSync(edenJsPath, 'utf8'), { filename: edenJsPath });
process.chdir(cwdBefore);

global.Module.postRun = global.Module.postRun || [];
global.Module.postRun.push(() => {
    const M = global.Module;
    if (typeof M._eden_emod_selftest_web !== 'function') {
        console.log('FAIL: eden_emod_selftest_web not exported (needs an EDEN_DIAGNOSTICS build)');
        process.exit(1);
    }
    const FS = global.FS;
    try { FS.mkdir('/emod-fixtures'); } catch (e) { /* exists */ }
    let n = 0;
    for (const f of fs.readdirSync(packDir)) {
        FS.writeFile('/emod-fixtures/' + f, fs.readFileSync(path.join(packDir, f)));
        n++;
    }
    console.log(`[emod-test] ${n} pack files copied into MEMFS; running ${FULL ? 'full' : 'quick'} gates`);
    const t0 = Date.now();
    const rc = M._eden_emod_selftest_web(FULL ? 0 : 1);
    const fails = lines.filter((l) => /^\[eden-emod\] FAIL/.test(l)).length;
    const passes = lines.filter((l) => /^\[eden-emod\] PASS/.test(l)).length;
    console.log(`[emod-test] rc=${rc}, ${passes} PASS, ${fails} FAIL, ${((Date.now() - t0) / 1000).toFixed(1)} s`);
    console.log(rc === 0 && fails === 0 && passes > 0 ? 'ALL PASS' : 'FAILED');
    process.exit(rc === 0 && fails === 0 && passes > 0 ? 0 : 1);
});

setTimeout(() => {
    console.log('FAIL: timed out (module never finished booting, or the gates hung)');
    process.exit(1);
}, FULL ? 1800000 : 600000).unref();
