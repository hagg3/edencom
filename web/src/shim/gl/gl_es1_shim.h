// gl_es1_shim.h — D2: thin OpenGL ES 1.1 fixed-function -> WebGL2 shim.
//
// WHY THIS EXISTS (see WORKING/web-port-plan.md decision D2 and docs/rendering.md):
// Eden is OpenGL ES 1.1, fixed-function only — no shaders anywhere (docs/rendering.md
// "Fixed constraints"). Emscripten's `-sLEGACY_GL_EMULATION` is unmaintained and known-weak
// exactly where this engine leans on it hardest (fog, the texture-matrix atlas trick,
// two-light GL_LIGHTING). So instead: enumerate the EXACT ES1 fixed-function surface the
// engine calls, declare it here, and implement it (gl_es1_shim.cpp, WIP) as a matrix-stack +
// fixed small-shader-count translator to WebGL2. This is also the seam Stage R7 later swaps
// for a full WebGL2/Three.js-class backend (docs/engine-vs-game.md step 7) — same header,
// different .cpp.
//
// SOURCE OF THIS INVENTORY (verified by grep, not guessed — 2026-07-19):
//   grep -ohE '\bgl[A-Z][A-Za-z0-9_]*' Classes/*.mm Lighting.mm | sort -u
// then cross-referenced per-file to separate "engine, needed here" from "EAGLView.mm /
// Texture2D.mm only, NOT needed here because those .mm files are excluded by CMakeLists.txt
// (replaced by the web canvas/WebGL2-context seam in Stage P2, which talks to WebGL2 directly,
// not through this ES1 surface)". Also cross-checked Classes/project.c (the GLU port —
// gluPerspective et al. are hand-rolled from raw GL calls, not glFrustumf) since it's a
// compiled engine source too.
//
// EXCLUDED from this shim (confirmed callers are seam-only, not engine):
//   glGenFramebuffersOES/glBindFramebufferOES/glFramebufferRenderbufferOES/
//   glRenderbufferStorageOES/glCheckFramebufferStatusOES/glDeleteFramebuffersOES/
//   glDeleteRenderbuffersOES/glDiscardFramebufferEXT/glBindRenderbufferOES/
//   glGenRenderbuffersOES/glGetRenderbufferParameterivOES
//     -> ONLY in Classes/EAGLView.mm (default-framebuffer setup for CAEAGLLayer) and
//        Classes/Texture2D.mm. Both are seam-excluded (see CMakeLists.txt). Stage P2's
//        canvas/WebGL2-context setup (src/seam/EAGLView_web.*) creates its GL context and
//        default framebuffer directly via emscripten_webgl_create_context / WebGL2's native
//        (non-OES) framebuffer API — it does not need this shim to expose the OES names.
//   glMatrixIndexPointerOES/glWeightPointerOES/glCurrentPaletteMatrixOES/glLineWidth/
//   glTexEnvi(partial)/glCompressedTexImage2D
//     *** CORRECTION 2 (Pass 8): glLineWidth is NOT PVRT-only either — Classes/Graphics.mm's
//     initGraphics calls it (glLineWidth(6.6f), just after the VBO setup). It is a real GLES2
//     entry point so it needs no ES1 emulation, but it IS a live engine call and is therefore
//     covered by GROUP 2b's context guard below. Third entry on this list to be wrong; treat
//     the remainder as unverified. ***
//     *** CORRECTION (first successful link): glPointParameterf/glPointParameterfv/
//     glPointSizePointerOES were ALSO listed here as PVRT-only. They are not — Classes/
//     SpecialEffects.mm calls all three for the particle system. They now live in GROUP 8
//     below. The rest of this list still holds. ***
//     -> Present in the PVRT SDK sources (PVRTglesExt.cpp, PVRTPrint3DAPI.cpp — vendored,
//        docs/third-party.md) which are compiled but, per grep, their entry points
//        (CPVRTPrint3D::*, skinning/bone-batch paths) are never called from any engine
//        (non-PVRT) file — Model.mm uses only CPVRTModelPOD::ReadFromFile (data parsing,
//        no GL). TODO P1: confirm the linker drops these via function-level dead-code
//        elimination (-ffunction-sections -Wl,--gc-sections); if not, these PVRT files may
//        need excluding/stubbing too (flagged as an open risk in archive/PORT-STATUS-2026-08-13.md, NOT solved
//        here since it can't be verified without emcc).
//
// GROUPS BELOW mirror the plan's D2 description and rendering.md's pass structure so this
// header doubles as the WebGL2 backend's implementation checklist.
#ifndef EDEN_GL_ES1_SHIM_H
#define EDEN_GL_ES1_SHIM_H

