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
#include <algorithm>
#include <cmath>
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
    m_slots = 8;
    m_maxPageLen = 0;
    m_pitch = m_btnH = 0.0f;
    m_titleY = 0.0f;
    m_laidOutPage = -1;
    m_panel = m_content = CGRectMake(0, 0, 0, 0);
    for (int k = 0; k < KB_ROWS_PER_PAGE; ++k) m_slot[k] = CGRectMake(0, 0, 0, 0);
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

// SettingsMenu::fit()'s rule: rows as tight as the pointer allows (30u on a mouse, the 44pt touch
// floor on touch), as many per page as the screen holds, re-paged when either changes.
void KeybindsMenu::fit() {
    using namespace GLW;
    const float tf = touchFloor();
    m_pitch = std::max(du(30), tf);
    m_btnH  = std::max(du(30), tf);
    const float margin = du(12), pad = du(12), gap = du(10);
    const float avail = SCREEN_HEIGHT - 2.0f * margin - 2.0f * pad - 2.0f * (m_btnH + gap);
    int cap = (int)std::floor(avail / m_pitch);
    if (cap < 3) cap = 3;
    if (cap > KB_ROWS_PER_PAGE) cap = KB_ROWS_PER_PAGE;
    if (cap != m_slots) { m_slots = cap; m_built = false; m_laidOutPage = -1; }
}

// A group heading takes a SLOT of its own (SettingsMenu's rule): every page starts with its
// group's heading, a continued group repeats it, and a heading never ends a page. Stage 5.3 drew
// the heading in the gap above a strip in 11pt, which in the display face was unreadable.
void KeybindsMenu::build() {
    if (m_built) return;
    m_built = true;

    m_vis.clear();
    m_pages.clear();
    const int n = eden_keybind_count();
    for (int i = 0; i < n; ++i)
        if (!eden_keybind_native_hidden(i)) m_vis.push_back(i);   // no native implementation

    std::vector<Item> cur;
    const char* curGroup = NULL;
    for (int v = 0; v < (int)m_vis.size(); ++v) {
        const char* g = eden_keybind_group(m_vis[v]);
        bool head = cur.empty() || !curGroup || std::strcmp(g, curGroup) != 0;
        if ((int)cur.size() + (head ? 2 : 1) > m_slots) {
            m_pages.push_back(cur);
            cur.clear();
            head = true;
        }
        if (head) { Item h = { -1, v }; cur.push_back(h); }
        Item it = { v, 0 };
        cur.push_back(it);
        curGroup = g;
    }
    if (!cur.empty() || m_pages.empty()) m_pages.push_back(cur);   // never zero pages
    if (m_page >= (int)m_pages.size()) m_page = 0;
    m_maxPageLen = 0;
    for (size_t p = 0; p < m_pages.size(); ++p)
        if ((int)m_pages[p].size() > m_maxPageLen) m_maxPageLen = (int)m_pages[p].size();

    m_title.set("Controls", GLW::du(32), UITextAlignmentCenter);
    m_back.setLabel("Back", GLW::du(22));
    m_reset.setLabel("Defaults", GLW::du(22));
    m_prev.setLabel("<", GLW::du(22));
    m_next.setLabel(">", GLW::du(22));
    m_capture.set("Press a key -- Esc to cancel", GLW::du(24), UITextAlignmentCenter);
}

