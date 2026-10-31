//  SkyCycle.cpp — Stage DN (day/night cycle). DN.1: the night predicate (behaviour-neutral).
//  DN.2: the clock, the lock and the SKY_CLOCK record. DN.4: the sun and moon. DN.5: the night state
//  (hysteresis flip + re-mesh watcher) and the light curves (plan section 4).
#include <string.h>
#include <math.h>
#include "SkyCycle.h"
#include "Terrain.h"
#include "World.h"
#include "Util.h"
#include "Globals.h"
#include "Constants.h"
#include "Renderer.h"
#include "Camera.h"

extern Vector colorTable[256];
extern bool* chunksToUpdate;
extern bool* columnsToUpdate;

bool sky_paintedNight(){
    return v_equals(World::getWorld->terrain->final_skycolor,colorTable[54]);
}

namespace { bool nightState(); }
bool sky_isNight(){
    if(SkyCycle::enabled())return nightState();
    return sky_paintedNight();
}

void sky_requestNightRemesh(){
    if(LOW_MEM_DEVICE)return;
    if(!chunksToUpdate||!columnsToUpdate)return;   // DN.5: the watcher can run before allocateMemory
    memset(chunksToUpdate,true,sizeof(bool)*CHUNKS_PER_SIDE*CHUNKS_PER_SIDE*CHUNKS_PER_COLUMN);
    memset(columnsToUpdate,true,sizeof(bool)*CHUNKS_PER_SIDE*CHUNKS_PER_SIDE);
}

namespace {
bool g_enabled=false;
double g_dayLength=20*60;
double g_t=SkyCycle::DEFAULT_TIME;
bool g_locked=false;
// What the world's file holds (or would, with no record: the defaults), so a save writes the
// record only when the clock or the lock moved. A world nobody ran the cycle in stays record-free.
float g_savedT=(float)SkyCycle::DEFAULT_TIME;
bool g_savedLocked=false;

double wrap01(double t){
    if(!(t==t))return SkyCycle::DEFAULT_TIME;   // NaN
    t-=floor(t);
    return t>=1.0?0.0:t;
}

// DN.5 (plan D4): the clock's night bit. Night on below -4.5 degrees of sun elevation, off above -3.5
// (a band around the plan's -4, so a clock locked or set at the threshold cannot flicker). A jump
// (setTime, load, reset) decides against -4 itself.
bool g_night=false;
bool g_nightKnown=false;
const float FLIP_DEG=-4.0f, FLIP_BAND=0.5f;
// The re-mesh watcher (SkyCycle::update): the effective night bit last frame, and whether the cycle
// was on then, so a flip, or a toggle that changes the bit, re-meshes the window once.
bool g_watchValid=false, g_watchNight=false, g_watchEnabled=false;
int g_flips=0;

float elevationDeg(){
    const Vector s=SkyCycle::sunDir();
    float y=s.y; if(y>1)y=1; if(y<-1)y=-1;
    return (float)(asin(y)*180.0/M_PI);
}
void updateNight(bool jump){
    const float e=elevationDeg();
    if(jump||!g_nightKnown)g_night=e<FLIP_DEG;
    else if(g_night&&e>FLIP_DEG+FLIP_BAND)g_night=false;
    else if(!g_night&&e<FLIP_DEG-FLIP_BAND)g_night=true;
    g_nightKnown=true;
}
bool nightState(){
    if(!g_nightKnown)updateNight(true);
    return g_night;
}
}

