# Conventions, Technical Debt, Assumptions & Limitations

Read this before writing any code. These are the implicit rules the codebase follows
(or violates consistently), plus the traps that bite newcomers.

## Coordinate & naming conventions

1. **Argument order is `(x, z, y)` and y is UP.**
   Nearly every terrain function — `getLand(x,z,y)`, `setLand(x,z,y,…)`,
   `buildBlock(x,z,y)`, `addlight(x,z,y,…)` — takes horizontal x, horizontal z,
   *then* vertical y. But `Vector` fields are `.x/.y/.z` with `.y` up, `Point3D` is
   `{x,y,z}`, chunk indices go `(cx, cy, cz)` in `threeToOne`, and bounds arrays are
   `[x, y, z, x2, y2, z2]`. **Every off-by-one-axis bug in this codebase comes from
   here.** When in doubt, read the callee's loop body.
2. **Storage order**: within a chunk, `CC(x,z,y) = x·256 + z·16 + y` (y fastest —
   vertical strips contiguous). `blockarray`: `x·g_xz_stride + z·g_world_height + y` (the height is runtime — see world-and-terrain.md).
   The RLE bundle transposes to `CC(y,z,x)`.
3. **Two coordinate spaces**: absolute world blocks (≈65,000 near spawn) for logic
   and storage; render space rebased by `fm->chunkOffsetX/Z · 16` for GL. Terrain
   vertex data is additionally in quarter-block units (×4 shorts, ×0.25 modelview).
4. **`IS_IPAD` means "2× UI scale"**, and is set for Retina iPhones too
   (stock `EAGLView -initWithCoder:`). `IS_WIDESCREEN` = iPhone 5. Screen metrics are globals.
   **MODIFIED FROM STOCK (2026-07-31), and the change is worth understanding before you read any
   layout code**: `SCREEN_WIDTH`/`SCREEN_HEIGHT` were *device constants* here — one of
   480×320 / 568×320, picked once in `EAGLView -initWithCoder:` and never changed again. They are
   now a *derived layout coordinate system* that can change while the game runs. On the stock iOS
   target nothing moves; on every port target (iOS included) the point space comes from the real
   window aspect and a UI-scale setting, so a desktop window gets a bigger point space and a
   proportionally smaller HUD instead of the iPhone-5 layout scaled up 4×. Two consequences for
   anyone touching `Classes/`:
   - The rect arithmetic that used to sit in `Hud::Hud()` / `Menu::Menu()` now lives in
     `Hud::layoutForScreen()` / `Menu::layoutForScreen()` and **must stay idempotent** — it can be
     re-run at any time. In particular the file-static margins in `Hud.mm` (`marginLeft2` and
     friends) are *mutated* by that code and are reset at the top of the method for exactly this
     reason. `Input::screenMetricsChanged()` is the same idea for `Input::scr_width/scr_height`,
     which it now *reads* from `SCREEN_*` instead of re-deriving from device constants.
   - `IS_WIDESCREEN` no longer means "this is an iPhone 5". It means "there is more width here than
     the 480-point layout was drawn for", which is what all ~10 of its branches in `Classes/`
     actually key off.
   The selection logic itself is platform detection and stays in the port's seam
   (`port/src/seam/DisplayProfile_web.mm`); only the layout generalisation is in the engine.
5. **Singletons as static members**: `World::getWorld`, `Resources::getResources` are
   public static *pointers*, assigned in constructors. `Input::getInput()` is a
   function. Hot data are C globals (`blockarray`, `colorTable`, `blockinfo`…).
6. **Obj-C++ everywhere**: `.mm` files mix C++ classes (game logic) with ObjC
   (Foundation I/O, UIKit). Manual retain/release (pre-ARC). ObjC message-syntax
   remnants appear inside comments from a past ObjC→C++ conversion — large comment
   blocks of `[obj method:...]` code are the *old* implementation kept for reference.
7. **`rt` prefix** on `TerrainChunk` fields = "render thread copy" (double-buffer
   leftovers). `c` suffix on globals (`chunkTablec`, `getLandc`) ≈ "C/global fast
   path".
8. **Return conventions**: `-1` = invalid/out-of-bounds block type (distinct from
   air = 0); color 0 = unpainted; hashmap key 0 = invalid.
9. **Dead code is kept, commented out.** The codebase is its own version control.
   Do not delete these blocks in refactors without checking they're truly dead —
   some (face merging, custom blocks) document intended future features and the
   author left activation instructions ("UNCOMMENT FOR FACE MERGING").

## Technical debt register (the honest list)

