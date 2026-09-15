//
//  Lighting.h
//  Eden
//
//  Created by Ari Ronen on 1/21/13.
//
//
#ifndef Eden_Lighting_h
#define Eden_Lighting_h


#import "Vector.h"



void calculateLighting();
BOOL calculateLightingSlice();   // budgeted per-frame form for the post-bulk-reload path
void calculateLightingSliceReset();
void addlight(int xx,int zz,int yy,float brightness,Vector color);



#endif