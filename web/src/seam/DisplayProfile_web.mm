// DisplayProfile_web.mm — implementation of the profile + derived display metrics.
// See DisplayProfile_web.h for what a "profile" is, what "unpinned" means, and why both audit rows
// (D1 and D4) land in one file.
//
// WHY THIS IS SEAM CODE AND NOT ENGINE CODE. The split the audit draws is: platform DETECTION stays
// in the seam, layout GENERALISATION goes in the engine. So the two engine-side changes this file
// depends on — `Hud::layoutForScreen()` / `Menu::layoutForScreen()` (rect math lifted out of the
// constructors so it can run more than once) and `Input::screenMetricsChanged()` — live in
// `Classes/`, and everything about *what the numbers should be on a browser* lives here.
//
// THE SIDE-EFFECT RULE (this port's most-bitten class, PORT-STATUS "a seam replacement owes the
// SIDE EFFECTS of what it replaced"): the globals written by eden_display_apply() below are the
// complete set the original `Classes/EAGLView.mm -initWithCoder:` wrote — SCREEN_WIDTH,
// SCREEN_HEIGHT, IS_WIDESCREEN, P_ASPECT_RATIO, IS_IPAD, IS_RETINA, SUPPORTS_RETINA, SCALE_WIDTH,
// SCALE_HEIGHT, G_EAGL_VIEW — minus G_EAGL_VIEW, which EAGLView_web.mm still owns because it is the
// view pointer, not a metric.
#import "DisplayProfile_web.h"

// Before the engine headers: Globals.h declares `const GLubyte blockColor[]` without including a GL
// header of its own, so it only compiles behind something that has already defined the GL types.
#include "gl_es1_shim.h"
#import "../../../Classes/Globals.h"
#import "../../../Classes/Constants.h"
#import "../../../Classes/World.h"
#import "../../../Classes/Hud.h"
#import "../../../Classes/Menu.h"
#import "../../../Classes/Input.h"
#include "../shim/foundation/platform_shims.h"   // EDEN_EXPORT (Phase N Stage 1)
#include <cstdio>
#include <cmath>

// Declared in Classes/Globals.mm but NOT in Globals.h — the original EAGLView.mm:34 externs it
// locally, so this file does the same (same reasoning as EAGLView_web.mm's copy).
extern bool IS_WIDESCREEN;

// Owned by Settings_web.mm, in the plain-mutable-global style every other port-owned setting there
// uses. Both are stored as the ENUM INDEX of their kSettings[] row, and index 0 is "Auto" for both,
// which is what makes the profile a live default rather than a one-shot write: leaving them on Auto
// means changing the input mode re-resolves them. Their compiled defaults are Auto, so this file
// behaves correctly during the frames before eden_settings_init() has run.
extern float eden_ui_scale;
extern float eden_display_layout;

// ---------------------------------------------------------------------------------------------
// The profile table (audit D4). Two rows of DEFAULTS. Adding a third profile is adding a row.
// ---------------------------------------------------------------------------------------------
//
// DESKTOP defaults, and why each one:
//   ui_scale 125%     — halves the on-screen size of every HUD element versus the pinned profile
//                       (a 512-point-tall layout space instead of 320) without going all the way to
//                       100%, which is legible on a 27" display but small on a 13" laptop window.
//                       The row exists precisely because that is a taste call.
//   layout  Adaptive  — the whole point of D1 on a resizable window: no letterboxing, and a wider
//                       window shows more world.
//   fps/dpr/scale     — unchanged from the compiled defaults; desktop has no thermal budget to
//                       protect and "as sharp as the display allows" is the right default there.
//
// TOUCH defaults were deliberately EXACTLY what this port shipped before this row landed —
// ui_scale 200% + Classic aspect reproduces 568x320 to the point, and fps_cap 60 / dpr_cap 1.5x are
// the values the pre-existing eden_apply_profile_defaults() already wrote. That was the audit's own
// mitigation ("keep the pinned profile as the default until the new path is verified on a real
// phone") expressed as data rather than as a flag: the unexercised path was opt-in on touch.
//
// **`layout` IS NOW ADAPTIVE ON TOUCH TOO (2026-09-07), and the verification the mitigation was
// waiting for is what changed it.** A real iPad Air 2 played the native build and reported: "the
// letterbox fix locked the display to 16:9, so a 4:3 iPad now plays with black bars top and
// bottom" (STATUS §2.0.12, bug 4). Pinning the aspect was never about the aspect — it was about
// not trusting a layout the engine's absolutely-sized rects had never been laid out in. So the
// pin is replaced by the thing it was standing in for: the CLASSIC-WIDTH FLOOR in
// eden_display_apply() below, which guarantees the point space is never NARROWER than the 568x320
// the art was drawn for, on any aspect between kMinAspect and kMaxAspect. With that floor, a 4:3
// screen gets a 568x426 point space — full-bleed, undistorted, and MORE vertical world rather than
// two black bars. A player who wants the shipped framing still has the Classic row in Settings.
//
// LOW-MEM is not a third INPUT profile — it is the ROADMAP Phase M / M5.2 overlay. Only its
// fps_cap / dpr_cap / render_scale columns are ever read (by Settings_web.mm's seeder, when
// eden_low_memory() is set); ui_scale / layout / touch_chrome still come from the desktop-or-touch
// input profile underneath. The values: dpr_cap 1x (index 0) and render_scale 75% (index 1) drop
// the drawing-buffer / compositor cost M0 measured at ~30-45 MB on a 2 GB device, and fps_cap 45
// (index 2) trims the sustained GPU load that pairs with it. Adding this row is exactly the
// extension shape this comment block describes.
static const EdenProfile kProfiles[3] = {
    // name       ui_scale  layout  fps_cap  dpr_cap  render_scale  touch_chrome
    {  "desktop",        2,      2,       0,       2,            2,            0 },
    {  "touch",          4,      2,       3,       1,            2,            1 },
    {  "low-mem",        4,      1,       2,       0,            1,            1 },
};

