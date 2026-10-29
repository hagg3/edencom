//
//  Terrain.m
//  prototype
//
//  Created by Ari Ronen on 10/11/10.
//  Copyright 2010 __MyCompanyName__. All rights reserved.
//

#include "Terrain.h"
#include "Frustum.h"
#include "Globals.h"
#include "Model.h"
#include "VectorUtil.h"

// The repo-root Lighting.h -- the one Eden.xcodeproj lists. A bare "Lighting.h" resolves to
// Classes/Lighting.h first, an empty stock stub with the SAME include guard (Stage R / R.3 needs
// the real one: the light store's accessors live there).
#include "../Lighting.h"
#include "SignTool.h"
#include "CmdScript.h"
#include "FileManagerHelper.h"   // fmh_defaultBandCount (Stage R / R.2b)
#include "Shadow.h"              // H.2 baked shadows
#include "MpWorld.h"             // MP.3a: the session world (no file behind the window)

// Repo-root Lighting.h is the built one (Eden.xcodeproj / web CMake); the Classes/ copy next to
// this file is a stale snapshot with no prototypes, hence these explicit decls.
void calculateLighting();
#include "F3Timing.h"
#include "PassTimers.h"   // P.1b per-pass instruments
#include "Renderer.h"     // G.5b-6: the terrain passes draw through R::
#include "port/src/shim/foundation/platform_shims.h"   // eden_platform_now_ms
bool calculateLightingSlice();
void calculateLightingSliceReset();

//@implementation Terrain
//@synthesize home,loaded,world_name,level_seed,tgen,counter,skycolor,final_skycolor,chunkTable,portals,fireworks;

#define BEDROCK_LEVEL 3;




int vertices_rendered=0;
int max_vertices=100000;
static int faces_rendered=0;
int chunks_rendered=0;
int chunks_rendered2=0;

static Terrain* singleton;

 bool* columnsToUpdate;
 bool* chunksToUpdate;
//static BOOL* chunksToUpdatefg;

static bool* chunksToUpdateImmediatley;

// Puts a chunk back on the dirty lists (ring streaming dropped its geometry).
static void redirtyChunk(int idxn){
    if(!chunksToUpdate||!columnsToUpdate)return;
    if(idxn<0||idxn>=CHUNKS_PER_SIDE*CHUNKS_PER_SIDE*CHUNKS_PER_COLUMN)return;
    chunksToUpdate[idxn]=true;
    columnsToUpdate[idxn/CHUNKS_PER_COLUMN]=true;
}


block8* blockarray;
//static color8* shadowarray;
Vector8* lightarray=NULL;   // R.3: only the self-check's dense mirror now (Lighting.h)
//static map_t chunkMapc;
TerrainChunk** chunkTablec;
static bool secondPass;
static double start; // eden_platform_now_ms() at construction (was an NSDate)
extern bool firstframe;
extern bool pendingRespawnSound;




//front face
//back face
//left face
//right face
//bot face
//top face
BurnNode* burnList;
// H.8 Experiments toggles (Settings.cpp "fire_melts_ice"/"fire_chars_vines"), off by default.
// eden_set_fire_*() add/remove IS_FLAMMABLE on the ice family / vine so burnBlock accepts them;
// the burn-expiry branch in update() then turns ice into water and vines into dark stone.
bool FIRE_MELTS_ICE=false;
bool FIRE_CHARS_VINES=false;
// MP.3a: both are forced off in a multiplayer session (a client-only rule the server and retail don't
// have, engine plan §11); MpWorld re-applies on entering and leaving one.
static void eden_apply_fire_flags(void){
    static const int ice[]={TYPE_ICE,TYPE_ICE_RAMP1,TYPE_ICE_RAMP2,TYPE_ICE_RAMP3,TYPE_ICE_RAMP4,
                            TYPE_ICE_SIDE1,TYPE_ICE_SIDE2,TYPE_ICE_SIDE3,TYPE_ICE_SIDE4};
    const bool session=MpWorld::active();
    for(size_t i=0;i<sizeof(ice)/sizeof(ice[0]);i++){
        if(FIRE_MELTS_ICE&&!session)blockinfo[ice[i]]|=IS_FLAMMABLE; else blockinfo[ice[i]]&=~IS_FLAMMABLE;
    }
    if(FIRE_CHARS_VINES&&!session)blockinfo[TYPE_VINE]|=IS_FLAMMABLE; else blockinfo[TYPE_VINE]&=~IS_FLAMMABLE;
}
extern "C" void eden_set_fire_melts_ice(int on){FIRE_MELTS_ICE=on!=0;eden_apply_fire_flags();}
extern "C" void eden_set_fire_chars_vines(int on){FIRE_CHARS_VINES=on!=0;eden_apply_fire_flags();}
extern "C" void eden_reapply_fire_flags(void){eden_apply_fire_flags();}
static TreeNode troot={};
static int do_reload=0;

/*-(void) startLoadingThread{
    [NSThread detachNewThreadSelector:@selector(chunkBuildingThread:) toTarget:self withObject:self];
}*/
void genChildren(TreeNode* node){

	//NSLog(@"%d %d %d    %d %d %d",node->bounds[0],node->bounds[1],node->bounds[2]
	//	  ,node->bounds[3],node->bounds[4],node->bounds[5]);
	
	node->hasChildren=true;
	if(node->bounds[3]-node->bounds[0]<=1&&
	   node->bounds[4]-node->bounds[1]<=1&&
	   node->bounds[5]-node->bounds[2]<=1){
		return;
	}
	int half[3];
	for(int i=0;i<3;i++){
		half[i]=(node->bounds[i+3]+node->bounds[i])/2;
	}
	
	for(int j=0;j<8;j++){
		bool toosmall=false;
		TreeNode* child=(TreeNode*)malloc(sizeof(TreeNode));
		memset(child,0,sizeof(TreeNode));
		child->dataList=NULL;
		for(int k=0;k<3;k++){
			
			if( ((j+1)/(k+1))%2==0&&!(j==5&&k==2)){//picks an octrant 
				child->bounds[k]=node->bounds[k];
				child->bounds[k+3]=half[k];
			}else{
				
				child->bounds[k]=half[k];
				child->bounds[k+3]=node->bounds[k+3];
			}
			if(child->bounds[k]==child->bounds[k+3])toosmall=true;
			
		}
		//NSLog(@"%d %d %d    %d %d %d",child->bounds[0],child->bounds[1],child->bounds[2]
		//	  ,child->bounds[3],child->bounds[4],child->bounds[5]);
		
		for(int i=0;i<6;i++){
			child->rbounds[i]=child->bounds[i]*BLOCK_SIZE;	
		}
		if(!toosmall){
			
			//gentree(child);
			node->children[j]=child;
		}
		
	}
	
	
}
void removeFromTree(TreeNode* tnode,ListNode* node){
    ListNode* prev=NULL;
    ListNode* cur=tnode->dataList;
    while(cur!=NULL){
        if(cur==node){
            if(prev!=NULL){
                prev->next=cur->next;
            }else
                tnode->dataList=NULL;
           // [(NSNumber*)cur->data release];
           // free(cur);
           // printg("found and removed node\n");
            break;
        }
        prev=cur;
        cur=cur->next;
    }
}

TreeNode* addToTree(TreeNode* node,int* bounds,void* data){
	//const static int childLocation[3][8]={{0,0,0,0,1,1,1,1},
	//								     {1,1,0,0,1,1,0,0},
	//									 {1,0,1,0,1,0,1,0}};
    
        //printf("addint to tree\n");
    
	if(!node->hasChildren)genChildren(node);
	
	/*for(int i=0;i<8;i++){
		TreeNode* child=node->children[i];
		if(child==NULL)continue;
		BOOL contained=TRUE;
		for(int j=0;j<3;j++){
			if(!(child->bounds[j]<=bounds[j]&&child->bounds[j+3]>=bounds[j+3])){
				contained=FALSE;
			}
		}
		if(contained){
			return addToTree(child,bounds,data);
			
			break;
		}		
	}*/
	
		ListNode* newNode=(ListNode*)malloc(sizeof(ListNode));
    
		memset(newNode,0,sizeof(ListNode));
    newNode->dead=false;
        // (was [data retain] on an NSNumber*; the only caller is commented out, so the list is always empty)
		newNode->data=data;
		newNode->next=node->dataList;
		node->dataList=newNode;
        return node;
	
   
}
void freeTree(TreeNode* node){
	if(node==NULL)return;
	ListNode* n=node->dataList;
	while(n!=NULL){		
		ListNode* t=n->next;
		free(n);
		n=t;
	}
	node->dataList=NULL;
	if(node->hasChildren)
	for(int i=0;i<8;i++){
		freeTree(node->children[i]);
	}
}
void initTree(TreeNode* node){
	node->bounds[0]=node->bounds[2]=-T_SIZE*3;
    node->bounds[1]=-20;
	node->bounds[3]=node->bounds[5]=T_SIZE*3;
	node->bounds[4]=T_HEIGHT+20;
	
	
	for(int i=0;i<6;i++){
		node->rbounds[i]=node->bounds[i]*BLOCK_SIZE;	
	}
	
}

void Terrain::clearBlocks(){
	
	memset(blockarray,0,sizeof(block8)*T_SIZE*T_SIZE*T_HEIGHT);
  //  memset(shadowarray,0,sizeof(color8)*T_SIZE*T_SIZE);
    shadow_clear();   // H.2: the 1.x shadowarray's successor (Shadow.h)
    if(!LOW_MEM_DEVICE)
    light_storeClear();   // Stage R / R.3 (Lighting.h); was memset(lightarray,...)
}



// PVRShell functions

extern int g_offcx;
extern int g_offcz;
Terrain::Terrain(){
    g_offcx=T_SIZE*100;
    g_offcz=T_SIZE*100;
    start = eden_platform_now_ms();
    tgen=new TerrainGenerator(this);
	
    
   
    
    
    
    
    liquids=new Liquids();
    portals=new Portal();
    fireworks=new Firework();
	 initTree(&troot);
	
	singleton=this;
	loaded=false;
	world_name.clear();
	do_reload=0;
	nburn=0;
    chunkTablec=NULL;
    
	
}
TreeNode* addToTree(TreeNode* node,int* bounds,void* data);

void Terrain::allocateMemory(){
    // B.5b-2: every array below is sized from the height and the window radius of the world being
    // entered. A table left over from a different shape cannot be reused (World::exitToMenu frees
    // it, so this is insurance, not a path any caller takes today).
    static int alloc_side=0,alloc_height=0;
    if(chunkTablec!=NULL){
        if(alloc_side==CHUNKS_PER_SIDE&&alloc_height==T_HEIGHT)return;
        printf("allocateMemory: window changed shape under a live table (%d/%d -> %d/%d), reallocating\n",
               alloc_side,alloc_height,CHUNKS_PER_SIDE,T_HEIGHT);
        // deallocateMemory walks the CURRENT CHUNKS_PER_SIDE/T_HEIGHT; free with the old shape.
        const int height=T_HEIGHT,radius=T_RADIUS;
        eden_set_world_height(alloc_height);
        eden_set_window_radius(alloc_side/2);
        deallocateMemory();
        eden_set_world_height(height);
        eden_set_window_radius(radius);
    }
    alloc_side=CHUNKS_PER_SIDE;
    alloc_height=T_HEIGHT;
    chunkTablec=chunkTable=(TerrainChunk**)malloc(sizeof(TerrainChunk*)*CHUNKS_PER_SIDE*CHUNKS_PER_SIDE*CHUNKS_PER_COLUMN);
    const int tbounds[6]={-1,-1,-1,-1,-1,-1};
    for(int i=0;i<CHUNKS_PER_SIDE*CHUNKS_PER_SIDE*CHUNKS_PER_COLUMN;i++)
    {
        chunkTable[i]=new TerrainChunk(tbounds,this);
       /* TerrainChunk* chunk=chunkTable[i];
            chunk->m_treenode=addToTree(&troot,chunk->pbounds,(NSNumber*)(long)i);
            if(chunk->m_treenode){
                chunk->m_listnode=chunk->m_treenode->dataList;
            }*/
        
    }
    //memset(chunkTable,0,sizeof(TerrainChunk*)*CHUNKS_PER_SIDE*CHUNKS_PER_SIDE*CHUNKS_PER_COLUMN);
    printf("allocated and zeroed chunks");
    chunksToUpdate=(bool*)malloc(sizeof(bool)*CHUNKS_PER_SIDE*CHUNKS_PER_SIDE*CHUNKS_PER_COLUMN);
    columnsToUpdate=(bool*)malloc(sizeof(bool)*CHUNKS_PER_SIDE*CHUNKS_PER_SIDE);
    chunksToUpdateImmediatley=(bool*)malloc(sizeof(bool)*CHUNKS_PER_SIDE*CHUNKS_PER_SIDE*CHUNKS_PER_COLUMN);
    memset(chunksToUpdateImmediatley,0,sizeof(bool)*CHUNKS_PER_SIDE*CHUNKS_PER_SIDE*CHUNKS_PER_COLUMN);
    blockarray=(block8*)malloc(sizeof(block8)*(T_SIZE+1)*(T_SIZE+1)*(T_HEIGHT+1));
    shadow_allocate();   // H.2: T_SIZE*T_SIZE tops, sized like blockarray from the window radius
    if(!LOW_MEM_DEVICE)
    light_storeAllocate();   // Stage R / R.3: a brick table, not the dense lightarray (Lighting.h)
}

void Terrain::deallocateMemory(){
    for(int i=0;i<CHUNKS_PER_SIDE*CHUNKS_PER_SIDE*CHUNKS_PER_COLUMN;i++){
        if(chunkTablec[i]!=NULL){
            delete chunkTablec[i];
            
            chunkTablec[i]=NULL;
        }
    }
    free(chunkTablec);
    chunkTablec=NULL;
    chunkTable=NULL;
    
    free(chunksToUpdate);
    chunksToUpdate=NULL;
    free(columnsToUpdate);
    columnsToUpdate=NULL;
    free(chunksToUpdateImmediatley);
    chunksToUpdateImmediatley=NULL;
    free(blockarray);
    blockarray=NULL;
    shadow_free();
    if(!LOW_MEM_DEVICE)
light_storeFree();   // R.3: the only place a light brick is freed (Lighting.h)
    
}

int freeOldChunks(any_t passedIn,any_t chunkToUnload){	
	TerrainChunk* chunk=(TerrainChunk*)chunkToUnload;
    delete chunk;
	return MAP_OK;
}
int unloadChunk(any_t passedIn,any_t chunkToUnload){
	//BOOL partial=(BOOL)(int)passedIn;
	//if(partial)NSLog(@"lololol");
	TerrainChunk* chunk=(TerrainChunk*)chunkToUnload;
    delete chunk;
	return MAP_OK;
}

	
	
	

void Terrain::unloadTerrain(bool exitToMenu){
    loaded=false;
    CmdScript::reset(); //D.4c: a world that closes or warps stops every command-block script
	if(exitToMenu){
        // Only wipe the portal/firework registries when the mesh cache (troot) is ALSO being
        // discarded. Portal::addPortal only runs when a chunk's mesh is actually rebuilt
        // (TerrainChunk.mm), which most portals never get again after their first meshing unless
        // their chunk is dirtied or streams back in fresh -- so clearing the registry on every
        // unloadTerrain(FALSE) call (warpToHome, column streaming, etc., which keep the cached
        // meshes and never remesh an untouched portal's chunk again) permanently orphaned any
        // portal whose chunk wasn't freshly remeshed afterward: proximity ambience (and actual
        // teleport, Portal::enterPortal) would both silently stop finding it.
        portals->removeAllPortals();
        fireworks->removeAllFireworks();
        freeTree(&troot);
        initTree(&troot);
	//	hashmap_iterate(chunkMap, unloadChunk, NULL);
	//	hashmap_remove_all(chunkMap,FALSE);
	}
	
	//initTree(
	
	//release ur memz!
		
}

int extraGeneration(any_t passedIn,any_t chunkToGen){
	
	//TerrainChunk* chunk=(TerrainChunk*)chunkToGen;
   	
	return MAP_OK;
}
static bool update_lighting=false;

// TRUE while prepareAndLoadGeometry is part-way through a bulk window reload (the count>140 path)
// and still owes the window some columns. See the long comment at the reload itself: the reload
// used to read+mesh every stale column inside one call, which measured a 104-124 ms main-thread
// block on a teleport; it now spends a per-frame budget and takes several frames instead.
// Reset on load so a world that was mid-reload when it got unloaded can't leave this latched.
static bool bulk_reload_active=false;
// Set by the meshing pass when the per-frame budget stopped it short, i.e. that reload still owes
// the window geometry even if every column has been read.
static bool bulk_reload_meshing=false;
// B.5b-1a ring streaming (prepareAndLoadGeometry), its two follow-ups.
// A window shift has happened and its creature top-up is owed once the ring has caught up (stale
// count back to 0): the top-up's randomised ground search would otherwise put creatures on columns
// that have not streamed in yet.
static bool ring_creatures_owed=false;
// Diagnostics (eden_debug_terrain_geometry): columns the ring has landed, and bulk reloads started,
// since launch. A walk that streams leaves the second unchanged; that is gate 1's "it really rang".
int g_ring_columns_landed=0;
int g_bulk_reloads=0;
// B.5b-1b diagnostics (--evict-selftest): evict-saves the ring ran, dynamics nodes it dropped,
// autosaves the timer ran.
int g_ring_evict_saves=0;
int g_ring_dynamics_dropped=0;
int g_autosaves=0;
bool Terrain::testNoEvictSave=false;
bool Terrain::testNoDropDynamics=false;
// Stage R / R.2b: count the reload's column reads in occupied chunks (see the read loop in
// prepareAndLoadGeometry). false is the stock count of BULK_RELOAD_CHUNK_BUDGET/CHUNKS_PER_COLUMN
// columns a frame. OFF by default since R.4 (2026-10-04): on an iPad Air 2 it took the 256z walk
// reload from 27 frames to 10 but its worst frame from 33-39 ms to 69-97 ms (64z: 52-67), because
// a frame then reads up to 4x the bytes. Revisit with S.4 (.emod drops the 8 KB air bands).
// Written only by diagnostics (native --read-budget-bands=1).
bool g_read_budget_bands=false;

void updateLightingBegin(){
    if(LOW_MEM_DEVICE)return;
    update_lighting=true;
    { F3Scope _f3(&g_f3.lit_begin_ms); g_f3.lit_begin_calls++;
    light_storeClear(); }
    calculateLightingSliceReset();   // restart the sliced sweep from column 0
}

void Terrain::loadTerrain(const std::string& name,bool fromArchive){
    
    double start_time=((eden_platform_now_ms()-start)/1000.0);
	if(loaded)unloadTerrain(false);
   
    World::getWorld->hud->goldencubes=10;
	counter=0;
	//skycolor=MakeVector(-1,-1,-1);
    
    Vector v=skycolor=MakeVector(1.0,1.0,1.0);
     extern Vector colorTable[256];
    if(v_equals(final_skycolor,colorTable[14]))
        v=MakeVector(0.5,0.72,0.9);
    float clr2[4]={v.x-.03f, v.y-.03f, v.z-.03f, 1.0f};
    Graphics::setFogColor(clr2);
    
	burnList=NULL;
	nburn=0;
   
    
	world_name=name;   // G.2b-2: a copy (safe when `name` is world_name itself: warp/reload pass it)
	World::getWorld->fm->loadWorld(name,fromArchive);
    
   /* for(int x=0;x<T_SIZE;x++){
        for(int z=0;z<T_SIZE;z++){
            for(int y=T_HEIGHT-1;y>=0;y--){
                if(blockarray[x*T_SIZE*T_HEIGHT+z*T_HEIGHT+y]>0&&blockarray[x*T_SIZE*T_HEIGHT+z*T_HEIGHT+y]!=TYPE_CLOUD){
                    shadowarray[x*T_SIZE+z]=y;
                    break;
                }
            }
        }
    }*/
    
    firstframe=true;
    //hashmap_iterate(chunkMap,extraGeneration,NULL);
	//startDynamics];
   
    void updateLightingBegin();
    updateLightingBegin();
    double end_time=((eden_platform_now_ms()-start)/1000.0);
    extern bool loaded_new_terrain;
    loaded_new_terrain=true;
    
	loaded=1;
    // loadWorld just filled the whole window, so no bulk reload is outstanding -- and a latched
    // TRUE from the previous world would run a slice against a window this world never staged.
    bulk_reload_active=false;
    bulk_reload_meshing=false;
    // Likewise the ring's follow-up (B.5b-1a): a load places creatures itself.
    ring_creatures_owed=false;
    World::getWorld->hud->justLoaded=1;
    
	//NSLog(@"dict entries: %d",hashmap_length(chunkMap));
	//NSLog(@"%f",[NSThread threadPriority]);
    
    
    float ttime=end_time-start_time;
    ttime++;
  // printg("loadtime: %f  \n",ttime);
	
	
}
void Terrain::warpToPoint(float x,float z,float y){
    Vector pp;
	pp.x=(x+.5f);
	pp.z=(z+.5f);
	pp.y=(y+1);
	//World::getWorld->player.pos=pp;
    f3_begin("E1");
    World::getWorld->fm->saveWorld(pp);
	unloadTerrain(false);
    
	loadTerrain(world_name,false);
    //[World::getWorld->player reset];
    
    World::getWorld->player->groundPlayer();
}
void Terrain::warpToHome(){
	Vector pp;
	pp.x=(home.x+.5f);
	pp.z=(home.z+.5f);
	pp.y=(home.y+1);
	//World::getWorld->player.pos=pp;
    f3_begin("E1");
	World::getWorld->fm->saveWorld(pp);
	unloadTerrain(false);
    
	loadTerrain(world_name,false);
    //[World::getWorld->player reset];

    World::getWorld->player->groundPlayer();
}
void Terrain::addToUpdateList(int cx,int cy,int cz){
    if(cy<0||cy>=CHUNKS_PER_COLUMN)return;   // Q.1 F16: threeToOne doesn't wrap cy
     //issue #1 continued
    chunksToUpdate[threeToOne(cx,cy,cz)]=true;
    columnsToUpdate[getColIndex(cx,cz)]=true;
    }

void Terrain::addToUpdateList2(int cx,int cy,int cz){
    if(cy<0||cy>=CHUNKS_PER_COLUMN)return;   // Q.1 F16: threeToOne doesn't wrap cy
     //issue #1 continued
    chunksToUpdate[threeToOne(cx,cy,cz)]=true;
    columnsToUpdate[getColIndex(cx,cz)]=true;
}

