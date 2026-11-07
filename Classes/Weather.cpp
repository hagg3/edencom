//  Weather.cpp — Stage AT. AT.1b: the state, the schedule (plan §10.3), the region/snowline rule
//  (plan A5) and the WEATHER record (type 12). Off = absent: with Gameplay > Weather off, update()
//  returns before touching anything and needsSave() stays false, so the world never gets the record.
#include <string.h>
#include <math.h>
#include "Weather.h"
#include "SkyCycle.h"
#include "World.h"
#include "Terrain.h"
#include "TerrainGenerator.h"
#include "TerrainGen2.h"     // DEFAULT_LEVEL_SEED
#include "Constants.h"
#include "Globals.h"
#include "ModScript.h"
#include "Resources.h"

extern "C" int eden_weather_enabled(void);    // port/src/seam/Settings.cpp (AT.1a)
extern "C" int eden_reduce_flashing(void);   // Video > Reduce flashing (AT.1a)
extern "C" int eden_weather_override(void);   // --weather=: -1 none, 0 off, 1 clear, 2 overcast, 3 rain, 4 snow, 5 storm

bool eden_default_region(float x, float z, int* px, int* pz){
    // Player.cpp's bed picker, verbatim (float -> int truncation included).
    int ppx=x-4096*CHUNK_SIZE+GSIZE/2;
    int ppz=z-4096*CHUNK_SIZE+GSIZE/2;
    ppx=ppx/(GSIZE/4);
    ppz=ppz/(GSIZE/4);
    if(ppx>4)ppx=4;
    if(ppz>4)ppz=4;
    if(ppx<0)ppx=0;
    if(ppz<0)ppz=0;
    if(px)*px=ppx;
    if(pz)*pz=ppz;
    World* w=World::getWorld;
    return w&&w->terrain&&w->terrain->tgen&&w->terrain->tgen->LEVEL_SEED==DEFAULT_LEVEL_SEED;
}

