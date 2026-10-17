//
//  GLWidgets.mm
//  Eden
//
//  See GLWidgets.h. This is web-port-final:web/docs/design-system.md expressed in Graphics::drawRect fills —
//  which is the whole reason the kit was affordable: every bevel in the system is a stack of
//  hard-edged rectangles (zero blur, square corners), so it needs NO new atlas art (the icons
//  aside — U.1's media/ui/ui_icons.png) and no new shader. A "shadow" is a rectangle drawn offset behind the face; an "inset highlight" is two
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

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

extern float SCREEN_WIDTH;
extern float SCREEN_HEIGHT;
extern "C" float eden_ui_raster_density(void);   // port/src/seam/DisplayProfile_web.mm

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

extern "C" int eden_effective_input_is_touch(void);   // port/src/seam/Settings_web.mm (both targets)
float touchFloor() { return eden_effective_input_is_touch() ? kTouchFloor : 0.0f; }

// A hit box: the explicit one if a screen set it, else the drawn rect.
static bool inBox(CGRect drawn, CGRect hitBox, float x, float y) {
    const CGRect r = (hitBox.size.width > 0.0f && hitBox.size.height > 0.0f) ? hitBox : drawn;
    return x >= r.origin.x && x <= r.origin.x + r.size.width &&
           y >= r.origin.y && y <= r.origin.y + r.size.height;
}

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
const Color kTextMuted     = { 0x97/255.0f, 0x97/255.0f, 0x97/255.0f, 1.0f };   // --eden-gray-300
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

// --- icons (U.1) ---------------------------------------------------------------------------
// Loaded once, on first use, and never freed (it is 256 KB of texture for the life of the app).
// LINEAR, unlike the engine's NEAREST art: a cell is 64 px of antialiased stroke drawn at anything
// from ~16u to ~40u, so it is minified on some screens and magnified on others, and NEAREST
// staircases a 2.4-wide stroke either way. CLAMP_TO_EDGE so the outer cells' edges do not sample
// the opposite side of the atlas.
static Texture2D* s_icons = NULL;

