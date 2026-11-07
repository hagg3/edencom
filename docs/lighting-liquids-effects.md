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
  vertex colors. Skylight is 1.0 by day, 0.35 at night: `sky_isNight()` (`Classes/SkyCycle`), which is the
  painted sky being the night palette entry (`colorTable[54]`, `sky_paintedNight()`) unless Experiments ›
  *Day/night cycle* is on (Stage DN; then the clock's bit, flipped with hysteresis at -4.5° / -3.5° of sun
  elevation, each flip one full-window re-mesh). **DN.5 (2026-10-31), cycle on only:** each chunk records the
  bit it was baked with (`TerrainChunk::bakedNight`, published by `prepareVBO`) and is drawn under
  `R::setWorldLight(SkyCycle::worldLight(bakedNight))`: `L(t)` for a day bake, `L(t)/0.35` for a night one
  (times a warm/cool tint), so the two match away from lamps and the flip's re-mesh streams in without a
  patchwork (`--sky-shot` flip frames: ≤ 1 level). `L` is 1.0 above 15°, 0.35 at -4°, 0.2625 by -18°. Flowers
  take the current bit's light; door / golden-cube / creature LightSets blend the stock day and night values
  (`SkyCycle::blendLight`). Cycle off, none of this runs (`--scene-shot` 0/66). There is no sun direction in the lighting; `getShadow` still returns a constant 1.0
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

### Day/night cycle (Stage DN, World › *Day/night cycle*, default off)

Cycle off, everything above is unchanged (`--scene-shot` 0/66, `--stage1` pins). On, the painted regions are
ignored (they stay saved; the sky brush toasts that the clock owns the sky) and:
- **Clock** (`Classes/SkyCycle`): `t` in 0..1 (0 midnight, 0.25 sunrise, 0.5 noon, 0.75 sunset; new worlds
  start at 0.30), full day = *Day length* (10/20/40/60 min, `experiments.day_length`). `update` runs from
  `World::update` and stops under the pause menu. `timefreeze` / World Properties › *Lock time of day* hold
  it; `time T` / `day` / `night` set it (`world.time`). Saved per world as `.emod` record 11 `SKY_CLOCK`
  (`docs/emod-file-format.md`, `docs/save-load.md`); a multiplayer session has its own clock, reset once at
  `MpWorld::beginSession`, never saved.
- **Sky** (`Classes/SkyLook`, `Terrain::render`'s sky pass, one `SkyCycle::enabled()` branch ahead of the stock
  sky): 7 sun-elevation keyframes (zenith / horizon / glow colours) drawn as a 12-row vertex-colour gradient
  that follows camera pitch; a dawn/dusk glow band, stronger facing the sun; 400 fixed-seed pixel stars
  (night only) rotated about the sun axis and projected on the CPU each frame (≤ 2 point draws, no static
  buffer). `Terrain::update` sets the fog to the keyframe's horizon colour while on.
- **Sun and moon** (`SkyCycle::drawSunMoon`): `media/sky_sprites.png` (32×16, stepped 16×16 sun left, moon
  right, white; the tint colours them), ~5° tall snapped to 16-point multiples, drawn through
  `Texture2D::drawSkySprite` after the sky and before terrain, so terrain occludes them.
- **Light:** see the skylight paragraph above (`R::setWorldLight`, `bakedNight`, hysteresis, one re-mesh per
  flip, two flips a day). Harness: `--sky-shot[=PREFIX] [--sky-times=…]` renders frames at fixed clock times.

## Weather (`Classes/Weather`, Stage AT)

AT.1b is the state; AT.2a draws overcast, AT.2b rain, AT.3 snow, AT.4 lightning, AT.5 mist, AT.6 clouds (all below; splashes are AT.2c, in Rain; clouds need only Video › *Atmosphere*, not the weather). Off by default (Gameplay › *Weather*,
`eden_weather_enabled()`, overridden for one run by `--weather=`): then `Weather::update` returns at once and
nothing is saved. On (with or without the day/night cycle):
- **State:** a spell's `kind` (clear / overcast / rain / storm), an `intensity` easing toward the kind's target
  (0 / 0.4 / 0.7 / 1) at 1/45 per second (never a pop), seconds left, a lock and a "snow everywhere" flag.
  `update` runs from `World::update` right after `SkyCycle::update` and stops under the pause menu.
