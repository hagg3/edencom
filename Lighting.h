//
//  Lighting.h
//  Eden
//
//  Created by Ari Ronen on 1/21/13.
//
//
#ifndef Eden_Lighting_h
#define Eden_Lighting_h


#import "Vector.h"
#import "Constants.h"



void calculateLighting();
BOOL calculateLightingSlice();   // budgeted per-frame form for the post-bulk-reload path
void calculateLightingSliceReset();
void addlight(int xx,int zz,int yy,float brightness,Vector color);

// Stage R / R.3: the light store. Stock Eden kept one dense Vector8 per voxel of the window
// (`lightarray`, 288*288*T_HEIGHT*3 bytes: 15.9 MB at 64z, 63.7 MB at 256z) and memset all of it on
// every load, warp and bulk reload, though only voxels within LIGHT_RADIUS of a TYPE_LIGHTBOX are
// ever non-zero. It is now one 12 KB brick per toroidal chunk slot, allocated the first time
// addlight() writes a non-zero value into it; a slot with no brick has no light.
//
// Indexed exactly as the dense array was: by the TOROIDAL voxel index ((x+g_offcx)%T_SIZE,
// (z+g_offcz)%T_SIZE, y), so a brick slot is a chunkTable slot and the values are the dense
// array's, byte for byte (eden_debug_light_state hashes them the same way for both).
//
// Bricks are NEVER freed during play. MeshPool's workers call calcLight() off the main thread with
// no lock (TerrainChunk.h note 2), which was safe for the dense array because it never moved. So
// light_storeClear() zeroes each brick and moves it to a pool that addlight() reuses, and only
// light_storeFree() (Terrain::deallocateMemory, after mp_drain) frees memory. A worker holding a
// pointer it loaded before a clear reads zeros or another slot's light: a stale value, the same
// tolerated class as the dense array's unlocked memset, never a dangling pointer. Peak memory is
// the most bricks lit at once, bounded by the dense size.
extern Vector8** lightbricks;   // CHUNKS_PER_SIDE*CHUNKS_PER_SIDE*CHUNKS_PER_COLUMN slots

static inline int light_brickSlot(int tx,int tz,int y){
    return ((tx/CHUNK_SIZE)*CHUNKS_PER_SIDE+(tz/CHUNK_SIZE))*CHUNKS_PER_COLUMN+y/CHUNK_SIZE;
}
static inline int light_brickIndex(int tx,int tz,int y){
    return (tx%CHUNK_SIZE)*CHUNK_SIZE2+(tz%CHUNK_SIZE)*CHUNK_SIZE+y%CHUNK_SIZE;
}
// The light at a toroidal voxel, or NULL for none (all zero). y must be in [0,T_HEIGHT).
static inline const Vector8* light_get(int tx,int tz,int y){
    if(!lightbricks)return NULL;
    const Vector8* b=__atomic_load_n(&lightbricks[light_brickSlot(tx,tz,y)],__ATOMIC_ACQUIRE);
    return b?&b[light_brickIndex(tx,tz,y)]:NULL;
}

void light_storeAllocate();   // Terrain::allocateMemory
void light_storeFree();       // Terrain::deallocateMemory, after mp_drain
void light_storeClear();      // what memset(lightarray,0,...) used to be
unsigned long long light_storeBytes();
int light_storeBricks();      // bricks in the table (lit slots), not counting the pool
// Diagnostics only (eden_debug_set_light_selfcheck, before a world loads): also keep the old dense
// array, written by the old code in parallel, and compare. Voxels that disagree.
extern bool g_light_selfcheck;
long long light_selfcheckMismatches();



#endif