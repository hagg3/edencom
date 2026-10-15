# GL ES 1.1 → OpenGL ES 3.0 / desktop GL Shim

> Written for the web (WebGL2) build, retired 2026-10-13; the translator itself is what every native
> target runs. Web-only parts are at tag `web-port-final` ([web-port-archive](../../docs/web-port-archive.md)).

`port/src/shim/gl/` (`gl_es1_shim.h`, `gl_fixed_function.cpp`, `gl_shim_internal.h`) implements only
the ~45 GL ES 1.1 fixed-function calls the engine actually makes — a thin, purpose-built shim, not a
general ES1 emulator. The native context, loader and drawable live in `native/src/shim/gl/`
(`gl_context_native.cpp`, `gl_loader_native.cpp`, `gl_es1_extras_native.cpp`). Read [`../../docs/rendering.md`](../../docs/rendering.md) first for the
draw passes, vertex formats, and meshing this shim sits underneath — none of that
changes; this file is about how fixed-function calls get turned into modern GL draws.

## Matrix stacks
Must seed to identity in their constructor. A pass-14 bug left static matrices
zero-initialized, which silently zeroed `u_texmat` and blacked out the entire menu
with no GL error — the kind of failure that only shows up as "nothing renders,"
not a GL error you can catch.

## Two GLSL programs, not one
GLSL ES 1.00 forbids indexing a uniform array by an attribute value, which is exactly
what matrix-palette (creature) skinning needs. Rather than branch in the shared
fixed-function-equivalent shader, skinning is a **second** GLSL ES 3.00 program
(`kSkinVertexShader`). Both programs share attribute slot numbers via one
`eden_gl_setup_attributes()` so switching programs doesn't require rebinding vertex
state.

- The palette matrix maps object space straight to eye space, because `Model.mm`
  reads back `GL_MODELVIEW_MATRIX` already including a `glScalef` — so the skin
  shader multiplies by `u_proj`, not a combined `u_mvp`.
- `glVertexAttribPointer` defaults any unsupplied vector components to `(_, 0, 0, 1)`.
  A 4-wide bone-weight loop over a POD model with only 2–3 real influences reads a
  phantom `1.0` weight from the unset 4th slot — handled via an explicit
  `u_boneCount` uniform rather than relying on the default.

### Which dialect each target gets — and the switch is the CONTEXT, not the toolchain
`eden_gl_preamble()` writes the version line and the `EDEN_ATTR`/`EDEN_VARY`/`EDEN_TEX2D`/
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
considered and rejected: `TerrainChunk.mm` binds its EBO outside of any draw call,
which would corrupt a non-default VAO's element-array binding as a side effect.

`glDeleteBuffers` must invalidate any attribute cache entry naming the deleted
buffer — GL detaches deleted buffers from all binding points, and `glGenBuffers`
recycles names, so a stale cache entry can silently point at a *different*, newer
buffer with the same name. `TerrainChunk.mm` deletes and regenerates per-chunk VBOs
every streaming boundary (several call sites), making this a live hazard, not a
theoretical one.

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
`Util.mm`'s `findWorldCoords` raycast reads back whatever `GL_VIEWPORT` currently reports
and assumes it matches the engine's own POINT space × `SCALE_*`. Once the drawable became
dynamically sized, the real viewport no longer matched that. Fix: `eden_gl_glGetIntegerv(
GL_VIEWPORT, …)` answers `kPickViewport` — the POINT space × `SCALE_*` — for picking
purposes regardless of the real drawable size; the real size is tracked separately in
`g_viewport` but deliberately not surfaced to this one call site.

