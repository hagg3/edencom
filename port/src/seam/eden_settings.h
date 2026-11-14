// eden_settings.h — QA.19 (A4-03): the settings and keybind accessors the engine and the harness may call,
// declared once. Defined in port/src/seam/Settings.cpp (which includes this, so a signature drift is a
// compile error there) over its two tables, kSettings[] (+ kBindings[], what a row's value does) and
// kKeybinds[]. Before QA.19 each caller hand-wrote its own prototypes. Row and kind meanings:
// docs/ui.md "Settings"; port/CLAUDE.md "Settings and keybinds live in exactly one place".
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// ---- the model's lifetime
void        eden_settings_init(void);            // pumped each frame until it takes (needs World::getWorld->menu)
int         eden_settings_loaded(void);
void        eden_settings_boot_stock(void);      // SettingsMenu::load(): the stock five, before the model loads
int         eden_settings_check_bindings(const char* skip, char* why, int whyLen);   // --console-selftest
void        eden_settings_test_reinit(void);     // --settings-selftest: load again from eden_prefs

// ---- rows (index = position in kSettings[])
int         eden_settings_count(void);
int         eden_settings_index_of(const char* key);   // -1 if none
const char* eden_settings_key(int i);
const char* eden_settings_label(int i);
const char* eden_settings_group(int i);
const char* eden_settings_page(int i);           // SX.1: display placement, "" = the group's tab
const char* eden_settings_page_target(int i);    // SX.1: a KIND_PAGE row's page id
const char* eden_settings_hint(int i);
int         eden_settings_parent(int i);         // U.2: the toggle row this one depends on, or -1
int         eden_settings_kind(int i);           // 0 toggle, 1 range, 2 enum, 4 text, 5 page, 6 heading, 7 cvar
int         eden_settings_scope(int i);          // SX.6: 0 always, 1 world in play, 2 no world
float       eden_settings_min(int i);
float       eden_settings_max(int i);
float       eden_settings_step(int i);
float       eden_settings_def(int i);
int         eden_settings_enum_count(int i);
const char* eden_settings_enum_label(int i, int j);
int         eden_settings_native_hidden(int i);
int         eden_settings_apply(int i);
const char* eden_settings_cvar(int i);           // SX.6: the row's console name ("" = not a cvar)
const char* eden_settings_schema(void);

// ---- values
float       eden_settings_get(int i);
void        eden_settings_set(int i, float v);   // clamps, applies (the row's binding), persists
int         eden_settings_set_by_key(const char* key, float v);   // range rows only; 0 = unknown key
float       eden_settings_toggle(int i);
const char* eden_settings_text(int i);           // KIND_TEXT rows
void        eden_settings_set_text(int i, const char* text);
void        eden_settings_reset_all(void);       // every row and every keybind
void        eden_move_tuning_reset(void);        // SRC.8b
void        eden_weather_tuning_reset(void);     // SX.6
void        eden_cycle_camera_view(void);        // H.12: F5
const char* eden_mods_catalogue_url(void);       // K.3-6

// ---- values the renderer, weather and input read
int         eden_atmo_tier(void);
int         eden_atmo_element(int e);
void        eden_atmo_set_override(int tier);
int         eden_atmo_override(void);
void        eden_weather_set_override(int kind);
int         eden_weather_override(void);
int         eden_weather_enabled(void);
int         eden_reduce_flashing(void);
int         eden_effective_input_is_touch(void);
void        eden_set_detected_touch(int isTouch);
void        eden_test_force_touch_input(void);

// ---- the settings screen
int         eden_settings_menu_open(void);
void        eden_settings_menu_open_now(void);
void        eden_settings_menu_close(void);
int         eden_settings_menu_back(void);       // Escape; 1 = consumed

// ---- keybinds (index = position in kKeybinds[]; a code is a USB HID usage ID)
int         eden_keybind_count(void);
int         eden_keybind_index(const char* action);
const char* eden_keybind_action(int i);
const char* eden_keybind_label(int i);
const char* eden_keybind_group(int i);
int         eden_keybind_default(int i);
int         eden_keybind_secondary(int i);
int         eden_keybind_native_hidden(int i);
const char* eden_keybind_code_name(int code);
const char* eden_keybind_code_label(int code);
int         eden_keybind_get(int i);
void        eden_keybind_set(int i, int code);
void        eden_keybind_reset_all(void);
int         eden_keybind_action_after(int prev, int code);
void        eden_keybind_capture_begin(int i);
void        eden_keybind_capture_cancel(void);
int         eden_keybind_capture_active(void);
int         eden_keybind_capture_feed(int code);
const char* eden_keybinds_schema(void);
const char* eden_keybind_code_table(void);

#ifdef __cplusplus
}
#endif
