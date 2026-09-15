// eden-opfs.js — ROADMAP Phase C / C2: the OPFS persistence backend (2026-09-02).
// Requires: FS, MEMFS (globals from eden.js — non-MODULARIZE output, same assumption
// eden-storage.js already makes). Publishes: window.EdenOPFS (globalThis.EdenOPFS under node).
// Design + the alternatives that were rejected: ../../WORKING/opfs-backend-plan.md.
//
// WHAT THIS REPLACES. /documents was mounted on IDBFS with {autoPersist:true}. IDBFS syncs at
// WHOLE-FILE granularity: the world file's mtime changes on every save, so `reconcile()` re-`put`s
// the ENTIRE file into IndexedDB per autosave — measured at 279 MB written + a ~558 MB structured-
// clone transient + a ~53 ms (desktop) / ~150-250 ms (mobile) synchronous main-thread stall on the
// real Diane-NewDawn256z specimen, against the ~130 KB the engine's save actually dirties
// (WORKING/c1-idbfs-sync-cost-2026-09-02.md). Autosaves fire every streaming-boundary crossing.
//
// WHAT IT DOES INSTEAD. MEMFS stays the engine's filesystem — unchanged, synchronous, zero engine
// risk. This file records the byte RANGES the engine writes under the mount and mirrors only those
// into OPFS through a `FileSystemSyncAccessHandle` (`write(buf, {at})` — a true random-access
// partial write) held by a dedicated Worker.
//
// THE CONSTRAINT THAT SHAPES ALL OF IT: sync access handles exist in Workers ONLY, and the engine
// runs on the browser main thread (this port is deliberately not PROXY_TO_PTHREAD — see
// web/CLAUDE.md), where Atomics.wait is illegal. So OPFS CANNOT back the engine's own reads, only
// the durable mirror underneath them. Corollary worth keeping: "use OPFS" is not the win by itself
// — `createWritable()` is available on the main thread but writes through a swap file and copies
// the whole existing file first, i.e. exactly the cost this row deletes. Sync access handles are
// the win.
//
// WHAT IT DOES NOT FIX: MEMFS still holds the whole world in the JS heap (279 MB for Diane). That
// needs the engine itself on a worker (web-port-plan.md's D1) and is not this row.
//
// Shape: an Emscripten FS *type* ({mount, syncfs}) exactly like library_idbfs.js, so it drops into
// the existing plumbing — FS.mount(...) / FS.syncfs(true|false, cb) — and every existing caller of
// FS.syncfs (eden-loaderror.js's restore-then-reload, tools/safari-256z-authoring-live.js) keeps
// working with no change. The byte sink is INJECTABLE, which is what makes the op log testable
// without a browser: tools/headless-opfs-mirror-test.js supplies a node-`fs` sink whose
// fs.writeSync(fd, buf, 0, len, at) is the same primitive as the worker's write(buf, {at}).
(function (global) {
  'use strict';

  function available() {
    try {
      return typeof navigator !== 'undefined' && !!navigator.storage &&
             typeof navigator.storage.getDirectory === 'function' &&
             typeof Worker !== 'undefined';
    } catch (e) { return false; }
  }

  // -----------------------------------------------------------------------------------------
  // The FS type: MEMFS + an ordered op log
  // -----------------------------------------------------------------------------------------
  // `sink` must implement apply(ops, transferList) -> Promise, readAll() -> Promise<[{name,bytes}]>
  // and (optionally) close() -> Promise. Ops are plain objects, applied IN ORDER:
  //   {op:'create',   path}
  //   {op:'mkdir',    path}
  //   {op:'write',    path, at, data:Uint8Array}
  //   {op:'truncate', path, size}
  //   {op:'unlink',   path}
  //   {op:'rename',   path, to}
  // Order is the correctness property, not an implementation detail: B5's in-place save writes a
  // `.savejrnl` rollback journal BEFORE it touches the world file's destructive region and deletes
  // it after (docs/save-load.md). A backend that reordered those — as IDBFS's mtime reconcile is
  // free to — would persist a torn world with no journal to recover it.
  function fsType(sink, options) {
    var opts = options || {};
    var debug = !!opts.debug;

    var log = [];              // pending ops, in engine order
    var scheduled = 0;         // setTimeout id of the pending debounced flush
    var chain = Promise.resolve();
    var suspendDepth = 0;      // populate/migration writes must not echo back to the sink
    var mountpoint = '/';
    var stats = {
      flushes: 0, ops: 0, bytesWritten: 0,
      lastBytes: 0, lastOps: 0, lastMs: 0, maxBytes: 0, errors: 0, lastError: null
    };

    function relPath(abs) {
      if (mountpoint === '/') return abs.replace(/^\//, '');
      return abs.indexOf(mountpoint + '/') === 0 ? abs.slice(mountpoint.length + 1)
                                                 : abs.replace(/^\//, '');
    }
    function nodePath(node) { return relPath(FS.getPath(node)); }

    function schedule() {
      // Same reasoning as IDBFS.queuePersist: a save writes/renames several files inside one frame,
      // so batch everything that happens before the current task yields into ONE flush.
      if (scheduled || !log.length) return;
      scheduled = setTimeout(function () { scheduled = 0; flush(); }, 0);
    }

    function push(op) {
      if (suspendDepth) return;
      log.push(op);
      schedule();
    }

    // Bytes are NOT copied here — only the range. They are sliced out of node.contents at flush
    // time, which dedupes the engine's repeated stdio writes over the same region for free and
    // keeps exactly one copy of each written byte on the main thread.
    function recordWrite(node, at, len) {
      if (suspendDepth || len <= 0) return;
      var last = log[log.length - 1];
      if (last && last.t === 'w' && last.node === node &&
          at <= last.at + last.len && at + len >= last.at) {
        var end = Math.max(last.at + last.len, at + len);
        last.at = Math.min(last.at, at);
        last.len = end - last.at;
        schedule();
        return;
      }
      log.push({ t: 'w', node: node, path: nodePath(node), at: at, len: len });
      schedule();
    }

    // Every batch ends with a size op per file it touched, so the mirror's length always matches
    // MEMFS's usedBytes even if some MEMFS path shrinks a file without going through setattr
    // (`write` with canOwn replaces node.contents wholesale — reachable, if rare). Only emitted
    // for a node still reachable at its path: MEMFS's unlink leaves node.parent intact, so
    // FS.getPath on a deleted node still answers, and re-creating a just-deleted world would be a
    // real bug rather than a defensive extra.
    function sizeOpFor(node) {
      var abs, resolved;
      try { abs = FS.getPath(node); resolved = FS.lookupPath(abs).node; } catch (e) { return null; }
      if (resolved !== node) return null;
      return { op: 'truncate', path: relPath(abs), size: node.usedBytes | 0 };
    }

    function materialize() {
      var out = [], transfer = [], bytes = 0, touched = [];
      for (var i = 0; i < log.length; i++) {
        var e = log[i];
        if (e.t !== 'w') { out.push(e.rec); continue; }
        var node = e.node, contents = node && node.contents;
        if (!contents) continue;                   // unlinked later in this same batch
        var used = node.usedBytes | 0;
        var at = e.at, end = Math.min(e.at + e.len, used);
        if (end <= at) continue;
        var slice = contents.subarray ? contents.slice(at, end)
                                      : new Uint8Array(contents.slice(at, end));
        out.push({ op: 'write', path: e.path, at: at, data: slice });
        transfer.push(slice.buffer);
        bytes += slice.length;
        if (touched.indexOf(node) < 0) touched.push(node);
      }
      for (var j = 0; j < touched.length; j++) {
        var sz = sizeOpFor(touched[j]);
        if (sz) out.push(sz);
      }
      log.length = 0;
      return { ops: out, transfer: transfer, bytes: bytes };
    }

    function flush(cb) {
      if (scheduled) { clearTimeout(scheduled); scheduled = 0; }
      var batch = materialize();
      var run = chain.then(function () {
        if (!batch.ops.length) return;
        var t0 = (typeof performance !== 'undefined' ? performance.now() : Date.now());
        return Promise.resolve(sink.apply(batch.ops, batch.transfer)).then(function () {
          var t1 = (typeof performance !== 'undefined' ? performance.now() : Date.now());
          stats.flushes++;
          stats.ops += batch.ops.length;
          stats.bytesWritten += batch.bytes;
          stats.lastBytes = batch.bytes;
          stats.lastOps = batch.ops.length;
          stats.lastMs = t1 - t0;
          if (batch.bytes > stats.maxBytes) stats.maxBytes = batch.bytes;
          if (debug) {
            console.log('[eden-opfs] flush: ' + batch.ops.length + ' ops, ' + batch.bytes +
                        ' B, ' + (t1 - t0).toFixed(1) + ' ms');
          }
        });
      }).catch(function (err) {
        stats.errors++;
        stats.lastError = String((err && err.message) || err);
        throw err;
      });
      chain = run.catch(function () {});          // one failed batch must not wedge the queue
      if (cb) run.then(function () { cb(null); }, function (e) { cb(e); });
      return run;
    }

    // FS.syncfs(true, cb) — populate MEMFS from OPFS. Runs with recording suspended, or the
    // populate would immediately queue itself straight back out to the sink.
    function populate(mount, cb) {
      Promise.resolve(sink.readAll()).then(function (entries) {
        suspendDepth++;
        try {
          for (var i = 0; i < entries.length; i++) {
            var e = entries[i];
            var bytes = e.bytes instanceof Uint8Array ? e.bytes : new Uint8Array(e.bytes);
            FS.writeFile(mount.mountpoint + '/' + e.name, bytes);
          }
        } finally { suspendDepth--; }
        cb(null);
      }, function (err) { cb(err); });
    }

    var type = {
      // Same structure as library_idbfs.js's mount(): reuse MEMFS wholesale and inject tracking
      // into the cloned node_ops, propagating them to every node created under the mount.
      mount: function (mount) {
        mountpoint = mount.mountpoint;
        var mnt = MEMFS.mount(mount);
        var memfsOps = mnt.node_ops;
        mnt.node_ops = Object.assign({}, memfsOps);

        mnt.node_ops.mknod = function (parent, name, mode, dev) {
          var node = memfsOps.mknod(parent, name, mode, dev);
          node.node_ops = mnt.node_ops;
          node.memfs_stream_ops = node.stream_ops;
          node.stream_ops = Object.assign({}, node.stream_ops);
          node.stream_ops.write = function (stream, buffer, offset, length, position, canOwn) {
            var written = node.memfs_stream_ops.write(stream, buffer, offset, length, position, canOwn);
            if (written > 0) recordWrite(stream.node, position, written);
            return written;
          };
          // FS.mkdir routes through mknod too (FS.mkdir -> FS.mknod with S_IFDIR), which is why
          // there is no separate mkdir hook here.
          push({ rec: { op: FS.isDir(node.mode) ? 'mkdir' : 'create', path: nodePath(node) } });
          return node;
        };
        mnt.node_ops.setattr = function (node, attr) {
          var r = memfsOps.setattr(node, attr);
          if (attr.size !== undefined) {
            push({ rec: { op: 'truncate', path: nodePath(node), size: attr.size } });
          }
          return r;
        };
        mnt.node_ops.unlink = function (parent, name) {
          var p = relPath(FS.getPath(parent) + '/' + name);
          var r = memfsOps.unlink(parent, name);
          push({ rec: { op: 'unlink', path: p } });
          return r;
        };
        mnt.node_ops.rmdir = function (parent, name) {
          var p = relPath(FS.getPath(parent) + '/' + name);
          var r = memfsOps.rmdir(parent, name);
          push({ rec: { op: 'unlink', path: p } });
          return r;
        };
        mnt.node_ops.rename = function (old_node, new_dir, new_name) {
          var from = nodePath(old_node);
          var r = memfsOps.rename(old_node, new_dir, new_name);
          push({ rec: { op: 'rename', path: from, to: nodePath(old_node) } });
          return r;
        };
        return mnt;
      },

      syncfs: function (mount, doPopulate, cb) {
        if (doPopulate) return populate(mount, cb);
        flush(cb);
      },

      // Not part of the FS-type contract — the backend's own controls, reached through
      // EdenStorage/EdenOPFS rather than through FS.
      _eden: {
        stats: function () {
          return Object.assign({}, stats);
        },
        flush: flush,
        suspend: function (fn) {
          suspendDepth++;
          try { return fn(); } finally { suspendDepth--; }
        },
        pending: function () { return log.length; }
      }
    };
    return type;
  }

  // -----------------------------------------------------------------------------------------
  // The browser sink: a dedicated Worker holding the sync access handles
  // -----------------------------------------------------------------------------------------
  // NOTE for deploys: tools/build-dist.js content-hashes the public/*.js files referenced by a
  // literal src="…" in the HTML, which is this file but NOT the worker (nothing links it from
  // markup). So the worker keeps its plain name in dist/ — which is exactly what this `new Worker`
  // call needs — and relies on the service worker's network-first policy for freshness rather than
  // on a hashed filename. Don't "fix" that by hashing it without also rewriting this string.
  function makeWorkerSink(options) {
    var opts = options || {};
    var worker = new Worker(opts.workerUrl || 'eden-opfs-worker.js');
    var nextId = 1;
    var waiting = Object.create(null);
    var dead = null;

    worker.onmessage = function (ev) {
      var m = ev.data || {};
      var w = waiting[m.id];
      if (!w) return;
      delete waiting[m.id];
      if (m.ok) w.resolve(m.result);
      else w.reject(new Error(m.error || 'eden-opfs worker error'));
    };
    worker.onerror = function (ev) {
      dead = new Error('eden-opfs worker failed: ' + (ev.message || 'unknown'));
      Object.keys(waiting).forEach(function (k) { waiting[k].reject(dead); delete waiting[k]; });
    };

    function call(msg, transfer) {
      if (dead) return Promise.reject(dead);
      return new Promise(function (resolve, reject) {
        var id = nextId++;
        waiting[id] = { resolve: resolve, reject: reject };
        msg.id = id;
        worker.postMessage(msg, transfer || []);
      });
    }

    return {
      init: function () { return call({ cmd: 'init', dir: opts.dir || 'documents' }); },
      apply: function (ops, transfer) { return call({ cmd: 'apply', ops: ops }, transfer); },
      readAll: function () { return call({ cmd: 'readAll' }); },
      list: function () { return call({ cmd: 'list' }); },
      wipe: function () { return call({ cmd: 'wipe' }); },
      close: function () {
        return call({ cmd: 'close' }).catch(function () {}).then(function () { worker.terminate(); });
      }
    };
  }

  var EdenOPFS = {
    available: available,
    fsType: fsType,
    makeWorkerSink: makeWorkerSink
  };

  if (typeof module === 'object' && module.exports) module.exports = EdenOPFS;
  global.EdenOPFS = EdenOPFS;
})(typeof globalThis !== 'undefined' ? globalThis : this);
