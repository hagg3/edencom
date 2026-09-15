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
// NSErrorException.h — D3a shim, low priority (4+4 raw mentions, see foundation-usage.md).
// NSException maps to NSLog + abort() — no @try/@catch unwinding support assumed; TODO P1:
// verify no engine (non-seam) file actually @catches one (a scan at write time found none,
// but re-verify if this trips a mysterious abort during the P1 spike).
#ifndef EDEN_SHIM_NSERROREXCEPTION_H
#define EDEN_SHIM_NSERROREXCEPTION_H

#import "NSObject.h"

@class NSString;
@class NSDictionary;

@interface NSError : NSObject
+ (NSError *)errorWithDomain:(NSString *)domain code:(NSInteger)code userInfo:(NSDictionary *)info;
- (NSString *)localizedDescription;
@end

@interface NSException : NSObject
+ (NSException *)exceptionWithName:(NSString *)name reason:(NSString *)reason
                           userInfo:(NSDictionary *)info;
+ (void)raise:(NSString *)name format:(NSString *)fmt, ...;
- (NSString *)reason;
@end

// Standard Foundation exception-name constant — Texture2D.h's `default:` switch cases raise this
// by name (`[NSException raise:NSInternalInconsistencyException format:@""]`), same as real
// Foundation declares it as an `extern NSString * const`.
extern NSString *const NSInternalInconsistencyException;

#endif
#endif  // EDEN_USE_SYSTEM_FOUNDATION
