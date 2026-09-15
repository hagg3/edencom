//
//  Lighting.m
//  Eden
//
//  Created by Ari Ronen on 1/21/13.
//
//

#import "Lighting.h"
#import "Terrain.h"
extern Vector8* lightarray;
extern block8* blockarray;


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
                int lidx=((xx+x+g_offcx)%T_SIZE)*T_SIZE*T_HEIGHT+((zz+z+g_offcz)%T_SIZE)*T_HEIGHT+yy+y;
                if(inten!=0){
            //        printf("%f ",inten);
                }
               
                lightarray[lidx].x=MAX(0,MIN(255,lightarray[lidx].x+64.0f*inten*brightness*color.x));
                lightarray[lidx].y=MAX(0,MIN(255,lightarray[lidx].y+64.0f*inten*brightness*color.y));
                lightarray[lidx].z=MAX(0,MIN(255,lightarray[lidx].z+64.0f*inten*brightness*color.z));
                

               
               
               
            }
        }
    }
   // printf("\n");
}
extern Vector colorTable[256];
extern TerrainChunk** chunkTablec;

// One (cx,cz) column of the resident window: scan its chunks for TYPE_LIGHTBOX and splat each
// one's light. Shared by calculateLighting (whole window in one call, on world load) and
// calculateLightingSlice (a budgeted strip per frame, after a bulk window reload) so the two can
// never drift.
static void sweepLightingColumn(int cx,int cz){
    for(int cy=0;cy<CHUNKS_PER_COLUMN;cy++){
        TerrainChunk* chunk=chunkTablec[threeToOne(cx,cy,cz)];
        if(!chunk)continue;
        for(int y=chunk->pbounds[1];y<CHUNK_SIZE+chunk->pbounds[1];y++){
            for(int x=chunk->pbounds[0];x<CHUNK_SIZE+chunk->pbounds[0];x++){
                for(int z=chunk->pbounds[2];z<CHUNK_SIZE+chunk->pbounds[2];z++){
                    if(getLandc(x,z,y)==TYPE_LIGHTBOX){
                        addlight(x,z,y,1.0f,colorTable[getColorc(x,z,y)]);
                        World::getWorld->terrain->refreshChunksInRadius(x,z,y,LIGHT_RADIUS);
                    }
                }
            }
        }
    }
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

// Post-bulk-reload lighting (Terrain.mm's update_lighting path). calculateLighting above is an
// O(window volume) scan for TYPE_LIGHTBOX -- ~5.3M voxel tests at 64z, ~21M at 256z -- and
// measured as a single unbudgeted ~20ms (64z) / ~80ms (256z) main-thread stall once per
// teleport/warp (tools/headless-mesh-burst-probe*.js, 2026-08-27: it, not the chunk mesh budget,
// is the 256z reload spike). This walks the same window a budgeted strip of columns per frame,
// holding a cursor between calls; a partly-swept window just has some lightboxes not yet
// contributing for a few frames -- the same tolerated-stale state the reload budget itself relies
// on. Budget is counted in chunks, like BULK_RELOAD_CHUNK_BUDGET, so the per-frame cost stays
// flat as world height scales (256 chunks = 64 columns at 64z, 16 at 256z). Returns TRUE once the
// whole window has been swept; the caller clears update_lighting on that.
#define LIGHTING_SWEEP_CHUNK_BUDGET 256

static int g_lighting_sweep_cursor=0;

// Called by updateLightingBegin (Terrain.mm) whenever it zeroes lightarray to start a fresh
// rebuild -- otherwise a second teleport mid-slice would resume from the old cursor and never
// re-sweep the columns before it.
void calculateLightingSliceReset(){ g_lighting_sweep_cursor=0; }

BOOL calculateLightingSlice(){
    if(LOW_MEM_DEVICE)return TRUE;
    int& cursor=g_lighting_sweep_cursor;
    const int ncols=CHUNKS_PER_SIDE*CHUNKS_PER_SIDE;
    int col_budget=LIGHTING_SWEEP_CHUNK_BUDGET/CHUNKS_PER_COLUMN;
    if(col_budget<1)col_budget=1;
    for(int done=0;done<col_budget&&cursor<ncols;done++,cursor++)
        sweepLightingColumn(cursor/CHUNKS_PER_SIDE,cursor%CHUNKS_PER_SIDE);
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


