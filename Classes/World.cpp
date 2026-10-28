//
//  World.m
//  prototype
//
//  Created by Ari Ronen on 10/10/1	.
//  Copyright 2010 __MyCompanyName__. All rights reserved.
//

#include "World.h"
#include "OpenGL_Internal.h"
#include "Model.h"
#include "Globals.h"
#include "F3Timing.h"
#include "PassTimers.h"   // P.1b per-pass instruments (no-ops unless a harness turns them on)
#include "TerrainGen2.h"

#include "Alert.h"
#include "GLDialog.h"
#include "ConsoleMenu.h"   // C.3 (Menu.h only forward-declares it)
#include "SignRenderer.h"
#include "Renderer.h"      // G.5b-4: the effects pass bracket
#include "CmdScript.h"
#include "MpWorld.h"       // MP.3a: the session world
#include <iostream>
#include <pthread.h>
#include "port/src/shim/foundation/platform_shims.h"   // eden_platform_now_ms, eden_platform_bundle_root

#include "VKeyboard.h"

// Web port hook (port/src/seam/LoadFailure.cpp), same cross-TU-call convention FileManager.cpp
// already uses for eden_report_load_failure -- see the doneLoading==2 check in World::loadWorld
// below for why the engine's own state machine now consults it. Only linked when the web seam
// sources are built; a from-scratch iOS build would need its own stub.
extern "C" int eden_load_failed(void);
// Web port hook (port/src/seam/DisplayProfile.cpp) — ROADMAP Phase M / M5.3. Non-zero when the
// page has flagged this device as too RAM-poor for the port's 256z ("New Dawn") worlds. loadWorld
// below refuses such a world through the same eden_report_load_failure() channel a corrupt save
// uses, so the JS recovery dialog can point the player at Settings -> Storage -> "Convert to 64z".
extern "C" int eden_low_memory(void);
extern "C" int eden_window_radius_for_world(int height);   // Settings.cpp (B.5b-2)
extern "C" void eden_report_load_failure(const char* world_file_name, const char* reason);
// ROADMAP N.4.11: R::beginScene/endScene bracket the play-mode 3D pass so it renders at the
// `render_scale` setting and is composited under a full-density HUD (FT.16: a textured quad; no-ops
// at 100%). Until FT.16 World called the port's eden_scene_pass_begin/end directly.

//@synthesize cam, terrain, player, hud,fm/*,FLIPPED*/,effects,realtime,bestGraphics,doneLoading;
//@synthesize game_mode,menu;
 bool exit_to_menu=false;

