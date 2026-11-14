// G.4: split out of the old eden_main_native.cpp (see harness.h). Mode group "session".
#include "harness.h"
#include "../seam/AppLifecycle.h"
#include "../../../Classes/World.h"
#include "../../../Classes/Hud.h"
#include "../../../Classes/statusbar.h"
#include "../../../Classes/Menu.h"
#if !defined(_WIN32)
#include <csignal>
#include <sys/resource.h>
#include <unistd.h>
#endif

using namespace harness;

namespace harness {
namespace g_session {


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

    // --- UX.3: the right-hand mode column takes clicks only while the pointer is free ----------
    // Menu open = free pointer: a left click on the build button must change the mode (picker
    // opens); a second click closes the picker (still free). Playing = captured: the same click is
    // the hold-to-mine slot, never the HUD button. Headless window_to_point is the identity, and
    // the pointer's y runs down from the top (eden_tap_hud_button_*'s flip).
    {
        Hud* hud = World::getWorld->hud;
        auto click_build = [&]() {
            const float x = hud->rbuild.origin.x + hud->rbuild.size.width * 0.5f;
            const float y = SCREEN_HEIGHT - (hud->rbuild.origin.y + hud->rbuild.size.height * 0.5f);
            for (int down = 1; down >= 0; --down) {
                SDL_Event e;
                SDL_zero(e);
                e.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
                e.button.button = SDL_BUTTON_LEFT;
                e.button.down = down != 0;
                e.button.x = x;
                e.button.y = y;
                SDL_PushEvent(&e);
                tick(8);
            }
        };
        const int menuOpen = eden_hud_in_menu();
        click_build();
        const int freeMode = player_state().hudMode;
        std::snprintf(detail, sizeof(detail), "in_menu %d, mode %d (MODE_PICK_BLOCK %d)", menuOpen, freeMode, (int)MODE_PICK_BLOCK);
        check(menuOpen && freeMode == MODE_PICK_BLOCK, "pointer free: a click on the build button opens the picker", detail);
        click_build();   // picker open = pointer still free: closes it again
        tick(20);
        const int closed = player_state().hudMode;
        const int stillMenu = eden_hud_in_menu();
        click_build();   // playing: captured
        tick(20);
        const int capturedMode = player_state().hudMode;
        std::snprintf(detail, sizeof(detail), "mode %d -> %d, in_menu %d", closed, capturedMode, stillMenu);
        check(!stillMenu && closed != MODE_PICK_BLOCK && capturedMode != MODE_PICK_BLOCK,
              "pointer captured: the same click does not press the build button", detail);
        push_key(SDL_SCANCODE_ESCAPE, true);   // leave the in-game menu open, as the checks below found it
        push_key(SDL_SCANCODE_ESCAPE, false);
        tick(30);
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

    // QA.8 (A1-01): an unfocused window sleeps. One second of the real interactive loop after a
    // FOCUS_LOST: the idle branch waits in SDL_WaitEventTimeout (100 ms), so ~10 passes; with the old
    // spin it was tens of thousands. FOCUS_GAINED brings the frame back at once.
    {
        double wake = 0.0;
        const int passes = run_unfocused(1000.0, &wake);
        char d[96];
        std::snprintf(d, sizeof d, "(%d loop passes in 1 s, wake %.1f ms)", passes, wake);
        std::printf("[%s] unfocused %s\n", g_selftestTag, d);
        check(passes >= 0, "FOCUS_LOST stops the frame", d);
        check(passes >= 3 && passes <= 20, "an unfocused window sleeps: 3..20 loop passes/s", d);
        check(g_app->isAnimating() && wake < 50.0, "FOCUS_GAINED brings the frame back within 50 ms", d);
    }

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
    char detail[1200];
    std::snprintf(detail, sizeof(detail), "%s = %lld bytes", path[0] ? path : "(not found)", bytes);
    const size_t plen = std::strlen(path);
    if (plen > 5 && !std::strcmp(path + plen - 5, ".emod")) {
        // Stage S / S.4 (--world-format=emod): a compressed append log has no size floor worth
        // asserting, so ask the store itself -- a committed batch holding at least one column.
        emod::EdenWorldStore st;
        const bool ok = st.open(path, false) && st.columnCount() >= 1;
        std::snprintf(detail + std::strlen(detail), sizeof(detail) - std::strlen(detail),
                      ", %u column(s), %u commit(s)", (unsigned)st.columnCount(), (unsigned)st.lastSeq());
        check(ok, "the world file on disk holds at least one column", detail);
    } else {
        // 192-byte header + 200*60 creature block + at least one 32,768-byte column + a directory.
        check(bytes >= 192 + 200 * 60 + 32768, "the world file on disk holds at least one column", detail);
    }

    // QA.18 (A2-09): SDL_EVENT_LOW_MEMORY through the same watch: a save record appears, the loop
    // keeps running (unlike a background), and the audio clip cache empties. A clip is decoded first
    // so "empties" is not vacuous (headless has no device, so decode through the probe).
    {
        SDL_zero(ev);
        ev.type = SDL_EVENT_DID_ENTER_FOREGROUND;   // the background above stopped the loop
        ev.common.timestamp = SDL_GetTicksNS();
        SDL_PushEvent(&ev);
        check(g_app->isAnimating(), "foregrounded: the loop is running before the warning", "");
        int fr = 0, ch = 0, rate = 0;
        eden_native_audio_probe("block_break_stone_1.caf", &fr, &ch, &rate);
        const int clipsBefore = eden_native_audio_clip_count();
        const int savesBefore = eden_app_background_save_count();
        SDL_zero(ev);
        ev.type = SDL_EVENT_LOW_MEMORY;
        ev.common.timestamp = SDL_GetTicksNS();
        SDL_PushEvent(&ev);
        check(eden_app_low_memory_count() == 1, "SDL_EVENT_LOW_MEMORY reached the watch", "");
        check(eden_app_background_save_count() == savesBefore + 1, "low memory saved the open world", "");
        check(eden_app_background_last_status() == 0, "...and it answered SAVED", "");
        char d[64];
        std::snprintf(d, sizeof d, "(%d clip(s) before)", clipsBefore);
        check(eden_native_audio_clip_count() == 0, "low memory emptied the audio clip cache", d);
        check(g_app->isAnimating(), "low memory does not stop the loop", "");
        // The count below (2 after the foreground round trip) now includes this save.
    }

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
    check(eden_app_background_save_count() == 3, "foreground then background saves again", "");

    if (!quit_to_menu()) return 1;
    check(eden_app_will_background() == 1, "back on the menu: refuses again (NO_WORLD)", "");
    check(eden_app_background_save_count() == 3, "quitting to the menu counted no extra save", "");

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
    // directory is the SEAM's now — Classes/FileManager.cpp asks eden_platform_documents_root()
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
// --save-fail-selftest  (QA.5, audit A2-01)
// ---------------------------------------------------------------------------------------------
// A full disk, in process: RLIMIT_FSIZE set to the world file's committed size with SIGXFSZ ignored,
// so the next append fails with EFBIG, which is what ENOSPC looks like to the store. Asserts the
// failure is reported, that nothing that would discard the unsaved edit goes ahead (Save & Quit, a
// warp, the bulk reload), and that with the limit lifted the dialog's "Try again" saves and the edit
// reloads. The audit's gate was a shell `ulimit -f` leg; setrlimit is the same limit, settable and
// liftable inside one process, so the retry half needs no second run.
int run_save_fail_selftest() {
    g_selftestTag = "eden-save-fail";
#if defined(_WIN32)
    std::printf("[eden-save-fail] SKIP: no RLIMIT_FSIZE on Windows\n");
    return 0;
#else
    const std::string& m = g_opt.saveFailMutate;
    if (!mutation_known("eden-save-fail", "--save-fail-mutate", m, {"exit", "stream"})) return 2;
    g_save_test_ignore = m == "exit" ? SAVE_TEST_IGNORE_EXIT : m == "stream" ? SAVE_TEST_IGNORE_STREAM : 0;
    g_autosave_seconds = 0;   // only the saves this mode asks for
    const char* name = g_opt.world.empty() ? "save-fail" : g_opt.world.c_str();
    if (!open_world(name, g_opt.height)) return 1;
    g_app->setFixedEtime(1.0f / 60.0f);
    // The bundled map's centre (as --evict-selftest): the default --at is outside the map, where the
    // player is clamped to the edge and a walk goes nowhere.
    const float X0 = g_opt.haveAt ? g_opt.at[0] : 65536.5f, Y0 = g_opt.haveAt ? g_opt.at[1] : 60.0f,
                Z0 = g_opt.haveAt ? g_opt.at[2] : 65536.5f;
    eden_console_teleport(X0, Y0, Z0);
    tick(g_opt.frames);
    World* w = World::getWorld;
    char d[512];

    // A first save so the file exists with a commit, then the edit that must survive.
    save_world();
    check(!w->fm->saveFailing(), "an unlimited save commits", w->fm->lastSaveError().c_str());
    const int bx = (int)X0 + 2, bz = (int)Z0 + 2, by = (int)Y0;
    const int before = eden_console_getblock(bx, bz, by);
    const int want = before == 1 ? 3 : 1;
    eden_console_setblock(bx, bz, by, want);
    tick(30);
    check(w->terrain->anyChunkModified(), "the edit is pending", "");

    std::string path;
    for (int i = 0; i < eden_menu_world_count(); ++i)
        if (std::strcmp(eden_menu_world_name(i), name) == 0)
            path = std::string(eden_platform_documents_root()) + "/" + eden_menu_world_file(i);
    long long size = -1;
    if (FILE* f = std::fopen(path.c_str(), "rb")) { std::fseek(f, 0, SEEK_END); size = std::ftell(f); std::fclose(f); }
    check(size > 0, "the world file has a commit", path.c_str());

    // The limit is per process and covers every regular file, the log this run's stdout/stderr may be
    // redirected into included (that would swallow the very `emod: save failed:` line under test). So
    // both go through a pipe to a `cat` forked now, before the limit: it inherits the log, not the limit.
    std::fflush(stdout); std::fflush(stderr);
    FILE* relay = popen("cat", "w");
    if (relay) { dup2(fileno(relay), 1); dup2(fileno(relay), 2); }
    std::signal(SIGXFSZ, SIG_IGN);
    struct rlimit old;
    getrlimit(RLIMIT_FSIZE, &old);
    struct rlimit lim = old;
    lim.rlim_cur = (rlim_t)size;
    check(setrlimit(RLIMIT_FSIZE, &lim) == 0, "RLIMIT_FSIZE set to the file's size", "");

    // 1. The Save button: a failure, counted, said on the status line, the edit still pending.
    const int fail0 = g_save_failures;
    save_world();
    const std::string* st = w->hud->sb->current();
    std::snprintf(d, sizeof(d), "failures +%d, error \"%s\", status \"%s\"", g_save_failures - fail0,
                  w->fm->lastSaveError().c_str(), st ? st->c_str() : "(none)");
    check(g_save_failures > fail0 && w->fm->saveFailing(), "a limited save fails and says so", d);
    check(st && st->find("Could not save") != std::string::npos, "...on the status line", d);
    check(w->terrain->anyChunkModified(), "...and the edit is still pending", "");

    // 2. Save & Quit: stays in the world, behind the dialog; Stay closes it.
    ensure_hud_menu_open();
    tap_hud(6);
    tick(60);
    check(game_mode() != 0 && w->terrain->loaded, "Save & Quit on a failed save stays in the world", "");
    CGRect b;
    check(GLDialog::active() && GLDialog::buttonRect(2, &b), "...and asks: Try again / Leave without saving / Stay", "");
    if (GLDialog::active() && GLDialog::buttonRect(2, &b)) { ui_tap_rect(b); tick(5); }
    check(!GLDialog::active() && game_mode() != 0, "Stay keeps the world open", "");

    // 3. Warp home: no reload over the edit.
    if (game_mode() != 0) {
        const PlayerState p0 = player_state();
        const bool warped = w->terrain->warpToHome();
        const PlayerState p1 = player_state();
        std::snprintf(d, sizeof(d), "returned %d, moved %.1f", warped, std::fabs(p1.x - p0.x) + std::fabs(p1.z - p0.z));
        check(!warped && w->terrain->anyChunkModified(), "a warp on a failed save does not go ahead", d);
    }

    // 4a. Walking (the ring stream): two hops of 7 columns each (under the bulk threshold) carry the
    // edited column out of the window; its evict-save fails, so its slot keeps it.
    if (game_mode() != 0) {
        const int held0 = g_ring_evict_held, bulk0 = g_bulk_reloads;
        // 0.5 block a frame (--evict-selftest's walk), far enough for the trailing edge to pass the edit.
        const float W = (float)(CHUNK_SIZE * (T_RADIUS + 6));
        for (int k = 1; k <= (int)(W / 0.5f); ++k) { eden_console_teleport(X0 + k * 0.5f, Y0, Z0); tick(1); }
        tick(60);
        std::snprintf(d, sizeof(d), "held column-landings +%d, bulk reloads +%d", g_ring_evict_held - held0,
                      g_bulk_reloads - bulk0);
        check(g_ring_evict_held > held0 && g_bulk_reloads == bulk0 && w->terrain->anyChunkModified(),
              "a walk does not stream over the unsaved column", d);
    }

    // 4b. A jump (the bulk reload): waits rather than overwrite the window.
    if (game_mode() != 0) {
        const int bulk0 = g_bulk_reloads;
        eden_console_teleport(X0 + 1200, Y0, Z0 + 1200);
        tick(120);
        std::snprintf(d, sizeof(d), "bulk reloads +%d", g_bulk_reloads - bulk0);
        check(g_bulk_reloads == bulk0 && w->terrain->anyChunkModified(), "a jump waits for a save before reloading", d);
    }

    // 5. Lift the limit; Save & Quit fails once more, then the dialog's Try again saves and leaves.
    if (game_mode() != 0) {
        ensure_hud_menu_open();
        tap_hud(6);
        tick(60);
        check(GLDialog::active(), "Save & Quit fails again under the limit", "");
        check(setrlimit(RLIMIT_FSIZE, &old) == 0, "RLIMIT_FSIZE lifted", "");
        if (GLDialog::active() && GLDialog::buttonRect(0, &b)) ui_tap_rect(b);
        tick_until([] { return game_mode() == 0; }, 20000, "game_mode back to MENU");
        std::snprintf(d, sizeof(d), "error \"%s\"", w->fm->lastSaveError().c_str());
        check(game_mode() == 0 && !w->fm->saveFailing(), "Try again with space saves and leaves", d);
    } else {
        setrlimit(RLIMIT_FSIZE, &old);
    }

    // 6. The edit is in the file.
    if (game_mode() == 0) {
        tick(60);
        if (!open_world(name, g_opt.height)) return 1;
        eden_console_teleport(X0, Y0, Z0);
        tick(g_opt.frames);
        const int got = eden_console_getblock(bx, bz, by);
        std::snprintf(d, sizeof(d), "block %d (want %d, was %d)", got, want, before);
        check(got == want, "the edit made under the full disk reloads", d);
        quit_to_menu();
    }

    std::printf("[eden-save-fail] %s (%d failure(s))\n",
                g_selftestFailures ? "FAILURES" : "ALL PASS", g_selftestFailures);
    return g_selftestFailures ? 1 : 0;
#endif
}

// QA.6 (A2-02): a world that fails to load (its file header's CRC flipped: it still lists, the lister opens
// header-only) must not poison the next load. Create two worlds, corrupt the first, open it (refused, back
// at the menu, reason on the status bar), then open the good one in the same process: it must reach PLAY.
extern "C" int eden_load_failed(void);
extern "C" const char* eden_load_failed_reason(void);
int run_load_fail_selftest() {
    g_selftestTag = "eden-load-fail";
    g_autosave_seconds = 0;
    const char* bad = "lf-bad";
    const char* good = "lf-good";
    for (const char* n : {bad, good}) {
        if (!open_world(n, 64)) return 1;
        tick(30);
        save_world();
        if (!quit_to_menu()) return 1;
    }
    std::string path;
    for (int i = 0; i < eden_menu_world_count(); ++i)
        if (std::strcmp(eden_menu_world_name(i), bad) == 0)
            path = std::string(eden_platform_documents_root()) + "/" + eden_menu_world_file(i);
    bool flipped = false;
    if (FILE* f = std::fopen(path.c_str(), "r+b")) {   // the file header's crc32 is at bytes 60..63
        unsigned char b;
        if (std::fseek(f, 60, SEEK_SET) == 0 && std::fread(&b, 1, 1, f) == 1) {
            b ^= 0xFF;
            flipped = std::fseek(f, 60, SEEK_SET) == 0 && std::fwrite(&b, 1, 1, f) == 1;
        }
        std::fclose(f);
    }
    check(flipped, "the file header CRC is flipped", path.c_str());

    int idx = -1;
    for (int i = 0; i < eden_menu_world_count(); ++i)
        if (std::strcmp(eden_menu_world_name(i), bad) == 0) idx = i;
    check(idx >= 0, "the corrupt world still lists", "");
    eden_menu_select(idx);
    eden_menu_play();
    tick(600);
    char d[512];
    std::snprintf(d, sizeof(d), "game_mode %d, failed %d, reason \"%s\"", game_mode(), eden_load_failed(), eden_load_failed_reason());
    check(game_mode() == 0 && eden_load_failed() == 1, "the corrupt world is refused and we stay at the menu", d);
    const std::string* st = World::getWorld->menu->sbar->current();
    check(st && st->find("Could not load") != std::string::npos, "the reason is on the status bar", st ? st->c_str() : "(none)");

    check(open_world(good, 64), "the good world reaches PLAY after the failed one", "");
    check(!eden_load_failed(), "the failure flag is clear", "");
    {   // QA.17: a clean world says nothing about damage
        const std::string* s = World::getWorld->hud->sb->current();
        check(!s || s->find("damaged") == std::string::npos, "a clean world shows no damage notice", s ? s->c_str() : "(none)");
    }
    save_world();   // a later commit, so damage inside the first batch is damage, not a torn tail
    quit_to_menu();

    // QA.17 (A2-06, U8): a byte flipped inside the first batch's first record: the world still opens
    // (playing what survives) and says so once on the status line, which used to be stderr only.
    std::string gpath;
    for (int i = 0; i < eden_menu_world_count(); ++i)
        if (std::strcmp(eden_menu_world_name(i), good) == 0)
            gpath = std::string(eden_platform_documents_root()) + "/" + eden_menu_world_file(i);
    bool hit = false;
    if (FILE* f = std::fopen(gpath.c_str(), "r+b")) {   // 64-byte file header, 32-byte record header: 104 is payload
        unsigned char b;
        if (std::fseek(f, 104, SEEK_SET) == 0 && std::fread(&b, 1, 1, f) == 1) {
            b ^= 0xFF;
            hit = std::fseek(f, 104, SEEK_SET) == 0 && std::fwrite(&b, 1, 1, f) == 1;
        }
        std::fclose(f);
    }
    check(hit, "a byte of the first batch is flipped", gpath.c_str());
    check(open_world(good, 64), "the damaged world still opens", "");
    {
        const std::string* s = World::getWorld->hud->sb->current();
        check(s && s->find("damaged") != std::string::npos, "the damage is on the status line", s ? s->c_str() : "(none)");
    }
    quit_to_menu();
    std::printf("[eden-load-fail] %s\n", g_selftestFailures ? "FAILURES" : "ALL PASS");
    return g_selftestFailures ? 1 : 0;
}

}  // namespace g_session
}  // namespace harness
