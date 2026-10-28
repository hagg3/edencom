//
//  BlockBreak.m
//  prototype
//
//  Created by Ari Ronen on 11/2/10.
//  Copyright 2010 __MyCompanyName__. All rights reserved.
//

#include "BlockBreak.h"
#include "Globals.h"
#include "Terrain.h"
#include "Graphics.h"

#include "VecMath.h"
#include "Renderer.h"
//#import "SpecialEffects.h"

#define BREAKP 10
#define VANISHP 60
#define EXPP 1
#define pbuffer_size2 20000
#define max_bparticles pbuffer_size2

#define PT_GRAV 0
#define PT_NOGRAV 1
#define PT_FIREWORK 2
static unsigned short pindices[max_bparticles];
static vertexpBreak pbuffer2[pbuffer_size2];
static vertexpBreak trails[pbuffer_size2];
static int trail_count;
static int pvbi_counter2;

typedef struct{
	Vector pos;
	int pvbi; //particle vertex buffer index
	float vx,vy,vz;	
    int type;
    float size;    
    float rx,rz;
    float vrx,vrz;
}particle;

typedef struct _cnode{
	particle particles[VANISHP];
    int n_particles;
	float life;
    float start_life;
}cnode;





static cnode list[max_bparticles];
static int list_size;

