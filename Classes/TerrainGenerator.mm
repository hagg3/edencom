//
//  TerrainGenerator.m
//  prototype
//
//  Created by Ari Ronen on 10/23/10.
//  Copyright 2010 __MyCompanyName__. All rights reserved.
//

#import "TerrainGenerator.h"
#import "Terrain.h"
#import "Util.h"
#import "TerrainGen2.h"
#import "ClassicNoiseTables.h"
#import "MpWorld.h"   // MP.3a: the session overlay's landing hook
#include <vector>


float noise3(float vec[3]);
float noise2(float vec[2]);
static void init(void);
static void init_from_random(void);
static float noiseFast(float x, float y, float z);
static Terrain* ter;
extern block8* blockarray;
static const int TREE_SPACING=50;
int boundx;
int boundz;
block8 tblocks[(CHUNKS_PER_COLUMN_MAX*CHUNK_SIZE*2)*(CHUNK_SIZE*2)*(CHUNK_SIZE*2)];  //max-sized; indexing uses the runtime CHUNKS_PER_COLUMN
block8 tcolors[(CHUNKS_PER_COLUMN_MAX*CHUNK_SIZE*2)*(CHUNK_SIZE*2)*(CHUNK_SIZE*2)];  //max-sized; indexing uses the runtime CHUNKS_PER_COLUMN
TerrainGenerator::TerrainGenerator(Terrain* parent){
    init();
	ter=parent;
	LEVEL_SEED=0;
    genCaves=FALSE;
    classicState=CLASSIC_STATE_UNSET;
    classic_legacy_params(&classic,0);
    
	
	
}
void tgenInit(){
    init();
}
static TerrainChunk* column[CHUNKS_PER_COLUMN_MAX];  //max-sized, only CHUNKS_PER_COLUMN used

inline static int getCustomct(int x,int z,int y){
     return  tcolors[x*CHUNKS_PER_COLUMN*CHUNK_SIZE*CHUNK_SIZE*2*2+z*CHUNKS_PER_COLUMN*CHUNK_SIZE*2+y];
    
}
inline static int getCustomt(int x,int z,int y){  
    return  tblocks[x*CHUNKS_PER_COLUMN*CHUNK_SIZE*CHUNK_SIZE*2*2+z*CHUNKS_PER_COLUMN*CHUNK_SIZE*2+y];
    
  
	
}
inline static void setCustomt(int x,int z,int y,int type,int color){  
    
    tblocks[x*CHUNKS_PER_COLUMN*CHUNK_SIZE*CHUNK_SIZE*2*2+z*CHUNKS_PER_COLUMN*CHUNK_SIZE*2+y]=type;
    tcolors[x*CHUNKS_PER_COLUMN*CHUNK_SIZE*CHUNK_SIZE*2*2+z*CHUNKS_PER_COLUMN*CHUNK_SIZE*2+y]=color;         
    
}

inline static int getLandt(int x,int z,int y){  
 
    if(x>=0&&y>=0&&z>=0&&x<CHUNK_SIZE&&z<CHUNK_SIZE&&y<T_HEIGHT)
    
        return column[(int)y/CHUNK_SIZE]->pblocks[x*(CHUNK_SIZE*CHUNK_SIZE)+z*(CHUNK_SIZE)+(y-((int)y/CHUNK_SIZE)*CHUNK_SIZE)];
    return 0;
}


inline static void setLandt(int x,int z,int y,int type){  
  
    
    	column[(int)y/CHUNK_SIZE]->pblocks[x*(CHUNK_SIZE*CHUNK_SIZE)+z*(CHUNK_SIZE)+(y-((int)y/CHUNK_SIZE)*CHUNK_SIZE)]=type;
	
}

inline static void setColort(int x,int z,int y,int color){  
	column[(int)y/CHUNK_SIZE]->pcolors[x*(CHUNK_SIZE*CHUNK_SIZE)+z*(CHUNK_SIZE)+(y-((int)y/CHUNK_SIZE)*CHUNK_SIZE)]=color;
	
}
extern int g_offcx,g_offcz;

