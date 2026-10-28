//
//  NewWorldMenu.mm
//  Eden
//
//  Stage U.5: the New World screen. See NewWorldMenu.h for what it replaced and how Stage H
//  extends it; the shape (fixed RailFrame panel, IconRail, TwoLineRows in a variable-height
//  ScrollView, the usage_id touch claim) is SettingsMenu.mm's, which is the one to compare against.
//

#include "NewWorldMenu.h"
#include "Graphics.h"
#include "Globals.h"
#include "Input.h"
#include "Menu.h"
#include "World.h"
#include "Texture2D.h"
#include "TerrainGenerator.h"
#include "ClassicOptions.h"

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
    // FileManager::genflat picks Flat; Normal is the bundled map (plan U.5). H.4: Classic is the
    // seeded 1.x generator -- "normal" to the type seam, plus a GENERATOR record (play()).
    { "Flat",    UIICON_SQUARE,   1, "flat"    },
    { "Normal",  UIICON_MOUNTAIN, 0, "normal"  },
    { "Classic", UIICON_TREES,    0, "classic" },
};
enum { NW_TYPES=(int)(sizeof(kTypes)/sizeof(kTypes[0])), NW_ALL_TYPES=(1<<NW_TYPES)-1 };
enum { NW_CLASSIC_TYPE=2, NW_CLASSIC=1<<NW_CLASSIC_TYPE };
// The first visit opens on Normal, the landscape players expect; later visits keep the last pick.
enum { NW_DEFAULT_TYPE=1 };

// H.4 added four kinds: NW_SEED (a text field + the Random button), NW_SLIDER (a detented slider
// over `opts`), NW_TOGGLE and NW_PREVIEW (the Classic tab's top-down image).
enum { NW_TEXT=0, NW_CHOICE=1, NW_SEED=2, NW_SLIDER=3, NW_TOGGLE=4, NW_PREVIEW=5 };
struct NWRow {
    const char* key; const char* title; const char* hint;
    int kind;
    const NWOption* opts; int nOpts;   // NW_CHOICE / NW_SLIDER
    int maxBytes;                      // NW_TEXT / NW_SEED
    unsigned types;
    int def;                           // NW_CHOICE / NW_SLIDER: the default option; NW_TOGGLE: 0/1
};
// S.5e (the user, 2026-10-10): New Dawn 256 is the default height, so it is first. H.3 (the user,
// 2026-10-15): "Classic 64" is "Original 64" now that Classic names a generator.
static const NWOption kHeights[]={ { "New Dawn 256", 256 }, { "Original 64", 64 } };
// H.4's Classic option tables (Scale, Hilliness, Caves, Trees, Size) are in ClassicOptions.h since
// H.15, which shares them with World Properties.
static const NWRow kRows[]={
    // 48, not WorldFileHeader::name's 50: FileManager's header writer copies the display name
    // with getCString:maxLength:49, which counts the terminator and fails outright on a longer one.
    { "name",    "World name", "Shown in the world list.",                        NW_TEXT,    NULL,     0, 48, NW_ALL_TYPES, 0 },
    { "seed",    "Seed",       "A number, or any words. Blank keeps the one shown.", NW_SEED,   NULL,     0, 24, NW_CLASSIC,   0 },
    { "preview", "Preview",    "",                                                NW_PREVIEW, NULL,     0, 0,  NW_CLASSIC,   0 },
    { "height",  "Height",     "Original is the first game's; New Dawn is 4x taller.", NW_CHOICE, kHeights, 2, 0, NW_ALL_TYPES, 0 },
    { "scale",   "Scale",      "",                                                NW_SLIDER,  kScales,  5, 0,  NW_CLASSIC,   0 },
    { "hills",   "Hilliness",  "How far the land rises and falls.",               NW_CHOICE,  kHills,   5, 0,  NW_CLASSIC,   2 },
    { "caves",   "Caves",      "2011: the first caves, opening into hillsides. Deep: further down.", NW_CHOICE, kCaves, 3, 0, NW_CLASSIC, 0 },
    { "trees",   "Trees",      "How many trees grow.",                            NW_CHOICE,  kTrees,   4, 0,  NW_CLASSIC,   2 },
    { "flowers", "Flowers",    "The original blue flower patches.",               NW_TOGGLE,  NULL,     0, 0,  NW_CLASSIC,   1 },
    { "clouds",  "Clouds",     "Low cloud slabs over the land.",                  NW_TOGGLE,  NULL,     0, 0,  NW_CLASSIC,   1 },
    { "size",    "Size",       "Extended has no edge; 2880 square has the original map's wall.", NW_CHOICE, kSizes, 2, 0, NW_CLASSIC, 0 },
};
enum { NW_ROWS=(int)(sizeof(kRows)/sizeof(kRows[0])) };

