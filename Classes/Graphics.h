//
//  Graphics.h
//  prototype
//
//  Created by Ari Ronen on 10/11/10.
//  Copyright 2010 __MyCompanyName__. All rights reserved.
//
#ifndef Eden_Graphics_h
#define Eden_Graphics_h



#include <stdint.h>

#include "Texture2D.h"
#include "glu.h"
#include "Resources.h"
#include "Renderer.h"

// G.5b-7: the 2D pen. The HUD, the menus and the GLW kit are written as ES 1.1 immediate-mode code:
// they set a colour, a blend mode, depth and a texture, then draw text and icons through Texture2D and
// a few quads of their own. Those calls used to land in the GL shim's logical state, which Texture2D
// read back (R::legacyState/legacyColor). The shim is gone, so the engine keeps that state here, set
// by the same calls in the same order (each one names the ES 1.1 call it replaced). Only pen draws
// read it: Texture2D's quads, Pen::strip and the two preview cubes' tint. R:: draws never do.
namespace Pen {
void color(float r,float g,float b,float a);                                   // glColor4f
void colorub(unsigned char r,unsigned char g,unsigned char b,unsigned char a); // glColor4ub
void blend(bool on);                    // glEnable / glDisable(GL_BLEND)
void blendFunc(R::Blend f);             // glBlendFunc: B_ALPHA (SA,1-SA), B_ADD (SA,1), B_ONE (1,1)
void depthTest(bool on);                // GL_DEPTH_TEST
void depthWrite(bool on);               // glDepthMask
void cull(bool on);                     // GL_CULL_FACE
void texture(R::Tex t);                 // glBindTexture(GL_TEXTURE_2D, t)
void textured(bool on);                 // glEnable / glDisable(GL_TEXTURE_2D)
R::State state();                       // what a pen draw passes to R::setState
R::Rgba rgba();
// glVertexPointer(3, float) + glTexCoordPointer(2, float) + glDrawArrays(GL_TRIANGLE_STRIP, first,
// count) with the pen's state, colour and (when textured) texture. count <= 16.
void strip(const float* xyz,const float* st,int first,int count);
}


class Graphics{
public:
    static void setZFAR(float zfar);
    static float viewDistanceZFAR(float tierZfar);
    static void setCameraFog(float zfar);
    static void initGraphics();
    //static void drawText(NSString* text,float x,float y);
    static void drawRect(float x1,float y1,float x2,float y2);
    // G.5b-1: the 2D screen bracket and an untextured filled rect on R:: (Renderer.h). `kitBegin` is
    // GLW::begin2D's body: ortho (0..w, 0..h, -1..1) over an identity modelview, depth off, alpha blend.
    static void fillRect(float x1,float y1,float x2,float y2,float r,float g,float b,float a);
    // G.5b-2: a full-screen fillRect in the engine's 2D point space (the HUD/menu fade, liquid and flash overlays).
    static void fillScreen(float r,float g,float b,float a);
    static void kitBegin(float w,float h);
    static void kitEnd();
    static void kitState();
    static void prepareScene();
    static void prepareMenu();
    static void endMenu();
    static void drawFirework(float x,float y,float z, int color, float scale,float rot);
    static void drawCube(float x,float y,float z,int type,int buildsize);
    static void startPreview();
    static void endPreview();
    // G.5b-6: the world fog (colour from Terrain; start/end from P_ZFAR), stated with R::setFog.
    static void setFogColor(const float clr[4]);
    static void applyWorldFog();
    static void beginTerrain();
    static void endTerrain();
    static void beginHud();
    static void drawSkybox();
    
    static void endHud();
    
};



typedef struct _vertexStruct
{
    float position[3];
    float normal[3];
    uint8_t colors[4];  
	float texs[2];
	  
} vertexStruct;
static_assert(sizeof(vertexStruct) == 36, "vertexStruct is read by R::kFmt* (render_formats.cpp) at fixed strides");

typedef struct _vertexStructSmall
{
    int16_t position[3];
    int16_t pad;
    uint8_t colors[4];  
	int16_t texs[2];
	  
} vertexStructSmall;
static_assert(sizeof(vertexStructSmall) == 16, "vertexStructSmall is read by R::kFmt* (render_formats.cpp) at fixed strides");

typedef struct _vertexpStruct
{
    float position[3];
    uint8_t colors[4]; 
    float size[1];
	    
} vertexpStruct;
static_assert(sizeof(vertexpStruct) == 20, "vertexpStruct is read by R::kFmt* (render_formats.cpp) at fixed strides");

typedef struct _vertexpBreak
{
    float position[3];
    float normal[3];
    uint8_t colors[4]; 
	int16_t texs[2];
	    
} vertexpBreak;
static_assert(sizeof(vertexpBreak) == 32, "vertexpBreak is read by R::kFmt* (render_formats.cpp) at fixed strides");

#endif