namespace {
using namespace Weather;

// ---- AT.4 lightning state (not saved) -------------------------------------------------------------
const float FLASH_PEAK=0.5f;        // +50 % at most (plan A9)
const float FLASH_FADE=0.3f;        // seconds
const float DIM_FLOOR=0.15f;        // Reduce flashing: down to 0.85
const float DIM_SECONDS=0.5f;
const float SECOND_AT=0.4f;
const int MAX_THUNDER=4;
float g_flashT=-1.0f;               // seconds since the strike; < 0 none
bool g_second=false;
float g_untilStrike=-1.0f;          // < 0: draw on the first eligible frame
float g_thunderIn[MAX_THUNDER];
int g_thunderVar[MAX_THUNDER];
int g_thunderN=0;
int g_strikes=0,g_lastVar=-1,g_played=0;
uint64_t g_lrng=0x4C49474854ull;    // "LIGHT"
void (*g_thunderHook)(int)=NULL;
double lrand(){
    uint64_t z=(g_lrng+=0x9E3779B97F4A7C15ull);
    z=(z^(z>>30))*0xBF58476D1CE4E5B9ull;
    z=(z^(z>>27))*0x94D049BB133111EBull;
    z^=z>>31;
    return (double)(z>>11)*(1.0/9007199254740992.0);
}
// The flash/dim level for the current strike, 0..1.
float flashLevel(){
    if(g_flashT<0)return 0.0f;
    if(eden_reduce_flashing()){
        const float u=g_flashT/DIM_SECONDS;
        return u>=1.0f?0.0f:sinf(u*(float)M_PI);
    }
    float a=1.0f-g_flashT/FLASH_FADE;
    if(a<0)a=0;
    if(g_second&&g_flashT>=SECOND_AT){
        float b=0.6f*(1.0f-(g_flashT-SECOND_AT)/FLASH_FADE);
        if(b>a)a=b;
    }
    return a;
}

bool g_active=false;
int g_lastKind=-1;            // AT.1c: for the Lua `weather` event; -1 = adopt the next kind silently
bool g_seeded=false;          // false after reset(): the first active update seeds from LEVEL_SEED
State g_s={CLEAR,0,0.0f,1.0f,0,0};
State g_saved=g_s;            // what the file holds (or, with no record, the freshly seeded state)
int g_forced=-1;              // the --weather= kind applied (1..5), so it is applied once per change
float g_snowline=0.75f;
float g_mistHeight=0.40f;   // AT.5: the valley mist's top, a fraction of T_HEIGHT (cvar weather.mistheight)
// The walk (plan §10.3). Row = this spell, column = the next; zeros keep it on clear - overcast - rain -
// storm (rain entered and left through overcast, storm only from rain). With g_meanDays these give time
// shares clear 0.60, overcast 0.20, rain 0.15, storm 0.05 (visit rates 1.2 : 1 : 1 : 0.83 balance on each edge).
float g_next[KIND_COUNT][KIND_COUNT]={
    {0.5f,0.5f,0.0f,0.0f},
    {0.6f,0.0f,0.4f,0.0f},
    {0.0f,0.4f,0.1f,0.5f},
    {0.0f,0.0f,0.6f,0.4f},
};
float g_meanDays[KIND_COUNT]={0.5f,0.2f,0.15f,0.06f};
float g_clearScale=1.0f;      // SX.9: Weather frequency (clear spells' length)
float g_stormScale=1.0f;      // SX.9: Storms (the rain -> storm weight)
const float kTarget[KIND_COUNT]={0.0f,0.4f,0.7f,1.0f};

uint64_t splitmix(uint64_t& s){
    uint64_t z=(s+=0x9E3779B97F4A7C15ull);
    z=(z^(z>>30))*0xBF58476D1CE4E5B9ull;
    z=(z^(z>>27))*0x94D049BB133111EBull;
    return z^(z>>31);
}
double u01(){return (double)(splitmix(g_s.rng)>>11)*(1.0/9007199254740992.0);}

float drawLength(int k){
    // mean x [0.5, 1.5): spells in fractions of a day, so any Day length gets the same weather per day
    double s=g_meanDays[k]*(k==CLEAR?g_clearScale:1.0f)*daySeconds()*(0.5+u01());
    return s<1.0?1.0f:(float)s;
}

// The next spell: drawn from this kind's row of g_next (a repeat is a new spell of the same kind).
void nextSpell(){
    float row[KIND_COUNT];
    float sum=0;
    for(int k=0;k<KIND_COUNT;k++){
        row[k]=g_next[g_s.kind][k];
        if(g_s.kind==RAIN&&k==STORM)row[k]*=g_stormScale;   // SX.9
        sum+=row[k];
    }
    double r=u01()*(sum>0?sum:1);
    int k=g_s.kind;
    for(int j=0;j<KIND_COUNT;j++){
        if(row[j]<=0)continue;
        k=j;
        if(r<row[j])break;
        r-=row[j];
    }
    g_s.kind=k;
    g_s.flags&=~FLAG_SNOW;
    g_s.secondsLeft=drawLength(k);
    g_s.spells++;
}

void seed(uint32_t levelSeed){
    g_s.kind=CLEAR;
    g_s.flags=0;
    g_s.intensity=0.0f;
    g_s.spells=0;
    g_s.rng=(uint64_t)levelSeed^SEED_SALT;
    g_s.secondsLeft=drawLength(CLEAR);   // a world's first spell is clear, of a drawn length
    g_seeded=true;
    g_saved=g_s;                          // nothing to write until it has moved (P7)
}

float cover(){float c=g_s.intensity/kTarget[OVERCAST];return c>1?1:c<0?0:c;}
float heavy(){float h=(g_s.intensity-kTarget[OVERCAST])/(1.0f-kTarget[OVERCAST]);return h>1?1:h<0?0:h;}
}

