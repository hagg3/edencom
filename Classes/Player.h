//
//  Player.h
//  prototype
//
//  Created by Ari Ronen on 10/14/10.
//  Copyright 2010 __MyCompanyName__. All rights reserved.
//
#ifndef Eden_Player_h
#define Eden_Player_h


#include <math.h>
#include "Util.h"
#include "Terrain.h"
#include "World.h"
#include "Camera.h"
#include "Input.h"
#include "OpenGL_Internal.h"
class World;

// Player::takeDamage cause, picks the death sound (H.6).
enum DEATH_CAUSES{DEATH_NONE=0,DEATH_LAVA=1,DEATH_CREATURE=2,DEATH_TNT=3};
// FM.1 fly state: 0 Off, 1 Fly, 2 Noclip. Cycle is a no-op unless FLY_ALLOWED.
int  fly_get_state();
void fly_set_state(int s);
void fly_cycle_state();

class Player {
public:
	float boxbase;
	float boxheight;
    float max_walk_speed;
	Vector pos,lpos,vel,accel;
    Vector walk_force;
	float yaw,pitch;	
	float speed,hspeed,vspeed;	
	bool move_back;
	bool jumping;
    bool doublejump;
	World* world;
    
	int invertcam;
    bool inPortal;
    bool climbing;
    bool inLiquid;
    float flash;
    bool onIce;
    bool dead;
    bool autojump_option;
    bool health_option;
    float life;
    bool crouching;  // web port crouch key (CROUCH_HELD, Player.mm) halves boxheight to fit 1x1 gaps
    
    void groundPlayer();
    void reset();
    void horizc();
    bool preupdate(float etime);
    bool vertc();
    void processInput(float etime);
    bool update(float etime);
    Player(World* world);
    bool test(int x,int y,int z,float r);
    bool canFit(float testHeight);
    void move(float etime);
    void takeDamage(float damage,int cause=DEATH_NONE);
    void render();
    void setSpeed(Vector svel,float speed);
    bool checkCollision();
    bool postMove(float etime);       // FM.2: update()'s tail after move(), shared with noclip
    void noclipUpdate(float etime);   // FM.2
    bool boxBlocked();                // FM.2: box overlaps a block vertc() treats as solid
    void unstick();                   // FM.2: pop up out of solid on leaving noclip
    Vector lol[50];
    
    Vector lol1[50];
   // Polyhedra testbox;
    Vector lol2[50];
};


Vector getFlowDirection(int x,int z,int y);

#endif

