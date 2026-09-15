// DisplayProfile_web.h — the port's device PROFILE and its derived display metrics.
//
// Audit rows D1 ("unpin the display constants") and D4 ("one build, two profiles"), implemented
// together because neither is much use alone: D1 is what makes the point space a variable, D4 is
// what decides which value it takes without forking the build.
//
// WHAT A "PROFILE" IS (D4). Two of them — `desktop` and `touch` — each a row of DEFAULTS, not a
// code path. The profile is auto-detected once (it follows `eden_effective_input_is_touch()`, which
// is the existing Auto/Touch/Keyboard+Mouse arbitration in Settings_web.mm, so there is exactly one
// detector and one user override for both concepts) and every field it carries is a default for a
// setting the player can still change by hand. Nothing branches on the profile at run time; it only
// seeds `kSettings[]` rows that had no stored value. That is the whole point of the row: keep the
// PC/mobile divergence in data, in the seam.
//
// WHAT "UNPINNED" MEANS (D1). `SCREEN_WIDTH`/`SCREEN_HEIGHT` are the engine's LAYOUT coordinate
// system ("points"), not its resolution — all 114 `IS_IPAD`/`IS_RETINA` sites and every HUD/menu
// rect are expressed in them, and the pixel buffer has been decoupled from them since the dynamic
// drawable landed. Pinning them to 568x320 therefore did not pin the resolution, it pinned the UI
// DENSITY: on a 2560x1440 window every button was drawn 4.5x its design size. So:
//
//     SCREEN_HEIGHT = kBasePointHeight * 100 / ui_scale_pct     <- a user/profile choice
//     SCREEN_WIDTH  = round_even(SCREEN_HEIGHT * aspect)        <- the real window's aspect
//     P_ASPECT_RATIO = SCREEN_WIDTH / SCREEN_HEIGHT             <- ALWAYS recomputed (see below)
//
// A wider window therefore shows MORE WORLD at the same UI size, rather than the same world with
// bigger buttons, and `ui_scale` is the one knob that moves UI size. At ui_scale 200% + the classic
// aspect this reproduces 568x320 exactly, byte for byte — that is the touch profile's default and
// the reason the shipped mobile layout is unchanged.
//
// THE STOCK BUG NOT REPRODUCED HERE: `Classes/EAGLView.mm:138-143` only recomputes P_ASPECT_RATIO
// inside `if(IS_WIDESCREEN)`; the non-widescreen branch silently keeps an iPad 4:3 default that
// never matched its own 480x320 point space. This file always derives it. (iPad's own 4:3 layout is
// NOT resurrected — that branch, `EAGLView.mm:114-119`, is commented out in the original and never
// drove a live layout on any device, so there is no tested branch to bring back. See
// `WORKING/archive/aspect-ratio-toggle-scope.md`.)
//
// ORDER IS LOAD-BEARING at boot: `eden_display_set_viewport()` must be called before
// `eden_seam_create_eagl_view()` (i.e. from the page's `onRuntimeInitialized`, which fires before
// `main()`), because `World::World()` reads the metrics. It is safe to call later too — anything
// past construction goes through the re-layout path below.
#ifndef EDEN_DISPLAY_PROFILE_WEB_H
#define EDEN_DISPLAY_PROFILE_WEB_H

#ifdef __cplusplus
extern "C" {
#endif

enum { EDEN_PROFILE_DESKTOP = 0, EDEN_PROFILE_TOUCH = 1, EDEN_PROFILE_LOWMEM = 2 };

// Every field is an INDEX into the option list of the identically-named row in Settings_web.mm's
// kSettings[], except `touch_chrome`. Indexes rather than values so the profile table and the
// settings schema cannot disagree about what "60 fps" or "1.5x" means — the schema owns that
// mapping already and this table only picks a row from it.
typedef struct EdenProfile {
    const char* name;
    int ui_scale;        // "Auto,100%,125%,150%,200%"        — UI density (see the formula above)
    int display_layout;  // "Auto,Classic 16:9,Adaptive"       — where the aspect comes from
    int fps_cap;         // "Uncapped,30,45,60"
    int dpr_cap;         // "1x,1.5x,2x"
    int render_scale;    // "50%,75%,100%,125%"
    int touch_chrome;    // 1 = draw the on-screen joystick / jump / crouch buttons
} EdenProfile;

// EDEN_PROFILE_DESKTOP or EDEN_PROFILE_TOUCH, following the input-mode arbitration. Never returns
// EDEN_PROFILE_LOWMEM — that one is not an input profile, it is an orthogonal "this device is short
// on RAM" overlay (ROADMAP Phase M / M5) that only supplies the three video-preset fields and is
// applied on top of whichever input profile is active. Reach it with eden_profile_get() directly.
int eden_active_profile(void);
const EdenProfile* eden_profile_get(int id);
const EdenProfile* eden_profile_active(void);

// Low-memory device overlay (ROADMAP Phase M / M5.2). Set by the page BEFORE eden_settings_init()
// runs — from public/eden-host.js, when navigator.deviceMemory is low, when a prior threaded-build
// load-failure downgrade is remembered in localStorage, or ?lowmem=1. While on, Settings_web.mm's
// profile-default seeder takes dpr_cap / render_scale (and fps_cap if set) from
// kProfiles[EDEN_PROFILE_LOWMEM] instead of the input profile's, and the engine refuses to open a
// 256-block-tall world (World::loadWorld -> eden_report_load_failure). Player overrides still win —
// this only changes the *default* for a row the player has never touched.
void eden_set_low_memory(int on);
int  eden_low_memory(void);

// The page's real canvas/viewport box, in CSS pixels. Only the RATIO is used (the pixel buffer is
// sized separately by eden_set_drawable_size). Re-derives the metrics and re-lays-out the UI.
void eden_display_set_viewport(int css_w, int css_h);

// Re-derive the metrics from the current viewport + settings and re-lay-out. Call after anything
// that could change ui_scale / display_layout / the active profile.
void eden_display_refresh(void);

// The derived point space, for the page's DOM-event -> point-space conversion (toEnginePoint) and
// for anything else that used to hard-code 568x320.
int   eden_display_point_width(void);
int   eden_display_point_height(void);
// SCREEN_WIDTH/SCREEN_HEIGHT as a ratio, x1000 (an int so it crosses the wasm boundary without a
// float export). The page letterboxes the canvas to this — in Adaptive mode it equals the window's
// own aspect and the letterbox is a no-op, but the clamp below can make them differ.
int   eden_display_aspect_x1000(void);

// Does the active profile want the on-screen joystick / jump / crouch chrome? A profile FIELD, so
// there is one answer rather than a `use_joystick` here and a CSS rule there.
int   eden_profile_touch_chrome(void);
// The active profile's name ("desktop" / "touch"), for the debug JSON and settings hints.
const char* eden_profile_name(void);

#ifdef __cplusplus
}
#endif

#endif // EDEN_DISPLAY_PROFILE_WEB_H
