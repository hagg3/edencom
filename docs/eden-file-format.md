# The `.eden` World File Format

Authoritative reference, reconstructed from `Classes/FileManager.h/.mm`,
`FileManagerHelper.mm`, and cross-checked against the two documents in the repo root:
- `Eden_file_format.txt` — the developer's own 24-line summary.
- `MROB.txt` — Robert Munafo's pre-source-release reverse engineering (matches the
  code exactly for v2-era files; his "12×12 columns" observation is the 1.7-era
  window size, 18×18 in this version).

The sister project `eden-world-editor` (a separate Tauri/Rust/React `.eden`
editor, independent of this codebase) reimplements this same on-disk format from
scratch and has hit real parsing bugs on large user-supplied worlds; its
postmortem surfaced two hazards worth knowing about even though neither is a live
bug in this codebase's own reader/writer — see the "Practical notes for tool
authors" section below.

## Layout

```
┌────────────────────────────────────────────┐ offset 0
│ WorldFileHeader              (192 bytes)   │
├────────────────────────────────────────────┤ offset 192
│ BLOCK DATA: column records, append-only    │
│   each column = 32768 bytes (uncompressed) │
├────────────────────────────────────────────┤ directory_offset − 200·sizeof(EntityData)
│ CREATURES: 200 × EntityData  (fixed size)  │   (only if version ≥ 3)
├────────────────────────────────────────────┤ directory_offset
│ DIRECTORY: ColumnIndex × N until EOF       │
└────────────────────────────────────────────┘
```

Design rationale (from `Eden_file_format.txt`): newly touched columns are **appended**
— they overwrite the creatures+directory region, which is then rewritten after the
new data, and only `directory_offset` in the header changes. Block data never shifts.

## Header — `WorldFileHeader` (`FileManager.h:20-35`)

```c
typedef struct {
    int level_seed;                  // terrain gen seed; 0=flat, 333333=default world
    Vector pos;                      // player position (3 floats, world units)
    Vector home;                     // home/spawn block coordinates
    float yaw;                       // player yaw, degrees
    unsigned long long directory_offset;
    char name[50];                   // display name, NUL-terminated
    // ---- added after 1.1.1 ----
    int version;                     // FILE_VERSION == 4 in this build
    char hash[36];                   // MD5 (hex string) of the preview screenshot
    unsigned char skycolors[16];     // 4×4 region sky-color palette indices
    int goldencubes;                 // remaining golden-cube inventory
    char reserved[...];              // pads struct to exactly 192 bytes
} WorldFileHeader;
```

The comment in the header is emphatic: **192 bytes including padding is load-bearing**
("be careful modifying this to not corrupt old maps"). `saveColumn` even asserts
`(chunk_offset−192) % SIZEOF_COLUMN == 0`. The `hash` links a world file to its
uploaded `.png` preview for the sharing service (see [networking.md](networking.md)).

Note: the struct is written/read by raw `memcpy`/`fwrite` — the format is
**little-endian, ARM/x86 struct layout** (4-byte alignment for the first section, the
`unsigned long long` lands at offset 32). MROB.txt's dump confirms this layout
empirically (directory pointer observed at bytes 0x20–0x27).

**Every byte of the header is now zeroed before it is filled** (Phase N Stage 3,
2026-09-06). It was not before: `saveWorld` and `writeGenToDisk` `malloc` the 192 bytes and
assign the eleven fields they use, and `-getCString:maxLength:` NUL-*terminates* rather than
pads — so the tail of `name[50]`, the tail of `hash[36]`, the whole of `reserved[]` and the
struct's own padding went to disk as whatever `malloc` last returned. Three consequences,
which is why this is recorded here rather than as a tidy-up:

- two identical saves were never byte-identical, on one machine let alone across platforms —
  which is what defeated Stage 3's cross-platform byte comparison and is how this was found;
- a world file carries a few dozen bytes of the writing process's heap, and the sharing
  service **uploads world files** ([networking.md](networking.md));
- `reserved[]` has not actually been zero in any file written since 2.1.1, so the note at
  line 208 below ("a real v5 file, whose `reserved[]` was entirely zero") described a file
  written by the *original* app, not by this fork. **A future format version growing into
  `reserved[]` must still treat a non-zero value there as "old file, field absent"** — files
  written between 2.1.1 and 2026-09-06 have garbage in it.

Nothing reads those bytes, so zeroing them changes how no existing world loads.

## Block data — column records

