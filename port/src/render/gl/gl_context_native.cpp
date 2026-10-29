// gl_context_native.cpp — the GL context, the window and the drawable (moved from port/src/shim/gl/ to
// port/src/render/gl/ in G.5b-7, when the ES 1.1 shim it once served was deleted). Declared in
// gl_context.h; R:: (render_core.cpp, gl_renderer.cpp) draws through the context this file creates.
// Desktop GL has no context-loss concept, so the web twin's webglcontext{lost,restored} half
// (web-port-final:web/src/shim/gl/gl_context_web.cpp) has no counterpart here.
//
// FOUR THINGS THIS FILE OWNS:
//
//  1. **The SDL window.** The GL context has to exist before `World::World()` — measured, not
//     preferred: Graphics::initGraphics() issues real glGenBuffers/glBufferData during
//     construction (R::bufCreate/bufData, texture uploads). port/src/harness/ therefore calls
//     eden_gl_context_create() as its first act, before the seam builds anything, and there is no
//     earlier place a window could come from. App::boot() (port/src/seam/App.cpp) publishes the
//     screen-metric globals afterwards.
//
//  2. **One global VAO, bound once and never switched.** Core profile REQUIRES a non-zero vertex
//     array object bound for any draw. R:: sets its attribute pointers per draw against that one
//     VAO (gl_renderer.cpp's attribute cache keys on it staying put), so binding one at context
//     creation and leaving it bound satisfies the profile; plan R7's per-format VAOs were revisited
//     in G.5b-7 and not taken (port/docs/renderer.md § VAOs). Handled here and nowhere else.
//
//  3. **GL_PROGRAM_POINT_SIZE.** GLES enables gl_PointSize unconditionally; desktop GL gates it.
//     Particles and block-break debris are point sprites, so without this they draw at one pixel.
//
//  4. **Entry-point loading, on Linux and Windows only** (Phase N Stage 3.2). macOS is the odd
//     one out here in this port's favour: <OpenGL/gl3.h> declares the 3.2-core subset and ld64
//     resolves it statically, so no loader is needed. `opengl32.dll` exports GL 1.1 and nothing
//     else, so on Windows a loader is mandatory, and Linux uses the same one deliberately rather
//     than leaning on libGL exporting more than the spec promises. See gl_loader_native.h.
//
// HEADLESS: eden_gl_context_create() returns 0 when the process was started without a window
// (eden_native_gl_set_headless(1) before the call). eden_gl_have_context() then answers 0, every R::
// call goes to the null backend, and the engine runs
// its full update/mesh/save path with no renderer. Stage 1's geometry, RSS and save-round-trip
// numbers are all taken that way, so they need no display.

#include "gl_context.h"
#include "gl_state.h"   // the platform's GL (GLES/gl.h + GLES2/gl2.h)
#include "../render_internal.h"   // rgl::invalidateAll

#include <SDL3/SDL.h>

#include "../../eden_app_identity.h"   // EDEN_APP_WINDOW_TITLE — Emod is not Eden

#include <cstdio>

// One string, so the boot line names what was actually created rather than always saying
// "desktop GL" — on iOS it is an ES 3.0 context and a log that says otherwise is the kind of
// small lie that costs an hour when a shader fails to compile.
#if defined(EDEN_PLATFORM_IOS)
#define EDEN_GL_CONTEXT_KIND "GLES 3.0"
#else
#define EDEN_GL_CONTEXT_KIND "desktop GL"
#endif

namespace {

static SDL_Window*   g_window = nullptr;
static SDL_GLContext g_gl = nullptr;
static GLuint        g_global_vao = 0;
static int           g_drawable_w = 0;
static int           g_drawable_h = 0;
// THE LETTERBOX (Phase N Stage 4.3, 2026-09-06). The engine's point space has an aspect and the
// screen has an aspect, and they are not the same on a 4:3 iPad: the touch profile pins the
// classic 16:9 layout. Web has always handled that — `public/eden-viewport.js` fits the CANVAS to
// the engine's aspect and letterboxes the rest — and native had no equivalent, so it stretched a
// 16:9 layout across a 4:3 screen. That is not a cosmetic bug: `gluPerspective(..., P_ASPECT_RATIO,
// ...)` and `Util.mm`'s findWorldCoords raycast both assume the RENDERED BOX IS THE ENGINE'S
// ASPECT, so a distorted box stretches the scene *and misaims every touch*. Reported from an
// iPad Air 2 as "HUD slightly squashed, switches don't respond, mine does nothing" — one cause.
//
// So the drawable is the fitted BOX and this is where it sits inside the real framebuffer. Every
// glViewport in this file uses it, and the input translator subtracts the same origin.
static int           g_vp_x = 0;
static int           g_vp_y = 0;
static bool          g_headless = false;

}  // namespace

