// Settings_web.mm — the port's settings model (pass 28).
//
// WHAT THIS REPLACES
// `Classes/SettingsMenu.mm` draws a fixed 5-row GL panel of ON/OFF images with hard-coded pixel
// rects (`rect_on[j].origin.x = 300+115` and friends), reachable only from the main menu's Options
// button. It is not extensible — every row is a hand-placed texture and a hard-coded `if` in both
// update() and render() — and there is nowhere to put a slider at all.
//
// So the DATA half of that class is kept and the UI half is replaced:
//   * KEPT, untouched: `SettingsMenu::{load,save,getNewWorldName}` and its `properties[]` array.
//     Those own the engine-visible meaning of every toggle (Resources::playmusic, Player::
//     autojump_option/health_option, CREATURES_ON, ...) and the NSUserDefaults round-trip. This
//     file WRITES `properties[i].value` and then calls the engine's own `save()`, so the engine
//     applies its own settings exactly as it always did.
//   * REPLACED via `--wrap`: `SettingsMenu::update(float)` and `SettingsMenu::render()` become
//     no-ops, so the old GL panel neither draws nor eats touches while the DOM panel is up. Both
//     are called from `Classes/Menu.mm` (a different translation unit from SettingsMenu.mm), which
//     is what makes wasm-ld's --wrap able to see them at all.
// With both wrapped, `Menu::update`/`Menu::render` still take their `if (showsettings) { ...;
// return; }` early-outs — so the main menu stops responding and keeps drawing just its background
// while the panel is open. That is exactly the modal behaviour the old panel had.
//
// The port's OWN preferences (sensitivity, FOV, volumes, block preview, ...) live here too, in one
// table with the engine ones, because a settings screen split across two models would drift. The
// JS side (public/eden-settings.js) renders whatever `eden_settings_schema()` describes and knows
// nothing about which half a row belongs to.
//
// PERSISTENCE is NSUserDefaults for everything — the engine rows through `SettingsMenu::save()`,
// the port rows written here directly. That shim is localStorage-backed as of pass 28, so both
// halves survive a reload with one mechanism (see src/shim/foundation/NSUserDefaults.mm).
#import "../shim/foundation/uikit_stubs.h"
#import "../shim/foundation/NSUserDefaults.h"
#import "../shim/foundation/NSNumber.h"
#import "../shim/foundation/NSString.h"
#import "../../../Classes/World.h"
#import "../../../Classes/Menu.h"
#import "../../../Classes/SettingsMenu.h"
#import "../../../Classes/Resources.h"
#import "../../../Classes/Input.h"
#import "../../../Classes/SimpleAudioEngine.h"
#import "DisplayProfile_web.h"
#include "../shim/foundation/platform_shims.h"   // EDEN_EXPORT (Phase N Stage 1)
#include <cstdio>
#include <cstring>
#include <cmath>

// Mirrors the anonymous enum at the top of Classes/SettingsMenu.mm. Those constants are file-local
// there, so the indexes are duplicated — same situation as Player's `usage_id` in Input_web.mm. If
// they ever change, the engine toggles silently address the wrong row; the labels below are the
// canary (they are the engine's own `pnames[]` strings).
enum {
    ENG_CREATURES = 0,
    ENG_AUTOJUMP  = 1,
    ENG_HEALTH    = 2,
    ENG_SOUND     = 3,
    ENG_MUSIC     = 4,
};

// ---------------------------------------------------------------------------------------------
// The schema. Order here IS the display order; `group` drives the section headings in the panel.
// `kind`: 0 = toggle (0/1), 1 = range, 2 = enum (a small label list, value is the 0-based index
// into `options`). `engine` >= 0 means the value lives in SettingsMenu::properties[engine]; -1
// means this file owns it.
// ---------------------------------------------------------------------------------------------
// KIND_KEY (3) is NOT used by any kSettings[] row — it is the row-kind constant the GL keybinds
// screen dispatches on, and it lives here so the kind space stays one enumeration. See the
// KEYBINDS section near the bottom of this file for why a keybind is a parallel table.
enum { KIND_TOGGLE = 0, KIND_RANGE = 1, KIND_ENUM = 2, KIND_KEY = 3 };

struct Setting {
    const char* key;      // persistence key AND the JS-side id
    const char* label;
    const char* group;
    int   kind;
    int   engine;         // index into SettingsMenu::properties[], or -1
    float min, max, step;
    float def;
    const char* hint;     // one line of explanation, shown under the control
    const char* options;  // KIND_ENUM only: comma-separated labels, e.g. "Auto,Touch,Keyboard+Mouse"
};

