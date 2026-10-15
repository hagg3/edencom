//
//  NewWorldMenu.mm
//  Eden
//
//  Stage U.5: the New World screen. See NewWorldMenu.h for what it replaced and how Stage H
//  extends it; the shape (fixed RailFrame panel, IconRail, TwoLineRows in a variable-height
//  ScrollView, the usage_id touch claim) is SettingsMenu.mm's, which is the one to compare against.
//

#import "NewWorldMenu.h"
#import "Graphics.h"
#import "Globals.h"
#import "Input.h"
#import "Menu.h"
#import "World.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

extern float SCREEN_WIDTH;
extern float SCREEN_HEIGHT;

// ---------------------------------------------------------------------------------------------
// The tables. A world TYPE is a rail tile; an OPTION is a row. `types` is a bitmask over kTypes
// of the types a row applies to, so a generator-only option (a seed) can skip Flat.
// ---------------------------------------------------------------------------------------------
struct NWType { const char* title; int icon; int flat; const char* shot; };
static const NWType kTypes[]={
    // FileManager::genflat is the only switch today: exactly two generators (plan U.5).
    { "Flat",   UIICON_SQUARE,   1, "flat"   },
    { "Normal", UIICON_MOUNTAIN, 0, "normal" },
};
enum { NW_TYPES=(int)(sizeof(kTypes)/sizeof(kTypes[0])), NW_ALL_TYPES=(1<<NW_TYPES)-1 };
// The first visit opens on Normal, the landscape players expect; later visits keep the last pick.
enum { NW_DEFAULT_TYPE=1 };

enum { NW_TEXT=0, NW_CHOICE=1 };
struct NWOption { const char* label; int value; };
struct NWRow {
    const char* key; const char* title; const char* hint;
    int kind;
    const NWOption* opts; int nOpts;   // NW_CHOICE: option 0 is the default
    int maxBytes;                      // NW_TEXT
    unsigned types;
};
// S.5e (the user, 2026-10-10): New Dawn 256 is the default height, so it is first.
static const NWOption kHeights[]={ { "New Dawn 256", 256 }, { "Classic 64", 64 } };
static const NWRow kRows[]={
    // 48, not WorldFileHeader::name's 50: FileManager's header writer copies the display name
    // with getCString:maxLength:49, which counts the terminator and fails outright on a longer one.
    { "name",   "World name", "Shown in the world list.",                        NW_TEXT,   NULL,     0, 48, NW_ALL_TYPES },
    { "height", "Height",     "Classic is the original game's; New Dawn is 4x taller.", NW_CHOICE, kHeights, 2, 0,  NW_ALL_TYPES },
};
enum { NW_ROWS=(int)(sizeof(kRows)/sizeof(kRows[0])) };

// The pending-choice seam (port/src/seam/Menu_web.mm): one-shot values that showAlertWorldType()
// and FileManager::probeWorldHeight() take instead of asking. Declared here, as SettingsMenu.mm
// declares the settings accessors.
extern "C" {
    void eden_menu_set_pending_world_type(int flat);
    void eden_menu_set_pending_world_height(int height);
}

static const int usage_id=13;   // distinct from Menu (7), SettingsMenu (3), GLDialog (9), Keybinds (11), WorldBrowser (12)

// ---------------------------------------------------------------------------------------------
NewWorldMenu::NewWorldMenu(){
    m_open=false;
    m_type=NW_DEFAULT_TYPE;
    m_builtType=-1;
    m_builtU=0;
    m_heldRow=m_heldOpt=-1;
    m_nRows=0;
    m_titleY=0;
    m_scrollTouch=m_railTouch=-1;
    m_barDrag=false;
    m_panel=m_content=CGRectMake(0,0,0,0);
    for(int r=0;r<NW_MAX_ROWS;r++){ m_choice[r]=0; m_rowIdx[r]=-1; }
    int icons[NW_TYPES];
    for(int t=0;t<NW_TYPES;t++) icons[t]=kTypes[t].icon;
    m_rail.setTiles(icons,NW_TYPES);
    for(int r=0;r<NW_ROWS&&r<NW_MAX_ROWS;r++)
        if(kRows[r].kind==NW_TEXT) m_field[r].setMaxBytes(kRows[r].maxBytes);
}

