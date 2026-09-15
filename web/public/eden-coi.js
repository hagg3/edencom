// eden-coi.js — the client half of cross-origin isolation, and the ONE place this port registers
// its service worker (audit row 36/C1, pass 65). MUST be the first script the page loads: the
// whole point is to decide "can this page have SharedArrayBuffer?" before anything downloads a
// build that needs one.
//
// WHY THIS EXISTS. A `-DEDEN_THREADED=ON` build's wasm memory IS a SharedArrayBuffer, and a
// browser only exposes SharedArrayBuffer to a cross-origin-ISOLATED document — one whose own
// navigation response carried:
//     Cross-Origin-Opener-Policy: same-origin
//     Cross-Origin-Embedder-Policy: require-corp
// `tools/serve.js` sends both. **GitHub Pages cannot send any response header from repo content**
// (no `_headers` support — the same wall audit row 15/B6 hit with Cache-Control), so a threaded
// build deployed there would boot into "SharedArrayBuffer is not defined" and a black canvas. The
// decision (2026-08-04, WORKING/c1-threaded-build-handoff.md §5) was to fix that in the page
// rather than migrate hosts: a service worker can synthesise those two headers on the navigation
// it serves. That is the well-known `coi-serviceworker` pattern — Godot, Unity and ffmpeg.wasm all
// ship a copy of it for exactly this problem.
//
// THREE THINGS ARE DIFFERENT FROM UPSTREAM coi-serviceworker, and each is deliberate:
//
//  1. **No second service worker.** This port already ships one (`../service-worker.js`,
//     root-scoped, network-first app shell). Two registrations CANNOT coexist at one scope — the
//     second `register()` replaces the first — so dropping upstream's file in beside it would have
//     produced two workers displacing each other on alternate loads. The header synthesis lives in
//     `service-worker.js` instead (see its COI section), and this file is the client half only:
//     register, verify, reload once. It therefore also owns the plain PWA registration that used
//     to sit at the bottom of eden-st.html.
//
//  2. **A real capability handshake, not an assumption.** Upstream reloads as soon as a worker
//     controls the page. That conflates three different states this port has to tell apart: not
//     isolated *yet* (the worker hasn't claimed this client), not isolated *by this worker* (a
//     returning player is still being controlled by the pre-pass-65 shell worker, which knows
//     nothing about COOP/COEP), and not isolatable *at all* (no service worker, insecure origin).
//     So the controller is PINGED (`eden-coi-ping`) and must answer before a reload is worth
//     doing; a `sessionStorage` marker makes the reload strictly one-shot, because "reload until
//     isolated" against a worker that will never isolate is an infinite loop, and an infinite
//     reload loop is the classic way this pattern goes wrong.
//
//  3. **It fails CLOSED, loudly.** If isolation cannot be obtained, `whenSettled` still fires and
//     eden-host.js downgrades `?build=thr` to the single-threaded build with a message on the
//     status line. Never a black canvas: web/CLAUDE.md's most-repeated warning is that a dead
//     engine looks exactly like a renderer bug, and "SharedArrayBuffer is not defined" during
//     instantiation is one more way to produce that picture.
//
// ESCAPE HATCH: `?coi=off` disables all of it — no registration-driven reload here, and
// service-worker.js declines to synthesise the headers for that URL too (see wantsIsolation()),
// so the switch is honest even once the worker is installed. `?build=thr&coi=off` is therefore
// also the way to exercise the fail-closed path deliberately.
'use strict';

