//  Precip.cpp — Stage AT.2b: rain. See Precip.h.
#include <math.h>
#include <string.h>
#include <limits.h>
#include "Precip.h"
#include "Weather.h"
#include "SkyCycle.h"
#include "World.h"
#include "Terrain.h"
#include "TerrainChunk.h"
#include "Camera.h"
#include "FileManager.h"
#include "Renderer.h"
#include "Util.h"
#include "Constants.h"
#include "Globals.h"

extern "C" int eden_atmo_tier(void);      // port/src/seam/Settings.cpp (AT.1a): 0 Off, 1 Low, 2 High
extern "C" int eden_atmo_element(int e);  // AT_PRECIP, AT_SPLASH (Weather.h)
extern int g_offcx,g_offcz;               // GBLOCK reads them (Terrain.cpp)
extern TerrainChunk** chunkTablec;
extern block8* blockarray;

namespace {
using namespace Precip;

const int N=BOX*BOX;
const int SEEDS=CAP_HIGH;                // the buffer holds the High cap; Low draws a prefix
R::Tex g_tex=0;
R::Buf g_buf=0;
unsigned char g_top[N];                  // by slot (z & 31) * BOX + (x & 31): the texture's layout
int g_colX[N],g_colZ[N];                 // the world column a slot holds; INT_MIN = not cached
bool g_rowDirty[BOX];
double g_snowFall=0,g_fall=0,g_driftX=0,g_driftZ=0;   // the motion so far (wrapped)
int g_drawn=0,g_splashes=0;
double g_splashPh=0;                    // AT.2c: the splash cycle clock, [0, 1)
long g_scans=0,g_rowUploads=0;

inline int slotOf(int x,int z){return (z&(BOX-1))*BOX+(x&(BOX-1));}
inline int floordiv(int a,int b){return a>=0?a/b:-((-a+b-1)/b);}

void invalidateAll(){
    for(int i=0;i<N;i++){g_colX[i]=INT_MIN;g_colZ[i]=INT_MIN;}
}

// The y of the first free cell above column (x, z)'s highest block (any non-air block stops rain: glass,
// water, leaves and doors too), from below `from`; bands that are empty from the top are skipped.
int scanTop(int x,int z,int from){
    g_scans++;
    if(!blockarray)return 0;
    if(from>T_HEIGHT)from=T_HEIGHT;
    if(chunkTablec){
        const int s=getColIndex(floordiv(x,CHUNK_SIZE),floordiv(z,CHUNK_SIZE));
        int band=(from-1)/CHUNK_SIZE;
        while(band>=0){
            TerrainChunk* c=chunkTablec[s*CHUNKS_PER_COLUMN+band];
            if(!c||!c->typesEmpty)break;
            band--;
        }
        if((band+1)*CHUNK_SIZE<from)from=(band+1)*CHUNK_SIZE;
    }
    const block8* strip=blockarray+((x+g_offcx)%T_SIZE)*g_xz_stride+((z+g_offcz)%T_SIZE)*g_world_height;
    for(int y=from-1;y>=0;y--)
        if(strip[y]!=TYPE_NONE)return y+1>255?255:y+1;
    return 0;
}

void setSlot(int s,int v){
    if(g_top[s]==v)return;
    g_top[s]=(unsigned char)v;
    g_rowDirty[s/BOX]=true;
}

void freeAll(){
    if(g_tex)R::texDestroy(g_tex);
    if(g_buf)R::bufDestroy(g_buf);
    g_tex=0;g_buf=0;
    g_drawn=0;g_splashes=0;
    invalidateAll();
}

bool ensure(){
    if(g_tex&&g_buf)return true;
    freeAll();
    memset(g_top,0,sizeof(g_top));
    memset(g_rowDirty,0,sizeof(g_rowDirty));
    const R::TexDesc d={BOX,BOX,R::TF_L8,false,false,true,true,"precip-tops"};
    g_tex=R::texCreate(d,g_top);
    // The seeds: a fixed splitmix stream, so every run (and every shot) draws the same rain.
    static unsigned char v[SEEDS*6*4];
    uint64_t r=0x5241494Eull;   // "RAIN"
    for(int i=0;i<SEEDS;i++){
        r+=0x9E3779B97F4A7C15ull;
        uint64_t z=r;
        z=(z^(z>>30))*0xBF58476D1CE4E5B9ull;
        z=(z^(z>>27))*0x94D049BB133111EBull;
        z^=z>>31;
        static const unsigned char kCorner[6]={0,1,3, 0,3,2};   // bit 0 side, bit 1 tail: two triangles
        for(int c=0;c<6;c++){
            unsigned char* o=v+(i*6+c)*4;
            o[0]=(unsigned char)(z);o[1]=(unsigned char)(z>>8);o[2]=(unsigned char)(z>>16);o[3]=kCorner[c];
        }
    }
    g_buf=R::bufCreate(R::BK_VERTEX,R::U_STATIC);
    R::bufData(g_buf,v,sizeof(v));
    return g_tex&&g_buf;
}

// Every slot holds its column of the box at (x0, z0); columns that entered since the last frame (or were
// invalidated) are scanned. Then the dirty rows upload as one contiguous range per run.
void syncCache(int x0,int z0){
    for(int x=x0;x<x0+BOX;x++)
        for(int z=z0;z<z0+BOX;z++){
            const int s=slotOf(x,z);
            if(g_colX[s]==x&&g_colZ[s]==z)continue;
            g_colX[s]=x;g_colZ[s]=z;
            setSlot(s,scanTop(x,z,T_HEIGHT));
        }
    for(int r=0;r<BOX;){
        if(!g_rowDirty[r]){r++;continue;}
        int e=r;
        while(e<BOX&&g_rowDirty[e]){g_rowDirty[e]=false;e++;}
        R::texUpdate(g_tex,0,r,BOX,e-r,R::TF_L8,g_top+r*BOX);
        g_rowUploads+=e-r;
        r=e;
    }
}

bool enabled(){
    return eden_atmo_element(AT_PRECIP)&&Weather::active();
}
}

