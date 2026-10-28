//
//  statusbar.m
//  prototype
//
//  Created by Ari Ronen on 11/9/10.
//  Copyright 2010 __MyCompanyName__. All rights reserved.
//

#include "statusbar.h"
#include "Globals.h"
#include "OpenGL_Internal.h"
#include "GLWidgets.h"

static const int thresh=1000; //beyond this don't erase text ever.


statusbar::statusbar(CGRect rect, float font_size_in){
    
	pos=rect;
	text=NULL;
    hasMessage=false;
	textlife=0;
	font_size=font_size_in;
    toast=NULL;
	
}
statusbar::statusbar(CGRect rect){
    pos=rect;
    text=NULL;
    hasMessage=false;
    textlife=0;
    font_size=20;
    toast=NULL;
}
statusbar::~statusbar(){
    clear();
    delete toast;
}
void statusbar::useToast(){
    if(!toast)toast=new GLW::Toast();
}

void statusbar::setStatus(const std::string& status,float time){
	this->setStatus(status,time,UITextAlignmentCenter);
}
extern "C" float eden_ui_raster_density(void);   // port/src/seam/DisplayProfile.cpp
void statusbar::setStatus(const std::string& status,float time,UITextAlignment align){
   if(hasMessage&&status==message){
        textlife=time;
        if(toast)toast->show(status.c_str(),time);   // re-shows it if it had expired
        return;
                     
    }
    
    clearText();   // not clear(): new text on a showing toast must not restart its fade-in
    if(CHECK_GL_ERROR()){}
	message=status;   // G.2b-2: a copy (was a retained NSString*)
    hasMessage=true;
    const std::string& statusStr=message;
    if(toast){
        toast->show(message.c_str(),time);
    }
    else if(IS_IPAD){
		text=new Texture2D(statusStr,
									CGSizeMake(pos.size.width*SCALE_WIDTH,
														  pos.size.height*SCALE_HEIGHT) ,
									align,
										 (float)(font_size*2),   // G.2b-1: a point size (was [UIFont systemFontOfSize:])
                                         eden_ui_raster_density());   // N.4.8: 1:1 on device pixels
	}
	else{
	text=new Texture2D(statusStr,
								CGSizeMake(pos.size.width,
													  pos.size.height) ,
								 align,
									  (float)font_size);
	}
	textlife=time;
   
	//printg("message set:%s time:%f\n",[message cString],textlife);
}
void statusbar::clear(){
    clearText();
    if(toast)toast->clear();
}
void statusbar::clearText(){
	if(text!=NULL){
        delete text;
		
		text=NULL;
        
	}	
    message.clear();
    hasMessage=false;
}
void statusbar::update(float etime){
	if(textlife<thresh)
	textlife-=etime;
    if(toast)toast->update(etime);
    
   // printg("message update set:%s time:%f\n",[message cString],textlife);
}
void statusbar::render(){
    //glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    if(toast){
        toast->render();
        return;
    }
	if(text!=NULL&&textlife>0){
		Pen::color(0.0, 0.0, 0.0, 1.0);
        
        CGPoint p=CGPointMake(pos.origin.x,pos.origin.y);
        if(IS_IPAD){
            p.x*=SCALE_WIDTH;
            p.y*=SCALE_HEIGHT;
            p.y+=pos.size.height;
        }else
        p.y+=pos.size.height/2;
		text->drawAtPoint(p);
        if(IS_IPAD){
            p.x-=1;
            p.y+=1;
        }
		p.x-=1;
		p.y+=1;
		Pen::color(1.0, 1.0, 1.0, 1.0);
		text->drawAtPoint(p);
        //printg("text: %s\n",[message cString]);
	}
   // printg("text null or textlif<=0\n");
	
}
void statusbar::renderPlain(){
    if(toast){
        toast->render();
        return;
    }
    if(text!=NULL&&textlife>0){
		
        CGPoint p=CGPointMake(pos.origin.x,pos.origin.y);
        if(IS_IPAD){
            p.x*=SCALE_WIDTH;
            p.y*=SCALE_HEIGHT;
            p.y+=pos.size.height*SCALE_HEIGHT/2;
        }else
            p.y+=pos.size.height/2;
		
		p.x-=1;
		p.y+=1;
		
		text->drawAtPoint(p);
       
	}
    }

