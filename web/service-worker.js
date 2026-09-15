// service-worker.js — perf-audit row #21: PWA installability + a one-time-download cost for the
// static app shell, without fighting two things this port already depends on:
//
//   1. RESUME-HERE's documented dev workflow ("serve on a FRESH PORT after a rebuild") exists
//      because nothing else in this port caches build-st/build-rel output — a service worker that
//      cached eden.js/eden.wasm/eden.data cache-first would silently start serving a stale build
//      to anyone who'd ever visited before, in a way a fresh port can't fix (a SW's cache persists
//      across ports/origins-are-per-origin-not-per-port... actually per ORIGIN, and localhost:PORT
//      is a distinct origin per port, so a genuinely fresh port *would* dodge a stale SW too — but
//      the reasoning below holds regardless: don't add a second source of staleness on top of the
//      one that workflow already manages by hand).
//   2. Row #9's lazy `Eden.eden` FS node (pass 46) — a hand-written Range-request byte-server. A
//      SW that intercepted a Range request and answered it out of a whole-file cache entry (or
//      cached a 206 response as if it were the full body) would silently break that contract.
//
// So the strategy is NETWORK-FIRST for everything, falling back to a cache ONLY when the network
// genuinely fails (the actual offline case a PWA exists for) — never cache-first. An online dev
// session or player always gets the freshest bytes; nothing here can make a rebuild look stale.
// Range requests and `Eden.eden` itself are never intercepted at all — those requests pass straight
// to the network exactly as if this file didn't exist.
//
// SECOND JOB, added in pass 65 (audit row 36/C1): synthesising the two cross-origin-isolation
// headers the THREADED build needs, for hosts that cannot send them. See the COI section below —
// and note that this file being the origin's one and only service worker is exactly why that job
// landed here instead of in a separate `coi-serviceworker.js` (two registrations cannot share a
// scope; the second `register()` replaces the first).
'use strict';

const CACHE_NAME = 'eden-shell-v1';

// Precached at install time: the app shell JS/CSS/HTML/icons, which change only across an
// intentional deploy, not across every local rebuild — the *reason* build-st/build-rel/Eden.eden
// are deliberately absent from this list (see header). Those are still cached, just lazily, by the
// network-first fetch handler below the first time they're successfully requested.
const SHELL_URLS = [
  'public/eden-st.html',
  'public/manifest.webmanifest',
  'public/eden-ui.js',
  'public/eden-ui.css',
  'public/eden-icons.js',
  'public/eden-assets.js',
  'public/eden-menu.js',
  'public/eden-pausemenu.js',
  'public/eden-settings.js',
  'public/eden-storage.js',
  // ROADMAP Phase C / C2: the OPFS persistence backend and its worker. Both precached for the
  // same reason as the shell scripts above — on an offline load a missing eden-opfs.js would
  // silently drop every visitor back to the IndexedDB whole-file save path, and a missing
  // eden-opfs-worker.js would fail the same way one step later (the worker never starts).
  'public/eden-opfs.js',
  'public/eden-opfs-worker.js',
  'public/eden-loading.js',
  'public/eden-loaderror.js',
  // Audit row 28/C5: the five files eden-st.html's own inline script was split into — required for
  // the page to boot at all (not an optional feature module like eden-gamepad.js/eden-console.js,
  // which this list already omitted before the split and still does).
  // Audit row 36/C1 pass 65: the client half of the cross-origin-isolation shim (see the COI
  // section below). Precached for the same reason as the five files above — the page's FIRST
  // script, and on an offline load a missing one would leave `window.EdenCOI` undefined.
  'public/eden-coi.js',
  'public/eden-host.js',
  'public/eden-viewport.js',
  'public/eden-keybinds.js',
  'public/eden-hotbar.js',
  'public/eden-input.js',
  'public/audio/icons/Icon_57.png',
  'public/audio/icons/Icon_72.png',
  'public/audio/icons/Icon_114.png',
  'public/audio/icons/Icon_144.png',
];

self.addEventListener('install', (event) => {
  self.skipWaiting();
  event.waitUntil(
    caches.open(CACHE_NAME)
      .then((cache) => cache.addAll(SHELL_URLS))
      .catch((err) => console.warn('[eden-sw] shell precache incomplete:', err))
  );
});

