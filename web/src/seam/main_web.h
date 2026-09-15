// main_web.h/.cpp — Stage P1 seam replacement for the ROOT main.m (note: NOT Classes/main.m —
// CLAUDE.md flags this ambiguity: "Which main.m (root vs Classes/main.m) does Xcode
// reference?" — tools/engine-sources.txt resolves it: the compiled one is the repo-root
// `main.m`, listed there verbatim as `main.m`, not `Classes/main.m`. Either way this seam
// replaces whichever one Xcode built.)
//
// The original (repo-root main.m) is nearly empty by design — it just wraps
// UIApplicationMain() in an autorelease pool. All the real lifecycle work already lived in
// EdenAppDelegate (Classes/EdenAppDelegate.mm, replaced by EdenAppDelegate_web.*, this same
// directory). This file preserves that split rather than collapsing everything into
// src/entry/eden_main.cpp's `int main()`: this is "what main.m used to orchestrate"
// (constructing the top-level app-delegate-equivalent object), while src/entry/eden_main.cpp
// owns the actual WASM/JS entry point (`int main()`, canvas/worker bootstrap, the
// requestAnimationFrame-equivalent loop registration — Stage P2/D1). Only one `int main()` can
// exist in the final link; it lives in src/entry/, not here.
#ifndef EDEN_SEAM_MAIN_WEB_H
#define EDEN_SEAM_MAIN_WEB_H

namespace eden_web {

// Constructs the (process-lifetime) EdenAppDelegate and calls didFinishLaunching(). Called
// once from src/entry/eden_main.cpp's int main(), analogous to UIApplicationMain() handing
// control to the app delegate after finishing its own setup.
// TODO P1: NSAutoreleasePool wrapping (original main.m:13,17 — `pool = [[NSAutoreleasePool
// alloc] init]` / `[pool release]`) — needs the Foundation shim's pool to actually be
// exercised end-to-end first (P0.1 spike); trivial to add once that's proven.
void eden_seam_main();

// Accessor for src/entry/eden_main.cpp's frame loop (Stage P2/D1) — returns the process-
// lifetime app delegate constructed by eden_seam_main(), or nullptr if not yet called.
class EdenAppDelegate;
EdenAppDelegate* eden_seam_get_app_delegate();

} // namespace eden_web

#endif
