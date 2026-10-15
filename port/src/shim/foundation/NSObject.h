// ---------------------------------------------------------------------------------------------
// Phase N Stage 1 (WORKING/native-migration-plan-2026-09-04.md): on a target with a real
// Foundation, this shim class does not exist — the system one does, and defining ours alongside
// libobjc's/Foundation's would be a duplicate-class collision with undefined resolution. Files
// that #import this header by relative path (the seam files do, and cannot be redirected with
// -I because quoted includes always resolve against the including file's own directory first)
// therefore get the real Foundation here instead.
//
// EDEN_USE_SYSTEM_FOUNDATION is defined by native/CMakeLists.txt only; read that file's header
// for why the native macOS target cannot use this port's hand-written ObjC runtime (MEASURED:
// clang cannot lower ANY GNU ObjC ABI to Mach-O — it emits selector-name globals in a COMDAT,
// which the MachO backend does not support. gnustep-1.9, gnustep-1.8, gcc and objfw all fail
// identically).
// ---------------------------------------------------------------------------------------------
#if defined(EDEN_USE_SYSTEM_FOUNDATION)
#import <Foundation/Foundation.h>
#else
// NSObject.h — D3a shim root class.
//
// Real Objective-C (@interface/@implementation), not a C++ mimic: the engine's .mm files use
// genuine bracket-message-send syntax (`[obj method:arg]`, `@"literals"`, `@selector()`)
// throughout, so this shim only works compiled under a real ObjC frontend + runtime — per
// plan decision D3(a), clang's GNU objc runtime (`-fobjc-runtime=gnustep-2.0`, see
// CMakeLists.txt) providing `id`/`SEL`/`Class`/message dispatch, with THIS file supplying the
// one thing a bare runtime doesn't: an `NSObject` root class and the handful of Foundation
// classes the engine actually calls (see foundation-usage.md).
//
// P0.1 (web-port-plan.md) is the still-open spike that proves this links; nothing below has
// been compiled yet (no emcc on this machine — see archive/PORT-STATUS-2026-08-13.md).
//
// Manual retain/release throughout (CLAUDE.md convention #6 — this engine predates ARC and
// the port keeps it that way rather than perturb every engine file). Build with
// `-fno-objc-arc`.
#ifndef EDEN_SHIM_NSOBJECT_H
#define EDEN_SHIM_NSOBJECT_H

#import <objc/objc.h>
#import <objc/runtime.h>

// BOOL/YES/NO/nil come from <objc/objc.h> under the GNU runtime; the engine's own headers
// (e.g. Classes/Util.h's `BOOL dead;`) assume these are already in scope exactly as they
// would be on iOS.

@class NSString;

// NSCoding/NSCopying are referenced by a couple of engine @interfaces (e.g. VKeyboard's PText,
// now seam-excluded) — declared here as empty formal protocols so any straggler `<NSCoding>`
// conformance elsewhere still parses. Not implemented (no archiving in this port).
@protocol NSCoding
@end
@protocol NSCopying
@end

// NSFastEnumeration backs `for (UITouch *t in touches)` in Input.mm (real Foundation declares
// this in NSEnumerator.h). NSArray.mm:79 already implements -countByEnumeratingWithState:
// against exactly this signature; the protocol + state struct were never declared, which is
// why NSSet/NSArray's `<NSFastEnumeration>` conformance failed to parse.
typedef struct {
    unsigned long state;
    id *itemsPtr;
    unsigned long *mutationsPtr;
    unsigned long extra[5];
} NSFastEnumerationState;

@protocol NSFastEnumeration
- (NSUInteger)countByEnumeratingWithState:(NSFastEnumerationState *)state objects:(id *)stackbuf count:(NSUInteger)len;
@end

// Real Foundation declares a formal `NSObject` PROTOCOL alongside the class of the same name
// (that is not a typo — Foundation genuinely has both, and `@interface NSObject <NSObject>` is
// the class adopting the protocol). The shim's @interface below already adopted it, but the
// protocol itself was never declared — 50 "cannot find protocol declaration for 'NSObject'"
// errors in the fifth real build. Only the members the shim actually implements are listed.
@protocol NSObject
- (id)retain;
- (oneway void)release;
- (id)autorelease;
- (NSUInteger)retainCount;
- (Class)class;
- (BOOL)isKindOfClass:(Class)cls;
- (BOOL)isMemberOfClass:(Class)cls;
- (BOOL)respondsToSelector:(SEL)sel;
- (BOOL)isEqual:(id)other;
- (NSUInteger)hash;
@end

// *** ZERO IVARS BEYOND `isa` — LOAD-BEARING, DO NOT ADD ANY. ***
// `@"..."` literals are emitted by clang as static instances of the constant-string class
// with a hard-coded layout of exactly `{ Class isa; const char *cString; unsigned length; }`
// (see NSConstantString in NSString.h). That class descends from NSObject, so ANY ivar
// declared here shifts the layout clang already baked into 743 static literal instances
// across the engine — silently corrupting every one of them. The retain count therefore
// lives OUTSIDE the object, in a side table (NSObject.mm), exactly as GNUstep keeps it out
// of the ivar area. This is the resolution of the D3 constant-string risk recorded in
// web-port-plan.md ("D3 refinement"); re-read it before touching this @interface.
@interface NSObject <NSObject> {
    // (no ivars — see the comment above)
}

+ (id)alloc;
+ (id)new;
+ (Class)class;
+ (Class)superclass;
- (id)init;
- (id)retain;
- (oneway void)release;
- (id)autorelease;
- (NSUInteger)retainCount;
- (void)dealloc;
- (Class)class;
- (BOOL)isKindOfClass:(Class)cls;
- (BOOL)isMemberOfClass:(Class)cls;
- (BOOL)respondsToSelector:(SEL)sel;
- (BOOL)isEqual:(id)other;
- (NSUInteger)hash;
- (NSString *)description;

@end

// NSCoder — nib/archive unarchiving. Forward-declared but never implemented: the only reference
// is src/seam/EAGLView_web.mm's -initWithCoder: override, which exists solely to give the class a
// designated initializer matching the original's. The web build constructs the view directly
// rather than unarchiving it from a nib, so nothing ever passes a real coder. See that file's
// TODO P2.
@class NSCoder;

#endif
#endif  // EDEN_USE_SYSTEM_FOUNDATION
