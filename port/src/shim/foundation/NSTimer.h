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
// NSTimer.h — D3a shim. Header-only-with-trivial-impl; per foundation-usage.md the real user
// (EdenViewController.mm's CADisplayLink-fallback path) is seam-excluded and replaced by
// requestAnimationFrame in src/entry/eden_main.cpp (Stage P2). No non-seam engine file uses
// NSTimer as of this pass — kept for completeness only.
#ifndef EDEN_SHIM_NSTIMER_H
#define EDEN_SHIM_NSTIMER_H

#import "NSObject.h"

@interface NSTimer : NSObject
+ (NSTimer *)scheduledTimerWithTimeInterval:(double)interval target:(id)target
                                     selector:(SEL)sel userInfo:(id)info repeats:(BOOL)repeats;
- (void)invalidate;
@end

#endif
#endif  // EDEN_USE_SYSTEM_FOUNDATION
