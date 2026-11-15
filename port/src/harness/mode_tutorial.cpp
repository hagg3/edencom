// UX.10-1: --tutorial-selftest, the bundled tutorial world's load leg (WORKING/onboarding-plan-2026-11-15.md §4 (1)).
// Headless; give it a fresh --docs. Mode group "tutorial".
//
//   1. the bundle carries Tutorial.emod; a copy planted as Worlds/<id>/world.emod is listed under the
//      manifest's name (port/tools/tutorial/manifest.txt, written by port/tools/tutorial.py from the layout)
//   2. it opens as a 64z default-map world (seed 333333) with the manifest's sign and command-block counts
//   3. every command-block script parses through CmdScript::parse with no error, and its cell is steel (74)
//   4. every sign hangs on a solid block
//   5. the player starts in the manifest's spawn cell, standing on solid ground, and is still there 2 s later
//
// UX.10-2 (plan §4 (2), (3)), with prefs kept in a file under --docs so nothing reaches the real profile:
//   6. install on an empty Documents: one world, carrying the marker, key set; a second scan adds nothing
//   7. delete it, scan: nothing added, key still set. Restore: one again. Restore with one present: replaced, still one
//   8. an install that cannot commit leaves the key unset (and lists nothing); a world without the marker is not the tutorial
//   --tutorial-phase=install / verify: the kill-point halves of port/tools/tutorial-crash-gate.sh
//   --tutorial-mutate=nomarker (the sidecar is not written) must FAIL leg 6; =keyfirst (the key set before the commit) must FAIL the verify phase.
//
// UX.10-3 (plan §4 (4), (5)): the hints. In the installed tutorial world, scripted input through the seams the game's own
// keys, pad and fingers use completes every step in order for each input mode (keyboard + mouse, touch, pad), each
// showing that mode's copy; the same world with its marker removed shows no card; a session shows none.
//   --tutorial-mutate=hintsbyname (a world is "the tutorial" by its name, not its marker) must FAIL "a world without the marker shows no card".
#include "harness.h"
#include "eden_fs.h"
#include <cmath>
#include <cstdio>
#include "../../../Classes/Player.h"
#include "../../../Classes/Tutorial.h"
#include "../../../Classes/MpWorld.h"
#include "../../../Classes/Hud.h"
#include "eden_prefs.h"

using namespace harness;

