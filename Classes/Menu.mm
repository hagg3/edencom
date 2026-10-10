//
//  Menu.m
//  prototype
//
//  Created by Ari Ronen on 10/24/10.
//  Copyright 2010 __MyCompanyName__. All rights reserved.
//

#import "Menu.h"
#import "GLDialog.h"
#import "Graphics.h"
#import "Globals.h"
#import "Util.h"
#import "World.h"
#import "zpipe.h"
#import "FileArchive.h"
#import "Alert.h"
#import "WorldShare.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <vector>


//@synthesize loading,showsettings,sbar,is_sharing,
//			showlistscreen,selected_world,shared_list,shareutil;
extern float SCREEN_WIDTH; 
extern float SCREEN_HEIGHT;
extern float P_ASPECT_RATIO;
#define AUTO_LOAD false


static float fade_out=0;

// ---------------------------------------------------------------------------------------------
// Stage 5.6 — the main menu on the GL widget kit
// ---------------------------------------------------------------------------------------------
// Stock drew a carousel of atlas "world blocks" with arrow buttons, five icon buttons in the
// corners (options, create, delete, share, get worlds) and two statusbar lines; a world was picked
// by tapping its block, and played by tapping it again. What changes is the drawing and the
// hit-testing; the STATE and the ACTIONS are the stock ones — world_list / selected_world, the
// `loading` ladder in render(), the create branch (now createWorld()), showAlertDeleteConfirm ->
// a_deleteConfirm, showsettings, sbar for status. So web's Menu_web.mm accessors, which drive the
// same state, are unaffected, and "tap the selected world again to play" still works.
//
// The shape is the DOM's Load World screen (web/public/eden-menu.js) under the painted title art
// (design-system.md, "Iconography": the logo and the parallax layers stay art): a WINDOW with a
// titlebar, a CONTENT list of ListRows with a scrollbar, and an action bar. Share is not offered
// (inert stubs on both hosts: ShareUtil/SharedList in seam_link_stubs*.mm). Get Worlds is, since
// Stage 5.9, wherever the host can fetch: it opens WorldBrowser (Classes/WorldBrowser.h), not the
// stock SharedList.
//
// Density follows 5.4b: rows and buttons 30u under a mouse, the 44pt floor under a finger, and
// the window shrink-wraps the list instead of filling the screen.
#define MK_MAX_ROWS 24

struct MenuKit {
    GLW::Label      title, status, empty, emptyHint;
    GLW::Button     settings, create, play, del, getWorlds;
    GLW::ListRow    rows[MK_MAX_ROWS];
    std::string     rowText[MK_MAX_ROWS];   // what each row's label was built from, so a label
    bool            rowBuilt[MK_MAX_ROWS];  // is re-rasterised only when its text changes
    GLW::ScrollView scroll;
    CGRect          panel, list;
    float           pitch, btnH, titleY;
    bool            built;
    std::string     shownStatus;
    WorldNode*      lastSelected;
    int             listTouch, barTouch, downRow;
    MenuKit() : pitch(30), btnH(30), titleY(0), built(false), lastSelected(NULL),
                listTouch(-1), barTouch(-1), downRow(-1) {
        panel=list=CGRectMake(0,0,0,0);
        for(int k=0;k<MK_MAX_ROWS;k++) rowBuilt[k]=false;
    }
};

static int menu_world_count(Menu* m){
    int n=0;
    for(WorldNode* p=m->world_list;p;p=p->next) n++;
    return n;
}
static WorldNode* menu_node_at(Menu* m,int index){
    if(index<0) return NULL;
    WorldNode* p=m->world_list;
    while(p&&index-->0) p=p->next;
    return index<0?p:NULL;
}
static int menu_index_of(Menu* m,WorldNode* node){
    int i=0;
    for(WorldNode* p=m->world_list;p;p=p->next,i++) if(p==node) return i;
    return -1;
}