- **Threading scars**: a background chunk-mesher and streaming thread were removed
  after race conditions (the author's `//issue #1/#2/#3` comments in `Terrain.mm`
  mark the exact hazards; `archive/crashes/` holds the resulting App Store crash logs). The
  double-buffered chunk fields, `chunksToUpdateImmediatley`, and the sanity-check
  printouts are all scaffolding of that design. The load pthread survives, unlocked. *(Port:
  `Classes/MeshPool.{h,mm}` is a later worker pool for meshing and column decode, but it compiles only
  under `EDEN_THREADED`, which only the retired web build defined; every native target meshes inline.)*
- **Global mutable state**: FileManager's static file handle/header, Terrain's file
  statics, Model's static pools — none of it is re-entrant; exactly one world at a
  time, one of each system.
- **Vestigial octree** (`TreeNode`/`addToTree`/`renderTree`) — now a linear scan;
  the tree is still built and freed for nothing.
- **Disabled systems kept in-tree**: face merging (meshing), `TYPE_CUSTOM`
  half-blocks, the 1.x shadow columns (`getShadow` returns 1.0; superseded by H.2's
  `Classes/Shadow.mm`, which is live), FileArchive compression,
  occlusion queries, noise-seeded runtime worldgen,
  biome-marker PNG input, `WetNode` liquid heights, TestFlight.
- **Copy-paste blocks**: `refreshChunksInRadius` is 7 hand-unrolled loops;
  `destroyBlock`/`explodeBlock` are near-duplicates; door/portal/golden-cube render
  loops share 80% of their bodies; `getRampType`/`getRampType2` differ by one branch.
  When fixing a bug in one, grep for its twin.
- **Magic numbers**: streaming threshold `count>140`, progress `/324`, burn cap 300,
  spawn tables, hard-coded layout rects, memory thresholds — all inline. *(Port: the first two now
  scale with the runtime window — `count*324>140*ncols`, progress over `CHUNKS_PER_SIDE²` — B.5b-2.)*
- **The window origin has one formula** (B.5b-1a, 2026-10-11): `windowOriginChunk(pos)` in
  `Terrain.h`, corner-centred. A site that computes `pos/16 - T_RADIUS` itself disagrees with the
  streamer by one row half the time, and the streamer then re-reads that row. The outer ring of the
  window is never meshed (data for its neighbours), so an "is it drawn" or "is it settled" check over
  `chunksToUpdate` must skip it (`slotOnOuterRing`, as `f3_frameEnd` does).
- **Error handling** is `printg`/`printf` logging plus soldier-on; in stock, file I/O failures
  during save could silently corrupt worlds (no atomic writes). *(Port: `.emod` saves are atomic —
  one commit record per save, [save-load.md](save-load.md).)*
- **Typos are API**: `chunksToUpdateImmediatley`, `criticle errror` (and `Frustrum` in the
  `Frustum.{h,mm}` header comments) — keep them; renaming breaks greps against history and the community's knowledge.

## Assumptions the code makes

