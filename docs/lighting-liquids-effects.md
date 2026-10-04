# Lighting, Liquids, Portals, Fireworks & Effects

Grab-bag of the dynamic systems layered on the voxel grid. Fire/TNT propagation lives
in `Terrain.mm` and is documented in [world-and-terrain.md](world-and-terrain.md).

## Lighting (root-level `Lighting.mm` + `Lighting.h`)

**Note the file location:** the compiled lighting code is `Lighting.mm` in the *repo
root*, not `Classes/` (`Classes/Lighting.c` exists but is not in the build). **The same goes for the
header:** the real one is the repo-root `Lighting.h` (the one `Eden.xcodeproj` lists).
`Classes/Lighting.h` is an empty stock stub with the *same* include guard (`Eden_Lighting_h`), and a
bare `#import "Lighting.h"` from a file in `Classes/` resolves to the stub first. `Terrain.mm` did
exactly that until R.3 and now imports `"../Lighting.h"`.

- Data (**modified from stock, Stage R / R.3, 2026-10-03**): the light field is one RGB byte-triple
  per voxel of the resident window, toroidally indexed like `blockarray`, but it is no longer stored
  densely. Stock kept `Vector8* lightarray` for every voxel (288·288·`T_HEIGHT`·3 bytes: **15.9 MB at
  64z, 63.7 MB at 256z**) and memset all of it on every load, warp and bulk reload, although only
  voxels within `LIGHT_RADIUS` of a lightbox are ever non-zero. It is now `Vector8** lightbricks`:
  one pointer per toroidal chunk slot (1296 / 5184), each NULL or a 12 KB brick of
  `CHUNK_SIZE`³ triples laid out `CC(x,z,y)`. A brick is allocated the first time `addlight` writes a
  non-zero value into its slot; no brick means no light. Accessors in `Lighting.h`
  (`light_brickSlot`, `light_brickIndex`, `light_get`), store functions in `Lighting.mm`
  (`light_storeAllocate/Free/Clear`). Entirely disabled on `LOW_MEM_DEVICE`, as before.
  - **Bricks are never freed during play.** `MeshPool`'s workers call `calcLight` off the main thread
    without a lock (threaded web only; no native target has workers), which was safe for the dense
    array only because it never moved. `light_storeClear()` (what the memset was) NULLs each lit slot,
    zeroes the brick in place and moves it to a pool that `addlight` reuses; only `light_storeFree()`
    in `Terrain::deallocateMemory`, after `mp_drain`, frees memory. A worker that loaded a pointer
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
- `addlight(x,z,y, brightness, color)` (`Lighting.mm:17`) — adds (or with
  brightness=−1, subtracts) a spherical falloff light of radius `LIGHT_RADIUS 5`:
  `contribution = 64·(1−dist/5)·brightness·color`, clamped 0..255 per channel.
  Called when lightboxes are placed/destroyed/repainted.
- `calculateLighting()` (`Lighting.mm`) — full rebuild: finds every `TYPE_LIGHTBOX` in the resident
  window and re-adds its light, refreshing affected chunk meshes.
  Triggered via `updateLightingBegin()` (zero the array + set a flag) after world
  load and after every streaming event — lights are **not** persisted, they're
  re-derived from lightbox blocks. The per-column body is `sweepLightingColumn(cx,cz)`, shared with
  the sliced form below; a hit goes through `sweepLightingHit` (`addlight` + `refreshChunksInRadius`).
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
- `calculateLightingSlice()` (`Lighting.mm`, web-port perf change 2026-08-27) — the
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
  `eden_debug_light_state()` (`web/src/seam/DebugState_web.mm`; `.light` on native `--stage1`) walks
  every voxel in the dense order through `lightAtToroidal()`, so it reads the same for the dense array
  and the bricks (`580637df278f24c9` on W64, `7fd87220d15161e5` on W256, 2732 lit voxels, both).
  **`--stage1` alone does not exercise brick reuse:** a deliberately broken clear (bricks pooled
  without being zeroed) left the stage1 hash unchanged, because that run only ever sweeps into fresh
  bricks. `--light-selftest` (place / saturate / repaint / break through `buildBlock`, `paintBlock`,
  `destroyBlock`, across the toroidal seam and both y clips, then a fresh sweep) caught it at once.
  `--light-selfcheck` keeps the stock dense array beside the bricks, written by the stock code, and
  reports voxels where they differ (`mismatch`, must be 0).
