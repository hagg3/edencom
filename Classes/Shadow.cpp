//
//  Shadow.mm
//  Eden
//
//  Stage H / H.2: baked top-face shadows (Shadow.h). An engine file added AFTER the 2.1.1 import:
//  it is listed in port/tools/engine-sources.txt, not in port/tools/engine-sources.txt.
//
#include "Shadow.h"
#include "Terrain.h"
#include "Util.h"
#include "../Lighting.h"   // light_get: the brick store (the root header, not Classes/)
#include <stdlib.h>
#include <string.h>

int g_shadow_mode=SHADOW_OFF;
unsigned char* shadowTop=NULL;
unsigned char g_shadow_caster[256];
int g_shadow_test_mutation=0;
int g_shadow_dirtied=0;

extern block8* blockarray;
extern int g_offcx,g_offcz;
extern bool* chunksToUpdate;
extern bool* columnsToUpdate;
extern TerrainChunk** chunkTablec;

static int g_shadow_setting=SHADOW_OFF;
static int g_shadow_override=-1;
// One byte per chunk-column slot (getColIndex): its voxels landed since its tops were scanned.
static bool g_shadow_stale[CHUNKS_PER_SIDE_MAX*CHUNKS_PER_SIDE_MAX];

// Casters: everything but air, cloud (as in 1.x), liquids at every level (a flowing surface would
// move the top on every tick, and a seabed under shallow water should stay lit), glass, weave and
// the IS_OBJECT blocks (doors, portals, flowers, golden cube). A new block type gets its status
// from its flags; the change-impact table in docs/conventions-and-pitfalls.md says so.
static void shadow_buildCasterTable(){
    static bool built=false;
    if(built)return;
    built=true;
    memset(g_shadow_caster,0,sizeof(g_shadow_caster));
    for(int t=1;t<=NUM_BLOCKS;t++){
        if(t==TYPE_CLOUD||t==TYPE_GLASS||t==TYPE_WEAVE)continue;
        if(blockinfo[t]&(IS_LIQUID|IS_OBJECT))continue;
        g_shadow_caster[t]=1;
    }
}

static inline int shadow_slot(int x,int z){
    return ((x+g_offcx)%T_SIZE)*T_SIZE+((z+g_offcz)%T_SIZE);
}

void shadow_allocate(){
    shadow_buildCasterTable();
    free(shadowTop);
    shadowTop=(unsigned char*)calloc((size_t)T_SIZE*T_SIZE,1);
    for(int i=0;i<CHUNKS_PER_SIDE_MAX*CHUNKS_PER_SIDE_MAX;i++)g_shadow_stale[i]=true;
}
void shadow_free(){
    free(shadowTop);
    shadowTop=NULL;
}
void shadow_clear(){
    if(shadowTop)memset(shadowTop,0,(size_t)T_SIZE*T_SIZE);
    shadow_markAllStale();
}
void shadow_markAllStale(){
    for(int i=0;i<CHUNKS_PER_SIDE_MAX*CHUNKS_PER_SIDE_MAX;i++)g_shadow_stale[i]=true;
}
void shadow_markLanded(int cx,int cz){
    if(g_shadow_test_mutation==3)return;
    g_shadow_stale[getColIndex(cx,cz)]=true;
}
int shadow_pendingColumns(){
    int n=0;
    for(int i=0;i<CHUNKS_PER_SIDE*CHUNKS_PER_SIDE;i++)if(g_shadow_stale[i])n++;
    return n;
}

bool shadow_slotPending(int colslot){
    return colslot>=0&&colslot<CHUNKS_PER_SIDE*CHUNKS_PER_SIDE&&g_shadow_stale[colslot];
}

// The topmost caster of one toroidal block column, scanning down from below `from`.
static inline int shadow_scan(int tx,int tz,int from){
    const block8* strip=blockarray+tx*g_xz_stride+tz*g_world_height;
    for(int y=from-1;y>=0;y--)
        if(g_shadow_caster[(unsigned char)strip[y]])return y;
    return 0;
}

int shadow_syncLandedColumns(){
    if(!shadowTop||!blockarray||!chunkTablec)return 0;
    int synced=0;
    for(int s=0;s<CHUNKS_PER_SIDE*CHUNKS_PER_SIDE;s++){
        if(!g_shadow_stale[s])continue;
        g_shadow_stale[s]=false;
        synced++;
        // getColIndex's slot s is (cx mod CPS)*CPS + (cz mod CPS): its block columns are the
        // toroidal 16x16 square at (s/CPS*16, s%CPS*16). Start above the highest occupied band.
        int band=CHUNKS_PER_COLUMN-1;
        while(band>=0){
            TerrainChunk* c=chunkTablec[s*CHUNKS_PER_COLUMN+band];
            if(!c||!c->typesEmpty)break;
            band--;
        }
        const int from=(band+1)*CHUNK_SIZE;
        const int tx0=(s/CHUNKS_PER_SIDE)*CHUNK_SIZE,tz0=(s%CHUNKS_PER_SIDE)*CHUNK_SIZE;
        for(int tx=tx0;tx<tx0+CHUNK_SIZE;tx++)
            for(int tz=tz0;tz<tz0+CHUNK_SIZE;tz++)
                shadowTop[tx*T_SIZE+tz]=(unsigned char)shadow_scan(tx,tz,from);
    }
    return synced;
}

static inline int floordiv(int a,int b){ return a>=0?a/b:-((-a+b-1)/b); }

