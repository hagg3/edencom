//
//  ConsoleMenu.mm
//  Eden
//
//  Stage C / C.3: see ConsoleMenu.h. The shape is SettingsMenu's (a railFrame panel, the kit's rail,
//  a row-snapped ScrollView, one touch claim) with three differences: the world keeps running behind
//  it, it owns a TextField that stays focused, and the scrollback is lines of text, not settings rows.
//
//  COST. A scrollback line is one Label (a texture per wrapped line), built the first frame the Console
//  or strip shows it and kept, keyed by Console::scrollbackSerial(), until it falls out of the 200 kept
//  -- never rebuilt per frame (docs/ui.md). The Variables and Help tabs build their rows (about 130
//  TwoLineRows) the first time they are shown and keep them until the screen's width or u() changes.
//

#import "ConsoleMenu.h"
#import "Console.h"
#import "CmdScript.h"
#import "Graphics.h"
#import "Globals.h"
#import "Input.h"
#import "Util.h"
#import "World.h"
#import "ChatFeed.h"     // MP.7: the Chat tab
#import "MpServers.h"    // eden_mp_profile_name (the Players tab's own row)
#import "Player.h"
#import "MpWorld.h"
#import "UIIcons.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <strings.h>

static const int usage_id=17;   // distinct from Menu (7), SettingsMenu (3), GLDialog (9), Player (10), Keybinds (11), WorldBrowser (12), NewWorld (13), WorldProperties (14), Hud (15, 16)

enum { CB_NONE=0, CB_BACK, CB_COLLAPSE, CB_RUN, CB_TAB, CB_UP, CB_DOWN, CB_PILL };

struct ConsoleMenu::Cached {
    GLW::Label label;
    int   kind;
    float height;
};

struct ConsoleMenu::InfoRow {
    GLW::TwoLineRow row;
    std::string title, hint;
    std::string insert;      // what a tap puts in the input
    float height;
    InfoRow() : height(0) {}
};

using namespace GLW;

static GLW::Color lineColor(int kind){
    switch(kind){
    case Console::ECHO:                 return rgb(0x3f6b0a);   // lime, dark enough for the white face
    case CmdScript::Sink::ERR:          return rgb(0xb3261e);
    case CmdScript::Sink::JOB:          return kTextSecondary;
    case ChatFeed::K_SERVER:            return rgb(0xa15c00);   // MP.7: amber, dark enough for the white face
    case ChatFeed::K_SELF:              return rgb(0x3f6b0a);
    case ChatFeed::K_CMD:               return kTextSecondary;
    case ChatFeed::K_NOTICE:            return rgb(0xb3261e);
    default:                            return kText;
    }
}

ConsoleMenu::ConsoleMenu()
 : m_open(false), m_collapsed(false), m_tab(TAB_CONSOLE), m_railN(3), m_railSession(false), m_srcTab(0),
   m_fieldTab(0), m_fieldCode(true), m_titleTab(-1), m_follow(true), m_newBelow(false), m_lastSerial(0),
   m_builtU(0), m_builtW(0), m_labelsU(0), m_labelsCollapsed(false), m_builtMatchW(-1), m_histPos(-1), m_titleY(0),
   m_touchScroll(-1), m_touchRail(-1), m_barDrag(false), m_downButton(CB_NONE), m_stripLines(3),
   m_autoStrip(false), m_noRail(false) {
    m_panel=m_content=m_bar=m_scrollArea=m_stripRect=m_inputRect=m_toolsRect=CGRectMake(0,0,0,0);
    const int icons[3]={UIICON_TERMINAL,UIICON_SETTINGS,UIICON_INFO};
    m_rail.setTiles(icons,3);
    m_field.setMaxBytes(511);
    m_field.setCodeMode(true);      // C.4: no autocorrect/autocapitalisation; Tab/Up/Down reach us
    m_field.setPlaceholder("Type a command - try help");
}

ConsoleMenu::~ConsoleMenu(){
    for(std::map<unsigned,Cached*>::iterator it=m_cache.begin();it!=m_cache.end();++it) delete it->second;
    for(int t=0;t<TABS;t++) for(size_t i=0;i<m_info[t].size();i++) delete m_info[t][i];
}

// ---------------------------------------------------------------------------------------------
// open / close
// ---------------------------------------------------------------------------------------------
void ConsoleMenu::open(){
    Console::init();
    Console::historyLoad();
    m_open=true;
    m_histPos=-1;
    m_follow=true;
    m_newBelow=false;
    m_touchScroll=m_touchRail=-1;
    m_downButton=CB_NONE;
    m_scroll.endDrag();
    m_infoScroll.endDrag();
    m_rail.setHeld(-1);
    m_builtU=0;                     // re-flow: the screen may have changed shape since the last visit
    for(int t=0;t<TABS;t++){ for(size_t i=0;i<m_info[t].size();i++) delete m_info[t][i]; m_info[t].clear(); }   // rebuilt on show: cvars registered since
    syncRail();
    configureField();
    if(m_tab==TAB_CHAT) ChatFeed::markSeen();
    layout();
    m_field.focus();                // typing is the point; the layout makes room for the keyboard
}

// MP.7: T / `/` / the touch button. The same screen as the `console` key, on the Chat tab.
void ConsoleMenu::openChat(const char* prefill){
    if(!MpWorld::active()) return;
    if(!m_open) open();
    showTab(TAB_CHAT);
    setInput(prefill?prefill:"");
    configureField();
    m_field.focus();
}

void ConsoleMenu::close(){
    if(!m_open) return;
    m_field.blur();
    m_open=false;
    m_downButton=CB_NONE;
    if(World::getWorld&&World::getWorld->menu) World::getWorld->menu->showconsole=FALSE;
    Input::getInput()->clearAll();  // a touch that was live when it closed must not land on the HUD
}

