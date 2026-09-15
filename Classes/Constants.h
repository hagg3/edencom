//
//  Constants.h
//  Eden
//
//  Created by Ari Ronen on 11/3/11.
//  Copyright (c) 2011 __MyCompanyName__. All rights reserved.
//

#ifndef Eden_Constants_h
#define Eden_Constants_h

#define P_FOVY 90.0f

#define P_ZNEAR .012f
#define IPAD_WIDTH 1024
#define IPAD_HEIGHT 768
#define IPHONE_WIDTH 480
#define IPHONE5_WIDTH 568
#define IPHONE_HEIGHT 320
#define GRAVITY 20.0f
#define THIRD_PERSON 0

#define EXPLOSION_RADIUS 5

#define NUM_BLOCKS 111

#define NUM_CREATURES 7
#define M_MOOF 0
#define M_BATTY 1
#define M_GREEN 2
#define M_NERGLE 3
#define M_STUMPY 4
#define M_CHARGER 5
#define M_STALKER 6
#define CHUNK_SIZE 16
#define CHUNK_SIZE2 (CHUNK_SIZE*CHUNK_SIZE)
#define CHUNK_SIZE3 (CHUNK_SIZE*CHUNK_SIZE*CHUNK_SIZE)
#define NOISE_CONSTANT ((128/CHUNK_SIZE)*CHUNK_SIZE)
#define T_SIZE ((int)( ((300/CHUNK_SIZE)*CHUNK_SIZE)))
#define T_RADIUS (T_SIZE/CHUNK_SIZE/2)
#define T_SEALEVEL 2
#define CHUNKS_PER_SIDE (T_SIZE/CHUNK_SIZE)

// ---- world height: RUNTIME, per world (256z "New Dawn" support, 2026-08-06) ----
// T_HEIGHT/T_BLOCKS/CHUNKS_PER_COLUMN/MAX_CREATURES_SAVED were compile-time constants
// (64 / 288*288*64 / 4 / 200) until this build learned to read the 256-block-tall .eden
// variant, which stores 16 chunk-bands per column instead of 4. They now expand to
// globals set once per world entry by eden_set_world_height(), BEFORE
// Terrain::allocateMemory() sizes the per-world arrays. Every use site was already
// symbolic, so this is a mechanical substitution -- the exceptions are fixed-size
// declarations, which must use the *_MAX forms below (see docs/world-and-terrain.md
// "Runtime world height" and WORKING/256z-format-backport-plan-2026-08-05.md).
// A 64z world must keep costing exactly what it cost before: the defaults here ARE the
// old constants, and nothing sets 256 unless a loaded file's header says version>=5.
#define T_HEIGHT_DEFAULT 64
#define T_HEIGHT_MAX 256
#define CHUNKS_PER_COLUMN_MAX (T_HEIGHT_MAX/CHUNK_SIZE)
#define MAX_CREATURES_SAVED_MAX 400

#ifdef __cplusplus
extern "C" {
#endif
extern int g_world_height;        // 64 or 256
extern int g_chunks_per_column;   // g_world_height/CHUNK_SIZE  (4 or 16)
extern int g_xz_stride;           // T_SIZE*g_world_height -- blockarray/lightarray x stride
extern int g_t_blocks;            // T_SIZE*T_SIZE*g_world_height
extern int g_column_bytes;        // on-disk column record: 32768 or 131072
extern int g_max_creatures_saved; // creature slots in the save file, DERIVED from the file
void eden_set_world_height(int height);
void eden_set_creature_slots(int slots);

// ---- save strategy: the size at which the whole-file copy stops (256z Stage 3, B5) ----
// Every save used to run on a scratch whole-file COPY of the world (copyItemAtPath: -> .savetmp),
// committed with one rename. Measured against a real 279 MB 256z specimen with NOTHING edited
// (web/tools/headless-save-io-probe.js): 558 MB read + 558 MB written per save -- 2x the file
// each way -- against ~155 KB of actually-changed bytes, plus a transient allocation of the
// ENTIRE file (the read buffer), which on a 32-bit wasm heap cannot succeed at all for the
// multi-GB worlds the 256z format allows. A file at or above this many bytes is saved IN PLACE
// with a small rollback journal instead (FileManager::saveWorld); below it, nothing changes.
// A runtime global rather than a bare #define at the use site so a test can force either path
// on a small world -- see web/tools/headless-save-inplace-test.js.
#define EDEN_SAVE_INPLACE_THRESHOLD_DEFAULT (16ull*1024*1024)
extern unsigned long long g_save_inplace_threshold;
void eden_set_save_inplace_threshold(unsigned long long bytes);

// ---- save strategy: does the rollback journal cover the dirty columns too? (C3) ----
// B5 shipped with a known hole: above the threshold the journal covered only the region an
// APPEND can destroy (the creature block, the directory, everything past them), so a crash
// between journal and commit could leave one dirty column half-old/half-new. C3 (2026-09-02)
// closes it -- saveWorld now also journals the ORIGINAL bytes of every column it is about to
// overwrite at its existing offset, which costs one extra read + write of exactly the columns
// the save was writing anyway. Default TRUE; a probe sets it FALSE to A/B that cost, and it is
// the one-flag rollback if a world ever turns out to dirty enough columns for it to hurt.
extern int g_save_journal_columns;
void eden_set_save_journal_columns(int on);
#ifdef __cplusplus
}
#endif

