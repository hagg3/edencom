//
//  Lighting.m
//  Eden
//
//  Created by Ari Ronen on 1/21/13.
//
//

#import "Lighting.h"
#include "F3Timing.h"
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
// B.5b-1c: the neighbourhood rebuild's private brick table (same slots as lightbricks, non-NULL only
// during light_rebuildNeighbourhood), its own pool of zeroed bricks (at most 9 columns' worth), and
// per column slot the world chunk column the slot's light was last computed for (LIT_NONE: none).
static Vector8** lightscratch=NULL;
static Vector8** lightscratch_pool=NULL;
static int lightscratch_npool=0;
static int lightscratch_total=0;
static int* lit_col=NULL;               // 2 ints per column slot: wcx, wcz
static unsigned char* rebuild_mask=NULL;   // per column slot: inside the 3x3 being rebuilt
#define LIT_NONE INT_MIN
// P.10: per column slot, "this slot's occupant was scanned and holds no lightbox" -- for the world
// chunk column in lbfree_col (2 ints per slot, as lit_col). sweepLightingColumn skips the slot's
// 256 strip memchrs while it holds. Only "none" is ever trusted (a slot with lightboxes is always
// scanned), so the one way to go wrong is a lightbox appearing behind the flag's back; every way
// one can is cleared here: an occupant change (checked on every use), Terrain::addChunk
// (light_markLanded), Terrain::setLand of TYPE_LIGHTBOX (light_onSetLand), and the start of a full
// sweep or a store clear (every load, warp and bulk reload).
static unsigned char* lbfree=NULL;
static int* lbfree_col=NULL;
int g_light_cols_scanned=0,g_light_cols_skipped=0;
static void light_lbfreeClearAll(){
    if(lbfree)memset(lbfree,0,CHUNKS_PER_SIDE*CHUNKS_PER_SIDE);
}
int g_light_rebuilds=0;
double g_light_rebuild_ms=0,g_light_rebuild_max_ms=0;   // F3 clock (0 without EDEN_F3_HOOKS)
int g_light_test_mutation=0;

