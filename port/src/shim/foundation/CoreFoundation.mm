// CoreFoundation.mm — implementations for framework/CoreFoundation/CoreFoundation.h.
//
// Read that header first: it says which of these are real (CFAbsoluteTimeGetCurrent, CFRelease,
// CFRetain) and which are Stage-P5 audio-path stubs (the CFString/CFURL pair). This file is
// Objective-C++ because toll-free bridging is implemented the only honest way — by sending
// -retain/-release to the object the CFTypeRef actually is.
#import "NSObject.h"

#include <CoreFoundation/CoreFoundation.h>

#include "platform_shims.h"

extern "C" {

// The CF reference date (2001-01-01 00:00:00 GMT) as a Unix timestamp. Only the DIFFERENCE of
// two readings matters to World.mm, but honoring the epoch keeps a web-build log line comparable
// with a device one.
static const double kCFAbsoluteTimeIntervalSince1970 = 978307200.0;

CFAbsoluteTime CFAbsoluteTimeGetCurrent(void) {
  // eden_platform_now_ms() is milliseconds from a monotonic clock (performance.now() on web),
  // NOT wall time — so it is offset by the page's own epoch here. Wall-clock accuracy is not
  // what World.mm wants (it measures terrain-gen and file-write durations); monotonicity is,
  // and this is the one clock that guarantees it on every target.
  return eden_platform_now_ms() / 1000.0 - kCFAbsoluteTimeIntervalSince1970;
}

void CFRelease(CFTypeRef cf) {
  if (cf) [(id)cf release];
}

CFTypeRef CFRetain(CFTypeRef cf) {
  if (cf) [(id)cf retain];
  return cf;
}

// TODO P5: both of these serve CDOpenALSupport.m's "is this a .wav or a .caf?" check. Returning
// null/greater-than makes that check fall through to its CAF branch. Nothing loads audio data
// until Stage P5 anyway (see framework/AudioToolbox/AudioFile.h), so this is unreachable in
// practice rather than merely wrong.
CFStringRef CFURLCopyPathExtension(CFURLRef url) {
  (void)url;
  return 0;
}

CFComparisonResult CFStringCompare(CFStringRef a, CFStringRef b, CFStringCompareFlags flags) {
  (void)a;
  (void)b;
  (void)flags;
  return kCFCompareGreaterThan;
}

}  // extern "C"
