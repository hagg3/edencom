# World Representation & Terrain System

## Purpose
How voxels are stored in memory, how the "infinite" world illusion works, how blocks
are read/edited, and how gameplay dynamics (fire, TNT, doors, portals) are layered on
top of the voxel grid.

## Key constants (`Classes/Constants.h`)

| Constant | Value | Meaning |
|---|---|---|
| `CHUNK_SIZE` | 16 | Blocks per chunk edge |
| `T_SIZE` | 288 | Resident window edge, in blocks (18 chunks) |
| `T_HEIGHT` | **64 or 256, runtime** | World height, blocks (4 or 16 chunks) |
| `T_RADIUS` | 9 | Half the window, in chunks |
| `CHUNKS_PER_SIDE` | 18 | Window edge in chunks |
| `CHUNKS_PER_COLUMN` | **4 or 16, runtime** | Vertical chunks |
| `NUM_BLOCKS` | 111 | Highest block type id |
| `BLOCK_SIZE` | 1.0f | World units per block |

## Runtime world height (2026-08-06)

World height used to be hard-capped at 64 by a `#define`. It is now a **per-world runtime
value**: 64 for every world this engine creates, 256 for a loaded `.eden` whose header
`version` is 5 or 6 — the 256-block-tall "New Dawn" variant (see
[eden-file-format.md](eden-file-format.md)). One binary opens both; **64z stays the default
and costs exactly what it always cost**.

`T_HEIGHT`, `T_BLOCKS`, `CHUNKS_PER_COLUMN` and `MAX_CREATURES_SAVED` are now macros over
globals (`g_world_height`, `g_t_blocks`, `g_chunks_per_column`, `g_max_creatures_saved`), set
by `eden_set_world_height()` / `eden_set_creature_slots()` in `Classes/Globals.mm`. Three
rules follow, and breaking any of them is silent:

