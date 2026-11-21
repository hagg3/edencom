// LiveCommands_native.cpp — a scripted-build feed for an INTERACTIVE native session.
// `--live-cmds=<file>` makes the interactive loop tail <file> and run the commands appended to it,
// so a script outside the process can edit the world while someone watches the window. It is the
// native counterpart of web's dev console (public/eden-console.js → DevConsole.cpp), which native
// has no DOM to host; like that console it is EDEN_DIAGNOSTICS-only (CMakeLists.txt) and has no
// place in a build meant to be played.
//
// The file is a plain text file, not a FIFO: the reader keeps its own offset and re-reads whatever
// has been appended since the last frame, so the writer can be `echo >>`, a script, or several of
// them in turn, and nothing blocks if no writer is attached. One command per line; '#' comments.
//
//   open <world name>          open (creating if absent, "normal" type) a world from the main menu
//   set  x y z type [color]    one block — VECTOR order (y up), NOT Terrain's (x,z,y)
//   tp   x y z [yaw pitch]     move the player (and optionally the camera)
//   look yaw pitch
//   rate n                     max `set`s per frame (default 40) — what makes a build watchable
//   wait n                     pause the queue for n frames
//   save                       FileManager::saveWorld(), what the HUD's save button calls
//   say  text                  print to stdout
//   press x y z                push the command block there (D.4c's @touch), Vector order
//   cells x y z x2 y2 z2       print every cell of the box as type:colour (Vector order, x fastest
//                              then z, then y), one `[live] cells` line
//   cmdprobe                   print CmdScript::describe() (live / started / refused ... counts)
//   cmdtrace 0|1               print every command-block statement as it runs
//   photo                      take the F2 screenshot (Hud's camera path): the frame becomes the
//                              world's thumbnail, <world file>.png beside it in Documents
//   shot NAME [KBTOP]          EX.1-0: the next frame WITH its UI (menus, dialogs, a mod's eden.menu/ask/input)
//                              as NAME.bmp (".bmp" added if missing; relative to the cwd; the dir must exist),
//                              the same capture as --shot's (drawn, read back, not presented). KBTOP (0..1) fakes
//                              an on-screen keyboard covering that fraction of the screen from the bottom
//                              (eden_text_input_keyboard_top's point-space, y-up answer), for 6 frames first so a
//                              screen that lays out from it re-lays out (the Console, the server form; a GLDialog
//                              prompt such as eden.input is lifted by the iOS view instead and does not move on a
//                              desktop); the queue waits.
//                              Prints `[live] shot NAME.bmp WxH` (or `[live] shot failed: ...`). Works in menus too.
//   key NAME                   EX.1-0: press and release one key (an SDL scancode name: escape, return, tab, up, a ...)
//                              through the event queue, as the keyboard would: `key escape` picks a dialog's safe
//                              (last) button, cancels an eden.input, closes a mod menu.
//   modspage                   EX.1-0: open the pause menu's Mods page (ModScript::openModsPage), as its button does
//   installzip PATH            EX.1-0: install a pack zip into Documents/Packs as Mods > Install from file does
//                              (PackInstall::installZip, no sha256, no expected id); prints `[live] installzip ok
//                              <id> <version> -> Packs/<folder>` or `[live] installzip refused: <why>` and the
//                              validator's lines. Works on the main menu: the pack loads at the next world entry.
//   /<console line>            a Console line (Classes/Console.h) with the anchor fixed at =0 =0 =0, so
//                              relative = absolute; output echoes as `[live] <text>` (errors `[live] ! <text>`).
//                              `/cells` and `/quit` are the harness-only console verbs registered below.
//   quit                       end the session (an SDL quit event: what closing the window does)
//
// `set` goes through Terrain::updateChunks + setColor — the same pair buildBlock ends in — and, for
// a lightbox, the addlight + refreshChunksInRadius buildBlock does first (DevConsole's setblock
// skips that, so its lightboxes are dark until the next load re-derives lights).

