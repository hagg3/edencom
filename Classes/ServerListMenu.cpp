//
//  ServerListMenu.mm
//  Eden
//
//  MP.6: see ServerListMenu.h. The list is a row-snapped GLW::ScrollView of uniform two-line rows
//  drawn here (the kit's ListRow is one line); the claim protocol and the railFrame panel are the
//  other kit screens'. Every label is rebuilt only when the text it shows changes.
//
#include "ServerListMenu.h"
#include "ServerFormMenu.h"
#include "GLDialog.h"
#include "Graphics.h"
#include "Globals.h"
#include "Input.h"
#include "Menu.h"
#include "World.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

extern float SCREEN_WIDTH;
extern float SCREEN_HEIGHT;

static const int usage_id=19;   // distinct from Menu (7), SettingsMenu (3), GLDialog (9), ..., Console (17), ServerForm (18)
static ServerListMenu* s_self=NULL;   // the dialog callbacks' way back (one list exists)

// The selection tint of GLW::ListRow (a light lime wash + a 4u lime bar).
static const GLW::Color kSelTint={ 0.45f*0xc6/255.0f+0.55f, 0.45f*0xf1/255.0f+0.55f, 0.45f*0x75/255.0f+0.55f, 1.0f };
static const GLW::Color kSelBar ={ 0x89/255.0f, 0xc3/255.0f, 0x1f/255.0f, 1.0f };

ServerListMenu::ServerListMenu(){
    s_self=this;
    m_open=false;
    m_form=new ServerFormMenu(this);
    m_sel=-1;
    m_pending=0;
    m_tab=0; m_mmPending=0; m_tabHeld=-1; m_mmOn=false;
    m_pane=false;
    m_built=false;
    m_builtU=0;
    m_titleY=0;
    m_rowH=30;
    m_phase=0;
    m_paneN=0;
    m_listTouch=m_barTouch=m_downRow=-1;
    m_connecting=m_debugConn=false;
    m_lockLearnt=false;
    m_cN=0;
    std::memset(&m_js,0,sizeof(m_js));
    m_panel=m_list=m_paneRect=m_barRect=m_cPanel=CGRectMake(0,0,0,0);
    for(int k=0;k<SL_MAX_VISIBLE;k++) m_rowUi[k].rect=CGRectMake(0,0,0,0);
    m_join_.row=-1;
    m_join_.port=0;
    m_join_.remember=false;
}

ServerListMenu::~ServerListMenu(){ delete m_form; }

void ServerListMenu::open(){
    m_open=true;
    eden_mp_servers_load();
    eden_mp_matchmakers_reload();
    m_tab=0;
    m_tabHeld=-1;
    m_mmOn=eden_mp_matchmaker_count()>0;
    m_sel=eden_mp_servers_count()>0?0:-1;
    m_scroll.setFirst(0);
    m_listTouch=m_barTouch=m_downRow=-1;
    for(int k=0;k<SL_MAX_VISIBLE;k++) m_rowUi[k].key.clear();
    eden_mp_servers_refresh(-1);   // list plan §4: every row on open, no polling after
}

void ServerListMenu::close(){
    if(m_form->isOpen()) m_form->close(ServerFormMenu::OUT_BACK);
    m_open=false;
    Input::getInput()->clearAll();
}

bool ServerListMenu::back(){
    if(!m_open) return false;
    if(m_form->isOpen()) return m_form->back();
    if(m_connecting){
        if(m_js.stage<EDEN_MP_JOIN_LOADING){ eden_mp_join_cancel(); m_connecting=m_debugConn=false; }
        return true;
    }
    close();
    if(World::getWorld&&World::getWorld->menu) World::getWorld->menu->showHome(true);   // UI plan §1
    return true;
}

int ServerListMenu::count() const{
    return m_tab==1?eden_mp_matchmaker_rows():eden_mp_servers_count();
}

// MP.8: switch tabs. Matchmakers asks every address again on each visit (no polling after).
void ServerListMenu::showTab(int t){
    if(t<0||t>1||(t==1&&!m_mmOn)||t==m_tab) return;
    m_tab=t;
    m_scroll.setFirst(0);
    m_listTouch=m_barTouch=m_downRow=-1;
    for(int k=0;k<SL_MAX_VISIBLE;k++) m_rowUi[k].key.clear();
    m_paneKey.clear();
    m_sel=-1;
    if(t==1) eden_mp_matchmakers_fetch();
    m_sel=count()>0?0:-1;
}

void ServerListMenu::select(int i){
    if(i<0||i>=count()) return;   // the rows may have grown since the last layout   // the rows may have grown since the last layout
    m_sel=i;
    m_scroll.ensureVisible(i);
}

std::string ServerListMenu::displayName(const EdenMpServerRow& r) const{
    if(r.label[0]) return r.label;
    if(r.name[0]) return r.name;
    char b[300];
    std::snprintf(b,sizeof(b),"%s:%d",r.host,r.port);
    return b;
}

// ---------------------------------------------------------------------------------------------
// layout
// ---------------------------------------------------------------------------------------------
// Builds `l` from `text`, shortened with "..." until it fits `maxW` (labels do not clip).
static void sl_fit(GLW::Label& l,const std::string& text,float pt,float maxW,GLW::Face face){
    l.set(text.c_str(),pt,UITextAlignmentLeft,0,face);
    if(l.width()<=maxW||text.empty()) return;
    size_t n=(size_t)(text.size()*maxW/std::max(1.0f,l.width()));
    for(int tries=0;tries<6&&n>0;tries++){
        while(n>0&&((unsigned char)text[n]&0xC0)==0x80) n--;
        l.set((text.substr(0,n)+"...").c_str(),pt,UITextAlignmentLeft,0,face);
        if(l.width()<=maxW) return;
        n=n>4?n-4:0;
    }
}

