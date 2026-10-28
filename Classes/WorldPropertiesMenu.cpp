//
//  WorldPropertiesMenu.mm
//  Eden
//
//  Stage H / H.15: the world list's Properties screen. See WorldPropertiesMenu.h for what it is;
//  the shape (RailFrame panel, IconRail, TwoLineRows in a variable-height ScrollView, the usage_id
//  touch claim) is NewWorldMenu.mm's, which is the one to compare against.
//

#include "WorldPropertiesMenu.h"
#include "ClassicOptions.h"
#include "FileManager.h"
#include "GLDialog.h"
#include "Graphics.h"
#include "Globals.h"
#include "Input.h"
#include "Menu.h"
#include "World.h"
#include "Texture2D.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>

extern float SCREEN_WIDTH;
extern float SCREEN_HEIGHT;

// Row kinds. PHOTO is the World tab's first row (its own drawing, no TwoLineRow); VALUE is a
// read-only row with right-aligned text; the rest are New World's controls.
enum { WPK_PHOTO=0, WPK_TEXT=1, WPK_VALUE=2, WPK_CHOICE=3, WPK_SLIDER=4, WPK_TOGGLE=5 };

static const int usage_id=14;   // distinct from Menu (7), SettingsMenu (3), GLDialog (9), Keybinds (11), WorldBrowser (12), NewWorld (13)
static const int kNameBytes=48; // New World's name rule (NewWorldMenu.mm kRows "name")

static WorldPropertiesMenu* s_confirming=NULL;   // the screen a confirm dialog answers to

// ---------------------------------------------------------------------------------------------
// formatting
// ---------------------------------------------------------------------------------------------
static std::string wp_thousands(long long v){
    char b[32];
    std::snprintf(b,sizeof(b),"%lld",v<0?-v:v);
    std::string s=b,out;
    for(size_t i=0;i<s.size();i++){
        if(i&&(s.size()-i)%3==0) out+=',';
        out+=s[i];
    }
    return v<0?"-"+out:out;
}
static std::string wp_bytes(unsigned long long n){
    char b[32];
    const double v=(double)n;
    if(v>=1073741824.0) std::snprintf(b,sizeof(b),"%.1f GB",v/1073741824.0);
    else if(v>=1048576.0) std::snprintf(b,sizeof(b),"%.1f MB",v/1048576.0);
    else std::snprintf(b,sizeof(b),"%.0f KB",std::max(1.0,v/1024.0));
    return b;
}
static std::string wp_date(long long t){
    if(t<=0) return "unknown";
    const time_t tt=(time_t)t, now=time(NULL);
    struct tm a, b;
#ifdef _WIN32
    localtime_s(&a,&tt); localtime_s(&b,&now);
#else
    localtime_r(&tt,&a); localtime_r(&now,&b);
#endif
    if(a.tm_year==b.tm_year&&a.tm_yday==b.tm_yday) return "today";
    char s[32];
    strftime(s,sizeof(s),a.tm_year==b.tm_year?"%d %b":"%d %b %Y",&a);
    return s[0]=='0'?std::string(s+1):std::string(s);
}
static const char* wp_heightName(int h){ return h>=T_HEIGHT_MAX?"New Dawn 256":"Original 64"; }
static std::string wp_typeName(const WorldProps& p){
    if(p.kind!=WP_KIND_EMOD&&p.type==WP_TYPE_CLASSIC) return "Classic";
    if(p.type==WP_TYPE_FLAT) return "Flat";
    if(p.type==WP_TYPE_NORMAL) return "Normal";
    if(p.genSource==WP_GEN_VOID) return "Classic (newer Emod)";
    return p.gen.profile==CLASSIC_PROFILE_LEGACY?"Classic (1.x)":"Classic";
}
// A UTF-8 string cut to at most n bytes without splitting a character, "..." when cut.
static std::string wp_clip(const std::string& s,size_t n){
    if(s.size()<=n) return s;
    size_t k=n;
    while(k>0&&((unsigned char)s[k]&0xC0)==0x80) k--;
    return s.substr(0,k)+"...";
}

