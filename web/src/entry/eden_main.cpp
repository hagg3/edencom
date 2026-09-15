// eden_main.cpp — the real WASM entry point (`int main()`). Owns:
//   1. calling into src/seam/main_web.* to build the EdenAppDelegate/EdenViewController/World
//      chain (mirrors UIApplicationMain() handing off to the app delegate);
//   2. the requestAnimationFrame-equivalent loop that used to be CADisplayLink
//      (Classes/EdenViewController.mm, replaced) — via emscripten_set_main_loop, which
//      internally uses rAF and (critically) does NOT block the browser's event loop, unlike a
//      plain while(true).
//
// WHAT ACTUALLY RUNS: the single-threaded build (EDEN_THREADED=OFF, the default since audit row
// A1) on the browser's main thread, against a normal <canvas> owned by public/eden-st.html, with
// a fully wired GL shim behind it. The threaded/OffscreenCanvas architecture the older comments
// here described — `main` proxied onto a worker via PROXY_TO_PTHREAD, a transferred
// OffscreenCanvas, public/worker-bootstrap.js's handshake — was never built. It is audit row 36
// (3-6 weeks, and gated behind row 9's cooperative world load), and worker-bootstrap.js /
// public/index.html are sketches for it, not live code (audit row 8 disposes of them). Under
// headless `node eden.js` there is no canvas and no rAF; emscripten falls back to
// MainLoop.fakeRequestAnimationFrame, a real ~60 Hz setTimeout loop, and the GL shim no-ops.

#include "../seam/main_web.h"
#include "../seam/EdenAppDelegate_web.h"
#include "../shim/gl/gl_es1_shim.h"

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#endif

// perf-audit row #14: an optional frame-rate cap, mainly for thermal/battery on touch profiles
// (defaulted there — see Settings_web.mm's eden_apply_input_profile). `emscripten_set_main_loop`
// is registered with fps=0 (rAF-driven, the display's native refresh rate) so the cap is a simple
// "are we early?" gate rather than a second timer — cheap, and it can never make the loop run
// FASTER than the display, only drop frames.
//
// Audit row A4 (fixed): the cap used to `return` from this callback outright, which skipped
// World::update() along with the drawing. World::update() is what consumes queued touches and
// keys — with a 30 fps cap on a 120 Hz display three of every four pointer events landed in a
// tick that never ran the consumer, and because every `hud->` input flag is re-derived from the
// live touch set each frame, a tap that began and ended inside a skipped window was dropped
// entirely. The cap is defaulted ON for touch profiles, so this was a mobile-only input-drop bug.
// The gate now only decides whether the frame DRAWS; update runs every rAF.
//
// Cost note: capping now saves the render half only, so the thermal/battery win is smaller than
// it looks on paper (per pass 47, column decode/meshing in Terrain::update is the dominant cost,
// and that now runs uncapped). That is the deliberate trade — dropped input is a correctness bug,
// a smaller power saving is not.
extern "C" int eden_get_fps_cap(void);

#ifdef __EMSCRIPTEN__
// Audit row A8: public/eden-st.html's trackCursorNeed used to be a SECOND, independent rAF chain
// reading/writing pointer-lock state, crosshair visibility and drawable size — registered
// separately from emscripten_set_main_loop, so its ordering relative to the engine's own tick
// within a frame was whatever registration order happened to land on, producing one-frame-latency
// cursor/lock quirks. Calling this at the tail of every eden_frame_tick() instead means the page's
// per-frame DOM work always runs in the SAME callback, right after whatever this tick did to the
// engine, with no second scheduler in the loop. `Module.__edenFramePost` is optional so headless
// runs (no `window`/no hook installed) are a plain no-op.
EM_JS(void, eden_frame_post_hook, (), {
    if (Module.__edenFramePost) Module.__edenFramePost();
});
#endif

#ifdef __EMSCRIPTEN__
extern "C" void eden_heap_pressure_tick(void);  // src/seam/HeapProbe_web.mm
#endif

