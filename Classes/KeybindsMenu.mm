//
//  KeybindsMenu.mm
//  Eden
//
//  See KeybindsMenu.h. Layout + the touch-claim protocol; every pixel is a GLWidgets call, which
//  is the shape Stage 5.1 set for a converted screen (GLDialog.mm is the worked example).
//
#import "KeybindsMenu.h"
#import "Globals.h"
#import "Util.h"
#import "World.h"
#include <cstdio>
#include <cstring>

extern float SCREEN_WIDTH;
extern float SCREEN_HEIGHT;

// The Stage 5.2 model, in the shared seam file web/src/seam/Settings_web.mm.
extern "C" {
int  eden_keybind_count(void);
const char* eden_keybind_label(int i);
const char* eden_keybind_group(int i);
int  eden_keybind_get(int i);
int  eden_keybind_secondary(int i);
int  eden_keybind_native_hidden(int i);
const char* eden_keybind_code_label(int code);
void eden_keybind_reset_all(void);
void eden_keybind_capture_begin(int i);
void eden_keybind_capture_cancel(void);
int  eden_keybind_capture_active(void);
}

// Distinct from Menu / SettingsMenu (3) / GLDialog (9). SettingsMenu hands this screen the whole
// frame while it is up, so the two never contend — but they must not SHARE an id either, or a
// touch that began in one would be released by the other.
static const int usage_id = 11;

KeybindsMenu::KeybindsMenu() {
    m_active = false;
    m_built = false;
    m_page = 0;
    m_touchSlot = -1;
    m_titleTop = 0.0f;
    m_laidOutPage = -1;
    m_panel = CGRectMake(0, 0, 0, 0);
}

KeybindsMenu::~KeybindsMenu() {}

void KeybindsMenu::show() {
    m_active = true;
    m_page = 0;
    m_laidOutPage = -1;              // force a page rebuild on the first update
    m_touchSlot = -1;
    eden_keybind_capture_cancel();   // never inherit an armed capture from a previous visit
}

void KeybindsMenu::hide() {
    m_active = false;
    m_touchSlot = -1;
    eden_keybind_capture_cancel();
}

void KeybindsMenu::build() {
    if (m_built) return;
    m_built = true;

    m_vis.clear();
    m_groupHead.clear();
    const char* prevGroup = "";
    const int n = eden_keybind_count();
    for (int i = 0; i < n; ++i) {
        if (eden_keybind_native_hidden(i)) continue;   // no native implementation: do not offer it
        const char* g = eden_keybind_group(i);
        m_groupHead.push_back(std::strcmp(g, prevGroup) != 0 ? 1 : 0);
        prevGroup = g;
        m_vis.push_back(i);
    }

    m_title.set("Controls", GLW::du(24), UITextAlignmentLeft);
    m_back.setLabel("Back", GLW::du(14));
    m_reset.setLabel("Defaults", GLW::du(14));
    m_prev.setLabel("<", GLW::du(16));
    m_next.setLabel(">", GLW::du(16));
    m_capture.set("Press a key -- Esc to cancel", GLW::du(16), UITextAlignmentCenter);
}

// A row's label, its button text, and the fixed-secondary note.
//
// THE SECONDARY IS NOT ON THE BUTTON. It was, in the first version, as "W / Up" — and the first
// artefact showed why that is wrong twice over: "L Shift / R Shift" word-wrapped out of its own
// button, and putting a binding the player CANNOT change inside the control that changes bindings
// says the wrong thing. It is dim text beside the button instead: visible, because arrow-key
// movement is live input and hiding it makes it look like a bug, but plainly not the control.
void KeybindsMenu::refreshKeyLabel(int k) {
    const int vi = m_page * KB_ROWS_PER_PAGE + k;
    if (vi >= (int)m_vis.size()) {
        m_key[k].setLabel("");
        m_rowLabel[k].clear();
        m_secLabel[k].clear();
        m_groupLabel[k].clear();
        return;
    }
    const int mi = m_vis[vi];

    const char* primary = eden_keybind_code_label(eden_keybind_get(mi));
    m_key[k].setPointSize(GLW::du(14));
    m_key[k].setLabel(primary && primary[0] ? primary : "--", GLW::du(14));

    const int sec = eden_keybind_secondary(mi);
    if (sec != 0) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "also %s", eden_keybind_code_label(sec));
        // Right-aligned against an explicit box: Label resolves alignment against the width it
        // was wrapped to, so a right-aligned label with no maxWidth aligns against zero and lands
        // on top of whatever is to its right — which is the key button.
        m_secLabel[k].set(buf, GLW::du(11), UITextAlignmentRight, GLW::du(140));
    } else {
        m_secLabel[k].clear();
    }

    m_rowLabel[k].set(eden_keybind_label(mi), GLW::du(14), UITextAlignmentLeft);
    if (m_groupHead[vi]) m_groupLabel[k].set(eden_keybind_group(mi), GLW::du(11), UITextAlignmentLeft);
    else                 m_groupLabel[k].clear();
}

