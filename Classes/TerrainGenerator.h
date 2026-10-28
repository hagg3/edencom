//
//  TerrainGenerator.h
//  prototype
//
//  Created by Ari Ronen on 10/23/10.
//  Copyright 2010 __MyCompanyName__. All rights reserved.
//
#ifndef Eden_TerrainGenerator_h
#define Eden_TerrainGenerator_h


#include "Terrain.h"

// Stage H / H.4: the seeded "Classic" generator, the 1.x generateColumn revived and pinned
// (WORKING/feature-archaeology-2026-10-13/generator-plan.md §3; docs/terrain-generation.md).
// These are the GENERATOR record's fields (generator-plan.md §4.1); the record itself is
// classic_encode_record()/classic_decode_record() below.
enum { CLASSIC_CAVES_OFF=0, CLASSIC_CAVES_2011=1, CLASSIC_CAVES_DEEP=2 };
enum { CLASSIC_PROFILE_NEW=0, CLASSIC_PROFILE_LEGACY=1 };
#define CLASSIC_RECORD_BYTES 28
struct ClassicGen{
    int seed;                 // = level_seed
    unsigned char scale4;     // scale x4: 4 (1x), 6, 8, 12, 16; 256z worlds only
    unsigned char hilliness;  // 0 Plains, 1 Rolling, 2 Classic, 3 Rugged, 4 Wild
    unsigned char caves;      // CLASSIC_CAVES_*
    unsigned char trees;      // 0 None, 1 Sparse, 2 Classic, 3 Dense
    unsigned char flowers;    // 0/1: block 12 patches
    unsigned char clouds;     // 0/1
    unsigned char size;       // 0 extended, 1 2880 square (gets X.6's wall)
    unsigned char profile;    // CLASSIC_PROFILE_*
    unsigned char flags;      // bit 0: caves were auto-detected (legacy)
};
void classic_legacy_params(ClassicGen* g,int seed);    // a 1.x world's terrain (caves off until detected)
void classic_default_params(ClassicGen* g,int seed);   // a new world's defaults
// One window of terrain, a pure function of (g, coords): no engine state, no RNG state.
// types/colors are [(x*D+z)*height+y]. A 16x16 window at a column's corner is that column;
// the new profile's trees cross column borders, so a wider window (the seam gate) agrees with
// the columns it covers. The legacy profile draws trees only in a column-aligned 16x16 window.
// decorate=false stops after bedrock/stone/dirt/caves (the 1.x byte-equality gate).
void classic_generate_window(const ClassicGen& g,int wx0,int wz0,int W,int D,int height,
                             block8* types,color8* colors,bool decorate);
int classic_height(const ClassicGen& g,int wx,int wz,int height);   // the heightmap (preview, spawn)
int classic_surface_top(const ClassicGen& g,int wx,int wz,int height); // highest dirt under air, -1 if none
void classic_encode_record(const ClassicGen& g,unsigned char out[CLASSIC_RECORD_BYTES]);
// 1 = a Classic revision-1 record (g filled), 0 = not a generator this build knows (void).
int classic_decode_record(const unsigned char* in,size_t len,ClassicGen* g);
// The 36-byte `.eden` mirror in WorldFileHeader.reserved[] ("EMG1" + record + CRC-32).
void classic_encode_mirror(const ClassicGen& g,unsigned char out[36]);
int classic_decode_mirror(const unsigned char in[36],ClassicGen* g);
// Legacy cave detection (generator-plan.md §4.3): over one saved column (`.emod` bands, 4,096
// type bytes then 4,096 paint bytes each), counts the cells the 2011 model carves (n3 <= 0 in
// the 2011 band, where the no-cave model says stone) and how many of them the save holds as air.
void classic_cave_votes(int seed,int cx,int cz,const unsigned char* col,int bands,long* cells,long* air);
// --classic-selftest only: 1 = tables after an earlier random() caller (what the Windows shim,
// or any reordering, got), 2 = truncate the heightmap instead of roundf (both must fail the 1.x
// gate), 3 = new-profile trees ignore roots across the column border (must fail the seam gate).
void classic_set_mutation(int m);

class TerrainGenerator{
public:
    TerrainGenerator(Terrain* parent);
    void generateColumn(int x,int z, bool bgthread);
    void generateEmptyColumn(int cx,int cz);
    void generateCloud();
    void placeTree(int x,int z,int y);


	int LEVEL_SEED;
    bool genCaves;
    // H.4: what a non-flat, non-default seed generates. FileManager sets it at load.
    // CLASSIC_STATE_VOID: the world's GENERATOR record is from a newer Emod -> void columns.
    enum { CLASSIC_STATE_UNSET=0, CLASSIC_STATE_SET=1, CLASSIC_STATE_VOID=2 };
    int classicState;
    ClassicGen classic;
    void setClassic(const ClassicGen& g){ classic=g; classicState=CLASSIC_STATE_SET; }
};

float noise3(float vec[3]);
float noise2(float vec[2]);
void tgenInit();


#endif