extern int g_pick_viewport[4];   // render_core.cpp: what R::pickViewport answers
namespace rgl { float g_point_px_scale = 1.0f; }   // gl_renderer.cpp scales point sprites by it (N.4.11)
using rgl::g_point_px_scale;

extern "C" {

void eden_mem_trace(const char* where);   // HeapProbe_native.cpp (N.4.9), no-op without --mem-trace

void eden_native_gl_set_headless(int on) { g_headless = (on != 0); }

SDL_Window* eden_native_gl_window(void) { return g_window; }

// Phase N Stage 1's --smoke mode suspends presenting for exactly one frame so it can read the
// back buffer back with glReadPixels. Without that, "did anything actually get drawn?" is not
// answerable from inside the process: after SDL_GL_SwapWindow the back buffer's contents are
// undefined by spec, and reading GL_FRONT on a core-profile Apple context is not dependable
// either. Draw-call counts alone cannot distinguish "drew the world" from "drew the world with a
// shader that outputs black", which is precisely the failure mode a GLSL-dialect mistake produces.
static bool g_present_enabled = true;
void eden_native_gl_set_present(int on) { g_present_enabled = (on != 0); }

void eden_mem_trace_frame(void);   // HeapProbe_native.cpp: --mem-trace=2's first-frames samples

// THE PRESENT GUARD (N.4.12). On iOS, SDL_GL_SwapWindow presents whichever renderbuffer is bound
// (see the scene-pass notes below), so any code that leaves another one bound freezes the screen
// silently: the game keeps running and every readback-based check still passes. The view's own
// renderbuffer is recorded once the context is live; a present that finds a different binding puts
// it back, says so once, and counts it, so --smoke can fail on it (eden_native_gl_present_guard_fires).
static GLint g_view_rb = -1;
static int   g_present_guard_fires = 0;
int eden_native_gl_present_guard_fires(void) { return g_present_guard_fires; }

static void eden_scene_diag_at_present(void);   // FT.16a, with the scene pass below

void eden_native_gl_present(void) {
    eden_scene_diag_at_present();
#if defined(EDEN_PLATFORM_IOS)
    if (g_window && g_present_enabled && g_view_rb >= 0) {
        GLint rb = 0;
        glGetIntegerv(GL_RENDERBUFFER_BINDING, &rb);
        if (rb != g_view_rb) {
            if (g_present_guard_fires++ == 0)
                std::fprintf(stderr, "[eden-gl] PRESENT GUARD: renderbuffer %d bound at present, not "
                                     "the view's %d; rebinding (the frame would not have shown).\n",
                             (int)rb, (int)g_view_rb);
            glBindRenderbuffer(GL_RENDERBUFFER, (GLuint)g_view_rb);
        }
        // P.11: stock EAGLView discarded depth before presentRenderbuffer; SDL's swap does not, so
        // a tile GPU stores the whole depth buffer every frame (≈ 12.6 MB at 2048x1536 on an Air 2)
        // for nobody to read. The bound framebuffer is SDL's view FBO (never 0 on UIKit), but name
        // the default-framebuffer token too in case that ever changes.
        GLint fb = 0;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fb);
        const GLenum depth = fb ? GL_DEPTH_ATTACHMENT : GL_DEPTH;
        glInvalidateFramebuffer(GL_FRAMEBUFFER, 1, &depth);
    }
#endif
    if (g_window && g_present_enabled) SDL_GL_SwapWindow(g_window);
    eden_mem_trace_frame();
}

int eden_gl_have_context(void) { return g_gl != nullptr ? 1 : 0; }

