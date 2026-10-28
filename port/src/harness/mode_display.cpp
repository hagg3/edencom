// G.4: split out of the old eden_main_native.cpp (see harness.h). Mode group "display".
#include "harness.h"

using namespace harness;

namespace harness {
namespace g_display {


// ---------------------------------------------------------------------------------------------
// W.2: native gates for the retired web checks that had no replacement (docs/web-port-archive.md)
// ---------------------------------------------------------------------------------------------
// Number readers for the eden_debug_* JSON probes. Each probe writes a fixed key set, so a strstr
// on `"key":` is exact; a missing key reads as -9999 (no real field has that value).
int probe_int(const char* json, const char* key) {
    char k[64];
    std::snprintf(k, sizeof(k), "\"%s\":", key);
    const char* p = json ? std::strstr(json, k) : nullptr;
    if (!p) return -9999;
    p += std::strlen(k);
    if (!std::strncmp(p, "true", 4)) return 1;
    if (!std::strncmp(p, "false", 5)) return 0;
    return atoi(p);
}
double probe_num(const char* json, const char* key) {
    char k[64];
    std::snprintf(k, sizeof(k), "\"%s\":", key);
    const char* p = json ? std::strstr(json, k) : nullptr;
    return p ? atof(p + std::strlen(k)) : -9999.0;
}
struct ProbeRect { float v[4]; };
ProbeRect probe_rect(const char* json, const char* key) {
    ProbeRect r = {{-1, -1, -1, -1}};
    char k[64];
    std::snprintf(k, sizeof(k), "\"%s\":[", key);
    const char* p = json ? std::strstr(json, k) : nullptr;
    if (p) std::sscanf(p + std::strlen(k), "%f,%f,%f,%f", &r.v[0], &r.v[1], &r.v[2], &r.v[3]);
    return r;
}
bool same_rect(const ProbeRect& a, const ProbeRect& b) {
    for (int i = 0; i < 4; ++i) if (std::fabs(a.v[i] - b.v[i]) >= 0.01f) return false;
    return true;
}
std::string rect_str(const ProbeRect& r) {
    char b[96];
    std::snprintf(b, sizeof(b), "[%g,%g,%g,%g]", r.v[0], r.v[1], r.v[2], r.v[3]);
    return b;
}

// --recolor-selftest  (W.2 (b); was web-port-final:web/tools/headless-recolor-test.js, audit row 11 / A5)
// The paint / door / creature-skin recolour pipeline. Its failure is invisible: a Texture2D whose
// GL name stayed 0 draws as nothing and reports no error. eden_debug_recolor_state(color)
// (port/src/seam/DebugState.cpp) reports the stored source images, the recoloured textures'
// names and the positional skin/mask tables Resources fills while loading. Headless, the names are
// the shim's fake ones -- nonzero is still exactly the "something was uploaded" signal.
extern "C" const char* eden_debug_recolor_state(int color);
int run_recolor_selftest() {
    g_selftestTag = "eden-recolor";
    tick(3);
    Resources::getResources->loadGameAssets();   // Q.3: the paint/door art is captured by the game-texture load, which the menu no longer does
    std::printf("[eden-recolor] metrics IS_IPAD=%d SUPPORTS_RETINA=%d SCALE=%gx%g SCREEN=%gx%g\n", (int)IS_IPAD,
                (int)SUPPORTS_RETINA, SCALE_WIDTH, SCALE_HEIGHT, SCREEN_WIDTH, SCREEN_HEIGHT);
    char d[600];
    std::string s1 = eden_debug_recolor_state(1);
    std::snprintf(d, sizeof(d), "%s", s1.c_str());
    check(probe_int(s1.c_str(), "stored_paint") != -9999, "Resources exists and the probe answers", d);
    std::printf("[eden-recolor] colour 1: %s\n", s1.c_str());
    const char* s = s1.c_str();
    check(probe_int(s, "stored_paint") == 1, "storedPaint was captured during texture load", "");
    check(probe_int(s, "stored_paint_mask") == 1, "storedPaintMask was captured during texture load", "");
    check(probe_int(s, "stored_door") == 1, "storedDoor was captured during texture load", "");
    check(probe_int(s, "stored_door_mask") == 1, "storedDoorMask was captured during texture load", "");
    const int pw = probe_int(s, "paint_w"), ph = probe_int(s, "paint_h");
    const int mw = probe_int(s, "mask_w"), mh = probe_int(s, "mask_h");
    std::snprintf(d, sizeof(d), "paint %dx%d, mask %dx%d", pw, ph, mw, mh);
    check(pw > 0 && ph > 0, "paint image decoded to real pixels", d);
    check(mw > 0 && mh > 0, "paint mask decoded to real pixels", d);
    check(pw == mw && ph == mh, "paint image and mask agree on size", d);
    check(pw == 90 && mw == 90, "the ipad~ (2x) paint art loaded, as on a device", d);
    check(probe_int(s, "paint_tex") > 0, "getPaintTex(1) produced a texture (name != 0)", "");
    check(probe_int(s, "door_tex") > 0, "getDoorTex(1) produced a texture (name != 0)", "");

    // paint_cache is ONE slot keyed on colour (a second colour must rebuild it); door_cache is
    // per colour, so two colours are two live names at once.
    const std::string a = eden_debug_recolor_state(2);
    const std::string b = eden_debug_recolor_state(7);
    const int da = probe_int(a.c_str(), "door_tex"), db = probe_int(b.c_str(), "door_tex");
    std::snprintf(d, sizeof(d), "door 2 -> %d, door 7 -> %d", da, db);
    check(probe_int(b.c_str(), "paint_tex") > 0, "a second paint colour also produces a texture", "");
    check(da > 0 && db > 0 && da != db, "door textures for two colours are distinct names", d);

    // The creature half: Resources fills storedSkins/storedMasks POSITIONALLY (the Nth texture load
    // after loadResources zeroes the counters), so a reordered load silently swaps creatures' skins.
    // 5 models x 2 states; 15 skin PNGs load and every third (the Rage variant) is not stored.
    std::snprintf(d, sizeof(d), "skins %d, masks %d, counters %d/%d", probe_int(s, "skins_filled"),
                  probe_int(s, "masks_filled"), probe_int(s, "skin_counter"), probe_int(s, "mask_counter"));
    check(probe_int(s, "skins_filled") == 10, "all 10 creature skins captured", d);
    check(probe_int(s, "masks_filled") == 10, "all 10 creature masks captured", d);
    check(probe_int(s, "skin_counter") == 10, "skin counter stopped at 10 (every 3rd skipped)", d);
    check(probe_int(s, "mask_counter") == 10, "mask counter stopped at 10", d);

    const std::string a2 = eden_debug_recolor_state(2);
    std::snprintf(d, sizeof(d), "%d then %d", da, probe_int(a2.c_str(), "door_tex"));
    check(probe_int(a2.c_str(), "door_tex") == da, "the door cache returns a stable name per colour", d);

    std::printf("[eden-recolor] %s (%d failure(s))\n", g_selftestFailures ? "FAILURES" : "ALL PASS", g_selftestFailures);
    return g_selftestFailures ? 1 : 0;
}

// --display-selftest  (W.2 (c); was web-port-final:web/tools/headless-display-profile-test.js, audit D1 + D4)
// The derived point space (port/src/seam/DisplayProfile.cpp) and the re-layout it drives
// (Hud::layoutForScreen, Menu::layoutForScreen, Input::screenMetricsChanged). A rect that does not
// follow a metrics change is drawn in one place and hit-tested in another, silently. Pins: (1) the
// stock 568x320 HUD rects, byte-exact; (2) the derivation; (3) idempotent, reversible re-layout;
// (4) the profiles' chrome and the classic-width floor; (5) the aspect clamps; (6) X.20's whole-
// pixels-per-point snap on the desktop profile. --touch-selftest
// covers the touch half at a real window; this one is headless and drives the viewport itself
// (eden_display_set_viewport is exactly what DisplayMode_native.cpp calls on a window resize).
extern "C" void eden_display_set_viewport(int css_w, int css_h);
extern "C" void eden_display_set_viewport_px(int css_w, int css_h, int px_w, int px_h);   // X.20
extern "C" int  eden_display_px_per_point(void);
extern "C" void eden_display_refresh(void);
int run_display_selftest() {
    g_selftestTag = "eden-display";
    if (!tick_until([] { return probe_int(eden_debug_display_state(), "hud") == 1; }, 600, "the Hud")) {
        check(false, "World + Hud came up", eden_debug_display_state());
        std::printf("[eden-display] FAILURES (%d failure(s))\n", g_selftestFailures);
        return 1;
    }
    char d[300];
    std::string st;
    auto state = [&]() -> const char* { st = eden_debug_display_state(); return st.c_str(); };
    // Option indexes: ui_scale 0 Auto, 1 100%, 2 125%, 3 150%, 4 200%; display_layout 0 Auto,
    // 1 Classic, 2 Adaptive; input_mode 0 Auto, 1 Touch, 2 Keyboard+Mouse.
    auto set = [](const char* key, float v) {
        if (!set_setting_by_key(key, v)) std::fprintf(stderr, "[eden-display] no setting %s\n", key);
        tick(2);
    };
    auto pickViewport = [&](const char* label, const char* s) {
        // Util.mm's findWorldCoords unprojects a point-space tap scaled by SCALE_* (2) against
        // GL_VIEWPORT, so GL_VIEWPORT must answer the point space x2, never the real drawable.
        ProbeRect pv = probe_rect(s, "pick_viewport");   // two numbers; the other two stay -1
        const double w = probe_num(s, "SCREEN_WIDTH"), h = probe_num(s, "SCREEN_HEIGHT");
        std::snprintf(d, sizeof(d), "%s: [%g,%g] vs %gx%g x2", label, pv.v[0], pv.v[1], w, h);
        check(pv.v[0] == (float)(w * 2) && pv.v[1] == (float)(h * 2), "GL_VIEWPORT is the point space x2", d);
    };
    auto gridInCard = [&](const char* label, const char* s) {
        const ProbeRect c = probe_rect(s, "rpaintframe");
        bool ok = true;
        for (const char* k : {"block0", "color0"}) {
            const ProbeRect r = probe_rect(s, k);
            ok = ok && r.v[0] >= c.v[0] && r.v[1] >= c.v[1] && r.v[0] + r.v[2] <= c.v[0] + c.v[2] &&
                 r.v[1] + r.v[3] <= c.v[1] + c.v[3];
        }
        std::snprintf(d, sizeof(d), "%s: card %s block0 %s color0 %s", label, rect_str(c).c_str(),
                      rect_str(probe_rect(s, "block0")).c_str(), rect_str(probe_rect(s, "color0")).c_str());
        check(ok, "the picker grid sits inside the picker card", d);
    };
    auto dims = [&](const char* s) {
        std::snprintf(d, sizeof(d), "%gx%g (aspect %.4f)", probe_num(s, "SCREEN_WIDTH"), probe_num(s, "SCREEN_HEIGHT"),
                      probe_num(s, "P_ASPECT_RATIO"));
        return (const char*)d;
    };

    // ---- 1. The classic profile is byte-exact ----------------------------------------------------
    set("input_mode", 2);
    set("display_layout", 1);
    set("ui_scale", 4);
    const std::string classic = state();
    const char* c = classic.c_str();
    check(probe_num(c, "SCREEN_WIDTH") == 568 && probe_num(c, "SCREEN_HEIGHT") == 320, "classic: 568x320 points", dims(c));
    check(std::fabs(probe_num(c, "P_ASPECT_RATIO") - 1.775) < 0.001, "classic: P_ASPECT_RATIO is 1.775", dims(c));
    check(probe_int(c, "IS_WIDESCREEN") == 1, "classic: IS_WIDESCREEN", "");
    const ProbeRect scr = probe_rect(c, "scr");
    std::snprintf(d, sizeof(d), "Input [%g,%g]", scr.v[0], scr.v[1]);
    check(scr.v[0] == 568 && scr.v[1] == 320, "classic: Input's point space matches the engine", d);
    // The stock rects, by hand from Classes/Hud.mm's constants at 568x320:
    //   HUDR_X = 568-45-13 = 510; rmine.y = 4*320/5-45-3 = 208; rjumphit.x = 510-17 = 493, w = 75 (FM.3 dropped stock's -1);
    //   rmenu.y = 320-45 = 275; blockBounds[0] = (7+15+4+57, -3+320-10-15-38) = (83, 254);
    //   rburn.y = 320-45-3 = 272; rpaint.y = 2*320/5-45-3+5 = 85; rpaintframe = (4+57, 10+10) 402x282.
    const struct { const char* key; ProbeRect want; } kStock[] = {
        {"rmine",       {{510, 208, 45, 45}}},
        {"rjumphit",    {{493, 0, 75, 68}}},
        {"rmenu",       {{0, 275, 45, 45}}},
        {"block0",      {{83, 254, 38, 38}}},
        {"rburn",       {{510, 272, 45, 45}}},
        {"rpaint",      {{510, 85, 45, 45}}},
        {"rpaintframe", {{61, 20, 402, 282}}},
    };
    for (const auto& k : kStock) {
        const ProbeRect got = probe_rect(c, k.key);
        char what[64];
        std::snprintf(what, sizeof(what), "classic: %s is the stock rect", k.key);
        std::snprintf(d, sizeof(d), "%s, want %s", rect_str(got).c_str(), rect_str(k.want).c_str());
        check(same_rect(got, k.want), what, d);
    }
    pickViewport("classic", c);

    // ---- 2. The derivation ----------------------------------------------------------------------
    set("display_layout", 2);
    set("ui_scale", 1);
    eden_display_set_viewport(1600, 1000);
    tick(2);
    const std::string wide = state();
    const char* w = wide.c_str();
    check(probe_num(w, "SCREEN_HEIGHT") == 640, "adaptive/100%: point height is 640", dims(w));
    check(probe_num(w, "SCREEN_WIDTH") == 1024, "adaptive/100%: point width follows the 1.6 aspect", dims(w));
    check(std::fabs(probe_num(w, "P_ASPECT_RATIO") - probe_num(w, "SCREEN_WIDTH") / probe_num(w, "SCREEN_HEIGHT")) < 0.0005,
          "adaptive: P_ASPECT_RATIO is always width/height", dims(w));
    check(std::abs(probe_int(w, "aspect_x1000") - 1600) <= 2, "adaptive: the aspect getter agrees", dims(w));
    const ProbeRect wscr = probe_rect(w, "scr");
    std::snprintf(d, sizeof(d), "Input [%g,%g]", wscr.v[0], wscr.v[1]);
    check(wscr.v[0] == 1024 && wscr.v[1] == 640, "adaptive: Input's point space followed", d);
    check(std::fabs(probe_rect(w, "rmine").v[0] - (1024 - 45 - 13)) < 0.01, "adaptive: the right-anchored column followed",
          rect_str(probe_rect(w, "rmine")).c_str());
    check(std::fabs(probe_rect(w, "rmenu").v[1] - (640 - 45)) < 0.01, "adaptive: the top-anchored icon followed",
          rect_str(probe_rect(w, "rmenu")).c_str());
    pickViewport("adaptive/100%", w);
    gridInCard("adaptive/100%", w);
    {
        const float burnY = probe_rect(w, "rburn").v[1], paintY = probe_rect(w, "rpaint").v[1];
        std::snprintf(d, sizeof(d), "rburn.y %g, rpaint.y %g", burnY, paintY);
        // rpaint carries a stock +5 nudge on top of its three 64-point steps.
        check(std::fabs((burnY - paintY + 5) - 64 * 3) < 0.01, "adaptive: the mode column keeps its 64-point pitch", d);
        check(std::fabs(burnY - (640 - 45 - 3)) < 0.01, "adaptive: the mode column stays top-anchored", d);
    }
    const ProbeRect mf = probe_rect(w, "rmenuframe");
    check(std::fabs(mf.v[2] - 268) < 0.01 && std::fabs(mf.v[3] - 240) < 0.01, "adaptive: the menu card keeps 268x240",
          rect_str(mf).c_str());
    set("ui_scale", 2);
    const std::string dense = state();
    check(probe_num(dense.c_str(), "SCREEN_HEIGHT") == 512, "ui_scale 125%: point height is 512", dims(dense.c_str()));
    check(std::fabs(probe_num(dense.c_str(), "P_ASPECT_RATIO") - probe_num(w, "P_ASPECT_RATIO")) < 0.005,
          "ui_scale 125%: the aspect is unchanged", dims(dense.c_str()));
    pickViewport("ui_scale 125%", dense.c_str());
    gridInCard("ui_scale 125%", dense.c_str());

    // ---- 3. Re-layout is idempotent and reversible -----------------------------------------------
    set("ui_scale", 1);
    const std::string again = state();
    std::snprintf(d, sizeof(d), "block0 %s vs %s", rect_str(probe_rect(again.c_str(), "block0")).c_str(),
                  rect_str(probe_rect(w, "block0")).c_str());
    check(same_rect(probe_rect(again.c_str(), "rmine"), probe_rect(w, "rmine")) &&
          same_rect(probe_rect(again.c_str(), "block0"), probe_rect(w, "block0")) &&
          same_rect(probe_rect(again.c_str(), "color0"), probe_rect(w, "color0")),
          "idempotent: returning to a size reproduces its rects", d);
    // block0.x carries marginLeft2, which the layout body MUTATES: a pass that did not reset it
    // would drift right on every refresh.
    eden_display_refresh();
    eden_display_refresh();
    tick(2);
    std::snprintf(d, sizeof(d), "block0 %s", rect_str(probe_rect(state(), "block0")).c_str());
    check(same_rect(probe_rect(st.c_str(), "block0"), probe_rect(w, "block0")), "idempotent: extra refreshes do not drift", d);

    // ---- 4. Back to classic: the stock numbers again ---------------------------------------------
    set("display_layout", 1);
    set("ui_scale", 4);
    const std::string back = state();
    bool same = true;
    for (const char* k : {"rmine", "rjumphit", "rmenu", "block0", "color0", "rpaintframe"})
        same = same && same_rect(probe_rect(back.c_str(), k), probe_rect(c, k));
    std::snprintf(d, sizeof(d), "block0 %s", rect_str(probe_rect(back.c_str(), "block0")).c_str());
    check(same, "round trip: classic rects identical after adaptive", d);

    // ---- 5. The profiles ---------------------------------------------------------------------------
    set("input_mode", 1);
    const std::string touch = state();
    check(std::strstr(touch.c_str(), "\"profile\":\"touch\"") != nullptr, "forcing Touch selects the touch profile", "");
    check(probe_int(touch.c_str(), "touch_chrome") == 1 && probe_int(touch.c_str(), "use_joystick") == 1,
          "the touch profile turns the on-screen chrome on", "");
    set("input_mode", 2);
    const std::string desk = state();
    check(std::strstr(desk.c_str(), "\"profile\":\"desktop\"") != nullptr, "forcing Keyboard+Mouse selects desktop", "");
    check(probe_int(desk.c_str(), "touch_chrome") == 0 && probe_int(desk.c_str(), "use_joystick") == 0,
          "the desktop profile turns the on-screen chrome off", "");
    // Auto + the touch profile on a 16:9 screen is the shipped layout, rect for rect.
    set("ui_scale", 0);
    set("display_layout", 0);
    set("input_mode", 1);
    eden_display_set_viewport(1136, 640);
    tick(2);
    const std::string at = state();
    check(probe_num(at.c_str(), "SCREEN_WIDTH") == 568 && probe_num(at.c_str(), "SCREEN_HEIGHT") == 320,
          "Auto + touch on 16:9 is still 568x320", dims(at.c_str()));
    check(same_rect(probe_rect(at.c_str(), "rmine"), probe_rect(c, "rmine")) &&
          same_rect(probe_rect(at.c_str(), "block0"), probe_rect(c, "block0")),
          "Auto + touch reproduces the stock HUD rects", rect_str(probe_rect(at.c_str(), "block0")).c_str());
    // On 4:3 it fills the screen, and the classic-width floor keeps the space >= 568 wide.
    eden_display_set_viewport(1024, 768);
    tick(2);
    const std::string ipad = state();
    check(std::abs(probe_int(ipad.c_str(), "aspect_x1000") - 1333) <= 5, "Auto + touch on 4:3 fills it", dims(ipad.c_str()));
    check(probe_num(ipad.c_str(), "SCREEN_WIDTH") >= 568, "the classic-width floor keeps >= 568 points", dims(ipad.c_str()));
    eden_display_set_viewport(800, 1200);
    tick(2);
    check(probe_num(state(), "SCREEN_WIDTH") >= 568, "the floor holds at the aspect clamp", dims(st.c_str()));
    eden_display_set_viewport(568, 320);
    tick(2);
    check(probe_num(state(), "SCREEN_WIDTH") == 568 && probe_num(st.c_str(), "SCREEN_HEIGHT") == 320,
          "an iPhone-5-sized viewport resolves to 568x320", dims(st.c_str()));

    // ---- 6. The clamps ----------------------------------------------------------------------------
    set("input_mode", 2);
    eden_display_set_viewport(400, 900);
    tick(2);
    check(std::abs(probe_int(state(), "aspect_x1000") - 1200) <= 10, "a portrait viewport clamps to aspect 1.20", dims(st.c_str()));
    eden_display_set_viewport(3840, 1080);
    tick(2);
    check(std::abs(probe_int(state(), "aspect_x1000") - 2400) <= 10, "a 32:9 viewport clamps to aspect 2.40", dims(st.c_str()));

    // ---- 7. X.20: the pixel snap (desktop, adaptive) ----------------------------------------------
    // Given the box in device pixels, the desktop point space is a whole number of pixels per point
    // (1 or even preferred; odd only when 25% closer). Expected values are the measured windows:
    // the default 1136x640 on the 1x and the 2x monitor, a maximised 1080p window, a maximised
    // window on the "looks like 2304x1296" 2x screen.
    set("display_layout", 2);
    set("ui_scale", 2);
    const struct { int cw, ch, pw, ph, wantW, wantH, wantP; const char* what; } kSnap[] = {
        {1136, 640, 1136,  640, 1136, 640, 1, "125%, default window, 1x: 1 px/pt"},
        {1136, 640, 2272, 1280, 1136, 640, 2, "125%, default window, 2x: 2 px/pt, same points"},
        {1920, 1050, 1920, 1050, 960, 525, 2, "125%, maximised 1080p: 2 px/pt, art 1:1, odd height kept"},
        {2304, 1240, 4608, 2480, 1152, 620, 4, "125%, maximised 2x 2304-wide: 4 px/pt"},
    };
    for (const auto& k : kSnap) {
        eden_display_set_viewport_px(k.cw, k.ch, k.pw, k.ph);
        tick(2);
        std::snprintf(d, sizeof(d), "%s, snap %d", dims(state()), eden_display_px_per_point());
        check(probe_num(st.c_str(), "SCREEN_WIDTH") == k.wantW && probe_num(st.c_str(), "SCREEN_HEIGHT") == k.wantH &&
              eden_display_px_per_point() == k.wantP, k.what, d);
    }
    pickViewport("snapped 4 px/pt", st.c_str());
    set("ui_scale", 4);
    eden_display_set_viewport_px(1920, 1050, 1920, 1050);
    tick(2);
    std::snprintf(d, sizeof(d), "%s, snap %d", dims(state()), eden_display_px_per_point());
    check(probe_num(st.c_str(), "SCREEN_HEIGHT") == 350 && eden_display_px_per_point() == 3,
          "200%, maximised 1080p: an odd 3 px/pt when it is much closer", d);
    set("ui_scale", 2);
    eden_display_set_viewport(1136, 640);
    tick(2);
    std::snprintf(d, sizeof(d), "%s, snap %d", dims(state()), eden_display_px_per_point());
    check(probe_num(st.c_str(), "SCREEN_HEIGHT") == 512 && eden_display_px_per_point() == 0,
          "no pixel size: unsnapped, the old 512-point space", d);
    set("input_mode", 1);
    set("ui_scale", 0);
    eden_display_set_viewport_px(1136, 640, 2272, 1280);
    tick(2);
    std::snprintf(d, sizeof(d), "%s, snap %d", dims(state()), eden_display_px_per_point());
    check(probe_num(st.c_str(), "SCREEN_WIDTH") == 568 && probe_num(st.c_str(), "SCREEN_HEIGHT") == 320 &&
          eden_display_px_per_point() == 0, "the touch profile is never snapped", d);
    set("input_mode", 2);

    std::printf("[eden-display] %s (%d failure(s))\n", g_selftestFailures ? "FAILURES" : "ALL PASS", g_selftestFailures);
    return g_selftestFailures ? 1 : 0;
}

// --convert64-selftest  (W.2 (e); was web-port-final:web/tools/headless-256z-convert-crosscheck.js)
// FileManager::convertWorldTo64 is a from-scratch C++ restatement of port/tools/eden-convert.js's
// `--to-64`; the two are kept in step by hand. This authors a 256z world with known content above
// y=63 (a 6x6 lattice of brick pillars at four heights, 20 apart so each lands in its own column,
// and three door pairs straddling the cut: DOOR1 at 63 kept, DOOR_TOP at 64 cut = one orphan each),
// saves it, exports the `.emod` to a NewDawn256z `.eden`, then converts with the engine twice --
// the `.emod` (what the Storage tab runs on a live world) and a copy of the `.eden` (an import
// source) -- and prints each report. port/tools/convert64-crosscheck.js runs this mode, converts the
// kept reference copy (<docs>/convert64-ref/src256.eden) with eden-convert.js and asserts all
// three agree. The in-process checks below are only the floor: the content really landed above 63.
int run_convert64_selftest() {
    g_selftestTag = "eden-convert64";
    const char* name = g_opt.world.empty() ? "convert64-xcheck" : g_opt.world.c_str();
    if (!open_world(name, 256)) return 1;
    tick(g_opt.frames / 2);
    char d[600];
    check(g_world_height == 256, "the world opened at height 256", eden_debug_world_format());

    const PlayerState ps = player_state();
    const int bx = (int)std::lround(ps.x), bz = (int)std::lround(ps.z);
    const int kHighY[] = {64, 90, 150, 220};
    int placed = 0;
    for (int gx = 0; gx < 6; ++gx)
        for (int gz = 0; gz < 6; ++gz)
            for (int y : kHighY) { eden_console_setblock(bx + (gx - 3) * 20, bz + (gz - 3) * 20, y, 13 /* TYPE_BRICK */); placed++; }
    // Off the lattice (its offsets are multiples of 20 in -60..40), so no door top overwrites a brick.
    const int kDoors[3][2] = {{bx + 60, bz}, {bx, bz + 60}, {bx - 50, bz - 50}};
    for (const auto& c : kDoors) {
        eden_console_setblock(c[0], c[1], 63, 66 /* TYPE_DOOR1 */);
        eden_console_setblock(c[0], c[1], 64, 70 /* TYPE_DOOR_TOP */);
    }
    // Creatures above the cut, for the dropped/relocated counters: a 5x5 brick deck at y=150, off the
    // lattice (x and z offsets 8..12), and three creatures stood on it just before the save.
    for (int x = bx + 8; x <= bx + 12; ++x)
        for (int z = bz + 8; z <= bz + 12; ++z) { eden_console_setblock(x, z, 150, 13 /* TYPE_BRICK */); placed++; }
    std::snprintf(d, sizeof(d), "getblock(%d,%d,220) = %d", bx, bz, eden_console_getblock(bx, bz, 220));
    check(eden_console_getblock(bx, bz, 220) == 13, "a high-band block reads back before saving", d);
    std::snprintf(d, sizeof(d), "door at 63/64: %d/%d", eden_console_getblock(kDoors[0][0], kDoors[0][1], 63),
                  eden_console_getblock(kDoors[0][0], kDoors[0][1], 64));
    check(eden_console_getblock(kDoors[0][0], kDoors[0][1], 63) == 66 &&
          eden_console_getblock(kDoors[0][0], kDoors[0][1], 64) == 70, "a door pair straddles the cut", d);
    tick(30);
    int spawned = 0;
    for (int i = 0; i < 3; ++i) spawned += SpawnCreatureAt(i, Vector{bx + 9.5f + i, 151.5f, bz + 10.5f}) ? 1 : 0;
    std::snprintf(d, sizeof(d), "%d of 3", spawned);
    check(spawned == 3, "three creatures spawn on the deck above the cut", d);
    save_world();
    if (!quit_to_menu()) return 1;
    tick(10);

    int idx = -1;
    for (int i = 0; i < eden_menu_world_count(); ++i) if (!std::strcmp(eden_menu_world_name(i), name)) idx = i;
    check(idx >= 0, "the world is listed after the save", name);
    if (idx < 0) return 1;
    const std::string docs = eden_platform_documents_root();
    const std::string file = eden_menu_world_file(idx);
    const std::string refDir = docs + "/convert64-ref";
    mkdirs(refDir);
    const std::string ref = refDir + "/src256.eden", engEden = "convert64-xcheck-src.eden";
    emod::ExportOptions xo;
    xo.target = emod::X_NEWDAWN256Z;
    std::string err;
    const bool exA = emod::export_world_file(docs + "/" + file, ref, xo, false, nullptr, &err);
    const bool exB = exA && emod::export_world_file(docs + "/" + file, docs + "/" + engEden, xo, false, nullptr, &err);
    std::snprintf(d, sizeof(d), "%s -> %s: %s", file.c_str(), ref.c_str(), err.empty() ? "ok" : err.c_str());
    check(exA && exB, "the 256z world exports to a NewDawn256z .eden (twice)", d);
    if (!exA || !exB) return 1;
    std::printf("[eden-convert64] ref %s\n", ref.c_str());
    std::printf("[eden-convert64] placed %d\n", placed + 3);

    auto report = [&](const char* leg, const std::string& target) {
        const ConvertTo64Report r = World::getWorld->fm->convertWorldTo64(target);
        std::printf("[eden-convert64] engine-%s {\"ok\":%s,\"columns\":%d,\"blocksDiscarded\":%d,\"columnsAffected\":%d,"
                    "\"doorsOrphaned\":%d,\"creaturesDropped\":%d,\"creaturesRelocated\":%d,\"creaturesOverflow\":%d}\n",
                    leg, r.ok ? "true" : "false", r.columns, r.blocksDiscarded, r.columnsAffected, r.doorsOrphaned,
                    r.creaturesDropped, r.creaturesRelocated, r.creaturesOverflow);
        std::snprintf(d, sizeof(d), "%s: ok %d \"%s\", discarded %d, orphaned doors %d", leg, r.ok ? 1 : 0, r.error,
                      r.blocksDiscarded, r.doorsOrphaned);
        check(r.ok, "convertWorldTo64 succeeds", d);
        check(r.blocksDiscarded >= placed + 3, "it discards at least the bricks and door tops placed", d);
        check(r.doorsOrphaned == 3, "it clears exactly the three orphaned doors", d);
        std::snprintf(d, sizeof(d), "%s: dropped %d, relocated %d, overflow %d", leg, r.creaturesDropped,
                      r.creaturesRelocated, r.creaturesOverflow);
        check(r.creaturesDropped >= 1, "it drops the creatures above the cut", d);
    };
    report("emod", file);
    report("eden", engEden);

    std::printf("[eden-convert64] %s (%d failure(s))\n", g_selftestFailures ? "FAILURES" : "ALL PASS", g_selftestFailures);
    return g_selftestFailures ? 1 : 0;
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
        {"title-new.mp3",                       ".mp3 (music track)"},
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
        // Drop it again: title-new.mp3 decodes to ~8 M frames, i.e. ~32 MB of PCM in the effect
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
        a->playBackgroundMusic("title-new.mp3", false);
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
                // LG.5: there are no in-game songs now (NUM_SONGS 0), so the check is that no TITLE
                // track is playing over gameplay; it was "an in-game song is playing".
                const bool titlePlaying = ch0 && std::strstr(ch0, "title") && a->isBackgroundMusicPlaying();
                check(!titlePlaying,
                      "Music switched on in a world does not play a title track", detail);
            }
        }
    }

    std::printf("[eden-audio] %s (%d failure(s))\n",
                g_selftestFailures ? "FAILURES" : "ALL PASS", g_selftestFailures);
    return g_selftestFailures ? 1 : 0;
}

// ---------------------------------------------------------------------------------------------
// --leak-probe — the native counterpart of web-port-final:web/tools/headless-alloc-leak-probe.js and of
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

}  // namespace g_display
}  // namespace harness
