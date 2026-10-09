//
//  GLWidgets.h
//  Eden
//
//  The engine-side GL widget kit (Phase N Stage 5.1,
//  WORKING/phase-n-stage3plus-plan-2026-09-05.md). ONE cross-platform UI language drawn with the
//  engine's own GL calls, so iOS, Android, the native desktop builds and the web build's
//  `legacy_menu` fallback all get the same chrome without a per-platform toolkit and without a
//  new dependency.
//
//  It is a transcription of web/docs/design-system.md — the same tokens, the same THREE bevel
//  mechanics (RAISED / SUNKEN / PRESSED, plus the two surfaces WINDOW / CONTENT), square corners,
//  hard-edged zero-blur shadows. Read that document before changing anything here; if you think
//  you need a fourth bevel you almost certainly need one of the three.
//
//  WHAT IT IS NOT (yet): a retained widget tree. Stage 5.1's plan calls for Panel/Label/Button/
//  Toggle/Slider/Stepper/Cycler/ListRow/ScrollView/TabRail/TextField; this file ships the token +
//  drawing layer, Label (with word wrap), Button, and since Stage 5.4 / N.4.5 Toggle, Slider,
//  Stepper (which is also the Cycler) and TextField, and since Stage 5.6 ListRow and a
//  row-snapped ScrollView, and since Stage 5.9 a horizontal TabRail and progressBar() (the world
//  browser's). A new control lands HERE, not in a screen.
//
//  COORDINATE SPACE. Everything below is in POINT space with y UP, the space Button/CGRect and
//  Input's touch coordinates already use. GLW::fill()/bevel() apply the SCALE_* multiply that
//  Graphics::drawRect needs internally, exactly as SettingsMenu's sm_fill and GLDialog's
//  gldialog_fill did (this file replaces both idioms). Callers never scale.
//
#ifndef Eden_GLWidgets_h
#define Eden_GLWidgets_h

#import "Texture2D.h"
#include <string>
#include <vector>

namespace GLW {

// --- scale ---------------------------------------------------------------------------------
// The kit's answer to the CSS `--u` unit: every dimension is authored in mockup pixels against
// the 783x587 design frame and multiplied by u(). Unlike the web the denominator here is POINT
// space (~1138x640 at the default UI scale), which already tracks the window, so u() sits near
// 1.1 on desktop and moves with the display profile instead of with raw pixels.
float u();
inline float du(float designPx) { return designPx * u(); }

// --- palette (web/public/eden-ui.css :root) ------------------------------------------------
struct Color { float r, g, b, a; };
Color rgb(unsigned int hex, float a = 1.0f);

extern const Color kScrim;         // --eden-scrim
extern const Color kWindowFace;    // --eden-surface-window
extern const Color kContentFace;   // --eden-surface-content
extern const Color kFaceTop;       // --eden-face-top      (the 6% fake top-edge highlight)
extern const Color kFaceBottom;    // --eden-face-bottom
extern const Color kFacePressed;   // --eden-face-pressed
extern const Color kKeyline;       // --eden-gray-500
extern const Color kWindowKeyline; // --eden-gray-700
extern const Color kText;          // --eden-text
extern const Color kTextSecondary; // --eden-text-secondary
extern const Color kTextPressed;   // --eden-text-pressed
extern const Color kWhite;
extern const Color kBlack;

// --- primitives ----------------------------------------------------------------------------
void fill(CGRect r, Color c);
void fill(float x, float y, float w, float h, Color c);

// The three mechanics plus the two surfaces. `r` is the control's box; the drop shadow is drawn
// OUTSIDE it (down-right in screen terms, i.e. -y here), as CSS box-shadow does.
enum BevelStyle { BEVEL_RAISED, BEVEL_SUNKEN, BEVEL_PRESSED, BEVEL_WINDOW, BEVEL_CONTENT };
void bevel(CGRect r, BevelStyle style);

// --- text ----------------------------------------------------------------------------------
// The design system's two faces (design-system.md, "Type"): DISPLAY is Jersey 10, the pixel face
// for everything the player reads as chrome — titles, button labels, row titles, values; BODY is
// the platform sans, for actual descriptive sentences only (a dialog's body, a hint line). Jersey
// is small for its em (caps are 0.50 of the pixel height against Arial's 0.64), so a display size
// reads like a body size about 1.28x smaller — the CSS's 22u buttons beside 15u body text.
enum Face { FACE_BODY = 0, FACE_DISPLAY = 1 };

// One label = one or more rasterised lines. Word-wrapped at construction (the raster seam is
// single-line), so building one is a texture upload per line: build on show()/resize, never in a
// frame loop. `pt` is in POINTS; the SCALE_* correction the raster needs is applied inside.
class Label {
public:
    Label();
    ~Label();