- Consumption: `calcLight(x,z,y, skylight, channel)` (`Terrain.mm`) adds
  `light/64` to the base skylight and clamps to 1.5; the mesher bakes this into
  vertex colors. Skylight is 1.0 by day, 0.35 when the sky color is the night palette
  entry (`colorTable[54]`). There is **no** sunlight occlusion — `getShadow` returns
  a constant 1.0 (the 1.7-era shadow system is commented out).

## Liquids (`Classes/Liquids.mm`)

Water and lava are voxels with the fill level encoded in the type
(`TYPE_WATER` full=4 → `WATER3/2/1`; same for lava). Helpers: `getLevel(type)`,
`getBaseType(type)`, `genLevel(baseType, level)`.

- `addSource(x,z,y)` — pushes onto `plist` (spread queue);
  `removeSource` — onto `plist2` (drain queue with the removed type/level).
- `update(etime)` (`Liquids.mm:495`) runs on a delay timer and processes queues:
  spread flows downhill first (full-level block below), then sideways with
  decreasing level (`genLevel(type, level−1)`), inheriting the source's color;
  lava additionally refuses to flow into the player (`player->test`). Draining
  reverses it, removing dependent flow blocks. Every change goes through
  `Terrain::updateChunks`, so liquid motion re-meshes chunks continuously —
  the main reason big waterfalls tank the frame rate.
- `clearLiquids()` — called by `endDynamics` (save, overload valve): stops all flow
  but leaves the liquid blocks as-is.
- Player/creature interaction: `inLiquid`, `getFlowDirection` (push), lava damage.
- Surface rendering (slopes, animated texture) is in the mesher — see
  [rendering.md](rendering.md).
- Note: the `WetNode`/`wetmap` machinery in the header is a dead earlier design;
  `updateHeights` returns FALSE immediately.

## Portals (`Classes/Portal.mm`)

- Registry of up to `MAX_PORTAL 1000` `sportal{x,y,z,dir,color}` records.
- Populated **from the mesher**: when `rebuild2` encounters `TYPE_PORTAL_TOP` it calls
  `portals->addPortal(...)` (`TerrainChunk.mm:530`) — so the registry always reflects
  currently-resident chunks. `removeAllPortals` on unload.
- `enterPortal(x,y,z, vel)` — finds the **next portal of the same color** in the
  registry (cycling, so pairs/chains work) and returns destination + exit vector;
  `Player::move` performs the travel (a save+reload warp if outside the window).
- Rendered as extracted `StaticObject`s with the swirl texture and rotating UVs
  (`Terrain::render`), plus the frame blocks as normal geometry.

## Fireworks (`Classes/Firework.mm`)

`sfirework{pos, color, fuse, vel}` pool (`MAX_FIREWORK 80`). A burning
`TYPE_FIREWORK` block launches one (`Terrain::shootFirework`): rises with a fuse,
then explodes into particles colored by the block's paint
(`SpecialEffects::addFirework`). Rendered as camera-facing quads
(`Graphics::drawFirework`).

## Particles & block-break effects (`SpecialEffects.mm`, `Fire.mm`, `BlockBreak.mm`)

`SpecialEffects` is the façade `World` owns:
- `addBlockBreak/addBlockExplode(x,z,y,type,color)` — `BlockBreak.mm` spawns textured
  debris cubes with physics using the broken block's texture and paint.
- `addFire(x,z,y,type,life)` / `removeFire(pid)` / `updateFire` — `Fire.mm` flame +
  smoke particle emitters (point sprites, `vertexpStruct`); ids returned so the
  burn system (`BurnNode.pid`) can extinguish emitters.
- `addCreatureVanish(x,z,y,color,type)` — the poof used by TNT/creature death.
- `clearAllEffects()` from `endDynamics`/world load.
Buffer cap: `pbuffer_size 10000` particles.

## Sky color system (partly in `TerrainGen2.mm`)

Per-world 4×4 grid of sky palette indices (`regionSkyColors`, persisted in the header
as `skycolors[16]`). `updateSkyColor1/2` map the player's position in the default
world onto the grid and set `terrain->final_skycolor`; `Terrain::update` interpolates
`skycolor` toward it and updates the fog color; `Terrain::render` crossfades the
colored/B&W skybox textures. Painting the sky (`paintSky`) recolors the region the
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
- `endDynamics` (any save!) silently kills active fire fronts and liquid queues —
  by design, but surprising when testing fire features.

## Safe vs. risky to modify
- **Safe:** light radius/intensity, liquid spread rates, particle counts/lifetimes,
  firework physics.
- **Caution:** liquid queue processing order (spread-vs-drain interleaving prevents
  infinite loops), the mesher→portal registry coupling, lighting rebuild triggers.