// Escape: a focused field owns the key (TextField turns it into EV_CANCEL and blurs), so this is
// reached with the field blurred -- close. A focused field only gets here from a harness: blur it.
void ConsoleMenu::back(){
    if(m_field.focused()){ m_field.blur(); return; }
    close();
}

void ConsoleMenu::toggle(){
    if(!m_open){
        if(World::getWorld&&World::getWorld->menu) World::getWorld->menu->showconsole=TRUE;
        open();
    }else if(!m_field.focused()){
        close();                    // a focused field owns every key, so the backquote types there
    }
}

void ConsoleMenu::showTab(int t){
    if(t<0||t>=TABS) return;
    if(t>=TAB_CHAT&&!MpWorld::active()) return;      // MP.7: the session tiles exist in a session only
    m_tab=t;
    m_infoScroll.setFirst(0);
    m_infoScroll.endDrag();
    if(t==TAB_CHAT){ ChatFeed::markSeen(); m_follow=true; m_newBelow=false; }
    configureField();
    layout();
}

// ---------------------------------------------------------------------------------------------
// MP.7: the session tiles, the chat's lines, the field's mode
// ---------------------------------------------------------------------------------------------
int ConsoleMenu::tabOfTile(int tile) const{
    static const int kSession[5]={TAB_CHAT,TAB_PLAYERS,TAB_CONSOLE,TAB_VARIABLES,TAB_HELP};
    if(m_railSession) return tile>=0&&tile<5?kSession[tile]:-1;
    return tile>=0&&tile<3?tile:-1;
}

int ConsoleMenu::tileOfTab(int tab) const{
    if(!m_railSession) return tab>=0&&tab<3?tab:0;
    switch(tab){ case TAB_CHAT: return 0; case TAB_PLAYERS: return 1; case TAB_CONSOLE: return 2; case TAB_VARIABLES: return 3; default: return 4; }
}

void ConsoleMenu::syncRail(){
    const bool sess=MpWorld::active();
    if(sess==m_railSession&&m_railN>0) return;
    m_railSession=sess;
    if(sess){
        const int icons[5]={UIICON_MESSAGE_SQUARE,UIICON_USERS,UIICON_TERMINAL,UIICON_SETTINGS,UIICON_INFO};
        m_rail.setTiles(icons,5);
        m_railN=5;
    }else{
        const int icons[3]={UIICON_TERMINAL,UIICON_SETTINGS,UIICON_INFO};
        m_rail.setTiles(icons,3);
        m_railN=3;
        if(m_tab>=TAB_CHAT) m_tab=TAB_CONSOLE;
    }
}

const std::vector<Console::Line>& ConsoleMenu::srcLines() const{
    return m_tab==TAB_CHAT?ChatFeed::lines():Console::scrollback();
}

unsigned ConsoleMenu::srcSerial() const{
    return m_tab==TAB_CHAT?ChatFeed::serial():Console::scrollbackSerial();
}

// Chat and Console share the one field. Each tab keeps its own draft; the chat's byte cap is the
// server's; the chat is prose (autocorrect on) on a touch device unless the line is a `/` command.
// A hardware keyboard needs the code mode for Tab / Up / Down to reach us, so desktop stays in it.
void ConsoleMenu::configureField(){
    const int which=m_tab==TAB_CHAT?1:0;
    if(which!=m_fieldTab && inputTab()){
        std::string mine=m_field.text();
        m_field.setText(m_otherText.c_str());
        m_otherText=mine;
        m_fieldTab=which;
        m_histPos=-1;
        m_matches.clear();
        if(which){ m_field.setMaxBytes(ChatFeed::MAX_BYTES); m_field.setPlaceholder("Type a message, / for commands"); }
        else      { m_field.setMaxBytes(511);                m_field.setPlaceholder("Type a command - try help"); }
    }
    if(which){
        const std::string& t=m_field.text();
        const bool code=touchFloor()<=0.0f||(!t.empty()&&t[0]=='/');
        if(code!=m_fieldCode){ m_fieldCode=code; m_field.setCodeMode(code); }
    }else if(!m_fieldCode){ m_fieldCode=true; m_field.setCodeMode(true); }
}

int ConsoleMenu::playerRows() const{ return (int)m_info[TAB_PLAYERS].size(); }

// The Players tab: you, then every player seen (MpWorld's peers: joined, moved or spoke, and not
// left or silent for 30 s). The protocol has no who-is-on query, so this is "seen", not "online".
void ConsoleMenu::fillPlayerRows(){
    std::vector<InfoRow*>& rows=m_info[TAB_PLAYERS];
    const int n=MpWorld::peerCount();
    std::vector<std::string> names, hints;
    char me[40];
    if(!eden_mp_profile_name(me,sizeof(me))) std::snprintf(me,sizeof(me),"You");
    names.push_back(std::string(me)+" (you)");
    hints.push_back("");
    Vector pp=World::getWorld&&World::getWorld->player?World::getWorld->player->pos:MakeVector(0,0,0);
    std::vector<std::pair<std::string,std::string> > others;
    for(int i=0;i<n;i++){
        MpWorld::PeerView v;
        if(!MpWorld::peerView(i,&v)) continue;
        char h[64];
        const float dx=v.x-pp.x, dz=v.z-pp.z;
        std::snprintf(h,sizeof(h),"%d blocks away",(int)std::sqrt(dx*dx+dz*dz));
        others.push_back(std::make_pair(std::string(v.name),std::string(h)));
    }
    std::sort(others.begin(),others.end());
    for(size_t i=0;i<others.size();i++){ names.push_back(others[i].first); hints.push_back(others[i].second); }
    while(rows.size()>names.size()){ delete rows.back(); rows.pop_back(); }
    while(rows.size()<names.size()) rows.push_back(new InfoRow());
    for(size_t i=0;i<rows.size();i++){
        rows[i]->title=names[i];
        rows[i]->hint=hints[i];
        rows[i]->insert=i==0?std::string():"/msg "+others[i-1].first+" ";
    }
}

