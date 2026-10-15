//
//  SettingsMenu.mm
//  prototype
//
//  Created by Ari Ronen on 11/4/10.
//  Copyright 2010 __MyCompanyName__. All rights reserved.
//
//  Phase N Stage 2.5 (WORKING/native-migration-plan-2026-09-04.md): render()/update() were a
//  fixed five-row panel of hand-placed ON/OFF images with a hard-coded `if` chain. They are now a
//  generic paginated list over the shared settings table — the same data
//  port/src/seam/Settings_web.mm feeds public/eden-settings.js, reached here through the
//  eden_settings_* accessors (added in that file's portable half, so both targets link them).
//
//  Stage 5.4 (WORKING/ROADMAP.md) reskinned it onto the GL widget kit: a WINDOW panel with a
//  titlebar, CONTENT strips per row, and the kit's Toggle / Slider / Stepper as the three row
//  controls — a real draggable slider for KIND_RANGE replacing the Stage 2.5 [-] [+] stepper.
//  Every pixel is a GLW:: call; the shape is KeybindsMenu.mm's, which is the one to copy.
//
//  Stage U.3 (WORKING/ui-rework-plan-2026-10-11.md) replaced the paged list with the reference
//  layout: a fixed panel, a 7-tab IconRail, two-line rows (title + hint) in a row-snapped
//  variable-height ScrollView, dependents indented and muted while their parent is off. U.4 made
//  the Keys tab the keybinds list (KeybindsMenu, embedded in the content rect) with a Defaults
//  action in the titlebar.
//
//  On WEB this code does not run: Settings_web.mm --wrap's SettingsMenu::update/render to no-ops
//  (the DOM panel owns settings there). It is the native build's real settings UI, and the
//  fallback for any future host without a DOM.
//
//  load()/save()/getNewWorldName() and `properties[]` are UNCHANGED — they own the five
//  engine-backed toggles' meaning and the NSUserDefaults round-trip, and Settings_web.mm's ENG_*
//  mapping still indexes `properties[]`. All value writes here go through eden_settings_set(),
//  which does the clamp + engine commit + the music-tune side effect + the port-settings re-apply.
//

#import "SettingsMenu.h"
#import "GLWidgets.h"
#import "Graphics.h"
#import "KeybindsMenu.h"
#import "Globals.h"
#import "Util.h"
#import "World.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

// The shared settings table, as scalars (Settings_web.mm). Declared here rather than via a header
// to match the eden_menu_take_pending_world_height / eden_report_load_failure convention — a
// from-scratch iOS build supplies its own stub.
extern "C" {
    int         eden_settings_count(void);
    const char* eden_settings_label(int i);
    const char* eden_settings_key(int i);
    const char* eden_settings_group(int i);
    const char* eden_settings_hint(int i);
    int         eden_settings_parent(int i);        // U.2: the toggle row this one depends on, or -1
    int         eden_settings_kind(int i);          // 0 toggle, 1 range, 2 enum
    float       eden_settings_min(int i);
    float       eden_settings_max(int i);
    float       eden_settings_step(int i);
    int         eden_settings_enum_count(int i);
    const char* eden_settings_enum_label(int i, int j);
    int         eden_settings_native_hidden(int i);
    float       eden_settings_get(int i);
    void        eden_settings_set(int i, float v);
    float       eden_settings_toggle(int i);
    int         eden_settings_loaded(void);
    void        eden_settings_menu_close(void);
    void        eden_keybind_reset_all(void);
}

enum { SM_KIND_TOGGLE = 0, SM_KIND_RANGE = 1, SM_KIND_ENUM = 2 };

enum  {

