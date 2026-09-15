// eden-host.js — audit row 28/C5: the shared boot-time primitives every other extracted module
// (eden-viewport.js, eden-keybinds.js, eden-hotbar.js, eden-input.js) and the remaining inline
// script in eden-st.html read or write. MUST be the first of the five eden-st.html split files to
// load — everything below is either a `const` computed from `document`/`location` alone (safe at
// any point) or a value another file's TOP-LEVEL code (not just a later-firing callback) touches
// immediately, e.g. eden-viewport.js's crosshair setup calls `updateCrosshairPosition()` at load
// time, which reads `canvasEl` synchronously.
//
// This split relies on a real, load-bearing fact about classic (non-module, non-defer/async)
// `<script>` tags: every one of them, inline or `src=`, shares ONE top-level scope — a `let`/
// `const` declared in an earlier tag is visible to a later tag exactly as if they were one file.
// (`eden-input.js`'s own header explains the one place this cuts the other way: a function body
// can reference a `let` declared LATER in the same or a later file, because that function only
// ever runs once the whole document's scripts have finished their synchronous first pass — i.e.
// after every declaration has already run. Only TOP-LEVEL code, executed immediately as a script
// loads, must not read a not-yet-declared binding.) Splitting the old single inline script into
// ordered `<script src>` tags does not change any of this, it only makes "who declares what, and
// who must load first" an explicit, documented contract instead of one 1300-line file's implicit
// ordering — which is the actual thing audit row 28/C5 asked for ("extraction must make those
// dependencies explicit").
'use strict';

const statusEl = document.getElementById('eden-status');
const canvasEl = document.getElementById('eden-canvas');

// Flipped true by eden-st.html's Module.onRuntimeInitialized. Read by every other split file's
// event handlers/tick functions to gate on "is the wasm module (and therefore the World) up yet".
let moduleReady = false;
function callIfReady(fn) { if (moduleReady) fn(); }

// Phase 6 wheel-jump pending-end flag — written by eden-input.js's wheelJumpPulse(), consumed one
// frame later by eden-st.html's trackCursorNeed(). Declared here (not in eden-input.js) because
// both files need it and neither is a natural owner of the other.
let jumpPulsePending = false;

// Small transient overlay for state changes that have no on-screen indicator of their own (fly
// mode, block preview) — the engine's HUD is all custom GL, so there is nowhere to put this
// in-world. Used by eden-input.js (action dispatch) and EdenGamepad's onConnect (also wired from
// eden-input.js).
let toastTimer = null;
function showToast(text) {
  let el = document.getElementById('eden-toast');
  if (!el) {
    el = document.createElement('div');
    el.id = 'eden-toast';
    el.className = 'eden-toast';   // styled in eden-ui.css
    // Announced rather than silently painted: these messages (fly mode on/off, block preview) are
    // the ONLY feedback those toggles give, so a screen-reader user would otherwise get nothing at
    // all from the keyboard shortcut.
    el.setAttribute('role', 'status');
    el.setAttribute('aria-live', 'polite');
    document.body.appendChild(el);
  }
  el.textContent = text;
  el.style.opacity = '1';
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { el.style.opacity = '0'; }, 1400);
}

// Q1 (dazzling-munching-bengio.md perf audit): pick which configured build to run.
// `?build=rel` -> build-rel/ (Release, -O2 -fno-strict-aliasing, no DWARF — see CMakeLists.txt).
// Default (no param, or anything else) -> build-st/ (Debug, -O0 -g -gsource-map), unchanged
// behaviour from every prior pass. Kept as a query param rather than a second HTML file so both
// builds stay behind one set of DOM/input/settings wiring instead of two copies drifting.
// Relative (not root-absolute) so this page still resolves build-rel/build-st correctly when
// served under a URL prefix (e.g. a GitHub Pages project site at /<repo>/public/eden-st.html).
//
// `?build=thr` (audit row 36/C1, pass 63) -> build-thr/, the in-development THREADED build
// (`emcmake cmake -B build-thr -DCMAKE_BUILD_TYPE=Debug -DEDEN_THREADED=ON`). Its wasm memory is
// a SharedArrayBuffer, so the page must be cross-origin isolated or instantiation fails outright
// — tools/serve.js sends the required COOP/COEP headers, `python3 -m http.server` does not, and
// GitHub Pages CANNOT (see tools/build-dist.js's note), which is what public/eden-coi.js exists
// to work around.
//
// `let`, not `const`: edenSettleBuildDir() below can downgrade it. Read LAZILY everywhere (the
// eden.js <script src> and Module.locateFile are both evaluated after that call), so the
// downgrade is seen by everything that matters.
const EDEN_BUILD_PARAM = new URLSearchParams(location.search).get('build');
// ROADMAP Phase M / M0 (temporary — revert with the probe): measurement trees the memory probe
// drives. The *-thr* ones are threaded and need cross-origin isolation just like ?build=thr.
const EDEN_M0_BUILDS = {
  relwdiag: '../build-relwdiag/', 'relwdiag-cap512': '../build-relwdiag-cap512/',
  relthr: '../build-relthr/', 'relthr-cap512': '../build-relthr-cap512/',
};
const EDEN_M0_THREADED = new Set(['relthr', 'relthr-cap512']);
let EDEN_BUILD_DIR = EDEN_BUILD_PARAM === 'rel' ? '../build-rel/'
  : EDEN_BUILD_PARAM === 'thr' ? '../build-thr/'
  : EDEN_M0_BUILDS[EDEN_BUILD_PARAM] ? EDEN_M0_BUILDS[EDEN_BUILD_PARAM]
  : '../build-st/';

