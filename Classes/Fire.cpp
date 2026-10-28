//
//  Fire.m
//  prototype
//
//  Created by Ari Ronen on 11/3/10.
//  Copyright 2010 __MyCompanyName__. All rights reserved.
//

#include "Fire.h"
#include "Globals.h"
#include "Terrain.h"
#include "SpecialEffects.h"
#include "Renderer.h"


extern "C" {
#define n_particles 7
#define max_fparticles 6000
#define max_bb 500
#define SMOKE_SIZER 37

static unsigned short pindices[max_fparticles];


typedef struct{
	Vector pos;
	int pvbi; //particle vertex buffer index
	float vx,vy,vz;
	float life;
	float slife;   
	
}particle;

typedef struct{
	particle particles[n_particles];
	float life;
	int pid;
    bool type;
	float x,z,y;
}firenode;
    
    

static firenode list[max_bb];
    }
static int list_size;
static int num_particles;
static bool updateIndexes;

Fire::Fire(void){
	list_size=0;
	updateIndexes=true;
	num_particles=0;
	
	
}
void Fire::removeNode( int idx){
    
    updateIndexes=true;
    num_particles-=n_particles;   
    
    if(idx!=list_size-1){
        list[idx]=list[list_size-1];
        
    }
    list_size--;
	
    
}

static const float SPEED=.5f;
static const float SPEEDY=2;
extern vertexpStruct pbuffer[pbuffer_size];
bool Fire::update(float etime){
	for(int k=0;k<list_size;k++){
        firenode* node=&list[k];
        
		int n_dead=0;
		for(int i=0;i<n_particles;i++){
			if(node->particles[i].life<0){
                if(node->life<=0){
                 pbuffer[node->particles[i].pvbi].size[0]=0;
                    n_dead++;
                    continue;
                }
				node->particles[i].pos.x=node->x;
				node->particles[i].pos.y=node->y-.3f;
				node->particles[i].pos.z=node->z;
				
				/*int dps=arc4random()%4;
				
				int dpx[4]={0,1,0,-1};
				int dpz[4]={-1,0,1,0};				
				node->particles[i].pos.x+=dpx[dps]*BLOCK_SIZE/1.7;
				node->particles[i].pos.z+=dpz[dps]*BLOCK_SIZE/1.7;
				int dpai=(arc4random()%200-100);
				float dpa=dpai/100.0f;
				//NSLog(@"a:%f",dpa);
				if(dps%2==0){
					node->particles[i].pos.x+=dpa*BLOCK_SIZE/2;	
					
				}else{					
					node->particles[i].pos.z+=dpa*BLOCK_SIZE/2;	
				
				}*/
				int dx=(arc4random()%200-100);
				int dz=(arc4random()%200-100);
				int dy=(arc4random()%50)+80;
				node->particles[i].vx=dx/100.0f*SPEED;
				node->particles[i].vz=dz/100.0f*SPEED;
				node->particles[i].vy=dy/100.0f*SPEEDY;
				int dlife=(arc4random()%60);
				node->particles[i].life=dlife/60.0f*1+1;
				node->particles[i].slife=node->particles[i].life+1;
                pbuffer[node->particles[i].pvbi].size[0]=SMOKE_SIZER;
                updateIndexes=true;
			}
            if(node->particles[i].life>0){
				//pbuffer[node->particles[i].pvbi].colors[3]=255;
                 if(IS_RETINA||IS_IPAD){
                     if(node->particles[i].life<node->particles[i].slife/2){
                         float alpha;
                         alpha=node->particles[i].life/(node->particles[i].slife/2.0f);
                         pbuffer[node->particles[i].pvbi].colors[3]=alpha*255;
                     }
                     if(node->particles[i].life<node->particles[i].slife/2){
                         /*int size=SMOKE_SIZER*(node->particles[i].slife-node->particles[i].life+1);
                         if(size>SMOKE_SIZER*3){
                             size=SMOKE_SIZER*3;
                         }*/
                         pbuffer[node->particles[i].pvbi].size[0]+=1;
                         
                     }
                     //pbuffer[node->particles[i].pvbi].size[0]=SMOKE_SIZER*node->particles[i].life/node->particles[i].slife;
                 }else{
                     pbuffer[node->particles[i].pvbi].size[0]=75*node->particles[i].life/node->particles[i].slife;
                     
                 }
                //if( pbuffer[node->particles[i].pvbi].colors[1])
                   //  pbuffer[node->particles[i].pvbi].colors[1]=60*(node->particles[i].life/node->particles[i].slife);
				node->particles[i].life-=etime;
				node->particles[i].pos.x+=node->particles[i].vx*etime;
				node->particles[i].pos.y+=node->particles[i].vy*etime;			
				node->particles[i].pos.z+=node->particles[i].vz*etime;
				setParticle(node->particles[i].pos,node->particles[i].pvbi);
			}
		}
		node->life-=etime;
		if(node->life<=0&&n_dead==n_particles){
			this->removeNode(k);
			
		}
	}
	
	
	if(updateIndexes){
       
		
		num_particles=0;
		 for(int k=0;k<list_size;k++){	
             firenode* node=&list[k];
            // if(node->life>0)
			for(int i=0;i<n_particles;i++){
                if(node->particles[i].life>0)
				pindices[num_particles++]=node->particles[i].pvbi;
				
			}			
			
		}
        if(num_particles>max_fparticles){
            printg("real particle overflow\n");
        }

		updateIndexes=false;
	}
	return false;
}
//#define NUM_COLORS 2
const GLubyte colors[4][3]={
	
    
    {255,255,255},
     {255,255,255},
    {255,0,0},
	{255,100,0},
/*	{80,80,80},
	{40,40,40},
	{170,185,40},
	{170,40,40},*/
	
};
void Fire::removeFire(int ppid){
    for(int k=0;k<list_size;k++)
		if(list[k].pid==ppid){
			list[k].life=.2f;
			break;
		}
    
}
void Fire::updateFire(int idx,Vector pos){
     //printg("fire updated to model %f,%f%f,\n",pos.x,pos.z,pos.y);
    for(int k=0;k<list_size;k++)
		if(list[k].pid==idx){
			list[k].x=pos.x;
            list[k].y=pos.y;
            list[k].z=pos.z;
			break;
		}
    
}

