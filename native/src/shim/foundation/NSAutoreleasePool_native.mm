// NSAutoreleasePool_native.mm — the C push/drain pair from
// web/src/shim/foundation/NSAutoreleasePool.h, over Apple's real runtime (Phase N Stage 1,
// WORKING/native-migration-plan-2026-09-04.md).
//
// The @interface half of that header is web-only (real Foundation has NSAutoreleasePool); the C
// pair is not, because both entry points call it once per frame and both are plain C++ — see
// EdenViewController_native.cpp / EdenViewController_web.cpp and audit row A2, which is about the
// real bug of frame-scoped autoreleased objects piling up with no pool to drain them.
//
// objc_autoreleasePoolPush/Pop rather than +[NSAutoreleasePool alloc]/-drain: it is the same
// mechanism the modern @autoreleasepool block compiles to, it is valid under ARC and non-ARC
// alike, and it returns an opaque token that maps exactly onto this API's `void*` — where an
// NSAutoreleasePool* would have to be released rather than popped, and getting that wrong is a
// leak of everything the frame autoreleased.
#include <objc/objc.h>

extern "C" {
void* objc_autoreleasePoolPush(void);
void  objc_autoreleasePoolPop(void* context);
}

extern "C" void* eden_autoreleasepool_push(void) {
    return objc_autoreleasePoolPush();
}

extern "C" void eden_autoreleasepool_drain(void* pool) {
    // A null token would abort inside libobjc. The web shim tolerates a null pool (it is a
    // no-op there), so match that rather than introduce a platform-specific crash.
    if (pool) objc_autoreleasePoolPop(pool);
}