static const char* sl_statusWord(int st){
    switch(st){
        case EDEN_MP_ST_BASIC: return "Online, no details";
        case EDEN_MP_ST_FULL: return "Server full";
        case EDEN_MP_ST_NOT_RUNNING: return "Not running";
        case EDEN_MP_ST_NO_ANSWER: return "No answer";
        case EDEN_MP_ST_UNKNOWN_HOST: return "Unknown host";
        default: return "";
    }
}

// Column widths of a row's right side, points.
static float sl_lockW(){ return GLW::du(30); }
static float sl_pingW(){ return GLW::du(86); }
static float sl_playersW(){ return GLW::du(60); }

void ServerListMenu::buildRow(int slot,int index){
    using namespace GLW;
    RowUi& ui=m_rowUi[slot];
    const EdenMpServerRow& r=m_rows[index];
    char hp[300];
    std::snprintf(hp,sizeof(hp),"%s:%d",r.host,r.port);
    std::string sub;
    if(m_tab==1) sub=std::string(hp)+"  -  "+r.motd;   // MP.8: where it is, and who listed it
    else if(r.status==EDEN_MP_ST_ONLINE||r.status==EDEN_MP_ST_CACHED||r.status==EDEN_MP_ST_PROBING)
        sub=r.motd[0]?r.motd:hp;
    else sub=sl_statusWord(r.status);
    char players[32]="-", ping[16]="-";
    if(r.players>=0&&r.max>=0) std::snprintf(players,sizeof(players),"%d/%d",r.players,r.max);
    else if(r.players>=0) std::snprintf(players,sizeof(players),"%d",r.players);
    else if(r.status==EDEN_MP_ST_FULL&&r.max>0) std::snprintf(players,sizeof(players),"%d/%d",r.max,r.max);
    if(r.pingMs>=0&&r.status!=EDEN_MP_ST_PROBING) std::snprintf(ping,sizeof(ping),"%d ms",r.pingMs);
    const std::string title=displayName(r);
    char key[1024];
    std::snprintf(key,sizeof(key),"%s|%s|%s|%s|%.3f|%.1f",title.c_str(),sub.c_str(),players,ping,u(),m_list.size.width);
    if(ui.key==key) return;
    ui.key=key;
    const float textW=m_list.size.width-du(18)-du(6)-du(20)-sl_lockW()-sl_pingW()-sl_playersW();
    sl_fit(ui.title,title,du(22),textW,FACE_DISPLAY);
    sl_fit(ui.sub,sub,du(14),m_list.size.width-du(18)-du(6)-du(24),FACE_BODY);
    ui.players.set(players,du(20),UITextAlignmentLeft);
    ui.ping.set(ping,du(16),UITextAlignmentLeft);
    if(ui.q.empty()) ui.q.set("?",du(16),UITextAlignmentLeft);
}

void ServerListMenu::buildPane(){
    using namespace GLW;
    if(m_sel<0||m_sel>=(int)m_rows.size()){ m_paneN=0; m_paneKey.clear(); return; }
    const EdenMpServerRow& r=m_rows[m_sel];
    std::vector<std::string> L;
    std::vector<int> face;   // 1 = display title
    L.push_back(displayName(r)); face.push_back(1);
    char b[400];
    std::snprintf(b,sizeof(b),"%s:%d",r.host,r.port); L.push_back(b); face.push_back(0);
    if(m_tab==1){   // MP.8: a matchmaker row knows only the players, the lock and who listed it
        if(r.players>=0){ std::snprintf(b,sizeof(b),"%d playing",r.players); L.push_back(b); face.push_back(0); }
        L.push_back(r.locked==1?"Locked":r.locked==0?"Open":"Lock not known"); face.push_back(0);
        L.push_back(std::string("Listed ")+r.motd); face.push_back(0);
        L.push_back("Save it to My servers to see its details."); face.push_back(0);
    }else if(r.status==EDEN_MP_ST_ONLINE){
        if(r.players>=0&&r.max>=0) std::snprintf(b,sizeof(b),"Online, %d of %d playing",r.players,r.max);
        else std::snprintf(b,sizeof(b),"Online");
        L.push_back(b);
    }else if(r.status==EDEN_MP_ST_PROBING||r.status==EDEN_MP_ST_CACHED) L.push_back("Checking...");
    else L.push_back(sl_statusWord(r.status));
    if(m_tab==0) face.push_back(0);
    if(m_tab==0&&r.pingMs>=0){ std::snprintf(b,sizeof(b),"Ping %d ms",r.pingMs); L.push_back(b); face.push_back(0); }
    if(m_tab==0&&r.status!=EDEN_MP_ST_UNKNOWN_HOST){
        L.push_back(r.locked==1?(r.hasPassword?"Locked (password saved)":"Locked"):r.locked==0?"Open":"Lock not known yet");
        face.push_back(0);
    }
    std::string fl;
    for(const char* c=r.flags;*c;c++){
        const char* w=*c=='t'?"TNT off":*c=='f'?"Fire off":*c=='z'?"Protected areas":*c=='p'?"PvP on":NULL;
        if(!w) continue;
        if(!fl.empty()) fl+=", ";
        fl+=w;
    }
    if(m_tab==0&&!fl.empty()){ L.push_back(fl); face.push_back(0); }
    if(m_tab==0&&r.motd[0]){ L.push_back(std::string("\"")+r.motd+"\""); face.push_back(0); }
    if(m_tab==0&&r.fromCache&&r.seen>0){
        const long long ago=(long long)time(NULL)-r.seen;
        if(ago<120) std::snprintf(b,sizeof(b),"Last seen just now");
        else if(ago<7200) std::snprintf(b,sizeof(b),"Last seen %lld min ago",ago/60);
        else if(ago<172800) std::snprintf(b,sizeof(b),"Last seen %lld h ago",ago/3600);
        else std::snprintf(b,sizeof(b),"Last seen %lld days ago",ago/86400);
        L.push_back(b); face.push_back(0);
    }
    std::string key;
    for(const std::string& s:L){ key+=s; key+='\n'; }
    char uk[48];
    std::snprintf(uk,sizeof(uk),"%.3f|%.1f",u(),m_paneRect.size.width);
    key+=uk;
    if(key==m_paneKey) return;
    m_paneKey=key;
    m_paneN=std::min((int)L.size(),8);
    const float w=m_paneRect.size.width-du(16);
    for(int i=0;i<m_paneN;i++){
        if(face[i]) m_paneLines[i].set(L[i].c_str(),du(24),UITextAlignmentLeft,w,FACE_DISPLAY);
        else m_paneLines[i].set(L[i].c_str(),du(14),UITextAlignmentLeft,w,FACE_BODY);
    }
}