static int pid=0;
int Fire::addFire(float x,float z,float y,int type,float life){
   // if(type==1)
    //printg("fire added to model %f,%f,%f,\n",x,z,y);
	if(list_size>=max_bb){
        printg("alert: list_size overflow\n");
        return -1;   // Q.1 F14
    }
	while(num_particles+n_particles>=max_fparticles){
        printg("alert: particle overflow\n");
		this->removeNode(arc4random()%list_size);
	}
	num_particles+=n_particles;
	
	firenode* p=&list[list_size];
	
	for(int i=0;i<n_particles;i++){
		p->particles[i].life=-1;		
		
		int pvbi=getPVBI();
		p->particles[i].pvbi=pvbi;
		
		int color=arc4random()%2;
        //if(type==1){
            color=arc4random()%4;
       // }
		pbuffer[pvbi].colors[0]=colors[color][0];
		pbuffer[pvbi].colors[1]=colors[color][1];
		pbuffer[pvbi].colors[2]=colors[color][2];
		pbuffer[pvbi].colors[3]=255;
      /*  pbuffer[pvbi].colors[0]=255;
		pbuffer[pvbi].colors[1]=255;
		pbuffer[pvbi].colors[2]=255;*/
        if(IS_RETINA){
             pbuffer[pvbi].size[0]=SMOKE_SIZER;
        }else{
        pbuffer[pvbi].size[0]=70;
        }
	}
	if(type==0){
        p->type=1;
	p->x=x*BLOCK_SIZE+BLOCK_SIZE/2;
	p->y=y*BLOCK_SIZE+BLOCK_SIZE/2;
	p->z=z*BLOCK_SIZE+BLOCK_SIZE/2;
    }else{
        p->x=x;
        p->y=y;
        p->z=z;
        p->type=0;
    }
	p->pid=pid++;
	p->life=life;
	updateIndexes=true;	
	
	list_size++;	
	
	return p->pid;
	
}
int Fire::addSmoke(float x, float z, float y){
    if(list_size>=max_bb){   // Q.1 F14
        printg("alert: list_size overflow\n");
        return -1;
    }
    while(num_particles+n_particles>max_fparticles){
        printg("alert: particle overflow\n");
		this->removeNode(arc4random()%list_size);
	}
	num_particles+=n_particles;
	
	firenode* p=&list[list_size];
	
	for(int i=0;i<n_particles;i++){
		p->particles[i].life=-1;
		
		int pvbi=getPVBI();
		p->particles[i].pvbi=pvbi;
		
		int color=arc4random()%2;
		pbuffer[pvbi].colors[0]=colors[color][0];
		pbuffer[pvbi].colors[1]=colors[color][1];
		pbuffer[pvbi].colors[2]=colors[color][2];
		pbuffer[pvbi].colors[3]=255;
        /*  pbuffer[pvbi].colors[0]=255;
         pbuffer[pvbi].colors[1]=255;
         pbuffer[pvbi].colors[2]=255;*/
        if(IS_RETINA){
            pbuffer[pvbi].size[0]=SMOKE_SIZER;
        }else{
            pbuffer[pvbi].size[0]=70;
        }
	}
	
        p->type=2;
        p->x=x*BLOCK_SIZE+BLOCK_SIZE/2;
        p->y=y*BLOCK_SIZE+BLOCK_SIZE/2;
        p->z=z*BLOCK_SIZE+BLOCK_SIZE/2;
    
	p->pid=pid++;
	p->life=.5f;
	updateIndexes=true;
	
	list_size++;	
	
	return p->pid;
    
}
Vector getFrameUV(int frame,int sprite){
    Vector uv;
    return uv;
}
static int frame=0,frame2=0;
// G.5b-4: two R::drawWorld pairs (a dark SA,1-SA pass, then the additive SA,1 one) over the flame
// sheet, depth-tested without depth writes, culled, fog off; the frame is the texture transform.
void Fire::renderFireSprites(){
   // glDepthMask(GL_TRUE);
    
	//glDisable(GL_BLEND);
    
    //////////BOT AND TOP
    R::State st = { R::B_ALPHA, true, false, true, 0.0f, 0.0f };
    const R::Tex flame = Resources::getResources->getTex(SPRITE_FLAME)->name;
  
    R::uv().push();
  
    R::uv().scale(1.0f/16,1.0f/16.0f,1);
    
    int framescale=4;
    frame=(frame+1)%((112)*framescale);
    int row=(frame/framescale)/16;
    int col=(frame/framescale)%16;
    // printg("frame: %d\n",frame/framescale);
   // glTranslatef(col,row,0);
    
    framescale=4;
    frame2=(frame2+1)%((32)*framescale);
    row=(frame2/framescale)/16;
    col=(frame2/framescale)%16;
    //printg("row: %d\n",row);
    R::uv().translate(col,row+14,0);
    
    
    
    vertexObject objVertices[max_bb*6*6];
    extern const GLshort cubeShortVertices[36*3];
    extern const GLshort cubeTexture[36*2];
    const GLshort* cubeTextureCustom=cubeTexture;
    const GLshort* cubeVertices=cubeShortVertices;
    int vert=0;
    float poof=1.18f;
    float epoofx=1.3f;
    float epoofy=1.7f;

    
    
    // glDepthMask(FALSE);
    poof=1.1;
    float epoof=1.0f;
    vert=0;
    Vector  camp=World::getWorld->player->pos;
    
    for(int i=0;i<list_size;i++){
        firenode* node=&list[i];
        if(node==NULL)continue;
        if(node->type==0){
           
            continue;}
        if(node->life>0){
            float dist=sqrtf((node->x - camp.x)*(node->x - camp.x) + (node->z-camp.z)*(node->z-camp.z) + (node->y - camp.y)*(node->y-camp.y));
            
            float poof=1.05f+dist*0.035f;
                       for(int k=0;k<6*6;k++){
                Vector vc;
                
                vc=MakeVector((cubeVertices[k*3]-.5f)*poof,(cubeVertices[k*3+1]-.5f)*poof,(cubeVertices[k*3+2]-.5f)*poof);
                
                if(k>=24&&k<30){
                    vc.x*=epoof;
                    vc.z*=epoof;
                    
                }else{
                    continue;
                    
                }
                objVertices[vert].position[0]=(node->x)+vc.x;
                objVertices[vert].position[1]=node->y+vc.y;
                objVertices[vert].position[2]=node->z+vc.z;
                
                
                
                
                //  Vector uv=getFrameUV(0,SPRITE_FLAME);
                objVertices[vert].texs[0]=cubeTextureCustom[k*2+0]*.8f+.1f;
                objVertices[vert].texs[1]=cubeTextureCustom[k*2+1]*.8f+.1f;
                
                           objVertices[vert].colors[0]=0;
                           objVertices[vert].colors[1]=0;
                           objVertices[vert].colors[2]=0;
                           if(node->life<.2f){
                               // printg("rendering dying fire\n");
                               objVertices[vert].colors[3]=5*node->life*255;
                           }else
                               objVertices[vert].colors[3]=255;

                
                
                vert++;
            }
        }
        // node->
        
    }
    //  glDepthMask(TRUE);
   // glPolygonOffset(-30.0f,1.0f);
    //glEnable(GL_POLYGON_OFFSET_FILL);

    R::Geometry g = { &R::kFmtObject, 0, objVertices, 0, nullptr, R::I_NONE, 0, vert, R::TRIANGLES };
	
	
    st.blend = R::B_ALPHA;
    R::setState(st);
      R::drawWorld(g, flame, R::Rgba{1, 1, 1, 1});
   
    for(int i=0;i<vert;i++){
        objVertices[i].colors[0]=255;
        objVertices[i].colors[1]=255;
        objVertices[i].colors[2]=255;
        
    }
    st.blend = R::B_ADD;
    R::setState(st);
   R::drawWorld(g, flame, R::Rgba{1, 1, 1, 1});
    
    R::uv().pop();
    R::uv().push();
    R::uv().loadIdentity();
   
    R::uv().scale(1.0f/16,1.0f/8.0f,1);
    
    framescale=4;
    frame=(frame+1)%((112)*framescale);
    row=(frame/framescale)/16;
    col=(frame/framescale)%16;
   // printg("frame: %d\n",frame/framescale);
    R::uv().translate(col,row,0);
    
   
    
  
    
    
    vert=0;
     poof=1.25f;
   epoofx=1.2f;
     epoofy=1.6f;
    for(int i=0;i<list_size;i++){
        
        firenode* node=&list[i];
        if(node->type==0){
            
            continue;}
        
        if(node->life>0){
            
            float dist=sqrtf((node->x - camp.x)*(node->x - camp.x) + (node->z-camp.z)*(node->z-camp.z) + (node->y - camp.y)*(node->y-camp.y));
       // printg("dist:%f\n",dist);
        float poof=1.05+dist*0.049f;
            
        for(int k=0;k<6*6;k++){
            Vector vc;
         
            vc=MakeVector((cubeVertices[k*3]-.5f)*poof,(cubeVertices[k*3+1]-.5f)*1.3f*poof+.44f,(cubeVertices[k*3+2]-.5f)*poof);
            
            if(k<12){
                vc.x*=epoofx;
                vc.y*=epoofy;
            }else if(k<24){
                vc.y*=epoofy;
                vc.z*=epoofx;
            }else{
                continue;
             //   vc.x*=epoof;
              //  vc.z*=epoof;
            }
            objVertices[vert].position[0]=(node->x)+vc.x;
            objVertices[vert].position[1]=node->y+vc.y;
            objVertices[vert].position[2]=node->z+vc.z;
            
            
            
          
          //  Vector uv=getFrameUV(0,SPRITE_FLAME);
            objVertices[vert].texs[0]=cubeTextureCustom[k*2+0];
            objVertices[vert].texs[1]=cubeTextureCustom[k*2+1];
            
            objVertices[vert].colors[0]=0;
            objVertices[vert].colors[1]=0;
           objVertices[vert].colors[2]=0;
            if(node->life<.2f){
               // printg("rendering dying fire\n");
                objVertices[vert].colors[3]=5*node->life*255;
            }else
            objVertices[vert].colors[3]=255;
               
           
            
            
            vert++;
        }
        }
       // node->
        
    }
     //glPolygonOffset(-3000000.0f,1.0f);
    g.count = vert;
	
    static float alpha_cycle=1.0f;
   // glColor4f(0,0,0,alpha_cycle);
    st.blend = R::B_ALPHA;
    R::setState(st);
    R::drawWorld(g, flame, R::Rgba{1, 1, 1, 1});
    for(int i=0;i<vert;i++){
        objVertices[i].colors[0]=255;
        objVertices[i].colors[1]=255;
        objVertices[i].colors[2]=255;
        
    }
    st.blend = R::B_ADD;
    R::setState(st);
    //glColor4f(1.0,1.0,1,alpha_cycle);
    R::drawWorld(g, flame, R::Rgba{1, 1, 1, 1});
    
    alpha_cycle-=.025f;
    if(alpha_cycle<0)alpha_cycle=1.0f;
    
    
    
    
    
    //////////
    
    R::uv().pop();
    
  //glDisable(GL_POLYGON_OFFSET_FILL);
    
    
}
void Fire::render(){
   /* glColor4f(0,0,0,1.0f);
    glDisableClientState(GL_COLOR_ARRAY);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    
    glDrawElements(GL_POINTS,num_particles,GL_UNSIGNED_SHORT,pindices);	
   */
    // G.5b-4: the smoke points (pbuffer, SpecialEffects.cpp) as R::drawSprites: blended, depth-tested
    // without depth writes, culled, size per point with ES 1.1's attenuation {1, 0, .002}.
    const R::State st = { R::B_ALPHA, true, false, true, 0.0f, 0.0f };
    R::setState(st);
    const R::Geometry g = { &R::kFmtSprite, 0, pbuffer, 0, pindices, R::I_U16, 0, num_particles, R::POINTS };
    const R::SpriteParams sp = { 1.0f, {1.0f, 0.0f, .002f} };
	R::drawSprites(g, Resources::getResources->getTex(ICO_SMOKE)->name, sp);
    
   // glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    this->renderFireSprites();
    
}
void Fire::clearAllEffects(){
	updateIndexes=true;
    list_size=0;
    num_particles=0;
	
	
}

