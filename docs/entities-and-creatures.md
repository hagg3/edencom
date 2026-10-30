# Entity System (Creatures)

## Purpose
The seven ambient creatures (Moof, Batty, Green, Nergle, Stumpy, Charger, Stalker) —
their models, animation, AI, interaction, and persistence. This is the closest thing
the codebase has to a general entity system, and it is deliberately *not* general:
everything is creature-specific.

## Important files & types
- `Classes/Model.cpp` — the entire system behind a C function API
  (`Model.h`): `LoadModels`, `UnloadModels`, `UpdateModels`, `RenderModels`,
  `PointTestModels`, `PickupModel`, `PlaceModel`, `ColorModel`, `HitModel`,
  `BurnModel`, `ExplodeModels`, `SaveModels`, `LoadModels2`,
  `addMoreCreaturesIfNeeded`, `killCreature`.
- `Entity` struct (`typedef struct _entity` in `Model.cpp`) — runtime state: position/velocity (`Vec3f`),
  AI `state`, animation `frame`, `color`, fire/liquid/damage flags, `dest`/`gotoDest`
  wander target, collision `box` (Polyhedra).
- `EntityData` (`Vector.h`) — the 60-byte serialized subset (`MAX_CREATURES_SAVED` slots in the
  save file: 200 for an ordinary world, derived from the file; see
  [eden-file-format.md](eden-file-format.md)).
- `guys[300]` — static entity pool (`nguys`), plus two extra slots outside it: `guys[nguys]` is the
  translucent placement ghost (`PlaceModel(-1/-2)`), `guys[PLAYER_BODY]` (`nguys+1`, H.12) the
  third-person player body (`UpdatePlayerBody`/`HidePlayerBody`/`PlayerBodyAvailable` in `Model.h`). Every
  loop in `Model.cpp` is `i < nguys`, which is what keeps both out of saving, `UpdateModels`,
  `PointTestModels` (the player cannot mine/paint/burn their own body) and `CountActiveCreatures`; an
  `i <= nguys` loop would break that. `RenderModels` draws the body after the ghost (opaque, no frustum
  test). The body never goes through `setState`: `walkVoiceLast[]` is sized `nguys+1`, and setState would
  play creature walk voices. `creatureData[MAX_CREATURES_SAVED_MAX]` (400, in
  `FileManager.cpp`) — save staging.
- `CreatureModel models[NUM_CREATURES]` (`Classes/CreatureModel.{h,cpp}`, LG.7, 2026-10-27) — one
  animated model per species, read from `<bundle>/<Name>.edm`, VBO-uploaded via `m_puiVbo`; textures/skins
  resolved through `Resources::getSkin(type, color, state)`. Until LG.7 this was the PowerVR SDK's
  `CPVRTModelPOD` reading the `.pod` files; `CreatureModel` keeps its field names (`pMesh`, `pNode`,
  `sBoneBatches`, `nNumMeshNode`, `nNumFrame`) and reproduces its animation **bit for bit**: the same
  lerp/slerp/matrix expressions in the same order (FMA contraction makes a regrouped sum a different
  float, so do not tidy them), the same per-node world-matrix caches, double-precision determinant in
  the bone inverse. `SetFrame` clamps to the last *blend* (G.2s, 2026-10-24): the engine sets frames up to
  `nNumFrame-1`, and every key read blends toward `nFrame+1`, which stock read one past the end of the
  animation arrays (ASan). The last frame is reached as `nNumFrame-2` with blend 1.
- **`.edm`** — written once, offline, by `port/tools/pod2edm.py` from the `media/models/*.pod` art (which
  stays in the repo as the source but no longer ships; CI runs `pod2edm.py --check`). Little-endian,
  4-byte fields: `"EDM1"`, version 1, `numFrame numMesh numNode numMeshNode`; per mesh `numVertex
  numFaces`, five attributes {type, n, stride, offset} (position, normal, UV0, bone indices u8, bone
  weights float; POD type numbering; offsets into the interleaved block), the interleaved block, the u16
  triangle list, the bone batches (`batchBoneMax batchCnt`, bones, per-batch bone counts, first
  triangles); per node `meshIdx parent animFlags` and position/rotation(x,y,z,w)/scale keys — one key if
  static, `numFrame` keys if animated (the POD's per-frame index arrays already resolved). Everything
  else in a `.pod` (materials, cameras, lights, names) is dropped; the tool refuses strips, matrix
  animation and the old node format, none of which the seven models use. Two quirks carried over: a
  POD's second, unskinned mesh (Green's and Nergle's) has no bone batches and so never draws, and the
  bone-array test is `n != 0` (the SDK tested `pData != NULL`, i.e. a nonzero interleaved offset;
  `pod2edm` checks the two agree).
- **Gate: `--creature-selftest`** (`Classes/CreatureModelSelftest.cpp`): every creature posed at every
  quarter frame as `DrawModel` poses it, CPU-skinned like the palette shader, hashed. LG.7 ran it with the
  SDK still linked and every bone matrix and skinned float was byte-identical to `CPVRTModelPOD`'s (and
  `VecMath.h` to `PVRTVector`'s); the hashes are pinned for macOS arm64 (libm and FMA move the last bit
  elsewhere, where the run is structural only). `--scene-shot` A/B against the SDK build: 0 of 66 images
  differ. `Model.cpp`, `BlockBreak.cpp` and `VectorUtil.cpp` use `Classes/VecMath.h` (`Vec3f`, `Vec4f`,
  `Mat4f`), the SDK's `PVRTVec3/4`/`PVRTMat4` semantics to the bit (column-major; `Mat4f*Vec4f` is M·v;
  `inverse()` is the affine float one).
