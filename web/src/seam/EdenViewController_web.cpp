// EdenViewController_web.cpp — see header for the design-decision rationale (fresh C++, not a
// reused Objective-C interface).
#include "EdenViewController_web.h"
#include <cstdio>
#include <algorithm>
#include "../../../Classes/World.h"
#include "../../../Classes/Globals.h"
#include <emscripten/emscripten.h>
#include "../shim/gl/gl_es1_shim.h"
#include "../shim/foundation/NSAutoreleasePool.h"

namespace eden_web {

static double eden_now_seconds() {
    // Audit row B3: this runs every frame (drawFrame below), so the clock read is on the hot
    // path. `emscripten_get_now()` binds straight to `performance.now()` — the same clock
    // requestAnimationFrame itself uses — where `std::chrono::steady_clock` under wasm routed
    // through a heavier libc path for the identical value. Resolves the TODO P1 this function
    // carried (NSDate.mm's `eden_now_seconds` is a separate, wall-clock-semantics function and
    // out of this row's scope — see its own comment).
    return emscripten_get_now() / 1000.0;
}

EdenViewController::EdenViewController() {}

EdenViewController::~EdenViewController() {
    delete world; // mirrors Classes/EdenViewController.mm's -dealloc ("delete world;")
}

void EdenViewController::construct() {
    // Mirrors Classes/EdenViewController.mm:40-51's LOW_GRAPHICS/LOW_MEM_DEVICE memory-tier
    // detection — see header TODO. Defaulting to "best graphics" until that's decided.
    // TODO P2: LOW_GRAPHICS = ...; LOW_MEM_DEVICE = ...;
    world = new World();
}

void EdenViewController::startAnimation() {
    if (animating) return;
    startTime = eden_now_seconds();
    lastTime = 0.0;
    // Confirmed: src/entry/eden_main.cpp owns the requestAnimationFrame registration
    // (emscripten_set_main_loop, called once from main()) — this flag only gates eden_frame_tick()
    // from doing work per call, matching the original's animating-bool-gates-drawFrame shape.
    animating = true;
}

void EdenViewController::stopAnimation() {
    if (!animating) return;
    // No registration to cancel here (see startAnimation above) — emscripten_set_main_loop keeps
    // calling eden_frame_tick() every rAF regardless; this flag is what makes those calls no-ops,
    // mirroring the effect (not the mechanism) of Classes/EdenViewController.mm:164-181's
    // displayLink/animationTimer invalidate.
    animating = false;
}

bool EdenViewController::drawFrame(bool renderThisFrame) {
    // Mirrors Classes/EdenViewController.mm:188-229. Does not call EAGLView_web.mm's
    // setFramebuffer/presentFramebuffer/deleteFramebuffer/createFramebuffer — see the header's
    // note on drawFrame() for why: the GL context is created once at boot
    // (src/entry/eden_main.cpp's eden_gl_context_create(), before eden_seam_main()) and WebGL
    // presents implicitly when this rAF callback returns, so there is no per-frame framebuffer
    // dance to reproduce.
    double now = eden_now_seconds() - startTime;
    float etime = (float)(now - lastTime);
    lastTime = now;

    // Audit row A3: clamp etime. Unclamped, a backgrounded tab / GC pause / devtools breakpoint
    // / syncfs stall feeds a multi-second delta straight into Terrain::update and physics —
    // guaranteed on the very first tab-switch, not an edge case. Cap chosen as ~3 frames at
    // 30 fps (matches the mobile fps-cap floor elsewhere in this file's caller); World::update
    // itself already substeps large etime less gracefully than this cap avoids needing to rely on.
    constexpr float kMaxEtime = 0.1f;
    etime = std::min(etime, kMaxEtime);

    // Audit row A2 (TODO P1, Classes/EdenViewController.mm:198): real Foundation code wraps
    // each frame in an NSAutoreleasePool. Without one, every autorelease'd object World::update/
    // render create (the shim's NSSet/UIEvent/NSString paths autorelease routinely) accumulated
    // into whatever outer pool existed, or leaked outright — a slow heap climb that only shows up
    // as a long-session memory ramp. C-linkage push/drain (NSAutoreleasePool.h) keeps this file
    // plain C++ per its header comment's rationale.
    void *autoreleasePool = eden_autoreleasepool_push();

    // Under headless `node eden.js` there is no canvas and nothing renders, so "the engine is
    // alive and ticking" looks EXACTLY like "it hung during construction" — emscripten_set_main_loop
    // never returns either way. These three lines are the only observable difference, which is why
    // `tools/headless-p1-gate.js` greps stderr for exactly this text rather than watching a canvas.
    // Gated behind EDEN_DIAGNOSTICS (perf audit Q7/C8) — kept for build-st's default-ON
    // diagnostics, compiled out of a build meant to be played.
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

    // Perf-audit item #4: rotate the GL shim's per-frame draw-call accounting. Here rather than
    // inside the shim's glClear, because prepareScene does not clear the colour buffer — see
    // eden_gl_stats_frame_boundary()'s comment. Cheap (two struct copies) and it is what
    // Module._eden_gl_stat() reports. Only rotated on frames that actually draw, so a capped
    // frame does not report a spurious zero-draw frame to _eden_gl_stat().
    if (renderThisFrame) eden_gl_stats_frame_boundary();

    // Audit row A4: even when the cap says "don't draw", renderFrame() is still CALLED — it owns
    // the GAME_MODE_WAIT -> target_game_mode transition and the exit_to_menu check, neither of
    // which is reachable from World::update(). Skipping the call outright wedges a loading world
    // in WAIT forever. (The port's recurring defect class: a replacement owes the side effects,
    // not just the visible output.)
    if (world) world->renderFrame(renderThisFrame);

    // ---- Audit row 22 (B7): the engine's retina/quality swap is a DELIBERATE NO-OP here. ----
    //
    // This block used to mirror Classes/EdenViewController.mm:207-226 — flip IS_IPAD / IS_RETINA /
    // SCALE_WIDTH / SCALE_HEIGHT, then delete and recreate the framebuffer at the new density. The
    // port kept the first half and never had the second, which is the worst of both: the layout
    // globals would move while the actual drawable stayed exactly where it was.
    //
    // Resolved as a no-op rather than wired up, for three reasons, in order of weight:
    //
    // 1. THOSE GLOBALS ARE THIS PORT'S LAYOUT COORDINATE SYSTEM, not its resolution. EAGLView_web's
    //    establishScreenMetrics pins SCREEN_WIDTH=568 / SCREEN_HEIGHT=320 / IS_IPAD=IS_RETINA=TRUE
    //    / SCALE=2 and the engine lays every HUD and menu element out in that point space; the real
    //    drawable is decoupled from it entirely by applyDrawableSize() in public/eden-st.html
    //    (CSS box x min(devicePixelRatio, dpr_cap) x render_scale). Flipping IS_IPAD to FALSE
    //    therefore does not lower the resolution — it halves the UI's own layout math while the
    //    surface is unchanged. That is the "latent layout-corruption path" the audit row names, and
    //    it is precisely the coupling row 18 (D1, unpin the display constants) exists to untangle.
    //    Do not re-wire this before D1 lands; afterwards, the swap belongs in whatever profile
    //    system D4 introduces, not here.
    // 2. THE PORT ALREADY HAS A BETTER KNOB FOR THE SAME INTENT. "Cheaper pixels" is what
    //    `render_scale` and `dpr_cap` do, as real user-facing settings, without touching layout.
    //    Reviving a 2010 iPhone-4-era @2x/@1x toggle on top of them would give the player two
    //    controls for one thing, one of which corrupts the HUD.
    // 3. IT IS CURRENTLY UNREACHABLE ANYWAY, so a no-op changes no observable behaviour today.
    //    World::update returns TRUE only for `game_mode==GAME_MODE_WAIT && SUPPORTS_RETINA &&
    //    !bestGraphics` (World.mm:477-480). `bestGraphics` is set TRUE at World.mm:184/193, and
    //    SettingsMenu::load's only path to FALSE is `LOW_MEM_DEVICE||LOW_GRAPHICS` — neither of
    //    which anything in this port ever assigns (they are zero-initialised BOOL globals and
    //    EdenViewController_web::construct() still carries the TODO saying so) — after which
    //    `IS_WIDESCREEN` forces it back TRUE regardless, and EAGLView_web.mm:110 pins that TRUE.
    //
    // The return value is still propagated so a caller (or a future D1/D4 profile system) can see
    // that the engine asked. Under EDEN_DIAGNOSTICS it announces itself once, because reason 3 is
    // the kind of "unreachable" that a single future assignment to LOW_GRAPHICS makes reachable
    // again, and silence would then be indistinguishable from correctness.
    if (retinaSwapRequested) {
#ifdef EDEN_DIAGNOSTICS
        static bool announced = false;
        if (!announced) {
            announced = true;
            std::fprintf(stderr,
                "[eden] World::update requested a retina/quality swap. Ignored by design "
                "(audit row 22 / B7) — flipping IS_IPAD/SCALE_* would move the layout point space "
                "without moving the drawable. If you are seeing this, something now sets "
                "LOW_MEM_DEVICE/LOW_GRAPHICS and this decision is worth revisiting alongside "
                "row 18 (D1).\n");
        }
#endif
    }

    eden_autoreleasepool_drain(autoreleasePool);

    return retinaSwapRequested;
}

} // namespace eden_web