namespace harness {
namespace g_tutorial {
namespace {

struct Manifest {
    std::string name;
    int signs = -1, cmds = -1, sx = 0, sy = 0, sz = 0;
    bool ok = false;
};

Manifest readManifest(const std::string& path) {
    Manifest m;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return m;
    char line[256];
    int got = 0;
    while (std::fgets(line, sizeof line, f)) {
        if (line[0] == '#') continue;
        if (!std::strncmp(line, "name ", 5)) {
            m.name = line + 5;
            while (!m.name.empty() && (m.name.back() == '\n' || m.name.back() == '\r')) m.name.pop_back();
            got |= 1;
        } else if (std::sscanf(line, "signs %d", &m.signs) == 1) got |= 2;
        else if (std::sscanf(line, "cmds %d", &m.cmds) == 1) got |= 4;
        else if (std::sscanf(line, "spawn %d %d %d", &m.sx, &m.sy, &m.sz) == 3) got |= 8;
    }
    std::fclose(f);
    m.ok = got == 15;
    return m;
}

// Not something to stand on or hang a sign from: air, liquids, objects (doors, portals, flowers, golden cubes),
// ladders and vines. The same set as tutorial.py's NOT_SOLID.
bool solid(int t) {
    switch (t) {
    case 0: case TYPE_WATER: case TYPE_WATER1: case TYPE_WATER2: case TYPE_WATER3:
    case TYPE_LAVA: case TYPE_LAVA1: case TYPE_LAVA2: case TYPE_LAVA3:
    case TYPE_LADDER: case TYPE_VINE: case TYPE_FLOWER: case TYPE_GOLDEN_CUBE:
        return false;
    default:
        return !(t >= TYPE_DOOR1 && t <= TYPE_DOOR_TOP) && !(t >= TYPE_PORTAL1 && t <= TYPE_PORTAL_TOP) && t > 0;
    }
}

std::string docsRoot() { return eden_platform_documents_root(); }

// Folder worlds on disk (listed or not: a `.partial` staging dir has a leading dot and never counts).
int worldDirs(int* marked) {
    int n = 0;
    if (marked) *marked = 0;
    const std::string wroot = docsRoot() + "/Worlds";
    for (const std::string& id : eden_fs_list(wroot)) {
        if (id.empty() || id[0] == '.' || !eden_fs_exists(wroot + "/" + id + "/world.emod")) continue;
        ++n;
        if (marked && TutorialInstall::readMarker(wroot + "/" + id, NULL)) ++*marked;
    }
    return n;
}
int listedWorlds() {
    std::vector<FileManager::WorldEntry> e;
    World::getWorld->fm->listWorlds(&e);
    return (int)e.size();
}
// The prefs live in a file beside the worlds, so a key set here is read back by the next process (the kill loop).
void usePrefsFile() { eden_prefs_reset_for_test((docsRoot() + "/tutorial-prefs").c_str()); }
int keySet() { return eden_prefs_get_int(TutorialInstall::prefsKey(), 0); }
void scan() { World::getWorld->menu->loadWorlds(); }   // the install hook sits at the top of every scan

// The first half of a kill-point run: a fresh Documents, one scan (the hook installs), maybe _Exit(86) inside it.
int phaseInstall() {
    usePrefsFile();
    TutorialInstall::autoInstall = true;
    scan();
    std::printf("[eden-tutorial] install phase done: %d world(s), key %d\n", worldDirs(NULL), keySet());
    return 0;
}

// The second half, in a fresh process after the kill: never a half world listed, never the key without the world;
// the next launch finishes the install, and once.
int phaseVerify() {
    usePrefsFile();
    char d[300];
    int marked = 0;
    const int dirs = worldDirs(&marked);
    std::snprintf(d, sizeof d, "%d world(s) on disk, %d marked, %d listed, key %d", dirs, marked, listedWorlds(), keySet());
    check(dirs == marked && dirs <= 1, "after the kill: no half world, at most the one", d);
    check(!keySet() || dirs == 1, "after the kill: the key is never set without the world", d);
    TutorialInstall::autoInstall = true;
    scan();
    std::snprintf(d, sizeof d, "%d world(s), %d marked, key %d", worldDirs(&marked), marked, keySet());
    check(worldDirs(&marked) == 1 && marked == 1 && keySet() == 1, "the next launch leaves exactly one tutorial world and the key", d);
    scan();
    check(worldDirs(&marked) == 1, "and a further scan adds nothing", "");
    for (const std::string& e : eden_fs_list(docsRoot() + "/Worlds")) {
        if (e.size() > 8 && e[0] == '.' && e.compare(e.size() - 8, 8, ".partial") == 0)
            check(false, "no staging dir is left behind", e.c_str());
    }
    std::printf("[eden-tutorial] verify %s\n", g_selftestFailures ? "FAILED" : "PASSED");
    return g_selftestFailures ? 1 : 0;
}

// ---- the hints (UX.10-3)
extern "C" {
void eden_input_test_set_pad_last(int on);   // Input_native.cpp
void eden_apply_look_delta(float dx, float dy);   // Input.cpp: what the mouse and the pad's right stick call
void eden_set_jump(int down);
void eden_select_hotbar_slot(int slot);
void eden_hotbar_scroll(int dir);
}

const char* modeName(int m) { return m == Tutorial::IM_TOUCH ? "touch" : m == Tutorial::IM_PAD ? "pad" : "kbm"; }

void setInputMode(int m) {
    eden_set_detected_touch(m == Tutorial::IM_TOUCH ? 1 : 0);
    eden_input_test_set_pad_last(m == Tutorial::IM_PAD ? 1 : 0);
}

// Ticks until a hint is on the card: the "Done" flash is over and any picker fade has finished (3 s at most).
void settle() {
    const Uint64 t0 = SDL_GetTicks();   // the frame's etime is wall clock here
    while ((!Tutorial::cardShown() || Tutorial::cardText() == "Done") && SDL_GetTicks() - t0 < 3000) tick(1);
    tick(3);
}

void goTo(float x, float z, float yaw = 0) {
    Player* p = World::getWorld->player;
    eden_console_teleport(x, p->pos.y, z);
    p->yaw = yaw;
    p->pitch = -30;   // looking at the ground a few blocks ahead: a click always has a block to act on
    tick(4);
    settle();
}

// One full run of the nine steps in one input mode. `dir` is the world folder. Returns true if every check held.
bool runSequence(int mode, const std::string& dir, const std::string& key, const Manifest& m) {
    char d[400];
    const int fails0 = g_selftestFailures;
    TutorialInstall::Marker fresh;
    TutorialInstall::writeMarker(dir, fresh);
    Tutorial::worldLoaded(dir, key);
    setInputMode(mode);
    Player* p = World::getWorld->player;
    Hud* hud = World::getWorld->hud;
    hud->mode = MODE_BUILD;
    eden_console_teleport((float)m.sx + 0.5f, p->pos.y, (float)m.sz + 0.5f);
    p->yaw = 0; p->pitch = -30;
    tick(6);
    const float oz = (float)m.sz + 0.5f;
    const float ux = (float)m.sx;   // the start pad's centre cell is u = 0; the stations' x are u offsets from it (layout.py)
    std::snprintf(d, sizeof d, "mode %s -> %d", modeName(mode), Tutorial::inputMode());
    check(Tutorial::inputMode() == mode, "the input mode is the one the leg set", d);

    for (int i = 0; i < Tutorial::stepCount(); ++i) {
        const char* id = Tutorial::stepId(i);
        // walk to the step's station (u offsets of layout.py's signs: walk 7, build 17, blocks 27, hotbar 39, finish 117)
        const float at = !std::strcmp(id, "jump") ? 5.5f : (!std::strcmp(id, "break") || !std::strcmp(id, "place")) ? 14.5f :
                         (!std::strcmp(id, "picker") || !std::strcmp(id, "paint")) ? 25.5f : !std::strcmp(id, "hotbar") ? 37.5f :
                         !std::strcmp(id, "menu") ? 115.5f : -1.0f;
        if (at >= 0) goTo(ux + at, oz);
        const std::string want = Tutorial::hintText(i, mode);
        std::snprintf(d, sizeof d, "[%s] step %s: card \"%s\", want \"%s\", current %d", modeName(mode), id,
                      Tutorial::cardText().c_str(), want.c_str(), Tutorial::currentStep());
        check(Tutorial::currentStep() == i && Tutorial::cardShown() && Tutorial::cardText() == want && !want.empty() &&
                  want.find('{') == std::string::npos && want.find('?') == std::string::npos,
              "the card shows the step's hint in the mode's copy, keys filled in", d);
        for (int j = i + 1; j < Tutorial::stepCount(); ++j)
            if (Tutorial::stepDone(j)) check(false, "no later step is done early", Tutorial::stepId(j));
        // ---- the player does what it says, through the engine's own input seams
        if (!std::strcmp(id, "look")) {
            const float y0 = p->yaw;   // the mouse's delta: turn in small steps, check the angle on the player
            while (std::fabs(p->yaw - y0) < 30.0f) { eden_apply_look_delta(8, 0); tick(1); }
            tick(2);
            check(!Tutorial::stepDone(i), "30 degrees of turn is not enough", "");
            while (std::fabs(p->yaw - y0) < 50.0f) { eden_apply_look_delta(8, 0); tick(1); }
            tick(2);
        } else if (!std::strcmp(id, "move")) {
            eden_console_teleport(p->pos.x + 1.0f, p->pos.y, p->pos.z);
            tick(2);
            check(!Tutorial::stepDone(i), "one block is not enough", "");
            eden_console_teleport(p->pos.x + 3.0f, p->pos.y, p->pos.z);
            tick(2);
        } else if (!std::strcmp(id, "jump")) {
            tick(30);
            eden_set_jump(1);
            tick(4);
            eden_set_jump(0);
            tick(4);
        } else if (!std::strcmp(id, "break") || !std::strcmp(id, "place") || !std::strcmp(id, "paint")) {
            if (!std::strcmp(id, "paint")) {
                tap_hud(2);                                 // the colour picker opens (a real tap on the HUD's paint button)...
                tap_hud(2);                                 // ...and a second tap takes the colour: paint mode
                check(hud->mode == MODE_PAINT, "the second tap takes the colour (paint mode)", "");
            }
            // a mouse button, a pad trigger (it drives the same hold-to-act machine) and a tap in the mode all end in these two
            const bool build = std::strcmp(id, "break") != 0;
            // a click can land on nothing or on a cell the build refuses: the player turns and tries again, as a person would
            for (int tries = 0; tries < 8 && !Tutorial::stepDone(i); ++tries) {
                p->yaw = 90.0f * (tries % 4);
                p->pitch = tries < 4 ? -30.0f : 0.0f;
                tick(2);
                if (hud->mode == MODE_NONE) hud->mode = MODE_BUILD;
                eden_click_begin(build ? 1 : 0);
                tick(4);
                eden_click_end(build ? 1 : 0);
                tick(4);
            }
        } else if (!std::strcmp(id, "picker")) {
            tap_hud(1);
            check(hud->mode == MODE_PICK_BLOCK, "the block picker is open", "");
        } else if (!std::strcmp(id, "hotbar")) {
            if (mode == Tutorial::IM_TOUCH) tap_hud(11 + 3);
            else if (mode == Tutorial::IM_PAD) eden_hotbar_scroll(1);
            else eden_select_hotbar_slot(2);
            tick(3);
        } else if (!std::strcmp(id, "menu")) {
            tap_hud(0);
        }
        tick(2);
        std::snprintf(d, sizeof d, "[%s] step %s, card \"%s\"", modeName(mode), id, Tutorial::cardText().c_str());
        check(Tutorial::stepDone(i), "the step completes", d);
        if (!std::strcmp(id, "picker")) tap_hud(1);          // close the picker again
        if (!std::strcmp(id, "paint")) tap_hud(2), tap_hud(2);
        if (!std::strcmp(id, "menu") && eden_hud_in_menu()) tap_hud(0);
        if (i + 1 < Tutorial::stepCount()) {
            bool flashed = false;
            for (int k = 0; k < 6 && !flashed; ++k) { flashed = Tutorial::cardText() == "Done"; if (!flashed) tick(1); }
            check(flashed || !std::strcmp(id, "picker") || !std::strcmp(id, "paint") || !std::strcmp(id, "menu"),
                  "a short \"Done\" flash follows", id);
            settle();
        }
    }
    tick(200);
    check(!Tutorial::cardShown(), "all nine done: no card", Tutorial::cardText().c_str());
    TutorialInstall::Marker mk;
    TutorialInstall::readMarker(dir, &mk);
    std::snprintf(d, sizeof d, "%d done in the marker", (int)mk.done.size());
    bool inOrder = (int)mk.done.size() == Tutorial::stepCount();
    for (size_t i = 0; i < mk.done.size() && inOrder; ++i) inOrder = mk.done[i] == Tutorial::stepId((int)i);
    check(inOrder, "the marker records every step, in order", d);
    return g_selftestFailures == fails0;
}

}  // namespace

int run_tutorial_selftest() {
    g_selftestTag = "eden-tutorial";
    char d[700];
    if (!mutation_known("eden-tutorial", "--tutorial-mutate", g_opt.tutorialMutate, {"nomarker", "keyfirst", "hintsbyname"})) return 2;
    Tutorial::flashSeconds = 0.15f;
    Tutorial::testHintsByName = g_opt.tutorialMutate == "hintsbyname";
    TutorialInstall::testMutation = g_opt.tutorialMutate == "nomarker" ? 1 : g_opt.tutorialMutate == "keyfirst" ? 2 : 0;
    if (g_opt.tutorialPhase == "install") return phaseInstall();
    if (g_opt.tutorialPhase == "verify") return phaseVerify();

    const Manifest m = readManifest(g_opt.tutorialManifest);
    check(m.ok, "the layout manifest reads", g_opt.tutorialManifest.c_str());
    if (!m.ok) return 1;

    // ---- 1. bundled, planted, listed
    const std::string src = eden_bundle_path("Tutorial.emod");
    check(!src.empty() && eden_fs_exists(src), "the bundle carries Tutorial.emod", src.c_str());
    if (src.empty()) return 1;
    const std::string dir = std::string(eden_platform_documents_root()) + "/Worlds/1794700800-tutr";
    const bool planted = eden_fs_make_dirs(dir) && eden_fs_copy(src, dir + "/world.emod");
    check(planted, "plant it as a folder world", dir.c_str());
    World::getWorld->menu->loadWorlds();
    bool listed = false;
    for (int i = 0; i < eden_menu_world_count(); ++i)
        if (m.name == eden_menu_world_name(i)) listed = true;
    check(listed, "the world list shows it under the layout's name", m.name.c_str());
    if (!planted || !listed) return 1;   // open_world would CREATE a world of that name

    // ---- 2. opens as a default-map 64z world with the layout's trailer
    if (!open_world(m.name.c_str(), 64)) {
        check(false, "open the tutorial world", "");
        return 1;
    }
    tick(10);
    Terrain* ter = World::getWorld->terrain;
    std::snprintf(d, sizeof d, "seed %d, height %d", ter->level_seed, (int)T_HEIGHT);
    check(ter->level_seed == 333333 && T_HEIGHT == 64, "a 64z world on the bundled map (seed 333333)", d);
    const WorldTrailer* t = (const WorldTrailer*)eden_debug_world_trailer();
    std::snprintf(d, sizeof d, "%d signs, %d command blocks (layout: %d, %d)", t ? (int)t->signCount() : -1,
                  t ? (int)t->cmdCount() : -1, m.signs, m.cmds);
    check(t && t->parsed() && (int)t->signCount() == m.signs && (int)t->cmdCount() == m.cmds,
          "the trailer holds the layout's signs and command blocks", d);
    if (!t) return 1;

    // ---- 3. every script parses; every command block is steel
    {
        std::string bad;
        for (size_t i = 0; i < t->cmdCount(); ++i) {
            const TrailerCmd& c = t->cmd(i);
            char text[513];
            std::memcpy(text, c.script, 512);
            text[512] = 0;
            CmdScript::Program p;
            CmdScript::parse(text, &p);
            const int cell = eden_console_getblock(c.x, c.y, c.z);   // Terrain argument order = the file's (x, y, z)
            if (p.errors || p.stmts.empty() || cell != TYPE_STEEL) {
                char b[160];
                std::snprintf(b, sizeof b, " [%d %d %d: %d errors (%s), cell %d]", c.x, c.y, c.z, p.errors,
                              p.firstError.c_str(), cell);
                bad += b;
            }
        }
        check(bad.empty(), "every script parses and sits on steel", bad.c_str());
    }
    // ---- 4. every sign hangs on a solid block
    {
        std::string bad;
        for (size_t i = 0; i < t->signCount(); ++i) {
            const TrailerSign& s = t->sign(i);
            const int cell = eden_console_getblock(s.x, s.y, s.z);
            if (!solid(cell)) {
                char b[96];
                std::snprintf(b, sizeof b, " [%d %d %d: %d]", s.x, s.y, s.z, cell);
                bad += b;
            }
        }
        check(bad.empty(), "every sign hangs on a solid block", bad.c_str());
    }
    // ---- 5. the spawn: in the layout's cell, on solid ground, and it stays there
    {
        Player* p = World::getWorld->player;
        const float feet0 = p->pos.y - p->boxheight / 2;
        const int fx = (int)std::floor(p->pos.x), fy = (int)std::floor(feet0 + 0.01f), fz = (int)std::floor(p->pos.z);
        const int under = eden_console_getblock(fx, fz, fy - 1);
        std::snprintf(d, sizeof d, "feet cell %d %d %d (layout %d %d %d), under it %d, yaw %.0f", fx, fy, fz, m.sx, m.sy,
                      m.sz, under, p->yaw);
        check(fx == m.sx && fy == m.sy && fz == m.sz && solid(under) && eden_console_getblock(fx, fz, fy) == 0 &&
                  eden_console_getblock(fx, fz, fy + 1) == 0,
              "the player starts on solid ground in the spawn cell", d);
        tick(120);
        const float feet1 = p->pos.y - p->boxheight / 2;
        std::snprintf(d, sizeof d, "feet %.3f -> %.3f", feet0, feet1);
        check(std::fabs(feet1 - feet0) < 0.1f && (int)std::floor(feet1 + 0.01f) == m.sy, "and is still standing there 2 s later", d);
    }

    quit_to_menu();
    check(!TutorialInstall::isTutorialWorld(docsRoot(), "Worlds/1794700800-tutr/world.emod"),
          "a world without the marker is not the tutorial world", "");

    // ---- 6-8. install, boot, delete, restore (UX.10-2)
    World::getWorld->fm->removeWorldDir("Worlds/1794700800-tutr");
    usePrefsFile();
    eden_prefs_set_int(TutorialInstall::prefsKey(), 0);
    TutorialInstall::autoInstall = true;
    int marked = 0;
    std::snprintf(d, sizeof d, "%d world(s), key %d", worldDirs(&marked), keySet());
    check(worldDirs(NULL) == 0 && !keySet(), "an empty Documents, key unset", d);
    scan();
    std::snprintf(d, sizeof d, "%d world(s), %d marked, %d listed, key %d", worldDirs(&marked), marked, listedWorlds(), keySet());
    check(worldDirs(&marked) == 1 && marked == 1 && listedWorlds() == 1 && keySet() == 1,
          "the first scan installs one world with the marker and sets the key", d);
    std::string key;
    {
        const std::vector<std::string> k = TutorialInstall::markedWorlds(docsRoot());
        if (!k.empty()) key = k[0];
    }
    TutorialInstall::Marker mk;
    check(!key.empty() && TutorialInstall::isTutorialWorld(docsRoot(), key) &&
              TutorialInstall::readMarker(docsRoot() + "/" + FileManager::worldDirOf(key), &mk) && mk.done.empty() && !mk.hidden,
          "the marker reads back as v1, nothing done, not hidden", key.c_str());
    scan();
    check(worldDirs(&marked) == 1, "a second scan adds nothing", "");
    {
        bool named = false;
        std::vector<FileManager::WorldEntry> e;
        World::getWorld->fm->listWorlds(&e);
        for (const auto& w : e) named = named || (w.key == key && w.name == m.name);
        check(named, "the list shows it under the layout's name", m.name.c_str());
    }
    World::getWorld->fm->deleteWorld(key);   // My Worlds' own delete
    scan();
    std::snprintf(d, sizeof d, "%d world(s), key %d", worldDirs(NULL), keySet());
    check(worldDirs(NULL) == 0 && keySet() == 1, "after a delete the next scan adds nothing (the key stays set)", d);
    const std::string r1 = TutorialInstall::restore(docsRoot());
    check(!r1.empty() && worldDirs(&marked) == 1 && marked == 1, "Restore adds it back", r1.c_str());
    {   // a player's mark on the world, then Restore replaces it with a fresh one
        TutorialInstall::Marker done;
        done.done.push_back("walk");
        done.hidden = true;
        check(TutorialInstall::writeMarker(docsRoot() + "/" + FileManager::worldDirOf(r1), done), "mark a step done and hide the hints", "");
    }
    const std::string r2 = TutorialInstall::restore(docsRoot());
    TutorialInstall::Marker fresh;
    const bool fr = !r2.empty() && TutorialInstall::readMarker(docsRoot() + "/" + FileManager::worldDirOf(r2), &fresh);
    std::snprintf(d, sizeof d, "%d world(s), %d marked, old %s", worldDirs(&marked), marked,
                  eden_fs_exists(docsRoot() + "/" + r1) ? "still there" : "gone");
    check(fr && worldDirs(&marked) == 1 && marked == 1 && r2 != r1 && !eden_fs_exists(docsRoot() + "/" + r1) &&
              fresh.done.empty() && !fresh.hidden,
          "Restore with one present replaces it (fresh marker), still one", d);
    // A failed install: Worlds is a file here, so nothing can be staged. No world, key unset.
    {
        const std::string bad = docsRoot() + "/badroot";
        eden_fs_make_dirs(bad);
        eden_fs_write_atomic(bad + "/Worlds", "x", 1);
        eden_prefs_set_int(TutorialInstall::prefsKey(), 0);
        const bool did = TutorialInstall::installOnce(bad);
        check(!did && !keySet() && !eden_fs_is_dir(bad + "/Worlds"), "an install that cannot commit leaves the key unset", "");
    }

    // ---- 9-11. the hints (UX.10-3)
    {
        g_tickInput = true;   // the input translator's per-frame tick: it tracks the hand's tool for a right click
        scan();
        const std::vector<std::string> keys = TutorialInstall::markedWorlds(docsRoot());
        check(keys.size() == 1, "one tutorial world to play", "");
        if (keys.size() != 1) return 1;
        const std::string tkey = keys[0];
        const std::string tdir = docsRoot() + "/" + FileManager::worldDirOf(tkey);
        if (!open_world(m.name.c_str(), 64)) { check(false, "open the installed tutorial world", ""); return 1; }
        tick(20);
        check(Tutorial::active(), "the marked world has hints", "");
        runSequence(Tutorial::IM_KBM, tdir, tkey, m);
        runSequence(Tutorial::IM_TOUCH, tdir, tkey, m);
        runSequence(Tutorial::IM_PAD, tdir, tkey, m);
        setInputMode(Tutorial::IM_KBM);

        // The same world without its marker: the old path stays silent.
        quit_to_menu();
        check(eden_fs_remove(tdir + "/" + TutorialInstall::markerFile()), "remove the marker", "");
        check(open_world(m.name.c_str(), 64), "open the world again, without the marker", "");
        tick(30);
        std::snprintf(d, sizeof d, "active %d, card \"%s\"", Tutorial::active() ? 1 : 0, Tutorial::cardText().c_str());
        check(!Tutorial::active() && !Tutorial::cardShown(), "a world without the marker shows no card", d);
        // A multiplayer session has no folder: it is told "" and stays silent too.
        Tutorial::worldLoaded("", tkey);
        tick(10);
        check(!Tutorial::active() && !Tutorial::cardShown(), "a session (no world folder) shows no card", "");
        quit_to_menu();
    }

    std::printf("[eden-tutorial] %s\n", g_selftestFailures ? "FAILED" : "PASSED");
    return g_selftestFailures ? 1 : 0;
}

}  // namespace g_tutorial
}  // namespace harness