static void eden_frame_tick() {
    eden_web::EdenAppDelegate* app = eden_web::eden_seam_get_app_delegate();
    if (app && app->viewController.isAnimating()) {
        bool renderThisFrame = true;

#ifdef __EMSCRIPTEN__
        int capFps = eden_get_fps_cap();
        if (capFps > 0) {
            static double lastRenderMs = 0.0;
            double now = emscripten_get_now();
            double minInterval = 1000.0 / (double)capFps;
            if (lastRenderMs != 0.0 && now - lastRenderMs < minInterval) {
                renderThisFrame = false;
            } else {
                lastRenderMs = now;
            }
        }
#endif

        app->viewController.drawFrame(renderThisFrame);
        // The bool it returns is the engine's retina/graphics-quality swap request. Deliberately
        // not acted on — audit row 22 (B7), resolved as an explicit no-op; the full reasoning is
        // at the matching block in EdenViewController_web::drawFrame(). Short version: this port's
        // IS_IPAD/SCALE_* globals are its LAYOUT point space, not its resolution (that is
        // applyDrawableSize + render_scale + dpr_cap), so honouring the swap would move the HUD's
        // coordinate system out from under an unchanged drawable. Revisit only with row 18 (D1).
    }

#ifdef __EMSCRIPTEN__
    // ROADMAP Phase M / M1: -sMAXIMUM_MEMORY made "out of linear memory" a reachable end to a
    // session, so something has to watch the approach. Self-throttling to once every 600 frames
    // and one-shot per threshold — see src/seam/HeapProbe_web.mm.
    eden_heap_pressure_tick();
    eden_frame_post_hook();
#endif
}

#if defined(__EMSCRIPTEN__) && defined(EDEN_DIAGNOSTICS)
// TEMPORARY test-harness aid (pass 22, delete with the DebugState probe): the browser throttles
// the rAF-driven main loop to ~0 fps while the automation tab is backgrounded (document.hidden),
// which makes `_eden_input_pointer_event` -> Menu::update consumption non-deterministic (a touch
// may sit unclaimed for seconds). This lets a test pump N frames synchronously regardless of tab
// visibility: `Module._eden_debug_tick(30)`. It just calls the same frame tick the rAF loop does.
// Gated behind EDEN_DIAGNOSTICS (perf audit Q7) — a shipped build has no scripted driver to serve.
extern "C" EMSCRIPTEN_KEEPALIVE void eden_debug_tick(int n) {
    for (int i = 0; i < n; i++) eden_frame_tick();
}
#endif

int main(int argc, char** argv) {
    (void)argc; (void)argv;

    // The WebGL2 context MUST be live before eden_seam_main(), and that ordering is not a
    // preference — it is measured. `World::World()` calls `Graphics::initGraphics()`, which
    // issues real glGenBuffers/glBufferData/glEnable during construction (PORT-STATUS Pass 8
    // §1). Stage P1's premise that "construction is GL-free" was wrong; creating the context
    // first is what makes those calls land on a real context instead of the headless guard.
    //
    // A failure here is NOT fatal: under `node eden.js` there is no canvas, create() returns 0,
    // and the guard keeps the whole engine running headless — which is exactly what the
    // EDEN_THREADED=OFF debug build wants.
    eden_gl_context_create(0, 0);

    eden_web::eden_seam_main();

#ifdef __EMSCRIPTEN__
    // fps=0 -> driven by requestAnimationFrame at the display's native refresh rate;
    // simulate_infinite_loop=true -> main() returns immediately, matching Emscripten's
    // expected shape for a persistent, event-driven web app (mirrors the original's
    // UIApplicationMain() never really "returning" either).
    emscripten_set_main_loop(eden_frame_tick, 0, /*simulate_infinite_loop=*/true);
#else
    // TODO P0.1/P1: no non-Emscripten target is planned (this engine has no desktop port per
    // docs/engine-vs-game.md's own note: "a desktop port was once contemplated but never
    // started"), so this branch exists only so the file has SOME shape to read/reason about
    // on a machine without emcc (this one, currently — see archive/PORT-STATUS-2026-08-13.md). Not a real
    // fallback loop.
    eden_frame_tick();
#endif

    return 0;
}