One record per 16×16 **column** = `CHUNKS_PER_COLUMN` (4) chunks stacked bottom-up:

```
for cy in 0..3:
    block8 types [4096]   // CC order: x*256 + z*16 + y  (y fastest — vertical strips)
    color8 colors[4096]
```

`SIZEOF_COLUMN = 16·16·16·4·2 = 32768` bytes. Types are signed bytes (block ids
0..111), colors unsigned bytes (palette indices, 0 = unpainted).

Special cases in the type byte you must preserve when writing tools:
- Liquids encode fill level in the type (`TYPE_WATER`=full, `WATER3/2/1` descending;
  same for lava).
- Ramps/side-pieces encode orientation in groups of 4 consecutive ids.
- Doors/portals: bottom block `TYPE_DOOR1..4`/`TYPE_PORTAL1..4` encodes facing;
  top block is `TYPE_DOOR_TOP`/`TYPE_PORTAL_TOP`.

## Creatures (version ≥ 3)

`MAX_CREATURES_SAVED = 200` fixed slots of `EntityData` (`Vector.h:38-49`):

```c
typedef struct {
    Vector pos, vel;       // 24 bytes
    float angle;
    int   type;            // creature id 0..6, or -1 = empty slot
    int   color;
    float touched, extra2, extra3;
    Vector extra4;
} EntityData;              // 60 bytes → block = 12,000 bytes
```

Located at `directory_offset − 200*sizeof(EntityData)`. Files with version < 3 have no
creature block; the loader fills slots with `type=-1`.

## Directory

Read from `directory_offset` to EOF (`FileManager::readDirectory` — there is **no
count field**; EOF terminates):

```c
typedef struct {
    int x, z;                        // absolute chunk-column coordinates
    unsigned long long chunk_offset; // absolute file offset of the column record
} ColumnIndex;                       // 16 bytes with padding
```

In memory the directory is a hashmap keyed by `twoToOne(x,z) = (x<<15)|z` — hence the
hard limit of 15-bit chunk coordinates. Key 0 is treated as invalid/corrupt and
skipped.

### The post-directory sign trailer (`NewFormat256z` worlds)

A 2026-08 update to the closed-source game writes in-game **signs** into the directory region
itself rather than into a new section: after the real `ColumnIndex` rows and before EOF it appends
rows whose `x` field is `0xffffffff`, which fails `twoToOne`'s range check and so is skipped by the
reader above. That "skip" is exactly why the game can get away with it — and why any *writer* that
rebuilds the directory from its in-memory hashmap silently destroys every sign in the world.

Stripped of the `ff ff ff ff` tag that prefixes every 16-byte row, the payload stream is:

```
"SGN1" | u32 payload_len      — wrapper row (payload_len = 12 × following row count)
"SGN1" | u32 version | u32 count
i32 x, i32 y, i32 z           — sign world position   ┐
i32 a, i32 b, i32 c           — unknown               │ one 120-byte record, i.e. 10 tag rows
char text[96]                 — NUL-padded ASCII      ┘
… more records, then zero-padding to fill out the last row
```

Measured on `TESTERS/quarry-NewFormat256z.zip` (3.97 GB, `version` 5): 24,167 directory rows, of
which the last 12 — and only the last 12, contiguously — fail the coordinate gate. The trailer
appears only once a world actually has signs or the new block types (112–127) on it; an otherwise
identical world from the same game build has none.

**This engine now round-trips it.** `readDirectory` captures the contiguous run of gate-failing
rows *at the end* of the directory verbatim (capped at 1 MiB, a multiple of 16 so it can never
split a row) and `fwriteDirectory` re-emits it immediately after the real entries, then truncates
the file to that point. Rows that fail the gate *interior* to the real entries are still dropped,
as before — those are corruption, not a trailer. Nothing here parses a sign: the trailer is an
opaque blob, which is all round-tripping needs, since sign records hold world block coordinates
and never file offsets. Regression cover: `web/tools/headless-save-trailer-test.js`, using the
literal 192 bytes from `quarry.eden`. Full provenance:
`WORKING/newformat256z-sign-trailer-2026-08-24.md`.

Writing signs is **not** supported and is not planned here; the same 120-byte record layout also
appears in a true sidecar file (`signs_<world>.eden.dat`) that this engine does not read.

## RLE variant (bundled default world only)

The shipped `Eden.eden` (repo root / app bundle) uses the same header/directory but
**compressed column records**, written by `FileManager::saveGenColumn` and read only by
`FileManagerHelper::fmh_readColumnFromDefault`:

