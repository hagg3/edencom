//
//  Globals.m
//  prototype
//
//  Created by Ari Ronen on 10/13/10.
//  Copyright 2010 __MyCompanyName__. All rights reserved.
//
#include <cstdio>
#include <stdint.h>
#include "Constants.h"

float SCREEN_WIDTH;
float SCREEN_HEIGHT;
float SCALE_WIDTH;
float SCALE_HEIGHT;
float P_ASPECT_RATIO;
float P_ZFAR;
bool IS_WIDESCREEN;
bool IS_IPAD;  //This flag gets set for ipad AND 'retina' devices, which is basically all supported devices these days.
bool IS_RETINA;
bool SUPPORTS_RETINA=false;
bool CREATURES_ON;
int CAMERA_VIEW=0;   // H.12
bool LOW_MEM_DEVICE;
bool DEFER_GAME_TEXTURES=true;   // Q.3
bool LOW_GRAPHICS;

int g_offcx,g_offcz;

// Runtime world height -- see Constants.h. Defaults are the pre-2026-08-06 compile-time
// constants, so anything that never calls eden_set_world_height() behaves exactly as before
// (that includes the offline TerrainGen2 bake, which is 64z by construction).
int g_world_height=T_HEIGHT_DEFAULT;
int g_chunks_per_column=T_HEIGHT_DEFAULT/CHUNK_SIZE;
// B.5b-2: the resident window, runtime too (Constants.h). Literal defaults, not T_SIZE, so no
// initialiser below depends on another global's dynamic initialisation order.
int g_window_radius=T_RADIUS_DEFAULT;
int g_chunks_per_side=T_RADIUS_DEFAULT*2;
int g_t_size=T_RADIUS_DEFAULT*2*CHUNK_SIZE;
int g_xz_stride=T_RADIUS_DEFAULT*2*CHUNK_SIZE*T_HEIGHT_DEFAULT;
int g_t_blocks=T_RADIUS_DEFAULT*2*CHUNK_SIZE*T_RADIUS_DEFAULT*2*CHUNK_SIZE*T_HEIGHT_DEFAULT;
int g_column_bytes=CHUNK_SIZE3*(T_HEIGHT_DEFAULT/CHUNK_SIZE)*2;
int g_max_creatures_saved=200;
void eden_set_world_height(int height){
    if(height!=64&&height!=256){
        printf("eden_set_world_height: refusing unsupported height %d, staying at %d\n",height,g_world_height);
        return;
    }
    g_world_height=height;
    g_chunks_per_column=height/CHUNK_SIZE;
    g_xz_stride=T_SIZE*height;
    g_t_blocks=T_SIZE*T_SIZE*height;
    g_column_bytes=CHUNK_SIZE3*g_chunks_per_column*2;
    printf("world height set to %d (%d bands, %d B per column record)\n",
           g_world_height,g_chunks_per_column,g_column_bytes);
}
// B.5b-2. Same contract as the height: once per world entry, before Terrain::allocateMemory(), and
// never while a window is allocated (every per-world array, the light bricks and the toroidal
// offsets are sized or aligned from it). g_offcx/g_offcz only keep `(x+g_offcx)%T_SIZE` positive,
// so they must stay a multiple of T_SIZE -- the Terrain constructor set them once, for 288.
void eden_set_window_radius(int radius){
    if(radius<T_RADIUS_MIN)radius=T_RADIUS_MIN;
    if(radius>T_RADIUS_MAX)radius=T_RADIUS_MAX;
    g_window_radius=radius;
    g_chunks_per_side=radius*2;
    g_t_size=g_chunks_per_side*CHUNK_SIZE;
    g_xz_stride=g_t_size*g_world_height;
    g_t_blocks=g_t_size*g_t_size*g_world_height;
    g_offcx=g_offcz=g_t_size*100;
    printf("window radius set to %d (%d x %d blocks, %d columns)\n",
           radius,g_t_size,g_t_size,g_chunks_per_side*g_chunks_per_side);
}
int eden_meshed_radius_blocks(void){
    return (g_window_radius-1)*CHUNK_SIZE-CHUNK_SIZE/2;
}
// The creature block is sized from the FILE, not from its version: the sibling world editor
// writes v5 saves with no creature block at all, and the one measured New Dawn specimen has
// 400 slots where the version alone would only tell us ">=5". 0 is a legal answer and means
// "this file has nowhere to put creatures" -- LoadCreatures/saveCreatures then do nothing,
// which is exactly the pre-version-3 behaviour they already implement.
void eden_set_creature_slots(int slots){
    if(slots<0)slots=0;
    if(slots>MAX_CREATURES_SAVED_MAX)slots=MAX_CREATURES_SAVED_MAX;
    g_max_creatures_saved=slots;
}
// B5: see Constants.h. 0 would mean "always save in place" and is legal (the tests use it);
// there is deliberately no upper clamp, so "never save in place" stays expressible too.
unsigned long long g_save_inplace_threshold=EDEN_SAVE_INPLACE_THRESHOLD_DEFAULT;
void eden_set_save_inplace_threshold(unsigned long long bytes){
    g_save_inplace_threshold=bytes;
    printf("save: whole-file-copy threshold set to %llu B\n",g_save_inplace_threshold);
}
// C3: see Constants.h. TRUE is the atomic setting; FALSE reproduces B5's tear-one-column trade.
int g_save_journal_columns=1;

