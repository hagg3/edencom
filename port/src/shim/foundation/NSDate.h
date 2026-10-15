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
// NSDate.h — D3a shim. Backed by double seconds-since-epoch via emscripten_get_now()/
// std::chrono (per plan: "CFAbsoluteTime -> emscripten_get_now"). NSDateFormatter is declared
// but its formatting methods are TODO P4 (see foundation-usage.md — only 2 raw mentions,
// exact format string not worth guessing before that stage re-reads the call sites).
#ifndef EDEN_SHIM_NSDATE_H
#define EDEN_SHIM_NSDATE_H

#import "NSObject.h"

typedef double NSTimeInterval;

@interface NSDate : NSObject {
@public
    NSTimeInterval _secondsSinceEpoch;
}
+ (NSDate *)date;
+ (NSDate *)dateWithTimeIntervalSinceNow:(NSTimeInterval)secs;
- (NSTimeInterval)timeIntervalSinceNow;
- (NSTimeInterval)timeIntervalSince1970;
- (NSTimeInterval)timeIntervalSinceDate:(NSDate *)other;
@end

@interface NSDateFormatter : NSObject
- (void)setDateFormat:(NSString *)fmt; // TODO P4: store + honor format string
- (NSString *)dateFormat;              // TODO P4
- (NSString *)stringFromDate:(NSDate *)date; // TODO P4: currently returns a fixed ISO-ish stamp
- (NSDate *)dateFromString:(NSString *)str;  // TODO P4: currently returns [NSDate date]
@end

#endif
#endif  // EDEN_USE_SYSTEM_FOUNDATION