void ConsoleMenu::setCollapsed(bool c){
    if(m_collapsed==c) return;
    m_collapsed=c;
    m_field.setLines(strip()?1:2);
    m_builtU=0;
    layout();
}

void ConsoleMenu::setInput(const char* text){
    m_field.setText(text);
}

bool ConsoleMenu::inputFocused() const { return m_field.focused(); }
void ConsoleMenu::blurInput(){ m_field.blur(); }

// ---------------------------------------------------------------------------------------------
// running a line
// ---------------------------------------------------------------------------------------------
void ConsoleMenu::submit(){
    if(m_tab==TAB_CHAT){
        // MP.7: a chat line (or a `/` command, which the server runs). The field clears only on a
        // send; a too-fast line stays for the next try.
        switch(ChatFeed::send(m_field.text())){
        case ChatFeed::SENT:
            m_field.setText("");
            m_histPos=-1; m_draft.clear();
            m_follow=true; m_newBelow=false;
            if(touchFloor()<=0.0f) close();      // desktop: Minecraft's habit; on touch the keyboard stays up
            break;
        case ChatFeed::TOO_FAST:
            ChatFeed::addNotice("Chat line not sent (too fast). Wait a moment and send it again.");
            m_follow=true;
            break;
        case ChatFeed::NO_SESSION:
            ChatFeed::addNotice("Not connected to a server.");
            break;
        case ChatFeed::EMPTY:
            if(touchFloor()<=0.0f) close();      // desktop: Enter on an empty field closes
            break;
        }
        return;
    }
    std::string line=m_field.text();
    while(!line.empty()&&std::isspace((unsigned char)line[line.size()-1])) line.erase(line.size()-1);
    size_t k=0;
    while(k<line.size()&&std::isspace((unsigned char)line[k])) ++k;
    line=line.substr(k);
    if(line.empty()) return;
    Console::print(Console::ECHO,"> "+line);   // the echo is the screen's job (Console::run's contract)
    m_field.setText("");
    m_histPos=-1; m_draft.clear(); m_matches.clear();
    m_follow=true; m_newBelow=false;
    Console::run(line.c_str());
}

void ConsoleMenu::complete(){
    if(m_tab==TAB_CHAT){   // MP.7: complete a player name from the seen set
        std::string t=m_field.text();
        const size_t sp=t.find_last_of(' ');
        const std::string stem=sp==std::string::npos?t:t.substr(sp+1);
        if(stem.empty()||stem[0]=='/') return;
        std::vector<std::string> hit;
        for(int i=0;i<MpWorld::peerCount();i++){
            MpWorld::PeerView v;
            if(MpWorld::peerView(i,&v)&&!strncasecmp(v.name,stem.c_str(),stem.size())) hit.push_back(v.name);
        }
        std::sort(hit.begin(),hit.end());
        m_matches.clear();
        if(hit.empty()) return;
        if(hit.size()==1){ t=(sp==std::string::npos?std::string():t.substr(0,sp+1))+hit[0]+(sp==std::string::npos?": ":" "); m_field.setText(t.c_str()); }
        else for(size_t i=0;i<hit.size();i++){ if(i) m_matches+="  "; m_matches+=hit[i]; }
        return;
    }
    const Console::Completion c=Console::complete(m_field.text());
    m_field.setText(c.line.c_str());
    m_matches.clear();
    if(c.matches.size()>1){
        for(size_t i=0;i<c.matches.size();i++){ if(i) m_matches+="  "; m_matches+=c.matches[i]; }
    }
    m_histPos=-1;
}

void ConsoleMenu::historyStep(int dir){
    const bool chat=m_tab==TAB_CHAT;
    const int n=chat?(int)ChatFeed::historyCount():(int)Console::historyCount();
    if(dir<0){
        if(n==0) return;
        if(m_histPos<0){ m_draft=m_field.text(); m_histPos=n-1; }
        else if(m_histPos>0) m_histPos--;
        m_field.setText((chat?ChatFeed::historyGet((size_t)m_histPos):Console::historyGet((size_t)m_histPos)).c_str());
    }else{
        if(m_histPos<0) return;
        m_histPos++;
        if(m_histPos>=n){ m_histPos=-1; m_field.setText(m_draft.c_str()); }
        else m_field.setText((chat?ChatFeed::historyGet((size_t)m_histPos):Console::historyGet((size_t)m_histPos)).c_str());
    }
}

// ---------------------------------------------------------------------------------------------
// scrollback cache
// ---------------------------------------------------------------------------------------------
// One Label per kept line, keyed by serial. The key moves with the world of lines (the oldest fall
// off), so a line keeps its texture while it scrolls and a new one is the only upload.
void ConsoleMenu::syncScrollback(){
    const std::vector<Console::Line>& sb=srcLines();
    const unsigned serial=srcSerial();
    const unsigned base=serial-(unsigned)sb.size();
    const float textW=m_scrollArea.size.width-du(16);
    const int srcTab=m_tab==TAB_CHAT?1:0;
    if(srcTab!=m_srcTab){   // MP.7: another source's serials: the cache is not this one's
        m_srcTab=srcTab;
        m_builtW=-1;
        m_lastSerial=serial;
        m_follow=true; m_newBelow=false;
    }
    if(m_builtU!=u()||m_builtW!=textW){
        for(std::map<unsigned,Cached*>::iterator it=m_cache.begin();it!=m_cache.end();++it) delete it->second;
        m_cache.clear();
        m_builtU=u(); m_builtW=textW;
    }
    // Free what fell out of the kept range (the oldest lines, or all of them after `clear`).
    while(!m_cache.empty()&&m_cache.begin()->first<base){ delete m_cache.begin()->second; m_cache.erase(m_cache.begin()); }
    for(size_t i=0;i<sb.size();i++){
        const unsigned key=base+(unsigned)i;
        if(m_cache.count(key)) continue;
        Cached* c=new Cached();
        c->kind=sb[i].kind;
        c->label.set(sb[i].text.empty()?" ":sb[i].text.c_str(),du(15),UITextAlignmentLeft,textW>du(40)?textW:du(40),FACE_BODY);
        c->height=c->label.height()+du(3);
        m_cache[key]=c;
    }
    if(serial!=m_lastSerial){
        if(!m_follow) m_newBelow=true;
        m_lastSerial=serial;
    }
}