static const Setting kSettings[] = {
  // key                 label                group        kind         engine        min  max  step  def   hint                                                                options
  { "health",            "Health",            "Gameplay",  KIND_TOGGLE, ENG_HEALTH,     0,   1,   1,   1,  "Take damage from falls, fire and creatures.",                     NULL },
  { "autojump",          "Auto-jump",         "Gameplay",  KIND_TOGGLE, ENG_AUTOJUMP,   0,   1,   1,   1,  "Step up single blocks automatically.",                            NULL },
  { "creatures",         "Creatures",         "Gameplay",  KIND_TOGGLE, ENG_CREATURES,  0,   1,   1,   1,  "Spawn and render wildlife. Off is faster.",                       NULL },

  { "music",             "Music",             "Audio",     KIND_TOGGLE, ENG_MUSIC,      0,   1,   1,   1,  NULL,                                                               NULL },
  { "sound",             "Sound effects",     "Audio",     KIND_TOGGLE, ENG_SOUND,      0,   1,   1,   1,  NULL,                                                               NULL },
  { "music_volume",      "Music volume",      "Audio",     KIND_RANGE,  -1,             0,   1, .05f,  1,  NULL,                                                               NULL },
  { "ambience_volume",   "Ambience volume",   "Audio",     KIND_RANGE,  -1,             0,   1, .05f,  1,  NULL,                                                               NULL },
  { "effects_volume",    "Effects volume",    "Audio",     KIND_RANGE,  -1,             0,   1, .05f,  1,  NULL,                                                               NULL },
  { "touch_controls_sound", "Touch controls sound", "Audio", KIND_TOGGLE, -1,           0,   1,   1,   0,  "Joystick and jump-button taps make a sound. Touchscreen only -- never fires for keyboard/mouse. Off by default.", NULL },

  // input_mode: 0=Auto (matchMedia + first-input detection, see eden-st.html), 1=Touch,
  // 2=Keyboard+Mouse. Auto is the default so nothing changes for players who never open Settings.
  { "input_mode",        "Input mode",        "Controls",  KIND_ENUM,   -1,             0,   2,   1,   0,  "Auto-detects touch vs. keyboard/mouse; force one if it guesses wrong.", "Auto,Touch,Keyboard+Mouse" },
  { "hold_to_act",       "Hold to mine/build", "Controls",  KIND_TOGGLE, -1,            0,   1,   1,   1,  "Hold the mouse button to repeat, instead of one block per click.", NULL },
  { "mouse_sensitivity", "Mouse sensitivity", "Controls",  KIND_RANGE,  -1,          .25f,  3, .05f,  1,  "Look speed while the pointer is locked.",                         NULL },
  { "mouse_sensitivity_y", "Mouse sensitivity (Y)", "Controls", KIND_RANGE, -1,      .25f,  3, .05f,  1,  "Vertical look speed, if you want it different from horizontal.",  NULL },
  { "invert_look",       "Invert look",       "Controls",  KIND_TOGGLE, -1,             0,   1,   1,   0,  NULL,                                                               NULL },
  // Row #24 (gamepad). Two translators read these now — public/eden-gamepad.js on web and
  // native/src/seam/Input_native.cpp (Phase N Stage 2) on native — both of them pure translators
  // over the existing input entry points. The rows deliberately do
  // not touch the engine (engine >= 0). ON by default because the module is inert until a pad is
  // actually connected AND has been interacted with, so it costs a disconnected player nothing.
  { "gamepad",           "Gamepad",           "Controls",  KIND_TOGGLE, -1,             0,   1,   1,   1,  "Use a connected controller (standard mapping). Sticks move and look; triggers mine and build.", NULL },
  { "gamepad_look_sensitivity", "Gamepad look speed", "Controls", KIND_RANGE, -1,     .25f,  3, .05f,  1,  "Right-stick look speed. Composes with mouse sensitivity.",        NULL },
  { "gamepad_deadzone",  "Gamepad deadzone",  "Controls",  KIND_RANGE,  -1,          .05f, .5f, .05f, .15f, "Ignore stick movement smaller than this. Raise it if the camera drifts on its own.", NULL },

  { "fov",               "Field of view",     "Video",     KIND_RANGE,  -1,            60, 110,   1,  80,  "Vertical FOV in degrees. 80 is the original.",                    NULL },
  { "display_mode",      "Display",           "Video",     KIND_ENUM,   -1,             0,   2,   1,   0,  "Fixed size, fit the window, or a fullscreen button.",             "Fixed,Fit window,Fullscreen" },
  // Audit rows D1 + D4 — the two knobs on the (now derived) engine POINT space. Both default to
  // Auto, which defers to the active device profile (src/seam/DisplayProfile_web.mm's kProfiles[]):
  // desktop gets 125% + Adaptive, touch gets 200% + Classic, i.e. exactly the 568x320 layout this
  // port shipped with. Leaving them on Auto is not the same as picking the profile's value by hand
  // — Auto re-resolves if the input mode changes, an explicit choice does not.
  { "ui_scale",          "UI scale",          "Video",     KIND_ENUM,   -1,             0,   4,   1,   0,  "Size of the HUD, buttons and menus. Larger percentages mean bigger UI and less of the world on screen.", "Auto,100%,125%,150%,200%" },
  { "display_layout",    "Layout aspect",     "Video",     KIND_ENUM,   -1,             0,   2,   1,   0,  "Adaptive fits the game's layout to the window shape (wider windows show more world). Classic keeps the original 16:9 phone layout and letterboxes.", "Auto,Classic 16:9,Adaptive" },
  // Audit item #6 (§4a/§4c.1-2): the drawable used to be pinned to 1136x640 with devicePixelRatio
  // never read at all, so on any modern display the game was a small buffer upscaled — permanently
  // blurry. These two rows are the knobs on the now-dynamic drawable that public/eden-st.html
  // computes (CSS box x min(devicePixelRatio, dpr_cap) x render_scale). Neither touches the
  // engine's 568x320 POINT space; see eden_set_drawable_size() in the GL shim.
  // Defaults reproduce "as sharp as the display allows, no supersampling": cap 2x, scale 100%.
  { "render_scale",      "Render scale",      "Video",     KIND_ENUM,   -1,             0,   3,   1,   2,  "Internal resolution. Lower is faster and softer; 100% matches the window.", "50%,75%,100%,125%" },
  { "dpr_cap",           "Max pixel ratio",   "Video",     KIND_ENUM,   -1,             0,   2,   1,   2,  "Upper limit on the display's pixel density. Lower it if the frame rate suffers.", "1x,1.5x,2x" },
  // Row #14: an opt-in frame-rate ceiling, mainly for thermal/battery on touch devices (a voxel
  // game at an uncapped rAF is a thermal-throttle machine there — perf-audit §4b). Uncapped is the
  // default for everyone; eden_apply_input_profile() below sets it to 60 the first time a touch
  // profile is detected AND the player has never explicitly touched this row.
  { "fps_cap",           "Frame rate cap",    "Video",     KIND_ENUM,   -1,             0,   3,   1,   0,  "Limit how fast the game renders. Lower saves battery/heat on touch devices.", "Uncapped,30,45,60" },

  { "crosshair",         "Crosshair",         "Interface", KIND_TOGGLE, -1,             0,   1,   1,   1,  "Reticle at screen centre while the mouse is locked.",             NULL },
  { "block_preview",     "Block preview",     "Interface", KIND_TOGGLE, -1,             0,   1,   1,   0,  "Translucent ghost of the block you are about to place. (B)",      NULL },
  // The escape hatch for the rebuilt DOM UI (public/eden-menu.js + public/eden-pausemenu.js). ON =
  // the original 2010 GL menus, which are still compiled and still running underneath — for the
  // MAIN menu "legacy" is literally "stop drawing the overlay" and there is no second code path to
  // keep alive. The IN-GAME menu needs one extra step in the other direction: its GL panel is
  // suppressed by default (eden_hud_draw_menu_screen_hook, installed in Menu_web.mm) because the
  // DOM panel no longer covers the whole canvas, so this flag both re-enables that panel and keeps
  // the DOM one closed. Read by eden_menu_active()/eden_legacy_ui_active() (Menu_web.mm), by
  // public/eden-menu.js's own poll, and by public/eden-pausemenu.js's tick().
  { "legacy_menu",       "Legacy UI",         "Interface", KIND_TOGGLE, -1,             0,   1,   1,   0,  "Use the original 2010 OpenGL menus — both the main menu and the in-game menu — instead of the rebuilt ones.", NULL },

  // Experiments: opt-in, off by default (except fly/fps_normalize, which keep their pre-existing
  // defaults now that they've moved tabs — only their group changed, not their behavior).
  { "fly",               "Fly mode",          "Experiments", KIND_TOGGLE, -1,           0,   1,   1,   0,  "Free flight. Space/Ctrl to rise and fall. (F)",                   NULL },
  { "fps_normalize",     "Frame-rate normalize", "Experiments", KIND_TOGGLE, -1,        0,   1,   1,   1,  "Keep walk speed the same at any refresh rate (PC audit F1).",     NULL },
  { "advanced_movement", "Advanced movement (bhop)", "Experiments", KIND_TOGGLE, -1,     0,   1,   1,   0,  "Opt-in: zero-delay bunny-hop and wheel-jump. Off by default.",    NULL },
  { "crouch",            "Crouch mode",       "Experiments", KIND_TOGGLE, -1,           0,   1,   1,   0,  "Adds a crouch key/button (halves hitbox height for 1-block gaps). Off by default.", NULL },

  { "save_backup",       "Keep a save backup", "Saves",      KIND_TOGGLE, -1,           0,   1,   1,   1,  "Keep a '.bak' copy of a world's previous save so a corrupted or interrupted save can be recovered. Roughly doubles the disk write per save below the in-place threshold. On by default -- this is what a corrupted-load recovery prompt offers to restore.", NULL },
};
static const int kSettingCount = (int)(sizeof(kSettings) / sizeof(kSettings[0]));

static float g_value[kSettingCount];
static bool  g_loaded = false;
// Row #14: did NSUserDefaults actually have a value for this (port-owned) row at load time? Used
// by eden_apply_input_profile() to tell "the player explicitly chose this" apart from "still
// sitting at the compiled default" — profile-driven defaults must only ever touch the latter,
// mirroring input_mode's own Auto/explicit arbitration (this file's header comment on Phase 2).
static bool  g_hadStored[kSettingCount];

static int eden_setting_index(const char* key) {
    for (int i = 0; i < kSettingCount; ++i)
        if (std::strcmp(kSettings[i].key, key) == 0) return i;
    return -1;
}

// Port-owned values the rest of the port reads. Declared in Input_web.mm / consumed by the
// gluPerspective wrap below.
float eden_look_sensitivity = 1.0f;   // read by eden_apply_look_delta
float eden_fov_degrees      = 80.0f;  // read by __wrap_gluPerspective
// PC controls audit F1/F2 (Movement_web.mm's --wrap of Player::setSpeed). Default ON; a toggle
// exists so stock (frame-time-dependent) feel can be A/B'd without a rebuild.
float eden_fps_normalize    = 1.0f;
// PC controls audit Phase 3/4/6 — plain mutable globals in the same style, read by
// Input_web.mm/eden-st.html.
float eden_mouse_sensitivity_y = 1.0f;   // read by eden_apply_look_delta (Y axis)
float eden_hold_to_act      = 1.0f;      // read by eden-st.html's hold-to-act state machine
float eden_crosshair        = 1.0f;      // read by eden-st.html's crosshair overlay
float eden_advanced_movement = 0.0f;     // read by Movement_web.mm's Phase 6 hook
float eden_display_mode     = 0.0f;      // 0=Fixed/1=Fit/2=Fullscreen-on-demand, read by eden-st.html
// Audit item #6. Stored as the ENUM INDEX (same as every other KIND_ENUM row); the getters below
// translate to the numbers the page actually wants, so the index<->value table lives in exactly
// one place instead of being duplicated in JS.
float eden_render_scale     = 2.0f;      // index into {50%,75%,100%,125%}
float eden_dpr_cap          = 2.0f;      // index into {1x,1.5x,2x}
float eden_fps_cap          = 0.0f;      // index into {Uncapped,30,45,60}; row #14
// Audit D1/D4. Both are enum indexes whose 0 is "Auto"; read (not written) by
// src/seam/DisplayProfile_web.mm, which resolves Auto against the active profile every time it
// recomputes the point space. Never seeded by eden_apply_profile_defaults — see that function.
float eden_ui_scale         = 0.0f;      // index into {Auto,100%,125%,150%,200%}
float eden_display_layout   = 0.0f;      // index into {Auto,Classic 16:9,Adaptive}
// Read by eden_menu_active() (Menu_web.mm) and public/eden-menu.js. 0 = the rebuilt DOM menu.
float eden_legacy_menu      = 0.0f;
// Row #24's three gamepad rows, mirrored into C for the same reason every other row above is:
// there is now a SECOND consumer. On web the translator is public/eden-gamepad.js and reads them
// through the settings bridge; on native it is native/src/seam/Input_native.cpp, which has no JS
// to read anything through. The rows themselves are unchanged, and their schema comment's "no
// C-side gamepad code" note is what these three lines retire.
float eden_gamepad_enabled  = 1.0f;
float eden_gamepad_look_sensitivity = 1.0f;
float eden_gamepad_deadzone = 0.15f;
// Read (via eden_get_save_backup() below, not directly -- see there) by the portable
// save_backup.cpp, shared with native. Default ON: it is what keeps a corrupted/truncated save
// recoverable through eden_load_restore_backup(), so OFF is an explicit opt-out of that safety
// net, not a neutral default.
float eden_save_backup      = 1.0f;