// Emscripten DOES ship a real ES 1.1 header (system/include/GLES/gl.h) — discovered during the
// third real build, when Classes/error.c and the PVRT files failed on ES1-only constants
// (GL_STACK_OVERFLOW/GL_STACK_UNDERFLOW/GL_RGBA) that <GLES2/gl2.h> does not define. Including
// the genuine ES1 header instead gives the complete, authoritative ES1 type + enum surface for
// free, and declares the fixed-function entry points with exactly the signatures the engine
// expects — so this shim's job narrows to *implementing* them (gl_es1_shim.cpp) rather than
// also having to redeclare them correctly. Duplicate identical declarations are legal, so the
// shim's own prototypes below remain valid.
#include <GLES/gl.h>     // ES 1.1: GLenum/GLfloat/GLubyte/... + ALL ES1 enums + fixed-function
                          // prototypes (glMatrixMode, glFogf, glLightfv, client arrays, ...).

// Phase N Stage 0.2 (WORKING/native-migration-plan-2026-09-04.md): portable stand-in for
// EMSCRIPTEN_KEEPALIVE, so gl_fixed_function.cpp's debug/stat exports keep working unmodified
// on a native (non-Emscripten) target instead of needing every call site edited or deleted.
#if defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>
#define EDEN_GL_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define EDEN_GL_EXPORT
#endif

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------------------
// ES1-only enum constants (no GLES2 equivalent — GLES2 dropped the fixed-function pipeline
// entirely). Values are the standard Khronos GL/GLES1 constants, stable since OpenGL 1.x and
// unchanged across every vendor's ES1 headers. TODO P1: cross-check verbatim against a real
// GLES/gl.h once emsdk/an SDK snapshot is available locally (none was found in this repo or
// on this machine to diff against — see archive/PORT-STATUS-2026-08-13.md "Open questions").
// ---------------------------------------------------------------------------------------
#define GL_MODELVIEW                   0x1700
#define GL_PROJECTION                  0x1701
#define GL_TEXTURE                     0x1702
#define GL_MODELVIEW_MATRIX            0x0BA6
#define GL_PROJECTION_MATRIX           0x0BA7
#define GL_TEXTURE_MATRIX              0x0BA8
#define GL_MATRIX_MODE                 0x0BA0

#define GL_VERTEX_ARRAY                0x8074
#define GL_NORMAL_ARRAY                0x8075
#define GL_COLOR_ARRAY                 0x8076
#define GL_TEXTURE_COORD_ARRAY         0x8078

#define GL_FOG                         0x0B60
#define GL_FOG_MODE                    0x0B65
#define GL_FOG_DENSITY                 0x0B62
#define GL_FOG_START                   0x0B63
#define GL_FOG_END                     0x0B64
#define GL_FOG_COLOR                   0x0B66
#define GL_LINEAR                      0x2601   // also a valid GL_FOG_MODE value

#define GL_LIGHTING                    0x0B50
#define GL_LIGHT0                      0x4000
#define GL_LIGHT1                      0x4001
#define GL_AMBIENT                     0x1200
#define GL_DIFFUSE                     0x1201
#define GL_SPECULAR                    0x1202
#define GL_POSITION                    0x1203
#define GL_EMISSION                    0x1600
#define GL_SHININESS                   0x1601
#define GL_AMBIENT_AND_DIFFUSE         0x1602
#define GL_LIGHT_MODEL_AMBIENT         0x0B53
#define GL_LIGHT_MODEL_TWO_SIDE        0x0B52

