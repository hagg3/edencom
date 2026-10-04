//
//  Lighting.m
//  Eden
//
//  Created by Ari Ronen on 1/21/13.
//
//

#import "Lighting.h"
#import "Terrain.h"
// Stage R / R.3: no longer the light store (see Lighting.h). NULL unless g_light_selfcheck, when it
// is the stock dense array again, written by the stock code beside the bricks as a reference.
extern Vector8* lightarray;
extern block8* blockarray;

Vector8** lightbricks=NULL;
static Vector8** lightbrick_pool=NULL;   // zeroed bricks waiting for reuse; see Lighting.h
static int lightbrick_npool=0;
static int lightbrick_slots=0;
static int lightbrick_total=0;          // bricks malloc'd: in the table plus in the pool
bool g_light_selfcheck=false;

void light_storeAllocate(){
    lightbrick_slots=CHUNKS_PER_SIDE*CHUNKS_PER_SIDE*CHUNKS_PER_COLUMN;
    lightbricks=(Vector8**)calloc(lightbrick_slots,sizeof(Vector8*));
    lightbrick_pool=(Vector8**)malloc(sizeof(Vector8*)*lightbrick_slots);
    lightbrick_npool=0;
    lightbrick_total=0;
    lightarray=g_light_selfcheck?(Vector8*)calloc((size_t)T_SIZE*T_SIZE*T_HEIGHT,sizeof(Vector8)):NULL;
}
void light_storeFree(){
    for(int i=0;i<lightbrick_slots;i++)free(lightbricks[i]);
    for(int i=0;i<lightbrick_npool;i++)free(lightbrick_pool[i]);
    free(lightbricks);
    free(lightbrick_pool);
    lightbricks=lightbrick_pool=NULL;
    lightbrick_slots=lightbrick_npool=lightbrick_total=0;
    free(lightarray);
    lightarray=NULL;
}
// Proportional to the bricks in use (a few dozen per lightbox cluster), where the memset it
// replaces was the whole window: 16 MB at 64z, 64 MB at 256z, on every reload and warp.
void light_storeClear(){
    for(int i=0;i<lightbrick_slots;i++){
        Vector8* b=lightbricks[i];
        if(!b)continue;
        __atomic_store_n(&lightbricks[i],(Vector8*)NULL,__ATOMIC_RELEASE);
        memset(b,0,sizeof(Vector8)*CHUNK_SIZE3);
        lightbrick_pool[lightbrick_npool++]=b;
    }
    if(lightarray)memset(lightarray,0,sizeof(Vector8)*T_SIZE*T_SIZE*T_HEIGHT);
}
static Vector8* light_newBrick(int slot){
    Vector8* b;
    if(lightbrick_npool)b=lightbrick_pool[--lightbrick_npool];
    else{
        b=(Vector8*)calloc(CHUNK_SIZE3,sizeof(Vector8));
        if(!b)return NULL;   // that light is lost; the dense array could not fail here, but it
        lightbrick_total++;  // was one 16-64 MB malloc up front instead
    }
    __atomic_store_n(&lightbricks[slot],b,__ATOMIC_RELEASE);
    return b;
}
unsigned long long light_storeBytes(){
    return (unsigned long long)lightbrick_total*CHUNK_SIZE3*sizeof(Vector8)+
           (unsigned long long)lightbrick_slots*2*sizeof(Vector8*);
}
int light_storeBricks(){
    int n=0;
    for(int i=0;i<lightbrick_slots;i++)if(lightbricks[i])n++;
    return n;
}
long long light_selfcheckMismatches(){
    if(!lightarray)return -1;
    long long bad=0;
    for(int tx=0;tx<T_SIZE;tx++)
        for(int tz=0;tz<T_SIZE;tz++)
            for(int y=0;y<T_HEIGHT;y++){
                const Vector8 d=lightarray[tx*T_SIZE*T_HEIGHT+tz*T_HEIGHT+y];
                const Vector8* s=light_get(tx,tz,y);
                if(s?(s->x!=d.x||s->y!=d.y||s->z!=d.z):(d.x||d.y||d.z))bad++;
            }
    return bad;
}


