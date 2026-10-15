//
//  KeybindsMenu.mm
//  Eden
//
//  See KeybindsMenu.h. Since U.4 this is the Keys tab's content inside SettingsMenu, not a screen
//  of its own: no scrim, panel, titlebar or touch claim here — SettingsMenu owns all four.
//
#import "KeybindsMenu.h"
#import "Globals.h"
#import "Util.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

extern float SCREEN_WIDTH;
extern float SCREEN_HEIGHT;

// The Stage 5.2 model, in the shared seam file port/src/seam/Settings_web.mm.
extern "C" {
int  eden_keybind_count(void);
const char* eden_keybind_label(int i);
const char* eden_keybind_group(int i);
int  eden_keybind_get(int i);
int  eden_keybind_secondary(int i);
int  eden_keybind_native_hidden(int i);
const char* eden_keybind_code_label(int code);
void eden_keybind_capture_begin(int i);
void eden_keybind_capture_cancel(void);
int  eden_keybind_capture_active(void);
}

KeybindsMenu::KeybindsMenu() {
    m_built = false;
    m_wasCapturing = false;
    m_builtU = 0.0f;
    m_pressed = -1;
    m_content = CGRectMake(0, 0, 0, 0);
}

KeybindsMenu::~KeybindsMenu() {}

// The key button's art: compact, like a settings stepper (29u); on touch its hit box is the row.
static float kb_keyW() { return GLW::du(96); }
static float kb_keyH() { return GLW::du(29); }

float KeybindsMenu::headingHeight() const { return GLW::du(30); }

void KeybindsMenu::build() {
    using namespace GLW;
    if (m_built && m_builtU == u()) return;
    m_built = true;
    m_builtU = u();
    m_pressed = -1;

    // Items: a heading wherever the group changes, then its rows. Rows with no native
    // implementation (fullscreen, settings) are hidden, as eden_settings_native_hidden() does.
    m_items.clear();
    m_hint.clear();
    const char* curGroup = NULL;
    const int n = eden_keybind_count();
    for (int i = 0; i < n && (int)m_items.size() < KB_MAX_ITEMS - 1; ++i) {
        if (eden_keybind_native_hidden(i)) continue;
        const char* g = eden_keybind_group(i);
        if (!curGroup || std::strcmp(g, curGroup) != 0) {
            Item h = { -1, g };
            m_items.push_back(h);
            m_hint.push_back("");
            curGroup = g;
        }
        Item it = { i, g };
        m_items.push_back(it);
        // THE SECONDARY IS NOT ON THE BUTTON (Stage 5.3): a binding the player cannot change does
        // not belong inside the control that changes bindings. It is the row's second line.
        const int sec = eden_keybind_secondary(i);
        char buf[64] = "";
        if (sec != 0) std::snprintf(buf, sizeof(buf), "also %s", eden_keybind_code_label(sec));
        m_hint.push_back(buf);
    }
    for (int k = 0; k < (int)m_items.size(); ++k) {
        if (m_items[k].mi < 0) m_head[k].set(m_items[k].group, du(20), UITextAlignmentLeft);
        else m_head[k].clear();
    }
    m_capture.set("Press a key -- Esc to cancel", du(24), UITextAlignmentCenter);
    m_scroll.setFirst(0);
    refresh();
}

void KeybindsMenu::refresh() {
    for (int k = 0; k < (int)m_items.size(); ++k) {
        if (m_items[k].mi < 0) { m_key[k].setLabel(""); continue; }
        const char* primary = eden_keybind_code_label(eden_keybind_get(m_items[k].mi));
        m_key[k].setLabel(primary && primary[0] ? primary : "--", GLW::du(20));
    }
}