#define GL_SHADE_MODEL                 0x0B54
#define GL_SMOOTH                      0x1D01
#define GL_FLAT                        0x1D00

#define GL_ALPHA_TEST                  0x0BC0
#define GL_ALPHA_TEST_FUNC             0x0BC1
#define GL_TEXTURE_ENV                 0x2300
#define GL_TEXTURE_ENV_MODE            0x2200
#define GL_TEXTURE_ENV_COLOR           0x2201
#define GL_MODULATE                    0x2100
#define GL_DECAL                       0x2101
#define GL_BLEND_ENV                   0x0BE2

// ---------------------------------------------------------------------------------------
// GROUP 1 — Matrix stack. Callers: Graphics.mm (beginTerrain/endTerrain/prepareScene/
// beginHud/prepareMenu), Terrain.mm (render/render2, per-chunk translate + the
// glScalef(.25,.25,.25) terrain scale), TerrainChunk.mm, Camera.mm (view matrix), Model.mm
// (creature transforms), World.mm, Player.mm, Menu*.mm, Toolbar.mm. glMultMatrixf is used by
// Classes/project.c's hand-rolled gluPerspective/gluLookAt (the GLU port) — NOT by ES1
// glFrustumf, which the engine never calls (grep-confirmed absent from Classes/*.mm +
// Lighting.mm + project.c/glue.c).
// A real WebGL2 backend has no matrix stack — this shim owns one (16-float arrays × a small
// stack depth per mode) and uploads to the active shader's uniform on each draw call.
// ---------------------------------------------------------------------------------------
void glMatrixMode(GLenum mode);
void glLoadIdentity(void);
void glLoadMatrixf(const GLfloat* m);
void glMultMatrixf(const GLfloat* m);
void glPushMatrix(void);
void glPopMatrix(void);
void glOrthof(GLfloat left, GLfloat right, GLfloat bottom, GLfloat top, GLfloat near_, GLfloat far_);
void glScalef(GLfloat x, GLfloat y, GLfloat z);
void glTranslatef(GLfloat x, GLfloat y, GLfloat z);
void glRotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z);
// glFrustumf: NOT called anywhere in the engine (grep-confirmed) — declared for API
// completeness / in case a future refactor introduces it, but has no known caller today.
void glFrustumf(GLfloat left, GLfloat right, GLfloat bottom, GLfloat top, GLfloat near_, GLfloat far_);