static int num_particles;
static int num_vertices;
static bool updateIndexes;
static Vector polyp[4];
static Vector polyn[4];
static Vector polyf[4][3];
static Vector polyfn[4][3];
static const float SPEED=2.5;
static const float SPEEDY=10;
const float LIFE=2.0f;
void mf(int i,int p1,int p2,int p3){
    polyf[i][0]=polyp[p1];
    polyf[i][1]=polyp[p2];
    polyf[i][2]=polyp[p3];
    polyfn[i][0]=polyn[p1];
    polyfn[i][1]=polyn[p2];
    polyfn[i][2]=polyn[p3];
}
BlockBreak::BlockBreak(){
	
    trail_count=0;
	updateIndexes=true;
	num_particles=num_vertices=0;
    list_size=0;
    memset(pbuffer2,0,sizeof(vertexpBreak)*pbuffer_size2);
	pvbi_counter2=0;
    polyp[0]=MakeVector(0,.816f,0);
    polyp[1]=MakeVector(0,-.816f,.816f);
    polyp[2]=MakeVector(-.816f,-.816f,-.816f);
    polyp[3]=MakeVector(.816f,-.816f,-.816f);
    
    for(int i=0;i<4;i++){
        polyn[i]=polyp[i];
        NormalizeVector(&polyn[i]);
    }
    mf(0,0,2,1);
    mf(1,0,1,3);
    mf(2,0,3,2);
    mf(3,1,2,3);

	
}
int BlockBreak::update(float etime){
    trail_count=0;
	for(int k=0;k<list_size;k++){
       
		//float tetime=LIFE-node.life;
		for(int i=0;i<list[k].n_particles;i++){
           
			list[k].particles[i].pos.x+=list[k].particles[i].vx*etime;
			list[k].particles[i].pos.y+=list[k].particles[i].vy*etime;			
			list[k].particles[i].pos.z+=list[k].particles[i].vz*etime;
            if(list[k].particles[i].type==PT_GRAV){
                list[k].particles[i].vy-=60*etime;
            }
            list[k].particles[i].rx+=list[k].particles[i].vrx*etime;
            list[k].particles[i].rz+=list[k].particles[i].vrz*etime;
            particle p1=list[k].particles[i];
            if(p1.vy<0&&getLandc(p1.pos.x,p1.pos.z,p1.pos.y-p1.size*.716f)>0){
                float np=(int)(p1.pos.y-p1.size*.716f)+1+p1.size*.716f;
                if(np-p1.pos.y<.5f){
                    list[k].particles[i].pos.y=np;
                    list[k].particles[i].vy=0;
                    list[k].particles[i].vx*=.7f;
                    list[k].particles[i].vz*=.7f;
                    list[k].particles[i].vrx*=.7f;
                    list[k].particles[i].vrz*=.7f;
                }
                
                //node.particles[i].rx*=0;
                //node.particles[i].rz*=0;
            }
			
			//pbuffer2[node.particles[i].pvbi].position[0]=node.particles[i].pos.x;
           // pbuffer2[node.particles[i].pvbi].position[1]=node.particles[i].pos.y;
         //   pbuffer2[node.particles[i].pvbi].position[2]=node.particles[i].pos.z;
             particle p=list[k].particles[i];
            float rsize=p.size;
            if(list[k].life<LIFE/2){
                rsize*=list[k].life/(LIFE/2);
            }
            Mat4f rot= Mat4f::RotationX(p.rx)*Mat4f::RotationZ(p.rz);
            for (int f=0;f<4;f++){
                for(int v=0;v<3;v++){
                    
                    Vec4f vec=
                   rot*
                    Vec4f(polyf[f][v].x*rsize,polyf[f][v].y*rsize,polyf[f][v].z*rsize,1);
                                                                                   
                    pbuffer2[p.pvbi+f*3+v].position[0]=p.pos.x+vec.x;
                     pbuffer2[p.pvbi+f*3+v].position[1]=p.pos.y+vec.y;
                     pbuffer2[p.pvbi+f*3+v].position[2]=p.pos.z+vec.z;
                    
                    
                    vec=rot* Vec4f(polyfn[f][v].x,polyfn[f][v].y,polyfn[f][v].z,1);
                    pbuffer2[p.pvbi+f*3+v].normal[0]=vec.x;
                    pbuffer2[p.pvbi+f*3+v].normal[1]=vec.y;
                    pbuffer2[p.pvbi+f*3+v].normal[2]=vec.z;
                     
                }
                
                    
            }
            if(p.type==PT_FIREWORK){
                if(trail_count<pbuffer_size2-2){
                    float elapsed=list[k].start_life-list[k].life;
                    if(elapsed>list[k].life)
                        elapsed=list[k].life;
                    if(elapsed>.5f)elapsed=.5f;
                    trails[trail_count].position[0]=p.pos.x;
                    trails[trail_count].position[1]=p.pos.y;
                    trails[trail_count].position[2]=p.pos.z;
                    trails[trail_count].colors[0]= pbuffer2[p.pvbi].colors[0];   
                    trails[trail_count].colors[1]= pbuffer2[p.pvbi].colors[1];
                    trails[trail_count].colors[2]= pbuffer2[p.pvbi].colors[2];
                    trails[trail_count].colors[3]= pbuffer2[p.pvbi].colors[3];
                    
                    trail_count++;
                    trails[trail_count].position[0]=p.pos.x-p.vx*elapsed;
                    trails[trail_count].position[1]=p.pos.y-p.vy*elapsed;
                    trails[trail_count].position[2]=p.pos.z-p.vz*elapsed;
                    trails[trail_count].colors[0]= pbuffer2[p.pvbi].colors[0];   
                    trails[trail_count].colors[1]= pbuffer2[p.pvbi].colors[1];
                    trails[trail_count].colors[2]= pbuffer2[p.pvbi].colors[2];
                    trails[trail_count].colors[3]= 0;
                    
                    trail_count++;
                }
                
                
                
            }
            
		}
		list[k].life-=etime;
		if(list[k].life<=0){
            this->removeNode(k);
						
		}
	}
	
	
	if(updateIndexes){
        
		num_particles=0;
        num_vertices=0;
		for(int k=0;k<list_size;k++)	
			for(int i=0;i<list[k].n_particles;i++){
                for(int j=0;j<12;j++)
                    pindices[num_particles*12+j]=list[k].particles[i].pvbi+j;
				
                num_particles++;
                num_vertices+=12;
			}			
        
    }		
	
    updateIndexes=false;

return false;
}

