# Lighting, Liquids, Portals, Fireworks & Effects

Grab-bag of the dynamic systems layered on the voxel grid. Fire/TNT propagation lives
in `Terrain.cpp` and is documented in [world-and-terrain.md](world-and-terrain.md).

## Lighting (root-level `Lighting.cpp` + `Lighting.h`)

**Note the file location:** the compiled lighting code is `Lighting.cpp` in the *repo
root*, not `Classes/` (`Classes/Lighting.c` exists but is not in the build). **The same goes for the
header:** the real one is the repo-root `Lighting.h` (the one `Eden.xcodeproj` lists).
`Classes/Lighting.h` is an empty stock stub with the *same* include guard (`Eden_Lighting_h`), and a
bare `#include "Lighting.h"` from a file in `Classes/` resolves to the stub first. `Terrain.cpp` did
exactly that until R.3 and now imports `"../Lighting.h"`.

- Data (**modified from stock, Stage R / R.3, 2026-10-03**): the light field is one RGB byte-triple
  per voxel of the resident window, toroidally indexed like `blockarray`, but it is no longer stored
  densely. Stock kept `Vector8* lightarray` for every voxel (288·288·`T_HEIGHT`·3 bytes: **15.9 MB at
  64z, 63.7 MB at 256z**) and memset all of it on every load, warp and bulk reload, although only
  voxels within `LIGHT_RADIUS` of a lightbox are ever non-zero. It is now `Vector8** lightbricks`:
  one pointer per toroidal chunk slot (1296 / 5184), each NULL or a 12 KB brick of
  `CHUNK_SIZE`³ triples laid out `CC(x,z,y)`. A brick is allocated the first time `addlight` writes a
  non-zero value into its slot; no brick means no light. Accessors in `Lighting.h`
  (`light_brickSlot`, `light_brickIndex`, `light_get`), store functions in `Lighting.cpp`
  (`light_storeAllocate/Free/Clear`). Entirely disabled on `LOW_MEM_DEVICE`, as before.
  - **Bricks are never freed during play.** (Written for the dormant `MeshPool` workers, deleted in SM.8, which
    called `calcLight` off the main thread; the rule stays as defence in depth.) `light_storeClear()` (what the memset was) NULLs each lit slot,
    zeroes the brick in place and moves it to a pool that `addlight` reuses; only `light_storeFree()`
    in `Terrain::deallocateMemory` frees memory. A worker that loaded a pointer
    before a clear reads zeros or another slot's light, a stale cosmetic value of the same class as
    the dense array's unlocked memset, never freed memory. Pointer loads/stores are
    `__atomic` acquire/release. Peak memory is the most bricks lit at once (bounded by the dense size).
  - **Same values, byte for byte.** `addlight` runs the stock arithmetic on the brick's current value
    (zero when there is no brick) and only allocates if the result is non-zero, so breaking a lightbox
    (brightness −1, clamped at 0) never allocates. `calcLight` adds nothing when there is no brick,
    which is the `0/64.0f` the dense zero added, so the clamp sees the same value. One deliberate
    difference: `calcLight` now answers "no light" for a `y` outside `[0,T_HEIGHT)`, where the dense
    index read a neighbouring strip (or past the array's ends). No caller passes one.
  - **Measured (Mac, `--headless --stage1`, 2026-10-03):** world heap 51.8 → 35.1 MB on `F3 W64`
    (−16.7) and 148.8 → 84.5 MB on `F3 W256` (−64.2); `phys_footprint` 59.6 → 37.9 and 164.4 → 95.7 MB.
    The lit store on those worlds is 8 bricks (6 lightboxes): 119 KB at 64z, 181 KB at 256z including
    the tables. The per-warp memset (16 / 64 MB; 1.2 / 4.2 ms on the A8X, F.3) is now 8 × 12 KB.
- `addlight(x,z,y, brightness, color)` (`Lighting.cpp`, body `light_splat`) — adds (or with
  brightness=−1, subtracts) a spherical falloff light of radius `LIGHT_RADIUS 5`:
  `contribution = 64·(1−dist/5)·brightness·color`, clamped 0..255 per channel.
  Called when lightboxes are placed/destroyed/repainted.
  **No toroidal wrap (modified from stock, B.5b-1c, 2026-10-12).** Stock wrote every voxel of the
  sphere through `(x+g_offcx)%T_SIZE`, so a lightbox within 5 blocks of the window edge also lit the
  *opposite* edge's column. A voxel whose column is not resident (`isColumnResident`, one `pbounds`
  compare per (x,z) of the sphere) now gets nothing. Every writer — edits, both sweeps, the per-column
  rebuild below — goes through `light_splat`, so a load and a walk agree on what the edge gets.
