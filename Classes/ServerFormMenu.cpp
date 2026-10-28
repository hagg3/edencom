//
//  ServerFormMenu.mm
//  Eden
//
//  MP.6: see ServerFormMenu.h. The shape (railFrame panel, TwoLineRows, the usage_id touch claim)
//  is NewWorldMenu.mm's.
//
#include "ServerFormMenu.h"
#include "ServerListMenu.h"
#include "MpServers.h"
#include "GLDialog.h"
#include "Graphics.h"
#include "Globals.h"
#include "Input.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

extern float SCREEN_WIDTH;
extern float SCREEN_HEIGHT;

static const int usage_id=18;   // distinct from Menu (7), SettingsMenu (3), GLDialog (9), Keybinds (11), WorldBrowser (12), NewWorld (13), WorldProperties (14), Hud (15, 16), Console (17)

// characterType 0..6 is one of each shipped model (MpWorld::avatarModel; 7..10 repeat them).
static const char* const kCharacters[7]={ "Green", "Moof", "Batty", "Nergle", "Stumpy", "Charger", "Stalker" };

static ServerFormMenu* s_confirming=NULL;   // the form whose Delete confirm is up

ServerFormMenu::ServerFormMenu(ServerListMenu* list){
    m_list=list;
    m_open=false;
    m_thenJoin=false;
    m_mode=MODE_ADD;
    m_builtMode=MODE_ADD;
    m_row=-1;
    m_character=0;
    m_builtU=0;
    m_valid=false;
    m_addrErr=m_addrWarn=false;
    m_footShown=true;
    m_titleY=0;
    m_decHeld=m_incHeld=false;
    m_shownChar=-1; m_shownCharU=0;
    joinPort=0;
    joinRemember=false;
    m_panel=m_content=CGRectMake(0,0,0,0);
    for(int k=0;k<4;k++) m_rowShown[k]=k;
    m_label.setMaxBytes(48);
    m_addr.setMaxBytes(260);
    m_pass.setMaxBytes(64);
    m_name.setMaxBytes(20);
    m_label.setKind(EDEN_TEXT_KIND_USERNAME);
    m_addr.setKind(EDEN_TEXT_KIND_USERNAME);
    m_name.setKind(EDEN_TEXT_KIND_USERNAME);
    m_pass.setSecure(true);
    m_charStep.setGlyphs("<",">");
}

ServerFormMenu::~ServerFormMenu(){ blurFields(); }

void ServerFormMenu::blurFields(){
    m_label.blur(); m_addr.blur(); m_pass.blur(); m_name.blur();
}

int ServerFormMenu::nRows() const{ return m_mode==MODE_PROFILE?2:4; }

GLW::TextField* ServerFormMenu::fieldAt(int k){
    if(m_mode==MODE_PROFILE) return k==0?&m_name:NULL;
    if(k==0) return &m_label;
    if(k==1) return &m_addr;
    if(k==2) return &m_pass;
    return NULL;
}

void ServerFormMenu::open(Mode m){
    m_mode=m;
    m_open=true;
    m_builtU=0;   // rebuild the labels for this mode
    m_footShown=true;
    m_decHeld=m_incHeld=false;
    layout();
    // Under a pointer the first field is one keystroke away; on touch the keyboard would cover the
    // screen unasked (New World's rule).
    if(GLW::touchFloor()==0.0f){
        if(m==MODE_PROFILE) m_name.focus();
        else if(m==MODE_ADD) m_addr.focus();
    }
}

void ServerFormMenu::openAdd(){
    m_row=-1;
    m_label.setText(""); m_addr.setText(""); m_pass.setText("");
    m_label.setPlaceholder("Optional");
    m_addr.setPlaceholder("host or host:port");
    m_pass.setPlaceholder("None");
    m_remember.setOn(true);
    open(MODE_ADD);
}

