# Content packs (`pack.json`)

Normative schema for Emod's content packs (Stage K.2, plan `WORKING/data-driven-content-plan-2026-11-01.md`).
Where `Classes/ContentPack.cpp` or `port/tools/pack.py` disagree with this page, this page wins and the code
has a bug. Status: **K.2-3 (2026-11-01)** loads the stock pack `eden` at boot; **K.2-4 (2026-11-01)** loads
mod packs (`Documents/Packs/`, `--packs=`) at every world entry ("Mod packs" below). **K.2-5
(2026-11-01)** adds the Mods picker pages ("Mods page" in `ui.md`); a mod block can also be placed by name from the console
(`set ~ ~ ~2 emod_example:jelly`).

## What a pack is

One directory holding one `pack.json` and the PNGs it names by relative path:

```
<folder>/pack.json
<folder>/<any relative path>.png          (mod packs: 32x32 RGBA or RGB)
```

The pack `id` is its namespace: a block `stone` in pack `eden` is named `eden:stone` everywhere (the console,
the world's `BLOCK_PALETTE`, the picker). `eden` is the bundled stock pack; `emod` is reserved for packs Emod
ships; a mod pack may use neither. The bundle also carries `packs/emod_example/`, a template that is **not**
loaded by default (D14), and `packs/wonders/`, the example every player gets in `Packs/` on first start (below).

### The stock pack `eden`

`media/packs/eden/pack.json`, shipped as `<bundle>/packs/eden/pack.json` (the `native/CMakeLists.txt` rule
links or copies `media/packs/**` beside the executable with its tree kept; the packaging steps in
`.github/workflows/native.yml` copy `packs/`, and the iOS `.app` carries it). It is **the only source of rows
0..127 of every per-type table**: `blockinfo`, `blockColor`, `blockTypeFaces`, `hudBlocksMap`,
`blockTntMap`, `blockSoundSet`, `blockPaintArt`, the sound sets `blockSoundSets[]`, the tile tables
`tileTwin`/`tileParticleArt`, the stock names and console aliases (`BlockRegistry`'s stock half) and the
two picker pages (`Hud`). The C initialisers that held them are gone (`pristine-engine` keeps the originals).

- **Boot (D5):** `main()` calls `ContentPack::loadStock(bundle root)` right after the roots are set and
  **before any mode runs** (the selftests and `--content-dump` read the tables too). It reads
  `<bundle>/packs/eden/pack.json`, else `<bundle>/media/packs/eden/pack.json` (a bundle root that is the repo),
  compiles the whole pack into a staging copy and writes the live tables only when every check passes, then
  rebuilds the shadow casters. Until then every row 0..127 is zero; rows 128..255 are the missing block's
  (`Globals.cpp`) and the world's palette (`BlockRegistry::resolve`).
- **A missing or invalid `eden` is fatal:** every error line goes to stderr (`[pack] ...`), an interactive run
  also shows a message box, and the process exits 1. Never a half-filled table.
- **What it compiles to is pinned:** `--content-dump` prints the tables (canonical text) and
  `.github/scripts/check-content-dump.py` pins its SHA-256 and diffs it against
  `port/tools/fixtures/content/stock-dump.txt` on every CI leg, the iOS simulator included. Editing
  `pack.json` changes block behaviour for every world: re-pin the dump in the same commit and say so.
- **Never renumber a `stock_id`** (root `CLAUDE.md` rule 4: worlds store the raw byte).

## JSON dialect

RFC 8259 over UTF-8 (`Classes/Json.h`), plus `//` line comments wherever whitespace may stand (packs are
hand-written). No trailing commas, no `/* */`, no NaN/Infinity, **no duplicate keys** in one object. A leading
BOM is skipped. Caps: 1 MiB per `pack.json`, nesting depth 32, 512 blocks per pack. Object member order is
significant where this page says so (sound sets, aliases).

## Top level

| Key | Type | Required | Meaning |
|---|---|---|---|
| `format` | int | yes | this document's version: `1`. A higher one is refused. |
| `id` | string | yes | `[a-z0-9_]{1,24}`; must be `"eden"` for the stock pack. |
| `name`, `version` | string | no | shown in K.3's manager. |
| `tiles` | object | yes | tile name → tile (below). |
| `sound_sets` | object | yes (`eden`) | set name → sound set, **in index order**: the first is every block's default. |
| `blocks` | array | yes | block definitions (below), at most 512. |
| `aliases` | object | no (`eden` only) | console short name → block name, in the console's order. |
| `picker_pages` | array | yes (`eden`) | the two stock picker pages (below). |

Unknown keys are an error in `eden` and a `warning:` line in a mod pack (a pack written for a newer Emod still
loads what this build understands). A mod pack's top level is `format`, `id`, `name`, `version`, `tiles`
(optional) and `blocks`.

## Tiles

`"tiles": { "<name>": { ... } }` with names `[a-z0-9_]{1,24}`. A block's face names a tile, either by its
local name or as `"<pack>:<name>"` (`"eden:stone"`; inside `eden` the two are the same).

| Key | Type | Meaning |
|---|---|---|
| `atlas` | 1 or 2 | *(stock)* `atlas.png` (opaque, uploaded RGB565) or `atlas2.png` (the transparent pass). |
| `index` | 0..127 | *(stock)* the tile's slot in that atlas strip. One name per (atlas, index). |
| `unpainted` | tile name | the tile an **unpainted** face swaps to (drawn white, the art carries its colour): grass side, TNT side/top, brick, the 112-127 greyscale art. Stock: both tiles must be atlas 1 tiles and the twin cannot be index 0 (`tileTwin` uses 0 for "none"). Mod: another tile of the same pack; it takes effect on opaque blocks with the `paint_art` trait. |
| `particle_art` | bool | debris drawn with this tile takes its art (white) whatever the block (atlas 1 only): grass side and the 112-127 tiles. |
| `png` | path | *(mod packs, required)* a 32x32 PNG, relative to the pack folder (no `..`, `\`, `:` or leading `/`; at most 1 MiB). The loader places it in a free atlas slot ("Mod packs" below). |

`eden` names every used `atlas.png` tile 0..65 (`grass_top` = 0 … `grass_flower` = 64, `missing` = 65; the
names follow `Constants.h` `TEX_*`, 32-47 are `<block>_color` and 48-63 `<block>` for ids 112-127) and the four
`atlas2.png` tiles blocks use (`glass` 0, `weave` 8, `water` 16, `lava` 24). The engine still names a few
tiles by constant for bespoke drawing (`TEX_CLOUD`, `TEX_GRASS_TOP`, …): the atlas *images* are not data.

## Sound sets

`"sound_sets": { "<name>": {"build": S, "break": S, "land": S|null, "step": S|null, "tnt": G} }`. A block
names one by its full name (`"sounds": "eden:stone"`). `build` and `break` are required; `land`/`step` absent
or `null` mean silent; `tnt` is the group the block joins in an explosion's sound mix: `other`, `dirt`,
`stone`, `glass`. At most 64 sets. Sound names (`Resources.h` `SOUND_TYPES` minus `S_`, lowercased):
`build_wood build_water build_stone build_leaves build_lava build_glass build_dirt build_generic build_metal
build_light break_wood break_water break_stone break_leaves break_lava break_dirt break_glass break_generic
metal_destroy land_soft land_hard footsteps_hard footsteps_soft`.

A set is a tuple, not a material: stock's per-event chains disagreed (brick builds as stone but mixes into a
blast as "other"), so `eden` has 25 sets (`docs/world-and-terrain.md` "Per-type behaviour"). Only `eden`
defines sets in K.2; custom sound files are a conditional row (K.2-A).

## Blocks

```jsonc
{"id": "ladder", "stock_id": 18, "kind": "cube",
 "traits": ["solid", "flammable", "climbable"],
 "faces": {"sides": "ladder", "bottom": "wood", "top": "wood"},
 "color": [230, 202, 109], "sounds": "eden:wood", "bt": "eden:btladder"}
```

| Key | Type | Meaning |
|---|---|---|
| `id` | string | `[a-z0-9_/]{1,48}`, unique in the pack; the block's name is `<pack>:<id>`. |
| `stock_id` | 0..127 | *(eden only, required)* the type byte. All 128 present exactly once. |
| `kind` | string | the bespoke engine code the block runs (D7): `cube` (default), `air`, `grass`, `tnt`, `liquid`, `ramp`, `side`, `firework`, `door`, `object`, `lightbox`, `portal`, `custom`, `block_tnt`, `bt`. Documentation and validation only (the code keys on the id); from K.2-4 a mod block must be `cube`. |
| `traits` | names | behaviour, each compiling to a `blockinfo` bit or a table row (below). |
| `flags` | names | *(eden only)* raw `blockinfo` bits that belong to kinds: `ramp side ramp_or_side liquid water lava colored_special grass ice object door portal block_tnt`. |
| `faces` | object | `{"all": T}`, `{"sides": T, "top": T, "bottom": T}`, or all six of `front` (-z), `back` (+z), `left` (-x), `right` (+x), `bottom`, `top` (`blockTypeFaces` index order 0..5; `cubeShortVertices` in `Geometry.cpp`). Every tile must be in the atlas the block draws from (atlas 2 iff `transparent`). Absent: every face is index 0 (only `eden:none` and `eden:custom`, which draw no faces of their own). |
| `color` | [r, g, b] | 0..255 each, required: `blockColor`. The mesher **tints an unpainted block's tiles by it** (most stock tiles are grey art coloured this way; a `paint_art` block draws its tiles' `unpainted` twins white instead), and debris and the flat-colour stand-ins use it. A mod tile drawn in full colour wants `[255, 255, 255]`. |
| `sounds` | set name | full name (`"eden:stone"`); absent = the first set. |
| `paint_art_sites` | names | *(eden only)* which of the six "draws its own art when unpainted" sites apply: `chunk batch cursor icon picker particle` (stock's lists disagreed; K.2-2). Not together with the `paint_art` trait. |
| `bt` | block name or `null` | *(eden only)* the picker cell's "block TNT" second block (`hudBlocksMap`): a name, `null` (-1, no variant), or absent (0; rows that never sit in a picker cell). |
| `tnt_map` | block name | *(eden only)* a block-TNT block's base material (`blockTntMap`); absent = 0. |
| `fallback`, `fallback_paint` | `eden:` name, 0..54 | *(mod packs; `fallback` required, paint default 0)* the placeable stock block export/upload write instead (K.1 rules: 1..127, not 66-70 or 75-80; a canonical `eden:` name, not an alias). |
| `picker` | bool | *(mod packs, default true)* list the block on the Mods picker page (K.2-5). |

### Traits

| Trait | Compiles to |
|---|---|
| `solid` | absence of `IS_NOTSOLID` (a block without it is walk-through) |
| `hard` | `IS_HARD` |
| `flammable` | `IS_FLAMMABLE` |
| `transparent` | `IS_ATLAS2`: drawn from `atlas2.png` in the sorted transparent pass |
| `climbable` | `IS_CLIMB` (bit 18) |
| `bouncy` | `IS_BOUNCY` (19) |
| `slippery` | `IS_SLIPPERY` (20) |
| `blastproof` | `IS_BLASTPROOF` (21) |
| `unbreakable` | `IS_UNBREAKABLE` (22) |
| `no_shadow` | `IS_NOSHADOW` (23): no baked-shadow caster |
| `paint_art` | `blockPaintArt` = every site (`ART_ALL`) |

A trait or flag listed twice is an error.

## Aliases and picker pages (`eden` only)

`"aliases": {"stone": "eden:stone", "lamp": "eden:lightbox", ...}`: the console's unqualified names, in
`CmdScript`'s completion order (`[a-z0-9_]{1,24}`). `"picker_pages"`: exactly two arrays, of 35 and 20 cells
(the stock grid, `Hud.h` `NUM_DISPLAY_BLOCKS` / `NUM_PICKER_PAGE2_CELLS`); a cell is a block name or a tool,
`"tool:sign"` / `"tool:cmd"`. A block's page for "open on the current block's page" is read from these lists.

## Validation and messages

Every refusal is one line `<pack>@<byte offset>: <message>`: the offset is where the offending value starts
(a JSON error: where the reader stopped; a missing key: the containing object). The engine and `pack.py`
check the same rules **in the same order** and print the same lines. A JSON error stops at the first; any
other error is collected and the check goes on, so a broken pack reports everything at once. `eden`: any
error refuses the pack (and the boot). A mod pack's lines are headed by its **folder** name (the id may be
the broken part): a `warning:` refuses nothing, an error inside a tile makes that tile unknown (its blocks
then fail with "unknown tile"), an error inside a block skips that block, and an error at the top level
(`format`, `id`, `name`/`version` types, `tiles` not an object, `blocks` missing, not an array or over 512)
skips the whole pack. Every line goes to stderr as `[pack] ...` and to the console's `packs` verb.

## Mod packs (K.2-4)

```jsonc
{"format": 1, "id": "stairs", "name": "Stairs and Trims", "version": "1.0.0",
 "tiles": {"oak": {"png": "textures/oak.png", "unpainted": "oak_color"},
           "oak_color": {"png": "textures/oak_color.png"}},
 "blocks": [{"id": "oak_planks", "faces": {"all": "oak"}, "color": [196, 150, 96],
             "traits": ["solid", "flammable", "paint_art"], "sounds": "eden:wood",
             "fallback": "eden:wood", "fallback_paint": 0, "picker": true}]}
```

- **Where they come from (U3, D15).** At every world entry: each `--packs=DIR` (a pack folder, or a folder of
  packs; repeatable) in argument order, then every folder of `<Documents>/Packs/` by name, bytewise. All of
  them load unless the enable list switches them off or reorders them (K.3, "Managing packs" below). A folder without a `pack.json` is skipped with a line; a pack
  whose `id` is `eden`/`emod`, or is already loaded from an earlier folder, is refused. On iOS
  `Documents/Packs/` is reachable from the Files app.
- **When (D6).** `World::loadWorld`, on the main thread, before the load pthread: the previous load's
  definitions are unregistered, the packs are validated and each valid block becomes a `BlockRegistry`
  definition `<id>:<block id>`; then the palette resolves against them (`FileManager::loadWorld`). Adding,
  removing or editing a pack takes effect at the next world load, no restart. A multiplayer session loads
  none (K.1 D7) and gets the stock atlases back.
- **What a block may say.** Kind `cube` only (any other `kind` is refused: kinds are bespoke engine code,
  D7); `traits` (the table above); `faces` (required; a pack tile by name or `<id>:<name>`, or any
  `eden:` tile, which costs no atlas slot and must be in the block's atlas); `color` (required); `sounds`
  (an `eden:` sound set; absent = `eden:generic`); `fallback` (required), `fallback_paint`, `picker`. The
  stock-only keys (`stock_id`, `flags`, `paint_art_sites`, `bt`, `tnt_map`) are errors. A mod block's
  `hudBlocksMap` row is -1 (no block-TNT variant), `blockTntMap` 0, and it never carries `IS_MISSING`.
- **Atlas tiles (D10).** Pack tiles go into the free slots of the two atlas strips: `atlas.png` 66..127
  (62 tiles, uploaded RGB565: alpha is dropped) for opaque blocks and `atlas2.png` 32..127 for `transparent`
  ones, where **a tile takes 8 slots**: `Terrain::render2` shifts the whole transparent pass's V by
  `frame/16` (0..7 tiles), so every atlas2 tile is an 8-frame strip (water and lava animate; glass 0-7 and
  weave 8-15 repeat one image). The loader writes the PNG into all 8, so **12 transparent tiles** fit
  (strips at 32, 40, ..., 120); an animated mod tile is a later row. Allocation is first use, pack by pack, block by block, face by face; a tile used by
  both an opaque and a transparent block takes one slot in each; an opaque block's tile brings its
  `unpainted` twin along (`tileTwin`; only opaque blocks swap to twins). A block whose new tiles do not all fit draws `TEX_MISSING` on every
  face (from `atlas.png`, so a transparent one turns opaque) and the loader prints one line; nothing else
  moves. `Resources::setModTiles` composes the stock pixels (decoded from the bundle the first time a pack
  has a tile, then kept: 2 x 512 KiB) with the tiles and re-uploads only an atlas whose content changed,
  with its mip chain (`Texture2D::replaceFromImage`, the constructors' arguments). Engine code reads
  `atlas->name` at every draw, so nothing caches the old texture.
- **What a world keeps.** Nothing about the pack: a placed block's byte and its name in `BLOCK_PALETTE`
  (K.1). Without the pack the bytes draw as the missing block and survive saves untouched; with it back the
  look returns.
- **The console.** `packs` lists every folder looked at (loaded or not, each with its lines), the loader's
  lines and the atlas tiles used; `packs <id>` lists that pack's blocks with their byte in this world.
- **The example pack** `media/packs/emod_example/` (bundled as `packs/emod_example/`, not loaded by default):
  six blocks that between them use every trait, an `eden:` tile, own tiles with an unpainted twin and one
  transparent block. Its art is drawn by `port/tools/example-pack-art.py`. To try it, copy the folder into
  `Documents/Packs/` and open a world, or run with `--packs=<bundle>/packs/emod_example`.
- **The Wonder Blocks pack** `media/packs/wonders/` (bundled as `packs/wonders/`, **installed for every player**:
  an interactive start copies it into `Packs/wonders/` once per Documents folder, `ContentPack::seedBundledPacks`,
  recorded in `Packs/.bundled` so a player who removes it keeps it removed; harness runs never seed; gate:
  `--mods-selftest` leg 9): the user-facing example, every feature reachable from the block picker with no console. 23 blocks on 26 tiles
  (Kenney's CC0 Voxel Pack plus Emod-drawn icons, `CREDITS.txt`; drawn by `port/tools/wonders-pack-art.py`):
  all faces, sides/top (logs, kits), a paintable tile with its twin (paint_brick), `color` tinting a full-colour
  or grey tile (slick_ice, bounce_pad, warp_pad), stock tiles only (vine_ladder), four transparent blocks, two
  walk-through ones, every trait. Thirteen Lua files sharing one state, one feature each (`lib.lua`'s
  `W.when(event, block, fn)` routes events by block): a guide tour, cabin and bridge kits, a sapling that grows a
  pine (faster in rain), warp pads linked by paint colour (re-found after a reload by a background scan), a lucky
  block, sun/moon/rain stones, gold ore, a creature egg, a first-aid block and a paint ripple over bounce pads.
  Its `README.md` is the player guide and the mod maker's map of the folder. CI (linux) checks its `## mods`
  parity like the example's. It replaced the console-driven `showcase` pack (2026-11-07; git history).

- **Scripts (K.4).** `"scripts": ["scripts/main.lua", ...]` lists sandboxed Lua sources (at most 16, each a
  relative `.lua` path inside the pack, 256 KiB each, 1 MiB together, source only). A bad entry refuses the pack;
  with `"scripts"` the `"blocks"` key may be left out. The bytes join the pack hash after the tiles. What a script
  can do, its budgets and its events: [scripting.md](scripting.md). The example pack's `scripts/main.lua` adds
  `mod pillar N`.

## Managing packs (K.3)

Plan: `WORKING/mod-manager-plan-2026-11-01.md`. Code: `Classes/ContentPack` (list, order, resolution, hash,
world mods) and `Classes/PackInstall` (zip install, catalogue). Gate: `eden_native --mods-selftest --docs=<fresh>`.

- **Dependencies.** A mod pack may say `"requires": ["base", "trims>=1.2"]` and `"conflicts": ["other"]` (at
  most 32 each; ids as for `id`; a version is digits and dots, compared part by part, the leading digits of each
  part of the installed pack's `version`, missing parts 0; only `>=`). A malformed entry, or one naming the pack
  itself, is a top-level error. After every folder is compiled and duplicate ids dropped: packs whose
  requirements are not loaded are skipped (to a fixpoint), then a pack that conflicts with an **earlier** loaded
  pack (declared on either side) is skipped, then requirements again. Each skip is one loader line, `<folder>:
  needs "x" (not loaded)` / `(not installed)` / `needs "x" 1.2 or newer (found "1.0")` / `conflicts with "x"`,
  each ending `(skipped)`. A requirement does not change the order. `pack.py` resolves `--packs=` folders the same
  way (`port/tools/fixtures/content/mods-resolve/`, parity in CI).
- **The enable list** `Packs/mods.json`: `{"format": 1, "packs": [{"id": "stairs", "enabled": true}, ...]}`.
  Order = load order (tile slots, picker order, which of two conflicting packs wins). An installed pack that is
  not listed loads after the listed ones, enabled; a listed pack that is not installed keeps its entry; a broken
  file is ignored with a line (`mods.json: ... (ignored: every pack is enabled)`). `--packs=` folders load first
  and are never listed. Only the manager writes the file (`setPackEnabled`, `movePack`; console `packs on|off
  ID`); changes apply at the next world load. A switched-off pack still appears in `packs` as `off`.
- **What a world uses** is its palette's namespaces (`ContentPack::worldMods()`: loaded / off / not installed,
  with the number of palette ids). Nothing new is stored in the world. A switched-off pack that a world uses is
  not loaded for it: its blocks draw as the missing block until it is switched on.
- **The mod-list hash** (`ContentPack::modsHash()`): SHA-256 over `"<id> <version> <pack hash>\n"` per loaded pack
  in load order, where the pack hash covers `pack.json` and every tile PNG it reads; `""` with no pack. It is what
  a server will advertise as INFO `mods=` (`docs/networking.md`); the console's `packs` prints its first 16 digits.
- **Installing a zip** (`PackInstall::installZip`): one pack per zip, `pack.json` at the root or inside one top
  folder (Finder's/Explorer's "Compress" output; `__MACOSX/` and dot files are skipped). Refused before anything
  reaches `Packs/`: an unsafe entry name (absolute, `..`, `\`, `:`), encryption, zip64, a method other than stored
  or deflate, a CRC mismatch, over 32 MiB zipped / 16 MiB unpacked / 4 MiB per file / 1024 entries, a SHA-256
  mismatch (catalogue downloads), a pack the validator refuses (its lines are returned), or a folder of the pack's
  id that holds another pack. It unpacks into `Packs/.install.partial/` and commits by renames: an installed pack
  with the same id is replaced in its own folder (folder name and list place kept); otherwise the folder is
  `Packs/<id>/`.
- **The catalogue** a Get Mods source serves: `{"format": 1, "packs": [{"id", "name", "version", "description",
  "author", "url", "sha256", "size", "requires"}]}`. An entry without a valid id, an `http(s)://` url or a 64-hex
  sha256, a duplicate id, or `eden`/`emod` is dropped (`PackInstall::parseCatalogue`). No catalogue is hosted yet.
- **The screen** (K.3-5/6) that drives all of the above: `docs/ui.md` "The Mods screen". It lists `scanPacks`, toggles with `setPackEnabled`, reorders with
  `movePack`, removes with `removePackFolder`, installs a picked zip or a catalogue entry with `installZip`; a world that uses an off or missing mod says so once on entry.

## Make your first block (player guide)

A mod block is a folder with a `pack.json` and a PNG. No code, no rebuild.

1. **Find the folder.** `Packs/` inside the game's data folder (macOS `~/Library/Application Support/Emod/Packs/`; on iOS
   Files → On My iPhone/iPad → Emod → `Packs`). Make it if it is missing. A trial run can use
   `eden_native --packs=<folder>` instead.
2. **Copy the template.** Copy the bundled `packs/emod_example/` (repo: `media/packs/emod_example/`) into `Packs/`
   and rename the folder, e.g. `my_blocks`. Open `pack.json` and change `"id"` to the same name (lowercase
   letters, digits, `_`; `eden` and `emod` are taken). The id becomes the prefix of your block names.
3. **Draw a tile.** A 32x32 PNG (RGB or RGBA) in the folder, say `textures/brick.png`.
4. **Describe it.** Keep one tile and one block; delete the rest of the template:
   ```json
   {"format": 1, "id": "my_blocks", "name": "My blocks", "version": "1.0.0",
    "tiles":  {"brick": {"png": "textures/brick.png"}},
    "blocks": [{"id": "brick", "faces": {"all": "brick"}, "color": [255, 255, 255],
                "traits": ["solid", "hard"], "sounds": "eden:stone", "fallback": "eden:stone"}]}
   ```
   `color` tints grey art (use white for full-colour art); `traits` are the table in "Traits"; `fallback` is
   the stock block other players (and you, without the pack) see.
5. **Check it.** `python3 port/tools/pack.py validate Packs/my_blocks` prints every problem, one per line; in the
   game the console verb `packs` lists the same lines and `packs my_blocks` lists the blocks and their bytes.
6. **Use it.** Open a world (packs are read at every world entry, no restart). Place it from the Mods pages of the
   block picker, or `set ~ ~ ~2 my_blocks:brick` in the console. The first placement in a world asks to confirm,
   because that world then holds bytes (128..255) only your pack explains.
7. **Remove it** and the blocks show as the magenta missing block; the bytes, paint and the name in
   `BLOCK_PALETTE` stay, so putting the pack back restores the look. Exports (`.eden`) carry each block's
   `fallback` instead.

Rehearsed on copies of real worlds (K.2-6, `TESTERS/k2/rehearsal/`): placed six blocks, saved, reopened without the
pack (bytes kept, a save with no pack changes nothing), reopened with it, `emod.py verify` OK, exported to `.eden`
and re-imported with the palette intact.

## Tools and gates

- `eden_native --mods-selftest --docs=<fresh>` (K.3, `port/src/harness/mode_mods.cpp`): the enable list and its
  order, requires/conflicts, the hash, `worldMods`, zip install/update and every refusal, the catalogue parser.
  CI: every desktop leg and sanitize.

- `python3 port/tools/pack.py validate DIR` — the verdict, one line per error (stock rules for a folder named
  `eden` or with `--stock`, the mod rules otherwise; reads the repo's `media/packs/eden` for `eden:` names).
- `eden_native --content-dump --packs=DIR` also prints the `## mods` section (packs, lines, each block's rows,
  the tile allocation, the twins) after the pinned text; `python3 port/tools/pack.py compile DIR... [--check
  LOG]` prints the same section independently (CI, linux: parity on the example pack).
- `eden_native --packs-selftest --docs=<fresh>` — the mod runtime gate (`port/src/harness/mode_packs.cpp`): the
  example pack copied into `Documents/Packs/` beside an impostor, a broken, an empty and a duplicate folder;
  every block placed by the console; save + reload; the pack removed (placeholders, bytes, paint and palette
  unchanged) and restored (the mesh hash returns); a pack over both budgets (60 opaque and 12 transparent
  tiles); the load order; a session; the atlas upload count and pixels (all 8 frames of a strip). CI: every leg, sanitize and the iOS simulator.
- `python3 port/tools/pack.py compile media/packs/eden [--check DUMP]` — the `--content-dump` text the pack
  compiles to, independent of the engine; CI (linux) checks it against the binary's dump (parity).
- `python3 port/tools/pack.py fixtures port/tools/fixtures/content/packs` and `eden_native
  --pack-selftest=port/tools/fixtures/content/packs` — every `<case>/pack.json` must be refused with exactly
  the lines of its `expect.txt`, in order; the selftest also re-compiles the loaded stock pack against the live
  tables, checks the 1 MiB cap, and that a refused pack leaves the tables alone. A case folder named `mod-*`
  is a mod pack (its PNGs beside it): its `expect.txt` holds every line, warnings included, and the verdict
  `= refused` or `= blocks N`. CI: every leg, the iOS simulator with the fixtures copied into the container.
  A new rule needs a fixture line in both.
- CI (linux) boots with a broken stock pack (`--bundle=` a copy holding the `blocks` fixture) and requires exit 1.
- `pack.py extract DUMP -o PACK.json` — the one-off that wrote `media/packs/eden/pack.json` from the K.2-2 dump.
  Kept so the derivation stays reviewable; the shipped file is hand-edited from K.2-3 on.