void light_storeAllocate(){
    lightbrick_slots=CHUNKS_PER_SIDE*CHUNKS_PER_SIDE*CHUNKS_PER_COLUMN;
    lightbricks=(Vector8**)calloc(lightbrick_slots,sizeof(Vector8*));
    lightbrick_pool=(Vector8**)malloc(sizeof(Vector8*)*lightbrick_slots);
    lightbrick_npool=0;
    lightbrick_total=0;
    lightscratch=(Vector8**)calloc(lightbrick_slots,sizeof(Vector8*));
    lightscratch_pool=(Vector8**)malloc(sizeof(Vector8*)*9*CHUNKS_PER_COLUMN);
    lightscratch_npool=lightscratch_total=0;
    lit_col=(int*)malloc(sizeof(int)*2*CHUNKS_PER_SIDE*CHUNKS_PER_SIDE);
    for(int i=0;i<2*CHUNKS_PER_SIDE*CHUNKS_PER_SIDE;i++)lit_col[i]=LIT_NONE;
    rebuild_mask=(unsigned char*)calloc(CHUNKS_PER_SIDE*CHUNKS_PER_SIDE,1);
    lbfree=(unsigned char*)calloc(CHUNKS_PER_SIDE*CHUNKS_PER_SIDE,1);
    lbfree_col=(int*)malloc(sizeof(int)*2*CHUNKS_PER_SIDE*CHUNKS_PER_SIDE);
    lightarray=g_light_selfcheck?(Vector8*)calloc((size_t)T_SIZE*T_SIZE*T_HEIGHT,sizeof(Vector8)):NULL;
}
void light_storeFree(){
    for(int i=0;i<lightbrick_slots;i++)free(lightbricks[i]);
    for(int i=0;i<lightbrick_npool;i++)free(lightbrick_pool[i]);
    free(lightbricks);
    free(lightbrick_pool);
    lightbricks=lightbrick_pool=NULL;
    lightbrick_slots=lightbrick_npool=lightbrick_total=0;
    for(int i=0;i<lightscratch_npool;i++)free(lightscratch_pool[i]);   // never left in lightscratch
    free(lightscratch);
    free(lightscratch_pool);
    lightscratch=lightscratch_pool=NULL;
    lightscratch_npool=lightscratch_total=0;
    free(lit_col);
    lit_col=NULL;
    free(rebuild_mask);
    rebuild_mask=NULL;
    free(lbfree);
    lbfree=NULL;
    free(lbfree_col);
    lbfree_col=NULL;
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
    light_lbfreeClearAll();   // P.10: Terrain::clearBlocks and every bulk reload come through here
}
void light_markLanded(int cx,int cz){
    if(lbfree&&g_light_test_mutation!=4)lbfree[getColIndex(cx,cz)]=0;
}
void light_onSetLand(int x,int z,int type){
    if(type==TYPE_LIGHTBOX&&lbfree&&g_light_test_mutation!=4)
        lbfree[getColIndex(x/CHUNK_SIZE,z/CHUNK_SIZE)]=0;
}
bool light_columnKnownFree(int cx,int cz){
    if(!lbfree)return false;
    const int s=getColIndex(cx,cz);
    return lbfree[s]&&lbfree_col[s*2]==cx*CHUNK_SIZE&&lbfree_col[s*2+1]==cz*CHUNK_SIZE;
}
// A zeroed brick for the table, not yet published.
static Vector8* light_takeBrick(){
    Vector8* b;
    if(lightbrick_npool)b=lightbrick_pool[--lightbrick_npool];
    else{
        b=(Vector8*)calloc(CHUNK_SIZE3,sizeof(Vector8));
        if(!b)return NULL;   // that light is lost; the dense array could not fail here, but it
        lightbrick_total++;  // was one 16-64 MB malloc up front instead
    }
    return b;
}
static Vector8* light_newBrick(int slot){
    Vector8* b=light_takeBrick();
    if(b)__atomic_store_n(&lightbricks[slot],b,__ATOMIC_RELEASE);
    return b;
}
// The rebuild's scratch table is main-thread-only (no worker ever sees it), so no atomics.
static Vector8* light_newScratchBrick(int slot){
    Vector8* b;
    if(lightscratch_npool)b=lightscratch_pool[--lightscratch_npool];
    else{
        if(lightscratch_total>=9*CHUNKS_PER_COLUMN)return NULL;   // cannot happen: a 3x3 of columns
        b=(Vector8*)calloc(CHUNK_SIZE3,sizeof(Vector8));
        if(!b)return NULL;
        lightscratch_total++;
    }
    lightscratch[slot]=b;
    return b;
}
unsigned long long light_storeBytes(){
    return (unsigned long long)(lightbrick_total+lightscratch_total)*CHUNK_SIZE3*sizeof(Vector8)+
           (unsigned long long)lightbrick_slots*3*sizeof(Vector8*);
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
// addlight's body, and the neighbourhood rebuild's. `scratch` writes the rebuild's private table
// instead of the live one; `colMask` (per column slot, NULL = all) clips the writes to the slots
// being rebuilt.
//
// B.5b-1c: no toroidal wrap. A voxel whose column is not resident -- past the window edge, where the
// toroidal index lands in the OPPOSITE edge's column -- gets nothing (isColumnResident, one pbounds
// compare per (x,z) of the sphere, hoisted out of the y loop). Stock lit that opposite column, and
// under ring streaming nothing would ever take that light away again. Every writer (edits, both
// sweeps, the rebuild) goes through here, so a fresh load and a walk agree on what the edge gets.
static void light_splat(int xx,int zz,int yy,float brightness,Vector color,bool scratch,const unsigned char* colMask){
 //   printf("light intensities: ");
    Vector8** table=scratch?lightscratch:lightbricks;
    for(int x=-LIGHT_RADIUS;x<=LIGHT_RADIUS;x++){
        for(int z=-LIGHT_RADIUS;z<=LIGHT_RADIUS;z++){
            if(x*x+z*z>LIGHT_RADIUS*LIGHT_RADIUS)continue;
            //if(xx+x<0||xx+x>=T_SIZE||zz+<0||z>=T_SIZE)return;
            if(g_light_test_mutation!=3&&!isColumnResident(xx+x,zz+z))continue;
            const int tx=(xx+x+g_offcx)%T_SIZE,tz=(zz+z+g_offcz)%T_SIZE;
            if(colMask&&!colMask[(tx/CHUNK_SIZE)*CHUNKS_PER_SIDE+tz/CHUNK_SIZE])continue;
            for(int y=-LIGHT_RADIUS;y<=LIGHT_RADIUS;y++){
                if(x*x+z*z+y*y>LIGHT_RADIUS*LIGHT_RADIUS)continue;
                if(y+yy<0||y+yy>=T_HEIGHT)continue;
                float inten=1.0f-sqrtf(x*x+z*z+y*y)/LIGHT_RADIUS;

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
                Vector8* b=table[slot];
                Vector8 cur;
                if(b)cur=b[light_brickIndex(tx,tz,yy+y)];
                else cur.x=cur.y=cur.z=0;
                Vector8 nv;
                nv.x=MAX(0,MIN(255,cur.x+64.0f*inten*brightness*color.x));
                nv.y=MAX(0,MIN(255,cur.y+64.0f*inten*brightness*color.y));
                nv.z=MAX(0,MIN(255,cur.z+64.0f*inten*brightness*color.z));
                if(!b){
                    if(!nv.x&&!nv.y&&!nv.z)continue;
                    if(!(b=scratch?light_newScratchBrick(slot):light_newBrick(slot)))continue;
                }
                b[light_brickIndex(tx,tz,yy+y)]=nv;
            }
        }
    }
   // printf("\n");
}
void addlight(int xx,int zz,int yy,float brightness,Vector color){
    if(LOW_MEM_DEVICE)return;
    light_splat(xx,zz,yy,brightness,color,false,NULL);
}
extern Vector colorTable[256];
extern TerrainChunk** chunkTablec;

// One lightbox hit: splat its light and mark the chunks it touches for re-meshing. The one place
// the two sweep forms below (and any future one) turn a found TYPE_LIGHTBOX into work.
static void sweepLightingHit(int x,int z,int y){
    g_f3.lightboxes++;
    addlight(x,z,y,1.0f,colorTable[getColorc(x,z,y)]);
    World::getWorld->terrain->refreshChunksInRadius(x,z,y,LIGHT_RADIUS);
}

// One (cx,cz) column of the resident window: find its TYPE_LIGHTBOX blocks and hand each one to
// `hit`. Shared by calculateLighting (whole window in one call, on world load),
// calculateLightingSlice (a budgeted strip per frame, after a bulk window reload) -- both with
// sweepLightingHit -- and light_rebuildNeighbourhood (B.5b-1c), so the three can never drift.
// Returns the number of lightboxes found.
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
//
// P.10: a regular column found to hold no lightbox is remembered (lbfree, above) and skipped until
// its slot's occupant changes or something could have put a lightbox in it. Walking with no
// lightboxes, the 25 slots of every neighbourhood rebuild were all scan (0.5 / 1.0 ms per landed
// column on the A8X at 64z / 256z); now only the newly landed column is.
static int sweepLightingColumn(int cx,int cz,void (*hit)(int x,int z,int y)){
    int hits=0;
    TerrainChunk* first=chunkTablec[threeToOne(cx,0,cz)];
    const int s=cx*CHUNKS_PER_SIDE+cz;
    if(lbfree&&lbfree[s]&&first&&lbfree_col[s*2]==first->pbounds[0]&&lbfree_col[s*2+1]==first->pbounds[2]){
        g_light_cols_skipped++;
        return 0;
    }
    g_light_cols_scanned++;
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
                    const block8* found=(const block8*)memchr(p,TYPE_LIGHTBOX,end-p);
                    if(!found)break;
                    hit(x,z,(int)(found-strip));
                    hits++;
                    p=found+1;
                }
            }
        }
        if(lbfree){
            lbfree[s]=(hits==0);
            lbfree_col[s*2]=first->pbounds[0];
            lbfree_col[s*2+1]=first->pbounds[2];
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
                        hit(x,z,y);
                        hits++;
                    }
                }
            }
        }
    }
    return hits;
}