	//S_LEFTY_MODE=1,
	S_PLAY_MUSIC=4,
	S_PLAY_SOUND=3,
    S_HEALTH=2,
	S_AUTOJUMP=1,
    S_CREATURES=0,
};
static NSString* pnames[NUM_PROP]={
	[S_HEALTH]=@"Health",
	[S_PLAY_MUSIC]=@"Music",
	[S_PLAY_SOUND]=@"Sound Effects",
	[S_AUTOJUMP]=@"Autojump",
    [S_CREATURES]=@"Creatures"
};
static const int pdefaults[NUM_PROP]={
	[S_HEALTH]=TRUE,
	[S_PLAY_MUSIC]=TRUE,
	[S_PLAY_SOUND]=TRUE,
	[S_AUTOJUMP]=TRUE,
    [S_CREATURES]=TRUE,
};
extern float SCREEN_WIDTH;
extern float SCREEN_HEIGHT;
extern float P_ASPECT_RATIO;

// ---------------------------------------------------------------------------------------------
SettingsMenu::SettingsMenu(){
	for(int i=0;i<NUM_PROP;i++){
		properties[i].name=pnames[i];
		properties[i].value=pdefaults[i];
		properties[i].tex=NULL;
	}

	rect_settings.size.width=246;
	rect_settings.size.height=45;
	rect_settings.origin.x=SCREEN_WIDTH/2-rect_settings.size.width/2;
	rect_settings.origin.y=SCREEN_HEIGHT-rect_settings.size.height-3;

    if(LOW_MEM_DEVICE){
        properties[S_CREATURES].value=false;
    }

    m_built=false;
    m_seeded=false;
    m_tab=0;
    m_builtTab=-1;
    m_nRows=0;
    m_titleY=0;
    m_builtU=0;
    m_dragSlot=-1;
    m_dragTouch=-1;
    m_scrollTouch=-1;
    m_barDrag=false;
    m_railTouch=-1;
    m_panel=m_content=CGRectMake(0,0,0,0);
    for(int k=0;k<SM_MAX_ROWS;k++) m_si[k]=-1;
    m_keys=new KeybindsMenu();

	this->load();
}

SettingsMenu::~SettingsMenu(){
    delete m_keys;
}

// ---------------------------------------------------------------------------------------------
// tabs (U.3)
// ---------------------------------------------------------------------------------------------
// The rail's seven tabs (plan D5), in rail order. `group` is the kSettings[] group string a row
// must carry to land on the tab; Interface's rows (crosshair, block preview, legacy UI) go to Video,
// mapped here rather than by retitling them so the DOM panel's sections are untouched (U.2). Keys
// has no settings rows: its content is the embedded KeybindsMenu (U.4).
struct SMTab { const char* title; const char* group; int icon; const char* shot; };
static const SMTab kTabs[]={
    { "Gameplay",    "Gameplay",    UIICON_WRENCH,        "gameplay"    },
    { "Audio",       "Audio",       UIICON_VOLUME_2,      "audio"       },
    { "Controls",    "Controls",    UIICON_GAMEPAD_2,     "controls"    },
    { "Keys",        NULL,          UIICON_KEYBOARD,      "keys"        },
    { "Video",       "Video",       UIICON_MONITOR,       "video"       },
    { "Experiments", "Experiments", UIICON_FLASK_CONICAL, "experiments" },
    { "Saves",       "Saves",       UIICON_HARD_DRIVE,    "saves"       },
};
enum { SM_TABS=(int)(sizeof(kTabs)/sizeof(kTabs[0])), SM_TAB_KEYS=3 };

static int sm_tab_of_group(const char* g){
    if(std::strcmp(g,"Interface")==0) g="Video";
    for(int t=0;t<SM_TABS;t++)
        if(kTabs[t].group&&std::strcmp(kTabs[t].group,g)==0) return t;
    return -1;
}

