// AppLifecycle_web.h — status codes for eden_app_will_background(). See AppLifecycle_web.mm.
//
// Kept in a header rather than inlined as bare integers because three callers agree on them: the
// implementation, the host (public/eden-host.js mirrors them in a comment) and
// tools/headless-save-on-background-test.js. Native's SDL lifecycle handler is the fourth.
#ifndef EDEN_SEAM_APPLIFECYCLE_WEB_H
#define EDEN_SEAM_APPLIFECYCLE_WEB_H

enum {
    EDEN_BG_SAVED     =  0,  // a world was open and has been written
    EDEN_BG_NO_WORLD  =  1,  // no world open (menu, or between worlds) — nothing to do
    EDEN_BG_BUSY      =  2,  // a world is loading/unloading; saving now would write a partial one
    EDEN_BG_NO_ENGINE = -1   // World::getWorld not up yet (or already torn down)
};

#ifdef __cplusplus
extern "C" {
#endif
int eden_app_will_background(void);
int eden_app_background_save_count(void);
int eden_app_background_last_status(void);
#ifdef __cplusplus
}
#endif

#endif
