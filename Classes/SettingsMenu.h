//
//  SettingsMenu.h
//  prototype
//
//  Created by Ari Ronen on 11/4/10.
//  Copyright 2010 __MyCompanyName__. All rights reserved.
//
#ifndef Eden_SettingsMenu_h
#define Eden_SettingsMenu_h



#include "Texture2D.h"
#include "Input.h"
#include "GLWidgets.h"
#include <string>
#include <vector>

class KeybindsMenu;
typedef struct{
	int value;
	const char* name;   // G.2b-1: the eden_prefs key
	CGRect box;
	Texture2D* tex;
}property;
#define NUM_PROP 5

// Phase N Stage 2.5: the GL settings screen is a generic list over the shared kSettings[] table
// (port/src/seam/Settings.cpp), reached through eden_settings_* accessors — see SettingsMenu.mm.
// Stage 5.4 reskinned it onto the GL widget kit (Classes/GLWidgets.h).
// `properties[]` and load()/save()/getNewWorldName() are untouched: the engine-visible meaning of
// the five engine-backed toggles and the prefs round-trip (eden_prefs since G.2b-1) still live there, and
// Settings.cpp's ENG_* mapping still indexes it.
//
// Stage U.3 (WORKING/ui-rework-plan-2026-10-11.md §3) rebuilt the screen on the reference layout: a
// FIXED panel (the same size on every tab), a vertical IconRail of 7 tabs, the tab's rows as
// TwoLineRows (title + hint) on one CONTENT surface, a row-snapped variable-height ScrollView, and
// dependent rows indented and muted while their parent toggle is off. The pages, the pager and the
// group headings are gone. SM_MAX_ROWS is only the per-tab array capacity.
#define SM_MAX_ROWS 32

class SettingsMenu{
public:
    SettingsMenu();
    ~SettingsMenu();
    void update(float etime);
    void render();
    void save();
    void load();
    std::string getNewWorldName();
    // Stage 5.3's programmatic entry point into the keybinds list; since U.4 it selects the Keys
    // tab. Exists so harnesses can reach it without hit-testing a rail tile.
    void showKeybinds();
    // Stage 5.5: open on the first tab of Settings proper — not on whatever child or tab a
    // previous visit left behind. The in-game pause menu calls this as it opens the screen.
    void resetView();
    // U.3: Escape. Cancels an armed key capture, else closes the whole screen (as Back does).
    void back();
    CGRect backRect() const { return m_back.rect(); }   // --ui-selftest: the titlebar's Back
    CGRect defaultsRect() const { return m_defaults.rect(); }   // the Keys tab's Defaults
    bool keyRect(int keybind, CGRect* r);              // Keys tab: that action's key button
    // Harness entry points: --shot captures every tab, and --ui-selftest asks where a row's control
    // is (showRow switches to that row's tab, scrolls it into view and returns the toggle / slider /
    // stepper rect in point space) instead of hard-coding a layout. The Keys tab has no settings
    // rows (tabRowCount 0): its content is KeybindsMenu. tabName() is the shot suffix ("gameplay").
    int  tabCount() const;
    void showTab(int t);
    const char* tabName(int t) const;
    int  tabRowCount(int t);
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
    void build();                      // kSettings[] rows -> tabs, once
    void selectTab(int t);             // switch tab: reset scroll, rebuild that tab's labels
    void buildRows();                  // the current tab's row labels + widget state
    void layout();                     // panel/rail/rows from SCREEN_* (per frame; labels only on change)
    void controlSize(int si, float* w, float* h) const;
    void refreshValue(int k);          // row k's value text / widget value from the model
    void stepRow(int k, int dir);
    void editText(int k);              // MP.8: the Matchmakers prompt
    static void textPromptCb(int chosen, const char* text);      // KIND_ENUM stepper, one step
    void commitSlider(int k);
    bool rowMuted(int k) const;        // its parent toggle is off
    bool rowShown(int k) const;        // inside the scroll window this frame

    bool m_built;
    bool m_seeded;                     // eden_settings_loaded() seen; rows live from then on
    int  m_tab;                        // selected tab
    int  m_builtTab;                   // the tab buildRows() last built, -1 = stale
    std::vector<std::vector<int> > m_tabRows;   // per tab, kSettings[] indices in table order

    GLW::Label      m_title;
    GLW::Button     m_back;
    GLW::Button     m_defaults;            // U.4: the Keys tab's titlebar action
    GLW::IconRail   m_rail;
    GLW::ScrollView m_scroll;
    int  m_nRows;                      // rows on the current tab
    int  m_si[SM_MAX_ROWS];
    GLW::TwoLineRow m_row[SM_MAX_ROWS];
    GLW::Label   m_valueLabel[SM_MAX_ROWS];   // a slider's readout
    GLW::Toggle  m_toggle[SM_MAX_ROWS];
    GLW::Slider  m_slider[SM_MAX_ROWS];
    GLW::Stepper m_stepper[SM_MAX_ROWS];
    GLW::Button  m_textBtn[SM_MAX_ROWS];      // MP.8: a KIND_TEXT row's "None, Add" / "2 addresses, Edit"
    CGRect m_panel, m_content;
    float  m_titleY;
    float  m_builtU;                   // u() the labels were built at; a change rebuilds them
    int    m_dragSlot, m_dragTouch;    // the slider being dragged, and by which touch
    int    m_scrollTouch;              // a touch scrolling the content (or the bar), -1 none
    bool   m_barDrag;
    int    m_railTouch;                // a touch that went down on the rail, -1 none
    KeybindsMenu* m_keys;              // U.4: the Keys tab's content
};


#endif
