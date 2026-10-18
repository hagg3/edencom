//
//  FileManager.h
//  prototype
//
//  Created by Ari Ronen on 10/25/10.
//  Copyright 2010 __MyCompanyName__. All rights reserved.
//
#ifndef Eden_FileManager_h
#define Eden_FileManager_h


#import "Terrain.h"
#include "WorldTrailer.h"



#define FILE_VERSION 4
// The first header version that means 256 blocks tall (16 chunk-bands per column record).
// version 6 exists in the wild too and is treated as 256z; what distinguishes it from 5 is
// unknown, so a >=5 file keeps its OWN version on save -- see docs/eden-file-format.md.
#define FILE_VERSION_256Z 5
#define FILE_VERSION_256Z_MAX 6
// Runtime, because CHUNKS_PER_COLUMN is (Constants.h): 32768 at 64z, 131072 at 256z.
#define SIZEOF_COLUMN ((unsigned long long)g_column_bytes)


typedef struct{
	int level_seed;
	Vector pos;
	Vector home;
	float yaw;
	unsigned long long directory_offset;
	char name[50];
    
    //below here is post 1.1.1 stuff
    int version;
    char hash[36];
    unsigned char skycolors[16];
    int goldencubes;
	char reserved[100-sizeof(int)-36-16-sizeof(int)];	 //subtract new stuff from reserve bytes,
    //192 bytes(including padding is the correct size, be careful modifying this to not corrupt old maps
}WorldFileHeader;
typedef struct{
	int x, z;
	unsigned long long chunk_offset;
}ColumnIndex;
typedef struct{
	int n_vertices;

// RENAMED from `ChunkHeader` for Phase N Stage 1 (WORKING/native-migration-plan-2026-09-04.md).
// macOS's real <Foundation/Foundation.h> transitively includes CoreServices, whose
// CarbonCore/AIFF.h has carried `typedef struct ChunkHeader ChunkHeader;` since the 1990s — so the
// two collided in every engine translation unit on the native target. This is a portability fix,
// not a platform #ifdef: the name changes on every target, the struct layout does not, and the
// type is not written to disk by any live code path (its only uses are inside the commented-out
// mesh-caching block in FileManager.mm around line 1359 — archaeology, kept per CLAUDE.md #6 and
// renamed alongside so it stays coherent if anyone revives it).
}EdenChunkHeader;

// 256z Stage 3 item 5: the in-app "Convert to 64z" action (Settings -> Storage tab). Same report
// shape as port/tools/eden-convert.js's --to-64 direction, whose algorithm this is a from-scratch
// C++ port of (see FileManager::convertWorldTo64's own header comment for what's shared and what
// isn't). `ok`==FALSE means nothing was written; `error` explains why.
typedef struct{
    BOOL ok;
    char error[160];
    int columns;
    int blocksDiscarded;
    int columnsAffected;
    int doorsOrphaned;
    int creaturesDropped;
    int creaturesRelocated;
    int creaturesOverflow;
    BOOL posClamped;
    BOOL homeClamped;
}ConvertTo64Report;

// H.15: the Properties screen's record of a world (Classes/WorldProps.h; its own header because it
// carries a ClassicGen, and TerrainGenerator.h -> Terrain.h -> this file would be a cycle).
struct WorldProps;

// PORT SEAM (Phase N Stage 2), same shape as Hud.h's eden_hud_draw_menu_screen_hook: NULL by
// default, so stock behaviour is unchanged and this costs one null test per save.
//
// Called at the top of FileManager::saveWorld(Vector) with the world file's full path, BEFORE
// anything writes to it, so a host that wants a "previous save" slot can copy the last-known-good
// bytes aside. The WEB build leaves it NULL — its Foundation shim owns every file open and puts
// the same guard on NSFileHandle instead, which is strictly better placed (it also covers the
// rollback journal). NATIVE installs it, because it links Apple's real Foundation and has no such
// choke point. See port/src/shim/foundation/save_backup.h.
extern void (*eden_save_backup_hook)(const char* worldFilePath);

// The save directory, for a host that wants to choose it rather than let the platform decide.
// NULL by default, in which case FileManager's constructor keeps its original
// NSSearchPathForDirectoriesInDomains(NSDocumentDirectory) lookup unchanged — which is what the
// web build uses, because its Foundation shim already answers that call with the port's own root.
// Native installs this: real Foundation would otherwise put worlds in ~/Documents, and Stage 2
// chose ~/Library/Application Support/Eden. The returned string must outlive the process.
extern const char* (*eden_documents_root_hook)(void);

