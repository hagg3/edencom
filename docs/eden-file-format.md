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
0..127; 112–127 are the 2026 game's NewFormat256z blocks, native since Stage D.2a), colors unsigned bytes (palette indices, 0 = unpainted).

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
creature block; the loader fills slots with `type=-1`. **Exception (S.3b, 2026-10-09):** the 2026
game writes version-2 256z worlds that *do* carry a 400-slot block, so since then the engine decides
"has a creature block" from the file (`deriveColumnSpans`: version ≥ 3, or a non-zero gap that is a
whole number of slots), not from `version < 3`.

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
i32 a, i32 b, i32 c           — face, colour, facing  │ one 120-byte record, i.e. 10 tag rows
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
as before — those are corruption, not a trailer. Since Stage D / D.3a the game also parses it (the
model below), but an untouched trailer is still written back as the exact bytes read — and moving
columns can never invalidate it, since its records hold world block coordinates and never file
offsets. Regression cover: `web/tools/headless-save-trailer-test.js`, using the
literal 192 bytes from `quarry.eden`. Full provenance:
`WORKING/newformat256z-sign-trailer-2026-08-24.md`.

The engine draws signs (D.3b, `Classes/SignRenderer.mm`, docs/rendering.md) and places, edits and removes
them (D.3c, `Classes/SignTool.mm`, docs/ui.md); the model that stores them is D.3a, below. **What Emod
writes** for a new sign: `a` = the face tapped (never 2 — the bottom is refused), `b` = the build paint
(`hud->block_paintcolor`) or **45** (charcoal) with none — the 2026 game writes 2 — and `c` = the face's own
direction for a wall sign (0/2/1/3 for a = 0/1/4/5; the game writes the placer's quarter there, and nothing
reads it), the quarter facing the player for a standing one. For a new **command block** (D.4b, the CMD tool,
`CmdTool` in `Classes/SignTool.mm`): an unpainted steel (74) voxel plus a `CMB1` record with `flags` 0 and an
empty script; editing rewrites the script only (≤ 511 bytes, printable ASCII; empty is allowed and keeps the
block); Emod refuses a 513th command block per world. Any live edit that turns a block into air drops
every sign and the `CMB1` record anchored on it (`Terrain::updateChunks`, docs/world-and-terrain.md).
**Command-block scripts** (D.4c, `Classes/CmdScript.{h,mm}`) run the language of
<https://db.edengame.net/cmd.html>. Two facts about the stored data were measured on the adder world, not
taken from that page: a script's `X Y Z` maps to the record's `(x + X, y + Z, z + Y)` — file order, the
middle number is height (`run -4 0 -4` from ONE reaches the "reset (units)" block 4 across in x and 4 in
file y), and a script's COLOR is the **stored colour byte** 0..54, not cmd.html's "0–53": the adder writes
`lamp 54` and its saved display lamps hold 54. The interpreter keeps no state in the file (a running
script is runtime-only), so running a world's command blocks never changes its trailer, except that a
script's `set … air` on a command block removes that record, as mining does. The 2026 game also keeps a local world's records in sidecar files `signs_<world>.eden.dat` /
`cmd_<world>.eden.dat` — exactly one *bare* section each (`MAGIC | u32 version | u32 count` + records, no
outer wrapper), same coordinates — which a single exported `.eden` does not carry. The engine adopts them
(below).

**More than signs (measured 2026-10-10, `TESTERS/3 bit adder 1791611312.eden`).** The trailer is a *list of
sections*. Each section has an outer wrapper `MAGIC | u32 inner_len | u32 0`, then
`MAGIC | u32 version (1) | u32 count`, then fixed-size records:

- `SGN1`: the 120-byte sign records above.
- **`CMB1`: 528-byte command-block records**, laid out as `i32 x, i32 y, i32 z, i32 flags (0 in all 99 seen),
  char script[512]`. Every record sits on a steel (74) voxel: a command block is a steel block plus a record.

In that world, `CMB1` comes first, then `SGN1`, with no padding. The sign ints are no longer unknown. A decode of
225 in-region signs against their anchors' air neighbours gives:

- **a** is the anchor face: 0 = −x, 1 = +x, 3 = top, 4 = −y, 5 = +y. 2 = bottom is never seen.
- **c** is the direction the text faces, as a quarter-turn: 0 −x, 1 −y, 2 +x, 3 +y. It is fixed by the face on wall
  signs and free on top signs.
- **b** is the board's colour as a `colorTable` index, 1..54, i.e. paint swatch + 1 (`Hud::genColorTable`). The greys are
  9/18/27/36/45/54. The 2026 game writes **2** (light orange) when no paint is selected.

The anchor `(x, y, z)` is the block the sign hangs on; `z` is the height. Scripts: `TESTERS/d-plan/`.