extern int g_offcx;
extern int g_offcz;
void addlight(int xx,int zz,int yy,float brightness,Vector color){
    if(LOW_MEM_DEVICE)return;

 //   printf("light intensities: ");
    for(int x=-LIGHT_RADIUS;x<=LIGHT_RADIUS;x++){
        for(int z=-LIGHT_RADIUS;z<=LIGHT_RADIUS;z++){
            for(int y=-LIGHT_RADIUS;y<=LIGHT_RADIUS;y++){
                if(x*x+z*z+y*y>LIGHT_RADIUS*LIGHT_RADIUS)continue;
                if(y+yy<0||y+yy>=T_HEIGHT)continue;
                float inten=1.0f-sqrtf(x*x+z*z+y*y)/LIGHT_RADIUS;

                //if(xx+x<0||xx+x>=T_SIZE||zz+<0||z>=T_SIZE)return;
                const int tx=(xx+x+g_offcx)%T_SIZE,tz=(zz+z+g_offcz)%T_SIZE;
                if(lightarray){   // R.3 self-check: the stock dense write, untouched
                int lidx=tx*T_SIZE*T_HEIGHT+tz*T_HEIGHT+yy+y;
                lightarray[lidx].x=MAX(0,MIN(255,lightarray[lidx].x+64.0f*inten*brightness*color.x));
                lightarray[lidx].y=MAX(0,MIN(255,lightarray[lidx].y+64.0f*inten*brightness*color.y));
                lightarray[lidx].z=MAX(0,MIN(255,lightarray[lidx].z+64.0f*inten*brightness*color.z));
                }
                // R.3: the same arithmetic on the brick. A slot with no brick reads as zero, and
                // stays brickless unless this write leaves something non-zero -- so a lightbox
                // being broken (brightness -1, clamped at 0) never allocates.
                const int slot=light_brickSlot(tx,tz,yy+y);
                Vector8* b=lightbricks[slot];
                Vector8 cur;
                if(b)cur=b[light_brickIndex(tx,tz,yy+y)];
                else cur.x=cur.y=cur.z=0;
                Vector8 nv;
                nv.x=MAX(0,MIN(255,cur.x+64.0f*inten*brightness*color.x));
                nv.y=MAX(0,MIN(255,cur.y+64.0f*inten*brightness*color.y));
                nv.z=MAX(0,MIN(255,cur.z+64.0f*inten*brightness*color.z));
                if(!b){
                    if(!nv.x&&!nv.y&&!nv.z)continue;
                    if(!(b=light_newBrick(slot)))continue;
                }
                b[light_brickIndex(tx,tz,yy+y)]=nv;
            }
        }
    }
   // printf("\n");
}
extern Vector colorTable[256];
extern TerrainChunk** chunkTablec;

// One lightbox hit: splat its light and mark the chunks it touches for re-meshing. The one place
// the two sweep forms below (and any future one) turn a found TYPE_LIGHTBOX into work.
static void sweepLightingHit(int x,int z,int y){
    addlight(x,z,y,1.0f,colorTable[getColorc(x,z,y)]);
    World::getWorld->terrain->refreshChunksInRadius(x,z,y,LIGHT_RADIUS);
}

