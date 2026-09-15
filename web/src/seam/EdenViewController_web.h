// EdenViewController_web.h — seam replacement for Classes/EdenViewController.mm. This is the
// live, single-threaded (EDEN_THREADED=OFF, the default since audit row A1) production frame
// loop's per-frame worker — not a headless smoke test; it runs on every real page load.
//
// UNLIKE EAGLView_web.mm (which is forced to reuse the original Classes/EAGLView.h — see that
// file's header comment for why), NOTHING outside Classes/EdenViewController.mm itself
// includes "EdenViewController.h" (grep-confirmed, see docs/archive/PORT-STATUS-2026-08-13.md). So this seam
// replacement is free to be a plain C++ class instead of an Objective-C UIViewController
// subclass — there is no nib/storyboard on the web, no CADisplayLink-vs-NSTimer fallback
// dance (Classes/EdenViewController.mm:135-181, all iOS-runtime-version-detection cruft with
// no web equivalent).
//
// Owns the `World*` and reproduces Classes/EdenViewController.mm's drawFrame() logic
// (etime computation, world->update/render, the IS_RETINA/IS_IPAD graphics-quality swap,
// resolved as a deliberate no-op — audit row B7/22) — see the .cpp for the line-by-line mapping.
// Driven by src/entry/eden_main.cpp's emscripten_set_main_loop callback (real requestAnimationFrame,
// main thread — there is no worker/OffscreenCanvas loop; that's audit row 36, unbuilt), not by this
// class itself — this class does not own the loop, just one frame's worth of work, matching the
// original split (EdenViewController owned the CADisplayLink target, but drawFrame did the actual
// work per invocation).
#ifndef EDEN_SEAM_EDENVIEWCONTROLLER_WEB_H
#define EDEN_SEAM_EDENVIEWCONTROLLER_WEB_H

class World;

namespace eden_web {

class EdenViewController {
public:
    EdenViewController();
    ~EdenViewController();

    // Originally driven by -awakeFromNib (EAGLContext creation, low-memory-device detection via
    // [NSProcessInfo processInfo].physicalMemory — Classes/EdenViewController.mm:24-74). The
    // memory-tier detection (LOW_GRAPHICS/LOW_MEM_DEVICE) has no web equivalent yet and is left
    // at its zero-initialised "best graphics" default (see the .cpp's construct() and audit row
    // B7's note on why nothing currently sets these). Still open: derive a substitute from
    // navigator.deviceMemory (widely but not universally supported), or default to best-graphics
    // on desktop and flag it a known "web build differs from device here" note in
    // docs/player-input-camera.md / docs/rendering.md once decided.
    void construct();

    void startAnimation();
    void stopAnimation();
    bool isAnimating() const { return animating; }

    // One frame. Returns what World::update returned (retina-swap-requested flag) so the
    // caller (src/entry/eden_main.cpp) can decide whether to react — mirrors
    // Classes/EdenViewController.mm:188-229's drawFrame. Does not call EAGLView_web.mm's
    // setFramebuffer/presentFramebuffer/createFramebuffer/deleteFramebuffer: the GL context is
    // created once by src/shim/gl/gl_es1_shim.cpp at boot and WebGL presents implicitly when the
    // rAF callback returns, so there is no per-frame swap-buffers call to make (see
    // EAGLView_web.mm's presentFramebuffer for the one-line confirmation of that).
    //
    // Audit row A4: `renderThisFrame == false` runs the update half only (input consumption,
    // physics, streaming) and hands World::renderFrame(FALSE) the draw skip. The frame-rate cap
    // in eden_main.cpp uses that instead of skipping the whole tick — see that file's gate.
    bool drawFrame(bool renderThisFrame = true);

    World* world = nullptr;

private:
    bool animating = false;
    double startTime = 0.0;   // seconds, monotonic — emscripten_get_now()-backed (audit row B3)
    double lastTime = 0.0;
};

} // namespace eden_web

#endif
