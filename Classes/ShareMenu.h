//
//  ShareMenu.h
//  prototype
//
//  Created by Ari Ronen on 11/16/10.
//  Copyright 2010 __MyCompanyName__. All rights reserved.
//

#ifndef Eden_ShareMenu_h
#define Eden_ShareMenu_h

#include "World.h"
#include "Menu.h"
#include "Util.h"

class ShareMenu{
public:
    ShareMenu();
	WorldNode* node;
	CGRect input_background;
	Button rect_cancel;
	Button rect_submit;
  
	CGRect share_explain_rect;
	statusbar* share_explain_lbl;
	statusbar* label_bar;
	statusbar* name_bar;
	std::string displays;   // G.2b-2: both were NSMutableString* (the class is a link stub)
	std::string name;
 
	//BOOL starto;
    
    void trimDisplay();
    void beginShare(WorldNode* world);
    void endShare(bool cancel);
    void activate();
    void deactivate();
    void update(float etime);
    void render();
    void keyTyped(char c);
};



#endif