- Constants: `NUM_CREATURES 7`, `M_MOOF..M_STALKER` (`Constants.h`).

## Lifecycle
- `LoadModels(resourcePath)` — on world entry (only if `CREATURES_ON`, which is off
  for `LOW_MEM_DEVICE`): loads the 7 `.edm` files, computes per-species bounds
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
  ranges in per-species tables (`moof_states`, `batty_states`, … in `Model.cpp`):
  idle/breathe, look, walk, jump, damage; behavioural flags: `excited` (player
  greets), `runaway`/`scared` (after being hit), `ragetimer` (Charger/Stalker
  aggression — they charge and damage the player), `onfire` (burns then dies with a
  vanish effect), liquid buoyancy, gravity + polyhedra collision against terrain
  (same SAT machinery as the player).
- `RenderModels()` — skinned per bone batch: the CPU builds each batch's bone matrices
  (`CreatureModel::GetBoneWorldMatrix`, times `m_mView * m_mTransform`, so a palette entry maps object
  space straight to eye space) and `DrawModel` hands them to **`R::drawSkinned`** (G.5b-5, 2026-10-28;
  stock 2.1 loaded them into `GL_OES_matrix_palette`, which the port's shim emulated until then). Each
  mesh's VBO/IBO are `R::Buf`s and its interleaved layout an `R::VertexFormat` built in `LoadVbos`
  (`meshFormat`: float position/normal/UV/weights, unsigned-byte bone indices, the block's one stride).
  The draw states everything 2.1's ES 1.1 state gave it: opaque, depth-tested and written, culled; the
  ghost (`guys[nguys]`) alpha-blended; LIGHT0 a point light at the eye (ambient .3 / diffuse .7, .2/.35
  at night), model ambient 0.2; the world fog, read with the transitional `R::legacyFog()` until
  Terrain/Graphics move (G.5b-6); the tint that `glColor4f` used to be (`modelTint`: flash, on fire,
  ghost alpha). A skin is picked into `tex` on every branch (2.1 bound the rage/default skins in place).
  `LoadModels` no longer gates on `GL_EXTENSIONS`. Creature shadows (`DrawShadows`: `R::drawWorld`
  strips, blended, depth not written, fog off) and the dead `DRAW_BOUNDING_BOXES` boxes (`DrawBox`:
  `R::drawWorld` lines; a former `GL_LINE_LOOP` is a closed `LINE_STRIP`) are on `R::` too, so `Model.cpp`
  makes no GL call. 2.1 left `GL_LIGHT0` enabled after this pass; nothing reads that now (only
  `--state-trace`'s `light=` field showed it).
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
caller already knows the position is valid. Callers: command-block scripts (`CmdScript.cpp`, D.4c),
the diagnostics dev console (`port/src/seam/DevConsole.cpp`, always built since G.1) and the
`--cmds-selftest` harness. `CountActiveCreatures()` is a read-only count (model present, updating **and alive**: `SpawnCreatureAt` may reuse a dead creature's slot, so counting dead ones made a spawn read +0) for the console's `stats`
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
- `setViewNow()` (`Model.cpp`) refreshes the cached view matrix used by both creatures and the
  golden-cube env-map (`CalcEnvMap`).

## Common pitfalls
- Entity positions are **absolute world coordinates**. `wrapx/wrapz` are now identity stubs (their
  toroidal mapping is commented out); block lookups wrap inside `getLandc`, so compare against block
  coordinates only through it. Spawning offsets by `fm->chunkOffsetX/Z * CHUNK_SIZE`.
- The pool is fixed at 300; `addMoreCreaturesIfNeeded` recycles far-away entities.
  Only `MAX_CREATURES_SAVED` (200 for an ordinary world) survive a save.
- `Model.cpp` uses `VecMath.h` types (`Vec3f`; `PVRTVec3` until LG.7) while the rest of the game uses
  `Vector` — conversion helpers `vpv/MakeVector2/MakePVR` in `Model.cpp` (names kept).
- All state is file-static; there can never be two worlds' creatures loaded.
- **`addMoreCreaturesIfNeeded` used to hang on land with no ground** (fixed H.4, 2026-10-15). When a
  random column had no ground (`ly == -1`, "oob model gen") it `continue`d without advancing the slot,
  so a window with no ground anywhere — a void world — retried the same slot forever and hung the
  load. It now gives up for this frame after 40 such tries (`oobTries`); creatures appear once land does.

## Safe vs. risky to modify
- **Safe:** AI tuning (timers, ranges), spawn density, new interactions following the
  existing dispatch pattern.
- **Caution:** `EntityData` layout (save format!), the pool/save-slot mapping,
  the animation math in `CreatureModel.cpp`/`VecMath.h` (bit-exact to the SDK; `--creature-selftest`
  fails on any change, and a deliberate one re-pins its hashes and says so), anything assuming `NUM_CREATURES==7`
  (state tables are per-species and hand-written).
