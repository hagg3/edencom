//
//  Terrain.m
//  prototype
//
//  Created by Ari Ronen on 10/11/10.
//  Copyright 2010 __MyCompanyName__. All rights reserved.
//

#import "Terrain.h"
#import "Frustum.h"
#import "Globals.h"
#import "Model.h"
#import "VectorUtil.h"

#import "Lighting.h"
#import "MeshPool.h"

// Repo-root Lighting.h is the built one (Eden.xcodeproj / web CMake); the Classes/ copy next to
// this file is a stale snapshot with no prototypes, hence these explicit decls.
void calculateLighting();
BOOL calculateLightingSlice();
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

 BOOL* columnsToUpdate;
 BOOL* chunksToUpdate;
//static BOOL* chunksToUpdatefg;

static BOOL* chunksToUpdateImmediatley;

// B3 Stage 2. Handed to the mesh pool so it can put a chunk back on the dirty lists after it
// publishes a mesh that went stale under a worker (an edit landed after the snapshot was taken) or
// that rebuild2() refused. Lives here rather than in MeshPool.mm because chunksToUpdate /
// columnsToUpdate are this file's state.
static void mp_redirtyChunk(int idxn){
    if(!chunksToUpdate||!columnsToUpdate)return;
    if(idxn<0||idxn>=CHUNKS_PER_SIDE*CHUNKS_PER_SIDE*CHUNKS_PER_COLUMN)return;
    chunksToUpdate[idxn]=TRUE;
    columnsToUpdate[idxn/CHUNKS_PER_COLUMN]=TRUE;
}


block8* blockarray;
//static color8* shadowarray;
Vector8* lightarray;
//static map_t chunkMapc;
TerrainChunk** chunkTablec;
static BOOL secondPass;
static NSDate* start;
extern bool firstframe;




//front face
//back face
//left face
//right face
//bot face
//top face
BurnNode* burnList;
static TreeNode troot={};
static int do_reload=0;