// ---------------------------------------------------------------------------------------------
WorldPropertiesMenu::WorldPropertiesMenu(){
    m_open=false;
    m_tab=TAB_WORLD;
    m_node=NULL;
    m_lastResult=-2;
    m_nAll=m_nRows=0;
    m_heldRow=m_heldOpt=-1;
    m_labelsBuilt=false;
    m_builtU=0;
    m_builtTab=-1;
    m_sliderRow=m_sliderTouch=-1;
    m_photo=NULL;
    m_photoAspect=1.5f;
    m_photoRect=CGRectMake(0,0,0,0);
    m_photoRowTop=0;
    m_panel=m_content=CGRectMake(0,0,0,0);
    m_titleY=0;
    m_scrollTouch=m_railTouch=-1;
    m_barDrag=false;
    m_props.kind=WP_KIND_EMOD; m_props.type=WP_TYPE_NORMAL; m_props.genSource=WP_GEN_RECORD;
    std::memset(&m_props.gen,0,sizeof(m_props.gen));
    for(int r=0;r<WP_MAX_ROWS;r++){ m_choice[r]=0; m_rowIdx[r]=-1; m_rows[r].kind=WPK_VALUE; m_rows[r].key=""; m_rows[r].opts=NULL; m_rows[r].nOpts=0; m_rows[r].tab=0; }
    static const int icons[TABS]={ UIICON_INFO, UIICON_MOUNTAIN };
    m_rail.setTiles(icons,TABS);
    m_name.setMaxBytes(kNameBytes);
    m_name.setWorldNameOnly(true);   // FT.3
}

WorldPropertiesMenu::~WorldPropertiesMenu(){
    blurFields();
    delete m_photo;
    if(s_confirming==this) s_confirming=NULL;
}

int WorldPropertiesMenu::rowOf(const char* key) const{
    if(!key) return -1;
    for(int r=0;r<m_nAll;r++) if(!std::strcmp(m_rows[r].key,key)) return r;
    return -1;
}

void WorldPropertiesMenu::blurFields(){ m_name.blur(); }

void WorldPropertiesMenu::addRow(int tab,int kind,const char* key,const char* title,const char* hint,
                                 const char* value,const NWOption* opts,int nOpts){
    if(m_nAll>=WP_MAX_ROWS) return;
    Row& R=m_rows[m_nAll++];
    R.tab=tab; R.kind=kind; R.key=key;
    R.title=title?title:""; R.hint=hint?hint:""; R.value=value?value:"";
    R.opts=opts; R.nOpts=nOpts;
}

