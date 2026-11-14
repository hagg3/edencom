// Settings.cpp — the port's settings model (pass 28).
//
// WHAT THIS REPLACES
// `Classes/SettingsMenu.mm` draws a fixed 5-row GL panel of ON/OFF images with hard-coded pixel
// rects (`rect_on[j].origin.x = 300+115` and friends), reachable only from the main menu's Options
// button. It is not extensible — every row is a hand-placed texture and a hard-coded `if` in both
// update() and render() — and there is nowhere to put a slider at all.
//
// Every setting, the stock five (Health, Auto-jump, Creatures, Sound effects, Music) included, is one
// row of kSettings[] (what it looks like) plus one entry of kBindings[] (what its value does: a float
// it is copied to, an apply hook, or both). QA.19 replaced the 80-branch strcmp chain and the stock
// rows' second store (SettingsMenu::properties[]/save(), whose load() stomped port rows) with that
// table; a value row without a binding fails eden_settings_check_bindings (--console-selftest).
//
// PERSISTENCE is eden_prefs for everything (G.2b-1; NSUserDefaults before): a row's value in
// thousandths under its key, except the stock five, which keep stock 2.1's names and 0/1 ints
// ("Health", "Autojump", "Creatures", "Sound effects", "Music": SettingBinding::stock) so existing
// prefs files read back. SettingsMenu::load() still reads those five once at construction, through
// eden_settings_boot_stock(), so the title tune obeys Music before this model loads.
#include "../shim/foundation/eden_base.h"
#include "../shim/foundation/eden_image.h"
#include "../shim/foundation/eden_prefs.h"
#include "eden_settings.h"   // QA.19: the accessors below, declared once
#include "../../../Classes/World.h"
#include "../../../Classes/Menu.h"
#include "../../../Classes/SettingsMenu.h"
#include "../../../Classes/Resources.h"
#include "../../../Classes/Player.h"        // QA.19: health/autojump/invert_look targets
#include "../../../Classes/MpWorld.h"       // QA.19: creatures (MpWorld::gateCreatures)
#include "../../../Classes/Globals.h"       // CREATURES_ON
#include "../../../Classes/Input.h"
#include "../../../Classes/SimpleAudioEngine.h"
#include "../../../Classes/Shadow.h"          // H.2: the shadows row
#include <string>
#include <cstdlib>   // getenv (S.5e: EDEN_UPGRADE_256Z)
#include "DisplayProfile.h"
#include "../shim/foundation/platform_shims.h"   // (Phase N Stage 1)
#include "../../../Classes/Renderer.h"           // G.5b-4: Original lighting
#include <cstdio>
#include <cstring>
#include <cmath>
#include "../../../Classes/SrclikeMove.h"   // SRC.2: srclike_movement / srclike_autobhop
#include "../../../Classes/SkyCycle.h"    // DN.2: day_night_cycle / day_length
#include "../../../Classes/Weather.h"     // SX.6: snowline / mistheight
#include "../../../Classes/TouchLook.h"   // Stage L: TOUCH_LOOK_* globals the touch_look_* rows write

// ---------------------------------------------------------------------------------------------
// The schema. Order here IS the display order; `group` drives the section headings in the panel.
// `kind`: 0 = toggle (0/1), 1 = range, 2 = enum (a small label list, value is the 0-based index
// into `options`). What a row's value does is its kBindings[] entry (QA.19), further down.
// ---------------------------------------------------------------------------------------------
// KIND_KEY (3) is NOT used by any kSettings[] row — it is the row-kind constant the GL keybinds
// screen dispatches on, and it lives here so the kind space stays one enumeration. See the
// KEYBINDS section near the bottom of this file for why a keybind is a parallel table.
// MP.8: KIND_TEXT (4) is a string row (Experiments > Matchmakers): its value lives in g_text[], not
// g_value[], persists as an NSString, and has no float meaning (min/max/step/def are unused).
// SX.1 (WORKING/settings-ux-audit-2026-11-06.md §4.1): KIND_PAGE (5) is an "Open" row that opens the
// sub-page named in its `options` (SettingsMenu's kPages[]); KIND_HEADING (6) is a label-only section
// heading. Neither has a value: never persisted, never a cvar (the console skips kind >= KIND_TEXT).
// SX.6 (audit §4.2): KIND_CVAR (7) is a row that SHOWS a console variable owned elsewhere (`options` = its
// name, e.g. "world.time"): the GL screen draws it as the cvar's own kind with the cvar's range, and reads and
// writes through the cvar's get/set. No value here: never persisted (the owner saves it: world ones in the
// .emod at the next commit), never re-bridged as a cvar. A value that persists in eden_prefs is an ordinary
// row instead, and its cvar is derived from it (eden_settings_cvar): one table either way.
enum { KIND_TOGGLE = 0, KIND_RANGE = 1, KIND_ENUM = 2, KIND_KEY = 3, KIND_TEXT = 4, KIND_PAGE = 5, KIND_HEADING = 6, KIND_CVAR = 7 };
// SX.6: when the GL screen shows a row. SCOPE_WORLD rows need a world in play (in a multiplayer session they
// stay, badged "temporary": U3); SCOPE_NO_WORLD rows stand in for them on the title screen's Settings.
enum { SCOPE_ALWAYS = 0, SCOPE_WORLD = 1, SCOPE_NO_WORLD = 2 };

struct Setting {
    const char* key;      // persistence key AND the JS-side id
    const char* label;
    const char* group;
    int   kind;
    float min, max, step;
    float def;
    const char* hint;     // one line of explanation, shown under the control
    const char* options;  // KIND_ENUM: comma-separated labels, e.g. "Auto,Touch,Keyboard+Mouse". KIND_PAGE: the page id it opens
    const char* parent;   // key of a KIND_TOGGLE row this one depends on (muted while that is off), or NULL.
                          // TRAILING on purpose: positional init leaves it NULL for every row that omits it.
    // SX.1: DISPLAY placement only, decoupled from `group`. `group` names the console variable
    // (lower(group) + "." + key) and must never change for a shipped row; `page` says where the row is
    // drawn: a sub-page id from SettingsMenu's kPages[] or another tab's group. NULL = the group's tab.
    // Trailing, like `parent`, so rows that omit it are untouched.
    const char* page;
    int scope;            // SX.6: SCOPE_*; trailing, 0 = always shown
};