// No lost-context concept on desktop GL. Kept because gl_context.h declares it
// (plan rule 4: the eden_* C API is identical on every platform) and because a caller asking
// "is the renderer lost?" deserves a real answer rather than a missing symbol.
int eden_gl_context_is_lost(void) { return 0; }

// Every glViewport in this file goes through here, so the origin cannot be forgotten in one of
// them — which is exactly the shape of bug that produces "it draws, but clicks land elsewhere".
static void eden_apply_viewport(void) {
    if (!eden_gl_have_context()) return;
    glViewport(g_vp_x, g_vp_y, g_drawable_w, g_drawable_h);
}

extern "C" void eden_native_gl_set_letterbox(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    if (x == g_vp_x && y == g_vp_y && w == g_drawable_w && h == g_drawable_h) return;
    g_vp_x = x; g_vp_y = y;
    g_drawable_w = w; g_drawable_h = h;
    std::fprintf(stderr, "[eden-gl] letterbox: %dx%d px at (%d,%d)\n", w, h, x, y);
    eden_apply_viewport();
}

extern "C" void eden_native_gl_get_letterbox(int* x, int* y, int* w, int* h) {
    if (x) *x = g_vp_x;
    if (y) *y = g_vp_y;
    if (w) *w = g_drawable_w;
    if (h) *h = g_drawable_h;
}

int eden_gl_context_create(int drawable_width, int drawable_height) {
    if (g_gl) return 1;
    if (g_headless) {
        std::fprintf(stderr, "[eden-gl] no canvas — staying headless (--headless).\n");
        return 0;
    }

    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "[eden-gl] SDL_InitSubSystem(VIDEO) failed: %s\n", SDL_GetError());
        return 0;
    }

#if defined(EDEN_PLATFORM_IOS)
    // iOS (Phase N Stage 4.1): OpenGL ES 3.0, which is the SAME context version the web build
    // gets from WebGL 2 — so this is the one platform whose GL surface the old ES 1.1 shim had
    // years of hours on. ES 3.0 is the stage's device floor: it is the version the GROUP 8
    // matrix-palette skinning shaders need (`#version 300 es`), and every A7 and later device —
    // i.e. every device that can run iOS 15 — has it.
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#else
    // macOS offers exactly two core-profile versions, 3.2 and 4.1, and nothing in the renderer needs
    // anything past 3.2 — the request is 4.1 so the driver is free to hand back its best, and
    // <OpenGL/gl3.h> (which declares the 3.2 subset) still covers every entry point used.
    // G.1(e): elsewhere the request walks 4.1 -> 3.3 -> 3.2 (see the CreateContext loop below), so a
    // driver that caps lower (Windows on ARM's GL-over-D3D12 stops at 3.3) still gets a context.
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
#endif
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    // Matches the web twin's context attributes: no stencil (grep-confirmed the engine never
    // touches it), no MSAA (the original CAEAGLLayer had none).
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 0);

    const int w = drawable_width  > 0 ? drawable_width  : 1136;
    const int h = drawable_height > 0 ? drawable_height : 640;

    // iOS has no window manager: the window IS the screen, and asking for anything else gets a
    // letterboxed one. SDL_WINDOW_FULLSCREEN with a zero-ish size lets UIKit hand back the real
    // device bounds, and HIGH_PIXEL_DENSITY is what makes that a RETINA drawable rather than a
    // point-sized one — the same distinction EAGLView.mm's contentScaleFactor made in 2010 and
    // the same one IS_IPAD encodes (CLAUDE.md convention 3). RESIZABLE is meaningless there.
    const SDL_WindowFlags kFlags = SDL_WINDOW_OPENGL | SDL_WINDOW_HIGH_PIXEL_DENSITY |
#if defined(EDEN_PLATFORM_IOS)
                                   SDL_WINDOW_FULLSCREEN | SDL_WINDOW_BORDERLESS;
#else
                                   SDL_WINDOW_RESIZABLE;
#endif
    eden_mem_trace("gl:video-up");
    g_window = SDL_CreateWindow(EDEN_APP_WINDOW_TITLE, w, h, kFlags);
    if (!g_window) {
        std::fprintf(stderr, "[eden-gl] SDL_CreateWindow failed: %s\n", SDL_GetError());
        return 0;
    }
