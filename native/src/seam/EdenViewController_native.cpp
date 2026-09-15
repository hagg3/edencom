// EdenViewController_native.cpp — see the header for the design rationale, and
// web/src/seam/EdenViewController_web.cpp for the annotated original this mirrors. Kept
// deliberately line-comparable with that file so the two stay diffable; the comments there
// explain WHY each step exists (audit rows A2/A3/A4/B7) and are not repeated in full here.
#include "EdenViewController_native.h"

#include <algorithm>
#include <cstdio>

#include "../../../Classes/World.h"
#include "../../../Classes/Globals.h"
#include "gl_es1_shim.h"
#include "platform_shims.h"
#include "NSAutoreleasePool.h"

extern "C" void eden_native_gl_present(void);

namespace eden_native {

static double eden_now_seconds() {
    // Stage 0.3's portable clock. On Emscripten it is performance.now(); here it is
    // std::chrono::steady_clock. Both are monotonic milliseconds, which is all this needs.
    return eden_platform_now_ms() / 1000.0;
}

EdenViewController::EdenViewController() {}

EdenViewController::~EdenViewController() {
    delete world;  // mirrors Classes/EdenViewController.mm's -dealloc
}

void EdenViewController::construct() {
    world = new World();
}

void EdenViewController::startAnimation() {
    if (animating) return;
    startTime = eden_now_seconds();
    lastTime = 0.0;
    animating = true;
}

void EdenViewController::stopAnimation() {
    if (!animating) return;
    animating = false;
}

bool EdenViewController::drawFrame(bool renderThisFrame) {
    double now = eden_now_seconds() - startTime;
    float etime = (float)(now - lastTime);
    lastTime = now;

    // Fixed-step override for the scripted harnesses — see setFixedEtime() in the header for why.
    // Deliberately applied AFTER the clock read so `lastTime` still advances honestly: a run that
    // switches back to real time mid-session does not then get one enormous delta.
    if (fixedEtime > 0.0f) etime = fixedEtime;

    // Audit row A3: clamp etime. A window drag, a breakpoint or a stalled disk read otherwise
    // feeds a multi-second delta straight into Terrain::update and physics.
    constexpr float kMaxEtime = 0.1f;
    etime = std::min(etime, kMaxEtime);

    // Audit row A2: real Foundation wraps each frame in an NSAutoreleasePool, and without one
    // every autoreleased object the frame creates accumulates into a root pool nothing drains.
    void *autoreleasePool = eden_autoreleasepool_push();

    // The same three lines web/tools/headless-p1-gate.js greps for, emitted identically so the
    // native gate can assert the same text (Stage 1 criterion 1).
#ifdef EDEN_DIAGNOSTICS
    static int tickCount = 0;
    if (tickCount < 3) {
        std::fprintf(stderr, "[eden-p1] tick %d: World::update(%.4f s) entered\n", tickCount, etime);
    }
#endif

    bool retinaSwapRequested = world && world->update(etime);

#ifdef EDEN_DIAGNOSTICS
    if (tickCount < 3) {
        std::fprintf(stderr, "[eden-p1] tick %d: World::update returned (retinaSwap=%d)\n",
                     tickCount, (int)retinaSwapRequested);
        ++tickCount;
    }
#endif

    if (renderThisFrame) eden_gl_stats_frame_boundary();

    // Audit row A4: renderFrame() is called even when the frame does not draw — it owns the
    // GAME_MODE_WAIT -> target_game_mode transition and the exit-to-menu check, neither of which
    // World::update() can reach. Skipping it wedges a loading world in WAIT forever.
    if (world) world->renderFrame(renderThisFrame);

    // Audit row 22 (B7): the engine's retina/quality swap is a deliberate no-op, exactly as on
    // web — IS_IPAD/SCALE_* are this port's LAYOUT point space, not its resolution, so honouring
    // the swap would move the HUD's coordinate system out from under an unchanged drawable.
    if (retinaSwapRequested) {
#ifdef EDEN_DIAGNOSTICS
        static bool announced = false;
        if (!announced) {
            announced = true;
            std::fprintf(stderr,
                "[eden] World::update requested a retina/quality swap. Ignored by design "
                "(audit row 22 / B7).\n");
        }
#endif
    }

    eden_autoreleasepool_drain(autoreleasePool);

    // THE ONE REAL DIVERGENCE FROM THE WEB TWIN: WebGL presents implicitly when the animation
    // frame callback returns; desktop GL has to be told. Guarded on renderThisFrame so a capped
    // frame does not block on vsync for a frame it never drew.
    if (renderThisFrame) eden_native_gl_present();

    return retinaSwapRequested;
}

}  // namespace eden_native