// Pure rect arithmetic from SCREEN_* and the list's length; cheap, run every frame like
// SettingsMenu::layout(), so a display-profile switch or a new world re-flows on the next frame.
void Menu::layoutKit(){
    using namespace GLW;
    MenuKit* k=kit;
    if(!k->built){
        k->built=true;
        k->title.set("Worlds",du(32),UITextAlignmentCenter);
        k->settings.setLabel("Settings",du(22));
        k->create.setLabel("New",du(22));
        k->getWorlds.setLabel("Get Worlds",du(22));
        k->play.setLabel("Play",du(22));
        k->play.setTone(GLW::Button::TONE_POSITIVE);
        k->del.setLabel("Delete",du(22));
        rect_rename.setLabel("Rename",du(22));
        kit_share.setLabel("Share",du(22));
        k->empty.set("No worlds yet",du(24),UITextAlignmentCenter);
        k->emptyHint.set("Choose New to make one.",du(15),UITextAlignmentCenter,0,FACE_BODY);
    }
    const float tf=touchFloor();
    k->pitch=std::max(du(30),tf);
    k->btnH=std::max(du(30),tf);
    const float pad=du(12), gap=du(10), margin=du(12), statusH=du(26), barW=du(20);

    // The window lives between the logo (rect_name, laid out by layoutForScreen) and the status
    // line, and is as tall as its list needs — at least four rows, so a short list does not make a
    // stub of a window, and at most what fits.
    const float top=rect_name.origin.y-gap;
    const float bottom=margin+statusH;
    float cw=du(520);
    const float maxCw=SCREEN_WIDTH-margin*2.0f-pad*2.0f;
    if(cw>maxCw) cw=maxCw;
    const float fixed=pad+k->btnH+gap+gap+k->btnH+pad;
    int cap=(int)std::floor((top-bottom-fixed)/k->pitch);
    if(cap<2) cap=2;
    if(cap>MK_MAX_ROWS) cap=MK_MAX_ROWS;
    const int count=menu_world_count(this);
    int vis=std::max(count,4);
    if(vis>cap) vis=cap;
    k->scroll.setRows(count,vis);
    k->scroll.setPitch(k->pitch);

    const float listH=vis*k->pitch;
    const float ph=fixed+listH, pw=cw+pad*2.0f;
    float py=bottom+((top-bottom)-ph)*0.5f;
    if(py<bottom) py=bottom;
    k->panel=CGRectMake((SCREEN_WIDTH-pw)*0.5f,py,pw,ph);
    const float left=k->panel.origin.x+pad, right=left+cw;

    // Titlebar: Settings at the left, the title centred on the window, [Get Worlds] New at the
    // right. The title gives way when Get Worlds would run into it (renderKit).
    const float barY=py+ph-pad-k->btnH;
    k->settings.setRect(CGRectMake(left,barY,du(112),k->btnH));
    k->create.setRect(CGRectMake(right-du(80),barY,du(80),k->btnH));
    k->getWorlds.setRect(CGRectMake(right-du(80)-gap-du(140),barY,du(140),k->btnH));
    k->titleY=barY+(k->btnH+k->title.height())*0.5f;

    // The list and its scrollbar, side by side, the bar flush right.
    const float listY=barY-gap-listH;
    k->list=CGRectMake(left,listY,cw-barW-du(6),listH);
    k->scroll.setBarRect(CGRectMake(right-barW,listY,barW,listH));
    for(int r=0;r<MK_MAX_ROWS;r++){
        k->rows[r].setRect(CGRectMake(k->list.origin.x,listY+listH-(r+1)*k->pitch,k->list.size.width,k->pitch));
        k->rows[r].setLast(r==vis-1);
    }

    // Action bar: the destructive and the rare action at the left, Play at the right.
    const float actY=py+pad;
    k->del.setRect(CGRectMake(left,actY,du(96),k->btnH));
    rect_rename.setRect(CGRectMake(left+du(96)+gap,actY,du(110),k->btnH));
    kit_share.setRect(CGRectMake(left+du(96)+gap+du(110)+gap,actY,du(100),k->btnH));
    k->play.setRect(CGRectMake(right-du(120),actY,du(120),k->btnH));

    const bool idle=(loading==0);
    k->play.setEnabled(idle&&selected_world!=NULL);
    k->del.setEnabled(idle&&selected_world!=NULL);
    rect_rename.setEnabled(idle&&selected_world!=NULL);
    kit_share.setEnabled(idle&&WorldShare::offered(selected_world));
    k->create.setEnabled(idle);
    k->settings.setEnabled(idle);
    k->getWorlds.setEnabled(idle&&browserOffered());

    // Keep the selection on screen when it moved under us (New, a web accessor, a delete).
    if(selected_world!=k->lastSelected){
        k->lastSelected=selected_world;
        k->scroll.ensureVisible(menu_index_of(this,selected_world));
    }
    // Row labels: compared by TEXT, not by node or NSString pointer — a freed node or name can come
    // back at the same address — and re-rasterised only when it changed (scroll, rename, delete).
    const int first=k->scroll.first();
    for(int r=0;r<vis;r++){
        WorldNode* n=menu_node_at(this,first+r);
        std::string t=n?cpstring(n->display_name):std::string();
        if(n&&n->needs_convert)t+="  (needs conversion)";
        if(n&&n->original_bytes>0){
            // S.5: the hidden original the player kept when it was converted (Share... > Remove original).
            char b[48];
            const double v=(double)n->original_bytes;
            if(v>=1073741824.0) snprintf(b,sizeof(b),"  (+%.1f GB original kept)",v/1073741824.0);
            else snprintf(b,sizeof(b),"  (+%.0f MB original kept)",std::max(1.0,v/1048576.0));
            t+=b;
        }
        if(!k->rowBuilt[r]||t!=k->rowText[r]){
            k->rowBuilt[r]=true;
            k->rowText[r]=t;
            k->rows[r].setTitle(t.c_str(),du(22));
        }
        k->rows[r].setSelected(n!=NULL&&n==selected_world);
    }
}

// A tap on the world at `index`: select it, or — if it already is — play it (stock behaviour).
void Menu::tapRow(int index){
    WorldNode* n=menu_node_at(this,index);
    if(!n||loading) return;
    if(n==selected_world){
        loading=1;
        sbar->setStatus(@"Loading " ,9999);
    }else{
        selected_world=n;
        fnbar->setStatus(selected_world->display_name ,9999);
    }
}

CGRect Menu::controlRect(const char* which){
    layoutKit();
    if(!strcmp(which,"settings")) return kit->settings.rect();
    if(!strcmp(which,"new"))      return kit->create.rect();
    if(!strcmp(which,"play"))     return kit->play.rect();
    if(!strcmp(which,"delete"))   return kit->del.rect();
    if(!strcmp(which,"rename"))   return rect_rename.rect();
    if(!strcmp(which,"share"))    return kit_share.rect();
    if(!strcmp(which,"list"))     return kit->list;
    if(!strcmp(which,"scrollbar"))return kit->scroll.barRect();
    if(!strcmp(which,"getworlds"))return kit->getWorlds.rect();
    return CGRectMake(0,0,0,0);
}

