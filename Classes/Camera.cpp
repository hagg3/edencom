//
//  Camera.m
//  prototype
//
//  Created by Ari Ronen on 10/11/10.
//  Copyright 2010 __MyCompanyName__. All rights reserved.
//

#include "Camera.h"
#include "Util.h"
#include <math.h>
#include "OpenGL_Internal.h"
#include "Input.h"
#include "Frustum.h"
#include "Graphics.h"
#include "Renderer.h"   // G.5b-3: the matrix readbacks
#include <string.h>
#include "Terrain.h"
#include "Liquids.h"

#define CAM_SPEED 4.0f
#define YAW_SPEED .5f
#define PITCH_SPEED .5f
#define CAM_TP_DIST 4.0f      // H.12: stock's own n=4.0f
#define CAM_TP_MIN .6f
#define CAM_TP_GROW 8.0f      // blocks/s back out after a pull-in (shrinking is instant)


Camera::Camera(){

	
	initFrustum();
	
	
	
}
void Camera::reset(){
	px=py=pz=vx=vy=vz=yaw=pitch=0;
	yaw=90;
	pz=-15;
	py=25;
	px=10;
	pitch=0;
	mode=0;
	dist=CAM_TP_DIST;
	float projviewf[16];
	memcpy(projviewf, R::proj().top().m, sizeof(projviewf));   // G.5b-3: was glGetFloatv
	float modelviewf[16];
	memcpy(modelviewf, R::mv().top().m, sizeof(modelviewf));
	float rview[16];
	MatrixMultiplyBy4x(projviewf,modelviewf,rview);
	setFrustum(rview);
}
Camera::~Camera(){
	destroyFrustum();
	
}
bool Camera::update(float etime){
	//FREE Movement/ NO COLLISION DETECTION/GRAVITY
	/*Input* input=[Input getInput];
	if(input.down){
		if(input.taps>1)
		speed=CAM_SPEED*etime;
		if(input.taps>2)
			speed*=-1;
		int dx=input.mx-input.pmx;
		int dy=input.my-input.pmy;
		[input clearMove];
		if(dy!=0){
		 	yaw+=-dy*YAW_SPEED;
		}
		if(dx!=0){
			pitch+=-dx*PITCH_SPEED;
		}
	}else{
		speed=0;
	}
	if(yaw>=360)yaw-=360;
	if(yaw<=-360)yaw+=360;
	//if(pitch > 89) pitch=89;
	//if(pitch < -89) pitch=-89;
	
	float cosYaw=cos(D2R(yaw));
	float sinYaw=sin(D2R(yaw));
	float sinPitch=2*sin(D2R(pitch));
	
	px+=cosYaw*speed;
	pz+=sinYaw*speed;
	py+=sinPitch*speed;
	*/
	return false;
}

void Camera::render2(){

    //NSLog(@"speed:%f",speed);
	//yaw+=1;
	float cosYaw=cos(D2R(yaw));
	float sinYaw=sin(D2R(yaw));
	
	float sinPitch=sin(D2R(pitch));
	Vector alook;
	alook.x=px+cosYaw*(1.0f-absf(sinPitch));
	alook.y=py+sinPitch;
	alook.z=pz+sinYaw*(1.0f-absf(sinPitch));
	
		
	/*glPushMatrix();
     
     // Reset and transform the matrix.
     glLoadIdentity();
     if(World::getWorld->FLIPPED)
     glRotatef(90,0,0,1);
     else
     glRotatef(270,0,0,1);	
     
     gluLookAt(
     0,0,0,
     look.x-px,look.y-py,look.z-pz,
     0,1,0);
     
     [Graphics drawSkybox];
     glPopMatrix();*/
    
	if(mode==1){
		// H.12: stock's arm here was a different camera from render()'s (eye 8 back and 15 up).
		// The picking matrix is now the drawn frame's, in absolute coords like the mode==0 arm
		// (findWorldCoords walks absolute blocks, so no chunkOffset here). mode==2 (front view)
		// deliberately falls through to the first-person matrix: the front camera looks back at
		// the player, so taps and the crosshair aim along the player's own look, as in first person.
		Vector eye,at;
		viewPoints(&eye,&at);
		gluLookAt(eye.x, eye.y, eye.z, at.x, at.y, at.z, 0, 1, 0);
	}else{
         gluLookAt(px, py, pz, alook.x, alook.y, alook.z, 0, 1, 0);	
        
		//gluLookAt(px-World::getWorld->fm.chunkOffsetX*CHUNK_SIZE, py, pz-World::getWorld->fm.chunkOffsetZ*CHUNK_SIZE, look.x-World::getWorld->fm.chunkOffsetX*CHUNK_SIZE, look.y, look.z-World::getWorld->fm.chunkOffsetZ*CHUNK_SIZE, 0, 1, 0);	
	}


}
void Camera::render(){
	//NSLog(@"speed:%f",speed);
	//yaw+=1;
	float cosYaw=cos(D2R(yaw));
	float sinYaw=sin(D2R(yaw));
	
	float sinPitch=sin(D2R(pitch));
	//printg("pitch: %f\n",pitch);
	look.x=px+cosYaw*(1.0f-absf(sinPitch));
	look.y=py+sinPitch;
	look.z=pz+sinYaw*(1.0f-absf(sinPitch));
	
	
	// G.5b-7: the modelview (prepareScene's; gluLookAt multiplies onto R::mv()).
	R::mv().push();
	// H.12: the frustum is built from the same (eye, target) as the scene below. Stock always fed
	// it the first-person pair, so in third person everything was culled against the wrong frustum.
	Vector eye,at;
	viewPoints(&eye,&at);
	gluLookAt(eye.x, eye.y, eye.z, at.x, at.y, at.z, 0, 1, 0);	
	float projviewf[16];
	memcpy(projviewf, R::proj().top().m, sizeof(projviewf));   // G.5b-3: was glGetFloatv
	float modelviewf[16];
	memcpy(modelviewf, R::mv().top().m, sizeof(modelviewf));
	float rview[16];
	MatrixMultiplyBy4x(projviewf,modelviewf,rview);
	setFrustum(rview);
	
	R::mv().pop();
	
	R::mv().push();
	
    // Reset and transform the matrix.
    R::mv().loadIdentity();
		
	
		gluLookAt(
			 0,0,0,
			  look.x-px,look.y-py,look.z-pz,
			  0,1,0);
	
    Graphics::drawSkybox();
	
	R::mv().pop();

	if(mode!=0){
		// H.12: stock's arm put the eye 6 units BELOW the player (py-6.0f), looking up at them.
		//float n=4.0f;
		//gluLookAt(px+n*cosYaw-World::getWorld->fm->chunkOffsetX*CHUNK_SIZE, py-6.0f, pz+n*sinYaw-World::getWorld->fm->chunkOffsetZ*CHUNK_SIZE, px-World::getWorld->fm->chunkOffsetX*CHUNK_SIZE, py, pz-World::getWorld->fm->chunkOffsetZ*CHUNK_SIZE, 0, 1, 0);
		float ox=World::getWorld->fm->chunkOffsetX*CHUNK_SIZE;
		float oz=World::getWorld->fm->chunkOffsetZ*CHUNK_SIZE;
		gluLookAt(eye.x-ox, eye.y, eye.z-oz, at.x-ox, at.y, at.z-oz, 0, 1, 0);
	}else{
       //gluLookAt(px, py, pz, look.x, look.y, look.z, 0, 1, 0);
        
		gluLookAt(px-World::getWorld->fm->chunkOffsetX*CHUNK_SIZE, py, pz-World::getWorld->fm->chunkOffsetZ*CHUNK_SIZE, look.x-World::getWorld->fm->chunkOffsetX*CHUNK_SIZE, look.y, look.z-World::getWorld->fm->chunkOffsetZ*CHUNK_SIZE, 0, 1, 0);
	}
		
}

