//
//  GLDialog.h
//  Eden
//
//  A centred modal drawn in the engine's own GL UI. It replaces the five UIAlertViews the original
//  game used (Classes/Alert.mm) for hosts that have no native dialog layer — i.e. the native
//  SDL build, iOS/Android, and the web build's `legacy_menu` fallback.
//
//  Phase N Stage 5.1: the hand-rolled dark panel is gone; this is now the FIRST consumer of the
//  GL widget kit (Classes/GLWidgets.h) and it renders the design system's "Alert-2Stack" —
//  a light beveled window with a display-face title, an optional wrapped body sentence, and a
//  TWO-COLUMN button grid whose odd last button (in practice "Cancel") spans the full width.
//
//  The web build's DEFAULT UI does not use this: web/src/seam/seam_link_stubs.mm overrides
//  showAlertWorldType()/showAlertWarpHome() with DOM overlays and never calls GLDialog::show(),
//  so GLDialog::active() is permanently false there and World::render/update skip it.
//
//  Not a general modal stack: one dialog at a time, a C-function callback. The callback fires
//  AFTER the dialog closes and MAY call show() again (the native world-type -> world-height flow
//  chains two dialogs this way).
//
#ifndef Eden_GLDialog_h
#define Eden_GLDialog_h

#import "GLWidgets.h"

#define GLDIALOG_MAX_BUTTONS 6

class GLDialog {
public:
    static GLDialog* getDialog();
    static bool active();

    // `buttons` is an array of `n` (1..GLDIALOG_MAX_BUTTONS) NUL-terminated ASCII strings, laid
    // out left-to-right in two columns, top-to-bottom; an odd final button spans both columns.
    // `body` may be NULL — one or two plain sentences explaining the choice, wrapped to the panel.
    // `cb` is invoked once, with the chosen button index [0, n), after the dialog has closed.
    static void show(const char* title, const char* body,
                     const char* const* buttons, int n, void (*cb)(int chosen));

    void update(float etime);
    void render();

private:
    GLDialog();
    void reset();
    void layout();

    bool  m_active;
    void (*m_cb)(int);
    int   m_nButtons;
    int   m_touchSlot;                 // Input touch index we claimed, or -1

    std::string m_titleText, m_bodyText;
    GLW::Label  m_title;
    GLW::Label  m_body;
    GLW::Button m_btn[GLDIALOG_MAX_BUTTONS];
    CGRect      m_panel;
    float       m_titleTop, m_bodyTop; // point-space y of each block's top edge
};

#endif
