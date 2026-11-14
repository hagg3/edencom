//  Ambient.cpp — Stage AT.7: ambient particles. See Ambient.h.
#include <math.h>
#include <string.h>
#include <stdint.h>
#include "Ambient.h"
#include "Weather.h"
#include "SkyCycle.h"
#include "World.h"
#include "Terrain.h"
#include "Camera.h"
#include "FileManager.h"
#include "Hud.h"
#include "Graphics.h"
#include "Renderer.h"
#include "Constants.h"
#include "Globals.h"

extern "C" int eden_atmo_tier(void);      // port/src/seam/Settings.cpp (AT.1a): 0 Off, 1 Low, 2 High
extern "C" int eden_atmo_element(int e);  // AT_AMBIENT, AT_HALOS (Weather.h)
extern Vector colorTable[256];
int eden_lamp_positions(float ex,float ey,float ez,float radius,int cap,float (*out)[3]);   // Terrain.cpp (AT.8)

namespace {
using namespace Ambient;

struct P { float x,y,z,vx,vy,vz,age,life,phase; uint8_t kind,paint; bool live; };   // paint: an ember's lava colour
P g_p[POOL];
int g_n=0,g_kind[KINDS]={0},g_drawn=0;
long g_spawned=0;
uint32_t g_rng=0x414D4249u;               // "AMBI"
vertexpStruct g_v[POOL];                  // the draw batch, streamed

// Each kind's share of the cap, so one kind (motes on a meadow) cannot starve the others.
// AT.11: motes 0.25 (were 0.60 and took nearly every slot in the open).
const float kShare[KINDS]={0.40f,0.30f,0.25f,0.30f,0.30f};

// AT.11: spawn sources. A random sample that finds a grass / leaf / lava cell remembers it here, and the kind
// then emits from its remembered cells at a steady rate: random sampling alone found a 2x2 lava pool or a
// small canopy about once a minute (embers and leaves never showed). Checked again on every emission (still
// that block, still in range), so an edit or a walk away forgets it.
struct Src { int x,y,z; };
enum { SRC_MAX = 8 };
Src g_src[KINDS][SRC_MAX];
int g_srcN[KINDS]={0},g_srcNext[KINDS]={0};
// AT.11b: embers 0.35 (were 1.5, a steady fountain); each emission is a pop of one to three sparks.
const float kRate[KINDS]={0.30f,0.25f,0.0f,0.35f,0.0f};   // emissions a second per remembered source
float g_clock=0;                          // AT.11: seconds, wrapped; halos' and the water sparkle's clock

// AT.11: the glow texture (lamp halos and firefly glows): a round glow quantised into hard rings on a 16 x 16
// grid, nearest-sampled, drawn additively on camera-facing quads. Built on first use, freed when unused.
R::Tex g_glow=0;
struct GV { float x,y,z; uint8_t c[4]; float u,v; };
const R::VertexFormat kFmtGlow={24,{R::A_F32,3,0},{R::A_U8N,4,12},{R::A_F32,2,16},{},{},{},{}};
enum { GLOW_MAX = 64 + CAP_HIGH };        // lamps + every firefly
GV g_gv[GLOW_MAX*6];

inline float rnd(){g_rng^=g_rng<<13;g_rng^=g_rng>>17;g_rng^=g_rng<<5;return (g_rng>>8)*(1.0f/16777216.0f);}
inline float rng(float a,float b){return a+(b-a)*rnd();}

bool enabled(){return eden_atmo_element(AT_AMBIENT);}
int capNow(){return enabled()?(eden_atmo_tier()>=2?CAP_HIGH:CAP_LOW):0;}
int kindCap(int k){const int c=(int)(capNow()*kShare[k]+0.5f);return c<1&&capNow()?1:c;}

bool isGrass(int t){return t==TYPE_GRASS||t==TYPE_GRASS2||t==TYPE_GRASS3||t==TYPE_FLOWER;}
bool isLava(int t){return t==TYPE_LAVA||t==TYPE_LAVA1||t==TYPE_LAVA2||t==TYPE_LAVA3;}
bool isWater(int t){return t==TYPE_WATER||t==TYPE_WATER1||t==TYPE_WATER2||t==TYPE_WATER3;}

void kill(int i){
    if(!g_p[i].live)return;
    g_p[i].live=false;g_n--;g_kind[g_p[i].kind]--;
}

P* add(Kind k,float x,float y,float z){
    if(g_n>=capNow()||g_kind[k]>=kindCap(k))return nullptr;
    for(int i=0;i<POOL;i++){
        if(g_p[i].live)continue;
        P& p=g_p[i];
        p.x=x;p.y=y;p.z=z;p.vx=p.vy=p.vz=0;p.age=0;p.phase=rnd()*6.2831853f;p.kind=(uint8_t)k;p.paint=0;p.live=true;
        switch(k){
            case FIREFLY:p.life=rng(8,14);break;
            case LEAF:   p.life=rng(8,14);p.vy=-0.8f;break;
            case MOTE:   p.life=rng(10,16);break;
            // AT.11b: a spark pops up fast, slows (update decelerates it), drifts and dies in about a second.
            case EMBER:  p.life=rng(0.7f,1.6f);p.vy=rng(1.8f,3.2f);p.vx=rng(-0.5f,0.5f);p.vz=rng(-0.5f,0.5f);break;
            default:     p.life=rng(2,4);p.vy=rng(0.8f,1.3f);break;
        }
        g_n++;g_kind[k]++;g_spawned++;
        return &p;
    }
    return nullptr;
}

void remember(Kind k,int x,int y,int z){
    for(int i=0;i<g_srcN[k];i++)if(g_src[k][i].x==x&&g_src[k][i].y==y&&g_src[k][i].z==z)return;
    int i=g_srcN[k]<SRC_MAX?g_srcN[k]++:(g_srcNext[k]++%SRC_MAX);
    g_src[k][i]=Src{x,y,z};
}
void forget(Kind k,int i){g_src[k][i]=g_src[k][--g_srcN[k]];}

// Does cell (x, z, y) (Terrain order) still feed kind k?
bool feeds(Terrain* t,Kind k,int x,int y,int z){
    const int c=t->getLand(x,z,y);
    switch(k){
        case FIREFLY:return isGrass(c)&&t->getLand(x,z,y+1)==TYPE_NONE;
        case LEAF:   return c==TYPE_LEAVES&&t->getLand(x,z,y-1)==TYPE_NONE;
        case EMBER:  return isLava(c)&&t->getLand(x,z,y+1)==TYPE_NONE;
        default:     return false;
    }
}
void emitAt(Terrain* t,Kind k,int x,int y,int z){
    switch(k){
        case FIREFLY:add(FIREFLY,x+rnd(),y+rng(1.2f,2.6f),z+rnd());break;
        case LEAF:   add(LEAF,x+rnd(),y-0.05f,z+rnd());break;
        case EMBER:{
            // AT.11b: a pop of 1-3 sparks off one point, tinted by the lava's paint (green "acid" gives green).
            const int c=t->getColor(x,z,y);
            const float px=x+rng(0.15f,0.85f),pz=z+rng(0.15f,0.85f);
            const int burst=1+(rnd()<0.35f)+(rnd()<0.15f);
            for(int b=0;b<burst;b++)if(P* p=add(EMBER,px,y+1.02f,pz))p->paint=(uint8_t)(c>0?c:0);
            break;}
        default:break;
    }
}

void trySample(Terrain* t,float ex,float ey,float ez,bool night,bool wet,bool under){
    const int x=(int)floorf(ex+rng(-RADIUS,RADIUS)),z=(int)floorf(ez+rng(-RADIUS,RADIUS));
    if(under){
        // Bubbles: a liquid cell within a few blocks of the eye.
        const int bx=(int)floorf(ex+rng(-3,3)),bz=(int)floorf(ez+rng(-3,3)),by=(int)floorf(ey+rng(-2,2));
        if(isWater(t->getLand(bx,bz,by)))add(BUBBLE,bx+rnd(),by+rnd(),bz+rnd());
        return;
    }
    const int y=(int)floorf(ey+rng(-6,10));   // AT.11: more above the eye (canopies), was +-6
    const int c=t->getLand(x,z,y),a=t->getLand(x,z,y+1);
    if(c<0||a<0)return;
    if(isGrass(c)&&a==TYPE_NONE){
        remember(FIREFLY,x,y,z);
        if(night&&!wet&&rnd()<0.25f)emitAt(t,FIREFLY,x,y,z);
    }else if(c==TYPE_LEAVES){
        if(t->getLand(x,z,y-1)==TYPE_NONE){remember(LEAF,x,y,z);if(rnd()<0.08f)emitAt(t,LEAF,x,y,z);}
    }else if(isLava(c)){
        if(a==TYPE_NONE){remember(EMBER,x,y,z);if(rnd()<0.40f)emitAt(t,EMBER,x,y,z);}
    }else if(c==TYPE_NONE&&a==TYPE_NONE){
        if(!night&&!wet&&rnd()<0.04f)add(MOTE,x+rnd(),y+rnd(),z+rnd());   // AT.11: 0.04 (was 0.10)
    }
}

// AT.11: a column probe finds the surfaces a random cell rarely hits (a 2x2 lava pool is 4 cells in ~9000): from
// 10 above the eye down to 10 below, the first block; leaves are followed down to their underside. <= ~24 reads.
void probeColumn(Terrain* t,float ex,float ey,float ez){
    const int x=(int)floorf(ex+rng(-RADIUS,RADIUS)),z=(int)floorf(ez+rng(-RADIUS,RADIUS));
    const int top=(int)floorf(ey)+10,bot=(int)floorf(ey)-10;
    for(int y=top;y>=bot;y--){
        const int c=t->getLand(x,z,y);
        if(c<0)return;
        if(c==TYPE_NONE)continue;
        if(c==TYPE_LEAVES){
            int u=y;
            while(u>bot&&t->getLand(x,z,u-1)==TYPE_LEAVES)u--;
            if(t->getLand(x,z,u-1)==TYPE_NONE)remember(LEAF,x,u,z);
            return;
        }
        if(t->getLand(x,z,y+1)!=TYPE_NONE)return;
        if(isGrass(c))remember(FIREFLY,x,y,z);
        else if(isLava(c))remember(EMBER,x,y,z);
        return;
    }
}

// Each remembered source emits at its kind's rate; a source no longer that block or out of range is forgotten.
void emitFromSources(Terrain* t,float ex,float ez,float etime,bool night,bool wet){
    for(int k=0;k<KINDS;k++){
        if(!(kRate[k]>0))continue;
        for(int i=g_srcN[k]-1;i>=0;i--){
            const Src& sc=g_src[k][i];
            const float dx=sc.x+0.5f-ex,dz=sc.z+0.5f-ez;
            if(dx*dx+dz*dz>(RADIUS+2)*(RADIUS+2)){forget((Kind)k,i);continue;}
            if(rnd()>=kRate[k]*etime)continue;
            if(!feeds(t,(Kind)k,sc.x,sc.y,sc.z)){forget((Kind)k,i);continue;}
            if(k==FIREFLY&&(!night||wet))continue;
            emitAt(t,(Kind)k,sc.x,sc.y,sc.z);
        }
    }
}
}

