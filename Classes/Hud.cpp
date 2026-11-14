//
//  Hud.m
//  prototype
//
//  Created by Ari Ronen on 10/16/10.
//  Copyright 2010 __MyCompanyName__. All rights reserved.
//

extern bool pendingRespawnSound;
#include "Hud.h"
#include "GLWidgets.h"
#include <algorithm>
#include <cstring>
#include "Globals.h"
#include "Frustum.h"
#include "TerrainGen2.h"


#include "Alert.h"
#include "GLDialog.h"
#include "MpWorld.h"   // MP.3a: pause-menu gates
#include "ChatFeed.h"   // MP.7: the feed and the chat button
#include "Menu.h"
#include "BlockTraits.h"   // K.2-1: per-type behaviour
#include "ContentPack.h"   // K.2-5: the Mods page
#include "BlockRegistry.h"
#include "Hotbar.h"      // UX.8-3: applyHand / noteHand
#include "BlockIcon.h"   // UX.8-1: the build button's block picture

// FM.3: fly state for the jump/crouch/cycle buttons (Player.mm; fly_* are declared in Player.h)
extern bool FLY_ALLOWED;
bool DEV_CONSOLE_ALLOWED=false;   // Settings > Experiments > Allow developer console: the singleplayer HUD console button
static inline bool consoleBtnShown(){ return DEV_CONSOLE_ALLOWED&&!MpWorld::active(); }
extern bool FLY_MODE;
int  fly_get_state();
void fly_cycle_state();

extern float SCREEN_WIDTH; 
extern float SCREEN_HEIGHT;
extern float P_ASPECT_RATIO;

static float HUDR_X= 427.0f;
static float HUD_BOX_SIZE= 45.0f;
static float HUDR_NUM= 5.0f;
static int BLOCK_ICON_SPACING =15;
static int BLOCK_ICON_SIZE =38;
static int COLOR_ICON_SPACING =9;
static int COLOR_ICON_SIZE =32;
// K.2-3: the two picker pages and hudBlocksMap (a picker cell's BT "second block" variant; -1 none)
// come from the stock pack's "picker_pages" and each block's "bt" (media/packs/eden/pack.json;
// ContentPack::commitStock -> setPickerPages at boot). Page 1 is the stock grid of 35 cells; page 2 is
// D.2p's 2026 second page in ID order, H.14's grass3, FT.1's weeds, then the sign and command tools.
static int hudBlocks[NUM_DISPLAY_BLOCKS];
extern "C" int hudBlocksMap[BLOCK_IDS]={};
static int hudBlocks2[NUM_PICKER_PAGE2_CELLS];
void Hud::setPickerPages(const std::vector<int>& page0,const std::vector<int>& page1){
    for(int i=0;i<NUM_DISPLAY_BLOCKS;i++) hudBlocks[i]=i<(int)page0.size()?page0[i]:TYPE_NONE;
    for(int i=0;i<NUM_PICKER_PAGE2_CELLS;i++) hudBlocks2[i]=i<(int)page1.size()?page1[i]:TYPE_NONE;
}
// K.2-5 (D13): the Mods pages. Cells are the loaded mod blocks whose pack says "picker": true, in load
// order, 35 to a page after the two stock pages; none in a multiplayer session (a session loads no packs).
static std::string s_modSel;          // the selected mod block's full name, kept across worlds
static bool s_modConfirmed=false;     // this world's first-placement confirm was answered OK
static std::vector<int> modCells(){
    std::vector<int> v;
    if(MpWorld::active()) return v;
    const ContentPack::ModReport& r=ContentPack::modReport();
    if(r.session) return v;
    for(size_t i=0;i<r.blocks.size();i++) if(r.blocks[i].picker) v.push_back((int)i);
    return v;
}
int Hud::pickerPageCount(){
    const int n=(int)modCells().size();
    return NUM_PICKER_PAGES+(n+NUM_DISPLAY_BLOCKS-1)/NUM_DISPLAY_BLOCKS;
}
const std::string& Hud::modSelection(){ return s_modSel; }
std::string Hud::modCellName(int type){
    const ContentPack::ModReport& r=ContentPack::modReport();
    const int i=type-HUD_MOD_BASE;
    return (i>=0&&i<(int)r.blocks.size())?r.blocks[i].name:std::string();
}
static int mod_sentinel_of(const std::string& name){
    const ContentPack::ModReport& r=ContentPack::modReport();
    for(size_t i=0;i<r.blocks.size();i++) if(r.blocks[i].name==name) return HUD_MOD_BASE+(int)i;
    return -1;
}
int Hud::pickerCellCount(int page){
    if(page>=NUM_PICKER_PAGES){
        const int n=(int)modCells().size()-(page-NUM_PICKER_PAGES)*NUM_DISPLAY_BLOCKS;
        return n<0?0:(n>NUM_DISPLAY_BLOCKS?NUM_DISPLAY_BLOCKS:n);
    }
    return page==1?NUM_PICKER_PAGE2_CELLS:NUM_DISPLAY_BLOCKS;
}
int Hud::pickerCell(int page,int i){
    if(i<0||i>=pickerCellCount(page)) return -1;
    if(page>=NUM_PICKER_PAGES) return HUD_MOD_BASE+modCells()[(page-NUM_PICKER_PAGES)*NUM_DISPLAY_BLOCKS+i];
    return page==1?hudBlocks2[i]:hudBlocks[i];
}
// The cells of `page` into out[NUM_DISPLAY_BLOCKS]; returns the count.
static int page_cells(int page,int* out){
    const int n=Hud::pickerCellCount(page);
    for(int i=0;i<n;i++) out[i]=Hud::pickerCell(page,i);
    return n;
}
// SIGN landed with D.3c, CMD with D.4b. Both need a text prompt (a sign's text, a command block's
// script), so both stay veiled on a host with no GL text input (web, until 5.9's web text-input
// seam -- un-veiling there is this line's job); a veiled cell's tap does nothing.
bool Hud::pickerToolEnabled(int type){
    if(type==HUD_TOOL_SIGN||type==HUD_TOOL_CMD) return GLDialog::textEntryAvailable();
    return false;
}
// The BT ("second block") variant of a picker cell, -1 for none — and always -1 for a tool,
// which must never index hudBlocksMap.
static int picker_bt(int type){ return hud_picker_is_tool(type)?-1:hudBlocksMap[type]; }
static CGRect picker_hit(CGRect r){
    const float h=std::max((float)r.size.height,GLW::touchFloor());
    // Grown DOWNWARD only: centred, it would reach into the grid's last row and steal its taps.
    return CGRectMake(r.origin.x,r.origin.y+r.size.height-h,r.size.width,h);
}
// K.2-3: the page a cell is on, from the pages themselves (was a hard-coded id list of page 2).
static int picker_page_of(int type){
    if(type>STOCK_BLOCK_MAX&&!hud_picker_is_tool(type)&&!s_modSel.empty()) type=mod_sentinel_of(s_modSel);   // a resolved mod id
    if(hud_picker_is_mod(type)){
        const std::vector<int> v=modCells();
        for(size_t i=0;i<v.size();i++) if(HUD_MOD_BASE+v[i]==type) return NUM_PICKER_PAGES+(int)i/NUM_DISPLAY_BLOCKS;
        return 0;
    }
    for(int i=0;i<NUM_PICKER_PAGE2_CELLS;i++) if(hudBlocks2[i]==type) return 1;
    return 0;
}

static int marginVert=10;
static int marginLeft=10;
static int marginLeft2=10-6;
//static int marginRight=60;
Vector colorTable[256];

//@implementation Hud
//@synthesize mode,blocktype,leftymode,use_joystick,paintColor,liquidColor,block_paintcolor,heartbeat,goldencubes,build_size,fade_out;
//@synthesize m_jump,m_back,m_fwd,m_left,m_right,test_a,m_joy,fps,justLoaded,flash,flashcolor;
//@synthesize sb,hideui,take_screenshot,underLiquid,holding_creature,creature_color,inmenu,var1,var2,var3;




static Vector test1;
static Vector test2;
static CGRect test1r;
static CGRect test2r;
extern bool IS_WIDESCREEN;

void Hud::genColorTable(){
   
    int c;
    int r;
    c=r=0;
    colorTable[0].x=1.0f;
    colorTable[0].y=1.0f;
    colorTable[0].z=1.0f;
    for(int i=0;i<NUM_COLORS;i++){
		
		        float red,g,b;
        if(c==8){
            if(r==5)
                HSVtoRGB(&red,&g,&b,1,0,0.01f);
            else
                HSVtoRGB(&red,&g,&b,1,0,1.0f-r/5.0f);
        }else{
            float hu=c;
            if(c==2)
                hu-=.6f;
            if(r<3)
                HSVtoRGB(&red,&g,&b,360.0f*(float)hu/8,(float)(r+1)/3,1);
            else
                HSVtoRGB(&red,&g,&b,360.0f*(float)hu/8,1,1.0f-(float)(r-2)/4);  /*
                                                                                 int hu=c;
                                                                                 if(r>=3)hu+=8;
                                                                                 if(r==0||r==3)
                                                                                 HSVtoRGB(&red,&g,&b,360.0f*(float)hu/16,0.5f,1);
                                                                                 else if(r==1||r==4)
                                                                                 HSVtoRGB(&red,&g,&b,360.0f*(float)hu/16,1,1);
                                                                                 else
                                                                                 HSVtoRGB(&red,&g,&b,360.0f*(float)hu/16,1,0.5f);*/
        }
        colorTable[i+1].x=red;
        colorTable[i+1].y=g;
        colorTable[i+1].z=b;
        
		c++;
		if(c>8){
			c=0;
			r++;
		}
	}

    
    
}
// ---------------------------------------------------------------------------------------------
// Stage 5.5 — the in-game (pause) menu on the GL widget kit.
// ---------------------------------------------------------------------------------------------
// Stock drew a 2010 frame texture with four icon buttons and four text-image buttons beside them
// (rsave/rhome/rcam/rexit + rtSave/...), hit-tested by handlePickMenu(). That input path is KEPT
// — the claim on touch-down (inbox3), the action on release (inbox2), the actions themselves —
// and only two things change: layoutPauseMenu() places those same Button rects as a column of
// kit buttons in a WINDOW panel, and renderMenuScreen() draws the kit instead of the textures.
// Each rt* text rect now equals its icon rect (the label is ON the button), so every existing
// `inbox2(r)||inbox2(rt)` test still means "this button".
//
// Two actions are new, both from the DOM pause menu (web-port-final:web/public/eden-pausemenu.js): Resume, and
// Settings — the GL SettingsMenu as an in-game modal (World::update), offered only where that
// screen is live (eden_gl_settings_available(): not on web, where the DOM owns settings).
//
// The engine's own action icons stay, drawn on the left of each button — design-system.md's
// "Action icons" rule: the same painted objects the 2010 HUD used for these actions.
extern "C" int eden_gl_settings_available(void);   // port/src/seam/Settings.cpp (both targets)
extern "C" int eden_get_hotbar_shown(void);       // Settings.cpp: Settings > Gameplay > Hotbar (UX.8-4)
extern "C" int eden_get_crosshair(void);           // Settings.cpp: Settings > Interface > Crosshair
extern "C" void eden_console_menu_open(void);      // Menu.cpp (C.3): opens ConsoleMenu over the world
extern "C" int  eden_profile_touch_chrome(void);   // DisplayProfile.cpp (UX.3): hide the jump button on keyboard + mouse
extern "C" int  eden_chat_open(int slash);          // Menu.cpp (MP.7): the Console screen on its Chat tile

enum { PM_RESUME, PM_SAVE, PM_HOME, PM_PHOTO, PM_SETTINGS, PM_CONSOLE, PM_QUIT, PM_COUNT };
static const char* kPauseLabels[PM_COUNT] = {
    "Resume", "Save game", "Warp home", "Take photo", "Settings", "Console", "Quit to menu"   // ui-label
};

struct HudPauseKit {
    GLW::Label  title;
    GLW::Button btn[PM_COUNT];
    CGRect      panel;
    float       titleTop;
    bool        built;
    HudPauseKit() : titleTop(0), built(false) { panel = CGRectMake(0, 0, 0, 0); }
};

Hud::Hud(){
    pauseKit=new HudPauseKit();

    fps=60;
	flash=-1;
    build_size=1;
    flashcolor=MakeVector(1.0,1.0,1.0);
   //  printf("flashcolor memlocation1: %X",(unsigned int)(&flashcolor));
	hideui=false;
	ttime=0;
    blocktype_pressed=-1;
    pickerPage=0;
    justLoaded=1;
    inmenu=false;
    underLiquid=false;
    m_crouch=false;
    m_flyup=m_flydown=false;
    // G.2s (UBSan): stock never set these; Hud is heap-new'd, so update()'s first wasJumpPressed and
    // render()'s first take_screenshot test read garbage (a load of "190" as bool was the report).
    m_jump=false;
    take_screenshot=false;
    // G.5b-0 (--scene-shot found it): the same class. ObjC's alloc zeroed paintColor, so stock started
    // with no paint colour; heap garbage here drew the paint button in a random colour each launch.
    paintColor=0;
    fade_out=0;
	test_a=0;
	mode=MODE_MINE;
	blocktype=TYPE_BRICK;
    holding_creature=false;
    block_paintcolor=0;

    Hud::genColorTable();

    // Both of these size themselves from layoutForScreen() below (statusbar::pos is public, and
    // Joystick::layoutForScreen() moves the pad in from the safe left edge, I.2), so they are
    // constructed once here and merely repositioned on a later re-layout rather than being rebuilt.
	sb= new statusbar(CGRectMake(0,0,0,0));
    sb->useToast();   // N.5.11: every HUD message is the GL kit's toast (GLWidgets.h)
	//gamepad=[[Gamepad alloc] init];
	joystick=new Joystick();

    test1r=CGRectMake(0,0,50,50);
    test2r=CGRectMake(0,0,100,100);
    test1=MakeVector(randf(1),randf(1),randf(1));
    test2=colorTable[lookupColor(test1)];

    this->layoutForScreen();
}