bool Menu::rowRect(int index,CGRect* r){
    layoutKit();
    const int k=index-kit->scroll.first();
    if(index<0||k<0||k>=kit->scroll.visible()||index>=kit->scroll.total()) return false;
    if(r) *r=kit->rows[k].rect();
    return true;
}

int Menu::firstVisibleRow(){ return kit->scroll.first(); }
int Menu::visibleRows(){ return kit->scroll.visible(); }

// Split out of the constructor so the point space can change after the Menu exists — see the same
// method on Hud for why (web port audit D1/D4: SCREEN_WIDTH/SCREEN_HEIGHT are derived from the real
// window aspect and a UI-scale setting, not pinned to one device profile). Pure rect arithmetic,
// unchanged from the constructor's; idempotent. The world-list nodes are deliberately NOT re-seeded
// here: Menu::update recomputes their positions from SCREEN_WIDTH every frame anyway, so a
// re-layout that reset them would only stomp the carousel animation mid-flight.
void Menu::layoutForScreen(){
	rect_name.size.width=284*.77f;
	rect_name.size.height=83*.77f;
	rect_name.origin.x=SCREEN_WIDTH/2-rect_name.size.width/2;
    rect_name.origin.y=SCREEN_HEIGHT-rect_name.size.height-10;
    if(IS_IPAD&&!IS_RETINA){
        rect_name.origin.x=SCREEN_WIDTH/2-(591/SCALE_WIDTH)/2.0f;
        rect_name.origin.y-=10;
    }


	rect_loading.size.width=150;
	rect_loading.size.height=40;
	rect_loading.origin.x=SCREEN_WIDTH/2-rect_loading.size.width/2;
	rect_loading.origin.y=SCREEN_HEIGHT-123;
	rect_loading.size.width=SCREEN_WIDTH;
	rect_loading.origin.x=0;
    if(IS_IPAD){
        rect_loading.origin.y-=10;
    }
	sbar->pos=rect_loading;
	rect_loading.origin.y=SCREEN_HEIGHT-233;
	fnbar->pos=rect_loading;

	left_arrow.size.width=36;
	left_arrow.size.height=89;
   // if(IS_IPAD)
	//left_arrow.origin.x=20;
   // else
    left_arrow.origin.x=0;
	left_arrow.origin.y=SCREEN_HEIGHT/2-45;

	right_arrow.size.width=36;
	right_arrow.size.height=89;
     if(IS_IPAD)
	right_arrow.origin.x=SCREEN_WIDTH-41;
    else
       right_arrow.origin.x=SCREEN_WIDTH-36;
	right_arrow.origin.y=SCREEN_HEIGHT/2-45;



	rect_options.size.width=180;
	rect_options.size.height=62;
	rect_options.origin.x=(SCREEN_WIDTH)/2-90;

	rect_options.origin.y=5;

	rect_share.size.width=70;
	rect_share.size.height=70;
    if(!IS_IPAD)
	rect_share.origin.x=0;
    else{
    rect_share.origin.x=20;
    }
	rect_share.origin.y=11;


	rect_loadshared.size.width=70;
	rect_loadshared.size.height=70;
	rect_loadshared.origin.x=SCREEN_WIDTH-70;
	rect_loadshared.origin.y=11;



	rect_delete.size.width=70;
	rect_delete.size.height=70;
    if(!IS_IPAD)
        rect_delete.origin.x=0;
    else{
        rect_delete.origin.x=20;
    }
	rect_delete.origin.y=(SCREEN_HEIGHT-70);


	rect_create.size.width=70;
	rect_create.size.height=70;
	rect_create.origin.x=(SCREEN_WIDTH-70);
	rect_create.origin.y=(SCREEN_HEIGHT-70);
}

