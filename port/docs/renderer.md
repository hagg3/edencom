# The engine-owned renderer (`Classes/Renderer.h`, Stage G.5)

Plan and decisions R1–R10: [`WORKING/renderer-interface-plan-2026-10-25.md`](../../WORKING/renderer-interface-plan-2026-10-25.md).
G.5b-0 (2026-10-25) landed the foundation with **no look change**: the interface, the renderer-owned
matrix stacks the GL shim forwards into, the shared GL state cache with the lazy-apply bridge, the five
programs, and the instruments. G.5b-1 (2026-10-27) moved the 2D screen brackets (`Graphics::prepareMenu/endMenu/beginHud/endHud`, `GLW::begin2D/end2D`) and `GLW::fill` (`Graphics::fillRect` → `R::draw2D`) onto `R::`; G.5b-2 (2026-10-27) moved the HUD's and menu's untextured draws (crosshair, liquid/flash/fade overlays via `Graphics::fillScreen`) and the last hand-copied 2D brackets (Hud fly button, ChatFeed) onto `R::`; the Hud's textured atlas quads (picker faces) stayed on the shim; G.5b-3 (2026-10-27) moved `Texture2D` (every upload is `R::texCreate`/`texDestroy`, every text/icon/sky quad one `R::draw2D` / `R::drawWorld`) and the readbacks (`takeScreenshot` → `R::readPixels`; picking, `Camera`'s frustum and `DebugState`'s probe → `R::mv()/proj()/pickViewport()`). G.5b-4 (2026-10-27) moved the effects pass (`SpecialEffects`, `Fire`, `BlockBreak`, `Firework`, `Player`'s cubes via `Graphics::startPreview/drawFirework/drawCube`, `World`'s effects bracket), the signs pass (`SignRenderer`: textures, buffer, draws) and `MpWorld`'s name-tag bracket, and turned on program 5 behind Settings › Video › *Original lighting* (debris and the firework cube; see below). G.5b-5 (2026-10-28) moved the creatures (`Model.cpp`: `DrawModel` → `R::drawSkinned` per bone batch, mesh buffers `R::Buf`, shadows and debug boxes `R::drawWorld`; no GL call left) and deleted the shim's `GL_OES_matrix_palette` emulation (palette state, entry points, skinning program, the `GL_EXTENSIONS` answer, `glLightfv` tracking); the world fog they need comes through the transitional `R::legacyFog()` until Terrain/Graphics move. G.5b-6 (2026-10-28) moved the terrain passes: chunk buffers are `R::Buf` (`chunkUploadArray`, the per-frame index upload, `bufDestroy`), pass 1 one `R::drawWorld` per chunk over its vertex + index buffers and pass 2 one per visible face run (`kFmtTerrain`, atlas / atlas2), the object batches `R::Buf` + `R::drawWorld` (portals, swirls, flowers) or `R::drawLit` (doors, golden cubes: *Original lighting* now covers them), the sky's ortho bracket and per-layer state (`Texture2D::drawSky` takes its tint), the atlas scale and liquid row shift on `R::uv()`, and `Graphics::beginTerrain/endTerrain/prepareScene` (gluPerspective's matrix straight onto `R::proj()`) plus the world fog (`Graphics::setFogColor/applyWorldFog`). Terrain.cpp and TerrainChunk.cpp keep only small exit hand-offs of ES 1.1 state (below). **G.5b-7 (2026-10-28) moved the rest and deleted the shim** (`gl_fixed_function.cpp`, `gl_es1_shim.h`, `gl_shim_internal.h`): the HUD's own quads (`Pen::strip`), the 2D kit state, the Texture2D callers, `initGraphics`' initial state, `drawRect`/`drawSkybox`, the terrain exit hand-offs and every clear (`R::setClearColor`/`R::clear`) are on `R::` or the pen (below); `RenderModels` uses `Graphics::applyWorldFog()`; `project.c`'s glu matrix calls reach the stacks through `eden_glu_*` hooks (`Graphics.cpp`); the context and loader moved to `port/src/render/gl/`. No engine or host file calls GL: the engine has no GL header (SM.7, 2026-10-28: `<OpenGLES/ES1/gl.h>` deleted, engine types are `<stdint.h>`; `glu.h` keeps its own typedefs and error enums; `static_assert`s on the vertex structs pin the `R::kFmt*` strides), and `port/tools/gl-call-gate.py` (CI, linux) fails any live `gl[A-Z]` call outside `port/src/render/`. Results: [`WORKING/g5b7-results-2026-10-28.md`](../../WORKING/g5b7-results-2026-10-28.md).
everything else.

## Files
| File | What |
|---|---|
| `Classes/Renderer.h` | The interface (namespace `R`): plain C++, no GL types or enums. |
| `port/src/render/render_core.cpp` | Matrix stacks (R2; the math moved verbatim from the shim), per-draw state, frame / scene / 2D brackets and their invariants, the pick viewport, `--state-trace`, dispatch to GL or null. |
| `port/src/render/render_formats.cpp` | `R::kFmt*` from the engine's vertex structs via `offsetof` (prefixed: it includes `Graphics.h`). |
| `port/src/render/render_passes.{h,cpp}` | Which pass a draw is in (fed by `Classes/PassTimers.h` when `g_pass.track`), its category, and `--scene-shot`'s per-pass draw mask. |
| `port/src/render/render_internal.h` | What the core and the two backends share (`rcore::`, `rgl::`, `rnull::`). |
| `port/src/render/gl/gl_state.{h,cpp}` | The GL state cache: program, texture unit, attributes (+ constant attributes), buffer bindings and `glDeleteBuffers`' rule, M0 buffer bytes, draw counters (`eden_gl_stat*`), and ACTUAL (what GL holds; the shim's LOGICAL half went in G.5b-7). |
| `port/src/render/gl/gl_context.h`, `gl_context_native.cpp` | The context, window, drawable, letterbox, pick viewport and the scene pass at render scale (N.4.11); plain-C header, the host's only view of GL (moved from `port/src/shim/gl/` in G.5b-7). |
| `port/src/render/gl/gl_loader_native.{h,cpp}` | Linux/Windows entry-point table (69 since G.5b-7). |
| `port/src/render/gl/framework/` | `<GLES/gl.h>`/`<GLES2/gl2.h>`: the platform's real GL, for the backend only. The engine-facing `<OpenGLES/ES1/*.h>` pair is gone (SM.7). |
| `port/src/render/gl/gl_stream.{h,cpp}` | Q.5's two streaming rings. |
| `port/src/render/gl/gl_textures.{h,cpp}` | Texture uploads (`texCreate`, `texSampling`) with every shim rule (deferred `GL_GENERATE_MIPMAP`, Q.4 swizzle, N.4.9 flush, accounting, probe, `--tex-exp=`). |
| `port/src/render/gl/gl_programs.{h,cpp}` | Dialect preamble + compile/link and the five programs (+ `lit-flat`, G.5b-4) with per-program uniform caches. |
| `port/src/render/gl/gl_renderer.cpp` | The GL backend: draws, textures, buffers, readback. |
| `port/src/render/gl/gl_selftest.cpp` | `--render-selftest-gl`'s unit checks; `--lit-oracle`'s scenes, core draws and comparison. |
| `port/src/render/gl/lit_oracle{.h,_legacy_macos.cpp}` | `--lit-oracle`'s fixed-function half: a CGL legacy 2.1 context, its own object library (macOS only), every GL entry point by `dlsym` (see the pitfall below). |
| `port/src/render/null/null_renderer.cpp` | No context: nonzero distinct ids, counted draws. |

