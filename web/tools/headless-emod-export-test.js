// headless-emod-export-test.js — Stage S / S.5 gate (5) + the web import path, in the wasm build
// under node. Drives the Storage tab's own exports (src/seam/Storage_web.mm), the ones
// public/eden-storage.js calls, with no DOM:
//   (a) export: W256 (Diane, New Dawn v5, 266 MB as a `.eden`) is put in /documents as its
//       1 MB `.emod` and exported as a streamed `.eden.gz`; the wasm heap (sbrk top, sampled after
//       every 1 MB chunk) may grow by at most HEAP_BOUND across the whole export, and the
//       inflated download must be byte-identical to the original `.eden` in the zip;
//   (b) pre-flight == output: the report's `bytes` equals the inflated size, and a plain
//       (uncompressed) export is exactly `bytes` long;
//   (c) import with the `.emod` switch on: the same zip, fed through eden_storage_import_*, lists
//       as a `.emod` (format "emod"), no `.eden` ever appears in /documents (watched after every
//       step), the temp is gone, and its own-format export is the same `.eden` again;
//   (d) a Legacy64z export report counts the 256z content it drops (S.5b, smoke-level here — the
//       byte gates are the native ones in WORKING/s5-results-2026-10-10.md).
//
// Usage: node tools/headless-emod-export-test.js [path/to/eden.js]   (build-st or build-rel)
'use strict';
const fs = require('fs');
const path = require('path');
const vm = require('vm');
const zlib = require('zlib');
const crypto = require('crypto');
const { execFileSync } = require('child_process');

const positional = process.argv.slice(2).filter((a) => !a.startsWith('--'));
const edenJsPath = path.resolve(positional[0] || path.join(__dirname, '..', 'build-st', 'eden.js'));
const edenDir = path.dirname(edenJsPath);
const repo = path.join(__dirname, '..', '..');
const W256_EMOD = path.join(repo, 'TESTERS', 's4', 'ref-Diane-NewDawn256z.emod');
const W256_ZIP = path.join(repo, 'TESTERS', 'Diane-NewDawn256z.eden.zip');
const HEAP_BOUND = 8 << 20;

for (const f of [edenJsPath, W256_EMOD, W256_ZIP]) {
    if (!fs.existsSync(f)) { console.log('FAIL: missing', f); process.exit(1); }
}
const original = execFileSync('unzip', ['-p', W256_ZIP, 'Diane-NewDawn256z.eden'], { maxBuffer: 400 << 20 });
const sha = (b) => crypto.createHash('sha256').update(b).digest('hex');
const originalSha = sha(original);

global.require = require;
global.__dirname = edenDir;
global.__filename = edenJsPath;
global.Module = { print: () => {}, printErr: () => {} };
const cwdBefore = process.cwd();
process.chdir(edenDir);
vm.runInThisContext(fs.readFileSync(edenJsPath, 'utf8'), { filename: edenJsPath });
process.chdir(cwdBefore);

let fails = 0;
function check(ok, what) { console.log((ok ? 'PASS ' : 'FAIL ') + what); if (!ok) fails++; }

function utf8(M, p) {
    let e = p;
    while (M.HEAPU8[e]) e++;
    return new TextDecoder().decode(new Uint8Array(M.HEAPU8.subarray(p, e)));
}
const list = (M) => JSON.parse(utf8(M, M._eden_storage_list_worlds()));
const heap = (M) => JSON.parse(utf8(M, M._eden_debug_heap()));
// emmalloc's live bytes (build-rel only; -1 under build-st's dlmalloc). sbrkTop alone can read 0
// growth because the exporter's ~1.3 MB of buffers fit in free heap — live bytes show what it holds.
const live = (M) => (M._eden_debug_alloc ? JSON.parse(utf8(M, M._eden_debug_alloc())).live : -1);

function exportAt(M, index, target, gzip) {
    const h0 = heap(M).sbrkTop;            // before begin: the exporter's own buffers count too
    const l0 = live(M);
    let lMax = l0;
    const rep = JSON.parse(utf8(M, M._eden_storage_export_begin(index, target, 0, gzip, 0)));
    if (rep.error) return { rep };
    const parts = [];
    let hMax = h0;
    for (;;) {
        const n = M._eden_storage_export_next();
        if (n < 0) { M._eden_storage_export_end(); return { rep, error: utf8(M, M._eden_storage_job_error()) }; }
        if (n === 0) break;
        const p = M._eden_storage_export_chunk();
        parts.push(Buffer.from(M.HEAPU8.subarray(p, p + n)));
        hMax = Math.max(hMax, heap(M).sbrkTop);
        if (l0 >= 0 && parts.length % 16 === 1) lMax = Math.max(lMax, live(M));
    }
    M._eden_storage_export_end();
    return { rep, out: Buffer.concat(parts), heapDelta: hMax - h0, liveDelta: l0 >= 0 ? lMax - l0 : -1 };
}

