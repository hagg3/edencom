// EdenAppDelegate_native.h — native twin of web/src/seam/EdenAppDelegate_web.h (Phase N Stage 1).
//
// Nothing outside Classes/EdenAppDelegate.mm includes "EdenAppDelegate.h", so this is free to be
// plain C++. Reproduces the lifecycle shape of Classes/EdenAppDelegate.mm's UIApplicationDelegate
// callbacks, mapped onto SDL's app lifecycle rather than the browser's:
//   didFinishLaunchingWithOptions: -> didFinishLaunching()  (once, from main_native)
//   applicationWillResignActive:   -> onVisibilityHidden()  (SDL_EVENT_WINDOW_FOCUS_LOST)
//   applicationDidBecomeActive:    -> onVisibilityVisible() (SDL_EVENT_WINDOW_FOCUS_GAINED)
//   applicationWillTerminate:      -> onPageHide()          (SDL_EVENT_QUIT)
// Appirater/Flurry are dropped, same as on web.
#ifndef EDEN_SEAM_EDENAPPDELEGATE_NATIVE_H
#define EDEN_SEAM_EDENAPPDELEGATE_NATIVE_H

#include "EdenViewController_native.h"

namespace eden_native {

class EdenAppDelegate {
public:
    void didFinishLaunching();
    void onVisibilityHidden();
    void onVisibilityVisible();
    // Phase N Stage 4.2: eden_app_will_background() then stopAnimation(). Driven by
    // SDL_EVENT_QUIT and, on iOS/Android, SDL_EVENT_WILL_ENTER_BACKGROUND. onVisibilityHidden()
    // above deliberately does NOT save — see the .cpp for the asymmetry with web.
    void onPageHide();

    EdenViewController viewController;
};

}  // namespace eden_native

#endif