### The trailer model (Stage D / D.3a, 2026-10-10)

`Classes/WorldTrailer.{h,cpp}` (engine) and `web/tools/emod.py trailer` (the reference; no shared code)
read the trailer by these rules, on both containers (an `.emod`'s `SIGN_TRAILER` record holds the same
bytes):

1. Empty → no trailer. Otherwise every 16-byte row must start `ff ff ff ff`; the payload is the 12 bytes
   after each tag.
2. Walk the payload: a 4-byte zero magic (or fewer than 12 bytes left) ends the sections; what remains is
   the **tail**, which must be all zero (kept verbatim). Each section is `MAGIC | u32 inner_len | u32 w3`
   + `inner_len` bytes. `SGN1` / `CMB1` must carry `MAGIC | u32 version | u32 count` and exactly
   `count × 120` / `count × 528` body bytes, and appear at most once each. Any other magic is kept
   verbatim, wrapper included, in its slot. `w3` and `version` are kept as read.
3. Anything else (a row without the tag, a known section whose sizes disagree, a second `SGN1`, a
   non-zero tail) makes the trailer **opaque**: written back byte for byte, every edit refused.
4. **Untouched = the original bytes exactly** (not a re-serialisation). Once edited, the trailer is
   rebuilt: sections in their original order and slots, a section an edit emptied is left out (an
   untouched empty one stays), then the tail, zero-padded to a whole row. A brand-new section goes
   `CMB1` before `SGN1` (the 2026 game's order). No section left → no trailer at all (a `.emod` gets an
   empty `SIGN_TRAILER`, which removes it).
5. **Cap: the encoded trailer ≤ 1 MiB** (= `DIR_TRAILER_MAX` = the `.emod` limit). With no command
   blocks that is **6,553 signs** (`(786,432 − 24) / 120`); the next sign or command block is refused
   with a toast, never truncated.
6. Anchor index: signs and command blocks are hashed by anchor block, so "is anything anchored on this
   block" is O(1) (D.3c's removal hook). One sign per (anchor, face); one command block per block.
7. Coordinates: file `(x, y, z)` = `Terrain` argument order `(x, z, y)` = engine `Vector (x, y_up=z, z=y)`;
   `TrailerPos::fromEngine` / `toEngine` are the one conversion.
8. Sidecars are adopted only for a magic the trailer has no section of (the trailer wins); the first save
   writes them in and renames the sidecar `*.adopted`.

Measured (2026-10-10): adder 80,480 B = `CMB1` 99 + `SGN1` 67; alpinecraft 40,032 B = `SGN1` 250; Quarry
192 B = `SGN1` 1 ("test", a = 4, b = 9, c = 1); all three parse and re-serialise byte-identical in both
tools. The 2026 game's `signlab` sidecars (14 signs, 4 command blocks) adopt into a world with no trailer.

