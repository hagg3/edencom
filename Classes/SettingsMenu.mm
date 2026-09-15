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
#import "Graphics.h"
#import "Globals.h"
#import "Util.h"
#import "World.h"

#include <cmath>
#include <cstdio>
#include <cstring>

// The shared settings table, as scalars (Settings_web.mm). Declared here rather than via a header
// to match the eden_menu_take_pending_world_height / eden_report_load_failure convention — a
// from-scratch iOS build supplies its own stub.
extern "C" {
    int         eden_settings_count(void);
    const char* eden_settings_label(int i);
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
// helpers
// ---------------------------------------------------------------------------------------------
// This port always runs with the "2x UI scale" flags (IS_IPAD/IS_RETINA true, SCALE_* == 2 —
// DisplayProfile_web.mm) on both web and native. Two consequences the drawing here has to respect:
//   * Texture2D::drawText multiplies the rect ORIGIN by SCALE_* but draws the texture at its raw
//     pixel size, so a label built at font `pt` renders `pt/SCALE` points tall — sm_text scales
//     the font up so it lands at `pt` POINTS.
//   * Graphics::drawRect draws in the RAW ortho (0..SCREEN_*SCALE), not point space — sm_fill
//     scales its point-space rect up to match.
// Also: pass POWER-OF-TWO w/h. initFromString rounds the buffer up to POT and centres/right-aligns
// the text within that POT buffer, but drawText only samples the requested sub-rect — a non-POT
// width silently clips right-aligned text and offsets centred text. Callers use POT dims and draw
// the whole quad.
static float sm_scale() { return IS_IPAD ? SCALE_WIDTH : 1.0f; }

static Texture2D* sm_text(const char* s, int w, int h, UITextAlignment align, float pt) {
    return new Texture2D([NSString stringWithUTF8String:(s ? s : "")],
                         CGSizeMake(w, h), align, [UIFont systemFontOfSize:pt * sm_scale()]);
}

static void sm_fill(CGRect r, float cr, float cg, float cb, float ca) {
    const float s = sm_scale();
    glDisable(GL_TEXTURE_2D);
    glColor4f(cr, cg, cb, ca);
    Graphics::drawRect(r.origin.x * s, r.origin.y * s,
                       (r.origin.x + r.size.width) * s, (r.origin.y + r.size.height) * s);
    glEnable(GL_TEXTURE_2D);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
}

// Draw a `wpx`x`hpx` text texture at point-space (px,py). drawText scales the ORIGIN by SCALE but
// not the extent, so the origin that lands the texture where we want is computed here: `center`
// puts the texture's middle at (px,py); otherwise (px,py) is its top-left in point space (still
// vertically centred on py for row use). left-anchored text passes center=false.
static void sm_blit(Texture2D* t, int wpx, int hpx, float px, float py, bool center) {
    if (!t) return;
    const float s = sm_scale();
    float ox = center ? (px - wpx / (2.0f * s)) : px;
    float oy = py - hpx / (2.0f * s);
    t->drawText(CGRectMake(ox, oy, wpx, hpx));
}

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

    m_page=0;
    m_rowsPerPage=SM_ROWS_PER_PAGE;
    m_built=false;
    m_seeded=false;
    for(int i=0;i<6;i++) m_glyph[i]=NULL;

	this->load();
}

SettingsMenu::~SettingsMenu(){
    for(size_t i=0;i<m_label.size();i++)    if(m_label[i])    delete m_label[i];
    for(size_t i=0;i<m_value.size();i++)    if(m_value[i])    delete m_value[i];
    for(size_t i=0;i<m_groupHdr.size();i++) if(m_groupHdr[i]) delete m_groupHdr[i];
    for(int i=0;i<6;i++) if(m_glyph[i]) delete m_glyph[i];
}

void SettingsMenu::rebuildValueTex(int si){
    if((size_t)si>=m_value.size()) return;
    if(m_value[si]){ delete m_value[si]; m_value[si]=NULL; }
    int kind=eden_settings_kind(si);
    char buf[64];
    if(kind==SM_KIND_ENUM){
        int idx=(int)lroundf(eden_settings_get(si));
        int n=eden_settings_enum_count(si);
        if(idx<0) idx=0; if(n>0&&idx>=n) idx=n-1;
        std::snprintf(buf,sizeof(buf),"%s",eden_settings_enum_label(si,idx));
    }else{
        float v=eden_settings_get(si);
        float st=eden_settings_step(si);
        std::snprintf(buf,sizeof(buf), (st>=1.0f?"%.0f":"%.2f"), v);
    }
    m_value[si]=sm_text(buf,128,32,UITextAlignmentCenter,15);
}

