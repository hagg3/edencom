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
// NSLogSupport.h — D3a shim for the real `NSLog` C function (233 call sites — the highest-
// traffic single Foundation symbol in the engine, see foundation-usage.md). NOTE: the actual
// NSLog(...) *macro* the engine's call sites expand through is defined in the UNMODIFIED
// ../../../Eden_Prefix.pch (force-included for engine sources via CMakeLists.txt's
// `-include`, not duplicated here):
//     #ifndef __OPTIMIZE__
//     #    define NSLog(...) NSLog(__VA_ARGS__)   // expands to a call to THIS function
//     #else
//     #    define NSLog(...) {}                   // compiled away entirely in Release
//     #endif
// `__OPTIMIZE__` is auto-defined by clang at -O1 and above, so this "just works" the same way
// it did on device: Debug builds (-O0, see CMakeLists.txt) log; Release (-O2) compiles NSLog
// out with zero overhead, matching the original iOS build's behavior exactly.
#ifndef EDEN_SHIM_NSLOGSUPPORT_H
#define EDEN_SHIM_NSLOGSUPPORT_H

@class NSString;

#ifdef __cplusplus
extern "C" {
#endif
void NSLog(NSString *format, ...);
#ifdef __cplusplus
}
#endif

#endif
#endif  // EDEN_USE_SYSTEM_FOUNDATION