int compare_front2back (const void *a, const void *b);
int compare_rebuild_order (const void *a, const void *b);
int compare_back2front (const void *a, const void *b);

 int idxrl=0;
// B.5b-2: was [13000] -- enough for any window up to 256z at radius 14, overrun at 16 (16,384 chunks;
// ASan, R16 256z walk). Sized to the largest window, like prepareAndLoadGeometry's dirty list.
TerrainChunk* rebuildList[CHUNKS_PER_SIDE_MAX*CHUNKS_PER_SIDE_MAX*CHUNKS_PER_COLUMN_MAX];
//static int sanity_test=0;
/*- (void)chunkBuildingThread:(id)object{
    return;
    
    //Terrain* ter=object;
    [NSThread setThreadPriority:.2];
    printg("Chunk Building thread started, priority: %f \n",[NSThread threadPriority]);
	//[NSThread sleepForTimeInterval:2.00f];
	
	while(TRUE){
        NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
        if(loaded]){
            int num=0;
            int list[2000];
            //issue #1 chunksToUpdate and columnsToUpdate not synchronized, simultaneous data access from this thread, and loading thread
            if(idxrl<10000){
                for(int x=0;x<CHUNKS_PER_SIDE;x++){
                    for(int z=0;z<CHUNKS_PER_SIDE;z++){
                        if(columnsToUpdate[getColIndex(x,z)]){
                            for(int y=0;y<CHUNKS_PER_COLUMN;y++){
                                if(chunksToUpdate[threeToOne(x,y,z)]){
                                    
                                    int n=threeToOne(x,y,z);
                                    
                                    if(n>=CHUNKS_PER_SIDE*CHUNKS_PER_SIDE*CHUNKS_PER_COLUMN||n<0){
                                        printg("out of bounds index: %d\n",n);
                                    }
                                    list[num++]=n;
                                    
                                    chunksToUpdate[threeToOne(x,y,z)]=FALSE;
                                }
                                
                            }
                            columnsToUpdate[getColIndex(x,z)]=FALSE;
                            // if(num>=1000){printg("1234overflow\n");break;}
                            
                        }
                    }
                }
                
                
                
                
                // goto cleanup;
                
                
                
                for(int i=0;i<num;i++){
                    TerrainChunk* chunk=NULL;
                    if(list[i]<0||list[i]>=CHUNKS_PER_SIDE*CHUNKS_PER_SIDE*CHUNKS_PER_COLUMN){
                        printg("out of bounds access list[%d]=%d  num: %d idxrl: %d  max:%d\n",i,list[i],num,idxrl, CHUNKS_PER_SIDE*CHUNKS_PER_SIDE*CHUNKS_PER_COLUMN);
                        //  continue;
                    }
                    //issue #3 continued
                    chunk=chunkTable[list[i]];
                    //=malloc(sizeof(TerrainChunk*)*CHUNKS_PER_SIDE*CHUNKS_PER_SIDE*CHUNKS_PER_COLUMN);
                    if(chunk){
                        rebuildList[idxrl++]=chunk;
                        chunk.idxn=list[i];
                    }
                }
            }
            // printg("rebuild %d\n",idx);
            if(idxrl>0){
                qsort (rebuildList, idxrl, sizeof (TerrainChunk*), compare_rebuild_order);
//                
//                 for(int x=0;x<CHUNKS_PER_SIDE;x++){
//                 for(int z=0;z<CHUNKS_PER_SIDE;z++){
//                 // if(columnsToUpdate[getColIndex(x,z)]){
//                 for(int y=0;y<CHUNKS_PER_COLUMN;y++){
//                 if(chunksToUpdatefg[threeToOne(x,y,z)])
//                 {
//                 TerrainChunk* chunk;
//                 
//                 chunk=chunkTable[threeToOne(x,y,z)];
//                 [chunk rebuild2:FALSE];
//                 
//                 if(chunk){
//                 chunksToUpdateImmediatley[threeToOne(x,y,z)]=TRUE;
//                 }
//                 chunksToUpdatefg[threeToOne(x,y,z)]=FALSE;
//                 }
//                 }
//                 
//                 }
//                 }
    
            }
            if(idxrl>0)
            for(int i=0;i<35;i++){
                
                idxrl--;
                if(idxrl!=0&&rebuildList[idxrl-1]==rebuildList[idxrl]){
                    i--;
                    continue;
                    printg("really???\n");
                }
                // printg("hi\n");
                if(rebuildList[idxrl]){
                    sanity_test++;
                    if(sanity_test!=1){
                        printg("sanity test failed\n");
                    }
                    
                    //issue #1 continued
                    if([rebuildList[idxrl] rebuild2]==-1){
                        chunksToUpdate[rebuildList[idxrl].idxn]=TRUE;
                        columnsToUpdate[rebuildList[idxrl].idxn/CHUNKS_PER_COLUMN]=TRUE;
                    }else{
                        
                        rebuildList[idxrl].needsRebuild=FALSE;
                        
                        //issue #2 chunksToUpdateImmediatley shared data access with main thread, not synchronized
                        chunksToUpdateImmediatley[rebuildList[idxrl].idxn]=TRUE;
                    }
                    sanity_test--;
                }
                
                if(idxrl==0)break;
                
            }
           // printg("idxrl:%d\n",idxrl);
            
            
		}
    cleanup:
        [pool release];
        
	}
}
*/
/*-(void) initialGenChunks{
	for(int x=0;x<T_SIZE/CHUNK_SIZE;x++){
		for(int z=0;z<T_SIZE/CHUNK_SIZE;z++){
			[tgen generateColumn:x:z];
	
		}
	}
	
//	NSLog(@"generated: %d chunks",n_chunks);
	
	
	
}*/
/*- (void)readdChunk:(TerrainChunk*)chunk:(int)cx:(int)cy:(int)cz{
	
	NSNumber* chunkIdx=[NSNumber numberWithInt:threeToOne(cx,cy,cz)];
	TerrainChunk* old=chunkTable[threeToOne(cx,cy,cz)];
    if(old)printg("overwriting something2\n");
    chunkTable[threeToOne(cx,cy,cz)]=chunk;

	//hashmap_put(chunkMap,threeToOne(cx,cy,cz),chunk);
	
	addToTree(&troot,chunk.pbounds,chunkIdx);
	
}*/

void Terrain::addChunk(TerrainChunk* chunk, int cx,int cy,int cz,bool rebuild){
    // R.2: every path that lands a column's voxels ends here, after pblocks is final --
    // FileManager::readColumn (all three band branches, and the RLE branch that can leave a band
    // short), fmh_publishColumnFromDefault, and every
    // TerrainGenerator column. So this is the one place the empty bit is computed from scratch.
    tc_refreshTypesEmpty(chunk);
    // H.2: the column's shadow tops are rescanned before the next mesh pass (Shadow.h).
    shadow_markLanded(cx,cz);
    // P.10: and its "no lightbox" memory is forgotten (Lighting.h).
    light_markLanded(cx,cz);
    // B.5b-1a: the slot's old mesh belongs to the column that just left it. Stock re-meshed every
    // landed column within a frame or so, so drawing that mesh at the new pbounds was a brief
    // glitch; now a column landing on the window's outer ring is never meshed (it is data for its
    // inner neighbour), so the old mesh would stay drawn there as a copy of terrain 288 blocks away.
    chunk->dropGeometry();

	//NSNumber* chunkIdx=[NSNumber numberWithInt:threeToOne(cx,cy,cz)];
	
     //issue #3 continued
   // TerrainChunk* old=chunkTable[threeToOne(cx,cy,cz)];
    
   /* if(old){
        if(old==chunk){
           //chunk.m_listnode->dead=TRUE;
            readdtree=FALSE;
            //removeFromTree(chunk.m_treenode,chunk.m_listnode);
          //  printg("reusing chunk\n");
        }else
        printg("ERROR:chunk overwrite error\n");
        
        //[old release];
        
    }*/
    //chunkTable[threeToOne(cx,cy,cz)]=chunk;
	//hashmap_put(chunkMap,threeToOne(cx,cy,cz),chunk);
	
	//@synchronized(chunksToUpdate){
	if(rebuild){
       addToUpdateList(cx,cy,cz);
       
        
       
             addToUpdateList(cx+1,cy,cz);
        
             addToUpdateList(cx-1,cy,cz);
       
             addToUpdateList(cx,cy,cz+1);
       
             addToUpdateList(cx,cy,cz-1);
	}
	//}
}

/*- (BOOL)setCustom:(int)x :(int)z :(int)y :(int)type :(int)color{
    if(type!=TYPE_NONE){
        if(getLandc(x/2,z/2,y/2)!=TYPE_CUSTOM){
            setLand:x/2:z/2:y/2:TYPE_CUSTOM:FALSE];
        }
    }
    int cx=x/2/CHUNK_SIZE;
    int cy=y/2/CHUNK_SIZE;
    int cz=z/2/CHUNK_SIZE;
    TerrainChunk* chunk;
    chunk=chunkTable[threeToOne(cx,cy,cz)];
    if(!chunk)return FALSE;
    
    int r=[chunk setCustom:x-cx*CHUNK_SIZE*2:z-cz*CHUNK_SIZE*2:y-cy*CHUNK_SIZE*2:type:color];
    if(r!=-1){
        setLand:x/2:z/2:y/2:r:FALSE];
        return TRUE;
    }else{
        return FALSE;
    }
    
    
    
}*/
void Terrain::setLand(int x,int z,int y,int type,bool chunkToo){
	
	if(y<0||y>=T_HEIGHT)return;
   
    const int oldtype=GBLOCK(x,z,y);
    GBLOCK(x,z,y)=type;
    // H.2: every runtime edit lands here (TerrainChunk::setLand forwards both of its branches), so
    // this is where the chunks a caster change re-shades are dirtied and the diagnostic column top
    // follows the edit (Shadow.h).
    shadow_onSetLand(x,z,y,oldtype,type);
    light_onSetLand(x,z,type);   // P.10: a new lightbox un-forgets its column (Lighting.h)

	if(chunkToo){
        int cx=x/CHUNK_SIZE;
        int cy=y/CHUNK_SIZE;
        int cz=z/CHUNK_SIZE;
        
        TerrainChunk* chunk;
        chunk=chunkTable[threeToOne(cx,cy,cz)];
        //hashmap_get(chunkMap, threeToOne(cx,cy,cz),(any_t)&chunk);
        if(!chunk){
            return;
            
        }
        
        x-=cx*CHUNK_SIZE;
        y-=cy*CHUNK_SIZE;
        z-=cz*CHUNK_SIZE;
        // extern block8* blocks2;
       /* if(chunk.pblocks[x*CHUNK_SIZE*CHUNK_SIZE+z*CHUNK_SIZE+y]==TYPE_CUSTOM){
            
            SmallBlock* sb=chunk.psblocks[x*CHUNK_SIZE*CHUNK_SIZE+z*CHUNK_SIZE+y];
            if(sb){
                printg("clearing blocks");
                for(int i=0;i<8;i++){
                    sb->blocks[i]=0;
                    sb->colors[i]=0;
                }
                
            }
        }*/
       
            chunk->pblocks[x*(CHUNK_SIZE*CHUNK_SIZE)+z*(CHUNK_SIZE)+y]=type;
        // R.2: a block landing in an all-air chunk must take it off the empty shortcut before the
        // next budget decision. Setting air leaves the bit alone; the rebuild2() scan the edit
        // triggers is what notices a chunk that is now all air (TerrainChunk.h).
        if(type)chunk->typesEmpty=false;
        chunk->modified=true;
        
		
	}
	
}
bool Terrain::setColor(int x,int z,int y, color8 color){
    if(y<0||y>=T_HEIGHT)return false;
    
   
    int cx=x/CHUNK_SIZE;
    int cy=y/CHUNK_SIZE;
    int cz=z/CHUNK_SIZE;
    
    TerrainChunk* chunk;
     chunk=chunkTable[threeToOne(cx,cy,cz)];
    //hashmap_get(chunkMap, threeToOne(cx,cy,cz),(any_t)&chunk);
    if(!chunk){
        return false;
        
    }
    
    x-=cx*CHUNK_SIZE;
    y-=cy*CHUNK_SIZE;
    z-=cz*CHUNK_SIZE;
    color8 c1=chunk->pcolors[x*(CHUNK_SIZE*CHUNK_SIZE)+z*(CHUNK_SIZE)+y];
    if(c1==color) return false;
    chunk->modified=true;
    chunk->pcolors[x*(CHUNK_SIZE*CHUNK_SIZE)+z*(CHUNK_SIZE)+y]=color;
		
	// NSLog(@"hi! %f,%f,%f",color.x,color.y,color.z);
    return true;

    
    
}
/*- (void)destroyCustom:(int)x :(int)z :(int)y{
    [World::getWorld->effects addBlockBreak:x/2.0f :z/2.0f :y/2.0f :getCustomc(x ,z ,y) :0];
	updateCustom: x: z: y: TYPE_NONE :0];
    

}*/
void Terrain::destroyBlock(int x,int z,int y){
    printg(@"%d, %d, %d",x,z,y);
    int cur=getLandc(x,z,y);
    if(cur==TYPE_GOLDEN_CUBE||cur==TYPE_BEDROCK)return;
    if(blockinfo[cur]&IS_LIQUID){
        liquids->removeSource(x,z,y,cur);
    }else{
       // [liquids checkPoint:x:z:y];
    }
    int paint=getColor(x,z,y);
    if((cur==TYPE_TNT||cur==TYPE_FIREWORK||blockinfo[cur]&IS_BLOCKTNT)||isOnFire(x,z,y)){
        paint=getColor(x,z,y);//save color so it can be used when explosion is triggered
    }
    World::getWorld->effects->addBlockBreak(x ,z ,y ,getLand(x ,z ,y),getColor(x,z,y));
    if(cur==TYPE_LIGHTBOX){
       void addlight(int xx,int zz,int yy,float brightness,Vector color);
        
        extern Vector colorTable[256];
        addlight(x,z,y,-1.0f,colorTable[paint]);
        updateChunks(x ,z ,y ,TYPE_NONE);
         refreshChunksInRadius(x,z,y,LIGHT_RADIUS);
        
    }
	
	updateChunks(x ,z ,y ,TYPE_NONE);
    setColor(x,z,y,paint);//adds color attribute back in after updatechunks clears it
    
    if(blockinfo[cur]&IS_DOOR){
        if(cur==TYPE_DOOR_TOP){
            updateChunks(x ,z ,y-1 ,TYPE_NONE);
            setColor(x,z,y-1,paint);
        }else{
            updateChunks(x ,z ,y+1 ,TYPE_NONE);
            setColor(x,z,y+1,paint);
        }
    }
    if(blockinfo[cur]&IS_PORTAL){
        printg("trying to remove portal\n");
        if(cur==TYPE_PORTAL_TOP){
            updateChunks(x ,z ,y-1 ,TYPE_NONE);
            setColor(x,z,y-1,paint);
            portals->removePortal(x,y,z);
        }else{
            updateChunks(x,z,y+1,TYPE_NONE);
            setColor(x,z,y+1,paint);
            portals->removePortal(x,y+1,z);
            
        }
    }
}

void Terrain::explodeBlock(int x,int z,int y){
    int cur=getLandc(x,z,y);
    if(cur==TYPE_GOLDEN_CUBE)return;
    if(blockinfo[cur]&IS_LIQUID){
            liquids->removeSource(x,z,y,cur);
    }else{
      //  [liquids checkPoint:x:z:y];
    }
    
    int paint=getColor(x,z,y);
    if((cur==TYPE_TNT||cur==TYPE_FIREWORK||blockinfo[cur]&IS_BLOCKTNT)||isOnFire(x,z,y)){
        paint=getColor(x,z,y);//save color so it can be used when explosion is triggered
    }
    if(cur==TYPE_LIGHTBOX){
        void addlight(int xx,int zz,int yy,float brightness,Vector color);
        
        extern Vector colorTable[256];
        //paint=getColor:x:z:y];
        addlight(x,z,y,-1.0f,colorTable[paint]);
        
        refreshChunksInRadius(x,z,y,LIGHT_RADIUS);
        
        updateChunks(x ,z ,y ,TYPE_NONE);
        
        
    }
    //[World::getWorld->effects addBlockBreak:x :z :y :getLand:x :z :y]:getColor:x:z:y]];
    updateChunks(x,z,y,TYPE_NONE);
    setColor(x,z,y,paint);
   //adds color attribute back in after updatechunks clears it
    
    if(blockinfo[cur]&IS_DOOR){
        if(cur==TYPE_DOOR_TOP){
            updateChunks(x,z,y-1,TYPE_NONE);
            setColor(x,z,y-1,paint);
            
        }else{
            updateChunks(x,z,y+1,TYPE_NONE);
            setColor(x,z,y+1,paint);
        }
    }
    if(blockinfo[cur]&IS_PORTAL){
        printg("trying to remove portal\n");
        if(cur==TYPE_PORTAL_TOP){
            updateChunks(x,z,y-1,TYPE_NONE);
            setColor(x,z,y-1,paint);
            
            portals->removePortal(x,y,z);
        }else{
            updateChunks(x,z,y+1,TYPE_NONE);
            setColor(x,z,y+1,paint);
            
            portals->removePortal(x,y+1,z);
            
        }
    }
	World::getWorld->effects->addBlockExplode(x ,z ,y ,getLand(x,z,y) ,getColor(x,z,y));
    
    //updateChunks:x :z :y :TYPE_BRICK];
	//updateChunks:x :z :y :TYPE_NONE];
}

bool isOnFire(int x ,int z, int y){
    BurnNode* n=burnList;
    while(n!=NULL){
        if(n->x==x&&n->y==y&&n->z==z){
            return true;
        }
        n=n->next;
    }
    
    return false;
}
void Terrain::burnBlock(int x,int z,int y,bool causedByExplosion){
    if(!isColumnResident(x,z))return;   // B.5b-1b: not past the window edge (it aliases)
	int type=getLandc(x, z, y);
	if(type<0)return;
	if(blockinfo[type]&IS_FLAMMABLE){
		BurnNode* n=burnList;
		while(n!=NULL){
			if(n->x==x&&n->y==y&&n->z==z){
				return;
			}
			n=n->next;
		}
		nburn++;
		
		BurnNode* node=(BurnNode*)malloc(sizeof(BurnNode));
		node->x=x;
		node->y=y;
		node->z=z;
		node->type=type;
		node->mpLit=0;
		if(type==TYPE_TNT||type==TYPE_FIREWORK||blockinfo[type]&IS_BLOCKTNT){
            if((type==TYPE_TNT||blockinfo[type]&IS_BLOCKTNT)&&causedByExplosion){
                
                if(blockinfo[type]&IS_BLOCKTNT){
                    node->life=.5f+randf(.3f);
                }else
                node->life=.5+randf(.3f);
            }
            else
            node->life=4;
		
        }else
			node->life=6;
		node->sid=Resources::getResources->startedBurn(node->life);
		node->time=node->life;	
		
		node->pid=World::getWorld->effects->addFire(x ,z ,y ,0 ,node->life+.3);
		node->next=NULL;
        updateChunks(x,z,y,type);
		
        
		BurnNode* front=burnList;
		if(front!=NULL)
			node->next=front;
		burnList=node;
		
	 }
}
/*0,0,1, //front face
0,0,-1, //back face
-1,0,0, //left face
1,0,0, //right face
0,-1,0, //bot face
0,1,0, //top face	*/
bool isRampFaceSolid[4][6]={
    {false,false,false,true,false,true},
    {true,false,false,false,false,true},
    {false,true,false,false,false,true},
    {false,false,true,false,false,true},
};
bool isSideFaceSolid[4][6]={
    {false,false,true,true,false,false},
    {true,false,false,true,false,false},
    {true,true,false,false,false,false},
    {false,true,true,false,false,false}, 
};
bool isFaceSolid(int x,int z,int y, int d){
    int dx[]={1,0,-1,0,0,0};
    int dz[]={0,1,0,-1,0,0};
    int dy[]={0,0,0,0,-1,1};
    int type=getLandc(x+dx[d],z+dz[d],y+dy[d]);
    if(type>0&&type!=TYPE_NONE){
        if(type>=TYPE_STONE_RAMP1&&type<=TYPE_ICE_RAMP4){            
            return isRampFaceSolid[type%4][d];
        }else if(type>=TYPE_STONE_SIDE1&&type<=TYPE_ICE_SIDE4){           
            return isSideFaceSolid[type%4][d];
        }else 
            return true;
    }
    return false;
}
int getRampType(int x,int z,int y, int t){
    int type=t;
   
    bool sides[4];
     int n=0;
    for(int i=0;i<4;i++){        
        sides[i]=isFaceSolid(x,z,y,i);
        if(sides[i])n++;
    }
    
    if(n==2)
    for(int i=0;i<4;i++){
        
        if(sides[i]&&sides[(i+1)%4]){
            type+=(TYPE_STONE_SIDE1-TYPE_STONE_RAMP1)+i;
           // NSLog(@"s:%d",i);
            return type;
        }
    }
    int yaw=World::getWorld->player->yaw;
    int r=0;
    yaw+=360;
    yaw%=360;
    if(yaw>=45&&yaw<=90+45){
        r=0;
    }else if(yaw>=90+45&&yaw<=180+45){
        r=1;
    }else if(yaw>=180+45&&yaw<=270+45){
        r=2;
    }else if(yaw>=270||yaw<45){
        r=3;
    }
    //NSLog(@"r:%d",r);
    type+=r;
    return type;
}
int getRampType2(int x,int z,int y, int t){
    int type=t;
    
    bool sides[4];
    int n=0;
    for(int i=0;i<4;i++){
        sides[i]=isFaceSolid(x,z,y,i);
        if(sides[i])n++;
    }
    
    if(n==2)
        for(int i=0;i<4;i++){
            
            if(sides[i]&&sides[(i+1)%4]){
                type+=(TYPE_STONE_SIDE1-TYPE_STONE_RAMP1)+i;
                // NSLog(@"s:%d",i);
                return type;
            }
        }
    if(n>2){
        if(t==TYPE_STONE_RAMP1){
            return TYPE_STONE;
            
        }if(t==TYPE_ICE_RAMP1){
            return TYPE_ICE;
            
        }if(t==TYPE_WOOD_RAMP1){
            return TYPE_WOOD;
        }if(t==TYPE_SHINGLE_RAMP1){
            return TYPE_SHINGLE;
        }
    }
    int yaw=World::getWorld->player->yaw;
    int r=0;
    yaw+=360;
    yaw%=360;
    if(yaw>=45&&yaw<=90+45){
        r=0;
    }else if(yaw>=90+45&&yaw<=180+45){
        r=1;
    }else if(yaw>=180+45&&yaw<=270+45){
        r=2;
    }else if(yaw>=270||yaw<45){
        r=3;
    }
    //NSLog(@"r:%d",r);
    type+=r;
    return type;
}
/*- (void)buildCustom:(int)x :(int)z :(int)y{
    int build=World::getWorld->hud.blocktype;
   // int type=getLandc(x,z,y);
    updateCustom:x :z :y :build :World::getWorld->hud.block_paintcolor];
    
    
}
- (void)paintCustom:(int)x :(int)z :(int)y :(int)color{
     updateCustom:x :z :y :getCustomc(x,z,y) :color];
    
}*/