void ServerFormMenu::openEdit(int row){
    EdenMpServerRow r;
    if(!eden_mp_servers_get(row,&r)) return;
    m_row=row;
    m_label.setText(r.label);
    char a[300];
    if(std::strchr(r.host,':')) std::snprintf(a,sizeof(a),"[%s]:%d",r.host,r.port);   // a v6 literal
    else if(r.port!=27015) std::snprintf(a,sizeof(a),"%s:%d",r.host,r.port);
    else std::snprintf(a,sizeof(a),"%s",r.host);
    m_addr.setText(a);
    char pw[80];
    eden_mp_servers_password(row,pw,sizeof(pw));
    m_pass.setText(pw);
    m_label.setPlaceholder(r.name[0]?r.name:"Optional");
    m_addr.setPlaceholder("host or host:port");
    m_pass.setPlaceholder("None");
    m_remember.setOn(r.hasPassword!=0);
    open(MODE_EDIT);
}

void ServerFormMenu::openProfile(bool thenJoin){
    m_thenJoin=thenJoin;
    char n[32];
    eden_mp_profile_name(n,sizeof(n));
    m_name.setText(n);
    m_name.setPlaceholder("Your name");
    m_character=eden_mp_profile_character();
    open(MODE_PROFILE);
}

void ServerFormMenu::close(Outcome o){
    blurFields();
    m_pass.setText("");   // a typed password stays only as long as the form
    m_open=false;
    // As NewWorldMenu::close: a touch live when the screen opened may still be claimed.
    Input::getInput()->clearAll();
    const bool thenJoin=m_thenJoin;
    m_thenJoin=false;
    m_list->formClosed(o,m_row,thenJoin);
}

bool ServerFormMenu::back(){
    if(!m_open) return false;
    close(OUT_BACK);
    return true;
}

// ---------------------------------------------------------------------------------------------
// validation (UI plan §3.1, §4)
// ---------------------------------------------------------------------------------------------
void ServerFormMenu::validate(){
    m_addrErr=m_addrWarn=false;
    if(m_mode==MODE_PROFILE){
        char err[96];
        if(eden_mp_name_problem(m_name.text().c_str(),err,sizeof(err))){
            m_nameHint=m_name.text().empty()?"What other players see. 1-20 letters, no : [ ]":err;
            m_addrErr=!m_name.text().empty();
            m_valid=false;
        }else{
            m_nameHint="What other players see. 1-20 letters, no : [ ]";
            m_valid=true;
        }
        return;
    }
    char host[260], err[96];
    int port=0;
    if(!eden_mp_parse_address(m_addr.text().c_str(),host,sizeof(host),&port,err,sizeof(err))){
        m_addrHint=m_addr.text().empty()?"host or host:port. Port 27015 if none.":err;
        m_addrErr=!m_addr.text().empty();
        m_valid=false;
    }else{
        const int dup=eden_mp_servers_find(host,port,m_mode==MODE_EDIT?m_row:-1);
        EdenMpServerRow r;
        if(dup>=0&&eden_mp_servers_get(dup,&r)){
            m_addrHint=std::string("Already in your list as ")+(r.label[0]?r.label:r.host);
            m_addrWarn=true;
        }else m_addrHint="host or host:port. Port 27015 if none.";
        m_valid=true;
    }
    // The protocol's JOIN line is ':'-separated: a ':' in the password cannot be sent.
    if(m_pass.text().find(':')!=std::string::npos){ m_valid=false; }
}

bool ServerFormMenu::saveEnabled() const{ return m_valid; }

const char* ServerFormMenu::hint(const char* which) const{
    if(which&&!std::strcmp(which,"name")) return m_nameHint.c_str();
    return m_addrHint.c_str();
}

GLW::TextField* ServerFormMenu::field(const char* which){
    if(!which) return NULL;
    if(!std::strcmp(which,"label")) return &m_label;
    if(!std::strcmp(which,"address")) return &m_addr;
    if(!std::strcmp(which,"password")) return &m_pass;
    if(!std::strcmp(which,"name")) return &m_name;
    return NULL;
}