#define T_HEIGHT (g_world_height)
#define T_BLOCKS (g_t_blocks)
#define CHUNKS_PER_COLUMN (g_chunks_per_column)
#define MAX_CREATURES_SAVED (g_max_creatures_saved)
#define BLOCK_SIZE 1.0f

#define LIGHT_RADIUS 5.0f

enum BLOCK_TYPES{
	TYPE_NONE=0,
	TYPE_BEDROCK=1,
	TYPE_STONE=2,
	TYPE_DIRT=3,
	TYPE_SAND=4,
	TYPE_LEAVES=5,
	TYPE_TREE=6,
	TYPE_WOOD=7,
	TYPE_GRASS=8,
	TYPE_TNT=9,	
	TYPE_DARK_STONE=10,
	TYPE_GRASS2=11,
	TYPE_GRASS3=12,
	TYPE_BRICK=13,
	TYPE_COBBLESTONE=14,
	TYPE_ICE=15,
	TYPE_CRYSTAL=16,	
	TYPE_TRAMPOLINE=17,
    TYPE_LADDER=18,
    TYPE_CLOUD=19,  
    TYPE_WATER=20,
    TYPE_WEAVE=21,	
    TYPE_VINE=22,    
    TYPE_LAVA=23,
    TYPE_STONE_RAMP1=24,
    TYPE_STONE_RAMP2=25,
    TYPE_STONE_RAMP3=26,
    TYPE_STONE_RAMP4=27,
    TYPE_WOOD_RAMP1=28,
    TYPE_WOOD_RAMP2=29,
    TYPE_WOOD_RAMP3=30,
    TYPE_WOOD_RAMP4=31,
    TYPE_SHINGLE_RAMP1=32,
    TYPE_SHINGLE_RAMP2=33,
    TYPE_SHINGLE_RAMP3=34,
    TYPE_SHINGLE_RAMP4=35,
    TYPE_ICE_RAMP1=36,
    TYPE_ICE_RAMP2=37,
    TYPE_ICE_RAMP3=38,
    TYPE_ICE_RAMP4=39,   
    TYPE_STONE_SIDE1=40,
    TYPE_STONE_SIDE2=41,
    TYPE_STONE_SIDE3=42,
    TYPE_STONE_SIDE4=43,
    TYPE_WOOD_SIDE1=44,
    TYPE_WOOD_SIDE2=45,
    TYPE_WOOD_SIDE3=46,
    TYPE_WOOD_SIDE4=47,
    TYPE_SHINGLE_SIDE1=48,
    TYPE_SHINGLE_SIDE2=49,
    TYPE_SHINGLE_SIDE3=50,
    TYPE_SHINGLE_SIDE4=51, 
    TYPE_ICE_SIDE1=52,
    TYPE_ICE_SIDE2=53,
    TYPE_ICE_SIDE3=54,
    TYPE_ICE_SIDE4=55,
    TYPE_SHINGLE=56,
    TYPE_GRADIENT=57,
    TYPE_GLASS=58,  
    TYPE_WATER3=59,   
    TYPE_WATER2=60,    
    TYPE_WATER1=61,
    TYPE_LAVA3=62,
    TYPE_LAVA2=63,
    TYPE_LAVA1=64,   
    TYPE_FIREWORK=65,  
    TYPE_DOOR1=66,
    TYPE_DOOR2=67,
    TYPE_DOOR3=68,
    TYPE_DOOR4=69,
    TYPE_DOOR_TOP=70,
    TYPE_GOLDEN_CUBE=71,
    TYPE_LIGHTBOX=72,
    TYPE_FLOWER=73,
    TYPE_STEEL=74,
    TYPE_PORTAL1=75,
    TYPE_PORTAL2=76,
    TYPE_PORTAL3=77,
    TYPE_PORTAL4=78,
    TYPE_PORTAL_TOP=79,
    
   
   // TYPE_EMPTY_SPACE=80,
   TYPE_CUSTOM=80,
     TYPE_BLOCK_TNT=81,
    TYPE_BTGRASS=82,
    TYPE_BTDARKSTONE=83,
    TYPE_BTSTONE=84,
    TYPE_BTDIRT=85,
    TYPE_BTSAND=86,
    TYPE_BTTNT=87,
    TYPE_BTWOOD=88,
    TYPE_BTSHINGLE=89,
    TYPE_BTGLASS=90,
    TYPE_BTGRADIENT=91,
    TYPE_BTTREE=92,
    TYPE_BTLEAVES=93,
    TYPE_BTBRICK=94,
    TYPE_BTCOBBLESTONE=95,
    TYPE_BTVINES=96,
    TYPE_BTLADDER=97,
    TYPE_BTICE=98,
    TYPE_BTCRYSTAL=99,
    TYPE_BTTRAMPOLINE=100,
    TYPE_BTCLOUD=101,
    TYPE_BTSTONESIDE=102,
    TYPE_BTWOODSIDE=103,
    TYPE_BTICESIDE=104,
    TYPE_BTSHINGLESIDE=105,
    TYPE_BTFENCE=106,
    TYPE_BTWATER=107,
    TYPE_BTLAVA=108,
    TYPE_BTFIREWORK=109,
    TYPE_BTLIGHTBOX=110,
    TYPE_BTSTEEL=111,
    
};
#define IS_FLAMMABLE       0b00000000000000000001
#define IS_NOTSOLID        0b00000000000000000010
#define IS_RAMP            0b00000000000000000100
#define IS_SIDE            0b00000000000000001000
#define IS_RAMPORSIDE      0b00000000000000010000
#define IS_ATLAS2          0b00000000000000100000
#define IS_LIQUID          0b00000000000001000000
#define IS_WATER           0b00000000000010000000
#define IS_LAVA            0b00000000000100000000
#define IS_COLOREDSPECIAL  0b00000000001000000000
#define IS_GRASS           0b00000000010000000000
#define IS_ICE             0b00000000100000000000
#define IS_OBJECT          0b00000001000000000000
#define IS_DOOR            0b00000010000000000000
#define IS_PORTAL          0b00000100000000000000
#define IS_HARD            0b00001000000000000000
#define IS_BLOCKTNT        0b00010000000000000000