void Terrain::buildBlock(int x,int z,int y){
    if(World::getWorld->hud->blocktype==TYPE_GOLDEN_CUBE){
        if(World::getWorld->hud->goldencubes<=0)return;
         printg("goldencubes %d paint color: %d\n",World::getWorld->hud->goldencubes, World::getWorld->hud->block_paintcolor);
        World::getWorld->hud->goldencubes--;
        Resources::getResources->playSound(S_TREASURE_PLACE);
       
    }
    if(y<0||y>=T_HEIGHT)return;
    int cur=getLandc(x,z,y);
    if((blockinfo[cur]&IS_LIQUID&&getLevel(cur)<4)){
               liquids->removeSource(x,z,y,cur);
    }
	int type=World::getWorld->hud->blocktype;
    if(type==TYPE_WATER||type==TYPE_LAVA)
        liquids->addSource(x,z,y);
    
    
    
    if(type==TYPE_ICE_RAMP1||type==TYPE_STONE_RAMP1||type==TYPE_WOOD_RAMP1||type==TYPE_SHINGLE_RAMP1)
    {
        type=getRampType(x,z,y,type);
        if(type%4==0)
            printg(@"type 1 yo");
        
    }
    if(type==TYPE_DOOR_TOP){
        int boty=y;
        if(getLandc(x,z,y-1)==TYPE_NONE){
            boty=y-1;
        }else if(getLandc(x,z,y+1)==TYPE_NONE){
            boty=y;
        }else return;
        
        updateChunks(x,z,boty+1,TYPE_DOOR_TOP);
        
        setColor(x ,z ,boty+1 , World::getWorld->hud->block_paintcolor );
        
        int yaw=World::getWorld->player->yaw;
        int r=0;
        yaw+=360;
        yaw%=360;
        if(yaw>=45&&yaw<=90+45){
            r=0;
        }else if(yaw>=90+45&&yaw<=180+45){
            r=1;
        }else if(yaw>=180+45&&yaw<=270+45){
            r=2;
        }else if(yaw>=270||yaw<45){
            r=3;
        }
        updateChunks(x,z,boty,TYPE_DOOR1+r);
        setColor(x,z,boty,World::getWorld->hud->block_paintcolor);
       
        
        return;
    }else if(type==TYPE_PORTAL_TOP){
        int boty=y;
        if(getLandc(x,z,y-1)==TYPE_NONE){
            boty=y-1;
        }else if(getLandc(x,z,y+1)==TYPE_NONE){
            boty=y;
        }else return;
        
        updateChunks(x ,z ,boty+1 ,TYPE_PORTAL_TOP);
        setColor(x ,z ,boty+1, World::getWorld->hud->block_paintcolor );
        
        int yaw=World::getWorld->player->yaw;
        int r=0;
        yaw+=360;
        yaw%=360;
        if(yaw>=45&&yaw<=90+45){
            r=0;
        }else if(yaw>=90+45&&yaw<=180+45){
            r=1;
        }else if(yaw>=180+45&&yaw<=270+45){
            r=2;
        }else if(yaw>=270||yaw<45){
            r=3;
        }
        
        updateChunks(x ,z ,boty ,TYPE_PORTAL1+r);
        setColor(x ,z ,boty , World::getWorld->hud->block_paintcolor );
        
        return; 
    }else if(type==TYPE_LIGHTBOX){
        void addlight(int xx,int zz,int yy,float brightness,Vector color);
       extern Vector colorTable[256];
        addlight(x,z,y,1.0f,colorTable[World::getWorld->hud->block_paintcolor]);
        
        updateChunks(x ,z ,y ,type);
        refreshChunksInRadius(x,z,y,LIGHT_RADIUS);
        setColor(x ,z ,y ,World::getWorld->hud->block_paintcolor );

    }else{
        updateChunks(x ,z ,y ,type);
         setColor(x ,z ,y ,World::getWorld->hud->block_paintcolor );
    }
    
    if(World::getWorld->hud->blocktype==TYPE_GOLDEN_CUBE){
        if(World::getWorld->hud->goldencubes<=0){
            World::getWorld->hud->goldencubes=0;
            World::getWorld->hud->blocktype=TYPE_BRICK;
            World::getWorld->hud->block_paintcolor=0;
        }
    }
}
void Terrain::paintBlock(int x,int z,int y,int color){
    
    int pos[3]={x,y,z};
	int cx,cy,cz;
    int cur=getLandc(x,z,y);

	if(cur==TYPE_LIGHTBOX){
        int pcolor=getColorc(x,z,y);
        if(setColor(x ,z ,y ,color)){
            
            void addlight(int xx,int zz,int yy,float brightness,Vector color);
            extern Vector colorTable[256];
            addlight(x,z,y,-1.0f,colorTable[pcolor]);
            addlight(x,z,y,1.0f,colorTable[color]);
            refreshChunksInRadius(x,z,y,LIGHT_RADIUS);
            
            
            
            cx=pos[0]/CHUNK_SIZE;
            cy=pos[1]/CHUNK_SIZE;
            cz=pos[2]/CHUNK_SIZE;
            addToUpdateList2(cx,cy,cz);
        }
        
        
    }
	if(setColor(x ,z ,y ,color)){
	cx=pos[0]/CHUNK_SIZE;
	cy=pos[1]/CHUNK_SIZE;
	cz=pos[2]/CHUNK_SIZE;
    addToUpdateList2(cx,cy,cz);
    }
       if(blockinfo[cur]&IS_PORTAL){
        if(cur==TYPE_PORTAL_TOP){
            
            setColor(x,z,y-1,color);
            portals->paintPortal(x,y,z,color);   // Q.1 F28: (x,y,z) y vertical
            pos[1]--;
            cx=pos[0]/CHUNK_SIZE;
            cy=pos[1]/CHUNK_SIZE;
            cz=pos[2]/CHUNK_SIZE;
            addToUpdateList2(cx,cy,cz);
        }else{
            
            setColor(x,z,y+1,color);
            portals->paintPortal(x,y+1,z,color);   // Q.1 F28
            
            pos[1]++;
            cx=pos[0]/CHUNK_SIZE;
            cy=pos[1]/CHUNK_SIZE;
            cz=pos[2]/CHUNK_SIZE;
            addToUpdateList2(cx,cy,cz);
            
        }
    }
    if(blockinfo[cur]&IS_DOOR){
        if(cur!=TYPE_DOOR_TOP){
           paintBlock(x,z,y+1,color);
        }
    }
    
}
void Terrain::refreshChunksInRadius(int x,int z,int y,int radius){
    int pos[3]={x,y,z};
	int cx,cy,cz;
	int radius2=radius*2;
    
   
	cx=pos[0]/CHUNK_SIZE;
	cy=pos[1]/CHUNK_SIZE;
	cz=pos[2]/CHUNK_SIZE;
    addToUpdateList2(cx,cy,cz);
	
	int cx2,cy2,cz2;
	for(int i=0;i<3;i++){
		pos[i]+=radius;
        
		cx2=pos[0]/CHUNK_SIZE;
		cy2=pos[1]/CHUNK_SIZE;
		cz2=pos[2]/CHUNK_SIZE;
        addToUpdateList2(cx2,cy2,cz2);
		
		pos[i]-=radius2;
		cx2=pos[0]/CHUNK_SIZE;
		cy2=pos[1]/CHUNK_SIZE;
		cz2=pos[2]/CHUNK_SIZE;
        addToUpdateList2(cx2,cy2,cz2);
		
		pos[i]+=radius;
		
	}
    for(int i=0;i<3;i++){
        int j=(i+1)%3;
            pos[i]+=radius;
            pos[j]+=radius;
            
            cx2=pos[0]/CHUNK_SIZE;
            cy2=pos[1]/CHUNK_SIZE;
            cz2=pos[2]/CHUNK_SIZE;
            addToUpdateList2(cx2,cy2,cz2);
            
            pos[i]-=radius2;
            pos[j]-=radius2;
            cx2=pos[0]/CHUNK_SIZE;
            cy2=pos[1]/CHUNK_SIZE;
            cz2=pos[2]/CHUNK_SIZE;
            addToUpdateList2(cx2,cy2,cz2);
            
            pos[i]+=radius;
            pos[j]+=radius;
        
		
	}
    for(int i=0;i<3;i++){
        int j=(i+1)%3;
        pos[i]+=radius;
        pos[j]-=radius;
        
        cx2=pos[0]/CHUNK_SIZE;
        cy2=pos[1]/CHUNK_SIZE;
        cz2=pos[2]/CHUNK_SIZE;
        addToUpdateList2(cx2,cy2,cz2);
        
        pos[i]-=radius2;
        pos[j]+=radius2;
        cx2=pos[0]/CHUNK_SIZE;
        cy2=pos[1]/CHUNK_SIZE;
        cz2=pos[2]/CHUNK_SIZE;
        addToUpdateList2(cx2,cy2,cz2);
        
        pos[i]+=radius;
        pos[j]-=radius;
        
		
	}
    for(int i=0;i<3;i++){
        int j=(i+1)%3;
        int k=(j+1)%3;
        pos[i]+=radius;
        pos[j]+=radius;
        pos[k]+=radius;
        
        cx2=pos[0]/CHUNK_SIZE;
        cy2=pos[1]/CHUNK_SIZE;
        cz2=pos[2]/CHUNK_SIZE;
        addToUpdateList2(cx2,cy2,cz2);
        
        pos[i]-=radius2;
        pos[j]-=radius2;
        pos[k]-=radius2;
        
        cx2=pos[0]/CHUNK_SIZE;
        cy2=pos[1]/CHUNK_SIZE;
        cz2=pos[2]/CHUNK_SIZE;
        addToUpdateList2(cx2,cy2,cz2);
        
        pos[i]+=radius;
        pos[j]+=radius;
        pos[k]+=radius;
        
		
	}
    for(int i=0;i<3;i++){
        int j=(i+1)%3;
        int k=(j+1)%3;
        pos[i]+=radius;
        pos[j]-=radius;
        pos[k]+=radius;
        
        cx2=pos[0]/CHUNK_SIZE;
        cy2=pos[1]/CHUNK_SIZE;
        cz2=pos[2]/CHUNK_SIZE;
        addToUpdateList2(cx2,cy2,cz2);
        
        pos[i]-=radius2;
        pos[j]+=radius2;
        pos[k]-=radius2;
        
        cx2=pos[0]/CHUNK_SIZE;
        cy2=pos[1]/CHUNK_SIZE;
        cz2=pos[2]/CHUNK_SIZE;
        addToUpdateList2(cx2,cy2,cz2);
        
        pos[i]+=radius;
        pos[j]-=radius;
        pos[k]+=radius;
        
		
	}
    for(int i=0;i<3;i++){
        int j=(i+1)%3;
        int k=(j+1)%3;
        pos[i]+=radius;
        pos[j]-=radius;
        pos[k]-=radius;
        
        cx2=pos[0]/CHUNK_SIZE;
        cy2=pos[1]/CHUNK_SIZE;
        cz2=pos[2]/CHUNK_SIZE;
        addToUpdateList2(cx2,cy2,cz2);
        
        pos[i]-=radius2;
        pos[j]+=radius2;
        pos[k]+=radius2;
        
        cx2=pos[0]/CHUNK_SIZE;
        cy2=pos[1]/CHUNK_SIZE;
        cz2=pos[2]/CHUNK_SIZE;
        addToUpdateList2(cx2,cy2,cz2);
        
        pos[i]+=radius;
        pos[j]-=radius;
        pos[k]-=radius;
        
		
	}
    for(int i=0;i<3;i++){
        int j=(i+1)%3;
        int k=(j+1)%3;
        pos[i]+=radius;
        pos[j]+=radius;
        pos[k]-=radius;
        
        cx2=pos[0]/CHUNK_SIZE;
        cy2=pos[1]/CHUNK_SIZE;
        cz2=pos[2]/CHUNK_SIZE;
        addToUpdateList2(cx2,cy2,cz2);
        
        pos[i]-=radius2;
        pos[j]-=radius2;
        pos[k]+=radius2;
        
        cx2=pos[0]/CHUNK_SIZE;
        cy2=pos[1]/CHUNK_SIZE;
        cz2=pos[2]/CHUNK_SIZE;
        addToUpdateList2(cx2,cy2,cz2);
        
        pos[i]+=radius;
        pos[j]+=radius;
        pos[k]-=radius;
        
		
	}

}
void Terrain::updateChunks(int x,int z,int y,int type){
    int pos[3]={x,y,z};
	int cx,cy,cz;
	
    if(type==TYPE_NONE){
        setColor(x,z,y,0);
        SignTool::anchorBecameAir(x,z,y); //D.3c: a live edit to air takes the signs + command block on it
    }
    
    setLand(x,z,y,type,true);
	
    
	cx=pos[0]/CHUNK_SIZE;
	cy=pos[1]/CHUNK_SIZE;
	cz=pos[2]/CHUNK_SIZE;
    addToUpdateList2(cx,cy,cz);
    
	
	int cx2,cy2,cz2;
	for(int i=0;i<3;i++){
		pos[i]++;
        
		cx2=pos[0]/CHUNK_SIZE;
		cy2=pos[1]/CHUNK_SIZE;
		cz2=pos[2]/CHUNK_SIZE;
        addToUpdateList2(cx2,cy2,cz2);
        
		
		pos[i]-=2;
		cx2=pos[0]/CHUNK_SIZE;
		cy2=pos[1]/CHUNK_SIZE;
		cz2=pos[2]/CHUNK_SIZE;
        addToUpdateList2(cx2,cy2,cz2);
		
		pos[i]++;
		
	}
}
// MP.3b: factored from CmdScript.mm's setCell (D.4c), which keeps its refusals and calls this.
void Terrain::setCellState(int x,int z,int y,int type,int color){
    void addlight(int xx,int zz,int yy,float brightness,Vector color);
    extern Vector colorTable[256];
    const int cur=getLand(x,z,y);
    if(cur<0)return;
    const int paint=color<0?0:color;
    if(blockinfo[cur]&IS_LIQUID)liquids->removeSource(x,z,y,cur);
    if(cur==TYPE_LIGHTBOX)addlight(x,z,y,-1.0f,colorTable[getColor(x,z,y)&255]);
    if(type==TYPE_LIGHTBOX)addlight(x,z,y,1.0f,colorTable[paint&255]);
    updateChunks(x,z,y,type);                    // air also drops what hangs on the block
    if(type!=TYPE_NONE||paint)setColor(x,z,y,(color8)paint);
    if(type==TYPE_WATER||type==TYPE_LAVA)liquids->addSource(x,z,y);
    if(cur==TYPE_LIGHTBOX||type==TYPE_LIGHTBOX)refreshChunksInRadius(x,z,y,LIGHT_RADIUS);
}
/*- (void)updateCustom:(int)x :(int)z :(int)y:(int)type:(int)color{
   	int pos[3]={x/2,y/2,z/2};
	int cx,cy,cz;
	
   // if(type==TYPE_NONE)
	//setColor:x:z:y:0];
	BOOL rebuildNeighbors=setCustom:x :z :y :type :color];
    
	cx=pos[0]/CHUNK_SIZE;
	cy=pos[1]/CHUNK_SIZE;
	cz=pos[2]/CHUNK_SIZE;
	 addToUpdateList2:cx:cy:cz];
	
    if(rebuildNeighbors){
	int cx2,cy2,cz2;
	for(int i=0;i<3;i++){
		pos[i]++;
				
		cx2=pos[0]/CHUNK_SIZE;
		cy2=pos[1]/CHUNK_SIZE;
		cz2=pos[2]/CHUNK_SIZE;
		 addToUpdateList2:cx2:cy2:cz2];
		
		pos[i]-=2;
		cx2=pos[0]/CHUNK_SIZE;
		cy2=pos[1]/CHUNK_SIZE;
		cz2=pos[2]/CHUNK_SIZE;
		 addToUpdateList2:cx2:cy2:cz2];
		
		pos[i]++;
		
	}
    }
	
	
	
	
}*/

float getShadow(int x,int z,int y){
    return 1.0f;
   // return .5f;
 /*   float ret=y/T_HEIGHT/2+.7f;
    for(int i=1;i<20;i++){
        if(i+y>=T_HEIGHT){
            if(ret>1)ret=1;
            return ret;
        }
        if(getLandc(x,z,y+i)!=TYPE_NONE){
            
            ret-=.05f;
           
            
        }
    }
    if(getLandc(x,z,y)==TYPE_LIGHTBOX){
      //  printg("lightarray at box:%f\n",lightarray[((x+g_offcx)%T_SIZE)*T_SIZE*T_HEIGHT+((z+g_offcz)%T_SIZE)*T_HEIGHT+y].x);
    }
    
    return 1.0f;*/
    //if(x<=0||z<=0||y<0||x>=T_SIZE-1||z>=T_SIZE-1||y>=T_HEIGHT)return 0;
    
    
    /*int count=0;
    for(int dx=-1;dx<=1;dx++)
        for(int dz=-1;dz<=1;dz++){
            //if(x+dx>=0&&x+dx<CHUNK_SIZE&&z+dz>=0&&z+dz<CHUNK_SIZE)
            if(shadowarray[((x+dx+g_offcx)%T_SIZE)*T_SIZE+((z+dz+g_offcz)%T_SIZE)]>y)count++;
        }
    float ret=100.0f*count/9.0f;
    ret=1.0f-ret;
    if(ret<0)return 0;
    return ret;*/
    
}
float calcLight(int x,int z,int y,float shadow,int coord){
    if(LOW_MEM_DEVICE)return shadow;
    // Stage R / R.3: the brick store (Lighting.h). No brick = no light, which adds exactly the 0.0f
    // the dense array's zero did, so the clamp below sees the same value. The y test is new: every
    // caller passes an in-window y, and the dense index read a NEIGHBOURING strip's light for one
    // that was not (or past the array, at its ends).
    const Vector8* lv=(y>=0&&y<T_HEIGHT)?light_get((x+g_offcx)%T_SIZE,(z+g_offcz)%T_SIZE,y):NULL;
    if(lv){
    if(coord==0)
        shadow+=(float)lv->x/64.0f;
    else if(coord==1)
        shadow+=(float)lv->y/64.0f;
    else if(coord==2)
        shadow+=(float)lv->z/64.0f;
    }
    
    
    
    if(shadow<0)shadow=0;
    if(shadow>1.5f)shadow=1.5f;
    return shadow;
}
// Q.1 F26: all three channels from one light_get (same LOW_MEM_DEVICE, y-range and [0,1.5] clamps as calcLight).
void calcLight3(int x,int z,int y,float shadow,float out[3]){
    if(LOW_MEM_DEVICE){out[0]=out[1]=out[2]=shadow;return;}
    const Vector8* lv=(y>=0&&y<T_HEIGHT)?light_get((x+g_offcx)%T_SIZE,(z+g_offcz)%T_SIZE,y):NULL;
    float a=shadow,b=shadow,c=shadow;
    if(lv){
        a+=(float)lv->x/64.0f;
        b+=(float)lv->y/64.0f;
        c+=(float)lv->z/64.0f;
    }
    out[0]=a<0?0:(a>1.5f?1.5f:a);
    out[1]=b<0?0:(b>1.5f?1.5f:b);
    out[2]=c<0?0:(c>1.5f?1.5f:c);
}
// The light at one voxel of the window, by TOROIDAL index (tx,tz in [0,T_SIZE)), for diagnostics
// (eden_debug_light_state). Answers what calcLight would add, whatever the store looks like.
Vector8 lightAtToroidal(int tx,int tz,int y){
    Vector8 v; v.x=v.y=v.z=0;
    if(LOW_MEM_DEVICE||y<0||y>=T_HEIGHT)return v;
    const Vector8* lv=light_get(tx,tz,y);
    return lv?*lv:v;
}
bool lightingSweepPending(){ return update_lighting; }
unsigned long long lightStoreBytes(){
    if(LOW_MEM_DEVICE)return 0;
    return light_storeBytes();
}
int getLandc2(int x,int z,int y){
    if(y<0||y>=T_HEIGHT)return -1;
    return GBLOCK(x,z,y);
    
    
}
/*int getCustomc(int x,int z,int y){
   
        if(getLandc(x/2,z/2,y/2)!=TYPE_CUSTOM){
            int n=getLandc(x/2,z/2,y/2);
           // printg("get custom on non-custom\n");
           return n;
            
            
        }
   
    int cx=x/2/CHUNK_SIZE;
    int cy=y/2/CHUNK_SIZE;
    int cz=z/2/CHUNK_SIZE;
    TerrainChunk* chunk;
    chunk=chunkTablec[threeToOne(cx,cy,cz)];
    if(!chunk)return FALSE;
    
    return [chunk getCustom:x-cx*CHUNK_SIZE*2:z-cz*CHUNK_SIZE*2:y-cy*CHUNK_SIZE*2];
}*/
// B.5b-1b. The toroidal arrays alias every column T_SIZE blocks apart onto one slot, so a write
// through GBLOCK/updateChunks to a column that is not resident lands in whichever column IS. Stock's
// dynamics did that at the window edge -- a fire or a flow reaching past it wrote into the opposite
// edge -- and under ring streaming it would write into the column that just landed in a departed
// one's slot. Fire, liquid and explosion spread ask this first. (A node's OWN column is
// dropDynamicsInColumn's job: it is resident by construction until the ring evicts it.)
bool isColumnResident(int x,int z){
    const int cx=x/CHUNK_SIZE,cz=z/CHUNK_SIZE;
    TerrainChunk* c=chunkTablec?chunkTablec[threeToOne(cx,0,cz)]:NULL;
    return c&&c->pbounds[0]==cx*CHUNK_SIZE&&c->pbounds[2]==cz*CHUNK_SIZE;
}
 int getLandc(int x,int z,int y){	
	//if(x<0||z<0||y<0||x>=T_SIZE||z>=T_SIZE||y>=T_HEIGHT)return -1;	
   
	return GBLOCK(x,z,y);
	/*int cx=x/CHUNK_SIZE;
	int cy=y/CHUNK_SIZE;
	int cz=z/CHUNK_SIZE;
	TerrainChunk* chunk;
	hashmap_get(chunkMapc,threeToOne(cx,cy,cz),(any_t)&chunk);
	if(!chunk)return -1;
	x-=cx*CHUNK_SIZE;
	y-=cy*CHUNK_SIZE;
	z-=cz*CHUNK_SIZE;
	return chunk.blocks[x*(CHUNK_SIZE*CHUNK_SIZE)+z*(CHUNK_SIZE)+y];	*/													
	
}
int getColorc(int x,int z,int y){
    if(y<0||y>=T_HEIGHT)return 0;
	
	int cx=x/CHUNK_SIZE;
	int cy=y/CHUNK_SIZE;
	int cz=z/CHUNK_SIZE;
	TerrainChunk* chunk;
    chunk=chunkTablec[threeToOne(cx,cy,cz)];
	//hashmap_get(chunkMap,threeToOne(cx,cy,cz),(any_t)&chunk);
	if(!chunk)return 0;
	x-=cx*CHUNK_SIZE;
	y-=cy*CHUNK_SIZE;
	z-=cz*CHUNK_SIZE;
	return chunk->pcolors[x*(CHUNK_SIZE*CHUNK_SIZE)+z*(CHUNK_SIZE)+y];
   
}

