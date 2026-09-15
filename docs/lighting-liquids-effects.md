# Lighting, Liquids, Portals, Fireworks & Effects

Grab-bag of the dynamic systems layered on the voxel grid. Fire/TNT propagation lives
in `Terrain.mm` and is documented in [world-and-terrain.md](world-and-terrain.md).

## Lighting (root-level `Lighting.mm` + `Lighting.h`)

**Note the file location:** the compiled lighting code is `Lighting.mm` in the *repo
root*, not `Classes/` (`Classes/Lighting.c` exists but is not in the build).

- Data: `Vector8* lightarray` — one RGB byte-triple per voxel in the resident window,
  toroidally indexed like `blockarray`. Entirely disabled on `LOW_MEM_DEVICE`.
- `addlight(x,z,y, brightness, color)` (`Lighting.mm:17`) — adds (or with
  brightness=−1, subtracts) a spherical falloff light of radius `LIGHT_RADIUS 5`:
  `contribution = 64·(1−dist/5)·brightness·color`, clamped 0..255 per channel.
  Called when lightboxes are placed/destroyed/repainted.
- `calculateLighting()` (`Lighting.mm`) — full rebuild: scans every resident chunk
  for `TYPE_LIGHTBOX` and re-adds its light, refreshing affected chunk meshes.
  Triggered via `updateLightingBegin()` (zero the array + set a flag) after world
  load and after every streaming event — lights are **not** persisted, they're
  re-derived from lightbox blocks. The per-column scan body is factored into
  `sweepLightingColumn(cx,cz)`, shared with the sliced form below.
- `calculateLightingSlice()` (`Lighting.mm`, web-port perf change 2026-08-27) — the
  same sweep but a budgeted strip of columns per frame (`LIGHTING_SWEEP_CHUNK_BUDGET`
  256 chunks, counted like `BULK_RELOAD_CHUNK_BUDGET` so it stays flat at 256z),
  holding a cursor between frames and returning TRUE only when the window is fully
  swept. This is what the **post-bulk-reload** `update_lighting` path calls now: the
  full scan is O(window volume) (~5.3M voxel tests at 64z, ~21M at 256z) and was one
  unbudgeted ~20 ms (64z) / ~80 ms (256z) main-thread stall per teleport/warp — the
  actual 256z reload spike, not the chunk-mesh budget. `updateLightingBegin()` calls
  `calculateLightingSliceReset()` so a second teleport mid-sweep restarts from column 0.
- Consumption: `calcLight(x,z,y, skylight, channel)` (`Terrain.mm:1505`) adds
  `lightarray/64` to the base skylight and clamps to 1.5; the mesher bakes this into
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
  (`calculateLightingSlice`) so the *scan* no longer stalls a frame, but a dense
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