- `calculateLighting()` (`Lighting.cpp`) — full rebuild: finds every `TYPE_LIGHTBOX` in the resident
  window and re-adds its light, refreshing affected chunk meshes.
  Triggered via `updateLightingBegin()` (sets `update_lighting`, clears the brick store with
  `light_storeClear()`, resets the sliced sweep cursor) after world
  load, warps and bulk reloads — lights are **not** persisted, they're re-derived from lightbox
  blocks. The per-column body is `sweepLightingColumn(cx,cz,hit)`, shared with the sliced form below
  and with the per-column rebuild; a sweep hit goes through `sweepLightingHit` (`addlight` +
  `refreshChunksInRadius`).
- **Light per landed column under ring streaming (B.5b-1c, 2026-10-12).** A walk makes no bulk
  reloads, so no sweep: instead `light_syncLandedColumns()` runs once a frame in
  `prepareAndLoadGeometry`, after the ring lands its columns and before the meshing pass, whenever
  no full sweep is pending (`!update_lighting && !bulk_reload_active`). It keeps, per column slot,
  the world column the slot's light was computed for (`lit_col`; recorded for the whole window when a
  sweep *starts*), and for every slot whose occupant changed — a ring landing, or a decode a worker
  published — calls `light_rebuildNeighbourhood`:
  - **Why the 3×3 of slots is enough.** A landing changes the light of the departing column's
    spatial neighbours (its lightboxes stop counting) and the newcomer's (its start). A resident
    column at world offset d from either sits in slot S+d mod 18, so both sets are inside the 3×3 of
    *slots* around S (on the trailing and the leading edge respectively). Everything else is untouched.
  - **How.** The 3×3 is rebuilt into a private scratch brick table from the lightboxes of the 5×5 of
    slots around S (`sweepLightingColumn` with a hit that splats into scratch, clipped by a slot
    mask), then published brick by brick: unchanged bricks are not touched, changed ones are copied
    in place (or taken from / returned to the pool) and their chunk marked for re-meshing
    (`addToUpdateList2`). A mesh worker reading the live table therefore never sees a cleared brick
    that did not change, and one that did is re-meshed anyway. Scratch is at most 9 columns of bricks.
  - **Byte-identical to the sweep.** The 5×5 is visited in ascending slot order, the order the sweep's
    cursor walks the window, so every voxel gets the same contributions in the same order. (Each
    contribution is truncated to an integer and `cur+d` is a float add, so order is *nearly* but not
    provably free; the same order makes it moot.)
  - **A sweep in flight.** Columns that land while a sliced sweep is running differ from the `lit_col`
    recorded at its start, so their 3×3s are rebuilt the frame after it finishes; that also removes
    whatever the sweep wrote for their departed occupants.
  - **Cost** (Mac, Release, 784 lightboxes over the walk area, ~0.5 per column): 0.10 ms per landed
    column on average, 0.15 ms worst at 64z; 0.16 / 0.35 ms at 256z. `eden_debug_light_state()` reports
    `rebuilds`, `rebuild_ms`, `rebuild_max_ms` (the times only with `EDEN_F3_HOOKS`). Since P.10 most of
    that cost is gone (next bullet).
  - **Lightbox-free columns are not rescanned (P.10, 2026-10-14; modified from stock).** Of the 25
    slots a rebuild visits, 24 were scanned by the previous landings, and on a world with no
    lightboxes the scan *was* the cost (0.51 / 1.01 ms per landed column on the A8X at 64z / 256z).
    `Lighting.cpp` keeps, per column slot, a "scanned, holds no lightbox" byte (`lbfree`) and the world
    column it was true for (`lbfree_col`); `sweepLightingColumn` returns 0 at once while both hold. Only
    "none" is ever trusted — a slot with lightboxes is always scanned — so the one way to be wrong is
    a lightbox appearing behind the flag's back, and every route clears it: an occupant change
    (checked on each use), `Terrain::addChunk` (`light_markLanded`), `Terrain::setLand` of
    `TYPE_LIGHTBOX` (`light_onSetLand`; every runtime edit lands there), and `light_storeClear` /
    the start of any full sweep (every load, warp, bulk reload, `clearBlocks`). Only the regular strip
    path records "none"; an irregular column is scanned every time. Mac, Release, `--stage1 --walk=
    160,0,0,160` from 65536 with no lightboxes (360 rebuilds, mean of 3 reps after a discarded one):
    **0.215 → 0.011 ms per rebuild at 64z, 0.369 → 0.016 ms at 256z**, walked `geom` unchanged.
    `eden_debug_light_state()` reports `cols_scanned` / `cols_skipped`.
  - **Gates.** `WORKING/b5b1/ring-light-gate.sh H OUT` (fresh load at the end of 7 walks on a lightbox
    lattice world at night = walked geometry, chunk state and light) and native `--ring-light-selftest`
    (the in-process CI form: walked light + meshes = a full re-sweep + re-mesh; since P.10 it also
    builds a lightbox in a remembered-free column at the +x edge and walks one more chunk so that
    column's 3×3 is rebuilt). `--light-mutate=nosync` and `=nomark` fail both; `=nolbmark` (P.10: the
    landing/edit hooks never clear `lbfree`) fails the selftest's P.10 step, light and mesh checks;
    `=wrap` fails `--light-selftest`'s window-edge step. **Test at night**:
    by day `TerrainChunk` never calls `calcLight` (`skylight==1.0`), so a mesh baked from stale light is
    invisible to every checksum; the lattice prep paints all 16 sky regions night (palette 54) and saves.
