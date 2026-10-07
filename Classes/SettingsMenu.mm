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
//  web/src/seam/Settings_web.mm feeds public/eden-settings.js, reached here through the
//  eden_settings_* accessors (added in that file's portable half, so both targets link them).
//
//  Stage 5.4 (WORKING/ROADMAP.md) reskinned it onto the GL widget kit: a WINDOW panel with a
//  titlebar, CONTENT strips per row, and the kit's Toggle / Slider / Stepper as the three row
//  controls — a real draggable slider for KIND_RANGE replacing the Stage 2.5 [-] [+] stepper.
//  Every pixel is a GLW:: call; the shape is KeybindsMenu.mm's, which is the one to copy.
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
    m_rowsPerPage=8;
    m_maxPageLen=0;
    m_pitch=m_btnH=0;
    m_page=0;
    m_laidOutPage=-1;
    m_titleY=0;
    m_dragSlot=-1;
    m_dragTouch=-1;
    m_panel=m_content=CGRectMake(0,0,0,0);
    for(int k=0;k<SM_ROWS_PER_PAGE;k++) m_slot[k]=CGRectMake(0,0,0,0);
    m_keys=new KeybindsMenu();

	this->load();
}

SettingsMenu::~SettingsMenu(){
    delete m_keys;
}

// ---------------------------------------------------------------------------------------------
// pages
// ---------------------------------------------------------------------------------------------
// A group heading takes a SLOT of its own rather than being squeezed into the gap above a row:
// Stage 2.5 printed "Audio" between two rows in 11pt and it read as part of neither. Paging rule:
// every page starts with its group's heading (a continued group repeats it), and a heading is
// never the last slot on a page — it moves to the next one with its first row.
void SettingsMenu::build(){
    if(m_built) return;
    m_built=true;

    m_pages.clear();
    m_groups.clear();
    std::vector<Item> cur;
    int curGroup=-1;
    const int count=eden_settings_count();
    for(int i=0;i<count;i++){
        if(eden_settings_native_hidden(i)) continue;
        const char* g=eden_settings_group(i);
        if(m_groups.empty()||m_groups.back()!=g) m_groups.push_back(g);
        const int gi=(int)m_groups.size()-1;
        bool head=cur.empty()||gi!=curGroup;
        if((int)cur.size()+(head?2:1)>m_rowsPerPage){
            m_pages.push_back(cur);
            cur.clear();
            head=true;
        }
        if(head){ Item h={-1,gi}; cur.push_back(h); }
        Item it={i,gi};
        cur.push_back(it);
        curGroup=gi;
    }
    if(!cur.empty()) m_pages.push_back(cur);
    if(m_page>=(int)m_pages.size()) m_page=0;
    m_maxPageLen=0;
    for(size_t p=0;p<m_pages.size();p++)
        if((int)m_pages[p].size()>m_maxPageLen) m_maxPageLen=(int)m_pages[p].size();

    // Display face throughout (GLW::FACE_DISPLAY, the Label default) — Jersey 10 runs ~1.3x the
    // sans size for the same visual weight, hence the bigger numbers than Stage 5.4's.
    m_title.set("Settings", GLW::du(32), UITextAlignmentCenter);
    m_back.setLabel("Back", GLW::du(22));
    m_keysBtn.setLabel("Controls", GLW::du(22));
    m_prev.setLabel("<", GLW::du(22));
    m_next.setLabel(">", GLW::du(22));
    m_laidOutPage=-1;
}

// The density rule (2026-10-05, the user's look at 5.4: "too dense... massive switches and huge
// text-to-boundary padding"): rows are as tight as the pointer allows — 30u on a mouse, the 44pt
// touch floor on touch — and as many fit on a page as the screen has room for. A change in
// either (a display-profile switch, a window resize) re-pages.
void SettingsMenu::fit(){
    using namespace GLW;
    const float tf=touchFloor();
    m_pitch=std::max(du(30),tf);
    m_btnH=std::max(du(30),tf);
    const float margin=du(12), pad=du(12), gap=du(10);
    const float avail=SCREEN_HEIGHT-2.0f*margin-2.0f*pad-2.0f*(m_btnH+gap);
    int cap=(int)std::floor(avail/m_pitch);
    if(cap<3) cap=3;
    if(cap>SM_ROWS_PER_PAGE) cap=SM_ROWS_PER_PAGE;
    if(cap!=m_rowsPerPage){
        m_rowsPerPage=cap;
        m_built=false;
    }
}

