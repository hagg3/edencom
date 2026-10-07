//
//  statusbar.h
//  prototype
//
//  Created by Ari Ronen on 11/9/10.
//  Copyright 2010 __MyCompanyName__. All rights reserved.
//
#ifndef Eden_statusbar_h
#define Eden_statusbar_h



#import "Graphics.h"

class statusbar {
public:
    statusbar(CGRect rect);
    statusbar(CGRect rect,float font_size);
    void setStatus(NSString* status, float time);
    void setStatus(NSString* status, float time,UITextAlignment align);
    void clear();
    void update(float etime);
    void render();
    void renderPlain();
    // Stage 5.6: the live message (NULL once its time ran out), for a screen that draws the
    // status line itself in the GL widget kit instead of through render().
    NSString* current() const { return (text!=NULL&&textlife>0)?message:NULL; }
    CGRect pos;
private:
	
	Texture2D* text;
    NSString* message;
	float textlife;
	float font_size;
};

#endif