// ---------------------------------------------------------------------------------------
// GROUP 2 — Client vertex arrays. Callers: TerrainChunk.mm (render/render2 — vertexStructSmall
// via glVertexPointer/glColorPointer/glTexCoordPointer + glDrawElements/glDrawArrays),
// Graphics.mm, Model.mm (vertexObject immediate batches + glNormalPointer for creature
// env-map lighting), Firework.mm, SpecialEffects.mm, BlockBreak.mm, Fire.mm, Hud.mm.
// glDrawArrays/glDrawElements/glEnable/glDisable(GL_*_ARRAY via EnableClientState) are the
// hot path — rendering.md's whole "Frame passes" section runs through this group every frame.
// GLES2 has no client-array / EnableClientState concept — this shim maps each enabled array
// to a bound vertex-attribute slot in the fixed vertex-color/texcoord shader (see
// docs/rendering.md vertex formats: vertexStructSmall / vertexObject / vertexpStruct).
// ---------------------------------------------------------------------------------------
void glEnableClientState(GLenum array);
void glDisableClientState(GLenum array);
void glVertexPointer(GLint size, GLenum type, GLsizei stride, const void* pointer);
void glColorPointer(GLint size, GLenum type, GLsizei stride, const void* pointer);
void glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const void* pointer);
void glNormalPointer(GLenum type, GLsizei stride, const void* pointer);
// glDrawArrays / glDrawElements / glBindBuffer / glBufferData / glBufferSubData / glGenBuffers /
// glDeleteBuffers are declared by <GLES2/gl2.h> above (identical signatures in ES1 and ES2) —
// NOT redeclared here, but listed as part of this group's inventory for completeness:
//   glDrawArrays, glDrawElements, glGenBuffers, glDeleteBuffers, glBindBuffer, glBufferData,
//   glBufferSubData  (TerrainChunk.mm prepareVBO/render/render2)
//
// ---------------------------------------------------------------------------------------
// GROUP 2b — CONTEXT GUARD over the "passthrough" GL surface (added Pass 8).
//
// The assumption recorded above — "a real WebGL context exists by the time any GL call
// fires" — is FALSE for this engine. `World::World()` -> `Graphics::initGraphics()`
// (Classes/Graphics.mm:170-188, unmodified engine code) issues glGenBuffers/glBindBuffer/
// glBufferData/glEnable/glLineWidth/glShadeModel DURING CONSTRUCTION, long before
// `world->update(etime)` — and, in the browser, before Stage P2's context-creation seam
// (`EAGLView_web.mm`, still TODO P2) has run. Under plain `node build-st/eden.js` there is
// no canvas at all, so Emscripten's real bindings dereference an undefined `GLctx` and throw.
//
// So every passthrough entry point the engine actually calls is routed through a thin
// `eden_gl_*` wrapper (macro-renamed below; implemented in gl_es1_shim.cpp):
//     context live  -> forwards to the real Emscripten/WebGL binding, unchanged.
//     no context    -> no-ops, and hands back plausible-but-fake object names so the engine's
//                      own bookkeeping (TerrainChunk's vertexBuffer handles, which it tests
//                      against 0) stays self-consistent. Warns once on stderr — this path is
//                      deliberately visible, not a silent swallow.
// "Context live" is `emscripten_webgl_get_current_context() != 0` — the real thing, not a
// flag the seam sets, so it cannot drift out of sync with reality.
//
// This is NOT throwaway work for Stage P2 (cf. PORT-STATUS Pass 7's "OPEN" option 2, which
// framed it as such): once P2's `EAGLView_web.mm` creates and makes-current a WebGL2 context,
// every wrapper below starts forwarding and the guard costs one predicate per call. What P2
// still owes is the *draw-path translation* (GROUP 2's client arrays -> attribs + shader),
// which is orthogonal to this.
//
// Option 1 from that same writeup (wire `headless-gl` into the EDEN_THREADED=OFF build) was
// rejected: `headless-gl` is a native node addon, and Emscripten's GL bindings talk to
// `GLctx`/`document.createElement("canvas")` rather than to an arbitrary gl object, so
// adopting it means faking a DOM for the node build — more moving parts than the thing it
// unblocks, and it buys nothing the browser build can use.
// ---------------------------------------------------------------------------------------
int  eden_gl_have_context(void);   // 1 if a real GL context is current; 0 = guarded/no-op mode