void SettingsMenu::refreshValue(int k){
    if(m_page>=(int)m_pages.size()||k>=(int)m_pages[m_page].size()) return;
    const int si=m_pages[m_page][k].si;
    if(si<0) return;
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
        // LEFT-aligned beside the track, not right-aligned in a 56u box: Label aligns inside its
        // POT texture, which is wider than the box, so "right" printed past the row's edge.
        m_valueLabel[k].set(buf, GLW::du(20), UITextAlignmentLeft);
        break;
    }
    }
}

void SettingsMenu::buildPage(){
    m_laidOutPage=m_page;
    m_dragSlot=-1;
    m_dragTouch=-1;
    char buf[32];
    std::snprintf(buf,sizeof(buf),"%d / %d",m_page+1,(int)m_pages.size());
    m_pageLabel.set(buf, GLW::du(20), UITextAlignmentCenter);
    for(int k=0;k<SM_ROWS_PER_PAGE;k++){
        m_rowLabel[k].clear();
        m_valueLabel[k].clear();
        m_slider[k].endDrag();
        if(m_page>=(int)m_pages.size()||k>=(int)m_pages[m_page].size()) continue;
        const Item& it=m_pages[m_page][k];
        if(it.si<0){
            m_rowLabel[k].set(m_groups[it.group].c_str(), GLW::du(20), UITextAlignmentLeft);
            continue;
        }
        m_rowLabel[k].set(eden_settings_label(it.si), GLW::du(22), UITextAlignmentLeft);
        const int kind=eden_settings_kind(it.si);
        if(kind==SM_KIND_RANGE)
            m_slider[k].setRange(eden_settings_min(it.si),eden_settings_max(it.si),eden_settings_step(it.si));
        else if(kind==SM_KIND_ENUM)
            m_stepper[k].setGlyphs("<",">");
        refreshValue(k);
    }
}

void SettingsMenu::layout(){
    using namespace GLW;
    const float pad=du(12), gap=du(10);
    const float pages=(float)m_pages.size();

    // THE WINDOW SITS BEHIND THE CONTENT, it does not fill the screen: a column capped at the
    // mockups' measure, as tall as the longest page (so paging never resizes it), centred.
    // (Stage 5.4 drew a full-screen panel around a centred column — the user, 2026-10-05: "the
    // settings window edges should not fill the entire screen and just sit behind the content".)
    float cw=du(500);
    const float maxCw=SCREEN_WIDTH-du(12)*2.0f-pad*2.0f;
    if(cw>maxCw) cw=maxCw;
    const float pagerH=(pages>1)?m_btnH+gap:0.0f;
    const float ph=pad+m_btnH+gap+m_maxPageLen*m_pitch+pagerH+pad;
    const float pw=cw+pad*2.0f;
    m_panel=CGRectMake((SCREEN_WIDTH-pw)*0.5f,(SCREEN_HEIGHT-ph)*0.5f,pw,ph);
    m_content=CGRectMake(m_panel.origin.x+pad,m_panel.origin.y,cw,ph);
    const float left=m_content.origin.x, right=left+cw;

    // Titlebar (`.eden-titlebar`): Back at the left, the title centred on the WINDOW, actions at
    // the right — Controls opens the Stage 5.3 keybinds screen.
    const float barY=m_panel.origin.y+ph-pad-m_btnH;
    m_back.setRect(CGRectMake(left,barY,du(80),m_btnH));
    m_keysBtn.setRect(CGRectMake(right-du(104),barY,du(104),m_btnH));
    m_titleY=barY+(m_btnH+m_title.height())*0.5f;

    // Pager along the bottom: < [n / m] >.
    const float pagerY=m_panel.origin.y+pad;
    m_prev.setRect(CGRectMake(left,pagerY,du(40),m_btnH));
    m_next.setRect(CGRectMake(right-du(40),pagerY,du(40),m_btnH));

    // Rows. The CONTROLS stay compact at any profile; on touch only their hit boxes grow, to the
    // whole row slot (design-system.md: the floor raises the hit box, not the art).
    const float top=barY-gap;
    const float ch=std::min(m_pitch-du(8),std::max(du(22),touchFloor()*0.6f));
    const float cr=right-du(8);                      // controls' right edge, inside the strip
    const float sw=du(160);                          // slider / stepper width
    for(int k=0;k<SM_ROWS_PER_PAGE;k++){
        const float rowTop=top-k*m_pitch;
        m_slot[k]=CGRectMake(left,rowTop-m_pitch,cw,m_pitch);
        const float cy=rowTop-m_pitch*0.5f;
        const float slotY=rowTop-m_pitch;
        const float tw=ch*2.4f;                      // the split pill, at the CSS's ~3:1 squashed
        m_toggle[k].setRect(CGRectMake(cr-tw,cy-ch*0.5f,tw,ch));
        m_toggle[k].setHitRect(CGRectMake(cr-tw-du(8),slotY,tw+du(16),m_pitch));
        // Slider: track, then a left-aligned readout in the last 52u.
        const float sx=cr-du(52)-sw;
        m_slider[k].setRect(CGRectMake(sx,cy-ch*0.5f,sw,ch));
        m_slider[k].setHitRect(CGRectMake(sx-du(8),slotY,sw+du(16),m_pitch));
        m_stepper[k].setRect(CGRectMake(cr-sw,cy-ch*0.5f,sw,ch));
        m_stepper[k].setHitRect(CGRectMake(cr-sw,slotY,sw,m_pitch));
    }
}