namespace Precip {

void update(float etime,bool paused){
    if(paused||!(etime>0)||!g_tex)return;
    const float storm=Weather::kind()==Weather::STORM?1.0f:0.4f;
    g_fall=fmod(g_fall+etime*FALL_SPEED,(double)BOX_H);
    g_snowFall=fmod(g_snowFall+etime*SNOW_SPEED,(double)BOX_H);
    g_driftX=fmod(g_driftX+etime*1.6*storm,(double)BOX);
    g_driftZ=fmod(g_driftZ+etime*0.9*storm,(double)BOX);
    g_splashPh=fmod(g_splashPh+etime*0.9,1.0);
}

void render(){
    g_drawn=0;g_splashes=0;
    if(!enabled()){if(g_tex||g_buf)freeAll();return;}
    const float amount=Weather::precipAmount();
    World* w=World::getWorld;
    if(!(amount>0)||!w||!w->cam||!w->fm||!w->terrain)return;
    Vector eye,at;
    w->cam->viewPoints(&eye,&at);
    const Weather::Precip kindHere=Weather::precipAt(eye.x,eye.z,eye.y);
    const bool snow=kindHere==Weather::PRECIP_SNOW;
    if(kindHere!=Weather::PRECIP_RAIN&&!snow)return;   // dry: never
    if(!ensure())return;
    const int ex=(int)floorf(eye.x),ez=(int)floorf(eye.z),ey=(int)floorf(eye.y);
    const int x0=ex-BOX/2,z0=ez-BOX/2;
    syncCache(x0,z0);
    const int capN=cap();
    int n=(int)(capN*amount+0.5f);
    if(n>capN)n=capN;
    if(n<=0)return;

    // Render-local: the camera's modelview is world - chunkOffset * CHUNK_SIZE.
    const int ox=w->fm->chunkOffsetX*CHUNK_SIZE,oz=w->fm->chunkOffsetZ*CHUNK_SIZE;
    R::PrecipParams p;
    p.snow=snow;
    p.origin[0]=(float)(x0-ox);p.origin[1]=(float)(ey-BELOW);p.origin[2]=(float)(z0-oz);
    p.size[0]=BOX;p.size[1]=BOX_H;p.size[2]=BOX;
    p.offset[0]=(float)g_driftX;p.offset[1]=(float)(BOX_H-(snow?g_snowFall:g_fall));p.offset[2]=(float)g_driftZ;
    const float storm=Weather::kind()==Weather::STORM?1.0f:0.4f;
    p.slant[0]=1.6f*storm/FALL_SPEED;p.slant[1]=0.9f*storm/FALL_SPEED;
    p.length=STREAK_LENGTH;
    p.width=0.03f;
    p.widthPerDist=0.0025f;
    p.fade[0]=BOX*0.30f;p.fade[1]=BOX*0.5f;
    // The drops take the world light (cycle and overcast), so night rain is dim.
    float L[3]={1,1,1};
    if(SkyCycle::lightActive())SkyCycle::worldLight(false,L);
    p.color=R::Rgba{0.70f*L[0],0.75f*L[1],0.85f*L[2],0.35f+0.15f*amount};
    if(snow){
        p.width=0.07f;p.widthPerDist=0.006f;
        p.color=R::Rgba{0.95f*L[0],0.96f*L[1],1.0f*L[2],0.70f+0.15f*amount};
    }
    p.tops=g_tex;
    p.topsN=BOX;
    p.topsShift[0]=((ox%BOX)+BOX)%BOX;p.topsShift[1]=((oz%BOX)+BOX)%BOX;

    R::setState(R::State{R::B_ALPHA,true,false,false,0.0f,0.0f});
    const R::Geometry g={&R::kFmtPrecip,g_buf,nullptr,0,nullptr,R::I_NONE,0,n*6,R::TRIANGLES};
    R::drawPrecip(g,p);
    g_drawn=n;

    // AT.2c: splashes (High, atmo.splash). The same seeds and map, flat quads on the columns' tops.
    if(!snow&&eden_atmo_tier()>=2&&eden_atmo_element(AT_SPLASH)){
        int m=(int)(SPLASH_HIGH*amount+0.5f);
        if(m>SPLASH_HIGH)m=SPLASH_HIGH;
        if(m>0){
            R::PrecipParams sp=p;
            sp.splash=true;
            sp.length=(float)g_splashPh;
            sp.color=R::Rgba{0.80f*L[0],0.85f*L[1],0.95f*L[2],0.70f};   // AT.11: a hard ring now (was a soft blob at 0.55)
            const R::Geometry sg={&R::kFmtPrecip,g_buf,nullptr,0,nullptr,R::I_NONE,0,m*6,R::TRIANGLES};
            R::drawPrecip(sg,sp);
            g_splashes=m;
        }
    }
}

void onSetLand(int x,int z,int y,int type){
    if(!g_tex)return;
    const int s=slotOf(x,z);
    if(g_colX[s]!=x||g_colZ[s]!=z)return;
    const int cur=g_top[s];
    if(type!=TYPE_NONE){
        if(y+1>cur)setSlot(s,y+1>255?255:y+1);
    }else if(y+1==cur){
        setSlot(s,scanTop(x,z,y));
    }
}

void onLanded(int cx,int cz){
    if(!g_tex)return;
    for(int x=cx*CHUNK_SIZE;x<(cx+1)*CHUNK_SIZE;x++)
        for(int z=cz*CHUNK_SIZE;z<(cz+1)*CHUNK_SIZE;z++){
            const int s=slotOf(x,z);
            if(g_colX[s]==x&&g_colZ[s]==z){g_colX[s]=INT_MIN;g_colZ[s]=INT_MIN;}
        }
}

void reset(){freeAll();}

bool live(){return g_tex!=0&&g_buf!=0;}
int drawn(){return g_drawn;}
int top(int x,int z){
    const int s=slotOf(x,z);
    return g_tex&&g_colX[s]==x&&g_colZ[s]==z?g_top[s]:-1;
}
int splashes(){return g_splashes;}
long scans(){return g_scans;}
long rowUploads(){return g_rowUploads;}
int cap(){
    if(!enabled())return 0;
    return eden_atmo_tier()>=2?CAP_HIGH:CAP_LOW;
}

}