void RLETEST(){
    
    bool passed=true;
    printg("rleTest started\n");
    //rand init data;
    for(int test=0;test<100;test++){
    block8 blocks[CHUNK_SIZE3];
    color8 colors[CHUNK_SIZE3];
    
    for(int i=0;i<CHUNK_SIZE3;i++){
        blocks[i]=arc4random()%2;
        colors[i]=arc4random()%2;
    }
    
    //compress
    
    color8 rledata[CHUNK_SIZE3*3];
    int marker=-1;
    int marker_color=-1;
    int count =0;
    int dataidx=0;
    for(int i=0;i<CHUNK_SIZE3;i++){
        int t=blocks[i];
        int c=colors[i];
        if(t==marker&&c==marker_color&&count!=127){
            count++;
            
        }else{
            if(count>0){
                
                
                rledata[dataidx++]=marker;
                rledata[dataidx++]=marker_color;
               
                rledata[dataidx++]=count;
                count=0;
                marker=-1;
                marker_color=-1;
            }
            marker_color=c;
            marker=t;
            count++;
            
            
        }
    }
    if(count>0){
        rledata[dataidx++]=marker;
        rledata[dataidx++]=marker_color;
        rledata[dataidx++]=count;
        count=0;
        marker=-1;
        marker_color=-1;
    }
    
    if(dataidx>CHUNK_SIZE3*3){
        printg("dataidx overflow\n");
    }
    else {
      /*  if(dataidx/CHUNK_SIZE3>1)
            putchar('!');
        else putchar('.');*/
        
        
      //  sfh->directory_offset+=dataidx;
    }
    
    
    ///DECOMPRESS
    
    color8* buf=rledata;
    
    block8 rblocks[CHUNK_SIZE3];
    color8 rcolors[CHUNK_SIZE3];
    
        memset(rblocks,-1,CHUNK_SIZE3*sizeof(block8));
        memset(rcolors,-1,CHUNK_SIZE3*sizeof(block8));
   // NSData* data=[rcfile readDataOfLength:(CHUNK_SIZE3*3*sizeof(block8))];
    int n=CHUNK_SIZE3*3;
    //[data getBytes:buf length:n];
    
    int idx=0;
    int idx2=0;
    while(idx<n){
        int marker=buf[idx++];
        int marker_color=buf[idx++];
        int count=buf[idx++];
        //printg("count %d\n",count);
        for(int i=0;i<count;i++){
            if(idx2>CHUNK_SIZE3){
                printg("data overflow %d\n",idx2);
                //break;
            }
            rblocks[idx2]=marker;
            rcolors[idx2]=marker_color;
            idx2++;
           
            
        }
        if(idx2>=CHUNK_SIZE3){
            
            break;
            
        }
    }
        
        if(idx2>CHUNK_SIZE3)
        {printg("data overflow %d\n",idx2);
        }else if(idx2<CHUNK_SIZE3)
        {printg("data underflow\n");}
    
    
    for(int i=0;i<CHUNK_SIZE3;i++){
        if(rblocks[i]!=blocks[i]){
            passed=false;
        }
        if(rcolors[i]!=colors[i]){
            passed=false;
        }
    }
   
    
    }
    if(passed){printg("RLE passed\n");}
    else{ printg("rle failed\n");}
    
    
}
World* World::getWorld=NULL;
World::World(){
   
   res=new Resources();
    Resources::getResources=res;
   
    World::getWorld=this;
    fm=NULL;   // Hud::Hud() runs before fm is created and asks MpWorld::active() (reads fm->sessionWorld); a C++ new doesn't zero (Windows crashed, Linux got lucky)

    if(JUST_TERRAIN_GEN){
        RLETEST();
        double start=eden_platform_now_ms()/1000.0;
        printg("Terrain gen started\n");
        fm=new FileManager();
        Hud::genColorTable();
        fm->loadGenFromDisk();
        tg2_init();
        
        printg("Terrain gen finished %f\n",(eden_platform_now_ms()/1000.0-start));
        start=eden_platform_now_ms()/1000.0;
         fm->writeGenToDisk();
        printg("File write finished %f\n",(eden_platform_now_ms()/1000.0-start));
        
        
        bestGraphics=true;
        Graphics::initGraphics();
       
        return;
    }
    
    tc_initGeometry();
    game_mode=GAME_MODE_MENU;
    
    bestGraphics=true;
    terrain=new Terrain();
    Graphics::initGraphics();
    
    alert_init();
    vkeyboard_init();
    
    cam=new Camera();
    
    player=new Player(this);
    hud=new Hud();
    fm=new FileManager();
    effects=new SpecialEffects();
    menu=new Menu();
   
          
   // [NSThread detachNewThreadSelector:@selector(loadWorldThread2:) toTarget:self withObject:self];
    //[terrain startLoadingThread];
  //  FLIPPED=FALSE;
    Resources::getResources->playMenuTune();
   // [[UIDevice currentDevice] beginGeneratingDeviceOrientationNotifications];
   // [UIApplication sharedApplication].statusBarOrientation = UIInterfaceOrientationLandscapeRight;
    
     //tg2_init();
   
	/*int crapsize=1;//(16777216);
	int* crap=malloc(sizeof(int)*crapsize);
	for(int i=0;i<crapsize;i++){
		crap[i]=rand();
	}*/
	
    doneLoading=0;
	//NSLog(@"glerr:%s",gluErrorString(glGetError()));	
	
}
/*- (void)loadWorldThread2:(id)object{
    return;
    World* world=object;
    
    [NSThread setThreadPriority:0.1f];
    printg("Loading thread started, priority: %f\n",[NSThread threadPriority]);  
      int i=0;
    while(true){
        NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
       // goto cleanup;
        if(world.terrain->loaded){
            int chunkOffsetX=player->pos.x/CHUNK_SIZE-T_RADIUS;
            int chunkOffsetZ=player->pos.z/CHUNK_SIZE-T_RADIUS;
            int r=T_RADIUS;
            bool isloaded[CHUNKS_PER_SIDE_MAX][CHUNKS_PER_SIDE_MAX];
            int count=0;
                       //NSLog(@"player p
            for(int x=0;x<2*r;x++){
                for(int z=0;z<2*r;z++){
                    //	NSLog(@"lch:%d",asdf++);
                    TerrainChunk* chunk;
                    chunk=world.terrain->chunkTable[threeToOne(x+chunkOffsetX,0,z+chunkOffsetZ)];
                   // hashmap_get(world.terrain.chunkMap, threeToOne(x+chunkOffsetX, 0, z+chunkOffsetZ), (any_t)&chunk);
                    if(chunk){
                       
                        
                        if( chunk->pbounds[0]!=(x+chunkOffsetX)*CHUNK_SIZE||
                            chunk->pbounds[2]!=(z+chunkOffsetZ)*CHUNK_SIZE)
                       
                        
                        {
                            
                            //   printg("(%d,%d)=?=(%d,%d)\n",chunk.pbounds[0],chunk.pbounds[2],(x+chunkOffsetX)*CHUNK_SIZE,(z+chunkOffsetZ)*CHUNK_SIZE);  
                          
                            count++;
                            isloaded[x][z]=FALSE;
                          //  printg("overwriting a chunk\n");
                          
                        }
                          else
                        isloaded[x][z]=TRUE;
                      
                    }
                    else{
                        count++;
                        isloaded[x][z]=FALSE;
                    }
                   		
                }
            }
            if(count==0) goto cleanup;
            // printg("chunks to load:%d\n",count);
            NSString* file_name=[NSString stringWithFormat:@"%@/%@",world.fm->documents,world.terrain->world_name];
            
           
            NSFileHandle* saveFile=[NSFileHandle fileHandleForReadingAtPath:file_name];
            
            for(int x=0;x<2*r;x++){
                for(int z=0;z<2*r;z++){
                    if(!isloaded[x][z]){
                        world.fm->readColumn( x+chunkOffsetX,z+chunkOffsetZ,saveFile);
                    }
                }
            }
             
            [saveFile closeFile];
           
            
 
            
           
            
        } 
        i+=(int)sqrt(2);;
        //[NSThread sleepForTimeInterval:0.50f];
        
    cleanup: 
        [pool release];
    }
   
    //
    
    
    //do_reload=4;
   
}*/
//extern "C" loadWorldThread(void* ptr);
void* loadWorldThread(void* ptr){
    World::getWorld->terrain->loadTerrain(*(const std::string*)ptr,true);   // G.2b-2: World::loadWorld's static copy
    World::getWorld->doneLoading=2;
    
    return NULL;
}


