//
//  KeybindsMenu.h
//  Eden
//
//  The GL keybinds list (Phase N Stage 5.3, WORKING/phase-n-stage3plus-plan-2026-09-05.md): every
//  rebindable action with its current key, drawn with the widget kit (Classes/GLWidgets.h) and
//  reading the Stage 5.2 keybind model (the KEYBINDS section of web/src/seam/Settings_web.mm, a
//  SHARED seam file — so this list and the web build's Keys tab are two front-ends over one table).
//
//  U.4 (WORKING/ui-rework-plan-2026-10-11.md): it is no longer a screen. It is the CONTENT of
//  SettingsMenu's Keys tab — SettingsMenu owns the panel, the titlebar (Back, title, Defaults), the
//  rail, the touch claim and the scroll gestures, and hands this the content rect, the scrollbar
//  rect and the touches that land on a row. What lives here is only what is keybind-shaped: the
//  rows (group headings + TwoLineRows of action · "also X" · key button), the capture modal, and
//  refreshing button text when a capture resolves. Web --wrap's SettingsMenu::update/render to
//  no-ops (EDEN_LD_WRAP, Settings_web.mm), so this cannot draw or eat input on web.
//
//  CAPTURE. Tapping a row's key button arms the model's one capture slot
//  (eden_keybind_capture_begin); the next key the platform input layer sees is fed to
//  eden_keybind_capture_feed instead of being played. Escape cancels (the model consumes it, so it
//  never reaches the menu key). See that function for why arming lives in the model.
//
#ifndef Eden_KeybindsMenu_h
#define Eden_KeybindsMenu_h

#import "GLWidgets.h"
#include <string>
#include <vector>

// Array capacity for items (rows + group headings). The model has 26 rows in 4 groups today.
#define KB_MAX_ITEMS 40

class KeybindsMenu {
public:
    KeybindsMenu();
    ~KeybindsMenu();

    // Every label, once (on entering the tab, or a u() change) — a label is a texture upload.
    void build();
    void refresh();                    // every key button's text from the model (Defaults, capture)
    // Per frame: row heights into the scroll view, rows placed from its first() row.
    void layout(CGRect content, CGRect bar);
    GLW::ScrollView& scroll()          { return m_scroll; }

    // Input, under SettingsMenu's touch claim. down() presses the key button under the point and
    // says whether it took the touch; up() arms a capture if the release is on that button.
    bool down(float mx, float my);
    void up(float mx, float my);
    void clearPressed();
    void poll();                       // a capture that resolved since last frame -> refresh()

    bool capturing() const;
    void cancelCapture();

    void render();                     // rows + scrollbar; the caller has set up the GL state
    void renderCapture(CGRect panel);  // the "Press a key" modal, over the whole screen

private:
    // A row (mi = model index) or a group heading (mi < 0, `group` names it).
    struct Item { int mi; const char* group; };

    KeybindsMenu(const KeybindsMenu&);
    KeybindsMenu& operator=(const KeybindsMenu&);
    bool shown(int k) const;
    float headingHeight() const;

    std::vector<Item> m_items;
    bool  m_built;
    bool  m_wasCapturing;
    float m_builtU;
    int   m_pressed;                   // item whose key button is held, -1 none

    GLW::TwoLineRow m_row[KB_MAX_ITEMS];
    GLW::Label      m_head[KB_MAX_ITEMS];
    GLW::Button     m_key[KB_MAX_ITEMS];
    std::vector<std::string> m_hint;   // "also Up", or empty
    GLW::Label      m_capture;
    GLW::ScrollView m_scroll;
    CGRect m_content;
};

#endif