void SettingsMenu::build(){
    if(m_built) return;
    m_built=true;
    m_tabRows.assign(SM_TABS,std::vector<int>());
    const int count=eden_settings_count();
    for(int i=0;i<count;i++){
        if(eden_settings_native_hidden(i)) continue;
        int t=sm_tab_of_group(eden_settings_group(i));
        if(t<0){
            // A new group string with no tab: shown (on Gameplay) rather than silently dropped.
            std::fprintf(stderr,"[eden-settings] group '%s' has no tab; row '%s' shown on Gameplay\n",
                         eden_settings_group(i),eden_settings_key(i));
            t=0;
        }
        if((int)m_tabRows[t].size()>=SM_MAX_ROWS){
            std::fprintf(stderr,"[eden-settings] tab '%s' is over SM_MAX_ROWS; row '%s' dropped\n",
                         kTabs[t].title,eden_settings_key(i));
            continue;
        }
        m_tabRows[t].push_back(i);
    }
    int icons[SM_TABS];
    for(int t=0;t<SM_TABS;t++) icons[t]=kTabs[t].icon;
    m_rail.setTiles(icons,SM_TABS);
    for(int t=0;t<SM_TABS;t++) m_rail.setEnabled(t,t==SM_TAB_KEYS||!m_tabRows[t].empty());
    if(m_tab<0||m_tab>=SM_TABS||(m_tab!=SM_TAB_KEYS&&m_tabRows[m_tab].empty())) m_tab=0;
    m_builtTab=-1;
}

void SettingsMenu::selectTab(int t){
    build();
    if(t<0||t>=SM_TABS||(t!=SM_TAB_KEYS&&m_tabRows[t].empty())) return;
    if(t!=m_tab){
        if(m_keys) m_keys->cancelCapture();
        m_tab=t;
        m_scroll.setFirst(0);
        if(m_keys) m_keys->scroll().setFirst(0);
        m_builtTab=-1;
    }
    m_rail.ensureVisible(t);
}

// The control column a row reserves (plan §3.1): the 104x34u toggle, a ~200u slider with its 40u
// readout beside it, or a stepper of two 29u buttons around a 110u well.
void SettingsMenu::controlSize(int si,float* w,float* h) const{
    using namespace GLW;
    switch(eden_settings_kind(si)){
    case SM_KIND_TOGGLE: *w=Toggle::designW(); *h=Toggle::designH(); break;
    case SM_KIND_ENUM:   *w=du(29)*2.0f+du(110); *h=du(29); break;
    default:             *w=du(200)+du(48); *h=du(29); break;
    }
}

bool SettingsMenu::rowMuted(int k) const{
    const int p=eden_settings_parent(m_si[k]);
    return p>=0&&eden_settings_get(p)==0.0f;
}

bool SettingsMenu::rowShown(int k) const{
    return m_seeded&&k>=m_scroll.first()&&k<m_scroll.first()+m_scroll.visible()&&k<m_nRows;
}

void SettingsMenu::refreshValue(int k){
    if(k<0||k>=m_nRows) return;
    const int si=m_si[k];
    char buf[64];
    switch(eden_settings_kind(si)){
    case SM_KIND_TOGGLE:
        m_toggle[k].setOn(eden_settings_get(si)!=0.0f);
        break;
    case SM_KIND_ENUM:{
        int idx=(int)lroundf(eden_settings_get(si));
        int n=eden_settings_enum_count(si);
        if(idx<0) idx=0; if(n>0&&idx>=n) idx=n-1;
        m_stepper[k].setValueText(eden_settings_enum_label(si,idx), GLW::du(20));
        break;
    }
    default:{
        const float v=eden_settings_get(si);
        if(!m_slider[k].dragging()) m_slider[k].setValue(v);
        // A 0..1 range is a volume and reads as a percentage; anything else is a multiplier or a
        // count, printed in the precision its step implies.
        if(eden_settings_min(si)==0.0f&&eden_settings_max(si)==1.0f)
            std::snprintf(buf,sizeof(buf),"%d%%",(int)lroundf(v*100.0f));
        else
            std::snprintf(buf,sizeof(buf),(eden_settings_step(si)>=1.0f?"%.0f":"%.2f"),v);
        // LEFT-aligned beside the track, not right-aligned in a box: Label aligns inside its POT
        // texture, which is wider than the box, so "right" printed past the row's edge.
        m_valueLabel[k].set(buf, GLW::du(20), UITextAlignmentLeft);
        break;
    }
    }
}