extern int chunk_load_count;

void World::loadWorld(const std::string& nameIn){
    std::string name=nameIn;   // G.2b-2: a copy, because a finished conversion swaps in the `.emod`'s name below
    if(doneLoading==0){
        
        // Stage S / S.5: a `.eden` is converted to an `.emod` first, a time slice per call (Menu
        // keeps calling loadWorld every frame while loading==4), then the world loads from the
        // `.emod` as if it had always been one. The `.eden` is only read; when it is done the
        // player is asked below whether to delete it. If conversion cannot run (no space, an
        // unsupported header) the `.eden` simply plays in place, as it always did.
        if(!fm->sessionWorld&&(fm->conversionActive()||fm->worldNeedsConversion(name))){
            // S.5e: first, the height it converts to (the "Keep 64z?" prompt, when there is one).
            int g=fm->convertHeightGate(name);
            if(g==0){
                menu->sbar->setStatus("Converting World...",20);
                return;
            }
            if(g<0){
                menu->sbar->setStatus("",1);
                menu->loading=0;
                return;
            }
            int pct=0;
            std::string converted;
            int r=fm->convertStep(name,&pct,&converted);
            if(r==0){
                menu->sbar->setStatus(strf("Converting World... %d%%",pct),20);
                return;
            }
            if(r<0&&fm->isArchiveName(name)){
                // Nothing to fall back to: an archive can only be played converted.
                std::string why=fm->conversionError();
                menu->sbar->setStatus(strf("Could not convert this world: %s",why.empty()?"unknown error":why.c_str()),6);
                menu->loading=0;
                return;
            }
            if(r==1&&!converted.empty()){
                // G.2b-2: the load pthread below gets its own static copy of `name`, so nothing has
                // to keep `converted` alive for it any more (it was an autoreleased NSString).
                name=converted;
                WorldNode* sel=menu->selected_world;
                if(sel){
                    sel->file_name=converted;
                    sel->needs_convert=false;
                }
            }
        }
        doneLoading=1;
        Resources::getResources->stopMenuTune();
        // The world's HEIGHT has to be known before allocateMemory() sizes blockarray/lightarray
        // and the chunk table, and the only place it is written down is the save file's header.
        // Probe it here (the header, plus the directory below version 5 -- S.3b; a file we are about to open anyway); a world that does
        // not exist yet is 64z, which is what makes "new worlds stay 64z" true by construction.
        // MP.3a: a session world is always 256z (the server's y is 0..255; engine plan §2).
        int probed_height=fm->sessionWorld?T_HEIGHT_MAX:fm->probeWorldHeight(name,true);
        // ROADMAP Phase M / M5.3: a 256z world costs ~+64 MB of window arrays plus 4x the chunk
        // objects and GL buffers on top of the 64z budget. On a device the page has flagged as
        // low-memory that is very likely never viable, and a clear "too large for this device"
        // beats a tab kill with an unsaved world. Bail the same way the doneLoading==2 corrupt-load
        // check below does (terrain untouched, parked in the menu) and let eden-loaderror.js own
        // the message + the "Convert to 64z" pointer.
        if(probed_height>=T_HEIGHT_MAX && eden_low_memory()){
            eden_report_load_failure(name.c_str(),"TALL_WORLD_LOW_MEM");
            doneLoading=0;
            menu->loading=0;
            return;
        }
        eden_set_world_height(probed_height);
        // B.5b-2: the window radius, under the height's rules (set before allocateMemory, fixed for
        // the session; Constants.h). It depends on the height on web (256z is held to 12 there).
        // MP.3a: a session caps it at 9, the most one 128-block REGION lattice point covers (engine plan §6.1).
        int radius=eden_window_radius_for_world(probed_height);
        if(fm->sessionWorld&&radius>T_RADIUS_DEFAULT)radius=T_RADIUS_DEFAULT;
        eden_set_window_radius(radius);
        if(LOW_MEM_DEVICE){
            menu->deactivate();
            
            Resources::getResources->unloadMenuTextures();
            terrain->allocateMemory();
            terrain->loadTerrain(name,true);
            doneLoading=2;
            hud->fade_out=1;


        }else{
            terrain->allocateMemory();
            pthread_t foo;
            // G.2b-2: one load at a time, so one static copy outlives the thread (was the NSString*).
            static std::string threadName;
            threadName=name;
            pthread_create(&foo,NULL,loadWorldThread, &threadName);
           // std::thread first (loadWorldThread,name);
            //[NSThread detachNewThreadSelector:@selector(loadWorldThread:) toTarget:self withObject:name];
        }
        
        
    }
    if(doneLoading>=1){
        int pct=100.0f*(float)(terrain->counter)/(float)(CHUNKS_PER_SIDE*CHUNKS_PER_SIDE);   // B.5b-2: was 324
        
        if(pct>100)pct=100;
        if(fm->convertingWorld){
            menu->sbar->setStatus("Converting World...",100);
        }else{
            //if(pct==100){
               // [menu.sbar setStatus:[NSString stringWithFormat:@"Reticulating Splines... "]:20];
           // }else{
                menu->sbar->setStatus(strf("Loading World... %d%%",pct),20);
           // }
        }
        
        
        
        if(doneLoading==2){
         //   printg("done loading !\n");

            // FileManager::loadWorld() (called on the loadWorldThread pthread this doneLoading==2
            // is waiting on) bails out early -- terrain cleared, nothing read -- when the save's
            // header/directory fail its sanity check, but it has no way to tell ITS caller that;
            // it only reports failure sideways via eden_report_load_failure() (LoadFailure.h),
            // polled by the web port's own JS recovery dialog (public/eden-loaderror.js). Without
            // this check, this state machine didn't know or care and fell straight through to
            // GAME_MODE_PLAY with a Terrain that was never populated -- reproducibly crashes
            // (stack overflow / OOB) within a frame or two of rendering it. Stay parked in the
            // menu instead and let the dialog own recovery; reset menu->loading (not just
            // doneLoading) or Menu::update's loading==4 branch immediately calls loadWorld()
            // again next frame and retries the same doomed load forever.
            if(eden_load_failed()){
                doneLoading=0;
                menu->loading=0;
                return;
            }

            if(!LOW_MEM_DEVICE){
            menu->deactivate();
        
            Resources::getResources->unloadMenuTextures();
            }
            
            Resources::getResources->loadGameAssets();
            
            // Stage S / S.5: this world was just converted from a `.eden` -- ask what to do with it.
            // Skipped (original kept, hidden as paired) by EDEN_CONVERT_PROMPT=0, which headless
            // harnesses set so a modal does not sit over the HUD they drive.
            if(!fm->pendingOriginal().empty()){
                const char* e=getenv("EDEN_CONVERT_PROMPT");
                if(e&&e[0]=='0')fm->resolveOriginal(false);
                else showAlertDeleteOriginal(fm->pendingOriginal().c_str());
            }
            
            // [terrain loadTerrain:name];
            doneLoading=0;
            cam->reset();
        
            player->reset();
            game_mode=GAME_MODE_WAIT;
            target_game_mode=GAME_MODE_PLAY;
            menu->loading=0;
            menu->sbar->clear();
            
            
            
             
            if(CREATURES_ON||MpWorld::active()){   // MP.5: a session loads the models for its remote players
               
                LoadModels((std::string(eden_platform_bundle_root())+"/").c_str());
            }
            
        }
    }
	
}
void World::exitToMenu(){
    exit_to_menu=false;
   // printg("hihihi\n");
	terrain->unloadTerrain(true);
    fm->closeWorld();   // Stage S: release the `.emod` store (after unloadTerrain saved into it)
    if(CREATURES_ON||MpWorld::active()){
        UnloadModels();
    }
   // printg("loading menu textures\n");
    Resources::getResources->unloadGameAssets();
      terrain->deallocateMemory();
	Resources::getResources->loadMenuTextures();
    if(SUPPORTS_RETINA&&!IS_RETINA){
      //  printg("menu activated2\n");

        IS_IPAD=true;
        IS_RETINA=true;
       
        SCALE_WIDTH=2;
        SCALE_HEIGHT=2;
        menu->activate();
        IS_IPAD=false;
        IS_RETINA=false;
        SCALE_WIDTH=1;
        SCALE_HEIGHT=1;
        
    }else{
       // printg("menu activated\n");
        menu->activate();
   	}

	Resources::getResources->playMenuTune();
	target_game_mode=GAME_MODE_WAIT;
    game_mode=GAME_MODE_WAIT;

    fm->compressLastPlayed();
    MpWorld::endSession();   // MP.3a: leaving a session drops its overlay (nothing was saved)
   
//printg("hi\n");
	
	
}