- **The scan is a `memchr` (Stage R / R.1, 2026-10-03).** `blockarray` is y-fastest, so each (x,z) of a
  column is one contiguous `T_HEIGHT`-byte strip; `sweepLightingColumn` runs `memchr` for
  `TYPE_LIGHTBOX` over each strip instead of a `getLandc` per voxel (~5.3M calls at 64z, ~21M at 256z,
  each a toroidal-index multiply plus two modulos). It reads the same bytes `getLandc` reads, so it
  finds the same lightboxes. Two things to know:
  - **Order changed, result did not.** Hits now arrive in (x,z,y) order, not (cy,y,x,z). `addlight`
    only adds and saturates at 255, so the final byte is order-independent, and `chunksToUpdate` is a
    set. This was **proved, not assumed**: `lightarray` FNV-1a hashes are identical before and after on
    a lightbox-bearing world at 64z and 256z (below).
  - **A column that is not the regular one takes the old per-voxel loop.** The strip form needs every
    chunk of the column present, stacked at `cy*CHUNK_SIZE` and sharing x/z bounds; otherwise (a
    deferred or unpublished slot) the old per-chunk scan runs, honouring each chunk's own `pbounds`.
    That fallback was kept verbatim, not deleted.
- `calculateLightingSlice()` (`Lighting.cpp`, port perf change 2026-08-27) — the
  same sweep but a budgeted strip of columns per frame, holding a cursor between frames and
  returning TRUE only when the window is fully swept. This is what the **post-bulk-reload**
  `update_lighting` path calls: the old full scan was one unbudgeted ~20 ms (64z) / ~80 ms (256z)
  main-thread stall per teleport/warp. `updateLightingBegin()` calls `calculateLightingSliceReset()`
  so a second teleport mid-sweep restarts from column 0.
  - **Budget (R.1): `LIGHTING_SWEEP_COLUMN_BUDGET 108` columns and `LIGHTING_SWEEP_HIT_BUDGET 32`
    lightboxes per call**, whichever comes first (the hit check is at column boundaries). It was
    `LIGHTING_SWEEP_CHUNK_BUDGET 256` *chunks*, which is 64 columns at 64z but 16 at 256z, so a 324-column
    warp took 6 / 21 calls. A column now costs 256 memchr calls over `T_HEIGHT` bytes, so cost per
    column grows only with memchr bytes and a 108-column call is ~28k calls and ~1.8 MB (64z) / ~7 MB
    (256z). **A 324-column window takes 3 calls at any height.** The hit budget bounds the other cost,
    since `addlight` is ~1.3k voxels of `sqrtf` plus a refresh.
  - **Measured on the Mac (R.1, same machine, before vs after, medians of 3):** slices 10.1 → 0.6 ms
    (64z) and 41.1 → 1.4 ms (256z); calls 6 / 21 → 3 / 3; E1 warp 120 → 80 ms (64z, 6 → 3 frames) and
    361 → 121 ms (256z, 21 → 3 frames). Detail in `WORKING/ROADMAP.md` R.1.
  - **Measured on an iPad Air 2 before R.1 (F.3, 2026-10-03):** ~42 ms (64z) / ~198 ms (256z) per
    warp in 6 / 21 calls, content-independent (190–206 ms with 0 or with 6 lightboxes), because the scan
    was per voxel, not per hit. After a walk-triggered reload the sweep starts only after the mesh
    drain, so the event's frames are `mesh span + sweep calls`. Not re-measured on the device yet (R.4).
