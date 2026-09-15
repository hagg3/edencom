// EdenAppDelegate_web.h — Stage P1/P7 seam replacement for Classes/EdenAppDelegate.mm.
//
// Like EdenViewController_web (see that file's header), nothing outside
// Classes/EdenAppDelegate.mm itself includes "EdenAppDelegate.h" (grep-confirmed) — free to be
// plain C++. Reproduces the lifecycle shape of Classes/EdenAppDelegate.mm's
// UIApplicationDelegate callbacks, mapped onto browser lifecycle events (Stage P7):
//   didFinishLaunchingWithOptions:  -> construct()          (called once, from main_web)
//   applicationWillResignActive:    -> onVisibilityHidden()  (document 'visibilitychange')
//   applicationDidBecomeActive:     -> onVisibilityVisible()
//   applicationWillTerminate:       -> onPageHide()           ('pagehide' — THE C1 audit fix,
//                                       web-port-plan.md Stage P7: "save-on-background...
//                                       now trivial and correct on web")
//   applicationDidEnterBackground:  -> (original was empty — nothing to port)
// Appirater/Flurry (Classes/EdenAppDelegate.mm:11-12,33,35,63) are DROPPED per plan ("Strip:
// Appirater.mm (rating), Flurry key... they don't belong in a web build" — CLAUDE.md L12 /
// web-port-plan.md Stage P7). No replacement needed: nothing else in the non-seam engine
// references Appirater (grep-confirmed, see foundation-usage.md).
#ifndef EDEN_SEAM_EDENAPPDELEGATE_WEB_H
#define EDEN_SEAM_EDENAPPDELEGATE_WEB_H

#include "EdenViewController_web.h"

namespace eden_web {

class EdenAppDelegate {
public:
    void didFinishLaunching();     // owns viewController construction + startAnimation
    void onVisibilityHidden();     // -> viewController.stopAnimation()
    void onVisibilityVisible();    // -> viewController.startAnimation()
    void onPageHide();             // Phase N Stage 4.2: eden_app_will_background() (the C1 fix,
                                    // formerly a P4/P7 TODO here) and then stopAnimation().
                                    // NOTE: on web nothing drives these three lifecycle methods
                                    // yet — public/eden-host.js calls the export directly. See
                                    // EdenAppDelegate_web.cpp's comment for the full story.

    EdenViewController viewController;
};

} // namespace eden_web

#endif