// ---------------------------------------------------------------------------------------------
// layout
// ---------------------------------------------------------------------------------------------
void ServerFormMenu::layout(){
    using namespace GLW;
    validate();
    if(m_builtU!=u()||m_builtMode!=m_mode){
        m_builtU=u();
        m_builtMode=m_mode;
        m_title.set(m_mode==MODE_ADD?"Add Server":m_mode==MODE_EDIT?"Edit Server":"Profile",du(32),UITextAlignmentCenter);
        m_back.setLabel("Back",du(20)); m_back.sizeToFit();
        m_save.setLabel("Save",du(20)); m_save.setTone(GLW::Button::TONE_POSITIVE); m_save.sizeToFit();
        m_del.setLabel("Delete",du(20)); m_del.setTone(GLW::Button::TONE_DANGER); m_del.sizeToFit();
        m_up.setLabel("Move up",du(20)); m_up.sizeToFit();
        m_down.setLabel("Move down",du(20)); m_down.sizeToFit();
        m_join.setLabel("Join now",du(20)); m_join.setIcon(UIICON_PLAY); m_join.sizeToFit();
        for(TextField* f:{&m_label,&m_addr,&m_pass,&m_name}) f->setPointSize(du(20));
    }
    const RailFrame f=railFrame();
    m_panel=f.panel;
    const CGRect& T=f.titlebar;
    m_back.setRect(CGRectMake(T.origin.x,T.origin.y,0,T.size.height));
    titlebarHit(m_back);
    m_save.setRect(CGRectMake(T.origin.x+T.size.width-m_save.fitWidth(),T.origin.y,0,T.size.height));
    titlebarHit(m_save);
    m_save.setEnabled(m_valid);
    m_titleY=f.titleY(m_title);

    // The rail's and the scrollbar's columns are the form's too (no rail, no scroll).
    const float left=f.rail.origin.x, right=f.bar.origin.x+f.bar.size.width;
    const float footH=controlH(30), gap=du(6);
    const bool foot=m_mode!=MODE_PROFILE;
    CGRect C=CGRectMake(left,f.content.origin.y+(foot?footH+gap:0.0f),right-left,
                        f.content.size.height-(foot?footH+gap:0.0f));

    // Keyboard avoidance: a keyboard reaching into the panel takes the foot's room and the rows'
    // bottom; the focused row is kept the lowest visible one.
    int focusK=-1;
    for(int k=0;k<nRows();k++){ TextField* tf=fieldAt(k); if(tf&&tf->focused()) focusK=k; }
    float kb=focusK>=0?eden_text_input_keyboard_top():0.0f;
    const CGRect safe=safeRect();
    if(kb<0.0f) kb=safe.origin.y+safe.size.height*0.5f;   // pending: keep to where none can reach
    m_footShown=foot;
    if(kb>m_panel.origin.y){
        m_footShown=false;
        const float top=(float)(C.origin.y+C.size.height);
        const float bot=std::min(top-du(40),std::max((float)f.content.origin.y,kb+du(6)));
        C=CGRectMake(C.origin.x,bot,C.size.width,top-bot);
    }
    m_content=C;

    const float fw=std::min(du(300),(float)(C.size.width*0.55f)), fh=du(34);
    float heights[4];
    const int n=nRows();
    for(int k=0;k<n;k++){
        const char* title=""; const char* h=""; float cw=fw, ch=fh;
        m_rowUi[k].setHintColor(kText);
        if(m_mode==MODE_PROFILE){
            if(k==0){ title="Multiplayer name"; h=m_nameHint.c_str(); if(m_addrErr) m_rowUi[k].setHintColor(kTextError); }
            else{ title="Character"; h="How other players see you."; cw=du(29)*2+du(130); ch=du(29); }
        }else{
            if(k==0){ title="Name"; h="Shown in your list. Blank takes the server's."; }
            else if(k==1){ title="Address"; h=m_addrHint.c_str();
                if(m_addrErr) m_rowUi[k].setHintColor(kTextError); else if(m_addrWarn) m_rowUi[k].setHintColor(kTextWarn); }
            else if(k==2){ title="Password";
                h=m_pass.text().find(':')!=std::string::npos?"A password cannot hold a :":"Leave blank if none.";
                if(m_pass.text().find(':')!=std::string::npos) m_rowUi[k].setHintColor(kTextError); }
            else{ title="Remember password"; h="Off: used for this join only."; cw=Toggle::designW(); ch=Toggle::designH(); }
        }
        m_rowUi[k].build(title,h,C.size.width,cw,ch);
        m_rowUi[k].setLast(k==n-1);
        heights[k]=m_rowUi[k].height();
    }
    // Stack from the top; if the focused row would fall below the area, shift up so it is the
    // lowest visible, hiding the rows that leave the top.
    float total=0, focusBottom=0;
    for(int k=0;k<n;k++){ total+=heights[k]; if(k==focusK) focusBottom=total; }
    float shift=0;
    if(focusK>=0&&focusBottom>C.size.height) shift=focusBottom-C.size.height;
    float y=C.origin.y+C.size.height+shift;
    for(int k=0;k<n;k++){
        const float top=y, bottom=y-heights[k];
        y=bottom;
        const bool shown=top<=C.origin.y+C.size.height+0.5f&&bottom>=C.origin.y-0.5f;
        m_rowShown[k]=shown?k:-1;
        m_rowUi[k].setOrigin(C.origin.x,top);
        float cw=fw, ch=fh;
        if(m_mode==MODE_PROFILE&&k==1){ cw=du(29)*2+du(130); ch=du(29); }
        if(m_mode!=MODE_PROFILE&&k==3){ cw=Toggle::designW(); ch=Toggle::designH(); }
        const CGRect cr=m_rowUi[k].controlRect(cw,ch);
        if(TextField* tf=fieldAt(k)){ tf->setRect(cr); tf->setKeepVisible(m_panel); }
        else if(m_mode==MODE_PROFILE){
            if(m_shownChar!=m_character||m_shownCharU!=u()){   // a texture upload: on change only
                m_shownChar=m_character; m_shownCharU=u();
                m_charStep.setValueText(kCharacters[m_character],du(20));
            }
            m_charStep.setRect(cr);
            m_charStep.setHitRect(m_rowUi[k].rect());
        }else{
            m_remember.setRect(cr);
            m_remember.setHitRect(touchFloor()>0.0f?m_rowUi[k].rect():CGRectMake(0,0,0,0));
        }
    }

    // The foot: the rare and destructive actions at the left (Edit only), Join now at the right.
    const float fy=f.content.origin.y;
    float x=left;
    GLW::Button* lefts[3]={&m_del,&m_up,&m_down};
    for(GLW::Button* b:lefts){ b->setRect(CGRectMake(x,fy,0,footH)); x+=b->fitWidth()+du(5); }
    m_join.setRect(CGRectMake(right-m_join.fitWidth(),fy,0,footH));
    m_join.setEnabled(m_valid);
    const int cnt=eden_mp_servers_count();
    m_up.setEnabled(m_mode==MODE_EDIT&&m_row>0);
    m_down.setEnabled(m_mode==MODE_EDIT&&m_row>=0&&m_row<cnt-1);
}

