# Terrain Generation

## Purpose
Where world geometry comes from. There are **three distinct generators**, and the most
important one runs offline — understanding this split explains why "the generator"
seems to do so little at runtime.

## The three paths

| Path | File | When it runs | Output |
|---|---|---|---|
| Offline default-world generator | `TerrainGen2.mm` | Only in `JUST_TERRAIN_GEN` builds, on the developer's machine | The 2880×2880-block `Eden.eden` (RLE, repo root), baked to the shipped `Eden.emod` by `port/tools/emod.py bake-default` (checked by `verify-default`) |
| Runtime generator | `TerrainGenerator.mm` | Any column missing from both the save file and the bundle: seed 0 → flat, any other seed but 333333 → **Classic** (H.4) | Layered flat columns / the seeded 1.x terrain / air columns |
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

- `generateColumn(cx, cz, bgthread)` — three outcomes since H.4 (2026-10-15): **void**
  (`generateEmptyColumn`) when the world's GENERATOR record is from a newer Emod
  (`classicState == CLASSIC_STATE_VOID`) or a Classic "2880 square" world asks outside the square;
  **flat** for `LEVEL_SEED == 0`; otherwise **Classic** (below). Before H.4 the noise branches were dead
  (`FALSE&&LEVEL_SEED!=0`) and every seed got the flat recipe. The flat recipe: bedrock at y=0,
  stone to y=15, dirt to y=31, grass cap at y=32
  (`TerrainGenerator::generateColumn`). Those heights are fixed at `T_HEIGHT_DEFAULT/4` and
  `T_HEIGHT_DEFAULT/2` whatever the world height (2026-10-10). Until then they were `T_HEIGHT/4` and
  `T_HEIGHT/2`, so a 256z world got a 128-tall slab (grass at y=128) and its unsaved preview columns
  sat 96 blocks above the shared 256z worlds' saved ground at y=32 (seen in play on a shared half-adder
  world). Emod-made 256z flat worlds saved before the fix keep their y=128 columns, so they now meet
  newly generated columns at a 96-block cliff. Until H.4 the function also `memset` `CHUNKS_PER_COLUMN`×32³
  bytes of scratch twice per column, ~1 MB at 256z, for a commented-out half-block experiment; those
  memsets are commented out now (nothing reads `tblocks`/`tcolors`). Reuses the chunk objects in the table
  (`resetForReuse` + `setBounds`), copies types into `blockarray`, `addChunk` marks
  meshes dirty.
- `generateEmptyColumn` — same bookkeeping, all air; used beyond the default world's
  edge (this is the "edge of the world" players can reach ~1440 blocks from spawn in
  any direction... after which the world is void). That wall is `Player::update`'s position clamp, not
  terrain; X.6: it is skipped for "unbounded" worlds (see player-input-camera.md).
- `placeTree`, `generateCloud`, `noise2/3` — legacy 1.x-era generation, now mostly
  referenced by the offline generator.
- **The old noise branch is the 1.x generator, and it still reproduces 1.x
  terrain exactly** (verified 2026-10-15, H.3; `WORKING/feature-archaeology-2026-10-13/generator-plan.md` §1).
  H.4 wrapped that body in `#if 0` (archaeology) and re-implemented it as `classic_generate_window()`;
  the findings below are what the re-implementation keeps or fixes.
  With `genCaves` false it regenerates the unmodified saved columns of archived 2011–2014 worlds
  bit for bit, and the bottom 64 blocks of a 2026-game 256z world.
  - **The surface is not quite the code's.** 1.x flower patches are block 12 `TYPE_GRASS3` on the
    `(x+x%10)%20<4` grid (H.14 restored its flower top, tile `TEX_GRASS_FLOWER`), where the code places `TYPE_FLOWER` (73) on `TYPE_GRASS`, a post-1.x change.
  - **Trees are column-local** (they root only at local x/z 2–13, which leaves tree-free lanes), and clouds sit at y 61–62.
  - **`offsety` is `T_HEIGHT/2`**: that is right only at 64z, and the 1.x scale is 32.
  - **Worlds up to Aug 2011 had caves.** They use the same noise3 with a band reaching ~9 blocks below the surface and
    dark-stone veins at n3 ≤ 0.005. The surviving code has the later, deeper band (`< h−22`) and 0.01.
  - **The noise tables came from unseeded `random()`** in `init()`. macOS and glibc give the same sequence
    (1804289383, …), so those platforms match 1.x. The Windows shim's `random()` is arc4random-based, so on
    Windows the tables differed on every launch. **H.4 bakes them** (`Classes/ClassicNoiseTables.h`, 12,336 B,
    written by `WORKING/feature-archaeology-2026-10-13/h3-tools/bake_tables.py`); the old `init()` survives
    as `init_from_random()`, used only by `--classic-selftest --classic-mutate=tables`.

