# Rendering Pipeline

## Purpose
Everything drawn on screen: GL state management, chunk meshing, the two terrain
passes, special objects, sky, and the per-vertex data formats.

## Fixed constraints
- **OpenGL ES 1.1, fixed-function.** The engine has no shaders of its own. *(Port: since G.5b-7 (2026-10-28) the engine
  makes no GL call at all: it draws through its own renderer `R::` ([renderer.md](../port/docs/renderer.md)),
  whose five programs reproduce the fixed-function semantics the engine was written against (fog, texture
  matrix, alpha test, point attenuation, matrix-palette skinning, `GL_LIGHT*` behind *Original lighting*).
  The ES 1.1 translator that served it until then is history: [gl-shim.md](../port/docs/history/gl-shim.md).)*
- Lighting effects are baked into vertex colors. GL lighting (`GL_LIGHT0`/`GL_LIGHT1`) is used only
  for immediate-mode objects: doors and the golden cube's env-mapped specular highlight
  (`Terrain.cpp`), creatures (`Model.cpp`), block-break debris (`BlockBreak.cpp`) and the rising
  firework cube (`Firework.cpp`).
  *(Port, G.5b-4: debris and the firework cube are `R::drawLit` draws with an `R::LightSet`, lit per
  vertex as 2.1 did only while Settings › Video › *Original lighting* is on (default); off, they draw
  unlit. Two stock details this exposed: debris was **flat shaded** (`initGraphics` sets `GL_FLAT`;
  `BlockBreak`'s `glShadeModel(GL_SMOOTH)` comes after its draw), and its light position is loaded while
  the matrix mode is `GL_TEXTURE`, so the effects pass's modelview places it far off in eye space, a
  nearly directional light. The port draws everything else smooth (the shim always ignored `glShadeModel`).
  `port/docs/renderer.md` § Original lighting; gate `--lit-oracle`. G.5b-6 did the same for doors (LIGHT0
  at the eye, ambient .3 / diffuse .7, night .17 / .3) and golden cubes (LIGHT1 alone at eye (1,1,0),
  .3 / .3, night .15 / .15, white specular, shininess 100), both smooth. **Terrain stays smooth**: 2.1.1 drew
  the chunk passes under `GL_FLAT` too (only the objects section switched to smooth), but the port has
  drawn them smooth since it began, stock faces are mostly one colour per face anyway, and the port's
  *Smooth* shadows (`lighting-liquids-effects.md`) are per-vertex and need the interpolation.)*
- Stock: a single 16-bit depth buffer, RGBA8 color, the depth attachment discarded before present
  (`EAGLView`). *Port:* 24-bit depth (`gl_context_native.cpp`). SDL's swap does not discard, so on
  iOS `eden_native_gl_present` invalidates the view FBO's depth before `SDL_GL_SwapWindow` and
  `eden_scene_pass_end` invalidates the scaled scene FBO's before its composite (P.11,
  `glInvalidateFramebuffer`; ≈ 12.6 MB/frame of tile store on an Air 2). Desktop does not.