- **Gate for any change to the sweep or the store: hash the light, on a world that has lightboxes.** The
  `--headless --stage1` corner fixture has **zero** lightboxes in its window (`hits=0`), so it passes
  whatever the sweep does. And the `full` mesh checksum is not enough either: with one of six lightboxes
  deliberately skipped, `lightarray` changed (`44f3…` → `01ca…` at 64z) but `geom` and `full` stayed
  `8f02…` / `349d…`. Use a world with lightboxes (Diane's home window has 6; `F3 W64` is its lossless
  `--to-64` copy, `--at=65408,30,66288`) and compare an FNV-1a hash of the light after the sweep
  completes, and run it once with the bug put back. Since R.3 that hash is permanent:
  `eden_debug_light_state()` (`port/src/seam/DebugState.cpp`; `.light` on native `--stage1`) walks
  every voxel in the dense order through `lightAtToroidal()`, so it reads the same for the dense array
  and the bricks (`580637df278f24c9` on W64, `7fd87220d15161e5` on W256, 2732 lit voxels, both).
  **`--stage1` alone does not exercise brick reuse:** a deliberately broken clear (bricks pooled
  without being zeroed) left the stage1 hash unchanged, because that run only ever sweeps into fresh
  bricks. `--light-selftest` (place / saturate / repaint / break through `buildBlock`, `paintBlock`,
  `destroyBlock`, across the toroidal seam and both y clips, then a fresh sweep) caught it at once.
  `--light-selfcheck` keeps the stock dense array beside the bricks, written by the stock code, and
  reports voxels where they differ (`mismatch`, must be 0).
- Consumption: `calcLight(x,z,y, skylight, channel)` (`Terrain.cpp`) adds
  `light/64` to the base skylight and clamps to 1.5; the mesher bakes this into
  vertex colors. Skylight is 1.0 by day, 0.35 when the sky color is the night palette
  entry (`colorTable[54]`). There is no sun direction; `getShadow` still returns a constant 1.0
  and the 1.x `shadowarray` code stays commented out as archaeology. The port's replacement is the
  baked top-face shadows below (H.2).