void ServerListMenu::layout(){
    using namespace GLW;
    if(!m_built||m_builtU!=u()){
        m_built=true;
        m_builtU=u();
        m_title.set("Multiplayer",du(32),UITextAlignmentCenter);
        m_back.setLabel("Back",du(20)); m_back.sizeToFit();
        m_add.setLabel("+ Add",du(20)); m_add.sizeToFit();
        m_edit.setLabel("Edit",du(20)); m_edit.sizeToFit();
        m_refresh.setLabel("Refresh",du(20)); m_refresh.sizeToFit();
        m_join.setLabel("Join",du(20)); m_join.setIcon(UIICON_PLAY); m_join.setTone(GLW::Button::TONE_POSITIVE); m_join.sizeToFit();
        m_emptyAdd.setLabel("+ Add server",du(20)); m_emptyAdd.sizeToFit();
        m_empty.set("No servers yet. Add one by its address.",du(16),UITextAlignmentCenter,0,FACE_BODY);
        m_cancel.setLabel("Cancel",du(20));
        m_save.setLabel("Save",du(20)); m_save.sizeToFit();
        static const char* const kTabs[]={"My servers","Matchmakers"};
        m_tabs.setTabs(kTabs,2,du(18));
        m_profileName.clear();
        for(int k=0;k<SL_MAX_VISIBLE;k++) m_rowUi[k].key.clear();
        m_paneKey.clear();
    }
    // The rows' data (cheap: plain structs).
    m_mmOn=eden_mp_matchmaker_count()>0;
    if(!m_mmOn&&m_tab==1) m_tab=0;
    const int n=count();
    m_rows.resize(n);
    for(int i=0;i<n;i++){
        if(m_tab==1) eden_mp_matchmaker_get(i,&m_rows[i]);
        else eden_mp_servers_get(i,&m_rows[i]);
    }
    if(m_sel>=n) m_sel=n-1;
    if(m_sel<0&&n>0) m_sel=0;

    char nm[32];
    std::string prof=eden_mp_profile_name(nm,sizeof(nm))?std::string("Playing as ")+nm:std::string("Set your name");
    if(prof!=m_profileName){ m_profileName=prof; m_profile.setLabel(prof.c_str(),du(20)); m_profile.sizeToFit(); }

    const RailFrame f=railFrame();
    m_panel=f.panel;
    const CGRect& T=f.titlebar;
    m_back.setRect(CGRectMake(T.origin.x,T.origin.y,0,T.size.height));
    titlebarHit(m_back);
    m_add.setRect(CGRectMake(T.origin.x+T.size.width-m_add.fitWidth(),T.origin.y,0,T.size.height));
    titlebarHit(m_add);
    m_add.setEnabled(eden_mp_servers_count()<eden_mp_servers_cap());
    // MP.8: the tabs take the title's place between Back and Add.
    {
        const float x0=m_back.rect().origin.x+m_back.rect().size.width+du(10);
        const float x1=m_add.rect().origin.x-du(10);
        const float w=std::min((float)du(330),x1-x0);
        m_tabs.setRect(CGRectMake(x0+(x1-x0-w)*0.5f,T.origin.y,w,T.size.height));
        m_tabs.setSelected(m_tab);
        m_tabs.setHeld(m_tabHeld);
    }
    m_titleY=f.titleY(m_title);

    const float left=f.rail.origin.x, right=f.bar.origin.x+f.bar.size.width;
    const float barH=controlH(30), gap=du(6);
    m_barRect=CGRectMake(left,f.content.origin.y,right-left,barH);
    const CGRect area=CGRectMake(left,f.content.origin.y+barH+gap,right-left,f.content.size.height-barH-gap);
    // The pane needs height for its lines and width beside a full row (UI plan §2.1): the desktop
    // and the iPad keep it, a phone's 320-point landscape does not.
    m_pane=area.size.height>=300.0f&&area.size.width>=du(560);
    const float paneW=m_pane?std::max(du(220),(float)(area.size.width*0.36f)):0.0f;
    m_paneRect=CGRectMake(area.origin.x+area.size.width-paneW,area.origin.y,paneW,area.size.height);
    m_list=CGRectMake(area.origin.x,area.origin.y,area.size.width-(m_pane?paneW+gap:0.0f),area.size.height);

    m_rowH=std::max(touchFloor(),du(48));
    int vis=(int)std::floor(m_list.size.height/m_rowH);
    if(vis<1) vis=1;
    if(vis>SL_MAX_VISIBLE) vis=SL_MAX_VISIBLE;
    m_scroll.setRows(n,vis);
    m_scroll.setPitch(m_rowH);
    const float barW=du(18);
    m_scroll.setBarRect(CGRectMake(m_list.origin.x+m_list.size.width-barW,m_list.origin.y,barW,m_list.size.height));
    for(int s=0;s<vis;s++){
        m_rowUi[s].rect=CGRectMake(m_list.origin.x,m_list.origin.y+m_list.size.height-(s+1)*m_rowH,
                                   m_list.size.width-barW-du(4),m_rowH);
        const int idx=m_scroll.first()+s;
        if(idx<n) buildRow(s,idx);
    }
    if(m_pane) buildPane();

    // Action bar: who you are at the left, Edit · Refresh · Join at the right.
    const float by=m_barRect.origin.y;
    m_profile.setMaxWidth(std::max((float)du(80),right-left-m_join.fitWidth()-m_refresh.fitWidth()-m_edit.fitWidth()-du(24)));
    m_profile.setRect(CGRectMake(left,by,0,barH));
    float x=right-m_join.fitWidth();
    m_join.setRect(CGRectMake(x,by,0,barH));
    x-=du(6)+m_refresh.fitWidth();
    m_refresh.setRect(CGRectMake(x,by,0,barH));
    x-=du(6)+m_edit.fitWidth();
    m_edit.setRect(CGRectMake(x,by,0,barH));
    const bool hasSel=m_sel>=0&&m_sel<n;
    m_edit.setEnabled(hasSel&&m_tab==0);
    m_save.setRect(m_edit.rect());
    m_save.setEnabled(hasSel&&m_tab==1);
    m_refresh.setEnabled(m_tab==1?m_mmPending==0:(n>0&&m_pending==0));
    m_join.setEnabled(hasSel&&m_rows[m_sel].status!=EDEN_MP_ST_UNKNOWN_HOST);
    m_emptyAdd.setRect(CGRectMake(m_list.origin.x+(m_list.size.width-m_emptyAdd.fitWidth())*0.5f,
                                  m_list.origin.y+m_list.size.height*0.5f-barH-du(6),0,barH));

    // The Connecting window: the dialog's width, centred in the safe rect, sized in render.
    const CGRect safe=safeRect();
    const float cw=std::min(du(440),(float)safe.size.width-du(24)*2.0f);
    m_cPanel.size.width=cw;
    m_cPanel.origin.x=safe.origin.x+(safe.size.width-cw)*0.5f;
}

