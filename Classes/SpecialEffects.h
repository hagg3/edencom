//
//  SpecialEffects.h
//  prototype
//
//  Created by Ari Ronen on 11/2/10.
//  Copyright 2010 __MyCompanyName__. All rights reserved.
//
#ifndef Eden_SpecialEffects_h
#define Eden_SpecialEffects_h


#include "BlockBreak.h"
#include "Fire.h"

#include <OpenGLES/ES1/gl.h>
#include <OpenGLES/ES1/glext.h>
#include "Util.h"
#define pbuffer_size 10000

class SpecialEffects{
    
public:
    SpecialEffects();
    bool update(float etime);
    void addBlockBreak(int x, int z, int y,int type,int color);
    void addBlockExplode(int x, int z,int y,int type,int color);
    void addCreatureVanish(float x,float z,float y,int color,int type);
    int addFire(float x,float z,float y,int type,float life);
    int addSmoke(float x,float z,float y);
    void addFirework(float x,float z, float y,int color);
    void render();
    void updateFire(int idx,Vector pos);
    void removeFire(int ppid);
    void clearAllEffects();
    
    
private:
    BlockBreak* bb;
    Fire* fire;
};



void setParticle(Vector p,int pvbi);
int getPVBI();

#endif