### Baked shadows (`Classes/Shadow.{h,mm}`) — *modified from stock (H.2, 2026-10-14)*
The 1.x feature (top faces under a canopy, roof or overhang are darker), restored on the port's
terms. Plan and gates: `WORKING/feature-archaeology-2026-10-13/shadows-plan.md`.
- **Setting:** Video › *Shadows* = Off / Classic / Smooth (`shadows` in `Settings.cpp`; engine
  global `g_shadow_mode`). **Default Off** until the user's T look picks the shipped one. Not in
  the graphics presets (CPU meshing only). Native `--shadows=off|classic|smooth` is a
  **non-persisting override** (`shadow_setOverride`): native prefs are global, not per `--docs`, so
  a test must not write the player's setting.
- **Data (nearest caster, 2026-10-23):** the shade reads the **nearest caster above each face**, not the
  column's topmost, so canopies, balconies, mall roofs and cave ceilings shade the floor below them
  (a tester saw no shadows indoors/underground when only the topmost block counted). `shadow_gather(x0,z0,y0,out)`
  builds, per mesh, an 18×18-column × 16-y table of gaps (`SHADOW_GATHER_BYTES` = 5184 B) straight from
  `blockarray` (≤ 39 blocks scanned per column); 0 = no caster within reach. With a single layer above
  a face it equals the old topmost-only value. `shadowTop[T_SIZE*T_SIZE]` (the y of each column's topmost
  caster, toroidal like `GBLOCK`, 83 KB at R9) survives only as the diagnostic `--shadow-selftest` checks
  (`stale`). Derived, never saved — no format change. Casters: everything except air, cloud, liquids (every level), glass, weave and `IS_OBJECT` blocks
  (`g_shadow_caster[]`, built from `blockinfo` flags, so a new block type needs no table edit).
- **Shade:** a covering column's weight is 16 when its nearest caster is 1–8 blocks above the face's block,
  fading to 0 at 24 (`SHADOW_REACH_FULL/END`), so deep caves keep today's brightness and the
  ground under a sky island is not shaded. Classic: the 3×3 of columns, `255-100*S/144`
  (155..255; exactly 1.x's `255-100*count/9` when every gap ≤ 8). Smooth: per vertex, the 2×2
  meeting at each corner, `255-100*S/64`. Integer, so every target agrees.
- **Applied to the sky term only:** `light_top = calcLight(x,z,y, skylight*s, c)` (`s = shade/255`),
  and by day capped at 1.0 so a shaded face beside a lightbox never outshines an unshaded one. Only
  `f==5` (top) faces; side faces are not shaded (deferred to the T look). `top_shadow` in
  `rebuild2()` stays dead: it carries the ramp factor, and reviving it would darken ramp tops even
  with shadows Off. The light store itself is untouched (`--shadow-selftest` step 4 checks its hash).
- **Upkeep (on or off):** `Terrain::addChunk` flags the column; `shadow_syncLandedColumns()` rescans
  flagged columns right before the mesh pass in `prepareAndLoadGeometry`; `Terrain::setLand` calls
  `shadow_onSetLand(x,z,y,oldtype,type)` after its `GBLOCK` write (keeps `shadowTop`; cheap).
  Only with shadows on: a block **turning into or out of a caster** (any y, not just the top) dirties the
  chunks covering the 3×3 of columns × `[y−23, y−1]`,
  and `columnMeshable` also waits for the 4 diagonal columns. So **Off schedules and meshes
  byte-for-byte like stock** (same `geom`/`full`, same mesh-bench time).
- `rebuild2()` gathers the 18×18 tops itself (`shadow_gather`). Colours only — the counting pass
  never sees it.
- **Cost (Mac, `--mesh-bench=31`, 3 interleaved runs):** Classic +2.2 % (64z) / +1.1 % (256z) per
  re-mesh, Smooth +3.9 % / +3.1 %; Off = stock.
- **Gates:** `--shadow-selftest` (CI runs it at the default radius and at `--radius=16`; `--height=256`
  is a manual leg; `--shadow-mutate=nohook|nodirty|
  nolanding|nodiag` must each FAIL), and CI's "geometry 64z shadows" leg (`geom` unchanged, `full`
  pinned in `check-stage1.py`). Probes: `eden_debug_shadow_state` (`stale` must be 0),
  `eden_debug_top_shade`, `eden_debug_chunk_mesh_hash`.

