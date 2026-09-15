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
//  drawing layer, Label (with word wrap) and Button, which is what GLDialog needed to match the
//  mockups. The rest land as their screens do (5.3-5.7) — add them HERE, not in a screen.
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
// One label = one or more rasterised lines. Word-wrapped at construction (the raster seam is
// single-line), so building one is a texture upload per line: build on show()/resize, never in a
// frame loop. `pt` is in POINTS; the SCALE_* correction the raster needs is applied inside.
class Label {
public:
    Label();
    ~Label();

    // maxWidth == 0 disables wrapping. align is UITextAlignmentLeft/Center/Right.
    void set(const char* text, float pt, UITextAlignment align, float maxWidth = 0.0f);
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
};

// --- Button --------------------------------------------------------------------------------
// A RAISED box with a centred chrome label that flips to PRESSED while held. Owns no input
// policy: screens still run the `itouch touches[]` / usage_id claim protocol themselves and call
// hit()/setPressed(), because who may claim a touch is a screen-level decision.
class Button {
public:
    Button();
    ~Button();

    void setLabel(const char* text, float pt = 15.0f);
    void setPointSize(float pt);              // re-wraps in place; call before setRect()
    void setRect(CGRect r);                   // re-runs the label's wrap against the new width
    CGRect rect() const { return m_rect; }

    bool pressed() const     { return m_pressed; }
    void setPressed(bool p)  { m_pressed = p; }
    bool hit(float x, float y) const;

    void render() const;

private:
    void rebuildLabel();

    Button(const Button&);
    Button& operator=(const Button&);

    Label       m_label;
    std::string m_text;
    CGRect      m_rect;
    float       m_pt;
    bool        m_pressed;
};

// The design system's 44pt touch floor (`pointer: coarse` in the CSS). Rows are laid out at
// max(design height, kTouchFloor) on every target — a desktop mouse loses nothing by it.
extern const float kTouchFloor;

}  // namespace GLW

#endif
