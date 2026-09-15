// eden-opfs-worker.js — the byte sink for eden-opfs.js (ROADMAP Phase C / C2, 2026-09-02).
//
// Exists because `FileSystemSyncAccessHandle` — the ONLY OPFS API with a true random-access
// partial write — is exposed in Workers and nowhere else. The main thread records which byte
// ranges the engine dirtied (eden-opfs.js) and posts them here; this file applies them with
// write(buf, {at}). Nothing else in the port runs in a worker on the JS side, so keep this file
// self-contained: it has no imports, touches no DOM, and never talks to the engine.
//
// Protocol (one message in, one message out, matched by `id`):
//   {cmd:'init',    dir}          -> {backend:'opfs'}          acquires the OPFS directory
//   {cmd:'apply',   ops}          -> {bytes, ops}              applies ops IN ORDER, then flushes
//   {cmd:'readAll'}               -> [{name, bytes}]           whole-file read, buffers transferred
//   {cmd:'list'}                  -> [{name, size}]
//   {cmd:'wipe'}                  -> {removed}                 test/migration escape hatch
//   {cmd:'close'}                 -> {}                        releases every handle's lock
//
// A sync access handle takes an EXCLUSIVE lock for as long as it is open, so handles are cached
// per file for the session (opening one per op costs a round trip through the storage layer) and
// released on 'close'. A second tab on the same origin therefore cannot acquire them — `init`
// fails, and eden-storage.js falls back to IDBFS for that tab rather than running two writers
// against one world file.
'use strict';

var rootDir = null;      // FileSystemDirectoryHandle for the mirror directory
var handles = new Map(); // path -> FileSystemSyncAccessHandle
var totalBytes = 0;
var locked = false;      // another tab holds the sync-handle locks — read-only for us

// Safari shipped OPFS before the sync-handle methods were specified as synchronous, and some
// builds still return promises from them. Awaiting a non-promise is free, so every call goes
// through here rather than assuming one shape.
async function maybe(v) { return v && typeof v.then === 'function' ? await v : v; }

async function dirFor(path, create) {
  var parts = path.split('/').filter(Boolean);
  var dir = rootDir;
  for (var i = 0; i < parts.length - 1; i++) {
    dir = await dir.getDirectoryHandle(parts[i], { create: !!create });
  }
  return { dir: dir, name: parts[parts.length - 1] };
}

async function handleFor(path, create) {
  var h = handles.get(path);
  if (h) return h;
  var loc = await dirFor(path, create);
  var fh = await loc.dir.getFileHandle(loc.name, { create: !!create });
  h = await fh.createSyncAccessHandle();
  handles.set(path, h);
  return h;
}

async function closeHandle(path) {
  var h = handles.get(path);
  if (!h) return;
  handles.delete(path);
  try { await maybe(h.close()); } catch (e) { /* already gone */ }
}

async function removePath(path) {
  await closeHandle(path);
  var loc = await dirFor(path, false);
  try { await loc.dir.removeEntry(loc.name, { recursive: true }); } catch (e) { /* ENOENT */ }
}

// OPFS has no portable rename. FileSystemHandle.move() is the fast path where it exists (Chrome
// 111+, and it is a metadata operation); everywhere else this is a chunked copy + remove, which is
// only ever hit by the BELOW-threshold save path (a 64z world's `.savetmp` commit — the engine
// already copied that whole file itself, so the copy here is not a new class of cost) and by
// FileManager::convertWorldTo64.
const COPY_CHUNK = 4 * 1024 * 1024;
async function renamePath(from, to) {
  await closeHandle(from);
  await closeHandle(to);
  var src = await dirFor(from, false);
  var dst = await dirFor(to, true);
  var srcFile;
  try { srcFile = await src.dir.getFileHandle(src.name, { create: false }); }
  catch (e) { return; }                            // source vanished — nothing to move
  if (typeof srcFile.move === 'function') {
    try {
      await srcFile.move(dst.dir, dst.name);
      return;
    } catch (e) { /* fall through to copy */ }
  }
  var sh = await srcFile.createSyncAccessHandle();
  var dstFile = await dst.dir.getFileHandle(dst.name, { create: true });
  var dh = await dstFile.createSyncAccessHandle();
  try {
    var size = await maybe(sh.getSize());
    await maybe(dh.truncate(0));
    var buf = new Uint8Array(Math.min(COPY_CHUNK, size || 1));
    for (var at = 0; at < size; at += COPY_CHUNK) {
      var n = Math.min(COPY_CHUNK, size - at);
      var view = buf.subarray(0, n);
      await maybe(sh.read(view, { at: at }));
      await maybe(dh.write(view, { at: at }));
    }
    await maybe(dh.flush());
  } finally {
    try { await maybe(sh.close()); } catch (e) {}
    try { await maybe(dh.close()); } catch (e) {}
  }
  try { await src.dir.removeEntry(src.name); } catch (e) {}
}

