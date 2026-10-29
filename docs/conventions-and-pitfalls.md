# Conventions, Technical Debt, Assumptions & Limitations

Read this before writing any code. These are the implicit rules the codebase follows
(or violates consistently), plus the traps that bite newcomers.

## Coordinate & naming conventions

1. **Argument order is `(x, z, y)` and y is UP.**
   Nearly every terrain function — `getLand(x,z,y)`, `setLand(x,z,y,…)`,
   `buildBlock(x,z,y)`, `addlight(x,z,y,…)` — takes horizontal x, horizontal z,
   *then* vertical y. But `Vector` fields are `.x/.y/.z` with `.y` up, `Point3D` is
   `{x,y,z}`, chunk indices go `(cx, cy, cz)` in `threeToOne`, and bounds arrays are
   `[x, y, z, x2, y2, z2]`. **Every off-by-one-axis bug in this codebase comes from
   here.** When in doubt, read the callee's loop body.
2. **Storage order**: within a chunk, `CC(x,z,y) = x·256 + z·16 + y` (y fastest —
   vertical strips contiguous). `blockarray`: `x·g_xz_stride + z·g_world_height + y` (the height is runtime — see world-and-terrain.md).
   The RLE bundle transposes to `CC(y,z,x)`.
3. **Two coordinate spaces**: absolute world blocks (≈65,000 near spawn) for logic
   and storage; render space rebased by `fm->chunkOffsetX/Z · 16` for GL. Terrain
   vertex data is additionally in quarter-block units (×4 shorts, ×0.25 modelview).
4. **`IS_IPAD` means "2× UI scale"**, and is set for Retina iPhones too
   (stock `EAGLView -initWithCoder:`). `IS_WIDESCREEN` = iPhone 5. Screen metrics are globals.
   **MODIFIED FROM STOCK (2026-07-31), and the change is worth understanding before you read any
   layout code**: `SCREEN_WIDTH`/`SCREEN_HEIGHT` were *device constants* here — one of
   480×320 / 568×320, picked once in `EAGLView -initWithCoder:` and never changed again. They are
   now a *derived layout coordinate system* that can change while the game runs. On the stock iOS
   target nothing moves; on every port target (iOS included) the point space comes from the real
   window aspect and a UI-scale setting, so a desktop window gets a bigger point space and a
   proportionally smaller HUD instead of the iPhone-5 layout scaled up 4×. Two consequences for
   anyone touching `Classes/`:
   - The rect arithmetic that used to sit in `Hud::Hud()` / `Menu::Menu()` now lives in
     `Hud::layoutForScreen()` / `Menu::layoutForScreen()` and **must stay idempotent** — it can be
     re-run at any time. In particular the file-static margins in `Hud.cpp` (`marginLeft2` and
     friends) are *mutated* by that code and are reset at the top of the method for exactly this
     reason. `Input::screenMetricsChanged()` is the same idea for `Input::scr_width/scr_height`,
     which it now *reads* from `SCREEN_*` instead of re-deriving from device constants.
   - `IS_WIDESCREEN` no longer means "this is an iPhone 5". It means "there is more width here than
     the 480-point layout was drawn for", which is what all ~10 of its branches in `Classes/`
     actually key off.
   The selection logic itself is platform detection and stays in the port's seam
   (`port/src/seam/DisplayProfile.cpp`); only the layout generalisation is in the engine.
5. **Singletons as static members**: `World::getWorld`, `Resources::getResources` are
   public static *pointers*, assigned in constructors. `Input::getInput()` is a
   function. Hot data are C globals (`blockarray`, `colorTable`, `blockinfo`…).
6. **Obj-C++ everywhere**: `.mm` files mix C++ classes (game logic) with ObjC
   (Foundation I/O, UIKit). Manual retain/release (pre-ARC). ObjC message-syntax
   remnants appear inside comments from a past ObjC→C++ conversion — large comment
   blocks of `[obj method:...]` code are the *old* implementation kept for reference.
7. **`rt` prefix** on `TerrainChunk` fields = "render thread copy" (double-buffer
   leftovers). `c` suffix on globals (`chunkTablec`, `getLandc`) ≈ "C/global fast
   path".
