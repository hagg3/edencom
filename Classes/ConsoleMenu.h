//
//  ConsoleMenu.h
//  Eden
//
//  Stage C / C.3 (WORKING/dev-console-plan.md §4; docs/console.md, docs/ui.md): the player-facing
//  Console screen over Console:: (Classes/Console.h) -- a line typed here is a command-block script.
//
//  FULL layout: the shared GLW::railFrame() panel. Titlebar Back · "Console" · [collapse]; rail tiles
//    Console   (UIICON_TERMINAL): the scrollback (a row-snapped, variable-height ScrollView of body-face
//              Labels, 200 lines, follows the bottom unless the player scrolled up), a suggestion/tool
//              strip (completion matches, ⇥ ▲ ▼, the anchor) and the input (a 2-line GLW::TextField + Run);
//    Variables (UIICON_SETTINGS): every cvar, name · value over its hint; a tap puts `name ` in the input;
//    Help      (UIICON_INFO): the block verbs, then the console verbs; a tap puts `verb ` in the input.
//  STRIP layout (collapsed): the last 2-4 lines over the input, docked to the TOP edge so the world shows.
//
//  C.4, the on-screen keyboard (eden_text_input_keyboard_top()): while it is up over the Console tab,
//  the panel spans the screen top to the keyboard's top, without the rail, so the input is the last
//  thing above the keyboard and SDL's own view shift has nothing to do. If that would show fewer than
//  4 scrollback lines (an iPhone in landscape), or the keyboard's height is not known yet, the strip
//  is used instead -- the AUTO strip, which is not the player's collapse and goes when the keyboard
//  does. A hardware keyboard's Tab / Up / Down reach complete() / historyStep() via the field.
//
//  The world RUNS behind the screen (Menu::showconsole is not Menu::showsettings): World::update calls
//  update() first -- a submitted line executes here, in the input phase, like a build tap -- and the
//  play step then runs with no player input, because this screen claims every touch. Mouse capture is
//  released while it is open (eden_ui_wants_cursor), and the keys are the focused field's.
//
//  Closing returns to PLAY (the pause menu's Console button closes the pause menu; the `console` key
//  opens from play). Escape: the first press blurs the focused field (TextField's rule), the next closes.
//
#ifndef Eden_ConsoleMenu_h
#define Eden_ConsoleMenu_h

#import "GLWidgets.h"
#import "Console.h"
#include <map>
#include <string>
#include <vector>

class ConsoleMenu {
public:
    ConsoleMenu();
    ~ConsoleMenu();

    void open();                         // from play; focuses the input
    void openChat(const char* prefill);  // MP.7: T, `/` and the touch button: the Chat tab with `prefill` typed
    void close();
    bool isOpen() const       { return m_open; }
    void back();                         // Escape: blur the field, else close
    void toggle();                       // the `console` key: opens, or closes while the field is not focused
    void update(float etime);            // World::update, FIRST in the frame
    void render();                       // after the HUD, in the HUD's ortho

    // MP.7: in a multiplayer session two tiles join in FRONT of the rail (Chat, Players); the ids append.
    enum { TAB_CONSOLE = 0, TAB_VARIABLES = 1, TAB_HELP = 2, TAB_CHAT = 3, TAB_PLAYERS = 4, TABS = 5 };
    int  tab() const          { return m_tab; }
    void showTab(int t);
    bool collapsed() const    { return m_collapsed; }
    void setCollapsed(bool c);           // remembered for the session

    // The input line. submit() is what Run and Return do: echo, run, clear, scroll to the bottom.
    const std::string& input() const { return m_field.text(); }
    void setInput(const char* text);
    void submit();
    void complete();                     // ⇥
    void historyStep(int dir);           // ▲ (-1: older) / ▼ (+1: newer)

