# Entity System (Creatures)

## Purpose
The seven ambient creatures (Moof, Batty, Green, Nergle, Stumpy, Charger, Stalker) —
their models, animation, AI, interaction, and persistence. This is the closest thing
the codebase has to a general entity system, and it is deliberately *not* general:
everything is creature-specific.

## Important files & types
- `Classes/Model.mm` (2811 lines) — the entire system behind a C function API
  (`Model.h`): `LoadModels`, `UnloadModels`, `UpdateModels`, `RenderModels`,
  `PointTestModels`, `PickupModel`, `PlaceModel`, `ColorModel`, `HitModel`,
  `BurnModel`, `ExplodeModels`, `SaveModels`, `LoadModels2`,
  `addMoreCreaturesIfNeeded`, `killCreature`.
- `Entity` struct (`Model.mm:215`) — runtime state: position/velocity (PVRTVec3),
  AI `state`, animation `frame`, `color`, fire/liquid/damage flags, `dest`/`gotoDest`
  wander target, collision `box` (Polyhedra).
- `EntityData` (`Vector.h:38`) — the 60-byte serialized subset (200 slots in the save
  file; see [eden-file-format.md](eden-file-format.md)).
- `guys[300]` — static entity pool (`nguys`); `creatureData[200]` — save staging.
- PowerVR: `CPVRTModelPOD models[NUM_CREATURES]` — one animated `.pod` scene per
  species, VBO-uploaded via `m_puiVbo`; textures/skins resolved through
  `Resources::getSkin(type, color, state)`.
- Constants: `NUM_CREATURES 7`, `M_MOOF..M_STALKER` (`Constants.h:27-35`).

## Lifecycle
- `LoadModels(resourcePath)` — on world entry (only if `CREATURES_ON`, which is off
  for `LOW_MEM_DEVICE`): loads the 7 POD files, computes per-species bounds
  (`cmin/cmax`, collision polyhedra `mpolys`), builds VBOs.
- `LoadModels2()` — after `LoadCreatures` filled `creatureData` from the save file:
  instantiates entities from the 200 slots (`type == -1` ⇒ empty).
- `addMoreCreaturesIfNeeded()` — called after every streaming event: keeps the area
  around the player populated by spawning new creatures at valid surface positions
  (species chosen per biome/height — verify specifics in the function before relying
  on them; confidence medium on the exact spawn rules).
- `SaveModels()` — writes the nearest/active entities back into `creatureData` before
  `saveCreatures()` persists them.
- `UnloadModels()` on exit to menu.

## Per-frame
- `UpdateModels(etime)` — AI state machine per entity. States map to animation frame
  ranges in per-species tables (`moof_states`, `batty_states`, … `Model.mm:150-200`):
  idle/breathe, look, walk, jump, damage; behavioural flags: `excited` (player
  greets), `runaway`/`scared` (after being hit), `ragetimer` (Charger/Stalker
  aggression — they charge and damage the player), `onfire` (burns then dies with a
  vanish effect), liquid buoyancy, gravity + polyhedra collision against terrain
  (same SAT machinery as the player).
- `RenderModels()` — skinned via CPU matrix palette from the POD animation
  (fixed-function: the POD's per-frame transforms are applied, no GPU skinning),
  distance/frustum-culled (`insideView`), colored by `Resources::getSkin` textures
  (each species has per-color skin variants), damage flash, env-map shine on some.
- `MMM::ExplodeModels(pos, color)` — TNT blast: kills or paints creatures in radius
  (painted TNT recolors creatures — matches the paint-bomb block behaviour).

## Interaction entry points
The raycast (`findWorldCoords`) tests creatures first via
`PointTestModels(x,y,z)` → index into `guys`; the HUD mode then dispatches:
pick up (`PickupModel` → held in `Hud::holding_creature`, placed with `PlaceModel`),
paint (`ColorModel`), hit (`HitModel` with knockback), burn (`BurnModel`).

Web port dev console (project-audit-2026-07-30 row F5, `web/src/seam/DevConsole_web.mm`,
EDEN_DIAGNOSTICS-only): `SpawnCreatureAt(type, pos)` places a creature at an exact position by
reusing the ambient spawner's slot-scavenging condition and `ResetModel`, skipping
`addMoreCreaturesIfNeeded`'s randomized ground search and biome-based type selection since the
caller already knows the position is valid. `CountActiveCreatures()` is a read-only count for the
console's `stats` command. Both are genuine engine additions (`Classes/Model.{h,mm}`), not seam
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
- Player: `wrapx/wrapz` map creature positions into the toroidal window;
  `setViewNow` refreshes the cached view matrix used by both creatures and the
  golden-cube env-map (`CalcEnvMap`).

## Common pitfalls
- Entity positions are stored in **absolute world coordinates** but simulated against
  the toroidal window — `wrapx/wrapz` are required whenever comparing to block
  coordinates; forgetting them teleports creatures 288 blocks away.
- The pool is fixed at 300; `addMoreCreaturesIfNeeded` recycles far-away entities.
  Only 200 survive a save.
- `Model.mm` uses PVRT math types (`PVRTVec3`) while the rest of the game uses
  `Vector` — conversion helpers `vpv/MakeVector2` at `Model.mm:293-311`.
- All state is file-static; there can never be two worlds' creatures loaded.

## Safe vs. risky to modify
- **Safe:** AI tuning (timers, ranges), spawn density, new interactions following the
  existing dispatch pattern.
- **Caution:** `EntityData` layout (save format!), the pool/save-slot mapping,
  POD loading (PowerVR SDK internals), anything assuming `NUM_CREATURES==7`
  (state tables are per-species and hand-written).