// Every label of the tab, built ONCE on a tab switch (or a u() change), never per frame: a label is
// a texture upload per line, and the worst tab (Video) is ~15 rows of title + hint + value. The
// TwoLineRows themselves are (re)built by layout(), which is free when nothing changed.
void SettingsMenu::buildRows(){
    using namespace GLW;
    m_builtTab=m_tab;
    m_builtU=u();
    m_dragSlot=-1;
    m_dragTouch=-1;
    char buf[64];
    std::snprintf(buf,sizeof(buf),"%s Settings",kTabs[m_tab].title);
    m_title.set(buf, du(32), UITextAlignmentCenter);
    m_back.setLabel("Back", du(20));
    m_back.sizeToFit();
    m_defaults.setLabel("Defaults", du(20));
    m_defaults.sizeToFit();
    if(m_tab==SM_TAB_KEYS&&m_keys) m_keys->build();
    const std::vector<int>& rows=m_tabRows[m_tab];
    m_nRows=(int)rows.size();       // 0 on Keys: its rows are KeybindsMenu's
    for(int k=0;k<SM_MAX_ROWS;k++){
        m_valueLabel[k].clear();
        m_slider[k].endDrag();
        m_si[k]=k<m_nRows?rows[k]:-1;
        if(k>=m_nRows) continue;
        const int si=m_si[k];
        const int kind=eden_settings_kind(si);
        if(kind==SM_KIND_RANGE)
            m_slider[k].setRange(eden_settings_min(si),eden_settings_max(si),eden_settings_step(si));
        else if(kind==SM_KIND_ENUM)
            m_stepper[k].setGlyphs("<",">");
        refreshValue(k);
    }
}

void SettingsMenu::layout(){
    using namespace GLW;
    if(!m_seeded&&eden_settings_loaded()){ m_seeded=true; m_builtTab=-1; }
    if(m_builtTab!=m_tab||m_builtU!=u()) buildRows();

    // THE PANEL IS FIXED (plan D3, §3.1): the 783x587u design frame less a 12u margin, centred, the
    // same on every tab so switching never resizes it. Since U.5 the arithmetic is the kit's
    // railFrame(), shared with New World.
    const RailFrame f=railFrame();
    m_panel=f.panel;
    const CGRect& T=f.titlebar;

    // Titlebar: Back shrink-wrapped at the left, the title centred on the WINDOW. 29u art; on touch
    // the hit box grows to the 44pt floor, the art does not.
    m_back.setRect(CGRectMake(T.origin.x,T.origin.y,0,T.size.height));
    titlebarHit(m_back);
    // Defaults (Keys only), shrink-wrapped at the right: the tab's titlebar action (plan §3, U.4).
    m_defaults.setRect(CGRectMake(T.origin.x+T.size.width-m_defaults.fitWidth(),T.origin.y,0,T.size.height));
    titlebarHit(m_defaults);
    m_titleY=f.titleY(m_title);

    // Body: rail | content | scrollbar, 6u apart.
    m_rail.setRect(f.rail);
    m_rail.setSelected(m_tab);
    const CGRect barRect=f.bar;
    m_scroll.setBarRect(barRect);
    m_content=f.content;
    const CGRect& C=m_content;
    const float tf=touchFloor();
    if(m_tab==SM_TAB_KEYS){
        if(m_keys) m_keys->layout(C,barRect);
        return;
    }

    // Rows: heights from the BUILT labels (TwoLineRow::build re-wraps only when the width changed).
    float heights[SM_MAX_ROWS];
    for(int k=0;k<m_nRows;k++){
        const int si=m_si[k];
        float cw,ch;
        controlSize(si,&cw,&ch);
        const float indent=eden_settings_parent(si)>=0?du(14):0.0f;
        m_row[k].build(eden_settings_label(si),eden_settings_hint(si),C.size.width,cw,ch,indent);
        m_row[k].setLast(k==m_nRows-1);
        heights[k]=m_row[k].height();
    }
    m_scroll.setRowHeights(heights,m_nRows,C.size.height);

    const bool touch=tf>0.0f;
    for(int k=0;k<m_nRows;k++){
        if(!rowShown(k)) continue;
        m_row[k].setOrigin(C.origin.x,C.origin.y+C.size.height-m_scroll.rowOffset(k));
        const CGRect rr=m_row[k].rect();
        const int si=m_si[k];
        float cw,ch;
        controlSize(si,&cw,&ch);
        const CGRect cr=m_row[k].controlRect(cw,ch);
        // The CONTROLS stay compact at any profile; on touch the hit box grows to the row
        // (design-system.md, "Touch floor": the floor raises the hit box, not the art).
        switch(eden_settings_kind(si)){
        case SM_KIND_TOGGLE:
            m_toggle[k].setRect(cr);
            m_toggle[k].setHitRect(touch?rr:CGRectMake(0,0,0,0));
            break;
        case SM_KIND_ENUM:
            m_stepper[k].setRect(cr);
            m_stepper[k].setHitRect(CGRectMake(cr.origin.x,rr.origin.y,cr.size.width,rr.size.height));
            break;
        default:{
            const CGRect sr=CGRectMake(cr.origin.x,cr.origin.y,du(200),cr.size.height);
            m_slider[k].setRect(sr);
            m_slider[k].setHitRect(CGRectMake(sr.origin.x-du(8),rr.origin.y,sr.size.width+du(16),rr.size.height));
            break;
        }
        }
    }
}

