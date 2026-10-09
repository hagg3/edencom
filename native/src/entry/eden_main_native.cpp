// eden_main_native.cpp — the native `int main()` (Phase N Stage 1,
// WORKING/native-migration-plan-2026-09-04.md). Twin of web/src/entry/eden_main.cpp; that file
// registers an emscripten_set_main_loop callback and returns, this one owns a real while-loop.
//
// It also carries Stage 1's HARNESS, because Stage 1's deliverable is "two numbers and one diff,
// then stop" and there is no `node eden.js` here to script the module from the outside. The
// scripted modes below drive the same engine exports the web harnesses drive
// (eden_menu_create_world / eden_menu_play / eden_tap_hud_button_* / eden_debug_*), so the two
// sides are running the same session and their outputs are directly comparable — which is the
// whole point of the criteria.
//
//   --headless           no window; every GL call no-ops behind the shim's context guard, exactly
//                        as `node eden.js` does. Geometry, memory and save round-trips are all
//                        taken this way, so Stage 1 needs no display.
//   --p1-gate            criterion 1: boot, three ticks, print the same three `[eden-p1]` lines
//                        web/tools/headless-p1-gate.js asserts on, exit.
//   --stage1             criteria 2+3: menu phase, then create-or-load a world, teleport to a
//                        fixed origin, settle, and print geometry + mesh checksum + memory per
//                        phase as `[eden-stage1] <phase> {json}` lines.
//   --smoke              opens a REAL WINDOW, loads a world, runs --frames frames and reports the
//                        GL version, the shim's per-frame draw accounting and the geometry
//                        checksum, then exits. This is the plan's highest-risk item made
//                        checkable: macOS core profile refuses to draw without a bound VAO (the
//                        shim deliberately has no VAO concept), and the four ES-dialect shaders
//                        have to compile as desktop GLSL. Both fail as a black window and no
//                        error, so "did it draw" has to be a number.
//   --input-selftest     Stage 2: pushes synthetic SDL key/mouse events through the real
//                        translator (src/seam/Input_native.cpp) and asserts the ENGINE moved —
//                        the player's position, yaw and HUD mode, read back through the same
//                        eden_debug_* probes the web harnesses use. It runs headless, because the
//                        input path needs no GL; what it cannot cover is feel, which is a human's
//                        job. Exists because "input is wired up" is otherwise only checkable by
//                        someone sitting at the keyboard, and that is not a regression test.
//   --shot[=PREFIX]      opens a REAL WINDOW and writes BMP captures of three screens — the main
//                        menu, the in-world HUD and the in-game (ESC) panel — as
//                        PREFIX-menu.bmp / -settings.bmp (+ -settings-pN.bmp per page) /
//                        -keybinds.bmp / -hud.bmp / -pausemenu.bmp / -dialog.bmp /
//                        -menu-world.bmp / -rename.bmp (default prefix eden-shot), and since 5.9
//                        -browser.bmp / -browser-current.bmp (Get Worlds; live network unless
//                        --net-fixtures is given).
//                        The GL UI is native's only UI until Stage 5, so "what does it look like"
//                        needs an artefact; --smoke only answers "did it draw at all".
//   --keybind-selftest   Stage 5.2/5.3: the keybind MODEL and the rebind path. Asserts the
//                        defaults resolve, that rebinding a key really moves the behaviour from
//                        the old physical key to the new one (both directions checked, which is
//                        the half a "does the setter store it" test would miss), that a default
//                        double-binding still fires both of its actions, and that reset restores.
//                        Headless.
//   --ui-selftest        Stage 5.4 + N.4.5: the GL settings widgets (toggle tap, slider drag,
//                        enum stepper) through real pointer events at the rects the screen
//                        reports; GLW::TextField through pushed SDL text/key events (UTF-8
//                        backspace, the byte cap, Return, Escape); and the world rename, checked
//                        on disk — only WorldFileHeader::name may change. Headless.
//   --browser-selftest   ROADMAP 5.9: Get Worlds (Classes/WorldBrowser.mm) end to end, OFFLINE —
//                        it writes fixtures under <docs>/.net-fixtures and points the net seam at
//                        them, then drives the menu button, the source tabs, the archive filter,
//                        the servers' Featured/Recent/More/Search, five downloads (deflate zip,
//                        zip-in-zip, no-overwrite, two-member gzip, an HTML page that must be
//                        refused), the unreachable-server state, and plays a downloaded world.
//                        Headless; cleans up after itself.
//   --net-live-selftest  the same screen against the REAL archive and edengame.net servers: every
//                        source's lists and a preview, and the smallest archive world downloaded
//                        and loaded. Not a gate (it depends on three hosts being up).
//   --net-fixtures=DIR   answer every eden_net_fetch from DIR/<host>/<path>@<query> instead of the
//                        network (also the EDEN_NET_FIXTURES environment variable).
//   --objc-selftest      Stage 3: this port's own ObjC runtime (Linux/Windows only; macOS uses
//                        Apple's). Dispatch, ivar layout, super, categories, the empty-base ivar
//                        bias and the @"literal" layout.
//   --audio-selftest     Stage 2: decodes one real file of each shipped format and asserts a
//                        played effect reaches the mixer, drains, and that a music channel
//                        streams. Headless (no window needed) but it DOES open the audio device
//                        and make noise — that is the point.
//   --touch-selftest     Stage 4.3: pushes SDL finger events at the engine's OWN reported HUD rect
//                        and asserts the tap reaches Hud::inmenu. The only test of the touch path
//                        outside a browser.
//   --gamepad-selftest   Stage 2: attaches an SDL VIRTUAL gamepad, drives its sticks/buttons, and
//                        asserts the engine moved — so it needs no physical controller and runs
//                        headless. Native counterpart of web/tools/headless-gamepad-test.js.
//   --background-selftest Stage 4.2: pushes SDL_EVENT_WILL_ENTER_BACKGROUND and asserts the event
//                        WATCH (not the poll loop -- see eden_lifecycle_event_watch) reaches
//                        eden_app_will_background() and a real world file lands on disk.
//   --save-roundtrip     criterion 4: create a world, edit a block, save, quit, reload, and print
//                        the geometry checksum on both sides plus the saved file's path so the
//                        bytes can be diffed against a web-written save.
//   --leak-probe[=N]     Stage 3 verification: N (default 8) load->quit cycles of one world in one
//                        process, asserting the allocator's live-bytes figure is FLAT. The native
//                        counterpart of web/tools/headless-alloc-leak-probe.js and of
//                        headless-heap-ceiling-probe.js --torture=same64, which cannot run here
//                        because they drive `eden.js` through node. It matters on the non-Apple
//                        legs specifically: the shim Foundation runs outside Emscripten there for
//                        the first time, and this runtime frees objects without .cxx_destruct.
//   --height=64|256      world height for a newly created world (default 64).
//   --empty-shortcut=0   Stage R / R.2's A/B lever: 0 restores the pre-R.2 mesh scheduling (all-air
//                        chunks scanned and counted against the bulk-reload budget). Default 1.
//   --read-budget-bands=0  Stage R / R.2b's A/B lever: 0 restores the stock column-read budget
//                        (BULK_RELOAD_CHUNK_BUDGET/CHUNKS_PER_COLUMN columns a frame) instead of
//                        counting a landed column by its occupied bands. Default 1.
//   --empty-bit-selftest R.2: places a block in an all-air chunk and removes it again, asserting the
//                        empty bit follows both transitions and the block really meshes.
//   --light-selftest     R.3: lightbox place/repaint/break (incl. saturation, the y clip and the
//                        toroidal seam) through the engine's own edit paths, light hash per step.
//   --light-selfcheck    R.3: also keep the stock dense light array, written by the stock code,
//                        and report voxels where it and the brick store disagree (`.light`'s
//                        `mismatch`, must be 0). --light-selftest asserts it when present.
//   --empty-selfcheck    R.2's stale-bit check: rebuild2() scans the chunks the empty bit lets it
//                        skip and counts any that were not empty. Reported on `.chunkstate`.
//   --world=NAME         world display name to create or reuse (default depends on the mode).
//   --at=X,Y,Z           teleport target. FIXED BY DEFAULT AND THAT MATTERS: a new world spawns
//                        the player tens of columns away each run, and chunk contents differ by
//                        position, so a floating origin would make every checksum incomparable.
//   --frames=N           settle frames per phase (default 600 — ~10 s of engine time).
//   --docs=DIR           the save directory. WORKS AS OF STAGE 2 — through Stage 1 it was inert,
//                        because Classes/FileManager.mm asked real Foundation for
//                        NSSearchPathForDirectoriesInDomains(NSDocumentDirectory) and the
//                        eden_platform_documents_root() seam this flag drives was only read by the
//                        Foundation SHIM, which this target does not compile. That call site now
//                        asks the seam on both targets (byte-identical on web, where the shim's
//                        answer was already exactly this value). Default:
//                        ~/Library/Application Support/Emod — Stage 2's deliberate choice (and
//                        Emod, not Eden: see src/eden_app_identity.h), with
//                        a one-time migration of anything an earlier build left in ~/Documents.
//   --bundle=DIR         likewise ignored: real NSBundle answers with the executable's own
//                        directory, which is why CMake symlinks the flat asset set there.
//
// With no scripted mode it just plays: SDL event pump plus the frame loop.

#include "../seam/main_native.h"
#include "../seam/EdenAppDelegate_native.h"
#include "../seam/Input_native.h"
#include "../seam/SimpleAudioEngine_native.h"
#include "save_backup.h"                 // web/src/shim/foundation — the shared backup rules
#include "../../../Classes/FileManager.h"  // eden_save_backup_hook
#include "../../../Classes/SimpleAudioEngine.h"
#include "../../../Classes/Resources.h"
#include "../../../Classes/Constants.h"    // CHUNK_SIZE / T_SIZE, for the map-border arithmetic below
#include "../../../Classes/TerrainGen2.h"  // GSIZE — same, and it is macros only
#include "../../../Classes/Menu.h"         // Menu::settings, for --shot's keybinds capture
#include "../../../Classes/SettingsMenu.h" // SettingsMenu::showKeybinds()
#include "../../../Classes/GLDialog.h"    // --ui-selftest drives GLDialog::prompt
#include "gl_es1_shim.h"   // also gives glGetString (renamed to the shim's guarded form)
#include "platform_shims.h"
#include "../eden_app_identity.h"   // Emod is not Eden — see that file

#import <Foundation/Foundation.h>

#include <SDL3/SDL.h>

// PHASE N STAGE 4.1 — iOS HAS NO `int main()`, AND THIS ONE LINE IS THE WHOLE ENTRY POINT.
// <SDL3/SDL_main.h> defines `main` to `SDL_main` and supplies a real main() that calls
// SDL_RunApp -> UIApplicationMain, so the process gets a UIApplication, an app delegate and a
// run loop before a single line below executes — and SDL's delegate then calls this file's
// main() from -postFinishLaunch and exit()s with what it returns. That is why the harness's
// argv parsing and exit codes still work under `simctl launch`.
//
// INCLUDED ONLY ON iOS, deliberately. On macOS, Linux and Windows SDL_main.h is a no-op-ish
// convenience (SDL_MAIN_AVAILABLE, not NEEDED) — but it still renames main, and the three
// desktop legs have working entry points that this stage has no business perturbing. One
// platform's requirement should not become four platforms' indirection.
#if defined(EDEN_PLATFORM_IOS)
#include <SDL3/SDL_main.h>
#endif

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <algorithm>
#include <vector>
#include <cmath>
#include <sys/stat.h>
#include <dirent.h>
#include <cerrno>
#include <cctype>
#include <unistd.h>
#include <zlib.h>                  // --browser-selftest builds its gzip/zip fixtures

// Shared seam exports (web/src/seam/*, compiled into this target too).
extern "C" {
int   eden_get_fps_cap(void);
void  eden_settings_init(void);
void  eden_set_detected_touch(int isTouch);
int   eden_settings_loaded(void);
void  eden_heap_pressure_tick(void);
void  eden_native_gl_set_headless(int on);
void  eden_native_gl_set_present(int on);
int   eden_native_gl_present_guard_fires(void);
void  eden_gl_glReadPixels(int x, int y, int w, int h, unsigned fmt, unsigned type, void* px);
void  eden_gl_context_get_drawable_size(int* w, int* h);
SDL_Window* eden_native_gl_window(void);

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

// Phase N Stage 4.2 (web/src/seam/AppLifecycle_web.mm, cmake-shared into this target).
int   eden_app_will_background(void);
int   eden_app_background_save_count(void);
int   eden_app_background_last_status(void);

void  eden_tap_hud_button_begin(int which);
void  eden_tap_hud_button_end(int which);
int   eden_hud_in_menu(void);

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
int   eden_debug_light_edit(int op, int x, int z, int y, int color);
void  eden_debug_set_light_selfcheck(int on);
void  eden_debug_set_empty_selfcheck(int on);
long long eden_debug_chunk_empty(int x, int z, int y);
int   eden_console_getblock(int x, int z, int y);
const char* eden_bench_save(void);   // S.0(a) throwaway
void  eden_debug_set_mesh_checksum(int on);
const char* eden_debug_world_format(void);
const char* eden_debug_heap(void);
void  eden_debug_heap_reset_peak(void);
void  eden_native_apply_display_mode(void);   // src/seam/DisplayMode_native.cpp
void  eden_live_commands_init(const char* path);   // src/seam/LiveCommands_native.mm (diagnostics)
void  eden_live_commands_tick(void);
}

extern float SCREEN_WIDTH;
extern float SCREEN_HEIGHT;

// Settings_web.mm's mirrored gamepad rows; --gamepad-selftest reports the live deadzone.
extern float eden_gamepad_deadzone;

// Classes/Alert.h — the native seam (seam_link_stubs_native.mm) implements this as a GLDialog.
void showAlertWarpHome();

void eden_set_save_inplace_threshold(unsigned long long);   // S.0(a) throwaway
namespace {

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
    std::string docs;
    std::string bundle;
    std::string shot;                    // --shot=PREFIX: file prefix for --shot's captures
    std::string liveCmds;                // --live-cmds=FILE: see src/seam/LiveCommands_native.mm
    int  renderScalePct = 0;             // --render-scale=50|75|100|125: 0 leaves the setting alone
};

Options g_opt;
eden_native::EdenAppDelegate* g_app = nullptr;

// Phase N Stage 4.2 — see the SDL_AddEventWatch() call in main() for why this is a watch and not a
// case in the poll loop. A named SDLCALL function rather than a lambda because SDLCALL is a real
// calling convention on the Windows toolchains Stage 3 ships, and a converted lambda's is whatever
// the compiler's default happens to be.
static bool SDLCALL eden_lifecycle_event_watch(void*, SDL_Event* ev) {
    if (!g_app) return true;
    switch (ev->type) {
        case SDL_EVENT_WILL_ENTER_BACKGROUND:
        case SDL_EVENT_TERMINATING:
            g_app->onPageHide();          // save the open world, then stop the loop
            break;
        case SDL_EVENT_DID_ENTER_FOREGROUND:
            g_app->onVisibilityVisible(); // ...and start it again on the way back
            break;
        default:
            break;
    }
    return true;                          // never filter: the poll loop still sees everything
}

// ---------------------------------------------------------------------------------------------
// Frame driving
// ---------------------------------------------------------------------------------------------

// One frame, plus the SDL event pump when there is a window. The 2 ms sleep in scripted mode is
// not throttling for its own sake: EdenViewController::drawFrame derives etime from a real clock,
// and a loop that spins as fast as it can feeds the engine a ~0 s delta, which stalls everything
// time-driven (streaming, physics, the lighting slices) while burning CPU. 2 ms gives an etime in
// the same ballpark a real frame has.
void pump_events();

// Set by --input-selftest only. The other scripted modes must NOT run the input tick: it calls
// eden_ui_tick(), which drives the block-preview ghost — and a preview block in the window would
// change the very geometry checksum --stage1 exists to compare.
bool g_tickInput = false;

// The settings model, pumped once a frame until it takes. `eden_settings_init()` needs
// World::getWorld->menu->settings to exist and early-returns until it does, so the contract is
// "call it every frame and stop when eden_settings_loaded() answers yes" — exactly what
// public/eden-st.html's rAF loop does on web. It is NOT a one-shot call after eden_seam_main():
// the World is built by the app delegate but the menu's SettingsMenu is not reachable on the
// first frame, and a single early call is a silent no-op that leaves every port-owned setting at
// its file-scope initialiser instead of its declared default.
bool set_render_scale_pct(int pct);   // --render-scale, defined beside run_scale_probe()
extern "C" int g_eden_mem_trace;           // HeapProbe_native.cpp, --mem-trace (N.4.9)
extern "C" void eden_mem_trace_install(void);   // HeapProbe_native.cpp, --mem-trace=2
extern "C" void eden_gl_set_tex_experiments(int bits);   // gl_fixed_function.cpp, --tex-exp=
extern "C" void eden_gl_set_tex_flush(int on);           // gl_fixed_function.cpp, N.4.9's fix
extern "C" void eden_mem_trace(const char* where);
void settings_pump() {
    if (!eden_settings_loaded()) eden_settings_init();
    // --render-scale=N is applied the first frame the model has loaded (before then a write would
    // be overwritten by the stored value). It is an ordinary settings write, so it persists the
    // way a player's choice in the Settings screen would.
    static bool scaleApplied = false;
    if (!scaleApplied && g_opt.renderScalePct > 0 && eden_settings_loaded()) {
        scaleApplied = true;
        set_render_scale_pct(g_opt.renderScalePct);
    }
}

void tick(int n) {
    if (!g_app) return;
    for (int i = 0; i < n; ++i) {
        pump_events();
        settings_pump();
        if (g_tickInput) eden_native_input_tick();
        g_app->viewController.drawFrame(true);
        eden_native_audio_tick();
        eden_heap_pressure_tick();
        // The 1 ms yield is not frame pacing any more (setFixedEtime does that) — it is there so
        // the wall-clock-driven hold-to-act repeat in the input translator still gets a realistic
        // number of pulses per scripted second, and so a runaway loop is interruptible.
        if (g_opt.mode) SDL_Delay(1);
    }
}

// Ticks until `pred` is true or `timeoutFrames` frames have passed. Returns whether it happened.
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

int game_mode() {
    const char* s = eden_debug_menu_state();
    const char* k = std::strstr(s, "\"game_mode\":");
    return k ? atoi(k + 12) : -1;
}

// ---------------------------------------------------------------------------------------------
// Scripted phases
// ---------------------------------------------------------------------------------------------

void report(const char* phase) {
    std::printf("[eden-stage1] %s.mem %s\n", phase, eden_debug_heap());
    eden_mem_trace(phase);
    std::printf("[eden-stage1] %s.geometry %s\n", phase, eden_debug_terrain_geometry());
    std::printf("[eden-stage1] %s.checksum %s\n", phase, eden_debug_mesh_checksum());
    std::printf("[eden-stage1] %s.chunkstate %s\n", phase, eden_debug_chunk_state());
    std::printf("[eden-stage1] %s.light %s\n", phase, eden_debug_light_state());
    std::printf("[eden-stage1] %s.format %s\n", phase, eden_debug_world_format());
    std::fflush(stdout);
}

// Create the named world if the menu does not already list it, then play it. Mirrors what
// public/eden-menu.js does and what the headless probes do: set the pending world TYPE and HEIGHT
// first (the engine asks for both at Play time, not at create time — see Menu_web.mm), then
// create, select and play.
bool open_world(const char* displayName, int height) {
    int idx = -1;
    const int count = eden_menu_world_count();
    for (int i = 0; i < count; ++i) {
        if (std::strcmp(eden_menu_world_name(i), displayName) == 0) { idx = i; break; }
    }
    // "Normal" (not flat) so the world streams from the bundled Eden.eden — which is the whole
    // point for criterion 2: the default world is a fixed, shipped 2880x2880 map, so the same
    // origin yields the same blocks on every platform and every run. A flat world would compare
    // equal for the trivial reason that it is empty.
    eden_menu_set_pending_world_type(0);
    eden_menu_set_pending_world_height(height);
    if (idx < 0) {
        char* buf = eden_menu_name_buffer();
        std::snprintf(buf, 128, "%s", displayName);
        idx = eden_menu_create_world();
        if (idx < 0) { std::fprintf(stderr, "[eden-stage1] create_world failed\n"); return false; }
    }
    eden_menu_select(idx);
    if (eden_menu_play() != 1) { std::fprintf(stderr, "[eden-stage1] menu_play refused\n"); return false; }
    return tick_until([] { return game_mode() == 1; }, 60000, "game_mode == PLAY");
}

// The HUD taps the web harnesses use, with the same button indices (0 = open the in-game menu,
// 3 = save, 6 = exit to the main menu). Driven as real synthetic touches rather than by poking
// engine flags, because every `hud->` input flag is re-derived from the live touch set each frame
// — writing the flag directly is silently overwritten (web/CLAUDE.md's fast facts; it has bitten
// this port three times).
void tap_hud(int which) {
    eden_tap_hud_button_begin(which);
    tick(6);
    eden_tap_hud_button_end(which);
    tick(6);
}

void ensure_hud_menu_open() {
    if (eden_hud_in_menu() == 0) tap_hud(0);
}

void save_world() {
    ensure_hud_menu_open();
    tap_hud(3);
    tick(120);
}

bool quit_to_menu() {
    ensure_hud_menu_open();
    tap_hud(6);
    return tick_until([] { return game_mode() == 0; }, 20000, "game_mode back to MENU");
}

int run_p1_gate() {
    // Exactly what web/tools/headless-p1-gate.js asserts: the headless-no-context line, then three
    // "[eden-p1] tick N: World::update returned" lines. Both come from code shared with, or
    // line-for-line mirrored from, the web build — see EdenViewController_native.cpp.
    tick(3);
    return 0;
}

int run_stage1() {
    const char* name = g_opt.world.empty() ? "stage1" : g_opt.world.c_str();

    // Phase 1 — menu. Settle first: Resources::loadResources runs during World construction and
    // the menu's own art loads over the first frames, so a sample taken immediately reports a
    // process that has not finished starting.
    tick(g_opt.frames / 4);
    std::printf("[eden-stage1] menu.mem %s\n", eden_debug_heap());
    eden_mem_trace("menu");
    std::fflush(stdout);

    // Phase 2 — a world.
    if (!open_world(name, g_opt.height)) return 1;
    // Teleport to the FIXED origin before settling (see --at in the header comment for why a
    // fixed one is required for the checksum to mean anything).
    eden_console_teleport(g_opt.at[0], g_opt.at[1], g_opt.at[2]);
    tick(g_opt.frames);
    report(g_opt.height == 256 ? "world256" : "world64");

    // Phase 3 — back to the menu, so the "peak" figures cover a full load/unload cycle the way
    // headless-heap-ceiling-probe.js's do.
    if (!quit_to_menu()) return 1;
    tick(g_opt.frames / 4);
    std::printf("[eden-stage1] menu2.mem %s\n", eden_debug_heap());
    eden_mem_trace("menu2");
    std::fflush(stdout);
    return 0;
}

// The window pass. Everything Stage 1 measures is headless, so this exists purely to answer
// "does the desktop-GL backend actually draw?" — which the checksums cannot, because they are
// computed CPU-side before the vertices ever reach a buffer.
//
// The signal is eden_gl_stat(): 0 = draw calls in the last completed frame, 1 = setup calls
// issued, 2 = calls elided by the shim's dirty caches. A frame that drew nothing reports 0 draws
// while everything else looks healthy, which is exactly what a missing VAO or a shader that failed
// to compile produces on macOS core profile — no GL error, no exception, a black window.
int run_smoke() {
    const char* name = g_opt.world.empty() ? "stage1-smoke" : g_opt.world.c_str();
    std::printf("[eden-smoke] GL_VERSION  %s\n", (const char*)glGetString(GL_VERSION));
    std::printf("[eden-smoke] GL_RENDERER %s\n", (const char*)glGetString(GL_RENDERER));
    std::fflush(stdout);

    tick(120);
    std::printf("[eden-smoke] menu frame: draws=%d issued=%d elided=%d\n",
                eden_gl_stat(0), eden_gl_stat(1), eden_gl_stat(2));
    std::fflush(stdout);

    if (!open_world(name, g_opt.height)) return 1;
    eden_console_teleport(g_opt.at[0], g_opt.at[1], g_opt.at[2]);
    tick(g_opt.frames);

    const int draws = eden_gl_stat(0);
    std::printf("[eden-smoke] world frame: draws=%d issued=%d elided=%d\n",
                draws, eden_gl_stat(1), eden_gl_stat(2));

    // Render one more frame WITHOUT presenting, then read the back buffer. This is the difference
    // between "the shim issued 132 draw calls" and "132 draw calls put colour on the screen" —
    // and a shader-dialect mistake lands squarely between the two.
    {
        int w = 0, h = 0;
        eden_gl_context_get_drawable_size(&w, &h);
        const int side = 64;
        if (w >= side && h >= side) {
            eden_native_gl_set_present(0);
            tick(1);
            std::vector<unsigned char> px((size_t)side * side * 4);
            // 0x1908 = GL_RGBA, 0x1401 = GL_UNSIGNED_BYTE. Named numerically because this file
            // sees the shim's guarded GL surface, not the raw enums.
            eden_gl_glReadPixels((w - side) / 2, (h - side) / 2, side, side, 0x1908, 0x1401, px.data());
            eden_native_gl_set_present(1);
            long long sum = 0; int nonBlack = 0;
            for (size_t i = 0; i < px.size(); i += 4) {
                const int lum = px[i] + px[i + 1] + px[i + 2];
                sum += lum;
                if (lum > 12) nonBlack++;   // >4/255 per channel on average: not a clear-colour pixel
            }
            const int total = side * side;
            std::printf("[eden-smoke] centre %dx%d readback: %d/%d non-black, mean luma %.1f\n",
                        side, side, nonBlack, total, (double)sum / (total * 3.0));
            if (nonBlack * 4 < total) {
                std::fprintf(stderr, "[eden-smoke] FAIL: the frame drew, but the pixels are (nearly) "
                                     "all background — suspect the shaders or the texture path.\n");
                return 1;
            }
        }
    }
    std::printf("[eden-smoke] %s\n", eden_debug_mesh_checksum());
    std::fflush(stdout);
    // A world frame that issues no draw calls is the failure this mode exists to catch, so say so
    // in the exit code rather than leaving it to be read out of the log.
    if (draws <= 0) {
        std::fprintf(stderr, "[eden-smoke] FAIL: the world frame issued no draw calls.\n");
        return 1;
    }
    // The readback above cannot see a frame that was drawn but never PRESENTED (N.4.12: iOS at a
    // non-100% render scale), so the present guard's count is the check for that.
    if (eden_native_gl_present_guard_fires() > 0) {
        std::fprintf(stderr, "[eden-smoke] FAIL: %d present(s) found the wrong renderbuffer bound.\n",
                     eden_native_gl_present_guard_fires());
        return 1;
    }
    return 0;
}

