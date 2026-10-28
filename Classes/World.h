//
//  World.h
//  prototype
//
//  Created by Ari Ronen on 10/10/10.
//  Copyright 2010 __MyCompanyName__. All rights reserved.
//
#ifndef Eden_World_h
#define Eden_World_h



#include <OpenGLES/ES1/gl.h>
#include <OpenGLES/ES1/glext.h>
#include "Texture2D.h"
#include "Terrain.h"
#include "Graphics.h"
#include "Camera.h"
#include "Resources.h"
#include "Player.h"
#include "Hud.h"
#include "Menu.h"
#include "FileManager.h"
#include "SpecialEffects.h"


class FileManager;
class Player;
class Terrain;
class Hud;
class Menu;
#define GAME_MODE_MENU 0
#define GAME_MODE_WAIT 2
#define GAME_MODE_PLAY 1
#define JUST_TERRAIN_GEN 0
class World{
public:
	Terrain* terrain;
	Camera* cam;
	Resources* res;
	Player* player;
	Hud* hud;
	Menu* menu;
	FileManager* fm;
	SpecialEffects* effects;
    int target_game_mode;
	int game_mode;
	//BOOL FLIPPED;
    int doneLoading;
    float realtime;
    bool bestGraphics;
    bool sanityCheck;
    
    World();
    ~World();
    static World* getWorld;
    bool update(float etime);
    void loadWorld(const std::string& name);
    void render();
    void renderFrame(bool draw);
    void exitToMenu();
    
    
};

#endif
