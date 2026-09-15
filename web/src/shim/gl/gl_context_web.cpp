// gl_context_web.cpp — Emscripten WebGL2 context ownership, split out of gl_es1_shim.cpp
// (Phase N Stage 0.2, WORKING/native-migration-plan-2026-09-04.md).
//
// This file is the ONLY genuinely Emscripten-coupled ~40 lines the old gl_es1_shim.cpp had:
// context create/destroy/set_drawable_size, the two webglcontext{lost,restored} callbacks, and
// emscripten_pause_main_loop. Everything else — the matrix stack, client arrays, fog/lighting/
// texture-env state capture, the draw path and its two shader programs — moved unchanged to
// gl_fixed_function.cpp, which has zero Emscripten dependency and is cmake-shared with the
// future native/src/shim/gl/gl_context_native.cpp (desktop GL context creation, no
// context-loss concept to speak of).
//
// EDEN_GL_NO_GUARD: this translation unit sees the REAL passthrough names (glViewport,
// glBindFramebuffer, ...) for the same reason gl_fixed_function.cpp does — see that file's
// header and gl_es1_shim.h GROUP 2b.
#define EDEN_GL_NO_GUARD 1
#include "gl_es1_shim.h"
#include "gl_shim_internal.h"
#include <cstdio>
#include <GLES2/gl2.h>
#include <emscripten/emscripten.h>
#include <emscripten/html5.h>
#include <emscripten/html5_webgl.h>

namespace eden_gl_shim {

// The live context handle and the drawable size. g_context_lost exists because a lost WebGL
// context keeps its handle non-zero and emscripten_webgl_get_current_context() keeps returning
// it — the guard predicate below reads these two rather than that query, both for correctness
// across a context loss and because it removes a wasm->JS crossing from every guarded GL call.
static EMSCRIPTEN_WEBGL_CONTEXT_HANDLE g_ctx = 0;
static int g_drawable_w = 0;
static int g_drawable_h = 0;
static bool g_context_lost = false;

} // namespace eden_gl_shim

using namespace eden_gl_shim;