#include "../../../port/src/shim/foundation/eden_base.h"
#include "../../../port/src/shim/foundation/eden_image.h"
#include "../../../Classes/World.h"
#include "../../../Classes/Constants.h"
#include "../../../Lighting.h"   // LM.1: light_refreshRadius
#include "../../../Classes/CmdScript.h"
#include "../../../Classes/Console.h"
#include "../../../Classes/ModScript.h"  // EX.1-0: modspage
#include "../../../Classes/PackInstall.h" // EX.1-0: installzip
#include "../../../Classes/Renderer.h"   // EX.1-0: R::readPixels
#include "../render/gl/gl_context.h"     // EX.1-0: eden_gl_context_get_drawable_size
#include <SDL3/SDL.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

void power_parse_exp(const char* list);   // harness/power.cpp (PW.0b)
void eden_native_debug_set_keyboard_top(float top);   // Input_native.cpp
namespace harness { namespace g_shots { bool write_bmp(const char* path, int w, int h, const std::vector<unsigned char>& rgba); } }

extern "C" {
int   eden_menu_world_count(void);
const char* eden_menu_world_name(int index);
void  eden_menu_select(int index);
int   eden_menu_play(void);
char* eden_menu_name_buffer(void);
int   eden_menu_create_world(void);
void  eden_menu_set_pending_world_type(int flat);
void  eden_menu_set_pending_world_height(int height);
const char* eden_debug_menu_state(void);
void  eden_take_screenshot(void);
void  eden_native_gl_set_present(int on);
const char* eden_platform_documents_root(void);
}
void addlight(int xx, int zz, int yy, float brightness, Vector color);
extern Vector colorTable[256];