async function applyOps(ops) {
  if (locked) throw new Error('OPFS is locked by another tab — writes are disabled here');
  var bytes = 0;
  var touched = new Set();
  for (var i = 0; i < ops.length; i++) {
    var o = ops[i];
    switch (o.op) {
      case 'mkdir':
        await dirFor(o.path + '/x', true);
        break;
      case 'create':
        await handleFor(o.path, true);
        break;
      case 'write': {
        var h = await handleFor(o.path, true);
        var data = o.data instanceof Uint8Array ? o.data : new Uint8Array(o.data);
        await maybe(h.write(data, { at: o.at }));
        bytes += data.length;
        touched.add(h);
        break;
      }
      case 'truncate': {
        var th = await handleFor(o.path, true);
        // Only ever shrink-or-grow to the authoritative MEMFS size; a no-op truncate to the
        // current size is cheap and keeps the branch simple.
        await maybe(th.truncate(o.size));
        touched.add(th);
        break;
      }
      case 'unlink':
        await removePath(o.path);
        break;
      case 'rename':
        await renamePath(o.path, o.to);
        break;
      default:
        throw new Error('unknown op ' + o.op);
    }
  }
  for (var h2 of touched) { try { await maybe(h2.flush()); } catch (e) {} }
  totalBytes += bytes;
  return { bytes: bytes, ops: ops.length };
}

async function listAll() {
  var out = [];
  for await (var entry of rootDir.values()) {
    if (entry.kind !== 'file') continue;
    var h = await handleFor(entry.name, false);
    out.push({ name: entry.name, size: await maybe(h.getSize()) });
  }
  return out;
}

async function readAll() {
  var out = [], transfer = [];
  for await (var entry of rootDir.values()) {
    if (entry.kind !== 'file') continue;
    var buf;
    if (locked) {
      // No sync handle available (another tab owns the locks). getFile() needs no lock, so the
      // worlds are still READABLE here — eden-storage.js turns that into a session-only,
      // clearly-warned mount rather than showing the player an empty world list.
      var file = await entry.getFile();
      buf = await file.arrayBuffer();
    } else {
      var h = await handleFor(entry.name, false);
      var size = await maybe(h.getSize());
      buf = new ArrayBuffer(size);
      if (size) await maybe(h.read(new Uint8Array(buf), { at: 0 }));
    }
    out.push({ name: entry.name, bytes: new Uint8Array(buf) });
    transfer.push(buf);
  }
  return { value: out, transfer: transfer };
}

async function wipe() {
  var names = [];
  for await (var entry of rootDir.values()) names.push(entry.name);
  for (var i = 0; i < names.length; i++) await removePath(names[i]);
  return { removed: names.length };
}

async function closeAll() {
  var paths = Array.from(handles.keys());
  for (var i = 0; i < paths.length; i++) await closeHandle(paths[i]);
  return {};
}

async function handle(msg) {
  switch (msg.cmd) {
    case 'init': {
      var root = await navigator.storage.getDirectory();
      rootDir = await root.getDirectoryHandle(msg.dir || 'documents', { create: true });
      // Prove a sync access handle can actually be acquired HERE, at init, rather than
      // discovering it on the first save. Two distinct failures, and they need different
      // answers: no sync handles at all (old/limited browser) is a hard failure and the caller
      // falls back to IDBFS; a LOCK held by another tab is not — the worlds are still readable,
      // so report `locked` and let eden-storage.js mount read-only with a warning instead of
      // showing an empty world list or running a second writer against the same files.
      if (typeof FileSystemFileHandle === 'undefined' ||
          !FileSystemFileHandle.prototype.createSyncAccessHandle) {
        throw new Error('no createSyncAccessHandle in this worker');
      }
      var probe = await rootDir.getFileHandle('.eden-opfs-probe', { create: true });
      try {
        var ph = await probe.createSyncAccessHandle();
        await maybe(ph.close());
        try { await rootDir.removeEntry('.eden-opfs-probe'); } catch (e) {}
      } catch (e) {
        locked = true;
      }
      return { value: { backend: 'opfs', locked: locked } };
    }
    case 'apply':   return { value: await applyOps(msg.ops || []) };
    case 'readAll': return await readAll();
    case 'list':    return { value: await listAll() };
    case 'wipe':    return { value: await wipe() };
    case 'close':   return { value: await closeAll() };
    default: throw new Error('unknown cmd ' + msg.cmd);
  }
}

// Messages are serialised: `handle` is async, so without this a second message could start
// running between two awaits of the first, interleaving (say) a readAll with a half-applied
// batch. The main thread already chains its own flushes, but that is its invariant, not ours.
var queue = Promise.resolve();
self.onmessage = function (ev) {
  var msg = ev.data || {};
  var id = msg.id;
  queue = queue.then(function () {
    return handle(msg).then(function (r) {
      self.postMessage({ id: id, ok: true, result: r.value }, r.transfer || []);
    }, function (err) {
      self.postMessage({ id: id, ok: false, error: String((err && err.message) || err) });
    });
  });
};
