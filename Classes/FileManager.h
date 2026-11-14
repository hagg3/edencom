//
//  FileManager.h
//  prototype
//
//  Created by Ari Ronen on 10/25/10.
//  Copyright 2010 __MyCompanyName__. All rights reserved.
//
#ifndef Eden_FileManager_h
#define Eden_FileManager_h


#include "Terrain.h"
#include "WorldTrailer.h"
#include "eden_fs.h"   // G.2b-2: EdenFile, the column-read handle (was NSFileHandle)
#include <string>



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
static_assert(sizeof(WorldFileHeader)==192, "WorldFileHeader is raw-memcpy'd to disk: 192 bytes");
typedef struct{
	int x, z;
	unsigned long long chunk_offset;
}ColumnIndex;
static_assert(sizeof(ColumnIndex)==16, "ColumnIndex is raw-memcpy'd to disk: 16 bytes");
typedef struct{
	int n_vertices;

// RENAMED from `ChunkHeader` for Phase N Stage 1 (WORKING/native-migration-plan-2026-09-04.md).
// macOS's real <Foundation/Foundation.h> transitively includes CoreServices, whose
// CarbonCore/AIFF.h has carried `typedef struct ChunkHeader ChunkHeader;` since the 1990s — so the
// two collided in every engine translation unit on the native target. This is a portability fix,
// not a platform #ifdef: the name changes on every target, the struct layout does not, and the
// type is not written to disk by any live code path (its only uses are inside the commented-out
// mesh-caching block in FileManager.cpp around line 1359 — archaeology, kept per CLAUDE.md #6 and
// renamed alongside so it stays coherent if anyone revives it).
}EdenChunkHeader;

// 256z Stage 3 item 5: the in-app "Convert to 64z" action (Settings -> Storage tab). Same report
// shape as port/tools/eden-convert.js's --to-64 direction, whose algorithm this is a from-scratch
// C++ port of (see FileManager::convertWorldTo64's own header comment for what's shared and what
// isn't). `ok`==FALSE means nothing was written; `error` explains why.
typedef struct{
    bool ok;
    char error[160];
    int columns;
    int blocksDiscarded;
    int columnsAffected;
    int doorsOrphaned;
    int creaturesDropped;
    int creaturesRelocated;
    int creaturesOverflow;
    bool posClamped;
    bool homeClamped;
}ConvertTo64Report;

// H.15: the Properties screen's record of a world (Classes/WorldProps.h; its own header because it
// carries a ClassicGen, and TerrainGenerator.h -> Terrain.h -> this file would be a cycle).
struct WorldProps;

// B.5b-1b: engine seconds since the open world's last committed save (any kind). Terrain::update
// advances it and runs the autosave when it passes g_autosave_seconds with edits pending;
// saveEmodWorld zeroes it on a commit. g_autosave_seconds <= 0 turns the autosave off (tests).
extern float g_seconds_since_save;
extern float g_autosave_seconds;
// QA.5: saves that failed in this process (the save-fail gate counts them).
extern int g_save_failures;
// QA.5 mutations (--save-fail-mutate=): SAVE_TEST_IGNORE_EXIT lets Save & Quit and warps, and
// SAVE_TEST_IGNORE_STREAM the ring and bulk streaming, go ahead on a failed save, as before QA.5.
enum { SAVE_TEST_IGNORE_EXIT=1, SAVE_TEST_IGNORE_STREAM=2 };
extern int g_save_test_ignore;

class FileManager {
public:
    int chunkOffsetX;
    int chunkOffsetZ;
    