void SettingsMenu::commitSlider(int k){
    eden_settings_set(m_si[k],m_slider[k].value());   // set() clamps + commits + side effects
    refreshValue(k);
}

void SettingsMenu::stepRow(int k,int dir){
    const int si=m_si[k];
    int kind=eden_settings_kind(si);
    if(kind==SM_KIND_ENUM){
        int n=eden_settings_enum_count(si); if(n<1) n=1;
        int idx=(int)lroundf(eden_settings_get(si))+dir;
        if(idx<0) idx=n-1; if(idx>=n) idx=0;
        eden_settings_set(si,(float)idx);
    }else{
        float st=eden_settings_step(si); if(st<=0.0f) st=0.05f;
        eden_settings_set(si, eden_settings_get(si)+dir*st);   // set() clamps to [min,max]
    }
    // An enum can drive others (the graphics preset sets view distance): refresh the whole tab.
    for(int j=0;j<m_nRows;j++) if(j==k||eden_settings_kind(m_si[j])!=SM_KIND_TOGGLE) refreshValue(j);
}

static const int usage_id=3;

void SettingsMenu::update(float etime){
    (void)etime;
    build();
    layout();
    const bool keysTab=m_tab==SM_TAB_KEYS&&m_keys;
    GLW::ScrollView& scroll=keysTab?m_keys->scroll():m_scroll;
    if(keysTab) m_keys->poll();
    // While a key capture is armed the screen is inert: a touch cancels it and does nothing else.
    // Letting the tap that gets you out of "press a key" also act on what is under it would rebind
    // (or Back out of) whatever you happened to hit.
    const bool capturing=keysTab&&m_keys->capturing();

    // The wheel scrolls the rows (wheel away = earlier rows). The settings rail fits its seven
    // tiles at any u (plan §3.1), so it has no wheel of its own here.
    if(int wheel=eden_ui_take_wheel()) if(!capturing) scroll.scrollBy(-wheel);

    Input* input=Input::getInput();
    itouch* touches=input->getTouches();

    for(int i=0;i<MAX_TOUCHES;i++){
        if(touches[i].inuse==0&&touches[i].down==M_DOWN){
            touches[i].inuse=usage_id;
            if(capturing) continue;
            const float mx=touches[i].mx, my=touches[i].my;
            m_back.setPressed(m_back.hit(mx,my));
            if(keysTab) m_defaults.setPressed(m_defaults.hit(mx,my));
            if(m_rail.hitRail(mx,my)&&m_railTouch<0){
                m_railTouch=i;
                m_rail.setHeld(m_rail.hit(mx,my));
                m_rail.beginDrag(my);
                continue;
            }
            if(scroll.hitBar(mx,my)&&m_scrollTouch<0){
                m_scrollTouch=i; m_barDrag=true;
                scroll.barBegin(my);
                continue;
            }
            // A key button is pressed on the down edge but armed only on release (up() below),
            // so a drag that starts on one still scrolls.
            if(keysTab) m_keys->down(mx,my);
            bool grabbed=false;
            for(int k=0;k<m_nRows&&!grabbed;k++){
                if(!rowShown(k)) continue;
                const int kind=eden_settings_kind(m_si[k]);
                if(kind==SM_KIND_RANGE&&m_dragSlot<0&&m_slider[k].hit(mx,my)){
                    // The down edge already moves the thumb — a tap on the track is a jump there.
                    m_dragSlot=k; m_dragTouch=i; grabbed=true;
                    if(m_slider[k].beginDrag(mx)) commitSlider(k);
                }else if(kind==SM_KIND_ENUM){
                    m_stepper[k].setPressed(m_stepper[k].hitDec(mx,my),m_stepper[k].hitInc(mx,my));
                }
            }
            // Anywhere else in the content may become a scroll (a touch drag); until it travels
            // past the slop it is still a tap, and the release below acts on it.
            if(!grabbed&&m_scrollTouch<0&&mx>=m_content.origin.x&&mx<=m_content.origin.x+m_content.size.width&&
               my>=m_content.origin.y&&my<=m_content.origin.y+m_content.size.height){
                m_scrollTouch=i; m_barDrag=false;
                scroll.beginContentDrag(my);
            }
        }
        if(touches[i].inuse==usage_id&&touches[i].down==M_DOWN){
            const float mx=touches[i].mx, my=touches[i].my;
            // Held: only the touch that grabbed a thumb drives it, and it keeps driving it when the
            // finger wanders off the track (vertically or past the ends), as a native slider does.
            if(i==m_dragTouch&&m_dragSlot>=0){
                if(m_slider[m_dragSlot].dragTo(mx)) commitSlider(m_dragSlot);
            }
            if(i==m_scrollTouch){
                if(m_barDrag) scroll.barDragTo(my);
                else if(scroll.contentDragTo(my)){
                    for(int k=0;k<SM_MAX_ROWS;k++) m_stepper[k].setPressed(false,false);
                    if(keysTab) m_keys->clearPressed();
                }
            }
            if(i==m_railTouch&&m_rail.dragTo(my)) m_rail.setHeld(-1);
        }
        if(touches[i].inuse==usage_id&&touches[i].down==M_RELEASE){
            const float mx=touches[i].mx, my=touches[i].my;
            touches[i].inuse=0;
            touches[i].down=M_NONE;
            m_back.setPressed(false);
            m_defaults.setPressed(false);
            for(int k=0;k<SM_MAX_ROWS;k++) m_stepper[k].setPressed(false,false);
            if(capturing){ m_keys->cancelCapture(); continue; }

            if(i==m_dragTouch){
                if(m_dragSlot>=0){
                    if(m_slider[m_dragSlot].dragTo(mx)) commitSlider(m_dragSlot);
                    m_slider[m_dragSlot].endDrag();
                }
                m_dragSlot=-1; m_dragTouch=-1;
                continue;                     // a drag that ends over a button does not press it
            }
            if(i==m_scrollTouch){
                const bool scrolled=m_barDrag||scroll.scrolling();
                scroll.endDrag();
                m_scrollTouch=-1; m_barDrag=false;
                if(scrolled){                 // a scroll is not a tap on the row it ended over
                    if(keysTab) m_keys->clearPressed();
                    continue;
                }
            }
            if(i==m_railTouch){
                const bool scrolled=m_rail.scrolling();
                m_rail.endDrag();
                m_rail.setHeld(-1);
                m_railTouch=-1;
                if(scrolled) continue;
                const int t=m_rail.hit(mx,my);
                if(t>=0) selectTab(t);
                continue;
            }
            if(m_back.hit(mx,my)){
                // save() + showsettings=FALSE, plus the two things a bare save() misses: it
                // re-applies the port settings load() stomps (invertcam, use_joystick — harmless
                // on the title screen, wrong in a world since Stage 5.5 opens this in-game) and
                // clears the touch table so the release does not land on the screen beneath.
                eden_settings_menu_close();
                return;
            }
            if(keysTab){
                if(m_defaults.hit(mx,my)){ eden_keybind_reset_all(); m_keys->refresh(); continue; }
                m_keys->up(mx,my);
                continue;
            }
            // A muted (dependent) row still works: muting is display-only (plan §5), so a volume
            // can be set while its sound is off.
            for(int k=0;k<m_nRows;k++){
                if(!rowShown(k)) continue;
                const int si=m_si[k];
                const int kind=eden_settings_kind(si);
                if(kind==SM_KIND_TOGGLE){
                    if(m_toggle[k].hit(mx,my)){ eden_settings_toggle(si); refreshValue(k); break; }
                }else if(kind==SM_KIND_ENUM){
                    if(m_stepper[k].hitDec(mx,my))      { stepRow(k,-1); break; }
                    else if(m_stepper[k].hitInc(mx,my)) { stepRow(k,+1); break; }
                }
            }
        }
    }
}