int Terrain::getLand(int x,int z,int y){
	//return -1;
     if(y<0||y>=T_HEIGHT)return -1;
	//if(x<0||z<0||y<0||x>=T_SIZE||z>=T_SIZE||y>=T_HEIGHT)return -1;	
    if(x+g_offcx<0||z+g_offcz<0){
        printg("under/overflow (%d,%d)\n",x,z);
    }
	return GBLOCK_SAFE(x,z,y);
	int cx=x/CHUNK_SIZE;
	int cy=y/CHUNK_SIZE;
	int cz=z/CHUNK_SIZE;
	TerrainChunk* chunk;
     chunk=chunkTable[threeToOne(cx,cy,cz)];
	//hashmap_get(chunkMap,threeToOne(cx,cy,cz),(any_t)&chunk);
	if(!chunk)return -1;
	x-=cx*CHUNK_SIZE;
	y-=cy*CHUNK_SIZE;
	z-=cz*CHUNK_SIZE;
	return chunk->pblocks[x*(CHUNK_SIZE*CHUNK_SIZE)+z*(CHUNK_SIZE)+y];
	
}
int Terrain::getColor(int x,int z,int y){
	//return -1;
	if(y<0||y>=T_HEIGHT)return 0;	
	
	int cx=x/CHUNK_SIZE;
	int cy=y/CHUNK_SIZE;
	int cz=z/CHUNK_SIZE;
	TerrainChunk* chunk;
     chunk=chunkTable[threeToOne(cx,cy,cz)];
	//hashmap_get(chunkMap,threeToOne(cx,cy,cz),(any_t)&chunk);
	if(!chunk)return 0;
	x-=cx*CHUNK_SIZE;
	y-=cy*CHUNK_SIZE;
	z-=cz*CHUNK_SIZE;
	return chunk->pcolors[x*(CHUNK_SIZE*CHUNK_SIZE)+z*(CHUNK_SIZE)+y];
	
}
void Terrain::shootFirework(int x,int z,int y){
    fireworks->addFirework(x,y,z,getColor(x,z,y));
    Resources::getResources->playSound(S_FIREWORK_LIFTOFF);
   // [World::getWorld->effects addCreatureVanish:x+.5f:z+.5f:y+5:getColor:x:z:y]:TYPE_TNT];
    
    destroyBlock(x ,z,y);
    //printg("shooting firework, color:%d\n",getColor:x:z:y]);
}
extern "C" const int blockTntMap[NUM_BLOCKS+1]={
    [TYPE_BTGRASS]=TYPE_GRASS,
    [TYPE_BTDARKSTONE]=TYPE_DARK_STONE,
    [TYPE_BTSTONE]=TYPE_STONE,
    [TYPE_BTDIRT]=TYPE_DIRT,
    [TYPE_BTSAND]=TYPE_SAND,
   [ TYPE_BTTNT]=TYPE_TNT,
   [ TYPE_BTWOOD]=TYPE_WOOD,
   [ TYPE_BTSHINGLE]=TYPE_SHINGLE,
   [ TYPE_BTGLASS]=TYPE_GLASS,
   [ TYPE_BTGRADIENT]=TYPE_GRADIENT,
   [ TYPE_BTTREE]=TYPE_TREE,
   [ TYPE_BTLEAVES]=TYPE_LEAVES,
   [ TYPE_BTBRICK]=TYPE_BRICK,
   [ TYPE_BTCOBBLESTONE]=TYPE_COBBLESTONE,
   [ TYPE_BTVINES]=TYPE_VINE,
   [ TYPE_BTLADDER]=TYPE_LADDER,
   [ TYPE_BTICE]=TYPE_ICE,
  [  TYPE_BTCRYSTAL]=TYPE_CRYSTAL,
  [  TYPE_BTTRAMPOLINE]=TYPE_TRAMPOLINE,
  [  TYPE_BTCLOUD]=TYPE_CLOUD,
  [  TYPE_BTSTONESIDE]=TYPE_STONE,
  [  TYPE_BTWOODSIDE]=TYPE_WOOD,
  [  TYPE_BTICESIDE]=TYPE_ICE,
  [  TYPE_BTSHINGLESIDE]=TYPE_SHINGLE,
  [  TYPE_BTFENCE]=TYPE_WEAVE,
  [  TYPE_BTWATER]=TYPE_WATER,
  [  TYPE_BTLAVA]=TYPE_LAVA,
  [  TYPE_BTFIREWORK]=TYPE_FIREWORK,
  [  TYPE_BTLIGHTBOX]=TYPE_LIGHTBOX,
  [  TYPE_BTSTEEL]=TYPE_STEEL,
};
void Terrain::blocktntexplode(int x,int z,int y,int btype){
   
  // printf("type %d  btt %d\n",type,blockTntMap[btype]);
    
    int color=getColor(x,z,y);
    //if(color!=0)
    //    Resources::getResources->playSound:S_GOOP_EXPLODE];
    //else
        Resources::getResources->playSound(S_EXPLODE);
    
    Vector v=MakeVector(x+.5f,y+.5f,z+.5f);
    
    printf("define hit: %d, %d\n",(int)sizeof(bool), (int)sizeof(bool));

    
    
    
    
    
    MMM::ExplodeModels(v,color);
  
    if(color==0){
         World::getWorld->effects->addCreatureVanish(x+.5f,z+.5f,y+.5f,-1,blockTntMap[btype]);
    }else
    World::getWorld->effects->addCreatureVanish(x+.5f,z+.5f,y+.5f,color,blockTntMap[btype]);
    
    bool painting=false;
    //BOOL building =true;
    
    if(color!=0)painting=true;
    //destroyBlock:x :z :y];
    int er=2;
    int boundleft=er;
    int boundright=er;
    int boundforward=er;
    int boundbackward=er;
    int boundtop=er;
    int boundbot=er;
    if(getLand(x-1,z,y)>0) boundleft=0;
    if(getLand(x+1,z,y)>0) boundright=0;
    if(getLand(x,z-1,y)>0) boundforward=0;
    if(getLand(x,z+1,y)>0) boundbackward=0;
    if(y+1==T_HEIGHT||getLand(x,z,y+1)>0) boundtop=0;
    if(y-1<=0||getLand(x,z,y-1)>0) boundbot=0;
    paintBlock(x ,z ,y,color);
   	for(int i=0;i<=er;i++){
        for(int j=x-boundleft;j<=x+boundright;j++){
            for(int k=z-boundforward;k<=z+boundbackward;k++){
                if(!isColumnResident(j,k))continue;   // Q.1 F17
                int yy=er-i;
                
              //  int ox=j-x;
              //  int oz=k-z;
              //  int oy=yy;
                if(abs(j-x)+abs(k-z)==er*2){
                    if(btype==TYPE_BTICESIDE||btype==TYPE_BTSHINGLESIDE||btype==TYPE_BTWOODSIDE||btype==TYPE_BTSTONESIDE)
                    continue;
                }
                if(y-yy>0&&yy<=boundbot){
                    int type=getLandc(j, k, y-yy);
                    if(type==0){
                        if(!World::getWorld->player->test(j ,y-yy ,k,1)){
                            if(btype==TYPE_BTWATER||btype==TYPE_BTLAVA)
                                liquids->addSource(j,k ,y-yy );
                            updateChunks(j,k ,y-yy ,blockTntMap[btype]);
                             paintBlock(j ,k ,y-yy,color);
                        }
                    }else if(type>0&&blockinfo[type]&IS_BLOCKTNT){
                        burnBlock(j ,k ,y-yy ,true);
                    }
                }
                if(y+yy<T_HEIGHT&&yy<=boundtop){
                    int type=getLandc(j, k, y+yy);
                    
                    if(type==0){
                        if(!World::getWorld->player->test(j ,y+yy,k,1)){
                            if(btype==TYPE_BTWATER||btype==TYPE_BTLAVA)
                                liquids->addSource(j,k ,y+yy );
                            updateChunks(j ,k ,y+yy ,blockTntMap[btype]);
                            paintBlock(j ,k ,y+yy,color);
                        }
                    }else if(type>0&&blockinfo[type]&IS_BLOCKTNT){
                        burnBlock(j ,k ,y+yy ,true);
                    }
                    
                }
                
            }
        }
        
    }
    
    if(btype==TYPE_BTICESIDE||btype==TYPE_BTSHINGLESIDE||btype==TYPE_BTWOODSIDE||btype==TYPE_BTSTONESIDE){
        int bstype=0;
        if(btype==TYPE_BTICESIDE)bstype=TYPE_ICE_RAMP1;
        if(btype==TYPE_BTSHINGLESIDE)bstype=TYPE_SHINGLE_RAMP1;
        if(btype==TYPE_BTWOODSIDE)bstype=TYPE_WOOD_RAMP1;
        if(btype==TYPE_BTSTONESIDE)bstype=TYPE_STONE_RAMP1;
    for(int i=0;i<=er;i++){
        
        for(int j=x-boundleft;j<=x+boundright;j++){
            for(int k=z-boundforward;k<=z+boundbackward;k++){
                if(!isColumnResident(j,k))continue;   // Q.1 F17
                if(abs(j-x)+abs(k-z)!=er*2)continue;
                 
                int yy=er-i;
                
                //  int ox=j-x;
                //  int oz=k-z;
                //  int oy=yy;
                
                if(y-yy>0&&yy<=boundbot){
                    int type=getLandc(j, k, y-yy);
                    if(type==0){
                        if(!World::getWorld->player->test(j ,y-yy ,k,1)){
                            updateChunks(j,k ,y-yy ,getRampType2(j,k,y-yy,bstype));
                            paintBlock(j ,k ,y-yy,color);
                        }
                    }else if(type>0&&blockinfo[type]&IS_BLOCKTNT){
                        burnBlock(j ,k ,y-yy ,true);
                    }
                }
                if(y+yy<T_HEIGHT&&yy<=boundtop){
                    int type=getLandc(j, k, y+yy);
                    
                    if(type==0){
                        if(!World::getWorld->player->test(j ,y+yy,k,1)){
                            updateChunks(j ,k ,y+yy ,getRampType2(j,k,y+yy,bstype));
                            paintBlock(j ,k ,y+yy,color);
                        }
                    }else if(type>0&&blockinfo[type]&IS_BLOCKTNT){
                        burnBlock(j ,k ,y+yy ,true);
                    }
                    
                }
                
            }
        }
        
    }
    }
   
     boundleft=er;
    boundright=er;
    boundforward=er;
     boundbackward=er;
     boundtop=er;
    boundbot=er;
    
   	for(int i=0;i<=er;i++){
        for(int j=x-boundleft;j<=x+boundright;j++){
            for(int k=z-boundforward;k<=z+boundbackward;k++){
                if(!isColumnResident(j,k))continue;   // Q.1 F17
                int yy=er-i;
                
                //  int ox=j-x;
                //  int oz=k-z;
                //  int oy=yy;
                
                if(y-yy>0&&yy<=boundbot){
                    int type=getLandc(j, k, y-yy);
                    if(type>0&&blockinfo[type]&IS_BLOCKTNT){
                        burnBlock(j ,k ,y-yy ,true);
                    }
                }
                if(y+yy<T_HEIGHT&&yy<=boundtop){
                    int type=getLandc(j, k, y+yy);
                    
                    if(type>0&&blockinfo[type]&IS_BLOCKTNT){
                        burnBlock(j ,k ,y+yy ,true);
                    }
                    
                }
                
            }
        }
        
    }
}

// H.9: the explosion sound follows the blast's majority material. Counts the solid blocks in the same
// sphere the explode() loop visits; glass/crystal/ice >=25% -> mix_glass_crystal, else dirt/sand/grass
// or stone majority picks that sound, anything else (wood, brick, mixed) is tnt_explode_mix.
int Terrain::tntSoundForBlast(int x,int z,int y){
    int dirt=0,stone=0,glass=0,total=0;
    for(int j=x-EXPLOSION_RADIUS;j<=x+EXPLOSION_RADIUS;j++){
        for(int k=z-EXPLOSION_RADIUS;k<=z+EXPLOSION_RADIUS;k++){
            if(!isColumnResident(j,k))continue;
            for(int yy=-EXPLOSION_RADIUS;yy<=EXPLOSION_RADIUS;yy++){
                if((j-x)*(j-x)+(k-z)*(k-z)+yy*yy>EXPLOSION_RADIUS*EXPLOSION_RADIUS)continue;
                if(y+yy<0||y+yy>=T_HEIGHT)continue;
                int t=getLandc(j,k,y+yy);
                if(t<=TYPE_NONE||(blockinfo[t]&IS_LIQUID))continue;
                total++;
                if(t==TYPE_DIRT||t==TYPE_SAND||t==TYPE_GRASS||t==TYPE_GRASS2||t==TYPE_GRASS3)dirt++;
                else if(t==TYPE_STONE||t==TYPE_DARK_STONE||t==TYPE_COBBLESTONE||t==TYPE_BEDROCK||(t>=TYPE_STONE_RAMP1&&t<=TYPE_STONE_RAMP4)||(t>=TYPE_STONE_SIDE1&&t<=TYPE_STONE_SIDE4))stone++;
                else if(t==TYPE_GLASS||t==TYPE_CRYSTAL||(blockinfo[t]&IS_ICE))glass++;
            }
        }
    }
    if(total==0)return S_EXPLODE;
    if(glass*4>=total)return S_TNT_MIX_GLASS;
    if(dirt*2>total)return S_TNT_DIRT;
    if(stone*2>total)return S_TNT_STONE;
    return S_TNT_MIX;
}
void Terrain::explode(int x,int z,int y){
    
	
    int color=getColor(x,z,y);
    if(color!=0)
         Resources::getResources->playSound(S_GOOP_EXPLODE);
    else
        Resources::getResources->playSound(tntSoundForBlast(x,z,y));
    
    Vector v=MakeVector(x+.5f,y+.5f,z+.5f);
    
    
    MMM::ExplodeModels(v,color);
    World::getWorld->effects->addCreatureVanish(x+.5f,z+.5f,y+.5f,color,TYPE_TNT);
    
    bool painting=false;
   // BOOL building =false;
   
    if(color!=0)painting=true;
	//destroyBlock:x :z :y];
   	for(int i=1;i<=EXPLOSION_RADIUS;i++){
		for(int j=x-EXPLOSION_RADIUS;j<=x+EXPLOSION_RADIUS;j++){
			for(int k=z-EXPLOSION_RADIUS;k<=z+EXPLOSION_RADIUS;k++){
				int yy=EXPLOSION_RADIUS-i;

				int ox=j-x;
				int oz=k-z;
				int oy=yy;
				if(ox*ox+oz*oz+oy*oy>EXPLOSION_RADIUS*EXPLOSION_RADIUS)
					continue;
                if(!isColumnResident(j,k))continue;   // B.5b-1b: not past the window edge (it aliases)
                
                if(painting){
                    int type=getLandc(j, k, y-yy);
                    if(type!=-1)
                        if(type!=TYPE_TNT||getColor(j,k,y-yy)==0)
                            paintBlock(j ,k ,y-yy,color);
                        
                    type=getLandc(j, k, y+yy);
                    
                     if(y+yy<T_HEIGHT&&(type!=TYPE_TNT||getColor(j,k,y-yy)==0))
                         paintBlock(j ,k ,y+yy,color);
                    
                }else{
                    int type=getLandc(j, k, y-yy);
                    if(type!=-1){
                        if(blockinfo[type]&IS_FLAMMABLE){
                            if(isOnFire(j,k,y-yy)){continue;}
                            //if(type==TYPE_TNT)
                            //	explode:j:k:y-yy];
                            //else
                            burnBlock(j ,k ,y-yy ,true);
                        }else{
                            if(type!=TYPE_BEDROCK&&type!=TYPE_STEEL){
                                explodeBlock(j ,k ,y-yy);
                            }
                        }
                    }
                    if(y+yy<T_HEIGHT){
                    type=getLandc(j, k, y+yy);
                    
                    if(blockinfo[type]&IS_FLAMMABLE){
                        if(isOnFire(j,k,y+yy))continue;
                        
						burnBlock(j ,k ,y+yy ,true);
                    }else{
                        if(type!=TYPE_BEDROCK&&type!=TYPE_STEEL)
                            explodeBlock(j ,k ,y+yy);
                    }
                    }
                }
				
			}
		}
		
	}	
}
/*-(void)addColumnsIfNeeded{
	Vector ppos=World::getWorld->player.pos;
	ppos.x/=BLOCK_SIZE;
	ppos.z/=BLOCK_SIZE;
	ppos.x+=CHUNK_SIZE/2;
	ppos.z+=CHUNK_SIZE/2;
	ppos.x/=CHUNK_SIZE;
	ppos.z/=CHUNK_SIZE;
	int cx=ppos.x;
	int cz=ppos.z;
	int CVRADIUS=2;
	for(int i=cx-CVRADIUS;i<cx+CVRADIUS;i++){
		for(int j=cz-CVRADIUS;j<cz+CVRADIUS;j++){
			if(i<0||j<0)continue;
			TerrainChunk* chunk;
			hashmap_get(chunkMap, threeToOne(cx,0,cz),(any_t)&chunk);
			if(!chunk){
			[tgen generateColumn:i :j];				
			}
		}
	}
}*/
extern float P_ZFAR;
void Terrain::reloadIfNeeded(){
    return;  //disabled?
	float radius=T_SIZE/8;//(P_ZFAR/2)/BLOCK_SIZE;
	Player* player=World::getWorld->player;
	if(player->pos.x/BLOCK_SIZE-radius<0||player->pos.x/BLOCK_SIZE+radius>T_SIZE||
	   player->pos.z/BLOCK_SIZE-radius<0||player->pos.z/BLOCK_SIZE+radius>T_SIZE){
		do_reload=1;
		World::getWorld->hud->sb->setStatus("Loading ",999);
       
            
		
	}
}
Vector gcrot={0};
Vector portal_rot={0};
const float BURN_SPREAD_TIME=1.0f;
int chunk_load_count=0;
bool doingsomeloading=false;
static float blending_alpha;
static bool blending=false;
float last_etime;

