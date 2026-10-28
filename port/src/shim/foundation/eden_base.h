// eden_base.h — G.2b-0 (WORKING/de-objc-plan-2026-10-24.md D8). The base types the engine is written
// against, in a header that is plain C/C++: BOOL/YES/NO/nil/TRUE/FALSE, CGFloat/CGPoint/CGSize/CGRect
// with their *Make helpers and CGRectZero, UITextAlignment*, and MIN/MAX/ABS.
//
// One definition per target that ObjC++ and plain C++ translation units agree on, so mangled names
// and struct layouts match while both kinds coexist. On Apple the CG types are the SDK's own
// (CGFloat is double there, as it always was); elsewhere they are the port's float structs that
// uikit_stubs.h used to hold. `NSUInteger`/`NSInteger` are NOT provided (D8: the 13 uses become
// size_t/long in their files).
//
// Safe to include twice: every definition is guarded (the guards are the ones the deleted G.3 shim
// headers, framework/objc/objc.h and uikit_stubs.h, used).
#ifndef EDEN_BASE_H
#define EDEN_BASE_H

#if defined(__APPLE__)
#include <objc/objc.h>                    // BOOL, YES, NO, nil (C header, valid in C++)
#include <CoreGraphics/CGGeometry.h>      // CGFloat, CGPoint, CGSize, CGRect, CGRectZero, *Make
#else
#ifndef OBJC_BOOL_DEFINED
#define OBJC_BOOL_DEFINED
typedef signed char BOOL;
#endif
#ifndef YES
#define YES ((BOOL)1)
#endif
#ifndef NO
#define NO ((BOOL)0)
#endif
#if !defined(__OBJC__) && !defined(nil)
#define nil 0
#endif

typedef float CGFloat;                    // 32-bit, as on armv7 (the shim's long-standing choice)
typedef struct { CGFloat x, y; } CGPoint;
typedef struct { CGFloat width, height; } CGSize;
typedef struct { CGPoint origin; CGSize size; } CGRect;
static const CGRect CGRectZero = {{0, 0}, {0, 0}};
static inline CGPoint CGPointMake(CGFloat x, CGFloat y) { CGPoint p = {x, y}; return p; }
static inline CGSize CGSizeMake(CGFloat w, CGFloat h) { CGSize s = {w, h}; return s; }
static inline CGRect CGRectMake(CGFloat x, CGFloat y, CGFloat w, CGFloat h) {
    CGRect r = {{x, y}, {w, h}}; return r;
}
#endif

#ifndef TRUE
#define TRUE ((BOOL)1)
#endif
#ifndef FALSE
#define FALSE ((BOOL)0)
#endif

#ifndef EDEN_UITEXTALIGNMENT_DEFINED
#define EDEN_UITEXTALIGNMENT_DEFINED
typedef enum {
    UITextAlignmentLeft, UITextAlignmentCenter, UITextAlignmentRight
} UITextAlignment;
#endif

// Foundation's definitions, guarded the same way Apple's are.
#ifndef MAX
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif
#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif
#ifndef ABS
#define ABS(a) (((a) < 0) ? (-(a)) : (a))
#endif

#endif  // EDEN_BASE_H