namespace SkyCycle {

void setEnabled(bool on){g_enabled=on;}
bool enabled(){return g_enabled;}
void setDayLengthMinutes(int minutes){g_dayLength=(minutes>0?minutes:20)*60.0;}
double dayLengthSeconds(){return g_dayLength;}

double time(){return g_t;}
void setTime(double t){g_t=wrap01(t);updateNight(true);}
bool locked(){return g_locked;}
void setLocked(bool on){g_locked=on;}

void update(float etime,bool paused){
    if(g_enabled&&!g_locked&&!paused&&etime>0){
        g_t=wrap01(g_t+etime/g_dayLength);
        updateNight(false);
    }
    // DN.5: the flip. Every play frame (paused too: a `time` typed at the Console applies at once),
    // before Terrain::update meshes. While the cycle is off the painted-sky paths re-mesh themselves
    // (paintSky, the region walk), so the watcher only acts while the cycle is on or was last frame.
    const bool eff=sky_isNight();
    if(g_watchValid&&eff!=g_watchNight&&(g_enabled||g_watchEnabled)){
        sky_requestNightRemesh();
        if(!LOW_MEM_DEVICE)g_flips++;
    }
    g_watchNight=eff;
    g_watchValid=true;
    g_watchEnabled=g_enabled;
}

void reset(){
    g_t=DEFAULT_TIME;
    g_locked=false;
    g_savedT=(float)g_t;
    g_savedLocked=false;
    updateNight(true);
    g_watchValid=false;   // a world (re)loads: every chunk meshes fresh, no flip owed
}

bool decode(const uint8_t* rec,size_t n,double* t,bool* lockedOut){
    if(!rec||n!=RECORD_BYTES)return false;
    float f;
    memcpy(&f,rec,4);   // f32 LE (every target is little-endian)
    if(!(f>=0.0f&&f<1.0f))return false;   // also refuses NaN and infinities
    if(t)*t=f;
    if(lockedOut)*lockedOut=(rec[4]&FLAG_LOCKED)!=0;
    return true;   // other flag bits and bytes 5..15 are written 0 and ignored on read
}

void load(const uint8_t* rec,size_t n){
    double t;
    bool l;
    if(!decode(rec,n,&t,&l)){
        reset();
        return;
    }
    g_t=t;
    g_locked=l;
    g_savedT=(float)t;
    g_savedLocked=l;
    updateNight(true);
    g_watchValid=false;
}

void encode(uint8_t out[RECORD_BYTES]){
    memset(out,0,RECORD_BYTES);
    float f=(float)g_t;
    if(!(f<1.0f))f=0.0f;   // a t just under 1 can round up to 1.0f
    memcpy(out,&f,4);
    out[4]=g_locked?FLAG_LOCKED:0;
}

bool needsSave(){
    float f=(float)g_t;
    if(!(f<1.0f))f=0.0f;
    return f!=g_savedT||g_locked!=g_savedLocked;
}

void saved(){
    float f=(float)g_t;
    g_savedT=f<1.0f?f:0.0f;
    g_savedLocked=g_locked;
}

Vector sunDir(){
    // Plan D2: theta = 2pi(t - 0.25), rising in +x at t 0.25; the arc is tilted 20 degrees off the
    // zenith toward +z so the noon sun is not straight overhead.
    const double th=2.0*M_PI*(g_t-0.25);
    const double tilt=20.0*M_PI/180.0;
    Vector v;
    v.x=(float)cos(th);
    v.y=(float)(sin(th)*cos(tilt));
    v.z=(float)(sin(th)*sin(tilt));
    return v;
}

}

// ---- DN.5: the light curves (plan section 4) ------------------------------------------------
namespace {
float smooth01(float x){
    if(x<0)x=0; if(x>1)x=1;
    return x*x*(3.0f-2.0f*x);
}
// L(elevation): 1.0 at 15 degrees and above; exactly NIGHT_SKYLIGHT at the -4 flip point; 0.75 of that
// by -18 (darker than the stock flat night; DN.8 tunes it).
float lightAt(float e){
    const float N=SkyCycle::NIGHT_SKYLIGHT;
    if(e>=15.0f)return 1.0f;
    if(e>=FLIP_DEG)return N+(1.0f-N)*smooth01((e-FLIP_DEG)/(15.0f-FLIP_DEG));
    return N-N*0.25f*smooth01((FLIP_DEG-e)/14.0f);
}
// The time-of-day colour: white by day, warm gold around the horizon, cool blue at night.
void tintAt(float e,float out[3]){
    out[0]=out[1]=out[2]=1.0f;
    float wg=1.0f-fabsf(e-3.0f)/12.0f;
    if(wg<0)wg=0;
    wg*=0.7f;
    const float warm[3]={1.0f,0.80f,0.62f};
    const float cool[3]={0.78f,0.86f,1.0f};
    const float wn=smooth01((FLIP_DEG-e)/12.0f);
    for(int c=0;c<3;c++){
        out[c]+=(warm[c]-out[c])*wg;
        out[c]+=(cool[c]-out[c])*wn;
    }
}
}

