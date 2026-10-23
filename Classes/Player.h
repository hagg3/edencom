//
//  Player.h
//  prototype
//
//  Created by Ari Ronen on 10/14/10.
//  Copyright 2010 __MyCompanyName__. All rights reserved.
//
#ifndef Eden_Player_h
#define Eden_Player_h


#import <math.h>
#import "Util.h"
#import "Terrain.h"
#import "World.h"
#import "Camera.h"
#import "Input.h"
#import "OpenGL_Internal.h"
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
	BOOL move_back;
	BOOL jumping;
    BOOL doublejump;
	World* world;
    
	int invertcam;
    BOOL inPortal;
    BOOL climbing;
    BOOL inLiquid;
    float flash;
    BOOL onIce;
    BOOL dead;
    BOOL autojump_option;
    BOOL health_option;
    float life;
    BOOL crouching;  // web port crouch key (CROUCH_HELD, Player.mm) halves boxheight to fit 1x1 gaps
    
    void groundPlayer();
    void reset();
    void horizc();
    BOOL preupdate(float etime);
    BOOL vertc();
    void processInput(float etime);
    BOOL update(float etime);
    Player(World* world);
    BOOL test(int x,int y,int z,float r);
    BOOL canFit(float testHeight);
    void move(float etime);
    void takeDamage(float damage,int cause=DEATH_NONE);
    void render();
    void setSpeed(Vector svel,float speed);
    BOOL checkCollision();
    BOOL postMove(float etime);       // FM.2: update()'s tail after move(), shared with noclip
    void noclipUpdate(float etime);   // FM.2
    BOOL boxBlocked();                // FM.2: box overlaps a block vertc() treats as solid
    void unstick();                   // FM.2: pop up out of solid on leaving noclip
    Vector lol[50];
    
    Vector lol1[50];
   // Polyhedra testbox;
    Vector lol2[50];
};


Vector getFlowDirection(int x,int z,int y);

#endif

