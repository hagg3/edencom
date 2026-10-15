# Terrain Generation

## Purpose
Where world geometry comes from. There are **three distinct generators**, and the most
important one runs offline — understanding this split explains why "the generator"
seems to do so little at runtime.

## The three paths

| Path | File | When it runs | Output |
|---|---|---|---|
| Offline default-world generator | `TerrainGen2.mm` | Only in `JUST_TERRAIN_GEN` builds, on the developer's machine | The 2880×2880-block `Eden.eden` (RLE, repo root), baked to the shipped `Eden.emod` by `port/tools/emod.py bake-default` (checked by `verify-default`) |
| Runtime flat/empty generator | `TerrainGenerator.mm` | Seed 0 worlds; any column missing from both the save file and the bundle | Layered flat columns / air columns |
| Bundle streaming | `FileManagerHelper.mm` | Seed 333333 (`DEFAULT_LEVEL_SEED`) worlds — i.e. every normal world | columns copied out of the bundled map — `Eden.emod` since S.6 (the zstd bake of `Eden.eden`; the RLE original is the fallback) |

So at runtime, a normal world's "terrain generation" is *file decompression*: every
default world starts as a byte-identical copy-on-write view of the same pre-generated
2880×2880 map, and only columns the player modifies get written into their own save
file. This is why all default worlds share the same landscape and the same 10
hand-picked spawn points (the `spx`/`spz`/`spy` tables in `FileManager::loadWorld`).

## Offline generator — `TerrainGen2.mm`

Activated by setting `JUST_TERRAIN_GEN` to 1 in `World.h` (then run it once and set it back to 0); `World::World()` then
short-circuits into: `fm->loadGenFromDisk()` (optional biome-map PNG — currently
mostly disabled), `tg2_init()`, `fm->writeGenToDisk()`, and renders a 2D debug map
(`tg2_render`) instead of the game.

Working buffers: `block8* blockz`, `color8* colorz` sized
`GSIZE×GSIZE×T_HEIGHT` where `GSIZE = 2880` (literal since B.5g; was `T_SIZE*10`) (~1 GB total — desktop/
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

`FileManager::writeGenToDisk()` then emits `Eden.eden`: standard header
(seed 0 in the file — the *loader* keys off the world's own seed, not the bundle's),
RLE columns via `saveGenColumn` (transposed voxel order for better runs — see
[eden-file-format.md](eden-file-format.md)), directory at the end. Replace the repo-root `Eden.eden`
with it, then re-bake the shipped bundle with `emod.py bake-default`.

Region sky colors: the 4×4 `defaultRegionSkyColors` grid (`FileManager.mm`) maps
quadrants of the default world to sky palette entries; `updateSkyColor1/2`
(`TerrainGen2.mm`) pick the region under the player and drive the sky crossfade.
Painting the sky (paint tool aimed at the sky) calls `paintSky` which edits
`regionSkyColors` — a per-world-region, persisted-in-header effect.

## Runtime generator — `TerrainGenerator.mm`

- `generateColumn(cx, cz, bgthread)` — despite the dead noise-based branches
  (`FALSE&&LEVEL_SEED!=0`), the live code path is the flat recipe: bedrock at y=0,
  stone to y=15, dirt to y=31, grass cap at y=32
  (`TerrainGenerator::generateColumn`). Those heights are fixed at `T_HEIGHT_DEFAULT/4` and
  `T_HEIGHT_DEFAULT/2` whatever the world height (2026-10-10). Until then they were `T_HEIGHT/4` and
  `T_HEIGHT/2`, so a 256z world got a 128-tall slab (grass at y=128) and its unsaved preview columns
  sat 96 blocks above the shared 256z worlds' saved ground at y=32 (seen in play on a shared half-adder
  world). Emod-made 256z flat worlds saved before the fix keep their y=128 columns, so they now meet
  newly generated columns at a 96-block cliff. The function also `memset`s
  `CHUNKS_PER_COLUMN`×32³ bytes of scratch twice per column, ~1 MB at 256z. (Noted 2026-10-03, `.emod`
  F.2; read from the code, not observed in play.) Reuses the chunk objects in the table
  (`resetForReuse` + `setBounds`), copies types into `blockarray`, `addChunk` marks
  meshes dirty.
- `generateEmptyColumn` — same bookkeeping, all air; used beyond the default world's
  edge (this is the "edge of the world" players can reach ~1440 blocks from spawn in
  any direction... after which the world is void). That wall is `Player::update`'s position clamp, not
  terrain; X.6: it is skipped for "unbounded" worlds (see player-input-camera.md).
- `placeTree`, `generateCloud`, `noise2/3` — legacy 1.x-era generation, now mostly
  referenced by the offline generator.