Menu::Menu(){
   
    fade_out=0;
    kit=new MenuKit();
    settings=new SettingsMenu();
    menu_back=new Menu_background();
	delete_mode=FALSE;
	share_mode=FALSE;

	sbar=new statusbar(CGRectMake(0,0,0,0));
	fnbar=new statusbar(CGRectMake(0,0,0,0));
	this->layoutForScreen();
    share_menu=new ShareMenu();
	fnbar->clear();


	world_list=NULL;
	selected_world=NULL;
	this->loadWorlds();
	
	WorldNode* node=world_list;
	while(node!=NULL){
		node->rect.size.width=70;
		node->rect.size.height=70;
		node->rect.origin.x=(SCREEN_WIDTH/2);
		node->rect.origin.y=130;
		node->tex=Resources::getResources->getMenuTex(MENU_BLOCK_UNSELECTED);
		node->anim=node->rect;
		node=node->next;
	}
	
	shareutil=[[ShareUtil alloc] init];
	showsettings=FALSE;
	is_sharing=FALSE;
    showlistscreen=FALSE;
    loading=FALSE;
	loading_world_list=0;
	
    shared_list=new SharedList();
    browser=new WorldBrowser();
    showbrowser=FALSE;
	
    
   /*  WorldNode* new_world;
    
    ///sample terrain gens
   new_world=malloc(sizeof(WorldNode));
    memset(new_world,0,sizeof(WorldNode));
    new_world->display_name=@"Mountains";
    new_world->file_name=[NSString stringWithFormat:@"%@.eden",genhash()];
    [new_world->file_name retain];
    [new_world->display_name retain];
    [self addWorld:new_world];
    selected_world=new_world;
    [sbar setStatus:[NSString stringWithFormat:@"%@ created",new_world->display_name]
                   :2];
    [fnbar setStatus:selected_world->display_name :9999];
    
    
  // WorldNode* new_world;
    new_world=malloc(sizeof(WorldNode));
    memset(new_world,0,sizeof(WorldNode));
    new_world->display_name=@"Red planet";
    new_world->file_name=[NSString stringWithFormat:@"%@.eden",genhash()];
    [new_world->file_name retain];
    [new_world->display_name retain];
    [self addWorld:new_world];
    selected_world=new_world;
    [sbar setStatus:[NSString stringWithFormat:@"%@ created",new_world->display_name]
                   :2];
    [fnbar setStatus:selected_world->display_name :9999];
    
    new_world=malloc(sizeof(WorldNode));
    memset(new_world,0,sizeof(WorldNode));
    new_world->display_name=@"River Trees";
    new_world->file_name=[NSString stringWithFormat:@"%@.eden",genhash()];
    [new_world->file_name retain];
    [new_world->display_name retain];
    [self addWorld:new_world];
    selected_world=new_world;
    [sbar setStatus:[NSString stringWithFormat:@"%@ created",new_world->display_name]
                   :2];
    [fnbar setStatus:selected_world->display_name :9999];
    
    
    new_world=malloc(sizeof(WorldNode));
    memset(new_world,0,sizeof(WorldNode));
    new_world->display_name=@"Ice slides";
    new_world->file_name=[NSString stringWithFormat:@"%@.eden",genhash()];
    [new_world->file_name retain];
    [new_world->display_name retain];
    [self addWorld:new_world];
    selected_world=new_world;
    [sbar setStatus:[NSString stringWithFormat:@"%@ created",new_world->display_name]
                   :2];
    [fnbar setStatus:selected_world->display_name :9999];
    
    new_world=malloc(sizeof(WorldNode));
    memset(new_world,0,sizeof(WorldNode));
    new_world->display_name=@"Ponies";
    new_world->file_name=[NSString stringWithFormat:@"%@.eden",genhash()];
    [new_world->file_name retain];
    [new_world->display_name retain];
    [self addWorld:new_world];
    selected_world=new_world;
    [sbar setStatus:[NSString stringWithFormat:@"%@ created",new_world->display_name]
                   :2];
    [fnbar setStatus:selected_world->display_name :9999];*/
    
    
    /*
     if(g_terrain_type==0){
     makeDirt();
     }else if(g_terrain_type==1){
     makeMars();
     }else if(g_terrain_type==2){
     makePonyWorld();
     }else if(g_terrain_type==3){
     makeMountains();
     }else if(g_terrain_type==4){
     makeDesert();
     }*/
    
	
}
void Menu::activate(){
    fade_out=0;
	sbar->setStatus(@"Choose world to load" ,99999);
	if(selected_world!=NULL)
	fnbar->setStatus(selected_world->display_name ,9999);
	share_menu->activate();
	shared_list->activate();
}
void Menu::deactivate(){
    sbar->clear();
    fnbar->clear();
    share_menu->deactivate();
	shared_list->deactivate();
	
}
void Menu::loadWorlds(){
	World::getWorld->fm->cleanConversionTemps();   // a killed conversion's .spill/.converting
	std::vector<std::pair<std::string,long long> > hidden_originals;   // S.5: (paired .emod, original's bytes)
	world_list=NULL;
	selected_world=NULL;
	NSError* err;
	NSArray* dirContents = [[NSFileManager defaultManager] 
							
							contentsOfDirectoryAtPath:World::getWorld->fm->documents error:&err];
    
    
    //readIndex();
	world_list_end=world_list;
    int dirc=(int)[dirContents count];
    BOOL reloadDir=FALSE;
    
  /*  for(int i=0;i<dirc;i++){
		NSString* file_name=[dirContents objectAtIndex:i];
        if([file_name hasSuffix:@".eden"]){
            if([file_name isEqualToString:@"Eden.eden"])continue;
            CompressWorld([file_name cStringUsingEncoding:NSUTF8StringEncoding]);
            reloadDir=TRUE;
        }
        
    }*/
    if(reloadDir){
        dirContents=[[NSFileManager defaultManager]
         
         contentsOfDirectoryAtPath:World::getWorld->fm->documents error:&err];
        dirc=(int)[dirContents count];
    }
    
	for(int i=0;i<dirc;i++){
		NSString* file_name=[dirContents objectAtIndex:i];
        NSLog(@"%@",file_name);
        if([file_name isEqualToString:@"Eden.eden.archive"])continue;
		//NSString* real_name=[NSString stringWithFormat:@"test%d",i];
        
        NSString* wut=[file_name pathExtension];
        wut=[wut uppercaseString];
        if([wut isEqualToString:@"PNG"]){
            continue;
        }
        // Archives are listed only where playing one converts it (S.5d; FileManager::conversionEnabled:
        // g_world_format on, not web) -- with the flag off this list is exactly what it was.
        BOOL is_archive=([wut isEqualToString:@"GZ"]||[wut isEqualToString:@"ZIP"])
            &&FileManager::conversionEnabled()&&FileManager::isArchiveName(file_name);
        if(![wut isEqualToString:@"EDEN"]&&![wut isEqualToString:@"EMOD"]&&!is_archive)
            continue;
        // Stage S / S.5: a `.eden` that already has a paired `.emod` is the same world -- the
        // `.emod` lists, the original stays hidden (the player kept it when asked, or has not been
        // asked yet). One that does not is listed and flagged: it converts when it is played.
        BOOL needs_convert=FALSE;
        if([wut isEqualToString:@"EDEN"]||is_archive){
            NSString* paired=World::getWorld->fm->pairedEmodFor(file_name);
            if(paired){
                // Hidden; its size goes on the `.emod`'s row once the list is built (below).
                struct stat sb;
                long long b=(stat([[NSString stringWithFormat:@"%@/%@",World::getWorld->fm->documents,file_name] UTF8String],&sb)==0)?(long long)sb.st_size:0;
                hidden_originals.push_back(std::make_pair(cpstring(paired),b));
                continue;
            }
            needs_convert=World::getWorld->fm->worldNeedsConversion(file_name);
        }
        
		NSString* real_name=World::getWorld->fm->getName(file_name);
        if(real_name==NULL){
            real_name=@"Unknown World";
        }
        
		NSLog(@"'%@'",real_name);
        if([real_name isEqualToString:@"error~"]){
            continue;
        }
        
       // file_name=[file_name stringByDeletingPathExtension];
		WorldNode* node=(WorldNode*)malloc(sizeof(WorldNode));
		memset(node, 0, sizeof(WorldNode));
		node->display_name=real_name;
		node->file_name=file_name;
		node->needs_convert=needs_convert;
		[node->display_name retain];
		[node->file_name retain];
		if(world_list_end==NULL){
			world_list_end=node;
			world_list=node;
			selected_world=node;
			fnbar->setStatus(selected_world->display_name ,9999);
		}else{
			world_list_end->next=node;
			node->prev=world_list_end;
			world_list_end=node;			
		}
		
		
	}
	for(size_t i=0;i<hidden_originals.size();i++){
		for(WorldNode* n=world_list;n;n=n->next){
			if(cpstring(n->file_name)==hidden_originals[i].first){n->original_bytes=hidden_originals[i].second;break;}
		}
	}
	// An empty Documents folder used to synthesise one placeholder world here (a name from
	// settings->getNewWorldName() and a fresh genhash() file name) so the stock iOS carousel was
	// never empty. Removed 2026-08-06 on a user report: that entry looks exactly like a saved
	// world but has no file behind it, so tapping it drops into loadWorld()'s create-a-new-world
	// branch and stops on the Flat/Normal question -- which read as "loading forever". The list is
	// now honest: nothing appears until the player creates, imports or downloads a world, and both
	// front-ends already have a real empty state for that (public/eden-menu.js's "No worlds yet"
	// card; the GL carousel simply draws nothing, its layout is already NULL-guarded).
	// selected_world stays NULL in that case -- every deref of it reachable from an empty list is
	// guarded; see Menu::refreshfn/activate and the layout block below.
}
void Menu::addWorld(WorldNode* node){
	if(world_list_end==NULL){
		world_list_end=node;
		world_list=node;
		selected_world=node;		
	}else{
		world_list_end->next=node;
		node->prev=world_list_end;
		world_list_end=node;			
	}
}
void Menu::removeWorld(WorldNode* node){
	if(node==NULL)return;
	if(node==selected_world){
		if(node->prev)
			selected_world=node->prev;
		else {
			selected_world=node->next;
		}
	}
	if(node==world_list_end)world_list_end=node->prev;
	if(node==world_list)world_list=node->next;
	if(node->prev){
		node->prev->next=node->next;
	}
	if(node->next){
		node->next->prev=node->prev;
	}
	
	free(node);
	if(selected_world)
		fnbar->setStatus(selected_world->display_name ,9999);
	else{
		fnbar->setStatus(@"" ,9999);
	}
}
static const int usage_id=7;
#define SPACE 75

