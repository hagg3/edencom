// eden-loading.js — a real loading/progress UI (perf-audit-dazzling-munching-bengio.md row 11:
// Requires: window.EdenUI, Module.dataFileDownloads. Publishes: window.EdenLoading. See docs/ui.md's
// dependency graph (audit I2).
// "Minute-plus black screen on mobile" — the old UI was a 12px monospace diagnostic line plus a
// black canvas, meant for developers, not players).
//
// Deliberately self-contained and additive: it does NOT replace eden-st.html's existing
// `#eden-status` diagnostic line (still useful for debugging a stuck/failed boot — see
// RESUME-HERE's "Watch out for" section) — it draws its own overlay on top and hides itself once
// the engine is confirmed alive.
//
// Byte-level progress comes from two places, both already-existing network requests — no new
// fetches, no wrapping of `window.fetch` (a global fetch monkeypatch was considered and rejected:
// too easy to subtly break the Emscripten wasm/data loaders that also call `fetch`, for a page
// that only actually needs progress on two specific large assets):
//   1. `eden.data` (~2-8 MB asset package): Emscripten's own generated loader already tracks this
//      byte-for-byte in `Module.dataFileDownloads['eden.data']` (see build-st/eden.js's
//      `loadPackage`/`fetchRemotePackage` — this is stock Emscripten output, not project code).
//      Polled, not pushed — no hook needed on the Emscripten side.
//   2. `Eden.eden` (the ~52 MB default-world map, fetched separately per pass 30 — see
//      src/seam/js/eden_default_world.pre.js): that file now reports progress through
//      `window.EdenLoading.setEdenFileProgress(loaded, total)` as it streams the response body,
//      the one place this module needed a real (small, seam-owned) code change elsewhere.
// Anything else in the boot sequence (wasm compile, the individual small per-texture/audio
// "requests" that are really just synchronous slices of the already-downloaded eden.data package)
// has no useful byte-level signal and is folded into a small fixed head/tail weight instead of
// being tracked precisely — see WEIGHT_* below.
(function () {
  'use strict';

  var WEIGHT_HEAD = 0.05; // wasm fetch+compile start, before either byte-tracked download begins
  var WEIGHT_TAIL = 0.05; // main()/World::World() running after both downloads finish

  var S = {
    root: null,
    bar: null,
    label: null,
    sublabel: null,
    shown: false,
    hidden: false,
    startTime: 0,
    slowTimer: null,
    edenFile: { loaded: 0, total: 0 },
  };

  // This screen is the one surface that must render BEFORE anything else exists — no wasm module,
  // no filesystem, so none of the engine's art is reachable and the background can only be a flat
  // colour. It still uses the design system's tokens, pixel display face and progress component,
  // so the very first thing the player sees already looks like the game. The rules below are the
  // few things the shared stylesheet cannot express: the full-screen boot layout and the
  // indeterminate sweep (there is no indeterminate state anywhere else in the system).
  var CSS = [
    '#eden-loading{position:fixed;inset:0;z-index:var(--eden-z-alert);display:flex;',
      'flex-direction:column;align-items:center;justify-content:center;gap:calc(14 * var(--u));',
      'background:var(--eden-gray-900);color:var(--eden-gray-150);',
      'font-family:var(--eden-font-body);font-size:var(--eden-text-body-md);text-align:center;',
      'transition:opacity .35s ease;padding:0 20px;box-sizing:border-box;}',
    '#eden-loading.eden-loading-hidden{opacity:0;pointer-events:none;}',
    '#eden-loading .eden-loading-title{font-family:var(--eden-font-display);',
      'font-size:var(--eden-text-display-lg);line-height:1;color:var(--eden-white);',
      'text-shadow:calc(2 * var(--u)) calc(2 * var(--u)) 0 var(--eden-black);}',
    '#eden-loading .eden-progress{width:min(calc(320 * var(--u)),80vw);}',
    // Indeterminate: a fixed-width fill swept across the track, for the phase before any byte
    // total is known. Deliberately the system's only continuous animation.
    '#eden-loading .eden-progress.indeterminate .eden-progress__fill{',
      'width:35% !important;animation:eden-loading-slide 1.1s ease-in-out infinite;}',
    '@keyframes eden-loading-slide{0%{margin-left:-35%;}100%{margin-left:100%;}}',
    '#eden-loading .eden-loading-sub{color:var(--eden-gray-300);',
      'font-size:var(--eden-text-body-sm);min-height:14px;}',
    '@media (prefers-reduced-motion:reduce){',
      '#eden-loading{transition:none;}',
      '#eden-loading .eden-progress.indeterminate .eden-progress__fill{animation:none;}}',
  ].join('');

  function injectCSS() {
    // The shared stylesheet supplies every --eden-* token the rules above reference, and eden-ui.js
    // supplies `--u`. Both are loaded before this file (see eden-st.html's script order), but ask
    // explicitly so this file is not silently dependent on that ordering.
    if (window.EdenUI) window.EdenUI.ensureCSS();
    if (document.getElementById('eden-loading-css')) return;
    var el = document.createElement('style');
    el.id = 'eden-loading-css';
    el.textContent = CSS;
    document.head.appendChild(el);
  }

  function build() {
    injectCSS();
    var root = document.createElement('div');
    root.id = 'eden-loading';

    var title = document.createElement('div');
    title.className = 'eden-loading-title';
    title.textContent = 'EDEN';

    var track = document.createElement('div');
    track.className = 'eden-progress indeterminate';   // shared component — see eden-ui.css
    track.setAttribute('role', 'progressbar');
    track.setAttribute('aria-label', 'Loading Eden');
    var fill = document.createElement('div');
    fill.className = 'eden-progress__fill';
    track.appendChild(fill);

    var label = document.createElement('div');
    label.className = 'eden-loading-sub';
    label.textContent = 'Starting…';

    var sublabel = document.createElement('div');
    sublabel.className = 'eden-loading-sub';

    root.appendChild(title);
    root.appendChild(track);
    root.appendChild(label);
    root.appendChild(sublabel);
    document.body.appendChild(root);

    S.root = root;
    S.track = track;
    S.bar = fill;
    S.label = label;
    S.sublabel = sublabel;
  }

  function show() {
    if (S.shown) return;
    S.shown = true;
    S.startTime = performance.now();
    build();
    // Most cold loads clear this in a few seconds; only reassure the player it hasn't hung if
    // it's genuinely taking a while (slow connection, cold cache, mobile).
    S.slowTimer = setTimeout(function () {
      if (!S.hidden && S.sublabel) S.sublabel.textContent = 'Still working — large first-time download…';
    }, 15000);
  }

  function setDeterminate(fraction, text, sub) {
    if (!S.shown) show();
    if (!S.root) return;
    S.track.classList.remove('indeterminate');
    var pct = Math.max(0, Math.min(100, fraction * 100)).toFixed(0);
    S.bar.style.width = pct + '%';
    S.track.setAttribute('aria-valuenow', pct);
    if (text) S.label.textContent = text;
    if (sub !== undefined) S.sublabel.textContent = sub;
  }

  function mb(n) { return (n / (1024 * 1024)).toFixed(1); }

  // Called on a light interval (see startPolling below) rather than from an Emscripten hook —
  // `Module.dataFileDownloads` is a plain object Emscripten's stock loader writes into with no
  // change-event, so polling it is the correct approach, not a design shortcut.
  function tick() {
    if (S.hidden) return;
    var M = window.Module;
    var dataDL = M && M.dataFileDownloads && M.dataFileDownloads['eden.data'];
    var ef = S.edenFile;

    var trackers = [];
    if (dataDL && dataDL.total > 0) trackers.push(dataDL);
    if (ef.total > 0) trackers.push(ef);

    if (trackers.length === 0) {
      if (!S.shown) show();
      setDeterminate(0, 'Starting…');
      S.track && S.track.classList.add('indeterminate');
      return;
    }

    var loaded = 0, total = 0;
    for (var i = 0; i < trackers.length; i++) { loaded += trackers[i].loaded; total += trackers[i].total; }
    // Head/tail weighting: the byte-tracked span is treated as the middle (1-HEAD-TAIL) of the
    // bar, so the bar doesn't sit at a misleading 0% during wasm compile or 100% while main() is
    // still bringing up the World.
    var innerFraction = total > 0 ? loaded / total : 0;
    var overall = WEIGHT_HEAD + innerFraction * (1 - WEIGHT_HEAD - WEIGHT_TAIL);

    var parts = [];
    if (dataDL && dataDL.total > 0) parts.push('assets ' + mb(dataDL.loaded) + '/' + mb(dataDL.total) + ' MB');
    if (ef.total > 0) parts.push('world data ' + mb(ef.loaded) + '/' + mb(ef.total) + ' MB');
    setDeterminate(overall, 'Downloading…', parts.join(' · '));
  }

  var pollHandle = null;
  function startPolling() {
    if (pollHandle) return;
    pollHandle = setInterval(tick, 150);
    tick();
  }

  function markReady() {
    if (S.hidden) return;
    S.hidden = true;
    if (pollHandle) { clearInterval(pollHandle); pollHandle = null; }
    if (S.slowTimer) { clearTimeout(S.slowTimer); S.slowTimer = null; }
    if (!S.root) return;
    setDeterminate(1, 'Ready');
    S.root.classList.add('eden-loading-hidden');
    setTimeout(function () { if (S.root && S.root.parentNode) S.root.parentNode.removeChild(S.root); }, 400);
  }

  window.EdenLoading = {
    start: function () { show(); startPolling(); },
    setEdenFileProgress: function (loaded, total) {
      S.edenFile.loaded = loaded;
      S.edenFile.total = total;
      if (!S.shown) show();
    },
    markReady: markReady,
  };
})();