// ---------------------------------------------------------------------------------------------
// the join
// ---------------------------------------------------------------------------------------------
void ServerListMenu::join(int row){
    if(row<0||row>=(int)m_rows.size()||m_connecting) return;
    const EdenMpServerRow& r=m_rows[row];
    if(r.status==EDEN_MP_ST_UNKNOWN_HOST) return;
    Pending p;
    p.row=row;
    p.host=r.host;
    p.port=r.port;
    p.label=displayName(r);
    p.locked=r.locked;
    if(m_tab==1){   // MP.8: a listed server is a direct connect (Save adds it to the list)
        p.row=-1;
        p.remember=false;
    }else{
        char pw[80];
        eden_mp_servers_password(row,pw,sizeof(pw));
        p.password=pw;
        p.remember=r.hasPassword!=0;
    }
    m_join_=p;
    continueJoin();
}

void ServerListMenu::continueJoin(){
    char nm[32];
    if(!eden_mp_profile_name(nm,sizeof(nm))){ m_form->openProfile(true); return; }   // UI plan §4: the first Join
    if(m_join_.locked==1&&m_join_.password.empty()){
        promptPassword(false);   // known locked, nothing saved: ask before connecting (§5)
        return;
    }
    startJoin(m_join_);
}

void ServerListMenu::startJoin(const Pending& p){
    char nm[32];
    eden_mp_profile_name(nm,sizeof(nm));
    m_join_=p;
    if(!eden_mp_join(p.host.c_str(),p.port,nm,eden_mp_profile_character(),p.password.c_str())) return;
    m_connecting=true;
    m_debugConn=false;
    m_lockLearnt=false;
    eden_mp_join_state(&m_js);
}

void ServerListMenu::promptPassword(bool again){
    char title[128];
    std::snprintf(title,sizeof(title),"%s is locked",m_join_.label.c_str());
    static const char* const kSaved[]={"Join","Join and remember","Cancel"};
    static const char* const kDirect[]={"Join","Cancel"};
    const bool saved=m_join_.row>=0;
    GLDialog::prompt(title,again?"Wrong password. Try again.":"Enter the server password to join.","",64,
                     saved?kSaved:kDirect,saved?3:2,passwordCb);
    GLDialog::setPromptSecure(true);
}

void ServerListMenu::passwordCb(int chosen,const char* text){
    ServerListMenu* s=s_self;
    if(!s) return;
    const bool saved=s->m_join_.row>=0;
    if(chosen==(saved?2:1)) return;   // Cancel / Escape
    if(!text||!*text||std::strchr(text,':')){ s->promptPassword(true); return; }
    s->m_join_.password=text;
    if(saved&&chosen==1){   // Join and remember: overwrites a saved one that failed (§5)
        const EdenMpServerRow& r=s->m_rows[s->m_join_.row];
        eden_mp_servers_update(s->m_join_.row,r.label,r.host,r.port,text);
        eden_mp_servers_save();
        s->m_join_.remember=true;
    }
    s->startJoin(s->m_join_);
}

// UI plan §7: one dialog per failure. The button set depends on the cause.
static int s_errKind=0;   // 0 Retry·Edit/Save·Back, 1 Retry·Back, 2 Change name·Back