namespace {
std::string g_path;
long g_offset = 0;
std::string g_partial;
std::deque<std::string> g_queue;
int g_rate = 40;
int g_wait = 0;
bool g_awaitPlay = false;
// EX.1-0 `shot`: frames left before the capture frame (-1 = none), whether the next drawn frame is it.
std::string g_shotPath;
int  g_shotFrames = -1;
bool g_shotArmed = false;
bool g_shotKb = false;

void start_shot(const char* rest) {
    char name[900] = {0};
    float kb = -1.0f;
    const int n = std::sscanf(rest, "%899s %f", name, &kb);
    if (n < 1) { std::printf("[live] shot failed: usage shot NAME [KBTOP]\n"); std::fflush(stdout); return; }
    g_shotPath = name;
    if (g_shotPath.size() < 4 || g_shotPath.compare(g_shotPath.size() - 4, 4, ".bmp") != 0) g_shotPath += ".bmp";
    g_shotKb = n >= 2 && kb >= 0.0f;
    if (g_shotKb) eden_native_debug_set_keyboard_top(std::min(kb, 1.0f) * SCREEN_HEIGHT);
    g_shotFrames = g_shotKb ? 6 : 0;
}

int game_mode() {
    const char* s = eden_debug_menu_state();
    const char* k = s ? std::strstr(s, "\"game_mode\":") : nullptr;
    return k ? atoi(k + 12) : -1;
}

void read_new_lines() {
    FILE* f = std::fopen(g_path.c_str(), "rb");
    if (!f) return;
    std::fseek(f, 0, SEEK_END);
    long end = std::ftell(f);
    if (end < g_offset) { g_offset = 0; g_partial.clear(); }   // truncated: start over
    if (end > g_offset) {
        std::string buf((size_t)(end - g_offset), '\0');
        std::fseek(f, g_offset, SEEK_SET);
        size_t n = std::fread(&buf[0], 1, buf.size(), f);
        g_offset += (long)n;
        g_partial.append(buf, 0, n);
        size_t nl;
        while ((nl = g_partial.find('\n')) != std::string::npos) {
            std::string line = g_partial.substr(0, nl);
            g_partial.erase(0, nl + 1);
            if (line.compare(0, 5, "pexp ") == 0) { power_parse_exp(line.c_str() + 5); continue; }   // PW.0b: at once, past any `wait`
            if (!line.empty() && line[0] != '#') g_queue.push_back(line);
        }
    }
    std::fclose(f);
}

void set_block(int x, int y, int z, int type, int color) {
    Terrain* t = World::getWorld->terrain;
    if (type == TYPE_LIGHTBOX) addlight(x, z, y, 1.0f, colorTable[color & 255]);
    t->updateChunks(x, z, y, type);
    if (type != TYPE_NONE) t->setColor(x, z, y, (color8)color);
    if (type == TYPE_LIGHTBOX) t->refreshChunksInRadius(x, z, y, light_refreshRadius());
}

void open_world(const char* name) {
    int idx = -1;
    int flat = 0;
    if (!std::strncmp(name, "flat:", 5)) { flat = 1; name += 5; }
    for (int i = 0; i < eden_menu_world_count(); ++i)
        if (!std::strcmp(eden_menu_world_name(i), name)) { idx = i; break; }
    eden_menu_set_pending_world_type(flat);
    eden_menu_set_pending_world_height(64);
    if (idx < 0) {
        std::snprintf(eden_menu_name_buffer(), 128, "%s", name);
        idx = eden_menu_create_world();
    }
    if (idx < 0) { std::fprintf(stderr, "[live] open: create failed\n"); return; }
    eden_menu_select(idx);
    if (eden_menu_play() != 1) { std::fprintf(stderr, "[live] open: play refused\n"); return; }
    g_awaitPlay = true;
}

void mirror_line(int kind, const char* text) {
    std::printf(kind == CmdScript::Sink::ERR ? "[live] ! %s\n" : "[live] %s\n", text);
    std::fflush(stdout);
}

std::string cells_text(const int a[6]) {
    Terrain* t = World::getWorld->terrain;
    std::string out;
    char b[24];
    for (int y = std::min(a[1], a[4]); y <= std::max(a[1], a[4]); ++y)
        for (int z = std::min(a[2], a[5]); z <= std::max(a[2], a[5]); ++z)
            for (int x = std::min(a[0], a[3]); x <= std::max(a[0], a[3]); ++x) {
                std::snprintf(b, sizeof(b), "%d:%d ", t->getLand(x, z, y), t->getColor(x, z, y));
                out += b;
            }
    return out;
}

void verb_cells(Console::Ctx& c) {
    int a[6];
    if (c.args.size() != 6) { c.error("usage: cells X Y Z X2 Y2 Z2 (absolute, Vector order)"); return; }
    for (int i = 0; i < 6; ++i) a[i] = atoi(c.args[i].c_str());
    c.print("cells %s", cells_text(a).c_str());
}

void verb_quit(Console::Ctx&) { SDL_Event e; SDL_zero(e); e.type = SDL_EVENT_QUIT; SDL_PushEvent(&e); }

void register_harness_verbs() {
    static bool done = false;
    if (done) return;
    done = true;
    Console::init();
    Console::addCommand("cells", "X Y Z X2 Y2 Z2", "Harness: every cell of a box as type:colour.", Console::CF_DIAG | Console::CF_WORLD,
                        {CmdScript::ARG_NUMBER, CmdScript::ARG_NUMBER, CmdScript::ARG_NUMBER, CmdScript::ARG_NUMBER, CmdScript::ARG_NUMBER, CmdScript::ARG_NUMBER}, verb_cells);
    Console::addCommand("quit", "", "Harness: end the session.", Console::CF_DIAG, {}, verb_quit);
    Console::setMirror(mirror_line);
}

// Returns true if the command consumed a unit of the per-frame `set` budget.
bool run(const std::string& line) {
    if (!line.empty() && line[0] == '/') {
        World* w = World::getWorld;
        if (!(w && w->terrain && w->player && game_mode() == 1)) { std::fprintf(stderr, "[live] not in a world, dropped: %s\n", line.c_str()); return false; }
        register_harness_verbs();
        Console::setAnchorFixed(0, 0, 0);
        Console::run(line.c_str() + 1);
        return false;
    }
    char cmd[16] = {0};
    int off = 0;
    if (std::sscanf(line.c_str(), "%15s %n", cmd, &off) < 1) return false;
    const char* rest = line.c_str() + off;
    World* w = World::getWorld;
    const bool inWorld = w && w->terrain && w->player && game_mode() == 1;
    if (!std::strcmp(cmd, "open"))      open_world(rest);
    else if (!std::strcmp(cmd, "rate")) g_rate = atoi(rest) > 0 ? atoi(rest) : 1;
    else if (!std::strcmp(cmd, "wait")) g_wait = atoi(rest);
    else if (!std::strcmp(cmd, "say"))  { std::printf("[live] %s\n", rest); std::fflush(stdout); }
    else if (!std::strcmp(cmd, "shot")) start_shot(rest);
    else if (!std::strcmp(cmd, "installzip")) {
        const PackInstall::Result r = PackInstall::installZip(rest, eden_platform_documents_root(), "");
        if (r.ok) std::printf("[live] installzip ok %s %s -> Packs/%s\n", r.id.c_str(), r.version.c_str(), r.folder.c_str());
        else std::printf("[live] installzip refused: %s\n", r.error.c_str());
        for (const std::string& l : r.lines) std::printf("[live] installzip | %s\n", l.c_str());
        std::fflush(stdout);
    }
    else if (!std::strcmp(cmd, "key")) {
        const SDL_Scancode sc = SDL_GetScancodeFromName(rest);
        if (sc == SDL_SCANCODE_UNKNOWN) { std::printf("[live] key: unknown key %s\n", rest); std::fflush(stdout); return false; }
        for (int down = 1; down >= 0; --down) {
            SDL_Event e;
            SDL_zero(e);
            e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
            e.key.scancode = sc;
            e.key.key = SDL_GetKeyFromScancode(sc, SDL_KMOD_NONE, false);
            e.key.down = down != 0;
            e.key.timestamp = SDL_GetTicksNS();
            SDL_PushEvent(&e);
        }
        g_wait = std::max(g_wait, 2);   // the press lands in the next frames' pump, before the next command
    }
    else if (!inWorld) std::fprintf(stderr, "[live] not in a world, dropped: %s\n", line.c_str());
    else if (!std::strcmp(cmd, "set")) {
        int x, y, z, t, c = 0;
        if (std::sscanf(rest, "%d %d %d %d %d", &x, &y, &z, &t, &c) >= 4) { set_block(x, y, z, t, c); return true; }
    } else if (!std::strcmp(cmd, "tp") || !std::strcmp(cmd, "look")) {
        float v[5];
        int n = std::sscanf(rest, "%f %f %f %f %f", &v[0], &v[1], &v[2], &v[3], &v[4]);
        Player* p = w->player;
        if (cmd[0] == 't' && n >= 3) {
            p->pos = MakeVector(v[0], v[1], v[2]);
            p->vel = MakeVector(0, 0, 0);
            if (n == 5) { p->yaw = v[3]; p->pitch = v[4]; }
        } else if (cmd[0] == 'l' && n == 2) { p->yaw = v[0]; p->pitch = v[1]; }
    } else if (!std::strcmp(cmd, "save")) w->fm->saveWorld();
    else if (!std::strcmp(cmd, "press")) {
        int x, y, z;
        if (std::sscanf(rest, "%d %d %d", &x, &y, &z) == 3) {
            const int r = CmdScript::press(TrailerPos::fromEngine(x, y, z));
            static const char* const kR[] = {"no command block", "started", "busy", "refused"};
            std::printf("[live] press %d %d %d: %s\n", x, y, z, kR[r & 3]);
            std::fflush(stdout);
        }
    } else if (!std::strcmp(cmd, "cells")) {
        int a[6];
        if (std::sscanf(rest, "%d %d %d %d %d %d", &a[0], &a[1], &a[2], &a[3], &a[4], &a[5]) == 6) {
            std::printf("[live] cells %s\n", cells_text(a).c_str());
            std::fflush(stdout);
        }
    } else if (!std::strcmp(cmd, "cmdprobe")) { std::printf("[live] cmd %s\n", CmdScript::describe()); std::fflush(stdout); }
    else if (!std::strcmp(cmd, "cmdtrace")) CmdScript::trace = atoi(rest) != 0;
    else if (!std::strcmp(cmd, "photo")) eden_take_screenshot();
    else if (!std::strcmp(cmd, "modspage")) { std::printf("[live] modspage %s\n", ModScript::openModsPage() ? "open" : "refused"); std::fflush(stdout); }
    else if (!std::strcmp(cmd, "quit")) { SDL_Event e; SDL_zero(e); e.type = SDL_EVENT_QUIT; SDL_PushEvent(&e); }
    else std::fprintf(stderr, "[live] unknown: %s\n", line.c_str());
    return false;
}
}  // namespace

