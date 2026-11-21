//
//  GLDialog.mm
//  Eden
//
//  See GLDialog.h. Drawn from World::render()'s tail and pumped from World::update()'s head so it
//  sits on top of whatever screen (menu or in-game HUD) is underneath and eats input while up.
//
//  All of the drawing lives in Classes/GLWidgets.{h,mm} now — this file is layout plus the touch
//  claim protocol, which is the shape every screen converted in Stage 5 should end up with.
//
#include "GLDialog.h"
#include <algorithm>
#include "Globals.h"
#include "Util.h"
#include "Input.h"
#include "World.h"

extern float SCREEN_WIDTH;
extern float SCREEN_HEIGHT;

GLDialog::GLDialog() {
    m_active = false;
    m_cb = NULL;
    m_promptCb = NULL;
    m_hasField = false;
    m_nButtons = 0;
    m_touchSlot = -1;
    m_titleTop = m_bodyTop = 0.0f;
    m_owner = 0;
    m_onEnd = NULL;
    m_hasLink = false;
    m_footerTop = 0.0f;
    m_panel = CGRectMake(0, 0, 0, 0);
}

bool GLDialog::testNoClamp = false;

GLDialog* GLDialog::getDialog() {
    static GLDialog* d = new GLDialog();
    return d;
}

bool GLDialog::active() {
    return getDialog()->m_active;
}

void GLDialog::reset() {
    m_title.clear();
    m_body.clear();
    for (int j = 0; j < GLDIALOG_MAX_BUTTONS; j++) { m_btn[j].setPressed(false); m_btn[j].setTone(GLW::Button::TONE_DEFAULT); }
    m_field.blur();                // stops the platform's text input (the iOS keyboard goes away)
    if (m_field.secure()) { m_field.setSecure(false); m_field.setKind(EDEN_TEXT_KIND_PLAIN); }
    m_field.setText("");           // MP.6: a password does not outlive its prompt
    m_hasField = false;
    m_promptCb = NULL;
    m_active = false;
    m_cb = NULL;
    m_nButtons = 0;
    m_touchSlot = -1;
    m_owner = 0;
    m_onEnd = NULL;
    m_hasLink = false;
    m_link.setPressed(false);
    m_footer.clear();
    m_footerText.clear();
    m_linkText.clear();
    m_bodyShown.clear();
}

// K.7-1b: a dialog that ends without one of its buttons tells its owner (the owner's record, never script code).
void GLDialog::endOwned(int why) {
    const int tok = m_owner;
    void (*fn)(int, int) = m_onEnd;
    reset();
    if (tok && fn) fn(tok, why);
}

void GLDialog::setOwner(int token, const char* footer, const char* link, void (*onEnd)(int, int)) {
    GLDialog* d = getDialog();
    if (!d->m_active) return;
    d->m_owner = token;
    d->m_onEnd = onEnd;
    d->m_footerText = footer ? footer : "";
    d->m_linkText = link ? link : "";
    d->m_hasLink = !d->m_linkText.empty();
    if (d->m_hasLink) { d->m_link.setLabel(d->m_linkText.c_str(), GLW::du(12)); d->m_link.sizeToFit(); }
    d->layout();
}

int GLDialog::ownerToken() { GLDialog* d = getDialog(); return d->m_active ? d->m_owner : 0; }

bool GLDialog::footerLinkRect(CGRect* r) {
    GLDialog* d = getDialog();
    if (!d->m_active || !d->m_hasLink) return false;
    d->layout();
    if (r) *r = d->m_link.rect();
    return true;
}