#if !defined(EDEN_PLATFORM_IOS)
    // X.20: EDEN_WINDOW_DISPLAY=N opens the window centred on the Nth display (0-based), so a
    // --shot can be taken on a 1x and a 2x monitor from the same terminal. Diagnostic only.
    if (const char* d = SDL_getenv("EDEN_WINDOW_DISPLAY")) {
        int n = 0;
        SDL_DisplayID* ids = SDL_GetDisplays(&n);
        const int i = SDL_atoi(d);
        if (ids && i >= 0 && i < n) {
            SDL_SetWindowPosition(g_window, SDL_WINDOWPOS_CENTERED_DISPLAY(ids[i]),
                                  SDL_WINDOWPOS_CENTERED_DISPLAY(ids[i]));
            SDL_SyncWindow(g_window);
        }
        SDL_free(ids);
    }
#endif

    eden_mem_trace("gl:window");
    g_gl = SDL_GL_CreateContext(g_window);
#if !defined(EDEN_PLATFORM_IOS) && !defined(__APPLE__)
    // G.1(e): 4.1 -> 3.3 -> 3.2 core. Every entry point R:: uses is in 3.2.
    static const int kFallback[][2] = {{3, 3}, {3, 2}};
    for (int i = 0; !g_gl && i < 2; ++i) {
        std::fprintf(stderr, "[eden-gl] core context refused (%s); trying %d.%d\n", SDL_GetError(),
                     kFallback[i][0], kFallback[i][1]);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, kFallback[i][0]);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, kFallback[i][1]);
        g_gl = SDL_GL_CreateContext(g_window);
    }
#endif
    if (!g_gl) {
        std::fprintf(stderr, "[eden-gl] SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(g_window);
        g_window = nullptr;
        return 0;
    }
    SDL_GL_MakeCurrent(g_window, g_gl);
    SDL_GL_SetSwapInterval(1);
#if defined(EDEN_PLATFORM_IOS)
    glGetIntegerv(GL_RENDERBUFFER_BINDING, &g_view_rb);   // SDL leaves its view's bound; see the present guard
#endif

    // (4) Phase N Stage 3.2: on Linux and Windows every GL call above 1.1 goes through a function
    // pointer this resolves (gl_loader_native.h explains why, and why it is an explicit list).
    // It MUST run here — after MakeCurrent, before the first GL call — because
    // wglGetProcAddress returns NULL without a current context, and the very next line is a real
    // GL call. macOS compiles this to nothing: it links <OpenGL/gl3.h>'s symbols statically.
#if !defined(__APPLE__)
    if (eden_native_gl_load_entry_points() != 0) {
        std::fprintf(stderr, "[eden-gl] entry-point load failed; refusing to run with a partial "
                             "GL surface (see the names above).\n");
        SDL_GL_DestroyContext(g_gl);
        g_gl = nullptr;
        SDL_DestroyWindow(g_window);
        g_window = nullptr;
        return 0;
    }
#endif

#if defined(EDEN_PLATFORM_IOS)
    // NEITHER (2) NOR (3) ON iOS, and both omissions are the ES spec rather than a shortcut:
    // vertex array object 0 is a legal binding in ES 3.0 (only desktop CORE profile forbids it),
    // and gl_PointSize is always enabled in ES — GL_PROGRAM_POINT_SIZE does not exist there, so
    // enabling it would be a GL_INVALID_ENUM on every boot. This is exactly what the WebGL 2 path
    // does, which is the point: on this platform the renderer runs the web build's configuration.
#else
    // (2) above. Generated and bound exactly once; nothing ever binds a different one, so the
    // renderer's attribute cache (which assumes a single, global attribute state) stays true.
    glGenVertexArrays(1, &g_global_vao);
    glBindVertexArray(g_global_vao);

    // (3) above.
    glEnable(GL_PROGRAM_POINT_SIZE);
#endif

    // The DRAWABLE is in pixels and may differ from the window size on a Retina display —
    // SDL_GetWindowSizeInPixels is the pixel one, which is what glViewport wants.
    SDL_GetWindowSizeInPixels(g_window, &g_drawable_w, &g_drawable_h);

    // Seed the viewport mirror before the engine's first draw, for the same reason the web twin
    // does: Util.mm's unproject reads GL_VIEWPORT, and an all-zeros rect there is a silent wrong
    // answer rather than an error.
    eden_apply_viewport();

    eden_mem_trace("gl:context");
    std::fprintf(stderr, "[eden-gl] %s context live (%dx%d px) — %s / GLSL %s\n",
                 EDEN_GL_CONTEXT_KIND,
                 g_drawable_w, g_drawable_h,
                 (const char*)glGetString(GL_VERSION),
                 (const char*)glGetString(GL_SHADING_LANGUAGE_VERSION));
    return 1;
}