// ---------------------------------------------------------------------------------------------
// the rows this world gets (plan §2)
// ---------------------------------------------------------------------------------------------
void WorldPropertiesMenu::buildRows(){
    const WorldProps& p=m_props;
    m_nAll=0;
    char buf[160];

    // ---- World
    addRow(TAB_WORLD,WPK_PHOTO,"photo","","");
    if(p.kind==WP_KIND_ARCHIVE)
        addRow(TAB_WORLD,WPK_VALUE,"name","World name","Inside a compressed file the name can't change. Play the world once to convert it.",
               wp_clip(p.name,28).c_str());
    else
        addRow(TAB_WORLD,WPK_TEXT,"name","World name","Shown in the world list.");
    if(p.type==WP_TYPE_CLASSIC){
        std::snprintf(buf,sizeof(buf),"%d",p.seed);
        addRow(TAB_WORLD,WPK_VALUE,"seed","Seed","A new seed is a new world: make one with New.",buf);
    }
    addRow(TAB_WORLD,WPK_VALUE,"height","Height",
           p.height>=T_HEIGHT_MAX?"Change it with Settings > Storage > Convert to 64z.":"Set when the world was made.",
           wp_heightName(p.height));
    if(p.columns>=0){
        std::string v=wp_thousands(p.columns)+(p.columns==1?" column":" columns");
        addRow(TAB_WORLD,WPK_VALUE,"land","Saved land","The columns you've changed; the rest is generated as you go.",v.c_str());
    }
    if(p.signs>=0){
        std::snprintf(buf,sizeof(buf),"%d",p.signs);
        addRow(TAB_WORLD,WPK_VALUE,"signs","Signs","",buf);
    }
    if(p.goldencubes>=0){
        std::snprintf(buf,sizeof(buf),"%d",p.goldencubes);
        addRow(TAB_WORLD,WPK_VALUE,"cubes","Golden cubes held","",buf);
    }
    {
        std::string v=(p.createdUnix>0?wp_date(p.createdUnix):std::string("unknown"))+" / "+wp_date(p.savedUnix);
        addRow(TAB_WORLD,WPK_VALUE,"dates","Created / Last saved","",v.c_str());
    }
    if(!p.convertedFrom.empty()){
        std::snprintf(buf,sizeof(buf),"%s (v%d)",wp_clip(p.convertedFrom,28).c_str(),p.convertedVersion);
        addRow(TAB_WORLD,WPK_VALUE,"converted","Converted from","",buf);
    }
    if(p.originalBytes>0)
        addRow(TAB_WORLD,WPK_VALUE,"original","Original kept","The file it was converted from. Share > Remove original deletes it.",
               wp_bytes(p.originalBytes).c_str());

    // ---- Terrain
    if(p.kind!=WP_KIND_EMOD){
        addRow(TAB_TERRAIN,WPK_VALUE,"type","Terrain","Play this world once to convert it; then its terrain can change.",wp_typeName(p).c_str());
        return;
    }
    if(p.type==WP_TYPE_FLAT){
        addRow(TAB_TERRAIN,WPK_VALUE,"type","Type","Only Classic worlds have terrain settings.","Flat");
        return;
    }
    if(p.type==WP_TYPE_NORMAL){
        addRow(TAB_TERRAIN,WPK_VALUE,"type","Type","Normal: the bundled 2880 map. Only Classic worlds have terrain settings.","Normal");
        return;
    }
    if(p.genSource==WP_GEN_VOID){
        addRow(TAB_TERRAIN,WPK_VALUE,"type","Type","Made by a newer Emod: its terrain settings can't be changed here.","Classic");
        return;
    }
    addRow(TAB_TERRAIN,WPK_VALUE,"lead",wp_typeName(p).c_str(),
           "Changes reshape all land you haven't built on, including places you've been. What you've built or dug stays.");
    addRow(TAB_TERRAIN,WPK_CHOICE,"hills","Hilliness","How far the land rises and falls.","",kHills,5);
    std::string caveHint="2011: the first caves, opening into hillsides. Deep: further down.";
    if(p.gen.flags&1){
        const int ci=classic_option_index(kCaves,3,p.gen.caves);
        caveHint=std::string("Detected from this world: ")+kCaves[ci].label+".";
    }
    addRow(TAB_TERRAIN,WPK_CHOICE,"caves","Caves",caveHint.c_str(),"",kCaves,3);
    addRow(TAB_TERRAIN,WPK_CHOICE,"trees","Trees","How many trees grow.","",kTrees,4);
    addRow(TAB_TERRAIN,WPK_TOGGLE,"flowers","Flowers","The original blue flower patches.");
    addRow(TAB_TERRAIN,WPK_TOGGLE,"clouds","Clouds","Low cloud slabs over the land.");
    addRow(TAB_TERRAIN,WPK_CHOICE,"size","Size","Extended has no edge; 2880 square has the original map's wall.","",kSizes,2);
    // Scale: 256z only (hidden at 64, as New World), and read-only on a 1.x world.
    if(p.height>=T_HEIGHT_MAX){
        if(p.gen.profile==CLASSIC_PROFILE_LEGACY)
            addRow(TAB_TERRAIN,WPK_VALUE,"scale","Scale","1.x worlds only match at 1x.",
                   kScales[classic_option_index(kScales,5,p.gen.scale4)].label);
        else
            addRow(TAB_TERRAIN,WPK_SLIDER,"scale","Scale","How big the landscape is. 1x is the original game's.","",kScales,5);
    }
}

// ---------------------------------------------------------------------------------------------
// open / close
// ---------------------------------------------------------------------------------------------
bool WorldPropertiesMenu::open(WorldNode* node){
    if(!node||node->file_name.empty()) return false;
    FileManager* fm=World::getWorld->fm;
    WorldProps p;
    if(!fm->readWorldProperties(node->file_name,&p)) return false;
    printg("properties: %s read in %.1f ms\n",node->file_name.c_str(),p.openMs);
    m_props=p;
    m_node=node;
    m_fileName=node->file_name;
    m_lastResult=-2;
    buildRows();
    // The controls start at the world's values.
    const ClassicGen& g=p.gen;
    for(int r=0;r<m_nAll;r++){
        const Row& R=m_rows[r];
        m_choice[r]=0;
        if(!std::strcmp(R.key,"hills")) m_choice[r]=classic_option_index(kHills,5,g.hilliness);
        if(!std::strcmp(R.key,"caves")) m_choice[r]=classic_option_index(kCaves,3,g.caves);
        if(!std::strcmp(R.key,"trees")) m_choice[r]=classic_option_index(kTrees,4,g.trees);
        if(!std::strcmp(R.key,"size"))  m_choice[r]=classic_option_index(kSizes,2,g.size);
        if(!std::strcmp(R.key,"flowers")) m_toggle[r].setOn(g.flowers!=0);
        if(!std::strcmp(R.key,"clouds"))  m_toggle[r].setOn(g.clouds!=0);
        if(R.kind==WPK_SLIDER){
            m_slider[r].setRange(0,(float)(R.nOpts-1),1);
            m_slider[r].setValue((float)classic_option_index(kScales,5,g.scale4));
        }
    }
    // The field holds the name; blank (cleared) keeps it, so the placeholder shows it too.
    m_name.setText(p.name.c_str());
    m_name.setPlaceholder(p.name.c_str());
    loadPhoto();
    m_open=true;
    m_tab=TAB_WORLD;
    m_scroll.setFirst(0);
    m_scrollTouch=m_railTouch=-1;
    m_barDrag=false;
    m_heldRow=m_heldOpt=-1;
    m_sliderRow=m_sliderTouch=-1;
    m_labelsBuilt=false;
    layout();
    return true;
}