// K.2-3: rows 0..127 of blockinfo, blockColor and blockTypeFaces come from the stock pack
// (media/packs/eden/pack.json, Classes/ContentPack: loadStock at boot, before any mode runs); the C
// initialisers are gone (`pristine-engine` and git history keep them). Zero until then; rows 128..255 are
// the missing block's (below) and the world's palette (BlockRegistry::resolve).
extern "C" int blockinfo[BLOCK_IDS]={};

extern "C" const uint8_t creatureColor[NUM_CREATURES+1][3]={
    [M_MOOF]={70,70,255},
    [M_GREEN]={115,206,74},
    [M_NERGLE]={255,70,255},
    [M_BATTY]={255,70,255},
    [M_STUMPY]={255,165,0},
    [M_STALKER]={235,20,0},
    [M_CHARGER]={235,20,0},
   
    
};
extern "C" uint8_t blockColor[BLOCK_IDS][3]={};

extern "C" int blockTypeFaces[BLOCK_IDS][6]={};

// K.1-1: the per-type tables have BLOCK_IDS (256) rows. Rows STOCK_BLOCK_MAX+1..255 are the world's
// palette ids; K.1-3 (BlockRegistry::resolve, at every world load) fills each from its definition, or
// with the "missing" block's rows below when it has none (D4/D10/D12): solid and breakable like stone
// (IS_HARD, no TNT mapping, not flammable), every face TEX_MISSING, magenta for particles, no picker
// mapping, and IS_MISSING so the mesher never paints it. Before K.1-1 the type byte was signed and a
// byte >= 128 indexed these tables at a negative offset; K.1-1 gave the rows stone's look.
extern "C" int hudBlocksMap[BLOCK_IDS];
extern "C" int blockTntMap[BLOCK_IDS];
extern uint8_t blockSoundSet[BLOCK_IDS];   // K.2-2 (BlockTraits.h)
extern uint8_t blockPaintArt[BLOCK_IDS];
void eden_set_missing_block_row(int t){
    if(t<=STOCK_BLOCK_MAX||t>=BLOCK_IDS)return;
    blockinfo[t]=IS_HARD|IS_MISSING;
    blockColor[t][0]=255; blockColor[t][1]=0; blockColor[t][2]=255;
    for(int i=0;i<6;i++)blockTypeFaces[t][i]=TEX_MISSING;
    hudBlocksMap[t]=-1;
    blockTntMap[t]=0;
    blockSoundSet[t]=0;   // K.2-2: eden:generic, no paint art
    blockPaintArt[t]=0;
}
void eden_fill_missing_block_rows(){
    for(int t=STOCK_BLOCK_MAX+1;t<BLOCK_IDS;t++)eden_set_missing_block_row(t);
}
// Dynamic initialisation: every table above (and in Hud.cpp/Terrain.cpp) is constant-initialised,
// which happens before any dynamic initialiser in any translation unit.
[[maybe_unused]] static const bool g_missing_block_rows_filled=(eden_fill_missing_block_rows(),true);
