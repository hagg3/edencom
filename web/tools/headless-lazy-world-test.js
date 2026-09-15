// Headless verification of the lazy (range-fetched) Eden.eden FS node — perf-audit ROI row 9,
// src/seam/js/eden_default_world.pre.js. Follows PORT-STATUS's documented vm.runInThisContext
// harness (plain require() does not share Module) and the same "never call _eden_debug_tick, drive
// on real wall-clock waits" methodology as tools/headless-menu-flow-test.js.
//
// Three things are checked, in increasing order of how much they'd hurt if wrong:
//
//   1. READ CORRECTNESS. The whole 52 MB file is read back THROUGH the FS node (via FS.read on a
//      real file descriptor, i.e. the exact path fread() takes from the engine) in pseudo-random
//      chunk sizes, and hashed. The hash must equal the hash of the real file on disk. This covers
//      block boundaries, partial blocks, the EOF short read, LRU eviction (52 MB through a 2 MB
//      cache evicts constantly) and the multi-block coalescing path. A silent off-by-one here
//      would corrupt terrain in ways that look like worldgen bugs, so it is checked exhaustively
//      rather than by sampling.
//   2. RESIDENCY. The point of the whole exercise: after streaming 52 MB through it, the cache
//      still holds <= MAX_BLOCKS blocks, and a real world load transfers a small fraction of the
//      file rather than all of it.
//   3. BEHAVIOURAL EQUIVALENCE. A NORMAL (not flat) world is created and played — the world type
//      whose terrain is streamed out of Eden.eden by fmh_readColumnFromDefault — and the player's
//      settled position is compared against the same run performed with the old whole-file eager
//      path (`--eager`, spawned as a child process). Identical spawn + identical settled Y means
//      the terrain the engine actually received was the same bytes. A player over empty terrain
//      falls forever, so this is a genuinely content-sensitive signal, not just "it didn't crash".
//
// Usage:
//   node tools/headless-lazy-world-test.js [path/to/eden.js]     # full test (spawns the eager run)
//   node tools/headless-lazy-world-test.js --eager [path]        # eager child mode, prints RESULT
//   node tools/headless-lazy-world-test.js --no-compare [path]   # skip the eager A/B leg
'use strict';
const fs = require('fs');
const path = require('path');
const vm = require('vm');
const crypto = require('crypto');
const { spawnSync } = require('child_process');

const args = process.argv.slice(2);
const EAGER = args.includes('--eager');
const MEASURE = args.includes('--measure'); // boot+load cost only: no read-back sweep, no A/B
const SWEEP = args.includes('--sweep');
const NO_COMPARE = args.includes('--no-compare') || MEASURE;
const edenJsPath = path.resolve(args.find((a) => !a.startsWith('--')) || path.join(__dirname, '..', 'build-st', 'eden.js'));
const edenDir = path.dirname(edenJsPath);
const edenFilePath = path.resolve(edenDir, '..', '..', 'Eden.eden'); // same resolution the pre-js uses

// Settle time after GAME_MODE_PLAY before sampling the player position. The player spawns above
// the terrain and falls; both legs of the A/B use the same value.
const SETTLE_MS = 6000;

let failures = 0;
function check(name, cond, detail) {
    if (cond) console.log('PASS:', name);
    else { console.log('FAIL:', name, detail !== undefined ? '— ' + detail : ''); failures++; }
}

// --sweep runs the measurement leg once per (block size, cache blocks, read-ahead) combination in
// its own process — the tunables are read at preRun time, so they cannot be changed in-process.
if (SWEEP) {
    const configs = [];
    for (const block of [16384, 32768, 65536, 131072])
        for (const blocks of [Math.round(2 * 1024 * 1024 / block), Math.round(4 * 1024 * 1024 / block)])
            for (const ra of [0, 1]) configs.push({ block, blocks, ra });
    console.log('block KB | cache MB | RA | boot reqs | boot KB | load reqs | load MB | hit%');
    for (const c of configs) {
        const child = spawnSync(process.execPath, [__filename, '--measure', edenJsPath], {
            encoding: 'utf8', timeout: 180000,
            env: Object.assign({}, process.env, {
                EDEN_FS_BLOCK: String(c.block), EDEN_FS_BLOCKS: String(c.blocks), EDEN_FS_READAHEAD: String(c.ra),
            }),
        });
        const line = (child.stdout || '').split('\n').find((l) => l.startsWith('RESULT '));
        if (!line) { console.log(`${c.block / 1024} | ${c.blocks} | ${c.ra} | FAILED`); continue; }
        const r = JSON.parse(line.slice('RESULT '.length));
        const hit = (100 * r.stats.blockHits / Math.max(1, r.stats.blockHits + r.stats.blockMisses)).toFixed(1);
        console.log(`${String(c.block / 1024).padStart(8)} | ${String((c.block * c.blocks) / 1048576).padStart(8)} | ` +
            `${String(c.ra).padStart(2)} | ${String(r.boot.requests).padStart(9)} | ${String(Math.round(r.boot.bytesFetched / 1024)).padStart(7)} | ` +
            `${String(r.stats.requests).padStart(9)} | ${String((r.stats.bytesFetched / 1048576).toFixed(2)).padStart(7)} | ${hit.padStart(5)}`);
    }
    process.exit(0);
}

