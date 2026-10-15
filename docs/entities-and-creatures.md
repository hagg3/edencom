# Entity System (Creatures)

## Purpose
The seven ambient creatures (Moof, Batty, Green, Nergle, Stumpy, Charger, Stalker) —
their models, animation, AI, interaction, and persistence. This is the closest thing
the codebase has to a general entity system, and it is deliberately *not* general:
everything is creature-specific.

## Important files & types
- `Classes/Model.mm` — the entire system behind a C function API
  (`Model.h`): `LoadModels`, `UnloadModels`, `UpdateModels`, `RenderModels`,
  `PointTestModels`, `PickupModel`, `PlaceModel`, `ColorModel`, `HitModel`,
  `BurnModel`, `ExplodeModels`, `SaveModels`, `LoadModels2`,
  `addMoreCreaturesIfNeeded`, `killCreature`.
- `Entity` struct (`typedef struct _entity` in `Model.mm`) — runtime state: position/velocity (PVRTVec3),
  AI `state`, animation `frame`, `color`, fire/liquid/damage flags, `dest`/`gotoDest`
  wander target, collision `box` (Polyhedra).
- `EntityData` (`Vector.h`) — the 60-byte serialized subset (`MAX_CREATURES_SAVED` slots in the
  save file: 200 for an ordinary world, derived from the file; see
  [eden-file-format.md](eden-file-format.md)).
- `guys[300]` — static entity pool (`nguys`); `creatureData[MAX_CREATURES_SAVED_MAX]` (400, in
  `FileManager.mm`) — save staging.
- PowerVR: `CPVRTModelPOD models[NUM_CREATURES]` — one animated `.pod` scene per
  species, VBO-uploaded via `m_puiVbo`; textures/skins resolved through
  `Resources::getSkin(type, color, state)`.
- Constants: `NUM_CREATURES 7`, `M_MOOF..M_STALKER` (`Constants.h`).

## Lifecycle
- `LoadModels(resourcePath)` — on world entry (only if `CREATURES_ON`, which is off
  for `LOW_MEM_DEVICE`): loads the 7 POD files, computes per-species bounds
  (`cmin/cmax`, collision polyhedra `mpolys`), builds VBOs.
- `LoadModels2()` — after `LoadCreatures` filled `creatureData` from the save file:
  instantiates entities from the saved slots (`type == -1` ⇒ empty).
- `addMoreCreaturesIfNeeded()` — called after every streaming event: keeps the area
  around the player populated by spawning new creatures at valid surface positions
  (species chosen per biome/height — verify specifics in the function before relying
  on them; confidence medium on the exact spawn rules).
- `SaveModels()` — writes the nearest/active entities back into `creatureData` before
  `saveCreatures()` persists them.
- `UnloadModels()` on exit to menu.

## Per-frame
- `UpdateModels(etime)` — AI state machine per entity. States map to animation frame
  ranges in per-species tables (`moof_states`, `batty_states`, … in `Model.mm`):
  idle/breathe, look, walk, jump, damage; behavioural flags: `excited` (player
  greets), `runaway`/`scared` (after being hit), `ragetimer` (Charger/Stalker
  aggression — they charge and damage the player), `onfire` (burns then dies with a
  vanish effect), liquid buoyancy, gravity + polyhedra collision against terrain
  (same SAT machinery as the player).
- `RenderModels()` — skinned with `GL_OES_matrix_palette` and PVRT bone batches: the CPU builds
  each batch's bone matrices (`GetBoneWorldMatrix`) and loads them into the palette; the vertex
  blend runs in the GL implementation (port: the shim's palette program, `gl_fixed_function.cpp`,
  with entry points from `native/src/seam/pvrt_matrix_palette_native.cpp`),
  distance/frustum-culled (`insideView`), colored by `Resources::getSkin` textures
  (each species has per-color skin variants), damage flash, env-map shine on some.
- `MMM::ExplodeModels(pos, color)` — TNT blast: kills or paints creatures in radius
  (painted TNT recolors creatures — matches the paint-bomb block behaviour).

## Interaction entry points
The raycast (`findWorldCoords`) tests creatures first via
`PointTestModels(x,y,z)` → index into `guys`; the HUD mode then dispatches:
pick up (`PickupModel` → held in `Hud::holding_creature`, placed with `PlaceModel`),
paint (`ColorModel`), hit (`HitModel` with knockback), burn (`BurnModel`).

`setState` can also play `VO_WALKING` on a transition into `DEFAULT_WALK` (H.11, the Experiments
toggle `creature_walk_voices`, off by default; once per creature per 8 s, never for Batty).

Port additions (project-audit-2026-07-30 row F5): `SpawnCreatureAt(type, pos)` places a creature at an exact position by
reusing the ambient spawner's slot-scavenging condition and `ResetModel`, skipping
`addMoreCreaturesIfNeeded`'s randomized ground search and biome-based type selection since the
caller already knows the position is valid. Callers: command-block scripts (`CmdScript.mm`, D.4c),
the diagnostics dev console (`port/src/seam/DevConsole_web.mm`, `EDEN_DIAGNOSTICS` only) and the
`--cmds-selftest` harness. `CountActiveCreatures()` is a read-only count for the console's `stats`
command and `--cmds-selftest`. Both are genuine engine additions (`Classes/Model.{h,mm}`), not seam
workarounds — the file-static `guys[]`/`nguys` meant a seam file could not read or write creature
state directly, so a small exported entry point was the only option once a runtime spawn command
was wanted.

## Interactions with other systems
- Terrain: collision reads `getLandc`; spawning samples surface heights; explosions
  come from `Terrain::explode`/`blocktntexplode`.
- Save/load: `FileManager::LoadCreatures/saveCreatures` (creatures block, v3+).
- Resources: `getSkin` textures, `voSound` creature voice lines (per-species
  Angry/Excited/Hit/Idle/Scared/... variants — the huge list of `.caf` files in the
  bundle).
- `setViewNow()` (`Model.mm`) refreshes the cached view matrix used by both creatures and the
  golden-cube env-map (`CalcEnvMap`).

## Common pitfalls
- Entity positions are **absolute world coordinates**. `wrapx/wrapz` are now identity stubs (their
  toroidal mapping is commented out); block lookups wrap inside `getLandc`, so compare against block
  coordinates only through it. Spawning offsets by `fm->chunkOffsetX/Z * CHUNK_SIZE`.
- The pool is fixed at 300; `addMoreCreaturesIfNeeded` recycles far-away entities.
  Only `MAX_CREATURES_SAVED` (200 for an ordinary world) survive a save.
- `Model.mm` uses PVRT math types (`PVRTVec3`) while the rest of the game uses
  `Vector` — conversion helpers `vpv/MakeVector2` in `Model.mm`.
- All state is file-static; there can never be two worlds' creatures loaded.

## Safe vs. risky to modify
- **Safe:** AI tuning (timers, ranges), spawn density, new interactions following the
  existing dispatch pattern.
- **Caution:** `EntityData` layout (save format!), the pool/save-slot mapping,
  POD loading (PowerVR SDK internals), anything assuming `NUM_CREATURES==7`
  (state tables are per-species and hand-written).
