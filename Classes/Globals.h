//
//  Globals.h
//  prototype
//
//  Created by Ari Ronen on 10/13/10.
//  Copyright 2010 __MyCompanyName__. All rights reserved.
//
#ifndef Eden_Globals_h
#define Eden_Globals_h


#include "Constants.h"
#include <stdint.h>


//#define SCALE_WIDTH 2.0f
//#define SCALE_HEIGHT 2.0f

extern float SCALE_WIDTH;
extern float SCALE_HEIGHT;
// A voxel's type byte. Unsigned since K.1-1 (2026-10-31): 0..STOCK_BLOCK_MAX are the stock ids,
// 128..255 are reserved for the per-world block palette (WORKING/block-registry-plan-2026-10-31.md).
// Non-type byte arrays that used to borrow this typedef (worldgen elevation/biome maps, the
// old generator's colour scratch) are sbyte, so their signedness did not change with it.
typedef unsigned char block8;
typedef unsigned char color8;
typedef unsigned char ubyte;
typedef signed char sbyte;



extern float P_ZFAR;
//The EAGL view is stored in the nib file. When it's unarchived it's sent -initWithCoder:.
extern float SCREEN_WIDTH; 
extern float SCREEN_HEIGHT;
extern float P_ASPECT_RATIO; 
extern bool IS_IPAD;
extern bool IS_RETINA;
extern bool SUPPORTS_RETINA;
extern bool CREATURES_ON;
extern int CAMERA_VIEW;   // H.12: 0 first person, 1 third behind, 2 third front (THIRD_PERSON, Constants.h)
extern bool LOW_MEM_DEVICE;
extern bool DEFER_GAME_TEXTURES;   // Q.3: game textures load in loadGameAssets (world entry), not under the menu; no lighting side effect
extern bool LOW_GRAPHICS;



// Per-type tables have BLOCK_IDS (256) rows, one per type byte. Rows 128..255 belong to the world's
// block palette: BlockRegistry::resolve rebuilds them at every world load (K.1-3); until a definition
// fills one, it holds the "missing" block's rows (eden_fill_missing_block_rows, Globals.cpp).
extern int blockTypeFaces[BLOCK_IDS][6];
extern uint8_t blockColor[BLOCK_IDS][3];
// non-const since H.8: Terrain.mm toggles IS_FLAMMABLE on ice/vine at runtime
extern int blockinfo[BLOCK_IDS];
// The other per-type tables (defined in Hud.cpp, Terrain.cpp); non-const since K.1-1 so rows
// 128..255 can be filled at run time (K.1-3 rebuilds them from the world's palette).
#ifdef __cplusplus
extern "C" {
#endif
extern int hudBlocksMap[BLOCK_IDS];
extern int blockTntMap[BLOCK_IDS];
#ifdef __cplusplus
}
#endif
// The "missing" block's row in every per-type table, for one type byte 128..255 (others: no-op), and
// for all of them (static init; BlockRegistry::resolve per unresolved id). K.1-3.
void eden_set_missing_block_row(int t);
void eden_fill_missing_block_rows();



#define SYSTEM_VERSION_GREATER_THAN_OR_EQUAL_TO(v)  ([[[UIDevice currentDevice] systemVersion] compare:v options:NSNumericSearch] != NSOrderedAscending)

#endif