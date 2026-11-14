//
//  Graphics.m
//  prototype
//
//  Created by Ari Ronen on 10/11/10.
//  Copyright 2010 __MyCompanyName__. All rights reserved.
//

#include "Graphics.h"
#include "Renderer.h"
#include "Globals.h"
#include "Terrain.h"
#include "VectorUtil.h"
#include "BlockTraits.h"   // K.2-1: per-type behaviour
#include "SkyCycle.h"
#include "Weather.h"     // AT.2a: overcast greys the stock fog colour
#include "Clouds.h"      // AT.6: cloud shadows on the terrain passes
#include "Ambient.h"     // AT.11: the water sparkle clock
#include "World.h"       // AT.8: the camera in a liquid (Hud::underLiquid)
#include "Hud.h"

#define M_SQRT3 1.732050808
//static UIFont* thefont=NULL;//move this to resources sometime

#define CUBE_VERTICES 36
extern float SCREEN_WIDTH; 
extern float SCREEN_HEIGHT;
extern float P_ASPECT_RATIO;

extern const int16_t cubeTexture[CUBE_VERTICES*2] ;

extern const int16_t cubeShortVertices[CUBE_VERTICES*3] ;
/*
const static float skyboxVertices[]={
	// Render the front quad	
	0, 0,   0.5f, -0.5f, -0.5f ,
	1, 0,  -0.5f, -0.5f, -0.5f ,
	1, 1,  -0.5f,  0.5f, -0.5f ,
	0, 1,   0.5f,  0.5f, -0.5f ,
	
	
	// Render the left quad	
	0, 0,   0.5f, -0.5f,  0.5f ,
	1, 0,   0.5f, -0.5f, -0.5f ,
	1, 1,   0.5f,  0.5f, -0.5f ,
	0, 1,   0.5f,  0.5f,  0.5f ,
	
	
	// Render the back quad	
	0, 0,  -0.5f, -0.5f,  0.5f ,
	1, 0,   0.5f, -0.5f,  0.5f ,
	1, 1,   0.5f,  0.5f,  0.5f ,
	0, 1,  -0.5f,  0.5f,  0.5f ,
	
	
	// Render the right quad	
	0, 0,  -0.5f, -0.5f, -0.5f ,
	1, 0,  -0.5f, -0.5f,  0.5f ,
	1, 1,  -0.5f,  0.5f,  0.5f ,
	0, 1,  -0.5f,  0.5f, -0.5f ,
	
	
	// Render the top quad	
	0, 1,  -0.5f,  0.5f, -0.5f ,
	0, 0,  -0.5f,  0.5f,  0.5f ,
	1, 0,   0.5f,  0.5f,  0.5f ,
	1, 1,   0.5f,  0.5f, -0.5f ,
	
	
	// Render the bottom quad	
	0, 0,  -0.5f, -0.5f, -0.5f ,
	0, 1,  -0.5f, -0.5f,  0.5f ,
	1, 1,   0.5f, -0.5f,  0.5f ,
	1, 0,   0.5f, -0.5f, -0.5f ,
	
	
};


const static float cubeTexture12[] = {
	1, 1, 	//4
	0, 1, 	//3
	1,0,	//7  A front
	0,0, 	//8  B front
	1,1,	//5  C bottom
	0, 1, 	//3  D right
	1, 0,	//1  E right
	1, 1, 	//4  F top
	0, 0,	//2  G top
	1,0, 	//7  H left
	0,1,	//6  I left
	1,1,	//5  J bottom
	0, 0,	//2  K back
	1, 0 	//1  L back
	
};
// 1-------3-------4-------2   Cube = 8 vertices
// |  E __/|\__ A  |  H __/|   =================
// | __/   |   \__ | __/   |   Single Strip: 4 3 7 8 5 3 1 4 2 7 6 5 2 1
// |/   D  |  B   \|/   I  |   12 triangles:     A B C D E F G H I J K L
// 5-------8-------7-------6
//         |  C __/|
//         | __/   |
//         |/   J  |
//         5-------6
//         |\__ K  |
//         |   \__ |
//         |  L   \|         Left  D+E
//         1-------2        Right  H+I
//         |\__ G  |         Back  K+L
//         |   \__ |        Front  A+B
//         |  F   \|          Top  F+G
//         3-------4       Bottom  C+J
//
const static float cubeVertices12[] = {
	0.5, 0.5, 0.5 ,	//4
	-0.5, 0.5, 0.5 ,	//3
	0.5,-0.5, 0.5 ,	//7  A front
	-0.5,-0.5, 0.5 ,	//8  B front
	-0.5,-0.5,-0.5 ,	//5  C bottom
	-0.5, 0.5, 0.5 ,	//3  D right
	-0.5, 0.5,-0.5 ,	//1  E right
	0.5, 0.5, 0.5 ,	//4  F top
	0.5, 0.5,-0.5 ,	//2  G top
	0.5,-0.5, 0.5 ,	//7  H left
	0.5,-0.5,-0.5 ,	//6  I left
	-0.5,-0.5,-0.5 ,	//5  J bottom
	0.5, 0.5,-0.5 ,	//2  K back
	-0.5, 0.5,-0.5 	//1  L back
	
};

const static float cubeNormals12[] = {
	0, 0, 0 ,		//4
	0, 0, 0 ,	//3
	0, 0, 1 ,		//7  A front
	0, 0, 1 ,	//8  B front
	0, -1, 0 ,	//5  C bottom
	1, 0, 0 ,	//3  D right
	1, 0, 0 ,	//1  E right
	0, 1, 0 ,		//4  F top
	0, 1, 0 ,		//2  G top
	-1, 0, 0 ,		//7  H left
	-1, 0, 0 ,		//6  I left
	0, -1, 0 ,	//5  J bottom
	0, 0, -1 ,		//2  K back
	0, 0, -1		//1  L back
	
};*/
static vertexStruct vertices[CUBE_VERTICES];   // drawFirework/drawCube's scratch (filled once below)
// G.5b-6: the two cube VBOs initGraphics built only fed drawTexCube/drawTexCubep (no callers) and
// drawSkybox's commented-out faces, so they went with those functions.
bool changedFog=false;
// The world fog colour (G.5b-6): Terrain sets it through setFogColor, the terrain passes state it with
// applyWorldFog. GL's default fog colour until initGraphics sets the first one.
static R::Rgba s_fogColor = { 0, 0, 0, 0 };

