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
#include_next <Foundation/Foundation.h>
#else
// Trampoline for `#import <Foundation/Foundation.h>` (angle-bracket — safe to redirect via
// include-path ordering; see gl_es1_shim's framework/OpenGLES/ES1/gl.h for why quoted
// "X.h" includes CANNOT be redirected this way, only angle-bracket ones). This directory
// (web/src/shim/foundation/framework/) is added to the include path BEFORE any system
// dirs in CMakeLists.txt, so this is the only `<Foundation/Foundation.h>` the build ever sees.
//
// Pulls in every D3a shim class (see foundation-usage.md for the full inventory + what's
// real vs. stubbed).
#ifndef EDEN_TRAMPOLINE_FOUNDATION_H
#define EDEN_TRAMPOLINE_FOUNDATION_H

// The real <Foundation/Foundation.h> transitively drags in the C standard library, and engine
// files rely on that: Classes/hashmap.mm and Classes/Frustum.mm call malloc/calloc/free having
// included nothing but the prefix header. Reproduce that here rather than editing those files
// (this port does not touch engine sources). Found by the third real build — these two files
// were the ONLY remaining non-GL compile failures at that point.
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
// <limits.h> joined the list in Phase N Stage 3.2, and it is the same story one platform later:
// Classes/Menu_background.mm uses UINT_MAX having included nothing but the prefix header. It
// compiled on emsdk and on macOS because both sysroots leak <limits.h> in through some other
// header on the way; glibc's do not. A missing include that three toolchains hid is exactly what
// a third target is for — and this is where the engine's ambient C library comes from, so it
// belongs here rather than in the engine file.
#include <limits.h>

// Platform primitives Emscripten's libc lacks but iOS provides (arc4random, MAX/MIN) —
// see ../../platform_shims.h. Engine files receive these via the prefix header on device, so
// they must arrive the same way here.
#include "../../platform_shims.h"

#include "../../NSObject.h"
#include "../../NSAutoreleasePool.h"
#include "../../NSString.h"
#include "../../NSData.h"
#include "../../NSNumber.h"
#include "../../NSArray.h"
#include "../../NSDate.h"
#include "../../NSFileHandle.h"
#include "../../NSFileManager.h"
#include "../../NSThread.h"
#include "../../NSBundle.h"
#include "../../NSUserDefaults.h"
#include "../../NSURLConnection.h"
#include "../../NSErrorException.h"
#include "../../NSTimer.h"
#include "../../NSOperation.h"
#include "../../NSLogSupport.h"

#endif
#endif  // EDEN_USE_SYSTEM_FOUNDATION
