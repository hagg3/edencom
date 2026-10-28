//
//  Input.h
//  prototype
//
//  Created by Ari Ronen on 10/13/10.
//  Copyright 2010 __MyCompanyName__. All rights reserved.
//
#ifndef Eden_Input_h
#define Eden_Input_h



#include "Util.h"
// G.2b-1 (de-ObjC plan D10): a touch is an engine struct, not a UITouch. The seam (Input.cpp)
// keeps ONE heap EdenTouch per live pointer from begin to end/cancel, so `itouch::touch_id` stays a
// stable pointer and every `==` match below keeps its meaning (a real touch id 0 is valid and the
// synthetic ids are negative, so an int id could not stand in for "no touch").
struct EdenTouch {
	CGPoint location;   // engine points, top-left origin, y down (what -locationInView: returned)
	int phase;          // 0 began, 1 moved, 2 ended, 3 cancelled
	double timestamp;   // seconds
	long identity;      // the seam's per-pointer id: >= 0 a real pointer, < 0 keyboard/mouse synthetic
	bool isReal;        // came from a touchscreen pointer, not a synthetic HUD press (was -isRealTouch)
};
typedef struct{
	int mx,my;
	int pmx,pmy;
	int fx,fy;
	int inuse;
	int down;
	int moved;	
	bool movecam;
	int previewtype;
	Point3D preview;
	bool placeBlock;
	const EdenTouch* touch_id;
	float etime;
   
    float scalex;
    float scaley;
    int build_size;
    // Stage L (TouchLook.h): every event's motion, in float engine points, before the int
    // truncation above. Not on disk. mx/pmx/fx keep their stock meaning for every other consumer.
    float lfx,lfy;     // last raw position
    float ffx,ffy;     // where the touch went down
    float ldx,ldy;     // look motion not yet consumed (TouchLook zeroes it)
    bool looklatched;  // crossed the slop once: looks until release (D5)
}itouch;
#define MAX_TOUCHES 5
enum MSTATE {
	M_DOWN=1,
	M_RELEASE=2,
	M_NONE=0
};

class Input{
public:
    Input();
    itouch touches[MAX_TOUCHES];
    int scr_width;
    int scr_height;
    static Input* getInput();
    itouch* getTouches();
    void touchesBegan(const EdenTouch* const* touches,int n);
    void touchesMoved(const EdenTouch* const* touches,int n);
    void touchesEnded(const EdenTouch* const* touches,int n);
    void keyTyped(char key);
    void clearAll();
    // Re-reads scr_width/scr_height from the current SCREEN_WIDTH/SCREEN_HEIGHT. The constructor
    // calls it too; it exists separately because the point space is no longer fixed for the process
    // lifetime (web port audit D1/D4 — it is derived from the window aspect and a UI-scale setting).
    void screenMetricsChanged();
   void touchesCancelled(const EdenTouch* const* touches,int n);
    
};

#endif

