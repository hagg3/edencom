// G.4: split out of the old eden_main_native.cpp (see harness.h). Mode group "look".
#include "harness.h"

using namespace harness;

namespace harness {
namespace g_look {


// --look-selftest: WORKING/touch-camera-plan-2026-10-17.md's gate for Stage L, steps 0-9 (the
// plan's numbering). Synthetic drags through eden_input_pointer_event (the real seam entry point),
// point space, top-left origin, at a fixed 1/60 s etime; reads player->yaw/pitch frame by frame.
// Headless has no window box, so TOUCH_PT_SCALE is 1 (one window point per engine point) until step
// 3 gives it one. --look-mutate=<name> puts one fixed bug back (TouchLook.h's TL_MUT_*); native.yml
// asserts every mutation FAILS this.
int run_look_selftest() {
    g_selftestTag = "eden-look";
    eden_set_detected_touch(1);
    eden_test_force_touch_input();
    eden_look_trace_enable(1);
    if (!g_opt.lookMutate.empty()) {
        TOUCH_LOOK_MUTATE = touchlook_mutation_bit(g_opt.lookMutate.c_str());
        if (!TOUCH_LOOK_MUTATE) {
            std::fprintf(stderr, "[eden-look] unknown --look-mutate=%s\n", g_opt.lookMutate.c_str());
            return 2;
        }
        std::printf("[eden-look] mutation: %s\n", g_opt.lookMutate.c_str());
    }
    const char* name = g_opt.world.empty() ? "look-selftest" : g_opt.world.c_str();
    if (!open_world(name, g_opt.height)) return 1;
    g_app->setFixedEtime(1.0f / 60.0f);
    tick(60);
    Player* pl = World::getWorld->player;
    Hud* hud = World::getWorld->hud;
    Terrain* ter = World::getWorld->terrain;
    pl->invertcam = 0;
    TOUCH_LOOK_SMOOTH = 0.0f;   // raw exactly; step 4 turns it on
    TOUCH_LOOK_GLIDE = 0.0f;
    hud->mode = MODE_BUILD;     // so a tap that is not cleared builds (steps 5-6)
    hud->blocktype = TYPE_STONE;
    int c[6];
    float lost = 0;
    char d[320];
    int ident = 40;
    auto turned = [](float a, float b) {
        float t = b - a;
        while (t > 180.0f) t -= 360.0f;
        while (t <= -180.0f) t += 360.0f;
        return t;
    };
    auto level = [&](float pitch) { pl->yaw = 0.0f; pl->pitch = pitch; tick(2); };
    auto centre = [](float* x, float* y) {
        *x = (float)(int)(SCREEN_WIDTH * 0.5f);
        *y = (float)(int)(SCREEN_HEIGHT * 0.5f);
    };
    int bx = 0, bz = 0, by = 0;   // the block-count box's centre, fixed per step (the player drifts)
    auto pin_box = [&]() { bx = (int)pl->pos.x; bz = (int)pl->pos.z; by = (int)pl->pos.y; };
    auto near_blocks = [&]() {   // non-air voxels in the pinned box (Terrain takes x, z, y)
        int n = 0;
        const int px = bx, pz = bz, py = by;
        for (int x = px - 7; x <= px + 7; ++x)
            for (int z = pz - 7; z <= pz + 7; ++z)
                for (int y = py - 7; y <= py + 7; ++y)
                    if (y >= 0 && y < T_HEIGHT && ter->getLand(x, z, y) > 0) ++n;
        return n;
    };
    // Where a tap released at pointer (x, y) would build: Player's own call, on Input.mm's int point.
    auto tap_target = [](float x, float y) {
        const int mx = (int)x, my = (int)((float)Input::getInput()->scr_height - y);
        return findWorldCoords(my, mx, FC_PLACE);   // sic: Player passes (my, mx)
    };
    // One horizontal drag: `n` moves of `step` engine points, one a frame, then the release.
    auto drag = [&](float x, float y, int n, float step) {
        const int id = ++ident;
        eden_input_pointer_event(0, id, x, y);
        tick(1);
        for (int f = 0; f < n; ++f) { x += step; eden_input_pointer_event(1, id, x, y); tick(1); }
        eden_input_pointer_event(2, id, x, y);
        tick(2);
    };
    const float slop = touchlook_slop_engine_pts();   // 12 headless
    const float deg = TOUCH_LOOK_DEG_PER_PT;
    float x0, y0;
    centre(&x0, &y0);

    // ---- Steps 0-1 (D1): three 1-point moves a frame for 30 frames. The 12-point slop holds frames
    // 1-4 (3k <= 12); frame 5 crosses and turns only the travel past the edge. Every later frame
    // turns all three of its points, so after frame k the camera has turned (3k - 12) points' worth.
    {
        level(0.0f);
        const int id = ++ident;
        const float yaw0 = pl->yaw;
        eden_input_pointer_event(0, id, x0, y0);
        tick(1);
        float x = x0, worst = 0.0f;
        int worstFrame = -1;
        for (int f = 1; f <= 30; ++f) {
            for (int k = 0; k < 3; ++k) { x += 1.0f; eden_input_pointer_event(1, id, x, y0); }
            tick(1);
            const float want = std::fmax(0.0f, (x - x0) - slop) * deg;
            const float e = std::fabs(turned(yaw0, pl->yaw) - want);
            if (e > worst) { worst = e; worstFrame = f; }
        }
        eden_input_pointer_event(2, id, x, y0);
        tick(2);
        eden_look_trace_last(c, &lost);
        std::snprintf(d, sizeof(d), "events %d frames %d multi %d look_frames %d lost %.2f%%", c[0], c[1], c[2],
                      c[3], lost);
        check(c[0] == 91 && c[1] == 31 && c[2] == 30 && c[3] == 27,
              "step 0: the trace counts 91 events, 31 frames, 30 multi-event, 27 look frames", d);
        check(std::fabs(lost) < 0.5f, "step 0: the trace sees nothing lost", d);
        const float total = turned(yaw0, pl->yaw), want = (90.0f - slop) * deg;
        std::snprintf(d, sizeof(d), "turned %.3f deg (want %.3f), worst frame %d off by %.4f", total, want,
                      worstFrame, worst);
        check(std::fabs(total - want) < 0.01f && worst < 0.01f,
              "step 1: a 3-events-per-frame drag turns its whole travel, every frame (smoothing 0 = raw)", d);
    }

    // ---- Step 2 (D2): 0.3 points a frame after a 20-point jump past the slop. Int points stepped
    // on ~30 of 100 frames; float accumulation turns every frame by the same amount.
    {
        level(0.0f);
        const int id = ++ident;
        const float yaw0 = pl->yaw;
        eden_input_pointer_event(0, id, x0, y0);
        tick(1);
        float x = x0 + 20.0f;
        eden_input_pointer_event(1, id, x, y0);
        tick(1);
        float prev = pl->yaw, minStep = 1e9f, maxStep = 0.0f;
        for (int f = 0; f < 100; ++f) {
            x += 0.3f;
            eden_input_pointer_event(1, id, x, y0);
            tick(1);
            const float st = turned(prev, pl->yaw);
            minStep = std::fmin(minStep, st);
            maxStep = std::fmax(maxStep, st);
            prev = pl->yaw;
        }
        eden_input_pointer_event(2, id, x, y0);
        tick(2);
        eden_look_trace_last(c, &lost);
        const float total = turned(yaw0, pl->yaw), want = ((x - x0) - slop) * deg;
        std::snprintf(d, sizeof(d), "per-frame step %.4f..%.4f deg (want %.4f), total %.3f (want %.3f), trace zero %d/%d",
                      minStep, maxStep, 0.3f * deg, total, want, c[4], c[5]);
        check(minStep > 0.3f * deg * 0.95f && maxStep < 0.3f * deg * 1.05f && c[4] == 0,
              "step 2: a 0.3-point/frame drag turns every frame by the same amount (no staircase)", d);
        check(std::fabs(total - want) < 0.01f, "step 2: and loses nothing", d);
    }

    // ---- Step 4: smoothing on, one event stream (120 events over 0.5 s, 100 points) ticked at 30, 60
    // and 120 Hz. Held still for 1 s the filter settles on the travel; released straight from the
    // motion, the flush leaves no remainder. Either way every rate ends on the same yaw.
    {
        TOUCH_LOOK_SMOOTH = 0.5f;
        const int rates[3] = {30, 60, 120};
        const float want = (100.0f - slop) * deg;
        float lag60 = 0.0f;
        for (int hold = 1; hold >= 0; --hold) {
            for (int r : rates) {
                g_app->setFixedEtime(1.0f / (float)r);
                level(0.0f);
                const int id = ++ident;
                const float yaw0 = pl->yaw;
                eden_input_pointer_event(0, id, x0, y0);
                tick(1);
                int k = 0;
                const int frames = r / 2;
                for (int n = 1; n <= frames; ++n) {
                    while (k < 120 && (k + 1) * r <= n * 240) {
                        ++k;
                        eden_input_pointer_event(1, id, x0 + 100.0f * (float)k / 120.0f, y0);
                    }
                    tick(1);
                }
                const float atEnd = turned(yaw0, pl->yaw);
                if (hold && r == 60) lag60 = want - atEnd;
                float held = atEnd;
                if (hold) { tick(r); held = turned(yaw0, pl->yaw); }
                eden_input_pointer_event(2, id, x0 + 100.0f, y0);
                tick(2);
                const float fin = turned(yaw0, pl->yaw);
                std::snprintf(d, sizeof(d), "%d Hz: %.3f deg at the motion's end, %.3f held, %.3f released (want %.3f)",
                              r, atEnd, held, fin, want);
                if (hold)
                    check(std::fabs(held - want) < want * 0.005f, "step 4: held still, the filter settles on the travel", d);
                check(std::fabs(fin - want) < 0.01f,
                      hold ? "step 4: released after the hold, nothing is left over"
                           : "step 4: released mid-motion, the flush leaves no remainder", d);
            }
        }
        std::snprintf(d, sizeof(d), "lag at the motion's end, 60 Hz: %.3f deg", lag60);
        check(lag60 > 0.05f, "step 4: smoothing 0.5 actually filters (the camera trails the finger)", d);
        g_app->setFixedEtime(1.0f / 60.0f);
        TOUCH_LOOK_SMOOTH = 0.0f;
    }

    // ---- Steps 5-6 (D4): one slop for tap and look. Looking down at the ground in Build mode (25 deg:
    // far enough that the target never overlaps the player, which builds nothing, and above -50,
    // where a tap would jump-and-build instead), a
    // 6-point wiggle builds one block and turns nothing; a 40-point drag turns (40 - slop) points'
    // worth and builds nothing.
    {
        level(-25.0f);
        tick(10);
        pin_box();
        const int b0 = near_blocks();
        const int id = ++ident;
        static const float wig[6][2] = {{3, 0}, {6, 2}, {2, -4}, {-3, -6}, {-6, 1}, {0, 0}};
        eden_input_pointer_event(0, id, x0, y0);
        tick(1);
        float wigTurn = 0.0f;
        for (auto& w : wig) {
            eden_input_pointer_event(1, id, x0 + w[0] / TOUCH_PT_SCALE, y0 + w[1] / TOUCH_PT_SCALE);
            tick(1);
            wigTurn = std::fmax(wigTurn, std::fabs(turned(0.0f, pl->yaw)) + std::fabs(pl->pitch + 25.0f));
        }
        const Point3D T = tap_target(x0, y0);
        eden_input_pointer_event(2, id, x0, y0);
        tick(6);
        const int b1 = near_blocks();
        std::snprintf(d, sizeof(d), "blocks %d -> %d, target %d,%d,%d now %d, largest turn %.3f deg, pitch %.3f",
                      b0, b1, T.x, T.y, T.z, ter->getLand(T.x, T.z, T.y), wigTurn, pl->pitch);
        check(b1 == b0 + 1 && T.x != -1 && ter->getLand(T.x, T.z, T.y) == TYPE_STONE && wigTurn < 1e-4f &&
              std::fabs(pl->pitch + 25.0f) < 1e-4f,
              "step 5: a 6-point wiggle is a tap: one block where it aimed, no turn at any point", d);

        level(-25.0f);
        pin_box();
        const int b2 = near_blocks();
        {   // drag() by hand, to read where the release would build
            const int id2 = ++ident;
            float x = x0;
            eden_input_pointer_event(0, id2, x, y0);
            tick(1);
            for (int f = 0; f < 20; ++f) { x += 2.0f; eden_input_pointer_event(1, id2, x, y0); tick(1); }
            const Point3D U = tap_target(x, y0);
            eden_input_pointer_event(2, id2, x, y0);
            tick(6);
            const int b3 = near_blocks();
            const float want = (40.0f - slop) * deg;
            std::snprintf(d, sizeof(d), "blocks %d -> %d, release target %d,%d,%d now %d, turned %.3f deg (want %.3f)", b2,
                          b3, U.x, U.y, U.z, U.x != -1 ? ter->getLand(U.x, U.z, U.y) : -1, turned(0.0f, pl->yaw), want);
            check(b3 == b2 && U.x != -1 && ter->getLand(U.x, U.z, U.y) == TYPE_NONE &&
                  std::fabs(turned(0.0f, pl->yaw) - want) < 0.01f,
                  "step 6: a 40-point drag turns past the slop and builds nothing", d);
        }
    }

    // ---- Step 7: glide. Off: the camera stops on release. On: it keeps turning, decays, and stops
    // within 1 s; a touch-down catches it.
    {
        for (int glide = 0; glide < 2; ++glide) {
            TOUCH_LOOK_GLIDE = glide ? 0.2f : 0.0f;   // half-life, s
            level(0.0f);
            drag(x0 - 60.0f, y0, 30, 4.0f);   // ~240 pt/s at release
            const float yR = pl->yaw;
            tick(6);
            const float y6 = pl->yaw;
            tick(54);
            const float y60 = pl->yaw;
            tick(30);
            const float y90 = pl->yaw;
            std::snprintf(d, sizeof(d), "glide %d: +%.3f deg after 0.1 s, +%.3f after 1 s, +%.3f after 1.5 s", glide,
                          turned(yR, y6), turned(yR, y60), turned(yR, y90));
            if (!glide)
                check(std::fabs(turned(yR, y90)) < 1e-4f, "step 7: glide off, the camera stops on release", d);
            else
                check(turned(yR, y6) > 0.5f && std::fabs(turned(y60, y90)) < 1e-4f,
                      "step 7: glide on, it keeps turning and stops within 1 s", d);
        }
        level(0.0f);
        drag(x0 - 60.0f, y0, 30, 4.0f);
        tick(3);
        const int id = ++ident;
        eden_input_pointer_event(0, id, x0, y0);   // a still finger
        tick(1);
        const float yC = pl->yaw;
        tick(10);
        std::snprintf(d, sizeof(d), "%.4f deg in the 10 frames after the touch-down", turned(yC, pl->yaw));
        check(std::fabs(turned(yC, pl->yaw)) < 1e-4f, "step 7: a touch-down catches a glide", d);
        for (int f = 1; f <= 10; ++f) { eden_input_pointer_event(1, id, x0 + 3.0f * f, y0); tick(1); }   // not a tap
        eden_input_pointer_event(2, id, x0 + 30.0f, y0);
        tick(2);
        TOUCH_LOOK_GLIDE = 0.0f;
        // Slider range: a longer half-life coasts further than a shorter one.
        float far[2];
        const float hl[2] = {0.1f, 0.6f};
        for (int k = 0; k < 2; ++k) {
            TOUCH_LOOK_GLIDE = hl[k];
            level(0.0f);
            drag(x0 - 60.0f, y0, 30, 4.0f);
            const float yR = pl->yaw;
            tick(120);
            far[k] = turned(yR, pl->yaw);
        }
        TOUCH_LOOK_GLIDE = 0.0f;
        std::snprintf(d, sizeof(d), "coast after release: half-life 0.1 s +%.3f deg, 0.6 s +%.3f deg", far[0], far[1]);
        check(far[0] > 0.1f && far[1] > far[0] * 1.5f, "step 7: the glide slider sets how far it coasts", d);
    }

    // ---- Step 8: arbitration unchanged. A drag on the joystick pad turns nothing; a second world
    // finger ends the first one's look (stock's num > 1 rule); pitch clamps at +80.
    {
        float jx = 0, jy = 0, jw = 0, jh = 0;
        const char* st = eden_debug_display_state();
        const char* p2 = st ? std::strstr(st, "\"rjoystick\":[") : nullptr;
        if (p2) std::sscanf(p2 + 13, "%f,%f,%f,%f", &jx, &jy, &jw, &jh);
        level(0.0f);
        if (jw > 0 && jh > 0) drag(jx + jw * 0.5f - 20.0f, SCREEN_HEIGHT - (jy + jh * 0.5f), 20, 2.0f);
        tick(30);
        std::snprintf(d, sizeof(d), "pad [%.0f,%.0f,%.0f,%.0f], turned %.4f deg", jx, jy, jw, jh, turned(0.0f, pl->yaw));
        check(jw > 0 && std::fabs(turned(0.0f, pl->yaw)) < 1e-4f, "step 8: a drag on the joystick pad turns nothing", d);

        level(0.0f);
        const int a = ++ident, b = ++ident;
        float x = x0 - 80.0f;
        eden_input_pointer_event(0, a, x, y0);
        tick(1);
        for (int f = 0; f < 3; ++f) { x += 2.0f; eden_input_pointer_event(1, a, x, y0); tick(1); }   // inside the slop
        const float yA = pl->yaw;
        eden_input_pointer_event(0, b, x0 + 100.0f, y0 + 40.0f);
        tick(1);
        for (int f = 0; f < 30; ++f) { x += 2.0f; eden_input_pointer_event(1, a, x, y0); tick(1); }
        const float yB = pl->yaw;
        eden_input_pointer_event(2, a, x, y0);
        for (int f = 1; f <= 10; ++f) { eden_input_pointer_event(1, b, x0 + 100.0f + 3.0f * f, y0 + 40.0f); tick(1); }
        eden_input_pointer_event(2, b, x0 + 130.0f, y0 + 40.0f);
        tick(2);
        std::snprintf(d, sizeof(d), "one finger %.3f deg (want %.3f), then %.4f with two down", turned(0.0f, yA),
                      0.0f, turned(yA, yB));
        check(std::fabs(turned(0.0f, yA)) < 1e-4f && std::fabs(turned(yA, yB)) < 1e-4f,
              "step 8: a second world finger stops a look that has not latched yet (num > 1)", d);

        level(0.0f);
        const int id = ++ident;
        float y = SCREEN_HEIGHT - 10.0f;
        eden_input_pointer_event(0, id, x0, y);
        tick(1);
        while (y > 10.0f) { y -= 5.0f; eden_input_pointer_event(1, id, x0, y); tick(1); }   // up the screen
        const float pTop = pl->pitch;
        for (int f = 0; f < 10; ++f) { y += 2.0f; eden_input_pointer_event(1, id, x0, y); tick(1); }
        const float pBack = pl->pitch;
        eden_input_pointer_event(2, id, x0, y);
        tick(2);
        std::snprintf(d, sizeof(d), "pitch %.3f at the top, %.3f after 20 points back (want %.3f)", pTop, pBack,
                      80.0f - 20.0f * deg);
        check(std::fabs(pTop - 80.0f) < 1e-3f && std::fabs(pBack - (80.0f - 20.0f * deg)) < 0.01f,
              "step 8: pitch clamps at +80 and turns back at once", d);
    }

    // ---- Step 8b (L.7): a look finger that is already dragging keeps turning when another finger
    // taps (stick finger held on the pad, look drag, a third finger taps and lifts).
    {
        float jx = 0, jy = 0, jw = 0, jh = 0;
        const char* st = eden_debug_display_state();
        const char* p2 = st ? std::strstr(st, "\"rjoystick\":[") : nullptr;
        if (p2) std::sscanf(p2 + 13, "%f,%f,%f,%f", &jx, &jy, &jw, &jh);
        level(0.0f);
        const int stick = ++ident, a = ++ident, b = ++ident;
        if (jw > 0 && jh > 0) { eden_input_pointer_event(0, stick, jx + jw * 0.5f, SCREEN_HEIGHT - (jy + jh * 0.5f)); tick(1); }
        float x = x0 - 80.0f;
        eden_input_pointer_event(0, a, x, y0);
        tick(1);
        for (int f = 0; f < 15; ++f) { x += 2.0f; eden_input_pointer_event(1, a, x, y0); tick(1); }
        eden_input_pointer_event(0, b, x0 + 100.0f, y0 + 40.0f);   // the tap
        tick(2);
        eden_input_pointer_event(2, b, x0 + 100.0f, y0 + 40.0f);
        tick(2);
        const float yT = pl->yaw;
        for (int f = 0; f < 20; ++f) { x += 2.0f; eden_input_pointer_event(1, a, x, y0); tick(1); }
        const float after = turned(yT, pl->yaw);
        eden_input_pointer_event(2, a, x, y0);
        if (jw > 0 && jh > 0) eden_input_pointer_event(2, stick, jx + jw * 0.5f, SCREEN_HEIGHT - (jy + jh * 0.5f));
        tick(2);
        std::snprintf(d, sizeof(d), "turned %.3f deg in the 20 frames after the tap (want %.3f)", after, 40.0f * deg);
        check(std::fabs(after - 40.0f * deg) < 0.05f, "step 8b: the look keeps turning after another finger taps (L.7)", d);
    }

    // ---- Step 9 (D5): the slop is latched. 40 points out, back through the start and 40 beyond:
    // the camera turns for the whole return path, none of it lost near the start.
    {
        level(0.0f);
        const int id = ++ident;
        float x = x0;
        eden_input_pointer_event(0, id, x, y0);
        tick(1);
        for (int f = 0; f < 20; ++f) { x += 2.0f; eden_input_pointer_event(1, id, x, y0); tick(1); }
        for (int f = 0; f < 40; ++f) { x -= 2.0f; eden_input_pointer_event(1, id, x, y0); tick(1); }
        eden_input_pointer_event(2, id, x, y0);
        tick(2);
        const float want = ((40.0f - slop) - 80.0f) * deg;
        std::snprintf(d, sizeof(d), "turned %.3f deg (want %.3f)", turned(0.0f, pl->yaw), want);
        check(std::fabs(turned(0.0f, pl->yaw) - want) < 0.01f, "step 9: an out-and-back drag turns the whole way back", d);
    }

    // ---- Step 3 (D3), last because it gives headless a window box: the same 60-WINDOW-point swipe
    // turns 60 - 12 window points' worth at every window size and UI scale (the engine point space
    // differs), at the phone rate on a phone-sized window and the tablet rate otherwise.
    {
        struct K { int w, h, ui; } ks[] = {{1024, 768, 4}, {1024, 768, 3}, {852, 393, 4}, {852, 393, 3}, {2048, 1536, 4}};
        float scMin = 1e9f, scMax = 0.0f;
        for (const K& k : ks) {
            set_setting_by_key("ui_scale", (float)k.ui);
            eden_display_set_viewport(k.w, k.h);
            tick(3);
            const float sc = TOUCH_PT_SCALE, dg = TOUCH_LOOK_DEG_PER_PT;
            scMin = std::fmin(scMin, sc);
            scMax = std::fmax(scMax, sc);
            float x, y;
            centre(&x, &y);
            level(0.0f);
            drag(x - 30.0f / sc, y, 30, 2.0f / sc);
            const float want = (60.0f - TOUCH_SLOP_PT) * dg;
            const bool phone = (k.h < 500 || k.w < 500);
            std::snprintf(d, sizeof(d), "%dx%d ui %d: %.0fx%.0f points, %.3f window pt/pt, %.2f deg/pt, turned %.3f (want %.3f)",
                          k.w, k.h, k.ui, SCREEN_WIDTH, SCREEN_HEIGHT, sc, dg, turned(0.0f, pl->yaw), want);
            check(std::fabs(turned(0.0f, pl->yaw) - want) < want * 0.01f && dg == (phone ? 0.65f : 0.5f),
                  "step 3: a 60-window-point swipe turns the same per window point at any size and UI scale", d);
        }
        std::snprintf(d, sizeof(d), "TOUCH_PT_SCALE %.3f..%.3f", scMin, scMax);
        check(scMax > scMin * 1.2f, "step 3: the sizes really do differ in window points per engine point", d);
    }

    std::printf("[eden-look] %s (%d failure(s))\n", g_selftestFailures ? "FAILURES" : "ALL PASS", g_selftestFailures);
    return g_selftestFailures ? 1 : 0;
}

int run_touch_selftest() {
    g_selftestTag = "eden-touch";
    g_tickInput = true;

    // A touch test runs in the TOUCH profile, on every platform. Without this the desktop legs
    // test the touch translator against a layout that has no on-screen joystick at all
    // (`use_joystick` is a profile field — Settings.cpp's eden_apply_input_profile), so the
    // stick checks below would be asserting against chrome that is not drawn. iOS already sets
    // this in main(); saying it again here is what makes the four platforms run the same test.
    eden_set_detected_touch(1);
    eden_test_force_touch_input();   // X.19: a stored Input mode of Keyboard+Mouse would beat the line above

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
    // DisplayProfile.cpp can still make the two differ on an extreme aspect). Assuming the
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
            // Fixed 1/60 s etime for EVERYTHING from here to the walk checks (X.11). The coast
            // check below flaked on loaded machines (Linux CI, 4/4 locally under load) because it
            // ran on wall-clock etime: Player::update integrates pos += vel*etime AND substeps
            // (re-applying its per-call damping) when a frame's displacement exceeds 0.1, so one
            // 20-tick window that caught a slow frame covered more ground than the first. The
            // touch joystick does not go through Movement.cpp's wall-clock EMA on native (only
            // the keyboard path does), so engine etime is the only clock this block reads.
            g_app->setFixedEtime(1.0f / 60.0f);
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
            // (1.02 lost to 1.175 vs 1.151 on Windows CI; that "substep jitter" was wall-clock etime,
            // fixed at the top of this block in X.11 — the 1.05 margin is now headroom, not noise.)
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
            eden_set_fly_mode(0);   // (still on the fixed step set at the top of this block)
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
            g_app->setFixedEtime(0.0f);
            eden_set_fly_mode(1);
            const float dw = std::sqrt((w1.x - w0.x) * (w1.x - w0.x) + (w1.z - w0.z) * (w1.z - w0.z));
            std::snprintf(detail, sizeof(detail),
                          "%.2f blocks in 2 s of engine time on flat stone (~7.3 expected), dy %.2f",
                          dw, w1.y - w0.y);
            check(dw > 4.0f && std::fabs(w1.y - w0.y) < 0.5f, "a held stick walks with fly OFF", detail);
        }
    }

