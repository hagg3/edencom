//
//  SpecialEffects.m
//  prototype
//
//  Created by Ari Ronen on 11/2/10.
//  Copyright 2010 __MyCompanyName__. All rights reserved.
//

#include "SpecialEffects.h"
#include "Graphics.h"

#include "World.h"
#include "Renderer.h"

vertexpStruct pbuffer[pbuffer_size];
static int pvbi_counter;





SpecialEffects::SpecialEffects(){
   
	memset(pbuffer,0,sizeof(vertexpStruct)*pbuffer_size);
	pvbi_counter=0;

    bb=new BlockBreak();
    fire=new Fire();
	
	
	
}
void setParticle(Vector p, int pvbi){
	
	pbuffer[pvbi].position[0]=p.x;
	pbuffer[pvbi].position[1]=p.y;
	pbuffer[pvbi].position[2]=p.z;
}
int getPVBI(){
	
	if(pvbi_counter==pbuffer_size-1){
		pvbi_counter=-1;	
	}
	return ++pvbi_counter;
}
bool SpecialEffects::update(float etime){
   
	bb->update(etime);
    fire->update(etime);
	

	return false;
}

// G.5b-4: the smoke points, fire quads and block-break debris are R:: draws (Fire::render,
// BlockBreak::render), each stating its own state. Fog is off for all of them: the effects pass runs
// after endTerrain, which turns it off. The stock point state (sprite on, COORD_REPLACE, size min 0.1,
// attenuation {1, 0, .002}, the size array) is Fire::render's SpriteParams now; no other draw used it.
void SpecialEffects::render(){
	
	R::fogOff();
	//glPointParameterf(GL_POINT_SIZE_MAX,30.0f);
	fire->render();
	
	bb->render();
	
	// The ES 1.1 state the stock code here left for the unported draws after the effects pass (the HUD,
	// next frame's terrain), handed to the shim as logical state until G.5b-7 deletes it.
	Pen::color(1.0f, 1.0f, 1.0f, 1.0f);
	Pen::blendFunc(R::B_ALPHA);
	Pen::blend(false);
	Pen::depthWrite(true);
	Pen::textured(true);
	Pen::texture(Resources::getResources->atlas->name);

}
void SpecialEffects::addBlockBreak(int x,int z,int y,int type,int color){
	bb->addBlockBreak(x,z,y,type,color);
	
}
int SpecialEffects::addFire(float x,float z,float y,int type,float life){
	return fire->addFire(x ,z ,y ,type ,life);
	
}
int SpecialEffects::addSmoke(float x,float z,float y){
    return fire->addSmoke(x,z,y);
}
void SpecialEffects::addFirework(float x,float z,float y, int color){
    
    bb->addFirework(x,z,y,color);
    
}
void SpecialEffects::addCreatureVanish(float x,float z,float y, int color,int type){
    bb->addCreatureVanish2(x,z,y,color,type);
    
}

void SpecialEffects::addBlockExplode(int x,int z,int y,int type,int color){
	bb->addBlockExplode(x,z,y,type,(int)color);
}
void SpecialEffects::removeFire(int ppid){
	fire->removeFire(ppid);
}
void SpecialEffects::updateFire(int idx,Vector pos){
    fire->updateFire(idx,pos);
}
void SpecialEffects::clearAllEffects(){
	
    bb->clearAllEffects();
	
	fire->clearAllEffects();
	
}

