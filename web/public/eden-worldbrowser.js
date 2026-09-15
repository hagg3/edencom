// eden-worldbrowser.js — data layer for the "Get Worlds" screen (eden-menu.js's renderGetWorlds).
// Requires: window.EdenStorage (importFile). Publishes: window.EdenWorldBrowser.
//
// Talks to https://hagg3.github.io/edenarchive/ — a static, community-run archive of published
// Eden worlds (owned by the same person who maintains this fork; see WORKING/ for context). There
// is no API: the whole catalog is one static JSON manifest, and each world's download is a
// GitHub-Pages-hosted zip derived from the manifest's `filename` field. Both are served with
// `Access-Control-Allow-Origin: *`, so this can fetch() them directly with no proxy.
//
// Manifest shape (one entry per world; verified 2026-08-01 against the live site):
//   { filename, worldname, publishdate, archivedate, filesize, author, tags: [...], url }
// `filename` is "<id>.eden" — the id also derives both asset URLs:
//   download:  assets/worldfiles/<id>/<id>.eden.zip
//   preview:   assets/worldfiles/<id>/<id>.eden.png   (frequently 404 — the site's own intro warns
//              most worlds published in the last 1-2 years have none; callers must handle that)
//
// DOWNLOAD FORMAT: a standard single-entry PK zip, Deflate or Stored, wrapping the raw .eden file.
// This file's own unzip walks the End Of Central Directory record rather than trusting the local
// file header's sizes directly — those are legitimately zero under the streaming/data-descriptor
// bit, and the archive site's own text warns some entries are "double-compressed" (a zip inside a
// zip), so extraction recurses until the payload stops looking like an archive.
//
// DEEP LINK: eden-st.html?playworld=<id> (id = a manifest entry's `filename` minus ".eden", e.g.
// "1315348100") skips the Get Worlds screen entirely — eden-menu.js's startAutoPlay() reuses a
// matching local save if one exists, otherwise calls downloadAndImport() below itself and then
// plays the result. This is what backs a "Play in browser" link/button on the archive site's own
// world pages and search results.
(function () {
  'use strict';

  var MANIFEST_URL = 'https://hagg3.github.io/edenarchive/assets/data/worlds.json';
  var WORLDFILES_BASE = 'https://hagg3.github.io/edenarchive/assets/worldfiles/';
  var ZIP_LOCAL_SIG = 0x04034b50;
  var ZIP_CENTRAL_SIG = 0x02014b50;
  var ZIP_EOCD_SIG = 0x06054b50;
  var GZIP_MAGIC = [0x1f, 0x8b];

  var manifestCache = null;
  var manifestPromise = null;

  // ---------------------------------------------------------------------------------------------
  // Manifest
  // ---------------------------------------------------------------------------------------------
  function fetchManifest(cb) {
    if (manifestCache) { cb(null, manifestCache); return; }
    if (!manifestPromise) {
      manifestPromise = fetch(MANIFEST_URL).then(function (r) {
        if (!r.ok) throw new Error('archive returned HTTP ' + r.status);
        return r.json();
      }).then(function (list) {
        manifestCache = Array.isArray(list) ? list : [];
        return manifestCache;
      }).catch(function (err) {
        manifestPromise = null;   // let a retry actually retry instead of replaying the same rejection
        throw err;
      });
    }
    manifestPromise.then(function (list) { cb(null, list); }, function (err) { cb(err); });
  }

  function entryMatches(entry, query, tag) {
    if (tag && (entry.tags || []).indexOf(tag) < 0) return false;
    if (!query) return true;
    var q = query.toLowerCase();
    if (entry.worldname && entry.worldname.toLowerCase().indexOf(q) >= 0) return true;
    if (entry.author && entry.author.toLowerCase().indexOf(q) >= 0) return true;
    // A handful of manifest entries have malformed tags (a stray `true`/number instead of a
    // string — e.g. "Brepa V2"'s tags include `true`) — coerce rather than assume string, or one
    // bad entry's tags.some() throws and silently kills the whole search (not just that row).
    return (entry.tags || []).some(function (t) { return String(t).toLowerCase().indexOf(q) >= 0; });
  }

  function search(list, query, tag) {
    return (list || []).filter(function (e) { return entryMatches(e, query, tag); });
  }

  function allTags(list) {
    var set = {};
    (list || []).forEach(function (e) { (e.tags || []).forEach(function (t) { set[t] = true; }); });
    return Object.keys(set).sort();
  }

  function idFor(entry) {
    return String((entry && entry.filename) || '').replace(/\.eden$/i, '');
  }

  function downloadUrl(entry) {
    var id = idFor(entry);
    return WORLDFILES_BASE + id + '/' + id + '.eden.zip';
  }

  function previewUrl(entry) {
    var id = idFor(entry);
    return WORLDFILES_BASE + id + '/' + id + '.eden.png';
  }

  // ---------------------------------------------------------------------------------------------
  // Zip extraction (single-entry archive -> raw bytes)
  // ---------------------------------------------------------------------------------------------
  function u32(bytes, off) {
    return new DataView(bytes.buffer, bytes.byteOffset + off, 4).getUint32(0, true);
  }
  function u16(bytes, off) {
    return new DataView(bytes.buffer, bytes.byteOffset + off, 2).getUint16(0, true);
  }

  // Scans backward for the End Of Central Directory record. Comment is near-always empty for
  // these single-file archives, but the full 65KB max-comment window is scanned anyway since it's
  // one pass over a small buffer.
  function findEOCD(bytes) {
    var maxComment = 65557; // 22-byte record + max 65535-byte comment
    var start = Math.max(0, bytes.length - maxComment);
    for (var i = bytes.length - 22; i >= start; i--) {
      if (u32(bytes, i) === ZIP_EOCD_SIG) return i;
    }
    return -1;
  }

  function inflateRaw(bytes) {
    if (typeof DecompressionStream === 'undefined') {
      return Promise.reject(new Error(
        'This browser cannot decompress downloaded worlds (no DecompressionStream support).'));
    }
    var stream = new Blob([bytes]).stream().pipeThrough(new DecompressionStream('deflate-raw'));
    return new Response(stream).arrayBuffer().then(function (buf) { return new Uint8Array(buf); });
  }

  function gunzip(bytes) {
    var stream = new Blob([bytes]).stream().pipeThrough(new DecompressionStream('gzip'));
    return new Response(stream).arrayBuffer().then(function (buf) { return new Uint8Array(buf); });
  }

  /**
   * Extracts and decompresses the first entry of a zip archive. Prefers the central directory's
   * compressed-size field over the local file header's — the header's is legitimately 0 when the
   * archiver used a streaming data-descriptor (general-purpose bit 3), which the central directory
   * is immune to.
   */
  function unzipFirstEntry(bytes) {
    var method, compSize, localOffset;
    var eocd = findEOCD(bytes);
    var cdOffset = eocd >= 0 ? u32(bytes, eocd + 16) : -1;
    if (cdOffset >= 0 && cdOffset < bytes.length - 4 && u32(bytes, cdOffset) === ZIP_CENTRAL_SIG) {
      method = u16(bytes, cdOffset + 10);
      compSize = u32(bytes, cdOffset + 20);
      localOffset = u32(bytes, cdOffset + 42);
    } else {
      // No usable central directory — fall back to the local header directly. Only correct for a
      // non-streamed archive, but every tool known to produce these files writes real sizes there.
      if (u32(bytes, 0) !== ZIP_LOCAL_SIG) return Promise.reject(new Error('not a zip archive'));
      method = u16(bytes, 8);
      compSize = u32(bytes, 18);
      localOffset = 0;
    }
    if (u32(bytes, localOffset) !== ZIP_LOCAL_SIG) {
      return Promise.reject(new Error('corrupt zip: bad local file header'));
    }
    var nameLen = u16(bytes, localOffset + 26);
    var extraLen = u16(bytes, localOffset + 28);
    var dataStart = localOffset + 30 + nameLen + extraLen;
    var compBytes = bytes.subarray(dataStart, dataStart + compSize);
    if (method === 0) return Promise.resolve(compBytes.slice());
    if (method === 8) return inflateRaw(compBytes);
    return Promise.reject(new Error('unsupported zip compression method ' + method));
  }

  /** Downloaded archive bytes -> raw .eden bytes, unwrapping nested zip/gzip layers as needed. */
  function extractEden(bytes, depth) {
    depth = depth || 0;
    if (depth > 4) return Promise.reject(new Error('archive nested too deeply'));
    if (bytes.length >= 4 && u32(bytes, 0) === ZIP_LOCAL_SIG) {
      return unzipFirstEntry(bytes).then(function (inner) { return extractEden(inner, depth + 1); });
    }
    if (bytes.length >= 2 && bytes[0] === GZIP_MAGIC[0] && bytes[1] === GZIP_MAGIC[1]) {
      return gunzip(bytes).then(function (inner) { return extractEden(inner, depth + 1); });
    }
    return Promise.resolve(bytes);
  }

  // ---------------------------------------------------------------------------------------------
  // Download + import
  // ---------------------------------------------------------------------------------------------
  function readWithProgress(resp, onProgress) {
    var total = Number(resp.headers.get('Content-Length')) || 0;
    if (!resp.body || !onProgress) return resp.arrayBuffer();
    var reader = resp.body.getReader();
    var chunks = [];
    var received = 0;
    return new Promise(function (resolve, reject) {
      function pump() {
        reader.read().then(function (step) {
          if (step.done) {
            var out = new Uint8Array(received);
            var off = 0;
            chunks.forEach(function (c) { out.set(c, off); off += c.length; });
            resolve(out.buffer);
            return;
          }
          chunks.push(step.value);
          received += step.value.length;
          if (total) onProgress(Math.min(99, Math.round((received / total) * 100)));
          pump();
        }).catch(reject);
      }
      pump();
    });
  }

  /**
   * Downloads `entry`'s zip, extracts the .eden payload, and hands it to
   * EdenStorage.importFile — the same path drag-and-drop import uses, so a downloaded world shows
   * up in Load World exactly like any other save. cb(ok, errorMessageOrNull).
   */
  function downloadAndImport(entry, opts, cb) {
    opts = opts || {};
    fetch(downloadUrl(entry))
      .then(function (resp) {
        if (!resp.ok) throw new Error('download failed: HTTP ' + resp.status);
        return readWithProgress(resp, opts.onProgress);
      })
      .then(function (buf) { return extractEden(new Uint8Array(buf)); })
      .then(function (edenBytes) {
        if (opts.onProgress) opts.onProgress(100);
        var name = idFor(entry) + '.eden';
        var file = new File([edenBytes], name, { type: 'application/octet-stream' });
        window.EdenStorage.importFile(file, cb);
      })
      .catch(function (err) { cb && cb(false, (err && err.message) || String(err)); });
  }

  window.EdenWorldBrowser = {
    fetchManifest: fetchManifest,
    search: search,
    allTags: allTags,
    idFor: idFor,
    downloadUrl: downloadUrl,
    previewUrl: previewUrl,
    downloadAndImport: downloadAndImport,
  };
})();