`kPickViewport` was a compile-time `{0, 0, 1136, 640}` until audit rows D1/D4 made the point
space itself derived (window aspect × a UI-scale setting — see
[ui.md](ui.md#the-point-space-derived-not-pinned-audit-rows-d1--d4)). It is now a variable that
`src/seam/DisplayProfile_web.mm` writes through `eden_gl_set_pick_viewport()` on every metrics
change; its initialiser is still the classic 568×320@2x, which is what the headless build and the
pre-first-layout frames get. **The invariant, not the number, is what matters: this answers the
POINT space, never the real drawable.** Getting it wrong is invisible at the screen centre and
grows toward the edges. Native `--display-selftest` asserts it at every point space it visits.

## Creature (matrix-palette) skinning emulation
- `glGetString(GL_EXTENSIONS)` is *answered*, not forwarded to the real driver: the
  shim prepends `GL_OES_matrix_palette` because `Model.mm` (`IsGLExtensionSupported("GL_OES_matrix_palette")`)
  gates POD-model loading on seeing that string.
- Entry points are installed by `native/src/seam/pvrt_matrix_palette_native.cpp`, a seam
  replacement for the PVRT extension file whose `CPVRTglesExt::LoadExtensions()` installs the
  shim's `GL_OES_matrix_palette` functions (the web build did it with a one-symbol `--wrap`).
- POD assets sit flat at the bundle root, matching the original Xcode project's
  Copy-Bundle-Resources layout — skin PNGs for the same models too; a missing one renders
  creatures as unbound-texture black silhouettes with no GL error.
- `GL_LIGHT0` ambient/diffuse (`glLightfv`) is tracked and applied **only** on this
  skinned path — general lighting for doors/the golden cube is still unimplemented in
  the shim (they render unlit), a deliberately narrow gate rather than a general
  ES1-lighting emulation. Which engine code enables `GL_LIGHTING`: docs/rendering.md "Fixed constraints".

## Point sprites (fire/smoke particles) need their own texturing gate
`u_useTexture` was gated on `g_texture2d_enabled && g_arrays[ATTR_TEXCOORD].enabled` — correct
for ordinary textured triangles, wrong for `GL_POINTS`. ES1 point sprites get their UV from
`GL_COORD_REPLACE_OES` (this shim's `gl_PointCoord`), not the texcoord client array, and
`SpecialEffects::render()` (`Classes/SpecialEffects.mm`) deliberately
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
ES1's `GL_GENERATE_MIPMAP` is a texture *parameter*; GLES2+ has only `glGenerateMipmap()` after the
upload, so `eden_gl_glTexParameteri` records it as a one-shot flag and `eden_gl_glTexImage2D` acts on
it. Stock `Texture2D::initData` sets it on **every** texture (both of its branches), so every texture
the engine uploaded, HUD art and text included, carried a mip chain. **Since P.15 (2026-10-13) the seam's `initData` (`Texture2D_web.mm`) sets it only when `genMips` is true**; the other branch uses `GL_NEAREST` min filtering and no chain. `g_tex_upload_bytes` counts
level 0 × 4/3 for those (`eden_debug_gl_tex_bytes()`).

**Every upload ends in a `glFlush()` (`g_tex_flush`, N.4.9's fix, 2026-10-04).** Without it, the iOS
GL driver on an iPad Air 2 held **192 MB** of `IOAccelerator` memory for the 45 MB the ~150 start-up
uploads amount to, plus ~80 MB more on the first frames: it keeps each upload's staging memory and
`glGenerateMipmap`'s working memory until the work is submitted, and nothing submitted it until the
first frame presented. With a flush after each upload the same textures cost 1.0–1.3× their nominal
size, as they always did on the Mac, and the 64z footprint went **338 → 145 MB**. The Mac costs
~1.0× either way. Measured, not assumed: `WORKING/n49-r4-device-results-2026-10-04.md`.

Two diagnostic hooks sit beside it, both off by default:
- **`g_tex_probe`** (native `--mem-trace=2`): called before the upload, after `glTexImage2D`, after
  `glGenerateMipmap` (and the flush), and after a `glFinish` it issues itself; `HeapProbe_native.cpp`
  turns that into one `[eden-tex]` line of deltas per texture. `Texture2D_web.mm` names the asset
  through `eden_gl_debug_tex_label()` (`<text>` for rasterised labels).
- **`g_tex_exp`** (native `--tex-exp=`): `nomips` drops the mip flag and rewrites any `*_MIPMAP_*` min
  filter to `GL_LINEAR` (without that the texture is incomplete and samples black); `storage`
  allocates with `glTexStorage2D` at the exact level count and fills level 0 with `glTexSubImage2D`
  (ES 3.0 only, so iOS only; it made the iPad footprint *worse*, 402 MB against 338); `noflush`
  turns the flush above off, i.e. the pre-fix behaviour, for an A/B.

## Debugging note
Add an `fprintf` in the shim and rebuild, or use the diagnostics build's counters
(`eden_gl_stats_*`, `--mem-trace`); `--shot` captures pixels. (The web-era note about browser GL
introspection tools is history.)
