// G.4: split out of the old eden_main_native.cpp (see harness.h). Mode group "shots".
#include "harness.h"

using namespace harness;

namespace harness {
namespace g_shots {

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
// `overlay`, when given, draws over that frame before the readback (the `kit` page's way in).
static bool g_shotPinned = false;   // G.5b-1: run_shot()'s determinism pin is on
bool g_fitGate = false;   // I.3: --fit-selftest runs run_shot()'s tour and checks instead of shooting
void fit_check(const char* label);   // defined after check()

bool capture(const char* label, void (*overlay)()) {
    if (g_fitGate) {
        GLW::debugRecordControls(true);
        eden_native_gl_set_present(0);
        tick(1);
        eden_native_gl_set_present(1);
        if (!overlay) fit_check(label);   // the kit page is a demo sheet, not a screen
        GLW::debugRecordControls(false);
        return true;
    }
    int w = 0, h = 0;
    eden_gl_context_get_drawable_size(&w, &h);
    if (w <= 0 || h <= 0) { std::fprintf(stderr, "[eden-shot] no drawable\n"); return false; }
    if (g_shotPinned && World::getWorld && World::getWorld->menu && World::getWorld->menu->menu_back) {
        World::getWorld->menu->menu_back->reset();   // the clouds/pinwheel do not depend on how long a page waited
        eden_rng_seed(1);
    }
    eden_native_gl_set_present(0);
    tick(1);
    if (overlay) overlay();
    std::vector<unsigned char> px((size_t)w * h * 4);
    R::readPixels(0, 0, w, h, px.data());
    eden_native_gl_set_present(1);
    if (g_opt.safeL > 0 || g_opt.safeT > 0 || g_opt.safeR > 0 || g_opt.safeB > 0) {
        // I.0: tint the unsafe bands (island / home bar) red so a shot shows what the hand covers.
        // Rows are bottom-up, so the "top" inset is the last rows. Window points -> drawable pixels.
        int sw = 0, sh = 0;
        if (eden_native_gl_window()) SDL_GetWindowSize(eden_native_gl_window(), &sw, &sh);
        if (sw > 0 && sh > 0) {
            const float kx = (float)w / sw, ky = (float)h / sh;
            const int L = (int)(g_opt.safeL * kx), R = (int)(g_opt.safeR * kx);
            const int T = (int)(g_opt.safeT * ky), B = (int)(g_opt.safeB * ky);
            for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
                if (x < L || x >= w - R || y < B || y >= h - T) {
                    unsigned char* q = px.data() + ((size_t)y * w + x) * 4;
                    q[0] = (unsigned char)((q[0] + 255 * 2) / 3); q[1] = (unsigned char)(q[1] * 2 / 3); q[2] = (unsigned char)(q[2] * 2 / 3);
                }
            }
        }
    }
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


// --shot's `kit` page (U.1): every GL kit control in every state, laid out in the Stage U panel
// (WORKING/ui-rework-plan-2026-10-11.md §3.1) so the geometry can be judged against the reference
// screenshots. It is drawn by the harness over the main-menu frame, not by a screen: the controls
// are the subject, and no screen uses all of them yet. Built once (labels are texture uploads).
void render_kit_page() {
    using namespace GLW;
    struct Kit {
        IconRail rail;
        GLW::Button back, play, defaults, share, tile;
        Label title;
        TwoLineRow rows[9];
        Toggle toggles[9];
        Slider slider;
        Stepper stepper;
        ScrollView scroll;
        CGRect panel, content;
    };
    static Kit* k = nullptr;
    const float pad = du(9), gap = du(6);
    if (!k) {
        k = new Kit;
        const int icons[] = { UIICON_WRENCH, UIICON_VOLUME_2, UIICON_GAMEPAD_2, UIICON_KEYBOARD,
                              UIICON_MONITOR, UIICON_FLASK_CONICAL, UIICON_HARD_DRIVE, UIICON_SETTINGS,
                              UIICON_SQUARE, UIICON_MOUNTAIN, UIICON_PLAY, UIICON_INFO };
        k->rail.setTiles(icons, 12);              // 12 > what fits: the rail scrolls
        k->rail.setEnabled(5, false);
        k->rail.setSelected(0);
        k->rail.setHeld(2);
        k->back.setLabel("Back", du(20));     k->back.sizeToFit();
        k->play.setLabel("Play", du(20));     k->play.setIcon(UIICON_PLAY);
        k->play.setTone(GLW::Button::TONE_POSITIVE); k->play.sizeToFit();
        k->defaults.setLabel("Defaults", du(20)); k->defaults.sizeToFit(); k->defaults.setPressed(true);
        k->share.setLabel("Share", du(20));   k->share.sizeToFit(); k->share.setEnabled(false);
        k->tile.setIcon(UIICON_INFO);         k->tile.sizeToFit();
        k->title.set("Kit Settings", du(32), UITextAlignmentCenter);
    }
    const float W = std::min(SCREEN_WIDTH - 2 * du(12), du(783));
    const float H = std::min(SCREEN_HEIGHT - 2 * du(12), du(587));
    k->panel = CGRectMake((SCREEN_WIDTH - W) * 0.5f, (SCREEN_HEIGHT - H) * 0.5f, W, H);
    const CGRect& P = k->panel;
    const float barH = du(29), barTop = P.origin.y + P.size.height - du(7);
    const float left = P.origin.x + pad, right = P.origin.x + P.size.width - pad;
    k->back.setRect(CGRectMake(left, barTop - barH, 0, barH));
    float x = right;
    GLW::Button* acts[] = { &k->play, &k->defaults, &k->share, &k->tile };
    for (GLW::Button* b : acts) { x -= b->fitWidth(); b->setRect(CGRectMake(x, barTop - barH, 0, barH)); x -= du(5); }
    const float bodyTop = barTop - barH - pad, bodyBot = P.origin.y + pad;
    k->rail.setRect(CGRectMake(left, bodyBot, IconRail::designW(), bodyTop - bodyBot));
    const float barW = du(18);
    const CGRect bar = CGRectMake(right - barW, bodyBot, barW, bodyTop - bodyBot);
    k->scroll.setBarRect(bar);
    const float cx0 = left + IconRail::designW() + gap;
    k->content = CGRectMake(cx0, bodyBot, bar.origin.x - gap - cx0, bodyTop - bodyBot);
    const CGRect& C = k->content;

    // Rows: every toggle state, a dependent pair, a wrapping hint, a slider, a stepper, no hint.
    struct R { const char* t; const char* h; int kind; bool on, muted, indent; };
    static const R spec[9] = {
        { "Health", "Take damage from creatures, explosions and lava.", 0, true, false, false },
        { "Creatures", "Spawn and render creatures.", 0, false, false, false },
        { "Hostile creatures", "Spawn and render Chargers and Stalkers (muted, parent off).", 0, true, true, true },
        { "Night creatures", "Muted and off.", 0, false, true, true },
        { "Render distance", "How far the world draws. Longer is slower; on an old iPad the Low "
          "preset keeps it at four chunks and this hint wraps to a second line to prove it can.", 1, false, false, false },
        { "Graphics preset", "Low, Default or High.", 2, false, false, false },
        { "Auto-jump", nullptr, 0, true, false, false },
        { "Mouse sensitivity", "A slider row.", 1, false, false, false },
        { "Experiments", "Off.", 0, false, false, false },
    };
    float heights[9];
    const float sliderW = du(200), stepW = du(29) * 2 + du(110);
    for (int i = 0; i < 9; ++i) {
        const float cw = spec[i].kind == 0 ? Toggle::designW() : spec[i].kind == 1 ? sliderW : stepW;
        const float ch = spec[i].kind == 0 ? Toggle::designH() : du(29);
        k->rows[i].build(spec[i].t, spec[i].h, C.size.width, cw, ch, spec[i].indent ? du(14) : 0.0f);
        k->rows[i].setMuted(spec[i].muted);
        heights[i] = k->rows[i].height();
    }
    k->scroll.setRowHeights(heights, 9, C.size.height);

    Pen::depthTest(false);
    Pen::blend(true);
    Pen::blendFunc(::R::B_ALPHA);
    Pen::textured(true);
    const float sc = IS_IPAD ? SCALE_WIDTH : 1.0f;
    ::R::begin2D(SCREEN_WIDTH * sc, SCREEN_HEIGHT * sc, 1.0f);

    fill(CGRectMake(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT), kScrim);
    bevel(P, BEVEL_WINDOW);
    k->back.render();
    for (GLW::Button* b : acts) b->render();
    k->title.drawCentered(P.origin.x + P.size.width * 0.5f, barTop - (barH - k->title.height()) * 0.5f, kText);
    k->rail.render();
    bevel(C, BEVEL_CONTENT);
    const int first = k->scroll.first(), last = first + k->scroll.visible();
    for (int i = first; i < last && i < 9; ++i) {
        TwoLineRow& row = k->rows[i];
        row.setOrigin(C.origin.x, C.origin.y + C.size.height - k->scroll.rowOffset(i));
        row.setLast(i == 8);
        row.render();
        if (spec[i].kind == 0) {
            Toggle& t = k->toggles[i];
            t.setRect(row.controlRect(Toggle::designW(), Toggle::designH()));
            t.setOn(spec[i].on);
            t.setMuted(spec[i].muted);
            t.render();
        } else if (spec[i].kind == 1) {
            k->slider.setRange(0, 1, 0.05f);
            k->slider.setValue(i == 4 ? 0.6f : 0.25f);
            k->slider.setRect(row.controlRect(sliderW, du(29)));
            k->slider.render();
        } else {
            k->stepper.setGlyphs("<", ">");
            k->stepper.setValueText("Default", du(20));
            k->stepper.setRect(row.controlRect(stepW, du(29)));
            k->stepper.render();
        }
    }
    k->scroll.render();

    ::R::end2D();
}


int run_shot() {
    const char* name = g_opt.world.empty() ? "shot" : g_opt.world.c_str();
    // No vsync: a capture needs drawn frames, not paced ones, and a vsync-blocked swap never
    // returns while the display is asleep or locked (2026-10-06: --shot sat at tick 0 for minutes
    // on an unattended Mac).
    SDL_GL_SetSwapInterval(0);
    // G.5b-1: the tour is the 2D kit's A/B, so it gets --scene-shot's determinism: engine time in fixed
    // 1/60 s steps, the engine RNG pinned, user input and focus events dropped.
    struct Pin {
        bool on;
        explicit Pin(bool o) : on(o) {
            if (!on) return;
            g_ignoreUserInput = true;
            g_app->viewController.setFixedEtime(1.0f / 60.0f);
            eden_rng_seed(1);
            g_shotPinned = true;
        }
        ~Pin() {
            if (!on) return;
            g_app->viewController.setFixedEtime(0.0f);
            eden_rng_unpin();
            g_ignoreUserInput = false;
            g_shotPinned = false;
        }
    } pin(!g_fitGate);
    tick(180);
    // I.7: -home is the first screen; -menu stays the world list ("My Worlds").
    if (World::getWorld && World::getWorld->menu) World::getWorld->menu->showHome(true);
    tick(4);
    capture("home");
    if (World::getWorld && World::getWorld->menu) World::getWorld->menu->showHome(false);
    tick(4);
    capture("menu");
    capture("kit", render_kit_page);

    // Stage 5.9: Get Worlds, on the archive tab and on the current server's, each with its first
    // world selected and (when the host has one) its preview. Live network unless --net-fixtures
    // was given; a capture of the "couldn't reach" state is still the screen working.
    if (World::getWorld && World::getWorld->menu && World::getWorld->menu->browserOffered()) {
        Menu* m = World::getWorld->menu;
        WorldBrowser* b = m->browser;
        m->openBrowser();
        for (int src = 0; src < 2; ++src) {
            CGRect r = b->tabRect(src);
            ui_tap(r.origin.x + r.size.width * 0.5f, r.origin.y + r.size.height * 0.5f);
            tick_until([b] { return !b->listLoading(); }, 6000, "the browser's list");
            if (b->rowRect(0, &r)) ui_tap(r.origin.x + r.size.width * 0.5f, r.origin.y + r.size.height * 0.5f);
            tick_until([b] { return b->previewShown(); }, 1500, "a preview");
            tick(4);
            capture(src == 0 ? "browser" : "browser-current");
        }
        b->close();
        m->showbrowser = false;
        tick(4);
    }

    // MP.6: the Multiplayer screens over the --mp-mock fixture (UI plan §9): the list, its empty
    // state, the forms (one with a faked keyboard), the password prompt, Connecting, two errors.
    if (World::getWorld && World::getWorld->menu) {
        Menu* m = World::getWorld->menu;
        ServerListMenu* sl = m->servers;
        ServerFormMenu* fm = sl->form();
        char keptName[32];   // the developer's own profile survives a --shot on real prefs
        eden_mp_profile_name(keptName, sizeof keptName);
        const int keptChar = eden_mp_profile_character();
        eden_mp_servers_mock(1);
        eden_mp_profile_set("Sam", 1);
        m->openServers();
        tick_until([] { return eden_mp_servers_poll() == 0; }, 300, "the mock probes");
        tick(4);
        capture("mp-list");
        sl->showTab(1);   // MP.8: the fixture's two matchmakers and four listings
        tick(4);
        capture("mp-list-matchmakers");
        sl->showTab(0);
        tick(2);
        fm->openAdd();
        fm->blurFields();
        tick(4);
        capture("mp-form-add");
        fm->close(ServerFormMenu::OUT_BACK);
        fm->openEdit(0);
        fm->blurFields();
        tick(4);
        capture("mp-form-edit");
        eden_native_debug_set_keyboard_top(SCREEN_HEIGHT * 0.55f);
        fm->field("password")->focus();
        tick(4);
        capture("mp-form-keyboard");
        eden_native_debug_set_keyboard_top(-2.0f);
        fm->close(ServerFormMenu::OUT_BACK);
        fm->openProfile(false);
        fm->blurFields();
        tick(4);
        capture("mp-profile");
        fm->close(ServerFormMenu::OUT_BACK);
        sl->select(0);
        sl->debugPassword(false);
        tick(4);
        capture("mp-password");
        GLDialog::dismiss();
        static const char* const kLines[] = {"[Server] Welcome, Sam! (Character Type: 1)",
                                             "Welcome! No griefing. /help for commands"};
        sl->debugConnecting(EDEN_MP_JOIN_RECEIVING, 0.62f, kLines, 2);
        tick(4);
        capture("mp-connecting");
        sl->back();
        sl->debugError(EDEN_MP_ERR_REFUSED, "Nothing is running at castle.example:27015.");
        tick(4);
        capture("mp-error-refused");
        GLDialog::dismiss();
        sl->debugError(EDEN_MP_ERR_INVALID_NAME, "The server refused the name Sam: name taken.");
        tick(4);
        capture("mp-error-name");
        GLDialog::dismiss();
        sl->close();
        m->showservers = false;
        eden_mp_servers_mock(2);   // the empty fixture (never the real servers.txt: --shot may run on real docs)
        m->openServers();
        tick(4);
        capture("mp-list-empty");
        sl->close();
        m->showservers = false;
        eden_mp_servers_mock(0);
        eden_mp_profile_set(keptName, keptChar);
        tick(4);
    }

    // U.5: the New World screen, one shot per world type (-newworld is the first visit's type,
    // Normal; -newworld-<type> for the others). The field is blurred so the shot shows the
    // placeholder rather than a caret.
    if (World::getWorld && World::getWorld->menu) {
        Menu* m = World::getWorld->menu;
        NewWorldMenu* nw = m->newworld;
        m->openNewWorld();
        nw->blurFields();
        tick(4);
        const int first = nw->type();
        capture("newworld");
        for (int t = 0; t < nw->typeCount(); ++t) {
            if (t == first) continue;
            nw->showType(t);
            tick(4);
            char label[48];
            std::snprintf(label, sizeof(label), "newworld-%s", nw->typeShot(t));
            capture(label);
            // H.4: the Classic tab at Original 64 (Scale hidden), then back to the default height.
            CGRect hb;
            if (!std::strcmp(nw->typeShot(t), "classic") && nw->choice("height") == 256 && nw->choiceRect("height", 1, &hb)) {
                ui_tap_rect(hb);
                tick(4);
                capture("newworld-classic-64");
                if (nw->choiceRect("height", 0, &hb)) ui_tap_rect(hb);
                tick(4);
            }
        }
        nw->showType(first);
        nw->back();
        m->shownewworld = false;
        tick(4);
    }

    // The rewritten generic GL settings screen (Phase N Stage 2.5). Meaningful only from the main
    // menu (in game the panel is a host overlay); shot here before any world loads.
    eden_settings_menu_open_now();
    tick(30);
    capture("settings");
    // U.3: every tab, so a row whose control does not fit is visible without a human clicking
    // through the rail. -settings.bmp stays the first tab (Gameplay; its name predates tabs), then
    // -settings-<tab> for the rest, Keys included (since U.4 that is the keybinds list).
    if (World::getWorld && World::getWorld->menu && World::getWorld->menu->settings) {
        SettingsMenu* sm = World::getWorld->menu->settings;
        for (int t = 1; t < sm->tabCount(); ++t) {
            sm->showTab(t);
            tick(4);
            char label[48];
            std::snprintf(label, sizeof(label), "settings-%s", sm->tabName(t));
            capture(label);
        }
        sm->resetView();
    }

    // Stage 5.3's -keybinds shot is gone with U.4: the list is the Keys tab, -settings-keys above.

    eden_settings_menu_close();
    tick(15);

    if (!open_world(name, g_opt.height)) return 1;
    eden_console_teleport(g_opt.at[0], g_opt.at[1], g_opt.at[2]);
    tick(g_opt.frames);
    capture("hud");

    // The HUD's status line (statusbar.mm, the full-width box). It printed two and a bit copies
    // side by side until Stage 5.6 (Texture2D.cpp's text-texture cap); this is the artefact.
    // N.5.11: it is the GL kit's toast now, which fades in over 0.2 s of etime — and etime is
    // wall clock here, so a fixed tick count catches it part-way. Wait out the fade in real time.
    World::getWorld->hud->sb->setStatus("World Saved", 30);
    {
        const Uint64 t0 = SDL_GetTicks();
        do tick(1); while (SDL_GetTicks() - t0 < 400);
    }
    capture("hud-status");
    World::getWorld->hud->sb->clear();

    // Stage 5.7: the block and colour pickers, opened the way E and C open them (a tap on the
    // HUD's rbuild / rpaint), and closed by the same tap.
    tap_hud(1);
    tick(20);
    capture("picker-blocks");
    // D.2p: page 2, turned the way [ ] and the wheel turn it.
    World::getWorld->hud->pickerPageStep(1);
    tick(10);
    capture("picker-blocks-p2");
    // ...and a page-2 block picked (117, cheese): the HUD's current-block icon and the held block.
    {
        const CGRect c = World::getWorld->hud->blockBounds[5];
        ui_tap(c.origin.x + c.size.width * 0.5f, c.origin.y + c.size.height * 0.5f);
    }
    tick(20);
    capture("hud-newblock");
    // D.3c: SIGN armed (the HUD's current-block button shows its caption), then its prompt.
    if (Hud::pickerToolEnabled(HUD_TOOL_SIGN)) {
        tap_hud(1);
        tick(10);
        const CGRect c = World::getWorld->hud->blockBounds[18];   // H.14: 16 is the flower block, FT.1: 17 weeds
        ui_tap(c.origin.x + c.size.width * 0.5f, c.origin.y + c.size.height * 0.5f);
        tick(20);
        capture("hud-sign");
        tap_hud(1);                    // D.4b: the picker again, SIGN now the selected cell
        tick(20);
        capture("picker-blocks-p2-sign");
        static const char* const kBtn[] = {"OK", "Cancel"};
        GLDialog::prompt("New sign", NULL, "Welcome to my world, traveller. Mind the lava pit past the second door!",
                         95, kBtn, 2, nullptr, 3);
        tick(20);
        capture("sign-prompt");
        GLDialog::dismiss();
        tick(2);
        // D.4b: CMD armed, then the command-block script prompt (multi-line, scrolled to the caret).
        {
            const CGRect c = World::getWorld->hud->blockBounds[19];
            ui_tap(c.origin.x + c.size.width * 0.5f, c.origin.y + c.size.height * 0.5f);
        }
        tick(20);
        capture("hud-cmd");
        GLDialog::prompt("Command block", "Statements separated by ;  Empty clears the script.",
                         "@touch; say Adding...; set ~0 ~2 ~0 gold; wait 1; fill ~-2 ~0 ~-2 ~2 ~3 ~2 glass; "
                         "wait 2; fill ~-2 ~0 ~-2 ~2 ~3 ~2 air; sound door; flash; say Done!",
                         511, kBtn, 2, nullptr, 6);
        tick(20);
        capture("cmd-prompt");
        GLDialog::dismiss();
        tick(2);
        World::getWorld->hud->blocktype = TYPE_STONE;
    }
    tap_hud(2);
    tick(20);
    capture("picker-colors");
    tap_hud(2);
    tick(10);

    // The in-game (ESC) panel, through the same synthetic HUD tap Escape uses. This is the shot
    // that would have caught the suppressed-panel bug (eden_ui_force_legacy) at Stage 2's start
    // instead of at the first play pass.
    eden_tap_hud_button_begin(0);
    tick(6);
    eden_tap_hud_button_end(0);
    tick(30);
    std::printf("[eden-shot] in_menu=%d\n", eden_hud_in_menu());
    capture("pausemenu");

    // Stage 5.5: Settings opened from the pause menu — the GL SettingsMenu as an in-game modal.
    // Set directly, as the pause menu's Settings button does; Back is the screen's own path.
    World::getWorld->menu->settings->resetView();
    World::getWorld->menu->showsettings = true;
    tick(10);
    capture("pausemenu-settings");
    World::getWorld->menu->showsettings = false;
    tick(2);

    // C.3: the Console over the running world -- full (a few lines through the real path), Variables,
    // Help, then the collapsed strip. Opened from play (the pause menu is toggled off and back on).
    eden_tap_hud_button_begin(0); tick(6); eden_tap_hud_button_end(0); tick(10);
    {
        ConsoleMenu* cm = World::getWorld->menu->console;
        eden_console_menu_open();
        tick(5);
        static const char* const kLines[] = { "help set", "where", "set 0 3 0 lamp", "video.fov", "fill 0 0 0 9 2 9 glas", "echo done" };
        for (const char* l : kLines) { cm->setInput(l); cm->submit(); tick(2); }
        tick(10);
        capture("console");
        cm->setInput("fi");
        cm->complete();
        tick(5);
        capture("console-complete");
        cm->showTab(ConsoleMenu::TAB_VARIABLES);
        tick(10);
        capture("console-variables");
        cm->showTab(ConsoleMenu::TAB_HELP);
        tick(10);
        capture("console-help");
        cm->showTab(ConsoleMenu::TAB_CONSOLE);
        cm->setCollapsed(true);
        tick(10);
        capture("console-strip");
        cm->setCollapsed(false);
        // C.4: a pretend on-screen keyboard over the bottom 30% (iPad-like), then 62% (iPhone
        // landscape: the auto strip). The keyboard itself is not drawn; the console's bottom is its top.
        eden_native_debug_set_keyboard_top(SCREEN_HEIGHT * 0.3f);
        tick(10);
        capture("console-keyboard");
        eden_native_debug_set_keyboard_top(SCREEN_HEIGHT * 0.62f);
        tick(10);
        capture("console-keyboard-tall");
        eden_native_debug_set_keyboard_top(-2.0f);
        cm->close();
        tick(5);
    }
    eden_tap_hud_button_begin(0); tick(6); eden_tap_hud_button_end(0); tick(10);

    // The GL modal (Phase N Stage 2.5) — the ESC menu's Home button raises this. Shown directly
    // here rather than hunting the icon's rect; the point is the artefact.
    ::showAlertWarpHome();
    tick(20);
    capture("dialog");

    // N.4.5: the main menu with a world selected (its Properties button, Rename's slot since H.15)
    // and the rename prompt (no longer reached from the list, kept as an artefact of the dialog).
    // H.15: World Properties -- the World tab without a photo, then with camera mode's photo
    // (taken here, in game, by the H.16 path), and the Terrain tab.
    GLDialog::dismiss();
    tick(2);
    {
        const std::string png = std::string(eden_platform_documents_root()) + "/" +
                                World::getWorld->terrain->world_name + ".png";
        std::remove(png.c_str());
    }
    if (quit_to_menu()) {
        tick(60);
        capture("menu-world");
        Menu* m = World::getWorld ? World::getWorld->menu : NULL;
        if (m && m->renameOffered()) {
            m->beginRename();
            tick(10);
            capture("rename");
            GLDialog::dismiss();
            tick(2);
        }
        if (m && m->propertiesOffered()) {
            m->openProperties();
            tick(10);
            capture("props-world-nophoto");
            m->props->back();
            m->showprops = false;
            tick(2);
        }
        // Back in for the photo: H.16's takeScreenshot() writes <world file>.png from the frame.
        const int sel = m ? eden_menu_selected_index() : -1;
        if (sel >= 0 && eden_menu_play() == 1 && tick_until([] { return game_mode() == 1; }, 60000, "game_mode == PLAY")) {
            tick(60);
            // H.16b: the real path: camera mode + take_screenshot reads inside Hud::render, over the
            // frame just drawn (a bare call here would read a presented, undefined back buffer).
            // Through the mouse-click path (eden_click_begin/end), not the F2 flag: a click in camera mode
            // once left camera mode before its release and took no photo.
            World::getWorld->hud->mode = MODE_CAMERA;
            eden_click_begin(1);
            tick(2);
            eden_click_end(1);
            tick(3);
            if (quit_to_menu()) {
                tick(30);
                if (m->propertiesOffered()) {
                    m->openProperties();
                    tick(10);
                    capture("props-world");
                    m->props->showTab(WorldPropertiesMenu::TAB_TERRAIN);
                    tick(5);
                    capture("props-terrain");
                    m->props->back();
                    m->showprops = false;
                    tick(2);
                }
            }
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------------------------
// --render-scale / --scale-probe  (ROADMAP N.4.11, the native render-resolution setting)
// ---------------------------------------------------------------------------------------------
// The setting is an ENUM row in Settings.cpp (index into 50/75/100/125%), so a percentage is
// mapped to its option index here rather than written raw. Returns false for a percentage the row
// does not offer, which the caller reports instead of silently rendering at 100%.
extern "C" int eden_settings_count(void);
extern "C" const char* eden_settings_key(int i);
extern "C" void eden_settings_set(int i, float v);
extern "C" int eden_get_render_scale_pct(void);
extern "C" int eden_settings_native_hidden(int i);
bool set_render_scale_pct(int pct) {
    static const int kPct[] = {50, 75, 100, 125};
    int opt = -1;
    for (int j = 0; j < 4; ++j) if (kPct[j] == pct) opt = j;
    if (opt < 0) { std::fprintf(stderr, "[eden-scale] %d%% is not an option (50/75/100/125)\n", pct); return false; }
    for (int i = 0; i < eden_settings_count(); ++i) {
        if (std::strcmp(eden_settings_key(i), "render_scale") == 0) {
            eden_settings_set(i, (float)opt);
            return eden_get_render_scale_pct() == pct;
        }
    }
    return false;
}

// Opens a world, then for each scale: settles, takes a --shot-style capture, and times frames.
// THE TIMING IS CPU+GPU PER FRAME, NOT PRESENT-PACED: swap interval is set to 0 for the probe and
// every frame ends in a 1-pixel glReadPixels, which cannot return until the GPU has finished the
// frame. That is what makes the scales comparable on a device whose present would otherwise
// quantise everything to 16.7 ms steps. The number to read is the median ("p50"); "fps" is
// 1000/p50, i.e. what the frame would allow, not what vsync would show. The capture is the
// on-screen check that the HUD stays sharp while the world softens.
int run_scale_probe() {
    const char* name = g_opt.world.empty() ? "scale-probe" : g_opt.world.c_str();
    if (!open_world(name, g_opt.height)) return 1;
    if (g_opt.haveAt) eden_console_teleport(g_opt.at[0], g_opt.at[1], g_opt.at[2]);
    tick(g_opt.frames);                       // let the window fill before timing anything
    SDL_GL_SetSwapInterval(0);
#if defined(EDEN_PLATFORM_IOS)
    const int kScales[] = {100, 75, 50, 100};   // no 125% on iOS (Settings.cpp, N.4.11b)
#else
    const int kScales[] = {100, 75, 50, 125, 100};
#endif
    const int kTimed = 240;
    for (int pct : kScales) {
        if (!set_render_scale_pct(pct)) return 1;
        tick(30);
        char label[32];
        std::snprintf(label, sizeof(label), "scale%d", pct);
        capture(label);
        std::vector<double> ms;
        ms.reserve(kTimed);
        unsigned char px[4];
        for (int i = 0; i < kTimed; ++i) {
            const Uint64 t0 = SDL_GetTicksNS();
            tick(1);
            R::readPixels(0, 0, 1, 1, px);
            ms.push_back((double)(SDL_GetTicksNS() - t0) / 1e6);
        }
        std::sort(ms.begin(), ms.end());
        const double p50 = ms[ms.size() / 2], p90 = ms[ms.size() * 9 / 10];
        int dw = 0, dh = 0;
        eden_gl_context_get_drawable_size(&dw, &dh);
        std::printf("[eden-scale] %3d%%  scene %4dx%-4d  p50 %6.2f ms  p90 %6.2f ms  (%5.1f fps)  mem %s\n",
                    pct, dw * pct / 100, dh * pct / 100, p50, p90, 1000.0 / p50, eden_debug_heap());
    }
    SDL_GL_SetSwapInterval(1);
    return 0;
}

// --preset-probe (B.8-measure): the same timing as --scale-probe, but over the graphics presets and
// the view-distance steps B.5a offers. Each step is p50/p90 of CPU+GPU frame time at the spot you
// give with --at; the last step restores Default. Put --shot=PREFIX before it for the captures.
int set_setting_by_key(const char* key, float v) {
    for (int i = 0; i < eden_settings_count(); ++i)
        if (std::strcmp(eden_settings_key(i), key) == 0) { eden_settings_set(i, v); return 1; }
    return 0;
}
int run_preset_probe() {
    const char* name = g_opt.world.empty() ? "preset-probe" : g_opt.world.c_str();
    if (!open_world(name, g_opt.height)) return 1;
    if (g_opt.haveAt) eden_console_teleport(g_opt.at[0], g_opt.at[1], g_opt.at[2]);
    tick(g_opt.frames);
    SDL_GL_SetSwapInterval(0);
    struct Step { const char* label; const char* key; float v; };
    const Step steps[] = {
        {"preset Default", "graphics_preset", 2}, {"preset Low", "graphics_preset", 1},
        {"preset High", "graphics_preset", 3},    {"view Auto", "view_distance", 0},
        {"view 4", "view_distance", 1}, {"view 5", "view_distance", 2}, {"view 6", "view_distance", 3},
        {"view 7", "view_distance", 4}, {"preset Default", "graphics_preset", 2},
    };
    const int kTimed = 240;
    for (const Step& st : steps) {
        if (!set_setting_by_key(st.key, st.v)) { std::fprintf(stderr, "[eden-preset] no setting %s\n", st.key); return 1; }
        tick(30);
        std::vector<double> ms;
        ms.reserve(kTimed);
        unsigned char px[4];
        for (int i = 0; i < kTimed; ++i) {
            const Uint64 t0 = SDL_GetTicksNS();
            tick(1);
            R::readPixels(0, 0, 1, 1, px);
            ms.push_back((double)(SDL_GetTicksNS() - t0) / 1e6);
        }
        std::sort(ms.begin(), ms.end());
        char label[48];
        std::snprintf(label, sizeof(label), "preset-%s-%s", st.key, st.label);
        for (char* c = label; *c; ++c) if (*c == ' ') *c = '-';
        capture(label);
        std::printf("[eden-preset] %-16s p50 %6.2f ms  p90 %6.2f ms  (%5.1f fps)  mem %s\n",
                    st.label, ms[ms.size() / 2], ms[ms.size() * 9 / 10], 1000.0 / ms[ms.size() / 2], eden_debug_heap());
    }
    SDL_GL_SetSwapInterval(1);
    return 0;
}

// --pass-probe[=N] (P.1b): where a frame goes, at the Stage P benchmark poses. Opens a world, and
// per pose teleports, settles (--frames, default 600), then times N frames (default 600) with the
// per-pass timers on (Classes/PassTimers.h) and prints one `[eden-pass] <pose>.passes {...}` line
// (PassAgg::print). Vsync is OFF unless --pass-vsync, so the `present` pass is time blocked in the
// swap -- the GPU proxy: near zero means the GPU kept up with the CPU. --pass-finish adds a
// glFinish at every pass boundary (each pass then includes its own GPU work; an upper bound, and
// the frame total is no longer a real frame). --at/--look replace the pose list with one pose.
int run_pass_probe() {
    const char* name = g_opt.world.empty() ? "pass-probe" : g_opt.world.c_str();
    struct Pose { const char* label; float x, y, z, yaw, pitch; };
    // The bundled map. `home` is the stock harness origin (--stage1's, 43 blocks above a lake), kept
    // so these numbers sit beside every other run's; `forest` and `water` are, of the 16x16 tiles in
    // the window loaded there, the one with the most leaf/trunk blocks (390) and a tile of the
    // 256-column lake, each ~8 blocks above its surface (picked once by a scan, 2026-10-12). All
    // three are inside that first window, so moving between them streams little.
    std::vector<Pose> poses = {
        {"home",   65536.5f, 60.0f, 65536.5f,  45.0f, -10.0f},
        {"forest", 65656.5f, 30.0f, 65448.5f, 135.0f, -15.0f},
        {"water",  65528.5f, 25.0f, 65592.5f,  45.0f, -10.0f},
    };
    if (g_opt.haveAt)
        poses = {{"at", g_opt.at[0], g_opt.at[1], g_opt.at[2], g_opt.haveLook ? g_opt.look[0] : 45.0f,
                  g_opt.haveLook ? g_opt.look[1] : -10.0f}};
    if (!open_world(name, g_opt.height)) return 1;
    {
        const char* ext = R::driverString(R::DS_EXTENSIONS);   // ES / compatibility only; core says NULL
        std::printf("[eden-pass] gl {\"renderer\":\"%s\",\"version\":\"%s\",\"ext_disjoint_timer_query\":%d}\n",
                    R::driverString(R::DS_RENDERER), R::driverString(R::DS_VERSION),
                    ext ? (std::strstr(ext, "GL_EXT_disjoint_timer_query") != nullptr) : -1);
        while (R::takeError() != 0) {}   // a core context answers GL_EXTENSIONS with INVALID_ENUM
    }
    if (!g_opt.passVsync) SDL_GL_SetSwapInterval(0);
    Player* pl = World::getWorld->player;
    int rc = 0;
    for (const Pose& ps : poses) {
        for (int k = 0; k < g_opt.frames; ++k) {
            eden_console_teleport(ps.x, ps.y, ps.z);
            pl->yaw = ps.yaw; pl->pitch = ps.pitch;
            tick(1);
        }
        int batt = -1;
        SDL_GetPowerInfo(nullptr, &batt);
        const int thermal0 = eden_native_thermal_state();
        PassAgg agg;
        g_pass.on = true; g_pass.finish = g_opt.passFinish;
        for (int k = 0; k < g_opt.passFrames; ++k) {
            eden_console_teleport(ps.x, ps.y, ps.z);
            pl->yaw = ps.yaw; pl->pitch = ps.pitch;
            pass_frame_reset();
            const Uint64 t0 = SDL_GetPerformanceCounter();
            tick(1);
            agg.add((double)(SDL_GetPerformanceCounter() - t0) * 1000.0 / (double)SDL_GetPerformanceFrequency());
        }
        agg.print("eden-pass", ps.label, g_opt.passVsync ? 1 : 0, thermal0, batt);
        g_pass.on = g_pass.finish = false;
        if (!g_opt.headless && PassAgg::q(agg.draws[P_R_CHUNKS], 0.5) == 0) rc = 1;   // a window that drew no terrain
    }
    SDL_GL_SetSwapInterval(1);
    return rc;
}
void fit_check(const char* label) {
    float sx = 0, sy = 0, sw = 0, sh = 0;
    eden_safe_rect(&sx, &sy, &sw, &sh);
    char who[400], detail[600], what[96];
    int total = 0;
    // The pause column is drawn under the Settings/dialog screens opened from it, so wherever it
    // overflows, its buttons are excused on every screen but its own.
    std::vector<CGRect> pause;
    bool overflow = false;
    if (World::getWorld && World::getWorld->hud && eden_hud_in_menu() == 1) {
        const Hud* h = World::getWorld->hud;
        const Button* pm[] = { &h->rresume, &h->rsave, &h->rhome, &h->rcam, &h->rsettings, &h->rconsole, &h->rexit };
        for (const Button* b : pm) {
            if (b->size.width <= 0) continue;
            pause.push_back(CGRectMake(b->origin.x, b->origin.y, b->size.width, b->size.height));
            if (b->origin.y < sy || b->origin.y + b->size.height > sy + sh) overflow = true;
        }
    }
    const bool own = !std::strcmp(label, "pausemenu");
    const std::vector<CGRect> none;
    const int bad = GLW::debugControlsOutside(CGRectMake(sx, sy, sw, sh), who, sizeof(who), &total,
                                              overflow && !own ? pause : none);
    std::snprintf(detail, sizeof(detail), "%d of %d outside %.0f,%.0f %.0fx%.0f%s%s", bad, total,
                  sx, sy, sw, sh, bad ? ": " : "", who);
    if (own) {
        std::snprintf(what, sizeof(what), "pausemenu: %s", overflow ? "OVERFLOWS" : "every control inside");
        check(total > 0 && !overflow && bad == 0, what, detail);
        return;
    }
    std::snprintf(what, sizeof(what), "%s: every control inside", label);
    check(total > 0 ? bad == 0 : true, what, detail);
}

int run_fit_selftest() {
    g_selftestTag = "eden-fit";
    g_fitGate = true;
    const int rc = run_shot();
    std::printf("[eden-fit] %s (%d failure(s)) at %.0fx%.0f points, touch floor %.0f\n",
                (rc || g_selftestFailures) ? "FAILURES" : "ALL PASS", g_selftestFailures,
                SCREEN_WIDTH, SCREEN_HEIGHT, GLW::touchFloor());
    return (rc || g_selftestFailures) ? 1 : 0;
}

}  // namespace g_shots
}  // namespace harness
