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
// NSString.h — D3a shim, heaviest-used Foundation class (232 raw mentions across the engine —
// see foundation-usage.md). Backed by std::string internally. Implements every method in the
// "Implemented" list of foundation-usage.md; the two raster-only methods
// (drawAtPoint:withFont:, drawInRect:withFont:..., sizeWithFont:) are declared but stubbed —
// they belong to Stage P2's Texture2D/font-raster rewrite, not this shim.
//
// *** @"..." string literals — RESOLVED BY DESIGN, NOT YET LINK-PROVEN (Stage P0.1) ***
// The engine uses `@"literal"` syntax 743 times across non-seam Classes/*.mm, none of which
// this port may edit. Clang lowers each one to a *statically emitted instance* of a
// compiler-designated class (named via `-fconstant-string-class=`) whose ivar layout clang
// hard-codes as exactly `{ Class isa; const char *cString; unsigned int length; }` — no other
// fields, in that order.
//
// The fix is the class cluster below, mirroring how GNUstep itself solves this:
//   NSObject           — zero ivars (retain count lives in a side table; see NSObject.h)
//   NSString           — zero ivars, ABSTRACT. Every method here is implemented in terms of
//                        two primitives, -UTF8String and -length, so it works on ANY subclass.
//   NSConstantString   — declares exactly the two ivars clang expects, so a literal's static
//                        layout is `{isa, cString, length}` and nothing else. Immortal:
//                        retain/release are no-ops, dealloc never runs.
//   EdenConcreteString — the heap-allocated, std::string-backed string every factory method
//                        actually returns. Free to carry whatever ivars it likes.
//
// Verified safe by grep (2026-07-19): no engine file subclasses NSString and no engine file
// calls -retainCount, so nothing outside this shim can observe either the cluster indirection
// or the count's storage location.
//
// *** TWO THINGS P0.1 MUST STILL CONFIRM AGAINST THE REAL RUNTIME: ***
//  1. The exact field types/order clang expects for the chosen runtime. `gnustep-2.0`'s v2 ABI
//     may use a wider/flagged layout than the classic `{isa, char*, unsigned}` assumed here —
//     if so, adjust NSConstantString's ivars to match, NOT the rest of the cluster.
//  2. That CMakeLists.txt's `-fconstant-string-class=NSConstantString` actually takes effect
//     (some runtime targets ignore it and hard-require the name `NXConstantString` — if so,
//     rename the class and add an NSConstantString alias).
// Until a real link runs, treat this file as designed-but-unproven. See web-port-plan.md
// "D3 refinement" and docs/archive/PORT-STATUS-2026-08-13.md.
#ifndef EDEN_SHIM_NSSTRING_H
#define EDEN_SHIM_NSSTRING_H

#import "NSObject.h"
#include <string>
// CGPoint/CGRect/CGSize (used by the P2-stubbed draw methods below) are plain C structs, not
// ObjC classes — `@class` forward-declaration doesn't work for them, so pull in the real
// definitions. Safe: uikit_stubs.h depends only on NSObject.h, already included above.
#include "uikit_stubs.h"

@class NSData;
@class NSArray;
@class UIFont; // opaque, see uikit_stubs.h — only used by the P2-stubbed draw methods below.

typedef unsigned long NSStringEncoding;
#define NSUTF8StringEncoding      8
#define NSASCIIStringEncoding     1
#define NSNumericSearch           64

typedef struct _NSRange { NSUInteger location; NSUInteger length; } NSRange;
static inline NSRange NSMakeRange(NSUInteger loc, NSUInteger len) { return (NSRange){loc, len}; }
// NSNotFound moved to framework/objc/objc.h (next to NSInteger/NSUInteger) — it is needed by
// files that never include NSString.h, and two definitions with DIFFERENT values ((NSUInteger)-1
// here vs. Apple's NSIntegerMax) is exactly the kind of mismatch that makes a "not found" check
// pass in one translation unit and fail in another.

typedef long NSComparisonResult;
#define NSOrderedAscending  -1
#define NSOrderedSame        0
#define NSOrderedDescending  1

// ABSTRACT — declares ZERO ivars (see the layout note at the top of this file). Storage lives
// in the concrete subclasses below. Every method is implemented against the -UTF8String /
// -length primitives, so it behaves identically on a heap string and on an `@"..."` literal.
// DO NOT add an ivar here; it would shift the layout of all 743 static literal instances.
@interface NSString : NSObject {
    // (no ivars — abstract superclass of the cluster)
}

+ (NSString *)string;
+ (NSString *)stringWithFormat:(NSString *)fmt, ...;
+ (NSString *)stringWithUTF8String:(const char *)utf8;
+ (NSString *)stringWithCString:(const char *)cstr encoding:(NSStringEncoding)enc;

- (id)init;
- (id)initWithFormat:(NSString *)fmt, ...;
- (id)initWithUTF8String:(const char *)utf8;
- (id)initWithCString:(const char *)cstr encoding:(NSStringEncoding)enc;
- (id)initWithString:(NSString *)other;
- (id)initWithBytes:(const void *)bytes length:(NSUInteger)len encoding:(NSStringEncoding)enc;