## Important files
- `Classes/Graphics.cpp/.h` — static GL state helpers + the vertex struct definitions.
- `Classes/Renderer.h` — the engine-owned renderer (Stage G.5, from 2026-10-25): matrix stacks the ES 1.1
  calls forwarded into, explicit per-draw state, five programs. The 2D brackets and fill (G.5b-1), every
  `Texture2D` upload and quad draw, the screenshot/picking/frustum readbacks (G.5b-3), the effects pass
  (Fire, BlockBreak, Firework, the player's held cube), the signs pass (G.5b-4), the creatures (G.5b-5) and
  the terrain passes (G.5b-6: chunks, object batches, sky, flowers, the brackets and fog in `Graphics`) are
  on it, and since G.5b-7 everything else: the HUD's own quads (`Pen::strip`) and the 2D code's immediate-mode
  state, kept engine-side as the **pen** (`Pen::` in `Graphics.h`); `port/docs/renderer.md`.
- `Classes/TerrainChunk.cpp` — mesh generation (`rebuild2`), VBO upload (`prepareVBO`),
  chunk draw (`render`, `render2`).
- `Classes/Terrain.cpp` — frame-level terrain passes, object batches, sky.
- `Classes/Geometry.cpp` — the canonical cube/ramp/side/liquid vertex+UV tables and the
  shared `allIndices[32768]` identity index array.
- `Classes/Frustum.cpp` — plane extraction + AABB test (`ViewTestAABB`).
- `Classes/Camera.cpp` — view matrix from player state.
- `Classes/glu.h`, `project.c` — GLU port used for `gluPerspective`/`gluUnProject`.

## Vertex formats (`Graphics.h`; `vertexObject` is in `Vector.h`)
- `vertexStructSmall` — terrain: `GLshort position[3]` + pad, `GLubyte colors[4]`,
  `GLshort texs[2]`. Positions are **premultiplied by 4** (quarter-block units) and
  chunk-local; the chunk's `render()` translates by `(pbounds - chunkOffset*16)*4`
  (`chunkOffset` follows every window shift since B.5b-1a; any value works because meshes are
  chunk-local, as long as it changes between frames, never mid-frame. The window's outer ring of
  columns is never meshed and draws nothing: docs/world-and-terrain.md "Streaming" 2a)
  and `Terrain::render2` wraps everything in a ×.25 scale; pass 1 relies on
  the modelview already being scaled the same way via `Graphics::beginTerrain` (both on `R::mv()` since G.5b-6).
  UV shorts are normalized by a **texture matrix** trick: `glMatrixMode(GL_TEXTURE);
  glScalef(1, 1/ATLAS_TILES, 1)` around the terrain passes and `BlockBreak::render` (each atlas
  is a 1×`ATLAS_TILES` strip). **Modified from stock (D.2t, 2026-10-10):** stock was 1/32 with
  32-tile atlases; both grew to 64 (`Constants.h` `ATLAS_TILES`) to carry blocks 112–127, then to **128 (H.14, 2026-10-16; the PNG loader's size cap in `Texture2D.cpp` had to go 2048→4096 with it, or every tile loads half-res)** for tile 64 `TEX_GRASS_FLOWER` (block 12's top, `grass3o.png`, full colour so the mesher leaves its paint white; `port/tools/atlas-h14.py`) — atlas2's
  extra 32 tiles are transparent padding so one constant serves both passes. Every consumer reads
  it: the two passes, `BlockBreak`, `Resources::getBlockTex`/`getBlockTexShort` (normalized
  `type/ATLAS_TILES` for the HUD icons, picker and `Graphics::drawCube`). The door, portal-frame and portal-swirl immediate batches (`Terrain::render`) used to hardcode `*32` to cancel the 1/32 matrix for their own non-atlas textures; they now multiply by `ATLAS_TILES` (X.7 — at `*32` only the top half of the door texture was sampled, stretched over both blocks). The image loader's cap is
  2048 (`kMaxImageTextureSize_Eden`); at the old 1024 the 2048-row atlas was silently halved.
- `vertexObject` — float version used for per-frame immediate batches (doors, cubes,
  portals, flowers, creatures env-map).
- `vertexpStruct` / `vertexpBreak` — point-sprite particles / block-break debris.

## Graphics state helpers (`Graphics.cpp`)
`prepareScene` (clear + `gluPerspective(eden_fov_degrees, aspect, 0.012, ZFAR-25)` — the fovy
was a hard-coded `80` until Phase N Stage 2.5; it now reads the shared FOV setting
(`port/src/seam/Settings.cpp`, `> 0` guard falls back to 80).),
`beginTerrain/endTerrain` (fog on, color arrays, atlas bind, ×0.25 scale; since G.5b-6: the pass's `R::State`
(no blend, depth test + write, cull), `applyWorldFog()` → `R::setFog`, the ×0.25 on `R::mv()`, and an exit
hand-off of the state stock left, on the pen since G.5b-7 — the chunk draws state their own
texture), `setFogColor`/`applyWorldFog` (G.5b-6: the fog colour Terrain sets, kept engine-side for `R::setFog`;
`RenderModels` uses `applyWorldFog` too since G.5b-7),
`beginHud/endHud` and `prepareMenu/endMenu` (ortho 2D, on `R::begin2D`/`R::end2D` since G.5b-1), `setZFAR` (also re-derives the
linear fog band: start `ZFAR-ZFAR/1.6`, end `ZFAR-30`). Fog color tracks sky color
(`Terrain::update`). `setZFAR` ignores its argument: it picks a per-tier value (`LOW_MEM_DEVICE` 90,
`LOW_GRAPHICS` 115, else 145) and passes it through `Graphics::viewDistanceZFAR`, which applies the user's
**View distance** setting (B.5a; `view_distance` enum, `eden_view_distance` global in `Settings.cpp`:
0 = Auto = the tier value, 1..6 = 4..9 chunks). The far plane is `ZFAR-25`, so ZFAR = blocks + 25, and blocks
are capped at `eden_meshed_radius_blocks()` = `(T_RADIUS−1)·16−8`, the meshed radius (120 at the default radius 9 —
the old `VIEW_DISTANCE_MAX_BLOCKS` literal — not the 144-block resident window). **B.5b-2:** with a larger window
(the *Render distance* setting, [world-and-terrain.md](world-and-terrain.md) "Runtime window radius") Auto follows
it out: ZFAR = meshed radius + 25 (161 / 193 / 257 at radius 10 / 12 / 16), so the far plane sits on the meshed edge
and the fog ends 5 blocks short of it, as 145 does at radius 9; at radius 9 Auto is the tier value exactly as
before. **Draw-culling only**: meshing and streaming are unchanged. Changing the setting re-calls `setZFAR`
immediately.
**Graphics preset** (B.8; `graphics_preset` row, `kPresets[]` in `Settings.cpp`): Custom / Low / Default / High is a
shortcut that writes `view_distance` and `render_scale` — Low = 4 chunks + 75% (50% on iOS since FT.16),
Default = Auto + 100% (the pre-B.8 look; shots match within animation noise; on iOS the device default, 75% on
Air 2-class hardware). High = Default plus `render_radius` (and 100% render scale)
(B.5b-2): radius 16 on desktop, 10 on iOS (P.4 decision 6; T.B16 measured R10/R12 on the Air 2). Moving any of
the three knobs by hand sets the preset to Custom. It does not touch `LOW_MEM_DEVICE`/`LOW_GRAPHICS`: those pick resources at
load time and are not switchable mid-session.

## Chunk meshing — `TerrainChunk::rebuild2()` ("here be dragons")

CPU-side, runs on the main thread inside `prepareAndLoadGeometry`. Stages:

0. **Empty shortcut (modified from stock, Stage R / R.2, 2026-10-03).** If the chunk's
   `typesEmpty` bit says its type plane is all air, `rebuild2()` returns right here, after
   `clearMeshes()` and the counter resets and **before both mesher passes**, with
   `clearOldVerticesOnly=TRUE`. That is exactly the state stage 1 leaves on an all-air chunk
   (it sets nothing when every type is 0; `has_light` FALSE, `num_objects` 0, `needsVBO` TRUE),
   so this only skips the 4096-voxel scan (~4 µs Mac, ~11 µs A8X). Stage 1, when it does run, writes the bit back (`typesEmpty = !hasAnything`), which is how a chunk dug back to air
   rejoins the shortcut. Who maintains the bit is in [world-and-terrain.md](world-and-terrain.md),
   under "The per-chunk empty bit". Gates: `eden_debug_chunk_state()` (state hash + bit audit),
   `--empty-bit-selftest`, and `--empty-selfcheck`, which runs the scan anyway and counts a bit
   that was wrong.
1. **Scan pass** over `pblocks`: find lightboxes (`has_light`), clamp corrupt types to
   stone, note whether the chunk contains any `IS_ATLAS2` (transparent) blocks,
   count `StaticObject`s (doors/golden cubes/flowers/portal tops), and fill the
   `hasBlocky[y]` row-skip table.
2. **Face visibility**: for each block, 6-bit mask of faces exposed to a
   `IS_NOTSOLID` neighbour (fast path when no transparent blocks; a slower variant
   handles liquid-level and same-type-same-color suppression so adjacent water
   surfaces don't z-fight). Bottom faces at y=0 are culled; top faces at the world
   ceiling are forced visible.
3. **Counting pass**: vertices are bucketed **per face direction** into 7 buckets
   (6 axis directions + bucket 6 for the "angled" face of ramps/sides, which can't be
   directionally culled). Two independent mesh streams: stream 1 (opaque, atlas 1)
   and stream 2 (`IS_ATLAS2`: water, lava, glass, leaves, weave, trampoline pads…).
   Objects are extracted into `objects[]` instead of meshed (portal *frames* also
   emit regular cube geometry).
4. **Fill pass**: writes `vertexStructSmall`s. Per-vertex color =
   `light[rgb] × paint[rgb] × cubeColors[face]` where `cubeColors` is the fixed
   per-face shade (fake directional light), `light` comes from `calcLight`
   (skylight 0.35 at night + colored `lightarray` contribution), plus special cases:
   burned blocks half-brightness, lava/lightbox fullbright, ramps/side pieces get
   precomputed `gshadows` face shades, water alpha 145. **Top faces (`f==5`) use `light_top`**,
   which is `light` unless baked shadows are on (H.2, *modified from stock*): then the sky term is
   scaled by the face's shade (Classic: one per face; Smooth: one per corner, picked from the unit
   cube's x/z), see docs/lighting-liquids-effects.md "Baked shadows". Geometry never changes, so
   `geom` is the same in every mode and only `full` moves. Unpainted (`color==0`)
   colorable blocks (grass, TNT, brick, vine…, and since D.2t blocks 112–127) set `coloring`:
   their face swaps to the `_COLOR` atlas tile (`TEX_NEWBLOCK`→`TEX_NEWBLOCK_COLOR` for 112–127)
   drawn with paint = white, since that art carries its own colour; the other unpainted blocks
   multiply their greyscale tile by `blockColor`, and painted blocks always use the greyscale tile
   × `colorTable`. Liquid blocks rewrite the cube's y-coordinates from
   the per-level `liquidCube` table so surfaces slope toward the outflow direction
   (chooses one of the 4 `side*Texture` top orientations).
   Face-merging (greedy strips via `face_size`) exists but is **disabled** — the code
   is under "UNCOMMENT FOR FACE MERGING" comments; `size` is always 1.
5. Rebuild `face_idx[]` prefix sums; set `needsVBO`.

Empty-result chunks set `clearOldVerticesOnly` so `prepareVBO` frees GL buffers.

### Off-thread meshing (removed in SM.8, 2026-10-28)
The 2026-08-27 worker-pool mesher (`Classes/MeshPool.{h,cpp}`, `EDEN_THREADED`, the `MeshSideEffect` sink, the
per-chunk `meshJobState`, worker column decode via `FileManager::readColumnDeferred`) was only ever compiled by
the retired web build's threaded trees; no native target defined it. SM.8 deleted it, along with the thread-local
mesher scratch and the snapshot/burn-mask hooks in `rebuild2()`: `rebuild2()` is single-threaded and reads the
chunk's own `pblocks`/`pcolors`, and meshes, column reads and uploads all run inline on the main thread. It is
recoverable from git (the last commit that has it is the one before the SM.8 commit; design in
`WORKING/b3-off-thread-meshing-plan.md`). If a threaded mesher is ever wanted again, the things it had to
guarantee were: a private snapshot of the voxels (the counting and fill passes must agree, rule 7), deferred
`Portal::addPortal` / corrupt-type `setLand` repair, a precomputed burn mask instead of walking `burnList`, and
never recycling a chunk or re-homing a column with a job in flight.

### VBO upload — `TerrainChunk::prepareVBO()`
Mallocs the per-chunk index scratch `indices`, uploads the two vertex streams and (re)creates
`elementBuffer`, then frees the staging arrays. The mesh fields (`n_vertices`, `num_vertices[]`,
`face_idx[]`, `objects`, …) are single copies: `rebuild2()` fills them and `prepareVBO()` runs later in the
same `World::update` (`Terrain::update` meshes, `updateAllImportantChunks` uploads), always before
`render()`. Until SM.8 there was a second `rt*`-prefixed set that `prepareVBO()` copied them into, left over
from the removed background meshing thread. Never let a frame render between `rebuild2()` and `prepareVBO()`:
the counts would describe the new mesh while the buffers still hold the old one.

**Since ROADMAP Phase M / M3(a) (2026-08-31) the GL buffer names persist across re-meshes.**
`prepareVBO` used to `glDeleteBuffers` + `glGenBuffers` all three names every rebuild, so a
full-window reload burst did ~1,500 buffer create/delete calls in a couple of seconds. Now
`chunkUploadArray()` keeps each name, tracks its byte capacity (`vbCapacity`/`vb2Capacity`,
never shrinks) and re-uploads with `glBufferSubData` when the new geometry fits, `glBufferData`
only to grow — the same pattern row E3 gave the object batches. The names are still deleted for
real in `unbuild()`, the destructor, and the `clearOldVerticesOnly` clear path. `render()`
early-returns on `n_vertices==0` and `render2()` only runs for chunks with `n_vertices2>0`,
so a name left allocated with stale contents is never drawn. Done for driver/CPU churn, not
memory — on the web build, where this was done, the buffers lived in the browser's GPU process
(`WORKING/web-port-memory-plan.md` §M3).

**Since Phase N Stage 1 (2026-09-05) `prepareVBO()` can also fingerprint what it publishes.**
`geomHash`/`fullHash` (`TerrainChunk.h`) are FNV-1a hashes of the vertex arrays, folded *just
before* the staging arrays are freed and read back by `eden_debug_mesh_checksum()`. They live on
the chunk rather than in the probe for a structural reason worth knowing: after `prepareVBO()` the
only copy of the vertex data is inside a GL buffer, so a probe that walks `chunkTablec` in a
settled world finds `verticesbg == NULL` and reports zero vertices. Publish time is the only moment
the data exists. `geom` covers position+texcoord (deterministic across platforms), `full` adds the
baked vertex colours (lighting, so it only converges after equal settling); `pad` is excluded
because the mesher never assigns it. The whole pass is **off by default** —
`eden_debug_set_mesh_checksum(1)` enables it, and it must be called before the world is meshed.
This is what proved the native (SDL3 + desktop GL) build meshes byte-identically to the (now
retired) web build at both heights: `WORKING/phase-n-stage1-results-2026-09-05.md`. Today every
native target must match the pair pinned in `.github/scripts/check-stage1.py`.

**`render()`'s only "is there geometry" test is `n_vertices==0`, so every mesh-count field has to
say "empty" from construction, not from the first `prepareVBO()`.** The constructor did not
initialise them until 2026-08-27: a chunk is allocated out of a heap that has just held vertex and
colour data, so `n_vertices` came up garbage-nonzero, `render()` sailed past the guard, and the
index re-pack `memcpy`'d from `allIndices + face_idx[i]` with `face_idx[i]` in the tens of
millions (into a NULL `indices`) — an out-of-bounds trap inside the frame tick. Stock never
reached it because a chunk was always meshed before it could be drawn. **Anything that lets a
resident chunk be drawn before its first mesh — the bulk reload's neighbourhood deferral
([world-and-terrain.md](world-and-terrain.md)) — depends on that initialisation.** Note the re-pack still bounds only its
*destination* cursor against `INDICES_MAX`, not `face_idx[i]+num_vertices[i]`; that is fine
while a chunk cannot exceed 32768 vertices, and is the guard to revisit if one ever can.

## Frame passes

```mermaid
flowchart TD
    A[Terrain::render — pass 1] --> A1[renderTree: one linear scan, one frustum test per chunk<br/>→ renderList AND renderList2 (P.7)]
    A1 --> A2[chunk render: directional face culling —<br/>camera-vs-bounds picks visible buckets,<br/>indices memcpy'd from allIndices, glDrawElements]
    A2 --> A3[Doors: rebuilt as float verts per frame,<br/>swing animation by player proximity, GL_LIGHT0]
    A3 --> A4[Golden cubes: tessellated, rotating,<br/>spheremap texture + specular GL_LIGHT1]
    A4 --> A5[Sky: ortho pass — colored skybox and/or BW skybox<br/>tinted skycolor, alpha-blended transitions]
    A5 --> A6[Portal quads: swirl texture, rotating UVs]
    B[RenderModels — creatures] --> C
    A6 --> B
    C[Terrain::render2 — pass 2] --> C1[renderList2 (filled by the pass-1 renderTree walk),<br/>back-to-front qsort by distance]
    C1 --> C2[chunk render2: per-face glDrawArrays,<br/>animated water/lava via texture-matrix translate<br/>frame/16 row shift]
    C2 --> C3[Flowers: billboards facing player,<br/>back-to-front sorted, flower atlas]
```

Pass-1 directional culling detail (`TerrainChunk::render`): for each of the 6 buckets,
`curVis[f]` compares camera position against chunk bounds (you can't see the +X faces
if you're at lower X than the chunk, etc.). When visibility changes, the chunk's index
buffer is re-packed by memcpy-ing runs of the identity `allIndices` array — cheap
because vertices are already grouped by face. Pass 1 already submits all 6 visible
buckets in **one** `glDrawElements` call against the re-packed index buffer, so it never
had the row-#20 problem below — only pass 2's `glDrawArrays` path did.

**Pass-2 chunk draw batching (perf-audit row #20, `TerrainChunk::render2`).** The 6 face
buckets in `vertexBuffer2` are laid out back-to-back by construction
(`face_idx2[i]=num_vertices2[i-1]+face_idx2[i-1]`, `TerrainChunk.cpp` around the mesher's
counting pass), but `render2()` used to issue one `glDrawArrays` per visible bucket, in a
scrambled per-axis order (5,1,0,3,2,4, not ascending index order) — 6 draw calls, an ES1
artefact once WebGL (and now the shim on every target) made per-call overhead real (§2/C2 of the perf audit). Since 2026-07-30
it collapses to **one** `glDrawArrays` over the whole contiguous span when all 6 buckets are
visible (the common case: it just means the camera sits within the chunk's XYZ bounds), and
falls back to the original 6 separate per-face draws, in their original order, otherwise
(the rarer partial-visibility case, typically a chunk at the world's edge). The merged path
does submit the 6 buckets in ascending-index order rather than the scrambled one, which is a
real order change — accepted because it only affects which of two *translucent triangles
from different faces of the same chunk* wins a rasterizer tie at the same screen pixel and
depth, which this stream (water/lava/glass/leaves quads facing 6 different directions of one
16×16×64 chunk) essentially never triggers. The cross-chunk order that actually matters for
translucency — `Terrain::render2`'s `renderList2` back-to-front qsort by chunk distance,
diagrammed above — is untouched; this is purely an intra-chunk optimization.

Water/lava animation: atlas 2 rows are animation frames; `render2` advances a global
`frame` counter and translates the texture matrix by `(int)(frame/16)` rows.

**Signs (D.3b, 2026-10-10 — not stock).** `SignRenderer::render()` (`Classes/SignRenderer.cpp`) runs
between `RenderModels()` and `Terrain::render2()` in `World::renderFrame`, so signs sit after the opaque
pass and under the sorted transparent one. It reads the trailer model (`fm->trailer()`), keeps signs
within 32 blocks of the player (which also keeps every anchor inside the toroidal window — `getLand`
wraps outside it), hides one whose anchor is air or whose front cell is opaque (`!IS_NOTSOLID`), and
builds one dynamic VBO per frame: command-block buttons as untextured vertex-coloured boxes (one
`glDrawArrays`); standing and wall-sign boards as boxes textured with the atlas planks tile (`TEX_WOOD`,
FT.14, turned a quarter so the planks run along the board, modulated by the sign's paint colour) and the standing post with the same planks tile unturned and 30% darker, ending at the board's lower edge (no overlap), one draw; then one textured quad per sign's text. Text textures come from the text
raster seam (`eden_rasterize_text_rgba`, one call per wrapped line, body face), outlined here by
dilating the coverage, 256 texels across the board, mipmapped; an LRU cache keyed by (board kind, text),
≤ 256 textures / 24 MB (64 / 8 MB until 2026-10-17: past the 64 nearest a dense server spawn drew blank boards), nearest signs first, ≤ 4 built a frame. Shapes (D.4b resized them to Minecraft's
2:1, the user 2026-10-10): a standing sign (a = 3) is a 1 × 0.5 board on a 0.5 post facing `c`; a wall
sign is a 7/8 × 1/2 board centred on the face, 1/32 off it; on a face of a block with a `CMB1` record it
fills the skin's 0.75 panel instead. Since G.5b-4 the pass is all `R::` (`texCreate`/`texDestroy` for the
text, `bufCreate`/`bufData`/`bufSubData` for the VBO, explicit state and fog-off per draw; the skin draw
keeps its polygon offset −1/−4 and the text its 0.02 alpha reference) and leaves no ES 1.1 state behind.

**Command blocks (D.4b, 2026-10-10 — not stock)** are drawn by the same pass: every non-air block with a
`CMB1` record within 64 blocks gets a fixed skin on each exposed face (neighbour not opaque) — one quad
textured with the community `cmd_block_V2_icon.png` (`Resources::getArtTex(ART_CMD_BLOCK)`, 32×32,
mipmapped, modulated by the face shade; an orange frame + charcoal panel before 2026-10-27) and a square button box that is red idle / green while
`CmdTool::running()` (D.4c sets it: while an instance of the script is alive, and for at least
0.25 s after a press so an instant script still flashes), with a fixed per-face shade (top 1.0, sides
0.8/0.7, bottom 0.55). While green the button is pushed in to 0.036 (1/32 + 0.005, still in front of a
board) from 0.047.
Never the voxel's paint, never meshed: the steel underneath stays stock steel. The textured face is a
separate draw under `glPolygonOffset(-1, -4)` 0.002 off the face — at `P_ZNEAR` 0.012 a 24-bit depth
step is ~0.02 block at 64, so distance alone would z-fight; the button stands 0.047 out, past a wall
sign's board (1/32), so a sign on the face never hides it. Buffer layout `[skin | flat | wood | text]`.
Everything is unlit, with cull face off for the pass; it enters with texture on / lighting off / blend
off / cull on / `GL_ARRAY_BUFFER` 0 and leaves exactly so. `eden_debug_signs()` reports the last
frame's counts (`cmds`, `cmdInRange`, `cmdFaces`, `cmdRunning` since D.4b).

The sky is not a skybox mesh — it's screen-space ortho quads (`Texture2D::drawSky`)
drawn *between* pass 1 and pass 2 at near-far depth, with a colored/B&W crossfade
state machine for sky-color regions (the `drawSky` calls in `Terrain::render`, `ICO_SKY_BOX`/`ICO_SKY_BOX_BW`).
*(Port, G.5b-6: the ortho bracket is on `R::proj()/mv()`, each layer states its `R::State` and passes its tint
to `drawSky(rect, depth, r, g, b, a)`; fog stays off from the sky to the end of pass 1, so the portal swirls
after it draw unfogged, as stock. The crossfading B&W layer is always `B_ALPHA`: stock blended it with
whatever blend func the previous frame's HUD left, which is `SA,1-SA` except with the HUD hidden
(`hideui`), where `beginHud`'s `ONE,ONE` leaked through and a sky transition brightened additively.)*

### Scene-pass bracket (modified from stock, 2026-10-04, ROADMAP N.4.11)

`World::renderFrame`'s play-mode branch now calls `R::beginScene()` before
`Graphics::prepareScene()` and `R::endScene()` just before `hud->render()` (until FT.16, 2026-10-28, it called
the port's `eden_scene_pass_begin/end` directly; `R::` forwards to them). Everything
between them (sky, terrain passes 1 and 2, creatures, effects, fireworks, the player's selection
outline) is the **3D pass**; the HUD, menus and `GLDialog` come after it. Both are port hooks
(`port/src/render/gl/gl_context.h`); at 100% they are no-ops. At any other `render_scale` the native
context (`port/src/render/gl/gl_context_native.cpp`) points the 3D pass at an offscreen framebuffer of
drawable × scale (colour TEXTURE + depth renderbuffer, both cleared at its start) and at `_end` clears the
window's colour (black; magenta under `--diag-clear`) and composites the scene with one linearly filtered
textured quad (the UI program) into the letterbox box, so the 2D UI still draws at full density.
**FT.16 (2026-10-28) replaced N.4.11's `glBlitFramebuffer` with the quad:** on the Air 2 at 50% the blit
never landed (10-09: HUD over a frozen menu frame; no scissor or other FBO user exists in the port), so the
setting was hidden on iOS from N.4.12 until FT.16 re-showed it (50/75/100%; Air 2-class iPads/iPhones
default to 75%: the plan's "match 2.1" 50% looked too soft to the user in Round 0, 2026-10-28; 75% is
2.25× stock 2.1's 1024×768, 100% is 4.0×). Round 0 on the Air 2 (ipa `2c4efd3`): 50/75/100% all present, the
`scene-diag` lines show the view's FBO 1 / renderbuffer 1 throughout and no GL errors. The window clear means a failed composite shows black, never
a stale frame. `--diag-clear` also logs `[eden-gl] scene-diag` lines once per scene size (framebuffer ids
at begin, composite and present; GL errors; the iOS view's colour format). Point-sprite sizes are compensated for the scale; picking
is unaffected (it unprojects against the fixed point-space viewport). Anything new that belongs to
the world must draw *before* `_end`, and anything that is UI must draw *after* it, or it renders at
the wrong resolution.

**iOS presents the BOUND renderbuffer** (N.4.12, 2026-10-08). SDL's UIKit swap is a bare
`presentRenderbuffer:GL_RENDERBUFFER`, so GL code on native must never leave a renderbuffer other
than SDL's view's bound at the end of a frame. The scene framebuffer's allocation once ended with
`glBindRenderbuffer(0)`; at 50%/75% the iPad then stopped presenting from the first world frame on
(screen frozen on the loading bar, game running underneath) while every readback-based harness
passed. The allocation now restores the previous binding, and `eden_native_gl_present()` (which also does P.11's depth discard) carries an
iOS-only **present guard**: it rebinds the view's renderbuffer if anything else is bound, logs
`[eden-gl] PRESENT GUARD` once, and `--smoke` fails if it ever fired.

## Camera (`Camera.cpp`)
First-person: position = player pos (eye offset), yaw/pitch from touch-look. `render()`
builds the modelview (rotate, then translate by `-(pos - chunkOffset*16)`); `render2()`
is the same matrix used by the picking raycast. Third person (H.12: `CAMERA_VIEW`, `cam->mode` 1 behind,
2 front) derives eye and target from `Camera::viewPoints()`, which feeds **both** the frustum lookAt and
the scene lookAt — stock built the frustum from the first-person pair, culling the third-person frame
against a view it was not drawn with. Keep the two lookAts the same pair. Details:
[player-input-camera.md](player-input-camera.md) § Camera.

## Frustum culling (`Frustum.cpp`)
`setFrustum(viewproj)` extracts 6 planes each frame (called from `Camera::render`);
`ViewTestAABB(rbounds, state)` does plane/AABB rejection with an inside-plane bitmask
optimization. Chunks failing entirely are skipped; there is no hierarchy (octree
removed).

## Object batch upload (perf-audit row 23/E3, 2026-08-06)
Doors, golden cubes, portal frames, portal swirls and flowers are still rebuilt on the
CPU every frame (animation — door swing, cube rotation, portal UV swirl, flower
billboard yaw — is unavoidable), but each batch now owns a persistent
`GL_DYNAMIC_DRAW` buffer (an `R::Buf` with `U_DYNAMIC` since G.5b-6, drawn with `R::drawWorld`, or
`R::drawLit` for doors and golden cubes; `Terrain.cpp`: `ObjectBatch`/`objBatchStage`/`objBatchDraw`,
`g_doorBatch`/`g_goldenBatch`/`g_portalBatch`/`g_swirlBatch`/`g_flowerBatch`) instead of
filling a stack-local `vertexObject objVertices[]` and handing GL four client-side
array pointers. The frame still fills a staging array exactly as before; the change is
one `glBufferSubData` upload per batch (growing via `glBufferData` in power-of-two
steps) instead of the GL shim silently re-uploading all four client arrays' worth of
the same bytes per draw (`gl_fixed_function.cpp`, `eden_gl_setup_attributes` — client-side
vertex data isn't legal in the core/ES3 contexts the shim targeted; since Q.5 the client-memory path (the shim's until G.5b-7, now `R::`'s for every `vmem` draw) copies an interleaved struct once, only the vertices used, into shared rings: `port/docs/history/gl-shim.md`). One buffer per batch, not a shared one — a shared
buffer would make its `glBufferSubData` a write the previous batch's draw is still
reading (driver sync stall each batch); five small elidable rebinds cost less than
that. Measured live in Safari on the retired web build (`web-port-final:web/tools/safari-objbatch-probe.js`;
no native twin), 24 doors + 24
golden cubes + 24 portals + 144 flowers): upload traffic dropped from 47
`glBufferData`/frame, 756.5 KB/frame (44.3 MB/s @ 60fps) to 8 `glBufferData` + 5
`glBufferSubData`/frame, 176.6 KB/frame (10.3 MB/s) — a 4.3× drop.

This also retired three latent stack-overrun bugs the old fixed `max_render_objects*6*6`
(10800-vertex) array had: golden cubes emit 144 vertices each (76 visible cubes
overran it), flowers emit 6 each against `MAX_FLOWERS=10000` (1801 visible overran
it), and `doorso`/`portalso` were unbounded writes into 500/200-entry stack arrays
(now clamped). The new staging arrays `realloc` to fit and return `NULL` on failure
rather than lying — every call site guards its loop with `objVertices&&`.

Instancing for the non-animated subset (portal frames, which reuse regular cube
geometry) is a separate future item (row 24/C2, ES3 instancing) — not done.

## Performance characteristics & knobs
- Meshing a chunk is O(4096·faces); a full window re-mesh (1296 chunks at the default radius and
  64z, 5184 at 256z) happens on
  load and on lighting changes — this is the main hitch source.
- `INDICES_MAX = 32768` (`Geometry.h`) caps vertices per chunk stream; faces beyond it
  are silently dropped (`render` guards with `continue`).
- `max_render_objects = 300` caps doors/cubes/portals per frame; `MAX_FLOWERS 10000`.
- Non-mip textures upload no mip chain and sample `GL_NEAREST` (P.15, `Texture2D.cpp` `initData`).
  A texture that sets a mip filter must be loaded with mips, or it samples an incomplete texture
  and draws black (the UI icon atlas, `GLWidgets.cpp` `iconAtlas()`, did until 2026-10-14).
- Disabled experiments you may see: occlusion queries (`isTesting`), distance-based
  ZFAR throttling by FPS, face merging.

## Per-pass instruments (modified from stock, 2026-10-12, ROADMAP P.1b)
`Classes/PassTimers.h` brackets every update and render pass with `pass_begin(P_…)` /
`pass_end(P_…)` (`World::update`, `World::renderFrame`, `Terrain::render`/`render2`, and the native
seam's present). Each pair adds the pass's CPU milliseconds and the GL shim's draw / vertex /
issued-setup counters (`eden_gl_stat_live()`, differenced) to `g_pass`. The hooks compile only under
diagnostics builds (every build, since G.1) and do nothing until a harness sets `g_pass.on`, so a normal frame pays one
bool test per hook. With `g_pass.finish` set, every boundary first calls `glFinish`, so a pass's
time becomes an upper bound that includes its own GPU work.
- **In an interactive session** `--power-phases` (with `--power-log=N`, PW.0a) turns the timers on and prints each
  window's per-frame means as `[eden-power-ph]`; `--power-exp` switches (PW.0b, `PassTimers.h` `pexp()`) skip one
  pass at a time for the Air 2 bisection legs. Mac numbers rank CPU cost only: macOS GL is Apple's GL-on-Metal
  translator, so most Mac CPU time is in the driver, not the engine.
- **Passes nest.** `r_opaque` (all of `Terrain::render`) contains `r_cull`, `r_chunks`,
  `r_objects` and `r_sky`; `r_transparent` contains `r_tchunks` and the flower part of
  `r_objects`; `r_cull` covers the single `renderTree` walk (P.7; was two) and the back-to-front `qsort`. Sum only the
  leaves. `render` is the play branch only, so a GL dialog drawn over it is not counted.
- **One draw per chunk per pass, not six.** The opaque loop builds one index list per chunk from the
  face buckets the camera can see, so `r_chunks.draws` is the number of chunks drawn. On the Mac, at
  the three `--pass-probe` poses: 49–80 draws, 66k–110k vertices (`WORKING/low-end-perf-audit-2026-10-12.md` §4).
- `present` exists only on native. With vsync off, the time blocked in the swap is the GPU proxy:
  near zero means the GPU kept up with the CPU.
- **On iOS (Air 2, 2026-10-13, Stage P.3)** the swap interval stays 1 whatever the probe asks, and the
  vsync wait shows up in **`r_setup`** (the frame's first framebuffer touch), not in `present`. CPU work
  per frame is therefore `frame − r_setup`. **`--pass-finish` on a tile-based GPU** charges every pass
  that draws anything a tile store/reload of ≥ ~2.7 ms (one 4-vertex quad reads 7 ms). Use it to
  compare a pass against itself, never as that pass's GPU cost.
  `WORKING/low-end-perf-audit-2026-10-12.md` §5.1 has the evidence.
- Harness: `--pass-probe` and `--stage1 --walk … --pass-timers` (native/README.md). Each pass reports `p50`, `p90`, `max` (2026-10-16) and `mean` ms.
- **`g_pass.track` (G.5b-0, 2026-10-25)** makes every `pass_begin`/`pass_end` also report to the renderer's
  pass tracker (`eden_render_pass_mark`, `port/src/render/render_passes.cpp`), so a draw knows its innermost
  pass: `--scene-shot` captures each pass alone with it, `--state-trace` names it, and `--render-selftest`
  checks the signs pass contract at `P_R_SIGNS` and that passes balance over a frame. Off unless a harness sets it.

## Common pitfalls
- Terrain vertex positions are shorts in quarter-block units — forgetting the ×4/×0.25
  produces subtly misplaced geometry.
- Object batches used to allocate `vertexObject objVertices[300*36]` on the stack;
  since row 23/E3 (2026-08-06) they're heap-backed persistent VBOs (`ObjectBatch`,
  above) that `realloc` to fit, so this is no longer a stack-overflow risk.
- GL state leaks easily: every pass assumes the previous one restored texture-matrix
  scale, fog, blend, and client arrays. Match every `glScalef` on `GL_TEXTURE` with
  its inverse (the code does this manually: the `glScalef(1,(float)ATLAS_TILES,1)` pairs in `Terrain::render`).
- `glDrawElements` with a stale `elementBuffer` after buffer deletion order changes
  crashes on device but often works in simulator.

## Safe vs. risky to modify
- **Safe:** colors/shade tables, fog parameters, adding a new object batch modeled on
  the flower/portal loops, atlas layout changes (update `getBlockTexShort` +
  `blockTypeFaces`; growing the strip past `ATLAS_TILES` means changing that one constant and
  regenerating both PNGs with `port/tools/atlas-d2t.py`'s pattern).
- **Caution:** `rebuild2` (counting and fill passes must stay exactly consistent or
  you'll write past `verticesbg`), bucket/`face_idx` bookkeeping, `prepareVBO`
  swap ordering, anything shared with the old threading design.

### Q.1a notes (2026-10-21)
- **F9 (object batches):** the golden cube's rotated corners/normals (`gcV`/`gcN`, 144 each) are built once per frame, not per cube. **F10 (signs):** `SignRenderer::render` orphans its VBO (`glBufferData(NULL)`) every frame before the sub-uploads. **F18:** `CalcEnvMap`'s sphere-map V is `*ATLAS_TILES` (was 32), next to X.8's sky note; changes the golden cube's look.

### Q.1b notes (2026-10-21)
- **F6 (object array ownership):** `objects` is freed on the pointer, never on `num_objects` (`clearMeshes()`, which `rebuild2`, `unbuild`, `dropGeometry` and `~TerrainChunk` all call, plus `resetForReuse`). Stock zeroed the count first, so every chunk drop and world exit leaked the array (measured 54–56 root leaks a warp, `TESTERS/perf-audit/leakobj3.*.leaks`; 0 after, `TESTERS/q1b/`). Since SM.8 `objects` is the one published array (there is no `rtobjects`), so `clearMeshes` frees it; `prepareVBO` zeroes `num_objects` when `objects` is NULL.
- **F7 (objects-only chunks):** `rebuild2` returns early (`clearOldVerticesOnly`) when a chunk has objects but no faces (a lone golden cube, door top or flower in air). `prepareVBO` keeps `objects` there and zeroes `num_objects` when it is NULL (the earlier early returns count objects without allocating them). Stock never drew these.
- **F19 (cull/sort eye):** `TerrainChunk::render`/`render2`'s per-direction face culling and `compare_back2front` read `g_cullEye` (`Terrain.cpp`), set at the top of `Terrain::render` from `Camera::viewPoints`. In third person that is the real eye, not the pivot `cam->px/py/pz`. First-person culling is unchanged; the transparent sort now measures from the eye (`player->pos` + 0.3·boxheight), not the feet.
