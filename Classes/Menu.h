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
    // can drive the same path the dialog does.
    GLW::Button rect_rename;
    bool renameOffered();
    void beginRename();
    bool renameSelected(const char* utf8);
    // Stage S / S.5: "Share" beside Rename — Export / Upload / Remove original (Classes/WorldShare.h).
    GLW::Button kit_share;      // (stock's rect_share is the old carousel's share corner, unused)
    bool shareShown();

    // Stage 5.6: the main menu on the GL widget kit — a WINDOW under the logo holding a titlebar
    // (Settings · Worlds · New), the world list (GLW::ListRow in a row-snapped GLW::ScrollView)
    // and an action bar (Delete · Rename · Play), with the status line under it. The stock
    // carousel's rects above (rect_options, rect_create, the arrows, ...) are still computed by
    // layoutForScreen() but no longer drawn or hit-tested. The kit's state lives in Menu.mm.
    struct MenuKit* kit;
    void layoutKit();
    void renderKit();
    void tapRow(int index);
    // --ui-selftest / --shot: where a control is, in point space (y up). `which` is one of
    // "settings", "new", "play", "delete", "rename", "list", "scrollbar"; rowRect() answers for a
    // world index, and false if that row is scrolled out of view. "getworlds" since 5.9.
    CGRect controlRect(const char* which);
    bool   rowRect(int index, CGRect* r);
    int    firstVisibleRow();
    int    visibleRows();

    // Stage 5.9: "Get Worlds" — the online browser (archive + the current and legacy Eden
    // servers), a full screen over the menu background like settings. Offered only where the host
    // can fetch (WorldBrowser::available(); never on web, whose DOM screen is its browser).
    WorldBrowser* browser;
    BOOL showbrowser;
    bool browserOffered();
    void openBrowser();

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