NewWorldMenu::~NewWorldMenu(){ blurFields(); }

int NewWorldMenu::rowOf(const char* key) const{
    if(!key) return -1;
    for(int r=0;r<NW_ROWS;r++) if(!std::strcmp(kRows[r].key,key)) return r;
    return -1;
}

void NewWorldMenu::blurFields(){
    for(int r=0;r<NW_ROWS;r++) if(kRows[r].kind==NW_TEXT) m_field[r].blur();
}

// The name is fresh on every visit, the type and height are kept: someone making a second world
// usually wants the same kind again. The placeholder is the name a blank field becomes — peeked,
// not taken, so Back does not burn a "World N".
void NewWorldMenu::open(){
    m_open=true;
    for(int r=0;r<NW_ROWS;r++) if(kRows[r].kind==NW_TEXT) m_field[r].setText("");
    m_autoName.clear();
    Menu* menu=World::getWorld?World::getWorld->menu:NULL;
    if(menu&&menu->settings){
        char buf[32];
        std::snprintf(buf,sizeof(buf),"World %d",menu->settings->world_counter+1);
        m_autoName=buf;
    }
    const int nr=rowOf("name");
    if(nr>=0) m_field[nr].setPlaceholder(m_autoName.c_str());
    m_scroll.setFirst(0);
    m_scrollTouch=m_railTouch=-1;
    m_barDrag=false;
    m_heldRow=m_heldOpt=-1;
    m_builtType=-1;
    layout();
    // With a keyboard the name is one keystroke away; on touch, raising the system keyboard
    // unasked would cover the screen before the player has seen it.
    if(nr>=0&&GLW::touchFloor()==0.0f) m_field[nr].focus();
}

void NewWorldMenu::close(){
    blurFields();
    m_open=false;
    // As eden_settings_menu_close(): Menu::update returned early for every frame this screen was
    // up, so a touch live when it opened may still sit in the table with a stale `inuse`.
    Input::getInput()->clearAll();
}

void NewWorldMenu::back(){ close(); }

void NewWorldMenu::selectType(int t){
    if(t<0||t>=NW_TYPES||t==m_type) return;
    m_type=t;
    m_scroll.setFirst(0);
    m_builtType=-1;
}

// ---------------------------------------------------------------------------------------------
// layout
// ---------------------------------------------------------------------------------------------
void NewWorldMenu::buildLabels(){
    using namespace GLW;
    m_builtType=m_type;
    m_builtU=u();
    char buf[64];
    std::snprintf(buf,sizeof(buf),"New %s World",kTypes[m_type].title);
    m_title.set(buf,du(32),UITextAlignmentCenter);
    m_back.setLabel("Back",du(20));
    m_back.sizeToFit();
    m_play.setLabel("Play",du(20));
    m_play.setIcon(UIICON_PLAY);
    m_play.setTone(GLW::Button::TONE_POSITIVE);
    m_play.sizeToFit();
    m_nRows=0;
    for(int r=0;r<NW_ROWS&&m_nRows<NW_MAX_ROWS;r++){
        if(!(kRows[r].types&(1u<<m_type))) continue;
        m_rowIdx[m_nRows++]=r;
        if(kRows[r].kind==NW_TEXT) m_field[r].setPointSize(du(20));
        for(int j=0;j<kRows[r].nOpts&&j<NW_MAX_OPTS;j++){
            m_opt[r][j].setLabel(kRows[r].opts[j].label,du(20));
            m_opt[r][j].sizeToFit();
        }
    }
}