// G.5b-7: the 2D pen (Graphics.h). Starts where ES 1.1 does (the shim's logical state did the same):
// blend, depth test, cull and texturing off, depth writes on, white; initGraphics turns on what 2.1 did.
namespace {
struct PenState {
    bool blend=false; R::Blend func=R::B_ALPHA;   // GL's initial (1,0) pair: the shim read it as SA,1-SA
    bool depthTest=false, depthWrite=true, cull=false, textured=false;
    R::Tex tex=0;
    R::Rgba color={1,1,1,1};
};
PenState s_pen;
}
namespace Pen {
void color(float r,float g,float b,float a){ s_pen.color=R::Rgba{r,g,b,a}; }
void colorub(unsigned char r,unsigned char g,unsigned char b,unsigned char a){
    s_pen.color=R::Rgba{r/255.0f,g/255.0f,b/255.0f,a/255.0f};   // the shim's conversion
}
void blend(bool on){ s_pen.blend=on; }
void blendFunc(R::Blend f){ s_pen.func=f; }
void depthTest(bool on){ s_pen.depthTest=on; }
void depthWrite(bool on){ s_pen.depthWrite=on; }
void cull(bool on){ s_pen.cull=on; }
void texture(R::Tex t){ s_pen.tex=t; }
void textured(bool on){ s_pen.textured=on; }
R::State state(){
    const R::State s={ s_pen.blend?s_pen.func:R::B_OFF, s_pen.depthTest, s_pen.depthWrite, s_pen.cull, 0.0f, 0.0f };
    return s;
}
R::Rgba rgba(){ return s_pen.color; }
void strip(const float* xyz,const float* st,int first,int count){
    struct V { float x,y,z,u,v; } q[16];
    if(count<=0||count>16) return;
    for(int i=0;i<count;i++){
        const int k=first+i;
        q[i].x=xyz[k*3]; q[i].y=xyz[k*3+1]; q[i].z=xyz[k*3+2]; q[i].u=st[k*2]; q[i].v=st[k*2+1];
    }
    R::setState(state());
    R::Geometry g={};
    g.fmt=&R::kFmtUI3UV; g.vmem=q; g.itype=R::I_NONE; g.first=0; g.count=count; g.prim=R::TRIANGLE_STRIP;
    R::draw2D(g, s_pen.textured?s_pen.tex:0, s_pen.color);
}
}