// The world's last in-game photo: camera mode writes `<world file>.png` beside the world (H.16).
// Sized from its IHDR for the aspect fit, as Get Worlds' preview is.
void WorldPropertiesMenu::loadPhoto(){
    delete m_photo;
    m_photo=NULL;
    m_photoAspect=1.5f;
    const std::string path=World::getWorld->fm->documents+"/"+m_fileName+".png";
    FILE* f=fopen(path.c_str(),"rb");
    if(!f) return;
    unsigned char b[24];
    const bool ok=fread(b,1,24,f)==24;
    fclose(f);
    if(!ok||!(b[0]==0x89&&b[1]=='P'&&b[2]=='N'&&b[3]=='G')) return;
    const unsigned w=(b[16]<<24)|(b[17]<<16)|(b[18]<<8)|b[19];
    const unsigned h=(b[20]<<24)|(b[21]<<16)|(b[22]<<8)|b[23];
    if(!w||!h) return;
    Texture2D* t=new Texture2D(path,false);
    if(!t->name){ delete t; return; }
    m_photo=t;
    m_photoAspect=(float)w/(float)h;
}

void WorldPropertiesMenu::close(){
    blurFields();
    m_open=false;
    delete m_photo;
    m_photo=NULL;
    if(s_confirming==this){ s_confirming=NULL; GLDialog::dismiss(); }
    // As NewWorldMenu::close(): Menu::update returned early for every frame this screen was up.
    Input::getInput()->clearAll();
}

void WorldPropertiesMenu::back(){ close(); }

void WorldPropertiesMenu::selectTab(int t){
    if(t<0||t>=TABS||t==m_tab) return;
    m_tab=t;
    m_scroll.setFirst(0);
    m_builtTab=-1;
}

// ---------------------------------------------------------------------------------------------
// what changed
// ---------------------------------------------------------------------------------------------
std::string WorldPropertiesMenu::trimmedName() const{
    const std::string& n=m_name.text();
    const size_t a=n.find_first_not_of(" \t"), b=n.find_last_not_of(" \t");
    return a==std::string::npos?std::string():n.substr(a,b-a+1);
}

bool WorldPropertiesMenu::nameChanged() const{
    if(m_props.kind==WP_KIND_ARCHIVE) return false;
    const std::string n=trimmedName();
    return !n.empty()&&n!=m_props.name;
}

void WorldPropertiesMenu::editedGenerator(ClassicGen* g) const{
    *g=m_props.gen;
    for(int r=0;r<m_nAll;r++){
        const Row& R=m_rows[r];
        if(R.kind==WPK_CHOICE){
            const int v=R.opts[m_choice[r]].value;
            if(!std::strcmp(R.key,"hills")) g->hilliness=(unsigned char)v;
            if(!std::strcmp(R.key,"caves")) g->caves=(unsigned char)v;
            if(!std::strcmp(R.key,"trees")) g->trees=(unsigned char)v;
            if(!std::strcmp(R.key,"size"))  g->size=(unsigned char)v;
        }else if(R.kind==WPK_TOGGLE){
            if(!std::strcmp(R.key,"flowers")) g->flowers=m_toggle[r].on()?1:0;
            if(!std::strcmp(R.key,"clouds"))  g->clouds=m_toggle[r].on()?1:0;
        }else if(R.kind==WPK_SLIDER){
            g->scale4=(unsigned char)R.opts[(int)(m_slider[r].value()+0.5f)].value;
        }
    }
    if(g->caves!=m_props.gen.caves) g->flags&=~1;   // no longer the detected caves
}

bool WorldPropertiesMenu::terrainChanged() const{
    if(m_props.kind!=WP_KIND_EMOD||m_props.type!=WP_TYPE_CLASSIC||m_props.genSource==WP_GEN_VOID) return false;
    ClassicGen g;
    editedGenerator(&g);
    unsigned char a[CLASSIC_RECORD_BYTES],b[CLASSIC_RECORD_BYTES];
    classic_encode_record(m_props.gen,a);
    classic_encode_record(g,b);
    return std::memcmp(a,b,sizeof(a))!=0;
}

