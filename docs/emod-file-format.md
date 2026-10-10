# The `.emod` World Container (format version 1)

**Normative.** This file is the specification. The reference implementation is
[`web/tools/emod.py`](../web/tools/emod.py). If the two disagree, this file wins and the tool has
a bug. Status: specified and tool-verified (S.1, 2026-10-09); **implemented in the engine as
`Classes/EdenWorldStore.{h,cpp}`** (S.3, 2026-10-09: the store and the forward-only converter,
byte-identical to `emod.py` on every target tested), and **played by the engine since S.4**
(2026-10-09: FileManager loads, streams, saves, renames and deletes `.emod` worlds; new worlds are
`.emod` only behind `g_world_format` — [save-load.md](save-load.md) § *`.emod` worlds*). Design
rationale, rejected alternatives and the roadmap live in
`WORKING/emod-format-implementation-plan-2026-10-03.md`; this file states only what a file must
look like and how to read it.

`.emod` holds exactly the content of a [`.eden`](eden-file-format.md) world in a different
container:
- **column data** is compressed per column and stores only the occupied 16-block bands;
- **a save appends** records and never overwrites a committed byte; a commit record makes a batch
  live, so a torn save rolls back to the previous commit with no journal;
- every record carries a **CRC-32**.

The 192-byte `.eden` header, the creature block and the sign trailer are kept **verbatim**, so
nothing in them has to be understood to round-trip it.

**Naming.** The extension is `.emod`, never `.eden`: the shipped game lists every `*.eden` and
would run its 1.x in-place conversion on this file, destroying it. Code symbols stay Eden
(`EdenWorldStore`, `eden_*`). The bundled map in this container will be `Eden.emod` (S.6).

## Conventions

- **Little-endian**, explicit-width fields, at the offsets given. No struct is written by `memcpy`;
  there is no compiler padding anywhere in the format.
- **CRC-32** is zlib's `crc32()` (IEEE 802.3 polynomial, reflected, init and final XOR `0xFFFFFFFF`),
  i.e. what every target already links.
- "Column", `(x, z)`, the band layout and `CC(x,z,y) = x*256 + z*16 + y` mean exactly what they
  mean in [eden-file-format.md](eden-file-format.md). A **band** is one 16×16×16 chunk of a column:
  4,096 type bytes then 4,096 paint bytes, 8,192 bytes.

## File layout

```
File       := FileHeader (64 B) Record*
Record     := RecordHeader (32 B) payload (payload_len B) padding (0–7 zero bytes to an 8-byte boundary)
```

Records start at offset 64 and every record starts on an 8-byte boundary.

### FileHeader (64 bytes, written once, never rewritten)

| offset | type | field | value |
|---|---|---|---|
| 0 | `char[8]` | magic | `"EMODWLD\0"` |
| 8 | u16 | fmt_version | **1** |
| 10 | u16 | bands | **4** (64-block-tall world) or **16** (256z) |
| 12 | u32 | flags | 0 |
| 16 | u64 | created_unix | seconds; informational, never read for a decision |
| 24 | `u8[36]` | reserved | 0 |
| 60 | u32 | crc32 | CRC-32 of bytes 0..59 |

A reader **must refuse** the file if the magic, the CRC or `fmt_version` doesn't match, if `bands`
is not 4 or 16, or if `flags` or `reserved` is non-zero (that means a newer writer). Changing a
world's height (256z → 64z) is done by writing a new file, never by editing this header.

### RecordHeader (32 bytes)

| offset | type | field |
|---|---|---|
| 0 | `char[4]` | tag = `"EmR1"` (bytes `45 6D 52 31`) |
| 4 | u8 | type (table below) |
| 5 | u8 | codec: 0 none, 1 zstd frame, 2 zlib stream, 3–255 reserved |
| 6 | u16 | band_mask (COLUMN only; else 0) |
| 8 | i32 | x (COLUMN only; else 0) |
| 12 | i32 | z (COLUMN only; else 0) |
| 16 | u32 | aux: COLUMN → `ord`; every other type → **decoded payload length** |
| 20 | u32 | payload_len (stored bytes, ≤ 16 MiB) |
| 24 | u32 | seq (the batch this record belongs to) |
| 28 | u32 | crc32 over header bytes 0..27 followed by the `payload_len` payload bytes |

