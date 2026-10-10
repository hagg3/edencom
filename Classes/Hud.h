//
//  Hud.h
//  prototype
//
//  Created by Ari Ronen on 10/16/10.
//  Copyright 2010 __MyCompanyName__. All rights reserved.
//
#ifndef Eden_Hud_h
#define Eden_Hud_h


#import "Input.h"
#import "Util.h"
#import "glu.h"
#import "Terrain.h"
#import "Graphics.h"
#import "World.h"
#import "Gamepad.h"
#import "Joystick.h"
#import "statusbar.h"
#import "Globals.h"
#define MODE_CAMERA 0
#define MODE_PICK_BLOCK 1
#define MODE_BUILD 2
#define MODE_MINE 3
#define MODE_BURN 4
#define MODE_PAINT 5
#define MODE_PICK_COLOR 6
#define MODE_NONE -1


#define NUM_DISPLAY_BLOCKS 35
#define NUM_COLORS (6*9)

// D.2p: the block picker's second page (the 2026 game's layout, in id order): 112..127, then the
// SIGN and CMD tools. The tools are SENTINELS, not block ids: they can never be a block8 or index
// a [NUM_BLOCKS+1] table, so every table read on a picker cell goes through hud_picker_is_tool().
#define NUM_PICKER_PAGES 2
#define NUM_PICKER_PAGE2_CELLS 18
#define HUD_TOOL_SIGN 1000
#define HUD_TOOL_CMD 1001
static inline bool hud_picker_is_tool(int type){ return type>=HUD_TOOL_SIGN; }

// Host hook: return false to suppress the built-in in-game menu screen (the 4-icon GL panel
// Hud::renderMenuScreen draws over the world). NULL — the default, and the only value the iOS
// target ever sees — means "always draw", i.e. stock behaviour. The web port installs a hook that
// returns false unless the player has opted back into the legacy GL UI, because it draws its own
// DOM menu in that panel's place and the two would otherwise be visible at the same time.
//
// A hook rather than a plain bool because there is then no flag for anyone to keep in sync: the
// answer is recomputed at the one moment it is needed. It cannot be a --wrap on renderMenuScreen()
// — that call is intra-TU, so the compiler resolves it directly and the linker never sees it.
extern bool (*eden_hud_draw_menu_screen_hook)(void);

class Hud {
public:
	int fps;
	int fpsc;
	int mode;
	float test_a;
    bool hideui;
    bool take_screenshot;
    bool underLiquid;
    bool inmenu;
    bool heartbeat;
    bool m_jump;
    bool m_crouch;  // web port touch crouch button (Player::preupdate OR's this with keyboard CROUCH_HELD)
    int justLoaded;
	int blocktype;
    int block_paintcolor;
    int holding_creature;
    int creature_color;
    int goldencubes;
    int build_size;
	float flash;
    float fade_out;
    
    Vector flashcolor;
   
	 int blocktype_pressed;
    Button rmine,rburn,rbuild,rjumphit,rjumprender,rpaint;
    Button rcrouchhit,rcrouchrender;
    
    Button rcam,rexit,rsave,rhome,rmenu;
    
	int leftymode;
	//Gamepad* gamepad;
	Joystick* joystick;
	statusbar* sb;
    CGRect rpaintframe;
    CGRect rmenuframe;
    Button rtSave,rtHome,rtCam,rtExit;
    // Stage 5.5: the in-game menu is a GL-kit panel. Two actions stock never had (Resume, and
    // Settings on targets where the GL settings screen is live); the kit state lives behind a
    // pointer so this header stays includable without GLWidgets.h.
    Button rresume,rsettings;
    struct HudPauseKit* pauseKit;
    
    float var1,var2,var3;
	CGRect blockBounds[NUM_DISPLAY_BLOCKS];
    // D.2p: which picker page is showing, and its ◀ ▶ strip under the grid (layoutForScreen()).
    // blockBounds[] is the same grid on both pages; page 2 fills its first 18 cells.
    int pickerPage;
    CGRect pickerPrev,pickerNext;
	//Texture2D* blockIcons[NUM_DISPLAY_BLOCKS];
    CGRect colorBounds[NUM_COLORS];
    //Vector hudColor[NUM_COLORS];
    color8 paintColor;
    Vector liquidColor;
	//Texture2D* colorIcons[NUM_COLORS];
	float ttime;
	int use_joystick;
    
    Hud();
    // Recomputes every screen-space rect from the CURRENT SCREEN_WIDTH/SCREEN_HEIGHT/IS_WIDESCREEN.
    // Split out of the constructor (which used to inline all of it) so the point space can change
    // after the Hud exists — the web port derives it from the real window aspect and a UI-scale
    // setting, and re-runs this on resize. Idempotent: every margin it consumes is reset at the
    // top, so calling it twice gives the same answer as calling it once.
    void layoutForScreen();
    BOOL handlePickBlock(int x,int y);
    // D.2p: step the block picker's page by `delta` (wraps). FALSE (and nothing happens) unless
    // the block picker is open — the ◀ ▶ taps, [ ] and the wheel all come through here.
    BOOL pickerPageStep(int delta);
    // D.2p: the type in picker cell `i` of `page` (-1 = no cell there).
    static int pickerCell(int page,int i);
    static int pickerCellCount(int page);
    // D.2p: a tool cell is selectable only once its row lands (SIGN: D.3c, CMD: D.4b).
    static bool pickerToolEnabled(int type);
    BOOL handlePickColor(int x,int y);
    BOOL handlePickMenu(int x,int y);
    void worldLoaded();
    BOOL update(float etime);
    void render();
    void asetHome();
    void awarpHome();
    
    static void genColorTable();
    
private:
    void renderColorPickScreen();
    void renderBlockAndBorder(CGRect recto);
    void renderMenuScreen();
    void layoutPauseMenu();       // Stage 5.5: the six action rects + the panel, from SCREEN_*
    void renderBlockScreen();
    
    BOOL pickSecondBlock;
};


/*
 - (BOOL)handlePickBlock:(int)x:(int)y;
 - (BOOL)handlePickColor:(int)x:(int)y;
 - (BOOL)handlePickMenu:(int)x:(int)y;
 - (void)worldLoaded;
 - (BOOL)update:(float)etime;
 - (void)render;
 - (void)asetHome;
 - (void)awarpHome;
 +(void)genColorTable;
 
 @property(nonatomic,readonly) int fps;
@property(nonatomic,assign) BOOL m_jump,m_left,m_right,m_fwd,m_back,m_joy,underLiquid,heartbeat;
@property(nonatomic,assign) int mode,build_size;
@property(nonatomic,assign) int leftymode;
@property(nonatomic,assign) int blocktype,block_paintcolor,creature_color;
@property(nonatomic,readonly) color8 paintColor;
@property(nonatomic,assign) Vector liquidColor,flashcolor;
@property(nonatomic,assign) float flash,var1,var2,var3,fade_out;
@property(nonatomic,readonly) float test_a;
@property(nonatomic,assign) BOOL hideui;
@property(nonatomic,readonly) statusbar* sb;
@property(nonatomic,assign) int use_joystick,justLoaded;
@property(nonatomic,assign) BOOL take_screenshot,inmenu;
@property(nonatomic,assign) int holding_creature,goldencubes;*/

#endif