## How a draw works (GL backend)
`begin(program, tex)`: `applyRenderer(ds().state, tex or the 1x1 white texture)` through the bridge,
`useProgram`, sampler 0. Uniforms are cached per program (`set1f/3f/4f/Mat4/Palette` send only a
change); `u_mvp` is recomputed only when `R::proj()` or `R::mv()`'s version moved. `submit()` sets the
attributes from the `VertexFormat` (a member the format lacks is disabled; colour becomes the tint and
point size `SpriteParams::size` as **constant attributes**, R5), streams `vmem` / `imem` through the
shim's rings with the same `[min,max]` copy and index rebase, draws, and hands the engine's buffer
bindings back (`restoreBindings`). Buffer indices over client-memory vertices are refused (an invariant
failure), as in the shim.

Fog by data: `setFog` sends `end` and `range = max(end - start, 1e-6)` (the shim's expression, CPU
side); `fogOff()` sends end 1e4, range 1, so `f` is exactly 1.0 for any distance the window reaches,
fp16 included. `alphaRef` −1 never discards. Sprites divide the attenuation by `g_point_px_scale²`
(N.4.11) like the shim; no min/max clamp (the shim has none). Fragment stages are `precision mediump`
on ES like the shim's, **except the texcoord varying, which is `highp`** (below).