void BlockBreak::removeNode(int idx){
   
    updateIndexes=true;
    num_particles-=list[idx].n_particles;
    num_vertices-=12*list[idx].n_particles;
    
    if(idx!=list_size-1){
        list[idx]=list[list_size-1];
        
    }
    list_size--;
	
    
}
static Vec4f lightPosition = Vec4f(0.0f,0.0f, 0.0f, 1.0f);
static Vec4f lightAmbient  = Vec4f(0.3f, 0.3f, 0.3f, 1.0f);
static Vec4f lightDiffuse  = Vec4f(0.7f, 0.7f, 0.7f, 1.0f);
void BlockBreak::render(){
    // G.5b-4: the debris is an R::drawLit, the firework trails an R::drawWorld of lines. Stated from
    // 2.1's ES 1.1 state (renderer plan §7):
    //  * LIGHT0 ambient .3, diffuse .7, model ambient 0.2, no specular. Its position (0,0,0,1) was loaded
    //    while glMatrixMode was GL_TEXTURE (the push / loadIdentity / pop below went to the texture
    //    stack), so ES 1.1 transformed it by the effects pass's MODELVIEW: the light sits at that
    //    matrix's origin in eye space, not at the eye.
    //  * GL_FLAT shading (Graphics::initGraphics; the glShadeModel(GL_SMOOTH) came after the debris draw),
    //    GL_NORMALIZE (the lit program always normalises).
    //  * Opaque, depth-tested and written, culled, fog off; the atlas through glScalef(1, 1/ATLAS_TILES, 1).
    // With Original lighting off the debris draws unlit, as the port always drew it (R8).
   // glEnable(GL_TEXTURE_2D);
   // glEnable(GL_SMOOTH);
    R::uv().push();
    R::uv().scale(1,1.0f/ATLAS_TILES,1);
    
    //Vec4f lightSpecular = Vec4f(0.2f, 0.2f, 0.2f, 1.0f);
    
    const R::Mat4& mvm = R::mv().top();
    R::LightSet ls = {};
    ls.light[0].on = true;
    for (int i = 0; i < 4; ++i)   // MODELVIEW * lightPosition
        ls.light[0].pos[i] = mvm.m[i] * lightPosition.x + mvm.m[4 + i] * lightPosition.y +
                             mvm.m[8 + i] * lightPosition.z + mvm.m[12 + i] * lightPosition.w;
    memcpy(ls.light[0].ambient, lightAmbient.ptr(), sizeof ls.light[0].ambient);
    memcpy(ls.light[0].diffuse, lightDiffuse.ptr(), sizeof ls.light[0].diffuse);
    ls.modelAmbient[0] = ls.modelAmbient[1] = ls.modelAmbient[2] = 0.2f; ls.modelAmbient[3] = 1.0f;
    ls.flat = true;
    R::setLights(ls);
    const R::State opaque = { R::B_OFF, true, true, true, 0.0f, 0.0f };
    R::setState(opaque);
	
   // glDisable(GL_CULL_FACE);
    // glEnableClientState(GL_TEXTURE_COORD_ARRAY);
   // glColor4f(1.0f,1,1,1.0f);
  
    const R::Geometry debris = { &R::kFmtBreak, 0, pbuffer2, 0, pindices, R::I_U16, 0, num_particles*12, R::TRIANGLES };
	R::drawLit(debris, Resources::getResources->atlas->name, R::Rgba{1, 1, 1, 1});
    R::uv().pop();

	// glEnable(GL_CULL_FACE);
   // glEnable(GL_TEXTURE_2D);
     //glDisable(GL_SMOOTH);
    
    const R::State blended = { R::B_ALPHA, true, true, true, 0.0f, 0.0f };
    R::setState(blended);
   
    // glDisable(GL_CULL_FACE);
    // glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    // glColor4f(1.0f,1,1,1.0f);
    
    const R::Geometry lines = { &R::kFmtBreak, 0, trails, 0, nullptr, R::I_NONE, 0, trail_count, R::LINES };
    R::drawWorld(lines, 0, R::Rgba{1, 1, 1, 1});   // untextured
		//glEnable(GL_TEXTURE_2D);
}