// Everything below used to live inline in the constructor above. It is unchanged arithmetic except
// where marked "adaptive point space": the original only ever ran against one of two hard-coded
// profiles (480x320 or 568x320 points), so a handful of offsets were hand-tuned constants for the
// wider of the two. Those now keep their stock value at exactly 568 points and split any width
// beyond it evenly, which is what makes a derived, window-shaped point space (web port: audit
// D1/D4) land the same UI in the same place instead of pinning it to the left edge.
void Hud::layoutForScreen(){
    // The margins are file-scope statics that the body below MUTATES (marginLeft2 += ...). Reset
    // them from their compile-time values first or a second call compounds the adjustment.
    marginVert=10;
    marginLeft=10;
    marginLeft2=10-6;

    // I.2: every edge-anchored element anchors to the SAFE rect (the screen less the island,
    // rounded corners and home bar; GLW::safeRect). With no insets sR/sT are SCREEN_WIDTH/HEIGHT
    // and s0x/s0y are 0, so every number below is the stock one.
    const CGRect safe=GLW::safeRect();
    const float s0x=safe.origin.x, s0y=safe.origin.y;
    const float sR=safe.origin.x+safe.size.width, sT=safe.origin.y+safe.size.height;

	HUDR_X=sR-HUD_BOX_SIZE-13;

	rjumprender.origin.x=HUDR_X;
	rjumprender.origin.y=s0y;
	rjumprender.size.width=45;
	rjumprender.size.height=45;
   // if(IS_IPAD){
        rjumprender.origin.y+=23;
        rjumprender.origin.x-=17;
    
    rmenu.origin.x=s0x;
    rmenu.origin.y=sT-45;
    rmenu.size.width=45;
    rmenu.size.height=45;
    rchat.origin.x=s0x+50;   // MP.7
    rchat.origin.y=sT-45;
    rchat.size.width=45;
    rchat.size.height=45;
    rcons=rchat;   // the singleplayer console button takes the chat button's place
   // }else{
    //    rjumprender.origin.y+=13;
    //    rjumprender.origin.x-=7;
    //}
    // FM.3: the jump, crouch/down and fly-cycle buttons sit HUD_FLY_GAP points apart. inbox2/inbox3 add
    // 4pt of slop on every side, so a gap under 8 made neighbouring hit rects overlap (a thumb hit
    // both); 10 leaves 2pt of dead zone. Hit rects equal the render rects (jump's runs on to the
    // screen edge and bottom, as stock).
    const float flyGap=10;
    rjumphit.origin.x=rjumprender.origin.x;
	rjumphit.origin.y=s0y;
	rjumphit.size.width=sR- rjumphit.origin.x;
	rjumphit.size.height=45+rjumprender.origin.y-s0y;

    // Crouch button: same icon as jump, drawn mirrored on the X axis, sitting just to its left.
    // While flying this slot is Fly down.
    rcrouchrender.origin.x=rjumprender.origin.x-45-flyGap;
    rcrouchrender.origin.y=rjumprender.origin.y;
    rcrouchrender.size.width=45;
    rcrouchrender.size.height=45;
    rcrouchhit.origin.x=rcrouchrender.origin.x;
    rcrouchhit.origin.y=s0y;
    rcrouchhit.size.width=45;
    rcrouchhit.size.height=45+rcrouchrender.origin.y-s0y;

    // FM.3 fly-mode cycle button, left of the crouch/down slot.
    rflyrender.origin.x=rcrouchrender.origin.x-45-flyGap;
    rflyrender.origin.y=rjumprender.origin.y;
    rflyrender.size.width=45;
    rflyrender.size.height=45;
    rflyhit.origin.x=rflyrender.origin.x;
    rflyhit.origin.y=s0y;
    rflyhit.size.width=45;
    rflyhit.size.height=45+rflyrender.origin.y-s0y;
    rflyrender.pressed=false;

	CGRect sbrect;
	sbrect.origin.x=s0x+20;
	sbrect.size.width=safe.size.width-40;
    
	sbrect.origin.y=s0y;
	sbrect.size.height=25;
	
    int roffy=3;
    // Adaptive point space: stock spread this four-button column PROPORTIONALLY down the right
    // edge — `k*SCREEN_HEIGHT/HUDR_NUM - HUD_BOX_SIZE - roffy` for k = 5,4,3,2. At the 320-point
    // height the engine was drawn for, that is a 64-point pitch running down from the top edge and
    // it reads as one toolbar; scaled into a taller point space the buttons stay 45 points but the
    // gaps grow with the screen, so it stops reading as a group. Keep the pitch the 320-point
    // layout had and anchor the column to the top edge, which is where it already sat (`k==5` is
    // flush against it). Identical to the stock arithmetic at SCREEN_HEIGHT==320.
    const float hudr_pitch=(float)IPHONE_HEIGHT/(HUDR_NUM);
    const float hudr_top=sT-(int)(HUD_BOX_SIZE)-roffy;
	rbuild.origin.x=HUDR_X;
	rbuild.origin.y=hudr_top-hudr_pitch*2;
	rbuild.size.width=rbuild.size.height=HUD_BOX_SIZE;
	rmine.origin.x=HUDR_X;
	rmine.origin.y=hudr_top-hudr_pitch;
	rmine.size.width=rmine.size.height=HUD_BOX_SIZE;
	rburn.origin.x=HUDR_X;
	rburn.origin.y=hudr_top;
	rburn.size.width=rburn.size.height=HUD_BOX_SIZE;
    rpaint.origin.x=HUDR_X;
	rpaint.origin.y=hudr_top-hudr_pitch*3+5;
	rpaint.size.width=rpaint.size.height=HUD_BOX_SIZE;
	int c=0;
	int r=0;
	/*if(IS_IPAD){
		BLOCK_ICON_SIZE=70;
		BLOCK_ICON_SPACING=50;
		rjumprender.origin.x-=25;
		rjumprender.origin.y+=40;
		marginVert*=2;
		marginLeft*=2;
		marginRight*=1.5;
		sbrect.size.height*=2;
	}*/
    if(IS_WIDESCREEN){
       // printg("It's widescreen homie!\n");
        marginLeft2+=57;

    }
    // Adaptive point space: the +57 above is the hand-tuned nudge that re-centred the 7-column
    // block/colour grid when the iPhone-5 profile added 88 points of width over the 480 one. Any
    // width past that profile is split evenly here, so the grid (and rpaintframe, which shares
    // marginLeft2) keeps the same position relative to the screen centre at any width, and 568
    // points still produces byte-identical numbers to the stock layout.
    // I.2: centred on the safe rect (the same centre while the insets are symmetric).
    marginLeft2+=(int)s0x;
    if(safe.size.width>IPHONE5_WIDTH)
        marginLeft2+=(int)((safe.size.width-IPHONE5_WIDTH)/2.0f);
	for(int i=0;i<NUM_DISPLAY_BLOCKS;i++){
		
		blockBounds[i].origin.x=7+BLOCK_ICON_SPACING+marginLeft2+(BLOCK_ICON_SPACING+BLOCK_ICON_SIZE)*c;
		blockBounds[i].origin.y=-3+sT-marginVert-BLOCK_ICON_SPACING-BLOCK_ICON_SIZE-
        (                                                                                                BLOCK_ICON_SPACING+BLOCK_ICON_SIZE)*r;									 ;
		blockBounds[i].size.height=BLOCK_ICON_SIZE;
		blockBounds[i].size.width=BLOCK_ICON_SIZE;

		c++;
		if(c>6){
			c=0;
			r++;
		}
	}
    // D.2p: the page strip, under the grid's last row: ◀ under the first column, ▶ under the
    // last, the page dots between them (renderBlockScreen). The grid itself is untouched, so page
    // 1's hit rects stay byte-identical (headless-display-profile-test pins them).
    {
        const CGRect first=blockBounds[NUM_DISPLAY_BLOCKS-7], last=blockBounds[NUM_DISPLAY_BLOCKS-1];
        const float h=22.0f;
        // I.2: never into the home-bar band; on a 320-point phone that eats into the 6-point gap.
        const float py=std::max((float)(first.origin.y-6.0f-h),s0y);
        pickerPrev=CGRectMake(first.origin.x,py,first.size.width,h);
        pickerNext=CGRectMake(last.origin.x,py,last.size.width,h);
    }

    c=r=0;
    for(int i=0;i<NUM_COLORS;i++){
		
		colorBounds[i].origin.x=5+4+COLOR_ICON_SPACING+marginLeft2+(COLOR_ICON_SPACING+COLOR_ICON_SIZE)*c;
		colorBounds[i].origin.y=(-5)+(-5)+sT-marginVert-BLOCK_ICON_SPACING-COLOR_ICON_SIZE-
        (                                                                                                COLOR_ICON_SPACING+COLOR_ICON_SIZE)*r;									 ;
		colorBounds[i].size.height=COLOR_ICON_SIZE;
		colorBounds[i].size.width=COLOR_ICON_SIZE;
        		c++;
		if(c>8){
			c=0;
			r++;
		}
	}
   	rhome.origin.y=rsave.origin.y=rexit.origin.y=rcam.origin.y=marginVert+26;
	int magic_number=40;
	/*if(IS_IPAD){
		magic_number=85;
	}*/
	
	rhome.origin.x=marginLeft+8*magic_number-30;
	rsave.origin.x=marginLeft+2*magic_number-30;
	rexit.origin.x=marginLeft+6*magic_number-30;
	rcam.origin.x=marginLeft+4*magic_number-30;
	rcam.size.width=rhome.size.width=rsave.size.width=rexit.size.width=45;
	rcam.size.height=rhome.size.height=rsave.size.height=rexit.size.height=45;
	sb->pos=sbrect;

	rpaintframe.origin.x=marginLeft2;
	rpaintframe.origin.y=marginVert+10;
	rpaintframe.size.width=402;
	rpaintframe.size.height=282;
    // Adaptive point space, and this one is not cosmetic. rpaintframe is the fixed 402x282 card
    // drawn BEHIND the colour swatch grid (and, via renderBlockScreen, behind the block grid). The
    // grids are anchored to the TOP of the screen — every one of their Y values is computed as
    // `SCREEN_HEIGHT - margins - ...` — while this constant Y anchors the card to the BOTTOM. At
    // 320 points the two coincide exactly, which is why the coupling was invisible for 15 years; at
    // any taller point space the card slides out from under its own contents and you get an empty
    // panel with a grid of swatches floating above it. Anchor it to the edge the grids use.
    if(sT>IPHONE_HEIGHT)
        rpaintframe.origin.y+=sT-IPHONE_HEIGHT;


    rmenuframe.origin.x=110;
  
	rmenuframe.origin.y=40;
	rmenuframe.size.width=SCREEN_WIDTH-200;
    
    if(IS_WIDESCREEN){
        rmenuframe.origin.x+=50;
        rmenuframe.size.width=SCREEN_WIDTH-300;
    }
	rmenuframe.size.height=SCREEN_HEIGHT-80;
    // Adaptive point space: the in-game menu panel is a fixed-size card, not a stretch-to-fit one
    // (its frame texture and its four buttons are all absolute sizes), so past the profile it was
    // drawn for it keeps that size and centres instead of growing to fill the screen.
    if(SCREEN_WIDTH>IPHONE5_WIDTH){
        rmenuframe.origin.x+=(SCREEN_WIDTH-IPHONE5_WIDTH)/2.0f;
        rmenuframe.size.width-=(SCREEN_WIDTH-IPHONE5_WIDTH);
    }
    if(SCREEN_HEIGHT>IPHONE_HEIGHT){
        rmenuframe.origin.y+=(SCREEN_HEIGHT-IPHONE_HEIGHT)/2.0f;
        rmenuframe.size.height-=(SCREEN_HEIGHT-IPHONE_HEIGHT);
    }
    rsave.origin.x=rmenuframe.origin.x+17;
    rhome.origin.x=rmenuframe.origin.x+17;
    rexit.origin.x=rmenuframe.origin.x+17;
    rcam.origin.x=rmenuframe.origin.x+17;
    int sep=50;
    rsave.origin.y=rmenuframe.origin.y+rmenuframe.size.height-60-sep*0;
    rhome.origin.y=rmenuframe.origin.y+rmenuframe.size.height-60-sep*1;
    rexit.origin.y=rmenuframe.origin.y+rmenuframe.size.height-60-sep*3;
    rcam.origin.y=rmenuframe.origin.y+rmenuframe.size.height-60-sep*2;
    
    
    rtSave=ButtonMake(rsave.origin.x+57,rsave.origin.y+5,180,56/2+10);
    rtHome=ButtonMake(rhome.origin.x+57,rhome.origin.y+5,180,56/2+10);
    rtCam=ButtonMake(rcam.origin.x+57,rcam.origin.y+5,180,56/2+10);
    rtExit=ButtonMake(rexit.origin.x+57,rexit.origin.y+5,180,56/2+10);
    layoutPauseMenu();   // Stage 5.5: replaces the four rects above with the kit column
    if(joystick) joystick->layoutForScreen();   // I.2: the stick pad follows the safe left edge
}

void Hud::layoutPauseMenu(){
    using namespace GLW;
    ::Button* r[PM_COUNT]={&rresume,&rsave,&rhome,&rcam,&rsettings,&rconsole,&rexit};
    const bool withSettings=eden_gl_settings_available()!=0;
    // MP.3a: a multiplayer session has no world file, no home and no thumbnail (engine plan §11), so
    // Save Game (which also sets home), Warp Home and Take Photo go the way Settings does without a GL screen.
    const bool session=MpWorld::active();
    auto hidden=[&](int k){ return k==PM_CONSOLE||(k==PM_SETTINGS&&!withSettings)||(session&&(k==PM_SAVE||k==PM_HOME||k==PM_PHOTO)); };
    int n=0;
    for(int k=0;k<PM_COUNT;k++) if(!hidden(k)) n++;
    // Gap > 8pt on purpose: inbox2/inbox3 add 4pt of slop on every side, and a smaller gap would
    // let one release land on two buttons — handlePickMenu runs every matching action.
    const float btnH=controlH(34);
    const float gap=std::max(du(10),10.0f), pad=du(14), titleH=du(40);
    const float bw=du(240);
    const CGRect safe=safeRect();   // I.2: centred on the safe rect
    // I.4: one column when it fits the safe rect, else Resume full-width on top and the rest in
    // pairs (a 320-point phone needs 7 rows otherwise). The gap between columns is the same > 8pt.
    float ph=pad+titleH+n*btnH+(n-1)*gap+pad;
    const bool twoCol=ph>safe.size.height;
    int rows=n;
    if(twoCol){
        rows=1+(n-1+1)/2;
        ph=pad+titleH+rows*btnH+(rows-1)*gap+pad;
    }
    const float colW=twoCol?bw*0.75f:bw;   // each of the pair: wide enough for "Quit to menu"
    const float fullW=twoCol?colW*2.0f+gap:bw;
    const float pw=fullW+pad*2.0f;
    const float px=safe.origin.x+(safe.size.width-pw)*0.5f, py=safe.origin.y+(safe.size.height-ph)*0.5f;
    pauseKit->panel=CGRectMake(px,py,pw,ph);
    pauseKit->titleTop=py+ph-pad;
    float y=py+ph-pad-titleH-btnH;
    int slot=0;   // visible-button counter
    for(int k=0;k<PM_COUNT;k++){
        if(hidden(k)){
            r[k]->origin.x=r[k]->origin.y=-10000;          // off-screen: never hit
            r[k]->size.width=r[k]->size.height=0;
            continue;
        }
        if(!twoCol){
            r[k]->origin.x=px+pad; r[k]->origin.y=y;
            r[k]->size.width=bw;   r[k]->size.height=btnH;
            y-=btnH+gap;
        }else if(slot==0){
            r[k]->origin.x=px+pad; r[k]->origin.y=y;
            r[k]->size.width=fullW; r[k]->size.height=btnH;
        }else{
            const int row=1+(slot-1)/2, col=(slot-1)%2;
            r[k]->origin.x=px+pad+col*(colW+gap);
            r[k]->origin.y=y-row*(btnH+gap);
            r[k]->size.width=colW; r[k]->size.height=btnH;
        }
        slot++;
    }
    // The label is on the button now: each text rect IS its button (see the block comment above).
    rtSave=rsave; rtHome=rhome; rtCam=rcam; rtExit=rexit;
}
static float at1=0,at2=0,at3=0;
void Hud::worldLoaded(){
    blocktype_pressed=-1;
    inmenu=false;
    pickSecondBlock=false;
    at1=0;
    // K.2-5: a new world has its own palette and its own first-placement confirm; a mod selection is
    // re-pointed by NAME (the sentinel's index and the resolved id both belong to the previous load).
    s_modConfirmed=false;
    if(blocktype>STOCK_BLOCK_MAX&&!hud_picker_is_tool(blocktype)||hud_picker_is_mod(blocktype)){
        const int s=s_modSel.empty()?-1:mod_sentinel_of(s_modSel);
        blocktype=(s>=0&&!MpWorld::active())?s:TYPE_BRICK;
    }
	if(mode==MODE_PICK_BLOCK){
		mode=MODE_BUILD;
	}
    if(mode==MODE_PICK_COLOR){
        mode=MODE_PAINT;
    }
	
}
// K.2-5 (D13): the palette id is taken at the first PLACEMENT, never at selection. In an unflagged world
// that placement rewrites the file once (flag compaction), so the first one asks. Failures reuse the
// console's wording (BlockRegistry::acquire's `why`: palette full, locked, session) as a status toast.
static bool mod_acquire_now(Hud* h){
    std::string why;
    const int id=BlockRegistry::acquire(s_modSel,&why);
    if(id<0){ h->sb->setStatus(why.empty()?"Could not use this mod block":why.c_str(),4); return false; }
    h->blocktype=id;
    return true;
}
static void mod_confirm_cb(int chosen){
    if(chosen!=0) return;
    s_modConfirmed=true;
    mod_acquire_now(World::getWorld->hud);
}
bool Hud::resolveModBlock(){
    if(!hud_picker_is_mod(blocktype)) return true;
    const std::string name=modCellName(blocktype);
    if(name.empty()){ blocktype=TYPE_BRICK; return false; }
    s_modSel=name;
    const int have=BlockRegistry::idOf(name);
    if(have>=0){ blocktype=have; return true; }
    if(MpWorld::active()){ sb->setStatus("Mod blocks are not available in a multiplayer world",4); return false; }
    if(!s_modConfirmed&&World::getWorld->fm&&!World::getWorld->fm->paletteFlagged()){
        if(!GLDialog::active()){
            static const char* const kBtn[]={"Place it","Cancel"};
            GLDialog::show("Use a mod block?",
                           "Worlds with mod blocks can't be opened by Emod builds older than 0.4.",
                           kBtn,2,mod_confirm_cb);
        }
        return false;
    }
    return mod_acquire_now(this);
}