namespace Weather {

bool active(){return g_active;}

double daySeconds(){return SkyCycle::enabled()?SkyCycle::dayLengthSeconds():NO_CYCLE_DAY_SECONDS;}

static void updateInner(float etime,bool paused);
void update(float etime,bool paused){
    updateInner(etime,paused);
    if(g_active&&g_seeded&&!paused&&etime>0){
        bool eligible=false,dry=false;
        World* w=World::getWorld;
        if(g_s.kind==STORM&&g_s.intensity>=0.99f&&w&&w->cam){
            Vector eye,at;
            w->cam->viewPoints(&eye,&at);
            const Precip p=precipAt(eye.x,eye.z,eye.y);
            eligible=p!=PRECIP_SNOW;
            dry=p==PRECIP_DRY;
        }
        lightningUpdate(etime,eligible,dry);
    }else if(!g_active){
        g_flashT=-1.0f;g_thunderN=0;
    }
    if(!g_active||!g_seeded)return;
    if(g_lastKind>=0&&g_lastKind!=g_s.kind)ModScript::noteWeather(g_s.kind,g_lastKind);
    g_lastKind=g_s.kind;
}

static void updateInner(float etime,bool paused){
    g_active=eden_weather_enabled()!=0;
    if(!g_active)return;
    if(!g_seeded){
        World* w=World::getWorld;
        seed(w&&w->terrain&&w->terrain->tgen?(uint32_t)w->terrain->tgen->LEVEL_SEED:0u);
    }
    // --weather=KIND: that kind at full strength for the run, held, never saved (needsSave()).
    const int ov=eden_weather_override();
    if(ov>=1){
        if(ov!=g_forced){
            static const int kKind[6]={CLEAR,CLEAR,OVERCAST,RAIN,RAIN,STORM};
            set((Kind)kKind[ov],1e6f,ov==4);
            g_s.intensity=kTarget[g_s.kind];
            g_forced=ov;
        }
        return;
    }
    if(paused||!(etime>0))return;
    const float target=kTarget[g_s.kind];
    const float step=etime/SWING_SECONDS;
    if(g_s.intensity<target)g_s.intensity=g_s.intensity+step>target?target:g_s.intensity+step;
    else if(g_s.intensity>target)g_s.intensity=g_s.intensity-step<target?target:g_s.intensity-step;
    if(!(g_s.flags&FLAG_LOCKED)){
        g_s.secondsLeft-=etime;
        if(g_s.secondsLeft<=0)nextSpell();
    }
}

void reset(){
    lightningReset();
    g_s.kind=CLEAR; g_s.flags=0; g_s.intensity=0.0f; g_s.secondsLeft=1.0f; g_s.spells=0; g_s.rng=0;
    g_saved=g_s;
    g_seeded=false;
    g_lastKind=-1;
    g_forced=-1;
}

void resetSeed(uint32_t levelSeed){
    reset();
    seed(levelSeed);
}

bool decode(const uint8_t* rec,size_t n,State* out){
    if(!rec||n!=RECORD_BYTES)return false;
    if(rec[0]>=KIND_COUNT)return false;
    float i,s;
    memcpy(&i,rec+4,4);   // LE (every target is little-endian)
    memcpy(&s,rec+8,4);
    if(!(i>=0.0f&&i<=1.0f))return false;            // also refuses NaN
    if(!(s>0.0f&&s<3.0e38f))return false;           // finite and > 0
    if(out){
        out->kind=rec[0];
        out->flags=rec[1]&(FLAG_LOCKED|FLAG_SNOW);  // other bits are ignored
        out->intensity=i;
        out->secondsLeft=s;
        memcpy(&out->spells,rec+12,4);
        memcpy(&out->rng,rec+16,8);
    }
    return true;
}

void load(const uint8_t* rec,size_t n){
    State s;
    if(!decode(rec,n,&s)){reset();return;}
    g_s=s;
    g_saved=s;
    g_seeded=true;
    g_lastKind=-1;
    g_forced=-1;
}

void encode(uint8_t out[RECORD_BYTES]){
    memset(out,0,RECORD_BYTES);
    out[0]=(uint8_t)g_s.kind;
    out[1]=(uint8_t)(g_s.flags&(FLAG_LOCKED|FLAG_SNOW));
    float i=g_s.intensity<0?0:g_s.intensity>1?1:g_s.intensity;
    float s=g_s.secondsLeft>0?g_s.secondsLeft:1.0f;
    memcpy(out+4,&i,4);
    memcpy(out+8,&s,4);
    memcpy(out+12,&g_s.spells,4);
    memcpy(out+16,&g_s.rng,8);
}

bool needsSave(){
    if(!g_seeded||eden_weather_override()>=1)return false;
    return g_s.kind!=g_saved.kind||g_s.flags!=g_saved.flags||
        fabsf(g_s.secondsLeft-g_saved.secondsLeft)>=30.0f||fabsf(g_s.intensity-g_saved.intensity)>=0.1f;
}

void saved(){g_saved=g_s;}

State state(){return g_s;}

Kind kind(){return (Kind)g_s.kind;}
float intensity(){return g_s.intensity;}
float secondsLeft(){return g_s.secondsLeft;}
bool locked(){return (g_s.flags&FLAG_LOCKED)!=0;}
void setLocked(bool on){if(on)g_s.flags|=FLAG_LOCKED; else g_s.flags&=~FLAG_LOCKED;}
bool forceSnow(){return (g_s.flags&FLAG_SNOW)!=0;}

void set(Kind k,float secs,bool snow){
    if(k<CLEAR||k>=KIND_COUNT)k=CLEAR;
    g_s.kind=k;
    if(snow)g_s.flags|=FLAG_SNOW; else g_s.flags&=~FLAG_SNOW;
    g_s.secondsLeft=secs>0?secs:drawLength(k);
}

float targetFor(Kind k){return k>=CLEAR&&k<KIND_COUNT?kTarget[k]:0.0f;}
void setTransitions(const float m[KIND_COUNT][KIND_COUNT]){
    // only along the chain: a jump (clear -> rain, overcast -> storm, ...) stays 0 whatever is asked
    for(int a=0;a<KIND_COUNT;a++)for(int b=0;b<KIND_COUNT;b++)
        g_next[a][b]=(a-b<=1&&b-a<=1&&m[a][b]>0)?m[a][b]:0.0f;
}
void setFrequency(int level){g_clearScale=level<=0?2.0f:level>=2?0.4f:1.0f;}
void setStorms(int level){g_stormScale=level<=0?0.0f:level==1?0.4f:level==2?1.0f:2.5f;}
void setMeanDays(const float d[KIND_COUNT]){for(int k=0;k<KIND_COUNT;k++)g_meanDays[k]=d[k]>0?d[k]:0.01f;}

float lightMul(){return (1.0f-0.30f*cover()-0.15f*heavy())*flashMul();}
float skyGrey(){return 0.8f*cover()+0.2f*heavy();}
float clearSky(){return 1.0f-cover();}
void greySky(float rgb[3]){
    if(!g_active)return;
    const float t=skyGrey();
    if(!(t>0))return;
    const float g=0.8f*(0.299f*rgb[0]+0.587f*rgb[1]+0.114f*rgb[2]);
    for(int c=0;c<3;c++)rgb[c]+=(g-rgb[c])*t;
    const float f=flashMul();   // AT.4: the lightning flash / dim (1 outside a strike)
    if(f!=1.0f)for(int c=0;c<3;c++){rgb[c]*=f;if(rgb[c]>1.0f)rgb[c]=1.0f;}
}
// AT.5: mist. Dawn mist is a bump around sunrise (SkyCycle time 0.26 +- 0.07) while the cycle runs; rain
// mist is heavy(). Both only with the weather on (G1: off = the stock fog).
float dawnMist(){
    if(!g_active||!SkyCycle::enabled())return 0.0f;
    const float x=(float)(SkyCycle::time()-0.26)/0.07f;
    if(!(x*x<1.0f))return 0.0f;
    const float b=1.0f-x*x;
    return b*b;
}
float mist(){
    if(!g_active)return 0.0f;
    const float d=dawnMist(),r=heavy();
    return d>r?d:r;
}
float fogScale(){
    if(!g_active)return 1.0f;
    return (1.0f-0.10f*cover()-0.30f*heavy())*(1.0f-0.30f*dawnMist());
}
bool heightFog(float params[4]){
    const float m=mist();
    if(!(m>0))return false;
    const float top=g_mistHeight*T_HEIGHT;
    params[0]=top;
    params[1]=1.0f/(0.12f*T_HEIGHT>2.0f?0.12f*T_HEIGHT:2.0f);   // full strength this far below the top
    params[2]=1.0f/48.0f;                                      // and from 48 blocks away
    params[3]=0.85f*m;
    return true;
}
void setMistHeight(float frac){g_mistHeight=frac<0?0:frac>1?1:frac;}
float mistHeight(){return g_mistHeight;}
float precipAmount(){return heavy();}

Precip precipAt(float x,float z,float y){
    if(!(precipAmount()>0))return PRECIP_NONE;
    if(g_s.flags&FLAG_SNOW)return PRECIP_SNOW;
    int px,pz;
    if(eden_default_region(x,z,&px,&pz)&&px==0){
        if(pz==0)return PRECIP_SNOW;
        if(pz==1||pz==2)return PRECIP_DRY;
    }
    return y>=g_snowline*T_HEIGHT?PRECIP_SNOW:PRECIP_RAIN;
}

float flashMul(){
    if(!g_active)return 1.0f;
    const float l=flashLevel();
    if(!(l>0))return 1.0f;
    return eden_reduce_flashing()?1.0f-DIM_FLOOR*l:1.0f+FLASH_PEAK*l;
}

void lightningUpdate(float etime,bool eligible,bool dry){
    if(g_flashT>=0){
        g_flashT+=etime;
        if(g_flashT>=(eden_reduce_flashing()?DIM_SECONDS:(g_second?SECOND_AT+FLASH_FADE:FLASH_FADE)))g_flashT=-1.0f;
    }
    for(int i=0;i<g_thunderN;){
        g_thunderIn[i]-=etime;
        if(g_thunderIn[i]<=0){
            const int v=g_thunderVar[i];
            g_thunderN--;
            g_thunderIn[i]=g_thunderIn[g_thunderN];g_thunderVar[i]=g_thunderVar[g_thunderN];
            g_played++;
            if(g_thunderHook)g_thunderHook(v); else Resources::getResources->playThunder(v);
        }else i++;
    }
    if(!eligible){g_untilStrike=-1.0f;return;}
    if(g_untilStrike<0)g_untilStrike=(float)((dry?8.0:4.0)+lrand()*(dry?12.0:8.0));   // dry: rarer
    g_untilStrike-=etime;
    if(g_untilStrike>0)return;
    g_untilStrike=(float)((dry?8.0:4.0)+lrand()*(dry?12.0:8.0));
    g_flashT=0.0f;
    g_second=!eden_reduce_flashing()&&lrand()<0.4;
    g_strikes++;
    // thunder: 0.5-4 s later; the longer the delay the further the strike (dry regions: always distant)
    const float delay=dry?(float)(2.5+lrand()*1.5):(float)(0.5+lrand()*3.5);
    const int var=delay<1.5f?0:delay<2.8f?1:2;
    g_lastVar=var;
    if(g_thunderN<MAX_THUNDER){g_thunderIn[g_thunderN]=delay;g_thunderVar[g_thunderN]=var;g_thunderN++;}
}
int strikeCount(){return g_strikes;}
int lastThunderVariation(){return g_lastVar;}
int thunderPlayed(){return g_played;}
void lightningReset(){
    g_flashT=-1.0f;g_second=false;g_untilStrike=-1.0f;g_thunderN=0;
    g_strikes=0;g_lastVar=-1;g_played=0;g_lrng=0x4C49474854ull;
}
void setThunderHook(void (*hook)(int)){g_thunderHook=hook;}

void setSnowline(float frac){g_snowline=frac<0?0:frac>1?1:frac;}
float snowline(){return g_snowline;}

}
