//
// Prefix header: force-included (-include / /FI) into the engine and seam sources by native/CMakeLists.txt's prefix rule
//

// G.3: the stock <Availability.h> SDK-3.0 check and the `#ifdef __OBJC__` Foundation/UIKit imports are gone:
// no engine file is ObjC any more (de-ObjC plan D9), and off Apple there is no such header to import.

// G.2b-0 (de-ObjC plan D8/D9): BOOL/CGRect/MIN/MAX... from a plain C/C++ header, so converted .cpp files see
// the same base types the .mm files always did.
#include "eden_base.h"

// G.5b-0 (renderer plan §9.1): every engine arc4random() call goes through one RNG a harness can pin
// (--scene-shot). Unpinned, eden_arc4random() IS the platform's arc4random (platform_shims.cpp), so the
// release path is unchanged. <stdlib.h> comes first so its own declaration is not renamed.
#include <stdlib.h>
#include <stdint.h>
// memcpy/memset/strlen...: engine files got <string.h> transitively from the deleted ES 1.1 shim and PVRT
// headers (G.5b-7, LG.7). Apple's libc++ still drags it in, Linux libstdc++ does not (CI run 40735042491).
// Same for INT_MIN/UINT_MAX (run 40735807558) and the other overload-free C headers. <math.h> is left out on
// purpose: it adds float overloads to the global namespace, which could change float results in a TU.
#include <string.h>
#include <limits.h>
#include <float.h>
#include <stdio.h>
#include <assert.h>
#ifdef __cplusplus
extern "C"
#endif
uint32_t eden_arc4random(void);
#define arc4random eden_arc4random

#ifndef __OPTIMIZE__
#    define NSLog(...) NSLog(__VA_ARGS__)
#    define printg(...) printf(__VA_ARGS__)
#else
#    define NSLog(...) {}
#    define printg(...) {}
#endif

#pragma clang diagnostic ignored "-Wmissing-selector-name"