bool Terrain::update(float etime){
    last_etime=etime;
    if(do_reload==-1){
        int pct=99*chunk_load_count/(2304/4)+counter/2;
        
        if(pct>100)pct=100;
        
            World::getWorld->hud->sb->setStatus(strf("Loading World  %d%%",pct),20);
        
        
        return false;
    }
    etime/=4;
    portal_rot.z-=5*etime;
    if(portal_rot.z>2*M_PI)portal_rot.z-=2*M_PI;
    if(portal_rot.z<0)portal_rot.z+=2*M_PI;
    
    gcrot.x+=2*etime;
    if(gcrot.x>2*M_PI)gcrot.x-=2*M_PI;
    gcrot.y+=1*etime;
    if(gcrot.y>2*M_PI)gcrot.y-=2*M_PI;
    gcrot.z+=.5f*etime;
    if(gcrot.z>2*M_PI)gcrot.z-=2*M_PI;
   etime*=4;
	BurnNode* prev=NULL;
	BurnNode* node=burnList;
	while(node!=NULL){
		if(node->time > node->life-BURN_SPREAD_TIME &&node->time-etime<=node->life-BURN_SPREAD_TIME&&!(blockinfo[node->type]&IS_BLOCKTNT)){
			burnBlock(node->x+1 ,node->z ,node->y ,false);
			burnBlock(node->x-1 ,node->z ,node->y ,false);
			burnBlock(node->x ,node->z+1 ,node->y ,false);
			burnBlock(node->x ,node->z-1 ,node->y ,false);
			burnBlock(node->x ,node->z ,node->y+1 ,false);
			burnBlock(node->x ,node->z ,node->y-1 ,false);

		}
		
		if(nburn>300){			
			endDynamics(false);
            
			break;
		}
		node->time-=etime;
		int tz=getLandc(node->x ,node->z ,node->y);
		if(node->time<=0||tz==TYPE_NONE){
			// MP.4 (E10): our own burn's mine goes out when its fuse runs out on time, never when the
			// cell went air first (a server restore's or a peer's mine).
			if(node->mpLit&&node->time<=0&&tz!=TYPE_NONE)MpWorld::burnFuseOut(node->x,node->y,node->z);
			if(prev==NULL)
				burnList=node->next;
			else
				prev->next=node->next;
			if(node->type==TYPE_TNT){
					
				explode(node->x ,node->z ,node->y);
				
			}else if(node->type==TYPE_FIREWORK){
            
                shootFirework(node->x ,node->z ,node->y);
            }else if(blockinfo[node->type]&IS_BLOCKTNT){
                blocktntexplode(node->x ,node->z ,node->y ,node->type);
            }
			nburn--;
			Resources::getResources->endBurnId(node->sid);
			World::getWorld->effects->removeFire(node->pid);
			if(tz!=TYPE_NONE){
				int after=TYPE_NONE;
				if(FIRE_MELTS_ICE&&blockinfo[node->type]&IS_ICE)after=TYPE_WATER;
				else if(FIRE_CHARS_VINES&&node->type==TYPE_VINE)after=TYPE_DARK_STONE;
				updateChunks(node->x ,node->z ,node->y ,after);
				if(after==TYPE_WATER)liquids->addSource(node->x ,node->z ,node->y);
			}
			free(node);			
			node=NULL;
			if(prev!=NULL)
			node=prev->next;
		}else {
			prev=node;
			node=node->next;
		}		
	}
   
    liquids->update(etime);
    fireworks->update(etime);
    // B.5b-1b autosave (decision 1): with edits pending and g_autosave_seconds since the last commit
    // of any kind, save -- without endDynamics. Stock's only periodic save was the walk reload's,
    // which the ring retired; this puts a bound back on how much a crash can lose. Not mid-bulk-
    // reload (that path saved at its start) and not mid-load. The clock restarts either way, so a
    // world with nothing to save pays one scan of the window a period.
    g_seconds_since_save+=etime;
    if(g_autosave_seconds>0&&g_seconds_since_save>=g_autosave_seconds&&loaded&&!bulk_reload_active&&do_reload==0){
        g_seconds_since_save=0;
        bool dirty=false;
        for(int i=0;i<CHUNKS_PER_SIDE*CHUNKS_PER_SIDE*CHUNKS_PER_COLUMN&&!dirty;i++)
            if(chunkTable[i]&&chunkTable[i]->modified)dirty=true;
        if(dirty){
            World::getWorld->fm->saveWorldKeepDynamics();
            g_autosaves++;
        }
    }
    extern Vector colorTable[256];
    if(interpolatev(&skycolor,final_skycolor,.25f,etime)){
         
        Vector v=skycolor;
        if(v_equals(final_skycolor,colorTable[14]))
        v=MakeVector(0.5,0.72,0.9);
        float clr[4]={v.x-.03f, v.y-.03f, v.z-.03f, 1.0f};
        
        Graphics::setFogColor(clr);
       // printg("TRUE\n");
    }
    
    if(v_equals(final_skycolor,colorTable[14])){
       
            blending_alpha-=.04f*etime*60;
         
    }else{
       
        //    extern Vector colorTable[256];
        
        if(blending){
                       blending_alpha+=.04f*etime*60;
                   }
    }
   if(do_reload==3){
       World::getWorld->hud->sb->clear();
        do_reload=0;
    }
	else if(do_reload==2){
        do_reload=0;
		World::getWorld->fm->saveWorld();
		unloadTerrain(false);
		//oldChunkMap=chunkMap;	
		//chunkMapc=chunkMap=hashmap_new();
       
        loadTerrain(world_name,false);
        //hashmap_iterate(oldChunkMap,freeOldChunks,NULL);
		//iterate oldchunkmap and release chunks that arent reused
		//hashmap_remove_all(oldChunkMap, FALSE);
		
		do_reload=3;
        printf("test1123\n");
				
		
		return false;
	}else if(do_reload==1){
        do_reload++;
    }else
	reloadIfNeeded();
	
	return false;
}

void Terrain::endDynamics(bool endLiquids){
	nburn=0;
    if(endLiquids)
    liquids->clearLiquids();
	while(burnList!=NULL){
		BurnNode* node=burnList->next;
		burnList->next=NULL;
		updateChunks(burnList->x ,burnList->z ,burnList->y ,TYPE_NONE);
		free(burnList);
		
		burnList=node;
		
	}
    World::getWorld->effects->clearAllEffects();
	Resources::getResources->endBurn();
	
}
int Terrain::dropDynamicsInColumn(int cx,int cz){
    int n=0;
    BurnNode* prev=NULL;
    BurnNode* node=burnList;
    while(node!=NULL){
        BurnNode* next=node->next;
        if(node->x/CHUNK_SIZE==cx&&node->z/CHUNK_SIZE==cz){
            if(prev==NULL)burnList=next; else prev->next=next;
            nburn--;
            Resources::getResources->endBurnId(node->sid);
            World::getWorld->effects->removeFire(node->pid);
            updateChunks(node->x ,node->z ,node->y ,TYPE_NONE);   // as endDynamics: the block burns out
            free(node);
            n++;
        }else prev=node;
        node=next;
    }
    return n+liquids->dropColumn(cx,cz);
}
int Terrain::dynamicsOutsideWindow(){
    int n=0;
    for(BurnNode* node=burnList;node!=NULL;node=node->next)
        if(!isColumnResident(node->x,node->z))n++;
    return n+liquids->countOutsideWindow();
}
void Terrain::startDynamics(){/*
    for(int i=0;i<T_SIZE*T_SIZE*T_HEIGHT;i++){
        if(blockarray[i]==TYPE_WATER||blockarray[i]==TYPE_LAVA){
            int n=i;
            int y=n%T_HEIGHT;
            n/=T_HEIGHT;
            int z=n%T_SIZE;
            n/=T_SIZE;
            int x=n;
            [liquids addSource:x:z:y:blockarray[i]:getColor:x:z:y]];
        }
    }*/
               
}
static double time1,time2,time3,time4;
static int hit_load_counter=0;

// How much of a bulk window reload one frame is allowed to do, counted in CHUNKS rather than in
// columns so the budget stays honest at 256z: a column is CHUNKS_PER_COLUMN chunks of BOTH the
// read (32 KB at 64z, 131 KB at 256z) and the mesh work, so a column budget would cost 4x as
// much per frame in a tall world. 96 chunks = 24 columns at 64z, 6 at 256z.
#define BULK_RELOAD_CHUNK_BUDGET 96
// H.4: the bulk reload's per-frame time for Classic-generated columns (generator-plan.md §2.1).
// 4 ms is ~11 legacy columns a frame on the iPad Air 2 (0.36 ms each); tune on the device.
#define GEN_FRAME_BUDGET_MS 4.0

// One stale column of the resident window, plus its squared chunk-space distance to the player,
// so a budgeted slice can take the nearest ones first (see the reload).
struct StaleColumn{
    int cx,cz;
    int d2;
};
static int compare_stale_column(const void* a,const void* b){
    int da=((const StaleColumn*)a)->d2;
    int db=((const StaleColumn*)b)->d2;
    if(da<db)return -1;
    if(da>db)return 1;
    return 0;
}

// Is toroidal chunk-table column (x,z) holding the column the window wants this frame, and are its
// four laterals? B.5b-1a made this permanent (it used to be asked only mid-bulk-reload) and stopped
// the laterals folding round the window: an outer-ring column's outward neighbour is outside the
// window, so the ring is never meshed -- it is data for its inner neighbour. Folding used to mesh an
// edge column against the OPPOSITE edge's data. Under ring streaming the outer ring is where columns
// land, so each column is meshed once, when it becomes inner, not on landing and again a row later.
// (CHUNKS_PER_SIDE == T_RADIUS*2, so a toroidal slot maps 1:1 onto a window slot. Unlike
// threeToOne's fixed +50*CHUNKS_PER_SIDE bias, this one really does have to fold the modulus
// twice: x is a small toroidal index and ox is an absolute chunk coordinate in the thousands.)
//
// This asks about the column AND the four lateral neighbours rebuild2() reads across, so a chunk
// is meshed exactly once instead of once per adjacent column that lands later: measured 1296
// rebuilds per burst instead of 1949, i.e. 144 ms of mesh CPU down to 107 ms (same-session A/B,
// tools/headless-mesh-burst-probe.js against build-relwdiag, 4 runs each).
//
// Pass 70 wrote exactly this, measured it, and REVERTED it, because it crashed the release build
// with an intermittent out-of-bounds inside the frame tick. That crash is now root-caused and
// fixed, and it was never about the neighbourhood test: deferring a RESIDENT column is the only
// way this engine ever draws a chunk that has not been meshed since it was allocated, and
// TerrainChunk's constructor left the whole mesh-count block uninitialised, so render()'s
// `n_vertices==0` guard read garbage and the index memcpy walked off allIndices. See the
// constructor in TerrainChunk.cpp. The narrow test only hid it by guaranteeing every drawn chunk
// had been meshed at least once.
//
// Keep that in mind before assuming a similar deferral is safe: what makes THIS one safe is that
// a deferred chunk's geometry still belongs to its own pbounds, and the chunk is now empty rather
// than garbage until it is meshed.
static bool columnMeshable(int x,int z,int ox,int oz,
                           const bool isloaded[CHUNKS_PER_SIDE_MAX][CHUNKS_PER_SIDE_MAX]){
    int wx=((x-ox)%CHUNKS_PER_SIDE+CHUNKS_PER_SIDE)%CHUNKS_PER_SIDE;
    int wz=((z-oz)%CHUNKS_PER_SIDE+CHUNKS_PER_SIDE)%CHUNKS_PER_SIDE;
    if(!isloaded[wx][wz])return false;
    // The four laterals -- rebuild2() reads one block across each side face, so a column whose
    // neighbour is still stale would have to be meshed again when it lands. A lateral outside the
    // window is never resident (no fold, see above).
    for(int d=0;d<4;d++){
        static const int dx[4]={-1,1,0,0};
        static const int dz[4]={0,0,-1,1};
        int nx=wx+dx[d];
        int nz=wz+dz[d];
        if(nx<0||nx>=CHUNKS_PER_SIDE||nz<0||nz>=CHUNKS_PER_SIDE)return false;
        if(!isloaded[nx][nz])return false;
    }
    // H.2: with shadows on, a column's top faces read the shadow tops of all 8 neighbours
    // (Shadow.h), so it also waits for the 4 diagonals. Off, this is the stock 4-wait.
    if(g_shadow_mode!=SHADOW_OFF&&g_shadow_test_mutation!=4){
        for(int d=0;d<4;d++){
            const int nx=wx+((d&1)?1:-1);
            const int nz=wz+((d&2)?1:-1);
            if(nx<0||nx>=CHUNKS_PER_SIDE||nz<0||nz>=CHUNKS_PER_SIDE)return false;
            if(!isloaded[nx][nz])return false;
        }
    }
    return true;
}

// B.5b-1a: the window origin prepareAndLoadGeometry chose this frame, for slotOnOuterRing (asked by
// f3_frameEnd, which only sees chunk slots).
static int g_win_ox=0,g_win_oz=0;
// Is toroidal column slot (x,z) on the window's outer ring -- the data-only band columnMeshable
// never meshes, and whose dirty flags therefore stay set for as long as it is on the ring?
static bool slotOnOuterRing(int x,int z){
    int wx=((x-g_win_ox)%CHUNKS_PER_SIDE+CHUNKS_PER_SIDE)%CHUNKS_PER_SIDE;
    int wz=((z-g_win_oz)%CHUNKS_PER_SIDE+CHUNKS_PER_SIDE)%CHUNKS_PER_SIDE;
    return wx==0||wx==CHUNKS_PER_SIDE-1||wz==0||wz==CHUNKS_PER_SIDE-1;
}

// B.5b-1a ring streaming: columns landed per frame while the window is short of 1..140 columns,
// which is all the time while walking. A straight walk needs < 0.4 a frame (18 columns per chunk
// crossing, one crossing per ~2.7 s), a diagonal ~0.8. One is the plan's starting point
// (WORKING/render-distance-runtime-plan-2026-10-11.md §3.2); the device round (T.B15) tunes it from
// the worst frame. Counted when a read STARTS, like the bulk budget.
#define RING_COLUMN_BUDGET 1