void TerrainGenerator::generateEmptyColumn(int cx,int cz){
    int ocx=cx;
	int ocz=cz;
    boundx=ocx*CHUNK_SIZE;
    boundz=ocz*CHUNK_SIZE;
    //memset(tblocks,0,sizeof(block8)*(CHUNKS_PER_COLUMN*CHUNK_SIZE*2)*(CHUNK_SIZE*2)*(CHUNK_SIZE*2));
   // memset(tcolors,0,sizeof(block8)*(CHUNKS_PER_COLUMN*CHUNK_SIZE*2)*(CHUNK_SIZE*2)*(CHUNK_SIZE*2));
	for(int cy=0;cy<CHUNKS_PER_COLUMN ;cy++){
		int bounds[6];
		bounds[0]=ocx*CHUNK_SIZE;
		bounds[1]=cy*CHUNK_SIZE;
		bounds[2]=ocz*CHUNK_SIZE;
		bounds[3]=(ocx+1)*CHUNK_SIZE;
		bounds[4]=(cy+1)*CHUNK_SIZE;
		bounds[5]=(ocz+1)*CHUNK_SIZE;
        TerrainChunk* chunk;
        TerrainChunk* old=ter->chunkTable[threeToOne(ocx,cy,ocz)];  //crash count: 1
        if(old){chunk=old;
            chunk->resetForReuse();
            chunk->setBounds(bounds);
        }
        else{
            printf("criticle errror re-allocating terrain chunk\n");
        }
        
       
        
		column[cy]=chunk;
        /*  if(bgthread){
         if(cy==0){
         // printg("adding from bgthread to chunkmap\n");
         }
         [ter readdChunk:chunk:ocx:cy:ocz];
         }else*/
		//[ter addChunk:chunk:ocx:cy:ocz:TRUE];
        
    }
   
    for(int x=0;x<CHUNK_SIZE;x++){
        for(int z=0;z<CHUNK_SIZE;z++){
            for(int y=0;y<T_HEIGHT;y++)
                GBLOCK(x+boundx,z+boundz,y)=
                
                column[(int)y/CHUNK_SIZE]->pblocks[x*(CHUNK_SIZE*CHUNK_SIZE)+z*(CHUNK_SIZE)+(y-((int)y/CHUNK_SIZE)*CHUNK_SIZE)];
            
        }
    }

    for(int cy=0;cy<CHUNKS_PER_COLUMN ;cy++){
        
        
        /*  if(bgthread){
         if(cy==0){
         // printg("adding from bgthread to chunkmap\n");
         }
         [ter readdChunk:chunk:ocx:cy:ocz];
         }else*/
        ter->addChunk(column[cy],ocx,cy,ocz,TRUE);
    }
}
void TerrainGenerator::generateColumn(int cx,int cz,BOOL bgthread){
	int ocx=cx;
	int ocz=cz;
    boundx=ocx*CHUNK_SIZE;
    boundz=ocz*CHUNK_SIZE;
    // H.4: these two cleared ~1 MB a column (at 256z) for the half-block experiment below, which
    // is commented out; nothing reads tblocks/tcolors any more (generator-plan.md §2.4).
    //memset(tblocks,0,sizeof(block8)*(CHUNKS_PER_COLUMN*CHUNK_SIZE*2)*(CHUNK_SIZE*2)*(CHUNK_SIZE*2));
    //memset(tcolors,0,sizeof(block8)*(CHUNKS_PER_COLUMN*CHUNK_SIZE*2)*(CHUNK_SIZE*2)*(CHUNK_SIZE*2));
    // H.4: a world whose GENERATOR record this build does not know gets void (never saved,
    // because generated columns are not `modified`); a Classic "2880 square" world gets void
    // outside the bundled map's square, behind Player::update's X.6 wall.
    if(LEVEL_SEED!=0&&(classicState==CLASSIC_STATE_VOID||
       (classicState==CLASSIC_STATE_SET&&classic.size==1&&
        (ocx<4096-GSIZE/CHUNK_SIZE/2||ocx>=4096+GSIZE/CHUNK_SIZE/2||ocz<4096-GSIZE/CHUNK_SIZE/2||ocz>=4096+GSIZE/CHUNK_SIZE/2)))){
        this->generateEmptyColumn(ocx,ocz);
        return;
    }
	for(int cy=0;cy<CHUNKS_PER_COLUMN ;cy++){
		int bounds[6];
		bounds[0]=ocx*CHUNK_SIZE;
		bounds[1]=cy*CHUNK_SIZE;
		bounds[2]=ocz*CHUNK_SIZE;
		bounds[3]=(ocx+1)*CHUNK_SIZE;
		bounds[4]=(cy+1)*CHUNK_SIZE;
		bounds[5]=(ocz+1)*CHUNK_SIZE;
        TerrainChunk* chunk;
        TerrainChunk* old=ter->chunkTable[threeToOne(ocx,cy,ocz)];  //crash count: 1
        if(old){chunk=old;
           chunk->resetForReuse();
            chunk->setBounds(bounds);
            //printf("found old chunk\n");
             
        }
        else{
            printf("criticle errror re-allocating terrain chunk\n");
        }
        
       
		column[cy]=chunk;
      /*  if(bgthread){
            if(cy==0){
               // printg("adding from bgthread to chunkmap\n");
            }
        [ter readdChunk:chunk:ocx:cy:ocz];	 
        }else*/
		//[ter addChunk:chunk:ocx:cy:ocz:TRUE];		
	}
    extern block8* blockz;
    extern color8* colorz;
   
   	if(FALSE&&LEVEL_SEED!=0){
        
        for(int x=0;x<CHUNK_SIZE;x++){
            for(int z=0;z<CHUNK_SIZE;z++){
                for(int y=0;y<T_HEIGHT;y++){
                    setLandt(x,z,y,BLOCK(((x+boundx+g_offcx)%T_SIZE),((g_offcz+z+boundz)%T_SIZE),y));
                    setColort(x,z,y,COLOR(((x+boundx+g_offcx)%T_SIZE),((g_offcz+z+boundz)%T_SIZE),y));
                }
              
                
            }
        }
        
        for(int x=0;x<CHUNK_SIZE;x++){
            for(int z=0;z<CHUNK_SIZE;z++){
                for(int y=0;y<T_HEIGHT;y++){
                    GBLOCK(x+boundx,z+boundz,y)=column[(int)y/CHUNK_SIZE]->pblocks[x*(CHUNK_SIZE*CHUNK_SIZE)+z*(CHUNK_SIZE)+(y-((int)y/CHUNK_SIZE)*CHUNK_SIZE)];
                    
                    
                }
            }
        }
        
        for(int cy=0;cy<CHUNKS_PER_COLUMN ;cy++){
            
            
            ter->addChunk(column[cy],ocx,cy,ocz,TRUE);
        }
        return;
        
    }else
    if(/*TRUE||*/LEVEL_SEED==0){   // H.4: only the flat world now; any other seed is Classic (below)
       for(int x=0;x<CHUNK_SIZE;x++){
            for(int z=0;z<CHUNK_SIZE;z++){
                 setLandt(x ,z ,0,TYPE_BEDROCK);
                // Emod: the slab is a fixed 64z-world slab (grass at 32) whatever the world height —
                // a 256z world's saved columns sit at 32 too, so T_HEIGHT/4 and T_HEIGHT/2 here put
                // the unsaved preview 96 blocks above them.
                for(int y=1;y<T_HEIGHT_DEFAULT/4;y++){
                    setLandt(x,z,y,TYPE_STONE);
                }
                for(int y=T_HEIGHT_DEFAULT/4;y<T_HEIGHT_DEFAULT/2;y++){
                    setLandt(x,z,y,TYPE_DIRT);
                }
              /*  int interval=4;
                if((x/interval)%2==0){
                    if((z/interval)%2==0){
                        setColort(x,z,T_HEIGHT/2,8);

                    }else
                        setColort(x,z,T_HEIGHT/2,44);

                }else{
                    if((z/interval)%2!=0){
                        setColort(x,z,T_HEIGHT/2,8);
                        
                    }else
                        setColort(x,z,T_HEIGHT/2,44);
                }*/
              //  setColort(x,z,T_HEIGHT/2,8);
                setLandt(x,z,T_HEIGHT_DEFAULT/2,TYPE_GRASS);
                
            }
        }
      /*  for(int x=0;x<CHUNK_SIZE*2;x++){
         

            for(int z=0;z<CHUNK_SIZE*2;z++){
                int h=sin((x+ocx*CHUNK_SIZE*2)/40.0f)*T_HEIGHT/2+T_HEIGHT+2;
                //int h2=sin((x+1+ocx*CHUNK_SIZE)/40.0f)*T_HEIGHT/4+T_HEIGHT/2+1;
               // int h3=sin((x-1+ocx*CHUNK_SIZE)/40.0f)*T_HEIGHT/4+T_HEIGHT/2+1;
                
                int h2=T_HEIGHT+2-T_HEIGHT/2;
                if(z>=10)h2=h;
                setCustomt(x ,z ,0,TYPE_BEDROCK,0);
                for(int y=1;y<T_HEIGHT/2;y++){
                    setCustomt(x,z,y,TYPE_STONE,0);
                }
               
                for(int y=T_HEIGHT/2;y<h2;y++){
                    setCustomt(x,z,y,TYPE_DIRT,0);
                }
                
                setCustomt(x,z,h2,TYPE_GRASS,0);
                if(h==T_HEIGHT/2+T_HEIGHT+2-1)
                setCustomt(x,z,h,TYPE_GRASS,0);
                
               // if(x==0&&z==0){
               //     for(int y=0;y<CHUNKS_PER_COLUMN;y++)
               //        setLandt(x,z,y*CHUNK_SIZE,TYPE_STONE);
               // }
            }
        }
        
        //   if(!bgthread){
        int dx[]={0,0,0,0,1,1,1,1};
        int dy[]={0,0,1,1,0,0,1,1};
        int dz[]={0,1,0,1,0,1,0,1};
        int n=0;
        for(int x=0;x<CHUNK_SIZE*2;x+=2){
            for(int z=0;z<CHUNK_SIZE*2;z+=2){
                for(int y=0;y<CHUNK_SIZE*CHUNKS_PER_COLUMN*2;y+=2){
                    int sblock=getCustomt(x,z,y);
                    int scolor=getCustomct(x,z,y);
                    BOOL isSolid=TRUE;
                    for(int d=1;d<8;d++){
                        if(getCustomt(x+dx[d],z+dz[d],y+dy[d])!=sblock||
                           getCustomct(x+dx[d],z+dz[d],y+dy[d])!=scolor){
                            isSolid=FALSE;
                            break;
                        }
                        
                    }
                    if(isSolid){
                        setLandt(x/2,z/2,y/2,sblock);
                        setColort(x/2,z/2,y/2,scolor);
                    }else{
                       setLandt(x/2,z/2,y/2,TYPE_STONE);
                        n++;
                        if(FALSE&&n<100){
                          setLandt(x/2,z/2,y/2,TYPE_CUSTOM);
                        int yy=y- ((int)(y/(CHUNK_SIZE*2))) *CHUNK_SIZE*2;
                        TerrainChunk* chunk=column[(int)(y/2/CHUNK_SIZE)];
                        SmallBlock* sb=malloc(sizeof(SmallBlock));
                       // memset(sb,0,sizeof(SmallBlock));
                    
                        chunk.psblocks[(x/2)*CHUNK_SIZE*CHUNK_SIZE+(z/2)*CHUNK_SIZE+yy/2]=sb;
                       
                        for(int d=0;d<8;d++){
                            int xx2=1-((x+dx[d])%2);
                            int zz2=1-((z+dz[d])%2);
                            int yy2=1-((y+dy[d])%2);
                            
                             sb->blocks[xx2*2*2+zz2*2+yy2]=getCustomt(x+dx[d],z+dz[d],y+dy[d]);
                            
                            sb->colors[xx2*2*2+zz2*2+yy2]=getCustomct(x+dx[d],z+dz[d],y+dy[d]);
                            
                            
                        }
                        }
                         
                    }
                    
                    //   return [column[(int)(y/CHUNK_SIZE)] setCustom:x:z:y-((int)(y/2)/CHUNK_SIZE)*CHUNK_SIZE*2];
                }
            }
        }*/
        // MP.3a: a multiplayer session's column is flat + what the server said differs (Classes/MpWorld.h),
        // written here so the GBLOCK copy and addChunk (R.2's typesEmpty) below see the final voxels.
        if(MpWorld::active())MpWorld::applyDelta(column,ocx,ocz);
              for(int x=0;x<CHUNK_SIZE;x++){
            for(int z=0;z<CHUNK_SIZE;z++){
                for(int y=0;y<T_HEIGHT;y++)
                    GBLOCK(x+boundx,z+boundz,y)=
                    
                    column[(int)y/CHUNK_SIZE]->pblocks[x*(CHUNK_SIZE*CHUNK_SIZE)+z*(CHUNK_SIZE)+(y-((int)y/CHUNK_SIZE)*CHUNK_SIZE)];
                
            }			
        }
        
        for(int cy=0;cy<CHUNKS_PER_COLUMN ;cy++){
           
          
            /*  if(bgthread){
             if(cy==0){
             // printg("adding from bgthread to chunkmap\n");
             }
             [ter readdChunk:chunk:ocx:cy:ocz];	 
             }else*/
            ter->addChunk(column[cy],ocx,cy,ocz,TRUE);
        }
      //  }
        return;
    }
    {
        // H.4: the Classic generator (generator-plan.md §3). A world with no GENERATOR record
        // reaching here is a 1.x random-seed world (seed not 0 / 333333): FileManager sets its
        // legacy profile at load; UNSET is only a world loaded by a path that did not.
        static block8 ctypes[CHUNK_SIZE*CHUNK_SIZE*T_HEIGHT_MAX];
        static color8 ccolors[CHUNK_SIZE*CHUNK_SIZE*T_HEIGHT_MAX];
        if(classicState!=CLASSIC_STATE_SET)classic_legacy_params(&classic,LEVEL_SEED);
        classic_generate_window(classic,boundx,boundz,CHUNK_SIZE,CHUNK_SIZE,T_HEIGHT,ctypes,ccolors,TRUE);
        for(int x=0;x<CHUNK_SIZE;x++){
            for(int z=0;z<CHUNK_SIZE;z++){
                const block8* ts=&ctypes[(x*CHUNK_SIZE+z)*T_HEIGHT];
                const color8* cs=&ccolors[(x*CHUNK_SIZE+z)*T_HEIGHT];
                for(int y=0;y<T_HEIGHT;y++){
                    int i=x*(CHUNK_SIZE*CHUNK_SIZE)+z*(CHUNK_SIZE)+(y-((int)y/CHUNK_SIZE)*CHUNK_SIZE);
                    column[(int)y/CHUNK_SIZE]->pblocks[i]=ts[y];
                    column[(int)y/CHUNK_SIZE]->pcolors[i]=cs[y];
                    GBLOCK(x+boundx,z+boundz,y)=ts[y];
                }
            }
        }
        for(int cy=0;cy<CHUNKS_PER_COLUMN ;cy++){
            ter->addChunk(column[cy],ocx,cy,ocz,TRUE);
        }
        return;
    }
#if 0   // H.4: the surviving 1.x body, superseded by classic_generate_window() (archaeology, rule 9)
    float var=3;  //how much variance in heightmap?
    //LEVEL_SEED=0;
	
	
	const int offsety=T_HEIGHT/2;
		for(int x=0;x<CHUNK_SIZE;x++){ //Heightmap
		for(int z=0;z<CHUNK_SIZE;z++){
            int h;
            
            float n=offsety;
            float FREQ=2.0f;
            float AMPLITUDE=4.0f;			
            for(int i=0;i<10;i++){
                float vec[2]={(float)FREQ*(x+cx*CHUNK_SIZE+LEVEL_SEED)/NOISE_CONSTANT,
                    (float)FREQ*(z+cz*CHUNK_SIZE+LEVEL_SEED)/NOISE_CONSTANT};
                n+=noise2(vec)*(AMPLITUDE)*var;
                FREQ*=2;
                AMPLITUDE/=2;
            }		
            h=(int)roundf(n);
            if(h-1>=T_HEIGHT){
                NSLog(@"NONONO9");
            }
            
			int FORMATION_HEIGHT=h-6;//how deep should the 3D noise apply
									//The deeper the 3D noise and the more variance in heightmap,
									//The more intense the terrain is
			
			for(int y=0;y<h;y++){	
				if(y<FORMATION_HEIGHT){
					if(y>(h%2+1)&&y<FORMATION_HEIGHT-16){
                        if(!genCaves){
                            setLandt(x ,z ,y ,TYPE_STONE);
                            continue;
                        }
						float n3=0;
						float FREQ3=4.0f;
						float AMPLITUDE3=0.25f;			
						for(int i=0;i<3;i++){
							float vec[3]={(float)FREQ3*(x+cx*CHUNK_SIZE+LEVEL_SEED)/NOISE_CONSTANT
								,(float)FREQ3*(z+cz*CHUNK_SIZE+LEVEL_SEED)/NOISE_CONSTANT,
								(float)FREQ3*(y+LEVEL_SEED)/NOISE_CONSTANT};
							n3+=noise3(vec)*(AMPLITUDE3);
							FREQ3*=2;
							AMPLITUDE3/=2;
						}	
						
						if(n3>0){		
							if(n3<=0.01f)
								setLandt(x ,z ,y ,TYPE_DARK_STONE);
							else
								setLandt(x ,z ,y ,TYPE_STONE);
							
						}else {
                            
							setLandt(x ,z ,y ,TYPE_NONE);
						}
						
											
					}else{
						setLandt(x ,z ,y ,TYPE_STONE);
					}
				}else{
                    float n3=0;
                   
					
					float FREQ3=3.0f;
					float AMPLITUDE3=0.5f;			
					for(int i=0;i<3;i++){
						float vec[3]={(float)FREQ3*(x+cx*CHUNK_SIZE+LEVEL_SEED)/NOISE_CONSTANT
							,(float)FREQ3*(z+cz*CHUNK_SIZE+LEVEL_SEED)/NOISE_CONSTANT,
							(float)FREQ3*(y+LEVEL_SEED)/NOISE_CONSTANT};
						n3+=noise3(vec)*(AMPLITUDE3);
						FREQ3*=2;
						AMPLITUDE3/=2;
					
                    }
					if(n3<0.07f){				
						setLandt(x ,z ,y ,TYPE_DIRT);				
					}
				}
					
			//	setLandt(x :z :y :TYPE_DIRT];	
				
				
			}
			
			//if(self getLandc(<#int x#>, <#int z#>, <#int y#>)
			//setLandt(x :z :h-1 :TYPE_GRASS];		
						
		}
		
	}
	
	
	/*
	for(int x=0;x<ter.width;x++){
		for(int z=0;z<ter.depth;z++){
			for(int y=0;y<ter.height;y++){
				float n=0;
				float FREQ=4.0f;
				float AMPLITUDE=0.5f;			
				for(int i=0;i<3;i++){
					float vec[3]={(float)FREQ*x/(ter.width),(float)FREQ*z/(ter.depth),(float)FREQ*y/(ter.height)};
					n+=noise3(vec)*(AMPLITUDE);
					FREQ*=2;
					AMPLITUDE/=2;
				}			
				if(n<0.0f){				
				e	[ter setLand:x :z :y :TYPE_DIRT];				
				}			
			}		
		}	
	}*/
	
	for(int x=0;x<CHUNK_SIZE;x++){
		for(int z=0;z<CHUNK_SIZE;z++){
						 setLandt(x ,z ,0,TYPE_BEDROCK);
			
			for(int y=0;y<T_HEIGHT;y++){
				if(getLandt(x ,z ,y)==TYPE_NONE){
					if(getLandt(x ,z ,y-1)==TYPE_DIRT){	
                        if(arc4random()%2==0&&(x+x%10)%20<4&&(z+z%10)%20<4){
							setLandt(x ,z ,y ,TYPE_FLOWER);	
							setLandt(x ,z ,y-1 ,TYPE_GRASS);	
						}else if(arc4random()%2==0){
							setLandt(x ,z ,y-1 ,TYPE_GRASS);	
						}else{							
							setLandt(x ,z ,y-1 ,TYPE_GRASS2);							
						}
                         //setColort(x ,z ,y-1 ,22+18);
                        //setLandt(x ,z ,y-1 ,TYPE_GRASS);	
					}
				}else{
					
					
					
				}
				
			}
			
		}
	}
    
    	for(int x=2;x<CHUNK_SIZE-2;x++){ //Trees
		for(int z=2;z<CHUNK_SIZE-2;z++){
			for(int y=1;y<T_HEIGHT-1;y++){
				if(getLandt(x ,z ,y)==TYPE_GRASS||getLandt(x ,z ,y)==TYPE_GRASS2){
					this->placeTree(x ,z ,y+1);
				}
			}
		}
	}
    int cloud=arc4random()%5;
   
    if(cloud==0){
        this->generateCloud();
    }
	for(int cy=CHUNKS_PER_COLUMN-1;cy>=0 ;cy--){
       
        //[column[cy] setShadow];
    }
   
       
        for(int x=0;x<CHUNK_SIZE;x++){
            for(int z=0;z<CHUNK_SIZE;z++){
                for(int y=0;y<T_HEIGHT;y++){
                 GBLOCK(x+boundx,z+boundz,y)=column[(int)y/CHUNK_SIZE]->pblocks[x*(CHUNK_SIZE*CHUNK_SIZE)+z*(CHUNK_SIZE)+(y-((int)y/CHUNK_SIZE)*CHUNK_SIZE)];
                               
          
                }
            }
        }
  
    for(int cy=0;cy<CHUNKS_PER_COLUMN ;cy++){
        
        
              ter->addChunk(column[cy],ocx,cy,ocz,TRUE);
    }

#endif
}
void TerrainGenerator::generateCloud(){
    int num=arc4random()%4+4;
    for(int rep=0;rep<num;rep++){
        int x=arc4random()%7;
        int y=arc4random()%7;
        int w=arc4random()%(CHUNK_SIZE-x);
        int h=arc4random()%(CHUNK_SIZE-y);
        if(w<4)w=4;
        if(h<4)h=4;
        int d=arc4random()%2+2;
        for(int i=0;i<w;i++)
            for(int j=0;j<h;j++){
                setLandt(i+x,j+y,T_HEIGHT-d,TYPE_CLOUD);
            }
    }

}
/*int xs=x/SPACING;
 int xsr=xs+1;
 int zs=z/SPACING;
 int zsr=zs+1;			
 
 float fx=(float)x/SPACING;
 float fz=(float)z/SPACING;
 float dx=xsr-fx;
 float dz=zsr-fz;
 float ww1=(dx+dz)/2;
 float ww2=((1-dx)+dz)/2;
 float ww3=(dx+(1-dz))/2;
 float ww4=((1-dx)+(1-dz))/2;
 
 float h=ww1*heights[xs][zs]+ww2*heights[xsr][zs]+
 ww3*heights[xs][zsr]+ww4*heights[xsr][zsr];
 h/=2;
 
 [ter setLand:x :z :0 :TYPE_BEDROCK];
 for(int k=1;k<n-1;k++){
 [ter setLand:x :z :k :TYPE_DIRT];
 
 }
 
 [ter setLand:x :z :n-1 :TYPE_GRASS];
 */
