// AutoreleasePool_native.cpp — the C push/drain pair EdenViewController_native.cpp brackets every
// frame with (audit row A2: frame-scoped autoreleased objects piling up with no pool to drain them).
// Was port/src/shim/foundation/NSAutoreleasePool_native.mm (Apple) + the shim's NSAutoreleasePool.mm
// (elsewhere) until G.3; it never needed the ObjC frontend, only libobjc's C entry points.
//
// APPLE: the engine is plain C++ since G.2b, but the Apple seam ObjC that runs inside a frame
// (AppleNet's NSURLSession, AppleAudio, IOSKeyboard) still autoreleases (de-ObjC plan D12).
// objc_autoreleasePoolPush/Pop rather than +[NSAutoreleasePool alloc]/-drain: it is the same
// mechanism @autoreleasepool compiles to, valid under ARC and non-ARC alike, and its opaque token maps
// exactly onto this API's `void*`.
// ELSEWHERE: there is no ObjC at all (G.3), so both calls are no-ops.

#if defined(__APPLE__)
extern "C" {
void* objc_autoreleasePoolPush(void);
void  objc_autoreleasePoolPop(void* context);
}

extern "C" void* eden_autoreleasepool_push(void) {
    return objc_autoreleasePoolPush();
}

extern "C" void eden_autoreleasepool_drain(void* pool) {
    // A null token would abort inside libobjc; tolerate it, as the non-Apple no-op does.
    if (pool) objc_autoreleasePoolPop(pool);
}
#else
extern "C" void* eden_autoreleasepool_push(void) { return 0; }
extern "C" void eden_autoreleasepool_drain(void* pool) { (void)pool; }
#endif