void Terrain::prepareAndLoadGeometry(){
    time1=time2=((eden_platform_now_ms()-start)/1000.0);
    World* world=World::getWorld;
    Player* player=world->player;

    
    
    
    int m_chunkOffsetX=0;
    int m_chunkOffsetZ=0;
    // Hoisted out of the load block below because the meshing pass reads it too: mid-bulk-reload
    // it is the record of which columns of the window are resident RIGHT NOW, which is what tells
    // the mesher which chunks are worth meshing yet (see BULK_RELOAD_CHUNK_BUDGET).
    // B.5b-2: sized to the largest window; only [0,2*T_RADIUS) of each axis is used.
    bool isloaded[CHUNKS_PER_SIDE_MAX][CHUNKS_PER_SIDE_MAX];

    ///////////load geom from file or gen
    if(loaded){
        m_chunkOffsetX=windowOriginChunk(player->pos.x);
        m_chunkOffsetZ=windowOriginChunk(player->pos.z);
        g_win_ox=m_chunkOffsetX;
        g_win_oz=m_chunkOffsetZ;

                int r=T_RADIUS;
        int count=0;
        //NSLog(@"player p
        for(int x=0;x<2*r;x++){
            for(int z=0;z<2*r;z++){
                //	NSLog(@"lch:%d",asdf++);
                TerrainChunk* chunk;
                chunk=chunkTable[threeToOne(x+m_chunkOffsetX,0,z+m_chunkOffsetZ)];
                // hashmap_get(world.terrain.chunkMap, threeToOne(x+chunkOffsetX, 0, z+chunkOffsetZ), (any_t)&chunk);
                if(chunk){
                   // printf("found chunk with wrong bounds");
                    
                    if( chunk->pbounds[0]!=(x+m_chunkOffsetX)*CHUNK_SIZE||
                       chunk->pbounds[2]!=(z+m_chunkOffsetZ)*CHUNK_SIZE)
                        
                        
                    {
                        
                        //   printg("(%d,%d)=?=(%d,%d)\n",chunk.pbounds[0],chunk.pbounds[2],(x+chunkOffsetX)*CHUNK_SIZE,(z+chunkOffsetZ)*CHUNK_SIZE);
                        
                        count++;
                        isloaded[x][z]=false;
                        //  printg("overwriting a chunk\n");
                        
                    }
                    else
                        isloaded[x][z]=true;
                    
                }
                else{
                    count++;
                    isloaded[x][z]=false;
                }

            }
        }

        // B.5b-1a: once per window shift (a chunk crossing), not once per bulk reload as stock did.
        // fm->chunkOffset is the RENDER origin (camera, chunk translates, creature rebase) and
        // "the window" for creature activation; any value works for drawing because meshes are
        // chunk-local, but it has to change between frames, and this is before render. The sky
        // region check rides on it too; the creature top-up waits for the ring to catch up.
        {
            FileManager* fm=world->fm;
            if(fm->chunkOffsetX!=m_chunkOffsetX||fm->chunkOffsetZ!=m_chunkOffsetZ){
                fm->chunkOffsetX=m_chunkOffsetX;
                fm->chunkOffsetZ=m_chunkOffsetZ;
                extern bool loaded_new_terrain;
                loaded_new_terrain=true;
                ring_creatures_owed=true;
            }
        }

        // The bulk window reload. This used to save, read EVERY stale column and then mesh all of
        // them inside this one call -- one engine frame -- which tools/headless-mesh-burst-probe.js
        // measured at a 104-124 ms main-thread block on a teleport (324 columns / 1296 chunks:
        // ~60% mesh CPU, ~20-30% column read/decode, the rest the save that precedes it). It is
        // now frame-budgeted: the save still happens once, up front and before anything is
        // overwritten (that ordering is the one correctness rule here), and the read+mesh loop
        // spends BULK_RELOAD_CHUNK_BUDGET chunks' worth of columns per frame until the window is
        // whole again. No new concurrency -- this is cooperative slicing on the same thread.
        //
        // Why a partly-filled window is safe: the engine ALREADY tolerates up to 140 stale slots
        // at all times (that is what this threshold means -- a slot whose pbounds say some other
        // world position, drawn where its pbounds put it, i.e. outside the view distance). A
        // slice only extends that tolerated state by a handful of frames. The one thing that
        // must not wait is the ground under the player, hence the nearest-first ordering below.
        // B.5b-2: stock's 140 and 300 of 324 slots, kept as fractions of the window (exactly 140
        // and 300 at the default radius), so a jump is a jump at every radius.
        const int ncols=4*r*r;
        if(count*324>140*ncols||bulk_reload_active) {
            if(!bulk_reload_active){
                hit_load_counter++;
                if(hit_load_counter==1){
                    World::getWorld->hud->sb->setStatus("Loading",999);
                    if(count*324>300*ncols){
                        hit_load_counter++;
                    }

                }
                if(hit_load_counter>=2){
                    hit_load_counter=0;
                    World::getWorld->hud->sb->clear();
                    f3_begin("E2");
                    World::getWorld->fm->saveWorld();

                    World::getWorld->fm->chunkOffsetX=m_chunkOffsetX;
                    World::getWorld->fm->chunkOffsetZ=m_chunkOffsetZ;

                    // Zero the light array NOW, before the first column lands, so every chunk this
                    // reload meshes bakes the same zeroed light the single-frame version gave it.
                    // Only the "recompute the lights" half is deferred to the final slice: running
                    // calculateLighting() against a half-read window would miss every lightbox in
                    // a column that hasn't streamed in yet.
                    void updateLightingBegin();
                    updateLightingBegin();
                    update_lighting=false;
                    // The bulk path tops up creatures at its end, which covers anything the ring
                    // still owed.
                    ring_creatures_owed=false;
                    g_bulk_reloads++;

                    bulk_reload_active=true;
                }
            }
            if(bulk_reload_active){
              //  printf("chunks to load:%d\n",count);
                // Nearest-first. Budgeting means some columns wait a few frames, and the ones the
                // player is standing in must not be the ones that wait -- collision reads
                // blockarray directly, so a far-away column can be late but the player's own
                // column cannot.
                static StaleColumn stale[CHUNKS_PER_SIDE_MAX*CHUNKS_PER_SIDE_MAX];
                int nstale=0;
                int pcx=player->pos.x/CHUNK_SIZE;
                int pcz=player->pos.z/CHUNK_SIZE;
                for(int x=0;x<2*r;x++){
                    for(int z=0;z<2*r;z++){
                        if(!isloaded[x][z]){
                            int cx=x+m_chunkOffsetX;
                            int cz=z+m_chunkOffsetZ;
                            stale[nstale].cx=cx;
                            stale[nstale].cz=cz;
                            stale[nstale].d2=(cx-pcx)*(cx-pcx)+(cz-pcz)*(cz-pcz);
                            nstale++;
                        }
                    }
                }

                if(nstale==0){
                    // Reads are done. The reload is only over once the geometry they dirtied has
                    // been meshed too, or the deferred backlog would all come due in one frame --
                    // which is the burst again, just moved to the end (measured: it was).
                    if(!bulk_reload_meshing){
                        bulk_reload_active=false;
                        addMoreCreaturesIfNeeded();
                        if(!LOW_MEM_DEVICE)update_lighting=true;
                        extern bool loaded_new_terrain;
                        loaded_new_terrain=true;
                    }
                }else{
                    std::string file_name=world->fm->documents+"/"+world_name;

                    //[sf_lock lock];
                    // Reopened per slice on purpose: holding a handle across frames would outlive
                    // the rename an autosave does under it (FileManager::saveWorld writes a
                    // .savetmp and renames), leaving the slice reading a file nobody points at.
                    // MP.3a: a session has no file (its name, "mp:host:port", is not a path).
                    EdenFile* saveFile=world->fm->sessionWorld?NULL:EdenFile::openRead(file_name);   // G.2b-2: was NSFileHandle

                    qsort(stale,nstale,sizeof(StaleColumn),compare_stale_column);

                    int budget=BULK_RELOAD_CHUNK_BUDGET/CHUNKS_PER_COLUMN;
                    if(budget<1)budget=1;
                    // Stage R / R.2b: the read half of the budget, counted in OCCUPIED chunks.
                    // A column that lands costs its non-empty bands (typesEmpty, R.2, refreshed by
                    // addChunk as it lands; at least 1), so a 256z column of 64z-shaped terrain
                    // costs what the 64z column costs and the reload reads ~24 columns a frame
                    // instead of 6. It cannot be known before the read, so the column that crosses
                    // the budget still lands (<= CHUNKS_PER_COLUMN-1 over, as the mesh pass's last
                    // chunk can be). Capped at the 64z column count, so no frame reads more columns
                    // than a 64z frame does and 64z itself is unchanged. What grows is BYTES per
                    // frame at 256z (.eden stores air bands as 8 KB of zeros): up to 4x, which is
                    // the device cost R.4 must time. g_read_budget_bands=false is the old count.
                    extern bool g_read_budget_bands;
                    const bool byBands=g_read_budget_bands;
                    if(byBands){
                        budget=BULK_RELOAD_CHUNK_BUDGET/(T_HEIGHT_DEFAULT/CHUNK_SIZE);
                        if(budget<1)budget=1;
                    }
                    int spent=0;
                    // H.4 (generator-plan.md §2.1): a column the Classic generator makes is charged
                    // by measured time, not chunks -- GEN_FRAME_BUDGET_MS of generation a frame,
                    // always at least one column -- so a jump into unexplored land takes a few more
                    // frames instead of one long one. Saved and bundled columns keep the chunk budget.
                    double genMs=0; int genCols=0;
                    if(budget>nstale)budget=nstale;
                    for(int i=0,taken=0;i<nstale&&taken<budget&&
                                        (!byBands||spent<BULK_RELOAD_CHUNK_BUDGET);i++){
                        if(world->fm->columnWillGenerate(stale[i].cx,stale[i].cz)){
                            if(genCols>0&&genMs>=GEN_FRAME_BUDGET_MS)continue;
                            double g0=eden_platform_now_ms();
                            world->fm->readColumn(stale[i].cx,stale[i].cz,saveFile);
                            genMs+=eden_platform_now_ms()-g0;
                            genCols++;
                            isloaded[stale[i].cx-m_chunkOffsetX][stale[i].cz-m_chunkOffsetZ]=true;
                            continue;
                        }
                        //removeLights
                        world->fm->readColumn(stale[i].cx,stale[i].cz,saveFile);
                        isloaded[stale[i].cx-m_chunkOffsetX][stale[i].cz-m_chunkOffsetZ]=true;
                        if(byBands){
                            int occupied=0;
                            for(int cy=0;cy<CHUNKS_PER_COLUMN;cy++){
                                TerrainChunk* c=chunkTable[threeToOne(stale[i].cx,cy,stale[i].cz)];
                                if(!c||!c->typesEmpty)occupied++;
                            }
                            spent+=occupied>0?occupied:1;
                        }
                        //addlights
                        taken++;
                    }

                    delete saveFile;   // closes; NULL-safe like [nil closeFile]
                    //void calculateLighting();
                }
            }
            time2=((eden_platform_now_ms()-start)/1000.0);
            
        }else if(count>0){
            // B.5b-1a ring streaming. Stock left up to 140 slots stale -- a walk showed bare sky
            // 1-2 chunks ahead from the 3rd crossing on (plan §1.1) -- and then reloaded ~144 of them
            // in one burst. Now every frame lands the nearest stale columns under RING_COLUMN_BUDGET,
            // so the window keeps up with a walk; the bulk path above is left for jumps (teleport,
            // console tp, ice faster than the budget), and warps stay synchronous.
            static StaleColumn ring[CHUNKS_PER_SIDE_MAX*CHUNKS_PER_SIDE_MAX];
            int nring=0;
            int pcx=player->pos.x/CHUNK_SIZE;
            int pcz=player->pos.z/CHUNK_SIZE;
            for(int x=0;x<2*r;x++){
                for(int z=0;z<2*r;z++){
                    if(!isloaded[x][z]){
                        int cx=x+m_chunkOffsetX;
                        int cz=z+m_chunkOffsetZ;
                        ring[nring].cx=cx;
                        ring[nring].cz=cz;
                        ring[nring].d2=(cx-pcx)*(cx-pcx)+(cz-pcz)*(cz-pcz);
                        nring++;
                    }
                }
            }
            qsort(ring,nring,sizeof(StaleColumn),compare_stale_column);
            std::string file_name=world->fm->documents+"/"+world_name;
            // Reopened per frame, for the reason the bulk slice gives. None in a session (MP.3a).
            EdenFile* saveFile=world->fm->sessionWorld?NULL:EdenFile::openRead(file_name);   // G.2b-2: was NSFileHandle
            for(int i=0,taken=0;i<nring&&taken<RING_COLUMN_BUDGET;i++){
                const int cx=ring[i].cx,cz=ring[i].cz;
                // B.5b-1b, per column, in this order (plan §3.3 step 2):
                // 1. Drop the departing occupant's dynamics. Its burn and liquid nodes are keyed by
                //    absolute coordinates and read through the toroidal arrays, so left alone they
                //    would simulate against the column landing in this slot -- write water or air
                //    into it, dirty it, and get it saved. Before the save, because a burn node ends
                //    as air (endDynamics' rule), and that air belongs in the evict-save.
                // 2. Save-before-stream: if the occupant holds edits, save them before readColumn
                //    overwrites them -- without endDynamics, so fire and water elsewhere in the
                //    window carry on (stock stopped all of them every ~128 blocks walked). One save
                //    covers every modified column in the window, so it runs at most once per edit.
                TerrainChunk* occ=chunkTable[threeToOne(cx,0,cz)];
                if(occ&&!testNoDropDynamics)
                    g_ring_dynamics_dropped+=dropDynamicsInColumn(occ->pbounds[0]/CHUNK_SIZE,occ->pbounds[2]/CHUNK_SIZE);
                for(int cy=0;cy<CHUNKS_PER_COLUMN&&!testNoEvictSave;cy++){
                    TerrainChunk* c=chunkTable[threeToOne(cx,cy,cz)];
                    if(c&&c->modified){
                        world->fm->saveWorldKeepDynamics();
                        g_ring_evict_saves++;
                        break;
                    }
                }
                world->fm->readColumn(cx,cz,saveFile);
                isloaded[cx-m_chunkOffsetX][cz-m_chunkOffsetZ]=true;
                g_ring_columns_landed++;
                taken++;
            }
            delete saveFile;
        }else{
            // The window is whole. Settle what the ring owes now that every column is resident.
            if(ring_creatures_owed){
                ring_creatures_owed=false;
                addMoreCreaturesIfNeeded();
            }
        }

        // B.5b-1c: light per landed column (plan §3.3 step 4). Every slot that changed occupant
        // since its light was computed -- a ring landing above -- has its 3x3 of slots re-lit, and the chunks whose light changed
        // are dirtied, so this has to run before the meshing pass below. Not while a full sweep is
        // owed or running (a bulk reload, a load, a warp): that sweep lights the whole window, and
        // whatever lands during it is caught here the frame after it finishes (Lighting.mm).
        if(!LOW_MEM_DEVICE&&!update_lighting&&!bulk_reload_active)
            light_syncLandedColumns();

        // B.5b-1a: the outer ring is data-only (columnMeshable), so nothing on it may be drawn: a
        // trailing column that was inner last crossing still holds its mesh. Drop what is there and leave the chunk dirty, so that when a
        // shift back makes it inner again it is meshed rather than left as a hole. (Belt and braces:
        // a column only turns inner again when the column beyond it re-lands, and that addChunk
        // dirties it too -- the walk gate passes with this redirty removed.) Idempotent and cheap
        // (68 columns of pointer tests) once the ring is clean.
        for(int x=0;x<2*r;x++){
            for(int z=0;z<2*r;z+=(x==0||x==2*r-1)?1:2*r-1){
                for(int cy=0;cy<CHUNKS_PER_COLUMN;cy++){
                    const int n=threeToOne(x+m_chunkOffsetX,cy,z+m_chunkOffsetZ);
                    TerrainChunk* c=chunkTable[n];
                    if(!c||!c->holdsGeometry())continue;
                    c->dropGeometry();
                    redirtyChunk(n);
                }
            }
        }
        //[sf_lock unlock];
        
        
        /*for(int x=0;x<2*r;x++){
         for(int z=0;z<2*r;z++){
         if(!isloaded[x][z]){
         int dirx=-T_RADIUS*2;
         int dirz=-T_RADIUS*2;
         if(x<r)dirx=-dirx;
         if(z<r)dirz=-dirz;
         TerrainChunk* chunk;
         hashmap_get(world.terrain.chunkMap, threeToOne(x+chunkOffsetX+dirx, 0, z+chunkOffsetZ), (any_t)&chunk);
         if(chunk){
         
         for(int i=0;i<CHUNKS_PER_COLUMN;i++)
         [terrain addToDeleteList:x+chunkOffsetX+dirx:i:z+chunkOffsetZ];
         
         }
         
         hashmap_get(world.terrain.chunkMap, threeToOne(x+chunkOffsetX, 0, z+chunkOffsetZ+dirz), (any_t)&chunk);
         if(chunk){
         
         for(int i=0;i<CHUNKS_PER_COLUMN;i++)
         [terrain addToDeleteList:x+chunkOffsetX+dirx:i:z+chunkOffsetZ+dirz];
         
         }
         
         //  [rebuild_lock lock];
         //  [NSThread sleepForTimeInterval:0.10f];
         
         //  [rebuild_lock unlock];
         }
         }
         }*/
        
        
        
    }
   
    
    
    
    // H.2: rescan the shadow tops of every column that landed since the last mesh pass (a ring or
    // bulk read above, a decode published at the top of this frame, a load), unconditionally and
    // with shadows on or off, so no mesh below can read a previous occupant's tops (Shadow.h).
    shadow_syncLandedColumns();

    //////////////////build geom
    if(loaded){
        int num=0;
        // B1: this used to be int list[2000], filled with no bounds check, against a worst case of
        // CHUNKS_PER_SIDE^2*CHUNKS_PER_COLUMN -- 1296 at 64z (already 65% full, one bulk-dirty
        // event from overrunning) and 5184 at 256z. Sized to that worst case now, and the fill is
        // guarded: dropping a dirty chunk costs one stale frame, overrunning the stack costs the tab.
        static int list[CHUNKS_PER_SIDE_MAX*CHUNKS_PER_SIDE_MAX*CHUNKS_PER_COLUMN_MAX];
        const int list_max=CHUNKS_PER_SIDE*CHUNKS_PER_SIDE*CHUNKS_PER_COLUMN;
        // While a bulk reload is in flight the same per-frame budget that limits its column reads
        // limits its meshing, using the deferral list_max already had. Outside a reload this is
        // list_max, i.e. the pass drains everything exactly as it always did -- an edit, an
        // explosion and the initial world load are all unbudgeted.
        const int frame_max=bulk_reload_active&&BULK_RELOAD_CHUNK_BUDGET<list_max
                            ?BULK_RELOAD_CHUNK_BUDGET:list_max;
        // Stage R / R.2: what frame_max counts. An all-air chunk (typesEmpty) costs rebuild2() a
        // few instructions now, not a scan, so it no longer spends the budget: at 256z ~80% of a
        // reload's dirty chunks are air, and counting them made the mesh span 4x the 64z one for
        // the same terrain. `num` still counts everything, because list[] is sized by list_max.
        extern bool g_empty_shortcut;
        int budgeted=0;

        idxrl=0;
        bulk_reload_meshing=false;

            for(int x=0;x<CHUNKS_PER_SIDE;x++){
                for(int z=0;z<CHUNKS_PER_SIDE;z++){
                    if(columnsToUpdate[getColIndex(x,z)]){
                        // Mid-bulk-reload the window is deliberately part-resident. A column that
                        // has not streamed in yet gets dirtied anyway, by every neighbouring
                        // column that HAS (addChunk marks the four laterals), and meshing it now
                        // just builds geometry for data that is about to be replaced: measured,
                        // the sliced reload meshed 4188 chunks instead of 2016 without this.
                        // Deferring is free -- the dirty flags stay set, and readColumn re-sets
                        // them when the column really lands.
                        // B.5b-1a: asked every frame now, not only mid-bulk-reload -- the ring
                        // keeps the window part-resident all the time, and the outer ring is
                        // never meshable (columnMeshable).
                        if(!columnMeshable(x,z,m_chunkOffsetX,m_chunkOffsetZ,isloaded))
                            continue;
                        // Set if any chunk of this column had to be left dirty (the frame_max
                        // deferral below), so the column flag survives to the next frame.
                        bool column_deferred=false;
                        for(int y=0;y<CHUNKS_PER_COLUMN;y++){
                            if(chunksToUpdate[threeToOne(x,y,z)]){

                                int n=threeToOne(x,y,z);

                                if(n>=CHUNKS_PER_SIDE*CHUNKS_PER_SIDE*CHUNKS_PER_COLUMN||n<0){
                                    printg("out of bounds index: %d\n",n);
                                }
                                const bool budgetFree=g_empty_shortcut&&chunkTable[n]&&
                                                      chunkTable[n]->typesEmpty;
                                if(num>=list_max){
                                    // Leave chunksToUpdate/columnsToUpdate set for what didn't fit,
                                    // so the next frame picks it up instead of losing it forever.
                                    printg("dirty-chunk list full at %d, deferring the rest\n",num);
                                    goto list_full;
                                }
                                if(!budgetFree&&budgeted>=frame_max){
                                    // Over budget: leave this one dirty for the next frame, same
                                    // as the old goto did -- but keep walking (R.2), so the air
                                    // chunks further on still clear this frame instead of each
                                    // waiting behind a non-empty one.
                                    bulk_reload_meshing=true;
                                    column_deferred=true;
                                    continue;
                                }
                                list[num++]=n;
                                if(!budgetFree)budgeted++;

                                chunksToUpdate[threeToOne(x,y,z)]=false;
                            }

                        }
                        if(!column_deferred)
                            columnsToUpdate[getColIndex(x,z)]=false;
                        // if(num>=1000){printg("1234overflow\n");break;}
                        
                    }
                }
            }
            list_full:



            // goto cleanup;
            
            
            
            for(int i=0;i<num;i++){
                TerrainChunk* chunk=NULL;
                if(list[i]<0||list[i]>=CHUNKS_PER_SIDE*CHUNKS_PER_SIDE*CHUNKS_PER_COLUMN){
                    printg("out of bounds access list[%d]=%d  num: %d idxrl: %d  max:%d\n",i,list[i],num,idxrl, CHUNKS_PER_SIDE*CHUNKS_PER_SIDE*CHUNKS_PER_COLUMN);
                    //  continue;
                }
                //issue #3 continued
                chunk=chunkTable[list[i]];
                //=malloc(sizeof(TerrainChunk*)*CHUNKS_PER_SIDE*CHUNKS_PER_SIDE*CHUNKS_PER_COLUMN);
                if(chunk){
                    rebuildList[idxrl++]=chunk;
                    chunk->idxn=list[i];
                }else{
                    printg("null chunk marked for updating??\n");
                }
            }
        
      
    

        for(int i=0;i<idxrl;i++){
            
                   double _f3m0=f3_now();
                   int _f3r=rebuildList[i]->rebuild2();
                   { double _d=f3_now()-_f3m0;
                     if(rebuildList[i]->n_vertices+rebuildList[i]->n_vertices2==0){g_f3.mesh_e_n++;g_f3.mesh_e_ms+=_d;}
                     else{g_f3.mesh_ne_n++;g_f3.mesh_ne_ms+=_d;}
                     if(g_f3.mesh_first<0)g_f3.mesh_first=g_f3.frames;
                     g_f3.mesh_last=g_f3.frames; }
                   if(_f3r==-1){
                    //    chunksToUpdate[rebuildList[i].idxn]=TRUE;
                     //   columnsToUpdate[rebuildList[i].idxn/CHUNKS_PER_COLUMN]=TRUE;
                       printg("fail update on chunk: %d    bounds %d %d %d   rebuildCounter: %d\n",i,rebuildList[i]->pbounds[0],rebuildList[i]->pbounds[1],rebuildList[i]->pbounds[2],rebuildList[i]->rebuildCounter);
                    }else{
                        
                      //  rebuildList[i]->needsRebuild=FALSE;
                        if(LOW_MEM_DEVICE)rebuildList[i]->prepareVBO();
                        else
                        //issue #2 chunksToUpdateImmediatley shared data access with main thread, not synchronized
                        chunksToUpdateImmediatley[rebuildList[i]->idxn]=true;
                    }
              
                
               // if(idxrl==0)break;
                
            }
         //printg("idxrl:%d\n",idxrl);
        
        idxrl=0;
        
        
    }
    if(update_lighting){
        // Sliced: the lightbox sweep is O(window volume) and was one unbudgeted ~20ms (64z) /
        // ~80ms (256z) frame per teleport/warp -- the actual 256z bulk-reload spike (the chunk
        // mesh budget was already height-scaled). calculateLightingSlice does a budgeted strip of
        // columns per frame and returns TRUE only once the whole window is swept.
        double _f3l0=f3_now();
        bool _f3ld=calculateLightingSlice();
        g_f3.lit_slice_ms+=f3_now()-_f3l0; g_f3.lit_slice_calls++;
        if(_f3ld){
            update_lighting=false;
            g_f3.lit_done_frame=(int)g_f3.frames;
        }
        // hit_load_counter=0;
    }

    
    time3=((eden_platform_now_ms()-start)/1000.0);
    
    
    
}
// F3: end-of-frame bookkeeping. An event is over once the window is resident, nothing is dirty,
// nothing is waiting on upload, and the lighting sweep has returned TRUE. 1800-frame safety cap.
static void f3_frameEnd(bool loaded){
    if(!g_f3.active)return;
    g_f3.frames++;
    if(!loaded)return;
    if(bulk_reload_active||bulk_reload_meshing||update_lighting){
        if(g_f3.frames>=1800)f3_end(true);
        return;
    }
    const int n=CHUNKS_PER_SIDE*CHUNKS_PER_SIDE*CHUNKS_PER_COLUMN;
    for(int i=0;i<n;i++)
        // B.5b-1a: the outer ring is never meshed, so its dirty bits never clear; it is not owed work.
        if((chunksToUpdate[i]&&!slotOnOuterRing(i/(CHUNKS_PER_SIDE*CHUNKS_PER_COLUMN),
                                                (i/CHUNKS_PER_COLUMN)%CHUNKS_PER_SIDE))||
           chunksToUpdateImmediatley[i]){
            if(g_f3.frames>=1800)f3_end(true);
            return;
        }
    f3_end(false);
}
void Terrain::updateAllImportantChunks(){
	double start_time=((eden_platform_now_ms()-start)/1000.0);

    int count=0;
   
    for(int x=0;x<CHUNKS_PER_SIDE;x++){
        for(int z=0;z<CHUNKS_PER_SIDE;z++){
            // if(columnsToUpdate[getColIndex(x,z)]){
            for(int y=0;y<CHUNKS_PER_COLUMN;y++){
                
                //issue #2 continued
                if(chunksToUpdateImmediatley[threeToOne(x,y,z)]){
                    TerrainChunk* chunk;
                    //issue #3 chunk data unsychronized shared access, main thread, building thread AND loading thread
                    chunk=chunkTable[threeToOne(x,y,z)];
                    
                    
                    if(chunk){
                        
                        { F3Scope _f3(&g_f3.up_ms); g_f3.up_n++;
                        chunk->prepareVBO(); }
                        count++;
                        
                    }
                    
                    chunksToUpdateImmediatley[threeToOne(x,y,z)]=false;
                }
                
            }
            //     columnsToUpdate[getColIndex(x,z)]=FALSE;
        }
    }


    		
	
  
    if(count>0){
        
        double end_time=((eden_platform_now_ms()-start)/1000.0);
        float etime=end_time-start_time;
        etime+=.0001f;
        
        time4=((eden_platform_now_ms()-start)/1000.0);
        if(time1!=time2){
           /* double fr=time2-time1;
            double mg=time3-time2;
            double ml=time4-time3;
            int frp=fr/(fr+mg+ml)*100;
            int mgp=mg/(fr+mg+ml)*100;
            int mlp=ml/(fr+mg+ml)*100;
           
            //frp=mgp+mlp+frp;//<---delete
            if(count>50){
            printg("File read: %f(%d%%)    Mesh gen: %f(%d%%)     Mesh load: %f(%d%%)\n ",fr,frp,mg,mgp,ml,mlp);
            printg("Chunks loaded: %d     Mesh gen time per chunk: %f ms\n",count,1000*mg/(double)count);
            }*/
        }
  //  NSLog(@"chunk updates: %d  etime: %f  etime/count: %f\n",count,etime,etime/count);
	
    }
    f3_frameEnd(loaded);
   
    
}
void Terrain::colort(float r,float g,float b){
	Pen::color(r,g,b,1);
}
static TerrainChunk* renderList[CHUNKS_PER_SIDE_MAX*CHUNKS_PER_SIDE_MAX*CHUNKS_PER_COLUMN_MAX];
static TerrainChunk* renderList2[CHUNKS_PER_SIDE_MAX*CHUNKS_PER_SIDE_MAX*CHUNKS_PER_COLUMN_MAX];
void renderTree(TreeNode* node,int state){
    //once upon a time this descended an oct-tree, profiling showed it was useless, now just iterates through chunk list
    //P.7: one walk fills BOTH lists (was two walks, one per pass), one frustum test per chunk, table order kept.
    //in_view keeps its old meaning: membership in the transparent list (the second walk used to overwrite it).
    int n=CHUNKS_PER_SIDE*CHUNKS_PER_SIDE*CHUNKS_PER_COLUMN;
    int r1=chunks_rendered,r2=chunks_rendered2;
    for(int i=0;i<n;i++){
        TerrainChunk* chunk=chunkTablec[i];
        chunk->in_view=false;
        bool want1=chunk->n_vertices!=0||chunk->num_objects!=0;
        bool want2=chunk->n_vertices2!=0;
        if(!want1&&!want2)continue;
        if(ViewTestAABB(chunk->rbounds,state)&VT_OUTSIDE) continue;
        if(want1)renderList[r1++]=chunk;
        if(want2){
            chunk->in_view=true;
            renderList2[r2++]=chunk;
        }
    }
    chunks_rendered=r1;
    chunks_rendered2=r2;
}
int compare_rebuild_order (const void *a, const void *b)
{
    TerrainChunk* first=*((TerrainChunk**)(a));
    TerrainChunk* second=*((TerrainChunk**)(b));
    Vector cam=World::getWorld->player->pos;
    Vector center=MakeVector((first->pbounds[3]+first->pbounds[0])/2.0f,
                             (first->pbounds[4]+first->pbounds[1])/2.0f,
                             (first->pbounds[5]+first->pbounds[2])/2.0f);
    
    float dist=(cam.x-center.x)*(cam.x-center.x)+
    (cam.y-center.y)*(cam.y-center.y)+
    (cam.z-center.z)*(cam.z-center.z);
    
    if(first->in_view)dist/=4;
    
    center=MakeVector((second->pbounds[3]+second->pbounds[0])/2.0f,
                      (second->pbounds[4]+second->pbounds[1])/2.0f,
                      (second->pbounds[5]+second->pbounds[2])/2.0f);
    float dist2=((cam.x-center.x)*(cam.x-center.x)+
           (cam.y-center.y)*(cam.y-center.y)+
           (cam.z-center.z)*(cam.z-center.z));
    
    if(second->in_view)dist2/=4;
    dist-=dist2;
    
    if (dist > 0)
        return -1;
    else if (dist < 0)
        return 1;
    else
        return 0;
    
   
}

int compare_front2back (const void *a, const void *b)
{
    TerrainChunk* first=*((TerrainChunk**)(a));
    TerrainChunk* second=*((TerrainChunk**)(b));
    Vector cam=World::getWorld->player->pos;
    Vector center=MakeVector((first->pbounds[3]+first->pbounds[0])/2.0f,
                             (first->pbounds[4]+first->pbounds[1])/2.0f,
                             (first->pbounds[5]+first->pbounds[2])/2.0f);
    
    float dist=(cam.x-center.x)*(cam.x-center.x)+
             (cam.y-center.y)*(cam.y-center.y)+
    (cam.z-center.z)*(cam.z-center.z);
    
    center=MakeVector((second->pbounds[3]+second->pbounds[0])/2.0f,
                      (second->pbounds[4]+second->pbounds[1])/2.0f,
                      (second->pbounds[5]+second->pbounds[2])/2.0f);
    dist-=((cam.x-center.x)*(cam.x-center.x)+
    (cam.y-center.y)*(cam.y-center.y)+
    (cam.z-center.z)*(cam.z-center.z));
 
    if (dist > 0)
        return 1;
    else if (dist < 0)
        return -1;
    else
        return 0;
}
// Q.1 F19: the eye this frame's chunk face culling and transparent sort use. Set at the top of
// Terrain::render from Camera::viewPoints, so in third person (H.12) it is the real eye, not the
// pivot cam->px/py/pz, which sits up to CAM_TP_DIST away. First person: eye == pivot.
Vector g_cullEye;
int compare_back2front (const void *a, const void *b)
{
    TerrainChunk* first=*((TerrainChunk**)(a));
    TerrainChunk* second=*((TerrainChunk**)(b));
    Vector cam=g_cullEye;   // Q.1 F19: was World::getWorld->player->pos
    Vector center=MakeVector((first->pbounds[3]+first->pbounds[0])/2.0f,
                             (first->pbounds[4]+first->pbounds[1])/2.0f,
                             (first->pbounds[5]+first->pbounds[2])/2.0f);
    
    float dist=(cam.x-center.x)*(cam.x-center.x)+
    (cam.y-center.y)*(cam.y-center.y)+
    (cam.z-center.z)*(cam.z-center.z);
    
    center=MakeVector((second->pbounds[3]+second->pbounds[0])/2.0f,
                      (second->pbounds[4]+second->pbounds[1])/2.0f,
                      (second->pbounds[5]+second->pbounds[2])/2.0f);
    dist-=((cam.x-center.x)*(cam.x-center.x)+
           (cam.y-center.y)*(cam.y-center.y)+
           (cam.z-center.z)*(cam.z-center.z));
    
    if (dist > 0)
        return -1;
    else if (dist < 0)
        return 1;
    else
        return 0;
}
int compare_objects_back2front (const void *a, const void *b)
{
    StaticObject first=*((StaticObject*)(a));
    StaticObject second=*((StaticObject*)(b));
    Vector cam=World::getWorld->player->pos;
    Vector center=first.pos;
    
    float dist=(cam.x-center.x)*(cam.x-center.x)+
    (cam.y-center.y)*(cam.y-center.y)+
    (cam.z-center.z)*(cam.z-center.z);
    
    center=second.pos;
    dist-=((cam.x-center.x)*(cam.x-center.x)+
           (cam.y-center.y)*(cam.y-center.y)+
           (cam.z-center.z)*(cam.z-center.z));
    
    if (dist > 0)
        return -1;
    else if (dist < 0)
        return 1;
    else
        return 0;
}
extern float SCREEN_WIDTH;
extern bool SUPPORTS_OGL2;
extern float SCREEN_HEIGHT;
static int frame_counter=0;
static int frame=0;