// ---------------------------------------------------------------------------------------------
// Phase 2 — input mode: Auto(0) / Touch(1) / Keyboard+Mouse(2). "Auto" defers to whatever the
// page has detected so far (matchMedia at boot, then the first real touch/key/mouse event —
// see eden-st.html); a real touch or key event is unambiguous evidence, matchMedia's
// `pointer:coarse` is just the pre-interaction guess. `g_detectedTouch` is JS-owned state fed in
// through eden_set_detected_touch(); this file only arbitrates it against the user's explicit
// override (`input_mode` != Auto).
// ---------------------------------------------------------------------------------------------
static int  g_inputMode = 0;         // mirrors g_value[idx of "input_mode"], kept as an int too
static bool g_detectedTouch = false;

static void eden_apply_input_profile(void);

extern "C" {
EDEN_EXPORT
int eden_effective_input_is_touch(void) {
    if (g_inputMode == 1) return 1;
    if (g_inputMode == 2) return 0;
    return g_detectedTouch ? 1 : 0;
}

EDEN_EXPORT
void eden_set_detected_touch(int isTouch) {
    g_detectedTouch = (isTouch != 0);
    eden_apply_input_profile();
}

// Lets the DOM UI panels (eden-pausemenu.js, eden-settings.js — plain HTML buttons, no touch
// path through Hud::update) play the same click sounds the engine's own HUD menu icons got wired
// up to in Classes/Hud.mm (pass 39, S_MENU_BUTTON_PRESS/RELEASE). `pressed` nonzero = press,
// zero = release. Resources::getResources is a singleton set up once in World's constructor
// (Classes/World.mm) and outlives every menu/game-mode transition, so this is safe to call from
// either the main menu or in-game.
EDEN_EXPORT
void eden_play_menu_button_sound(int pressed) {
    if (!Resources::getResources) return;
    Resources::getResources->playSound(pressed ? S_MENU_BUTTON_PRESS : S_MENU_BUTTON_RELEASE);
}

// Settings panel's boolean on/off switches ONLY (eden-settings.js's kind===0 rows) — deliberately
// separate from eden_play_menu_button_sound above. menu_button_press/release_01.mp3 (variation 01
// of that sound) is audibly different from variations 02-05 and was pulled out of that sound's
// random-variation pool (Classes/Resources.mm) specifically so it would stop turning up on
// ordinary button clicks; it now lives only here, as its own single-variation sound ID, for the
// one control it was asked to keep: switch toggles.
EDEN_EXPORT
void eden_play_switch_toggle_sound(int on) {
    if (!Resources::getResources) return;
    Resources::getResources->playSound(on ? S_SWITCH_TOGGLE_ON : S_SWITCH_TOGGLE_OFF);
}
} // extern "C"

// Row #14 (perf-audit §4c.3), generalised into audit row D4's first-class PROFILE concept: "one
// build, two profiles" — profile-driven DEFAULTS, never profile-driven code paths. The two profiles
// and every field they carry live in src/seam/DisplayProfile_web.mm's kProfiles[]; this function is
// the half that pushes those fields into `kSettings[]` rows.
//
// Runs once per session, the first time BOTH the settings model is loaded (so g_hadStored[] is
// meaningful) AND the input profile is actually resolved (Auto's g_detectedTouch has a real value,
// or the player forced one). Only ever adjusts a row the player has never explicitly touched
// (checked via g_hadStored — the same "explicit choice wins" rule input_mode itself already
// followed).
//
// WHY ui_scale AND display_layout ARE NOT IN THIS LIST even though the profile carries them: they
// default to "Auto", which resolves against the profile on every read. Seeding them here would
// convert a live default into a frozen explicit choice the first time the page booted, so an
// Auto-mode player who later plugged in a keyboard would keep the phone layout forever. The rows
// below have no Auto option, so a written default is the only mechanism available to them.
// (crosshair/block_preview already default to sane values for both profiles — see their schema rows
// and Section 4b — so there is nothing to override for them.)
extern "C" void eden_settings_set(int i, float v);   // defined further down in this file
static bool g_profileDefaultsApplied = false;
static void eden_seed_profile_default(const char* key, int optionIndex) {
    int i = eden_setting_index(key);
    if (i >= 0 && !g_hadStored[i]) eden_settings_set(i, (float)optionIndex);
}
static void eden_apply_profile_defaults(void) {
    if (g_profileDefaultsApplied || !g_loaded) return;
    g_profileDefaultsApplied = true;
    const EdenProfile* p = eden_profile_active();
    int dpr = p->dpr_cap, fps = p->fps_cap, rs = p->render_scale;
    // ROADMAP Phase M / M5.2: the low-memory overlay replaces the video-preset defaults with the
    // leaner kProfiles[EDEN_PROFILE_LOWMEM] row (1x pixel ratio, 75% render scale, 45 fps). It is
    // set by the page before this runs; still only a DEFAULT, so g_hadStored[] (a row the player
    // touched) wins exactly as with the input profile.
    if (eden_low_memory()) {
        const EdenProfile* lm = eden_profile_get(EDEN_PROFILE_LOWMEM);
        dpr = lm->dpr_cap;
        rs  = lm->render_scale;
        if (lm->fps_cap) fps = lm->fps_cap;
    }
    eden_seed_profile_default("dpr_cap",      dpr);
    eden_seed_profile_default("fps_cap",      fps);
    eden_seed_profile_default("render_scale", rs);
}

static void eden_apply_input_profile(void) {
    // The point space depends on the profile (ui_scale/display_layout resolve through it), so a
    // profile change is a metrics change. Safe before the World exists — DisplayProfile_web.mm
    // only re-lays-out what already exists.
    eden_display_refresh();
    if (!World::getWorld || !World::getWorld->hud) return;
    // Control chrome (the on-screen joystick + jump/crouch buttons) is a profile field, not a
    // separate detection. `use_joystick` is the engine's own name for it.
    World::getWorld->hud->use_joystick = eden_profile_touch_chrome() ? TRUE : FALSE;
    eden_apply_profile_defaults();
}

extern "C" void eden_keybind_reset_all(void);   // Stage 5.2, defined further down
extern "C" void eden_set_block_preview(int on);
extern "C" void eden_set_fly_mode(int on);
extern "C" void eden_set_crouch_enabled(int on);

static SettingsMenu* eden_engine_settings(void) {
    if (!World::getWorld || !World::getWorld->menu) return NULL;
    return World::getWorld->menu->settings;
}

// ---------------------------------------------------------------------------------------------
// Applying one row. Engine rows are written into `properties[]` and committed with the engine's
// own save() (which re-runs load(), i.e. the engine's own apply step). Port rows act directly.
//
// The re-apply at the bottom is NOT belt-and-braces: `SettingsMenu::load()` unconditionally sets
// `player->invertcam = FALSE` and `hud->use_joystick = TRUE` every time it runs (Classes/
// SettingsMenu.mm:188-189 — hard-coded, not saved preferences). So committing ANY engine toggle
// silently reverts two of the port's own control settings unless they are re-applied afterwards.
// This is the same class of trap pass 23 hit with use_joystick and Joystick::update.
// ---------------------------------------------------------------------------------------------
static void eden_apply_port_settings(void);