- **The dead noise branch below the `return` is the 1.x generator, and it still reproduces 1.x
  terrain exactly** (verified 2026-10-15, H.3; `WORKING/feature-archaeology-2026-10-13/generator-plan.md` §1).
  With `genCaves` false it regenerates the unmodified saved columns of archived 2011–2014 worlds
  bit for bit, and the bottom 64 blocks of a 2026-game 256z world.
  - **The surface is not quite the code's.** 1.x flower patches are block 12 `TYPE_GRASS3` on the
    `(x+x%10)%20<4` grid, where the code places `TYPE_FLOWER` (73) on `TYPE_GRASS`, a post-1.x change.
  - **Trees are column-local** (they root only at local x/z 2–13, which leaves tree-free lanes), and clouds sit at y 61–62.
  - **`offsety` is `T_HEIGHT/2`**: that is right only at 64z, and the 1.x scale is 32.
  - **Worlds up to Aug 2011 had caves.** They use the same noise3 with a band reaching ~9 blocks below the surface and
    dark-stone veins at n3 ≤ 0.005. The surviving code has the later, deeper band (`< h−22`) and 0.01.
  - **The noise tables come from unseeded `random()`** in `init()`. macOS and glibc give the same sequence
    (1804289383, …), so those platforms match 1.x. The Windows shim's `random()` is arc4random-based, so on
    Windows the tables differ on every launch. This is harmless today, because only TG2 (offline) evaluates the noise.

## Runtime bundle streaming — `FileManagerHelper.mm`
Opened once at app start (`fmh_init` from the FileManager constructor): opens the bundled
`Eden.emod` read-only through `emod::EdenWorldStore` (S.6). Only a bundle without `Eden.emod` falls
back to an NSFileHandle + directory hashmap over an RLE `Eden.eden` — and the native bundle ships no
`Eden.eden`, so on a shipped build that path is unreachable (`--bundled-map-selftest` covers the
bake). `fmh_readColumnFromDefault(cx,cz)` decodes a column into the reusable chunks (details in
[save-load.md](save-load.md)). Thread-consideration: this is called from
the loading pthread during initial load and from the main thread during streaming —
the shared file handle's seek/read pairs are the point of fragility if you ever
parallelize loading.

Since B3/B6 it is three functions rather than one — a main-thread
`fmh_readColumnRawFromDefault` (the store's zstd read, or the RLE fallback's seek + read), a pure
`fmh_decodeColumnBands` (run-expansion + the `CC(x,z,y)`↔`CC(y,z,x)` band transpose, which could
run on a worker under `EDEN_THREADED`, defined by no current build) and a main-thread
`fmh_publishColumnFromDefault`. *RLE fallback only (history since S.6):* the raw read pulls the **whole column record in one `readDataOfLength:`** of
`FMH_RECORD_HINT` (4 KB), slicing the four bands' 2-byte length prefixes and payloads
out of memory, with a top-up read for the rare record that runs past the hint. It used
to issue eight reads per column (a prefix and a payload per band); a record averages
~1.2 KB in the bundled map, so those eight reads were mostly per-call overhead. Do not
size the hint from the worst record seen — that makes every small column pull the big
one's bytes, and it measured worse than the occasional top-up.

## Seeds
- `LEVEL_SEED == 0` — flat world. New World's Flat / Normal tiles (U.5, `NewWorldMenu.mm` `kTypes`)
  set `FileManager::genflat`, i.e. seed 0 vs 333333. A seeded generator with caves ("Classic", a third
  tile) is planned (ROADMAP H.4, plan `WORKING/feature-archaeology-2026-10-13/generator-plan.md`); none is built.
- `LEVEL_SEED == DEFAULT_LEVEL_SEED (333333)` — bundled default world.
- Any other value — historical: 1.x used real seeded noise per column; in 2.1 the
  random-seed branch in `loadWorld` still exists (`arc4random()%300000`) but is
  unreachable because `g_terrain_type` is forced to 9 (in `FileManager::loadWorld`).
  Old worlds with such seeds fall through to `generateColumn`'s flat recipe for
  missing columns. Confidence: high (code inspection); behaviour for legacy noise
  worlds' *ungenerated* areas therefore differs from the original 1.x app.
  - Census (H.3, 2026-10-15): 140 of 359 archived worlds have such a seed, all in 1,366–299,743
    (`arc4random()%300000`). 107 of them are 2011 v2 files, and 7 are 2026-game 256z files whose 1.x terrain still lies at y < 64.
  - No world made after 2.0 has a fresh random seed, so "seed ∉ {0, 333333}" means "made by 1.x".
  - H.4 plans to give their unexplored columns the original terrain again, always at the 64z scale.

## Common pitfalls
- Don't call anything in `TerrainGen2.mm` at runtime on device — the buffers are
  sized for desktops.
- `GSIZE` and the `BLOCK/COLOR` macros silently truncate/alias if you pass world
  coordinates instead of gen-buffer coordinates (gen space is 0..2879; world space is
  offset by `centerChunk−r` = chunk 4006... specifically `centerChunk=4096`,
  `r=GSIZE/CHUNK_SIZE/2=90`).
- The repo-root `Eden.eden` is the source of the shipped `Eden.emod` and the reference specimen for
  the `.eden` format — don't regenerate either casually; every default world's unmodified terrain
  comes from them.

## Safe vs. risky to modify
- **Safe:** biome recipes and feature placers in `TerrainGen2.mm` (then regenerate the
  bundle in a `JUST_TERRAIN_GEN` build and re-bake `Eden.emod`), the flat-world layer heights.
- **Caution:** `DEFAULT_LEVEL_SEED` handling, spawn-point tables, anything about how
  missing columns resolve (the three-way fallback in `readColumn` is subtle and
  ordering-dependent).
