// G.4: split out of the old eden_main_native.cpp (see harness.h). Mode group "input".
#include "harness.h"
#include "../../../Classes/SrclikeMove.h"   // SRC.2: --srclike-selftest
#include "../../../Classes/Terrain.h"      // SRC.3: getLandc2
#include "../../../Classes/Util.h"         // SRC.3: makeRamp / makeSide (gate 1)
#include <chrono>
#include <cmath>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using namespace harness;

extern "C" {   // port/src/seam/Input.cpp (SRC.2 --srclike-selftest)
int  eden_get_hotbar_index(void);
void eden_wheel_action(int dir);
void eden_select_hotbar_slot(int slot);
void eden_apply_look_delta(float dx, float dy);   // SRC.7
}

extern float eden_srclike_sens;                   // port/src/seam/Settings.cpp (SRC.7)

namespace harness {
namespace g_input {


// ---------------------------------------------------------------------------------------------
// The bundled map has a HARD BORDER, and this harness's default origin sits exactly on it.
// ---------------------------------------------------------------------------------------------
// Classes/Player.mm's preupdate clamps pos.x and pos.z into
//     [4096*CHUNK_SIZE - GSIZE/2, 4096*CHUNK_SIZE + GSIZE/2]
// on every world carrying DEFAULT_LEVEL_SEED — which is every world these selftests create. GSIZE
// is T_SIZE*10 = 2880 and 4096*CHUNK_SIZE is 65536, so the playable square is [64096, 66976] on
// both axes, and this file's default `--at=1440,40,1440` (shared, verbatim, with
// web-port-final:web/tools/headless-mesh-checksum.js) is OUTSIDE it on both axes. The engine clamps it to exactly
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
// nothing. What was wrong was the belief about where it was, not the number. (B.5b-1a did
// re-record them, for a different reason: it changed which columns are meshed. The current values
// live in .github/scripts/check-stage1.py.)
// X.6: the clamp is skipped on "unbounded" worlds (FileManager::worldUnbounded); fresh worlds still clamp.
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
        set_setting_by_key("fly", 1);   // FM.1: V only cycles when "Allow fly" is on
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
// --fly-selftest (FM.2): noclip physics, WORKING/fly-modes-plan-2026-10-23.md §3.2–3.4. Fixed
// 1/60 s etime, W and Space through the real key path, look set on the Player. Measures Fly's
// real cruise (what NOCLIP_SPEED was chosen against), then noclip: cruise, pitch steers, crisp
// stop, passes a wall Fly stops at, lava does nothing, the y floor, unstick in stone vs no drop
// in open air, and a long run that streaming keeps up with (run it at --radius=9 and 16).
// FM.4 adds the cycle/toast and touch gates; --fly-mutate=move calls move() in noclip again and
// the wall gate must FAIL (native.yml runs it on linux/windows).
// ---------------------------------------------------------------------------------------------
int run_fly_selftest() {
    g_selftestTag = "eden-fly";
    g_tickInput = true;   // W/Space/Ctrl/Shift reach the engine through the input tick
    const char* name = g_opt.world.empty() ? "fly-selftest" : g_opt.world.c_str();
    if (!open_world(name, g_opt.height)) return 1;
    g_app->setFixedEtime(1.0f / 60.0f);
    tick(60);
    Player* pl = World::getWorld->player;
    char d[240];
    auto fill = [](int x0, int x1, int z0, int z1, int y0, int y1, int type) {
        for (int x = x0; x <= x1; ++x)
            for (int z = z0; z <= z1; ++z)
                for (int y = y0; y <= y1; ++y) eden_console_setblock(x, z, y, type);
    };
    auto at = [&](float x, float y, float z) {
        eden_console_teleport(x, y, z);
        tick(2);
        pl->vel = MakeVector(0, 0, 0);
        pl->yaw = 0; pl->pitch = 0;   // yaw 0 faces +x
    };
    auto speed_over = [&](int n) {   // blocks/s over the next n ticks, 3D
        const Vector a = pl->pos;
        tick(n);
        const Vector b = pl->pos;
        const float dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
        return std::sqrt(dx * dx + dy * dy + dz * dz) * 60.0f / (float)n;
    };
    const int bx = (int)kMapCentre, bz = (int)kMapCentre, by = g_opt.height - 8;
    fill(bx - 2, bx + 90, bz - 2, bz + 2, by - 2, by + 3, TYPE_NONE);   // an open corridor along +x


    if (g_opt.flyMutate == "move") NOCLIP_MUTATE_MOVE = true;
    else if (!g_opt.flyMutate.empty()) { std::fprintf(stderr, "[eden-fly] unknown --fly-mutate=%s\n", g_opt.flyMutate.c_str()); return 1; }
    auto press = [&](SDL_Scancode k) { push_key(k, true); tick(2); push_key(k, false); tick(2); };
    auto toast = [&]() -> std::string {
        const std::string* t = World::getWorld->hud->sb->current();
        return t ? *t : std::string("(none)");
    };

    // FM.4 gate 1: V cycles Off -> Fly -> Noclip -> Off and the toast says so.
    set_setting_by_key("fly", 1);
    fly_set_state(0);
    at(bx + 0.5f, by + 0.5f, bz + 0.5f);
    std::string tt[3]; int st[3];
    for (int i = 0; i < 3; ++i) { press(SDL_SCANCODE_V); tt[i] = toast(); st[i] = fly_get_state(); }
    std::snprintf(d, sizeof(d), "states %d %d %d, toasts '%s' '%s' '%s'", st[0], st[1], st[2], tt[0].c_str(), tt[1].c_str(), tt[2].c_str());
    check(st[0] == 1 && st[1] == 2 && st[2] == 0 && tt[0] == "Fly ON" && tt[1] == "NOCLIP on" && tt[2] == "Fly OFF",
          "V cycles Off -> Fly -> Noclip -> Off with the toast", d);

    // Gate 2: Allow fly off pins Off, no toast; turning it off in noclip leaves noclip, clear of solids.
    set_setting_by_key("fly", 0);
    World::getWorld->hud->sb->clear();
    for (int i = 0; i < 3; ++i) press(SDL_SCANCODE_V);
    std::snprintf(d, sizeof(d), "state %d, toast '%s'", fly_get_state(), toast().c_str());
    check(fly_get_state() == 0 && toast() == "(none)", "Allow fly off: V does nothing, no toast", d);
    set_setting_by_key("fly", 1);
    fill(bx + 30, bx + 32, bz - 1, bz + 1, by - 1, by + 1, TYPE_STONE);
    fly_set_state(2);
    at(bx + 31.5f, by + 0.5f, bz + 0.5f);   // inside the block
    tick(5);
    set_setting_by_key("fly", 0);
    tick(2);
    std::snprintf(d, sizeof(d), "state %d, blocked %d", fly_get_state(), (int)pl->boxBlocked());
    check(fly_get_state() == 0 && !pl->boxBlocked(), "turning Allow fly off in noclip leaves it, clear of solids", d);
    fill(bx + 30, bx + 32, bz - 1, bz + 1, by - 1, by + 1, TYPE_NONE);
    set_setting_by_key("fly", 1);

    // Gate 7: touch. Jump/crouch slots are fly up/down (m_jump never set), fire/pickaxe taps no
    // longer move y, the cycle button cycles only when allowed.
    fly_set_state(1);
    at(bx + 0.5f, by + 0.5f, bz + 0.5f);
    tick(30);
    Hud* hud = World::getWorld->hud;
    bool jumpSeen = false;
    float ty = pl->pos.y;
    eden_tap_hud_button_begin(8);
    for (int i = 0; i < 60; ++i) { tick(1); if (hud->m_jump) jumpSeen = true; }
    const bool upFlag = hud->m_flyup;
    eden_tap_hud_button_end(8);
    tick(30);
    const float up = pl->pos.y - ty;
    ty = pl->pos.y;
    eden_tap_hud_button_begin(9);
    for (int i = 0; i < 60; ++i) { tick(1); if (hud->m_jump) jumpSeen = true; }
    const bool downFlag = hud->m_flydown;
    eden_tap_hud_button_end(9);
    tick(30);
    const float down = pl->pos.y - ty;
    std::snprintf(d, sizeof(d), "hold jump slot: dy %+.2f (m_flyup %d); crouch slot: dy %+.2f (m_flydown %d); m_jump seen %d",
                  up, (int)upFlag, down, (int)downFlag, (int)jumpSeen);
    check(up > 2.0f && down < -2.0f && upFlag && downFlag && !jumpSeen, "touch: jump/crouch slots are fly up/down while flying", d);
    tick(60);
    ty = pl->pos.y;
    for (int b : {7, 1, 7, 1}) { eden_tap_hud_button_begin(b); tick(4); eden_tap_hud_button_end(b); tick(4); }   // fire x2, blocks x2 (each toggles back)
    tick(30);
    std::snprintf(d, sizeof(d), "dy %+.3f after tapping fire/blocks in Fly", pl->pos.y - ty);
    check(std::fabs(pl->pos.y - ty) < 0.05f, "touch: fire and block taps no longer move y", d);
    fly_set_state(0);
    eden_tap_hud_button_begin(10); tick(4); eden_tap_hud_button_end(10); tick(4);
    const int s1 = fly_get_state();
    eden_tap_hud_button_begin(10); tick(4); eden_tap_hud_button_end(10); tick(4);
    const int s2 = fly_get_state();
    set_setting_by_key("fly", 0);
    fly_set_state(0);
    tick(2);
    eden_tap_hud_button_begin(10); tick(4); eden_tap_hud_button_end(10); tick(4);
    const int s3 = fly_get_state();
    set_setting_by_key("fly", 1);
    std::snprintf(d, sizeof(d), "allowed: %d then %d; not allowed: %d", s1, s2, s3);
    check(s1 == 1 && s2 == 2 && s3 == 0, "touch: the cycle button cycles only when Allow fly is on", d);
    fly_set_state(0);

    // Fly's real cruise (plan §3.2: measure before choosing NOCLIP_SPEED).
    fly_set_state(1);
    at(bx + 0.5f, by + 0.5f, bz + 0.5f);
    push_key(SDL_SCANCODE_W, true);
    tick(240);
    const float flyCruise = speed_over(60);
    push_key(SDL_SCANCODE_W, false);
    std::printf("[eden-fly] Fly cruise (W, fly_accel 1, 60 Hz): %.1f b/s; NOCLIP_SPEED %.1f\n",
                flyCruise, NOCLIP_SPEED);

    // Noclip cruise and crisp stop.
    fly_set_state(2);
    at(bx + 0.5f, by + 0.5f, bz + 0.5f);
    push_key(SDL_SCANCODE_W, true);
    tick(60);
    const float ncCruise = speed_over(30);
    push_key(SDL_SCANCODE_W, false);
    tick(30);
    const float ncStopped = speed_over(6);
    std::snprintf(d, sizeof(d), "%.1f b/s (NOCLIP_SPEED %.1f)", ncCruise, NOCLIP_SPEED);
    check(std::fabs(ncCruise - NOCLIP_SPEED) < NOCLIP_SPEED * 0.05f, "noclip cruise is NOCLIP_SPEED", d);
    std::snprintf(d, sizeof(d), "%.3f b/s 0.5 s after release", ncStopped);
    check(ncStopped < 0.5f, "noclip stops crisply on release", d);

    // Pitch steers: +45 rises, 0 stays level.
    at(bx + 0.5f, by - 1.5f - 10.0f, bz + 0.5f);   // start lower so the rise fits under the clamp
    pl->pitch = 45;
    float y0 = pl->pos.y;
    push_key(SDL_SCANCODE_W, true);
    tick(60);
    const float riseUp = pl->pos.y - y0;
    pl->pitch = 0;
    tick(30);   // let vel.y decay
    y0 = pl->pos.y;
    tick(60);
    const float riseLevel = pl->pos.y - y0;
    push_key(SDL_SCANCODE_W, false);
    tick(30);
    std::snprintf(d, sizeof(d), "pitch +45: dy %.2f in 1 s; pitch 0: dy %.3f", riseUp, riseLevel);
    check(riseUp > 2.0f && std::fabs(riseLevel) < 0.1f, "noclip: look pitch steers the movement", d);

    // A wall Fly stops at, noclip passes.
    const int wx = bx + 8;
    fill(wx, wx, bz - 2, bz + 2, by - 2, by + 3, TYPE_STONE);
    fly_set_state(1);
    at(bx + 0.5f, by + 0.5f, bz + 0.5f);
    push_key(SDL_SCANCODE_W, true);
    tick(120);
    push_key(SDL_SCANCODE_W, false);
    const float flyX = pl->pos.x;
    fly_set_state(2);
    at(bx + 0.5f, by + 0.5f, bz + 0.5f);
    push_key(SDL_SCANCODE_W, true);
    tick(120);
    push_key(SDL_SCANCODE_W, false);
    const float ncX = pl->pos.x;
    std::snprintf(d, sizeof(d), "wall at x+%d: Fly reached x+%.1f, noclip x+%.1f", wx - bx, flyX - bx, ncX - bx);
    check(flyX < wx && ncX > wx + 2, "noclip passes a wall Fly stops at", d);

    // Unstick: inside solid stone pops up to air; in open air leaving noclip moves nothing.
    tick(30);
    at(wx + 0.5f, by + 0.5f, bz + 0.5f);   // inside the wall (noclip still on)
    tick(10);
    y0 = pl->pos.y;
    const bool wasBlocked = pl->boxBlocked();
    fly_set_state(0);
    std::snprintf(d, sizeof(d), "blocked %d -> %d, y %.2f -> %.2f", wasBlocked, (int)pl->boxBlocked(), y0, pl->pos.y);
    check(wasBlocked && !pl->boxBlocked() && pl->pos.y - y0 <= 7.0f && pl->pos.y > y0,
          "leaving noclip inside stone pops up to air", d);
    fly_set_state(2);
    at(bx + 40.5f, by + 0.5f, bz + 0.5f);
    tick(10);
    y0 = pl->pos.y;
    fly_set_state(0);
    std::snprintf(d, sizeof(d), "y %.3f -> %.3f", y0, pl->pos.y);
    check(pl->pos.y == y0, "leaving noclip in open air does not drop to the floor", d);

    // SRC.0: momentum carries out of noclip in open air (an unstick above still zeroes it).
    fly_set_state(2);
    at(bx + 40.5f, by + 0.5f, bz + 0.5f);
    pl->vel = MakeVector(6.0f, 0.0f, 3.0f);
    const float vx0 = pl->vel.x, vz0 = pl->vel.z;
    fly_set_state(0);
    std::snprintf(d, sizeof(d), "vel (%.3f, %.3f, %.3f) from (%.3f, 0, %.3f)", pl->vel.x, pl->vel.y, pl->vel.z, vx0, vz0);
    check(std::fabs(pl->vel.x - vx0) < vx0 * 0.01f && std::fabs(pl->vel.z - vz0) < vz0 * 0.01f,
          "leaving noclip in open air keeps horizontal momentum", d);

    // Lava: noclip is invulnerable, Fly is not.
    const int lx = bx + 60;
    fill(lx - 1, lx + 1, bz - 1, bz + 1, by - 1, by + 2, TYPE_LAVA);
    auto lava_life = [&](int state) {
        fly_set_state(state);
        at(lx + 0.5f, by + 0.5f, bz + 0.5f);
        pl->life = 1; pl->dead = false;
        float lo = 1;
        for (int i = 0; i < 120; ++i) { tick(1); if (pl->life < lo) lo = pl->life; }
        return lo;
    };
    const float ncLife = lava_life(2), flyLife = lava_life(1);
    fill(lx - 1, lx + 1, bz - 1, bz + 1, by - 1, by + 2, TYPE_NONE);
    fly_set_state(0);
    pl->life = 1; pl->dead = false;
    tick(10);
    std::snprintf(d, sizeof(d), "lowest life in 2 s of lava: noclip %.3f, Fly %.3f", ncLife, flyLife);
    check(ncLife == 1.0f && flyLife < 0.99f, "noclip is invulnerable (lava), Fly is not", d);

    // The y floor: diving down through the ground stops at y 1, no fall-out respawn.
    fly_set_state(2);
    at(bx + 20.5f, 12.0f, bz + 0.5f);
    pl->pitch = -80;
    push_key(SDL_SCANCODE_W, true);
    push_key(SDL_SCANCODE_LCTRL, true);
    float yMin = 1e9f, yMax = -1e9f;
    for (int i = 0; i < 180; ++i) {
        tick(1);
        if (pl->pos.y < yMin) yMin = pl->pos.y;
        if (pl->pos.y > yMax) yMax = pl->pos.y;
    }
    push_key(SDL_SCANCODE_W, false);
    push_key(SDL_SCANCODE_LCTRL, false);
    std::snprintf(d, sizeof(d), "y stayed in [%.2f, %.2f] diving for 3 s from y 12", yMin, yMax);
    check(yMin >= 1.0f && yMin < 1.5f && yMax <= 12.5f, "noclip is clamped at y 1 (no respawn)", d);

    // A long noclip run: streaming keeps up, no stall inside solids. Sprint along +x at mid height
    // (through hills) for 20 s; the player must cover the distance and land in loaded columns.
    at(kMapCentre - 300.0f, (float)g_opt.height / 3.0f, kMapCentre + 0.5f);
    tick(30);
    const float lx0 = pl->pos.x;
    push_key(SDL_SCANCODE_W, true);
    push_key(SDL_SCANCODE_LSHIFT, true);
    const Uint64 t0 = SDL_GetTicks();
    int unloaded = 0;   // frames whose own column was not resident yet (stream behind the player)
    for (int i = 0; i < 1200; ++i) {
        tick(1);
        if (getLandc2((int)pl->pos.x, (int)pl->pos.z, 0) <= 0) ++unloaded;
    }
    const Uint64 ms = SDL_GetTicks() - t0;
    push_key(SDL_SCANCODE_W, false);
    push_key(SDL_SCANCODE_LSHIFT, false);
    tick(120);
    const float ran = pl->pos.x - lx0;
    const int below = getLandc2((int)pl->pos.x, (int)pl->pos.z, 0);
    std::snprintf(d, sizeof(d), "%.0f blocks in 20 s engine time (%.1f s wall), own column unloaded "
                  "%d/1200 frames, bedrock below after = %d", ran, ms / 1000.0f, unloaded, below);
    check(ran > NOCLIP_SPEED * 18.0f && unloaded == 0 && below > 0, "a long noclip run: streaming keeps up", d);
    fly_set_state(0);

    g_app->setFixedEtime(0.0f);
    std::printf("[eden-fly] %s (%d failure(s))\n", g_selftestFailures ? "FAILURES" : "ALL PASS", g_selftestFailures);
    return g_selftestFailures ? 1 : 0;
}

// ---------------------------------------------------------------------------------------------
// --gamepad-selftest
// ---------------------------------------------------------------------------------------------
// Drives a REAL SDL virtual gamepad rather than calling the translator's internals, so the state
// travels the same path a physical controller's does — SDL's own joystick layer, the gamepad
// mapping database, SDL_GetGamepadAxis/Button — and then asserts the ENGINE moved. Same contract
// as --input-selftest, and the native counterpart of web-port-final:web/tools/headless-gamepad-test.js.
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
// Stage SRC (WORKING/srclike-movement-plan-2026-10-31.md). SRC.2 leg `input`: the input half of Source
// movement. The gates are the plan's SRC.2 row: the wheel cases of §4.6 (two notches in one frame = one
// attempt, a notch between ticks = one attempt, a notch in the air does nothing and is not banked, a
// notch on the landing frame hops), the hotbar off the wheel in the mode, autojump off, Auto-bhop, raw
// key axes, crouch implied, the move.* cvars. SRC.3 leg `trace`: the swept hull trace (below). SRC.4 leg
// `move`: the core tick against the SRC.1 vectors, and the frame driver (further below).
// ---------------------------------------------------------------------------------------------
static void source_input_leg();
static void srclike_trace_leg();
static void srclike_move_leg();
static void srclike_effects_leg();
static void srclike_special_leg();

int run_srclike_selftest() {
    g_selftestTag = "eden-srclike";
    g_tickInput = true;
    const std::string leg = g_opt.sourceLeg;
    if (!leg.empty() && leg != "input" && leg != "trace" && leg != "move" && leg != "effects" && leg != "special") {
        std::fprintf(stderr, "[eden-srclike] unknown leg '%s' (input, trace, move, effects, special)\n", leg.c_str());
        return 1;
    }
    const char* name = g_opt.world.empty() ? "srclike-selftest" : g_opt.world.c_str();
    if (!open_world(name, g_opt.height)) return 1;
    g_app->setFixedEtime(1.0f / 60.0f);
    tick(60);
    if (leg.empty() || leg == "input") source_input_leg();
    if (leg.empty() || leg == "trace") srclike_trace_leg();
    if (leg.empty() || leg == "move") srclike_move_leg();
    if (leg.empty() || leg == "effects") srclike_effects_leg();
    if (leg.empty() || leg == "special") srclike_special_leg();
    set_setting_by_key("srclike_movement", 0);
    g_app->setFixedEtime(0.0f);
    std::printf("[eden-srclike] %s (%d failure(s))\n", g_selftestFailures ? "FAILURES" : "ALL PASS", g_selftestFailures);
    return g_selftestFailures ? 1 : 0;
}

static void source_input_leg() {
    Player* pl = World::getWorld->player;
    char d[240];
    auto fill = [](int x0, int x1, int z0, int z1, int y0, int y1, int type) {
        for (int x = x0; x <= x1; ++x)
            for (int z = z0; z <= z1; ++z)
                for (int y = y0; y <= y1; ++y) eden_console_setblock(x, z, y, type);
    };
    const int bx = (int)kMapCentre, bz = (int)kMapCentre, by = g_opt.height - 8;
    eden_console_teleport(bx + 0.5f, by + 0.5f, bz + 0.5f);   // go there FIRST: a teleport re-centres the window and drops edits made out of it
    tick(120);
    fill(bx - 2, bx + 40, bz - 3, bz + 3, by - 1, by + 6, TYPE_NONE);   // an open room
    fill(bx - 2, bx + 40, bz - 3, bz + 3, by - 2, by - 2, TYPE_STONE);  // its floor
    auto stand = [&]() {
        eden_console_teleport(bx + 0.5f, by + 0.2f, bz + 0.5f);
        tick(2);
        pl->vel = MakeVector(0, 0, 0);
        pl->yaw = 0; pl->pitch = 0;   // yaw 0 faces +x
        tick(40);                      // settle on the floor
    };
    const float floorY = (float)(by - 1);   // top of the stone floor
    auto rise = [&](int n) {               // highest the feet got over n frames, above the floor
        float hi = -1e9f;
        for (int i = 0; i < n; ++i) { tick(1); hi = std::max(hi, pl->pos.y - pl->boxheight / 2 - floorY); }
        return hi;
    };

    // Gate 1: mode off: the wheel scrolls the hotbar and makes no jump pulse.
    set_setting_by_key("srclike_movement", 0);
    eden_select_hotbar_slot(3);
    eden_wheel_action(1);
    const bool pulseOff = SrclikeMove::takeJump(false);
    std::snprintf(d, sizeof(d), "hotbar %d, pulse %d", eden_get_hotbar_index(), (int)pulseOff);
    check(eden_get_hotbar_index() == 4 && !pulseOff, "mode off: wheel scrolls the hotbar, no jump pulse", d);

    // Gate 2: settings. Auto-bhop is a child row of Srclike movement; both reach SrclikeMove.
    set_setting_by_key("srclike_movement", 1);
    set_setting_by_key("srclike_autobhop", 1);
    const bool on = SrclikeMove::enabled() && SrclikeMove::autoBhop();
    set_setting_by_key("srclike_autobhop", 0);
    std::snprintf(d, sizeof(d), "enabled+autobhop %d, autobhop after off %d", (int)on, (int)SrclikeMove::autoBhop());
    check(on && !SrclikeMove::autoBhop(), "Experiments rows reach SrclikeMove", d);

    // Gate 3: mode on: the wheel does not scroll the hotbar, in either direction; many notches = one press.
    eden_select_hotbar_slot(3);
    eden_wheel_action(1); eden_wheel_action(-1); eden_wheel_action(1);
    const unsigned p0 = SrclikeMove::pressCount();
    const bool a = SrclikeMove::takeJump(false), b = SrclikeMove::takeJump(false);
    std::snprintf(d, sizeof(d), "hotbar %d, first take %d second take %d, presses +%u", eden_get_hotbar_index(), (int)a, (int)b, SrclikeMove::pressCount() - p0);
    check(eden_get_hotbar_index() == 3 && a && !b && SrclikeMove::pressCount() - p0 == 1,
          "mode on: wheel leaves the hotbar; three notches in one frame are one press", d);

    // Gate 4: a notch on the ground jumps (once), a notch in the air does nothing and is not banked.
    stand();
    const unsigned q0 = SrclikeMove::pressCount();
    eden_wheel_action(1); eden_wheel_action(1);   // two notches before the frame
    const float hop = rise(70);
    const unsigned q1 = SrclikeMove::pressCount();
    std::snprintf(d, sizeof(d), "apex %.2f blocks, presses +%u", hop, q1 - q0);
    check(hop > 0.9f && hop < 1.4f && q1 - q0 == 1, "wheel on the ground: one hop of ~1.1 blocks", d);
    stand();
    eden_wheel_action(-1);
    tick(1);   // takes off
    tick(8);   // in the air
    const unsigned r0 = SrclikeMove::pressCount();
    eden_wheel_action(1);
    const float hop2 = rise(80) ;
    tick(30);
    const bool grounded = std::fabs(pl->pos.y - pl->boxheight / 2 - floorY) < 0.15f;
    std::snprintf(d, sizeof(d), "airborne notch: presses +%u, apex stayed %.2f, grounded %d after landing + 30 frames",
                  SrclikeMove::pressCount() - r0, hop2, (int)grounded);
    check(SrclikeMove::pressCount() - r0 == 1 && hop2 < 1.4f && grounded, "wheel in the air is spent, not banked for the landing", d);

    // Gate 5: a notch on the frame after landing hops.
    stand();
    eden_wheel_action(1);
    tick(2);
    int guard = 0;
    auto feet = [&]() { return pl->pos.y - pl->boxheight / 2 - floorY; };
    while (feet() < 0.3f && guard++ < 200) tick(1);          // off the floor
    while (feet() > 0.001f && guard++ < 400) tick(1);        // ...and back down: resting on the floor
    tick(1);
    const float before = pl->pos.y;
    eden_wheel_action(1);
    const float hop3 = rise(60);
    std::snprintf(d, sizeof(d), "landed at y %.2f, next apex %.2f", before, hop3);
    check(hop3 > 0.9f, "a notch right after landing hops again", d);

    // Gate 6: holding Space: Auto-bhop off = one hop; on = repeated hops.
    auto holdHops = [&]() {   // takeoffs while Space is held for 3 s
        stand();
        int hops = 0;
        bool air = false;
        push_key(SDL_SCANCODE_SPACE, true);
        for (int i = 0; i < 180; ++i) {
            tick(1);
            const bool up = pl->pos.y - pl->boxheight / 2 - floorY > 0.15f;
            if (up && !air) ++hops;
            air = up;
        }
        push_key(SDL_SCANCODE_SPACE, false);
        tick(60);
        return (unsigned)hops;
    };
    const unsigned held = holdHops();
    set_setting_by_key("srclike_autobhop", 1);
    const unsigned auto_ = holdHops();
    set_setting_by_key("srclike_autobhop", 0);
    std::snprintf(d, sizeof(d), "3 s of held jump: Auto-bhop off %u hop(s), on %u", held, auto_);
    check(held == 1 && auto_ >= 3, "held jump hops once; with Auto-bhop it repeats", d);

    // Gate 7: autojump (Settings: on by default) hops a 1-block step in stock movement, not in the mode.
    auto stepClimb = [&]() {
        fill(bx + 6, bx + 12, bz - 3, bz + 3, by - 1, by - 1, TYPE_STONE);   // a 1-block step
        stand();
        push_key(SDL_SCANCODE_W, true);
        float y = 0;   // highest the feet got while walking over the step
        for (int i = 0; i < 100; ++i) { tick(1); y = std::max(y, pl->pos.y - pl->boxheight / 2 - floorY); }
        push_key(SDL_SCANCODE_W, false);
        fill(bx + 6, bx + 12, bz - 3, bz + 3, by - 1, by - 1, TYPE_NONE);
        return y;
    };
    set_setting_by_key("autojump", 1);
    set_setting_by_key("srclike_movement", 0);
    const float yOff = stepClimb();
    set_setting_by_key("srclike_movement", 1);
    const float yOn = stepClimb();
    std::snprintf(d, sizeof(d), "step 1 block high: feet %.2f above the floor with the mode off, %.2f on", yOff, yOn);
    check(yOff > 0.8f && yOn < 0.3f, "autojump climbs the step in stock, is off in Srclike movement", d);

    // Gate 8: raw key axes.
    stand();
    push_key(SDL_SCANCODE_W, true); tick(3);
    const SrclikeMove::UserCmd w = SrclikeMove::lastCmd();
    push_key(SDL_SCANCODE_D, true); tick(3);
    const SrclikeMove::UserCmd wd = SrclikeMove::lastCmd();
    push_key(SDL_SCANCODE_W, false); tick(3);
    const SrclikeMove::UserCmd dOnly = SrclikeMove::lastCmd();
    push_key(SDL_SCANCODE_D, false); tick(3);
    const SrclikeMove::UserCmd none = SrclikeMove::lastCmd();
    std::snprintf(d, sizeof(d), "W (%.2f,%.2f)  W+D (%.2f,%.2f)  D (%.2f,%.2f)  none (%.2f,%.2f)  yaw %.1f",
                  w.forward, w.side, wd.forward, wd.side, dOnly.forward, dOnly.side, none.forward, none.side, w.yaw);
    check(w.forward == 1 && w.side == 0 && wd.forward == 1 && std::fabs(std::fabs(wd.side) - 1) < 0.001f &&
          dOnly.forward == 0 && std::fabs(std::fabs(dOnly.side) - 1) < 0.001f && none.forward == 0 && none.side == 0 && w.yaw == pl->yaw,
          "usercmd carries raw +-1 key axes and the view yaw", d);

    // Gate 9: crouch is implied in the mode even with Crouch mode off.
    set_setting_by_key("crouch", 0);
    stand();
    const float hStand = pl->boxheight;
    push_key(SDL_SCANCODE_LCTRL, true); tick(40);
    const float hOn = pl->boxheight;
    push_key(SDL_SCANCODE_LCTRL, false); tick(40);
    set_setting_by_key("srclike_movement", 0);
    stand();
    push_key(SDL_SCANCODE_LCTRL, true); tick(40);
    const float hOff = pl->boxheight;
    push_key(SDL_SCANCODE_LCTRL, false); tick(40);
    std::snprintf(d, sizeof(d), "box height standing %.2f, Ctrl in the mode %.2f, Ctrl with mode and Crouch mode off %.2f", hStand, hOn, hOff);
    check(hOn < hStand * 0.7f && hOff > hStand * 0.95f, "Srclike movement implies crouch; off, Crouch mode off still means no crouch", d);

    // Gate 10: the move.* cvars: defaults, set, reset.
    Console::init();
    const SrclikeMove::Tunables& t = SrclikeMove::tunables();
    const bool defOk = t.accelerate == 5 && t.airaccelerate == 100 && t.friction == 4 && t.stopspeed == 75 &&
                       t.gravity == 800 && t.maxvelocity == 3500 && std::fabs(t.tickrate - 66.67f) < 0.01f;
    Console::run("move.airaccelerate 10");
    const float set10 = t.airaccelerate;
    Console::run("move.gravity 600");
    const float g600 = t.gravity;
    Console::run("reset move");
    std::snprintf(d, sizeof(d), "defaults ok %d, airaccelerate set -> %.0f, gravity set -> %.0f, after reset %.0f / %.0f",
                  (int)defOk, set10, g600, t.airaccelerate, t.gravity);
    check(defOk && set10 == 10 && g600 == 600 && t.airaccelerate == 100 && t.gravity == 800, "move.* cvars: SRC.1 defaults, settable, resettable", d);
    set_setting_by_key("srclike_movement", 0);
}

// ---------------------------------------------------------------------------------------------
// SRC.3 leg `trace`: SrclikeMove::traceHull against real resident terrain (plan D5, row SRC.3). A room is
// built ON the toroidal wrap seam (x and z both multiples of T_SIZE, so cell wx-1 lives in slot T_SIZE-1
// and cell wx in slot 0; T_SIZE is a multiple of 16, so it is a chunk boundary too). Expected values are
// worked out from the geometry, not from the trace's own plane formula, except where noted.
// Units: Source (u), z up; Eden cell (x, z, y) spans Source [x, z, y]*U .. +U.
// ---------------------------------------------------------------------------------------------
static void srclike_trace_leg() {
    using namespace SrclikeMove;
    char d[300];
    const double U = kUnitsPerBlock, eps = kDistEpsilon, hw = U / 3.0, H = 72.0, r2 = std::sqrt(2.0);
    const Vec3d mins{-hw, -hw, 0}, maxs{hw, hw, H};
    const int T = T_SIZE;
    const int wx = ((int)kMapCentre / T + 1) * T, wz = wx;   // 65664 at the default radius
    const int fy = g_opt.height - 10;                           // the floor layer; its top is Zf
    const double Zf = (fy + 1) * U;
    auto fill = [](int x0, int x1, int z0, int z1, int y0, int y1, int type) {
        for (int x = x0; x <= x1; ++x)
            for (int z = z0; z <= z1; ++z)
                for (int y = y0; y <= y1; ++y) eden_console_setblock(x, z, y, type);
    };
    eden_console_teleport(wx + 0.5f, fy + 3.0f, wz + 12.5f);   // re-centre the window first (edits outside it are dropped)
    tick(120);
    fill(wx - 13, wx + 13, wz - 13, wz + 13, fy + 1, fy + 8, TYPE_NONE);
    fill(wx - 13, wx + 13, wz - 13, wz + 13, fy, fy, TYPE_STONE);
    auto near = [](double a, double b, double tol = 1e-6) { return std::fabs(a - b) <= tol; };
    auto V = [](double x, double y, double z) { return Vec3d{x, y, z}; };
    auto trn = [&](const Trace& t) {   // "frac f n(x,y,z) end(x,y,z) ss/as type"
        char b[200];
        std::snprintf(b, sizeof(b), "frac %.6f n(%.3f,%.3f,%.3f) end(%.4f,%.4f,%.4f) ss%d as%d type %d @%d,%d,%d",
                      t.fraction, t.normal.x, t.normal.y, t.normal.z, t.endpos.x, t.endpos.y, t.endpos.z,
                      (int)t.startsolid, (int)t.allsolid, t.type, t.bx, t.bz, t.by);
        return std::string(b);
    };
    std::snprintf(d, sizeof(d), "T_SIZE %d, seam x=z=%d (mod T %d, mod 16 %d), floor y %d", T, wx, wx % T, wx % 16, fy);
    check(wx % T == 0 && wx % 16 == 0 && getLandc2(wx, wz, fy) == TYPE_STONE && getLandc2(wx - 1, wz - 1, fy + 1) == TYPE_NONE,
          "trace room built on the toroidal wrap seam", d);

    // Gate 1: brush geometry = the stock collision shapes, every ramp and side-ramp id. The stock
    // Polyhedra's vertices (Eden coords, makeX(x, x+1, z+1, z, y, y+1, type%4) as Player::vertc calls it)
    // must be exactly the cube corners our brush keeps.
    {
        int bad = 0, firstBad = -1;
        for (int type = TYPE_STONE_RAMP1; type <= TYPE_ICE_SIDE4; ++type) {
            const int x = 3, z = 5, y = 7;
            const bool ramp = type <= TYPE_ICE_RAMP4;
            Polyhedra p = ramp ? makeRamp(x, x + 1, z + 1, z, y, y + 1, type % 4) : makeSide(x, x + 1, z + 1, z, y, y + 1, type % 4);
            Plane pl[7];
            const int np = blockBrush(type, x, z, y, pl);
            Vec3d keep[8]; int nk = 0;
            for (int c = 0; c < 8; ++c) {
                const Vec3d q{(x + (c & 1)) * U, (z + ((c >> 1) & 1)) * U, (y + ((c >> 2) & 1)) * U};
                bool in = true;
                for (int i = 0; i < np; ++i) in = in && (pl[i].n.x * q.x + pl[i].n.y * q.y + pl[i].n.z * q.z <= pl[i].d + 1e-6);
                if (in) keep[nk++] = q;
            }
            bool ok = np == 7 && nk == p.n_points;
            for (int i = 0; ok && i < p.n_points; ++i) {
                const Vec3d s = toSource(p.points[i].x, p.points[i].y, p.points[i].z);
                bool found = false;
                for (int k = 0; k < nk; ++k) found = found || (near(s.x, keep[k].x, 1e-4) && near(s.y, keep[k].y, 1e-4) && near(s.z, keep[k].z, 1e-4));
                ok = found;
            }
            if (!ok) { ++bad; if (firstBad < 0) firstBad = type; }
        }
        std::snprintf(d, sizeof(d), "%d of 32 ramp/side ids differ (first %d)", bad, firstBad);
        check(bad == 0, "brushes match stock makeRamp/makeSide vertex for vertex", d);
    }

    // Gate 2: drop onto the floor, the hull straddling both seams (4 columns in 4 toroidal slots).
    {
        const Trace t = traceHull(V(wx * U, wz * U, Zf + 10), V(wx * U, wz * U, Zf - 30), mins, maxs);
        check(near(t.endpos.z, Zf + eps) && near(t.normal.z, 1) && !t.startsolid && t.type == TYPE_STONE && t.by == fy,
              "drop onto a cube floor across the wrap seam stops one DIST_EPSILON above it", trn(t).c_str());
    }
    // Gate 3: the ground probe (2 u down from resting) hits at fraction 0, not start-solid; touching
    // exactly IS start-solid (plan §8.2.4).
    {
        const Trace rest = traceHull(V(wx * U, wz * U, Zf + eps), V(wx * U, wz * U, Zf + eps - 2), mins, maxs);
        const Trace touch = traceHull(V(wx * U, wz * U, Zf), V(wx * U, wz * U, Zf - 2), mins, maxs);
        std::snprintf(d, sizeof(d), "rest: %s | touch ss %d", trn(rest).c_str(), (int)touch.startsolid);
        check(rest.fraction == 0 && near(rest.normal.z, 1) && !rest.startsolid && touch.startsolid,
              "ground probe from rest hits at 0; exact contact is start-solid", d);
    }
    // Gate 4: sliding along the floor across cube seams and both wrap seams, exactly level and with a
    // float-noise dip, is not a hit (the d2 >= d1 - tol rule, plan §8.2.3b).
    {
        const Trace a = traceHull(V((wx - 4) * U, (wz - 4) * U, Zf + eps), V((wx + 3) * U, (wz + 3) * U, Zf + eps), mins, maxs);
        const Trace b = traceHull(V((wx - 4) * U, wz * U, Zf + eps), V((wx + 3) * U, wz * U, Zf + eps - 1e-9), mins, maxs);
        std::snprintf(d, sizeof(d), "level: %s | dip: frac %.6f ss %d", trn(a).c_str(), b.fraction, (int)b.startsolid);
        check(a.fraction == 1 && !a.startsolid && b.fraction == 1 && !b.startsolid, "slide along a cube floor across seams is clear", d);
    }
    // Gate 5: axis walls (2 blocks high) in all four directions: stop hw + eps short, axial normal.
    {
        fill(wx + 4, wx + 4, wz, wz, fy + 1, fy + 2, TYPE_STONE);       // +x
        fill(wx - 5, wx - 5, wz, wz, fy + 1, fy + 2, TYPE_STONE);       // -x
        fill(wx - 1, wx - 1, wz + 4, wz + 4, fy + 1, fy + 2, TYPE_STONE); // +y (Eden +z)
        fill(wx - 1, wx - 1, wz - 5, wz - 5, fy + 1, fy + 2, TYPE_STONE); // -y
        const double py = (wz + 0.5) * U, px = (wx - 0.5) * U, z = Zf + eps;
        const Trace xp = traceHull(V((wx + 1) * U, py, z), V((wx + 6) * U, py, z), mins, maxs);
        const Trace xm = traceHull(V((wx - 1) * U, py, z), V((wx - 8) * U, py, z), mins, maxs);
        const Trace yp = traceHull(V(px, (wz + 1) * U, z), V(px, (wz + 6) * U, z), mins, maxs);
        const Trace ym = traceHull(V(px, (wz - 1) * U, z), V(px, (wz - 8) * U, z), mins, maxs);
        const bool ok = near(xp.endpos.x, (wx + 4) * U - hw - eps) && near(xp.normal.x, -1) &&
                        near(xm.endpos.x, (wx - 4) * U + hw + eps) && near(xm.normal.x, 1) &&
                        near(yp.endpos.y, (wz + 4) * U - hw - eps) && near(yp.normal.y, -1) &&
                        near(ym.endpos.y, (wz - 4) * U + hw + eps) && near(ym.normal.y, 1) &&
                        near(xp.endpos.z, z);
        std::snprintf(d, sizeof(d), "+x %.4f/%.4f -x %.4f/%.4f +y %.4f/%.4f -y %.4f/%.4f (got/want)",
                      xp.endpos.x, (wx + 4) * U - hw - eps, xm.endpos.x, (wx - 4) * U + hw + eps,
                      yp.endpos.y, (wz + 4) * U - hw - eps, ym.endpos.y, (wz - 4) * U + hw + eps);
        check(ok, "axis walls in four directions: stop hw + DIST_EPSILON short, axial normal", d);
    }
    // Gate 6: a ceiling: rising feet stop with the hull top one epsilon under it.
    {
        fill(wx + 8, wx + 8, wz + 8, wz + 8, fy + 4, fy + 4, TYPE_STONE);
        const double px = (wx + 8.5) * U, py = (wz + 8.5) * U;
        const Trace t = traceHull(V(px, py, Zf + eps), V(px, py, Zf + 3 * U), mins, maxs);
        check(near(t.endpos.z, (fy + 4) * U - H - eps) && near(t.normal.z, -1), "ceiling: hull top stops one DIST_EPSILON under it", trn(t).c_str());
    }
    // Gate 7: ramps, all four orientations (stone ramp ids, type%4 = 0..3). Dropped on the centre the
    // hull's high-side bottom edge meets the slope: feet z = Zf + U/2 + hw + eps*sqrt2 (the slope rises
    // one unit per unit; eps along a 45-degree normal is eps*sqrt2 vertically). From the low side at floor
    // level the slope is hit (nz 0.707, `ramp`) with the hull's leading face eps*(1-sqrt2) short of the low
    // edge (the feet ride eps above the floor, the expanded slope plane is eps off the slope); from the
    // high side the vertical back face is (axial).
    {
        int fails = 0;
        std::string why;
        for (int k = 0; k < 4; ++k) {
            const int type = TYPE_STONE_RAMP1 + k, bx = wx - 10, bz = wz - 10 + 4 * k;
            fill(bx, bx, bz, bz, fy + 1, fy + 1, type);
            const double cx = (bx + 0.5) * U, cy = (bz + 0.5) * U;
            const Trace drop = traceHull(V(cx, cy, (fy + 3) * U), V(cx, cy, Zf + eps), mins, maxs);
            // Uphill direction r: the slope normal's horizontal part points DOWNhill.
            const double rx = drop.normal.x < -0.1 ? 1 : (drop.normal.x > 0.1 ? -1 : 0);
            const double ry = drop.normal.y < -0.1 ? 1 : (drop.normal.y > 0.1 ? -1 : 0);
            const bool dropOk = near(drop.endpos.z, Zf + U / 2 + hw + eps * r2) && near(drop.normal.z, 1 / r2) &&
                                near(std::fabs(rx) + std::fabs(ry), 1) && drop.ramp && drop.type == type;
            // Low side, 2 blocks out, at floor level, moving uphill.
            const Trace up = traceHull(V(cx - 2 * U * rx, cy - 2 * U * ry, Zf + eps), V(cx, cy, Zf + eps), mins, maxs);
            const double lead = (up.endpos.x * rx + up.endpos.y * ry) + hw;          // hull's leading face along r
            const double low = (cx * rx + cy * ry) - U / 2;                          // ramp's low edge along r
            const bool upOk = up.fraction < 1 && near(up.normal.z, 1 / r2) && up.ramp && near(lead - low, eps * (1 - r2));
            // High side, moving downhill into the back face.
            const Trace back = traceHull(V(cx + 2 * U * rx, cy + 2 * U * ry, Zf + eps), V(cx, cy, Zf + eps), mins, maxs);
            const double trail = (back.endpos.x * rx + back.endpos.y * ry) - hw;
            const bool backOk = near(back.normal.x, rx) && near(back.normal.y, ry) && near(back.normal.z, 0) && !back.ramp &&
                                near(trail, low + U + eps);
            if (!(dropOk && upOk && backOk)) {
                ++fails;
                char b[400];
                std::snprintf(b, sizeof(b), " [type %d drop %d (%s) up %d lead-low %.4f back %d trail-high %.4f]", type,
                              (int)dropOk, trn(drop).c_str(), (int)upOk, lead - low, (int)backOk, trail - (low + U));
                why += b;
            }
        }
        std::snprintf(d, sizeof(d), "%d of 4 orientations wrong%s", fails, why.substr(0, 220).c_str());
        check(fails == 0, "ramps, 4 orientations: drop on the slope, run up it, hit the back face", d);
    }
    // Gate 8: side-ramps, all four orientations. Coming in from the missing corner along the diagonal's
    // normal, the hull's corner meets the diagonal at the cell centre: centre distance sqrt2*hw + eps.
    {
        int fails = 0;
        std::string why;
        Plane pl[7];
        for (int k = 0; k < 4; ++k) {
            const int type = TYPE_STONE_SIDE1 + k, bx = wx + 8, bz = wz - 10 + 4 * k;
            fill(bx, bx, bz, bz, fy + 1, fy + 1, type);
            blockBrush(type, bx, bz, fy + 1, pl);
            const Vec3d n = pl[6].n;   // the diagonal (checked against stock in gate 1)
            const double cx = (bx + 0.5) * U, cy = (bz + 0.5) * U;
            const Trace t = traceHull(V(cx + 2 * U * n.x, cy + 2 * U * n.y, Zf + eps), V(cx, cy, Zf + eps), mins, maxs);
            const double dist = (t.endpos.x - cx) * n.x + (t.endpos.y - cy) * n.y;
            const bool ok = near(t.normal.x, n.x) && near(t.normal.y, n.y) && near(n.z, 0) && near(std::fabs(n.x), 1 / r2) &&
                            near(std::fabs(n.y), 1 / r2) && near(dist, r2 * hw + eps) && !t.ramp && t.type == type;
            if (!ok) {
                ++fails;
                char b[300];
                std::snprintf(b, sizeof(b), " [type %d dist %.4f want %.4f %s]", type, dist, r2 * hw + eps, trn(t).c_str());
                why += b;
            }
        }
        std::snprintf(d, sizeof(d), "%d of 4 orientations wrong%s", fails, why.substr(0, 220).c_str());
        check(fails == 0, "side-ramps, 4 orientations: diagonal face hit from the missing corner", d);
    }
    // Gate 9: an outer corner. Diagonally at it: both axes stop hw + eps short; 1 u to the side: a miss.
    const int px = wx + 4, pz = wz + 6;   // a 2-high pillar
    fill(px, px, pz, pz, fy + 1, fy + 2, TYPE_STONE);
    {
        const double x0 = px * U, y0 = pz * U, z = Zf + eps;
        const Trace t = traceHull(V(x0 - hw - 20, y0 - hw - 20, z), V(x0 + 10, y0 + 10, z), mins, maxs);
        const Trace g = traceHull(V(x0 - hw - 40, y0 - hw - 1, z), V(x0 + U + hw + 40, y0 - hw - 1, z), mins, maxs);
        std::snprintf(d, sizeof(d), "at corner: %s | graze frac %.6f", trn(t).c_str(), g.fraction);
        check(near(t.endpos.x, x0 - hw - eps) && near(t.endpos.y, y0 - hw - eps) && t.fraction < 1 && g.fraction == 1,
              "outer corner: diagonal stops on both faces, a 1 u graze misses", d);
    }
    // Gate 10: start-solid. Inside the pillar: all-solid when the end is inside too; start-solid only
    // (the move is allowed out) when it is not.
    {
        const double cx = (px + 0.5) * U, cy = (pz + 0.5) * U, z = Zf + eps;
        const Trace in = traceHull(V(cx, cy, z), V(cx + 1, cy, z), mins, maxs);
        const Trace out = traceHull(V(cx, cy, z), V(cx - 3 * U, cy, z), mins, maxs);
        std::snprintf(d, sizeof(d), "in: %s | out: %s", trn(in).c_str(), trn(out).c_str());
        check(in.startsolid && in.allsolid && in.fraction == 0 && out.startsolid && !out.allsolid && out.fraction == 1,
              "start-solid / all-solid", d);
    }
    // Gate 11: walls on either side of the seams: the last slot (T_SIZE-1) and the first (0), in x and z.
    {
        const int row = wz + 10;
        fill(wx - 1, wx, row, row, fy + 1, fy + 2, TYPE_STONE);
        const double y = (row + 0.5) * U, z = Zf + eps;
        const Trace a = traceHull(V((wx - 4) * U, y, z), V((wx + 3) * U, y, z), mins, maxs);   // +x, hits cell wx-1
        fill(wx - 1, wx - 1, row, row, fy + 1, fy + 2, TYPE_NONE);
        const Trace b = traceHull(V((wx - 4) * U, y, z), V((wx + 3) * U, y, z), mins, maxs);   // +x, now cell wx
        fill(wx, wx, row, row, fy + 1, fy + 2, TYPE_NONE);
        fill(wx + 11, wx + 11, wz, wz, fy + 1, fy + 2, TYPE_STONE);
        const double x = (wx + 11.5) * U;
        const Trace c = traceHull(V(x, (wz - 4) * U, z), V(x, (wz + 3) * U, z), mins, maxs);   // +y, cell wz
        fill(wx + 11, wx + 11, wz, wz, fy + 1, fy + 2, TYPE_NONE);
        fill(wx + 11, wx + 11, wz - 1, wz - 1, fy + 1, fy + 2, TYPE_STONE);
        const Trace e = traceHull(V(x, (wz + 3) * U, z), V(x, (wz - 4) * U, z), mins, maxs);   // -y, cell wz-1
        std::snprintf(d, sizeof(d), "x: slot T-1 %.4f (cell %d) slot 0 %.4f (cell %d); y: slot 0 %.4f (cell %d) slot T-1 from +y %.4f (cell %d)",
                      a.endpos.x - ((wx - 1) * U - hw - eps), a.bx, b.endpos.x - (wx * U - hw - eps), b.bx,
                      c.endpos.y - (wz * U - hw - eps), c.bz, e.endpos.y - (wz * U + hw + eps), e.bz);
        check(near(a.endpos.x, (wx - 1) * U - hw - eps) && a.bx == wx - 1 && near(b.endpos.x, wx * U - hw - eps) && b.bx == wx &&
              near(c.endpos.y, wz * U - hw - eps) && c.bz == wz && near(e.endpos.y, wz * U + hw + eps) && e.bz == wz - 1,
              "walls either side of the toroidal/chunk seam, x and z", d);
    }
    // Gate 12: not solid: water, lava, a door, a flower, a golden cube. A ladder is (stock: you climb it by
    // pushing into it).
    {
        const int types[6] = {TYPE_WATER, TYPE_LAVA, TYPE_DOOR1, TYPE_FLOWER, TYPE_GOLDEN_CUBE, TYPE_LADDER};
        std::string got;
        bool ok = true;
        for (int i = 0; i < 6; ++i) {
            const int bx = wx - 12 + 2 * i, bz = wz + 8;
            fill(bx, bx, bz, bz, fy + 1, fy + 1, types[i]);
            const bool placed = getLandc2(bx, bz, fy + 1) == types[i];
            const double x = (bx + 0.5) * U, z = Zf + eps;
            const Trace t = traceHull(V(x, (bz - 2) * U, z), V(x, (bz + 3) * U, z), mins, maxs);
            const bool want = types[i] == TYPE_LADDER;
            ok = ok && placed && ((t.fraction < 1) == want) && (!want || t.type == TYPE_LADDER);
            char b[60];
            std::snprintf(b, sizeof(b), " %d:%s%.3f", types[i], placed ? "" : "(not placed)", t.fraction);
            got += b;
        }
        fill(wx - 12, wx - 2, wz + 8, wz + 8, fy + 1, fy + 2, TYPE_NONE);   // before any tick lets the liquids flow
        std::snprintf(d, sizeof(d), "type:fraction%s", got.c_str());
        check(ok, "liquids, doors, flowers, golden cubes are not solid; ladders are", d);
    }
    // Gate 13: a long fall across many cells (ten blocks of x, six down) lands on the floor.
    {
        const double y = (wz + 3.5) * U;
        const Trace t = traceHull(V((wx - 6) * U, y, (fy + 7) * U), V((wx + 4) * U, y, Zf - U), mins, maxs);
        check(near(t.endpos.z, Zf + eps, 1e-5) && near(t.normal.z, 1) && t.fraction > 0 && t.fraction < 1,
              "long diagonal fall over many cells lands on the floor", trn(t).c_str());
    }
    // Cost (plan risk "Air 2 cost"; informational): a ground probe and a 1-tick run at 3500 u/s.
    {
        const int n = 20000;
        const auto t0 = std::chrono::steady_clock::now();
        double sink = 0;
        for (int i = 0; i < n; ++i) {
            const double o = (i % 7) * 0.5;
            sink += traceHull(V(wx * U + o, wz * U, Zf + eps), V(wx * U + o, wz * U, Zf + eps - 2), mins, maxs).fraction;
            sink += traceHull(V(wx * U + o, wz * U, Zf + eps), V(wx * U + o + 52.5, wz * U, Zf + eps), mins, maxs).fraction;
        }
        const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / (2.0 * n);
        std::printf("[eden-srclike] trace cost: %.3f us per trace (ground probe + 52 u run; sink %.0f)\n", us, sink);
    }
    fill(wx - 13, wx + 13, wz - 13, wz + 13, fy + 1, fy + 8, TYPE_NONE);
}


// ---------------------------------------------------------------------------------------------
// SRC.4 leg `move`: the core tick (SrclikeMove::pmTick) against port/tools/fixtures/srclike-pm/vectors.txt,
// the SRC.1 model's golden output, within 1 % (absolute floor 0.05, so tick counts must be exact). Each
// scenario is srclike-pm-ref.py's sc_* function rebuilt in voxels: its infinite floor is a stone slab,
// its one big 45-degree wedge a stair of Eden ramps with cubes under them (the same solid union), its
// empty world the sky above the top of the world. Positions translate; nothing else differs. Then the
// frame driver through real Player frames: tick rate, interpolation at 60 and 144 Hz, re-seeding on a
// teleport, adopting an outside velocity, lifting a start-solid seed.
// Cmd translation: the model's side>0 is Source "right" (sin y, -cos y); SrclikeMove's side follows Eden's
// walk_force.x, (-sin y, cos y), so side = -model side. The model's forward/side are in u/s (= maxspeed).
// ---------------------------------------------------------------------------------------------
namespace {
using namespace SrclikeMove;

std::map<std::string, std::string> g_vectors;   // "section.key" -> JSON text

bool load_vectors(const std::string& path) {
    std::ifstream f(path);
    if (!f) return false;
    std::string line, sec;
    while (std::getline(f, line)) {
        if (line.rfind("## ", 0) == 0) { sec = line.substr(3); continue; }
        const size_t eq = line.find(" = ");
        if (sec.empty() || sec == "constants" || eq == std::string::npos) continue;
        g_vectors[sec + "." + line.substr(0, eq)] = line.substr(eq + 3);
    }
    return !g_vectors.empty();
}
double vnum(const std::string& k) {
    auto it = g_vectors.find(k);
    if (it == g_vectors.end() || it->second == "null") return NAN;
    return std::strtod(it->second.c_str(), nullptr);
}
std::vector<double> varr(const std::string& k) {
    std::vector<double> out;
    auto it = g_vectors.find(k);
    if (it == g_vectors.end()) return out;
    const char* p = it->second.c_str();
    while (*p && *p != '[') ++p;
    while (*p && *p != ']') {
        ++p;
        char* e = nullptr;
        const double v = std::strtod(p, &e);
        if (e == p) break;
        out.push_back(v);
        p = e;
        while (*p == ' ') ++p;
    }
    return out;
}
bool within(double got, double want) {
    if (std::isnan(want)) return std::isnan(got);
    return std::fabs(got - want) <= std::max(0.01 * std::fabs(want), 0.05);
}

// One model Player: hull 2/3 block x 72 u, maxspeed = Eden walk, press-edge jump like try_jump.
struct Sim {
    PmState s;
    bool held = false;
    double dt = 0;
    void init(const Vec3d& o, const Vec3d& v) {
        s = PmState{};
        s.origin = o; s.vel = v;
        s.halfWidth = kUnitsPerBlock / 3.0; s.height = 72.0; s.speedMul = 1.0;
        s.onground = false;
        pmCategorize(s);
        held = false;
        dt = tickInterval();
    }
    void tick(double fwd, double side, double yaw, bool jump) {   // fwd/side in the MODEL's sense, units of maxspeed
        UserCmd c{(float)fwd, (float)-side, jump && !held, false, (float)yaw, 0.0f, jump};
        held = jump;
        pmTick(s, c, dt);
    }
    double hspeed() const { return std::hypot(s.vel.x, s.vel.y); }
    double vyaw() const { return std::atan2(s.vel.y, s.vel.x) * 180.0 / M_PI; }
};
}  // namespace

static void srclike_move_leg() {
    char d[600];
    const double U = kUnitsPerBlock, eps = kDistEpsilon;
    if (!load_vectors(g_opt.srclikeVectors)) {
        std::snprintf(d, sizeof(d), "cannot read %s (run from native/, or pass --srclike-vectors=)", g_opt.srclikeVectors.c_str());
        check(false, "SRC.1 vectors loaded", d);
        return;
    }
    const int c = (int)kMapCentre, cz = c, H = g_opt.height;
    const int fy = H - 12;                 // floor layer; its top is Zf
    const double Zf = (fy + 1) * U;
    auto fill = [](int x0, int x1, int z0, int z1, int y0, int y1, int type) {
        for (int x = x0; x <= x1; ++x)
            for (int z = z0; z <= z1; ++z)
                for (int y = y0; y <= y1; ++y) eden_console_setblock(x, z, y, type);
    };
    set_setting_by_key("srclike_movement", 0);
    eden_console_teleport(c + 0.5f, fy + 3.0f, cz + 0.5f);   // re-centre the window first
    tick(120);
    // F: the floor slab (model floor_world), x [c-64, c+40], z [cz-20, cz+4].
    fill(c - 64, c + 40, cz - 20, cz + 4, fy + 1, fy + 4, TYPE_NONE);
    fill(c - 64, c + 40, cz - 20, cz + 4, fy, fy, TYPE_STONE);
    // The ramp id rising toward Source +y (Eden +z), from the brushes gate 1 of `trace` pinned to stock.
    int rampUp = -1;
    for (int t = TYPE_STONE_RAMP1; t < TYPE_STONE_RAMP1 + 4; ++t) {
        Plane pl[7];
        blockBrush(t, 0, 0, 0, pl);
        if (pl[6].n.y < -0.7 && pl[6].n.z > 0.7) rampUp = t;
    }
    // R: model ramp_walk_world. Floor, a 4-block ramp stair from row Rz, a 4-high plateau beyond.
    const int Rx0 = c + 44, Rx1 = c + 64, Rz = cz - 6;
    fill(Rx0, Rx1, Rz - 10, Rz + 14, fy + 1, fy + 8, TYPE_NONE);
    fill(Rx0, Rx1, Rz - 10, Rz + 14, fy, fy, TYPE_STONE);
    for (int k = 0; k < 4; ++k) {
        if (k > 0) fill(Rx0, Rx1, Rz + k, Rz + k, fy + 1, fy + k, TYPE_STONE);
        fill(Rx0, Rx1, Rz + k, Rz + k, fy + 1 + k, fy + 1 + k, rampUp);
    }
    fill(Rx0, Rx1, Rz + 4, Rz + 14, fy + 1, fy + 4, TYPE_STONE);
    // S: model surf_world. An 11-block ramp stair from row Sz, base layer Sy0, long in x.
    const int Sx0 = c - 4, Sx1 = c + 26, Sz = cz + 10, K = 10, Sy0 = H - 16;
    fill(Sx0, Sx1, Sz - 2, Sz + K + 3, Sy0, Sy0 + K + 4, TYPE_NONE);
    for (int k = 0; k <= K; ++k) {
        if (k > 0) fill(Sx0, Sx1, Sz + k, Sz + k, Sy0, Sy0 + k - 1, TYPE_STONE);
        fill(Sx0, Sx1, Sz + k, Sz + k, Sy0 + k, Sy0 + k, rampUp);
    }
    std::snprintf(d, sizeof(d), "%zu vectors from %s; ramp id %d; floor y %d, ramp-walk row %d, surf row %d base %d",
                  g_vectors.size(), g_opt.srclikeVectors.c_str(), rampUp, fy, Rz, Sz, Sy0);
    check(rampUp >= 0 && getLandc2(c, cz, fy) == TYPE_STONE && getLandc2(Rx0, Rz + 2, fy + 3) == rampUp &&
          getLandc2(Sx1, Sz + K, Sy0 + K) == rampUp, "move rooms built (floor slab, ramp walk, surf ramp)", d);

    const double ms = kWalkSpeed;
    const Vec3d F0{(c + 0.5) * U, (cz + 0.5) * U, Zf};   // model floor_world origin
    auto onFloor = [&](Sim& p, double vx) { p.init(Vec3d{F0.x, F0.y, Zf + eps}, Vec3d{vx, 0, 0}); };
    Sim p;

    // Gate 1: ground acceleration from rest, W held (sc_ground_accel).
    {
        onFloor(p, 0);
        int m50 = 0, m90 = 0, m100 = 0;
        double dist = 0, at[68] = {0};
        for (int t = 1; t <= 67; ++t) {
            const double x0 = p.s.origin.x;
            p.tick(1, 0, 0, false);
            dist += p.s.origin.x - x0;
            const double sp = p.hspeed();
            at[t] = sp;
            if (!m50 && sp >= ms * 0.5 - 1e-6) m50 = t;
            if (!m90 && sp >= ms * 0.9 - 1e-6) m90 = t;
            if (!m100 && sp >= ms - 1e-6) m100 = t;
        }
        const char* g = "ground_accel.";
        auto k = [&](const char* n) { return vnum(std::string(g) + n); };
        const bool ok = within(at[1], k("speed_after_1_ticks")) && within(at[2], k("speed_after_2_ticks")) &&
                        within(at[5], k("speed_after_5_ticks")) && within(at[10], k("speed_after_10_ticks")) &&
                        within(at[20], k("speed_after_20_ticks")) && within(at[67], k("speed_after_67_ticks")) &&
                        within(m50, k("ticks_to_50pct")) && within(m90, k("ticks_to_90pct")) &&
                        within(m100, k("ticks_to_100pct")) && within(dist, k("distance_after_67_ticks_u")) && p.s.onground;
        std::snprintf(d, sizeof(d), "after 1/2/5/10/20 ticks %.3f/%.3f/%.3f/%.3f/%.3f (want %.3f/%.3f/%.3f/%.3f/%.3f); 50/90/100%% at %d/%d/%d (want %.0f/%.0f/%.0f); 67-tick distance %.3f (want %.3f)",
                      at[1], at[2], at[5], at[10], at[20], k("speed_after_1_ticks"), k("speed_after_2_ticks"), k("speed_after_5_ticks"),
                      k("speed_after_10_ticks"), k("speed_after_20_ticks"), m50, m90, m100, k("ticks_to_50pct"), k("ticks_to_90pct"),
                      k("ticks_to_100pct"), dist, k("distance_after_67_ticks_u"));
        check(ok, "ground accel curve matches the model", d);
    }
    // Gate 2: stopping from maxspeed and from 400 u/s with no input (sc_stop).
    {
        auto stop = [&](double v0, int* ticks) {
            onFloor(p, v0);
            double dist = 0;
            int t = 0;
            while (p.hspeed() > 0 && t < 1000) { const double x0 = p.s.origin.x; p.tick(0, 0, 0, false); dist += p.s.origin.x - x0; ++t; }
            *ticks = t;
            return dist;
        };
        int t1, t2;
        const double d1 = stop(ms, &t1), d2 = stop(400.0, &t2);
        const bool ok = within(t1, vnum("stop_from_maxspeed.ticks_to_stop")) && within(d1, vnum("stop_from_maxspeed.distance_u")) &&
                        within(t2, vnum("stop_from_400.ticks_to_stop")) && within(d2, vnum("stop_from_400.distance_u"));
        std::snprintf(d, sizeof(d), "from maxspeed %d ticks %.3f u (want %.0f, %.3f); from 400 %d ticks %.3f u (want %.0f, %.3f)",
                      t1, d1, vnum("stop_from_maxspeed.ticks_to_stop"), vnum("stop_from_maxspeed.distance_u"),
                      t2, d2, vnum("stop_from_400.ticks_to_stop"), vnum("stop_from_400.distance_u"));
        check(ok, "stop distance and time match the model", d);
    }
    // Gate 3: one jump (sc_jump): apex and airtime.
    {
        onFloor(p, 0);
        double apex = 0;
        int t = 0, landed = -1;
        bool jump = true;
        while (t < 200) {
            p.tick(0, 0, 0, jump);
            jump = false;
            ++t;
            apex = std::max(apex, p.s.origin.z - Zf);
            if (p.s.onground) { landed = t; break; }
        }
        const bool ok = within(apex, vnum("jump.model_apex_u")) && within(landed, vnum("jump.airtime_ticks"));
        std::snprintf(d, sizeof(d), "apex %.3f u, airtime %d ticks (want %.3f, %.0f)", apex, landed, vnum("jump.model_apex_u"), vnum("jump.airtime_ticks"));
        check(ok, "jump apex and airtime match the model", d);
    }
    // Gate 4: perfect air strafe in open air, 67 ticks from 250 u/s, airaccelerate 100 and 10.
    {
        Tunables& tu = tunables();
        auto air = [&](double aa) {
            tu.airaccelerate = (float)aa;
            p.init(Vec3d{F0.x, F0.y, (H + 36) * U}, Vec3d{250, 0, 0});   // above the top of the world: nothing to hit
            for (int i = 0; i < 67; ++i) p.tick(0, 1, p.vyaw(), false);
            return p.hspeed();
        };
        const double a100 = air(100), a10 = air(10);
        resetTunables();
        const bool ok = within(a100, vnum("air_strafe_open_aa100.v_after_u_s")) && within(a10, vnum("air_strafe_open_aa10.v_after_u_s"));
        std::snprintf(d, sizeof(d), "aa100 %.3f, aa10 %.3f (want %.3f, %.3f)", a100, a10,
                      vnum("air_strafe_open_aa100.v_after_u_s"), vnum("air_strafe_open_aa10.v_after_u_s"));
        check(ok, "perfect air strafe gain matches the model (sqrt(v^2 + 900 n))", d);
    }
    // Gate 5: perfect strafe-bhop, 10 hops from maxspeed (sc_strafe_hops).
    {
        onFloor(p, ms);
        std::vector<double> land;
        double side = 1;
        int h = 0, t = 0, fric = 0;
        while (h < 10 && t < 5000) {
            const bool jump = p.s.onground;
            p.tick(0, side, p.vyaw(), jump);
            ++t;
            if (!p.s.jumped && jump) ++fric;
            if (p.s.onground) { ++h; side = -side; land.push_back(p.hspeed()); }
        }
        const std::vector<double> want = varr("strafe_hops_aa100.landing_speeds_u_s");
        bool ok = land.size() == want.size() && want.size() == 10 && within(t / 10.0, vnum("strafe_hops_aa100.ticks_per_hop")) &&
                  fric == (int)vnum("strafe_hops_aa100.ground_friction_ticks");
        for (size_t i = 0; ok && i < want.size(); ++i) ok = within(land[i], want[i]);
        std::snprintf(d, sizeof(d), "landings %.1f %.1f %.1f ... %.1f (want %.1f %.1f %.1f ... %.1f), %.1f ticks/hop, %d friction ticks; final pos %.1f,%.1f blocks from start",
                      land.size() > 0 ? land[0] : 0.0, land.size() > 1 ? land[1] : 0.0, land.size() > 2 ? land[2] : 0.0, land.empty() ? 0.0 : land.back(),
                      want.size() > 2 ? want[0] : 0.0, want.size() > 2 ? want[1] : 0.0, want.size() > 2 ? want[2] : 0.0, want.empty() ? 0.0 : want.back(),
                      t / 10.0, fric, (p.s.origin.x - F0.x) / U, (p.s.origin.y - F0.y) / U);
        check(ok, "strafe-bhop gain per hop matches the model", d);
    }
    // Gate 6: bhop retention with no input, 0 / 1 / 3 ticks late (sc_bhop_retention).
    {
        std::string got;
        bool ok = true;
        const char* names[3] = {"bhop_retention_perfect", "bhop_retention_1_late", "bhop_retention_3_late"};
        const int lates[3] = {0, 1, 3};
        for (int i = 0; i < 3; ++i) {
            onFloor(p, 400.0);
            std::vector<double> speeds;
            int wait = 0, t = 0;
            while (speeds.size() < 5 && t < 5000) {
                const bool jump = p.s.onground && wait >= lates[i];
                const bool wasAir = !p.s.onground;
                if (p.s.onground) ++wait;
                p.tick(0, 0, 0, jump);
                ++t;
                if (!p.s.onground) wait = 0;
                else if (wasAir) speeds.push_back(p.hspeed());
            }
            const std::vector<double> want = varr(std::string(names[i]) + ".landing_speeds_u_s");
            bool ok1 = speeds.size() == want.size() && !want.empty();
            for (size_t j = 0; ok1 && j < want.size(); ++j) ok1 = within(speeds[j], want[j]);
            ok = ok && ok1;
            char b[120];
            std::snprintf(b, sizeof(b), " late %d: %.2f..%.2f (want %.2f..%.2f)%s", lates[i], speeds.empty() ? 0.0 : speeds.front(),
                          speeds.empty() ? 0.0 : speeds.back(), want.empty() ? 0.0 : want.front(), want.empty() ? 0.0 : want.back(), ok1 ? "" : " X");
            got += b;
        }
        std::snprintf(d, sizeof(d), "%s", got.c_str());
        check(ok, "bhop retention per hop matches the model (0.94^late)", d);
    }
    // Gate 7: surf on the 45-degree ramp, 800 u/s along it, 67 ticks, no input / holding into it (sc_surf).
    {
        auto slopeZ = [&](double y) { return Sy0 * U + (y - Sz * U); };
        const double hw = U / 3.0;
        std::string got;
        bool ok = true;
        for (int hold = 0; hold < 2; ++hold) {
            const double y = Sz * U + 6.5 * U;
            p.init(Vec3d{(Sx0 + 1) * U, y, slopeZ(y + hw) + 0.1}, Vec3d{800, 0, 0});
            const double z0 = p.s.origin.z;
            int ground = 0;
            for (int i = 0; i < 67; ++i) { p.tick(0, hold ? -1 : 0, 0, false); ground += p.s.onground; }
            const char* sec = hold ? "surf_hold_into_ramp." : "surf_no_input.";
            const double vx = p.s.vel.x, dz = p.s.origin.z - z0;
            const double down = std::hypot(p.s.vel.y, p.s.vel.z) * (p.s.vel.z < 0 ? 1 : -1);
            const bool ok1 = within(vx, vnum(std::string(sec) + "vx_after_u_s")) && within(dz, vnum(std::string(sec) + "height_change_u")) &&
                             within(down, vnum(std::string(sec) + "downslope_speed_u_s")) && ground == (int)vnum(std::string(sec) + "ticks_on_ground");
            ok = ok && ok1;
            char b[220];
            std::snprintf(b, sizeof(b), "%s vx %.2f dz %.3f downslope %.2f ground %d (want %.2f %.3f %.2f %.0f)%s  ", hold ? "hold:" : "no input:",
                          vx, dz, down, ground, vnum(std::string(sec) + "vx_after_u_s"), vnum(std::string(sec) + "height_change_u"),
                          vnum(std::string(sec) + "downslope_speed_u_s"), vnum(std::string(sec) + "ticks_on_ground"), ok1 ? "" : "X");
            got += b;
        }
        std::snprintf(d, sizeof(d), "%s", got.c_str());
        check(ok, "surf: speed kept along the ramp, slide / climb match the model", d);
    }
    // Gate 8: walk up and down the 4-block ramp at maxspeed (sc_ramp_walk).
    const double Y0 = Rz * U - 100.0, Xr = (Rx0 + 6) * U;   // model ramp_walk_world origin in y; any x on the ramp
    {
        std::string got;
        bool ok = true;
        for (int up = 1; up >= 0; --up) {
            if (up) p.init(Vec3d{Xr, Y0, Zf + eps}, Vec3d{0, 0, 0});
            else    p.init(Vec3d{Xr, Y0 + 100.0 + 4 * U + 60.0, Zf + 4 * U + eps}, Vec3d{0, 0, 0});
            p.s.vel = Vec3d{0, ms * (up ? 1 : -1), 0};
            int ground = 0;
            double mn = 1e9, sum = 0;
            int n = 0;
            for (int i = 0; i < 150; ++i) {
                p.tick(1, 0, up ? 90.0 : -90.0, false);
                ground += p.s.onground;
                const double my = p.s.origin.y - Y0;
                if (100.0 + 20 < my && my < 100.0 + 4 * U - 20) { mn = std::min(mn, p.hspeed()); sum += p.hspeed(); ++n; }
            }
            const char* sec = up ? "ramp_walk_up." : "ramp_walk_down.";
            const double fh = p.s.origin.z - Zf, mean = n ? sum / n : 0;
            const bool ok1 = ground == (int)vnum(std::string(sec) + "ticks_on_ground") && within(fh, vnum(std::string(sec) + "final_height_u")) &&
                             n > 0 && within(mn, vnum(std::string(sec) + "min_hspeed_on_ramp_u_s")) && within(mean, vnum(std::string(sec) + "mean_hspeed_on_ramp_u_s"));
            ok = ok && ok1;
            char b[220];
            std::snprintf(b, sizeof(b), "%s ground %d/150 height %.3f min %.2f mean %.2f (want %.0f %.3f %.2f %.2f)%s  ", up ? "up:" : "down:",
                          ground, fh, mn, mean, vnum(std::string(sec) + "ticks_on_ground"), vnum(std::string(sec) + "final_height_u"),
                          vnum(std::string(sec) + "min_hspeed_on_ramp_u_s"), vnum(std::string(sec) + "mean_hspeed_on_ramp_u_s"), ok1 ? "" : "X");
            got += b;
        }
        std::snprintf(d, sizeof(d), "%s", got.c_str());
        check(ok, "walk up / down a 45-degree ramp at full speed, grounded throughout", d);
    }
    // Gate 9: landing on the ramp (U2): fast along it stays a surf plane; slow, or a clip to slow, is ground.
    {
        const char* names[3] = {"land_on_ramp_fast_along", "land_on_ramp_slow", "land_on_ramp_fast_uphill"};
        const Vec3d v0s[3] = {{600, 0, -100}, {100, 0, -100}, {0, 400, -100}};
        std::string got;
        bool ok = true;
        for (int i = 0; i < 3; ++i) {
            p.init(Vec3d{Xr, Y0 + 160.0, Zf + 120.0}, v0s[i]);
            int ground = 0, first = -1;
            for (int t = 0; t < 30; ++t) {
                p.tick(0, 0, 0, false);
                ground += p.s.onground;
                if (p.s.onground && first < 0) first = t + 1;
            }
            const double wf = vnum(std::string(names[i]) + ".first_ground_tick");
            const bool ok1 = ground == (int)vnum(std::string(names[i]) + ".ticks_on_ground") && (std::isnan(wf) ? first < 0 : first == (int)wf);
            ok = ok && ok1;
            char b[140];
            std::snprintf(b, sizeof(b), "%s: ground %d first %d (want %.0f %.0f)%s  ", names[i] + 13, ground, first,
                          vnum(std::string(names[i]) + ".ticks_on_ground"), std::isnan(wf) ? -1.0 : wf, ok1 ? "" : "X");
            got += b;
        }
        std::snprintf(d, sizeof(d), "%s", got.c_str());
        check(ok, "landing on a ramp: the surf rule (U2) decides ground like the model", d);
    }
    // Cost (plan risk "Air 2 cost"; informational): ticks of the surf scenario.
    {
        const int n = 5000;
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < n; ++i) {
            if (i % 67 == 0) { const double y = Sz * U + 6.5 * U; p.init(Vec3d{(Sx0 + 1) * U, y, Sy0 * U + (y + U / 3.0 - Sz * U) + 0.1}, Vec3d{800, 0, 0}); }
            p.tick(0, -1, 0, false);
        }
        const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / n;
        std::printf("[eden-srclike] tick cost: %.3f us per tick (surfing, holding into the ramp)\n", us);
    }

