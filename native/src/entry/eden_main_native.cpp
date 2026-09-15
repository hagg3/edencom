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
//                        PREFIX-menu.bmp / -hud.bmp / -pausemenu.bmp (default prefix eden-shot).
//                        The GL UI is native's only UI until Stage 5, so "what does it look like"
//                        needs an artefact; --smoke only answers "did it draw at all".
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

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <cmath>
#include <sys/stat.h>
#include <dirent.h>
#include <cerrno>
#include <cctype>
#include <unistd.h>

// Shared seam exports (web/src/seam/*, compiled into this target too).
extern "C" {
int   eden_get_fps_cap(void);
void  eden_settings_init(void);
void  eden_set_detected_touch(int isTouch);
int   eden_settings_loaded(void);
void  eden_heap_pressure_tick(void);
void  eden_native_gl_set_headless(int on);
void  eden_native_gl_set_present(int on);
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
void  eden_debug_set_mesh_checksum(int on);
const char* eden_debug_world_format(void);
const char* eden_debug_heap(void);
void  eden_debug_heap_reset_peak(void);
void  eden_native_apply_display_mode(void);   // src/seam/DisplayMode_native.cpp
}

extern float SCREEN_WIDTH;
extern float SCREEN_HEIGHT;

// Settings_web.mm's mirrored gamepad rows; --gamepad-selftest reports the live deadzone.
extern float eden_gamepad_deadzone;