void NewWorldMenu::controlSize(int r,float* w,float* h) const{
    using namespace GLW;
    const NWRow& R=kRows[r];
    if(R.kind==NW_TEXT){ *w=du(320); *h=du(34); return; }
    float cw=0;
    for(int j=0;j<R.nOpts&&j<NW_MAX_OPTS;j++) cw+=m_opt[r][j].fitWidth()+(j?du(5):0.0f);
    *w=cw; *h=du(29);
}

bool NewWorldMenu::rowShown(int k) const{
    return k>=m_scroll.first()&&k<m_scroll.first()+m_scroll.visible()&&k<m_nRows;
}

void NewWorldMenu::layout(){
    using namespace GLW;
    if(m_builtType!=m_type||m_builtU!=u()) buildLabels();
    const RailFrame f=railFrame();
    m_panel=f.panel;
    const CGRect& T=f.titlebar;
    m_back.setRect(CGRectMake(T.origin.x,T.origin.y,0,T.size.height));
    titlebarHit(m_back);
    m_play.setRect(CGRectMake(T.origin.x+T.size.width-m_play.fitWidth(),T.origin.y,0,T.size.height));
    titlebarHit(m_play);
    m_titleY=f.titleY(m_title);
    m_rail.setRect(f.rail);
    m_rail.setSelected(m_type);
    m_scroll.setBarRect(f.bar);
    m_content=f.content;
    const CGRect& C=m_content;

    float heights[NW_MAX_ROWS];
    for(int k=0;k<m_nRows;k++){
        const int r=m_rowIdx[k];
        float cw,ch;
        controlSize(r,&cw,&ch);
        m_row[k].build(kRows[r].title,kRows[r].hint,C.size.width,cw,ch);
        m_row[k].setLast(k==m_nRows-1);
        heights[k]=m_row[k].height();
    }
    m_scroll.setRowHeights(heights,m_nRows,C.size.height);

    for(int k=0;k<m_nRows;k++){
        if(!rowShown(k)) continue;
        const int r=m_rowIdx[k];
        m_row[k].setOrigin(C.origin.x,C.origin.y+C.size.height-m_scroll.rowOffset(k));
        float cw,ch;
        controlSize(r,&cw,&ch);
        const CGRect cr=m_row[k].controlRect(cw,ch);
        if(kRows[r].kind==NW_TEXT){
            m_field[r].setRect(cr);
            m_field[r].setKeepVisible(m_panel);   // the iOS keyboard leaves Play and the rows visible
        }else{
            float x=cr.origin.x;
            for(int j=0;j<kRows[r].nOpts&&j<NW_MAX_OPTS;j++){
                m_opt[r][j].setRect(CGRectMake(x,cr.origin.y,0,cr.size.height));
                x+=m_opt[r][j].fitWidth()+du(5);
            }
        }
    }
}

// ---------------------------------------------------------------------------------------------
// input
// ---------------------------------------------------------------------------------------------
// Creates the list entry and starts the load. The type and height go through the pending seam,
// so Menu::render's loading==2 branch finds them and raises no dialog.
void NewWorldMenu::play(){
    Menu* menu=World::getWorld?World::getWorld->menu:NULL;
    if(!menu||menu->loading) return;
    const int nr=rowOf("name");
    std::string name=nr>=0?m_field[nr].text():std::string();
    const size_t a=name.find_first_not_of(" \t"), b=name.find_last_not_of(" \t");
    name=(a==std::string::npos)?std::string():name.substr(a,b-a+1);
    const int hr=rowOf("height");
    const int height=hr>=0?kRows[hr].opts[m_choice[hr]].value:256;
    const int flat=kTypes[m_type].flat;
    close();
    // A blank name takes the engine's own "World N" (and advances its counter), as stock New did.
    NSString* ns=name.empty()?menu->settings->getNewWorldName():[NSString stringWithUTF8String:name.c_str()];
    if(!ns) ns=menu->settings->getNewWorldName();   // not valid UTF-8 (cannot happen: the field keeps whole chars)
    eden_menu_set_pending_world_type(flat);
    eden_menu_set_pending_world_height(height);
    menu->startNewWorld(ns);
}