    // ---- The frame driver, through real Player frames. ----
    Player* pl = World::getWorld->player;
    const float floorTop = (float)(fy + 1);
    auto feet = [&]() { return pl->pos.y - pl->boxheight / 2; };
    auto standAt = [&](float x, float z) {   // on the slab, settled, at rest
        eden_console_teleport(x, floorTop + 1.0f, z);
        pl->vel = MakeVector(0, 0, 0);
        pl->yaw = 0; pl->pitch = 0;
        tick(40);
    };
    set_setting_by_key("srclike_movement", 1);
    // Gate 10: seeding from a stock rest (feet exactly on the floor = start-solid to the trace) lifts by
    // DIST_EPSILON and stands; the tick runs at 66.67 Hz under 60 Hz frames.
    {
        set_setting_by_key("srclike_movement", 0);
        standAt(c + 0.5f, cz + 0.5f);   // stock collider rests the player
        const float stockFeet = feet();
        set_setting_by_key("srclike_movement", 1);
        const unsigned t0 = tickCount();
        tick(60);
        const unsigned ran = tickCount() - t0;
        const double lift = (state().origin.z - Zf);
        std::snprintf(d, sizeof(d), "stock feet %.4f (floor %.0f), source feet %.4f, state lift %.5f u, onground %d, %u ticks in 60 frames",
                      stockFeet, floorTop, feet(), lift, (int)state().onground, ran);
        check(std::fabs(stockFeet - floorTop) < 0.01f && lift > 0 && lift < 1.0 && state().onground && (ran == 66 || ran == 67),
              "mode on from a stock rest: lifted off the start-solid contact, grounded, 66.67 ticks/s", d);
    }
    // Gate 11: W for 1 s reaches walk speed (5.85 b/s); per-frame displacement is smooth (interpolated)
    // at 60 and 144 Hz frames: each frame's step of the interpolated origin within 0.5 % of speed*etime
    // once at full speed (un-interpolated 66.67 Hz ticks under 60 Hz frames would be 0 or 2 ticks: 100 %
    // off). Measured on the double origin: Player::pos is a float, whose step at x ~ 65 500 is 0.004 block,
    // 4-10 % of a frame's travel (stock has the same quantisation), so it gets a looser bound.
    {
        std::string got;
        bool ok = true;
        const float rates[2] = {60.0f, 144.0f};
        for (float hz : rates) {
            standAt(c - 30.5f, cz + 0.5f);
            g_app->setFixedEtime(1.0f / hz);
            push_key(SDL_SCANCODE_W, true);
            for (int i = 0; i < (int)hz; ++i) tick(1);   // 1 s: full speed after 27 ticks
            double worst = 0, worstF = 0;
            double x0 = renderOrigin().x;
            float f0 = pl->pos.x;
            const double want = kWalkSpeed / hz;   // u per frame
            for (int i = 0; i < (int)(hz / 2); ++i) {
                tick(1);
                worst = std::max(worst, std::fabs((renderOrigin().x - x0) / want - 1.0));
                worstF = std::max(worstF, std::fabs((pl->pos.x - f0) * U - want) / U);   // blocks
                x0 = renderOrigin().x;
                f0 = pl->pos.x;
            }
            const float ulp = std::nextafter(pl->pos.x, 1e9f) - pl->pos.x;
            const double sp = std::hypot(pl->vel.x, pl->vel.z);
            push_key(SDL_SCANCODE_W, false);
            g_app->setFixedEtime(1.0f / 60.0f);
            const bool ok1 = within(sp * U, kWalkSpeed) && worst < 0.005 && worstF <= 1.01 * ulp;
            ok = ok && ok1;
            char b[200];
            std::snprintf(b, sizeof(b), "%.0f Hz: speed %.3f b/s (want %.3f), worst step off by %.3f%% (double), %.4f block (float pos, ulp %.4f)%s  ",
                          hz, sp, kWalkSpeed / U, worst * 100, worstF, ulp, ok1 ? "" : " X");
            got += b;
        }
        std::snprintf(d, sizeof(d), "%s", got.c_str());
        check(ok, "frame driver: walk speed reached; interpolated motion is smooth at 60 and 144 Hz", d);
    }
    // Gate 12: a teleport re-seeds (no snap back); an outside velocity (a portal exit, leaving noclip) is
    // adopted, and the next frames carry it.
    {
        standAt(c + 0.5f, cz + 0.5f);
        eden_console_teleport(c + 10.5f, floorTop + 1.5f, cz - 5.5f);
        tick(1);
        const float tx = pl->pos.x, tz = pl->pos.z;
        const bool seeded = std::fabs(tx - (c + 10.5f)) < 0.01f && std::fabs(tz - (cz - 5.5f)) < 0.01f &&
                            std::fabs(state().origin.x / U - (c + 10.5)) < 0.01;
        tick(60);
        pl->vel = MakeVector(8.0f, 0, 0);   // 8 b/s along +x, e.g. out of noclip (SRC.0)
        tick(1);
        const double vAdopt = state().vel.x / U;
        float x0 = pl->pos.x;
        tick(6);
        const float moved = pl->pos.x - x0;
        std::snprintf(d, sizeof(d), "after teleport pos %.3f,%.3f state x %.3f; adopted vel %.3f b/s (friction-decayed from 8), moved %.3f blocks in 6 frames",
                      tx, tz, state().origin.x / U, vAdopt, moved);
        check(seeded && vAdopt > 6.0 && vAdopt < 8.0 && moved > 0.4f, "teleport re-seeds the tick state; an outside velocity is adopted", d);
    }
    // Gate 13: a jump through real frames: apex 45 u above the take-off origin (the model's), lands grounded
    // within the 2 u ground probe. (A landing tick that ends inside the probe distance grounds the hull
    // where it is, up to 2 u above the floor, and standing still never snaps it down: the model, and
    // Source's walk move, only stay-on-ground while moving. So the take-off here is not always the floor.)
    {
        standAt(c + 0.5f, cz + 0.5f);
        const double z0 = state().origin.z;
        const bool restOk = state().onground && z0 - Zf > 0 && z0 - Zf <= 2.0 + eps;
        push_key(SDL_SCANCODE_SPACE, true);
        tick(1);
        push_key(SDL_SCANCODE_SPACE, false);
        double apex = 0;
        for (int i = 0; i < 60; ++i) { tick(1); apex = std::max(apex, renderOrigin().z - z0); }
        std::snprintf(d, sizeof(d), "take-off %.3f u above the floor (grounded %d), apex %.3f u above it (model %.3f), landed %d at %.3f u",
                      z0 - Zf, (int)restOk, apex, vnum("jump.model_apex_u"), (int)state().onground, state().origin.z - Zf);
        check(restOk && within(apex, vnum("jump.model_apex_u")) && state().onground && state().origin.z - Zf <= 2.0 + eps,
              "a real-frame jump peaks at the model apex and lands", d);
    }
    // Gate 14 (SRC.7): camera. Stand eye 64 u over the feet; ducked 28 u (hull eased); pitch holds +-89;
    // cam->py follows eyeY; the degrees-per-count row; a step-up lags the eye then settles.
    {
        standAt(c + 0.5f, cz + 0.5f);
        Camera* cam = World::getWorld->cam;
        tick(2);
        const double eyeStand = eyeY(pl) - feet();
        const bool camOk = std::fabs(cam->py - eyeY(pl)) < 0.001f;
        push_key(SDL_SCANCODE_LCTRL, true);
        tick(40);
        const double eyeDuck = eyeY(pl) - feet();
        push_key(SDL_SCANCODE_LCTRL, false);
        tick(40);
        const double eyeBack = eyeY(pl) - feet();
        std::snprintf(d, sizeof(d), "eye over feet: stand %.3f (want %.3f), ducked %.3f (want %.3f), back %.3f; cam->py==eyeY %d",
                      eyeStand, 64.0 / U, eyeDuck, 28.0 / U, eyeBack, (int)camOk);
        check(std::fabs(eyeStand - 64.0 / U) < 0.01 && std::fabs(eyeDuck - 28.0 / U) < 0.01 &&
              std::fabs(eyeBack - 64.0 / U) < 0.01 && camOk, "C2 eye height stands 64 u, ducks 28 u, camera uses it", d);

        pl->pitch = 0;
        for (int i = 0; i < 40; ++i) eden_apply_look_delta(0, -50);   // look far up
        tick(2);
        const float up = pl->pitch;
        for (int i = 0; i < 80; ++i) eden_apply_look_delta(0, 50);    // far down
        tick(2);
        const float down = pl->pitch;
        std::snprintf(d, sizeof(d), "pitch after looking up %.2f, down %.2f (limit %.0f)", up, down, kPitchLimit);
        check(std::fabs(up - kPitchLimit) < 0.01f && std::fabs(down + kPitchLimit) < 0.01f, "C5 pitch clamps at +-89 in the mode", d);

        eden_srclike_sens = 2.0f;
        pl->yaw = 0;
        eden_apply_look_delta(100, 0);
        const float yawDeg = pl->yaw;
        eden_srclike_sens = 0.0f;
        std::snprintf(d, sizeof(d), "100 counts at CS sens 2.0 turned %.3f deg (want %.3f)", yawDeg, 100 * 2.0 * 0.022);
        check(std::fabs(yawDeg - 4.4f) < 0.01f, "C6 degrees per count = sens x 0.022", d);

        // C4 (step smoothing) has no gate: no Eden block makes a step under 18 u, the only height change a
        // grounded tick can show; a ramp is a slope the eye already follows.
    }
    set_setting_by_key("srclike_movement", 0);
    fill(c - 64, c + 40, cz - 20, cz + 4, fy, fy + 4, TYPE_NONE);
    fill(Rx0, Rx1, Rz - 10, Rz + 14, fy, fy + 8, TYPE_NONE);
    fill(Sx0, Sx1, Sz - 2, Sz + K + 3, Sy0, Sy0 + K + 4, TYPE_NONE);
}

// SRC.6 leg `special`: water, water-jump, ladders, ice, trampolines (plan D6), each in a voxel room driven
// through pmTick (the Sim above). The swim rate is the SRC.1 vector (107.66 u/s, 3 blocks in 1.08 s).
namespace {
using namespace SrclikeMove;
struct SpecialRoom {
    int x0, x1, z0, z1, fy;
};
}
static void srclike_special_leg() {
    char d[400];
    const double U = kUnitsPerBlock;
    const int c = (int)kMapCentre, H = g_opt.height, fy = H - 12;
    const double Zf = (fy + 1) * U;
    auto fill = [](int x0, int x1, int z0, int z1, int y0, int y1, int type) {
        for (int x = x0; x <= x1; ++x)
            for (int z = z0; z <= z1; ++z)
                for (int y = y0; y <= y1; ++y) eden_console_setblock(x, z, y, type);
    };
    set_setting_by_key("srclike_movement", 0);
    const int X0 = c - 30, X1 = c + 30, Z0 = c - 60, Z1 = c - 40;
    eden_console_teleport(c + 0.5f, fy + 3.0f, c - 50.0f);
    tick(120);
    fill(X0, X1, Z0, Z1, fy, fy + 10, TYPE_NONE);
    fill(X0, X1, Z0, Z1, fy, fy, TYPE_STONE);
    Sim p;
    const double cxu = (c + 0.5) * U, czu = (Z0 + 10 + 0.5) * U;

    // W1: a 5-deep pool (water layers fy+1..fy+5), submerged on the floor, jump held: steady swim-up.
    fill(c - 4, c + 4, Z0 + 6, Z0 + 14, fy + 1, fy + 5, TYPE_WATER);
    p.init(Vec3d{cxu, czu, Zf + kDistEpsilon}, Vec3d{0, 0, 0});
    pmWater(p.s);
    const int lvl0 = p.s.waterlevel;
    double vzAt30 = 0;
    int t = 0;
    for (; t < 200 && p.s.waterlevel >= 2; ++t) {
        p.tick(0, 0, 0, true);
        if (t == 30) vzAt30 = p.s.vel.z;
    }
    std::snprintf(d, sizeof(d), "start level %d, steady vz %.2f u/s (want 107.66), left deep water after %d ticks", lvl0, vzAt30, t);
    check(lvl0 == 3 && within(vzAt30, 107.66), "W1 swim up in a pool at the SRC.1 rate", d);

    // W2: no input in water: sinks slowly (the -60 wish), no free fall.
    p.init(Vec3d{cxu, czu, Zf + 4 * U}, Vec3d{0, 0, 0});
    for (int i = 0; i < 40; ++i) p.tick(0, 0, 0, false);
    std::snprintf(d, sizeof(d), "vz %.2f (want ~ -48)", p.s.vel.z);
    check(p.s.vel.z < -30 && p.s.vel.z > -70, "W2 sinks at about the -60 wish x 0.8", d);

    // W3: water-jump. Pool edge: a 1-block ledge (stone fy+1) east of the pool at x = c+5..; pool surface = ledge top.
    fill(c + 5, c + 12, Z0 + 6, Z0 + 14, fy + 1, fy + 1, TYPE_STONE);
    fill(c - 4, c + 4, Z0 + 6, Z0 + 14, fy + 1, fy + 1, TYPE_WATER);   // pool 1 deep... make it 2: raise ledge
    fill(c + 5, c + 12, Z0 + 6, Z0 + 14, fy + 2, fy + 2, TYPE_STONE);
    // ledge top = fy+3 (2 blocks above the floor); water fills fy+1..fy+2 so the surface is at the ledge top - 1.
    fill(c - 4, c + 4, Z0 + 6, Z0 + 14, fy + 3, fy + 5, TYPE_NONE);
    fill(c - 4, c + 4, Z0 + 6, Z0 + 14, fy + 1, fy + 2, TYPE_WATER);
    fill(c + 5, c + 12, Z0 + 6, Z0 + 14, fy + 3, fy + 5, TYPE_NONE);
    // swimmer just west of the ledge face, waist-deep in water, facing +x
    p.init(Vec3d{(c + 4.5) * U, czu, Zf + 0.6 * U}, Vec3d{0, 0, 0});
    bool wj = false; int landed = -1;
    for (int i = 0; i < 200; ++i) {
        p.tick(1, 0, 0, true);
        if (p.s.waterJumpT > 0) wj = true;
        if (wj && p.s.onground && p.s.origin.x > (c + 5) * U) { landed = i; break; }
    }
    std::snprintf(d, sizeof(d), "waterjump fired %d, landed tick %d, x %.2f blocks, z %.1f u (ledge top %.1f)", (int)wj, landed,
                  p.s.origin.x / U, p.s.origin.z, (fy + 3) * U);
    check(wj && landed >= 0 && p.s.origin.z >= (fy + 3) * U - 0.5, "W3 water-jump lifts the swimmer onto the ledge", d);

    // L1: a ladder column on the east wall, W held looking level: climbs ~200 u/s; idle falls again.
    fill(c + 20, c + 20, Z0 + 10, Z0 + 10, fy + 1, fy + 6, TYPE_STONE);
    fill(c + 19, c + 19, Z0 + 10, Z0 + 10, fy + 1, fy + 6, TYPE_LADDER);
    p.init(Vec3d{(c + 18.4) * U, (Z0 + 10 + 0.5) * U, Zf + kDistEpsilon}, Vec3d{0, 0, 0});
    for (int i = 0; i < 40; ++i) p.tick(1, 0, 0, false);
    const double rose = p.s.origin.z - Zf;
    std::snprintf(d, sizeof(d), "onLadder %d, rose %.1f u in 40 ticks (want ~ 120)", (int)p.s.onLadder, rose);
    check(p.s.onLadder && rose > 90 && rose < 150, "L1 W on a ladder climbs at ~200 u/s", d);
    for (int i = 0; i < 20; ++i) p.tick(0, 0, 0, false);
    check(!p.s.onLadder && p.s.vel.z < 0, "L2 releasing the key lets go (stock needs input)", "");
    p.init(Vec3d{(c + 18.65) * U, (Z0 + 10 + 0.5) * U, Zf + 2 * U}, Vec3d{0, 0, 0});
    for (int i = 0; i < 10; ++i) p.tick(1, 0, 0, false);
    const double zj = p.s.origin.z;
    p.tick(1, 0, 0, true);
    for (int i = 0; i < 4; ++i) p.tick(0, 0, 0, false);
    std::snprintf(d, sizeof(d), "z %.1f -> %.1f, vz %.1f", zj, p.s.origin.z, p.s.vel.z);
    check(p.s.origin.z > zj && !p.s.onLadder, "L3 jump leaves the ladder with a kick", d);

    // I1: ice. Run to 227 u/s then release: ice stops far later than stone (stock .99 vs .90).
    fill(c - 20, c - 6, Z0 + 2, Z0 + 4, fy, fy, TYPE_STONE);
    fill(c - 20, c - 6, Z0 + 16, Z0 + 18, fy, fy, TYPE_ICE);
    auto coast = [&](int zrow) {
        p.init(Vec3d{(c - 19.5) * U, (zrow + 0.5) * U, Zf + kDistEpsilon}, Vec3d{kWalkSpeed, 0, 0});
        const double x0 = p.s.origin.x;
        for (int i = 0; i < 200; ++i) p.tick(0, 0, 0, false);
        return (p.s.origin.x - x0) / U;
    };
    const double stoneD = coast(Z0 + 3), iceD = coast(Z0 + 17);
    std::snprintf(d, sizeof(d), "stone %.2f blocks, ice %.2f blocks", stoneD, iceD);
    check(stoneD > 0 && iceD > 5 * stoneD, "I1 ice stops much later than stone", d);

    // T1: trampoline: dropped from 3 blocks, it rebounds at least as high as it fell; stone does not.
    fill(c + 24, c + 26, Z0 + 4, Z0 + 6, fy, fy, TYPE_TRAMPOLINE);
    p.init(Vec3d{(c + 25.5) * U, (Z0 + 5.5) * U, Zf + 3 * U}, Vec3d{0, 0, 0});
    double peak = 0; bool bounced = false;
    for (int i = 0; i < 200; ++i) {
        p.tick(0, 0, 0, false);
        bounced = bounced || p.s.bounced;
        if (bounced) peak = std::max(peak, p.s.origin.z - Zf);
    }
    std::snprintf(d, sizeof(d), "bounced %d, rebound peak %.1f u (drop 3 blocks = %.1f u)", (int)bounced, peak, 3 * U);
    check(bounced && peak > 0.9 * 3 * U, "T1 trampoline rebounds to the drop height", d);

    fill(X0, X1, Z0, Z1, fy, fy + 10, TYPE_NONE);
}

// SRC.8: the side effects move()/vertc() own, run from the Srclike path (Player::srclikeEffects).
static void srclike_effects_leg() {
    char d[300];
    Player* pl = World::getWorld->player;
    Hud* hud = World::getWorld->hud;
    auto fill = [](int x0, int x1, int z0, int z1, int y0, int y1, int type) {
        for (int x = x0; x <= x1; ++x)
            for (int z = z0; z <= z1; ++z)
                for (int y = y0; y <= y1; ++y) eden_console_setblock(x, z, y, type);
    };
    const int bx = (int)kMapCentre, bz = (int)kMapCentre, by = g_opt.height - 8;
    set_setting_by_key("srclike_movement", 1);
    eden_console_teleport(bx + 0.5f, by + 0.5f, bz + 0.5f);
    tick(120);
    fill(bx - 6, bx + 6, bz - 6, bz + 6, by - 1, by + 6, TYPE_NONE);
    fill(bx - 6, bx + 6, bz - 6, bz + 6, by - 2, by - 2, TYPE_STONE);
    auto stand = [&](float dx) {
        eden_console_teleport(bx + 0.5f + dx, by - 1 + 0.01f, bz + 0.5f);
        tick(3);
        pl->vel = MakeVector(0, 0, 0);
    };
    stand(0);
    // E1: a golden cube in the hull is picked up (counter + block removed), once.
    const int before = hud->goldencubes;
    eden_console_setblock(bx, bz, by - 1, TYPE_GOLDEN_CUBE);
    tick(4);
    std::snprintf(d, sizeof(d), "cubes %d -> %d, block now %d", before, hud->goldencubes, getLandc2(bx, bz, by - 1));
    check(hud->goldencubes == before + 1 && getLandc2(bx, bz, by - 1) == TYPE_NONE, "E1 golden cube pickup from the Srclike path", d);
    // E2: lava in the hull hurts.
    stand(2);
    pl->life = 1.0f; pl->dead = false;
    eden_console_setblock(bx + 2, bz, by - 1, TYPE_LAVA);
    tick(4);
    std::snprintf(d, sizeof(d), "life %.3f", pl->life);
    check(pl->life < 1.0f, "E2 lava burns", d);
    eden_console_setblock(bx + 2, bz, by - 1, TYPE_NONE);
    pl->life = 1.0f;
    // E3: landing clears `jumping`.
    stand(-2);
    pl->jumping = true;
    pl->pos.y += 2.0f;
    pl->vel = MakeVector(0, -3.0f, 0);
    tick(90);
    std::snprintf(d, sizeof(d), "onground=%d jumping=%d", (int)SrclikeMove::state().onground, (int)pl->jumping);
    check(SrclikeMove::state().onground && !pl->jumping, "E3 landing clears jumping", d);
    // E4: out of the world pops back up with vy 0.
    pl->pos.y = -20.0f;
    tick(3);
    std::snprintf(d, sizeof(d), "y %.2f", pl->pos.y);
    check(pl->pos.y > 0.0f, "E4 below the world pops back", d);
    set_setting_by_key("srclike_movement", 0);
    fill(bx - 6, bx + 6, bz - 6, bz + 6, by - 2, by + 6, TYPE_NONE);
}

}  // namespace g_input
}  // namespace harness