bool WorldPropertiesMenu::saveEnabled() const{ return m_open&&(nameChanged()||terrainChanged()); }

// ---------------------------------------------------------------------------------------------
// saving (plan §3)
// ---------------------------------------------------------------------------------------------
void WorldPropertiesMenu::confirmCb(int chosen){
    WorldPropertiesMenu* s=s_confirming;
    s_confirming=NULL;
    if(chosen==0&&s&&s->m_open) s->commit();
}

void WorldPropertiesMenu::save(){
    if(!saveEnabled()) return;
    blurFields();
    if(!terrainChanged()){ commit(); return; }
    static const char* const kButtons[]={"Change terrain","Cancel"};
    s_confirming=this;
    GLDialog::show("Change this world's terrain?",
                   "Land you haven't built on will be regenerated with the new settings, everywhere, including places "
                   "you've already explored. Anything you've built or dug stays exactly as it is, so its edges may show. "
                   "Setting these back later brings the old land back.",
                   kButtons,2,confirmCb);
}

void WorldPropertiesMenu::commit(){
    FileManager* fm=World::getWorld->fm;
    WorldProps e=m_props;
    const std::string n=trimmedName();
    if(!n.empty()) e.name=n;
    editedGenerator(&e.gen);
    const int r=fm->applyWorldProperties(m_fileName,e);
    m_lastResult=r;
    if(r<0){
        static const char* const kOk[]={"OK"};
        GLDialog::show("Couldn't save the world's properties","Nothing was changed.",kOk,1,NULL);
        return;
    }
    Menu* menu=World::getWorld->menu;
    if((r&WP_WROTE_NAME)&&m_node){
        m_node->display_name=e.name;
        if(menu&&m_node==menu->selected_world) menu->fnbar->setStatus(m_node->display_name,9999);
    }
    if(menu) menu->sbar->setStatus("Saved",2);
    close();
}

// ---------------------------------------------------------------------------------------------
// layout
// ---------------------------------------------------------------------------------------------
void WorldPropertiesMenu::buildLabels(){
    using namespace GLW;
    m_labelsBuilt=true;
    m_builtU=u();
    m_builtTab=m_tab;
    const std::string t=wp_clip(m_props.name,24)+" Properties";
    m_title.set(t.c_str(),du(32),UITextAlignmentCenter);
    m_back.setLabel("Back",du(20));
    m_back.sizeToFit();
    m_save.setLabel("Save",du(20));
    m_save.setTone(GLW::Button::TONE_POSITIVE);
    m_save.sizeToFit();
    m_name.setPointSize(du(20));
    m_nRows=0;
    for(int r=0;r<m_nAll&&m_nRows<WP_MAX_ROWS;r++){
        const Row& R=m_rows[r];
        if(R.tab!=m_tab) continue;
        m_rowIdx[m_nRows++]=r;
        if(R.kind==WPK_VALUE){
            if(R.value.empty()) m_value[r].clear();
            else m_value[r].set(R.value.c_str(),du(20),UITextAlignmentLeft);   // one line; long values are clipped by buildRows
        }
        for(int j=0;j<R.nOpts&&j<WP_MAX_OPTS&&R.kind==WPK_CHOICE;j++){
            m_opt[r][j].setLabel(R.opts[j].label,du(20));
            m_opt[r][j].sizeToFit();
        }
    }
    // The photo row's text: the name, then height, type and file size.
    m_photoName.set(wp_clip(m_props.name,28).c_str(),du(26),UITextAlignmentLeft);
    m_photoLine[0].set(wp_heightName(m_props.height),du(16),UITextAlignmentLeft,0,FACE_BODY);
    m_photoLine[1].set(wp_typeName(m_props).c_str(),du(16),UITextAlignmentLeft,0,FACE_BODY);
    m_photoLine[2].set(wp_bytes(m_props.fileBytes).c_str(),du(16),UITextAlignmentLeft,0,FACE_BODY);
    m_noPhoto.set("No picture yet. Take one in camera mode.",du(14),UITextAlignmentCenter,du(150),FACE_BODY);
}

void WorldPropertiesMenu::controlSize(int r,float* w,float* h) const{
    using namespace GLW;
    const Row& R=m_rows[r];
    if(R.kind==WPK_TEXT){ *w=du(320); *h=du(34); return; }
    if(R.kind==WPK_SLIDER){ *w=du(260); *h=du(29); return; }
    if(R.kind==WPK_TOGGLE){ *w=Toggle::designW(); *h=Toggle::designH(); return; }
    if(R.kind==WPK_VALUE){
        *w=m_value[r].empty()?0.0f:m_value[r].width()+du(4);
        *h=std::max(du(29),m_value[r].empty()?0.0f:m_value[r].height());
        return;
    }
    float cw=0;
    for(int j=0;j<R.nOpts&&j<WP_MAX_OPTS;j++) cw+=m_opt[r][j].fitWidth()+(j?du(5):0.0f);
    *w=cw; *h=du(29);
}