static const Setting kSettings[] = {
  // key                 label                group        kind         min  max  step  def   hint                                                                options
  { "health",            "Health",            "Gameplay",  KIND_TOGGLE, 0,   1,   1,   1,  "Falls, fire and creatures hurt.",                     NULL },
  { "autojump",          "Auto-jump",         "Gameplay",  KIND_TOGGLE, 0,   1,   1,   1,  NULL,                            NULL },
  { "creatures",         "Creatures",         "Gameplay",  KIND_TOGGLE, 0,   1,   1,   1,  NULL,                       NULL },
  { "crouch",            "Crouch mode",       "Experiments", KIND_TOGGLE, 0,   1,   1,   0,  "Adds a crouch key for 1-block gaps.", NULL, NULL, "Gameplay" },
  { "hotbar",            "Hotbar",            "Gameplay",  KIND_TOGGLE, 0,   1,   1,   1,  "Nine quick slots for blocks and colours.", NULL },
  // Fly mode lives in Gameplay (was Experiments). The four sliders default to the original feel
  // (accel 10x walk, drag .995/frame, rise/fall JUMP_SPEED*3, no speed cap); see FLY_* in Classes/Player.mm.
  { "fly",               "Allow fly",         "Gameplay",  KIND_TOGGLE, 0,   1,   1,   0,  NULL,                   NULL },
  { "fly_accel",         "Fly acceleration",  "Gameplay",  KIND_RANGE,  .05f,   1, .05f,   1,  "Lower is gentler and easier to hold still.", NULL, "fly" },
  { "fly_max_speed",     "Fly top speed",     "Gameplay",  KIND_RANGE,  0, 100,   1,   0,  "Blocks per second. 0 is unlimited.", NULL, "fly" },
  { "fly_braking",       "Fly braking",       "Gameplay",  KIND_RANGE,  0,   1, .05f,   0,  "Higher stops sooner when you let go.", NULL, "fly" },
  { "fly_vertical",      "Fly rise/fall speed", "Gameplay", KIND_RANGE, .05f,   1, .05f,   1,  NULL, NULL, "fly" },
  { "noclip_speed",      "Noclip speed",      "Gameplay",  KIND_RANGE,  10, 100,   5,  60,  "Blocks per second while noclipping.", NULL, "fly" },
  { "heading_world_all", "All worlds", "World", KIND_HEADING, 0, 0, 0, 0, NULL, NULL },
  { "day_night_cycle",   "Day/night cycle",   "Gameplay", KIND_TOGGLE, 0,   1,   1,   0,  "The sky and light follow a clock. Overrides sky painting.", NULL, NULL, "World" },
  { "day_length",        "Day length",        "Gameplay", KIND_ENUM,   0,   3,   1,   1,  "Real minutes per full day.", "10 min,20 min,40 min,60 min", "day_night_cycle", "World" },
  // AT.1a (WORKING/atmosphere-plan-2026-11-05.md A6): random rain/snow/storms/mist. Works with the cycle off.
  // Classes/Weather (AT.1b) reads eden_weather_enabled(); off = nothing runs, nothing is saved.
  { "weather",           "Weather",           "Gameplay", KIND_TOGGLE, 0,   1,   1,   0,  "Rain, snow, storms and mist come and go.", NULL, NULL, "World" },
  // SX.9: how often (clear spells' length) and how often storms follow rain; Normal = the plan's shares.
  { "frequency",         "Weather frequency", "Weather", KIND_ENUM, 0,   2,   1,   1,  "How often it rains or snows. Normal is about 4 days in 10 overcast or wet.", "Rare,Normal,Often", "weather", "World" },
  { "storms",            "Storms",            "Weather", KIND_ENUM, 0,   3,   1,   2,  "How often rain turns into a thunderstorm.", "Off,Rare,Normal,Often", "weather", "World" },
  { "weather_tuning_open", "Weather tuning",  "World", KIND_PAGE, 0,   0,   0,   0,  "Snowline and mist height.", "weathertune", "weather" },
  // SX.6: the open world's own clock and weather (audit §4.3), through their cvars. Saved with the world at its
  // next save (records SKY_CLOCK, WEATHER); in a multiplayer session they only change this device's view (U3).
  { "heading_world_this", "This world", "World", KIND_HEADING, 0, 0, 0, 0, "Saved with this world.", NULL, NULL, NULL, SCOPE_WORLD },
  { "heading_world_none", "This world", "World", KIND_HEADING, 0, 0, 0, 0, "Open a world to change its time and weather.", NULL, NULL, NULL, SCOPE_NO_WORLD },
  { "world_time",        "Time of day",       "World", KIND_CVAR, 0,   0,   0,   0,  "Sets this world's clock. Noon is 12:00.", "world.time", NULL, NULL, SCOPE_WORLD },
  { "world_locktime",    "Lock time of day",  "World", KIND_CVAR, 0,   0,   0,   0,  "Keeps this world's clock still.", "world.locktime", "day_night_cycle", NULL, SCOPE_WORLD },
  { "weather_now",       "Weather now",       "World", KIND_CVAR, 0,   0,   0,   0,  "Starts that weather here now.", "weather.now", "weather", NULL, SCOPE_WORLD },
  { "weather_lock",      "Lock weather",      "World", KIND_CVAR, 0,   0,   0,   0,  "The current weather never ends.", "weather.lock", "weather", NULL, SCOPE_WORLD },
  // SX.6: World > Weather tuning. Group "Weather" makes them the weather.snowline / weather.mistheight cvars
  // (they were hand-made cvars holding session values; now persisted like any setting, for all worlds).
  { "snowline",          "Snowline",          "Weather", KIND_RANGE, 0,   1, .05f, .75f, "Snow falls at and above this share of the world height. The default map's snow mountain always snows.", NULL, NULL, "weathertune" },
  { "mistheight",        "Mist height",       "Weather", KIND_RANGE, 0,   1, .05f, .40f, "Top of the valley mist at dawn and in rain, as a share of the world height. Atmosphere High only.", NULL, NULL, "weathertune" },

  // SX.3: two clean trees. Ambience beds follow the Music toggle (Resources::soundEventBed), so they are Music children (U7).
  { "master_volume",     "Master volume",     "Audio",     KIND_RANGE,  0,   1, .05f,  1,  "Overall loudness, on top of the sliders below.",                  NULL },
  { "mute_in_background", "Mute in background", "Audio",   KIND_TOGGLE, 0,   1,   1,   0,  "Silence the game while its window is not in focus.",            NULL },
  { "music",             "Music",             "Audio",     KIND_TOGGLE, 0,   1,   1,   1,  NULL,                                                               NULL },
  { "music_volume",      "Music volume",      "Audio",     KIND_RANGE,  0,   1, .05f,  1,  NULL,                                                               NULL, "music" },
  { "ambience_volume",   "Ambience volume",   "Audio",     KIND_RANGE,  0,   1, .05f,  1,  NULL,                                                               NULL, "music" },
  { "night_ambience", "Night ambience", "Audio", KIND_TOGGLE, 0,   1,   1,   0,  "Night sounds; needs the day/night cycle.", NULL, "music" },
  { "sound",             "Sound effects",     "Audio",     KIND_TOGGLE, 0,   1,   1,   1,  NULL,                                                               NULL },
  { "effects_volume",    "Effects volume",    "Audio",     KIND_RANGE,  0,   1, .05f,  1,  NULL,                                                               NULL, "sound" },
  { "creature_walk_voices", "Creature walking sounds", "Audio", KIND_TOGGLE, 0,   1,   1,   0,  "Creatures call out as they start walking.", NULL, "sound" },
  { "touch_controls_sound", "Touch controls sound", "Audio", KIND_TOGGLE, 0,   1,   1,   0,  NULL, NULL },

  // input_mode: 0=Auto (matchMedia + first-input detection, see eden-st.html), 1=Touch,
  // 2=Keyboard+Mouse. Auto is the default so nothing changes for players who never open Settings.
  { "input_mode",        "Input mode",        "Controls",  KIND_ENUM,   0,   2,   1,   0,  "Force one if Auto guesses wrong. Menus work in every mode.", "Auto,Touch,Keyboard+Mouse" },
  { "hold_to_act",       "Hold to mine/build", "Controls",  KIND_TOGGLE, 0,   1,   1,   1,  "Repeat while the button is held.", NULL },
  { "mouse_sensitivity", "Mouse sensitivity", "Controls",  KIND_RANGE,  .25f,  3, .05f,  1,  NULL,                         NULL },
  { "mouse_sensitivity_y", "Mouse sensitivity (Y)", "Controls", KIND_RANGE, .25f,  3, .05f,  1,  "Vertical, separate from horizontal.",  NULL },
  { "invert_look",       "Invert look",       "Controls",  KIND_TOGGLE, 0,   1,   1,   0,  NULL,                                                               NULL },
  // Stage L: touch drag-camera rows (Classes/TouchLook.h). Shown only with touch input; the mouse rows are
  // hidden then (eden_settings_native_hidden). Smoothing 0 = raw exactly. Glide is a half-life in seconds, 0 = off (default).
  { "touch_look_speed",  "Touch look speed",  "Controls",  KIND_RANGE,  .25f,  3, .05f,  1,  "Drag camera, left-right.", NULL },
  { "touch_look_speed_y", "Touch look speed (Y)", "Controls", KIND_RANGE, .25f,  3, .05f,  1,  "Drag camera, up-down.", NULL },
  { "touch_look_smoothing", "Touch look smoothing", "Controls", KIND_RANGE, 0,   1, .05f, .2f, "0 follows your finger exactly.", NULL },
  { "touch_look_glide", "Touch look glide", "Controls", KIND_RANGE, 0, .6f, .05f,  0,  "Camera keeps turning after you lift your finger. 0 stops at once.", NULL },
  // Row #24 (gamepad). Two translators read these now — public/eden-gamepad.js on web and
  // port/src/seam/Input_native.cpp (Phase N Stage 2) on native — both of them pure translators
  // over the existing input entry points. The rows deliberately do
  // not touch the engine (engine >= 0). ON by default because the module is inert until a pad is
  // actually connected AND has been interacted with, so it costs a disconnected player nothing.
  { "gamepad",           "Gamepad",           "Controls",  KIND_TOGGLE, 0,   1,   1,   1,  "Use a connected controller.", NULL },
  { "gamepad_look_sensitivity", "Gamepad look speed", "Controls", KIND_RANGE, .25f,  3, .05f,  1,  NULL,        NULL, "gamepad" },
  { "gamepad_deadzone",  "Gamepad deadzone",  "Controls",  KIND_RANGE,  .05f, .5f, .05f, .15f, "Raise it if the camera drifts.", NULL, "gamepad" },

  { "heading_video_quality", "Quality", "Video", KIND_HEADING, 0, 0, 0, 0, NULL, NULL },
  // B.8: a shortcut over view_distance + render_scale (see kPresets[]). Stored like any row, but
  // moving either knob by hand drops it to Custom. Default reproduces the pre-B.8 look exactly.
#if defined(EDEN_PLATFORM_IOS)
  { "graphics_preset",   "Graphics preset",   "Video",     KIND_ENUM,   0,   3,   1,   2,  "Sets view distance, render scale and atmosphere.", "Custom,Low,Default,High" },
#else
  { "graphics_preset",   "Graphics preset",   "Video",     KIND_ENUM,   0,   3,   1,   2,  "Sets view distance, render scale and atmosphere.", "Custom,Low,Default,High" },
#endif
  // B.5a: draw-culling only (Graphics::setZFAR); a "less" knob for weak CPUs/GPUs. Auto = the per-device value.
  { "view_distance",     "View distance",     "Video",     KIND_ENUM,   0,   6,   1,   0,  "How far terrain is drawn. Lower is faster.", "Auto,4,5,6,7,8,9" },
  // B.5b-2: the resident window's radius in chunks (kRenderRadius[] below), read once per world
  // entry by eden_window_radius_for_world() -- the arrays are sized from it, so it cannot change
  // under an open world. 9 is stock. iOS offers 12 at most, opt-in (P.4 decision 5): T.B16 measured
  // the iPad Air 2 at R12 160 MiB (64z) and 240 MiB (Quarry-dense 256z), under the 300 MB bar.
  // X.17: 4-8 added below 9 (T_RADIUS_MIN is 4; the runtime window ran ASan clean at R4). The stored value is an
  // INDEX, so prepending options needed a new key: "render_radius" replaced "render_distance" (9,10,12,16 = old
  // indexes 0-3 = new 5-8), migrated once in eden_settings_init.
#if defined(EDEN_PLATFORM_IOS)
  { "render_radius",     "Render distance",   "Video",     KIND_ENUM,   0,   7,   1,   5,  "Applies when a world opens.", "4,5,6,7,8,9,10,12" },
#else
  { "render_radius",     "Render distance",   "Video",     KIND_ENUM,   0,   8,   1,   5,  "Applies when a world opens.", "4,5,6,7,8,9,10,12,16" },
#endif
  // Audit item #6 (§4a/§4c.1-2): the drawable used to be pinned to 1136x640 with devicePixelRatio
  // never read at all, so on any modern display the game was a small buffer upscaled — permanently
  // blurry. These two rows are the knobs on the now-dynamic drawable that public/eden-st.html
  // computes (CSS box x min(devicePixelRatio, dpr_cap) x render_scale). Neither touches the
  // engine's 568x320 POINT space; see eden_set_drawable_size() in the GL shim.
  // Defaults reproduce "as sharp as the display allows, no supersampling": cap 2x, scale 100%.
#if defined(EDEN_PLATFORM_IOS)
  // iOS offers no 125% (N.4.11b): on an iPad Air 2 it took the footprint 372 -> 601 MB, kept 564 MB
  // after going back to 100%, and bought nothing (the device is not fill-bound). max 2 makes
  // eden_settings_set() clamp to 100%; eden_get_render_scale_pct() reads a stored 3 as 100%.
  // Hidden and read as 100% from N.4.12 until FT.16 (2026-10-28) replaced the blit with a quad.
  // The default is per device: 75% on Air 2-class hardware (eden_ios_air2_class), 100% elsewhere.
  { "render_scale",      "Render scale",      "Video",     KIND_ENUM,   0,   2,   1,   2,  "Below 100% is softer but cooler and faster.", "50%,75%,100%" },
#else
  { "render_scale",      "Render scale",      "Video",     KIND_ENUM,   0,   3,   1,   2,  "Below 100% is softer but cooler and faster.", "50%,75%,100%,125%" },
#endif
  // H.2: baked top-face shadows (Classes/Shadow.h). CPU meshing only (< 1 % computed), so not in the
  // presets. Default Off until the user's T look picks the shipped one (ROADMAP Stage H).
  { "shadows",           "Shadows",           "Video",     KIND_ENUM,   0,   2,   1,   1,  "Soft shading under overhangs. Smooth costs a little more meshing.", "Off,Classic,Smooth" },
  // Lamp light in shadows: off = a shaded top face by day ignores coloured lamp light (shadows are plain grey).
  { "shadow_lamp_tint",   "Tinted lamp shadows", "Video",    KIND_TOGGLE, 0,   1,   1,   0,  "Lamp light colours shadows, even by day.", NULL },
  // AT.1a: the Atmosphere tier (A7). Default High; eden_apply_profile_defaults seeds Off on Air 2-class iOS.
  // A tier change writes the Atmo-group element rows below (P10); a preset knob (kPresets[].atmosphere).
  { "atmosphere",        "Atmosphere",        "Video",     KIND_ENUM,   0,   3,   1,   2,  "Clouds, precipitation and ambient particles.", "Off,Low,High,Custom" },
  { "atmosphere_details_open", "Atmosphere details", "Video", KIND_PAGE, 0,   0,   0,   0,  "Switch each element on or off. Edits show as Custom.", "atmosphere" },
  // Row #14: an opt-in frame-rate ceiling, mainly for thermal/battery on touch devices (a voxel
  // game at an uncapped rAF is a thermal-throttle machine there — perf-audit §4b). Uncapped is the
  // default for everyone; eden_apply_input_profile() below sets it to 60 the first time a touch
  // profile is detected AND the player has never explicitly touched this row.
#if defined(EDEN_PLATFORM_IOS)
  // P.14 (decision P.4-4): no 45 on iOS -- at vsync 1 on a 60 Hz panel it alternates 16.7 / 33.3 ms
  // frames. The list is 3 long here, so the stored index differs from the 4-entry list below:
  // eden_get_fps_cap() maps it, the profile seeder remaps, and eden_settings_init migrates once.
  { "fps_cap",           "Frame rate cap",    "Video",     KIND_ENUM,   0,   2,   1,   0,  "30 or 60 saves battery and heat.", "Uncapped,30,60" },
#else
  { "fps_cap",           "Frame rate cap",    "Video",     KIND_ENUM,   0,   3,   1,   0,  "30 or 60 saves battery and heat.", "Uncapped,30,45,60" },
#endif
  // G.5b-4 (renderer plan §7): ES 1.1 per-vertex lighting as 2.1 drew it. Off = the port's unlit look
  // before G.5b-4. Read by the GL backend's drawLit (R8); creatures are lit either way. Not in the presets.
  { "original_lighting", "Original lighting", "Video",     KIND_TOGGLE, 0,   1,   1,   1,  "Shading on debris and fireworks, as in Eden 2.1.", NULL },
  { "heading_video_view", "View", "Video", KIND_HEADING, 0, 0, 0, 0, NULL, NULL },
  { "fov",               "Field of view",     "Video",     KIND_RANGE,  60, 110,   1,  80,  "80 is the original.",                    NULL },
  { "crosshair",         "Crosshair",         "Interface", KIND_TOGGLE, 0,   1,   1,   1,  NULL,             NULL },
  { "block_preview",     "Block preview",     "Interface", KIND_TOGGLE, 0,   1,   1,   0,  "Shows where the next block goes.",      NULL },
  { "ui_scale",          "UI scale",          "Video",     KIND_ENUM,   0,   4,   1,   0,  "Size of menus and HUD. Auto picks for this device.", "Auto,100%,125%,150%,200%" },
  { "heading_video_comfort", "Comfort", "Video", KIND_HEADING, 0, 0, 0, 0, NULL, NULL },
  { "reduce_flashing",   "Reduce flashing",   "Video",     KIND_TOGGLE, 0,   1,   1,   0,  "Lightning dims the sky gently instead of flashing.", NULL },

  // AT.1a P10: one switch per element, set by the tier above (kAtmoTier[]). Group Atmo = the atmo.* cvars; hidden from the panel.
  { "precip",            "Rain and snow",       "Atmo",       KIND_TOGGLE, 0,   1,   1,   1,  NULL, NULL, NULL, "atmosphere" },
  { "splash",            "Rain splashes",       "Atmo",       KIND_TOGGLE, 0,   1,   1,   1,  NULL, NULL, NULL, "atmosphere" },
  { "clouds",            "Clouds",              "Atmo",       KIND_TOGGLE, 0,   1,   1,   1,  NULL, NULL, NULL, "atmosphere" },
  { "cloud_shadows",     "Cloud shadows",       "Atmo",       KIND_TOGGLE, 0,   1,   1,   0,  NULL, NULL, NULL, "atmosphere" },
  { "height_fog",        "Height fog",          "Atmo",       KIND_TOGGLE, 0,   1,   1,   1,  NULL, NULL, NULL, "atmosphere" },
  { "ambient",           "Ambient particles",   "Atmo",       KIND_TOGGLE, 0,   1,   1,   1,  NULL, NULL, NULL, "atmosphere" },
  { "water_tint",        "Water tint",          "Atmo",       KIND_TOGGLE, 0,   1,   1,   1,  NULL, NULL, NULL, "atmosphere" },
  { "glint",             "Water glint",         "Atmo",       KIND_TOGGLE, 0,   1,   1,   1,  NULL, NULL, NULL, "atmosphere" },
  { "halos",             "Lamp halos",          "Atmo",       KIND_TOGGLE, 0,   1,   1,   1,  NULL, NULL, NULL, "atmosphere" },
  // Audit rows D1 + D4 — the two knobs on the (now derived) engine POINT space. Both default to
  // Auto, which defers to the active device profile (src/seam/DisplayProfile.cpp's kProfiles[]):
  // desktop gets 125% + Adaptive, touch gets 200% + Classic, i.e. exactly the 568x320 layout this
  // port shipped with. Leaving them on Auto is not the same as picking the profile's value by hand
  // — Auto re-resolves if the input mode changes, an explicit choice does not.
  // UX.5 (I8): desktop only (hidden on iOS, eden_settings_native_hidden). F11 is the keybind.
  { "fullscreen",        "Fullscreen",        "Video",     KIND_TOGGLE, 0,   1,   1,   0,  "Fill the screen. F11 toggles it.", NULL },
  { "display_layout",    "Layout aspect",     "Video",     KIND_ENUM,   0,   2,   1,   0,  NULL, "Auto,Classic 16:9,Adaptive" },

  // Experiments: opt-in, off by default (except fps_normalize, which keeps its pre-existing
  // default now that it has moved tabs — only its group changed, not its behavior).
  { "srclike_movement",   "Srclike movement",   "Experiments", KIND_TOGGLE, 0,   1,   1,   0,  "Strafing, bunnyhopping and surfing. Keyboard and mouse.", NULL },
  { "srclike_settings_open", "Srclike settings", "Experiments", KIND_PAGE, 0,   0,   0,   0,  "Auto-bhop, sensitivity and the movement sliders.", "srclike", "srclike_movement" },
  { "camera_view",       "Camera view",       "Video", KIND_ENUM,   0,   2,   1,   0,  "Third person shows your body. F5 cycles.", "First person,Third person,Third person (front)", NULL, "Experiments" },
  { "fps_normalize",     "Frame-rate normalize", "Experiments", KIND_TOGGLE, 0,   1,   1,   1,  "Same walk speed at any refresh rate.",     NULL },
  { "srclike_autobhop",   "Auto-bhop (hold jump)", "Experiments", KIND_TOGGLE, 0,   1,   1,   0,  "Holding jump hops again on landing. Off: time each hop.", NULL, NULL, "srclike" },
  { "srclike_sens",       "Srclike sensitivity", "Experiments", KIND_RANGE, 0,  10, .05f,   0,  "Counter-Strike value (degrees per count = value x 0.022). 0 = use Mouse sensitivity.", NULL, NULL, "srclike" },
  // SRC.8b: "Movement tuning" opens the Settings sub-page "movetune" (SX.1: a KIND_PAGE row; it was a hidden tab).
  { "fire_melts_ice",    "Fire melts ice",    "Experiments", KIND_TOGGLE, 0,   1,   1,   0,  "Burning ice turns to water.", NULL },
  { "fire_chars_vines",  "Fire chars vines",  "Experiments", KIND_TOGGLE, 0,   1,   1,   0,  "Burning vines turn to dark stone.", NULL },
  { "dev_console",       "Allow developer console", "Experiments", KIND_TOGGLE, 0,   1,   1,   0,  "Adds a console button next to the settings icon in singleplayer.", NULL },
  { "double_brush",      "Double-size build brush", "Experiments", KIND_TOGGLE, 0,   1,   1,   0,  "A key builds 2x2x2 blocks at a time.", NULL },
  // DN.2 (WORKING/day-night-cycle-plan-2026-10-31.md U1): the clock runs and is saved per world, but
    // MP.8: comma-separated matchmaker addresses (host[:port], default 27020); empty = no Matchmakers tab.
  { "online_sources_open", "Online sources", "Experiments", KIND_PAGE, 0,   0,   0,   0,  "Matchmakers and the mod catalogue.", "online" },
  { "mp_matchmakers",    "Matchmakers",       "Experiments", KIND_TEXT,   0,   0,   0,   0,  "Lists other servers. Off when empty.", NULL, NULL, "online" },
  // K.3-5/6 (WORKING/mod-manager-plan-2026-11-01.md): "Mods" opens the manager (main menu only; the row's
  // text is unused). The catalogue URL is the Get Mods tab's source (M7); empty = the tab is hidden.
  { "mods_catalogue_url","Get Mods catalogue", "Experiments", KIND_TEXT,  0,   0,   0,   0,  "Address of a mod catalogue (http or https). Empty hides Get Mods.", NULL, NULL, "online" },

  // SRC.8b: the move.* tunables as sliders. The values apply into SrclikeMove::tunables(). SX.6: these rows ARE
  // the move.* cvars' one range table: group "Movement tuning" maps to the console name move.<key less "move_">
  // (eden_settings_cvar), so Console.cpp bridges them like any row (its bridgeMove copy is gone). SX.1 places
  // them on the "srclike" page by `page`; Back and Reset are the page's titlebar. Defaults = SrclikeMove::kDefaults.
  { "move_accelerate",   "Ground acceleration", "Movement tuning", KIND_RANGE, 1,  20, .5f,   5,  "How fast you reach walking speed (sv_accelerate).", NULL, NULL, "srclike" },
  { "move_airaccelerate","Air acceleration",   "Movement tuning", KIND_RANGE, 1, 1000,  1, 100,  "Air steering. 100 is the surf/bhop value.", NULL, NULL, "srclike" },
  { "move_friction",     "Friction",           "Movement tuning", KIND_RANGE, 0,  20, .5f,   4,  "How fast you slow on the ground.", NULL, NULL, "srclike" },
  { "move_stopspeed",    "Stop speed",         "Movement tuning", KIND_RANGE, 0, 300,   5,  75,  "Below this speed, friction stops you at a fixed rate.", NULL, NULL, "srclike" },
  { "move_gravity",      "Gravity",            "Movement tuning", KIND_RANGE, 100, 2000, 10, 800,  "Units/s^2. 800 is stock.", NULL, NULL, "srclike" },
  { "move_maxvelocity",  "Max velocity",       "Movement tuning", KIND_RANGE, 500, 10000, 100, 3500, "Speed cap per axis.", NULL, NULL, "srclike" },
  { "move_tickrate",     "Tick rate",          "Movement tuning", KIND_RANGE, 30, 128,   1, 66.67f, "Movement ticks per second. 66.67 is CS:S.", NULL, NULL, "srclike" },

  // B.5b-1b (2026-10-12): the periodic save that replaced the walk reload's. Off leaves saving to
  // the ring's evict-saves (a modified column leaving the window -- never optional, or the edit is
  // lost), warp, exit, the Save button and app background. Read as g_autosave_seconds (FileManager.h).
  { "heading_world_saves", "Saves", "World", KIND_HEADING, 0, 0, 0, 0, NULL, NULL },
  { "autosave",          "Autosave",          "Saves",       KIND_ENUM,   0,   3,   1,   3,  "Off still saves on exit and warp.", "Off,1 min,2 min,5 min", NULL, "World" },
  { "save_backup",       "Keep a save backup", "Saves",      KIND_TOGGLE, 0,   1,   1,   0,  "Keep the previous save as a .bak.", NULL, NULL, "World" },
  // Stage S / S.5e (the user, 2026-10-10): every `.emod` is 256z by default. Read through
  // eden_get_upgrade_256z() by FileManager's convert-on-play, native Get Worlds and the web Storage
  // tab's import; the convert prompt's "Keep 64z" overrides it for one world.
  { "upgrade_256z",      "Convert to 256 high", "Saves", KIND_TOGGLE, 0, 1, 1, 1, "Upgrade 64z worlds to 256z when converting: room to build up to y 255. Uses more memory.", NULL, NULL, "World" },
};
static const int kSettingCount = (int)(sizeof(kSettings) / sizeof(kSettings[0]));

