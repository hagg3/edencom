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
#include_next <CoreFoundation/CoreFoundation.h>
#else
// <CoreFoundation/CoreFoundation.h> — the CF surface this engine actually touches, which is
// small and almost entirely toll-free-bridged casts of Foundation objects.
//
// Call sites (grep, 2026-07-19): Util.mm's `CFRelease(provider)` + `CFStringRef md5hash`,
// World.mm's three `CFAbsoluteTimeGetCurrent()` timings, Sound.m's `(CFURLRef)afUrl` cast, and
// CDOpenALSupport.m's `CFURLCopyPathExtension`/`CFStringCompare`.
//
// ONE of these is a real implementation and the rest are not, which is the distinction to keep
// straight: `CFAbsoluteTimeGetCurrent` is genuinely implemented (web-port-plan.md Stage P1 lists
// it by name as a seam to replace, alongside arc4random), because World.mm uses it for elapsed
// timing that must actually advance. The CFString/CFURL helpers are P5-adjacent stubs used only
// by the audio path.
//
// The opaque struct pointers below are what make the engine's `(CFURLRef)nsurl` casts compile.
// They deliberately do NOT alias the shim's Objective-C classes at the type level — the engine
// always casts explicitly, so nothing needs the bridge to be implicit, and keeping them distinct
// stops CF types from silently accepting objects the CF functions here can't handle.
#ifndef EDEN_SHIM_COREFOUNDATION_COREFOUNDATION_H
#define EDEN_SHIM_COREFOUNDATION_COREFOUNDATION_H

#include <stddef.h>   // size_t — Classes/md5.h declares its API with it and includes only this
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef const void *CFTypeRef;
typedef const struct __CFString *CFStringRef;
typedef struct __CFString *CFMutableStringRef;
typedef const struct __CFURL *CFURLRef;
typedef const struct __CFData *CFDataRef;
typedef const struct __CFAllocator *CFAllocatorRef;

typedef signed long CFIndex;
typedef double CFAbsoluteTime;
typedef double CFTimeInterval;

typedef enum CFComparisonResult {
  kCFCompareLessThan    = -1,
  kCFCompareEqualTo     = 0,
  kCFCompareGreaterThan = 1
} CFComparisonResult;

typedef uint32_t CFStringCompareFlags;
enum {
  kCFCompareCaseInsensitive = 1,
  kCFCompareBackwards       = 4,
  kCFCompareAnchored        = 8
};

#define kCFAllocatorDefault ((CFAllocatorRef)0)

// REAL. Seconds since the CF reference date (2001-01-01). World.mm only ever DIFFERENCES two
// readings, so the epoch is irrelevant to behavior — but it is honored anyway so that a value
// logged from the web build is comparable to one logged on device.
CFAbsoluteTime CFAbsoluteTimeGetCurrent(void);

// REAL for shim Foundation objects — forwards to -release, which is exactly what toll-free
// bridging means on device. Passing a genuinely non-Objective-C CF object would be a bug, and
// there are none in this tree (every CFTypeRef here originates as a cast Foundation object).
void CFRelease(CFTypeRef cf);
CFTypeRef CFRetain(CFTypeRef cf);

// P5 stubs — audio-path only (CDOpenALSupport.m), see this header's opening note.
CFStringRef CFURLCopyPathExtension(CFURLRef url);
CFComparisonResult CFStringCompare(CFStringRef a, CFStringRef b, CFStringCompareFlags flags);

#ifdef __cplusplus
}
#endif

#endif
#endif  // EDEN_USE_SYSTEM_FOUNDATION