1. **A fixed-size declaration cannot use them.** Use `T_HEIGHT_MAX` (256),
   `CHUNKS_PER_COLUMN_MAX` (16) or `MAX_CREATURES_SAVED_MAX` (400) for anything whose size is
   fixed at compile time (`creatureData[]`, `renderList[]`, `TerrainGenerator`'s `tblocks`/
   `tcolors` scratch, the `columns[]` locals, `prepareAndLoadGeometry`'s dirty list). The
   compiler catches this one — a VLA at file scope is an error.
2. **The height must be set before `Terrain::allocateMemory()`**, which sizes `blockarray`,
   `lightarray` and the chunk table from it. `World::loadWorld` does that by probing the save
   file's header (`FileManager::probeWorldHeight`) before it allocates. A world that does not
   exist yet answers 64, which is what makes "new worlds are 64z" true by construction.
3. **The hot path pays for the stride, not the multiply.** `GBLOCKIDXCLEAN` uses a precomputed
   `g_xz_stride` (`T_SIZE*g_world_height`) rather than multiplying two globals per access.
   Measured on the load+mesh path (`tools/headless-load-timing.js`, 9 runs per build,
   2026-08-06): median first-load 67.0 ms with runtime height vs 67.0 ms with the old literal
   constants, and the runtime build's `eden.wasm` is 540 B **smaller**. The cost is not
   detectable.

### Memory: what a tall world actually costs

Measured in `build-st` (Debug) against a real 18×18-column 256z world whose terrain reaches
y≈250, 2026-08-06:

| | menu only | 64z world resident | 256z world resident |
|---|---:|---:|---:|
| wasm heap | 96 MB | 128 MB | **413 MB** |

The per-world arrays account for ~95 MB of that (`blockarray` 5.4 → 21.5 MB, `lightarray`
15.9 → 63.7 MB, 1296 → 5184 chunk objects at 8 KB each); the rest is **mesh** memory, which
scales with how much solid terrain there actually is, not with the height alone — a tall world
that is mostly air costs much less (the flat 256z specimen sits at 248 MB). `-sINITIAL_MEMORY`
stays at 96 MB (audit row E1's conclusion is unchanged): growth is on, and over-reserving would
only raise the floor for the menu-only session that every player starts in. **Practical
consequence: 256z is a desktop feature on web.** iOS Safari's per-tab ceiling is well under
400 MB — the same constraint that produced audit row A11.

## Block identity: type + color

Every voxel is two bytes:
- `block8` type — `enum BLOCK_TYPES` in `Constants.h` (0 = air, 1 = bedrock, …,
  ramps/side-ramps in groups of 4 orientations, liquids at 4 fill levels, doors and
  portals as 2-block-tall composites, `TYPE_BT*` = "block TNT" variants that explode
  into a 5×5 patch of their base material).
- `color8` paint — index into the global `colorTable[256]`; 0 = unpainted.
  The table is generated procedurally in `Hud::genColorTable()` (`Hud.mm:151`):
  entry 0 is white, then 54 entries = 9 hues × 6 shades via HSV (hue column 8 is the
  grayscale ramp). `NUM_COLORS` is 54.

Per-type static properties live in `Globals.mm`:
- `blockinfo[NUM_BLOCKS+1]` — bit flags (`IS_FLAMMABLE`, `IS_NOTSOLID`, `IS_RAMP`,
  `IS_ATLAS2` = drawn in transparent pass, `IS_LIQUID`, `IS_WATER/IS_LAVA`,
  `IS_OBJECT` = rendered as a mesh object not a cube, `IS_DOOR`, `IS_PORTAL`,
  `IS_HARD`, `IS_BLOCKTNT`, …). **This table is the single source of truth for block
  behaviour**; adding a block type means adding rows here, in `blockTypeFaces`
  (face→texture mapping) and `blockColor` (default RGB).
- `blockTypeFaces[type][6]` — which atlas tile each of the 6 faces uses.
- `blockColor[type][3]` — intrinsic tint.

## The toroidal resident window

The world is conceptually huge (chunk coordinates are packed into 15 bits each by
`twoToOne` — `Util.mm:1053` — so the addressable world is 32,768 × 32,768 chunks;
the default world's centre sits at chunk (4096, 4096) ⇒ block ≈ 65,536).

Only a **288×288×`T_HEIGHT` window around the player** (64 tall normally, 256 for a v5/v6 world) is in memory, in two parallel
structures, both indexed *modulo the window size* so absolute world coordinates can be
used directly:

1. **`block8* blockarray`** — flat type-only cache used by all hot-path reads
   (meshing, collision, lighting). Access macros in `Terrain.h:33-37`:
   ```c
   GBLOCKIDX(x,z,y) = ((x+g_offcx)%T_SIZE)*g_xz_stride
                    + ((z+g_offcz)%T_SIZE)*g_world_height + y
   // g_xz_stride == T_SIZE*T_HEIGHT, precomputed; the height is runtime (see above)
   ```
   `g_offcx = g_offcz = T_SIZE*100` exist only to keep the `%` result positive.
   Because indexing is modular, **no data moves when the player walks** — streaming
   overwrites the cells that now map to the newly-entered columns.
   Note colors are *not* in this array; color reads go through the chunk objects.

2. **`TerrainChunk** chunkTable`** — 18×18×`CHUNKS_PER_COLUMN` = 1296 chunk objects (5184 at 256z), allocated once in
   `Terrain::allocateMemory()` (`Terrain.mm:241`) and **reused forever** (never
   freed/reallocated during play; `resetForReuse()`/`setBounds()` repurpose them).
   Indexed by the `threeToOne(cx,cy,cz)` macro (`Util.h:120`), again modulo
   `CHUNKS_PER_SIDE`. A chunk knows its absolute bounds in `pbounds[6]` — comparing
   `pbounds` against the expected coordinates is how the code detects that a table
   slot still holds a stale, faraway chunk (see streaming below).

Each `TerrainChunk` (`TerrainChunk.h`) owns:
- `pblocks[4096]`, `pcolors[4096]` — authoritative type+color, layout `CC(x,z,y) =
  x*256 + z*16 + y` (y fastest ⇒ vertical strips are contiguous, matching the save
  format).
- Mesh state (see [rendering.md](rendering.md)): staging vertex arrays, per-face
  vertex counts, GL buffer names — in two copies, plain and `rt*`-prefixed
  ("render-thread") versions swapped by `prepareVBO()`.
- `StaticObject* objects` — extracted door/portal/golden-cube/flower instances.
- `modified` flag — set on any edit; drives incremental saving.

`Vector8* lightarray` (RGB byte per voxel, same toroidal indexing) holds the colored
point-light field; see [lighting-liquids-effects.md](lighting-liquids-effects.md).
Skipped entirely on `LOW_MEM_DEVICE`.

### Memory budget
blockarray ≈ 5.3 MB, chunk blocks+colors ≈ 10.6 MB, lightarray ≈ 15.9 MB, plus
transient meshes. This is why `LOW_MEM_DEVICE` (< ~300 MB RAM) drops the light field
and loads synchronously.

## Reading and writing blocks (`Terrain.mm`)

Read APIs (world coordinates, `(x, z, y)` order — see
[conventions-and-pitfalls.md](conventions-and-pitfalls.md)):
- `getLandc(x,z,y)` — free function, raw `GBLOCK`, no bounds check except what the
  macro provides. Fast path used everywhere.
- `Terrain::getLand` — adds y bounds check and a fully-wrapped `GBLOCK_SAFE`.
- `getColorc` / `Terrain::getColor` — go through the chunk's `pcolors`.

Write path — always go through these, never poke arrays directly:
- `Terrain::setLand(x,z,y,type,chunkToo)` — writes `blockarray` and (if `chunkToo`)
  the owning chunk's `pblocks` + `modified` flag. Does **not** mark meshes dirty.
- `Terrain::updateChunks(x,z,y,type)` (`Terrain.mm:1394`) — the standard "place a
  block" primitive: clears color if erasing, calls `setLand`, then marks the chunk
  and all 6 face-neighbouring chunks dirty via `addToUpdateList2`.
- `Terrain::setColor` — writes chunk color, returns whether it changed.

Dirty-list mechanics: `chunksToUpdate[]` (per chunk) + `columnsToUpdate[]` (per
column, an iteration accelerator). Drained once per frame by
`prepareAndLoadGeometry`; `chunksToUpdateImmediatley[]` then queues the rebuilt
chunks for VBO upload in `updateAllImportantChunks`.

## Gameplay operations on blocks

All in `Terrain.mm`:

- **`buildBlock(x,z,y)`** (`:1042`) — places `hud->blocktype` with
  `hud->block_paintcolor`. Ramp types auto-orient: `getRampType` inspects which of the
  4 side neighbours are solid; two adjacent solid sides ⇒ becomes a corner ("side")
  piece, otherwise faces the player's yaw quadrant. Doors/portals place two stacked
  blocks (`TYPE_*1+r` bottom encoding direction, `TYPE_*_TOP` top). Lightboxes call
  `addlight(…, +1)` and refresh chunks in `LIGHT_RADIUS`. Golden cubes decrement the
  `goldencubes` inventory.
- **`destroyBlock` / `explodeBlock`** (`:742`, `:793`) — refuse bedrock/golden-cube
  (destroy) and bedrock/steel (explosion); remove liquid sources; spawn break/explode
  particles; remove paired door/portal halves; lightboxes subtract their light.
- **`paintBlock(x,z,y,color)`** — recolors, handles lightbox re-light and painting
  both door/portal halves.
- **`burnBlock(x,z,y,causedByExplosion)`** (`:866`) — adds a `BurnNode` to the global
  singly-linked `burnList` if the type `IS_FLAMMABLE`. Life: 6 s normal blocks; 4 s
  TNT (0.5–0.8 s if chained from an explosion). Registers a looping fire sound and a
  fire particle emitter.
- **Fire propagation** (`Terrain::update` `:1954`): 1 s after ignition
  (`BURN_SPREAD_TIME`) a burning block ignites its 6 neighbours. On expiry: TNT →
  `explode` (sphere radius `EXPLOSION_RADIUS=5`; if the TNT was painted, the
  explosion *paints* instead of destroys — that's the paint-bomb feature), firework
  blocks launch, `TYPE_BT*` → `blocktntexplode` (builds a diamond of the base
  material, or ramps for the side variants). More than 300 simultaneous burns trips
  `endDynamics` which clears all fire/liquids/effects — the anti-meltdown valve.
- **`warpToPoint` / `warpToHome`** — save the world with a warp position, then
  unload+reload terrain around the target. Warping is implemented as a save/load
  cycle, not a teleport.

Doors and portals are *stored* as voxels but *rendered and animated* as extracted
`StaticObject`s (see [rendering.md](rendering.md)); portal linking/teleportation is in
`Portal.mm` ([lighting-liquids-effects.md](lighting-liquids-effects.md)).

## Streaming (the "Loading…" hiccup)

`Terrain::prepareAndLoadGeometry` (`Terrain.mm:2117`), every frame while `loaded`:

1. Compute desired window origin `m_chunkOffset* = player.pos/16 - T_RADIUS`.
2. For each of the 18×18 ground-level chunk slots, compare `pbounds` with the desired
   absolute coordinates → count stale slots.
3. If `count > 140` (player crossed roughly a chunk boundary): a two-frame hysteresis
   (`hit_load_counter`) shows the "Loading" status bar, then:
   - `fm->saveWorld()` (flush modified columns **before** they get overwritten!),
   - update `fm->chunkOffsetX/Z` (the render-origin rebase),
   - `updateLightingBegin()` (zero the light array; the *recompute* is deferred, below),
   - **frame-budgeted from here on** (modified from stock, 2026-08-13 — the whole reload used
     to happen inside this one call, a measured 104–131 ms main-thread block on a teleport or a
     Warp Home): `bulk_reload_active` latches, and each frame spends
     `BULK_RELOAD_CHUNK_BUDGET` (96) chunks' worth of `fm->readColumn(cx,cz,file)` —
     **nearest-to-the-player first**, since collision reads `blockarray` directly — from the save
     file if the directory has it, else from the bundled default world / generator. The budget is
     in chunks rather than columns so it stays flat at 256z, where a column is 4× the bytes and
     4× the mesh work. The file handle is reopened per slice, because an autosave in between
     renames a `.savetmp` over it.
   - when every column has landed *and* the meshing they dirtied has drained:
     `addMoreCreaturesIfNeeded()`, `loaded_new_terrain`, and the lighting recompute
     (`update_lighting` → `calculateLightingSlice()` at the tail of the same pass —
     itself frame-budgeted since 2026-08-27: the full `calculateLighting` scan is
     O(window volume) and was a ~20 ms / ~80 ms (64z / 256z) unbudgeted stall here,
     the real 256z reload spike; it now sweeps a strip of columns per frame and
     `update_lighting` stays set until the sweep reports done).
4. Drain dirty lists → `rebuild2()` each chunk → queue VBO uploads. **Since 2026-08-27 the chunks
   dirtied by a bulk reload are meshed on worker threads instead** (`Classes/MeshPool.{h,mm}`) —
   edits, explosions, fire and the initial load still mesh inline in this pass. The rule that
   matters here: a column whose chunks have a mesh job in flight is **skipped by the column read
   above**, because `readColumn()` re-homes the slot a worker is reading. Since the same date the
   column **read** is split too: a column served from the bundled default map has its RLE decode
   done on a worker (`FileManager::readColumnDeferred` → `fmh_decodeColumnBands`), and does not
   count as resident until that decode publishes. See docs/rendering.md "Off-thread meshing". Two things modify this pass
   **while a bulk reload is in flight** (and only then — an edit, an explosion and the initial
   world load still drain in one frame): a column is skipped, flags intact, unless it **and its
   four lateral neighbours** have all streamed in, because `rebuild2()` reads one block across
   each side face and meshing early just builds geometry for data about to be replaced; and the
   same 96-chunk budget caps the pass, reusing the deferral the `list_max` guard already had.
   The neighbourhood test means each chunk is meshed exactly once per reload — 1296 rebuilds and
   107 ms of mesh CPU per burst, against 1949 and 144 ms for a test that asked only about the
   column itself (`tools/headless-mesh-burst-probe.js`, same-session A/B).

   That neighbourhood test was written, reverted and restored: it is the only thing in the engine
   that draws a chunk which has not been meshed since it was allocated, and until 2026-08-27
   `TerrainChunk`'s constructor left the whole `rt*` geometry block uninitialised, so
   `render()`'s `rtn_vertices==0` guard read garbage from a recycled heap and the index `memcpy`
   walked off `allIndices`. See [rendering.md](rendering.md) and
   `WORKING/chunk-streaming-redesign-prompt.md` §2. **Any future deferral that leaves a resident
   chunk unmeshed — a worker-thread mesher, above all — depends on that initialisation.**

The old octree (`TreeNode troot`, `addToTree`, …) is vestigial: `renderTree()` says it
plainly — "once upon a time this descended an oct-tree, profiling showed it was
useless, now just iterates through chunk list" (`Terrain.mm:2430`).

## Lifecycle
- `Terrain()` constructed at app start; `allocateMemory()` only when entering a world;
  `deallocateMemory()` on exit to menu (frees blockarray/lightarray/chunk objects).
- `loadTerrain(name, fromArchive)` → `FileManager::loadWorld` does the real work.
- `unloadTerrain(exitToMenu)` only clears portals/fireworks and resets the mesh cache
  (`troot`) when `exitToMenu==TRUE` — i.e. when actually leaving the world, not on the
  internal `unloadTerrain(FALSE)` calls (`warpToHome` among them) that keep chunk
  objects around for reuse. It used to clear the portal registry unconditionally, which
  silently broke both portal teleportation (`Portal::enterPortal`) and portal proximity
  ambience ([resources-and-audio.md](resources-and-audio.md)) for any portal whose
  chunk wasn't freshly remeshed afterward — `Portal::addPortal` only runs from a mesh
  rebuild (`TerrainChunk.mm`), which most portals never get again once meshed unless
  their chunk is dirtied or streams back in fresh. Fixed by gating the registry wipe on
  the same `exitToMenu` flag as the mesh-cache reset right next to it.

## Common pitfalls
- **Argument order is `(x, z, y)` with y vertical** in nearly every terrain API, but
  storage order and some structs differ. Triple-check every call site you write.
- `getLandc` returns garbage (wrapped data) for coordinates outside the resident
  window rather than failing — callers guard with `y` checks only.
- `Terrain::getLand` returns `-1` for out-of-height; `getColorc` returns 0. Several
  callers rely on `-1` being distinct from `TYPE_NONE==0`.
- Editing blocks without `updateChunks` leaves stale meshes; editing during the
  streaming save window can be lost.
- `blockarray` holds **types only**. Any new per-voxel attribute needs either the
  chunk arrays (persisted) or a parallel toroidal array (transient, like light).
- The `TYPE_CUSTOM` / `SmallBlock` half-resolution sub-block system is dead code
  (commented out everywhere) — don't resurrect it casually; the save format never
  supported it.

## Safe vs. risky to modify
- **Safe:** block property tables in `Globals.mm`/`Constants.h` (append, don't renumber
  — saved worlds store raw type bytes), explosion radii, burn times, new gameplay ops
  built from `updateChunks`/`setColor`.
- **Caution:** anything touching `GBLOCKIDX`/`threeToOne` indexing, `T_SIZE`/`T_HEIGHT`
  (changes save format column size! `SIZEOF_COLUMN` depends on them), the streaming
  threshold logic, chunk reuse (`resetForReuse` assumes the slot is being overwritten).
