//
//  Firework.m
//  Eden
//
//  Created by Ari Ronen on 5/22/12.
//  Copyright (c) 2012 __MyCompanyName__. All rights reserved.
//

#include "Firework.h"

#include <math.h>
#include "Util.h"
#include "World.h"
#include "OpenGL_Internal.h"
#include "Renderer.h"

Firework::Firework(){
    n_firework=0;
    
}
void Firework::addFirework(int x,int y,int z,int color){
    int n=n_firework<MAX_FIREWORK?n_firework++:MAX_FIREWORK-1;   // Q.1 F15: overwrite the last slot when full
    fireworks[n].pos=MakeVector(x,y,z);
    fireworks[n].vel=MakeVector(0,27,0);
   // fireworks[n].vel=MakeVector(0,0,0);
    fireworks[n].color=color;
    if(color==0)fireworks[n].color=arc4random()%53+1;
    
    if(arc4random()%40==0){
        fireworks[n].color=0;
    }
       fireworks[n].fuse=randf(0.8f)+1.3f;
}
void Firework::removeAllFireworks(){
    n_firework=0;
    
}
void Firework::removeFirework(int i){
    for(int j=i+1;j<n_firework;j++){
        fireworks[j-1]=fireworks[j];
    }
    if(i<n_firework){
        n_firework--;
    }
}
void Firework::update(float etime) {
    for(int i=0;i<n_firework;i++){
        fireworks[i].pos=v_add(fireworks[i].pos,v_mult(fireworks[i].vel,etime));
        if(World::getWorld->terrain->getLand(roundf(fireworks[i].pos.x),
                                            roundf(fireworks[i].pos.z),
                                            roundf(fireworks[i].pos.y))>0){
            fireworks[i].fuse=-1;
        }
        fireworks[i].fuse-=etime;
       
    }
    frot+=(M_PI*2/4.0f)*etime;
    if(frot>M_PI*2)frot-=M_PI*2;
}
void Firework::render(){
    Graphics::startPreview();
    // G.5b-4: the cube is an R::drawLit (Graphics::drawFirework). 2.1's ES 1.1 state, stated: LIGHT0 at
    // the eye (its position loaded under an identity modelview), the default model ambient 0.2, no
    // specular (material specular 0), smooth shading. With Original lighting off it draws unlit (R8).
    float lightPosition[4] = {0.0f,0.0f, 0.0f, 1.0f};
    float lightAmbient[4]  = {0.3f, 0.3f, 0.3f, 1.0f};
    float lightDiffuse[4]  = {0.7f, 0.7f, 0.7f, 1.0f};    
    R::LightSet ls = {};
    ls.light[0].on = true;
    memcpy(ls.light[0].pos, lightPosition, sizeof lightPosition);
    memcpy(ls.light[0].ambient, lightAmbient, sizeof lightAmbient);
    memcpy(ls.light[0].diffuse, lightDiffuse, sizeof lightDiffuse);
    ls.modelAmbient[0] = ls.modelAmbient[1] = ls.modelAmbient[2] = 0.2f; ls.modelAmbient[3] = 1.0f;
    ls.flat = false;
    R::setLights(ls);
     for(int i=0;i<n_firework;i++){
     /*[Graphics drawCube:fireworks[i].pos.x
                      :fireworks[i].pos.y
                      :fireworks[i].pos.z
                      :TYPE_FIREWORK
                      :1];*/
         Graphics::drawFirework(fireworks[i].pos.x,
                                fireworks[i].pos.y,
                                fireworks[i].pos.z,
                                fireworks[i].color,
                                0.5f,
                                frot);
        
         
         if(fireworks[i].fuse<0){
            Resources::getResources->playSound(S_FIREWORK_EXPLODE);
             Vector skyc=World::getWorld->terrain->skycolor;
             skyc.x+=.3f;
             skyc.y+=.3f;
             skyc.z+=.3f;
             if(skyc.x>1)skyc.x=1;
             if(skyc.y>1)skyc.y=1;
             if(skyc.z>1)skyc.z=1;
             World::getWorld->terrain->skycolor=skyc;
             World::getWorld->effects->addCreatureVanish(fireworks[i].pos.x,fireworks[i].pos.z,fireworks[i].pos.y,fireworks[i].color,TYPE_FIREWORK);
             World::getWorld->effects->addFirework(fireworks[i].pos.x,fireworks[i].pos.z,fireworks[i].pos.y,fireworks[i].color);
             this->removeFirework(i);
             i--;
             
         }
     }
    Graphics::endPreview();
    
}

