//
//  Input.m
//  prototype
//
//  Created by Ari Ronen on 10/13/10.
//  Copyright 2010 __MyCompanyName__. All rights reserved.
//

#import "Input.h"
#import "Globals.h"
#import "World.h"
#import "Hud.h"
static Input* singleton=NULL;

extern float SCREEN_WIDTH; 
extern float SCREEN_HEIGHT;
extern float P_ASPECT_RATIO;
extern BOOL IS_WIDESCREEN;

Input* Input::getInput(){
    if(!singleton){
        singleton=new Input();
    }
    return singleton;
}

Input::Input(){
	this->clearAll();
	this->screenMetricsChanged();
}

// Was inline in the constructor, and picked scr_width/scr_height out of the same three hard-coded
// device profiles (1024x768 / 568x320 / 480x320) EAGLView had just written into SCREEN_WIDTH/
// SCREEN_HEIGHT — i.e. it re-derived a value it could simply have read. Reading it instead is what
// lets the point space stop being one of three constants (web port audit D1/D4: it is derived from
// the real window aspect and a UI-scale setting, and can change while the game is running).
// Equivalent on every profile the original shipped: EAGLView -initWithCoder: sets SCREEN_WIDTH/
// SCREEN_HEIGHT before anything can construct an Input, and the non-retina iPad branch is the one
// the original commented out (EAGLView.mm:114-119), so it never set 1024x768 there either.
void Input::screenMetricsChanged(){
    scr_width=(int)SCREEN_WIDTH;
    scr_height=(int)SCREEN_HEIGHT;
    if(scr_width<=0) scr_width=IPHONE5_WIDTH;
    if(scr_height<=0) scr_height=IPHONE_HEIGHT;
}
void Input::clearAll(){
	for(int i=0;i<MAX_TOUCHES;i++){
		touches[i].mx=touches[i].my=0;	
		touches[i].pmx=touches[i].pmy=0;	
		touches[i].inuse=0;
		touches[i].down=M_NONE;
		touches[i].moved=0;
		touches[i].touch_id=NULL;
		touches[i].placeBlock=FALSE;	
	}
}

/*- (void)clearMove:(int)i{
	pmx=mx;
	pmy=my;
}*/

void Input::keyTyped(NSString* key){
	char ch=[key characterAtIndex:0];
	if(ch=='h'){
		World::getWorld->hud->hideui=!World::getWorld->hud->hideui;	
	}
	
}
itouch* Input::getTouches(){
	return touches;
	
}
void Input::touchesBegan(NSSet* mtouches, UIEvent* event){
	for(UITouch* touch in mtouches){
		
		int idx=-1;
		for(int i=0;i<MAX_TOUCHES;i++){
			if(touches[i].down==M_NONE){
				idx=i;
				break;
			}
		}
		if(idx==-1){ //Too many touches, ignore
			continue;			
		}
		
		touches[idx].touch_id=touch;
		CGPoint point=[touch locationInView:touch.view];
        point.y=scr_height-point.y;
        //printg("touch (%f,%f)\n",point.x,point.y);
		/*if(World::getWorld->FLIPPED){
			//point.x+=11;
			point.x=scr_width-point.x;
			point.y=scr_height-point.y;
		}*/		
		touches[idx].down=M_DOWN;
     
		touches[idx].inuse=0;
		touches[idx].etime=0;
		touches[idx].moved=YES;
        if(IS_IPAD&&!IS_RETINA){
            touches[idx].mx=(float)point.x/SCALE_WIDTH;
            touches[idx].my=(float)point.y/SCALE_HEIGHT;
        }else{
            touches[idx].mx=point.x;
            touches[idx].my=point.y;
        }
		
		touches[idx].pmx=touches[idx].mx;
		touches[idx].pmy=touches[idx].my;
		touches[idx].fx=touches[idx].mx;
		touches[idx].fy=touches[idx].my;
		touches[idx].placeBlock=TRUE;	
		touches[idx].previewtype=TYPE_NONE;
		touches[idx].movecam=TRUE;
	}
}

void Input::touchesMoved(NSSet* mtouches, UIEvent* event){
	for(UITouch* touch in mtouches){
	//	NSLog(@"touchm %@",touch);
		int idx=-1;
		for(int i=0;i<MAX_TOUCHES;i++){
			if(touches[i].touch_id==touch){
				idx=i;
				break;
			}
		}
		if(idx==-1){
			continue;
		}
		CGPoint point=[touch locationInView:touch.view];
        point.y=scr_height-point.y;
		/*if(World::getWorld->FLIPPED){
			//point.x+=11;
			point.x=scr_width-point.x;
			point.y=scr_height-point.y;
		}*/
		touches[idx].moved=TRUE;
		touches[idx].pmx=touches[idx].mx;
		touches[idx].pmy=touches[idx].my;
		if(IS_IPAD&&!IS_RETINA){
            touches[idx].mx=(float)point.x/SCALE_WIDTH;
            touches[idx].my=(float)point.y/SCALE_HEIGHT;
        }else{
            touches[idx].mx=point.x;
            touches[idx].my=point.y;
        }
	}		
}
void Input::touchesEnded(NSSet* mtouches, UIEvent* event){
	for(UITouch* touch in mtouches){
		//
		int idx=-1;
		for(int i=0;i<MAX_TOUCHES;i++){
			if(touches[i].touch_id==touch){
				idx=i;
				break;
			}
		}
		if(idx==-1){
			continue;
		}
        if(!touch)continue;
		CGPoint point=[touch locationInView:touch.view];
        point.y=scr_height-point.y;
		/*if(World::getWorld->FLIPPED){
		//	point.x+=11;
			point.x=scr_width-point.x;
			point.y=scr_height-point.y;
		}	*/
		touches[idx].moved=TRUE;
		touches[idx].pmx=touches[idx].mx;
		touches[idx].pmy=touches[idx].my;
		if(IS_IPAD&&!IS_RETINA){
            touches[idx].mx=(float)point.x/SCALE_WIDTH;
            touches[idx].my=(float)point.y/SCALE_HEIGHT;
        }else{
            touches[idx].mx=point.x;
            touches[idx].my=point.y;
        }
		touches[idx].touch_id=0;
		if(touches[idx].inuse)
			touches[idx].down=M_RELEASE;
		else
			touches[idx].down=M_NONE;

	}
}
void Input::touchesCancelled(NSSet* mtouches, UIEvent* event){
    this->clearAll();
	
}


