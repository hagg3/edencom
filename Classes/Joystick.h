//
//  Joystick.h
//  prototype
//
//  Created by Ari Ronen on 12/27/10.
//  Copyright 2010 __MyCompanyName__. All rights reserved.
//
#ifndef Eden_Joystick_h
#define Eden_Joystick_h




class Joystick{
    public:
    Joystick();
    // I.2: puts the pad its stock 20 points in from the SAFE rect's bottom-left corner (not the
    // screen's). Re-run by Hud::layoutForScreen; idempotent.
    void layoutForScreen();
    BOOL update(float etime);
    void render();
};


#endif