// B.5b-1b: engine seconds since the open world's last committed save (any kind). Terrain::update
// advances it and runs the autosave when it passes g_autosave_seconds with edits pending;
// saveEmodWorld zeroes it on a commit. g_autosave_seconds <= 0 turns the autosave off (tests).
extern float g_seconds_since_save;
extern float g_autosave_seconds;

class FileManager {
public:
    int chunkOffsetX;
    int chunkOffsetZ;
    
    NSString* documents;
    BOOL convertingWorld;
    BOOL genflat;
    BOOL worldUnbounded;   // X.6: world holds data outside the bundled 2880 square -> Player::update skips the edge clamp
    // MP.3a: the open world is a multiplayer session (Classes/MpWorld.h): no file; loadWorld synthesises it
    // from MpWorld's header, readColumn is the flat recipe + overlay, and the three saves commit to RAM.
    BOOL sessionWorld;
    FileManager();
    BOOL worldExists(std::string name,BOOL appendArchive);
    void saveGenColumn(int cx,int cz,int origin);
    void readColumn(int cx,int cz,NSFileHandle* nsfh);
    // B3 Stage 3. What Terrain's bulk reload calls instead of readColumn(). Identical, except that
    // a column coming from the BUNDLED default map -- the only path with an RLE decode in it, and
    // B1's ~75% of the burst's column-read cost -- may be handed to a worker instead. Returns TRUE
    // if the column has landed synchronously (the caller marks it loaded), FALSE if a decode job
    // was dispatched and the column will land in a later frame. readColumn() itself is unchanged
    // and still synchronous for every other caller, world load included.
    BOOL readColumnDeferred(int cx,int cz,NSFileHandle* nsfh);
    BOOL columnWillGenerate(int cx,int cz);   // H.4: readColumn would run the Classic generator
    void saveWorld();
    void saveWorld(Vector warp);
    // B.5b-1b: saveWorld() WITHOUT its opening endDynamics -- fire keeps burning and water keeps
    // flowing across it. The ring stream's evict-save (a modified column is about to be overwritten)
    // and the autosave timer use it; warp, exit, the Save button, app background and the bulk reload
    // keep the full saveWorld() (render-distance plan, decision 1).
    void saveWorldKeepDynamics();
    void loadGenFromDisk();
    void writeGenToDisk();
    void fwriteDirectory();
    void readDirectory();
    void clearDirectory();
    void compressLastPlayed();
    NSString* getName(NSString* file_name);
    // N.4.5: the world list's rename. Rewrites WorldFileHeader::name in place and nothing else;
    // a world with no file yet (created, never played) needs no write — its first save takes the
    // name from Menu::selected_world. Returns FALSE only if a file exists and the write failed.
    BOOL renameWorld(NSString* file_name, NSString* display_name);
    // H.15: the Properties screen's pair. read: FALSE if the file cannot be read. apply: writes
    // only what differs from the file -- for an `.emod`, ONE committed batch of WORLD_HEADER (the
    // name; pos/home lifted to the new surface where their column is not saved land) and, for a
    // Classic world, a newer GENERATOR (same seed and profile; the detected flag cleared when caves
    // changed). A legacy/mirror world gets its record on any save. `.eden`: the name only.
    // Returns WP_WROTE_* / WP_LIFTED_* bits, 0 when nothing differed, -1 when the write failed.
    BOOL readWorldProperties(NSString* file_name, WorldProps* out);
    int  applyWorldProperties(NSString* file_name, const WorldProps& edited);
    void setName(std::string fn,std::string dn);
    void setImageHash(NSString* hash);
    void loadWorld(NSString* name,BOOL fromArchive);
    BOOL deleteWorld(NSString* name);
    // Reads ONLY the header of an existing save and answers 64 or 256, so World::loadWorld can call
    // eden_set_world_height() before Terrain::allocateMemory() sizes the per-world arrays. A world
    // that does not exist yet (or any file we can't read a header from) answers 64: new worlds are
    // 64z, which is the decision recorded in the 256z plan.
    int probeWorldHeight(NSString* name,BOOL fromArchive);
    // 256z Stage 3 item 5: convert an existing 256z ("New Dawn") save to 64z in place (space
    // reclaim). Destructive -- see the function body for exactly what it discards -- so it writes
    // to a scratch file and only replaces the original on full success, same temp+rename pattern
    // saveWorld() uses. Refuses if `name` is the world currently open in this session.
    ConvertTo64Report convertWorldTo64(NSString* name);

