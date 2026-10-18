//
//  ServerFormMenu.h
//  Eden
//
//  MP.6 (WORKING/multiplayer-ui-plan-2026-10-17.md §3, §4): the Add / Edit server form and the
//  Profile form, one class in two modes, opened from the server list (ServerListMenu). The panel is
//  GLW::railFrame()'s with no rail: TwoLineRows on the white content surface, a titlebar Back · title
//  · Save, and (Add/Edit) a foot of Delete · Move up · Move down · Join now.
//
//  Add/Edit write servers.txt through the MpServers.h seam. Join now joins with the typed values and
//  saves nothing (list plan §6: direct connect). Profile stores mp_name / mp_character.
//
//  Keyboard avoidance (§3.2, the Console's rule): when the on-screen keyboard reaches into the
//  panel, the rows are laid out from just above it, the focused one the lowest visible, and the foot
//  hides until the keyboard goes. Save stays in the titlebar, so it is always reachable.
//
#ifndef Eden_ServerFormMenu_h
#define Eden_ServerFormMenu_h

#import "GLWidgets.h"
#include <string>

class ServerListMenu;

class ServerFormMenu {
public:
    enum Mode { MODE_ADD, MODE_EDIT, MODE_PROFILE };
    // What closed the form, for the list (ServerListMenu::formClosed).
    enum Outcome { OUT_BACK, OUT_SAVED, OUT_DELETED, OUT_JOIN_NOW, OUT_PROFILE_SAVED };

    explicit ServerFormMenu(ServerListMenu* list);
    ~ServerFormMenu();

    void openAdd();
    void openEdit(int row);
    void openProfile(bool thenJoin);   // thenJoin: the first Join opened it; Save continues the join
    void close(Outcome o);
    bool isOpen() const      { return m_open; }
    Mode mode() const        { return m_mode; }
    bool back();             // Escape: 1 = consumed (closes, discarding)
    void update(float etime);
    void render();

    // Join now's values (valid after OUT_JOIN_NOW).
    std::string joinHost, joinPassword, joinLabel;
    int joinPort;
    bool joinRemember;

    // --- harness surface: point space, y up -----------------------------------------------------
    // controlRect: "back", "save", "label", "address", "password", "remember", "delete", "up",
    // "down", "joinnow", "name", "character.dec", "character.inc".
    CGRect controlRect(const char* which);
    bool   saveEnabled() const;
    const char* hint(const char* which) const;   // "address" / "name": the validation hint shown
    GLW::TextField* field(const char* which);
    bool   footShown() const { return m_footShown; }
    void   blurFields();

private:
    ServerFormMenu(const ServerFormMenu&);
    ServerFormMenu& operator=(const ServerFormMenu&);

    void open(Mode m);
    void layout();
    void validate();
    void save();
    void joinNow();
    int  nRows() const;
    GLW::TextField* fieldAt(int k);

    ServerListMenu* m_list;
    bool  m_open, m_thenJoin;
    Mode  m_mode;
    int   m_row;                 // MODE_EDIT: the row being edited
    int   m_character;           // MODE_PROFILE: 0..6
    float m_builtU;
    Mode  m_builtMode;

    GLW::Label      m_title;
    GLW::Button     m_back, m_save, m_del, m_up, m_down, m_join;
    GLW::TwoLineRow m_rowUi[4];
    GLW::TextField  m_label, m_addr, m_pass, m_name;
    GLW::Toggle     m_remember;
    GLW::Stepper    m_charStep;
    std::string     m_addrHint, m_nameHint;
    bool            m_valid, m_addrErr, m_addrWarn, m_footShown;
    int             m_rowShown[4];    // visible rows this frame (keyboard avoidance), -1 = hidden
    CGRect m_panel, m_content;
    float  m_titleY;
    bool   m_decHeld, m_incHeld;
    int    m_shownChar;
    float  m_shownCharU;
};

#endif
