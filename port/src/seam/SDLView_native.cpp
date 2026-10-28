// SDLView_native.cpp — the screen-metric setup the web twin's EAGLView did (Phase N Stage 1,
// WORKING/native-migration-plan-2026-09-04.md). G.2b-5: plain C++; the EAGLView class itself went with
// the last ObjC seam (nothing messaged its framebuffer methods; EdenViewController_native.cpp drives
// eden_native_gl_present directly).
#include "../../../Classes/Input.h"
#include "../../../Classes/Globals.h"
#include "../../../Classes/Constants.h"
#include "gl_context.h"
#include "DisplayProfile.h"

extern "C" void eden_native_gl_present(void);
extern "C" void eden_native_apply_display_mode(void);   // src/seam/DisplayMode_native.cpp

// Establishes the screen-metric globals. Called from main_native.cpp BEFORE the World is constructed,
// because World::World() already reads SCREEN_WIDTH/SCREEN_HEIGHT/P_ASPECT_RATIO through Graphics.
// G.2b-5: the EAGLView class (and G_EAGL_VIEW, which nothing read) is gone; the one thing its -init did
// was this. The name is kept for the callers.
//
// CLAUDE.md convention #3: the screen metrics are globals set by the display module. The engine is
// landscape, so SCREEN_WIDTH is the LONG axis — do not "fix" that apparent inversion. The point space
// itself is DERIVED (window aspect x ui_scale) by DisplayProfile.cpp; this only asks
// DisplayMode_native.cpp to hand it the shape of the real window and fit the drawable to the aspect
// it chose (Phase N Stage 4.3).
extern "C" void eden_seam_create_eagl_view(void) {
    static bool done = false;
    if (done) return;
    done = true;
    eden_native_apply_display_mode();
}

// P.1a: the thermal state (0 nominal, 1 fair, 2 serious, 3 critical), -1 where there is no such
// notion. Apple's is AppleNet_native.mm (the ObjC wall); the rest answer here.
#if !defined(__APPLE__)
extern "C" int eden_native_thermal_state(void) { return -1; }
#endif