void SettingsMenu::load(){
	NSUserDefaults *prefs = [NSUserDefaults standardUserDefaults];
    for(int i=0;i<NUM_PROP;i++){
		NSNumber* n=[prefs objectForKey:properties[i].name];
		if(n!=nil){
			properties[i].value=[n intValue];
		}
	}
	world_counter=0;
	NSNumber* n=[prefs objectForKey:@"new_world_counter"];
	if(n!=nil)
	world_counter=[n intValue];
	Resources::getResources->playmusic=properties[S_PLAY_MUSIC].value;
	Resources::getResources->playsound=properties[S_PLAY_SOUND].value;
   World::getWorld->player->autojump_option=properties[S_AUTOJUMP].value;
    World::getWorld->player->health_option=properties[S_HEALTH].value;
	World::getWorld->player->invertcam=FALSE;
	World::getWorld->hud->use_joystick=TRUE;
    World::getWorld->terrain->tgen->genCaves=FALSE;
    World::getWorld->bestGraphics=properties[S_AUTOJUMP].value;
    CREATURES_ON=properties[S_CREATURES].value;

    World::getWorld->bestGraphics=TRUE;
    if(LOW_MEM_DEVICE||LOW_GRAPHICS){
        World::getWorld->bestGraphics=FALSE;
    }
    extern BOOL IS_WIDESCREEN;
    if(IS_WIDESCREEN){
        World::getWorld->bestGraphics=TRUE;
    }
}
void SettingsMenu::save(){
	NSUserDefaults *prefs = [NSUserDefaults standardUserDefaults];
	for(int i=0;i<NUM_PROP;i++){
		[prefs setObject:[NSNumber numberWithInt:properties[i].value] forKey:properties[i].name];
	}
	[prefs synchronize];
    this->load();

}
void SettingsMenu::showKeybinds(){
    selectTab(SM_TAB_KEYS);
    layout();
}