extern "C" void eden_live_commands_init(const char* path) {
    g_path = path;
    // Start at the current end: a file left over from an earlier session is history, not orders.
    if (FILE* f = std::fopen(path, "rb")) { std::fseek(f, 0, SEEK_END); g_offset = std::ftell(f); std::fclose(f); }
    std::printf("[live] tailing %s\n", path);
    std::fflush(stdout);
}

extern "C" void eden_live_commands_tick(void) {
    if (g_path.empty()) return;
    read_new_lines();
    if (g_awaitPlay) {
        if (game_mode() != 1) return;
        g_awaitPlay = false;
        g_wait = 90;   // let the first frames of streaming/meshing land before editing
    }
    if (g_shotFrames >= 0 || g_shotArmed) {   // EX.1-0: the queue waits for a pending shot
        if (g_shotFrames > 0) { --g_shotFrames; return; }
        if (g_shotFrames == 0) { g_shotFrames = -1; g_shotArmed = true; eden_native_gl_set_present(0); }
        return;
    }
    if (g_wait > 0) { --g_wait; return; }
    int budget = g_rate;
    while (!g_queue.empty() && budget > 0 && g_wait == 0 && !g_awaitPlay && g_shotFrames < 0) {
        std::string line = g_queue.front();
        g_queue.pop_front();
        if (run(line)) --budget;
    }
}

// EX.1-0: called by the interactive loop right after a frame is drawn (harness/core.cpp). The armed frame
// was drawn with present off, so its back buffer is still whole (mode_shots.cpp capture()'s trick).
extern "C" void eden_live_commands_after_frame(void) {
    if (!g_shotArmed) return;
    g_shotArmed = false;
    int w = 0, h = 0;
    eden_gl_context_get_drawable_size(&w, &h);
    bool ok = w > 0 && h > 0;
    std::vector<unsigned char> px;
    if (ok) {
        px.resize((size_t)w * h * 4);
        ok = R::readPixels(0, 0, w, h, px.data());
    }
    eden_native_gl_set_present(1);
    if (g_shotKb) { eden_native_debug_set_keyboard_top(-2.0f); g_shotKb = false; }
    if (!ok) std::printf("[live] shot failed: no drawable (headless?)\n");
    else if (!harness::g_shots::write_bmp(g_shotPath.c_str(), w, h, px)) std::printf("[live] shot failed: cannot write %s\n", g_shotPath.c_str());
    else std::printf("[live] shot %s %dx%d\n", g_shotPath.c_str(), w, h);
    std::fflush(stdout);
}