## Liquids (`Classes/Liquids.cpp`)

Water and lava are voxels with the fill level encoded in the type
(`TYPE_WATER` full=4 → `WATER3/2/1`; same for lava). Helpers: `getLevel(type)`,
`getBaseType(type)`, `genLevel(baseType, level)`. (`getLevel`/`getBaseType` are `static inline` in `Liquids.h` since P.8.)

- `addSource(x,z,y)` — pushes onto `plist` (spread queue);
  `removeSource` — onto `plist2` (drain queue with the removed type/level).
- `Liquids::update(etime)` runs on a delay timer and processes queues:
  spread flows downhill first (full-level block below), then sideways with
  decreasing level (`genLevel(type, level−1)`), inheriting the source's color;
  lava additionally refuses to flow into the player (`player->test`). Draining
  reverses it, removing dependent flow blocks. Every change goes through
  `Terrain::updateChunks`, so liquid motion re-meshes chunks continuously —
  the main reason big waterfalls tank the frame rate.
- `clearLiquids()` — called by `endDynamics` (save, overload valve): stops all flow
  but leaves the liquid blocks as-is.
- **B.5b-1b (2026-10-12):** `dropColumn(cx,cz)` drops the nodes of one chunk column as ring
  streaming evicts it (it unlinks properly; stock `removePoint` sets `plist=NULL` on a head removal,
  orphaning the rest, and is left alone), and the spread skips a neighbour whose column is not
  resident (`isColumnResident`) instead of writing through the toroidal alias.
  `countOutsideWindow()` is the probe `--evict-selftest` asserts is 0.
- Player/creature interaction: `inLiquid`, `getFlowDirection` (push), lava damage.
- Surface rendering (slopes, animated texture) is in the mesher — see
  [rendering.md](rendering.md).
- Note: the `WetNode`/`wetmap` machinery in the header is a dead earlier design;
  `updateHeights` returns FALSE immediately.

## Portals (`Classes/Portal.cpp`)