// ---------------------------------------------------------------------------------------------
// --shot: frame captures of the real window
// ---------------------------------------------------------------------------------------------
// The GL UI is native's ONLY UI until Stage 5, so "does the interface look right" is now a
// first-class question — and the first play pass answered it with "broken looking and messy",
// which is not something a checksum or a draw-call count can localise. --smoke already proves the
// backend draws; this proves WHAT it draws, by writing the back buffer to a file a human (or the
// next session) can look at without sitting at the keyboard.
//
// BMP rather than PNG because it needs no encoder: this tree vendors stb_image (decode) but not
// stb_image_write, and 24-bit BMP is a 54-byte header plus bottom-up BGR rows — which is also
// exactly the order glReadPixels hands them back, so there is no flip. macOS Preview and
// `sips -s format png` both read it.
bool write_bmp(const char* path, int w, int h, const std::vector<unsigned char>& rgba) {
    FILE* f = std::fopen(path, "wb");
    if (!f) { std::fprintf(stderr, "[eden-shot] cannot open %s\n", path); return false; }
    const int rowBytes = ((w * 3) + 3) & ~3;          // BMP rows are 4-byte aligned
    const int imageBytes = rowBytes * h;
    unsigned char hdr[54] = {0};
    auto put32 = [&](int off, unsigned v) {
        hdr[off] = v & 0xff; hdr[off+1] = (v >> 8) & 0xff;
        hdr[off+2] = (v >> 16) & 0xff; hdr[off+3] = (v >> 24) & 0xff;
    };
    hdr[0] = 'B'; hdr[1] = 'M';
    put32(2, 54 + imageBytes); put32(10, 54);
    put32(14, 40); put32(18, (unsigned)w); put32(22, (unsigned)h);
    hdr[26] = 1; hdr[28] = 24;                         // 1 plane, 24 bpp, BI_RGB
    put32(34, (unsigned)imageBytes);
    std::fwrite(hdr, 1, sizeof(hdr), f);
    std::vector<unsigned char> row((size_t)rowBytes, 0);
    for (int y = 0; y < h; ++y) {                      // glReadPixels is already bottom-up
        const unsigned char* src = rgba.data() + (size_t)y * w * 4;
        for (int x = 0; x < w; ++x) {
            row[(size_t)x * 3 + 0] = src[(size_t)x * 4 + 2];   // B
            row[(size_t)x * 3 + 1] = src[(size_t)x * 4 + 1];   // G
            row[(size_t)x * 3 + 2] = src[(size_t)x * 4 + 0];   // R
        }
        std::fwrite(row.data(), 1, row.size(), f);
    }
    std::fclose(f);
    return true;
}

// Draws one more frame WITHOUT presenting it and reads the whole back buffer, the same trick
// run_smoke()'s centre readback uses — a presented frame's back buffer is undefined after
// SDL_GL_SwapWindow, so reading it after the fact is a race with the driver.
bool capture(const char* label) {
    int w = 0, h = 0;
    eden_gl_context_get_drawable_size(&w, &h);
    if (w <= 0 || h <= 0) { std::fprintf(stderr, "[eden-shot] no drawable\n"); return false; }
    eden_native_gl_set_present(0);
    tick(1);
    std::vector<unsigned char> px((size_t)w * h * 4);
    eden_gl_glReadPixels(0, 0, w, h, 0x1908, 0x1401, px.data());   // GL_RGBA, GL_UNSIGNED_BYTE
    eden_native_gl_set_present(1);
    char path[1024];
    std::snprintf(path, sizeof(path), "%s-%s.bmp",
                  g_opt.shot.empty() ? "eden-shot" : g_opt.shot.c_str(), label);
    if (!write_bmp(path, w, h, px)) return false;
    int ww = 0, wh = 0, wp = 0, hp = 0;
    if (eden_native_gl_window()) {
        SDL_GetWindowSize(eden_native_gl_window(), &ww, &wh);
        SDL_GetWindowSizeInPixels(eden_native_gl_window(), &wp, &hp);
    }
    std::printf("[eden-shot] %s -> %s (drawable %dx%d, window %dx%d pt / %dx%d px, "
                "point space %.0fx%.0f)\n",
                label, path, w, h, ww, wh, wp, hp, SCREEN_WIDTH, SCREEN_HEIGHT);
    std::printf("[eden-shot]   player %s\n", eden_debug_player_state());
    std::printf("[eden-shot]   geom   %s\n", eden_debug_terrain_geometry());
    std::fflush(stdout);
    return true;
}

void ui_tap(float x, float yUp);   // --ui-selftest's pointer tap, defined below