static bool last_skycolor_was_defaultblue=false;

int lolc=0;

// -------------------------------------------------------------------------------------------------
// Persistent dynamic VBOs for the per-frame object batches (perf-audit row 23/E3, 2026-08-06).
//
// Doors, golden cubes, portal frames, portal swirls and flowers are still REBUILT ON THE CPU every
// frame -- they animate (door swing angle, cube rotation, portal UV swirl, flower billboard yaw) and
// none of that geometry logic changed. What changed is where the result lives.
//
// Each batch used to fill a fixed-size `vertexObject objVertices[max_render_objects*6*6]` STACK
// local (~380 KB) and hand it to GL as four CLIENT-SIDE arrays. On real ES 1.1 that was free. On
// WebGL it is not: client-side vertex data is forbidden outright, so the GL shim
// (web-port-final:web/src/shim/gl/gl_es1_shim.cpp, eden_gl_setup_attributes) had to copy the span into a streaming
// VBO once per ENABLED ARRAY per draw -- four uploads of the same interleaved bytes every frame,
// plus a fresh glVertexAttribPointer each time because the buffer it latched kept changing.
//
// Now each batch owns one persistent GL_DYNAMIC_DRAW buffer plus a heap staging array. The frame
// fills the staging array exactly as before, uploads it ONCE with glBufferSubData, and specifies the
// four arrays as byte offsets into that buffer. The shim then takes its VBO-resident path: zero
// uploads, and offsets that are identical frame to frame, so the specifications elide too.
//
// Measured in real Safari with tools/safari-objbatch-probe.js (24 doors + 24 golden cubes +
// 24 portals + 144 flowers in view, against an empty-flat-world control):
//     before   47 glBufferData/frame, 756.5 KB/frame  = 44.3 MB/s at 60 fps
//     after     8 glBufferData + 5 glBufferSubData/frame, 176.6 KB/frame = 10.3 MB/s
// i.e. the object batches' per-frame upload traffic dropped 4.3x, which is the 4-uploads-of-the-
// same-bytes redundancy plus the tail of the array the old fixed-size span always carried.
//
// Three things to know before touching this:
//  - One buffer PER BATCH, deliberately, not one shared buffer. A shared buffer would make all four
//    offset specifications byte-identical and therefore fully elidable, but it would also make each
//    batch's glBufferSubData a write to memory the previous batch's draw is still reading, i.e. an
//    implicit driver sync every batch. Separate buffers trade ~16 elidable setup calls for zero
//    write-after-read hazards; the uploads were the expensive half.
//  - (Until G.5b-6, when the batches became R:: draws that bind nothing for the ES 1.1 code:)
//    objBatchDraw() must be called with GL_ARRAY_BUFFER at the engine's own binding (0 at every
//    call site here), and it restores that before returning. docs/rendering.md: every pass assumes
//    its predecessor restored state, and a leaked non-zero GL_ARRAY_BUFFER silently reinterprets the
//    NEXT client-array glVertexPointer as a byte offset -- that does not fail, it draws garbage.
//  - Growing is REAL now, which retires three latent stack overruns the old fixed 10800-vertex
//    array had. A golden cube emits 144 vertices (6 faces x tess^2 x 6), so 76 visible cubes used to
//    write past the end of it; MAX_FLOWERS is 10000 flowers x 6 vertices = 60000; and `doorso`/
//    `portalso` were themselves unbounded writes into 500/200-entry stack arrays (now clamped).
//    objBatchStage() reallocs instead, and returns NULL rather than lying if that fails, which is
//    what the `objVertices&&` loop guards at each call site are for.
struct ObjectBatch {
    R::Buf buffer;             // renderer buffer (G.5b-6), 0 until the first draw
    int gpuCapacity;           // vertices the GL buffer is currently sized for
    vertexObject* staging;     // CPU staging array (was a stack local)
    int stagingCapacity;
};

static ObjectBatch g_doorBatch   = {0,0,NULL,0};
static ObjectBatch g_goldenBatch = {0,0,NULL,0};
static ObjectBatch g_portalBatch = {0,0,NULL,0};
static ObjectBatch g_swirlBatch  = {0,0,NULL,0};
static ObjectBatch g_flowerBatch = {0,0,NULL,0};

// Returns a staging array good for `verts` vertices, or NULL if it could not be had. Never shrinks:
// these are per-frame scratch buffers whose high-water mark is what matters.
static vertexObject* objBatchStage(ObjectBatch* b,int verts){
    if(verts<=0)return b->staging;
    if(verts>b->stagingCapacity){
        vertexObject* grown=(vertexObject*)realloc(b->staging,(size_t)verts*sizeof(vertexObject));
        if(grown==NULL)return NULL;   // old (smaller) array is still valid and still owned
        b->staging=grown;
        b->stagingCapacity=verts;
    }
    return b->staging;
}

// Upload `verts` staged vertices into the batch's buffer and draw them as one R:: draw (G.5b-6): the
// world program, or with `lit` R::drawLit under the LightSet the caller stated (doors, golden cubes;
// Settings > Video > Original lighting decides inside the renderer). kFmtObject carries the normals
// either way; the world program does not read them.
static void objBatchDraw(ObjectBatch* b,int verts,R::Tex tex,bool lit){
    if(verts<=0||b->staging==NULL)return;
    if(b->buffer==0){
        b->buffer=R::bufCreate(R::BK_VERTEX,R::U_DYNAMIC);
        if(b->buffer==0)return;
        b->gpuCapacity=0;
    }
    if(verts>b->gpuCapacity){
        // Powers of two so a scene drifting up in object count reallocates a handful of times per
        // session rather than every frame. Dynamic, not the static usage TerrainChunk::prepareVBO
        // uses -- this whole span is rewritten every single frame.
        int cap=b->gpuCapacity?b->gpuCapacity:1024;
        while(cap<verts)cap*=2;
        R::bufData(b->buffer,NULL,(size_t)cap*sizeof(vertexObject));
        b->gpuCapacity=cap;
    }
    R::bufSubData(b->buffer,0,b->staging,(size_t)verts*sizeof(vertexObject));
    R::Geometry g={};
    g.fmt=&R::kFmtObject; g.vbuf=b->buffer; g.itype=R::I_NONE; g.first=0; g.count=verts; g.prim=R::TRIANGLES;
    const R::Rgba white={1,1,1,1};
    if(lit)R::drawLit(g,tex,white);
    else R::drawWorld(g,tex,white);
}
// -------------------------------------------------------------------------------------------------