- Registry of up to `MAX_PORTAL 1000` `sportal{x,y,z,dir,color}` records.
- Populated **from the mesher**: when `rebuild2` encounters `TYPE_PORTAL_TOP` it calls
  `portals->addPortal(...)` (from `TerrainChunk`'s mesher) — so the registry always reflects
  currently-resident chunks. `removeAllPortals` on unload.
- `enterPortal(x,y,z, vel)` — finds the **next portal of the same color** in the
  registry (cycling, so pairs/chains work) and returns destination + exit vector;
  `Player::move` performs the travel (a save+reload warp if outside the window).
- Rendered as extracted `StaticObject`s with the swirl texture and rotating UVs
  (`Terrain::render`), plus the frame blocks as normal geometry.

## Fireworks (`Classes/Firework.cpp`)

`sfirework{pos, color, fuse, vel}` pool (`MAX_FIREWORK 80`). A burning
`TYPE_FIREWORK` block launches one (`Terrain::shootFirework`): rises with a fuse,
then explodes into particles colored by the block's paint
(`SpecialEffects::addFirework`). While rising it is drawn as a small spinning, lit, textured cube
(`Graphics::drawFirework`, `blockTypeFaces[TYPE_CLOUD]` faces, `GL_LIGHT0`).

## Particles & block-break effects (`SpecialEffects.cpp`, `Fire.cpp`, `BlockBreak.cpp`)

`SpecialEffects` is the façade `World` owns:
- `addBlockBreak/addBlockExplode(x,z,y,type,color)` — `BlockBreak.cpp` spawns textured
  debris cubes with physics using the broken block's texture and paint.
- `addFire(x,z,y,type,life)` / `removeFire(pid)` / `updateFire` — `Fire.cpp` flame +
  smoke particle emitters (point sprites, `vertexpStruct`); ids returned so the
  burn system (`BurnNode.pid`) can extinguish emitters.
- `addCreatureVanish(x,z,y,color,type)` — the poof used by TNT/creature death.
- `clearAllEffects()` from `endDynamics`/world load.
Buffer cap: `pbuffer_size 10000` particles.

## Sky color system (partly in `TerrainGen2.cpp`)

Per-world 4×4 grid of sky palette indices (`regionSkyColors`, persisted in the header
as `skycolors[16]`). `updateSkyColor1/2` map the player's position in the default
world onto the grid and set `terrain->final_skycolor`; `Terrain::update` interpolates
`skycolor` toward it (X.6: the region index clamps to [0,3]; it used to clamp to 4, one past the
array, reachable on flat worlds and on unbounded worlds past the bundled square) and updates the fog color; `Terrain::render` crossfades the
colored/B&W skybox textures. The sky quad (`Texture2D::drawSky`, the port's copy is
`port/src/seam/Texture2D.cpp`) draws *inside* `render`'s atlas texture matrix `glScalef(1,1/ATLAS_TILES,1)` (`R::uv()`
since G.5b-6, which also hands it its tint and state explicitly: `docs/rendering.md` sky note), so
its t range is `0..ATLAS_TILES` (stock wrote `32`, then the atlas size; left at 32 after D.2t it showed only the top
quarter of `sky_box.png`, losing the pale horizon the fog colour blends into: X.8). Pitch picks the rows: level shows
the whole gradient, looking down shows its pale bottom. Painting the sky (`paintSky`) recolors the region the
player is in — a signature Eden feature. Night = palette entry 54 (dims skylight and
GL lights for doors/creatures).

## Common pitfalls
- Lighting rebuilds re-mesh chunks in radius; a wall of lightboxes on a streaming
  boundary causes visible hitching. The post-reload rebuild is now sliced
  (`calculateLightingSlice`, a `memchr` per strip) so the *scan* no longer stalls a frame, but a dense
  lightbox cluster can still spread its re-meshes across the following frames.
- Liquids and fire both mutate terrain during `Terrain::update` — never cache raw
  block pointers across it.
- Portal registry rebuilds via meshing means a portal in a *not-yet-meshed* chunk is
  briefly unknown to `enterPortal`.
- `endDynamics` (warp, exit, Save button, app background, bulk reload) silently kills active fire
  fronts and liquid queues — by design, but surprising when testing fire features. Since B.5b-1b the
  walk's evict-saves and the autosave no longer do; only the evicted column's own nodes go.

## Safe vs. risky to modify
- **Safe:** light radius/intensity, liquid spread rates, particle counts/lifetimes,
  firework physics.
- **Caution:** liquid queue processing order (spread-vs-drain interleaving prevents
  infinite loops), the mesher→portal registry coupling, lighting rebuild triggers.

## `startDynamics()` is a stub
`Terrain::startDynamics()` (`Terrain.cpp`) has an empty body: the original full-window liquid rescan is
commented out. So liquids that were flowing when a world was saved stay still after it loads until
something disturbs them. Restoring the rescan re-floods worlds (every source in the window restarts),
so it was parked on purpose (ROADMAP Stage H, report item 8).

### Q.1a notes (2026-10-21)
- **Q.1a:** F14 `Fire::addFire`/`addSmoke` return -1 when the node list is full; F15 `addFirework` overwrites the last slot when full (stock wrote one past the array); F26 `calcLight3` fetches the light once for all three channels (colours only).