static int getPVBI2(){
    
	pvbi_counter2+=12;
	if(pvbi_counter2+12>=pbuffer_size2){
		pvbi_counter2=0;	
	}
   
	return pvbi_counter2;
}
extern Vector colorTable[256];
extern const uint8_t blockColor[NUM_BLOCKS+1][3];
extern const uint8_t creatureColor[NUM_CREATURES][3];
extern const int blockTypeFaces[NUM_BLOCKS+1][6];

void BlockBreak::addBlockExplode(int x,int z,int y,int type, int color) {
    if(type<=0)return;
	while(num_particles*12+EXPP*12>max_bparticles){
		this->removeNode(arc4random()%list_size);
	}
	    //NSLog(@"%d %d type:%d",x,z,type);
	cnode p;
    p.n_particles=EXPP;
    num_particles+=p.n_particles;
    num_vertices+=12*p.n_particles;

	//p.next=NULL;
	for(int i=0;i<p.n_particles;i++){		
		
		p.particles[i].pos.x=x*BLOCK_SIZE+BLOCK_SIZE/2;
		p.particles[i].pos.y=y*BLOCK_SIZE+BLOCK_SIZE/2;
		p.particles[i].pos.z=z*BLOCK_SIZE+BLOCK_SIZE/2;
		int dx=(arc4random()%200-100);
		int dz=(arc4random()%200-100);
		int dy=(arc4random()%25)+5;
		p.particles[i].vx=dx/100.0f*SPEED*2;
		p.particles[i].vz=dz/100.0f*SPEED*2;
		p.particles[i].vy=dy/25.0f*SPEEDY*4;	
        
        p.particles[i].rx=randf(6.28);
        p.particles[i].rz=randf(6.28);
        p.particles[i].type=PT_GRAV;
        p.particles[i].vrx=randf(20)-10;
        p.particles[i].vrz=randf(20)-10;
        
		int pvbi=getPVBI2();
		p.particles[i].pvbi=pvbi;
		p.particles[i].size=randf(.1f)+.4f;
        bool useColorTex=false;
        int bf=blockTypeFaces[type][arc4random()%6];
        if(blockinfo[type]&IS_ATLAS2)
            bf=blockTypeFaces[TYPE_CLOUD][arc4random()%6];
        for(int j=0;j<12;j++){
            if(color==0){
                if(bf==TEX_GRASS_SIDE||type==TYPE_GRASS3||type==TYPE_TNT||type==TYPE_BRICK||type==TYPE_VINE||type==TYPE_FIREWORK||IS_NEWBLOCK_TEX(bf))
                {
                    useColorTex=true;
                    pbuffer2[pvbi+j].colors[0]=255;
                    pbuffer2[pvbi+j].colors[1]=255;
                    pbuffer2[pvbi+j].colors[2]=255;
                }else{
                    pbuffer2[pvbi+j].colors[0]=(int)(blockColor[type][0])%256;
                    pbuffer2[pvbi+j].colors[1]=(int)(blockColor[type][1])%256;
                    pbuffer2[pvbi+j].colors[2]=(int)(blockColor[type][2])%256;
                }
            }else{
                Vector clr=colorTable[color];
                pbuffer2[pvbi+j].colors[0]=(int)(clr.x*255)%256;
                pbuffer2[pvbi+j].colors[1]=(int)(clr.y*255)%256;
                pbuffer2[pvbi+j].colors[2]=(int)(clr.z*255)%256;
                
            }
            
            pbuffer2[pvbi+j].colors[3]=255;		
        }
        CGPoint tp;
        
        if(useColorTex){
            if(bf==TEX_GRASS_SIDE)
                bf=TEX_GRASS_SIDE_COLOR;
            else if(bf==TEX_TNT_SIDE)
                bf=TEX_TNT_SIDE_COLOR;
            else if(bf==TEX_TNT_TOP)
                bf=TEX_TNT_TOP_COLOR;
            else if(bf==TEX_BRICK)
                bf=TEX_BRICK_COLOR;
            else if(IS_NEWBLOCK_TEX(bf))
                bf=NEWBLOCK_COLOR_TEX(bf);   // D.2t
        }
        tp=Resources::getResources->getBlockTexShort(bf);
        
        static int triTextureCustom[6]={0,0,
            0,1,
            1,1,};
        
        for (int f=0;f<4;f++){
            for(int v=0;v<3;v++){
                pbuffer2[pvbi+f*3+v].texs[0]=triTextureCustom[v*2];
                pbuffer2[pvbi+f*3+v].texs[1]=triTextureCustom[v*2+1]*tp.y+tp.x;
                
            }
            
            
        }
	}
	
	p.start_life=p.life=LIFE;
	updateIndexes=true;	
    for(int i=0;i<p.n_particles;i++){	
        list[list_size].particles[i]=p.particles[i];
    }
	list[list_size]=p;
    list_size++;

	
}

