# Terrain Generation

## Purpose
Where world geometry comes from. There are **three distinct generators**, and the most
important one runs offline — understanding this split explains why "the generator"
seems to do so little at runtime.

## The three paths

| Path | File | When it runs | Output |
|---|---|---|---|
| Offline default-world generator | `TerrainGen2.mm` (2918 lines) | Only in `JUST_TERRAIN_GEN` builds, on the developer's machine | The 2880×2880-block `Eden.eden` bundled with the app |
| Runtime flat/empty generator | `TerrainGenerator.mm` | Seed 0 worlds; any column missing from both the save file and the bundle | Layered flat columns / air columns |
| Bundle streaming | `FileManagerHelper.mm` | Seed 333333 (`DEFAULT_LEVEL_SEED`) worlds — i.e. every normal world | RLE-decoded columns copied out of the bundled `Eden.eden` |

So at runtime, a normal world's "terrain generation" is *file decompression*: every
default world starts as a byte-identical copy-on-write view of the same pre-generated
2880×2880 map, and only columns the player modifies get written into their own save
file. This is why all default worlds share the same landscape and the same 10
hand-picked spawn points (`FileManager.mm:1425`).

## Offline generator — `TerrainGen2.mm`

Activated by setting `JUST_TERRAIN_GEN` to 1 in `World.h:35`; `World::World()` then
short-circuits into: `fm->loadGenFromDisk()` (optional biome-map PNG — currently
mostly disabled), `tg2_init()`, `fm->writeGenToDisk()`, and renders a 2D debug map
(`tg2_render`) instead of the game.

Working buffers: `block8* blockz`, `color8* colorz` sized
`GSIZE×GSIZE×T_HEIGHT` where `GSIZE = T_SIZE*10 = 2880` (~1 GB total — desktop/
simulator only). Access macros `BLOCK(x,z,y)` / `COLOR(x,z,y)` in `TerrainGen2.h`.

`tg2_init()` orchestrates the biome recipes (all in this file, grep-able by name):
`makeGreenHills`, `makeMountains`, `makeRiverTrees`, `makeDesert`, `makeBeach`,
`makeMars`, `makePonies` (unicorn/rainbow biome), `makeMix`, `makeOcean`,
`makeTransition` (biome edge blending), plus feature placers: `makeVolcano`,
`makePyramid`/`makePyramid2`, `makeWorm` (cave worms, `WORM_FREQ 300`), `makeCave`,
`makeSkyIsland`, `makeTree`/`makeTree2`/`makePalmTree`, `floodFill` (water filling),
`genTemperatureMap`, and the `colorCycle*` helpers that paint gradient color schemes.
Perlin-style noise comes from `noise2`/`noise3` (implementation shared with
`TerrainGenerator.mm`).

`writeGenToDisk()` (`FileManager.mm:258`) then emits `Eden.eden`: standard header
(seed 0 in the file — the *loader* keys off the world's own seed, not the bundle's),
RLE columns via `saveGenColumn` (transposed voxel order for better runs — see
[eden-file-format.md](eden-file-format.md)), directory at the end.

Region sky colors: the 4×4 `defaultRegionSkyColors` grid (`FileManager.mm:36`) maps
quadrants of the default world to sky palette entries; `updateSkyColor1/2`
(`TerrainGen2.mm:2765+`) pick the region under the player and drive the sky crossfade.
Painting the sky (paint tool aimed at the sky) calls `paintSky` which edits
`regionSkyColors` — a per-world-region, persisted-in-header effect.

## Runtime generator — `TerrainGenerator.mm`

- `generateColumn(cx, cz, bgthread)` — despite the dead noise-based branches
  (`FALSE&&LEVEL_SEED!=0`), the live code path is the flat recipe: bedrock at y=0,
  stone to y=15, dirt to y=31, grass cap at y=32
  (`TerrainGenerator.mm:210-239`). Reuses the chunk objects in the table
  (`resetForReuse` + `setBounds`), copies types into `blockarray`, `addChunk` marks
  meshes dirty.
- `generateEmptyColumn` — same bookkeeping, all air; used beyond the default world's
  edge (this is the "edge of the world" players can reach ~1440 blocks from spawn in
  any direction... after which the world is void).
- `placeTree`, `generateCloud`, `noise2/3` — legacy 1.x-era generation, now mostly
  referenced by the offline generator.

## Runtime bundle streaming — `FileManagerHelper.mm`
Opened once at app start (`fmh_init` from the FileManager constructor): keeps its own
NSFileHandle + directory hashmap for the bundled `Eden.eden`.
`fmh_readColumnFromDefault(cx,cz)` decodes the RLE column into the reusable chunks
(details in [save-load.md](save-load.md)). Thread-consideration: this is called from
the loading pthread during initial load and from the main thread during streaming —
the shared file handle's seek/read pairs are the point of fragility if you ever
parallelize loading.

Since the web port's B3/B6 work it is three functions rather than one — a main-thread
`fmh_readColumnRawFromDefault` (seek + read the raw RLE bytes), a pure
`fmh_decodeColumnBands` (run-expansion + the `CC(x,z,y)`↔`CC(y,z,x)` band transpose,
which is what can run on a worker) and a main-thread `fmh_publishColumnFromDefault`.
The raw read pulls the **whole column record in one `readDataOfLength:`** of
`FMH_RECORD_HINT` (4 KB), slicing the four bands' 2-byte length prefixes and payloads
out of memory, with a top-up read for the rare record that runs past the hint. It used
to issue eight reads per column (a prefix and a payload per band); a record averages
~1.2 KB in the bundled map, so those eight reads were mostly per-call overhead. Do not
size the hint from the worst record seen — that makes every small column pull the big
one's bytes, and it measured worse than the occasional top-up.

## Seeds
- `LEVEL_SEED == 0` — flat world.
- `LEVEL_SEED == DEFAULT_LEVEL_SEED (333333)` — bundled default world.
- Any other value — historical: 1.x used real seeded noise per column; in 2.1 the
  random-seed branch in `loadWorld` still exists (`arc4random()%300000`) but is
  unreachable because `g_terrain_type` is forced to 9 (`FileManager.mm:1368`).
  Old worlds with such seeds fall through to `generateColumn`'s flat recipe for
  missing columns. Confidence: high (code inspection); behaviour for legacy noise
  worlds' *ungenerated* areas therefore differs from the original 1.x app.

## Common pitfalls
- Don't call anything in `TerrainGen2.mm` at runtime on device — the buffers are
  sized for desktops.
- `GSIZE` and the `BLOCK/COLOR` macros silently truncate/alias if you pass world
  coordinates instead of gen-buffer coordinates (gen space is 0..2879; world space is
  offset by `centerChunk−r` = chunk 4006... specifically `centerChunk=4096`,
  `r=GSIZE/CHUNK_SIZE/2=90`).
- The bundled `Eden.eden` at the repo root is both a runtime asset and the reference
  specimen for the file format — don't regenerate it casually; every default world's
  unmodified terrain comes from it.

## Safe vs. risky to modify
- **Safe:** biome recipes and feature placers in `TerrainGen2.mm` (then regenerate the
  bundle in a `JUST_TERRAIN_GEN` build), the flat-world layer heights.
- **Caution:** `DEFAULT_LEVEL_SEED` handling, spawn-point tables, anything about how
  missing columns resolve (the three-way fallback in `readColumn` is subtle and
  ordering-dependent).