/*-(void) startLoadingThread{
    [NSThread detachNewThreadSelector:@selector(chunkBuildingThread:) toTarget:self withObject:self];
}*/
void genChildren(TreeNode* node){

	//NSLog(@"%d %d %d    %d %d %d",node->bounds[0],node->bounds[1],node->bounds[2]
	//	  ,node->bounds[3],node->bounds[4],node->bounds[5]);
	
	node->hasChildren=TRUE;
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
		BOOL toosmall=FALSE;
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
			if(child->bounds[k]==child->bounds[k+3])toosmall=TRUE;
			
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

TreeNode* addToTree(TreeNode* node,int* bounds,NSNumber* data){
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
    newNode->dead=FALSE;
        [data retain];
		newNode->data=data;
		newNode->next=node->dataList;
		node->dataList=newNode;
        return node;
	
   
}
void freeTree(TreeNode* node){
	if(node==NULL)return;
	ListNode* n=node->dataList;
	while(n!=NULL){		
		[(NSNumber*)n->data release];
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
    if(!LOW_MEM_DEVICE)
    memset(lightarray,0,sizeof(Vector8)*T_SIZE*T_SIZE*T_HEIGHT);
}



// PVRShell functions

extern int g_offcx;
extern int g_offcz;
Terrain::Terrain(){
    g_offcx=T_SIZE*100;
    g_offcz=T_SIZE*100;
    start = [NSDate date];
    [start retain];
    tgen=new TerrainGenerator(this);
	
    
   
    
    
    
    
    liquids=new Liquids();
    portals=new Portal();
    fireworks=new Firework();
	 initTree(&troot);
	
	singleton=this;
	loaded=FALSE;
	world_name=NULL;
	do_reload=0;
	nburn=0;
    chunkTablec=NULL;
    
	
}
TreeNode* addToTree(TreeNode* node,int* bounds,NSNumber* data);

void Terrain::allocateMemory(){
    if(chunkTablec!=NULL)return;
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
    chunksToUpdate=(BOOL*)malloc(sizeof(BOOL)*CHUNKS_PER_SIDE*CHUNKS_PER_SIDE*CHUNKS_PER_COLUMN);
    columnsToUpdate=(BOOL*)malloc(sizeof(BOOL)*CHUNKS_PER_SIDE*CHUNKS_PER_SIDE);
    chunksToUpdateImmediatley=(BOOL*)malloc(sizeof(BOOL)*CHUNKS_PER_SIDE*CHUNKS_PER_SIDE*CHUNKS_PER_COLUMN);
    memset(chunksToUpdateImmediatley,0,sizeof(BOOL)*CHUNKS_PER_SIDE*CHUNKS_PER_SIDE*CHUNKS_PER_COLUMN);
    blockarray=(block8*)malloc(sizeof(block8)*(T_SIZE+1)*(T_SIZE+1)*(T_HEIGHT+1));
    if(!LOW_MEM_DEVICE)
    lightarray=(Vector8*)malloc(sizeof(Vector8)*T_SIZE*T_SIZE*T_HEIGHT);
}

void Terrain::deallocateMemory(){
    // B3 Stage 2, invalidation rule 3: nothing may be in flight when the chunks it is reading are
    // deleted. Discard rather than publish -- these chunks are about to stop existing, and their
    // destructors free the vertex buffers the workers built.
    mp_drain(NULL,FALSE);
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
    if(!LOW_MEM_DEVICE)
free(lightarray);
    
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

	
	
	

void Terrain::unloadTerrain(BOOL exitToMenu){
    loaded=FALSE;
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
static BOOL update_lighting=FALSE;

// TRUE while prepareAndLoadGeometry is part-way through a bulk window reload (the count>140 path)
// and still owes the window some columns. See the long comment at the reload itself: the reload
// used to read+mesh every stale column inside one call, which measured a 104-124 ms main-thread
// block on a teleport; it now spends a per-frame budget and takes several frames instead.
// Reset on load so a world that was mid-reload when it got unloaded can't leave this latched.
static BOOL bulk_reload_active=FALSE;
// Set by the meshing pass when the per-frame budget stopped it short, i.e. that reload still owes
// the window geometry even if every column has been read.
static BOOL bulk_reload_meshing=FALSE;

void updateLightingBegin(){
    if(LOW_MEM_DEVICE)return;
    update_lighting=TRUE;
    memset(lightarray,0,sizeof(Vector8)*T_SIZE*T_SIZE*T_HEIGHT);
    calculateLightingSliceReset();   // restart the sliced sweep from column 0
}

void Terrain::loadTerrain(NSString* name,BOOL fromArchive){
    
    double start_time=-[start timeIntervalSinceNow];
	if(loaded)unloadTerrain(FALSE);
   
    World::getWorld->hud->goldencubes=10;
	counter=0;
	//skycolor=MakeVector(-1,-1,-1);
    
    Vector v=skycolor=MakeVector(1.0,1.0,1.0);
     extern Vector colorTable[256];
    if(v_equals(final_skycolor,colorTable[14]))
        v=MakeVector(0.5,0.72,0.9);
    float clr2[4]={v.x-.03f, v.y-.03f, v.z-.03f, 1.0f};
    glFogfv(GL_FOG_COLOR,clr2);
    
	burnList=NULL;
	nburn=0;
   
    
	world_name=name;
	[world_name retain];
    // B3 Stage 2, invalidation rule 1: loadWorld() calls readColumn() for the whole window, which
    // re-homes every chunk slot. Nothing may be meshing when that happens. (In practice a load is
    // always preceded by a teardown or a fresh world, so this drains nothing -- it is here so the
    // rule holds by construction rather than by luck.)
    mp_drain(mp_redirtyChunk,TRUE);
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
    
    firstframe=TRUE;
    //hashmap_iterate(chunkMap,extraGeneration,NULL);
	//startDynamics];
   
    void updateLightingBegin();
    updateLightingBegin();
    double end_time=-[start timeIntervalSinceNow];
    extern BOOL loaded_new_terrain;
    loaded_new_terrain=TRUE;
    
	loaded=1;
    // loadWorld just filled the whole window, so no bulk reload is outstanding -- and a latched
    // TRUE from the previous world would run a slice against a window this world never staged.
    bulk_reload_active=FALSE;
    bulk_reload_meshing=FALSE;
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
    World::getWorld->fm->saveWorld(pp);
	unloadTerrain(FALSE);
    
	loadTerrain(world_name,FALSE);
    //[World::getWorld->player reset];
    
    World::getWorld->player->groundPlayer();
}
void Terrain::warpToHome(){
	Vector pp;
	pp.x=(home.x+.5f);
	pp.z=(home.z+.5f);
	pp.y=(home.y+1);
	//World::getWorld->player.pos=pp;
	World::getWorld->fm->saveWorld(pp);
	unloadTerrain(FALSE);
    
	loadTerrain(world_name,FALSE);
    //[World::getWorld->player reset];

    World::getWorld->player->groundPlayer();
}
void Terrain::addToUpdateList(int cx,int cy,int cz){
     //issue #1 continued
    chunksToUpdate[threeToOne(cx,cy,cz)]=TRUE;
    columnsToUpdate[getColIndex(cx,cz)]=TRUE;
    }

void Terrain::addToUpdateList2(int cx,int cy,int cz){
     //issue #1 continued
    chunksToUpdate[threeToOne(cx,cy,cz)]=TRUE;
    columnsToUpdate[getColIndex(cx,cz)]=TRUE;
}

int compare_front2back (const void *a, const void *b);
int compare_rebuild_order (const void *a, const void *b);
int compare_back2front (const void *a, const void *b);

 int idxrl=0;
TerrainChunk* rebuildList[13000];
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

void Terrain::addChunk(TerrainChunk* chunk, int cx,int cy,int cz,BOOL rebuild){
	
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
void Terrain::setLand(int x,int z,int y,int type,BOOL chunkToo){
	
	if(y<0||y>=T_HEIGHT)return;
   
    GBLOCK(x,z,y)=type;

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
        chunk->modified=TRUE;
        // B3 Stage 2, invalidation rule 2: if a worker is meshing this chunk it is meshing a
        // snapshot taken before this edit. That is SAFE -- it cannot tear -- but the mesh it
        // produces is one edit out of date, so flag it and let the publish step re-dirty.
        tc_noteChunkWritten(chunk);
        
		
	}
	
}
BOOL Terrain::setColor(int x,int z,int y, color8 color){
    if(y<0||y>=T_HEIGHT)return FALSE;
    
   
    int cx=x/CHUNK_SIZE;
    int cy=y/CHUNK_SIZE;
    int cz=z/CHUNK_SIZE;
    
    TerrainChunk* chunk;
     chunk=chunkTable[threeToOne(cx,cy,cz)];
    //hashmap_get(chunkMap, threeToOne(cx,cy,cz),(any_t)&chunk);
    if(!chunk){
        return FALSE;
        
    }
    
    x-=cx*CHUNK_SIZE;
    y-=cy*CHUNK_SIZE;
    z-=cz*CHUNK_SIZE;
    color8 c1=chunk->pcolors[x*(CHUNK_SIZE*CHUNK_SIZE)+z*(CHUNK_SIZE)+y];
    if(c1==color) return FALSE;
    chunk->modified=TRUE;
    chunk->pcolors[x*(CHUNK_SIZE*CHUNK_SIZE)+z*(CHUNK_SIZE)+y]=color;
    tc_noteChunkWritten(chunk);   // B3 Stage 2, invalidation rule 2 (see Terrain::setLand)
		
	// NSLog(@"hi! %f,%f,%f",color.x,color.y,color.z);
    return TRUE;

    
    
}
/*- (void)destroyCustom:(int)x :(int)z :(int)y{
    [World::getWorld->effects addBlockBreak:x/2.0f :z/2.0f :y/2.0f :getCustomc(x ,z ,y) :0];
	updateCustom: x: z: y: TYPE_NONE :0];
    

}*/
void Terrain::destroyBlock(int x,int z,int y){
    NSLog(@"%d, %d, %d",x,z,y);
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
            return TRUE;
        }
        n=n->next;
    }
    
    return FALSE;
}
void Terrain::burnBlock(int x,int z,int y,BOOL causedByExplosion){
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
            NSLog(@"type 1 yo");
        
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
            portals->paintPortal(x,z,y,color);
            pos[1]--;
            cx=pos[0]/CHUNK_SIZE;
            cy=pos[1]/CHUNK_SIZE;
            cz=pos[2]/CHUNK_SIZE;
            addToUpdateList2(cx,cy,cz);
        }else{
            
            setColor(x,z,y+1,color);
            portals->paintPortal(x,z,y+1,color);
            
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
	
    if(type==TYPE_NONE)
        setColor(x,z,y,0);
    
    setLand(x,z,y,type,TRUE);
	
    
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
    if(coord==0)
        shadow+=(float)lightarray[((x+g_offcx)%T_SIZE)*T_SIZE*T_HEIGHT+((z+g_offcz)%T_SIZE)*T_HEIGHT+y].x/64.0f;
    else if(coord==1)
        shadow+=(float)lightarray[((x+g_offcx)%T_SIZE)*T_SIZE*T_HEIGHT+((z+g_offcz)%T_SIZE)*T_HEIGHT+y].y/64.0f;
    else if(coord==2)
        shadow+=(float)lightarray[((x+g_offcx)%T_SIZE)*T_SIZE*T_HEIGHT+((z+g_offcz)%T_SIZE)*T_HEIGHT+y].z/64.0f;
    
    
    
    if(shadow<0)shadow=0;
    if(shadow>1.5f)shadow=1.5f;
    return shadow;
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
    
    printf("define hit: %d, %d\n",(int)sizeof(BOOL), (int)sizeof(bool));

    
    
    
    
    
    MMM::ExplodeModels(v,color);
  
    if(color==0){
         World::getWorld->effects->addCreatureVanish(x+.5f,z+.5f,y+.5f,-1,blockTntMap[btype]);
    }else
    World::getWorld->effects->addCreatureVanish(x+.5f,z+.5f,y+.5f,color,blockTntMap[btype]);
    
    BOOL painting=false;
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
                        burnBlock(j ,k ,y-yy ,TRUE);
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
                        burnBlock(j ,k ,y+yy ,TRUE);
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
                        burnBlock(j ,k ,y-yy ,TRUE);
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
                        burnBlock(j ,k ,y+yy ,TRUE);
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
                int yy=er-i;
                
                //  int ox=j-x;
                //  int oz=k-z;
                //  int oy=yy;
                
                if(y-yy>0&&yy<=boundbot){
                    int type=getLandc(j, k, y-yy);
                    if(type>0&&blockinfo[type]&IS_BLOCKTNT){
                        burnBlock(j ,k ,y-yy ,TRUE);
                    }
                }
                if(y+yy<T_HEIGHT&&yy<=boundtop){
                    int type=getLandc(j, k, y+yy);
                    
                    if(type>0&&blockinfo[type]&IS_BLOCKTNT){
                        burnBlock(j ,k ,y+yy ,TRUE);
                    }
                    
                }
                
            }
        }
        
    }
}