// The pending-choice seam (port/src/seam/Menu.cpp): one-shot values that showAlertWorldType()
// and FileManager::probeWorldHeight() take instead of asking. Declared here, as SettingsMenu.mm
// declares the settings accessors.
extern "C" {
    void eden_menu_set_pending_world_type(int flat);
    void eden_menu_set_pending_world_height(int height);
    void eden_menu_set_pending_generator(const unsigned char* rec28);   // H.4: NULL = none
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
    m_previewRect=CGRectMake(0,0,0,0);
    m_sliderRow=m_sliderTouch=-1;
    m_diceHeld=false;
    m_randomSeed=1;
    m_preview=NULL;
    for(int i=0;i<4;i++) m_previewKey[i]=-1;
    for(int r=0;r<NW_MAX_ROWS;r++){ m_choice[r]=0; m_rowIdx[r]=-1; }
    int icons[NW_TYPES];
    for(int t=0;t<NW_TYPES;t++) icons[t]=kTypes[t].icon;
    m_rail.setTiles(icons,NW_TYPES);
    for(int r=0;r<NW_ROWS&&r<NW_MAX_ROWS;r++){
        const NWRow& R=kRows[r];
        if(R.kind==NW_TEXT||R.kind==NW_SEED) m_field[r].setMaxBytes(R.maxBytes);
        if(R.kind==NW_TEXT) m_field[r].setWorldNameOnly(true);   // FT.3
        if(R.kind==NW_CHOICE) m_choice[r]=R.def;
        if(R.kind==NW_TOGGLE) m_toggle[r].setOn(R.def!=0);
        if(R.kind==NW_SLIDER){ m_slider[r].setRange(0,(float)(R.nOpts-1),1); m_slider[r].setValue((float)R.def); }
    }
}

NewWorldMenu::~NewWorldMenu(){ blurFields(); delete m_preview; }

int NewWorldMenu::rowOf(const char* key) const{
    if(!key) return -1;
    for(int r=0;r<NW_ROWS;r++) if(!std::strcmp(kRows[r].key,key)) return r;
    return -1;
}

void NewWorldMenu::blurFields(){
    for(int r=0;r<NW_ROWS;r++) if(kRows[r].kind==NW_TEXT||kRows[r].kind==NW_SEED) m_field[r].blur();
}

// ---------------------------------------------------------------------------------------------
// H.4: the seed (generator-plan.md §5.1). Digits are the number; any other text is FNV-1a hashed.
// Either way it lands in [1, 2^24) -- the float noise keeps its finest octave below ~2^20 -- and
// never on 333333, which means "the bundled map".
// ---------------------------------------------------------------------------------------------
static int nw_reduceSeed(unsigned long long v){
    if(!(v>=1&&v<(1ull<<24))) v=v%((1ull<<24)-1)+1;
    if(v==333333) v++;
    return (int)v;
}
int NewWorldMenu::seedFromText(const char* utf8){
    std::string t=utf8?utf8:"";
    const size_t a=t.find_first_not_of(" \t"), b=t.find_last_not_of(" \t");
    if(a==std::string::npos) return 0;
    t=t.substr(a,b-a+1);
    bool digits=t.size()<=18;
    for(size_t i=0;i<t.size()&&digits;i++) digits=t[i]>='0'&&t[i]<='9';
    if(digits) return nw_reduceSeed(strtoull(t.c_str(),NULL,10));
    unsigned int h=2166136261u;
    for(size_t i=0;i<t.size();i++){ h^=(unsigned char)t[i]; h*=16777619u; }
    return nw_reduceSeed(h);
}
void NewWorldMenu::rollSeed(){
    m_randomSeed=nw_reduceSeed(((unsigned long long)arc4random()<<8)^arc4random());
    char buf[16];
    std::snprintf(buf,sizeof(buf),"%d",m_randomSeed);
    const int sr=rowOf("seed");
    if(sr>=0){ m_field[sr].setText(""); m_field[sr].setPlaceholder(buf); }
}
int NewWorldMenu::seed() const{
    const int sr=rowOf("seed");
    const int v=sr>=0?seedFromText(m_field[sr].text().c_str()):0;
    return v?v:m_randomSeed;
}
void NewWorldMenu::classicSettings(ClassicGen* g) const{
    classic_default_params(g,seed());
    const int hr=rowOf("height");
    const bool tall=hr<0||kRows[hr].opts[m_choice[hr]].value==256;
    const int sc=rowOf("scale");
    if(sc>=0&&tall) g->scale4=(unsigned char)kRows[sc].opts[(int)(m_slider[sc].value()+0.5f)].value;
    int r;
    if((r=rowOf("hills"))>=0) g->hilliness=(unsigned char)kRows[r].opts[m_choice[r]].value;
    if((r=rowOf("caves"))>=0) g->caves=(unsigned char)kRows[r].opts[m_choice[r]].value;
    if((r=rowOf("trees"))>=0) g->trees=(unsigned char)kRows[r].opts[m_choice[r]].value;
    if((r=rowOf("size"))>=0)  g->size=(unsigned char)kRows[r].opts[m_choice[r]].value;
    if((r=rowOf("flowers"))>=0) g->flowers=m_toggle[r].on()?1:0;
    if((r=rowOf("clouds"))>=0)  g->clouds=m_toggle[r].on()?1:0;
}