static void eden_scene_fbo_release(void);   // N.4.11, below

void eden_gl_context_destroy(void) {
    if (!g_gl) return;
    eden_scene_fbo_release();   // while the context that owns it is still current
    if (g_global_vao) { glDeleteVertexArrays(1, &g_global_vao); g_global_vao = 0; }
    SDL_GL_DestroyContext(g_gl);
    g_gl = nullptr;
    if (g_window) { SDL_DestroyWindow(g_window); g_window = nullptr; }
    g_drawable_w = g_drawable_h = 0;
    // The renderer's programs, rings, caches and white texture belonged to the context that just
    // died, so drop the handles rather than reuse names from a dead context.
    rgl::invalidateAll();
}

void eden_gl_context_get_drawable_size(int* width, int* height) {
    if (width)  *width  = g_drawable_w;
    if (height) *height = g_drawable_h;
}

void eden_gl_set_pick_viewport(int width, int height) {
    if (width <= 0 || height <= 0) return;
    if (g_pick_viewport[2] == width && g_pick_viewport[3] == height) return;
    g_pick_viewport[2] = width;
    g_pick_viewport[3] = height;
    std::fprintf(stderr, "[eden-gl] pick viewport now %dx%d (engine point space x scale).\n",
                 width, height);
}

void eden_gl_context_set_drawable_size(int width, int height) {
    if (width <= 0 || height <= 0) return;
    if (width == g_drawable_w && height == g_drawable_h) return;
    g_drawable_w = width;
    g_drawable_h = height;
    // A bare size change means "the box is the whole surface" — eden_native_apply_display_mode()
    // is what re-fits it afterwards if the aspects differ.
    g_vp_x = 0; g_vp_y = 0;
    eden_apply_viewport();
}

// The web twin's page-facing entry point for a dynamic drawable. On native the window manager
// owns the box, so this is reached only from the resize handler in port/src/harness/ — the
// GL_MAX_RENDERBUFFER_SIZE clamp is kept because a 5K display at 2x asks for a drawable plenty of
// GPUs refuse, and the failure mode there is a black frame, not an error.
void eden_set_drawable_size(int width, int height) {
    const int kMaxDrawableDim = 4096;
    if (width <= 0 || height <= 0) return;
    if (width > kMaxDrawableDim || height > kMaxDrawableDim) {
        float s = (float)kMaxDrawableDim / (float)((width > height) ? width : height);
        width  = (int)(width  * s);
        height = (int)(height * s);
        if (width < 1) width = 1;
        if (height < 1) height = 1;
    }
    eden_gl_context_set_drawable_size(width, height);
}

void eden_gl_context_bind_default_framebuffer(void) {
    if (!eden_gl_have_context()) return;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    eden_apply_viewport();
}

