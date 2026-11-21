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
//  The web build's DEFAULT UI does not use this: web-port-final:web/src/seam/seam_link_stubs.mm overrides
//  showAlertWorldType()/showAlertWarpHome() with DOM overlays and never calls GLDialog::show(),
//  so GLDialog::active() is permanently false there and World::render/update skip it.
//
//  Not a general modal stack: one dialog at a time, a C-function callback. The callback fires
//  AFTER the dialog closes and MAY call show() again (the native world-type -> world-height flow
//  chains two dialogs this way).
//
#ifndef Eden_GLDialog_h
#define Eden_GLDialog_h

#include "GLWidgets.h"

#define GLDIALOG_MAX_BUTTONS 8   // 8 since the Debug menu (four rows)

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

    // N.4.5: the same dialog with a GLW::TextField between the body and the buttons, focused on
    // show (on iOS that raises the system keyboard). Return chooses button 0, Escape the LAST
    // button — so pass the confirm first and "Cancel" last, which the grid already wants. `cb`
    // gets the chosen index and the field's UTF-8 text (valid only during the call). Check
    // textEntryAvailable() first: on a host without GL text input (web) the field cannot be typed
    // into, and the caller should not offer the action at all.
    // `lines` > 1 (D.4b) makes the field multi-line: it wraps, shows that many lines, and scrolls
    // with the caret -- for texts longer than a line (a sign's 95 bytes, a script's 511).
    static void prompt(const char* title, const char* body, const char* initialText, int maxBytes,
                       const char* const* buttons, int n, void (*cb)(int chosen, const char* text),
                       int lines = 1);
    // UX.4: call right after show()/prompt(): button j draws with the danger tone (a destructive choice).
    static void setDanger(int j);
    static bool textEntryAvailable();
    // MP.6: call right after prompt() for a password -- the field draws bullets and raises the
    // secure keyboard (GLW::TextField::setSecure). Cleared when the dialog closes.
    static void setPromptSecure(bool on);
    // The open prompt's field (the harness: its shownText() must never hold the password), or NULL.
    static const GLW::TextField* promptField();
    // Closes whatever is up WITHOUT calling its callback — the harness's way out of a modal
    // (--shot), and the answer to "the screen under the dialog went away". Not a "Cancel":
    // a caller that needs its cancel branch to run must pick the button instead.
    static void dismiss();
    // Stage 5.6: where button j is (point space, y up) while a dialog is up — so a harness taps the
    // real button instead of assuming the grid. False when no dialog is up or j is out of range.
    static bool buttonRect(int j, CGRect* r);
    // UX.9: Escape over an open dialog = its last button (Cancel / Keep editing; a prompt's field does this itself).
    // True if a dialog was up and took it.
    static bool escape();

    // K.7-1b (V11, finding C-7): a mod's dialog. Call right after show()/prompt(). `token` > 0 owns the dialog;
    // `footer` is the muted "Mod: <pack>" line; `link` != NULL adds the footer link (a control OUTSIDE the button
    // grid, so Escape still picks the script's own last button). `onEnd(token, why)` runs, once, when the dialog
    // ends WITHOUT one of its buttons: END_REPLACED (a show()/prompt() over it, or dismiss()) or END_LINK (the
    // link was tapped; the dialog has closed). It must not run script code. A button choice does not call it.
    enum { END_REPLACED = 1, END_LINK = 2 };
    static void setOwner(int token, const char* footer, const char* link, void (*onEnd)(int token, int why));
    static int  ownerToken();                       // 0 = no dialog, or an engine dialog
    static bool footerLinkRect(CGRect* r);          // where the footer link is (the harness taps it); false = none
    static bool testNoClamp;                        // mutation (--lua-mutate=dialognoclamp): no body clamp (C-8)

    void update(float etime);
    void render();

private:
    GLDialog();
    void reset();
    void layout();
    void choose(int j);
    void layoutFooter(float px, float pad, float innerW, float py);
    void endOwned(int why);

    bool  m_active;
    void (*m_cb)(int);
    void (*m_promptCb)(int, const char*);
    bool  m_hasField;
    GLW::TextField m_field;
    int   m_nButtons;
    int   m_touchSlot;                 // Input touch index we claimed, or -1

    std::string m_titleText, m_bodyText;
    GLW::Label  m_title;
    GLW::Label  m_body;
    GLW::Button m_btn[GLDIALOG_MAX_BUTTONS];
    CGRect      m_panel;
    float       m_titleTop, m_bodyTop; // point-space y of each block's top edge
    int         m_owner;                           // K.7-1b: a mod dialog's token (0 = none)
    void      (*m_onEnd)(int, int);
    std::string m_footerText, m_linkText, m_bodyShown;
    GLW::Label  m_footer;
    GLW::Button m_link;
    bool        m_hasLink;
    float       m_footerTop;
};

#endif