```
per chunk (4 per column):
    uint16_be length              // total bytes including these 2
    repeat: { int8 type, uint8 color, uint8 count (1..127) }
```

Additionally the voxel order inside RLE chunks is **transposed** to `CC(y,z,x)`
(x fastest) "to maximize compression" (horizontal runs compress better), and the
reader un-transposes: `pblocks[CC(x,z,y)] = tblocks[CC(y,z,x)]`
(`FileManagerHelper.mm:203-208`). User save files are always raw (the `rle` flag in
`FileManager::readColumn` is hardcoded `false`).

## Version history (as handled by `FileManager::loadWorld`)

| version | Era | Differences |
|---|---|---|
| garbage (<1 or >1000) | 1.x | Legacy format: 1 byte/block, **no colors**, 30 block types. Converted in place by `convertFile` using `convertType`/`convertColor` tables (old colored blocks become type+paint). |
| 2 | early 2.0 | Current layout, no creature block (`chunk_offset` points relative to a file without it). |
| 3 | 2.x | Adds the creatures block before the directory. |
| 4 | 2.1 (current, `FILE_VERSION`) | Adds `goldencubes` + `skycolors[16]` to the header (upgraded in-place on load: v3 files get 10 cubes and all-blue sky). |

Files are silently upgraded to v4 on the first save.

## The 256z ("New Dawn") variant — version ≥ 5

The closed-source successor ships a **256-block-tall** variant of this same format. **This
engine reads and plays it natively as of 2026-08-06** (backport plan Stage 2): the world height
is a per-world runtime value, chosen from this header's `version` before the terrain arrays are
allocated — see "Runtime world height" in [world-and-terrain.md](world-and-terrain.md).
A version **above 6** is still refused outright via `eden_report_load_failure()`, because a
format nobody here has seen would otherwise be read at a stride we guessed and then overwritten
by the first autosave. What follows is the byte-level ground truth, most of it **measured
first-hand** against a real 4,805,686,272-byte version-5 world, and since 2026-08-06 also
exercised against two smaller v5 specimens written by the sibling world editor
(`~/eden-world-editor`) and loaded by this engine.

| | 64z (this engine) | 256z |
|---|---|---|
| header `version` | ≤ 4 | 5 or 6 |
| chunk-bands per column | 4 | **16** |
| `SIZEOF_COLUMN` | 32,768 | **131,072** |
| creature slots | 200 (12,000 B) | **400 (24,000 B)** |
| world Z ceiling | 63 | 255 |

Everything else is **identical** and needs no conversion:
- The 192-byte header layout is byte-for-byte the same (`name` @40, `version` @92, `hash`
  @96, `skycolors[16]` @132, `goldencubes` @148, `reserved` @152 — all verified against a
  real v5 file, whose `reserved[]` was entirely zero).
- Intra-chunk addressing: band *b* at `+b*8192`, types at `+CC(x,z,y)`, paint at `+4096` of
  the same. The mesher and every chunk-local code path are height-agnostic already.
- The block-ID space is unchanged (0–111, through `TYPE_BTSTEEL`). **No block-ID conversion
  applies** — the `convertType`/`convertColor` tables are for the *other* legacy path,
  `version` outside 1..1000.
- `ColumnIndex` is still 16 B `{i32 x, i32 z, u64 offset}`. The specimen's chunk coords
  (x 4026–4251, z 3942–4182) still fit the 15-bit `twoToOne` key, but 3,891 of its offsets
  are ≥ 4 GiB — see the u64 warning below.

Confidence: the 400-slot creature block is measured from **one** specimen (the gap between
the last column's end and `directory_offset` is exactly 24,000 B, with `type == -1`
sentinels at a 60-byte stride aligned to it) and every slot in it was empty, so the 60-byte
`EntityData` *layout* inside a 256z file is inferred from the sentinel stride, not from real
entity data. **Derive the creature-block size from the file** —
`directory_offset − (max chunk_offset + column_size)` — rather than trusting the version;
that makes the assumption self-checking, and it is also what copes with third-party writers
that emit no creature block at all.

Still unknown: what distinguishes `version` 6 from 5 (this engine treats 6 as 256z and
**preserves** it on write, never normalising to 5); whether a 256z bundled template uses the
same per-band RLE framing with 16 bands; whether anything is ever written into `reserved[]`.

### What this engine does with one (Stage 2, 2026-08-06)