// ---------------------------------------------------------------------------------------------
// the Variables / Help rows
// ---------------------------------------------------------------------------------------------
void ConsoleMenu::buildInfoRows(int tab){
    std::vector<InfoRow*>& rows=m_info[tab];
    for(size_t i=0;i<rows.size();i++) delete rows[i];
    rows.clear();
    if(tab==TAB_VARIABLES){
        const size_t n=Console::cvarCount();
        for(size_t i=0;i<n;i++){
            const Console::Cvar* c=Console::cvarAt(i);
            InfoRow* r=new InfoRow();
            r->insert=std::string(c->name)+" ";
            rows.push_back(r);
        }
    }else if(tab==TAB_HELP){
        size_t n=0;
        const CmdScript::Verb* v=CmdScript::verbs(&n);
        for(size_t i=0;i<n;i++){
            InfoRow* r=new InfoRow();
            r->title=std::string(v[i].name)+(v[i].usage&&v[i].usage[0]?std::string(" ")+v[i].usage:std::string());
            r->hint=std::string(v[i].help?v[i].help:"")+" (also works in command blocks)";
            r->insert=std::string(v[i].name)+" ";
            rows.push_back(r);
        }
        const size_t nc=Console::commandCount();
        for(size_t i=0;i<nc;i++){
            const Console::CommandInfo ci=Console::commandAt(i);
            InfoRow* r=new InfoRow();
            r->title=std::string(ci.name)+(ci.usage&&ci.usage[0]?std::string(" ")+ci.usage:std::string());
            r->hint=ci.help?ci.help:"";
            r->insert=std::string(ci.name)+" ";
            rows.push_back(r);
        }
    }
}

int ConsoleMenu::helpRows() const { return (int)m_info[TAB_HELP].size(); }
int ConsoleMenu::varRows() const  { return (int)m_info[TAB_VARIABLES].size(); }

void ConsoleMenu::pickRow(int tab,int index){
    if(tab<=0||tab>=TABS||index<0||index>=(int)m_info[tab].size()) return;
    const std::string insert=m_info[tab][(size_t)index]->insert;
    if(tab==TAB_PLAYERS){   // MP.7: a tap puts `/msg name ` in the chat (your own row does nothing)
        if(insert.empty()) return;
        showTab(TAB_CHAT);
        m_field.setText(insert.c_str());
        configureField();
        m_histPos=-1;
        m_field.focus();
        return;
    }
    showTab(TAB_CONSOLE);
    m_field.setText(insert.c_str());
    m_histPos=-1;
    m_field.focus();
}

// ---------------------------------------------------------------------------------------------
// layout
// ---------------------------------------------------------------------------------------------
void ConsoleMenu::buildLabels(){
    m_title.set(m_tab==TAB_CHAT?"Chat":m_tab==TAB_PLAYERS?"Players":"Console",du(32),UITextAlignmentCenter);
    m_titleTab=m_tab;
    m_back.setLabel("Back",du(20));       m_back.sizeToFit();
    m_collapse.setLabel(m_collapsed?"Expand":"Collapse",du(20)); m_collapse.sizeToFit();
    m_run.setLabel(m_tab==TAB_CHAT?"Send":"Run",du(20)); m_run.setTone(GLW::Button::TONE_POSITIVE); m_run.sizeToFit();
    m_tabBtn.setLabel("Tab",du(18));      m_tabBtn.sizeToFit(7.0f);
    m_up.setLabel("Up",du(18));           m_up.sizeToFit(7.0f);
    m_down.setLabel("Down",du(18));       m_down.sizeToFit(7.0f);
    m_newPill.setLabel("New lines",du(16)); m_newPill.sizeToFit(7.0f);
    m_field.setPointSize(du(22));
}

// C.4: railFrame(), unless an on-screen keyboard reaches into it. Then the panel runs from just above
// the keyboard to just below the screen top, and has no rail: the content takes the rail's width
// (there is no room for tabs while typing; they come back with the keyboard's going).
RailFrame ConsoleMenu::frame(float kbTop){
    RailFrame f=railFrame();
    m_noRail=false;
    if(kbTop<=0||kbTop+du(6)<=f.panel.origin.y) return f;
    m_noRail=true;
    const CGRect safe=safeRect();   // I.2
    const float bottom=kbTop+du(6), top=safe.origin.y+safe.size.height-du(6);
    f.panel=CGRectMake(f.panel.origin.x,bottom,f.panel.size.width,std::max(0.0f,top-bottom));
    const float pad=du(9), gap=du(6), barH=du(29), barW=du(18);
    const float left=f.panel.origin.x+pad, right=f.panel.origin.x+f.panel.size.width-pad;
    const float barTop=top-du(7);
    f.titlebar=CGRectMake(left,barTop-barH,right-left,barH);
    const float bodyTop=barTop-barH-pad, bodyBot=bottom+pad;
    const float bodyH=std::max(0.0f,bodyTop-bodyBot);
    f.rail=CGRectMake(left,bodyBot,0,bodyH);
    f.bar=CGRectMake(right-barW,bodyBot,barW,bodyH);
    f.content=CGRectMake(left,bodyBot,right-barW-gap-left,bodyH);
    return f;
}