void SettingsMenu::resetView(){
    if(m_keys){ m_keys->cancelCapture(); m_keys->clearPressed(); }
    build();
    selectTab(0);
    m_scroll.setFirst(0);
    m_rail.ensureVisible(0);
    m_dragSlot=m_dragTouch=-1;
    m_scrollTouch=m_railTouch=-1;
    m_barDrag=false;
    m_scroll.endDrag();
    m_rail.endDrag();
    m_rail.setHeld(-1);
}

// U.3: Escape (Input_native.cpp, through eden_settings_menu_back). It used to arrive as a tap on the
// HUD's menu button, which this screen claimed as one of its own touches and dropped — the
// "Escape inside in-game Settings is swallowed" bug. Since U.4 Keys is a tab, so Escape is Back on
// every tab. (Escape during a key capture never gets here: the keybind model consumes it.)
void SettingsMenu::back(){
    if(m_keys&&m_keys->capturing()){ m_keys->cancelCapture(); return; }
    eden_settings_menu_close();
}

// --shot / --ui-selftest entry points (see the header). They lay the tab out immediately so a
// caller can read rects back without waiting a frame.
int SettingsMenu::tabCount() const{
    return SM_TABS;
}

const char* SettingsMenu::tabName(int t) const{
    return (t>=0&&t<SM_TABS)?kTabs[t].shot:"";
}