// Classes/Alert.h — the native seam (seam_link_stubs_native.mm) implements this as a GLDialog.
void showAlertWarpHome();

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
void settings_pump() {
    if (!eden_settings_loaded()) eden_settings_init();
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
    for (int i = 0; i < timeoutFrames; ++i) {
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
    std::printf("[eden-stage1] %s.geometry %s\n", phase, eden_debug_terrain_geometry());
    std::printf("[eden-stage1] %s.checksum %s\n", phase, eden_debug_mesh_checksum());
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

int run_shot() {
    const char* name = g_opt.world.empty() ? "shot" : g_opt.world.c_str();
    tick(180);
    capture("menu");

    // The rewritten generic GL settings screen (Phase N Stage 2.5). Meaningful only from the main
    // menu (in game the panel is a host overlay); shot here before any world loads.
    eden_settings_menu_open_now();
    tick(30);
    capture("settings");
    eden_settings_menu_close();
    tick(15);

    if (!open_world(name, g_opt.height)) return 1;
    eden_console_teleport(g_opt.at[0], g_opt.at[1], g_opt.at[2]);
    tick(g_opt.frames);
    capture("hud");

    // The in-game (ESC) panel, through the same synthetic HUD tap Escape uses. This is the shot
    // that would have caught the suppressed-panel bug (eden_ui_force_legacy) at Stage 2's start
    // instead of at the first play pass.
    eden_tap_hud_button_begin(0);
    tick(6);
    eden_tap_hud_button_end(0);
    tick(30);
    std::printf("[eden-shot] in_menu=%d\n", eden_hud_in_menu());
    capture("pausemenu");

    // The GL modal (Phase N Stage 2.5) — the ESC menu's Home button raises this. Shown directly
    // here rather than hunting the icon's rect; the point is the artefact.
    ::showAlertWarpHome();
    tick(20);
    capture("dialog");
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
            check(peak <= first * 1.02f && last < first * 0.9f,
                  "lifting the finger stops driving the player", detail);
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
        std::string acc;
        for (const char* p = extra; ; ++p) {
            if (*p && !std::isspace((unsigned char)*p)) { acc += *p; continue; }
            if (!acc.empty()) { if (int rc = parse_one_arg(acc.c_str())) return rc; acc.clear(); }
            if (!*p) break;
        }
    }
    return eden_main_after_args(argc, argv);
}

// The body of the loop above, verbatim from when it was one. Split out only so argv and EDEN_ARGS
// cannot drift into two parsers.
static int parse_one_arg(const char* a) {
    {
        if (!std::strcmp(a, "--headless"))            g_opt.headless = true;
        else if (!std::strcmp(a, "--p1-gate"))        { g_opt.mode = "p1-gate";  g_opt.headless = true; }
        else if (!std::strcmp(a, "--stage1"))         g_opt.mode = "stage1";
        else if (!std::strcmp(a, "--smoke"))          g_opt.mode = "smoke";
        else if (!std::strcmp(a, "--input-selftest")) { g_opt.mode = "input-selftest"; g_opt.headless = true; }
        else if (!std::strcmp(a, "--audio-selftest")) { g_opt.mode = "audio-selftest"; g_opt.headless = true; }
        else if (!std::strcmp(a, "--gamepad-selftest")) { g_opt.mode = "gamepad-selftest"; g_opt.headless = true; }
        else if (!std::strcmp(a, "--touch-selftest")) { g_opt.mode = "touch-selftest"; g_opt.headless = true; }
        else if (!std::strcmp(a, "--objc-selftest")) { g_opt.mode = "objc-selftest"; g_opt.headless = true; }
        else if (!std::strcmp(a, "--save-roundtrip")) g_opt.mode = "save-roundtrip";
        else if (!std::strcmp(a, "--background-selftest")) { g_opt.mode = "background-selftest"; g_opt.headless = true; }
        else if (!std::strcmp(a, "--shot"))           g_opt.mode = "shot";
        else if (starts_with(a, "--shot="))           { g_opt.mode = "shot"; g_opt.shot = a + 7; }
        else if (starts_with(a, "--height="))         g_opt.height = atoi(a + 9);
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
    eden_gl_context_create(g_opt.winW, g_opt.winH);

    eden_native::eden_seam_main();
    g_app = eden_native::eden_seam_get_app_delegate();
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
        else if (!std::strcmp(g_opt.mode, "input-selftest")) rc = run_input_selftest();
        else if (!std::strcmp(g_opt.mode, "audio-selftest")) rc = run_audio_selftest();
        else if (!std::strcmp(g_opt.mode, "gamepad-selftest")) rc = run_gamepad_selftest();
        else if (!std::strcmp(g_opt.mode, "touch-selftest")) rc = run_touch_selftest();
        else if (!std::strcmp(g_opt.mode, "objc-selftest")) rc = run_objc_selftest();
        else if (!std::strcmp(g_opt.mode, "save-roundtrip")) rc = run_save_roundtrip();
        else if (!std::strcmp(g_opt.mode, "background-selftest")) rc = run_background_selftest();
        else if (!std::strcmp(g_opt.mode, "shot"))          rc = run_shot();
        else if (!std::strcmp(g_opt.mode, "leak-probe"))    rc = run_leak_probe();
    } else {
        // Interactive. The frame-rate cap is the same setting the web build's frame gate reads
        // (Settings_web.mm's eden_get_fps_cap), applied the same way: it decides whether the frame
        // DRAWS, never whether update runs — audit row A4, where skipping the whole tick dropped
        // input on capped frames.
        double lastRenderMs = 0.0;
        while (g_running) {
            pump_events();
            if (!g_app) break;
            settings_pump();
            bool renderThisFrame = true;
            const int capFps = eden_get_fps_cap();
            if (capFps > 0) {
                const double now = eden_platform_now_ms();
                const double minInterval = 1000.0 / (double)capFps;
                if (lastRenderMs != 0.0 && now - lastRenderMs < minInterval) renderThisFrame = false;
                else lastRenderMs = now;
            }
            // BEFORE drawFrame: World::update() is what consumes the queued touches, so input
            // pushed after it would land a frame late — the same ordering public/eden-input.js
            // gets by running inside the rAF callback ahead of the engine tick.
            eden_native_input_tick();
            if (g_app->viewController.isAnimating()) g_app->viewController.drawFrame(renderThisFrame);
            // AFTER the tick that queued this frame's sounds, so a sound fired now is bound to
            // the device in the same frame rather than the next one.
            eden_native_audio_tick();
            eden_heap_pressure_tick();
            if (!renderThisFrame) SDL_Delay(1);   // vsync only blocks on frames that swap
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