World::~World(){
    delete terrain;
    delete player;
    delete cam;
    delete res;
    delete hud;
    delete menu;
    delete fm;
    delete effects;
	
	//[[UIDevice currentDevice] endGeneratingDeviceOrientationNotifications];
	
}
#if EDEN_F3_HOOKS
// F3 (opt-in, -DEDEN_F3_HOOKS=ON): env-gated Mac-baseline driver, F3_DRIVE="<world display name>". Opens the world,
// then 3x { Warp Home (E1), emulated straight walk of 160 blocks -x at 15 blocks/s (E2) } and exits.
// R.0: also runs on iOS via <Documents>/f3-drive.txt (see f3_drive_name).
extern "C" {
int   eden_menu_world_count(void);
const char* eden_menu_world_name(int index);
void  eden_menu_select(int index);
int   eden_menu_play(void);
}
// R.0: the driver is enabled by F3_DRIVE in the environment (Mac) OR by a marker file
// <Documents>/f3-drive.txt whose first line is the world display name (iOS: no env, no touch input).
// With neither it is inert: one getenv + one fopen at the first frame, then a static NULL.
// The marker is deleted when the run finishes so a relaunch is an ordinary launch.
extern "C" const char* eden_platform_documents_root(void);
static std::string g_f3_marker;
static const char* f3_drive_name(){
    static bool probed=false; static char buf[256]; static const char* nm=NULL;
    if(probed)return nm;
    probed=true;
    if(const char* e=getenv("F3_DRIVE")){nm=e;return nm;}
    const char* root=eden_platform_documents_root();
    if(!root)return NULL;
    g_f3_marker=std::string(root)+"/f3-drive.txt";
    if(FILE* f=fopen(g_f3_marker.c_str(),"r")){
        if(fgets(buf,sizeof buf,f)){
            size_t n=strlen(buf); while(n&&(buf[n-1]=='\n'||buf[n-1]=='\r'||buf[n-1]==' '))buf[--n]=0;
            if(n)nm=buf;
        }
        fclose(f);
    }
    if(!nm)g_f3_marker.clear();
    return nm;
}
static void f3_drive(World* w){
    const char* nm=f3_drive_name();
    if(!nm)return;
    static int phase=0,wait=0,rep=0; static float y0=0,x0=0,z0=0,walked=0;
    if(wait>0){wait--;return;}
    if(g_f3.active&&phase!=4)return;
    switch(phase){
    case 0:{ // open world from the menu
        int idx=-1;
        for(int i=0;i<eden_menu_world_count();i++)if(!strcmp(eden_menu_world_name(i),nm)){idx=i;break;}
        if(idx<0){fprintf(stderr,"[F3] drive: world '%s' not found\n",nm);fflush(stderr);if(!g_f3_marker.empty())remove(g_f3_marker.c_str());exit(2);}
        eden_menu_select(idx);
        if(eden_menu_play()!=1){fprintf(stderr,"[F3] drive: play refused\n");fflush(stderr);if(!g_f3_marker.empty())remove(g_f3_marker.c_str());exit(2);}
        phase=1; wait=600; break; }
    case 1: // settle after load (first load is not an event)
        if(w->game_mode!=GAME_MODE_PLAY||!w->terrain->loaded){wait=30;break;}
        fprintf(stderr,"[F3] drive: in world '%s', starting rep %d\n",nm,rep+1);
        phase=2; break;
    case 2: // E1
        w->terrain->warpToHome(); phase=3; wait=300; break;
    case 3:{ // E2: walk. R.4: reps 1-2 walk -x exactly as F.3 did (rep 1 comparable, rep 2 warm);
             // reps 3-5 walk +z, +x, -z from Home, i.e. into territory this process has not read.
        static const char* const kName[5]={"-x","-x","+z","+x","-z"};
        x0=w->player->pos.x; y0=62.0f; z0=w->player->pos.z; walked=0; phase=4;
        fprintf(stderr,"[F3] drive: rep %d walks %s\n",rep+1,kName[rep]); break;}
    case 4:{
        static const float kDir[5][2]={{-1,0},{-1,0},{0,1},{1,0},{0,-1}};
        walked+=0.25f;
        w->player->pos=MakeVector(x0+kDir[rep][0]*walked,y0,z0+kDir[rep][1]*walked); w->player->vel=MakeVector(0,0,0);
        if(walked>=160.0f){phase=5; wait=600;}
        break;}
    case 5:
        if(++rep>=5){fprintf(stderr,"[F3] drive: done\n");fflush(stderr);
            if(!g_f3_marker.empty())remove(g_f3_marker.c_str());exit(0);}
        phase=2; break;
    }
}
#else
static inline void f3_drive(World*){}
#endif

