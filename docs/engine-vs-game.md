# Engine vs. Game: Separation Map & Refactoring Roadmap

This doc classifies the code by reusability and sketches how the project could evolve
toward a clean engine/game split — **as guidance for future porting/refactoring work,
not something to do now**.

## Classification

### Already-generic (engine-grade as-is)
| Component | Files | Notes |
|---|---|---|
| Hashmap | `hashmap.mm/.h` | int→ptr map, no deps |
| Frustum culling | `Frustum.mm/.h` | plane extraction + AABB test, pure C |
| GLU port | `glu.h`, `project.c`, `glue.c`, … | unproject/perspective |
| Math & SAT collision | `Util.mm` (vector ops, `collidePolyhedra`, `makeBox/Ramp/Side`), `VectorUtil.cpp`, `Vector.h` | needs extraction from the grab-bag `Util.mm`, which also holds game code |
| Texture/string rendering | `Texture2D.mm` | UIKit-dependent but self-contained |
| Async HTTP | `FileDownload.mm`, `FileUpload.mm` | Foundation-dependent, generic |
| Audio | CocosDenshion stack | already a library |
| Model loading/animation | PVRT suite | already a library |
| Compression/hash | `zpipe`, `md5` | libraries |

### Engine-shaped but entangled (the real refactoring targets)
| Component | Files | Entanglements to cut |
|---|---|---|
| Frame loop / GL surface | `EdenViewController`, `EAGLView`, `Graphics` | global screen metrics; retina-swap logic knows about game modes |
| Voxel store + toroidal paging | `Terrain` (storage half), `blockarray` macros | reads `World::getWorld` for player position; `blockinfo` tables baked in |
| Chunk mesher | `TerrainChunk::rebuild2/prepareVBO` | hard-codes Eden's block semantics (liquids, ramps, atlas layout, paint rules); the bucketed-faces + directional-culling core is generic and valuable |
| Column file format + streaming | `FileManager` | static state; UI callouts (statusbar) mid-I/O; Menu supplies display name during save |
| Particle systems | `SpecialEffects`, `Fire`, `BlockBreak` | pull textures from `Resources` by game-specific ids |
| Input layer | `Input.mm` | generic 5-touch tracker, but consumers poll it with game rules |
| GL immediate-UI kit | `statusbar`, `VKeyboard`, `Alert`, `Button`/`inbox*`, `Joystick` | a decent retained-nothing UI toolkit hiding inside game files |

### Purely game-specific
`World` (mode machine), block tables (`Constants.h`, `Globals.mm`), gameplay ops in
`Terrain` (build/destroy/burn/explode/paint), `Player`, `Hud`, `Menu`+satellites,
`Model.mm` creatures & AI, `Liquids`, `Portal`, `Firework`, `Lighting` policy,
`TerrainGenerator`/`TerrainGen2` recipes, `ShareUtil` endpoints, spawn tables.

## Seams that make the split feasible

1. **`blockinfo` flags are already the abstraction.** The mesher, physics, fire and
   liquids almost never test raw type ids — they test flags (`IS_NOTSOLID`,
   `IS_ATLAS2`, `IS_FLAMMABLE`…). Promoting the block tables to data files gives you
   a generic voxel engine with Eden as its first content pack. The exceptions
   (explicit `TYPE_*` range checks for ramps/doors/portals/liquid levels) are
   enumerable — grep `TYPE_STONE_RAMP1` and `TYPE_DOOR` to find them all.
2. **The C-function façades are accidental interfaces.** `Model.h`, `Lighting.h`,
   the `getLandc`-family — they already isolate callers from internals; formalizing
   them as engine APIs is mostly renaming.
3. **`World::getWorld->…` chains are the dependency graph made visible.** Every such
   chain is a constructor-injection candidate; there are only ~8 distinct
   dependencies behind them (terrain, player, hud, fm, effects, res, cam, menu).
4. **File format vs. FileManager**: the format (header/columns/directory arithmetic)
   is pure logic separable from NSFileHandle; a portable `eden_format.c` would serve
   the game, tools, and the server alike.

## Suggested incremental roadmap (lowest risk first)

1. **Documentation & tests first** (done here): pin the file format with a round-trip
   test against the bundled `Eden.eden` before touching any I/O code.
2. **Extract pure-C modules**: math/SAT out of `Util.mm`; file-format arithmetic out
   of `FileManager` (keep NSFileHandle at the edges).
3. **Data-drive the block tables**: generate `Constants.h`/`Globals.mm` tables from a
   single declarative source; eliminates the multi-file update trap.
4. **De-globalize screen metrics** behind a `Display` struct passed to Graphics/UI.
5. **Split Terrain** into `VoxelStore` (arrays, paging, dirty lists) and
   `TerrainGameplay` (build/burn/explode/paint) — the current class already has this
   internal boundary.
6. **Mesher isolation**: give `rebuild2` an explicit input interface (block sampler +
   material table) instead of globals — prerequisite for re-threading it safely or
   porting to ES2/Metal.
7. **Renderer port** (the big one): the fixed-function usage is narrow (matrices,
   vertex arrays, fog, two lights, texture matrix). A thin RHI covering those would
   unlock ES2/Metal/GL desktop with the same passes. The texture-matrix atlas trick
   and vertex-color baking translate directly to a trivial shader.
8. Only after 5–7: revisit background meshing/streaming with real synchronization —
   the original author's `//issue` comments are the test plan.

## Porting notes (non-iOS targets)
Platform-touching code is confined to: `EAGLView`/`EdenViewController` (surface +
loop + touches), `Texture2D` (image decode, string raster), Foundation file I/O in
`FileManager`/helpers, CocosDenshion (audio), NSURLConnection networking, and
`arc4random`/`CFAbsoluteTimeGetCurrent` sprinkles. Everything else is C/C++ with
OpenGL ES 1.1 — the `Eden_mac`/`Eden-mac` stubs in the repo suggest a desktop port
was once contemplated but never started.