void GLDialog::show(const char* title, const char* body,
                    const char* const* buttons, int n, void (*cb)(int)) {
    if (n < 1) n = 1;
    if (n > GLDIALOG_MAX_BUTTONS) n = GLDIALOG_MAX_BUTTONS;

    GLDialog* d = getDialog();
    const int oldTok = d->m_active ? d->m_owner : 0;   // C-7: a show() over a mod's dialog resolves its waiter
    void (*oldEnd)(int, int) = d->m_onEnd;
    d->reset();
    if (oldTok && oldEnd) oldEnd(oldTok, END_REPLACED);
    d->m_active = true;
    d->m_cb = cb;
    d->m_nButtons = n;
    d->m_titleText = title ? title : "";
    d->m_bodyText  = body  ? body  : "";
    for (int j = 0; j < n; j++) d->m_btn[j].setLabel(buttons[j] ? buttons[j] : "");
    d->layout();
}

void GLDialog::setDanger(int j) {
    GLDialog* d = getDialog();
    if (d->m_active && j >= 0 && j < d->m_nButtons) d->m_btn[j].setTone(GLW::Button::TONE_DANGER);
}

void GLDialog::dismiss() { GLDialog* d = getDialog(); if (d->m_active && d->m_owner) d->endOwned(END_REPLACED); else d->reset(); }

bool GLDialog::buttonRect(int j, CGRect* r) {
    GLDialog* d = getDialog();
    if (!d->m_active || j < 0 || j >= d->m_nButtons) return false;
    d->layout();
    if (r) *r = d->m_btn[j].rect();
    return true;
}

bool GLDialog::textEntryAvailable() { return eden_text_input_available() != 0; }

void GLDialog::setPromptSecure(bool on) {
    GLDialog* d = getDialog();
    if (!d->m_active || !d->m_hasField) return;
    const bool had = d->m_field.focused();
    if (had) d->m_field.blur();    // re-announce with the secure keyboard kind
    d->m_field.setSecure(on);
    if (!on) d->m_field.setKind(EDEN_TEXT_KIND_PLAIN);
    if (had) d->m_field.focus();
}

const GLW::TextField* GLDialog::promptField() {
    GLDialog* d = getDialog();
    return d->m_active && d->m_hasField ? &d->m_field : NULL;
}

void GLDialog::prompt(const char* title, const char* body, const char* initialText, int maxBytes,
                      const char* const* buttons, int n, void (*cb)(int, const char*), int lines) {
    show(title, body, buttons, n, NULL);
    GLDialog* d = getDialog();
    d->m_hasField = true;
    d->m_promptCb = cb;
    d->m_field.setMaxBytes(maxBytes > 0 ? maxBytes : 49);
    d->m_field.setLines(lines);
    // Display face, like .eden-field; a multi-line field a size down so a line holds more.
    d->m_field.setPointSize(GLW::du(lines > 1 ? 18 : 20));
    d->m_field.setText(initialText);
    d->layout();                   // again, now with the field's row
    d->m_field.focus();
}

// Close BEFORE the callback — it may show() the next dialog. The prompt's text is copied out
// first because reset() is what tears the field down.
void GLDialog::choose(int j) {
    void (*cb)(int) = m_cb;
    void (*pcb)(int, const char*) = m_promptCb;
    const std::string text = m_field.text();
    reset();
    if (pcb) pcb(j, text.c_str());
    else if (cb) cb(j);
}