    // I.2: every edge-anchored HUD rect (and the rail-screen panel) inside the safe rect. Only a
    // real check where there are insets (`--device=iphone16`, `--safe-area=`, a notched device);
    // there it also proves it has teeth: with the insets ignored the same layout must FAIL.
    {
        float sx = 0, sy = 0, sw = 0, sh = 0;
        eden_safe_rect(&sx, &sy, &sw, &sh);
        char who[512];
        const int bad = eden_debug_safe_area_violations(sx, sy, sw, sh, who, sizeof(who));
        std::snprintf(detail, sizeof(detail), "safe rect %.0f,%.0f %.0fx%.0f in %.0fx%.0f; %d outside%s%s",
                      sx, sy, sw, sh, SCREEN_WIDTH, SCREEN_HEIGHT, bad, bad ? ": " : "", who);
        check(bad == 0, "every HUD hit box lies inside the safe rect", detail);
        if (sw < SCREEN_WIDTH || sh < SCREEN_HEIGHT) {
            eden_display_debug_ignore_safe_area(1);
            const int bad0 = eden_debug_safe_area_violations(sx, sy, sw, sh, who, sizeof(who));
            eden_display_debug_ignore_safe_area(0);
            std::snprintf(detail, sizeof(detail), "%d rect(s) outside with the insets ignored: %s", bad0, who);
            check(bad0 > 0, "the safe-area gate fails with the inset ignored", detail);
        }
    }

    std::printf("[eden-touch] %s (%d failure(s))\n",
                g_selftestFailures ? "FAILURES" : "ALL PASS", g_selftestFailures);
    return g_selftestFailures ? 1 : 0;
}

}  // namespace g_look
}  // namespace harness
