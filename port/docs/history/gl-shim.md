# GL ES 1.1 → OpenGL ES 3.0 / desktop GL Shim

> **History: deleted in G.5b-7 (2026-10-28).** The engine draws through `R::` ([renderer.md](../renderer.md)); no
> engine or host file calls GL. What this file describes that still runs moved into `port/src/render/gl/`
> unchanged: texture uploads (`gl_textures.cpp`: NPOT, L/A/LA swizzle, flush, accounting), the streaming rings
> (`gl_stream.cpp`), the state cache (`gl_state.cpp`), the context, letterbox and loader (`gl_context_native.cpp`,
> `gl_loader_native.*`) and the `framework/` headers. The translator itself (`gl_fixed_function.cpp`,
> `gl_es1_shim.h`, `gl_shim_internal.h`) is in git history. Paths below are the pre-G.5b-7 ones.

> Written for the web (WebGL2) build, retired 2026-10-13; the translator itself is what every native
> target runs. Web-only parts are at tag `web-port-final` ([web-port-archive](../../../docs/web-port-archive.md)).

`port/src/shim/gl/` (`gl_es1_shim.h`, `gl_fixed_function.cpp`, `gl_shim_internal.h`) implements only
the ~45 GL ES 1.1 fixed-function calls the engine actually makes — a thin, purpose-built shim, not a
general ES1 emulator. The native context, loader and drawable live in `port/src/shim/gl/`
(`gl_context_native.cpp`, `gl_loader_native.cpp`; `gl_es1_extras_native.cpp`, two ES1 entry points only the PowerVR SDK called, went with it in LG.7). Read [`../../docs/rendering.md`](../../../docs/rendering.md) first for the
draw passes, vertex formats, and meshing this shim sits underneath — none of that
changes; this file is about how fixed-function calls get turned into modern GL draws.

**Since G.5b-0 (2026-10-25) the shim sits on the engine-owned renderer's GL backend**
([renderer.md](../renderer.md)). It keeps its fixed-function state capture, its two programs and their
uniform caches, the client-array grouping and the draw entry points; the matrix stacks are
`R::mv()/proj()/uv()` (`port/src/render/render_core.cpp`), and the program / texture-unit / attribute
/ buffer-binding caches, the M0 buffer accounting and the draw counters (`gl_state.cpp`), the two
streaming rings (`gl_stream.cpp`), texture uploads (`gl_textures.cpp`) and shader compilation
(`gl_programs.cpp`) moved under `port/src/render/gl/` unchanged. The texture binding, blend, depth
test and write, cull and polygon offset are now **logical** state applied at the next draw (the
bridge, below).

## Matrix stacks
Owned by the renderer since G.5b-0 (`R::MatrixStack`, R2): `glMatrixMode`/`glPushMatrix`/
`glTranslatef`/... forward to `R::mv()`, `R::proj()` or `R::uv()`, so ported and unported code share
one stack, and `glGetFloatv(GL_*_MATRIX)` reads them. (The `GL_MATRIX_PALETTE_OES` mode, which had no
stack, went with the palette emulation in G.5b-5.)
They must seed to identity in their constructor. A pass-14 bug left static matrices
zero-initialized, which silently zeroed `u_texmat` and blacked out the entire menu
with no GL error — the kind of failure that only shows up as "nothing renders,"
not a GL error you can catch.

## One GLSL program (two until G.5b-5)
GLSL ES 1.00 forbids indexing a uniform array by an attribute value, which is exactly
what matrix-palette (creature) skinning needs, so the shim had a **second** GLSL ES 3.00
program (`kSkinVertexShader`) until G.5b-5 moved the creatures to the renderer's program 4
(`R::drawSkinned`, [renderer.md](../renderer.md)). Every program, the shim's and the renderer's,
binds the same attribute slot numbers, so switching programs never rebinds vertex state.

- The palette matrix maps object space straight to eye space, because `Model.cpp`
  reads back `GL_MODELVIEW_MATRIX` already including a `glScalef` — so the skin
  shader multiplies by `u_proj`, not a combined `u_mvp`.