// Everything here is in design pixels through GLW::du(), so the panel keeps the mockup's
// proportions at any display profile. Two columns, and an ODD LAST BUTTON SPANS BOTH — which is
// what puts "Cancel" on its own full-width row in all three of the port's dialogs.
void GLDialog::layout() {
    using namespace GLW;
    const float pad   = du(16);
    const float gap   = du(8);
    // Compact on a pointer, the 44 OS-pt floor on touch (GLW::controlH) — the same density rule as
    // SettingsMenu. Title and buttons are the display face, so they run ~1.3x the body size.
    const float rowH  = controlH(32);
    const float titlePt = du(30), bodyPt = du(14), btnPt = du(20);

    float pw = du(420);
    const CGRect safe = GLW::safeRect();   // I.2: centred clear of the island and home bar
    const float maxW = safe.size.width - du(24) * 2.0f;
    if (pw > maxW) pw = maxW;
    const float innerW = pw - pad * 2.0f;
    const float colW   = (innerW - gap) * 0.5f;

    m_title.set(m_titleText.c_str(), titlePt, UITextAlignmentLeft, innerW);

    // K.7-1b (C-8): the footer row ("Mod: <pack>", and the mute link at its right), then the rest of the panel
    // without the body; the body gets what is left of the safe height and is cut to it with an ellipsis.
    float footerRowH = 0.0f;
    float linkW = 0.0f;
    if (!m_footerText.empty() || m_hasLink) {
        if (m_hasLink) { linkW = m_link.fitWidth(); m_link.setRect(CGRectMake(0, 0, linkW, controlH(26))); }
        m_footer.set(m_footerText.c_str(), du(12), UITextAlignmentLeft, std::max(du(40), innerW - linkW - (m_hasLink ? du(8) : 0.0f)), FACE_BODY);
        footerRowH = std::max(m_footer.height(), m_hasLink ? controlH(26) : 0.0f);
    } else m_footer.clear();
    const int rows = (m_nButtons + 1) / 2;
    const float fieldH = m_hasField ? std::max(rowH, m_field.heightForLines(m_field.lines())) : rowH;
    const float fixedH = pad + m_title.height()
             + (m_hasField ? du(12) + fieldH : 0.0f)
             + du(14) + rows * (rowH + gap) - gap + pad
             + (footerRowH > 0.0f ? du(10) + footerRowH : 0.0f);
    m_bodyShown = m_bodyText;
    m_body.set(m_bodyShown.c_str(), bodyPt, UITextAlignmentLeft, innerW, FACE_BODY);
    const float maxBodyH = safe.size.height - du(16) - fixedH - du(4);
    if (!testNoClamp && !m_body.empty() && m_body.height() > maxBodyH) {
        const float keep = std::max(maxBodyH, m_body.lineHeight());
        size_t lo = 0, hi = m_bodyText.size();   // the longest prefix (+ "...") that fits
        while (lo < hi) {
            size_t mid = (lo + hi + 1) / 2;
            while (mid > lo && ((unsigned char)m_bodyText[mid] & 0xC0) == 0x80) mid--;   // not inside a UTF-8 sequence
            if (mid <= lo) { hi = lo; break; }
            m_body.set((m_bodyText.substr(0, mid) + "...").c_str(), bodyPt, UITextAlignmentLeft, innerW, FACE_BODY);
            if (m_body.height() <= keep) lo = mid; else hi = mid - 1;
        }
        m_bodyShown = m_bodyText.substr(0, lo) + "...";
        m_body.set(m_bodyShown.c_str(), bodyPt, UITextAlignmentLeft, innerW, FACE_BODY);
    }
    float ph = fixedH + (m_body.empty() ? 0.0f : du(4) + m_body.height());

    const float px = safe.origin.x + (safe.size.width  - pw) * 0.5f;
    const float py = safe.origin.y + (safe.size.height - ph) * 0.5f;
    m_panel = CGRectMake(px, py, pw, ph);

    m_titleTop = py + ph - pad;
    m_bodyTop  = m_titleTop - m_title.height() - du(4);
    if (footerRowH > 0.0f) {
        m_footerTop = py + pad + footerRowH - (footerRowH - m_footer.height()) * 0.5f;
        if (m_hasLink) m_link.setRect(CGRectMake(px + pw - pad - linkW, py + pad, linkW, footerRowH));
    }

    float rowTop = (m_body.empty() ? m_titleTop - m_title.height()
                                   : m_bodyTop - m_body.height()) - du(14);
    if (m_hasField) {
        rowTop += du(14) - du(12);
        m_field.setRect(CGRectMake(px + pad, rowTop - fieldH, innerW, fieldH));
        // The iOS keyboard lifts the view only as far as this rect's bottom: the field alone left
        // the buttons under the keyboard (T.B1). The whole panel keeps them, and the title, on screen.
        m_field.setKeepVisible(m_panel);
        rowTop -= fieldH + du(14);
    }
    for (int j = 0; j < m_nButtons; j++) {
        const bool lastOdd = (j == m_nButtons - 1) && (m_nButtons % 2 == 1);
        const int  col = j % 2;
        const float w = lastOdd ? innerW : colW;
        const float x = lastOdd ? px + pad : px + pad + col * (colW + gap);
        m_btn[j].setPointSize(btnPt);      // before setRect: the wrap is resolved there
        m_btn[j].setRect(CGRectMake(x, rowTop - rowH, w, rowH));
        if (lastOdd || col == 1) rowTop -= (rowH + gap);
    }
}