// G.5b-7: project.c's GLU builders (C) multiply onto the renderer's stacks through these. gluLookAt
// multiplies the modelview (the matrix mode current at every call in the engine); gluPerspective,
// gluOrtho2D, glhFrustumf2 and gluPickMatrix (no callers) the projection, as their GL use would.
extern "C" void eden_glu_mult(int projection,const float* m){
    (projection?R::proj():R::mv()).mult(m);
}
extern "C" void eden_glu_translate(int projection,float x,float y,float z){
    (projection?R::proj():R::mv()).translate(x,y,z);
}
extern "C" void eden_glu_scale(int projection,float x,float y,float z){
    (projection?R::proj():R::mv()).scale(x,y,z);
}
extern "C" void eden_glu_ortho(int projection,float l,float r,float b,float t,float n,float f){
    (projection?R::proj():R::mv()).ortho(l,r,b,t,n,f);
}
extern float P_ZFAR;
extern bool SUPPORTS_OGL2;
void Graphics::initGraphics()
{
	
	for(int f=0;f<6;f++){	
		
		for(int v=0;v<6;v++){
			int v_idx=f*6+v;
			for(int coord=0;coord<3;coord++)
			vertices[v_idx].position[coord]=cubeShortVertices[v_idx*3+coord];
			
			//front face
			//back face
			//left face
			//right face
			//bot face
			//top face
			
			vertices[v_idx].texs[0]=cubeTexture[v_idx*2];
			vertices[v_idx].texs[1]=cubeTexture[v_idx*2+1];
		}
	} 
   // glEnable(GL_LIGHTING);
	
	Pen::cull(true);
	Pen::depthTest(true);
	Pen::textured(true);
	
	// G.5b-7: GL_COLOR_MATERIAL, glLineWidth(6.6f), glShadeModel(GL_FLAT), the two client arrays and
	// GL_FOG_MODE GL_LINEAR fed only the shim (the renderer's programs are colour-material, smooth
	// (G.5b-6) and linear-fog by construction; nothing draws lines), so they went with it.
	//glEnableClientState(GL_NORMAL_ARRAY);
    //if(!SUPPORTS_OGL2){
	//glFogf(GL_FOG_DENSITY,.3);
	
    Vector v=MakeVector(0.6,0.6,0.9);
    
    float clr2[4]={v.x-.03f, v.y-.03f, v.z-.03f, 1.0f};
    extern Vector colorTable[256];
    if(v_equals(World::getWorld->terrain->final_skycolor,colorTable[14]))
        v=MakeVector(0.5,0.72,0.9);
        setFogColor(clr2);
        
        P_ZFAR--;
    Graphics::setZFAR(P_ZFAR+1);
        //glHint(GL_FOG_HINT,GL_NICEST);
        applyWorldFog();
        R::fogOff();
   // }
    
   
   
  
	//changedFog=TRUE:
   
	
}
// G.5b-6: the world fog: Terrain sets the colour, the terrain passes and (G.5b-7) the creatures state it.
void Graphics::setFogColor(const float clr[4]){
    s_fogColor.r=clr[0]; s_fogColor.g=clr[1]; s_fogColor.b=clr[2]; s_fogColor.a=clr[3];
}
extern "C" int eden_atmo_tier(void);      // port/src/seam/Settings.cpp (AT.1a): 0 Off, 1 Low, 2 High
extern "C" int eden_atmo_element(int e);  // AT_HEIGHT_FOG, AT_WATER_TINT, AT_GLINT (Weather.h)
// AT.8: the camera is in a liquid and Video > Atmosphere is on (Off = the stock look: only the HUD fill).
static const float UNDERWATER_FOG_END=24.0f;   // AT.11: 24 (was 28)
static float clampf(float x,float a,float b){return x<a?a:x>b?b:x;}
static bool underwaterFog(){
    World* w=World::getWorld;
    return eden_atmo_tier()>0&&w&&w->hud&&w->hud->underLiquid;
}
void Graphics::worldFogRange(float* start,float* end){
    float s0=P_ZFAR-P_ZFAR/1.6f, e0=P_ZFAR-30;
    if(underwaterFog()){ *start=1.0f; *end=UNDERWATER_FOG_END<e0?UNDERWATER_FOG_END:e0; return; }
    const float s=Weather::fogScale();
    if(s!=1.0f){ s0*=s; e0*=s; }
    *start=s0; *end=e0;
}
void Graphics::applyWorldFog(){
    float start,end;
    worldFogRange(&start,&end);
    if(underwaterFog()){
        // AT.8: fog is new under water (stock only tinted the screen): the liquid's own colour (in the day's light),
        // thick: nothing past UNDERWATER_FOG_END shows. Terrain past it is not culled (the sky behind would show).
        // AT.11: 0.7 of the liquid colour, so open water reads deep rather than as a flat bright tint.
        const Vector lc=World::getWorld->hud->liquidColor;
        float L[3]={1,1,1};
        if(SkyCycle::lightActive())SkyCycle::worldLight(false,L);
        const R::Rgba c={0.7f*lc.x*L[0],0.7f*lc.y*L[1],0.7f*lc.z*L[2],1.0f};
        R::setFog(c, start, end);
        return;
    }
    // AT.5: mist pulls both in (every tier; uniforms only; worldFogRange). The colour stays the sky's, so a chunk
    // culled past the pulled-in end (mistCullDistance) looks as the stock far-plane cut does. 1.0 = untouched.
    // AT.2a: with the cycle off, overcast greys the stock/painted fog colour here (the cycle's
    // SkyLook::fogColor is already greyed). Weather off or clear: the colour as set, exactly.
    if(Weather::active()&&!SkyCycle::enabled()){
        float rgb[3]={s_fogColor.r,s_fogColor.g,s_fogColor.b};
        Weather::greySky(rgb);
        const R::Rgba c={rgb[0],rgb[1],rgb[2],s_fogColor.a};
        R::setFog(c, start, end);
        return;
    }
    R::setFog(s_fogColor, start, end);
}
void Graphics::applyHeightFog(){
    float params[4];
    if(underwaterFog()||eden_atmo_tier()<2||!eden_atmo_element(AT_HEIGHT_FOG)||!Weather::heightFog(params)){ R::heightFogOff(); return; }
    // world y of an eye-space point: the view is a rotation (x a uniform scale) and a translation, so
    // y = (column 1 of its 3x3) . (eye - t) / |column 1|^2, t the view's translation (column-major m).
    const float* m=R::mv().top().m;
    const float cx=m[4],cy=m[5],cz=m[6];
    const float n2=cx*cx+cy*cy+cz*cz;
    if(!(n2>1e-12f)){ R::heightFogOff(); return; }
    const float plane[4]={cx/n2,cy/n2,cz/n2,-(cx*m[12]+cy*m[13]+cz*m[14])/n2};
    R::setHeightFog(plane,params);
}
// AT.8: water's colour pulled toward the sky/fog colour by the day and the weather (Low and High), plus the High
// tier's sun glint (AT.11: texel sparkles along the sun's path, the EDEN_WATER_GLINT twins), as R::setWaterTint on the transparent chunk draws. Call with the camera's view on the stack
// (before the blocks' x0.25 push). Off under water (the fog covers it) and with the element off.
void Graphics::applyWaterTint(){
    if(!eden_atmo_element(AT_WATER_TINT)||underwaterFog()){ R::waterTintOff(); return; }
    const float* m=R::mv().top().m;
    float up[3]={m[4],m[5],m[6]};
    const float un=sqrtf(up[0]*up[0]+up[1]*up[1]+up[2]*up[2]);
    if(!(un>1e-6f)){ R::waterTintOff(); return; }
    float rgb[3]={s_fogColor.r,s_fogColor.g,s_fogColor.b};
    if(Weather::active()&&!SkyCycle::enabled())Weather::greySky(rgb);
    const float tint[4]={rgb[0],rgb[1],rgb[2],0.30f};
    float sun[4]={0,0,0,0};
    if(eden_atmo_tier()>=2&&eden_atmo_element(AT_GLINT)){
        Vector s;
        float day;
        if(SkyCycle::enabled()){
            const float t=clampf((SkyCycle::sunElevationDeg()-2.0f)/13.0f,0.0f,1.0f);
            day=t*t*(3.0f-2.0f*t);
            s=SkyCycle::sunDir();
        }else{
            day=sky_isNight()?0.0f:1.0f;
            s=MakeVector(0.28f,0.94f,0.18f);
        }
        const float strength=0.55f*day*Weather::clearSky();
        if(strength>0.01f){
            // world -> eye: the view's 3x3 (column-major m), then unit length
            float e[3];
            for(int i=0;i<3;i++)e[i]=m[0*4+i]*s.x+m[1*4+i]*s.y+m[2*4+i]*s.z;
            const float en=sqrtf(e[0]*e[0]+e[1]*e[1]+e[2]*e[2]);
            if(en>1e-6f){ sun[0]=e[0]/en; sun[1]=e[1]/en; sun[2]=e[2]/en; sun[3]=strength; }
        }
    }
    const float upv[4]={up[0]/un,up[1]/un,up[2]/un,sun[3]>0?Ambient::clock()*0.7f:0.0f};   // AT.11: w = the sparkle clock
    R::setWaterTint(tint,sun,upv);
}
float Graphics::mistCullDistance(){
    const float s=Weather::fogScale();
    if(!(s<1.0f))return 0.0f;
    return (P_ZFAR-30)*s;
}
void Graphics::setCameraFog(float zfar){
     zfar=190;
     changedFog=true;
     P_ZFAR=zfar;
}
// B.5a: user view distance. eden_view_distance is the Settings enum index (0 = Auto = the per-tier value
// below, 1..6 = 4..9 chunks of radius). The far plane is ZFAR-25, so ZFAR = blocks + 25. Draw-culling only:
// meshing and streaming are untouched. The cap is the meshed radius (~120 blocks), not the resident window.
// B.5b-2: the cap is eden_meshed_radius_blocks() (120 at the default radius 9, the old literal), and
// Auto follows a larger window out to it: at radius R the far plane sits at the meshed edge and the
// fog ends 5 blocks short of it, as the tier value 145 does at R9. A smaller window clamps it.
extern float eden_view_distance;
float Graphics::viewDistanceZFAR(float tierZfar){
    const int cap=eden_meshed_radius_blocks();
    int idx=(int)(eden_view_distance+.5f);
    if(idx<=0){
        if(T_RADIUS>T_RADIUS_DEFAULT)return cap+25>tierZfar?cap+25:tierZfar;
        return cap+25<tierZfar?cap+25:tierZfar;
    }
    int blocks=(idx+3)*CHUNK_SIZE;
    if(blocks>cap)blocks=cap;
    return blocks+25;
}
void Graphics::setZFAR(float zfar){

    
    if(LOW_MEM_DEVICE){
        zfar=90;
    }else if(LOW_GRAPHICS){
        zfar=115;
    }else{
        zfar=145;
    }
    zfar=viewDistanceZFAR(zfar);
    changedFog=true;
    if(zfar!=P_ZFAR){
        
        P_ZFAR=zfar;
        
    }
}
// The 2D brackets: the matrix stacks are R::'s, the blend/depth state the pen's (Texture2D's text and
// icons read it). R:: draws state their own (fillRect). The stock buffer unbinds and client-array
// enables fed only the shim's draws (G.5b-7).
void Graphics::kitState(){
    Pen::blend(true);
    Pen::blendFunc(R::B_ALPHA);
}
void Graphics::kitBegin(float w,float h){
    Pen::depthTest(false);
    kitState();
    R::begin2D(w, h, 1.0f);
}
void Graphics::kitEnd(){
    R::end2D();
    Pen::blend(false);
    Pen::depthTest(true);
}
void Graphics::prepareMenu(){
	R::setClearColor(R::Rgba{0.0f, 0.0f, 0.0f, 1.0f});
	R::clear(true, true);
    // The old code did glLoadIdentity on the projection, then the ortho: not a push, so the caller's
    // projection is replaced (the menu frame owns the whole screen).
    float w = SCREEN_WIDTH, h = SCREEN_HEIGHT;
    if(IS_IPAD){
        if(IS_RETINA){ w = SCREEN_WIDTH*2; h = SCREEN_HEIGHT*2; }
        else { w = IPAD_WIDTH; h = IPAD_HEIGHT; }
    }
    R::proj().loadIdentity();
    R::proj().ortho(0, w, 0, h, -1, 1);
    R::mv().loadIdentity();
	Pen::depthTest(false);   
    Pen::blend(true);
    Pen::blendFunc(R::B_ALPHA);
}
// (Stock ended with glMatrixMode(GL_PROJECTION); the engine has no current matrix mode since G.5b-7.)
void Graphics::endMenu(){

	Pen::blend(false);
    Pen::depthTest(true);
}

