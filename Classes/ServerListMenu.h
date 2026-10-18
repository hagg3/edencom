//
//  ServerListMenu.h
//  Eden
//
//  MP.6 (WORKING/multiplayer-ui-plan-2026-10-17.md §1, §2, §5-§7): the Multiplayer screen, opened by
//  the home screen's Multiplayer tile. A full screen over the menu background like Get Worlds:
//  Back · "Multiplayer" · Add, the saved servers as two-line rows (label; MOTD, else host:port, else
//  the status word; players, ping bars + ms, a lock), a detail pane where the screen is tall enough,
//  and an action bar "Playing as Name" · Edit · Refresh · Join.
//
//  It also owns the join's UI: the password prompt (GLDialog::prompt, secure), the Connecting window
//  (stage line, bar, the welcome/MOTD lines, Cancel) and the one error dialog (Retry · Edit/Save
//  server · Back). A session's Exit lands back here (Menu::activate). Data: Classes/MpServers.h.
//
#ifndef Eden_ServerListMenu_h
#define Eden_ServerListMenu_h

#import "GLWidgets.h"
#import "MpServers.h"
#include <string>
#include <vector>

class ServerFormMenu;

#define SL_MAX_VISIBLE 16

class ServerListMenu {
public:
    ServerListMenu();
    ~ServerListMenu();

    void open();              // reads servers.txt, probes every row
    void close();
    bool isOpen() const       { return m_open; }
    bool back();              // Escape: the form / Connecting / the list, in that order; 1 = consumed
    void update(float etime);
    void render();

    // The form's close (ServerFormMenu::close).
    void formClosed(int outcome, int row, bool thenJoin);

    // --- harness surface (--ui-selftest / --shot): point space, y up ---------------------------
    // controlRect: "back", "add", "edit", "refresh", "join", "profile", "emptyadd", "list",
    // "cancel" (Connecting). rowRect: a server's row (false when scrolled out of view).
    CGRect controlRect(const char* which);
    bool   rowRect(int index, CGRect* r);
    int    selected() const   { return m_sel; }
    // MP.8: 0 = My servers, 1 = Matchmakers (exists only while Experiments > Matchmakers has addresses).
    int    tab() const        { return m_tab; }
    void   showTab(int t);
    bool   tabsShown() const  { return m_mmOn; }
    void   select(int i);
    bool   paneShown() const  { return m_pane; }
    bool   connecting() const { return m_connecting; }
    int    probesPending() const { return m_pending; }
    ServerFormMenu* form()    { return m_form; }
    // --shot: fake the Connecting window at a stage (no socket), or show a canned error.
    void   debugConnecting(int stage, float progress, const char* const* lines, int n);
    void   debugError(int err, const char* text);
    void   debugPassword(bool again);
    void   join(int row);     // a tap on the selected row / Join

    // The join in flight (for the prompt, the error dialog and Retry).
    struct Pending {
        int row;              // -1 = a direct connect (Join now)
        std::string host, label, password;
        int port;
        bool remember;        // Join and remember / the form's toggle (direct: nothing to save to)
        int  locked = -1;     // what the list said (-1 unknown, 0 open, 1 password); the matchmaker's too
    };

private:
    ServerListMenu(const ServerListMenu&);
    ServerListMenu& operator=(const ServerListMenu&);

    void layout();
    void buildRow(int slot, int index);
    void buildPane();
    void startJoin(const Pending& p);
    void continueJoin();      // after the profile / the password prompt
    void promptPassword(bool again);
    void showError(int err, const char* text);
    void pollJoin();
    void renderConnecting();
    std::string displayName(const EdenMpServerRow& r) const;
    int  count() const;       // rows on the shown tab

    static void passwordCb(int chosen, const char* text);
    static void errorCb(int chosen);

    bool m_open;
    ServerFormMenu* m_form;

    std::vector<EdenMpServerRow> m_rows;
    int  m_sel, m_pending;
    int  m_tab, m_mmPending, m_tabHeld;   // MP.8
    bool m_mmOn;
    std::string m_mmEmptyKey;
    bool m_pane, m_built;
    float m_builtU;

    GLW::Label  m_title, m_empty, m_profileCap, m_emptyMm;
    GLW::TabRail m_tabs;
    GLW::Button m_back, m_add, m_edit, m_refresh, m_join, m_profile, m_emptyAdd, m_save;
    GLW::ScrollView m_scroll;
    CGRect m_panel, m_list, m_paneRect, m_barRect;
    float  m_titleY, m_rowH, m_phase;
    std::string m_profileName;

    struct RowUi {
        GLW::Label title, sub, players, ping, q;
        std::string key;      // what it was built from
        CGRect rect;
    };
    RowUi m_rowUi[SL_MAX_VISIBLE];
    GLW::Label m_paneLines[8];
    std::string m_paneKey;
    int m_paneN;

    int  m_listTouch, m_barTouch, m_downRow;

    // The join.
    Pending m_join_;
    bool m_connecting, m_debugConn;
    EdenMpJoinState m_js;
    GLW::Label m_cTitle, m_cStage, m_cLines[8];
    std::string m_cTitleKey, m_cStageKey, m_cLineKey[8];
    int m_cN;
    GLW::Button m_cancel;
    CGRect m_cPanel;
    bool m_lockLearnt;
};

#endif
