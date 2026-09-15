// EAGLView_web.mm — Stage P1/P2 seam replacement for Classes/EAGLView.mm.
//
// WHY THIS FILE REUSES THE ORIGINAL HEADER (unlike EdenAppDelegate_web/EdenViewController_web
// in this same directory, which are fresh C++): Classes/EAGLView.h is `#import "EAGLView.h"`'d
// (QUOTED include) directly by three non-seam ENGINE files this port must not edit —
// Classes/Globals.mm, Classes/Util.mm, Classes/World.mm (all three just `extern`/define the
// global `EAGLView* G_EAGL_VIEW;`, per grep — see foundation-usage.md). Quoted includes always
// resolve relative to the including file's own directory FIRST (see gl_es1_shim.h's framework
// trampolines for why that's different from angle-bracket `<...>` includes) — so those three
// files will ALWAYS see the real Classes/EAGLView.h, no matter what -I paths this build adds.
// The only way to give `EAGLView` web-appropriate behavior is therefore to provide a NEW
// @implementation of the SAME @interface declared in that untouched header — not to shadow or
// fork the header. This is the pattern to replicate for Texture2D/FileManager/
// SimpleAudioEngine/etc. when their turn comes (P2/P4/P5) — see archive/PORT-STATUS-2026-08-13.md.
//
// STATUS: skeleton only. Every method is a real, minimal implementation or a clearly marked
// TODO; nothing here has been compiled (no emcc on this machine, see archive/PORT-STATUS-2026-08-13.md).
// Framebuffer setup is deferred entirely to Stage P2 ("GL surface + fixed-function shim +
// first frame") — this file's job for P1 is just to make `EAGLView` exist as a type and to
// start the touch-forwarding path into Input.mm (Stage P3) once one exists.

#import "../../../Classes/EAGLView.h"
#import "../../../Classes/Input.h"
#import "../../../Classes/Globals.h"
#import "../../../Classes/Constants.h"
#include "gl_es1_shim.h"
#include "DisplayProfile_web.h"

extern EAGLView* G_EAGL_VIEW; // defined in Classes/Globals.mm — unchanged

// Constructs the one process-lifetime EAGLView and, as a side effect of -init, publishes it as
// G_EAGL_VIEW and establishes the screen-metric globals. Called from src/seam/main_web.cpp
// BEFORE the World is constructed, because World::World() already reads SCREEN_WIDTH/
// SCREEN_HEIGHT/P_ASPECT_RATIO through Graphics.
//
// A plain C entry point because every caller in the seam is plain C++ (EdenAppDelegate_web,
// main_web, eden_main) and none of them should need the ObjC frontend just to kick this off.
//
// Deliberately never released: on iOS the nib owned this view for the process lifetime, and
// G_EAGL_VIEW is a raw global the engine dereferences from several files (Util.mm, World.mm,
// SharedList.mm) with no ownership protocol at all. CLAUDE.md #6 (manual retain/release) is
// satisfied by matching the original's lifetime, not by inventing a release the engine would
// then read through.
extern "C" void eden_seam_create_eagl_view(void) {
    if (G_EAGL_VIEW) return;
    (void)[[EAGLView alloc] init];
}

@implementation EAGLView

@dynamic context;

+ (Class)layerClass {
    // TODO P2: on real iOS this backs the view with a CAEAGLLayer. On web there is no CALayer
    // — the WebGL2 context is created directly against an HTML <canvas> (or an
    // OffscreenCanvas transferred into the worker per plan D1) in src/entry/eden_main.cpp, not
    // through this class at all. Returning nil rather than a real class is intentional: this
    // codepath should never actually run in the web build (nothing calls +layerClass without
    // a real UIKit view-backing-store mechanism, which doesn't exist here).
    return nil;
}

- (id)initWithCoder:(NSCoder *)coder {
    // Nib/storyboard loading has no web equivalent — src/entry/eden_main.cpp allocs this view
    // directly instead of unarchiving one, so this is just the designated initializer.
    (void)coder;
    self = [super init];
    if (self) [self establishScreenMetrics];
    return self;
}

- (id)init {
    self = [super init];
    if (self) [self establishScreenMetrics];
    return self;
}

