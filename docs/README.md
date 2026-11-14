# Eden: World Builder — Engineering Documentation *(legacy iOS source)*

Documentation for the Eden 2.1.1 source release and its Emod port. Written to minimize
reverse-engineering: start with the overview, then jump to your subsystem.

> **A note on ids.** Tokens like `B.5b` or `K.1-4` are the maintainer's internal milestone ids, and
> `WORKING/…` paths are private design notes that are not in this repository. The facts stated
> here stand without them.

> ⚠️ **This doc set describes the original iOS/Xcode target**, which is no longer
> built or tested. The live project is the native port ([`../native/`](../native/)
> over the shared seam/shim layer in [`../port/`](../port/)); the port's reference docs are
> [`../port/docs/`](../port/docs/README.md), organized to mirror this set so each file there reads
> as a delta against the file here. Mentions of "web" or "the web port" in these files describe the
> Emscripten build that pioneered the port and was retired on 2026-10-13
> ([web-port-archive.md](web-port-archive.md)).
>
> These files remain the authority on **engine internals** — voxel storage, the
> `.eden` format, meshing, worldgen, the coordinate conventions — and the port docs
> cross-reference them directly. Read them as iOS-era in *framing* only: build
> instructions, platform assumptions (UIKit, CADisplayLink, a real ES 1.1 context, touch-only
> input) and anything implying `Classes/` is pristine are stale. Where a file matters, it says what
> the port does instead ("*Port:*" notes).
>
> **What is compiled now** is `port/tools/engine-sources.txt` (every engine file the build uses, stock and
> post-import, one per line; the stock iOS project and the never-compiled stock files are in `archive/stock-unbuilt/`). The excluded stock shell
> (`main.m`, `EdenAppDelegate`, `EdenViewController`, `EAGLView`) creates a real ES 1.1 context and a
> CADisplayLink loop; it is kept as the reference for the retina/`IS_IPAD` metric logic, and
> [execution-flow.md](execution-flow.md) describes the native loop that replaces it.
>
> `Classes/` became editable on 2026-07-25 (the web port's never-edit rule was retired;
> the stock 2.1.1 source is the public [BarrelDevelopment/EdenWorldBuilder](https://github.com/BarrelDevelopment/EdenWorldBuilder) repository, the thing to compare against). **Engine changes must update the
> matching file here in the same commit** — otherwise this set silently stops being true.

## Index

### Orientation
- [architecture-overview.md](architecture-overview.md) — the big picture, ownership
  graph, the three core architectural ideas, reading order.
- [conventions-and-pitfalls.md](conventions-and-pitfalls.md) — **required reading**:
  the `(x,z,y)` convention, coordinate spaces, technical debt, assumptions,
  limitations, change-impact table.
- [execution-flow.md](execution-flow.md) — boot, frame loop, game-mode state machine,
  threading model.

### Core systems
- [world-and-terrain.md](world-and-terrain.md) — voxel storage, the toroidal window,
  block edit operations, fire/TNT dynamics, streaming.
- [rendering.md](rendering.md) — the pipeline (stock GL ES 1.1; the port's `R::` renderer), chunk meshing, render passes,
  vertex formats, culling.
- [terrain-generation.md](terrain-generation.md) — the offline default-world
  generator, runtime flat gen, bundle streaming.
- [eden-file-format.md](eden-file-format.md) — the `.eden` binary format (header,
  columns, creatures, directory, RLE variant, version history).
- [emod-file-format.md](emod-file-format.md) — **normative spec** of the `.emod` container
  (this fork's append-only, per-column-compressed world file; Stage S). Played by the engine since
  S.4 (S.8: the only world format; `.eden` is import/convert/export only); reference tool `port/tools/emod.py`.
- [save-load.md](save-load.md) — FileManager pipeline, `.emod` saves, autosave triggers,
  `.eden` import/convert/export.

### Gameplay & presentation
- [player-input-camera.md](player-input-camera.md) — touches (and how SDL input becomes them),
  picking raycast, physics/collision, portals, fly mode.
- [entities-and-creatures.md](entities-and-creatures.md) — the creature system.
- [lighting-liquids-effects.md](lighting-liquids-effects.md) — colored lights,
  water/lava, portals, fireworks, particles, sky colors.
- [ui.md](ui.md) — HUD modes, menu system, GL widget kit, Settings, New World, Get Worlds.
- [console.md](console.md) — *Emod only:* the command-block language shared by blocks and typed
  console lines (parse options, the anchor, `runLine`, jobs, the verb table; Stage C).
- [console-reference.md](console-reference.md) — *Emod only:* every verb, variable and coordinate form for people
  (tables generated from the binary by `port/tools/console-reference.py`).
- [resources-and-audio.md](resources-and-audio.md) — atlases, textures, sound engine.
- [scripting.md](scripting.md) — *Emod only:* sandboxed Lua mod scripts (K.4): the sandbox, budgets, the
  `eden` API over the command-block verbs, events, the `lua`/`scripts`/`mod` console verbs.

### Ecosystem
- [networking.md](networking.md) — world sharing: the stock client, the port's Get Worlds/Share,
  the edengame.net protocol.
- [third-party.md](third-party.md) — vendored libraries and repo-root miscellany.
- [engine-vs-game.md](engine-vs-game.md) — reusable vs. game-specific code, seams,
  refactoring/porting roadmap.
- [web-port-archive.md](web-port-archive.md) — the retired Emscripten/WASM build: where it is
  (tag `web-port-final`), what was removed, which native check replaced each web test.
- [`reference/`](reference/) — original material, not docs of this tree: the format notes
  `Eden_file_format.txt` and `MROB.txt`, `EdenDesignDoc.pdf`, `sound_design_draft.pdf`,
  `pitofoldcode.txt`, the GLU `la-map.c`, a 2010 1.0 promo-code list.

## Provenance & confidence
These documents were produced by close reading of the 2.1.1 source (July 2026),
cross-checked against the in-repo format docs (`reference/MROB.txt`,
`reference/Eden_file_format.txt`) and the Xcode project, and have been kept current with the port
since. Statements of uncertain confidence are flagged inline
("confidence: medium", "verify in …"). File/line references use the current tree;
line numbers will drift with edits — prefer the symbol names for grepping.