void NewWorldMenu::update(float etime){
    (void)etime;
    if(!m_open) return;
    layout();
    using namespace GLW;

    for(int r=0;r<NW_ROWS;r++){
        if(kRows[r].kind!=NW_TEXT) continue;
        // Return in the name field is Play: the form's default action. Escape only leaves the field.
        if(m_field[r].update()==TextField::EV_COMMIT){ play(); return; }
    }
    if(int wheel=eden_ui_take_wheel()) m_scroll.scrollBy(-wheel);

    Input* input=Input::getInput();
    itouch* touches=input->getTouches();
    const bool touch=touchFloor()>0.0f;
    for(int i=0;i<MAX_TOUCHES;i++){
        if(touches[i].inuse==0&&touches[i].down==M_DOWN){
            touches[i].inuse=usage_id;
            const float mx=touches[i].mx, my=touches[i].my;
            m_back.setPressed(m_back.hit(mx,my));
            m_play.setPressed(m_play.hit(mx,my));
            if(m_rail.hitRail(mx,my)&&m_railTouch<0){
                m_railTouch=i;
                m_rail.setHeld(m_rail.hit(mx,my));
                m_rail.beginDrag(my);
                continue;
            }
            if(m_scroll.hitBar(mx,my)&&m_scrollTouch<0){
                m_scrollTouch=i; m_barDrag=true;
                m_scroll.barBegin(my);
                continue;
            }
            for(int k=0;k<m_nRows;k++){
                if(!rowShown(k)) continue;
                const int r=m_rowIdx[k];
                for(int j=0;j<kRows[r].nOpts&&j<NW_MAX_OPTS;j++)
                    if(m_opt[r][j].hit(mx,my)){ m_heldRow=r; m_heldOpt=j; }
            }
            if(m_scrollTouch<0&&mx>=m_content.origin.x&&mx<=m_content.origin.x+m_content.size.width&&
               my>=m_content.origin.y&&my<=m_content.origin.y+m_content.size.height){
                m_scrollTouch=i; m_barDrag=false;
                m_scroll.beginContentDrag(my);
            }
        }
        if(touches[i].inuse==usage_id&&touches[i].down==M_DOWN){
            const float my=touches[i].my;
            if(i==m_scrollTouch){
                if(m_barDrag) m_scroll.barDragTo(my);
                else if(m_scroll.contentDragTo(my)) m_heldRow=m_heldOpt=-1;
            }
            if(i==m_railTouch&&m_rail.dragTo(my)) m_rail.setHeld(-1);
        }
        if(touches[i].inuse==usage_id&&touches[i].down==M_RELEASE){
            const float mx=touches[i].mx, my=touches[i].my;
            touches[i].inuse=0;
            touches[i].down=M_NONE;
            const bool backHit=m_back.pressed()&&m_back.hit(mx,my);
            const bool playHit=m_play.pressed()&&m_play.hit(mx,my);
            m_back.setPressed(false);
            m_play.setPressed(false);
            const int heldRow=m_heldRow, heldOpt=m_heldOpt;
            m_heldRow=m_heldOpt=-1;
            // A tap on a text row focuses its field — on touch anywhere on the row (the floor raises
            // the hit box, not the art) — and any other release leaves it, as a desktop form does.
            int focus=-1;
            for(int k=0;k<m_nRows;k++){
                if(!rowShown(k)) continue;
                const int r=m_rowIdx[k];
                if(kRows[r].kind==NW_TEXT&&(m_field[r].hit(mx,my)||(touch&&m_row[k].hit(mx,my)))) focus=r;
            }
            if(focus<0) blurFields();

            if(i==m_railTouch){
                const bool scrolled=m_rail.scrolling();
                m_rail.endDrag();
                m_rail.setHeld(-1);
                m_railTouch=-1;
                if(scrolled) continue;
                const int t=m_rail.hit(mx,my);
                if(t>=0) selectType(t);
                continue;
            }
            if(i==m_scrollTouch){
                const bool scrolled=m_barDrag||m_scroll.scrolling();
                m_scroll.endDrag();
                m_scrollTouch=-1; m_barDrag=false;
                if(scrolled) continue;        // a scroll is not a tap on what it ended over
            }
            if(backHit){ close(); return; }
            if(playHit){ play(); return; }
            if(heldRow>=0&&m_opt[heldRow][heldOpt].hit(mx,my)){ m_choice[heldRow]=heldOpt; continue; }
            if(focus>=0) m_field[focus].focus();
        }
    }
}