// UX.8-3: the hotbar's side of the hand (Classes/Hotbar.h).
bool Hud::handAvailable(const HotbarSlot& s){
    if(s.kind==HOTBAR_COLOUR) return s.colour>=1&&s.colour<=NUM_COLORS;
    if(s.kind!=HOTBAR_BLOCK) return false;
    if(!s.mod.empty()) return !MpWorld::active()&&!ContentPack::modReport().session&&mod_sentinel_of(s.mod)>=0;
    if(s.type==HUD_TOOL_SIGN||s.type==HUD_TOOL_CMD) return pickerToolEnabled(s.type);
    if(s.type==TYPE_GOLDEN_CUBE) return goldencubes>0;   // as the picker
    return s.type>0&&s.type<=STOCK_BLOCK_MAX&&s.type!=TYPE_BEDROCK;
}
bool Hud::applyHand(const HotbarSlot& s,bool keepMode){
    if(!handAvailable(s)){
        if(!keepMode&&s.kind==HOTBAR_BLOCK&&!s.mod.empty())
            sb->setStatus(MpWorld::active()?"Mod blocks are not available in a multiplayer world"
                                           :"This mod block is not installed",4);
        return false;
    }
    if(s.kind==HOTBAR_COLOUR){
        paintColor=s.colour;
        if(!keepMode) mode=MODE_PAINT;
    }else{
        if(!s.mod.empty()){
            blocktype=mod_sentinel_of(s.mod);   // a palette id only at the first placement (resolveModBlock)
            s_modSel=s.mod;
        }else blocktype=s.type;
        block_paintcolor=s.colour;
        if(!keepMode) mode=MODE_BUILD;
    }
    if(!keepMode){   // an open picker is closed: nothing of it may commit later
        blocktype_pressed=-1;
        pickSecondBlock=false;
    }
    return true;
}
HotbarSlot Hud::handSlot(){
    if(mode==MODE_PAINT)
        return (paintColor>=1&&paintColor<=NUM_COLORS)?HotbarSlot::paint(paintColor):HotbarSlot();
    const int c=(block_paintcolor>=0&&block_paintcolor<=NUM_COLORS)?block_paintcolor:0;
    if(hud_picker_is_mod(blocktype)){
        const std::string n=modCellName(blocktype);
        return n.empty()?HotbarSlot():HotbarSlot::modBlock(n,c);
    }
    if(blocktype==HUD_TOOL_SIGN||blocktype==HUD_TOOL_CMD) return HotbarSlot::block(blocktype,c);
    if(blocktype>STOCK_BLOCK_MAX){   // a resolved mod block: by name, never by this world's id
        const char* n=BlockRegistry::nameOf(blocktype);
        return n?HotbarSlot::modBlock(n,c):HotbarSlot();
    }
    if(blocktype<=0||blocktype==TYPE_BEDROCK) return HotbarSlot();
    return HotbarSlot::block(blocktype,c);
}
void Hud::noteHand(){ Hotbar::get().noteHand(handSlot()); }

static const int usage_id=15;
static const int usage_id2=16;
static const int usage_hb=17;   // UX.8-5: a touch that went down on the hotbar strip
static int delayedaction=0;
static int delayedtimer=0;

// QA.5 (A2-01): Save & Quit whose save failed stays in the world and asks. Before, it tore the world
// down regardless, and every edit since the last commit went with it. "Leave without saving" is the
// way out when the disk cannot be freed; it goes through exit_to_menu, as the MP link's exit does.
extern bool exit_to_menu;   // World.cpp: renderFrame's end leaves the world
static void save_fail_exit_cb(int chosen){
    World* w=World::getWorld;
    if(chosen==0){                       // Try again: the same delayed Save & Quit
        delayedtimer=1;
        delayedaction=5;
        w->hud->sb->setStatus("Saving and quitting...",999);
    }else if(chosen==1){                 // Leave without saving
        fprintf(stderr,"emod: left the world without saving (%s)\n",w->fm->lastSaveError().c_str());
        exit_to_menu=true;
        Resources::getResources->playSound(S_EXIT_WORLD);
    }                                    // Stay: the status line already says why
}
static void show_save_fail_exit(){
    static const char* const kBtn[]={"Try again","Leave without saving","Stay"};
    static std::string body;
    body="The changes since the last save are still here. "+
         std::string(World::getWorld->fm->lastSaveError().find("disk full")!=std::string::npos
                     ?"The device may be full: free some space, then try again."
                     :"Reason: "+World::getWorld->fm->lastSaveError());
    GLDialog::show("Could not save this world",body.c_str(),kBtn,3,save_fail_exit_cb);
}
extern int chunks_rendered;
extern int chunks_rendered2;
extern int vertices_rendered;
extern int max_vertices;
extern float P_ZFAR;
extern bool SUPPORTS_OGL2;

int flamecount=0;
static int lmode=MODE_NONE;
//static int warpCount=0;