// B.5b-1c: per-column light under ring streaming (plan §3.3 step 4).
//
// The invariant: every resident column's light is exactly what a full sweep of the current window
// would give it -- the clamped sum of the lightboxes in RESIDENT columns within LIGHT_RADIUS, never
// wrapped (light_splat). A column landing in slot S changes that for two sets of columns: the
// departing occupant's spatial neighbours (its lightboxes no longer count) and the newcomer's (its
// do). Both sets sit in the 3x3 of SLOTS around S, because a resident column at world offset d from
// S's occupant, old or new, is in slot S+d mod CHUNKS_PER_SIDE. So the 3x3 is cleared and re-lit
// from the lightboxes of the 5x5 of slots around it, writes clipped to the 3x3, and nothing else
// in the window moves.
//
// Byte-identical to the full sweep, not just equal in spirit: a voxel's light is built from the
// same hits in the same order. The 5x5 is visited in ascending slot order, as the sweep's cursor
// visits the window, and within a column the hits come from the same sweepLightingColumn. (addlight
// truncates each float contribution, and cur+d rounded in float can carry, so order is not quite
// free; the same order makes the question moot.)
//
// Built into the scratch table and then published brick by brick, only where a brick's bytes
// changed. A mesh worker can be reading the live table (calcLight, no lock), so the live bricks are
// never cleared and refilled in place: a brick that did not change is never touched, and one that
// did is marked for re-meshing, which redoes any mesh a worker baked from it mid-copy.
static void rebuildHit(int x,int z,int y){
    light_splat(x,z,y,1.0f,colorTable[getColorc(x,z,y)],true,rebuild_mask);
}
static void light_rebuildNeighbourhood(int scx,int scz){
    const double t0=f3_now();
    const int n=CHUNKS_PER_SIDE;
    int cols[9][2];
    for(int i=0;i<3;i++)
        for(int j=0;j<3;j++){
            cols[i*3+j][0]=(scx+i-1+n)%n;
            cols[i*3+j][1]=(scz+j-1+n)%n;
            rebuild_mask[cols[i*3+j][0]*n+cols[i*3+j][1]]=1;
        }
    if(lightarray){   // the R.3 self-check's dense mirror: clear the same 9 columns, re-lit below
        for(int k=0;k<9;k++)
            for(int lx=0;lx<CHUNK_SIZE;lx++)
                for(int lz=0;lz<CHUNK_SIZE;lz++){
                    const int tx=cols[k][0]*CHUNK_SIZE+lx,tz=cols[k][1]*CHUNK_SIZE+lz;
                    memset(&lightarray[tx*T_SIZE*T_HEIGHT+tz*T_HEIGHT],0,sizeof(Vector8)*T_HEIGHT);
                }
    }
    for(int cx=0;cx<n;cx++){
        const int dx=((cx-scx)%n+n)%n;
        if(dx>2&&dx<n-2)continue;
        for(int cz=0;cz<n;cz++){
            const int dz=((cz-scz)%n+n)%n;
            if(dz>2&&dz<n-2)continue;
            sweepLightingColumn(cx,cz,rebuildHit);
        }
    }
    Terrain* ter=World::getWorld->terrain;
    const size_t brick=sizeof(Vector8)*CHUNK_SIZE3;
    for(int k=0;k<9;k++){
        const int cx=cols[k][0],cz=cols[k][1];
        rebuild_mask[cx*n+cz]=0;
        for(int cy=0;cy<CHUNKS_PER_COLUMN;cy++){
            const int i=threeToOne(cx,cy,cz);
            Vector8* o=lightbricks[i];
            Vector8* sc=lightscratch[i];
            if(!o&&!sc)continue;
            bool changed=true;
            if(o&&sc){
                changed=memcmp(o,sc,brick)!=0;
                if(changed)memcpy(o,sc,brick);
            }else if(sc){
                Vector8* b=light_takeBrick();
                if(b){
                    memcpy(b,sc,brick);
                    __atomic_store_n(&lightbricks[i],b,__ATOMIC_RELEASE);
                }
            }else{   // went dark: back to the pool, as light_storeClear does
                __atomic_store_n(&lightbricks[i],(Vector8*)NULL,__ATOMIC_RELEASE);
                memset(o,0,brick);
                lightbrick_pool[lightbrick_npool++]=o;
            }
            if(sc){
                lightscratch[i]=NULL;
                memset(sc,0,brick);
                lightscratch_pool[lightscratch_npool++]=sc;
            }
            if(changed&&g_light_test_mutation!=2)ter->addToUpdateList2(cx,cy,cz);
        }
    }
    g_light_rebuilds++;
    const double ms=f3_now()-t0;
    g_light_rebuild_ms+=ms;
    if(ms>g_light_rebuild_max_ms)g_light_rebuild_max_ms=ms;
}