bool World::update(float etime){
    f3_frame_begin();
    f3_drive(this);
    PassScope _passUpdate(P_UPDATE);
    if(JUST_TERRAIN_GEN){
        return false;
    }
    if(game_mode==GAME_MODE_WAIT){
        if(SUPPORTS_RETINA&&!bestGraphics){
            return true;
        }
       
        return false;
    }
    realtime=etime;
   // NSLog(@"Hi");
	if(menu->is_sharing!=1){
	/*if([UIDevice currentDevice].orientation==UIDeviceOrientationLandscapeRight){
		//[UIApplication sharedApplication].statusBarOrientation = UIInterfaceOrientationLandscapeRight;
        if(FLIPPED==FALSE)
            [UIApplication sharedApplication].statusBarOrientation = UIInterfaceOrientationLandscapeLeft;
        
		FLIPPED=TRUE;
        
	}
	else if([UIDevice currentDevice].orientation==UIDeviceOrientationLandscapeLeft){
		//[UIApplication sharedApplication].statusBarOrientation = UIInterfaceOrientationLandscapeLeft;
        if(FLIPPED==TRUE)
          [UIApplication sharedApplication].statusBarOrientation = UIInterfaceOrientationLandscapeRight;  
		FLIPPED=FALSE;
         
		
	}*/
	}
    
	
	Resources::getResources->update(etime);
	// MP.3b: a session's socket pump, REGION pacer and join drain run every frame in every mode --
	// the server keeps talking under a modal, a menu or a load (engine plan §5).
	MpWorld::tick();
	// A GL dialog (GLDialog.h) is a full-screen modal: while one is up it owns input and the
	// screen underneath is frozen. Mirrors Menu's `if(showsettings){...;return;}` early-out.
	if(GLDialog::active()){
		GLDialog::getDialog()->update(etime);
		MpWorld::update(etime);   // MP.3b: a session can't freeze; the world events still land
		return false;
	}
	// Stage 5.5: Settings opened from the in-game (pause) menu is the same kind of modal — the
	// world is frozen under it, and Back returns to the pause menu. Only the GL pause menu sets
	// this in play (it asks eden_gl_settings_available() first), so on web it never happens.
	if(game_mode==GAME_MODE_PLAY&&menu->showsettings){
		menu->settings->update(etime);
		MpWorld::update(etime);   // MP.3b: as under a GLDialog
		return false;
	}
	// C.3: the Console is a play-only screen; leaving play closes it.
	if(menu->showconsole&&game_mode!=GAME_MODE_PLAY) menu->console->close();
	if(game_mode==GAME_MODE_MENU){
		menu->update(etime);
	}else if(game_mode==GAME_MODE_PLAY){
		
		if(etime>1.0f/20.0f)etime=1.0f/20.0f;
		// C.3: the Console runs FIRST (a submitted line executes in the input phase, like a build
		// tap) and claims every touch; the play step below then runs as usual with no player
		// input. Unlike Settings it freezes nothing.
		if(menu->showconsole) menu->console->update(etime);
        cam->update(etime);
		
        pass_begin(P_U_TERRAIN);
		terrain->update(etime);
        pass_end(P_U_TERRAIN);
        pass_begin(P_U_HUD);
        hud->update(etime);
        pass_end(P_U_HUD);
        if(game_mode==GAME_MODE_WAIT){
            return false;
        }
        pass_begin(P_U_CREATURES);
         if(CREATURES_ON&&!player->dead)
        UpdateModels(etime);
        pass_end(P_U_CREATURES);
       
        pass_begin(P_U_PLAYER);
		player->preupdate(etime);
        pass_end(P_U_PLAYER);
        pass_begin(P_U_EFFECTS);
         if(!player->dead)
		effects->update(etime);
        CmdScript::update(etime); //D.4c: command-block scripts edit before this frame's meshing
        MpWorld::update(etime);   //MP.3b step 5c: remote edits after this frame's local ones, before meshing
        pass_end(P_U_EFFECTS);
        
        pass_begin(P_U_MESH);
        terrain->prepareAndLoadGeometry();
        pass_end(P_U_MESH);
        pass_begin(P_U_UPLOAD);
        terrain->updateAllImportantChunks();
        pass_end(P_U_UPLOAD);
       
		
	}
	return false;
}