// ---------------------------------------------------------------------------------------
// GROUP 2c — Context ownership (added Pass 9, Stage P2).
//
// The WebGL2 context is created HERE rather than in EAGLView_web.mm, even though the seam is
// the conceptual owner, for one measured reason: `Graphics::initGraphics()` issues real GL
// during `World::World()` (see GROUP 2b), so the context must be live BEFORE the seam's
// object graph is built — i.e. before `eden_seam_main()`. src/entry/eden_main.cpp calls
// `eden_gl_context_create()` as its first act; EAGLView_web's createFramebuffer/
// deleteFramebuffer forward here so the retina-swap path (which recreates the drawable at a
// new pixel density) still reads naturally through the seam.
//
// Returns 1 on success, 0 if no canvas exists (the `node eden.js` case) — a 0 leaves GROUP
// 2b's guard closed and the headless path intact, which is exactly what the debug build wants.
// It is deliberately NOT an error: both configurations are supported.
// ---------------------------------------------------------------------------------------
int  eden_gl_context_create(int drawable_width, int drawable_height);
void eden_gl_context_destroy(void);
// Drawable size in PIXELS (not CSS px). NOT what eden_gl_glGetIntegerv answers for GL_VIEWPORT —
// that is a fixed 1136x640 (kPickViewport in the .cpp), deliberately decoupled from this since
// item #6 (dynamic drawable); see that constant's comment for why Util.mm's unproject/picking
// needs the fixed answer, not the real one.
void eden_gl_context_get_drawable_size(int* width, int* height);
// The size eden_gl_glGetIntegerv answers GL_VIEWPORT with — see kPickViewport in the .cpp. It is
// SCREEN_WIDTH*SCALE_WIDTH x SCREEN_HEIGHT*SCALE_HEIGHT, i.e. the engine's retina-doubled POINT
// space, NOT the real drawable. Was a compile-time 1136x640 constant until the point space itself
// became derived (audit D1/D4); DisplayProfile_web.mm now calls this whenever it changes. Passing
// a non-positive size is ignored, so the boot default survives an early/garbage call.
void eden_gl_set_pick_viewport(int width, int height);
// Force the drawable (canvas backing store) to a specific PIXEL size and re-seed the g_viewport
// MIRROR to match (kept for parity with real GL state — no longer what GL_VIEWPORT answers with,
// see kPickViewport). This is the RETINA path: the engine renders in POINTS (SCREEN_WIDTH/HEIGHT,
// e.g. 568x320) but a retina drawable is 2x that in pixels (1136x640) — exactly as the original
// Classes/EAGLView.mm -createFramebuffer sized its CAEAGLLayer renderbuffer at contentsScale 2
// and then `glViewport(0,0,framebufferWidth,framebufferHeight)`. Called from EAGLView_web's
// -establishScreenMetrics once IS_RETINA and SCREEN_WIDTH/HEIGHT are known (which is AFTER
// context creation but BEFORE any engine GL — see main_web.cpp's load-bearing order). Safe to
// call headless (updates only the mirror; no canvas resize) and safe to call with the same size
// it already is (a no-op). Non-positive args are ignored.
void eden_gl_context_set_drawable_size(int width, int height);
// Binds the default framebuffer + full-drawable viewport. EAGLView_web -setFramebuffer.
void eden_gl_context_bind_default_framebuffer(void);

// ---------------------------------------------------------------------------------------
// Exports for the host page (perf-audit items #4/#5/#6). All three are EMSCRIPTEN_KEEPALIVE
// in the .cpp, i.e. reachable as Module._<name> with no ccall/cwrap.
//
//   eden_gl_context_is_lost()  — 1 after `webglcontextlost` until a successful recreate. The
//                                shim also PUSHES both events to window.EdenRenderer (see the
//                                context-loss block in the .cpp) — the page does not poll.
//   eden_set_drawable_size()   — dynamic drawable: CSS box x min(devicePixelRatio, cap) x
//                                render_scale, computed in JS. NEVER changes the engine's
//                                568x320 point space; clamps to a GL-safe maximum dimension.
//   eden_gl_stat(which)        — per-frame draw-path call accounting for the dirty-tracking
//                                caches: 0=draws, 1=setup calls issued, 2=elided by the caches,
//                                3=issued+elided. Last completed frame.
// ---------------------------------------------------------------------------------------
int  eden_gl_context_is_lost(void);
void eden_set_drawable_size(int width, int height);
int  eden_gl_stat(int which);
// Rotates eden_gl_stat()'s counters. Call once per frame, before the engine renders — glClear is
// not a usable boundary here (prepareScene does not clear the colour buffer).
void eden_gl_stats_frame_boundary(void);

void eden_gl_glGenBuffers(GLsizei n, GLuint* buffers);
void eden_gl_glDeleteBuffers(GLsizei n, const GLuint* buffers);
void eden_gl_glBindBuffer(GLenum target, GLuint buffer);
void eden_gl_glBufferData(GLenum target, GLsizeiptr size, const void* data, GLenum usage);
void eden_gl_glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void* data);
void eden_gl_glDrawArrays(GLenum mode, GLint first, GLsizei count);
void eden_gl_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices);
void eden_gl_glGenTextures(GLsizei n, GLuint* textures);
void eden_gl_glDeleteTextures(GLsizei n, const GLuint* textures);
void eden_gl_glBindTexture(GLenum target, GLuint texture);
void eden_gl_glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width,
                          GLsizei height, GLint border, GLenum format, GLenum type,
                          const void* pixels);
