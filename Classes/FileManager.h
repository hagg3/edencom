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
// shape as web/tools/eden-convert.js's --to-64 direction, whose algorithm this is a from-scratch
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

// PORT SEAM (Phase N Stage 2), same shape as Hud.h's eden_hud_draw_menu_screen_hook: NULL by
// default, so stock behaviour is unchanged and this costs one null test per save.
//
// Called at the top of FileManager::saveWorld(Vector) with the world file's full path, BEFORE
// anything writes to it, so a host that wants a "previous save" slot can copy the last-known-good
// bytes aside. The WEB build leaves it NULL — its Foundation shim owns every file open and puts
// the same guard on NSFileHandle instead, which is strictly better placed (it also covers the
// rollback journal). NATIVE installs it, because it links Apple's real Foundation and has no such
// choke point. See web/src/shim/foundation/save_backup.h.
extern void (*eden_save_backup_hook)(const char* worldFilePath);

// The save directory, for a host that wants to choose it rather than let the platform decide.
// NULL by default, in which case FileManager's constructor keeps its original
// NSSearchPathForDirectoriesInDomains(NSDocumentDirectory) lookup unchanged — which is what the
// web build uses, because its Foundation shim already answers that call with the port's own root.
// Native installs this: real Foundation would otherwise put worlds in ~/Documents, and Stage 2
// chose ~/Library/Application Support/Eden. The returned string must outlive the process.
extern const char* (*eden_documents_root_hook)(void);

class FileManager {
public:
    int chunkOffsetX;
    int chunkOffsetZ;
    
    NSString* documents;
    BOOL convertingWorld;
    BOOL genflat;
    FileManager();
    BOOL worldExists(std::string name,BOOL appendArchive);
    void saveColumn(int cx,int cz);
    void saveGenColumn(int cx,int cz,int origin);
    void readColumn(int cx,int cz,NSFileHandle* nsfh);
    // B3 Stage 3. What Terrain's bulk reload calls instead of readColumn(). Identical, except that
    // a column coming from the BUNDLED default map -- the only path with an RLE decode in it, and
    // B1's ~75% of the burst's column-read cost -- may be handed to a worker instead. Returns TRUE
    // if the column has landed synchronously (the caller marks it loaded), FALSE if a decode job
    // was dispatched and the column will land in a later frame. readColumn() itself is unchanged
    // and still synchronous for every other caller, world load included.
    BOOL readColumnDeferred(int cx,int cz,NSFileHandle* nsfh);
    void saveWorld();
    void saveWorld(Vector warp);
    void loadGenFromDisk();
    void writeGenToDisk();
    void fwriteDirectory();
    void readDirectory();
    void clearDirectory();
    void compressLastPlayed();
    void convertFile(NSString* file_name);
    NSString* getName(NSString* file_name);
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

private:
    int oldOffsetX;
    int oldOffsetZ;
    void LoadCreatures();
    void saveCreatures();
    // Per-column record span, in bytes, for the columns whose span is SHORTER than a full record.
    // The New Dawn specimen has exactly one such column (107,072 B where 131,072 was expected), and
    // reading it at full stride would pull in 24,000 bytes of its neighbour. Keyed by twoToOne(x,z),
    // value is the byte span; a column absent from this map has a full-size record.
    void* shortSpans;   // map_t (hashmap.h); declared void* so this header stays include-order-free
    void deriveColumnSpans();
    void clearColumnSpans();
    // B5: if the last save of this world was interrupted mid-write, put the file back the way the
    // last COMPLETE save left it, using the small journal saveWorld() writes before it starts
    // rewriting a large file in place. A no-op (one stat) when there is no journal, which is every
    // world below g_save_inplace_threshold and every world whose last save finished normally.
    void recoverInterruptedSave(NSString* file_name);
};
std::string fullPathForFilename(const char* fn);
#endif