void TerrainGenerator::placeTree(int x,int z,int y){
	//
	if(arc4random()%TREE_SPACING!=0)return;
	int tree_height=arc4random()%3+6;
	if(y+tree_height>=T_HEIGHT) return;
	for(int i=x-1;i<=x+1;i++){
		for(int j=z-1;j<=z+1;j++){
			int type=getLandt(i ,j ,y-1);
			if(!(type==TYPE_GRASS||type==TYPE_GRASS2||type==TYPE_DIRT))
				return;		
            if(getLandt(i ,j ,y)!=TYPE_NONE)return;
		}
	}
	for(int i=x-1;i<=x+1;i++){
		for(int j=z-1;j<=z+1;j++){
			for(int k=y;k<y+tree_height;k++){
				int type=getLandt(i ,j ,k);
				if(type==TYPE_NONE||type==TYPE_LEAVES)
					continue;
				break;
							
				
			}
		}
		
	}
	
	//NSLog(@"placing tree %d %d %d",x,z,y+i);
	for(int i=0;i<3*tree_height/4;i++){
		setLandt(x ,z ,y+i ,TYPE_TREE);
	}
	int color=arc4random()%4;
	int ct[4]={0,19,20,21};
    int type=TYPE_LEAVES;
	
	for(int i=x-2;i<=x+2;i++){
		for(int j=z-2;j<=z+2;j++){
			for(int k=y+2*tree_height/3;k<tree_height+y;k++){
				if(getLandt(i ,j ,k)!=TYPE_TREE){
					if(i==x-2||i==x+2||j==z-2||j==z+2){
                        if((i==x-2||i==x+2)&&(j==z-2||j==z+2)&&(k==y+2*tree_height/3||k==y+tree_height-1)){
                        }else
						if(arc4random()%2==0){
							setLandt(i ,j ,k ,type);
                            setColort(i ,j ,k ,ct[color]);
                        }
					}
					else {
						setLandt(i ,j ,k ,type);
                        setColort(i ,j ,k ,ct[color]);
					}
					
				}
			}
		}
	}
	
}






