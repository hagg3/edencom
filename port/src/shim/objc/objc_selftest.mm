// objc_selftest.mm — audit row 21/I6: headless coverage for objc_runtime.cpp's message dispatch.
//
// objc_runtime.cpp itself is plain C++ (it IMPLEMENTS the dispatch machinery); it has no
// @interface/@implementation to exercise directly. This file is real Objective-C, compiled by
// the same clang frontend + `-fobjc-runtime=gnustep-1.9` flags as the engine (see CMakeLists.txt),
// so every message send below goes through the actual runtime a regression would break — not a
// mock of it. Gated EDEN_DIAGNOSTICS (CMakeLists.txt), same as DebugState_web.mm/DevConsole_web.mm:
// a shipped build has no reason to carry a self-test.
//
// Exercises exactly the three things objc_abi.h's header comment names as "where a from-memory
// implementation would have been wrong" (see objc_runtime.cpp's own header): slot-based dispatch
// (including an override reaching the SUBCLASS's slot, not the superclass's), negative
// instance_size + superclass-relative ivar offsets (a subclass ivar read/written correctly
// alongside an inherited one), and category method merging. Also covers `super` dispatch, since
// Classes/*.mm leans on it constantly and a broken super_class-as-name-string resolution would be
// exactly the kind of silent, ABI-shaped bug this row worries about.
//
// Phase N Stage 3.3: this file is no longer web-only. Linux and Windows compile the SAME
// objc_runtime.cpp this exercises (macOS does not — it uses Apple's libobjc, which needs no proof
// from us), and they are the first targets to run it at 64-bit widths, so the exports are
// EDEN_EXPORT rather than EMSCRIPTEN_KEEPALIVE and the classes below now include the ivar shapes
// whose layout ONLY differs off wasm32.
#import "../foundation/NSObject.h"
#import "../foundation/NSString.h"
#include "../foundation/platform_shims.h"
#include <cstdio>
#include <cstring>

// --- Base class + subclass override: slot-based dispatch, not IMP-based (objc_abi.h note 1) ---
@interface EdenSelfTestBase : NSObject {
@public
    int baseIvar;
}
- (int)value;
- (int)baseOnly;
@end

@implementation EdenSelfTestBase
- (id)init {
    self = [super init];
    if (self) baseIvar = 10;
    return self;
}
- (int)value { return baseIvar; }
- (int)baseOnly { return 111; }
@end

// Negative instance_size + superclass-relative ivar offsets (objc_abi.h note 2): subIvar must
// land at its own offset without corrupting or reading baseIvar, and -value's OVERRIDE must be
// what a real message send resolves to (subclass's method table wins over the superclass's).
@interface EdenSelfTestSub : EdenSelfTestBase {
@public
    int subIvar;
}
- (int)value;
- (int)subOnly;
- (int)baseValueViaSuper;
@end

@implementation EdenSelfTestSub
- (id)init {
    self = [super init];
    if (self) subIvar = 20;
    return self;
}
- (int)value { return subIvar; }             // override: must win over EdenSelfTestBase's -value
- (int)subOnly { return 222; }
// `super` dispatch (objc_abi.h note 3: super_class arrives as a name string) — must reach
// EdenSelfTestBase's -value, not re-enter this class's own override (infinite recursion) and not
// silently resolve to nothing.
- (int)baseValueViaSuper { return [super value]; }
@end

// Category merging (objc_runtime.cpp's header: "category merging is implemented anyway since it
// is a dozen lines" — the only two real call sites, FileUpload/Appirater, are seam-excluded, so
// nothing else in the built engine exercises this path at all).
@interface EdenSelfTestBase (EdenSelfTestCategory)
- (int)categoryOnly;
@end

@implementation EdenSelfTestBase (EdenSelfTestCategory)
- (int)categoryOnly { return 333; }
@end

// --- objc_abi.h note (2) at 64-bit widths (Phase N Stage 3.3) ---------------------------------
// The classes above are the NO-BIAS case: EdenSelfTestBase's superclass chain reaches NSObject,
// but EdenSelfTestBase itself carries an ivar, so EdenSelfTestSub's emitted first-ivar offset is
// an ordinary 0 and resolveClass()'s `cppEmptyBaseBias` never fires. That means they cannot
// detect a bias-handling regression at all — they pass whether the correction is applied or not.
//
// These two do. `EdenSelfTestEmptyMid` has zero own ivars and descends from this port's zero-ivar
// NSObject, so its subclass is the "entire ancestor chain is empty" shape that clang biases by
// exactly one byte, marked with the literal `-1` first-ivar sentinel. Stage 3.3 re-measured that
// on LP64 and LLP64 and it is unchanged — but it is a SILENT failure if a toolchain ever changes
// it, so it wants a test and not just a comment. The mixed int/double/pointer ivars matter: a
// word-sized bias (the natural wrong guess) would misplace `emptyDouble` on a 64-bit target while
// leaving the `int` looking plausible.
@interface EdenSelfTestEmptyMid : NSObject
- (int)midOnly;
@end

@implementation EdenSelfTestEmptyMid
- (int)midOnly { return 444; }
@end

@interface EdenSelfTestEmptyKid : EdenSelfTestEmptyMid {
@public
    int          emptyInt;
    double       emptyDouble;
    const char  *emptyPtr;
}
@end