int run_shot() {
    const char* name = g_opt.world.empty() ? "shot" : g_opt.world.c_str();
    // No vsync: a capture needs drawn frames, not paced ones, and a vsync-blocked swap never
    // returns while the display is asleep or locked (2026-10-06: --shot sat at tick 0 for minutes
    // on an unattended Mac).
    SDL_GL_SetSwapInterval(0);
    tick(180);
    capture("menu");

    // Stage 5.9: Get Worlds, on the archive tab and on the current server's, each with its first
    // world selected and (when the host has one) its preview. Live network unless --net-fixtures
    // was given; a capture of the "couldn't reach" state is still the screen working.
    if (World::getWorld && World::getWorld->menu && World::getWorld->menu->browserOffered()) {
        Menu* m = World::getWorld->menu;
        WorldBrowser* b = m->browser;
        m->openBrowser();
        for (int src = 0; src < 2; ++src) {
            CGRect r = b->tabRect(src);
            ui_tap(r.origin.x + r.size.width * 0.5f, r.origin.y + r.size.height * 0.5f);
            tick_until([b] { return !b->listLoading(); }, 6000, "the browser's list");
            if (b->rowRect(0, &r)) ui_tap(r.origin.x + r.size.width * 0.5f, r.origin.y + r.size.height * 0.5f);
            tick_until([b] { return b->previewShown(); }, 1500, "a preview");
            tick(4);
            capture(src == 0 ? "browser" : "browser-current");
        }
        b->close();
        m->showbrowser = FALSE;
        tick(4);
    }

    // The rewritten generic GL settings screen (Phase N Stage 2.5). Meaningful only from the main
    // menu (in game the panel is a host overlay); shot here before any world loads.
    eden_settings_menu_open_now();
    tick(30);
    capture("settings");
    // Stage 5.4: every page, so a row whose control does not fit is visible without a human
    // paging through. -settings.bmp stays page 1 (its name predates paging).
    if (World::getWorld && World::getWorld->menu && World::getWorld->menu->settings) {
        SettingsMenu* sm = World::getWorld->menu->settings;
        for (int p = 1; p < sm->pageCount(); ++p) {
            sm->showPage(p);
            tick(4);
            char label[32];
            std::snprintf(label, sizeof(label), "settings-p%d", p + 1);
            capture(label);
        }
        sm->showPage(0);
    }

    // Stage 5.3's keybinds screen, reached the way the Keys button reaches it. Shown through
    // SettingsMenu::showKeybinds() rather than by hit-testing that button: where the button sits
    // is not what this artefact is for, and a layout change to the settings screen should not be
    // able to silently stop capturing the keybinds one.
    if (World::getWorld && World::getWorld->menu && World::getWorld->menu->settings) {
        World::getWorld->menu->settings->showKeybinds();
        tick(30);
        capture("keybinds");
    }

    eden_settings_menu_close();
    tick(15);

    if (!open_world(name, g_opt.height)) return 1;
    eden_console_teleport(g_opt.at[0], g_opt.at[1], g_opt.at[2]);
    tick(g_opt.frames);
    capture("hud");

    // The HUD's status line (statusbar.mm, the full-width box). It printed two and a bit copies
    // side by side until Stage 5.6 (Texture2D_web.mm's text-texture cap); this is the artefact.
    World::getWorld->hud->sb->setStatus(@"World Saved", 30);
    tick(4);
    capture("hud-status");
    World::getWorld->hud->sb->clear();

    // Stage 5.7: the block and colour pickers, opened the way E and C open them (a tap on the
    // HUD's rbuild / rpaint), and closed by the same tap.
    tap_hud(1);
    tick(20);
    capture("picker-blocks");
    tap_hud(1);
    tap_hud(2);
    tick(20);
    capture("picker-colors");
    tap_hud(2);
    tick(10);

    // The in-game (ESC) panel, through the same synthetic HUD tap Escape uses. This is the shot
    // that would have caught the suppressed-panel bug (eden_ui_force_legacy) at Stage 2's start
    // instead of at the first play pass.
    eden_tap_hud_button_begin(0);
    tick(6);
    eden_tap_hud_button_end(0);
    tick(30);
    std::printf("[eden-shot] in_menu=%d\n", eden_hud_in_menu());
    capture("pausemenu");

    // Stage 5.5: Settings opened from the pause menu — the GL SettingsMenu as an in-game modal.
    // Set directly, as the pause menu's Settings button does; Back is the screen's own path.
    World::getWorld->menu->settings->resetView();
    World::getWorld->menu->showsettings = TRUE;
    tick(10);
    capture("pausemenu-settings");
    World::getWorld->menu->showsettings = FALSE;
    tick(2);

    // The GL modal (Phase N Stage 2.5) — the ESC menu's Home button raises this. Shown directly
    // here rather than hunting the icon's rect; the point is the artefact.
    ::showAlertWarpHome();
    tick(20);
    capture("dialog");

    // N.4.5: the main menu with a world selected (its Rename button) and the rename prompt.
    GLDialog::dismiss();
    tick(2);
    if (quit_to_menu()) {
        tick(60);
        capture("menu-world");
        if (World::getWorld && World::getWorld->menu && World::getWorld->menu->renameOffered()) {
            World::getWorld->menu->beginRename();
            tick(10);
            capture("rename");
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------------------------
// --render-scale / --scale-probe  (ROADMAP N.4.11, the native render-resolution setting)
// ---------------------------------------------------------------------------------------------
// The setting is an ENUM row in Settings_web.mm (index into 50/75/100/125%), so a percentage is
// mapped to its option index here rather than written raw. Returns false for a percentage the row
// does not offer, which the caller reports instead of silently rendering at 100%.
extern "C" int eden_settings_count(void);
extern "C" const char* eden_settings_key(int i);
extern "C" void eden_settings_set(int i, float v);
extern "C" int eden_get_render_scale_pct(void);
bool set_render_scale_pct(int pct) {
    static const int kPct[] = {50, 75, 100, 125};
    int opt = -1;
    for (int j = 0; j < 4; ++j) if (kPct[j] == pct) opt = j;
    if (opt < 0) { std::fprintf(stderr, "[eden-scale] %d%% is not an option (50/75/100/125)\n", pct); return false; }
    for (int i = 0; i < eden_settings_count(); ++i) {
        if (std::strcmp(eden_settings_key(i), "render_scale") == 0) {
            eden_settings_set(i, (float)opt);
            return eden_get_render_scale_pct() == pct;
        }
    }
    return false;
}

// Opens a world, then for each scale: settles, takes a --shot-style capture, and times frames.
// THE TIMING IS CPU+GPU PER FRAME, NOT PRESENT-PACED: swap interval is set to 0 for the probe and
// every frame ends in a 1-pixel glReadPixels, which cannot return until the GPU has finished the
// frame. That is what makes the scales comparable on a device whose present would otherwise
// quantise everything to 16.7 ms steps. The number to read is the median ("p50"); "fps" is
// 1000/p50, i.e. what the frame would allow, not what vsync would show. The capture is the
// on-screen check that the HUD stays sharp while the world softens.
int run_scale_probe() {
    const char* name = g_opt.world.empty() ? "scale-probe" : g_opt.world.c_str();
    if (!open_world(name, g_opt.height)) return 1;
    if (g_opt.haveAt) eden_console_teleport(g_opt.at[0], g_opt.at[1], g_opt.at[2]);
    tick(g_opt.frames);                       // let the window fill before timing anything
    SDL_GL_SetSwapInterval(0);
#if defined(EDEN_PLATFORM_IOS)
    const int kScales[] = {100, 75, 50, 100};   // no 125% on iOS (Settings_web.mm, N.4.11b)
#else
    const int kScales[] = {100, 75, 50, 125, 100};
#endif
    const int kTimed = 240;
    for (int pct : kScales) {
        if (!set_render_scale_pct(pct)) return 1;
        tick(30);
        char label[32];
        std::snprintf(label, sizeof(label), "scale%d", pct);
        capture(label);
        std::vector<double> ms;
        ms.reserve(kTimed);
        unsigned char px[4];
        for (int i = 0; i < kTimed; ++i) {
            const Uint64 t0 = SDL_GetTicksNS();
            tick(1);
            eden_gl_glReadPixels(0, 0, 1, 1, 0x1908, 0x1401, px);   // GL_RGBA, GL_UNSIGNED_BYTE
            ms.push_back((double)(SDL_GetTicksNS() - t0) / 1e6);
        }
        std::sort(ms.begin(), ms.end());
        const double p50 = ms[ms.size() / 2], p90 = ms[ms.size() * 9 / 10];
        int dw = 0, dh = 0;
        eden_gl_context_get_drawable_size(&dw, &dh);
        std::printf("[eden-scale] %3d%%  scene %4dx%-4d  p50 %6.2f ms  p90 %6.2f ms  (%5.1f fps)  mem %s\n",
                    pct, dw * pct / 100, dh * pct / 100, p50, p90, 1000.0 / p50, eden_debug_heap());
    }
    SDL_GL_SetSwapInterval(1);
    return 0;
}

// Push a real SDL_Event through SDL_PushEvent so it comes back out of SDL_PollEvent and travels
// the SAME path a physical key does — rather than calling eden_native_input_handle_event()
// directly, which would test the translator while skipping the pump that feeds it.
void push_key(SDL_Scancode sc, bool down) {
    SDL_Event e;
    SDL_zero(e);
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.scancode = sc;
    e.key.repeat = false;
    SDL_PushEvent(&e);
}
void push_mouse_motion(float xrel, float yrel) {
    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_EVENT_MOUSE_MOTION;
    e.motion.xrel = xrel;
    e.motion.yrel = yrel;
    SDL_PushEvent(&e);
}
void push_mouse_button(Uint8 button, bool down) {
    SDL_Event e;
    SDL_zero(e);
    e.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
    e.button.button = button;
    SDL_PushEvent(&e);
}

struct PlayerState { float x, y, z, yaw, pitch; int hudMode, blocktype; };
PlayerState player_state() {
    PlayerState p{};
    const char* s = eden_debug_player_state();
    const char* k = std::strstr(s, "\"pos\":[");
    if (k) sscanf(k + 7, "%f,%f,%f", &p.x, &p.y, &p.z);
    if ((k = std::strstr(s, "\"yaw\":")))       p.yaw = (float)atof(k + 6);
    if ((k = std::strstr(s, "\"pitch\":")))     p.pitch = (float)atof(k + 8);
    if ((k = std::strstr(s, "\"hud_mode\":")))  p.hudMode = atoi(k + 11);
    if ((k = std::strstr(s, "\"blocktype\":"))) p.blocktype = atoi(k + 12);
    return p;
}

int g_selftestFailures = 0;
const char* g_selftestTag = "eden-input";   // set by whichever selftest is running
void check(bool ok, const char* what, const char* detail) {
    std::printf("[%s] %-46s %s%s%s\n", g_selftestTag, what, ok ? "PASS" : "FAIL",
                detail && *detail ? "  " : "", detail ? detail : "");
    if (!ok) g_selftestFailures++;
}

// ---------------------------------------------------------------------------------------------
// --touch-selftest  (Phase N Stage 4.3, the SDL touch translator)
// ---------------------------------------------------------------------------------------------
// Touch is the one input path this port has always had and has never been able to TEST outside a
// browser: the engine half is the original 2010 code, the translator half was `public/eden-input.js`
// only, and a desktop leg has no fingers. SDL_PushEvent does, so this drives the same four events
// UIKit delivers on a device and asserts the effect at the far end.
//
// **IT USES THE ENGINE'S OWN HUD RECT, not a guessed coordinate.** eden_debug_display_state()
// reports `rmenu` (the in-game menu corner icon) in the same point space the touch core consumes,
// so this cannot pass by hitting the wrong thing or fail because a layout constant moved — which
// is exactly the failure mode a hard-coded "tap at 0.9, 0.1" would have.
//
// TWO COORDINATE FACTS, both of which are one-line bugs if they are wrong:
//   * SDL reports finger positions NORMALISED to the window (0..1); the mouse path reports window
//     points. The translator multiplies by the point space directly for that reason.
//   * HUD rects are BOTTOM-LEFT origin and the pointer API is TOP-LEFT (see
//     eden_tap_hud_button_begin, which flips with SCREEN_HEIGHT - cy). The flip belongs to the
//     caller, so this test does it too — and if the translator ever grows its own flip, this test
//     is what catches the double one.
// Defined further down, next to --input-selftest/--gamepad-selftest, which is where the rest of
// the movement harness lives. Declared here so the joystick checks below can reuse it rather than
// grow a second copy — the map-border clamp lesson (see teleport_for_movement) applies to any test
// that asks whether the player moved.
void teleport_for_movement();
void rise_until_clear(int maxFrames);
void check_clear_of_map_border(const char* what);

extern "C" void eden_set_fly_mode(int);
// ---------------------------------------------------------------------------------------------
// --empty-bit-selftest  (Stage R / R.2, the per-chunk empty bit)
// ---------------------------------------------------------------------------------------------
// --stage1 proves the shortcut changes nothing on a world nobody edits. This covers the two
// transitions an edit makes, through the same Terrain::updateChunks entry point a build/mine
// reaches: a block placed in an all-air chunk must clear the bit BEFORE the next mesh decision
// (else rebuild2() skips the scan and the block is invisible), and the chunk must rejoin the
// shortcut once it is air again (rebuild2()'s scan refreshes the bit; setLand never sets it).
int run_empty_bit_selftest() {
    g_selftestTag = "eden-empty";
    // NOT --empty-selfcheck's mode: that runs the full scan on a chunk the bit calls empty, which
    // would mesh the block below even with a stale bit and make the test pass vacuously.
    const char* name = g_opt.world.empty() ? "empty-bit-selftest" : g_opt.world.c_str();
    if (!open_world(name, g_opt.height)) return 1;
    eden_console_teleport(g_opt.at[0], g_opt.at[1], g_opt.at[2]);
    tick(g_opt.frames / 2);

    // Top band of the window, a few blocks off the player: air on the bundled map at any height.
    // Off the player's REAL position -- the default --at is clamped to the map corner (see the
    // default-origin comment in eden_main_after_args), so --at itself is not where it lands.
    const PlayerState ps = player_state();
    const int x = (int)ps.x + 5, z = (int)ps.z + 5, y = g_opt.height - 3;
    char d[256];
    auto st = [&] { return eden_debug_chunk_empty(x, z, y); };
    long long s0 = st();
    std::snprintf(d, sizeof(d), "chunk at (%d,%d,%d): probe %lld", x, z, y, s0);
    check(s0 >= 0 && (s0 & 3) == 3, "target chunk is air and marked empty", d);
    if (s0 < 0 || (s0 & 3) != 3) {
        std::printf("[eden-empty] FAILURES (%d failure(s))\n", g_selftestFailures);
        return 1;
    }

    eden_console_setblock(x, z, y, 2 /* TYPE_STONE */);
    long long s1 = st();
    std::snprintf(d, sizeof(d), "probe %lld", s1);
    check((s1 & 3) == 0, "placing a block clears the bit at once", d);
    tick(30);
    long long s2 = st();
    std::snprintf(d, sizeof(d), "getblock %d, vertices %lld -> %lld",
                  eden_console_getblock(x, z, y), s0 >> 2, s2 >> 2);
    check(eden_console_getblock(x, z, y) == 2 && (s2 >> 2) > (s0 >> 2),
          "the placed block meshes (vertices grew)", d);

    eden_console_setblock(x, z, y, 0);
    tick(30);
    long long s3 = st();
    std::snprintf(d, sizeof(d), "probe %lld, vertices %lld", s3, s3 >> 2);
    check((s3 & 3) == 3 && (s3 >> 2) == (s0 >> 2), "removing it re-marks the chunk empty", d);

    const char* cs = eden_debug_chunk_state();
    const char* k1 = std::strstr(cs, "\"stale\":");
    const char* k2 = std::strstr(cs, "\"selfcheck_stale\":");
    check(k1 && atoi(k1 + 8) == 0 && k2 && atoi(k2 + 18) == 0,
          "window audit: no stale bits, self-check clean", cs);

    std::printf("[eden-empty] %s (%d failure(s))\n",
                g_selftestFailures ? "FAILURES" : "ALL PASS", g_selftestFailures);
    return g_selftestFailures ? 1 : 0;
}


// ---- S.0(a) THROWAWAY: --save-bench=1,10,50,144 (dirty-column save cost; never merged) ----------
static std::string g_saveBenchList = "1,10,50,144";
int run_save_bench() {
    g_selftestTag = "eden-savebench";
    const char* name = g_opt.world.empty() ? "S0 W64" : g_opt.world.c_str();
    if (!open_world(name, g_opt.height)) return 1;
    eden_console_teleport(g_opt.at[0], g_opt.at[1], g_opt.at[2]);
    tick(g_opt.frames / 2);
    if (const char* th = std::getenv("SAVEBENCH_THRESHOLD")) { eden_set_save_inplace_threshold(strtoull(th, 0, 10)); }
    const PlayerState ps = player_state();
    const int px = ((int)ps.x / 16) * 16 + 8, pz = ((int)ps.z / 16) * 16 + 8;
    std::printf("[eden-savebench] world=%s player=(%d,%d)\n", name, (int)ps.x, (int)ps.z);
    { const char* r = eden_bench_save(); std::printf("[eden-savebench] warmup(no dirt) %s\n", r); }
    int flip = 0;
    std::string list = g_saveBenchList;
    for (size_t pos = 0; pos < list.size();) {
        size_t c = list.find(',', pos); if (c == std::string::npos) c = list.size();
        int n = atoi(list.substr(pos, c - pos).c_str()); pos = c + 1;
        for (int rep = 0; rep < 3; ++rep) {
            ++flip;
            int side = 1; while (side * side < n) ++side;
            int placed = 0;
            for (int i = 0; i < side && placed < n; ++i)
                for (int j = 0; j < side && placed < n; ++j, ++placed)
                    eden_console_setblock(px + (i - side / 2) * 16, pz + (j - side / 2) * 16, 30, 10 + (flip % 20));
            tick(3);
            const char* r = eden_bench_save();
            std::printf("[eden-savebench] cols=%d rep=%d %s\n", n, rep, r);
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------------------------
// --light-selftest  (Stage R / R.3, the light store)
// ---------------------------------------------------------------------------------------------
// --stage1's `.light` hash covers the sweep on load. This covers what the sweep never does: the
// edit paths (buildBlock's +1 splat, destroyBlock's and paintBlock's -1, which subtracts and
// clamps at 0), saturation at 255 (five overlapping lightboxes), the y clip at the top and bottom
// of the window, and the toroidal seam (a light whose radius wraps from index T_SIZE-1 to 0 on both
// axes). Every step prints the light hash, so a run against the old dense array is the reference
// for a run against anything else; the run is also self-checking: after everything is removed
// and a fresh sweep runs, the light must be exactly what the world had before the test touched it.
int run_light_selftest() {
    g_selftestTag = "eden-light";
    const char* name = g_opt.world.empty() ? "light-selftest" : g_opt.world.c_str();
    if (!open_world(name, g_opt.height)) return 1;
    eden_console_teleport(g_opt.at[0], g_opt.at[1], g_opt.at[2]);
    tick(g_opt.frames / 2);

    auto field = [](const char* js, const char* key) -> long long {
        const char* k = std::strstr(js, key);
        return k ? atoll(k + std::strlen(key)) : -1;
    };
    auto hashOf = [](const char* js) {
        const char* k = std::strstr(js, "\"hash\":\"");
        return k ? std::string(k + 8, 16) : std::string();
    };
    auto settle = [&] {
        for (int i = 0; i < 600 && field(eden_debug_light_state(), "\"pending\":") != 0; i++) tick(1);
    };
    int step = 0;
    auto show = [&](const char* what) {
        const char* js = eden_debug_light_state();
        std::printf("[eden-light] step %d %-22s %s\n", step++, what, js);
        std::fflush(stdout);
        char d[64];
        std::snprintf(d, sizeof(d), "mismatch %lld", field(js, "\"mismatch\":"));
        if (std::strstr(js, "\"mismatch\":"))
            check(field(js, "\"mismatch\":") == 0, "self-check: the store matches the dense mirror", d);
        return std::string(js);
    };

    settle();
    const std::string s0 = show("baseline");
    // Anchors off the player's REAL position (see --empty-bit-selftest for why not --at).
    const PlayerState ps = player_state();
    const int px = (int)ps.x, pz = (int)ps.z, H = g_opt.height;
    // The seam: world x with x % T_SIZE == T_SIZE-2 (g_offcx is a multiple of T_SIZE), nearest the
    // player, so x-5..x+5 covers toroidal indices T_SIZE-7..T_SIZE-1 and 0..3. Same for z with 1.
    int xs = px - px % T_SIZE + T_SIZE - 2; if (xs - px > T_SIZE / 2) xs -= T_SIZE;
    int zs = pz - pz % T_SIZE + 1;          if (zs - pz > T_SIZE / 2) zs -= T_SIZE;
    if (pz - zs > T_SIZE / 2) zs += T_SIZE;
    const int yt = H - 3;
    struct P { int x, z, y; };
    const P A{xs, zs, yt}, B{xs + 1, zs, yt}, C{xs - 1, zs, yt}, D{xs, zs + 1, yt}, E{xs, zs, yt - 1};
    const P F{px + 3, pz + 3, 2};

    eden_debug_light_edit(0, A.x, A.z, A.y, 0);
    const std::string s1 = show("place A (seam, top)");
    check(hashOf(s1.c_str()) != hashOf(s0.c_str()) && field(s1.c_str(), "\"lit\":") > field(s0.c_str(), "\"lit\":"),
          "placing a lightbox adds light", s1.c_str());
    for (const P& p : {B, C, D, E}) eden_debug_light_edit(0, p.x, p.z, p.y, 0);
    show("place B-E (saturate)");
    eden_debug_light_edit(0, F.x, F.z, F.y, 5);
    show("place F (bottom clip)");
    eden_debug_light_edit(2, A.x, A.z, A.y, 12);
    show("repaint A");
    eden_debug_light_edit(1, B.x, B.z, B.y, 0);
    eden_debug_light_edit(1, C.x, C.z, C.y, 0);
    show("break B, C");
    for (const P& p : {A, D, E, F}) eden_debug_light_edit(1, p.x, p.z, p.y, 0);
    show("break A, D, E, F");
    tick(30);
    eden_debug_light_edit(3, 0, 0, 0, 0);
    settle();
    const std::string s7 = show("fresh sweep");
    check(hashOf(s7.c_str()) == hashOf(s0.c_str()), "after a fresh sweep the light is the pre-test light", s7.c_str());
#if 1   // R.3: the sparse store's promises (would fail against the dense array, by design)
    {
        const long long dense = 3LL * T_SIZE * T_SIZE * H;
        const long long b = field(s7.c_str(), "\"bytes\":");
        char d[96];
        std::snprintf(d, sizeof(d), "%lld bytes held vs %lld dense", b, dense);
        check(b >= 0 && b * 8 < dense, "the store holds well under 1/8 of the dense array", d);
    }
#endif

    std::printf("[eden-light] %s (%d failure(s))\n",
                g_selftestFailures ? "FAILURES" : "ALL PASS", g_selftestFailures);
    return g_selftestFailures ? 1 : 0;
}

int run_touch_selftest() {
    g_selftestTag = "eden-touch";
    g_tickInput = true;

    // A touch test runs in the TOUCH profile, on every platform. Without this the desktop legs
    // test the touch translator against a layout that has no on-screen joystick at all
    // (`use_joystick` is a profile field — Settings_web.mm's eden_apply_input_profile), so the
    // stick checks below would be asserting against chrome that is not drawn. iOS already sets
    // this in main(); saying it again here is what makes the four platforms run the same test.
    eden_set_detected_touch(1);

    const char* name = g_opt.world.empty() ? "touch-selftest" : g_opt.world.c_str();
    if (!open_world(name, g_opt.height)) return 1;
    tick(120);

    // Ask the engine where its menu button is. `rmenu` is [x, y, w, h] in point space.
    float rx = 0, ry = 0, rw = 0, rh = 0;
    {
        const char* st = eden_debug_display_state();
        const char* p = st ? std::strstr(st, "\"rmenu\":[") : nullptr;
        if (p) std::sscanf(p + 9, "%f,%f,%f,%f", &rx, &ry, &rw, &rh);
    }
    char detail[220];
    std::snprintf(detail, sizeof(detail), "rmenu [%.1f,%.1f,%.1f,%.1f] in a %.0fx%.0f point space",
                  rx, ry, rw, rh, SCREEN_WIDTH, SCREEN_HEIGHT);
    check(rw > 0.0f && rh > 0.0f, "the engine reports a menu-button rect", detail);
    if (rw <= 0.0f || rh <= 0.0f) {
        std::printf("[eden-touch] FAILURES (%d failure(s))\n", ++g_selftestFailures);
        return 1;
    }

    // Centre of the rect, flipped into the top-left space the pointer API uses...
    const float cx = rx + rw * 0.5f;
    const float cy = SCREEN_HEIGHT - (ry + rh * 0.5f);

    // ...and then normalised the way SDL reports a finger — THROUGH THE LETTERBOX, which is the
    // half of this test that matters. A finger is a fraction of the WHOLE WINDOW, and the HUD is
    // drawn inside a box that may be smaller than the window (bars, when the layout's aspect and
    // the screen's differ — the touch profile pinned 16:9 until 2026-09-07, and the clamps in
    // DisplayProfile_web.mm can still make the two differ on an extreme aspect). Assuming the
    // fraction
    // maps straight onto the point space is exactly the assumption the translator used to make,
    // and it is what an iPad Air 2 reported as "switches don't respond to touch at all". So this
    // computes the inverse of what the translator does; if the two ever disagree, the tap misses
    // and this test says so.
    // Point space (top-left origin) -> the 0..1 window fraction SDL reports a finger as. Now a
    // lambda because the joystick checks at the bottom need it too: any test that pushes a finger
    // has to invert exactly what the translator does, and having two copies of that inverse is how
    // the two silently stop agreeing.
    auto to_norm = [&](float px, float pyTopLeft, float* outNx, float* outNy) {
        float nx = px / SCREEN_WIDTH;
        float ny = pyTopLeft / SCREEN_HEIGHT;
        // Read the box LIVE rather than reusing the capture above: opening a world re-derives the
        // display metrics (the profile seeder runs on the first settings pump), so a value read
        // before that is a value from a different layout.
        int lx = 0, ly = 0, lw = 0, lh = 0;
        eden_native_gl_get_letterbox(&lx, &ly, &lw, &lh);
        const float bx = (float)lx, by = (float)ly, bw = (float)lw, bh = (float)lh;
        if (bw > 0.0f && bh > 0.0f && eden_native_gl_window()) {
            // Pixels -> window points, the same conversion window_to_point() makes.
            int ww = 0, wh = 0, pw = 0, ph = 0;
            SDL_GetWindowSize(eden_native_gl_window(), &ww, &wh);
            SDL_GetWindowSizeInPixels(eden_native_gl_window(), &pw, &ph);
            const float sx = (pw > 0 && ww > 0) ? (float)pw / (float)ww : 1.0f;
            const float sy = (ph > 0 && wh > 0) ? (float)ph / (float)wh : 1.0f;
            const float boxXpt = bx / sx, boxYpt = by / sy;
            const float boxWpt = bw / sx, boxHpt = bh / sy;
            if (ww > 0 && wh > 0 && boxWpt > 0.0f && boxHpt > 0.0f) {
                nx = (boxXpt + px        / SCREEN_WIDTH  * boxWpt) / (float)ww;
                ny = (boxYpt + pyTopLeft / SCREEN_HEIGHT * boxHpt) / (float)wh;
            }
        }
        *outNx = nx;
        *outNy = ny;
    };
    float nx = 0.0f, ny = 0.0f;
    to_norm(cx, cy, &nx, &ny);
    if (eden_native_gl_window()) {
        int lx = 0, ly = 0, lw = 0, lh = 0, pw = 0, ph = 0;
        eden_native_gl_get_letterbox(&lx, &ly, &lw, &lh);
        SDL_GetWindowSizeInPixels(eden_native_gl_window(), &pw, &ph);
        if (lw > 0 && lh > 0) {
            std::snprintf(detail, sizeof(detail), "box %dx%d px at (%d,%d) in a %dx%d px surface",
                          lw, lh, lx, ly, pw, ph);
            check(lw <= pw && lh <= ph, "the letterbox fits inside the surface", detail);
        }
    }

    auto push_finger_id = [&](SDL_FingerID id, Uint32 type, float x, float y) {
        SDL_Event ev;
        SDL_zero(ev);
        ev.type = type;
        ev.tfinger.timestamp = SDL_GetTicksNS();
        ev.tfinger.touchID = 1;      // any non-zero device; the translator keys on fingerID
        ev.tfinger.fingerID = id;    // the slot table is what maps it onto a pointer identity
        ev.tfinger.x = x;
        ev.tfinger.y = y;
        ev.tfinger.dx = 0.0f;
        ev.tfinger.dy = 0.0f;
        ev.tfinger.pressure = 1.0f;
        SDL_PushEvent(&ev);
    };
    auto push_finger = [&](Uint32 type, float x, float y) { push_finger_id(7, type, x, y); };

    // --- a tap on the menu icon opens the in-game menu ---
    {
        const int before = eden_hud_in_menu();
        push_finger(SDL_EVENT_FINGER_DOWN, nx, ny);
        tick(8);
        push_finger(SDL_EVENT_FINGER_UP, nx, ny);
        tick(24);
        const int after = eden_hud_in_menu();
        std::snprintf(detail, sizeof(detail), "tap at (%.3f,%.3f) normalised: in_menu %d -> %d",
                      nx, ny, before, after);
        check(after != before, "a finger tap reaches the HUD", detail);
    }

    // --- and a second tap closes it, which is the part a stuck touch would break ---
    {
        const int before = eden_hud_in_menu();
        push_finger(SDL_EVENT_FINGER_DOWN, nx, ny);
        tick(8);
        push_finger(SDL_EVENT_FINGER_UP, nx, ny);
        tick(24);
        const int after = eden_hud_in_menu();
        std::snprintf(detail, sizeof(detail), "in_menu %d -> %d", before, after);
        check(after != before, "a second tap closes it (no stuck touch)", detail);
    }

    // --- a CANCELLED finger must not leave its slot occupied ---
    // Nine fingers is a dropped ninth, by design; a leaked slot turns into one after eight taps.
    // This is the cheap proof that the release path runs for cancel as well as for up.
    {
        for (int i = 0; i < 10; ++i) {
            push_finger(SDL_EVENT_FINGER_DOWN, 0.5f, 0.5f);
            tick(2);
            push_finger(SDL_EVENT_FINGER_CANCELED, 0.5f, 0.5f);
            tick(2);
        }
        const int before = eden_hud_in_menu();
        push_finger(SDL_EVENT_FINGER_DOWN, nx, ny);
        tick(8);
        push_finger(SDL_EVENT_FINGER_UP, nx, ny);
        tick(24);
        const int after = eden_hud_in_menu();
        std::snprintf(detail, sizeof(detail), "after 10 cancelled fingers: in_menu %d -> %d",
                      before, after);
        check(after != before, "cancelled fingers free their slots", detail);
    }

    // Back to PLAYING before anything below runs. The three checks above leave the in-game menu
    // open, and `g_relativeMouse` — the flag that decides whether a mouse event looks/mines at all
    // — is derived from `eden_hud_in_menu()`. The first version of the synthetic-mouse checks
    // below sat here with the menu open, so they passed with the fix REMOVED: they were asserting
    // against a mouse path that was switched off, not against the guard. A gate has to be run once
    // with the bug put back.
    if (eden_hud_in_menu()) {
        push_finger(SDL_EVENT_FINGER_DOWN, nx, ny);
        tick(8);
        push_finger(SDL_EVENT_FINGER_UP, nx, ny);
        tick(24);
    }
    std::snprintf(detail, sizeof(detail), "in_menu %d", eden_hud_in_menu());
    check(eden_hud_in_menu() == 0, "the checks below run while PLAYING, not in the menu", detail);

    // ---------------------------------------------------------------------------------------
    // WHAT THIS GATE DID NOT ASSERT UNTIL NOW, and what the second device pass cost because of it
    // (STATUS §2.0.12). Everything above taps: down, a few ticks, up. That proves a finger reaches
    // the HUD and that a slot comes back. It says nothing about (a) a finger that is HELD — which
    // is the entire on-screen joystick — and nothing about (b) the events SDL generates BESIDES
    // the finger, which on a real touchscreen is a whole second mouse.
    // ---------------------------------------------------------------------------------------------

    // --- SDL's touch->mouse emulation must not reach the game twice ---
    // SDL_HINT_TOUCH_MOUSE_EVENTS defaults to "1", so on a device every finger also arrives as a
    // mouse event with `which == SDL_TOUCH_MOUSEID`. While playing, `g_relativeMouse` is true, so
    // that second copy went to mouse-look and to the hold-to-mine slot: one tap edited a block
    // where the finger landed AND at the crosshair ("breaking a block breaks it in TWO PLACES AT
    // ONCE"), and dragging the stick swung the camera. SDL_PushEvent does not synthesise these, so
    // this pushes them by hand — which is the only way a desk can see an iOS-only default.
    {
        auto push_mouse = [&](Uint32 type, SDL_MouseID which, float xrel, float yrel) {
            SDL_Event ev;
            SDL_zero(ev);
            ev.type = type;
            if (type == SDL_EVENT_MOUSE_MOTION) {
                ev.motion.timestamp = SDL_GetTicksNS();
                ev.motion.which = which;
                ev.motion.x = SCREEN_WIDTH * 0.5f;
                ev.motion.y = SCREEN_HEIGHT * 0.5f;
                ev.motion.xrel = xrel;
                ev.motion.yrel = yrel;
            } else {
                ev.button.timestamp = SDL_GetTicksNS();
                ev.button.which = which;
                ev.button.button = SDL_BUTTON_LEFT;
                ev.button.down = (type == SDL_EVENT_MOUSE_BUTTON_DOWN);
                ev.button.clicks = 1;
                ev.button.x = SCREEN_WIDTH * 0.5f;
                ev.button.y = SCREEN_HEIGHT * 0.5f;
            }
            SDL_PushEvent(&ev);
        };

        const PlayerState before = player_state();
        push_mouse(SDL_EVENT_MOUSE_MOTION, SDL_TOUCH_MOUSEID, 200.0f, 120.0f);
        tick(4);
        const PlayerState after = player_state();
        std::snprintf(detail, sizeof(detail), "yaw %.2f -> %.2f, pitch %.2f -> %.2f",
                      before.yaw, after.yaw, before.pitch, after.pitch);
        check(after.yaw == before.yaw && after.pitch == before.pitch,
              "a touch-synthesised mouse motion does not look", detail);

        push_mouse(SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_TOUCH_MOUSEID, 0.0f, 0.0f);
        tick(4);
        const int claimed = eden_native_input_debug_hold_active();
        push_mouse(SDL_EVENT_MOUSE_BUTTON_UP, SDL_TOUCH_MOUSEID, 0.0f, 0.0f);
        tick(4);
        std::snprintf(detail, sizeof(detail), "hold-to-act slot claimed: %d (expected 0)", claimed);
        check(claimed == 0, "a touch-synthesised mouse button does not mine", detail);
    }

    // --- a HELD finger on the joystick walks, and KEEPS walking ---
    // The stick is the one control that is a sustained state rather than an event, and this gate
    // had never held anything. "Pushing the joystick forward nudges the player a tiny bit at a
    // time" is precisely what a held finger looks like when only its first frame counts, so the
    // assertion is not just "did the player move" — it is "did the player move as far in the
    // SECOND half of the hold as in the first", with no further SDL events in between.
    {
        float jx = 0, jy = 0, jw = 0, jh = 0;
        {
            const char* st = eden_debug_display_state();
            const char* p2 = st ? std::strstr(st, "\"rjoystick\":[") : nullptr;
            if (p2) std::sscanf(p2 + 13, "%f,%f,%f,%f", &jx, &jy, &jw, &jh);
        }
        std::snprintf(detail, sizeof(detail), "rjoystick [%.1f,%.1f,%.1f,%.1f], use_joystick %s",
                      jx, jy, jw, jh, std::strstr(eden_debug_display_state(), "\"use_joystick\":1")
                                          ? "on" : "OFF");
        check(jw > 0.0f && jh > 0.0f, "the engine reports an on-screen joystick pad", detail);

        if (jw > 0.0f && jh > 0.0f) {
            teleport_for_movement();   // the map CENTRE — see its own comment for why --at's is not
            tick(120);
            rise_until_clear(1800);    // and clear of terrain, so "did not move" means the input
            check_clear_of_map_border("the player is clear of the map border");

            // Top of the pad, horizontally centred: full forward tilt. Point space is bottom-left
            // here (the rects are), and the pointer API is top-left — the same flip the rmenu tap
            // above makes, and the reason it is written out rather than folded into to_norm().
            const float px = jx + jw * 0.5f;
            const float py = SCREEN_HEIGHT - (jy + jh - 4.0f);
            float snx = 0.0f, sny = 0.0f;
            to_norm(px, py, &snx, &sny);

            const PlayerState p0 = player_state();
            push_finger_id(11, SDL_EVENT_FINGER_DOWN, snx, sny);
            tick(150);
            const PlayerState p1 = player_state();
            tick(150);                          // held, with NO further SDL events at all
            const PlayerState p2 = player_state();
            push_finger_id(11, SDL_EVENT_FINGER_UP, snx, sny);
            tick(20);

            const float d1 = std::sqrt((p1.x - p0.x) * (p1.x - p0.x) + (p1.z - p0.z) * (p1.z - p0.z));
            const float d2 = std::sqrt((p2.x - p1.x) * (p2.x - p1.x) + (p2.z - p1.z) * (p2.z - p1.z));
            std::snprintf(detail, sizeof(detail),
                          "finger held at (%.3f,%.3f): %.2f blocks in the first 150 ticks",
                          snx, sny, d1);
            check(d1 > 0.5f, "a held finger on the joystick walks", detail);
            std::snprintf(detail, sizeof(detail),
                          "%.2f blocks then %.2f blocks, no events in between", d1, d2);
            check(d2 > d1 * 0.5f, "and it keeps walking while it is held", detail);

            // Releasing must stop DRIVING the player — which is not the same as stopping the
            // player, and the difference is worth stating because the first version of this check
            // asserted the wrong one and failed. These checks run in fly mode (rise_until_clear,
            // so terrain cannot be mistaken for dead input), and fly mode damps lateral velocity
            // by 0.995 per frame on purpose (Classes/Player.mm:1036 — it is a glide). A released
            // stick therefore coasts for a long way and that is correct engine behaviour. What a
            // stuck touch would look like instead is a rate that does NOT decay, so decay is what
            // this asserts: six windows, each one slower than the last.
            // "Never faster than the first window, and clearly slower by the last" rather than a
            // strictly monotonic series: the engine integrates on its own substep, so one window
            // of six can come out a hair above its predecessor without anything being wrong. What
            // cannot happen while coasting is a window that beats the FIRST one.
            float first = 0.0f, last = 0.0f, peak = 0.0f;
            for (int w = 0; w < 6; ++w) {
                const PlayerState a = player_state();
                tick(20);
                const PlayerState b = player_state();
                const float d = std::sqrt((b.x - a.x) * (b.x - a.x) + (b.z - a.z) * (b.z - a.z));
                if (w == 0) first = d;
                if (d > peak) peak = d;
                last = d;
            }
            std::snprintf(detail, sizeof(detail),
                          "coasting down: %.3f -> %.3f blocks per 20 ticks, peak %.3f "
                          "(fly-mode 0.995 damping)", first, last, peak);
            // (1.02 lost to 1.175 vs 1.151 on Windows CI: substep jitter, as above.)
            check(peak <= first * 1.05f && last < first * 0.9f,
                  "lifting the finger stops driving the player", detail);


            // --- and WITHOUT fly mode: a held stick walks ---
            // Everything above flies (rise_until_clear), so until N.4.7 this gate never walked, and
            // "fly is fine, walking barely moves" reached a device. On a stone runway built in open
            // air, because the map centre is ponds and hills and a walk into water or a hillside
            // is not a question about input. Fixed 1/60 s etime: the engine's walk speed depends
            // on its tick rate (per-tick ground friction), so a wall-clock run would assert on
            // whatever rate this machine's vsync happens to give the harness. The tick rate the
            // REAL loop feeds the engine is the interactive loop's pacing, not this gate's.
            eden_set_fly_mode(0);
            g_app->viewController.setFixedEtime(1.0f / 60.0f);
            const int ry = g_opt.height - 14;
            const float wx = (float)(4096 * CHUNK_SIZE) + 0.5f, wz = (float)(4096 * CHUNK_SIZE) + 0.5f;
            eden_console_teleport(wx, (float)ry + 3.0f, wz);
            tick(60);
            for (int x = -20; x <= 20; ++x)      // square, so a yaw nudged by a stray host mouse
                for (int z = -20; z <= 20; ++z) {    // still walks on stone
                    eden_console_setblock((int)wx + x, (int)wz + z, ry, 2 /*TYPE_STONE*/);
                    for (int y = 1; y <= 4; ++y) eden_console_setblock((int)wx + x, (int)wz + z, ry + y, 0);
                }
            eden_console_teleport(wx, (float)ry + 2.5f, wz);
            tick(120);
            const PlayerState w0 = player_state();
            push_finger_id(12, SDL_EVENT_FINGER_DOWN, snx, sny);
            tick(120);
            const PlayerState w1 = player_state();
            push_finger_id(12, SDL_EVENT_FINGER_UP, snx, sny);
            tick(30);
            g_app->viewController.setFixedEtime(0.0f);
            eden_set_fly_mode(1);
            const float dw = std::sqrt((w1.x - w0.x) * (w1.x - w0.x) + (w1.z - w0.z) * (w1.z - w0.z));
            std::snprintf(detail, sizeof(detail),
                          "%.2f blocks in 2 s of engine time on flat stone (~7.3 expected), dy %.2f",
                          dw, w1.y - w0.y);
            check(dw > 4.0f && std::fabs(w1.y - w0.y) < 0.5f, "a held stick walks with fly OFF", detail);
        }
    }

    std::printf("[eden-touch] %s (%d failure(s))\n",
                g_selftestFailures ? "FAILURES" : "ALL PASS", g_selftestFailures);
    return g_selftestFailures ? 1 : 0;
}

// ---------------------------------------------------------------------------------------------
// --objc-selftest  (Phase N Stage 3.3)
// ---------------------------------------------------------------------------------------------
// A mode that exists only on the targets that USE this port's own Objective-C runtime — i.e. not
// macOS, which uses Apple's libobjc and needs no proof from us. On Linux and Windows it is the
// single most load-bearing check in the stage: web/src/shim/objc/objc_runtime.cpp is 800 lines of
// hand-written ABI, it had only ever run on wasm32, and every failure mode it has is silent
// (a wrong ivar offset reads the wrong memory; a wrong dispatch reaches the wrong method).
//
// The proof itself is web/src/shim/objc/objc_selftest.mm, shared with the web build's
// headless-shim-selftest suite, so a regression shows up on both targets from one source.
//
// It runs BEFORE any world is touched — a broken runtime would otherwise present as a corrupt
// save or a wrong checksum several thousand message sends downstream, which is exactly the kind of
// diagnosis this project has paid for before.
#if !defined(__APPLE__) && defined(EDEN_DIAGNOSTICS)
extern "C" int eden_objc_selftest_run(void);
#endif

int run_objc_selftest() {
#if defined(__APPLE__)
    std::fprintf(stderr, "[eden-objc] this target uses Apple's libobjc, not "
                         "web/src/shim/objc/objc_runtime.cpp — nothing to self-test.\n");
    return 0;
#elif !defined(EDEN_DIAGNOSTICS)
    std::fprintf(stderr, "[eden-objc] built without -DEDEN_DIAGNOSTICS=ON; the self-test is not "
                         "compiled in.\n");
    return 1;
#else
    return eden_objc_selftest_run() == 1 ? 0 : 1;
#endif
}

// ---------------------------------------------------------------------------------------------
// --audio-selftest
// ---------------------------------------------------------------------------------------------
// The audio twin of --input-selftest, and it exists for the same reason: "sound works" is
// otherwise only checkable by someone with speakers on, and that is not a regression test. It
// asserts the two things that can silently break — that the DECODER handled each of the five
// formats the sound library actually ships, and that a played effect reached the MIXER — and
// leaves the one thing it cannot judge (does it sound right) to a human.
//
// The five probes are one real file per format, chosen from what Classes/Resources.mm names:
// IMA4 .caf is 428 of the 436 effects, the others are a handful each.
int run_audio_selftest() {
    g_selftestTag = "eden-audio";
    struct Probe { const char* file; const char* what; };
    const Probe probes[] = {
        {"block_break_stone_1.caf",              ".caf (Apple IMA4 — 428 of 436 effects)"},
        {"explosion.caf",                        ".caf (the explosion set)"},
        {"ambience_open.wav",                    ".wav (ambience bed)"},
        {"trampoline_block_bounce_sound.mp3",    ".mp3 (effect)"},
        {"Eden_title.mp3",                       ".mp3 (music track)"},
        // Phase N Stage 3.5. One file in the whole library is AIFF, and on the non-Apple targets
        // it is the only user of a hand-written decoder that nothing else exercises — so without
        // this probe the AIFF path has no coverage at all on the platforms that actually need it.
        {"Grab.aif",                             ".aif (the one AIFF in the library)"},
    };
    char detail[200];
    for (const Probe& p : probes) {
        int frames = 0, ch = 0, rate = 0;
        const bool ok = eden_native_audio_probe(p.file, &frames, &ch, &rate) != 0;
        if (ok) std::snprintf(detail, sizeof(detail), "%s: %d frames, %d ch, %d Hz",
                              p.file, frames, ch, rate);
        else    std::snprintf(detail, sizeof(detail), "%s: DECODE FAILED", p.file);
        check(ok && frames > 0, p.what, detail);
        // Drop it again: Eden_title.mp3 decodes to ~8 M frames, i.e. ~32 MB of PCM in the effect
        // cache, and the cache is meant for short sounds. Nothing in the engine plays a music
        // track as an effect; only this probe does, and only to prove the decoder reads mp3.
        CocosDenshion::SimpleAudioEngine::sharedEngine()->unloadEffect(p.file);
    }

    // Reaching the mixer. playSound() goes through Resources, i.e. the path the engine itself
    // uses, rather than calling the backend directly — the point is that the ENGINE's sound comes
    // out, not that the backend can be driven.
    {
        CocosDenshion::SimpleAudioEngine* a = CocosDenshion::SimpleAudioEngine::sharedEngine();
        const unsigned int id = a->playEffect("explosion.caf", false);
        const int voices = eden_native_audio_voice_count();
        std::snprintf(detail, sizeof(detail), "voice id %u, %d voice(s) bound", id, voices);
        check(id != 0 && voices > 0, "an effect reaches the mixer", detail);

        // ...and drains. A voice that never reports empty is the leak that fills the 48-voice
        // pool a few minutes into a session, so the reap path is asserted, not assumed.
        for (int i = 0; i < 600 && eden_native_audio_voice_count() > 0; ++i) {
            eden_native_audio_tick();
            SDL_Delay(10);
        }
        std::snprintf(detail, sizeof(detail), "%d voice(s) left", eden_native_audio_voice_count());
        check(eden_native_audio_voice_count() == 0, "the voice drains and is reaped", detail);
    }

    // A streaming channel. isBackgroundMusicPlaying() is AVAudioPlayer's own view of itself, so
    // this is "the file opened, decoded and is running", not "we called play".
    {
        CocosDenshion::SimpleAudioEngine* a = CocosDenshion::SimpleAudioEngine::sharedEngine();
        a->setBackgroundMusicVolume(0.6f);
        a->playBackgroundMusic("Eden_title.mp3", false);
        SDL_Delay(300);
        const bool playing = a->isBackgroundMusicPlaying();
        std::snprintf(detail, sizeof(detail), "isBackgroundMusicPlaying=%d", playing ? 1 : 0);
        check(playing, "the music channel streams", detail);
        a->stopBackgroundMusic(false);
    }

    // An ambience channel, driven the way Resources::update drives them: play, then push a fade
    // value every frame. The fade is the half that is easy to get wrong — a channel that plays at
    // volume 0 forever is indistinguishable from one that never started.
    {
        CocosDenshion::SimpleAudioEngine* a = CocosDenshion::SimpleAudioEngine::sharedEngine();
        a->setAmbienceVolume(1.0f);
        a->playAmbience(0, "ambience_open.wav", true);
        a->setAmbienceFade(0, 1.0f);
        SDL_Delay(400);
        std::snprintf(detail, sizeof(detail), "isAmbiencePlaying(0)=%d", a->isAmbiencePlaying(0) ? 1 : 0);
        check(a->isAmbiencePlaying(0), "an ambience layer streams", detail);
        a->stopAmbience(0);
    }

    // ...and the same thing IN A WORLD, driven by the engine rather than by this harness. This is
    // the leg that matters: the direct call above proves the backend works, and only a real
    // session proves Resources::soundEventBed ever reaches it (it early-returns unless `playmusic`
    // is on and the game mode is PLAY, and the bed layer is chosen from the biome under the
    // player).
    {
        g_tickInput = false;
        const char* w = g_opt.world.empty() ? "audio-selftest" : g_opt.world.c_str();
        if (open_world(w, g_opt.height)) {
            tick(600);
            CocosDenshion::SimpleAudioEngine* a = CocosDenshion::SimpleAudioEngine::sharedEngine();
            const int bed = a->isAmbiencePlaying(0) ? 1 : 0;
            const int prox = a->isAmbiencePlaying(1) ? 1 : 0;
            std::snprintf(detail, sizeof(detail),
                          "bed=%d(%s vol %.2f) prox=%d playmusic=%d music=%d",
                          bed, eden_native_audio_channel_name(1),
                          eden_native_audio_channel_volume(1), prox,
                          Resources::getResources ? Resources::getResources->playmusic : -1,
                          a->isBackgroundMusicPlaying() ? 1 : 0);
            check(bed != 0 && eden_native_audio_channel_volume(1) > 0.0f,
                  "the engine starts an AUDIBLE ambience bed in-world", detail);

            // 2026-10-05, two music bugs the user heard. (1) Resources::update wrote its song
            // crossfade into the SLIDER's knob every frame, so a Music volume change lasted one
            // frame. The slider value must survive the engine running.
            auto idx = [](const char* key) {
                for (int i = 0; i < eden_settings_count(); ++i)
                    if (std::strcmp(eden_settings_key(i), key) == 0) return i;
                return -1;
            };
            const int mv = idx("music_volume"), mu = idx("music");
            if (mv >= 0) {
                eden_settings_set(mv, 0.30f);
                tick(30);
                const float got = a->getBackgroundMusicVolume();
                std::snprintf(detail, sizeof(detail), "set 0.30, after 30 frames %.2f", got);
                check(std::fabs(got - 0.30f) < 0.001f, "the Music volume setting survives Resources::update", detail);
                eden_settings_set(mv, 1.0f);
            }
            // (2) Switching Music on from the in-game settings called playMenuTune(): a TITLE
            // track over gameplay. In a world it must cue an in-game song instead.
            if (mu >= 0) {
                eden_settings_set(mu, 0.0f);
                tick(5);
                eden_settings_set(mu, 1.0f);
                tick(30);
                const char* ch0 = eden_native_audio_channel_name(0);
                std::snprintf(detail, sizeof(detail), "channel 0 = %s, playing=%d",
                              ch0 ? ch0 : "(none)", a->isBackgroundMusicPlaying() ? 1 : 0);
                check(ch0 && !std::strstr(ch0, "title") && a->isBackgroundMusicPlaying(),
                      "Music switched on in a world plays an in-game song, not a title track", detail);
            }
        }
    }

    std::printf("[eden-audio] %s (%d failure(s))\n",
                g_selftestFailures ? "FAILURES" : "ALL PASS", g_selftestFailures);
    return g_selftestFailures ? 1 : 0;
}

// ---------------------------------------------------------------------------------------------
// --leak-probe — the native counterpart of web/tools/headless-alloc-leak-probe.js and of
// headless-heap-ceiling-probe.js --torture=same64 (Phase N Stage 3 verification).
// ---------------------------------------------------------------------------------------------
// WHY THIS IS A NATIVE MODE AND NOT A REUSE OF THOSE TOOLS: both of them are node scripts that
// instantiate `eden.js` and call exports through `Module._`. There is no Module here. The measured
// quantity, though, is exactly theirs — load a world, quit to the menu, repeat in ONE process, and
// require the allocator's live-bytes figure to be FLAT — so this drives the same engine exports in
// the same order and prints the same statistic under the same key.
//
// It is Stage 3's obligation rather than a nice-to-have because the shim Foundation is running
// outside Emscripten for the first time, and that is precisely M6's leak class: this runtime's
// object_dispose() is a bare free() with no .cxx_destruct, so every shim class holding a C++ ivar
// owes a hand-written -dealloc. A missing one leaks silently and only shows up as a slope.
//
// The signal is `heapSize` (HeapProbe_native.cpp's allocInUse: malloc_zone size_in_use on Apple,
// mallinfo2 uordblks on glibc, PrivateUsage commit charge on Windows — read the Windows one as a
// trend, not a byte count). RSS is printed beside it but NOT asserted on: the OS is free to keep
// freed pages mapped, so RSS can plateau above the live figure without anything leaking.
//
// CYCLE 0 IS A WARM-UP AND IS NOT IN THE SLOPE. The first load populates caches that are supposed
// to persist for the life of the process — the texture set, the atlas, the resident block window —
// and counting that as leakage would make any threshold meaningless.
double heap_field(const char* json, const char* key) {
    char pat[64];
    std::snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char* k = std::strstr(json, pat);
    return k ? atof(k + std::strlen(pat)) : -1.0;
}

int run_leak_probe() {
    g_selftestTag = "eden-leak";
    const char* name = g_opt.world.empty() ? "leak-probe" : g_opt.world.c_str();
    const int cycles = g_opt.cycles < 3 ? 3 : g_opt.cycles;
    const double MB = 1024.0 * 1024.0;

    std::vector<double> live(cycles, 0.0), rss(cycles, 0.0);

    tick(g_opt.frames / 4);
    std::printf("[eden-leak] %d cycles of load->quit on a %dz world, one process\n",
                cycles, g_opt.height);
    std::fflush(stdout);

    for (int c = 0; c < cycles; ++c) {
        if (!open_world(name, g_opt.height)) return 1;
        // The same fixed origin the checksum modes use, so the load does real streaming work
        // rather than sitting on whatever column the spawn picked.
        eden_console_teleport(g_opt.at[0], g_opt.at[1], g_opt.at[2]);
        tick(g_opt.frames / 2);
        if (!quit_to_menu()) return 1;
        tick(g_opt.frames / 4);

        const char* h = eden_debug_heap();
        live[c] = heap_field(h, "heapSize");
        rss[c]  = heap_field(h, "rss");
        std::printf("[eden-leak] cycle %d/%d at the menu: live %7.2f MB   rss %7.2f MB%s\n",
                    c + 1, cycles, live[c] / MB, rss[c] / MB,
                    c == 0 ? "   (warm-up, not in the slope)" : "");
        std::fflush(stdout);
    }

    // A LEAST-SQUARES SLOPE, not first-vs-last, and the difference is not pedantry: the live figure
    // jitters by several MB between samples (18.5 / 23.6 / 23.7 / 19.3 / 19.0 in the run this was
    // sized against), because how much of the block window and how many pooled meshes are alive at
    // the instant of the sample is not identical cycle to cycle. Two endpoints can land on opposite
    // ends of that jitter and manufacture a slope out of nothing; a fit over every post-warm-up
    // point cannot.
    const int n = cycles - 1;                       // cycles 1..cycles-1, i.e. warm-up excluded
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (int c = 1; c < cycles; ++c) {
        const double x = (double)c, y = live[c];
        sx += x; sy += y; sxx += x * x; sxy += x * y;
    }
    const double denom = (double)n * sxx - sx * sx;
    const double perCycle = denom != 0.0 ? ((double)n * sxy - sx * sy) / denom : 0.0;
    char detail[220];
    std::snprintf(detail, sizeof(detail),
                  "%+.2f MB/cycle fitted over cycles 2..%d (live %.2f -> %.2f MB)",
                  perCycle / MB, cycles, live[1] / MB, live[cycles - 1] / MB);
    // 4 MB/cycle, and the number is sized from the jitter rather than from the leak. Three flat
    // 8-cycle runs on the development Mac fitted -0.71, +0.94 and +0.45 MB/cycle; the samples they
    // were fitted through oscillate in roughly 4.6 MB steps (18.4 / 23.0 / 27.9 recur exactly),
    // which is the resolution this statistic actually has. M6's leak was ~22 MB PER LOAD, so 4 MB
    // still catches it five times over. Raise this only with a measurement that says why.
    check(perCycle < 4.0 * MB, "the live heap is flat across world loads", detail);

    std::printf("[eden-leak] %s (%d failure(s))\n",
                g_selftestFailures ? "FAILURES" : "ALL PASS", g_selftestFailures);
    return g_selftestFailures ? 1 : 0;
}

// ---------------------------------------------------------------------------------------------
// The bundled map has a HARD BORDER, and this harness's default origin sits exactly on it.
// ---------------------------------------------------------------------------------------------
// Classes/Player.mm's preupdate clamps pos.x and pos.z into
//     [4096*CHUNK_SIZE - GSIZE/2, 4096*CHUNK_SIZE + GSIZE/2]
// on every world carrying DEFAULT_LEVEL_SEED — which is every world these selftests create. GSIZE
// is T_SIZE*10 = 2880 and 4096*CHUNK_SIZE is 65536, so the playable square is [64096, 66976] on
// both axes, and this file's default `--at=1440,40,1440` (shared, verbatim, with
// web/tools/headless-mesh-checksum.js) is OUTSIDE it on both axes. The engine clamps it to exactly
// the south-west corner, 64096,64096 — which is where every scripted run has been standing.
//
// THAT CLAMP, NOT THE INPUT PATH AND NOT TERRAIN, IS THE "moved 0.00 blocks" FLAKE. A player on
// the corner cannot move in -X or -Z at all: `pos.x += vel.x*etime` runs, the clamp puts pos.x
// straight back, and the reported displacement is EXACTLY zero — the tell that separates this from
// a blocked player, who always drifts a fraction. Measured with a per-frame dump: velocity built
// normally to -112 blocks/s while pos.x and pos.z never left 64096.0000, with the whole 9x9
// neighbourhood reading air. Whether a run passed was decided by the spawn's random yaw: the ~half
// of the circle pointing into the map moved 40-70 blocks, the other half moved 0.00 or a fraction.
// That is the "1 in 6" both movement selftests have been flaking at, and the previous sessions'
// hill-and-retry story was fitting a curve to it.
//
// So the movement selftests start at the map's CENTRE instead — 1440 blocks of clearance in every
// direction, which no 300-frame push can reach. An explicit --at still wins: a human debugging a
// specific spot is entitled to stand on the border.
//
// The CHECKSUM modes deliberately keep the corner origin. It is a fixed, in-bounds-after-clamping
// position that every platform lands on identically, which is all criterion 2 asks of it, and
// re-recording c26fa4fd2a5b2660 / 49b71a4e16afb125 / b19d3e33a8717c60 across four platforms buys
// nothing. What was wrong was the belief about where it was, not the number.
constexpr float kMapCentre = (float)(4096 * CHUNK_SIZE);
constexpr float kMapHalf   = (float)(GSIZE / 2);

// Teleport for a movement check: the map centre unless the caller named a spot.
void teleport_for_movement() {
    if (g_opt.haveAt) {
        eden_console_teleport(g_opt.at[0], g_opt.at[1], g_opt.at[2]);
        return;
    }
    eden_console_teleport(kMapCentre, g_opt.at[1], kMapCentre);
}

// ...and then SAY SO if we are on the border anyway. Without this the failure mode above is
// indistinguishable from a dead translator, which is what cost two sessions; with it, a run that
// somehow ends up clamped names itself. 64 blocks is comfortably more than the ~70 a 300-frame
// full-speed fly push covers.
void check_clear_of_map_border(const char* what) {
    const PlayerState p = player_state();
    const float marginX = std::fmin(p.x - (kMapCentre - kMapHalf), (kMapCentre + kMapHalf) - p.x);
    const float marginZ = std::fmin(p.z - (kMapCentre - kMapHalf), (kMapCentre + kMapHalf) - p.z);
    char detail[160];
    std::snprintf(detail, sizeof(detail), "at %.1f,%.1f — %.0f/%.0f blocks from the X/Z border",
                  p.x, p.z, marginX, marginZ);
    check(marginX > 64.0f && marginZ > 64.0f, what, detail);
}

// Rise until nothing can be in the way. Shared by --input-selftest and --gamepad-selftest.
// A player standing INSIDE a hill cannot walk, which is a correct engine behaviour that reads as
// "input is broken" from here — so ascend until y stops rising (the world ceiling, or a slab
// overhead) rather than for a fixed count. Uses the real key path so it still exercises the
// translator.
//
// THIS IS NOT THE FIX FOR THE "moved 0.00 blocks" FLAKE, though two sessions believed it was and
// this comment used to say so. That was the map-border clamp; see teleport_for_movement() above.
// The tell those sessions had and did not read: a player pressed against rock still drifts a
// fraction, and the flake reported EXACTLY zero. Clearing the terrain is still worth doing — it is
// why the check can use a 0.5-block threshold instead of arguing about scenery — but it is
// housekeeping, not the cure.
void rise_until_clear(int maxFrames) {
    if (!eden_get_fly_mode()) {
        push_key(SDL_SCANCODE_V, true);
        push_key(SDL_SCANCODE_V, false);
        tick(10);
    }
    // ...but NOT past the top of the world. Fly mode has no ceiling of its own, so "rise until it
    // stops" can park the player above the block array entirely (y 93 in a 64-tall world was
    // reached here), and horizontal travel is much slower up there — MEASURED, not explained:
    // 0.8 blocks in the same 300 frames that cover 44 lower down, with the same axes reaching
    // eden_set_move_input either way. Worth knowing if a future session sees "the player barely
    // moves" while flying high; it is not a state normal play reaches, so the harness just stays
    // out of it rather than this being chased here.
    const float ceiling = (float)g_opt.height - 8.0f;
    push_key(SDL_SCANCODE_SPACE, true);
    float lastY = player_state().y;
    int spent = 0;
    while (spent < maxFrames) {
        tick(60);
        spent += 60;
        const float y = player_state().y;
        if (y >= ceiling) break;           // high enough; do not leave the world
        if (y - lastY < 0.05f) break;      // stopped climbing: something solid overhead
        lastY = y;
    }
    push_key(SDL_SCANCODE_SPACE, false);
    tick(60);
}

// ---------------------------------------------------------------------------------------------
// --gamepad-selftest
// ---------------------------------------------------------------------------------------------
// Drives a REAL SDL virtual gamepad rather than calling the translator's internals, so the state
// travels the same path a physical controller's does — SDL's own joystick layer, the gamepad
// mapping database, SDL_GetGamepadAxis/Button — and then asserts the ENGINE moved. Same contract
// as --input-selftest, and the native counterpart of web/tools/headless-gamepad-test.js.
//
// SDL_AttachVirtualJoystick + the SDL_GAMEPAD_* masks is what makes SDL treat it as a mapped
// gamepad; without the masks it arrives as a bare joystick and SDL_IsGamepad() is false, which is
// exactly the "unknown layout" case the translator (and eden-gamepad.js before it) refuses to
// guess at.
int run_gamepad_selftest() {
    g_selftestTag = "eden-gamepad";
    g_tickInput = true;

    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    // THE MASKS AND THE COUNTS MUST AGREE, AND CONTIGUOUSLY. SDL builds the virtual pad's mapping
    // by walking the mask and assigning joystick indices IN ORDER, so a mask with a hole in it
    // (e.g. omitting GUIDE) shifts every later button: SDL_SetJoystickVirtualButton(…,
    // RIGHT_SHOULDER) would then set the joystick button that the mapping calls DPAD_UP, and the
    // test would report a dead shoulder button with nothing else wrong. Bits 0..DPAD_RIGHT is the
    // whole d-pad-and-face block with no gaps, so index == SDL_GamepadButton value throughout.
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_DPAD_RIGHT + 1;
    desc.button_mask = (Uint32)((1u << (SDL_GAMEPAD_BUTTON_DPAD_RIGHT + 1)) - 1u);
    desc.axis_mask = (Uint32)((1u << SDL_GAMEPAD_AXIS_COUNT) - 1u);
    desc.name = "Eden virtual pad";

    const SDL_JoystickID vid = SDL_AttachVirtualJoystick(&desc);
    if (!vid) {
        std::fprintf(stderr, "[eden-gamepad] SDL_AttachVirtualJoystick failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Joystick* vj = SDL_OpenJoystick(vid);
    if (!vj) {
        std::fprintf(stderr, "[eden-gamepad] SDL_OpenJoystick failed: %s\n", SDL_GetError());
        return 1;
    }
    check(SDL_IsGamepad(vid), "SDL maps the virtual pad as a gamepad", SDL_GetGamepadNameForID(vid));

    const char* name = g_opt.world.empty() ? "gamepad-selftest" : g_opt.world.c_str();
    if (!open_world(name, g_opt.height)) return 1;
    teleport_for_movement();   // the map CENTRE, not --at's corner-clamped default
    tick(240);

    char detail[200];

    // Clear the terrain first — see rise_until_clear(). A buried or hill-blocked player who
    // cannot walk reads as "the stick is not wired", and that is what this used to report.
    rise_until_clear(1800);
    check(eden_get_fly_mode() != 0, "fly mode is on for the movement checks", "");
    check_clear_of_map_border("the player is clear of the map border");

    // --- left stick moves ---
    // Retried once, for the one reason that survives teleport_for_movement(): the direction the
    // spawn happens to face may be into a hill. (The retry was ALSO credited with covering "the
    // virtual axis reads 0 on the first batch of ticks". It never did anything of the sort — the
    // axis was always arriving, and per-frame dumps show g_padForward at a clean 1.000 through
    // every one of the 0.00-block runs. Kept for the hill; the assertion and threshold are
    // unchanged.)
    {
        auto push_stick = [&](float* outDist) {
            PlayerState before = player_state();
            SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_LEFTY, -32767); // full forward (Y is +down)
            tick(300);
            SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_LEFTY, 0);
            tick(10);
            PlayerState after = player_state();
            const float dx = after.x - before.x, dz = after.z - before.z;
            *outDist = std::sqrt(dx * dx + dz * dz);
            return after;
        };

        float dist = 0.0f;
        PlayerState after = push_stick(&dist);
        if (dist <= 0.5f) {
            SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_RIGHTX, 32767);
            tick(120);                                  // roughly half a turn
            SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_RIGHTX, 0);
            rise_until_clear(600);
            after = push_stick(&dist);
        }
        std::snprintf(detail, sizeof(detail), "moved %.2f blocks (-> %.1f,%.1f,%.1f yaw %.0f)",
                      dist, after.x, after.y, after.z, after.yaw);
        check(dist > 0.5f, "left stick moves the player", detail);
    }

    // --- right stick looks ---
    {
        PlayerState before = player_state();
        SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_RIGHTX, 32767);
        tick(60);
        SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_RIGHTX, 0);
        tick(10);
        PlayerState after = player_state();
        std::snprintf(detail, sizeof(detail), "yaw %.2f -> %.2f", before.yaw, after.yaw);
        check(after.yaw != before.yaw, "right stick turns the camera", detail);
    }

    // --- deadzone: a stick inside it must produce NOTHING ---
    // The one assertion that would fail silently in the other direction: a drifting stick that
    // never rests at exactly zero turns the camera on its own forever, which is the single most
    // common controller complaint there is.
    {
        PlayerState before = player_state();
        SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_RIGHTX, (Sint16)(32767 * 0.10f));
        tick(120);
        SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_RIGHTX, 0);
        tick(10);
        PlayerState after = player_state();
        std::snprintf(detail, sizeof(detail), "0.10 tilt vs %.2f deadzone: yaw %.2f -> %.2f",
                      eden_gamepad_deadzone, before.yaw, after.yaw);
        check(after.yaw == before.yaw, "inside the deadzone nothing moves", detail);
    }

    // --- shoulder scrolls the hotbar ---
    {
        PlayerState before = player_state();
        SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, true);
        tick(20);
        SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, false);
        tick(20);
        PlayerState after = player_state();
        std::snprintf(detail, sizeof(detail), "blocktype %d -> %d", before.blocktype, after.blocktype);
        check(after.blocktype != before.blocktype, "shoulder scrolls the hotbar", detail);
    }

    // --- Start opens the in-game menu (the HUD-tap path, same rects the keyboard uses) ---
    {
        const int before = eden_hud_in_menu();
        SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_START, true);
        tick(10);
        SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_START, false);
        tick(30);
        const int after = eden_hud_in_menu();
        std::snprintf(detail, sizeof(detail), "in_menu %d -> %d", before, after);
        check(after != before, "Start opens the in-game menu", detail);
    }

    SDL_CloseJoystick(vj);
    SDL_DetachVirtualJoystick(vid);
    std::printf("[eden-gamepad] %s (%d failure(s))\n",
                g_selftestFailures ? "FAILURES" : "ALL PASS", g_selftestFailures);
    return g_selftestFailures ? 1 : 0;
}