void GLDialog::update(float etime) {
    (void)etime;
    if (!m_active) return;

    Input* input = Input::getInput();
    itouch* touches = input->getTouches();
    static const int usage_id = 9;   // distinct from Menu / SettingsMenu (3) / Hud

    if (m_hasField) {
        const GLW::TextField::Event ev = m_field.update();
        if (ev == GLW::TextField::EV_COMMIT) { choose(0); return; }
        if (ev == GLW::TextField::EV_CANCEL) { choose(m_nButtons - 1); return; }
    }

    for (int i = 0; i < MAX_TOUCHES; i++) {
        if (touches[i].inuse == 0 && touches[i].down == M_DOWN) {
            touches[i].inuse = usage_id;
            for (int j = 0; j < m_nButtons; j++)
                m_btn[j].setPressed(m_btn[j].hit(touches[i].mx, touches[i].my));
            if (m_hasLink) m_link.setPressed(m_link.hit(touches[i].mx, touches[i].my));
        }
        if (touches[i].inuse == usage_id && touches[i].down == M_RELEASE) {
            int chosen = -1;
            for (int j = 0; j < m_nButtons; j++) {
                m_btn[j].setPressed(false);
                if (m_btn[j].hit(touches[i].mx, touches[i].my)) chosen = j;
            }
            const bool onLink = m_hasLink && m_link.hit(touches[i].mx, touches[i].my);
            m_link.setPressed(false);
            const bool onField = m_hasField && m_field.hit(touches[i].mx, touches[i].my);
            touches[i].inuse = 0;
            touches[i].down = M_NONE;
            if (chosen >= 0) { choose(chosen); return; }
            if (onLink) { endOwned(END_LINK); return; }
            // Tapping the field re-raises the keyboard after the player dismissed it (iOS).
            if (onField) m_field.focus();
        }
    }
}

bool GLDialog::escape() {
    GLDialog* d = getDialog();
    if (!d->m_active) return false;
    if (!d->m_hasField) d->choose(d->m_nButtons - 1);
    return true;
}

void GLDialog::render() {
    if (!m_active) return;

    GLW::begin2D();
    // UX.7: the dialog draws last, so it takes the focus from the screen under it. Return is button 0
    // only where that was already the rule (a prompt) or there is nothing else to choose; a two-button
    // confirm (Delete / Cancel) wants its button focused first.
    GLW::Focus::screen(this, (m_hasField || m_nButtons == 1) ? &m_btn[0] : NULL);

    GLW::fill(CGRectMake(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT), GLW::kScrim);
    GLW::bevel(m_panel, GLW::BEVEL_WINDOW);

    const float tx = m_panel.origin.x + GLW::du(16);
    m_title.draw(tx, m_titleTop, GLW::kText);
    if (!m_body.empty()) m_body.draw(tx, m_bodyTop, GLW::kTextSecondary);

    if (m_footer.empty() == false) m_footer.draw(tx, m_footerTop, GLW::kTextMuted);
    if (m_hasLink) m_link.render();
    if (m_hasField) m_field.render();
    for (int j = 0; j < m_nButtons; j++) m_btn[j].render();

    GLW::end2D();
}