void ServerListMenu::showError(int err,const char* text){
    char title[128];
    std::snprintf(title,sizeof(title),"Couldn't join %s",m_join_.label.c_str());
    static const char* const kEdit[]={"Retry","Edit server","Back"};
    static const char* const kSave[]={"Retry","Save server","Back"};
    static const char* const kRetry[]={"Retry","Back"};
    static const char* const kName[]={"Change name","Back"};
    if(err==EDEN_MP_ERR_INVALID_NAME){
        s_errKind=2;
        GLDialog::show(title,text,kName,2,errorCb);
    }else if(err==EDEN_MP_ERR_DNS||err==EDEN_MP_ERR_REFUSED||err==EDEN_MP_ERR_TIMEOUT||err==EDEN_MP_ERR_CONNECT_FAILED){
        s_errKind=0;
        GLDialog::show(title,text,m_join_.row>=0?kEdit:kSave,3,errorCb);
    }else{
        s_errKind=1;
        GLDialog::show(title,text,kRetry,2,errorCb);
    }
}

void ServerListMenu::errorCb(int chosen){
    ServerListMenu* s=s_self;
    if(!s) return;
    if(s_errKind==2){ if(chosen==0) s->m_form->openProfile(false); return; }
    if(chosen==0){ s->startJoin(s->m_join_); return; }
    if(s_errKind!=0||chosen!=1) return;
    if(s->m_join_.row>=0){ s->m_form->openEdit(s->m_join_.row); return; }
    // A direct connect's "Save server": into the list as typed, selected.
    const int i=eden_mp_servers_add(s->m_join_.label==s->m_join_.host?"":s->m_join_.label.c_str(),s->m_join_.host.c_str(),
                                    s->m_join_.port,s->m_join_.remember?s->m_join_.password.c_str():"");
    if(i>=0){
        eden_mp_servers_save();
        eden_mp_servers_refresh(i);
        s->layout();
        s->select(i);
    }
}

void ServerListMenu::pollJoin(){
    if(m_debugConn) return;
    const int st=eden_mp_join_state(&m_js);
    if(!m_connecting){
        // A join this screen did not start (Reconnect from the Disconnected dialog, --mp-join): show it.
        if(st>=EDEN_MP_JOIN_CONNECTING&&st<=EDEN_MP_JOIN_LOADING){
            m_connecting=true;
            m_lockLearnt=false;
            if(m_join_.host!=m_js.host||m_join_.port!=m_js.port){
                m_join_.row=eden_mp_servers_find(m_js.host,m_js.port,-1);
                m_join_.host=m_js.host;
                m_join_.port=m_js.port;
                m_join_.label=m_join_.row>=0?displayName(m_rows[m_join_.row]):std::string(m_js.host);
                m_join_.password.clear();
                m_join_.remember=false;
            }
        }
        return;
    }
    // Learnt "locked" (list plan §5): a join that got through says whether a password was needed.
    if((st==EDEN_MP_JOIN_LOADING||st==EDEN_MP_JOIN_PLAYING)&&!m_lockLearnt){
        m_lockLearnt=true;
        if(m_join_.row>=0){
            eden_mp_servers_set_locked(m_join_.row,m_js.usedPassword?1:0);
            eden_mp_servers_save();
        }
    }
    if(st==EDEN_MP_JOIN_PLAYING||st==EDEN_MP_JOIN_IDLE){ m_connecting=false; return; }
    if(st!=EDEN_MP_JOIN_FAILED) return;
    eden_mp_join_ack();
    m_connecting=false;
    if(m_js.err==EDEN_MP_ERR_WRONG_PASSWORD){
        if(m_join_.row>=0){ eden_mp_servers_set_locked(m_join_.row,1); eden_mp_servers_save(); }
        promptPassword(!m_join_.password.empty());
        return;
    }
    showError(m_js.err,m_js.text);
}

void ServerListMenu::formClosed(int outcome,int row,bool thenJoin){
    switch(outcome){
        case ServerFormMenu::OUT_SAVED:
            layout();
            select(row);
            break;
        case ServerFormMenu::OUT_DELETED:
            eden_mp_servers_remove(row);
            eden_mp_servers_save();
            layout();
            m_sel=std::min(row,eden_mp_servers_count()-1);
            break;
        case ServerFormMenu::OUT_JOIN_NOW: {   // direct connect: saves nothing (list plan §6)
            Pending p;
            p.row=-1;
            p.host=m_form->joinHost;
            p.port=m_form->joinPort;
            p.label=m_form->joinLabel;
            p.password=m_form->joinPassword;
            p.remember=m_form->joinRemember;
            m_join_=p;
            char nm[32];
            if(!eden_mp_profile_name(nm,sizeof(nm))){ m_form->openProfile(true); break; }
            startJoin(p);
            break;
        }
        case ServerFormMenu::OUT_PROFILE_SAVED:
            if(thenJoin) continueJoin();
            break;
        default: break;
    }
}

