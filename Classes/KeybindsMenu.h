//
//  KeybindsMenu.h
//  Eden
//
//  The GL keybinds screen (Phase N Stage 5.3,
//  WORKING/phase-n-stage3plus-plan-2026-09-05.md). A paginated list of every rebindable action
//  with its current key, drawn with the Stage 5.1 widget kit (Classes/GLWidgets.h) and reading
//  the Stage 5.2 keybind model (the KEYBINDS section of web/src/seam/Settings_web.mm, a SHARED
//  seam file — so this screen and the web build's Keys tab are two front-ends over one table).
//
//  WHERE IT LIVES IN THE SCREEN GRAPH, and why it is not a sibling of SettingsMenu. It is owned
//  and driven by SettingsMenu: its "Keys" button shows this, and SettingsMenu::update/render
//  delegate to it wholesale while it is up. That is one entry point instead of a new Menu state,
//  and it buys the thing Stage 5's plan flags as the stage's main risk for free — web `--wrap`s
//  SettingsMenu::update/render to no-ops (EDEN_LD_WRAP, Settings_web.mm), so a screen reachable
//  only from inside them cannot draw or eat input on web no matter what it does.
//
//  CAPTURE. Tapping a row's key button arms the model's one capture slot
//  (eden_keybind_capture_begin); the next key the platform input layer sees is fed to
//  eden_keybind_capture_feed instead of being played. Escape cancels. See that function for why
//  arming lives in the model rather than here.
//
#ifndef Eden_KeybindsMenu_h
#define Eden_KeybindsMenu_h

#import "GLWidgets.h"
#import "Input.h"
#include <vector>

// Slots per page are fitted to the screen (fit(), 2026-10-05 — the same density rule as
// SettingsMenu); this is only the array capacity. A slot is a row or a group heading.
#define KB_ROWS_PER_PAGE 16

class KeybindsMenu {
public:
    KeybindsMenu();
    ~KeybindsMenu();

    bool active() const { return m_active; }
    void show();
    void hide();

    void update(float etime);
    void render();

private:
    // One slot on a page: a group heading (vi < 0, group = its m_vis index) or a row (vi = index
    // into m_vis).
    struct Item { int vi; int head; };

    void fit();                    // pitch + slots per page from SCREEN_*; re-pages on change
    void build();                  // resolve the visible rows into pages, once per fit
    void layout();                 // recompute rects from SCREEN_* (cheap, per frame)
    void refreshKeyLabel(int k);   // slot k of the current page -> its labels + button text

    KeybindsMenu(const KeybindsMenu&);
    KeybindsMenu& operator=(const KeybindsMenu&);

    bool  m_active;
    bool  m_built;
    int   m_page;
    int   m_touchSlot;
    int   m_slots;                 // fit()'s slots per page
    int   m_maxPageLen;
    float m_pitch, m_btnH;

    std::vector<int> m_vis;        // model row indices this build shows, in order
    std::vector<std::vector<Item> > m_pages;

    GLW::Label  m_title;
    GLW::Label  m_rowLabel[KB_ROWS_PER_PAGE];   // a row's action, or a heading's group name
    GLW::Label  m_secLabel[KB_ROWS_PER_PAGE];   // the fixed secondary binding, or empty
    GLW::Label  m_capture;         // the "Press a key..." overlay text
    GLW::Button m_key[KB_ROWS_PER_PAGE];
    GLW::Button m_back, m_reset, m_prev, m_next;
    GLW::Label  m_pageLabel;

    CGRect m_panel;
    CGRect m_content;              // the column the rows live in
    CGRect m_slot[KB_ROWS_PER_PAGE];
    float  m_titleY;
    int    m_laidOutPage;          // which page the labels/buttons were last built for
};

#endif