- `glVertexAttribPointer` defaults any unsupplied vector components to `(_, 0, 0, 1)`.
  A 4-wide bone-weight loop over a POD model with only 2–3 real influences reads a
  phantom `1.0` weight from the unset 4th slot — handled via an explicit
  `u_boneCount` uniform rather than relying on the default.

### Which dialect each target gets — and the switch is the CONTEXT, not the toolchain
`rgl::shaderPreamble()` (`port/src/render/gl/gl_programs.cpp`, was `eden_gl_preamble()`) writes the version line and the `EDEN_ATTR`/`EDEN_VARY`/`EDEN_TEX2D`/
`EDEN_FRAGCOLOR` aliases. There are two dialects, chosen by **`EDEN_GL_ES_DIALECT`**:

| Target | Context | Main pair | Skin pair |
|---|---|---|---|
| **iOS** (Phase N Stage 4.1) | **OpenGL ES 3.0** | GLSL ES 1.00 | `#version 300 es` |
| macOS / Linux / Windows | desktop GL 4.1 core | `#version 330 core` + aliases | `#version 330 core` |

It read `#if defined(__EMSCRIPTEN__)` until iOS arrived (the web build was the other ES target), and
that test answers "desktop" on a GLES target: every shader the engine draws with would have failed to compile, and the symptom is a
black screen with a shader log in it rather than an error anywhere near the cause. **A
`#if defined(<toolchain>)` standing in for a property of the RUNTIME is a bug waiting for the
second target that has that property.** `native/CMakeLists.txt` defines it on the iOS arm.

The other two ES-vs-desktop differences live in `gl_context_native.cpp`, not here: ES needs no
global VAO (binding 0 is legal; only desktop core profile forbids it) and has no
`GL_PROGRAM_POINT_SIZE` (`gl_PointSize` is always enabled), so iOS skips both.

## The lazy-apply bridge (G.5b-0, R3)
`glBindTexture(GL_TEXTURE_2D)`, `glBlendFunc`, `glDepthMask`, `glPolygonOffset` and
`glEnable`/`glDisable` of `GL_BLEND`, `GL_DEPTH_TEST`, `GL_CULL_FACE`, `GL_POLYGON_OFFSET_FILL` only
record **logical** state (`rgl::logical()`); no GL call. Every shim draw applies it
(`rgl::applyLogical`) against **actual**, the shadow of what GL holds, issuing only deltas; a renderer
draw applies its own state the same way (`applyRenderer`). So a ported `R::` draw neither inherits a
texture or blend mode from ES 1.1 code nor leaves one behind for it. Anything that needs the real
state now flushes it first:
- `glTexParameteri`, `glTexImage2D`, `glCompressedTexImage2D` (wrapped since G.5b-0 for that reason)
  act on the bound texture: `flushTexture()` first.
- `glClear` with the depth bit honours the depth mask: `flushDepthMask()` first.
- `glGetIntegerv(GL_TEXTURE_BINDING_2D)` answers the logical binding (SignRenderer saves
  and restores around its own binds; Texture2D no longer binds since G.5b-3).
- `glDeleteTextures` reverts a deleted binding to 0 in both logical and actual, as GL does.
A new direct GL call that depends on one of these states (a raw `glTexSubImage2D`, a `glClear` from
port code) must flush first, or it acts on a stale binding. The bridge's own calls are not counted in
`eden_gl_stat()`'s setup counters (they were engine passthroughs before). `--render-selftest-gl`
checks it (R3 lines). After a context loss `rgl::invalidateAll()` forgets actual, so the next draw
re-applies the logical state in full.

## Draw-state cache
A dirty-tracking cache shadows: the bound program, all 17 uniforms (cached per
program object), the 7 attribute specs, `glActiveTexture`, and buffer bindings —
issuing a real GL call only on an actual delta. Measured 34.4 → 4.5–5.9 setup calls
per draw (7.4–7.6× fewer). Two invariants if you touch this code:
- The cache write must happen **outside** any `force ||` short-circuit in the
  set-state helpers, or a forced-but-identical set desyncs the cache from reality.
