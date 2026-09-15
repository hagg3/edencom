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

#define KB_ROWS_PER_PAGE 7

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
    void build();                  // resolve the visible rows + their labels, once
    void layout();                 // recompute rects from SCREEN_* (cheap, per frame)
    void refreshKeyLabel(int k);   // row k of the current page -> its button text

    KeybindsMenu(const KeybindsMenu&);
    KeybindsMenu& operator=(const KeybindsMenu&);

    bool  m_active;
    bool  m_built;
    int   m_page;
    int   m_touchSlot;

    std::vector<int> m_vis;        // model row indices this build shows, in order
    std::vector<int> m_groupHead;  // parallel to m_vis: 1 = first row of its group

    GLW::Label  m_title;
    GLW::Label  m_rowLabel[KB_ROWS_PER_PAGE];
    GLW::Label  m_secLabel[KB_ROWS_PER_PAGE];   // the fixed secondary binding, or empty
    GLW::Label  m_groupLabel[KB_ROWS_PER_PAGE];
    GLW::Label  m_capture;         // the "Press a key..." overlay text
    GLW::Button m_key[KB_ROWS_PER_PAGE];
    GLW::Button m_back, m_reset, m_prev, m_next;

    CGRect m_panel;
    CGRect m_content;              // the centred column the rows live in
    float  m_titleTop;
    int    m_laidOutPage;          // which page m_rowLabel/m_key were last built for
};

#endif