// G.5b-6: the opaque terrain bracket on R:: (Terrain::render is its only caller): no blend, depth test
// and write, cull, the world fog, and the blocks' x0.25 under the modelview. The stock version enabled
// the colour array and bound the atlas for the chunk draws, which now state those themselves.
void Graphics::beginTerrain(){
    const R::State st = { R::B_OFF, true, true, true, 0.0f, 0.0f };
    R::setState(st);
    changedFog=false;
    applyWorldFog();
    applyHeightFog();   // AT.5: before the x0.25 below (the plane is the camera's view)
    Clouds::applyShadow();   // AT.6: likewise (R::setCloudShadow maps eye space through the camera's view)
	R::mv().push();
	R::mv().scale(.25f,.25f,.25f);
    //glDisable(GL_DEPTH_TEST); 
	//glScalef(BLOCK_SIZE, BLOCK_SIZE, BLOCK_SIZE);
	//glEnable(GL_ALPHA_TEST);
	//glAlphaFunc(GL_GREATER, 0.50f);
	
}
// Closes beginTerrain (and Terrain::render2's own opening). The stock code also left blend off for the
// HUD after it: the pen's (G.5b-7; its fog, colour-array and buffer lines fed only the shim).
void Graphics::endTerrain(){
	//glDisable(GL_ALPHA_TEST);
    R::mv().pop();
    R::fogOff();
    R::heightFogOff();   // AT.5
    R::cloudShadowOff(); // AT.6
    R::waterTintOff();   // AT.8
    Pen::blend(false);
	
}
void Graphics::endHud(){

    Pen::blend(false);
    Pen::depthTest(true);
   
    
   
    
	//glEnable(GL_LIGHTING);	
	R::end2D();
	//glEnableClientState(GL_NORMAL_ARRAY);
	
}