// Rebuilds the page's widgets when the page changed, and once more when the settings table
// finishes loading — before that the values are defaults, and the rows stay hidden and inert
// (as Stage 2.5's did) while Back still works.
void SettingsMenu::syncPage(){
    if(!m_seeded&&eden_settings_loaded()){ m_seeded=true; m_laidOutPage=-1; }
    if(m_laidOutPage!=m_page) buildPage();
}

void SettingsMenu::commitSlider(int k){
    const int si=m_pages[m_page][k].si;
    eden_settings_set(si,m_slider[k].value());      // set() clamps + commits + side effects
    refreshValue(k);
}

void SettingsMenu::stepRow(int k,int dir){
    const int si=m_pages[m_page][k].si;
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
    refreshValue(k);
}

static const int usage_id=3;

void SettingsMenu::update(float etime){
    (void)etime;
    // The keybinds screen takes the whole frame while it is up — input included, which is what
    // keeps the two screens' touch slots (usage_id 3 here, 11 there) from ever contending.
    if(m_keys&&m_keys->active()){ m_keys->update(etime); return; }
    fit();
    build();
    syncPage();
    layout();

    Input* input=Input::getInput();
    itouch* touches=input->getTouches();
    const int pages=(int)m_pages.size();
    const std::vector<Item>& page=m_pages[m_page];
    const int nItems=(int)page.size();

    for(int i=0;i<MAX_TOUCHES;i++){
        if(touches[i].inuse==0&&touches[i].down==M_DOWN){
            touches[i].inuse=usage_id;
            const float mx=touches[i].mx, my=touches[i].my;
            m_back.setPressed(m_back.hit(mx,my));
            m_keysBtn.setPressed(m_keysBtn.hit(mx,my));
            m_prev.setPressed(m_page>0&&m_prev.hit(mx,my));
            m_next.setPressed(m_page<pages-1&&m_next.hit(mx,my));
            for(int k=0;k<nItems&&m_seeded;k++){
                const int si=page[k].si;
                if(si<0) continue;
                const int kind=eden_settings_kind(si);
                if(kind==SM_KIND_RANGE&&m_dragSlot<0&&m_slider[k].hit(mx,my)){
                    // The down edge already moves the thumb — a tap on the track is a jump there.
                    m_dragSlot=k; m_dragTouch=i;
                    if(m_slider[k].beginDrag(mx)) commitSlider(k);
                }else if(kind==SM_KIND_ENUM){
                    m_stepper[k].setPressed(m_stepper[k].hitDec(mx,my),m_stepper[k].hitInc(mx,my));
                }
            }
        }
        // Held: only the touch that grabbed a thumb drives it, and it keeps driving it when the
        // finger wanders off the track (vertically or past the ends), as a native slider does.
        if(touches[i].inuse==usage_id&&touches[i].down==M_DOWN&&i==m_dragTouch&&m_dragSlot>=0){
            if(m_slider[m_dragSlot].dragTo(touches[i].mx)) commitSlider(m_dragSlot);
        }
        if(touches[i].inuse==usage_id&&touches[i].down==M_RELEASE){
            const float mx=touches[i].mx, my=touches[i].my;
            touches[i].inuse=0;
            touches[i].down=M_NONE;
            m_back.setPressed(false); m_keysBtn.setPressed(false);
            m_prev.setPressed(false); m_next.setPressed(false);
            for(int k=0;k<SM_ROWS_PER_PAGE;k++) m_stepper[k].setPressed(false,false);

            if(i==m_dragTouch){
                if(m_dragSlot>=0){
                    if(m_slider[m_dragSlot].dragTo(mx)) commitSlider(m_dragSlot);
                    m_slider[m_dragSlot].endDrag();
                }
                m_dragSlot=-1; m_dragTouch=-1;
                continue;                     // a drag that ends over a button does not press it
            }
            if(m_back.hit(mx,my)){
                // save() + showsettings=FALSE, plus the two things a bare save() misses: it
                // re-applies the port settings load() stomps (invertcam, use_joystick — harmless
                // on the title screen, wrong in a world since Stage 5.5 opens this in-game) and
                // clears the touch table so the release does not land on the screen beneath.
                eden_settings_menu_close();
                return;
            }
            if(m_keysBtn.hit(mx,my)){
                if(m_keys) m_keys->show();
                return;                       // the child owns the rest of this frame
            }
            if(m_prev.hit(mx,my)){ if(m_page>0) m_page--; continue; }
            if(m_next.hit(mx,my)){ if(m_page<pages-1) m_page++; continue; }
            for(int k=0;k<nItems&&m_seeded;k++){
                const int si=page[k].si;
                if(si<0) continue;
                const int kind=eden_settings_kind(si);
                if(kind==SM_KIND_TOGGLE){
                    if(m_toggle[k].hit(mx,my)){ eden_settings_toggle(si); refreshValue(k); }
                }else if(kind==SM_KIND_ENUM){
                    if(m_stepper[k].hitDec(mx,my))      stepRow(k,-1);
                    else if(m_stepper[k].hitInc(mx,my)) stepRow(k,+1);
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
    if(m_keys) m_keys->show();
}

void SettingsMenu::resetView(){
    if(m_keys&&m_keys->active()) m_keys->hide();
    m_page=0;
    m_dragSlot=m_dragTouch=-1;
}

// --shot / --ui-selftest entry points (see the header). Both lay the page out immediately so a
// caller can read rects back without waiting a frame.
int SettingsMenu::pageCount(){
    fit();
    build();
    return (int)m_pages.size();
}

void SettingsMenu::showPage(int p){
    fit();
    build();
    if(p<0||p>=(int)m_pages.size()) return;
    m_page=p;
    syncPage();
    layout();
}

bool SettingsMenu::showRow(const char* key,CGRect* control){
    fit();
    build();
    if(!key) return false;
    for(int p=0;p<(int)m_pages.size();p++){
        for(int k=0;k<(int)m_pages[p].size();k++){
            const int si=m_pages[p][k].si;
            if(si<0||std::strcmp(eden_settings_key(si),key)!=0) continue;
            showPage(p);
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
    if(m_keys&&m_keys->active()){ m_keys->render(); return; }
    fit();
    build();
    syncPage();
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
    m_keysBtn.render();
    m_title.drawCentered(m_panel.origin.x+m_panel.size.width*0.5f, m_titleY, kText);

    const int pages=(int)m_pages.size();
    const std::vector<Item>& page=m_pages[m_page];
    for(int k=0;m_seeded&&k<(int)page.size()&&k<SM_ROWS_PER_PAGE;k++){
        const CGRect r=m_slot[k];
        const float cy=r.origin.y+r.size.height*0.5f;
        if(page[k].si<0){
            // A heading sits on the bottom of its slot, next to the rows it names.
            m_rowLabel[k].draw(r.origin.x+du(2), r.origin.y+du(4)+m_rowLabel[k].lineHeight(), kTextSecondary);
            continue;
        }
        // Contiguous CONTENT strips: one box per row, so a label and its control read as one
        // thing (KeybindsMenu.mm, "A SUNKEN content strip behind the whole row").
        bevel(CGRectMake(r.origin.x, r.origin.y, r.size.width, r.size.height), BEVEL_CONTENT);
        m_rowLabel[k].draw(r.origin.x+du(10), cy+m_rowLabel[k].lineHeight()*0.5f, kText);
        switch(eden_settings_kind(page[k].si)){
        case SM_KIND_TOGGLE: m_toggle[k].render(); break;
        case SM_KIND_ENUM:   m_stepper[k].render(); break;
        default:{
            m_slider[k].render();
            const CGRect sr=m_slider[k].rect();
            m_valueLabel[k].draw(sr.origin.x+sr.size.width+du(8), cy+m_valueLabel[k].lineHeight()*0.5f, kText);
            break;
        }
        }
    }

    if(pages>1){
        if(m_page>0)         m_prev.render();
        if(m_page<pages-1)   m_next.render();
        const CGRect pr=m_prev.rect();
        m_pageLabel.drawCentered(m_content.origin.x+m_content.size.width*0.5f,
                                 pr.origin.y+(pr.size.height+m_pageLabel.height())*0.5f, kTextSecondary);
    }

    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();

    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
}