bool WorldPropertiesMenu::rowShown(int k) const{
    return k>=m_scroll.first()&&k<m_scroll.first()+m_scroll.visible()&&k<m_nRows;
}

void WorldPropertiesMenu::layout(){
    using namespace GLW;
    if(!m_labelsBuilt||m_builtTab!=m_tab||m_builtU!=u()) buildLabels();
    const RailFrame f=railFrame();
    m_panel=f.panel;
    const CGRect& T=f.titlebar;
    m_back.setRect(CGRectMake(T.origin.x,T.origin.y,0,T.size.height));
    titlebarHit(m_back);
    m_save.setRect(CGRectMake(T.origin.x+T.size.width-m_save.fitWidth(),T.origin.y,0,T.size.height));
    titlebarHit(m_save);
    m_save.setEnabled(saveEnabled());
    m_titleY=f.titleY(m_title);
    m_rail.setRect(f.rail);
    m_rail.setSelected(m_tab);
    m_scroll.setBarRect(f.bar);
    m_content=f.content;
    const CGRect& C=m_content;
    const float photoH=du(110), photoPad=du(8);

    float heights[WP_MAX_ROWS];
    for(int k=0;k<m_nRows;k++){
        const int r=m_rowIdx[k];
        if(m_rows[r].kind==WPK_PHOTO){ heights[k]=photoH+2*photoPad; continue; }
        float cw,ch;
        controlSize(r,&cw,&ch);
        m_row[k].build(m_rows[r].title.c_str(),m_rows[r].hint.c_str(),C.size.width,cw,ch);
        m_row[k].setLast(k==m_nRows-1);
        heights[k]=m_row[k].height();
    }
    m_scroll.setRowHeights(heights,m_nRows,C.size.height);

    m_photoRect=CGRectMake(0,0,0,0);
    for(int k=0;k<m_nRows;k++){
        if(!rowShown(k)) continue;
        const int r=m_rowIdx[k];
        const float yTop=C.origin.y+C.size.height-m_scroll.rowOffset(k);
        if(m_rows[r].kind==WPK_PHOTO){
            // The photo at the fixed 3:2 frame (H.16's aspect), full row height, at the row's left.
            m_photoRowTop=yTop;
            m_photoRect=CGRectMake(C.origin.x+du(12),yTop-photoPad-photoH,photoH*1.5f,photoH);
            continue;
        }
        m_row[k].setOrigin(C.origin.x,yTop);
        float cw,ch;
        controlSize(r,&cw,&ch);
        const CGRect cr=m_row[k].controlRect(cw,ch);
        const int kind=m_rows[r].kind;
        if(kind==WPK_TEXT){
            m_name.setRect(cr);
            m_name.setKeepVisible(m_panel);
        }else if(kind==WPK_SLIDER){
            const CGRect rr=m_row[k].rect();
            m_slider[r].setRect(cr);
            m_slider[r].setHitRect(CGRectMake(cr.origin.x-du(8),rr.origin.y,cr.size.width+du(16),rr.size.height));
        }else if(kind==WPK_TOGGLE){
            m_toggle[r].setRect(cr);
            m_toggle[r].setHitRect(touchFloor()>0.0f?m_row[k].rect():CGRectMake(0,0,0,0));
        }else if(kind==WPK_CHOICE){
            float x=cr.origin.x;
            for(int j=0;j<m_rows[r].nOpts&&j<WP_MAX_OPTS;j++){
                m_opt[r][j].setRect(CGRectMake(x,cr.origin.y,0,cr.size.height));
                x+=m_opt[r][j].fitWidth()+du(5);
            }
        }
    }
}