### Classic (H.4, 2026-10-15)
A pure function `classic_generate_window(g, wx0, wz0, W, D, height, types, colors, decorate)` of the
world's `ClassicGen` (= its GENERATOR record, [emod-file-format.md](emod-file-format.md)) and the block
coordinates: no engine state, no RNG state, so a column streams in identical every time and a 64×64
window equals the 16 columns it covers. Design and every constant: `generator-plan.md` §3 and its
"H.4 as built" notes.
- **Two profiles.** *Legacy* (a 1.x random-seed world, `classic_legacy_params`): scale 1×, Classic
  hilliness, column-aligned 1.x trees, caves off unless detected (below) — its unexplored columns are
  what 1.x would have generated. *New* (New World's Classic tab, `classic_default_params` + the tab's
  choices): trees root ±2 columns across borders (no tree-free lanes), the 1.x flower grid as block 12.
- **The 1.x terrain is bit-exact** at scale 1×, Classic hilliness, caves off: `--classic-selftest`
  regenerates 159 saved columns of a 2011 world (`port/tools/fixtures/classic/legacy-1315365413.bin.gz`)
  byte for byte, and pins four profile hashes that CI checks on every target. Keep the float expressions
  in the 1.x order and the file compiled with `-ffp-contract=off -fno-fast-math`
  (`native/CMakeLists.txt` sets it per source): one fused multiply-add moves a cell.
- **Scale** (`scale4` = 4·s; 256z only): heightmap `n = 8·scale4`, frequency `8/scale4`, amplitude `scale4`;
  bands deepen by `s`. 3D noise (caves, skin) is sampled on a lattice of `scale4/4` blocks and
  trilinearly interpolated (x, then z, then y lazily per node level), so 4× costs about what 1× does.
- **Caves:** *2011* (the Aug-2011 band, `3+R%3` up to ~9·s below the surface, opening into hillsides,
  veins at n3 ≤ 0.005) or *Deep* (the surviving code's band, `h%2+1` to 16·s below; veins 0.01).
- **Decoration** (grass kinds, flowers, trees, clouds) is a SplitMix64 hash of (seed, x, z, y, salt),
  not `arc4random()` as in 1.x.
- **Cost** (M2, generation only): legacy 0.08 ms/column, 1× new 0.20, 2× 256z 0.23, 4× 256z 0.27.
  `Terrain::update`'s bulk reload charges generated columns by time (`GEN_FRAME_BUDGET_MS` 4 ms, at least
  one a frame) rather than against the chunk budget.
- **Legacy cave detection** (`FileManager.mm` `fm_detectLegacyCaves`, at load, once): ≤ 64 saved columns
  vote with `classic_cave_votes`; ≥ 2,000 cells the 2011 model carves and at least half of them air in the
  save ⇒ caves 2011, `flags` bit 0. The result goes into the GENERATOR record written by the next save.

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
  set `FileManager::genflat`, i.e. seed 0 vs 333333. The Classic tile (H.4) is "normal" to that seam plus
  a parked GENERATOR record (`eden_menu_set_pending_generator`); FileManager's new-world branch takes it,
  sets `LEVEL_SEED` to the record's seed (in [1, 2^24), never 333333; `NewWorldMenu::seedFromText`) and
  spawns on the first dry top found spiralling out from block 4096·16+8 (`fm_classicSpawn`).
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
  - H.4 gives their unexplored columns the original terrain again (the legacy profile, always 1×), and
    writes a GENERATOR record at their next save so later builds never have to guess.
  - Which generator a world uses is decided once per load in `fm_setupGenerator` (`FileManager.mm`):
    seed 0 / 333333 → none; a GENERATOR record → it (unknown revision → void + a toast); else a header
    mirror (an imported `.eden`) → it; else the legacy profile + cave detection. The decision itself is
    `fm_resolveGenerator()` since H.15, which World Properties also calls, so its Terrain tab shows
    exactly what the next load uses.
  - **Changing an existing world's settings (H.15, 2026-10-15).** World Properties (ui.md) appends a newer
    GENERATOR to a closed Classic world: Hilliness, Caves, Trees, Flowers, Clouds, Size and (new 256z
    worlds only) Scale; never the seed, the height or the profile. Because a world stores only the
    columns the player changed and everything else is a pure function of (seed, settings), the change
    reshapes **all unsaved land, explored or not**; saved columns stay byte-for-byte, so their edges can
    show. Setting the old values back restores the old untouched land exactly (gate: `--props-selftest`).
    Changing Caves drops the record's "detected" flag. `home`/`pos` in unsaved columns are lifted to the
    new surface (save-load.md).

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