void World::render(){
    renderFrame(true);
}
// renderFrame(FALSE) draws nothing but still runs the two frame-boundary state transitions that
// have always lived in render(): the GAME_MODE_WAIT -> target_game_mode flip (update() returns
// early in WAIT, so render() is the only thing that leaves that state) and the exit_to_menu
// check. A caller that skips a frame entirely — the web port's frame-rate cap — would otherwise
// wedge the world in WAIT forever, so "skip the draw" and "skip the frame" must not be the same
// thing. render() itself is unchanged for every other caller.
void World::renderFrame(bool draw){
    if(JUST_TERRAIN_GEN&&draw){
        R::setClearColor(R::Rgba{.39f, .25f, .39f, 1.0f});
        Graphics::prepareScene();
        Graphics::prepareMenu();
        
        Pen::blend(true);
        Pen::blendFunc(R::B_ALPHA);
		Pen::color(1.0,0.0, 0.0, 1.0f);
        Pen::textured(false);
        
        tg2_render();
        /* int size=1;
        for(int x=0;x<1024;x++){
            for(int y=0;y<768;y++){
                glColor4f(randf(1.0f),0,0,.05f);
                [Graphics drawRect:x*size:y*size:x*size+15:y*size+15];
            }
        }*/
        
        
        Pen::textured(true);
        Pen::blend(false);
        Graphics::endMenu();
        
        
        //printg("hi");
    }
    if(game_mode==GAME_MODE_WAIT){
       
        game_mode=target_game_mode;
        if(target_game_mode==GAME_MODE_WAIT)target_game_mode=GAME_MODE_MENU;
        return;
    }
	if(!draw){
        //frame's draw half skipped by the caller; the exit_to_menu check below still runs
	}else if(game_mode==GAME_MODE_MENU){
       // [[World getWorld] loadWorld:menu.selected_world->file_name];	
		menu->render();
	}else if(game_mode==GAME_MODE_PLAY){
        pass_begin(P_RENDER);
        pass_begin(P_R_SETUP);
        R::beginScene(); //render-scale: the 3D pass may draw offscreen at its own resolution (FT.16: via R::)
        Graphics::prepareScene();
		
        
        cam->render();
        pass_end(P_R_SETUP);
		
       
       // [Graphics setLighting];	
       // glShadeModel(GL_SMOOTH);
      //  glDisable(GL_TEXTURE_2D);
        pass_begin(P_R_OPAQUE);
		terrain->render();
        pass_end(P_R_OPAQUE);
       // glEnable(GL_TEXTURE_2D);
        pass_begin(P_R_MODELS);
        if(CREATURES_ON||MpWorld::active())   // MP.5
            RenderModels();
        pass_end(P_R_MODELS);
        pass_begin(P_R_SIGNS);
        SignRenderer::render(); //D.3b: signs, after the opaque pass, before the sorted transparent one
        pass_end(P_R_SIGNS);
        pass_begin(P_R_TRANSPARENT);
        terrain->render2();
        pass_end(P_R_TRANSPARENT);
      //  glShadeModel(GL_FLAT);
       
       // glTranslatef([World getWorld].fm.chunkOffsetZ*CHUNK_SIZE,0,[World getWorld].fm.chunkOffsetZ*CHUNK_SIZE);
        pass_begin(P_R_EFFECTS);
        R::mv().push();   // G.5b-4: every effects draw is R:: (SpecialEffects, Firework, Player)
        R::mv().translate(-fm->chunkOffsetX*CHUNK_SIZE,0,-fm->chunkOffsetZ*CHUNK_SIZE);
		effects->render();
        terrain->fireworks->render();
        player->render();
				
         R::mv().pop();
        pass_end(P_R_EFFECTS);
        pass_begin(P_R_UPSCALE);
        R::endScene(); //composite the 3D pass into the window; the hud draws at full resolution
        pass_end(P_R_UPSCALE);
        pass_begin(P_R_HUD);
		hud->render(); //render hud last
		if(menu->showsettings) menu->settings->render();   // Stage 5.5, see update()
		if(menu->showconsole) menu->console->render();      // C.3, over the HUD
        pass_end(P_R_HUD);
        pass_end(P_RENDER);

	}

	// Drawn on top of whatever screen (menu or HUD) is underneath — see GLDialog.h.
	if(draw&&GLDialog::active())
		GLDialog::getDialog()->render();

	if(exit_to_menu)
        exitToMenu();
	
}