| Step | Where |
|---|---|
| Read `version` from the header before any allocation; 5/6 → height 256 | `FileManager::probeWorldHeight`, called from `World::loadWorld` |
| Size `blockarray`/`lightarray`/chunk table for that height | `Terrain::allocateMemory` |
| Derive creature-slot count and per-column spans from the directory | `FileManager::deriveColumnSpans` (see [save-load.md](save-load.md)) |
| Read 16 bands per column, zero-filling any the file is too short to hold | `FileManager::readColumn` |
| Seed unlisted columns from the bundled 64z `Eden.eden`, air above its 4 bands | `fmh_readColumnFromDefault` |
| Write 16 bands per column, keep the file's own version | `saveColumn` / `saveWorld` |

The bundled `Eden.eden` is deliberately **not** regenerated at 256z: the offline `TerrainGen2`
bake would need ~4 GB and its formulas mix proportional with absolute offsets, so a naive
stretch produces a differently-shaped world that would need an art pass. A 256z world seeded
from the default map therefore gets its 4 real bands and air above them.

Creating a *new* 256z world in-game (Stage 3 item 4) shipped 2026-08-26: the web port's New
World screen has a height choice that is 64z by default, and the choice flows into
`FileManager::probeWorldHeight` for the not-yet-existing file (see web/docs/eden-file-format.md
for the seam-level wiring). Landing this surfaced a latent bug in `saveWorld()`'s brand-new-file
path: the very first save of a new file relied on `sfh->version<3` to know whether
`directory_offset` still needed its one-time bump past the creature block, which was always true
for a 64z world (new worlds always started at version 2) but is false for one stamped straight to
version 5 — that world's first save left `directory_offset` pointing AT the creature block instead
of past it, and the next read misparsed the creature block as ~1500 garbage directory rows. Fixed
in `saveWorld()`'s `!existed` branch, which now seeds `directory_offset` past the creature block
up front for a 256z-stamped new file instead of relying on the version<3 bump.

### Converting between the two

`web/tools/eden-convert.js` converts both directions offline (pure byte surgery, no engine):

```
node tools/eden-convert.js --info   <in.eden>          # parse + report, change nothing
node tools/eden-convert.js --to-256 <in.eden> [-o out] # 64z -> 256z: 12 air bands per column
node tools/eden-convert.js --to-64  <in.eden> [-o out] # 256z -> 64z: DESTRUCTIVE, confirms first
```