void KeybindsMenu::layout(CGRect content, CGRect bar) {
    using namespace GLW;
    build();
    m_content = content;
    const int n = (int)m_items.size();
    float heights[KB_MAX_ITEMS];
    for (int k = 0; k < n; ++k) {
        if (m_items[k].mi < 0) { heights[k] = headingHeight(); continue; }
        m_row[k].build(eden_keybind_label(m_items[k].mi), m_hint[k].c_str(), content.size.width,
                       kb_keyW(), kb_keyH());
        m_row[k].setLast(k == n - 1 || m_items[k + 1].mi < 0);
        heights[k] = m_row[k].height();
    }
    m_scroll.setBarRect(bar);
    m_scroll.setRowHeights(heights, n, content.size.height);

    const bool touch = touchFloor() > 0.0f;
    for (int k = 0; k < n; ++k) {
        if (!shown(k) || m_items[k].mi < 0) continue;
        m_row[k].setOrigin(content.origin.x, content.origin.y + content.size.height - m_scroll.rowOffset(k));
        const CGRect cr = m_row[k].controlRect(kb_keyW(), kb_keyH());
        m_key[k].setRect(cr);
        const CGRect rr = m_row[k].rect();
        m_key[k].setHitRect(touch ? CGRectMake(cr.origin.x, rr.origin.y, cr.size.width, rr.size.height)
                                  : CGRectMake(0, 0, 0, 0));
    }
}

bool KeybindsMenu::shown(int k) const {
    return k >= m_scroll.first() && k < m_scroll.first() + m_scroll.visible() && k < (int)m_items.size();
}

bool KeybindsMenu::down(float mx, float my) {
    for (int k = 0; k < (int)m_items.size(); ++k) {
        if (!shown(k) || m_items[k].mi < 0 || !m_key[k].hit(mx, my)) continue;
        m_pressed = k;
        m_key[k].setPressed(true);
        return true;
    }
    return false;
}

void KeybindsMenu::up(float mx, float my) {
    const int k = m_pressed;
    clearPressed();
    if (k >= 0 && shown(k) && m_key[k].hit(mx, my)) eden_keybind_capture_begin(m_items[k].mi);
}

void KeybindsMenu::clearPressed() {
    if (m_pressed >= 0) m_key[m_pressed].setPressed(false);
    m_pressed = -1;
}

bool KeybindsMenu::keyRect(int mi, CGRect* r) {
    for (int k = 0; k < (int)m_items.size(); ++k) {
        if (m_items[k].mi != mi) continue;
        m_scroll.ensureVisible(k);
        layout(m_content, m_scroll.barRect());
        if (r) *r = m_key[k].rect();
        return true;
    }
    return false;
}

bool KeybindsMenu::capturing() const { return eden_keybind_capture_active() >= 0; }

void KeybindsMenu::cancelCapture() { eden_keybind_capture_cancel(); }

// A capture that resolved (the platform fed a key into the model) leaves stale button text.
// Polling one int per frame is cheaper than a callback and cannot get out of sync with a capture
// the model cancelled on its own.
void KeybindsMenu::poll() {
    const bool c = capturing();
    if (m_wasCapturing && !c) refresh();
    m_wasCapturing = c;
}

void KeybindsMenu::render() {
    using namespace GLW;
    for (int k = 0; k < (int)m_items.size(); ++k) {
        if (!shown(k)) continue;
        if (m_items[k].mi < 0) {
            // A heading sits on the bottom of its slot, next to the rows it names.
            const float top = m_content.origin.y + m_content.size.height - m_scroll.rowOffset(k);
            m_head[k].draw(m_content.origin.x + du(5), top - headingHeight() + du(4) + m_head[k].lineHeight(),
                           kTextSecondary);
            continue;
        }
        m_row[k].render();
        m_key[k].render();
    }
    m_scroll.render();
}

// The capture overlay is a second scrim over the whole screen rather than a badge on the row: a
// capture swallows the NEXT key press wherever it lands, so the modal state has to look modal or
// the player will keep typing at a screen that is no longer listening to them.
void KeybindsMenu::renderCapture(CGRect panel) {
    using namespace GLW;
    if (!capturing()) return;
    fill(CGRectMake(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT), kScrim);
    const float bh = du(56), bw = std::min<float>(panel.size.width - du(40), du(480));
    const CGRect box = CGRectMake(panel.origin.x + (panel.size.width - bw) * 0.5f, (SCREEN_HEIGHT - bh) * 0.5f, bw, bh);
    bevel(box, BEVEL_WINDOW);
    m_capture.drawCentered(box.origin.x + box.size.width * 0.5f,
                           box.origin.y + box.size.height * 0.5f + m_capture.height() * 0.5f, kText);
}
