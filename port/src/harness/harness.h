#pragma once
// harness.h — what every file of the --*-selftest harness shares (G.4: split out of the old eden_main_native.cpp).
// The prelude is the old file's include/extern block verbatim; the namespace below is the harness core
// (core.cpp). Each mode group lives in its own file under harness::g_<group> (nothing leaks between
// groups except through here), and main.cpp dispatches to the run_* declared at the bottom.
//                        a one-time migration of anything an earlier build left in ~/Documents.
//   --bundle=DIR         likewise ignored: real NSBundle answers with the executable's own
//                        directory, which is why CMake symlinks the flat asset set there.
//
// With no scripted mode it just plays: SDL event pump plus the frame loop.

#include "../seam/App.h"
#include "../seam/Input_native.h"
#include "../seam/SimpleAudioEngine_native.h"
#include "save_backup.h"                 // port/src/shim/foundation — the shared backup rules
#include "../../../Classes/FileManager.h"  // eden_save_backup_hook
#include "../../../Classes/EdenWorldStore.h" // --background-selftest's `.emod` leg
#include "../../../Classes/SimpleAudioEngine.h"
#include "../../../Classes/Resources.h"
#include "../../../Classes/Constants.h"    // CHUNK_SIZE / T_SIZE, for the map-border arithmetic below
#include "../../../Classes/TerrainGen2.h"  // GSIZE — same, and it is macros only
#include "../../../Classes/Menu.h"         // Menu::settings, for --shot's keybinds capture
#include "../../../Classes/WorldPropertiesMenu.h"   // H.15: --props-selftest, --ui-selftest, --shot
#include "../../../Classes/ConsoleMenu.h"           // C.3: --ui-selftest, --shot
#include "../../../Classes/SettingsMenu.h" // SettingsMenu::showKeybinds()
#include "../../../Classes/GLDialog.h"    // --ui-selftest drives GLDialog::prompt
#include "../../../Classes/WorldShare.h"  // --browser-selftest's Share legs (S.5 / S.5c)
#include "../../../Classes/EdenWorldExport.h"
#include "../../../Classes/WorldTrailer.h"   // --trailer-selftest (D.3a)
#include "../../../Classes/SignRenderer.h"   // --signs-selftest / --sign-shot (D.3b)
#include "../../../Classes/SignTool.h"       // --signs-selftest (D.3c)
#include "../../../Classes/CmdScript.h"      // --cmds-selftest (D.4c)
#include "../../../Classes/Console.h"        // --console-selftest (C.2)
#include "../seam/PlayerReadme_native.h"      // C.9: README.txt
#include "../../../Classes/Model.h"          // --cmds-selftest: CountActiveCreatures
#include "../../../Classes/PassTimers.h"     // --pass-probe / --pass-timers (P.1b)
#include "../../../Classes/Shadow.h"         // --shadows= / --shadow-selftest (H.2)
#include "../../../Classes/TerrainGenerator.h"   // --classic-selftest (H.4)
#include "../../../Classes/MpWorld.h"            // --mp-replay (MP.3a)
#include "../../../Classes/TouchLook.h"          // --look-selftest (Stage L)
extern bool  NOCLIP_MUTATE_MOVE;                    // Classes/Player.mm (--fly-mutate=move, FM.4)
extern float NOCLIP_SPEED;                          // Classes/Player.mm (--fly-selftest, FM.2)
#include "../../../Classes/ChatFeed.h"           // MP.7: --mp-replay leg 8
#include "../../../Classes/ServerListMenu.h"     // MP.6: --ui-selftest, --shot, --mp-mock
#include "../../../Classes/ServerFormMenu.h"
#include "../../../port/src/net/MpSession.h"     // --mp-replay: the session's own line parser, fed from a file
extern int g_classic_cave_votes_cells, g_classic_cave_votes_air;   // FileManager.cpp (H.4), --classic-selftest
extern bool g_wp_test_skip_generator;   // FileManager.cpp (H.15), --props-mutate=nogen
extern Vector colorTable[256];               // --cmds-selftest: the night sky (palette 54)
extern int g_ring_columns_landed, g_bulk_reloads, g_ring_evict_saves, g_ring_dynamics_dropped, g_autosaves; // Terrain.mm, --evict-selftest
extern int g_light_test_mutation;            // Lighting.mm (B.5b-1c), --light-mutate
bool light_columnKnownFree(int cx, int cz);  // Lighting.mm (P.10), --ring-light-selftest's last leg
extern int g_offcx, g_offcz;                 // Terrain.mm: the toroidal offset (--light-selftest's edge step)
Vector8 lightAtToroidal(int tx, int tz, int y);   // Terrain.mm
extern int regionSkyColors[4][4];            // FileManager.cpp: the saved per-region sky (--light-lattice)
void paintSky(int color);                    // TerrainGen2.mm
extern bool* chunksToUpdate;                 // Terrain.mm: the mesher's dirty flags (--ring-light-selftest)
extern bool* columnsToUpdate;
extern block8* blockarray;                   // Terrain.mm: --mesh-bench's raw window read (B.5b-2)
#include "../../../Classes/Terrain.h"
#include "../../../Classes/Player.h"
#include "../../../Classes/Camera.h"
#include "../../../Classes/Frustum.h"   // --camera-selftest (H.12): ViewTestAABB
#include "gl_context.h"   // the context, drawable and GL counters (port/src/render/gl/); no GL itself
#include "platform_shims.h"
#include "../eden_app_identity.h"   // Emod is not Eden — see that file

