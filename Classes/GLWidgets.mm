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

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

extern float SCREEN_WIDTH;
extern float SCREEN_HEIGHT;
extern "C" float eden_ui_raster_density(void);   // web/src/seam/DisplayProfile_web.mm

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

extern "C" int eden_effective_input_is_touch(void);   // web/src/seam/Settings_web.mm (both targets)
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
// The display face's own figure: Jersey 10 measures 0.34 em on mixed-case labels (stb_truetype,
// 2026-10-05; Arial measures 0.41 against the 0.52 above), so 0.42 keeps the same headroom.
static const float kAvgGlyph = 0.52f;
static const float kAvgGlyphDisplay = 0.42f;

static int potAtLeast(int v) { int p = 32; while (p < v && p < 2048) p <<= 1; return p; }

Label::Label() : m_pt(15.0f), m_texW(512), m_texH(64), m_align(UITextAlignmentCenter), m_boxW(0),
                 m_face(FACE_DISPLAY) {}
Label::~Label() { clear(); }

void Label::clear() {
    for (size_t i = 0; i < m_lines.size(); i++) if (m_lines[i]) delete m_lines[i];
    m_lines.clear();
}

float Label::lineHeight() const { return m_pt * 1.28f; }
float Label::height() const     { return m_lines.size() * lineHeight(); }

