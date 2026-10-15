// NSObject.mm — minimal root-class implementation. Reference-counted retain/release;
// -autorelease defers to the current NSAutoreleasePool (NSAutoreleasePool.h/.mm), matching
// real Foundation/Cocoa semantics closely enough for this engine's usage (it never relies on
// zeroing weak references or KVO, per a scan of Classes/*.mm — plain retain/release/autorelease
// only, CLAUDE.md convention #6).
#import "NSObject.h"
#import "NSAutoreleasePool.h"
#import "NSString.h"
#include <cstdio>
#include <cstdlib>
// uintptr_t (the retain-count side table keys objects by address). Emscripten's and Apple's
// sysroots both leak <stdint.h> in through one of the headers below; glibc's do not, so on the
// Linux leg this file did not compile at all. Phase N Stage 3.2.
#include <cstdint>
#include <unordered_map>
#include "platform_shims.h"   // EDEN_BUILD_HAS_THREADS
#if EDEN_BUILD_HAS_THREADS
#include <mutex>
#endif

// --- Retain counts live here, NOT in an ivar -------------------------------------------
// See the layout comment on @interface NSObject (NSObject.h): NSObject must declare zero
// ivars so clang's statically-emitted `@"..."` instances keep the layout it hard-codes.
// A side table keyed by object address is the simplest storage that satisfies that.
//
// Absent entry == count of 1. This makes the table cheap (only objects that are actually
// retained past their initial reference ever get an entry) and, critically, makes constant
// strings free: they are never inserted, always report 1, and can never reach 0.
//
// THREADING (audit row 36/C1, pass 63) — this table is the ONE piece of the Foundation shim that
// genuinely needs a lock, and the only one that gets one. Everywhere else the threaded build
// reaches for per-thread state instead (the ObjC dispatch caches in objc_runtime.cpp, the pool
// stack in NSAutoreleasePool.mm); this table cannot go that way, because retain and release for
// the SAME object may legitimately happen on different threads — Classes/World.mm hands its
// `NSString *name` from the main thread to loadWorldThread, which passes it on into
// Terrain::loadTerrain. A per-thread count would let each side independently drive it to zero.
//
// A std::mutex is affordable here in a way it would NOT have been on the message-dispatch path:
// every operation below is already an unordered_map hash + probe, so the lock is a small addition
// to an existing cost rather than a new cost on a bare pointer compare. It is also not the engine's
// hot loop — retain/release traffic is Foundation-object churn (strings, file handles, pool
// contents), not per-vertex or per-block work.
//
// Compiled out entirely without -pthread: `EdenRetainLock` is an empty struct there, so the
// single-threaded build keeps exactly the codegen it had.
//
// Still open (deliberately, and NOT needed for the world-load thread): if off-thread MESHING ever
// retains/releases Foundation objects at frame rate, revisit this — an atomic count in an object
// header word beats a locked side table at that volume. The reason the side table exists at all
// is NSObject's zero-ivar layout constraint (see above), so that change is a real design change,
// not a tweak.
namespace {
std::unordered_map<const void *, int> &edenRetainTable() {
    static std::unordered_map<const void *, int> table;
    return table;
}

// Phase N Stage 3.2: the condition was `defined(__EMSCRIPTEN_PTHREADS__)`, which is false on every
// native build — including the ones that retain and release objects on the world-load thread while
// the main thread renders. An unlocked shared table under real concurrency corrupts silently.
// See EDEN_BUILD_HAS_THREADS in platform_shims.h.
#if EDEN_BUILD_HAS_THREADS
std::mutex &edenRetainTableMutex() {
    static std::mutex m;
    return m;
}
struct EdenRetainLock {
    std::lock_guard<std::mutex> guard;
    EdenRetainLock() : guard(edenRetainTableMutex()) {}
};
#else
struct EdenRetainLock {};
#endif
} // namespace

// Constant strings are immortal: clang emits them as static data, so they were never
// malloc'd and must never be freed. NSConstantString overrides retain/release to no-ops
// (NSString.mm), so they never reach this table at all.

@implementation NSObject