#include <SDL3/SDL.h>


#include <cstddef>
#include <vector>
#include <zlib.h>
bool photoEncodePNG(const unsigned char* rgb, int w, int h, std::vector<unsigned char>& out);   // Classes/Util.mm (H.16)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <algorithm>
#include <map>
#include <set>
#include <vector>
#include <cmath>
#include <functional>
#include <sys/stat.h>
#include <dirent.h>
#include <cerrno>
#include <cctype>
#include <unistd.h>
#include <zlib.h>                  // --browser-selftest builds its gzip/zip fixtures

// Shared seam exports (port/src/seam/*, compiled into this target too).
extern "C" void eden_click_begin(int);               // Input.cpp
extern "C" void eden_click_end(int);
extern "C" {
int   eden_get_fps_cap(void);
void  eden_settings_init(void);
void  eden_set_detected_touch(int isTouch);
int   eden_settings_loaded(void);
void  eden_heap_pressure_tick(void);
int   eden_native_thermal_state(void);   // App.cpp (P.1a)
void  eden_native_gl_set_headless(int on);
void  eden_native_gl_set_present(int on);
int   eden_native_gl_present_guard_fires(void);
int   eden_get_render_scale_pct(void);
int   eden_power_exp(int bit);   // power.cpp (FT.15a): is this --power-exp switch on
void  eden_native_gl_set_diag_clear(int on);   // --diag-clear (FT.16a): magenta window clear in play
SDL_Window* eden_native_gl_window(void);
#define EDEN_PEXP_MENU30     16

void  eden_ui_force_legacy(int on);
int   eden_get_fly_mode(void);

int   eden_menu_active(void);
int   eden_menu_world_count(void);
const char* eden_menu_world_name(int index);
const char* eden_menu_world_file(int index);
void  eden_menu_select(int index);
int   eden_menu_selected_index(void);
int   eden_menu_play(void);
char* eden_menu_name_buffer(void);
int   eden_menu_create_world(void);
void  eden_menu_set_pending_world_type(int flat);
void  eden_menu_set_pending_world_height(int height);
int   eden_menu_delete_at(int index);   // X.15: --shadow-selftest clears its scratch world
void  eden_menu_set_pending_generator(const unsigned char* rec28);   // H.4: NULL = none

// Phase N Stage 4.2 (port/src/seam/AppLifecycle.cpp, cmake-shared into this target).
int   eden_app_will_background(void);
int   eden_app_background_save_count(void);
int   eden_app_background_last_status(void);

void  eden_tap_hud_button_begin(int which);
void  eden_tap_hud_button_end(int which);
int   eden_hud_in_menu(void);
void  eden_console_menu_open(void);      // C.3 (Menu.cpp)
int   eden_chat_open(int slash);         // MP.7 (Menu.cpp)
int   eden_console_menu_toggle(void);
int   eden_console_menu_back(void);
int   eden_ui_captures_keys(void);
int   eden_ui_wants_cursor(void);

void  eden_settings_menu_open_now(void);
void  eden_settings_menu_close(void);

int   eden_gl_stat(int which);
int   eden_console_teleport(float x, float y, float z);
int   eden_console_setblock(int x, int z, int y, int type);

const char* eden_debug_menu_state(void);
const char* eden_debug_player_state(void);
const char* eden_debug_terrain_geometry(void);
const char* eden_debug_display_state(void);   // --touch-selftest reads `rmenu` out of this
void        eden_native_gl_get_letterbox(int* x, int* y, int* w, int* h);
struct SDL_Window;
SDL_Window* eden_native_gl_window(void);
const char* eden_debug_mesh_checksum(void);
const char* eden_debug_chunk_state(void);
const char* eden_debug_light_state(void);
const char* eden_debug_shadow_state(void);                    // H.2
const char* eden_debug_top_shade(int x, int z, int y);        // H.2
const char* eden_debug_chunk_mesh_hash(int x, int z, int y);  // H.2
int   eden_debug_light_edit(int op, int x, int z, int y, int color);
void  eden_set_save_inplace_threshold(unsigned long long);   // N.8
void  eden_debug_set_light_selfcheck(int on);
void  eden_debug_set_empty_selfcheck(int on);
long long eden_debug_chunk_empty(int x, int z, int y);
int   eden_debug_build_block(int x, int z, int y, int type);
void* eden_debug_world_trailer(void);                    // WorldTrailer* (D.3a)
const char* eden_debug_world_file(void);
int   eden_debug_modified_chunks(void);
const char* eden_debug_trailer_refusal_toast(int result);
int   eden_console_getblock(int x, int z, int y);
void  eden_debug_set_mesh_checksum(int on);
const char* eden_debug_world_format(void);
const char* eden_debug_heap(void);
void  eden_debug_heap_reset_peak(void);
void  eden_native_apply_display_mode(void);   // src/seam/DisplayMode_native.cpp
void  eden_native_set_safe_area_override(float l, float t, float r, float b);   // I.2, same file
void  eden_safe_rect(float* x, float* y, float* w, float* h);   // I.2, DisplayProfile.cpp
void  eden_display_debug_ignore_safe_area(int on);
int   eden_debug_safe_area_violations(float sx, float sy, float sw, float sh, char* buf, int cap);       // I.2, DebugState.cpp
void  eden_live_commands_init(const char* path);   // src/seam/LiveCommands_native.cpp (diagnostics)
void  eden_live_commands_tick(void);
}
void power_parse_exp(const char* list);   // power.cpp (FT.15a)
void power_log_set(double secs);
void power_phases_set(bool on);   // PW.0a
void power_note_pace(double ms);
void power_frame_end();
void power_frame();
int  power_game_mode();