void Terrain::explode(int x,int z,int y){
    
	
    int color=getColor(x,z,y);
    if(color!=0)
         Resources::getResources->playSound(S_GOOP_EXPLODE);
    else
        Resources::getResources->playSound(S_EXPLODE);
    
    Vector v=MakeVector(x+.5f,y+.5f,z+.5f);
    
    
    MMM::ExplodeModels(v,color);
    World::getWorld->effects->addCreatureVanish(x+.5f,z+.5f,y+.5f,color,TYPE_TNT);
    
    BOOL painting=false;
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
                            burnBlock(j ,k ,y-yy ,TRUE);
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
                        
						burnBlock(j ,k ,y+yy ,TRUE);
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
		World::getWorld->hud->sb->setStatus(@"Loading ",999);
       
            
		
	}
}
Vector gcrot={0};
Vector portal_rot={0};
const float BURN_SPREAD_TIME=1.0f;
int chunk_load_count=0;
BOOL doingsomeloading=FALSE;
static float blending_alpha;
static BOOL blending=false;
float last_etime;

BOOL Terrain::update(float etime){
    last_etime=etime;
    if(do_reload==-1){
        int pct=99*chunk_load_count/(2304/4)+counter/2;
        
        if(pct>100)pct=100;
        
            World::getWorld->hud->sb->setStatus([NSString stringWithFormat:@"Loading World  %d%%",pct],20);
        
        
        return FALSE;
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
			burnBlock(node->x+1 ,node->z ,node->y ,FALSE);
			burnBlock(node->x-1 ,node->z ,node->y ,FALSE);
			burnBlock(node->x ,node->z+1 ,node->y ,FALSE);
			burnBlock(node->x ,node->z-1 ,node->y ,FALSE);
			burnBlock(node->x ,node->z ,node->y+1 ,FALSE);
			burnBlock(node->x ,node->z ,node->y-1 ,FALSE);

		}
		
		if(nburn>300){			
			endDynamics(FALSE);
            
			break;
		}
		node->time-=etime;
		int tz=getLandc(node->x ,node->z ,node->y);
		if(node->time<=0||tz==TYPE_NONE){
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
			if(tz!=TYPE_NONE)
			updateChunks(node->x ,node->z ,node->y ,TYPE_NONE);
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
    extern Vector colorTable[256];
    if(interpolatev(&skycolor,final_skycolor,.25f,etime)){
         
        Vector v=skycolor;
        if(v_equals(final_skycolor,colorTable[14]))
        v=MakeVector(0.5,0.72,0.9);
        float clr[4]={v.x-.03f, v.y-.03f, v.z-.03f, 1.0f};
        
        glFogfv(GL_FOG_COLOR,clr);
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
		unloadTerrain(FALSE);
		//oldChunkMap=chunkMap;	
		//chunkMapc=chunkMap=hashmap_new();
       
        loadTerrain(world_name,FALSE);
        //hashmap_iterate(oldChunkMap,freeOldChunks,NULL);
		//iterate oldchunkmap and release chunks that arent reused
		//hashmap_remove_all(oldChunkMap, FALSE);
		
		do_reload=3;
        printf("test1123\n");
				
		
		return FALSE;
	}else if(do_reload==1){
        do_reload++;
    }else
	reloadIfNeeded();
	
	return FALSE;
}

void Terrain::endDynamics(BOOL endLiquids){
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
//
// RE-TUNED, AND DELIBERATELY LEFT AT 96, after B3 Stage 2 (2026-08-27). Stage 2 turned this from a
// mesh budget into a DISPATCH budget -- the meshing it gates now happens on a worker -- so what it
// still governs on the main thread is the column read/decode, the half of the burst that has not
// moved yet (Stage 3). Swept against build-relthr, 2 runs each, 5 teleport bursts each:
//
//   budget | worst main-thread block | frames >16.66ms | window fill time
//      96  |            15 / 15 ms   |       0 / 0     |   ~265 ms
//     144  |            11 / 30 ms   |       0 / 1     |   ~183 ms
//     192  |            24 / 19 ms   |       1 / 1     |   ~135 ms
//
// Raising it fills the window visibly faster and it is tempting; it also puts frames back over the
// 60 fps budget, because what it buys more of per frame is now DECODE. At 96 the threaded build
// misses the budget zero times across five bursts, which is exactly what B3 was for -- the window
// filling in at 60 fps rather than 45 (WORKING/b3-off-thread-meshing-plan.md §1). Trading that away
// for a shorter fill would undo the reason for the change. Revisit at Stage 3, when decode moves off
// the main thread too and this stops governing main-thread work at all; 144 is the candidate then.

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

// Is toroidal chunk-table column (x,z) holding the column the window wants this frame? Only asked
// while a bulk reload is part-way through, where the answer can be no.
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
// TerrainChunk's constructor left the whole rt* geometry block uninitialised, so render()'s
// `rtn_vertices==0` guard read garbage and the index memcpy walked off allIndices. See the
// constructor in TerrainChunk.mm. The narrow test only hid it by guaranteeing every drawn chunk
// had been meshed at least once.
//
// Keep that in mind before assuming a similar deferral is safe: what makes THIS one safe is that
// a deferred chunk's geometry still belongs to its own pbounds, and the chunk is now empty rather
// than garbage until it is meshed. A worker-thread mesher produces the same "resident but not yet
// meshed" state by construction and depends on the same initialisation.
static bool columnMeshableDuringReload(int x,int z,int ox,int oz,
                                       const bool isloaded[T_RADIUS*2][T_RADIUS*2]){
    int wx=((x-ox)%CHUNKS_PER_SIDE+CHUNKS_PER_SIDE)%CHUNKS_PER_SIDE;
    int wz=((z-oz)%CHUNKS_PER_SIDE+CHUNKS_PER_SIDE)%CHUNKS_PER_SIDE;
    if(!isloaded[wx][wz])return false;
    // The four laterals, folded the same way -- rebuild2() reads one block across each side face,
    // so a column whose neighbour is still stale would have to be meshed again when it lands.
    for(int d=0;d<4;d++){
        static const int dx[4]={-1,1,0,0};
        static const int dz[4]={0,0,-1,1};
        int nx=((wx+dx[d])%CHUNKS_PER_SIDE+CHUNKS_PER_SIDE)%CHUNKS_PER_SIDE;
        int nz=((wz+dz[d])%CHUNKS_PER_SIDE+CHUNKS_PER_SIDE)%CHUNKS_PER_SIDE;
        if(!isloaded[nx][nz])return false;
    }
    return true;
}

void Terrain::prepareAndLoadGeometry(){
    time1=time2=-[start timeIntervalSinceNow];
    World* world=World::getWorld;
    Player* player=world->player;

    // B3 Stage 2. Publish anything the workers finished since the last frame BEFORE deciding what
    // to read or mesh this frame: a published chunk is IDLE again, so it is free to be re-dirtied,
    // re-dispatched, or (the rule that matters) re-homed by a column read. updateAllImportantChunks
    // publishes a second time at the end of the frame to catch jobs that landed during it -- the
    // plan doc's §4.2 names that one; this one exists because World::update calls the two back to
    // back, so publishing only there would keep every dispatched chunk busy for a whole extra frame.
    mp_publishFinished(mp_redirtyChunk);
    mp_beginFrame();
    
    
    
    int m_chunkOffsetX=0;
    int m_chunkOffsetZ=0;
    // Hoisted out of the load block below because the meshing pass reads it too: mid-bulk-reload
    // it is the record of which columns of the window are resident RIGHT NOW, which is what tells
    // the mesher which chunks are worth meshing yet (see BULK_RELOAD_CHUNK_BUDGET).
    bool isloaded[T_RADIUS*2][T_RADIUS*2];

    ///////////load geom from file or gen
    if(loaded){
        m_chunkOffsetX=player->pos.x/CHUNK_SIZE-T_RADIUS;
       m_chunkOffsetZ=player->pos.z/CHUNK_SIZE-T_RADIUS;

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
        if(count>140||bulk_reload_active) {
            if(!bulk_reload_active){
                hit_load_counter++;
                if(hit_load_counter==1){
                    World::getWorld->hud->sb->setStatus(@"Loading",999);
                    if(count>300){
                        hit_load_counter++;
                    }

                }
                if(hit_load_counter>=2){
                    hit_load_counter=0;
                    World::getWorld->hud->sb->clear();
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
                    update_lighting=FALSE;

                    bulk_reload_active=TRUE;
                }
            }
            if(bulk_reload_active){
              //  printf("chunks to load:%d\n",count);
                // Nearest-first. Budgeting means some columns wait a few frames, and the ones the
                // player is standing in must not be the ones that wait -- collision reads
                // blockarray directly, so a far-away column can be late but the player's own
                // column cannot.
                static StaleColumn stale[T_RADIUS*2*T_RADIUS*2];
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
                        bulk_reload_active=FALSE;
                        addMoreCreaturesIfNeeded();
                        if(!LOW_MEM_DEVICE)update_lighting=TRUE;
                        extern BOOL loaded_new_terrain;
                        loaded_new_terrain=TRUE;
                    }
                }else{
                    NSString* file_name=[NSString stringWithFormat:@"%@/%@",world->fm->documents,world_name];

                    //[sf_lock lock];
                    // Reopened per slice on purpose: holding a handle across frames would outlive
                    // the rename an autosave does under it (FileManager::saveWorld writes a
                    // .savetmp and renames), leaving the slice reading a file nobody points at.
                    NSFileHandle* saveFile=[NSFileHandle fileHandleForReadingAtPath:file_name];

                    qsort(stale,nstale,sizeof(StaleColumn),compare_stale_column);

                    int budget=BULK_RELOAD_CHUNK_BUDGET/CHUNKS_PER_COLUMN;
                    if(budget<1)budget=1;
                    if(budget>nstale)budget=nstale;
                    // B3 Stage 2, invalidation rule 1 (plan doc §4.3): readColumn re-homes every
                    // chunk of the column -- setBounds(), and a wholesale rewrite of pblocks/
                    // pcolors -- so a column with a mesh job in flight must not be read yet. Skip
                    // it and take the next-nearest stale column instead; the job finishes in a
                    // frame or two and the column is still in stale[] next frame. Structurally
                    // this is the same "wait for the state to be safe" the neighbour test below
                    // does, and it cannot livelock: workers always finish.
                    for(int i=0,taken=0;i<nstale&&taken<budget;i++){
                        if(mp_columnBusy(stale[i].cx,stale[i].cz))continue;
                        //removeLights
                        // B3 Stage 3: readColumnDeferred may hand the column's RLE decode to a
                        // worker and answer FALSE, meaning "not landed yet". Then it must NOT be
                        // marked loaded -- isloaded is what gates meshing (columnMeshableDuringReload)
                        // and what decides the reload is finished, and both answers have to stay
                        // "no" until the decode publishes. The column is still in stale[] next
                        // frame; mp_columnBusy above is what stops it being read a second time.
                        if(world->fm->readColumnDeferred( stale[i].cx,stale[i].cz,saveFile))
                            isloaded[stale[i].cx-m_chunkOffsetX][stale[i].cz-m_chunkOffsetZ]=TRUE;
                        //addlights
                        // Counted whether it landed or was dispatched: the budget limits how much
                        // work one frame STARTS, which is what it has to mean once the work is
                        // asynchronous.
                        taken++;
                    }

                    [saveFile closeFile];
                    //void calculateLighting();
                }
            }
            time2=-[start timeIntervalSinceNow];
            
        }else{
            
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
   
    
    
    
    //////////////////build geom
    if(loaded){
        int num=0;
        // B1: this used to be int list[2000], filled with no bounds check, against a worst case of
        // CHUNKS_PER_SIDE^2*CHUNKS_PER_COLUMN -- 1296 at 64z (already 65% full, one bulk-dirty
        // event from overrunning) and 5184 at 256z. Sized to that worst case now, and the fill is
        // guarded: dropping a dirty chunk costs one stale frame, overrunning the stack costs the tab.
        static int list[CHUNKS_PER_SIDE*CHUNKS_PER_SIDE*CHUNKS_PER_COLUMN_MAX];
        const int list_max=CHUNKS_PER_SIDE*CHUNKS_PER_SIDE*CHUNKS_PER_COLUMN;
        // While a bulk reload is in flight the same per-frame budget that limits its column reads
        // limits its meshing, using the deferral list_max already had. Outside a reload this is
        // list_max, i.e. the pass drains everything exactly as it always did -- an edit, an
        // explosion and the initial world load are all unbudgeted.
        const int frame_max=bulk_reload_active&&BULK_RELOAD_CHUNK_BUDGET<list_max
                            ?BULK_RELOAD_CHUNK_BUDGET:list_max;

        idxrl=0;
        bulk_reload_meshing=FALSE;

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
                        if(bulk_reload_active&&
                           !columnMeshableDuringReload(x,z,m_chunkOffsetX,m_chunkOffsetZ,isloaded))
                            continue;
                        // B3 Stage 2: set if any chunk of this column had to be left dirty because
                        // a worker still owns it, so the column flag survives to the next frame.
                        BOOL column_deferred=FALSE;
                        for(int y=0;y<CHUNKS_PER_COLUMN;y++){
                            if(chunksToUpdate[threeToOne(x,y,z)]){

                                int n=threeToOne(x,y,z);

                                if(n>=CHUNKS_PER_SIDE*CHUNKS_PER_SIDE*CHUNKS_PER_COLUMN||n<0){
                                    printg("out of bounds index: %d\n",n);
                                }
                                // A chunk a worker is meshing must not be meshed again underneath
                                // it, and must not be re-dispatched. Leave its dirty bit set --
                                // same shape as the frame_max deferral below -- and let the next
                                // frame, by which time the publish step has put it back to IDLE,
                                // pick it up.
                                if(mp_chunkBusy(chunkTable[n])){
                                    column_deferred=TRUE;
                                    continue;
                                }
                                if(num>=frame_max){
                                    // Leave chunksToUpdate/columnsToUpdate set for what didn't fit,
                                    // so the next frame picks it up instead of losing it forever.
                                    if(num>=list_max){
                                        printg("dirty-chunk list full at %d, deferring the rest\n",num);
                                    }else{
                                        bulk_reload_meshing=TRUE;
                                    }
                                    goto list_full;
                                }
                                list[num++]=n;

                                chunksToUpdate[threeToOne(x,y,z)]=FALSE;
                            }

                        }
                        if(!column_deferred)
                            columnsToUpdate[getColIndex(x,z)]=FALSE;
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
            
                   // B3 Stage 2. Deliberate scope limit (plan doc §4.3): only the bulk-reload /
                   // streaming path goes to a worker. Player edits, explosions, fire and the
                   // initial world load keep meshing inline -- they are few, latency-sensitive and
                   // touch data right next to the player, so sending them off-thread widens the
                   // invalidation surface for no measured gain (steady state is 0.065 ms/chunk and
                   // is not felt). Revisit only with a number. mp_dispatch answering FALSE -- no
                   // threads, no free slot, or something burning in this chunk -- falls straight
                   // through to the unmodified inline path below.
                   if(bulk_reload_active&&mp_dispatch(rebuildList[i],rebuildList[i]->idxn))
                       continue;
              
                   if(rebuildList[i]->rebuild2()==-1){
                    //    chunksToUpdate[rebuildList[i].idxn]=TRUE;
                     //   columnsToUpdate[rebuildList[i].idxn/CHUNKS_PER_COLUMN]=TRUE;
                       printg("fail update on chunk: %d    bounds %d %d %d   rebuildCounter: %d\n",i,rebuildList[i]->pbounds[0],rebuildList[i]->pbounds[1],rebuildList[i]->pbounds[2],rebuildList[i]->rebuildCounter);
                    }else{
                        
                      //  rebuildList[i]->needsRebuild=FALSE;
                        if(LOW_MEM_DEVICE)rebuildList[i]->prepareVBO();
                        else
                        //issue #2 chunksToUpdateImmediatley shared data access with main thread, not synchronized
                        chunksToUpdateImmediatley[rebuildList[i]->idxn]=TRUE;
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
        if(calculateLightingSlice())
            update_lighting=FALSE;
        // hit_load_counter=0;
    }

    
    time3=-[start timeIntervalSinceNow];
    
    
    
}
void Terrain::updateAllImportantChunks(){
	double start_time=-[start timeIntervalSinceNow];

    // B3 Stage 2's publish point (plan doc §4.2): a worker fills the non-rt fields, the main
    // thread copies them into rt* and uploads. This is where the pool's output joins the frame,
    // alongside the inline mesher's own chunksToUpdateImmediatley backlog below.
    mp_publishFinished(mp_redirtyChunk);
    
    
   
   
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
                        
                        chunk->prepareVBO();
                        count++;
                        
                    }
                    
                    chunksToUpdateImmediatley[threeToOne(x,y,z)]=FALSE;
                }
                
            }
            //     columnsToUpdate[getColIndex(x,z)]=FALSE;
        }
    }


    		
	
  
    if(count>0){
        
        double end_time=-[start timeIntervalSinceNow];
        float etime=end_time-start_time;
        etime+=.0001f;
        
        time4=-[start timeIntervalSinceNow];
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
   
    
}
void Terrain::colort(float r,float g,float b){
	glColor4f(r,g,b,1);
}
static TerrainChunk* renderList[(T_SIZE/CHUNK_SIZE)*(T_SIZE/CHUNK_SIZE)*CHUNKS_PER_COLUMN_MAX];
static TerrainChunk* renderList2[(T_SIZE/CHUNK_SIZE)*(T_SIZE/CHUNK_SIZE)*CHUNKS_PER_COLUMN_MAX];
void renderTree(TreeNode* node,int state){
    //once upon a time this descended an oct-tree, profiling showed it was useless, now just iterates through chunk list
    for(int i=0;i<CHUNKS_PER_SIDE*CHUNKS_PER_SIDE*CHUNKS_PER_COLUMN;i++){
        TerrainChunk* chunk=chunkTablec[i];
        chunk->in_view=FALSE;
        if(secondPass){
        if(chunk->rtn_vertices2==0){
            continue;
        }
        }else if(chunk->rtn_vertices==0&&chunk->rtnum_objects==0){
            continue;
        }
        int istate=ViewTestAABB(chunk->rbounds,state);
       /* if(node==&troot){
            istate=VT_INSIDE;
        
        }*/
        if(istate&VT_OUTSIDE) continue;
        chunk->in_view=TRUE;
        
        if(secondPass){
                                renderList2[chunks_rendered2]=chunk;
                                //[chunk render2];
                                chunks_rendered2+=1;
            
        }else{
        
                                renderList[chunks_rendered]=chunk;
                                // [chunk render];
                                
                                chunks_rendered+=1;
        }
    }
		
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
int compare_back2front (const void *a, const void *b)
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
extern BOOL SUPPORTS_OGL2;
extern float SCREEN_HEIGHT;
static int frame_counter=0;
static int frame=0;


static BOOL last_skycolor_was_defaultblue=FALSE;

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
// (web/src/shim/gl/gl_es1_shim.cpp, eden_gl_setup_attributes) had to copy the span into a streaming
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
//  - objBatchDraw() must be called with GL_ARRAY_BUFFER at the engine's own binding (0 at every
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
    GLuint buffer;             // GL name, 0 until the first draw
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

// Upload `verts` staged vertices, point the arrays at them, draw, restore the array binding.
// `withNormals` mirrors whether the caller has GL_NORMAL_ARRAY enabled (doors and golden cubes do;
// the portal and flower passes disable it) -- specifying a pointer for a disabled array would be
// harmless but misleading.
static void objBatchDraw(ObjectBatch* b,int verts,BOOL withNormals){
    if(verts<=0||b->staging==NULL)return;
    if(b->buffer==0){
        glGenBuffers(1,&b->buffer);
        if(b->buffer==0)return;
        b->gpuCapacity=0;
    }
    glBindBuffer(GL_ARRAY_BUFFER,b->buffer);
    if(verts>b->gpuCapacity){
        // Powers of two so a scene drifting up in object count reallocates a handful of times per
        // session rather than every frame. GL_DYNAMIC_DRAW, not the GL_STATIC_DRAW that
        // TerrainChunk::prepareVBO uses -- this whole span is rewritten every single frame.
        int cap=b->gpuCapacity?b->gpuCapacity:1024;
        while(cap<verts)cap*=2;
        glBufferData(GL_ARRAY_BUFFER,(GLsizeiptr)cap*sizeof(vertexObject),NULL,GL_DYNAMIC_DRAW);
        b->gpuCapacity=cap;
    }
    glBufferSubData(GL_ARRAY_BUFFER,0,(GLsizeiptr)verts*sizeof(vertexObject),b->staging);
    glVertexPointer(3,GL_FLOAT,sizeof(vertexObject),(const void*)offsetof(vertexObject,position));
    if(withNormals)
        glNormalPointer(GL_FLOAT,sizeof(vertexObject),(const void*)offsetof(vertexObject,normal));
    glTexCoordPointer(2,GL_FLOAT,sizeof(vertexObject),(const void*)offsetof(vertexObject,texs));
    glColorPointer(4,GL_UNSIGNED_BYTE,sizeof(vertexObject),(const void*)offsetof(vertexObject,colors));
    glDrawArrays(GL_TRIANGLES,0,verts);
    glBindBuffer(GL_ARRAY_BUFFER,0);
}
// -------------------------------------------------------------------------------------------------

void Terrain::render(){
    if(do_reload==-1)return;
   //  NSLog(@"rendering!!");
    Graphics::beginTerrain();
	
	vertices_rendered=0;
	faces_rendered=0;
	chunks_rendered=chunks_rendered2=0;
    
    glMatrixMode(GL_TEXTURE);
    glScalef(1,1.0f/32.0f,1);
   // glPushMatrix();
    glMatrixMode(GL_MODELVIEW);
	glPushMatrix();
	//float pushx=World::getWorld->fm.chunkOffsetX*CHUNK_SIZE*BLOCK_SIZE;
	//float pushz=World::getWorld->fm.chunkOffsetZ*CHUNK_SIZE*BLOCK_SIZE;
	//glTranslatef(-pushx, 0, -pushz);
   // printg("-------------start renderTree------------\n");
	renderTree(&troot,0);
   //printg("--------x--x--x----end----x--x--x--------\n");
    
   // qsort (renderList, chunks_rendered, sizeof (TerrainChunk*), compare_front2back);
  //  glDisable(GL_TEXTURE_2D);
    int chunksr=chunks_rendered;
    for(int i=0;i<chunks_rendered;i++){
      
        vertices_rendered+=renderList[i]->render();
        /*if(vertices_rendered>=max_vertices&&World::getWorld->hud.fps<40){
            if(World::getWorld->hud.mode!=MODE_CAMERA){
            [Graphics setZFAR:P_ZFAR-.5f];
            chunks_rendered-=(chunks_rendered-i+1);
            break;
            }
        }*/
    }
#define max_render_objects 300
    glEnable(GL_LIGHTING);
    
    glShadeModel(GL_SMOOTH);
    extern Vector colorTable[256];
    BOOL isNight=v_equals(final_skycolor,colorTable[54]);
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
    
    glEnable(GL_LIGHT0);
    glPushMatrix();
    glLoadIdentity();
    glLightfv(GL_LIGHT0, GL_POSITION, lightPosition);
    glLightfv(GL_LIGHT1, GL_POSITION, lightPosition2);
    glPopMatrix();
    glLightfv(GL_LIGHT0, GL_AMBIENT,  lightAmbient);
    glLightfv(GL_LIGHT0, GL_DIFFUSE,  lightDiffuse);
    glLightfv(GL_LIGHT1, GL_AMBIENT,  lightAmbient2);
    glLightfv(GL_LIGHT1, GL_DIFFUSE,  lightDiffuse2);
    
    
    glEnableClientState(GL_NORMAL_ARRAY);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glEnable(GL_NORMALIZE);
    extern const GLshort cubeShortVertices[36*3];
    extern const GLshort cubeTexture[36*2];
    extern const GLfloat cubeNormals[36*3];
    const GLshort* cubeVertices=cubeShortVertices;
    const GLshort* cubeTextureCustom=cubeTexture;
    StaticObject* doorso[500];
    int num_doors=0;
    for(int i=0;i<chunksr;i++){
        for(int j=0;j<renderList[i]->rtnum_objects;j++){
            if(renderList[i]->rtobjects[j].type!=TYPE_DOOR_TOP)continue;
            if(num_doors<500)doorso[num_doors++]=&renderList[i]->rtobjects[j];
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
        /*if(renderList[i].rtobjects[j].ani==0){
         renderList[i].rtobjects[j].ani=1;
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
                    objVertices[vert].texs[1]=cubeTextureCustom[k*2+1]*32;
                }
                else{
                    objVertices[vert].texs[0]=1-cubeTextureCustom[k*2+0];
                    objVertices[vert].texs[1]=cubeTextureCustom[k*2+1]*32;
                    
                    
                }
            }
            extern Vector colorTable[256];
            Vector color=colorTable[door->color];
            if(TRUE||door->color==0){
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
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,0);
    glBindBuffer(GL_ARRAY_BUFFER,0);

    glBindTexture(GL_TEXTURE_2D, Resources::getResources->getDoorTex(clr));
        objBatchDraw(&g_doorBatch,vert,TRUE);
        }
    }
    
    //float lightDiffuse[4]  = {0.7f, 0.7f, 0.7f, 1.0f};
    glLightfv(GL_LIGHT0, GL_DIFFUSE,  lightDiffuse);
    
    cubeVertices=cubeShortVertices;
   cubeTextureCustom=cubeTexture;
    vert=0;
    object=0;
    lolc++;
    
    int tess=2;

    float tessf=1.0f/(float)tess;
    // A cube emits 6 faces x tess^2 subquads x 6 vertices; at tess=2 that is 144 per object, which
    // is why max_render_objects of them never fitted the old max_render_objects*6*6 array.
    objVertices=objBatchStage(&g_goldenBatch,max_render_objects*6*tess*tess*6);
    for(int i=0;objVertices&&i<chunksr;i++){
        for(int j=0;j<renderList[i]->rtnum_objects;j++){
            if(renderList[i]->rtobjects[j].type!=TYPE_GOLDEN_CUBE)continue;
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
                               
                            }else if(f==4||f==5){
                               vc=MakeVector(cubeVertices[k*3]*tessf+yoff-.5f,cubeVertices[k*3+1]-.5f,cubeVertices[k*3+2]*tessf+xoff-.5f);
                            }
                            vc=rotateVertice(gcrot,vc);
                            vc.x+=.5f;
                            vc.y+=.5f;
                            vc.z+=.5f;
                            Vector vn=MakeVector(cubeNormals[k*3],cubeNormals[k*3+1],cubeNormals[k*3+2]);
                            
                            vn=rotateVertice(gcrot,vn);
                            objVertices[vert].normal[0]=vn.x;
                            objVertices[vert].normal[1]=vn.y;
                            objVertices[vert].normal[2]=vn.z;
                            
                            
                            objVertices[vert].position[0]=4*(renderList[i]->rtobjects[j].pos.x-World::getWorld->fm->chunkOffsetX*CHUNK_SIZE)+1+2.3*vc.x;
                            objVertices[vert].position[1]=4*renderList[i]->rtobjects[j].pos.y+1+2.3*vc.y;
                            objVertices[vert].position[2]=4*(renderList[i]->rtobjects[j].pos.z-World::getWorld->fm->chunkOffsetZ*CHUNK_SIZE)+1+2.3*vc.z;
                            
                            //objVertices[vert].texs[0]=xoff;
                            //objVertices[vert].texs[1]=yoff;
                          
                            CalcEnvMap(&objVertices[vert]);
                            
                            
                           
                            
                            
                            extern Vector colorTable[256];
                            Vector color=colorTable[renderList[i]->rtobjects[j].color];
                            if(renderList[i]->rtobjects[j].color==0){
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
    
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,0);
    glBindBuffer(GL_ARRAY_BUFFER,0);
    float coloursp[4] ={1.0f, 1.0f, 1.0f, 1.0f};
    
    float colourspm[4] ={1.0f, 1.0f, 1.0f, 1.0f};
   
    glEnable(GL_LIGHT1);
    glDisable(GL_LIGHT0);
    glMaterialfv(GL_FRONT_AND_BACK,GL_SPECULAR,colourspm) ;
       glMaterialf(GL_FRONT_AND_BACK, GL_SHININESS, 100.0f);
    glLightfv(GL_LIGHT1,GL_SPECULAR,coloursp);
                    
   // glDisable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, Resources::getResources->getTex(ICO_SPHEREMAP)->name);
    objBatchDraw(&g_goldenBatch,vert,TRUE);
        float coloursp2[4] = {0,0,0,0};
    glMaterialfv(GL_FRONT_AND_BACK,GL_SPECULAR,coloursp2) ;
    glLightfv(GL_LIGHT0,GL_SPECULAR,coloursp2);
   // glEnable(GL_TEXTURE_2D);
    glDisable(GL_LIGHTING);
    glDisable(GL_NORMALIZE);
    glShadeModel(GL_FLAT);
     glDisable(GL_LIGHT1);
    
    glDisable(GL_LIGHT0);
    
    glDisableClientState(GL_NORMAL_ARRAY);
    
    cubeVertices=cubeShortVertices;
     cubeTextureCustom=cubeTexture;
    vert=0;
   object=0;
    objVertices=objBatchStage(&g_portalBatch,max_render_objects*6*6);
    for(int i=0;objVertices&&i<chunksr;i++){
        for(int j=0;j<renderList[i]->rtnum_objects;j++){
            if(renderList[i]->rtobjects[j].type!=TYPE_PORTAL_TOP)continue;
           // printg("drawing portal\n");
            
            float rot=0;
            int dir=(renderList[i]->rtobjects[j].dir+3)%4;
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
                objVertices[vert].position[0]=4*(renderList[i]->rtobjects[j].pos.x-World::getWorld->fm->chunkOffsetX*CHUNK_SIZE)+vc.x;
                objVertices[vert].position[1]=4*renderList[i]->rtobjects[j].pos.y+vc.y;
                objVertices[vert].position[2]=4*(renderList[i]->rtobjects[j].pos.z-World::getWorld->fm->chunkOffsetZ*CHUNK_SIZE)+vc.z;
                
               
                if(k>=12){
                    objVertices[vert].texs[0]=cubeTextureCustom[k*2+0];     
                    objVertices[vert].texs[1]=0;
                }else{
                    if(k>=6){
                        objVertices[vert].texs[0]=cubeTextureCustom[k*2+0];
                        objVertices[vert].texs[1]=cubeTextureCustom[k*2+1]*32;
                    }
                    else{
                        objVertices[vert].texs[0]=1-cubeTextureCustom[k*2+0];
                        objVertices[vert].texs[1]=cubeTextureCustom[k*2+1]*32;
                        
                        
                    }
                }
                extern Vector colorTable[256];
                Vector color=colorTable[renderList[i]->rtobjects[j].color];
                if(true||renderList[i]->rtobjects[j].color==0){
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
    
  
    
    glBindTexture(GL_TEXTURE_2D, Resources::getResources->getTex(ICO_PORTAL)->name);
    objBatchDraw(&g_portalBatch,vert,FALSE);
    
    
    
    
    ///////////sky
    glDisable(GL_FOG);
	glDisableClientState(GL_COLOR_ARRAY);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
    glMatrixMode(GL_PROJECTION);
	glPushMatrix();
	glLoadIdentity();
/*	if(World::getWorld->FLIPPED)
		glRotatef(90,0,0,1);
	else
		glRotatef(270,0,0,1);*/
	
    
    if(IS_IPAD){
        if(IS_RETINA)
            glOrthof(0, SCREEN_WIDTH*2, 0, SCREEN_HEIGHT*2, -1, P_ZFAR);
        else
            glOrthof(0, IPAD_WIDTH, 0, IPAD_HEIGHT, -1, P_ZFAR);
	}else
        glOrthof(0, SCREEN_WIDTH, 0, SCREEN_HEIGHT, -1, P_ZFAR);
	glMatrixMode(GL_MODELVIEW);
	glPushMatrix();
    glLoadIdentity();
	
    //skycolor.x=0;
    extern Vector colorTable[256];
    if(v_equals(final_skycolor,colorTable[14])){
        last_skycolor_was_defaultblue=TRUE;
        
        glColor4f(1.0, 1.0, 1.0, 1.0);
        
        Resources::getResources->getTex(ICO_SKY_BOX)->drawSky(CGRectMake(0,0, SCREEN_WIDTH,SCREEN_HEIGHT) ,-P_ZFAR/1.000001);
        if(  blending_alpha>0&&
           (blending||
            !v_equals(final_skycolor,skycolor)  )
           ){
            if(!blending){
                blending=TRUE;
                blending_alpha=1.0f;
            }
            glEnable(GL_BLEND);
            Vector v=skycolor;
          //  Vector v2=World::getWorld->terrain.final_skycolor;
            
           
            
           // float alpha=1.0;
            
            glColor4f(v.x, v.y, v.z, blending_alpha);
            
            Resources::getResources->getTex(ICO_SKY_BOX_BW)->drawSky(CGRectMake(0,0, SCREEN_WIDTH,SCREEN_HEIGHT) ,-P_ZFAR/1.000004);
            
            
            glDisable(GL_BLEND);
           // blending_alpha-=.04f;
            if(blending_alpha<0){
                
                blending=FALSE;
            }
        }else{
            if(v_equals(final_skycolor,skycolor)){
            blending_alpha=1.0f;
            blending=FALSE;
            }
        }
    }else{
        if( last_skycolor_was_defaultblue){
            blending=TRUE;
            blending_alpha=0.0f;
            last_skycolor_was_defaultblue=FALSE;
        }
        //    extern Vector colorTable[256];
        
        if(blending){
            glColor4f(1.0, 1.0, 1.0, 1.0);
            
            Resources::getResources->getTex(ICO_SKY_BOX)->drawSky(CGRectMake(0,0, SCREEN_WIDTH,SCREEN_HEIGHT),-P_ZFAR/1.000001);
            glEnable(GL_BLEND);
            Vector v=skycolor;
            glColor4f(v.x, v.y, v.z, blending_alpha);
           // blending_alpha+=.04f;
            if(blending_alpha>1.0f){
                blending=FALSE;
            }
            Resources::getResources->getTex(ICO_SKY_BOX_BW)->drawSky(CGRectMake(0,0, SCREEN_WIDTH,SCREEN_HEIGHT),-P_ZFAR/1.000004);
            glColor4f(1.0, 1.0, 1.0, 1.0);
            glDisable(GL_BLEND);
        }else{
        Vector v=skycolor;
        glColor4f(v.x, v.y, v.z, 1.0);
        
        Resources::getResources->getTex(ICO_SKY_BOX_BW)->drawSky(CGRectMake(0,0, SCREEN_WIDTH,SCREEN_HEIGHT),-P_ZFAR/1.000001);
        glColor4f(1.0, 1.0, 1.0, 1.0);
        }
    }
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    
    
    glMatrixMode(GL_MODELVIEW);

    ////////////////sky
    
    glEnableClientState(GL_COLOR_ARRAY);
    
    
        
    
    StaticObject* portalso[200];
    int num_portals=0;
    for(int i=0;i<chunksr;i++){
        for(int j=0;j<renderList[i]->rtnum_objects;j++){
            if(renderList[i]->rtobjects[j].type!=TYPE_PORTAL_TOP)continue;
            if(num_portals<200)portalso[num_portals++]=&renderList[i]->rtobjects[j];
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
                objVertices[vert].texs[1]=(vc.y+.5f)*32;
                        
                
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
        glBindTexture(GL_TEXTURE_2D, Resources::getResources->getTex(ICO_SWIRL)->name);
        objBatchDraw(&g_swirlBatch,vert,FALSE);
        }

   // }
    
       


    
      
        glDisable(GL_BLEND);
    // glEnable(GL_TEXTURE_2D);
    /*if(World::getWorld->hud.fps<55){
        if(World::getWorld->hud.mode!=MODE_CAMERA){
            [Graphics setZFAR:P_ZFAR-.4f];
            
            
        }
    }*/
	glPopMatrix();
    
    glMatrixMode(GL_TEXTURE);
    glScalef(1,32.0f,1);
    
    glMatrixMode(GL_MODELVIEW);

    //if(!SUPPORTS_OGL2)
       	frame_counter++;
   
    
    firstframe=FALSE;
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
    glEnableClientState(GL_COLOR_ARRAY);
	
     glEnable(GL_FOG);
	glPushMatrix();
	glScalef(.25f,.25f,.25f);
    
    
    
    glMatrixMode(GL_TEXTURE);
    glScalef(1,1.0f/32.0f,1);
   
    // glPushMatrix();
    
    glMatrixMode(GL_MODELVIEW);
    glEnableClientState(GL_COLOR_ARRAY);
    // if(!SUPPORTS_OGL2)
  //  glEnable(GL_FOG);
    
    
    glEnable(GL_BLEND);
    glPushMatrix();
    
    glBindTexture(GL_TEXTURE_2D, Resources::getResources->atlas2->name);
    glMatrixMode(GL_TEXTURE);
    
    frame=(frame+1)%128;
  
    glTranslatef(0,(int)(frame/16),0);
    glMatrixMode(GL_MODELVIEW);
    
	//float pushx=World::getWorld->fm.chunkOffsetX*CHUNK_SIZE*BLOCK_SIZE;
	//float pushz=World::getWorld->fm.chunkOffsetZ*CHUNK_SIZE*BLOCK_SIZE;
	//glTranslatef(-pushx, 0, -pushz);
    secondPass=TRUE;
	renderTree(&troot,0);
    qsort (renderList2, chunks_rendered2, sizeof (TerrainChunk*), compare_back2front);
    
    for(int i=0;i<chunks_rendered2;i++){
        renderList2[i]->render2();
    }
    glMatrixMode(GL_TEXTURE);
    glTranslatef(0,-(int)(frame/16),0);
    glScalef(1,32.0f,1);
    
    glMatrixMode(GL_MODELVIEW);
    
    extern const GLshort cubeShortVertices[36*3];
    extern const GLshort cubeTexture[36*2];
    extern const GLfloat cubeNormals[36*3];
    const GLshort* cubeVertices=cubeShortVertices;
    const GLshort* cubeTextureCustom=cubeTexture;
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
        for(int j=0;j<renderList[i]->rtnum_objects;j++){
            if(renderList[i]->rtobjects[j].type!=TYPE_FLOWER)continue;
            if(flowers>=MAX_FLOWERS)break;
            flowerList[flowers]=renderList[i]->rtobjects[j];
            flowers++;
        }
    }
    
    qsort (flowerList, flowers, sizeof (StaticObject), compare_objects_back2front);
    extern Vector colorTable[256];
    BOOL isNight=v_equals(final_skycolor,colorTable[54]);
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
            light[0]=calcLight(flowerList[i].pos.x,flowerList[i].pos.z,flowerList[i].pos.y,skylight,0);
            light[1]=calcLight(flowerList[i].pos.x,flowerList[i].pos.z,flowerList[i].pos.y,skylight,1);
            light[2]=calcLight(flowerList[i].pos.x,flowerList[i].pos.z,flowerList[i].pos.y,skylight,2);
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
    
    
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,0);
    glBindBuffer(GL_ARRAY_BUFFER,0);
    
    glBindTexture(GL_TEXTURE_2D, Resources::getResources->getTex(ICO_FLOWER)->name);
    glEnable(GL_BLEND);


    //glDepthMask(GL_FALSE);
    glDepthMask(GL_TRUE);
    objBatchDraw(&g_flowerBatch,vert,FALSE);

    
   
    secondPass=FALSE;
    
    
    
    
    
	glPopMatrix();
    Graphics::endTerrain();
    
    
}
Terrain::~Terrain(){
	if(loaded)
        unloadTerrain(FALSE);
		
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
