// DisplayMode_native.cpp — the native twin of `public/eden-viewport.js`'s applyDisplayMode()
// (Phase N Stage 4.3, 2026-09-06).
//
// WHY THIS FILE EXISTS, AND WHAT ITS ABSENCE COST. The engine's point space is DERIVED — from a
// profile (desktop/touch), a UI scale, and the shape of the box it is being drawn into — by
// web/src/seam/DisplayProfile_web.mm, which both targets compile. That file needs one input from
// the host: `eden_display_set_viewport(w, h)`, "here is the box you have". **No native code ever
// called it.** Its own header says the call is load-bearing and must happen before the view is
// created; the desktop legs got away with it for three stages because their default window is
// 1136x640, which is exactly the classic 16:9 aspect the profile falls back to.
//
// An iPad Air 2 is 4:3, and the first device pass reported precisely what that produces: a HUD
// "slightly horizontally squashed", settings switches that "don't respond to touch at all", a
// joystick that is "very slow and buggy", and mine mode doing nothing. One cause. eden-viewport.js
// already had the sentence explaining it, about its own letterbox fit:
//
//     "…which is what keeps the picture undistorted, since findWorldCoords's raycast (Util.mm) and
//      gluPerspective(..., P_ASPECT_RATIO, ...) both assume the rendered box IS the engine's
//      aspect. A distorted box stretches the scene AND misaims every click."
//
// So this file does what that function does, in the same order, for the same reasons:
//   1. hand the engine the shape of the box in POINTS (the window size — the CSS-pixel analogue),
//      so an Adaptive layout can derive its aspect from it and a Classic one can keep 16:9;
//   2. read the aspect the engine settled on;
//   3. fit a box of that aspect inside the real PIXEL framebuffer, centred, and give the GL
//      context that box — which is the letterbox the web build gets for free from CSS.
//
// The input translator subtracts the same origin (native/src/seam/Input_native.cpp), so a touch
// and the pixel it lands on agree by construction rather than by coincidence.
//
// WHAT THIS DELIBERATELY DOES NOT DO: choose a layout. Classic-vs-Adaptive is a SETTING
// (`display_layout`, and the touch profile's default is Classic because that is what shipped on
// web and what a touch player is used to). On a 4:3 screen Classic means bars, and bars are the
// honest answer for a layout whose HUD rects are authored for 16:9 — the alternative, stretching,
// is the bug this file fixes. A player who prefers the wider view can set Adaptive in the settings
// screen and this code will fit the box to it with no bars at all.

#include <SDL3/SDL.h>

#include <cstdio>

#include "DisplayProfile_web.h"     // eden_display_set_viewport / eden_display_aspect_x1000

extern "C" {

SDL_Window* eden_native_gl_window(void);
void eden_native_gl_set_letterbox(int x, int y, int w, int h);
void eden_gl_context_get_drawable_size(int* width, int* height);

// Called at boot from SDLView_native.mm's -establishScreenMetrics (before World::World() reads the
// metrics — the ordering DisplayProfile_web.h requires) and again on every window size change.
void eden_native_apply_display_mode(void) {
    SDL_Window* win = eden_native_gl_window();
    if (!win) {
        // Headless: there is no box, so there is nothing to tell the engine about one. The profile
        // keeps its classic fallback, which is what every scripted mode has always run with — and
        // what the recorded geometry checksums were taken under. Do not "improve" this into a
        // synthetic box; it would change nothing about meshing and everything about the gates.
        return;
    }

    // (1) POINTS, not pixels. SDL_GetWindowSize is the point/logical size — the same space the DOM
    // reports CSS pixels in — and the point space the engine derives is a UI space, not a
    // framebuffer. Using pixels here would double the derived size on every retina display.
    int ww = 0, wh = 0;
    SDL_GetWindowSize(win, &ww, &wh);
    if (ww > 0 && wh > 0) eden_display_set_viewport(ww, wh);

    // (2) What the engine settled on. It is not necessarily the window's aspect: Classic pins
    // 16:9, and DisplayProfile clamps extreme shapes rather than hand its absolutely-sized HUD
    // rects a box they cannot fill.
    const float aspect = (float)eden_display_aspect_x1000() / 1000.0f;
    if (aspect <= 0.0f) return;

    // (3) The real framebuffer, in pixels, and the largest box of the engine's aspect that fits
    // inside it, centred.
    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(win, &pw, &ph);
    if (pw <= 0 || ph <= 0) eden_gl_context_get_drawable_size(&pw, &ph);
    if (pw <= 0 || ph <= 0) return;

    int bw = pw;
    int bh = (int)((float)pw / aspect + 0.5f);
    if (bh > ph) { bh = ph; bw = (int)((float)ph * aspect + 0.5f); }
    if (bw < 1) bw = 1;
    if (bh < 1) bh = 1;

    eden_native_gl_set_letterbox((pw - bw) / 2, (ph - bh) / 2, bw, bh);
}

}  // extern "C"