int SettingsMenu::tabRowCount(int t){
    build();
    return (t>=0&&t<SM_TABS)?(int)m_tabRows[t].size():0;
}

void SettingsMenu::showTab(int t){
    build();
    selectTab(t);
    layout();
}

bool SettingsMenu::keyRect(int keybind,CGRect* r){
    showKeybinds();
    return m_keys&&m_keys->keyRect(keybind,r);
}

bool SettingsMenu::showRow(const char* key,CGRect* control){
    build();
    if(!key) return false;
    for(int t=0;t<SM_TABS;t++){
        for(int k=0;k<(int)m_tabRows[t].size();k++){
            const int si=m_tabRows[t][k];
            if(std::strcmp(eden_settings_key(si),key)!=0) continue;
            selectTab(t);
            layout();
            m_scroll.ensureVisible(k);
            layout();
            if(control){
                switch(eden_settings_kind(si)){
                case SM_KIND_TOGGLE: *control=m_toggle[k].rect(); break;
                case SM_KIND_ENUM:   *control=m_stepper[k].rect(); break;
                default:             *control=m_slider[k].rect(); break;
                }
            }
            return true;
        }
    }
    return false;
}

NSString* SettingsMenu::getNewWorldName(){
	world_counter++;

	NSUserDefaults *prefs = [NSUserDefaults standardUserDefaults];
	[prefs setObject:[NSNumber numberWithInt:world_counter] forKey:@"new_world_counter"];
	[prefs synchronize];
	return [NSString stringWithFormat:@"World %d",world_counter];
}

void SettingsMenu::render(){
    build();
    layout();

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);

    const float s = IS_IPAD ? SCALE_WIDTH : 1.0f;
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrthof(0, SCREEN_WIDTH * s, 0, SCREEN_HEIGHT * s, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    using namespace GLW;
    fill(CGRectMake(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT), kScrim);
    bevel(m_panel, BEVEL_WINDOW);

    m_back.render();
    m_title.drawCentered(m_panel.origin.x+m_panel.size.width*0.5f, m_titleY, kText);
    m_rail.render();

    // One CONTENT surface; the rows are hairline-separated on it, no box per row.
    bevel(m_content, BEVEL_CONTENT);
    if(m_tab==SM_TAB_KEYS&&m_keys){
        m_defaults.render();
        m_keys->render();
        m_keys->renderCapture(m_panel);
    }
    for(int k=0;k<m_nRows;k++){
        if(!rowShown(k)) continue;
        const bool muted=rowMuted(k);
        m_row[k].setMuted(muted);
        m_row[k].render();
        switch(eden_settings_kind(m_si[k])){
        case SM_KIND_TOGGLE:
            m_toggle[k].setMuted(muted);
            m_toggle[k].render();
            break;
        case SM_KIND_ENUM: m_stepper[k].render(); break;
        default:{
            m_slider[k].render();
            const CGRect sr=m_slider[k].rect();
            const float cy=sr.origin.y+sr.size.height*0.5f;
            m_valueLabel[k].draw(sr.origin.x+sr.size.width+du(8), cy+m_valueLabel[k].lineHeight()*0.5f,
                                 muted?kTextMuted:kText);
            break;
        }
        }
    }
    if(m_tab!=SM_TAB_KEYS) m_scroll.render();

    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();

    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
}
