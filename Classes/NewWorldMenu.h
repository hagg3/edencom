//
//  NewWorldMenu.h
//  Eden
//
//  Stage U.5 (WORKING/ui-rework-plan-2026-10-11.md): the New World screen, opened by the main
//  menu's New. Stock 2.1.1 had no such screen: New added "World N" to the list at once and the
//  world type was a UIAlertView raised when it was first played (Alert.mm's showAlertWorldType),
//  and the port grew a second dialog for the height (S.5e). This screen asks all of it up front,
//  on the settings screen's layout (the kit's RailFrame): an IconRail with one tile per world type
//  (Flat · Normal = FileManager::genflat), and the options as TwoLineRows in a row-snapped
//  variable-height ScrollView — a "World name" TextField and a "Height" choice (New Dawn 256 /
//  Classic 64). Play creates the list entry with that name, parks the type and height in the
//  one-shot pending seam (port/src/seam/Menu.cpp) that showAlertWorldType() and
//  FileManager::probeWorldHeight() consume, and starts the load; Back creates nothing.
//
//  STAGE H / H.4 filled it: a third tile, Classic (the seeded 1.x generator, Classes/
//  TerrainGenerator.h), with its own rows -- a Seed field with a Random button, Scale (a detented
//  slider, 256 only), Hilliness / Caves / Trees / Size choices, Flowers / Clouds toggles and a live
//  top-down Preview -- whose values Play parks as a GENERATOR record in the pending seam.
//
//  ROOM FOR STAGE H (as U.5 left it). The options are a table (kRows in the .mm) whose rows say which world types
//  they apply to, and the types are a table too (kTypes). H.3/H.4's seeded generator adds rows
//  (a seed, caves, presets) and, if it becomes a third type, a tile — the rail scrolls and the
//  content scrolls, so neither needs a layout change. A new row KIND (a toggle, a stepper) is the
//  one thing that owes code here: a case in controlSize()/layout()/update()/render().
//
#ifndef Eden_NewWorldMenu_h
#define Eden_NewWorldMenu_h

#include "GLWidgets.h"

#define NW_MAX_ROWS 16
#define NW_MAX_OPTS 5

class Texture2D;
struct ClassicGen;

class NewWorldMenu {
public:
    NewWorldMenu();
    ~NewWorldMenu();

    void open();            // fresh: empty name, last type + height kept from the previous visit
    void close();           // Back: creates nothing
    bool isOpen() const     { return m_open; }
    void back();            // Escape: closes (a focused field's Escape only blurs it, in the field)
    void update(float etime);
    void render();

    // --- harness surface (--ui-selftest / --shot): point space, y up ---------------------------
    // controlRect: "back", "play", "name". typeRect: a rail tile. choiceRect: an option button of a
    // NW_CHOICE row ("height", 0 = New Dawn 256). choice(): that row's selected VALUE (256 / 64).
// "name" is also a key for text()/focused() (and for controlRect: the field).
    CGRect controlRect(const char* which);
    CGRect typeRect(int t);
    int    typeCount() const;
    int    type() const     { return m_type; }
    bool   typeFlat(int t) const;
    const char* typeShot(int t) const;      // the --shot suffix ("flat", "normal")
    void   showType(int t);
    bool   choiceRect(const char* key, int option, CGRect* r);
    int    choice(const char* key) const;
    const char* text(const char* key) const;   // a NW_TEXT row's field, "" if none
    bool   focused(const char* key) const;
    void   blurFields();                       // --shot: the placeholder, not a caret
    // H.4: "dice" is a controlRect key too (the Seed row's Random button). rowVisible: the row is
    // in this type's list (Scale is not at 64). toggleRect/sliderRect: a NW_TOGGLE/NW_SLIDER row's
    // control (scrolled into view). seed(): the number Play would use (the field, or the rolled one).
    bool   rowVisible(const char* key) const;
    bool   toggleRect(const char* key, CGRect* r);
    bool   toggle(const char* key) const;
    bool   sliderRect(const char* key, CGRect* r);
    int    seed() const;
    void   classicSettings(ClassicGen* g) const;   // what Play would park (Classic tab)
    static int seedFromText(const char* utf8);     // digits as the number, else FNV-1a; [1, 2^24) minus 333333

private:
    NewWorldMenu(const NewWorldMenu&);
    NewWorldMenu& operator=(const NewWorldMenu&);

    void selectType(int t);
    void buildLabels();                  // title + buttons; on a type switch or a u() change
    void layout();
    void controlSize(int r, float* w, float* h) const;   // r = kRows index
    bool rowShown(int k) const;          // k = visible-row slot, inside the scroll window
    int  rowOf(const char* key) const;   // kRows index, or -1
    void play();
    void rollSeed();
    void updatePreview();                // H.4: rebuild the Classic preview when its inputs change
    const char* hintFor(int r);          // kRows[r].hint, or a live one (Scale's level, the seed)

    bool m_open;
    int  m_type;                         // kTypes index
    int  m_builtType;                    // -1 = labels stale
    float m_builtU;
    int  m_choice[NW_MAX_ROWS];          // per kRows index: the selected option
    int  m_heldRow, m_heldOpt;           // a choice button under a held touch

    int  m_nRows;                        // rows shown for this type
    int  m_rowIdx[NW_MAX_ROWS];          // slot -> kRows index

    GLW::Label      m_title;
    GLW::Button     m_back, m_play;
    GLW::IconRail   m_rail;
    GLW::ScrollView m_scroll;
    GLW::TwoLineRow m_row[NW_MAX_ROWS];
    GLW::Button     m_opt[NW_MAX_ROWS][NW_MAX_OPTS];
    GLW::TextField  m_field[NW_MAX_ROWS];   // per kRows index, NW_TEXT and NW_SEED rows
    GLW::Toggle     m_toggle[NW_MAX_ROWS];  // per kRows index, NW_TOGGLE rows
    GLW::Slider     m_slider[NW_MAX_ROWS];  // per kRows index, NW_SLIDER rows
    GLW::Button     m_dice;                 // the Seed row's Random
    std::string     m_hint[NW_MAX_ROWS];
    int  m_sliderRow, m_sliderTouch;     // a slider under a held touch
    bool m_diceHeld;
    int  m_randomSeed;                   // what a blank Seed becomes (rolled per visit and by Random)
    Texture2D* m_preview;                // H.4: the Classic preview (128x128 RGBA)
    int  m_previewKey[4];                // seed, scale x4, hilliness, height it was built for
    CGRect m_previewRect;
    std::string     m_autoName;          // the name row's placeholder: what a blank name becomes
    CGRect m_panel, m_content;
    float  m_titleY;
    int    m_scrollTouch, m_railTouch;
    bool   m_barDrag;
};

#endif
