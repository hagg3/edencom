// eden_default_world.pre.js — the pre-generated default world (Eden.eden), served to the engine
// WITHOUT holding its ~52 MB in memory (perf-audit §5b.1b / ROI row 9, pass 46).
//
// ---------------------------------------------------------------------------------------------
// History
//
// Eden.eden (~52 MB, repo-root, RLE reference map — docs/eden-file-format.md) was originally
// --preload-file'd into eden.data, so first paint was gated on downloading the WHOLE ~54 MB asset
// package. Pass 30 pulled it out of the package and fetched it separately (this file), which fixed
// first paint but still held every byte resident in MEMFS for the whole session.
//
// Pass 46 makes it a real lazily-read file: /bundle/Eden.eden is now a custom Emscripten FS node
// whose stream_ops.read is served out of a small LRU of 64 KB blocks, filled on demand by
// SYNCHRONOUS same-origin HTTP Range requests (browser) or fs.readSync (node). Residency drops
// from ~52.5 MB to ~2 MB, and a cold boot transfers ~0.6 MB of it instead of 52 MB.
//
// ---------------------------------------------------------------------------------------------
// Why a hand-written node and not FS.createLazyFile
//
// Emscripten's own FS.createLazyFile does the same idea, but its LazyUint8Array throws immediately
// when constructed outside a Web Worker ("Cannot do synchronous binary XHRs outside webworkers in
// modern browsers") — confirmed against this repo's own emsdk (emsdk/upstream/emscripten/src/
// library_fs.js). That check is about the DEPRECATION of main-thread sync XHR, not about it having
// been removed: it still works in every current browser. This build runs on the main thread
// (build-st, EDEN_THREADED=OFF), so the port implements the same idea itself, which also buys three
// things createLazyFile does not do: request coalescing across a multi-block read, a bounded LRU
// (createLazyFile caches every chunk it ever touched, forever — it never evicts, so on this
// workload it would slowly re-accumulate the whole 52 MB), and a graceful fall back to the old
// whole-file fetch when the server does not do byte serving.
//
// ---------------------------------------------------------------------------------------------
// The hard constraint this design has to respect
//
// The file must be FULLY OPENABLE AND READABLE, synchronously, before main() runs.
// FileManagerHelper::fmh_init opens Eden.eden and reads its header + 518 KB directory
// SYNCHRONOUSLY and unconditionally during FileManager's constructor, which runs inside
// World::World() at app startup. If the file is missing at that point, fmh_init's NSFileHandle
// ends up nil, and this port's hand-written ObjC runtime (src/shim/objc/objc_runtime.cpp's
// nilMethod) only special-cases ZERO-ARGUMENT nil sends — [saveFile readDataOfLength:...] passes
// an argument, so a nil saveFile there traps with "function signature mismatch" rather than
// degrading gracefully (a deliberate, documented tradeoff in objc_runtime.cpp, not a bug to fix
// here: the general fix, -sEMULATE_FUNCTION_POINTER_CASTS=1, "costs size/perf everywhere").
//
// A lazy node satisfies that constraint exactly: the node EXISTS with its real size (so stat/open/
// llseek-to-end all behave), and every read() is synchronous — it just doesn't hold the bytes.
// Deferring the fetch past main() entirely would still need Asyncify at the fmh_init call site;
// that is not what this is.
//
// ---------------------------------------------------------------------------------------------
// Access pattern this is tuned for (measure before changing BLOCK_SIZE / MAX_BLOCKS)
//
//   - boot: 192-byte header at offset 0, then a 518,400-byte sequential scan of the ColumnIndex
//     directory at offset 54,505,440 (32,400 × 16 B entries) — read 16 bytes at a time by
//     fmh_read_directory, coalesced by stdio into ~1 KB read()s. ~9 block fetches total.
//   - gameplay: fmh_readColumnFromDefault seeks to one column's chunk_offset and reads 4 chunks
//     sequentially (a 2-byte length prefix then <=12 KB of RLE each). A column is a few KB and is
//     contiguous, so a column costs 0-1 block fetches, and neighbouring columns (the player walking)
//     mostly land in blocks already resident.
//
// ---------------------------------------------------------------------------------------------
// Escape hatches
//
//   - ?worldfs=eager on the page URL (or Module.EDEN_WORLD_FS = 'eager' before the module loads)
//     forces the pass-30 whole-file fetch. Use it to rule this file out when debugging terrain.
//   - Automatic: if the server does not answer a Range request with 206 + a parseable Content-Range
//     (python3 -m http.server does NOT do byte serving), the eager path is used instead, so the
//     port still works on any static server. web/tools/serve.js does support ranges.
//   - Module.EdenWorldFS exposes { mode, size, blockSize, maxBlocks, stats } for tests/diagnostics
//     (web/tools/headless-lazy-world-test.js asserts against it).
//
// This is a --pre-js (not a public/*.js <script>) so it runs identically under `node eden.js`
// (headless, no DOM, no <script> tags at all) and in the browser.