void KeybindsMenu::layout() {
    using namespace GLW;
    const float pad = du(16);

    m_panel = CGRectMake(du(8), du(8), SCREEN_WIDTH - du(16), SCREEN_HEIGHT - du(16));
    m_titleTop = m_panel.origin.y + m_panel.size.height - pad;

    // A CENTRED CONTENT COLUMN, not the full panel width. The first artefact put the labels at the
    // far left edge and the key buttons at the far right, with ~700pt of dead grey between them —
    // legible only if you can hold a row's two ends in your head at once. The column is capped at
    // the mockups' content measure and centred; on a narrow window it simply becomes the panel.
    float cw = du(560);
    const float maxCw = m_panel.size.width - pad * 2.0f;
    if (cw > maxCw) cw = maxCw;
    m_content = CGRectMake(m_panel.origin.x + (m_panel.size.width - cw) * 0.5f,
                           m_panel.origin.y, cw, m_panel.size.height);

    // Bottom control row, then the list gets whatever is left. Buttons are laid out at the design
    // system's 44pt touch floor (GLW::kTouchFloor), not at their design height — the same rule
    // every other converted screen follows, and the reason a phone can hit them at all.
    const float btnH = kTouchFloor;
    const float btnY = m_panel.origin.y + pad;
    const float bw   = du(96);
    m_back.setRect(CGRectMake(m_content.origin.x, btnY, bw, btnH));
    m_reset.setRect(CGRectMake(m_content.origin.x + bw + du(8), btnY, bw, btnH));
    const float right = m_content.origin.x + m_content.size.width;
    m_next.setRect(CGRectMake(right - du(48), btnY, du(48), btnH));
    m_prev.setRect(CGRectMake(right - du(48) * 2.0f - du(8), btnY, du(48), btnH));

    // du(20) rather than du(10): the first row on every page carries a group heading drawn ABOVE
    // its strip, and without the extra line it prints into the title.
    const float top    = m_titleTop - m_title.height() - du(20);
    const float bottom = btnY + btnH + du(10);
    float pitch = (top - bottom) / KB_ROWS_PER_PAGE;
    if (pitch > du(46)) pitch = du(46);

    const float keyW = du(104);
    for (int k = 0; k < KB_ROWS_PER_PAGE; ++k) {
        const float rowTop = top - k * pitch;
        float h = pitch - du(6);
        if (h > kTouchFloor) h = kTouchFloor;
        m_key[k].setRect(CGRectMake(right - keyW, rowTop - (pitch + h) * 0.5f, keyW, h));
    }
}