// A slot's labels and button text.
//
// THE SECONDARY IS NOT ON THE BUTTON. It was, in the first version, as "W / Up" — and the first
// artefact showed why that is wrong twice over: "L Shift / R Shift" word-wrapped out of its own
// button, and putting a binding the player CANNOT change inside the control that changes bindings
// says the wrong thing. It is dim text beside the button instead: visible, because arrow-key
// movement is live input and hiding it makes it look like a bug, but plainly not the control.
void KeybindsMenu::refreshKeyLabel(int k) {
    m_secLabel[k].clear();
    if (m_page >= (int)m_pages.size() || k >= (int)m_pages[m_page].size()) {
        m_key[k].setLabel("");
        m_rowLabel[k].clear();
        return;
    }
    const Item& it = m_pages[m_page][k];
    if (it.vi < 0) {
        m_key[k].setLabel("");
        m_rowLabel[k].set(eden_keybind_group(m_vis[it.head]), GLW::du(20), UITextAlignmentLeft);
        return;
    }
    const int mi = m_vis[it.vi];

    const char* primary = eden_keybind_code_label(eden_keybind_get(mi));
    m_key[k].setLabel(primary && primary[0] ? primary : "--", GLW::du(20));

    const int sec = eden_keybind_secondary(mi);
    if (sec != 0) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "also %s", eden_keybind_code_label(sec));
        // LEFT-aligned at a fixed offset from the button: Label resolves right alignment inside
        // its POT texture, which is wider than any box it is given, so "right" ran under the key.
        m_secLabel[k].set(buf, GLW::du(18), UITextAlignmentLeft);
    }
    m_rowLabel[k].set(eden_keybind_label(mi), GLW::du(22), UITextAlignmentLeft);
}

void KeybindsMenu::layout() {
    using namespace GLW;
    const float pad = du(12), gap = du(10);

    // The window sits behind the content (SettingsMenu::layout, the user's 2026-10-05 note): a
    // capped column, as tall as the longest page, centred.
    float cw = du(500);
    const float maxCw = SCREEN_WIDTH - du(12) * 2.0f - pad * 2.0f;
    if (cw > maxCw) cw = maxCw;
    const float pagerH = (m_pages.size() > 1) ? m_btnH + gap : 0.0f;
    const float ph = pad + m_btnH + gap + m_maxPageLen * m_pitch + pagerH + pad;
    const float pw = cw + pad * 2.0f;
    m_panel = CGRectMake((SCREEN_WIDTH - pw) * 0.5f, (SCREEN_HEIGHT - ph) * 0.5f, pw, ph);
    m_content = CGRectMake(m_panel.origin.x + pad, m_panel.origin.y, cw, ph);
    const float left = m_content.origin.x, right = left + cw;

    // Titlebar: Back · Controls · Defaults — the Settings screen's shape, so the two read as one.
    const float barY = m_panel.origin.y + ph - pad - m_btnH;
    m_back.setRect(CGRectMake(left, barY, du(80), m_btnH));
    m_reset.setRect(CGRectMake(right - du(104), barY, du(104), m_btnH));
    m_titleY = barY + (m_btnH + m_title.height()) * 0.5f;

    const float pagerY = m_panel.origin.y + pad;
    m_prev.setRect(CGRectMake(left, pagerY, du(40), m_btnH));
    m_next.setRect(CGRectMake(right - du(40), pagerY, du(40), m_btnH));

    // Key buttons are compact art; on touch the hit box is the row's full height.
    const float top = barY - gap;
    const float ch = std::min(m_pitch - du(6), std::max(du(24), touchFloor() * 0.6f));
    const float keyW = du(96);
    for (int k = 0; k < KB_ROWS_PER_PAGE; ++k) {
        const float rowTop = top - k * m_pitch;
        m_slot[k] = CGRectMake(left, rowTop - m_pitch, cw, m_pitch);
        const float cy = rowTop - m_pitch * 0.5f;
        m_key[k].setRect(CGRectMake(right - du(4) - keyW, cy - ch * 0.5f, keyW, ch));
        m_key[k].setHitRect(CGRectMake(right - du(4) - keyW, rowTop - m_pitch, keyW, m_pitch));
    }
}