// ---------------------------------------------------------------------------------------------
// input
// ---------------------------------------------------------------------------------------------
void ServerListMenu::update(float etime){
    if(!m_open) return;
    m_phase+=etime;
    m_pending=eden_mp_servers_poll();
    m_mmPending=eden_mp_matchmakers_poll();
    if(m_form->isOpen()){ m_form->update(etime); return; }
    layout();
    pollJoin();
    Input* input=Input::getInput();
    itouch* touches=input->getTouches();
    if(m_connecting){
        // The Connecting window: Cancel only (not while the world loads: §6).
        m_cancel.setEnabled(m_js.stage<EDEN_MP_JOIN_LOADING);
        for(int i=0;i<MAX_TOUCHES;i++){
            if(touches[i].inuse==0&&touches[i].down==M_DOWN){
                touches[i].inuse=usage_id;
                m_cancel.setPressed(m_cancel.enabled()&&m_cancel.hit(touches[i].mx,touches[i].my));
            }
            if(touches[i].inuse==usage_id&&touches[i].down==M_RELEASE){
                const bool hit=m_cancel.pressed()&&m_cancel.hit(touches[i].mx,touches[i].my);
                touches[i].inuse=0;
                touches[i].down=M_NONE;
                m_cancel.setPressed(false);
                if(hit){ if(!m_debugConn) eden_mp_join_cancel(); m_connecting=m_debugConn=false; }
            }
        }
        return;
    }
    if(int wheel=eden_ui_take_wheel()) m_scroll.scrollBy(-wheel);
    const int n=(int)m_rows.size();
    GLW::Button* const btns[]={&m_back,&m_add,&m_edit,&m_save,&m_refresh,&m_join,&m_profile,&m_emptyAdd};
    for(int i=0;i<MAX_TOUCHES;i++){
        if(touches[i].inuse==0&&touches[i].down==M_DOWN){
            touches[i].inuse=usage_id;
            const float mx=touches[i].mx, my=touches[i].my;
            for(GLW::Button* b:btns) b->setPressed(b->enabled()&&b->hit(mx,my)&&(b!=&m_emptyAdd||(n==0&&m_tab==0)));
            m_tabHeld=m_mmOn?m_tabs.hit(mx,my):-1;
            if(m_barTouch<0&&m_scroll.hitBar(mx,my)){
                m_barTouch=i;
                m_scroll.barBegin(my);
            }else if(m_listTouch<0&&n>0&&inbox(mx,my,m_list)&&!(m_pane&&inbox(mx,my,m_paneRect))){
                m_listTouch=i;
                m_scroll.beginContentDrag(my);
                m_downRow=-1;
                for(int s=0;s<m_scroll.visible();s++)
                    if(m_scroll.first()+s<n&&inbox(mx,my,m_rowUi[s].rect)) m_downRow=m_scroll.first()+s;
            }
        }
        if(touches[i].inuse==usage_id&&touches[i].down==M_DOWN){
            if(i==m_barTouch) m_scroll.barDragTo(touches[i].my);
            if(i==m_listTouch&&m_scroll.contentDragTo(touches[i].my)) m_downRow=-1;
        }
        if(touches[i].inuse==usage_id&&touches[i].down==M_RELEASE){
            const float mx=touches[i].mx, my=touches[i].my;
            touches[i].inuse=0;
            touches[i].down=M_NONE;
            GLW::Button* chosen=NULL;
            for(GLW::Button* b:btns){ if(b->pressed()&&b->hit(mx,my)) chosen=b; b->setPressed(false); }
            if(m_tabHeld>=0){
                const int t=m_tabs.hit(mx,my);
                const int held=m_tabHeld;
                m_tabHeld=-1;
                if(t==held&&i!=m_listTouch&&i!=m_barTouch){ showTab(t); continue; }
            }
            if(i==m_barTouch){ m_scroll.endDrag(); m_barTouch=-1; continue; }
            if(i==m_listTouch){
                const bool scrolled=m_scroll.scrolling();
                m_scroll.endDrag();
                m_listTouch=-1;
                const int row=m_downRow;
                m_downRow=-1;
                if(!scrolled&&row>=0){
                    const int s=row-m_scroll.first();
                    if(s>=0&&s<m_scroll.visible()&&inbox(mx,my,m_rowUi[s].rect)){
                        if(row==m_sel) join(row);   // a tap on the selected row joins (the lists' rule)
                        else m_sel=row;
                    }
                }
                continue;
            }
            if(!chosen) continue;
            if(chosen==&m_back){ back(); return; }
            if(chosen==&m_add||chosen==&m_emptyAdd){ m_form->openAdd(); return; }
            if(chosen==&m_edit&&m_sel>=0){ m_form->openEdit(m_sel); return; }
            if(chosen==&m_save&&m_sel>=0){
                const int idx=eden_mp_matchmaker_save(m_sel);
                if(idx>=0){ showTab(0); layout(); select(idx); }
                continue;
            }
            if(chosen==&m_refresh){ if(m_tab==1) eden_mp_matchmakers_fetch(); else eden_mp_servers_refresh(-1); continue; }
            if(chosen==&m_join&&m_sel>=0){ join(m_sel); return; }
            if(chosen==&m_profile){ m_form->openProfile(false); return; }
        }
    }
}

// ---------------------------------------------------------------------------------------------
// render
// ---------------------------------------------------------------------------------------------
static void sl_pingBars(float x,float y,float h,int ms,const GLW::Color& on,const GLW::Color& off){
    using namespace GLW;
    const int bars=ms<0?0:ms<80?3:ms<200?2:ms<500?1:0;   // Minecraft's reading (UI plan §2.2)
    const float bw=du(4), g=du(2);
    for(int b=0;b<3;b++){
        const float bh=h*(0.4f+0.3f*b);
        fill(x+b*(bw+g),y,bw,bh,b<bars?on:off);
    }
}