    // maxWidth == 0 disables wrapping. align is UITextAlignmentLeft/Center/Right.
    void set(const char* text, float pt, UITextAlignment align, float maxWidth = 0.0f,
             Face face = FACE_DISPLAY);
    void clear();

    bool  empty()  const { return m_lines.empty(); }
    float height() const;                     // total laid-out height, points
    float lineHeight() const;

    // Anchors, all point space: `x,yTop` is the top-left of the text block (lines run downward);
    // drawCentered centres the block horizontally on cx with its top at yTop.
    void draw(float x, float yTop, Color c) const;
    void drawCentered(float cx, float yTop, Color c) const;
    // Chrome text: the label in `c` with a 1u offset shadow in `shadow` (the design system's
    // press cue is that BOTH invert — black-on-white raised, white-on-black pressed).
    void drawChrome(float cx, float yTop, Color c, Color shadow) const;

private:
    Label(const Label&);
    Label& operator=(const Label&);
    void blit(Texture2D* t, float x, float yTop) const;

    std::vector<Texture2D*> m_lines;
    float m_pt;
    int   m_texW, m_texH;                     // POT texture dims the lines were built at
    UITextAlignment m_align;
    float m_boxW;                             // width the alignment was resolved against
    Face  m_face;
};

// --- Button --------------------------------------------------------------------------------
// A RAISED box with a centred chrome label that flips to PRESSED while held. Owns no input
// policy: screens still run the `itouch touches[]` / usage_id claim protocol themselves and call
// hit()/setPressed(), because who may claim a touch is a screen-level decision.
class Button {
public:
    // `.eden-btn--positive` (Play: a green face) and `--danger` (a red one). Same bevel either way.
    enum Tone { TONE_DEFAULT, TONE_POSITIVE, TONE_DANGER };

    Button();
    ~Button();

    void setLabel(const char* text, float pt = 15.0f);
    void setPointSize(float pt);              // re-wraps in place; call before setRect()
    void setRect(CGRect r);                   // re-runs the label's wrap against the new width
    CGRect rect() const { return m_rect; }
    void setTone(Tone t)     { m_tone = t; }

    bool pressed() const     { return m_pressed; }
    void setPressed(bool p)  { m_pressed = p; }
    // A disabled button keeps its chrome solid (design-system.md, "Placeholders and disabled
    // controls": a translucent ghost reads as a rendering bug), greys its label, and never hits.
    void setEnabled(bool e)  { m_enabled = e; if (!e) m_pressed = false; }
    bool enabled() const     { return m_enabled; }
    bool hit(float x, float y) const;
    // An optional larger box for hit(), so the art can stay compact while a touch target meets
    // touchFloor(). Zero-sized (the default) means "the drawn rect".
    void setHitRect(CGRect r) { m_hit = r; }

    void render() const;

private:
    void rebuildLabel();

    Button(const Button&);
    Button& operator=(const Button&);

    Label       m_label;
    std::string m_text;
    CGRect      m_rect, m_hit;
    float       m_pt;
    bool        m_pressed, m_enabled;
    Tone        m_tone;
};

// The design system's 44pt touch floor (`pointer: coarse` in the CSS). It raises the HIT BOX,
// not the art (design-system.md, "Touch floor"): touchFloor() is 44 while the input profile is
// touch and 0 otherwise, so a desktop screen can be as dense as its pointer allows. kTouchFloor
// is the constant itself, for a caller that needs the number regardless of profile.
extern const float kTouchFloor;
float touchFloor();

// --- Toggle (Stage 5.4) --------------------------------------------------------------------
// `.eden-toggle`: a SUNKEN split pill, no sliding knob. The left half is lime when on, the right
// half grey when off; the other half is the white content face. Holds a value, so it is SUNKEN
// (design-system.md, "New control that holds a value? It is SUNKEN."). Same input contract as
// Button: the screen claims the touch and calls hit(); the toggle never reads Input itself.
class Toggle {
public:
    Toggle();