#define lerp(q, w, y) ( w + q * (y - w) )


static inline float grad(int hash, float x, float y, float z) {
    int h = hash & 15;                      // CONVERT LO 4 BITS OF HASH CODE
    float u = h<8 ? x : y,                 // INTO 12 GRADIENT DIRECTIONS.
    v = h<4 ? y : h==12||h==14 ? x : z;
    return ((h&1) == 0 ? u : -u) + ((h&2) == 0 ? v : -v);
}
#define fade(t) (t * t * t * (t * (t * 6 - 15) + 10))


static int pp[512];


static const int permutation[] = { 151,160,137,91,90,15,
    131,13,201,95,96,53,194,233,7,225,140,36,103,30,69,142,8,99,37,240,21,10,23,
    190, 6,148,247,120,234,75,0,26,197,62,94,252,219,203,117,35,11,32,57,177,33,
    88,237,149,56,87,174,20,125,136,171,168, 68,175,74,165,71,134,139,48,27,166,
    77,146,158,231,83,111,229,122,60,211,133,230,220,105,92,41,55,46,245,40,244,
    102,143,54, 65,25,63,161, 1,216,80,73,209,76,132,187,208, 89,18,169,200,196,
    135,130,116,188,159,86,164,100,109,198,173,186, 3,64,52,217,226,250,124,123,
    5,202,38,147,118,126,255,82,85,212,207,206,59,227,47,16,58,17,182,189,28,42,
    223,183,170,213,119,248,152, 2,44,154,163, 70,221,153,101,155,167, 43,172,9,
    129,22,39,253, 19,98,108,110,79,113,224,232,178,185, 112,104,218,246,97,228,
    251,34,242,193,238,210,144,12,191,179,162,241, 81,51,145,235,249,14,239,107,
    49,192,214, 31,181,199,106,157,184, 84,204,176,115,121,50,45,127, 4,150,254,
    138,236,205,93,222,114,67,29,24,72,243,141,128,195,78,66,215,61,156,180
};


static inline float noiseFast(float x, float y, float z) {
    
        int X = (int)floorf(x) & 255,                  // FIND UNIT CUBE THAT
        Y = (int)floorf(y) & 255,                  // CONTAINS POINT.
        Z = (int)floorf(z) & 255;
        x -= floorf(x);                                // FIND RELATIVE X,Y,Z
        y -= floorf(y);                                // OF POINT IN CUBE.
        z -= floorf(z);
        float u = fade(x),                                // COMPUTE FADE CURVES
        v = fade(y),                                // FOR EACH OF X,Y,Z.
        w = fade(z);
    int A = pp[X  ]+Y;
    int AA = pp[A]+Z;
    int AB = pp[A+1]+Z;      // HASH COORDINATES OF
    int BVAR = pp[X+1]+Y;
    int BA = pp[BVAR]+Z;
    int BB = pp[BVAR+1]+Z;      // THE 8 CUBE CORNERS,
        
        return lerp(w, lerp(v, lerp(u, grad(pp[AA  ], x  , y  , z   ),  // AND ADD
                                    grad(pp[BA  ], x-1, y  , z   )), // BLENDED
                            lerp(u, grad(pp[AB  ], x  , y-1, z   ),  // RESULTS
                                 grad(pp[BB  ], x-1, y-1, z   ))),// FROM  8
                    lerp(v, lerp(u, grad(pp[AA+1], x  , y  , z-1 ),  // CORNERS
                                 grad(pp[BA+1], x-1, y  , z-1 )), // OF CUBE
                         lerp(u, grad(pp[AB+1], x  , y-1, z-1 ),
                              grad(pp[BB+1], x-1, y-1, z-1 ))));
    
}