enum BLOCK_TEXTURES{
	TEX_GRASS_TOP=0,
    TEX_GRASS_SIDE_COLOR=1,
    TEX_GRASS_SIDE=2,
    TEX_DIRT=3,
    TEX_SAND=4,
    TEX_STONE=5,
	TEX_BEDROCK=6,	
    TEX_TREE_SIDE=7,
	TEX_TREE_VERT=8,	
	TEX_WOOD=9,			
	TEX_TNT_SIDE_COLOR=10,
    TEX_TNT_SIDE=11,
	TEX_TNT_TOP_COLOR=12,
    TEX_TNT_TOP=13,
	TEX_GRASS_TOP2=14,
	TEX_DARK_STONE=15,
	TEX_LEAVES=16,	
	TEX_STEEL=17,
	TEX_BLOCKTNT=18,
	TEX_BRICK_COLOR=19,
    TEX_BRICK=20,
	TEX_COBBLESTONE=21,	
	TEX_CRYSTAL=22,
	TEX_LIGHTBOX=23,
    TEX_LADDER=24,
    TEX_CLOUD=25,   
    TEX_VINE=26,
    TEX_SHINGLE=27,
    TEX_GRADIENT=28,
    TEX_ICE=29,
    
    
    TEX_GLASS=0,	
    TEX_WEAVE=8,
    TEX_WATER=16,
    TEX_LAVA=24,
    TEX_TRAMPOLINE=30,
    TEX_FIREWORK=31,
    
};

#endif