static float g_value[kSettingCount];
static std::string g_text[kSettingCount];   // KIND_TEXT rows only

// K.3-8: the shipped values of two KIND_TEXT rows (everything else starts empty). A player who clears one
// stores "" and keeps it off; Reset puts the default back.
static const char* textDefault(const char* key) {
    if (!std::strcmp(key, "mods_catalogue_url")) return "https://hagg3.github.io/emod-packs/index.json";
    if (!std::strcmp(key, "mp_matchmakers")) return "45.79.193.87";
    return "";
}
static bool  g_loaded = false;
// Row #14: did eden_prefs actually have a value for this (port-owned) row at load time? Used
// by eden_apply_input_profile() to tell "the player explicitly chose this" apart from "still
// sitting at the compiled default" — profile-driven defaults must only ever touch the latter,
// mirroring input_mode's own Auto/explicit arbitration (this file's header comment on Phase 2).
static bool  g_hadStored[kSettingCount];

static int eden_setting_index(const char* key) {
    for (int i = 0; i < kSettingCount; ++i)
        if (std::strcmp(kSettings[i].key, key) == 0) return i;
    return -1;
}

extern "C" int eden_settings_index_of(const char* key) { return eden_setting_index(key); }   // UX.5: WindowState_native.cpp

// Port-owned values the rest of the port reads. Declared in Input.cpp / consumed by the
// gluPerspective wrap below.
float eden_look_sensitivity = 1.0f;   // read by eden_apply_look_delta
float eden_srclike_sens = 0.0f;       // SRC.7 C6: CS sensitivity, 0 = off (read by eden_apply_look_delta)
float eden_fov_degrees      = 80.0f;  // read by Graphics::prepareScene
float eden_view_distance    = 0.0f;   // enum index (0=Auto, 1..6 = 4..9 chunks), read by Graphics::viewDistanceZFAR
float eden_render_distance  = 5.0f;   // B.5b-2: enum index into kRenderRadius[], read by eden_window_radius_for_world
// PC controls audit F1/F2 (Movement.cpp's --wrap of Player::setSpeed). Default ON; a toggle
// exists so stock (frame-time-dependent) feel can be A/B'd without a rebuild.
float eden_fps_normalize    = 1.0f;
// PC controls audit Phase 3/4/6 — plain mutable globals in the same style, read by
// Input.cpp/eden-st.html.
float eden_mouse_sensitivity_y = 1.0f;   // read by eden_apply_look_delta (Y axis)
float eden_hold_to_act      = 1.0f;      // read by eden-st.html's hold-to-act state machine
float eden_hotbar_shown     = 1.0f;      // UX.8-4: Settings > Gameplay > Hotbar (the strip; the keys work either way)
float eden_crosshair        = 1.0f;      // read by eden-st.html's crosshair overlay
// Audit item #6. Stored as the ENUM INDEX (same as every other KIND_ENUM row); the getters below
// translate to the numbers the page actually wants, so the index<->value table lives in exactly
// one place instead of being duplicated in JS.
float eden_render_scale     = 2.0f;      // index into {50%,75%,100%,125%}
float eden_fps_cap          = 0.0f;      // index into {Uncapped,30,45,60} ({Uncapped,30,60} on iOS, P.14); row #14
// Audit D1/D4. Both are enum indexes whose 0 is "Auto"; read (not written) by
// src/seam/DisplayProfile.cpp, which resolves Auto against the active profile every time it
// recomputes the point space. Never seeded by eden_apply_profile_defaults — see that function.
float eden_ui_scale         = 0.0f;      // index into {Auto,100%,125%,150%,200%}
extern "C" void eden_window_set_fullscreen(int on);   // port/src/seam/WindowState_native.cpp (UX.5)
extern "C" int  eden_host_can_quit(void);
float eden_display_layout   = 0.0f;      // index into {Auto,Classic 16:9,Adaptive}
// Row #24's three gamepad rows, mirrored into C for the same reason every other row above is:
// there is now a SECOND consumer. On web the translator is public/eden-gamepad.js and reads them
// through the settings bridge; on native it is port/src/seam/Input_native.cpp, which has no JS
// to read anything through. The rows themselves are unchanged, and their schema comment's "no
// C-side gamepad code" note is what these three lines retire.
float eden_gamepad_enabled  = 1.0f;
float eden_gamepad_look_sensitivity = 1.0f;
float eden_gamepad_deadzone = 0.15f;
// Inert since S.8 (nothing writes a .bak any more); the row and its pref stay so a stored value reads back.
float eden_save_backup      = 0.0f;
// S.5e: read once per conversion through eden_get_upgrade_256z(). Default ON (the user's decision).
float eden_upgrade_256z     = 1.0f;