void eden_gl_glTexParameteri(GLenum target, GLenum pname, GLint param);
void eden_gl_glEnable(GLenum cap);
void eden_gl_glDisable(GLenum cap);
void eden_gl_glBlendFunc(GLenum sfactor, GLenum dfactor);
void eden_gl_glDepthMask(GLboolean flag);
void eden_gl_glClear(GLbitfield mask);
void eden_gl_glClearColor(GLclampf r, GLclampf g, GLclampf b, GLclampf a);
void eden_gl_glViewport(GLint x, GLint y, GLsizei width, GLsizei height);
void eden_gl_glLineWidth(GLfloat width);
void eden_gl_glPolygonOffset(GLfloat factor, GLfloat units);
void eden_gl_glHint(GLenum target, GLenum mode);
void eden_gl_glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format,
                          GLenum type, void* pixels);
void eden_gl_glGetIntegerv(GLenum pname, GLint* params);
// glGetFloatv is intercepted for the THREE matrix pnames (GL_MODELVIEW_MATRIX/
// GL_PROJECTION_MATRIX/GL_TEXTURE_MATRIX) — WebGL2 has no such query, so the shim's own
// tracked stack top is the only possible answer. Camera.mm's picking raycast and Model.mm read
// these; anything else forwards. (Was the GROUP 6 "TODO P2" below; done Pass 9.)
void eden_gl_glGetFloatv(GLenum pname, GLfloat* params);
GLenum eden_gl_glGetError(void);
const GLubyte* eden_gl_glGetString(GLenum name);

// ---------------------------------------------------------------------------------------
// GROUP 8 — GL_OES_matrix_palette emulation (pass 27, creature skinning).
//
// WebGL has no matrix-palette extension, so the seven creature PODs could not be drawn at all
// (Classes/Model.mm's LoadModels() early-returns when the extension is missing, leaving the
// POD objects null and the draw path to crash — see src/seam/model_render_guard.cpp for the
// history). This is the emulation: the shim ADVERTISES the extension in glGetString
// (GL_EXTENSIONS), tracks the palette + the two extra vertex arrays here, and does the actual
// blend on the GPU in a second shader program (GLSL ES 3.00, so it can index the palette
// uniform array by attribute value — GLSL ES 1.00 forbids that, which is why it is a separate
// program rather than a branch in the main one).
//
// These four are the exact signatures CPVRTglesExt's function-pointer table expects;
// src/seam/pvrt_matrix_palette.cpp installs them there (the engine's own PVRGetProcAddress
// resolves to NULL on this platform, so the pointers must be written from outside).
// ---------------------------------------------------------------------------------------
void eden_gl_glCurrentPaletteMatrixOES(GLuint matrixpaletteindex);
void eden_gl_glLoadPaletteFromModelViewMatrixOES(void);
void eden_gl_glMatrixIndexPointerOES(GLint size, GLenum type, GLsizei stride,
                                     const GLvoid* pointer);
void eden_gl_glWeightPointerOES(GLint size, GLenum type, GLsizei stride, const GLvoid* pointer);

