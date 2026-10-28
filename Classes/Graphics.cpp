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

#define M_SQRT3 1.732050808
//static UIFont* thefont=NULL;//move this to resources sometime

#define CUBE_VERTICES 36
extern float SCREEN_WIDTH; 
extern float SCREEN_HEIGHT;
extern float P_ASPECT_RATIO;

extern const GLshort cubeTexture[CUBE_VERTICES*2] ;

extern const GLshort cubeShortVertices[CUBE_VERTICES*3] ;
/*
const static GLfloat skyboxVertices[]={
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


const static GLfloat cubeTexture12[] = {
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
const static GLfloat cubeVertices12[] = {
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

const static GLfloat cubeNormals12[] = {
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
void Graphics::applyWorldFog(){
    const float start=P_ZFAR-P_ZFAR/1.6f, end=P_ZFAR-30;
    R::setFog(s_fogColor, start, end);
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
	GLfloat rectVertices[] = {
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

extern const GLubyte blockColor[NUM_BLOCKS+1][3];
extern const int blockTypeFaces[NUM_BLOCKS+1][6];
extern Vector colorTable[256];
//extern GLshort cubeShortVertices[];
extern GLshort ramp2ShortVertices[3*6*6];
extern GLshort ramp3ShortVertices[3*6*6];
extern GLshort ramp4ShortVertices[3*6*6];
extern const GLshort ramp1ShortVertices[];
extern GLshort side2ShortVertices[3*6*6];
extern GLshort side3ShortVertices[3*6*6];
extern GLshort side4ShortVertices[3*6*6];
extern const GLshort side1ShortVertices[];
extern const GLfloat cubeNormals[36*3];

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
    if(type==TYPE_GRASS||type==TYPE_GRASS2||type==TYPE_GRASS3||type==TYPE_TNT||type==TYPE_BRICK||type==TYPE_VINE||type==TYPE_FIREWORK||(type>=TYPE_ORE_SAND&&type<=TYPE_ALGAE)){
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
            
            
            if(bf==TEX_GRASS_SIDE)
                bf=TEX_GRASS_SIDE_COLOR;
            else if(bf==TEX_TNT_SIDE)
                bf=TEX_TNT_SIDE_COLOR;
            else if(bf==TEX_TNT_TOP)
                bf=TEX_TNT_TOP_COLOR;
            else if(bf==TEX_BRICK)
                bf=TEX_BRICK_COLOR;
            else if(IS_NEWBLOCK_TEX(bf))
                bf=NEWBLOCK_COLOR_TEX(bf);   // D.2t
            
            
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

