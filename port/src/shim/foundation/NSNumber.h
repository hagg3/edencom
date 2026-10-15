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
// NSNumber.h — D3a shim, tagged-union backed. Low-traffic (17 mentions, see
// foundation-usage.md) but implemented fully since it's trivial.
#ifndef EDEN_SHIM_NSNUMBER_H
#define EDEN_SHIM_NSNUMBER_H

#import "NSObject.h"

@interface NSNumber : NSObject {
@public
    enum { kInt, kFloat, kDouble, kBool, kUInt } _kind;
    union { int i; float f; double d; BOOL b; unsigned int u; } _v;
}

+ (NSNumber *)numberWithInt:(int)v;
+ (NSNumber *)numberWithFloat:(float)v;
+ (NSNumber *)numberWithDouble:(double)v;
+ (NSNumber *)numberWithBool:(BOOL)v;
+ (NSNumber *)numberWithUnsignedInt:(unsigned int)v;

- (int)intValue;
- (float)floatValue;
- (double)doubleValue;
- (BOOL)boolValue;
- (unsigned int)unsignedIntValue;

@end

#endif
#endif  // EDEN_USE_SYSTEM_FOUNDATION