// ---------------------------------------------------------------------------------------------
// --keybind-selftest  (Phase N Stage 5.2/5.3)
// ---------------------------------------------------------------------------------------------
// Stage 5.2 moved the keybind map out of `public/eden-keybinds.js` and into the shared C model
// (web/src/seam/Settings_web.mm). The thing that has to be true afterwards is not "the setter
// stored a number" — it is that the INPUT PATH reads the model, i.e. that rebinding moves the
// behaviour off the old key and onto the new one. So this asserts BOTH directions: the new key
// works AND the old one has stopped. A test that only checked the new key would pass against an
// Input_native.cpp that still had its hard-coded `switch (sc)` and merely also consulted the
// model, which is exactly the half-ported state this stage could have landed in.
//
// It also asserts the double-binding case, because "one key, two actions" is shipped
// configuration (Ctrl is flyDown and crouch) and the iterator that honours it
// (eden_keybind_action_after) is new code with exactly one caller.
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

// ---------------------------------------------------------------------------------------------
// --ui-selftest  (Stage 5.4 widgets + N.4.5 text entry and world rename)
// ---------------------------------------------------------------------------------------------
// Drives the REAL screens with the inputs a player would produce — pointer events at the rects
// the screen itself reports (SettingsMenu::showRow), SDL text/key events pushed through the pump
// — and asserts on the model and the disk, never on the widgets' own state. Headless: none of
// this needs a pixel, and --shot is the artefact for what it looks like.
extern "C" {
float eden_settings_get(int i);
int   eden_settings_kind(int i);
int   eden_settings_enum_count(int i);
void  eden_input_pointer_event(int phase, int identity, float x, float y);
}
int settings_index(const char* key) {
    for (int i = 0; i < eden_settings_count(); ++i)
        if (std::strcmp(eden_settings_key(i), key) == 0) return i;
    return -1;
}

// Engine touches are y-UP point space; eden_input_pointer_event takes y-DOWN (window order).
const int kUiPointer = 4242;
void ui_pointer(int phase, float x, float yUp) {
    eden_input_pointer_event(phase, kUiPointer, x, SCREEN_HEIGHT - yUp);
}
void ui_tap(float x, float yUp) {
    ui_pointer(0, x, yUp); tick(2);
    ui_pointer(2, x, yUp); tick(2);
}

void push_text(const char* utf8) {          // must be a literal: SDL keeps the pointer
    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_EVENT_TEXT_INPUT;
    e.text.text = utf8;
    SDL_PushEvent(&e);
}
void push_tap_key(SDL_Scancode sc) { push_key(sc, true); push_key(sc, false); }

int  g_promptChosen = -2;
std::string g_promptText;
void ui_prompt_cb(int chosen, const char* text) { g_promptChosen = chosen; g_promptText = text ? text : ""; }

bool read_file(const std::string& path, std::vector<unsigned char>* out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    out->clear();
    unsigned char buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out->insert(out->end(), buf, buf + n);
    std::fclose(f);
    return true;
}

void ui_tap_rect(CGRect r) { ui_tap(r.origin.x + r.size.width * 0.5f, r.origin.y + r.size.height * 0.5f); }

void push_wheel(float y) {
    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_EVENT_MOUSE_WHEEL;
    e.wheel.y = y;
    e.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
    SDL_PushEvent(&e);
}

// Stage 5.6: the kit main menu, driven only through taps at the rects the screen reports (and one
// wheel event), asserting on the menu's model — the list, the selection, `loading`, the disk.
// Works on whatever list this machine already has: it adds its own worlds and only deletes those.
int run_menu_selftest() {
    char detail[240];
    Menu* m = World::getWorld->menu;
    tick(5);
    const int n0 = eden_menu_world_count();

    // New: one more world, selected, scrolled into view.
    ui_tap_rect(m->controlRect("new"));
    tick(3);
    int sel = eden_menu_selected_index();
    CGRect rr;
    std::snprintf(detail, sizeof(detail), "count %d -> %d, selected %d, first %d of %d visible",
                  n0, eden_menu_world_count(), sel, m->firstVisibleRow(), m->visibleRows());
    check(eden_menu_world_count() == n0 + 1 && sel == n0 && m->rowRect(sel, &rr),
          "New adds a world, selects it, and scrolls it into view", detail);
    // Enough worlds that the list must scroll.
    for (int guard = 0; eden_menu_world_count() < m->visibleRows() + 3 && guard < 40; ++guard) {
        ui_tap_rect(m->controlRect("new"));
        tick(3);
    }
    const int total = eden_menu_world_count();
    const int vis = m->visibleRows();
    check(total > vis, "the list is longer than the window (it has to scroll)", "");

    // A tap on a row that is not selected selects it — and does not play it.
    sel = eden_menu_selected_index();
    int pick = m->firstVisibleRow();
    if (pick == sel) pick++;
    if (m->rowRect(pick, &rr)) ui_tap_rect(rr);
    tick(3);
    std::snprintf(detail, sizeof(detail), "tapped %d: selected %d loading %d", pick, eden_menu_selected_index(), m->loading);
    check(eden_menu_selected_index() == pick && m->loading == 0, "tapping a row selects it without playing", detail);

    // Drag the list down two rows: earlier rows come into view, and the drag selects nothing.
    {
        sel = eden_menu_selected_index();
        m->rowRect(m->firstVisibleRow(), &rr);
        const float pitch = rr.size.height;
        eden_menu_select(total - 1);                    // scrolls to the end, so there is room above
        tick(2);
        sel = total - 1;
        const int f0 = m->firstVisibleRow();
        const CGRect L = m->controlRect("list");
        const float x = L.origin.x + L.size.width * 0.5f, y0 = L.origin.y + L.size.height * 0.5f;
        ui_pointer(0, x, y0); tick(2);
        ui_pointer(1, x, y0 - pitch); tick(2);
        ui_pointer(1, x, y0 - 2.0f * pitch); tick(2);
        ui_pointer(2, x, y0 - 2.0f * pitch); tick(2);
        const int f1 = m->firstVisibleRow();
        std::snprintf(detail, sizeof(detail), "first %d -> %d, selected %d (was %d), loading %d",
                      f0, f1, eden_menu_selected_index(), sel, m->loading);
        check(f1 == f0 - 2 && eden_menu_selected_index() == sel && m->loading == 0,
              "dragging the list scrolls by whole rows and selects nothing", detail);

        // The wheel: one notch toward the user is one row later.
        push_wheel(-1.0f);
        tick(3);
        std::snprintf(detail, sizeof(detail), "first %d -> %d", f1, m->firstVisibleRow());
        check(m->firstVisibleRow() == f1 + 1, "a wheel notch scrolls one row", detail);

        // The scrollbar: a tap on the track above the thumb pages up.
        const int f2 = m->firstVisibleRow();
        const CGRect B = m->controlRect("scrollbar");
        ui_tap(B.origin.x + B.size.width * 0.5f, B.origin.y + B.size.height - 2.0f);
        const int want = std::max(0, f2 - vis);
        std::snprintf(detail, sizeof(detail), "first %d -> %d (want %d)", f2, m->firstVisibleRow(), want);
        check(m->firstVisibleRow() == want, "a tap on the scrollbar track pages", detail);
    }

    // Delete: Cancel keeps the world, Delete removes it (the dialog was a no-op before 5.6).
    {
        eden_menu_select(total - 1);
        tick(2);
        const std::string victim = eden_menu_world_name(total - 1);
        ui_tap_rect(m->controlRect("delete"));
        tick(3);
        CGRect b;
        const bool up = GLDialog::active() && GLDialog::buttonRect(1, &b);
        check(up, "Delete asks first", "");
        if (up) ui_tap_rect(b);
        tick(3);
        std::snprintf(detail, sizeof(detail), "count %d (was %d), dialog %d", eden_menu_world_count(), total,
                      GLDialog::active() ? 1 : 0);
        check(eden_menu_world_count() == total && !GLDialog::active(), "Cancel keeps the world", detail);
        ui_tap_rect(m->controlRect("delete"));
        tick(3);
        if (GLDialog::buttonRect(0, &b)) ui_tap_rect(b);
        tick(3);
        bool listed = false;
        for (int i = 0; i < eden_menu_world_count(); ++i) listed |= (victim == eden_menu_world_name(i));
        std::snprintf(detail, sizeof(detail), "count %d (was %d), \"%s\" listed %d", eden_menu_world_count(), total,
                      victim.c_str(), listed ? 1 : 0);
        check(eden_menu_world_count() == total - 1 && !listed, "Delete -> Delete removes the world", detail);
    }

    // Play: the button, then the stock world-type + height dialogs (Flat, Classic), to a world.
    {
        const int idx = eden_menu_world_count() - 1;
        eden_menu_select(idx);
        tick(2);
        const std::string name = eden_menu_world_name(idx);
        ui_tap_rect(m->controlRect("play"));
        CGRect b;
        const bool asked = tick_until([] { return GLDialog::active(); }, 600, "world-type dialog");
        if (asked && GLDialog::buttonRect(0, &b)) ui_tap_rect(b);            // Flat
        tick(3);
        if (GLDialog::active() && GLDialog::buttonRect(0, &b)) ui_tap_rect(b); // Classic 64
        const bool played = tick_until([] { return game_mode() == 1; }, 60000, "game_mode == PLAY");
        check(asked && played, "Play -> Flat -> Classic loads the selected world", name.c_str());
        if (!played) return 1;
        tick(60);
        if (!quit_to_menu()) return 1;
        tick(30);

        // Now it has a file: Delete must remove that too.
        int at = -1;
        for (int i = 0; i < eden_menu_world_count(); ++i) if (name == eden_menu_world_name(i)) at = i;
        check(at >= 0, "the played world is listed after quitting", name.c_str());
        if (at < 0) return 1;
        const std::string file = std::string(eden_platform_documents_root()) + "/" + eden_menu_world_file(at);
        std::vector<unsigned char> bytes;
        const bool had = read_file(file, &bytes);
        eden_menu_select(at);
        tick(2);
        ui_tap_rect(m->controlRect("delete"));
        tick(3);
        if (GLDialog::buttonRect(0, &b)) ui_tap_rect(b);
        tick(3);
        const bool gone = !read_file(file, &bytes);
        std::snprintf(detail, sizeof(detail), "%s: before %d after %d", file.c_str(), had ? 1 : 0, gone ? 0 : 1);
        check(had && gone, "deleting a played world removes its file", detail);
    }
    return 0;
}