self.addEventListener('activate', (event) => {
  event.waitUntil(
    caches.keys()
      .then((keys) => Promise.all(keys.filter((k) => k !== CACHE_NAME).map((k) => caches.delete(k))))
      .then(() => self.clients.claim())
  );
});

// ---------------------------------------------------------------------------------------------
// Cross-origin isolation (audit row 36/C1, pass 65) — the `coi-serviceworker` pattern, merged in
// ---------------------------------------------------------------------------------------------
// The threaded build (`?build=thr`, -DEDEN_THREADED=ON) has a SharedArrayBuffer for its wasm
// memory, and a browser only hands SharedArrayBuffer to a cross-origin-ISOLATED document — one
// whose OWN navigation response carried COOP: same-origin + COEP: require-corp. GitHub Pages
// cannot send either (no `_headers` support; same limitation audit row 15/B6 hit with
// Cache-Control), so the headers are synthesised HERE, on the navigation response this worker
// serves. `tools/serve.js` sends them for real, so locally this path is redundant — run it with
// `--no-coi` to reproduce a header-less host and actually exercise this code.
//
// TWO DELIBERATE NARROWINGS vs. upstream coi-serviceworker, both load-bearing:
//
//  1. **Navigations, plus WORKER SCRIPTS.** COOP and COEP are DOCUMENT-level policies; an ordinary
//     subresource does not need them (a same-origin subresource needs nothing at all under
//     require-corp, and a cross-origin one needs CORP or a passing CORS check, neither of which a
//     header on our own response could provide). Upstream rewrites every response because it
//     cannot tell what it is proxying; we can, so we do not touch an ordinary subresource — which
//     keeps the byte-serving, ETag and cache paths above exactly as they were.
//
//     **A dedicated worker's own script is the exception, and missing it broke the deployed
//     threaded build for six days (ROADMAP V7, root-caused 2026-09-03).** HTML's "check a global
//     object's embedder policy" step says: if the OWNER is cross-origin isolated and the worker
//     script's response is not itself `COEP: require-corp`, the worker fails to load. Chromium
//     enforces it; WebKit did not, which is exactly why every local `tools/serve.js` run (real
//     headers on EVERY response) and every Safari pass looked fine while deployed Chrome did not.
//     The failure is silent by construction — a blocked worker fires a bare `error` event with an
//     empty message and logs nothing — and it took out BOTH workers this port has:
//       * `build-thr/eden.js` loaded as the pthread pool's worker script (`new Worker(_scriptName)`)
//         → the `loading-workers` run dependency never resolves → `Module.calledRun` never flips →
//         menu never appears, no error, and only M5.1's 45 s failsafe rescues the visitor;
//       * `public/eden-opfs-worker.js` → `[eden-storage] OPFS unavailable (eden-opfs worker
//         failed: unknown)` → silent downgrade to the IndexedDB whole-file save path.
//     Both were reported as separate bugs. They are one bug, and this is it.
//
//     Applied to a worker script UNCONDITIONALLY rather than only when some client wants
//     isolation: a `fetch` event for a worker's main script does not reliably identify the
//     document that created it (`clientId` is the creator's only on some engines, and the SW
//     instance may have been spun up for this very request), and the header is inert where it is
//     not needed — an owner that is not isolated never runs the compatibility check, and neither
//     of this port's two workers loads a cross-origin no-cors subresource that a require-corp
//     policy of its own could block. `?coi=off` therefore stays honest too: it prevents any
//     ISOLATED DOCUMENT from existing, so the header on the worker script is unobservable.
//
//  2. **Only when the page asked for the threaded build.** Isolation is not free: under COEP
//     require-corp the document may no longer embed a cross-origin subresource that neither sends
//     CORP nor passes CORS. This port has exactly one cross-origin dependency — the edenarchive
//     manifest/worldfiles that `public/eden-worldbrowser.js` pulls from hagg3.github.io — and it
//     survives isolation only because those are `fetch()` calls in CORS mode against a host that
//     sends `Access-Control-Allow-Origin: *` (verified 2026-08-05). A no-cors load of the same
//     host (an `<img>` world thumbnail, say — an entirely plausible future addition) WOULD break
//     under isolation, because Pages sends no `Cross-Origin-Resource-Policy`. Isolating only the
//     threaded navigation means the single-threaded deploy that everyone actually loads today
//     cannot regress, and the blast radius of getting this wrong is one dev-only URL.
//
// If the deployed DEFAULT build ever becomes the threaded one, this predicate is the one thing to
// change (and `tools/build-dist.js --build=thr` already writes `?build=thr` into the index.html
// redirect, so the URL rule keeps holding until then).
function wantsIsolation(url) {
  // `?coi=off` is public/eden-coi.js's escape hatch and it is honoured HERE too, not just on the
  // client: once this worker is installed, it is the thing actually isolating the page, so a
  // switch that only stopped the client half would leave `?build=thr&coi=off` isolated anyway and
  // mean nothing. Honouring it here is also what makes the fail-closed path (eden-host.js's
  // edenSettleBuildDir) reachable on demand instead of only on a machine that happens to be
  // unable to run service workers.
  if (url.searchParams.get('coi') === 'off') return false;
  return url.searchParams.get('build') === 'thr';
}