- **Schedule:** a Markov walk on clear – overcast – rain – storm (`g_next` in `Weather.cpp`: only neighbours,
  repeats allowed), seeded per world from `LEVEL_SEED ^ "WEAT"` (splitmix64). Spell lengths are mean × [0.5, 1.5)
  in-game **days** (×`SkyCycle` day length, 20 min with the cycle off), so any *Day length* gets the same weather
  per day. Defaults give time shares 0.60 / 0.20 / 0.15 / 0.05 (`--weather-selftest=schedule` measures them). SX.9: Settings › World › *Weather frequency* scales clear spells (Rare ×2, Normal ×1, Often ×0.4) and *Storms* scales the rain → storm weight (Off ×0, Rare ×0.4, Normal ×1, Often ×2.5; the row is renormalised); Normal/Normal is the schedule above.
- **Look inputs** (identity at intensity 0, so clear changes nothing): `lightMul()` 1 → 0.7 overcast → 0.55
  storm; `skyGrey()`; `fogScale()`; `precipAmount()` (0 up to overcast's intensity, 1 in a full storm).
- **What falls** (`precipAt(x, z, y)`, plan A5): on the default seed the bed picker's 4×4 region grid
  (`eden_default_region`, shared with `Player.cpp`) makes (0,0) the snow mountain snow and (0,1) pyramid,
  (0,2) oasis dry; elsewhere snow at and above `weather.snowline` × `T_HEIGHT` (cvar, default 0.75; since SX.6 a persisted Settings row on World › *Weather tuning*), else
  rain. The snow flag makes it snow everywhere until the spell ends.
- **Saved** as `.emod` record 12 `WEATHER` (`docs/emod-file-format.md`), copying `SKY_CLOCK`'s rules; a
  multiplayer session resets it at `MpWorld::beginSession` and never saves it.
- **Overcast look (AT.2a).** No re-mesh ever (a dark storm never flips to the night bake). With weather on:
  - *Light:* `SkyCycle::worldLight` is multiplied by `lightMul()` (cycle off: the base is (1,1,1)), and the
    `dnLight` gate at all three `Terrain.cpp` sites (opaque chunks + doors/golden cubes, transparent chunks,
    flowers) is `SkyCycle::lightActive()` = cycle **or** weather. Portals stay (1,1,1). The LightSet-lit doors
    and golden cubes (Original lighting) get the same multiplier on their ambient/diffuse; the stock day/night
    pick for those LightSets stays keyed on the cycle alone (`SkyCycle::enabled()`).
  - *Sky, cycle on:* `SkyLook::lookFor` greys the keyframe zenith/horizon/glow colours (`Weather::greySky`:
    toward luma × 0.8 by `skyGrey()`), and the glow, stars, sun and moon fade by `clearSky()` (1 − cover).
    `SkyLook::fogColor` is therefore greyed too.
  - *Sky, cycle off:* one extra blended `ICO_SKY_BOX_BW` pass (depth `-P_ZFAR/1.000006`, nearer than the stock
    overlays) tinted the sky colour's grey at alpha `skyGrey()`, only while `skyGrey() > 0`; the fog colour is
    greyed in `Graphics::applyWorldFog` (only with the cycle off, as the cycle's is already greyed).
  - Fog *distance* (`fogScale()`) is AT.5's (Mist, below). Weather off or clear is byte-identical (A/B `--sky-shot`, 16/16
    frames, `--weather=clear` too). References: `TESTERS/atmosphere/at2a/overcast-*` (`--sky-shot
    --weather=overcast --sky-times=0,0.26,0.5,0.76`; the run adds `nocycle-level/up` frames).
- **Splashes (AT.2c, in `Precip.cpp`).** High only, with `atmo.splash` on, while rain draws: a second
  `R::drawPrecip` with `PrecipParams::splash` (same program, buffer and column-top map; no new resource). The
  first `SPLASH_HIGH` (1200) × `precipAmount()` seeds become flat quads on their column's top (+0.04): the seed's
  x/z pick a column fixed in the world, its y a phase in a 1.1 s cycle (`g_splashPh`, passed as `length`); a
  quarter of the cycle shows, a ring growing 0.10 → 0.38 blocks while it fades. AT.11: the ring is hard-edged (the
  fragment stage keeps only the outer cells of an 8 × 8 grid across the quad, `v_side.z` = 1 marks splash mode),
  alpha 0.70; it was a soft blob nobody could see. Sheltered columns splash on
  their roof; a column with top 0 shows none. `--weather-selftest=precip` checks Low = none, High = the count.
- **Snow (AT.3, same `Precip` / `P_PRECIP`).** When `Weather::precipAt` at the eye says snow (A5: region (0,0) on the default seed, above `weather.snowline` elsewhere, or `weather snow`), `Precip::render` draws the same seed buffer in flake mode (`PrecipParams::snow`, `u_ptops.w = 2`): camera-facing squares (1× or 2× by seed, ≥ `widthPerDist` × distance and, AT.11, ≤ 2 × `widthPerDist` × distance, so a near flake is a few pixels, not a screen-filling square; rain streaks take the same cap on their width), swaying ±0.35 block, falling at `SNOW_SPEED` (2.2 blocks/s, own clock). Same shelter map and fade; no splashes. Storm in a cold cell is simply heavy snow (no lightning exists yet). Shot: `--sky-shot --weather=snow --atmo=high` adds `snow-open` (refs `TESTERS/atmosphere/at3/shots/`).
- **Rain (AT.2b, `Classes/Precip.{h,cpp}`).** Drawn only when Video › *Atmosphere* is Low/High, `atmo.precip` is
  on, weather is on, `precipAmount() > 0` and `precipAt(eye)` is rain (snow is AT.3; dry draws nothing).
  - *One draw* (`R::drawPrecip`, program `P_PRECIP`, end of the effects pass, render-local modelview): a static
    buffer of 4000 streak seeds (`kFmtPrecip`, 4 B a vertex, 6 a streak, fixed splitmix stream so shots repeat),
    uploaded once. The vertex shader places each head at `seed × box + offset` wrapped into a 32 × 32 × 32 box
    round the eye (12 below, 20 above), so drops stay put in the world as the box follows; the tail leans
    up-wind. Per frame only uniforms move (`Precip::update` advances fall 14 blocks/s and a drift, stronger in a
    storm, and stops under the pause menu). Streaks drawn = cap × `precipAmount()`, caps Low 1500 / High 4000.
    Alpha fades toward the box edges and top; colour × `SkyCycle::worldLight` (night and overcast rain is dim).
    Blended, depth-tested, no depth write, no discard, no fog.
  - *Shelter:* a 32 × 32 column-top cache (`TF_L8` texture, by world `(x & 31, z & 31)`; value = the y above
    the column's highest **non-air** block, so glass, water, leaves and objects stop rain too: `shadowTop[]`
    excludes glass, liquids and cloud, which is why it is not reused). A head below its column's top collapses
    the streak outside the clip volume. A column is scanned (from the top, skipping empty bands) only when it
    enters the box, when its chunk column lands (`Terrain::addChunk` → `Precip::onLanded`) or when
    `Terrain::setLand` lowers its top (`Precip::onSetLand`, which raises it without a scan); only changed rows
    upload. A write that bypasses `setLand`/`addChunk` would not reach the cache (none known).
  - *Off = absent:* tier Off, `atmo.precip` off or weather off frees the texture and buffer on the next frame;
    `Terrain::clearBlocks`/`deallocateMemory` free them too; `P_PRECIP` is built on first use.
  - Gates: `--weather-selftest=precip` (off = absent, caps × intensity, G3: 600 frames scan/upload/allocate/
    re-mesh nothing, a roof placed and removed, the cache following a jump); `--sky-shot --weather=rain
    --atmo=high` adds `rain-open` / `rain-roof` (noon, snowline lifted, a 7×7 stone roof 3 above the eye: no
    streaks under it); references `TESTERS/atmosphere/at2b/shots/`.

- **Lightning (AT.4, `Weather::lightningUpdate`, `flashMul()`)**: only in a storm at full strength, never where
  `precipAt` says snow (a cold storm is heavy snow, no lightning). Strikes 4–12 s apart (dry regions 8–20 s). A
  strike multiplies the world light (`lightMul()`) and the sky/fog colours (`greySky()`) by `flashMul()`: up to
  **1.5** in one frame, linear fade over 0.3 s, 40 % of strikes add a 0.6 flash at +0.4 s, so at most 2 flashes in
  any second. **Video › *Reduce flashing*** swaps that for a dim (`flashMul()` 1 → 0.85 → 1 over 0.5 s, sine,
  never above 1; no second flash). Thunder (`S_THUNDER`, `Resources::playThunder`, no repeat filter) follows
  0.5–4 s later: delay < 1.5 s plays `thunder_01` (close), < 2.8 s `_02`, else `_03`; dry regions are always 2.5–4 s
  (distant). The three clips are synthesised in-house (filtered brown noise, `CREDITS-weather.txt`). Own RNG
  (not the schedule's `rng`), nothing saved, no night remesh (P6). No bolt sprite (the plan's optional item was
  left out). Gate: `--weather-selftest=lightning` (peak ≤ 1.5, ≤ 2 flashes/s, reduce never brightens, delay
  0.5–4 s, dry rarer + distant, off = exactly 1).
- **Mist (AT.5).** Three parts, all exactly absent with the weather off (`Weather::fogScale()` 1, `mist()` 0):
  - *Fog distance, every tier:* `Graphics::applyWorldFog` multiplies the stock start/end (`P_ZFAR − P_ZFAR/1.6`,
    `P_ZFAR − 30`) by `fogScale()` = (1 − 0.10 cover − 0.30 heavy) × (1 − 0.30 `dawnMist()`): 0.9 overcast, 0.6
    a full storm, 0.7 at a clear sunrise. `dawnMist()` is a bump around SkyCycle time 0.26 (± 0.07; 0 with the
    cycle off). Uniforms only; the fog **colour** is left as the sky's, so a culled chunk (below) shows the sky
    exactly where a fully fogged one would have shown nearly the same colour (the stock far-plane cut looks the same).
  - *Height fog (valley mist), High + `atmo.height_fog` only (plan P3):* `Graphics::applyHeightFog` (from
    `beginTerrain` and `Terrain::render2`, before the ×0.25 push; `endTerrain` turns it off) states
    `R::setHeightFog(plane, params)` only when `mist()` = max(`dawnMist()`, `precipAmount()`) > 0. The plane maps
    an eye-space point to world y (derived from the view matrix); params are the mist top (`weather.mistheight`
    × `T_HEIGHT`, cvar default 0.4; SX.6: a persisted Settings row, World › *Weather tuning*), 1/thickness (full 0.12 × `T_HEIGHT` below the top), 1/48 (full strength 48
    blocks away) and strength 0.85 × `mist()`. `drawWorld` then uses the `EDEN_HEIGHT_FOG` twins of program 1
    (`world-hfog`, `world-opaque-hfog`, built on first use): `f = min(f, 1 − s·depth·distance)`. Off, Low or no
    mist: the plain programs (no extra varying or ALU on the Air 2). Doors/golden cubes on `drawLit`, creatures
    and sprites keep the plain fog.
  - *Culling past the fog (plan S3):* while `fogScale() < 1`, `renderTree` skips chunks whose box is wholly more
    than `Graphics::mistCullDistance()` + 1 (the pulled-in fog end) from the eye; `P_ZFAR` and the sky are
    untouched. Counter `eden_mist_culled()` (a storm at the 64z site: ~340 chunks a frame). AT.9 measures the saving.
  - Gates: `--weather-selftest=mist` (off identities, dawn bump, height fog on High with mist only, storm culls,
    G3 600 frames, clear noon culls nothing); `--render-selftest-gl` (12 programs; the twin's half-fog pixel, off
    = plain, fog off = plain). Shots `TESTERS/atmosphere/at5/shots/` (`--sky-shot --sky-times=0.26,0.5
    --weather=clear|storm --atmo=low|high`; clear noon Low = High byte-identical).
- **Clouds (AT.6, `Classes/Clouds.{h,cpp}`).** Video › *Atmosphere* Low/High with `atmo.clouds` on, **weather or
  not** (the weather only changes their coverage and colour). No `cloud` block clash: `TYPE_CLOUD` is a building cube.
  - *The layer:* one horizontal plane at y = max(`T_HEIGHT`, 128) + 64 (192 in a 64z world, 320 in a 256z one;
    raised from `T_HEIGHT` + 24 on the user's look, 2026-11-06). `Clouds::render` runs in `Terrain::render`'s sky
    pass right after the sky, sun and moon (so after the opaque terrain), with the camera's projection/modelview
    still current: it unprojects three NDC points to get the view ray as an affine function of NDC
    (`ray0 + x·ray1 + y·ray2`) and draws one viewport quad (`R::drawClouds`, program `P_CLOUDS`, `kFmtClouds`, a
    static 4-vertex buffer) at the sky ortho's eye z `−P_ZFAR/1.000012`, nearer than the sky (`/1.000001`) and the
    sun/moon (`/1.000008`). Blended, depth-tested, no depth write: terrain hides it, so only visible sky pixels pay
    (plan P5). Each fragment intersects its ray with the plane and samples the map there.
  - *The map:* 128 × 128 `TF_L8`, repeat, nearest, generated at first use (4 octaves of tileable value noise, 8, 16,
    32 and 64 cells across, then **rank-equalised** to 0..255, so a threshold of 1 − c leaves exactly the fraction c as cloud). A texel is
    8 × 8 blocks (period 1024), so clouds are hard-edged blocks: the pixel look. (AT.11: was 12 blocks and 4..32
    cells, which drew one or two huge slabs at the base coverage; now several mid-sized clouds.) Texels further above the
    threshold shade toward a darker underside colour.
  - *Coverage:* 0.30 (`BASE_COVER`) with the weather off or clear, + 0.48 × cover (→ 0.78 overcast) + 0.17 ×
    heavy (→ 0.95 storm). *Colour (AT.11, lit by the sky, no longer × the world light, which made them muddy
    brown at dawn and in rain):* with the cycle on, `SkyLook::fogColor` (the horizon, already greyed by the weather)
    lifted toward white by 0.78 × daylight (daylight = smoothstep of the sun's elevation from −6° to 24°) and by
    ×1.15 + 0.04 at night: white at noon, peach/pink at dawn and dusk, faint silhouettes a little lighter than the
    night sky. Cycle off: white, 0.16 grey at a painted night, greyed by `Weather::greySky`. Then × (1 − 0.38 ×
    heavy) × `Weather::flashMul()` (storms darken, lightning lights); the underside shade is × (0.82 − 0.20 ×
    heavy). Alpha 0.90; faded out between 0.3 × and 1 × clamp(14 × height above the eye,
    500, 2000) blocks of horizontal distance. *Wind:* `Clouds::update` (after `Precip::update`, stops under the
    pause menu) drifts the map 1.2 blocks/s (+2.8 in a storm) toward +x/+z; the offsets are kept wrapped in
    doubles so no float grows.
  - *Cloud shadows, High + `atmo.cloud_shadows` only (plan P3):* `Clouds::applyShadow` (from `beginTerrain` and
    `Terrain::render2`, before the ×0.25 push; the sky pass and `endTerrain` turn it off) states
    `R::setCloudShadow(map, u, v, params)`: from the inverse view matrix, the sun direction and the chunk offset it
    builds the affine map eye position → map uv of the point where the ray toward the sun meets the plane (large
    constants wrapped in doubles). `drawWorld` then uses the `EDEN_CLOUD_SHADOW` twins (a **vertex**-stage fetch
    on texture unit 1: block-corner granularity), darkening a vertex by `strength` under a cloud texel. Strength
    0.32 × day (sun elevation 2° → 15°; cycle off: a fixed high sun, none on a painted night) × (1 − cover), so it
    fades as the sky closes over (overcast is already dark); < 0.01 states nothing (plain programs). Water and
    glass in `render2` take it too; portals, doors/golden cubes on `drawLit`, creatures and sprites do not.
  - *Off = absent:* tier Off, or `atmo.clouds` and `atmo.cloud_shadows` both off, frees the map and the quad at
    the next sky pass; `Terrain::deallocateMemory` frees them too; programs built on first use.
  - Gates: `--weather-selftest=clouds` (off = absent, Low = layer at 0.30 without shadows, High shadows by day
    only, storm coverage 0.95 with no shadow, G3: 600 frames upload/allocate/re-mesh nothing, Off frees);
    `--render-selftest-gl` (17 programs; a ray up hits a full texel, a ray down misses, the shadow twin halves a
    quad, a threshold no texel reaches = off). G1: plain `--sky-shot` byte-identical to the pre-AT.6 build (24/24).
    References `TESTERS/atmosphere/at6/shots/` (`--sky-shot --atmo=low|high`).
- **Ambient particles (AT.7, `Classes/Ambient.{h,cpp}`).** Video › *Atmosphere* Low/High with `atmo.ambient` on; needs no weather.
  An own fixed pool (`POOL` 256; cap Low 96 / High 256, never `pbuffer`), CPU-simulated in `Ambient::update` (after
  `Clouds::update`; stops under the pause menu), drawn by `Ambient::render` (beside `Precip::render`, render-local
  coordinates) as **one** `R::drawSprites` batch of untextured square points (`kFmtSprite`) streamed from the pool.
  AT.11: points are **world-sized** (the size attribute is world size × the focal length in pixels, `0.5 ×
  drawable height × proj[5]`, and the sprite attenuation (0, 0, 1) divides by the distance; firefly 0.05 (AT.11b,
  was 0.09), leaf 0.14, mote 0.035, ember 0.06 shrinking to half, bubble 0.09 blocks; nearer than 0.6 not drawn); they were a fixed 1–2 px and
  invisible at 720p. Spawning samples at most 8
  random cells a frame within 12 blocks of the eye (y −6 … +10) and probes 2 random columns (`PROBES`: from 10
  above the eye down to the first block; leaves followed to their underside), never a scan. **AT.11 sources:** a
  sample or probe that finds a grass / leaf-underside / lava cell remembers it (`SRC_MAX` 8 per kind), and each
  remembered cell emits at its kind's rate (firefly 0.30/s at night, leaf 0.25/s, ember 0.35/s, AT.11b: was 1.5/s, a steady fountain), checked again on
  every emission (still that block, within 14 blocks) and forgotten otherwise: random cells alone found a small
  lava pool or canopy about once a minute, so embers and leaves never showed. Each kind is capped to a share of
  the cap (AT.11: firefly 0.40, leaf 0.30, ember 0.30, bubble 0.30, mote 0.25; motes spawn at 0.04 a sample, were
  0.10 and 0.60 of the cap): *fireflies* at night (`sky_isNight`) above grass/flowers,
  blinking warm points; *leaves* below a `TYPE_LEAVES` block with air under it, falling and swaying; *dust motes*
  by day in open air; *embers* off lava with air above (AT.11b: each emission pops 1–3 sparks from one point,
  up at 1.8–3.2 blocks/s with a little sideways speed, slowed by 2.6/s² against drag and flutter, living 0.7–1.6 s,
  flickering, hot near-white cooling to yellow → orange → dull red; on **painted lava** the spark takes the paint's
  `colorTable` colour, whitened while hot, so green "acid" gives green embers: `P::paint`, read by `emitAt`); *bubbles* only while `hud->underLiquid` (a water
  cell within 3 blocks; the only kind spawned underwater). Rain (`precipAmount` > 0.3) stops fireflies and motes.
  A particle dies on age, past 16 blocks, or on entering a block (bubbles: on leaving water). Colours take the
  world light except the glowing kinds. A tier drop sheds the surplus at once; Off empties the pool;
  `Terrain::deallocateMemory`/`clearBlocks` reset it.
  Gate: `--weather-selftest=ambient` (off = absent, caps, 1000 forced spawns never pass the cap, High → Low sheds,
  G3: 600 frames bounded with no R resource of its own, night spawns no new motes). *Firefly glows* (AT.11) are
  drawn by `renderHalos` (below) with the lamp halos.

- **Water and light polish (AT.8).** Video › *Atmosphere* with the element switches `atmo.water_tint` (Low, High),
  `atmo.glint` and `atmo.halos` (High). (a) *Water tint*: `Graphics::applyWaterTint` (called from `Terrain::render2` before
  the x0.25 push, off after the chunk loop) states `R::setWaterTint(tint, sun, up)`; the `EDEN_WATER_TINT` vertex twins pull
  only vertices whose colour alpha is 145/255 (the mesher's `IS_WATER` mark; nothing else meshes 145) 30 % toward the
  sky/fog colour (greyed by weather). (b) *Glint* (AT.11, High + `atmo.glint`: the `EDEN_WATER_GLINT` twins of the four
  water-tint twins, chosen only while the glint strength `0.55 x day x clearSky` is > 0, so Low and night keep the plain
  tint twins): **texel sparkles**. The vertex stage passes the surface's 1/16-block cell (`a_position.xz` × 4) and the
  sun-path weight `(r.sun)^16` (r the view ray mirrored in the surface; 0 off water) and adds a faint sheen of 0.3 ×
  `(r.sun)^32`; the fragment stage hashes the cell and lights it near white where the hash is in the top 10 % × the
  weight and its own twinkle phase (`u_wup.w` = `Ambient::clock()` × 0.7) is in the first 30 % of a cycle. It replaced
  AT.8's per-vertex `(r.sun)^32` highlight, which smeared into one soft blob over a pool (user: "pixel sparkles").
  (c) *Underwater fog* (any tier above Off): while `Hud::underLiquid`, `applyWorldFog`
  uses 0.7 × the liquid's colour (in the clock's light; AT.11: was 1.0, a flat bright tint), start 1, end 24 (was 28), and height fog is off; the HUD's 0.5 fill is unchanged.
  `Graphics::worldFogRange` (AT.11) returns the start/end applyWorldFog states (under water, mist) for draws that
  fade themselves. Culling is not pulled in (the sky behind would show). (d) *Lamp halos* (AT.11 redesign; AT.8's
  flickering point sprite read as a pulsing square and was disabled): the first four `TYPE_LIGHTBOX`
  cells per chunk are recorded in `rebuild2`'s first scan loop (`TerrainChunk::lamps`, outside the count/fill pair);
  `eden_lamp_positions` walks only the chunk columns within the radius (a slot holding another column of the
  toroidal window is skipped) and keeps the **nearest** 64 within 40 blocks. `Ambient::renderHalos` draws them and
  the firefly glows as **one additive** (`B_ADD`) `R::drawWorld` of camera-facing quads (`kFmtGlow`: pos, colour,
  uv; streamed, no buffer) with a 16 × 16 `TF_L8` nearest **pixel-ring** texture (bands 1.0 / 0.59 / 0.31 / 0.12,
  core radius 0.24, made on first use, freed when nothing glows, `atmo-glow`): a halo is 2.6 blocks across (AT.11b:
  was 3.6, user: too big, not subtle), 0.9 toward the camera
  from the lamp's centre (so the block's own faces do not hide it), **steady** (no flicker: user), tinted by the
  lamp's paint (unpainted warm white), alpha 0.30 (AT.11b: was 0.42) × the lamp's visibility × night (the cycle's sun −4° … 8°, or the stock flag; a dark
  storm adds 0.35 × its cover) × the world fog's range (`worldFogRange`: an additive quad must not take the fog
  colour, so `fogOff` and the CPU fades it) × the 40-block edge × a fade nearer than 2.7 blocks. **Visibility**
  (AT.11b; the depth test alone let halos show through walls: the pulled quad poked out of a one-block wall and the
  wide quad spilled round whatever hid the lamp): `lampSeen` walks the voxels (Amanatides–Woo) from the eye to the
  centre of each lamp face turned to the eye whose neighbour cell is open; an occluder is a block > 0 without
  `IS_NOTSOLID | IS_LIQUID | IS_NOSHADOW` (glass, cloud, weave, water see through). Per-lamp state is matched by cell
  across frames and eased 0.15 a frame (no pop); a lamp new to the list is walked at once, others round-robin (≤ 16
  walks a frame). Lamp *light* still passes through blocks (stock lighting; a far-future item). A firefly glow is
  0.44 blocks across, its blink × 0.35 (AT.11b: were 0.8 and × 0.75). High + `atmo.halos` for lamps; glows whenever fireflies live. Gate:
  `--weather-selftest=halos` (Off = absent, none by day or on Low, three at night from one texture, G3 600 night
  frames, a lamp boxed in by stone loses its halo and gets it back, the nearest-first cap, Off frees the texture).
- **Review set (AT.11):** `--atmo-shot[=PREFIX]` (window) builds the scene-shot site + an annex (8 × 8 pool, grass,
  canopy, three lamps) and writes one frame per effect with its own forced tier and weather, plus a counter line
  each, then two edited scenes (AT.11b): `halos-wall` (a stone wall hides one lamp: two halos) and
  `ambient-lava-green` (the site's lava painted green, shot once three embers are up); the refs are `TESTERS/atmosphere/at11/{before,after}/` and `at11b/after/` (the tuning round). Verdicts per effect: `WORKING/atmosphere-plan-2026-11-05.md` §12.

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
