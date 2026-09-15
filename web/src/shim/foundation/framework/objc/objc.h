// ---------------------------------------------------------------------------------------------
// Phase N Stage 1 (WORKING/native-migration-plan-2026-09-04.md): DEFER, don't shadow, on a target
// that has the real header. This directory is ahead of the SDK's on the include path, so on macOS
// every system header that reaches for this name would otherwise land on the Emscripten-shaped
// trampoline below and fail to parse — a class of error that reports itself from deep inside the
// SDK with no hint of where it came from. `#include_next` resumes the search past this directory.
//
// EDEN_USE_SYSTEM_FOUNDATION is defined by native/CMakeLists.txt only. See that file's header for
// why the native macOS target uses Apple's real ObjC runtime and Foundation rather than this
// port's hand-written ones (short version: MEASURED — clang cannot lower ANY GNU ObjC ABI to
// Mach-O; it emits selector-name globals in a COMDAT and the MachO backend has no COMDATs).
// ---------------------------------------------------------------------------------------------
#if defined(EDEN_USE_SYSTEM_FOUNDATION)
#include_next <objc/objc.h>
#else
// objc/objc.h — minimal Objective-C runtime *interface* for the web port.
//
// *** WHY THIS FILE EXISTS (P0.1 spike result, 2026-07-19): Emscripten ships NO Objective-C
// runtime — not the headers, not a libobjc. A `find` across emsdk 3.1.74's system/ and cache/
// for `objc*`/`libobjc*` returns nothing. So this port must supply BOTH:
//   1. the runtime interface (this file + runtime.h) — needed to COMPILE, and
//   2. a runtime IMPLEMENTATION (class registration + message dispatch) — needed to LINK.
// (1) is what these headers are. (2) is NOT solved yet — see runtime.h's header comment and
// web-port-plan.md's "D3 runtime" open question. Compiling is a real milestone on its own, so
// these headers are worth having even before (2) is decided.
//
// `id`, `Class`, and `SEL` are clang builtins in Objective-C mode (verified by spike — a .mm
// using `Class isa` compiles with no objc header at all). They are NOT builtins in plain C/C++
// though, and this header transitively reaches .cpp/.c translation units (the engine's
// ColorUtil.cpp/VectorUtil.cpp/project.c and the PVRT SDK all pull in Globals.h → Foundation).
// Assuming "builtin, never declare" cost 431 of the 1002 errors in the second real build;
// hence the __OBJC__ split below — declare them ONLY where clang doesn't.
//
// Targets the GNU runtime ABI, `-fobjc-runtime=gnustep-1.9` (CMakeLists.txt) — chosen because
// gnustep-2.0 is incompatible with the wasm binary format. Under the GNU ABI clang emits calls
// to `objc_msg_lookup`/`objc_msg_lookup_super` rather than Apple's `objc_msgSend`; see runtime.h.
#ifndef EDEN_SHIM_OBJC_OBJC_H
#define EDEN_SHIM_OBJC_OBJC_H

#ifdef __cplusplus
extern "C" {
#endif

// --- The three core types ---------------------------------------------------------------
// In Objective-C mode clang provides these itself and redeclaring them is an error, so they
// are declared ONLY for the non-ObjC translation units that transitively include this header.
// The layouts match the GNU runtime ABI so that a struct declared in a .cpp and the same struct
// seen from a .mm agree — they must, since both link into one binary.
#ifndef __OBJC__
typedef struct objc_class  *Class;
typedef struct objc_object { Class isa; } *id;
typedef struct objc_selector *SEL;
#endif

// The engine relies on BOOL/YES/NO/nil being in scope exactly as they are on iOS (e.g.
// Classes/Util.h declares `BOOL dead;`), and gets them via the prefix header's Foundation import.
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

// Not part of real Foundation, but Classes/Texture2D.h (unmodified engine header) writes
// `BOOL pressed=FALSE;` as a default member initializer — TRUE/FALSE are plain C/C++ macros on
// iOS via a transitive system header this port doesn't provide, so they need to exist here too.
#ifndef TRUE
#define TRUE ((BOOL)1)
#endif
#ifndef FALSE
#define FALSE ((BOOL)0)
#endif

#ifndef nil
#define nil ((id)0)
#endif
#ifndef Nil
#define Nil ((Class)0)
#endif

// `oneway` is a distributed-objects qualifier that survives in Foundation's -release signature
// (and therefore in this shim's NSObject.h). Nothing distributed exists here; it's a no-op.
#ifndef oneway
#define oneway
#endif

// --- NSInteger/NSUInteger/CGFloat -----------------------------------------------------------
// Real Foundation declares these in <Foundation/NSObjCRuntime.h> and <CoreGraphics/CGBase.h>.
// The shim had no definition for them at all, which cost 87 "unknown type name 'NSUInteger'"
// errors cascading into 497 "expected a type" in the fifth real build — every Foundation shim
// header uses NSUInteger in a method signature. They live here (rather than in a separate
// NSObjCRuntime.h) because this header is the one thing every other shim header already
// includes.
//
// Widths target wasm32, matching the ORIGINAL 32-bit armv7 iOS build this engine shipped as —
// `long` is 32-bit under wasm32, so these are the same sizes the engine's on-disk structs and
// casts were written against. That matters: per CLAUDE.md, WorldFileHeader/ColumnIndex/
// EntityData are raw-memcpy'd, so integer widths are part of the save format's ABI.
typedef unsigned long NSUInteger;
typedef long          NSInteger;
#ifndef CGFLOAT_DEFINED
#define CGFLOAT_DEFINED
typedef float CGFloat;   // 32-bit CGFloat, as on armv7 — NOT the 64-bit arm64/macOS variant.
#endif

// NSNotFound — Foundation's "no such index" sentinel, returned by -rangeOfString:/-indexOfObject:
// and compared against by engine code. It lives in <NSObjCRuntime.h> on Apple; here it sits next
// to NSUInteger for the same reason that type does (see the block above): this is the one header
// every other shim header already includes.
//
// The VALUE matters. Apple defines it as NSIntegerMax, which is 32-bit here (wasm32, matching the
// original armv7 build) — NOT the 64-bit value a modern arm64/macOS build would use. Engine code
// that compares a returned index against NSNotFound only works if both sides agree.
#ifndef NSNotFound
#define NSNotFound ((NSInteger)0x7fffffff)
#endif

typedef unsigned short unichar;

typedef struct objc_method   *Method;
typedef struct objc_ivar     *Ivar;
typedef struct objc_category *Category;
typedef struct objc_property *objc_property_t;
typedef id (*IMP)(id, SEL, ...);

#ifdef __cplusplus
}
#endif

#endif
#endif  // EDEN_USE_SYSTEM_FOUNDATION
