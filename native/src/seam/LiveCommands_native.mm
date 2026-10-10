// LiveCommands_native.mm — a scripted-build feed for an INTERACTIVE native session.
// `--live-cmds=<file>` makes the interactive loop tail <file> and run the commands appended to it,
// so a script outside the process can edit the world while someone watches the window. It is the
// native counterpart of web's dev console (public/eden-console.js → DevConsole_web.mm), which native
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
//   quit                       end the session (an SDL quit event: what closing the window does)
//
// `set` goes through Terrain::updateChunks + setColor — the same pair buildBlock ends in — and, for
// a lightbox, the addlight + refreshChunksInRadius buildBlock does first (DevConsole's setblock
// skips that, so its lightboxes are dark until the next load re-derives lights).

#import "../../../web/src/shim/foundation/uikit_stubs.h"
#include "../../../Classes/World.h"
#include "../../../Classes/Constants.h"
#include "../../../Classes/CmdScript.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>

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
    if (type == TYPE_LIGHTBOX) t->refreshChunksInRadius(x, z, y, LIGHT_RADIUS);
}

void open_world(const char* name) {
    int idx = -1;
    for (int i = 0; i < eden_menu_world_count(); ++i)
        if (!std::strcmp(eden_menu_world_name(i), name)) { idx = i; break; }
    eden_menu_set_pending_world_type(0);
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

// Returns true if the command consumed a unit of the per-frame `set` budget.
bool run(const std::string& line) {
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
            std::string out;
            char b[24];
            for (int y = std::min(a[1], a[4]); y <= std::max(a[1], a[4]); ++y)
                for (int z = std::min(a[2], a[5]); z <= std::max(a[2], a[5]); ++z)
                    for (int x = std::min(a[0], a[3]); x <= std::max(a[0], a[3]); ++x) {
                        std::snprintf(b, sizeof(b), "%d:%d ", w->terrain->getLand(x, z, y), w->terrain->getColor(x, z, y));
                        out += b;
                    }
            std::printf("[live] cells %s\n", out.c_str());
            std::fflush(stdout);
        }
    } else if (!std::strcmp(cmd, "cmdprobe")) { std::printf("[live] cmd %s\n", CmdScript::describe()); std::fflush(stdout); }
    else if (!std::strcmp(cmd, "cmdtrace")) CmdScript::trace = atoi(rest) != 0;
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
    if (g_wait > 0) { --g_wait; return; }
    int budget = g_rate;
    while (!g_queue.empty() && budget > 0 && g_wait == 0 && !g_awaitPlay) {
        std::string line = g_queue.front();
        g_queue.pop_front();
        if (run(line)) --budget;
    }
}