8. **Return conventions**: `-1` = invalid/out-of-bounds block type (distinct from
   air = 0); color 0 = unpainted; hashmap key 0 = invalid.
9. **Dead code is kept, commented out.** The codebase is its own version control.
   Do not delete these blocks in refactors without checking they're truly dead —
   some (face merging, custom blocks) document intended future features and the
   author left activation instructions ("UNCOMMENT FOR FACE MERGING").

## Technical debt register (the honest list)

- **Commented-out code** is archaeology (CLAUDE.md rule 9, relaxed in SM.11, 2026-10-29): leave it in unrelated
  changes, but `pristine-engine` keeps the original, so a function you are already editing may lose the
  commented-out code inside it in the same commit. No sweeps.
- **Threading scars**: a background chunk-mesher and streaming thread were removed
  after race conditions (the author's `//issue #1/#2/#3` comments in `Terrain.cpp`
  mark the exact hazards; `archive/crashes/` holds the resulting App Store crash logs). The
  double-buffered chunk fields, `chunksToUpdateImmediatley`, and the sanity-check
  printouts are all scaffolding of that design. The load pthread survives, unlocked. *(Port:
  a later worker pool for meshing and column decode, `MeshPool`, only the retired web build compiled; SM.8
  deleted it, so every target meshes inline. It is in git history.)*
- **Global mutable state**: FileManager's static file handle/header, Terrain's file
  statics, Model's static pools — none of it is re-entrant; exactly one world at a
  time, one of each system.
- **Vestigial octree** (`TreeNode`/`addToTree`/`renderTree`) — now a linear scan;
  the tree is still built and freed for nothing.
- **Disabled systems kept in-tree**: face merging (meshing), `TYPE_CUSTOM`
  half-blocks, the 1.x shadow columns (`getShadow` returns 1.0; superseded by H.2's
  `Classes/Shadow.cpp`, which is live), FileArchive compression,
  occlusion queries, noise-seeded runtime worldgen,
  biome-marker PNG input, `WetNode` liquid heights, TestFlight.
- **Copy-paste blocks**: `refreshChunksInRadius` is 7 hand-unrolled loops;
  `destroyBlock`/`explodeBlock` are near-duplicates; door/portal/golden-cube render
  loops share 80% of their bodies; `getRampType`/`getRampType2` differ by one branch.
  When fixing a bug in one, grep for its twin.
- **Magic numbers**: streaming threshold `count>140`, progress `/324`, burn cap 300,
  spawn tables, hard-coded layout rects, memory thresholds — all inline. *(Port: the first two now
  scale with the runtime window — `count*324>140*ncols`, progress over `CHUNKS_PER_SIDE²` — B.5b-2.)*
- **The window origin has one formula** (B.5b-1a, 2026-10-11): `windowOriginChunk(pos)` in
  `Terrain.h`, corner-centred. A site that computes `pos/16 - T_RADIUS` itself disagrees with the
  streamer by one row half the time, and the streamer then re-reads that row. The outer ring of the
  window is never meshed (data for its neighbours), so an "is it drawn" or "is it settled" check over
  `chunksToUpdate` must skip it (`slotOnOuterRing`, as `f3_frameEnd` does).
- **Error handling** is `printg`/`printf` logging plus soldier-on; in stock, file I/O failures
  during save could silently corrupt worlds (no atomic writes). *(Port: `.emod` saves are atomic —
  one commit record per save, [save-load.md](save-load.md).)*
- **Typos are API**: `chunksToUpdateImmediatley`, `criticle errror` (and `Frustrum` in the
  `Frustum.{h,mm}` header comments) — keep them; renaming breaks greps against history and the community's knowledge.
- **Stock UB the sanitizer leg found and fixed** (G.2s, 2026-10-24; `-DEDEN_SANITIZE=ON`, CI job `sanitize`):
  `Hud::m_jump`/`take_screenshot` never initialised (heap garbage read on the first frame); `Hud::renderBlockAndBorder`
  copied 36 floats from a 24-float array; `Player` indexed `blockinfo[-1]` when `getLand` answered invalid (flying above
  the world); `CPVRTModelPOD::SetFrame` (now `CreatureModel::SetFrame`) let the last frame blend one key past the animation arrays; `Model.cpp`'s
  `&((unsigned short*)0)[n]` VBO offset. New code: a `getLand`/`getBlock` result can be `-1`, so check it before
  indexing a block table.
