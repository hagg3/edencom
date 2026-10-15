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
// NSFileManager.h — D3a shim, P1-adequate (POSIX-backed, works under Emscripten's default
// MEMFS). Stage P4 swaps the backing to OPFS directory APIs; keep this class's public surface
// stable across that swap (see foundation-usage.md "NSFileManager").
#ifndef EDEN_SHIM_NSFILEMANAGER_H
#define EDEN_SHIM_NSFILEMANAGER_H

#import "NSObject.h"

@class NSString;
@class NSArray;
@class NSData;   // -createFileAtPath:contents:attributes: takes one by pointer

typedef NSUInteger NSSearchPathDirectory;
typedef NSUInteger NSSearchPathDomainMask;
#define NSDocumentDirectory 9
#define NSUserDomainMask 1

@interface NSFileManager : NSObject
+ (NSFileManager *)defaultManager;
- (BOOL)fileExistsAtPath:(NSString *)path;
- (BOOL)fileExistsAtPath:(NSString *)path isDirectory:(BOOL *)isDir;
- (BOOL)createFileAtPath:(NSString *)path contents:(NSData *)contents attributes:(id)attrs;
- (BOOL)createDirectoryAtPath:(NSString *)path
   withIntermediateDirectories:(BOOL)createIntermediates
                    attributes:(id)attrs error:(id *)error;
- (BOOL)removeItemAtPath:(NSString *)path error:(id *)error;
- (BOOL)copyItemAtPath:(NSString *)src toPath:(NSString *)dst error:(id *)error;
- (BOOL)moveItemAtPath:(NSString *)src toPath:(NSString *)dst error:(id *)error;
- (NSArray *)contentsOfDirectoryAtPath:(NSString *)path error:(id *)error;
@end

// C function, not a method — matches Foundation's real free-function signature.
NSArray *NSSearchPathForDirectoriesInDomains(NSSearchPathDirectory directory,
                                              NSSearchPathDomainMask domainMask,
                                              BOOL expandTilde);

#endif
#endif  // EDEN_USE_SYSTEM_FOUNDATION