function run() {
    const M = global.Module, FS = global.FS;
    const docs = '/documents';
    FS.writeFile(docs + '/w256.emod', fs.readFileSync(W256_EMOD));
    M._eden_storage_reload_worlds();
    let worlds = list(M);
    const wi = worlds.findIndex((w) => w.file === 'w256.emod');
    check(wi >= 0 && worlds[wi].format === 'emod' && worlds[wi].originalBytes === 0 && worlds[wi].height === 256,
        `(list) w256.emod lists as format "emod", 256z, originalBytes 0: ${JSON.stringify(worlds[wi])}`);

    // (a) + (b)
    let t0 = Date.now();
    const gz = exportAt(M, wi, 0, 1);
    check(!gz.error && gz.out, `(a) streamed .eden.gz export ran (${gz.error || ((gz.out.length / 1048576).toFixed(2) + ' MB gz, ' + (Date.now() - t0) + ' ms')})`);
    const inflated = gz.out ? zlib.gunzipSync(gz.out) : Buffer.alloc(0);
    check(sha(inflated) === originalSha, `(a) inflated export = the original .eden (${inflated.length} B, sha ${sha(inflated).slice(0, 12)})`);
    check(gz.heapDelta <= HEAP_BOUND, `(a) wasm heap delta across the export ${(gz.heapDelta / 1048576).toFixed(2)} MB <= ${HEAP_BOUND >> 20} MB`);
    if (gz.liveDelta >= 0) check(gz.liveDelta <= HEAP_BOUND, `(a) live allocations during the export peak ${(gz.liveDelta / 1048576).toFixed(2)} MB above the start`);
    check(gz.rep.bytes === inflated.length, `(b) pre-flight bytes ${gz.rep.bytes} == exported size ${inflated.length}`);
    const raw = exportAt(M, wi, 0, 0);
    check(raw.out && raw.out.length === raw.rep.bytes && sha(raw.out) === originalSha, `(b) uncompressed export is exactly the pre-flight size and the original`);

    // (d)
    const leg = JSON.parse(utf8(M, M._eden_storage_export_begin(wi, 1, 0, 1, 1)));
    check(!leg.error && leg.target === 'Legacy64z' && leg.bytes > 0 && leg.bytes < gz.rep.bytes,
        `(d) Legacy64z dry-run report: ${leg.error || leg.summary}`);

    // (c)
    M._eden_set_world_format(1);
    const zipBytes = fs.readFileSync(W256_ZIP);
    FS.writeFile('/tmp/.emod-import', zipBytes);
    const enc = new TextEncoder().encode('Diane-NewDawn256z.eden.zip');
    M.HEAPU8.set(enc, M._eden_storage_name_buffer());
    M.HEAPU8[M._eden_storage_name_buffer() + enc.length] = 0;
    const began = M._eden_storage_import_begin();
    check(began === 1, `(c) import began${began ? '' : ': ' + utf8(M, M._eden_storage_job_error())}`);
    let r = 0, steps = 0, sawEden = false;
    t0 = Date.now();
    while (began && (r = M._eden_storage_import_step()) === 0) {
        steps++;
        if (FS.readdir(docs).some((f) => /\.eden$/i.test(f))) sawEden = true;
    }
    const result = utf8(M, M._eden_storage_job_result());
    check(r === 1, `(c) import finished (${r === 1 ? result : utf8(M, M._eden_storage_job_error())}, ${steps} slices, ${Date.now() - t0} ms)`);
    check(!sawEden && !FS.readdir(docs).some((f) => /\.eden$/i.test(f)), '(c) no .eden was ever written to /documents');
    let tmpGone = true;
    try { FS.stat('/tmp/.emod-import'); tmpGone = false; } catch (e) { /* gone */ }
    check(tmpGone && !FS.readdir(docs).some((f) => /\.(layer\d+|spill|converting|part)$/.test(f)), '(c) the import temp and every spill/layer file are gone');
    worlds = list(M);
    const ii = worlds.findIndex((w) => w.file === result);
    check(ii >= 0 && worlds[ii].format === 'emod' && worlds[ii].height === 256, `(c) the import lists: ${JSON.stringify(worlds[ii])}`);
    if (ii >= 0) {
        const back = exportAt(M, ii, 0, 1);
        check(back.out && sha(zlib.gunzipSync(back.out)) === originalSha, '(c) the imported world exports back to the original .eden');
    }
    M._eden_set_world_format(0);

    console.log(fails ? `FAILED (${fails})` : 'ALL PASS');
    process.exit(fails ? 1 : 0);
}

global.Module.postRun = global.Module.postRun || [];
global.Module.postRun.push(() => {
    // The FileManager exists once the menu has been constructed; poll for it.
    const t0 = Date.now();
    (function wait() {
        let ok = false;
        try { ok = Array.isArray(list(global.Module)) && global.Module._eden_storage_export_begin; } catch (e) { ok = false; }
        if (ok) { try { run(); } catch (e) { console.log('FAIL: threw', e && e.stack || e); process.exit(1); } return; }
        if (Date.now() - t0 > 60000) { console.log('FAIL: engine never became ready'); process.exit(1); }
        setTimeout(wait, 200);
    })();
});

setTimeout(() => { console.log('FAIL: timed out'); process.exit(1); }, 900000).unref();