void ConsoleMenu::layout(){
    syncRail();
    const float tf=touchFloor();
    const float pad=du(8);
    const float toolH=controlH(30);
    const float lh=du(15)*1.28f+du(3);              // one unwrapped scrollback line

    // C.4: where the on-screen keyboard is (0: none). Only the views with the input care; the
    // Variables/Help tabs blur the field, so the keyboard is on its way down there.
    const bool inputView=m_collapsed||inputTab();
    const float kb=inputView&&m_field.focused()?eden_text_input_keyboard_top():0.0f;
    bool autoStrip=false;
    if(!m_collapsed&&kb<0) autoStrip=true;           // a keyboard is coming, height unknown: top-dock
    else if(!m_collapsed&&kb>0){
        // Would the full layout above the keyboard keep 4 scrollback lines? (iPhone landscape: no.)
        const RailFrame f=frame(kb);
        const float inputRowH=std::max(m_field.heightForLines(2),tf);
        const float scrollH=f.content.size.height-pad-inputRowH-pad*0.5f-toolH-pad;
        if(scrollH<4*lh) autoStrip=true;
    }
    if(autoStrip!=m_autoStrip){
        m_autoStrip=autoStrip;
        m_field.setLines(strip()?1:2);
        m_builtU=0;
    }
    const bool relabel=m_labelsU!=u()||m_labelsCollapsed!=m_collapsed||m_title.empty()||m_titleTab!=m_tab;
    if(relabel){ buildLabels(); m_labelsU=u(); m_labelsCollapsed=m_collapsed; }

    if(strip()){
        // STRIP: top-docked, as wide as the screen less a margin, so the world shows underneath.
        // Above a keyboard it drops lines (down to one) rather than reach under it.
        // I.3 (its --fit-selftest found it): inside the safe rect, clear of the island.
        const CGRect safe=safeRect();
        const float margin=du(12), top=safe.origin.y+safe.size.height;
        const float fieldH=m_field.heightForLines(1);
        const float rowH=std::max(fieldH,std::max(tf,du(30)));
        const float panelW=safe.size.width-2*margin;
        int lines=m_stripLines;
        if(kb>0) while(lines>1&&pad+lh*lines+pad+rowH+pad>top-margin-kb-du(6)) lines--;
        const float textH=lh*lines;
        const float panelH=pad+textH+pad+rowH+pad;
        m_noRail=false;
        m_panel=CGRectMake(safe.origin.x+margin,top-margin-panelH,panelW,panelH);
        // Expand and Back, top right (the lines' width stops short of them, see below). The auto
        // strip has no Expand: the keyboard's going expands it.
        const float bw=m_back.fitWidth(), cw=m_autoStrip?0.0f:m_collapse.fitWidth()+du(6);
        const float by=m_panel.origin.y+panelH-pad-std::max(du(29),tf);
        const float bh=std::max(du(29),tf);
        m_back.setRect(CGRectMake(m_panel.origin.x+panelW-pad-bw,by,0,bh));
        m_collapse.setRect(CGRectMake(m_panel.origin.x+panelW-pad-bw-cw,by,0,bh));
        m_run.setRect(CGRectMake(m_panel.origin.x+panelW-pad-m_run.fitWidth(),m_panel.origin.y+pad,0,rowH));
        m_inputRect=CGRectMake(m_panel.origin.x+pad,m_panel.origin.y+pad,
                               panelW-pad*2-m_run.fitWidth()-du(6),rowH);
        m_field.setRect(m_inputRect);
        m_scrollArea=CGRectMake(m_panel.origin.x+pad,m_panel.origin.y+pad+rowH+pad,
                                panelW-pad*2-bw-cw-du(12),textH);
        m_stripRect=m_scrollArea;
        m_content=m_panel;
        syncScrollback();
        return;
    }

    const RailFrame f=frame(inputTab()?kb:0.0f);
    m_panel=f.panel;
    const CGRect& T=f.titlebar;
    m_back.setRect(CGRectMake(T.origin.x,T.origin.y,0,T.size.height));
    titlebarHit(m_back);
    m_collapse.setRect(CGRectMake(T.origin.x+T.size.width-m_collapse.fitWidth(),T.origin.y,0,T.size.height));
    titlebarHit(m_collapse);
    m_titleY=f.titleY(m_title);
    m_rail.setRect(f.rail);
    m_rail.setSelected(tileOfTab(m_tab));
    m_content=f.content;
    m_bar=f.bar;

    if(!inputTab()){
        // Variables / Help (and MP.7's Players): TwoLineRows on the content surface, built the first time the tab shows.
        std::vector<InfoRow*>& rows=m_info[m_tab];
        if(m_tab==TAB_PLAYERS) fillPlayerRows();
        else if(rows.empty()) buildInfoRows(m_tab);
        std::vector<float> heights(rows.size());
        for(size_t i=0;i<rows.size();i++){
            InfoRow* r=rows[i];
            if(m_tab==TAB_VARIABLES){
                const Console::Cvar* c=Console::cvarAt(i);
                r->title=std::string(c->name)+" = "+Console::cvarValue((int)i);
                std::string h=c->help&&c->help[0]?c->help:(c->label?c->label:"");
                if(c->apply==Console::APPLY_READONLY) h+=" (read-only)";
                else if(c->apply==Console::APPLY_NEXT_WORLD||c->apply==Console::APPLY_NEXT_CONVERT) h+=" (applies when a world opens)";
                r->hint=h;
            }
            r->row.build(r->title.c_str(),r->hint.c_str(),f.content.size.width,0,0,0,du(22));
            r->row.setLast(i+1==rows.size());
            r->height=r->row.height();
            heights[i]=r->height;
        }
        m_infoScroll.setBarRect(m_bar);
        m_infoScroll.setRowHeights(heights.empty()?NULL:&heights[0],(int)heights.size(),f.content.size.height);
        for(int i=m_infoScroll.first();i<(int)rows.size()&&i<m_infoScroll.first()+m_infoScroll.visible();i++)
            rows[(size_t)i]->row.setOrigin(f.content.origin.x,f.content.origin.y+f.content.size.height-m_infoScroll.rowOffset(i));
        return;
    }

    // CONSOLE tab: scrollback over a tool row over the input.
    const float fieldH=m_field.heightForLines(2);
    const float runW=m_run.fitWidth();
    const float inputRowH=std::max(fieldH,tf);
    const CGRect& C=f.content;
    m_inputRect=CGRectMake(C.origin.x+pad,C.origin.y+pad,C.size.width-pad*3-runW,inputRowH);
    m_field.setRect(m_inputRect);
    m_run.setRect(CGRectMake(C.origin.x+C.size.width-pad-runW,C.origin.y+pad,0,inputRowH));
    m_toolsRect=CGRectMake(C.origin.x+pad,C.origin.y+pad+inputRowH+pad*0.5f,C.size.width-pad*2,toolH);
    const float ty=m_toolsRect.origin.y;
    float right=m_toolsRect.origin.x+m_toolsRect.size.width;
    m_down.setRect(CGRectMake(right-m_down.fitWidth(),ty,0,toolH)); right-=m_down.fitWidth()+du(6);
    m_up.setRect(CGRectMake(right-m_up.fitWidth(),ty,0,toolH));     right-=m_up.fitWidth()+du(6);
    m_tabBtn.setRect(CGRectMake(right-m_tabBtn.fitWidth(),ty,0,toolH)); right-=m_tabBtn.fitWidth()+du(10);
    // The anchor, then the completion candidates in what is left of the row.
    {
        char buf[64];
        if(m_tab==TAB_CHAT) buf[0]=0;      // MP.7: the anchor is the Console's
        else if(Console::anchorIsFixed()){ int x,y,z; Console::currentAnchor(&x,&y,&z); std::snprintf(buf,sizeof(buf),"anchor: %d %d %d",x,y,z); }
        else std::snprintf(buf,sizeof(buf),"anchor: under you");
        if(m_anchorText!=buf||(m_anchor.empty()&&buf[0])||relabel){
            m_anchorText=buf;
            if(buf[0]) m_anchor.set(buf,du(15),UITextAlignmentLeft,0.0f,FACE_BODY);
            else m_anchor.clear();
        }
    }
    const float anchorX=right-m_anchor.width();
    m_toolsRect.size.width=std::max<float>(0.0f,anchorX-m_toolsRect.origin.x-du(8));   // the candidates' room
    const float areaBottom=m_toolsRect.origin.y+toolH+pad*0.5f;
    m_scrollArea=CGRectMake(C.origin.x+pad,areaBottom,C.size.width-pad*2,C.origin.y+C.size.height-areaBottom-pad*0.5f);
    m_scroll.setBarRect(CGRectMake(m_bar.origin.x,m_scrollArea.origin.y,m_bar.size.width,m_scrollArea.size.height));

    syncScrollback();
    const std::vector<Console::Line>& sb=srcLines();
    const unsigned base=srcSerial()-(unsigned)sb.size();
    std::vector<float> heights(sb.size());
    for(size_t i=0;i<sb.size();i++){
        std::map<unsigned,Cached*>::iterator it=m_cache.find(base+(unsigned)i);
        heights[i]=it!=m_cache.end()?it->second->height:du(20);
    }
    m_scroll.setRowHeights(heights.empty()?NULL:&heights[0],(int)heights.size(),m_scrollArea.size.height);
    if(m_follow&&!heights.empty()) m_scroll.ensureVisible((int)heights.size()-1);
    // The completion candidates: one wrapped line in the body face, rebuilt only when they change.
    if(m_builtMatches!=m_matches||m_builtMatchW!=m_toolsRect.size.width||relabel){
        m_builtMatches=m_matches; m_builtMatchW=m_toolsRect.size.width;
        if(m_matches.empty()||m_toolsRect.size.width<du(40)) m_matchLabel.clear();
        else m_matchLabel.set(m_matches.c_str(),du(15),UITextAlignmentLeft,m_toolsRect.size.width,FACE_BODY);
    }
    const CGRect sa=m_scrollArea;
    m_newPill.setRect(CGRectMake(sa.origin.x+sa.size.width-m_newPill.fitWidth()-du(6),sa.origin.y+du(6),0,std::max(du(26),tf)));
}