#define B 0x100
#define BM 0xff

#define N 0x1000
#define NP 12   /* 2^N */
#define NM 0xfff

static int p[B + B + 2];
static float g3[B + B + 2][3];
static float g2[B + B + 2][2];
static float g1[B + B + 2];
static int start = 0;

#define s_curve(t) ( t * t * (3. - 2. * t) )
#define setup(i,b0,b1,r0,r1)\
t = vec[i] + N;\
b0 = ((int)t) & BM;\
b1 = (b0+1) & BM;\
r0 = t - (int)t;\
r1 = r0 - 1.;

double noise1(double arg)
{
	int bx0, bx1;
	float rx0, rx1, sx, t, u, v, vec[1];
	
	vec[0] = arg;
	if (start) {
		start = 0;
		init();
	}
	
	setup(0, bx0,bx1, rx0,rx1);
	
	sx = s_curve(rx0);
	
	u = rx0 * g1[ p[ bx0 ] ];
	v = rx1 * g1[ p[ bx1 ] ];
	
	return lerp(sx, u, v);
}

float noise2(float vec[2])
{
	int bx0, bx1, by0, by1, b00, b10, b01, b11;
	float rx0, rx1, ry0, ry1, *q, sx, sy, a, b, t, u, v;
	register int i, j;
	
	if (start) {
		start = 0;
		init();
	}
	
	setup(0, bx0,bx1, rx0,rx1);
	setup(1, by0,by1, ry0,ry1);
	
	i = p[ bx0 ];
	j = p[ bx1 ];
	
	b00 = p[ i + by0 ];
	b10 = p[ j + by0 ];
	b01 = p[ i + by1 ];
	b11 = p[ j + by1 ];
	
	sx = s_curve(rx0);
	sy = s_curve(ry0);
	
#define at2(rx,ry) ( rx * q[0] + ry * q[1] )
	
	q = g2[ b00 ] ; u = at2(rx0,ry0);
	q = g2[ b10 ] ; v = at2(rx1,ry0);
	a = lerp(sx, u, v);
	
	q = g2[ b01 ] ; u = at2(rx0,ry1);
	q = g2[ b11 ] ; v = at2(rx1,ry1);
	b = lerp(sx, u, v);
	
	return lerp(sy, a, b);
}

float noise3(float vec[3])
{
	int bx0, bx1, by0, by1, bz0, bz1, b00, b10, b01, b11;
	float rx0, rx1, ry0, ry1, rz0, rz1, *q, sy, sz, a, b, c, d, t, u, v;
	register int i, j;
	
	if (start) {
		start = 0;
		init();
	}
	
	setup(0, bx0,bx1, rx0,rx1);
	setup(1, by0,by1, ry0,ry1);
	setup(2, bz0,bz1, rz0,rz1);
	
	i = p[ bx0 ];
	j = p[ bx1 ];
	
	b00 = p[ i + by0 ];
	b10 = p[ j + by0 ];
	b01 = p[ i + by1 ];
	b11 = p[ j + by1 ];
	
	t  = s_curve(rx0);
	sy = s_curve(ry0);
	sz = s_curve(rz0);
	
#define at3(rx,ry,rz) ( rx * q[0] + ry * q[1] + rz * q[2] )
	
	q = g3[ b00 + bz0 ] ; u = at3(rx0,ry0,rz0);
	q = g3[ b10 + bz0 ] ; v = at3(rx1,ry0,rz0);
	a = lerp(t, u, v);
	
	q = g3[ b01 + bz0 ] ; u = at3(rx0,ry1,rz0);
	q = g3[ b11 + bz0 ] ; v = at3(rx1,ry1,rz0);
	b = lerp(t, u, v);
	
	c = lerp(sy, a, b);
	
	q = g3[ b00 + bz1 ] ; u = at3(rx0,ry0,rz1);
	q = g3[ b10 + bz1 ] ; v = at3(rx1,ry0,rz1);
	a = lerp(t, u, v);
	
	q = g3[ b01 + bz1 ] ; u = at3(rx0,ry1,rz1);
	q = g3[ b11 + bz1 ] ; v = at3(rx1,ry1,rz1);
	b = lerp(t, u, v);
	
	d = lerp(sy, a, b);
	
	return lerp(sz, c, d);
}

static void normalize2(float v[2])
{
	float s;
	
	s = sqrt(v[0] * v[0] + v[1] * v[1]);
	v[0] = v[0] / s;
	v[1] = v[1] / s;
}

static void normalize3(float v[3])
{
	float s;
	
	s = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
	v[0] = v[0] / s;
	v[1] = v[1] / s;
	v[2] = v[2] / s;
}

// H.4: the tables are baked (Classes/ClassicNoiseTables.h, generator-plan.md §1.7). They are
// exactly what init_from_random() below produced on iOS/macOS/glibc from unseeded random() --
// which is what makes 1.x worlds regenerate -- and no longer depend on the platform's random()
// or on whoever called it first. g1 is not baked: noise1() has no caller.
static void init(void)
{
    for (int i=0; i < 256 ; i++) pp[256+i] = pp[i] = permutation[i];
    for (int i=0; i < B + B + 2; i++) {
        p[i] = kClassicP[i];
        memcpy(g2[i], &kClassicG2Bits[i*2], sizeof(float)*2);
        memcpy(g3[i], &kClassicG3Bits[i*3], sizeof(float)*3);
    }
}

// The original init(), kept as archaeology and for --classic-selftest's first mutation (tables
// from a reseeded random(), i.e. what the Windows shim's arc4random-based random() produced).
static void init_from_random(void)
{
    for (int i=0; i < 256 ; i++) pp[256+i] = pp[i] = permutation[i];
	int i, j, k;
	
	for (i = 0 ; i < B ; i++) {
		p[i] = i;
		
		g1[i] = (float)((random() % (B + B)) - B) / B;
		
		for (j = 0 ; j < 2 ; j++)
			g2[i][j] = (float)((random() % (B + B)) - B) / B;
		normalize2(g2[i]);
		
		for (j = 0 ; j < 3 ; j++)
			g3[i][j] = (float)((random() % (B + B)) - B) / B;
		normalize3(g3[i]);
	}
	
	while (--i) {
		k = p[i];
		p[i] = p[j = random() % B];
		p[j] = k;
	}
	
	for (i = 0 ; i < B + 2 ; i++) {
		p[B + i] = p[i];
		g1[B + i] = g1[i];
		for (j = 0 ; j < 2 ; j++)
			g2[B + i][j] = g2[i][j];
		for (j = 0 ; j < 3 ; j++)
			g3[B + i][j] = g3[i][j];
	}
}



// =============================================================================================
// H.4: the Classic generator (WORKING/feature-archaeology-2026-10-13/generator-plan.md §3).
//
// At scale 1x, Classic hilliness and caves off, the terrain below is the 1.x generateColumn
// (the #if 0 body above) bit for bit, which is what lets a 1.x random-seed world's unexplored
// columns regenerate exactly beside its saved ones (§1.2). Keep the float expressions in the
// 1.x order -- `(float)FREQ*(coord+seed)/NOISE_CONSTANT`, `noise*AMPLITUDE*var`, roundf -- and
// this translation unit compiled with -ffp-contract=off (native/CMakeLists.txt): one fused
// multiply-add moves a cell. --classic-selftest and native.yml's hash pins are the proof.
// Decoration (grass kinds, flowers, trees, clouds) was arc4random() in 1.x and is a counter
// hash here, so a column regenerates the same every time it streams in.
// =============================================================================================
#include "EdenWorldStore.h"   // emod::crc32_of (the .eden mirror's CRC)

static int g_classic_mutation=0;
void classic_set_mutation(int m){   // 1 tables, 2 trunc, 3 noborder (TerrainGenerator.h)
    g_classic_mutation=m;
    if(m==1){ for(int i=0;i<7;i++)random(); init_from_random(); }   // an earlier random() caller
    else init();
}

static const float kClassicHilliness[5]={1.0f,2.0f,3.0f,4.5f,6.0f};   // the editor's variance steps
static const int kClassicTreeSpacing[4]={0,80,50,25};                // None / Sparse / Classic / Dense
static const int kClassicLeafColors[4]={0,19,20,21};                 // placeTree's ct[]

