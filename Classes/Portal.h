//
//  Portal.h
//  Eden
//
//  Created by Ari Ronen on 5/16/12.
//  Copyright (c) 2012 __MyCompanyName__. All rights reserved.
//
#ifndef Eden_Portal_h
#define Eden_Portal_h



#include <math.h>
#include "Util.h"

#include "World.h"
#include "Camera.h"
#include "Input.h"
#include "OpenGL_Internal.h"

#define MAX_PORTAL 1000
typedef struct _portal{
    int x,y,z,dir,color;
    
}sportal;

class Portal{
public:
    Portal();
    void addPortal(int x,int y,int z,int dir,int color);
    void paintPortal(int x,int y,int z,int color);
    void removePortal(int x,int y,int z);
    void removeAllPortals();
    Vector2 enterPortal(int x,int y,int z,Vector vel);
    // Nearest portal to `pos` (block centers), for the proximity-ambience distance fade -- returns
    // FALSE (leaving *outPos untouched) when the world has no portals at all.
    bool nearestPortal(Vector pos,Vector* outPos);
private:
   int n_portal;
   sportal portals[MAX_PORTAL];

    
};

#endif