extern "C" {

// Two changes from the original one-liner (`emscripten_webgl_get_current_context() != 0`), both
// from audit finding C3:
//   * `!g_context_lost`. A lost WebGL context keeps its handle: the old test stayed TRUE across a
//     context loss, so the guard never closed, every call no-oped at the driver, and the frame
//     froze with no error anywhere. Now a lost context is indistinguishable from the headless case
//     to everything downstream, which is exactly the right semantics for a "forward if live" guard.
//   * `g_ctx` instead of the Emscripten query. This shim is the ONLY creator of a context in the
//     whole port (grep: emscripten_webgl_create_context appears here and nowhere else), so its own
//     handle is authoritative — and the query is a JS-library call, i.e. a wasm->JS crossing on
//     EVERY guarded GL call in the frame. Answering from wasm memory removes all of them.
int eden_gl_have_context(void) {
    return (g_ctx != 0 && !g_context_lost) ? 1 : 0;
}

// Exposed so the page can tell "renderer lost" apart from "still booting" (public/eden-st.html's
// renderer-lost panel polls nothing — it is pushed to — but the flag is useful from the console).
EDEN_GL_EXPORT
int eden_gl_context_is_lost(void) { return g_context_lost ? 1 : 0; }

// The canvas selector. NOT Emscripten's default "#canvas" — public/index.html:40 hosts
// `<canvas id="eden-canvas">`, and a selector mismatch here fails only at run time, in the
// browser, as a silently headless frame. Keep these two in sync.
static const char* kCanvasTarget = "#eden-canvas";

// ---- Context loss / restore (audit C3, item #5) ------------------------------------------
//
// Why this is worth real code rather than a log line: losing a WebGL context is ROUTINE on mobile
// (backgrounding, memory pressure, tab discard) and not rare on desktop (driver resets). Before
// this pass the port had no listener at all, and — worse — the guard predicate above stayed open
// across a loss, so the failure mode was a permanently frozen frame with an empty console.
//
// What is recoverable and what is not, honestly: the SHIM's own GL objects (both programs, the
// seven streaming VBOs, the streaming IBO) are rebuilt on demand, so
// eden_gl_shim_invalidate_gl_objects() is all they need. The ENGINE's objects are not: every
// texture is uploaded exactly once by Texture2D_web.mm's initData during load, and re-uploading
// them would mean re-running the engine's load path — which lives in Classes/ and cannot be
// driven from here (web/CLAUDE.md's one non-negotiable rule). So a restored context would render
// untextured garbage if the loop simply resumed. The chosen behaviour is therefore: PAUSE the main
// loop on loss, tell the page (which shows a "renderer lost — reload" panel), and on `restored`
// rebuild a real context so the shim's state is coherent again — but leave the loop paused and the
// page's reload prompt standing, because reload is the only path that actually gets textures back.
static void eden_gl_notify_page(int lost) {
    // The page hook is optional: `node eden.js` has no DOM, and a host page that predates this
    // pass simply has no window.EdenRenderer. Both must be silent no-ops rather than a JS throw
    // inside a GL callback.
    EM_ASM({
        var r = (typeof window !== 'undefined') && window.EdenRenderer;
        if (!r) return;
        try { if ($0) { r.onContextLost && r.onContextLost(); }
              else     { r.onContextRestored && r.onContextRestored(); } } catch (e) {}
    }, lost);
}

static EM_BOOL eden_gl_on_context_lost(int /*eventType*/, const void* /*reserved*/,
                                       void* /*userData*/) {
    if (g_context_lost) return EM_TRUE;
    g_context_lost = true;
    std::fprintf(stderr, "[eden-gl] WebGL context LOST — pausing the main loop.\n");
    // Returning EM_TRUE makes Emscripten preventDefault() the event. That is REQUIRED, not
    // cosmetic: without it the browser never fires `webglcontextrestored` at all.
    eden_gl_shim_invalidate_gl_objects();
    emscripten_pause_main_loop();
    eden_gl_notify_page(1);
    return EM_TRUE;
}

static EM_BOOL eden_gl_on_context_restored(int /*eventType*/, const void* /*reserved*/,
                                           void* /*userData*/) {
    std::fprintf(stderr, "[eden-gl] WebGL context restored event — rebuilding the context.\n");
    // Drop the dead handle first: eden_gl_context_create() early-returns while g_ctx is set, and
    // the handle we hold belongs to the context that was lost.
    if (g_ctx) { emscripten_webgl_destroy_context(g_ctx); g_ctx = 0; }
    g_context_lost = false;
    const int w = g_drawable_w, h = g_drawable_h;
    g_drawable_w = g_drawable_h = 0;
    if (!eden_gl_context_create(w, h)) {
        g_context_lost = true;   // could not get a new one: stay in the closed-guard state
        std::fprintf(stderr, "[eden-gl] context could not be recreated after restore.\n");
    }
    // Deliberately NOT emscripten_resume_main_loop() — see the block comment above: the engine's
    // textures died with the old context and only a page reload brings them back.
    eden_gl_notify_page(0);
    return EM_TRUE;
}

int eden_gl_context_create(int drawable_width, int drawable_height) {
    if (g_ctx) return 1;

    EmscriptenWebGLContextAttributes attrs;
    emscripten_webgl_init_context_attributes(&attrs);
    attrs.majorVersion = 2;              // WebGL2. Not 1: the shim's translation assumes GLES2+
    attrs.minorVersion = 0;              // semantics throughout (see the shader below).
    attrs.alpha = EM_FALSE;              // the engine clears to an opaque sky color every frame
    attrs.depth = EM_TRUE;               // Graphics.mm enables GL_DEPTH_TEST
    attrs.stencil = EM_FALSE;            // grep-confirmed: the engine never touches stencil
    attrs.antialias = EM_FALSE;          // matches the original CAEAGLLayer setup (no MSAA)
    attrs.preserveDrawingBuffer = EM_FALSE;
    attrs.enableExtensionsByDefault = EM_TRUE;
    attrs.failIfMajorPerformanceCaveat = EM_FALSE;

    EMSCRIPTEN_WEBGL_CONTEXT_HANDLE ctx = emscripten_webgl_create_context(kCanvasTarget, &attrs);
    if (ctx <= 0) {
        // NOT an error, and deliberately quiet at info level: under `node eden.js` there is no
        // DOM and no canvas, so this is the expected outcome for the debug build. GROUP 2b's
        // guard stays closed and the headless path is unchanged.
        std::fprintf(stderr, "[eden-gl] no canvas '%s' — staying headless (expected under node).\n",
                     kCanvasTarget);
        return 0;
    }
    if (emscripten_webgl_make_context_current(ctx) != EMSCRIPTEN_RESULT_SUCCESS) {
        std::fprintf(stderr, "[eden-gl] context created but could not be made current.\n");
        emscripten_webgl_destroy_context(ctx);
        return 0;
    }
    g_ctx = ctx;
    g_context_lost = false;

    // Audit item #5. Registered per created context, and re-registering on the same target is
    // idempotent in Emscripten's html5 event layer (it replaces the handler), so the restore path
    // coming back through here does not stack listeners.
    emscripten_set_webglcontextlost_callback(kCanvasTarget, nullptr, EM_FALSE,
                                            eden_gl_on_context_lost);
    emscripten_set_webglcontextrestored_callback(kCanvasTarget, nullptr, EM_FALSE,
                                                eden_gl_on_context_restored);

    if (drawable_width > 0 && drawable_height > 0) {
        emscripten_set_canvas_element_size(kCanvasTarget, drawable_width, drawable_height);
        g_drawable_w = drawable_width;
        g_drawable_h = drawable_height;
    } else {
        emscripten_get_canvas_element_size(kCanvasTarget, &g_drawable_w, &g_drawable_h);
    }

    // Seed the viewport mirror BEFORE the engine's first draw. Util.mm's unproject reads
    // GL_VIEWPORT, and an all-zeros rect there is a silent, non-crashing wrong answer — the
    // exact failure class docs/rendering.md warns about.
    g_viewport[0] = 0; g_viewport[1] = 0;
    g_viewport[2] = g_drawable_w; g_viewport[3] = g_drawable_h;
    glViewport(0, 0, g_drawable_w, g_drawable_h);

    std::fprintf(stderr, "[eden-gl] WebGL2 context live on '%s' (%dx%d px).\n",
                 kCanvasTarget, g_drawable_w, g_drawable_h);
    return 1;
}

void eden_gl_context_destroy(void) {
    if (!g_ctx) return;
    emscripten_webgl_destroy_context(g_ctx);
    g_ctx = 0;
    g_drawable_w = g_drawable_h = 0;
    // The program/stream buffers below belong to the destroyed context; drop the handles so
    // the next create() rebuilds them rather than reusing names from a dead context.
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
    // Mirror is always kept current (even headless — Util.mm's unproject reads GL_VIEWPORT
    // through the shim regardless of whether a real context exists). The actual canvas resize +
    // real glViewport only happen when a context is live: under `node eden.js` there is no
    // canvas element to size and no GL to call.
    g_viewport[0] = 0; g_viewport[1] = 0;
    g_viewport[2] = g_drawable_w; g_viewport[3] = g_drawable_h;
    if (!eden_gl_have_context()) return;
    emscripten_set_canvas_element_size(kCanvasTarget, g_drawable_w, g_drawable_h);
    glViewport(0, 0, g_drawable_w, g_drawable_h);
    std::fprintf(stderr, "[eden-gl] drawable resized to %dx%d px (retina).\n",
                 g_drawable_w, g_drawable_h);
}

// Audit item #6 (§4c.1-2): the page's entry point for a DYNAMIC drawable — CSS box size x
// min(devicePixelRatio, cap) x render_scale, computed in JS (public/eden-st.html's
// applyDrawableSize()) because only the DOM knows the box and the device pixel ratio.
//
// WHAT THIS DOES NOT TOUCH, and must never touch: SCREEN_WIDTH/SCREEN_HEIGHT/SCALE_* — the
// engine's 568x320 POINT space (EAGLView_web.mm's establishScreenMetrics). Only the backing store
// and the viewport change; the projection stays in points, and eden-st.html's toEnginePoint()
// already derives from getBoundingClientRect(), so no coordinate-space work is needed anywhere.
// This is exactly the boundary the audit's risk note draws around this item.
//
// The clamp is a real guard, not defensiveness: a 4K fullscreen box at devicePixelRatio 2 asks for
// 7680x4320, which exceeds GL_MAX_RENDERBUFFER_SIZE (typically 4096-16384, and 4096 on plenty of
// mobile GPUs) — an oversized drawable fails at canvas-resize time and leaves a black frame.
EDEN_GL_EXPORT
void eden_set_drawable_size(int width, int height) {
    const int kMaxDrawableDim = 4096;
    if (width <= 0 || height <= 0) return;
    if (width > kMaxDrawableDim || height > kMaxDrawableDim) {
        // Scale both axes by the same factor so the aspect ratio the page computed survives.
        float s = (float)kMaxDrawableDim /
                  (float)((width > height) ? width : height);
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
    glViewport(0, 0, g_drawable_w, g_drawable_h);
    g_viewport[0] = 0; g_viewport[1] = 0;
    g_viewport[2] = g_drawable_w; g_viewport[3] = g_drawable_h;
}


} // extern "C"
