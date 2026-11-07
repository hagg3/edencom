// K.2-2: the per-type behaviour tables behind BlockTraits.h (WORKING/data-driven-content-plan-2026-11-01.md
// D8/D9/D11). The stock rows were generated from the K.2-1 oracle (port/tools/fixtures/content/
// stock-dump.txt, `## oracle` / `## tiles`), i.e. from the per-site `type==TYPE_X` chains as they stood;
// `--content-dump` re-prints those sections from these tables and check-content-dump.py pins them.
#include "BlockTraits.h"
#include <string.h>

// K.2-3: every table below is filled from the stock pack (media/packs/eden/pack.json "sound_sets", each
// block's "sounds" / "paint_art_sites" / the paint_art trait, each tile's "unpainted" / "particle_art") by
// ContentPack::commitStock at boot; the C initialisers K.2-2 generated from the K.2-1 oracle are gone
// (git history keeps them). The sets are tuples, not materials, because the chains they replaced tested
// type lists and blockinfo flags per event: brick builds and breaks as stone but mixes into a blast as
// "other"; sand builds generic but breaks as dirt; the block-TNT family shares the doors' set. The six
// paint-art sites disagreed (BlockTraits.h ART_*), so blockPaintArt is a per-site mask.
BlockSoundSet blockSoundSets[SOUND_SETS_MAX];
int blockSoundSetCount=0;
// Rows 128..255 come from the palette's definitions or the missing block (set 0, no paint art).
uint8_t blockSoundSet[BLOCK_IDS];
uint8_t blockPaintArt[BLOCK_IDS];
uint8_t tileTwin[ATLAS_TILES];
uint8_t tileParticleArt[ATLAS_TILES];

int trait_soundSetByName(const char* name){
    for(int i=0;i<blockSoundSetCount;i++)if(strcmp(blockSoundSets[i].name,name)==0)return i;
    return -1;
}