// ---------------------------------------------------------------------------------------------
// input (NewWorldMenu::update's shape)
// ---------------------------------------------------------------------------------------------
void WorldPropertiesMenu::update(float etime){
    (void)etime;
    if(!m_open) return;
    layout();
    using namespace GLW;

    if(rowVisible("name")&&m_rows[rowOf("name")].kind==WPK_TEXT){
        // Return in the name field is Save (when there is something to save). Escape only leaves it.
        if(m_name.update()==TextField::EV_COMMIT){ save(); return; }
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
            m_save.setPressed(m_save.enabled()&&m_save.hit(mx,my));
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
                if(m_rows[r].kind==WPK_SLIDER){
                    if(m_sliderTouch<0&&m_slider[r].hit(mx,my)){
                        m_sliderRow=r; m_sliderTouch=i; grabbed=true;
                        m_slider[r].beginDrag(mx);
                    }
                    continue;
                }
                if(m_rows[r].kind!=WPK_CHOICE) continue;
                for(int j=0;j<m_rows[r].nOpts&&j<WP_MAX_OPTS;j++)
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
                else if(m_scroll.contentDragTo(my)) m_heldRow=m_heldOpt=-1;
            }
            if(i==m_railTouch&&m_rail.dragTo(my)) m_rail.setHeld(-1);
        }
        if(touches[i].inuse==usage_id&&touches[i].down==M_RELEASE){
            const float mx=touches[i].mx, my=touches[i].my;
            touches[i].inuse=0;
            touches[i].down=M_NONE;
            const bool backHit=m_back.pressed()&&m_back.hit(mx,my);
            const bool saveHit=m_save.pressed()&&m_save.hit(mx,my);
            m_back.setPressed(false);
            m_save.setPressed(false);
            const int heldRow=m_heldRow, heldOpt=m_heldOpt;
            m_heldRow=m_heldOpt=-1;
            if(i==m_sliderTouch){
                if(m_sliderRow>=0){ m_slider[m_sliderRow].dragTo(mx); m_slider[m_sliderRow].endDrag(); }
                m_sliderRow=m_sliderTouch=-1;
                continue;
            }
            // A tap on the name row focuses its field (on touch anywhere on the row); any other
            // release leaves it, as New World does.
            bool focus=false;
            for(int k=0;k<m_nRows;k++){
                if(!rowShown(k)) continue;
                const int r=m_rowIdx[k];
                if(m_rows[r].kind==WPK_TEXT&&(m_name.hit(mx,my)||(touch&&m_row[k].hit(mx,my)))) focus=true;
            }
            if(!focus) blurFields();

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
            if(i==m_scrollTouch){
                const bool scrolled=m_barDrag||m_scroll.scrolling();
                m_scroll.endDrag();
                m_scrollTouch=-1; m_barDrag=false;
                if(scrolled) continue;
            }
            if(backHit){ close(); return; }
            if(saveHit){ save(); return; }
            if(heldRow>=0&&m_opt[heldRow][heldOpt].hit(mx,my)){ m_choice[heldRow]=heldOpt; continue; }
            bool toggled=false;
            for(int k=0;k<m_nRows&&!toggled;k++){
                if(!rowShown(k)) continue;
                const int r=m_rowIdx[k];
                if(m_rows[r].kind==WPK_TOGGLE&&m_toggle[r].hit(mx,my)){ m_toggle[r].setOn(!m_toggle[r].on()); toggled=true; }
            }
            if(toggled) continue;
            if(focus) m_name.focus();
        }
    }
}