// ---------------------------------------------------------------------------------------------
// N.4.5: rename the selected world
// ---------------------------------------------------------------------------------------------
// In the action bar beside Delete since Stage 5.6 (layoutKit places it); offered only where the
// host has GL text entry, so on web the bar is Delete · Play.
bool Menu::renameOffered(){
    if(!selected_world||delete_mode||share_mode||loading) return false;
    if(!GLDialog::textEntryAvailable()) return false;
    return true;
}

// Stage S / S.5: Share sits beside Rename on the hosts that have it (native; WorldShare::offered
// also wants a playable world selected and nothing running). Drawn disabled while it cannot act.
bool Menu::shareShown(){
#if defined(__EMSCRIPTEN__)
    return false;
#else
    return selected_world!=NULL&&!delete_mode&&!share_mode;
#endif
}

// ---------------------------------------------------------------------------------------------
// Stage 5.9: Get Worlds
// ---------------------------------------------------------------------------------------------
bool Menu::browserOffered(){
    return WorldBrowser::available();
}

void Menu::openBrowser(){
    if(!browserOffered()||loading) return;
    browser->open();
    showbrowser=TRUE;
}

static void menu_rename_cb(int chosen,const char* text){
    if(chosen!=0) return;                 // "Cancel" (or Escape)
    World::getWorld->menu->renameSelected(text);
}

void Menu::beginRename(){
    if(!selected_world) return;
    static const char* const kButtons[]={"Rename","Cancel"};
    std::string cur=cpstring(selected_world->display_name);
    // 49 bytes: WorldFileHeader::name is char[50] and the last byte is the terminator.
    GLDialog::prompt("Rename world",NULL,cur.c_str(),49,kButtons,2,menu_rename_cb);
}

