#import "NSThread.h"
#include <unistd.h>

@implementation NSThread

+ (void)detachNewThreadSelector:(SEL)sel toTarget:(id)target withObject:(id)arg {
    // TODO P1 (if a real call site ever appears — none does today, see header): under
    // EDEN_THREADED this should become a real pthread_create; single-thread fallback would
    // call [target performSelector:sel withObject:arg] synchronously. Left unimplemented
    // (no-op) since nothing currently reaches it once Appirater.mm is stripped.
    (void)sel; (void)target; (void)arg;
}

+ (BOOL)isMainThread {
    // TODO P1: under EDEN_THREADED (PROXY_TO_PTHREAD), the "main thread" is the worker
    // running the game loop, not the browser's actual main thread — Emscripten's own
    // emscripten_is_main_runtime_thread() is probably the right primitive once building
    // under emcc. Placeholder always returns YES (matches this shim's current
    // single-thread-for-Foundation assumption, see NSAutoreleasePool.mm's pool-stack note).
    return YES;
}

+ (void)sleepForTimeInterval:(double)seconds {
    usleep((useconds_t)(seconds * 1000000.0));
}

@end
