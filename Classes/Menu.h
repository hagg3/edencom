//
//  Menu.h
//  prototype
//
//  Created by Ari Ronen on 10/24/10.
//  Copyright 2010 __MyCompanyName__. All rights reserved.
//
#ifndef Eden_Menu_h
#define Eden_Menu_h


#import "Texture2D.h"
#import "SettingsMenu.h"
#import "NewWorldMenu.h"
#import "Input.h"
#import "ShareUtil.h"
#import "statusbar.h"
#import "SharedList.h"
#import "ShareMenu.h"
#import "Util.h"
#import "Menu_background.h"
#import "GLWidgets.h"
#import "WorldBrowser.h"


class ShareMenu;
// H.15: not imported here -- WorldPropertiesMenu.h carries a ClassicGen (TerrainGenerator.h), and
// Terrain.h reaches this file through Player.h/World.h before Terrain is declared.
class WorldPropertiesMenu;
// C.3: the Console screen; ConsoleMenu.h pulls in Console.h/CmdScript.h, which Menu.h's includers don't need.
class ConsoleMenu;
// MP.6: the Multiplayer screen (Classes/ServerListMenu.h).
class ServerListMenu;



class Menu{
    
public:
		CGRect rect_name;
	
	WorldNode* world_list;
	WorldNode* world_list_end;
	WorldNode* selected_world;
	BOOL activeLeftArrow;
	BOOL activeRightArrow;
	
	
	CGRect rect_loading;
	Button rect_options;
	
	Button rect_share;
	
	Button rect_loadshared;
		ShareUtil* shareutil;
	Button rect_delete;
	
	Button rect_create;
	
	Button left_arrow;
	Button right_arrow;
    
	
	statusbar* sbar;
	statusbar* fnbar;
	SettingsMenu* settings;
	SharedList* shared_list;
	ShareMenu* share_menu;
	Menu_background* menu_back;
	
	int loading;
	int loading_world_list;
    // MP.3a: a multiplayer session's "mp:<host>:<port>" while it loads (MpWorld::openSession); nil otherwise.
    // The loading branch below plays it instead of selected_world.
    NSString* session_world_name;
	BOOL delete_mode;
	int is_sharing;
	BOOL share_mode;
	BOOL showsettings;
	BOOL showlistscreen;
    BOOL loadShared(SharedListNode* sharedNode);
    // Recomputes every screen-space rect from the CURRENT SCREEN_WIDTH/SCREEN_HEIGHT. Split out of
    // the constructor so the point space can change after construction; see Hud::layoutForScreen.
    void layoutForScreen();
    void update(float etime);
    void loadWorlds();
    void render();
    void refreshfn();
    void addWorld(WorldNode* node);
    void removeWorld(WorldNode* node);
    
    void activate();
    void deactivate();
    
    // N.4.5: rename the selected world. A GL text prompt (GLDialog::prompt) behind a "Rename"
    // button under the carousel, offered only where the host has GL text entry (not web, whose
    // DOM menu owns world naming). renameSelected() is the prompt's commit, public so a harness
    // can drive the same path the dialog does. H.15: the button is "Properties" now (below), and
    // the name is edited on that screen; the prompt is no longer reached from the list.
    GLW::Button rect_rename;
    bool renameOffered();
    void beginRename();
    bool renameSelected(const char* utf8);
    // Stage S / S.5: "Share" beside Rename — Export / Upload / Remove original (Classes/WorldShare.h).
    GLW::Button kit_share;      // (stock's rect_share is the old carousel's share corner, unused)
    bool shareShown();

