// G.4: split out of the old eden_main_native.cpp (see harness.h). Mode group "input".
#include "harness.h"

using namespace harness;

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

}  // namespace g_input
}  // namespace harness