extern float SCREEN_WIDTH;
extern float SCREEN_HEIGHT;

// Settings.cpp's mirrored gamepad rows; --gamepad-selftest reports the live deadzone.
extern float eden_gamepad_deadzone;

// Classes/Alert.h — the native seam (seam_link_stubs_native.cpp) implements this as a GLDialog.
void showAlertWarpHome();

extern "C" const char* eden_bench_save(void);   // DevConsole.cpp (X.1: --save-bench)

extern "C++" int eden_codec_bench(const char* path, int height, int want);   // CodecBench_native.cpp
extern "C++" int eden_world_tool_main(int argc, char** argv);                 // WorldTools_native.cpp
int fmh_selftestCompare(const char* edenPath, const char* emodPath, int* compared);   // Classes/FileManagerHelper.cpp (S.6)
extern "C" int eden_base_selftest_run(const char* workDir);   // port/src/seam/BaseSelftest_native.cpp (G.2b-0)
extern "C" int eden_net_selftest_run(const char* server);   // port/src/net/NetSelftest.cpp (MP.1)
extern "C" int eden_mp_selftest_run(const char* server);    // same file (MP.2)
extern "C" int eden_emod_selftest_run(const char* fixtureDir, const char* workDir, int flags,
                                      const char* file, const char* ref);       // Classes/EdenWorldStoreSelftest.cpp