void classic_legacy_params(ClassicGen* g,int seed){
    memset(g,0,sizeof(*g));
    g->seed=seed; g->scale4=4; g->hilliness=2; g->caves=CLASSIC_CAVES_OFF; g->trees=2;
    g->flowers=1; g->clouds=1; g->size=0; g->profile=CLASSIC_PROFILE_LEGACY; g->flags=0;
}
void classic_default_params(ClassicGen* g,int seed){
    classic_legacy_params(g,seed);
    g->profile=CLASSIC_PROFILE_NEW;
}

// ---- the record (generator-plan.md §4.1) and its .eden mirror (§4.2) -------------------------
void classic_encode_record(const ClassicGen& g,unsigned char out[CLASSIC_RECORD_BYTES]){
    memset(out,0,CLASSIC_RECORD_BYTES);
    out[0]=1; out[1]=0;                 // generator: 1 = Classic (u16 LE)
    out[2]=1; out[3]=0;                 // revision 1
    uint32_t s=(uint32_t)g.seed;
    out[4]=s&0xff; out[5]=(s>>8)&0xff; out[6]=(s>>16)&0xff; out[7]=(s>>24)&0xff;
    out[8]=g.scale4; out[9]=g.hilliness; out[10]=g.caves; out[11]=g.trees;
    out[12]=g.flowers; out[13]=g.clouds; out[14]=g.size; out[15]=g.profile; out[16]=g.flags;
}
int classic_decode_record(const unsigned char* in,size_t len,ClassicGen* g){
    if(len!=CLASSIC_RECORD_BYTES)return 0;
    if(in[0]!=1||in[1]!=0||in[2]!=1||in[3]!=0)return 0;
    for(int i=17;i<CLASSIC_RECORD_BYTES;i++)if(in[i])return 0;   // a revision this build does not know
    ClassicGen t; memset(&t,0,sizeof(t));
    t.seed=(int)((uint32_t)in[4]|((uint32_t)in[5]<<8)|((uint32_t)in[6]<<16)|((uint32_t)in[7]<<24));
    t.scale4=in[8]; t.hilliness=in[9]; t.caves=in[10]; t.trees=in[11];
    t.flowers=in[12]; t.clouds=in[13]; t.size=in[14]; t.profile=in[15]; t.flags=in[16];
    if(!(t.scale4==4||t.scale4==6||t.scale4==8||t.scale4==12||t.scale4==16))return 0;
    if(t.hilliness>4||t.caves>2||t.trees>3||t.flowers>1||t.clouds>1||t.size>1||t.profile>1||(t.flags&~1))return 0;
    *g=t;
    return 1;
}
void classic_encode_mirror(const ClassicGen& g,unsigned char out[36]){
    memcpy(out,"EMG1",4);
    classic_encode_record(g,out+4);
    uint32_t c=emod::crc32_of(out,32);
    out[32]=c&0xff; out[33]=(c>>8)&0xff; out[34]=(c>>16)&0xff; out[35]=(c>>24)&0xff;
}
int classic_decode_mirror(const unsigned char in[36],ClassicGen* g){
    if(memcmp(in,"EMG1",4)!=0)return 0;
    uint32_t c=emod::crc32_of(in,32);
    if(in[32]!=(c&0xff)||in[33]!=((c>>8)&0xff)||in[34]!=((c>>16)&0xff)||in[35]!=((c>>24)&0xff))return 0;
    return classic_decode_record(in+4,CLASSIC_RECORD_BYTES,g);
}

// ---- decoration RNG: SplitMix64 over (seed, x, z, y, salt) ------------------------------------
enum { CS_GRASS=1, CS_FLOWER, CS_ROOT, CS_TREE_H, CS_LEAF, CS_LEAF_COLOR, CS_CLOUD, CS_CAVE_LO };
static inline uint64_t cg_mix(uint64_t z){
    z+=0x9E3779B97F4A7C15ull;
    z=(z^(z>>30))*0xBF58476D1CE4E5B9ull;
    z=(z^(z>>27))*0x94D049BB133111EBull;
    return z^(z>>31);
}
static inline uint32_t cg_rand(const ClassicGen& g,int wx,int wz,int y,int salt){
    uint64_t h=cg_mix((uint64_t)(uint32_t)g.seed);
    h=cg_mix(h^(uint64_t)(uint32_t)wx);
    h=cg_mix(h^(uint64_t)(uint32_t)wz);
    h=cg_mix(h^(((uint64_t)(uint32_t)y<<32)|(uint32_t)salt));
    return (uint32_t)(h>>32);
}
static inline int cg_floordiv(int a,int b){ return a>=0?a/b:-((-a+b-1)/b); }

// ---- noise ----------------------------------------------------------------------------------
// 3 octaves of noise3 at integer world coords, exactly as the 1.x skin/cave loops built `vec`.
static float cg_noise3sum(float FREQ3,float AMPLITUDE3,int ax,int ay,int az){
    float n3=0;
    for(int i=0;i<3;i++){
        float vec[3]={(float)FREQ3*ax/NOISE_CONSTANT,(float)FREQ3*az/NOISE_CONSTANT,(float)FREQ3*ay/NOISE_CONSTANT};
        n3+=noise3(vec)*(AMPLITUDE3);
        FREQ3*=2;
        AMPLITUDE3/=2;
    }
    return n3;
}

int classic_height(const ClassicGen& g,int wx,int wz,int height){
    float var=kClassicHilliness[g.hilliness];
    float n=(float)(8*g.scale4);                 // offsety: 32*s (1.x: 32, never T_HEIGHT/2 -- §3.1)
    float FREQ=8.0f/g.scale4;                    // 2/s
    float AMPLITUDE=(float)g.scale4;             // 4*s
    int ax=wx+g.seed,az=wz+g.seed;
    for(int i=0;i<10;i++){
        float vec[2]={(float)FREQ*ax/NOISE_CONSTANT,(float)FREQ*az/NOISE_CONSTANT};
        n+=noise2(vec)*(AMPLITUDE)*var;
        FREQ*=2;
        AMPLITUDE/=2;
    }
    int h=g_classic_mutation==2?(int)n:(int)roundf(n);
    if(h<3)h=3;
    if(h>height-2)h=height-2;
    return h;
}

// Skin and cave noise. At 1x/1.5x each cell is sampled (the 1.x path, never latticed); at
// 2x/3x/4x on a lattice of step L=floor(s) anchored to world coords, interpolated in x, z then y
// (§2.2), so neighbouring columns share nodes. The node box memoises one window's nodes; a node
// outside it (a probe) is computed directly -- the same value either way.
struct CgField{
    float f0,a0; int L;
    int nx0,nz0,NX,NZ,NY;          // the cached node box
    std::vector<float> v; std::vector<unsigned char> have;
};
static void cg_field_init(CgField* f,const ClassicGen& g,float f0,float a0,int wx0,int wz0,int W,int D,int height){
    f->f0=f0; f->a0=a0; f->L=g.scale4/4;
    if(f->L<=1){ f->NX=f->NZ=f->NY=0; return; }
    f->nx0=cg_floordiv(wx0,f->L); f->nz0=cg_floordiv(wz0,f->L);
    f->NX=cg_floordiv(wx0+W-1,f->L)-f->nx0+2;
    f->NZ=cg_floordiv(wz0+D-1,f->L)-f->nz0+2;
    f->NY=(height-1)/f->L+2;
    size_t n=(size_t)f->NX*f->NZ*f->NY;
    f->v.assign(n,0.0f); f->have.assign(n,0);
}
static float cg_node(CgField* f,const ClassicGen& g,int i,int j,int k){   // node (i,k) horizontal, j vertical
    int ci=i-f->nx0,ck=k-f->nz0;
    if(f->NX&&ci>=0&&ci<f->NX&&ck>=0&&ck<f->NZ&&j>=0&&j<f->NY){
        size_t idx=((size_t)ci*f->NZ+ck)*f->NY+j;
        if(!f->have[idx]){ f->v[idx]=cg_noise3sum(f->f0,f->a0,i*f->L+g.seed,j*f->L+g.seed,k*f->L+g.seed); f->have[idx]=1; }
        return f->v[idx];
    }
    return cg_noise3sum(f->f0,f->a0,i*f->L+g.seed,j*f->L+g.seed,k*f->L+g.seed);
}
// One field along one (wx, wz) stack. With a lattice, the four node columns around the stack are
// interpolated in x then z once per node level j (lazily), and each cell only interpolates in y,
// so a stack costs a few node evaluations plus one lerp a cell. The probes (tree roots, the
// spawn search, cave detection) build the same CgCol, so they get the same bits as a window.
#define CG_NODE_LEVELS (T_HEIGHT_MAX/2+2)
struct CgCol{ int i,k; float tx,tz; float v[CG_NODE_LEVELS]; unsigned char have[CG_NODE_LEVELS]; };
static void cg_col_init(CgField* f,CgCol* col,int wx,int wz){
    if(f->L<=1)return;
    const int L=f->L;
    col->i=cg_floordiv(wx,L); col->k=cg_floordiv(wz,L);
    col->tx=(float)(wx-col->i*L)/L; col->tz=(float)(wz-col->k*L)/L;
    memset(col->have,0,sizeof(col->have));
}
static float cg_col_level(CgField* f,const ClassicGen& g,CgCol* col,int j){
    if(!col->have[j]){
        float n00=cg_node(f,g,col->i,j,col->k),  n10=cg_node(f,g,col->i+1,j,col->k);
        float n01=cg_node(f,g,col->i,j,col->k+1),n11=cg_node(f,g,col->i+1,j,col->k+1);
        float a=n00+(n10-n00)*col->tx, b=n01+(n11-n01)*col->tx;
        col->v[j]=a+(b-a)*col->tz;
        col->have[j]=1;
    }
    return col->v[j];
}
static float cg_col_field(CgField* f,const ClassicGen& g,CgCol* col,int wx,int y,int wz){
    if(f->L<=1)return cg_noise3sum(f->f0,f->a0,wx+g.seed,y+g.seed,wz+g.seed);
    const int L=f->L,j=y/L;                      // y >= 0
    float ty=(float)(y-j*L)/L;
    float v0=cg_col_level(f,g,col,j),v1=cg_col_level(f,g,col,j+1);
    return v0+(v1-v0)*ty;
}