    std::string documents;   // G.2b-2: the saves directory (was a retained NSString*)
    bool convertingWorld;
    bool genflat;
    bool worldUnbounded;   // X.6: world holds data outside the bundled 2880 square -> Player::update skips the edge clamp
    // MP.3a: the open world is a multiplayer session (Classes/MpWorld.h): no file; loadWorld synthesises it
    // from MpWorld's header, readColumn is the flat recipe + overlay, and the three saves commit to RAM.
    bool sessionWorld;
    // QA.14 (A2-03): this load runs on the world-load pthread (World::loadWorld sets it before
    // pthread_create and clears it after the join), so loadWorld leaves what the main thread owns while
    // the menu is up to finishLoadOnMain(): the input/effects/HUD resets and a new world's first save.
    // An inline load (warp, reload, LOW_MEM_DEVICE) has it false and does both in loadWorld, as always.
    bool loadOnThread=false;
    bool firstSaveOwed=false;
    void finishLoadOnMain();
    FileManager();
    bool worldExists(std::string name,bool appendArchive);
    void saveGenColumn(int cx,int cz,int origin);
    // G.2b-2: `file` is the open `.eden`'s read handle (null for an `.emod`, a session or a world with
    // no file yet: a column that would be read from it then keeps its slot's old bytes, as a nil
    // NSFileHandle did).
    void readColumn(int cx,int cz,EdenFile* file);
    // B3 Stage 3. What Terrain's bulk reload calls instead of readColumn(). Identical, except that
    // a column coming from the BUNDLED default map -- the only path with an RLE decode in it, and
    // B1's ~75% of the burst's column-read cost -- may be handed to a worker instead. Returns TRUE
    // if the column has landed synchronously (the caller marks it loaded), FALSE if a decode job
    // was dispatched and the column will land in a later frame. readColumn() itself is unchanged
    // and still synchronous for every other caller, world load included.
    bool columnWillGenerate(int cx,int cz);   // H.4: readColumn would run the Classic generator
    // QA.5: every save answers whether it committed. On a failure the store has rolled back to the
    // last commit and kept every `modified` flag (nothing is lost while the world stays open); the
    // reason is on stderr (`emod: save failed: ...`) and on the HUD's status line, and a caller that
    // would discard the window (exit, warp, a stream overwrite) must not go ahead (docs/save-load.md).
    bool saveWorld();
    bool saveWorld(Vector warp);
    // B.5b-1b: saveWorld() WITHOUT its opening endDynamics -- fire keeps burning and water keeps
    // flowing across it. The ring stream's evict-save (a modified column is about to be overwritten)
    // and the autosave timer use it; warp, exit, the Save button, app background and the bulk reload
    // keep the full saveWorld() (render-distance plan, decision 1).
    // QA.5: false without trying while saveHeld() (an automatic retry would fail the same way).
    bool saveWorldKeepDynamics();
    // QA.5: the last save failed and no save has committed since; why, as the store said it.
    bool saveFailing() const { return saveFailing_; }
    const std::string& lastSaveError() const { return lastSaveError_; }
    // QA.5: within SAVE_RETRY_HOLD_MS of a failure: the automatic saves (autosave, the ring's evict-save,
    // the bulk reload's, a death respawn's) skip rather than hammer a full disk every frame. The
    // player's own saves (button, exit, warp) always try.
    bool saveHeld() const;
    static const int SAVE_RETRY_HOLD_MS=2000;
    void loadGenFromDisk();
    void writeGenToDisk();
    void fwriteDirectory();
    void readDirectory();
    void clearDirectory();
    void compressLastPlayed();
    // G.2b-2: every name/path parameter is a std::string, and a std::string return of "" means what
    // nil meant (getName: unreadable as UTF-8 text; pairedEmodFor/originalOf/conversionError/
    // pendingOriginal: none). getName still answers "error~"/"error" for a missing/blank header.
    std::string getName(const std::string& file_name);
    // N.4.5: the world list's rename. Rewrites WorldFileHeader::name in place and nothing else;
    // a world with no file yet (created, never played) needs no write — its first save takes the
    // name from Menu::selected_world. Returns FALSE only if a file exists and the write failed.
    bool renameWorld(const std::string& file_name, const std::string& display_name);
    // H.15: the Properties screen's pair. read: FALSE if the file cannot be read. apply: writes
    // only what differs from the file -- for an `.emod`, ONE committed batch of WORLD_HEADER (the
    // name; pos/home lifted to the new surface where their column is not saved land) and, for a
    // Classic world, a newer GENERATOR (same seed and profile; the detected flag cleared when caves
    // changed). A legacy/mirror world gets its record on any save. `.eden`: the name only.
    // Returns WP_WROTE_* / WP_LIFTED_* bits, 0 when nothing differed, -1 when the write failed.
    bool readWorldProperties(const std::string& file_name, WorldProps* out);
    int  applyWorldProperties(const std::string& file_name, const WorldProps& edited);
    void setName(std::string fn,std::string dn);
    void setImageHash(const std::string& hash);
    void loadWorld(const std::string& name,bool fromArchive);
    bool deleteWorld(const std::string& name);
    // Reads ONLY the header of an existing save and answers 64 or 256, so World::loadWorld can call
    // eden_set_world_height() before Terrain::allocateMemory() sizes the per-world arrays. A world
    // that does not exist yet (or any file we can't read a header from) answers 64: new worlds are
    // 64z, which is the decision recorded in the 256z plan.
    int probeWorldHeight(const std::string& name,bool fromArchive);
    // 256z Stage 3 item 5: convert an existing 256z ("New Dawn") save to 64z in place (space
    // reclaim). Destructive -- see the function body for exactly what it discards -- so it writes
    // to a scratch file and only replaces the original on full success, same temp+rename pattern
    // saveWorld() uses. Refuses if `name` is the world currently open in this session.
    ConvertTo64Report convertWorldTo64(const std::string& name);

