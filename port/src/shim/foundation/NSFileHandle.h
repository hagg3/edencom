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
// NSFileHandle.h — D3a shim, P1-adequate only (see foundation-usage.md "NSFileHandle"). This
// is the append-only .eden format's I/O primitive (docs/eden-file-format.md,
// docs/save-load.md) — Stage P4 is where this gets its REAL implementation, swapping the
// backing store to OPFS FileSystemSyncAccessHandle (plan D1: "gives exactly that — synchronous
// random-access read/write/seek"). This pass backs it with plain stdio (FILE*) so the class
// exists and P1 (headless link) can proceed; do not trust it for real save-file correctness
// yet — P4's checklist (C2/M4/N4/M1/N3/H1/H6/M2 audit findings, all folded into that stage per
// the plan) has not been applied here.
#ifndef EDEN_SHIM_NSFILEHANDLE_H
#define EDEN_SHIM_NSFILEHANDLE_H

#import "NSObject.h"
#include <cstdio>

@class NSString;
@class NSData;

typedef unsigned long long eden_offset_t;

@interface NSFileHandle : NSObject {
@public
    FILE *_fp; // TODO P4: replace with an OPFS sync-access-handle wrapper; keep this class's
               // public method surface identical so FileManager.mm's (P4-rewritten) call
               // sites don't need to change shape, only what's behind them.
    char *_backupPathC;      // NULL once the backup has fired, or if this handle doesn't owe one
                             // (see -writeData:/-truncateFileAtOffset: in the .mm for why the
                             // backup is deferred to first WRITE for an "updating" handle instead
                             // of firing eagerly at open). Plain strdup'd C string, not an NSString
                             // ivar -- this class's ivar layout was only ever measured with one
                             // own ivar (_fp); a second ObjC-object ivar hit a real "function
                             // signature mismatch" crash in practice, not worth re-deriving the
                             // ivar-offset math for what a plain C string does just as well.
}

+ (NSFileHandle *)fileHandleForReadingAtPath:(NSString *)path;
+ (NSFileHandle *)fileHandleForWritingAtPath:(NSString *)path;
+ (NSFileHandle *)fileHandleForUpdatingAtPath:(NSString *)path;

- (void)seekToFileOffset:(eden_offset_t)offset;
- (eden_offset_t)seekToEndOfFile;
- (eden_offset_t)offsetInFile;
- (NSData *)readDataOfLength:(NSUInteger)length;
- (NSData *)readDataToEndOfFile;
- (void)writeData:(NSData *)data;
- (void)truncateFileAtOffset:(eden_offset_t)offset;
- (void)closeFile;

@end

#endif
#endif  // EDEN_USE_SYSTEM_FOUNDATION