extern "C" int eden_creature_selftest_run(const char* bundleRoot);   // Classes/CreatureModelSelftest.cpp (LG.7)


extern "C" int g_eden_mem_trace;           // HeapProbe_native.cpp, --mem-trace (N.4.9)
extern "C" void eden_mem_trace_install(void);   // HeapProbe_native.cpp, --mem-trace=2
extern "C" void eden_gl_set_tex_experiments(int bits);   // gl_textures.cpp, --tex-exp=
extern "C" void eden_gl_set_tex_flush(int on);           // gl_textures.cpp, N.4.9's fix
extern "C" void eden_mem_trace(const char* where);
extern "C" void eden_input_pointer_event(int phase, int identity, float x, float y);   // Input.cpp
extern "C" void eden_look_trace_enable(int on);     // Input.cpp, --look-trace (L.0)
extern "C" void eden_look_trace_frame(void);        // Input.cpp: closes a frame of the trace
extern "C" void eden_look_trace_last(int out[6], float* lostPct);   // --look-selftest
extern "C" void eden_test_force_touch_input(void);   // Settings.cpp (X.19)
extern "C" void eden_display_set_viewport(int css_w, int css_h);   // DisplayProfile.cpp (--look-selftest step 3)
extern "C" void eden_net_set_fixture_root(const char* dir);
extern "C" void eden_display_refresh(void);
extern "C" void eden_prefs_set_ephemeral(int on);    // Prefs_native.cpp (X.21)

extern "C" int eden_settings_count(void);
extern "C" const char* eden_settings_key(int i);
extern "C" void eden_settings_set(int i, float v);
extern "C" int eden_settings_native_hidden(int i);
extern "C" void eden_set_fly_mode(int);

extern "C" {
int  eden_keybind_count(void);
int  eden_keybind_index(const char* action);
int  eden_keybind_get(int i);
int  eden_keybind_default(int i);
void eden_keybind_set(int i, int code);
void eden_keybind_reset_all(void);
int  eden_keybind_action_after(int prev, int code);
int  eden_keybind_capture_active(void);
void eden_keybind_capture_begin(int i);
int  eden_keybind_capture_feed(int code);
const char* eden_keybind_code_name(int code);
}
extern "C" {   // port/src/seam/Settings.cpp -- C.2's gates read the tables the cvars were built from
int         eden_settings_count(void);
const char* eden_settings_key(int i);
const char* eden_settings_label(int i);
const char* eden_settings_group(int i);
int         eden_settings_kind(int i);
float       eden_settings_min(int i);
float       eden_settings_max(int i);
float       eden_settings_def(int i);
float       eden_settings_get(int i);
void        eden_settings_set(int i, float v);
int         eden_settings_enum_count(int i);
const char* eden_settings_enum_label(int i, int j);
int         eden_settings_native_hidden(int i);
int         eden_settings_apply(int i);
int         eden_keybind_count(void);
int         eden_keybind_index(const char* action);
int         eden_keybind_get(int i);
int         eden_keybind_default(int i);
int         eden_keybind_native_hidden(int i);
const char* eden_keybind_action(int i);
int         eden_keybind_action_after(int prev, int code);
void        eden_keybind_set(int i, int code);
}