bool Menu::renameSelected(const char* utf8){
    if(!selected_world||!utf8) return false;
    // Trim: a name of only spaces lists as a blank tile, and a trailing space is never meant.
    std::string n(utf8);
    size_t a=n.find_first_not_of(' ');
    size_t b=n.find_last_not_of(' ');
    n=(a==std::string::npos)?std::string():n.substr(a,b-a+1);
    if(n.empty()){
        sbar->setStatus(@"A world needs a name",2);
        return false;
    }
    NSString* nn=[NSString stringWithUTF8String:n.c_str()];
    if(!nn) return false;
    if(!World::getWorld->fm->renameWorld(selected_world->file_name,nn)){
        sbar->setStatus(@"Couldn't rename the world",2);
        return false;
    }
    [selected_world->display_name release];
    selected_world->display_name=nn;
    [selected_world->display_name retain];
    fnbar->setStatus(selected_world->display_name,9999);
    sbar->setStatus([NSString stringWithFormat:@"Renamed to %@",nn],2);
    return true;
}
void Menu::update(float etime){
	menu_back->update(etime);
	WorldShare::update();   // S.5: a running export/upload advances a slice per menu frame
	if(is_sharing){
		share_menu->update(etime);
		return;
	}
    if(loading){
        if(loading>=4&&LOW_MEM_DEVICE)
        fade_out+=etime/2;
        return;
    }
	if(loading_world_list){
		loading_world_list=0;
        shared_list->finished_list_dl=FALSE;
        
		[shareutil getSharedWorldList];
		showlistscreen=TRUE;
        
	}
	if(showsettings){
		settings->update(etime);
		return;
	}
	if(showbrowser){
		browser->update(etime);
		if(!browser->isOpen()) showbrowser=FALSE;   // Back, or a download handed back a world
		return;
	}
	if(showlistscreen){
		shared_list->update(etime);
		
	}
	// Stage 5.6: the kit menu's input (the stock carousel's block rects, arrows and corner buttons
	// are no longer hit-tested; see the block comment at the top of this file). Same usage_id claim
	// protocol as before: a free touch is claimed on its down edge, acted on at its release.
	layoutKit();
	MenuKit* k=kit;
	Input* input=Input::getInput();
	itouch* touches=input->getTouches();
	sbar->update(etime);
	if(int wheel=eden_ui_take_wheel()) k->scroll.scrollBy(-wheel);   // wheel away = earlier rows

	for(int i=0;i<MAX_TOUCHES;i++){
		if(touches[i].inuse==0&&touches[i].down==M_DOWN){
			touches[i].inuse=usage_id;
			const float mx=touches[i].mx, my=touches[i].my;
			k->settings.setPressed(k->settings.hit(mx,my));
			k->create.setPressed(k->create.hit(mx,my));
			k->getWorlds.setPressed(k->getWorlds.hit(mx,my));
			k->play.setPressed(k->play.hit(mx,my));
			k->del.setPressed(k->del.hit(mx,my));
			if(renameOffered()) rect_rename.setPressed(rect_rename.hit(mx,my));
			if(shareShown()) kit_share.setPressed(kit_share.hit(mx,my));
			if(loading) continue;
			if(k->barTouch<0&&k->scroll.hitBar(mx,my)){
				k->barTouch=i;
				k->scroll.barBegin(my);
			}else if(k->listTouch<0&&inbox(mx,my,k->list)){
				k->listTouch=i;
				k->scroll.beginContentDrag(my);
				k->downRow=-1;
				for(int r=0;r<k->scroll.visible();r++)
					if(k->rows[r].hit(mx,my)) k->downRow=k->scroll.first()+r;
			}
		}
		// Held: the thumb follows its touch; a list touch becomes a scroll once it travels.
		if(touches[i].inuse==usage_id&&touches[i].down==M_DOWN){
			if(i==k->barTouch) k->scroll.barDragTo(touches[i].my);
			if(i==k->listTouch&&k->scroll.contentDragTo(touches[i].my)) k->downRow=-1;
		}
		if(touches[i].inuse==usage_id&&touches[i].down==M_RELEASE){
			const float mx=touches[i].mx, my=touches[i].my;
			touches[i].inuse=0;
			touches[i].down=M_NONE;
			const bool settingsHit=k->settings.pressed()&&k->settings.hit(mx,my);
			const bool createHit=k->create.pressed()&&k->create.hit(mx,my);
			const bool getHit=k->getWorlds.pressed()&&k->getWorlds.hit(mx,my);
			const bool playHit=k->play.pressed()&&k->play.hit(mx,my);
			const bool delHit=k->del.pressed()&&k->del.hit(mx,my);
			const bool renameHit=renameOffered()&&rect_rename.pressed()&&rect_rename.hit(mx,my);
			const bool shareHit=shareShown()&&kit_share.pressed()&&kit_share.hit(mx,my);
			kit_share.setPressed(false);
			k->settings.setPressed(false); k->create.setPressed(false); k->play.setPressed(false);
			k->getWorlds.setPressed(false);
			k->del.setPressed(false); rect_rename.setPressed(false);

			if(i==k->barTouch){ k->scroll.endDrag(); k->barTouch=-1; continue; }
			if(i==k->listTouch){
				const bool scrolled=k->scroll.scrolling();
				k->scroll.endDrag();
				k->listTouch=-1;
				const int row=k->downRow;
				k->downRow=-1;
				// A tap: released on the row it went down on. A drag that came back is still a drag.
				if(!scrolled&&row>=0){
					const int r=row-k->scroll.first();
					if(r>=0&&r<k->scroll.visible()&&k->rows[r].hit(mx,my)) tapRow(row);
				}
				continue;
			}
			if(renameHit){
				beginRename();
				return;                 // the dialog owns input from the next frame
			}
			if(shareHit&&!loading&&WorldShare::offered(selected_world)){
				WorldShare::begin(selected_world);
				return;
			}
			if(settingsHit){
				settings->resetView();
				showsettings=TRUE;
				return;
			}
			if(getHit){
				openBrowser();
				return;
			}
			if(playHit&&selected_world&&loading==0){
				loading=1;
				sbar->setStatus(@"Loading " ,9999);
			}
			if(delHit&&selected_world){
				showAlertDeleteConfirm([NSString stringWithFormat:@"%@",selected_world->display_name]);
				return;
			}
			if(createHit){
				WorldNode* new_world;
				new_world=(WorldNode*)malloc(sizeof(WorldNode));
				memset(new_world,0,sizeof(WorldNode));
				new_world->display_name=settings->getNewWorldName();
				new_world->file_name=FileManager::newWorldFileName();   // .eden, or .emod (S.4 g_world_format)
				[new_world->file_name retain];
				[new_world->display_name retain];
				addWorld(new_world);
				selected_world=new_world;
				sbar->setStatus([NSString stringWithFormat:@"%@ created",new_world->display_name]
							   ,2);
				fnbar->setStatus(selected_world->display_name ,9999);
			}
		}
	}
			
	/*Input* input=[Input getInput];
	if(input.click){
		
		
	}*/
	
}

