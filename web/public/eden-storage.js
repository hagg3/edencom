// eden-storage.js — local persistence + the Settings panel's "Storage" tab (pass 29).
// Requires: Module.preRun, FS.*, Module._eden_storage_list_worlds, window.EdenOPFS
// (eden-opfs.js, loaded before this file); calls window.EdenLoadError (loaded later) only from
// async syncfs/quota callbacks, never at top-level script time — see docs/ui.md's dependency
// graph (audit I2) for why that ordering is safe. Publishes: window.EdenStorage.
//
// Mounts /documents (the REAL FileManager's save directory — see docs/save-load.md) on durable
// browser storage, so world saves survive a reload instead of vanishing with the MEMFS they used
// to live in only (docs/archive/PORT-STATUS-2026-08-13.md's #1 open item). TWO backends:
//
//   * **OPFS** (preferred, ROADMAP Phase C / C2, 2026-09-02) — `eden-opfs.js` + its worker. MEMFS
//     is still the engine's filesystem; the backend records the byte RANGES written under the
//     mount and mirrors only those into OPFS via a sync access handle. ~130 KB per autosave on a
//     279 MB world instead of IDBFS's whole-file re-put, off the main thread.
//   * **IDBFS** (fallback) — Emscripten's, with {autoPersist:true}: every close-after-write,
//     mkdir, unlink or rename under /documents queues a debounced IndexedDB sync. Whole-file
//     granularity (WORKING/c1-idbfs-sync-cost-2026-09-02.md measured 279 MB + a ~558 MB transient
//     per autosave on a real 256z world), which is exactly why OPFS came first — but it is fine at
//     64z and it is the only thing that works where OPFS or sync access handles don't.
//
// Either way nothing on the C++ side knows this file exists: no engine-side save hook, no --wrap.
// `?storage=idb` forces the fallback (the one-flag rollback if OPFS misbehaves in the field),
// `?storage=opfs` refuses to fall back, `?storage=auto` (default) prefers OPFS when available.
//
// This file's two jobs:
//   1. `mountAndSync`, wired into Module.preRun (see eden-st.html) — picks a backend, mounts it
//      and populates MEMFS from whatever was saved last time, as a run DEPENDENCY so main() (and
//      therefore Menu::loadWorlds' one-shot directory read) cannot run until that read has
//      actually finished. This has to happen JS-side: the populate is inherently async, and
//      blocking it is exactly what Module.preRun + addRunDependency/removeRunDependency exists for.
//   2. Render the Storage tab's per-world list from Module._eden_storage_list_worlds() /
//      _eden_storage_delete_world_at(i) (src/seam/Storage_web.mm) — INDEX based, like every other
//      wasm call in this port, so nothing here needs _malloc/_free on the export list.
//
// Guarded throughout: node's headless `eden.js` (docs/STATUS.md "Running it") has neither
// indexedDB nor navigator.storage, and must keep working exactly as before — MEMFS, session-only.
(function () {
  'use strict';

  var MOUNT_PATH = '/documents';
  var OPFS_DIR = 'documents';
  var MIGRATION_KEY = 'eden.opfs.migrated';
  var mounted = false;
  var backendName = 'none';   // 'opfs' | 'idb' | 'none'
  var opfsType = null;        // the FS type object, for its _eden.stats()/flush()
  var opfsSink = null;

  function idbAvailable() {
    try { return typeof indexedDB !== 'undefined'; } catch (e) { return false; }
  }

  function storageMode() {
    try {
      var m = (new URLSearchParams(location.search).get('storage') || 'auto').toLowerCase();
      return (m === 'idb' || m === 'opfs') ? m : 'auto';
    } catch (e) { return 'auto'; }
  }

  function lsGet(k) { try { return localStorage.getItem(k); } catch (e) { return null; } }
  function lsSet(k, v) { try { localStorage.setItem(k, v); } catch (e) {} }

  // Module.preRun entry (called by the Emscripten runtime with no arguments before main()).
  function mountAndSync() {
    if (typeof FS === 'undefined') return;
    var M = window.Module;
    var mode = storageMode();
    // Perf-audit C4/§6: without an explicit persist() request, origin storage is evictable under
    // browser storage pressure — best-effort, no UI gate (Chrome grants it silently based on
    // site-engagement heuristics; Firefox/Safari may prompt or ignore it). Covers OPFS and
    // IndexedDB alike; not awaited, since the result doesn't change anything else here.
    if (navigator.storage && navigator.storage.persist) {
      navigator.storage.persist().catch(function () {});
    }
    try { FS.mkdir(MOUNT_PATH); } catch (e) { /* fine if it already exists */ }

    // ONE run dependency covering whichever backend wins, including a fallback that only becomes
    // necessary after the OPFS worker has failed — main() must not start in between.
    M.addRunDependency('eden-storage-populate');
    var released = false;
    function done() {
      if (released) return;
      released = true;
      M.removeRunDependency('eden-storage-populate');
      checkQuotaAndWarn();
    }

    var wantOpfs = mode !== 'idb' && window.EdenOPFS && window.EdenOPFS.available();
    if (!wantOpfs) {
      if (mode === 'opfs') console.warn('[eden-storage] ?storage=opfs asked for, but OPFS is not available here');
      return startIdb(done);
    }
    startOpfs(function (ok) {
      if (ok) return done();
      if (mode === 'opfs') {
        console.warn('[eden-storage] ?storage=opfs asked for and failed — NOT falling back');
        return done();
      }
      startIdb(done);
    });
  }

  function startIdb(done) {
    if (!idbAvailable() || typeof IDBFS === 'undefined') return done();
    try {
      FS.mount(IDBFS, { autoPersist: true }, MOUNT_PATH);
    } catch (e) {
      console.warn('[eden-storage] IDBFS mount failed — falling back to session-only storage:', e);
      return done();
    }
    mounted = true;
    backendName = 'idb';
    FS.syncfs(/*populate:*/true, function (err) {
      if (err) console.warn('[eden-storage] IndexedDB -> MEMFS populate failed:', err);
      done();
    });
  }

  // OPFS boot. Order matters and is the migration design (WORKING/opfs-backend-plan.md §4.4):
  //   1. acquire a sync access handle in the worker FIRST — a second tab holding the lock, or a
  //      browser with OPFS but no sync handles, must fall back before anything is written;
  //   2. if this origin has never migrated, snapshot the IndexedDB worlds through IDBFS itself
  //      (mounted at /documents, read, unmounted) rather than re-implementing its record format;
  //   3. mount OPFS, populate from it, then write back any snapshot world OPFS doesn't have.
  // The IndexedDB copy is deliberately NOT deleted in the same session that migrates: it is a free
  // rollback for one reload, and it is cleared on the next boot that successfully finds those same
  // worlds in OPFS (localStorage 'pending' -> 'done').
  function startOpfs(cb) {
    var sink;
    try {
      sink = window.EdenOPFS.makeWorkerSink({ dir: OPFS_DIR });
    } catch (e) {
      console.warn('[eden-storage] OPFS worker could not start:', e);
      return cb(false);
    }
    sink.init().then(function (info) {
      // Another tab already owns the sync-access-handle locks. Do NOT fall back to IDBFS: after
      // migration the IndexedDB copy is gone, so that path would show this tab an empty world
      // list and happily let the player create worlds in a store nothing reads afterwards. OPFS
      // is still READABLE without a lock, so populate a plain session-only MEMFS from it and say
      // out loud that saves won't stick here.
      if (info && info.locked) return startOpfsReadOnly(sink, cb);
      // Step 2 — the IndexedDB read has to happen BEFORE the OPFS mount, not after: IDBFS names
      // its database after the mount point, so it can only be read at /documents, and unmounting
      // /documents later would throw away everything the OPFS populate had just put there.
      readIdbSnapshot(function (carried) {
        var type;
        try {
          type = window.EdenOPFS.fsType(sink, { debug: /(\?|&)opfsdebug\b/.test(location.search) });
          FS.mount(type, {}, MOUNT_PATH);
        } catch (e) {
          console.warn('[eden-storage] OPFS mount failed:', e);
          sink.close();
          return cb(false);
        }
        FS.syncfs(/*populate:*/true, function (err) {
          if (err) {
            console.warn('[eden-storage] OPFS -> MEMFS populate failed:', err);
            try { FS.unmount(MOUNT_PATH); } catch (e2) {}
            sink.close();
            return cb(false);
          }
          opfsType = type;
          opfsSink = sink;
          mounted = true;
          backendName = 'opfs';
          finishMigration(carried, cb);
        });
      });
    }, function (err) {
      console.warn('[eden-storage] OPFS unavailable (' + ((err && err.message) || err) +
                   ') — using IndexedDB');
      sink.close();          // nothing was mounted; don't leave an idle worker behind
      cb(false);
    });
  }

  // The "Eden is already open in another tab" mount: /documents stays plain MEMFS (session-only),
  // seeded from a lock-free OPFS read so the player still sees and can play their worlds. Nothing
  // is mirrored back, and the storage warning says so.
  function startOpfsReadOnly(sink, cb) {
    sink.readAll().then(function (entries) {
      try {
        entries.forEach(function (e) {
          FS.writeFile(MOUNT_PATH + '/' + e.name,
                       e.bytes instanceof Uint8Array ? e.bytes : new Uint8Array(e.bytes));
        });
      } catch (e) { console.warn('[eden-storage] read-only OPFS populate failed:', e); }
      backendName = 'readonly';
      mounted = false;
      console.warn('[eden-storage] OPFS is locked by another tab — this tab is session-only');
      setTimeout(function () {
        if (window.EdenLoadError && window.EdenLoadError.showStorageWarning && !warnedThisSession) {
          warnedThisSession = true;
          window.EdenLoadError.showStorageWarning(
            'Eden is already open in another tab. Your worlds are readable here, but changes made ' +
            'in THIS tab will not be saved — close the other tab and reload to play normally.');
        }
      }, 0);
      cb(true);
    }, function (err) {
      console.warn('[eden-storage] locked-OPFS read failed:', err);
      cb(false);
    });
  }

  // Migration phase 1's read: mount IDBFS at /documents, populate, copy the bytes out, unmount.
  // Reuses IDBFS's own reader rather than re-implementing its record format. cb([]) — never an
  // error path; a failure here just means nothing gets carried over and the worlds stay in
  // IndexedDB for a later attempt.
  function readIdbSnapshot(cb) {
    if (lsGet(MIGRATION_KEY)) return cb([]);          // 'pending' or 'done' — phase 1 is over
    if (!idbAvailable() || typeof IDBFS === 'undefined') return cb([]);
    try {
      FS.mount(IDBFS, {}, MOUNT_PATH);
    } catch (e) { return cb([]); }
    FS.syncfs(true, function () {
      var carried = [];
      try {
        FS.readdir(MOUNT_PATH).forEach(function (n) {
          if (n === '.' || n === '..') return;
          try {
            var st = FS.stat(MOUNT_PATH + '/' + n);
            if (!FS.isFile(st.mode) || !st.size) return;
            carried.push({ name: n, bytes: FS.readFile(MOUNT_PATH + '/' + n) });
          } catch (e) {}
        });
      } catch (e) {}
      try { FS.unmount(MOUNT_PATH); } catch (e) {}
      cb(carried);
    });
  }

  function finishMigration(carried, cb) {
    var phase = lsGet(MIGRATION_KEY);
    if (phase === 'done') return cb(true);

    if (phase === 'pending') {
      // Phase 2: the worlds copied last session came back out of OPFS this session, so the
      // IndexedDB copy has done its job as a one-reload rollback and can go.
      var names = (lsGet(MIGRATION_KEY + '.names') || '').split('|').filter(Boolean);
      var allPresent = names.every(function (n) {
        try { return FS.analyzePath(MOUNT_PATH + '/' + n).exists; } catch (e) { return false; }
      });
      if (allPresent) {
        try { indexedDB.deleteDatabase(MOUNT_PATH); } catch (e) {}
        lsSet(MIGRATION_KEY, 'done');
        console.log('[eden-storage] OPFS migration complete — IndexedDB copy released');
      }
      return cb(true);
    }

    // Phase 1's write half: anything IndexedDB had that OPFS doesn't goes in now, through the
    // live mount, so it is recorded and flushed like any other write.
    var written = [];
    (carried || []).forEach(function (f) {
      if (FS.analyzePath(MOUNT_PATH + '/' + f.name).exists) return;  // OPFS already has it
      try { FS.writeFile(MOUNT_PATH + '/' + f.name, f.bytes); written.push(f.name); } catch (e) {}
    });
    lsSet(MIGRATION_KEY + '.names', (carried || []).map(function (f) { return f.name; }).join('|'));
    lsSet(MIGRATION_KEY, 'pending');
    if (!written.length) return cb(true);
    console.log('[eden-storage] migrated ' + written.length + ' world(s) IndexedDB -> OPFS');
    FS.syncfs(false, function () { cb(true); });
  }

  // Audit row A7: a `QuotaExceededError` (or any other) out of `FS.syncfs(false, …)` used to be
  // swallowed by a bare `catch (e) {}` here and at importFile's syncfs below — the player sees a
  // successful-looking save that never actually reached IndexedDB. Surface it through the same
  // dialog eden-loaderror.js already shows for a corrupt load, since both are "your world isn't
  // safe" situations the player must not find out about only on next boot. Reported once per
  // session (`warnedThisSession`) so a string of autosaves during a long play session doesn't spam
  // the dialog for what is, after the first hit, the same underlying full-quota condition.
  var warnedThisSession = false;
  function reportSyncError(err) {
    console.warn('[eden-storage] IndexedDB persist failed:', err);
    if (warnedThisSession) return;
    if (!(window.EdenLoadError && window.EdenLoadError.showStorageWarning)) return;
    var msg = (err && (err.name === 'QuotaExceededError' || /quota/i.test(String(err.message || err))))
      ? 'Your browser’s storage quota is full, so this save did not actually persist.'
      : 'A save to browser storage failed to persist (' + (err && (err.name || err.message) || err) + ').';
    warnedThisSession = true;
    window.EdenLoadError.showStorageWarning(msg);
  }

  // Pre-flight warning: check usage against quota *before* it actually fails, since by the time
  // syncfs errors the write attempt already happened. 80% is the threshold the audit calls out —
  // early enough to give the player a chance to export a world (F2) or clear space.
  function checkQuotaAndWarn() {
    if (warnedThisSession) return;
    estimateQuota(function (est) {
      if (!est || !est.quota) return;
      if (est.usage / est.quota < 0.8) return;
      if (warnedThisSession) return;
      if (!(window.EdenLoadError && window.EdenLoadError.showStorageWarning)) return;
      warnedThisSession = true;
      window.EdenLoadError.showStorageWarning(
        'Browser storage is ' + Math.round(100 * est.usage / est.quota) + '% full (' +
        formatBytes(est.usage) + ' of ' + formatBytes(est.quota) + '). Saves may soon fail to ' +
        'persist — consider exporting a world (Storage tab) or freeing space.');
    });
  }

  // Belt-and-suspenders: autoPersist's own queue fires on a setTimeout(0), which a same-tick page
  // unload can in principle race. Both events actually fire on tab close/backgrounding, unlike
  // 'beforeunload' (unreliable on mobile Safari) or 'unload' (deprecated).
  function flushNow() {
    // Phase N Stage 4.2, and it MUST be the first thing here: ask the engine to write the open
    // world into MEMFS before we mirror MEMFS to durable storage. Declared in eden-host.js, which
    // loads after this file — legal, and load-bearing, because this function body only ever runs
    // long after every script's first pass (the same cross-file rule eden-host.js's own header
    // documents). Cheap and self-guarding when there is no world open or the module isn't up.
    if (typeof edenSaveOpenWorld === 'function') edenSaveOpenWorld();
    if (!mounted || typeof FS === 'undefined') return;
    checkQuotaAndWarn();
    try {
      FS.syncfs(false, function (err) { if (err) reportSyncError(err); });
    } catch (e) {
      reportSyncError(e);
    }
  }
  document.addEventListener('visibilitychange', function () {
    if (document.visibilityState === 'hidden') flushNow();
  });
  window.addEventListener('pagehide', flushNow);

  // ---------------------------------------------------------------------------------------------
  // Storage tab data access
  // ---------------------------------------------------------------------------------------------
  function M() { return window.Module; }
  function ready() {
    return window.__edenModuleReady && M() && typeof M()._eden_storage_list_worlds === 'function';
  }
  function utf8(ptr) {
    var H = M().HEAPU8, end = ptr;
    while (H[end]) end++;
    // The Uint8Array copy is load-bearing in the EDEN_THREADED build (shared memory cannot be
    // handed to TextDecoder) — full reasoning on the canonical copy in eden-settings.js.
    return new TextDecoder().decode(new Uint8Array(H.subarray(ptr, end)));
  }

  function listWorlds() {
    if (!ready()) return [];
    try { return JSON.parse(utf8(M()._eden_storage_list_worlds())); }
    catch (e) { return []; }
  }

  function deleteWorldAt(index) {
    if (!ready()) return false;
    return M()._eden_storage_delete_world_at(index) !== 0;
  }

  // 256z Stage 3 item 5: space-reclaim action for a 256z ("New Dawn") world. Destructive (see
  // FileManager::convertWorldTo64's own header for exactly what it discards), so this returns the
  // full report rather than a bool — the Storage tab shows it before/instead of a bare success
  // toast. `index` is the same list-position convention as deleteWorldAt/exportWorldAt.
  function convertTo64zAt(index) {
    if (!ready() || !M()._eden_storage_convert_to_64z_at) {
      return { ok: false, error: 'not available' };
    }
    try { return JSON.parse(utf8(M()._eden_storage_convert_to_64z_at(index))); }
    catch (e) { return { ok: false, error: 'malformed response' }; }
  }

  // Row #18 (perf-audit §6, promoted from pass 35's "quick and dirty test hook" — the mechanism
  // was already right, it just hadn't been paired with export or surfaced as a real feature yet):
  // drop a .eden file straight into Documents so it shows up in the world picker, no engine save
  // path involved. Writes via FS.writeFile (the same MEMFS/IDBFS-backed mount Menu::loadWorlds
  // reads), nudges autoPersist with an explicit syncfs (belt-and-suspenders, same as flushNow
  // above), then asks the engine to re-scan its world list. No format validation — a bad file just
  // fails to load like any other corrupt save (docs/eden-file-format.md is the source of truth if
  // that ever matters); cb(ok, errorMessageOrNull).
  function importFile(file, cb) {
    if (!file) { cb && cb(false, 'no file'); return; }
    var reader = new FileReader();
    reader.onload = function () {
      finishImport(file.name || ('import-' + Date.now()), new Uint8Array(reader.result), cb);
    };
    reader.onerror = function () { cb && cb(false, 'file read failed'); };
    reader.readAsArrayBuffer(file);
  }

  // A world exported via the "Compressed" option (see exportWorldAt) is a plain gzip member wrapped
  // around the raw .eden bytes — not the engine's own RLE variant (docs/eden-file-format.md's "RLE
  // variant" is decode-only, bundled-default-world-only, and has no encoder anywhere in this repo or
  // in eden-world-editor to reuse). Gzip is the browser-native, round-trip-safe choice: detect it
  // either by the `.gz` name (own export) or the gzip magic bytes (someone renamed it), inflate with
  // the same DecompressionStream every modern browser already ships, then import the raw bytes as
  // normal. Detected by magic bytes rather than name alone so a re-named .eden.gz dropped onto the
  // Storage tab's file input still round-trips.
  function isGzip(bytes) {
    return bytes.length >= 2 && bytes[0] === 0x1f && bytes[1] === 0x8b;
  }

  function finishImport(name, bytes, cb) {
    if (isGzip(bytes)) {
      if (typeof DecompressionStream === 'undefined') {
        cb && cb(false, 'This browser cannot decompress gzip worlds (no DecompressionStream support).');
        return;
      }
      name = name.replace(/\.gz$/i, '');
      inflateGzip(bytes).then(function (raw) {
        writeWorldFile(name, raw, cb);
      }, function (e) {
        cb && cb(false, 'gzip decompress failed: ' + (e && e.message || e));
      });
      return;
    }
    writeWorldFile(name, bytes, cb);
  }

  function writeWorldFile(name, bytes, cb) {
    try {
      if (typeof FS === 'undefined') throw new Error('FS unavailable');
      if (!/\.eden$/i.test(name)) name += '.eden';
      FS.writeFile(MOUNT_PATH + '/' + name, bytes);
      if (mounted) {
        try { FS.syncfs(false, function (err) { if (err) reportSyncError(err); }); }
        catch (e) { reportSyncError(e); }
      }
      if (ready() && M()._eden_storage_reload_worlds) M()._eden_storage_reload_worlds();
      cb && cb(true, null);
    } catch (e) {
      cb && cb(false, e.message || String(e));
    }
  }

  function inflateGzip(bytes) {
    var ds = new DecompressionStream('gzip');
    var stream = new Blob([bytes]).stream().pipeThrough(ds);
    return new Response(stream).arrayBuffer().then(function (buf) { return new Uint8Array(buf); });
  }

  function deflateGzip(bytes) {
    var cs = new CompressionStream('gzip');
    var stream = new Blob([bytes]).stream().pipeThrough(cs);
    return new Response(stream).arrayBuffer().then(function (buf) { return new Uint8Array(buf); });
  }

  // Row #18 (perf-audit §6): "the single best answer to my browser cleared my storage" —
  // download a world's real .eden file so the player has an off-device copy, independent of
  // navigator.storage.persist()'s best-effort guarantee above. Reads the file straight out of the
  // same IDBFS-backed MEMFS mount importFile() writes into, so export/import round-trip through
  // exactly the format docs/eden-file-format.md describes — no engine call needed, this is a pure
  // file copy out of the mount.
  function downloadBytes(bytes, filename) {
    var blob = new Blob([bytes], { type: 'application/octet-stream' });
    var url = URL.createObjectURL(blob);
    var a = document.createElement('a');
    a.href = url;
    a.download = filename;
    document.body.appendChild(a);
    a.click();
    document.body.removeChild(a);
    setTimeout(function () { URL.revokeObjectURL(url); }, 4000);
  }

  // `compress: true` gzips the raw .eden bytes before handing them to the browser's normal download
  // flow — same file, smaller download, and losslessly reversible by importFile's gzip detection
  // above. Kept synchronous-shaped (returns true/false immediately) for the uncompressed path since
  // that's what the Storage tab's existing caller expects; compression is inherently async
  // (CompressionStream), so that path takes `cb(ok, errorOrNull)` instead — callers that only care
  // about the uncompressed case can still ignore the third argument.
  function exportWorldAt(index, compress, cb) {
    if (typeof compress === 'function') { cb = compress; compress = false; }
    if (!ready() || typeof FS === 'undefined') { cb && cb(false, 'not ready'); return false; }
    var worlds = listWorlds();
    var w = worlds[index];
    if (!w) { cb && cb(false, 'no such world'); return false; }
    try {
      var data = FS.readFile(MOUNT_PATH + '/' + w.file);
      if (!compress) {
        downloadBytes(data, w.file);
        cb && cb(true, null);
        return true;
      }
      if (typeof CompressionStream === 'undefined') {
        cb && cb(false, 'This browser cannot compress worlds (no CompressionStream support).');
        return false;
      }
      deflateGzip(data).then(function (gz) {
        downloadBytes(gz, w.file + '.gz');
        cb && cb(true, null);
      }, function (e) {
        cb && cb(false, 'gzip compress failed: ' + (e && e.message || e));
      });
      return true;
    } catch (e) {
      console.warn('[eden-storage] export failed:', e);
      cb && cb(false, e.message || String(e));
      return false;
    }
  }

  function canCompress() {
    return typeof CompressionStream !== 'undefined';
  }

  function formatBytes(n) {
    if (n < 1024) return n + ' B';
    if (n < 1024 * 1024) return (n / 1024).toFixed(1) + ' KB';
    if (n < 1024 * 1024 * 1024) return (n / (1024 * 1024)).toFixed(1) + ' MB';
    return (n / (1024 * 1024 * 1024)).toFixed(2) + ' GB';
  }

  function formatDate(ms) {
    if (!ms) return 'Unknown';
    var d = new Date(ms);
    return d.toLocaleDateString(undefined, { year: 'numeric', month: 'short', day: 'numeric' }) +
      ' ' + d.toLocaleTimeString(undefined, { hour: '2-digit', minute: '2-digit' });
  }

  // Best-effort browser-reported IndexedDB usage/quota for the whole origin (covers settings'
  // localStorage too, not just worlds) — not supported everywhere, callback gets null if so.
  function estimateQuota(cb) {
    if (navigator.storage && navigator.storage.estimate) {
      navigator.storage.estimate().then(cb).catch(function () { cb(null); });
    } else {
      cb(null);
    }
  }

  window.EdenStorage = {
    mountAndSync: mountAndSync,
    isPersistent: function () { return mounted; },
    // Which durable backend actually won this session: 'opfs', 'idb', or 'none' (session-only
    // MEMFS). Read by the Settings panel's persistence line and by tools/safari-opfs-live.js.
    backend: function () { return backendName; },
    // Per-flush byte counters from the OPFS backend — the number ROADMAP C2 exists to move
    // (bytes written to storage per autosave). null on the IDBFS path, which has no equivalent:
    // IDBFS re-puts whole files and never reports how much.
    opfsStats: function () { return opfsType ? opfsType._eden.stats() : null; },
    listWorlds: listWorlds,
    deleteWorldAt: deleteWorldAt,
    convertTo64zAt: convertTo64zAt,
    importFile: importFile,
    exportWorldAt: exportWorldAt,
    canCompress: canCompress,
    formatBytes: formatBytes,
    formatDate: formatDate,
    estimateQuota: estimateQuota
  };
})();