void KeybindsMenu::update(float etime) {
    (void)etime;
    if (!m_active) return;
    fit();
    build();
    if (m_laidOutPage != m_page) {
        m_laidOutPage = m_page;
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%d / %d", m_page + 1, (int)m_pages.size());
        m_pageLabel.set(buf, GLW::du(20), UITextAlignmentCenter);
        for (int k = 0; k < KB_ROWS_PER_PAGE; ++k) refreshKeyLabel(k);
    }
    layout();

    Input* input = Input::getInput();
    itouch* touches = input->getTouches();
    const int pages = (int)m_pages.size();
    const std::vector<Item>& page = m_pages[m_page];
    const int nItems = (int)page.size();

    // While a capture is armed the list is inert: a touch cancels it and does nothing else. The
    // alternative — letting a tap both cancel and activate what is under it — means the tap that
    // gets you out of "press a key" also rebinds whatever row you happened to hit.
    const bool capturing = (eden_keybind_capture_active() >= 0);

    for (int i = 0; i < MAX_TOUCHES; ++i) {
        if (touches[i].inuse == 0 && touches[i].down == M_DOWN) {
            touches[i].inuse = usage_id;
            if (capturing) continue;
            const float mx = touches[i].mx, my = touches[i].my;
            m_back.setPressed(m_back.hit(mx, my));
            m_reset.setPressed(m_reset.hit(mx, my));
            m_prev.setPressed(m_page > 0 && m_prev.hit(mx, my));
            m_next.setPressed(m_page < pages - 1 && m_next.hit(mx, my));
            for (int k = 0; k < nItems; ++k)
                m_key[k].setPressed(page[k].vi >= 0 && m_key[k].hit(mx, my));
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
            for (int k = 0; k < nItems; ++k) {
                if (page[k].vi < 0) continue;
                if (m_key[k].hit(mx, my)) { eden_keybind_capture_begin(m_vis[page[k].vi]); break; }
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
    fit();
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

    m_back.render();
    m_reset.render();
    m_title.drawCentered(m_panel.origin.x + m_panel.size.width * 0.5f, m_titleY, kText);

    const int pages = (int)m_pages.size();
    const std::vector<Item>& page = m_pages[m_page];
    for (int k = 0; k < (int)page.size() && k < KB_ROWS_PER_PAGE; ++k) {
        const CGRect r = m_slot[k];
        const float cy = r.origin.y + r.size.height * 0.5f;
        if (page[k].vi < 0) {
            // A heading sits on the bottom of its slot, next to the rows it names.
            m_rowLabel[k].draw(r.origin.x + du(2), r.origin.y + du(4) + m_rowLabel[k].lineHeight(),
                               kTextSecondary);
            continue;
        }
        // A CONTENT strip behind the whole row, so a label and its button read as one thing.
        bevel(r, BEVEL_CONTENT);
        m_rowLabel[k].draw(r.origin.x + du(10), cy + m_rowLabel[k].lineHeight() * 0.5f, kText);
        if (!m_secLabel[k].empty())
            m_secLabel[k].draw(m_key[k].rect().origin.x - du(96),
                               cy + m_secLabel[k].lineHeight() * 0.5f, kTextSecondary);
        m_key[k].render();
    }

    if (pages > 1) {
        if (m_page > 0)         m_prev.render();
        if (m_page < pages - 1) m_next.render();
        const CGRect pr = m_prev.rect();
        m_pageLabel.drawCentered(m_content.origin.x + m_content.size.width * 0.5f,
                                 pr.origin.y + (pr.size.height + m_pageLabel.height()) * 0.5f,
                                 kTextSecondary);
    }

    // The capture overlay is a second scrim over the whole screen rather than a badge on the row:
    // a capture swallows the NEXT key press wherever it lands, so the modal state has to look
    // modal or the player will keep typing at a screen that is no longer listening to them.
    if (eden_keybind_capture_active() >= 0) {
        fill(CGRectMake(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT), kScrim);
        const float bh = du(56);
        const CGRect box = CGRectMake(m_panel.origin.x + du(20), (SCREEN_HEIGHT - bh) * 0.5f,
                                      m_panel.size.width - du(40), bh);
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