// ---------------------------------------------------------------------------------------------
// actions
// ---------------------------------------------------------------------------------------------
void ServerFormMenu::save(){
    if(!m_valid) return;
    if(m_mode==MODE_PROFILE){
        eden_mp_profile_set(m_name.text().c_str(),m_character);
        close(OUT_PROFILE_SAVED);
        return;
    }
    char host[260], err[96];
    int port=0;
    if(!eden_mp_parse_address(m_addr.text().c_str(),host,sizeof(host),&port,err,sizeof(err))) return;
    std::string label=m_label.text();
    const size_t a=label.find_first_not_of(' '), b=label.find_last_not_of(' ');
    label=a==std::string::npos?std::string():label.substr(a,b-a+1);
    // Remember off: the stored password is cleared on Save (UI plan §3.1).
    const char* pw=m_remember.on()?m_pass.text().c_str():"";
    if(m_mode==MODE_ADD){
        const int i=eden_mp_servers_add(label.c_str(),host,port,pw);
        if(i<0){
            static const char* const kOk[]={"OK"};
            GLDialog::show("List full","Remove a server to add another (100 at most).",kOk,1,NULL);
            return;
        }
        m_row=i;
    }else{
        eden_mp_servers_update(m_row,label.c_str(),host,port,pw);
    }
    eden_mp_servers_save();
    eden_mp_servers_refresh(m_row);
    close(OUT_SAVED);
}

