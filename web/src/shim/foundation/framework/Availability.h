// Trampoline for `<Availability.h>`, unconditionally #imported at the top of the PARENT
// tree's ../../../../Eden_Prefix.pch (untouched — we force-include that file as-is for
// engine sources, see CMakeLists.txt, rather than duplicate its NSLog-macro/diagnostic-pragma
// logic here). Apple's real Availability.h only defines SDK version-check macros
// (__IPHONE_3_0 etc.) that the pch's own `#ifndef __IPHONE_3_0` / #warning guard against —
// leaving it undefined here just means that harmless warning fires once per translation unit,
// exactly as it would on a very old SDK. No content needed beyond "the file exists".
//
// UPDATE (this pass): "no content needed" turned out to be wrong for one macro.
// Classes/CDAudioManager.h branches on `#if __IPHONE_OS_VERSION_MIN_REQUIRED >= 30000` and takes
// the pre-3.0 path when it is undefined (undefined identifiers evaluate to 0 in #if), asking for
// "CDXMacOSXSupport.h" — a file that is NOT in this tree, because the shipped app never compiled
// that branch. Defining these reproduces the shipped build's own configuration (Eden-Info.plist
// targets iOS 3.0+); it is not a porting choice so much as restoring a fact the SDK supplied.
//
// UPDATE (Phase N Stage 1, WORKING/native-migration-plan-2026-09-04.md): on a NATIVE Apple target
// this file must ADD to the SDK's Availability.h, not replace it. This directory is on the
// include path ahead of the SDK's, so `#include <Availability.h>` from any *system* header —
// <sys/resource.h>, <_stdio.h>, <malloc/_malloc_type.h>, and dozens more — lands here, and
// without `__API_AVAILABLE`/`__OSX_AVAILABLE_STARTING` those headers fail to parse. That presents
// as hundreds of "expected function body after function declarator" errors inside the SDK, which
// is about as far from the real cause as a diagnostic can get. `#include_next` continues the
// search from the directory after this one, so it finds the genuine SDK header, and the iOS
// version constants below are then layered on top exactly as before.
//
// Deliberately NOT `#if defined(__EMSCRIPTEN__)`-guarded the other way round: Emscripten's
// sysroot has no <Availability.h> at all, so #include_next there would be a hard error.
#if defined(__APPLE__)
#include_next <Availability.h>
#endif

// ...AND, on that target, add NOTHING. The block below defines __IPHONE_OS_VERSION_MIN_REQUIRED,
// which on a real macOS SDK makes every system header believe it is being compiled for iOS 3.0.
// That is not a subtle divergence: <AudioToolbox/...> then pulls the iOS branch of CoreServices
// and Security, and the build dies inside SecCertificate.h with "functions that differ only in
// their return type cannot be overloaded" — 300 lines from anything this project wrote. The one
// engine header that genuinely needs the macro (Classes/CDAudioManager.h, choosing AVFoundation
// over a CDXMacOSXSupport.h that is not in this tree) is served instead by
// native/src/shim/CDXMacOSXSupport.h, which supplies exactly what that branch would have.
#if !defined(EDEN_USE_SYSTEM_FOUNDATION)

#ifndef EDEN_TRAMPOLINE_AVAILABILITY_H
#define EDEN_TRAMPOLINE_AVAILABILITY_H

#define __IPHONE_2_0     20000
#define __IPHONE_3_0     30000
#define __IPHONE_4_0     40000

#ifndef __IPHONE_OS_VERSION_MIN_REQUIRED
#define __IPHONE_OS_VERSION_MIN_REQUIRED __IPHONE_3_0
#endif
#ifndef __IPHONE_OS_VERSION_MAX_ALLOWED
#define __IPHONE_OS_VERSION_MAX_ALLOWED __IPHONE_4_0
#endif

#endif  // EDEN_TRAMPOLINE_AVAILABILITY_H

#endif  // !EDEN_USE_SYSTEM_FOUNDATION