BOOL Menu::loadShared(SharedListNode* sharedNode){
    NSString* rfile_name=[NSString stringWithFormat:@"%@/%@",World::getWorld->fm->documents,sharedNode->file_name];

    const char* fname=[rfile_name cStringUsingEncoding:NSUTF8StringEncoding];
  //  NSString* new_name=[NSString stringWithFormat:@"%@/%@.archive",World::getWorld->fm.documents,sharedNode->file_name];
   // const char* cnewname=[new_name cStringUsingEncoding:NSUTF8StringEncoding];
    
    
  //  rename(fname,cnewname);
    NSString* temp_name=[NSString stringWithFormat:@"%@/temp",World::getWorld->fm->documents];
    const char* tname=[temp_name cStringUsingEncoding:NSUTF8StringEncoding];
    
    
   FILE* fsource = fopen(fname, "rb");
    if(!fsource){
        NSLog(@"cant open %s",fname);
        return FALSE;
    }
    
    FILE* fdest = fopen(tname, "wb");
    if(!fdest)
    {
        NSLog(@"cant open temp");
        fclose(fsource);
        return FALSE;
    }
    NSLog(@"source: %s\ndest: %s\n",fname,tname);
     int ret=decompressFile(fsource, fdest);
  
    
    fclose(fsource);
    fclose(fdest);
    remove(fname);
    rename(tname,fname);
    if (ret != Z_OK){
        zerr(ret);
        remove(fname);
        return FALSE;
    }
    
    
	WorldNode* new_world;
	new_world=(WorldNode*)malloc(sizeof(WorldNode));
	memset(new_world,0,sizeof(WorldNode));
	new_world->display_name=sharedNode->name;
	new_world->file_name=sharedNode->file_name;
	[new_world->file_name retain];
	[new_world->display_name retain];
	addWorld(new_world);
	selected_world=new_world;
    fnbar->setStatus(selected_world->display_name ,9999);
    
    
   // addToIndex([selected_world->file_name cStringUsingEncoding:NSUTF8StringEncoding],selected_world->display_name);
   
   // addToIndex([selected_world->file_name cStringUsingEncoding:NSUTF8StringEncoding],selected_world->display_name);
	
   
	//[shareutil loadShared:sharedNode->file_name];
	return TRUE;	
	
}
void Menu::refreshfn(){
 // selected_world is legitimately NULL now whenever no world exists (see loadWorlds).
 if(selected_world==NULL){
     fnbar->setStatus(@"" ,9999);
     return;
 }
 fnbar->setStatus(selected_world->display_name ,9999);
	
}
void Menu::renderKit(){
    using namespace GLW;
    layoutKit();
    MenuKit* k=kit;

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);

    bevel(k->panel,BEVEL_WINDOW);
    k->settings.render();
    k->create.render();
    const bool offerGet=browserOffered();
    if(offerGet) k->getWorlds.render();
    // The title is centred on the window; beside Get Worlds it may not fit between the buttons
    // (Label has no measure call, so this is the width estimate its own wrap uses, 0.42 em/char).
    const float cx=k->panel.origin.x+k->panel.size.width*0.5f;
    const float halfTitle=du(32)*0.42f*6.0f*0.5f;
    const float titleRoom=(offerGet?k->getWorlds.rect().origin.x:k->create.rect().origin.x)-du(6);
    if(cx+halfTitle<=titleRoom) k->title.drawCentered(cx,k->titleY,kText);

    bevel(k->list,BEVEL_CONTENT);
    const int vis=k->scroll.visible();
    if(k->scroll.total()==0){
        const float cx=k->list.origin.x+k->list.size.width*0.5f;
        const float h=k->empty.height()+du(6)+k->emptyHint.height();
        const float yTop=k->list.origin.y+(k->list.size.height+h)*0.5f;
        k->empty.drawCentered(cx,yTop,kText);
        k->emptyHint.drawCentered(cx,yTop-k->empty.height()-du(6),kTextSecondary);
    }else{
        for(int r=0;r<vis&&k->scroll.first()+r<k->scroll.total();r++) k->rows[r].render();
    }
    k->scroll.render();

    k->del.render();
    if(renameOffered()) rect_rename.render();
    if(shareShown()) kit_share.render();
    k->play.render();

    // The status line, over the background art: in-game chrome's inverted palette (light text,
    // dark 1u shadow — design-system.md, "In-game chrome"), centred under the window.
    NSString* st=sbar->current();
    std::string t=st?cpstring(st):std::string();
    const size_t e=t.find_last_not_of(" ");      // "Loading " has a stock trailing space
    t=(e==std::string::npos)?std::string():t.substr(0,e+1);
    if(t!=k->shownStatus){
        k->shownStatus=t;
        if(!t.empty()) k->status.set(t.c_str(),du(20),UITextAlignmentCenter);
        else k->status.clear();
    }
    if(!k->status.empty()){
        const float yTop=k->panel.origin.y-du(6);
        k->status.drawChrome(SCREEN_WIDTH*0.5f,yTop,kWhite,kBlack);
    }
    glColor4f(1.0f,1.0f,1.0f,1.0f);
}