    void setRect(CGRect r)   { m_rect = r; }
    CGRect rect() const      { return m_rect; }
    bool on() const          { return m_on; }
    void setOn(bool v)       { m_on = v; }
    bool hit(float x, float y) const;
    void setHitRect(CGRect r) { m_hit = r; }  // as Button::setHitRect

    void render() const;

private:
    CGRect m_rect, m_hit;
    bool   m_on;
};

// --- Slider (Stage 5.4) --------------------------------------------------------------------
// `.eden-slider`: a SUNKEN track (10u, --eden-surface-track) with a RAISED thumb (16x24u). The rect
// is the whole hit box — the track is centred in it vertically, so the box can sit at the touch
// floor while the art stays the CSS size. The value snaps to `step` from `min`.
//
// Drag protocol, for a screen that owns the touch: beginDrag(x) on the down edge (it jumps the
// thumb to the finger, as a native range input does), dragTo(x) every frame the touch is held,
// endDrag() on release. Both return true only when the SNAPPED value changed, so a screen that
// commits on true writes once per step, not once per frame.
class Slider {
public:
    Slider();

    void setRange(float minV, float maxV, float step);
    void setValue(float v);                   // clamps + snaps
    float value() const      { return m_value; }

    void setRect(CGRect r)   { m_rect = r; }
    CGRect rect() const      { return m_rect; }
    bool hit(float x, float y) const;
    void setHitRect(CGRect r) { m_hit = r; }  // as Button::setHitRect; drags still map on rect()

    bool beginDrag(float x);
    bool dragTo(float x);
    void endDrag()           { m_dragging = false; }
    bool dragging() const    { return m_dragging; }

    void render() const;

private:
    float snap(float v) const;
    float valueAt(float x) const;
    CGRect thumbRect() const;

    CGRect m_rect, m_hit;
    float  m_min, m_max, m_step, m_value;
    bool   m_dragging;
};

// --- Stepper (Stage 5.4) -------------------------------------------------------------------
// [-] value [+] — two RAISED buttons around a SUNKEN value well. It is also the kit's Cycler: a
// screen showing an enum passes "<" / ">" glyphs and wraps the index itself. The value text is
// the caller's (it knows the units); the stepper only lays it out. hitDec()/hitInc() + the
// buttons' pressed state are the whole input surface.
class Stepper {
public:
    Stepper();

    void setGlyphs(const char* dec, const char* inc);   // default "-" "+"
    void setValueText(const char* text, float pt);
    void setRect(CGRect r);
    CGRect rect() const      { return m_rect; }
    // Stretches both buttons' hit boxes to this box's y-span; each keeps its own x-span. Call after
    // setRect(). Zero-sized = the drawn rects.
    void setHitRect(CGRect r);

    bool hitDec(float x, float y) const { return m_dec.hit(x, y); }
    bool hitInc(float x, float y) const { return m_inc.hit(x, y); }
    void setPressed(bool dec, bool inc) { m_dec.setPressed(dec); m_inc.setPressed(inc); }

    void render() const;

private:
    Stepper(const Stepper&);
    Stepper& operator=(const Stepper&);

    Button      m_dec, m_inc;
    Label       m_value;
    std::string m_decGlyph, m_incGlyph;
    float       m_glyphPt;
    CGRect      m_rect;
};

// --- ListRow (Stage 5.6) -------------------------------------------------------------------
// `.eden-listrow--selectable`: one entry in a list that is itself the control (a world in the
// main menu). Drawn on a CONTENT surface: no face of its own, a 1u hairline under it unless it
// is the last row, and when selected the design system's TINT — a light lime wash plus a 4u lime
// bar on the left edge (design-system.md, "Selection is a tint, not a fill"). The title is the
// display face, left-aligned and vertically centred. Same input contract as Button.
class ListRow {
public:
    ListRow();

    void setTitle(const char* utf8, float pt);   // a texture upload: call on change, not per frame
    void setRect(CGRect r)   { m_rect = r; }
    CGRect rect() const      { return m_rect; }
    void setSelected(bool s) { m_selected = s; }
    bool selected() const    { return m_selected; }
    void setLast(bool l)     { m_last = l; }      // the last row draws no separator
    bool hit(float x, float y) const;

    void render() const;

private:
    ListRow(const ListRow&);
    ListRow& operator=(const ListRow&);