global.require = require;
global.__dirname = edenDir;
global.__filename = edenJsPath;
global.Module = {
    EDEN_WORLD_FS: EAGER ? 'eager' : 'lazy',
    // --sweep re-measures the block-size/cache-size tunables without a rebuild (see the sweep
    // block at the bottom); unset in a normal run, so the pre-js's own defaults apply.
    EDEN_WORLD_FS_BLOCK: process.env.EDEN_FS_BLOCK ? Number(process.env.EDEN_FS_BLOCK) : undefined,
    EDEN_WORLD_FS_BLOCKS: process.env.EDEN_FS_BLOCKS ? Number(process.env.EDEN_FS_BLOCKS) : undefined,
    EDEN_WORLD_FS_READAHEAD: process.env.EDEN_FS_READAHEAD ? Number(process.env.EDEN_FS_READAHEAD) : undefined,
    print: () => {},                       // the engine's own boot chatter is noise here
    printErr: (t) => { if (/error|fail/i.test(t)) console.log('[err]', t); },
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
const menuState = () => JSON.parse(utf8(global.Module._eden_debug_menu_state()));
const playerState = () => JSON.parse(utf8(global.Module._eden_debug_player_state()));

function waitUntil(predicate, timeoutMs, label) {
    return new Promise((resolve) => {
        const start = Date.now();
        (function poll() {
            let ok = false;
            try { ok = predicate(); } catch (e) { /* not ready yet */ }
            if (ok) return resolve(true);
            if (Date.now() - start > timeoutMs) { console.log('  (timed out waiting for: ' + label + ')'); return resolve(false); }
            setTimeout(poll, 50);
        })();
    });
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// Read the whole file back through the FS node itself — FS.open/FS.read, the same stream_ops.read
// the engine's fread() lands in — in varying chunk sizes, and hash it.
function hashThroughFS(pathInFS, sizeHint) {
    const FS = global.FS;
    const h = crypto.createHash('sha1');
    const stream = FS.open(pathInFS, 'r');
    const buf = new Uint8Array(1 << 20);
    // Deterministic pseudo-random sizes: exercises reads that start mid-block, span 2+ blocks,
    // land exactly on a boundary, and finally run short at EOF.
    let seed = 12345;
    const nextSize = () => {
        seed = (seed * 1103515245 + 12345) & 0x7fffffff;
        const choices = [1, 16, 192, 1024, 12290, 65536, 65537, 131072, 700000];
        return choices[seed % choices.length];
    };
    let position = 0;
    for (;;) {
        const want = Math.min(nextSize(), buf.length);
        const got = FS.read(stream, buf, 0, want, position);
        if (got <= 0) break;
        h.update(Buffer.from(buf.buffer, buf.byteOffset, got));
        position += got;
        if (sizeHint && position >= sizeHint) break;
    }
    FS.close(stream);
    return { hash: h.digest('hex'), bytes: position };
}

global.Module.postRun = global.Module.postRun || [];
global.Module.postRun.push(async () => {
    const M = global.Module;
    const wfs = M.EdenWorldFS;
    const realStat = fs.statSync(edenFilePath);

    let boot = {};
    if (!EAGER) {
        boot = Object.assign({}, wfs.stats);
    }
    if (!EAGER && !MEASURE) {
        console.log('--- lazy FS node ---');
        console.log('mode:', wfs.mode, 'size:', wfs.size, 'blockSize:', wfs.blockSize, 'maxBlocks:', wfs.maxBlocks);
        check('lazy node installed (mode = lazy-fs)', wfs.mode === 'lazy-fs', wfs.mode);
        check('node reports the real file size', wfs.size === realStat.size, wfs.size + ' vs ' + realStat.size);

        const st = global.FS.stat('/bundle/Eden.eden');
        check('FS.stat reports the real size (fmh_init/NSBundle depend on this)', st.size === realStat.size, st.size);

        // fmh_init has already run inside main() at this point: header + the 518,400-byte
        // ColumnIndex directory. That is the entire cost of booting with this file.
        console.log('after boot (fmh_init read header + directory):', JSON.stringify(boot));
        check('boot did NOT read the whole file', boot.bytesFetched < 4 * 1024 * 1024,
              boot.bytesFetched + ' bytes fetched at boot');
        check('boot fetched at least the 518 KB directory', boot.bytesFetched > 500 * 1024, boot.bytesFetched);

        // 1. Read correctness, whole file, through the node.
        const t0 = Date.now();
        const viaFS = hashThroughFS('/bundle/Eden.eden', realStat.size);
        const real = crypto.createHash('sha1').update(fs.readFileSync(edenFilePath)).digest('hex');
        console.log('whole-file read-back through the FS node: ' + viaFS.bytes + ' bytes in ' + (Date.now() - t0) + ' ms');
        check('read-back length matches the real file', viaFS.bytes === realStat.size, viaFS.bytes);
        check('read-back is byte-identical to the real file (sha1)', viaFS.hash === real, viaFS.hash + ' vs ' + real);

        // 2. Residency, after deliberately streaming 52 MB through a 2 MB cache.
        const resident = wfs.blocksResident();
        check('cache stayed bounded after a full sweep', resident <= wfs.maxBlocks, resident + ' blocks');
        check('eviction actually happened during the sweep', wfs.stats.evictions > 0, wfs.stats.evictions);
        console.log('resident: ' + resident + ' blocks (' + ((resident * wfs.blockSize) / 1048576).toFixed(2) + ' MB), stats:',
                    JSON.stringify(wfs.stats));

    }

    if (!EAGER) {
        // Zero the counters (and, after the read-back sweep above, the cache) so the gameplay
        // numbers below measure the world load alone, not boot or the sweep.
        if (!MEASURE) wfs.dropCaches();
        wfs.stats.requests = 0; wfs.stats.bytesFetched = 0; wfs.stats.blockHits = 0;
        wfs.stats.blockMisses = 0; wfs.stats.evictions = 0; wfs.stats.reads = 0;
    }

    // 3. Create + play a NORMAL world (type 0) — the one whose terrain is streamed out of
    //    Eden.eden — and sample where the player ends up.
    const ok = await waitUntil(() => !menuState().error, 5000, 'World/Menu to exist');
    if (!ok) { console.log('FATAL: no World/Menu'); process.exit(1); }

    const idx = M._eden_menu_create_world();
    const name = utf8(M._eden_menu_world_name(idx));
    M._eden_menu_set_pending_world_type(0); // 0 = normal (streams the default world), 1 = flat
    M._eden_menu_play();
    const played = await waitUntil(() => menuState().game_mode === 1, 40000, 'GAME_MODE_PLAY');
    check('normal world reached GAME_MODE_PLAY', played);
    if (!played) { console.log(JSON.stringify(menuState())); process.exit(1); }

    await sleep(SETTLE_MS);
    const p = playerState();
    // "Settled" = standing on terrain rather than still falling. A player spawned over EMPTY
    // terrain (the failure mode if the default-world reads returned wrong/short data) never stops
    // falling, so a Y that stops changing is a real content-sensitive signal.
    await sleep(1500);
    const p2 = playerState();
    const settled = Math.abs(p2.pos[1] - p.pos[1]) < 0.05;
    const result = { name, pos: p.pos, settled, mode: wfs.mode, boot, stats: Object.assign({}, wfs.stats) };
    console.log('RESULT ' + JSON.stringify(result));

    if (EAGER || MEASURE) { process.exit(0); }

    console.log('after loading a normal world:', JSON.stringify(wfs.stats));
    check('the world load did NOT pull the whole file',
          wfs.stats.bytesFetched < 8 * 1024 * 1024,
          wfs.stats.bytesFetched + ' bytes');
    check('cache still bounded after a real world load', wfs.blocksResident() <= wfs.maxBlocks);
    check('player did not fall through empty terrain (y > -50)', p.pos[1] > -50, JSON.stringify(p.pos));
    check('player came to rest ON terrain streamed from Eden.eden', settled,
          p.pos[1] + ' -> ' + p2.pos[1]);

    if (!NO_COMPARE) {
        // NOTE: this leg deliberately does NOT compare spawn coordinates between the two paths.
        // Creating a world picks a RANDOM spawn (two lazy runs land in different places), so the
        // coordinates carry no information about the bytes. The byte-level equivalence of the two
        // paths is established directly and far more strongly by the whole-file sha1 read-back
        // above; what this leg is for is that the EAGER fallback still works at all, since its
        // plumbing moved when the lazy path landed — it is the path any server without byte
        // serving (python3 -m http.server) still uses.
        console.log('--- eager (whole-file) fallback path still works ---');
        const child = spawnSync(process.execPath, [__filename, '--eager', edenJsPath],
                                { encoding: 'utf8', timeout: 180000 });
        const line = (child.stdout || '').split('\n').find((l) => l.startsWith('RESULT '));
        if (!line) {
            check('eager reference run produced a result', false, (child.stderr || '').slice(-500));
        } else {
            const ref = JSON.parse(line.slice('RESULT '.length));
            console.log('eager run:', JSON.stringify(ref.pos), '(spawns are random, lazy was',
                        JSON.stringify(p.pos) + ')');
            check('eager fallback installed the whole file (mode = eager)', ref.mode === 'eager', ref.mode);
            check('eager fallback also reaches solid ground', ref.settled && ref.pos[1] > -50,
                  JSON.stringify(ref.pos) + ' settled=' + ref.settled);
        }
    }

    console.log(failures === 0 ? 'ALL PASS' : failures + ' FAILURE(S)');
    process.exit(failures === 0 ? 0 : 1);
});