// ─────────────────────────────────────────────────────────────────────────────────────────────
// ROADMAP Phase M / M5.1 + M5.2 — low-memory / mobile viability.
//
// M5.1: the deployed site sends EVERY visitor to ?build=thr, and the threaded build is known not
// to finish loading on a 2 GB iPad Air 2 ("stuck on Starting… forever"). navigator.deviceMemory
// does not exist on Safari, so a device heuristic cannot catch that case. Instead:
// edenArmThreadedLoadFailsafe() starts a timer when a threaded tree is about to load; if the
// module has not initialised by then, it remembers the failure in localStorage and reloads
// single-threaded. edenSettleBuildDir() honours that memory on every subsequent visit, so the
// second load is fast and needs no timeout. `?lowmem=retry` clears the memory for one attempt;
// `?lowmem=1` / `?lowmem=off` force it on/off by hand.
//
// M5.2: EDEN_LOW_MEMORY is also fed to the engine (Module._eden_set_low_memory, from
// eden-st.html's onRuntimeInitialized) so DisplayProfile_web.mm's kProfiles[EDEN_PROFILE_LOWMEM]
// row seeds a leaner video preset (1x pixel ratio, 75% render scale). navigator.deviceMemory ≤ 4
// feeds the video preset only — it is too coarse to deny a device the threaded build.
const EDEN_LOWMEM_KEY = 'eden.lowmem';        // '1' once a device is known/declared low-memory
const EDEN_LOWMEM_GEN_KEY = 'eden.lowmem.gen';
const EDEN_LOWMEM_PARAM = new URLSearchParams(location.search).get('lowmem');

// A remembered downgrade is a VERDICT ABOUT A BUILD, not a permanent fact about the device, and
// it is stored forever — so when the reason a device failed is fixed, the memory has to be
// invalidated or that visitor never gets the threaded build again. That was not hypothetical:
// ROADMAP V7 (2026-09-03) had the deployed threaded build failing to boot in EVERY Chromium
// because the service worker never sent `COEP: require-corp` on worker scripts, so between
// 2026-08-30 and the fix, every desktop visitor's failsafe fired and wrote this flag.
//
// **Bump this string whenever a change could plausibly turn a previous "it didn't load" into
// "it loads now"** — a boot/isolation/worker fix, an Emscripten flag change, a memory cut. The
// cost of bumping it wrongly is bounded and small: a device that really cannot run the threaded
// build (the 2 GB iPad Air 2) pays one more 45 s failsafe wait, once, and re-remembers.
const EDEN_LOWMEM_GEN = '2026-09-03-coi-worker-coep';