// ---------------------------------------------------------------------------------------------
// Phase 2 — input mode: Auto(0) / Touch(1) / Keyboard+Mouse(2). "Auto" defers to whatever the
// page has detected so far (matchMedia at boot, then the first real touch/key/mouse event —
// see eden-st.html); a real touch or key event is unambiguous evidence, matchMedia's
// `pointer:coarse` is just the pre-interaction guess. `g_detectedTouch` is JS-owned state fed in
// through eden_set_detected_touch(); this file only arbitrates it against the user's explicit
// override (`input_mode` != Auto).
// ---------------------------------------------------------------------------------------------
void eden_audio_set_master(float v);
void eden_audio_set_bg_muted(int m);
static bool g_muteInBackground = false;
void eden_settings_focus_changed(int focused) {
    eden_audio_set_bg_muted((g_muteInBackground && !focused) ? 1 : 0);
}
static int  g_inputMode = 0;         // mirrors g_value[idx of "input_mode"], kept as an int too
static bool g_detectedTouch = false;

static void eden_apply_input_profile(void);

extern "C" {
int eden_effective_input_is_touch(void) {
    if (g_inputMode == 1) return 1;
    if (g_inputMode == 2) return 0;
    return g_detectedTouch ? 1 : 0;
}

// For the touch selftest (X.19): force the effective input to Touch for this run only. Selftests read
// the REAL prefs file, so a Mac whose Input mode is "Keyboard+Mouse" overrode eden_set_detected_touch
// and the stick checks ran on a desktop profile. Deliberately NOT eden_settings_set(): that persists.
void eden_test_force_touch_input(void) {
    const int i = eden_setting_index("input_mode");
    if (i >= 0) g_value[i] = 1.0f;
    g_inputMode = 1;
    eden_apply_input_profile();
}

void eden_set_detected_touch(int isTouch) {
    g_detectedTouch = (isTouch != 0);
    eden_apply_input_profile();
}


} // extern "C"

// Row #14 (perf-audit §4c.3), generalised into audit row D4's first-class PROFILE concept: "one
// build, two profiles" — profile-driven DEFAULTS, never profile-driven code paths. The two profiles
// and every field they carry live in src/seam/DisplayProfile.cpp's kProfiles[]; this function is
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

#if defined(EDEN_PLATFORM_IOS)
// FT.16 (plan: WORKING/thermal-power-plan-2026-10-28.md §6): A8/A8X/A9-era devices draw the 3D pass
// at 75% (2.25x stock 2.1's 1024x768 pixels on an iPad; 100% is 4.0x). The plan's "match 2.1" seeded
// 50%; after Round 0 on the Air 2 (2026-10-28) the user found 50% too soft and chose 75%. By hw.machine: iPad5,x (Air 2, mini 4), iPad6,11/12 (iPad 5th gen),
// iPhone7,x (6/6 Plus), iPhone8,x (6s/6s Plus/SE). The A9X iPad Pros (iPad6,3-8) are not on the list.
#include <sys/sysctl.h>
static int eden_ios_air2_class(void) {
    static int cached = -1;
    if (cached >= 0) return cached;
    char m[64] = {0};
    size_t n = sizeof(m) - 1;
    cached = 0;
    if (sysctlbyname("hw.machine", m, &n, nullptr, 0) != 0) return cached;
    if (std::strncmp(m, "iPad5,", 6) == 0 || std::strcmp(m, "iPad6,11") == 0 ||
        std::strcmp(m, "iPad6,12") == 0 || std::strncmp(m, "iPhone7,", 8) == 0 ||
        std::strncmp(m, "iPhone8,", 8) == 0)
        cached = 1;
    std::fprintf(stderr, "[eden-settings] device %s: %s render scale default\n", m,
                 cached ? "Air 2-class, 75%" : "100%");
    return cached;
}
#endif
// The device's render_scale default, as an option index (0 = 50%, 1 = 75%, 2 = 100%).
static int eden_device_render_scale_default(int profileIndex) {
#if defined(EDEN_PLATFORM_IOS)
    if (eden_ios_air2_class()) return 1;
#endif
    return profileIndex;
}
static bool g_presetApplying = false;

// AT.1a (WORKING/atmosphere-plan-2026-11-05.md A7, P10, P11): the Atmosphere tier and its element switches.
// Air 2-class iOS defaults to Off, everything else to High. A tier change writes the nine Atmo-group rows from
// kAtmoTier[] (Low: precip, clouds, ambient, water tint; High: all); an element row changed by hand stays
// until the next tier change. --atmo= overrides the tier for one run (not stored), and the pinned harness
// modes force Off unless it is given.
// (the AT_* enum is in Classes/Weather.h: the engine reads elements by name)
static const char* const kAtmoKey[AT_COUNT] = { "precip", "splash", "clouds", "cloud_shadows", "height_fog",
                                                "ambient", "water_tint", "glint", "halos" };
static const unsigned char kAtmoTier[3][AT_COUNT] = {
    {0, 0, 0, 0, 0, 0, 0, 0, 0},   // Off
    {1, 0, 1, 0, 0, 1, 1, 0, 0},   // Low
    {1, 1, 1, 0, 1, 1, 1, 1, 1},   // High (cloud shadows opt-in)
};
static void eden_atmo_sync(void);
static int g_atmoOverride = -1;      // --atmo=: -1 none, else the tier
static int g_weatherOverride = -1;   // --weather=: -1 none, 0 off, 1 clear, 2 overcast, 3 rain, 4 snow, 5 storm
static int eden_atmo_device_default(void) {
#if defined(EDEN_PLATFORM_IOS)
    if (eden_ios_air2_class()) return 0;
#endif
    return 2;
}
static bool g_profileDefaultsApplied = false;
static void eden_seed_profile_default(const char* key, int optionIndex) {
    int i = eden_setting_index(key);
    if (i >= 0 && !g_hadStored[i]) eden_settings_set(i, (float)optionIndex);
}
static void eden_apply_profile_defaults(void) {
    if (g_profileDefaultsApplied || !g_loaded) return;
    g_profileDefaultsApplied = true;
    const EdenProfile* p = eden_profile_active();
    int fps = p->fps_cap, rs = p->render_scale;
    // ROADMAP Phase M / M5.2: the low-memory overlay replaces the video-preset defaults with the
    // leaner kProfiles[EDEN_PROFILE_LOWMEM] row (1x pixel ratio, 75% render scale, 45 fps). It is
    // set by the page before this runs; still only a DEFAULT, so g_hadStored[] (a row the player
    // touched) wins exactly as with the input profile.
    if (eden_low_memory()) {
        const EdenProfile* lm = eden_profile_get(EDEN_PROFILE_LOWMEM);
        rs  = lm->render_scale;
        if (lm->fps_cap) fps = lm->fps_cap;
    }
#if defined(EDEN_PLATFORM_IOS)
    if (fps >= 2) fps -= 1;   // profile indices are into {Uncapped,30,45,60}; iOS dropped 45 (P.14): 45 -> 30, 60 -> 60
    if (eden_ios_air2_class()) fps = 1;   // FT.15d: Air 2-class seeds 30 fps (-0.9 W, FT.15c); index 1 in {Uncapped,30,60}
#endif
    eden_seed_profile_default("fps_cap",      fps);
    eden_seed_profile_default("render_scale", eden_device_render_scale_default(rs));
    if (eden_atmo_device_default() != 2) {   // AT.1a A7: Air 2-class starts Off (High is the row default); not a preset change
        g_presetApplying = true;
        eden_seed_profile_default("atmosphere", eden_atmo_device_default());
        g_presetApplying = false;
    }
    // FT.13: crosshair ON with keyboard+mouse, OFF on touch; seeded once from the profile resolved at first run.
    eden_seed_profile_default("crosshair", eden_profile_touch_chrome() ? 0 : 1);
    // UX.8-4 (H4): the hotbar strip is on with keyboard + mouse, off on touch.
    eden_seed_profile_default("hotbar", eden_profile_touch_chrome() ? 0 : 1);
}

static void eden_apply_input_profile(void) {
    // The point space depends on the profile (ui_scale/display_layout resolve through it), so a
    // profile change is a metrics change. Safe before the World exists — DisplayProfile.cpp
    // only re-lays-out what already exists.
    eden_display_refresh();
    if (!World::getWorld || !World::getWorld->hud) return;
    // Control chrome (the on-screen joystick + jump/crouch buttons) is a profile field, not a
    // separate detection. `use_joystick` is the engine's own name for it.
    World::getWorld->hud->use_joystick = eden_profile_touch_chrome() ? true : false;
    eden_apply_profile_defaults();
}

extern "C" void eden_keybind_reset_all(void);   // Stage 5.2, defined further down
extern "C" void eden_set_block_preview(int on);
extern "C" void eden_set_fly_allowed(int on);
extern float FLY_ACCEL_MULT, FLY_MAX_SPEED, FLY_BRAKING, FLY_VERTICAL_MULT, NOCLIP_SPEED;
extern "C" void eden_set_crouch_enabled(int on);
extern "C" void eden_set_fire_melts_ice(int on);
extern "C" void eden_set_fire_chars_vines(int on);
extern bool CREATURE_WALK_VOICES;
extern bool DOUBLE_BRUSH;
extern "C" void eden_set_build_size(int size);
extern "C" void eden_set_camera_view(int view);

static SettingsMenu* eden_engine_settings(void) {
    if (!World::getWorld || !World::getWorld->menu) return NULL;
    return World::getWorld->menu->settings;
}


// B.8 presets: enum indexes of view_distance (0 = Auto, 1 = 4 chunks) and render_scale (0 = 50%,
// 1 = 75%, 2 = 100%). Index 0 (Custom) has no row. On iOS (FT.16) Low is 50% and Default is the
// device's default (eden_preset_render_scale), so an Air 2 at its seeded 50% still reads Default.
// B.5b-2 adds render_distance (an index into kRenderRadius[]): High is radius 16 on desktop native,
// and 10 on iOS (P.4 decision 6, after T.B16 measured R10 on the Air 2; R12 is opt-in only there).
struct GfxPreset { int view_distance, render_scale, render_distance, atmosphere; };   // AT.1a: atmosphere -1 = the device default
#if defined(EDEN_PLATFORM_IOS)
static const GfxPreset kPresets[] = { {0, 0, 5, 0}, {1, 0, 5, 0}, {0, 2, 5, -1}, {0, 2, 6, 2} };   // Custom, Low, Default, High
#else
static const GfxPreset kPresets[] = { {0, 0, 5, 0}, {1, 1, 5, 0}, {0, 2, 5, -1}, {0, 2, 8, 2} };
#endif
static const int kRenderRadius[] = { 4, 5, 6, 7, 8, 9, 10, 12, 16 };
static int eden_preset_render_scale(int p) {
    return p == 2 ? eden_device_render_scale_default(kPresets[2].render_scale) : kPresets[p].render_scale;
}
static int eden_preset_atmosphere(int p) {
    return kPresets[p].atmosphere < 0 ? eden_atmo_device_default() : kPresets[p].atmosphere;
}
static void eden_preset_sync(int changedKnob);
static void eden_persist_setting(int i);
extern "C" void eden_settings_set(int i, float v);