window.EdenCOI = (function () {
  // How long to wait for a freshly-registered worker to install, activate, claim this client and
  // answer the ping. Generous, because it is only ever paid on the FIRST visit to a threaded URL
  // (afterwards the worker is already controlling and the page is isolated on arrival) — and the
  // page is going to be thrown away by a reload anyway, so the wait costs nothing that was going
  // to be kept.
  var ACQUIRE_TIMEOUT_MS = 5000;
  var PING_TIMEOUT_MS = 500;
  var SW_URL = '../service-worker.js';   // resolves to the server root from this file's public/ URL
  var RELOAD_KEY = 'eden.coi.reload-attempted';

  var params = new URLSearchParams(location.search);
  var disabled = params.get('coi') === 'off';
  // Single source of truth for "this page wants the threaded build". eden-host.js maps this to a
  // build DIRECTORY; it does not re-derive the condition, and service-worker.js keys its
  // isolation predicate off the same `?build=thr` (see wantsIsolation() there).
  var threadedRequested = params.get('build') === 'thr';

  var state = 'unavailable';   // 'isolated' | 'pending' | 'unavailable' | 'off'
  var reason = '';
  var settled = true;
  var waiters = [];

  function settle(newState, why) {
    state = newState;
    reason = why || '';
    settled = true;
    while (waiters.length) waiters.shift()();
  }

  // sessionStorage throws outright in some privacy configurations — a storage failure must not be
  // the thing that stops the game from booting.
  function marker(op) {
    try {
      if (op === 'set') return window.sessionStorage.setItem(RELOAD_KEY, '1');
      if (op === 'clear') return window.sessionStorage.removeItem(RELOAD_KEY);
      return window.sessionStorage.getItem(RELOAD_KEY) === '1';
    } catch (e) { return false; }
  }

  // The plain PWA registration (perf-audit row #21), unchanged in spirit from the copy that used
  // to live at the bottom of eden-st.html: deferred to `load` so it never competes with boot for
  // bandwidth, best-effort, silent on failure. Used on every path that is NOT trying to obtain
  // isolation right now — the isolation path registers immediately instead, because there the
  // worker is a prerequisite rather than a nicety.
  function registerAfterLoad() {
    if (!('serviceWorker' in navigator)) return;
    window.addEventListener('load', function () {
      navigator.serviceWorker.register(SW_URL).catch(function (err) {
        console.warn('[eden-coi] service worker registration failed (non-fatal):', err);
      });
    });
  }

  // "Can the worker CONTROLLING this page isolate it?" — see service-worker.js's message handler.
  // Resolves false (rather than hanging) both when nothing controls the page and when what does
  // control it is an older worker with no handler for the ping.
  function controllerCanIsolate() {
    return new Promise(function (resolve) {
      var controller = navigator.serviceWorker.controller;
      if (!controller) { resolve(false); return; }
      var channel = new MessageChannel();
      var timer = setTimeout(function () { resolve(false); }, PING_TIMEOUT_MS);
      channel.port1.onmessage = function (event) {
        clearTimeout(timer);
        resolve(!!(event.data && event.data.edenCoi));
      };
      try {
        controller.postMessage({ type: 'eden-coi-ping' }, [channel.port2]);
      } catch (e) {
        clearTimeout(timer);
        resolve(false);
      }
    });
  }

  function nextControllerChangeOrTick() {
    return new Promise(function (resolve) {
      var done = false;
      var finish = function () {
        if (done) return;
        done = true;
        // Detach explicitly: `{once:true}` only removes a listener that FIRED, and the timeout
        // wins most iterations — without this the poll loop below leaves one dead listener per
        // 250 ms tick attached to the container.
        navigator.serviceWorker.removeEventListener('controllerchange', finish);
        resolve();
      };
      navigator.serviceWorker.addEventListener('controllerchange', finish, { once: true });
      setTimeout(finish, 250);
    });
  }

  // Register, then poll `controllerCanIsolate()` until a worker that answers has claimed us or the
  // deadline passes. Polling rather than awaiting a single `controllerchange`: the worker can claim
  // this client BEFORE the listener is attached (a warm registration activates fast), in which case
  // the event never fires and a pure event-driven wait would sit here until the timeout.
  function acquireIsolatingController() {
    var deadline = Date.now() + ACQUIRE_TIMEOUT_MS;
    return navigator.serviceWorker.register(SW_URL).then(function () {
      function attempt() {
        return controllerCanIsolate().then(function (ok) {
          if (ok) return true;
          if (Date.now() >= deadline) return false;
          return nextControllerChangeOrTick().then(attempt);
        });
      }
      return attempt();
    });
  }

  if (self.crossOriginIsolated) {
    // Already isolated — either the server sent the headers (tools/serve.js) or a previous load's
    // reload worked. Clear the one-shot marker so a LATER navigation in this tab still gets one
    // attempt of its own if isolation is somehow lost again.
    marker('clear');
    settle('isolated', '');
    registerAfterLoad();
  } else if (disabled) {
    settle('off', '?coi=off');
    registerAfterLoad();
  } else if (!threadedRequested) {
    // The single-threaded build needs nothing from this file. Not an error, and deliberately not
    // an opportunity: isolating this page anyway would apply COEP to a build that gains nothing
    // from it (see service-worker.js's second narrowing).
    settle('unavailable', 'not requested');
    registerAfterLoad();
  } else if (!self.isSecureContext || !('serviceWorker' in navigator)) {
    settle('unavailable', 'no service worker on this origin');
    console.warn('[eden-coi] ?build=thr needs cross-origin isolation, and this context cannot ' +
      'register a service worker to obtain it (needs HTTPS or localhost). Falling back.');
  } else if (marker('get')) {
    // We already reloaded once for this and are STILL not isolated. Stop: the alternative is a
    // reload loop that never terminates. This is also the ONLY honest "this browser can't do it"
    // signal available — note in particular that probing `typeof SharedArrayBuffer` here would be
    // worthless, because a non-isolated page hides that constructor *precisely because* it is not
    // isolated (always in Firefox/Safari, and in Chrome on Android), so the probe would report
    // "unsupported" on exactly the browsers this shim exists to rescue.
    settle('unavailable', 'reloaded once and the page is still not isolated');
    console.warn('[eden-coi] reloaded once for cross-origin isolation and it did not take. The ' +
      'service worker may be blocked (private window, an extension, or a host that strips it), ' +
      'or this browser does not support SharedArrayBuffer at all. Falling back to the ' +
      'single-threaded build; `?coi=off` skips this entirely.');
    registerAfterLoad();   // still want the app-shell worker, even though it can't isolate us
  } else {
    state = 'pending';
    settled = false;
    console.log('[eden-coi] page is not cross-origin isolated and ?build=thr needs it — ' +
      'installing the service worker that synthesises COOP/COEP, then reloading once.');
    acquireIsolatingController().then(function (ok) {
      if (!ok) {
        settle('unavailable', 'the service worker never took control of this page');
        console.warn('[eden-coi] no isolation-capable service worker claimed this page within ' +
          ACQUIRE_TIMEOUT_MS + 'ms. Falling back to the single-threaded build.');
        return;
      }
      marker('set');
      // Everything this document has downloaded so far is discarded here. That is the pattern's
      // known, accepted cost (handoff §5): one extra page load on the first threaded visit,
      // because a document's isolation is fixed by the response that created it and cannot be
      // acquired afterwards. eden-st.html holds off on requesting eden.js until whenSettled()
      // fires, so the bytes thrown away are the page's own scripts, not the wasm.
      location.reload();
    }).catch(function (err) {
      settle('unavailable', 'service worker registration failed');
      console.warn('[eden-coi] service worker registration failed:', err);
    });
  }

  return {
    // 'isolated'    — crossOriginIsolated is true; the threaded build can be loaded.
    // 'pending'     — a reload is in flight; do not start loading a build.
    // 'unavailable' — isolation is not going to happen on this page; fail closed.
    // 'off'         — ?coi=off; treated exactly like 'unavailable' by callers.
    state: function () { return state; },
    reason: function () { return reason; },
    isolated: function () { return !!self.crossOriginIsolated; },
    threadedRequested: threadedRequested,
    // Fires as soon as the isolation question is answered — synchronously, if it already is.
    // Never fires on the 'pending' path that ends in a reload, by design: the page is about to be
    // replaced, and booting the engine into a document that is one macrotask from being discarded
    // wastes a wasm instantiation and can leave an IDBFS mount half-open.
    whenSettled: function (cb) { if (settled) cb(); else waiters.push(cb); },
  };
})();
