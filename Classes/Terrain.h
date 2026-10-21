//
//  Terrain.h
//  prototype
//
//  Created by Ari Ronen on 10/11/10.
//  Copyright 2010 __MyCompanyName__. All rights reserved.
//
#ifndef Eden_Terrain_h
#define Eden_Terrain_h



#import <OpenGLES/ES1/gl.h>
#import <OpenGLES/ES1/glext.h>
#import "Texture2D.h"
#import "Graphics.h"
#import "TerrainChunk.h"
#import "Player.h"
#import "Util.h"
#import "TerrainGenerator.h"
#import "hashmap.h"
#import "Globals.h"
#import "Liquids.h"
#import "Portal.h"
#import "Firework.h"
//#import "Constants.h"
class Player;
class TerrainChunk;
class TerrainGenerator;
class Portal;
class Firework;

// g_xz_stride is T_SIZE*T_HEIGHT precomputed (Constants.h): the world height is a runtime
// per-world value now, and this macro is the hottest thing in the engine -- keeping the
// x-stride as its own global costs one load instead of a multiply on every block access.
#define GBLOCKIDXCLEAN(x,z,y)  ((x+g_offcx)%T_SIZE)*g_xz_stride + ((z+g_offcz)%T_SIZE)*g_world_height + y
#define GBLOCKIDX(x,z,y) GBLOCKIDXCLEAN((x),(z),(y))
#define GBLOCK(x,z,y) blockarray[GBLOCKIDX(x,z,y)]
#define GBLOCK_SAFE(x,z,y) blockarray[(GBLOCKIDX(x,z,y)+T_BLOCKS)%T_BLOCKS]
#define GBLOCKR(x,z,y) GBLOCK((int)(x),(int)(z),(int)(y))

// B.5b-1a: the resident window's first chunk on one axis, for a player at block coordinate p.
// Stock was floor(p/16)-T_RADIUS, which spans [pc-9, pc+9): the +edge is only 128-144 blocks
// away. Centring on the nearest chunk CORNER instead puts it 136-152 blocks away on every side,
// so the meshed region (the window minus its data-only outer ring) reaches 120-136, past every
// fog end. EVERY site that picks the window must use this, or a load and the streamer disagree
// about which columns belong and one of them streams a whole row back
// (WORKING/render-distance-runtime-plan-2026-10-11.md §1.3).
static inline int windowOriginChunk(float p){
    return (int)floorf((p+CHUNK_SIZE*0.5f)/CHUNK_SIZE)-T_RADIUS;
}

class Terrain  {
public:
	TerrainGenerator* tgen;
	int n_chunks;
	BOOL loaded;
	Vector home;
	int level_seed;
	int nburn;
    Vector skycolor,final_skycolor;
    TerrainChunk **chunkTable;

	NSString* world_name;
    Liquids* liquids;
    Portal* portals;
    Firework* fireworks;
    int counter;
    
    Terrain();
    ~Terrain();
    void loadTerrain(NSString* name,BOOL fromArchive);
    BOOL update(float etime);
    void setLand(int x,int z,int y,int type, BOOL chunkToo);
    BOOL setColor(int x,int z,int y, color8 color);
    void buildBlock(int x,int z,int y);
    void paintBlock(int x,int z,int y, int color);
    void destroyBlock(int x,int z,int y);
    void burnBlock(int x,int z,int y, BOOL causedByExplosion);
    void updateChunks(int x,int z,int y,int type);
    // MP.3b: one cell to (type, color) with no refusals and no effects -- liquid source out/in,
    // lightbox light out/in, updateChunks (air drops signs/CMB1), the paint, the light refresh.
    // CmdScript's setCell (after its refusals) and MpWorld's remote apply share it. color<0 on air
    // leaves the cell's colour alone (updateChunks already zeroes it); on a block it means 0.
    void setCellState(int x,int z,int y,int type,int color);
    int getLand(int x,int z,int y);
    int getColor(int x,int z,int y);
    void refreshChunksInRadius(int x,int z,int y,int radius);
    void warpToPoint(float x,float z,float y);
    void blocktntexplode(int x,int z,int y,int type);
    void addToUpdateList(int cx,int cy,int cz);
    void addToUpdateList2(int cx,int cy,int cz);
    void updateAllImportantChunks();
    void prepareAndLoadGeometry();
    void addChunk(TerrainChunk* chunk,int cx,int cy,int cz, BOOL rebuild);
    void render();
    void render2();
    void unloadTerrain(BOOL exitToMenu);
    void warpToHome();
    void clearBlocks();
    void colort(float r,float g,float b);
    void endDynamics(BOOL endLiquids);
    // B.5b-1b ring streaming: chunk column (cx,cz) is leaving the window. Its burn nodes end the way
    // endDynamics ends them (the block becomes air) and its liquid nodes are dropped, so nothing
    // keyed by its absolute coordinates outlives it to simulate against the column that lands in
    // its slot. Returns the number of nodes dropped. dynamicsOutsideWindow() is the probe (must be 0).
    int dropDynamicsInColumn(int cx,int cz);
    int dynamicsOutsideWindow();
    // --evict-selftest's bug-put-back switches (gate 6): skip the evict-save / the node dropping.
    static bool testNoEvictSave, testNoDropDynamics;
    void startDynamics();
    void explodeBlock(int x,int z,int y);
    void allocateMemory();
    void deallocateMemory();
    void shootFirework(int x,int z,int y);
    
    
private:
    int tntSoundForBlast(int x,int z,int y);
    void explode(int x,int z,int y);
    void reloadIfNeeded();
};

float getShadow(int x,int z,int y);
float calcLight(int x,int z,int y,float shadow,int coord);
void calcLight3(int x,int z,int y,float shadow,float out[3]);   // Q.1 F26: one light fetch, three channels
 int getLandc(int x,int z,int y);
 int getLandc2(int x,int z,int y);
// B.5b-1b: does the window's slot for block column (x,z) hold that column right now?
bool isColumnResident(int x,int z);
int getColorc(int x,int z,int y);
bool isOnFire(int x ,int z, int y);
int getRampType(int x,int z,int y, int t);

//int getCustomc(int x,int z,int y);
/*
@property(nonatomic, assign) NSString* world_name;
@property(nonatomic, assign) Vector home,skycolor,final_skycolor;
@property(nonatomic, assign) BOOL loaded;
@property(nonatomic, assign) int level_seed,counter;

@property(nonatomic,readonly) TerrainChunk** chunkTable;

@property(nonatomic,readonly) TerrainGenerator* tgen;
@property(nonatomic,readonly) Portal* portals;
@property(nonatomic,readonly) Firework* fireworks;*/

#endif