- **`Hud::paintColor` was never initialised** (found by G.5b-0's `--scene-shot`, 2026-10-25): heap garbage
  drew the paint button in a different colour each launch until the first pick. The class of bug is
  general: Objective-C `alloc` zeroed every ivar, a C++ `new` does not, so any member the stock `init`
  never assigned is garbage since the de-ObjC conversion. `Hud::Hud` now sets it to 0. When a capture
  differs run to run, suspect this before the renderer.
- **Engine randomness goes through one hook** (G.5b-0): `eden_prefix.h` renames every engine
  `arc4random()` to `eden_arc4random()` (`port/src/shim/foundation/platform_shims.cpp`), a PCG32 a harness
  can pin with `eden_rng_seed()`; unpinned it is `arc4random()`. New engine code that wants a random
  number should call `arc4random()` so captures stay reproducible. (`rand()`/`random()` are not routed.)

## Assumptions the code makes

- Exactly one GL context, ES 1.1, main-thread-only. *(Port: still one context on the main thread;
  it is ES 3.0 or desktop GL, and the engine draws through `R::` (`Classes/Renderer.h`); since G.5b-7 no
  engine or host file calls GL, gated by `port/tools/gl-call-gate.py`.)*
- 32-bit-era struct layout, little-endian, for every byte written to disk
  (`WorldFileHeader` = 192 bytes, `ColumnIndex` = 16, `EntityData` = 60). Compiling
  for a platform that pads differently silently corrupts save files.
- `block8` is *signed* (types ≤ 127; negative = corrupt sentinel), `color8` unsigned.
- World height exactly 64; chunk size exactly 16; window 18×18 (`T_SIZE` etc. are
  `#define`s that many literals secretly depend on). *(Port: height and window are both runtime
  now — 2026-08-06 and B.5b-2, 2026-10-12; `rebuildList[13000]` was one such literal.)*
- Chunk coordinates fit in 15 bits (`twoToOne`), non-negative.
- The bundled map exists and matches `DEFAULT_LEVEL_SEED` worlds. *(Port: the bundle is `Eden.emod`;
  the repo-root `Eden.eden` is its source and is not shipped.)*
- Frame time ≈ 1/60 s; physics constants are tuned for it (with a 50 ms clamp).
- Documents directory contains only world files and their PNGs (the world browser
  header-parses every file in it).

## Known limitations

- 1296 resident chunks at 64z, 5184 at 256z (`CHUNKS_PER_SIDE²·CHUNKS_PER_COLUMN`) at the default radius 9; 16,384 at radius 16, 256z.
- One liquid update queue globally; heavy flow re-meshes chunks every tick.
- 200 persisted creature slots in a 64z save (400 in a 256z one, and the real number is
  **derived from the file**, not the version — a save can legitimately have none); 300 live;
  1000 portals; 80 fireworks; 32k vertices per chunk stream (excess silently dropped).
- No multiplayer of any kind; "sharing" is file upload/download. *(Port: Stage MP adds a client —
  [networking.md](networking.md).)*
- *(Port)* The native text raster (`TextRaster_native.cpp`) draws ASCII only: "•", "·", "…" and
  non-ASCII remote names/MOTDs come out as mojibake, so UI strings use `*`, ", " and "...".
- Worlds from 2.2.7+ App Store builds ("New Dawn", 256-block-tall, header `version` 5 or 6) load
  and play natively as of 2026-08-06 — see [world-and-terrain.md](world-and-terrain.md) "Runtime
  world height". They never loaded "height-truncated" as the README claimed: before the Stage-0
  guard this build read every column past the first at the wrong stride (garbage) and the next
  autosave silently overwrote the file at 64 blocks tall, destroying it. A version **above** 6 is
  still refused outright rather than guessed at.
  New World offers both heights (New Dawn 256 is its default, `NewWorldMenu.cpp`), and a 256z world
  saves keeping its own version.
- Stock: backgrounding does not save; only streaming/warp/exit do. *(Port: also on app background,
  on ring eviction of a modified column and on the 60 s autosave — [save-load.md](save-load.md).)*

## When you change X, also check Y

| If you change… | Also check… |
|---|---|
| Block type enum / tables | Save compat (raw bytes on disk!), `enum BLOCK_TYPES` (`Constants.h`), `blockinfo`, `blockTypeFaces`, `blockColor` (`Globals.cpp`), atlas tile, `blockTntMap`, `hudBlocksMap`, HUD picker list (`Hud.cpp`), and the type's flags in meshing/physics/fire. **The id space is full at 127** (`block8` is signed; ids 112–127 since Stage D.2a): there is no room to append another type |
| Anything `Hud::Hud()`/`Menu::Menu()` calls that reads `World::getWorld->…` | `World::World()` builds `hud` before `fm` (and `menu` after both); a C++ `new` does not zero, so an unset member is garbage on Windows and happens to be 0 on Linux. `fm` is nulled at the top of the ctor; null-check anything else you read |
| `Player::update`'s tail (after `move()`) | It lives in `postMove`, shared with the noclip branch (`noclipUpdate`, which never calls `move()`); change it there and `--fly-selftest` (`--fly-mutate=move` must fail it) |
| A new damage source | Route it through `Player::takeDamage`, or noclip stops being invulnerable (it returns early there) |
| A trigger in `Player::move()`/`vertc` (portals, pickups, ladders, liquids, lava, footsteps) | Noclip skips `move()` entirely, so it never fires there; decide if that is intended |
| The jump/crouch HUD slots or `Hud::m_flyup/m_flydown` | While flying they are fly up/down, not jump/crouch; the cycle button is `rflyhit`; `--fly-selftest` taps them via `eden_tap_hud_button_begin(8/9/10)` |
| `colorTable` generation | Every painted block in every existing world re-tints |
| A block type's flags (`IS_LIQUID`/`IS_OBJECT`) or `TYPE_CLOUD`/`GLASS`/`WEAVE` | Whether it casts a baked shadow (`g_shadow_caster[]`, `Classes/Shadow.cpp`, built from the flags), and so CI's "geometry 64z shadows" `full` if the bundled map holds it |
| Touch arbitration in `Player::processInput` (`inuse`, `movecam`, `placeBlock`, the `num > 1` rule) or the touch look (Stage L) | `Classes/TouchLook` owns the arithmetic only; the slop must stay ONE threshold for tap and look (a look below the tap threshold turns the camera and then builds where the world moved to); the release loop runs before the look loop, so a look's last frame is consumed in `touchlook_end`; `TOUCH_PT_SCALE` is written in `eden_display_apply()` beside `SCREEN_WIDTH`. Gate: `--look-selftest` and its `--look-mutate=` legs; then a device (docs/player-input-camera.md "Touch look") |
| `CHUNK_SIZE` | `SIZEOF_COLUMN` (file format!), progress divisors, `INDICES_MAX` sizing |
| A save site, or anything that opens `<docs>/<world_name>` (MP.3a) | The multiplayer session redirect: `FileManager::sessionWorld` → `commitSession` (commit to RAM). A session's name `mp:<host>:<port>` is not a path; a new file open must check `sessionWorld` (`--mp-replay`'s last check fails if a session writes to the save directory) |
| A new path that writes `pblocks` on landing, or the flat branch of `generateColumn` | `MpWorld::applyDelta` must stay after the recipe and before the `GBLOCK` copy/`addChunk`; `--mp-replay` leg 1 |
| A new world-changing feature (block-painting creature, console verb, script op, inventory) | Is it an E11 gate in a session (`MpWorld::active()`; engine plan §11)? An edit nobody else sees is a silent desync |
| `Terrain::setCellState`, `CmdScript::setCell`, or `World::update`'s order around step 5c (MP.3b) | Both callers share the write sequence (`--cmds-selftest`, `--console-selftest`, `--mp-replay` legs 3-4); remote edits must stay after local ones and before meshing, and the two modal early-outs must keep calling `MpWorld::update` |
| A mine/build path a remote edit could take (MP.3b/MP.4) | A relayed edit must never reach an intent site that sends (echo); remote applies call `setCellState`/engine primitives, never `MpWorld::intent*` (`--mp-replay` leg 6, `--mp-mutate=echo`) |
| A new player edit path (a tool, a gesture, a world-changing move like the portal exit) | Is it an intent site? In a session it must go through `MpWorld::intent*` (or send its own line), else reconcile reverts it on the next `REGION` and nobody else sees it |
| A queue, set or buffer the server's data can grow (MP) | Count it in `MpWorld::heldBytes()` (or the session's own byte count), or it escapes the 256 MB `TooMuchData` cap (Q.6; `--mp-selftest` `legRobustness`, `--mp-replay` leg 4's held-bytes check) |
| `BurnNode` or the burn list's expiry in `Terrain::update` | `mpLit` (MP.4): the follow-up mine goes out only on a timed expiry with the cell still solid (`--mp-replay` leg 6's TNT and restore cases) |
| `T_SIZE`/`T_RADIUS`/`CHUNKS_PER_SIDE` (window; **runtime** since B.5b-2) | **not** the file format (no save field depends on it; 2026-10-11; `GSIZE` is the literal 2880 since B.5g). Same rules as the height: anything sized at compile time uses `CHUNKS_PER_SIDE_MAX`/`T_SIZE_MAX`; set by `eden_set_window_radius()` before `allocateMemory`; per-frame loops over the window cost (2R)²·bands. Inventory: `WORKING/render-distance-runtime-plan-2026-10-11.md` §2 |
| `T_HEIGHT`/`CHUNKS_PER_COLUMN` (now **runtime** globals) | Anything sized at COMPILE time must use `T_HEIGHT_MAX`/`CHUNKS_PER_COLUMN_MAX`/`MAX_CREATURES_SAVED_MAX` instead — a file-scope VLA is a build error, a stack one is not |
| `WorldFileHeader`/`EntityData`/`ColumnIndex` | All existing saves + the bundled `Eden.emod`/`Eden.eden` + the upload server; the header is also an `.emod` `WORLD_HEADER` record checked against `sizeof(WorldFileHeader)` |
| The `.emod` container (`EdenWorldStore`) | [emod-file-format.md](emod-file-format.md) is normative: change it, `port/tools/emod.py` and the fixture pack `port/tools/fixtures/emod/` together (`--emod-selftest` byte-compares them on every target) |
| zstd (version, level, compile flags) | Byte-identical output on every target (the fixture gate); re-bake `Eden.emod` with `emod.py bake-default` |
| `WorldTrailer` records (`SGN1`/`CMB1`) | `static_assert`ed at 120/528 bytes in `WorldTrailer.h`; existing saves, `emod.py trailer` |
| Meshing (`rebuild2`) | Counting pass and fill pass must agree exactly; `prepareVBO` swap; the CI geometry checksums (`.github/scripts/check-stage1.py`: re-pin only for a deliberate change, and say so in the commit) |
| Shade formula or `SHADOW_REACH_*` (`Shadow.cpp`) | The pinned Classic `full` in `check-stage1.py --shadows` |
| Code that binds a `Texture2D`'s `name` and draws it with its own UVs | Since Q.2 uploads are NPOT (no POT canvas unless `sizeToFit`): the art must be POT or the UVs in image units. Since Q.4 a grey+alpha PNG is LA88, and desktop core turns L/A/LA into R8/RG8 + a per-texture swizzle, so re-uploading RGBA into a name that held LA would keep the swizzle (`port/docs/history/gl-shim.md`) |
| Anything that calls `Resources::getTex` before a world is entered | Since Q.3 the game set (106 textures) loads at world entry (`DEFER_GAME_TEXTURES`); `getTex` lazy-loads the whole set if it is empty, which costs the decode at the menu. A selftest that needs game art calls `loadGameAssets()` first (`--recolor-selftest` does) |
| A GL entry point the GL backend (`port/src/render/gl/`) starts calling | Linux and Windows resolve every GL call through `port/src/render/gl/gl_loader_native.h`: add it to BOTH lists (the X-macro and the `#define` rename), or Windows fails to link (Q.5 added `glMapBufferRange`/`glUnmapBuffer`) |
| A GL call in the backend (`port/src/render/gl/`) that changes or depends on the texture binding, blend, depth test/write, cull or polygon offset | Go through `gl_state`'s ACTUAL cache (`bindTextureNow`, `depthWriteNow`, `applyRenderer`), never raw `glBindTexture`/`glDepthMask`: the cache would go stale and the next draw elide a bind it needs. Anywhere else a GL call is a compile error (the engine's `<OpenGLES/ES1/gl.h>` has no functions) or a `gl-call-gate.py` failure. `--render-selftest-gl`'s R3 lines; `--scene-shot` A/B |
| `glMatrixMode`/push/pop/load semantics, or a draw site moving to `R::` | The stacks are `R::mv()/proj()/uv()` (R2); a ported site states everything per draw (`port/docs/renderer.md`). Gates: `--scene-shot` byte-identical against the tree before the change, `--render-selftest` (CI) and `-gl` |
| 2D code that sets a colour / blend / depth / cull / texture for the draws after it (HUD, menus, kits, `Texture2D` callers) | That state is the **pen** (`Pen::` in `Graphics.h`, G.5b-7); only `Texture2D` quads, `Pen::strip` and the preview cubes' tint read it, an `R::` draw never does. Keep the order of pen calls when moving code (a later `Texture2D` draw sees whatever the last call set; `Terrain`'s exit hand-offs set it for the HUD) and diff `--state-trace` before/after |
| A setting (a `kSettings[]` row) | `docs/console-reference.md`'s generated tables: rerun `port/tools/console-reference.py native/build/eden_native` or `--console-selftest` fails (G.5b-4 bit it); Settings › that tab's `--shot` page changes |
| The world fog (colour, start/end, `setZFAR`) | G.5b-6: the colour goes through `Graphics::setFogColor` (Terrain) and the terrain passes state it with `Graphics::applyWorldFog()`; creatures (`RenderModels`) use `applyWorldFog()` too since G.5b-7. Fog is off from the sky to the end of pass 1 (portal swirls unfogged, as stock) |
| Lighting through `R::drawLit`, or `LightSet` | `--lit-oracle` (Mac, window) compares against real fixed-function GL within 2/255; *Original lighting* off must stay byte-identical (`--scene-shot` with `--set=original_lighting=0`). A light loaded under a non-identity matrix is transformed by it (BlockBreak's is, `port/docs/renderer.md` § Original lighting) |
| A creature model or its draw (`.edm` layout, `Model.cpp` `LoadVbos`/`DrawModel`) | `meshFormat` builds one `R::VertexFormat` per mesh from the position stride (it warns once if another attribute's differs) and a bone batch over 16 matrices is truncated (warned): `pod2edm.py` must keep both true. Gate: `--scene-shot` A/B, whose log line `[eden-scene] … models {"nNumMesh":…}` must show the models loaded (an A/B binary run outside its build dir loads none: `EDEN_BUNDLE`, `port/docs/history/gl-shim.md`), plus `--creature-selftest` |
| A draw from client memory (`R::Geometry` with `vmem`/`imem`, no `Buf`) | The backend copies it into its stream rings (Q.5): interleaved arrays share one copy, only `[min,max]` vertices go up and indices are rebased. VBO indices with client arrays is unsupported (draws wrong geometry); `--shot` UI regions and `TESTERS/q5-shim-streaming/photos.sh` are the visual check |
| A new engine source file | `port/tools/engine-sources.txt`; a seam replacement for a stock file means deleting that file's line from `engine-sources.txt` (archive the file) and adding the seam to `native/CMakeLists.txt` |
| Update order in `World::update` | Streaming/save/mesh/upload dependencies (see execution-flow.md) |
| Anything in a `.h` under `Classes/` | Massive rebuild — headers include each other liberally (Terrain.h ↔ World.h ↔ Player.h circular, broken by forward decls) |
| A preference (a new key, a setting, keybind, console history, MP profile) | Since G.2b-1 every target reads and writes `eden_prefs_*` (`port/src/shim/foundation/eden_prefs.h`; one flat file, `port/src/seam/Prefs_native.cpp`); nothing calls `NSUserDefaults` any more. Keys must not contain TAB/CR/LF, string values not CR/LF. Every `--*` mode runs prefs-ephemeral (`eden_prefs_set_ephemeral`, X.21/Q.1 F13) on every target, so a selftest must not expect a value to persist; `--base-selftest` covers the store |
| Touch delivery (`Input::touches*`, `itouch::touch_id`, anything that reads `isReal`) | G.2b-1: a touch is `EdenTouch` (`Classes/Input.h`), one heap object per live pointer owned by `Input.cpp`'s `activeTouches()`; slots match it by pointer. Don't key on `identity` (0 is a real finger), don't free a touch the engine still holds before its `touchesEnded`/`Cancelled`. Gates: `--touch-selftest` (+ `--window=1024x768 --touch-profile`), `--look-selftest` and its 10 mutations, `--fly-selftest` (+ `--fly-mutate=move`) |
| A `Texture2D` constructor or a recolour source | G.2b-1 (plan D1/D11): paths and label text are `const std::string&` (never add a `const char*` overload: the raw-data constructors take `const void*`), images are `const EdenImage*` that the caller still owns and frees (`eden_image_free`), a font is a `float` point size. `--recolor-selftest`, `--leak-probe`, `--ui-selftest` |
| An engine header (`Classes/*.h`, root `Lighting.h`), or a new one | Every engine header is plain C++ (G.2b-2), and since G.3 nothing compiles ObjC except the Apple walls in `port/src/seam/` and there is no Foundation/UIKit on any include path, so an `@interface`, `NSString*` or `#import <Foundation/…>`/`<UIKit/…>` fails the build on every leg (`Classes/HeaderProbe.cpp`, the G.2b-2 proof, went in G.3). `port/tools/objc-surface.py --check` fails a new `.mm`/`.m` |
| A world name, file name or status-line text in the engine | G.2b-2: they are `std::string` (`Terrain::world_name`, `Menu::session_world_name`, `WorldNode::display_name/file_name`, `statusbar::setStatus/current`, every `FileManager.h` name). `""` is "none" where `nil` was; `statusbar::current()` returns null when nothing shows. Where the old code relied on `stringWithUTF8String:` returning nil for bad UTF-8 (Menu's rename, Console's text, `Menu`'s new-world name), the check is now `utf8_valid()` (`StrUtil.h`, Apple's strict rules); keep one wherever outside text becomes a name. A `WorldNode` is `new`/`delete`. `--base-selftest` covers `utf8_valid`; save gates in `native/README.md` |
| File I/O or a header string field in the engine (`FileManager`, `FileManagerHelper`, since G.2b-4) | It is `EdenFile` / `eden_fs_*` / `eden_bundle_path` (`port/src/shim/foundation/eden_fs.h`): a missing file opens as null, a bundle miss is `""`, nothing makes a `.bak`. A header `name`/`hash` that was written with `getCString:maxLength:` (the save's name and hash, `setImageHash`) is now `eden_cstr_copy(dst, 49/33, s)` (empty on overflow, Apple's rule, D4); the rename paths (`emodSetHeaderName`, `renameWorld`, `applyWorldProperties`) were always plain C `strncpy` (truncating). Keep each as it is, or that field's bytes change. A string that came from outside and may not be UTF-8 goes through `FileManager.cpp`'s `fm_ns` (the old `nil`). Gates: the save/format set in `native/README.md` and a byte A/B (`TESTERS/g2b4/ab.sh` shape) |

### Q.1a notes (2026-10-21)
- **Q.1a:** `threeToOne` doesn't wrap `cy`, so `addToUpdateList*` now reject `cy` outside `[0,CHUNKS_PER_COLUMN)` (F16). `Portal::paintPortal` takes `(x,y,z)` with y vertical, unlike the Terrain `(x,z,y)` convention; `paintBlock` passed them swapped (F28).
