// G.4: the harness core, split out of the old eden_main_native.cpp (see harness.h).
#include "harness.h"

namespace harness {


void eden_setenv(const char* k, const char* v) {
#if defined(_WIN32)
    _putenv_s(k, v);
#else
    setenv(k, v, 1);
#endif
}


Options g_opt;
int g_meshBenchPasses = 0;                          // --mesh-bench=N (was in mode_boot)
std::string g_saveBenchList = "1,10,50,144";      // --save-bench=LIST

eden_native::EdenAppDelegate* g_app = nullptr;

// Phase N Stage 4.2 — see the SDL_AddEventWatch() call in main() for why this is a watch and not a
// case in the poll loop. A named SDLCALL function rather than a lambda because SDLCALL is a real
// calling convention on the Windows toolchains Stage 3 ships, and a converted lambda's is whatever
// the compiler's default happens to be.
bool SDLCALL eden_lifecycle_event_watch(void*, SDL_Event* ev) {
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
    // --render-scale=N is applied the first frame the model has loaded (before then a write would
    // be overwritten by the stored value). It is an ordinary settings write, so it persists the
    // way a player's choice in the Settings screen would.
    static bool scaleApplied = false;
    if (!scaleApplied && g_opt.renderScalePct > 0 && eden_settings_loaded()) {
        scaleApplied = true;
        set_render_scale_pct(g_opt.renderScalePct);
    }
    // I.0: the stored prefs load after startup and would put a saved Input mode back over --touch-profile.
    static bool touchForced = false;
    if (!touchForced && g_opt.touchProfile && eden_settings_loaded()) {
        touchForced = true;
        eden_test_force_touch_input();
    }
    static bool setsApplied = false;
    if (!setsApplied && !g_opt.sets.empty() && eden_settings_loaded()) {
        setsApplied = true;
        for (auto& kv : g_opt.sets)
            if (!set_setting_by_key(kv.first.c_str(), kv.second))
                std::fprintf(stderr, "[eden] --set: no setting %s\n", kv.first.c_str());
    }
}

void tick(int n) {
    if (!g_app) return;
    for (int i = 0; i < n; ++i) {
        pump_events();
        settings_pump();
        if (g_tickInput) eden_native_input_tick();
        eden_look_trace_frame();
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
    std::printf("[eden-stage1] %s.shadow %s\n", phase, eden_debug_shadow_state());
    std::printf("[eden-stage1] %s.format %s\n", phase, eden_debug_world_format());
    std::fflush(stdout);
}

// Create the named world if the menu does not already list it, then play it. Mirrors what
// public/eden-menu.js does and what the headless probes do: set the pending world TYPE and HEIGHT
// first (the engine asks for both at Play time, not at create time — see Menu.cpp), then
// create, select and play.
bool open_world(const char* displayName, int height) {
    int idx = -1;
    const int count = eden_menu_world_count();
    for (int i = 0; i < count; ++i) {
        if (std::strcmp(eden_menu_world_name(i), displayName) == 0) { idx = i; break; }
    }
    // X.16: `_` may stand for a space (args are whitespace-split), tried only when the literal name
    // is not a world: --world=Quarry_Town_V24I opens "Quarry Town V24I".
    std::string spaced;
    if (idx < 0 && std::strchr(displayName, '_')) {
        spaced = displayName;
        for (size_t k = 0; k < spaced.size(); ++k) if (spaced[k] == '_') spaced[k] = ' ';
        for (int i = 0; i < count; ++i)
            if (std::strcmp(eden_menu_world_name(i), spaced.c_str()) == 0) { idx = i; displayName = spaced.c_str(); break; }
    }
    const Uint64 tOpen0 = SDL_GetTicks();
    // "Normal" (not flat) so the world streams from the bundled Eden.eden — which is the whole
    // point for criterion 2: the default world is a fixed, shipped 2880x2880 map, so the same
    // origin yields the same blocks on every platform and every run. A flat world would compare
    // equal for the trivial reason that it is empty.
    eden_menu_set_pending_world_type(0);
    eden_menu_set_pending_world_height(height);
    // H.4: --classic=SEED[,SCALE4] makes a world this call creates a Classic one (new profile,
    // the New World tab's defaults), as the tab's Play would; an existing world keeps its own.
    unsigned char rec[CLASSIC_RECORD_BYTES];
    const bool classic = idx < 0 && !g_opt.classicNew.empty();
    if (classic) {
        ClassicGen g;
        int seed = 0, scale4 = 4, caves = -1, hills = -1, trees = -1;
        std::sscanf(g_opt.classicNew.c_str(), "%d,%d,%d,%d,%d", &seed, &scale4, &caves, &hills, &trees);
        classic_default_params(&g, seed);
        if (height == 256) g.scale4 = (unsigned char)scale4;
        if (caves >= 0 && caves <= 2) g.caves = (unsigned char)caves;     // X.13: 0 Off, 1 2011, 2 Deep
        if (hills >= 0 && hills <= 4) g.hilliness = (unsigned char)hills;
        if (trees >= 0 && trees <= 3) g.trees = (unsigned char)trees;
        classic_encode_record(g, rec);
    }
    eden_menu_set_pending_generator(classic ? rec : nullptr);
    if (idx < 0) {
        char* buf = eden_menu_name_buffer();
        std::snprintf(buf, 128, "%s", displayName);
        idx = eden_menu_create_world();
        if (idx < 0) { std::fprintf(stderr, "[eden-stage1] create_world failed\n"); return false; }
    }
    eden_menu_select(idx);
    if (eden_menu_play() != 1) { std::fprintf(stderr, "[eden-stage1] menu_play refused\n"); return false; }
    const bool ok = tick_until([] { return game_mode() == 1; }, 60000, "game_mode == PLAY");
    std::printf("[eden-stage1] open_world \"%s\" %s first-load %llu ms\n", displayName, classic ? "(created Classic)" : "", (unsigned long long)(SDL_GetTicks() - tOpen0));
    std::fflush(stdout);
    return ok;
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

// I.3: every kit control drawn on a screen of --shot's tour lies inside the safe rect (the screen,
// where there are no insets). Run at a device preset (`--fit-selftest --device=iphone16`). The pause
// menu fits a phone since I.4 (two columns when one does not fit), so it is a plain pass too; the
// overflow bookkeeping below stays as the gate's teeth should the column ever outgrow the screen.

// ---------------------------------------------------------------------------------------------
// Interactive loop
// ---------------------------------------------------------------------------------------------

bool g_running = true;
// G.5b-0: --scene-shot's captures must not depend on what the person at the machine does while the
// window is up. Set, pump_events drops every input event and the focus changes (a focus loss pauses
// the game), so a click or a keystroke elsewhere cannot press a HUD button or skip frames.
bool g_ignoreUserInput = false;

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
        if (g_ignoreUserInput && e.type != SDL_EVENT_QUIT && e.type != SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED &&
            e.type != SDL_EVENT_WINDOW_SAFE_AREA_CHANGED)
            continue;
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
            case SDL_EVENT_WINDOW_SAFE_AREA_CHANGED:   // I.2: rotation, island side, home bar
                eden_native_apply_display_mode();
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
    // G.2b-5: SDL_GetBasePath() is that same NSBundle resourcePath on iOS (with a trailing '/').
    if (const char* base = SDL_GetBasePath()) {
        std::string rp = base;
        while (rp.size() > 1 && rp.back() == '/') rp.pop_back();
        if (!rp.empty()) return rp;
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
        if (stat((dir + "/Eden.emod").c_str(), &st) == 0) return dir;
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
    // src/seam/Prefs_native.cpp already puts the prefs file. One "Emod"
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
// directory a user's official Eden files would use. See port/src/eden_app_identity.h for the
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
// footnote. Same substitution as src/seam/Prefs_native.cpp.
void mkdirs(const std::string& path) {
    SDL_CreateDirectory(path.c_str());
}

bool starts_with(const char* s, const char* p) { return std::strncmp(s, p, std::strlen(p)) == 0; }


}  // namespace harness