// The Classic preview (generator-plan.md §5.3): the heightmap only, 128x128 samples one every
// 4*s blocks around the middle of chunk 4096,4096 (where spawn is searched from), hillshaded in
// grass greens with height bands. Rebuilt only when the seed, scale, hilliness or height change:
// ~16k x 10 noise2 calls, a few ms on a desktop and ~15 ms on the A8X, once per change.
void NewWorldMenu::updatePreview(){
    if(m_type!=NW_CLASSIC_TYPE) return;
    ClassicGen g;
    classicSettings(&g);
    const int hr=rowOf("height");
    const int height=hr>=0?kRows[hr].opts[m_choice[hr]].value:256;
    const int key[4]={g.seed,g.scale4,g.hilliness,height};
    if(m_preview&&!std::memcmp(key,m_previewKey,sizeof(key))) return;
    std::memcpy(m_previewKey,key,sizeof(key));
    enum { N=128 };
    static float hgt[N][N];
    static unsigned char px[N*N*4];
    const int step=g.scale4;                     // 4 blocks x s
    const int c0=4096*CHUNK_SIZE+CHUNK_SIZE/2;
    for(int i=0;i<N;i++)
        for(int j=0;j<N;j++)
            hgt[i][j]=(float)classic_height(g,c0+(i-N/2)*step,c0+(j-N/2)*step,height);
    const float base=8.0f*g.scale4;              // 32*s, the heightmap's middle
    for(int j=0;j<N;j++){
        for(int i=0;i<N;i++){
            const float h=hgt[i][j];
            const float dx=hgt[i>0?i-1:i][j]-hgt[i<N-1?i+1:i][j];
            const float dz=hgt[i][j>0?j-1:j]-hgt[i][j<N-1?j+1:j];
            float shade=1.0f+(dx+dz)*0.9f/step;
            if(shade<0.55f) shade=0.55f;
            if(shade>1.35f) shade=1.35f;
            float t=(h-base)/base;                // -1 (low) .. +1 (high)
            if(t<-1) t=-1;
            if(t>1) t=1;
            float r,gg,b;
            if(t<0){ r=70+25*(1+t); gg=125+40*(1+t); b=45+20*(1+t); }      // lowland greens
            else if(t<0.6f){ r=95+80*t; gg=165-10*t; b=65+60*t; }          // uplands, paler
            else{ float u=(t-0.6f)/0.4f; r=143+80*u; gg=159+60*u; b=101+110*u; }   // peaks
            unsigned char* p=&px[((size_t)j*N+i)*4];
            p[0]=(unsigned char)std::min(255.0f,r*shade);
            p[1]=(unsigned char)std::min(255.0f,gg*shade);
            p[2]=(unsigned char)std::min(255.0f,b*shade);
            p[3]=255;
        }
    }
    for(int d=-3;d<=3;d++){                      // spawn: a small white cross at the centre
        unsigned char* a=&px[((size_t)(N/2)*N+N/2+d)*4];
        unsigned char* b=&px[((size_t)(N/2+d)*N+N/2)*4];
        a[0]=a[1]=a[2]=255; b[0]=b[1]=b[2]=255;
    }
    delete m_preview;
    m_preview=new Texture2D(px,kTexture2DPixelFormat_RGBA8888,N,N,CGSizeMake(N,N));
}

