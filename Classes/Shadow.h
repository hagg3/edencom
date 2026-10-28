//
//  Shadow.h
//  Eden
//
//  Stage H / H.2 (2026-10-14): baked top-face shadows, the 1.x feature restored.
//  Plan: WORKING/feature-archaeology-2026-10-13/shadows-plan.md. Engine doc:
//  docs/lighting-liquids-effects.md "Baked shadows".
//
//  The shade of a top face comes from the NEAREST caster above it (within SHADOW_REACH_END), read
//  from blockarray when a chunk is meshed (shadow_gather), so it works under any ceiling, not just
//  the column's topmost block. shadowTop[] (the y of each column's topmost caster, 0 = none, by the
//  toroidal slot like GBLOCK) is now only a diagnostic the self-test checks; it is kept current
//  whether shadows are on or off (shadow_syncLandedColumns, shadow_onSetLand). Only the dirtying
//  of what a caster change re-shades, the mesher's 8-neighbour wait and the colours themselves
//  depend on g_shadow_mode, so Off meshes and schedules exactly like stock.
//
//  The shade multiplies only the SKY term of a top face's light: calcLight(x,z,y,skylight*s,c),
//  capped at 1.0 by day. It never touches top_shadow (rebuild2's ramp factor, which nothing reads:
//  reviving it would darken ramp tops with shadows Off).
//
#ifndef Eden_Shadow_h
#define Eden_Shadow_h

#include "Constants.h"
#include "Globals.h"

// A covering column counts fully when its NEAREST caster above the face is 1..SHADOW_REACH_FULL
// blocks up, fades linearly to nothing at SHADOW_REACH_END, and is ignored beyond that. Nearest, not
// topmost: a canopy, balcony or ceiling under a roof or inside a cave shades the floor below it. Deep
// caves keep today's brightness; the ground under a sky island is not shaded. Changing either
// re-pins CI's "geometry 64z shadows" `full`.
#define SHADOW_REACH_FULL 8
#define SHADOW_REACH_END 24
// A mesh reads the tops of its 16x16 columns and the ring of 1 around them.
#define SHADOW_GATHER (CHUNK_SIZE+2)
#define SHADOW_GATHER_N (SHADOW_GATHER*SHADOW_GATHER)
// The gather table: per column of the 18x18 (index (dx*SHADOW_GATHER+dz)*CHUNK_SIZE), one byte per
// chunk-local y = the gap up to the nearest caster above that block (1..SHADOW_REACH_END-1), 0 = none
// within reach.
#define SHADOW_GATHER_BYTES (SHADOW_GATHER_N*CHUNK_SIZE)

enum { SHADOW_OFF=0, SHADOW_CLASSIC=1, SHADOW_SMOOTH=2 };

extern int g_shadow_mode;                 // what the mesher uses: the setting, or the --shadows= override
extern unsigned char* shadowTop;          // T_SIZE*T_SIZE, NULL with no world
extern unsigned char g_shadow_caster[256];   // by (unsigned char)type: 1 = casts

// --shadow-mutate= (native, diagnostics): the bug put back. 1 nohook = setLand never updates the
// top (G1's behaviour); 2 nodirty = it updates but re-meshes nothing; 3 nolanding = addChunk never
// flags the column; 4 nodiag = the mesher waits on the 4 laterals only. 0 = off.
extern int g_shadow_test_mutation;
extern int g_shadow_dirtied;              // diagnostics: chunks the edit hook marked dirty

void shadow_allocate();                   // Terrain::allocateMemory, after the window is sized
void shadow_free();                       // Terrain::deallocateMemory
void shadow_clear();                      // Terrain::clearBlocks: every top 0, every column stale
void shadow_markLanded(int cx,int cz);    // Terrain::addChunk (world chunk coordinates)
void shadow_markAllStale();               // diagnostics: what a reload would cause
int  shadow_pendingColumns();
bool shadow_slotPending(int colslot);     // by getColIndex slot (diagnostics)
int  shadow_syncLandedColumns();          // prepareAndLoadGeometry, right before the mesh pass
void shadow_onSetLand(int x,int z,int y,int oldtype,int type);   // Terrain::setLand, after the GBLOCK write
// The setting (0/1/2) and the native --shadows= override (-1 = none). Changing the effective
// mode re-meshes the window; shadowTop is already current.
void shadow_setSetting(int mode);
void shadow_setOverride(int mode);
// Main thread: for the chunk whose min corner is block (x0,y0,z0), the gap table (above) of the 18x18
// block columns from (x0-1,z0-1), i.e. its own columns and the ring, read from blockarray.
void shadow_gather(int x0,int z0,int y0,unsigned char* out);

static inline int shadow_weight(int gap){
    if(gap<=0)return 0;
    if(gap<=SHADOW_REACH_FULL)return 16;
    if(gap<SHADOW_REACH_END)return SHADOW_REACH_END-gap;
    return 0;
}
// Classic, per face: the 3x3 of columns round local column (lx,lz), for the top face of the block
// at chunk-local y. 155..255; 255 = unshaded. Integer, so every target agrees. With every gap <= 8 this
// is exactly G1's 255-100*count/9.
static inline int shadow_classic(const unsigned char* gaps,int lx,int lz,int y){
    int S=0;
    for(int dx=0;dx<3;dx++)
        for(int dz=0;dz<3;dz++)
            S+=shadow_weight((int)gaps[((lx+dx)*SHADOW_GATHER+(lz+dz))*CHUNK_SIZE+y]);
    return 255-(100*S)/144;
}
// Smooth, per vertex: the 2x2 of columns meeting at corner (vx,vz) in {0,1} of local column
// (lx,lz). 155..255.
static inline int shadow_corner(const unsigned char* gaps,int lx,int lz,int y,int vx,int vz){
    int S=0;
    for(int dx=0;dx<2;dx++)
        for(int dz=0;dz<2;dz++)
            S+=shadow_weight((int)gaps[((lx+vx+dx)*SHADOW_GATHER+(lz+vz+dz))*CHUNK_SIZE+y]);
    return 255-(100*S)/64;
}
// Smooth for all 4 corners at once (out[vx*2+vz]), from the 9 weights the corners share: what
// shadow_corner gives each, at half the weight evaluations. Returns 1 if any corner is shaded.
static inline int shadow_corners(const unsigned char* gaps,int lx,int lz,int y,int out[4]){
    int w[3][3];
    for(int dx=0;dx<3;dx++)
        for(int dz=0;dz<3;dz++)
            w[dx][dz]=shadow_weight((int)gaps[((lx+dx)*SHADOW_GATHER+(lz+dz))*CHUNK_SIZE+y]);
    int any=0;
    for(int k=0;k<4;k++){
        const int vx=k>>1,vz=k&1;
        const int S=w[vx][vz]+w[vx+1][vz]+w[vx][vz+1]+w[vx+1][vz+1];
        out[k]=255-(100*S)/64;
        any|=S;
    }
    return any!=0;
}
// A shaded top face's light (block at world x,z,y). `light` is what the face gets unshaded
// (rebuild2's light[]). Night: the sky term is scaled; day: capped at 1.0, so a shaded face beside
// a lightbox never outshines an unshaded one. calcLight's arithmetic exactly, out of line (Shadow.mm)
// so `skylight*s` stays its own statement and cannot be fused into the add on any target.
void shadow_lightTop(int x,int z,int y,float skylight,int shade,const float light[3],float out[3]);
// The same for n shades of one face (Smooth's 4 corners), fetching the light once.
void shadow_lightTops(int x,int z,int y,float skylight,const int* shades,int n,const float light[3],float (*out)[3]);

#endif
