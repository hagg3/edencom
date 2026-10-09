// gl_context_native.cpp — desktop-GL context ownership, the native twin of
// web/src/shim/gl/gl_context_web.cpp (Phase N Stage 1,
// WORKING/native-migration-plan-2026-09-04.md).
//
// It implements exactly the interface gl_es1_shim.h GROUP 2c declares, against the same shared
// state gl_shim_internal.h carries, so the 1,900-line portable translator
// (web/src/shim/gl/gl_fixed_function.cpp) is compiled unchanged into both targets. Shorter than
// the web twin for one structural reason: desktop GL has no context-loss concept, so the
// webglcontext{lost,restored} half of that file has no counterpart here.
//
// FOUR THINGS THIS FILE OWNS THAT THE WEB TWIN DOES NOT:
//
//  1. **The SDL window.** The GL context has to exist before `World::World()` — measured, not
//     preferred: Graphics::initGraphics() issues real glGenBuffers/glBufferData during
//     construction (see gl_es1_shim.h GROUP 2b). eden_main_native.cpp therefore calls
//     eden_gl_context_create() as its first act, before the seam builds anything, and there is no
//     earlier place a window could come from. SDLView_native.mm publishes G_EAGL_VIEW and the
//     screen-metric globals afterwards, mirroring EAGLView_web.mm.
//
//  2. **One global VAO, bound once and never switched.** Core profile REQUIRES a non-zero vertex
//     array object bound for any draw, and the shim deliberately does not use VAOs (it is an ES1
//     translator, and Classes/TerrainChunk.mm binds its element buffer outside a draw call — see
//     web/docs/gl-shim.md). Binding one at context creation and leaving it bound satisfies the
//     profile without the shim having to learn what a VAO is. The plan named this as Stage 1's
//     most likely failure point; it is handled here and nowhere else.
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
// (eden_native_gl_set_headless(1) before the call). That is the same contract the web build has
// under `node eden.js` — GROUP 2b's guard stays closed, every GL call no-ops, and the engine runs
// its full update/mesh/save path with no renderer. Stage 1's geometry, RSS and save-round-trip
// numbers are all taken that way, so they need no display.

#define EDEN_GL_NO_GUARD 1
#include "gl_es1_shim.h"
#include "gl_shim_internal.h"

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

namespace eden_gl_shim {

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

}  // namespace eden_gl_shim

using namespace eden_gl_shim;

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

void eden_native_gl_present(void) {
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
    }
#endif
    if (g_window && g_present_enabled) SDL_GL_SwapWindow(g_window);
    eden_mem_trace_frame();
}

int eden_gl_have_context(void) { return g_gl != nullptr ? 1 : 0; }

// No lost-context concept on desktop GL. Kept because gl_es1_shim.h declares it for both targets
// (plan rule 4: the eden_* C API is identical on every platform) and because a caller asking
// "is the renderer lost?" deserves a real answer rather than a missing symbol.
EDEN_GL_EXPORT
int eden_gl_context_is_lost(void) { return 0; }

// Every glViewport in this file goes through here, so the origin cannot be forgotten in one of
// them — which is exactly the shape of bug that produces "it draws, but clicks land elsewhere".
static void eden_apply_viewport(void) {
    g_viewport[0] = g_vp_x; g_viewport[1] = g_vp_y;
    g_viewport[2] = g_drawable_w; g_viewport[3] = g_drawable_h;
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
    // gets from WebGL 2 — so this is the one platform whose GL surface the shim has already had
    // years of hours on. ES 3.0 is the stage's device floor: it is the version the GROUP 8
    // matrix-palette skinning shaders need (`#version 300 es`), and every A7 and later device —
    // i.e. every device that can run iOS 12 — has it.
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#else
    // macOS offers exactly two core-profile versions, 3.2 and 4.1, and nothing in this shim needs
    // anything past 3.2 — the request is 4.1 so the driver is free to hand back its best, and
    // <OpenGL/gl3.h> (which declares the 3.2 subset) still covers every entry point used.
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

    eden_mem_trace("gl:window");
    g_gl = SDL_GL_CreateContext(g_window);
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
    // does, which is the point: on this platform the shim runs the web build's configuration.
#else
    // (2) above. Generated and bound exactly once; nothing ever binds a different one, so the
    // shim's own buffer bookkeeping (which assumes a single, global attribute state) stays true.
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
    // Same contract as the web twin: the shim's programs and streaming buffers belonged to the
    // context that just died, so drop the handles rather than reuse names from a dead context.
    eden_gl_shim_invalidate_gl_objects();
}

void eden_gl_context_get_drawable_size(int* width, int* height) {
    if (width)  *width  = g_drawable_w;
    if (height) *height = g_drawable_h;
}

void eden_gl_set_pick_viewport(int width, int height) {
    if (width <= 0 || height <= 0) return;
    if (kPickViewport[2] == width && kPickViewport[3] == height) return;
    kPickViewport[2] = width;
    kPickViewport[3] = height;
    std::fprintf(stderr, "[eden-gl] pick viewport now %dx%d (engine point space x scale).\n",
                 width, height);
}