namespace Ambient {

void update(float etime,bool paused){
    if(!paused&&etime>0){g_clock+=etime>0.1f?0.1f:etime;if(g_clock>1000.0f)g_clock-=1000.0f;}
    const int cp=capNow();
    if(!cp){if(g_n||g_srcN[0]||g_srcN[1]||g_srcN[3])reset();return;}
    World* w=World::getWorld;
    if(paused||!(etime>0)||!w||!w->cam||!w->terrain||!w->hud)return;
    if(etime>0.1f)etime=0.1f;
    Vector eye,at;
    w->cam->viewPoints(&eye,&at);
    const bool under=w->hud->underLiquid;

    // Tier drop: shed the surplus, newest slot first.
    for(int i=POOL-1;i>=0&&g_n>cp;i--)kill(i);
    for(int i=0;i<POOL;i++){
        P& p=g_p[i];
        if(!p.live)continue;
        p.age+=etime;
        const float t=p.age;
        switch(p.kind){
            case FIREFLY:
                p.vx=0.5f*sinf(t*0.9f+p.phase);p.vz=0.5f*cosf(t*0.7f+p.phase*1.7f);p.vy=0.25f*sinf(t*1.3f+p.phase);break;
            case LEAF:
                p.vx=0.7f*sinf(t*1.8f+p.phase);p.vz=0.5f*cosf(t*1.4f+p.phase);break;
            case MOTE:
                p.vx=0.18f*sinf(t*0.5f+p.phase);p.vz=0.18f*cosf(t*0.4f+p.phase*1.3f);p.vy=0.06f*sinf(t*0.8f+p.phase);break;
            case EMBER:{   // AT.11b: keeps its own velocity: buoyancy loses to drag, sideways drag + a little flutter
                const float dr=1.0f-1.2f*etime;
                p.vy-=2.6f*etime;p.vx=p.vx*dr+0.8f*sinf(t*7.0f+p.phase)*etime;p.vz=p.vz*dr+0.8f*cosf(t*6.1f+p.phase)*etime;break;}
            default:
                p.vx=0.15f*sinf(t*2.5f+p.phase);p.vz=0.15f*cosf(t*2.1f+p.phase);break;
        }
        p.x+=p.vx*etime;p.y+=p.vy*etime;p.z+=p.vz*etime;
        const float dx=p.x-eye.x,dz=p.z-eye.z;
        bool dead=p.age>=p.life||dx*dx+dz*dz>(RADIUS+4)*(RADIUS+4)||fabsf(p.y-eye.y)>14;
        if(!dead){
            const int bt=w->terrain->getLand((int)floorf(p.x),(int)floorf(p.z),(int)floorf(p.y));
            if(p.kind==BUBBLE)dead=!(bt>=0&&(isWater(bt)));
            else dead=bt!=TYPE_NONE;          // into a block (or out of the loaded world): gone; a leaf lands
        }
        if(dead)kill(i);
    }
    const bool night=sky_isNight();
    const bool wet=Weather::precipAmount()>0.3f;
    for(int s=0;s<SAMPLES;s++)trySample(w->terrain,eye.x,eye.y,eye.z,night,wet,under);
    if(!under)for(int s=0;s<PROBES;s++)probeColumn(w->terrain,eye.x,eye.y,eye.z);
    if(!under)emitFromSources(w->terrain,eye.x,eye.z,etime,night,wet);
}

// The pixels a point of world size 1 covers at eye distance 1: half the drawable height x the projection's
// cot(fovy / 2). The sprite program divides a point's size by its distance (atten 0, 0, 1).
static float focalPx(){
    int dw=0,dh=0;
    R::drawableSize(dw,dh);
    const float f=R::proj().top().m[5];
    return dh>0&&f>0?0.5f*dh*f:540.0f;
}

void render(){
    g_drawn=0;
    if(!g_n)return;
    World* w=World::getWorld;
    if(!w||!w->fm||!w->cam)return;
    // AT.11: world-sized square pixels (size / distance), was a fixed 1-2 px that vanished at 720p.
    const float focal=focalPx();
    Vector eye,at;
    w->cam->viewPoints(&eye,&at);
    float L[3]={1,1,1};
    if(SkyCycle::lightActive())SkyCycle::worldLight(false,L);
    const float ox=(float)(w->fm->chunkOffsetX*CHUNK_SIZE),oz=(float)(w->fm->chunkOffsetZ*CHUNK_SIZE);
    int n=0;
    for(int i=0;i<POOL;i++){
        const P& p=g_p[i];
        if(!p.live)continue;
        const float ddx=p.x-eye.x,ddy=p.y-eye.y,ddz=p.z-eye.z;
        if(ddx*ddx+ddy*ddy+ddz*ddz<0.36f)continue;   // nearer than 0.6: it would fill the screen
        float r,g,b,a,sz;
        const float u=p.age/p.life;
        const float fade=fminf(1.0f,fminf(p.age*2.0f,(p.life-p.age)*2.0f));   // ease in and out
        switch(p.kind){
            case FIREFLY:{
                const float blink=0.5f+0.5f*sinf(p.age*3.2f+p.phase*3.0f);
                r=1.0f;g=0.95f;b=0.45f;a=fade*(0.20f+0.60f*blink*blink);sz=0.05f;break;}   // AT.11b: 0.05 (was 0.09)
            case LEAF:  r=0.40f*L[0];g=0.62f*L[1];b=0.18f*L[2];a=0.95f*fade;sz=0.14f;break;
            case MOTE:  r=0.98f*L[0];g=0.95f*L[1];b=0.85f*L[2];a=0.45f*fade;sz=0.035f;break;
            case EMBER:{
                // AT.11b: hot (near white) to cooling, flickering; unpainted lava gives yellow -> orange -> dull red,
                // painted lava its paint, whitened while hot.
                const float fl=0.80f+0.20f*sinf(p.age*31.0f+p.phase*5.0f),hot=1.0f-u;
                if(p.paint){
                    const Vector& cv=colorTable[p.paint];
                    const float dim=fl*(1.0f-0.45f*u),wh=0.55f*hot*hot;
                    r=(cv.x+(1.0f-cv.x)*wh)*dim;g=(cv.y+(1.0f-cv.y)*wh)*dim;b=(cv.z+(1.0f-cv.z)*wh)*dim;
                }else{r=(1.0f-0.30f*u)*fl;g=(0.90f-0.72f*u)*fl;b=(0.50f-0.45f*u)*fl;}
                a=fminf(1.0f,p.age*12.0f)*fminf(1.0f,(p.life-p.age)*3.0f)*(1.0f-0.4f*u);
                sz=0.06f*(1.0f-0.5f*u);break;}
            default:    r=0.80f*L[0];g=0.92f*L[1];b=1.0f*L[2];a=0.60f*fade;sz=0.09f;break;
        }
        vertexpStruct& v=g_v[n++];
        v.position[0]=p.x-ox;v.position[1]=p.y;v.position[2]=p.z-oz;
        v.colors[0]=(uint8_t)(fminf(r,1.0f)*255);v.colors[1]=(uint8_t)(fminf(g,1.0f)*255);
        v.colors[2]=(uint8_t)(fminf(b,1.0f)*255);v.colors[3]=(uint8_t)(fminf(a,1.0f)*255);
        v.size[0]=sz*focal;
    }
    if(!n)return;
    R::setState(R::State{R::B_ALPHA,true,false,false,0.0f,0.0f});
    const R::Geometry g={&R::kFmtSprite,0,g_v,0,nullptr,R::I_NONE,0,n,R::POINTS};
    R::drawSprites(g,0,R::SpriteParams{1.0f,{0.0f,0.0f,1.0f}});
    g_drawn=n;
}

static float clamp01(float x){return x<0.0f?0.0f:x>1.0f?1.0f:x;}
static int g_halosDrawn=0,g_glowsDrawn=0;
static long g_glowUploads=0;

// The ring texture: radius bands in a 16 x 16 grid (centre 1.0 .. outer ring 0.12, corners 0; AT.11b: smaller core).
static bool ensureGlow(){
    if(g_glow)return true;
    static unsigned char px[16*16];
    for(int y=0;y<16;y++)for(int x=0;x<16;x++){
        const float dx=(x+0.5f-8.0f)/8.0f,dy=(y+0.5f-8.0f)/8.0f,r=sqrtf(dx*dx+dy*dy);
        px[y*16+x]=(unsigned char)(r<0.24f?255:r<0.48f?150:r<0.72f?80:r<0.96f?30:0);
    }
    const R::TexDesc d={16,16,R::TF_L8,false,false,true,true,"atmo-glow"};
    g_glow=R::texCreate(d,px);
    g_glowUploads++;
    return g_glow!=0;
}
static void freeGlow(){if(g_glow)R::texDestroy(g_glow);g_glow=0;}

// AT.11b: a lamp's halo shows only if the eye can see a face of it. The depth test alone let a halo through: the
// quad is pulled 0.9 toward the camera, so a lamp set in a one-block wall poked its rings out of the far side, and
// the wide quad spilled round the edges of whatever hid the lamp. Occluder = a solid, non-see-through block.
static bool occludes(int b){return b>0&&!(blockinfo[b]&(IS_NOTSOLID|IS_LIQUID|IS_NOSHADOW));}
// Voxel walk (Amanatides-Woo) from eye e to point q; true if no occluder lies on the way. Cells: Terrain order.
static bool clearRay(Terrain* t,const float e[3],const float q[3]){
    int c[3],step[3],last[3];
    float tmax[3],tdel[3];
    const float d[3]={q[0]-e[0],q[1]-e[1],q[2]-e[2]};
    for(int i=0;i<3;i++){
        c[i]=(int)floorf(e[i]);last[i]=(int)floorf(q[i]);
        step[i]=d[i]>0?1:-1;
        const float ad=fabsf(d[i]);
        tdel[i]=ad>1e-6f?1.0f/ad:1e30f;
        const float edge=d[i]>0?(c[i]+1-e[i]):(e[i]-c[i]);
        tmax[i]=ad>1e-6f?edge/ad:1e30f;
    }
    for(int n=0;n<4*HALO_RADIUS+8;n++){
        if(c[0]==last[0]&&c[1]==last[1]&&c[2]==last[2])return true;
        if(n>0&&occludes(t->getLand(c[0],c[2],c[1])))return false;
        const int a=tmax[0]<tmax[1]?(tmax[0]<tmax[2]?0:2):(tmax[1]<tmax[2]?1:2);
        if(tmax[a]>1.0f)return true;
        c[a]+=step[a];tmax[a]+=tdel[a];
    }
    return true;
}
// Can the eye see the lamp centred at l? Each face turned to the eye with an open cell before it, by a ray to that
// face's centre (just outside it). <= 3 walks, the first clear one wins.
static bool lampSeen(Terrain* t,const float e[3],const float l[3]){
    for(int a=0;a<3;a++){
        const float s=e[a]>l[a]?1.0f:-1.0f;
        if(fabsf(e[a]-l[a])<0.5f)continue;   // the eye is level with this face's slab: it cannot see it
        float q[3]={l[0],l[1],l[2]};
        q[a]+=0.55f*s;
        if(occludes(t->getLand((int)floorf(q[0]),(int)floorf(q[2]),(int)floorf(q[1]))))continue;
        if(clearRay(t,e,q))return true;
    }
    return false;
}
// Per-lamp visibility, kept across frames so a halo fades rather than pops. Re-walked VIS_PER_FRAME lamps a frame
// (round robin); a lamp new to the list is walked at once.
struct Vis { int x,y,z; float v; bool seen; };
static Vis g_vis[HALO_MAX],g_visNew[HALO_MAX];
static int g_visN=0,g_visNext=0;
enum { VIS_PER_FRAME = 16 };

// One camera-facing quad (two triangles) centred on render-local c, half-size hs, colour rgb x a.
static int quad(int n,const float c[3],float hs,const float rt[3],const float up[3],const float rgb[3],float a){
    static const float k[6][2]={{-1,-1},{1,-1},{1,1},{-1,-1},{1,1},{-1,1}};
    const uint8_t cr=(uint8_t)(clamp01(rgb[0])*255),cg=(uint8_t)(clamp01(rgb[1])*255),cb=(uint8_t)(clamp01(rgb[2])*255);
    const uint8_t ca=(uint8_t)(clamp01(a)*255);
    for(int i=0;i<6;i++){
        GV& v=g_gv[n*6+i];
        v.x=c[0]+(k[i][0]*rt[0]+k[i][1]*up[0])*hs;
        v.y=c[1]+(k[i][0]*rt[1]+k[i][1]*up[1])*hs;
        v.z=c[2]+(k[i][0]*rt[2]+k[i][1]*up[2])*hs;
        v.c[0]=cr;v.c[1]=cg;v.c[2]=cb;v.c[3]=ca;
        v.u=0.5f+0.5f*k[i][0];v.v=0.5f+0.5f*k[i][1];
    }
    return n+1;
}

// AT.11 (redesign of AT.8's halos, plan S1): one additive draw of glow quads: a steady pixel-ring halo round
// every lit lamp near the camera at night (High + atmo.halos), tinted by its paint, sized in world units, and
// a small glow round each firefly (whenever ambient particles are on). Faded on the CPU by the world fog's
// range (an additive quad must not take the fog colour) and near the camera. One texture, no buffer.
void renderHalos(){
    g_halosDrawn=0;g_glowsDrawn=0;
    const bool halos=eden_atmo_tier()>=2&&eden_atmo_element(AT_HALOS);
    const bool flies=g_kind[FIREFLY]>0;
    if(!halos&&!flies&&!capNow()){freeGlow();return;}
    World* w=World::getWorld;
    if(!w||!w->fm||!w->cam||!w->terrain)return;
    float dark;
    if(SkyCycle::enabled()){
        const float t=clamp01((SkyCycle::sunElevationDeg()+4.0f)/12.0f);   // -4 deg full dark .. 8 deg none
        dark=1.0f-t*t*(3.0f-2.0f*t);
    }else dark=sky_isNight()?1.0f:0.0f;
    dark=fmaxf(dark,0.35f*(1.0f-Weather::clearSky()));   // a dark storm lights lamps a little too
    Vector eye,at;
    w->cam->viewPoints(&eye,&at);
    static float pos[HALO_MAX][3];
    const int nl=halos&&dark>0.02f?eden_lamp_positions(eye.x,eye.y,eye.z,(float)HALO_RADIUS,HALO_MAX,pos):0;
    if(!nl&&!flies)return;
    if(!ensureGlow())return;
    // camera right / up in render-local space: the view's rotation rows
    const float* m=R::mv().top().m;
    float rt[3]={m[0],m[4],m[8]},up[3]={m[1],m[5],m[9]};
    const float rl=sqrtf(rt[0]*rt[0]+rt[1]*rt[1]+rt[2]*rt[2]),ul=sqrtf(up[0]*up[0]+up[1]*up[1]+up[2]*up[2]);
    if(!(rl>1e-6f&&ul>1e-6f))return;
    for(int i=0;i<3;i++){rt[i]/=rl;up[i]/=ul;}
    float fs,fe;
    Graphics::worldFogRange(&fs,&fe);
    const float fr=fe-fs>1.0f?fe-fs:1.0f;
    const float ox=(float)(w->fm->chunkOffsetX*CHUNK_SIZE),oz=(float)(w->fm->chunkOffsetZ*CHUNK_SIZE);
    int n=0;
    const float e3[3]={eye.x,eye.y,eye.z};
    int walks=0;
    for(int i=0;i<nl;i++){
        const int lx=(int)floorf(pos[i][0]),ly=(int)floorf(pos[i][1]),lz=(int)floorf(pos[i][2]);
        int o=-1;
        for(int j=0;j<g_visN;j++)if(g_vis[j].x==lx&&g_vis[j].y==ly&&g_vis[j].z==lz){o=j;break;}
        Vis& nv=g_visNew[i];
        nv.x=lx;nv.y=ly;nv.z=lz;
        if(o<0){nv.seen=lampSeen(w->terrain,e3,pos[i]);nv.v=nv.seen?1.0f:0.0f;walks++;}
        else{
            nv=g_vis[o];
            if(walks<VIS_PER_FRAME&&(i%VIS_PER_FRAME)==(g_visNext%VIS_PER_FRAME)){nv.seen=lampSeen(w->terrain,e3,pos[i]);walks++;}
            nv.v=nv.seen?fminf(1.0f,nv.v+0.15f):fmaxf(0.0f,nv.v-0.15f);
        }
    }
    memcpy(g_vis,g_visNew,sizeof(Vis)*nl);g_visN=nl;g_visNext++;
    for(int i=0;i<nl;i++){
        const float dx=pos[i][0]-eye.x,dy=pos[i][1]-eye.y,dz=pos[i][2]-eye.z;
        const float d=sqrtf(dx*dx+dy*dy+dz*dz);
        // AT.11b: 0.30 (was 0.42), x the eye's view of the lamp
        const float a=0.30f*g_vis[i].v*dark*clamp01((fe-d)/fr)*clamp01((HALO_RADIUS-d)/8.0f)*clamp01((d-1.2f)/1.5f);
        if(!(a>0.004f))continue;
        // A painted lamp glows its paint colour; an unpainted one warm white.
        const int col=w->terrain->getColor((int)floorf(pos[i][0]),(int)floorf(pos[i][2]),(int)floorf(pos[i][1]));
        float rgb[3]={1.0f,0.84f,0.55f};
        if(col>0){const Vector& cv=colorTable[col];rgb[0]=0.35f+0.65f*cv.x;rgb[1]=0.35f+0.65f*cv.y;rgb[2]=0.35f+0.65f*cv.z;}
        // The lamp is a solid block: the quad sits 0.9 toward the camera so its own faces do not hide the rings.
        const float k=-0.9f/d;
        const float c[3]={pos[i][0]+dx*k-ox,pos[i][1]+dy*k,pos[i][2]+dz*k-oz};
        n=quad(n,c,1.3f,rt,up,rgb,a);   // AT.11b: half-size 1.3 (was 1.8)
    }
    g_halosDrawn=n;
    for(int i=0;i<POOL&&n<GLOW_MAX;i++){
        const P& p=g_p[i];
        if(!p.live||p.kind!=FIREFLY)continue;
        const float dx=p.x-eye.x,dy=p.y-eye.y,dz=p.z-eye.z;
        const float d=sqrtf(dx*dx+dy*dy+dz*dz);
        const float blink=0.5f+0.5f*sinf(p.age*3.2f+p.phase*3.0f);
        const float fade=fminf(1.0f,fminf(p.age*2.0f,(p.life-p.age)*2.0f));
        const float a=0.35f*fade*blink*blink*clamp01((fe-d)/fr)*clamp01((d-0.6f)/0.6f);
        if(!(a>0.004f))continue;
        static const float fly[3]={0.85f,1.0f,0.35f};
        const float c[3]={p.x-ox,p.y,p.z-oz};
        n=quad(n,c,0.22f,rt,up,fly,a);   // AT.11b: 0.22 / 0.35 (were 0.40 / 0.75)
    }
    g_glowsDrawn=n-g_halosDrawn;
    if(!n)return;
    R::setState(R::State{R::B_ADD,true,false,false,0.0f,0.0f});
    R::fogOff();
    const R::Geometry g={&kFmtGlow,0,g_gv,0,nullptr,R::I_NONE,0,n*6,R::TRIANGLES};
    R::drawWorld(g,g_glow,R::Rgba{1,1,1,1});
}
int halosDrawn(){return g_halosDrawn;}
int glowsDrawn(){return g_glowsDrawn;}
long glowUploads(){return g_glowUploads;}
bool glowLive(){return g_glow!=0;}
float clock(){return g_clock;}

void reset(){
    memset(g_p,0,sizeof(g_p));
    g_n=0;g_drawn=0;
    for(int k=0;k<KINDS;k++){g_kind[k]=0;g_srcN[k]=0;g_srcNext[k]=0;}
    freeGlow();
    g_visN=0;g_visNext=0;
}

int cap(){return capNow();}
int count(){return g_n;}
int count(Kind k){return g_kind[k];}
int drawn(){return g_drawn;}
long spawned(){return g_spawned;}
bool spawnAt(Kind k,float x,float y,float z){return add(k,x,y,z)!=nullptr;}

}
