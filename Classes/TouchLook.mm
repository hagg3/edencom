//
//  TouchLook.mm
//  Emod — Stage L: the touch drag camera. See TouchLook.h and docs/player-input-camera.md.
//
//  Direct manipulation: the camera turns a fixed number of degrees per WINDOW point the finger
//  moves, every event's motion counted (Input.mm accumulates t->ldx/ldy before its int
//  truncation). An optional One-Euro filter smooths the finger's POSITION, not its delta, so the
//  total is conserved: when the finger stops the filtered position converges on the real one, and
//  release flushes what is left. Smoothing 0 bypasses the filter, so 0 is raw exactly.
//
#import "TouchLook.h"
#include <math.h>
#include <string.h>

float TOUCH_PT_SCALE=1.0f;
float TOUCH_LOOK_DEG_PER_PT=0.5f;
float TOUCH_LOOK_SENS_X=1.0f;
float TOUCH_LOOK_SENS_Y=1.0f;
float TOUCH_LOOK_SMOOTH=0.2f;   // "low"; tuned in L.6
float TOUCH_LOOK_GLIDE=0.0f;   // half-life, seconds; 0 = off
float TOUCH_SLOP_PT=12.0f;      // UIKit's pan slop is ~10; stock's 15 engine pt was ~27 iOS pt
int   TOUCH_LOOK_MUTATE=0;

namespace {

// One-Euro (Casiez et al. 2012) in window points and seconds.
const double kMaxCutoffHz=30.0;   // smoothing ~0: barely filtered
const double kMinCutoffHz=1.0;    // smoothing 1: heavy
const double kBeta=0.05;          // cutoff rises 1 Hz per 20 pt/s, so a flick is unfiltered
const double kDerivCutoffHz=4.0;  // the speed estimate (also the glide's start velocity)
const double kGlideStopDegS=3.0;  // ...or 5% of where it started, whichever is larger

struct Slot{
    double rx,ry;     // raw accumulated position, window points
    double fx,fy;     // filtered position
    double vx,vy;     // speed estimate, window points per second
    double consumed,discarded,sumx,sumy;   // engine points, for --look-trace
};
Slot g_slots[MAX_TOUCHES];

double g_glideYaw=0,g_glidePitch=0,g_glideStop=0;   // degrees per second
bool g_gliding=false;

double alpha(double cutoffHz,double dt){
    const double tau=1.0/(2.0*M_PI*cutoffHz);
    return 1.0/(1.0+tau/dt);
}
float scale(){
    if(TOUCH_LOOK_MUTATE&TL_MUT_ENGINEPTS) return 1.0f;
    return TOUCH_PT_SCALE>0?TOUCH_PT_SCALE:1.0f;
}
// Takes t->ldx/ldy into the slot's raw position.
void consume(Slot& s,itouch* t,double* wx,double* wy){
    s.consumed+=fabs((double)t->ldx)+fabs((double)t->ldy);
    s.sumx+=t->ldx;
    s.sumy+=t->ldy;
    *wx=(double)t->ldx*scale();
    *wy=(double)t->ldy*scale();
    t->ldx=t->ldy=0;
    s.rx+=*wx;
    s.ry+=*wy;
}
void toDegrees(double wx,double wy,float* dyaw,float* dpitch){
    *dyaw=(float)(wx*TOUCH_LOOK_DEG_PER_PT*TOUCH_LOOK_SENS_X);
    *dpitch=(float)(wy*TOUCH_LOOK_DEG_PER_PT*TOUCH_LOOK_SENS_Y);
}

} // namespace

float touchlook_slop_engine_pts(void){
    const float k=TOUCH_PT_SCALE>0?TOUCH_PT_SCALE:1.0f;
    return TOUCH_SLOP_PT/k;
}

void touchlook_begin(int slot){
    if(slot<0||slot>=MAX_TOUCHES) return;
    Slot& s=g_slots[slot];
    s.rx=s.ry=s.fx=s.fy=s.vx=s.vy=0;
    s.consumed=s.discarded=s.sumx=s.sumy=0;
    g_gliding=false;   // a touch-down catches the camera
}

void touchlook_discard(int slot,itouch* t){
    if(slot>=0&&slot<MAX_TOUCHES)
        g_slots[slot].discarded+=fabs((double)t->ldx)+fabs((double)t->ldy);
    t->ldx=t->ldy=0;
}