- Exactly one GL context, ES 1.1, main-thread-only. *(Port: still one context on the main thread;
  it is ES 3.0 or desktop GL, and the shim translates the engine's ES 1.1 calls.)*
- 32-bit-era struct layout, little-endian, for every byte written to disk
  (`WorldFileHeader` = 192 bytes, `ColumnIndex` = 16, `EntityData` = 60). Compiling
  for a platform that pads differently silently corrupts save files.
- `block8` is *signed* (types ≤ 127; negative = corrupt sentinel), `color8` unsigned.
- World height exactly 64; chunk size exactly 16; window 18×18 (`T_SIZE` etc. are
  `#define`s that many literals secretly depend on). *(Port: height and window are both runtime
  now — 2026-08-06 and B.5b-2, 2026-10-12; `rebuildList[13000]` was one such literal.)*
- Chunk coordinates fit in 15 bits (`twoToOne`), non-negative.
- The bundled map exists and matches `DEFAULT_LEVEL_SEED` worlds. *(Port: the bundle is `Eden.emod`;
  the repo-root `Eden.eden` is its source and is not shipped.)*
- Frame time ≈ 1/60 s; physics constants are tuned for it (with a 50 ms clamp).
- Documents directory contains only world files and their PNGs (the world browser
  header-parses every file in it).

## Known limitations

- 1296 resident chunks at 64z, 5184 at 256z (`CHUNKS_PER_SIDE²·CHUNKS_PER_COLUMN`) at the default radius 9; 16,384 at radius 16, 256z.
- One liquid update queue globally; heavy flow re-meshes chunks every tick.
- 200 persisted creature slots in a 64z save (400 in a 256z one, and the real number is
  **derived from the file**, not the version — a save can legitimately have none); 300 live;
  1000 portals; 80 fireworks; 32k vertices per chunk stream (excess silently dropped).
- No multiplayer of any kind; "sharing" is file upload/download.
- Worlds from 2.2.7+ App Store builds ("New Dawn", 256-block-tall, header `version` 5 or 6) load
  and play natively as of 2026-08-06 — see [world-and-terrain.md](world-and-terrain.md) "Runtime
  world height". They never loaded "height-truncated" as the README claimed: before the Stage-0
  guard this build read every column past the first at the wrong stride (garbage) and the next
  autosave silently overwrote the file at 64 blocks tall, destroying it. A version **above** 6 is
  still refused outright rather than guessed at.
  New World offers both heights (New Dawn 256 is its default, `NewWorldMenu.mm`), and a 256z world
  saves keeping its own version.
- Stock: backgrounding does not save; only streaming/warp/exit do. *(Port: also on app background,
  on ring eviction of a modified column and on the 60 s autosave — [save-load.md](save-load.md).)*

## When you change X, also check Y

| If you change… | Also check… |
|---|---|
| Block type enum / tables | Save compat (raw bytes on disk!), `enum BLOCK_TYPES` (`Constants.h`), `blockinfo`, `blockTypeFaces`, `blockColor` (`Globals.mm`), atlas tile, `blockTntMap`, `hudBlocksMap`, HUD picker list (`Hud.mm`), and the type's flags in meshing/physics/fire. **The id space is full at 127** (`block8` is signed; ids 112–127 since Stage D.2a): there is no room to append another type |
| `colorTable` generation | Every painted block in every existing world re-tints |
| A block type's flags (`IS_LIQUID`/`IS_OBJECT`) or `TYPE_CLOUD`/`GLASS`/`WEAVE` | Whether it casts a baked shadow (`g_shadow_caster[]`, `Classes/Shadow.mm`, built from the flags), and so CI's "geometry 64z shadows" `full` if the bundled map holds it |
| `CHUNK_SIZE` | `SIZEOF_COLUMN` (file format!), progress divisors, `INDICES_MAX` sizing |
| `T_SIZE`/`T_RADIUS`/`CHUNKS_PER_SIDE` (window; **runtime** since B.5b-2) | **not** the file format (no save field depends on it; 2026-10-11; `GSIZE` is the literal 2880 since B.5g). Same rules as the height: anything sized at compile time uses `CHUNKS_PER_SIDE_MAX`/`T_SIZE_MAX`; set by `eden_set_window_radius()` before `allocateMemory`; per-frame loops over the window cost (2R)²·bands. Inventory: `WORKING/render-distance-runtime-plan-2026-10-11.md` §2 |
| `T_HEIGHT`/`CHUNKS_PER_COLUMN` (now **runtime** globals) | Anything sized at COMPILE time must use `T_HEIGHT_MAX`/`CHUNKS_PER_COLUMN_MAX`/`MAX_CREATURES_SAVED_MAX` instead — a file-scope VLA is a build error, a stack one is not |
| `WorldFileHeader`/`EntityData`/`ColumnIndex` | All existing saves + the bundled `Eden.emod`/`Eden.eden` + the upload server; the header is also an `.emod` `WORLD_HEADER` record checked against `sizeof(WorldFileHeader)` |
| The `.emod` container (`EdenWorldStore`) | [emod-file-format.md](emod-file-format.md) is normative: change it, `port/tools/emod.py` and the fixture pack `port/tools/fixtures/emod/` together (`--emod-selftest` byte-compares them on every target) |
| zstd (version, level, compile flags) | Byte-identical output on every target (the fixture gate); re-bake `Eden.emod` with `emod.py bake-default` |
| `WorldTrailer` records (`SGN1`/`CMB1`) | `static_assert`ed at 120/528 bytes in `WorldTrailer.h`; existing saves, `emod.py trailer` |
| Meshing (`rebuild2`) | Counting pass and fill pass must agree exactly; `prepareVBO` swap; the CI geometry checksums (`.github/scripts/check-stage1.py`: re-pin only for a deliberate change, and say so in the commit) |
| Shade formula or `SHADOW_REACH_*` (`Shadow.mm`) | The pinned Classic `full` in `check-stage1.py --shadows` |
| A new engine source file | `cmake/EngineSources.cmake` (never `port/tools/engine-sources.txt`); a seam replacement for a stock file goes in `EDEN_NATIVE_SEAM_EXCLUDE` (`native/CMakeLists.txt`) |
| Update order in `World::update` | Streaming/save/mesh/upload dependencies (see execution-flow.md) |
| Anything in a `.h` under `Classes/` | Massive rebuild — headers include each other liberally (Terrain.h ↔ World.h ↔ Player.h circular, broken by forward decls) |