// The rename. Defined AFTER <GLES/gl.h> above, so the real prototypes (and the real symbols,
// which gl_es1_shim.cpp calls after #undef-ing these) are untouched. EDEN_GL_NO_GUARD lets
// that .cpp — and only it — see the unguarded names.
#ifndef EDEN_GL_NO_GUARD
#define glGenBuffers      eden_gl_glGenBuffers
#define glDeleteBuffers   eden_gl_glDeleteBuffers
#define glBindBuffer      eden_gl_glBindBuffer
#define glBufferData      eden_gl_glBufferData
#define glBufferSubData   eden_gl_glBufferSubData
#define glDrawArrays      eden_gl_glDrawArrays
#define glDrawElements    eden_gl_glDrawElements
#define glGenTextures     eden_gl_glGenTextures
#define glDeleteTextures  eden_gl_glDeleteTextures
#define glBindTexture     eden_gl_glBindTexture
#define glTexImage2D      eden_gl_glTexImage2D
#define glTexParameteri   eden_gl_glTexParameteri
#define glEnable          eden_gl_glEnable
#define glDisable         eden_gl_glDisable
#define glBlendFunc       eden_gl_glBlendFunc
#define glDepthMask       eden_gl_glDepthMask
#define glClear           eden_gl_glClear
#define glClearColor      eden_gl_glClearColor
#define glViewport        eden_gl_glViewport
#define glLineWidth       eden_gl_glLineWidth
#define glPolygonOffset   eden_gl_glPolygonOffset
#define glHint            eden_gl_glHint
#define glReadPixels      eden_gl_glReadPixels
#define glGetIntegerv     eden_gl_glGetIntegerv
#define glGetFloatv       eden_gl_glGetFloatv
#define glGetError        eden_gl_glGetError
#define glGetString       eden_gl_glGetString
#endif // EDEN_GL_NO_GUARD

// ---------------------------------------------------------------------------------------
// GROUP 3 — Fog. Callers: Graphics.mm (beginTerrain sets GL_FOG state; setZFAR re-derives the
// linear fog band per docs/rendering.md), Terrain.mm (fog color tracks sky color).
// ---------------------------------------------------------------------------------------
void glFogf(GLenum pname, GLfloat param);
void glFogfv(GLenum pname, const GLfloat* params);
// glFogx (fixed-point) is in the plan's description but NOT found by grep in
// Classes/*.mm+Lighting.mm — the engine only calls the float variants. Declared here anyway
// since the plan names it explicitly as an expected call; verify against real device build
// notes if one ever surfaces (TODO, low priority — no known caller today).
void glFogx(GLenum pname, GLfixed param);

// ---------------------------------------------------------------------------------------
// GROUP 4 — Lighting. Callers: Terrain.mm/Graphics.mm (doors: GL_LIGHT0; golden cube:
// GL_LIGHT1 spheremap specular — docs/rendering.md "Fixed constraints": "the only uses of GL
// lighting are doors and the golden cube's env-mapped specular highlight"). glEnable/glDisable
// with GL_LIGHTING/GL_LIGHT0/GL_LIGHT1 route through the shared GROUP 6 glEnable/glDisable
// (from <GLES2/gl2.h>), not redeclared here.
// ---------------------------------------------------------------------------------------
void glLightfv(GLenum light, GLenum pname, const GLfloat* params);
void glLightModelfv(GLenum pname, const GLfloat* params);
void glMaterialf(GLenum face, GLenum pname, GLfloat param);
void glMaterialfv(GLenum face, GLenum pname, const GLfloat* params);

// ---------------------------------------------------------------------------------------
// GROUP 5 — Texture env / texture-matrix atlas trick. Callers: Graphics.mm/Terrain.mm
// (glMatrixMode(GL_TEXTURE); glScalef(1, 1/32, 1) around terrain passes — the atlas is a
// 1x32-tile strip, docs/rendering.md vertex formats section; render2's water/lava animation
// glTranslatef on the texture matrix). glTexEnvi selects GL_MODULATE/GL_DECAL. This group
// reuses the GROUP 1 matrix-stack functions with mode==GL_TEXTURE — no separate matrix API,
// just documented here as a distinct *usage* the WebGL2 backend must replicate (a per-draw
// UV transform uniform, since WebGL2 has no texture matrix either).
// ---------------------------------------------------------------------------------------
void glTexEnvi(GLenum target, GLenum pname, GLint param);
// glTexParameteri / glTexImage2D / glBindTexture / glGenTextures / glDeleteTextures /
// glCompressedTexImage2D / glPixelStorei are declared by <GLES2/gl2.h> (shared ES1/ES2
// signatures) — not redeclared; listed here for the inventory record (Texture2D.mm/
// Resources.mm/TerrainChunk.mm atlas bind).