namespace harness {

struct Options {
    bool headless = false;
    const char* mode = nullptr;          // "p1-gate" | "stage1" | "save-roundtrip" | nullptr
    int  height = 64;
    std::string world;
    float at[3] = {0.0f, 40.0f, 0.0f};
    bool haveAt = false;
    int  frames = 600;
    int  cycles = 8;                     // --leak-probe: load/quit cycles in one process
    int  winW = 0, winH = 0;             // --window=WxH: 0 means the built-in default (16:9)
    bool touchProfile = false;           // --touch-profile: pretend to be a touchscreen
    float safeL = 0, safeT = 0, safeR = 0, safeB = 0;   // --safe-area=L,T,R,B (window points); I.0 overlay
    std::string docs;
    std::string bundle;
    std::string shot;                    // --shot=PREFIX: file prefix for --shot's captures
    std::string netServer;               // --server=host:port: --net-selftest / --mp-selftest live leg
    std::string mpReplay, mpMutate;      // --mp-replay=FILE, --mp-mutate=noevictcommit|nolandhook (MP.3a)
    int mpOverlayMb = 0;                 // --mp-overlay-mb=N (0 = MpWorld's default, 48)
    std::string mpJoin, mpName, mpPassword;   // --mp-join=HOST[:PORT], --mp-name=, --mp-password= (MP.3b)
    int mpType = 0;                      // --mp-type=N (characterType)
    bool mpEdits = false;                // --mp-edits: MP.4's two-client leg, the builder
    std::string mpHold;                  // --mp-hold=FILE: ... the watcher, until FILE exists
    std::string codecFile;               // --codec-file=PATH: the raw .eden --codec-bench samples
    std::string emodFixtures, emodWork, emodFile, emodRef;   // --emod-selftest's inputs
    std::string bundledEden, bundledEmod;                    // --bundled-map-selftest's inputs
    bool emodQuick = false;
    std::string liveCmds;                // --live-cmds=FILE: see src/seam/LiveCommands_native.cpp
    bool trailerNoDirty = false;         // --trailer-no-dirty-mark: the mutation (live leg only)
    bool trailerKeep = false;            // --trailer-keep: stop after the reload (leave the edited world for emod.py --diff)
    std::vector<std::string> trailerFiles;   // --trailer-file=W.eden|W.emod (repeatable): --trailer-selftest's byte gate
    std::string evictMutate;             // --evict-mutate=nosave|nodrop: --evict-selftest's mutation legs
    std::string signsMutate;             // --signs-mutate=bottom|nohook: --signs-selftest's mutation legs
    std::string cmdsMutate;              // --cmds-mutate=nodepth: --cmds-selftest's mutation leg
    std::string consoleMutate;           // --console-mutate=anchor: --console-selftest's mutation leg (C.1)
    std::string consoleCorpus = "../port/tools/fixtures/cmds/corpus.txt";   // --console-corpus=
    std::string consoleRef = "../docs/console-reference.md";                // --console-reference=
    bool cmdsTrace = false;              // --cmds-trace: print every command-block statement
    float look[2] = {0.0f, 0.0f};        // --look=YAW,PITCH (degrees): --sign-shot's view
    bool haveLook = false;
    float walk[8] = {0};                 // --walk=DX,DZ[,...]: --stage1's scripted walk legs (B.5b-1a)
    int  walkLegs = 0;
    int  lightLattice = 0;               // --light-lattice=STEP: --stage1 prepares the ring-light gate's world (B.5b-1c)
    int  renderScalePct = 0;             // --render-scale=50|75|100|125: 0 leaves the setting alone
    std::vector<std::pair<std::string,float>> sets;   // --set=KEY=VALUE (repeatable): a settings write once loaded (P.2a)
    int  passFrames = 600;               // --pass-probe=N: timed frames per pose (P.1b)
    bool passTimers = false;             // --pass-timers: --stage1 --walk also prints `walk.passes`
    bool passFinish = false;             // --pass-finish: glFinish at every pass boundary (upper bound per pass)
    bool passVsync = false;              // --pass-vsync: keep vsync on (the player's frame; present then holds the vsync wait)
    std::string convertAnswer;           // --convert-answer=upgrade|keep|cancel: --convert-prompt-selftest
    std::string classicFixture = "../port/tools/fixtures/classic/legacy-1315365413.bin.gz";   // --classic-fixture=
    std::string flyMutate;               // --fly-mutate=move: --fly-selftest's mutation leg (FM.4)
    std::string lookMutate;              // --look-mutate=<name>: --look-selftest's mutation legs (Stage L)
    std::string classicMutate;           // --classic-mutate=tables|trunc|noborder: --classic-selftest's mutation legs
    std::string propsMutate;             // --props-mutate=nogen: --props-selftest's mutation leg (H.15)
    std::string propsWorld;              // --props-world=FILE: --props-selftest times reading this world (gate 4)
    std::string classicNew;              // --classic=SEED[,SCALE4]: a world open_world() CREATES is Classic (ring-walk gate)
    std::string stateTrace;              // --state-trace=FILE: --scene-shot's one-frame draw trace (G.5b-0)
    std::string litOracleDir;            // --lit-oracle=DIR: also write each scene's two images there (G.5b-4)
};
struct PlayerState { float x, y, z, yaw, pitch; int hudMode, blocktype; };

extern int g_meshBenchPasses;
extern std::string g_saveBenchList;
bool SDLCALL eden_lifecycle_event_watch(void*, SDL_Event* ev);
void eden_setenv(const char* k, const char* v);
extern Options g_opt;
extern eden_native::App* g_app;
extern bool g_tickInput;
void settings_pump();
void tick(int n);
int game_mode();
void report(const char* phase);
bool open_world(const char* displayName, int height);
void tap_hud(int which);
void ensure_hud_menu_open();
void save_world();
bool quit_to_menu();
void push_key(SDL_Scancode sc, bool down);
void push_mouse_motion(float xrel, float yrel);
void push_mouse_button(Uint8 button, bool down);
PlayerState player_state();
extern int g_selftestFailures;
extern const char* g_selftestTag;
extern bool g_ignoreUserInput;   // G.5b-0: --scene-shot drops input and focus events (core.cpp)
void check(bool ok, const char* what, const char* detail);
extern bool g_running;
void pump_events();
std::string default_bundle(const char* argv0);
std::string default_docs();
bool is_eden_save_name(const std::string& n);
void migrate_saves_from(const std::string& from, const std::string& to, int* moved);
bool is_uuid_name(const char* n);
void migrate_from_legacy_app_dir(const std::string& newDocs, int* moved);
void migrate_legacy_saves(const std::string& newDocs);
void migrate_legacy_saves(const std::string& newDocs);
void mkdirs(const std::string& path);
bool starts_with(const char* s, const char* p);

namespace g_boot { int run_p1_gate(); int run_stage1(); int run_smoke(); }
using g_boot::run_p1_gate;
using g_boot::run_stage1;
using g_boot::run_smoke;
namespace g_shots { bool capture(const char* label, void (*overlay)() = nullptr); int run_shot(); int run_scale_probe(); int run_preset_probe(); int run_pass_probe(); int run_fit_selftest(); }
using g_shots::run_shot;
using g_shots::run_scale_probe;
using g_shots::run_preset_probe;
using g_shots::run_pass_probe;
using g_shots::run_fit_selftest;
namespace g_world { int run_empty_bit_selftest(); int run_newblocks_selftest(); int run_photo_selftest(); int run_trailer_selftest(); int run_classic_selftest(); }
using g_world::run_empty_bit_selftest;
using g_world::run_newblocks_selftest;
using g_world::run_photo_selftest;
using g_world::run_trailer_selftest;
using g_world::run_classic_selftest;
namespace g_props { int run_camera_selftest(); int run_props_selftest(); int run_signs_selftest(); int run_sign_shot(); }
using g_props::run_camera_selftest;
using g_props::run_props_selftest;
using g_props::run_signs_selftest;
using g_props::run_sign_shot;
namespace g_cmds { int run_cmds_selftest(); int run_console_selftest(); int run_save_bench(); }
using g_cmds::run_cmds_selftest;
using g_cmds::run_console_selftest;
using g_cmds::run_save_bench;
namespace g_light { int run_light_selftest(); int run_ring_light_selftest(); int run_shadow_selftest(); int run_evict_selftest(); }
using g_light::run_light_selftest;
using g_light::run_ring_light_selftest;
using g_light::run_shadow_selftest;
using g_light::run_evict_selftest;
namespace g_mp { int mp_join_from_args(); int run_mp_replay(); int run_mp_join(); }
using g_mp::run_mp_replay;
using g_mp::mp_join_from_args;
using g_mp::run_mp_join;
namespace g_look { int run_look_selftest(); int run_touch_selftest(); }
using g_look::run_look_selftest;
using g_look::run_touch_selftest;
namespace g_display { int run_recolor_selftest(); int run_display_selftest(); int run_convert64_selftest(); int run_audio_selftest(); int run_leak_probe(); }
using g_display::run_recolor_selftest;
using g_display::run_display_selftest;
using g_display::run_convert64_selftest;
using g_display::run_audio_selftest;
using g_display::run_leak_probe;
namespace g_input { int run_fly_selftest(); int run_gamepad_selftest(); }
using g_input::run_fly_selftest;
using g_input::run_gamepad_selftest;
namespace g_ui { int run_menu_selftest(); int run_convert_prompt_selftest(); int run_picker_selftest(); int run_ui_selftest(); }
using g_ui::run_menu_selftest;
using g_ui::run_convert_prompt_selftest;
using g_ui::run_picker_selftest;
using g_ui::run_ui_selftest;
namespace g_browser { int run_browser_selftest_body(bool live); int run_browser_selftest(bool live); int run_keybind_selftest(); }
using g_browser::run_browser_selftest_body;
using g_browser::run_browser_selftest;
using g_browser::run_keybind_selftest;
namespace g_session { int run_input_selftest(); int run_background_selftest(); int run_save_roundtrip(); }
namespace g_render { int run_scene_shot(); int run_render_selftest(); int run_lit_oracle(); }
using g_render::run_scene_shot;
using g_render::run_render_selftest;
using g_render::run_lit_oracle;
using g_session::run_input_selftest;
using g_session::run_background_selftest;
using g_session::run_save_roundtrip;
struct PassAgg {
    std::vector<double> frame, ms[P_COUNT];
    std::vector<long> draws[P_COUNT], verts[P_COUNT], setup[P_COUNT];
    std::vector<long> fDraws, fVerts, fSetup, fElided;
    void add(double frameMs) {
        frame.push_back(frameMs);
        for (int i = 0; i < P_COUNT; ++i) {
            ms[i].push_back(g_pass.ms[i]);
            draws[i].push_back(g_pass.draws[i]); verts[i].push_back(g_pass.verts[i]); setup[i].push_back(g_pass.setup[i]);
        }
        // eden_gl_stat() is the frame before this one's (it rotates at drawFrame's boundary); over
        // hundreds of frames at a still pose that is the same distribution.
        fDraws.push_back(eden_gl_stat(0)); fVerts.push_back(eden_gl_stat(4));
        fSetup.push_back(eden_gl_stat(1)); fElided.push_back(eden_gl_stat(2));
    }
    template <typename T> static T q(std::vector<T> v, double f) {
        if (v.empty()) return T();
        std::sort(v.begin(), v.end());
        return v[std::min(v.size() - 1, (size_t)(f * (double)v.size()))];
    }
    static double mean(const std::vector<double>& v) {
        double t = 0; for (double x : v) t += x; return v.empty() ? 0 : t / (double)v.size();
    }
    void print(const char* tag, const char* label, int vsync, int thermal0, int batt0) const {
        std::string out;
        char b[256];
        std::snprintf(b, sizeof b, "{\"frames\":%d,\"vsync\":%d,\"finish\":%d,\"frame_p50_ms\":%.3f,\"frame_p90_ms\":%.3f,"
                      "\"frame_max_ms\":%.3f,\"thermal_start\":%d,\"thermal_end\":%d,\"battery_pct_start\":%d,"
                      "\"draws\":%ld,\"verts\":%ld,\"setup\":%ld,\"elided\":%ld,\"p\":{",
                      (int)frame.size(), vsync, g_pass.finish ? 1 : 0, q(frame, 0.5), q(frame, 0.9),
                      q(frame, 1.0), thermal0, eden_native_thermal_state(), batt0,
                      q(fDraws, 0.5), q(fVerts, 0.5), q(fSetup, 0.5), q(fElided, 0.5));
        out += b;
        for (int i = 0; i < P_COUNT; ++i) {
            std::snprintf(b, sizeof b, "%s\"%s\":{\"p50\":%.3f,\"p90\":%.3f,\"max\":%.3f,\"mean\":%.3f,\"draws\":%ld,\"verts\":%ld,\"setup\":%ld}",
                          i ? "," : "", kEdenPassNames[i], q(ms[i], 0.5), q(ms[i], 0.9), q(ms[i], 1.0), mean(ms[i]),
                          q(draws[i], 0.5), q(verts[i], 0.5), q(setup[i], 0.5));
            out += b;
        }
        std::printf("[%s] %s.passes %s}}\n", tag, label, out.c_str());
        std::fflush(stdout);
    }
};

template <typename Pred>
bool tick_until(Pred pred, int timeoutFrames, const char* what) {
    // A frame budget alone is racy against worker threads (the net seam, the manifest parse): a
    // fast machine burns 1500 frames in a few ms. So a timeout needs the frames AND 10 s of wall
    // clock (a real failure costs at most 10 s more) — Windows CI lost "selecting a world shows
    // its preview" to exactly this.
    const Uint64 t0 = SDL_GetTicks();
    for (int i = 0; i < timeoutFrames || SDL_GetTicks() - t0 < 10000; ++i) {
        if (pred()) return true;
        tick(1);
    }
    std::fprintf(stderr, "[eden-stage1] TIMEOUT waiting for %s (%d frames)\n", what, timeoutFrames);
    return false;
}

// ---- cross-group helpers (each lives in the named group's file) ----
namespace g_ui { void ui_pointer(int phase, float x, float yUp); void ui_tap(float x, float yUp); void ui_tap_rect(CGRect r);
                 void push_text(const char* utf8); void push_tap_key(SDL_Scancode sc); bool read_file(const std::string& path, std::vector<unsigned char>* out);
                 void ui_menu_to(bool home); }
using g_ui::ui_pointer; using g_ui::ui_tap; using g_ui::ui_tap_rect; using g_ui::push_text; using g_ui::push_tap_key; using g_ui::read_file; using g_ui::ui_menu_to;
namespace g_boot { int place_light_lattice(int S, const char* tag); }
using g_boot::place_light_lattice;
namespace g_input { void teleport_for_movement(); void rise_until_clear(int maxFrames); void check_clear_of_map_border(const char* what); }
using g_input::teleport_for_movement; using g_input::rise_until_clear; using g_input::check_clear_of_map_border;
namespace g_shots { int set_setting_by_key(const char* key, float v); bool set_render_scale_pct(int pct);
                   bool write_bmp(const char* path, int w, int h, const std::vector<unsigned char>& rgba); }
using g_shots::capture; using g_shots::set_setting_by_key; using g_shots::set_render_scale_pct;
namespace g_world { namespace classic_test {
struct Fixture { int seed = 0, height = 0; std::vector<std::pair<int, int> > keys; std::vector<uint8_t> types; };
bool load_fixture(const std::string& path, Fixture& fx, std::string& err);
bool write_world(const std::string& path, const char* name, int seed, int hx, int hz,
                 const Fixture* fx, const uint8_t* genRecord, const uint8_t* mirror, std::string& err);
int count_records(const std::string& path, int type);
bool play_file(const char* file);
int column_vs_pure(const ClassicGen& g, int cx, int cz, int height);
} }

}  // namespace harness