static void eden_apply_setting(int i, bool commitEngine) {
    if (i < 0 || i >= kSettingCount) return;
    const Setting& s = kSettings[i];
    const float v = g_value[i];

    if (s.engine >= 0) {
        SettingsMenu* sm = eden_engine_settings();
        if (!sm) return;
        sm->properties[s.engine].value = (v != 0.0f) ? 1 : 0;
        if (!commitEngine) return;
        sm->save();                       // engine: writes NSUserDefaults, then load() applies
        // Music has one side effect save()/load() does not do: the menu tune has to be started or
        // stopped right now. Classes/SettingsMenu.mm:150-158 did this inline in its touch handler,
        // which is exactly the piece a wrapped-out update() would otherwise lose.
        if (s.engine == ENG_MUSIC && Resources::getResources) {
            if (v != 0.0f) Resources::getResources->playMenuTune();
            else           Resources::getResources->stopMenuTune();
        }
        eden_apply_port_settings();       // undo load()'s invertcam/use_joystick stomp
        return;
    }

    if (std::strcmp(s.key, "block_preview") == 0) {
        eden_set_block_preview(v != 0.0f ? 1 : 0);
    } else if (std::strcmp(s.key, "fly") == 0) {
        eden_set_fly_mode(v != 0.0f ? 1 : 0);
    } else if (std::strcmp(s.key, "crouch") == 0) {
        eden_set_crouch_enabled(v != 0.0f ? 1 : 0);
    } else if (std::strcmp(s.key, "mouse_sensitivity") == 0) {
        eden_look_sensitivity = v;
    } else if (std::strcmp(s.key, "fov") == 0) {
        eden_fov_degrees = v;
    } else if (std::strcmp(s.key, "fps_normalize") == 0) {
        eden_fps_normalize = v;
    } else if (std::strcmp(s.key, "hold_to_act") == 0) {
        eden_hold_to_act = v;
    } else if (std::strcmp(s.key, "mouse_sensitivity_y") == 0) {
        eden_mouse_sensitivity_y = v;
    } else if (std::strcmp(s.key, "crosshair") == 0) {
        eden_crosshair = v;
    } else if (std::strcmp(s.key, "advanced_movement") == 0) {
        eden_advanced_movement = v;
    } else if (std::strcmp(s.key, "display_mode") == 0) {
        eden_display_mode = v;
    } else if (std::strcmp(s.key, "render_scale") == 0) {
        eden_render_scale = v;
    } else if (std::strcmp(s.key, "dpr_cap") == 0) {
        eden_dpr_cap = v;
    } else if (std::strcmp(s.key, "fps_cap") == 0) {
        eden_fps_cap = v;
    } else if (std::strcmp(s.key, "ui_scale") == 0) {
        eden_ui_scale = v;
        eden_display_refresh();     // re-derives the point space and re-lays-out the HUD/menu
    } else if (std::strcmp(s.key, "display_layout") == 0) {
        eden_display_layout = v;
        eden_display_refresh();
    } else if (std::strcmp(s.key, "legacy_menu") == 0) {
        eden_legacy_menu = v;
    } else if (std::strcmp(s.key, "gamepad") == 0) {
        eden_gamepad_enabled = v;
    } else if (std::strcmp(s.key, "gamepad_look_sensitivity") == 0) {
        eden_gamepad_look_sensitivity = v;
    } else if (std::strcmp(s.key, "gamepad_deadzone") == 0) {
        eden_gamepad_deadzone = v;
    } else if (std::strcmp(s.key, "save_backup") == 0) {
        eden_save_backup = v;
    } else if (std::strcmp(s.key, "input_mode") == 0) {
        g_inputMode = (int)lroundf(v);
        eden_apply_input_profile();
    } else if (std::strcmp(s.key, "invert_look") == 0) {
        if (World::getWorld && World::getWorld->player)
            World::getWorld->player->invertcam = (v != 0.0f) ? TRUE : FALSE;
    } else if (std::strcmp(s.key, "music_volume") == 0) {
        if (CocosDenshion::SimpleAudioEngine::sharedEngine())
            CocosDenshion::SimpleAudioEngine::sharedEngine()->setBackgroundMusicVolume(v);
    } else if (std::strcmp(s.key, "ambience_volume") == 0) {
        if (CocosDenshion::SimpleAudioEngine::sharedEngine())
            CocosDenshion::SimpleAudioEngine::sharedEngine()->setAmbienceVolume(v);
    } else if (std::strcmp(s.key, "effects_volume") == 0) {
        if (CocosDenshion::SimpleAudioEngine::sharedEngine())
            CocosDenshion::SimpleAudioEngine::sharedEngine()->setEffectsVolume(v);
    } else if (std::strcmp(s.key, "touch_controls_sound") == 0) {
        extern bool touchControlsSoundEnabled;
        touchControlsSoundEnabled = (v != 0.0f);
    }
}

static void eden_apply_port_settings(void) {
    for (int i = 0; i < kSettingCount; ++i)
        if (kSettings[i].engine < 0) eden_apply_setting(i, false);
}

// Port rows persist here; engine rows persist through SettingsMenu::save(). Both end up in
// NSUserDefaults, which is localStorage-backed (pass 28) — one store, one lifetime.
static void eden_persist_setting(int i) {
    if (kSettings[i].engine >= 0) return;   // owned by SettingsMenu::save()
    NSUserDefaults* prefs = [NSUserDefaults standardUserDefaults];
    NSString* key = [NSString stringWithUTF8String:kSettings[i].key];
    // Stored as an int in thousandths so one NSNumber type covers toggles and ranges alike (the
    // NSUserDefaults shim persists NSNumber only — see its header comment).
    [prefs setObject:[NSNumber numberWithInt:(int)lroundf(g_value[i] * 1000.0f)] forKey:key];
    [prefs synchronize];
}

