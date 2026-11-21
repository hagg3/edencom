//
//  ModMenuScreen.h
//  Emod -- K.7-3a: the screen a mod script's eden.menu{...} opens (docs/ui.md "Script menus", docs/scripting.md).
//
//  WHAT IT DRAWS. The shared GLW::railFrame() panel: titlebar Back · the menu's title · a muted "Mod: <pack>" at
//  the right (a script cannot pass itself off as the game); then the menu's tabs: one tab = no rail, the content
//  takes the rail's width; 2..8 tabs = the IconRail when every tab has an icon, else a TabRail across the top of
//  the content. A tab's rows are a GLW::RowList (the K.7-2b settings page's kit list): heading, text paragraph,
//  row (icon + label + detail, the whole row taps), button, toggle, slider, choice. Nothing here takes a
//  coordinate, size, colour or font from the script.
//
//  THE WORLD RUNS (plan V12 (a), the Console's model): World::update calls update() in the Console's slot, before
//  terrain->update, and the play step then runs with no player input because this screen claims every touch and
//  (eden_ui_captures_keys) every key but the menu key; the sky and weather clocks stop as under the pause menu.
//  A dialog raised by a row's action freezes the world as any GLDialog does and draws over this screen.
//
//  NO LUA HERE. The spec is plain C++ (ModMenu::Spec, validated by ModScript); what the player does is reported
//  to ModScript::menuInput / menuClosed, which queue it, and the row's on_action runs in step 2 of the same
//  frame's ModScript::update (rule 7: an edit it makes is meshed that frame).
//
//  Back, Escape (Menu::back) and the menu key close it (on_close runs once). Keys: UX.7's focus model (Tab walks the
//  rows in order, Return activates, arrows move and change a slider/choice); on touch no ring and no tint.
//
#ifndef Eden_ModMenuScreen_h
#define Eden_ModMenuScreen_h

#include "GLWidgets.h"
#include <string>
#include <vector>

namespace ModMenu {
enum RowType { R_HEADING = 0, R_TEXT, R_ROW, R_BUTTON, R_TOGGLE, R_SLIDER, R_CHOICE };
struct Row {
    int type = R_HEADING;
    std::string id, label, hint, detail, action;
    int icon = -1;                          // UIIcons.h id (R_ROW), -1 = none
    bool danger = false, enabled = true;
    bool on = false;                        // R_TOGGLE
    double min = 0, max = 1, step = 1, value = 0;   // R_SLIDER
    std::vector<std::string> options;       // R_CHOICE
    int choice = 0;                         // R_CHOICE, 0-based (Lua sees 1-based)
};
struct Tab { std::string id, label; int icon = -1; std::vector<Row> rows; };
struct Spec { std::string id, title, pack; std::vector<Tab> tabs; };
}

namespace ModMenuScreen {

// ModScript's side. open() closes the Console screen (the menu claims the input); close() runs no callback.
void open(int serial, const ModMenu::Spec& spec, int tab);
void close();
bool isOpen();
int  serial();                       // the open menu's (0 = none)
void setRow(int tab, int row, const ModMenu::Row& r);
void showTab(int tab);
int  tab();

// The engine's side.
void back();                         // Back / Escape / the menu key: closes and reports it (on_close)
void update(float etime);            // World::update, after the Console's slot
void render();                       // after the HUD (and the Console), in the HUD's ortho

// Harness (--lua-selftest / --shot / --fit-selftest): point space, y up.
CGRect controlRect(const char* which);   // "back", "panel", "content"
CGRect tabRect(int t);                   // the tab's rail tile or TabRail tile; zero-size with one tab
int    rowCount();                       // the shown tab's
bool   rowControlRect(int row, CGRect* r);   // the row's control (the row for heading/text/row); false = off screen
void   showRow(int row);
std::string rowTitle(int row);           // what the shown tab's row says now (m:set's effect)
bool   railShown();                      // IconRail
bool   tabsShown();                      // TabRail

}  // namespace ModMenuScreen

#endif