void BlockBreak::addFirework(float x,float z,float y,int color){
    while(num_particles*12+VANISHP*12>max_bparticles){
		this->removeNode(arc4random()%list_size);
	}
	
    cnode p;
    p.n_particles=VANISHP;
    num_particles+=p.n_particles;
    num_vertices+=12*p.n_particles;
	
	for(int i=0;i<p.n_particles;i++){		
		
		p.particles[i].pos.x=x;
		p.particles[i].pos.y=y;
		p.particles[i].pos.z=z;
		
		int dx=(arc4random()%200-100);
		int dz=(arc4random()%200-100);
		int dy=(arc4random()%200-100);
        Vector vt=MakeVector(dx,dy,dz);
        NormalizeVector(&vt);
        
		p.particles[i].vx=vt.x*SPEED*3;
		p.particles[i].vz=vt.z*SPEED*3;
		p.particles[i].vy=vt.y*SPEED*3;
        
        
        
        p.particles[i].rx=randf(6.28);
        p.particles[i].rz=randf(6.28);
        p.particles[i].type=PT_FIREWORK;
        p.particles[i].vrx=randf(40)-20;
        p.particles[i].vrz=randf(40)-20;
        
		int pvbi=getPVBI2();
		p.particles[i].pvbi=pvbi;
		p.particles[i].size=randf(.1f)+.2f;
               int bf;
        
        bf=blockTypeFaces[TYPE_CLOUD][arc4random()%6]; 
        int ttc=color;
        if(ttc==0)               
            ttc=arc4random()%53+1;
        for(int j=0;j<12;j++){
            
            Vector clr=colorTable[ttc];
                pbuffer2[pvbi+j].colors[0]=(int)(clr.x*255)%256;
                pbuffer2[pvbi+j].colors[1]=(int)(clr.y*255)%256;
                pbuffer2[pvbi+j].colors[2]=(int)(clr.z*255)%256;
            if(pbuffer2[pvbi+j].colors[0]<50)pbuffer2[pvbi+j].colors[0]=50;
             if(pbuffer2[pvbi+j].colors[1]<50)pbuffer2[pvbi+j].colors[1]=50;
             if(pbuffer2[pvbi+j].colors[2]<50)pbuffer2[pvbi+j].colors[2]=50;
            
                
                
            if(pbuffer2[pvbi+j].colors[0]<200&&pbuffer2[pvbi+j].colors[1]<200&&pbuffer2[pvbi+j].colors[2]<200){
                int cmax=MAX(MAX(pbuffer2[pvbi+j].colors[0],pbuffer2[pvbi+j].colors[1]),pbuffer2[pvbi+j].colors[2]);
                for(int i=0;i<3;i++){
                    if(pbuffer2[pvbi+j].colors[i]==cmax){
                        pbuffer2[pvbi+j].colors[i]=255;
                    }
                }
            }
           
            
            pbuffer2[pvbi+j].colors[3]=255;		
        }
        CGPoint tp;
        
        
        tp=Resources::getResources->getBlockTexShort(bf);
        
        static int triTextureCustom[6]={0,0,
            0,1,
            1,1,};
        
        for (int f=0;f<4;f++){
            for(int v=0;v<3;v++){
                pbuffer2[pvbi+f*3+v].texs[0]=triTextureCustom[v*2];
                pbuffer2[pvbi+f*3+v].texs[1]=triTextureCustom[v*2+1]*tp.y+tp.x;
                
            }
            
            
        }
	}
	
	p.start_life=p.life=(LIFE);
    updateIndexes=true;	
	list[list_size]=p;
    for(int i=0;i<p.n_particles;i++){	
        list[list_size].particles[i]=p.particles[i];
    }
    list_size++;
    
}
void BlockBreak::addCreatureVanish2(float x,float z,float y,int color,int type){
    
	
    
	while(num_particles*12+VANISHP*12>max_bparticles){
		this->removeNode(arc4random()%list_size);
	}
	
    //NSLog(@"%d %d type:%d",x,z,type);
	cnode p;
    p.n_particles=VANISHP;
    num_particles+=p.n_particles;
    num_vertices+=12*p.n_particles;
	//p.next=NULL;
	for(int i=0;i<p.n_particles;i++){		
		
		p.particles[i].pos.x=x;
		p.particles[i].pos.y=y;
		p.particles[i].pos.z=z;
		
		int dx=(arc4random()%200-100);
		int dz=(arc4random()%200-100);
		int dy=(arc4random()%200-100);
		p.particles[i].vx=dx/100.0f*SPEED*6;
		p.particles[i].vz=dz/100.0f*SPEED*6;
		p.particles[i].vy=dy/100.0f*SPEED*6;
        
        p.particles[i].rx=randf(6.28);
        p.particles[i].rz=randf(6.28);
        p.particles[i].type=PT_NOGRAV;
        p.particles[i].vrx=randf(40)-20;
        p.particles[i].vrz=randf(40)-20;
        
		int pvbi=getPVBI2();
		p.particles[i].pvbi=pvbi;
		p.particles[i].size=randf(.1f)+.6f;
        bool useColorTex=false;
        int bf;
        if(type<NUM_CREATURES)
        bf=blockTypeFaces[TYPE_CLOUD][arc4random()%6];
        else{
           bf=blockTypeFaces[TYPE_CLOUD][arc4random()%6]; 
        }
        
        
        if(type==TYPE_FIREWORK){
            int ttc=color;
            if(ttc==0)
                ttc=arc4random()%53+1;
            for(int j=0;j<12;j++){
                
                Vector clr=colorTable[ttc];
                pbuffer2[pvbi+j].colors[0]=(int)(clr.x*255)%256;
                pbuffer2[pvbi+j].colors[1]=(int)(clr.y*255)%256;
                pbuffer2[pvbi+j].colors[2]=(int)(clr.z*255)%256;
               
                if(pbuffer2[pvbi+j].colors[0]<50)pbuffer2[pvbi+j].colors[0]=50;
                if(pbuffer2[pvbi+j].colors[1]<50)pbuffer2[pvbi+j].colors[1]=50;
                if(pbuffer2[pvbi+j].colors[2]<50)pbuffer2[pvbi+j].colors[2]=50;
                
                
                
                if(pbuffer2[pvbi+j].colors[0]<200&&pbuffer2[pvbi+j].colors[1]<200&&pbuffer2[pvbi+j].colors[2]<200){
                    int cmax=MAX(MAX(pbuffer2[pvbi+j].colors[0],pbuffer2[pvbi+j].colors[1]),pbuffer2[pvbi+j].colors[2]);
                    for(int i=0;i<3;i++){
                        if(pbuffer2[pvbi+j].colors[i]==cmax){
                            pbuffer2[pvbi+j].colors[i]=255;
                        }
                    }
                }
                
                
                pbuffer2[pvbi+j].colors[3]=255;
            }
           
            
        }else{
        for(int j=0;j<12;j++){
            if(color==0){  
                if(type<NUM_CREATURES){
                pbuffer2[pvbi+j].colors[0]=(int)(creatureColor[type][0])%256;
                pbuffer2[pvbi+j].colors[1]=(int)(creatureColor[type][1])%256;
                pbuffer2[pvbi+j].colors[2]=(int)(creatureColor[type][2])%256;
                }else{
                    pbuffer2[pvbi+j].colors[0]=(int)(blockColor[TYPE_DIRT][0])%256;
                    pbuffer2[pvbi+j].colors[1]=(int)(blockColor[TYPE_DIRT][1])%256;
                    pbuffer2[pvbi+j].colors[2]=(int)(blockColor[TYPE_DIRT][2])%256;
                    
                }
                
            }else if(color==-1){
                pbuffer2[pvbi+j].colors[0]=(int)(blockColor[type][0])%256;
                pbuffer2[pvbi+j].colors[1]=(int)(blockColor[type][1])%256;
                pbuffer2[pvbi+j].colors[2]=(int)(blockColor[type][2])%256;
                
            }else{
                Vector clr=colorTable[color];
                pbuffer2[pvbi+j].colors[0]=(int)(clr.x*255)%256;
                pbuffer2[pvbi+j].colors[1]=(int)(clr.y*255)%256;
                pbuffer2[pvbi+j].colors[2]=(int)(clr.z*255)%256;
                
            
                
            }
            
            pbuffer2[pvbi+j].colors[3]=255;		
        }
        }
        CGPoint tp;
        
        if(useColorTex){
             if(bf==TEX_GRASS_SIDE)
                bf=TEX_GRASS_SIDE_COLOR;
            else if(bf==TEX_TNT_SIDE)
                bf=TEX_TNT_SIDE_COLOR;
            else if(bf==TEX_TNT_TOP)
                bf=TEX_TNT_TOP_COLOR;
            else if(bf==TEX_BRICK)
                bf=TEX_BRICK_COLOR;
            else if(IS_NEWBLOCK_TEX(bf))
                bf=NEWBLOCK_COLOR_TEX(bf);   // D.2t
        }
        tp=Resources::getResources->getBlockTexShort(bf);
        
        static int triTextureCustom[6]={0,0,
            0,1,
            1,1,};
        
        for (int f=0;f<4;f++){
            for(int v=0;v<3;v++){
                pbuffer2[pvbi+f*3+v].texs[0]=triTextureCustom[v*2];
                pbuffer2[pvbi+f*3+v].texs[1]=triTextureCustom[v*2+1]*tp.y+tp.x;
                
            }
            
            
        }
	}
	
	p.start_life=p.life=(LIFE*1.0f/3);
    updateIndexes=true;	
	list[list_size]=p;
    for(int i=0;i<p.n_particles;i++){	
        list[list_size].particles[i]=p.particles[i];
    }
    list_size++;
    
	
}
void BlockBreak::addBlockBreak(int x,int z,int y,int type,int color){
	if(type<=0)return;
    
	while(num_particles*12+BREAKP*12>max_bparticles){
		this->removeNode(arc4random()%list_size);
	}
	
//NSLog(@"%d %d type:%d",x,z,type);
	cnode p;
    p.n_particles=BREAKP;
    num_particles+=p.n_particles;
    num_vertices+=12*p.n_particles;
	//p.next=NULL;
	for(int i=0;i<p.n_particles;i++){		
		
		p.particles[i].pos.x=x*BLOCK_SIZE+BLOCK_SIZE/2;
		p.particles[i].pos.y=y*BLOCK_SIZE+BLOCK_SIZE/2;
		p.particles[i].pos.z=z*BLOCK_SIZE+BLOCK_SIZE/2;
		
		int dx=(arc4random()%200-100);
		int dz=(arc4random()%200-100);
		int dy=(arc4random()%50)+50;
		p.particles[i].vx=dx/100.0f*SPEED;
		p.particles[i].vz=dz/100.0f*SPEED;
		p.particles[i].vy=dy/100.0f*SPEEDY;
        
        p.particles[i].rx=randf(6.28);
        p.particles[i].rz=randf(6.28);
        p.particles[i].type=PT_GRAV;
        p.particles[i].vrx=randf(20)-10;
        p.particles[i].vrz=randf(20)-10;
        
		int pvbi=getPVBI2();
		p.particles[i].pvbi=pvbi;
		p.particles[i].size=randf(.07f)+.05f;
        bool useColorTex=false;
         int bf=blockTypeFaces[type][arc4random()%6];
        if(blockinfo[type]&IS_ATLAS2)
            bf=blockTypeFaces[TYPE_CLOUD][arc4random()%6];
        for(int j=0;j<12;j++){
            if(color==0){
                if(bf==TEX_GRASS_SIDE||type==TYPE_GRASS3||type==TYPE_TNT||type==TYPE_BRICK||type==TYPE_VINE||type==TYPE_FIREWORK||IS_NEWBLOCK_TEX(bf))
                {
                    useColorTex=true;
                    pbuffer2[pvbi+j].colors[0]=255;
                    pbuffer2[pvbi+j].colors[1]=255;
                    pbuffer2[pvbi+j].colors[2]=255;
                }else{
                    pbuffer2[pvbi+j].colors[0]=(int)(blockColor[type][0])%256;
                    pbuffer2[pvbi+j].colors[1]=(int)(blockColor[type][1])%256;
                    pbuffer2[pvbi+j].colors[2]=(int)(blockColor[type][2])%256;
                }
            }else{
                Vector clr=colorTable[color];
                pbuffer2[pvbi+j].colors[0]=(int)(clr.x*255)%256;
                pbuffer2[pvbi+j].colors[1]=(int)(clr.y*255)%256;
                pbuffer2[pvbi+j].colors[2]=(int)(clr.z*255)%256;
                
            }
            
            pbuffer2[pvbi+j].colors[3]=255;		
        }
        CGPoint tp;
        
        if(useColorTex){
       if(bf==TEX_GRASS_SIDE)
            bf=TEX_GRASS_SIDE_COLOR;
        else if(bf==TEX_TNT_SIDE)
            bf=TEX_TNT_SIDE_COLOR;
        else if(bf==TEX_TNT_TOP)
            bf=TEX_TNT_TOP_COLOR;
        else if(bf==TEX_BRICK)
            bf=TEX_BRICK_COLOR;
        else if(IS_NEWBLOCK_TEX(bf))
            bf=NEWBLOCK_COLOR_TEX(bf);   // D.2t
        }
        tp=Resources::getResources->getBlockTexShort(bf);

        static int triTextureCustom[6]={0,0,
                                        0,1,
                                        1,1,};
        
        for (int f=0;f<4;f++){
            for(int v=0;v<3;v++){
                pbuffer2[pvbi+f*3+v].texs[0]=triTextureCustom[v*2];
                pbuffer2[pvbi+f*3+v].texs[1]=triTextureCustom[v*2+1]*tp.y+tp.x;
                             
            }
            
            
        }
	}
	
	p.start_life=p.life=LIFE;
    updateIndexes=true;	
	list[list_size]=p;
    for(int i=0;i<p.n_particles;i++){	
        list[list_size].particles[i]=p.particles[i];
    }
    list_size++;
    
	
}
void BlockBreak::clearAllEffects(){

	updateIndexes=true;
    num_particles=0;
    num_vertices=0;
    list_size=0;
	
	
	
}

