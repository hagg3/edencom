// NSAutoreleasePool.mm — a real thread-local pool stack over std::vector<id>. Draining
// releases every collected object once, in reverse-insertion order (matches real Foundation's
// LIFO-ish draining closely enough — the engine never depends on exact drain ordering between
// distinct objects, only on "eventually released after the pool that captured it dies").
#import "NSAutoreleasePool.h"
#include "platform_shims.h"   // EDEN_BUILD_HAS_THREADS
#include <vector>

// THE POOL STACK IS PER-THREAD (audit row 36/C1, pass 63) — as it is in real Foundation, and as
// the file header's old TODO said it would have to become "if that invariant ever changes."
// It changed. The threaded build makes Classes/World.mm's world-load pthread real, and that
// thread's FIRST statement is `[[NSAutoreleasePool alloc] init]` (Classes/World.mm:317,
// loadWorldThread) — so two threads push and pop this stack concurrently. A single shared
// std::vector would corrupt on the concurrent push_back, and worse, `+currentPool` on the load
// thread would hand back the MAIN thread's frame pool, so every NSString the loader autoreleased
// would be drained by the render thread's next frame boundary — a use-after-free that would
// present as terrain corruption, not as a threading bug.
//
// `thread_local` in any build that has threads, so the single-threaded one's codegen is unchanged
// (a plain static, no TLS indirection on -autorelease's path). Each thread gets its own lazily
// created fallback root pool via +currentPool below, which is the correct behaviour anyway:
// the load thread's pool must not be the main thread's.
//
// The condition was `#if defined(__EMSCRIPTEN_PTHREADS__)` until Phase N Stage 3.2, which is false
// on native even though native runs both of the threads described above. The use-after-free this
// paragraph predicts is exactly what happened on the Linux leg — a SIGSEGV in dispatch, reached
// from -[NSAutoreleasePool release]. See EDEN_BUILD_HAS_THREADS in platform_shims.h.
#if EDEN_BUILD_HAS_THREADS
#define EDEN_POOL_TLS thread_local
#else
#define EDEN_POOL_TLS
#endif
static EDEN_POOL_TLS std::vector<NSAutoreleasePool *> g_poolStack;

// NON-POD IVARS ARE NOT SAFE IN THIS PORT — see the long note in NSUserDefaults.mm for the
// measurement and the mechanism. Short version: class_createInstance() is `calloc`, and the
// hand-written runtime has no `.cxx_construct`/`.cxx_destruct`, so a C++ ivar is neither
// constructed nor destroyed. An all-zero `std::vector` at least *reads* as a valid empty vector,
// so this one never crashed the way NSUserDefaults' `std::unordered_map` did — but the missing
// destructor meant every pool's heap buffer was leaked at `object_dispose`'s bare `free()`.
// -release's `_objects.clear()` releases the contents and drops size to 0; it does not give the
// capacity back. Since audit row A2 (pass 53) there is one pool per frame, so that was a leak at
// display refresh rate, in the very code added to stop a leak. Heap pointer + explicit delete.
@implementation NSAutoreleasePool {
    std::vector<id> *_objects;   // calloc'd to null; allocated by -init, deleted by -release
}

+ (NSAutoreleasePool *)currentPool {
    if (g_poolStack.empty()) {
        // No pool active — matches real Foundation's "leaked, logged" behavior loosely; here
        // we just lazily create a root pool so -autorelease never crashes during early
        // (pre-main-pool) engine construction, e.g. static initializers.
        [[[NSAutoreleasePool alloc] init] autorelease]; // note: this pool leaks by design,
                                                          // it's the fallback root.
    }
    return g_poolStack.back();
}

- (id)init {
    self = [super init];
    if (self) {
        _objects = new std::vector<id>();
        g_poolStack.push_back(self);
    }
    return self;
}

- (void)addObject:(id)obj {
    _objects->push_back(obj);
}

- (void)drain {
    [self release];
}

- (oneway void)release {
    if (_objects) {
        for (auto it = _objects->rbegin(); it != _objects->rend(); ++it) {
            [*it release];
        }
        _objects->clear();
    }
    if (!g_poolStack.empty() && g_poolStack.back() == self) {
        g_poolStack.pop_back();
    }
    [super release];   // -> NSObject -dealloc -> this class's -dealloc when the count hits 0
}

// The buffer, as opposed to its contents, is freed here rather than in -release: NSObject's
// -release only deallocs at a zero count, and object_dispose() is a bare free() that will not
// run any C++ destructor for us (see the ivar note above).
- (void)dealloc {
    delete _objects;
    _objects = nullptr;
    [super dealloc];
}

@end

// See NSAutoreleasePool.h: C-linkage wrappers for plain-C++ callers (EdenViewController_web.cpp's
// per-frame pool, audit row A2). Manual retain/release, no ARC/bridge casts needed (CLAUDE.md #6).
void *eden_autoreleasepool_push(void) {
    return (void *)[[NSAutoreleasePool alloc] init];
}

void eden_autoreleasepool_drain(void *pool) {
    [(NSAutoreleasePool *)pool drain];
}

// --- the RUNTIME's entry points, which `@autoreleasepool { ... }` lowers to ------------------
// Phase N Stage 3.2. The two wrappers above are this port's own C API and were enough while the
// only callers were plain-C++ seam files calling them by name. `@autoreleasepool` is ordinary
// Objective-C syntax, though, and clang lowers it to `objc_autoreleasePoolPush` /
// `objc_autoreleasePoolPop` — runtime entry points, not Foundation ones. A runtime that does not
// provide them cannot compile that syntax, which showed up as two undefined symbols from
// native/src/entry/eden_main_native.cpp on the Linux leg. It never appeared on web because
// nothing there had written `@autoreleasepool`, and never on macOS because Apple's libobjc has
// them; that is not a reason for this runtime to be missing them.
//
// They live here rather than in objc_runtime.cpp for one mechanical reason: that file is plain
// C++ and cannot send a message, and a pool is an object. The `void *` is the pool itself, which
// is exactly the contract Apple's pair has (the "context" is opaque to the caller).
// extern "C" explicitly: unlike the two wrappers above, these are not declared in
// NSAutoreleasePool.h (nothing in this port calls them by name — the compiler does), so there is
// no earlier declaration to inherit C linkage from. Without it they mangle and stay undefined.
extern "C" void *objc_autoreleasePoolPush(void) {
    return (void *)[[NSAutoreleasePool alloc] init];
}

extern "C" void objc_autoreleasePoolPop(void *pool) {
    // Nested pools drain in reverse order, which -drain already enforces through currentPool.
    if (pool) [(NSAutoreleasePool *)pool drain];
}