// ROADMAP Phase M / M5.2 — the low-memory overlay flag. Plain mutable global in the same style as
// Settings_web.mm's port-owned settings; read only through eden_low_memory().
static bool g_low_memory = false;

// Option-list indexes, spelled out so the arithmetic below never contains a bare literal that has
// to be cross-referenced against Settings_web.mm's schema strings.
enum { UI_SCALE_AUTO = 0 };
static const int kUiScalePct[] = { 0 /*Auto*/, 100, 125, 150, 200 };
enum { LAYOUT_AUTO = 0, LAYOUT_CLASSIC = 1, LAYOUT_ADAPTIVE = 2 };

// The point space at ui_scale 100%. 640 rather than 320 so that 200% — the profile the engine was
// written for — lands exactly on the stock 320-point height with no rounding.
static const float kBasePointHeight = 640.0f;

// Guard rails on the derived numbers. The aspect clamp is what keeps a portrait phone or a 32:9
// monitor from producing a layout the engine's absolutely-sized rects cannot fill sensibly; the
// page letterboxes to eden_display_aspect_x1000() when the clamp bites, so the picture stays
// undistorted either way. The width/height clamps are belt-and-braces against a garbage viewport
// (a 0-height box during a fullscreen transition, say) reaching the projection math.
static const float kMinAspect = 1.20f;
static const float kMaxAspect = 2.40f;
static const int   kMinPointW = 320,  kMaxPointW = 1600;
static const int   kMinPointH = 240,  kMaxPointH = 800;

// THE DENSITY FLOOR — one engine point may never be smaller than one CSS pixel.
//
// `ui_scale` alone gives a FIXED point space, which means the UI is a fixed FRACTION of the canvas:
// shrink the window and every HUD icon shrinks with it. That is right on a desktop monitor (the
// game gets bigger, the HUD grows with it) and wrong the moment the window gets small — resize a
// desktop browser to phone proportions and the 45-point mode buttons come out at 35 CSS px and
// keep going. Reported from live play 2026-07-31, and it is not a touch-profile problem: nothing
// about resizing a mouse-driven window flips `pointer: coarse`, so the desktop profile's 512-point
// space is still in force and still correct — it just needs a floor.
//
// One CSS pixel per point is not an arbitrary floor: it is EXACTLY the density the engine's
// absolutely-sized art was drawn for. On the iPhone 5 the viewport was 568x320 CSS pixels and the
// point space was 568x320 — UIKit points ARE CSS pixels. So this says "never render the HUD denser
// than the device it was designed on", which is also what makes a phone-shaped window degrade into
// something close to the classic layout instead of a miniature of the desktop one.
static const float kMinCssPxPerPoint = 1.0f;

// The width the engine's absolutely-sized HUD/menu rects were drawn against (the iPhone 5's 568
// points). Used as a FLOOR on the derived point width — see eden_display_apply().
static const int kClassicPointW = IPHONE5_WIDTH;

// The page's real box, in CSS pixels. 0 until the page reports one; until then the classic aspect
// is used, which is also what the headless build (`node eden.js`, no DOM) runs on forever.
static int g_cssW = 0;
static int g_cssH = 0;

// Last-applied point space, so a no-op refresh does not re-run the engine's layout.
static int g_pointW = 0;
static int g_pointH = 0;

// Settings_web.mm owns the input-mode arbitration; the profile follows it rather than detecting
// anything a second time.
extern "C" int eden_effective_input_is_touch(void);