extern "C" {

// Called once the world exists. Seeds every row: engine rows FROM the engine (SettingsMenu's ctor
// has already run its own load()), port rows from NSUserDefaults, then applies the port half.
EDEN_EXPORT
void eden_settings_init(void) {
    if (g_loaded) return;
    SettingsMenu* sm = eden_engine_settings();
    if (!sm) return;                        // too early; caller retries next frame
    g_loaded = true;

    NSUserDefaults* prefs = [NSUserDefaults standardUserDefaults];
    for (int i = 0; i < kSettingCount; ++i) {
        if (kSettings[i].engine >= 0) {
            g_value[i] = (float)sm->properties[kSettings[i].engine].value;
            continue;
        }
        g_value[i] = kSettings[i].def;
        NSString* key = [NSString stringWithUTF8String:kSettings[i].key];
        id stored = [prefs objectForKey:key];
        g_hadStored[i] = (stored != nil);
        if (stored) g_value[i] = [(NSNumber*)stored intValue] / 1000.0f;
    }
    // Migration (Phase 2): "touch_joystick" was an independent toggle before input_mode existed
    // (pass 28-30). If a player had explicitly turned it ON and never saw an "input_mode" pref
    // (i.e. this is their first run since the upgrade), honor that as an explicit Touch choice
    // rather than silently reverting them to Auto-detect. Reads the OLD key directly since its
    // row no longer exists in kSettings[].
    {
        int input_mode_i = eden_setting_index("input_mode");
        if (input_mode_i >= 0 &&
            [prefs objectForKey:[NSString stringWithUTF8String:"input_mode"]] == nil) {
            id oldTouch = [prefs objectForKey:[NSString stringWithUTF8String:"touch_joystick"]];
            if (oldTouch && [(NSNumber*)oldTouch intValue] > 0) g_value[input_mode_i] = 1.0f;
        }
    }
    eden_apply_port_settings();
}

// Has eden_settings_init() actually run yet? It needs the World to exist, so it happens some
// frames after module init — later than the page's own boot sizing pass. public/eden-st.html uses
// this as a one-shot edge to re-derive the drawable from the RESTORED render_scale/dpr_cap
// (item #6); without it a persisted non-default value only took effect on the first resize.
EDEN_EXPORT
int eden_settings_loaded(void) { return g_loaded ? 1 : 0; }

// The panel's whole data model, as JSON. One string rather than an accessor per field so the C
// table above stays the single source of truth and the JS never hard-codes a row.
EDEN_EXPORT
const char* eden_settings_schema(void) {
    // SIZED WITH REAL HEADROOM ON PURPOSE. `snprintf` truncates safely, so overflowing this does
    // not corrupt memory — it emits JSON with no closing bracket, `JSON.parse` throws in
    // eden-settings.js, and the ENTIRE settings panel silently fails to render. Adding one row with
    // a long hint is enough to do it: at 6144 the table had 210 bytes of slack left when the D1/D4
    // rows landed. The `truncated` guard below turns that failure into a visible one.
    static char buf[12288];
    int n = 0;
    n += snprintf(buf + n, sizeof(buf) - n, "[");
    for (int i = 0; i < kSettingCount && n < (int)sizeof(buf) - 1; ++i) {
        const Setting& s = kSettings[i];
        n += snprintf(buf + n, sizeof(buf) - n,
            "%s{\"i\":%d,\"key\":\"%s\",\"label\":\"%s\",\"group\":\"%s\",\"kind\":%d,"
            "\"min\":%g,\"max\":%g,\"step\":%g,\"hint\":%s%s%s,\"options\":%s%s%s}",
            i ? "," : "", i, s.key, s.label, s.group, s.kind, s.min, s.max, s.step,
            s.hint ? "\"" : "", s.hint ? s.hint : "null", s.hint ? "\"" : "",
            s.options ? "\"" : "", s.options ? s.options : "null", s.options ? "\"" : "");
    }
    // snprintf's return is what WOULD have been written, so `n` running past the buffer is the
    // truncation signal. Say so out loud — the alternative is a settings panel that renders nothing
    // with no clue why, which is the same silent-failure class this port keeps getting bitten by.
    const bool truncated = (n >= (int)sizeof(buf) - 2);
    snprintf(buf + n, sizeof(buf) - n, "]");
    if (truncated) {
        std::fprintf(stderr, "[eden-settings] SCHEMA JSON TRUNCATED at %d bytes — grow buf[] in "
                             "eden_settings_schema(). The settings panel will not render.\n", n);
    }
    return buf;
}

EDEN_EXPORT
float eden_settings_get(int i) {
    if (i < 0 || i >= kSettingCount) return 0.0f;
    return g_value[i];
}

EDEN_EXPORT
void eden_settings_set(int i, float v) {
    if (i < 0 || i >= kSettingCount) return;
    const Setting& s = kSettings[i];
    if (v < s.min) v = s.min;
    if (v > s.max) v = s.max;
    g_value[i] = v;
    g_hadStored[i] = true;   // an explicit change now counts the same as a persisted one (row #14)
    eden_apply_setting(i, true);
    eden_persist_setting(i);
}

// Audit row 20/G2 (unify settings + keybinds): the two persistence systems (this C table vs.
// eden-keybinds.js's own localStorage blob) stay split on purpose — a JS float can't hold a
// code->action map — but that split had left the SURFACE fragmented too: no single "reset
// everything" existed anywhere (only the Keys tab's own local reset), which is the fragmentation
// the audit's row actually complains about, not the storage split itself. This is the engine-side
// half of one shared reset; eden-settings.js's new "Reset" tab calls this AND
// window.EdenKeybinds.resetDefaults() together, then re-renders once.
EDEN_EXPORT
void eden_settings_reset_all(void) {
    for (int i = 0; i < kSettingCount; ++i) eden_settings_set(i, kSettings[i].def);
    eden_keybind_reset_all();   // Stage 5.2: keybinds moved into C, so "reset everything" means it
}

// Convenience for the port's own keyboard shortcuts (B, F): keeps the key and the panel in sync
// instead of each writing its own copy of the state. Index-based, NOT key-based, deliberately —
// passing a C string in from JS would need `_malloc`/`_free` added to the export list, and the JS
// side already holds the schema (which carries every key -> index mapping) for free.
EDEN_EXPORT
float eden_settings_toggle(int i) {
    if (i < 0 || i >= kSettingCount) return 0.0f;
    eden_settings_set(i, g_value[i] != 0.0f ? 0.0f : 1.0f);
    return g_value[i];
}

// Is the ENGINE currently in its settings state? `Menu::showsettings` is the main menu's Options
// button (Classes/Menu.mm:534). Polled from JS so the panel opens for the engine's own button
// without the port having to hit-test that button itself.
EDEN_EXPORT
int eden_settings_menu_open(void) {
    if (!World::getWorld || !World::getWorld->menu) return 0;
    return World::getWorld->menu->showsettings ? 1 : 0;
}

EDEN_EXPORT
void eden_settings_menu_close(void) {
    if (!World::getWorld || !World::getWorld->menu) return;
    World::getWorld->menu->showsettings = FALSE;
    SettingsMenu* sm = eden_engine_settings();
    if (sm) { sm->save(); eden_apply_port_settings(); }
    // Menu::update returned early for every frame the panel was up, so any touch that was live
    // when it opened is still sitting in the slot table with a stale `inuse`. Clear the lot rather
    // than let the main menu act on a press that belonged to the panel.
    Input::getInput()->clearAll();
}

// Opens the engine's settings state from the port side (the in-game gear button). Only meaningful
// in the menu; in game the panel is shown by the JS on its own and this is not called.
EDEN_EXPORT
void eden_settings_menu_open_now(void) {
    if (!World::getWorld || !World::getWorld->menu) return;
    World::getWorld->menu->showsettings = TRUE;
}

// Plain getters for the port-owned globals eden-st.html polls every frame (crosshair visibility,
// hold-to-act repeat, fullscreen/fit mode, the Phase 6 opt-in). Cheaper and simpler for a
// polled-every-frame value than round-tripping through the schema's key->index lookup each time —
// that path is for the settings PANEL, which only needs it once per open.
EDEN_EXPORT
int eden_get_hold_to_act(void) { return eden_hold_to_act != 0.0f; }
EDEN_EXPORT
int eden_get_crosshair(void) { return eden_crosshair != 0.0f; }
EDEN_EXPORT
int eden_get_display_mode(void) { return (int)lroundf(eden_display_mode); }
EDEN_EXPORT
int eden_get_advanced_movement(void) { return eden_advanced_movement != 0.0f; }
// Not polled -- read once per save by save_backup.cpp (portable C++, no Settings_web.mm include),
// which is the actual reason this is a function and not a direct extern of the float above.
EDEN_EXPORT
int eden_get_save_backup(void) { return eden_save_backup != 0.0f; }

// Audit item #6. Returned as integer percent / hundredths rather than floats so the page does no
// index->value mapping of its own (the tables below are the only copy). Out-of-range indexes fall
// back to the neutral entry, not to a clamp: a stale persisted index from a future build should
// look like "default", not like "50%".
EDEN_EXPORT
int eden_get_render_scale_pct(void) {
    static const int kPct[] = {50, 75, 100, 125};
    int i = (int)lroundf(eden_render_scale);
    if (i < 0 || i >= (int)(sizeof(kPct) / sizeof(kPct[0]))) return 100;
    return kPct[i];
}
EDEN_EXPORT
int eden_get_dpr_cap_x100(void) {
    static const int kCap[] = {100, 150, 200};
    int i = (int)lroundf(eden_dpr_cap);
    if (i < 0 || i >= (int)(sizeof(kCap) / sizeof(kCap[0]))) return 200;
    return kCap[i];
}

// Row #14. Read every rAF tick by eden_main.cpp's frame gate — plain int, not a bool getter, since
// 0 means "uncapped" and any other value IS the target fps, no separate on/off flag needed.
EDEN_EXPORT
int eden_get_fps_cap(void) {
    static const int kFps[] = {0, 30, 45, 60};
    int i = (int)lroundf(eden_fps_cap);
    if (i < 0 || i >= (int)(sizeof(kFps) / sizeof(kFps[0]))) return 0;
    return kFps[i];
}

// ---------------------------------------------------------------------------------------------
// Table accessors for the ENGINE's own GL settings screen (Phase N Stage 2.5). Classes/
// SettingsMenu.mm iterates these to draw a row per kSettings[] entry generically — the same
// data public/eden-settings.js gets from eden_settings_schema(), but as scalars so a C++ caller
// needs no JSON parser. Flat one-liners in the eden_get_* style above. Values still go through
// eden_settings_get/set/toggle; these are metadata only.
//
// On web the GL panel is --wrap'd to a no-op so these are unused there; they exist on both
// targets because Settings_web.mm's portable half compiles into both (this is above the
// EDEN_LD_WRAP line on purpose).
EDEN_EXPORT int eden_settings_count(void) { return kSettingCount; }

static const Setting* eden_row(int i) {
    return (i >= 0 && i < kSettingCount) ? &kSettings[i] : NULL;
}

EDEN_EXPORT const char* eden_settings_label(int i) { const Setting* s = eden_row(i); return s ? s->label : ""; }
EDEN_EXPORT const char* eden_settings_group(int i) { const Setting* s = eden_row(i); return s ? s->group : ""; }
EDEN_EXPORT const char* eden_settings_key(int i)   { const Setting* s = eden_row(i); return s ? s->key   : ""; }
EDEN_EXPORT int   eden_settings_kind(int i) { const Setting* s = eden_row(i); return s ? s->kind : KIND_TOGGLE; }
EDEN_EXPORT float eden_settings_min(int i)  { const Setting* s = eden_row(i); return s ? s->min  : 0.0f; }
EDEN_EXPORT float eden_settings_max(int i)  { const Setting* s = eden_row(i); return s ? s->max  : 1.0f; }
EDEN_EXPORT float eden_settings_step(int i) { const Setting* s = eden_row(i); return s ? s->step : 1.0f; }
EDEN_EXPORT float eden_settings_def(int i)  { const Setting* s = eden_row(i); return s ? s->def  : 0.0f; }

// KIND_ENUM: `options` is a comma-separated label list. Count them, and copy label #j into a
// static scratch buffer (same no-malloc convention as eden_settings_schema()).
EDEN_EXPORT int eden_settings_enum_count(int i) {
    const Setting* s = eden_row(i);
    if (!s || !s->options || !s->options[0]) return 0;
    int n = 1;
    for (const char* p = s->options; *p; ++p) if (*p == ',') ++n;
    return n;
}

EDEN_EXPORT const char* eden_settings_enum_label(int i, int j) {
    static char buf[64];
    buf[0] = '\0';
    const Setting* s = eden_row(i);
    if (!s || !s->options || j < 0) return buf;
    const char* p = s->options;
    for (int k = 0; k < j; ++k) {
        p = std::strchr(p, ',');
        if (!p) return buf;
        ++p;
    }
    const char* end = std::strchr(p, ',');
    size_t len = end ? (size_t)(end - p) : std::strlen(p);
    if (len >= sizeof(buf)) len = sizeof(buf) - 1;
    std::memcpy(buf, p, len);
    buf[len] = '\0';
    return buf;
}

// Rows whose effect is not wired on native yet — the GL screen skips these rather than show a
// control that does nothing (render scale / DPR / UI scale / layout are the web drawable and
// point-space knobs; input_mode is DOM detection; legacy_menu is the DOM-vs-GL switch that is
// forced on here anyway).
EDEN_EXPORT int eden_settings_native_hidden(int i) {
    const Setting* s = eden_row(i);
    if (!s) return 1;
    static const char* const kHidden[] = {
        "render_scale", "dpr_cap", "display_mode", "ui_scale",
        "display_layout", "input_mode", "legacy_menu",
    };
    for (const char* h : kHidden) if (std::strcmp(s->key, h) == 0) return 1;
    return 0;
}


// =============================================================================================
// KEYBINDS — the C-side model (Phase N Stage 5.2)
// =============================================================================================
// WHAT THIS RETIRES. `web/public/eden-keybinds.js` owned DEFAULT_KEYMAP, ACTION_LABELS, the
// localStorage blob and the capture protocol, and its header called itself a "deliberate
// exception to 'settings live in C, never in the JS'" with a stated reason: *the C settings model
// stores floats only and NSUserDefaults persists NSNumber only, so a code->action map cannot live
// there.* That reason was true of the representation it assumed — `event.code` STRINGS — and only
// of that one.
//
// THE OBSERVATION THAT DISSOLVES IT. A physical key already has a numeric name that every target
// here agrees on: its **USB HID usage ID**. SDL3's `SDL_Scancode` values *are* those IDs
// (SDL_SCANCODE_A == 4 == HID usage 0x04), and the browser's `event.code` strings are a 1:1
// renaming of the same table (`KeyA` <-> 4). So a binding is an INT below 512, a float holds it
// exactly, and `[NSNumber numberWithInt:]` persists it — with no string storage anywhere and no
// change to the NSUserDefaults shim. kKeyNames[] below is that one shared table; it is the only
// place the browser spelling and the HID number are related to each other, on either target.
//
// WHY THESE ARE NOT `kSettings[]` ROWS. The plan (phase-n-stage3plus-plan §5.2) called for a
// `KIND_KEY` row type inside `Setting`. A keybind carries four fields a float row has no place
// for — the action id, a fixed secondary binding, and the continuous/momentary dispatch class —
// and `Setting.min/max/step/def` would all be dead weight. Widening `Setting` to fit would also
// grow `eden_settings_schema()`'s JSON by ~26 rows that the generic renderer cannot draw anyway.
// So keybinds get a parallel table with the same persistence mechanism and the same shape of API,
// and `eden_settings_reset_all()` resets both. KIND_KEY exists as a kind constant for the GL
// screen's row dispatch (Classes/KeybindsMenu), not as a member of kSettings[].
//
// CONFLICTS ARE ALLOWED, deliberately: `flyDown` and `crouch` ship bound to the same Ctrl on
// purpose (see the defaults below), so a "clear the other binding" rule would fight the shipped
// configuration on the first rebind. Duplicates are legal and every bound action fires.
// ---------------------------------------------------------------------------------------------

// The shared key table. `code` is the USB HID usage ID / SDL3 scancode; `name` is the browser's
// `event.code` spelling of the same key; `label` is what a UI shows. Ordered by code so the
// binary-search-free linear scans below stay predictable, and so a missing row is visible.
#define EDEN_KEY_TABLE(X) \
    X(4,"KeyA","A") X(5,"KeyB","B") X(6,"KeyC","C") X(7,"KeyD","D") X(8,"KeyE","E") \
    X(9,"KeyF","F") X(10,"KeyG","G") X(11,"KeyH","H") X(12,"KeyI","I") X(13,"KeyJ","J") \
    X(14,"KeyK","K") X(15,"KeyL","L") X(16,"KeyM","M") X(17,"KeyN","N") X(18,"KeyO","O") \
    X(19,"KeyP","P") X(20,"KeyQ","Q") X(21,"KeyR","R") X(22,"KeyS","S") X(23,"KeyT","T") \
    X(24,"KeyU","U") X(25,"KeyV","V") X(26,"KeyW","W") X(27,"KeyX","X") X(28,"KeyY","Y") \
    X(29,"KeyZ","Z") \
    X(30,"Digit1","1") X(31,"Digit2","2") X(32,"Digit3","3") X(33,"Digit4","4") \
    X(34,"Digit5","5") X(35,"Digit6","6") X(36,"Digit7","7") X(37,"Digit8","8") \
    X(38,"Digit9","9") X(39,"Digit0","0") \
    X(40,"Enter","Enter") X(41,"Escape","Esc") X(42,"Backspace","Backspace") X(43,"Tab","Tab") \
    X(44,"Space","Space") X(45,"Minus","-") X(46,"Equal","=") X(47,"BracketLeft","[") \
    X(48,"BracketRight","]") X(49,"Backslash","\\\\") X(51,"Semicolon",";") \
    X(52,"Quote","'") X(53,"Backquote","`") X(54,"Comma",",") X(55,"Period",".") \
    X(56,"Slash","/") X(57,"CapsLock","Caps") \
    X(58,"F1","F1") X(59,"F2","F2") X(60,"F3","F3") X(61,"F4","F4") X(62,"F5","F5") \
    X(63,"F6","F6") X(64,"F7","F7") X(65,"F8","F8") X(66,"F9","F9") X(67,"F10","F10") \
    X(68,"F11","F11") X(69,"F12","F12") \
    X(73,"Insert","Ins") X(74,"Home","Home") X(75,"PageUp","PgUp") X(76,"Delete","Del") \
    X(77,"End","End") X(78,"PageDown","PgDn") \
    X(79,"ArrowRight","Right") X(80,"ArrowLeft","Left") X(81,"ArrowDown","Down") \
    X(82,"ArrowUp","Up") \
    X(83,"NumLock","NumLock") X(84,"NumpadDivide","Num /") X(85,"NumpadMultiply","Num *") \
    X(86,"NumpadSubtract","Num -") X(87,"NumpadAdd","Num +") X(88,"NumpadEnter","Num Enter") \
    X(89,"Numpad1","Num 1") X(90,"Numpad2","Num 2") X(91,"Numpad3","Num 3") \
    X(92,"Numpad4","Num 4") X(93,"Numpad5","Num 5") X(94,"Numpad6","Num 6") \
    X(95,"Numpad7","Num 7") X(96,"Numpad8","Num 8") X(97,"Numpad9","Num 9") \
    X(98,"Numpad0","Num 0") X(99,"NumpadDecimal","Num .") \
    /* The modifier labels carry NO SPACE ("LShift", not "L Shift") on purpose: the GL keybinds
       screen word-wraps a label at its box width, and "also R Shift" is the one string in this
       table wide enough to wrap and clip itself into illegibility. */ \
    X(224,"ControlLeft","LCtrl") X(225,"ShiftLeft","LShift") X(226,"AltLeft","LAlt") \
    X(227,"MetaLeft","LMeta") X(228,"ControlRight","RCtrl") X(229,"ShiftRight","RShift") \
    X(230,"AltRight","RAlt") X(231,"MetaRight","RMeta")

struct KeyName { int code; const char* name; const char* label; };
static const KeyName kKeyNames[] = {
#define EDEN_KEY_ROW(c, n, l) { c, n, l },
    EDEN_KEY_TABLE(EDEN_KEY_ROW)
#undef EDEN_KEY_ROW
};
static const int kKeyNameCount = (int)(sizeof(kKeyNames) / sizeof(kKeyNames[0]));

// Internal name lookup. Returns NULL for a code this build has no row for, which is the
// "is this a key we can name?" test both the loader and the setter below use — a binding nobody
// can display is a binding nobody can undo from the UI.
static const char* eden_keybind_code_name_internal(int code) {
    for (int i = 0; i < kKeyNameCount; ++i)
        if (kKeyNames[i].code == code) return kKeyNames[i].name;
    return NULL;
}

// Scancode constants used by the defaults below, spelled out rather than #defined one by one.
// These are the same numbers SDL's headers give; native asserts the identity in its selftest so a
// future SDL that renumbered scancodes would fail loudly instead of rebinding everyone's keyboard.
enum {
    KC_A = 4, KC_C = 6, KC_D = 7, KC_E = 8, KC_F = 9, KC_L = 15, KC_O = 18,
    KC_S = 22, KC_V = 25, KC_W = 26, KC_B = 5,
    KC_1 = 30, KC_2 = 31, KC_3 = 32, KC_4 = 33, KC_5 = 34,
    KC_6 = 35, KC_7 = 36, KC_8 = 37, KC_9 = 38,
    KC_ESCAPE = 41, KC_SPACE = 44,
    KC_RIGHT = 79, KC_LEFT = 80, KC_DOWN = 81, KC_UP = 82,
    KC_LCTRL = 224, KC_LSHIFT = 225, KC_LALT = 226,
    KC_RCTRL = 228, KC_RSHIFT = 229, KC_RALT = 230,
};

// Dispatch class. CONTINUOUS actions are a held STATE the input translator keeps in a set;
// MOMENTARY ones are an EDGE that must not repeat-fire under the OS's auto-repeat. The split is
// eden-keybinds.js's CONTINUOUS_ACTIONS / MOMENTARY_ACTIONS, carried over verbatim — and the
// comment there about a blanket `if (e.repeat) return` having been silently lost once is the
// reason this is a per-action property rather than a rule at the call site.
enum { KB_CONTINUOUS = 1, KB_MOMENTARY = 2 };
// Which host can actually DO the action. `fullscreen` and `settings` have no native
// implementation (native has no DOM fullscreen and reaches settings through the menu), so the GL
// keybinds screen hides them rather than offer a control that does nothing — the same policy
// eden_settings_native_hidden() applies to the settings rows.
enum { KB_WEB = 1, KB_NATIVE = 2, KB_BOTH = 3 };

struct Keybind {
    const char* action;   // stable id; also the persistence key suffix and the JS-side id
    const char* label;
    const char* group;
    int   def;            // default primary code
    int   sec;            // FIXED secondary binding (not user-rebindable), 0 = none
    int   dispatch;       // KB_CONTINUOUS | KB_MOMENTARY
    int   avail;          // KB_WEB | KB_NATIVE
};

// The defaults ARE eden-keybinds.js's DEFAULT_KEYMAP, code for code — including the two
// deliberate oddities it documents: Space is jump-on-foot and ascend-while-flying (one action,
// because eden_set_jump ignores it while flying), and Ctrl is flyDown AND crouch on the same
// physical key (crouch shrinks the collision box regardless of fly state, so they never fight).
// F is the fire tool and V is fly-toggle, which is the swap a live playtest asked for.
static const Keybind kKeybinds[] = {
  // action        label                    group        def         sec        dispatch       avail
  { "moveForward", "Move forward",          "Movement",  KC_W,       KC_UP,     KB_CONTINUOUS, KB_BOTH },
  { "moveBack",    "Move back",             "Movement",  KC_S,       KC_DOWN,   KB_CONTINUOUS, KB_BOTH },
  { "moveLeft",    "Strafe left",           "Movement",  KC_A,       KC_LEFT,   KB_CONTINUOUS, KB_BOTH },
  { "moveRight",   "Strafe right",          "Movement",  KC_D,       KC_RIGHT,  KB_CONTINUOUS, KB_BOTH },
  { "sprint",      "Sprint",                "Movement",  KC_LSHIFT,  KC_RSHIFT, KB_CONTINUOUS, KB_BOTH },
  { "walk",        "Walk (slow)",           "Movement",  KC_LALT,    KC_RALT,   KB_CONTINUOUS, KB_BOTH },
  { "jump",        "Jump / fly up",         "Movement",  KC_SPACE,   0,         KB_CONTINUOUS, KB_BOTH },
  { "flyDown",     "Fly down",              "Movement",  KC_LCTRL,   KC_RCTRL,  KB_CONTINUOUS, KB_BOTH },
  { "crouch",      "Crouch",                "Movement",  KC_LCTRL,   KC_RCTRL,  KB_CONTINUOUS, KB_BOTH },

  { "fireTool",    "Fire tool",             "Actions",   KC_F,       0,         KB_MOMENTARY,  KB_BOTH },
  { "flyToggle",   "Toggle fly mode",       "Actions",   KC_V,       0,         KB_MOMENTARY,  KB_BOTH },
  { "blockPicker", "Block picker",          "Actions",   KC_E,       0,         KB_MOMENTARY,  KB_BOTH },
  { "colorPicker", "Colour picker",         "Actions",   KC_C,       0,         KB_MOMENTARY,  KB_BOTH },

  { "menu",        "Menu / pause",          "Interface", KC_ESCAPE,  0,         KB_MOMENTARY,  KB_BOTH },
  { "settings",    "Open settings",         "Interface", KC_O,       0,         KB_MOMENTARY,  KB_WEB  },
  { "blockPreview","Toggle block preview",  "Interface", KC_B,       0,         KB_MOMENTARY,  KB_BOTH },
  { "fullscreen",  "Fullscreen",            "Interface", KC_L,       0,         KB_MOMENTARY,  KB_WEB  },

  { "hotbar1", "Hotbar 1", "Hotbar", KC_1, 0, KB_MOMENTARY, KB_BOTH },
  { "hotbar2", "Hotbar 2", "Hotbar", KC_2, 0, KB_MOMENTARY, KB_BOTH },
  { "hotbar3", "Hotbar 3", "Hotbar", KC_3, 0, KB_MOMENTARY, KB_BOTH },
  { "hotbar4", "Hotbar 4", "Hotbar", KC_4, 0, KB_MOMENTARY, KB_BOTH },
  { "hotbar5", "Hotbar 5", "Hotbar", KC_5, 0, KB_MOMENTARY, KB_BOTH },
  { "hotbar6", "Hotbar 6", "Hotbar", KC_6, 0, KB_MOMENTARY, KB_BOTH },
  { "hotbar7", "Hotbar 7", "Hotbar", KC_7, 0, KB_MOMENTARY, KB_BOTH },
  { "hotbar8", "Hotbar 8", "Hotbar", KC_8, 0, KB_MOMENTARY, KB_BOTH },
  { "hotbar9", "Hotbar 9", "Hotbar", KC_9, 0, KB_MOMENTARY, KB_BOTH },
};
static const int kKeybindCount = (int)(sizeof(kKeybinds) / sizeof(kKeybinds[0]));

static int  g_keyCode[kKeybindCount];
static bool g_keybindsLoaded = false;

static const Keybind* eden_keybind_row(int i) {
    return (i >= 0 && i < kKeybindCount) ? &kKeybinds[i] : NULL;
}

// `keybind.moveForward` — namespaced so it cannot collide with a settings key, present or future.
static NSString* eden_keybind_prefs_key(int i) {
    static char buf[64];
    snprintf(buf, sizeof(buf), "keybind.%s", kKeybinds[i].action);
    return [NSString stringWithUTF8String:buf];
}

static void eden_keybinds_load(void) {
    if (g_keybindsLoaded) return;
    g_keybindsLoaded = true;
    NSUserDefaults* prefs = [NSUserDefaults standardUserDefaults];
    for (int i = 0; i < kKeybindCount; ++i) {
        g_keyCode[i] = kKeybinds[i].def;
        id stored = [prefs objectForKey:eden_keybind_prefs_key(i)];
        if (!stored) continue;
        int v = [(NSNumber*)stored intValue];
        // A code this build has no NAME for is a code this build cannot show, unbind or explain.
        // Fall back to the default rather than persist a number nobody can read — the same
        // "a stale index should look like default, not like a clamp" rule the video rows follow.
        if (v == 0 || eden_keybind_code_name_internal(v)) g_keyCode[i] = v;
    }
}

// ---------------------------------------------------------------------------------------------
// Public API. Everything crossing to JS passes INTS only, in both directions — the settings
// bridge's standing rule (public/eden-settings.js: passing a string INTO wasm would mean adding
// _malloc/_free to the export list). The name<->code direction JS needs therefore arrives as a
// table it reads once, not as a call per lookup.
// ---------------------------------------------------------------------------------------------

EDEN_EXPORT int eden_keybind_count(void) { return kKeybindCount; }

EDEN_EXPORT const char* eden_keybind_action(int i) { const Keybind* k = eden_keybind_row(i); return k ? k->action : ""; }
EDEN_EXPORT const char* eden_keybind_label(int i)  { const Keybind* k = eden_keybind_row(i); return k ? k->label  : ""; }
EDEN_EXPORT const char* eden_keybind_group(int i)  { const Keybind* k = eden_keybind_row(i); return k ? k->group  : ""; }
EDEN_EXPORT int eden_keybind_default(int i)   { const Keybind* k = eden_keybind_row(i); return k ? k->def : 0; }
EDEN_EXPORT int eden_keybind_secondary(int i) { const Keybind* k = eden_keybind_row(i); return k ? k->sec : 0; }
EDEN_EXPORT int eden_keybind_is_continuous(int i) { const Keybind* k = eden_keybind_row(i); return k && (k->dispatch & KB_CONTINUOUS) ? 1 : 0; }
EDEN_EXPORT int eden_keybind_is_momentary(int i)  { const Keybind* k = eden_keybind_row(i); return k && (k->dispatch & KB_MOMENTARY)  ? 1 : 0; }
// Mirrors eden_settings_native_hidden(): 1 = the GL screen must not offer this row.
EDEN_EXPORT int eden_keybind_native_hidden(int i) {
    const Keybind* k = eden_keybind_row(i);
    return (!k || !(k->avail & KB_NATIVE)) ? 1 : 0;
}

EDEN_EXPORT const char* eden_keybind_code_name(int code) {
    const char* n = eden_keybind_code_name_internal(code);
    return n ? n : "";
}

EDEN_EXPORT const char* eden_keybind_code_label(int code) {
    for (int i = 0; i < kKeyNameCount; ++i)
        if (kKeyNames[i].code == code) return kKeyNames[i].label;
    return "";
}

EDEN_EXPORT int eden_keybind_get(int i) {
    eden_keybinds_load();
    return (i >= 0 && i < kKeybindCount) ? g_keyCode[i] : 0;
}

EDEN_EXPORT void eden_keybind_set(int i, int code) {
    if (i < 0 || i >= kKeybindCount) return;
    if (code != 0 && !eden_keybind_code_name_internal(code)) return;   // unknown key: refuse
    eden_keybinds_load();
    g_keyCode[i] = code;
    NSUserDefaults* prefs = [NSUserDefaults standardUserDefaults];
    [prefs setObject:[NSNumber numberWithInt:code] forKey:eden_keybind_prefs_key(i)];
    [prefs synchronize];
}

EDEN_EXPORT void eden_keybind_reset_all(void) {
    eden_keybinds_load();
    for (int i = 0; i < kKeybindCount; ++i) eden_keybind_set(i, kKeybinds[i].def);
}

EDEN_EXPORT int eden_keybind_action_after(int prev, int code);

// The translators' hot path (once per key event, not per frame). Returns the FIRST action bound
// to `code`, or -1. Callers that must honour a deliberate double-binding — Ctrl is flyDown AND
// crouch out of the box — iterate with eden_keybind_action_after() instead of stopping here.
EDEN_EXPORT int eden_keybind_action_for_code(int code) {
    return eden_keybind_action_after(-1, code);
}

// Iterator form: pass the previous result (or -1 to start), get the next action bound to `code`,
// or -1 when there are no more. Exists because "one key, two actions" is shipped configuration
// and not an edge case, and because a rebind can create more of them at any time.
EDEN_EXPORT int eden_keybind_action_after(int prev, int code) {
    if (code == 0) return -1;
    eden_keybinds_load();
    for (int i = (prev < 0 ? 0 : prev + 1); i < kKeybindCount; ++i)
        if (g_keyCode[i] == code || (kKeybinds[i].sec != 0 && kKeybinds[i].sec == code)) return i;
    return -1;
}

EDEN_EXPORT int eden_keybind_index(const char* action) {
    for (int i = 0; i < kKeybindCount; ++i)
        if (std::strcmp(kKeybinds[i].action, action) == 0) return i;
    return -1;
}

// ---------------------------------------------------------------------------------------------
// Rebind CAPTURE. Arming lives here, not in a screen, because the key that closes a capture
// arrives on a path no GL screen can see: the engine's Input has touches and nothing else, so on
// native the scancode comes in through Input_native.cpp's SDL handler and has to reach whatever
// screen asked for it. One armed slot in the model is the only place both halves can agree on.
//
// Web does NOT use this — it captures in the DOM (public/eden-keybinds.js), because there the
// keydown must also be swallowed before it reaches the page's own listeners, which is a DOM
// concern with a DOM answer. Two capture front-ends, one model underneath.
// ---------------------------------------------------------------------------------------------
static int g_captureRow = -1;

EDEN_EXPORT void eden_keybind_capture_begin(int i) {
    g_captureRow = (i >= 0 && i < kKeybindCount) ? i : -1;
}
EDEN_EXPORT void eden_keybind_capture_cancel(void) { g_captureRow = -1; }
EDEN_EXPORT int  eden_keybind_capture_active(void) { return g_captureRow; }

// The platform input layer hands EVERY key here first while a capture is armed. Returns nonzero
// if the key was consumed (so the caller must not also dispatch it as gameplay input).
//
// Escape CANCELS rather than binds, and that costs Escape the ability to be rebound onto from
// this screen — a deliberate trade, because Escape is the only key guaranteed to exist on every
// keyboard and a capture with no way out strands the player in a modal with no menu key.
EDEN_EXPORT int eden_keybind_capture_feed(int code) {
    if (g_captureRow < 0) return 0;
    const int row = g_captureRow;
    g_captureRow = -1;
    if (code == 41 /* Escape */) return 1;          // cancel, binding unchanged
    if (!eden_keybind_code_name_internal(code)) return 1;   // unknown key: swallow, change nothing
    eden_keybind_set(row, code);
    return 1;
}

// The panel's whole keybind model as JSON, same contract as eden_settings_schema(): one string so
// the C table stays the single source of truth and no JS file hard-codes an action. `code` is the
// live primary binding, so this is re-read after a rebind rather than patched in place.
EDEN_EXPORT const char* eden_keybinds_schema(void) {
    static char buf[8192];
    eden_keybinds_load();
    int n = 0;
    n += snprintf(buf + n, sizeof(buf) - n, "[");
    for (int i = 0; i < kKeybindCount && n < (int)sizeof(buf) - 1; ++i) {
        const Keybind& k = kKeybinds[i];
        n += snprintf(buf + n, sizeof(buf) - n,
            "%s{\"i\":%d,\"action\":\"%s\",\"label\":\"%s\",\"group\":\"%s\",\"code\":%d,"
            "\"sec\":%d,\"def\":%d,\"continuous\":%d,\"momentary\":%d}",
            i ? "," : "", i, k.action, k.label, k.group, g_keyCode[i], k.sec, k.def,
            (k.dispatch & KB_CONTINUOUS) ? 1 : 0, (k.dispatch & KB_MOMENTARY) ? 1 : 0);
    }
    const bool truncated = (n >= (int)sizeof(buf) - 2);
    snprintf(buf + n, sizeof(buf) - n, "]");
    if (truncated) {
        std::fprintf(stderr, "[eden-keybinds] SCHEMA JSON TRUNCATED at %d bytes — grow buf[] in "
                             "eden_keybinds_schema(). The Keys tab will not render.\n", n);
    }
    return buf;
}

// kKeyNames[] as JSON: the `event.code` <-> HID-code dictionary the page needs to turn a real
// KeyboardEvent into a binding. Read once and cached JS-side; it never changes at runtime.
EDEN_EXPORT const char* eden_keybind_code_table(void) {
    static char buf[6144];
    int n = 0;
    n += snprintf(buf + n, sizeof(buf) - n, "[");
    for (int i = 0; i < kKeyNameCount && n < (int)sizeof(buf) - 1; ++i) {
        n += snprintf(buf + n, sizeof(buf) - n, "%s{\"c\":%d,\"n\":\"%s\",\"l\":\"%s\"}",
                      i ? "," : "", kKeyNames[i].code, kKeyNames[i].name, kKeyNames[i].label);
    }
    const bool truncated = (n >= (int)sizeof(buf) - 2);
    snprintf(buf + n, sizeof(buf) - n, "]");
    if (truncated) {
        std::fprintf(stderr, "[eden-keybinds] CODE TABLE JSON TRUNCATED at %d bytes — grow buf[] "
                             "in eden_keybind_code_table(). Rebinding will not work.\n", n);
    }
    return buf;
}

// ---------------------------------------------------------------------------------------------
// EDEN_LD_WRAP — Phase N Stage 1 (WORKING/native-migration-plan-2026-09-04.md).
//
// Everything below this line is a `-Wl,--wrap=` interposer, and --wrap is a GNU-ld/wasm-ld
// feature that **Apple's ld64 does not have**. The plan says to decide per symbol rather than as
// a blanket policy, so the file is split rather than excluded: the portable part above (the
// settings model / the movement exports / the timing accumulators) compiles into BOTH targets,
// and only these interposers are web-only. web/CMakeLists.txt defines EDEN_LD_WRAP alongside the
// matching -Wl,--wrap= flags, so the two can never drift apart — dropping a wrap flag without
// dropping its function would be a link error, not a silent behaviour change.
//
// WHAT NATIVE LOSES BY NOT HAVING THESE, stated so it is a decision and not an omission:
//   Settings   — the FOV slider (gluPerspective keeps the engine's hard-coded 80 degrees) and the
//                neutralisation of the legacy GL settings panel. Native WANTS that panel live:
//                the GL UI is native's UI until Stage 5, and Stage 2.5 is about extending it.
//   Movement   — frame-rate-normalised walk speed and the opt-in bhop tweak. Both are PC-input
//                polish that Stage 2 re-lands through the SDL input path, where the call site is
//                ours to edit and no linker trick is needed.
//   MeshTiming — measurement only. Native has a profiler.
// None of the three touches vertex data, which is what Stage 1's byte-identical-geometry
// criterion compares.
// ---------------------------------------------------------------------------------------------
}  // extern "C" — the portable half of this file ends here
#ifdef EDEN_LD_WRAP
extern "C" {

// ---------------------------------------------------------------------------------------------
// --wrap: the old GL settings panel. See this file's header for why both halves are neutralised
// rather than the class being seam-replaced wholesale.
// ---------------------------------------------------------------------------------------------
void __real__ZN12SettingsMenu6updateEf(SettingsMenu* self, float etime);
void __wrap__ZN12SettingsMenu6updateEf(SettingsMenu* self, float etime) {
    (void)self; (void)etime;   // the DOM panel owns this input now
}

void __real__ZN12SettingsMenu6renderEv(SettingsMenu* self);
void __wrap__ZN12SettingsMenu6renderEv(SettingsMenu* self) {
    (void)self;                // ...and this pixel area; Menu::render still draws menu_back under it
}

// --wrap: FOV. `Classes/Graphics.mm:370` hard-codes `gluPerspective(80, ...)` and there is no
// engine-side setting for it, but gluPerspective itself lives in Classes/project.c (the vendored
// GLU port) — a different translation unit, so the call is interceptable. Graphics::prepareScene
// is the only live caller (grep: the other two mentions are commented out), so overriding fovy
// unconditionally is safe. Picking follows for free: Util.mm's unproject reads the resulting
// projection matrix back with glGetFloatv rather than assuming 80 degrees.
void __real_gluPerspective(double fovy, double aspect, double zNear, double zFar);
void __wrap_gluPerspective(double fovy, double aspect, double zNear, double zFar) {
    (void)fovy;
    __real_gluPerspective((double)eden_fov_degrees, aspect, zNear, zFar);
}

}  // extern "C"
#endif  // EDEN_LD_WRAP