- `eden_gl_shim_invalidate_gl_objects()` must reset every cache after any GL object
  deletion — a recycled name (see below) must never be trusted as "same object."

`GL_ARRAY_BUFFER`/`GL_ELEMENT_ARRAY_BUFFER` bindings are the one piece of draw state
the *engine itself* also writes directly, so they're tracked against
`g_bound_*_buffer` mirror variables rather than folded into a VAO. VAOs were
considered and rejected: `TerrainChunk.cpp` binds its EBO outside of any draw call,
which would corrupt a non-default VAO's element-array binding as a side effect.

`glDeleteBuffers` must invalidate any attribute cache entry naming the deleted
buffer — GL detaches deleted buffers from all binding points, and `glGenBuffers`
recycles names, so a stale cache entry can silently point at a *different*, newer
buffer with the same name. `TerrainChunk.cpp` deletes and regenerates per-chunk VBOs
every streaming boundary (several call sites), making this a live hazard, not a
theoretical one.

## Client-memory streaming (Q.5, 2026-10-22)
Core GL and ES3 have no client-side vertex arrays, so every draw whose arrays point at CPU memory
(HUD and menus, object batches, `Fire`, `BlockBreak`, sky/trails) copies them into GL buffers first.
`eden_gl_setup_attributes(lo, hi)` does it in three steps:
- **Groups.** Enabled client arrays that share a stride and whose first vertex's bytes all fit inside
  one stride window (an interleaved struct: `BlockBreak`'s position/normal/colour/texcoord) are
  copied **once**; each member points at its byte offset inside that copy.
- **Range.** Only vertices `[lo, hi)` are copied, so vertex `lo` becomes index 0. The function
  returns that rebase: `glDrawArrays` draws from `first - base`; `glDrawElements` with client
  indices scans min and max and subtracts `base` from each index while copying them. A draw that
  also has a VBO-resident array isn't rebased (base 0).
- **Rings.** The copies go into two shared rings (`rgl::g_vring` 4 MiB for vertices, `rgl::g_iring`
  256 KiB for indices; `port/src/render/gl/gl_stream.cpp` since G.5b-0, which the renderer's
  client-memory draws use too) through `glMapBufferRange(WRITE | INVALIDATE_RANGE | UNSYNCHRONIZED)`. A ring is
  orphaned (`glBufferData(NULL)`) only when the next range doesn't fit, so no range is rewritten
  inside one storage generation and nothing waits on the GPU. A draw bigger than the ring grows it.
  If the map returns NULL the bytes are staged and sent with `glBufferSubData`.

Each range starts 16-byte aligned plus the source address mod 4, so members keep their client
alignment. Client slots now get a new `glVertexAttribPointer` each draw (the offset moves); VBO
slots are still elided. `glMapBufferRange` / `glUnmapBuffer` are in the non-Apple loader
(`gl_loader_native.h`, both lists). Before Q.5 each enabled slot was a separate
`glBufferData(STREAM_DRAW)` of `[0, maxIndex]`: `BlockBreak` uploaded 4 × 640 KB a draw once its
ring wrapped. Measured on the M2 (`sample`, 8 s of repeated TNT blasts, main thread):
`eden_gl_setup_attributes` 67–70 → 17–28 samples, `BlockBreak::render` 18–23 → 2–8,
`glBufferData` 39–52 → 0. UI pixels are unchanged (`--shot` diffs only in the animated background).
The bench is `TESTERS/q5-shim-streaming/bench.sh`; the iPad Air 2 number is still owed.

**Only arrays the program reads are copied (G.2s-1, 2026-10-24).** `eden_gl_slot_live()` treats an
enabled `GL_TEXTURE_COORD_ARRAY` as off while `GL_TEXTURE_2D` is disabled: both programs then set
`u_useTexture` 0 and never sample `v_texcoord`. The engine leaves the texcoord array enabled across
untextured draws all the time (`GLW::fill` only `glDisable(GL_TEXTURE_2D)`s before
`Graphics::drawRect`), with its pointer still aimed at the previous draw's stack array, so copying
it was a 32-byte stack over-read that a windowed ASan run (home menu) caught. Real ES1 drivers read
it too; the engine is not changed. Pixels unchanged (`--shot` A/B: opaque UI byte-identical, the rest
differs only where it animates). A new attribute the shader ignores under some state should be
added to that function, not copied.

## Context guard and context loss
Each guarded shim wrapper forwards only if `eden_gl_have_context()`; with no context (headless, or a failed
`eden_gl_context_create`) GL calls no-op and the engine keeps running — that is what the headless
harness modes rely on. Native never loses its context (`eden_gl_context_is_lost()` is 0); the web
build's WebGL context-loss handling is at the tag.

## Drawable size vs. logical screen size
The drawable is dynamic; the engine's notion of screen size is not. On native the window
manager owns it: the resize handler calls `eden_set_drawable_size()` (`gl_context_native.cpp`;
the web build scaled a CSS box × devicePixelRatio there), and the drawable is the fitted box
placed in the real framebuffer by `eden_native_gl_set_letterbox`. But
`SCREEN_WIDTH`/`SCREEN_HEIGHT` (the globals the engine reads) are untouched by it — the
drawable and the point space are two independent things. The backing store is clamped to
4096px/axis (risk of exceeding `GL_MAX_RENDERBUFFER_SIZE` otherwise).

**This is the root cause of the mobile touch-offset bug** fixed during the web port ("fix mobile
touch offset in mine/build raycast"):
`Util.cpp`'s `findWorldCoords` raycast reads back whatever `GL_VIEWPORT` currently reports
and assumes it matches the engine's own POINT space × `SCALE_*`. Once the drawable became
dynamically sized, the real viewport no longer matched that. Fix: `eden_gl_glGetIntegerv(
GL_VIEWPORT, …)` answers `kPickViewport` — the POINT space × `SCALE_*` — for picking
purposes regardless of the real drawable size; the real size is tracked separately in
`g_viewport` but deliberately not surfaced to this one call site.

`kPickViewport` was a compile-time `{0, 0, 1136, 640}` until audit rows D1/D4 made the point
space itself derived (window aspect × a UI-scale setting — see
[ui.md](../ui.md#the-point-space-derived-not-pinned-audit-rows-d1--d4)). It is now a variable that
`src/seam/DisplayProfile.cpp` writes through `eden_gl_set_pick_viewport()` on every metrics
change; its initialiser is still the classic 568×320@2x, which is what the headless build and the
pre-first-layout frames get. **The invariant, not the number, is what matters: this answers the
POINT space, never the real drawable.** Getting it wrong is invisible at the screen centre and
grows toward the edges. Native `--display-selftest` asserts it at every point space it visits.

## Creature (matrix-palette) skinning emulation: deleted in G.5b-5 (2026-10-28)
The shim emulated `GL_OES_matrix_palette` (palette state, `glCurrentPaletteMatrixOES`/`glMatrixIndexPointerOES`/
`glWeightPointerOES`, a second skinning program, and `GL_EXTENSIONS` answered with the extension prepended because
`LoadModels` gated on it) from pass 27 until G.5b-5 moved `Model.cpp` onto `R::drawSkinned`; the code is in git
history before that tier. What is left of it: `glGetString(GL_EXTENSIONS)` is forwarded unmodified; `glLightfv` and
`glLightModelfv` are no-ops (their only consumer was the skinning program; the `GL_LIGHT0/1` enables are still
mirrored for `--state-trace`); `ATTR_MATRIXINDEX`/`ATTR_WEIGHT` stay as renderer attribute slots.
- The `.edm` models (since LG.7; `.pod` before) sit flat at the bundle root, matching the original Xcode project's
  Copy-Bundle-Resources layout — skin PNGs for the same models too. **A binary run outside its build directory finds
  the repo root as its bundle, where there is no `Moof.edm`: creatures silently never load** (an A/B reference copied
  out of `native/build` then shows "identical" models passes because both are empty). Run such a copy with
  `EDEN_BUNDLE=<abs>/native/build`; `--scene-shot` logs `[eden-scene] … models {"nNumMesh":…}` to check.

## Point sprites (fire/smoke particles) need their own texturing gate
`u_useTexture` was gated on `g_texture2d_enabled && g_arrays[ATTR_TEXCOORD].enabled` — correct
for ordinary textured triangles, wrong for `GL_POINTS`. ES1 point sprites get their UV from
`GL_COORD_REPLACE_OES` (this shim's `gl_PointCoord`), not the texcoord client array, and
`SpecialEffects::render()` (`Classes/SpecialEffects.cpp`) deliberately
`glDisableClientState(GL_TEXTURE_COORD_ARRAY)`s before drawing the fire/smoke `GL_POINTS` for
exactly that reason. The old gate read that as "not texturing," dropped `u_useTexture` to 0, and
the point sprites silently fell back to a flat vertex-colored square — reported as "fire looks
like plain colored squares instead of a smoke/ember sprite." Fix: the gate now also passes when
`mode == GL_POINTS && g_point_sprite_enabled`, mirroring the shader's own `mix(v_texcoord,
gl_PointCoord, u_pointSprite)` UV selection. Both `glDrawArrays`/`glDrawElements` pass their real
`mode` into `eden_gl_apply_uniforms(mode)` already, so no call-site change was needed.

**Point sizes follow the framebuffer, so a scaled scene pass compensates (N.4.11, 2026-10-04).**
ES1 point sizes are in framebuffer pixels. Native can render the 3D pass into a smaller (or larger)
offscreen framebuffer and upscale it (`eden_scene_pass_begin/end`, `gl_context_native.cpp`), which
would make every particle `1/scale` times too big on screen. `g_point_px_scale`
(`gl_shim_internal.h`) carries the scale into the draw path, which divides the three attenuation
coefficients by `scale²` on upload: `size / sqrt((a + b·d + c·d²)/s²)` is `size · s`, with no shader
change. It is exactly 1 at 100%, where the uploaded values are bit-identical to before.

## Texture uploads: the mip flag, the counter, the N.4.9 probe and experiments
The code is `port/src/render/gl/gl_textures.cpp` since G.5b-0 (`rgl::texParameteri`/`texImage2D`); the
shim's `glTexParameteri`/`glTexImage2D` flush the logical binding and call it, and `R::texCreate`
uploads through the same path, so every rule below holds for both.
ES1's `GL_GENERATE_MIPMAP` is a texture *parameter*; GLES2+ has only `glGenerateMipmap()` after the
upload, so `eden_gl_glTexParameteri` records it as a one-shot flag and `eden_gl_glTexImage2D` acts on
it. Stock `Texture2D::initData` sets it on **every** texture (both of its branches), so every texture
the engine uploaded, HUD art and text included, carried a mip chain. **Since P.15 (2026-10-13) the seam's `initData` (`Texture2D.cpp`) sets it only when `genMips` is true**; the other branch uses `GL_NEAREST` min filtering and no chain. `g_tex_upload_bytes` counts
level 0 × 4/3 for those (`eden_debug_gl_tex_bytes()`).

**Every upload ends in a `glFlush()` (`g_tex_flush`, N.4.9's fix, 2026-10-04).** Without it, the iOS
GL driver on an iPad Air 2 held **192 MB** of `IOAccelerator` memory for the 45 MB the ~150 start-up
uploads amount to, plus ~80 MB more on the first frames: it keeps each upload's staging memory and
`glGenerateMipmap`'s working memory until the work is submitted, and nothing submitted it until the
first frame presented. With a flush after each upload the same textures cost 1.0–1.3× their nominal
size, as they always did on the Mac, and the 64z footprint went **338 → 145 MB**. The Mac costs
~1.0× either way. Measured, not assumed: `WORKING/n49-r4-device-results-2026-10-04.md`.

**Uploads are NPOT since Q.2 (2026-10-22).** Every context the port creates is ES 3.0 or desktop core
(4.1/3.2), where non-power-of-two textures are core with `GL_REPEAT` and mip chains, so
`Texture2D.cpp` no longer pads an image to a POT canvas unless the caller passed `sizeToFit` (only the
two block atlases, which are POT anyway); density-built labels upload at their raster size too. The
padding was a third of decoded texture memory (149 unique textures on the desktop `--shot` tour:
46.7 → 31.2 MiB; the menu's `ipad~pinwheel.png` alone 16 → 9.5 MiB). The shim's `glTexImage2D` needs nothing:
the port never changes `GL_UNPACK_ALIGNMENT` from 4, so the seam rounds an NPOT canvas width up to keep a
16-bit (2 texels), RGB888 or 8-bit (4 texels) row aligned. Pixel-diff of the tour against the POT build:
identical apart from NEAREST picking the other texel where @2x art lands at exactly 2:1 (texel
boundaries; X.23) and the time-driven pinwheel angle. Plan: `WORKING/npot-textures-plan-2026-10-22.md`.

**One- and two-channel uploads since Q.4 (2026-10-22).** `Texture2D.cpp` uploads a grey+alpha PNG
(stb `comp == 2`) as LA88 when the caller asked for `Automatic` (the only such asset is the menu's
`ipad~pinwheel.png`: 9.5 → 4.75 MiB). ES 3.0 (iOS) takes `GL_LUMINANCE_ALPHA` as is; desktop core GL
removed `GL_LUMINANCE`/`GL_ALPHA`/`GL_LUMINANCE_ALPHA` (`GL_INVALID_ENUM`, no texture), so on every
non-`EDEN_GL_ES_DIALECT` build the shim's `glTexImage2D` uploads them as `GL_R8`/`GL_RG8` and sets the
texture's swizzle (GL 3.3 core; the context is 4.1) to what the ES 1 format sampled: L → (L,L,L,1),
A → (0,0,0,A), LA → (L,L,L,A). The swizzle is texture-object state and is not reset by a later RGBA
upload into the same name; nothing does that (`Texture2D` gens a fresh name per image). The
`[eden-tex]` probe and the upload byte counter still see the ES format. `--shot` tour before/after:
the menu pages differ only by the clock-driven pinwheel angle. Data: `TESTERS/q4/`.

Two diagnostic hooks sit beside it, both off by default:
- **`g_tex_probe`** (native `--mem-trace=2`): called before the upload, after `glTexImage2D`, after
  `glGenerateMipmap` (and the flush), and after a `glFinish` it issues itself; `HeapProbe_native.cpp`
  turns that into one `[eden-tex]` line of deltas per texture. `Texture2D.cpp` names the asset
  through `eden_gl_debug_tex_label()` (`<text>` for rasterised labels).
- **`g_tex_exp`** (native `--tex-exp=`): `nomips` drops the mip flag and rewrites any `*_MIPMAP_*` min
  filter to `GL_LINEAR` (without that the texture is incomplete and samples black); `storage`
  allocates with `glTexStorage2D` at the exact level count and fills level 0 with `glTexSubImage2D`
  (ES 3.0 only, so iOS only; it made the iPad footprint *worse*, 402 MB against 338); `noflush`
  turns the flush above off, i.e. the pre-fix behaviour, for an A/B. `fp16uv` (I.1, read by
  `gl_programs.cpp`, not the upload path) rounds the fragment texcoord to fp16: the old iOS look
  (`port/docs/renderer.md` "Texcoord precision").

## Debugging note
Add an `fprintf` in the shim and rebuild, or use the diagnostics build's counters
(`eden_gl_stats_*`, `--mem-trace`); `--shot` captures pixels. (The web-era note about browser GL
introspection tools is history.)

### Q.1a notes (2026-10-21)
- **F4:** `EdenLoadPNG` reads the file with `[[NSData alloc] initWithContentsOfFile:]` and releases it right after `stbi_load_from_memory`, so the PNG bytes (≈ 5 MiB at the menu) no longer sit in an autorelease root.