// The screen-metric globals (CLAUDE.md convention #3: "Screen metrics are globals set in
// `EAGLView initWithCoder:`"). THIS IS LOAD-BEARING AND WAS MISSING: the original sets
// SCREEN_WIDTH/SCREEN_HEIGHT/IS_WIDESCREEN/P_ASPECT_RATIO here (Classes/EAGLView.mm:122-142)
// from [UIScreen mainScreen], and the web seam replaced that method without replacing the
// assignments — leaving every one of them at zero. Nothing crashes on a zero SCREEN_WIDTH; the
// projection matrix just comes out degenerate and the frame renders empty, which is precisely
// the "subtly wrong frame rather than a crash" failure mode RESUME-HERE warns about for P2.
//
// The engine is landscape, so SCREEN_WIDTH is the LONG axis and SCREEN_HEIGHT the short one — do
// not "fix" that apparent inversion, Util.mm's takeScreenshot reads them the same way round.
//
// NO LONGER PINNED (audit rows D1 + D4, 2026-07-31). This method used to hard-code the original's
// iPhone-5 widescreen branch — 568x320 points, IS_WIDESCREEN/IS_IPAD/IS_RETINA all TRUE. All of
// that arithmetic moved to src/seam/DisplayProfile_web.mm, which derives the point space from the
// real window aspect and a UI-scale setting instead, with the pinned 568x320 still reachable (and
// still the touch profile's default) as "Classic 16:9 + ui_scale 200%". Read that file's header
// before changing anything here; in particular the retina/scale flags are NOT what D1 unpins and
// are still set unconditionally, because SCALE_WIDTH/SCALE_HEIGHT are DIVISORS in layout math
// (Menu.mm:39 `591/SCALE_WIDTH`, Hud.mm `128/SCALE_WIDTH`, Input.mm `point.x/SCALE_WIDTH`) and were
// once the source of a silent inf/nan-geometry bug when this seam left them at 0.
//
// The page calls eden_display_set_viewport() from onRuntimeInitialized — which fires BEFORE main()
// — so by the time this runs the real canvas box is usually already known. When it is not (headless
// `node eden.js`, or a boot with no layout yet) the classic aspect is used and the page's first
// resize corrects it through the re-layout path.
- (void)establishScreenMetrics {
    eden_display_refresh();

    G_EAGL_VIEW = self;

    // Make the DRAWABLE a real retina surface: points * contentScaleFactor. The engine keeps
    // projecting in POINTS (SCREEN_WIDTH/HEIGHT above), and the viewport maps that onto the full
    // pixel drawable — exactly as Classes/EAGLView.mm -createFramebuffer sized its renderbuffer
    // at contentsScale 2 and then glViewport'd the framebuffer's PIXEL dimensions. The context
    // is already live at this point (eden_main.cpp creates it before eden_seam_main), so this
    // resizes the existing canvas rather than waiting for a createFramebuffer that the web boot
    // path never calls. contentScaleFactor is 2 for retina (NOT SCALE_WIDTH, which is a separate
    // layout scalar that happens to also be 2 on this profile but is 2.13/2.4 on a real iPad).
    //
    // Perf-audit item #6: this is now the BOOT/fallback size only. public/eden-st.html's
    // applyDrawableSize() re-sizes the drawable from the real CSS box x min(devicePixelRatio,
    // dpr_cap) x render_scale as soon as the runtime is up (and on every resize after that), via
    // the same eden_gl_context_set_drawable_size path. This line still matters: it is what the
    // headless build and the pre-first-layout frames get, and the engine's point space below is
    // still the only thing that decides the projection.
    const float contentScaleFactor = IS_RETINA ? 2.0f : 1.0f;
    eden_gl_context_set_drawable_size((int)(SCREEN_WIDTH  * contentScaleFactor),
                                      (int)(SCREEN_HEIGHT * contentScaleFactor));
}

// @dynamic context (above) means we supply these accessors manually, backed by the `context`
// ivar Classes/EAGLView.h already declares (`@private EAGLContext *context;`) — no new ivar
// needed, just don't shadow the setter's `context` parameter name against it.
- (EAGLContext *)context {
    return context;
}

- (void)setContext:(EAGLContext *)newContext {
    if (context != newContext) {
        [context release];
        context = [newContext retain];
    }
}

- (void)setFramebuffer {
    eden_gl_context_bind_default_framebuffer();
}

- (BOOL)presentFramebuffer {
    // WebGL presents implicitly when the animation-frame callback returns — there is no
    // swap-buffers call to make. Kept as a distinct method for symmetry with the original
    // drawFrame() call shape (Classes/EdenViewController.mm:206, replaced).
    return YES;
}

- (void)createFramebuffer {
    // Idempotent (eden_gl_context_create returns early if a context is already live), so the
    // retina-swap path can call delete-then-create without a special case. Size 0x0 means "take
    // the canvas element's current size" — the page owns the layout, not this code.
    eden_gl_context_create(0, 0);
}

- (void)deleteFramebuffer {
    eden_gl_context_destroy();
}

// ---- Touch forwarding (Stage P3) ----
// The original (Classes/EAGLView.mm:156-167) is a pure pass-through into Input.mm, which is
// an ENGINE file this port keeps as-is (per CLAUDE.md/plan: "Input.mm, Joystick.mm (input
// tracker — remap events INTO these)"). This file's job is only to construct UITouch/UIEvent
// instances (uikit_stubs.h, see web/src/shim/foundation) from whatever the actual web input
// source is (Pointer Events, per Stage P3) and forward them here unchanged. The web input
// source itself (pointerdown/pointermove/pointerup/pointercancel listeners) is NOT written
// yet — TODO P3, see web-port-plan.md Stage P3 ("Map Pointer/Touch events -> the 5-slot
// Input.mm tracker").
- (void)touchesBegan:(NSSet *)touches withEvent:(UIEvent *)event {
    Input::getInput()->touchesBegan(touches, event);
}
- (void)touchesMoved:(NSSet *)touches withEvent:(UIEvent *)event {
    Input::getInput()->touchesMoved(touches, event);
}
- (void)touchesEnded:(NSSet *)touches withEvent:(UIEvent *)event {
    Input::getInput()->touchesEnded(touches, event);
}
- (void)touchesCancelled:(NSSet *)touches withEvent:(UIEvent *)event {
    // Folds in audit finding L5 (WORKING/audit-report.md, absorbed by Stage P3 per the plan's
    // Phase A table: "L5 input... Fixed in P3") — TODO P3: verify Input::touchesCancelled
    // clears more than just `mtouches` once that file is revisited; not this pass's job to
    // patch Input.mm itself (kept untouched, per CLAUDE.md).
    Input::getInput()->touchesCancelled(touches, event);
}

- (void)dealloc {
    [context release];
    [super dealloc];
}

@end