void SettingsMenu::buildTextures(){
    if(m_built) return;
    m_built=true;

    int count=eden_settings_count();
    m_label.assign(count,NULL);
    m_value.assign(count,NULL);
    m_groupHdr.assign(count,NULL);
    m_vis.clear();

    const char* prevGroup="";
    for(int i=0;i<count;i++){
        if(eden_settings_native_hidden(i)) continue;
        m_vis.push_back(i);
        m_label[i]=sm_text(eden_settings_label(i),512,32,UITextAlignmentLeft,14);
        const char* g=eden_settings_group(i);
        if(std::strcmp(g,prevGroup)!=0){
            m_groupHdr[i]=sm_text(g,512,32,UITextAlignmentLeft,11);
            prevGroup=g;
        }
        if(eden_settings_kind(i)!=SM_KIND_TOGGLE) rebuildValueTex(i);
    }

    m_glyph[0]=sm_text("-", 32,32,UITextAlignmentCenter,20);
    m_glyph[1]=sm_text("+", 32,32,UITextAlignmentCenter,20);
    m_glyph[2]=sm_text("<", 32,32,UITextAlignmentCenter,18);
    m_glyph[3]=sm_text(">", 32,32,UITextAlignmentCenter,18);
    m_glyph[4]=sm_text("On", 64,32,UITextAlignmentCenter,15);
    m_glyph[5]=sm_text("Off",64,32,UITextAlignmentCenter,15);
}

void SettingsMenu::layout(){
    float w=SCREEN_WIDTH, h=SCREEN_HEIGHT;

    rect_settings.size.width=246;
    rect_settings.size.height=45;
    rect_settings.origin.x=w/2-rect_settings.size.width/2;
    rect_settings.origin.y=h-rect_settings.size.height-3;

    rect_save.size.width=104; rect_save.size.height=32;
    rect_save.origin.x=w/2-rect_save.size.width/2; rect_save.origin.y=14;
    rect_prev=ButtonMake(18,14,86,32);
    rect_next=ButtonMake(w-18-86,14,86,32);

    float top=h-46.0f;          // just below the "Options" header
    float bottom=56.0f;         // just above the button row
    float pitch=(top-bottom)/m_rowsPerPage;
    if(pitch>52.0f) pitch=52.0f;

    const int first=m_page*m_rowsPerPage;
    const int visN=(int)m_vis.size();
    for(int k=0;k<m_rowsPerPage;k++){
        float rTop=top-(k+1)*pitch;
        float cy=rTop+pitch*0.5f;
        float right=w-22.0f;
        int vi=first+k;
        int kind=(vi<visN)? eden_settings_kind(m_vis[vi]) : -1;
        if(kind==SM_KIND_TOGGLE){
            m_ctlA[k]=ButtonMake(right-84.0f, cy-14.0f, 84.0f, 28.0f);   // ON/OFF button
            m_ctlB[k]=ButtonMake(0,0,0,0);
        }else{
            // "<"/"-"  [ value ]  ">"/"+"  — non-overlapping, value drawn in the 120px gap
            m_ctlB[k]=ButtonMake(right-30.0f,           cy-14.0f, 30.0f, 28.0f);
            m_ctlA[k]=ButtonMake(right-30.0f-120.0f-30.0f, cy-14.0f, 30.0f, 28.0f);
        }
    }
}

void SettingsMenu::stepRow(int si,int dir){
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
    rebuildValueTex(si);
}

static const int usage_id=3;

void SettingsMenu::refreshSeededValues(){
    if(m_seeded || !eden_settings_loaded()) return;
    m_seeded=true;
    for(size_t k=0;k<m_vis.size();k++)
        if(eden_settings_kind(m_vis[k])!=SM_KIND_TOGGLE) rebuildValueTex(m_vis[k]);
}