`--to-256` is lossless and `64z → 256z → 64z` is byte-identical to the original (which is
what `tools/eden-convert-test.js` asserts, on synthesised fixtures and verified by hand on a
real save). `--to-64` reports the exact number of non-air blocks it would destroy before
asking, clears door/portal bottoms whose `*_TOP` half was cut off at y=63, clamps `pos.y`/
`home.y` into `[0,63]` (a tall world's player legitimately stands above the 64z ceiling), and
drops creatures above the ceiling before compacting 400 slots into 200. It refuses the
RLE-compressed bundled `Eden.eden` and 1.x legacy files rather than mangling them.

The web port also has an **in-app** "Convert to 64z" action (Settings → Storage tab), backed by
`FileManager::convertWorldTo64` — a from-scratch C++ restatement of this same `--to-64` algorithm
over `NSFileHandle`, since a browser has no Node to shell out to `eden-convert.js`. The two are
hand-kept in sync, not shared code; see web/docs/eden-file-format.md for what that implies.

Both directions are verified against **real** v5 files as of 2026-08-06 (the earlier session had
only synthesised fixtures): `~/eden-specimens/v5-flat-8x8.eden` and `v5-natural-18x18.eden`,
generated by the sibling editor's own `write_world_file` — which, usefully, emits **no creature
block**, the adversarial case for the derived-size logic above. Regenerate them with a temporary
`#[cfg(test)]` in that project's `src-tauri/src/worldgen.rs` calling `generate_flat_chunk` /
`generate_natural_world` + `write_world_file`; nothing in this repo can produce one.

## Auxiliary files
- `<world>.png` in Documents — preview screenshot (taken by the HUD camera mode),
  uploaded alongside the world when sharing; MD5 stored in the header.
- `<world>.savetmp` — the scratch copy an ordinary (below-threshold) save is built in, swapped in
  by one rename. `<world>.savetmp.bak` / `<world>.bak` — whole-file backup slots the web port's
  `NSFileHandle` shim keeps for the load-failure dialog. None of these are `.eden` files and none
  are listed as worlds; all three are absent above the in-place-save threshold.
- `<world>.savejrnl` — the rollback journal a large in-place save writes before it starts, and
  deletes on commit. Its own small header (`EDNJRNL`, version 2) plus the world's previous 192-byte
  header and the file's pre-save tail, then zero or more `EDNJCOL` records — a 24-byte
  `{magic, offset, length}` followed by `length` bytes — holding the pre-save contents of each
  column the save overwrites in place. If you find one on disk the last save was interrupted; the
  engine replays it on next load. See [save-load.md](save-load.md).
- `FileArchive.h` (compress-on-exit via zlib/`zpipe`) is **entirely commented out** —
  `compressLastPlayed()` is a no-op. Worlds on disk are uncompressed.

## Practical notes for tool authors
- To enumerate worlds: list the Documents directory; any file whose first 192 bytes
  parse as a header with a non-empty `name` is a world (`Menu::loadWorlds` +
  `FileManager::getName` do exactly this, `error~` sentinel for failures).
- To read a column: header → seek `directory_offset` → scan `ColumnIndex` records →
  hash/scan for (x,z) → seek `chunk_offset` → read 4×(4096+4096).
- Columns never in the directory: untouched default-world terrain (fetch from the
  bundled `Eden.eden` by the same algorithm) or ungenerated flat terrain.
- When writing: append the column at `directory_offset − 12000`, bump
  `directory_offset` by 32768, rewrite creatures + directory + header.
- **When rewriting the directory, re-emit the sign trailer** (above) if the file had one, or you
  will silently delete every sign the game put in that world.
- **Decode `chunk_offset` as the full 64-bit field, never as its low 32 bits.**
  `FileManager` itself is safe here — it's a raw C struct read via `memcpy`/`fread`
  into a real `unsigned long long`, and column data for one world (bounded by
  `T_SIZE`/window streaming) never approaches 4 GiB. But this has bitten an
  *external* reimplementation of this exact format: the sister editor project
  `eden-world-editor` (Rust/Tauri, parses `.eden` files independently) shipped a
  directory-entry decoder that read only bytes `[8..12]` of the 16-byte
  `ColumnIndex` as a `u32`, silently discarding the high word at `[12..16]`. It
  went unnoticed until a >4 GiB world (a different, later 256×256×256 "256z"
  variant of the format than this codebase targets, with 128 KB columns instead
  of 32 KB) produced chunks whose true offset carried a nonzero high word — every
  such chunk resolved to `true_offset − 2³²`, landing inside an unrelated,
  misaligned chunk and rendering as a "mosaic" of plausible-but-wrong blocks
  (type-plane reads landing on paint-plane bytes and vice versa). The fix was to
  decode `x`/`z` as `i32` and `chunk_offset` as `u64` in one pass, exactly per the
  layout above — any tool that instead reads the offset as `u32`/`i32`, or trusts
  a hand reverse-engineered doc based on a small test world (both `MROB.txt` and
  the historical `EdenWorldManipulator2.0` C# tool made this exact mistake, since
  every offset in a small world has a zero high word and the bug is invisible),
  will corrupt large worlds without any error.
- **Don't assume a fixed per-column stride when scanning/repairing a directory.**
  This codebase's own writer always appends at a clean `+32768` stride, so this
  isn't a live bug here — but `eden-world-editor`'s postmortem on the same bug
  found that real large-world files can contain a directory entry whose gap to
  the *next* entry is smaller than the column size (one observed case: 107,072 B
  instead of 131,072 B — exactly one 24,000-byte **400-slot creature block** short,
  i.e. what this engine's own `saveColumn` offset formula produces when
  `MAX_CREATURES_SAVED` is wrong for the file; strong hypothesis, not proven). A
  robust external reader should derive each column's readable span from
  `next_offset − offset` (clamped to the record size), not assume every entry
  owns the full record size unconditionally — and should zero-pad the shortfall
  rather than read on into the neighbour. `web/tools/eden-convert.js` does both,
  and repairs the anomaly by always *writing* full-size records.

## Uncertainties
- `EntityData.touched/extra2/extra3/extra4` semantics are only partially clear
  (`touched` is a timer in creature AI; the extras appear unused — confidence: medium.
  Verify in `Model.mm` `SaveModels`/`LoadModels2` before repurposing).
- Exact byte offsets inside the 192-byte header past `name` depend on compiler padding;
  they were stable across the shipped armv7 builds but **verify with a hex dump before
  hard-coding offsets in an external tool** (the repo's `Eden.eden` is a ready-made
  reference specimen).