// ---------------------------------------------------------------------------------------------

extern "C" int eden_active_profile(void) {
    return eden_effective_input_is_touch() ? EDEN_PROFILE_TOUCH : EDEN_PROFILE_DESKTOP;
}

extern "C" const EdenProfile* eden_profile_get(int id) {
    if (id != EDEN_PROFILE_TOUCH && id != EDEN_PROFILE_LOWMEM) id = EDEN_PROFILE_DESKTOP;
    return &kProfiles[id];
}

extern "C" EDEN_EXPORT void eden_set_low_memory(int on) { g_low_memory = (on != 0); }
extern "C" EDEN_EXPORT int eden_low_memory(void) { return g_low_memory ? 1 : 0; }

extern "C" const EdenProfile* eden_profile_active(void) {
    return eden_profile_get(eden_active_profile());
}

static int eden_resolved_ui_scale_pct(void) {
    int idx = (int)lroundf(eden_ui_scale);
    if (idx == UI_SCALE_AUTO) idx = eden_profile_active()->ui_scale;
    if (idx < 1 || idx >= (int)(sizeof(kUiScalePct) / sizeof(kUiScalePct[0])))
        idx = kProfiles[EDEN_PROFILE_TOUCH].ui_scale;   // stock 200% is the safe fallback
    return kUiScalePct[idx];
}

static int eden_resolved_layout(void) {
    int idx = (int)lroundf(eden_display_layout);
    if (idx == LAYOUT_AUTO) idx = eden_profile_active()->display_layout;
    return (idx == LAYOUT_ADAPTIVE) ? LAYOUT_ADAPTIVE : LAYOUT_CLASSIC;
}

// Rounds to an EVEN integer: every 2D pass projects through `glOrthof(0, SCREEN_WIDTH*2, ...)`
// (Graphics.mm's IS_IPAD && IS_RETINA branch), and an odd point dimension there puts the ortho
// edge on a half-pixel of the retina-doubled space for no benefit.
static int eden_round_even(float v) {
    int n = (int)(v / 2.0f + 0.5f) * 2;
    return n;
}

