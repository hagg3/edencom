# The `.eden` World File Format (Web Port)

**Format is identical to the root docs — see
[`../../docs/eden-file-format.md`](../../docs/eden-file-format.md).** The 192-byte
header, 32 KB column records, and end-of-file `ColumnIndex` directory are read/written
by the same `FileManager.mm`/`FileManagerHelper.mm` code as the native build (these
files are compiled unmodified — see [save-load.md](save-load.md) for why that was
possible). Nothing about the byte layout changes for the web port; only where the
bytes physically live changes (IDBFS-backed `/documents`, see
[save-load.md](save-load.md)).

## How the bundle gets to the browser
The bundled `Eden.eden` (~52.5 MB, RLE variant) is symlinked into `public/Eden.eden`
and populated into `/bundle/Eden.eden` via a `--pre-js` (`src/seam/js/
eden_default_world.pre.js`) registered as a `Module.preRun` run dependency — it works
under both a browser `fetch()` and `node eden.js`'s `fs.readFileSync`. This is
**concurrent with, not deferred past,** the main asset package load; it is not truly
lazy/byte-range-fetched. `FS.createLazyFile` throws outside a Web Worker on the
emsdk version this port pins, and deferring the read past `main()` would need
Asyncify, because `FileManagerHelper::fmh_init` reads the bundle synchronously and
unconditionally during `World::World()`. Net effect: the whole ~52.5 MB file is held
fully resident in MEMFS for the session — a real, currently-unsolved memory cost (see
[world-and-terrain.md](world-and-terrain.md)). The eventual fix would be either a
custom FS node backed by a byte-range-request LRU cache, or moving to the threaded
build (where a synchronous `XHR` in a worker is legal) — neither is done.

## One real divergence, not caused by the web port
Upstream Eden ("New Dawn") has since moved to a **v5** `.eden`: same header, same block
IDs, same intra-chunk addressing, but **256 blocks tall** — 16 chunk-bands per column,
131,072-byte column records, a 400-slot creature block. **This fork reads, plays, authors and
converts that variant natively** (world height is a per-world runtime value; native read/play
landed 2026-08-06, in-app authoring + conversion landed 2026-08-26). The measured byte-level
facts and the unknowns are in
[`../../docs/eden-file-format.md`](../../docs/eden-file-format.md)'s "256z variant" section;
the staged plan to support it natively is
[`../../WORKING/256z-format-backport-plan-2026-08-05.md`](../../WORKING/256z-format-backport-plan-2026-08-05.md).

Three things exist today:
- **Native 256z read/play/save.** A version 5 or 6 header makes `World::loadWorld` size the
  terrain arrays for 256 before allocating them; `tools/headless-256z-test.js` is the
  regression test (it builds its own fixture by running `eden-convert.js` over a world this
  engine just saved, so the converter and the engine check each other).
  **The web-specific cost is memory**: a tall world with real terrain measured **413 MB** of
  wasm heap versus 128 MB for a 64z one (`build-st`, an 18×18-column world reaching y≈250).
  Desktop browsers are fine with that; iOS Safari is not — the same ceiling audit row A11 hit.
  Nothing warns the player about this yet.
- **`FileManager::loadWorld` refuses `version > 6`** and routes it to the existing
  `eden_report_load_failure()` hook (`src/seam/LoadFailure_web.mm` →
  `public/eden-loaderror.js`'s recovery dialog). That guard started life as ">= 5" and the
  reason it exists is unchanged: such a file loads *silently* — its version is inside the
  legacy path's `1..1000` range and its `directory_offset` passes the sanity checks — reads
  every column past the first at the wrong stride, and is then **overwritten by the first
  autosave**, which streaming triggers within seconds of walking. Destruction, not truncation.
- **`tools/eden-convert.js`** converts a world in either direction offline, so a v5 world can
  be played here today by converting it down (losing everything above y=63, with a count
  reported before it asks). See the root doc for the CLI. `tools/eden-convert-test.js` is its
  test — pure Node, no wasm, no fixture files: it synthesises worlds byte by byte, asserts the
  64z→256z→64z round-trip is byte-identical, and covers the real-world short-column anomaly.

- **In-app authoring and conversion (2026-08-26, 256z Stage 3 items 4-5).** The New World screen
  (`public/eden-menu.js`'s "Height format" segmented control) can create a 256z world directly —
  `Menu_web.mm`'s `eden_menu_set_pending_world_height`/`eden_menu_take_pending_world_height` park
  the choice the same one-shot way world type already does, consumed by
  `FileManager::probeWorldHeight` for a not-yet-existing file. 64z stays the default: nothing sets
  256 unless the player explicitly picked it on that screen. The Settings → Storage tab
  (`public/eden-settings.js` + `Storage_web.mm`'s `eden_storage_convert_to_64z_at`) offers a
  "Convert to 64z" action on any 256z world in the list, backed by
  `FileManager::convertWorldTo64` (`Classes/FileManager.mm`) — a from-scratch C++ restatement of
  `eden-convert.js`'s `--to-64` algorithm over `NSFileHandle` instead of Node's `fs`, since a
  browser has no Node to shell out to. Same temp+rename atomicity as `saveWorld()`; refuses rather
  than touching the world that's currently open. Regression test:
  `tools/headless-256z-authoring-test.js`. **Known gap**: unlike `headless-256z-test.js` (which
  checks the engine and the CLI converter against each other), nothing checks
  `convertWorldTo64`'s block-discard/door-orphan/creature-relocation counters against
  `eden-convert.js`'s on a world with real content above y=63 — the two implementations are
  hand-kept in sync, not shared code. Also surfaced one pre-existing bug while landing this: a
  brand-new file's very first save assumed `sfh->version<3` (true for every 64z world, since new
  worlds always started at version 2) to decide whether `directory_offset` needed its one-time bump
  past the creature block — a new world stamped straight to version 5 skipped that bump and got a
  garbage directory. Fixed in `saveWorld()`'s `!existed` branch (see its comment).

A fourth thing landed 2026-08-25 and is worth knowing about before touching any directory code:
**the post-directory sign trailer**. Worlds from a 2026-08 game update (`NewFormat256z`) append
in-game sign records inside the directory region, tagged so this engine's reader steps over them —
and, until this fix, so that any save which rewrote the directory silently deleted them. Byte
layout, capture/re-emit rule and the specimen it was measured on are in the root doc's
"post-directory sign trailer" section; `tools/headless-save-trailer-test.js` is the regression
test. Nothing here parses or writes a sign.

Neither needs a seam: `FileManager.mm`/`FileManagerHelper.mm` are **not** in
`EDEN_SEAM_EXCLUDE` — they compile verbatim for web, so all format work is shared with the
native target automatically.
