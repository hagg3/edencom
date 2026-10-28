// G.4: split out of the old eden_main_native.cpp (see harness.h). Mode group "ui".
#include "harness.h"

using namespace harness;

namespace harness {
namespace g_ui {


// ---------------------------------------------------------------------------------------------
// --keybind-selftest  (Phase N Stage 5.2/5.3)
// ---------------------------------------------------------------------------------------------
// Stage 5.2 moved the keybind map out of `public/eden-keybinds.js` and into the shared C model
// (port/src/seam/Settings.cpp). The thing that has to be true afterwards is not "the setter
// stored a number" — it is that the INPUT PATH reads the model, i.e. that rebinding moves the
// behaviour off the old key and onto the new one. So this asserts BOTH directions: the new key
// works AND the old one has stopped. A test that only checked the new key would pass against an
// Input_native.cpp that still had its hard-coded `switch (sc)` and merely also consulted the
// model, which is exactly the half-ported state this stage could have landed in.
//
// It also asserts the double-binding case, because "one key, two actions" is shipped
// configuration (Ctrl is flyDown and crouch) and the iterator that honours it
// (eden_keybind_action_after) is new code with exactly one caller.
extern "C" void eden_test_force_touch_input(void);   // Settings.cpp (X.19)
extern "C" void eden_text_input_clamp_keep(float, float, float*, float*);   // Input_native.cpp (FT.6)
extern "C" {
int  eden_keybind_count(void);
int  eden_keybind_index(const char* action);
int  eden_keybind_get(int i);
int  eden_keybind_default(int i);
void eden_keybind_set(int i, int code);
void eden_keybind_reset_all(void);
int  eden_keybind_action_after(int prev, int code);
int  eden_keybind_capture_active(void);
void eden_keybind_capture_begin(int i);
int  eden_keybind_capture_feed(int code);
const char* eden_keybind_code_name(int code);
}

// ---------------------------------------------------------------------------------------------
// --ui-selftest  (Stage 5.4 widgets + N.4.5 text entry and world rename)
// ---------------------------------------------------------------------------------------------
// Drives the REAL screens with the inputs a player would produce — pointer events at the rects
// the screen itself reports (SettingsMenu::showRow), SDL text/key events pushed through the pump
// — and asserts on the model and the disk, never on the widgets' own state. Headless: none of
// this needs a pixel, and --shot is the artefact for what it looks like.
extern "C" {
float eden_settings_get(int i);
int   eden_settings_kind(int i);
int   eden_settings_enum_count(int i);
void  eden_input_pointer_event(int phase, int identity, float x, float y);
}
int settings_index(const char* key) {
    for (int i = 0; i < eden_settings_count(); ++i)
        if (std::strcmp(eden_settings_key(i), key) == 0) return i;
    return -1;
}

// Engine touches are y-UP point space; eden_input_pointer_event takes y-DOWN (window order).
const int kUiPointer = 4242;
void ui_pointer(int phase, float x, float yUp) {
    eden_input_pointer_event(phase, kUiPointer, x, SCREEN_HEIGHT - yUp);
}
void ui_tap(float x, float yUp) {
    ui_pointer(0, x, yUp); tick(2);
    ui_pointer(2, x, yUp); tick(2);
}

void push_text(const char* utf8) {          // must be a literal: SDL keeps the pointer
    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_EVENT_TEXT_INPUT;
    e.text.text = utf8;
    SDL_PushEvent(&e);
}
void push_tap_key(SDL_Scancode sc) { push_key(sc, true); push_key(sc, false); }

int  g_promptChosen = -2;
std::string g_promptText;
void ui_prompt_cb(int chosen, const char* text) { g_promptChosen = chosen; g_promptText = text ? text : ""; }

bool read_file(const std::string& path, std::vector<unsigned char>* out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    out->clear();
    unsigned char buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out->insert(out->end(), buf, buf + n);
    std::fclose(f);
    return true;
}

void ui_tap_rect(CGRect r) { ui_tap(r.origin.x + r.size.width * 0.5f, r.origin.y + r.size.height * 0.5f); }

// I.7: put the main menu on the home screen or on the world list ("My Worlds") with the tap a
// player would make (the home's My Worlds tile, the list's Back). Boot lands on home; Quit to Menu
// and a Get Worlds download land on the list.
void ui_menu_to(bool home) {
    Menu* m = World::getWorld ? World::getWorld->menu : NULL;
    if (!m || (m->showhome != false) == home) return;
    ui_tap_rect(m->controlRect(home ? "back" : "home.myworlds"));
    tick(3);
}

void push_wheel(float y) {
    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_EVENT_MOUSE_WHEEL;
    e.wheel.y = y;
    e.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
    SDL_PushEvent(&e);
}

// MP.6: the Multiplayer screens (UI plan §9's tap paths), from the home screen and back to it.
// A real servers.txt in this run's --docs first (add, save, probe, join -> profile -> error, delete),
// then the --mp-mock fixture (one row per status, the secure prompt, the keyboard rule).
std::string mp_read_servers_txt() {
    const std::string p = std::string(eden_platform_documents_root()) + "/servers.txt";
    FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) return std::string();
    std::string s;
    char b[1024];
    size_t n;
    while ((n = std::fread(b, 1, sizeof b, f)) > 0) s.append(b, n);
    std::fclose(f);
    return s;
}

int mp_ui_checks(Menu* m) {
    char detail[300];
    ServerListMenu* sl = m->servers;
    ServerFormMenu* fm = sl->form();
    std::remove((std::string(eden_platform_documents_root()) + "/servers.txt").c_str());
    eden_mp_profile_set("", 0);
    ui_tap_rect(m->controlRect("home.multiplayer"));
    tick(3);
    check(m->showservers && eden_mp_servers_count() == 0, "a fresh list is empty", "");
    check(sl->controlRect("emptyadd").size.width > 0, "the empty list offers + Add server", "");

    // Add: typed, validated live, saved.
    ui_tap_rect(sl->controlRect("add"));
    tick(3);
    check(fm->isOpen() && fm->mode() == ServerFormMenu::MODE_ADD, "Add opens the server form", "");
    check(fm->field("address")->focused(), "Add focuses the Address field under a pointer", "");
    push_text("bad host");
    tick(3);
    std::snprintf(detail, sizeof detail, "hint '%s' save %d", fm->hint("address"), fm->saveEnabled());
    check(!fm->saveEnabled() && !std::strcmp(fm->hint("address"), "No spaces in an address"),
          "an address with a space disables Save and says why", detail);
    fm->field("address")->setText("");
    push_text("127.0.0.1:1");
    tick(3);
    check(fm->saveEnabled(), "a valid host:port enables Save", fm->hint("address"));
    ui_tap_rect(fm->controlRect("save"));
    tick(3);
    const std::string txt = mp_read_servers_txt();
    check(!fm->isOpen() && eden_mp_servers_count() == 1 && txt.find("\t127.0.0.1\t1\t") != std::string::npos &&
          txt.compare(0, 15, "emod-servers 1\n") == 0, "Save writes servers.txt and lists the row", txt.c_str());
    tick_until([sl] { EdenMpServerRow r; return eden_mp_servers_get(0, &r) && r.status == EDEN_MP_ST_NOT_RUNNING; }, 400,
               "the probe of a closed port");
    EdenMpServerRow r0;
    eden_mp_servers_get(0, &r0);
    std::snprintf(detail, sizeof detail, "status %d", r0.status);
    check(r0.status == EDEN_MP_ST_NOT_RUNNING, "the new row is probed (a closed port reads Not running)", detail);

    // Join with no name: the Profile form first; Save continues the join, which fails into the dialog.
    CGRect rr;
    sl->rowRect(0, &rr);
    ui_tap_rect(rr);   // the selected row: a tap joins
    tick(3);
    check(fm->isOpen() && fm->mode() == ServerFormMenu::MODE_PROFILE, "the first Join with no name opens Profile", "");
    fm->field("name")->setText("");
    push_text("a:b");
    tick(3);
    check(!fm->saveEnabled() && !std::strcmp(fm->hint("name"), "No : [ or ]"), "Profile refuses a name with ':'", fm->hint("name"));
    fm->field("name")->setText("");
    push_text("Sam");
    tick(3);
    ui_tap_rect(fm->controlRect("character.inc"));
    tick(2);
    ui_tap_rect(fm->controlRect("save"));
    tick(3);
    char nm[32];
    eden_mp_profile_name(nm, sizeof nm);
    check(!std::strcmp(nm, "Sam") && eden_mp_profile_character() == 1, "Profile saves the name and character", nm);
    check(sl->connecting() || GLDialog::active(), "Profile's Save continues the join", "");
    tick_until([] { return GLDialog::active(); }, 600, "the join's error dialog");
    CGRect b2;
    check(GLDialog::active() && GLDialog::buttonRect(2, &b2), "a refused join shows Retry / Edit server / Back", "");
    if (GLDialog::active()) { GLDialog::buttonRect(2, &b2); ui_tap_rect(b2); tick(3); }
    check(!GLDialog::active() && !sl->connecting() && m->showservers, "Back leaves the dialog on the list", "");

    // Connecting -> Cancel (a server that accepts and never answers).
    {
        eden_net::Sock ls;
        int port = 0;
        eden_net::tcpListen(0, ls, &port);
        char host[64];
        std::snprintf(host, sizeof host, "127.0.0.1");
        const int i = eden_mp_servers_add("Silent", host, port, "");
        eden_mp_servers_save();
        sl->select(i);
        tick(2);
        ui_tap_rect(sl->controlRect("join"));
        tick(5);
        std::snprintf(detail, sizeof detail, "sel %d sel-count %d dialog %d form %d tab %d", sl->selected(), eden_mp_servers_count(), GLDialog::active(), fm->isOpen(), sl->tab());
        check(sl->connecting(), "Join opens the Connecting window", detail);
        ui_tap_rect(sl->controlRect("cancel"));
        tick(3);
        EdenMpJoinState js;
        eden_mp_join_state(&js);
        check(!sl->connecting() && js.stage == EDEN_MP_JOIN_IDLE && !MpWorld::link(), "Cancel closes the join", "");
        eden_net::closeSock(ls);
        eden_mp_servers_remove(i);
        eden_mp_servers_save();
    }

    // Edit -> Delete -> Remove.
    sl->select(0);
    ui_tap_rect(sl->controlRect("edit"));
    tick(3);
    check(fm->isOpen() && fm->mode() == ServerFormMenu::MODE_EDIT, "Edit opens the form on the row", "");
    ui_tap_rect(fm->controlRect("delete"));
    tick(3);
    CGRect b0;
    if (GLDialog::buttonRect(0, &b0)) { ui_tap_rect(b0); tick(3); }
    check(!fm->isOpen() && eden_mp_servers_count() == 0 && mp_read_servers_txt() == "emod-servers 1\n",
          "Delete -> Remove empties the list and the file", "");
    push_tap_key(SDL_SCANCODE_ESCAPE);
    tick(3);
    check(!m->showservers && m->showhome, "Escape on the server list returns home", "");

    // The fixture: one row per status (list plan §4-§5), the secure prompt, the keyboard rule.
    eden_mp_servers_mock(1);
    ui_tap_rect(m->controlRect("home.multiplayer"));
    tick(3);
    tick_until([] { return eden_mp_servers_poll() == 0; }, 300, "the mock probes");
    tick(2);
    static const int kWant[7] = {EDEN_MP_ST_ONLINE, EDEN_MP_ST_ONLINE, EDEN_MP_ST_BASIC, EDEN_MP_ST_FULL,
                                 EDEN_MP_ST_NOT_RUNNING, EDEN_MP_ST_NO_ANSWER, EDEN_MP_ST_UNKNOWN_HOST};
    int okRows = 0;
    for (int i = 0; i < 7; ++i) { EdenMpServerRow r; okRows += eden_mp_servers_get(i, &r) && r.status == kWant[i]; }
    check(eden_mp_servers_count() == 7 && okRows == 7, "the mock list shows every status", "");
    EdenMpServerRow c0;
    eden_mp_servers_get(0, &c0);
    check(!std::strcmp(c0.motd, "Welcome! No griefing. /help for commands") && c0.players == 12 && c0.locked == 1,
          "an INFO reply fills the row", c0.motd);
    // MP.8: the Matchmakers tab (the fixture has two addresses and four canned listings).
    {
        char clean[200], err[200];
        check(eden_mp_matchmakers_validate("a.example, b.example:99 ,", clean, sizeof clean, err, sizeof err) &&
              !std::strcmp(clean, "a.example:27020, b.example:99"), "the Matchmakers setting is cleaned (default port 27020)", clean);
        check(!eden_mp_matchmakers_validate("good.example, bad host", clean, sizeof clean, err, sizeof err) &&
              std::strstr(err, "bad host"), "an invalid Matchmakers entry rejects the save and names it", err);
        check(!eden_mp_matchmakers_validate("1,2,3,4,5,6,7,8,9", clean, sizeof clean, err, sizeof err), "more than 8 matchmakers is refused", err);
        check(sl->tabsShown() && sl->tab() == 0, "matchmakers set: the list shows the tabs, on My servers", "");
        ui_tap_rect(sl->controlRect("tab1"));
        tick(3);
        std::snprintf(detail, sizeof detail, "tab %d rows %d", sl->tab(), eden_mp_matchmaker_rows());
        check(sl->tab() == 1 && eden_mp_matchmaker_rows() == 4, "the Matchmakers tab lists the canned LIST rows", detail);
        EdenMpServerRow mr;
        eden_mp_matchmaker_get(0, &mr);
        check(!std::strcmp(mr.label, "Sky Kingdom") && mr.players == 9, "listings sort by players, then name", mr.label);
        CGRect sv = sl->controlRect("save"), ed = sl->controlRect("edit");
        check(sv.size.width > 0 && sv.origin.x == ed.origin.x, "Save takes Edit's place on the Matchmakers tab", "");
        sl->select(1);
        tick(2);
        ui_tap_rect(sl->controlRect("save"));
        tick(3);
        EdenMpServerRow last;
        const int n = eden_mp_servers_count();
        eden_mp_servers_get(n - 1, &last);
        std::snprintf(detail, sizeof detail, "tab %d count %d last '%s'", sl->tab(), n, last.label);
        check(sl->tab() == 0 && n == 8 && !std::strcmp(last.label, "Moonlit Builders") && sl->selected() == n - 1,
              "Save copies a listing into My servers and selects it", detail);
        eden_mp_servers_remove(n - 1);   // back to the fixture's seven for the checks below
        sl->select(0);
        tick(2);
    }
    sl->select(6);
    tick(2);
    ui_tap_rect(sl->controlRect("join"));
    tick(3);
    check(!sl->connecting() && !GLDialog::active(), "Join is off for an Unknown host row", "");
    sl->select(1);
    sl->debugPassword(false);
    tick(2);
    GLDialog::getDialog();
    push_text("secret");
    tick(3);
    const GLW::TextField* pf = GLDialog::promptField();
    std::snprintf(detail, sizeof detail, "text '%s' shown '%s'", pf ? pf->text().c_str() : "-", pf ? pf->shownText().c_str() : "-");
    check(pf && pf->secure() && pf->text() == "secret" && pf->shownText().find("secret") == std::string::npos,
          "the password prompt's raster never holds the password", detail);
    GLDialog::dismiss();
    tick(2);
    // The keyboard never covers the focused field (the standing rule): the form with a faked keyboard.
    ui_tap_rect(sl->controlRect("add"));
    tick(3);
    const float kb = SCREEN_HEIGHT * 0.55f;
    eden_native_debug_set_keyboard_top(kb);
    fm->field("password")->focus();
    tick(3);
    const CGRect pr = fm->controlRect("password");
    std::snprintf(detail, sizeof detail, "field y %.0f..%.0f keyboard top %.0f foot %d", pr.origin.y, pr.origin.y + pr.size.height, kb,
                  fm->footShown());
    check(pr.origin.y >= kb && !fm->footShown() && fm->controlRect("save").origin.y > kb,
          "a keyboard over the form: the focused field and Save stay above it", detail);
    eden_native_debug_set_keyboard_top(-2.0f);
    fm->blurFields();
    ui_tap_rect(fm->controlRect("back"));
    tick(3);
    push_tap_key(SDL_SCANCODE_ESCAPE);
    tick(3);
    eden_mp_servers_mock(0);
    eden_mp_profile_set("", 0);
    check(!m->showservers && m->showhome, "back home after the multiplayer checks", "");
    return 0;
}

// Stage 5.6: the kit main menu, driven only through taps at the rects the screen reports (and one
// wheel event), asserting on the menu's model — the list, the selection, `loading`, the disk.
// Works on whatever list this machine already has: it adds its own worlds and only deletes those.
int run_menu_selftest() {
    char detail[240];
    Menu* m = World::getWorld->menu;
    tick(5);
    const int n0 = eden_menu_world_count();

    // I.7: the home screen. Boot lands on it; Multiplayer is drawn but disabled until MP.6; My Worlds
    // opens the list, its Back and Escape both return home; Escape on home stays there.
    {
        const bool booted = m->showhome != false;
        if (!booted) { m->showHome(true); tick(3); }
        check(booted, "boot lands on the home screen", "");
        // MP.6: the tile opens the server list now (its own block below); Back returns home.
        ui_tap_rect(m->controlRect("home.multiplayer"));
        tick(3);
        check(m->showservers && !m->showbrowser && !m->showsettings, "the Multiplayer tile opens the server list", "");
        ui_tap_rect(m->servers->controlRect("back"));
        tick(3);
        check(!m->showservers && m->showhome, "the server list's Back returns home", "");
        if (mp_ui_checks(m) != 0) return 1;
        const CGRect gw = m->controlRect("getworlds"), hg = m->controlRect("home.getworlds");
        auto same = [](CGRect a, CGRect b) {
            return a.origin.x == b.origin.x && a.origin.y == b.origin.y && a.size.width == b.size.width &&
                   a.size.height == b.size.height && a.size.width > 0;
        };
        check(same(gw, hg) && same(m->controlRect("settings"), m->controlRect("home.settings")),
              "the old \"getworlds\"/\"settings\" keys resolve to the home's controls", "");
        ui_tap_rect(m->controlRect("home.settings"));
        tick(3);
        check(m->showsettings && m->showhome, "home -> Settings opens settings", "");
        eden_settings_menu_close();
        tick(3);
        check(!m->showsettings && m->showhome, "Settings closes back to the home screen", "");
        ui_tap_rect(m->controlRect("home.myworlds"));
        tick(3);
        check(!m->showhome, "the My Worlds tile opens the world list", "");
        ui_tap_rect(m->controlRect("back"));
        tick(3);
        check(m->showhome, "the list's Back returns home", "");
        push_tap_key(SDL_SCANCODE_ESCAPE);
        tick(3);
        check(m->showhome && !m->showsettings, "Escape on the home screen does nothing", "");
        ui_menu_to(false);
        push_tap_key(SDL_SCANCODE_ESCAPE);
        tick(3);
        check(m->showhome, "Escape on the list returns home", "");
        ui_menu_to(false);
        check(!m->showhome, "back on the list for the checks below", "");
    }

    // U.5: New opens the New World screen and adds nothing; Back closes it, still nothing.
    ui_tap_rect(m->controlRect("new"));
    tick(3);
    NewWorldMenu* nw = m->newworld;
    std::snprintf(detail, sizeof(detail), "shown %d open %d count %d -> %d", m->shownewworld ? 1 : 0,
                  nw->isOpen() ? 1 : 0, n0, eden_menu_world_count());
    check(m->shownewworld && nw->isOpen() && eden_menu_world_count() == n0, "New opens the New World screen", detail);
    check(nw->focused("name") && eden_text_input_active() == 1, "the name field has focus on a desktop", "");
    ui_tap_rect(nw->controlRect("back"));
    tick(3);
    std::snprintf(detail, sizeof(detail), "shown %d count %d (was %d) text input %d", m->shownewworld ? 1 : 0,
                  eden_menu_world_count(), n0, eden_text_input_active());
    check(!m->shownewworld && eden_menu_world_count() == n0 && eden_text_input_active() == 0,
          "Back closes it and creates nothing", detail);

    // A world created through the seam: one more, selected, scrolled into view.
    eden_menu_create_world();
    tick(3);
    int sel = eden_menu_selected_index();
    CGRect rr;
    std::snprintf(detail, sizeof(detail), "count %d -> %d, selected %d, first %d of %d visible",
                  n0, eden_menu_world_count(), sel, m->firstVisibleRow(), m->visibleRows());
    check(eden_menu_world_count() == n0 + 1 && sel == n0 && m->rowRect(sel, &rr),
          "a new world is selected and scrolled into view", detail);
    // Enough worlds that the list must scroll.
    for (int guard = 0; eden_menu_world_count() < m->visibleRows() + 3 && guard < 40; ++guard) {
        eden_menu_create_world();
        tick(3);
    }
    const int total = eden_menu_world_count();
    const int vis = m->visibleRows();
    check(total > vis, "the list is longer than the window (it has to scroll)", "");

    // A tap on a row that is not selected selects it — and does not play it.
    sel = eden_menu_selected_index();
    int pick = m->firstVisibleRow();
    if (pick == sel) pick++;
    if (m->rowRect(pick, &rr)) ui_tap_rect(rr);
    tick(3);
    std::snprintf(detail, sizeof(detail), "tapped %d: selected %d loading %d", pick, eden_menu_selected_index(), m->loading);
    check(eden_menu_selected_index() == pick && m->loading == 0, "tapping a row selects it without playing", detail);

    // Drag the list down two rows: earlier rows come into view, and the drag selects nothing.
    {
        sel = eden_menu_selected_index();
        m->rowRect(m->firstVisibleRow(), &rr);
        const float pitch = rr.size.height;
        eden_menu_select(total - 1);                    // scrolls to the end, so there is room above
        tick(2);
        sel = total - 1;
        const int f0 = m->firstVisibleRow();
        const CGRect L = m->controlRect("list");
        const float x = L.origin.x + L.size.width * 0.5f, y0 = L.origin.y + L.size.height * 0.5f;
        ui_pointer(0, x, y0); tick(2);
        ui_pointer(1, x, y0 - pitch); tick(2);
        ui_pointer(1, x, y0 - 2.0f * pitch); tick(2);
        ui_pointer(2, x, y0 - 2.0f * pitch); tick(2);
        const int f1 = m->firstVisibleRow();
        std::snprintf(detail, sizeof(detail), "first %d -> %d, selected %d (was %d), loading %d",
                      f0, f1, eden_menu_selected_index(), sel, m->loading);
        check(f1 == f0 - 2 && eden_menu_selected_index() == sel && m->loading == 0,
              "dragging the list scrolls by whole rows and selects nothing", detail);

        // The wheel: one notch toward the user is one row later.
        push_wheel(-1.0f);
        tick(3);
        std::snprintf(detail, sizeof(detail), "first %d -> %d", f1, m->firstVisibleRow());
        check(m->firstVisibleRow() == f1 + 1, "a wheel notch scrolls one row", detail);

        // The scrollbar: a tap on the track above the thumb pages up.
        const int f2 = m->firstVisibleRow();
        const CGRect B = m->controlRect("scrollbar");
        ui_tap(B.origin.x + B.size.width * 0.5f, B.origin.y + B.size.height - 2.0f);
        const int want = std::max(0, f2 - vis);
        std::snprintf(detail, sizeof(detail), "first %d -> %d (want %d)", f2, m->firstVisibleRow(), want);
        check(m->firstVisibleRow() == want, "a tap on the scrollbar track pages", detail);
    }

    // Delete: Cancel keeps the world, Delete removes it (the dialog was a no-op before 5.6).
    {
        eden_menu_select(total - 1);
        tick(2);
        const std::string victim = eden_menu_world_name(total - 1);
        ui_tap_rect(m->controlRect("delete"));
        tick(3);
        CGRect b;
        const bool up = GLDialog::active() && GLDialog::buttonRect(1, &b);
        check(up, "Delete asks first", "");
        if (up) ui_tap_rect(b);
        tick(3);
        std::snprintf(detail, sizeof(detail), "count %d (was %d), dialog %d", eden_menu_world_count(), total,
                      GLDialog::active() ? 1 : 0);
        check(eden_menu_world_count() == total && !GLDialog::active(), "Cancel keeps the world", detail);
        ui_tap_rect(m->controlRect("delete"));
        tick(3);
        if (GLDialog::buttonRect(0, &b)) ui_tap_rect(b);
        tick(3);
        bool listed = false;
        for (int i = 0; i < eden_menu_world_count(); ++i) listed |= (victim == eden_menu_world_name(i));
        std::snprintf(detail, sizeof(detail), "count %d (was %d), \"%s\" listed %d", eden_menu_world_count(), total,
                      victim.c_str(), listed ? 1 : 0);
        check(eden_menu_world_count() == total - 1 && !listed, "Delete -> Delete removes the world", detail);
    }

    // Play: the button, then the stock world-type + height dialogs (Flat, then the height's first
    // button -- New Dawn 256, the default since S.5e), to a world.
    {
        const int idx = eden_menu_world_count() - 1;
        eden_menu_select(idx);
        tick(2);
        const std::string name = eden_menu_world_name(idx);
        ui_tap_rect(m->controlRect("play"));
        CGRect b;
        const bool asked = tick_until([] { return GLDialog::active(); }, 600, "world-type dialog");
        if (asked && GLDialog::buttonRect(0, &b)) ui_tap_rect(b);            // Flat
        tick(3);
        if (GLDialog::active() && GLDialog::buttonRect(0, &b)) ui_tap_rect(b); // New Dawn 256 (S.5e)
        const bool played = tick_until([] { return game_mode() == 1; }, 60000, "game_mode == PLAY");
        check(asked && played, "Play -> Flat -> the default height loads the selected world", name.c_str());
        if (!played) return 1;
        std::snprintf(detail, sizeof(detail), "g_world_height %d", g_world_height);
        check(g_world_height == 256, "the height dialog's default (button 0) is New Dawn 256 (S.5e)", detail);
        tick(60);
        if (!quit_to_menu()) return 1;
        tick(30);

        // Now it has a file: Delete must remove that too.
        int at = -1;
        for (int i = 0; i < eden_menu_world_count(); ++i) if (name == eden_menu_world_name(i)) at = i;
        check(at >= 0, "the played world is listed after quitting", name.c_str());
        if (at < 0) return 1;
        const std::string file = std::string(eden_platform_documents_root()) + "/" + eden_menu_world_file(at);
        std::vector<unsigned char> bytes;
        const bool had = read_file(file, &bytes);
        eden_menu_select(at);
        tick(2);
        ui_tap_rect(m->controlRect("delete"));
        tick(3);
        if (GLDialog::buttonRect(0, &b)) ui_tap_rect(b);
        tick(3);
        const bool gone = !read_file(file, &bytes);
        std::snprintf(detail, sizeof(detail), "%s: before %d after %d", file.c_str(), had ? 1 : 0, gone ? 0 : 1);
        check(had && gone, "deleting a played world removes its file", detail);
    }

    // U.5 (and W.2 (d)): the New World screen end to end, through its real controls — a typed name
    // (a two-byte character included), the Flat tile, Classic 64, Play -> a flat 64z world, no
    // dialog, whose file header carries the typed name. Then Escape: it closes the screen.
    {
        NewWorldMenu* nw = m->newworld;
        const int before = eden_menu_world_count();
        ui_tap_rect(m->controlRect("new"));
        tick(3);
        const char* typed = "Selftest V\xC3\xADlag";   // "Selftest Vílag"
        push_text(typed);
        tick(2);
        int flatTile = -1;
        for (int t = 0; t < nw->typeCount(); ++t) if (nw->typeFlat(t)) flatTile = t;
        ui_tap_rect(nw->typeRect(flatTile));
        tick(2);
        CGRect b;
        if (nw->choiceRect("height", 1, &b)) ui_tap_rect(b);     // Original 64
        tick(2);
        std::snprintf(detail, sizeof(detail), "type %d (flat tile %d) height %d name \"%s\" focused %d",
                      nw->type(), flatTile, nw->choice("height"), nw->text("name"), nw->focused("name") ? 1 : 0);
        check(nw->type() == flatTile && nw->choice("height") == 64 && std::strcmp(nw->text("name"), typed) == 0 &&
              !nw->focused("name"), "the rail picks Flat, the pair picks Original 64, the name is typed (and a tap elsewhere blurs it)", detail);
        ui_tap_rect(nw->controlRect("play"));
        tick(2);
        bool dialog = false;
        const bool played = tick_until([&dialog] { dialog |= GLDialog::active(); return game_mode() == 1; }, 60000,
                                       "game_mode == PLAY");
        std::snprintf(detail, sizeof(detail), "played %d dialog %d count %d (was %d) height %d genflat %d",
                      played ? 1 : 0, dialog ? 1 : 0, eden_menu_world_count(), before, g_world_height,
                      World::getWorld->fm->genflat ? 1 : 0);
        check(played && !dialog && eden_menu_world_count() == before + 1 && g_world_height == 64 &&
              World::getWorld->fm->genflat, "Play makes and loads a flat Original 64 world without asking again", detail);
        if (!played) return 1;
        tick(60);
        if (!quit_to_menu()) return 1;
        tick(30);
        int at = -1;
        for (int i = 0; i < eden_menu_world_count(); ++i) if (std::strcmp(eden_menu_world_name(i), typed) == 0) at = i;
        std::string onDisk;
        if (at >= 0) {
            onDisk = World::getWorld->fm->getName(eden_menu_world_file(at));
        }
        std::snprintf(detail, sizeof(detail), "listed at %d, header name \"%s\"", at, onDisk.c_str());
        check(at >= 0 && onDisk == typed, "the world's file header carries the typed name", detail);

        // Escape over the screen is its Back. The field has focus (desktop), so the first Escape
        // only leaves the field; the second closes the screen.
        const int count = eden_menu_world_count();
        ui_tap_rect(m->controlRect("new"));
        tick(3);
        push_tap_key(SDL_SCANCODE_ESCAPE);
        tick(3);
        const bool fieldLeft = !nw->focused("name") && m->shownewworld;
        push_tap_key(SDL_SCANCODE_ESCAPE);
        tick(3);
        std::snprintf(detail, sizeof(detail), "after one: field left %d; after two: shown %d count %d (was %d)",
                      fieldLeft ? 1 : 0, m->shownewworld ? 1 : 0, eden_menu_world_count(), count);
        check(fieldLeft && !m->shownewworld && eden_menu_world_count() == count,
              "Escape leaves the name field, then closes the screen", detail);

        if (at >= 0) {
            eden_menu_select(at);
            tick(2);
            ui_tap_rect(m->controlRect("delete"));
            tick(3);
            if (GLDialog::buttonRect(0, &b)) ui_tap_rect(b);
            tick(3);
        }
    }

    // H.4: the Classic tab through its real controls. Scale is hidden at Original 64 and back at
    // New Dawn 256; a worded seed becomes seedFromText's number; the slider, a choice and a toggle
    // set their fields; Play loads a Classic world whose generator is exactly those settings, and
    // its first save writes the GENERATOR record.
    {
        NewWorldMenu* nw = m->newworld;
        ui_tap_rect(m->controlRect("new"));
        tick(3);
        push_text("Selftest Classic");
        tick(2);
        int classicTile = -1;
        for (int t = 0; t < nw->typeCount(); ++t) if (!std::strcmp(nw->typeShot(t), "classic")) classicTile = t;
        ui_tap_rect(nw->typeRect(classicTile));
        tick(2);
        CGRect b;
        if (nw->choiceRect("height", 1, &b)) ui_tap_rect(b);     // Original 64
        tick(2);
        const bool hidden64 = nw->choice("height") == 64 && !nw->rowVisible("scale") && nw->rowVisible("seed");
        if (nw->choiceRect("height", 0, &b)) ui_tap_rect(b);     // New Dawn 256
        tick(2);
        const bool shown256 = nw->choice("height") == 256 && nw->rowVisible("scale");
        std::snprintf(detail, sizeof(detail), "type %d (classic tile %d) scale hidden at 64 %d, shown at 256 %d",
                      nw->type(), classicTile, hidden64 ? 1 : 0, shown256 ? 1 : 0);
        check(nw->type() == classicTile && hidden64 && shown256, "the Classic tab hides Scale at 64 and shows it at 256", detail);

        const int rolled = nw->seed();
        ui_tap_rect(nw->controlRect("seed"));
        tick(2);
        push_text("hello");
        tick(2);
        const int want = NewWorldMenu::seedFromText("hello");
        std::snprintf(detail, sizeof(detail), "rolled %d, typed \"%s\" -> %d (want %d); \"12345\" -> %d, \"333333\" -> %d",
                      rolled, nw->text("seed"), nw->seed(), want, NewWorldMenu::seedFromText("12345"),
                      NewWorldMenu::seedFromText("333333"));
        check(rolled >= 1 && rolled < (1 << 24) && rolled != 333333 && want >= 1 && want < (1 << 24) &&
              nw->seed() == want && NewWorldMenu::seedFromText("12345") == 12345 &&
              NewWorldMenu::seedFromText("333333") != 333333,
              "a blank seed is the rolled one; digits are the number, words are hashed into [1, 2^24)", detail);

        CGRect sr;
        if (nw->sliderRect("scale", &sr)) ui_tap_rect(CGRectMake(sr.origin.x + sr.size.width - 1, sr.origin.y, 1, sr.size.height));
        tick(2);
        if (nw->choiceRect("trees", 3, &b)) ui_tap_rect(b);      // Dense
        tick(2);
        if (nw->choiceRect("caves", 1, &b)) ui_tap_rect(b);      // 2011
        tick(2);
        if (nw->toggleRect("flowers", &b)) ui_tap_rect(b);       // off
        tick(2);
        ClassicGen want_g;
        nw->classicSettings(&want_g);
        std::snprintf(detail, sizeof(detail), "scale4 %d trees %d caves %d flowers %d clouds %d",
                      want_g.scale4, want_g.trees, want_g.caves, want_g.flowers, want_g.clouds);
        check(want_g.scale4 == 16 && want_g.trees == 3 && want_g.caves == CLASSIC_CAVES_2011 && !want_g.flowers &&
              want_g.clouds && !nw->toggle("flowers"), "the slider, a choice and a toggle set the Classic settings", detail);

        ui_tap_rect(nw->controlRect("play"));
        tick(2);
        bool dialog = false;
        const bool played = tick_until([&dialog] { dialog |= GLDialog::active(); return game_mode() == 1; }, 60000,
                                       "game_mode == PLAY");
        TerrainGenerator* tg = World::getWorld->terrain->tgen;
        const ClassicGen& g = tg->classic;
        std::snprintf(detail, sizeof(detail), "played %d dialog %d height %d genflat %d state %d seed %d/%d scale4 %d trees %d caves %d flowers %d",
                      played ? 1 : 0, dialog ? 1 : 0, g_world_height, World::getWorld->fm->genflat ? 1 : 0,
                      tg->classicState, tg->LEVEL_SEED, g.seed, g.scale4, g.trees, g.caves, g.flowers);
        check(played && !dialog && g_world_height == 256 && !World::getWorld->fm->genflat &&
              tg->classicState == TerrainGenerator::CLASSIC_STATE_SET && tg->LEVEL_SEED == want && g.seed == want &&
              g.profile == CLASSIC_PROFILE_NEW && g.scale4 == 16 && g.trees == 3 && g.caves == CLASSIC_CAVES_2011 &&
              !g.flowers && g.clouds, "Play loads a Classic world with exactly the tab's settings", detail);
        if (!played) return 1;
        tick(60);
        if (!quit_to_menu()) return 1;
        tick(30);
        int at = -1;
        for (int i = 0; i < eden_menu_world_count(); ++i) if (std::strcmp(eden_menu_world_name(i), "Selftest Classic") == 0) at = i;
        const std::string file = at >= 0 ? std::string(eden_platform_documents_root()) + "/" + eden_menu_world_file(at) : std::string();
        const int n9 = at >= 0 ? g_world::classic_test::count_records(file, emod::T_GENERATOR) : -1;
        std::snprintf(detail, sizeof(detail), "listed at %d, %d GENERATOR record(s)", at, n9);
        check(at >= 0 && n9 == 1, "the Classic world's save carries its GENERATOR record", detail);
        if (at >= 0) {
            eden_menu_select(at);
            tick(2);
            ui_tap_rect(m->controlRect("delete"));
            tick(3);
            if (GLDialog::buttonRect(0, &b)) ui_tap_rect(b);
            tick(3);
        }
    }
    return 0;
}

// --convert-prompt-selftest (Stage S / S.5e): the "Convert world" prompt, through its real buttons.
// --docs holds a 64z `.eden` whose display name is --world=NAME; --world-format=emod;
// --convert-answer=upgrade|keep|cancel picks the button. The toggle and the prompt are forced on
// (EDEN_UPGRADE_256Z=1, EDEN_CONVERT_PROMPT unset), so this is the player's path, not a harness's.
// One answer per process: a converted source is paired and never asks again.
int run_convert_prompt_selftest() {
    g_selftestTag = "eden-convert";
    char d[320];
    const std::string ans = g_opt.convertAnswer;
    const int btn = ans == "upgrade" ? 0 : ans == "keep" ? 1 : ans == "cancel" ? 2 : -1;
    if (btn < 0) { check(false, "--convert-answer=upgrade|keep|cancel is given", ans.c_str()); return 1; }
    tick(5);
    int idx = -1;
    for (int i = 0; i < eden_menu_world_count(); ++i) if (g_opt.world == eden_menu_world_name(i)) idx = i;
    std::snprintf(d, sizeof(d), "\"%s\"", g_opt.world.c_str());
    check(idx >= 0, "the source world is listed", d);
    if (idx < 0) return 1;
    const std::string docs = eden_platform_documents_root();
    const std::string src = eden_menu_world_file(idx);
    auto emodCount = [&docs]() {
        int n = 0;
        for (int i = 0; i < eden_menu_world_count(); ++i) {
            const std::string f = eden_menu_world_file(i);
            n += f.size() > 5 && f.compare(f.size() - 5, 5, ".emod") == 0;
        }
        (void)docs;
        return n;
    };
    const int emod0 = emodCount();
    auto play = [&]() {
        eden_menu_select(idx);
        tick(2);
        return eden_menu_play() == 1 && tick_until([] { return GLDialog::active(); }, 600, "the convert prompt");
    };
    // The prompt, not "World converted" (which a conversion that never asked would show next): no
    // `.emod` beside the source yet, and no original waiting on that question.
    auto converted = [&]() {
        const std::string stem = src.substr(0, src.find_last_of('.'));
        FILE* f = std::fopen((docs + "/" + stem + ".emod").c_str(), "rb");
        if (f) std::fclose(f);
        return f != nullptr || !World::getWorld->fm->pendingOriginal().empty();
    };
    const bool asked = play() && !converted();
    std::snprintf(d, sizeof(d), "%s, game_mode %d, converted %d", src.c_str(), game_mode(), converted() ? 1 : 0);
    check(asked && game_mode() != 1, "Play on a 64z source asks before converting", d);
    if (!asked) return 1;
    CGRect b;
    if (GLDialog::buttonRect(btn, &b)) ui_tap_rect(b);
    if (btn == 2) {
        const bool back = tick_until([] { return World::getWorld->menu->loading == 0; }, 600, "loading back to 0");
        tick(30);
        World::getWorld->menu->loadWorlds();
        tick(2);
        std::snprintf(d, sizeof(d), "loading %d, game_mode %d, .emod %d -> %d", World::getWorld->menu->loading, game_mode(),
                      emod0, emodCount());
        check(back && game_mode() != 1 && !GLDialog::active() && emodCount() == emod0, "Cancel backs out to the menu, nothing converted", d);
        idx = -1;
        for (int i = 0; i < eden_menu_world_count(); ++i) if (src == eden_menu_world_file(i)) idx = i;
        const bool again = idx >= 0 && play() && !converted();
        check(again, "the next Play asks again", "");
        if (again && GLDialog::buttonRect(2, &b)) ui_tap_rect(b);
        tick_until([] { return World::getWorld->menu->loading == 0; }, 600, "loading back to 0");
        return 0;
    }
    const bool played = tick_until([] { return game_mode() == 1; }, 60000, "game_mode == PLAY");
    const std::string file = eden_debug_world_file();
    emod::EdenWorldStore st;
    const bool opened = st.open(file.c_str(), false);
    const int want = btn == 0 ? 256 : 64;
    std::snprintf(d, sizeof(d), "%s: g_world_height %d, bands %d", file.c_str(), g_world_height, opened ? st.bands() : -1);
    check(played && opened && g_world_height == want && st.bands() == want / 16,
          btn == 0 ? "Upgrade to 256 converts to a 16-band .emod and plays 256z" : "Keep 64 converts to a 4-band .emod and plays 64z", d);
    st.close();
    if (GLDialog::active()) { GLDialog::dismiss(); tick(2); }   // "World converted": keep the original
    return 0;
}

// Stage 5.7: the pickers kept stock's hit path; prove a tap on a cell still picks, through the
// HUD's own rects, and that the current choice is what a build would use.
extern "C" const int hudBlocksMap[NUM_BLOCKS+1];   // Classes/Hud.mm
int run_picker_selftest() {
    char detail[200];
    Hud* hud = World::getWorld->hud;
    tap_hud(1);
    tick(5);
    check(hud->mode == MODE_PICK_BLOCK, "E's HUD button opens the block picker", "");
    // Cells 2 and 3 are dark stone and stone in the stock table: plain blocks, no special pick.
    const CGRect c0 = hud->blockBounds[2];
    ui_tap_rect(c0);
    tick(5);
    std::snprintf(detail, sizeof(detail), "mode %d blocktype %d", hud->mode, hud->blocktype);
    check(hud->mode == MODE_BUILD, "tapping a block cell picks it and closes the picker", detail);
    const int picked = hud->blocktype;
    tap_hud(1);
    tick(5);
    ui_tap_rect(hud->blockBounds[3]);
    tick(5);
    std::snprintf(detail, sizeof(detail), "blocktype %d -> %d", picked, hud->blocktype);
    check(hud->mode == MODE_BUILD && hud->blocktype != picked, "a different cell picks a different block", detail);
    const int page1Pick = hud->blocktype;

    // --- D.2p: page 2 ------------------------------------------------------------------------
    tap_hud(1);
    tick(5);
    std::snprintf(detail, sizeof(detail), "page %d", hud->pickerPage);
    check(hud->pickerPage == 0, "the picker opens on page 1 when the current block is a page-1 one", detail);
    ui_tap_rect(hud->pickerNext);
    tick(5);
    std::snprintf(detail, sizeof(detail), "mode %d page %d", hud->mode, hud->pickerPage);
    check(hud->mode == MODE_PICK_BLOCK && hud->pickerPage == 1, "> turns to page 2 and keeps the picker open", detail);
    {
        int got[16], reopenedOn2 = 0;
        bool inOrder = true;
        for (int i = 0; i < 16; ++i) {
            ui_tap_rect(hud->blockBounds[i]);
            tick(5);
            got[i] = hud->mode == MODE_BUILD ? hud->blocktype : -1;
            inOrder &= got[i] == TYPE_ORE_SAND + i;
            tap_hud(1);                                    // reopen: lands on the current block's page
            tick(5);
            reopenedOn2 += hud->pickerPage == 1;
        }
        std::snprintf(detail, sizeof(detail), "first %d last %d, reopened on page 2: %d/16", got[0], got[15], reopenedOn2);
        check(inOrder, "page 2's cells pick 112..127 in id order", detail);
        check(reopenedOn2 == 16, "the picker reopens on page 2 while a page-2 block is current", detail);
    }
    {
        // D.3c / D.4b: SIGN and CMD are live where GL text entry exists (here).
        ui_tap_rect(hud->blockBounds[19]);                 // CMD
        tick(5);
        const bool cmdArmed = hud->mode == MODE_BUILD && hud->blocktype == HUD_TOOL_CMD;
        tap_hud(1);                                        // reopen: the tool's page
        tick(5);
        ui_tap_rect(hud->blockBounds[18]);                 // SIGN
        tick(5);
        const bool signArmed = hud->mode == MODE_BUILD && hud->blocktype == HUD_TOOL_SIGN;
        tap_hud(1);
        tick(5);
        std::snprintf(detail, sizeof(detail), "cells %d %d, CMD armed %d, SIGN armed %d, reopened on page %d",
                      Hud::pickerCell(1, 18), Hud::pickerCell(1, 19), cmdArmed, signArmed, hud->pickerPage);
        check(Hud::pickerCell(1, 16) == TYPE_GRASS3 && Hud::pickerCell(1, 17) == TYPE_GRASS2 && Hud::pickerCell(1, 18) == HUD_TOOL_SIGN && Hud::pickerCell(1, 19) == HUD_TOOL_CMD && cmdArmed &&
              signArmed && hud->mode == MODE_PICK_BLOCK && hud->pickerPage == 1,
              "the flower block, weeds, SIGN and CMD sit after 127 and each tool arms", detail);
    }
    ui_tap_rect(hud->pickerPrev);
    tick(5);
    const int afterPrev = hud->pickerPage;
    ui_tap_rect(hud->blockBounds[3]);
    tick(5);
    std::snprintf(detail, sizeof(detail), "page after < %d, blocktype %d (stock pick %d)", afterPrev, hud->blocktype, page1Pick);
    check(afterPrev == 0 && hud->mode == MODE_BUILD && hud->blocktype == page1Pick,
          "< returns to page 1 and its picks are unchanged", detail);

    // [ ] and the wheel, as real SDL events through the translator; < on page 1 wraps.
    tap_hud(1);
    tick(5);
    push_key(SDL_SCANCODE_RIGHTBRACKET, true); push_key(SDL_SCANCODE_RIGHTBRACKET, false);
    tick(3);
    const int afterKey = hud->pickerPage;
    push_key(SDL_SCANCODE_LEFTBRACKET, true); push_key(SDL_SCANCODE_LEFTBRACKET, false);
    tick(3);
    const int afterKey2 = hud->pickerPage;
    push_wheel(-1.0f);                                     // toward the user = next page
    tick(3);
    const int afterWheel = hud->pickerPage;
    push_wheel(1.0f);
    tick(3);
    const int afterWheel2 = hud->pickerPage;
    ui_tap_rect(hud->pickerPrev);
    tick(5);
    std::snprintf(detail, sizeof(detail), "] %d [ %d wheel %d/%d, < from page 1 %d",
                  afterKey, afterKey2, afterWheel, afterWheel2, hud->pickerPage);
    check(afterKey == 1 && afterKey2 == 0 && afterWheel == 1 && afterWheel2 == 0 && hud->pickerPage == 1,
          "] / [ and the wheel page the picker; < wraps", detail);
    tap_hud(1);                                            // close
    tick(5);

    // Block TNT's second pick: no page-2 cell has a BT variant, so all are veiled; SIGN/CMD too.
    {
        tap_hud(1);
        tick(5);
        if (hud->pickerPage != 0) { ui_tap_rect(hud->pickerNext); tick(5); }
        int tnt = -1;
        for (int i = 0; i < Hud::pickerCellCount(0); ++i) if (Hud::pickerCell(0, i) == TYPE_BLOCK_TNT) tnt = i;
        if (tnt >= 0) ui_tap_rect(hud->blockBounds[tnt]);
        tick(5);
        ui_tap_rect(hud->pickerNext);
        tick(5);
        int bt = 0;
        for (int i = 0; i < 16; ++i) bt += hudBlocksMap[Hud::pickerCell(1, i)] != -1;
        ui_tap_rect(hud->blockBounds[18]);                 // SIGN mid-pick: nothing
        tick(5);
        const int modeAfterSign = hud->mode;
        ui_tap_rect(hud->blockBounds[0]);                  // 112: the stock fallback, a plain block
        tick(5);
        std::snprintf(detail, sizeof(detail), "tnt cell %d, page-2 BT variants %d, mode after SIGN %d, then %d blocktype %d",
                      tnt, bt, modeAfterSign, hud->mode, hud->blocktype);
        check(tnt >= 0 && bt == 0 && modeAfterSign == MODE_PICK_BLOCK && hud->mode == MODE_BUILD &&
              hud->blocktype == TYPE_ORE_SAND,
              "Block TNT's second pick survives a page turn and veils every page-2 cell", detail);
    }

    tap_hud(2);
    tick(5);
    check(hud->mode == MODE_PICK_COLOR, "C's HUD button opens the colour picker", "");
    ui_tap_rect(hud->colorBounds[10]);
    tick(5);
    std::snprintf(detail, sizeof(detail), "mode %d paintColor %d", hud->mode, (int)hud->paintColor);
    check(hud->mode == MODE_PAINT && (int)hud->paintColor == 11, "tapping swatch 10 paints colour 11", detail);
    return 0;
}

int run_ui_selftest() {
    g_selftestTag = "eden-ui";
    g_tickInput = true;            // headless pump_events() only drains SDL's queue with this set
    char detail[240];
    tick(120);
    check(eden_settings_loaded() != 0, "the settings model loaded", "");

    // --- the slider's arithmetic, on its own --------------------------------------------------
    {
        GLW::Slider sl;
        sl.setRect(CGRectMake(100, 100, 200, 40));
        sl.setRange(0.25f, 3.0f, 0.05f);
        sl.setValue(1.0f);
        const bool moved = sl.beginDrag(100.0f);
        const float lo = sl.value();
        const bool again = sl.dragTo(90.0f);           // already pinned at min: no change
        sl.dragTo(300.0f);
        const float hi = sl.value();
        sl.dragTo(173.0f);
        const float mid = sl.value();
        sl.endDrag();
        const float steps = (mid - 0.25f) / 0.05f;
        std::snprintf(detail, sizeof(detail), "lo %.3f hi %.3f mid %.4f (%.3f steps) moved=%d again=%d",
                      lo, hi, mid, steps, moved, again);
        check(moved && !again && std::fabs(lo - 0.25f) < 1e-4f && std::fabs(hi - 3.0f) < 1e-4f &&
              std::fabs(steps - std::floor(steps + 0.5f)) < 1e-3f,
              "Slider pins to its ends, snaps to min+k*step, reports only real changes", detail);
    }

    // --- U.1: the kit's new arithmetic (variable-height ScrollView, IconRail, sizeToFit) -------
    {
        GLW::ScrollView sv;
        const float hs[] = { 50, 50, 120, 30, 30, 30 };   // 310 tall in a 100 view
        sv.setRowHeights(hs, 6, 100.0f);
        const int v0 = sv.visible();                       // 50+50 fit, the 120 does not
        sv.ensureVisible(2);                               // the tall row: alone, overflowing
        const int f2 = sv.first(), v2 = sv.visible();
        sv.setFirst(99);                                   // clamps to the last row that fills
        const int fmax = sv.first(), vmax = sv.visible();
        sv.setFirst(0);
        sv.beginContentDrag(0.0f);
        sv.contentDragTo(76.0f);                           // > 50 + 25: two rows on
        const int fd = sv.first();
        sv.endDrag();
        const float off = (sv.setFirst(1), sv.rowOffset(3));   // 50 + 120
        sv.setRows(10, 4);                                 // back to uniform
        const bool uni = sv.visible() == 4 && sv.total() == 10;
        std::snprintf(detail, sizeof(detail), "v0 %d first/visible at 2: %d/%d max %d/%d drag %d off %.0f uniform %d",
                      v0, f2, v2, fmax, vmax, fd, off, uni);
        check(v0 == 2 && f2 == 2 && v2 == 1 && fmax == 3 && vmax == 3 && fd == 2 && off == 170.0f && uni,
              "ScrollView variable row heights: visible/first/clamp/drag/rowOffset", detail);

        GLW::IconRail rail;
        const int icons[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 };
        rail.setTiles(icons, 10);
        const float tw = GLW::IconRail::designW();
        rail.setRect(CGRectMake(0, 0, tw, GLW::du(4 * 2 + 64 * 3 + 6 * 2)));   // exactly 3 tiles
        rail.setEnabled(1, false);
        const CGRect t0 = rail.tileRect(0), t3 = rail.tileRect(3);
        const int h0 = rail.hit(t0.origin.x + 2, t0.origin.y + 2);
        const CGRect t1 = rail.tileRect(1);
        const int h1 = rail.hit(t1.origin.x + 2, t1.origin.y + 2);   // disabled: inert
        const bool moved = rail.scrollBy(100);
        const int rf = rail.first();
        std::snprintf(detail, sizeof(detail), "visible %d tile0 %.0fx%.0f hidden3 %.0f hit0 %d hit1 %d scrolled %d first %d",
                      rail.visibleTiles(), t0.size.width, t0.size.height, t3.size.width, h0, h1, moved, rf);
        check(rail.visibleTiles() == 3 && t0.size.width > 0 && t3.size.width == 0 && h0 == 0 && h1 == -1 &&
              moved && rf == 7, "IconRail: whole tiles, disabled tiles inert, scrolls to the last page", detail);

        GLW::Button a, b;
        a.setLabel("Ok", GLW::du(20));   a.sizeToFit();
        b.setLabel("A much longer action label", GLW::du(20)); b.sizeToFit();
        a.setRect(CGRectMake(10, 10, 500, 29));          // the width is ignored once fitted
        std::snprintf(detail, sizeof(detail), "short %.1f (min %.1f) long %.1f rect %.1f",
                      a.fitWidth(), GLW::du(64), b.fitWidth(), a.rect().size.width);
        check(std::fabs(a.fitWidth() - GLW::du(64)) < 0.01f && b.fitWidth() > GLW::du(64) &&
              a.rect().size.width == a.fitWidth(), "Button::sizeToFit shrink-wraps with a 64u floor", detail);
    }

    // --- N.5.11: the toast's lifetime, and the HUD-style statusbar routed through it ----------
    {
        GLW::Toast t;
        t.show("Can't place a sign under the block", 2.0f);
        const bool shown = t.visible() && t.rect().size.width > 0.0f;
        const float w1 = t.rect().size.width;
        t.update(1.5f);
        t.show("Can't place a sign under the block", 2.0f);   // same text: lifetime reset only
        t.update(1.5f);
        const bool refreshed = t.visible();
        t.show("World Saved", 2.0f);
        const float w2 = t.rect().size.width;                 // shrink-wrapped: shorter text, narrower
        t.update(2.5f);
        const bool expired = !t.visible() && t.text().empty();
        t.show("Loading", 1000.0f);
        t.update(5000.0f);
        const bool sticky = t.visible();
        t.clear();
        statusbar sb(CGRectMake(0, 0, 0, 0));
        sb.useToast();
        sb.setStatus("World Saved", 1.0f);
        const bool sbShown = sb.current() != nullptr;
        sb.update(2.0f);
        const bool sbGone = sb.current() == nullptr;
        sb.setStatus("World Saved", 1.0f);                  // same text after expiry re-shows
        const bool sbAgain = sb.current() != nullptr;
        std::snprintf(detail, sizeof(detail), "shown=%d w %.1f>%.1f refreshed=%d expired=%d sticky=%d "
                      "sb %d/%d/%d", shown, w1, w2, refreshed, expired, sticky, sbShown, sbGone, sbAgain);
        check(shown && w1 > w2 && w2 > 0.0f && refreshed && expired && sticky && sbShown && sbGone &&
              sbAgain, "Toast shows, refreshes on the same text, expires, honours >=1000 s, "
              "shrink-wraps; statusbar::useToast() routes through it", detail);
    }

    // --- the settings screen, through real pointer events -------------------------------------
    eden_settings_menu_open_now();
    tick(10);
    SettingsMenu* sm = (World::getWorld && World::getWorld->menu) ? World::getWorld->menu->settings : nullptr;
    if (!sm) { check(false, "the settings screen exists", ""); return 1; }
    {   // U.3: seven tabs, every settings row on exactly one of them
        int rows = 0, shown = 0;
        for (int t = 0; t < sm->tabCount(); ++t) rows += sm->tabRowCount(t);
        for (int i = 0; i < eden_settings_count(); ++i) if (!eden_settings_native_hidden(i)) shown++;
        std::snprintf(detail, sizeof(detail), "%d tabs, %d rows on them, %d rows shown", sm->tabCount(), rows, shown);
        check(sm->tabCount() == 7 && rows == shown && sm->tabRowCount(3) == 0,
              "settings has 7 tabs and every native row is on one", detail);
    }
    {   // U.4: the Keys tab. A key button arms a capture, any tap cancels it, Defaults resets.
        // Every binding is saved first and put back after: Defaults resets them ALL.
        std::vector<int> saved;
        for (int i = 0; i < eden_keybind_count(); ++i) saved.push_back(eden_keybind_get(i));
        const int mi = eden_keybind_index("blockPicker");
        CGRect kr;
        int armed = -1, cancelled = -1, rebound = 0, restored = 0;
        const bool found = mi >= 0 && sm->keyRect(mi, &kr);
        if (found) {
            tick(2);
            ui_tap_rect(kr);
            armed = eden_keybind_capture_active();
            ui_tap_rect(kr);
            cancelled = eden_keybind_capture_active();
            eden_keybind_set(mi, (int)SDL_SCANCODE_T);
            rebound = eden_keybind_get(mi);
            ui_tap_rect(sm->defaultsRect());
            restored = eden_keybind_get(mi);
        }
        std::snprintf(detail, sizeof(detail), "found=%d armed=%d cancelled=%d rebound=%d restored=%d (default %d)",
                      (int)found, armed, cancelled, rebound, restored, found ? eden_keybind_default(mi) : -1);
        check(found && armed == mi && cancelled < 0 && rebound == (int)SDL_SCANCODE_T &&
              restored == eden_keybind_default(mi),
              "Keys tab: a key button arms a capture, a tap cancels, Defaults resets", detail);
        for (int i = 0; i < (int)saved.size(); ++i) eden_keybind_set(i, saved[i]);
        sm->resetView();
        tick(2);
    }

    const int iHealth = settings_index("health");
    const int iVol    = settings_index("music_volume");
    // The first ENUM row this build shows (input_mode, the obvious one, is web-only).
    const char* modeKey = nullptr;
    for (int i = 0; i < eden_settings_count() && !modeKey; ++i)
        if (eden_settings_kind(i) == 2 && sm->showRow(eden_settings_key(i), nullptr)) modeKey = eden_settings_key(i);
    const int iMode   = modeKey ? settings_index(modeKey) : -1;
    const float health0 = eden_settings_get(iHealth), vol0 = eden_settings_get(iVol);
    const float mode0 = iMode >= 0 ? eden_settings_get(iMode) : 0.0f;
    CGRect r;

    {   // B.8: presets drive view_distance (+ render_scale off iOS); a hand edit drops to Custom
        const int iP = settings_index("graphics_preset"), iV = settings_index("view_distance"),
                  iD = settings_index("render_radius");     // B.5b-2, X.17
        const float p0 = iP >= 0 ? eden_settings_get(iP) : 0.0f, v0 = iV >= 0 ? eden_settings_get(iV) : 0.0f,
                    d0 = iD >= 0 ? eden_settings_get(iD) : 0.0f;
#if defined(EDEN_PLATFORM_IOS)
        const float highD = 6.0f;                              // radius 10 (P.4 decision 6)
#else
        const float highD = 8.0f;                              // radius 16 (kRenderRadius[8])
#endif
        bool ok = iP >= 0 && iV >= 0 && iD >= 0;
        if (ok) {
            eden_settings_set(iP, 1.0f);                       // Low
            ok = eden_settings_get(iV) == 1.0f && eden_settings_get(iD) == 5.0f && eden_settings_get(iP) == 1.0f;
            eden_settings_set(iP, 3.0f);                       // High: Auto view, the larger window
            ok = ok && eden_settings_get(iV) == 0.0f && eden_settings_get(iD) == highD && eden_settings_get(iP) == 3.0f;
            eden_settings_set(iV, 3.0f);                       // by hand
            ok = ok && eden_settings_get(iP) == 0.0f;          // -> Custom
            eden_settings_set(iP, 2.0f);                       // Default restores Auto and radius 9
            ok = ok && eden_settings_get(iV) == 0.0f && eden_settings_get(iD) == 5.0f;
            eden_settings_set(iD, 2.0f);                       // the radius by hand -> Custom too
            ok = ok && eden_settings_get(iP) == 0.0f;
            eden_settings_set(iV, v0); eden_settings_set(iD, d0); eden_settings_set(iP, p0);
        }
        check(ok, "graphics presets set their knobs; a hand edit becomes Custom", "");
    }

    if (sm->showRow("health", &r)) {
        tick(2);
        ui_tap(r.origin.x + r.size.width * 0.5f, r.origin.y + r.size.height * 0.5f);
        const float after = eden_settings_get(iHealth);
        ui_tap(r.origin.x + r.size.width * 0.5f, r.origin.y + r.size.height * 0.5f);
        std::snprintf(detail, sizeof(detail), "%.0f -> %.0f -> %.0f", health0, after, eden_settings_get(iHealth));
        check(after != health0 && eden_settings_get(iHealth) == health0, "tapping a Toggle flips its setting, twice restores", detail);
    } else check(false, "the health row is shown", "");

    if (sm->showRow("music_volume", &r)) {
        tick(2);
        const float cy = r.origin.y + r.size.height * 0.5f;
        ui_pointer(0, r.origin.x + 1.0f, cy); tick(3);
        const float atLeft = eden_settings_get(iVol);
        ui_pointer(1, r.origin.x + r.size.width - 1.0f, cy + 60.0f); tick(3);   // off the track, vertically
        const float atRight = eden_settings_get(iVol);
        ui_pointer(1, r.origin.x + r.size.width * 0.5f, cy); tick(3);
        const float atMid = eden_settings_get(iVol);
        ui_pointer(2, r.origin.x + r.size.width * 0.5f, cy); tick(3);
        std::snprintf(detail, sizeof(detail), "left %.2f right %.2f mid %.2f (was %.2f)", atLeft, atRight, atMid, vol0);
        check(atLeft == 0.0f && atRight == 1.0f && std::fabs(atMid - 0.5f) <= 0.051f,
              "dragging a Slider writes the setting live, and keeps tracking off the track", detail);
    } else check(false, "the music volume row is shown", "");

    if (modeKey && sm->showRow(modeKey, &r)) {
        tick(2);
        const float h = r.size.height;    // the stepper's buttons are square at its height
        ui_tap(r.origin.x + r.size.width - h * 0.5f, r.origin.y + h * 0.5f);
        const int n = eden_settings_enum_count(iMode);
        const int want = ((int)mode0 + 1) % (n > 0 ? n : 1);
        std::snprintf(detail, sizeof(detail), "%s: %.0f -> %.0f of %d", modeKey, mode0, eden_settings_get(iMode), n);
        check((int)eden_settings_get(iMode) == want, "a Stepper's > steps an enum row", detail);
    } else check(false, "an enum row is shown", "");

    eden_settings_set(iHealth, health0);
    eden_settings_set(iVol, vol0);
    if (iMode >= 0) eden_settings_set(iMode, mode0);
    eden_settings_menu_close();
    tick(10);

    if (run_menu_selftest() != 0) return 1;

    // --- the text field: typing, UTF-8 backspace, the byte cap, Return, Escape -----------------
    {
        static const char* const kBtn[] = { "OK", "Cancel" };
        check(GLDialog::textEntryAvailable(), "native reports GL text entry", "");
        g_promptChosen = -2;
        GLDialog::prompt("Selftest", NULL, "abc", 8, kBtn, 2, ui_prompt_cb);
        tick(2);
        check(eden_text_input_active() == 1, "focusing the field starts platform text input", "");
        push_text("d\xC3\xA9");             // "dé": é is two bytes
        tick(2);
        push_tap_key(SDL_SCANCODE_BACKSPACE);
        tick(2);
        push_text("xyz12345");
        push_tap_key(SDL_SCANCODE_W);       // must reach the field's queue (nothing), not the game
        tick(2);
        push_tap_key(SDL_SCANCODE_RETURN);
        tick(3);
        std::snprintf(detail, sizeof(detail), "chosen %d text \"%s\" active %d dialog %d",
                      g_promptChosen, g_promptText.c_str(), eden_text_input_active(), GLDialog::active() ? 1 : 0);
        check(g_promptChosen == 0 && g_promptText == "abcdxyz1" && eden_text_input_active() == 0 && !GLDialog::active(),
              "type + backspace (a whole UTF-8 char) + 8-byte cap + Return commits", detail);

        push_text("late");
        tick(2);
        char sink[16];
        check(eden_text_input_take(sink, sizeof(sink)) == 0, "text typed with no field focused is dropped", "");

        g_promptChosen = -2;
        GLDialog::prompt("Selftest", NULL, "keep", 8, kBtn, 2, ui_prompt_cb);
        tick(2);
        push_text("zz");
        push_tap_key(SDL_SCANCODE_ESCAPE);
        tick(3);
        std::snprintf(detail, sizeof(detail), "chosen %d text \"%s\" in_menu %d", g_promptChosen,
                      g_promptText.c_str(), eden_hud_in_menu());
        check(g_promptChosen == 1 && !GLDialog::active(), "Escape picks the LAST button (Cancel) and closes", detail);
    }

    // --- rename a world that exists on disk -------------------------------------------------
    // Play it once so there is a file (a created-but-never-played world has none), go back to the
    // menu, rename through the real button -> screen -> save path, then check the file: ONLY
    // WorldFileHeader::name may differ, and the world must still load under its new name.
    // H.15: the button is Properties (World Properties, Classes/WorldPropertiesMenu.h), whose World
    // tab holds the name; a name-only change saves with no dialog. (N.4.5 opened a prompt here.)
    const char* name = g_opt.world.empty() ? "ui-selftest" : g_opt.world.c_str();
    if (!open_world(name, g_opt.height)) return 1;
    tick(60);
    if (!quit_to_menu()) return 1;
    tick(30);
    check(!World::getWorld->menu->showhome, "Quit to Menu lands on My Worlds, not the home screen", "");   // I.7
    int idx = -1;
    for (int i = 0; i < eden_menu_world_count(); ++i)
        if (std::strcmp(eden_menu_world_name(i), name) == 0) idx = i;
    check(idx >= 0, "the played world is listed", name);
    if (idx < 0) return 1;
    eden_menu_select(idx);
    tick(5);
    const std::string file = std::string(eden_platform_documents_root()) + "/" + eden_menu_world_file(idx);
    std::vector<unsigned char> before, after;
    check(read_file(file, &before) && before.size() > sizeof(WorldFileHeader), "the world has a file", file.c_str());

    Menu* menu = World::getWorld->menu;
    const bool offered = menu->propertiesOffered();
    check(offered, "the menu offers Properties for the selected world (Rename's slot)", "");
    ui_tap_rect(menu->controlRect("properties"));
    tick(3);
    WorldPropertiesMenu* wp = menu->props;
    std::snprintf(detail, sizeof(detail), "showprops %d open %d dialog %d save %d", menu->showprops ? 1 : 0,
                  wp->isOpen() ? 1 : 0, GLDialog::active() ? 1 : 0, wp->saveEnabled() ? 1 : 0);
    check(menu->showprops && wp->isOpen() && !GLDialog::active() && !wp->saveEnabled(),
          "Properties opens World Properties (no prompt), Save disabled until something changes", detail);
    std::snprintf(detail, sizeof(detail), "name \"%s\" height \"%s\"", wp->text("name"), wp->value("height"));
    check(std::strcmp(wp->text("name"), name) == 0 && wp->rowVisible("photo") && !wp->rowVisible("seed"),
          "the World tab shows the photo row and the name, and no seed for a Normal world", detail);
    ui_tap_rect(wp->tabRect(WorldPropertiesMenu::TAB_TERRAIN));
    tick(2);
    std::snprintf(detail, sizeof(detail), "tab %d type \"%s\" hills %d", wp->tab(), wp->value("type"),
                  wp->rowVisible("hills") ? 1 : 0);
    check(wp->tab() == WorldPropertiesMenu::TAB_TERRAIN && wp->rowVisible("type") && !wp->rowEditable("type") &&
          !wp->rowVisible("hills"), "a Normal world's Terrain tab is its type only", detail);
    ui_tap_rect(wp->tabRect(WorldPropertiesMenu::TAB_WORLD));
    tick(2);
    ui_tap_rect(wp->controlRect("name"));
    tick(2);
    check(wp->focused("name") && eden_text_input_active() == 1, "tapping the name field focuses it", "");
    for (int i = 0; i < 64; ++i) push_tap_key(SDL_SCANCODE_BACKSPACE);
    tick(2);
    push_text("Renamed \xC3\x9C");          // "Renamed Ü"
    tick(2);
    check(wp->saveEnabled() && !wp->terrainChanged(), "a new name enables Save (a name-only change)", "");
    push_tap_key(SDL_SCANCODE_RETURN);       // Return in the field is Save
    tick(5);
    std::snprintf(detail, sizeof(detail), "open %d dialog %d result %d", wp->isOpen() ? 1 : 0,
                  GLDialog::active() ? 1 : 0, wp->lastResult());
    check(!wp->isOpen() && !menu->showprops && !GLDialog::active() && wp->lastResult() == WP_WROTE_NAME,
          "a name-only save raises no dialog and closes the screen", detail);
    const char* want = "Renamed \xC3\x9C";
    const std::string listed = eden_menu_world_name(idx);   // copy: one static buffer per accessor family
    std::string onDisk = World::getWorld->fm->getName(eden_menu_world_file(idx));
    std::snprintf(detail, sizeof(detail), "listed \"%s\" on disk \"%s\"", listed.c_str(), onDisk.c_str());
    check(listed == want && onDisk == want, "the rename reaches the list and the file header", detail);

    read_file(file, &after);
    // S.8: the world is an `.emod` -- append-only, so a rename appends a new WORLD_HEADER record and no
    // byte already written moves (the `.eden` form rewrote the 50 name bytes in place).
    size_t diffPrefix = 0;
    for (size_t i = 0; i < before.size() && i < after.size(); ++i)
        if (before[i] != after[i]) diffPrefix++;
    std::snprintf(detail, sizeof(detail), "size %zu -> %zu, %zu earlier bytes changed",
                  before.size(), after.size(), diffPrefix);
    check(after.size() > before.size() && diffPrefix == 0,
          "the rename only appended to the file", detail);
    {
        // H.15: Escape over the screen is Back (discards).
        ui_tap_rect(menu->controlRect("properties"));
        tick(3);
        const bool opened = menu->showprops;
        push_tap_key(SDL_SCANCODE_ESCAPE);
        tick(3);
        std::vector<unsigned char> again;
        read_file(file, &again);
        std::snprintf(detail, sizeof(detail), "opened %d showprops %d bytes %zu -> %zu", opened ? 1 : 0,
                      menu->showprops ? 1 : 0, after.size(), again.size());
        check(opened && !menu->showprops && again.size() == after.size(), "Escape closes Properties and writes nothing", detail);
    }

    const bool reopened = open_world(want, g_opt.height);
    check(reopened, "the renamed world loads under its new name", "");

    // --- Stage 5.5: the kit pause menu --------------------------------------------------------
    // Real taps at the rects the Hud laid out: Settings opens the GL SettingsMenu as an in-game
    // modal, its own Back returns to the pause menu (not to gameplay, not to the title screen),
    // and Resume closes the menu.
    if (reopened) {
        Hud* hud = World::getWorld->hud;
        tick(30);
        if (!eden_hud_in_menu()) tap_hud(0);
        tick(10);
        check(eden_hud_in_menu() == 1, "Escape's HUD button opens the pause menu", "");
        const Button rs = hud->rsettings;
        ui_tap(rs.origin.x + rs.size.width * 0.5f, rs.origin.y + rs.size.height * 0.5f);
        tick(5);
        std::snprintf(detail, sizeof(detail), "showsettings=%d game_mode=%d", (int)menu->showsettings,
                      World::getWorld->game_mode);
        check(menu->showsettings && World::getWorld->game_mode == GAME_MODE_PLAY,
              "pause menu -> Settings opens the settings screen over the world", detail);
        const CGRect bk = menu->settings->backRect();
        ui_tap(bk.origin.x + bk.size.width * 0.5f, bk.origin.y + bk.size.height * 0.5f);
        tick(5);
        std::snprintf(detail, sizeof(detail), "showsettings=%d in_menu=%d", (int)menu->showsettings,
                      eden_hud_in_menu());
        check(!menu->showsettings && eden_hud_in_menu() == 1, "Settings' Back returns to the pause menu", detail);
        // U.3: Escape over in-game Settings is its Back (it used to be swallowed). U.4: on the Keys
        // tab with a capture armed, the first Escape only cancels the capture.
        ui_tap(rs.origin.x + rs.size.width * 0.5f, rs.origin.y + rs.size.height * 0.5f);
        tick(5);
        const int miPick = eden_keybind_index("blockPicker");
        const int pick0 = eden_keybind_get(miPick);
        CGRect kr;
        if (menu->settings->keyRect(miPick, &kr)) ui_tap_rect(kr);
        tick(2);
        const int armed = eden_keybind_capture_active();
        push_tap_key(SDL_SCANCODE_ESCAPE);
        tick(5);
        const bool stillSettings = menu->showsettings && eden_keybind_capture_active() < 0 &&
                                   eden_keybind_get(miPick) == pick0;
        push_tap_key(SDL_SCANCODE_ESCAPE);
        tick(5);
        std::snprintf(detail, sizeof(detail), "armed=%d; after 1st Esc settings+uncaptured=%d; after 2nd "
                      "showsettings=%d in_menu=%d", armed, (int)stillSettings, (int)menu->showsettings,
                      eden_hud_in_menu());
        check(armed == miPick && stillSettings && !menu->showsettings && eden_hud_in_menu() == 1,
              "Escape steps back capture -> Settings -> the pause menu", detail);
        // --- C.3: the Console over the running world --------------------------------------------
        {
            ConsoleMenu* cm = menu->console;
            const Button* pm[7] = { &hud->rresume, &hud->rsave, &hud->rhome, &hud->rcam, &hud->rsettings,
                                    &hud->rconsole, &hud->rexit };
            bool allIn = true;
            for (const Button* b : pm)
                if (b->size.width <= 0 || b->origin.x < 0 || b->origin.y < 0 ||
                    b->origin.x + b->size.width > SCREEN_WIDTH || b->origin.y + b->size.height > SCREEN_HEIGHT) allIn = false;
            std::snprintf(detail, sizeof(detail), "screen %.0fx%.0f console button y=%.0f h=%.0f", SCREEN_WIDTH, SCREEN_HEIGHT,
                          hud->rconsole.origin.y, hud->rconsole.size.height);
            check(allIn, "the pause menu's seven buttons all fit on the screen", detail);
            const Button rc = hud->rconsole;
            ui_tap(rc.origin.x + rc.size.width * 0.5f, rc.origin.y + rc.size.height * 0.5f);
            tick(5);
            std::snprintf(detail, sizeof(detail), "showconsole=%d open=%d in_menu=%d game_mode=%d", (int)menu->showconsole,
                          cm->isOpen() ? 1 : 0, eden_hud_in_menu(), World::getWorld->game_mode);
            check(menu->showconsole && cm->isOpen() && eden_hud_in_menu() == 0 && World::getWorld->game_mode == GAME_MODE_PLAY,
                  "pause menu -> Console opens it over the world and closes the pause menu", detail);
            check(cm->inputFocused() && eden_text_input_active() == 1, "the Console's input is focused on open", "");
            check(eden_ui_wants_cursor() == 1, "the Console releases the mouse", "");

            push_text("echo hello world");
            tick(2);
            push_tap_key(SDL_SCANCODE_RETURN);
            tick(3);
            bool echoed = false, printed = false;
            for (const Console::Line& l : Console::scrollback()) {
                if (l.kind == Console::ECHO && l.text == "> echo hello world") echoed = true;
                if (l.kind != Console::ECHO && l.text == "hello world") printed = true;
            }
            check(echoed && printed && cm->input().empty() && cm->inputFocused(),
                  "Return runs the line: echoed, answered, the field cleared and still focused", "");
            check(cm->visibleLines() >= 2 && cm->following(), "the scrollback shows both lines and follows the bottom", "");

            ui_tap_rect(cm->controlRect("up"));
            tick(3);
            check(cm->input() == "echo hello world", "Up recalls the last line", cm->input().c_str());
            ui_tap_rect(cm->controlRect("down"));
            tick(3);
            check(cm->input().empty(), "Down returns to the (empty) draft", cm->input().c_str());

            cm->setInput("vid");
            ui_tap_rect(cm->controlRect("tab"));
            tick(3);
            check(cm->input().compare(0, 6, "video.") == 0, "Tab completes a variable to its group", cm->input().c_str());
            cm->setInput("");

            ui_tap_rect(cm->tabRect(ConsoleMenu::TAB_VARIABLES));
            tick(5);
            std::snprintf(detail, sizeof(detail), "tab %d rows %d (cvars %zu)", cm->tab(), cm->varRows(), Console::cvarCount());
            check(cm->tab() == ConsoleMenu::TAB_VARIABLES && cm->varRows() == (int)Console::cvarCount() && cm->varRows() > 40,
                  "the Variables tab lists every variable", detail);
            // Every cvar's name pointer still names it (C.3 found them dangling once: a vector of records
            // with std::string members moved them, and the short names live inside the string object).
            int badName = 0;
            for (size_t i = 0; i < Console::cvarCount(); ++i)
                if (Console::cvarIndex(Console::cvarAt(i)->name) != (int)i) badName++;
            check(badName == 0, "every variable's name pointer is stable", "");
            ui_tap_rect(cm->tabRect(ConsoleMenu::TAB_HELP));
            tick(5);
            std::snprintf(detail, sizeof(detail), "tab %d rows %d", cm->tab(), cm->helpRows());
            check(cm->tab() == ConsoleMenu::TAB_HELP && cm->helpRows() >= 30, "the Help tab lists the verbs", detail);
            ui_tap_rect(cm->tabRect(ConsoleMenu::TAB_CONSOLE));
            tick(5);
            check(cm->tab() == ConsoleMenu::TAB_CONSOLE && cm->inputFocused(), "back on the Console tab the input is focused again", "");

            // The strip: collapse, run a line from it, expand.
            ui_tap_rect(cm->controlRect("collapse"));
            tick(5);
            check(cm->collapsed(), "Collapse docks the strip", "");
            const CGRect sp = cm->controlRect("panel");
            check(sp.origin.y + sp.size.height > SCREEN_HEIGHT * 0.6f, "the strip sits at the top so the world shows below", "");
            cm->setInput("echo from the strip");
            ui_tap_rect(cm->controlRect("run"));
            tick(3);
            bool strip = false;
            for (const Console::Line& l : Console::scrollback()) if (l.text == "from the strip") strip = true;
            check(strip && cm->visibleLines() >= 1, "Run works from the strip", "");
            ui_tap_rect(cm->controlRect("collapse"));
            tick(3);
            check(!cm->collapsed(), "Expand restores the full panel", "");

            // FT.6: a keep-visible panel taller than the room above a keyboard is clamped around the field, so
            // iOS's slide-up (area bottom to keyboard top) cannot lift the field off the screen.
            {
                const float H = SCREEN_HEIGHT, fh = 34.0f, fy = H * 0.7f;       // a field near the top
                float ky = H * 0.05f, kh = H * 0.9f;                            // a panel nearly the screen
                eden_text_input_clamp_keep(fy, fh, &ky, &kh);
                std::snprintf(detail, sizeof(detail), "field %.0f..%.0f keep %.0f..%.0f cap %.0f", fy, fy + fh, ky, ky + kh, H * 0.38f);
                check(kh <= H * 0.38f + 0.5f && ky <= fy + 0.5f && ky + kh >= fy + fh - 0.5f,
                      "FT.6: a tall keep-visible panel is clamped and still holds the field", detail);
                float ky2 = fy - 20.0f, kh2 = fh + 40.0f;
                eden_text_input_clamp_keep(fy, fh, &ky2, &kh2);
                check(ky2 == fy - 20.0f && kh2 == fh + 40.0f, "FT.6: a small keep-visible rect is left alone", "");
            }

            // FT.5: on touch the full layout's titlebar hit boxes (touch-floor tall) must not outlive it, or
            // the strip's Menu (and Back, same code) are dead while Run works. Touch forced for this leg only.
            {
                const int iIn = settings_index("input_mode");
                const float inPrev = iIn >= 0 ? eden_settings_get(iIn) : 0.0f;
                eden_test_force_touch_input();
                tick(3);                                            // the full layout, touch hit boxes set
                ui_tap_rect(cm->controlRect("collapse"));
                tick(3);
                check(cm->collapsed(), "touch: Collapse docks the strip", "");
                ui_tap_rect(cm->controlRect("collapse"));           // Menu, in the strip
                tick(3);
                check(!cm->collapsed(), "touch: the strip's Menu works after the full layout", "");
                if (iIn >= 0) eden_settings_set(iIn, inPrev);
                tick(3);
            }

            // C.4: a hardware keyboard's Up / Down / Tab do what the buttons do.
            push_tap_key(SDL_SCANCODE_UP);
            tick(3);
            check(cm->input() == "echo from the strip", "the Up key recalls the last line", cm->input().c_str());
            push_tap_key(SDL_SCANCODE_DOWN);
            tick(3);
            check(cm->input().empty(), "the Down key returns to the draft", cm->input().c_str());
            cm->setInput("vid");
            push_tap_key(SDL_SCANCODE_TAB);
            tick(3);
            check(cm->input().compare(0, 6, "video.") == 0, "the Tab key completes", cm->input().c_str());
            cm->setInput("");

            // C.4: the on-screen keyboard (faked through the seam). A low keyboard: the full layout moves
            // above it without the rail; a tall one (iPhone landscape) or an unknown one: the auto strip;
            // gone: the full layout with the rail. The input is never below the keyboard's top.
            const float kbLow = SCREEN_HEIGHT * 0.3f, kbTall = SCREEN_HEIGHT * 0.62f;
            eden_native_debug_set_keyboard_top(kbLow);
            tick(3);
            CGRect in = cm->controlRect("input"), pn = cm->controlRect("panel");
            std::snprintf(detail, sizeof(detail), "kb %.0f input y %.0f panel y %.0f auto %d rail %d lines %d", kbLow,
                          in.origin.y, pn.origin.y, cm->autoStrip(), cm->railShown(), cm->visibleLines());
            check(!cm->autoStrip() && !cm->railShown() && in.origin.y >= kbLow && pn.origin.y >= kbLow && cm->visibleLines() >= 2,
                  "a keyboard: the full layout sits above it, without the rail", detail);
            eden_native_debug_set_keyboard_top(kbTall);
            tick(3);
            in = cm->controlRect("input"); pn = cm->controlRect("panel");
            std::snprintf(detail, sizeof(detail), "kb %.0f input y %.0f panel y %.0f auto %d lines %d", kbTall,
                          in.origin.y, pn.origin.y, cm->autoStrip(), cm->visibleLines());
            check(cm->autoStrip() && !cm->collapsed() && in.origin.y >= kbTall && pn.origin.y >= kbTall && cm->visibleLines() >= 1,
                  "a tall keyboard: the auto strip, above it", detail);
            push_text("echo over the keyboard");
            tick(2);
            push_tap_key(SDL_SCANCODE_RETURN);
            tick(3);
            bool overKb = false;
            for (const Console::Line& l : Console::scrollback()) if (l.text == "over the keyboard") overKb = true;
            check(overKb, "typing and Return work in the auto strip", "");
            eden_native_debug_set_keyboard_top(-1.0f);
            tick(3);
            check(cm->autoStrip(), "a keyboard of unknown height: the auto strip (top-docked)", "");
            eden_native_debug_set_keyboard_top(0.0f);
            tick(3);
            check(!cm->autoStrip() && cm->railShown() && cm->controlRect("panel").origin.y < kbLow,
                  "the keyboard gone: the full layout and the rail are back", "");
            eden_native_debug_set_keyboard_top(-2.0f);

            // Escape: the focused field takes the first (blur), the second closes. Blurred, the keys are
            // still the Console's: typing must never walk.
            push_tap_key(SDL_SCANCODE_ESCAPE);
            tick(3);
            check(cm->isOpen() && !cm->inputFocused() && eden_ui_captures_keys() == 1, "Escape once blurs the field and keeps the Console open", "");
            push_tap_key(SDL_SCANCODE_ESCAPE);
            tick(3);
            check(!menu->showconsole && !cm->isOpen() && eden_hud_in_menu() == 0, "Escape twice closes it, back to play", "");

            // The `console` key (Backquote): opens from play; typed into a focused field; closes once the
            // field is blurred.
            push_tap_key(SDL_SCANCODE_GRAVE);
            tick(3);
            check(menu->showconsole && cm->isOpen() && cm->inputFocused() && cm->input().empty(), "the console key opens it, input focused and empty", "");
            push_tap_key(SDL_SCANCODE_GRAVE);
            tick(3);
            check(cm->isOpen(), "a focused field owns the console key", "");
            push_tap_key(SDL_SCANCODE_ESCAPE);
            tick(3);
            push_tap_key(SDL_SCANCODE_GRAVE);
            tick(3);
            check(!cm->isOpen(), "the console key closes it while the field is blurred", "");
            ui_tap_rect(cm->controlRect("back"));   // closed: a no-op, must not crash
            tick(2);

            // Back closes it too.
            eden_console_menu_open();
            tick(3);
            ui_tap_rect(cm->controlRect("back"));
            tick(3);
            check(!cm->isOpen() && !menu->showconsole, "Back closes the Console", "");
            tap_hud(0);                      // the pause menu again, for Resume below
            tick(10);
        }
        const Button rr = hud->rresume;
        ui_tap(rr.origin.x + rr.size.width * 0.5f, rr.origin.y + rr.size.height * 0.5f);
        tick(5);
        check(eden_hud_in_menu() == 0, "Resume closes the pause menu", "");
        run_picker_selftest();
        quit_to_menu();
    }

    std::printf("[eden-ui] %s (%d failure(s))\n", g_selftestFailures ? "FAILURES" : "ALL PASS", g_selftestFailures);
    return g_selftestFailures ? 1 : 0;
}

}  // namespace g_ui
}  // namespace harness