    // --- harness surface (--ui-selftest / --shot): point space, y up ---------------------------------
    // controlRect: "back", "collapse", "input", "run", "tab" (⇥), "up", "down", "scrollback", "panel".
    CGRect controlRect(const char* which);
    CGRect tabRect(int t);
    bool   inputFocused() const;
    void   blurInput();
    int    visibleLines() const;         // scrollback rows drawn this frame (Console tab / strip)
    bool   following() const { return m_follow; }
    bool   autoStrip() const { return m_autoStrip; }   // C.4: strip because of the keyboard
    bool   railShown() const { return !m_autoStrip && !m_collapsed && !m_noRail; }
    int    helpRows() const;
    int    varRows() const;
    // MP.7 harness: the rail's tiles left to right (Chat, Players, Console, Variables, Help in a session).
    int    railTabs() const   { return m_railN; }
    int    playerRows() const;
    bool   chatTab() const    { return m_tab==TAB_CHAT; }
    bool   inputTab() const   { return m_tab==TAB_CONSOLE||m_tab==TAB_CHAT; }   // the tabs with the scrollback + input

private:
    ConsoleMenu(const ConsoleMenu&);
    ConsoleMenu& operator=(const ConsoleMenu&);

    struct Cached;                       // one scrollback line's label, keyed by its serial
    struct InfoRow;

    void layout();
    GLW::RailFrame frame(float kbTop);   // railFrame(), or the keyboard-aware one (C.4)
    bool strip() const        { return m_collapsed || m_autoStrip; }
    void buildLabels();
    void syncScrollback();
    void buildInfoRows(int tab);
    void pickRow(int tab, int index);
    void drawScrollback(CGRect area);
    void drawStrip();
    bool lineBackToField(int index);
    int  hitButton(float x, float y) const;
    void scrolled();                     // the player moved the scrollback: re-derive "following"
    // MP.7: the lines the scrollback tab shows (Console's, or the chat's) and the rail's tile <-> tab map.
    const std::vector<Console::Line>& srcLines() const;
    unsigned srcSerial() const;
    int  tabOfTile(int tile) const;
    int  tileOfTab(int tab) const;
    void syncRail();                     // the rail has the session tiles while MpWorld::active()
    void configureField();               // placeholder, byte cap and code mode for the shown tab
    void fillPlayerRows();

    bool m_open;
    bool m_collapsed;
    int  m_tab;
    int  m_railN;                        // MP.7: tiles on the rail (3, or 5 in a session)
    bool m_railSession;
    int  m_srcTab;                       // which tab's lines the label cache holds
    int  m_fieldTab;                     // the tab the field was last configured for
    bool m_fieldCode;
    int  m_titleTab;
    std::string m_otherText;             // the other tab's draft (Console <-> Chat share the one field)
    bool m_follow;                       // the scrollback sticks to the bottom
    bool m_newBelow;                     // lines arrived while scrolled up
    unsigned m_lastSerial;
    float m_builtU;                      // u() and text width the scrollback labels were built at
    float m_builtW;
    float m_labelsU;                     // u() the chrome labels were built at (and their state)
    bool  m_labelsCollapsed;
    std::string m_builtMatches;
    float m_builtMatchW;
    int  m_histPos;                      // -1: editing; else an index into the history
    std::string m_draft;                 // what was typed before ▲ started
    std::string m_matches;               // the last completion's candidates, one wrapped line
    std::string m_anchorText;

    GLW::Label      m_title;
    GLW::Label      m_anchor, m_matchLabel;
    GLW::Button     m_back, m_collapse, m_tabsChip, m_run, m_tabBtn, m_up, m_down, m_newPill;
    GLW::IconRail   m_rail;
    GLW::ScrollView m_scroll;            // the Console tab's scrollback
    GLW::ScrollView m_infoScroll;        // Variables / Help
    GLW::TextField  m_field;

    std::map<unsigned, Cached*> m_cache; // serial -> label
    std::map<unsigned, Cached*> m_cacheOther;   // Q.1 F25: the other source tab's labels, swapped in on a tab switch
    float m_builtUOther, m_builtWOther;         // ...and the u()/width they were built at
    std::vector<unsigned> m_visSerials;  // the serials drawn, top row first (tests)
    std::vector<InfoRow*> m_info[TABS];  // the Variables / Help tabs' rows, built on first show

    CGRect m_panel, m_content, m_bar, m_scrollArea, m_stripRect, m_inputRect, m_toolsRect;
    float  m_titleY;
    int    m_touchScroll, m_touchRail;
    bool   m_barDrag;
    int    m_downButton;                 // 0 none, else the button a touch went down on (Button enum in .mm)
    int    m_stripLines;                 // lines the strip shows
    bool   m_autoStrip;                  // C.4: strip forced by the keyboard (not the player's collapse)
    bool   m_noRail;                     // C.4: the full layout above a keyboard has no rail
};

#endif
