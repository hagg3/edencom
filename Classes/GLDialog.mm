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
#import "GLDialog.h"
#include <algorithm>
#import "Globals.h"
#import "Util.h"
#import "Input.h"
#import "World.h"

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
    m_panel = CGRectMake(0, 0, 0, 0);
}

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
    for (int j = 0; j < GLDIALOG_MAX_BUTTONS; j++) m_btn[j].setPressed(false);
    m_field.blur();                // stops the platform's text input (the iOS keyboard goes away)
    m_hasField = false;
    m_promptCb = NULL;
    m_active = false;
    m_cb = NULL;
    m_nButtons = 0;
    m_touchSlot = -1;
}

void GLDialog::show(const char* title, const char* body,
                    const char* const* buttons, int n, void (*cb)(int)) {
    if (n < 1) n = 1;
    if (n > GLDIALOG_MAX_BUTTONS) n = GLDIALOG_MAX_BUTTONS;

    GLDialog* d = getDialog();
    d->reset();
    d->m_active = true;
    d->m_cb = cb;
    d->m_nButtons = n;
    d->m_titleText = title ? title : "";
    d->m_bodyText  = body  ? body  : "";
    for (int j = 0; j < n; j++) d->m_btn[j].setLabel(buttons[j] ? buttons[j] : "");
    d->layout();
}

void GLDialog::dismiss() { getDialog()->reset(); }

bool GLDialog::buttonRect(int j, CGRect* r) {
    GLDialog* d = getDialog();
    if (!d->m_active || j < 0 || j >= d->m_nButtons) return false;
    d->layout();
    if (r) *r = d->m_btn[j].rect();
    return true;
}

bool GLDialog::textEntryAvailable() { return eden_text_input_available() != 0; }

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
    // Compact on a pointer, the 44pt floor on touch (GLW::touchFloor) — the same density rule as
    // SettingsMenu. Title and buttons are the display face, so they run ~1.3x the body size.
    const float rowH  = std::max(du(32), touchFloor());
    const float titlePt = du(30), bodyPt = du(14), btnPt = du(20);

    float pw = du(420);
    const float maxW = SCREEN_WIDTH - du(24) * 2.0f;
    if (pw > maxW) pw = maxW;
    const float innerW = pw - pad * 2.0f;
    const float colW   = (innerW - gap) * 0.5f;

    m_title.set(m_titleText.c_str(), titlePt, UITextAlignmentLeft, innerW);
    m_body.set(m_bodyText.c_str(),  bodyPt,  UITextAlignmentLeft, innerW, FACE_BODY);

    const int rows = (m_nButtons + 1) / 2;
    const float fieldH = m_hasField ? std::max(rowH, m_field.heightForLines(m_field.lines())) : rowH;
    float ph = pad + m_title.height() + (m_body.empty() ? 0.0f : du(4) + m_body.height())
             + (m_hasField ? du(12) + fieldH : 0.0f)
             + du(14) + rows * (rowH + gap) - gap + pad;

    const float px = (SCREEN_WIDTH  - pw) * 0.5f;
    const float py = (SCREEN_HEIGHT - ph) * 0.5f;
    m_panel = CGRectMake(px, py, pw, ph);

    m_titleTop = py + ph - pad;
    m_bodyTop  = m_titleTop - m_title.height() - du(4);

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
        }
        if (touches[i].inuse == usage_id && touches[i].down == M_RELEASE) {
            int chosen = -1;
            for (int j = 0; j < m_nButtons; j++) {
                m_btn[j].setPressed(false);
                if (m_btn[j].hit(touches[i].mx, touches[i].my)) chosen = j;
            }
            const bool onField = m_hasField && m_field.hit(touches[i].mx, touches[i].my);
            touches[i].inuse = 0;
            touches[i].down = M_NONE;
            if (chosen >= 0) { choose(chosen); return; }
            // Tapping the field re-raises the keyboard after the player dismissed it (iOS).
            if (onField) m_field.focus();
        }
    }
}

void GLDialog::render() {
    if (!m_active) return;

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);

    const float s = IS_IPAD ? SCALE_WIDTH : 1.0f;
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrthof(0, SCREEN_WIDTH * s, 0, SCREEN_HEIGHT * s, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    GLW::fill(CGRectMake(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT), GLW::kScrim);
    GLW::bevel(m_panel, GLW::BEVEL_WINDOW);

    const float tx = m_panel.origin.x + GLW::du(16);
    m_title.draw(tx, m_titleTop, GLW::kText);
    if (!m_body.empty()) m_body.draw(tx, m_bodyTop, GLW::kTextSecondary);

    if (m_hasField) m_field.render();
    for (int j = 0; j < m_nButtons; j++) m_btn[j].render();

    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();

    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
}
