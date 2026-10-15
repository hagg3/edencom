# The web port (retired 2026-10-13)

From July to October 2026 the engine also ran in a browser: an Emscripten/WASM build under `web/`,
deployed to GitHub Pages. It was where the port's seam/shim layer was invented, and until Phase N it
was the only live target. **It was retired on 2026-10-13**: the native SDL3 builds (macOS, Linux,
Windows, iOS; Android next) are the product, and keeping a fifth target green was costing more than it
returned.

## Where it is now
- **Tag `web-port-final`** (commit `bd46491`) in this repo is the last commit with a buildable web
  target. `git show web-port-final:web/CLAUDE.md` is its fast-facts file; `git checkout
  web-port-final -- web/` restores the tree.
- **hagg3/edence** is the frozen public snapshot of the web port (no longer synced). hagg3/edencom
  (the superset with `native/`) continues; see `publish-to-edencom.sh`.
- The shared code moved, not died: `web/src/{seam,shim}` → [`port/src/`](../port/CLAUDE.md),
  `web/tools/{emod.py, fixtures, engine-sources.txt, …}` → `port/tools/`, the two UI fonts →
  `media/fonts/`, and the shim/UI reference pages → [`port/docs/`](../port/docs/README.md). Any
  `web/...` path still printed in a comment or an older doc names the retired build; read it at the tag.

## What was removed
- **The host:** `web/public/` (the DOM menu, settings, pause menu, hotbar, loading/load-error
  overlays, world browser, storage backends `eden-storage.js` / `eden-opfs*.js`, `eden-coi.js`,
  `eden-host.js`), `web/service-worker.js`, the PWA manifest.
- **The build:** `web/CMakeLists.txt` (Emscripten 3.1.74; trees `build-st` Debug single-threaded,
  `build-rel`, `build-thr`/`build-relthr` pthreads, `build-relwdiag`, `-cap512`, `build-alloctrace`),
  `web/cmake/`, the local `web/emsdk`, and the web-only seam files (`EAGLView_web`, `main_web`,
  `EdenAppDelegate_web`, `EdenViewController_web`, `HeapProbe_web`, `AllocTrace_web`,
  `pthread_sync_web`, `pvrt_matrix_palette`, `seam_link_stubs`, `SimpleAudioEngine_web`,
  `gl_context_web`, the `EM_JS` `NSUserDefaults.mm`, the entry `eden_main.cpp`).
- **Deploy and CI:** `.github/workflows/pages.yml`; native.yml's `web` job.
- **Tools:** `serve.js` (COOP/COEP, ranges, compression), `serve-fastly-repro.js`, `build-dist.js`,
  every `headless-*.js` (drove `eden.js` under node), every `safari-*.js` (safaridriver), the
  browser/Chrome memory probes.
- **Docs:** `web/CLAUDE.md` (folded into [`port/CLAUDE.md`](../port/CLAUDE.md)), `web/README.md`, and the
  `web/docs/` pages that were browser-only: build-and-toolchain, execution-flow (the Emscripten frame
  loop), save-load (MEMFS/IDBFS/OPFS mirror, lazy range-fetched `Eden.eden`), eden-file-format (the
  bundle in the browser), networking (CORS), rendering, entities, lighting, terrain-generation,
  world-and-terrain (the 512 MB linear-memory cap), third-party (web vendored JS), `archive/`.

## Web-only knowledge worth one line each (full text at the tag)
- Threaded wasm needs cross-origin isolation; GitHub Pages can't send headers, so a service worker
  synthesised COOP/COEP — and Chromium also requires COEP on **worker script** responses (V7).
- Fastly applies a `Range` to the **gzip** representation when the client sends `Accept-Encoding:
  gzip`; WebKit's synchronous XHR does.
- A JS exception under a synchronous engine read, or touching an unexported `Module` method, kills
  the frame loop silently; "count engine frames, not rAF frames".
- Linear memory was capped at 512 MB and hitting it aborted; 256z at R16 did not fit.
- OPFS sync access handles are Worker-only; saves were mirrored by byte range from MEMFS.

## Retired web checks and what replaces them on native
| Web check | Native replacement | Gap |
|---|---|---|
| `headless-p1-gate` | `--p1-gate` | — |
| `headless-mesh-checksum` (cross-target geometry) | `--headless --stage1` + `.github/scripts/check-stage1.py`; Linux→Windows/macOS/iOS cross-loads in native.yml | wasm leg gone (by design) |
| `headless-emod-store-test` | `--emod-selftest --emod-fixtures=../port/tools/fixtures/emod` | — |
| `headless-emod-export-test` | `--export=…` CLI; `--browser-selftest` (Share › Export) | — |
| `headless-save-on-background-test` | `--background-selftest` (refuses on the menu, writes in play) | — |
| `headless-gamepad-test` | `--gamepad-selftest` | — |
| `headless-menu-flow-test` | `--ui-selftest`, `--browser-selftest`, `--convert-prompt-selftest` | — |
| `headless-256z-authoring-test` (New World height picker, Convert to 64z) | `--convert-prompt-selftest`, `--stage1 --height=256`, `--ui-selftest` (New World's height choice, U.5); `port/tools/convert64-crosscheck.js` (`--convert64-selftest`: `convertWorldTo64` on the `.emod` and a `.eden` vs `eden-convert.js --to-64`, the retired `headless-256z-convert-crosscheck`) | — (W.2, 2026-10-14) |
| `headless-display-profile-test` (derived point space, profiles) | `--display-selftest` (stock 568×320 rects byte-exact, derivation, idempotent re-layout, profiles, clamps), `--touch-selftest --window=1024x768 --touch-profile`, `--shot` | — (W.2) |
| `headless-shim-selftest` (GL shim bookkeeping + ObjC runtime) | `--gl-selftest` (calls `eden_gl_selftest_run()`), `--objc-selftest` (Linux/Windows) | — (W.2) |
| `headless-recolor-test` (paint/door/skin recolour) | `--recolor-selftest` (boots with the display profile applied first, as a windowed/device boot does: a plain headless boot loads the 1× art and no paint mask) | — (W.2) |
| `headless-opfs-mirror-test`, `headless-lazy-world-test`, `safari-*` | — | not applicable (browser storage / transport) |
| `eden-convert-test.js` | unchanged, now in the linux job | — |
| heap/leak/timing probes | `--leak-probe`, `--mem-trace`, `--save-bench`, `--mesh-bench`, `--pass-probe` | column-read bench has no native twin |