void Graphics::beginHud(){
    
    float w = SCREEN_WIDTH, h = SCREEN_HEIGHT;
    if(IS_IPAD){
        if(IS_RETINA){ w = SCREEN_WIDTH*2; h = SCREEN_HEIGHT*2; }
        else { w = IPAD_WIDTH; h = IPAD_HEIGHT; }
    }
    R::begin2D(w, h, 1.0f);                    // pushes both stacks; the HUD's depth range is -1..P_ZFAR
    R::proj().loadIdentity();
    R::proj().ortho(0, w, 0, h, -1, P_ZFAR);
    
    Pen::color(1.0, 1.0, 1.0, 1.0);
    
	Pen::depthTest(false);   
    Pen::blend(true);
    Pen::blendFunc(R::B_ONE);
	//glDisableClientState(GL_NORMAL_ARRAY);
}

// The vertical FOV setting. On web gluPerspective is intercepted by --wrap and fed this same
// value (port/src/seam/Settings.cpp); Apple's ld64 has no --wrap, so the native build reads it
// at the call site instead. Defined in Settings.cpp, which both targets compile; a from-scratch
// iOS build would supply its own stub (same convention as eden_report_load_failure).
extern float eden_fov_degrees;

void Graphics::prepareScene(){
	//glClearColor(.29f, .65f, .79f, 1.0f);
	R::clear(false, true);
	//glClear();
    // G.5b-6: gluPerspective's matrix (project.c, the same double math), straight onto R::proj().
    R::proj().loadIdentity();
    {
        const double fovy=eden_fov_degrees>0.0f?eden_fov_degrees:80, aspect=P_ASPECT_RATIO;
        const double zNear=P_ZNEAR, zFar=P_ZFAR-25;
        const double radians=fovy/2*M_PI/180, deltaZ=zFar-zNear, sine=sin(radians);
        if(deltaZ!=0&&sine!=0&&aspect!=0){
            const double cotangent=cos(radians)/sine;
            float m[16]={1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
            m[0]=cotangent/aspect;
            m[5]=cotangent;
            m[10]=-(zFar+zNear)/deltaZ;
            m[11]=-1;
            m[14]=-2*zNear*zFar/deltaZ;
            m[15]=0;
            R::proj().mult(m);
        }
    }
	//static int c=0;
    //if(c==0){c++;
       // printg("ar: %f, fovy:%f\n",P_ASPECT_RATIO,P_FOVY);}
	
	R::mv().loadIdentity();   // Camera::render's gluLookAt multiplies onto it (eden_glu_mult)
/*	if(World::getWorld->FLIPPED)
		glRotatef(90,0,0,1);
	else
		glRotatef(270,0,0,1);	*/
	
	
	
}
/*void Graphics::drawText(NSString* text,float x,float y){
	
	
		
	if(!thefont){
		thefont=[UIFont systemFontOfSize:12.0]	;
		[thefont retain];
	}
	

    Texture2D *textTex = new Texture2D(text,CGSizeMake(200,15.0),UITextAlignmentCenter,thefont);
	///NSLog(@"%@",textTex);
	textTex->drawAtPoint(CGPointMake(x, y),0 ,FALSE);
	
    delete textTex;
	
	
}*/
void Graphics::drawRect(float x1,float y1,float x2,float y2){
	float rectVertices[] = {
		x1	  ,y1	 ,-.5   ,
		x2    ,y1    ,-.5   ,		
		x1	  ,y2    ,-.5   ,
        x2 	  ,y2    ,-.5       
    };   
    // G.5b-7: TerrainGen2's dev view, inside World::renderFrame's JUST_TERRAIN_GEN bracket (texturing
    // off): an untextured pen draw.
    static const R::VertexFormat fmt = { 12, R::Attr{ R::A_F32, 3, 0 }, R::Attr{}, R::Attr{}, R::Attr{}, R::Attr{}, R::Attr{}, R::Attr{} };
    R::setState(Pen::state());
    R::Geometry g_ = {};
    g_.fmt = &fmt; g_.vmem = rectVertices; g_.itype = R::I_NONE; g_.first = 0; g_.count = 4; g_.prim = R::TRIANGLE_STRIP;
    R::draw2D(g_, 0, Pen::rgba());
}
// Untextured filled rect on R::draw2D, stating everything it needs (R4): alpha blend, no depth, no cull.
// Same quad as drawRect (z -.5, strip order); the colour is the tint, not the shim's current colour.
void Graphics::fillRect(float x1,float y1,float x2,float y2,float r,float g,float b,float a){
    static const R::VertexFormat fmt = { 12, R::Attr{ R::A_F32, 3, 0 }, R::Attr{}, R::Attr{}, R::Attr{}, R::Attr{}, R::Attr{}, R::Attr{} };
    const float v[] = { x1,y1,-.5f,  x2,y1,-.5f,  x1,y2,-.5f,  x2,y2,-.5f };
    R::State st = { R::B_ALPHA, false, false, false, 0.0f, 0.0f };
    R::setState(st);
    R::Geometry g_ = {};
    g_.fmt = &fmt; g_.vmem = v; g_.itype = R::I_NONE; g_.first = 0; g_.count = 4; g_.prim = R::TRIANGLE_STRIP;
    R::Rgba tint = { r, g, b, a };
    R::draw2D(g_, 0, tint);
}
void Graphics::fillScreen(float r,float g,float b,float a){
    float w = SCREEN_WIDTH, h = SCREEN_HEIGHT;
    if(IS_IPAD){
        if(IS_RETINA){ w = SCREEN_WIDTH*2; h = SCREEN_HEIGHT*2; }
        else { w = IPAD_WIDTH; h = IPAD_HEIGHT; }
    }
    fillRect(0,0,w,h,r,g,b,a);
}
// G.5b-4: the bracket of Firework::render and Player::render (its only callers), whose cubes are R::
// draws now (drawFirework, drawCube). They draw blended, depth-tested and culled, with fog off (the
// effects pass always runs after endTerrain). The stock glEnable(GL_BLEND)/glBindBuffer pairs left the
// shim's state as they found it, so nothing replaces them.
void Graphics::startPreview(){
    const R::State st = { R::B_ALPHA, true, true, true, 0.0f, 0.0f };
    R::setState(st);
    R::fogOff();
}
void Graphics::endPreview(){
}
void Graphics::drawSkybox(){
	
	Pen::color(1.0, 1.0, 1.0, 1.0f);
	Pen::depthTest(false);
	
	Pen::cull(false);
	/*Resources* res=[Resources getResources];
	//front face
	//back face
	//left face
	//right face
	//bot face
	//top face
	//glBufferSubData(GL_ARRAY_BUFFER,0,sizeof(vertexStruct)*CUBE_VERTICES,vertices);
	glEnable(GL_TEXTURE_2D);
	 glTranslatef(-0.5f, -0.5f, -0.5f);	
	
	for(int f=0;f<6;f++){	
		if(f==0)
			//glBindTexture(GL_TEXTURE_2D, [res getTex:ICO_SKYFRONT].name);
			glBindTexture(GL_TEXTURE_2D, [res getTex:ICO_SKYSIDE].name);
		if(f==1)
			//glBindTexture(GL_TEXTURE_2D, [res getTex:ICO_SKYBACK].name);
			glBindTexture(GL_TEXTURE_2D, [res getTex:ICO_SKYSIDE].name);
		if(f==2)
			//glBindTexture(GL_TEXTURE_2D, [res getTex:ICO_SKYRIGHT].name);
			glBindTexture(GL_TEXTURE_2D, [res getTex:ICO_SKYSIDE].name);
		if(f==3)
			//glBindTexture(GL_TEXTURE_2D, [res getTex:ICO_SKYLEFT].name);
			glBindTexture(GL_TEXTURE_2D, [res getTex:ICO_SKYSIDE].name);
		if(f==4)
			glBindTexture(GL_TEXTURE_2D, [res getTex:ICO_SKYBOT].name);
		if(f==5)
			glBindTexture(GL_TEXTURE_2D, [res getTex:ICO_SKYTOP].name);
						  
			
						  
		glVertexPointer(3, GL_FLOAT, sizeof(vertexStruct), (void*)offsetof(vertexStruct,position)
						);	
						  
						  
						  
		glTexCoordPointer(2, GL_FLOAT,  sizeof(vertexStruct),  (void*)offsetof(vertexStruct,texs)
						 );
						  
						  
						  
						  
						
						  
						  
						 
					  
		 glDrawArrays(GL_TRIANGLES, f*6, 6);	
						  
						  			
	}*/
	
	
	Pen::cull(true);
	Pen::depthTest(true);
	
}

extern uint8_t blockColor[BLOCK_IDS][3];
extern int blockTypeFaces[BLOCK_IDS][6];
extern Vector colorTable[256];
//extern int16_t cubeShortVertices[];
extern int16_t ramp2ShortVertices[3*6*6];
extern int16_t ramp3ShortVertices[3*6*6];
extern int16_t ramp4ShortVertices[3*6*6];
extern const int16_t ramp1ShortVertices[];
extern int16_t side2ShortVertices[3*6*6];
extern int16_t side3ShortVertices[3*6*6];
extern int16_t side4ShortVertices[3*6*6];
extern const int16_t side1ShortVertices[];
extern const float cubeNormals[36*3];

void Graphics::drawFirework(float x,float y,float z, int color, float scale, float rot){
    // G.5b-4: an R::drawLit (Firework::render sets the light). The texture and colour also go to the pen,
    // as 2.1 left them for the HUD after the effects pass (G.5b-7).
        Pen::texture(Resources::getResources->atlas->name);
  
	Resources* res=Resources::getResources;
	
    
	for(int f=0;f<6;f++){	
        int bf=blockTypeFaces[TYPE_CLOUD][f];
          CGPoint tp;
        tp=res->getBlockTex(bf);
        for(int v=0;v<6;v++){
			int v_idx=f*6+v;
			vertices[v_idx].texs[0]=cubeTexture[v_idx*2];
			vertices[v_idx].texs[1]=cubeTexture[v_idx*2+1]*tp.y+tp.x;
            
            for(int coord=0;coord<3;coord++){             
                 vertices[v_idx].position[coord]=cubeShortVertices[v_idx*3+coord]-.5f;                    
               
                
            } 
            Vector vc=rotateVertice(MakeVector(rot,rot,0),MakeVector( vertices[v_idx].position[0],
                                                                        vertices[v_idx].position[1],
                                                                       vertices[v_idx].position[2]));
            vertices[v_idx].position[0]=vc.x*scale+.5f;
            vertices[v_idx].position[1]=vc.y*scale+.5f;
            vertices[v_idx].position[2]=vc.z*scale+.5f;
            Vector vn=MakeVector(cubeNormals[v_idx*3],cubeNormals[v_idx*3+1],cubeNormals[v_idx*3+2]);
            
            vn=rotateVertice(MakeVector(rot,rot,0),vn);
            vertices[v_idx].normal[0]=vn.x;
            vertices[v_idx].normal[1]=vn.y;
            vertices[v_idx].normal[2]=vn.z;

            
       	}
    }
   
  
    R::mv().push();
    if(color==0){
           Pen::colorub(blockColor[TYPE_FIREWORK][0], blockColor[TYPE_FIREWORK][1], blockColor[TYPE_FIREWORK][2], 255);
    }else{   
        Vector vclr=colorTable[color];       
        Pen::color(vclr.x, vclr.y, vclr.z, 1.0f);
    }
    const R::Rgba tint = Pen::rgba();   // the colour just set (the pen converts glColor4ub as the shim did)
     
              
             
        
        
  
    // G.5b-4: streamed from `vertices` (was glBufferSubData into vertexBuffer + pointers).
    const R::Geometry g = { &R::kFmtCube, 0, vertices, 0, nullptr, R::I_NONE, 0, CUBE_VERTICES, R::TRIANGLES };
	
    
	
	
	
	
    //glScalef(scale,scale,scale);

	R::mv().translate(x,y,z); 
    
       R::drawLit(g, Resources::getResources->atlas->name, tint);	
	
    
	R::mv().pop();
    
}
void Graphics::drawCube(float x,float y,float z,int type,int buildsize){
    // printg("drawCube: %f, %f, %f\n",x,y,z);
    // G.5b-4: an R::drawWorld inside Graphics::startPreview's state (the caller may turn culling off).
    // The texture and colour also go to the pen, as 2.1 left them for the HUD (G.5b-7).
    const R::Tex cubeTex = !(blockinfo[type]&IS_ATLAS2) ? Resources::getResources->atlas->name
                                                         : Resources::getResources->atlas2->name;
    if(!(blockinfo[type]&IS_ATLAS2))
        Pen::texture(Resources::getResources->atlas->name);
    else
        Pen::texture(Resources::getResources->atlas2->name);
    
    if(type==TYPE_ICE_RAMP1||type==TYPE_STONE_RAMP1||type==TYPE_WOOD_RAMP1||type==TYPE_SHINGLE_RAMP1)
    {
        type=getRampType(x,z,y,type);
        
        
    }
 
/*	1, 1, 	//4
	0, 1, 	//3
	1,0,	//7  A front
	0,0, 	//8  B front
	1,1,	//5  C bottom
	0, 1, 	//3  D right
	1, 0,	//1  E right
	1, 1, 	//4  F top
	0, 0,	//2  G top
	1,0, 	//7  H left
	0,1,	//6  I left
	1,1,	//5  J bottom
	0, 0,	//2  K back
	1, 0 	//1  L back*/
    Resources* res=Resources::getResources;
	bool coloring=false;
    if(trait_paintArtCursor(type)){   // K.2-1
        if(!World::getWorld->hud->block_paintcolor)
        coloring=true;
    }
    int btype=World::getWorld->terrain->getLand(x,z,y);
  //  if(buildsize==0)btype=getCustomc(x,z,y);
    if(btype!=TYPE_NONE){
       
       // type=btype;
       // coloring=FALSE;
    }else{
        btype=type;
        
    }
   // int cc=World::getWorld->hud.paintColor;
  //  Vector clr=colorTable[cc];
    Vector center=MakeVector(0,0,0);
    int count=0;
	for(int f=0;f<6;f++){	
        int bf=blockTypeFaces[type][f];
        //if(btype!=TYPE_NONE){
        //    bf=blockTypeFaces[TYPE_CLOUD][f];
       // }
        if(coloring){
            
            
            bf=trait_unpaintedTwin(bf);   // K.2-1
            
            
        }
        CGPoint tp;
        tp=res->getBlockTex(bf);
				for(int v=0;v<6;v++){
			int v_idx=f*6+v;
			vertices[v_idx].texs[0]=cubeTexture[v_idx*2];
			vertices[v_idx].texs[1]=cubeTexture[v_idx*2+1]*tp.y+tp.x;
            
            for(int coord=0;coord<3;coord++){
                if(btype>=TYPE_STONE_SIDE1&&btype<=TYPE_ICE_SIDE4){
                    if(btype%4==0){
                        vertices[v_idx].position[coord]=side1ShortVertices[v_idx*3+coord];
                    }else if((btype+1)%4==0){
                        vertices[v_idx].position[coord]=side2ShortVertices[v_idx*3+coord]; 
                    }else if((btype+2)%4==0){
                        vertices[v_idx].position[coord]=side3ShortVertices[v_idx*3+coord]; 
                    }else if((btype+3)%4==0){
                        vertices[v_idx].position[coord]=side4ShortVertices[v_idx*3+coord]; 
                    }
                    
                }else if(btype>=TYPE_STONE_RAMP1&&btype<=TYPE_ICE_RAMP4){
                    if(btype%4==0){
                        vertices[v_idx].position[coord]=ramp1ShortVertices[v_idx*3+coord];
                    }else if((btype+1)%4==0){
                        vertices[v_idx].position[coord]=ramp2ShortVertices[v_idx*3+coord]; 
                    }else if((btype+2)%4==0){
                        vertices[v_idx].position[coord]=ramp3ShortVertices[v_idx*3+coord]; 
                    }else if((btype+3)%4==0){
                        vertices[v_idx].position[coord]=ramp4ShortVertices[v_idx*3+coord]; 
                    }                
                }
                else{
                    vertices[v_idx].position[coord]=cubeShortVertices[v_idx*3+coord];                    
                }
                
            }
            if(btype>=TYPE_STONE_RAMP1&&btype<=TYPE_ICE_SIDE4){
                if(absf(vertices[v_idx].position[0])>2)continue;
                count++;
                center.x+=vertices[v_idx].position[0];
                center.y+=vertices[v_idx].position[1];
                center.z+=vertices[v_idx].position[2];
            }			
            
       	}
    }
    center.x/=count;
    center.y/=count;
    center.z/=count; 
    //NSLog(@"count:%d",count);
     if(btype>=TYPE_STONE_RAMP1&&btype<=TYPE_ICE_SIDE4)
    for(int i=0;i<CUBE_VERTICES;i++){
        vertices[i].position[0]-=center.x;
        vertices[i].position[1]-=center.y;
        vertices[i].position[2]-=center.z;
    }
        //if(type>=TYPE_BLANK_RED&&type<=TYPE_BLANK_PINK){
        //	glColor4ub(blockColor[type][0], blockColor[type][1], blockColor[type][2], 255/2);
        //}else{
    R::mv().push();
    if(!coloring){
        if(World::getWorld->hud->block_paintcolor&&(World::getWorld->hud->mode==MODE_BUILD)){
            Vector vclr=colorTable[World::getWorld->hud->block_paintcolor];
            
            if(type==TYPE_CLOUD)
                Pen::color(vclr.x, vclr.y, vclr.z, 1.0f/6);
            else{            
                Pen::color(vclr.x, vclr.y, vclr.z, 1.0f/2);
            }
        }
        else{
        
        if(type==TYPE_CLOUD)
            Pen::colorub(blockColor[type][0], blockColor[type][1], blockColor[type][2], 255/6);
        else{            
            Pen::colorub(blockColor[type][0], blockColor[type][1], blockColor[type][2], 255/2);
        }
        }
    }else{
        if(type==TYPE_CLOUD)
            Pen::color(1.0f,1.0f,1.0f,0.15f);
        else {
            
            Pen::color(1.0f,1.0f,1.0f,0.5f);
        }
    }
   // glColor4f(1.0f,0.0f,0.0f,1.0f);
	//}
    const R::Rgba tint = Pen::rgba();   // the colour just set (the pen converts glColor4ub as the shim did)
    // G.5b-4: streamed from `vertices` (was glBufferSubData into vertexBuffer + pointers).
    const R::Geometry g = { &R::kFmtCube, 0, vertices, 0, nullptr, R::I_NONE, 0, CUBE_VERTICES, R::TRIANGLES };
	
	


	
  
	
	
	
	
	if(buildsize==0){
        R::mv().scale(.5f,.5f,.5f);
    }
	R::mv().translate(x, y, z); 
   // printg("working tanslate: %f,%f,%f\n",x,y,z);
    if(buildsize==2){
        R::mv().scale(2,2,2);
    }
    x=(int)x;
    y=(int)y;
    z=(int)z;
    // H.12: buildsize -1 marks Player::render's fallback body, so a cloud-block build preview
    // in third person keeps its 1x1x1 shape.
    if(THIRD_PERSON&&type==TYPE_CLOUD&&buildsize==-1){
        R::mv().scale(.6666f, 1.85f, .66666f);
    }else{
        R::mv().scale(BLOCK_SIZE, BLOCK_SIZE, BLOCK_SIZE);
        if(btype!=TYPE_NONE){
            // NSLog(@"drawing on something");
            if(btype>=TYPE_STONE_RAMP1&&btype<=TYPE_ICE_SIDE4){
                R::mv().translate(center.x,center.y,center.z);
                R::mv().scale(1.01f,1.01f,1.01f);
                
                
                   
                
            }else{
                R::mv().translate(-.005f,-.005f,-.005f);
                R::mv().scale(1.01f,1.01f,1.01f);
            }
            //glDisable(GL_DEPTH_TEST); 
        }
    }
    R::drawWorld(g, cubeTex, tint);	
	
    
	R::mv().pop();
	
	
	
}

