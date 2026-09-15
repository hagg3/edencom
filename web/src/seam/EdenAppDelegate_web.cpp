#include "EdenAppDelegate_web.h"
#include "AppLifecycle_web.h"

namespace eden_web {

void EdenAppDelegate::didFinishLaunching() {
    viewController.construct();
    viewController.startAnimation();
}

void EdenAppDelegate::onVisibilityHidden() {
    // Phase N Stage 4.2. A hidden tab is a tab the browser is allowed to discard without another
    // callback, so this is a real departure and not just a pause -- save first, THEN stop the
    // loop.
    //
    // READ THIS BEFORE BELIEVING THIS LINE RUNS ON WEB: nothing drives this delegate's lifecycle
    // methods on the web target today (grep-confirmed -- main_web.cpp calls didFinishLaunching()
    // and that is the only call site anywhere; the P7 header's "'visibilitychange' ->
    // onVisibilityHidden()" mapping was a design, never a wiring). public/eden-host.js therefore
    // calls _eden_app_will_background() DIRECTLY, and that is the path Stage 4.2 actually ships
    // and tests. The call below exists so that the two ways in cannot disagree if the delegate is
    // ever driven for real -- on native it already is (SDL_EVENT_QUIT -> onPageHide) -- and
    // because eden_app_will_background() is idempotent-shaped, a doubled call is harmless.
    eden_app_will_background();
    viewController.stopAnimation();
}

void EdenAppDelegate::onVisibilityVisible() {
    viewController.startAnimation();
}

void EdenAppDelegate::onPageHide() {
    // The C1 audit fix that was a TODO here from Stage P7 until Phase N Stage 4.2, waiting on
    // "FileManager's web-facing save entry point" -- which is now eden_app_will_background().
    // Same caveat as onVisibilityHidden() above about who actually calls this on web.
    eden_app_will_background();
    viewController.stopAnimation();
}

} // namespace eden_web