Module['preRun'] = Module['preRun'] || [];
Module['preRun'].push(function () {
  if (typeof FS === 'undefined') return;

  var DEP = 'eden-default-world-fetch';
  var URL_PATH = 'Eden.eden';   // browser: relative to public/eden-st.html (CMakeLists symlinks
                                // public/Eden.eden -> ../Eden.eden, same trick as public/audio)
  // Tunables. These defaults are MEASURED, not guessed — tools/headless-lazy-world-test.js --sweep
  // runs a real boot + initial world load once per combination (each in its own process, since
  // these are read at preRun time). Cold boot + first normal-world load, totals:
  //
  //     block  cache  read-ahead |  requests   bytes
  //     16 KB   4 MB      0      |    108      1.7 MB
  //     32 KB   4 MB      0      |     81      2.5 MB
  //     32 KB   4 MB      1      |     49      3.0 MB   <- default
  //     64 KB   2 MB      1      |     41      5.1 MB
  //    128 KB   2 MB      1      |     40      9.9 MB
  //
  // Request count is weighted over raw bytes because every one of these is a SYNCHRONOUS XHR on
  // the main thread: they cannot overlap, so N requests cost N round trips of dead time no matter
  // how fast the link is. 32 KB/4 MB/RA1 is the knee — halving the requests of the 16 KB row for
  // ~1 MB more transfer, and still 4 MB resident against the old 52.5 MB.
  // Overridable from Module so the sweep can re-measure without a rebuild.
  var BLOCK_SIZE = Module['EDEN_WORLD_FS_BLOCK'] || 32768;  // one column of RLE is ~1.6 KB, but a
                                                            // window ROW of columns is contiguous
  var MAX_BLOCKS = Module['EDEN_WORLD_FS_BLOCKS'] || 128;   // 128 × 32 KB = 4 MB residency ceiling
  var READAHEAD = Module['EDEN_WORLD_FS_READAHEAD'] === undefined ? 1 : Module['EDEN_WORLD_FS_READAHEAD'];

  var nowMs = (typeof performance === 'object' && performance.now)
    ? function () { return performance.now(); }
    : function () { return Date.now(); };

  Module['addRunDependency'](DEP);
  var depDone = false;
  function doneDep() { if (!depDone) { depDone = true; Module['removeRunDependency'](DEP); } }

  // fetchMs / fetchMsMax (B1, ROADMAP Phase B): wall time spent in readRange() — the synchronous
  // XHR range fetch (browser) or fs.readSync (node) that is the actual transport under a cold
  // block read. Paired with MeshTiming_web.mm's ioMs (total fread time): fetchMs is the transport
  // subset, ioMs - fetchMs is this cache/coalesce layer's own overhead.
  var stats = { requests: 0, bytesFetched: 0, reads: 0, blockHits: 0, blockMisses: 0, evictions: 0,
                fetchMs: 0, fetchMsMax: 0 };
  Module['EdenWorldFS'] = { mode: 'pending', size: 0, blockSize: BLOCK_SIZE, maxBlocks: MAX_BLOCKS, stats: stats };

  // ------------------------------------------------------------------ eager fallback (pass 30)

  // Pass 50: serve straight out of the already-downloaded buffer instead of FS.writeFile, which
  // makes MEMFS allocate and memcpy a SECOND ~size-of-file buffer on top of the one `bytes`
  // already holds. On a host where byte serving is not usable — which since pass 57 means Safari
  // against GitHub Pages, whose sync-XHR ranges come out of the gzip representation (see the probe
  // below; the older claim here, that Pages ignores Range outright, was measured wrong) — this IS
  // the whole-file path for every cold boot, so that transient
  // doubling — on top of whatever the download itself (chunks array + concat, see eagerFetch)
  // was already holding — is real peak-memory pressure, not a rounding error. iOS Safari's
  // per-tab memory ceiling is tighter than desktop, and this function's old body was the
  // single biggest resident allocation in the whole boot path (52 MB copy of a 52 MB copy). No
  // reports of an actual crash/OOM message — Safari fails this kind of pressure silently, which
  // reads indistinguishable from "stuck loading forever". This makes the file resident exactly
  // once for its whole lazy-node lifetime (same node shape as installLazyNode below, minus any
  // network path — `read` is a plain subarray copy out of `bytes`).
  function populateEager(bytes) {
    try { FS.mkdirTree('/bundle'); } catch (e) { /* already created by the media preloads */ }
    try { FS.unlink('/bundle/Eden.eden'); } catch (e) { /* not there yet, the normal case */ }
    try {
      var node = FS.createFile('/bundle', 'Eden.eden', {}, true, false);
      node.contents = null;
      Object.defineProperty(node, 'usedBytes', { get: function () { return bytes.length; }, configurable: true });

      var ops = Object.create(null);
      for (var key in node.stream_ops) ops[key] = node.stream_ops[key];
      ops.read = function (stream, buffer, offset, length, position) {
        if (position >= bytes.length || length <= 0) return 0;
        if (position + length > bytes.length) length = bytes.length - position;
        buffer.set(bytes.subarray(position, position + length), offset);
        return length;
      };
      ops.write = function () { throw new FS.ErrnoError(63 /* EPERM */); };
      ops.mmap = function () { throw new FS.ErrnoError(63 /* EPERM */); };
      node.stream_ops = ops;

      Module['EdenWorldFS'].mode = 'eager';
      Module['EdenWorldFS'].size = bytes.length;
    } catch (e) {
      console.warn('[eden] installing /bundle/Eden.eden failed:', e);
    }
    doneDep();
  }

  // ------------------------------------------------------------------ the lazy node

  // `readRange(start, endInclusive)` -> Uint8Array; supplied by the node or browser backend below.
  // Everything from here down is backend-agnostic, which is what lets the headless test exercise
  // the whole cache/coalescing/eviction path for real.
  function installLazyNode(size, readRange, mode) {
    try { FS.mkdirTree('/bundle'); } catch (e) { /* already created by the media preloads */ }
    try { FS.unlink('/bundle/Eden.eden'); } catch (e) { /* not there yet, which is the normal case */ }

    var blocks = new Map();   // blockIndex -> Uint8Array (Map iterates in insertion order: LRU
                              // is maintained by delete+re-set on touch, no side list needed)

    function touch(bi, buf) {
      blocks.delete(bi);
      blocks.set(bi, buf);
    }

    function evictIfNeeded() {
      while (blocks.size > MAX_BLOCKS) {
        var oldest = blocks.keys().next().value;
        blocks.delete(oldest);
        stats.evictions++;
      }
    }

    // Fetch blocks b0..b1 inclusive in ONE request and file them individually.
    function fetchBlocks(b0, b1) {
      var start = b0 * BLOCK_SIZE;
      var end = Math.min((b1 + 1) * BLOCK_SIZE, size) - 1;
      var _t0 = nowMs();
      var data = readRange(start, end);
      var _dt = nowMs() - _t0;
      stats.fetchMs += _dt;
      if (_dt > stats.fetchMsMax) stats.fetchMsMax = _dt;
      stats.requests++;
      stats.bytesFetched += data.length;
      for (var bi = b0; bi <= b1; bi++) {
        var from = bi * BLOCK_SIZE - start;
        if (from >= data.length) break; // short read at EOF
        var to = Math.min(from + BLOCK_SIZE, data.length);
        touch(bi, data.subarray(from, to));
      }
      evictIfNeeded();
    }

    // Make sure every block in [b0,b1] is resident, fetching each contiguous missing run once.
    function ensure(b0, b1) {
      var lastBlock = Math.ceil(size / BLOCK_SIZE) - 1;
      var i = b0;
      while (i <= b1) {
        if (blocks.has(i)) { touch(i, blocks.get(i)); stats.blockHits++; i++; continue; }
        var j = i;
        while (j + 1 <= b1 && !blocks.has(j + 1)) j++;
        // Read-ahead: only past the END of the requested span, and only into blocks we don't have
        // — the engine's two access patterns (the directory scan, a column's four chunks) are both
        // strictly forward-sequential, so the next block is nearly always the next thing wanted.
        if (j === b1) {
          for (var k = 0; k < READAHEAD && j + 1 <= lastBlock && !blocks.has(j + 1); k++) j++;
        }
        stats.blockMisses += (j - i + 1);
        fetchBlocks(i, j);
        i = j + 1;
      }
    }

    // Copy [position, position+length) into dest at destOffset. Returns bytes copied.
    function readInto(dest, destOffset, position, length) {
      stats.reads++;
      if (position >= size || length <= 0) return 0;
      if (length > size - position) length = size - position;

      var b0 = Math.floor(position / BLOCK_SIZE);
      var b1 = Math.floor((position + length - 1) / BLOCK_SIZE);

      // A read wider than the whole cache can't be served block-by-block without evicting blocks
      // this same read still needs. Nothing in the engine does this today (the largest read is a
      // ~12 KB RLE chunk), but a bypass is two lines and removes the failure mode entirely.
      if (b1 - b0 + 1 > MAX_BLOCKS) {
        var _tw = nowMs();
        var whole = readRange(position, position + length - 1);
        var _dtw = nowMs() - _tw;
        stats.fetchMs += _dtw;
        if (_dtw > stats.fetchMsMax) stats.fetchMsMax = _dtw;
        stats.requests++;
        stats.bytesFetched += whole.length;
        dest.set(whole, destOffset);
        return whole.length;
      }

      ensure(b0, b1);
      var got = 0;
      while (got < length) {
        var pos = position + got;
        var bi = Math.floor(pos / BLOCK_SIZE);
        var blk = blocks.get(bi);
        if (!blk) break; // only reachable on a short read past real EOF
        var within = pos - bi * BLOCK_SIZE;
        var n = Math.min(length - got, blk.length - within);
        if (n <= 0) break;
        dest.set(blk.subarray(within, within + n), destOffset + got);
        got += n;
      }
      return got;
    }

    var node = FS.createFile('/bundle', 'Eden.eden', {}, true, false);
    node.contents = null;
    // MEMFS's own getattr (attr.size) and llseek (SEEK_END) both read node.usedBytes, so defining
    // it as a getter is all it takes for stat/fseek(SEEK_END)/ftello to report the real size while
    // no bytes are held. Same hook FS.createLazyFile uses.
    Object.defineProperty(node, 'usedBytes', { get: function () { return size; }, configurable: true });

    // MEMFS shares ONE stream_ops object across every node — copy before overriding, never mutate.
    var ops = Object.create(null);
    for (var key in node.stream_ops) ops[key] = node.stream_ops[key];

    ops.read = function (stream, buffer, offset, length, position) {
      return readInto(buffer, offset, position, length);
    };
    // Read-only: the bundle copy is never written by the engine (saves go to /documents), and a
    // write that silently vanished into the cache would be far worse than EPERM. Same for mmap —
    // nothing in the engine mmaps a file (stdio fread is the only reader), so refusing is honest
    // where a half-implemented mapping would not be. Both errno values are Emscripten's own
    // numbering (EPERM = 63, see eden.js's ERRNO_CODES), NOT musl's.
    ops.write = function () { throw new FS.ErrnoError(63 /* EPERM */); };
    ops.mmap = function () { throw new FS.ErrnoError(63 /* EPERM */); };
    node.stream_ops = ops;

    Module['EdenWorldFS'].mode = mode;
    Module['EdenWorldFS'].size = size;
    Module['EdenWorldFS'].blocksResident = function () { return blocks.size; };
    Module['EdenWorldFS'].dropCaches = function () { blocks.clear(); };
    doneDep();
  }

  // ------------------------------------------------------------------ backends

  var isNode = (typeof process === 'object') && process.versions && process.versions.node;

  function wantsEager() {
    if (Module['EDEN_WORLD_FS'] === 'eager') return true;
    if (Module['EDEN_WORLD_FS'] === 'lazy') return false;
    if (typeof location !== 'undefined' && location.search && /[?&]worldfs=eager\b/.test(location.search)) return true;
    return false;
  }

  if (isNode) {
    // Headless (`node eden.js`, run from build-st/ — resolve via __dirname, not cwd, so it works
    // regardless of where node was invoked from). No HTTP here: fs.readSync IS the range read, so
    // headless gets the same residency win AND exercises the identical cache/coalescing code.
    var nodeFs, p;
    try {
      nodeFs = require('fs');
      var nodePath = require('path');
      p = nodePath.join(__dirname, '..', '..', 'Eden.eden'); // build-st/ -> web/ -> repo root
      if (wantsEager()) {
        populateEager(nodeFs.readFileSync(p));
      } else {
        var fd = nodeFs.openSync(p, 'r');
        var nodeSize = nodeFs.fstatSync(fd).size;
        installLazyNode(nodeSize, function (start, end) {
          var len = end - start + 1;
          // Buffer.alloc, not allocUnsafe: a sub-4 KB allocUnsafe comes out of Node's shared
          // pool, whose ArrayBuffer is reused by later allocations — and these buffers are
          // RETAINED in the block cache as Uint8Array views, so a pooled one would be silently
          // rewritten under the cache. (Reachable at EOF, where the last block is a partial read.)
          var buf = Buffer.alloc(len);
          var got = nodeFs.readSync(fd, buf, 0, len, start);
          return new Uint8Array(buf.buffer, buf.byteOffset, got);
        }, 'lazy-fs');
      }
    } catch (e) {
      console.warn('[eden] node Eden.eden open failed (default-world terrain will be empty):', e);
      doneDep();
    }
  } else if (typeof fetch === 'function') {
    // Browser. Two stages, and the split matters: the CAPABILITY PROBE is async (a normal fetch,
    // no main-thread stall, before any run dependency is released), and only the per-block READS
    // are synchronous — which they have to be, because they happen underneath a synchronous
    // fread() inside the engine.
    var eagerFetch = function () {
      // Pass 30's path, streaming so public/eden-loading.js can show byte-level progress for
      // what is then the largest download of a cold boot. Pass 50: when the response carries a
      // Content-Length (GitHub Pages always sends one), pre-allocate ONE destination buffer and
      // fill it in place, instead of accumulating a chunks[] array and concatenating into a
      // second buffer at the end — that old pattern peaked at 2x the file's resident size during
      // the copy, stacked on top of what populateEager then duplicated AGAIN via FS.writeFile
      // (fixed separately, see that function). A stated Content-Length is the wire/encoded size,
      // which is smaller than the decoded body only when the response is compression-encoded;
      // GitHub Pages does not compress `application/octet-stream`, but if some future host does,
      // overflowing the pre-allocated buffer falls back to the safe chunks[] path for the rest of
      // the transfer rather than throwing.
      fetch(URL_PATH).then(function (r) {
        if (!r.ok) throw new Error('HTTP ' + r.status);
        var total = Number(r.headers.get('Content-Length')) || 0;
        var reportProgress = (typeof window !== 'undefined' && window.EdenLoading)
          ? window.EdenLoading.setEdenFileProgress : null;
        if (!r.body || !r.body.getReader) return r.arrayBuffer();

        var reader = r.body.getReader();
        var out = total > 0 ? new Uint8Array(total) : null;   // pre-sized, filled in place
        var chunks = out ? null : [];                          // fallback path, only if no size
        var loaded = 0;
        function pump() {
          return reader.read().then(function (step) {
            if (step.done) {
              if (out) return (loaded === out.length) ? out.buffer : out.buffer.slice(0, loaded);
              var merged = new Uint8Array(loaded);
              var mergedOffset = 0;
              for (var i = 0; i < chunks.length; i++) { merged.set(chunks[i], mergedOffset); mergedOffset += chunks[i].length; }
              return merged.buffer;
            }
            if (out && loaded + step.value.length > out.length) {
              // Content-Length under-reported the real (decoded) size — bail to the chunked path
              // for the remainder rather than throwing on an out-of-bounds .set().
              chunks = [out.subarray(0, loaded), step.value];
              out = null;
            } else if (out) {
              out.set(step.value, loaded);
            } else {
              chunks.push(step.value);
            }
            loaded += step.value.length;
            if (reportProgress) reportProgress(loaded, total || loaded);
            return pump();
          });
        }
        return pump();
      })
        .then(function (buf) { populateEager(new Uint8Array(buf)); })
        .catch(function (e) {
          console.warn('[eden] Eden.eden fetch failed (default-world terrain will be empty):', e);
          doneDep();
        });
    };

    // Synchronous XHR, deprecated-but-functional on the main thread. responseType can NOT be set
    // on a synchronous main-thread XHR (InvalidAccessError), so the binary comes back through the
    // classic overrideMimeType('text/plain; charset=x-user-defined') channel: that charset maps
    // bytes 0x80-0xFF to U+F780-U+F7FF, so `charCodeAt(i) & 0xFF` recovers the exact byte.
    //
    // `rawSyncRange` is the transport; `syncRange` below is what the lazy node gets. The split
    // exists so the capability probe can exercise the EXACT transport the reads will use — see
    // the probe's comment for the Safari/gzip bug that made that necessary.
    var rawSyncRange = function (start, end) {
      var xhr = new XMLHttpRequest();
      xhr.open('GET', URL_PATH, false);
      xhr.setRequestHeader('Range', 'bytes=' + start + '-' + end);
      if (xhr.overrideMimeType) xhr.overrideMimeType('text/plain; charset=x-user-defined');
      xhr.send(null);
      if (xhr.status !== 206 && xhr.status !== 200) {
        throw new Error('Eden.eden range ' + start + '-' + end + ': HTTP ' + xhr.status +
                        ' (Content-Range ' + xhr.getResponseHeader('Content-Range') + ')');
      }
      var text = xhr.responseText || '';
      var out = new Uint8Array(text.length);
      for (var i = 0; i < text.length; i++) out[i] = text.charCodeAt(i) & 0xFF;
      // A 200 here means the server ignored Range and sent the whole file (the probe below should
      // have caught that, but a proxy can change its mind mid-session) — slice out what was asked
      // for so the read is still correct, at the cost of the transfer.
      if (xhr.status === 200 && out.length > end - start + 1) return out.subarray(start, end + 1);
      return out;
    };

    // A FAILED RANGE READ MUST NOT THROW A PLAIN JS ERROR. This runs underneath a synchronous
    // fread() inside the engine, which during boot is underneath main() itself; a bare throw
    // unwinds THROUGH the wasm frames, so `main()` never reaches emscripten_set_main_loop() and
    // the engine's frame loop is never registered. The page survives (the DOM menu, hotbar and
    // eden-st.html's own rAF watchdog all keep running) and the only symptom is a permanently
    // black canvas over a perfectly healthy WebGL2 context — a failure that looks like a renderer
    // bug and is nothing of the kind. That is the shape the Safari/GitHub-Pages bug took; the
    // probe below is what stops it happening, and this is what keeps it survivable if some other
    // host finds a new way to break mid-session.
    //
    // FS.ErrnoError is the right currency instead: Emscripten's syscall wrappers catch it and
    // return -errno to the caller, so a broken read degrades to an I/O error the engine's own
    // truncated/corrupt-world handling can see (and eden-loaderror.js can surface), with the wasm
    // stack intact.
    var syncRange = function (start, end) {
      try {
        return rawSyncRange(start, end);
      } catch (e) {
        console.error('[eden] Eden.eden lazy read failed at ' + start + '-' + end +
                      ' — reporting EIO to the engine rather than unwinding main():', e);
        Module['EdenWorldFS'].degraded = true;
        throw new FS.ErrnoError(29 /* EIO */);
      }
    };

    if (wantsEager()) {
      eagerFetch();
    } else {
      // Probe: one byte, asking for a range. A byte-serving server answers 206 with a
      // "bytes 0-0/<total>" Content-Range, which is also where the file size comes from — no
      // separate HEAD needed. Anything else (200, no Content-Range, a Content-Encoding meaning
      // the range would be of the COMPRESSED body) means byte serving is not usable here.
      //
      // STAGE 2 IS NOT OPTIONAL, and the reason is worth keeping. This fetch() probe passing does
      // NOT mean the reads will work, because the reads do not use fetch() — they use synchronous
      // XHR, and the two do not negotiate the same content encoding. Measured on Safari against
      // GitHub Pages (both macOS and iOS, live): WebKit's fetch() suppresses compression when the
      // request carries a Range header, so the probe gets identity and a truthful
      // "bytes 0-0/55023840". WebKit's sync XHR does not, so Fastly answers it from the *gzip*
      // representation instead — 9,223,675 bytes — and applies the byte range to the COMPRESSED
      // body. Every small read still succeeds (and returns gzip bytes where the engine expects
      // terrain), and the first read past 9.2 MB — the ColumnIndex at EOF, i.e. the very first
      // thing the engine reads — comes back 416. Chromium negotiates identity on both, which is
      // why this was Safari-only, and web/tools/serve.js never compresses the .eden, which is why
      // it only ever appeared on the deployed site.
      //
      // So: probe the real transport, at the FAR END of the file, and require the size it reports
      // to agree with stage 1. A server serving ranges out of a compressed representation fails
      // this on both counts (416, or a Content-Range total that is the compressed size), and we
      // take the whole-file path — which is correct everywhere, just heavier.
      fetch(URL_PATH, { headers: { 'Range': 'bytes=0-0' } }).then(function (r) {
        var cr = r.headers.get('Content-Range');
        var enc = r.headers.get('Content-Encoding');
        var m = cr && /\/\s*(\d+)\s*$/.exec(cr);
        if (r.status !== 206 || !m || enc) {
          console.log('[eden] Eden.eden: server does not do byte serving (status ' + r.status +
                      ', Content-Range ' + cr + ', Content-Encoding ' + enc + ') — fetching it whole.');
          eagerFetch();
          return;
        }
        var total = Number(m[1]);
        return r.arrayBuffer().then(function () {
          // Stage 2: the same sync XHR the reads use, on the last byte of the file.
          try {
            var tail = rawSyncRange(total - 1, total - 1);
            if (tail.length !== 1) throw new Error('tail read returned ' + tail.length + ' bytes, expected 1');
          } catch (e) {
            console.log('[eden] Eden.eden: byte serving is not usable from synchronous XHR on this ' +
                        'host (' + e.message + ') — fetching it whole. This is the Safari/GitHub-Pages ' +
                        'gzip-range case; see this file\'s probe comment.');
            eagerFetch();
            return;
          }
          installLazyNode(total, syncRange, 'lazy-range');
          console.log('[eden] Eden.eden: lazy range-fetch active (' + total + ' bytes, ' +
                      (BLOCK_SIZE / 1024) + ' KB blocks, ' + MAX_BLOCKS + ' cached).');
        });
      }).catch(function (e) {
        // installLazyNode() releases the run dependency, which starts main() SYNCHRONOUSLY inside
        // the .then above — so anything the engine throws during boot lands here too, looking
        // exactly like a probe failure. Re-running eagerFetch() at that point would swap the FS
        // node out from under a half-booted engine and bury the real error under a wrong message
        // (which is how the Safari bug first presented). Only fall back if the boot has not
        // actually started yet.
        if (depDone) {
          console.error('[eden] Eden.eden: error escaped engine startup, not a probe failure:', e);
          return;
        }
        console.warn('[eden] Eden.eden range probe failed, falling back to whole-file fetch:', e);
        eagerFetch();
      });
    }
  } else {
    console.warn('[eden] no fetch() and not node — default-world terrain will be empty.');
    doneDep();
  }
});
