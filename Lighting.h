//
//  Lighting.h
//  Eden
//
//  Created by Ari Ronen on 1/21/13.
//
//
#ifndef Eden_Lighting_h
#define Eden_Lighting_h


#include "Vector.h"
#include "Constants.h"



void calculateLighting();
bool calculateLightingSlice();   // budgeted per-frame form for the post-bulk-reload path
void calculateLightingSliceReset();
void addlight(int xx,int zz,int yy,float brightness,Vector color);

// LM.1: the lamp light profile. Stock Eden's lamps are a linear sphere of radius LIGHT_RADIUS (5);
// "New Dawn" is the same formula, bytes and clamp at a wider radius (LIGHT_RADIUS_NEWDAWN, fitted
// from screenshots). Only the radius differs, so every LIGHT_RADIUS use reads light_radius() now.
//
// The radius is capped at LIGHT_RADIUS_MAX (17): a lamp's last non-zero voxel is ceil(R)-1 blocks
// out, and light_rebuildNeighbourhood's 3x3-cleared / 5x5-re-lit rebuild is exact only while that
// stays inside the lamp's own column's neighbours (ceil(R)-1 <= CHUNK_SIZE). 18 breaks the ring
// walk (--light-mutate=r18 proves it). Main thread only, like every light write.
enum { LIGHT_MODE_LEGACY=0, LIGHT_MODE_NEWDAWN=1 };
#define LIGHT_RADIUS_NEWDAWN 16.0f
#define LIGHT_RADIUS_MAX 17.0f
float light_radius();             // the current profile's radius (Legacy: exactly LIGHT_RADIUS)
int light_mode();
// The radius to give refreshChunksInRadius: stock's LIGHT_RADIUS for Legacy, else ceil(R) but at
// most CHUNK_SIZE (its 27 probe points are +-r per axis, so r must not skip a neighbour chunk).
int light_refreshRadius();
// Switch profile. With a world loaded: a fresh light sweep (updateLightingBegin) and a re-mesh of
// the whole window, because going New Dawn -> Legacy, chunks 5-16 blocks from a lamp would keep
// meshes baked from light that no longer exists. No-op if nothing changes.
void light_setProfile(int mode);
// New Dawn's radius (LM.4 tunes it in 12-17; native --lamp-radius=). Clamped to [1, LIGHT_RADIUS_MAX]
// (to 18 under --light-mutate=r18). Takes effect at once if New Dawn is current.
void light_setNewDawnRadius(float r);

// LM.2: the world's choice, stored as `.emod` record 14 LIGHTING (docs/emod-file-format.md): 8 bytes,
// u16 version (1), u8 mode (0 Legacy, 1 New Dawn; any other value draws as Legacy and is kept verbatim),
// u8 origin (0 auto-detected, 1 the player chose), u8 detector revision, 3 reserved zero bytes.
enum { LIGHTING_RECORD_BYTES=8, LIGHTING_RECORD_VERSION=1, LIGHTING_DETECTOR_REV=1 };
struct LightingRecord { unsigned char mode, origin, detector; };
// False for the wrong length or an unknown version (the world then has no usable record: Legacy).
bool light_recordDecode(const unsigned char* p,unsigned long n,LightingRecord* out);
void light_recordEncode(const LightingRecord& r,unsigned char out[LIGHTING_RECORD_BYTES]);
// The profile a record's mode byte draws: New Dawn for 1, Legacy for everything else.
int light_recordProfile(unsigned char mode);
// A loaded world's profile (FileManager::finishLoadOnMain, World Properties' live switch): light_setProfile
// unless the run forced one (--lamp-light=, which every test world has to obey over its own record).
void light_setWorldProfile(int mode);
// LM.3: the auto-detect rule (docs/lighting-liquids-effects.md), for a world with no usable LIGHTING record.
// New Dawn only when the toggle is on and the world has 16 bands, a PROVENANCE record (provFlags = its flags
// word) and was not promoted from 64z (flag 256 clear). Pure: every input is plain data.
enum { LIGHT_PROV_PROMOTED=256 };
int light_detectMode(bool autoOn,int bands,bool hasProvenance,unsigned provFlags);
void light_forceProfile(int mode);   // the harness flag: sets the profile and makes light_setWorldProfile a no-op; -1 releases

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

// B.5b-1c (ring streaming): per-column light. Called once a frame before meshing, while no full
// sweep is pending: every column slot whose occupant changed since its light was last computed (a
// ring landing, a decode published by a worker) gets its 3x3 neighbourhood of slots re-lit from
// the 5x5 around it, so walking keeps the window's light exactly what a fresh load's sweep gives
// (Lighting.mm, at light_rebuildNeighbourhood). Returns the neighbourhoods rebuilt.
int light_syncLandedColumns();
extern int g_light_rebuilds;        // diagnostics: neighbourhoods rebuilt since launch,
extern double g_light_rebuild_ms,g_light_rebuild_max_ms;   // their total and worst wall time
// --ring-light-gate's bug-put-back switch (native --light-mutate=): 1 = never rebuild, 2 = rebuild
// but do not re-mesh what changed, 3 = let light wrap past the window edge (stock), 4 = never
// forget a "no lightbox here" slot on a landing or a lightbox edit (P.10), 5 = (LM.1) let New Dawn's
// radius reach 18, one past LIGHT_RADIUS_MAX. 0 = off.
extern int g_light_test_mutation;
// P.10: a column slot found to hold no lightbox is not scanned again until one of these (or an
// occupant change, or a full sweep / store clear) says it might hold one now. addChunk takes world
// chunk coordinates; setLand world block coordinates, after the GBLOCK write.
void light_markLanded(int cx,int cz);              // Terrain::addChunk
void light_onSetLand(int x,int z,int type);         // Terrain::setLand
extern int g_light_cols_scanned,g_light_cols_skipped;   // diagnostics: sweepLightingColumn calls
bool light_columnKnownFree(int cx,int cz);          // diagnostics: world chunk column remembered as free

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