void SettingsMenu::update(float etime){
    (void)etime;
    buildTextures();
    refreshSeededValues();
    layout();

    Input* input=Input::getInput();
    itouch* touches=input->getTouches();

    const int first=m_page*m_rowsPerPage;
    const int visN=(int)m_vis.size();

    for(int i=0;i<MAX_TOUCHES;i++){
        if(touches[i].inuse==0&&touches[i].down==M_DOWN){
            touches[i].inuse=usage_id;
            inbox3(touches[i].mx,touches[i].my,&rect_save);
            inbox3(touches[i].mx,touches[i].my,&rect_prev);
            inbox3(touches[i].mx,touches[i].my,&rect_next);
            for(int k=0;k<m_rowsPerPage;k++){
                inbox3(touches[i].mx,touches[i].my,&m_ctlA[k]);
                inbox3(touches[i].mx,touches[i].my,&m_ctlB[k]);
            }
        }
        if(touches[i].inuse==usage_id&&touches[i].down==M_RELEASE){
            if(inbox2(touches[i].mx,touches[i].my,&rect_save)){
                this->save();
                World::getWorld->menu->showsettings=FALSE;
            }else if(inbox2(touches[i].mx,touches[i].my,&rect_prev)){
                if(m_page>0) m_page--;
            }else if(inbox2(touches[i].mx,touches[i].my,&rect_next)){
                if((m_page+1)*m_rowsPerPage<visN) m_page++;
            }else if(eden_settings_loaded()){
                for(int k=0;k<m_rowsPerPage;k++){
                    int vi=first+k;
                    if(vi>=visN) break;
                    int si=m_vis[vi];
                    int kind=eden_settings_kind(si);
                    if(kind==SM_KIND_TOGGLE){
                        if(inbox2(touches[i].mx,touches[i].my,&m_ctlA[k]))
                            eden_settings_toggle(si);
                    }else{
                        if(inbox2(touches[i].mx,touches[i].my,&m_ctlA[k])) stepRow(si,-1);
                        else if(inbox2(touches[i].mx,touches[i].my,&m_ctlB[k])) stepRow(si,+1);
                    }
                }
            }
            rect_save.pressed=FALSE; rect_prev.pressed=FALSE; rect_next.pressed=FALSE;
            for(int k=0;k<m_rowsPerPage;k++){ m_ctlA[k].pressed=FALSE; m_ctlB[k].pressed=FALSE; }
            touches[i].inuse=0;
            touches[i].down=M_NONE;
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
NSString* SettingsMenu::getNewWorldName(){
	world_counter++;

	NSUserDefaults *prefs = [NSUserDefaults standardUserDefaults];
	[prefs setObject:[NSNumber numberWithInt:world_counter] forKey:@"new_world_counter"];
	[prefs synchronize];
	return [NSString stringWithFormat:@"World %d",world_counter];
}

void SettingsMenu::render(){
    buildTextures();
    refreshSeededValues();
    layout();

    // Dark backing so the list stays legible over the animated menu art.
    sm_fill(CGRectMake(8, 52, SCREEN_WIDTH-16, SCREEN_HEIGHT-52-40), 0.06f, 0.06f, 0.09f, 0.93f);

	glColor4f(1.0, 1.0, 1.0, 1.0f);
	Resources::getResources->getMenuTex(MENU_OPTIONS_HEADER)->drawText(rect_settings);

    const int visN=(int)m_vis.size();
    const int pages=(visN+m_rowsPerPage-1)/m_rowsPerPage;

    // button row
    glColor4f(1,1,1,1);
    Resources::getResources->getMenuTex(MENU_SAVE)->drawButton(rect_save);
    if(m_page>0){
        sm_fill(RectFromButton(rect_prev),0.24f,0.26f,0.32f,1.0f);
        sm_blit(m_glyph[2],32,32, rect_prev.origin.x+rect_prev.size.width/2, rect_prev.origin.y+rect_prev.size.height/2, true);
    }
    if(m_page<pages-1){
        sm_fill(RectFromButton(rect_next),0.24f,0.26f,0.32f,1.0f);
        sm_blit(m_glyph[3],32,32, rect_next.origin.x+rect_next.size.width/2, rect_next.origin.y+rect_next.size.height/2, true);
    }

    if(!eden_settings_loaded()) return;

    const int first=m_page*m_rowsPerPage;
    for(int k=0;k<m_rowsPerPage;k++){
        int vi=first+k;
        if(vi>=visN) break;
        int si=m_vis[vi];
        CGRect a=RectFromButton(m_ctlA[k]);
        float cy=a.origin.y+a.size.height*0.5f;

        // Group heading in the gap above the row (skip the top row — no room there).
        if(k>0 && m_groupHdr[si]){
            glColor4f(0.60f,0.66f,0.82f,1.0f);
            sm_blit(m_groupHdr[si],512,32, 24, a.origin.y+a.size.height+9, false);
            glColor4f(1,1,1,1);
        }

        glColor4f(0.95f,0.95f,0.95f,1.0f);
        sm_blit(m_label[si],512,32, 26, cy, false);
        glColor4f(1,1,1,1);

        int kind=eden_settings_kind(si);
        if(kind==SM_KIND_TOGGLE){
            bool on=eden_settings_get(si)!=0.0f;
            if(on) sm_fill(a,0.20f,0.42f,0.26f,1.0f);
            else   sm_fill(a,0.22f,0.22f,0.26f,1.0f);
            sm_blit(m_glyph[on?4:5],64,32, a.origin.x+a.size.width*0.5f, cy, true);
        }else{
            CGRect b=RectFromButton(m_ctlB[k]);
            sm_fill(a,0.24f,0.26f,0.32f,1.0f);
            sm_fill(b,0.24f,0.26f,0.32f,1.0f);
            sm_blit(m_glyph[kind==SM_KIND_ENUM?2:0],32,32, a.origin.x+a.size.width*0.5f, cy, true);
            sm_blit(m_glyph[kind==SM_KIND_ENUM?3:1],32,32, b.origin.x+b.size.width*0.5f, cy, true);
            if((size_t)si<m_value.size())
                sm_blit(m_value[si],128,32, (a.origin.x+a.size.width + b.origin.x)*0.5f, cy, true);
        }
    }
}