void KeybindsMenu::update(float etime) {
    (void)etime;
    if (!m_active) return;
    build();
    if (m_laidOutPage != m_page) {
        m_laidOutPage = m_page;
        for (int k = 0; k < KB_ROWS_PER_PAGE; ++k) refreshKeyLabel(k);
    }
    layout();

    Input* input = Input::getInput();
    itouch* touches = input->getTouches();
    const int visN   = (int)m_vis.size();
    const int pages  = (visN + KB_ROWS_PER_PAGE - 1) / KB_ROWS_PER_PAGE;
    const int first  = m_page * KB_ROWS_PER_PAGE;

    // While a capture is armed the list is inert: a touch cancels it and does nothing else. The
    // alternative — letting a tap both cancel and activate what is under it — means the tap that
    // gets you out of "press a key" also rebinds whatever row you happened to hit.
    const bool capturing = (eden_keybind_capture_active() >= 0);

    for (int i = 0; i < MAX_TOUCHES; ++i) {
        if (touches[i].inuse == 0 && touches[i].down == M_DOWN) {
            touches[i].inuse = usage_id;
            if (capturing) continue;
            m_back.setPressed(m_back.hit(touches[i].mx, touches[i].my));
            m_reset.setPressed(m_reset.hit(touches[i].mx, touches[i].my));
            m_prev.setPressed(m_page > 0 && m_prev.hit(touches[i].mx, touches[i].my));
            m_next.setPressed(m_page < pages - 1 && m_next.hit(touches[i].mx, touches[i].my));
            for (int k = 0; k < KB_ROWS_PER_PAGE; ++k)
                m_key[k].setPressed(first + k < visN && m_key[k].hit(touches[i].mx, touches[i].my));
        }
        if (touches[i].inuse == usage_id && touches[i].down == M_RELEASE) {
            const float mx = touches[i].mx, my = touches[i].my;
            touches[i].inuse = 0;
            touches[i].down = M_NONE;

            m_back.setPressed(false); m_reset.setPressed(false);
            m_prev.setPressed(false); m_next.setPressed(false);
            for (int k = 0; k < KB_ROWS_PER_PAGE; ++k) m_key[k].setPressed(false);

            if (capturing) { eden_keybind_capture_cancel(); continue; }

            if (m_back.hit(mx, my)) { hide(); return; }
            if (m_reset.hit(mx, my)) {
                eden_keybind_reset_all();
                for (int k = 0; k < KB_ROWS_PER_PAGE; ++k) refreshKeyLabel(k);
                continue;
            }
            if (m_prev.hit(mx, my)) { if (m_page > 0)          { m_page--; } continue; }
            if (m_next.hit(mx, my)) { if (m_page < pages - 1)  { m_page++; } continue; }
            for (int k = 0; k < KB_ROWS_PER_PAGE; ++k) {
                if (first + k >= visN) break;
                if (m_key[k].hit(mx, my)) { eden_keybind_capture_begin(m_vis[first + k]); break; }
            }
        }
    }

    // A capture that resolved (the platform fed a key into the model) leaves stale button text.
    // Polling one int per frame is cheaper than a callback and cannot get out of sync with a
    // capture the model cancelled on its own.
    if (capturing && eden_keybind_capture_active() < 0)
        for (int k = 0; k < KB_ROWS_PER_PAGE; ++k) refreshKeyLabel(k);
}

void KeybindsMenu::render() {
    if (!m_active) return;
    build();
    layout();

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

    using namespace GLW;
    fill(CGRectMake(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT), kScrim);
    bevel(m_panel, BEVEL_WINDOW);

    const float pad = du(16);
    const float lx  = m_content.origin.x;
    m_title.draw(lx, m_titleTop, kText);

    const int visN  = (int)m_vis.size();
    const int pages = (visN + KB_ROWS_PER_PAGE - 1) / KB_ROWS_PER_PAGE;
    const int first = m_page * KB_ROWS_PER_PAGE;

    for (int k = 0; k < KB_ROWS_PER_PAGE; ++k) {
        if (first + k >= visN) break;
        const CGRect r = m_key[k].rect();
        const float cy = r.origin.y + r.size.height * 0.5f;
        // Above the strip, clear of it: the strip's own top edge is +du(3), and a label draws
        // DOWNWARD from the y it is given, so anything less than one line height past that edge
        // prints into the row below its heading.
        if (!m_groupLabel[k].empty())
            m_groupLabel[k].draw(lx, r.origin.y + r.size.height + du(5) + m_groupLabel[k].lineHeight(),
                                 kTextSecondary);
        // A SUNKEN content strip behind the whole row. Without it the label and its button are two
        // unrelated things at opposite ends of a wide grey field; with it they read as one row,
        // which is the entire job of the design system's CONTENT surface.
        bevel(CGRectMake(lx, r.origin.y - du(3),
                         m_content.size.width, r.size.height + du(6)), BEVEL_CONTENT);
        m_rowLabel[k].draw(lx + du(10), cy + m_rowLabel[k].lineHeight() * 0.5f, kText);
        if (!m_secLabel[k].empty())
            m_secLabel[k].draw(r.origin.x - du(140) - du(10),
                               cy + m_secLabel[k].lineHeight() * 0.5f, kTextSecondary);
        m_key[k].render();
    }

    m_back.render();
    m_reset.render();
    if (m_page > 0)         m_prev.render();
    if (m_page < pages - 1) m_next.render();

    // The capture overlay is a second scrim over the whole screen rather than a badge on the row:
    // a capture swallows the NEXT key press wherever it lands, so the modal state has to look
    // modal or the player will keep typing at a screen that is no longer listening to them.
    if (eden_keybind_capture_active() >= 0) {
        fill(CGRectMake(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT), kScrim);
        const float bh = du(64);
        const CGRect box = CGRectMake(m_panel.origin.x + du(40),
                                      (SCREEN_HEIGHT - bh) * 0.5f,
                                      m_panel.size.width - du(80), bh);
        bevel(box, BEVEL_WINDOW);
        m_capture.drawCentered(box.origin.x + box.size.width * 0.5f,
                               box.origin.y + box.size.height * 0.5f + m_capture.height() * 0.5f,
                               kText);
    }

    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();

    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
}