static int eden_clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// The one place the metrics are written. Everything else in this file funnels here.
static void eden_display_apply(void) {
    const int   pct    = eden_resolved_ui_scale_pct();
    const int   layout = eden_resolved_layout();

    float aspect = (float)IPHONE5_WIDTH / (float)IPHONE_HEIGHT;   // 1.775 — the classic profile
    if (layout == LAYOUT_ADAPTIVE && g_cssW > 0 && g_cssH > 0)
        aspect = (float)g_cssW / (float)g_cssH;
    if (aspect < kMinAspect) aspect = kMinAspect;
    if (aspect > kMaxAspect) aspect = kMaxAspect;

    float pointH = kBasePointHeight * 100.0f / (float)pct;
    // Apply the density floor against the box the page will actually letterbox the canvas to, not
    // against the raw viewport: when the aspect clamp bites (a portrait window), the canvas is
    // shorter than the viewport and it is the canvas the UI is drawn into.
    if (g_cssW > 0 && g_cssH > 0) {
        float boxH = (float)g_cssH;
        if ((float)g_cssW / aspect < boxH) boxH = (float)g_cssW / aspect;
        const float maxPointsForBox = boxH / kMinCssPxPerPoint;
        if (pointH > maxPointsForBox) pointH = maxPointsForBox;
    }
    // THE CLASSIC-WIDTH FLOOR, and it is the reason the touch profile no longer has to pin 16:9.
    //
    // The density floor above says "one point is never smaller than one CSS pixel" — never DENSER
    // than the device the art was drawn on. This is its sibling in the other axis: the point space
    // is never NARROWER than that device either. Every HUD and menu rect in Classes/ is an
    // absolute number of points laid out against a 568-point width (Hud.mm's mode buttons, the
    // block picker grid, the joystick pad at x=20 w=88), so a space narrower than 568 does not
    // scale the UI down, it runs it off the right edge — and `IS_WIDESCREEN` (10 branches in
    // Classes/) flips below 480 as well.
    //
    // Deriving the height first and the width from the aspect is what makes that possible on a
    // narrow screen: raising pointH is the ONLY way to widen the point space at a fixed aspect. On
    // 4:3 it lifts a ui_scale-200% layout from 426x320 to 568x426 — the same UI width as the
    // classic layout, with the extra points spent on world rather than on letterbox bars.
    //
    // It runs AFTER the density cap deliberately. The cap is a comfort heuristic ("do not make the
    // buttons physically tiny"); this is a correctness floor ("do not lay the UI out in a box it
    // does not fit in"), so when a small window makes the two disagree, this one wins.
    if (aspect > 0.0f) {
        const float minPointH = (float)kClassicPointW / aspect;
        if (pointH < minPointH) pointH = minPointH;
    }
    const int h = eden_clampi(eden_round_even(pointH), kMinPointH, kMaxPointH);
    const int w = eden_clampi(eden_round_even((float)h * aspect), kMinPointW, kMaxPointW);

    // Retina/UI-scale flags. Unchanged from what EAGLView_web.mm pinned — see CLAUDE.md #3
    // ("IS_IPAD == true also on retina iPhones", i.e. it means '2x UI scale') and this port's own
    // note that SCALE_WIDTH/SCALE_HEIGHT are DIVISORS in layout math and must never be 0. These are
    // NOT what D1 unpins: they select the @2x asset set and the point->ortho factor, both of which
    // stay correct at any point-space size. `ui_scale` moves the point space, not these.
    IS_IPAD         = TRUE;
    IS_RETINA       = TRUE;
    SUPPORTS_RETINA = TRUE;
    SCALE_WIDTH     = 2.0f;
    SCALE_HEIGHT    = 2.0f;

    SCREEN_WIDTH  = (float)w;
    SCREEN_HEIGHT = (float)h;
    // ALWAYS derived — the stock EAGLView.mm only did this inside `if(IS_WIDESCREEN)` and left the
    // other branch on a 4:3 default that matched no live layout. See the header.
    P_ASPECT_RATIO = SCREEN_WIDTH / SCREEN_HEIGHT;
    // Stock set this by comparing the device's bounds against 568 points. What every one of its
    // ~10 branches in Classes/ actually keys off is "is there more width here than the 480-point
    // layout was drawn for", so that is what it now says. True for every profile this port can
    // produce, which is correct: kMinPointW is already above 480 at the smallest UI scale.
    IS_WIDESCREEN = (SCREEN_WIDTH > (float)IPHONE_WIDTH);

    // Util.mm's findWorldCoords unprojects a POINT-space tap against whatever GL_VIEWPORT reports,
    // after scaling it by SCALE_*. Keep the shim's answer in step or every click lands off-target.
    eden_gl_set_pick_viewport((int)(SCREEN_WIDTH * SCALE_WIDTH),
                              (int)(SCREEN_HEIGHT * SCALE_HEIGHT));

    const bool changed = (w != g_pointW || h != g_pointH);
    g_pointW = w;
    g_pointH = h;
    if (!changed) return;

    std::fprintf(stderr, "[eden-display] profile=%s ui_scale=%d%% layout=%s -> %dx%d points"
                         " (aspect %.3f)\n",
                 eden_profile_active()->name, pct,
                 (layout == LAYOUT_ADAPTIVE) ? "adaptive" : "classic",
                 w, h, (double)P_ASPECT_RATIO);

    // Re-layout whatever already exists. At boot none of it does — the constructors read the
    // globals set above — so this is purely the "metrics changed while running" path.
    Input::getInput()->screenMetricsChanged();
    if (World::getWorld) {
        if (World::getWorld->hud)  World::getWorld->hud->layoutForScreen();
        if (World::getWorld->menu) World::getWorld->menu->layoutForScreen();
    }
}

extern "C" {

EDEN_EXPORT
void eden_display_set_viewport(int css_w, int css_h) {
    if (css_w <= 0 || css_h <= 0) return;
    if (css_w == g_cssW && css_h == g_cssH) return;
    g_cssW = css_w;
    g_cssH = css_h;
    eden_display_apply();
}

EDEN_EXPORT
void eden_display_refresh(void) { eden_display_apply(); }

EDEN_EXPORT
int eden_display_point_width(void)  { return g_pointW > 0 ? g_pointW : IPHONE5_WIDTH; }

EDEN_EXPORT
int eden_display_point_height(void) { return g_pointH > 0 ? g_pointH : IPHONE_HEIGHT; }

EDEN_EXPORT
int eden_display_aspect_x1000(void) {
    if (g_pointH <= 0) return (IPHONE5_WIDTH * 1000) / IPHONE_HEIGHT;
    return (int)(1000.0f * (float)g_pointW / (float)g_pointH + 0.5f);
}

// Does the active profile want the on-screen joystick / jump / crouch chrome? Read by
// Settings_web.mm's eden_apply_input_profile (which owns hud->use_joystick) and exported for the
// page, which hides its own touch-only affordances on the same signal.
EDEN_EXPORT
int eden_profile_touch_chrome(void) { return eden_profile_active()->touch_chrome; }

// The active profile's name, for the debug JSON and for the settings panel's "Auto (desktop)" hint.
EDEN_EXPORT
const char* eden_profile_name(void) { return eden_profile_active()->name; }

} // extern "C"