void ServerListMenu::renderConnecting(){
    using namespace GLW;
    const CGRect safe=safeRect();
    const float pad=du(16);
    const float w=m_cPanel.size.width, inner=w-pad*2.0f;
    // Text (rebuilt only on change).
    std::string title=m_join_.label.empty()?std::string(m_js.host):m_join_.label;
    if(title!=m_cTitleKey){ m_cTitleKey=title; m_cTitle.set(title.c_str(),du(26),UITextAlignmentCenter,inner); }
    const char* stage=m_js.stage==EDEN_MP_JOIN_CONNECTING?"Connecting...":m_js.stage==EDEN_MP_JOIN_JOINING?"Joining..."
                     :m_js.stage==EDEN_MP_JOIN_RECEIVING?"Receiving world...":m_js.stage==EDEN_MP_JOIN_LOADING?"Loading world...":"";
    char sl[64];
    if(m_js.progress>=0.0f&&m_js.stage>=EDEN_MP_JOIN_RECEIVING) std::snprintf(sl,sizeof(sl),"%s  %d%%",stage,(int)(m_js.progress*100.0f+0.5f));
    else std::snprintf(sl,sizeof(sl),"%s",stage);
    if(m_cStageKey!=sl){ m_cStageKey=sl; m_cStage.set(sl,du(18),UITextAlignmentLeft); }
    // The welcome / MOTD lines (§6), plus the row's flags, oldest dropped to fit the safe rect.
    std::vector<std::string> lines;
    char b[256];
    for(int i=0;i<m_js.lines&&i<8;i++) if(m_debugConn?false:eden_mp_join_line(i,b,sizeof(b))) lines.push_back(b);
    if(m_debugConn) for(int i=0;i<m_cN;i++) lines.push_back(m_cLineKey[i]);
    if(m_join_.row>=0&&m_join_.row<(int)m_rows.size()){
        std::string fl;
        for(const char* c=m_rows[m_join_.row].flags;*c;c++){
            const char* t=*c=='t'?"TNT off":*c=='f'?"Fire off":*c=='z'?"Protected areas":*c=='p'?"PvP on":NULL;
            if(t){ if(!fl.empty()) fl+=", "; fl+=t; }
        }
        if(!fl.empty()) lines.push_back(fl);
    }
    const float btnH=controlH(30), barH=du(14);
    const float fixed=pad+m_cTitle.height()+du(10)+m_cStage.height()+du(6)+barH+du(10)+btnH+pad;
    float room=safe.size.height-du(24)-fixed;
    // Line labels: rebuilt per slot when their text changes.
    static GLW::Label s_lines[9];
    static std::string s_keys[9];
    int shown=0;
    float lh=0;
    const int first=std::max(0,(int)lines.size()-9);
    std::vector<int> idx;
    for(int i=(int)lines.size()-1;i>=first;i--){
        const int slot=i-first;
        if(s_keys[slot]!=lines[i]){ s_keys[slot]=lines[i]; s_lines[slot].set(lines[i].c_str(),du(14),UITextAlignmentLeft,inner,FACE_BODY); }
        if(lh+s_lines[slot].height()+du(2)>room) break;
        lh+=s_lines[slot].height()+du(2);
        idx.insert(idx.begin(),slot);
        shown++;
    }
    const float h=fixed+lh;
    m_cPanel.size.height=h;
    m_cPanel.origin.y=safe.origin.y+(safe.size.height-h)*0.5f;
    const float x=m_cPanel.origin.x+pad;
    float y=m_cPanel.origin.y+h-pad;

    bevel(m_cPanel,BEVEL_WINDOW);
    m_cTitle.drawCentered(m_cPanel.origin.x+w*0.5f,y,kText);
    y-=m_cTitle.height()+du(10);
    m_cStage.draw(x,y,kText);
    y-=m_cStage.height()+du(6);
    progressBar(CGRectMake(x,y-barH,inner,barH),m_js.stage>=EDEN_MP_JOIN_RECEIVING?m_js.progress:-1.0f,m_phase*0.8f);
    y-=barH+du(10);
    for(int s:idx){ s_lines[s].draw(x,y,kTextSecondary); y-=s_lines[s].height()+du(2); }
    m_cancel.setPointSize(du(20));
    m_cancel.setRect(CGRectMake(m_cPanel.origin.x+(w-du(140))*0.5f,m_cPanel.origin.y+pad,du(140),btnH));
    m_cancel.render();
}