    // K.1-3 (block-registry plan D13/D14): before the first mod block id is allocated in the open world.
    // A flagged world: true. An unflagged one is rewritten once by the flag compaction (FileHeader flags
    // bit 0 + a BLOCK_PALETTE that reserves every latent byte >= 128 as `emod:unknown_<id>`), then the
    // registry reloads that palette, so a new id can never give old garbage a meaning. Main thread, in
    // play. False + `why` when there is no open `.emod`, it is damaged, or the rewrite fails.
    bool preparePaletteAllocation(std::string* why);
    // K.2-5: the open world is already flagged for mod blocks (no compaction needed; no first-placement confirm).
    bool paletteFlagged() const;
    // ---- Stage S: `.emod` worlds (docs/emod-file-format.md; Classes/EdenWorldStore.h) ----
    // A world is an `.emod` iff its file name ends in ".emod"; every method above routes on that.
    static bool isEmodName(const std::string& name);
    // A dropped-in `.gz` / `.zip` (a compressed `.eden`): only ever a source for convertStep().
    static bool isArchiveName(const std::string& name);
    // The key for a brand-new world: "Worlds/<id>/world.emod" (K.0w; the folder is made by the first save).
    static std::string newWorldFileName();
    // ---- K.0w world folders (WORKING/world-folders-plan-2026-10-30.md §3, §5.1) ----
    // A world key is a Documents-relative path. A folder world is "Worlds/<id>/world.emod"; a loose one
    // is its bare name. The id lives in the folder name only.
    static bool validWorldId(const std::string& id);               // [0-9A-Za-z_-]{1,64}, no leading dot
    static std::string newWorldId(const std::string& documents);   // "<unix>-<4 a-z>", folder name free
    static std::string downloadWorldId(const std::string& documents,const std::string& serverId);
    static std::string worldKeyOfId(const std::string& id);        // "Worlds/<id>/world.emod"
    static std::string stagingDirOf(const std::string& id);        // "Worlds/.<id>.partial" (a Documents-relative path)
    // K.0w-3 staging (plan §5.2): an import writes `<documents>/Worlds/.<id>.partial/world.emod`; the commit
    // point is ONE no-replace rename of that dir to `Worlds/<id>` (a fresh id if taken). beginStaging makes the
    // dir (returns its absolute path, "" on failure); commitStaging returns the new key ("" = still staged).
    // stageStep is the EDEN_TEST_STAGE_KILL_AFTER=<n> hook (_Exit(86) after the n-th staging step).
    static std::string beginStaging(const std::string& documents,const std::string& id);
    static std::string commitStaging(const std::string& documents,const std::string& id);
    static void discardStaging(const std::string& documents,const std::string& id);
    static void stageStep(const char* what);
    static bool isFolderWorld(const std::string& key);             // "Worlds/<valid id>/world.emod"
    static std::string worldDirOf(const std::string& key);         // "Worlds/<id>" or "" for a loose key
    static std::string photoPathOf(const std::string& key);        // folder: <dir>/photo.png; loose: key+".png"
    static std::string originalPathOf(const std::string& key,const std::string& ext); // <dir>/original<ext>
    static std::string sidecarPathOf(const std::string& key,const std::string& kind); // kind "signs"|"cmd": <dir>/<kind>.eden.dat
    // Rule 4: removes `<documents>/Worlds/<valid id>` and nothing else (false for any other path).
    // K.0w-1: every "Worlds/<id>/world.emod" that exists (listWorlds' folder half).
    std::vector<std::string> folderWorldKeys();
    // K.0w-4 (plan §5.3): THE world list, shared by Menu::loadWorlds and the Storage seam so "row N" cannot
    // disagree. Folder worlds + loose .emod by world.emod mtime (newest first, ties by key), then dropped
    // sources (.eden/archives that still need converting) by name. A .eden with a paired .emod is hidden and
    // its size goes on the .emod's original_bytes. Adoption is NOT run here (callers run adoptLooseWorlds first).
    struct WorldEntry{ std::string key,name; long long mtime=0,original_bytes=0; bool needs_convert=false; };
    void listWorlds(std::vector<WorldEntry>* out);
    bool removeWorldDir(const std::string& relDir);
    // K.0w-2 (plan §4): recovers any `Worlds/.<id>.migrating` and moves every loose, readable `<X>.emod`
    // in the Documents root (but the open world) into `Worlds/<id>/` with its photo, paired original
    // and 2026-game sidecars. Crash-safe and idempotent; a no-op while a convert/download/import job
    // runs. Runs at the top of every world-list scan. Returns the number of worlds adopted/recovered;
    // `moved` gets (loose name, new key) pairs.
    int adoptLooseWorlds(std::vector<std::pair<std::string,std::string> >* moved=NULL);
    // The key a loose name was adopted under in this process, or the name itself (harness lookups).
    static std::string adoptedKeyOf(const std::string& looseName);
    // The world list's two questions about a `.eden`: is it already converted (a `<stem>.emod`
    // whose PROVENANCE names a source of the same size -> the list hides the `.eden`), and does it
    // still need converting.
    bool edenHasPairedEmod(const std::string& edenName);
    bool worldNeedsConversion(const std::string& name);
    // S.5: the `.emod` a source is paired with ("" if none), the reverse (the kept original `.eden`
    // or archive of an `.emod`, "" if none), and the world list's "Remove original" (deletes only
    // that file; the `.emod` is untouched).
    std::string pairedEmodFor(const std::string& srcName);
    std::string originalOf(const std::string& emodName);
    bool removeOriginal(const std::string& emodName);
    // Deletes stale `*.emod.converting` / `.spill` / `.creating` / `.compact` left by a killed run.
    void cleanConversionTemps();
    // Convert `name` (a `.eden`) to a new `.emod`, a time slice per call so the loading screen keeps
    // drawing. Returns 0 while running (`*percent` set), 1 when done (`*outName` = the `.emod`),
    // -1 when it could not (too little free space, an unsupported header, a write error): the caller
    // then plays the `.eden` in place exactly as before. Nothing is ever written to the `.eden`.
    int  convertStep(const std::string& name, int* percent, std::string* outName);
    // S.5e: the height that conversion writes. upgradeOnConvert() is Settings' "Upgrade 64z worlds to
    // 256z when converting" (EDEN_UPGRADE_256Z overrides it). convertHeightGate(name) runs before the
    // first convertStep(): 1 = go, 0 = the "Keep 64z?" prompt is up (call again next frame), -1 = the
    // player cancelled. It asks only when the toggle is on and the source is not already 256z, and
    // never under EDEN_CONVERT_PROMPT=0. answerConvertHeight() is the prompt's callback: 1 upgrade,
    // 0 keep the source's height, -1 cancel.
    static bool upgradeOnConvert();
    int  convertHeightGate(const std::string& name);
    void answerConvertHeight(int choice);
    bool conversionActive();
    // Why the last convertStep() returned -1 ("" if it did not).
    std::string conversionError();
    // The `.eden` that a finished conversion left behind and the player has not been asked about
    // yet ("" if none). resolveOriginal(TRUE) deletes it, (FALSE) keeps it (it stays hidden: paired).
    std::string pendingOriginal();
    void resolveOriginal(bool deleteIt);
    // Closes the open `.emod` store (exit to menu, delete, a re-load). A no-op for `.eden` worlds.
    void closeWorld();
    // TRUE while the open world is an `.emod` (the store is open). For diagnostics.
    bool emodOpen();

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
    void loadTrailer(const std::string& name,const unsigned char* bytes,unsigned long long len);
    bool loadEmodWorld(const std::string& name);
    bool saveEmodWorld(Vector warp);
    bool saveFailed(const std::string& why);   // QA.5: stderr + status line; always returns false
    void saveCommitted();                       // QA.5: clears the failing state
    bool saveFailing_=false;
    std::string lastSaveError_;
    double saveFailedAtMs_=0;
    int saveFailStreak_=0;
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