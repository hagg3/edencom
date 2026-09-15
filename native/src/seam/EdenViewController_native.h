// EdenViewController_native.h — native twin of web/src/seam/EdenViewController_web.h
// (Phase N Stage 1, WORKING/native-migration-plan-2026-09-04.md).
//
// Nothing outside Classes/EdenViewController.mm itself includes "EdenViewController.h"
// (grep-confirmed for the web seam and re-checked for this one), so this replacement is free to
// be a plain C++ class rather than a UIViewController subclass — no nib, no CADisplayLink-vs-
// NSTimer fallback dance.
//
// Owns the `World*` and reproduces Classes/EdenViewController.mm's drawFrame() logic. Identical
// to the web twin apart from three things, each marked at its site in the .cpp:
//   * the clock is eden_platform_now_ms() (Stage 0.3's portable clock) rather than
//     emscripten_get_now();
//   * drawFrame() presents the frame, because a desktop GL context does not present implicitly
//     the way WebGL does when the rAF callback returns;
//   * the retina/quality-swap request is reported and ignored for the SAME reason the web twin
//     ignores it (the IS_IPAD/SCALE_* globals are the LAYOUT point space, not the resolution).
#ifndef EDEN_SEAM_EDENVIEWCONTROLLER_NATIVE_H
#define EDEN_SEAM_EDENVIEWCONTROLLER_NATIVE_H

class World;

namespace eden_native {

class EdenViewController {
public:
    EdenViewController();
    ~EdenViewController();

    // Originally -awakeFromNib. LOW_GRAPHICS/LOW_MEM_DEVICE detection from real physical memory
    // is Stage 2's row (the plan's "Low-memory detection" line); left at the zero-initialised
    // "best graphics" default here so Stage 1's numbers are taken on the same settings the web
    // build measures with.
    void construct();

    void startAnimation();
    void stopAnimation();
    bool isAnimating() const { return animating; }

    // One frame. Returns what World::update returned (the retina-swap-requested flag).
    bool drawFrame(bool renderThisFrame = true);

    // Phase N Stage 2: force a FIXED etime instead of reading the clock. Zero (the default) means
    // "use the real elapsed time", which is what an interactive session must do.
    //
    // This exists because the scripted harnesses were not reproducible: etime comes from the wall
    // clock, so a frame's worth of simulated time depends on how loaded the machine is, and
    // "hold W for 300 frames" measured anywhere from 0.00 to 92.31 blocks of travel across runs on
    // the same binary. A harness whose result depends on what else is running is not evidence.
    // With a fixed step, N frames is always exactly N/60 s of engine time.
    void setFixedEtime(float seconds) { fixedEtime = seconds; }

    World* world = nullptr;

private:
    bool animating = false;
    double startTime = 0.0;   // seconds, monotonic — eden_platform_now_ms()-backed
    double lastTime = 0.0;
    float  fixedEtime = 0.0f; // 0 = use the clock; see setFixedEtime()
};

}  // namespace eden_native

#endif
