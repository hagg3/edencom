//
//  SettingsMenu.h
//  prototype
//
//  Created by Ari Ronen on 11/4/10.
//  Copyright 2010 __MyCompanyName__. All rights reserved.
//
#ifndef Eden_SettingsMenu_h
#define Eden_SettingsMenu_h



#import "Texture2D.h"
#import "Input.h"
#import "GLWidgets.h"
#include <string>
#include <vector>

class KeybindsMenu;
typedef struct{
	int value;
	NSString* name;
	CGRect box;
	Texture2D* tex;
}property;
#define NUM_PROP 5

// Phase N Stage 2.5: the GL settings screen is a generic paginated list over the shared
// kSettings[] table (web/src/seam/Settings_web.mm), reached through eden_settings_* accessors —
// see SettingsMenu.mm. Stage 5.4 reskinned it onto the GL widget kit (Classes/GLWidgets.h):
// Toggle / Slider / Stepper rows in a WINDOW panel, the same shape as KeybindsMenu.
// `properties[]` and load()/save()/getNewWorldName() are untouched: the engine-visible meaning of
// the five engine-backed toggles and the NSUserDefaults round-trip still live there, and
// Settings_web.mm's ENG_* mapping still indexes it.
//
// Rows per page is NOT fixed (2026-10-05): fit() derives it from the screen height and the row
// pitch, which is compact on a pointer and the 44pt touch floor on touch, so a desktop window
// shows ~3x the rows a phone does. SM_ROWS_PER_PAGE is only the array capacity.
#define SM_ROWS_PER_PAGE 16

class SettingsMenu{
public:
    SettingsMenu();
    ~SettingsMenu();
    void update(float etime);
    void render();
    void save();
    void load();
    NSString* getNewWorldName();
    // Stage 5.3's programmatic entry point into the keybinds screen — the same thing the
    // "Controls" button does. Exists so the --shot harness and --keybind-selftest can reach the
    // screen without hit-testing a button rect whose position is not what they are testing.
    void showKeybinds();
    // Stage 5.5: open on page 1 of Settings proper — not on whatever child or page a previous
    // visit left behind. The in-game pause menu calls this as it opens the screen.
    void resetView();
    CGRect backRect() const { return m_back.rect(); }   // --ui-selftest: the titlebar's Back
    // Stage 5.4's harness entry points, same reasoning: --shot captures every page, and
    // --ui-selftest asks where a row's control is (showRow switches to that row's page and
    // returns the toggle / slider / stepper rect in point space) instead of hard-coding a layout.
    int  pageCount();
    void showPage(int p);
    bool showRow(const char* key, CGRect* control);

	CGRect rect_settings;


	// Stock 2.1.1 fields. Nothing reads them since Stage 2.5/5.4 (the screen draws from the
	// kit), kept so this header still diffs cleanly against pristine-engine.
	Button rect_save;
	Button rect_on[NUM_PROP];

	CGRect rect_off;
	int world_counter;
	property properties[NUM_PROP];

private:
    // One slot on a page: a group heading (si < 0) or a kSettings[] row.
    struct Item { int si; int group; };

    void fit();                        // row pitch + rows per page from SCREEN_*; re-pages on change
    void build();                      // visible rows -> pages, once settings exist
    void buildPage();                  // the current page's labels + widget state
    void syncPage();                   // buildPage() when the page or the loaded state changed
    void layout();                     // recompute rects from SCREEN_* (cheap, per frame)
    void refreshValue(int k);          // slot k's value text / widget value from the model
    void stepRow(int k, int dir);      // KIND_ENUM / KIND_RANGE stepper, one step
    void commitSlider(int k);

    bool m_built;
    bool m_seeded;                     // eden_settings_loaded() seen; rows live from then on
    int  m_rowsPerPage;                // fit()'s answer; build() pages against it
    int  m_maxPageLen;                 // the longest page, so the window keeps one height
    float m_pitch, m_btnH;             // fit()'s row pitch and button height, points
    int  m_page;
    int  m_laidOutPage;
    std::vector<std::vector<Item> > m_pages;
    std::vector<std::string> m_groups;

    GLW::Label   m_title;
    GLW::Label   m_pageLabel;
    GLW::Button  m_back, m_keysBtn, m_prev, m_next;
    GLW::Label   m_rowLabel[SM_ROWS_PER_PAGE];
    GLW::Label   m_valueLabel[SM_ROWS_PER_PAGE];   // a slider's readout
    GLW::Toggle  m_toggle[SM_ROWS_PER_PAGE];
    GLW::Slider  m_slider[SM_ROWS_PER_PAGE];
    GLW::Stepper m_stepper[SM_ROWS_PER_PAGE];
    CGRect m_slot[SM_ROWS_PER_PAGE];
    CGRect m_panel, m_content;
    float  m_titleY;
    int    m_dragSlot, m_dragTouch;    // the slider being dragged, and by which touch
    KeybindsMenu* m_keys;
};


#endif