// Stage 5.7: the pickers kept stock's hit path; prove a tap on a cell still picks, through the
// HUD's own rects, and that the current choice is what a build would use.
int run_picker_selftest() {
    char detail[200];
    Hud* hud = World::getWorld->hud;
    tap_hud(1);
    tick(5);
    check(hud->mode == MODE_PICK_BLOCK, "E's HUD button opens the block picker", "");
    // Cells 2 and 3 are dark stone and stone in the stock table: plain blocks, no special pick.
    const CGRect c0 = hud->blockBounds[2];
    ui_tap_rect(c0);
    tick(5);
    std::snprintf(detail, sizeof(detail), "mode %d blocktype %d", hud->mode, hud->blocktype);
    check(hud->mode == MODE_BUILD, "tapping a block cell picks it and closes the picker", detail);
    const int picked = hud->blocktype;
    tap_hud(1);
    tick(5);
    ui_tap_rect(hud->blockBounds[3]);
    tick(5);
    std::snprintf(detail, sizeof(detail), "blocktype %d -> %d", picked, hud->blocktype);
    check(hud->mode == MODE_BUILD && hud->blocktype != picked, "a different cell picks a different block", detail);

    tap_hud(2);
    tick(5);
    check(hud->mode == MODE_PICK_COLOR, "C's HUD button opens the colour picker", "");
    ui_tap_rect(hud->colorBounds[10]);
    tick(5);
    std::snprintf(detail, sizeof(detail), "mode %d paintColor %d", hud->mode, (int)hud->paintColor);
    check(hud->mode == MODE_PAINT && (int)hud->paintColor == 11, "tapping swatch 10 paints colour 11", detail);
    return 0;
}

int run_ui_selftest() {
    g_selftestTag = "eden-ui";
    g_tickInput = true;            // headless pump_events() only drains SDL's queue with this set
    char detail[240];
    tick(120);
    check(eden_settings_loaded() != 0, "the settings model loaded", "");

    // --- the slider's arithmetic, on its own --------------------------------------------------
    {
        GLW::Slider sl;
        sl.setRect(CGRectMake(100, 100, 200, 40));
        sl.setRange(0.25f, 3.0f, 0.05f);
        sl.setValue(1.0f);
        const bool moved = sl.beginDrag(100.0f);
        const float lo = sl.value();
        const bool again = sl.dragTo(90.0f);           // already pinned at min: no change
        sl.dragTo(300.0f);
        const float hi = sl.value();
        sl.dragTo(173.0f);
        const float mid = sl.value();
        sl.endDrag();
        const float steps = (mid - 0.25f) / 0.05f;
        std::snprintf(detail, sizeof(detail), "lo %.3f hi %.3f mid %.4f (%.3f steps) moved=%d again=%d",
                      lo, hi, mid, steps, moved, again);
        check(moved && !again && std::fabs(lo - 0.25f) < 1e-4f && std::fabs(hi - 3.0f) < 1e-4f &&
              std::fabs(steps - std::floor(steps + 0.5f)) < 1e-3f,
              "Slider pins to its ends, snaps to min+k*step, reports only real changes", detail);
    }

    // --- the settings screen, through real pointer events -------------------------------------
    eden_settings_menu_open_now();
    tick(10);
    SettingsMenu* sm = (World::getWorld && World::getWorld->menu) ? World::getWorld->menu->settings : nullptr;
    if (!sm) { check(false, "the settings screen exists", ""); return 1; }
    std::snprintf(detail, sizeof(detail), "%d pages", sm->pageCount());
    check(sm->pageCount() > 1, "settings paginate", detail);

    const int iHealth = settings_index("health");
    const int iVol    = settings_index("music_volume");
    // The first ENUM row this build shows (input_mode, the obvious one, is web-only).
    const char* modeKey = nullptr;
    for (int i = 0; i < eden_settings_count() && !modeKey; ++i)
        if (eden_settings_kind(i) == 2 && sm->showRow(eden_settings_key(i), nullptr)) modeKey = eden_settings_key(i);
    const int iMode   = modeKey ? settings_index(modeKey) : -1;
    const float health0 = eden_settings_get(iHealth), vol0 = eden_settings_get(iVol);
    const float mode0 = iMode >= 0 ? eden_settings_get(iMode) : 0.0f;
    CGRect r;

    if (sm->showRow("health", &r)) {
        tick(2);
        ui_tap(r.origin.x + r.size.width * 0.5f, r.origin.y + r.size.height * 0.5f);
        const float after = eden_settings_get(iHealth);
        ui_tap(r.origin.x + r.size.width * 0.5f, r.origin.y + r.size.height * 0.5f);
        std::snprintf(detail, sizeof(detail), "%.0f -> %.0f -> %.0f", health0, after, eden_settings_get(iHealth));
        check(after != health0 && eden_settings_get(iHealth) == health0, "tapping a Toggle flips its setting, twice restores", detail);
    } else check(false, "the health row is shown", "");

    if (sm->showRow("music_volume", &r)) {
        tick(2);
        const float cy = r.origin.y + r.size.height * 0.5f;
        ui_pointer(0, r.origin.x + 1.0f, cy); tick(3);
        const float atLeft = eden_settings_get(iVol);
        ui_pointer(1, r.origin.x + r.size.width - 1.0f, cy + 60.0f); tick(3);   // off the track, vertically
        const float atRight = eden_settings_get(iVol);
        ui_pointer(1, r.origin.x + r.size.width * 0.5f, cy); tick(3);
        const float atMid = eden_settings_get(iVol);
        ui_pointer(2, r.origin.x + r.size.width * 0.5f, cy); tick(3);
        std::snprintf(detail, sizeof(detail), "left %.2f right %.2f mid %.2f (was %.2f)", atLeft, atRight, atMid, vol0);
        check(atLeft == 0.0f && atRight == 1.0f && std::fabs(atMid - 0.5f) <= 0.051f,
              "dragging a Slider writes the setting live, and keeps tracking off the track", detail);
    } else check(false, "the music volume row is shown", "");

    if (modeKey && sm->showRow(modeKey, &r)) {
        tick(2);
        const float h = r.size.height;    // the stepper's buttons are square at its height
        ui_tap(r.origin.x + r.size.width - h * 0.5f, r.origin.y + h * 0.5f);
        const int n = eden_settings_enum_count(iMode);
        const int want = ((int)mode0 + 1) % (n > 0 ? n : 1);
        std::snprintf(detail, sizeof(detail), "%s: %.0f -> %.0f of %d", modeKey, mode0, eden_settings_get(iMode), n);
        check((int)eden_settings_get(iMode) == want, "a Stepper's > steps an enum row", detail);
    } else check(false, "an enum row is shown", "");

    eden_settings_set(iHealth, health0);
    eden_settings_set(iVol, vol0);
    if (iMode >= 0) eden_settings_set(iMode, mode0);
    eden_settings_menu_close();
    tick(10);

    if (run_menu_selftest() != 0) return 1;

    // --- the text field: typing, UTF-8 backspace, the byte cap, Return, Escape -----------------
    {
        static const char* const kBtn[] = { "OK", "Cancel" };
        check(GLDialog::textEntryAvailable(), "native reports GL text entry", "");
        g_promptChosen = -2;
        GLDialog::prompt("Selftest", NULL, "abc", 8, kBtn, 2, ui_prompt_cb);
        tick(2);
        check(eden_text_input_active() == 1, "focusing the field starts platform text input", "");
        push_text("d\xC3\xA9");             // "dé": é is two bytes
        tick(2);
        push_tap_key(SDL_SCANCODE_BACKSPACE);
        tick(2);
        push_text("xyz12345");
        push_tap_key(SDL_SCANCODE_W);       // must reach the field's queue (nothing), not the game
        tick(2);
        push_tap_key(SDL_SCANCODE_RETURN);
        tick(3);
        std::snprintf(detail, sizeof(detail), "chosen %d text \"%s\" active %d dialog %d",
                      g_promptChosen, g_promptText.c_str(), eden_text_input_active(), GLDialog::active() ? 1 : 0);
        check(g_promptChosen == 0 && g_promptText == "abcdxyz1" && eden_text_input_active() == 0 && !GLDialog::active(),
              "type + backspace (a whole UTF-8 char) + 8-byte cap + Return commits", detail);

        push_text("late");
        tick(2);
        char sink[16];
        check(eden_text_input_take(sink, sizeof(sink)) == 0, "text typed with no field focused is dropped", "");

        g_promptChosen = -2;
        GLDialog::prompt("Selftest", NULL, "keep", 8, kBtn, 2, ui_prompt_cb);
        tick(2);
        push_text("zz");
        push_tap_key(SDL_SCANCODE_ESCAPE);
        tick(3);
        std::snprintf(detail, sizeof(detail), "chosen %d text \"%s\" in_menu %d", g_promptChosen,
                      g_promptText.c_str(), eden_hud_in_menu());
        check(g_promptChosen == 1 && !GLDialog::active(), "Escape picks the LAST button (Cancel) and closes", detail);
    }

    // --- rename a world that exists on disk -------------------------------------------------
    // Play it once so there is a file (a created-but-never-played world has none), go back to the
    // menu, rename through the real button -> prompt -> renameSelected path, then check the file:
    // ONLY WorldFileHeader::name may differ, and the world must still load under its new name.
    const char* name = g_opt.world.empty() ? "ui-selftest" : g_opt.world.c_str();
    if (!open_world(name, g_opt.height)) return 1;
    tick(60);
    if (!quit_to_menu()) return 1;
    tick(30);
    int idx = -1;
    for (int i = 0; i < eden_menu_world_count(); ++i)
        if (std::strcmp(eden_menu_world_name(i), name) == 0) idx = i;
    check(idx >= 0, "the played world is listed", name);
    if (idx < 0) return 1;
    eden_menu_select(idx);
    tick(5);
    const std::string file = std::string(eden_platform_documents_root()) + "/" + eden_menu_world_file(idx);
    std::vector<unsigned char> before, after;
    check(read_file(file, &before) && before.size() > sizeof(WorldFileHeader), "the world has a file", file.c_str());

    Menu* menu = World::getWorld->menu;
    const bool offered = menu->renameOffered();
    check(offered, "the menu offers Rename for the selected world", "");
    const CGRect rb = menu->rect_rename.rect();
    ui_tap(rb.origin.x + rb.size.width * 0.5f, rb.origin.y + rb.size.height * 0.5f);
    tick(2);
    check(GLDialog::active() && eden_text_input_active() == 1, "the Rename button opens the prompt, focused", "");
    for (int i = 0; i < 64; ++i) push_tap_key(SDL_SCANCODE_BACKSPACE);
    tick(2);
    push_text("Renamed \xC3\x9C");          // "Renamed Ü"
    tick(2);
    push_tap_key(SDL_SCANCODE_RETURN);
    tick(5);
    const char* want = "Renamed \xC3\x9C";
    const std::string listed = eden_menu_world_name(idx);   // copy: one static buffer per accessor family
    std::string onDisk = cpstring(World::getWorld->fm->getName([NSString stringWithUTF8String:eden_menu_world_file(idx)]));
    std::snprintf(detail, sizeof(detail), "listed \"%s\" on disk \"%s\"", listed.c_str(), onDisk.c_str());
    check(listed == want && onDisk == want, "the rename reaches the list and the file header", detail);

    read_file(file, &after);
    size_t diffOutside = 0, diffInside = 0;
    const size_t n0 = offsetof(WorldFileHeader, name), n1 = n0 + sizeof(((WorldFileHeader*)0)->name);
    for (size_t i = 0; i < before.size() && i < after.size(); ++i) {
        if (before[i] == after[i]) continue;
        if (i >= n0 && i < n1) diffInside++; else diffOutside++;
    }
    std::snprintf(detail, sizeof(detail), "size %zu -> %zu, %zu name bytes changed, %zu other bytes changed",
                  before.size(), after.size(), diffInside, diffOutside);
    check(before.size() == after.size() && diffInside > 0 && diffOutside == 0,
          "only the header's name bytes changed on disk", detail);

    const bool reopened = open_world(want, g_opt.height);
    check(reopened, "the renamed world loads under its new name", "");

    // --- Stage 5.5: the kit pause menu --------------------------------------------------------
    // Real taps at the rects the Hud laid out: Settings opens the GL SettingsMenu as an in-game
    // modal, its own Back returns to the pause menu (not to gameplay, not to the title screen),
    // and Resume closes the menu.
    if (reopened) {
        Hud* hud = World::getWorld->hud;
        tick(30);
        if (!eden_hud_in_menu()) tap_hud(0);
        tick(10);
        check(eden_hud_in_menu() == 1, "Escape's HUD button opens the pause menu", "");
        const Button rs = hud->rsettings;
        ui_tap(rs.origin.x + rs.size.width * 0.5f, rs.origin.y + rs.size.height * 0.5f);
        tick(5);
        std::snprintf(detail, sizeof(detail), "showsettings=%d game_mode=%d", (int)menu->showsettings,
                      World::getWorld->game_mode);
        check(menu->showsettings && World::getWorld->game_mode == GAME_MODE_PLAY,
              "pause menu -> Settings opens the settings screen over the world", detail);
        const CGRect bk = menu->settings->backRect();
        ui_tap(bk.origin.x + bk.size.width * 0.5f, bk.origin.y + bk.size.height * 0.5f);
        tick(5);
        std::snprintf(detail, sizeof(detail), "showsettings=%d in_menu=%d", (int)menu->showsettings,
                      eden_hud_in_menu());
        check(!menu->showsettings && eden_hud_in_menu() == 1, "Settings' Back returns to the pause menu", detail);
        const Button rr = hud->rresume;
        ui_tap(rr.origin.x + rr.size.width * 0.5f, rr.origin.y + rr.size.height * 0.5f);
        tick(5);
        check(eden_hud_in_menu() == 0, "Resume closes the pause menu", "");
        run_picker_selftest();
        quit_to_menu();
    }

    std::printf("[eden-ui] %s (%d failure(s))\n", g_selftestFailures ? "FAILURES" : "ALL PASS", g_selftestFailures);
    return g_selftestFailures ? 1 : 0;
}

// ---------------------------------------------------------------------------------------------
// --browser-selftest  (ROADMAP 5.9: Get Worlds — the archive, the current and the legacy server)
// ---------------------------------------------------------------------------------------------
// OFFLINE, and that is the point: the CI runners must not depend on edengame.net being up, so
// Net_native.cpp's fixture mode answers every URL from files this test writes first —
// `<docs>/.net-fixtures/<host>/<path>@<query>`. Everything above the HTTP stack is the real thing:
// the menu's button, the browser's tabs/rows/field/buttons through pointer and SDL text events,
// the list parsers, the per-frame unpacker (a gzip, a deflate zip, a stored zip around a deflate
// zip), the import, and finally loading a downloaded world. The payload is a world this test plays
// and saves first, so "the download is a world" is checked byte-for-byte and by loading it.
//
// The legacy server gets NO fixtures on purpose: that is the "server unreachable" path.
// --net-live-selftest below runs the same screen against the real servers (not a CI gate).
extern "C" void eden_net_set_fixture_root(const char* dir);
void mkdirs(const std::string& path);

namespace {
std::vector<std::string> g_fixtureFiles;

bool write_bytes(const std::string& path, const std::vector<unsigned char>& b) {
    const size_t slash = path.find_last_of('/');
    if (slash != std::string::npos) mkdirs(path.substr(0, slash));
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const bool ok = b.empty() || std::fwrite(b.data(), 1, b.size(), f) == b.size();
    std::fclose(f);
    g_fixtureFiles.push_back(path);
    return ok;
}
std::vector<unsigned char> bytes_of(const std::string& s) { return std::vector<unsigned char>(s.begin(), s.end()); }

std::vector<unsigned char> deflate_bytes(const std::vector<unsigned char>& in, int windowBits) {
    z_stream z;
    std::memset(&z, 0, sizeof(z));
    deflateInit2(&z, 6, Z_DEFLATED, windowBits, 8, Z_DEFAULT_STRATEGY);
    std::vector<unsigned char> out(deflateBound(&z, (uLong)in.size()) + 64);
    z.next_in = (Bytef*)in.data();
    z.avail_in = (uInt)in.size();
    z.next_out = out.data();
    z.avail_out = (uInt)out.size();
    deflate(&z, Z_FINISH);
    out.resize(z.total_out);
    deflateEnd(&z);
    return out;
}
std::vector<unsigned char> gzip_bytes(const std::vector<unsigned char>& in) { return deflate_bytes(in, 15 + 16); }

// A single-entry PK zip, the shape the archive serves: local header, data, central directory, EOCD.
// `streamed` sets general-purpose bit 3 and ZEROES the local header's sizes, as a streaming
// archiver does — the case that makes a reader trust the central directory.
std::vector<unsigned char> zip_bytes(const char* name, const std::vector<unsigned char>& data, bool deflate,
                                     bool streamed) {
    const std::vector<unsigned char> body = deflate ? deflate_bytes(data, -15) : data;
    const unsigned crc = (unsigned)crc32(0, data.data(), (uInt)data.size());
    const unsigned nlen = (unsigned)std::strlen(name);
    std::vector<unsigned char> z;
    auto u16 = [&](unsigned v) { z.push_back(v & 0xff); z.push_back((v >> 8) & 0xff); };
    auto u32 = [&](unsigned v) { u16(v & 0xffff); u16(v >> 16); };
    const unsigned method = deflate ? 8 : 0, flags = streamed ? 8 : 0;
    u32(0x04034b50); u16(20); u16(flags); u16(method); u16(0); u16(0);
    u32(streamed ? 0 : crc); u32(streamed ? 0 : (unsigned)body.size()); u32(streamed ? 0 : (unsigned)data.size());
    u16(nlen); u16(0);
    z.insert(z.end(), name, name + nlen);
    z.insert(z.end(), body.begin(), body.end());
    if (streamed) { u32(0x08074b50); u32(crc); u32((unsigned)body.size()); u32((unsigned)data.size()); }
    const unsigned cdOff = (unsigned)z.size();
    u32(0x02014b50); u16(20); u16(20); u16(flags); u16(method); u16(0); u16(0);
    u32(crc); u32((unsigned)body.size()); u32((unsigned)data.size());
    u16(nlen); u16(0); u16(0); u16(0); u16(0); u32(0); u32(0);
    z.insert(z.end(), name, name + nlen);
    const unsigned cdSize = (unsigned)z.size() - cdOff;
    u32(0x06054b50); u16(0); u16(0); u16(1); u16(1); u32(cdSize); u32(cdOff); u16(0);
    return z;
}

int menu_index_of_file(const char* file) {
    for (int i = 0; i < eden_menu_world_count(); ++i) if (!std::strcmp(eden_menu_world_file(i), file)) return i;
    return -1;
}

bool browser_wait_list(WorldBrowser* b) {
    return tick_until([b] { return !b->listLoading(); }, 3000, "the browser's list");
}

// Selects the row whose id is `id` (scrolled into view first) and taps
// Download, then waits for the browser to hand back to the menu or to report a failure.
bool browser_download(WorldBrowser* b, Menu* m, const char* id) {
    int at = -1;
    for (int i = 0; i < b->entryCount(); ++i) if (!std::strcmp(b->entryId(i), id)) at = i;
    CGRect r;
    if (at >= 0) b->showRow(at);
    if (at < 0 || !b->rowRect(at, &r)) return false;
    ui_tap_rect(r);
    tick(2);
    if (b->selectedIndex() != at) return false;
    ui_tap_rect(b->controlRect("download"));
    tick(2);
    return tick_until([b, m] { return !m->showbrowser || !b->busy(); }, 6000, "the download");
}
}  // namespace