void Menu::render(){
    Graphics::prepareMenu();
	
    
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    
    
    
		glColor4f(1.0, 1.0, 1.0, 1.0f);
	menu_back->render();
	
	
	
	if(showsettings){
		settings->render();
        Graphics::endMenu();
		
		return;
	}
	if(showbrowser){
		browser->render();
        Graphics::endMenu();
		return;
	}
	if(showlistscreen){
		shared_list->render();
        Graphics::endMenu();
		
		return;
	}
	if(is_sharing==1){
		share_menu->render();
        Graphics::endMenu();
		
		return;
	}
		glColor4f(1.0, 1.0, 1.0, 1.0f);
	//[Graphics drawRect:20:20:SCREEN_WIDTH-20:SCREEN_HEIGHT-20];
	
    if(IS_IPAD&&!IS_RETINA)
        Resources::getResources->getMenuTex(MENU_LOGO)->drawText(rect_name);
    else
	Resources::getResources->getMenuTex(MENU_LOGO)->drawInRect2(rect_name);
	// Stage 5.6: the kit window replaces the carousel, the corner icons and the two statusbar lines
	// (sbar's text is drawn by the kit as the status line; fnbar's job, naming the selection, is
	// the selected row's).
	renderKit();
    if(AUTO_LOAD&&loading==0){
       
        
        loading=2;
    }
	if(loading){
		if(loading==2){
			if(selected_world!=NULL){
				
				//NSString* wname=selected_world->file_name;
                if(World::getWorld->fm->worldExists(cpstring(selected_world->file_name),TRUE)){
				//[[World getWorld] loadWorld:wname];
                    loading=4;
                }
                else{
                    extern int g_terrain_type;
                        
                    g_terrain_type=0;
                    if([selected_world->display_name isEqualToString:@"Mountains"]){
                        g_terrain_type=3;
                    }else if([selected_world->display_name isEqualToString:@"Red planet"]){
                        g_terrain_type=1;
                    }else if([selected_world->display_name isEqualToString:@"River Trees"]){
                        g_terrain_type=2;
                    }else if([selected_world->display_name isEqualToString:@"Desert"]){
                        g_terrain_type=4;
                    }else if([selected_world->display_name isEqualToString:@"Ponies"]){
                        g_terrain_type=5;
                    }else if([selected_world->display_name isEqualToString:@"Normal"]){
                        g_terrain_type=0;
                    }
                  //  g_terrain_type=2;
                   
                    //loading=4;
                    /*new world prompt
                    if(!World::getWorld->FLIPPED){
                        [UIApplication sharedApplication].statusBarOrientation = UIInterfaceOrientationLandscapeRight;
                    }
                    else{
                        [UIApplication sharedApplication].statusBarOrientation = UIInterfaceOrientationLandscapeLeft;
                        
                    }*/
                    
                    showAlertWorldType();
                    
                    loading++;
                }
				
				//[sbar clear];
			}
		}else if(loading==4){
            if(LOW_MEM_DEVICE){
                if(fade_out>=1.0f){
                    NSString* wname=selected_world->file_name;
           
                    World::getWorld->loadWorld(wname);
                }
            }else{
                NSString* wname=selected_world->file_name;
                
                World::getWorld->loadWorld(wname);
            }

            
        }else if(loading<2){
            //NSLog(@"load world %@",selected_world->file_name);
            loading++;
        }
		
	}
    
    if(fade_out>0){
        glColor4f(0,0,0,fade_out);
        glDisable(GL_TEXTURE_2D);
        glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        if(IS_IPAD){
            if(IS_RETINA){
                Graphics::drawRect(0,0,SCREEN_WIDTH*2,SCREEN_HEIGHT*2);
            }else
                Graphics::drawRect(0,0,IPAD_WIDTH,IPAD_HEIGHT);
        }else{
            Graphics::drawRect(0,0,SCREEN_WIDTH,SCREEN_HEIGHT);
            
        }
        glEnable(GL_TEXTURE_2D);
        sbar->render();
    }
	glDisable(GL_BLEND);
    Graphics::endMenu();
    
}

void Menu::a_genFlat(BOOL b){
    World::getWorld->fm->genflat=b;
    loading++;
}
void Menu::a_deleteCancel(){
    sbar->setStatus(@"" ,2);
}
void Menu::a_deleteConfirm(){
     NSString* wname=selected_world->file_name;
     removeWorld(selected_world);
     
     if(World::getWorld->fm->deleteWorld(wname))
     sbar->setStatus(@"World deleted" ,2);
     else{
     NSLog(@"delete failed\n");
     sbar->setStatus(@"World deleted"  ,2);
     }
    
}