Padding is not covered by the CRC. It must be zero; a reader ignores it, `emod.py verify` reports
non-zero padding as damage.

### Codecs

| codec | payload | rules |
|---|---|---|
| 0 | the decoded bytes themselves | `payload_len` = decoded length |
| 1 | exactly **one zstd frame**, no skippable frames, no trailing bytes | the frame **must** have the content-checksum flag and the content-size field set, and content size = decoded length. No dictionary. The level is not stored; one decoder reads every level. |
| 2 | exactly one zlib stream (RFC 1950, with adler-32), no trailing bytes | inflates to exactly the decoded length |

An empty payload (decoded length 0) always uses codec 0. A file may mix codecs freely.
**Writers that must produce byte-identical files** (conversion, compaction and the cross-target
gate) use zstd **1.5.7, level 3**, checksum on, content size on, single-shot compression of the
whole payload. That is what `emod.py` writes (Python `zstandard` 0.25.0 bundles 1.5.7), and it is
the version S.2 vendors. Offline artefacts (the bundled map) may use level 19.

## Record types

| type | name | key | payload (decoded) | codec | notes |
|---|---|---|---|---|---|
| 1 | `COLUMN` | `(x, z)` | for each set bit *b* of `band_mask`, ascending: that band's 8,192 bytes | any | decoded length **must** equal popcount(band_mask) × 8,192 |
| 2 | `WORLD_HEADER` | singleton | the 192-byte `.eden` `WorldFileHeader`, verbatim | **0** | including 2.1.1-era garbage in `reserved[]`. Its `directory_offset` is stale and ignored; export recomputes it. Its `version` is kept verbatim and is 1–6; 5/6 ⇒ `bands` 16, **not the converse** — the 2026 game writes v2 256z worlds (S.3b), so a v1–v4 header may sit in a 4- or 16-band file. |
| 3 | `CREATURES` | singleton | the `.eden` creature block, verbatim: *slots* × 60 bytes | any | 0–400 slots; 0 is legal (the sibling editor writes none). For a `version` < 3 world the engine ignores it on load, as it does in `.eden`. |
| 4 | `SIGN_TRAILER` | singleton | the opaque gate-failing directory tail, verbatim | any | a multiple of 16 bytes, ≤ 1 MiB. Absent ⇔ no trailer; a later empty one removes it. *Informative:* despite the name it carries every trailer section — command blocks (`CMB1`) as well as signs (`SGN1`); see eden-file-format.md "The trailer model". The name is kept because this spec is frozen; to the store the payload stays opaque. |
| 5 | `PROVENANCE` | singleton | what the world was converted from (below) | any | written once by conversion; never by the engine's saves |
| 6 | `SUMMARY` | singleton | 16 bytes (below) | **0** | |
| 7 | `COMMIT` | — | 24 bytes (below) | **0** | ends a batch |
| 8 | `INDEX` | — | reserved | — | **not written in v1** (decided by S.3: a full scan opens Quarry's 30,299-column log in 15 ms on the Mac, ~45 ms by the ×3 A8X proxy, under the 100 ms threshold). A v1 reader ignores it. |
| 9–255 | reserved | — | — | — | a reader treats one as a member of its batch and otherwise ignores it; `verify` reports it |

### COLUMN

- **Key** `(x, z)`: absolute chunk-column coordinates, as in `ColumnIndex`.
  `0 ≤ x, z < 32768` and `(x, z) ≠ (0, 0)`: the engine's `twoToOne` key 0 is invalid.
- **band_mask**: bit *b* is set **iff** band *b* has any non-zero byte in its type plane **or its
  paint plane**. Real worlds have bands whose types are all air but whose paint is not (16 in
  SN City, 28 in Quarry), so a types-only test would silently drop paint. Bits ≥ `bands` must be
  clear. A cleared band decodes as 8,192 zero bytes.
  - Writers **must** be canonical (no all-zero band stored). Readers accept a stored all-zero
    band; `verify` warns about it.
  - Mask 0 (an all-air column) has codec 0 and an empty payload.
- **ord** (`aux`): the column's slot in an exported `.eden`, i.e. its rank by file offset in the
  source. A conversion assigns 0, 1, 2, … in offset order. A rewritten column keeps its ord; a new
  column gets `SUMMARY.next_ord`. Live columns have **distinct** ords, which may be sparse.
  - Ord is not the directory row order: the engine writes its directory in hashmap order. That
    order is kept in `PROVENANCE`.

### SUMMARY (16 bytes)

| offset | type | field |
|---|---|---|
| 0 | u32 | live_columns |
| 4 | u32 | next_ord: 1 + the highest live ord, or 0 with no columns |
| 8 | u16 | band_or: OR of every live `band_mask` |
| 10 | u16 | reserved = 0 |
| 12 | u32 | reserved = 0 |

All three values are **exact** for the live state after the batch that carries the SUMMARY is
applied. A batch that writes any `COLUMN` must also write a `SUMMARY`. `band_or` is the
constant-time "highest occupied band" answer that `.eden` cannot give.

### COMMIT (24 bytes)

| offset | type | field |
|---|---|---|
| 0 | u32 | seq; must equal the record header's `seq` |
| 4 | u32 | count: records in the batch, excluding this COMMIT |
| 8 | u32 | crc_of_crcs: CRC-32 over the batch records' `crc32` header fields, each as 4 LE bytes, in file order |
| 12 | u32 | reserved = 0 |
| 16 | u64 | prev_commit: file offset of the previous COMMIT record, 0 for the first |

A COMMIT has `band_mask`, `x` and `z` 0 and `aux` = 24. Anything else is not a COMMIT.

### PROVENANCE

The fields are packed with no padding. All offsets after `name` vary with the name length.

| type | field |
|---|---|
| u32 | prov_version = 1 |
| u32 | flags: the reasons an export cannot be byte-identical to the source (table below); 0 ⇔ it will be |
| u64 | source_size |
| i64 | source_mtime (seconds) |
| `u8[32]` | SHA-256 of the source's first 192 bytes followed by its whole directory region (`directory_offset`..EOF) |
| u32 | source `version` |
| u32 | source directory rows (whole 16-byte rows, trailer included) |
| u64 | dead bytes in the source's block region |
| u32 | creature slots derived |
| u32, bytes | name_len, then the source file name (UTF-8, no path) |
| u32, n × (i32 x, i32 z, u32 span) | short column spans |
| u32, n × (i32 x, i32 z) | the live directory rows **in source row order** |

| flag | meaning |
|---|---|
| 1 | non-canonical layout: columns not packed from offset 192 at full stride, dead bytes, or `directory_offset` ≠ 192 + n × stride + slots × 60 |
| 2 | short spans (exported at full stride, zero-filled) |
| 4 | interior gate-failing directory rows (dropped, as the engine drops them) |
| 8 | duplicate directory keys (the last row wins, as in the engine) |
| 16 | two rows share an offset |
| 32 | the directory region is not a whole number of rows |
| 64 | the creature gap is not a whole number of slots (the version's default was assumed) |
| 128 | a gate-failing tail over 1 MiB (dropped, as the engine drops it) |
| 256 | S.5e: the converter promoted a 64z source to 256z (below); the `.emod` is the 256z world, not the source's bytes |

Pairing a `.emod` with its source `.eden` (S.5) compares `source_size` and the SHA-256.

## Batches and commits — writing

A **batch** is one save: records that share one `seq`, followed by a `COMMIT` carrying that `seq`.
- The first batch has seq 1, and each later batch has seq = previous + 1.
- **Every record of the batch is written before its COMMIT**, the whole batch is flushed, and then
  the file is `fsync`ed (`FlushFileBuffers` on Windows; a no-op on web, whose ordered OPFS mirror
  provides the ordering).
- **The next batch starts only after that `fsync` returns.** Only the last batch in a file can
  therefore be torn by a crash or power loss; the reading rules below rely on it.

Before appending, a writer truncates the file to the reader's **truncation point** (below), which
drops any uncommitted or torn tail. A writer **never truncates below the end of an applied commit**.
A world's first batch contains at least `WORLD_HEADER`, `CREATURES` and `SUMMARY`.

**Compaction** writes a new file with the same `bands`, and the old `created_unix`, containing one
batch (seq 1) of the live records, then `fsync`s and renames over the old one. Ords are kept as
they are, so they may be sparse.
- `EdenWorldStore` writes them in conversion's order (`WORLD_HEADER`, `CREATURES`, a non-empty
  `SIGN_TRAILER`, `PROVENANCE`, `COLUMN`s by ord, `SUMMARY`, `COMMIT`); reserved types are dropped.
  zstd payloads are copied verbatim after they decode, anything else is re-encoded at zstd-3. So
  compacting a fresh conversion reproduces it byte for byte (an S.3 gate).
- Compaction is refused while the session's *damaged* flag is set.
- Every batch `EdenWorldStore` writes carries a `SUMMARY`, whether or not it has a `COLUMN`.
- A new world is created as `<path>.creating` (file header only) and renamed to `<path>` after its
  first batch is durable, so no file without a commit is ever listed under the world's name.

## Reading (normative)

1. **File header.** Validate it as above, or refuse the file.
2. **Scan.** From offset 64, *frame* a record at `p`. Framing succeeds when:
   - the tag matches;
   - `payload_len` ≤ 16 MiB;
   - the record plus its padding lies within the file;
   - the CRC matches.

   On success, continue at the record's end. On failure, `[p, q)` is a **damaged span**, where `q`
   is the next 8-aligned offset at which a record frames, or EOF.
3. **Commits.** The *valid commits* C₁…Cₘ are the framed records that are COMMITs. Let Rₖ be the
   byte range from the end of Cₖ₋₁ (or offset 64) to the start of Cₖ, and Sₖ its seq.
   - **Batch(Cₖ)** = the framed records in Rₖ whose seq = Sₖ.
   - **Orphans** = the other framed records in Rₖ. They belong to a batch whose commit was destroyed.
   - **Batch check:**
     - |Batch| = count;
     - crc_of_crcs matches;
     - `prev_commit` = offset of Cₖ₋₁ (0 for k = 1), or lies inside Rₖ. The second case means the
       commit it names was destroyed (its batch shows up as orphans or damaged spans); that is
       reported as damage.
4. **Apply** in file order. The latest applied record of a key wins.
   - Orphans with seq < Sₖ are applied, and are **damage**.
   - If the batch check passes, Batch(Cₖ) is applied. Damaged spans or orphans in Rₖ are
     **damage**, and so is a seq that is not Sₖ₋₁ + 1.
   - If it fails and k < m, Batch(Cₖ)'s records are still applied. Each of them passed its own CRC,
     and fsync ordering means this batch was complete before Cₖ₊₁ was written, so a failure here is
     media damage, not a tear. This is **damage**.
   - If it fails and k = m, the last batch is **torn**: it is discarded, so the world is the state
     at Cₘ₋₁ (plus any earlier orphans).
5. **Tail.** Bytes after Cₘ are an uncommitted tail: a save that never reached its commit. They are
   ignored.
6. **Truncation point** (for writers):
   - the end of Cₘ if its batch was applied;
   - if Cₘ's batch was torn, the end of the last applied orphan in Rₘ, or else the end of Cₘ₋₁
     (offset 64 if there is none).
7. **No valid commit** means the file holds no world, and the reader must refuse it.

**Damage** sets a per-session *damaged* flag. It **blocks compaction**, so the bad bytes are never
the only copy destroyed, and should offer export. A torn last batch or a tail is a normal crash
outcome and is not damage.

**Decoding a live record** can still fail: a zstd checksum, a wrong decoded length, or a framing
error inside the payload. When it does, the reader uses the newest **earlier applied** version of
the same key, which is still in the file until compaction. For a column with no earlier version, it
loads air **without marking the column modified**. It never substitutes the default map or the
generator, because the next save would then overwrite a player's build. Either case is damage.

### Invariants of a live state (`emod.py verify` checks all of them)

- `WORLD_HEADER` and `CREATURES` exist and decode. `WORLD_HEADER` is 192 bytes, its version is 1–6
  (5/6 only with 16 bands), and it uses codec 0.
- `CREATURES` is 0–400 × 60 bytes, and `SIGN_TRAILER` is a multiple of 16 up to 1 MiB.
- If there are columns, a `SUMMARY` exists and is exact.
- Every live column:
  - has an addressable key and no band bits ≥ `bands`;
  - has a distinct ord;
  - decodes to popcount × 8,192 bytes;
  - has codec 0 and no payload when its mask is 0.

## Conversion from `.eden`

**Upgrading to 256z (S.5e, 2026-10-10) — policy, not format.** The engine's converter can promote a
64z source: `EdenEmodConverter::Options::upgradeTo256z`, which the game sets from Settings' "Upgrade
64z worlds to 256z when converting" (default on) or the convert prompt's per-world answer. Then the
file header says `bands` 16; each `COLUMN` payload and `band_mask` are exactly a 64z conversion's (the
twelve bands above are absent, i.e. air); `WORLD_HEADER` is the source's with `version` < 5 set to 5
(a v3 header first gets the loader's v3 → v4 fields: `goldencubes` 10, sky colours 14);
`CREATURES` is the source's slots followed by empty ones (`type` −1, every other byte 0) to 400;
`PROVENANCE` describes the source and adds flag 256. Nothing else differs, and the rules below are
unchanged. `emod.py` never upgrades, so its byte-identical gates are untouched; with the option off
the engine's output is still `emod.py`'s byte for byte.

A conversion reads the source **exactly as the engine does** (`FileManager::readDirectory` and
`deriveColumnSpans`):
- **Height (S.3b):** never from `version` alone. Version 5/6 → 16 bands; otherwise the stride whose
  creature gap (`directory_offset − (highest live offset + stride)`) is 0 or a whole number of 60-B
  slots ≤ 24,000, 131,072 tried first; else the minimum distinct-offset gap (≥ 131,072 → 16); else 4.
  Normative copy: [eden-file-format.md](eden-file-format.md) *Height detection*. A streaming converter
  cannot know the stride before EOF, so it spills on a fixed 131,072 B grid and cuts columns at the
  detected stride afterwards (64z columns therefore always take the rebuild path; the output bytes
  are the same either way).
- **Directory:** read to EOF. The last row wins for a duplicate key. Rows failing the
  `twoToOne` gate are dropped, except a contiguous run at the very end, which is the sign trailer
  (kept verbatim, ≤ 1 MiB).
- **Creature slots:** `(directory_offset − (highest offset + stride)) / 60` when that is a whole
  number from 0 to 400, else the height's default (200, or 400 for 256z).
- **Column spans:** each span runs to the next-higher offset, capped at the stride. A short span
  reads as zero-filled.

It writes one batch, seq 1, in this order:
1. `WORLD_HEADER`;
2. `CREATURES`;
3. `SIGN_TRAILER`, if there is one;
4. `PROVENANCE`;
5. one `COLUMN` per live directory key, in file-offset order, with ord = rank;
6. `SUMMARY`;
7. `COMMIT`.

The output goes to `<stem>.emod.converting`. Every record is re-read and compared with the source,
then the file is `fsync`ed and renamed. The source is only ever read.

**Determinism.** Conversion is a pure function of the source bytes, the source file name and two
timestamps: `created_unix` and `PROVENANCE.source_mtime`, both of which `emod.py` takes from the
source's mtime. `emod.py convert --epoch N` sets both, for fixtures that must be byte-identical
across machines.

### Streaming conversion (forward-only)

Downloads and imports arrive as a byte stream, so the engine's converter
(`EdenEmodConverter`, and `emod.py convert --stream` as its reference) never seeks the source. Its
output must be, and is, **byte-identical** to the random-access conversion above. The method:
1. The first 192 bytes are the header, which gives `directory_offset`.
2. `[192, directory_offset)` is cut into stride-sized **slots** at `192 + k × stride`. Each slot is
   band-split and encoded exactly as a COLUMN payload would be, then appended to
   `<out>.spill`. A small per-slot index records `{spill offset, mask, codec, stored length, slot
   length, checksum of the bytes as fed}`.
3. `[directory_offset, EOF)` is the directory (and any trailer), held in RAM.
4. At EOF the directory is parsed exactly as above.
   - A live column whose offset is a slot boundary, with a full-stride span inside the region, **is**
     that slot: its spilled payload is copied.
   - Anything else is rebuilt byte-exactly from the spill and re-encoded: an off-grid offset, a short
     span, or an offset outside the region. Every slot read back is checked against the checksum of
     the bytes that were fed; without that, the read-back verification would be checking the spill
     against itself.
   - The creature block is read back the same way.
5. Then come the record order, the verification, the `fsync` and the rename of "Conversion" above,
   and the spill is deleted. A failure deletes every temp file.

Real worlds are often off-grid: 23,484 of Quarry's 30,299 columns take the rebuild path, because its
records start 24,000 B past their slot boundaries. RAM is one slot, the directory, about 40 B of
index per column and two decoded slots: Quarry streams from its `.gz` with 1.9 MB held. Disk use is
the spill (about the size of the `.emod`) plus the `.converting` file. Inputs it refuses, with no
temp file left: a 1.x header, `version > 6`, a stream that ends before `directory_offset`, a
directory region over 256 MiB, and an optional temp-bytes cap (S.5's 500 MB).

## Export to `.eden`

1. **Pre-flight, exact:**
   192 + *n* × stride + creature bytes + 16 × *n* + trailer bytes,
   where stride = `bands` × 8,192.
2. **Header:** `WORLD_HEADER` with bytes 32..39 (`directory_offset`) set to
   192 + *n* × stride + creature bytes.
3. **Columns:** in ascending ord, the *i*-th at offset 192 + *i* × stride, full stride, cleared
   bands zero.
4. **Creature block:** `CREATURES`.
5. **Directory:** one row per live column. Rows come in `PROVENANCE`'s source row order (keys still
   live), then any other columns in ord order. Each row is `{i32 x, i32 z, u64 offset}`.
6. **Trailer:** `SIGN_TRAILER`.

The output goes to `<name>.eden.exporting`, is `fsync`ed and renamed. **When `PROVENANCE.flags` = 0
and the world has not been saved since conversion, the export is byte-identical to the source.**
Otherwise it is *logically* identical: the same header apart from `directory_offset`, the same
creature block, the same trailer, and the same engine view of every column.

### Target formats (S.5b, 2026-10-10) — not part of the container

The rules above are the **own-format** export. The engine's exporter (`Classes/EdenWorldExport.cpp`)
adds three targets for other readers; they are policy, so `emod.py` does not implement them, and
none of them changes what an `.emod` holds:

| Target | Height | `version` | Creature block | Ids 112–127 | Sign trailer |
|---|---|---|---|---|---|
| own format | the source's | the source's | the source's | kept | kept, or pruned |
| Legacy64z | 64 (bands 0–3; `convertWorldTo64`'s cut: lost type bytes counted, doors/portals orphaned at y = 63 cleared) | 4 if the source is 256z, else the source's | 200 slots; y ≥ 64 dropped, survivors relocated | → stone (2), painted the nearest swatch (D.2b) | dropped |
| NewDawn256z | 256 (64z padded with air) | 5 | 400 slots, padding type −1 | → painted stone | dropped |
| NewFormat256z | 256 | the source's if 256z, else 5 | 400 slots | kept | kept, or pruned |

`pos.y` / `home.y` are clamped on a 256→64 cut. **Pruning** keeps a trailer record (sign or command
block) only when its column `(floor(x/16), floor(y/16))` is in the world; the trailer is rebuilt only
when something is actually dropped, so a keep-all export carries the original bytes. The pre-flight
(`begin()`) scans columns only for Legacy64z / NewDawn (the cut and the substitution need it) and
reports the exact output size and every loss — blocks above 63, ids replaced, signs and command
blocks dropped or pruned, creatures dropped — as JSON (`ExportReport::json()`) and as one sentence
for a dialog (`summary()`). D.2b (done): 112–127 → stone painted the colorTable swatch nearest each id's placeholder colour (`kNewBlockPaint` in `EdenWorldExport.cpp`).

**Known gap (found by S.4's damage test):** a COLUMN whose *frame* CRC fails is a damaged span, so
its key is unknowable and the column reads as never saved — the engine then fills it from the default
map or the generator, which the rule above forbids only for records that frame and then fail to
decode. Its bytes are not lost (damage blocks compaction), but nothing yet recovers them. A future
reader could report the header's `x`, `z` as *suspect* keys when the record header parses; not
specified.

## Verified (S.5 / S.5b, 2026-10-10) — export
- The pre-flight size equals the written size on every export (all targets, keep and prune, `.eden` and
  `.emod` sources, native and wasm).
- alpinecraft (v2 256z, 250 signs) → each of the four targets × keep/prune: the outputs from the `.eden` and
  from the `.emod` are identical. Own format is byte-identical to the source. Legacy64z equals
  `convertWorldTo64`'s output except the 1,675 ids 112–127 now stone, and the report's counters match
  that function's (229,854 blocks discarded / 138 columns / 0 doors / 3 creatures). NewDawn has no id > 111
  and no trailer. Prune keeps 158 of 250 signs, all in-region, and the trailer parses. `emod.py`
  converts and verifies every output.
- BlockmapWIP 64z → NewDawn256z → Legacy64z is byte-identical to the source.
- W256 (Diane, 266 MB `.eden`) from its 1 MB `.emod` as a streamed `.eden.gz` in wasm: 1.13 MB live
  allocation peak, and the inflated output is the original's SHA-256.
- Full record: `WORKING/s5-results-2026-10-10.md`.

## Verified (S.4, 2026-10-09) — the engine
- `--headless --stage1` new world as `.emod`: `geom c26fa4fd2a5b2660 / full 49b71a4e16afb125` (64z) and
  `b19d3e33a8717c60` (256z), identical to `.eden`.
- Real worlds, the same checksum loaded from the `.eden`, from the engine's own conversion, and from
  the `.emod` reloaded: Diane (256z v5), alpinecraft (v2 256z + signs), CanvasXEN0, BlockmapWIP.
  The engine's conversion is byte-identical to `emod.py convert` (alpinecraft, Diane).
- alpinecraft's 40,032-byte sign trailer survives an `.emod` edit at y = 200 + save + reload, byte
  for byte (checked through `emod.py export`).
- A COLUMN that frames but does not decode loads as air and is not rewritten by the next save.

## Verified (S.3, 2026-10-09)

Results: `WORKING/s3-results-2026-10-09.md`. `eden_native --emod-selftest` and
`web/tools/headless-emod-store-test.js` (wasm) run the same gates over the checked-in pack
`web/tools/fixtures/emod/`, which `emod.py fixtures-pack` writes:
- every S.1 fixture is stream-converted to emod.py's exact bytes (SHA-256), and emod.py's
  two-batch, zlib and raw files read back to emod.py's state;
- crash injection at every byte of a batch, killed prefix and zero-filled: always the old or the new
  commit, never a mix;
- the corrupt gate: 29/29 cases reported; with the CRC check off it fails, 10 + 1, the same as emod.py;
- seven real worlds, Quarry's `.gz` included, are byte-identical to `emod.py convert`.

## Verified (S.1, 2026-10-09)

Results: `WORKING/s1-results-2026-10-09.md`.
- **Round trip:** `.eden → .emod → .eden` is byte-identical on W64, the 14 MB W64 cut, W256 (Diane),
  SN City and the two sibling-editor v5 specimens. Quarry is logically identical; its one short
  span is reported.
- **Determinism:** two conversions are byte-identical.
- **Corrupt gate:** `emod.py --corrupt-gate` flips one byte in the header and the payload of every
  record type written (first and last occurrence), flips two bytes of the file header, and
  truncates mid-record in both batches. All 29 cases are reported, with no mixed state. With
  `--no-crc` the same gate **fails** (10 cases undetected, 1 mixed state).
- **Fixtures:** `emod.py fixtures` writes synthetic `.eden` files covering:
  - a paint-only band;
  - an all-air column;
  - a 0-slot creature block, in a v6 and a v2 world;
  - a sign trailer;
  - a short span;
  - a dead record, a duplicate key and an interior bad row;
  - both heights.

  `emod.py selftest` round-trips them under zstd, zlib and none, then runs the gate.