void ServerListMenu::render(){
    if(!m_open) return;
    if(m_form->isOpen()){ m_form->render(); return; }
    layout();

    GLW::begin2D();

    using namespace GLW;
    if(m_connecting){
        // Over the menu backdrop, not the list (UI plan §6).
        renderConnecting();
    }else{
        fill(CGRectMake(0,0,SCREEN_WIDTH,SCREEN_HEIGHT),kScrim);
        bevel(m_panel,BEVEL_WINDOW);
        m_back.render();
        m_add.render();
        const float cx=m_panel.origin.x+m_panel.size.width*0.5f;
        const float room=std::min(m_add.rect().origin.x-cx,cx-(m_back.rect().origin.x+m_back.rect().size.width))-du(6);
        if(m_mmOn) m_tabs.render();
        else if(m_title.width()*0.5f<=room) m_title.drawCentered(cx,m_titleY,kText);

        bevel(m_list,BEVEL_CONTENT);
        const int n=(int)m_rows.size();
        if(n==0&&m_tab==0){
            m_empty.drawCentered(m_list.origin.x+m_list.size.width*0.5f,m_list.origin.y+m_list.size.height*0.5f+du(24),kTextSecondary);
            m_emptyAdd.render();
        }else if(n==0){   // MP.8: asking, or nothing listed, or every matchmaker failed
            char err[160];
            std::string t=m_mmPending>0?"Asking matchmakers...":"No servers are listed right now.";
            if(m_mmPending==0&&eden_mp_matchmaker_error(err,sizeof(err))) t=std::string(err)+".";
            if(t!=m_mmEmptyKey){ m_mmEmptyKey=t; m_emptyMm.set(t.c_str(),du(16),UITextAlignmentCenter,m_list.size.width-du(24),FACE_BODY); }
            m_emptyMm.drawCentered(m_list.origin.x+m_list.size.width*0.5f,m_list.origin.y+m_list.size.height*0.5f+m_emptyMm.height()*0.5f,kTextSecondary);
        }
        const int vis=m_scroll.visible();
        for(int s=0;s<vis&&m_scroll.first()+s<n;s++){
            const int idx=m_scroll.first()+s;
            const EdenMpServerRow& r=m_rows[idx];
            const RowUi& ui=m_rowUi[s];
            const CGRect R=ui.rect;
            if(idx==m_sel){ fill(R,kSelTint); fill(R.origin.x,R.origin.y,4*u(),R.size.height,kSelBar); }
            if(s<vis-1&&idx<n-1) fill(R.origin.x,R.origin.y,R.size.width,u(),rgb(0x000000,0.28f));
            const bool dim=r.status==EDEN_MP_ST_CACHED||r.status==EDEN_MP_ST_PROBING;
            const float tx=R.origin.x+du(10);
            const float topY=R.origin.y+R.size.height-du(5);
            ui.title.draw(tx,topY,kText);
            const Color subC=dim?kTextMuted:r.status==EDEN_MP_ST_FULL?kTextWarn:r.status==EDEN_MP_ST_UNKNOWN_HOST?kTextError
                            :(r.status==EDEN_MP_ST_NOT_RUNNING||r.status==EDEN_MP_ST_NO_ANSWER)?kTextMuted:kTextSecondary;
            ui.sub.draw(tx+du(14),R.origin.y+du(5)+ui.sub.height(),subC);
            // The right column on the title's line: players, ping bars + ms, the lock.
            const Color rc=dim?kTextMuted:kText;
            const float lineMid=topY-ui.title.height()*0.5f;
            float x=R.origin.x+R.size.width-du(6)-sl_lockW();
            if(r.status!=EDEN_MP_ST_UNKNOWN_HOST){
                const float isz=du(18);
                const int id=r.locked==1?UIICON_LOCK:UIICON_LOCK_OPEN;
                icon(id,CGRectMake(x,lineMid-isz*0.5f,isz,isz),rc);
                if(r.locked<0) ui.q.draw(x+isz,lineMid+ui.q.height()*0.5f,rc);
            }
            x-=sl_pingW();
            if(m_tab==1){}   // a matchmaker gives no ping (UI plan §2.3)
            else if(r.status==EDEN_MP_ST_PROBING) progressBar(CGRectMake(x,lineMid-du(5),sl_pingW()-du(12),du(10)),-1.0f,m_phase*0.8f+idx*0.13f);
            else{
                sl_pingBars(x,lineMid-du(8),du(16),r.pingMs,rc,rgb(0x000000,0.15f));
                ui.ping.draw(x+du(20),lineMid+ui.ping.height()*0.5f,rc);
            }
            x-=sl_playersW();
            ui.players.draw(x,lineMid+ui.players.height()*0.5f,rc);
        }
        m_scroll.render();

        if(m_pane){
            bevel(m_paneRect,BEVEL_CONTENT);
            float y=m_paneRect.origin.y+m_paneRect.size.height-du(8);
            for(int i=0;i<m_paneN;i++){
                if(y-m_paneLines[i].height()<m_paneRect.origin.y) break;
                m_paneLines[i].draw(m_paneRect.origin.x+du(8),y,i==0?kText:kTextSecondary);
                y-=m_paneLines[i].height()+du(4);
            }
        }
        m_profile.render();
        if(m_tab==1) m_save.render();
        else m_edit.render();
        m_refresh.render();
        m_join.render();
    }

    GLW::end2D();
}

// ---------------------------------------------------------------------------------------------
// harness surface
// ---------------------------------------------------------------------------------------------
CGRect ServerListMenu::controlRect(const char* which){
    layout();
    if(!which) return CGRectMake(0,0,0,0);
    if(!std::strcmp(which,"back")) return m_back.rect();
    if(!std::strcmp(which,"add")) return m_add.rect();
    if(!std::strcmp(which,"edit")) return m_edit.rect();
    if(!std::strcmp(which,"save")) return m_save.rect();
    if(!std::strcmp(which,"tabs")) return m_tabs.rect();
    if(!std::strcmp(which,"tab0")) return m_tabs.tabRect(0);
    if(!std::strcmp(which,"tab1")) return m_tabs.tabRect(1);
    if(!std::strcmp(which,"refresh")) return m_refresh.rect();
    if(!std::strcmp(which,"join")) return m_join.rect();
    if(!std::strcmp(which,"profile")) return m_profile.rect();
    if(!std::strcmp(which,"emptyadd")) return m_emptyAdd.rect();
    if(!std::strcmp(which,"list")) return m_list;
    if(!std::strcmp(which,"pane")) return m_paneRect;
    if(!std::strcmp(which,"cancel")) return m_cancel.rect();
    if(!std::strcmp(which,"connecting")) return m_cPanel;
    return CGRectMake(0,0,0,0);
}

bool ServerListMenu::rowRect(int index,CGRect* r){
    layout();
    const int s=index-m_scroll.first();
    if(index<0||index>=(int)m_rows.size()||s<0||s>=m_scroll.visible()) return false;
    if(r) *r=m_rowUi[s].rect;
    return true;
}

void ServerListMenu::debugConnecting(int stage,float progress,const char* const* lines,int n){
    m_connecting=true;
    m_debugConn=true;
    m_js.stage=stage;
    m_js.progress=progress;
    m_cN=std::min(n,8);
    for(int i=0;i<m_cN;i++) m_cLineKey[i]=lines[i];
    if(m_join_.label.empty()&&m_sel>=0&&m_sel<(int)m_rows.size()){
        m_join_.row=m_sel;
        m_join_.label=displayName(m_rows[m_sel]);
    }
}

void ServerListMenu::debugError(int err,const char* text){
    m_connecting=m_debugConn=false;
    if(m_sel>=0&&m_sel<(int)m_rows.size()){ m_join_.row=m_sel; m_join_.label=displayName(m_rows[m_sel]); }
    showError(err,text);
}

void ServerListMenu::debugPassword(bool again){
    if(m_sel>=0&&m_sel<(int)m_rows.size()){ m_join_.row=m_sel; m_join_.label=displayName(m_rows[m_sel]); }
    promptPassword(again);
}