// ---------------------------------------------------------------------------------------------
// input
// ---------------------------------------------------------------------------------------------
static bool inRectPt(CGRect r,float x,float y){
    return x>=r.origin.x&&x<=r.origin.x+r.size.width&&y>=r.origin.y&&y<=r.origin.y+r.size.height;
}

// Which titlebar/tool button is under the point (CB_NONE if none). The strip has Expand, Back and Run;
// the Console tab adds the tools and the "new lines" pill; Variables/Help have Back and Collapse.
int ConsoleMenu::hitButton(float mx,float my) const{
    const bool consoleTab=inputTab();
    if(m_back.hit(mx,my)) return CB_BACK;
    if(!m_autoStrip&&m_collapse.hit(mx,my)) return CB_COLLAPSE;
    if((strip()||consoleTab)&&m_run.hit(mx,my)) return CB_RUN;
    if(!strip()&&consoleTab){
        if(m_tabBtn.hit(mx,my)) return CB_TAB;
        if(m_up.hit(mx,my)) return CB_UP;
        if(m_down.hit(mx,my)) return CB_DOWN;
        if(m_newBelow&&!m_follow&&m_newPill.hit(mx,my)) return CB_PILL;
    }
    return CB_NONE;
}

// The scrollback moved by the player's hand: "following" is whether the last row is in view.
void ConsoleMenu::scrolled(){
    m_follow=m_scroll.first()+m_scroll.visible()>=m_scroll.total();
    if(m_follow) m_newBelow=false;
}

