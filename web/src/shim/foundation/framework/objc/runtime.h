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
#include_next <objc/runtime.h>
#else
// objc/runtime.h — the subset of the Objective-C runtime API this port's Foundation shim calls.
//
// *** THIS FILE DECLARES; IT DOES NOT IMPLEMENT. *** Per the P0.1 spike (see objc/objc.h's
// header comment), Emscripten ships no libobjc, so every symbol below — plus the dispatch
// entry points clang emits behind every `[obj msg]` in the engine — is currently UNDEFINED at
// link time. Compiling can succeed without them; linking cannot. Resolving that is the open
// "D3 runtime" decision recorded in web-port-plan.md; the three candidates are:
//   (a) vendor ObjFW's runtime      — self-contained, portable, known to build on odd targets;
//                                     would mean switching to `-fobjc-runtime=objfw` (spike
//                                     confirmed that flag compiles).
//   (b) vendor GCC's libobjc        — matches the gnustep-1.9 GNU ABI currently selected.
//   (c) hand-write a minimal runtime — the engine uses only plain classes + message sends: no
//                                     categories, no protocols beyond empty markers, no ObjC
//                                     exceptions, no KVO, no ARC. The needed surface is roughly
//                                     just this header plus objc_msg_lookup. Smallest artifact,
//                                     most control, but it is real work and must be exactly
//                                     right about the GNU ABI's class/metaclass layout.
// Do NOT expand this header speculatively — every symbol added here is one more thing whichever
// option wins has to provide.
//
// The declarations below are exactly what src/shim/foundation/*.mm currently calls (NSObject.mm's
// +alloc/-dealloc/-class/-isKindOfClass:/-respondsToSelector:, NSString.mm's cluster-swap check).
#ifndef EDEN_SHIM_OBJC_RUNTIME_H
#define EDEN_SHIM_OBJC_RUNTIME_H

#include "objc.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- Instance lifecycle (NSObject.mm: +alloc, -dealloc) ---
id    class_createInstance(Class cls, size_t extraBytes);
void  object_dispose(id obj);

// --- Introspection (NSObject.mm: -class/-isKindOfClass:/-isMemberOfClass:/-respondsToSelector:,
//     NSString.mm: the class-cluster swap guard) ---
Class object_getClass(id obj);
Class class_getSuperclass(Class cls);
BOOL  class_respondsToSelector(Class cls, SEL sel);
const char *class_getName(Class cls);
Class objc_getClass(const char *name);

// --- Selectors ---
SEL   sel_registerName(const char *name);
const char *sel_getName(SEL sel);

// --- GNU-ABI message dispatch. Clang emits calls to THESE (not Apple's objc_msgSend) for every
//     bracket send in the engine under -fobjc-runtime=gnustep-1.9. Declared here for
//     completeness/documentation: whichever runtime option above wins must define them.
struct objc_super { id receiver; Class super_class; };
IMP objc_msg_lookup(id receiver, SEL sel);
IMP objc_msg_lookup_super(struct objc_super *super, SEL sel);

#ifdef __cplusplus
}
#endif

#endif
#endif  // EDEN_USE_SYSTEM_FOUNDATION
