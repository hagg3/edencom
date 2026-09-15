// SDLView_native.mm — native twin of web/src/seam/EAGLView_web.mm (Phase N Stage 1,
// WORKING/native-migration-plan-2026-09-04.md).
//
// Same structural constraint as the web seam, for the same reason: Classes/EAGLView.h is
// `#import "EAGLView.h"`'d (QUOTED) by three engine files that just want the global
// `EAGLView* G_EAGL_VIEW` — Globals.mm, Util.mm, World.mm — and a quoted include always resolves
// against the including file's own directory first, so those three will always see the real
// header. The only way to give EAGLView platform-appropriate behaviour is therefore a NEW
// @implementation of the SAME @interface. Read EAGLView_web.mm's header comment before changing
// anything here; this file is deliberately its near-copy so the two stay diffable.
//
// WHAT DIFFERS FROM THE WEB TWIN, and only this:
//   * -createFramebuffer/-deleteFramebuffer forward to the native context functions, which are
//     backed by an SDL window rather than a canvas selector.
//   * -presentFramebuffer really presents. WebGL presents implicitly when the rAF callback
//     returns; SDL_GL_SwapWindow does not, so this method is load-bearing here where on web it is
//     a comment.
// Everything else — the screen-metric globals, the retina drawable sizing, the touch forwarding —
// is the web file verbatim, because none of it was ever browser-specific.

#import "../../../Classes/EAGLView.h"
#import "../../../Classes/Input.h"
#import "../../../Classes/Globals.h"
#import "../../../Classes/Constants.h"
#include "gl_es1_shim.h"
#include "DisplayProfile_web.h"

extern EAGLView* G_EAGL_VIEW;  // defined in Classes/Globals.mm — unchanged

extern "C" void eden_native_gl_present(void);
extern "C" void eden_native_apply_display_mode(void);   // src/seam/DisplayMode_native.cpp

// Constructs the one process-lifetime EAGLView and, as a side effect of -init, publishes it as
// G_EAGL_VIEW and establishes the screen-metric globals. Called from main_native.cpp BEFORE the
// World is constructed, because World::World() already reads SCREEN_WIDTH/SCREEN_HEIGHT/
// P_ASPECT_RATIO through Graphics. Plain C entry point for the same reason as the web twin: every
// caller in the seam is plain C++.
//
// Deliberately never released — matches the original's process-lifetime nib ownership, and
// G_EAGL_VIEW is a raw global the engine dereferences with no ownership protocol at all.
extern "C" void eden_seam_create_eagl_view(void) {
    if (G_EAGL_VIEW) return;
    (void)[[EAGLView alloc] init];
}

@implementation EAGLView

@dynamic context;

+ (Class)layerClass {
    // No CALayer off iOS. Nothing calls +layerClass without a real UIKit view-backing-store
    // mechanism, which does not exist here — returning nil is the honest answer, same as web.
    return nil;
}

- (id)initWithCoder:(NSCoder *)coder {
    (void)coder;   // no nib/storyboard — main_native.cpp allocs this view directly
    self = [super init];
    if (self) [self establishScreenMetrics];
    return self;
}

- (id)init {
    self = [super init];
    if (self) [self establishScreenMetrics];
    return self;
}

// CLAUDE.md convention #3: the screen metrics are globals set here. The engine is landscape, so
// SCREEN_WIDTH is the LONG axis — do not "fix" that apparent inversion, Util.mm's takeScreenshot
// reads them the same way round. The point space itself is DERIVED (window aspect x ui_scale) by
// DisplayProfile_web.mm, which is shared with the web target; this method only asks it to refresh
// and then sizes the drawable to match.
- (void)establishScreenMetrics {
    G_EAGL_VIEW = self;

    // Phase N Stage 4.3: hand the display module the shape of the real window BEFORE asking it to
    // derive anything, then fit the drawable to the aspect it chose. That is the whole of
    // src/seam/DisplayMode_native.cpp, and it replaces the two lines that used to live here:
    //
    //     const float contentScaleFactor = IS_RETINA ? 2.0f : 1.0f;
    //     eden_gl_context_set_drawable_size(SCREEN_WIDTH * f, SCREEN_HEIGHT * f);
    //
    // Those sized the drawable to the POINT SPACE times two — i.e. they made the framebuffer follow
    // the engine's idea of the screen instead of the other way round. On a 1136x640 desktop window
    // the two agree and nothing showed; on a 4:3 iPad they do not, and the first device pass found
    // every consequence at once (see this file's twin for the list). eden_display_refresh() is not
    // called here any more either — eden_display_set_viewport() applies on its own.
    eden_native_apply_display_mode();
}

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
    // The one method that does more here than on web: a desktop GL context has to be told to
    // swap. (Called from EdenViewController_native::drawFrame, not from here.)
    eden_native_gl_present();
    return YES;
}

- (void)createFramebuffer {
    // Idempotent — eden_gl_context_create returns early if a context is live. 0x0 means "keep the
    // window's current size".
    eden_gl_context_create(0, 0);
}

- (void)deleteFramebuffer {
    eden_gl_context_destroy();
}

// ---- Touch forwarding ----
// Pure pass-through into Classes/Input.mm, exactly as the original and the web twin. The native
// event source (SDL mouse/keyboard/gamepad -> the eden_* input exports) is Stage 2's job; these
// four exist so the type is complete and so an SDL translator has somewhere to deliver to.
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
    Input::getInput()->touchesCancelled(touches, event);
}

- (void)dealloc {
    [context release];
    [super dealloc];
}

@end