struct CgCtx{
    const ClassicGen* g; int height;
    int skinDepth,deepMargin,cave2011Top;   // round(6s), round(16s), round(9s)
    CgField skin,cave;
};
static void cg_ctx_init(CgCtx* c,const ClassicGen& g,int wx0,int wz0,int W,int D,int height){
    c->g=&g; c->height=height;
    c->skinDepth=(int)roundf(6.0f*g.scale4/4.0f);
    c->deepMargin=(int)roundf(16.0f*g.scale4/4.0f);
    c->cave2011Top=(int)roundf(9.0f*g.scale4/4.0f);
    cg_field_init(&c->skin,g,12.0f/g.scale4,0.5f,wx0,wz0,W,D,height);     // 1.x: FREQ3 3, AMPLITUDE3 0.5
    cg_field_init(&c->cave,g,16.0f/g.scale4,0.25f,wx0,wz0,W,D,height);    // 1.x: FREQ3 4, AMPLITUDE3 0.25
}

// One (wx, wz) stack of 1.x's body loop: its height and the bounds that depend on it, once.
struct CgStack{
    int wx,wz,h,F,bandLo,bandHi; float vein;
    CgCol skin,cave;
};
static void cg_stack_init(CgCtx* c,CgStack* s,int wx,int wz,int h){
    const ClassicGen& g=*c->g;
    s->wx=wx; s->wz=wz; s->h=h;
    s->F=h-c->skinDepth;                        // FORMATION_HEIGHT
    s->bandLo=1<<30; s->bandHi=0; s->vein=0.01f;   // no band: caves off
    if(g.caves==CLASSIC_CAVES_DEEP){
        s->bandLo=h%2+1; s->bandHi=s->F-c->deepMargin;
    }else if(g.caves==CLASSIC_CAVES_2011){
        s->bandLo=3+(int)(cg_rand(g,wx,wz,0,CS_CAVE_LO)%3);   // 3..5 (§1.4: soft, not a function of h)
        s->bandHi=h-c->cave2011Top;
        s->vein=0.005f;
    }
    cg_col_init(&c->skin,&s->skin,wx,wz);
    cg_col_init(&c->cave,&s->cave,wx,wz);
}
// One cell of terrain before decoration (1.x's per-y body).
static int cg_cell(CgCtx* c,CgStack* s,int y){
    if(y>=s->h)return TYPE_NONE;
    if(y==0)return TYPE_BEDROCK;
    if(y<s->F){
        if(!(y>s->bandLo&&y<s->bandHi))return TYPE_STONE;
        float n3=cg_col_field(&c->cave,*c->g,&s->cave,s->wx,y,s->wz);
        if(n3>0)return n3<=s->vein?TYPE_DARK_STONE:TYPE_STONE;
        return TYPE_NONE;
    }
    float n3=cg_col_field(&c->skin,*c->g,&s->skin,s->wx,y,s->wz);
    return n3<0.07f?TYPE_DIRT:TYPE_NONE;
}
static int cg_top(CgCtx* c,CgStack* s){
    int F=s->F; if(F<1)F=1;
    int above=TYPE_NONE;
    for(int y=s->h-1;y>=F;y--){
        int t=cg_cell(c,s,y);
        if(t==TYPE_DIRT&&above==TYPE_NONE)return y;
        above=t;
    }
    return -1;
}
int classic_surface_top(const ClassicGen& g,int wx,int wz,int height){
    CgCtx c; cg_ctx_init(&c,g,wx,wz,1,1,height);
    CgStack st; cg_stack_init(&c,&st,wx,wz,classic_height(g,wx,wz,height));
    return cg_top(&c,&st);
}
// 1.x's surface pass for one dirt-under-air cell (local coords make the flower patch grid).
static int cg_surface_kind(const ClassicGen& g,int wx,int wz,int y){
    int lx=wx&(CHUNK_SIZE-1),lz=wz&(CHUNK_SIZE-1);
    if(g.flowers&&cg_rand(g,wx,wz,y,CS_FLOWER)%2==0&&(lx+lx%10)%20<4&&(lz+lz%10)%20<4)return TYPE_GRASS3;
    return cg_rand(g,wx,wz,y,CS_GRASS)%2==0?TYPE_GRASS:TYPE_GRASS2;
}

struct CgWin{
    int wx0,wz0,W,D,H; block8* t; color8* c;
    bool in(int wx,int wz,int y) const { return wx>=wx0&&wx<wx0+W&&wz>=wz0&&wz<wz0+D&&y>=0&&y<H; }
    size_t at(int wx,int wz,int y) const { return ((size_t)(wx-wx0)*D+(wz-wz0))*H+y; }
    int get(int wx,int wz,int y) const { return in(wx,wz,y)?t[at(wx,wz,y)]:TYPE_NONE; }
    void set(int wx,int wz,int y,int type,int color){ if(in(wx,wz,y)){ t[at(wx,wz,y)]=type; c[at(wx,wz,y)]=color; } }
};

// The 1.x crown over a trunk at (wx, base, wz), drawn wherever it falls inside the window. Leaves
// never replace a trunk; the caller orders trees, so later trunks replace earlier leaves.
static void cg_draw_tree(const ClassicGen& g,CgWin* w,int wx,int wz,int base,int th){
    for(int i=0;i<3*th/4;i++)w->set(wx,wz,base+i,TYPE_TREE,0);
    int color=kClassicLeafColors[cg_rand(g,wx,wz,base,CS_LEAF_COLOR)%4];
    for(int i=wx-2;i<=wx+2;i++){
        for(int j=wz-2;j<=wz+2;j++){
            for(int k=base+2*th/3;k<th+base;k++){
                if(!w->in(i,j,k)||w->get(i,j,k)==TYPE_TREE)continue;
                if(i==wx-2||i==wx+2||j==wz-2||j==wz+2){
                    if((i==wx-2||i==wx+2)&&(j==wz-2||j==wz+2)&&(k==base+2*th/3||k==base+th-1)){
                    }else if(cg_rand(g,i,j,k,CS_LEAF)%2==0){
                        w->set(i,j,k,TYPE_LEAVES,color);
                    }
                }else{
                    w->set(i,j,k,TYPE_LEAVES,color);
                }
            }
        }
    }
}