static Texture2D* iconAtlas() {
    if (!s_icons) {
        // genMips TRUE: the LINEAR_MIPMAP_LINEAR below needs the chain. Since P.15 a non-mip load
        // builds none, and the atlas sampled as an incomplete texture — every icon a black square
        // (found by U.5's shots, 2026-10-14).
        s_icons = new Texture2D(@"ui_icons.png", FALSE, TRUE);
        glBindTexture(GL_TEXTURE_2D, s_icons->name);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    return s_icons;
}

void icon(int id, CGRect r, Color c) {
    if (r.size.width <= 0.0f || r.size.height <= 0.0f) return;
    if (id < 0 || id >= UIICON_COUNT) id = UIICON_FALLBACK;
    Texture2D* t = iconAtlas();
    if (!t || !t->name) return;
    // t = 0 is the PNG's TOP row (Texture2D::drawInRect's convention), and y is up here, so the
    // quad's top edge takes the cell's t0.
    const float cell = 1.0f / UIICON_GRID;
    const float s0 = UIIconCol(id) * cell, s1 = s0 + cell;
    const float t0 = UIIconRow(id) * cell, t1 = t0 + cell;
    const float k = uiScale();
    const float x0 = r.origin.x * k, y0 = r.origin.y * k;
    const float x1 = (r.origin.x + r.size.width) * k, y1 = (r.origin.y + r.size.height) * k;
    const GLfloat v[] = { x0, y0, 0,  x1, y0, 0,  x0, y1, 0,  x1, y1, 0 };
    const GLfloat uv[] = { s0, t1,  s1, t1,  s0, t0,  s1, t0 };
    glColor4f(c.r, c.g, c.b, c.a);
    glBindTexture(GL_TEXTURE_2D, t->name);
    glVertexPointer(3, GL_FLOAT, 0, v);
    glTexCoordPointer(2, GL_FLOAT, 0, uv);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
}

// --- text ----------------------------------------------------------------------------------
// The raster seam (eden_rasterize_text_rgba / the web canvas twin) exposes no measuring call, so
// wrapping estimates a line's width as chars * kAvgGlyph * pt. 0.52 is a sans-serif average; it
// over-estimates all-caps and under-estimates "iiii", which for dialog body copy costs at worst
// one extra break. If a measure ever lands in the seam, use it here and delete this constant.
// The display face's own figure: Jersey 10 measures 0.34 em on mixed-case labels (stb_truetype,
// 2026-10-05; Arial measures 0.41 against the 0.52 above), so 0.42 keeps the same headroom.
static const float kAvgGlyph = 0.52f;
static const float kAvgGlyphDisplay = 0.42f;

static int potAtLeast(int v) { int p = 32; while (p < v && p < 2048) p <<= 1; return p; }

Label::Label() : m_pt(15.0f), m_texW(512), m_texH(64), m_align(UITextAlignmentCenter), m_boxW(0),
                 m_face(FACE_DISPLAY), m_width(0) {}
Label::~Label() { clear(); }

void Label::clear() {
    for (size_t i = 0; i < m_lines.size(); i++) if (m_lines[i]) delete m_lines[i];
    m_lines.clear();
    m_width = 0;
}

float Label::lineHeight() const { return m_pt * 1.28f; }
float Label::height() const     { return m_lines.size() * lineHeight(); }

void Label::set(const char* text, float pt, UITextAlignment align, float maxWidth, Face face,
                int keepLastLines) {
    clear();
    m_pt = pt;
    m_align = align;
    m_boxW = maxWidth;
    m_face = face;
    if (!text || !*text) return;

    const float glyph = (face == FACE_DISPLAY ? kAvgGlyphDisplay : kAvgGlyph) * pt;
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
        // The field's mode: a word too long for any line is cut into line-sized pieces.
        while (keepLastLines > 0 && perLine > 1 && (int)word.size() > perLine) {
            const int room = cur.empty() ? perLine : perLine - (int)cur.size() - 1;
            if (room <= 0) { lines.push_back(cur); cur.clear(); continue; }
            cur += (cur.empty() ? "" : " ") + word.substr(0, (size_t)room);
            lines.push_back(cur);
            cur.clear();
            word = word.substr((size_t)room);
        }
        if (cur.empty())                                       cur = word;
        else if ((int)(cur.size() + 1 + word.size()) <= perLine) cur += " " + word;
        else { lines.push_back(cur); cur = word; }
        p = (*e == ' ') ? e + 1 : e;
    }
    if (!cur.empty()) lines.push_back(cur);
    if (lines.empty()) return;
    if (keepLastLines > 0 && (int)lines.size() > keepLastLines)
        lines.erase(lines.begin(), lines.end() - keepLastLines);

    size_t widest = 0;
    for (size_t i = 0; i < lines.size(); i++) if (lines[i].size() > widest) widest = lines[i].size();

    const float s = uiScale();
    // POT on purpose: initFromString centres/aligns inside a POT buffer but drawText samples only
    // the rect asked for, so a non-POT width silently offsets centred text (the trap SettingsMenu
    // and GLDialog both carry a comment about). Sized in PIXELS, which is pt * scale.
    m_texW = potAtLeast((int)std::ceil(widest * glyph * s) + 16);
    m_texH = potAtLeast((int)std::ceil(pt * s * 1.6f));
    // N.4.8: rasterised at the drawable's real density (texels per ortho unit), so text is 1:1
    // on device pixels instead of magnified through NEAREST. Layout above stays in ortho units.
    const float density = eden_ui_raster_density();

    // The face is a sticky seam switch, so it is set for exactly these rasters and put back:
    // every non-kit caller of the raster (statusbar.mm, SharedList.mm) expects the body face.
    eden_text_raster_set_face(face);
    for (size_t i = 0; i < lines.size(); i++) {
        m_lines.push_back(new Texture2D([NSString stringWithUTF8String:lines[i].c_str()],
                                        CGSizeMake(m_texW, m_texH), align,
                                        [UIFont systemFontOfSize:pt * s], density));
        // N.5.11: measured at the same pixel size the raster used, back to points.
        float w = eden_text_raster_measure(lines[i].c_str(), pt * s) / s;
        if (w <= 0.0f) w = lines[i].size() * glyph;
        if (w > m_width) m_width = w;
    }
    eden_text_raster_set_face(FACE_BODY);
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

// --- Toast (N.5.11) ------------------------------------------------------------------------
// eden-ui.css `.eden-toast`: padding 4u 12u, rgba(44,43,43,.85), a 2u gray-500 keyline,
// display-sm (22u) white text, a 200 ms opacity transition. The web one sits at top: 12%; this
// one sits near the bottom edge, where the stock status line was (the user's call, 2026-10-10).
static const float kToastFade  = 0.2f;
static const float kToastNever = 1000.0f;   // statusbar.mm's `thresh`: never expires

static float toastMaxTextWidth() { return SCREEN_WIDTH - du(160); }

Toast::Toast() : m_life(0), m_age(0), m_builtU(0) {}

void Toast::show(const char* utf8, float seconds) {
    const std::string t = utf8 ? utf8 : "";
    if (t.empty()) { clear(); return; }
    const bool wasVisible = visible();
    m_life = seconds;
    if (wasVisible && t == m_text) return;
    if (!wasVisible) m_age = 0;
    m_text = t;
    m_builtU = u();
    m_label.set(m_text.c_str(), du(22), UITextAlignmentCenter, toastMaxTextWidth(), FACE_DISPLAY);
}

void Toast::clear() {
    m_text.clear();
    m_label.clear();
    m_life = 0;
}

void Toast::update(float etime) {
    if (m_text.empty()) return;
    m_age += etime;
    if (m_life < kToastNever) m_life -= etime;
    if (m_life <= 0) { clear(); return; }
    // The window was resized under a showing toast: re-wrap at the new scale.
    if (u() != m_builtU) {
        m_builtU = u();
        m_label.set(m_text.c_str(), du(22), UITextAlignmentCenter, toastMaxTextWidth(), FACE_DISPLAY);
    }
}

CGRect Toast::rect() const {
    float tw = m_label.width();
    if (tw > toastMaxTextWidth()) tw = toastMaxTextWidth();
    const float w = tw + 2 * du(12);
    const float h = m_label.height() + 2 * du(4);
    return CGRectMake((SCREEN_WIDTH - w) * 0.5f, du(16), w, h);
}

void Toast::render() const {
    if (!visible() || m_label.empty()) return;
    float a = m_age / kToastFade;
    if (m_life < kToastNever && m_life / kToastFade < a) a = m_life / kToastFade;
    if (a > 1.0f) a = 1.0f;
    if (a <= 0.0f) return;
    const CGRect r = rect();
    fill(r, C(0x2c2b2b, 0.85f * a));
    border(r, 2 * u(), C(0x646464, a));
    Color c = kWhite;
    c.a = a;
    m_label.drawCentered(r.origin.x + r.size.width * 0.5f, r.origin.y + r.size.height - du(4), c);
}

// --- Button --------------------------------------------------------------------------------
Button::Button() : m_pt(15.0f), m_pressed(false), m_enabled(true), m_tone(TONE_DEFAULT), m_icon(-1),
                   m_fitPad(-1.0f) {
    m_rect = CGRectMake(0, 0, 0, 0);
    m_hit = CGRectMake(0, 0, 0, 0);
}
Button::~Button() {}

// The label is only rasterised once the box is known — its wrap width is the box's. A caller
// therefore sets text and point size first and the rect last, and every setter is idempotent.
void Button::rebuildLabel() {
    // A shrink-wrapped button's width comes FROM the label, so its label never wraps.
    if (m_fitPad >= 0.0f) {
        if (m_text.empty()) m_label.clear();
        else m_label.set(m_text.c_str(), m_pt, UITextAlignmentCenter);
        m_rect.size.width = fitWidth();
        return;
    }
    if (m_text.empty() || m_rect.size.width <= 0.0f) { m_label.clear(); return; }
    m_label.set(m_text.c_str(), m_pt, UITextAlignmentCenter, m_rect.size.width - du(16));
}

// eden-ui.css `.eden-btn--sm`: 9u side padding, min-width 64u, a 16u icon 5u before the label.
static const float kBtnIcon = 16.0f, kBtnIconGap = 5.0f, kBtnMinW = 64.0f;

void Button::sizeToFit(float padX) {
    m_fitPad = padX < 0.0f ? 0.0f : padX;
    rebuildLabel();
}

float Button::fitWidth() const {
    float w = m_label.empty() ? 0.0f : m_label.width();
    if (m_icon >= 0) w += du(kBtnIcon) + (m_label.empty() ? 0.0f : du(kBtnIconGap));
    w += 2.0f * du(m_fitPad >= 0.0f ? m_fitPad : 9.0f);
    return std::max(w, du(kBtnMinW));
}

void Button::setLabel(const char* text, float pt) {
    m_pt = pt;
    m_text = text ? text : "";
    rebuildLabel();
}

void Button::setPointSize(float pt) { m_pt = pt; rebuildLabel(); }

// Only a WIDTH change re-wraps. Screens call setRect() from a per-frame layout(), and until 5.4
// this rebuilt the label every time — a texture upload (and, since N.4.9, a glFlush) per button
// per frame on every converted screen. Position changes need nothing: the label draws at render().
void Button::setRect(CGRect r) {
    if (m_fitPad >= 0.0f) {                   // shrink-wrapped: the width is the content's
        const bool rebuild = m_label.empty() && !m_text.empty();
        m_rect = r;
        m_rect.size.width = fitWidth();
        if (rebuild) rebuildLabel();
        return;
    }
    const bool rewrap = (r.size.width != m_rect.size.width) || m_label.empty();
    m_rect = r;
    if (rewrap) rebuildLabel();
}

bool Button::hit(float x, float y) const { return m_enabled && inBox(m_rect, m_hit, x, y); }

// eden-ui.css tones: positive is --eden-green-play under the 6% white band and --eden-green-500
// pressed; danger is --eden-red-100 / --eden-red-500 (whose pressed label goes white).
static const Color kPlayFace   = { 0xa9/255.0f, 0xe4/255.0f, 0xb0/255.0f, 1.0f };
static const Color kGreen500   = { 0x89/255.0f, 0xc3/255.0f, 0x1f/255.0f, 1.0f };
static const Color kRed100     = { 0xe8/255.0f, 0xa4/255.0f, 0x9b/255.0f, 1.0f };
static const Color kRed500     = { 0xc0/255.0f, 0x39/255.0f, 0x2b/255.0f, 1.0f };

void Button::render() const {
    bevel(m_rect, m_pressed ? BEVEL_PRESSED : BEVEL_RAISED);
    if (m_tone != TONE_DEFAULT) {
        // Re-face inside the keyline (2u) — the bevel's shadow, keyline and far-corner inset stay.
        const float U = u();
        const CGRect in = CGRectMake(m_rect.origin.x + 2*U, m_rect.origin.y + 2*U,
                                     m_rect.size.width - 4*U, m_rect.size.height - 4*U);
        if (m_pressed) {
            fill(in, m_tone == TONE_POSITIVE ? kGreen500 : kRed500);
            insetTopLeft(in, 2*U, C(0x000000, 0.50f));
        } else {
            const float band = m_rect.size.height * 0.06f;
            fill(in.origin.x, in.origin.y, in.size.width, in.size.height - band,
                 m_tone == TONE_POSITIVE ? kPlayFace : kRed100);
            insetBottomRight(in, 1*U, C(0x646464, 0.47f));
        }
    }
    // The icon takes the label's colour, so a press inverts it the way `currentColor` did.
    const Color ink = !m_enabled ? kKeyline
                    : m_pressed ? (m_tone == TONE_DANGER ? kWhite : kTextPressed) : kText;
    float cx = m_rect.origin.x + m_rect.size.width * 0.5f;
    if (m_icon >= 0) {
        if (m_label.empty()) {                // an icon tile: the icon IS the content
            const float sz = std::min(m_rect.size.width, m_rect.size.height) * 0.6f;
            icon(m_icon, CGRectMake(cx - sz * 0.5f, m_rect.origin.y + (m_rect.size.height - sz) * 0.5f,
                                    sz, sz), ink);
            return;
        }
        // Icon + label centred as one group; the label centres on what is left after the icon.
        const float isz = du(kBtnIcon), gap = du(kBtnIconGap);
        const float group = isz + gap + m_label.width();
        const float gx = cx - group * 0.5f;
        icon(m_icon, CGRectMake(gx, m_rect.origin.y + (m_rect.size.height - isz) * 0.5f, isz, isz), ink);
        cx = gx + isz + gap + m_label.width() * 0.5f;
    }
    if (m_label.empty()) return;
    const float yTop = m_rect.origin.y + (m_rect.size.height + m_label.height()) * 0.5f;
    // "Pressed == selected" inverts BOTH the label and its shadow — that inversion is the cue.
    if (!m_enabled) m_label.drawCentered(cx, yTop, kKeyline);
    else if (m_pressed) m_label.drawChrome(cx, yTop, m_tone == TONE_DANGER ? kWhite : kTextPressed, kBlack);
    else           m_label.drawChrome(cx, yTop, kText,        kWhite);
}

// --- Toggle --------------------------------------------------------------------------------
// eden-ui.css `.eden-toggle`: 104x34u, SUNKEN on the content face; each half is (50% - inset)
// wide. Checked: the ON half takes the lime 12% face (the toggle's own one-stop gradient, a
// sibling of the button's 6% one) and the OFF half goes white; unchecked, ON is white and OFF is
// dark grey (U.1: gray-500, after the reference; the CSS says gray-300).
static const Color kLimeTop    = { 0xc6/255.0f, 0xf1/255.0f, 0x75/255.0f, 1.0f };   // --eden-green-100
static const Color kLime       = { 0x89/255.0f, 0xc3/255.0f, 0x1f/255.0f, 1.0f };   // --eden-green-500
static const Color kTrack      = { 0x45/255.0f, 0x45/255.0f, 0x45/255.0f, 1.0f };   // --eden-surface-track
static const Color kPlaceholder = { 0.0f, 0.0f, 0.0f, 0.5f };                       // --eden-text-placeholder

static bool inRect(CGRect r, float x, float y) {
    return x >= r.origin.x && x <= r.origin.x + r.size.width &&
           y >= r.origin.y && y <= r.origin.y + r.size.height;
}

Toggle::Toggle() : m_on(false), m_muted(false) { m_rect = m_hit = CGRectMake(0, 0, 0, 0); }

float Toggle::designW() { return du(104); }
float Toggle::designH() { return du(34); }

bool Toggle::hit(float x, float y) const { return inBox(m_rect, m_hit, x, y); }

// U.1, from the reference (WORKING/referenceui/ Gameplay Settings): the pill reads as heavy next
// to a 32u title because its keyline is 2u of near-black and its off half is the dark gray-500,
// not the CSS's gray-300 (which is what the 5.4 toggle drew, and why it looked faint). The muted
// palette is the reference's "Hostile creatures" row: every part one step lighter, still opaque.
static const Color kToggleKeyline      = { 0x2c/255.0f, 0x2c/255.0f, 0x2c/255.0f, 1.0f };   // --eden-gray-800
static const Color kToggleOff          = { 0x64/255.0f, 0x64/255.0f, 0x64/255.0f, 1.0f };   // --eden-gray-500
static const Color kToggleMutedKeyline = { 0x97/255.0f, 0x97/255.0f, 0x97/255.0f, 1.0f };   // --eden-gray-300
static const Color kToggleMutedOff     = { 0xb4/255.0f, 0xb4/255.0f, 0xb4/255.0f, 1.0f };
static const Color kToggleMutedLime    = { 0xc4/255.0f, 0xe1/255.0f, 0x8f/255.0f, 1.0f };   // green-500 50% on white
static const Color kToggleMutedLimeTop = { 0xe2/255.0f, 0xf8/255.0f, 0xba/255.0f, 1.0f };   // green-100 50% on white

void Toggle::render() const {
    const float U = u();
    const CGRect r = m_rect;
    fill(r, kContentFace);
    // Halves sit inside the 2u keyline with a 1u white gutter, as in the reference.
    const float in = 3*U;
    const float x = r.origin.x, y = r.origin.y + in;
    const float hw = r.size.width * 0.5f - in, hh = r.size.height - 2*in;
    if (m_on) {
        const float band = hh * 0.12f;
        fill(x + in, y, hw, hh - band, m_muted ? kToggleMutedLime : kLime);
        fill(x + in, y + hh - band, hw, band, m_muted ? kToggleMutedLimeTop : kLimeTop);
    } else {
        fill(x + r.size.width - in - hw, y, hw, hh, m_muted ? kToggleMutedOff : kToggleOff);
    }
    // SUNKEN's recess (the inset shadow from the top-left) over the halves, as the CSS box-shadow
    // paints over the background; then the heavy keyline instead of SUNKEN's 1u one. Muted, the
    // recess goes too: it is what makes the control read as live.
    if (!m_muted) insetTopLeft(CGRectMake(r.origin.x + 2*U, r.origin.y + 2*U, r.size.width - 4*U,
                                          r.size.height - 4*U), 2*U, C(0x2c2b2b, 0.30f));
    border(r, 2*U, m_muted ? kToggleMutedKeyline : kToggleKeyline);
}

// --- Slider --------------------------------------------------------------------------------
Slider::Slider() : m_min(0), m_max(1), m_step(0), m_value(0), m_dragging(false) {
    m_rect = m_hit = CGRectMake(0, 0, 0, 0);
}

void Slider::setRange(float minV, float maxV, float step) {
    m_min = minV;
    m_max = (maxV > minV) ? maxV : minV;
    m_step = step > 0.0f ? step : 0.0f;
    setValue(m_value);
}

// Snapped from MIN, not from zero: a 0.25..3 range in 0.05 steps has to land on 0.25, 0.30 ...
// and rounding v/step would put it on 0.25 only by floating-point luck.
float Slider::snap(float v) const {
    if (v < m_min) v = m_min;
    if (v > m_max) v = m_max;
    if (m_step > 0.0f) {
        v = m_min + std::floor((v - m_min) / m_step + 0.5f) * m_step;
        if (v > m_max) v = m_max;
    }
    return v;
}

void Slider::setValue(float v) { m_value = snap(v); }

// The thumb's CENTRE travels the track inset by half a thumb at each end, so min and max put the
// thumb flush with the track's ends rather than half off them (what a native range input does).
// The CSS thumb is 16x24u on a 10u track. Those are CAPS here, not sizes: a compact row hands the
// slider a shorter box, and the thumb and track shrink with it in the same proportions.
static float thumbH(CGRect r) { return std::min(du(24), (float)r.size.height); }
static float thumbW(CGRect r) { return thumbH(r) * (16.0f / 24.0f); }

float Slider::valueAt(float x) const {
    const float tw = thumbW(m_rect);
    const float x0 = m_rect.origin.x + tw * 0.5f;
    const float span = m_rect.size.width - tw;
    if (span <= 0.0f || m_max <= m_min) return m_min;
    float t = (x - x0) / span;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return m_min + t * (m_max - m_min);
}

CGRect Slider::thumbRect() const {
    const float tw = thumbW(m_rect), th = thumbH(m_rect);
    const float span = m_rect.size.width - tw;
    const float t = (m_max > m_min) ? (m_value - m_min) / (m_max - m_min) : 0.0f;
    return CGRectMake(m_rect.origin.x + span * t,
                      m_rect.origin.y + (m_rect.size.height - th) * 0.5f, tw, th);
}

bool Slider::hit(float x, float y) const { return inBox(m_rect, m_hit, x, y); }

bool Slider::beginDrag(float x) {
    m_dragging = true;
    return dragTo(x);
}

bool Slider::dragTo(float x) {
    if (!m_dragging) return false;
    const float v = snap(valueAt(x));
    if (v == m_value) return false;
    m_value = v;
    return true;
}

void Slider::render() const {
    const float th = thumbH(m_rect) * (10.0f / 24.0f);
    const CGRect track = CGRectMake(m_rect.origin.x, m_rect.origin.y + (m_rect.size.height - th) * 0.5f,
                                    m_rect.size.width, th);
    fill(track, kTrack);
    bevel(track, BEVEL_SUNKEN);
    // The thumb is the small raised bevel (--bevel-raised-sm: 2u keyline, 2u drop, 2u highlight)
    // on its own 8% face, NOT BEVEL_RAISED's 3u highlight — at 16u wide the full one eats the face.
    const CGRect t = thumbRect();
    const float U = u();
    fill(t.origin.x + 2*U, t.origin.y - 2*U, t.size.width, t.size.height, C(0x000000, 0.25f));
    const float band = t.size.height * 0.08f;
    fill(t.origin.x, t.origin.y, t.size.width, t.size.height - band, kFaceBottom);
    fill(t.origin.x, t.origin.y + t.size.height - band, t.size.width, band, kFaceTop);
    insetBottomRight(t, 2*U, C(0x646464, 0.47f));
    border(t, 2*U, kKeyline);
}

// --- Stepper -------------------------------------------------------------------------------
Stepper::Stepper() : m_decGlyph("-"), m_incGlyph("+"), m_glyphPt(0.0f) { m_rect = CGRectMake(0, 0, 0, 0); }

// The glyphs are sized from the buttons (setRect), so a compact stepper gets compact glyphs.
void Stepper::setGlyphs(const char* dec, const char* inc) {
    m_decGlyph = dec ? dec : "-";
    m_incGlyph = inc ? inc : "+";
    m_glyphPt = 0.0f;                     // re-rasterise at the next setRect()
}

void Stepper::setValueText(const char* text, float pt) {
    m_value.set(text, pt, UITextAlignmentCenter);
}

// Buttons are square at the rect's height; the well takes what is left between them, with a 4u
// gap each side so the buttons' drop shadows do not land on the well's keyline.
void Stepper::setRect(CGRect r) {
    m_rect = r;
    const float b = r.size.height;
    m_dec.setRect(CGRectMake(r.origin.x, r.origin.y, b, b));
    m_inc.setRect(CGRectMake(r.origin.x + r.size.width - b, r.origin.y, b, b));
    // Only on a size change: setRect() runs every frame, and a label is a texture upload.
    const float pt = std::min(du(22), b * 0.9f);
    if (pt != m_glyphPt) {
        m_glyphPt = pt;
        m_dec.setLabel(m_decGlyph.c_str(), pt);
        m_inc.setLabel(m_incGlyph.c_str(), pt);
    }
}

void Stepper::setHitRect(CGRect h) {
    if (h.size.width <= 0.0f || h.size.height <= 0.0f) {
        m_dec.setHitRect(CGRectMake(0, 0, 0, 0));
        m_inc.setHitRect(CGRectMake(0, 0, 0, 0));
        return;
    }
    const CGRect d = m_dec.rect(), i = m_inc.rect();
    m_dec.setHitRect(CGRectMake(d.origin.x, h.origin.y, d.size.width, h.size.height));
    m_inc.setHitRect(CGRectMake(i.origin.x, h.origin.y, i.size.width, h.size.height));
}

void Stepper::render() const {
    const float b = m_rect.size.height, g = du(4);
    const CGRect well = CGRectMake(m_rect.origin.x + b + g, m_rect.origin.y,
                                   m_rect.size.width - 2*b - 2*g, m_rect.size.height);
    fill(well, kContentFace);
    bevel(well, BEVEL_SUNKEN);
    if (!m_value.empty())
        m_value.drawCentered(well.origin.x + well.size.width * 0.5f,
                             well.origin.y + (well.size.height + m_value.height()) * 0.5f, kText);
    m_dec.render();
    m_inc.render();
}

// --- ListRow -------------------------------------------------------------------------------
// color-mix(in srgb, --eden-green-100 45%, white): the CSS selection tint, resolved.
static const Color kSelectTint = { 0.45f*0xc6/255.0f + 0.55f, 0.45f*0xf1/255.0f + 0.55f,
                                   0.45f*0x75/255.0f + 0.55f, 1.0f };

ListRow::ListRow() : m_selected(false), m_last(false) { m_rect = CGRectMake(0, 0, 0, 0); }

void ListRow::setTitle(const char* utf8, float pt) { m_title.set(utf8, pt, UITextAlignmentLeft); }

bool ListRow::hit(float x, float y) const { return inRect(m_rect, x, y); }

void ListRow::render() const {
    const float U = u();
    const CGRect r = m_rect;
    if (m_selected) {
        fill(r, kSelectTint);
        fill(r.origin.x, r.origin.y, 4*U, r.size.height, kLime);
    }
    if (!m_last) fill(r.origin.x, r.origin.y, r.size.width, 1*U, C(0x000000, 0.28f));
    if (!m_title.empty())
        m_title.draw(r.origin.x + du(10), r.origin.y + (r.size.height + m_title.height()) * 0.5f, kText);
}

// --- ScrollView ----------------------------------------------------------------------------
ScrollView::ScrollView() : m_pitch(30.0f), m_total(0), m_visible(1), m_first(0), m_thumbDrag(false),
                           m_contentDrag(false), m_contentScrolling(false), m_dragY0(0), m_dragFirst0(0),
                           m_viewH(0) {
    m_bar = CGRectMake(0, 0, 0, 0);
}

void ScrollView::setRows(int total, int visible) {
    m_heights.clear();
    m_total = total > 0 ? total : 0;
    m_visible = visible > 0 ? visible : 1;
    setFirst(m_first);
}

void ScrollView::setRowHeights(const float* heights, int n, float viewH) {
    m_total = (n > 0 && heights) ? n : 0;
    m_heights.assign(heights, heights + m_total);
    m_viewH = viewH;
    if (m_heights.empty()) m_visible = 1;
    setFirst(m_first);
}

// Whole rows from `first` whose summed height fits the view; at least one, so a row taller than
// the view still shows (and overflows) instead of the list going blank.
int ScrollView::visibleFrom(int first) const {
    int n = 0;
    float used = 0.0f;
    for (int i = first; i < m_total; i++) {
        if (n > 0 && used + m_heights[i] > m_viewH + 0.01f) break;
        used += m_heights[i];
        n++;
    }
    return n > 0 ? n : 1;
}

// Variable mode: the first row from which everything to the end fits — scrolling stops with the
// last row at the bottom, not with it alone at the top.
int ScrollView::maxFirst() const {
    if (m_heights.empty()) return m_total > m_visible ? m_total - m_visible : 0;
    float used = 0.0f;
    int f = m_total;
    while (f > 0 && (f == m_total || used + m_heights[f - 1] <= m_viewH + 0.01f)) {
        used += m_heights[f - 1];
        f--;
    }
    return f;
}

float ScrollView::rowOffset(int index) const {
    if (m_heights.empty()) return (index - m_first) * m_pitch;
    float y = 0.0f;
    for (int i = m_first; i < index && i < m_total; i++) y += m_heights[i];
    return y;
}

void ScrollView::setFirst(int f) {
    if (f > maxFirst()) f = maxFirst();
    if (f < 0) f = 0;
    m_first = f;
    if (!m_heights.empty()) m_visible = visibleFrom(m_first);
}

bool ScrollView::scrollBy(int rows) {
    const int before = m_first;
    setFirst(m_first + rows);
    return m_first != before;
}

void ScrollView::ensureVisible(int index) {
    if (index < 0) return;
    if (index < m_first) setFirst(index);
    else if (m_heights.empty()) { if (index >= m_first + m_visible) setFirst(index - m_visible + 1); }
    else {
        int f = m_first;                      // the least scroll: advance until index is in view
        while (f < index && index >= f + visibleFrom(f)) f++;
        setFirst(f);
    }
}

// The thumb's length is the visible fraction, floored at the CSS's 24u min-height; it travels the
// track in maxFirst() discrete stops. y is UP, so first()==0 puts the thumb at the TOP.
CGRect ScrollView::thumbRect() const {
    const float U = u();
    const CGRect t = CGRectMake(m_bar.origin.x + 2*U, m_bar.origin.y + 2*U,
                                m_bar.size.width - 4*U, m_bar.size.height - 4*U);
    if (m_total <= m_visible || t.size.height <= 0.0f) return t;
    float h = t.size.height * (float)m_visible / (float)m_total;
    if (h < du(24)) h = std::min(du(24), (float)t.size.height);
    const float travel = t.size.height - h;
    const float frac = (float)m_first / (float)maxFirst();
    return CGRectMake(t.origin.x, t.origin.y + travel * (1.0f - frac), t.size.width, h);
}

bool ScrollView::hitBar(float x, float y) const { return inRect(m_bar, x, y); }

bool ScrollView::barBegin(float y) {
    const CGRect th = thumbRect();
    if (y >= th.origin.y && y <= th.origin.y + th.size.height) {
        m_thumbDrag = true;
        m_dragY0 = y;
        m_dragFirst0 = m_first;
        return false;
    }
    // On the track: one page toward the pointer, as a native scrollbar's track click does.
    return scrollBy(y > th.origin.y ? -m_visible : m_visible);
}

bool ScrollView::barDragTo(float y) {
    if (!m_thumbDrag || maxFirst() <= 0) return false;
    const CGRect th = thumbRect();
    const float travel = (m_bar.size.height - 4*u()) - th.size.height;
    if (travel <= 0.0f) return false;
    const float rowsPerUnit = (float)maxFirst() / travel;
    const int before = m_first;
    setFirst(m_dragFirst0 + (int)lroundf((m_dragY0 - y) * rowsPerUnit));   // finger down = later rows
    return m_first != before;
}

void ScrollView::beginContentDrag(float y) {
    m_contentDrag = true;
    m_contentScrolling = false;
    m_dragY0 = y;
    m_dragFirst0 = m_first;
}

// A finger moving UP pulls later rows into view, as a touch list does. Below the slop it is still
// a tap; once past it, it is a scroll for the rest of the touch even if it comes back.
// The slop is a third of a row; with variable heights that is the row the drag started on.
float ScrollView::dragPitch() const {
    if (m_heights.empty()) return m_pitch;
    if (m_dragFirst0 >= 0 && m_dragFirst0 < m_total) return m_heights[m_dragFirst0];
    return m_pitch;
}

bool ScrollView::contentDragTo(float y) {
    if (!m_contentDrag) return false;
    const float dy = y - m_dragY0;
    if (!m_contentScrolling && std::fabs(dy) < std::max(du(8), dragPitch() * 0.35f)) return false;
    m_contentScrolling = true;
    if (m_heights.empty()) {
        if (m_pitch > 0.0f) setFirst(m_dragFirst0 + (int)lroundf(dy / m_pitch));
        return true;
    }
    // Variable mode: walk the real heights from the row the drag started on, moving one row each
    // time the finger has covered more than half of the next one (what lroundf does above).
    int f = m_dragFirst0;
    float left = std::fabs(dy);
    if (dy > 0.0f) {                          // finger up: later rows
        while (f < m_total && left > m_heights[f] * 0.5f) { left -= m_heights[f]; f++; }
    } else {
        while (f > 0 && left > m_heights[f - 1] * 0.5f) { left -= m_heights[f - 1]; f--; }
    }
    setFirst(f);
    return true;
}

void ScrollView::endDrag() { m_thumbDrag = m_contentDrag = m_contentScrolling = false; }

void ScrollView::render() const {
    if (m_bar.size.width <= 0.0f || m_bar.size.height <= 0.0f) return;
    fill(m_bar, kTrack);
    bevel(m_bar, BEVEL_SUNKEN);
    // The thumb: --bevel-raised-sm, the slider thumb's recipe.
    const CGRect t = thumbRect();
    const float U = u();
    fill(t.origin.x + 2*U, t.origin.y - 2*U, t.size.width, t.size.height, C(0x000000, 0.25f));
    const float band = t.size.height * 0.08f;
    fill(t.origin.x, t.origin.y, t.size.width, t.size.height - band, m_thumbDrag ? kFacePressed : kFaceBottom);
    fill(t.origin.x, t.origin.y + t.size.height - band, t.size.width, band, m_thumbDrag ? kFacePressed : kFaceTop);
    insetBottomRight(t, 2*U, C(0x646464, 0.47f));
    border(t, 2*U, kKeyline);
}

// --- TabRail -------------------------------------------------------------------------------
TabRail::TabRail() : m_n(0), m_sel(0), m_held(-1) { m_rect = CGRectMake(0, 0, 0, 0); }

void TabRail::setTabs(const char* const* labels, int n, float pt) {
    m_n = n < 0 ? 0 : (n > GLW_MAX_TABS ? GLW_MAX_TABS : n);
    for (int i = 0; i < m_n; i++) m_tabs[i].setLabel(labels[i], pt);
}

CGRect TabRail::tabRect(int i) const {
    if (i < 0 || i >= m_n) return CGRectMake(0, 0, 0, 0);
    const float g = du(6);
    const float w = (m_rect.size.width - g * (m_n - 1)) / (float)m_n;
    return CGRectMake(m_rect.origin.x + i * (w + g), m_rect.origin.y, w, m_rect.size.height);
}

void TabRail::setRect(CGRect r) {
    m_rect = r;
    for (int i = 0; i < m_n; i++) m_tabs[i].setRect(tabRect(i));   // re-wraps only on a width change
}

int TabRail::hit(float x, float y) const {
    for (int i = 0; i < m_n; i++) if (m_tabs[i].hit(x, y)) return i;
    return -1;
}

void TabRail::render() const {
    for (int i = 0; i < m_n; i++) {
        Button& b = const_cast<Button&>(m_tabs[i]);   // pressed is presentation state only
        b.setPressed(i == m_sel || i == m_held);
        b.render();
    }
}

// --- RailFrame (U.5) -----------------------------------------------------------------------
// Lifted unchanged out of SettingsMenu::layout() (U.3), so New World shares it.
RailFrame railFrame() {
    RailFrame f;
    // THE PANEL IS FIXED (plan D3): on the constraining axis it is exactly the design frame, since
    // u() = min(W/783, H/587).
    const float W = std::min(SCREEN_WIDTH - 2.0f * du(12), du(783));
    const float H = std::min(SCREEN_HEIGHT - 2.0f * du(12), du(587));
    f.panel = CGRectMake((SCREEN_WIDTH - W) * 0.5f, (SCREEN_HEIGHT - H) * 0.5f, W, H);
    const float pad = du(9), gap = du(6);
    const float left = f.panel.origin.x + pad, right = f.panel.origin.x + W - pad;
    const float barH = du(29), barTop = f.panel.origin.y + H - du(7);
    f.titlebar = CGRectMake(left, barTop - barH, right - left, barH);
    const float bodyTop = barTop - barH - pad, bodyBot = f.panel.origin.y + pad;
    f.rail = CGRectMake(left, bodyBot, IconRail::designW(), bodyTop - bodyBot);
    const float barW = du(18);
    f.bar = CGRectMake(right - barW, bodyBot, barW, bodyTop - bodyBot);
    const float cx0 = left + IconRail::designW() + gap;
    f.content = CGRectMake(cx0, bodyBot, right - barW - gap - cx0, bodyTop - bodyBot);
    return f;
}

float RailFrame::titleY(const Label& title) const {
    return titlebar.origin.y + titlebar.size.height - (titlebar.size.height - title.height()) * 0.5f;
}

void titlebarHit(Button& b) {
    const float tf = touchFloor();
    const CGRect r = b.rect();
    if (tf > r.size.height)
        b.setHitRect(CGRectMake(r.origin.x, r.origin.y + (r.size.height - tf) * 0.5f, r.size.width, tf));
    else b.setHitRect(CGRectMake(0, 0, 0, 0));
}

// --- IconRail (U.1) ------------------------------------------------------------------------
// eden-ui.css `.eden-tabrail` + `.eden-btn--tab`: 64u tiles, 6u gap, 4u padding, a 38u icon, the
// rgba(150,150,150,.55) rail surface under a 1u gray-500 keyline (--bevel-content's inset).
static const float kRailPad = 4.0f, kRailGap = 6.0f, kRailTile = 64.0f, kRailIcon = 38.0f;
static const Color kRailFace = { 150/255.0f, 150/255.0f, 150/255.0f, 0.55f };   // --eden-surface-rail

IconRail::IconRail() : m_sel(0), m_held(-1) { m_rect = CGRectMake(0, 0, 0, 0); }

float IconRail::designW() { return du(kRailTile + 2 * kRailPad); }

void IconRail::setTiles(const int* icons, int n) {
    m_icons.assign(icons, icons + (n > 0 && icons ? n : 0));
    m_enabled.assign(m_icons.size(), 1);
    m_scroll.setFirst(0);
    setRect(m_rect);
}

void IconRail::setEnabled(int i, bool e) { if (i >= 0 && i < count()) m_enabled[i] = e ? 1 : 0; }
bool IconRail::enabled(int i) const      { return i >= 0 && i < count() && m_enabled[i]; }

float IconRail::tileSize() const { return std::max(0.0f, (float)m_rect.size.width - 2 * du(kRailPad)); }

// The scroll model is the ScrollView's uniform mode with the tile pitch: a tile "row" is a tile
// plus its gap, and the last tile's missing gap is what the +gap in the visible count pays for.
void IconRail::setRect(CGRect r) {
    m_rect = r;
    const float pitch = tileSize() + du(kRailGap);
    const float inner = r.size.height - 2 * du(kRailPad) + du(kRailGap);
    m_scroll.setPitch(pitch);
    m_scroll.setRows(count(), pitch > 0.0f ? (int)std::floor(inner / pitch + 0.001f) : 1);
}

CGRect IconRail::tileRect(int i) const {
    const int k = i - m_scroll.first();
    if (i < 0 || i >= count() || k < 0 || k >= m_scroll.visible()) return CGRectMake(0, 0, 0, 0);
    const float t = tileSize(), p = du(kRailPad);
    const float yTop = m_rect.origin.y + m_rect.size.height - p - k * (t + du(kRailGap));
    return CGRectMake(m_rect.origin.x + p, yTop - t, t, t);
}

bool IconRail::hitRail(float x, float y) const { return inRect(m_rect, x, y); }

int IconRail::hit(float x, float y) const {
    for (int i = m_scroll.first(); i < count() && i < m_scroll.first() + m_scroll.visible(); i++)
        if (m_enabled[i] && inRect(tileRect(i), x, y)) return i;
    return -1;
}

void IconRail::render() const {
    fill(m_rect, kRailFace);
    border(m_rect, 1 * u(), kKeyline);
    const float isz = du(kRailIcon) * (tileSize() / du(kRailTile));   // the icon scales with the tile
    for (int i = m_scroll.first(); i < count() && i < m_scroll.first() + m_scroll.visible(); i++) {
        const CGRect t = tileRect(i);
        const bool down = m_enabled[i] && (i == m_sel || i == m_held);
        bevel(t, down ? BEVEL_PRESSED : BEVEL_RAISED);
        // Disabled is design-system.md's "solid chrome, grey ink": the CSS's grayscale(1) of an
        // already grey tile is exactly a muted icon.
        const Color ink = !m_enabled[i] ? kTextMuted : down ? kTextPressed : kText;
        icon(m_icons[i], CGRectMake(t.origin.x + (t.size.width - isz) * 0.5f,
                                    t.origin.y + (t.size.height - isz) * 0.5f, isz, isz), ink);
    }
}

// --- TwoLineRow (U.1) ----------------------------------------------------------------------
// eden-ui.css `.eden-listrow`: padding 4u 5u, a 10u gap before the control; the title is
// display-md (32u) at line-height 1, the hint body-md (15u). Label lays lines out at 1.28 em, so
// a one-line title is lifted (1.28 - 1) / 2 em — Label::blit centres the glyphs on the line — and
// given a box of titleHeight().
static const float kRowPadY = 4.0f, kRowPadX = 5.0f, kRowGap = 10.0f, kRowHintGap = 1.0f;

TwoLineRow::TwoLineRow() : m_width(0), m_controlW(0), m_controlH(0), m_indent(0), m_titlePt(0),
                           m_hintPt(0), m_height(0), m_x(0), m_yTop(0), m_muted(false), m_last(false) {}

void TwoLineRow::build(const char* title, const char* hint, float width, float controlW, float controlH,
                       float indent, float titlePt, float hintPt) {
    const std::string t = title ? title : "", h = hint ? hint : "";
    if (titlePt <= 0.0f) titlePt = du(32);
    if (hintPt <= 0.0f) hintPt = du(15);
    const bool same = t == m_titleText && h == m_hintText && width == m_width && controlW == m_controlW &&
                      indent == m_indent && titlePt == m_titlePt && hintPt == m_hintPt &&
                      (!m_title.empty() || t.empty());
    m_controlH = controlH;
    if (!same) {
        m_titleText = t; m_hintText = h;
        m_width = width; m_controlW = controlW; m_indent = indent;
        m_titlePt = titlePt; m_hintPt = hintPt;
        float textW = width - 2 * du(kRowPadX) - indent - (controlW > 0.0f ? controlW + du(kRowGap) : 0.0f);
        if (textW < du(40)) textW = du(40);
        m_title.set(t.c_str(), titlePt, UITextAlignmentLeft, textW, FACE_DISPLAY);
        m_hint.set(h.c_str(), hintPt, UITextAlignmentLeft, textW, FACE_BODY);
    }
    // Height from the BUILT labels. A one-line title is a 0.85 em box; a title that had to wrap keeps
    // Label's own 1.28 em spacing (tighter would overlap the lines) — render() mirrors this.
    float text = m_title.empty() ? 0.0f : titleHeight();
    if (!m_hint.empty()) text += du(kRowHintGap) + m_hint.height();
    m_height = std::max(text, m_controlH) + 2 * du(kRowPadY);
}

// 0.85 em, not the CSS's 1: Jersey's raster leaves its descender room under the baseline and the
// hint's own line adds leading above its caps, so a full em read as a loose gap in the U.1 kit shot.
float TwoLineRow::titleHeight() const {
    return m_title.height() <= m_title.lineHeight() * 1.01f ? m_titlePt * 0.85f : m_title.height();
}

CGRect TwoLineRow::rect() const { return CGRectMake(m_x, m_yTop - m_height, m_width, m_height); }

CGRect TwoLineRow::controlRect(float w, float h) const {
    return CGRectMake(m_x + m_width - du(kRowPadX) - w, m_yTop - m_height * 0.5f - h * 0.5f, w, h);
}

bool TwoLineRow::hit(float x, float y) const { return inRect(rect(), x, y); }

void TwoLineRow::render() const {
    const float U = u();
    if (!m_last) fill(m_x, m_yTop - m_height, m_width, 1*U, C(0x000000, 0.28f));
    const float x = m_x + du(kRowPadX) + m_indent;
    float y = m_yTop - du(kRowPadY);
    if (!m_title.empty()) {
        const bool one = m_title.height() <= m_title.lineHeight() * 1.01f;
        const float lift = one ? (m_title.lineHeight() - m_titlePt) * 0.5f : 0.0f;
        m_title.draw(x, y + lift, m_muted ? kTextMuted : kText);
        y -= titleHeight();
    }
    if (!m_hint.empty()) m_hint.draw(x, y - du(kRowHintGap), m_muted ? kTextMuted : kText);
}

void progressBar(CGRect r, float frac, float phase) {
    fill(r, kTrack);
    const float U = u();
    const CGRect in = CGRectMake(r.origin.x + 2*U, r.origin.y + 2*U, r.size.width - 4*U, r.size.height - 4*U);
    float x = in.origin.x, w;
    if (frac < 0.0f) {                       // indeterminate: a third-width block ping-pongs
        w = in.size.width / 3.0f;
        float t = std::fmod(phase, 2.0f);
        if (t > 1.0f) t = 2.0f - t;
        x += (in.size.width - w) * t;
    } else {
        w = in.size.width * std::min(1.0f, frac);
    }
    if (w > 0.0f) {
        const float band = in.size.height * 0.12f;
        fill(x, in.origin.y, w, in.size.height - band, kLime);
        fill(x, in.origin.y + in.size.height - band, w, band, kLimeTop);
    }
    bevel(r, BEVEL_SUNKEN);
}

// --- TextField -----------------------------------------------------------------------------
static TextField* s_focused = NULL;

TextField::TextField() : m_pt(16.0f), m_maxBytes(49), m_lines(1), m_showingPlaceholder(false), m_code(false) {
    m_rect = CGRectMake(0, 0, 0, 0);
    m_keep = CGRectMake(0, 0, 0, 0);
}

// Tells the platform where the field is, and what else the keyboard must leave visible.
void TextField::announce() const {
    eden_text_input_keep_visible(m_keep.origin.x, m_keep.origin.y, m_keep.size.width, m_keep.size.height);
    if (m_code) eden_text_input_start_code(m_rect.origin.x, m_rect.origin.y, m_rect.size.width, m_rect.size.height);
    else        eden_text_input_start(m_rect.origin.x, m_rect.origin.y, m_rect.size.width, m_rect.size.height);
}

TextField::~TextField() { if (s_focused == this) blur(); }

void TextField::rebuild() {
    std::string shown = m_text;
    m_showingPlaceholder = false;
    if (focused()) shown += "|";
    else if (shown.empty() && !m_placeholder.empty()) { shown = m_placeholder; m_showingPlaceholder = true; }
    if (shown.empty()) { m_label.clear(); return; }
    if (m_lines > 1 && m_rect.size.width > du(12))
        m_label.set(shown.c_str(), m_pt, UITextAlignmentLeft, m_rect.size.width - du(12), FACE_DISPLAY, m_lines);
    else
        m_label.set(shown.c_str(), m_pt, UITextAlignmentLeft);
}

void TextField::setLines(int n) { m_lines = n < 1 ? 1 : n; rebuild(); }

float TextField::heightForLines(int n) const { return n * m_pt * 1.28f + du(10); }   // Label::lineHeight

// Re-announces the IME area only when the rect moved: screens lay out every frame, and
// SDL_StartTextInput per frame is a keyboard re-show request per frame on iOS.
void TextField::setRect(CGRect r) {
    const bool moved = r.origin.x != m_rect.origin.x || r.origin.y != m_rect.origin.y ||
                       r.size.width != m_rect.size.width || r.size.height != m_rect.size.height;
    const bool rewrap = m_lines > 1 && r.size.width != m_rect.size.width;
    m_rect = r;
    if (moved && focused()) announce();
    if (rewrap) rebuild();
}

void TextField::setKeepVisible(CGRect r) {
    const bool moved = r.origin.x != m_keep.origin.x || r.origin.y != m_keep.origin.y ||
                       r.size.width != m_keep.size.width || r.size.height != m_keep.size.height;
    m_keep = r;
    if (moved && focused()) announce();
}

void TextField::setPointSize(float pt) { m_pt = pt; rebuild(); }

void TextField::setText(const char* utf8) {
    m_text = utf8 ? utf8 : "";
    if ((int)m_text.size() > m_maxBytes) {
        size_t n = (size_t)m_maxBytes;
        while (n > 0 && ((unsigned char)m_text[n] & 0xC0) == 0x80) n--;   // never split a char
        m_text.resize(n);
    }
    rebuild();
}

void TextField::setPlaceholder(const char* utf8) { m_placeholder = utf8 ? utf8 : ""; rebuild(); }

bool TextField::hit(float x, float y) const { return inRect(m_rect, x, y); }

bool TextField::focused() const { return s_focused == this; }

void TextField::focus() {
    if (s_focused == this) return;
    if (s_focused) s_focused->blur();
    s_focused = this;
    char drain[256];
    while (eden_text_input_take(drain, sizeof(drain)) > 0) {}   // nothing typed before focus counts
    announce();
    rebuild();
}

void TextField::blur() {
    if (s_focused != this) return;
    s_focused = NULL;
    eden_text_input_stop();
    rebuild();
}

// Returns false once the field has committed or cancelled, so the caller stops feeding it.
bool TextField::applyInput(const char* bytes, int n, Event* ev) {
    for (int i = 0; i < n; ++i) {
        const unsigned char c = (unsigned char)bytes[i];
        if (c == '\r' || c == '\n') { *ev = EV_COMMIT; return false; }
        if (c == 0x1b)              { *ev = EV_CANCEL; return false; }
        if (c == '\b') {
            if (m_text.empty()) continue;
            size_t k = m_text.size() - 1;
            while (k > 0 && ((unsigned char)m_text[k] & 0xC0) == 0x80) k--;   // a whole UTF-8 char
            m_text.resize(k);
            *ev = EV_CHANGED;
            continue;
        }
        if (c == EDEN_TEXT_KEY_TAB || c == EDEN_TEXT_KEY_UP || c == EDEN_TEXT_KEY_DOWN) {
            if (m_code) m_keys += (char)c;             // C.4: the screen's, via takeKey()
            continue;
        }
        if (c < 0x20 || c == 0x7f) continue;          // other control bytes: not text
        // A lead byte starts a character of 1-4 bytes; take it whole or not at all.
        int len = 1;
        if      ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        else if ((c & 0xC0) == 0x80) continue;        // a stray continuation byte
        if (i + len > n) break;
        if ((int)m_text.size() + len <= m_maxBytes) {
            m_text.append(bytes + i, (size_t)len);
            *ev = EV_CHANGED;
        }
        i += len - 1;
    }
    return true;
}

int TextField::takeKey() {
    if (m_keys.empty()) return 0;
    const int k = (unsigned char)m_keys[0];
    m_keys.erase(0, 1);
    return k;
}

TextField::Event TextField::update() {
    m_keys.clear();
    if (!focused()) return EV_NONE;
    Event ev = EV_NONE;
    char buf[256];
    int n;
    bool open = true;
    while (open && (n = eden_text_input_take(buf, sizeof(buf))) > 0) open = applyInput(buf, n, &ev);
    // The platform ended text input under us — on iOS, the user dismissing the keyboard. Not a
    // commit (they may only want to see the screen) and not a cancel (the typing is kept).
    if (open && !eden_text_input_active()) ev = EV_BLURRED;
    if (ev == EV_COMMIT || ev == EV_CANCEL || ev == EV_BLURRED) blur();
    else if (ev == EV_CHANGED) rebuild();
    return ev;
}

void TextField::render() const {
    fill(m_rect, kContentFace);
    bevel(m_rect, BEVEL_SUNKEN);
    if (m_label.empty()) return;
    const float yTop = m_lines > 1 ? m_rect.origin.y + m_rect.size.height - du(5)   // top-aligned
                                   : m_rect.origin.y + (m_rect.size.height + m_label.height()) * 0.5f;
    m_label.draw(m_rect.origin.x + du(6), yTop, m_showingPlaceholder ? kPlaceholder : kText);
}

// --- Version tag -----------------------------------------------------------------------------
#ifndef EMOD_VERSION_STRING
#define EMOD_VERSION_STRING "dev"
#endif

void drawVersionTag() {
    static Label tag;
    static float builtU = 0.0f;
    if (tag.empty() || builtU != u()) {
        const std::string t = std::string("Emod ") + EMOD_VERSION_STRING;
        tag.set(t.c_str(), 14.0f, UITextAlignmentLeft);
        builtU = u();
    }
    const float x = du(6), yTop = SCREEN_HEIGHT - du(6), o = u();
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    glEnable(GL_TEXTURE_2D);
    tag.draw(x + o, yTop - o, kBlack);
    tag.draw(x, yTop, kWhite);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
}

}  // namespace GLW