function edenLowMemStored() {
  try { return localStorage.getItem(EDEN_LOWMEM_KEY) === '1'; } catch (e) { return false; }
}
function edenLowMemSetStored(on) {
  try {
    if (on) {
      localStorage.setItem(EDEN_LOWMEM_KEY, '1');
      localStorage.setItem(EDEN_LOWMEM_GEN_KEY, EDEN_LOWMEM_GEN);
    } else {
      localStorage.removeItem(EDEN_LOWMEM_KEY);
      localStorage.removeItem(EDEN_LOWMEM_GEN_KEY);
    }
  } catch (e) { /* private mode / disabled storage — the session-scoped path still works */ }
}
// Runs before anything reads the flag: a memory recorded by an older build is discarded and the
// device gets one clean attempt at the current one.
(function edenExpireStaleLowMemVerdict() {
  try {
    if (localStorage.getItem(EDEN_LOWMEM_KEY) !== '1') return;
    if (localStorage.getItem(EDEN_LOWMEM_GEN_KEY) === EDEN_LOWMEM_GEN) return;
    localStorage.removeItem(EDEN_LOWMEM_KEY);
    localStorage.removeItem(EDEN_LOWMEM_GEN_KEY);
    console.log('[eden-host] discarding a threaded-load downgrade remembered by an older build ' +
      '(ROADMAP Phase M / M5.1) — retrying the threaded build once.');
  } catch (e) { /* no storage: nothing remembered, nothing to expire */ }
})();
// Apply the by-hand overrides once, at load.
if (EDEN_LOWMEM_PARAM === '1' || EDEN_LOWMEM_PARAM === 'on') edenLowMemSetStored(true);
else if (EDEN_LOWMEM_PARAM === 'off' || EDEN_LOWMEM_PARAM === '0' || EDEN_LOWMEM_PARAM === 'retry') {
  edenLowMemSetStored(false);
}

// Does the engine get the low-memory video preset this session? Stored flag, an explicit ?lowmem=1,
// or a Blink deviceMemory reading of 4 GB or less. ?lowmem=off wins outright.
const EDEN_LOW_MEMORY =
  EDEN_LOWMEM_PARAM === 'off' || EDEN_LOWMEM_PARAM === '0' ? false
  : edenLowMemStored() || EDEN_LOWMEM_PARAM === '1' || EDEN_LOWMEM_PARAM === 'on' ||
    (typeof navigator !== 'undefined' && navigator.deviceMemory > 0 && navigator.deviceMemory <= 4);

// Was the threaded build previously found not to load on this device? (Distinct from EDEN_LOW_MEMORY
// — deviceMemory ≤ 4 does NOT imply this, only a real observed load failure or an explicit request
// does.) ?lowmem=retry forces one more threaded attempt regardless.
function edenThreadedDowngradeRemembered() {
  return EDEN_LOWMEM_PARAM !== 'retry' && edenLowMemStored();
}

// Started from eden-st.html's edenLoadModule(), right after the eden.js <script> is appended, and
// only when a threaded tree is what got appended. `moduleReady` (declared at the top of this file)
// flips true from Module.onRuntimeInitialized.
function edenArmThreadedLoadFailsafe() {
  const threaded = EDEN_BUILD_DIR === '../build-thr/' || EDEN_M0_THREADED.has(EDEN_BUILD_PARAM);
  if (!threaded) return;
  let secs = parseInt(new URLSearchParams(location.search).get('thrtimeout'), 10);
  if (!(secs >= 15 && secs <= 180)) secs = 45;   // a cold iPad download of ~17 MB + the world is slow
  setTimeout(() => {
    if (moduleReady) return;
    console.warn('[eden-host] threaded build has not initialised after ' + secs + 's — remembering ' +
      'the failure and reloading single-threaded (ROADMAP Phase M / M5.1). ?lowmem=retry to retry.');
    edenLowMemSetStored(true);
    statusEl.textContent = 'Threaded build did not finish loading — switching to single-threaded…';
    // Reload with ?lowmem=retry stripped (leaving it would re-clear the flag we just set and loop
    // straight back into the failed threaded attempt). edenSettleBuildDir() then sees the stored
    // flag and downgrades to build-st, the guaranteed-populated fallback tree.
    const u = new URL(location.href);
    u.searchParams.delete('lowmem');
    location.replace(u.toString());
  }, secs * 1000);
}
// ─────────────────────────────────────────────────────────────────────────────────────────────