    Label  m_title;
    CGRect m_rect;
    bool   m_selected, m_last;
};

// --- ScrollView (Stage 5.6) ----------------------------------------------------------------
// A vertical list's scroll model plus its `.eden-scrollbar`. ROW-SNAPPED on purpose: the kit has
// no clipping (no scissor through the GL translator's letterboxed, render-scaled FBO path), so a
// partially visible row would draw outside its box. Scrolling therefore moves whole rows — first()
// is the index of the top visible row — which also keeps a ListRow's label upload count bounded
// by the visible rows rather than the list's length.
//
// Three ways to scroll, each a pure function of pointer positions the screen hands it:
//   - drag the CONTENT (touch): beginContentDrag(y) on the down edge, contentDragTo(y) while held;
//     it reports true once the finger has travelled far enough to be a scroll rather than a tap,
//     and from then on the screen should treat the touch as a scroll (no row selection on release);
//   - drag the THUMB, or tap the TRACK to page (barBegin / barDragTo);
//   - scrollBy(n) — the mouse wheel, arrow keys.
// The bar is SUNKEN track + small RAISED thumb, the slider's parts stood on end. When everything
// fits, the track stays and the thumb fills it (the CSS `.is-inert` state).
class ScrollView {
public:
    ScrollView();

    void setRows(int total, int visible);    // clamps first()
    int  total() const       { return m_total; }
    int  visible() const     { return m_visible; }
    int  first() const       { return m_first; }
    void setFirst(int f);
    bool scrollBy(int rows);                 // true if first() changed
    void ensureVisible(int index);           // scrolls the least distance that shows `index`

    void setBarRect(CGRect r)  { m_bar = r; }
    CGRect barRect() const     { return m_bar; }
    void setPitch(float p)     { m_pitch = p; }   // the content's row pitch, for content drags

    bool hitBar(float x, float y) const;
    bool barBegin(float y);                  // on the thumb: grab it; on the track: page toward y
    bool barDragTo(float y);                 // while the thumb is held
    void beginContentDrag(float y);
    bool contentDragTo(float y);             // true while the touch is a scroll (see above)
    bool scrolling() const   { return m_contentScrolling; }
    void endDrag();

    void render() const;

private:
    CGRect thumbRect() const;
    int    maxFirst() const  { return m_total > m_visible ? m_total - m_visible : 0; }

    CGRect m_bar;
    float  m_pitch;
    int    m_total, m_visible, m_first;
    // Drag state: the pointer y and first() at the down edge.
    bool   m_thumbDrag, m_contentDrag, m_contentScrolling;
    float  m_dragY0;
    int    m_dragFirst0;
};

// --- TabRail (Stage 5.9) -------------------------------------------------------------------
// A horizontal row of tab tiles: RAISED buttons, the selected one PRESSED (design-system.md:
// "pressed == selected" is the whole segmented-control mechanic). The web's `.eden-tabrail` is the
// settings screen's VERTICAL icon rail; this is its sibling for a screen whose tabs are a few words
// (the world browser's sources). Equal-width tiles with the kit's 6u gap. Same input contract as
// Button: the screen claims the touch, calls hit() on the down edge and setHeld() while it is down.
#define GLW_MAX_TABS 6
class TabRail {
public:
    TabRail();

    void setTabs(const char* const* labels, int n, float pt);   // texture uploads: call once
    void setRect(CGRect r);
    CGRect rect() const          { return m_rect; }
    int  count() const           { return m_n; }
    void setSelected(int i)      { m_sel = i; }       // -1 = none (e.g. a search is showing)
    int  selected() const        { return m_sel; }
    void setHeld(int i)          { m_held = i; }      // the tile under a held touch, -1 none
    int  hit(float x, float y) const;                 // tile index, or -1
    CGRect tabRect(int i) const;

    void render() const;

private:
    TabRail(const TabRail&);
    TabRail& operator=(const TabRail&);