// ---------------------------------------------------------------------------------------
// GROUP 6 — Fixed state / blend / clear / misc. Callers: everywhere (Graphics.mm state
// helpers are the canonical choke point per docs/rendering.md "Graphics state helpers").
// glEnable/glDisable/glBlendFunc/glColor4f/glClear/glClearColor/glDepthMask/glViewport/
// glHint/glPolygonOffset/glLineWidth/glShadeModel/glReadPixels/glGetError/glGetFloatv/
// glGetIntegerv/glGetString are ALL declared by <GLES2/gl2.h> already (identical ES1/ES2
// signatures) except the two below, which are ES1-only:
// ---------------------------------------------------------------------------------------
void glColor4f(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha);
void glColor4ub(GLubyte red, GLubyte green, GLubyte blue, GLubyte alpha);
void glAlphaFunc(GLenum func, GLclampf ref);
void glShadeModel(GLenum mode);
// glGetFloatv is used for matrix READBACK (GL_MODELVIEW_MATRIX/GL_PROJECTION_MATRIX) by
// Camera.mm (picking raycast setup) and Model.mm — <GLES2/gl2.h>'s glGetFloatv only knows
// ES2 pnames; this shim's .cpp must special-case GL_MODELVIEW_MATRIX/GL_PROJECTION_MATRIX/
// GL_TEXTURE_MATRIX by returning the shim's own tracked matrix-stack top, since WebGL2 has no
// such native query. TODO P2: implement in gl_es1_shim.cpp — this is Camera-critical (picking
// breaks silently, not a link error, if missed).

// ---------------------------------------------------------------------------------------
// GROUP 7 — Occlusion query extension (TerrainChunk.mm — rendering.md "Disabled experiments:
// occlusion queries (isTesting)"). Confirmed by grep to be the ONLY caller; the feature is
// compile-time/runtime disabled in shipped behavior. Stub as safe no-ops (query "never
// available") rather than real WebGL2 occlusion queries — not worth implementing unless a
// future session re-enables the feature.
// ---------------------------------------------------------------------------------------
#define GL_ANY_SAMPLES_PASSED_EXT      0x8C2F
void glBeginQueryEXT(GLenum target, GLuint id);
void glEndQueryEXT(GLenum target);
void glGetQueryObjectivEXT(GLuint id, GLenum pname, GLint* params);
void glGenQueriesEXT(GLsizei n, GLuint* ids);

// ---------------------------------------------------------------------------------------
// GROUP 8 — Point sprites (Classes/SpecialEffects.mm: the particle system).
//
// Found at the FIRST SUCCESSFUL LINK, not by the original grep — which is why the exclusion
// note at the top of this file has been corrected. The grep looked for these names in engine
// files and concluded "PVRT only"; it missed SpecialEffects.mm, and nothing revealed that
// until the linker asked for the symbols.
//
// ES1 point sprites have no direct WebGL equivalent: WebGL2 always draws gl_PointSize-sized
// points and has no fixed-function distance attenuation (GL_POINT_DISTANCE_ATTENUATION) and no
// per-vertex point-size ARRAY. Stage P2's shader can reproduce both — attenuation is a formula
// on gl_PointSize, and the size array becomes an ordinary vertex attribute — so these are
// declared with faithful signatures now and implemented as state capture. See the .cpp.
// ---------------------------------------------------------------------------------------
#define GL_POINT_SPRITE_OES             0x8861
#define GL_COORD_REPLACE_OES            0x8862
#define GL_POINT_SIZE_ARRAY_OES         0x8B9C
#define GL_POINT_SIZE_MIN               0x8126
#define GL_POINT_SIZE_MAX               0x8127
#define GL_POINT_DISTANCE_ATTENUATION   0x8129
#define GL_POINT_FADE_THRESHOLD_SIZE    0x8128

void glPointParameterf(GLenum pname, GLfloat param);
void glPointParameterfv(GLenum pname, const GLfloat* params);
void glPointSizePointerOES(GLenum type, GLsizei stride, const GLvoid* pointer);

#ifdef __cplusplus
}
#endif

#endif // EDEN_GL_ES1_SHIM_H