// `kind` is 'document' (COOP + COEP, the navigation case) or 'worker' (COEP only — COOP is a
// browsing-context policy and means nothing on a worker script).
function withIsolationHeaders(res, kind) {
  // `new Response(res.body, …)` cannot reconstruct these: an opaque/opaqueredirect response has an
  // unreadable body (status 0), and rewriting a redirect would swallow it. Pass them through — a
  // page that failed to load is not one that needs isolating.
  if (!res || res.status === 0 || res.type === 'opaqueredirect' || !res.ok) return res;
  const headers = new Headers(res.headers);
  if (kind !== 'worker') headers.set('Cross-Origin-Opener-Policy', 'same-origin');
  headers.set('Cross-Origin-Embedder-Policy', 'require-corp');
  return new Response(res.body, { status: res.status, statusText: res.statusText, headers });
}

// The capability handshake public/eden-coi.js uses to answer "is the worker CONTROLLING this page
// one that can isolate it?" — which is not the same question as "is a worker controlling it". A
// returning player's browser may still be running the pre-pass-65 shell worker from cache; that
// one has no message handler, so the ping times out and eden-coi.js correctly keeps waiting for
// the update instead of reloading into a page that would never become isolated.
self.addEventListener('message', (event) => {
  const data = event.data;
  if (data && data.type === 'eden-coi-ping' && event.ports && event.ports[0]) {
    event.ports[0].postMessage({ edenCoi: true });
  }
});

self.addEventListener('fetch', (event) => {
  const req = event.request;
  if (req.method !== 'GET') return;                    // never touch saves/POSTs
  if (req.headers.has('range')) return;                 // never touch the lazy Eden.eden byte-server
  let url;
  try { url = new URL(req.url); } catch (e) { return; }
  if (url.origin !== self.location.origin) return;      // don't cache cross-origin requests
  if (/\/Eden\.eden$/.test(url.pathname)) return;        // never cache the default-world file itself

  // 'document' | 'worker' | null — see narrowing 1 above. `req.destination` is 'worker' for
  // `new Worker(url)` (the pthread pool's copy of eden.js, and eden-opfs-worker.js).
  const isolate = (req.mode === 'navigate' && wantsIsolation(url)) ? 'document'
    : (req.destination === 'worker' || req.destination === 'sharedworker') ? 'worker'
    : null;

  event.respondWith(
    fetch(req).then((res) => {
      if (res && res.ok) {
        // Cached BEFORE the isolation headers are added, so what lands in the cache is exactly
        // what the server sent. The `.then` below re-applies them on the way out of either path,
        // so an offline navigation is isolated too.
        const copy = res.clone();
        caches.open(CACHE_NAME).then((cache) => cache.put(req, copy)).catch(() => {});
      }
      return res;
    }).catch(() => caches.match(req).then((cached) => {
      if (cached) return cached;
      throw new Error('eden-sw: offline and not cached: ' + url.pathname);
    })).then((res) => (isolate ? withIsolationHeaders(res, isolate) : res))
  );
});
