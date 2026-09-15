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
#include <vector>

class KeybindsMenu;
typedef struct{
	int value;
	NSString* name;
	CGRect box;
	Texture2D* tex;
}property;
#define NUM_PROP 5

// Phase N Stage 2.5: the GL settings screen is now a generic paginated list over the shared
// kSettings[] table (web/src/seam/Settings_web.mm), reached through eden_settings_* accessors —
// see SettingsMenu.mm. `properties[]` and load()/save()/getNewWorldName() are untouched: the
// engine-visible meaning of the five engine-backed toggles and the NSUserDefaults round-trip
// still live there, and Settings_web.mm's ENG_* mapping still indexes it.
#define SM_ROWS_PER_PAGE 7
// "-" "+" "<" ">" "On" "Off" "Keys"
#define SM_NUM_GLYPH 7

class SettingsMenu{
public:
    SettingsMenu();
    ~SettingsMenu();
    void update(float etime);
    void render();
    void save();
    void load();
    NSString* getNewWorldName();
    // Stage 5.3's programmatic entry point into the keybinds screen — the same thing the "Keys"
    // button does. Exists so the --shot harness and --keybind-selftest can reach the screen
    // without hit-testing a button rect whose position is not what they are testing.
    void showKeybinds();

	CGRect rect_settings;


	Button rect_save;
	Button rect_prev;
	Button rect_next;
	// Stage 5.3's entry point. The keybinds screen is a CHILD of this one, not a sibling: it is
	// reachable only from inside update()/render(), both of which web --wraps to no-ops, so the
	// stage's stated web-regression risk is structurally zero for it. See KeybindsMenu.h.
	Button rect_keys;


	Button rect_on[NUM_PROP];

	CGRect rect_off;
	int world_counter;
	property properties[NUM_PROP];

private:
    void buildTextures();
    void refreshSeededValues();        // rebuild range/enum value textures once settings load
    void layout();                     // recompute the per-frame control rects from SCREEN_*
    void stepRow(int si, int dir);     // KIND_RANGE / KIND_ENUM +/- one step
    void rebuildValueTex(int si);

    int m_page;
    int m_rowsPerPage;
    std::vector<int> m_vis;            // kSettings[] indices shown on this build, in order
    std::vector<Texture2D*> m_label;   // per kSettings index (NULL if not built)
    std::vector<Texture2D*> m_value;   // per kSettings index (KIND_RANGE/ENUM only)
    std::vector<Texture2D*> m_groupHdr;// per kSettings index, set on the first visible row of a group
    Texture2D* m_glyph[SM_NUM_GLYPH];  // "-" "+" "<" ">" "On" "Off" "Keys"
    bool m_built;
    bool m_seeded;                     // value textures refreshed once eden_settings_init() ran
    Button m_ctlA[SM_ROWS_PER_PAGE];   // toggle rect, or the dec/"<" rect
    Button m_ctlB[SM_ROWS_PER_PAGE];   // the inc/">" rect (KIND_RANGE/ENUM only)
    KeybindsMenu* m_keys;
};


#endif
