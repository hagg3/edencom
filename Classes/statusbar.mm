//
//  statusbar.m
//  prototype
//
//  Created by Ari Ronen on 11/9/10.
//  Copyright 2010 __MyCompanyName__. All rights reserved.
//

#import "statusbar.h"
#import "Globals.h"
#import "OpenGL_Internal.h"
#import "GLWidgets.h"

static const int thresh=1000; //beyond this don't erase text ever.


statusbar::statusbar(CGRect rect, float font_size_in){
    
	pos=rect;
	text=NULL;
    message=NULL;
	textlife=0;
	font_size=font_size_in;
    toast=NULL;
	
}
statusbar::statusbar(CGRect rect){
    pos=rect;
    text=NULL;
    message=NULL;
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

void statusbar::setStatus(NSString* status,float time){
	this->setStatus(status,time,UITextAlignmentCenter);
}
extern "C" float eden_ui_raster_density(void);   // web/src/seam/DisplayProfile_web.mm
void statusbar::setStatus(NSString* status,float time,UITextAlignment align){
   if(message&&[status isEqualToString:message]){
        textlife=time;
        if(toast)toast->show([status UTF8String],time);   // re-shows it if it had expired
        return;
                     
    }
    
    clearText();   // not clear(): new text on a showing toast must not restart its fade-in
    if(CHECK_GL_ERROR()){}
	message=status;
    [message retain];
    if(toast){
        toast->show([status UTF8String],time);
    }
    else if(IS_IPAD){
		text=new Texture2D(status,
									CGSizeMake(pos.size.width*SCALE_WIDTH,
														  pos.size.height*SCALE_HEIGHT) ,
									align,
										 [UIFont systemFontOfSize:font_size*2],
                                         eden_ui_raster_density());   // N.4.8: 1:1 on device pixels
	}
	else{
	text=new Texture2D(status,
								CGSizeMake(pos.size.width,
													  pos.size.height) ,
								 align,
									  [UIFont systemFontOfSize:font_size]);
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
    if(message){
        [message release];
        message=NULL;
    }
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
		glColor4f(0.0, 0.0, 0.0, 1.0);
        
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
		glColor4f(1.0, 1.0, 1.0, 1.0);
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