// ---- N.4.11: the scene pass at its own resolution -------------------------------------------
//
// THE DESIGN POINT. The drawable is the display's full pixel density (2048x1536 on an iPad Air 2,
// which stock Eden never rendered at), and the A8X pays for every one of those pixels in fill
// rate. Shrinking the DRAWABLE would be the one-line way to trade sharpness for frame rate, and is
// exactly what web does, but on native it would drag the UI down with it: eden_ui_raster_density()
// follows the drawable, so a 50% drawable is N.4.8's 1.8x-magnified text all over again. So only
// the 3D pass moves. eden_scene_pass_begin() (R::beginScene) points it at an offscreen colour+depth
// framebuffer of (letterbox box x scale); eden_scene_pass_end() (R::endScene) composites that into
// the box and puts the window's framebuffer back, and the HUD/menus/dialogs then draw at full
// density on top. At 100% neither call touches GL: no framebuffer is created, nothing is
// composited, and the frame is the one this port has always drawn.
//
// THE COMPOSITE IS ONE TEXTURED QUAD (FT.16, 2026-10-28), not glBlitFramebuffer. The scene's colour
// is a texture attachment and the UI program draws it across the box with linear filtering, the same
// cost as the blit and the same pixels on desktop. The blit was N.4.11's choice; on the Air 2 at 50%
// it never landed (10-09: the HUD drew over a frozen menu frame, with 0 guard fires and 0 GL errors
// over SSH), and no scissor or other FBO user exists to explain it, so the ES blit rules leave the
// picture. Before the quad the window's colour is CLEARED (black, or magenta under --diag-clear):
// a composite that fails shows black, never a stale menu, and the tile GPU starts the window's frame
// from a clear instead of loading the last one. The scene framebuffer is cleared at its start too.
// --diag-clear also clears the window at 100%, under the sky, so a gap in the sky shows magenta.
//
// THE WINDOW'S FRAMEBUFFER IS NOT ALWAYS 0. On iOS, SDL's UIKit view draws into a framebuffer
// object of its own and the system presents its renderbuffer; binding 0 there would send the HUD
// nowhere. So _begin reads GL_FRAMEBUFFER_BINDING and _end restores exactly that.
//
// NOR IS ITS RENDERBUFFER BINDING OURS TO CHANGE (N.4.12, 2026-10-08). SDL's UIKit swap is a bare
// `[context presentRenderbuffer:GL_RENDERBUFFER]`, which presents WHATEVER renderbuffer is bound
// ("viewRenderbuffer should always be bound here. Code that binds something else is responsible
// for rebinding viewRenderbuffer", SDL_uikitopenglview.m). Allocating the scene framebuffer used
// to finish with glBindRenderbuffer(0), so from the first scaled frame on every present failed
// (GL_INVALID_OPERATION, which the next texture upload reported as "invalid operation in
// initData"): the iPad kept showing the last menu frame, the loading bar at 58%, while the game
// ran on underneath at ~40% CPU. Every --smoke/--shot check passed because they read the
// framebuffer back and never look at what was presented. Desktop swaps ignore the binding, which
// is why macOS never reproduced it. So the allocation saves GL_RENDERBUFFER_BINDING and puts it back.
//
// WHAT ELSE A SCALED PASS CHANGES, and what was checked:
//   * picking: Util.mm's findWorldCoords unprojects against R::pickViewport, the engine's POINT
//     space, never the real viewport, so a smaller framebuffer cannot misaim a tap;
//   * point sprites (smoke, fire, debris) are sized in framebuffer pixels, so the draw path scales
//     them by g_point_px_scale (gl_renderer.cpp's sprite draw) to keep their size on screen;
//   * the camera-mode screenshot (Util.mm glReadPixels) and --shot both read the window's
//     framebuffer after the composite, so they see the composited frame;
//   * the HUD runs with depth testing off (Graphics::beginHud), so the window framebuffer's depth
//     no longer receiving the scene's depth changes nothing it draws.
// Lines (glLineWidth) are not compensated: core profile and ES 3 draw them 1 px wide anyway.
//
// FT.16a's LOG: once per scene size, `[eden-gl] scene-diag` lines give the framebuffer ids at begin,
// at the composite and at present, the GL error after the composite, and (iOS) the view's colour
// format, so a device run over SSH says which framebuffer the frame went to without a look.
static GLuint g_scene_fbo = 0, g_scene_color = 0 /* a texture since FT.16 */, g_scene_depth = 0;
static int    g_scene_fbo_w = 0, g_scene_fbo_h = 0;
static GLint  g_scene_prev_fbo = 0;
static bool   g_scene_active = false;
static bool   g_scene_failed = false;   // an incomplete FBO once: stop trying, draw at 100%
static bool   g_diag_clear = false;     // --diag-clear (FT.16a)
static bool   g_scene_diag_present = false;   // log the binding at the next present (once per size)

