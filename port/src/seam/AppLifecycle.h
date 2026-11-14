// AppLifecycle.h — status codes for eden_app_will_background(). See AppLifecycle.cpp.
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
    EDEN_BG_FAILED    =  3,  // QA.5: the save failed (stderr says why); the last commit is intact
    EDEN_BG_NO_ENGINE = -1   // World::getWorld not up yet (or already torn down)
};

#ifdef __cplusplus
extern "C" {
#endif
int eden_app_will_background(void);
int eden_app_background_save_count(void);
int eden_app_background_last_status(void);
// QA.18 (A2-09): SDL_EVENT_LOW_MEMORY. Same save policy as eden_app_will_background(), but the loop
// keeps running; also frees the audio clip cache. Returns the save status.
int eden_app_low_memory(void);
int eden_app_low_memory_count(void);
// QA.14 (A2-03): 1 while World::loadWorld's state machine is between "load started" and "world in"
// (the load pthread may be running). The input tick skips its player writes then.
int eden_world_loading(void);
// QA.14: wait out a load in flight. The quit path calls it so main() never returns under the load thread.
void eden_world_load_join(void);
#ifdef __cplusplus
}
#endif

#endif