void touchlook_step(int slot,itouch* t,float etime,float* dyaw,float* dpitch){
    *dyaw=*dpitch=0;
    if(slot<0||slot>=MAX_TOUCHES) return;
    Slot& s=g_slots[slot];
    double wx,wy;
    consume(s,t,&wx,&wy);
    const double dt=etime>1e-4f?(double)etime:1.0/60.0;
    const double ad=alpha(kDerivCutoffHz,dt);
    s.vx+=ad*(wx/dt-s.vx);
    s.vy+=ad*(wy/dt-s.vy);
    double nfx=s.rx,nfy=s.ry;
    if(TOUCH_LOOK_SMOOTH>0){
        const double sm=TOUCH_LOOK_SMOOTH>1?1.0:(double)TOUCH_LOOK_SMOOTH;
        const double minCut=kMaxCutoffHz*pow(kMinCutoffHz/kMaxCutoffHz,sm);
        const double a=alpha(minCut+kBeta*sqrt(s.vx*s.vx+s.vy*s.vy),dt);
        nfx=s.fx+a*(s.rx-s.fx);
        nfy=s.fy+a*(s.ry-s.fy);
    }
    toDegrees(nfx-s.fx,nfy-s.fy,dyaw,dpitch);
    s.fx=nfx;
    s.fy=nfy;
}

void touchlook_end(int slot,itouch* t,float* dyaw,float* dpitch){
    *dyaw=*dpitch=0;
    if(slot<0||slot>=MAX_TOUCHES) return;
    Slot& s=g_slots[slot];
    double wx,wy;
    consume(s,t,&wx,&wy);
    if(!(TOUCH_LOOK_MUTATE&TL_MUT_NOFLUSH)){
        toDegrees(s.rx-s.fx,s.ry-s.fy,dyaw,dpitch);
        s.fx=s.rx;
        s.fy=s.ry;
    }
    if(TOUCH_LOOK_GLIDE>0){
        float gy,gp;
        toDegrees(s.vx,s.vy,&gy,&gp);
        const double mag=sqrt((double)gy*gy+(double)gp*gp);
        g_glideStop=fmax(kGlideStopDegS,0.05*mag);
        g_gliding=mag>g_glideStop;
        g_glideYaw=gy;
        g_glidePitch=gp;
    }
}

void touchlook_glide(float etime,float* dyaw,float* dpitch){
    *dyaw=*dpitch=0;
    if(!(TOUCH_LOOK_GLIDE>0)) g_gliding=false;   // switched off mid-glide
    if(!g_gliding||etime<=0) return;
    *dyaw=(float)(g_glideYaw*etime);
    *dpitch=(float)(g_glidePitch*etime);
    if(TOUCH_LOOK_MUTATE&TL_MUT_NODECAY) return;
    const double k=pow(0.5,(double)etime/(double)TOUCH_LOOK_GLIDE);
    g_glideYaw*=k;
    g_glidePitch*=k;
    if(sqrt(g_glideYaw*g_glideYaw+g_glidePitch*g_glidePitch)<g_glideStop) g_gliding=false;
}

void touchlook_stats(int slot,double out[4]){
    const bool ok=slot>=0&&slot<MAX_TOUCHES;
    out[0]=ok?g_slots[slot].consumed:0;
    out[1]=ok?g_slots[slot].discarded:0;
    out[2]=ok?g_slots[slot].sumx:0;
    out[3]=ok?g_slots[slot].sumy:0;
}

int touchlook_mutation_bit(const char* name){
    static const struct{ const char* name; int bit; } k[]={
        {"lastonly",TL_MUT_LASTONLY},{"int",TL_MUT_INT},{"enginepts",TL_MUT_ENGINEPTS},
        {"noflush",TL_MUT_NOFLUSH},{"noslop",TL_MUT_NOSLOP},{"noclear",TL_MUT_NOCLEAR},
        {"nodecay",TL_MUT_NODECAY},{"unlatched",TL_MUT_UNLATCHED},{"multi",TL_MUT_MULTI},{"clearlatch",TL_MUT_CLEARLATCH},
    };
    for(unsigned i=0;i<sizeof(k)/sizeof(k[0]);i++)
        if(name&&strcmp(name,k[i].name)==0) return k[i].bit;
    return 0;
}