**Every reader of the directory must apply the coordinate gate (fixed S.5b, 2026-10-10).** Trailer rows
look like directory rows to a reader that does not: `FileManager::convertWorldTo64` and
`web/tools/eden-convert.js` used to count them as columns, so alpinecraft and Quarry were refused ("1897
of 3227 columns are not 131072 B"). Both now keep only addressable rows (`0 ≤ x, z < 32768`, key ≠ 0).
Neither carries the trailer into a 64z file. The exporter (`Classes/EdenWorldExport.cpp`, save-load.md
§ Export) parses it into sections only to count, prune or drop records: a record's column is
`(floor(x/16), floor(y/16))` — `x, y` horizontal, as above.

## RLE variant (bundled default world only)

> **S.6 (2026-10-10): the game no longer reads this variant by default.** The app bundle ships
> `Eden.emod` (an ordinary `.emod`, 4 bands, zstd-19) baked from `Eden.eden` by `emod.py
> bake-default`; the RLE reader in `FileManagerHelper.mm` survives only as the fallback for a bundle
> with no `Eden.emod` and as the gate's reference. `Eden.eden` stays in the repo as the source.

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

Files are silently upgraded to v4 on the first save — **64z files only** since S.3b: a 256z world
keeps its version verbatim (5, 6, or the 2 the 2026 game writes).

**The version is not a height marker** (S.3b, measured 2026-10-09 on `TESTERS/alpinecraft spawn
1790748193.eden`): the 2026 game stamps `version 2` on some 256z worlds (131,072 B stride, a
24,000 B creature gap, content to band 6) and 5 on others (Quarry). The height is decided from the
file — see *Height detection* below.

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
| header `version` | ≤ 4 | 5 or 6 — **or ≤ 4** (the 2026 game writes v2 256z worlds; S.3b) |
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
- The block-ID space is 0–111 for every pre-2026 file; NewFormat256z worlds add 112–127, which
  the engine draws and keeps since Stage D.2a (Legacy64z/NewDawn exports still map them to stone).
  **No block-ID conversion applies** — the `convertType`/`convertColor` tables are for the *other* legacy path,
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

### Height detection (S.3b, 2026-10-09)

One rule, the sibling editor's (VuencEdit `detect_chunk_size_by_creature_gap`), in one function,
`emod::eden_detect_bands` (`Classes/EdenWorldStore.cpp`), restated in `web/tools/emod.py`
(`detect_bands`) and `web/tools/eden-convert.js` (`detectBands`):
1. `version` 5 or 6 → 256z.
2. Otherwise, over the **live** directory rows (twoToOne-addressable, last row wins), try the stride
   131,072 then 32,768: the one for which `directory_offset − (highest offset + stride)` is 0 or a
   whole number of 60-byte slots ≤ 24,000 wins. The two candidates' gaps differ by 98,304 B, so at
   most one can pass. This is what decides a **single-column** world, which has no offset gap.
3. Neither passes: the smallest gap between distinct live offsets ≥ 131,072 → 256z; fewer than two
   columns → 64z.

Callers: `FileManager::probeWorldHeight` (reads `directory_offset`..EOF before any allocation),
`FileManager::convertWorldTo64`, and the `.emod` converter. Mutation-checked: putting
`version >= 5` back fails 9 checks of `--emod-selftest`.

### What this engine does with one (Stage 2, 2026-08-06)

| Step | Where |
|---|---|
| Decide the height before any allocation: 5/6 → 256, else the creature-gap detection above (S.3b) | `FileManager::probeWorldHeight`, called from `World::loadWorld` |
| Size `blockarray`/`lightarray`/chunk table for that height | `Terrain::allocateMemory` |
| Derive creature-slot count and per-column spans from the directory | `FileManager::deriveColumnSpans` (see [save-load.md](save-load.md)) |
| Read 16 bands per column, zero-filling any the file is too short to hold | `FileManager::readColumn` |
| Seed unlisted columns from the bundled 64z `Eden.eden`, air above its 4 bands | `fmh_readColumnFromDefault` |
| Write 16 bands per column, keep the file's own version (including a 256z v2) | `saveColumn` / `saveWorld` |

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

## Compressibility and band occupancy (measured 2026-10-03)

The `.emod` Phase 0 study measured real worlds against this format with
`web/tools/eden-format-measure.py`. The tool is read-only, and its `--corrupt-gate` proves the
round-trip check can fail. Raw results: `WORKING/emod-format-phase0-f1-results-2026-10-03.md`. Plan and
recommendation: `WORKING/emod-format-phase0-f2-plan-2026-10-03.md`. The facts that outlive the study:

- **A column is ~99% redundant whatever its height.** Per-column zstd level 3 averages 460–494 B per
  column on three real worlds: a 64z flat city, a 256z New Dawn world and a 256z NewFormat world. That
  is 1.4% of a 32 KB record and 0.4% of a 128 KB one. zstd-19 averages 306–339 B, zlib-6 423–555 B,
  lz4 1,034–1,686 B. The largest specimen (3.79 GB, 30,299 columns) totals 14.3 MB under zstd-3.
  Whole-file xz reaches 0.2–0.6% of file size, which is the "~90% smaller" players see when they
  zip a world, and it is a lower bound. Per-column compression costs only ~1.6–2.2× whole-file
  compression and keeps random access.
- **256z worlds are mostly 64z content in a 256z container.** Bands 4–15 are all-air in **100%** of
  Diane's columns and **99.6%** of Quarry's; the outliers reach bands 5–11. Across all bands, 80–86%
  of a 256z world's bands are all-air, against 17.9% for the 64z city. One consequence:
  `eden-convert.js --to-64` is lossless for most real 256z worlds, but check its destroyed-block
  count, never assume it.
- **"All-air" must test all 8,192 bytes of a band, not just the 4,096 type bytes.** Real files
  contain bands whose types are all `TYPE_NONE` but whose paint plane is non-zero (16 in the city,
  28 in Quarry). Anything that elides or skips "empty" bands on disk by checking types alone will
  drop paint.
- **Eliding all-air bands is a runtime lever, not a size lever.** Elision alone leaves 14–82% of raw,
  and elision on top of zstd-3 saves less than 1%, because zstd already collapses zero runs. What
  skipping empty bands saves is reading, decoding, allocating, meshing and lighting them.
- **Pristine default-map columns are common.** In a seed-333333 world, 97.4% of columns were
  byte-identical to the bundled map: bands 0–3 equal, 4–15 air. That world was nearly untouched,
  so treat the figure as a best case.

The container those numbers led to is specified in [emod-file-format.md](emod-file-format.md)
(S.1, 2026-10-09). `web/tools/emod.py` converts any `.eden` to it and back, byte-identically for an
engine-written world.

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
