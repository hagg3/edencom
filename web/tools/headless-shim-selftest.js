// headless-shim-selftest.js — audit row 21/I6: "nothing covers the GL shim's state tracking, the
// ObjC runtime's dispatch... where a regression is both likely and silent." Drives the two
// EDEN_DIAGNOSTICS-only self-test exports added for this row:
//   - Module._eden_gl_selftest_run()   (src/shim/gl/gl_es1_shim.cpp) — the GUARDED/no-context
//     object-name bookkeeping every headless boot already depends on (Graphics::initGraphics()
//     issues real glGenBuffers/glBindBuffer/glBufferData during World::World() with no canvas to
//     receive them). The real draw-path dirty-caches (GROUP 2's setup-call elision) only run
//     against a live WebGL context and are NOT covered here — see that function's own header for
//     why headless-gl was rejected project-wide; that half needs a live-browser pass.
//   - Module._eden_objc_selftest_run() (src/shim/objc/objc_selftest.mm) — real @interface/
//     @implementation test classes exercising slot-based dispatch, superclass-relative ivar
//     layout, `super`, and category merging through the actual runtime.
//
// Neither export depends on World/Menu state, so both run from Module.postRun rather than
// waiting on menuState() the way headless-save-roundtrip-test.js does.
//
// Only meaningful against an EDEN_DIAGNOSTICS=ON tree (build-st) — both exports are compiled out
// otherwise, same caveat as headless-p1-gate.js's [eden-p1] prints (row #12/Q7).
//
// Usage: node tools/headless-shim-selftest.js [path/to/eden.js]  (defaults to ../build-st/eden.js)
'use strict';
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const positional = process.argv.slice(2).filter((a) => !a.startsWith('--'));
const edenJsPath = path.resolve(positional[0] || path.join(__dirname, '..', 'build-st', 'eden.js'));
const edenDir = path.dirname(edenJsPath);

if (!fs.existsSync(edenJsPath)) {
    console.log('FAIL: no eden.js at', edenJsPath, '(build it first)');
    process.exit(1);
}

global.require = require;
global.__dirname = edenDir;
global.__filename = edenJsPath;
global.Module = {
    print: (t) => console.log('[out]', t),
    printErr: (t) => console.log('[err]', t),
};

const cwdBefore = process.cwd();
process.chdir(edenDir);
const src = fs.readFileSync(edenJsPath, 'utf8');
vm.runInThisContext(src, { filename: edenJsPath });
process.chdir(cwdBefore);

let failures = 0;
function check(name, cond) {
    if (cond) { console.log('PASS:', name); }
    else { console.log('FAIL:', name); failures++; }
}

global.Module.postRun = global.Module.postRun || [];
global.Module.postRun.push(() => {
    if (typeof global.Module._eden_gl_selftest_run !== 'function') {
        check('eden_gl_selftest_run exported (EDEN_DIAGNOSTICS build)', false);
    } else {
        check('GL shim guarded-mode self-test', global.Module._eden_gl_selftest_run() === 1);
    }

    if (typeof global.Module._eden_objc_selftest_run !== 'function') {
        check('eden_objc_selftest_run exported (EDEN_DIAGNOSTICS build)', false);
    } else {
        check('ObjC runtime dispatch self-test', global.Module._eden_objc_selftest_run() === 1);
    }

    console.log(failures === 0 ? 'ALL PASS' : `${failures} FAILURE(S)`);
    process.exit(failures === 0 ? 0 : 1);
});

setTimeout(() => {
    console.log('FAIL: timed out waiting for postRun (module never finished booting)');
    process.exit(1);
}, 20000).unref();