// A tap on a scrollback line that echoes input puts the input back in the field.
bool ConsoleMenu::lineBackToField(int index){
    const std::vector<Console::Line>& sb=srcLines();
    if(index<0||index>=(int)sb.size()) return false;
    const Console::Line& l=sb[(size_t)index];
    if(l.kind!=Console::ECHO||l.text.size()<3) return false;
    m_field.setText(l.text.substr(2).c_str());     // drop the "> "
    m_histPos=-1;
    m_field.focus();
    return true;
}

void ConsoleMenu::update(float etime){
    (void)etime;
    if(!m_open) return;
    configureField();
    layout();
    const bool consoleTab=inputTab();
    GLW::ScrollView& scroll=consoleTab?m_scroll:m_infoScroll;
    if(m_tab==TAB_CHAT&&m_follow) ChatFeed::markSeen();

    if(int wheel=eden_ui_take_wheel()){
        if(!strip()){
            scroll.scrollBy(-wheel);
            if(consoleTab) scrolled();
        }
    }

    // The field first: Return runs, Escape blurs (a second Escape closes, from the seam), a dismissed
    // soft keyboard keeps the line.
    if(consoleTab||m_collapsed){
        if(m_field.update()==GLW::TextField::EV_COMMIT){ submit(); if(m_open) m_field.focus(); }
        // C.4: a hardware keyboard's Tab / Up / Down, the same as the buttons.
        for(int k=m_field.takeKey();k;k=m_field.takeKey()){
            if(k==EDEN_TEXT_KEY_TAB) complete();
            else historyStep(k==EDEN_TEXT_KEY_UP?-1:+1);
        }
    }

    Input* input=Input::getInput();
    itouch* touches=input->getTouches();
    for(int i=0;i<MAX_TOUCHES;i++){
        if(touches[i].inuse==0&&touches[i].down==M_DOWN){
            // Every touch is this screen's while it is open: the world runs on, but nothing reaches
            // the HUD, the joystick or the player (their flags re-derive to false each frame).
            touches[i].inuse=usage_id;
            const float mx=touches[i].mx, my=touches[i].my;
            const int b=hitButton(mx,my);
            m_downButton=b;
            m_back.setPressed(b==CB_BACK); m_collapse.setPressed(b==CB_COLLAPSE); m_run.setPressed(b==CB_RUN);
            m_tabBtn.setPressed(b==CB_TAB); m_up.setPressed(b==CB_UP); m_down.setPressed(b==CB_DOWN);
            m_newPill.setPressed(b==CB_PILL);
            if(b!=CB_NONE) continue;
            if(strip()) continue;
            if(railShown()&&m_rail.hitRail(mx,my)&&m_touchRail<0){
                m_touchRail=i; m_rail.setHeld(m_rail.hit(mx,my)); m_rail.beginDrag(my);
                continue;
            }
            if(scroll.hitBar(mx,my)&&m_touchScroll<0){
                m_touchScroll=i; m_barDrag=true; scroll.barBegin(my);
                continue;
            }
            if(m_touchScroll<0&&inRectPt(consoleTab?m_scrollArea:m_content,mx,my)){
                m_touchScroll=i; m_barDrag=false; scroll.beginContentDrag(my);
            }
        }
        if(touches[i].inuse==usage_id&&touches[i].down==M_DOWN){
            const float my=touches[i].my;
            if(i==m_touchScroll){
                if(m_barDrag) scroll.barDragTo(my);
                else scroll.contentDragTo(my);
                if(consoleTab) scrolled();
            }
            if(i==m_touchRail&&m_rail.dragTo(my)) m_rail.setHeld(-1);
        }
        if(touches[i].inuse==usage_id&&touches[i].down==M_RELEASE){
            const float mx=touches[i].mx, my=touches[i].my;
            touches[i].inuse=0;
            touches[i].down=M_NONE;
            m_back.setPressed(false); m_collapse.setPressed(false); m_run.setPressed(false);
            m_tabBtn.setPressed(false); m_up.setPressed(false); m_down.setPressed(false); m_newPill.setPressed(false);
            const int downB=m_downButton;
            m_downButton=CB_NONE;
            if(downB!=CB_NONE){
                if(hitButton(mx,my)!=downB) continue;     // pressed here, released elsewhere: no press
                switch(downB){
                case CB_BACK:     close(); return;
                case CB_COLLAPSE: setCollapsed(!m_collapsed); break;
                case CB_RUN:      submit(); if(m_open) m_field.focus(); else return; break;
                case CB_TAB:      complete(); m_field.focus(); break;
                case CB_UP:       historyStep(-1); m_field.focus(); break;
                case CB_DOWN:     historyStep(+1); m_field.focus(); break;
                case CB_PILL:     m_follow=true; m_newBelow=false; break;
                }
                continue;
            }
            if(i==m_touchRail){
                const bool dragged=m_rail.scrolling();
                m_rail.endDrag(); m_rail.setHeld(-1); m_touchRail=-1;
                if(!dragged){
                    const int tile=m_rail.hit(mx,my);
                    const int t=tile>=0?tabOfTile(tile):-1;
                    if(t>=0){ showTab(t); if(inputTab()) m_field.focus(); else m_field.blur(); }
                }
                continue;
            }
            bool wasScroll=false;
            if(i==m_touchScroll){
                wasScroll=m_barDrag||scroll.scrolling();
                scroll.endDrag(); m_touchScroll=-1; m_barDrag=false;
            }
            if(wasScroll) continue;
            // A tap, not a scroll.
            if((consoleTab||m_collapsed)&&m_field.hit(mx,my)){ m_field.focus(); continue; }
            if(strip()) continue;
            if(consoleTab){
                const float top=m_scrollArea.origin.y+m_scrollArea.size.height;
                const int n=(int)srcLines().size();
                const unsigned base=srcSerial()-(unsigned)n;
                for(int k=m_scroll.first();k<n&&k<m_scroll.first()+m_scroll.visible();k++){
                    std::map<unsigned,Cached*>::iterator it=m_cache.find(base+(unsigned)k);
                    if(it==m_cache.end()) continue;
                    const float y=top-m_scroll.rowOffset(k);
                    if(my<=y&&my>y-it->second->height&&mx>=m_scrollArea.origin.x&&mx<=m_scrollArea.origin.x+m_scrollArea.size.width){
                        lineBackToField(k);
                        break;
                    }
                }
            }else{
                std::vector<InfoRow*>& rows=m_info[m_tab];
                for(int k=m_infoScroll.first();k<(int)rows.size()&&k<m_infoScroll.first()+m_infoScroll.visible();k++)
                    if(rows[(size_t)k]->row.hit(mx,my)){ pickRow(m_tab,k); break; }
            }
        }
    }
}