// A block at (x,z,y) turning into or out of a caster can re-shade top faces in the 3x3 of columns
// round it with y in [y-REACH_END+1, y-1]. Dirty every chunk covering that.
static void shadow_dirtyCasterChange(int x,int z,int y){
    int y0=y-SHADOW_REACH_END+1,y1=y-1;
    if(y0<0)y0=0;
    if(y1>=T_HEIGHT)y1=T_HEIGHT-1;
    if(y1<y0)return;
    const int cx0=floordiv(x-1,CHUNK_SIZE),cx1=floordiv(x+1,CHUNK_SIZE);
    const int cz0=floordiv(z-1,CHUNK_SIZE),cz1=floordiv(z+1,CHUNK_SIZE);
    for(int cx=cx0;cx<=cx1;cx++)
        for(int cz=cz0;cz<=cz1;cz++)
            for(int cy=y0/CHUNK_SIZE;cy<=y1/CHUNK_SIZE;cy++){
                chunksToUpdate[threeToOne(cx,cy,cz)]=true;
                columnsToUpdate[getColIndex(cx,cz)]=true;
                g_shadow_dirtied++;
            }
}

void shadow_onSetLand(int x,int z,int y,int oldtype,int type){
    if(!shadowTop||g_shadow_test_mutation==1)return;
    const int slot=shadow_slot(x,z);
    const int old=shadowTop[slot];
    int now=old;
    if(g_shadow_caster[(unsigned char)type]){
        if(y>old)now=y;
    }else if(y==old){
        now=shadow_scan((x+g_offcx)%T_SIZE,(z+g_offcz)%T_SIZE,y);
    }
    shadowTop[slot]=(unsigned char)now;
    if(g_shadow_caster[(unsigned char)oldtype]!=g_shadow_caster[(unsigned char)type]&&
       g_shadow_mode!=SHADOW_OFF&&g_shadow_test_mutation!=2&&chunksToUpdate)
        shadow_dirtyCasterChange(x,z,y);
}

static void shadow_apply(){
    const int mode=g_shadow_override>=0?g_shadow_override:g_shadow_setting;
    if(mode==g_shadow_mode)return;
    g_shadow_mode=mode;
    // Every chunk re-meshes, the way --ring-light-selftest's reference does; the tops are current.
    if(chunksToUpdate&&columnsToUpdate&&chunkTablec){
        memset(chunksToUpdate,true,sizeof(bool)*CHUNKS_PER_SIDE*CHUNKS_PER_SIDE*CHUNKS_PER_COLUMN);
        memset(columnsToUpdate,true,sizeof(bool)*CHUNKS_PER_SIDE*CHUNKS_PER_SIDE);
    }
}
void shadow_setSetting(int mode){
    g_shadow_setting=mode<SHADOW_OFF?SHADOW_OFF:mode>SHADOW_SMOOTH?SHADOW_SMOOTH:mode;
    shadow_apply();
}
void shadow_setOverride(int mode){
    g_shadow_override=mode<0?-1:mode>SHADOW_SMOOTH?SHADOW_SMOOTH:mode;
    shadow_apply();
}

void shadow_gather(int x0,int z0,int y0,unsigned char* out){
    if(!blockarray){ memset(out,0,SHADOW_GATHER_BYTES); return; }
    // Casters up to REACH_END-1 above the chunk's top layer can still reach it.
    int ytop=y0+CHUNK_SIZE-1+SHADOW_REACH_END-1;
    if(ytop>=T_HEIGHT)ytop=T_HEIGHT-1;
    for(int dx=0;dx<SHADOW_GATHER;dx++){
        const int tx=(x0-1+dx+g_offcx)%T_SIZE;
        for(int dz=0;dz<SHADOW_GATHER;dz++){
            const int tz=(z0-1+dz+g_offcz)%T_SIZE;
            const block8* strip=blockarray+tx*g_xz_stride+tz*g_world_height;
            unsigned char* o=out+(dx*SHADOW_GATHER+dz)*CHUNK_SIZE;
            int nearest=-1;
            for(int y=ytop;y>=y0;y--){
                if(y<y0+CHUNK_SIZE){
                    const int g=nearest>=0?nearest-y:0;
                    o[y-y0]=(unsigned char)(g<SHADOW_REACH_END?g:0);
                }
                if(g_shadow_caster[(unsigned char)strip[y]])nearest=y;
            }
        }
    }
}

// calcLight's arithmetic (Terrain.mm) with the brick fetched once for all n shades: Smooth asks for
// 4 per face, and 12 out-of-line calls were most of its cost. `sky` is a separate statement before
// the add, as in calcLight, so no target can fuse the multiply into it.
void shadow_lightTops(int x,int z,int y,float skylight,const int* shades,int n,const float light[3],float (*out)[3]){
    const Vector8* lv=NULL;
    if(!LOW_MEM_DEVICE&&y>=0&&y<T_HEIGHT)lv=light_get((x+g_offcx)%T_SIZE,(z+g_offcz)%T_SIZE,y);
    float add[3]={0,0,0};
    if(lv){
        add[0]=(float)lv->x/64.0f;
        add[1]=(float)lv->y/64.0f;
        add[2]=(float)lv->z/64.0f;
    }
    for(int k=0;k<n;k++){
        if(shades[k]>=255){
            out[k][0]=light[0]; out[k][1]=light[1]; out[k][2]=light[2];
            continue;
        }
        const float s=shades[k]/255.0f;
        const float sky=skylight!=1.0f?skylight*s:s;
        for(int c=0;c<3;c++){
            float v=sky;
            if(lv)v+=add[c];
            if(v<0)v=0;
            if(v>1.5f)v=1.5f;
            if(skylight==1.0f&&v>1.0f)v=1.0f;
            out[k][c]=v;
        }
    }
}
void shadow_lightTop(int x,int z,int y,float skylight,int shade,const float light[3],float out[3]){
    shadow_lightTops(x,z,y,skylight,&shade,1,light,(float(*)[3])out);
}
