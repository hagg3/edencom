// App.cpp — startup, the frame and the lifecycle hooks, in boot order (SM.9; see App.h).
//
//   port/src/harness/main.cpp   creates the window + GL context, then calls App::boot()
//   App::boot()                 screen metrics -> World -> start the loop
//   App::drawFrame()            World::update, World::renderFrame inside the R:: frame bracket, present
//   App::onVisibility*/onPageHide   the SDL focus / quit / background events
#include "App.h"

#include <algorithm>
#include <cstdio>

#include "../../../Classes/World.h"
#include "../../../Classes/Globals.h"
#include "../../../Classes/PassTimers.h"
#include "../../../Classes/Renderer.h"
#include "gl_context.h"
#include "platform_shims.h"
#include "AppLifecycle.h"

extern "C" void eden_native_gl_present(void);
extern "C" void eden_native_apply_display_mode(void);   // src/seam/DisplayMode_native.cpp
// The C pool seam (AutoreleasePool_native.cpp): on Apple the frame still reaches seam ObjC (AppleNet,
// AppleAudio, IOSKeyboard) that may autorelease; elsewhere it is a no-op (G.3).
extern "C" void* eden_autoreleasepool_push(void);
extern "C" void eden_autoreleasepool_drain(void* pool);

// P.1a: the thermal state (0 nominal, 1 fair, 2 serious, 3 critical), -1 where there is no such
// notion. Apple's is AppleNet_native.mm (the ObjC wall); the rest answer here.
#if !defined(__APPLE__)
extern "C" int eden_native_thermal_state(void) { return -1; }
#endif

namespace eden_native {

static App* g_app = nullptr;   // process-lifetime, as the app delegate is on iOS

static double now_seconds() { return eden_platform_now_ms() / 1000.0; }

App* App::get() { return g_app; }

void App::boot() {
    if (g_app) return;
    // 1. The screen-metric globals. ORDER IS LOAD-BEARING: World::World() reads SCREEN_WIDTH/
    // SCREEN_HEIGHT/P_ASPECT_RATIO through Graphics. The engine is landscape, so SCREEN_WIDTH is the
    // LONG axis -- do not "fix" that apparent inversion (CLAUDE.md convention #3). The point space is
    // DERIVED (window aspect x ui_scale) by DisplayProfile.cpp; this asks DisplayMode_native.cpp to
    // hand it the shape of the real window and fit the drawable to the aspect it chose.
    eden_native_apply_display_mode();
    // 2. The world. LOW_GRAPHICS/LOW_MEM_DEVICE stay at their zero-initialised "best graphics" default.
    g_app = new App();
    g_app->world = new World();
    // 3. Run.
    g_app->startAnimation();
}

void App::startAnimation() {
    if (animating) return;
    startTime = now_seconds();
    lastTime = 0.0;
    animating = true;
}

void App::stopAnimation() {
    animating = false;
}

bool App::drawFrame(bool renderThisFrame) {
    double now = now_seconds() - startTime;
    float etime = (float)(now - lastTime);
    lastTime = now;

    // Fixed-step override for the scripted harnesses (setFixedEtime). Applied AFTER the clock read so
    // `lastTime` still advances honestly: a run that switches back to real time mid-session does not
    // then get one enormous delta.
    if (fixedEtime > 0.0f) etime = fixedEtime;

    // Clamp etime. A window drag, a breakpoint or a stalled disk read otherwise feeds a multi-second
    // delta straight into Terrain::update and physics.
    constexpr float kMaxEtime = 0.1f;
    etime = std::min(etime, kMaxEtime);

    // Each frame is wrapped in an autorelease pool on Apple; without one every autoreleased object the
    // frame creates accumulates into a root pool nothing drains.
    void *autoreleasePool = eden_autoreleasepool_push();

    // The first three ticks are logged; the native p1 gate asserts these exact lines.
    static int tickCount = 0;
    if (tickCount < 3) {
        std::fprintf(stderr, "[eden-p1] tick %d: World::update(%.4f s) entered\n", tickCount, etime);
    }

    bool retinaSwapRequested = world && world->update(etime);

    if (tickCount < 3) {
        std::fprintf(stderr, "[eden-p1] tick %d: World::update returned (retinaSwap=%d)\n",
                     tickCount, (int)retinaSwapRequested);
        ++tickCount;
    }

    if (renderThisFrame) eden_gl_stats_frame_boundary();

    // renderFrame() is called even when the frame does not draw: it owns the GAME_MODE_WAIT ->
    // target_game_mode transition and the exit-to-menu check, neither of which World::update() can
    // reach. Skipping it wedges a loading world in WAIT forever.
    // The R:: frame bracket (Classes/Renderer.h) asserts nothing in play; under --render-selftest it is
    // where the matrix-stack, 2D/scene and pass balance invariants are checked.
    R::beginFrame();
    if (world) world->renderFrame(renderThisFrame);
    R::endFrame();

    // The engine's retina/quality swap is a deliberate no-op: IS_IPAD/SCALE_* are this port's LAYOUT
    // point space, not its resolution, so honouring the swap would move the HUD's coordinate system
    // out from under an unchanged drawable.
    if (retinaSwapRequested) {
        static bool announced = false;
        if (!announced) {
            announced = true;
            std::fprintf(stderr,
                "[eden] World::update requested a retina/quality swap. Ignored by design.\n");
        }
    }

    eden_autoreleasepool_drain(autoreleasePool);

    // Desktop GL has to be told to present. Guarded on renderThisFrame so a capped frame does not
    // block on vsync for a frame it never drew. With vsync off (--pass-probe), the time blocked here
    // is the GPU proxy.
    if (renderThisFrame) { pass_begin(P_PRESENT); eden_native_gl_present(); pass_end(P_PRESENT); }

    return retinaSwapRequested;
}

void App::onVisibilityHidden() {
    // DELIBERATELY DOES NOT SAVE. This is SDL_EVENT_WINDOW_FOCUS_LOST, i.e. alt-tab on a desktop, and
    // a desktop window that lost focus is not going anywhere.
    stopAnimation();
}

void App::onVisibilityVisible() {
    startAnimation();
}

void App::onPageHide() {
    // Closing the window (SDL_EVENT_QUIT) would otherwise throw away everything since the last
    // streaming boundary. Also driven by SDL_EVENT_WILL_ENTER_BACKGROUND on iOS/Android, which is the
    // case this exists for -- see the event watch in port/src/harness/core.cpp for why that one cannot
    // go through the poll loop.
    eden_app_will_background();
    stopAnimation();
}

}  // namespace eden_native
