// Headless regression guard for audit row 11 / A5 — the paint-icon recolor pipeline
// (ManipulateImagePixelData + the storeImage bookkeeping in Texture2D_web.mm::initFromPath).
//
// WHY THIS EXISTS AS A TEST AND NOT AS AN EYEBALL: the failure this guards against is completely
// silent. Before the fix, ManipulateImagePixelData returned null, Resources::getPaintTex fed that
// null into `new Texture2D(cgimage, …)`, initFromImage's `if (image == NULL) return;` left the GL
// name at 0, and every draw of that texture bound "no texture" — no GL error, no console message,
// just an icon that isn't there. Exactly the shape of bug that comes back unnoticed.
//
// Usage: node tools/headless-recolor-test.js [path/to/eden.js]  (defaults to ../build-st/eden.js)
// Needs an EDEN_DIAGNOSTICS=ON tree (build-st) — eden_debug_recolor_state is compiled out of
// build-rel, the same as every other probe in src/seam/DebugState_web.mm.
//
// Methodology note (PORT-STATUS "Distilled hard-won knowledge" / web/CLAUDE.md "fast facts"):
// emscripten_set_main_loop is ALREADY running under node (fps=0 -> fakeRequestAnimationFrame, a
// real ~60 Hz setTimeout loop), so this script never calls _eden_debug_tick — mixing manual ticks
// on top of a live loop is the double-tick hazard that has produced spurious crashes before. It
// drives state and waits on wall-clock timers, same recipe as the other four harnesses.
'use strict';
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const positional = process.argv.slice(2).filter((a) => !a.startsWith('--'));
const edenJsPath = path.resolve(positional[0] || path.join(__dirname, '..', 'build-st', 'eden.js'));
const edenDir = path.dirname(edenJsPath);

global.require = require;
global.__dirname = edenDir;
global.__filename = edenJsPath;
global.Module = {
    print: () => {},          // the texture-decode diagnostic is ~120 lines of boot spam
    printErr: () => {},
};

const cwdBefore = process.cwd();
process.chdir(edenDir); // eden.js resolves eden.wasm/eden.data relative to cwd under node
vm.runInThisContext(fs.readFileSync(edenJsPath, 'utf8'), { filename: edenJsPath });
process.chdir(cwdBefore);

function utf8(ptr) {
    if (!ptr) return '';
    const heap = global.Module.HEAPU8;
    let end = ptr;
    while (heap[end] !== 0) end++;
    return Buffer.from(heap.buffer, heap.byteOffset + ptr, end - ptr).toString('utf8');
}

function waitUntil(predicate, timeoutMs, label) {
    return new Promise((resolve) => {
        const start = Date.now();
        const poll = () => {
            let ok = false;
            try { ok = predicate(); } catch (e) { /* not ready yet */ }
            if (ok) return resolve(true);
            if (Date.now() - start > timeoutMs) {
                console.log(`  (timed out waiting for: ${label})`);
                return resolve(false);
            }
            setTimeout(poll, 50);
        };
        poll();
    });
}

let failures = 0;
function check(name, cond) {
    if (cond) console.log('PASS:', name);
    else { console.log('FAIL:', name); failures++; }
}

function recolor(color) {
    return JSON.parse(utf8(global.Module._eden_debug_recolor_state(color)));
}

global.Module.postRun = global.Module.postRun || [];
global.Module.postRun.push(async () => {
    const haveProbe = typeof global.Module._eden_debug_recolor_state === 'function';
    check('eden_debug_recolor_state export present (EDEN_DIAGNOSTICS build)', haveProbe);
    if (!haveProbe) {
        console.log('FATAL: point this at build-st, not build-rel');
        process.exit(1);
    }

    // Resources is constructed during World's own construction, which main() has kicked off.
    const ready = await waitUntil(() => !recolor(1).error, 15000, 'Resources to exist');
    check('Resources singleton exists after main()', ready);
    if (!ready) { console.log('FATAL, aborting'); process.exit(1); }

    // --- The inputs. These four globals live in Resources.mm and are filled in by
    // Texture2D_web.mm::initFromPath's storeImage block. All four nil is the pre-fix state.
    const s = recolor(1);
    console.log('recolor state @ color 1:', JSON.stringify(s));
    check('storedPaint was captured during texture load', s.stored_paint === 1);
    check('storedPaintMask was captured during texture load', s.stored_paint_mask === 1);
    check('storedDoor was captured during texture load', s.stored_door === 1);
    check('storedDoorMask was captured during texture load', s.stored_door_mask === 1);

    // The shipped art is ipad~palette.png / ipad~paint_mask.png (there is no non-ipad
    // paint_mask.png at all, and IS_IPAD is pinned TRUE in this port), both 90x90. If the
    // classification block ever moves BELOW the ipad~ probe in initFromPath, the match on the
    // bare name fails and these come back -1 — which is the specific way this can silently break.
    check('paint image decoded to real pixels', s.paint_w > 0 && s.paint_h > 0);
    check('paint mask decoded to real pixels', s.mask_w > 0 && s.mask_h > 0);
    check('paint image and mask agree on size', s.paint_w === s.mask_w && s.paint_h === s.mask_h);

    // --- The output. A GL texture name of 0 is the bug: it means Texture2D never uploaded
    // anything, which is exactly what a null out of ManipulateImagePixelData produces.
    check('getPaintTex(1) produced a real GL texture (name != 0)', s.paint_tex > 0);
    check('getDoorTex(1) produced a real GL texture (name != 0)', s.door_tex > 0);

    // --- Distinct colours must produce distinct textures, not one cached result handed out for
    // every colour. paint_cache is a SINGLE slot keyed on colour, so asking for a second colour
    // must rebuild it; door_cache is per-colour, so both must be live at once.
    const a = recolor(2);
    const b = recolor(7);
    check('a second paint colour also produces a real texture', b.paint_tex > 0);
    check('door textures for two colours are distinct GL names',
        a.door_tex > 0 && b.door_tex > 0 && a.door_tex !== b.door_tex);

    // --- The creature half. storedSkins/storedMasks are filled POSITIONALLY by initFromPath (the
    // Nth texture load after Resources::loadResources zeroes each counter), which is the fragile
    // part: reorder a texture load in Resources.mm and creatures silently wear each other's skins.
    // 5 models x 2 states = 10 of each, and the skin counter must land on exactly 10 despite
    // 15 skin PNGs being loaded (every third — the Rage variant — is deliberately not stored).
    check('all 10 creature skins captured', s.skins_filled === 10);
    check('all 10 creature masks captured', s.masks_filled === 10);
    check('skin counter stopped at 10 (15 loaded, every 3rd skipped)', s.skin_counter === 10);
    check('mask counter stopped at 10', s.mask_counter === 10);

    // Re-asking for a colour whose door texture is already cached must return the SAME name
    // (getDoorTex's cache), while the single-slot paint cache legitimately may not.
    const a2 = recolor(2);
    check('door texture cache returns a stable name for the same colour', a2.door_tex === a.door_tex);

    console.log(failures === 0 ? 'ALL PASS' : `${failures} FAILURE(S)`);
    process.exit(failures === 0 ? 0 : 1);
});
