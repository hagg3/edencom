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
// NSThread.h — D3a shim, near-moot (see foundation-usage.md "NSThread": the only real
// detachNewThreadSelector: call sites are in Appirater.mm, which this port strips entirely;
// the world-load thread uses raw pthread_create, not NSThread — CLAUDE.md convention #4).
// Declared for completeness / in case a straggler call site turns up.
#ifndef EDEN_SHIM_NSTHREAD_H
#define EDEN_SHIM_NSTHREAD_H

#import "NSObject.h"

@interface NSThread : NSObject
+ (void)detachNewThreadSelector:(SEL)sel toTarget:(id)target withObject:(id)arg;
+ (BOOL)isMainThread;
+ (void)sleepForTimeInterval:(double)seconds;
@end

#endif
#endif  // EDEN_USE_SYSTEM_FOUNDATION