int run_browser_selftest_body(bool live) {
    char detail[300];
    Menu* m = World::getWorld->menu;
    WorldBrowser* b = m->browser;
    const std::string docs = eden_platform_documents_root();

    if (live) {
        check(WorldBrowser::available(), "this build has a network backend", "");
        if (!WorldBrowser::available()) return 1;
        ui_tap_rect(m->controlRect("getworlds"));
        tick(3);
        check(m->showbrowser && b->isOpen(), "Get Worlds opens the browser", "");
        const char* names[3] = { "archive", "current server", "legacy server" };
        for (int src = 0; src < 3; ++src) {
            ui_tap_rect(b->tabRect(src));
            tick(3);
            browser_wait_list(b);
            std::snprintf(detail, sizeof(detail), "%d worlds; status \"%s\"; first \"%s\"", b->entryCount(),
                          b->statusText(), b->entryName(0));
            char what[80];
            std::snprintf(what, sizeof(what), "the %s lists worlds", names[src]);
            check(b->entryCount() > 5, what, detail);
            if (src >= 1) {                   // Recent, the paged list, on the servers
                ui_tap_rect(b->modeRect(WorldBrowser::MODE_RECENT));
                tick(3);
                browser_wait_list(b);
                std::snprintf(detail, sizeof(detail), "%d worlds; first \"%s\" (%s)", b->entryCount(), b->entryName(0), b->entryId(0));
                std::snprintf(what, sizeof(what), "the %s's Recent list", names[src]);
                check(b->entryCount() > 5, what, detail);
            }
            if (b->entryCount() > 0) {
                CGRect r;
                if (b->rowRect(0, &r)) ui_tap_rect(r);
                tick_until([b] { return b->previewShown(); }, 4000, "a preview");
                std::printf("[eden-browser] %s preview for \"%s\": %s\n", names[src], b->entryName(0),
                            b->previewShown() ? "shown" : "none (many worlds have none)");
            }
        }
        // One real download, chosen to be SMALL: the archive is the only source that publishes
        // sizes, and the first entry of a server list can be gigabytes (on 2026-10-07 the current
        // server's first Featured world inflated to 1.88 GB). HTTPS + a zip, end to end.
        ui_tap_rect(b->tabRect(WorldBrowser::SRC_ARCHIVE));
        tick(3);
        browser_wait_list(b);
        int pick = -1;
        double best = 1e18;
        for (int i = 0; i < b->entryCount(); ++i) {
            double v = 0;
            char unit[8] = {0};
            if (std::sscanf(b->entrySize(i), "%lf %7s", &v, unit) != 2 || v <= 0) continue;
            const double bytes = v * (!std::strcmp(unit, "GB") ? 1e9 : !std::strcmp(unit, "MB") ? 1e6 : !std::strcmp(unit, "KB") ? 1e3 : 1.0);
            if (bytes < best) { best = bytes; pick = i; }
        }
        const std::string id = pick >= 0 ? b->entryId(pick) : "";
        const std::string size = pick >= 0 ? b->entrySize(pick) : "";
        const int before = eden_menu_world_count();
        const bool ok = !id.empty() && browser_download(b, m, id.c_str());
        const int at = menu_index_of_file((id + ".eden").c_str());
        std::snprintf(detail, sizeof(detail), "id %s (%s), worlds %d -> %d, status \"%s\", listed as \"%s\"", id.c_str(),
                      size.c_str(), before, eden_menu_world_count(), b->statusText(), at >= 0 ? eden_menu_world_name(at) : "-");
        check(ok && at >= 0 && !m->showbrowser, "the smallest archive world downloads, unpacks and is listed", detail);
        if (at >= 0) {
            eden_menu_select(at);
            tick(2);
            ui_tap_rect(m->controlRect("play"));
            const bool played = tick_until([] { return game_mode() == 1; }, 60000, "game_mode == PLAY");
            check(played, "the downloaded world loads", "");
            if (played) { tick(60); quit_to_menu(); tick(10); }
            const int again = menu_index_of_file((id + ".eden").c_str());
            if (again >= 0) { eden_menu_select(again); m->a_deleteConfirm(); }
        }
        if (m->showbrowser) { ui_tap_rect(b->controlRect("back")); tick(3); }
        return 0;
    }

    // --- a world to serve: play one, save it, read its bytes --------------------------------
    const char* name = g_opt.world.empty() ? "browser-selftest" : g_opt.world.c_str();
    if (!open_world(name, g_opt.height)) return 1;
    tick(60);
    if (!quit_to_menu()) return 1;
    tick(20);
    int src = -1;
    for (int i = 0; i < eden_menu_world_count(); ++i) if (!std::strcmp(eden_menu_world_name(i), name)) src = i;
    std::vector<unsigned char> world;
    const bool haveWorld = src >= 0 && read_file(docs + "/" + eden_menu_world_file(src), &world);
    check(haveWorld && world.size() > 192, "a saved world to serve", "");
    if (!haveWorld) return 1;

    // --- fixtures -----------------------------------------------------------------------------
    const std::string fx = docs + "/.net-fixtures";
    const std::string arch = fx + "/hagg3.github.io/edenarchive/assets";
    write_bytes(arch + "/data/worlds.json", bytes_of(
        "[\n"
        " {\"filename\": \"1600000001.eden\", \"worldname\": \"Selftest Alpha\", \"publishdate\": \"2020-09-13\",\n"
        "  \"archivedate\": null, \"filesize\": \"1.0 MB\", \"author\": \"Tester\", \"tags\": [\"castle\", true, 7],\n"
        "  \"url\": \"/worlds/alpha/\"},\n"
        " {\"filename\": \"1600000002.eden\", \"worldname\": \"Selftest Beta \\u00dc\", \"publishdate\": null,\n"
        "  \"archivedate\": null, \"filesize\": null, \"author\": null, \"tags\": [], \"url\": \"/worlds/beta/\"},\n"
        " {\"filename\": \"../evil.eden\", \"worldname\": \"Path traversal\", \"tags\": []}\n"
        "]\n"));
    // Alpha: a deflate zip written the streaming way (bit 3, zero sizes in the local header).
    write_bytes(arch + "/worldfiles/1600000001/1600000001.eden.zip", zip_bytes("1600000001.eden", world, true, true));
    // Beta: the "double-compressed" case the archive's own site warns about — a stored zip
    // around a deflate zip around the world.
    write_bytes(arch + "/worldfiles/1600000002/1600000002.eden.zip",
                zip_bytes("1600000002.eden.zip", zip_bytes("1600000002.eden", world, true, false), false, false));
    std::vector<unsigned char> png;
    char exe[1024] = {0};
    const char* base = SDL_GetBasePath();
    std::snprintf(exe, sizeof(exe), "%sreport_flag.png", base ? base : "");
    const bool havePng = read_file(exe, &png);
    if (havePng) write_bytes(arch + "/worldfiles/1600000001/1600000001.eden.png", png);

    const std::string app2 = fx + "/app2.edengame.net", files2 = fx + "/files2.edengame.net";
    write_bytes(files2 + "/popularlist.txt", bytes_of("1600000101.eden\nServer Alpha.name\n"));
    write_bytes(app2 + "/list2.php@start=0&sort=2", bytes_of(
        "1600000101.eden\nServer Alpha.name\n\n1600000102.eden\nBroken World.name\n"));   // a stray blank line
    write_bytes(app2 + "/list2.php@start=2&sort=2", bytes_of(
        "1600000102.eden\nBroken World.name\n1600000103.eden\nServer Gamma.name\n"));    // one repeat, one new
    write_bytes(app2 + "/list2.php@start=3&sort=2", bytes_of(""));
    write_bytes(app2 + "/list2.php@search=gamma", bytes_of("1600000103.eden\nServer Gamma.name\n"));
    write_bytes(files2 + "/1600000101.eden", gzip_bytes(world));
    write_bytes(files2 + "/1600000102.eden", bytes_of("<html><body>Not found</body></html>\n"));
    // A gzip file of two members (what `cat a.gz b.gz` makes) — still one world once inflated.
    {
        const size_t half = world.size() / 2;
        std::vector<unsigned char> a(world.begin(), world.begin() + (long)half), c(world.begin() + (long)half, world.end());
        std::vector<unsigned char> two = gzip_bytes(a), tail = gzip_bytes(c);
        two.insert(two.end(), tail.begin(), tail.end());
        write_bytes(files2 + "/1600000103.eden", two);
    }
    eden_net_set_fixture_root(fx.c_str());

    // --- the menu offers it, and it opens on the archive --------------------------------------
    tick(3);
    check(m->browserOffered(), "the menu offers Get Worlds", "");
    ui_tap_rect(m->controlRect("getworlds"));
    tick(3);
    check(m->showbrowser && b->isOpen() && b->source() == WorldBrowser::SRC_ARCHIVE,
          "Get Worlds opens the browser on the archive tab", "");
    browser_wait_list(b);
    std::snprintf(detail, sizeof(detail), "%d entries: \"%s\", \"%s\"", b->entryCount(), b->entryName(0), b->entryName(1));
    check(b->entryCount() == 2 && !std::strcmp(b->entryName(0), "Selftest Alpha") &&
          !std::strcmp(b->entryName(1), "Selftest Beta \xC3\x9C"),
          "the manifest parses (\\u escape, odd tags) and drops a bad id", detail);

    // The archive filters as you type: by name, then by a tag.
    ui_tap_rect(b->controlRect("search"));
    tick(2);
    push_text("beta");
    tick(3);
    std::snprintf(detail, sizeof(detail), "%d: \"%s\"", b->entryCount(), b->entryName(0));
    check(b->entryCount() == 1 && !std::strcmp(b->entryId(0), "1600000002"), "typing filters the archive by name", detail);
    for (int k = 0; k < 4; ++k) push_tap_key(SDL_SCANCODE_BACKSPACE);
    tick(2);
    push_text("castle");
    tick(3);
    std::snprintf(detail, sizeof(detail), "%d: \"%s\"", b->entryCount(), b->entryName(0));
    check(b->entryCount() == 1 && !std::strcmp(b->entryId(0), "1600000001"), "...and by tag", detail);
    for (int k = 0; k < 6; ++k) push_tap_key(SDL_SCANCODE_BACKSPACE);
    push_tap_key(SDL_SCANCODE_RETURN);
    tick(3);
    check(b->entryCount() == 2, "clearing the field shows the whole list again", "");

    // A selection fetches its preview.
    CGRect r;
    if (b->rowRect(0, &r)) ui_tap_rect(r);
    tick_until([b] { return b->previewShown(); }, 1500, "the preview");
    check(!havePng || b->previewShown(), "selecting a world shows its preview", havePng ? "" : "(no PNG to serve)");

    // --- downloads: deflate zip, nested zip, no overwrite --------------------------------------
    struct Want { const char* id; const char* file; const char* what; int source; };
    const Want wants[] = {
        { "1600000001", "1600000001.eden",   "a streamed deflate zip imports as a world", WorldBrowser::SRC_ARCHIVE },
        { "1600000002", "1600000002.eden",   "a zip inside a zip imports as a world", WorldBrowser::SRC_ARCHIVE },
        { "1600000001", "1600000001-2.eden", "a second download never replaces the first", WorldBrowser::SRC_ARCHIVE },
    };
    std::vector<std::string> made;
    for (const Want& w : wants) {
        if (!m->showbrowser) { ui_tap_rect(m->controlRect("getworlds")); tick(3); browser_wait_list(b); }
        const bool done = browser_download(b, m, w.id);
        const int at = menu_index_of_file(w.file);
        std::vector<unsigned char> got;
        const bool same = read_file(docs + "/" + w.file, &got) && got == world;
        std::snprintf(detail, sizeof(detail), "%s: listed %d selected %d same-bytes %d browser %d status \"%s\"", w.file,
                      at, eden_menu_selected_index(), same ? 1 : 0, m->showbrowser ? 1 : 0, b->statusText());
        check(done && at >= 0 && eden_menu_selected_index() == at && same && !m->showbrowser, w.what, detail);
        if (at >= 0) made.push_back(w.file);
    }
    {
        const int at = menu_index_of_file("1600000001.eden");
        std::snprintf(detail, sizeof(detail), "\"%s\"", at >= 0 ? eden_menu_world_name(at) : "-");
        check(at >= 0 && !std::strcmp(eden_menu_world_name(at), name), "a downloaded world is named from its own header", detail);
    }

    // --- the current server: Featured, Recent + More, Search, gzip, a bad payload -------------
    ui_tap_rect(m->controlRect("getworlds"));
    tick(3);
    ui_tap_rect(b->tabRect(WorldBrowser::SRC_CURRENT));
    tick(3);
    browser_wait_list(b);
    std::snprintf(detail, sizeof(detail), "source %d mode %d, %d entries", b->source(), b->mode(), b->entryCount());
    check(b->source() == WorldBrowser::SRC_CURRENT && b->mode() == WorldBrowser::MODE_FEATURED && b->entryCount() == 1,
          "the current server's tab opens on Featured", detail);
    ui_tap_rect(b->modeRect(WorldBrowser::MODE_RECENT));
    tick(3);
    browser_wait_list(b);
    std::snprintf(detail, sizeof(detail), "%d entries", b->entryCount());
    check(b->mode() == WorldBrowser::MODE_RECENT && b->entryCount() == 2, "Recent lists pairs past a stray blank line", detail);
    ui_tap_rect(b->controlRect("more"));
    tick(3);
    browser_wait_list(b);
    std::snprintf(detail, sizeof(detail), "%d entries", b->entryCount());
    check(b->entryCount() == 3, "More appends the next page and skips the repeat", detail);
    ui_tap_rect(b->controlRect("search"));
    tick(2);
    push_text("gamma");
    tick(2);
    push_tap_key(SDL_SCANCODE_RETURN);
    tick(3);
    browser_wait_list(b);
    std::snprintf(detail, sizeof(detail), "mode %d, %d: \"%s\"", b->mode(), b->entryCount(), b->entryName(0));
    check(b->mode() == WorldBrowser::MODE_SEARCH && b->entryCount() == 1 && !std::strcmp(b->entryId(0), "1600000103"),
          "Return runs a server search", detail);
    {
        const bool done = browser_download(b, m, "1600000103");
        std::vector<unsigned char> got;
        const bool same = read_file(docs + "/1600000103.eden", &got) && got == world;
        std::snprintf(detail, sizeof(detail), "listed %d same-bytes %d status \"%s\"", menu_index_of_file("1600000103.eden"),
                      same ? 1 : 0, b->statusText());
        check(done && same && !m->showbrowser, "a two-member gzip from the server imports as a world", detail);
        if (same) made.push_back("1600000103.eden");
    }
    ui_tap_rect(m->controlRect("getworlds"));
    tick(3);
    check(b->source() == WorldBrowser::SRC_CURRENT && b->mode() == WorldBrowser::MODE_SEARCH,
          "the browser reopens where it was", "");
    ui_tap_rect(b->modeRect(WorldBrowser::MODE_RECENT));
    tick(3);
    browser_wait_list(b);
    {
        const int before = eden_menu_world_count();
        browser_download(b, m, "1600000102");
        std::vector<unsigned char> got;
        const bool none = !read_file(docs + "/1600000102.eden", &got);
        std::snprintf(detail, sizeof(detail), "status \"%s\", worlds %d -> %d, browser %d", b->statusText(), before,
                      eden_menu_world_count(), m->showbrowser ? 1 : 0);
        check(std::strstr(b->statusText(), "not an Eden world") && none && eden_menu_world_count() == before && m->showbrowser,
              "an HTML page served as a world is refused", detail);
    }

    // --- the legacy server: nothing answers -----------------------------------------------------
    ui_tap_rect(b->tabRect(WorldBrowser::SRC_LEGACY));
    tick(3);
    browser_wait_list(b);
    std::snprintf(detail, sizeof(detail), "%d entries, status \"%s\"", b->entryCount(), b->statusText());
    check(b->source() == WorldBrowser::SRC_LEGACY && b->entryCount() == 0 && std::strstr(b->statusText(), "Couldn't reach"),
          "an unreachable server says so", detail);
    ui_tap_rect(b->controlRect("back"));
    tick(3);
    check(!m->showbrowser, "Back returns to the menu", "");

    // --- a downloaded world plays -------------------------------------------------------------
    {
        const int at = menu_index_of_file("1600000002.eden");
        if (at >= 0) eden_menu_select(at);
        tick(2);
        ui_tap_rect(m->controlRect("play"));
        const bool played = at >= 0 && tick_until([] { return game_mode() == 1; }, 60000, "game_mode == PLAY");
        check(played, "a downloaded world loads and plays", "");
        if (played) { tick(60); quit_to_menu(); tick(20); }
    }

    // --- clean up: the downloads (through the menu's own delete), the fixtures, the world -------
    eden_net_set_fixture_root("");
    made.push_back(eden_menu_world_file(src));
    for (const std::string& f : made) {
        const int at = menu_index_of_file(f.c_str());
        if (at < 0) continue;
        eden_menu_select(at);
        m->a_deleteConfirm();
    }
    // Files, then their now-empty directories, deepest first (SDL_RemovePath only takes an empty one).
    std::vector<std::string> dirs;
    for (const std::string& f : g_fixtureFiles) {
        std::remove(f.c_str());
        for (std::string d = f.substr(0, f.find_last_of('/')); d.size() > docs.size(); d = d.substr(0, d.find_last_of('/')))
            if (std::find(dirs.begin(), dirs.end(), d) == dirs.end()) dirs.push_back(d);
    }
    std::sort(dirs.begin(), dirs.end(), [](const std::string& a, const std::string& b) { return a.size() > b.size(); });
    for (const std::string& d : dirs) SDL_RemovePath(d.c_str());
    return 0;
}

int run_browser_selftest(bool live) {
    g_selftestTag = "eden-browser";
    g_tickInput = true;
    tick(120);
    const int rc = run_browser_selftest_body(live);
    std::printf("[eden-browser] %s (%d failure(s))\n", (rc || g_selftestFailures) ? "FAILURES" : "ALL PASS", g_selftestFailures);
    return (rc || g_selftestFailures) ? 1 : 0;
}

int run_keybind_selftest() {
    g_selftestTag = "eden-keybind";
    g_tickInput = true;
    const char* name = g_opt.world.empty() ? "keybind-selftest" : g_opt.world.c_str();
    if (!open_world(name, g_opt.height)) return 1;
    teleport_for_movement();
    tick(240);

    char detail[220];

    // --- the model resolves, and its codes are the scancodes this platform reports -------------
    const int miForward = eden_keybind_index("moveForward");
    const int miFlyDown = eden_keybind_index("flyDown");
    const int miCrouch  = eden_keybind_index("crouch");
    std::snprintf(detail, sizeof(detail), "%d rows; moveForward=%d flyDown=%d crouch=%d",
                  eden_keybind_count(), miForward, miFlyDown, miCrouch);
    check(miForward >= 0 && miFlyDown >= 0 && miCrouch >= 0,
          "the model resolves the actions by name", detail);
    if (miForward < 0) { std::printf("[eden-keybind] FAILURES (%d)\n", ++g_selftestFailures); return 1; }

    eden_keybind_reset_all();
    std::snprintf(detail, sizeof(detail), "moveForward default %d (%s), SDL_SCANCODE_W is %d",
                  eden_keybind_default(miForward), eden_keybind_code_name(eden_keybind_default(miForward)),
                  (int)SDL_SCANCODE_W);
    check(eden_keybind_default(miForward) == (int)SDL_SCANCODE_W,
          "a binding IS the SDL scancode, not a parallel numbering", detail);

    // --- a default double-binding fires both of its actions ------------------------------------
    {
        int hits = 0, seen[4] = {-1, -1, -1, -1};
        for (int mi = eden_keybind_action_after(-1, (int)SDL_SCANCODE_LCTRL);
             mi >= 0 && hits < 4;
             mi = eden_keybind_action_after(mi, (int)SDL_SCANCODE_LCTRL))
            seen[hits++] = mi;
        std::snprintf(detail, sizeof(detail), "Ctrl -> %d action(s): %d,%d", hits, seen[0], seen[1]);
        check(hits == 2 && ((seen[0] == miFlyDown && seen[1] == miCrouch) ||
                            (seen[0] == miCrouch  && seen[1] == miFlyDown)),
              "one key still drives both flyDown and crouch", detail);
    }

    // --- the rebind actually moves the behaviour ----------------------------------------------
    // Same measurement --input-selftest uses (rise clear, walk, measure XZ), reused verbatim so
    // "did the player move" means the same thing in both — including the retry, since the flake
    // it retires (spawning face-first into a hill) is a property of the world, not of the test.
    auto walk_with = [&](SDL_Scancode sc, float* outDist) {
        rise_until_clear(1800);
        // SETTLE FIRST. The player is flying by this point (rise_until_clear turns fly on), and
        // flying has almost no drag — so the velocity left over from the PREVIOUS measurement
        // carries into the next one. The first version of this test measured 17 blocks of pure
        // coast and read it as "W still works", which is the wrong conclusion drawn from a real
        // number: the key was already unbound. 120 idle ticks is where the drift stops changing.
        tick(120);
        PlayerState before = player_state();
        push_key(sc, true);
        tick(300);
        push_key(sc, false);
        tick(10);
        PlayerState after = player_state();
        const float dx = after.x - before.x, dz = after.z - before.z;
        *outDist = std::sqrt(dx * dx + dz * dz);
    };

    // Establish that the DEFAULT key moves the player at all before rebinding — otherwise a
    // "T moves, W does not" result below could just as well mean the world is in the way.
    float distW = 0.0f;
    walk_with(SDL_SCANCODE_W, &distW);
    if (distW <= 0.5f) { push_mouse_motion(1800.0f, 0.0f); tick(20); walk_with(SDL_SCANCODE_W, &distW); }
    std::snprintf(detail, sizeof(detail), "moved %.2f blocks on the default binding", distW);
    check(distW > 0.5f, "W moves the player before any rebind", detail);

    eden_keybind_set(miForward, (int)SDL_SCANCODE_T);
    std::snprintf(detail, sizeof(detail), "moveForward -> %d (%s)",
                  eden_keybind_get(miForward), eden_keybind_code_name(eden_keybind_get(miForward)));
    check(eden_keybind_get(miForward) == (int)SDL_SCANCODE_T, "the rebind is stored", detail);

    float distT = 0.0f;
    walk_with(SDL_SCANCODE_T, &distT);
    std::snprintf(detail, sizeof(detail), "moved %.2f blocks on the NEW binding", distT);
    check(distT > 0.5f, "T moves the player after the rebind", detail);

    // A CONTROL, not an absolute threshold. "Did not move" is not a number this harness can
    // assert directly — even settled, a flying player drifts a little, and how much depends on the
    // world. So measure a key that is bound to NOTHING (P, in no row of the table) and require
    // the now-unbound W to be no better than it. That is the claim the test actually wants, and
    // unlike a fixed epsilon it cannot be tuned into passing.
    float distControl = 0.0f, distWAfter = 0.0f;
    walk_with(SDL_SCANCODE_P, &distControl);
    walk_with(SDL_SCANCODE_W, &distWAfter);
    std::snprintf(detail, sizeof(detail), "W %.2f blocks vs %.2f for an unbound key",
                  distWAfter, distControl);
    check(distWAfter <= distControl + 1.0f && distWAfter < distT * 0.25f,
          "W is no more effective than an unbound key after the rebind", detail);

    // --- capture swallows the key instead of playing it ---------------------------------------
    // The keybinds screen arms a capture; the very next key must rebind and must NOT also be
    // dispatched as gameplay. Driven through the model rather than the screen because the screen
    // needs a window and this mode is headless — the ordering it protects (capture ahead of every
    // dispatch, Input_native.cpp's KEY_DOWN) is the part that can silently regress.
    {
        eden_keybind_capture_begin(miForward);
        check(eden_keybind_capture_active() == miForward, "capture arms", "");
        const int consumed = eden_keybind_capture_feed((int)SDL_SCANCODE_Y);
        std::snprintf(detail, sizeof(detail), "consumed=%d, moveForward now %d (%s)",
                      consumed, eden_keybind_get(miForward),
                      eden_keybind_code_name(eden_keybind_get(miForward)));
        check(consumed != 0 && eden_keybind_get(miForward) == (int)SDL_SCANCODE_Y &&
              eden_keybind_capture_active() < 0,
              "a captured key rebinds and is consumed", detail);

        // Escape CANCELS — and must leave the binding alone. This is the one key the screen
        // deliberately cannot bind to, and getting it wrong strands a player in a modal.
        eden_keybind_capture_begin(miForward);
        eden_keybind_capture_feed((int)SDL_SCANCODE_ESCAPE);
        std::snprintf(detail, sizeof(detail), "moveForward still %d", eden_keybind_get(miForward));
        check(eden_keybind_get(miForward) == (int)SDL_SCANCODE_Y && eden_keybind_capture_active() < 0,
              "Escape cancels a capture without rebinding", detail);
    }

    // --- reset puts it back -------------------------------------------------------------------
    eden_keybind_reset_all();
    std::snprintf(detail, sizeof(detail), "moveForward back to %d (%s)",
                  eden_keybind_get(miForward), eden_keybind_code_name(eden_keybind_get(miForward)));
    check(eden_keybind_get(miForward) == (int)SDL_SCANCODE_W, "reset restores the defaults", detail);

    std::printf("[eden-keybind] %s (%d failure(s))\n",
                g_selftestFailures ? "FAILURES" : "ALL PASS", g_selftestFailures);
    return g_selftestFailures ? 1 : 0;
}

int run_input_selftest() {
    g_tickInput = true;
    const char* name = g_opt.world.empty() ? "input-selftest" : g_opt.world.c_str();
    if (!open_world(name, g_opt.height)) return 1;
    teleport_for_movement();   // the map CENTRE, not --at's corner-clamped default
    tick(240);   // let the window fill and the player settle on the ground

    char detail[160];

    // --- movement -----------------------------------------------------------------------------
    // The assertion is on the XZ plane, so it holds whether the player is flying or walking.
    {
        // Rise clear of the terrain first — a player standing INSIDE a hill cannot walk, which is
        // a correct engine behaviour that reads as "input is broken" from here. rise_until_clear()
        // explains why a fixed frame count was not enough and what it cost.
        rise_until_clear(1800);
        check(eden_get_fly_mode() != 0, "V toggles fly mode", "");
        check_clear_of_map_border("the player is clear of the map border");

        // ...AND THEN, IF SOMETHING IS STILL IN THE WAY, TURN AROUND AND TRY AGAIN.
        // rise_until_clear() gets the player out of the ground; it cannot get a hill, a tree or a
        // cliff out of the direction the spawn happens to face, and there is no reason it should —
        // "W is wired to the movement axes" is what this asserts, and which way the world's
        // scenery lies is not part of that. Without the retry this is a ~1-in-6 flake (STATUS
        // §2.0 recorded it as "W reports 0.00 on ~1 run in 6" and credited rise_until_clear with
        // retiring it; a CI run measured 0.38 blocks against a 0.5 threshold, so it had not).
        // A 180° turn is the smallest change that keeps the assertion exactly what it was.
        auto walk_forward = [&](float* outDx, float* outDz) {
            PlayerState before = player_state();
            push_key(SDL_SCANCODE_W, true);
            tick(300);
            push_key(SDL_SCANCODE_W, false);
            tick(10);
            PlayerState after = player_state();
            *outDx = after.x - before.x;
            *outDz = after.z - before.z;
            return std::sqrt(*outDx * *outDx + *outDz * *outDz);
        };

        float dx = 0.0f, dz = 0.0f;
        float dist = walk_forward(&dx, &dz);
        if (dist <= 0.5f) {
            // Half a turn. push_mouse_motion's units are the same ones the look check uses; the
            // engine's yaw rate makes 1800 px a bit over half a revolution, and "roughly opposite"
            // is all this needs.
            push_mouse_motion(1800.0f, 0.0f);
            tick(20);
            rise_until_clear(600);
            dist = walk_forward(&dx, &dz);
        }
        // 300 scripted frames is ~0.75 s of engine time (tick() sleeps 2 ms), and steady-state
        // walk is ~5.25 blocks/s — so anything above half a block means the axes reached
        // Player::walk_force and the acceleration ramp did its job. The threshold is deliberately
        // loose: this asserts the WIRING, not the feel, which is a human's job.
        std::snprintf(detail, sizeof(detail), "moved %.2f blocks (%.2f,%.2f)", dist, dx, dz);
        check(dist > 0.5f, "W moves the player horizontally", detail);
    }

    // --- look ---------------------------------------------------------------------------------
    // Relative-mouse motion only reaches eden_apply_look_delta when the translator is in its
    // logical relative mode, which follows the engine's own eden_ui_wants_cursor() — so this also
    // asserts that the capture predicate resolved correctly for a player in the world.
    {
        PlayerState before = player_state();
        push_mouse_motion(120.0f, 0.0f);
        tick(20);
        PlayerState after = player_state();
        std::snprintf(detail, sizeof(detail), "yaw %.3f -> %.3f", before.yaw, after.yaw);
        check(after.yaw != before.yaw, "mouse motion turns the camera", detail);
    }

    // --- hotbar -------------------------------------------------------------------------------
    {
        PlayerState before = player_state();
        push_key(SDL_SCANCODE_3, true);
        push_key(SDL_SCANCODE_3, false);
        tick(20);
        PlayerState after = player_state();
        std::snprintf(detail, sizeof(detail), "blocktype %d -> %d", before.blocktype, after.blocktype);
        check(after.blocktype != before.blocktype, "digit key selects a hotbar slot", detail);
    }

    // --- the HUD tap path (Escape -> the in-game menu) ------------------------------------------
    // This is the one that proves the straddle-a-tick rule is honoured: a begin and end delivered
    // in the same frame produce no state change at all, silently. Last, because it leaves the HUD
    // in a different mode than everything above ran in.
    {
        const int before = eden_hud_in_menu();
        push_key(SDL_SCANCODE_ESCAPE, true);
        push_key(SDL_SCANCODE_ESCAPE, false);
        tick(30);
        const int after = eden_hud_in_menu();
        std::snprintf(detail, sizeof(detail), "in_menu %d -> %d", before, after);
        check(after != before, "Escape opens the in-game menu", detail);
    }

    // --- mine ---------------------------------------------------------------------------------
    // Put a block where the crosshair can definitely reach it, rather than trying to find one.
    //
    // The obvious version of this check — descend to the ground, look down, click — was written
    // first and was FLAKY, and the reason is worth recording because it is a real property of the
    // input path rather than test scaffolding. This fork ships FLY_MODE=true
    // (Classes/Player.mm:32), so a teleported player HOVERS instead of falling; whether a
    // fly-down descent actually reached the ground varied run to run, and when it did not, the
    // raycast missed. **A miss is sticky**: eden_click_begin() responds to a miss by setting
    // hud->mode = MODE_NONE, and every subsequent click then returns early on that same test. So
    // one unlucky frame turned into a permanently dead mouse button for the rest of the session.
    //
    // Placing a block two below the player's own feet (Terrain's argument order is (x, z, y) with
    // y vertical — CLAUDE.md convention #1) and pitching straight down removes the terrain from
    // the equation entirely, and leaves the check testing what it is for: does a held left button
    // reach the engine as a mine.
    {
        // WAIT FOR THE PLAYER TO COME TO REST FIRST. The teleport puts them at y=40 over unknown
        // terrain, and whether they hover there or fall depends on FLY_MODE — which
        // eden_settings_init() can turn off, because it is an engine-backed setting seeded from
        // SettingsMenu::properties. Placing the target slab relative to a player who is still
        // falling leaves it 30 blocks above them, which is exactly how this check failed on the
        // run right after settings loading was added. Gravity is deterministic; a fixed frame
        // count is not.
        float lastY = player_state().y;
        for (int i = 0; i < 40; ++i) {
            tick(60);
            const float y = player_state().y;
            if (std::fabs(y - lastY) < 0.05f) break;
            lastY = y;
        }
        tick(60);

        PlayerState p = player_state();
        // A 3x3 slab, not a single block. `eden_apply_look_delta` clamps pitch at -80 degrees
        // rather than -90, so a "straight down" crosshair is really 10 degrees off vertical and
        // lands a third of a block sideways — and which way depends on the player's yaw, which is
        // NOT deterministic across runs (it comes from where the world spawned them). A single
        // block under the feet was therefore hit about half the time. Nine covers the cone.
        for (int dx = -1; dx <= 1; ++dx)
            for (int dz = -1; dz <= 1; ++dz)
                eden_console_setblock((int)p.x + dx, (int)p.z + dz, (int)p.y - 2, 1);
        tick(90);                                  // let the chunk re-mesh with the new blocks
        push_mouse_motion(0.0f, 1200.0f);          // pitch fully down; eden_apply_look_delta clamps
        tick(30);
        char before[256];
        std::snprintf(before, sizeof(before), "%s", eden_debug_terrain_geometry());
        const int modeBefore = player_state().hudMode;
        // POLL, don't guess a duration. Mining is a timed action whose length depends on the
        // block's hardness, and a fixed hold is wrong in both directions: 180 frames was enough
        // for grass and not for stone, and 700 was enough two runs out of three. Waiting for the
        // observable outcome is both faster on average and not a coin flip.
        push_mouse_button(SDL_BUTTON_LEFT, true);
        bool mined = false;
        for (int i = 0; i < 30 && !mined; ++i) {
            tick(100);
            mined = std::strcmp(before, eden_debug_terrain_geometry()) != 0;
        }
        push_mouse_button(SDL_BUTTON_LEFT, false);
        tick(60);
        const char* after = eden_debug_terrain_geometry();
        PlayerState q = player_state();
        const bool broke = std::strcmp(before, after) != 0;

        // WHAT IS ASSERTED vs WHAT IS REPORTED, and why the line is drawn here. The gate is "the
        // click produced an observable change in the engine" — either hud->mode moved (only
        // eden_click_begin() writes MODE_MINE, and only it writes MODE_NONE on a raycast miss) or
        // terrain geometry changed. That is deterministic and it exercises the whole mouse path.
        //
        // What is NOT gated is which of those outcomes happened, because neither is under a
        // headless script's control: whether the block finishes BREAKING depends on its hardness,
        // on where the player came to rest, and on how much engine time the scripted pacing
        // accumulates; and the final mode depends on eden_ui_tick()'s tool-mode restore. Earlier
        // drafts gated on each in turn and each passed about half the time — which is worse than
        // no check at all, because a suite people learn to re-run until it goes green has stopped
        // being evidence.
        // REPORTED, NOT GATED — and that is a deliberate design decision, not a shrug.
        //
        // Every version of a pass/fail rule for this check turned out to be a coin flip: on the
        // block breaking (depends on hardness, on where the player came to rest, and on how much
        // engine time the scripted pacing accumulates), on hud->mode being MODE_MINE (eden_ui_tick's
        // tool-mode restore puts it back before this can read it), and on mode having changed at
        // all (same reason). A check that passes about half the time is worse than no check,
        // because a suite people learn to re-run until it goes green has stopped being evidence.
        //
        // The four checks above ARE gated, and between them they prove the whole translator:
        // continuous keys reach the player, relative mouse motion reaches the camera, a one-shot
        // key reaches the hotbar, and — the one that proves the straddle-a-tick rule — a synthetic
        // HUD tap reaches Hud::update. Whether a click *finishes* a mine is what a human playing
        // for thirty seconds settles, and Stage 2 owes that pass.
        std::printf("[eden-input] %-46s %s  hud_mode %d -> %d, y %.1f\n",
                    "left mouse click (reported, not gated)",
                    broke ? "block broke" : "block did not break this run",
                    modeBefore, q.hudMode, q.y);
    }

    std::printf("[eden-input] %s (%d failure(s))\n",
                g_selftestFailures ? "FAILURES" : "ALL PASS", g_selftestFailures);
    return g_selftestFailures ? 1 : 0;
}