// One (cx,cz) column of the resident window: find its TYPE_LIGHTBOX blocks and splat each one's
// light. Shared by calculateLighting (whole window in one call, on world load) and
// calculateLightingSlice (a budgeted strip per frame, after a bulk window reload) so the two can
// never drift. Returns the number of lightboxes found.
//
// blockarray is y-fastest, so each (x,z) of the column is one contiguous T_HEIGHT-byte strip, and a
// memchr per strip replaces what used to be a getLandc per voxel (a toroidal-index multiply and
// two modulos each; ~21M of them per window at 256z). It reads exactly the bytes getLandc reads, so
// the same lightboxes are found. The hits arrive in (x,z,y) order instead of the old (cy,y,x,z)
// order; that cannot change lightarray (addlight only ever adds, saturating at 255, so the final
// byte is order-independent) nor the chunksToUpdate flags (a set).
//
// The strip form assumes the column is the regular one: every chunk present, stacked at
// cy*CHUNK_SIZE, all sharing the column's x/z bounds. If any chunk breaks that (a deferred or
// not-yet-published slot) the column takes the old per-chunk, per-voxel scan, which honours each
// chunk's own pbounds exactly as before.
static int sweepLightingColumn(int cx,int cz){
    int hits=0;
    TerrainChunk* first=chunkTablec[threeToOne(cx,0,cz)];
    bool regular=(first!=NULL);
    for(int cy=0;regular&&cy<CHUNKS_PER_COLUMN;cy++){
        TerrainChunk* chunk=chunkTablec[threeToOne(cx,cy,cz)];
        if(!chunk||chunk->pbounds[0]!=first->pbounds[0]||chunk->pbounds[2]!=first->pbounds[2]||
           chunk->pbounds[1]!=cy*CHUNK_SIZE)regular=false;
    }
    if(regular){
        const int ex=first->pbounds[0]+CHUNK_SIZE,ez=first->pbounds[2]+CHUNK_SIZE;
        for(int x=first->pbounds[0];x<ex;x++){
            for(int z=first->pbounds[2];z<ez;z++){
                const block8* strip=&GBLOCK(x,z,0);
                const block8* end=strip+T_HEIGHT;
                const block8* p=strip;
                while(p<end){
                    const block8* hit=(const block8*)memchr(p,TYPE_LIGHTBOX,end-p);
                    if(!hit)break;
                    sweepLightingHit(x,z,(int)(hit-strip));
                    hits++;
                    p=hit+1;
                }
            }
        }
        return hits;
    }
    for(int cy=0;cy<CHUNKS_PER_COLUMN;cy++){
        TerrainChunk* chunk=chunkTablec[threeToOne(cx,cy,cz)];
        if(!chunk)continue;
        for(int y=chunk->pbounds[1];y<CHUNK_SIZE+chunk->pbounds[1];y++){
            for(int x=chunk->pbounds[0];x<CHUNK_SIZE+chunk->pbounds[0];x++){
                for(int z=chunk->pbounds[2];z<CHUNK_SIZE+chunk->pbounds[2];z++){
                    if(getLandc(x,z,y)==TYPE_LIGHTBOX){
                        sweepLightingHit(x,z,y);
                        hits++;
                    }
                }
            }
        }
    }
    return hits;
}

void calculateLighting(){
    //printf("calculating lighting first load\n");
    if(LOW_MEM_DEVICE)return;
    
    extern TerrainChunk** chunkTablec;
   /* extern BOOL* chunksToUpdate;
    extern BOOL* columnsToUpdate;
    Vector8 fill;
    fill.x=128; fill.y=0; fill.z=0;
    memset(lightarray,128,sizeof(Vector8)*T_SIZE*T_SIZE*T_HEIGHT);
    for(int i=0;i<T_SIZE*T_SIZE*T_HEIGHT;i++){
        lightarray[i].x=fill.x;
        lightarray[i].z=fill.z;
        lightarray[i].y=fill.y;
    }
    memset(chunksToUpdate,TRUE,sizeof(BOOL)*CHUNKS_PER_SIDE*CHUNKS_PER_SIDE*CHUNKS_PER_COLUMN);
    memset(columnsToUpdate,TRUE,sizeof(BOOL)*CHUNKS_PER_SIDE*CHUNKS_PER_SIDE);
    for(int x=0;x<T_SIZE;x++){
        for(int z=0;z<T_SIZE;z++){
            int shadow=0;
            for(int y=T_HEIGHT-1;y>=0;y--){
                int lidx=((x+g_offcx)%T_SIZE)*T_SIZE*T_HEIGHT+((z+g_offcz)%T_SIZE)*T_HEIGHT+y;
                lightarray[lidx].x=lightarray[lidx].y=lightarray[lidx].z=128-shadow*15;
                if(getLandc(x,z,y)!=TYPE_NONE){
                    if((shadow+1)*15<128)
                    shadow++;
                }else{
                    shadow -=2;
                    if(shadow<0)shadow=0;
                }
                
            }
        }
    }*/
    
    
    for(int cx=0;cx<CHUNKS_PER_SIDE;cx++){
        for(int cz=0;cz<CHUNKS_PER_SIDE;cz++){
            sweepLightingColumn(cx,cz);
        }
    }
    
    
    //printf("calculating lighting first load end\n");
}