const char* NewWorldMenu::hintFor(int r){
    const NWRow& R=kRows[r];
    char buf[160];
    if(!std::strcmp(R.key,"scale")){
        const int v=(int)(m_slider[r].value()+0.5f);
        std::snprintf(buf,sizeof(buf),"%s: the landscape %s. New Dawn 256 only.",R.opts[v].label,
                      v==0?"of the original game":"taller and wider");
        m_hint[r]=buf;
        return m_hint[r].c_str();
    }
    if(R.kind==NW_PREVIEW){
        std::snprintf(buf,sizeof(buf),"Seed %d, from above. The cross is where you start.",seed());
        m_hint[r]=buf;
        return m_hint[r].c_str();
    }
    return R.hint;
}

// The name is fresh on every visit, the type and height are kept: someone making a second world
// usually wants the same kind again. The placeholder is the name a blank field becomes — peeked,
// not taken, so Back does not burn a "World N".
void NewWorldMenu::open(){
    m_open=true;
    for(int r=0;r<NW_ROWS;r++) if(kRows[r].kind==NW_TEXT||kRows[r].kind==NW_SEED) m_field[r].setText("");
    rollSeed();                          // H.4: a fresh seed per visit (shown as the placeholder)
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
    m_sliderRow=m_sliderTouch=-1;
    m_diceHeld=false;
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
    m_dice.setLabel("Random",du(20));
    m_dice.sizeToFit();
    m_nRows=0;
    const int hr=rowOf("height");
    const bool tall=hr<0||kRows[hr].opts[m_choice[hr]].value==256;
    for(int r=0;r<NW_ROWS&&m_nRows<NW_MAX_ROWS;r++){
        if(!(kRows[r].types&(1u<<m_type))) continue;
        if(!std::strcmp(kRows[r].key,"scale")&&!tall) continue;   // H.4: scale is 256z only (hidden, not greyed)
        m_rowIdx[m_nRows++]=r;
        if(kRows[r].kind==NW_TEXT||kRows[r].kind==NW_SEED) m_field[r].setPointSize(du(20));
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
    if(R.kind==NW_SEED){ *w=du(200)+du(5)+m_dice.fitWidth(); *h=du(34); return; }
    if(R.kind==NW_SLIDER){ *w=du(260); *h=du(29); return; }
    if(R.kind==NW_TOGGLE){ *w=Toggle::designW(); *h=Toggle::designH(); return; }
    if(R.kind==NW_PREVIEW){ *w=du(150); *h=du(150); return; }
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
        m_row[k].build(kRows[r].title,hintFor(r),C.size.width,cw,ch);
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
        }else if(kRows[r].kind==NW_SEED){
            const float bw=m_dice.fitWidth();
            m_field[r].setRect(CGRectMake(cr.origin.x,cr.origin.y,cr.size.width-bw-du(5),cr.size.height));
            m_field[r].setKeepVisible(m_panel);
            m_dice.setRect(CGRectMake(cr.origin.x+cr.size.width-bw,cr.origin.y,0,cr.size.height));
        }else if(kRows[r].kind==NW_SLIDER){
            const CGRect rr=m_row[k].rect();
            m_slider[r].setRect(cr);
            m_slider[r].setHitRect(CGRectMake(cr.origin.x-du(8),rr.origin.y,cr.size.width+du(16),rr.size.height));
        }else if(kRows[r].kind==NW_TOGGLE){
            m_toggle[r].setRect(cr);
            m_toggle[r].setHitRect(touchFloor()>0.0f?m_row[k].rect():CGRectMake(0,0,0,0));
        }else if(kRows[r].kind==NW_PREVIEW){
            m_previewRect=cr;
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
    const int playType=m_type;
    close();
    // A blank name takes the engine's own "World N" (and advances its counter), as stock New did.
    std::string ns=name.empty()?menu->settings->getNewWorldName():name;   // G.2b-2 (the field keeps whole UTF-8 chars)
    // H.4: a Classic world is "normal" to the type seam plus its GENERATOR record, which
    // FileManager's new-world branch takes (seed, settings) and writes in the first save.
    unsigned char rec[CLASSIC_RECORD_BYTES];
    if(playType==NW_CLASSIC_TYPE){
        ClassicGen g;
        classicSettings(&g);
        classic_encode_record(g,rec);
    }
    eden_menu_set_pending_world_type(flat);
    eden_menu_set_pending_world_height(height);
    eden_menu_set_pending_generator(playType==NW_CLASSIC_TYPE?rec:NULL);
    menu->startNewWorld(ns);
}

void NewWorldMenu::update(float etime){
    (void)etime;
    if(!m_open) return;
    layout();
    using namespace GLW;

    for(int r=0;r<NW_ROWS;r++){
        if(kRows[r].kind!=NW_TEXT&&kRows[r].kind!=NW_SEED) continue;
        if(kRows[r].kind==NW_SEED&&m_type!=NW_CLASSIC_TYPE) continue;
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
            bool grabbed=false;
            for(int k=0;k<m_nRows;k++){
                if(!rowShown(k)) continue;
                const int r=m_rowIdx[k];
                if(kRows[r].kind==NW_SLIDER){
                    // H.4: the down edge moves the thumb (a tap on the track jumps there), as Settings.
                    if(m_sliderTouch<0&&m_slider[r].hit(mx,my)){
                        m_sliderRow=r; m_sliderTouch=i; grabbed=true;
                        m_slider[r].beginDrag(mx);
                    }
                    continue;
                }
                if(kRows[r].kind==NW_SEED){ m_diceHeld=m_dice.hit(mx,my); m_dice.setPressed(m_diceHeld); continue; }
                if(kRows[r].kind!=NW_CHOICE) continue;
                for(int j=0;j<kRows[r].nOpts&&j<NW_MAX_OPTS;j++)
                    if(m_opt[r][j].hit(mx,my)){ m_heldRow=r; m_heldOpt=j; }
            }
            if(grabbed) continue;
            if(m_scrollTouch<0&&mx>=m_content.origin.x&&mx<=m_content.origin.x+m_content.size.width&&
               my>=m_content.origin.y&&my<=m_content.origin.y+m_content.size.height){
                m_scrollTouch=i; m_barDrag=false;
                m_scroll.beginContentDrag(my);
            }
        }
        if(touches[i].inuse==usage_id&&touches[i].down==M_DOWN){
            const float mx=touches[i].mx, my=touches[i].my;
            if(i==m_sliderTouch&&m_sliderRow>=0) m_slider[m_sliderRow].dragTo(mx);
            if(i==m_scrollTouch){
                if(m_barDrag) m_scroll.barDragTo(my);
                else if(m_scroll.contentDragTo(my)){ m_heldRow=m_heldOpt=-1; m_diceHeld=false; m_dice.setPressed(false); }
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
            const bool diceHit=m_diceHeld&&m_dice.hit(mx,my);
            m_diceHeld=false;
            m_dice.setPressed(false);
            if(i==m_sliderTouch){
                if(m_sliderRow>=0){ m_slider[m_sliderRow].dragTo(mx); m_slider[m_sliderRow].endDrag(); }
                m_sliderRow=m_sliderTouch=-1;
                continue;                     // a drag that ends over a button does not press it
            }
            // A tap on a text row focuses its field — on touch anywhere on the row (the floor raises
            // the hit box, not the art) — and any other release leaves it, as a desktop form does.
            int focus=-1;
            for(int k=0;k<m_nRows;k++){
                if(!rowShown(k)) continue;
                const int r=m_rowIdx[k];
                if(kRows[r].kind==NW_TEXT&&(m_field[r].hit(mx,my)||(touch&&m_row[k].hit(mx,my)))) focus=r;
                if(kRows[r].kind==NW_SEED&&!m_dice.hit(mx,my)&&(m_field[r].hit(mx,my)||(touch&&m_row[k].hit(mx,my)))) focus=r;
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
            if(heldRow>=0&&m_opt[heldRow][heldOpt].hit(mx,my)){
                m_choice[heldRow]=heldOpt;
                if(!std::strcmp(kRows[heldRow].key,"height")) m_builtType=-1;   // Scale shows at 256 only
                continue;
            }
            if(diceHit){ rollSeed(); continue; }
            bool toggled=false;
            for(int k=0;k<m_nRows&&!toggled;k++){
                if(!rowShown(k)) continue;
                const int r=m_rowIdx[k];
                if(kRows[r].kind==NW_TOGGLE&&m_toggle[r].hit(mx,my)){ m_toggle[r].setOn(!m_toggle[r].on()); toggled=true; }
            }
            if(toggled) continue;
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

    GLW::begin2D();

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
        if(kRows[r].kind==NW_SEED){ m_field[r].render(); m_dice.render(); continue; }
        if(kRows[r].kind==NW_SLIDER){ m_slider[r].render(); continue; }
        if(kRows[r].kind==NW_TOGGLE){ m_toggle[r].render(); continue; }
        if(kRows[r].kind==NW_PREVIEW){
            updatePreview();
            bevel(m_previewRect,BEVEL_SUNKEN);
            if(m_preview){
                const float in=du(3);
                Pen::textured(true);
                Pen::color(1,1,1,1);
                m_preview->drawInRect(CGRectMake(m_previewRect.origin.x+in,m_previewRect.origin.y+in,
                                                 m_previewRect.size.width-2*in,m_previewRect.size.height-2*in));
            }
            continue;
        }
        // A choice is a segmented pair: pressed == selected (design-system.md), and the held one.
        for(int j=0;j<kRows[r].nOpts&&j<NW_MAX_OPTS;j++){
            m_opt[r][j].setPressed(j==m_choice[r]||(r==m_heldRow&&j==m_heldOpt));
            m_opt[r][j].render();
        }
    }
    m_scroll.render();

    GLW::end2D();
}

// ---------------------------------------------------------------------------------------------
// harness surface
// ---------------------------------------------------------------------------------------------
CGRect NewWorldMenu::controlRect(const char* which){
    layout();
    if(!which) return CGRectMake(0,0,0,0);
    if(!std::strcmp(which,"back")) return m_back.rect();
    if(!std::strcmp(which,"play")) return m_play.rect();
    if(!std::strcmp(which,"dice")) return m_dice.rect();
    const int r=rowOf(which);
    if(r>=0&&(kRows[r].kind==NW_TEXT||kRows[r].kind==NW_SEED)) return m_field[r].rect();
    if(r>=0&&kRows[r].kind==NW_PREVIEW) return m_previewRect;
    return CGRectMake(0,0,0,0);
}

bool NewWorldMenu::rowVisible(const char* key) const{
    const int r=rowOf(key);
    for(int k=0;k<m_nRows&&r>=0;k++) if(m_rowIdx[k]==r) return true;
    return false;
}

// Scrolls row r into view (the harness taps what it can see); false when r is not in this list.
static bool nw_reveal(int r,int nRows,const int* rowIdx,GLW::ScrollView& scroll){
    for(int k=0;k<nRows;k++){
        if(rowIdx[k]!=r) continue;
        if(!(k>=scroll.first()&&k<scroll.first()+scroll.visible())) scroll.ensureVisible(k);
        return true;
    }
    return false;
}

bool NewWorldMenu::toggleRect(const char* key,CGRect* rect){
    const int r=rowOf(key);
    if(r<0||kRows[r].kind!=NW_TOGGLE) return false;
    layout();
    if(!nw_reveal(r,m_nRows,m_rowIdx,m_scroll)) return false;
    layout();
    if(rect) *rect=m_toggle[r].rect();
    return true;
}

bool NewWorldMenu::toggle(const char* key) const{
    const int r=rowOf(key);
    return r>=0&&kRows[r].kind==NW_TOGGLE&&m_toggle[r].on();
}

bool NewWorldMenu::sliderRect(const char* key,CGRect* rect){
    const int r=rowOf(key);
    if(r<0||kRows[r].kind!=NW_SLIDER) return false;
    layout();
    if(!nw_reveal(r,m_nRows,m_rowIdx,m_scroll)) return false;
    layout();
    if(rect) *rect=m_slider[r].rect();
    return true;
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
    return (r>=0&&(kRows[r].kind==NW_TEXT||kRows[r].kind==NW_SEED))?m_field[r].text().c_str():"";
}

bool NewWorldMenu::focused(const char* key) const{
    const int r=rowOf(key);
    return r>=0&&(kRows[r].kind==NW_TEXT||kRows[r].kind==NW_SEED)&&m_field[r].focused();
}
