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

namespace GLW { class Toast; }

class statusbar {
public:
    statusbar(CGRect rect);
    statusbar(CGRect rect,float font_size);
    ~statusbar();
    // N.5.11: draw as the GL kit's toast (GLWidgets.h) instead of the stock line — the
    // stock line was hard to read and printed twice at the screen's opposite ends. Hud's bar
    // opts in; pos and the alignment argument are then ignored. Modified from stock.
    void useToast();
    void setStatus(NSString* status, float time);
    void setStatus(NSString* status, float time,UITextAlignment align);
    void clear();
    void update(float etime);
    void render();
    void renderPlain();
    // Stage 5.6: the live message (NULL once its time ran out), for a screen that draws the
    // status line itself in the GL widget kit instead of through render().
    NSString* current() const { return (message!=NULL&&textlife>0)?message:NULL; }
    CGRect pos;
private:
    void clearText();
	
	Texture2D* text;
    NSString* message;
	float textlife;
	float font_size;
    GLW::Toast* toast;
};

#endif