// Audit row 36/C1, pass 65 — FAIL CLOSED. Called by eden-st.html from EdenCOI.whenSettled(), i.e.
// after public/eden-coi.js has had its chance to obtain cross-origin isolation (service worker +
// one reload) and immediately before the chosen build is actually requested.
//
// Loading build-thr into a page that is not isolated does not produce a diagnosable error: the
// module throws "SharedArrayBuffer is not defined" somewhere inside instantiation, main() never
// runs, and what the player sees is a live DOM over a black canvas — the exact picture
// web/CLAUDE.md warns "reads as a renderer bug and is not one". Running the single-threaded build
// instead is strictly better in every case: it is the same game, it always works, and the reason
// for the downgrade is stated on the status line rather than left to devtools archaeology.
function edenSettleBuildDir() {
  const threaded = EDEN_BUILD_PARAM === 'thr' || EDEN_M0_THREADED.has(EDEN_BUILD_PARAM);
  if (!threaded) return;

  // ROADMAP Phase M / M5.1: a remembered load failure on this device downgrades before we even
  // try, so the common case (revisiting on a 2 GB iPad) is one fast single-threaded boot rather
  // than a 45 s wait for the failsafe timer.
  if (edenThreadedDowngradeRemembered()) {
    EDEN_BUILD_DIR = '../build-st/';
    console.warn('[eden-host] the threaded build previously failed to load on this device ' +
      '(ROADMAP Phase M / M5.1) — running single-threaded. Append ?lowmem=retry to try again.');
    statusEl.textContent = 'Threaded build previously failed here — running single-threaded.';
    showToast('Running single-threaded (threaded build failed here before)');
    return;
  }

  if (self.crossOriginIsolated) return;
  EDEN_BUILD_DIR = '../build-st/';
  const why = window.EdenCOI ? window.EdenCOI.reason() : 'eden-coi.js did not load';
  console.warn('[eden-host] ?build=thr needs a cross-origin-isolated page (COOP: same-origin + ' +
    'COEP: require-corp) for SharedArrayBuffer, and this page is not isolated (' + why + '). ' +
    'Loading the SINGLE-THREADED build instead. Locally, serve with `node tools/serve.js <port>`, ' +
    'which sends both headers directly.');
  statusEl.textContent = 'Threaded build unavailable (' + why + ') — running single-threaded.';
  // …and a toast, because the status line is a single shared line that the shim's own
  // `[eden-gl] drawable resized…` print overwrites a second or two later (verified in Safari,
  // pass 65) — leaving no on-page trace at all of a downgrade the player explicitly asked against.
  showToast('Threaded build unavailable — running single-threaded');
}
// Audit row 15/B6: content-hashed filenames for long-cache-life deploys. Empty here — this file
// is served verbatim (unhashed) by tools/serve.js for local dev, per that file's own header on why
// hashing doesn't belong in the dev loop. `tools/build-dist.js` rewrites this literal object (and
// the 'eden.js' src eden-st.html's own inline script uses) in the COPY it assembles into dist/,
// mapping each bare filename Emscripten's glue asks for to the hashed name actually on disk, so
// eden.js itself never needs to know its own wasm/data file was renamed.
const EDEN_ASSET_MAP = {};

// -------------------------------------------------------------------------------------------
// Phase N Stage 4.2 — save-on-background.
// -------------------------------------------------------------------------------------------
// The engine only ever saved on streaming boundaries, warps, the Save button and exit-to-menu
// (root CLAUDE.md says so outright). Nothing saved when the PAGE went away — so a tab the browser
// discards under memory pressure, or a phone that kills a backgrounded Safari, lost everything
// since the last streaming boundary. eden-storage.js's own visibilitychange/pagehide handlers do
// not cover this: they flush the storage MIRROR, which can only persist bytes the engine has
// already written.
//
// `eden_app_will_background()` (src/seam/AppLifecycle_web.mm) is the engine side and owns the
// "is it safe to save right now?" policy for every target. This is the web host's way in. It
// returns one of the EDEN_BG_* codes from src/seam/AppLifecycle_web.h:
//     0 saved · 1 no world open · 2 busy (loading/unloading) · -1 engine not up
//
// WHO CALLS IT, AND WHY IT IS NOT A LISTENER HERE: order matters, and it is the one thing a
// second listener could not guarantee. The engine must write into MEMFS BEFORE eden-storage.js
// mirrors MEMFS to OPFS/IndexedDB, or the save misses this departure and waits for the next one —
// and this file loads AFTER eden-storage.js (see eden-st.html), so a `pagehide` listener added
// here would run second. So eden-storage.js's flushNow() — which is already registered on both
// visibilitychange and pagehide, and whose entire job is "the app is going away, persist
// everything" — calls this first thing. One ordered pipeline, one registration site.
function edenSaveOpenWorld() {
  const M = window.Module;
  if (!moduleReady || !M || typeof M._eden_app_will_background !== 'function') return -1;
  try {
    return M._eden_app_will_background();
  } catch (e) {
    // A departure handler is the worst possible place to throw: it would skip the storage flush
    // that runs after it, turning "we might not have saved" into "we definitely did not persist".
    console.warn('[eden-host] save-on-background failed', e);
    return -1;
  }
}
