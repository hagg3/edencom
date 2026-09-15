// main_native.h/.cpp — native twin of web/src/seam/main_web.{h,cpp} (Phase N Stage 1).
//
// Preserves the original split rather than collapsing it into the entry point: this is "what the
// repo-root main.m used to orchestrate" (construct the app-delegate-equivalent and hand control
// to it, as UIApplicationMain did), while src/entry/eden_main_native.cpp owns the real `int main`
// and the frame loop.
#ifndef EDEN_SEAM_MAIN_NATIVE_H
#define EDEN_SEAM_MAIN_NATIVE_H

namespace eden_native {

// Constructs the process-lifetime EdenAppDelegate and calls didFinishLaunching().
void eden_seam_main();

class EdenAppDelegate;
EdenAppDelegate* eden_seam_get_app_delegate();

}  // namespace eden_native

#endif