void Terrain::render(){
    { Vector at; World::getWorld->cam->viewPoints(&g_cullEye,&at); }   // Q.1 F19, before render2 too
    if(do_reload==-1)return;
   //  NSLog(@"rendering!!");
    Graphics::beginTerrain();
	
	vertices_rendered=0;
	faces_rendered=0;
	chunks_rendered=chunks_rendered2=0;
    
    R::uv().scale(1,1.0f/ATLAS_TILES,1);
   // glPushMatrix();
	R::mv().push();
	//float pushx=World::getWorld->fm.chunkOffsetX*CHUNK_SIZE*BLOCK_SIZE;
	//float pushz=World::getWorld->fm.chunkOffsetZ*CHUNK_SIZE*BLOCK_SIZE;
	//glTranslatef(-pushx, 0, -pushz);
   // printg("-------------start renderTree------------\n");
    pass_begin(P_R_CULL);
	renderTree(&troot,0);
    pass_end(P_R_CULL);
   //printg("--------x--x--x----end----x--x--x--------\n");
    
   // qsort (renderList, chunks_rendered, sizeof (TerrainChunk*), compare_front2back);
  //  glDisable(GL_TEXTURE_2D);
    int chunksr=chunks_rendered;
    pass_begin(P_R_CHUNKS);
    for(int i=0;i<chunks_rendered&&!pexp(EDEN_PEXP_NOCHUNKS);i++){
      
        vertices_rendered+=renderList[i]->render();
        /*if(vertices_rendered>=max_vertices&&World::getWorld->hud.fps<40){
            if(World::getWorld->hud.mode!=MODE_CAMERA){
            [Graphics setZFAR:P_ZFAR-.5f];
            chunks_rendered-=(chunks_rendered-i+1);
            break;
            }
        }*/
    }
    pass_end(P_R_CHUNKS);
    pass_begin(P_R_OBJECTS);
#define max_render_objects 300
    // G.5b-6: doors and golden cubes are R::drawLit (smooth shaded, as here); the lights below go into
    // R::LightSets. Their positions were loaded under an identity modelview: eye space as written.
    extern Vector colorTable[256];
    bool isNight=v_equals(final_skycolor,colorTable[54]);
    float lightPosition[4] = {0.0f,0.0f, 0.0f, 1.0f};
    float lightAmbient[4]  = {0.3f, 0.3f, 0.3f, 1.0f};
    float lightDiffuse[4]  = {0.7f, 0.7f, 0.7f, 1.0f};
    
    float lightPosition2[4] = {1.0f,1.0f, 0.0f, 1.0f};
    float lightAmbient2[4]  = {0.3f, 0.3f, 0.3f, 1.0f};
    float lightDiffuse2[4]  = {0.3f, 0.3f, 0.3f, 1.0f};
    
    
    if(!LOW_MEM_DEVICE&&isNight){
        float NlightPosition[4] = {0.0f,0.0f, 0.0f, 1.0f};
        float NlightAmbient[4]  = {0.17f, 0.17f, 0.17f, 1.0f};
        float NlightDiffuse[4]  = {0.3f, 0.3f, 0.3f, 1.0f};
        
        float NlightPosition2[4] = {1.0f,1.0f, 0.0f, 1.0f};
        float NlightAmbient2[4]  = {0.15f, 0.15f, 0.15f, 1.0f};
        float NlightDiffuse2[4]  = {0.15f, 0.15f, 0.15f, 1.0f};
        for(int i=0;i<4;i++){
            lightPosition[i]=NlightPosition[i];
            lightAmbient[i]  =NlightAmbient[i];
            lightDiffuse[i]  =NlightDiffuse[i];
            
            lightPosition2[i] =NlightPosition2[i];
            lightAmbient2[i]  =NlightAmbient2[i];
            lightDiffuse2[i]  =NlightDiffuse2[i];
        }
    }
    
    //PVRTVec4 lightSpecular = PVRTVec4(0.2f, 0.2f, 0.2f, 1.0f);
    
    // Doors: LIGHT0 at the eye, LIGHT1 off, ES 1.1's default model ambient 0.2 and material specular 0
    // (the golden cube puts it back to 0 after itself), so no highlight.
    R::LightSet lights={};
    lights.light[0].on=true;
    for(int i=0;i<4;i++){
        lights.light[0].pos[i]=lightPosition[i];
        lights.light[0].ambient[i]=lightAmbient[i];
        lights.light[0].diffuse[i]=lightDiffuse[i];
        lights.light[1].pos[i]=lightPosition2[i];
        lights.light[1].ambient[i]=lightAmbient2[i];
        lights.light[1].diffuse[i]=lightDiffuse2[i];
        lights.modelAmbient[i]=i<3?0.2f:1.0f;
    }
    R::setLights(lights);
    extern const int16_t cubeShortVertices[36*3];
    extern const int16_t cubeTexture[36*2];
    extern const float cubeNormals[36*3];
    const int16_t* cubeVertices=cubeShortVertices;
    const int16_t* cubeTextureCustom=cubeTexture;
    StaticObject* doorso[500];
    int num_doors=0;
    for(int i=0;i<chunksr;i++){
        for(int j=0;j<renderList[i]->num_objects;j++){
            if(renderList[i]->objects[j].type!=TYPE_DOOR_TOP)continue;
            if(num_doors<500)doorso[num_doors++]=&renderList[i]->objects[j];
        }
    }
    vertexObject* objVertices=objBatchStage(&g_doorBatch,num_doors*6*6);
   // printg("chunksr %d   num doors:%d\n",chunksr,num_doors);
    int vert=0;
    int object=0;
    
    
   
        setViewNow();
    Vector ppos=World::getWorld->player->pos;
    for(int clr=0;clr<60;clr++){
        vert=0;
        object=0;
    for(int i=0;objVertices&&i<num_doors;i++){
        StaticObject* door=doorso[i];
        if(door->color!=clr){
            continue;
        }
        int dir=door->dir;
        Vector v1=door->pos;
        v1.x+=.5f;
        v1.z+=.5f;
        v1.y=ppos.y;
        Vector vdist=v_sub(ppos,v1);
        
        int dist=v_length2(vdist);
        int prev_ani=door->ani;
        if(ppos.y>=door->pos.y&&ppos.y<=door->pos.y+2&&dist<2*2){
            door->ani=-1;
        }else
            door->ani=1;
        
        if(prev_ani!=door->ani){
            if(door->ani<0){
                Resources::getResources->playSound(S_DOOR_OPEN);
            }else if(door->ani > 0){
                Resources::getResources->playSound(S_DOOR_CLOSED);
            }
        }
        float rot=door->rot;
        /*if(renderList[i].objects[j].ani==0){
         renderList[i].objects[j].ani=1;
         }*/
        if(door->ani==1){
            door->rot+=6*last_etime;
        }else if(door->ani==-1){
            door->rot-=6*last_etime;
        }
        if(door->rot<0){
            
            door->rot=0;
            
           
            
        }
        if(door->rot>M_PI/2){
            
            

            door->rot=M_PI/2;
            
        }
        Vector offsets=MakeVector(0,0,0);
        if(dir==0){
            offsets.x=4;
            rot+=M_PI/2;
        }else if(dir==1){
            rot+=M_PI;
            offsets.z=4;
            offsets.x=4;
            
            
        }else if(dir==2){
            rot+=M_PI+M_PI/2;
            offsets.z=4;
            
        }else if(dir==3){
            
        }
        
        if(rot>2*M_PI)rot-=(int)(rot/(2*M_PI))*(2*M_PI);
        
        
        for(int k=0;k<6*6;k++){
            Vector vc;
            
            vc=MakeVector(cubeVertices[k*3]*4,cubeVertices[k*3+1]*2*4,cubeVertices[k*3+2]*.60f-.30);
            
            
            
            vc=rotateVertice(MakeVector(0,rot,0),vc);
            vc.x+=offsets.x;
            vc.z+=offsets.z;
            if(vc.x<0)vc.x=0;
            if(vc.z<0)vc.z=0;
            if(vc.x>4)vc.x=4;
            if(vc.z>4)vc.z=4;
            /*if(vc.x>4)vc.x=4;
             if(vc.z>4)vc.z=4;*/
            objVertices[vert].position[0]=4*(door->pos.x-World::getWorld->fm->chunkOffsetX*CHUNK_SIZE)+vc.x;
            objVertices[vert].position[1]=4*door->pos.y+vc.y;
            objVertices[vert].position[2]=4*(door->pos.z-World::getWorld->fm->chunkOffsetZ*CHUNK_SIZE)+vc.z;
            if(k==0){
                /* printg("door objVertices[%d]= (%d,%d,%d) suggested offsets(%d,%d),\n",j,door->pos[0]
                 ,door->pos[1]
                 ,door->pos[2],
                 -World::getWorld->fm.chunkOffsetX,-World::getWorld->fm.chunkOffsetZ);*/
                
            }
            Vector vn=MakeVector(cubeNormals[k*3],cubeNormals[k*3+1],cubeNormals[k*3+2]);
            
            vn=rotateVertice(MakeVector(0,rot,0),vn);
            objVertices[vert].normal[0]=vn.x;
            objVertices[vert].normal[1]=vn.y;
            objVertices[vert].normal[2]=vn.z;
            /*
             for(int coord=0;coord<3;coord++){
             if(coord==1)
             objVertices[vert].position[coord]=4*renderList[i].objects[j].pos[coord]+4*cubeVertices[k*3+coord]*2;
             else if (coord==2)
             objVertices[vert].position[coord]=4*renderList[i].objects[j].pos[coord]+.5f*cubeVertices[k*3+coord];
             else
             objVertices[vert].position[coord]=4*renderList[i].objects[j].pos[coord]+4*cubeVertices[k*3+coord];
             }*/
            if(k>=12){
                objVertices[vert].texs[0]=cubeTextureCustom[k*2+0];
                objVertices[vert].texs[1]=0;
            }else{
                if(k>=6){
                    objVertices[vert].texs[0]=cubeTextureCustom[k*2+0];
                    objVertices[vert].texs[1]=cubeTextureCustom[k*2+1]*ATLAS_TILES;
                }
                else{
                    objVertices[vert].texs[0]=1-cubeTextureCustom[k*2+0];
                    objVertices[vert].texs[1]=cubeTextureCustom[k*2+1]*ATLAS_TILES;
                    
                    
                }
            }
            extern Vector colorTable[256];
            Vector color=colorTable[door->color];
            if(true||door->color==0){
                color.x=color.y=color.z=1;
                
            }
            objVertices[vert].colors[0]=color.x*255;
            objVertices[vert].colors[1]=color.y*255;
            objVertices[vert].colors[2]=color.z*255;
            objVertices[vert].colors[3]=255;
            vert++;
            
        }
    }
        if(vert!=0){
        objBatchDraw(&g_doorBatch,vert,Resources::getResources->getDoorTex(clr),true);
        }
    }
    
    //float lightDiffuse[4]  = {0.7f, 0.7f, 0.7f, 1.0f};
    
    cubeVertices=cubeShortVertices;
   cubeTextureCustom=cubeTexture;
    vert=0;
    object=0;
    lolc++;
    
    int tess=2;

    float tessf=1.0f/(float)tess;
    // A cube emits 6 faces x tess^2 subquads x 6 vertices; at tess=2 that is 144 per object, which
    // is why max_render_objects of them never fitted the old max_render_objects*6*6 array.
    // Q.1 F9: the rotated unit-cube corners/normals are the same for every golden cube; rotate once per frame.
    static Vector gcV[144],gcN[144];
    {
        int idx=0;
        for(int f=0;f<6;f++)
            for(int x=0;x<tess;x++){
                float xoff=x*tessf;
                for(int y=0;y<tess;y++){
                    float yoff=y*tessf;
                    for(int qc=0;qc<6;qc++){
                        int k=f*6+qc;
                        Vector vc;
                        if(f==0||f==1){
                            vc=MakeVector(cubeVertices[k*3]*tessf+xoff-.5f,cubeVertices[k*3+1]*tessf+yoff-.5f,cubeVertices[k*3+2]-.5f);
                        }else if(f==2||f==3){
                            vc=MakeVector(cubeVertices[k*3]-.5f,cubeVertices[k*3+1]*tessf+yoff-.5f,cubeVertices[k*3+2]*tessf+xoff-.5f);
                        }else{
                            vc=MakeVector(cubeVertices[k*3]*tessf+yoff-.5f,cubeVertices[k*3+1]-.5f,cubeVertices[k*3+2]*tessf+xoff-.5f);
                        }
                        gcV[idx]=rotateVertice(gcrot,vc);
                        gcN[idx]=rotateVertice(gcrot,MakeVector(cubeNormals[k*3],cubeNormals[k*3+1],cubeNormals[k*3+2]));
                        idx++;
                    }
                }
            }
    }
    objVertices=objBatchStage(&g_goldenBatch,max_render_objects*6*tess*tess*6);
    for(int i=0;objVertices&&i<chunksr;i++){
        for(int j=0;j<renderList[i]->num_objects;j++){
            if(renderList[i]->objects[j].type!=TYPE_GOLDEN_CUBE)continue;
            int gidx=0;
            for(int f=0;f<6;f++)
                
                for(int x=0;x<tess;x++){
                    for(int y=0;y<tess;y++){
                        for(int qc=0;qc<6;qc++){
                            Vector vc=gcV[gidx];
                            Vector vn=gcN[gidx];
                            gidx++;
                            vc.x+=.5f;
                            vc.y+=.5f;
                            vc.z+=.5f;
                            objVertices[vert].normal[0]=vn.x;
                            objVertices[vert].normal[1]=vn.y;
                            objVertices[vert].normal[2]=vn.z;
                            
                            
                            objVertices[vert].position[0]=4*(renderList[i]->objects[j].pos.x-World::getWorld->fm->chunkOffsetX*CHUNK_SIZE)+1+2.3*vc.x;
                            objVertices[vert].position[1]=4*renderList[i]->objects[j].pos.y+1+2.3*vc.y;
                            objVertices[vert].position[2]=4*(renderList[i]->objects[j].pos.z-World::getWorld->fm->chunkOffsetZ*CHUNK_SIZE)+1+2.3*vc.z;
                            
                            //objVertices[vert].texs[0]=xoff;
                            //objVertices[vert].texs[1]=yoff;
                          
                            CalcEnvMap(&objVertices[vert]);
                            
                            
                           
                            
                            
                            extern Vector colorTable[256];
                            Vector color=colorTable[renderList[i]->objects[j].color];
                            if(renderList[i]->objects[j].color==0){
                                color.x=color.y=color.z=1;
                                
                            }
                            objVertices[vert].colors[0]=color.x*255;
                            objVertices[vert].colors[1]=color.y*255;
                            objVertices[vert].colors[2]=color.z*255;
                            objVertices[vert].colors[3]=255;
                            vert++;
                        } 
                    }
                }
            
            object++;
            if(object==max_render_objects)break;
        }
        if(object==max_render_objects)break;
    }
    
    // Golden cubes: LIGHT1 alone, white specular on light and material, shininess 100 (the sphere-map
    // UVs come from CalcEnvMap above). The stock code then put material and LIGHT0 specular back to 0.
    lights.light[0].on=false;
    lights.light[1].on=true;
    for(int i=0;i<4;i++){
        lights.light[1].specular[i]=1.0f;
        lights.matSpecular[i]=1.0f;
    }
    lights.shininess=100.0f;
   // glDisable(GL_TEXTURE_2D);
    R::setLights(lights);
    objBatchDraw(&g_goldenBatch,vert,Resources::getResources->getTex(ICO_SPHEREMAP)->name,true);
   // glEnable(GL_TEXTURE_2D);
    
    cubeVertices=cubeShortVertices;
     cubeTextureCustom=cubeTexture;
    vert=0;
   object=0;
    objVertices=objBatchStage(&g_portalBatch,max_render_objects*6*6);
    for(int i=0;objVertices&&i<chunksr;i++){
        for(int j=0;j<renderList[i]->num_objects;j++){
            if(renderList[i]->objects[j].type!=TYPE_PORTAL_TOP)continue;
           // printg("drawing portal\n");
            
            float rot=0;
            int dir=(renderList[i]->objects[j].dir+3)%4;
            Vector offsets=MakeVector(0,0,0);
            if(dir==0){
                offsets.x=4;
                rot+=M_PI/2;
            }else if(dir==1){
                rot+=M_PI;
                offsets.z=4;
                offsets.x=4;
                
                
            }else if(dir==2){
                rot+=M_PI+M_PI/2; 
                offsets.z=4;
                
            }else if(dir==3){
                
            }
            
            if(rot>2*M_PI)rot-=(int)(rot/(2*M_PI))*(2*M_PI);
            
            
          //  for(int k=0;k<6*6;k++){
               // Vector vc;
                
                //vc=MakeVector(cubeVertices[k*3]*4,cubeVertices[k*3+1]*2*4,cubeVertices[k*3+2]*.50f-.25);
                
                
               
            
            
            for(int k=0;k<6*6;k++){
                Vector vc=MakeVector(cubeVertices[k*3]*4,cubeVertices[k*3+1]*2*4,cubeVertices[k*3+2]*.25f-.25f);
                
                vc=rotateVertice(MakeVector(0,rot,0),vc);
                vc.x+=offsets.x;
                vc.z+=offsets.z;
               // if(vc.x<0)vc.x=0;
                //if(vc.z<0)vc.z=0;
                //if(vc.x>4)vc.x=4;
                //if(vc.z>4)vc.z=4;
               // vc=rotateVertice(MakeVector(0,rot,0),vc);
                objVertices[vert].position[0]=4*(renderList[i]->objects[j].pos.x-World::getWorld->fm->chunkOffsetX*CHUNK_SIZE)+vc.x;
                objVertices[vert].position[1]=4*renderList[i]->objects[j].pos.y+vc.y;
                objVertices[vert].position[2]=4*(renderList[i]->objects[j].pos.z-World::getWorld->fm->chunkOffsetZ*CHUNK_SIZE)+vc.z;
                
               
                if(k>=12){
                    objVertices[vert].texs[0]=cubeTextureCustom[k*2+0];     
                    objVertices[vert].texs[1]=0;
                }else{
                    if(k>=6){
                        objVertices[vert].texs[0]=cubeTextureCustom[k*2+0];
                        objVertices[vert].texs[1]=cubeTextureCustom[k*2+1]*ATLAS_TILES;
                    }
                    else{
                        objVertices[vert].texs[0]=1-cubeTextureCustom[k*2+0];
                        objVertices[vert].texs[1]=cubeTextureCustom[k*2+1]*ATLAS_TILES;
                        
                        
                    }
                }
                extern Vector colorTable[256];
                Vector color=colorTable[renderList[i]->objects[j].color];
                if(true||renderList[i]->objects[j].color==0){
                    color.x=color.y=color.z=1;
                    
                }
                objVertices[vert].colors[0]=color.x*255;
                objVertices[vert].colors[1]=color.y*255;
                objVertices[vert].colors[2]=color.z*255;
                objVertices[vert].colors[3]=255;
                vert++;
            }
            
            object++;
            if(object==max_render_objects)break;
        }
        if(object==max_render_objects)break;
    }
    
  
    
    const R::Tex portalTex=Resources::getResources->getTex(ICO_PORTAL)->name;
    objBatchDraw(&g_portalBatch,vert,portalTex,false);
    pass_end(P_R_OBJECTS);
    
    
    
    
    pass_begin(P_R_SKY);
    ///////////sky
    // G.5b-6: the sky's ortho bracket and state on R::. Fog stays off from here to the end of the pass
    // (the swirls too), as the stock glDisable(GL_FOG) left it. Depth test and write and cull as the
    // pass; the BW layer alpha-blends over the first one. (Stock used whatever blend func the last frame's
    // HUD left: SA,1-SA, or with the HUD hidden beginHud's ONE,ONE, an accident; docs/rendering.md.)
    R::fogOff();
    R::proj().push();
    R::proj().loadIdentity();
/*	if(World::getWorld->FLIPPED)
		glRotatef(90,0,0,1);
	else
		glRotatef(270,0,0,1);*/
    if(IS_IPAD){
        if(IS_RETINA)
            R::proj().ortho(0, SCREEN_WIDTH*2, 0, SCREEN_HEIGHT*2, -1, P_ZFAR);
        else
            R::proj().ortho(0, IPAD_WIDTH, 0, IPAD_HEIGHT, -1, P_ZFAR);
	}else
        R::proj().ortho(0, SCREEN_WIDTH, 0, SCREEN_HEIGHT, -1, P_ZFAR);
	R::mv().push();
    R::mv().loadIdentity();
    const R::State skyOpaque={R::B_OFF,true,true,true,0.0f,0.0f};
    const R::State skyBlend={R::B_ALPHA,true,true,true,0.0f,0.0f};
    float handoffColor[4]={1,1,1,1};   // the current colour the stock glColor4f calls left behind
    //skycolor.x=0;
    extern Vector colorTable[256];
    if(v_equals(final_skycolor,colorTable[14])){
        last_skycolor_was_defaultblue=true;
        R::setState(skyOpaque);
        Resources::getResources->getTex(ICO_SKY_BOX)->drawSky(CGRectMake(0,0, SCREEN_WIDTH,SCREEN_HEIGHT) ,-P_ZFAR/1.000001,1,1,1,1);
        if(  blending_alpha>0&&
           (blending||
            !v_equals(final_skycolor,skycolor)  )
           ){
            if(!blending){
                blending=true;
                blending_alpha=1.0f;
            }
            Vector v=skycolor;
          //  Vector v2=World::getWorld->terrain.final_skycolor;
           // float alpha=1.0;
            R::setState(skyBlend);
            Resources::getResources->getTex(ICO_SKY_BOX_BW)->drawSky(CGRectMake(0,0, SCREEN_WIDTH,SCREEN_HEIGHT) ,-P_ZFAR/1.000004,v.x,v.y,v.z,blending_alpha);
            handoffColor[0]=v.x; handoffColor[1]=v.y; handoffColor[2]=v.z; handoffColor[3]=blending_alpha;
           // blending_alpha-=.04f;
            if(blending_alpha<0){
                blending=false;
            }
        }else{
            if(v_equals(final_skycolor,skycolor)){
            blending_alpha=1.0f;
            blending=false;
            }
        }
    }else{
        if( last_skycolor_was_defaultblue){
            blending=true;
            blending_alpha=0.0f;
            last_skycolor_was_defaultblue=false;
        }
        //    extern Vector colorTable[256];
        if(blending){
            R::setState(skyOpaque);
            Resources::getResources->getTex(ICO_SKY_BOX)->drawSky(CGRectMake(0,0, SCREEN_WIDTH,SCREEN_HEIGHT),-P_ZFAR/1.000001,1,1,1,1);
            Vector v=skycolor;
           // blending_alpha+=.04f;
            if(blending_alpha>1.0f){
                blending=false;
            }
            R::setState(skyBlend);
            Resources::getResources->getTex(ICO_SKY_BOX_BW)->drawSky(CGRectMake(0,0, SCREEN_WIDTH,SCREEN_HEIGHT),-P_ZFAR/1.000004,v.x,v.y,v.z,blending_alpha);
        }else{
        Vector v=skycolor;
        R::setState(skyOpaque);
        Resources::getResources->getTex(ICO_SKY_BOX_BW)->drawSky(CGRectMake(0,0, SCREEN_WIDTH,SCREEN_HEIGHT),-P_ZFAR/1.000001,v.x,v.y,v.z,1.0f);
        }
    }
    R::mv().pop();
    R::proj().pop();
    ////////////////sky
    pass_end(P_R_SKY);
    pass_begin(P_R_OBJECTS);
    R::setState(skyOpaque);   // the swirls: no blend, unfogged (see the sky above)
    R::Tex handoffTex=portalTex;
    StaticObject* portalso[200];
    int num_portals=0;
    for(int i=0;i<chunksr;i++){
        for(int j=0;j<renderList[i]->num_objects;j++){
            if(renderList[i]->objects[j].type!=TYPE_PORTAL_TOP)continue;
            if(num_portals<200)portalso[num_portals++]=&renderList[i]->objects[j];
        }
    }
    objVertices=objBatchStage(&g_swirlBatch,num_portals*6);

   // for(int clr=0;clr<60;clr++){
        vert=0;
        object=0;


    for(int i=0;objVertices&&i<num_portals;i++){
        StaticObject* portal=portalso[i];
       // if(portal->color!=clr){
       //     continue;
       // }
            float rot=0;
            int dir=(portal->dir+3)%4;
            Vector offsets=MakeVector(0,0,0);
            if(dir==0){
                offsets.x=4;
                rot+=M_PI/2;
            }else if(dir==1){
                rot+=M_PI;
                offsets.z=4;
                offsets.x=4;
                
                
            }else if(dir==2){
                rot+=M_PI+M_PI/2; 
                offsets.z=4;
                
            }else if(dir==3){
                
            }
            
            if(rot>2*M_PI)rot-=(int)(rot/(2*M_PI))*(2*M_PI);
            
                        
            
            
            
            for(int k=0;k<6;k++){
                
                Vector vc=MakeVector((cubeVertices[k*3]-cubeVertices[k*3]*20.0f/64.0f+10.0f/64.0f)*4,
                                     (cubeVertices[k*3+1]*2-(cubeVertices[k*3+1]*(6.0f/64.0f+4.0f/64.0f))+4.0f/64.0f)*4,
                                     
                                     cubeVertices[k*3+2]*.25f-.26f);
                
                vc=rotateVertice(MakeVector(0,rot,0),vc);
                vc.x+=offsets.x;
                vc.z+=offsets.z;
                
                
                objVertices[vert].position[0]=4*(portal->pos.x-World::getWorld->fm->chunkOffsetX*CHUNK_SIZE)+vc.x;
                objVertices[vert].position[1]=4*portal->pos.y+vc.y;
                objVertices[vert].position[2]=4*(portal->pos.z-World::getWorld->fm->chunkOffsetZ*CHUNK_SIZE)+vc.z;
                
                
                vc=MakeVector((cubeTextureCustom[k*2+0]*(.723-.276f)+.276f)-.5f,(cubeTextureCustom[k*2+1]*(.947-.053f)+.053f)-.5f,0);
                vc=rotateVertice(portal_rot,vc);
                objVertices[vert].texs[0]=vc.x+.5f;
                objVertices[vert].texs[1]=(vc.y+.5f)*ATLAS_TILES;
                        
                
                extern Vector colorTable[256];
                Vector color=colorTable[portal->color];
                if(portal->color==0){
                    color.x=color.y=color.z=1;
                    
                }
                objVertices[vert].colors[0]=color.x*255;
                objVertices[vert].colors[1]=color.y*255;
                objVertices[vert].colors[2]=color.z*255;
                objVertices[vert].colors[3]=255;
                vert++;
                
            }
           
        
        if(object==max_render_objects)break;
    }
        
        
        
        if(vert!=0){
        handoffTex=Resources::getResources->getTex(ICO_SWIRL)->name;
        objBatchDraw(&g_swirlBatch,vert,handoffTex,false);
        }
    pass_end(P_R_OBJECTS);

   // }
    
       


    
      
        // glEnable(GL_TEXTURE_2D);
    /*if(World::getWorld->hud.fps<55){
        if(World::getWorld->hud.mode!=MODE_CAMERA){
            [Graphics setZFAR:P_ZFAR-.4f];
            
            
        }
    }*/
	R::mv().pop();
    R::uv().scale(1,(float)ATLAS_TILES,1);
    // What the stock pass left for the HUD's pen (G.5b-7): the last texture bound and the sky's last
    // colour. (Its lighting and normal-array lines fed only the shim.)
    Pen::texture(handoffTex);
    Pen::color(handoffColor[0],handoffColor[1],handoffColor[2],handoffColor[3]);
    //if(!SUPPORTS_OGL2)
       	frame_counter++;
   
    
    firstframe=false;
    if(pendingRespawnSound){pendingRespawnSound=false;Resources::getResources->playSound(S_RESPAWN);}   // H.9
	if(frame_counter==120){
	//printg("chunks: %d, faces: %d, vertices: %d\n",chunks_rendered,faces_rendered,vertices_rendered);
		frame_counter=0;
	}
    Graphics::endTerrain();
	
}
int getFlowerIndex(int color){
    if(color==0)return 31;
    color--;
    int hue=color%9;
    int sat=color/9;
    
    
    return hue*3+sat/2;
}
void Terrain::render2(){
    // G.5b-6: the transparent pass on R::. Alpha blend (SA,1-SA), depth test and write, cull, the world
    // fog; the blocks' x0.25; the atlas scale plus the liquid row shift on R::uv(). Graphics::endTerrain
    // closes it as it closes Terrain::render.
    const R::State st={R::B_ALPHA,true,true,true,0.0f,0.0f};
    R::setState(st);
    Graphics::applyWorldFog();
	R::mv().push();
	R::mv().scale(.25f,.25f,.25f);
    R::uv().scale(1,1.0f/ATLAS_TILES,1);
    // glPushMatrix();
    // if(!SUPPORTS_OGL2)
  //  glEnable(GL_FOG);
    R::mv().push();
    frame=(frame+1)%128;
    R::uv().translate(0,(int)(frame/16),0);
    
	//float pushx=World::getWorld->fm.chunkOffsetX*CHUNK_SIZE*BLOCK_SIZE;
	//float pushz=World::getWorld->fm.chunkOffsetZ*CHUNK_SIZE*BLOCK_SIZE;
	//glTranslatef(-pushx, 0, -pushz);
    secondPass=true;
    pass_begin(P_R_CULL);
    //P.7: renderList2 was filled by the single renderTree walk above
    qsort (renderList2, chunks_rendered2, sizeof (TerrainChunk*), compare_back2front);
    pass_end(P_R_CULL);
    
    pass_begin(P_R_TCHUNKS);
    for(int i=0;i<chunks_rendered2&&!pexp(EDEN_PEXP_NOCHUNKS);i++){
        renderList2[i]->render2();
    }
    pass_end(P_R_TCHUNKS);
    pass_begin(P_R_OBJECTS);
    R::uv().translate(0,-(int)(frame/16),0);
    R::uv().scale(1,(float)ATLAS_TILES,1);
    
    extern const int16_t cubeShortVertices[36*3];
    extern const int16_t cubeTexture[36*2];
    extern const float cubeNormals[36*3];
    const int16_t* cubeVertices=cubeShortVertices;
    const int16_t* cubeTextureCustom=cubeTexture;
    int vert=0;
    int object=0;
    setViewNow();
    cubeVertices=cubeShortVertices;
    cubeTextureCustom=cubeTexture;
#define MAX_FLOWERS 10000
    StaticObject flowerList[10000];
    
    vert=0;
    object=0;
    int flowers=0;
    for(int i=0;i<chunks_rendered;i++){
        for(int j=0;j<renderList[i]->num_objects;j++){
            if(renderList[i]->objects[j].type!=TYPE_FLOWER)continue;
            if(flowers>=MAX_FLOWERS)break;
            flowerList[flowers]=renderList[i]->objects[j];
            flowers++;
        }
    }
    
    qsort (flowerList, flowers, sizeof (StaticObject), compare_objects_back2front);
    extern Vector colorTable[256];
    bool isNight=v_equals(final_skycolor,colorTable[54]);
    // 6 vertices per billboard. MAX_FLOWERS of them is 60000, which never fitted the old
    // max_render_objects*6*6 (10800) stack array -- 1801 visible flowers overran it.
    vertexObject* objVertices=objBatchStage(&g_flowerBatch,flowers*6);
    for(int i=0;objVertices&&i<flowers;i++){
        
        
        // printg("rendering flower?\n");
        for(int k=0;k<6;k++){
            Vector vc=MakeVector((cubeVertices[k*3]-.5f)*.5f,cubeVertices[k*3+1],cubeVertices[k*3+2]);
            Vector dir;
            dir.y=0;
            dir.x=flowerList[i].pos.x+.5f-World::getWorld->player->pos.x;
            dir.z=flowerList[i].pos.z+.5f-World::getWorld->player->pos.z;
            
            float targetangle=(atan2(dir.z,dir.x)-atan2(0,1))-M_PI_2;
            
            
            
            
            vc=rotateVertice(MakeVector(0,targetangle,0),vc);
            vc.x+=.5f;
            vc.z+=.5f;
            /*
             Vector vn=MakeVector(cubeNormals[k*3],cubeNormals[k*3+1],cubeNormals[k*3+2]);
             
             vn=rotateVertice(gcrot,vn);
             objVertices[vert].normal[0]=vn.x;
             objVertices[vert].normal[1]=vn.y;
             objVertices[vert].normal[2]=vn.z;*/
            
            objVertices[vert].position[0]=4*(flowerList[i].pos.x-World::getWorld->fm->chunkOffsetX*CHUNK_SIZE)+4*vc.x;
            objVertices[vert].position[1]=4*flowerList[i].pos.y+4*vc.y;
            objVertices[vert].position[2]=4*(flowerList[i].pos.z-World::getWorld->fm->chunkOffsetZ*CHUNK_SIZE)+4*vc.z;
            
            
            int sidx=getFlowerIndex(flowerList[i].color);
          //  vert_array[vert_c].texs[0]=cubeTextureCustom[st]*size;
            
           // printg("picking flower:%d\n",sidx);
            
            
            
           // vert_array[vert_c].texs[1]=cubeTextureCustom[st+1]*tp.y+tp.x;
            int row=sidx/8;
            int col=sidx%8;
            float width=1/8.0f;
            float height=1/4.0f;
            objVertices[vert].texs[0]=cubeTextureCustom[k*2+0]*width+col*width;
            objVertices[vert].texs[1]=cubeTextureCustom[k*2+1]*height+row*height;
            
            extern Vector colorTable[256];
            Vector color=colorTable[flowerList[i].color];
            color.x=color.y=color.z=1;
            
            float skylight=.35f;
            if(!isNight||LOW_MEM_DEVICE)skylight=1.0f;
            float light[3];
            calcLight3(flowerList[i].pos.x,flowerList[i].pos.z,flowerList[i].pos.y,skylight,light);
            if(light[0]>1){
                light[0]=1;
            }if(light[1]>1){
                light[1]=1;
            }
            if(light[2]>1){
                light[2]=1;
            }
            
           // if(!isNight){
            objVertices[vert].colors[0]=color.x*255*light[0];
            objVertices[vert].colors[1]=color.y*255*light[1];
            objVertices[vert].colors[2]=color.z*255*light[2];
           //}else{
                
            //    objVertices[vert].colors[0]=color.x*255*.65f;
             //   objVertices[vert].colors[1]=color.y*255*.65f;
              //  objVertices[vert].colors[2]=color.z*255*.65f;
         //   }
            objVertices[vert].colors[3]=255;
            vert++;
        }
        
    }
    
    
    //glDepthMask(GL_FALSE);
    const R::Tex flowerTex=Resources::getResources->getTex(ICO_FLOWER)->name;
    objBatchDraw(&g_flowerBatch,vert,flowerTex,false);
    pass_end(P_R_OBJECTS);

    
   
    secondPass=false;
	R::mv().pop();
    // The flower texture stays the pen's binding, as the stock pass left it (G.5b-7).
    Pen::texture(flowerTex);
    Pen::depthWrite(true);
    Graphics::endTerrain();
    
    
}
Terrain::~Terrain(){
	if(loaded)
        unloadTerrain(false);
		
	//free(landscape);
	//landscape=NULL;
	
}


/*- (BOOL)isVisible:(int)x:(int)z:(int)y{
 Camera* cam=World::getWorld->cam;
 Vector cpos,bpos;
 Vector a;
 //cdir.x=cam.look.x-cam.px;
 //cdir.y=cam.look.y-cam.py;
 //cdir.z=cam.look.z-cam.pz;
 cpos.x=cam.px/BLOCK_SIZE;
 cpos.y=cam.py/BLOCK_SIZE;
 cpos.z=cam.pz/BLOCK_SIZE;
 bpos.x=x;
 bpos.y=y;
 bpos.z=z;
 a.x=cpos.x-bpos.x;
 a.y=cpos.y-bpos.y;
 a.z=cpos.z-bpos.z;
 
 int hidden=0;
 
 if(a.y>0){		
 if(getLandc(x, z, y+1)>0)
 hidden++;			
 }else{
 if(getLandc(x, z, y-1)>0)
 hidden++;			
 }
 if(a.x>0){
 if(getLandc(x+1, z, y)>0)
 hidden++;			
 }else{
 if(getLandc(x-1, z, y)>0)
 hidden++;			
 }
 if(a.z>0){
 if(getLandc(x, z+1, y)>0)
 hidden++;			
 }else{
 if(getLandc(x, z-1, y)>0)
 hidden++;			
 }
 
 if(hidden==3)
 return FALSE;
 
 
 return TRUE;
 
 
 }*/