    Button m_tabs[GLW_MAX_TABS];
    int    m_n, m_sel, m_held;
    CGRect m_rect;
};

// `.eden-progress` (Stage 5.9): a SUNKEN track with the lime 12% fill growing from the left.
// `frac` is clamped to 0..1; a negative value draws an indeterminate bar (a third-width block that
// moves with `phase`, any increasing number — the caller's clock).
void progressBar(CGRect r, float frac, float phase = 0.0f);

// --- TextField (N.4.5) ---------------------------------------------------------------------
// `.eden-field`: a SUNKEN white well with left-aligned text. The engine's first text entry since
// the iOS-only VKeyboard.mm (seam-excluded on every port target).
//
// INPUT comes from the platform text-input seam (eden_text_input_* below), not from Input's
// touches: focus() starts the platform's text input (SDL_StartTextInput on native, which is what
// raises the system keyboard on iOS), blur() stops it, and update() drains what was typed. One
// field holds focus at a time; focusing another blurs the first.
//
// THE CARET IS A GLYPH. The raster seam has no measure call (see kAvgGlyph in the .mm), so a caret
// drawn as a rectangle would sit at an estimated x and visibly drift off the end of real text. It
// is rasterised as part of the label instead ("name|"), which puts it exactly after the last
// glyph by construction. Editing is therefore at the end only — type and backspace — which is
// the whole of what renaming a world needs.
class TextField {
public:
    enum Event { EV_NONE, EV_CHANGED, EV_COMMIT, EV_CANCEL, EV_BLURRED };

    TextField();
    ~TextField();

    void setRect(CGRect r);
    CGRect rect() const      { return m_rect; }
    // What an on-screen keyboard must not cover while this field is focused, the field included —
    // a dialog passes its whole panel so its buttons stay reachable (T.B1, 2026-10-08). Zero-size
    // (the default) means just the field.
    void setKeepVisible(CGRect r);
    void setPointSize(float pt);
    void setText(const char* utf8);
    const std::string& text() const { return m_text; }
    void setPlaceholder(const char* utf8);
    // Bytes, not characters: the limit that matters is the storage it lands in
    // (WorldFileHeader::name is char[50]). A multi-byte character is never split.
    void setMaxBytes(int n)  { m_maxBytes = n; }

    bool hit(float x, float y) const;
    void focus();
    void blur();
    bool focused() const;

    // Call once per frame while the field is on screen. Return / the keyboard's Done is
    // EV_COMMIT, Escape is EV_CANCEL. EV_BLURRED is the platform ending text input on its own —
    // the iOS keyboard dismissed — which keeps the text and leaves the decision to the screen
    // (a dialog stays up; tapping the field again re-raises the keyboard). All three blur.
    Event update();
    void render() const;

private:
    TextField(const TextField&);
    TextField& operator=(const TextField&);
    void rebuild();
    void announce() const;
    bool applyInput(const char* bytes, int n, Event* ev);

    CGRect      m_rect, m_keep;
    float       m_pt;
    int         m_maxBytes;
    std::string m_text, m_placeholder;
    Label       m_label;
    bool        m_showingPlaceholder;
};

}  // namespace GLW

// The platform text-input seam (N.4.5). Native: native/src/seam/Input_native.cpp (SDL text input,
// the system keyboard on iOS). Web: stubs in web/src/seam/seam_link_stubs.mm returning 0 — the
// DOM owns text entry there until Stage 5.10, so a GL screen must check available() before
// offering an editable field. The rect is POINT space, y up, and only positions the IME / keyboard.
// take() drains typed UTF-8; editing keys arrive inline as '\b' (backspace), '\r' (return/done)
// and 0x1b (escape).
extern "C" {
// The raster seam's face switch (2026-10-05): 0 = body, 1 = display (Jersey 10). Sticky; Label
// sets it around its own rasters and puts it back to 0. Native: TextRaster_native.cpp (bundled
// Jersey10-Regular.ttf via stb_truetype, falling back to the body face); web: Texture2D_web.mm.
void eden_text_raster_set_face(int face);
int  eden_text_input_available(void);
void eden_text_input_start(float x, float y, float w, float h);
void eden_text_input_stop(void);
int  eden_text_input_active(void);
int  eden_text_input_take(char* buf, int cap);
// T.B1: the region to keep clear of an on-screen keyboard (point space, y up), set before
// eden_text_input_start(); zero-size = only the field. iOS folds it into the text-input area SDL
// lifts the view above the keyboard by; desktops ignore it so the IME still anchors on the field.
void eden_text_input_keep_visible(float x, float y, float w, float h);
// Stage 5.6: mouse-wheel notches the platform saw while NOT in mouse-look, positive = away from
// the user (scroll up), drained by the call. Native: Input_native.cpp. Web: 0 (the DOM scrolls).
int  eden_ui_take_wheel(void);
}

#endif