void classic_generate_window(const ClassicGen& g,int wx0,int wz0,int W,int D,int height,
                             block8* types,color8* colors,bool decorate){
    memset(types,0,(size_t)W*D*height*sizeof(block8));
    memset(colors,0,(size_t)W*D*height*sizeof(color8));
    CgWin w={wx0,wz0,W,D,height,types,colors};
    CgCtx c; cg_ctx_init(&c,g,wx0,wz0,W,D,height);
    std::vector<int> hs((size_t)W*D);
    for(int x=0;x<W;x++){                       // heightmap + body (1.x's first loop)
        for(int z=0;z<D;z++){
            int wx=wx0+x,wz=wz0+z;
            int h=classic_height(g,wx,wz,height);
            hs[(size_t)x*D+z]=h;
            block8* col=&types[((size_t)x*D+z)*height];
            CgStack st; cg_stack_init(&c,&st,wx,wz,h);
            for(int y=0;y<h;y++)col[y]=cg_cell(&c,&st,y);
        }
    }
    if(!decorate)return;
    for(int x=0;x<W;x++){                       // surface: grass kinds and flower patches
        for(int z=0;z<D;z++){
            block8* col=&types[((size_t)x*D+z)*height];
            for(int y=1;y<height;y++){
                if(col[y]==TYPE_NONE&&col[y-1]==TYPE_DIRT)col[y-1]=cg_surface_kind(g,wx0+x,wz0+z,y-1);
            }
        }
    }
    const int spacing=kClassicTreeSpacing[g.trees];
    if(spacing&&g.profile==CLASSIC_PROFILE_LEGACY){
        // 1.x: roots only at local 2..13 of a column, scanned x, z, y, each seeing the trees
        // before it -- which leaves the tree-free lanes on the 16-block grid (§1.5, decision 10).
        if(W==CHUNK_SIZE&&D==CHUNK_SIZE&&(wx0&(CHUNK_SIZE-1))==0&&(wz0&(CHUNK_SIZE-1))==0){
            for(int x=2;x<CHUNK_SIZE-2;x++){
                for(int z=2;z<CHUNK_SIZE-2;z++){
                    for(int y=1;y<height-1;y++){
                        int t=w.get(wx0+x,wz0+z,y);
                        if(t!=TYPE_GRASS&&t!=TYPE_GRASS2)continue;
                        int wx=wx0+x,wz=wz0+z,base=y+1;          // placeTree(x,z,y+1)
                        if(cg_rand(g,wx,wz,base,CS_ROOT)%spacing!=0)continue;
                        int th=cg_rand(g,wx,wz,base,CS_TREE_H)%3+6;
                        if(base+th>=height)continue;
                        bool ok=true;
                        for(int i=wx-1;i<=wx+1&&ok;i++){
                            for(int j=wz-1;j<=wz+1;j++){
                                int below=w.get(i,j,base-1);
                                if(!(below==TYPE_GRASS||below==TYPE_GRASS2||below==TYPE_DIRT)||w.get(i,j,base)!=TYPE_NONE){ ok=false; break; }
                            }
                        }
                        if(ok)cg_draw_tree(g,&w,wx,wz,base,th);
                    }
                }
            }
        }
    }else if(spacing){
        // New worlds: a root anywhere, decided by hash and by the undecorated terrain only, so
        // every column that a crown reaches draws the same tree (decision 10; gate 3).
        const int M=3;                                   // roots within 2, their 3x3 within 3
        const int AW=W+2*M,AD=D+2*M;
        std::vector<int> hmemo((size_t)AW*AD,-1);
        for(int x=0;x<W;x++)for(int z=0;z<D;z++)hmemo[(size_t)(x+M)*AD+(z+M)]=hs[(size_t)x*D+z];
        #define CG_H(WX,WZ) (hmemo[(size_t)((WX)-wx0+M)*AD+((WZ)-wz0+M)]>=0?hmemo[(size_t)((WX)-wx0+M)*AD+((WZ)-wz0+M)]:\
            (hmemo[(size_t)((WX)-wx0+M)*AD+((WZ)-wz0+M)]=classic_height(g,(WX),(WZ),height)))
        const int R=g_classic_mutation==3?0:2;           // 3: --classic-selftest's "noborder" mutation
        for(int wx=wx0-R;wx<wx0+W+R;wx++){
            for(int wz=wz0-R;wz<wz0+D+R;wz++){
                if(cg_rand(g,wx,wz,0,CS_ROOT)%spacing!=0)continue;
                CgStack rs; cg_stack_init(&c,&rs,wx,wz,CG_H(wx,wz));
                int top=cg_top(&c,&rs);
                if(top<1||cg_surface_kind(g,wx,wz,top)==TYPE_GRASS3)continue;
                int base=top+1,th=cg_rand(g,wx,wz,0,CS_TREE_H)%3+6;
                if(base+th>=height)continue;
                bool ok=true;
                for(int i=wx-1;i<=wx+1&&ok;i++){
                    for(int j=wz-1;j<=wz+1;j++){
                        CgStack ns; cg_stack_init(&c,&ns,i,j,CG_H(i,j));
                        if(cg_cell(&c,&ns,top)!=TYPE_DIRT||cg_cell(&c,&ns,base)!=TYPE_NONE){ ok=false; break; }
                    }
                }
                if(ok)cg_draw_tree(g,&w,wx,wz,base,th);
            }
        }
        #undef CG_H
    }
    if(g.clouds){
        // 1.x generateCloud(): one column in five gets 4..7 slabs, all inside that column, at
        // y 61/62 at 1x (⌊64s⌋-2/-3 at scale s, decision 11), whatever the world height.
        int top=16*g.scale4; if(top>height)top=height;
        for(int cx=cg_floordiv(wx0,CHUNK_SIZE);cx<=cg_floordiv(wx0+W-1,CHUNK_SIZE);cx++){
            for(int cz=cg_floordiv(wz0,CHUNK_SIZE);cz<=cg_floordiv(wz0+D-1,CHUNK_SIZE);cz++){
                int bx=cx*CHUNK_SIZE,bz=cz*CHUNK_SIZE;
                if(cg_rand(g,bx,bz,0,CS_CLOUD)%5!=0)continue;
                int num=cg_rand(g,bx,bz,1,CS_CLOUD)%4+4;
                for(int rep=0;rep<num;rep++){
                    int r=8+rep*6;
                    int x=cg_rand(g,bx,bz,r,CS_CLOUD)%7;
                    int y=cg_rand(g,bx,bz,r+1,CS_CLOUD)%7;
                    int cw=cg_rand(g,bx,bz,r+2,CS_CLOUD)%(CHUNK_SIZE-x);
                    int ch=cg_rand(g,bx,bz,r+3,CS_CLOUD)%(CHUNK_SIZE-y);
                    if(cw<4)cw=4;
                    if(ch<4)ch=4;
                    int d=cg_rand(g,bx,bz,r+4,CS_CLOUD)%2+2;
                    for(int i=0;i<cw;i++)
                        for(int j=0;j<ch;j++)
                            w.set(bx+i+x,bz+j+y,top-d,TYPE_CLOUD,0);
                }
            }
        }
    }
}

void classic_cave_votes(int seed,int cx,int cz,const unsigned char* col,int bands,long* cells,long* air){
    ClassicGen g; classic_legacy_params(&g,seed); g.caves=CLASSIC_CAVES_2011;
    CgCtx c; cg_ctx_init(&c,g,cx*CHUNK_SIZE,cz*CHUNK_SIZE,CHUNK_SIZE,CHUNK_SIZE,T_HEIGHT_DEFAULT);
    for(int x=0;x<CHUNK_SIZE;x++){
        for(int z=0;z<CHUNK_SIZE;z++){
            int wx=cx*CHUNK_SIZE+x,wz=cz*CHUNK_SIZE+z;
            CgStack st; cg_stack_init(&c,&st,wx,wz,classic_height(g,wx,wz,T_HEIGHT_DEFAULT));
            // 6..h-10: inside the 2011 band whatever its soft lower edge (3..5) was in that column.
            for(int y=6;y<st.bandHi&&y<bands*CHUNK_SIZE;y++){
                if(cg_col_field(&c.cave,g,&st.cave,wx,y,wz)>0)continue;
                (*cells)++;
                if(col[(size_t)(y/CHUNK_SIZE)*8192+x*(CHUNK_SIZE*CHUNK_SIZE)+z*CHUNK_SIZE+y%CHUNK_SIZE]==TYPE_NONE)(*air)++;
            }
        }
    }
}