// ---------------------------------------------------------------------------------------------
// render
// ---------------------------------------------------------------------------------------------
void NewWorldMenu::render(){
    if(!m_open) return;
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
    m_play.render();
    m_title.drawCentered(m_panel.origin.x+m_panel.size.width*0.5f, m_titleY, kText);
    m_rail.render();

    bevel(m_content, BEVEL_CONTENT);
    for(int k=0;k<m_nRows;k++){
        if(!rowShown(k)) continue;
        const int r=m_rowIdx[k];
        m_row[k].render();
        if(kRows[r].kind==NW_TEXT){ m_field[r].render(); continue; }
        // A choice is a segmented pair: pressed == selected (design-system.md), and the held one.
        for(int j=0;j<kRows[r].nOpts&&j<NW_MAX_OPTS;j++){
            m_opt[r][j].setPressed(j==m_choice[r]||(r==m_heldRow&&j==m_heldOpt));
            m_opt[r][j].render();
        }
    }
    m_scroll.render();

    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();

    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
}

// ---------------------------------------------------------------------------------------------
// harness surface
// ---------------------------------------------------------------------------------------------
CGRect NewWorldMenu::controlRect(const char* which){
    layout();
    if(!which) return CGRectMake(0,0,0,0);
    if(!std::strcmp(which,"back")) return m_back.rect();
    if(!std::strcmp(which,"play")) return m_play.rect();
    const int r=rowOf(which);
    if(r>=0&&kRows[r].kind==NW_TEXT) return m_field[r].rect();
    return CGRectMake(0,0,0,0);
}

CGRect NewWorldMenu::typeRect(int t){ layout(); return m_rail.tileRect(t); }
int  NewWorldMenu::typeCount() const{ return NW_TYPES; }
bool NewWorldMenu::typeFlat(int t) const{ return t>=0&&t<NW_TYPES&&kTypes[t].flat; }
const char* NewWorldMenu::typeShot(int t) const{ return (t>=0&&t<NW_TYPES)?kTypes[t].shot:""; }
void NewWorldMenu::showType(int t){ selectType(t); layout(); }

bool NewWorldMenu::choiceRect(const char* key,int option,CGRect* rect){
    const int r=rowOf(key);
    if(r<0||kRows[r].kind!=NW_CHOICE||option<0||option>=kRows[r].nOpts) return false;
    layout();
    for(int k=0;k<m_nRows;k++){
        if(m_rowIdx[k]!=r) continue;
        if(!rowShown(k)){ m_scroll.ensureVisible(k); layout(); }
        if(rect) *rect=m_opt[r][option].rect();
        return true;
    }
    return false;
}

int NewWorldMenu::choice(const char* key) const{
    const int r=rowOf(key);
    return (r>=0&&kRows[r].kind==NW_CHOICE)?kRows[r].opts[m_choice[r]].value:-1;
}

const char* NewWorldMenu::text(const char* key) const{
    const int r=rowOf(key);
    return (r>=0&&kRows[r].kind==NW_TEXT)?m_field[r].text().c_str():"";
}

bool NewWorldMenu::focused(const char* key) const{
    const int r=rowOf(key);
    return r>=0&&kRows[r].kind==NW_TEXT&&m_field[r].focused();
}
