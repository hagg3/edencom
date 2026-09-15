#include "EdenAppDelegate_native.h"
#include "../../../web/src/seam/AppLifecycle_web.h"

namespace eden_native {

void EdenAppDelegate::didFinishLaunching() {
    viewController.construct();
    viewController.startAnimation();
}

void EdenAppDelegate::onVisibilityHidden() {
    // DELIBERATELY DOES NOT SAVE, unlike the web twin. This is SDL_EVENT_WINDOW_FOCUS_LOST, i.e.
    // alt-tab on a desktop, and a desktop window that lost focus is not going anywhere -- nobody
    // is about to discard the process. On web a hidden tab genuinely IS a departure (the browser
    // may discard it with no further callback), which is why EdenAppDelegate_web.cpp saves here
    // and this file does not. The asymmetry is the platform, not an oversight.
    viewController.stopAnimation();
}

void EdenAppDelegate::onVisibilityVisible() {
    viewController.startAnimation();
}

void EdenAppDelegate::onPageHide() {
    // Phase N Stage 4.2, and on this target it closed a real hole rather than a theoretical one:
    // until now, closing the window (SDL_EVENT_QUIT) threw away everything since the last
    // streaming boundary. Also driven by SDL_EVENT_WILL_ENTER_BACKGROUND on iOS/Android, which is
    // the case the whole row exists for -- see the event watch in eden_main_native.cpp for why
    // that one cannot go through the poll loop.
    eden_app_will_background();
    viewController.stopAnimation();
}

}  // namespace eden_native
