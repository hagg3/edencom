# World Representation & Terrain (Web Port)

**Identical to the root docs — see
[`../../docs/world-and-terrain.md`](../../docs/world-and-terrain.md).**

`Classes/Terrain.mm`/`TerrainChunk.mm` are compiled as-is (not in `CMakeLists.txt`'s
`EDEN_SEAM_EXCLUDE` list). The toroidal window, chunk storage layout, block edit
operations, and streaming logic are unmodified. The one place the web port touches
anything adjacent is the mesher's *output* (vertex upload to WebGL2), covered in
[gl-shim.md](gl-shim.md), not the storage/edit model covered by the root doc.

## A 256z world costs 413 MB of wasm heap
World height became a per-world runtime value on 2026-08-06 (root doc, "Runtime world height"),
so a v5/v6 `.eden` opens 256 blocks tall here. Measured in `build-st` against an 18×18-column
world whose terrain reaches y≈250: **413 MB** resident, against 128 MB for a 64z world and 96 MB
for the menu alone. `-sINITIAL_MEMORY` stays at 96 MB — growth handles the rest, and raising it
would only penalise the menu-only session. Treat 256z as a **desktop** capability on web until
someone measures a real iPhone; audit row A11 is the precedent for what happens when a tab
exceeds iOS Safari's budget (it does not fail gracefully).

**Warned, not refused, as of the pass following pass 69.** `FileManager::probeWorldHeight` was
already being read to size arrays before a world loads; `Storage_web.mm`'s
`eden_storage_list_worlds()` now also reports each world's real height (64/256) as a `height`
field, joined into `eden-menu.js`'s Load World list (`subtitleFor`, which used to hardcode "64z"
unconditionally — now honest per-world) and the Settings → Storage tab's per-world row
(`eden-settings.js`). Clicking Play/Load on a 256z world shows a toast warning
(`eden-menu.js`'s `warnIfTall`) — but only when `eden_profile_name()` reports the **touch**
profile, since desktop is confirmed fine and warning every desktop load of a big world would just
be noise. This is a soft warning, not a pre-flight refusal: a phone player who ignores it still
hits the same OOM this section describes, unmitigated — nothing here reduces the 413 MB footprint
itself. Live-verified in real Safari (desktop profile correctly does NOT toast; the JSON `height`
field round-trips correctly for both a synthesized 64z save and the checked-in 256z specimen).

**Refused outright on a low-memory device (ROADMAP Phase M / M5.3).** When the page has set the
low-memory flag (`eden_set_low_memory`, `src/seam/DisplayProfile_web.mm` — see
[ui.md](ui.md#low-memory-overlay) and `web-port-memory-plan.md` §M5), `World::loadWorld` reads
`probeWorldHeight` first and, for a 256z world, bails *before* `allocateMemory()` through
`eden_report_load_failure(name, "TALL_WORLD_LOW_MEM")` — terrain untouched, parked in the menu.
`eden-loaderror.js` recognises that reason token and shows a "World needs more memory" dialog
pointing at Settings → Storage → "Convert to 64z" instead of the corrupt-save recovery dialog.

## The 18×18 window is a hard floor on web too
The resident window size (`T_SIZE` etc.) is baked into `SIZEOF_COLUMN`/the save
format (root docs), so it can't be shrunk to save browser memory without breaking
every existing save including the bundled default world — don't treat it as a lever
for a web memory budget. The bundled `Eden.eden` itself is also currently held fully
resident (not lazily paged) once fetched — see
[eden-file-format.md](eden-file-format.md) for why, and
[save-load.md](save-load.md) for the similar "two full copies resident" situation
with saved-world data (MEMFS + IndexedDB).