// ---------------------------------------------------------------------------------------------
// QA.19: what each row's value does, as data. One entry per value row (toggle, range, enum), keyed by
// the row's key and resolved to an index once (eden_settings_resolve_bindings); a row with no entry,
// an entry naming no row, or an entry with neither `bind` nor `apply` fails the check that
// --console-selftest runs (eden_settings_check_bindings). `bind` receives the value first, then
// `apply(i, v, commit)` runs: commit = a deliberate change (eden_settings_set), false = the load-time
// pass (init), which must not re-run one-shot actions (a preset writing its knobs). `stock` = the
// stock 2.1 prefs name a row persists under (a 0/1 int), so old prefs read back.
// ---------------------------------------------------------------------------------------------
typedef void (*SettingApply)(int i, float v, bool commit);
struct SettingBinding {
    const char*  key;
    float*       bind;
    SettingApply apply;
    const char*  stock;
};
static bool on(float v) { return v != 0.0f; }
static void apply_none(int, float, bool) {}   // read where used (eden_weather_enabled, eden_reduce_flashing, ...)
static void apply_preset_knob(int i, float, bool) { eden_preset_sync(i); }
static void apply_atmo_element(int, float, bool commit) { if (commit) eden_atmo_sync(); }
static void apply_move_tunable(int i, float v, bool) {   // SRC.8b
    SrclikeMove::Tunables& t = SrclikeMove::tunables();
    float* f[] = {&t.accelerate, &t.airaccelerate, &t.friction, &t.stopspeed, &t.gravity, &t.maxvelocity, &t.tickrate};
    static const char* const k[] = {"move_accelerate", "move_airaccelerate", "move_friction", "move_stopspeed",
                                    "move_gravity", "move_maxvelocity", "move_tickrate"};
    for (int j = 0; j < 7; ++j) if (std::strcmp(kSettings[i].key, k[j]) == 0) *f[j] = v;
}
static Player* settings_player(void) { return World::getWorld ? World::getWorld->player : nullptr; }
extern bool DEV_CONSOLE_ALLOWED;
extern bool touchControlsSoundEnabled;
extern bool nightAmbienceEnabled;
static const SettingBinding kBindings[] = {
  // The stock five (2.1's SettingsMenu rows), under their stock prefs names.
  { "health",    nullptr, [](int, float v, bool) { if (Player* p = settings_player()) p->health_option = on(v); }, "Health" },
  { "autojump",  nullptr, [](int, float v, bool) { if (Player* p = settings_player()) p->autojump_option = on(v); }, "Autojump" },
  { "creatures", nullptr, [](int, float v, bool) { CREATURES_ON = MpWorld::gateCreatures(on(v)); }, "Creatures" },   // MP.3a: never in a session
  { "sound",     nullptr, [](int, float v, bool) { if (Resources::getResources) Resources::getResources->playsound = on(v); }, "Sound effects" },
  // Music also has to start or stop now (stock did it inline in its touch handler); musicToggled() picks the
  // title tune or an in-game song by mode.
  { "music",     nullptr, [](int, float v, bool commit) {
        if (!Resources::getResources) return;
        Resources::getResources->playmusic = on(v);
        if (commit) Resources::getResources->musicToggled(on(v));
    }, "Music" },

  { "crouch",          nullptr, [](int, float v, bool) { eden_set_crouch_enabled(on(v)); } },
  { "hotbar",          &eden_hotbar_shown },
  { "fly",             nullptr, [](int, float v, bool) { eden_set_fly_allowed(on(v)); } },
  { "fly_accel",       &FLY_ACCEL_MULT },
  { "fly_max_speed",   &FLY_MAX_SPEED },
  { "fly_braking",     &FLY_BRAKING },
  { "fly_vertical",    &FLY_VERTICAL_MULT },
  { "noclip_speed",    &NOCLIP_SPEED },   // SX.4
  { "day_night_cycle", nullptr, [](int, float v, bool) { SkyCycle::setEnabled(on(v)); } },
  { "day_length",      nullptr, [](int, float v, bool) {
        static const int kMinutes[] = {10, 20, 40, 60};
        const int d = (int)lroundf(v);
        SkyCycle::setDayLengthMinutes(d >= 0 && d < 4 ? kMinutes[d] : 20);
    } },
  { "weather",         nullptr, apply_none },   // eden_weather_enabled()
  { "frequency",       nullptr, [](int, float v, bool) { Weather::setFrequency((int)lroundf(v)); } },   // SX.9
  { "storms",          nullptr, [](int, float v, bool) { Weather::setStorms((int)lroundf(v)); } },
  { "snowline",        nullptr, [](int, float v, bool) { Weather::setSnowline(v); } },     // SX.6 (was session-only)
  { "mistheight",      nullptr, [](int, float v, bool) { Weather::setMistHeight(v); } },

  { "master_volume",   nullptr, [](int, float v, bool) { eden_audio_set_master(v); } },
  { "mute_in_background", nullptr, [](int, float v, bool) {
        g_muteInBackground = on(v);
        if (!g_muteInBackground) eden_audio_set_bg_muted(0);
    } },
  { "music_volume",    nullptr, [](int, float v, bool) {
        if (CocosDenshion::SimpleAudioEngine::sharedEngine()) CocosDenshion::SimpleAudioEngine::sharedEngine()->setBackgroundMusicVolume(v);
    } },
  { "ambience_volume", nullptr, [](int, float v, bool) {
        if (CocosDenshion::SimpleAudioEngine::sharedEngine()) CocosDenshion::SimpleAudioEngine::sharedEngine()->setAmbienceVolume(v);
    } },
  { "night_ambience",  nullptr, [](int, float v, bool) { nightAmbienceEnabled = on(v); } },
  { "effects_volume",  nullptr, [](int, float v, bool) {
        if (CocosDenshion::SimpleAudioEngine::sharedEngine()) CocosDenshion::SimpleAudioEngine::sharedEngine()->setEffectsVolume(v);
    } },
  { "creature_walk_voices", nullptr, [](int, float v, bool) { CREATURE_WALK_VOICES = on(v); } },
  { "touch_controls_sound", nullptr, [](int, float v, bool) { touchControlsSoundEnabled = on(v); } },

  { "input_mode",      nullptr, [](int, float v, bool) { g_inputMode = (int)lroundf(v); eden_apply_input_profile(); } },
  { "hold_to_act",     &eden_hold_to_act },
  { "mouse_sensitivity", &eden_look_sensitivity },
  { "mouse_sensitivity_y", &eden_mouse_sensitivity_y },
  { "invert_look",     nullptr, [](int, float v, bool) { if (Player* p = settings_player()) p->invertcam = on(v); } },
  { "touch_look_speed",     &TOUCH_LOOK_SENS_X },
  { "touch_look_speed_y",   &TOUCH_LOOK_SENS_Y },
  { "touch_look_smoothing", &TOUCH_LOOK_SMOOTH },
  { "touch_look_glide", nullptr, [](int, float v, bool) { TOUCH_LOOK_GLIDE = v > 0.0f ? v : 0.0f; } },
  { "gamepad",         &eden_gamepad_enabled },
  { "gamepad_look_sensitivity", &eden_gamepad_look_sensitivity },
  { "gamepad_deadzone", &eden_gamepad_deadzone },

  { "graphics_preset", nullptr, [](int, float v, bool commit) {
        // Only a deliberate change applies the knobs; the load-time pass must not overwrite stored knobs
        // (or the low-memory seeds) with the preset's.
        const int p = (int)lroundf(v);
        if (!commit || p < 1 || p >= (int)(sizeof(kPresets) / sizeof(kPresets[0]))) return;
        g_presetApplying = true;
        eden_settings_set(eden_setting_index("view_distance"), (float)kPresets[p].view_distance);
        eden_settings_set(eden_setting_index("render_radius"), (float)kPresets[p].render_distance);
        eden_settings_set(eden_setting_index("render_scale"), (float)eden_preset_render_scale(p));
        eden_settings_set(eden_setting_index("atmosphere"), (float)eden_preset_atmosphere(p));
        g_presetApplying = false;
    } },
  { "view_distance",   &eden_view_distance, [](int i, float, bool) {
        if (World::getWorld) Graphics::setZFAR(0);   // re-derive ZFAR now (it is otherwise only set on world load)
        eden_preset_sync(i);
    } },
  { "render_radius",   &eden_render_distance, apply_preset_knob },   // read at the next world entry, never applied to an open one
  { "render_scale",    &eden_render_scale, apply_preset_knob },
  { "shadows",         nullptr, [](int, float v, bool) { shadow_setSetting((int)lroundf(v)); } },   // re-meshes if the mode changed
  { "shadow_lamp_tint", nullptr, [](int, float v, bool) { shadow_setLampTint(v >= 0.5f); } },     // re-meshes if it changed
  { "atmosphere",      nullptr, [](int i, float v, bool commit) {
        // A deliberate change writes the element rows (the load-time pass must not overwrite hand-set ones).
        const int t = (int)lroundf(v);
        if (commit && t >= 0 && t <= 2) {
            for (int e = 0; e < AT_COUNT; ++e) {
                const int ei = eden_setting_index(kAtmoKey[e]);
                if (ei < 0) continue;
                g_value[ei] = (float)kAtmoTier[t][e];
                g_hadStored[ei] = true;
                eden_persist_setting(ei);
            }
        }
        eden_preset_sync(i);
    } },
  { "fps_cap",         &eden_fps_cap },
  { "original_lighting", nullptr, [](int, float v, bool) { R::setOriginalLighting(on(v)); } },
  { "fov",             &eden_fov_degrees },
  { "crosshair",       &eden_crosshair },
  { "block_preview",   nullptr, [](int, float v, bool) { eden_set_block_preview(on(v)); } },
  { "ui_scale",        &eden_ui_scale, [](int, float, bool) { eden_display_refresh(); } },   // re-derives the point space, re-lays-out
  { "reduce_flashing", nullptr, apply_none },   // eden_reduce_flashing()
  { "precip",          nullptr, apply_atmo_element },
  { "splash",          nullptr, apply_atmo_element },
  { "clouds",          nullptr, apply_atmo_element },
  { "cloud_shadows",   nullptr, apply_atmo_element },
  { "height_fog",      nullptr, apply_atmo_element },
  { "ambient",         nullptr, apply_atmo_element },
  { "water_tint",      nullptr, apply_atmo_element },
  { "glint",           nullptr, apply_atmo_element },
  { "halos",           nullptr, apply_atmo_element },
  { "fullscreen",      nullptr, [](int, float v, bool) { eden_window_set_fullscreen(on(v)); } },   // UX.5: held until the window exists
  { "display_layout",  &eden_display_layout, [](int, float, bool) { eden_display_refresh(); } },

  { "srclike_movement", nullptr, [](int, float v, bool) { SrclikeMove::setEnabled(on(v)); } },
  { "camera_view",     nullptr, [](int, float v, bool) { eden_set_camera_view((int)lroundf(v)); } },
  { "fps_normalize",   &eden_fps_normalize },
  { "srclike_autobhop", nullptr, [](int, float v, bool) { SrclikeMove::setAutoBhop(on(v)); } },
  { "srclike_sens",    &eden_srclike_sens },
  { "fire_melts_ice",  nullptr, [](int, float v, bool) { eden_set_fire_melts_ice(on(v)); } },
  { "fire_chars_vines", nullptr, [](int, float v, bool) { eden_set_fire_chars_vines(on(v)); } },
  { "dev_console",     nullptr, [](int, float v, bool) { DEV_CONSOLE_ALLOWED = on(v); } },
  { "double_brush",    nullptr, [](int, float v, bool) { DOUBLE_BRUSH = on(v); if (!DOUBLE_BRUSH) eden_set_build_size(1); } },
  { "move_accelerate",    nullptr, apply_move_tunable },
  { "move_airaccelerate", nullptr, apply_move_tunable },
  { "move_friction",      nullptr, apply_move_tunable },
  { "move_stopspeed",     nullptr, apply_move_tunable },
  { "move_gravity",       nullptr, apply_move_tunable },
  { "move_maxvelocity",   nullptr, apply_move_tunable },
  { "move_tickrate",      nullptr, apply_move_tunable },

  { "autosave",        nullptr, [](int, float v, bool) {
        static const float kPeriod[] = {0, 60, 120, 300};
        const int a = (int)lroundf(v);
        g_autosave_seconds = (a >= 0 && a < 4) ? kPeriod[a] : 60;
    } },
  { "save_backup",     &eden_save_backup },
  { "upgrade_256z",    &eden_upgrade_256z },
};
static const int kBindingCount = (int)(sizeof(kBindings) / sizeof(kBindings[0]));
static const SettingBinding* g_binding[kSettingCount];   // by row; filled by eden_settings_resolve_bindings
static bool g_bindingsResolved = false;
static bool settings_row_has_value(int i) { return kSettings[i].kind <= KIND_ENUM; }   // toggle, range, enum

