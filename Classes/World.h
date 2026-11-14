//
//  World.h
//  prototype
//
//  Created by Ari Ronen on 10/10/10.
//  Copyright 2010 __MyCompanyName__. All rights reserved.
//
#ifndef Eden_World_h
#define Eden_World_h



#include <stdint.h>
#include <atomic>
#include <pthread.h>
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
    // QA.7 (A2-03): written by the world-load pthread (2 = done) and polled by the main thread, the MP
    // link and the background-save/convert guards, so it is atomic: the thread's store of 2 releases
    // everything it wrote (chunk tables, header, palette) to the main thread's load of it.
    std::atomic<int> doneLoading;
    pthread_t loadThread;          // QA.7: joined in World::loadWorld's doneLoading==2 branch
    bool loadThreadLive=false;
    void joinLoadThread();          // QA.14: also at quit, so no load thread outlives main()
    bool modsNoticePending=false;   // K.3-6: say which of the world's mods are off/missing, once, after entry
    void showModsNotice();          // QA.7 (A2-04): called once the load is done, not on its first frame
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
