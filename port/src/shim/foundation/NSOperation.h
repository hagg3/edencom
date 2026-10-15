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
// NSOperation / NSOperationQueue — declaration-level shim.
//
// One user in this tree: Classes/CocosDenshion.h's `@interface CDAsynchBufferLoader : NSOperation`,
// used by -loadBuffersAsynchronously: to decode sound effects off the main thread. CocosDenshion.h
// is pulled in by Classes/Resources.mm (an ordinary engine file), so the superclass must exist for
// the engine to compile even though the audio implementation waits for Stage P5.
//
// -main is the method a subclass overrides and -start is what a queue calls; both are declared so
// CDAsynchBufferLoader's override compiles. The queue's -addOperation: runs the operation
// SYNCHRONOUSLY here rather than on another thread — see NSOperation.mm for why that is the right
// stub for this port rather than a no-op or a real thread.
#ifndef EDEN_SHIM_NSOPERATION_H
#define EDEN_SHIM_NSOPERATION_H

#import "NSObject.h"

@interface NSOperation : NSObject
- (void)main;
- (void)start;
- (BOOL)isFinished;
- (BOOL)isCancelled;
- (void)cancel;
@end

@interface NSOperationQueue : NSObject
- (void)addOperation:(NSOperation *)op;
- (void)setMaxConcurrentOperationCount:(NSInteger)count;
- (void)cancelAllOperations;
@end

#endif
#endif  // EDEN_USE_SYSTEM_FOUNDATION