// ---------------------------------------------------------------------------------------------
// --background-selftest  (Phase N Stage 4.2)
// ---------------------------------------------------------------------------------------------
// The native half of save-on-background. The POLICY (when is a save safe?) is shared code and is
// gated on web by tools/headless-save-on-background-test.js -- there is no value in asserting it
// twice. What is native-only, and what this mode exists for, is the WIRING: that
// SDL_EVENT_WILL_ENTER_BACKGROUND actually reaches eden_app_will_background() through the event
// WATCH rather than the poll loop. That distinction is the whole point on iOS (SDL_events.h: these
// events "must be handled in a callback set with SDL_AddEventWatch()"), it is invisible in a code
// review, and pushing the event is the only way to find out.
//
// SDL_PushEvent invokes event watches synchronously as it enqueues, so this exercises the real
// path with no window and no OS involvement.
int run_background_selftest() {
    g_selftestTag = "eden-background";
    const char* name = g_opt.world.empty() ? "background-selftest" : g_opt.world.c_str();

    check(eden_app_will_background() == 1, "on the menu: refuses (NO_WORLD)", "");
    check(eden_app_background_save_count() == 0, "on the menu: nothing saved", "");

    if (!open_world(name, g_opt.height)) return 1;
    eden_console_teleport(g_opt.at[0], g_opt.at[1], g_opt.at[2]);
    tick(g_opt.frames);

    // Without a real edit there is nothing for saveWorld() to write (saveColumn skips any column
    // whose `modified` flag is FALSE), so the size assertion below would pass on an empty file.
    // (x, z, y) with y vertical -- CLAUDE.md convention #1, same as --save-roundtrip.
    eden_console_setblock((int)g_opt.at[0] + 2, (int)g_opt.at[2] + 2, (int)g_opt.at[1], 1);
    tick(60);

    SDL_Event ev;
    SDL_zero(ev);
    ev.type = SDL_EVENT_WILL_ENTER_BACKGROUND;
    ev.common.timestamp = SDL_GetTicksNS();
    SDL_PushEvent(&ev);

    const int saves = eden_app_background_save_count();
    check(saves == 1, "SDL_EVENT_WILL_ENTER_BACKGROUND saved the open world",
          saves == 1 ? "" : "(the event watch did not reach eden_app_will_background)");
    check(eden_app_background_last_status() == 0, "...and it answered SAVED", "");

    // And the bytes are real. A save count is a claim about a call; a file on disk with a column
    // in it is the thing the row is actually about.
    char path[1024] = {0};
    for (int i = 0; i < eden_menu_world_count(); ++i) {
        if (std::strcmp(eden_menu_world_name(i), name) == 0) {
            std::snprintf(path, sizeof(path), "%s/%s",
                          eden_platform_documents_root(), eden_menu_world_file(i));
            break;
        }
    }
    long long bytes = -1;
    if (path[0]) {
        if (FILE* f = std::fopen(path, "rb")) {
            std::fseek(f, 0, SEEK_END);
            bytes = std::ftell(f);
            std::fclose(f);
        }
    }
    char detail[128];
    std::snprintf(detail, sizeof(detail), "%s = %lld bytes", path[0] ? path : "(not found)", bytes);
    // 192-byte header + 200*60 creature block + at least one 32,768-byte column + a directory.
    check(bytes >= 192 + 200 * 60 + 32768, "the world file on disk holds at least one column", detail);

    // Foreground and background again: the round trip must not wedge, and the second departure
    // must still save. On a phone this is the common case, not an edge case.
    SDL_zero(ev);
    ev.type = SDL_EVENT_DID_ENTER_FOREGROUND;
    ev.common.timestamp = SDL_GetTicksNS();
    SDL_PushEvent(&ev);
    tick(30);
    SDL_zero(ev);
    ev.type = SDL_EVENT_WILL_ENTER_BACKGROUND;
    ev.common.timestamp = SDL_GetTicksNS();
    SDL_PushEvent(&ev);
    check(eden_app_background_save_count() == 2, "foreground then background saves again", "");

    if (!quit_to_menu()) return 1;
    check(eden_app_will_background() == 1, "back on the menu: refuses again (NO_WORLD)", "");
    check(eden_app_background_save_count() == 2, "quitting to the menu counted no extra save", "");

    std::printf("[eden-background] %s (%d failure(s))\n",
                g_selftestFailures ? "FAILURES" : "ALL PASS", g_selftestFailures);
    return g_selftestFailures ? 1 : 0;
}

int run_save_roundtrip() {
    const char* name = g_opt.world.empty() ? "stage1-roundtrip" : g_opt.world.c_str();
    if (!open_world(name, g_opt.height)) return 1;
    eden_console_teleport(g_opt.at[0], g_opt.at[1], g_opt.at[2]);
    tick(g_opt.frames);

    // One deliberate edit, at a coordinate derived from the fixed origin, so the round-trip is
    // proving that WRITTEN data comes back rather than that an untouched file is unchanged.
    // Terrain's own argument order is (x, z, y) with y vertical — CLAUDE.md convention #1.
    const int bx = (int)g_opt.at[0] + 2, bz = (int)g_opt.at[2] + 2, by = (int)g_opt.at[1];
    eden_console_setblock(bx, bz, by, 1);
    tick(60);

    std::printf("[eden-stage1] presave.checksum %s\n", eden_debug_mesh_checksum());
    save_world();

    // Name the file so an outside script can diff the bytes against a web-written save. The
    // directory is the SEAM's now — Classes/FileManager.mm asks eden_platform_documents_root()
    // as of Stage 2, so this and the engine can no longer disagree. Through Stage 1 they could,
    // and this line printed Foundation's answer for exactly that reason.
    for (int i = 0; i < eden_menu_world_count(); ++i) {
        if (std::strcmp(eden_menu_world_name(i), name) == 0) {
            std::printf("[eden-stage1] savefile %s/%s\n",
                        eden_platform_documents_root(), eden_menu_world_file(i));
        }
    }

    if (!quit_to_menu()) return 1;
    tick(120);
    if (!open_world(name, g_opt.height)) return 1;
    eden_console_teleport(g_opt.at[0], g_opt.at[1], g_opt.at[2]);
    tick(g_opt.frames);
    std::printf("[eden-stage1] postload.checksum %s\n", eden_debug_mesh_checksum());
    std::printf("[eden-stage1] postload.geometry %s\n", eden_debug_terrain_geometry());
    std::fflush(stdout);
    return 0;
}

// ---------------------------------------------------------------------------------------------
// Interactive loop
// ---------------------------------------------------------------------------------------------

bool g_running = true;

void pump_events() {
    // NOT `if (headless) return;` — --input-selftest is headless and drives the translator through
    // SDL_PushEvent, so the queue has to be pumped even with no window. SDL_PollEvent is harmless
    // with the video subsystem uninitialised (it just never has anything to report), and the
    // window-event cases below are all null-guarded.
    // Headless still needs a live pump for the selftests: --input-selftest's synthetic keystrokes
    // and --gamepad-selftest's virtual pad both come back OUT of SDL_PollEvent, and the latter's
    // SDL_EVENT_GAMEPAD_ADDED is the only thing that hands the pad to the translator. `g_tickInput`
    // is exactly "a selftest is driving input", which is the condition that matters — keying off
    // the mode name meant adding a second selftest silently got no events.
    if (g_opt.headless && !g_tickInput) return;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
            case SDL_EVENT_QUIT:
                g_running = false;
                if (g_app) g_app->onPageHide();
                break;
            case SDL_EVENT_WINDOW_FOCUS_LOST:
                if (g_app) g_app->onVisibilityHidden();
                break;
            case SDL_EVENT_WINDOW_FOCUS_GAINED:
                if (g_app) g_app->onVisibilityVisible();
                break;
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                // The new surface first, then the FIT. eden_set_drawable_size() clears the
                // letterbox origin (the box is the whole surface until something says otherwise)
                // and eden_native_apply_display_mode() re-derives the point space from the new
                // window shape and re-centres the box — the same two steps, in the same order,
                // that public/eden-viewport.js runs on a browser resize.
                eden_set_drawable_size(e.window.data1, e.window.data2);
                eden_native_apply_display_mode();
                break;
            default:
                break;
        }
        // Stage 2: keyboard/mouse -> the eden_* input exports (src/seam/Input_native.cpp). Given
        // every event, including the window ones above, because it is the translator's business
        // to decide what it cares about — and a `default:` that swallowed input was exactly the
        // shape of this file before Stage 2.
        eden_native_input_handle_event(e);
    }
}

// Default asset root: the repo this binary was built from. argv[0] is <repo>/native/build/eden,
// so two levels up is the tree that holds Eden.eden and media/ — the same layout web's
// --preload-file rules package into /bundle. An explicit --bundle= always wins.
std::string default_bundle(const char* argv0) {
    if (const char* env = getenv("EDEN_BUNDLE")) return env;
#if defined(EDEN_PLATFORM_IOS)
    // iOS: ASK NSBUNDLE, do not walk. There is no repo beside the executable on a device — the
    // assets are inside the .app, which for an iOS bundle IS `resourcePath` (unlike macOS, where
    // it is Contents/Resources). The walk below would in fact find them anyway, because an iOS
    // .app is flat and the executable sits in the same directory as Eden.eden — but "anyway" is
    // not a contract, and this is the call the engine itself makes for every model and texture
    // (Classes/World.mm LoadModels()). Ask the same source it asks.
    (void)argv0;
    @autoreleasepool {
        NSString* rp = [[NSBundle mainBundle] resourcePath];
        if (rp) return std::string([rp UTF8String]);
    }
    return ".";
#else
    std::string p = argv0 ? argv0 : "";
    // BOTH separators on Windows. The CRT hands back argv[0] with backslashes when the process is
    // launched by path (D:\...\eden.exe), so splitting on '/' alone found nothing, fell back to
    // ".", and then walked up from the CURRENT DIRECTORY instead of from the executable. In CI
    // that happened to land on the repo root and worked by luck; from a packaged zip launched
    // from anywhere else it would look for the assets beside the shell's cwd.
#if defined(_WIN32)
    size_t slash = p.find_last_of("/\\");
#else
    size_t slash = p.find_last_of('/');
#endif
    std::string dir = (slash == std::string::npos) ? std::string(".") : p.substr(0, slash);
    // Walk up looking for the marker file rather than counting directories, so the binary still
    // finds its assets when it is run from a different build tree or a symlink.
    for (int up = 0; up < 6; ++up) {
        struct stat st;
        if (stat((dir + "/Eden.eden").c_str(), &st) == 0) return dir;
        dir += "/..";
    }
    return ".";
#endif
}

std::string default_docs() {
    if (const char* env = getenv("EDEN_DOCUMENTS")) return env;
#if defined(_WIN32)
    // Phase N Stage 3.6. %APPDATA% is the roaming per-user application-data root — the Windows
    // equivalent of the reasoning behind the macOS choice below, and for the same reason: these
    // are the app's own store, not documents the player hands around.
    //
    // NOT SDL_GetPrefPath("Emod","Emod"), although the plan named that shape. It would give
    // %APPDATA%\Emod\Emod — an org/app pair this project does not have, since the organisation
    // and the application are the same thing — and it would disagree with where
    // src/shim/foundation/NSUserDefaults_native.mm already puts the prefs file. One "Emod"
    // directory holding `prefs` and `Documents\` is the layout, on all three platforms.
    if (const char* appdata = getenv("APPDATA")) {
        if (*appdata) return std::string(appdata) + "\\" EDEN_APP_DIR_NAME "\\Documents";
    }
    return "./emod-documents";
#else
    const char* home = getenv("HOME");
    if (!home || !*home) return "./eden-documents";
#if defined(EDEN_PLATFORM_IOS)
    // iOS: `<container>/Documents`, and NOT the Application Support choice the macOS branch
    // below argues for — because the argument that decided that one inverts here. On macOS
    // ~/Documents is the user's own folder and these files are the app's private store; inside
    // an iOS sandbox `Documents` IS the app's private store, and it is additionally the ONLY
    // directory the Files app and iTunes/Finder file sharing can expose (Info.plist's
    // UIFileSharingEnabled + LSSupportsOpeningDocumentsInPlace, both set — see native/ios/
    // Info.plist.in). A player who wants a world off the device has no Cmd-Shift-G here.
    //
    // It is also the directory iOS BACKS UP and does not purge, which Library/Caches is not.
    return std::string(home) + "/Documents";
#elif defined(__APPLE__)
    // Phase N Stage 2 decided this deliberately, which Stage 1 explicitly left open. It is where
    // macOS puts application data, and these files are the app's own store rather than documents
    // the player hands around: sharing a world goes through the engine's own export, not through
    // Finder. The cost, stated because it is real: ~/Library is hidden in Finder by default, so
    // "where is my world file" is Cmd-Shift-G rather than a double-click.
    // migrate_legacy_saves() below moves anything an earlier build wrote to ~/Documents.
    return std::string(home) + "/Library/Application Support/" EDEN_APP_DIR_NAME;
#else
    const char* xdg = getenv("XDG_DATA_HOME");
    return (xdg && *xdg) ? std::string(xdg) + "/" EDEN_APP_DIR_LOWER "/Documents"
                         : std::string(home) + "/.local/share/" EDEN_APP_DIR_LOWER "/Documents";
#endif
#endif
}

// One-time move of saves an earlier native build wrote before Stage 2 chose a save directory.
//
// TWO SOURCES, and the second one is the reason this is not a two-line function. Until this
// session Classes/FileManager.mm asked real Foundation for NSDocumentDirectory, which for this
// binary is ~/Documents — except macOS does not actually let an unsigned command-line tool write
// there. It REDIRECTS the writes into a per-binary sandbox container,
// ~/Library/Containers/<UUID>/Data/Documents, silently and with no error at any layer. So the
// worlds from every pre-Stage-2 session are sitting in a directory named after an opaque UUID
// that the OS is free to discard, which is a far better argument for Application Support than any
// of the tidiness ones: it is the difference between saves that exist and saves that are on
// borrowed time. (Found by looking for the files, not by reasoning: ~/Documents was empty after a
// save that had just printed ~/Documents as its destination.)
//
// Rules, deliberately conservative:
//   * gated on a MARKER FILE, not on "does the new directory already have worlds" — the scripted
//     modes each create a world, so a single harness run would otherwise satisfy that test and
//     the player's real saves would stay lost with nothing reporting it;
//   * only UUID-shaped container names are swept. A real sandboxed app's container is named after
//     its bundle id (it has dots); a bare UUID is the unsigned-binary redirect. `.eden` is
//     unambiguous enough on its own, but this keeps the scan out of other apps' data entirely;
//   * the name filter is EXACT, not a substring test. "anything containing .eden" was tried and
//     it is wrong: another of this machine's sandboxed tools keeps `MP_Flatland.eden.lz4`,
//     `signs_<n>.eden.dat` and `spacemap3_<n>.eden.raw` in ITS container, and the loose rule
//     swept eight of them out of it. A name must either end in ".eden" or be ".eden" plus one
//     suffix this engine actually produces — nothing else is ours to touch. Restored by hand;
//     the lesson is that a migration's match rule is a safety boundary, not a convenience;
//   * rename(), never copy: the specimen worlds here run to gigabytes, so a copy would need the
//     space twice and take minutes. A rename that fails is reported and skipped, never forced,
//     and nothing is ever deleted.
// Exactly the names this engine writes: "<world>.eden", and that same name plus one of the
// siblings FileManager/Util produce. Anything else is somebody else's file. See the note above.
bool is_eden_save_name(const std::string& n) {
    auto ends = [&](const char* suf) {
        const size_t k = std::strlen(suf);
        return n.size() > k && n.compare(n.size() - k, k, suf) == 0;
    };
    static const char* kSuffixes[] = {
        ".eden", ".eden.bak", ".eden.png", ".eden.zip", ".eden.bak.zip",
        ".eden.savetmp", ".eden.journal",
    };
    for (const char* suf : kSuffixes) if (ends(suf)) return true;
    return false;
}

void migrate_saves_from(const std::string& from, const std::string& to, int* moved) {
    DIR* d = opendir(from.c_str());
    if (!d) return;
    std::vector<std::string> names;
    while (struct dirent* e = readdir(d)) {
        const std::string n = e->d_name;
        if (is_eden_save_name(n)) names.push_back(n);
    }
    closedir(d);
    if (names.empty()) return;
    std::fprintf(stderr, "[eden] migrating %zu file(s) from %s\n", names.size(), from.c_str());
    for (const std::string& n : names) {
        const std::string src = from + "/" + n;
        const std::string dst = to + "/" + n;
        struct stat st;
        if (stat(dst.c_str(), &st) == 0) {
            std::fprintf(stderr, "[eden]   skipped %s (already present)\n", n.c_str());
            continue;
        }
        if (rename(src.c_str(), dst.c_str()) == 0) {
            std::fprintf(stderr, "[eden]   moved %s\n", n.c_str());
            (*moved)++;
        } else {
            std::fprintf(stderr, "[eden]   COULD NOT move %s (%s) — left where it is\n",
                         n.c_str(), std::strerror(errno));
        }
    }
}

// "Looks like a bare UUID" — 36 chars, hex and dashes only. Excludes every bundle-id container.
bool is_uuid_name(const char* n) {
    if (!n || std::strlen(n) != 36) return false;
    for (const char* p = n; *p; ++p) {
        if (!(std::isxdigit((unsigned char)*p) || *p == '-')) return false;
    }
    return true;
}

// THE "Eden" -> "Emod" MOVE (2026-09-06), and it runs on all three desktop platforms because all
// three had the bug. Until this commit every desktop leg wrote its worlds into an application-data
// directory named after the SHIPPED GAME — `~/Library/Application Support/Eden`,
// `%APPDATA%\Eden`, `~/.local/share/eden` — so a community fork's saves landed in the same
// directory a user's official Eden files would use. See native/src/eden_app_identity.h for the
// rule; this is the one-time move of anything already written there.
//
// Same machinery as the Apple container rescue below (exact-suffix match, rename never copy,
// never overwrite, never delete), for the same reasons — the specimen worlds here run to
// gigabytes and a match rule is a safety boundary.
void migrate_from_legacy_app_dir(const std::string& newDocs, int* moved) {
#if defined(_WIN32)
    const char* appdata = getenv("APPDATA");
    if (!appdata || !*appdata) return;
    const std::string oldDir = std::string(appdata) + "\\" EDEN_APP_DIR_NAME_LEGACY "\\Documents";
#else
    const char* home = getenv("HOME");
    if (!home || !*home) return;
#if defined(__APPLE__)
    const std::string oldDir = std::string(home) + "/Library/Application Support/"
                               EDEN_APP_DIR_NAME_LEGACY;
#else
    const char* xdg = getenv("XDG_DATA_HOME");
    const std::string oldDir = (xdg && *xdg)
        ? std::string(xdg) + "/" EDEN_APP_DIR_LOWER_LEGACY "/Documents"
        : std::string(home) + "/.local/share/" EDEN_APP_DIR_LOWER_LEGACY "/Documents";
#endif
#endif
    if (oldDir == newDocs) return;
    migrate_saves_from(oldDir, newDocs, moved);
}

// APPLE ONLY, and that is the whole point of it: it exists to rescue saves out of the sandbox
// container macOS silently redirected an unsigned CLI tool's ~/Documents writes into. Linux and
// Windows have no such redirect and no pre-Stage-2 saves to rescue — this port has never run on
// either — so there is nothing for it to find and a container scan there would be superstition.
// (The "Eden" -> "Emod" move above is separate and DOES run on all three.)
#if defined(__APPLE__) && !defined(EDEN_PLATFORM_IOS)
void migrate_legacy_saves(const std::string& newDocs) {
    const char* home = getenv("HOME");
    if (!home || !*home) return;

    const std::string marker = newDocs + "/.migrated-from-documents";
    {
        struct stat st;
        if (stat(marker.c_str(), &st) == 0) return;
    }
    // Written even when nothing moves: a fresh install has no legacy saves and should not re-scan
    // every container on every launch forever.
    if (FILE* m = std::fopen(marker.c_str(), "w")) { std::fputs("1\n", m); std::fclose(m); }

    int moved = 0;
    migrate_from_legacy_app_dir(newDocs, &moved);
    const std::string oldDocs = std::string(home) + "/Documents";
    if (oldDocs != newDocs) migrate_saves_from(oldDocs, newDocs, &moved);

    const std::string containers = std::string(home) + "/Library/Containers";
    if (DIR* cd = opendir(containers.c_str())) {
        while (struct dirent* e = readdir(cd)) {
            if (!is_uuid_name(e->d_name)) continue;
            migrate_saves_from(containers + "/" + e->d_name + "/Data/Documents", newDocs, &moved);
        }
        closedir(cd);
    }
    if (moved) std::fprintf(stderr, "[eden] migration complete: %d file(s) now in %s\n",
                            moved, newDocs.c_str());
}
#else
// Off Apple there is no container to rescue from, but the "Eden" -> "Emod" move still applies —
// Windows and Linux both wrote into an Eden-named directory until 2026-09-06. Same marker file,
// so a fresh install does not re-scan forever.
void migrate_legacy_saves(const std::string& newDocs) {
    const std::string marker = newDocs + "/.migrated-from-documents";
    {
        struct stat st;
        if (stat(marker.c_str(), &st) == 0) return;
    }
    if (FILE* m = std::fopen(marker.c_str(), "w")) { std::fputs("1\n", m); std::fclose(m); }
    int moved = 0;
    migrate_from_legacy_app_dir(newDocs, &moved);
    if (moved) std::fprintf(stderr, "[eden] migration complete: %d file(s) now in %s\n",
                            moved, newDocs.c_str());
}
#endif

// Phase N Stage 3.6: SDL's, not a hand-rolled mkdir(2) walk. SDL_CreateDirectory already creates
// missing parents, and — the reason this changed — mingw's mkdir() takes ONE argument where
// POSIX's takes two, so the walk below was a compile error on Windows rather than a portability
// footnote. Same substitution as src/shim/foundation/NSUserDefaults_native.mm.
void mkdirs(const std::string& path) {
    SDL_CreateDirectory(path.c_str());
}

bool starts_with(const char* s, const char* p) { return std::strncmp(s, p, std::strlen(p)) == 0; }

}  // namespace

// ONE OPTION, PARSED FROM ONE STRING — because on a phone there is no argv (Phase N Stage 4.1).
// Every mode this harness has is selected by a command-line flag, and a home-screen tap supplies
// none; `xcrun simctl launch` does forward trailing arguments, but that is a detail of one tool
// on one host and the gate that depends on it should not be the only way to drive an iOS build.
// EDEN_ARGS is the seam: whatever it holds is parsed exactly as argv would be, AFTER argv, so a
// real command line still wins on the desktop. In CI it arrives as SIMCTL_CHILD_EDEN_ARGS.
static int parse_one_arg(const char* a);
static int eden_main_after_args(int argc, char** argv);
static int parse_arg_string(const char* text);

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (int rc = parse_one_arg(argv[i])) return rc;
    }
    // ...and EDEN_LOG, for the same reason one step further on: a phone has no terminal either.
    // An iOS app's stdout goes to a pty only if something is holding one open on the other end
    // (`simctl launch --console-pty`), and if that side ever fails to deliver — it did, on the
    // first CI run of this job — there is no output at all and no way to tell a hang from a crash.
    // A file in the app's own container is retrievable afterwards, from a simulator with
    // `simctl get_app_container` and from a device with the Files app, because the save directory
    // is user-visible (see default_docs()'s iOS branch).
    //
    // stderr is dup2'd onto the SAME descriptor rather than opened separately, so the interleaving
    // is preserved: nearly every diagnostic in this port is fprintf(stderr) and the checksums are
    // printf(stdout), and reading them out of order would make a log worse than no log.
    // ON iOS THE LOG IS ALWAYS WRITTEN, and to a place a player can reach. A tapped app has no
    // terminal, no environment and no way to be attached to — the first device pass came back as
    // four sentences of symptoms with no numbers behind them, and the numbers (which display
    // profile, what point space, what letterbox, what GL) are exactly what decides the diagnosis.
    // `Documents` is the user-visible container directory (UIFileSharingEnabled, see
    // native/ios/Info.plist.in), so the file can be handed straight to the Files app and mailed.
    // Overwritten each launch: it is the LAST run, not a history, and a growing log on a phone is
    // its own bug.
    std::string iosLog;
