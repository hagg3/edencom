//
//  BlockIcon.cpp
//  UX.8-1: lifted from Hud::renderBlockAndBorder (stock maths, G.5b-7 Pen::strip draws). The build
//  button passes its own stock Geo, so its pixels did not change.
//

#include "BlockIcon.h"
#include "Globals.h"
#include "Graphics.h"
#include "Resources.h"
#include "BlockTraits.h"
#include "BlockRegistry.h"
#include <cmath>

extern Vector colorTable[256];
extern int16_t side1ShortVertices[3*6*6], side2ShortVertices[3*6*6], side3ShortVertices[3*6*6], side4ShortVertices[3*6*6];
extern int16_t ramp1ShortVertices[3*6*6], ramp2ShortVertices[3*6*6], ramp3ShortVertices[3*6*6], ramp4ShortVertices[3*6*6];
extern int16_t side1Texture[2*6*6], side2Texture[2*6*6], side3Texture[2*6*6], side4Texture[2*6*6];
extern int16_t ramp1Texture[2*6*6], ramp2Texture[2*6*6], ramp3Texture[2*6*6], ramp4Texture[2*6*6];

namespace BlockIcon {

bool isFlat(int type){
    return type==TYPE_FLOWER||type==TYPE_GOLDEN_CUBE||type==TYPE_DOOR_TOP||type==TYPE_PORTAL_TOP;
}

// Texture2D::drawInRect takes point space and multiplies by SCALE_* on an iPad-scale UI; the rest of
// BlockIcon is in the pass's raw units, so undo that first. The picture fills the (square) box.
void flat(int type,int colour,CGRect r){
    if(IS_IPAD){
        r.origin.x/=SCALE_WIDTH; r.size.width/=SCALE_WIDTH;
        r.origin.y/=SCALE_HEIGHT; r.size.height/=SCALE_HEIGHT;
    }
    Pen::color(1.0f,1.0f,1.0f,1.0f);
    Resources::getResources->getPaintedTex(type,colour)->drawInRect(r);
}

static bool is_ramp(int type){ return type>=TYPE_STONE_RAMP1&&type<=TYPE_ICE_RAMP4; }
static bool is_side(int type){ return type>=TYPE_STONE_SIDE1&&type<=TYPE_ICE_SIDE4; }


// Everything a picture needs of a block: a stock table row or a mod BlockDef (UX.8-2).
struct Look {
    int type;            // stock/palette row for the special cases; 0 for a mod block (none apply)
    int info;
    int faces[6];        // blockTypeFaces row: front, back, left, right, bottom, top
    uint8_t color[3];
    bool artIcon;        // the art carries its own colour (paint_art ICON): drawn untinted
};

static Look stockLook(int type){
    Look l;
    l.type=type; l.info=blockinfo[type];
    for(int f=0;f<6;f++) l.faces[f]=blockTypeFaces[type][f];
    for(int c=0;c<3;c++) l.color[c]=blockColor[type][c];
    l.artIcon=trait_paintArtIcon(type);   // K.2-1; FT.1: weeds (GRASS2) take blockColor like the world mesh
    return l;
}

// The atlas tile (a getBlockTex point) for face slot `f`. TNT, firework, ladder and block-TNT show one
// picture on every face (stock); the others use their own face tile, the unpainted twin when unpainted.
static CGPoint tileFor(const Look& l,int colour,int f){
    Resources* res=Resources::getResources;
    const int type=l.type;
    if(type==TYPE_TNT) return res->getBlockTex(colour!=0?TEX_TNT_SIDE:TEX_TNT_SIDE_COLOR);
    if(type==TYPE_FIREWORK) return res->getBlockTex(TEX_FIREWORK);
    if(type==TYPE_LADDER) return res->getBlockTex(l.faces[3]);
    if(l.info&IS_BLOCKTNT) return res->getBlockTex(TEX_BLOCKTNT);
    const int bf=l.faces[f];
    if(colour==0){
        if(type==TYPE_BRICK) return res->getBlockTex(TEX_BRICK_COLOR);
        if(IS_NEWBLOCK_TEX(bf)) return res->getBlockTex(NEWBLOCK_COLOR_TEX(bf));   // D.2t
    }
    return res->getBlockTex(bf);
}

// Face shades: the top is lit, the front a little down, the side the darkest (the world mesher's
// proportions, flat).
static const float kShadeTop=1.0f, kShadeFront=0.84f, kShadeSide=0.66f;

static void setColor(const Look& l,int colour,float shade){
    if(colour!=0){
        const Vector c=colorTable[colour];
        Pen::color(c.x*shade,c.y*shade,c.z*shade,1.0f);
    }else if(l.artIcon){
        Pen::color(shade,shade,shade,1.0f);
    }else{
        Pen::color(l.color[0]/255.0f*shade,l.color[1]/255.0f*shade,l.color[2]/255.0f*shade,1.0f);
    }
}

// A ramp or a side block, from the world mesher's own triangles (Geometry.cpp), projected the way the
// cube is (x mirrored: the front face is seen from outside, so +x runs left) and back-face culled by
// screen winding, so each of the four orientations looks like the block that gets placed.
static void wedge(const Look& l,int type,int colour,const Geo& g){
    const int16_t *V,*T;
    const int k=type%4;   // 24, 28, ... and 40, 44, ... are all ≡ 0 mod 4: orientation 1..4 = k 0..3
    if(is_ramp(type)){
        V=k==0?ramp1ShortVertices:k==1?ramp2ShortVertices:k==2?ramp3ShortVertices:ramp4ShortVertices;
        T=k==0?ramp1Texture:k==1?ramp2Texture:k==2?ramp3Texture:ramp4Texture;
    }else{
        V=k==0?side1ShortVertices:k==1?side2ShortVertices:k==2?side3ShortVertices:side4ShortVertices;
        T=k==0?side1Texture:k==1?side2Texture:k==2?side3Texture:side4Texture;
    }
    const float span=g.size-g.off;
    for(int tri=0;tri<12;tri++){
        const int v0=tri*3;                       // 3 vertices per triangle, 6 per face
        if(V[v0*3+1]<0||V[v0*3+4]<0||V[v0*3+7]<0) continue;   // -9: the face has no triangle here
        float sx[3],sy[3],wx[3],wy[3],wz[3];
        for(int i=0;i<3;i++){
            wx[i]=V[(v0+i)*3]; wy[i]=V[(v0+i)*3+1]; wz[i]=V[(v0+i)*3+2];
            sx[i]=g.x+g.off+g.offx+(1.0f-wx[i])*span+wz[i]*g.depth;
            sy[i]=g.y+g.off+g.offy+wy[i]*span+wz[i]*g.depth;
        }
        const float cross=(sx[1]-sx[0])*(sy[2]-sy[0])-(sy[1]-sy[0])*(sx[2]-sx[0]);
        if(cross<=0) continue;
        const float ax=wx[1]-wx[0],ay=wy[1]-wy[0],az=wz[1]-wz[0],bx=wx[2]-wx[0],by=wy[2]-wy[0],bz=wz[2]-wz[0];
        const float ny=az*bx-ax*bz, nz=ax*by-ay*bx;   // world normal's y and z (outward by the table's winding)
        const float len=sqrtf((ay*bz-az*by)*(ay*bz-az*by)+ny*ny+nz*nz);
        const float uy=len>0?ny/len:0, uz=len>0?nz/len:0;
        const float shade=uy>0.9f?kShadeTop:uy>0.1f?(kShadeTop+kShadeFront)*0.5f:uz<-0.5f?kShadeFront:kShadeSide;
        const CGPoint tp=tileFor(l,colour,tri/2);
        float xyz[9],st[6];
        for(int i=0;i<3;i++){
            xyz[i*3]=sx[i]; xyz[i*3+1]=sy[i]; xyz[i*3+2]=0;
            st[i*2]=T[(v0+i)*2]; st[i*2+1]=tp.x+T[(v0+i)*2+1]*tp.y;
        }
        setColor(l,colour,shade);
        Pen::strip(xyz,st,0,3);
    }
}

static void cubeLook(const Look& l,int type,int colour,const Geo& g,int flags){
    Resources* res=Resources::getResources;
    if(l.info&IS_ATLAS2){
        Pen::texture(res->atlas2->name);
    }else{
        Pen::texture(res->atlas->name);
    }
    if(!(flags&FRONT_ONLY)&&(is_ramp(type)||is_side(type))){
        wedge(l,type,colour,g);
        Pen::color(1.0f,1.0f,1.0f,1.0f);
        return;
    }
    const float off=g.off, size=g.size, depth=g.depth, offx=g.offx, offy=g.offy;
    float bvertices[] = {
        off+offx,			off+offy,							0,
        size+offx,		off+offy,							0,
        off+offx,			size+offy,                        0,
        size+offx,		size+offy,                        0,

        off+offx,			size+offy,                        0,
        size+offx,		size+offy,                        0,
        off+depth+offx,			size+depth+offy,          0,
        size+depth+offx,		size+depth+offy,              0,

        size+offx,			off+offy,							0,
        size+depth+offx,		off+depth+offy,							0,
        size+offx,			size+offy,                        0,
        size+depth+offx,		size+depth+offy,                        0,
    };
    float vertices[3*4*3];
    for(int i=0;i<3*4*3;i+=3){
        vertices[i]=bvertices[i]+g.x;
        vertices[i+1]=bvertices[i+1]+g.y;
        vertices[i+2]=bvertices[i+2];
    }
    // face slots: 0 front, 5 top, 3 side (the strip order is front, top, side)
    const int slot[3]={0,5,3};
    const float shade[3]={kShadeFront,kShadeTop,kShadeSide};
    // Pen::strip indexes the vertex and the st arrays by the same `first`: one 12-vertex st array,
    // quad i (front, top, side) taking tile ti.
    float st[3*4*2];
    auto quad=[&](int i,CGPoint tp){
        const float q[]={0,tp.y+tp.x, 1,tp.y+tp.x, 0,tp.x, 1,tp.x};
        for(int k=0;k<8;k++) st[i*8+k]=q[k];
    };
    if(flags&FRONT_ONLY){
        quad(0,tileFor(l,colour,0));
        setColor(l,colour,1.0f);
        Pen::strip(vertices,st,0,4);
    }else if(l.info&IS_BLOCKTNT){
        // Front and side are the TNT wrapper; the top shows what the block turns into.
        const CGPoint wrap=tileFor(l,colour,0);
        quad(0,wrap); quad(2,wrap);
        Pen::color(kShadeFront,kShadeFront,kShadeFront,1.0f);
        Pen::strip(vertices,st,0,4);
        Pen::color(kShadeSide,kShadeSide,kShadeSide,1.0f);
        Pen::strip(vertices,st,8,4);

        const int subtype=blockTntMap[type];
        CGPoint tp;
        if(subtype==TYPE_TNT||subtype==TYPE_FIREWORK||subtype==TYPE_LADDER){
            tp= res->getBlockTex(blockTypeFaces[subtype][3]);
            if(subtype==TYPE_TNT){
                tp= res->getBlockTex(TEX_TNT_SIDE_COLOR);
            }
        }else
            tp= res->getBlockTex(blockTypeFaces[subtype][5]);
        if(subtype==TYPE_GLASS||subtype==TYPE_WEAVE){
            tp= res->getBlockTex(TEX_CLOUD);
        }else if(subtype==TYPE_WATER||subtype==TYPE_LAVA){
            tp= res->getBlockTex(TEX_DIRT);
        }
        quad(1,tp);
        if (colour!=0) {
            const Vector clr=colorTable[colour];
            Pen::color(clr.x*kShadeTop,clr.y*kShadeTop,clr.z*kShadeTop,1.0f);
        }else{
            Pen::colorub(blockColor[subtype][0], blockColor[subtype][1], blockColor[subtype][2], 255);
        }
        Pen::strip(vertices,st,4,4);
    }else{
        for(int i=0;i<3;i++){
            quad(i,tileFor(l,colour,slot[i]));
            setColor(l,colour,shade[i]);
            Pen::strip(vertices,st,i*4,4);
        }
    }
    Pen::color(1.0f,1.0f,1.0f,1.0f);
}

void cube(int type,int colour,const Geo& g,int flags){
    cubeLook(stockLook(type),type,colour,g,flags);
}

void cubeMod(const std::string& name,int colour,const Geo& g){
    const BlockRegistry::BlockDef* d=BlockRegistry::findDef(name);
    if(!d) return;
    Look l;
    l.type=0; l.info=d->info;
    for(int f=0;f<6;f++) l.faces[f]=d->faces[f];
    for(int c=0;c<3;c++) l.color[c]=d->color[c];
    l.artIcon=(d->paintArt&ART_ICON)!=0;
    cubeLook(l,0,colour,g,0);
}

// A cube filling the box, in whole units, centred: front face 2/3, depth 1/3 of the box (the build button
// keeps its own stock Geo).
Geo fit(CGRect r){
    const float box=fminf(r.size.width,r.size.height);
    Geo g;
    g.off=0;
    // Cabinet-style: the receding edge is half the front's (the stock 24:27 read stretched toward NE).
    g.depth=floorf(box/3.0f);
    g.size=floorf(box)-g.depth;
    g.offx=g.offy=0;
    g.cory=-roundf(g.size*3.0f/27.0f);
    g.x=roundf(r.origin.x+(r.size.width-(g.size+g.depth))*0.5f);
    g.y=roundf(r.origin.y+(r.size.height-(g.size+g.depth))*0.5f);
    return g;
}

void draw(int type,int colour,CGRect r,int flags){
    if(isFlat(type)) flat(type,colour,r);
    else cube(type,colour,fit(r),flags);
}

void drawMod(const std::string& name,int colour,CGRect r){
    cubeMod(name,colour,fit(r));
}

// ---- plain fills (raw pass units, alpha blended; Graphics::fillRect) ----
static void box(float x,float y,float w,float h,float rr,float gg,float bb,float a){
    Graphics::fillRect(x,y,x+w,y+h,rr,gg,bb,a);
}

void chip(int colour,CGRect r){
    if(colour<=0) return;
    const float box_=fminf(r.size.width,r.size.height);
    const float s=roundf(fmaxf(box_*0.34f,4.0f)), o=fmaxf(1.0f,roundf(box_/32.0f));
    const float x=r.origin.x+r.size.width-s, y=r.origin.y;
    const Vector c=colorTable[colour];
    box(x-o,y-o,s+o*2,s+o*2,0,0,0,0.85f);   // the outline
    box(x,y,s,s,c.x,c.y,c.z,1.0f);
    Pen::color(1.0f,1.0f,1.0f,1.0f);
}

void drawSwatch(int colour,CGRect r){
    const float u=fmaxf(1.0f,roundf(fminf(r.size.width,r.size.height)/20.0f));
    const float inset=roundf(fminf(r.size.width,r.size.height)*0.12f);
    const float x=r.origin.x+inset, y=r.origin.y+inset, w=r.size.width-inset*2, h=r.size.height-inset*2;
    const Vector c=colorTable[colour>0?colour:1];
    box(x,y,w,h,0.39f,0.39f,0.39f,1.0f);                       // keyline
    box(x+u,y+u,w-u*2,h-u*2,c.x,c.y,c.z,1.0f);                  // the paint
    box(x+u,y+h-u*2,w-u*2,u,1,1,1,0.25f);                       // top-edge highlight, as a bevel has
    box(x+u,y+u,w-u*2,u,0,0,0,0.25f);                           // and its shade
    Pen::color(1.0f,1.0f,1.0f,1.0f);
}

void drawEmpty(CGRect r){
    const float m=fminf(r.size.width,r.size.height);
    const float len=roundf(m*0.34f), th=fmaxf(2.0f,roundf(m*0.07f));
    const float cx=r.origin.x+r.size.width*0.5f, cy=r.origin.y+r.size.height*0.5f;
    box(roundf(cx-len*0.5f),roundf(cy-th*0.5f),len,th,0.7f,0.7f,0.7f,0.45f);
    box(roundf(cx-th*0.5f),roundf(cy-len*0.5f),th,len,0.7f,0.7f,0.7f,0.45f);
    Pen::color(1.0f,1.0f,1.0f,1.0f);
}

}