namespace SkyCycle {

float sunElevationDeg(){return elevationDeg();}

float skyLight(){
    if(!g_enabled)return 1.0f;
    return lightAt(elevationDeg());
}

void worldLight(bool bakedNight,float out[3]){
    if(!g_enabled){out[0]=out[1]=out[2]=1.0f;return;}
    const float e=elevationDeg();
    const float L=lightAt(e);
    const float k=bakedNight?L/NIGHT_SKYLIGHT:L;
    float tint[3];
    tintAt(e,tint);
    for(int c=0;c<3;c++)out[c]=k*tint[c];
}

void blendLight(const float day[3],const float night[3],float out[3]){
    if(!g_enabled){
        const float* src=(!LOW_MEM_DEVICE&&sky_isNight())?night:day;
        for(int c=0;c<3;c++)out[c]=src[c];
        return;
    }
    const float e=elevationDeg();
    const float L=lightAt(e);
    float tint[3];
    tintAt(e,tint);
    for(int c=0;c<3;c++){
        float v;
        if(L>=NIGHT_SKYLIGHT){
            const float f=(1.0f-L)/(1.0f-NIGHT_SKYLIGHT);
            v=day[c]+(night[c]-day[c])*f;
        }else v=night[c]*(L/NIGHT_SKYLIGHT);
        out[c]=v*tint[c];
    }
}

int nightFlips(){return g_flips;}

}

// ---- DN.4: the pixel sun and moon -----------------------------------------------------------
// Drawn by Terrain::render inside the sky's ortho pass, after the gradient and before terrain, so
// trees and hills cover them. Sprites: media/sky_sprites.png (32x16, sun left, moon right).
#include <math.h>
#include "Resources.h"
#include "Texture2D.h"
#include "World.h"
#include "Camera.h"
#include "Constants.h"
extern float P_ASPECT_RATIO;
extern float eden_fov_degrees;

namespace SkyCycle {

static void drawBody(Texture2D* tex, const Vector& d, const Vector& fwd, const Vector& right,
                     const Vector& up, float u0, float u1, float cr, float cg, float cb){
    const float f=d.x*fwd.x+d.y*fwd.y+d.z*fwd.z;
    if(f<=0.05f) return;
    const double fovy=eden_fov_degrees>0.0f?eden_fov_degrees:80.0;
    const float th=(float)tan(fovy*M_PI/360.0);                 // half-height at distance 1
    const float sx=(d.x*right.x+d.y*right.y+d.z*right.z)/f/(th*P_ASPECT_RATIO);   // -1..1
    const float sy=(d.x*up.x+d.y*up.y+d.z*up.z)/f/th;
    // ~5 degrees tall, snapped to whole texels (a multiple of 16 points) so the steps stay crisp.
    float size=(float)SCREEN_HEIGHT*(5.0f/(float)fovy);
    size=16.0f*floorf(size/16.0f+0.5f); if(size<16.0f) size=16.0f;
    const float cx=floorf((sx*0.5f+0.5f)*(float)SCREEN_WIDTH+0.5f);
    const float cy=floorf((sy*0.5f+0.5f)*(float)SCREEN_HEIGHT+0.5f);
    if(cx<-size||cx>SCREEN_WIDTH+size||cy<-size||cy>SCREEN_HEIGHT+size) return;
    tex->drawSkySprite(CGRectMake(cx-size*0.5f,cy-size*0.5f,size,size),-P_ZFAR/1.000008,u0,u1,cr,cg,cb,1.0f);
}

void drawSunMoon(){
    if(!enabled()) return;
    Camera* cam=World::getWorld->cam;
    Vector fwd; cam->viewDir(&fwd);
    Vector right={fwd.y*0-fwd.z*1, fwd.z*0-fwd.x*0, fwd.x*1-fwd.y*0};   // fwd x (0,1,0)
    const float rl=sqrtf(right.x*right.x+right.y*right.y+right.z*right.z);
    if(rl<1e-4f) return;                                                   // looking straight up or down
    right.x/=rl; right.y/=rl; right.z/=rl;
    Vector up={right.y*fwd.z-right.z*fwd.y, right.z*fwd.x-right.x*fwd.z, right.x*fwd.y-right.y*fwd.x};   // right x fwd
    Texture2D* tex=Resources::getResources->getArtTex(ART_SKY_SPRITES);
    if(!tex) return;
    const Vector s=sunDir();
    if(s.y>-0.2f){
        // pale yellow at noon -> orange at the horizon
        float k=s.y/0.4f; if(k<0)k=0; if(k>1)k=1;
        drawBody(tex,s,fwd,right,up,0.0f,0.5f,
                 0.94f+0.02f*k, 0.63f+0.28f*k, 0.31f+0.38f*k);
    }
    if(s.y<0.2f){
        Vector m={-s.x,-s.y,-s.z};
        drawBody(tex,m,fwd,right,up,0.5f,1.0f,0.85f,0.88f,0.95f);
    }
}

}