    // ---- Stage S: `.emod` worlds (docs/emod-file-format.md; Classes/EdenWorldStore.h) ----
    // A world is an `.emod` iff its file name ends in ".emod"; every method above routes on that.
    static BOOL isEmodName(NSString* name);
    // A dropped-in `.gz` / `.zip` (a compressed `.eden`): only ever a source for convertStep().
    static BOOL isArchiveName(NSString* name);
    // The file name for a brand-new world: genhash() + ".emod".
    static NSString* newWorldFileName();
    // The world list's two questions about a `.eden`: is it already converted (a `<stem>.emod`
    // whose PROVENANCE names a source of the same size -> the list hides the `.eden`), and does it
    // still need converting.
    BOOL edenHasPairedEmod(NSString* edenName);
    BOOL worldNeedsConversion(NSString* name);
    // S.5: the `.emod` a source is paired with (nil if none), the reverse (the kept original `.eden`
    // or archive of an `.emod`, nil if none), and the world list's "Remove original" (deletes only
    // that file; the `.emod` is untouched).
    NSString* pairedEmodFor(NSString* srcName);
    NSString* originalOf(NSString* emodName);
    BOOL removeOriginal(NSString* emodName);
    // Deletes stale `*.emod.converting` / `.spill` / `.creating` / `.compact` left by a killed run.
    void cleanConversionTemps();
    // Convert `name` (a `.eden`) to a new `.emod`, a time slice per call so the loading screen keeps
    // drawing. Returns 0 while running (`*percent` set), 1 when done (`*outName` = the `.emod`),
    // -1 when it could not (too little free space, an unsupported header, a write error): the caller
    // then plays the `.eden` in place exactly as before. Nothing is ever written to the `.eden`.
    int  convertStep(NSString* name, int* percent, NSString** outName);
    // S.5e: the height that conversion writes. upgradeOnConvert() is Settings' "Upgrade 64z worlds to
    // 256z when converting" (EDEN_UPGRADE_256Z overrides it). convertHeightGate(name) runs before the
    // first convertStep(): 1 = go, 0 = the "Keep 64z?" prompt is up (call again next frame), -1 = the
    // player cancelled. It asks only when the toggle is on and the source is not already 256z, and
    // never under EDEN_CONVERT_PROMPT=0. answerConvertHeight() is the prompt's callback: 1 upgrade,
    // 0 keep the source's height, -1 cancel.
    static BOOL upgradeOnConvert();
    int  convertHeightGate(NSString* name);
    void answerConvertHeight(int choice);
    BOOL conversionActive();
    // Why the last convertStep() returned -1 (nil if it did not).
    NSString* conversionError();
    // The `.eden` that a finished conversion left behind and the player has not been asked about
    // yet (nil if none). resolveOriginal(TRUE) deletes it, (FALSE) keeps it (it stays hidden: paired).
    NSString* pendingOriginal();
    void resolveOriginal(BOOL deleteIt);
    // Closes the open `.emod` store (exit to menu, delete, a re-load). A no-op for `.eden` worlds.
    void closeWorld();
    // TRUE while the open world is an `.emod` (the store is open). For diagnostics.
    BOOL emodOpen();

    // ---- Stage D / D.3a: the open world's signs and command blocks (Classes/WorldTrailer.h) ----
    // Loaded with the world on both containers (and from the 2026 game's `signs_`/`cmd_<world>.dat`
    // sidecars when the world has no such section). Edit it through this pointer; an edit marks it
    // dirty, and the next saveWorld() writes it even when no column changed: the `.eden` directory
    // is rewritten (its only home), an `.emod` gets a new SIGN_TRAILER record. Never NULL.
    WorldTrailer* trailer();
    // The toast for a refused trailer edit (WorldTrailer::Result), through the HUD's status bar.
    // Returns the result so callers can `return reportTrailerRefusal(r)==WorldTrailer::TR_OK`.
    int reportTrailerRefusal(int result);

private:
    void loadTrailer(NSString* name,const unsigned char* bytes,unsigned long long len);
    BOOL loadEmodWorld(NSString* name);
    void saveEmodWorld(Vector warp);
    void loadSessionWorld();   // MP.3a
    void commitSession(Vector warp);
    int oldOffsetX;
    int oldOffsetZ;
    // Per-column record span, in bytes, for the columns whose span is SHORTER than a full record.
    // The New Dawn specimen has exactly one such column (107,072 B where 131,072 was expected), and
    // reading it at full stride would pull in 24,000 bytes of its neighbour. Keyed by twoToOne(x,z),
    // value is the byte span; a column absent from this map has a full-size record.
    void* shortSpans;   // map_t (hashmap.h); declared void* so this header stays include-order-free
    void deriveColumnSpans();
    void clearColumnSpans();
};
std::string fullPathForFilename(const char* fn);
#endif