- (NSUInteger)length;
- (unichar)characterAtIndex:(NSUInteger)index;
- (NSString *)substringFromIndex:(NSUInteger)index;
- (NSString *)substringToIndex:(NSUInteger)index;
- (NSString *)substringWithRange:(NSRange)range;
- (NSArray *)componentsSeparatedByString:(NSString *)sep;

- (NSString *)stringByAppendingString:(NSString *)other;
- (NSString *)stringByAppendingFormat:(NSString *)fmt, ...;
- (NSString *)stringByAppendingPathComponent:(NSString *)component;
- (NSString *)stringByDeletingPathExtension;
- (NSString *)stringByDeletingLastPathComponent;
- (NSString *)pathExtension;
- (NSString *)lastPathComponent;
- (NSString *)stringByReplacingOccurrencesOfString:(NSString *)target withString:(NSString *)repl;
- (BOOL)isAbsolutePath;

- (BOOL)isEqualToString:(NSString *)other;
- (NSComparisonResult)compare:(NSString *)other;
- (NSComparisonResult)compare:(NSString *)other options:(NSUInteger)opts;
- (BOOL)hasPrefix:(NSString *)prefix;
- (BOOL)hasSuffix:(NSString *)suffix;
- (NSRange)rangeOfString:(NSString *)needle;

- (double)doubleValue;
- (int)intValue;
- (float)floatValue;
- (BOOL)boolValue;
- (NSInteger)integerValue;

- (NSString *)uppercaseString;
- (NSString *)lowercaseString;

- (const char *)UTF8String;
- (const char *)cStringUsingEncoding:(NSStringEncoding)enc;
- (const char *)cString; // deprecated Foundation method, still called 3x — see foundation-usage.md
- (BOOL)getCString:(char *)buffer maxLength:(NSUInteger)maxLength encoding:(NSStringEncoding)enc; // Util.mm cpstring()
- (NSData *)dataUsingEncoding:(NSStringEncoding)enc;
- (BOOL)writeToFile:(NSString *)path atomically:(BOOL)atomically
           encoding:(NSStringEncoding)enc error:(id *)error;

// P2 (Texture2D/font raster — TODO, not this shim's job, stubbed as no-ops/zero-size):
- (void)drawAtPoint:(CGPoint)point withFont:(UIFont *)font;
- (void)drawInRect:(CGRect)rect withFont:(UIFont *)font;
- (CGSize)sizeWithFont:(UIFont *)font;

@end

// A named type for the _std ivar below, so -dealloc can spell the explicit destructor call
// (`~eden_std_string`) — see NSString.mm's -dealloc for why one has to exist at all.
typedef std::string eden_std_string;

// The heap-allocated, std::string-backed member of the cluster — what every NSString factory
// method actually returns. Engine code only ever sees it through an `NSString *`, exactly as
// on real Foundation (where the concrete class is the equally-private __NSCFString).
@interface EdenConcreteString : NSString {
@public
    eden_std_string _std; // shim-internal; engine code never touches this (nor could it — the
                          // engine's variables are all typed `NSString *`, which has no _std).
}
+ (EdenConcreteString *)stringWithStd:(const std::string &)s; // autoreleased, shim-internal
- (std::string &)stdString;                                   // mutable access for NSMutableString
@end

// *** LAYOUT-CRITICAL — see the note at the top of this file. ***
// Must declare EXACTLY these two ivars, in this order, and inherit only from zero-ivar
// classes, so a literal's static layout is `{ Class isa; const char *cString; unsigned length; }`
// — the layout clang bakes into every `@"..."` it emits. Adding, reordering, or resizing any
// field here silently corrupts all 743 literal instances in the engine. Selected via
// `-fconstant-string-class=NSConstantString` (CMakeLists.txt).
@interface NSConstantString : NSString {
@public
    const char  *_cString;
    unsigned int _length;
}
@end

@interface NSMutableString : EdenConcreteString
+ (NSMutableString *)string;
+ (NSMutableString *)stringWithCapacity:(NSUInteger)capacity;
- (void)appendString:(NSString *)other;
- (void)appendFormat:(NSString *)fmt, ...;
- (void)replaceOccurrencesOfString:(NSString *)target withString:(NSString *)repl
                            options:(NSUInteger)opts range:(NSRange)range;
- (void)setString:(NSString *)other;
@end

#ifdef __cplusplus
#include <cstdarg>
#include <string>
// Shared %@-aware formatter (implemented in NSString.mm) — used by NSString's own
// stringWithFormat:/family AND by NSLogSupport.mm's NSLog, so both go through one formatting
// implementation instead of two subtly-different copies.
std::string eden_format_nsstring(const char *fmt, va_list args);
#endif

#endif
#endif  // EDEN_USE_SYSTEM_FOUNDATION