void eden_gl_context_set_drawable_size(int width, int height) {
    if (width <= 0 || height <= 0) return;
    if (width == g_drawable_w && height == g_drawable_h) return;
    g_drawable_w = width;
    g_drawable_h = height;
    // A bare size change means "the box is the whole surface" — eden_native_apply_display_mode()
    // is what re-fits it afterwards if the aspects differ. Mirror stays current even headless:
    // Util.mm's unproject reads GL_VIEWPORT through the shim whether or not a context exists.
    g_vp_x = 0; g_vp_y = 0;
    eden_apply_viewport();
}

// The web twin's page-facing entry point for a dynamic drawable. On native the window manager
// owns the box, so this is reached only from the resize handler in eden_main_native.cpp — the
// GL_MAX_RENDERBUFFER_SIZE clamp is kept because a 5K display at 2x asks for a drawable plenty of
// GPUs refuse, and the failure mode there is a black frame, not an error.
EDEN_GL_EXPORT
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
// the 3D pass moves. eden_scene_pass_begin() points it at an offscreen colour+depth framebuffer
// of (letterbox box x scale); eden_scene_pass_end() upscales that into the box with one linear
// blit and puts the window's framebuffer back, and the HUD/menus/dialogs then draw at full
// density on top. At 100% neither call touches GL: no framebuffer is created, no blit happens,
// and the frame is the one this port has always drawn.
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
//   * picking: Util.mm's findWorldCoords unprojects against kPickViewport, the engine's POINT
//     space, never the real viewport, so a smaller framebuffer cannot misaim a tap;
//   * point sprites (smoke, fire, debris) are sized in framebuffer pixels, so the draw path scales
//     them by g_point_px_scale (gl_fixed_function.cpp) to keep their size on screen;
//   * the camera-mode screenshot (Util.mm glReadPixels) and --shot both read the window's
//     framebuffer after the blit, so they see the composited frame;
//   * the HUD runs with depth testing off (Graphics::beginHud), so the window framebuffer's depth
//     no longer receiving the scene's depth changes nothing it draws.
// Lines (glLineWidth) are not compensated: core profile and ES 3 draw them 1 px wide anyway.
static GLuint g_scene_fbo = 0, g_scene_color = 0, g_scene_depth = 0;
static int    g_scene_fbo_w = 0, g_scene_fbo_h = 0;
static GLint  g_scene_prev_fbo = 0;
static bool   g_scene_active = false;
static bool   g_scene_failed = false;   // an incomplete FBO once: stop trying, draw at 100%

extern "C" int eden_get_render_scale_pct(void);   // Settings_web.mm (both targets compile it)

static void eden_scene_fbo_release(void) {
    if (g_scene_fbo)   { glDeleteFramebuffers(1, &g_scene_fbo);    g_scene_fbo = 0; }
    if (g_scene_color) { glDeleteRenderbuffers(1, &g_scene_color); g_scene_color = 0; }
    if (g_scene_depth) { glDeleteRenderbuffers(1, &g_scene_depth); g_scene_depth = 0; }
    g_scene_fbo_w = g_scene_fbo_h = 0;
}

// The size the scene renders at for a given box and percentage, clamped to what the GPU will
// allocate (125% of a 5K window is past most GL_MAX_RENDERBUFFER_SIZEs) and to at least 1 px.
static void eden_scene_size(int pct, int* w, int* h) {
    GLint maxrb = 4096;
    glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &maxrb);
    int sw = (int)((long)g_drawable_w * pct / 100);
    int sh = (int)((long)g_drawable_h * pct / 100);
    if (sw > maxrb || sh > maxrb) {
        const float k = (float)maxrb / (float)(sw > sh ? sw : sh);
        sw = (int)(sw * k); sh = (int)(sh * k);
    }
    *w = sw < 1 ? 1 : sw;
    *h = sh < 1 ? 1 : sh;
}

void eden_scene_pass_begin(void) {
    g_scene_active = false;
    if (!eden_gl_have_context() || g_scene_failed) return;
    const int pct = eden_get_render_scale_pct();
    if (pct == 100 || g_drawable_w <= 0 || g_drawable_h <= 0) {
        // Back at 100%: give the memory back rather than keep an idle second framebuffer.
        if (g_scene_fbo) eden_scene_fbo_release();
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
        glGenRenderbuffers(1, &g_scene_color);
        glGenRenderbuffers(1, &g_scene_depth);
        glBindFramebuffer(GL_FRAMEBUFFER, g_scene_fbo);
        glBindRenderbuffer(GL_RENDERBUFFER, g_scene_color);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, w, h);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, g_scene_color);
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
        std::fprintf(stderr, "[eden-gl] scene pass at %d%%: %dx%d px, upscaled into %dx%d.\n",
                     pct, w, h, g_drawable_w, g_drawable_h);
    } else {
        glBindFramebuffer(GL_FRAMEBUFFER, g_scene_fbo);
    }
    glViewport(0, 0, w, h);
    g_point_px_scale = (float)w / (float)g_drawable_w;
    g_scene_active = true;
}

void eden_scene_pass_end(void) {
    if (!g_scene_active) return;
    g_scene_active = false;
    g_point_px_scale = 1.0f;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_scene_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)g_scene_prev_fbo);
    glBlitFramebuffer(0, 0, g_scene_fbo_w, g_scene_fbo_h,
                      g_vp_x, g_vp_y, g_vp_x + g_drawable_w, g_vp_y + g_drawable_h,
                      GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)g_scene_prev_fbo);
    eden_apply_viewport();
}

}  // extern "C"