// H.12 ---------------------------------------------------------------------------------------
// The orbit uses the engine's own (non-unit) first-person look vector, normalized, and puts the
// third-person eye ON the first-person ray through the pivot (px,py,pz = the player's eye). So
// the screen-centre ray behind the player is the first-person ray: aim does not move when the
// view is toggled, and findWorldCoords only has to slide its origin up to the pivot.
void Camera::viewDir(Vector* d){
	float sinPitch=sin(D2R(pitch));
	d->x=cos(D2R(yaw))*(1.0f-absf(sinPitch));
	d->y=sinPitch;
	d->z=sin(D2R(yaw))*(1.0f-absf(sinPitch));
	NormalizeVector(d);
}
void Camera::viewPoints(Vector* eye,Vector* at){
	Vector d;
	if(mode==1){
		viewDir(&d);
		*eye=MakeVector(px-d.x*dist,py-d.y*dist,pz-d.z*dist);
		*at=MakeVector(px,py,pz);
	}else if(mode==2){
		viewDir(&d);
		*eye=MakeVector(px+d.x*dist,py+d.y*dist,pz+d.z*dist);
		*at=MakeVector(px+d.x*(dist-1.0f),py+d.y*(dist-1.0f),pz+d.z*(dist-1.0f));
	}else{
		float cosYaw=cos(D2R(yaw));
		float sinYaw=sin(D2R(yaw));
		float sinPitch=sin(D2R(pitch));
		*eye=MakeVector(px,py,pz);
		*at=MakeVector(px+cosYaw*(1.0f-absf(sinPitch)),py+sinPitch,pz+sinYaw*(1.0f-absf(sinPitch)));
	}
}
// Occlusion pull-in: march from the pivot toward where the eye wants to be; the first block a
// camera cannot sit in caps dist. Passable is Player.mm's preview test (air, or shallow liquid);
// -1 (not loaded) counts as passable. Shrinks instantly, grows back at CAM_TP_GROW.
void Camera::updateThirdPerson(float etime){
	if(mode==0){
		dist=CAM_TP_DIST;
		return;
	}
	Vector d;
	viewDir(&d);
	float s=(mode==1)?-1.0f:1.0f;
	float target=CAM_TP_DIST;
	for(float t=.25f;t<=CAM_TP_DIST+.001f;t+=.25f){
		float x=px+s*d.x*t, y=py+s*d.y*t, z=pz+s*d.z*t;
		if(y<0)continue;
		int type=World::getWorld->terrain->getLand((int)floorf(x),(int)floorf(z),(int)floorf(y));
		if(type>0&&!(blockinfo[type]&IS_LIQUID&&getLevel(type)<4)){
			target=t-.3f;
			if(target<CAM_TP_MIN)target=CAM_TP_MIN;
			break;
		}
	}
	if(target<dist)dist=target;
	else{
		dist+=CAM_TP_GROW*etime;
		if(dist>target)dist=target;
	}
}