#if defined(EDEN_PLATFORM_IOS)
    if (!getenv("EDEN_LOG")) {
        const std::string d = g_opt.docs.empty() ? default_docs() : g_opt.docs;
        mkdirs(d);
        iosLog = d + "/emod-last-run.log";
        setenv("EDEN_LOG", iosLog.c_str(), 1);
    }
#endif
    if (const char* logPath = getenv("EDEN_LOG")) {
        if (freopen(logPath, "w", stdout)) {
            dup2(fileno(stdout), fileno(stderr));
            setvbuf(stdout, nullptr, _IOLBF, 0);
            setvbuf(stderr, nullptr, _IOLBF, 0);
            std::fprintf(stderr, "[eden] logging to %s\n", logPath);
        }
    }
    if (const char* extra = getenv("EDEN_ARGS")) {
        std::fprintf(stderr, "[eden] EDEN_ARGS=%s\n", extra);
        if (int rc = parse_arg_string(extra)) return rc;
    }
#if defined(EDEN_PLATFORM_IOS)
    // THE ONE-SHOT ARGS FILE (2026-10-04) — EDEN_ARGS for a device driven over SSH. A SpringBoard
    // launch (`uiopen --bundleid`) carries no environment, so `Documents/eden-args.txt` is the only
    // way to put a jailbroken iPad into `--shot` or a selftest without Xcode. It is RENAMED to
    // eden-args.last.txt before it is parsed, so it applies to exactly one launch: a leftover file
    // must never trap the next home-screen tap in a harness mode. Parsed after EDEN_ARGS, same parser.
    {
        const std::string d = g_opt.docs.empty() ? default_docs() : g_opt.docs;
        const std::string f = d + "/eden-args.txt", used = d + "/eden-args.last.txt";
        if (std::rename(f.c_str(), used.c_str()) == 0) {
            std::string text;
            if (FILE* fp = std::fopen(used.c_str(), "r")) {
                char buf[1024];
                size_t n;
                while ((n = std::fread(buf, 1, sizeof(buf), fp)) > 0) text.append(buf, n);
                std::fclose(fp);
            }
            std::fprintf(stderr, "[eden] eden-args.txt=%s\n", text.c_str());
            if (int rc = parse_arg_string(text.c_str())) return rc;
            // --shot's captures land beside the log, where scp can reach them; the app's cwd
            // is not writable.
            if (g_opt.mode && !std::strcmp(g_opt.mode, "shot") && g_opt.shot.empty())
                g_opt.shot = d + "/eden-shot";
        }
    }
#endif
    return eden_main_after_args(argc, argv);
}

// Whitespace-separated options, each parsed exactly as one argv entry.
static int parse_arg_string(const char* text) {
    std::string acc;
    for (const char* p = text; ; ++p) {
        if (*p && !std::isspace((unsigned char)*p)) { acc += *p; continue; }
        if (!acc.empty()) { if (int rc = parse_one_arg(acc.c_str())) return rc; acc.clear(); }
        if (!*p) break;
    }
    return 0;
}

// The body of the loop above, verbatim from when it was one. Split out only so argv and EDEN_ARGS
// cannot drift into two parsers.
static int parse_one_arg(const char* a) {
    {
        if (!std::strcmp(a, "--headless"))            g_opt.headless = true;
        else if (!std::strcmp(a, "--p1-gate"))        { g_opt.mode = "p1-gate";  g_opt.headless = true; }
        else if (!std::strcmp(a, "--stage1"))         g_opt.mode = "stage1";
        else if (!std::strcmp(a, "--smoke"))          g_opt.mode = "smoke";
        else if (starts_with(a, "--save-bench="))     { g_opt.mode = "save-bench"; g_opt.headless = true; g_saveBenchList = a + 13; }
        else if (!std::strcmp(a, "--input-selftest")) { g_opt.mode = "input-selftest"; g_opt.headless = true; }
        else if (!std::strcmp(a, "--audio-selftest")) { g_opt.mode = "audio-selftest"; g_opt.headless = true; }
        else if (!std::strcmp(a, "--gamepad-selftest")) { g_opt.mode = "gamepad-selftest"; g_opt.headless = true; }
        else if (!std::strcmp(a, "--touch-selftest")) { g_opt.mode = "touch-selftest"; g_opt.headless = true; }
        else if (!std::strcmp(a, "--empty-bit-selftest")) { g_opt.mode = "empty-bit-selftest"; g_opt.headless = true; }
        else if (!std::strcmp(a, "--light-selftest")) { g_opt.mode = "light-selftest"; g_opt.headless = true; }
        else if (!std::strcmp(a, "--keybind-selftest")) { g_opt.mode = "keybind-selftest"; g_opt.headless = true; }
        else if (!std::strcmp(a, "--ui-selftest"))   { g_opt.mode = "ui-selftest"; g_opt.headless = true; }
        else if (!std::strcmp(a, "--browser-selftest")) { g_opt.mode = "browser-selftest"; g_opt.headless = true; }
        else if (!std::strcmp(a, "--net-live-selftest")) { g_opt.mode = "net-live-selftest"; g_opt.headless = true; }
        else if (starts_with(a, "--net-fixtures="))  eden_net_set_fixture_root(a + 15);
        else if (!std::strcmp(a, "--objc-selftest")) { g_opt.mode = "objc-selftest"; g_opt.headless = true; }
        else if (!std::strcmp(a, "--save-roundtrip")) g_opt.mode = "save-roundtrip";
        else if (!std::strcmp(a, "--background-selftest")) { g_opt.mode = "background-selftest"; g_opt.headless = true; }
        else if (!std::strcmp(a, "--shot"))           g_opt.mode = "shot";
        else if (starts_with(a, "--shot="))           { g_opt.mode = "shot"; g_opt.shot = a + 7; }
        else if (starts_with(a, "--height="))         g_opt.height = atoi(a + 9);
        else if (starts_with(a, "--empty-shortcut=")) { extern bool g_empty_shortcut; g_empty_shortcut = atoi(a + 17) != 0; }
        else if (std::strcmp(a, "--empty-selfcheck") == 0) eden_debug_set_empty_selfcheck(1);
        else if (std::strcmp(a, "--light-selfcheck") == 0) eden_debug_set_light_selfcheck(1);
        else if (starts_with(a, "--read-budget-bands=")) { extern bool g_read_budget_bands; g_read_budget_bands = atoi(a + 20) != 0; }
        else if (starts_with(a, "--world="))          g_opt.world = a + 8;
        else if (starts_with(a, "--frames="))         g_opt.frames = atoi(a + 9);
        else if (!std::strcmp(a, "--touch-profile")) g_opt.touchProfile = true;
        else if (starts_with(a, "--window=")) {
            // Phase N Stage 4.3. The letterbox fit can only be exercised where the window's aspect
            // differs from the engine's, and every default this harness has is 16:9 — which is the
            // reason the missing fit survived three stages. `--window=1024x768` reproduces an
            // iPad's 4:3 on a desktop, which is the only place it can be tested without hardware.
            int w = 0, h = 0;
            if (sscanf(a + 9, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
                g_opt.winW = w; g_opt.winH = h;
            }
        }
        else if (!std::strcmp(a, "--leak-probe"))     { g_opt.mode = "leak-probe"; g_opt.headless = true; }
        else if (starts_with(a, "--leak-probe="))     { g_opt.mode = "leak-probe"; g_opt.headless = true;
                                                        g_opt.cycles = atoi(a + 13); }
        else if (starts_with(a, "--cycles="))         g_opt.cycles = atoi(a + 9);
        else if (starts_with(a, "--docs="))           g_opt.docs = a + 7;
        else if (starts_with(a, "--bundle="))         g_opt.bundle = a + 9;
        else if (starts_with(a, "--live-cmds="))      g_opt.liveCmds = a + 12;
        else if (starts_with(a, "--render-scale="))   g_opt.renderScalePct = atoi(a + 15);
        else if (!std::strcmp(a, "--scale-probe"))    g_opt.mode = "scale-probe";
        else if (!std::strcmp(a, "--mem-trace"))      g_eden_mem_trace = 1;
        else if (starts_with(a, "--mem-trace="))      g_eden_mem_trace = atoi(a + 12);
        else if (starts_with(a, "--tex-exp=")) {
            // N.4.9 texture experiments (gl_fixed_function.cpp): comma list of nomips, storage,
            // noflush. Diagnostics for a device A/B. The per-upload flush has been the default
            // since N.4.9's fix, so `noflush` is the old behaviour; `flush` is accepted as a no-op.
            int bits = 0;
            for (const char* p = a + 10; *p; ) {
                const char* e = std::strchr(p, ',');
                const size_t n = e ? (size_t)(e - p) : std::strlen(p);
                if (n == 6 && !std::strncmp(p, "nomips", 6))       bits |= 1;
                else if (n == 7 && !std::strncmp(p, "storage", 7)) bits |= 2;
                else if (n == 5 && !std::strncmp(p, "flush", 5))   eden_gl_set_tex_flush(1);
                else if (n == 7 && !std::strncmp(p, "noflush", 7)) eden_gl_set_tex_flush(0);
                else { std::fprintf(stderr, "eden: unknown --tex-exp entry in '%s'\n", a); return 2; }
                p += n; if (*p == ',') ++p;
            }
#if !defined(EDEN_PLATFORM_IOS)
            if (bits & 2) std::fprintf(stderr, "eden: --tex-exp=storage is ES 3.0 (iOS) only; ignored here\n");
#endif
            eden_gl_set_tex_experiments(bits);
        }
        else if (starts_with(a, "--at=")) {
            if (sscanf(a + 5, "%f,%f,%f", &g_opt.at[0], &g_opt.at[1], &g_opt.at[2]) == 3)
                g_opt.haveAt = true;
        } else {
            std::fprintf(stderr, "eden: unknown option '%s' (see eden_main_native.cpp's header)\n", a);
            return 2;
        }
    }
    return 0;
}

static int eden_main_after_args(int argc, char** argv) {
    (void)argc;
    // `--touch-selftest --window=WxH` runs WINDOWED, and that is the whole point of the
    // combination: headless has no window, so the letterbox is degenerate and the mapping under
    // test is the fallback rather than the real one. With a window — and `--touch-profile` for the
    // touch layout, its UI scale and its on-screen joystick — this is an iPad's exact
    // configuration on a developer's desk.
    if (g_opt.mode && !std::strcmp(g_opt.mode, "touch-selftest")) {
#if defined(EDEN_PLATFORM_IOS)
        // ALWAYS windowed on iOS. There the window is the device's screen and the point space is
        // derived from it, so the whole window->point mapping is live — which is exactly what
        // broke, and testing the degenerate headless mapping there would prove nothing.
        g_opt.headless = false;
#else
        if (g_opt.winW > 0) g_opt.headless = false;
#endif
    }

    if (g_opt.mode && !g_opt.haveAt) {
        // A FIXED default origin: the engine spawns a new world at a hashed position, and chunk
        // contents are a function of position, so a run that did not teleport would checksum
        // something different every time.
        //
        // It is NOT, as this comment claimed until Stage 3, "inside the bundled map". 1440 is
        // outside the playable square on both axes and Player::preupdate clamps it to the map's
        // south-west CORNER — see teleport_for_movement() for the arithmetic and for what that
        // cost. Fixed and reproducible is all the checksum modes need, so the value stays and the
        // recorded fingerprints stay with it; the movement selftests go to the centre instead.
        g_opt.at[0] = 1440.0f; g_opt.at[1] = 40.0f; g_opt.at[2] = 1440.0f;
    }

    const std::string docs   = g_opt.docs.empty()   ? default_docs()            : g_opt.docs;
    const std::string bundle = g_opt.bundle.empty() ? default_bundle(argv[0])   : g_opt.bundle;
    mkdirs(docs);
    eden_platform_set_roots(docs.c_str(), bundle.c_str());
    migrate_legacy_saves(docs);

    // The previous-save slot. Stage 1 left native WITHOUT one, and that was a real regression
    // against web rather than a missing nicety: web gets it from the Foundation shim's
    // NSFileHandle, and this target links Apple's real Foundation, whose NSFileHandle is a class
    // cluster with `-writeData:` on a private subclass — so there is no honest place to swizzle
    // it. The engine grew a hook at the top of FileManager::saveWorld instead, and both targets
    // run the identical rules (web/src/shim/foundation/save_backup.cpp). Installed here rather
    // than inside the seam file so the file that OWNS the decision is the one that shows it.
    //
    // Narrower than web's, and knowingly so: the shim guards every file open, including the C3
    // rollback journal's, while this covers the world file at save time. That is the case the
    // slot exists for; the journal has its own recovery.
    eden_save_backup_hook = eden_save_backup_before_overwrite;
    // ...and the save DIRECTORY. Without this, Classes/FileManager.mm asks real Foundation and
    // gets ~/Documents; the seam's value (and therefore --docs=) would be set and ignored, which
    // is the state Stage 1 shipped in. eden_platform_documents_root() outlives the process, which
    // is what the hook's contract requires.
    eden_documents_root_hook = eden_platform_documents_root;
    // The seam value IS what the engine uses now — Classes/FileManager.mm asks
    // eden_platform_documents_root() rather than NSSearchPathForDirectoriesInDomains (Phase N
    // Stage 2). Through Stage 1 those were different things on this target and printing the seam
    // value here would have been a lie with a plausible shape; that is what changed.
    @autoreleasepool {
        std::fprintf(stderr, "[eden] documents=%s\n[eden] bundle=%s\n",
                     docs.c_str(), [[[NSBundle mainBundle] resourcePath] UTF8String]);
    }

    if (g_opt.headless) eden_native_gl_set_headless(1);
    // Audio off for every scripted mode, not just the headless ones. --stage1 and
    // --input-selftest run thousands of frames in a few seconds, so real playback would be every
    // sound the engine fires, decoded and queued at ~50x real time — and --smoke/--shot open a
    // window but are still measurements, not a session.
    if (g_opt.mode && std::strcmp(g_opt.mode, "audio-selftest") != 0)
        eden_native_audio_set_enabled(0);

    // The EVENTS subsystem, always. Video is initialised later by eden_gl_context_create() and only
    // when there is a window — but --input-selftest is headless and still needs a live event queue
    // for SDL_PushEvent/SDL_PollEvent to carry its synthetic keystrokes through the real pump.
    if (!SDL_InitSubSystem(SDL_INIT_EVENTS)) {
        std::fprintf(stderr, "[eden] SDL_InitSubSystem(EVENTS) failed: %s\n", SDL_GetError());
    }

    // Phase N Stage 4.2 — the mobile half of save-on-background, and it CANNOT go in the poll loop
    // below. SDL_events.h says so of these four events in as many words ("This event must be
    // handled in a callback set with SDL_AddEventWatch()"): on iOS they are delivered synchronously
    // from inside UIKit's applicationWillResignActive:/DidEnterBackground:, and by the time the
    // main loop next reaches SDL_PollEvent the OS may already have suspended — or killed — the
    // process. A watch runs on the spot, which is the only place a save is still possible. Desktop
    // never fires these, so this costs one function pointer there. (SDL_EVENT_QUIT stays in the
    // poll loop: it is a normal event and the loop must also stop running.)
    SDL_AddEventWatch(eden_lifecycle_event_watch, nullptr);

    // The GL context MUST be live before eden_seam_main(), and that is measured rather than
    // preferred: World::World() -> Graphics::initGraphics() issues real glGenBuffers/glBufferData
    // during construction. A failure here is NOT fatal — the shim's context guard keeps the whole
    // engine running headless, which is exactly what the scripted modes want.
    eden_mem_trace_install();
    eden_mem_trace("boot");
    eden_gl_context_create(g_opt.winW, g_opt.winH);

    // N.4.10 (2026-10-04): native never calls eden_set_low_memory(), on purpose — the decision,
    // not an omission. On web it refuses 256z worlds and seeds 1x DPR / 75% render scale / 45 fps.
    // On the 2 GB iPad Air 2, once N.4.9's texture flush landed, the F.3 walk peaks at 206 MB at
    // 64z and 252 MB at 256z, so the refusal would protect nothing. And 75% render scale on native
    // ADDS an offscreen framebuffer (N.4.11) rather than shrinking the drawable. Revisit only on a
    // device that measures over criterion 3 (300 MB) with the fix in; the predicate would then be
    // SDL_GetSystemRAM(). Never LOW_MEM_DEVICE: that turns coloured lighting off.
    eden_native::eden_seam_main();
    g_app = eden_native::eden_seam_get_app_delegate();
    eden_mem_trace("world-constructed");
    eden_native_input_init(eden_native_gl_window());

    // Must be BEFORE any world is meshed: prepareVBO() fingerprints a chunk only if this is on at
    // publish time, and it is opt-in because it costs a pass over every published vertex. Scripted
    // modes only — an interactive session should not pay for it.
    if (g_opt.mode) eden_debug_set_mesh_checksum(1);

    // NOT ENABLED: `g_app->viewController.setFixedEtime(1.0f/60.0f)` for the scripted modes. It is
    // the obviously right idea — "N frames" should be N/60 s of engine time regardless of machine
    // load — and it was tried here. It does not work on its own, because this build has THREE time
    // bases and only one of them would move: the engine's etime (fixed), Movement_web.mm's
    // frame-rate-normalisation EMA (wall clock, so with frames flying past it pins at its 4x clamp
    // and the player walks 500 blocks in 300 frames), and the input translator's hold-to-act repeat
    // (wall clock, so the number of mine pulses per second of engine time collapses). Making the
    // harness deterministic means putting all three on the same clock, which is a real piece of
    // design and not a line of setup. The lever is left in place, documented, and off — see
    // EdenViewController_native::setFixedEtime().

    // The settings model. This was the "NOT CALLED YET, DELIBERATELY" block through commit
    // d8e815f, because adding eden_settings_init() broke --input-selftest's "W moves the player"
    // check on 4 runs of 4. THE CAUSE WAS THE SELF-TEST, NOT THE SETTINGS.
    //
    //   * The check teleports to a fixed origin that lands the player INSIDE terrain, and got out
    //     of it by holding Space — which lifts you only because this fork ships FLY_MODE=true
    //     (Classes/Player.mm:32). The `fly` row's schema default is 0, so loading the settings
    //     correctly turned fly OFF, Space became an ordinary jump, the player stayed embedded in
    //     the hill and walked exactly 0.00 blocks. A buried player who cannot walk is the engine
    //     behaving; the harness was asserting on a default it never declared.
    //   * That is also why eden_set_detected_touch(0) "helped" (1 of 3 rather than 0 of 4): it
    //     changes the active display profile, hence the point space, hence nothing about movement
    //     — it just perturbed where the settle ended up. There was never a race.
    //
    // run_input_selftest() now forces fly on through the real V binding before it climbs, so it
    // asserts on the wiring rather than on a default. GENERALISES: when enabling a subsystem
    // "breaks" a test, check whether the test was depending on that subsystem being absent.
    //
    // Two host facts have to be stated before the model loads, because both are properties of
    // this build rather than preferences:
    eden_ui_force_legacy(1);        // there is no DOM UI here; the GL menus are the only menus
    // ...and iOS is the one target where the second fact inverts. Phase N Stage 4.1: this read
    // `eden_set_detected_touch(0)` unconditionally, which was true of the three desktop legs and
    // is a lie on a phone — the profile it selects lays the HUD out for a pointer on a device that
    // has none. Stage 2 paid for this class of mistake once already (the ESC menu drew nothing
    // because `legacy_menu` defaulted to a value chosen for a DOM host): EVERY port-owned default
    // was chosen against some other host, so audit them rather than inherit them.
    //
    // `--touch-profile` forces the same choice on a desktop run, and it is a testing tool with a
    // real job: the profile decides the LAYOUT — its UI scale, its on-screen joystick, and (until
    // 2026-09-07, when the device reported black bars) whether the aspect was pinned to 16:9. A
    // desktop window is not a phone, so without this flag an iPad's configuration cannot be
    // reproduced anywhere a developer can see it — which is most of why the missing viewport call
    // survived three stages.
#if defined(EDEN_PLATFORM_IOS)
    eden_set_detected_touch(1);
#else
    eden_set_detected_touch(g_opt.touchProfile ? 1 : 0);
#endif
    settings_pump();                // may be too early (needs menu->settings); tick() retries

    int rc = 0;
    if (g_opt.mode) {
        if (!std::strcmp(g_opt.mode, "p1-gate"))            rc = run_p1_gate();
        else if (!std::strcmp(g_opt.mode, "stage1"))        rc = run_stage1();
        else if (!std::strcmp(g_opt.mode, "smoke"))         rc = run_smoke();
        else if (!std::strcmp(g_opt.mode, "save-bench"))    rc = run_save_bench();
        else if (!std::strcmp(g_opt.mode, "input-selftest")) rc = run_input_selftest();
        else if (!std::strcmp(g_opt.mode, "keybind-selftest")) rc = run_keybind_selftest();
        else if (!std::strcmp(g_opt.mode, "ui-selftest")) rc = run_ui_selftest();
        else if (!std::strcmp(g_opt.mode, "browser-selftest")) rc = run_browser_selftest(false);
        else if (!std::strcmp(g_opt.mode, "net-live-selftest")) rc = run_browser_selftest(true);
        else if (!std::strcmp(g_opt.mode, "audio-selftest")) rc = run_audio_selftest();
        else if (!std::strcmp(g_opt.mode, "gamepad-selftest")) rc = run_gamepad_selftest();
        else if (!std::strcmp(g_opt.mode, "touch-selftest")) rc = run_touch_selftest();
        else if (!std::strcmp(g_opt.mode, "empty-bit-selftest")) rc = run_empty_bit_selftest();
        else if (!std::strcmp(g_opt.mode, "light-selftest")) rc = run_light_selftest();
        else if (!std::strcmp(g_opt.mode, "objc-selftest")) rc = run_objc_selftest();
        else if (!std::strcmp(g_opt.mode, "save-roundtrip")) rc = run_save_roundtrip();
        else if (!std::strcmp(g_opt.mode, "background-selftest")) rc = run_background_selftest();
        else if (!std::strcmp(g_opt.mode, "shot"))          rc = run_shot();
        else if (!std::strcmp(g_opt.mode, "scale-probe"))   rc = run_scale_probe();
        else if (!std::strcmp(g_opt.mode, "leak-probe"))    rc = run_leak_probe();
    } else {
        // Interactive. The frame-rate cap is the same setting the web build's frame gate reads
        // (Settings_web.mm's eden_get_fps_cap), but it is applied as PACING — sleep until the next
        // frame is due, then update and draw together — and not as web's "skip the draw, still run
        // update" gate. Web can afford that gate because rAF only fires at the display rate, so a
        // skipped draw still leaves one engine tick per display frame. This loop has no rAF: until
        // 2026-09-25 it ran `drawFrame(false)` + SDL_Delay(1) on every capped iteration, i.e. ~540
        // engine ticks/s against 58 draws/s under the touch profile's default 60 fps cap. The engine
        // cannot tick that fast (N.4.7, "walking barely moves on iOS"): ground friction is
        // per-TICK (Player.mm, vel*=.90), so walk speed falls with the tick rate, and positions
        // near the map centre are float32 at ~65536 where one ULP is 1/128 block — a 1.8 ms tick's
        // walking step is below half of that and `pos += vel*etime` rounds to no movement at all.
        // Fly mode (0.995 damping, far higher speed) cleared the ULP, which is why only walking died.
        //
        // Audit row A4 (skipping the whole tick dropped input on capped frames) does not come back:
        // nothing is skipped. Events that arrive while we sleep stay in SDL's queue and are pumped
        // into the same tick they would have reached anyway. Paced to a fixed deadline schedule, not
        // "interval since the last frame", so the average rate is the cap exactly; the slack lets a
        // frame start a little early (the vsync-blocking swap absorbs it) so that a cap equal to the
        // display rate never sleeps past a vsync and halves the frame rate.
        constexpr double kPaceSlackMs = 2.0;
        double nextFrameMs = 0.0;
#if defined(EDEN_DIAGNOSTICS)
        if (!g_opt.liveCmds.empty()) eden_live_commands_init(g_opt.liveCmds.c_str());
#endif
        while (g_running) {
            const int capFps = eden_get_fps_cap();
            if (capFps > 0) {
                const double interval = 1000.0 / (double)capFps;
                const double now = eden_platform_now_ms();
                // First frame, a cap change, or a stall (a slow load, a backgrounded app): restart
                // the schedule rather than rushing frames out to catch up.
                if (nextFrameMs == 0.0 || now - nextFrameMs > interval || nextFrameMs - now > interval)
                    nextFrameMs = now;
                const double wait = nextFrameMs - now - kPaceSlackMs;
                if (wait > 0.0) SDL_DelayPrecise((Uint64)(wait * 1e6));
                nextFrameMs += interval;
            } else {
                nextFrameMs = 0.0;
            }
            pump_events();
            if (!g_app) break;
            settings_pump();
            // BEFORE drawFrame: World::update() is what consumes the queued touches, so input
            // pushed after it would land a frame late — the same ordering public/eden-input.js
            // gets by running inside the rAF callback ahead of the engine tick.
            eden_native_input_tick();
#if defined(EDEN_DIAGNOSTICS)
            eden_live_commands_tick();
#endif
            if (g_app->viewController.isAnimating()) g_app->viewController.drawFrame(true);
            // AFTER the tick that queued this frame's sounds, so a sound fired now is bound to
            // the device in the same frame rather than the next one.
            eden_native_audio_tick();
            eden_heap_pressure_tick();
        }
    }

    eden_native_audio_shutdown();
    eden_gl_context_destroy();
    SDL_Quit();
#if defined(EDEN_PLATFORM_IOS)
    // ON iOS, RETURNING FROM main() DOES NOT END THE PROCESS — and the first CI run of the
    // simulator gates is what proved it. The app booted, decoded all 874 bundle assets, ran its
    // three [eden-p1] ticks with glErr=0 and then sat there: SDL's UIKit delegate calls SDL_main
    // from -postFinishLaunch and, when it returns, simply goes back to the UIKit run loop, which
    // is correct for an app someone tapped and wrong for a harness. `simctl launch --console-pty`
    // waits for the process, so a passing gate looked exactly like a hang for seven minutes.
    //
    // exit() rather than a request to UIKit to terminate: this build has no scene lifecycle to
    // unwind and everything above has already shut down. Unconditional, not `if (g_opt.mode)` —
    // an interactive session only reaches this line because g_running went false, i.e. the player
    // asked to quit, and an iOS app that will not close is worse than one that exits abruptly.
    std::fflush(nullptr);
    exit(rc);
#endif
    return rc;
}