struct HbLayout{
    bool touch;
    int n,start;
    float cell,gap,left,y,stripW;
    CGRect strip;
};
static int s_hbSlide=-1,s_hbSlideSel=-2;   // UX.8-5: slid window start; the selection it was slid under
static float s_hbLabelT=0;                  // UX.8-4: seconds left on the strip's name label
static bool s_hbChoosing=false;   // UX.8-5: a picker is open for a slot (its status line is up)
static int hbCell[MAX_TOUCHES],hbBase[MAX_TOUCHES];   // UX.8-5: per-touch strip state (slot or chevron, window start at down)
static float hbHeld[MAX_TOUCHES];
static bool hbDone[MAX_TOUCHES],hbMoved[MAX_TOUCHES];
bool Hud::update(float etime){
    if(s_hbLabelT>0) s_hbLabelT-=etime;
    if(s_hbChoosing&&mode!=MODE_PICK_BLOCK&&mode!=MODE_PICK_COLOR){   // UX.8-5: picker closed (commit or Escape)
        s_hbChoosing=false;
        Hotbar::get().setPickerTarget(-1);
        sb->clear();
    }
	if(flash>0){
		flash-=etime;
	}
    if(pickSecondBlock&&mode!=MODE_PICK_BLOCK){
        pickSecondBlock=false;
        sb->clear();
    }
    if(World::getWorld->player->flash>0){
       // World::getWorld->player.flash-=etime;
    }
	if(World::getWorld->player->dead){
        if(fade_out<1){
            fade_out+=etime/4;
        }else if(fade_out>=1){
            printg("warping home\n");
            // QA.5: a respawn is a warp. On a failed save the player stays down and it retries after
            // the hold, rather than reload the window over the unsaved edits.
            if(!World::getWorld->fm->saveHeld()&&World::getWorld->terrain->warpToHome()){
                pendingRespawnSound=true;   // H.9: played by Terrain::render once firstframe clears
                printg("finished warping\n");
            }
        }
    }else{
        if(fade_out>0){
            if(etime>.1f)etime=.1f;
            fade_out-=etime/2.5f;
            if(fade_out<0)fade_out=0;
        }
    }
	ttime+=World::getWorld->realtime;
	fpsc++;
    if(justLoaded){
        ttime=0;
        fpsc=0;
        justLoaded++;
        if(justLoaded==3){
            justLoaded=0;
        }
    }
    if(inmenu){
        
        
        at1+=etime*6;
        if(at1>=1){
            at1=1;
        }
        
        
        
    }else {
        at1-=etime*6;
        if(at1<=0)
            at1=0;
    }
   //if(mode==MODE_PICK_BLOCK){
    
    // D.2p: the wheel over an open block picker turns its page (away from the user = back).
    // Native only: eden_ui_take_wheel() is 0 on web, whose wheel reaches eden_picker_page_step.
    if(mode==MODE_PICK_BLOCK){
        if(int wheel=eden_ui_take_wheel()) pickerPageStep(wheel>0?-1:1);
    }
    if(mode==MODE_PICK_BLOCK){
        at2+=etime*9;
        if(at2>=1)
            at2=1;
    }else{
        at2-=etime*9;
        if(at2<=0)at2=0;
    }
    
    if(mode==MODE_PICK_COLOR){
        at3+=etime*9;
        if(at3>=1)
            at3=1;
    }else{
        at3-=etime*9;
        if(at3<=0)at3=0;
    }
    
    heartbeat=false;
	if(ttime>=1.0f){
        heartbeat=true;
		fps=fpsc;
       // float fpsf=1/World::getWorld->realtime;
        test1=MakeVector(randf(1),randf(1),randf(1));
        test2=colorTable[lookupColor(test1)];
       //  [sb setStatus:[NSString stringWithFormat:@"FPS: %d chunks:%d vertices:%d",fpsc,var1,var2] :2];
       //[sb setStatus:[NSString stringWithFormat:@"FPS: %d player:(%.1f,%.1f)",fpsc,World::getWorld->player.pos.x,World::getWorld->player.pos.z] :2];
		//printg("Fps:%d   vertices:%d   max_vertices:%d chunks:%d.\n",fpsc,vertices_rendered,max_vertices,chunks_rendered);
       // float pz=P_ZFAR;
        if(!SUPPORTS_OGL2){
           /* if( fpsf<28){
                 if(P_ZFAR>15.0f)
                P_ZFAR-=.1f;
                
            }else if(fpsf>35){
                 if(P_ZFAR<T_SIZE)
                P_ZFAR+=.1f;
            }*/ 
            
        }else{
             //[Graphics setZFAR:T_SIZE];
            //P_ZFAR=T_SIZE;
            if(fpsc<=59){
               
              //  max_vertices-=100*(60-fpsc);
            }else if(fpsc>60&&vertices_rendered>=max_vertices){
                //max_vertices+=100;
            }
            /*
            if( fpsc<59){
                P_ZFAR-=(60-fpsc);
            }else if(fpsc>=60){
                if(P_ZFAR<T_SIZE)
                P_ZFAR+=1;
            }*/
        }
        
    
		fpsc=0;
		ttime=0;
        
	}
	if(delayedtimer==1){
        delayedtimer=0;

        
		if(delayedaction==6){
            // Play before warpToHome(): it calls loadTerrain(), which sets firstframe=TRUE for the
            // rest of this frame, and Resources::playSound drops everything while firstframe is set
            // -- called after, this sound was silently discarded every time.
            Resources::getResources->playSound(S_WARP_HOME_ACTIVATED);
			if(World::getWorld->terrain->warpToHome())sb->clear();   // QA.5: a failed save keeps its status line
		}else if(delayedaction==5){
            mode=MODE_NONE;
           // printg("trying to exit\n!!!");
			if(!World::getWorld->fm->saveWorld()&&!(g_save_test_ignore&SAVE_TEST_IGNORE_EXIT)){
                show_save_fail_exit();   // QA.5: stay in the world; the dialog offers retry / leave / stay
            }else{
                World::getWorld->exitToMenu();
                Resources::getResources->playSound(S_EXIT_WORLD);
                sb->clear();
            }
			
		}
        delayedaction=0;
				return false;
		
	}
  

    
    if(delayedtimer>1){delayedtimer--; return false;}
	
	Input* input=Input::getInput();
    itouch* touches=input->getTouches();
	
	
	
	
	for(int i=0;i<MAX_TOUCHES;i++){
        if(touches[i].inuse==usage_hb){   // UX.8-5: tap selects, hold clears, drag slides the window
            hbHeld[i]+=etime;
            const bool rel=touches[i].down==M_RELEASE;
            const int c=hbCell[i];
            const float dx=(float)(touches[i].mx-touches[i].fx);
            HbLayout L;
            if(!hbDone[i]&&!rel&&c>=0&&!hbMoved[i]&&hbHeld[i]>=0.5f){   // long-press: clear
                hbDone[i]=true;
                Hotbar::get().clear(c);
                sb->setStatus("Slot "+std::to_string(c+1)+" cleared",2.0f);
                Resources::getResources->playSound(S_MENU_BUTTON_RELEASE);
            }
            if(!hbDone[i]&&fabsf(dx)>10.0f&&hotbarLayout(&L)&&L.n<Hotbar::SLOTS){   // swipe: slide, never select
                hbMoved[i]=true;
                int st=hbBase[i]-(int)floorf(dx/(L.cell+L.gap)+0.5f);
                if(st<0)st=0;
                if(st>Hotbar::SLOTS-L.n)st=Hotbar::SLOTS-L.n;
                s_hbSlide=st;s_hbSlideSel=Hotbar::get().selected();
            }
            if(rel){
                if(!hbDone[i]&&!hbMoved[i]){
                    if(c>=0) hotbarActivate(c);
                    else if(hotbarLayout(&L)){   // chevron: slide by a window
                        int st=L.start+(c==-2?-L.n:L.n);
                        if(st<0)st=0;
                        if(st>Hotbar::SLOTS-L.n)st=Hotbar::SLOTS-L.n;
                        s_hbSlide=st;s_hbSlideSel=Hotbar::get().selected();
                        Resources::getResources->playSound(S_MENU_BUTTON_PRESS);
                    }
                }
                touches[i].inuse=0;
                touches[i].down=M_NONE;
            }
            continue;
        }
        if(touches[i].inuse==usage_id2&&touches[i].down==M_RELEASE){
            bool handled;
            Resources::getResources->playSound(S_MENU_BUTTON_RELEASE);
            if(inmenu){
                if(handlePickMenu(touches[i].mx,touches[i].my)){
                    
                    handled=true;
                }
                
            }
            if(FLY_ALLOWED&&!inmenu&&inbox2(touches[i].mx,touches[i].my,&rflyhit)){   // FM.3
                fly_cycle_state();
                rflyhit.pressed=false;
            }
            if(MpWorld::active()&&!inmenu&&inbox2(touches[i].mx,touches[i].my,&rchat)){   // MP.7
                eden_chat_open(0);
                rchat.pressed=false;
            }
            if(consoleBtnShown()&&!inmenu&&inbox2(touches[i].mx,touches[i].my,&rcons)){
                rcons.pressed=false;
                eden_console_menu_open();
                Input::getInput()->clearAll();
            }
            if(inbox2(touches[i].mx,touches[i].my,&rmenu)){	
                inmenu=!inmenu;
                if(inmenu==true){
                    rcam.pressed=rtCam.pressed=false;rhome.pressed=rtHome.pressed=false;
                    rsave.pressed=rtSave.pressed=false;rexit.pressed=rtExit.pressed=false;
                    rresume.pressed=rsettings.pressed=rconsole.pressed=false;
                    if(mode==MODE_PICK_BLOCK){
                        mode=MODE_BUILD;
                        
                    }
                    if(mode==MODE_PICK_COLOR){
                        mode=MODE_PAINT;
                        
                    }
                    if(mode==MODE_CAMERA){
                        mode=lmode;
                    }
                    blocktype_pressed=-1;
                }else{
                    
                    extern bool FLY_MODE;
                  /*  FLY_MODE=!FLY_MODE;
                    printg("Fly mode set to %d\n",FLY_MODE);*/
                    
                    
                    
                   /*
                    int ppx,ppz,ppy;
                    ppy=3*T_HEIGHT/4;
                    ppx=144+4096*CHUNK_SIZE-GSIZE/2;
                    ppz=144+4096*CHUNK_SIZE-GSIZE/2;
                    int dwarpx[]={0,      0,      0,        0,GSIZE/4,GSIZE/2,3*GSIZE/4};
                    int dwarpz[]={0,GSIZE/4,GSIZE/2,3*GSIZE/4,0,      0,      0,};
                    ppx+=dwarpx[warpCount];
                    ppz+=dwarpz[warpCount];
                    
                    [World::getWorld->terrain warpToPoint:ppx:ppz:ppy];
                    printg("warping to: %d,%d,%d\n",ppx,ppz,ppy);
                    warpCount++;
                    if(warpCount==7){
                        warpCount=0;
                    }
                    */
                   /* int ppx=player.pos.x-4096*CHUNK_SIZE+GSIZE/2;
                    int ppz=player.pos.z-4096*CHUNK_SIZE+GSIZE/2;
                    ppx=ppx/(GSIZE/4);
                    ppz=ppz/(GSIZE/4);
                    if(ppx>4)ppx=4;
                    if(ppz>4)ppz=4;
                    if(ppx<0)ppx=0;
                    if(ppz<0)ppz=0;*/
                    
                }
              //  printg("menu touched\n");
                handled=true;
            }
                       if(handled){
                           Input::getInput()->clearAll();
                          
                touches[i].down=M_NONE;
            }
             touches[i].down=M_NONE;
            touches[i].inuse=0;
        }
            
        
        if(touches[i].inuse==0&&touches[i].down==M_DOWN){
            if(!inmenu){   // UX.8-5: the hotbar strip claims the touch (no placing, no camera turn)
                const int hc=hotbarCellAt((float)touches[i].mx,(float)touches[i].my);
                if(hc!=-1){
                    HbLayout L;
                    touches[i].inuse=usage_hb;
                    touches[i].down=M_NONE;
                    hbCell[i]=hc;hbHeld[i]=0;hbDone[i]=false;hbMoved[i]=false;
                    hbBase[i]=hotbarLayout(&L)?L.start:0;
                    continue;
                }
            }
            if(inmenu){
                if(inbox3(touches[i].mx,touches[i].my,&rresume)||
                   inbox3(touches[i].mx,touches[i].my,&rsettings)||
                   inbox3(touches[i].mx,touches[i].my,&rconsole)||
                   inbox3(touches[i].mx,touches[i].my,&rcam)||
                   inbox3(touches[i].mx,touches[i].my,&rhome)||
                   inbox3(touches[i].mx,touches[i].my,&rsave)||
                   inbox3(touches[i].mx,touches[i].my,&rexit)||
                   inbox3(touches[i].mx,touches[i].my,&rtCam)||
                   inbox3(touches[i].mx,touches[i].my,&rtHome)||
                   inbox3(touches[i].mx,touches[i].my,&rtSave)||
                   inbox3(touches[i].mx,touches[i].my,&rtExit))
                {
                    printg("something touched in menu\n");
                    inbox3(touches[i].mx,touches[i].my,&rresume);
                    inbox3(touches[i].mx,touches[i].my,&rsettings);
                    inbox3(touches[i].mx,touches[i].my,&rconsole);
                    inbox3(touches[i].mx,touches[i].my,&rcam);
                    inbox3(touches[i].mx,touches[i].my,&rhome);
                    inbox3(touches[i].mx,touches[i].my,&rsave);
                    inbox3(touches[i].mx,touches[i].my,&rexit);
                    inbox3(touches[i].mx,touches[i].my,&rtCam);
                    inbox3(touches[i].mx,touches[i].my,&rtHome);
                    inbox3(touches[i].mx,touches[i].my,&rtSave);
                    inbox3(touches[i].mx,touches[i].my,&rtExit);
                    touches[i].inuse=usage_id2;
                    touches[i].down=M_NONE;
                    Resources::getResources->playSound(S_MENU_BUTTON_PRESS);
                }
            }
            if(inbox3(touches[i].mx,touches[i].my,&rmenu)){
                touches[i].inuse=usage_id2;
                touches[i].down=M_NONE;
                Resources::getResources->playSound(S_MENU_BUTTON_PRESS);
            }
            if(FLY_ALLOWED&&!inmenu&&inbox3(touches[i].mx,touches[i].my,&rflyhit)){   // FM.3: claims the touch
                touches[i].inuse=usage_id2;
                touches[i].down=M_NONE;
                Resources::getResources->playSound(S_MENU_BUTTON_PRESS);
            }
            if(consoleBtnShown()&&!inmenu&&inbox3(touches[i].mx,touches[i].my,&rcons)){   // claims the touch
                touches[i].inuse=usage_id2;
                touches[i].down=M_NONE;
                Resources::getResources->playSound(S_MENU_BUTTON_PRESS);
            }
            if(MpWorld::active()&&!inmenu&&inbox3(touches[i].mx,touches[i].my,&rchat)){   // MP.7: claims the touch
                touches[i].inuse=usage_id2;
                touches[i].down=M_NONE;
                Resources::getResources->playSound(S_MENU_BUTTON_PRESS);
            }
        /*    if(inbox3(touches[i].mx,touches[i].my,&rburn)){
                 touches[i].inuse=usage_id2;
            }
            if(inbox3(touches[i].mx,touches[i].my,&rbuild)){
                 touches[i].inuse=usage_id2;
			}
            
            if(inbox3(touches[i].mx,touches[i].my,&rpaint)){
				 touches[i].inuse=usage_id2;
			}
			if(inbox3(touches[i].mx,touches[i].my,&rmine)){	
                touches[i].inuse=usage_id2;
			}*/
        
			bool handled=false;
            if(inbox2(touches[i].mx,touches[i].my,&rburn)){	
                flamecount++;
                if(flamecount==25){
                    flamecount=0;
                    
                }
                printg("flamecount: %d\n",flamecount);
                if(mode==MODE_BURN)mode=MODE_NONE;
                else
                    mode=MODE_BURN;
                
                inmenu=false;
                    handled=true;
			}
            if(inbox2(touches[i].mx,touches[i].my,&rbuild)){
                if(mode==MODE_PAINT){
                    block_paintcolor=paintColor;
                    mode=MODE_BUILD;
                    noteHand();   // UX.8-3: the painted block is a hotbar item
                }else
                    if(mode!=MODE_PICK_BLOCK){
                        mode=MODE_PICK_BLOCK;
                        pickerPage=picker_page_of(blocktype);   // D.2p: open on the current block's page
                        pickSecondBlock=false;
                        sb->clear();
                        inmenu=false;
                    }else 
                        mode=MODE_BUILD;
                
                blocktype_pressed=-1;
                Input::getInput()->clearAll();
                
                break;
                inmenu=false;
				handled=true;
			}
            
            if(inbox2(touches[i].mx,touches[i].my,&rpaint)){
				if(mode!=MODE_PICK_COLOR){
					mode=MODE_PICK_COLOR;
                    inmenu=false;
                }else 
                    mode=MODE_PAINT;
                blocktype_pressed=-1;
                input->clearAll();
                
                break;
                inmenu=false;
				handled=true;
			}
			if(inbox2(touches[i].mx,touches[i].my,&rmine)){	
                if(mode==MODE_MINE)mode=MODE_NONE;
                else
                    mode=MODE_MINE;
                inmenu=false;
                handled=true;
			}
            
			if(mode==MODE_PICK_BLOCK){
				if(handlePickBlock(touches[i].mx,touches[i].my)){
                    if(!pickSecondBlock){
                    inmenu=false;
                     sb->clear();
                    }
					handled=true;
                    //printg("handled\n");
				}				
			}
            if(mode==MODE_PICK_COLOR){
                
				if(handlePickColor(touches[i].mx,touches[i].my)){
                    inmenu=false;
					handled=true;
				}				
			}
			
			
            
			if(handled){
				touches[i].inuse=usage_id;
                touches[i].moved=false;
				touches[i].down=M_NONE;	
				break;
			}						
		}
	}
	for(int i=0;i<MAX_TOUCHES;i++){
		if(touches[i].inuse==usage_id&&touches[i].moved){
            if(mode==MODE_PICK_BLOCK){
				//if(handlePickBlock(touches[i].mx,touches[i].my)){
                    
                    
                //        touches[i].inuse=usage_id;
                //    touches[i].moved=FALSE;
                    
					
				//}
			}else if(mode==MODE_PICK_COLOR){
				if(handlePickColor(touches[i].mx,touches[i].my)){
                    touches[i].inuse=usage_id;
                    touches[i].moved=false;
									}				
			}
        }
    }
    
	/*
     */
    // m_jump is re-derived from touches every frame (level, not edge -- it stays TRUE every frame
    // the button is held), so the press/release sounds below need their own prev-frame latch.
    // touchControlsSoundEnabled (Settings) gates both this and the joystick begin/release sounds,
    // default off; jumpIsRealTouch additionally makes sure the sounds only fire for an actual
    // touchscreen tap on the button, never a keyboard-synthesized press (EdenTouch::isReal is false for
    // Input.cpp's synthetic kJumpTouchIdentity touch that drives Space, same for mouse/HUD-tap
    // identities) -- so e.g. Space-to-jump on desktop stays silent even with the setting on.
    bool wasJumpPressed=m_jump||m_flyup;
    static bool jumpIsRealTouch=false;
	m_jump=false;
    m_flyup=false;
    bool jumpTouchReal=false;
	for(int i=0;i<MAX_TOUCHES;i++){
		if((touches[i].inuse==0||touches[i].inuse==usage_id)&&touches[i].down==M_DOWN){
			if(inbox2(touches[i].mx,touches[i].my,&rjumphit)){
				if(FLY_MODE)m_flyup=true;   // FM.3: while flying the jump slot is Fly up
				else m_jump=true;
                inmenu=false;
				//test_a+=.0001;
				//if(test_a>.005)test_a=0;
				printg(@"attenuation: %f",test_a);
				touches[i].inuse=usage_id;
				printg(@"set touch %d to uid %d",touches[i].touch_id,usage_id);
                if(touches[i].touch_id&&touches[i].touch_id->isReal)jumpTouchReal=true;
			}

		}

	}
    extern bool touchControlsSoundEnabled;
    const bool jumpPressedNow=m_jump||m_flyup;
    if(jumpPressedNow&&!wasJumpPressed){
        jumpIsRealTouch=jumpTouchReal;
        if(jumpIsRealTouch&&touchControlsSoundEnabled)
            Resources::getResources->playSound(S_JUMP_BUTTON_PRESS);
    }else if(!jumpPressedNow&&wasJumpPressed){
        if(jumpIsRealTouch&&touchControlsSoundEnabled)
            Resources::getResources->playSound(S_JUMP_BUTTON_RELEASE);
    }

	m_crouch=false;
	m_flydown=false;
	extern bool CROUCH_ENABLED;
	if(CROUCH_ENABLED||FLY_MODE){   // FM.3: the slot is Fly down while flying, even with Crouch off
		for(int i=0;i<MAX_TOUCHES;i++){
			if((touches[i].inuse==0||touches[i].inuse==usage_id)&&touches[i].down==M_DOWN){
				if(inbox2(touches[i].mx,touches[i].my,&rcrouchhit)){
					if(FLY_MODE)m_flydown=true;
					else m_crouch=true;
					inmenu=false;
					touches[i].inuse=usage_id;
				}
			}
		}
	}

	if(use_joystick&&mode!=MODE_PICK_BLOCK&&mode!=MODE_PICK_COLOR){   // UX.2 (B11): the picker window covers the pad; it must not take (or draw under) the touch
		joystick->update((float)etime);
	}else{
		//[gamepad update:(float) etime];
	}
	sb->update(etime);
	for(int i=0;i<MAX_TOUCHES;i++){
		
		if(touches[i].inuse==usage_id&&touches[i].down==M_RELEASE){
			if(blocktype_pressed!=-1){
                const int pickedIn=mode;
                if(mode==MODE_PICK_BLOCK&&pickSecondBlock==false){
                    
                    
                    if(blocktype_pressed==TYPE_GOLDEN_CUBE){
                        if(goldencubes>0){
                           
                            block_paintcolor=20;
                            mode=MODE_BUILD;
                            blocktype=blocktype_pressed;
                            printg("set paintcolor %d \n",block_paintcolor);
                          
                        }
                    }else if(blocktype_pressed==TYPE_CUSTOM){
                        /*
                        build_size++;
                        if(build_size==2){
                            build_size=0;
                        }
                        printg("Setting buildsize: %d\n",build_size);
                         */
                    }else if(blocktype_pressed==TYPE_DOOR_TOP){
                      block_paintcolor=20;
                        mode=MODE_BUILD;
                        blocktype=blocktype_pressed;

                        
                    }else{
                        // The SIGN tool takes the last picked colour (paintColor is the index, 0 = none).
                        block_paintcolor=(blocktype_pressed==HUD_TOOL_SIGN)?paintColor:0;
                    
                        mode=MODE_BUILD;
                        blocktype=blocktype_pressed;
                        if(hud_picker_is_mod(blocktype)) s_modSel=modCellName(blocktype);   // K.2-5: by name
                        printf("blocktype:%d\n",blocktype);
                    }
                    
                }
                if(mode==MODE_PICK_COLOR){
                    mode=MODE_PAINT;
                    paintColor=blocktype_pressed+1;
                    
                    printg("paint color-1:%d\n",paintColor-1);
                }
                if(mode!=pickedIn) noteHand();   // UX.8-3: a committed pick (block or colour) reaches the hotbar
                blocktype_pressed=-1;
                
            }
            touches[i].inuse=0;
			touches[i].down=M_NONE;
		}
		
		
	}
	
	
	return false;
}

//extern float P_ZFAR;

extern uint8_t blockColor[BLOCK_IDS][3];
    
bool Hud::handlePickColor(int x,int y){
    bool handled=false;
    
    
   
    for(int i=0;i<NUM_COLORS;i++){
     if(inbox(x,y,colorBounds[i])){
         blocktype_pressed=i;
         
        // NSLog(@"picked color: %d",pressed);
   
         handled=true;
     }
     }
    return handled;
}
bool Hud::handlePickMenu(int x,int y){
    bool handled=false;
    if(inbox2(x,y,&rresume)){
        inmenu=false;
        return true;
    }
    if(inbox2(x,y,&rsettings)&&eden_gl_settings_available()){
        // The menu stays open underneath: Settings' Back returns here, as on web.
        World::getWorld->menu->settings->resetView();
        World::getWorld->menu->showsettings=true;
        Input::getInput()->clearAll();
        return true;
    }
    if(inbox2(x,y,&rconsole)){
        // C.3: the Console takes over from the pause menu; closing it returns to play (the key opens
        // it from play too), not to this menu. The world keeps running behind it.
        inmenu=false;
        rconsole.pressed=false;
        eden_console_menu_open();
        Input::getInput()->clearAll();
        return true;
    }
    if(inbox2(x,y,&rcam)||inbox2(x,y,&rtCam)){
        
        rcam.pressed=rtCam.pressed=false;
        
		mode=MODE_CAMERA;
		if(!SUPPORTS_OGL2){
            Graphics::setZFAR(75);
           
        }
		else Graphics::setZFAR(120);
        inmenu=false;
		handled=true;
	}
	if(inbox2(x,y,&rhome)||inbox2(x,y,&rtHome)){	
        rhome.pressed=rtHome.pressed=false;
    /*    if(!World::getWorld->FLIPPED){
            [UIApplication sharedApplication].statusBarOrientation = UIInterfaceOrientationLandscapeRight;
        }
        else{
            [UIApplication sharedApplication].statusBarOrientation = UIInterfaceOrientationLandscapeLeft;
            
        }*/
        showAlertWarpHome();
        
		inmenu=false;
		handled=true;
	}
	if(inbox2(x,y,&rsave)||inbox2(x,y,&rtSave)){
        rsave.pressed=rtSave.pressed=false;
		const bool saved=World::getWorld->fm->saveWorld();
        if(saved)Resources::getResources->playSound(S_WORLD_SAVED);
        //[World::getWorld->terrain updateAllImportantChunks];
      //  NSLog(@"saving..");
        Input::getInput()->clearAll();
        
        World::getWorld->terrain->startDynamics();
		if(saved)sb->setStatus("World Saved" ,3);   // QA.5: else saveFailed's status line stays
        
        
        
        
        inmenu=false;
		handled=true;
	}
	if(inbox2(x,y,&rexit)||inbox2(x,y,&rtExit)){	
        rexit.pressed=rtExit.pressed=false;
		delayedtimer=1;
		delayedaction=5;
		sb->setStatus("Saving and quitting..." ,999);
        inmenu=false;
		handled=true;
        
	}
	

    
    return handled;
}