**Texcoord precision (I.1/FT.8, 2026-10-28).** The block atlas is 32 x 4096: 128 tiles of 32 rows, and
the chunk mesher's v is exactly `(corner + type) / 128` (no inset; `getBlockTex` insets 1e-5, 0.04 row).
A `mediump` varying is fp16 on Apple GPUs, and fp16 rounds v by up to 0.25 row near tile 25 (cloud),
0.5 row near tile 63 and 1 row from tile 64 up (v >= 0.5), so under NEAREST magnification the edge of a
face sampled the next tile's first row: a 1 px strip of another block (iPhone 16 Pro Max; the Air 2 did
not show it). Measured on the CPU: 0.78 % / 1.56 % / 3.12 % of a tile's fragments land outside it in
fp16, 0 % in fp32. The fix is `in highp vec2 v_texcoord` in the world/lit, ui and skinned fragment
stages (ES 3.0 guarantees fragment `highp`; desktop GLSL ignores precision: `--scene-shot` byte-identical,
66/66). `--tex-exp=fp16uv` puts the fp16 rounding back (`EDEN_UV_FP16`, `kUvFS`) to show the old look on the Mac.

## The five programs (plan §6)
1 **world** (terrain, objects, sky, signs): pos, colour, uv × `u_uvso` (scale.xy, offset.xy: the
atlas scale and the liquid row shift), texture, alpha test by data, fog. 2 **ui**: pos, colour, uv,
texture. 3 **sprite**: points, size attribute or constant, ES 1.1 attenuation, `gl_PointCoord`, fog.
4 **skinned** (creatures since G.5b-5): what the shim's palette program was, without flags (palette maps
object → eye; `u_proj`; up to 16 matrices, `nmat` uploaded; `u_boneCount`; light 0 at the eye). 5 **lit**: ES 1.1 per-vertex lighting, two positional or
directional lights, colour material, non-local viewer, `u_ambSum` = model ambient + enabled lights'
ambient (CPU); shares world's fragment stage. `drawLit` routes to world when *Original lighting* is
off (R8). **`lit-flat`** (G.5b-4) is program 5 compiled with a `flat` colour varying (`EDEN_COLOR_QUAL`, a define
after the preamble): ES 1.1 `GL_FLAT`, the triangle's last vertex colours it; `drawLit` picks it when
`LightSet::flat`. Attribute locations are bound identically in every program (the shim's two too, until G.5b-7):
position 0, colour 1, texcoord 2, normal 3, pointsize 4, matrixindex 5, weight 6.

## Frame structure and invariants
`EdenViewController::drawFrame` brackets `World::renderFrame` with `R::beginFrame/endFrame`; the scene
bracket is `R::beginScene/endScene` since FT.16 (forwarding to `eden_scene_pass_begin/end`, which report to
`eden_render_note_scene`; G.5b-6's leftover). At render scale < 100 % `endScene` composites the scene texture with
one `rgl::draw2D` quad (identity matrices, state off) instead of N.4.11's blit; a backend entry point it added,
`glFramebufferTexture2D`, is in both lists of `gl_loader_native.h`. Counted always
(`rcore::inv()`), asserted by `--render-selftest`: draws outside a frame, nested / unmatched frames,
matrix stacks not back at their depth over a frame, 2D or scene left open, the pass tracker unbalanced
(when tracking), and the **signs pass contract** (texture on, lighting off, blend off, cull on, no array
buffer) at `P_R_SIGNS` begin and end. Since G.5b-7 the signs contract only counts (it read the shim's logical state; the signs pass states its own since G.5b-4).

## Instruments
- **`--scene-shot[=PREFIX]`** (window): a scratch world `scene-shot` with every object kind (water,
  lava, glass, leaves, doors, portal, golden cubes, flowers, both sign kinds, a command block, the 7
  creatures, fire, a firework, a block break), fixed `etime` 1/60, the engine RNG pinned
  (`eden_rng_seed`: `eden_prefix.h` renames every engine `arc4random()` to `eden_arc4random()`, a
  PCG32 while pinned), three poses × day/night, each as the full frame plus every pass category alone
  (66 BMPs). User input and focus events are dropped while it runs (`g_ignoreUserInput`): a keystroke
  or a click elsewhere on the machine pressed the jump button or paused the game and made captures
  differ run to run. `--render-scale=75` and `--window=WxH` give the 75 % and 2× legs.
- **`--state-trace=FILE`** with `--scene-shot`: one line per draw of the day pose-0 full frame (pass,
  category, call site, primitive, program, texture, colour source, fog, blend, depth, cull, offset,
  alpha, lights, sprite, texture matrix, stack depths, arrays). Compare two builds after normalising
  `tex=on:N`: GL texture **names** are allocated by the driver and are not stable between builds.
  Since G.5b-4 an `R::` draw writes a line too (`call=R`, its program, texture, colour source, fog,
  blend, depth, cull, offset, alpha ref, texture transform, and for `lit` the light set), so a tier
  diffs each ported draw against the shim line it replaced.
- **`--lit-oracle[=DIR]`** (macOS, window; G.5b-4): seven scenes (doors day/night, the firework cube,
  flat-shaded debris under its far light with an atlas uv transform, the golden cube day/night with
  specular, a directional + second light) on a 4×4-tessellated cube, drawn through real fixed-function
  GL (a private CGL legacy 2.1 context) and through `R::drawLit`; each must agree within 2/255 on every
  interior pixel, plus a negative check (the flat scene shaded smooth must differ). `DIR` gets each
  scene's two BMPs. Exit 2 where no legacy context exists (the stub off macOS).
- **`--render-selftest`** (headless, CI): R2, the null backend, negative self-checks (each invariant
  fires on its own violation), then the invariants over the menu, a world in play, its pause menu and
  the console, and back. **`--render-selftest-gl`** (window): the same plus the GL unit checks on a
  64×64 FBO — world (fog, texture matrix, client indices, ± blend), ui (± texture) and sprite against
  ES 1.1 arithmetic on the CPU (±2/255; until G.5b-7 against the shim's draw; fog distance is per VERTEX,
  as in ES 1.1); skinned (since G.5b-5, the shim's program being gone) and lit against ES 1.1 on the CPU
  (±1/255; skinned also checks that a two-bone blend lands the quad where both bones put it); R8; R3 (the state cache: a repeated draw issues no state, a deleted texture is forgotten).
  `--gl-selftest` (W.2) is an alias since G.5b-7: its no-context check is the null-backend one here.
- **`port/tools/shot-diff.py A B [--out DIR] [--tol N] [--mask X,Y,W,H]`**: BMP files or directories,
  stdlib only; exit 0 identical / 1 differ / 2 error.

## The pen (G.5b-7)
The HUD, the menus, the GLW kit, Joystick/Toolbar/Gamepad/statusbar and the preview cubes are written as ES 1.1
immediate-mode code: set a colour, blend, depth, cull and a texture, then draw text and icons through `Texture2D`
and a few quads of their own. G.5b-3…6 bridged that through the shim's logical state (`R::legacyState()`,
`legacyColor()`, `legacyFog()`, all deleted in G.5b-7). Now the engine keeps it in **`Pen::`** (`Classes/Graphics.h`):
`color/colorub`, `blend`, `blendFunc(R::Blend)`, `depthTest`, `depthWrite`, `cull`, `texture`, `textured`, each
named after the ES 1.1 call it replaced and called in the same order, so the state at every draw is what the shim's
logical state was (`--state-trace` and `--scene-shot` identical). Only pen draws read it: every `Texture2D` quad
(`setState(Pen::state())` + `draw2D(kFmtUI3UV, tint = Pen::rgba())`), `Pen::strip` (≤ 16 vertices, interleaved, one
`draw2D`: the Hud's picker faces and block-and-border quads, the GLW icon) and the preview cubes' tint. `drawSky`
stays `drawWorld` (fog off, `u_uvso`). Initial values are ES 1.1's after `initGraphics` (depth test, cull, texture on;
blend off with SA,1-SA). Headless the pen still tracks state; nothing reads GL.

**Exit hand-offs (G.5b-6), now pen calls.** `Graphics::endTerrain`, the end of `Terrain::render` and `render2` set
the pen's texture, colour and depth write the way the shim calls did, so the HUD after them starts from the same state.

**Clears.** `R::setClearColor` + `R::clear(color, depth)`: a depth clear always writes depth (it forces the depth
mask, which was always on at frame start), and while `--scene-shot` masks passes a depth clear also clears colour.

`TexDesc` has `clampS`/`clampT` (the atlas-style texture repeats S and clamps T) and `mipmaps` with `!linear` is
`NEAREST_MIPMAP_LINEAR`, the filter `Texture2D(genMips)` always used; the four PVRTC formats stay in
`Texture2DPixelFormat` but have no `TexFormat` (nothing creates them since G.1; `initData` aborts as it did on an
unknown format). `kFmtUI3UV` is the interleaved float3 pos + float2 uv quad.

## Original lighting (G.5b-4; plan §7)
Settings › Video › *Original lighting* (`original_lighting`, default on, not in the presets) →
`R::setOriginalLighting`. On, `drawLit` is program 5 (or `lit-flat`); off, it is `drawWorld`, which is
byte-identical to the unlit look the port had before (`--scene-shot` 0 of 66 with it off). Call sites so
are block-break debris (`BlockBreak::render`), the firework cube (`Graphics::drawFirework`) and, since G.5b-6,
doors and golden cubes (`Terrain::render`'s object batches). What 2.1 actually did, read from the stock calls:
- **Debris:** LIGHT0 ambient .3 / diffuse .7, model ambient 0.2, no specular, **flat shaded**
  (`Graphics::initGraphics` sets `GL_FLAT`; BlockBreak's `glShadeModel(GL_SMOOTH)` comes after its debris
  draw), and the light position is loaded while `glMatrixMode` is `GL_TEXTURE` (the push / loadIdentity /
  pop hit the texture stack), so ES 1.1 transformed (0,0,0,1) by the effects pass's modelview: the light
  sits at that matrix's origin in eye space, tens of thousands of units away, i.e. nearly directional. Not
  at the eye, as plan §2.2 assumed.
- **Firework cube:** LIGHT0 at the eye (identity modelview), .3 / .7, smooth.
- **Doors:** LIGHT0 at the eye, .3 / .7 (night, unless `LOW_MEM_DEVICE`: .17 / .3), LIGHT1 off, material specular
  0, smooth, under the terrain modelview (×0.25: the shader normalises normals, as `GL_NORMALIZE` did).
- **Golden cubes:** LIGHT1 alone at eye (1,1,0), .3 / .3 (night .15 / .15), light and material specular white,
  shininess 100, smooth; the sphere-map UVs from `CalcEnvMap` still go through the atlas `u_uvso`, as the
  texture matrix applied to them in 2.1. Both lights' positions were loaded under an identity modelview
  (checked, unlike BlockBreak's). `--lit-oracle`'s door and golden scenes are exactly these values.
The shim always ignored `glShadeModel`, so the port drew everything smooth.

**Pitfall (G.5b-4 to G.5b-7): a call named `glEnable` was the shim's.** The shim defined C functions with
the real GL names and the static linker bound any plain call to them, so the oracle's legacy half resolves
the framework's GL with `dlsym` (the first oracle drew nothing and drove the shim instead). The shim is gone,
but macOS's OpenGL.framework still exports the legacy names: a stray `glColor4f` would link and do nothing
useful on a core context. The engine header split and the grep gate are what stop one now.

## VAOs (plan R7, revisited in G.5b-7)
**Kept: one global VAO** bound at context creation (desktop core; ES 3.0 uses VAO 0), with
`gl_state`'s attribute cache eliding repeat `glVertexAttribPointer`/enable calls. Per-format or per-buffer VAOs
were considered now that every buffer bind goes through `R::` and rejected: most draws (HUD, menus, effects,
signs' text, sprites) stream through the Q.5 rings at a new offset every draw, so their attribute pointers change
per draw with or without a VAO, and separating format from buffer (`glVertexAttribFormat`/`glBindVertexBuffer`)
needs GL 4.3 / ES 3.1, which macOS (4.1) and the iOS floor (ES 3.0) lack; chunk buffers would need a VAO per chunk
buffer (1,296 columns × buckets at radius 9) for a saving the cache already mostly takes (`--smoke` world frame:
191 state calls issued, 603 elided). Revisit only if a profile on the Air 2 shows attribute setup hot.

## Rules for a ported call site (G.5b-1 on)
State everything per draw or per pass (`setState`, `setFog`/`fogOff`, texture, tint); read nothing the
ES 1.1 code left behind except the matrices; leave nothing behind for it. Where unported code after a
ported function read state the stock code left (the current colour, the texture binding, client arrays,
blend), set them on the pen (since G.5b-7; before it these were ES 1.1 hand-off calls to the shim) and
prove it with `--state-trace`: every other draw identical. Use a named `kFmt*` or a
`static const VertexFormat` for a private struct. Gate: `--scene-shot` byte-identical against the tree
before the tier, plus `--render-selftest` / `-gl`.
