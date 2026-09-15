// pthread_sync_web.c — SINGLE-THREADED-BUILD ONLY synchronous pthread_create.
//
// Compiled ONLY when EDEN_THREADED=OFF (see CMakeLists.txt). The threaded build links real
// Emscripten pthreads (-pthread), where this file is absent and the engine's world-load thread
// runs for real.
//
// WHY THIS EXISTS: the engine spawns exactly one thread — the world-load thread
// (Classes/World.mm:340, `pthread_create(&foo,NULL,loadWorldThread,name)`, the only
// pthread_create call in the whole engine, CLAUDE.md convention #4). In a NON-pthread Emscripten
// build there is no thread support, and Emscripten's stub `pthread_create` returns without ever
// running `start_routine`. The engine's loadWorldThread is what sets `doneLoading=2`; if it never
// runs, `World::loadWorld` is stuck forever at `doneLoading==1` and the menu hangs on the
// "Loading World..." bar — the freeze reported when clicking a world in eden-st.html.
//
// FIX: run the start routine synchronously on the calling (main) thread and return success. The
// world-load routine touches no GL and is fire-and-forget (the engine never joins/detaches it),
// so inline execution is behaviorally correct here — the only observable difference is that the
// progress bar jumps straight to done in one frame instead of animating. The CMakeLists.txt
// EDEN_THREADED=OFF branch already documented this as the intended single-thread behavior
// ("load runs synchronously") but never implemented it.
//
// HOW EXPENSIVE IS THAT, ACTUALLY — MEASURED 2026-07-31, and it settles project-audit row 9 (A6).
// That row asserted this "freezes the whole tab … on a slow phone with a large world this reads as
// a crash", rated the fix Opus 5 (high) / M, and gated the entire threaded build (row 36) behind
// fixing it first. None of that was measured. `tools/headless-load-timing.js` measures it, and the
// answer is:
//
//     build-rel:  20-27 ms of contiguous main-thread block, 3 runs      <- what players run
//     build-st:   51-62 ms                                              <- debug, ~2.5x slower
//
// i.e. ONE DROPPED FRAME in release, and ~100-270 ms even at a generous 5-10x mobile penalty. The
// load is bounded by construction — it is always the same 324 columns of the toroidal window (18x18
// at 32 KB each), whatever the size of the save file — so this does not grow with playtime, which
// is what "a large world" assumed. Do not spend an Asyncify/slicing project on this without a NEW
// measurement that contradicts the above; re-run the tool first.
//
// THE ONE PART THAT CAN STILL GET SLOW, and it is not the CPU: on a host that honors HTTP Range,
// the lazy Eden.eden FS node (src/seam/js/eden_default_world.pre.js) issues 18 SYNCHRONOUS XHRs
// during the load itself — 18 full round trips of dead main thread, invisible under node (the
// backend there is fs.readSync) and invisible on localhost. The deployed site does not take that
// path at all (GitHub Pages ignores Range, so it uses the eager whole-file fallback — audit row
// A11), so this is a local-dev and future-host concern rather than a player-facing one. If this
// port is ever deployed somewhere that DOES serve ranges, that is the number to attack, and the
// cheap attack is prefetching/read-ahead in the FS node, not restructuring the engine's load.
//
// This is a user-object-file definition, so it takes precedence over Emscripten libc's archive
// stub at link time (no --allow-multiple-definition needed). If the threaded build ever tried to
// link this file it WOULD collide with real pthreads — hence the CMake gate.
#include <pthread.h>

int pthread_create(pthread_t *thread,
                   const pthread_attr_t *attr,
                   void *(*start_routine)(void *),
                   void *arg) {
    (void)attr;
    if (thread) *thread = (pthread_t)0;
    start_routine(arg);
    return 0;
}