void ServerFormMenu::joinNow(){
    if(!m_valid) return;
    char host[260], err[96];
    int port=0;
    if(!eden_mp_parse_address(m_addr.text().c_str(),host,sizeof(host),&port,err,sizeof(err))) return;
    joinHost=host;
    joinPort=port;
    joinPassword=m_pass.text();
    joinLabel=m_label.text().empty()?std::string(host):m_label.text();
    joinRemember=m_remember.on();
    close(OUT_JOIN_NOW);
}

static void sf_delete_cb(int chosen){
    ServerFormMenu* f=s_confirming;
    s_confirming=NULL;
    if(!f||chosen!=0||!f->isOpen()) return;
    f->close(ServerFormMenu::OUT_DELETED);
}

void ServerFormMenu::update(float etime){
    (void)etime;
    if(!m_open) return;
    layout();
    using namespace GLW;

    // Return moves on (Address -> Password; Password / Name is Save); Escape only leaves the field.
    for(int k=0;k<nRows();k++){
        TextField* tf=fieldAt(k);
        if(!tf) continue;
        const TextField::Event ev=tf->update();
        if(ev!=TextField::EV_COMMIT) continue;
        if(m_mode!=MODE_PROFILE&&k<2){ TextField* next=fieldAt(k+1); if(next) next->focus(); }
        else{ save(); return; }
    }
    if(!m_open) return;

    Input* input=Input::getInput();
    itouch* touches=input->getTouches();
    const bool touch=touchFloor()>0.0f;
    for(int i=0;i<MAX_TOUCHES;i++){
        if(touches[i].inuse==0&&touches[i].down==M_DOWN){
            touches[i].inuse=usage_id;
            const float mx=touches[i].mx, my=touches[i].my;
            m_back.setPressed(m_back.hit(mx,my));
            m_save.setPressed(m_save.enabled()&&m_save.hit(mx,my));
            if(m_footShown){
                m_join.setPressed(m_join.enabled()&&m_join.hit(mx,my));
                if(m_mode==MODE_EDIT){
                    m_del.setPressed(m_del.hit(mx,my));
                    m_up.setPressed(m_up.enabled()&&m_up.hit(mx,my));
                    m_down.setPressed(m_down.enabled()&&m_down.hit(mx,my));
                }
            }
            if(m_mode==MODE_PROFILE){
                m_decHeld=m_charStep.hitDec(mx,my);
                m_incHeld=!m_decHeld&&m_charStep.hitInc(mx,my);
                m_charStep.setPressed(m_decHeld,m_incHeld);
            }
        }
        if(touches[i].inuse==usage_id&&touches[i].down==M_RELEASE){
            const float mx=touches[i].mx, my=touches[i].my;
            touches[i].inuse=0;
            touches[i].down=M_NONE;
            const bool backHit=m_back.pressed()&&m_back.hit(mx,my);
            const bool saveHit=m_save.pressed()&&m_save.hit(mx,my);
            const bool joinHit=m_join.pressed()&&m_join.hit(mx,my);
            const bool delHit=m_del.pressed()&&m_del.hit(mx,my);
            const bool upHit=m_up.pressed()&&m_up.hit(mx,my);
            const bool downHit=m_down.pressed()&&m_down.hit(mx,my);
            const bool decHit=m_decHeld&&m_charStep.hitDec(mx,my), incHit=m_incHeld&&m_charStep.hitInc(mx,my);
            for(GLW::Button* b:{&m_back,&m_save,&m_join,&m_del,&m_up,&m_down}) b->setPressed(false);
            m_decHeld=m_incHeld=false;
            m_charStep.setPressed(false,false);

            int focus=-1;
            for(int k=0;k<nRows();k++){
                TextField* tf=fieldAt(k);
                if(tf&&m_rowShown[k]>=0&&(tf->hit(mx,my)||(touch&&m_rowUi[k].hit(mx,my)))) focus=k;
            }
            if(focus<0) blurFields();
            if(backHit){ close(OUT_BACK); return; }
            if(saveHit){ save(); return; }
            if(joinHit){ joinNow(); return; }
            if(delHit){
                EdenMpServerRow r;
                char body[128];
                std::snprintf(body,sizeof(body),"Remove \"%s\" from your list?",
                              eden_mp_servers_get(m_row,&r)?(r.label[0]?r.label:r.host):"this server");
                static const char* const kBtn[]={"Remove","Cancel"};
                s_confirming=this;
                GLDialog::show("Remove server",body,kBtn,2,sf_delete_cb);
                return;
            }
            if((upHit||downHit)&&m_mode==MODE_EDIT){
                const int d=upHit?-1:1;
                if(eden_mp_servers_move(m_row,d)){ m_row+=d; eden_mp_servers_save(); }
                continue;
            }
            if(decHit){ m_character=(m_character+6)%7; continue; }
            if(incHit){ m_character=(m_character+1)%7; continue; }
            if(m_mode!=MODE_PROFILE&&m_remember.hit(mx,my)){ m_remember.setOn(!m_remember.on()); continue; }
            if(focus>=0) fieldAt(focus)->focus();
        }
    }
}

