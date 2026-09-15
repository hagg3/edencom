// CDXMacOSXSupport.h — Phase N Stage 1 (WORKING/native-migration-plan-2026-09-04.md).
//
// Classes/CDAudioManager.h:26 branches on the iOS deployment version:
//     #if __IPHONE_OS_VERSION_MIN_REQUIRED >= 30000
//         #import <AVFoundation/AVFoundation.h>
//     #else
//         #import "CDXMacOSXSupport.h"
//     #endif
// — i.e. CocosDenshion's own name for "the Mac build's stand-in for AVFoundation". That file was
// never in this tree, because the shipped iOS app always took the first branch.
//
// The native target cannot take the first branch by the obvious route: defining
// __IPHONE_OS_VERSION_MIN_REQUIRED on a real macOS SDK makes every system header compile as if
// for iOS 3.0, which detonates inside CoreServices/Security (see
// web/src/shim/foundation/framework/Availability.h for the exact failure). So it takes the SECOND
// branch and this file supplies what the first one would have. A quoted
// `#import "CDXMacOSXSupport.h"` searches the including file's own directory (Classes/) first and
// then the -I list, which is how this is reached at all.
//
// It resolves to the PORT'S OWN <AVFoundation/AVFoundation.h> (web/src/shim/foundation/framework/),
// not Apple's — deliberately. The real one drags CoreServices in, whose CarbonCore/AIFF.h defines
// `ChunkHeader`, and so does Classes/FileManager.h; see that trampoline's header for the full
// note.
//
// Audio itself is stubbed for Stage 1 (native/src/seam/seam_link_stubs_native.mm); this exists so
// the CocosDenshion HEADERS parse in the three translation units that include them.
#ifndef EDEN_NATIVE_CDX_MACOSX_SUPPORT_H
#define EDEN_NATIVE_CDX_MACOSX_SUPPORT_H
#import <AVFoundation/AVFoundation.h>
#endif