void Label::set(const char* text, float pt, UITextAlignment align, float maxWidth, Face face) {
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

// --- Button --------------------------------------------------------------------------------
Button::Button() : m_pt(15.0f), m_pressed(false), m_enabled(true), m_tone(TONE_DEFAULT) {
    m_rect = CGRectMake(0, 0, 0, 0);
    m_hit = CGRectMake(0, 0, 0, 0);
}
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

// Only a WIDTH change re-wraps. Screens call setRect() from a per-frame layout(), and until 5.4
// this rebuilt the label every time — a texture upload (and, since N.4.9, a glFlush) per button
// per frame on every converted screen. Position changes need nothing: the label draws at render().
void Button::setRect(CGRect r) {
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
    if (m_label.empty()) return;
    const float cx   = m_rect.origin.x + m_rect.size.width * 0.5f;
    const float yTop = m_rect.origin.y + (m_rect.size.height + m_label.height()) * 0.5f;
    // "Pressed == selected" inverts BOTH the label and its shadow — that inversion is the cue.
    if (!m_enabled) m_label.drawCentered(cx, yTop, kKeyline);
    else if (m_pressed) m_label.drawChrome(cx, yTop, m_tone == TONE_DANGER ? kWhite : kTextPressed, kBlack);
    else           m_label.drawChrome(cx, yTop, kText,        kWhite);
}

// --- Toggle --------------------------------------------------------------------------------
// eden-ui.css `.eden-toggle`: 104x34u, SUNKEN on the content face; each half is inset 2u and
// (50% - 2u) wide. Checked: the ON half takes the lime 12% face (the toggle's own one-stop
// gradient, a sibling of the button's 6% one) and the OFF half goes white; unchecked, ON is white
// and OFF is grey-300.
static const Color kLimeTop    = { 0xc6/255.0f, 0xf1/255.0f, 0x75/255.0f, 1.0f };   // --eden-green-100
static const Color kLime       = { 0x89/255.0f, 0xc3/255.0f, 0x1f/255.0f, 1.0f };   // --eden-green-500
static const Color kOffHalf    = { 0x97/255.0f, 0x97/255.0f, 0x97/255.0f, 1.0f };   // --eden-gray-300
static const Color kTrack      = { 0x45/255.0f, 0x45/255.0f, 0x45/255.0f, 1.0f };   // --eden-surface-track
static const Color kPlaceholder = { 0.0f, 0.0f, 0.0f, 0.5f };                       // --eden-text-placeholder

static bool inRect(CGRect r, float x, float y) {
    return x >= r.origin.x && x <= r.origin.x + r.size.width &&
           y >= r.origin.y && y <= r.origin.y + r.size.height;
}

Toggle::Toggle() : m_on(false) { m_rect = m_hit = CGRectMake(0, 0, 0, 0); }

bool Toggle::hit(float x, float y) const { return inBox(m_rect, m_hit, x, y); }

void Toggle::render() const {
    const float U = u();
    fill(m_rect, kContentFace);
    const float x = m_rect.origin.x, y = m_rect.origin.y + 2*U;
    const float hw = m_rect.size.width * 0.5f - 2*U, hh = m_rect.size.height - 4*U;
    if (m_on) {
        const float band = hh * 0.12f;
        fill(x + 2*U, y, hw, hh - band, kLime);
        fill(x + 2*U, y + hh - band, hw, band, kLimeTop);
        fill(x + m_rect.size.width - 2*U - hw, y, hw, hh, kContentFace);
    } else {
        fill(x + 2*U, y, hw, hh, kContentFace);
        fill(x + m_rect.size.width - 2*U - hw, y, hw, hh, kOffHalf);
    }
    // The bevel last: SUNKEN's inset shadow sits ON TOP of the halves, as the CSS box-shadow does
    // (box-shadow paints over the background, under the children — and the halves are children,
    // but they are inset 2u, inside the 1u keyline and mostly clear of the 3u recess).
    bevel(m_rect, BEVEL_SUNKEN);
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
                           m_contentDrag(false), m_contentScrolling(false), m_dragY0(0), m_dragFirst0(0) {
    m_bar = CGRectMake(0, 0, 0, 0);
}

void ScrollView::setRows(int total, int visible) {
    m_total = total > 0 ? total : 0;
    m_visible = visible > 0 ? visible : 1;
    setFirst(m_first);
}

void ScrollView::setFirst(int f) {
    if (f > maxFirst()) f = maxFirst();
    if (f < 0) f = 0;
    m_first = f;
}

bool ScrollView::scrollBy(int rows) {
    const int before = m_first;
    setFirst(m_first + rows);
    return m_first != before;
}

void ScrollView::ensureVisible(int index) {
    if (index < 0) return;
    if (index < m_first) setFirst(index);
    else if (index >= m_first + m_visible) setFirst(index - m_visible + 1);
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
bool ScrollView::contentDragTo(float y) {
    if (!m_contentDrag) return false;
    const float dy = y - m_dragY0;
    if (!m_contentScrolling && std::fabs(dy) < std::max(du(8), m_pitch * 0.35f)) return false;
    m_contentScrolling = true;
    if (m_pitch > 0.0f) setFirst(m_dragFirst0 + (int)lroundf(dy / m_pitch));
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

TextField::TextField() : m_pt(16.0f), m_maxBytes(49), m_showingPlaceholder(false) {
    m_rect = CGRectMake(0, 0, 0, 0);
}

TextField::~TextField() { if (s_focused == this) blur(); }

void TextField::rebuild() {
    std::string shown = m_text;
    m_showingPlaceholder = false;
    if (focused()) shown += "|";
    else if (shown.empty() && !m_placeholder.empty()) { shown = m_placeholder; m_showingPlaceholder = true; }
    if (shown.empty()) { m_label.clear(); return; }
    m_label.set(shown.c_str(), m_pt, UITextAlignmentLeft);
}

// Re-announces the IME area only when the rect moved: screens lay out every frame, and
// SDL_StartTextInput per frame is a keyboard re-show request per frame on iOS.
void TextField::setRect(CGRect r) {
    const bool moved = r.origin.x != m_rect.origin.x || r.origin.y != m_rect.origin.y ||
                       r.size.width != m_rect.size.width || r.size.height != m_rect.size.height;
    m_rect = r;
    if (moved && focused())
        eden_text_input_start(r.origin.x, r.origin.y, r.size.width, r.size.height);
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
    eden_text_input_start(m_rect.origin.x, m_rect.origin.y, m_rect.size.width, m_rect.size.height);
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

TextField::Event TextField::update() {
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
    const float yTop = m_rect.origin.y + (m_rect.size.height + m_label.height()) * 0.5f;
    m_label.draw(m_rect.origin.x + du(6), yTop, m_showingPlaceholder ? kPlaceholder : kText);
}

}  // namespace GLW