+ (id)alloc {
    // Count of 1 is implicit (absent from the table) — nothing to record here.
    return class_createInstance(self, 0);
}

+ (id)new {
    return [[self alloc] init];
}

// Explicit class-side overrides — NOT redundant with -class/-isKindOfClass: below. Without
// these, `[SomeClass class]`/`+isKindOfClass:` fall through the GNU root-metaclass's
// super_class link to the ROOT CLASS's own INSTANCE method table (the standard trick that lets
// one root class supply both -foo and, via that fallback, +foo). That fallback reuses
// `-class`'s body (`object_getClass(self)`, i.e. `self->isa`) with `self` bound to the CLASS
// object being asked about — which computes its METACLASS, not the class itself. Every
// `[X class]` in the port was silently returning the wrong (metaclass) pointer this way, e.g.
// making `[foo isKindOfClass:[NSString class]]` compare against NSString's metaclass and
// false-negative for every real NSString instance (found via Texture2D_web.mm's `ipad~`
// retina-asset probe: `[NSString stringWithFormat:@"ipad~%@", path]`'s `%@` substitution calls
// `-isKindOfClass:[NSString class]`, always missed, so it fell back to `-description`, which
// NSConstantString/EdenConcreteString don't implement, producing the literal string
// "(null description)" as part of the filename and silently losing every retina asset).
+ (Class)class {
    return self;
}

+ (Class)superclass {
    return class_getSuperclass(self);
}

- (id)init {
    return self;
}

- (id)retain {
    EdenRetainLock lock;
    std::unordered_map<const void *, int> &table = edenRetainTable();
    std::unordered_map<const void *, int>::iterator it = table.find(self);
    if (it == table.end()) {
        table[self] = 2; // was the implicit 1
    } else {
        it->second++;
    }
    return self;
}

- (oneway void)release {
    // The lock is scoped to the table access ALONE, and -dealloc runs outside it. -dealloc takes
    // the same (non-recursive) mutex to erase its own entry, so calling it from inside this scope
    // would self-deadlock the moment the threaded build is enabled — and would do so only on the
    // path where an object actually dies, i.e. rarely enough to look like a random hang.
    bool shouldDealloc = false;
    {
        EdenRetainLock lock;
        std::unordered_map<const void *, int> &table = edenRetainTable();
        std::unordered_map<const void *, int>::iterator it = table.find(self);
        if (it == table.end()) {
            shouldDealloc = true; // implicit count of 1 → drops to 0
        } else if (--it->second <= 1) {
            table.erase(it); // back down to the implicit 1
        }
    }
    if (shouldDealloc) [self dealloc];
}

- (id)autorelease {
    [[NSAutoreleasePool currentPool] addObject:self];
    return self;
}

- (NSUInteger)retainCount {
    EdenRetainLock lock;
    std::unordered_map<const void *, int> &table = edenRetainTable();
    std::unordered_map<const void *, int>::iterator it = table.find(self);
    return it == table.end() ? 1 : (NSUInteger)it->second;
}

- (void)dealloc {
    {
        EdenRetainLock lock;
        edenRetainTable().erase(self); // never leave a stale entry for a recycled address
    }
    object_dispose(self); // outside the lock: it frees, and must not run under the table mutex
}

- (Class)class {
    return object_getClass(self);
}

- (BOOL)isKindOfClass:(Class)cls {
    Class c = object_getClass(self);
    while (c) {
        if (c == cls) return YES;
        c = class_getSuperclass(c);
    }
    return NO;
}

- (BOOL)isMemberOfClass:(Class)cls {
    return object_getClass(self) == cls;
}

- (BOOL)respondsToSelector:(SEL)sel {
    return class_respondsToSelector(object_getClass(self), sel);
}

- (BOOL)isEqual:(id)other {
    return self == other;
}

- (NSUInteger)hash {
    return (NSUInteger)(uintptr_t)self;
}

- (NSString *)description {
    // TODO P1: format as "<ClassName: 0xADDR>" once NSString's stringWithFormat: is wired to
    // this file's build unit (avoids a header-order dependency for now).
    return nil;
}

@end