// Which world chunk column toroidal column slot (cx,cz) holds (LIT_NONE: none). Chunk 0 is the
// column's representative, as isColumnResident has it: a column lands whole (readColumn on the main
// thread, or a decode published whole by mp_publishFinished).
static void light_slotOccupant(int cx,int cz,int* wcx,int* wcz){
    TerrainChunk* c=chunkTablec[threeToOne(cx,0,cz)];
    if(c){ *wcx=c->pbounds[0]/CHUNK_SIZE; *wcz=c->pbounds[2]/CHUNK_SIZE; }
    else *wcx=*wcz=LIT_NONE;
}
// A full sweep is starting against the window as it stands: record every slot's occupant as lit.
// A column that lands while the sweep is still in flight then differs from the record, and
// light_syncLandedColumns re-lights its 3x3 once the sweep is done -- which also undoes whatever
// the sweep wrote for that slot's departed occupant.
static void light_markWindowLit(){
    light_lbfreeClearAll();   // P.10: a full sweep trusts nothing remembered from before it
    if(!lit_col)return;
    for(int cx=0;cx<CHUNKS_PER_SIDE;cx++)
        for(int cz=0;cz<CHUNKS_PER_SIDE;cz++){
            const int k=(cx*CHUNKS_PER_SIDE+cz)*2;
            light_slotOccupant(cx,cz,&lit_col[k],&lit_col[k+1]);
        }
}
int light_syncLandedColumns(){
    if(LOW_MEM_DEVICE||!lightbricks||g_light_test_mutation==1)return 0;
    int rebuilt=0;
    for(int cx=0;cx<CHUNKS_PER_SIDE;cx++)
        for(int cz=0;cz<CHUNKS_PER_SIDE;cz++){
            const int k=(cx*CHUNKS_PER_SIDE+cz)*2;
            int wcx,wcz;
            light_slotOccupant(cx,cz,&wcx,&wcz);
            if(wcx==lit_col[k]&&wcz==lit_col[k+1])continue;
            lit_col[k]=wcx;
            lit_col[k+1]=wcz;
            light_rebuildNeighbourhood(cx,cz);
            rebuilt++;
        }
    return rebuilt;
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
    
    
    light_markWindowLit();   // B.5b-1c
    for(int cx=0;cx<CHUNKS_PER_SIDE;cx++){
        for(int cz=0;cz<CHUNKS_PER_SIDE;cz++){
            sweepLightingColumn(cx,cz,sweepLightingHit);
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

bool calculateLightingSlice(){
    if(LOW_MEM_DEVICE)return true;
    int& cursor=g_lighting_sweep_cursor;
    const int ncols=CHUNKS_PER_SIDE*CHUNKS_PER_SIDE;
    int hits=0;
    if(cursor==0)light_markWindowLit();   // B.5b-1c: the sweep starts against this window
    for(int done=0;done<LIGHTING_SWEEP_COLUMN_BUDGET&&hits<LIGHTING_SWEEP_HIT_BUDGET&&cursor<ncols;done++,cursor++)
        hits+=sweepLightingColumn(cursor/CHUNKS_PER_SIDE,cursor%CHUNKS_PER_SIDE,sweepLightingHit);
    if(cursor>=ncols){cursor=0;return true;}
    return false;
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


