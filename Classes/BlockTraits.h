// K.2-1 extracted each per-type behaviour decision from its call sites into one function per question;
// K.2-2 turned the bodies into table reads (WORKING/data-driven-content-plan-2026-11-01.md D8, §3.3):
// `blockinfo` bits 18-23 (Constants.h), the sound sets, the paint-art site masks and the tile twins below.
// The stock rows were filled from the K.2-1 oracle (today's code over all 256 type values) and
// `--content-dump`'s `## oracle` and `## tiles` sections are pinned unchanged (check-content-dump.py),
// so a table read answers exactly what the old `type==TYPE_X` chain did. A mod block (K.2-4) gets the
// same behaviour by carrying the trait; since K.2-3 the stock rows come from the `eden` pack (Classes/ContentPack).
//
// Every function takes a plain int and bounds it: some callers pass -1 (getLandc's "invalid"),
// Model.cpp's 999999 box sentinel, or a HUD tool cell (1000+); out of range answers "no trait" and
// sound set 0.
// D9 (deliberate change): the sounds no longer read IS_FLAMMABLE live, so Settings > Experiments >
// Fire melts ice leaves ice sounding like ice (it used to build and break like wood).
#ifndef EDEN_BLOCK_TRAITS_H
#define EDEN_BLOCK_TRAITS_H

#include <stdint.h>
#include "Globals.h"   // Constants.h + blockinfo

static inline int trait_info(int t) { return (unsigned)t < BLOCK_IDS ? blockinfo[t] : 0; }

// ---- Sound sets (BlockTraits.cpp). A set names one SOUND_TYPES id (Resources.h) per event; land and
// step may be -1 (silent). `blockSoundSet[t]` picks the set of type t; set 0 is `eden:generic` (air, the
// missing block and every row a table does not name). Rows 128..255 come from the palette's definitions
// (BlockRegistry::resolve) or the missing block (eden_set_missing_block_row).
enum { TRAIT_TNT_OTHER = 0, TRAIT_TNT_DIRT = 1, TRAIT_TNT_STONE = 2, TRAIT_TNT_GLASS = 3 };   // explosion mix
enum { SOUND_SETS_MAX = 64 };
struct BlockSoundSet {
    char name[32];          // `eden:stone`
    int16_t build, brk, land, step;
    uint8_t tnt;            // TRAIT_TNT_*: Terrain::tntSoundForBlast's material group
};
extern BlockSoundSet blockSoundSets[SOUND_SETS_MAX];
extern int blockSoundSetCount;
extern uint8_t blockSoundSet[BLOCK_IDS];
// -1 when no set has that name.
int trait_soundSetByName(const char* name);

static inline const BlockSoundSet& trait_sounds(int t) {
    return blockSoundSets[(unsigned)t < BLOCK_IDS ? blockSoundSet[t] : 0];
}
static inline int trait_buildSound(int t) { return trait_sounds(t).build; }   // Player::processInput ×2 build paths
static inline int trait_breakSound(int t) { return trait_sounds(t).brk; }     // the mine path
static inline int trait_landSound(int t) { return trait_sounds(t).land; }     // Player::vertc (the site skips it on ice)
static inline int trait_stepSound(int t) { return trait_sounds(t).step; }     // Player::move footsteps
static inline int trait_tntGroup(int t) { return trait_sounds(t).tnt; }       // Terrain::tntSoundForBlast

// ---- Movement and the world (blockinfo bits 18-23).
static inline bool trait_climbable(int t) { return trait_info(t) & IS_CLIMB; }
static inline bool trait_bouncy(int t) { return trait_info(t) & IS_BOUNCY; }
static inline bool trait_slippery(int t) { return trait_info(t) & IS_SLIPPERY; }        // Player + Model collision
static inline bool trait_blastproof(int t) { return trait_info(t) & IS_BLASTPROOF; }    // Terrain::explode
static inline bool trait_unbreakable(int t) { return trait_info(t) & IS_UNBREAKABLE; }  // mine + destroyBlock
static inline bool trait_noShadow(int t) { return trait_info(t) & IS_NOSHADOW; }        // shadow_buildCasterTable

// ---- "Draws its own art when unpainted" (the face draws white and the tile swaps to its unpainted
// twin). Stock's six sites disagreed (trampoline, grass 8/11, the block-TNT family), and each answer is
// pinned, so the trait is a per-site mask: a mod block's `paint_art` sets ART_ALL.
enum {
    ART_CHUNK = 1,      // TerrainChunk mesher, chunk path
    ART_BATCH = 2,      // TerrainChunk mesher, object-batch path
    ART_CURSOR = 4,     // Graphics::drawCube, the build preview
    ART_ICON = 8,       // Hud::renderBlockAndBorder, the selected-block icon
    ART_PICKER = 16,    // Hud::renderBlockScreen, the picker cells
    ART_PARTICLE = 32,  // BlockBreak debris (also by drawn tile: tileParticleArt)
    ART_ALL = 63,
};
extern uint8_t blockPaintArt[BLOCK_IDS];
static inline bool trait_paintArt(int t, int site) { return (unsigned)t < BLOCK_IDS && (blockPaintArt[t] & site); }
static inline bool trait_paintArtChunk(int t) { return trait_paintArt(t, ART_CHUNK); }
static inline bool trait_paintArtBatch(int t) { return trait_paintArt(t, ART_BATCH); }
static inline bool trait_paintArtCursor(int t) { return trait_paintArt(t, ART_CURSOR); }
static inline bool trait_paintArtIcon(int t) { return trait_paintArt(t, ART_ICON); }
static inline bool trait_paintArtPicker(int t) { return trait_paintArt(t, ART_PICKER); }

// ---- Per atlas.png tile: its unpainted twin (0 = none: tile 0 is grass top, never a twin) and whether
// debris drawn with it takes the art (white) whatever the type.
extern uint8_t tileTwin[ATLAS_TILES];
extern uint8_t tileParticleArt[ATLAS_TILES];
static inline bool trait_hasUnpaintedTwin(int bf) { return (unsigned)bf < ATLAS_TILES && tileTwin[bf] != 0; }
static inline int trait_unpaintedTwin(int bf) { return trait_hasUnpaintedTwin(bf) ? tileTwin[bf] : bf; }
static inline bool trait_paintArtParticle(int t, int bf) {
    return ((unsigned)bf < ATLAS_TILES && tileParticleArt[bf]) || trait_paintArt(t, ART_PARTICLE);
}

#endif
