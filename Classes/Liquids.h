//
//  Liquids.h
//  Eden
//
//  Created by Ari Ronen on 7/15/11.
//  Copyright 2011 __MyCompanyName__. All rights reserved.
//
#ifndef Eden_Liquids_h
#define Eden_Liquids_h



#include "hashmap.h"
#include "Globals.h"


typedef struct wnode{
	int x,y,z;
    
    int spread;
	int feeders;	
	int flowType;
    block8 blockType;
    color8 color;
    int max;
    int flow;
    int heights[4];	
}WetNode;

typedef struct _pnode{
	int x,y,z;
   
	struct _pnode* next;
    struct _pnode* prev;
}PNode;

typedef struct _pnode2{
	int x,y,z;
    int type;
    int level;
    
	struct _pnode2* next;
    struct _pnode2* prev;
}PNode2;

class Liquids{
public:
    Liquids();
    bool update(float etime);
    void render();
    void clearLiquids();
    void addSource(int x,int z,int y);
    void removeSource(int x,int z,int y,int type);
    // B.5b-1b: drop every node in chunk column (cx,cz), which is about to leave the window (its slot
    // is overwritten by a column 288 blocks away). Returns how many. countOutsideWindow() is the
    // probe: nodes whose column is not resident, which must stay 0.
    int dropColumn(int cx,int cz);
    int countOutsideWindow();
    
};

// P.8: inline (were out-of-line calls, ~15 % of rebuild2 samples); bodies unchanged.
static inline int getLevel(int type){
    if(type==TYPE_LAVA1||type==TYPE_WATER1)return 1;
    if(type==TYPE_LAVA2||type==TYPE_WATER2)return 2;
    if(type==TYPE_LAVA3||type==TYPE_WATER3)return 3;
    if(type==TYPE_LAVA||type==TYPE_WATER)return 4;
    return 0;
}
static inline int getBaseType(int type){
    if(type==TYPE_WATER2||type==TYPE_WATER3||type==TYPE_WATER1)return TYPE_WATER;
    if(type==TYPE_LAVA2||type==TYPE_LAVA3||type==TYPE_LAVA1)return TYPE_LAVA;
    return type;
}
int genLevel(int type,int level);

#endif