@implementation EdenSelfTestEmptyKid
- (id)init {
    self = [super init];
    if (self) { emptyInt = 55; emptyDouble = 66.5; emptyPtr = "77"; }
    return self;
}
@end

extern "C" {

#ifdef EDEN_DIAGNOSTICS
EDEN_EXPORT
int eden_objc_selftest_run(void) {
    EdenSelfTestSub *sub = [[EdenSelfTestSub alloc] init];
    if (!sub) {
        std::fprintf(stderr, "[eden-objc-selftest] FAIL: [[EdenSelfTestSub alloc] init] returned nil\n");
        return 0;
    }

    int ok = 1;

    // Slot-based dispatch: the override must win.
    if ([sub value] != 20) {
        std::fprintf(stderr, "[eden-objc-selftest] FAIL: [sub value] = %d, want 20 "
                              "(subclass override not dispatched)\n", [sub value]);
        ok = 0;
    }
    // Ivar layout: both the inherited and the subclass's own ivar must read back correctly and
    // not alias each other.
    if (sub->baseIvar != 10 || sub->subIvar != 20) {
        std::fprintf(stderr, "[eden-objc-selftest] FAIL: baseIvar=%d (want 10) subIvar=%d "
                              "(want 20) — superclass-relative ivar offset is wrong\n",
                      sub->baseIvar, sub->subIvar);
        ok = 0;
    }
    // A message only the base class implements must still resolve through the subclass's
    // (shorter, since it doesn't override it) method table.
    if ([sub baseOnly] != 111) {
        std::fprintf(stderr, "[eden-objc-selftest] FAIL: [sub baseOnly] = %d, want 111\n", [sub baseOnly]);
        ok = 0;
    }
    if ([sub subOnly] != 222) {
        std::fprintf(stderr, "[eden-objc-selftest] FAIL: [sub subOnly] = %d, want 222\n", [sub subOnly]);
        ok = 0;
    }
    // `super` dispatch must reach the SUPERCLASS's -value (10), not recurse into the override (20).
    if ([sub baseValueViaSuper] != 10) {
        std::fprintf(stderr, "[eden-objc-selftest] FAIL: [sub baseValueViaSuper] = %d, want 10 "
                              "(super dispatch resolved to the wrong slot)\n", [sub baseValueViaSuper]);
        ok = 0;
    }
    // Category merging.
    if ([sub categoryOnly] != 333) {
        std::fprintf(stderr, "[eden-objc-selftest] FAIL: [sub categoryOnly] = %d, want 333 "
                              "(category method not merged onto the base class)\n", [sub categoryOnly]);
        ok = 0;
    }

    [sub release];

    // --- the empty-base-class bias (objc_abi.h note 2), which nothing above can catch --------
    EdenSelfTestEmptyKid *kid = [[EdenSelfTestEmptyKid alloc] init];
    if (!kid) {
        std::fprintf(stderr, "[eden-objc-selftest] FAIL: [[EdenSelfTestEmptyKid alloc] init] returned nil\n");
        return 0;
    }
    if (kid->emptyInt != 55 || kid->emptyDouble != 66.5 ||
        !kid->emptyPtr || kid->emptyPtr[0] != '7') {
        std::fprintf(stderr, "[eden-objc-selftest] FAIL: emptyInt=%d (want 55) emptyDouble=%f "
                              "(want 66.5) emptyPtr=%s (want 77) — the objective-c++ empty-base "
                              "ivar bias is being mis-corrected\n",
                      kid->emptyInt, kid->emptyDouble, kid->emptyPtr ? kid->emptyPtr : "(null)");
        ok = 0;
    }
    if ([kid midOnly] != 444) {
        std::fprintf(stderr, "[eden-objc-selftest] FAIL: [kid midOnly] = %d, want 444 "
                              "(zero-ivar intermediate class not in the dispatch chain)\n", [kid midOnly]);
        ok = 0;
    }
    [kid release];

    // The @"literal" path is the highest-consequence instance of the same bias — NSConstantString
    // is exactly the "all-empty ancestor chain, then ivars" shape, and if its offsets are wrong
    // every one of the engine's ~740 string literals reads its bytes from the wrong place. A
    // literal that round-trips through -UTF8String proves the layout end to end.
    NSString *literal = @"eden-abi-probe";
    const char *round = [literal UTF8String];
    if (!round || std::strcmp(round, "eden-abi-probe") != 0) {
        std::fprintf(stderr, "[eden-objc-selftest] FAIL: @\"eden-abi-probe\" -UTF8String = %s "
                              "— NSConstantString's ivar offsets are wrong\n", round ? round : "(null)");
        ok = 0;
    }
    if ([literal length] != 14) {
        std::fprintf(stderr, "[eden-objc-selftest] FAIL: @\"eden-abi-probe\" -length = %lu, want 14\n",
                      (unsigned long)[literal length]);
        ok = 0;
    }

    if (ok) {
        std::fprintf(stderr, "[eden-objc-selftest] PASS: slot dispatch, ivar layout, super, "
                              "category merging, the empty-base ivar bias and the @\"literal\" "
                              "layout all resolved correctly\n");
    }
    return ok;
}
#endif

}  // extern "C"