// ---------------------------------------------------------------------------------------------
// render
// ---------------------------------------------------------------------------------------------
int ConsoleMenu::visibleLines() const { return (int)m_visSerials.size(); }

void ConsoleMenu::drawScrollback(CGRect area){
    m_visSerials.clear();
    const std::vector<Console::Line>& sb=srcLines();
    const unsigned base=srcSerial()-(unsigned)sb.size();
    const int first=m_scroll.first();
    const int last=std::min((int)sb.size(),first+m_scroll.visible());
    const float top=area.origin.y+area.size.height;
    for(int k=first;k<last;k++){
        std::map<unsigned,Cached*>::iterator it=m_cache.find(base+(unsigned)k);
        if(it==m_cache.end()) continue;
        it->second->label.draw(area.origin.x+du(8),top-m_scroll.rowOffset(k),lineColor(it->second->kind));
        m_visSerials.push_back(base+(unsigned)k);
    }
}

// The strip: the newest lines that fit its fixed text box, newest nearest the input.
void ConsoleMenu::drawStrip(){
    m_visSerials.clear();
    const std::vector<Console::Line>& sb=srcLines();
    const unsigned base=srcSerial()-(unsigned)sb.size();
    float y=m_stripRect.origin.y;                         // the box's bottom edge
    for(int k=(int)sb.size()-1;k>=0;k--){
        std::map<unsigned,Cached*>::iterator it=m_cache.find(base+(unsigned)k);
        if(it==m_cache.end()) continue;
        const float h=it->second->height;
        if(y+h>m_stripRect.origin.y+m_stripRect.size.height+0.5f) break;
        it->second->label.draw(m_stripRect.origin.x,y+h,lineColor(it->second->kind));
        m_visSerials.insert(m_visSerials.begin(),base+(unsigned)k);
        y+=h;
    }
}

void ConsoleMenu::render(){
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

    if(strip()){
        bevel(m_panel,BEVEL_WINDOW);
        bevel(m_panel,BEVEL_CONTENT);
        if(!m_autoStrip) m_collapse.render();
        m_back.render();
        drawStrip();
        m_field.render();
        m_run.render();
    }else{
        fill(CGRectMake(0,0,SCREEN_WIDTH,SCREEN_HEIGHT),kScrim);
        bevel(m_panel,BEVEL_WINDOW);
        m_back.render();
        m_title.drawCentered(m_panel.origin.x+m_panel.size.width*0.5f,m_titleY,kText);
        m_collapse.render();
        if(railShown()) m_rail.render();
        bevel(m_content,BEVEL_CONTENT);
        if(inputTab()){
            drawScrollback(m_scrollArea);
            m_scroll.render();
            if(m_newBelow&&!m_follow) m_newPill.render();
            // tool row: candidates left, anchor, then Tab / Up / Down
            if(!m_matchLabel.empty()) m_matchLabel.draw(m_toolsRect.origin.x,m_toolsRect.origin.y+(m_toolsRect.size.height+m_matchLabel.height())*0.5f,kTextSecondary);
            if(!m_anchor.empty())
                m_anchor.draw(m_tabBtn.rect().origin.x-du(10)-m_anchor.width(),
                              m_toolsRect.origin.y+(m_toolsRect.size.height+m_anchor.lineHeight())*0.5f,kTextSecondary);
            m_tabBtn.render(); m_up.render(); m_down.render();
            m_field.render();
            m_run.render();
        }else{
            std::vector<InfoRow*>& rows=m_info[m_tab];
            for(int k=m_infoScroll.first();k<(int)rows.size()&&k<m_infoScroll.first()+m_infoScroll.visible();k++)
                rows[(size_t)k]->row.render();
            m_infoScroll.render();
        }
    }

    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
}

// ---------------------------------------------------------------------------------------------
// harness
// ---------------------------------------------------------------------------------------------
CGRect ConsoleMenu::controlRect(const char* which){
    layout();
    if(!std::strcmp(which,"back"))       return m_back.rect();
    if(!std::strcmp(which,"collapse"))   return m_collapse.rect();
    if(!std::strcmp(which,"input"))      return m_inputRect;
    if(!std::strcmp(which,"run"))        return m_run.rect();
    if(!std::strcmp(which,"tab"))        return m_tabBtn.rect();
    if(!std::strcmp(which,"up"))         return m_up.rect();
    if(!std::strcmp(which,"down"))       return m_down.rect();
    if(!std::strcmp(which,"scrollback")) return m_scrollArea;
    if(!std::strcmp(which,"panel"))      return m_panel;
    return CGRectMake(0,0,0,0);
}

CGRect ConsoleMenu::tabRect(int t){
    layout();
    return m_rail.tileRect(tileOfTab(t));
}
