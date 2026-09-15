// ---------------------------------------------------------------------------------------------
// Phase N Stage 1 (WORKING/native-migration-plan-2026-09-04.md): this one stays THIS PORT'S OWN on
// every target, including native macOS where a real framework of the same name exists. Two
// measured reasons, both worth knowing before "improving" it into an #include_next:
//   * <AudioToolbox/...> and <AVFoundation/...> transitively pull in CoreServices, whose
//     CarbonCore/AIFF.h defines `ChunkHeader` — and so does Classes/FileManager.h, the engine's
//     on-disk chunk record. That is a typedef redefinition in every engine TU that reaches the
//     audio headers, i.e. most of them, reported ~5 includes deep from anything this project owns.
//   * <QuartzCore/QuartzCore.h> on macOS has no CAEAGLLayer, which is the only thing Classes/
//     Util.mm reaches it for.
// Native audio is stubbed for Stage 1 (native/src/seam/seam_link_stubs_native.mm) and Stage 2 gives
// it a real backend behind SimpleAudioEngine.h's own ~13-call contract — so nothing is lost by
// these headers only having to PARSE here.
// <QuartzCore/QuartzCore.h> — imported by Classes/Util.mm (and by the seam-excluded
// EdenViewController.mm, for CADisplayLink).
//
// Util.mm imports it for CoreGraphics, not for Core Animation: grepping it for `CA*` symbols
// finds nothing (the one apparent hit is the word "CALE" inside an identifier). On iOS,
// <QuartzCore/QuartzCore.h> transitively drags in CoreGraphics, which is what Util.mm's
// screenshot path actually uses — CGImageCreate, CGDataProviderCreateWithData,
// CGColorSpaceCreateDeviceRGB, and the UIGraphics* image-context calls.
//
// So this forwards to the CoreGraphics declarations rather than declaring any CA layer types.
// CADisplayLink, the one genuine Core Animation dependency, lives only in the seam files this
// port replaces outright (src/seam/EdenViewController_web.cpp drives the loop from
// requestAnimationFrame instead), so it is deliberately absent here.
#ifndef EDEN_TRAMPOLINE_QUARTZCORE_H
#define EDEN_TRAMPOLINE_QUARTZCORE_H
#import <UIKit/UIKit.h>
#endif