// Post-bulk-reload lighting (Terrain.mm's update_lighting path). calculateLighting above used to be
// an O(window volume) per-voxel scan for TYPE_LIGHTBOX -- ~5.3M getLandc calls at 64z, ~21M at
// 256z -- and was measured as a single unbudgeted ~20ms (64z) / ~80ms (256z) main-thread stall once
// per teleport/warp (tools/headless-mesh-burst-probe*.js, 2026-08-27: it, not the chunk mesh
// budget, is the 256z reload spike). It is now a memchr per (x,z) strip (see sweepLightingColumn),
// but the window is still walked a budgeted strip of columns per frame, holding a cursor between
// calls; a partly-swept window just has some lightboxes not yet contributing for a few frames --
// the same tolerated-stale state the reload budget itself relies on. Returns TRUE once the whole
// window has been swept; the caller clears update_lighting on that.
//
// The budget is counted in COLUMNS (it was chunks, 256 per frame, which at 256z meant 16 columns
// and 21 frames against 64z's 64 columns and 6). A column now costs 256 memchr calls over
// T_HEIGHT bytes, so the per-frame cost is 108 columns x 256 calls x T_HEIGHT bytes = ~28k calls
// and ~1.8 MB (64z) / ~7 MB (256z) of memchr, a couple of ms on the slowest device measured, and the
// whole 324-column window takes 3 frames at any height. The second bound is hits: addlight is
// ~1.3k voxels of sqrtf plus a chunk refresh, so a dense lightbox cluster stops the slice early
// (at a column boundary) rather than letting 108 columns of them land in one frame.
#define LIGHTING_SWEEP_COLUMN_BUDGET 108
#define LIGHTING_SWEEP_HIT_BUDGET 32

static int g_lighting_sweep_cursor=0;

// Called by updateLightingBegin (Terrain.mm) whenever it zeroes lightarray to start a fresh
// rebuild -- otherwise a second teleport mid-slice would resume from the old cursor and never
// re-sweep the columns before it.
void calculateLightingSliceReset(){ g_lighting_sweep_cursor=0; }

BOOL calculateLightingSlice(){
    if(LOW_MEM_DEVICE)return TRUE;
    int& cursor=g_lighting_sweep_cursor;
    const int ncols=CHUNKS_PER_SIDE*CHUNKS_PER_SIDE;
    int hits=0;
    for(int done=0;done<LIGHTING_SWEEP_COLUMN_BUDGET&&hits<LIGHTING_SWEEP_HIT_BUDGET&&cursor<ncols;done++,cursor++)
        hits+=sweepLightingColumn(cursor/CHUNKS_PER_SIDE,cursor%CHUNKS_PER_SIDE);
    if(cursor>=ncols){cursor=0;return TRUE;}
    return FALSE;
}
/*if(getLandc(x,z,y)==TYPE_NONE)continue;
 float ret=y/T_HEIGHT/2+.7f;
 for(int i=1;i<20;i++){
 if(i+y>=T_HEIGHT){
 
 break;
 }
 if(getLandc(x,z,y+i)!=TYPE_NONE){
 
 ret-=.05f;
 
 
 }
 }
 
 
 if(ret<0)ret=0;
 if(ret>1)ret=1;
 lightarray[x*T_SIZE*T_HEIGHT+z*T_HEIGHT+y]=ret;*/