// Fills g_binding[] and reports every mismatch into `why` (may be null). Returns the number of problems.
// `skip` (tests only: --console-mutate=unbound) leaves that key's entry out, as if it had none.
static int eden_settings_resolve_bindings(std::string* why, const char* skip = nullptr) {
    int bad = 0;
    for (int i = 0; i < kSettingCount; ++i) g_binding[i] = nullptr;
    for (int b = 0; b < kBindingCount; ++b) {
        const SettingBinding& e = kBindings[b];
        if (skip && !std::strcmp(e.key, skip)) continue;
        const int i = eden_setting_index(e.key);
        const char* fault = i < 0 ? "names no row" : !settings_row_has_value(i) ? "is on a row without a value"
                          : g_binding[i] ? "is bound twice" : (!e.bind && !e.apply) ? "has neither bind nor apply" : nullptr;
        if (fault) { ++bad; if (why) *why += std::string(e.key) + " " + fault + "; "; continue; }
        g_binding[i] = &e;
    }
    for (int i = 0; i < kSettingCount; ++i)
        if (settings_row_has_value(i) && !g_binding[i]) { ++bad; if (why) *why += std::string(kSettings[i].key) + " has no binding; "; }
    g_bindingsResolved = true;
    return bad;
}

static void eden_apply_setting(int i, bool commit) {
    if (i < 0 || i >= kSettingCount) return;
    if (!g_bindingsResolved) eden_settings_resolve_bindings(nullptr);
    const SettingBinding* b = g_binding[i];
    if (!b) return;
    const float v = g_value[i];
    if (b->bind) *b->bind = v;
    if (b->apply) b->apply(i, v, commit);
}

// The load-time pass: every row, no one-shot actions.
static void eden_apply_all_settings(void) {
    for (int i = 0; i < kSettingCount; ++i) eden_apply_setting(i, false);
}

// The stock prefs name a row persists under, or null (QA.19).
static const char* settings_stock_name(int i) {
    if (!g_bindingsResolved) eden_settings_resolve_bindings(nullptr);
    return g_binding[i] ? g_binding[i]->stock : nullptr;
}

// Every row persists here, in eden_prefs.
static void eden_persist_setting(int i) {
    if (kSettings[i].kind >= KIND_PAGE) return;   // SX.1: pages and headings hold no value
    const char* key = kSettings[i].key;
    if (const char* stock = settings_stock_name(i)) {   // a stock 2.1 row: its own name, a 0/1 int
        eden_prefs_set_int(stock, g_value[i] != 0.0f ? 1 : 0);
        eden_prefs_sync();
        return;
    }
    if (kSettings[i].kind == KIND_TEXT) {
        eden_prefs_set_str(key, g_text[i].c_str());
        eden_prefs_sync();
        return;
    }
    // Stored as an int in thousandths so one int type covers toggles and ranges alike.
    eden_prefs_set_int(key, (int)lroundf(g_value[i] * 1000.0f));
    eden_prefs_sync();
}

// A knob moved by hand (or by a profile seed): if it no longer matches the chosen preset, the
// preset becomes Custom. Not called while a preset is applying its own knobs.
// SX.5: the tier follows the element rows. Hand edits that match no tier read Custom (3); a Custom whose
// rows match a tier again snaps back to it. Rows not yet seeded (no stored value) are left alone.
static void eden_atmo_sync(void) {
    if (g_presetApplying) return;
    const int ai = eden_setting_index("atmosphere");
    if (ai < 0) return;
    int ei[AT_COUNT];
    for (int e = 0; e < AT_COUNT; ++e) { ei[e] = eden_setting_index(kAtmoKey[e]); if (ei[e] < 0) return; }
    int match = 3;
    for (int t = 0; t < 3 && match == 3; ++t) {
        bool m = true;
        for (int e = 0; e < AT_COUNT; ++e) m = m && ((g_value[ei[e]] != 0.0f) == (kAtmoTier[t][e] != 0));
        if (m) match = t;
    }
    const int cur = (int)lroundf(g_value[ai]);
    if (match == cur || (match == 3 && cur == 3)) return;
    if (cur != 3 && match != 3) return;   // tier row is authoritative until the elements differ
    if (cur == 3 || match == 3) {
        g_value[ai] = (float)match; g_hadStored[ai] = true; eden_persist_setting(ai);
        eden_preset_sync(ai);
    }
}

static void eden_preset_sync(int changedKnob) {
    (void)changedKnob;
    if (g_presetApplying) return;
    int pi = eden_setting_index("graphics_preset"), vi = eden_setting_index("view_distance"),
        ri = eden_setting_index("render_scale"), di = eden_setting_index("render_radius");
    if (pi < 0 || vi < 0 || ri < 0 || di < 0) return;
    int p = (int)lroundf(g_value[pi]);
    if (p < 1 || p >= (int)(sizeof(kPresets) / sizeof(kPresets[0]))) return;
    bool match = (int)lroundf(g_value[vi]) == kPresets[p].view_distance &&
                 (int)lroundf(g_value[di]) == kPresets[p].render_distance;
    match = match && (int)lroundf(g_value[ri]) == eden_preset_render_scale(p);
    // AT.1a: a player who never touched the tier keeps the preset they had (the row's default is not a choice)
    const int ai = eden_setting_index("atmosphere");
    if (ai >= 0 && g_hadStored[ai]) match = match && (int)lroundf(g_value[ai]) == eden_preset_atmosphere(p);
    if (!match) { g_value[pi] = 0.0f; g_hadStored[pi] = true; eden_persist_setting(pi); }
}

