//
//  ModMenuScreen.cpp
//  Emod -- K.7-3a: see ModMenuScreen.h. The shape is the Console's (a railFrame panel over a running world, one
//  touch claim, World::update's input slot) with the K.7-2b settings page's RowList for the rows.
//
#include "ModMenuScreen.h"
#include "ModScript.h"
#include "Globals.h"
#include "Graphics.h"
#include "Input.h"
#include "World.h"
#include "Menu.h"
#include "ConsoleMenu.h"
#include "UIIcons.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

using namespace GLW;

namespace ModMenuScreen {
namespace {

const int usage_id = 23;   // distinct from Menu (7), SettingsMenu (3), GLDialog (9), Player (10), Keybinds (11), WorldBrowser (12), NewWorld (13), WorldProperties (14), Hud (15, 16), Console (17)

struct State {
    bool open = false;
    int serial = 0, tab = 0, builtTab = -1, builtSerial = 0;
    ModMenu::Spec spec;
    RowList rl;
    IconRail rail;
    TabRail tabs;
    GLW::Button back;
    GLW::Label title, footer;
    std::string shownTitle, shownFooter;
    float labelsU = 0, titleW = -1;
    bool railOn = false, tabsOn = false;
    CGRect panel, content, bar;
    float titleY = 0;
    int touchBack = -1, touchRail = -1, touchTabs = -1;
    State() { panel = content = bar = CGRectMake(0, 0, 0, 0); }
};
State* g_st = NULL;
State& st() { if (!g_st) g_st = new State(); return *g_st; }

int stepDecimals(double step) {
    int dec = 0;
    double m = 1;
    while (dec < 3 && std::fabs(step * m - std::floor(step * m + 0.5)) > 1e-6) { ++dec; m *= 10; }
    return dec;
}
std::string sliderText(const ModMenu::Row& r) {
    char b[48];
    std::snprintf(b, sizeof b, "%.*f", stepDecimals(r.step), r.value);
    return b;
}

RowList::Row kitRow(const ModMenu::Row& m) {
    RowList::Row r;
    r.title = m.label;
    r.hint = m.hint;
    r.enabled = m.enabled;
    switch (m.type) {
    case ModMenu::R_HEADING: r.kind = RowList::HEADING; break;
    case ModMenu::R_TEXT:    r.kind = RowList::PARAGRAPH; break;
    case ModMenu::R_ROW:     r.kind = RowList::ITEM; r.detail = m.detail; r.icon = m.icon; break;
    case ModMenu::R_BUTTON:  r.kind = RowList::BUTTON; r.text = m.action; r.danger = m.danger; break;
    case ModMenu::R_TOGGLE:  r.kind = RowList::TOGGLE; r.on = m.on; break;
    case ModMenu::R_SLIDER:  r.kind = RowList::SLIDER; r.min = (float)m.min; r.max = (float)m.max; r.step = (float)m.step; r.value = (float)m.value; break;
    case ModMenu::R_CHOICE:  r.kind = RowList::CHOICE; r.options = m.options; r.choice = m.choice; break;
    }
    return r;
}

void buildRows() {
    State& s = st();
    std::vector<RowList::Row> rows;
    const std::vector<ModMenu::Row>& src = s.spec.tabs[(size_t)s.tab].rows;
    for (const ModMenu::Row& m : src) rows.push_back(kitRow(m));
    s.rl.setRows(rows);
    for (size_t i = 0; i < src.size(); ++i)
        if (src[i].type == ModMenu::R_SLIDER) s.rl.setSliderReadout((int)i, sliderText(src[i]));
        else if (src[i].type == ModMenu::R_BUTTON) s.rl.setTextLabel((int)i, src[i].action);
    s.builtTab = s.tab;
    s.builtSerial = s.serial;
}

void buildTabs() {
    State& s = st();
    const int n = (int)s.spec.tabs.size();
    bool icons = n >= 2;
    for (const ModMenu::Tab& t : s.spec.tabs) icons = icons && t.icon >= 0;
    s.railOn = icons;
    s.tabsOn = n >= 2 && !icons;
    if (s.railOn) {
        std::vector<int> ids;
        for (const ModMenu::Tab& t : s.spec.tabs) ids.push_back(t.icon);
        s.rail.setTiles(&ids[0], n);
    }
    if (s.tabsOn) {
        const char* labels[GLW_MAX_TABS];
        for (int i = 0; i < n && i < GLW_MAX_TABS; ++i) labels[i] = s.spec.tabs[(size_t)i].label.c_str();
        s.tabs.setTabs(labels, std::min(n, (int)GLW_MAX_TABS), du(18));
    }
}

void layout() {
    State& s = st();
    if (s.labelsU != u()) {
        s.labelsU = u();
        s.back.setLabel("Back", du(20));
        s.back.sizeToFit();
        s.shownTitle.clear();
        s.shownFooter.clear();
        buildTabs();   // (TabRail's labels are rasters at the scale)
    }
    const RailFrame f = railFrame();
    s.panel = f.panel;
    const CGRect& T = f.titlebar;
    s.back.setRect(CGRectMake(T.origin.x, T.origin.y, 0, T.size.height));
    titlebarHit(s.back);
    const std::string foot = s.spec.pack.empty() ? std::string() : "Mod: " + s.spec.pack;   // K.7-3b: the pause menu's Mods page is the game's own
    if (foot != s.shownFooter) { s.shownFooter = foot; s.footer.set(foot.c_str(), du(15), UITextAlignmentLeft, 0.0f, FACE_BODY); }
    const float side = std::max(s.back.fitWidth(), s.footer.width()) + du(8);
    const float tw = std::max((float)du(60), (float)T.size.width - 2.0f * side);
    if (s.spec.title != s.shownTitle || tw != s.titleW) {
        s.shownTitle = s.spec.title;
        s.titleW = tw;
        s.title.set(s.spec.title.c_str(), du(26), UITextAlignmentCenter, tw, FACE_DISPLAY);
    }
    s.titleY = f.titleY(s.title);

    CGRect C = f.content;
    if (s.railOn) {
        s.rail.setRect(f.rail);
        s.rail.setSelected(s.tab);
    } else {
        const float right = C.origin.x + C.size.width;
        C.origin.x = f.rail.origin.x;
        C.size.width = right - C.origin.x;
        if (s.tabsOn) {
            const float h = controlH(29);
            s.tabs.setRect(CGRectMake(C.origin.x, C.origin.y + C.size.height - h, C.size.width, h));
            s.tabs.setSelected(s.tab);
            C.size.height -= h + du(6);
        }
    }
    s.content = C;
    s.bar = CGRectMake(f.bar.origin.x, C.origin.y, f.bar.size.width, C.size.height);
    if (s.builtTab != s.tab || s.builtSerial != s.serial) buildRows();
    s.rl.layout(s.content, s.bar);
}

void selectTab(int t) {
    State& s = st();
    if (t < 0 || t >= (int)s.spec.tabs.size() || t == s.tab) return;
    s.tab = t;
    layout();
}

// What the player changed this frame, one report per row (a slider dragged across steps reports its last).
void drainChanges() {
    State& s = st();
    std::vector<int> rows;
    int k;
    RowList::Event ev;
    while (s.rl.takeChange(&k, &ev))
        if (std::find(rows.begin(), rows.end(), k) == rows.end()) rows.push_back(k);
    std::vector<ModMenu::Row>& src = s.spec.tabs[(size_t)s.tab].rows;
    for (int r : rows) {
        if (r < 0 || r >= (int)src.size()) continue;
        ModMenu::Row& m = src[(size_t)r];
        const RowList::Row& kr = s.rl.row(r);
        if (m.type == ModMenu::R_TOGGLE) m.on = kr.on;
        else if (m.type == ModMenu::R_SLIDER) { m.value = kr.value; s.rl.setSliderReadout(r, sliderText(m)); }
        else if (m.type == ModMenu::R_CHOICE) m.choice = kr.choice;
        else if (m.type != ModMenu::R_ROW && m.type != ModMenu::R_BUTTON) continue;
        if (s.serial == ModScript::MODS_PAGE) ModScript::modsPageInput(s.tab, r, m);   // K.7-3b
        else ModScript::menuInput(s.serial, s.tab, r, m);
        if (!s.open) return;
    }
}

}  // namespace

void open(int serialNo, const ModMenu::Spec& spec, int tab) {
    State& s = st();
    World* w = World::getWorld;
    if (w && w->menu && w->menu->showconsole && w->menu->console) w->menu->console->close();   // one screen owns the input
    s.open = true;
    s.serial = serialNo;
    s.spec = spec;
    s.tab = tab >= 0 && tab < (int)spec.tabs.size() ? tab : 0;
    s.builtTab = -1;
    s.labelsU = 0;            // relabel + rebuild the tabs for this spec
    s.touchBack = s.touchRail = s.touchTabs = -1;
    s.back.setPressed(false);
    s.rail.setHeld(-1);
    s.tabs.setHeld(-1);
    layout();
}

void close() {
    State& s = st();
    if (!s.open) return;
    s.open = false;
    s.serial = 0;
    s.touchBack = s.touchRail = s.touchTabs = -1;
    if (Input::getInput()) Input::getInput()->clearAll();   // a live touch must not land on the HUD
}

bool isOpen() { return g_st && g_st->open; }
int serial() { return isOpen() ? g_st->serial : 0; }
int tab() { return isOpen() ? g_st->tab : 0; }

void setRow(int t, int row, const ModMenu::Row& r) {
    State& s = st();
    if (!s.open || t < 0 || t >= (int)s.spec.tabs.size() || row < 0 || row >= (int)s.spec.tabs[(size_t)t].rows.size()) return;
    s.spec.tabs[(size_t)t].rows[(size_t)row] = r;
    if (t != s.tab || s.builtTab != s.tab) return;   // another tab: built from the spec when it shows
    s.rl.setRow(row, kitRow(r));
    if (r.type == ModMenu::R_SLIDER) s.rl.setSliderReadout(row, sliderText(r));
}

void showTab(int t) { if (isOpen()) selectTab(t); }

void back() {
    if (!isOpen()) return;
    const int was = g_st->serial;
    close();
    ModScript::menuClosed(was);
}

void update(float etime) {
    (void)etime;
    if (!isOpen()) return;
    State& s = st();
    layout();
    if (int wheel = eden_ui_take_wheel()) {
        float px = 0, py = 0;
        if (s.railOn && eden_ui_pointer(&px, &py) && s.rail.hitRail(px, py)) s.rail.scrollBy(-wheel);
        else s.rl.scrollBy(-wheel);
    }
    Input* input = Input::getInput();
    itouch* touches = input->getTouches();
    for (int i = 0; i < MAX_TOUCHES; i++) {
        itouch& t = touches[i];
        if (t.inuse == 0 && t.down == M_DOWN) {
            // Every touch is this screen's while it is open: nothing reaches the HUD, the joystick or the player.
            t.inuse = usage_id;
            const float mx = t.mx, my = t.my;
            if (s.back.hit(mx, my)) { s.back.setPressed(true); s.touchBack = i; }
            else if (s.railOn && s.touchRail < 0 && s.rail.hitRail(mx, my)) { s.touchRail = i; s.rail.setHeld(s.rail.hit(mx, my)); s.rail.beginDrag(my); }
            else if (s.tabsOn && s.touchTabs < 0 && s.tabs.hit(mx, my) >= 0) { s.touchTabs = i; s.tabs.setHeld(s.tabs.hit(mx, my)); }
            else s.rl.touchDown(i, mx, my);
        }
        if (t.inuse == usage_id && t.down == M_DOWN) {
            if (i == s.touchRail) { if (s.rail.dragTo(t.my)) s.rail.setHeld(-1); }
            else if (i == s.touchTabs) s.tabs.setHeld(s.tabs.hit(t.mx, t.my));
            else if (i != s.touchBack) s.rl.touchHeld(i, t.mx, t.my);
        }
        if (t.inuse == usage_id && t.down == M_RELEASE) {
            const float mx = t.mx, my = t.my;
            t.inuse = 0;
            t.down = M_NONE;
            if (i == s.touchBack) {
                s.touchBack = -1;
                s.back.setPressed(false);
                if (s.back.hit(mx, my)) { back(); return; }
                continue;
            }
            if (i == s.touchRail) {
                const bool dragged = s.rail.scrolling();
                s.rail.endDrag(); s.rail.setHeld(-1); s.touchRail = -1;
                if (!dragged) selectTab(s.rail.hit(mx, my));
                continue;
            }
            if (i == s.touchTabs) {
                s.touchTabs = -1; s.tabs.setHeld(-1);
                selectTab(s.tabs.hit(mx, my));
                continue;
            }
            s.rl.touchUp(i, mx, my);
        }
    }
    drainChanges();   // (a slider held across frames reports here each frame it moved)
}

void render() {
    if (!isOpen()) return;
    State& s = st();
    layout();
    begin2D();
    Focus::screen(g_st, NULL, true);   // UX.7: Tab walks the rows in order, Return activates; none of it on touch
    fill(CGRectMake(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT), kScrim);
    bevel(s.panel, BEVEL_WINDOW);
    s.back.render();
    s.title.drawCentered(s.panel.origin.x + s.panel.size.width * 0.5f, s.titleY, kText);
    {
        const CGRect T = railFrame().titlebar;
        s.footer.draw(T.origin.x + T.size.width - s.footer.width(), T.origin.y + (T.size.height + s.footer.lineHeight()) * 0.5f, kTextSecondary);
    }
    if (s.railOn) s.rail.render();
    if (s.tabsOn) s.tabs.render();
    s.rl.render();
    end2D();
}

CGRect controlRect(const char* which) {
    if (!isOpen()) return CGRectMake(0, 0, 0, 0);
    layout();
    State& s = st();
    if (!std::strcmp(which, "back")) return s.back.rect();
    if (!std::strcmp(which, "panel")) return s.panel;
    if (!std::strcmp(which, "content")) return s.content;
    return CGRectMake(0, 0, 0, 0);
}
CGRect tabRect(int t) {
    if (!isOpen()) return CGRectMake(0, 0, 0, 0);
    layout();
    State& s = st();
    if (s.railOn) return s.rail.tileRect(t);
    if (s.tabsOn) return s.tabs.tabRect(t);
    return CGRectMake(0, 0, 0, 0);
}
int rowCount() { return isOpen() ? st().rl.count() : 0; }
bool rowControlRect(int row, CGRect* r) { if (!isOpen()) return false; layout(); return st().rl.controlRect(row, r); }
void showRow(int row) { if (!isOpen()) return; layout(); st().rl.showRow(row); layout(); }
std::string rowTitle(int row) {
    if (!isOpen() || row < 0 || row >= st().rl.count()) return "";
    const RowList::Row& r = st().rl.row(row);
    return r.title;
}
bool railShown() { return isOpen() && st().railOn; }
bool tabsShown() { return isOpen() && st().tabsOn; }

}  // namespace ModMenuScreen
