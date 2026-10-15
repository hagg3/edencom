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
#include_next <CoreFoundation/CFURL.h>
#else
// <CoreFoundation/CFURL.h> — imported directly by Classes/CDOpenALSupport.h. Apple splits the CF
// types across per-type headers; this port keeps them in one place, so this is a trampoline.
#ifndef EDEN_SHIM_COREFOUNDATION_CFURL_H
#define EDEN_SHIM_COREFOUNDATION_CFURL_H
#include <CoreFoundation/CoreFoundation.h>
#endif
#endif  // EDEN_USE_SYSTEM_FOUNDATION