bool Hud::pickerPageStep(int delta){
    if(mode!=MODE_PICK_BLOCK||delta==0) return false;
    const int np=pickerPageCount();
    pickerPage=((pickerPage+delta)%np+np)%np;
    blocktype_pressed=-1;
    return true;
}

bool Hud::handlePickBlock(int x,int y){
	bool handled=false;
	// D.2p: the page arrows. A page turn keeps the picker (and a pending second-block pick) open.
	// The art is 22 points tall; the hit box meets the touch floor (design-system.md).
	if(inbox(x,y,picker_hit(pickerPrev))) return pickerPageStep(-1);
	if(inbox(x,y,picker_hit(pickerNext))) return pickerPageStep(1);
	int cells[NUM_DISPLAY_BLOCKS];
	page_cells(pickerPage,cells);
	for(int i=0;i<pickerCellCount(pickerPage);i++){
		if(inbox(x,y,blockBounds[i])){
            if(hud_picker_is_mod(cells[i])){
                // K.2-5: a mod block cell. Not a second-block candidate (no BT variant); consumed either way.
                if(!pickSecondBlock) blocktype_pressed=cells[i];
            }else
            if(hud_picker_is_tool(cells[i])){
                // A tool cell: consumed either way, picks only once its row has landed.
                if(pickerToolEnabled(cells[i])&&!pickSecondBlock) blocktype_pressed=cells[i];
            }else
            if(cells[i]==TYPE_BLOCK_TNT){
               // sb->setStatus(@"Pick second block type",999);
                pickSecondBlock=true;
                handled=true;
               // printf("no custom\n");
            }else{
                /*
                 TYPE_BTGRASS=82,
                 TYPE_BTDARKSTONE=83,
                 TYPE_BTSTONE=84,
                 TYPE_BTDIRT=85,
                 TYPE_BTSAND=86,
                 TYPE_BTTNT=87,
                 TYPE_BTWOOD=88,
                 TYPE_BTSHINGLE=89,
                 TYPE_BTGLASS=90,
                 TYPE_BTGRADIENT=91,
                 TYPE_BTTREE=92,
                 TYPE_BTLEAVES=93,
                 TYPE_BTBRICK=94,
                 TYPE_BTCOBBLESTONE=95,
                 TYPE_BTVINES=96,
                 TYPE_BTLADDER=97,
                 TYPE_BTICE=98,
                 TYPE_BTCRYSTAL=99,
                 TYPE_BTTRAMPOLINE=100,
                 TYPE_BTCLOUD=101,
                 TYPE_BTSTONESIDE=102,
                 TYPE_BTWOODSIDE=103,
                 TYPE_BTICESIDE=104,
                 TYPE_BTSHINGLESIDE=105,
                 TYPE_BTFENCESIDE=106,
                 TYPE_BTWATERSIDE=107,
                 TYPE_BTLAVASIDE=108,
                 TYPE_BTFIREWORKSIDE=109,
                 TYPE_BTLIGHTBOX=110,
                 TYPE_BTSTEEL=111,*/
                if(pickSecondBlock){
                    int nr=picker_bt(cells[i]);
                    
                    
                    if(nr==-1){
                        blocktype_pressed=cells[i];
                        printf("derp: %d\n",nr);
                    }else{ blocktype_pressed=nr;
                        printf("derp: %d\n",nr);
                    }
                    
                    pickSecondBlock=false;
                        
                    
                    
                }else{
                    blocktype_pressed=cells[i];
                    printf("derp: %d\n",blocktype_pressed);
                }
                // printg("set pressed: %d\n",i);
            }
           
			 handled=true;
            break;
		}
	}
		return handled;
    
}

// ---------------------------------------------------------------------------------------------
// Stage 5.7 — the block and colour pickers on the GL widget kit.
// ---------------------------------------------------------------------------------------------
// Same trick as the pause menu (5.5): the HIT-TEST PATH IS UNTOUCHED. blockBounds[] / colorBounds[]
// are laid out by layoutForScreen() exactly as before (headless-display-profile-test pins them at
// the classic profile), handlePickBlock / handlePickColor and Hud::update's release logic are
// unchanged. Only the drawing moves:
//   - the 2010 card texture (ICO_COLOR_SELECT_BACKGROUND over rpaintframe) -> a WINDOW bevel that
//     shrink-wraps the grid (rpaintframe is still computed; nothing draws it any more);
//   - each cell's border art (ICO_BLOCK_BORDER*, ICO_TRIANGLE_BORDER*, ICO_COLOR_BLOCK_BORDER*,
//     and the TNT "pick a second block" glow ICO_*_BORDER_ACTIVE) -> a RAISED cell, PRESSED while
//     held AND for the current choice (design-system.md: pressed == selected — stock only showed
//     the press), a lime ring for a block the second-block pick accepts, a grey veil for one it
//     does not;
//   - the swatches and block faces are the atlas art and stay the atlas art, drawn inside the cell.
// The kit snaps (design-system.md, "Motion"): drawn while the picker is open, not during the
// at2/at3 fade-out, which would leave full-opacity chrome over the game for a few frames.
static float picker_scale(){ return IS_IPAD ? SCALE_WIDTH : 1.0f; }

// The window around a grid: the union of its cells, padded.
static CGRect picker_panel(const CGRect* cells,int n){
    float x0=1e9f,y0=1e9f,x1=-1e9f,y1=-1e9f;
    for(int i=0;i<n;i++){
        x0=std::min(x0,(float)cells[i].origin.x); y0=std::min(y0,(float)cells[i].origin.y);
        x1=std::max(x1,(float)(cells[i].origin.x+cells[i].size.width));
        y1=std::max(y1,(float)(cells[i].origin.y+cells[i].size.height));
    }
    const float pad=GLW::du(10);
    return CGRectMake(x0-pad,y0-pad,(x1-x0)+pad*2.0f,(y1-y0)+pad*2.0f);
}

// The art's box inside a cell: clear of the 2u keyline, nudged down-right while the cell is
// pressed so the face moves with the bevel.
static CGRect picker_inner(CGRect cell,bool pressed){
    const float in=GLW::du(2)+2.0f, o=pressed?GLW::u():0.0f;
    return CGRectMake(cell.origin.x+in+o,cell.origin.y+in-o,cell.size.width-in*2.0f,cell.size.height-in*2.0f);
}

// One atlas face (tex point tp, the stock coordinate convention) as a quad — or, for a ramp, the
// stock's lower-left triangle — into a point-space box.
static void picker_face(CGRect r,CGPoint tp,bool triangle){
    const float s=picker_scale();
    const float x0=r.origin.x*s, y0=r.origin.y*s, x1=(r.origin.x+r.size.width)*s, y1=(r.origin.y+r.size.height)*s;
    float coordinates[] = { 0, tp.y+tp.x,  1, tp.y+tp.x,  0, tp.x,  1, tp.x };
    float vertices[] = { x0,y0,0,  x1,y0,0,  x0,y1,0,  x1,y1,0 };
    Pen::strip(vertices, coordinates, 0, triangle?3:4);
}

// K.2-5: a mod block's icon straight from its definition (it has no table row until placed): the +Y
// face, in the atlas the definition names, tinted like a stock cell unless its art carries colour.
static void mod_icon(const std::string& name,CGRect in){
    const BlockRegistry::BlockDef* d=BlockRegistry::findDef(name);
    if(!d) return;
    Resources* res=Resources::getResources;
    const int f=d->faces[5];
    Pen::texture((d->info&IS_ATLAS2)?res->atlas2->name:res->atlas->name);
    const CGPoint tp=IS_NEWBLOCK_TEX(f)?res->getBlockTex(NEWBLOCK_COLOR_TEX(f)):res->getBlockTex(f);
    if(d->paintArt&ART_PICKER) Pen::color(1.0f,1.0f,1.0f,1.0f);
    else Pen::colorub(d->color[0],d->color[1],d->color[2],255);
    picker_face(in,tp,false);
}
// Is picker cell `type` the block a build would place? (A mod cell: by name; the selection is either
// still the sentinel or already resolved to this world's id.)
static bool picker_cell_on(int type,int blocktype,bool pickSecondBlock,int blocktype_pressed){
    if(blocktype_pressed==type) return true;
    if(pickSecondBlock) return false;
    if(hud_picker_is_mod(type)){
        const std::string n=Hud::modCellName(type);
        if(n.empty()||n!=s_modSel) return false;
        return blocktype==type||(blocktype>STOCK_BLOCK_MAX&&!hud_picker_is_tool(blocktype)&&BlockRegistry::idOf(n)==blocktype);
    }
    return type==blocktype;
}

void Hud::renderColorPickScreen(){
    if(mode!=MODE_PICK_COLOR) return;
    using namespace GLW;
    Pen::blend(true);
    Pen::blendFunc(R::B_ALPHA);

    bevel(picker_panel(colorBounds,NUM_COLORS),BEVEL_WINDOW);
    const int current=(int)paintColor-1;           // paintColor is the colour index + 1, 0 = none
    for(int i=0;i<NUM_COLORS;i++)
        bevel(colorBounds[i],(blocktype_pressed==i||current==i)?BEVEL_PRESSED:BEVEL_RAISED);

    Pen::textured(true);
    Pen::texture(Resources::getResources->atlas->name);
    const CGPoint tp=Resources::getResources->getBlockTex(blockTypeFaces[TYPE_CLOUD][5]);
    for(int i=0;i<NUM_COLORS;i++){
        const Vector hudColor=colorTable[i+1];
        Pen::color(hudColor.x,hudColor.y,hudColor.z,1.0f);
        picker_face(picker_inner(colorBounds[i],blocktype_pressed==i||current==i),tp,false);
    }
    Pen::color(1.0f,1.0f,1.0f,1.0f);
    Pen::blend(true);
}
extern bool DOUBLE_BRUSH;   // Player.mm, H.7
void Hud::renderBlockAndBorder(CGRect recto){
    int type=blocktype;
    if(hud_picker_is_mod(type)){
        // K.2-5: a mod block not yet placed: the build background, then its icon from the definition.
        Pen::color(1.0f,1.0f,1.0f,1.0f);
        Resources::getResources->getTex((mode==MODE_BUILD||mode==MODE_PICK_BLOCK)?ICO_BUILD2_ACTIVE:ICO_BUILD2)->drawTextHalfsies(recto);
        const float sz=fminf(recto.size.width,recto.size.height)*0.6f;
        Pen::textured(true);
        mod_icon(modCellName(type),CGRectMake(recto.origin.x+(recto.size.width-sz)*0.5f,recto.origin.y+(recto.size.height-sz)*0.5f,sz,sz));
        Pen::color(1.0f,1.0f,1.0f,1.0f);
        return;
    }
    if(hud_picker_is_tool(type)){
        // D.3c: a tool is never a table index -- the build background, then its picture (a caption
        // until the Maysunbo art, 2026-10-27).
        Pen::color(1.0f,1.0f,1.0f,1.0f);
        Resources::getResources->getTex((mode==MODE_BUILD||mode==MODE_PICK_BLOCK)?ICO_BUILD2_ACTIVE:ICO_BUILD2)->drawTextHalfsies(recto);
        const float sz=fminf(recto.size.width,recto.size.height)*0.6f;
        Resources::getResources->getArtTex(type==HUD_TOOL_SIGN?ART_SIGN_ICON:ART_CMD_BLOCK)->drawInRect(
            CGRectMake(recto.origin.x+(recto.size.width-sz)*0.5f,recto.origin.y+(recto.size.height-sz)*0.5f,sz,sz));
        return;
    }
    
    
	Pen::color(1.0f,1.0f,1.0f,1.0f);
    if(BlockIcon::isFlat(type)){
        float offsetx;
        float offsety;
        float osize=90;
        if(type!=TYPE_FLOWER)osize=70;
        if(IS_IPAD&&!SUPPORTS_RETINA){
            offsetx=(128-osize)/2.0f/SCALE_WIDTH;
            offsety=(128-osize)/2.0f/SCALE_HEIGHT;
        }else{
            offsetx=(128-osize)/2.0f/2.0f;
            offsety=(128-osize)/2.0f/2.0f;
        }
        
        if(mode==MODE_BUILD||mode==MODE_PICK_BLOCK){
            /*
             ICOICO_GOLDCUBE_ACTIVE=94,
             ICOICO_FLOWER_ACTIVE=95,
             ICOICO_DOOR_ACTIVE=96,
             ICOICO_PORTAL_ACTIVE=97,
             */
            int tid=ICOICO_FLOWER_ACTIVE;   // QA.23: a flat type outside the four (palette block) gets the flower icon
            if(type==TYPE_FLOWER){
                tid=ICOICO_FLOWER_ACTIVE;
            }else if(type==TYPE_GOLDEN_CUBE){
                tid=ICOICO_GOLDCUBE_ACTIVE;
            }else if(type==TYPE_PORTAL_TOP){
                tid=ICOICO_PORTAL_ACTIVE;
            }else if(type==TYPE_DOOR_TOP){
                tid=ICOICO_DOOR_ACTIVE;
            }
            CGRect offrect=recto;
            offrect.origin.x-=offsetx;
            offrect.origin.y-=offsety;
            Resources::getResources->getTex(tid)->drawTextHalfsies(offrect);
            BlockIcon::flat(type,block_paintcolor,recto);
          
        }else{
            
            int tid=ICO_FLOWER_ICO;   // QA.23: a flat type outside the four (palette block) gets the flower icon
            if(type==TYPE_FLOWER){
                tid=ICO_FLOWER_ICO;
            }else if(type==TYPE_GOLDEN_CUBE){
                tid=ICO_GOLDCUBE;
            }else if(type==TYPE_PORTAL_TOP){
                tid=ICO_PORTAL2;
            }else if(type==TYPE_DOOR_TOP){
                tid=ICO_DOOR2;
            }

            BlockIcon::flat(type,block_paintcolor,recto);
            
            
        }
        Resources::getResources->getTex(ICO_BUILD_PLUS)->drawTextHalfsies(recto);
        if(type==TYPE_GOLDEN_CUBE){
           // printg("wtf\n");
            CGRect num_rect=recto;
            //recto.origin.x/=2;
            //recto.origin.y/=2;
            num_rect.origin.x+=18;
            if(goldencubes!=10){
                num_rect.origin.x+=3;
            }
            num_rect.origin.y-=2;
            if(goldencubes==0){
                Pen::color(1.0f,1.0f,1.0f,.3f);
                Resources::getResources->getTex(TEXT_NUMBERS)->drawNumbers(num_rect,goldencubes);
                Pen::color(1.0f,1.0f,1.0f,1.0f);
            }else{
                Resources::getResources->getTex(TEXT_NUMBERS)->drawNumbers(num_rect,goldencubes);
                
            }
        }
        return;
    }else
    if(type>=TYPE_STONE_RAMP1&&type<=TYPE_ICE_RAMP4){
        float offsetx;
        float offsety;
        if(IS_IPAD&&!SUPPORTS_RETINA){
            offsetx=(128-90)/2.0f/SCALE_WIDTH;
            offsety=(128-90)/2.0f/SCALE_HEIGHT;
            
            
        }else{
            offsetx=(128-90)/2.0f/2.0f;
            offsety=(128-90)/2.0f/2.0f;
            
        }
       // CGRect rdigits=CGRectMake(50,50,50,50);
       //  Resources::getResources->getTex:ICO_DIGITS]->drawText(rdigits);
        if(mode==MODE_BUILD||mode==MODE_PICK_BLOCK){
            
            recto.origin.x-=offsetx;
           recto.origin.y-=offsety;
            if(build_size!=0)
                Resources::getResources->getTex(ICO_BUILD3_ACTIVE)->drawTextHalfsies(recto);
            else {
                Resources::getResources->getTex(ICO_BUILD3_ACTIVE2)->drawTextHalfsies(recto);
            }
            
            recto.origin.x+=offsetx;
            recto.origin.y+=offsety;
        }else{
            if(build_size==0)
                Resources::getResources->getTex(ICO_BUILD2_UNDER2)->drawTextHalfsies(recto);
            else {
                Resources::getResources->getTex(ICO_BUILD3)->drawTextHalfsies(recto);
            }
            
        }
    }else{
        float offsetx;
        float offsety;
        
        if(IS_IPAD&&!SUPPORTS_RETINA){
            offsetx=(128-90)/2.0f/SCALE_WIDTH;
            offsety=(128-90)/2.0f/SCALE_HEIGHT;
            
            
        }else{
            offsetx=(128-90)/2.0f/2.0f;
            offsety=(128-90)/2.0f/2.0f;
            
        }
        
        if(mode==MODE_BUILD||mode==MODE_PICK_BLOCK){
            
            recto.origin.x-=offsetx;
            recto.origin.y-=offsety;
           // Resources::getResources->getTex:ICO_BUILD2_ACTIVE]->drawTextHalfsies(recto);
            if(build_size==0){
            Resources::getResources->getTex(ICO_BUILD2_ACTIVE2)->drawTextHalfsies(recto);
            }else{
                Resources::getResources->getTex(ICO_BUILD2_ACTIVE)->drawTextHalfsies(recto);
            }
            
            recto.origin.x+=offsetx;
            recto.origin.y+=offsety;
        }else{
            if(build_size==0)
            Resources::getResources->getTex(ICO_BUILD_UNDER2)->drawTextHalfsies(recto);
            else {
                Resources::getResources->getTex(ICO_BUILD2)->drawTextHalfsies(recto);
            }
        }
    }
	//glDisable(GL_BLEND);
	
    
    // UX.8-1: the cube itself is BlockIcon's; these are the stock sizes that fit this button's frame art.
    BlockIcon::Geo geo;
    int size=29;
    geo.off=2;
    geo.offx=0;
    geo.offy=4;
    int depth=24;
    if(build_size==0){
        depth=17;
        if(type>=TYPE_STONE_RAMP1&&type<=TYPE_ICE_RAMP4)depth=19;
        size=24;
        geo.offx+=9;
        geo.offy+=9;
    }
    if(build_size==2)size=45;

    CGRect rect=recto;
    if(!IS_RETINA&&SUPPORTS_RETINA){
        depth/=2;
        geo.offy-=2;
    }
    if(IS_IPAD){
        rect.origin.x*=SCALE_WIDTH;
        rect.origin.y*=SCALE_HEIGHT;
        size*=2;
        geo.off*=2;
    }
    geo.x=rect.origin.x;
    geo.y=rect.origin.y;
    geo.size=size;
    geo.depth=depth;
    geo.cory=-3;

    if(type==TYPE_CLOUD&&holding_creature){
        BlockIcon::cube(type,block_paintcolor,geo,BlockIcon::FRONT_ONLY);
        Resources::getResources->getTex(ICO_MOOF)->drawTextNoScale(
         CGRectMake(rect.origin.x+geo.off-geo.offx, rect.origin.y+geo.off-geo.offy,32, 32));
    }else
        BlockIcon::cube(type,block_paintcolor,geo);

    Pen::color(1.0f,1.0f,1.0f,1.0f);
    if(type>=TYPE_STONE_RAMP1&&type<=TYPE_ICE_RAMP4){
             
        if(build_size!=0)
        Resources::getResources->getTex(ICO_BUILD3_TOP)->drawTextHalfsies(recto);
        else
         Resources::getResources->getTex(ICO_BUILD3_TOP2)->drawTextHalfsies(recto);
        Resources::getResources->getTex(ICO_BUILD_PLUS)->drawTextHalfsies(recto);
    }else{
        if(build_size!=0)
        Resources::getResources->getTex(ICO_BUILD2_TOP)->drawTextHalfsies(recto);
        else {
             Resources::getResources->getTex(ICO_BUILD_OVER2)->drawTextHalfsies(recto);
        }
        
         Resources::getResources->getTex(ICO_BUILD_PLUS)->drawTextHalfsies(recto);
    }
    //vertices[v_idx].texs[0]=cubeTexture[st]*size;		
    
    //vertices[v_idx].texs[1]=cubeTexture[st+1]*tp.y+tp.x;
    
   
	
}

