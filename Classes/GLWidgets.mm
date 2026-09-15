//
//  GLWidgets.mm
//  Eden
//
//  See GLWidgets.h. This is web/docs/design-system.md expressed in Graphics::drawRect fills —
//  which is the whole reason the kit was affordable: every bevel in the system is a stack of
//  hard-edged rectangles (zero blur, square corners), so it needs NO new atlas art and no new
//  shader. A "shadow" is a rectangle drawn offset behind the face; an "inset highlight" is two
//  strips along the far edges; a keyline is a four-strip border.
//
//  THE ONE THING TO GET RIGHT WHEN EDITING: y is UP here and DOWN in CSS. Every token from the
//  stylesheet that says "down-right" (`--eden-drop-*`) is drawn at -y, and every `inset a a`
//  (from the top-left) is a strip along the TOP and LEFT edges, which in this space is +y and -x.
//
#import "GLWidgets.h"
#import "Graphics.h"
#import "Globals.h"
#import "Util.h"

#include <cmath>
#include <cstring>
#include <string>

extern float SCREEN_WIDTH;
extern float SCREEN_HEIGHT;

namespace GLW {

// This port always runs the "2x UI scale" flags (IS_IPAD/IS_RETINA true, SCALE_* == 2 — see
// DisplayProfile_web.mm and CLAUDE.md convention #3). Graphics::drawRect draws in the RAW ortho
// and Texture2D::drawText scales only a rect's ORIGIN, so both need this factor; callers of the
// kit never see it.
static float uiScale() { return IS_IPAD ? SCALE_WIDTH : 1.0f; }

// --- scale ---------------------------------------------------------------------------------
// min(w/783, h/587) is the CSS rule verbatim. The clamp is not: the web clamps to 1..2.4 because
// its denominator is raw CSS pixels, and this one is point space, which the display profile has
// already normalised to ~640 points tall. So the useful range is much narrower, and the floor is
// there for the 320x240 minimum profile rather than for phones.
float u() {
    float uw = SCREEN_WIDTH  / 783.0f;
    float uh = SCREEN_HEIGHT / 587.0f;
    float v = (uw < uh) ? uw : uh;
    if (v < 0.70f) v = 0.70f;
    if (v > 2.40f) v = 2.40f;
    return v;
}

const float kTouchFloor = 44.0f;

// --- palette -------------------------------------------------------------------------------
Color rgb(unsigned int hex, float a) {
    Color c;
    c.r = ((hex >> 16) & 0xFF) / 255.0f;
    c.g = ((hex >>  8) & 0xFF) / 255.0f;
    c.b = ( hex        & 0xFF) / 255.0f;
    c.a = a;
    return c;
}
static Color C(unsigned int hex, float a = 1.0f) { return rgb(hex, a); }

const Color kScrim         = { 12/255.0f, 7/255.0f, 4/255.0f, 0.55f };  // --eden-scrim
const Color kWindowFace    = { 0.788f, 0.788f, 0.788f, 0.90f };         // #c9c9c9 @ 90%
const Color kContentFace   = { 1.0f, 1.0f, 1.0f, 1.0f };
const Color kFaceTop       = { 1.0f, 1.0f, 1.0f, 1.0f };               // --eden-face-top
const Color kFaceBottom    = { 0.788f, 0.788f, 0.788f, 1.0f };         // #c9c9c9
const Color kFacePressed   = { 0.549f, 0.549f, 0.549f, 1.0f };         // #8c8c8c
const Color kKeyline       = { 0.392f, 0.392f, 0.392f, 1.0f };         // #646464
const Color kWindowKeyline = { 0.251f, 0.251f, 0.251f, 1.0f };         // #404040
const Color kText          = { 0.0f, 0.0f, 0.0f, 1.0f };
const Color kTextSecondary = { 0.392f, 0.392f, 0.392f, 1.0f };         // #646464
const Color kTextPressed   = { 0.788f, 0.788f, 0.788f, 1.0f };         // #c9c9c9
const Color kWhite         = { 1.0f, 1.0f, 1.0f, 1.0f };
const Color kBlack         = { 0.0f, 0.0f, 0.0f, 1.0f };

// --- primitives ----------------------------------------------------------------------------
void fill(float x, float y, float w, float h, Color c) {
    if (w <= 0.0f || h <= 0.0f) return;
    const float s = uiScale();
    glDisable(GL_TEXTURE_2D);
    glColor4f(c.r, c.g, c.b, c.a);
    Graphics::drawRect(x * s, y * s, (x + w) * s, (y + h) * s);
    glEnable(GL_TEXTURE_2D);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
}

void fill(CGRect r, Color c) { fill(r.origin.x, r.origin.y, r.size.width, r.size.height, c); }

// A `t`-thick border drawn INSIDE r, like CSS `box-shadow: inset 0 0 0 t`.
static void border(CGRect r, float t, Color c) {
    const float x = r.origin.x, y = r.origin.y, w = r.size.width, h = r.size.height;
    fill(x, y, w, t, c);                 // bottom
    fill(x, y + h - t, w, t, c);         // top
    fill(x, y, t, h, c);                 // left
    fill(x + w - t, y, t, h, c);         // right
}

// CSS `inset a a` — a shadow cast from the TOP-LEFT corner inward (+y / -x here).
static void insetTopLeft(CGRect r, float t, Color c) {
    const float x = r.origin.x, y = r.origin.y, w = r.size.width, h = r.size.height;
    fill(x, y + h - t, w, t, c);
    fill(x, y, t, h, c);
}

// CSS `inset -a -a` — cast from the BOTTOM-RIGHT inward. The design system's fake top-left light
// source: the far corner is the one that darkens.
static void insetBottomRight(CGRect r, float t, Color c) {
    const float x = r.origin.x, y = r.origin.y, w = r.size.width, h = r.size.height;
    fill(x, y, w, t, c);
    fill(x + w - t, y, t, h, c);
}

// The 6% gradient: NOT a blend, a hard one-stop fake top-edge highlight. See design-system.md.
static void buttonFace(CGRect r) {
    const float band = r.size.height * 0.06f;
    fill(r.origin.x, r.origin.y, r.size.width, r.size.height - band, kFaceBottom);
    fill(r.origin.x, r.origin.y + r.size.height - band, r.size.width, band, kFaceTop);
}

void bevel(CGRect r, BevelStyle style) {
    const float U = u();
    switch (style) {
    case BEVEL_RAISED:
        fill(r.origin.x + 2*U, r.origin.y - 2*U, r.size.width, r.size.height, C(0x000000, 0.25f));
        buttonFace(r);
        insetBottomRight(r, 3*U, C(0x646464, 0.47f));
        border(r, 2*U, kKeyline);
        break;
    case BEVEL_PRESSED:
        fill(r, kFacePressed);
        insetTopLeft(r, 4*U, C(0x000000, 0.50f));
        border(r, 2*U, kKeyline);
        break;
    case BEVEL_SUNKEN:
        // Deliberately draws NO face — a sunken control is a hole in whatever is behind it, and
        // its fill (track colour, field colour) belongs to the caller.
        insetTopLeft(r, 3*U, C(0x2c2b2b, 0.47f));
        border(r, 1*U, kKeyline);
        break;
    case BEVEL_WINDOW:
        fill(r.origin.x + 4*U, r.origin.y - 4*U, r.size.width, r.size.height, C(0x000000, 0.50f));
        fill(r, kWindowFace);
        border(r, 3*U, kWindowKeyline);
        break;
    case BEVEL_CONTENT:
        fill(r, kContentFace);
        border(r, 1*U, kKeyline);
        break;
    }
}

// --- text ----------------------------------------------------------------------------------
// The raster seam (eden_rasterize_text_rgba / the web canvas twin) exposes no measuring call, so
// wrapping estimates a line's width as chars * kAvgGlyph * pt. 0.52 is a sans-serif average; it
// over-estimates all-caps and under-estimates "iiii", which for dialog body copy costs at worst
// one extra break. If a measure ever lands in the seam, use it here and delete this constant.
static const float kAvgGlyph = 0.52f;

static int potAtLeast(int v) { int p = 32; while (p < v && p < 2048) p <<= 1; return p; }

Label::Label() : m_pt(15.0f), m_texW(512), m_texH(64), m_align(UITextAlignmentCenter), m_boxW(0) {}
Label::~Label() { clear(); }

void Label::clear() {
    for (size_t i = 0; i < m_lines.size(); i++) if (m_lines[i]) delete m_lines[i];
    m_lines.clear();
}

float Label::lineHeight() const { return m_pt * 1.28f; }
float Label::height() const     { return m_lines.size() * lineHeight(); }

void Label::set(const char* text, float pt, UITextAlignment align, float maxWidth) {
    clear();
    m_pt = pt;
    m_align = align;
    m_boxW = maxWidth;
    if (!text || !*text) return;

    const float glyph = kAvgGlyph * pt;
    const int perLine = (maxWidth > 0.0f && glyph > 0.0f) ? (int)(maxWidth / glyph) : 1 << 20;

    // Greedy word wrap. A single word longer than the line is left long rather than hyphenated —
    // it is a world name or a button label, and clipping one is better than inventing a break.
    std::vector<std::string> lines;
    std::string cur;
    const char* p = text;
    while (*p) {
        const char* e = p;
        while (*e && *e != ' ') e++;
        std::string word(p, e - p);
        if (cur.empty())                                       cur = word;
        else if ((int)(cur.size() + 1 + word.size()) <= perLine) cur += " " + word;
        else { lines.push_back(cur); cur = word; }
        p = (*e == ' ') ? e + 1 : e;
    }
    if (!cur.empty()) lines.push_back(cur);
    if (lines.empty()) return;

    size_t widest = 0;
    for (size_t i = 0; i < lines.size(); i++) if (lines[i].size() > widest) widest = lines[i].size();

    const float s = uiScale();
    // POT on purpose: initFromString centres/aligns inside a POT buffer but drawText samples only
    // the rect asked for, so a non-POT width silently offsets centred text (the trap SettingsMenu
    // and GLDialog both carry a comment about). Sized in PIXELS, which is pt * scale.
    m_texW = potAtLeast((int)std::ceil(widest * glyph * s) + 16);
    m_texH = potAtLeast((int)std::ceil(pt * s * 1.6f));

    for (size_t i = 0; i < lines.size(); i++) {
        m_lines.push_back(new Texture2D([NSString stringWithUTF8String:lines[i].c_str()],
                                        CGSizeMake(m_texW, m_texH), align,
                                        [UIFont systemFontOfSize:pt * s]));
    }
}

// drawText scales the rect ORIGIN by SCALE_* but draws the texture at its raw pixel extent, so
// the origin that lands a m_texH-pixel texture centred on a point-space row is derived here.
void Label::blit(Texture2D* t, float x, float yTop) const {
    if (!t) return;
    const float s = uiScale();
    t->drawText(CGRectMake(x, yTop - lineHeight() * 0.5f - m_texH / (2.0f * s), m_texW, m_texH));
}

void Label::draw(float x, float yTop, Color c) const {
    glColor4f(c.r, c.g, c.b, c.a);
    for (size_t i = 0; i < m_lines.size(); i++) blit(m_lines[i], x, yTop - i * lineHeight());
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
}

void Label::drawCentered(float cx, float yTop, Color c) const {
    draw(cx - m_texW / (2.0f * uiScale()), yTop, c);
}

void Label::drawChrome(float cx, float yTop, Color c, Color shadow) const {
    const float o = u();          // the system's 1u text shadow, down-right => +x / -y
    drawCentered(cx + o, yTop - o, shadow);
    drawCentered(cx, yTop, c);
}

// --- Button --------------------------------------------------------------------------------
Button::Button() : m_pt(15.0f), m_pressed(false) { m_rect = CGRectMake(0, 0, 0, 0); }
Button::~Button() {}

// The label is only rasterised once the box is known — its wrap width is the box's. A caller
// therefore sets text and point size first and the rect last, and every setter is idempotent.
void Button::rebuildLabel() {
    if (m_text.empty() || m_rect.size.width <= 0.0f) { m_label.clear(); return; }
    m_label.set(m_text.c_str(), m_pt, UITextAlignmentCenter, m_rect.size.width - du(16));
}

void Button::setLabel(const char* text, float pt) {
    m_pt = pt;
    m_text = text ? text : "";
    rebuildLabel();
}

void Button::setPointSize(float pt) { m_pt = pt; rebuildLabel(); }

void Button::setRect(CGRect r) { m_rect = r; rebuildLabel(); }

bool Button::hit(float x, float y) const {
    return x >= m_rect.origin.x && x <= m_rect.origin.x + m_rect.size.width &&
           y >= m_rect.origin.y && y <= m_rect.origin.y + m_rect.size.height;
}

void Button::render() const {
    bevel(m_rect, m_pressed ? BEVEL_PRESSED : BEVEL_RAISED);
    if (m_label.empty()) return;
    const float cx   = m_rect.origin.x + m_rect.size.width * 0.5f;
    const float yTop = m_rect.origin.y + (m_rect.size.height + m_label.height()) * 0.5f;
    // "Pressed == selected" inverts BOTH the label and its shadow — that inversion is the cue.
    if (m_pressed) m_label.drawChrome(cx, yTop, kTextPressed, kBlack);
    else           m_label.drawChrome(cx, yTop, kText,        kWhite);
}

}  // namespace GLW