void eden_native_gl_set_diag_clear(int on) { g_diag_clear = (on != 0); }

extern "C" int eden_power_exp(int bit);   // power.cpp (FT.15a)
extern "C" int eden_get_render_scale_pct(void);   // Settings.cpp (both targets compile it)

static void eden_scene_fbo_release(void) {
    if (g_scene_fbo)   { glDeleteFramebuffers(1, &g_scene_fbo);    g_scene_fbo = 0; }
    if (g_scene_color) { rgl::forgetTexture(g_scene_color); glDeleteTextures(1, &g_scene_color); g_scene_color = 0; }
    if (g_scene_depth) { glDeleteRenderbuffers(1, &g_scene_depth); g_scene_depth = 0; }
    g_scene_fbo_w = g_scene_fbo_h = 0;
}

// The size the scene renders at for a given box and percentage, clamped to what the GPU will
// allocate (125% of a 5K window is past most GL_MAX_RENDERBUFFER_SIZEs) and to at least 1 px.
static void eden_scene_size(int pct, int* w, int* h) {
    GLint maxrb = 4096, maxtex = 4096;
    glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &maxrb);
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxtex);
    if (maxtex < maxrb) maxrb = maxtex;   // the colour is a texture since FT.16
    int sw = (int)((long)g_drawable_w * pct / 100);
    int sh = (int)((long)g_drawable_h * pct / 100);
    if (sw > maxrb || sh > maxrb) {
        const float k = (float)maxrb / (float)(sw > sh ? sw : sh);
        sw = (int)(sw * k); sh = (int)(sh * k);
    }
    *w = sw < 1 ? 1 : sw;
    *h = sh < 1 ? 1 : sh;
}

void eden_render_note_scene(int begin);   // render_core.cpp: --render-selftest's bracket invariant

static const R::Rgba kDiagMagenta = {1.0f, 0.0f, 1.0f, 1.0f};

void eden_scene_pass_begin(void) {
    eden_render_note_scene(1);
    g_scene_active = false;
    if (!eden_gl_have_context()) return;
    const int pct = g_scene_failed ? 100 : eden_get_render_scale_pct();
    if (pct == 100 || g_drawable_w <= 0 || g_drawable_h <= 0) {
        // Back at 100%: give the memory back rather than keep an idle second framebuffer.
        if (g_scene_fbo) eden_scene_fbo_release();
        if (g_diag_clear) rgl::clear(true, false, kDiagMagenta);
        else rgl::clear(true, false, R::Rgba{0.0f, 0.0f, 0.0f, 1.0f});   // FT.15d: frame-start clear always on (-0.27 W, FT.15c)
        return;
    }
    int w, h;
    eden_scene_size(pct, &w, &h);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &g_scene_prev_fbo);
    if (!g_scene_fbo || w != g_scene_fbo_w || h != g_scene_fbo_h) {
        eden_scene_fbo_release();
        GLint prevRb = 0;
        glGetIntegerv(GL_RENDERBUFFER_BINDING, &prevRb);
        glGenFramebuffers(1, &g_scene_fbo);
        glGenTextures(1, &g_scene_color);
        glGenRenderbuffers(1, &g_scene_depth);
        glBindFramebuffer(GL_FRAMEBUFFER, g_scene_fbo);
        rgl::bindTextureNow(g_scene_color);   // through the cache, so the next draw rebinds its own
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_scene_color, 0);
        glBindRenderbuffer(GL_RENDERBUFFER, g_scene_depth);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, g_scene_depth);
        glBindRenderbuffer(GL_RENDERBUFFER, (GLuint)prevRb);   // iOS presents this one; see above
        const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            std::fprintf(stderr, "[eden-gl] scene framebuffer %dx%d incomplete (0x%x); rendering "
                                 "the scene at 100%% from now on.\n", w, h, (unsigned)status);
            eden_scene_fbo_release();
            glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)g_scene_prev_fbo);
            g_scene_failed = true;
            return;
        }
        g_scene_fbo_w = w; g_scene_fbo_h = h;
        g_scene_diag_present = true;
        std::fprintf(stderr, "[eden-gl] scene pass at %d%%: %dx%d px, composited into %dx%d.\n",
                     pct, w, h, g_drawable_w, g_drawable_h);
        std::fprintf(stderr, "[eden-gl] scene-diag begin: window fbo %d, renderbuffer %d (view %d), "
                             "scene fbo %u tex %u%s\n", (int)g_scene_prev_fbo, (int)prevRb,
                     (int)g_view_rb, (unsigned)g_scene_fbo, (unsigned)g_scene_color,
                     g_diag_clear ? ", --diag-clear on" : "");
    } else {
        glBindFramebuffer(GL_FRAMEBUFFER, g_scene_fbo);
    }
    glViewport(0, 0, w, h);
    rgl::clear(true, true, R::Rgba{0.0f, 0.0f, 0.0f, 1.0f});   // a fresh start for the tiler (FT.16)
    g_point_px_scale = (float)w / (float)g_drawable_w;
    g_scene_active = true;
}