void Hud::renderMenuScreen(){
    // The kit snaps, it does not fade (design-system.md, "Motion"): drawn while the menu is open
    // and not during at1's fade-out, which would leave a full-opacity panel for a few frames.
    if(!inmenu) return;
    using namespace GLW;
    layoutPauseMenu();   // cheap; keeps up with a display-profile switch mid-game
    HudPauseKit* k=pauseKit;
    if(!k->built){
        k->built=true;
        k->title.set("Paused", du(34), UITextAlignmentCenter);
        for(int j=0;j<PM_COUNT;j++) k->btn[j].setLabel(kPauseLabels[j], du(22));
        // C.3's UIICON_TERMINAL glyph gave way to the Maysunbo console_button art (2026-10-27).
    }
    ::Button* r[PM_COUNT]={&rresume,&rsave,&rhome,&rcam,&rsettings,&rconsole,&rexit};
    // An rt* rect may carry the press (handlePickMenu tests both), so either lights the button.
    const bool rtp[PM_COUNT]={false,rtSave.pressed,rtHome.pressed,rtCam.pressed,false,false,rtExit.pressed};

    // Same GL set-up as GLDialog::render, minus the projection: the HUD's ortho (Graphics::
    // beginHud) is already SCREEN*2, the space GLW draws in.
    Pen::blend(true);
    Pen::blendFunc(R::B_ALPHA);
    Pen::color(1.0f,1.0f,1.0f,1.0f);

    Focus::screen(k,&k->btn[0]);   // UX.7: Return = Resume
    fill(CGRectMake(0,0,SCREEN_WIDTH,SCREEN_HEIGHT), kScrim);
    bevel(k->panel, BEVEL_WINDOW);
    k->title.drawCentered(k->panel.origin.x+k->panel.size.width*0.5f,
                          k->titleTop-(du(40)-k->title.height())*0.5f, kText);

    Resources* res=Resources::getResources;
    Texture2D* icon[PM_COUNT]={NULL,res->getTex(ICO_SAVE),res->getTex(ICO_HOME),
                               res->getTex(ICO_SCREENSHOT),res->getTex(ICO_OPEN_MENU),
                               res->getArtTex(ART_CONSOLE),res->getTex(ICO_EXIT)};
    for(int j=0;j<PM_COUNT;j++){
        if(r[j]->size.width<=0) continue;
        k->btn[j].setRect(CGRectMake(r[j]->origin.x,r[j]->origin.y,r[j]->size.width,r[j]->size.height));
        k->btn[j].setPressed(r[j]->pressed||rtp[j]);
        k->btn[j].render();
        if(icon[j]&&r[j]->size.width>=du(236)){   // UX.2 (B6): no icon in the narrow two-column cells (it sat over the label)
            const float sz=r[j]->size.height-du(8);
            Pen::color(1.0f,1.0f,1.0f,1.0f);
            icon[j]->drawInRect(CGRectMake(r[j]->origin.x+du(6),
                                           r[j]->origin.y+(r[j]->size.height-sz)*0.5f,sz,sz));
        }
    }
    Pen::color(1.0f,1.0f,1.0f,1.0f);
}


// D.2p: the page strip's chrome. Rasterised once (and the arrows again only when the layout moves
// them), never per frame. (The tool cells' SIGN / CMD captions became pictures, 2026-10-27.)
struct HudPickerKit {
    GLW::Button prev,next;
    CGRect      prevAt,nextAt;
    bool        built;
    HudPickerKit() : built(false) { prevAt=nextAt=CGRectMake(0,0,0,0); }
};
static HudPickerKit* pickerKit=NULL;
void Hud::renderBlockScreen(){
    if(mode!=MODE_PICK_BLOCK) return;
    using namespace GLW;
    Resources* res=Resources::getResources;
    Pen::blend(true);
    Pen::blendFunc(R::B_ALPHA);

    if(!pickerKit) pickerKit=new HudPickerKit();
    HudPickerKit* k=pickerKit;
    if(!k->built){
        // UX.8-6: keyboard hint on the arrows (native `[` `]`); touch has no keys.
        const bool keys=eden_profile_touch_chrome()==0;
        k->prev.setLabel(keys?"[ <":"<",18.0f);
        k->next.setLabel(keys?"> ]":">",18.0f);
        k->built=true;
    }
    if(memcmp(&k->prevAt,&pickerPrev,sizeof(CGRect))){ k->prevAt=pickerPrev; k->prev.setRect(pickerPrev); }
    if(memcmp(&k->nextAt,&pickerNext,sizeof(CGRect))){ k->nextAt=pickerNext; k->next.setRect(pickerNext); }

    // The window is page 1's on both pages (the 2026 game's), grown down over the page strip.
    {
        CGRect cells[NUM_DISPLAY_BLOCKS+2];
        memcpy(cells,blockBounds,sizeof(blockBounds));
        cells[NUM_DISPLAY_BLOCKS]=pickerPrev;
        cells[NUM_DISPLAY_BLOCKS+1]=pickerNext;
        bevel(picker_panel(cells,NUM_DISPLAY_BLOCKS+2),BEVEL_WINDOW);
    }
    int hudBlocks[NUM_DISPLAY_BLOCKS];
    const int ncells=page_cells(pickerPage,hudBlocks);

    // Pass 1, chrome: the second-block ring, then the cell. TYPE_CUSTOM is a blank slot, as stock.
    // "Current" is the block a build would place; while a second block is being picked there is
    // no current one (the TNT cell is what started the pick).
    int golden_cubei=-1;
    for(int i=0;i<ncells;i++){
        const int type=hudBlocks[i];
        if(type==TYPE_GOLDEN_CUBE) golden_cubei=i;
        if(type==TYPE_CUSTOM) continue;
        const bool on=picker_cell_on(type,blocktype,pickSecondBlock,blocktype_pressed);
        if(pickSecondBlock&&picker_bt(type)!=-1){
            const float g=du(3);
            const CGRect c=blockBounds[i];
            fill(CGRectMake(c.origin.x-g,c.origin.y-g,c.size.width+g*2.0f,c.size.height+g*2.0f),rgb(0x89c31f));
        }
        bevel(blockBounds[i],on?BEVEL_PRESSED:BEVEL_RAISED);
    }

    // Pass 2, art: the atlas face (or the item's own icon) inside each cell.
    Pen::textured(true);
    for(int i=0;i<ncells;i++){
        const int type=hudBlocks[i];
        if(type==TYPE_CUSTOM) continue;
        const bool on=picker_cell_on(type,blocktype,pickSecondBlock,blocktype_pressed);
        const CGRect in=picker_inner(blockBounds[i],on);
        if(hud_picker_is_mod(type)){
            mod_icon(modCellName(type),in);
            continue;
        }
        if(hud_picker_is_tool(type)){
            // D.2p captioned these; since 2026-10-27 they are pictured (Maysunbo art).
            Pen::color(1.0f,1.0f,1.0f,1.0f);
            res->getArtTex(type==HUD_TOOL_SIGN?ART_SIGN_ICON:ART_CMD_BLOCK)->drawInRect(in);
            continue;
        }
        if(type==TYPE_FLOWER||type==TYPE_GOLDEN_CUBE||type==TYPE_PORTAL_TOP||type==TYPE_DOOR_TOP){
            int tid=ICO_FLOWER_ICO;
            if(type==TYPE_GOLDEN_CUBE)     tid=ICO_GOLDCUBE;
            else if(type==TYPE_PORTAL_TOP) tid=ICO_PORTAL2;
            else if(type==TYPE_DOOR_TOP)   tid=ICO_DOOR2;
            Pen::color(1.0f,1.0f,1.0f,(type==TYPE_GOLDEN_CUBE&&goldencubes==0)?0.3f:1.0f);
            res->getTex(tid)->drawInRect(in);
            continue;
        }
        Pen::texture((blockinfo[type]&IS_ATLAS2)?res->atlas2->name:res->atlas->name);
        CGPoint tp;
        if(type==TYPE_TNT)                 tp=res->getBlockTex(TEX_TNT_SIDE_COLOR);
        else if(type==TYPE_FIREWORK)       tp=res->getBlockTex(TEX_FIREWORK);
        else if(type==TYPE_BLOCK_TNT)      tp=res->getBlockTex(TEX_BLOCKTNT);
        else if(type==TYPE_LADDER)         tp=res->getBlockTex(blockTypeFaces[type][3]);
        else if(type==TYPE_BRICK)          tp=res->getBlockTex(TEX_BRICK_COLOR);
        else if(IS_NEWBLOCK_TEX(blockTypeFaces[type][5])) tp=res->getBlockTex(NEWBLOCK_COLOR_TEX(blockTypeFaces[type][5]));
        else                               tp=res->getBlockTex(blockTypeFaces[type][5]);
        // Faces whose art already carries its colour draw white; the rest take blockColor, as stock.
        if(trait_paintArtPicker(type))   // K.2-1
            Pen::color(1.0f,1.0f,1.0f,1.0f);
        else
            Pen::colorub(blockColor[type][0],blockColor[type][1],blockColor[type][2],255);
        picker_face(in,tp,type>=TYPE_STONE_RAMP1&&type<=TYPE_ICE_RAMP4);
        if(type==TYPE_CLOUD&&holding_creature){
            const float s=picker_scale();
            Pen::color(1.0f,1.0f,1.0f,1.0f);
            res->getTex(ICO_MOOF)->drawTextNoScale(CGRectMake(in.origin.x*s,in.origin.y*s,32,32));
        }
    }
    Pen::color(1.0f,1.0f,1.0f,1.0f);

    // Pass 3: blocks the second-block pick will not take are veiled rather than faded — the kit's
    // chrome stays solid (design-system.md, "Placeholders and disabled controls").
    // D.2p: a tool cell is veiled the same way until its row lands, and always during that pick.
    for(int i=0;i<ncells;i++){
        const int type=hudBlocks[i];
        if(type==TYPE_CUSTOM) continue;
        const bool veiled=hud_picker_is_mod(type)?pickSecondBlock
                          :hud_picker_is_tool(type)?(pickSecondBlock||!pickerToolEnabled(type))
                                                  :(pickSecondBlock&&picker_bt(type)==-1);
        if(veiled) fill(blockBounds[i],rgb(0xc9c9c9,0.6f));
    }

    // D.2p: the page strip — the arrows (kit buttons) and one dot per page, the current one filled.
    k->prev.render();
    k->next.render();
    {
        const float d=du(8),gap=du(8);
        const float x0=(pickerPrev.origin.x+pickerPrev.size.width+pickerNext.origin.x)*0.5f
                       -(pickerPageCount()*d+(pickerPageCount()-1)*gap)*0.5f;
        const float y=pickerPrev.origin.y+(pickerPrev.size.height-d)*0.5f;
        for(int p=0;p<pickerPageCount();p++){
            const float x=x0+p*(d+gap);
            fill(CGRectMake(x,y,d,d),rgb(0x000000));
            if(p!=pickerPage) fill(CGRectMake(x+u(),y+u(),d-2.0f*u(),d-2.0f*u()),rgb(0xc9c9c9));
        }
    }
    Pen::color(1.0f,1.0f,1.0f,1.0f);

    // The golden-cube count, where stock put it on that cell.
    if(golden_cubei>=0){
        CGRect num_rect=blockBounds[golden_cubei];
        num_rect.origin.x+=18;
        if(goldencubes!=10){
            num_rect.origin.x+=3;
        }
        num_rect.origin.y-=2;
        Pen::color(1.0f,1.0f,1.0f,goldencubes==0?.3f:1.0f);
        Resources::getResources->getTex(TEXT_NUMBERS)->drawNumbers(num_rect,goldencubes);
        Pen::color(1.0f,1.0f,1.0f,1.0f);
    }
	/*
    glColor4f(1.0, 1.0, 1.0, 1.0f);
    glEnable(GL_BLEND);
    for(int i=0;i<NUM_DISPLAY_BLOCKS;i++){
        int type=hudBlocks[i];
        if(build_size==0){blockBounds[i].size.width-=10;
            blockBounds[i].size.height-=10;
        }
               if(build_size==2){blockBounds[i].size.width+=10;
            blockBounds[i].size.height+=10;
        }
        if(type>=TYPE_STONE_RAMP1&&type<=TYPE_ICE_RAMP4){
            if(pressed==i)
                Resources::getResources->getTex:ICO_TRIANGLE_BORDER_PRESSED] drawInRect2:blockBounds[i]];
            else
                Resources::getResources->getTex:ICO_TRIANGLE_BORDER] drawInRect2:blockBounds[i]];
        }else{
            if(pressed==i)
                Resources::getResources->getTex:ICO_BLOCK_BORDER_PRESSED] drawInRect2:blockBounds[i]];
            else
                Resources::getResources->getTex:ICO_BLOCK_BORDER]->drawText(blockBounds[i]];
        }
                if(build_size==0){blockBounds[i].size.width+=10;
            blockBounds[i].size.height+=10;
        }
        if(build_size==2){blockBounds[i].size.width-=10;
            blockBounds[i].size.height-=10;
        }
        
    }*/

	/*
     Resources* res=Resources getResources];
     Texture2D* tsave=[res getTex:ICO_SAVE];	
     Texture2D* thome=[res getTex:ICO_HOME];	
     Texture2D* texit=[res getTex:ICO_EXIT];	
     Texture2D* tcam=[res getTex:ICO_SCREENSHOT];
     [tsave drawInRect2: rsave];		
	[thome drawInRect2: rhome];		
	[texit drawInRect2: rexit];	
	[tcam drawInRect2: rcam];*/
	
	
	return;
}
//extern float P_ZFAR;