    // Stage 5.6: the main menu on the GL widget kit — a WINDOW under the logo holding a titlebar
    // (Settings · Worlds · New; since I.7 Back · My Worlds · New, and no logo), the world list (GLW::ListRow in a row-snapped GLW::ScrollView)
    // and an action bar (Delete · Rename · Play), with the status line under it. The stock
    // carousel's rects above (rect_options, rect_create, the arrows, ...) are still computed by
    // layoutForScreen() but no longer drawn or hit-tested. The kit's state lives in Menu.mm.
    struct MenuKit* kit;
    void layoutKit();
    void renderKit();
    void tapRow(int index);
    // --ui-selftest / --shot: where a control is, in point space (y up). `which` is one of
    // "settings", "new", "play", "delete", "rename", "list", "scrollbar"; rowRect() answers for a
    // world index, and false if that row is scrolled out of view. "getworlds" since 5.9; "properties"
    // (= "rename", the same button) since H.15.
    // I.7: "back" (the list's titlebar) and the home screen's "home.myworlds", "home.multiplayer",
    // "home.getworlds", "home.settings"; the old "settings"/"getworlds" keys answer the home's
    // Settings button and Get Worlds tile (they left the list's titlebar).
    CGRect controlRect(const char* which);
    bool   rowRect(int index, CGRect* r);

    // Stage I / I.7: the home screen, the first thing after boot — the logo, a subtitle, three tiles
    // (My Worlds -> the list above; Multiplayer -> ServerListMenu since MP.6; Get Worlds) and Settings
    // beneath. The list ("My Worlds") is the screen with showhome FALSE: it has Back and no logo.
    // Quit to Menu (activate()) and a Get Worlds download land on the list.
    BOOL showhome;
    void layoutHome();
    void renderHome();
    void updateHome(float etime);
    void showHome(bool home);
    int    firstVisibleRow();
    int    visibleRows();

    // Stage 5.9: "Get Worlds" — the online browser (archive + the current and legacy Eden
    // servers), a full screen over the menu background like settings. Offered only where the host
    // can fetch (WorldBrowser::available(); never on web, whose DOM screen is its browser).
    WorldBrowser* browser;
    BOOL showbrowser;
    bool browserOffered();
    void openBrowser();

    // Stage U.5: "New" opens the New World screen (name, type, height) instead of adding "World N"
    // to the list at once; a full screen over the menu background like settings. Its Play calls
    // startNewWorld(): the list entry, selected, and loading=1 — the type and height are already
    // parked in the pending seam, so the stock world-type prompt never shows for it.
    NewWorldMenu* newworld;
    BOOL shownewworld;
    void openNewWorld();
    void startNewWorld(NSString* name);

    // Stage H / H.15: "Properties" (the rect_rename button, which was N.4.5's Rename) opens
    // World Properties for the selected world, a full screen over the menu background like New
    // World. The world is closed, so its Save writes the file directly (FileManager::applyWorldProperties).
    WorldPropertiesMenu* props;
    BOOL showprops;
    bool propertiesOffered();
    void openProperties();

    // Stage C / C.3: the Console (Classes/ConsoleMenu.h) over a RUNNING world -- unlike showsettings it
    // freezes nothing; World::update runs console->update first and then the whole play step without
    // player input. In play only (opened from the pause menu or the `console` key).
    ConsoleMenu* console;
    BOOL showconsole;

    // Stage MP / MP.6: "Multiplayer" -- the saved server list, its forms and the join's Connecting
    // window (Classes/ServerListMenu.h), opened by the home screen's tile. Drawn in place of the home
    // screen / world list (not as an early-returning screen) so the loading ladder below still runs
    // when a join reaches "Loading world". A session's Exit lands here (activate()).
    ServerListMenu* servers;
    BOOL showservers;
    void openServers();

    void a_genFlat(BOOL b);
    void a_deleteCancel();
    void a_deleteConfirm();
    Menu();
    
	//CGRect rcam
};
/*
@property(nonatomic,assign) int loading, is_sharing;
@property(nonatomic,assign) BOOL showsettings,showlistscreen;
@property(nonatomic,readonly) statusbar* sbar;
@property(nonatomic,readonly) WorldNode* selected_world;
@property(nonatomic,readonly) SharedList* shared_list;
@property(nonatomic,readonly) ShareUtil* shareutil;*/

#endif