extern "C" {

// The stock five's values, straight from eden_prefs, applied without the rest of the model: called by
// SettingsMenu::load() at construction, before the title tune starts and frames before eden_settings_init
// can run (it needs World::getWorld->menu->settings). Stock 2.1 read and applied them there too.
void eden_settings_boot_stock(void) {
    for (int i = 0; i < kSettingCount; ++i) {
        const char* stock = settings_stock_name(i);
        if (!stock) continue;
        g_value[i] = (float)(eden_prefs_get_int(stock, kSettings[i].def != 0.0f ? 1 : 0) != 0);
        eden_apply_setting(i, false);
    }
}

// Called once the world exists. Seeds every row from eden_prefs, then applies them all.
void eden_settings_init(void) {
    if (g_loaded) return;
    if (!eden_engine_settings()) return;   // too early; caller retries next frame
    g_loaded = true;
    {   // QA.19: a row without a binding does nothing; say so (--console-selftest fails on it)
        std::string why;
        if (eden_settings_resolve_bindings(&why))
            std::fprintf(stderr, "[eden-settings] BINDINGS: %s\n", why.c_str());
    }

    for (int i = 0; i < kSettingCount; ++i) {
        g_value[i] = kSettings[i].def;
        const char* key = kSettings[i].key;
        if (kSettings[i].kind >= KIND_PAGE) continue;   // SX.1: no value
        if (const char* stock = settings_stock_name(i)) {   // stock 2.1 name and 0/1 int
            g_hadStored[i] = (eden_prefs_type(stock) == 1);
            if (g_hadStored[i]) g_value[i] = (float)(eden_prefs_get_int(stock, 0) != 0);
            continue;
        }
        if (kSettings[i].kind == KIND_TEXT) {
            const char* t = eden_prefs_get_str(key);
            g_hadStored[i] = (t != nullptr);
            g_text[i] = t ? t : textDefault(key);
            continue;
        }
        g_hadStored[i] = (eden_prefs_type(key) == 1);
        if (g_hadStored[i]) g_value[i] = eden_prefs_get_int(key, 0) / 1000.0f;
    }
    // Migration (Phase 2): "touch_joystick" was an independent toggle before input_mode existed
    // (pass 28-30). If a player had explicitly turned it ON and never saw an "input_mode" pref
    // (i.e. this is their first run since the upgrade), honor that as an explicit Touch choice
    // rather than silently reverting them to Auto-detect. Reads the OLD key directly since its
    // row no longer exists in kSettings[].
    {
        int input_mode_i = eden_setting_index("input_mode");
        if (input_mode_i >= 0 && !eden_prefs_has("input_mode")) {
            if (eden_prefs_get_int("touch_joystick", 0) > 0) g_value[input_mode_i] = 1.0f;
        }
    }
    {   // X.17: "render_distance" (9,10,12,16) became "render_radius" (4..16); old index i is new i+5
        int di = eden_setting_index("render_radius");
        if (di >= 0 && !g_hadStored[di] && eden_prefs_type("render_distance") == 1) {
            int v = eden_prefs_get_int("render_distance", 0) / 1000 + 5;
            if (v > (int)kSettings[di].max) v = (int)kSettings[di].max;
            g_value[di] = (float)v; g_hadStored[di] = true;
            eden_persist_setting(di);
        }
    }
#if defined(EDEN_PLATFORM_IOS)
    {   // P.14: fps_cap lost its 45 entry, so a stored index > 1 shifts down once (45 -> 30, 60 -> 60)
        int fi = eden_setting_index("fps_cap");
        if (fi >= 0 && !eden_prefs_has("fps_cap_v2")) {
            if (g_hadStored[fi]) {
                int v = (int)lroundf(g_value[fi]);
                if (v >= 2) { g_value[fi] = (float)(v - 1); eden_persist_setting(fi); }
            }
            eden_prefs_set_int("fps_cap_v2", 1000);
            eden_prefs_sync();
        }
    }
#endif
    {   // B.8: a preset nobody chose is whatever the knobs add up to (Default, unless a seed moved them)
        int pi = eden_setting_index("graphics_preset");
        if (pi >= 0 && !g_hadStored[pi]) {
            int vi = eden_setting_index("view_distance"), ri = eden_setting_index("render_scale"),
                di = eden_setting_index("render_radius");
            bool isDefault = (int)lroundf(g_value[vi]) == kPresets[2].view_distance
                          && (int)lroundf(g_value[di]) == kPresets[2].render_distance
                          && (int)lroundf(g_value[ri]) == eden_preset_render_scale(2);
            const int ai = eden_setting_index("atmosphere");
            if (ai >= 0 && g_hadStored[ai]) isDefault = isDefault && (int)lroundf(g_value[ai]) == eden_preset_atmosphere(2);
            g_value[pi] = isDefault ? 2.0f : 0.0f;
        }
    }
    eden_apply_all_settings();
}

// Has eden_settings_init() actually run yet? It needs the World to exist, so it happens some
// frames after module init — later than the page's own boot sizing pass. public/eden-st.html uses
// this as a one-shot edge to re-derive the drawable from the RESTORED render_scale/dpr_cap
// (item #6); without it a persisted non-default value only took effect on the first resize.
int eden_settings_loaded(void) { return g_loaded ? 1 : 0; }

// QA.19 (--console-selftest): every value row has a binding and every binding names one. Returns the number of
// problems and lists them in `why`. `skip` drops one key's entry first (the mutation leg); the table is
// resolved again without it afterwards, so a running model is unchanged.
int eden_settings_check_bindings(const char* skip, char* why, int whyLen) {
    std::string w;
    const int bad = eden_settings_resolve_bindings(&w, skip && *skip ? skip : nullptr);
    eden_settings_resolve_bindings(nullptr);
    if (why && whyLen > 0) std::snprintf(why, (size_t)whyLen, "%s", w.c_str());
    return bad;
}

// QA.19 (--settings-selftest): load the model again from whatever eden_prefs now holds (the test points it at
// a fixture file first with eden_prefs_reset_for_test), profile defaults included, as a fresh boot would.
void eden_settings_test_reinit(void) {
    g_loaded = false;
    g_profileDefaultsApplied = false;
    for (int i = 0; i < kSettingCount; ++i) g_hadStored[i] = false;
    eden_settings_init();
}

// The panel's whole data model, as JSON. One string rather than an accessor per field so the C
// table above stays the single source of truth and the JS never hard-codes a row.
const char* eden_settings_schema(void) {
    // SIZED WITH REAL HEADROOM ON PURPOSE. `snprintf` truncates safely, so overflowing this does
    // not corrupt memory — it emits JSON with no closing bracket, `JSON.parse` throws in
    // eden-settings.js, and the ENTIRE settings panel silently fails to render. Adding one row with
    // a long hint is enough to do it: at 6144 the table had 210 bytes of slack left when the D1/D4
    // rows landed. The `truncated` guard below turns that failure into a visible one.
    static char buf[16384];
    int n = 0;
    n += snprintf(buf + n, sizeof(buf) - n, "[");
    for (int i = 0; i < kSettingCount && n < (int)sizeof(buf) - 1; ++i) {
        const Setting& s = kSettings[i];
        n += snprintf(buf + n, sizeof(buf) - n,
            "%s{\"i\":%d,\"key\":\"%s\",\"label\":\"%s\",\"group\":\"%s\",\"kind\":%d,"
            "\"min\":%g,\"max\":%g,\"step\":%g,\"hint\":%s%s%s,\"options\":%s%s%s,\"parent\":%s%s%s}",
            i ? "," : "", i, s.key, s.label, s.group, s.kind, s.min, s.max, s.step,
            s.hint ? "\"" : "", s.hint ? s.hint : "null", s.hint ? "\"" : "",
            s.options ? "\"" : "", s.options ? s.options : "null", s.options ? "\"" : "",
            s.parent ? "\"" : "", s.parent ? s.parent : "null", s.parent ? "\"" : "");
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

float eden_settings_get(int i) {
    if (i < 0 || i >= kSettingCount) return 0.0f;
    return g_value[i];
}

// AT.1a: what the renderer and Classes/Weather read. A tier of Off gates every element (Off = absent, G1).
int eden_atmo_tier(void) {
    if (g_atmoOverride >= 0) return g_atmoOverride;
    const int ai = eden_setting_index("atmosphere");
    const int t = ai < 0 ? 0 : (int)lroundf(g_value[ai]);
    return t > 2 ? 2 : t;   // SX.5: Custom (3) gates like High; the element rows decide what is drawn
}
int eden_atmo_element(int e) {
    if (e < 0 || e >= AT_COUNT) return 0;
    const int t = eden_atmo_tier();
    if (t <= 0) return 0;
    if (g_atmoOverride >= 0) return kAtmoTier[t > 2 ? 2 : t][e];
    static int idx[AT_COUNT];
    static bool have = false;
    if (!have) { for (int k = 0; k < AT_COUNT; ++k) idx[k] = eden_setting_index(kAtmoKey[k]); have = true; }
    return idx[e] >= 0 && g_value[idx[e]] != 0.0f ? 1 : 0;
}
void eden_atmo_set_override(int tier) { g_atmoOverride = tier < 0 ? -1 : tier > 2 ? 2 : tier; }
int eden_atmo_override(void) { return g_atmoOverride; }
void eden_weather_set_override(int kind) { g_weatherOverride = kind < 0 ? -1 : kind > 5 ? 5 : kind; }
int eden_weather_override(void) { return g_weatherOverride; }
int eden_weather_enabled(void) {
    if (g_weatherOverride >= 0) return g_weatherOverride > 0 ? 1 : 0;
    const int wi = eden_setting_index("weather");
    return wi >= 0 && g_value[wi] != 0.0f ? 1 : 0;
}
int eden_reduce_flashing(void) {
    const int ri = eden_setting_index("reduce_flashing");
    return ri >= 0 && g_value[ri] != 0.0f ? 1 : 0;
}

void eden_settings_set(int i, float v) {
    if (i < 0 || i >= kSettingCount) return;
    const Setting& s = kSettings[i];
    if (s.kind == KIND_TEXT || s.kind >= KIND_PAGE) return;   // strings go through eden_settings_set_text; pages/headings hold nothing
    if (v < s.min) v = s.min;
    if (v > s.max) v = s.max;
    g_value[i] = v;
    g_hadStored[i] = true;   // an explicit change now counts the same as a persisted one (row #14)
    eden_apply_setting(i, true);
    eden_persist_setting(i);
}

// MP.8: a KIND_TEXT row's string. The setter stores whatever it is given (the caller validates:
// SettingsMenu's Matchmakers prompt) and persists it as an NSString under the row's key.
// K.3-6: the Get Mods catalogue address ("" = unset).
const char* eden_mods_catalogue_url(void) {
    for (int i = 0; i < kSettingCount; i++)
        if (!std::strcmp(kSettings[i].key, "mods_catalogue_url")) return g_text[i].c_str();
    return "";
}

// SRC.8b: set a range row by key (the console's move.* variables write through here so the slider, the
// store and the persisted value stay one thing). Returns 0 for an unknown key.
int eden_settings_set_by_key(const char* key, float v) {
    const int i = eden_setting_index(key);
    if (i < 0 || kSettings[i].kind != KIND_RANGE) return 0;
    eden_settings_set(i, v);
    return 1;
}

// SX.6: World > Weather tuning > Defaults (snowline, mist height).
void eden_weather_tuning_reset(void) {
    for (int i = 0; i < kSettingCount; ++i)
        if (kSettings[i].page && !std::strcmp(kSettings[i].page, "weathertune") && kSettings[i].kind < KIND_TEXT)
            eden_settings_set(i, kSettings[i].def);
}

// SRC.8b: Movement tuning > Reset to defaults.
void eden_move_tuning_reset(void) {
    for (int i = 0; i < kSettingCount; ++i)
        if (std::strncmp(kSettings[i].key, "move_", 5) == 0 && kSettings[i].kind == KIND_RANGE)
            eden_settings_set(i, kSettings[i].def);
}

const char* eden_settings_text(int i) {
    if (i < 0 || i >= kSettingCount || kSettings[i].kind != KIND_TEXT) return "";
    return g_text[i].c_str();
}

void eden_settings_set_text(int i, const char* text) {
    if (i < 0 || i >= kSettingCount || kSettings[i].kind != KIND_TEXT) return;
    g_text[i] = text ? text : "";
    g_hadStored[i] = true;
    eden_persist_setting(i);
}

// Audit row 20/G2 (unify settings + keybinds): the two persistence systems (this C table vs.
// eden-keybinds.js's own localStorage blob) stay split on purpose — a JS float can't hold a
// code->action map — but that split had left the SURFACE fragmented too: no single "reset
// everything" existed anywhere (only the Keys tab's own local reset), which is the fragmentation
// the audit's row actually complains about, not the storage split itself. This is the engine-side
// half of one shared reset; eden-settings.js's new "Reset" tab calls this AND
// window.EdenKeybinds.resetDefaults() together, then re-renders once.
void eden_settings_reset_all(void) {
    for (int i = 0; i < kSettingCount; ++i) {
        if (kSettings[i].kind == KIND_TEXT) { g_text[i] = textDefault(kSettings[i].key); eden_persist_setting(i); continue; }
        if (kSettings[i].kind >= KIND_PAGE) continue;
        eden_settings_set(i, kSettings[i].def);
    }
    eden_keybind_reset_all();   // Stage 5.2: keybinds moved into C, so "reset everything" means it
}

// Convenience for the port's own keyboard shortcuts (B, F): keeps the key and the panel in sync
// instead of each writing its own copy of the state. Index-based, NOT key-based, deliberately —
// passing a C string in from JS would need `_malloc`/`_free` added to the export list, and the JS
// side already holds the schema (which carries every key -> index mapping) for free.
float eden_settings_toggle(int i) {
    if (i < 0 || i >= kSettingCount) return 0.0f;
    eden_settings_set(i, g_value[i] != 0.0f ? 0.0f : 1.0f);
    return g_value[i];
}

// H.12: the cameraView key (F5). Cycles through the setting, not around it, so the live mode, the
// Settings row and the persisted value can never disagree (fly mode's key writes the global only).
void eden_cycle_camera_view(void) {
    const int i = eden_setting_index("camera_view");
    if (i < 0) return;
    eden_settings_set(i, (float)(((int)lroundf(g_value[i]) + 1) % 3));
}

// Is the ENGINE currently in its settings state? `Menu::showsettings` is the main menu's Options
// button (Classes/Menu.mm:534). Polled from JS so the panel opens for the engine's own button
// without the port having to hit-test that button itself.
int eden_settings_menu_open(void) {
    if (!World::getWorld || !World::getWorld->menu) return 0;
    return World::getWorld->menu->showsettings ? 1 : 0;
}

void eden_settings_menu_close(void) {
    if (!World::getWorld || !World::getWorld->menu) return;
    World::getWorld->menu->showsettings = false;
    // Menu::update returned early for every frame the panel was up, so any touch that was live
    // when it opened is still sitting in the slot table with a stale `inuse`. Clear the lot rather
    // than let the main menu act on a press that belonged to the panel.
    Input::getInput()->clearAll();
}

// U.3: the "menu" key (Escape) while the engine's settings screen is up. 1 = consumed: the screen
// closed (as its Back does; since U.4 Keys is a tab, not a child screen). 0 = settings is not showing, and the key
// keeps its usual meaning. Native only calls it (Input_native.cpp) — the key used to reach the screen
// as a synthetic tap on the HUD's menu button, which the screen claimed as its own touch and dropped.
int eden_settings_menu_back(void) {
    if (!eden_settings_menu_open()) return 0;
    SettingsMenu* sm = eden_engine_settings();
    if (sm) sm->back();
    else eden_settings_menu_close();
    return 1;
}

// Opens the engine's settings state from the port side (the in-game gear button). Only meaningful
// in the menu; in game the panel is shown by the JS on its own and this is not called.
void eden_settings_menu_open_now(void) {
    if (!World::getWorld || !World::getWorld->menu) return;
    World::getWorld->menu->showsettings = true;
}

// Plain getters for the port-owned globals eden-st.html polls every frame (crosshair visibility,
// hold-to-act repeat, fullscreen/fit mode, the Phase 6 opt-in). Cheaper and simpler for a
// polled-every-frame value than round-tripping through the schema's key->index lookup each time —
// that path is for the settings PANEL, which only needs it once per open.
int eden_get_hold_to_act(void) { return eden_hold_to_act != 0.0f; }
int eden_get_crosshair(void) { return eden_crosshair != 0.0f; }
int eden_get_hotbar_shown(void) { return eden_hotbar_shown != 0.0f; }
// S.5e: "Convert to 256 high". EDEN_UPGRADE_256Z=0|1 wins over the setting:
// a harness pins the height so a gate does not depend on whatever the save directory's settings say
// (port/src/harness/ sets it to 0 for every scripted mode unless --upgrade-256z is given).
int eden_get_upgrade_256z(void) {
    const char* e = getenv("EDEN_UPGRADE_256Z");
    if (e && e[0]) return e[0] != '0';
    return eden_upgrade_256z != 0.0f;
}
// B.5b-2: the window radius World::loadWorld hands eden_set_window_radius() for a world of this
// height, before Terrain::allocateMemory(). EDEN_WINDOW_RADIUS=N wins, the way EDEN_UPGRADE_256Z
// does (every native scripted mode pins it to 9 unless --radius= is given, so a gate's checksums do
// not depend on a stored setting). Then the setting, held to what the platform can afford: web
// keeps a 256z world to 12 (linear memory is capped at 512 MB and a 256z window at 16 projects past
// ~370 MB, plan §1.4), and a low-memory device to the stock 9.
int eden_window_radius_for_world(int height) {
    const char* e = getenv("EDEN_WINDOW_RADIUS");
    if (e && e[0]) return atoi(e);
    int i = (int)lroundf(eden_render_distance);
    const int n = (int)(sizeof(kRenderRadius) / sizeof(kRenderRadius[0]));
    int r = (i >= 0 && i < n) ? kRenderRadius[i] : T_RADIUS_DEFAULT;
    (void)height;
    if (eden_low_memory()) r = T_RADIUS_DEFAULT;
    return r;
}

// Audit item #6. Returned as integer percent / hundredths rather than floats so the page does no
// index->value mapping of its own (the tables below are the only copy). Out-of-range indexes fall
// back to the neutral entry, not to a clamp: a stale persisted index from a future build should
// look like "default", not like "50%".
int eden_get_render_scale_pct(void) {
    static const int kPct[] = {50, 75, 100, 125};
    int i = (int)lroundf(eden_render_scale);
#if defined(EDEN_PLATFORM_IOS)
    // N.4.12 read every value as 100% here (the HUD drew over a frozen menu at 50%); FT.16's quad
    // composite brought the setting back. iOS has no 125%: a stored 3 still reads as 100%.
    if (i == 3) return 100;
#endif
    if (i < 0 || i >= (int)(sizeof(kPct) / sizeof(kPct[0]))) return 100;
    return kPct[i];
}
// Row #14. Read every rAF tick by eden_main.cpp's frame gate — plain int, not a bool getter, since
// 0 means "uncapped" and any other value IS the target fps, no separate on/off flag needed.
int eden_get_fps_cap(void) {
#if defined(EDEN_PLATFORM_IOS)
    static const int kFps[] = {0, 30, 60};
#else
    static const int kFps[] = {0, 30, 45, 60};
#endif
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
// (The retired web build --wrap'd the GL panel to a no-op and did not use these.)


int eden_settings_count(void) { return kSettingCount; }

static const Setting* eden_row(int i) {
    return (i >= 0 && i < kSettingCount) ? &kSettings[i] : NULL;
}

const char* eden_settings_label(int i) { const Setting* s = eden_row(i); return s ? s->label : ""; }
const char* eden_settings_group(int i) { const Setting* s = eden_row(i); return s ? s->group : ""; }
// SX.1: where the GL screen draws the row ("" = its group's tab); KIND_PAGE's target is eden_settings_page_target.
const char* eden_settings_page(int i)  { const Setting* s = eden_row(i); return (s && s->page) ? s->page : ""; }
const char* eden_settings_page_target(int i) {
    const Setting* s = eden_row(i);
    return (s && s->kind == KIND_PAGE && s->options) ? s->options : "";
}
const char* eden_settings_hint(int i)  { const Setting* s = eden_row(i); return (s && s->hint) ? s->hint : ""; }
// Index of the toggle row `i` depends on (muted while it is off), or -1. A parent that is missing or
// not a KIND_TOGGLE is a table bug (muting under a range/enum has no meaning): reported, then ignored.
int eden_settings_parent(int i) {
    const Setting* s = eden_row(i);
    if (!s || !s->parent) return -1;
    const int p = eden_setting_index(s->parent);
    if (p < 0 || kSettings[p].kind != KIND_TOGGLE) {
        std::fprintf(stderr, "[eden-settings] row '%s': parent '%s' is not a toggle row\n", s->key, s->parent);
        return -1;
    }
    return p;
}
const char* eden_settings_key(int i)   { const Setting* s = eden_row(i); return s ? s->key   : ""; }
// SX.6: the console variable a row is (a value row: lower(group) + "." + key; group "Movement tuning" is
// "move." + the key less "move_") or shows (KIND_CVAR: its `options`). "" for text, page and heading rows.
// Console.cpp's bridge and --console-selftest gate 3 both name cvars through this, so the rule is one place.
const char* eden_settings_cvar(int i) {
    const Setting* s = eden_row(i);
    if (!s) return "";
    if (s->kind == KIND_CVAR) return s->options ? s->options : "";
    if (s->kind >= KIND_TEXT) return "";
    static std::string name;
    if (!std::strcmp(s->group, "Movement tuning") && !std::strncmp(s->key, "move_", 5)) {
        name = std::string("move.") + (s->key + 5);
        return name.c_str();
    }
    name.clear();
    for (const char* g = s->group; *g; ++g) name += (char)((*g >= 'A' && *g <= 'Z') ? *g - 'A' + 'a' : *g);
    name += ".";
    name += s->key;
    return name.c_str();
}
int eden_settings_scope(int i) { const Setting* s = eden_row(i); return s ? s->scope : SCOPE_ALWAYS; }
int   eden_settings_kind(int i) { const Setting* s = eden_row(i); return s ? s->kind : KIND_TOGGLE; }
float eden_settings_min(int i)  { const Setting* s = eden_row(i); return s ? s->min  : 0.0f; }
float eden_settings_max(int i)  { const Setting* s = eden_row(i); return s ? s->max  : 1.0f; }
float eden_settings_step(int i) { const Setting* s = eden_row(i); return s ? s->step : 1.0f; }
float eden_settings_def(int i)  { const Setting* s = eden_row(i); return s ? s->def  : 0.0f; }

// KIND_ENUM: `options` is a comma-separated label list. Count them, and copy label #j into a
// static scratch buffer (same no-malloc convention as eden_settings_schema()).
int eden_settings_enum_count(int i) {
    const Setting* s = eden_row(i);
    if (!s || !s->options || !s->options[0]) return 0;
    int n = 1;
    for (const char* p = s->options; *p; ++p) if (*p == ',') ++n;
    return n;
}

const char* eden_settings_enum_label(int i, int j) {
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

// C.2 (the console): WHEN a row's value takes effect. 0 = at once (every row but two), 1 = the next
// world open ("read at the next world entry"), 2 = the next 64z -> 256z conversion. By key, not a
// table field: the #if'd duplicate rows above would each need the extra positional initialiser.
int eden_settings_apply(int i) {
    const Setting* s = eden_row(i);
    if (!s) return 0;
    if (std::strcmp(s->key, "render_radius") == 0) return 1;
    if (std::strcmp(s->key, "upgrade_256z") == 0) return 2;
    return 0;
}

// Rows whose effect is not wired on native yet — the GL screen skips these rather than show a
// control that does nothing (layout is a point-space knob; input_mode is DOM detection).
// render_scale is shown since N.4.11: native renders the 3D pass at it (gl_context_native.cpp's
// eden_scene_pass_begin).
int eden_settings_native_hidden(int i) {
    const Setting* s = eden_row(i);
    if (!s) return 1;
    static const char* const kHidden[] = {
        "display_layout",
    };
    for (const char* h : kHidden) if (std::strcmp(s->key, h) == 0) return 1;
    if (std::strcmp(s->key, "fullscreen") == 0) return eden_host_can_quit() ? 0 : 1;   // UX.5: no window manager on iOS
    // Stage L: look rows follow the effective input device.
    const bool touch = eden_effective_input_is_touch() != 0;
    if (std::strcmp(s->key, "touch_controls_sound") == 0) return touch ? 0 : 1;   // SX.3
    if (std::strncmp(s->key, "touch_look_", 11) == 0) return touch ? 0 : 1;
    if (std::strncmp(s->key, "mouse_sensitivity", 17) == 0) return touch ? 1 : 0;
    return 0;
}


// =============================================================================================
// KEYBINDS — the C-side model (Phase N Stage 5.2)
// =============================================================================================
// WHAT THIS RETIRES. `web-port-final:web/public/eden-keybinds.js` owned DEFAULT_KEYMAP, ACTION_LABELS, the
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
// exactly, and an int pref (`eden_prefs_set_int`; `[NSNumber numberWithInt:]` before G.2b-1) persists
// it — with no string storage anywhere. kKeyNames[] below is that one shared table; it is the only
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
    KC_S = 22, KC_T = 23, KC_V = 25, KC_W = 26, KC_X = 27, KC_B = 5,
    KC_1 = 30, KC_2 = 31, KC_3 = 32, KC_4 = 33, KC_5 = 34,
    KC_6 = 35, KC_7 = 36, KC_8 = 37, KC_9 = 38,
    KC_ESCAPE = 41, KC_SPACE = 44, KC_GRAVE = 53, KC_SLASH = 56, KC_F1 = 58, KC_F2 = 59, KC_F5 = 62, KC_F11 = 68,
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
  { "flyToggle",   "Cycle fly mode",        "Actions",   KC_V,       0,         KB_MOMENTARY,  KB_BOTH },
  { "blockPicker", "Block picker",          "Actions",   KC_E,       0,         KB_MOMENTARY,  KB_BOTH },
  { "colorPicker", "Colour picker",         "Actions",   KC_C,       0,         KB_MOMENTARY,  KB_BOTH },
  { "buildSize",   "Build brush size",      "Actions",   KC_X,       0,         KB_MOMENTARY,  KB_NATIVE },

  { "menu",        "Menu / pause",          "Interface", KC_ESCAPE,  0,         KB_MOMENTARY,  KB_BOTH },
  { "console",     "Console",               "Interface", KC_GRAVE,   0,         KB_MOMENTARY,  KB_NATIVE },
  // MP.7: both act only in a multiplayer session (Menu.cpp eden_chat_open).
  { "chat",        "Chat",                  "Interface", KC_T,       0,         KB_MOMENTARY,  KB_NATIVE },
  { "chatCommand", "Chat command",          "Interface", KC_SLASH,   0,         KB_MOMENTARY,  KB_NATIVE },
  { "settings",    "Open settings",         "Interface", KC_O,       0,         KB_MOMENTARY,  KB_WEB  },
  { "blockPreview","Toggle block preview",  "Interface", KC_B,       0,         KB_MOMENTARY,  KB_BOTH },
  { "hideHud",     "Hide HUD",              "Interface", KC_F1,      0,         KB_MOMENTARY,  KB_NATIVE },
  { "screenshot",  "Screenshot",            "Interface", KC_F2,      0,         KB_MOMENTARY,  KB_NATIVE },
  { "cameraView",  "Camera view",           "Interface", KC_F5,      0,         KB_MOMENTARY,  KB_NATIVE },
  { "fullscreen",  "Fullscreen",            "Interface", KC_F11,     0,         KB_MOMENTARY,  KB_NATIVE },   // UX.5

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
static const char* eden_keybind_prefs_key(int i) {
    static char buf[64];
    snprintf(buf, sizeof(buf), "keybind.%s", kKeybinds[i].action);
    return buf;
}

static void eden_keybinds_load(void) {
    if (g_keybindsLoaded) return;
    g_keybindsLoaded = true;
    for (int i = 0; i < kKeybindCount; ++i) {
        g_keyCode[i] = kKeybinds[i].def;
        const char* key = eden_keybind_prefs_key(i);
        if (eden_prefs_type(key) != 1) continue;
        int v = eden_prefs_get_int(key, 0);
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

int eden_keybind_count(void) { return kKeybindCount; }

const char* eden_keybind_action(int i) { const Keybind* k = eden_keybind_row(i); return k ? k->action : ""; }
const char* eden_keybind_label(int i)  { const Keybind* k = eden_keybind_row(i); return k ? k->label  : ""; }
const char* eden_keybind_group(int i)  { const Keybind* k = eden_keybind_row(i); return k ? k->group  : ""; }
int eden_keybind_default(int i)   { const Keybind* k = eden_keybind_row(i); return k ? k->def : 0; }
int eden_keybind_secondary(int i) { const Keybind* k = eden_keybind_row(i); return k ? k->sec : 0; }
// Mirrors eden_settings_native_hidden(): 1 = the GL screen must not offer this row.
int eden_keybind_native_hidden(int i) {
    const Keybind* k = eden_keybind_row(i);
    return (!k || !(k->avail & KB_NATIVE)) ? 1 : 0;
}

const char* eden_keybind_code_name(int code) {
    const char* n = eden_keybind_code_name_internal(code);
    return n ? n : "";
}

const char* eden_keybind_code_label(int code) {
    for (int i = 0; i < kKeyNameCount; ++i)
        if (kKeyNames[i].code == code) return kKeyNames[i].label;
    return "";
}

int eden_keybind_get(int i) {
    eden_keybinds_load();
    return (i >= 0 && i < kKeybindCount) ? g_keyCode[i] : 0;
}

void eden_keybind_set(int i, int code) {
    if (i < 0 || i >= kKeybindCount) return;
    if (code != 0 && !eden_keybind_code_name_internal(code)) return;   // unknown key: refuse
    eden_keybinds_load();
    g_keyCode[i] = code;
    eden_prefs_set_int(eden_keybind_prefs_key(i), code);
    eden_prefs_sync();
}

void eden_keybind_reset_all(void) {
    eden_keybinds_load();
    for (int i = 0; i < kKeybindCount; ++i) eden_keybind_set(i, kKeybinds[i].def);
}

int eden_keybind_action_after(int prev, int code);


// Iterator form: pass the previous result (or -1 to start), get the next action bound to `code`,
// or -1 when there are no more. Exists because "one key, two actions" is shipped configuration
// and not an edge case, and because a rebind can create more of them at any time.
int eden_keybind_action_after(int prev, int code) {
    if (code == 0) return -1;
    eden_keybinds_load();
    for (int i = (prev < 0 ? 0 : prev + 1); i < kKeybindCount; ++i)
        if (g_keyCode[i] == code || (kKeybinds[i].sec != 0 && kKeybinds[i].sec == code)) return i;
    return -1;
}

int eden_keybind_index(const char* action) {
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

void eden_keybind_capture_begin(int i) {
    g_captureRow = (i >= 0 && i < kKeybindCount) ? i : -1;
}
void eden_keybind_capture_cancel(void) { g_captureRow = -1; }
int  eden_keybind_capture_active(void) { return g_captureRow; }

// The platform input layer hands EVERY key here first while a capture is armed. Returns nonzero
// if the key was consumed (so the caller must not also dispatch it as gameplay input).
//
// Escape CANCELS rather than binds, and that costs Escape the ability to be rebound onto from
// this screen — a deliberate trade, because Escape is the only key guaranteed to exist on every
// keyboard and a capture with no way out strands the player in a modal with no menu key.
int eden_keybind_capture_feed(int code) {
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
const char* eden_keybinds_schema(void) {
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
const char* eden_keybind_code_table(void) {
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

// Is the GL settings screen LIVE on this target? Always: the GL UI is the UI on every live target.
// (The retired web build --wrap'd the panel away and answered 0; GL pause menu still asks.)
int eden_gl_settings_available(void) {
    return 1;
}
}  // extern "C"