void ServerFormMenu::render(){
    if(!m_open) return;
    layout();

    Pen::depthTest(false);
    Pen::blend(true);
    Pen::blendFunc(R::B_ALPHA);

    const float s = IS_IPAD ? SCALE_WIDTH : 1.0f;
    R::begin2D(SCREEN_WIDTH * s, SCREEN_HEIGHT * s, 1.0f);   // ortho 0..w, 0..h, -1..1 over an identity modelview

    using namespace GLW;
    fill(CGRectMake(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT), kScrim);
    bevel(m_panel, BEVEL_WINDOW);
    m_back.render();
    m_save.render();
    // The title gives way when it would run into Back or Save (a phone's titlebar).
    const float cx=m_panel.origin.x+m_panel.size.width*0.5f;
    const float room=std::min(m_save.rect().origin.x-cx,cx-(m_back.rect().origin.x+m_back.rect().size.width))-du(6);
    if(m_title.width()*0.5f<=room) m_title.drawCentered(cx, m_titleY, kText);

    bevel(m_content, BEVEL_CONTENT);
    for(int k=0;k<nRows();k++){
        if(m_rowShown[k]<0) continue;
        m_rowUi[k].render();
        if(TextField* tf=fieldAt(k)) tf->render();
        else if(m_mode==MODE_PROFILE) m_charStep.render();
        else m_remember.render();
    }
    if(m_footShown){
        if(m_mode==MODE_EDIT){ m_del.render(); m_up.render(); m_down.render(); }
        m_join.render();
    }

    R::end2D();

    Pen::blend(false);
    Pen::depthTest(true);
}

CGRect ServerFormMenu::controlRect(const char* which){
    layout();
    if(!which) return CGRectMake(0,0,0,0);
    if(!std::strcmp(which,"back")) return m_back.rect();
    if(!std::strcmp(which,"save")) return m_save.rect();
    if(!std::strcmp(which,"delete")) return m_del.rect();
    if(!std::strcmp(which,"up")) return m_up.rect();
    if(!std::strcmp(which,"down")) return m_down.rect();
    if(!std::strcmp(which,"joinnow")) return m_join.rect();
    if(!std::strcmp(which,"remember")) return m_remember.rect();
    if(!std::strcmp(which,"character.dec")||!std::strcmp(which,"character.inc")){
        const CGRect r=m_charStep.rect();
        const float bw=GLW::du(29);
        return !std::strcmp(which,"character.dec")?CGRectMake(r.origin.x,r.origin.y,bw,r.size.height)
                                                   :CGRectMake(r.origin.x+r.size.width-bw,r.origin.y,bw,r.size.height);
    }
    if(GLW::TextField* tf=field(which)) return tf->rect();
    return CGRectMake(0,0,0,0);
}