// ---------------------------------------------------------------------------------------------
// render
// ---------------------------------------------------------------------------------------------
void WorldPropertiesMenu::render(){
    if(!m_open) return;
    layout();

    GLW::begin2D();

    using namespace GLW;
    fill(CGRectMake(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT), kScrim);
    bevel(m_panel, BEVEL_WINDOW);
    m_back.render();
    m_save.render();
    m_title.drawCentered(m_panel.origin.x+m_panel.size.width*0.5f, m_titleY, kText);
    m_rail.render();

    bevel(m_content, BEVEL_CONTENT);
    for(int k=0;k<m_nRows;k++){
        if(!rowShown(k)) continue;
        const int r=m_rowIdx[k];
        const int kind=m_rows[r].kind;
        if(kind==WPK_PHOTO){
            const CGRect& P=m_photoRect;
            bevel(P,BEVEL_SUNKEN);
            const float in=du(3);
            const CGRect I=CGRectMake(P.origin.x+in,P.origin.y+in,P.size.width-2*in,P.size.height-2*in);
            if(m_photo){
                // Aspect-fit inside the frame (a photo at H.16's 3:2 fills it exactly).
                float w=I.size.width, h=w/m_photoAspect;
                if(h>I.size.height){ h=I.size.height; w=h*m_photoAspect; }
                fill(I,kBlack);
                Pen::textured(true);
                Pen::color(1,1,1,1);
                m_photo->drawInRect(CGRectMake(I.origin.x+(I.size.width-w)*0.5f,I.origin.y+(I.size.height-h)*0.5f,w,h));
            }else{
                fill(I,kKeyline);
                m_noPhoto.drawCentered(I.origin.x+I.size.width*0.5f,I.origin.y+(I.size.height+m_noPhoto.height())*0.5f,kWhite);
            }
            float x=P.origin.x+P.size.width+du(14), y=P.origin.y+P.size.height;
            m_photoName.draw(x,y,kText);
            y-=m_photoName.height()+du(4);
            for(int i=0;i<3;i++){ m_photoLine[i].draw(x,y,kTextSecondary); y-=m_photoLine[i].height()+du(2); }
            continue;
        }
        m_row[k].render();
        if(kind==WPK_TEXT){ m_name.render(); continue; }
        if(kind==WPK_SLIDER){ m_slider[r].render(); continue; }
        if(kind==WPK_TOGGLE){ m_toggle[r].render(); continue; }
        if(kind==WPK_VALUE){
            if(!m_value[r].empty()){
                float cw,ch;
                controlSize(r,&cw,&ch);
                const CGRect cr=m_row[k].controlRect(cw,ch);
                m_value[r].draw(cr.origin.x,cr.origin.y+(cr.size.height+m_value[r].height())*0.5f,kText);
            }
            continue;
        }
        for(int j=0;j<m_rows[r].nOpts&&j<WP_MAX_OPTS;j++){
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
CGRect WorldPropertiesMenu::controlRect(const char* which){
    layout();
    if(!which) return CGRectMake(0,0,0,0);
    if(!std::strcmp(which,"back")) return m_back.rect();
    if(!std::strcmp(which,"save")) return m_save.rect();
    if(!std::strcmp(which,"photo")) return m_photoRect;
    if(!std::strcmp(which,"name")&&rowVisible("name")&&m_rows[rowOf("name")].kind==WPK_TEXT) return m_name.rect();
    return CGRectMake(0,0,0,0);
}

CGRect WorldPropertiesMenu::tabRect(int t){ layout(); return m_rail.tileRect(t); }
void WorldPropertiesMenu::showTab(int t){ selectTab(t); layout(); }

bool WorldPropertiesMenu::rowVisible(const char* key) const{
    const int r=rowOf(key);
    for(int k=0;k<m_nRows&&r>=0;k++) if(m_rowIdx[k]==r) return true;
    return false;
}

bool WorldPropertiesMenu::rowEditable(const char* key) const{
    const int r=rowOf(key);
    return r>=0&&m_rows[r].kind!=WPK_VALUE&&m_rows[r].kind!=WPK_PHOTO;
}

// Scrolls row r into view (the harness taps what it can see); false when r is not in this tab.
bool WorldPropertiesMenu::reveal(int r){
    layout();
    for(int k=0;k<m_nRows;k++){
        if(m_rowIdx[k]!=r) continue;
        if(!rowShown(k)){ m_scroll.ensureVisible(k); layout(); }
        return true;
    }
    return false;
}

bool WorldPropertiesMenu::choiceRect(const char* key,int option,CGRect* rect){
    const int r=rowOf(key);
    if(r<0||m_rows[r].kind!=WPK_CHOICE||option<0||option>=m_rows[r].nOpts) return false;
    if(!reveal(r)) return false;
    if(rect) *rect=m_opt[r][option].rect();
    return true;
}

int WorldPropertiesMenu::choice(const char* key) const{
    const int r=rowOf(key);
    return (r>=0&&m_rows[r].kind==WPK_CHOICE)?m_rows[r].opts[m_choice[r]].value:-1;
}

bool WorldPropertiesMenu::toggleRect(const char* key,CGRect* rect){
    const int r=rowOf(key);
    if(r<0||m_rows[r].kind!=WPK_TOGGLE||!reveal(r)) return false;
    if(rect) *rect=m_toggle[r].rect();
    return true;
}

bool WorldPropertiesMenu::toggle(const char* key) const{
    const int r=rowOf(key);
    return r>=0&&m_rows[r].kind==WPK_TOGGLE&&m_toggle[r].on();
}

bool WorldPropertiesMenu::sliderRect(const char* key,CGRect* rect){
    const int r=rowOf(key);
    if(r<0||m_rows[r].kind!=WPK_SLIDER||!reveal(r)) return false;
    if(rect) *rect=m_slider[r].rect();
    return true;
}

const char* WorldPropertiesMenu::value(const char* key) const{
    const int r=rowOf(key);
    return r>=0?m_rows[r].value.c_str():"";
}

const char* WorldPropertiesMenu::text(const char* key) const{
    return (key&&!std::strcmp(key,"name"))?m_name.text().c_str():"";
}

bool WorldPropertiesMenu::focused(const char* key) const{
    return key&&!std::strcmp(key,"name")&&m_name.focused();
}