void eden_scene_pass_end(void) {
    eden_render_note_scene(0);
    if (!g_scene_active) return;
    g_scene_active = false;
    g_point_px_scale = 1.0f;
#if defined(EDEN_PLATFORM_IOS)
    {   // P.11: the scaled scene's depth is dead once drawn; don't let the tiler store it.
        const GLenum depth = GL_DEPTH_ATTACHMENT;
        glInvalidateFramebuffer(GL_FRAMEBUFFER, 1, &depth);
    }
#endif
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)g_scene_prev_fbo);
    eden_apply_viewport();
    const bool diag = g_scene_diag_present;
    GLenum errBefore = GL_NO_ERROR;   // what the scene pass left, so "after" is the composite's alone
    if (diag) { errBefore = glGetError(); while (glGetError() != GL_NO_ERROR) {} }
    // The window's frame starts here: clear it (letterbox bars included), then the quad covers the box.
    rgl::clear(true, false, g_diag_clear ? kDiagMagenta : R::Rgba{0.0f, 0.0f, 0.0f, 1.0f});
    rcore::DrawState& ds = rcore::ds();
    const R::State keep = ds.state;
    ds.state = R::State{R::B_OFF, false, false, false, 0.0f, 0.0f};
    R::proj().push(); R::proj().loadIdentity();
    R::mv().push();   R::mv().loadIdentity();
    static const float kQuad[16] = { -1, -1, 0, 0,   1, -1, 1, 0,   1, 1, 1, 1,   -1, 1, 0, 1 };
    R::Geometry g = {};
    g.fmt = &R::kFmtUI2UV; g.vmem = kQuad; g.itype = R::I_NONE;
    g.first = 0; g.count = 4; g.prim = R::TRIANGLE_FAN;
    rgl::draw2D(g, (R::Tex)g_scene_color, R::Rgba{1.0f, 1.0f, 1.0f, 1.0f});
    R::mv().pop();
    R::proj().pop();
    ds.state = keep;
    if (diag) {
        GLint fb = 0, vp[4] = {0, 0, 0, 0};
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fb);
        glGetIntegerv(GL_VIEWPORT, vp);
        std::fprintf(stderr, "[eden-gl] scene-diag composite: window fbo %d, viewport %d,%d %dx%d, "
                             "GL error before 0x%x, after 0x%x\n", (int)fb, vp[0], vp[1], vp[2], vp[3],
                     (unsigned)errBefore, (unsigned)glGetError());
    }
}

// The present half of FT.16a's log: the framebuffer and renderbuffer bound when the frame is handed
// to the system, and the view's colour format (iOS), once per scene size.
static void eden_scene_diag_at_present(void) {
    if (!g_scene_diag_present) return;
    g_scene_diag_present = false;
    GLint fb = 0, rb = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fb);
    glGetIntegerv(GL_RENDERBUFFER_BINDING, &rb);
    GLint fmt = 0;
#if defined(EDEN_PLATFORM_IOS)
    glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_INTERNAL_FORMAT, &fmt);
#endif
    std::fprintf(stderr, "[eden-gl] scene-diag present: fbo %d, renderbuffer %d (view %d), view colour "
                         "format 0x%x, GL error 0x%x\n", (int)fb, (int)rb, (int)g_view_rb,
                 (unsigned)fmt, (unsigned)glGetError());
}

}  // extern "C"