// ---- UX.8-4: the hotbar strip (docs/ui.md "Hotbar"). Drawn in the HUD's point space; the icons are
// BlockIcon's, which draw in raw pass units (point x `sc`). No hit rects yet (UX.8-5).
#include "eden_settings.h"   // the keybind accessors (QA.19)

static void hb_box(CGRect r,float t,GLW::Color c){   // a t-thick border inside r
    GLW::fill(r.origin.x,r.origin.y,r.size.width,t,c);
    GLW::fill(r.origin.x,r.origin.y+r.size.height-t,r.size.width,t,c);
    GLW::fill(r.origin.x,r.origin.y,t,r.size.height,c);
    GLW::fill(r.origin.x+r.size.width-t,r.origin.y,t,r.size.height,c);
}

// UX.8-5: where the strip is this frame. Shared by renderHotbar and every hit test, so a cell is
// exactly where it is drawn. `start` is the first visible slot (the window follows the selection until a
// swipe or chevron slides it; the slide is dropped when the selection changes).

bool Hud::hotbarLayout(HbLayout* L){
    using namespace GLW;
    if(!eden_get_hotbar_shown()||inmenu||at1>0||mode==MODE_NONE||hideui) return false;
    const bool touch=eden_profile_touch_chrome()!=0;
    const CGRect safe=GLW::safeRect();
    const float s0x=safe.origin.x,s0y=safe.origin.y;
    const float sT=s0y+safe.size.height;
    const float gap=2*GLW::u();
    const int NS=Hotbar::SLOTS;
    int n=NS;
    float cell,y,left;
    if(!touch){   // desktop: bottom centre
        cell=du(40);
        y=s0y+du(8);
        left=s0x+(safe.size.width-(NS*cell+(NS-1)*gap))*0.5f;
        left=floorf(left);
    }else{        // touch: top centre, between the corner buttons and the mode column; 9, else 7, else 5 cells
        const float tagEnd=s0x+((consoleBtnShown()||MpWorld::active())?175.0f:121.0f);
        const float lo=tagEnd+8,hi=HUDR_X-8;
        const int tiers[3]={9,7,5};
        cell=0;
        for(int t=0;t<3;t++){
            const float c=floorf(fminf(44.0f,(hi-lo-gap*(tiers[t]-1))/tiers[t]));
            n=tiers[t];
            cell=c;
            if(c>=36.0f) break;
        }
        if(cell<20.0f) return false;   // below the guard tier nothing sensible fits
        y=sT-cell;
        left=floorf(lo+((hi-lo)-(n*cell+(n-1)*gap))*0.5f);
    }
    const float stripW=n*cell+(n-1)*gap;
    const CGRect strip=CGRectMake(left,y,stripW,cell+du(2));
    // A picker hides the strip when its panel is in the way (always, on the touch top strip).
    if(mode==MODE_PICK_BLOCK||mode==MODE_PICK_COLOR||at2>0||at3>0){
        const CGRect& p=rpaintframe;
        if(touch||(strip.origin.x<p.origin.x+p.size.width&&strip.origin.x+strip.size.width>p.origin.x&&
                   strip.origin.y<p.origin.y+p.size.height&&strip.origin.y+strip.size.height>p.origin.y)) return false;
    }
    const int sel=Hotbar::get().selected();
    int start=0;
    if(n<NS){
        if(s_hbSlide>=0&&s_hbSlideSel==sel) start=s_hbSlide;
        else{
            start=(sel>=0?sel:0)-n/2;
            if(start<0)start=0;
            if(start>NS-n)start=NS-n;
        }
    }
    L->touch=touch;L->n=n;L->start=start;L->cell=cell;L->gap=gap;L->left=left;L->y=y;L->stripW=stripW;L->strip=strip;
    return true;
}

// Hit rects, in the HUD's point space (y up): the nine cells abut (each takes half a gap either side) so no
// two overlap, and on touch each is at least touchFloor() tall, hanging down from the top edge. The chevrons
// sit just outside the strip, between its ends and the neighbouring HUD buttons.
Button Hud::hotbarButton(int idx){
    Button b;
    b.origin=CGPointMake(0,0);b.size=CGSizeMake(0,0);b.pressed=false;
    HbLayout L;
    if(idx<0||idx>10||!hotbarLayout(&L)) return b;
    const float pitch=L.cell+L.gap;
    float top,h;
    if(L.touch){
        h=fmaxf(L.cell,GLW::touchFloor());
        top=L.y+L.cell;
    }else{
        h=L.cell+2*GLW::u();
        top=L.y+h;
    }
    const float y0=top-h;
    if(idx<9){
        const int k=idx-L.start;
        if(k<0||k>=L.n) return b;
        const float x0=L.left+k*pitch-(k==0?0:L.gap*0.5f);
        const float x1=L.left+k*pitch+L.cell+(k==L.n-1?0:L.gap*0.5f);
        b.origin=CGPointMake(x0,y0);b.size=CGSizeMake(x1-x0,h);
        return b;
    }
    if(L.n>=Hotbar::SLOTS) return b;
    const float w=GLW::du(14);
    if(idx==9&&L.start>0){b.origin=CGPointMake(L.left-L.gap-w,y0);b.size=CGSizeMake(w,h);}
    if(idx==10&&L.start+L.n<Hotbar::SLOTS){b.origin=CGPointMake(L.left+L.stripW+L.gap,y0);b.size=CGSizeMake(w,h);}
    return b;
}

static bool hb_in(float x,float y,const Button& b){
    return b.size.width>0&&x>=b.origin.x&&x<b.origin.x+b.size.width&&y>=b.origin.y&&y<b.origin.y+b.size.height;
}

// The strip target under a point: a slot 0-8, -1 none, -2/-3 the left/right chevron.
int Hud::hotbarCellAt(float x,float y){
    if(hb_in(x,y,hotbarButton(9))) return -2;
    if(hb_in(x,y,hotbarButton(10))) return -3;
    for(int i=0;i<Hotbar::SLOTS;i++) if(hb_in(x,y,hotbarButton(i))) return i;
    return -1;
}

void Hud::hotbarTarget(int i){
    Hotbar::get().setPickerTarget(i);
    s_hbChoosing=true;
    char buf[40];
    std::snprintf(buf,sizeof(buf),"Choosing for slot %d",i+1);
    sb->setStatus(buf,999);
}

bool Hud::hotbarActivate(int i){
    if(i<0||i>=Hotbar::SLOTS) return false;
    Hotbar& hb=Hotbar::get();
    const HotbarSlot& s=hb.slot(i);
    if(mode==MODE_PICK_BLOCK||mode==MODE_PICK_COLOR){hotbarTarget(i);return true;}
    const bool inUse=(mode==MODE_BUILD||mode==MODE_PAINT);
    if(s.kind==HOTBAR_EMPTY||(i==hb.selected()&&inUse)){   // H7: open the picker for this slot
        if(s.kind==HOTBAR_COLOUR) mode=MODE_PICK_COLOR;
        else{
            mode=MODE_PICK_BLOCK;
            pickerPage=picker_page_of(blocktype);
            pickSecondBlock=false;
        }
        inmenu=false;
        blocktype_pressed=-1;
        Input::getInput()->clearAll();
        Resources::getResources->playSound(S_MENU_BUTTON_PRESS);
        hotbarTarget(i);
        return true;
    }
    return hb.select(i);
}

bool Hud::hotbarAssignAt(int slot,int x,int y){
    if(slot<0||slot>=Hotbar::SLOTS) return false;
    HotbarSlot hs;
    if(mode==MODE_PICK_COLOR){
        for(int i=0;i<NUM_COLORS;i++)
            if(inbox(x,y,colorBounds[i])){hs=HotbarSlot::paint(i+1);break;}
    }else if(mode==MODE_PICK_BLOCK){
        int cells[NUM_DISPLAY_BLOCKS];
        page_cells(pickerPage,cells);
        for(int i=0;i<pickerCellCount(pickerPage);i++){
            if(!inbox(x,y,blockBounds[i])) continue;
            const int t=cells[i];
            if(hud_picker_is_mod(t)){
                const std::string nm=modCellName(t);
                if(!nm.empty()) hs=HotbarSlot::modBlock(nm);
            }else if(hud_picker_is_tool(t)){
                if(pickerToolEnabled(t)) hs=HotbarSlot::block(t,t==HUD_TOOL_SIGN?paintColor:0);
            }else if(t==TYPE_GOLDEN_CUBE){
                if(goldencubes>0) hs=HotbarSlot::block(t,20);
            }else if(t==TYPE_DOOR_TOP){
                hs=HotbarSlot::block(t,20);
            }else if(t!=TYPE_BLOCK_TNT&&t!=TYPE_CUSTOM&&t>0){
                hs=HotbarSlot::block(t,0);
            }
            break;
        }
    }
    if(hs.kind==HOTBAR_EMPTY) return false;
    Hotbar::get().assign(slot,hs);
    sb->setStatus("Slot "+std::to_string(slot+1)+": "+Hotbar::label(hs),2.0f);
    return true;
}

void Hud::renderHotbar(){
    using namespace GLW;
    GLW::Toast::setLift(0);
    HbLayout L;
    if(!hotbarLayout(&L)) return;
    const bool touch=L.touch;
    const int NS=Hotbar::SLOTS;
    const int n=L.n;
    const float cell=L.cell,gap=L.gap,left=L.left,y=L.y,stripW=L.stripW;
    if(!touch) GLW::Toast::setLift(cell+du(8)+du(8));   // the status toast sits above the strip

    Hotbar& hb=Hotbar::get();
    const int sel=hb.selected();
    const int start=L.start;
    static int lastSel=-2;
    if(sel!=lastSel){
        if(lastSel!=-2&&sel>=0) s_hbLabelT=1.5f;
        lastSel=sel;
    }
    const float sc=IS_IPAD?SCALE_WIDTH:1.0f;
    const bool inUse=(mode==MODE_BUILD||mode==MODE_PAINT||mode==MODE_PICK_BLOCK||mode==MODE_PICK_COLOR);
    const float u1=GLW::u();
    Pen::color(1,1,1,1);
    Pen::blend(true);
    Pen::blendFunc(R::B_ALPHA);
    static GLW::Label hintLbl[Hotbar::SLOTS];
    static std::string hintTxt[Hotbar::SLOTS];
    static GLW::Label chevL,chevR;
    for(int k=0;k<n;k++){
        const int i=start+k;
        const HotbarSlot& sl=hb.slot(i);
        const bool cur=(i==sel);
        CGRect r=CGRectMake(left+k*(cell+gap),y+(cur?2*u1:0),cell,cell);
        GLW::fill(r.origin.x+2*u1,r.origin.y-2*u1,r.size.width,r.size.height,GLW::rgb(0x000000,0.35f));   // the hard drop shadow
        GLW::fill(r,GLW::rgb(cur?0x2c2b2b:0x000000,cur?0.8f:0.55f));
        const float pad=fmaxf(2.0f,floorf(cell*0.12f));
        const CGRect in=CGRectMake(r.origin.x+pad,r.origin.y+pad,r.size.width-pad*2,r.size.height-pad*2);
        const CGRect raw=CGRectMake(in.origin.x*sc,in.origin.y*sc,in.size.width*sc,in.size.height*sc);
        Pen::textured(true);
        Pen::color(1,1,1,1);
        const bool ok=sl.kind==HOTBAR_EMPTY||handAvailable(sl);
        if(sl.kind==HOTBAR_EMPTY){
            BlockIcon::drawEmpty(raw);
        }else if(sl.kind==HOTBAR_COLOUR){
            BlockIcon::drawSwatch(sl.colour,raw);
        }else if(!sl.mod.empty()){
            BlockIcon::drawMod(sl.mod,sl.colour,raw);
            if(sl.colour) BlockIcon::chip(sl.colour,raw);
        }else if(hud_picker_is_tool(sl.type)){
            Resources::getResources->getArtTex(sl.type==HUD_TOOL_SIGN?ART_SIGN_ICON:ART_CMD_BLOCK)->drawInRect(in);
        }else{
            BlockIcon::draw(sl.type,sl.colour,raw);
            if(sl.colour) BlockIcon::chip(sl.colour,raw);
        }
        Pen::textured(false);
        Pen::color(1,1,1,1);
        Pen::blend(true);
        Pen::blendFunc(R::B_ALPHA);
        if(!ok){   // not holdable here (a mod block this world lacks): dimmed
            GLW::fill(r,GLW::rgb(0x000000,0.6f));
            Pen::textured(true);
            BlockIcon::drawEmpty(raw);
            Pen::textured(false);
        }
        if(cur) hb_box(r,3*u1,GLW::rgb(0x89c31f,inUse?1.0f:0.45f));   // the picker's "chosen" lime; dim when the hand is a mine/burn tool
        else    hb_box(r,2*u1,GLW::rgb(0x646464));
        if(!touch){   // key hint: the live binding of hotbarN
            char nm[16];
            std::snprintf(nm,sizeof(nm),"hotbar%d",i+1);
            const char* kl=eden_keybind_code_label(eden_keybind_get(eden_keybind_index(nm)));
            if(kl&&*kl){
                if(hintTxt[i]!=kl){hintTxt[i]=kl;hintLbl[i].set(kl,du(11),UITextAlignmentLeft);}
                hintLbl[i].draw(r.origin.x+3*u1+(cur?3*u1:0),r.origin.y+r.size.height-2*u1-(cur?3*u1:0),GLW::rgb(0xffffff,0.85f));
            }
        }
    }
    if(n<NS){   // more slots that way
        if(chevL.empty()){chevL.set("<",du(14),UITextAlignmentCenter);chevR.set(">",du(14),UITextAlignmentCenter);}
        if(start>0) chevL.drawCentered(left-du(6),y+cell*0.5f+chevL.height()*0.5f,GLW::kText);
        if(start+n<NS) chevR.drawCentered(left+stripW+du(6),y+cell*0.5f+chevR.height()*0.5f,GLW::kText);
    }
    // The item name for 1.5 s after a selection change; above the strip (desktop) or below it (touch).
    static GLW::Label nameLbl;
    static std::string nameTxt;
    if(s_hbLabelT>0&&sel>=0){
        const std::string t=Hotbar::label(hb.slot(sel));
        if(t!=nameTxt||nameLbl.empty()){nameTxt=t;nameLbl.set(t.c_str(),du(16),UITextAlignmentCenter);}
        const float cx=left+stripW*0.5f;
        const float yTop=touch?y-du(6):y+cell+du(2)*2+du(6)+nameLbl.height();
        nameLbl.drawChrome(cx,yTop,GLW::kText,GLW::rgb(0x000000,0.8f));
    }
    Pen::textured(true);
    Pen::color(1,1,1,1);
}

