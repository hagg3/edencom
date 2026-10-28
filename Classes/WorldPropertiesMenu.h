//
//  WorldPropertiesMenu.h
//  Eden
//
//  Stage H / H.15 (WORKING/feature-archaeology-2026-10-13/world-properties-plan.md): the world
//  list's Properties screen, which replaced the list's Rename button (N.4.5) in the same slot.
//  The New World screen's frame (GLW::railFrame): titlebar Back · "<name> Properties" · Save, and
//  two rail tiles --
//    World   (UIICON_INFO): the world's last in-game photo (camera mode's `<world file>.png`, H.16)
//            with its name, height, type and file size beside it; the name field; read-only rows
//            (seed, height, saved land, signs, golden cubes, created / last saved, converted from,
//            original kept).
//    Terrain (UIICON_MOUNTAIN): a Classic world's generator settings -- New World's Classic rows
//            over the same option tables (ClassicOptions.h); seed and height are not editable (a
//            new seed is a new world). Flat / Normal show their type only; an unconverted `.eden`
//            says to play it once first.
//  Opened from the world list only, so the world is closed and nothing re-streams. Save is enabled
//  once something differs; a name-only change saves at once, a terrain change asks first (GLDialog).
//  Saving is FileManager::applyWorldProperties(): one committed batch -- a newer WORLD_HEADER and a
//  newer GENERATOR -- with no format change (the newest committed singleton wins). Back discards.
//
#ifndef Eden_WorldPropertiesMenu_h
#define Eden_WorldPropertiesMenu_h

#include "GLWidgets.h"
#include "WorldProps.h"
#include "Util.h"        // WorldNode
#include <string>

#define WP_MAX_ROWS 20   // both tabs: 8-10 World rows + 8 Terrain rows
#define WP_MAX_OPTS 5

class Texture2D;
struct NWOption;

class WorldPropertiesMenu {
public:
    WorldPropertiesMenu();
    ~WorldPropertiesMenu();

    // Reads the world (FileManager::readWorldProperties) and opens on the World tab. False: the file
    // could not be read, and nothing opens. `node` is the list entry, renamed in place on a save.
    bool open(WorldNode* node);
    void close();
    bool isOpen() const       { return m_open; }
    void back();              // Escape / Back: discards, as New World's Back does
    void update(float etime);
    void render();

    // What the Save button does: nothing while saveEnabled() is false; the confirm first when the
    // terrain changed. --ui-selftest drives it through a tap; this is the same call.
    void save();
    bool saveEnabled() const;
    bool nameChanged() const;
    bool terrainChanged() const;
    void editedGenerator(ClassicGen* g) const;   // the Terrain tab's values over the world's record
    int  lastResult() const   { return m_lastResult; }   // the last applyWorldProperties() (-2 none)
    const WorldProps& props() const { return m_props; }

    // --- harness surface (--ui-selftest / --shot / --props-selftest): point space, y up ---------
    // controlRect: "back", "save", "name" (the field), "photo". tabRect: a rail tile (0 World,
    // 1 Terrain). rowVisible: the key is in the current tab's list. value(): a read-only row's text.
    CGRect controlRect(const char* which);
    CGRect tabRect(int t);
    int    tab() const        { return m_tab; }
    void   showTab(int t);
    bool   rowVisible(const char* key) const;
    bool   rowEditable(const char* key) const;
    bool   choiceRect(const char* key, int option, CGRect* r);
    int    choice(const char* key) const;          // the selected option's VALUE, -1 if none
    bool   toggleRect(const char* key, CGRect* r);
    bool   toggle(const char* key) const;
    bool   sliderRect(const char* key, CGRect* r);
    const char* value(const char* key) const;
    const char* text(const char* key) const;
    bool   focused(const char* key) const;
    void   blurFields();
    bool   hasPhoto() const   { return m_photo != NULL; }

    enum { TAB_WORLD = 0, TAB_TERRAIN = 1, TABS = 2 };

private:
    WorldPropertiesMenu(const WorldPropertiesMenu&);
    WorldPropertiesMenu& operator=(const WorldPropertiesMenu&);

    struct Row {
        int kind;
        const char* key;
        std::string title, hint, value;
        const NWOption* opts; int nOpts;
        int tab;
    };
    void addRow(int tab, int kind, const char* key, const char* title, const char* hint,
                const char* value = "", const NWOption* opts = NULL, int nOpts = 0);
    void buildRows();
    void buildLabels();
    void layout();
    void controlSize(int r, float* w, float* h) const;
    bool rowShown(int k) const;
    int  rowOf(const char* key) const;          // m_rows index, or -1
    bool reveal(int r);
    void selectTab(int t);
    void loadPhoto();
    void commit();                              // applyWorldProperties, then close or report
    std::string trimmedName() const;
    static void confirmCb(int chosen);

    bool m_open;
    int  m_tab;
    WorldNode* m_node;
    std::string m_fileName;
    WorldProps m_props;
    int  m_lastResult;

    Row  m_rows[WP_MAX_ROWS];
    int  m_nAll;                                // rows built for this world (both tabs)
    int  m_choice[WP_MAX_ROWS];
    int  m_heldRow, m_heldOpt;
    int  m_nRows;                               // rows shown for this tab
    int  m_rowIdx[WP_MAX_ROWS];
    bool m_labelsBuilt;
    float m_builtU;
    int  m_builtTab;

    GLW::Label      m_title;
    GLW::Button     m_back, m_save;
    GLW::IconRail   m_rail;
    GLW::ScrollView m_scroll;
    GLW::TwoLineRow m_row[WP_MAX_ROWS];
    GLW::Button     m_opt[WP_MAX_ROWS][WP_MAX_OPTS];
    GLW::Label      m_value[WP_MAX_ROWS];
    GLW::Toggle     m_toggle[WP_MAX_ROWS];
    GLW::Slider     m_slider[WP_MAX_ROWS];
    GLW::TextField  m_name;
    int  m_sliderRow, m_sliderTouch;

    // The photo row (World tab, first): the picture or its placeholder, and four lines beside it.
    Texture2D* m_photo;
    float  m_photoAspect;
    CGRect m_photoRect;
    float  m_photoRowTop;
    GLW::Label m_photoName, m_photoLine[3], m_noPhoto;

    CGRect m_panel, m_content;
    float  m_titleY;
    int    m_scrollTouch, m_railTouch;
    bool   m_barDrag;
};

#endif