void Hud::render(){
	//if(mode==MODE_MINE)return;
    Graphics::beginHud();
   
	if(mode==MODE_CAMERA){
        Graphics::setCameraFog(100);
        
		if(take_screenshot){
			if(!SUPPORTS_OGL2){
                Graphics::setZFAR(40);
                
              
                if(World::getWorld->terrain->tgen->LEVEL_SEED== 0){
                     Graphics::setZFAR(55);
                    
                }
            }
			//else P_ZFAR=70.0f;
			takeScreenshot();
			flash=1.0;
            flashcolor=MakeVector(1.0,1.0,1.0);
			take_screenshot=false;
			mode=MODE_BUILD;
			Resources::getResources->playSound(S_CAMERA);
		}
        Pen::blendFunc(R::B_ALPHA);
        sb->render();
        Graphics::endHud();
		
		return;
	}else{
        if(!SUPPORTS_OGL2){
            Graphics::setZFAR(40);
            
            
            if(World::getWorld->terrain->tgen->LEVEL_SEED== 0){
                Graphics::setZFAR(55);
                
            }
        }else{
            Graphics::setZFAR(40);
            
        }
       // if(!SUPPORTS_OGL2)P_ZFAR= 20.0f;
       // else P_ZFAR=70.0f;
    }
    lmode=mode;
	if(hideui){
        Graphics::endHud();
       
      return;  
    }
	
	
	
	Pen::color(1.0, 1.0, 1.0, 1.0f);
	Pen::blendFunc(R::B_ALPHA);
	//NSString* sfps=[NSString stringWithFormat:@"FPS:%3d",(int)fps];
	
	//[Graphics->drawText(sfps:10:10];
    
	
	
   	
	//glDisable(GL_TEXTURE_2D);
	//glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	
    Resources* res=Resources::getResources;
	
	//Texture2D* tcam=[res getTex:ICO_CAMERA];
	//Texture2D* tbuild=[res getTex:ICO_BUILD];	
	Texture2D* tmine=res->getTex(ICO_MINE);
	Texture2D* tburn=res->getTex(ICO_BURN);
	Texture2D* tjump=res->getTex(ICO_JUMP);
    Texture2D* tpaint=res->getPaintTex(paintColor);
	//Texture2D* tback=[res getTex:ICO_BACK];
	
	Pen::color(1.0, 1.0, 1.0, 1.0f);
    Button glowbox;
	if(mode==MODE_PICK_BLOCK||mode==MODE_BUILD){
        glowbox=rbuild;
        
    }else if(mode==MODE_BURN){
        glowbox=rburn;
        
    }else if(mode==MODE_PAINT||mode==MODE_PICK_COLOR){
        glowbox=rpaint;
    }else if(mode==MODE_MINE){
        glowbox=rmine;
    }
    
    if(mode!=MODE_NONE){
       /* glowbox.origin.x-=glowbox.size.width/2;
        glowbox.origin.y-=glowbox.size.height/2;
        glowbox.size.width*=2;
        glowbox.size.height*=2;       */ 
      //  [[res getTex:ICO_HIGHLIGHT] drawInRect2:glowbox];
    }
	Pen::color(1.0, 1.0, 1.0, 1.0f);
	if(mode==MODE_PICK_BLOCK){
		//glColor4f(1.0, 0.0, 0.0, 1.0f);
        renderBlockAndBorder(RectFromButton(rbuild));
	//[tbuild drawInRect2: rbuild];
    }else {
        renderBlockAndBorder(RectFromButton(rbuild));
        
    }
	Pen::color(1.0, 1.0, 1.0, 1.0f);
    // H.7: with the Experiments "Double-size build brush" on, a corner badge shows what the size key
    // does next (blocktoggle2: small to big, blocktoggle1: big to small).
    if(DOUBLE_BRUSH&&(mode==MODE_BUILD||mode==MODE_PICK_BLOCK)&&!hud_picker_is_tool(blocktype)){
        CGRect br=RectFromButton(rbuild);
        br.origin.x+=br.size.width*0.55f;
        br.size.width*=0.45f;
        br.size.height*=0.45f;
        Resources::getResources->getTex(build_size==2?ICO_SIZETOGGLE1:ICO_SIZETOGGLE2)->drawInRect2(br);
    }
    float offsetx;
    float offsety;

    if(IS_IPAD&&!SUPPORTS_RETINA){
       offsetx=(128-90)/2.0f/SCALE_WIDTH;
        offsety=(128-90)/2.0f/SCALE_HEIGHT;

        
    }else{
        offsetx=(128-90)/2.0f/2.0f;
      offsety=(128-90)/2.0f/2.0f;

    }
       if(IS_IPAD&&!SUPPORTS_RETINA){
      //  offsetx-=1;
     //   offsety-=2;
    }
	if(mode==MODE_MINE){
        glowbox=rmine;
        glowbox.origin.x-=offsetx;
        glowbox.origin.y-=offsety;
        res->getTex(ICO_MINE_ACTIVE)->drawButton(glowbox);
    }else
	tmine->drawButton( rmine);
	
    if(rmine.origin.x!=HUDR_X){
        
     //   printf("wtf: %f  chek1:%x  check2:%x\n",rmine.origin.x,(unsigned int)(&flashcolor),(unsigned int)&(World::getWorld->hud->flashcolor));
        
        printf("break here");
    }
    
	if(mode==MODE_BURN){
        glowbox=rburn;
        glowbox.origin.x-=offsetx;
        glowbox.origin.y-=offsety;
        res->getTex(ICO_BURN_ACTIVE)->drawButton(glowbox);
    }else
	tburn->drawButton(rburn);
    
    
	if(mode==MODE_PAINT||mode==MODE_PICK_COLOR){
        glowbox=rpaint;
        glowbox.origin.x-=offsetx;
        glowbox.origin.y-=offsety;
        res->getTex(ICO_PAINT_ACTIVE)->drawButton(glowbox);
        tpaint->drawButton( rpaint);
    }else
   	tpaint->drawButton( rpaint);
    
	
	if(m_jump||m_flyup){
        rjumprender.pressed=true;
        if(eden_profile_touch_chrome()) res->getTex(ICO_JUMP_ACTIVE)->drawButton(rjumprender);   // UX.3 (P6): no dead chrome on keyboard + mouse; the hit rect stays (Space drives it)
	}else{
        rjumprender.pressed=false;
        if(eden_profile_touch_chrome()) tjump->drawButton(rjumprender);
    }

    // Crouch button: its own art (Maysunbo, 2026-10-27; it was jump's, mirrored, with the shadow on
    // the wrong side). Hidden entirely when the "Crouch mode" experiment is off
    // (Settings.cpp's "crouch" row) — the button doesn't just no-op, it disappears.
    extern bool CROUCH_ENABLED;
    if(CROUCH_ENABLED||FLY_MODE){   // FM.3: also the Fly down button while flying
        if(m_crouch||m_flydown){
            rcrouchrender.pressed=true;
            res->getArtTex(ART_CROUCH_ACTIVE)->drawButton(rcrouchrender);
        }else{
            rcrouchrender.pressed=false;
            res->getArtTex(ART_CROUCH)->drawButton(rcrouchrender);
        }
    }

    // FM.3: the fly-mode cycle button (only when Allow fly is on). Glyph = the mode as a label on
    // the kit's dark plate, lime while flying; the labels are built once (a texture upload each).
    if(FLY_ALLOWED&&!inmenu){
        static GLW::Label flyLbl[3];
        static const char* const kFlyText[3]={"OFF","FLY","NC"};
        const int st=fly_get_state();
        if(flyLbl[st].empty())flyLbl[st].set(kFlyText[st],14,UITextAlignmentCenter);
        const CGRect r=CGRectMake(rflyrender.origin.x,rflyrender.origin.y,rflyrender.size.width,rflyrender.size.height);
        // X.31: no GLW::begin2D/end2D here. The HUD (Graphics::beginHud) is already the 2D bracket in
        // GLW's space, and a nested kitEnd() turned blending OFF and depth ON for the rest of the HUD:
        // every toast, the version tag and the joystick nub then drew their texture rects opaque. Same
        // set-up as renderMenuScreen instead.
        Pen::blend(true);
        Pen::blendFunc(R::B_ALPHA);
        GLW::fill(r,GLW::rgb(0x000000,rflyrender.pressed?0.8f:0.55f));
        flyLbl[st].drawCentered(r.origin.x+r.size.width/2,r.origin.y+r.size.height/2+flyLbl[st].height()/2,st?GLW::rgb(0xc6f175):GLW::kText);
        Pen::color(1,1,1,1);
    }



	res->getTex(ICO_OPEN_MENU)->drawButton(rmenu);
    if(inmenu||at1>0){


        renderMenuScreen();
    }
	
	if(use_joystick&&mode!=MODE_PICK_BLOCK&&mode!=MODE_PICK_COLOR){
		Pen::blendFunc(R::B_ALPHA);
		joystick->render();
	}
	else{
		Pen::blendFunc(R::B_ADD);
		//[gamepad render);
		Pen::blendFunc(R::B_ALPHA);
	}
	
	Pen::color(1.0, 1.0, 1.0, 1.0);
    
	MpWorld::renderPeerTags();   // MP.5
    if(consoleBtnShown()&&!inmenu){   // developer console button (singleplayer), where chat sits in a session
        Pen::textured(true);
        if(rcons.pressed) Pen::color(0.7f,0.7f,0.7f,1.0f); else Pen::color(1.0f,1.0f,1.0f,1.0f);
        Resources::getResources->getArtTex(ART_CONSOLE)->drawInRect(CGRectMake(rcons.origin.x,rcons.origin.y,rcons.size.width,rcons.size.height));
        Pen::color(1.0f,1.0f,1.0f,1.0f);
    }
    if(MpWorld::active()){   // MP.7: the chat feed (inert) and the touch button
        ChatFeed::renderHud(World::getWorld->menu&&World::getWorld->menu->showconsole);
        if(!inmenu) ChatFeed::renderButton(CGRectMake(rchat.origin.x,rchat.origin.y,rchat.size.width,rchat.size.height),rchat.pressed);
    }
    renderHotbar();   // UX.8-4 (sets the toast lift before the toast draws)
	sb->render();
    // Crosshair: a small cross with a gap at the screen centre (where mouse mine/build aims), drawn
    // dark first then light so it reads on any background. Hidden with no tool in hand and while a
    // picker is open.
    if(eden_get_crosshair()&&mode!=MODE_NONE&&mode!=MODE_PICK_BLOCK&&mode!=MODE_PICK_COLOR&&at2<=0&&at3<=0){
        float cw=SCREEN_WIDTH,ch=SCREEN_HEIGHT;
        if(IS_IPAD){
            if(IS_RETINA){cw=SCREEN_WIDTH*2;ch=SCREEN_HEIGHT*2;}
            else{cw=IPAD_WIDTH;ch=IPAD_HEIGHT;}
        }
        const float u=IS_IPAD?2.0f:1.0f;   // 1 px on a 1x phone, 2 on the 2x UI scale
        const float gap=3*u,len=7*u;
        const float cx=floorf(cw*0.5f),cy=floorf(ch*0.5f);
        for(int pass=0;pass<2;pass++){
            const float t=(pass==0?1.5f:0.5f)*u;   // shadow is wider than the light core
            const float cr=pass==0?0.0f:1.0f, ca=pass==0?0.55f:0.9f;
            Graphics::fillRect(cx-gap-len,cy-t,cx-gap,cy+t,cr,cr,cr,ca);
            Graphics::fillRect(cx+gap,cy-t,cx+gap+len,cy+t,cr,cr,cr,ca);
            Graphics::fillRect(cx-t,cy-gap-len,cx+t,cy-gap,cr,cr,cr,ca);
            Graphics::fillRect(cx-t,cy+gap,cx+t,cy+gap+len,cr,cr,cr,ca);
        }
        Pen::color(1.0,1.0,1.0,1.0);
    }
	if(mode==MODE_PICK_BLOCK||at2>0){
		renderBlockScreen();
	}
    if(mode==MODE_PICK_COLOR||at3>0){
       renderColorPickScreen();
    }
    if(underLiquid){
        //NSLog(@"%f",flash);
		Graphics::fillScreen(liquidColor.x, liquidColor.y, liquidColor.z, 0.5f);   // AT.8: unchanged; the underwater fog uses the same colour, so far terrain and this fill agree (AT.9 retunes)

    }
	if(flash>0||World::getWorld->player->flash>0){
       /// printf("hello\n");
        
		//NSLog(@"%f",flash);
		if(flash>0)
			Graphics::fillScreen(flashcolor.x, flashcolor.y, flashcolor.z, flash);
		else
			Graphics::fillScreen(1.0f, 0.0f, 0.0f, World::getWorld->player->flash);
        
		
	}
    // P.5: the warp home loads + meshes synchronously (330 / 222 ms). Dim the last frames before it so the
    // freeze reads as "Saving and warping home.." rather than a hang (delayedtimer 2 -> 1 -> fires).
    float cover=fade_out;
    if(delayedtimer>0&&delayedaction==6&&cover<.8f)cover=.8f;
    if(cover>0){
        Graphics::fillScreen(0,0,0,cover);
        if(cover>fade_out)sb->render(); // status text was drawn before the overlay; redraw on top

    }
  /*  glDisable(GL_TEXTURE_2D);
    glColor4f(test2.x,test2.y,test2.z,1.0f);
    
    [Graphics drawRect:test2r.origin.x:test2r.origin.y:test2r.size.width:test2r.size.height);
    glColor4f(test1.x,test1.y,test1.z,1.0f);
   
     [Graphics drawRect:test1r.origin.x:test1r.origin.y:test1r.size.width:test1r.size.height);
   */

    Pen::color(1.0f,1.0f,1.0f,1.0f);
  
    Pen::textured(true);
    // UX.3 (B2): right of the corner buttons (menu gear, then chat/console), not over the gear.
    GLW::drawVersionTag((consoleBtnShown()||MpWorld::active()?rchat.origin.x+rchat.size.width:rmenu.origin.x+rmenu.size.width)-rmenu.origin.x+6);
    
    Graphics::endHud();
    
}

void Hud::asetHome(){
    if(MpWorld::active())return;   // MP.3a: the server's SPAWN is a session's home
    Vector thome;
    Vector pp=World::getWorld->player->pos;
    thome.x=pp.x-.5f;
    thome.z=pp.z-.5f;
    thome.y=pp.y-1;
    World::getWorld->terrain->home=thome;
    const bool saved=World::getWorld->fm->saveWorld();   // QA.5: home is set either way; the next save carries it
    //[World::getWorld->terrain updateAllImportantChunks);
    //NSLog(@"saving..");
    Input::getInput()->clearAll();

    World::getWorld->terrain->startDynamics();
    Resources::getResources->playSound(S_WARP_HOME_LOCATION_SET);
    if(saved)sb->setStatus("World Saved",3);

}
void Hud::awarpHome(){
    if(MpWorld::active())return;   // MP.3a: no Warp Home in a session (death still respawns at SPAWN)
    delayedtimer=2;
    delayedaction=6;
    
    
    sb->setStatus("Saving and warping home.." ,999);
}

//@end
