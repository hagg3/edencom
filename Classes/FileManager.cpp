//
//  FileManager.m  (G.2b-4: .mm -> .cpp, Foundation -> EdenFile/eden_fs_*/std::string)
//  prototype
//
//  Created by Ari Ronen on 10/25/10.
//  Copyright 2010 __MyCompanyName__. All rights reserved.
//

#include "FileManager.h"
#include "F3Timing.h"
#include "hashmap.h"
#include "Util.h"
#include "Terrain.h"
#include "Model.h"
#include "TerrainGen2.h"
#include "FileArchive.h"
#include "FileManagerHelper.h"
#include "World.h"
#include <cstddef>   // offsetof, renameWorld
#include <cmath>     // QA.2 isfinite
#include "EdenWorldStore.h"   // emod::eden_detect_bands -- a .eden's height (S.3b)
#include "EdenWorldSource.h"  // the raw .eden bytes out of a .eden / .gz / .zip (S.5)
#include "EdenWorldExport.h"  // X.9: Legacy64z export for the .emod "Convert to 64z"
#include "WorldProps.h"        // H.15: readWorldProperties / applyWorldProperties
#include "Alert.h"            // showAlertConvertHeight (S.5e)
#include "MpWorld.h"          // MP.3a: the session world
#include "WorldBrowser.h"     // K.0w-2: adoption waits while a download runs
#include <map>
#include <cstdlib>   // std::_Exit (the adoption kill hook)
#include "StrUtil.h"          // G.2b-4: utf8_valid, eden_cstr_copy, path_has_suffix
#include "port/src/shim/foundation/eden_fs.h"          // G.2b-4: EdenFile, eden_fs_*, eden_bundle_path
#include "port/src/shim/foundation/platform_shims.h"   // G.2b-4: eden_platform_documents_root
#include "SkyCycle.h"                                 // DN.2: the SKY_CLOCK record
#include "Weather.h"                                  // AT.1b: the WEATHER record
#include "Hotbar.h"                                   // UX.8-3: the hotbar.json sidecar
#include "BlockRegistry.h"                            // K.1-3: the BLOCK_PALETTE record

// See FileManager.h. Both NULL on web — its Foundation shim already answers both questions the
// way this port wants — and both installed by the native entry point.

#include <vector>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <chrono>
#include <ctime>
#include <sys/stat.h>
#ifndef _WIN32
#include <sys/statvfs.h>
#endif   // no <windows.h> here: its `typedef int BOOL` clashes with the engine's signed char BOOL (eden_base.h)

//#import "TestFlight.h"

// G.2b-4: no Foundation left. Every NSString these bodies held was made from an engine string by
// nsstring(), which read its c_str() and gave nil for bytes with no UTF-8 form; fm_ns is that string
// ("" for the nil), for the places where it reaches a header field or a display name.
static std::string fm_ns(const std::string& s){
    std::string c(s.c_str());
    return utf8_valid(c)?c:std::string();
}
// The NSFileHandle calls the `.eden` paths below made were messages, so a nil handle was a silent no-op
// (reads nothing, writes nothing, offsets 0). These keep that for the static handle and the locals.
static void fh_seek(EdenFile* f,unsigned long long off){if(f)f->seek((long long)off);}
static unsigned long long fh_seekEnd(EdenFile* f){long long e=f?f->seekEnd():0;return e<0?0:(unsigned long long)e;}
static unsigned long long fh_tell(EdenFile* f){long long t=f?f->tell():0;return t<0?0:(unsigned long long)t;}
static size_t fh_read(EdenFile* f,void* dst,size_t n){return f?f->read(dst,n):0;}
static std::vector<uint8_t> fh_read(EdenFile* f,size_t n){return f?f->read(n):std::vector<uint8_t>();}
static void fh_write(EdenFile* f,const void* src,size_t n){if(f)f->write(src,n);}
static void fh_truncate(EdenFile* f,unsigned long long len){if(f)f->truncate((long long)len);}
static void fh_close(EdenFile*& f){delete f;f=NULL;}   // closeFile: nothing uses a handle after it

// Web port hook (port/src/seam/LoadFailure.cpp): reports a corrupt/truncated save to the
// browser so it can offer a recovery dialog instead of silently reading garbage (perf-audit C4's
// still-open "load-failure recovery UI" item). Plain cross-TU C function call, NOT a --wrap --
// there is no existing call site to intercept, this is a new one loadWorld() makes directly. Only
// linked when the web seam sources are built; a from-scratch iOS build would need its own stub.
extern "C" void eden_report_load_failure(const char* world_file_name, const char* reason);

// Web port hook (port/src/seam/Menu.cpp): the DOM New World screen's height picker parks its
// choice here the same way it already does for flat/normal (eden_menu_take_pending_world_type),
// so probeWorldHeight() below can answer something other than the 64z default for a world that
// doesn't exist yet. "Take" semantics: one-shot, -1 means nothing pending. Only linked when the
// web seam sources are built; a from-scratch iOS build would need its own stub.
extern "C" int eden_menu_take_pending_world_height(void);
// H.4: the New World screen's Classic tab parks the GENERATOR record here (same one-shot seam).
extern "C" int eden_menu_take_pending_generator(unsigned char* out28);
static void fm_resetGenerator();
void fm_setNewWorldGenerator(const ClassicGen& g);
static Vector fm_classicSpawn(const ClassicGen& g);
static void fm_generatorToast();
static void fm_damageNotice();






static map_t indexes;
static unsigned long long cur_dir_offset;
static map_t indexes_hmm;
static FileManager* single;
static std::string docs;
static EdenFile* saveFile;   // G.2b-4: was NSFileHandle*; only the dead `.eden` writer paths use it since S.8
static WorldFileHeader* sfh;
static bool writeDirectory;
static std::string imgHash;   // G.2b-4: "" is the old NULL (a retained NSString*)
static int file_version;

// ---- Stage S / S.4: the open `.emod` world ----------------------------------------------------
// One store at a time, owned like every other file-scope static here: the world-load pthread while
// the loading screen shows, the main thread after (plan §1; World.mm has no other thread that
// reads columns -- its background loader is commented out). NULL whenever the open world is a
// `.eden` (or none), and every `.eden` code path below is reached only when it is.
static emod::EdenWorldStore* emodStore=NULL;
// The 192-byte header the open `.emod` world was loaded with, kept so a save re-emits its
// reserved bytes verbatim (the spec stores WORLD_HEADER as an opaque record). malloc'd.
static WorldFileHeader* emodHeader=NULL;
// One column's decoded bytes (bands * 8192). Static on purpose: readColumn runs on the world-load
// pthread during a load and on the main thread after it, never both at once (plan §1, checked in
// S.4: World::loadWorld's doneLoading>=1 branch touches only fm->convertingWorld), so one buffer
// replaces a 128 KB allocate-and-zero per column read.
static uint8_t emodColBuf[CHUNKS_PER_COLUMN_MAX*emod::BAND];
// A finished conversion's source `.eden`, waiting for the player's answer (nil once resolved).
static std::string pendingOriginalPath;   // K.0w-3: where the kept original sits now (Documents-relative)
static std::string pendingOriginalName;   // G.2b-2: "" once resolved (was a retained NSString*)

// The 192-byte header of a dropped-in archive (`.eden.gz`, `.zip`), inflated from its first bytes.
// True only if it reads like a world a conversion could take: a plausible directory_offset and a
// version this build knows (1.x files and unknown versions have no converter and are not listed).
static bool peekArchiveHeader(const std::string& path,WorldFileHeader* out){
    emod::WorldSource src;
    if(!src.open(path)||src.kind()==emod::WorldSource::K_RAW)return false;
    if(src.read(out,sizeof(*out))!=sizeof(*out)||src.failed())return false;
    return out->directory_offset>=sizeof(WorldFileHeader)&&out->version>=1&&out->version<=FILE_VERSION_256Z_MAX;
}
// Stage S / S.4: rename an `.emod` world on disk -- its display name lives in the WORLD_HEADER
// record, which is never rewritten in place, so a new one is appended in its own committed batch.
// Shared by renameWorld (the world list) and setName (the share screen). If that world is the one
// open, its store is used, so the two handles never disagree about the file's end.
static bool emodSetHeaderName(const std::string& path,const std::string& displayName){
    emod::EdenWorldStore own;
    emod::EdenWorldStore* st=(emodStore&&emodStore->path()==path)?emodStore:&own;
    if(st==&own&&!own.open(path.c_str(),true))return false;
    std::vector<uint8_t> raw;
    if(st->readRecord(emod::T_WORLD_HEADER,raw)>emod::EdenWorldStore::READ_FALLBACK||raw.size()!=sizeof(WorldFileHeader))return false;
    WorldFileHeader h; memcpy(&h,&raw[0],sizeof(h));
    memset(h.name,0,sizeof(h.name));
    strncpy(h.name,displayName.c_str(),sizeof(h.name)-1);
    if(st==emodStore&&emodHeader)memcpy(emodHeader->name,h.name,sizeof(h.name));
    st->beginBatch();
    st->putRecord(emod::T_WORLD_HEADER,&h,sizeof(h));
    return st->commitBatch()?true:false;
}
// S.3b: whether the open file already has its creature block between the last column and the
// directory. Stock code asked `version>=3` (a v1/v2 file had none until its first save appended
// one and stamped 4) -- but the 2026 game writes version 2 on 256z worlds WITH a 400-slot block,
// which that test would append a second block after. Set by deriveColumnSpans() from the file.
static bool creature_block_on_disk;

// ---- the post-directory sign trailer (`NewFormat256z` worlds) --------------------------------
// A 2026-08 game update appends in-game SIGN records inside the chunk-directory region, after the
// real ColumnIndex entries and before EOF, every row tagged x = 0xffffffff so twoToOne() maps it
// to its "invalid, skip" value 0. readDirectory() below has always dropped those rows on read, by
// construction -- but fwriteDirectory() rebuilds the directory from the `indexes` hashmap alone,
// which those rows were never put into, so any save that rewrote the directory silently destroyed
// every sign in the world. (Pre-existing bug, independent of B5; see
// WORKING/newformat256z-sign-trailer-2026-08-24.md for the trace and the byte layout.)
// Fix: capture the CONTIGUOUS RUN of gate-failing rows at the END of the directory verbatim and
// re-emit it after the real entries on every rewrite. Rows that fail the gate *interior* to the
// real entries are still dropped, exactly as before -- those are corruption, not a trailer.
// These bytes are the file's copy. The game's copy is `worldTrailer` below (Stage D / D.3a), parsed
// from them at load; saveWorld() swaps the model's bytes in only when the model is dirty, so an
// untouched trailer is still re-emitted exactly as read (sign records hold world block coordinates,
// never file offsets, so relocating columns during a rewrite cannot invalidate them).
static unsigned char* dir_trailer=NULL;
static unsigned long long dir_trailer_len=0;
// Cap so a wholly-corrupt directory (every row failing the gate) can't be buffered in full. 1 MiB
// is a multiple of sizeof(ColumnIndex), so it can never split a row, and holds ~6,550 signs (a
// 120-byte record is 10 rows: 12 payload bytes a row). The sibling world editor caps its
// equivalent at 64 KiB; ours being larger only means we preserve more, never less. WorldTrailer::
// MAX_BYTES and the `.emod` SIGN_TRAILER limit are the same number.
#define DIR_TRAILER_MAX (1024*1024)
static_assert(DIR_TRAILER_MAX==WorldTrailer::MAX_BYTES,"one trailer cap on both containers");
// Stage D / D.3a: the open world's trailer as a model (signs, command blocks). See WorldTrailer.h and
// FileManager::trailer(). Loaded by loadWorld()/loadEmodWorld(), written by saveWorld() when dirty.
static WorldTrailer worldTrailer;
// Sidecars loadTrailer() adopted and no save has written into the world yet; see trailerSaved().
static std::vector<std::string> adoptedSidecars;
// A save that carried the model's bytes has landed: the model is clean, and an adopted sidecar is
// renamed `<name>.adopted`, so deleting every sign later cannot bring the sidecar's back on reload.
static void trailerSaved(){
    worldTrailer.markSaved();
    for(size_t i=0;i<adoptedSidecars.size();i++)
        emod::io::replace_file(adoptedSidecars[i],adoptedSidecars[i]+".adopted");
    adoptedSidecars.clear();
}

const int defaultRegionSkyColors[4][4]={
     {COLOR_BWG1,COLOR_BLUE1,COLOR_GREEN1,COLOR_RED1},
    {COLOR_ORANGE2,COLOR_NORMAL_BLUE,COLOR_NORMAL_BLUE,COLOR_NORMAL_BLUE},
    {COLOR_NORMAL_BLUE,COLOR_NORMAL_BLUE,COLOR_NORMAL_BLUE,COLOR_ORANGE1},
    {COLOR_PURPLE1,COLOR_NORMAL_BLUE,COLOR_NORMAL_BLUE,COLOR_RED5}};
int regionSkyColors[4][4]={
    {COLOR_BWG1,COLOR_BLUE1,COLOR_GREEN1,COLOR_RED1},
    {COLOR_ORANGE2,COLOR_NORMAL_BLUE,COLOR_NORMAL_BLUE,COLOR_NORMAL_BLUE},
    {COLOR_NORMAL_BLUE,COLOR_NORMAL_BLUE,COLOR_NORMAL_BLUE,COLOR_ORANGE1},
    {COLOR_PURPLE1,COLOR_NORMAL_BLUE,COLOR_NORMAL_BLUE,COLOR_RED5}};

// Sized for the maximum (400 slots, the measured New Dawn creature block); the live count is
// the runtime MAX_CREATURES_SAVED, derived per file. Model.mm externs this array.
EntityData creatureData[MAX_CREATURES_SAVED_MAX];
FileManager::FileManager(){
	single=this;
    genflat=false;
    worldUnbounded=false;
    sessionWorld=false;
    imgHash.clear();
    convertingWorld=false;
	// The save directory is the platform seam's (~/Library/Application Support/Emod, or --docs=); see docs/save-load.md.
	documents = eden_platform_documents_root();
    docs=documents;
    printg("!!!!!! %s\n",documents.c_str());
	oldOffsetX=oldOffsetZ=chunkOffsetX=chunkOffsetZ=-1;
	indexes=hashmap_new();
    indexes_hmm=indexes;
    shortSpans=hashmap_new();

    fmh_init(this);
	
	
}
bool FileManager::worldExists(std::string na,bool appendArchive){
    
   
    
    std::string file_name=docs + "/" +na;
    
    FILE* f;
    // "rb", not "r" — Phase N Stage 3. Existence is all this asks, so the mode is cosmetic here,
    // but every fopen in this file names a .eden save and the Windows CRT's text mode is not a
    // no-op on one (see getName below, where it was not cosmetic at all).
    if((f=fopen(file_name.c_str(),"rb"))){
        fclose(f);
        return true;
    }
    return false;

	//NSFileManager* fm=[NSFileManager defaultManager];
	//if(![fm fileExistsAtPath:file_name]){
	//	NSLog(@"%@ doesn't exist",file_name);
	//	return FALSE;
	//}else{
	//	NSLog(@"%@ exists",file_name);
	//	return TRUE;
	//}
}
static int count=0;

bool FileManager::deleteWorld(const std::string& nameIn){
    const std::string& name=nameIn;
    if(isFolderWorld(name)){   // K.0w: the whole folder is the world (rule 4)
        this->closeWorld();
        return removeWorldDir(worldDirOf(name));
    }
    if(isEmodName(nameIn)){
        // The open store holds the file; Windows will not delete an open one. Its temp names go too.
        this->closeWorld();
        std::string base=documents+"/"+name;
        // A C array, not an @[...] literal: web's Foundation shim has no arrayWithObjects:count:.
        static const char* const kTemps[]={".converting",".spill",".creating",".compact"};
        for(int i=0;i<4;i++)
            eden_fs_remove(base+kTemps[i]);
    }
    std::string img_name=documents+"/"+name+".png";
    if(eden_fs_exists(img_name)){
        eden_fs_remove(img_name);
    }
   // removeFromIndex(name);
	std::string file_name=documents+"/"+name;
	// B5: a leftover rollback journal must not outlive the world it belongs to -- a new world
	// created under the same file name would otherwise be "recovered" back into that stale tail
	// on its first load. Same reason the scratch/backup slots go here.
	eden_fs_remove(file_name+".savejrnl");
	
	
	if(eden_fs_exists(file_name)){
		if(eden_fs_remove(file_name))
			return true;
		else 
			return false;

	}
	return false;
	
	
}
bool FileManager::saveWorld(){
    return this->saveWorld(World::getWorld->player->pos);
    
    
}
void FileManager::compressLastPlayed(){
  //  NSString* name=World::getWorld->terrain.world_name;
	//NSString* file_name=[NSString stringWithFormat:@"%@/%@",documents,name];
   // CompressWorld([name cStringUsingEncoding:NSUTF8StringEncoding]);
    
}
void FileManager::loadGenFromDisk(){
#if 0   // G.2b-4: dead (only World's JUST_TERRAIN_GEN, which is 0, calls it); the stock UIImage/CoreGraphics body is kept as archaeology
    NSString *path =@"test.png";
    
   
    
    if (path != nil)
    {
        //UIImage *image = [UIImage imageNamed:path];
        
        if(![path isAbsolutePath])
            path = [[NSBundle mainBundle] pathForResource:path ofType:nil];
       
        UIImage *image = [UIImage imageWithContentsOfFile:path];
       
        if(image != NULL)printg("loaded image\n");
        
       
        
        // First get the image into your data buffer
        CGImageRef imageRef = [image CGImage];
        NSUInteger width = CGImageGetWidth(imageRef);
        NSUInteger height = CGImageGetHeight(imageRef);
        CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
        unsigned char *rawData = (unsigned char*) calloc(height * width * 4, sizeof(unsigned char));
        NSUInteger bytesPerPixel = 4;
        NSUInteger bytesPerRow = bytesPerPixel * width;
        NSUInteger bitsPerComponent = 8;
        CGContextRef context = CGBitmapContextCreate(rawData, width, height,
                                                     bitsPerComponent, bytesPerRow, colorSpace,
                                                     kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
        CGColorSpaceRelease(colorSpace);
        
        CGContextDrawImage(context, CGRectMake(0, 0, width, height), imageRef);
        CGContextRelease(context);
       
        extern sbyte* biomez;
        biomez=(sbyte*)malloc(height*width*sizeof(sbyte));
        memset(biomez,0,height*width*sizeof(sbyte));
        /*
        int xx=0;
        int yy=0;
        int count=width*height;
        
        // Now your rawData contains the image data in the RGBA8888 pixel format.
        int x=0;
        int y=0;
        int byteIndex = (bytesPerRow * yy) + xx * bytesPerPixel;
        //BOOL onetime=false;
        //int waterc=0;
        //int landc=0;

    int marker_colors[NUM_TERRAIN_MARKERS][3]={
            [TM_WATER]={0,0,255},
            [TM_GRASS]={0,255,0},
            [TM_BEACH]={255,255,0},
            [TM_MOUNTAINS]={255,255,255},
            [TM_MARS]={255,0,0},
            [TM_RIVERS]={0,255,255},
            [TM_UNICORN]={255,0,255}
            
        };
        for (int ii = 0 ; ii < count ; ++ii)
        {
            int red   = (rawData[byteIndex]     * 1.0);
            int green = (rawData[byteIndex + 1] * 1.0);
            int blue  = (rawData[byteIndex + 2] * 1.0);
           // int alpha = (rawData[byteIndex + 3] * 1.0);
            byteIndex += 4;
            x++;
            if(x==width){
                x=0;
                y++;
            }
            for(int i=0;i<NUM_TERRAIN_MARKERS;i++){
                if(red==marker_colors[i][0]&&green==marker_colors[i][1]&&blue==marker_colors[i][2]){
                    TM(x,y)=i;
                    break;
                }
            }
            
        }*/
        //printg("landc:%d waterc:%d total:%d\n",landc,waterc, count);
        free(rawData);
        
       
    
    }
#endif
    
    
}
void FileManager::writeGenToDisk(){
    printg("writing gen to disk\n");
    std::string name="Eden.eden";
	std::string file_name=documents+"/"+name;
    
    sfh=(WorldFileHeader*)malloc(sizeof(WorldFileHeader));
    // Zero it: see saveWorld's copy of this note. Only some of the 192 bytes are assigned below,
    // and the rest go to disk as-is.
    memset(sfh,0,sizeof(WorldFileHeader));
    
	if(eden_fs_exists(file_name)){
       

        bool success = eden_fs_remove(file_name);
        if(success){
            printg("removed existing world file\n");
        }else
            printg("error removing world file\n");
		
        
	}
	sfh->directory_offset=sizeof(WorldFileHeader)+sizeof(EntityData)*MAX_CREATURES_SAVED;;
    
	sfh->level_seed=0;
    
    int centerChunk=4096;
    int r=GSIZE/CHUNK_SIZE/2;
    Vector temp;
    temp.x=centerChunk*CHUNK_SIZE+CHUNK_SIZE/2;
    temp.z=centerChunk*CHUNK_SIZE+CHUNK_SIZE/2;
    temp.y=T_HEIGHT-10;
    sfh->home=temp;
    Vector temp2;
    temp2.x=BLOCK_SIZE*(sfh->home.x+.5f);
    temp2.y=BLOCK_SIZE*(sfh->home.y+1);
    temp2.z=BLOCK_SIZE*(sfh->home.z+.5f);
    sfh->pos=temp2;
    
    
	//sfh->home=MakeVector(5000,50,5000);
	//sfh->pos=MakeVector(5000,50,5000);
    
	sfh->yaw=90;
    sfh->version=FILE_VERSION;
    
    for(int i=0;i<4;i++){
        for(int j=0;j<4;j++){
            regionSkyColors[i][j]=defaultRegionSkyColors[i][j];
        }
    }
    
    for(int i=0;i<4;i++){
        for(int j=0;j<4;j++){
            sfh->skycolors[i*4+j]=regionSkyColors[i][j];
        }
    }
    
   
	strcpy(sfh->name,"Eden");
    
    // G.2b-4: createFileAtPath:contents: (a file holding just the header), then a handle for updating.
    fh_close(saveFile);
    eden_fs_create_empty(file_name);
    saveFile=EdenFile::openUpdate(file_name);
    fh_write(saveFile,sfh,sizeof(WorldFileHeader));
    
    
	//////////////////////////
	count=0;
	this->readDirectory();
	
    
    
    
 
    
 
    if(!NOBLOCKGEN)
    for(int x=0;x<GEN_CWIDTH;x++){
        for(int z=0;z<GEN_CDEPTH;z++){
            this->saveGenColumn(x+centerChunk-r,z+centerChunk-r,centerChunk-r);
        }
    }
    
    
	
	//[self saveCreatures];
    
   
	fh_seek(saveFile,0);
    fh_write(saveFile,sfh,sizeof(WorldFileHeader));
	if(writeDirectory){
		
		
		count=0;
		this->fwriteDirectory();
		
	}
	
	this->readDirectory();
	free(sfh);
	fh_close(saveFile);
    printg("finished writing gen to disk\n");

}

// (Stage S / S.8, 2026-10-11: the B5 rollback journal, the whole-file scratch copy and every other `.eden` save
// path are gone -- an `.emod` save is an append + commit record, see saveEmodWorld.)
bool FileManager::saveWorld(Vector warp){
    F3Scope _f3save(&g_f3.save_ms); g_f3.save_calls++;
	World::getWorld->terrain->endDynamics(true);
	//[World::getWorld->terrain updateAllImportantChunks];
	writeDirectory=false;
	Terrain* ter=World::getWorld->terrain;
	const std::string& name=ter->world_name;
	// MP.3a: a session commits to RAM (engine plan §3.2); nothing reaches disk.
	if(sessionWorld){
		this->commitSession(warp);
		Hotbar::get().saveIfDirty();   // UX.8-3: no folder, so prefs' hotbar.last only
		return true;
	}
	std::string file_name=documents+"/"+name;
	// Stage S / S.4: an `.emod` world appends a committed batch instead of rewriting a file -- no
	// scratch copy, journal, backup slot or threshold apply (docs/save-load.md, "`.emod` worlds").
	if(isEmodName(ter->world_name)){
		const bool ok=this->saveEmodWorld(warp);
		Hotbar::get().saveIfDirty();   // UX.8-3: after the save, so a new world's folder exists
		return ok;
	}
	// S.8: nothing but an `.emod` is saved. A world that is not one never gets here (loadWorld refuses it).
	return this->saveFailed(file_name+" is not an .emod");
}
float g_seconds_since_save=0;
float g_autosave_seconds=60;
int g_save_failures=0;
int g_save_test_ignore=0;

// QA.5 (A2-01): a failed save used to be a printg, which is `{}` in Release, and Save & Quit then tore
// the world down anyway. Now it is a stderr line (the first of a run of failures, then every 30th, so a
// stream retrying against a full disk does not flood the log) and the HUD's status line.
bool FileManager::saveFailed(const std::string& why){
    g_save_failures++;
    saveFailing_=true;
    lastSaveError_=why;
    saveFailedAtMs_=eden_platform_now_ms();
    if(saveFailStreak_++%30==0)
        fprintf(stderr,"emod: save failed: %s (%d in a row; the world stays open, nothing committed is lost)\n",
                why.c_str(),saveFailStreak_);
    World* w=World::getWorld;
    if(w&&w->hud&&w->hud->sb){
        // The store says "disk full?" for every write error; the other reasons are bugs or a damaged
        // file and are on stderr in full.
        bool full=why.find("disk full")!=std::string::npos;
        w->hud->sb->setStatus(full?"Could not save: the device may be full. Free some space, then save again."
                                  :"Could not save this world: "+why,6);
    }
    return false;
}
void FileManager::saveCommitted(){
    if(saveFailing_)fprintf(stderr,"emod: save committed again after %d failure(s)\n",saveFailStreak_);
    saveFailing_=false;
    saveFailStreak_=0;
    lastSaveError_.clear();
}
bool FileManager::saveHeld() const{
    return saveFailing_&&eden_platform_now_ms()-saveFailedAtMs_<SAVE_RETRY_HOLD_MS;
}
// B.5b-1b. Stock had no save that left dynamics running: every save -- including the one in front
// of each walk reload, i.e. every ~128 blocks walked -- put out every fire and stopped every flow.
// Under ring streaming a column leaves the window every few seconds of walking, so the evict-save
// would do that all the time. Nothing in the save needs dynamics stopped: a burning block is saved
// as the block it still is, a flowing one as the liquid it already is. What made stopping them
// load-bearing in stock was the toroidal aliasing after a reload, and the ring handles that per
// column instead (Terrain::dropDynamicsInColumn, before the column is overwritten).
bool FileManager::saveWorldKeepDynamics(){
    F3Scope _f3save(&g_f3.save_ms); g_f3.save_calls++;
	writeDirectory=false;
	const std::string& name=World::getWorld->terrain->world_name;
	if(sessionWorld){   // MP.3a: the evict-save and the autosave, to RAM
		if(!MpWorld::testNoEvictCommit)this->commitSession(World::getWorld->player->pos);
		return true;
	}
	if(saveHeld())return false;   // QA.5: the last one failed moments ago
	if(isEmodName(World::getWorld->terrain->world_name)){
		return this->saveEmodWorld(World::getWorld->player->pos);
	}
	return this->saveFailed(name+" is not an .emod");
}
// Rows actually WRITTEN by the pass below -- `count` is incremented for every row considered,
// including the ones the offset check rejects, so it cannot be used to find the directory's end.
static int dir_rows_written=0;
// X.6: the bundled map is the square of chunk columns [4096-GSIZE/CHUNK_SIZE/2, 4096+GSIZE/CHUNK_SIZE/2)
// (blocks [4096*CHUNK_SIZE-GSIZE/2, +GSIZE)). A world holding a column, `pos` or `home` outside it was
// made by a post-2.1 client that has no edge wall, so Player::update's clamp must not apply to it.
static bool fm_columnOutsideSquare(int cx,int cz){
	int lo=4096-GSIZE/CHUNK_SIZE/2,hi=4096+GSIZE/CHUNK_SIZE/2;
	return cx<lo||cx>=hi||cz<lo||cz>=hi;
}
static bool fm_blockOutsideSquare(float x,float z){
	float lo=4096*CHUNK_SIZE-GSIZE/2,hi=4096*CHUNK_SIZE+GSIZE/2;
	return x<lo||x>hi||z<lo||z>hi;
}
static int fm_scanOutside(any_t passedIn,any_t col){
	ColumnIndex* ci=(ColumnIndex*)col;
	if(ci&&fm_columnOutsideSquare(ci->x,ci->z))*(bool*)passedIn=true;
	return MAP_OK;
}
// QA.2 (A3-02): a header's floats come from a file anyone can hand us (a corrupt legacy save, a crafted
// `.eden`, a hand-edited `.emod`). A NaN or huge position reaches `(int)floorf` (UB) and a window origin
// that wraps `chunkOffset+2*r`. Clamped at read time, on the in-memory copy only: the 192 raw bytes on
// disk are never rewritten here (rule 5), a bad value is replaced in whatever normal commit follows.
// Same ceiling as the console's `=N` coordinates (CmdScript `coord`).
static bool fm_finiteWithin(float v){return std::isfinite(v)&&fabsf(v)<=10000000.0f;}
static bool fm_validPos(const Vector& v){return fm_finiteWithin(v.x)&&fm_finiteWithin(v.y)&&fm_finiteWithin(v.z);}
static void fm_sanitizeHeader(WorldFileHeader* h){
	if(!fm_validPos(h->home)){
		fprintf(stderr,"[eden] header: home is not finite/in range, using the default\n");   // not printg: silent in Release, and the QA.2 gate greps it
		h->home.x=4096*CHUNK_SIZE+CHUNK_SIZE/2;   // the default map's centre (generateDefaultWorld)
		h->home.z=4096*CHUNK_SIZE+CHUNK_SIZE/2;
		h->home.y=T_HEIGHT-10;
	}
	if(!fm_validPos(h->pos)){
		fprintf(stderr,"[eden] header: spawn is not finite/in range, using home\n");
		h->pos.x=BLOCK_SIZE*(h->home.x+.5f);
		h->pos.y=BLOCK_SIZE*(h->home.y+1);
		h->pos.z=BLOCK_SIZE*(h->home.z+.5f);
	}
	if(!std::isfinite(h->yaw))h->yaw=0;
}
static bool fm_headerOutside(WorldFileHeader* h){
	return fm_blockOutsideSquare(h->pos.x,h->pos.z)||fm_blockOutsideSquare(h->home.x,h->home.z);
}

int saveColIdx(any_t passedIn,any_t colToSave){
	count++;
	ColumnIndex* colIndex=(ColumnIndex*)colToSave;
	if(colIndex&&colIndex->chunk_offset<sfh->directory_offset){
		int n=twoToOne(colIndex->x, colIndex->z);
		if(n==0){
		//	NSLog(@"corrupted col:%d",colIndex->chunk_offset);
		}
		
	fh_write(saveFile,colIndex,sizeof(ColumnIndex));
	dir_rows_written++;
	}else{
		printg("WTF MATE\n");
	}
	return MAP_OK;
}
void FileManager::fwriteDirectory(){
	fh_seek(saveFile,sfh->directory_offset);
	dir_rows_written=0;
	hashmap_iterate(indexes, saveColIdx, NULL);
	// Re-emit whatever readDirectory captured past the real entries (sign records on a
	// NewFormat256z world; nothing at all on every other world) -- see dir_trailer's comment.
	if(dir_trailer&&dir_trailer_len){
		fh_write(saveFile,dir_trailer,(size_t)dir_trailer_len);
		printg("directory: re-emitted %llu B sign trailer after %d column rows\n",
			dir_trailer_len,dir_rows_written);
	}
	// The directory is read TO EOF, so any byte left past what we just wrote is parsed as another
	// directory row next time. Nothing shrinks the directory today, but the in-place save path
	// (B5) writes into the real file rather than into a fresh scratch copy, where a stale tail is
	// no longer impossible -- make the file end exactly where the directory does.
	fh_truncate(saveFile,sfh->directory_offset
		+(unsigned long long)dir_rows_written*sizeof(ColumnIndex)+dir_trailer_len);
}
void FileManager::readDirectory(){
	this->clearDirectory();
	if(dir_trailer){free(dir_trailer);dir_trailer=NULL;}
	dir_trailer_len=0;
	// The run of gate-failing rows seen since the last row that PASSED the gate. It only becomes
	// the trailer if the file ends while it is still open; a later valid row proves it was
	// interior garbage and resets it (dropped, as before).
	unsigned char* pending=NULL;
	unsigned long long pending_len=0;
	bool pending_over=false;
	fh_seek(saveFile,sfh->directory_offset);
	while(true){
		ColumnIndex* colIdx=(ColumnIndex*)malloc(sizeof(ColumnIndex));
		if(fh_read(saveFile,colIdx,sizeof(ColumnIndex))<sizeof(ColumnIndex)){free(colIdx);break;}
		count++;
		int n=twoToOne(colIdx->x, colIdx->z);
		if(n!=0){
		pending_len=0; pending_over=false;
		hashmap_put(indexes,n, (any_t)colIdx);
           // printg("reading dir\n");
        }else {
			if(!pending_over){
				if(pending_len+sizeof(ColumnIndex)<=DIR_TRAILER_MAX){
					if(!pending)pending=(unsigned char*)malloc(DIR_TRAILER_MAX);
					if(pending){
						memcpy(pending+pending_len,colIdx,sizeof(ColumnIndex));
						pending_len+=sizeof(ColumnIndex);
					}else pending_over=true;
				}else pending_over=true;
			}
			free(colIdx);
		}



	}
	if(pending_over){
		printg("directory: trailing unaddressable rows exceed %d B -- NOT preserving them\n",DIR_TRAILER_MAX);
	}else if(pending_len){
		dir_trailer=(unsigned char*)malloc((size_t)pending_len);
		if(dir_trailer){
			memcpy(dir_trailer,pending,(size_t)pending_len);
			dir_trailer_len=pending_len;
			printg("directory: captured a %llu B post-directory trailer (signs) to re-emit on rewrite\n",dir_trailer_len);
		}
	}
	if(pending)free(pending);
    this->deriveColumnSpans();
}

// ---- 256z support: derive the creature-block size and the per-column spans FROM THE FILE ----
// Two facts about a .eden that the header does not state and that this engine used to take as
// compile-time constants:
//
//  * how many creature slots sit between the last column and the directory. The version implies
//    200 (v>=3) or 400 (the one measured New Dawn world), but the sibling world editor's own
//    worldgen writes v5 files with NO creature block at all, so the version is not trustworthy.
//    The gap is computable: directory_offset - (highest chunk_offset + one column record).
//  * how long each column record actually is. Normally SIZEOF_COLUMN, but the measured New Dawn
//    specimen contains one column of 107,072 B (= 131,072 - 24,000, i.e. one creature block short,
//    consistent with its writer appending a column one creature-block too early). Reading that
//    column at full stride silently pulls in the next column's bytes, so spans are derived from
//    the gap to the next-highest offset and short ones are recorded here.
//
// Both derivations are pure arithmetic over the directory we just read -- no extra file I/O.
static int cmp_offsets(const void* a,const void* b){
    unsigned long long x=*(const unsigned long long*)a, y=*(const unsigned long long*)b;
    return x<y?-1:(x>y?1:0);
}
struct SpanCollect{ unsigned long long* offsets; int n; };
static int collectOffset(any_t passedIn,any_t item){
    SpanCollect* sc=(SpanCollect*)passedIn;
    ColumnIndex* ci=(ColumnIndex*)item;
    sc->offsets[sc->n++]=ci->chunk_offset;
    return MAP_OK;
}
struct SpanAssign{ unsigned long long* offsets; int n; unsigned long long dirEnd; map_t out; };
static int assignSpan(any_t passedIn,any_t item){
    SpanAssign* sa=(SpanAssign*)passedIn;
    ColumnIndex* ci=(ColumnIndex*)item;
    // binary search for this column's offset, then span = next offset (or the end of the block-data
    // region) - this offset.
    int lo=0,hi=sa->n-1,at=-1;
    while(lo<=hi){
        int mid=(lo+hi)/2;
        if(sa->offsets[mid]==ci->chunk_offset){at=mid;break;}
        if(sa->offsets[mid]<ci->chunk_offset)lo=mid+1; else hi=mid-1;
    }
    if(at<0)return MAP_OK;
    unsigned long long next=(at+1<sa->n)?sa->offsets[at+1]:sa->dirEnd;
    if(next<=ci->chunk_offset)return MAP_OK;
    unsigned long long span=next-ci->chunk_offset;
    if(span<SIZEOF_COLUMN){
        int n=twoToOne(ci->x,ci->z);
        if(n!=0){
            unsigned long long* stored=(unsigned long long*)malloc(sizeof(unsigned long long));
            *stored=span;
            hashmap_put(sa->out,n,(any_t)stored);
            printg("short column record at (%d,%d): %llu B of %llu\n",ci->x,ci->z,span,SIZEOF_COLUMN);
        }
    }
    return MAP_OK;
}
void FileManager::clearColumnSpans(){
    hashmap_remove_all(shortSpans,true);
}
void FileManager::deriveColumnSpans(){
    this->clearColumnSpans();
    int n=hashmap_length(indexes);
    if(n<=0){
        // Empty directory: nothing to derive from, fall back to what the height implies. (By
        // height, not version, since S.3b -- the two agree for every world that can get here: a
        // new world, which is stamped 2 or 5 to match.)
        eden_set_creature_slots((g_world_height>=T_HEIGHT_MAX?400:200));
        creature_block_on_disk=(sfh->version>=3);
        return;
    }
    unsigned long long* offsets=(unsigned long long*)malloc(sizeof(unsigned long long)*n);
    SpanCollect sc={offsets,0};
    hashmap_iterate(indexes,collectOffset,&sc);
    qsort(offsets,sc.n,sizeof(unsigned long long),cmp_offsets);

    // Creature block = whatever is left between the end of the last column and the directory.
    unsigned long long lastEnd=offsets[sc.n-1]+SIZEOF_COLUMN;
    int slots=(g_world_height>=T_HEIGHT_MAX?400:200);
    // S.3b: a v1/v2 file has no creature block yet -- unless the gap says it does (a 2026-game v2
    // 256z world carries 400 slots). Every v3+ file has one, as before.
    creature_block_on_disk=(sfh->version>=3);
    if(sfh->directory_offset>=lastEnd){
        unsigned long long gap=sfh->directory_offset-lastEnd;
        if(gap%sizeof(EntityData)==0&&gap/sizeof(EntityData)<=MAX_CREATURES_SAVED_MAX){
            slots=(int)(gap/sizeof(EntityData));
            if(gap>0)creature_block_on_disk=true;
        }else{
            printg("creature-block gap %llu B is not a whole number of %d-byte slots -- assuming %d\n",
                   gap,(int)sizeof(EntityData),slots);
        }
    }
    eden_set_creature_slots(slots);

    SpanAssign sa={offsets,sc.n,sfh->directory_offset-(unsigned long long)sizeof(EntityData)*slots,shortSpans};
    hashmap_iterate(indexes,assignSpan,&sa);
    free(offsets);
    printg("directory: %d columns, %d creature slots, %d short spans\n",
           sc.n,slots,hashmap_length(shortSpans));
}
void FileManager::clearDirectory(){
	hashmap_remove_all(indexes,true);
	//NSLog(@"hash %d",hashmap_length(indexes));
}	
	/*
 – offsetInFile
 – seekToEndOfFile
 – seekToFileOffset:
 – availableData
 – readDataToEndOfFile
 – readDataOfLength:
 – writeData:
 */

/*-(void)saveGenColumn:(int)cx:(int)cz:(int)origin{  // NO RUN LENGTH ENCODING VERSION
    
	
	ColumnIndex* colIndex=NULL;
	

	
    colIndex=malloc(sizeof(ColumnIndex));
    colIndex->chunk_offset=sfh->directory_offset-sizeof(EntityData)*MAX_CREATURES_SAVED;
    sfh->directory_offset+=SIZEOF_COLUMN;
    writeDirectory=TRUE;
    colIndex->x=cx;
    colIndex->z=cz;
    int n=twoToOneTest(cx,cz);
	hashmap_put(indexes, n, colIndex);
        
	
	[saveFile seekToFileOffset:colIndex->chunk_offset];
    block8 blocks[CHUNK_SIZE*CHUNK_SIZE*CHUNK_SIZE];
    block8 colors[CHUNK_SIZE*CHUNK_SIZE*CHUNK_SIZE];
	
    extern block8* blockz;
    extern color8* colorz;
    
    int xoffset=CHUNK_SIZE*(cx-origin);
    int zoffset=CHUNK_SIZE*(cz-origin);
   
    for(int cy=0;cy<CHUNKS_PER_COLUMN ;cy++){
        int yoffset=cy*CHUNK_SIZE;
        
            for(int x=0;x<CHUNK_SIZE;x++){
            for(int y=0;y<CHUNK_SIZE;y++){
            for(int z=0;z<CHUNK_SIZE;z++){
                blocks[x*CHUNK_SIZE*CHUNK_SIZE+z*CHUNK_SIZE+y]=BLOCK(x+xoffset,z+zoffset,y+yoffset);
                colors[x*CHUNK_SIZE*CHUNK_SIZE+z*CHUNK_SIZE+y]=COLOR(x+xoffset,z+zoffset,y+yoffset);
            }}}
      
        
			NSData* data=[NSData dataWithBytesNoCopy:blocks
											  length:(CHUNK_SIZE3*sizeof(block8))
										freeWhenDone:FALSE];
			[saveFile writeData:data];
            data=[NSData dataWithBytesNoCopy:colors
                                      length:(CHUNK_SIZE*CHUNK_SIZE*CHUNK_SIZE*sizeof(color8))
                                freeWhenDone:FALSE];
			[saveFile writeData:data];
		
	}
	
}*/

void FileManager::saveGenColumn(int cx,int cz,int origin){
  
	
	ColumnIndex* colIndex=NULL;
	
    
	
    colIndex=(ColumnIndex*)malloc(sizeof(ColumnIndex));
    colIndex->chunk_offset=sfh->directory_offset-sizeof(EntityData)*MAX_CREATURES_SAVED;
    
    writeDirectory=true;
    colIndex->x=cx;
    colIndex->z=cz;
    int n=twoToOneTest(cx,cz);
	hashmap_put(indexes, n, colIndex);
    
	
	fh_seek(saveFile,colIndex->chunk_offset);
    block8 blocks[CHUNK_SIZE3];
    color8 colors[CHUNK_SIZE3];
	
    extern block8* blockz;
    extern color8* colorz;
    
    int xoffset=CHUNK_SIZE*(cx-origin);
    int zoffset=CHUNK_SIZE*(cz-origin);
 
    for(int cy=0;cy<CHUNKS_PER_COLUMN ;cy++){
        int yoffset=cy*CHUNK_SIZE;
        
        for(int x=0;x<CHUNK_SIZE;x++){
            for(int y=0;y<CHUNK_SIZE;y++){
                for(int z=0;z<CHUNK_SIZE;z++){
                    //unusual coordinate order to maximize compression
                   /* blocks[y*CHUNK_SIZE*CHUNK_SIZE+x*CHUNK_SIZE+z]=BLOCK(x+xoffset,z+zoffset,y+yoffset);
                    colors[y*CHUNK_SIZE*CHUNK_SIZE+x*CHUNK_SIZE+z]=COLOR(x+xoffset,z+zoffset,y+yoffset);*/
                    
                    blocks[y*CHUNK_SIZE*CHUNK_SIZE+z*CHUNK_SIZE+x]=BLOCK(x+xoffset,z+zoffset,y+yoffset);
                    colors[y*CHUNK_SIZE*CHUNK_SIZE+z*CHUNK_SIZE+x]=COLOR(x+xoffset,z+zoffset,y+yoffset);
                    
                    
                
                }}}
        
       // memset(blocks,0,CHUNK_SIZE3);
       // memset(colors,0,CHUNK_SIZE3);
        
        /*int y=arc4random()%CHUNK_SIZE;
        for(int x=0;x<CHUNK_SIZE;x++){

                for(int z=0;z<CHUNK_SIZE;z++){
                    //if(y%2==0){
                    if(arc4random()%2==0){
                        blocks[x*CHUNK_SIZE*CHUNK_SIZE+z*CHUNK_SIZE+y]=TYPE_BRICK;
                        
                        colors[x*CHUNK_SIZE*CHUNK_SIZE+z*CHUNK_SIZE+y]=y%NUM_COLORS;
                    }
                        // }
                }
        }*/
       
        
        color8 rledata[CHUNK_SIZE3*3+2];
        int marker=-1;
        int marker_color=-1;
        int count =0;
        int dataidx=2;
        for(int i=0;i<CHUNK_SIZE3;i++){
            int t=blocks[i];
            int c=colors[i];
            if(t<0||c<0)printg("wtf mate");
            if(t==marker&&c==marker_color&&count!=127){
                count++;
                
            }else{
                if(count>0){
 
                   // printg("count: %d\n",count);
                    rledata[dataidx++]=marker;
                    rledata[dataidx++]=marker_color;
                    rledata[dataidx++]=count;
                    count=0;
                    marker=-1;
                    marker_color=-1;
                }
                marker_color=c;
                marker=t;
                count++;
                
                
            }
        }
        if(count>0){
            //printg("count: %d\n",count);
            rledata[dataidx++]=marker;
            rledata[dataidx++]=marker_color;
            rledata[dataidx++]=count;
            count=0;
            marker=-1;
            marker_color=-1;
        }
        
       
        if(dataidx>CHUNK_SIZE3*3){
            printg("dataidx overflow\n");
        }else{
            rledata[0]=dataidx/256;
            rledata[1]=dataidx%256;
            
            
           
            
            sfh->directory_offset+=dataidx;
        }
        
        fh_write(saveFile,rledata,dataidx*sizeof(color8));
               //[saveFile writeData:data];
		
	}
	
}
extern block8* blockarray;
extern int g_offcx;
extern int g_offcz;
// H.4: what Terrain's bulk reload charges by time (generator-plan.md §2.1): a column this world has
// not saved, on a Classic seed (not the flat world, not the bundled map).
bool FileManager::columnWillGenerate(int cx,int cz){
    Terrain* ter=World::getWorld->terrain;
    if(ter->tgen->LEVEL_SEED==DEFAULT_LEVEL_SEED||ter->tgen->LEVEL_SEED==0)return false;
    int n=twoToOne(cx,cz);
    if(n==0)return false;
    if(emodStore)return !emodStore->hasColumn(cx,cz);
    ColumnIndex* colIndex=NULL;
    hashmap_get(indexes,n,(any_t*)&colIndex);
    return colIndex==NULL;
}

void FileManager::readColumn(int cx,int cz,EdenFile* rcfile){
	Terrain* ter=World::getWorld->terrain;
	ColumnIndex* colIndex=NULL;
	int n= twoToOne(cx,cz);
	if(n==0){
		printg("mm\n");
		return;	
	}
    if(indexes_hmm!=indexes)printg("FATAL ERROR: indexes pointer corrupted!!!!\n");
	hashmap_get(indexes,n, (any_t*)&colIndex);
   
	if(emodStore?!emodStore->hasColumn(cx,cz):(colIndex==NULL)){
		
		Terrain* ter=World::getWorld->terrain;
     //   int cx2=cx-chunkOffsetX;
      //  int cz2=cz-chunkOffsetZ;
     //   if(rcfile==saveFile){
        //printg("loading column from gen %d,%d \n",cx,cz);
     if(ter->tgen->LEVEL_SEED==DEFAULT_LEVEL_SEED){
         fmh_readColumnFromDefault(cx,cz);
            
            return;
     }else{
         { F3Scope _f3(&g_f3.gen_ms); g_f3.cols_gen++;
         ter->tgen->generateColumn(cx,cz,false); }
      		return;
     }
	}
	//NSLog(@"reading col: %d, %d, %d",cx,cz,colIndex->chunk_offset);
		
	//cx-=chunkOffsetX;
	//cz-=chunkOffsetZ;
	
	double _f3rc0=f3_now(); double _f3rd=0; g_f3.cols_dir++;
	TerrainChunk* chunk=NULL;
	//int oldcx,oldcz;
	/*if(ter.oldChunkMap!=NULL){
		oldcx=cx+(chunkOffsetX-oldOffsetX);
		oldcz=cz+(chunkOffsetZ-oldOffsetZ);
		
		hashmap_get(ter.oldChunkMap, threeToOne(oldcx, 0, oldcz), (any_t)&chunk);
		
	}*/
	
	if(chunk!=NULL){
        
		printg("nononono123 abort!\n");
		/*for(int cy=0;cy<CHUNKS_PER_COLUMN ;cy++){
			//hashmap_get(ter.oldChunkMap, threeToOne(oldcx, cy, oldcz), (any_t)&chunk);
			[chunk retain];
			int bounds[6];			
			bounds[0]=cx*CHUNK_SIZE;
			bounds[1]=cy*CHUNK_SIZE;
			bounds[2]=cz*CHUNK_SIZE;
			bounds[3]=(cx+1)*CHUNK_SIZE;
			bounds[4]=(cy+1)*CHUNK_SIZE;
			bounds[5]=(cz+1)*CHUNK_SIZE;		
			[chunk setBounds:bounds];
            if(chunk.needsGen){
                //printg("adding background loaded chunk\n");
                [ter addChunk:chunk:cx:cy:cz:TRUE];
            }else
			[ter readdChunk:chunk:cx:cy:cz];	
			
			for(int x=0;x<CHUNK_SIZE;x++){
				for(int z=0;z<CHUNK_SIZE;z++){
                    memcpy(blockarray+((x+bounds[0])*T_SIZE*T_HEIGHT+(z+bounds[2])*T_HEIGHT+bounds[1]),
                           chunk.pblocks+(x*CHUNK_SIZE*CHUNK_SIZE+z*CHUNK_SIZE),
                           CHUNK_SIZE);
                    
                    
					
				}			
			}
            
		}*/
		
		
		
	}else{
      //  printg("loading column from file\n");
       /* if(saveFile==rcfile)
        printg("loading column from file\n");
        else 
            printg("attempting to load col from file for bgthread\n");
*/
		// Stage S / S.4: an `.emod` column arrives as ONE decoded buffer in the `.eden` intra-column
		// layout (per band: 4096 type bytes, then 4096 paint bytes), so the band loop below copies
		// from it instead of reading the file. A band the record does not store is zeros -- the
		// same air a short `.eden` span gets.
		// R.2 (plan §8): a band whose mask bit is clear is all zero, types AND paint, so it is
		// memset rather than copied; Terrain::addChunk then sets its empty bit from those zeros.
		// addChunk stays the bit's one writer on purpose (TerrainChunk.h's rule) -- seeding it here
		// too would save one ~0.1 us word scan per band and add a second writer to keep honest.
		uint16_t emodMask=0;
		if(emodStore){
			emod::EdenWorldStore::ReadResult rr=emodStore->readColumn(cx,cz,emodColBuf,&emodMask);
			if(rr==emod::EdenWorldStore::READ_FALLBACK){
				fprintf(stderr,"emod: column (%d,%d) damaged -- loaded its previous committed version\n",cx,cz);   // stderr: rare, and printg/NSLog compile out of Release
			}else if(rr!=emod::EdenWorldStore::READ_OK){
				// READ_AIR_DAMAGED (or a key that vanished): air, and NOT marked modified, so the
				// next save cannot overwrite the damaged record's earlier versions with it (plan §6).
				fprintf(stderr,"emod: column (%d,%d) damaged and has no earlier version -- loaded as air (not marked modified)\n",cx,cz);
				emodMask=0;
			}
			// QA.17: in play this is the main thread; during a load it may be the load thread, and
			// finishLoadOnMain shows the notice instead.
			if(rr!=emod::EdenWorldStore::READ_OK&&World::getWorld&&World::getWorld->doneLoading==0)fm_damageNotice();
		}else
		{ double _t=f3_now(); if(rcfile)rcfile->seek((long long)colIndex->chunk_offset); _f3rd+=f3_now()-_t; }   // G.2b-2: EdenFile; null = a nil handle (no-op)
        TerrainChunk* columns[CHUNKS_PER_COLUMN_MAX];
        // How many bands this record really holds. Normally all of them; a column recorded in
        // shortSpans is physically shorter than SIZEOF_COLUMN (see deriveColumnSpans) and the
        // bands past its end must be zero-filled rather than read, or we'd read the neighbour's.
        int bandsInFile=CHUNKS_PER_COLUMN;
        {
            unsigned long long* span=NULL;
            hashmap_get(shortSpans,n,(any_t*)&span);
            if(span!=NULL){
                bandsInFile=(int)(*span/(CHUNK_SIZE3*(sizeof(block8)+sizeof(color8))));
                if(bandsInFile<0)bandsInFile=0;
                if(bandsInFile>CHUNKS_PER_COLUMN)bandsInFile=CHUNKS_PER_COLUMN;
            }
        }
         for(int cy=0;cy<CHUNKS_PER_COLUMN ;cy++){
            int bounds[6];
            
            bounds[0]=cx*CHUNK_SIZE;
            bounds[1]=cy*CHUNK_SIZE;
            bounds[2]=cz*CHUNK_SIZE;
            bounds[3]=(cx+1)*CHUNK_SIZE;
            bounds[4]=(cy+1)*CHUNK_SIZE;
            bounds[5]=(cz+1)*CHUNK_SIZE;
            
            TerrainChunk* chunk=nullptr;
             //issue #3 continued
            TerrainChunk* old=ter->chunkTable[threeToOne(cx,cy,cz)];
            if(old){chunk=old;
                chunk->setBounds(bounds);

            }
            else{
                printf("crittcler error re-allocating terrain chunk\n");
          // chunk=new TerrainChunk(bounds,cx,cz,ter,TRUE);
            }
             
             
            columns[cy]=chunk;
            
           
             bool rle=false;
             if(rle){
                 
                 block8 tblocks[CHUNK_SIZE3];
                 color8 tcolors[CHUNK_SIZE3];
                 color8 buf[CHUNK_SIZE3*3];
                 //too much read
                 color8 buft[2]={0,0};
                 if(rcfile)rcfile->read(buft,2);
                 int chunk_data_length= buft[0]*256+buft[1]-2;
                 int n=(rcfile&&chunk_data_length>0)?(int)rcfile->read(buf,std::min((size_t)chunk_data_length,sizeof(buf))):0;
                 if(n<chunk_data_length){
                     printg("not enough file left, only read %d bytes\n",n);
                 }//else
                  //   printg("all good %d, %d  sizeofcolor8:%d\n",(int)n,(int)chunk_data_length,(int)sizeof(color8));
                 
                 int idx=0;
                 int idx2=0;
                 while(idx<n){
                     int marker=(block8)buf[idx++];
                     int marker_color=(color8)buf[idx++];
                     int count=(color8)buf[idx++];
                    // printg("count: %d\n",count);
                     if(count<0||count>127)printg("strange count %d\n ",count);
                     for(int i=0;i<count;i++){
                         if(idx2>CHUNK_SIZE3){
                            // printg("data overflow1 %d  n:%d\n",idx2,n);
                             break;
                         }
                         tblocks[idx2]=marker;
                         tcolors[idx2]=marker_color;
                         idx2++;
                        
                         
                     }
                     if(idx2>=CHUNK_SIZE3){
                         
                         break;
                         
                     }
                 }
                 if(idx2>CHUNK_SIZE3)putchar('>');
                 else if(idx2<CHUNK_SIZE3)putchar('<');
                 else if(idx2==CHUNK_SIZE3){
                   //  putchar('=');
                     for(int z=0;z<CHUNK_SIZE;z++)
                     for(int x=0;x<CHUNK_SIZE;x++)
                         for(int y=0;y<CHUNK_SIZE;y++){
                             chunk->pblocks[CC(x,z,y)]=tblocks[CC(y,z,x)];
                             chunk->pcolors[CC(x,z,y)]=tcolors[CC(y,z,x)];
                         }
                             
                     
                 }
                
                 
             }else if(emodStore){
                 if(emodMask&(1u<<cy)){
                     memcpy(chunk->pblocks,&emodColBuf[(size_t)cy*emod::BAND],CHUNK_SIZE3*sizeof(block8));
                     memcpy(chunk->pcolors,&emodColBuf[(size_t)cy*emod::BAND+CHUNK_SIZE3*sizeof(block8)],CHUNK_SIZE3*sizeof(color8));
                 }else{
                     memset(chunk->pblocks,0,CHUNK_SIZE3*sizeof(block8));
                     memset(chunk->pcolors,0,CHUNK_SIZE3*sizeof(color8));
                 }
             }else if(cy<bandsInFile){
                 double _t=f3_now();
                 // G.2b-2: straight into the slot, as readDataOfLength + getBytes did: a short read
                 // leaves the rest of the slot as it was, and a null handle (nil before) reads nothing.
                 if(rcfile)rcfile->read(chunk->pblocks,CHUNK_SIZE*CHUNK_SIZE*CHUNK_SIZE*sizeof(block8));
                 if(rcfile)rcfile->read(chunk->pcolors,CHUNK_SIZE*CHUNK_SIZE*CHUNK_SIZE*sizeof(color8));
                 _f3rd+=f3_now()-_t; g_f3.rd_bytes+=2*CHUNK_SIZE*CHUNK_SIZE*CHUNK_SIZE;
             }else{
                 // Band the file does not contain (short record) -- air, not a neighbour's bytes.
                 memset(chunk->pblocks,0,CHUNK_SIZE3*sizeof(block8));
                 memset(chunk->pcolors,0,CHUNK_SIZE3*sizeof(color8));
             }
            
           /*
            chunk.needsGen=TRUE;*/
            
           
                for(int x=0;x<CHUNK_SIZE;x++){
                    for(int z=0;z<CHUNK_SIZE;z++){
                        if((x+bounds[0]+g_offcx)<0||(z+bounds[0]+g_offcz)<0){
                            printg("over/underflowing...\n");
                        }
                        memcpy(
                               
                blockarray+
            ((x+bounds[0]+g_offcx)%T_SIZE)*T_SIZE*T_HEIGHT+
                ((z+bounds[2]+g_offcz)%T_SIZE)*T_HEIGHT+bounds[1],
                               chunk->pblocks+(x*CHUNK_SIZE*CHUNK_SIZE+z*CHUNK_SIZE),
                               CHUNK_SIZE);
                        
                    }			
                }
                
                  
                ter->addChunk(chunk,cx,cy,cz,true);
             
        }
        
	}
    
    g_f3.rd_ms+=_f3rd; g_f3.pub_ms+=(f3_now()-_f3rc0)-_f3rd;
	
}
std::string fullPathForFilename(const char* fn){
    return eden_bundle_path(fn);   // G.2b-4: "" on a miss (Apple's nil; cpstring(nil) read garbage)
    //return docs+"/"+std::string(fn);
}
void FileManager::setName(std::string fn,std::string dn){
    
   
   //file_name=[file_name stringByDeletingPathExtension];
   // NSLog(@"set name request on:%@",file_name);
   // NSString* nofp=file_name;
    
    // Stage S / S.4: an `.emod` keeps its name in the WORLD_HEADER record -- append a new one.
    if(fn.size()>5&&fn.compare(fn.size()-5,5,".emod")==0){
        if(!emodSetHeaderName(docs+"/"+fn,dn))printg("file to rename not found\n");
        return;
    }
    fn=docs+"/"+fn;
    FILE* f;
    if(!(f=fopen(fn.c_str(),"rw"))){
        printg("file to rename not found\n");
        return;
    }
    WorldFileHeader* fh2=(WorldFileHeader*)malloc(sizeof(WorldFileHeader));
    fread(fh2,sizeof(WorldFileHeader),1,f);
    fseek(f,0,SEEK_SET);
    strncpy(fh2->name,dn.c_str(),49);
    fwrite(fh2,sizeof(WorldFileHeader),1,f);
    
    free(fh2);
    fclose(f);
   // file_name=[NSString stringWithFormat:@"%@/%@",documents,file_name];
   // DecompressWorld([file_name cStringUsingEncoding:NSUTF8StringEncoding]);
  
	
	
/*	saveFile=[NSFileHandle fileHandleForUpdatingAtPath:file_name];
    if(saveFile==NULL){
        
    }
	WorldFileHeader* fh=(WorldFileHeader*)[[saveFile readDataOfLength:sizeof(WorldFileHeader)] bytes];
	WorldFileHeader* fh2=(WorldFileHeader*)malloc(sizeof(WorldFileHeader));
	memcpy(fh2,fh,sizeof(WorldFileHeader));
	[display_name getCString:fh2->name
								 maxLength:49
								  encoding:NSUTF8StringEncoding];
	NSData* dh=[NSData dataWithBytesNoCopy:fh2 length:sizeof(WorldFileHeader) freeWhenDone:TRUE];
	[saveFile seekToFileOffset:0];
	[saveFile writeData:dh];
	
	[saveFile closeFile];
	 
    */
   // CompressWorld([nofp cStringUsingEncoding:NSUTF8StringEncoding]);
	
}
void FileManager::setImageHash(const std::string& hashIn){
    const std::string& name=World::getWorld->terrain->world_name;
	std::string file_name=documents+"/"+name;
    imgHash=fm_ns(hashIn);   // G.2b-4: was a retained NSString (nil, so "", for bytes with no UTF-8 form)
   
    if(isEmodName(name)){
        // The hash lives in the WORLD_HEADER record: append a new one in its own committed batch.
        emod::EdenWorldStore own;
        emod::EdenWorldStore* st=emodStore;
        if(!st){
            if(!own.open(file_name.c_str(),true))return;
            st=&own;
        }
        std::vector<uint8_t> raw;
        if(st->readRecord(emod::T_WORLD_HEADER,raw)>emod::EdenWorldStore::READ_FALLBACK||raw.size()!=sizeof(WorldFileHeader))return;
        WorldFileHeader h; memcpy(&h,&raw[0],sizeof(h));
        memset(h.hash,0,sizeof(h.hash));
        eden_cstr_copy(h.hash,33,imgHash);   // getCString:maxLength:33 (D4)
        if(emodHeader&&st==emodStore)memcpy(emodHeader->hash,h.hash,sizeof(h.hash));
        st->beginBatch();
        st->putRecord(emod::T_WORLD_HEADER,&h,sizeof(h));
        if(!st->commitBatch())printg("emod: could not record the image hash\n");
        return;
    }
    // G.2b-4: a local handle (this assigned the static saveFile and left it closed). A short header
    // is refused instead of copied past its end.
    EdenFile* hf=EdenFile::openUpdate(file_name);
    if(!hf){
        printg("err gettin save file: %s\n",file_name.c_str());
        return;
    }
	WorldFileHeader* fh2=(WorldFileHeader*)malloc(sizeof(WorldFileHeader));
    if(hf->read(fh2,sizeof(WorldFileHeader))!=sizeof(WorldFileHeader)){
        printg("err reading has from file\n");
        free(fh2);
        delete hf;
        return;
    }
	eden_cstr_copy(fh2->hash,33,imgHash);   // getCString:maxLength:33 (D4): on overflow [0]=0, the rest untouched
    printg("MD5 hash of file  \"%s\": %s\n", 
          imgHash.c_str(), fh2->hash);
	hf->seek(0);
	hf->write(fh2,sizeof(WorldFileHeader));
	free(fh2);
	
	delete hf;	
   
}
/*-(NSString*)getArchiveName:(NSString*)name{
	if(![World::getWorld->fm worldExists:name:FALSE]) return @"error~";
    return getArchiveName(name);
	//NSString* file_name=[NSString stringWithFormat:@"%@/%@",documents,name];
    
	//return fname;
	
	
}*/

// N.4.5. The name is 50 bytes inside the 192-byte header that every save rewrites anyway (see
// saveWorld's sfh->name), so this is the smallest write that can rename a world: no scratch copy
// and no journal, because no other byte moves. The header is read first and the write refused if
// it is short, so a truncated or foreign file is left alone rather than padded out to a header.
// "r+b" for the reason getName's comment gives (the Windows CRT's text mode).
bool FileManager::renameWorld(const std::string& file_name, const std::string& display_name){
    std::string n=file_name;
    if(!worldExists(n,false)) return true;
    // H.15 (plan §3.5): a dropped-in `.eden.gz` / `.zip` holds its header COMPRESSED; the in-place
    // write below would land 50 raw bytes in the deflate stream and ruin it. Its name is read-only.
    if(isArchiveName(file_name)) return false;
    std::string fn=docs+"/"+n;
    if(isEmodName(file_name)){
        // The name is inside the WORLD_HEADER record, which is never rewritten in place: append a
        // new one. (Called from the world list, where no world -- so no open store -- exists.)
        return emodSetHeaderName(fn,display_name);
    }
    FILE* f=fopen(fn.c_str(),"r+b");
    if(!f) return false;
    WorldFileHeader fh;
    if(fread(&fh,sizeof(WorldFileHeader),1,f)!=1){ fclose(f); return false; }
    char name[sizeof(fh.name)];
    memset(name,0,sizeof(name));
    std::string dn=display_name;
    strncpy(name,dn.c_str(),sizeof(name)-1);
    bool ok=fseek(f,(long)offsetof(WorldFileHeader,name),SEEK_SET)==0
          &&fwrite(name,1,sizeof(name),f)==sizeof(name);
    if(fclose(f)!=0) ok=false;
    return ok;
}

std::string FileManager::getName(const std::string& nameIn){
    // G.2b-2: a name with no UTF-8 form is "" (it was nil: Menu::loadWorlds lists it as "Unknown World");
    // G.2b-4: fm_ns, which is what fm_s(nsstring(..)) did.
    std::string n=nameIn;
	if(!worldExists(n,false)) return "error~";
    std::string fn=docs+"/"+n;
    if(emod::WorldSource::isArchiveName(n)){
        // A dropped-in `.eden.gz` / `.zip`: its name is in the first 192 inflated bytes.
        WorldFileHeader h;
        if(!peekArchiveHeader(fn,&h))return "error~";
        h.name[49]=0;
        if(!h.name[0])return "error";
        return fm_ns(std::string(h.name));
    }
    if(isEmodName(nameIn)){
        emod::EdenWorldStore st;
        if(!st.open(fn.c_str(),false,true,true))return "error~";   // Q.1 F12: header-only (only T_WORLD_HEADER is read)
        std::vector<uint8_t> raw;
        if(st.readRecord(emod::T_WORLD_HEADER,raw)>emod::EdenWorldStore::READ_FALLBACK||raw.size()!=sizeof(WorldFileHeader))return "error~";
        WorldFileHeader h; memcpy(&h,&raw[0],sizeof(h));
        h.name[49]=0;
        if(!h.name[0])return "error";
        return fm_ns(std::string(h.name));
    }
    FILE* sf;
    // "rb", NOT "r", AND THIS ONE IS LOAD-BEARING ON WINDOWS. Menu::loadWorlds calls this for every
    // file in the saves directory to get its display name, so it is on the path of every world
    // list. The MS CRT's text mode translates CRLF to LF and treats a 0x1A byte as end of file,
    // and a WorldFileHeader is 192 bytes of raw struct that can contain both — so the fread below
    // would come up short and every world would list as "error~". POSIX ignores the "b"; this is
    // the mode the call always meant.
    sf=fopen(fn.c_str(),"rb");
    WorldFileHeader* fh=(WorldFileHeader*)malloc(sizeof(WorldFileHeader));
    if(fread(fh,sizeof(WorldFileHeader),1,sf)!=1){
        
      //  printf("help");
        fclose(sf);
         free(fh);
        return "error~";
    }
    fh->name[49]=NULL;
    std::string res_name(fh->name);
    
    if(res_name.length()==0){
        fclose(sf);
         free(fh);
        return "error";
    }
    
    
    fclose(sf);
    free(fh);
    //printf("file_name: %s",res_name.c_str());
    return fm_ns(res_name);
    
	/*NSString* file_name=[NSString stringWithFormat:@"%@/%@",documents,name];
	
	saveFile=[NSFileHandle fileHandleForReadingAtPath:file_name];		
    NSData* data=[saveFile readDataOfLength:sizeof(WorldFileHeader)];
                  if([data length]<sizeof(WorldFileHeader)){
                    
                      [saveFile closeFile];
                     
                       return @"error~";
                  }
	WorldFileHeader* fh=(WorldFileHeader*)[data bytes];
  
   
	
	NSString* fname=[NSString stringWithCString:fh->name encoding:NSUTF8StringEncoding];
	if([fname length]==0){
        fname=@"error~";
       
    }
	
   
	return @"";*/
	
	
}
extern bool SUPPORTS_OGL2;
extern float P_ZFAR;
  static int last_spawn_location=-1;

int FileManager::probeWorldHeight(const std::string& nameIn,bool fromArchive){
    const std::string& name=nameIn;
    if(emod::WorldSource::isArchiveName(name))return T_HEIGHT_DEFAULT;   // never opened as a world
    if(!worldExists(name,fromArchive)){
        // A world that doesn't exist yet is 64z by default -- unless the New World screen parked
        // an explicit 256z choice for it (see eden_menu_take_pending_world_height's header). This
        // is what makes "new worlds stay 64z unless the player explicitly picks 256z" true by
        // construction rather than by every caller remembering to ask.
        return (eden_menu_take_pending_world_height()==T_HEIGHT_MAX)?T_HEIGHT_MAX:T_HEIGHT_DEFAULT;
    }
    std::string file_name=documents+"/"+name;
    if(isEmodName(nameIn)){
        // No probe needed beyond the file header: an `.emod` states its height (bands 4 | 16).
        emod::EdenWorldStore st;
        if(!st.open(file_name.c_str(),false,true,true))return T_HEIGHT_DEFAULT;   // Q.1 F12: header-only (bands())
        return st.bands()==16?T_HEIGHT_MAX:T_HEIGHT_DEFAULT;
    }
    EdenFile* fh=EdenFile::openRead(file_name);
    if(fh==NULL)return T_HEIGHT_DEFAULT;
    WorldFileHeader headerData;
    int height=T_HEIGHT_DEFAULT;
    if(fh->read(&headerData,sizeof(WorldFileHeader))==sizeof(WorldFileHeader)){
        const WorldFileHeader* h=&headerData;
        if(h->version>=FILE_VERSION_256Z&&h->version<=FILE_VERSION_256Z_MAX)height=T_HEIGHT_MAX;
        else if(h->version>=1&&h->version<FILE_VERSION_256Z){
            // S.3b: the version is not a height marker -- the 2026 game stamps v2 on 256z worlds
            // too. Read the directory (directory_offset..EOF; the load reads it next anyway) and
            // let the creature gap decide, the sibling editor's rule (emod::eden_detect_bands).
            // Anything outside 1..6 keeps the 64z answer: loadWorld refuses or legacy-converts it.
            unsigned long long fileEnd=fh_seekEnd(fh);
            if(h->directory_offset>=sizeof(WorldFileHeader)&&h->directory_offset<=fileEnd
               &&fileEnd-h->directory_offset<=(256ull<<20)){
                fh->seek((long long)h->directory_offset);
                std::vector<uint8_t> dir=fh->read((size_t)(fileEnd-h->directory_offset));
                if(emod::eden_detect_bands_raw((const uint8_t*)h,dir.empty()?NULL:&dir[0],dir.size())==16)
                    height=T_HEIGHT_MAX;
            }
        }
    }
    delete fh;
    return height;
}

// ---- 256z Stage 3 item 5: in-app "Convert to 64z" (Settings -> Storage tab) ----
//
// A from-scratch C++ port of port/tools/eden-convert.js's `analyseAndConvertTo64` (Stage 1), not a
// call into it -- that tool is a standalone Node script (fs.readSync/writeSync over a plain fd)
// with no wasm/engine dependency, which is exactly what makes it independently trustworthy as a
// recovery/authoring tool outside the browser. This function is the SAME algorithm restated over
// NSFileHandle (EdenFile since G.2b-4) so the Storage tab can do it without shelling out to Node, which doesn't exist in a
// browser. Keep the two in sync by hand if the format's rules ever change; the shared source of
// truth for what the algorithm must do is docs/eden-file-format.md +
// WORKING/256z-format-backport-plan-2026-08-05.md, not either implementation.
//
// What it does, in order: read the header and directory, derive the creature-block size and each
// column's real span exactly like Stage 2's own readDirectory/deriveColumnSpans do, then stream
// each column out truncated to 4 bands (discarding bands 4-15, counting non-air blocks lost and
// clearing any door/portal half orphaned at the z=63 cut), relocate/drop creatures at or above
// z=64, clamp player/home y into [0,63], and write a fresh directory + header stamped version 4.
// Everything lands in a scratch file first; the original is only replaced after the scratch file
// is fully written and closed, same temp+rename pattern saveWorld() uses.
//
// The NewFormat256z post-directory trailer (signs, command blocks) is dropped, as eden-convert.js
// drops it: the output is Legacy64z, a format that has none (plan §4 *Format eras*; S.5b's
// exporter applies the same rule and reports it before writing).
static ConvertTo64Report convertEmodTo64(const std::string& name);   // X.9, defined with the converter below
ConvertTo64Report FileManager::convertWorldTo64(const std::string& nameIn){
    const std::string& name=nameIn;
    ConvertTo64Report report; memset(&report,0,sizeof(report));
    if(isEmodName(nameIn))return convertEmodTo64(name);

    if(!worldExists(name,false)){
        snprintf(report.error,sizeof(report.error),"world does not exist");
        return report;
    }
    // This does raw file surgery behind the engine's back; the currently-open world has its own
    // live NSFileHandle and in-memory directory/creature state that this function knows nothing
    // about. Refuse rather than race it -- the player can convert after returning to the menu.
    if(World::getWorld&&World::getWorld->doneLoading!=0&&World::getWorld->terrain
       &&!World::getWorld->terrain->world_name.empty()
       &&World::getWorld->terrain->world_name==nameIn){
        snprintf(report.error,sizeof(report.error),
                 "cannot convert the world that is currently open -- return to the main menu first");
        return report;
    }

    std::string file_name=documents+"/"+name;
    EdenFile* fh=EdenFile::openRead(file_name);
    if(fh==NULL){
        snprintf(report.error,sizeof(report.error),"could not open %s",file_name.c_str());
        return report;
    }
    WorldFileHeader header;
    if(fh->read(&header,sizeof(WorldFileHeader))!=sizeof(WorldFileHeader)){
        delete fh;
        snprintf(report.error,sizeof(report.error),"truncated header (save file too short)");
        return report;
    }
    if(header.version>FILE_VERSION_256Z_MAX){
        delete fh;
        snprintf(report.error,sizeof(report.error),
                 "unsupported world format (header version %d, newer than this build knows how to read)",header.version);
        return report;
    }
    unsigned long long fileSize=fh_seekEnd(fh);
    if(header.directory_offset<sizeof(WorldFileHeader)||header.directory_offset>fileSize){
        delete fh;
        snprintf(report.error,sizeof(report.error),
                 "directory offset outside file bounds (corrupt or truncated save)");
        return report;
    }
    // S.3b: below version 5 the header does not say the height (the 2026 game stamps v2 on 256z
    // worlds), so ask the file -- the same rule probeWorldHeight uses.
    if(header.version<FILE_VERSION_256Z){
        fh->seek((long long)header.directory_offset);
        std::vector<uint8_t> dir=fh->read((size_t)(fileSize-header.directory_offset));
        if(header.version<1||emod::eden_detect_bands_raw((const uint8_t*)&header,dir.empty()?NULL:&dir[0],dir.size())!=16){
            delete fh;
            snprintf(report.error,sizeof(report.error),
                     "world is already 64z (header version %d)",header.version);
            return report;
        }
    }

    const unsigned long long BAND_BYTES=(unsigned long long)CHUNK_SIZE*CHUNK_SIZE*CHUNK_SIZE*2;
    const unsigned long long COL_256=BAND_BYTES*16;
    const unsigned long long COL_64=BAND_BYTES*4;
    const unsigned long long ENTITY_SIZE=sizeof(EntityData);
    const unsigned long long DIR_ENTRY_SIZE=sizeof(ColumnIndex);
    const int OUT_SLOTS=200; // MAX_CREATURES_SAVED at 64z, Stage 2's own default

    // ---- read the directory (same struct, same on-disk shape readDirectory() trusts) ----
    unsigned long long dirBytes=fileSize-header.directory_offset;
    long dirCount=(long)(dirBytes/DIR_ENTRY_SIZE);
    struct Entry{int x,z; unsigned long long offset,span,newOffset;};
    std::vector<Entry> entries; entries.reserve(dirCount>0?dirCount:0);
    fh->seek((long long)header.directory_offset);
    for(long i=0;i<dirCount;i++){
        ColumnIndex ci;
        if(fh->read(&ci,(size_t)DIR_ENTRY_SIZE)!=DIR_ENTRY_SIZE)break;
        // S.5b: only addressable rows are columns (readDirectory's twoToOne gate). A NewFormat256z
        // world's sign/command-block trailer is rows tagged x=0xffffffff after the real ones; read as
        // columns they failed the stride check below ("1897 of 3227 columns are not ..."). The
        // trailer is not carried over: the output is Legacy64z, which has none.
        if(ci.x<0||ci.x>=32768||ci.z<0||ci.z>=32768||(((long long)ci.x<<15)+ci.z)==0)continue;
        Entry e; e.x=ci.x; e.z=ci.z; e.offset=ci.chunk_offset; e.span=0; e.newOffset=0;
        entries.push_back(e);
    }

    // ---- derive the creature-block size from the file, exactly like Stage 2 item 4 ----
    // S.3b: a v1/v2 file has no block unless the gap shows one (the 2026 game's v2 256z worlds
    // carry 400 slots), so for those the fallback is 0, not the measured 400.
    unsigned long long creatureBytes=0;
    const unsigned long long assumedCreatureBytes=(header.version>=3)?400ULL*ENTITY_SIZE:0;
    if(entries.empty()){
        creatureBytes=assumedCreatureBytes; // no columns to derive from: assume the measured default
    }else{
        unsigned long long lastEnd=0;
        for(size_t i=0;i<entries.size();i++)lastEnd=std::max(lastEnd,entries[i].offset);
        lastEnd+=COL_256;
        long long gap=(long long)header.directory_offset-(long long)lastEnd;
        if(gap<0||(unsigned long long)gap%ENTITY_SIZE!=0)creatureBytes=assumedCreatureBytes;
        else creatureBytes=(unsigned long long)gap;
    }

    // ---- per-column spans: the gap to the NEXT column, never assumed to be a full record ----
    std::vector<Entry*> sorted; sorted.reserve(entries.size());
    for(size_t i=0;i<entries.size();i++)sorted.push_back(&entries[i]);
    std::sort(sorted.begin(),sorted.end(),[](Entry* a,Entry* b){return a->offset<b->offset;});
    unsigned long long blockDataEnd=header.directory_offset-creatureBytes;
    for(size_t i=0;i<sorted.size();i++){
        unsigned long long next=(i+1<sorted.size())?sorted[i+1]->offset:blockDataEnd;
        long long span=(long long)std::min(COL_256,next-sorted[i]->offset);
        sorted[i]->span=(unsigned long long)std::max((long long)0,span);
    }
    // The bundled RLE template (or a damaged file) has columns nowhere near a clean 131072-byte
    // stride; this is byte surgery only and would silently mangle either. No --force override here
    // -- an in-app action refusing outright is safer than a UI checkbox for "I know what I'm doing".
    int odd=0;
    for(size_t i=0;i+1<sorted.size();i++)if(sorted[i]->span!=COL_256)odd++;
    if(odd>1){
        delete fh;
        snprintf(report.error,sizeof(report.error),
                 "%d of %d columns are not a clean 131072 B record -- looks like a damaged file, not a user save",
                 odd,(int)sorted.size());
        return report;
    }

    report.columns=(int)sorted.size();

    // ---- write the scratch output ----
    std::string temp_name=file_name+".64zconv";
    eden_fs_remove(temp_name); // drop any orphan from a previous failed attempt
    // fileHandleForWritingAtPath: (not ...ForUpdatingAtPath:) deliberately: temp_name does not exist
    // yet (just removed above), so its eager backup-before-overwrite is a no-op, whereas the
    // Updating variant's DEFERRED backup would fire on our first write and leave a stray
    // "<temp_name>.bak" of the empty scratch file behind.
    // Created empty first: Apple's Foundation (macOS/iOS) returns nil from fileHandleForWritingAtPath:
    // for a file that does not exist; only the port's shim created it, so on Apple targets this
    // always failed with "could not create scratch file" (W.2, --convert64-selftest).
    // G.2b-4: EdenFile keeps both rules (null for a missing file, no backups -- D5).
    eden_fs_create_empty(temp_name);
    EdenFile* wh=EdenFile::openWrite(temp_name);
    if(wh==NULL){
        delete fh;
        snprintf(report.error,sizeof(report.error),"could not create scratch file %s",temp_name.c_str());
        return report;
    }
    wh->seek(sizeof(WorldFileHeader)); // header is written last, once directory_offset is known

    std::vector<unsigned char> col(COL_256);
    for(size_t i=0;i<sorted.size();i++){
        Entry* e=sorted[i];
        std::fill(col.begin(),col.end(),0); // short spans read as air, never a neighbour's bytes
        if(e->span>0){
            fh->seek((long long)e->offset);
            fh->read(col.data(),(size_t)e->span);
        }
        int lost=0;
        for(unsigned long long b=4;b<16;b++){
            unsigned long long base=b*BAND_BYTES;
            for(unsigned long long j=0;j<BAND_BYTES/2;j++)if(col[base+j]!=0)lost++;
        }
        if(lost>0){report.blocksDiscarded+=lost; report.columnsAffected++;}

        // A door/portal bottom half retained at world y=63 whose top half lived at y=64 is orphaned
        // by the cut -- clear it rather than leave a half-door standing.
        unsigned long long topBand=3*BAND_BYTES, aboveBand=4*BAND_BYTES;
        for(int lx=0;lx<CHUNK_SIZE;lx++){
            for(int lz=0;lz<CHUNK_SIZE;lz++){
                int cc=CC(lx,lz,15);
                unsigned char bottom=col[topBand+cc];
                unsigned char above=col[aboveBand+CC(lx,lz,0)];
                bool orphan=((bottom>=TYPE_DOOR1&&bottom<=TYPE_DOOR4)&&above==TYPE_DOOR_TOP)||
                            ((bottom>=TYPE_PORTAL1&&bottom<=TYPE_PORTAL4)&&above==TYPE_PORTAL_TOP);
                if(orphan){
                    col[topBand+cc]=0;
                    col[topBand+4096+cc]=0; // paint byte for the same voxel
                    report.doorsOrphaned++;
                }
            }
        }
        e->newOffset=fh_tell(wh);
        wh->write(col.data(),(size_t)COL_64);
    }

    // ---- creatures: keep slot positions, relocate survivors from slots >= 200, drop y >= 64 ----
    std::vector<unsigned char> srcCre(creatureBytes);
    if(creatureBytes>0){
        unsigned long long start=header.directory_offset-creatureBytes;
        if(start>=sizeof(WorldFileHeader)){
            fh->seek((long long)start);
            fh->read(srcCre.data(),(size_t)creatureBytes);
        }
    }
    delete fh;
    long srcSlots=(long)(creatureBytes/ENTITY_SIZE);
    std::vector<unsigned char> outCre(OUT_SLOTS*ENTITY_SIZE,0);
    std::vector<int> freeSlots;
    for(int i=0;i<OUT_SLOTS;i++){
        EntityData* dst=(EntityData*)(outCre.data()+i*ENTITY_SIZE);
        if(i<srcSlots){
            const EntityData* src=(const EntityData*)(srcCre.data()+i*ENTITY_SIZE);
            memcpy(dst,src,ENTITY_SIZE);
            if(dst->type!=-1&&dst->pos.y>=64){dst->type=-1; report.creaturesDropped++; freeSlots.push_back(i);}
            else if(dst->type==-1)freeSlots.push_back(i);
        }else{dst->type=-1; freeSlots.push_back(i);}
    }
    for(long i=OUT_SLOTS;i<srcSlots;i++){
        const EntityData* src=(const EntityData*)(srcCre.data()+i*ENTITY_SIZE);
        if(src->type==-1)continue;
        if(src->pos.y>=64){report.creaturesDropped++; continue;}
        if(freeSlots.empty()){report.creaturesOverflow++; continue;}
        int dstSlot=freeSlots.back(); freeSlots.pop_back();
        memcpy(outCre.data()+dstSlot*ENTITY_SIZE,src,ENTITY_SIZE);
        report.creaturesRelocated++;
    }
    wh->write(outCre.data(),outCre.size());

    // ---- directory: original slot order, patched offsets ----
    unsigned long long directoryOffset=fh_tell(wh);
    std::vector<unsigned char> dirBuf(entries.size()*DIR_ENTRY_SIZE);
    for(size_t i=0;i<entries.size();i++){
        ColumnIndex ci; ci.x=entries[i].x; ci.z=entries[i].z; ci.chunk_offset=entries[i].newOffset;
        memcpy(dirBuf.data()+i*DIR_ENTRY_SIZE,&ci,sizeof(ci));
    }
    if(!dirBuf.empty())wh->write(dirBuf.data(),dirBuf.size());

    // ---- header: patched copy of the original, player/home clamped into the 64z ceiling ----
    WorldFileHeader outHeader=header;
    outHeader.directory_offset=directoryOffset;
    outHeader.version=FILE_VERSION;
    if(!(outHeader.pos.y<63)){outHeader.pos.y=63; report.posClamped=true;}
    if(outHeader.pos.y<0){outHeader.pos.y=0; report.posClamped=true;}
    if(!(outHeader.home.y<63)){outHeader.home.y=63; report.homeClamped=true;}
    if(outHeader.home.y<0){outHeader.home.y=0; report.homeClamped=true;}
    wh->seek(0);
    wh->write(&outHeader,sizeof(outHeader));
    delete wh;

    // ---- commit: only now does the original get replaced ----
    eden_fs_remove(file_name);
    if(!eden_fs_move(temp_name,file_name)){
        snprintf(report.error,sizeof(report.error),"converted successfully but could not replace the original file");
        return report;
    }
    report.ok=true;
    return report;
}

void FileManager::loadWorld(const std::string& nameIn,bool fromArchive){
    const std::string& name=nameIn;
   
    
    
    
	Terrain* ter=World::getWorld->terrain;
		ter->clearBlocks();
	Player* player=World::getWorld->player;
    this->closeWorld();   // a previous session's `.emod` store (warp, re-load); no-op for `.eden`
    // K.1-3: no palette survives into the next world or a session (a new world and a session have none;
    // loadEmodWorld reads the world's own). Rows 128..255 go back to the missing block.
    BlockRegistry::resetWorld();
    // D.3a: a brand-new world has none; the two load branches below read theirs. MP.3a: a session's
    // trailer is filled from SIGNP and outlives its warps and reloads (MpWorld::endSession clears it).
    if(!sessionWorld)worldTrailer.clear();
    // DN.2: a world without a SKY_CLOCK record (new, or from before Stage DN) starts at 0.30;
    // loadEmodWorld reads the record. A session's clock lives through its warps (MpWorld::beginSession).
    if(!sessionWorld){SkyCycle::reset();Weather::reset();}   // AT.1b: likewise the WEATHER record
    if(emod::WorldSource::isArchiveName(name)){
        // An archive that was not converted can't be played, and must never reach the .eden branch
        // below: its "header" is compressed bytes, which that branch would try to "upgrade" in place.
        eden_report_load_failure(name.c_str(),"this archive could not be converted to a world");
        return;
    }
    imgHash.clear();
    firstSaveOwed=false;
    // QA.14: not from the load pthread (it raced the input tick's Player::setSpeed/fly thrust);
    // World::loadWorld's doneLoading==2 branch resets the player on the main thread once the load is in.
    if(!loadOnThread)World::getWorld->player->reset();
	if(sessionWorld){
        this->loadSessionWorld();   // MP.3a: no file, no convert gate, no directory
    }else if(!worldExists(name,fromArchive)){
     
        
        extern int g_terrain_type;
        
        printg("making new world : %d\n",g_terrain_type);
        
      //  clear();
        bool gen_default=false;
       g_terrain_type=9;
        if(g_terrain_type==0){
            makeDirt();
        }else if(g_terrain_type==1){
           // makeMars();
        }else if(g_terrain_type==2){
            makeRiverTrees(T_SIZE/2,0,T_SIZE,T_SIZE,550);
        }else if(g_terrain_type==3){
             makeRiverTrees(T_SIZE/2,0,T_SIZE,T_SIZE,550);
            makeMountains(0,0,T_SIZE/2-16,T_SIZE,400);
            makeTransition(T_SIZE/2-16,0,T_SIZE/2,T_SIZE);
        }else if(g_terrain_type==4){
            makeDesert();
        }else if(g_terrain_type==5){
            makePonies();
        }else if(g_terrain_type==6){
            makeBeach();
        }else if(g_terrain_type==7){
            makeMix();
        }else if(g_terrain_type==8){
            genflat=true;
        }else if(g_terrain_type==9){
            gen_default=true;
        }

		this->clearDirectory();
        // H.4: a Classic world from the New World screen: its record's seed is the world's seed,
        // and the record goes into the first save below.
        fm_resetGenerator();
        ClassicGen newClassic;
        bool newIsClassic=false;
        {
            unsigned char rec[CLASSIC_RECORD_BYTES];
            if(eden_menu_take_pending_generator(rec)&&!genflat&&classic_decode_record(rec,sizeof(rec),&newClassic))
                newIsClassic=true;
        }
        if(genflat)ter->tgen->LEVEL_SEED= 0;
        else if(newIsClassic){
            ter->tgen->LEVEL_SEED=newClassic.seed;
            fm_setNewWorldGenerator(newClassic);
        }else if(gen_default){
           
            
            ter->tgen->LEVEL_SEED=DEFAULT_LEVEL_SEED;
            
        }else{
             ter->tgen->LEVEL_SEED=arc4random()%300000;
            
            
        }
		int centerChunk=4096;
        int r=T_SIZE/CHUNK_SIZE/2;

		ter->level_seed=ter->tgen->LEVEL_SEED;
		
        
        
        
	
		
		Vector temp;
		
        int tempyaw=90;
      
        if(newIsClassic){
            temp=fm_classicSpawn(newClassic);
            chunkOffsetX=windowOriginChunk(temp.x);
            chunkOffsetZ=windowOriginChunk(temp.z);
        }else if(gen_default){
            int spawn_location=arc4random()%10;
            while(spawn_location==last_spawn_location){
                spawn_location=arc4random()%10;
            }
            last_spawn_location=spawn_location;
            int spx[10]={/*64036+(700),*/64736,64629,66370, 66286,64919,65415,64763,64949,64233, 65555};
            int spz[10]={/*64036+(1700),*/65731,66306,65496,66286,64866,66296,66224,64254,64234, 65537};
            int spy[10]={/*25,     */ 22,24,14,22,30, 21,23,22,34,25};
            int spyaw[10]={/*0,    */ -176,-85,1,22,88, 176,-138,91,271,91};
            temp.x=spx[spawn_location];
            temp.z=spz[spawn_location];
            temp.y=spy[spawn_location];
            tempyaw=spyaw[spawn_location];
            
            chunkOffsetX=centerChunk-r;
            chunkOffsetZ=centerChunk-r;
            chunkOffsetX=windowOriginChunk(temp.x);   // B.5b-1a: corner-centred
            chunkOffsetZ=windowOriginChunk(temp.z);
            
        }else{
            chunkOffsetX=centerChunk-r;
            chunkOffsetZ=centerChunk-r;
            temp.x=centerChunk*CHUNK_SIZE+CHUNK_SIZE/2;
            temp.z=centerChunk*CHUNK_SIZE+CHUNK_SIZE/2;
            temp.y=T_HEIGHT-10;
        }
        
        
        
        for(int x=centerChunk-r;x<centerChunk+r;x++){
            
            for(int z=centerChunk-r;z<centerChunk+r;z++){
                
                // G.2b-2: no handle. Since S.8 only an `.emod` (or a session) gets here, whose columns
                // never touch it; the stale static NSFileHandle this passed was never read.
                readColumn(x,z,NULL);
                World::getWorld->terrain->counter++;
            }
        }
        
        
		ter->home=temp;
		Vector temp2;		
		temp2.x=BLOCK_SIZE*(ter->home.x+.5f);
		temp2.y=BLOCK_SIZE*(ter->home.y+1);
		temp2.z=BLOCK_SIZE*(ter->home.z+.5f);
            player->pos=temp2;
        
        if(ter->tgen->LEVEL_SEED==0){
            temp2.x=BLOCK_SIZE*(ter->home.x+.5f);
            temp2.y=34;
            temp2.z=BLOCK_SIZE*(ter->home.z+.5f);
            player->pos=temp2;
            for(int i=0;i<4;i++){
                for(int j=0;j<4;j++){
                    regionSkyColors[i][j]=COLOR_NORMAL_BLUE;
                }
            }
            printg("sup!!!\n!");
        }else if(newIsClassic){
            // H.4: 1.x had no regional skies -- the flat world's plain blue grid (generator-plan.md §5.2).
            for(int i=0;i<4;i++){
                for(int j=0;j<4;j++){
                    regionSkyColors[i][j]=COLOR_NORMAL_BLUE;
                }
            }
        }else{
            for(int i=0;i<4;i++){
                for(int j=0;j<4;j++){
                    regionSkyColors[i][j]=defaultRegionSkyColors[i][j];
                }
            }
        }
        //(player.pos).y=1;
        
		//printg("player pos init save: %f %f %f",player.pos.x,player.pos.y,player.pos.z);
		//NSLog(@"chunkOffsets: %d %d",chunkOffsetX,chunkOffsetZ);
        player->yaw=tempyaw;
        // g_world_height is already whatever World::loadWorld's probeWorldHeight() call decided
        // (64 by default, 256 only if the New World screen parked that choice -- see
        // eden_menu_take_pending_world_height above). A 256z new world is stamped straight to
        // FILE_VERSION_256Z so saveWorld() below preserves it instead of normalising to 4, and
        // gets the wider creature block before the clearing loop below runs.
        if(g_world_height>=T_HEIGHT_MAX){
            file_version=FILE_VERSION_256Z;
            eden_set_creature_slots(MAX_CREATURES_SAVED_MAX);
        }else{
            file_version=2;
        }
		//[ter updateAllImportantChunks];

        for(int i=0;i<MAX_CREATURES_SAVED;i++){
            creatureData[i].type=-1;
        }
        
        LoadModels2();
        // QA.14: the first save is finishLoadOnMain's (saveWorld -> Terrain::endDynamics ->
        // Resources::endBurn reaches the audio the main thread is ticking).
        firstSaveOwed=true;
	}else if(isEmodName(nameIn)){
        if(!this->loadEmodWorld(nameIn))return;
    }else{
        // S.8: a `.eden` is only ever a conversion SOURCE (World::loadWorld converts it first, then plays the
        // `.emod`); one that gets here was not converted and must never be opened for update.
        eden_report_load_failure(name.c_str(),"this world is not an .emod -- it has to be converted first");
        return;
    }
    if(!SUPPORTS_OGL2){
        if(ter->tgen->LEVEL_SEED== 0)
            Graphics::setZFAR(55);
       
        else 
         Graphics::setZFAR(40);
    }else{
        if(ter->tgen->LEVEL_SEED== 0)
         Graphics::setZFAR(120);
        else 
        Graphics::setZFAR(120);
    }

    if(!loadOnThread)this->finishLoadOnMain();
	updateSkyColor1(World::getWorld->player,true);
    extern bool loaded_new_terrain;
    loaded_new_terrain=true;

}
// QA.14 (A2-03): the tail of a load that touches state the main thread uses while the menu is still up
// (touches, effects, HUD, audio), so it always runs there: straight from loadWorld on an inline load,
// from World::loadWorld's doneLoading==2 branch (thread joined, load not failed) on a threaded one.
void FileManager::finishLoadOnMain(){
    if(firstSaveOwed){   // a new world's first save: creates the `.emod`
        firstSaveOwed=false;
        this->saveWorld();
    }
    Input::getInput()->clearAll();
    World::getWorld->effects->clearAllEffects();
    World::getWorld->hud->worldLoaded();
    {   // UX.8-3: the world's hotbar (its folder's hotbar.json; a loose key or a session keeps prefs' hotbar.last)
        const std::string dir=sessionWorld?std::string():worldDirOf(World::getWorld->terrain->world_name);
        Hotbar::get().load(dir.empty()?dir:documents+"/"+dir);
    }
    fm_generatorToast();   // H.4: a GENERATOR record from a newer Emod
    fm_damageNotice();     // QA.17: after the generator toast, so the damage line is the one left showing
}


// =============================================================================================
// Stage S / S.4 + S.5: `.emod` worlds -- docs/emod-file-format.md, Classes/EdenWorldStore.h
// =============================================================================================
// A world is an `.emod` iff its file name ends in ".emod". Everything above routes on that one
// test (isEmodName), so an `.eden` world takes exactly the code it always did, and a name that
// ends in ".emod" never reaches the `.eden` file handle or the directory hashmap. The container is
// append-only: a save appends the dirty columns + header + creatures + a COMMIT in one write and
// fsyncs; nothing a previous commit wrote is ever overwritten (so no scratch copy, no journal, no
// threshold -- docs/save-load.md).

// G.2b-2: plain C++ (path_has_suffix is hasSuffix:, measured; "" stands for the old nil).
bool FileManager::isEmodName(const std::string& name){
    return path_has_suffix(name,EMOD_WORLD_EXTENSION);
}

bool FileManager::isArchiveName(const std::string& name){
    // Never a backup slot or an export the engine itself left beside a world (`.eden.bak.zip`,
    // stock's CompressWorld `.eden.zip` of a world that is still listed): those are not new worlds.
    if(name.empty()||path_has_suffix(name,".bak.zip")||path_has_suffix(name,".bak.gz"))return false;
    return emod::WorldSource::isArchiveName(name);
}

// ---- K.0w world folders (plan §3, §5.1). All keys/paths here are Documents-relative with "/". ----
static const char* const kWorldsDir="Worlds";
static const char* const kWorldFile="world.emod";

bool FileManager::validWorldId(const std::string& id){
    if(id.empty()||id.size()>64||id[0]=='.')return false;
    for(size_t i=0;i<id.size();i++){
        char c=id[i];
        if(!((c>='0'&&c<='9')||(c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_'||c=='-'))return false;
    }
    return true;
}

static std::string fm_randomLetters(int n){
    std::string s;
    for(int i=0;i<n;i++)s+=(char)('a'+arc4random()%26);
    return s;
}

// An id is free when no world folder, staging dir or adoption dir uses it.
static bool fm_idFree(const std::string& documents,const std::string& id){
    const std::string w=documents+"/"+kWorldsDir+"/";
    return !eden_fs_exists(w+id)&&!eden_fs_exists(w+"."+id+".partial")&&!eden_fs_exists(w+"."+id+".migrating");
}

std::string FileManager::worldKeyOfId(const std::string& id){
    return std::string(kWorldsDir)+"/"+id+"/"+kWorldFile;
}

std::string FileManager::stagingDirOf(const std::string& id){
    return std::string(kWorldsDir)+"/."+id+".partial";
}

// K.0w-3: EDEN_TEST_STAGE_KILL_AFTER=<n> _Exits (86) after the n-th mutating staging step, counted across
// the process (port/tools/folders-crash-gate.sh loops n over a conversion).
void FileManager::stageStep(const char* what){
    static int limit=-2,count=0;
    if(limit==-2){const char* e=getenv("EDEN_TEST_STAGE_KILL_AFTER");limit=(e&&*e)?atoi(e):-1;}
    if(limit<0)return;
    if(++count==limit){
        fprintf(stderr,"stage: test kill after step %d (%s)\n",count,what);
        fflush(stderr);fflush(stdout);
        std::_Exit(86);
    }
}

std::string FileManager::beginStaging(const std::string& documents,const std::string& id){
    if(!validWorldId(id))return std::string();
    const std::string d=documents+"/"+stagingDirOf(id);
    if(!eden_fs_make_dirs(d))return std::string();
    stageStep("staging dir made");
    return d;
}

std::string FileManager::commitStaging(const std::string& documents,const std::string& id){
    const std::string wroot=documents+"/"+kWorldsDir,from=documents+"/"+stagingDirOf(id);
    if(eden_fs_size(from+"/"+kWorldFile)<0)return std::string();   // nothing committed in it
    std::string nid=id;
    for(int tries=0;tries<8;tries++){
        // The commit point: one no-replace rename on one volume. Before it the world is only ever a dot-dir
        // (swept by cleanConversionTemps); after it the world is listed.
        if(eden_fs_move_new(from,wroot+"/"+nid)){
            stageStep("staging dir renamed");
            eden_fs_sync_dir(wroot);
            return worldKeyOfId(nid);
        }
        nid=id+"-"+fm_randomLetters(4);
        while(eden_fs_exists(wroot+"/"+nid))nid=id+"-"+fm_randomLetters(4);
    }
    return std::string();
}

void FileManager::discardStaging(const std::string& documents,const std::string& id){
    if(validWorldId(id))eden_fs_remove_tree(documents+"/"+stagingDirOf(id));
}

std::string FileManager::newWorldId(const std::string& documents){
    char base[32];
    snprintf(base,sizeof(base),"%lld",(long long)time(NULL));
    for(;;){
        std::string id=std::string(base)+"-"+fm_randomLetters(4);
        if(fm_idFree(documents,id))return id;
    }
}

std::string FileManager::downloadWorldId(const std::string& documents,const std::string& serverId){
    if(!validWorldId(serverId))return newWorldId(documents);
    for(std::string id=serverId;;id=serverId+"-"+fm_randomLetters(4)){
        if(fm_idFree(documents,id))return id;
    }
}

bool FileManager::isFolderWorld(const std::string& key){
    const std::string pre=std::string(kWorldsDir)+"/",post=std::string("/")+kWorldFile;
    if(key.size()<=pre.size()+post.size()||key.compare(0,pre.size(),pre)!=0)return false;
    if(key.compare(key.size()-post.size(),post.size(),post)!=0)return false;
    return validWorldId(key.substr(pre.size(),key.size()-pre.size()-post.size()));
}

std::string FileManager::worldDirOf(const std::string& key){
    if(!isFolderWorld(key))return std::string();
    return key.substr(0,key.size()-strlen(kWorldFile)-1);
}

std::string FileManager::photoPathOf(const std::string& key){
    std::string d=worldDirOf(key);
    return d.empty()?key+".png":d+"/photo.png";
}

std::string FileManager::originalPathOf(const std::string& key,const std::string& ext){
    std::string d=worldDirOf(key);
    return d.empty()?std::string():d+"/original"+ext;
}

std::string FileManager::sidecarPathOf(const std::string& key,const std::string& kind){
    std::string d=worldDirOf(key);
    return d.empty()?std::string():d+"/"+kind+".eden.dat";
}

std::vector<std::string> FileManager::folderWorldKeys(){
    std::vector<std::string> out;
    const std::string root=documents+"/"+kWorldsDir;
    std::vector<std::string> ids=eden_fs_list(root);
    for(size_t i=0;i<ids.size();i++){
        if(!validWorldId(ids[i]))continue;
        std::string key=worldKeyOfId(ids[i]);
        if(eden_fs_size(documents+"/"+key)>=0)out.push_back(key);
    }
    return out;
}

void FileManager::listWorlds(std::vector<WorldEntry>* out){
    out->clear();
    std::vector<std::string> names=eden_fs_list(documents);
    std::vector<std::string> fw=folderWorldKeys();
    names.insert(names.end(),fw.begin(),fw.end());
    std::vector<WorldEntry> worlds,dropped;
    std::vector<std::pair<std::string,long long> > hidden;   // (paired .emod, original's bytes)
    for(size_t i=0;i<names.size();i++){
        const std::string& f=names[i];
        if(f=="Eden.eden.archive")continue;
        std::string ext=path_ext(f);
        for(size_t k=0;k<ext.size();k++)ext[k]=(char)toupper((unsigned char)ext[k]);
        const bool archive=(ext=="GZ"||ext=="ZIP")&&isArchiveName(f);
        if(ext!="EDEN"&&ext!="EMOD"&&!archive)continue;
        bool needs=false;
        if(ext=="EDEN"||archive){
            std::string paired=pairedEmodFor(f);
            if(!paired.empty()){
                long long b=eden_fs_size(documents+"/"+f);
                hidden.push_back(std::make_pair(paired,b>0?b:0));
                continue;
            }
            needs=worldNeedsConversion(f);
        }
        WorldEntry e;
        e.key=f;
        e.name=getName(f);
        if(e.name.empty())e.name="Unknown World";   // a name with no UTF-8 form
        if(e.name=="error~")continue;
        e.needs_convert=needs;
        e.mtime=eden_fs_mtime(documents+"/"+f);
        if(isFolderWorld(f)){
            const std::string o=originalOf(f);
            long long b=o.empty()?-1:eden_fs_size(documents+"/"+o);
            if(b>0)e.original_bytes=b;
        }
        (ext=="EMOD"?worlds:dropped).push_back(e);
    }
    std::sort(worlds.begin(),worlds.end(),[](const WorldEntry& a,const WorldEntry& b){
        return a.mtime!=b.mtime?a.mtime>b.mtime:a.key<b.key;});
    std::sort(dropped.begin(),dropped.end(),[](const WorldEntry& a,const WorldEntry& b){return a.key<b.key;});
    out->insert(out->end(),worlds.begin(),worlds.end());
    out->insert(out->end(),dropped.begin(),dropped.end());
    for(size_t i=0;i<hidden.size();i++)
        for(size_t j=0;j<out->size();j++)
            if((*out)[j].key==hidden[i].first){(*out)[j].original_bytes=hidden[i].second;break;}
}

bool FileManager::removeWorldDir(const std::string& relDir){
    const std::string pre=std::string(kWorldsDir)+"/";
    if(relDir.size()<=pre.size()||relDir.compare(0,pre.size(),pre)!=0||!validWorldId(relDir.substr(pre.size())))return false;
    return eden_fs_remove_tree(docs+"/"+relDir);
}

std::string FileManager::newWorldFileName(){
    return worldKeyOfId(newWorldId(docs));
}

void FileManager::closeWorld(){
    if(emodStore){delete emodStore;emodStore=NULL;}
    if(emodHeader){free(emodHeader);emodHeader=NULL;}
}

bool FileManager::emodOpen(){return emodStore!=NULL;}

// ---- H.4: which generator fills this world's unsaved columns -----------------------------------
// generator-plan.md §4. Seed 0 is the flat world and 333333 the bundled map; any other seed is
// Classic: the GENERATOR record if the world has one, else a valid `.eden` mirror in the header's
// reserved[] (an imported Classic world), else the legacy rule -- a 1.x random-seed world, whose
// caves are detected once from its saved columns. The record is written by the next save
// (fm_generatorPending) and never again, so a world's terrain never flips.
static bool fm_generatorPending=false;
static bool fm_generatorVoidToast=false;
int g_classic_cave_votes_cells=0,g_classic_cave_votes_air=0;   // the last detection, for --classic-selftest
static int fm_detectLegacyCaves(emod::EdenWorldStore* st,int seed){
    std::vector<std::pair<int32_t,int32_t> > keys=st->columnKeys();
    std::vector<uint8_t> col((size_t)st->bands()*emod::BAND);
    long cells=0,air=0;
    size_t step=keys.size()>64?keys.size()/64:1;
    for(size_t i=0,n=0;i<keys.size()&&n<64;i+=step,n++){
        if(st->readColumn(keys[i].first,keys[i].second,&col[0])>emod::EdenWorldStore::READ_FALLBACK)continue;
        classic_cave_votes(seed,keys[i].first,keys[i].second,&col[0],st->bands(),&cells,&air);
    }
    g_classic_cave_votes_cells=(int)cells; g_classic_cave_votes_air=(int)air;
    printg("classic: legacy cave detection %ld/%ld carved cells are air\n",air,cells);
    return cells>=2000&&air*2>=cells?CLASSIC_CAVES_2011:CLASSIC_CAVES_OFF;
}
// Which settings a Classic seed's world generates with (WP_GEN_*). Shared by the load and by the
// Properties screen (H.15), so the screen shows exactly what the next load would use.
static int fm_resolveGenerator(emod::EdenWorldStore* st,const WorldFileHeader* h,int seed,ClassicGen* g){
    std::vector<uint8_t> rec;
    if(st&&st->hasRecord(emod::T_GENERATOR)){
        if(st->readRecord(emod::T_GENERATOR,rec)<=emod::EdenWorldStore::READ_FALLBACK&&
           classic_decode_record(rec.empty()?NULL:&rec[0],rec.size(),g)&&g->seed==seed)return WP_GEN_RECORD;
        return WP_GEN_VOID;
    }
    if(classic_decode_mirror((const unsigned char*)h->reserved,g)&&g->seed==seed)return WP_GEN_MIRROR;
    classic_legacy_params(g,seed);
    if(st&&st->columnCount()>0){
        g->caves=fm_detectLegacyCaves(st,seed);
        g->flags=1;
    }
    return WP_GEN_LEGACY;
}
static void fm_setupGenerator(TerrainGenerator* tg,emod::EdenWorldStore* st,const WorldFileHeader* h){
    tg->classicState=TerrainGenerator::CLASSIC_STATE_UNSET;
    fm_generatorPending=false;
    fm_generatorVoidToast=false;
    if(tg->LEVEL_SEED==0||tg->LEVEL_SEED==DEFAULT_LEVEL_SEED)return;
    ClassicGen g;
    const int src=fm_resolveGenerator(st,h,tg->LEVEL_SEED,&g);
    if(src==WP_GEN_VOID){
        tg->classicState=TerrainGenerator::CLASSIC_STATE_VOID;
        fm_generatorVoidToast=true;
        return;
    }
    tg->setClassic(g);
    fm_generatorPending=src!=WP_GEN_RECORD;
}
// The void columns' one-time toast, once the HUD is up (FileManager::loadWorld's tail).
// QA.17 (A2-06, U8 = notice only): a damaged `.emod` used to say so on stderr alone, invisible on a device,
// while compaction stays off and the file grows by every save. Once per load, on the main thread: from
// finishLoadOnMain (damage found by open or by the load's column reads) or from a column read in play.
static bool fm_damageNoticeShown=false;
static void fm_damageNotice(){
    if(fm_damageNoticeShown||!emodStore||!emodStore->damaged())return;
    World* w=World::getWorld;
    if(!w||!w->hud||!w->hud->sb)return;
    fm_damageNoticeShown=true;
    w->hud->sb->setStatus("Part of this world's file is damaged: playing what survives. Export it to keep a clean copy.",8);
}
static void fm_generatorToast(){
    if(!fm_generatorVoidToast)return;
    fm_generatorVoidToast=false;
    World::getWorld->hud->sb->setStatus("This world was made by a newer Emod: unexplored land stays empty",6);
}
// A new world starts with no generator decided (a previous world's must not leak into it).
static void fm_resetGenerator(){
    World::getWorld->terrain->tgen->classicState=TerrainGenerator::CLASSIC_STATE_UNSET;
    fm_generatorPending=false;
    fm_generatorVoidToast=false;
}
// H.4: a new Classic world's record arrives from the New World screen (Menu.cpp's pending
// seam) and goes into the world's first save.
void fm_setNewWorldGenerator(const ClassicGen& g){
    World::getWorld->terrain->tgen->setClassic(g);
    fm_generatorPending=true;
}
// generator-plan.md §5.2: the first block column, spiralling out from the middle of chunk 4096,4096,
// whose surface is open sky over dirt (the skin's top cell is solid: not a cave mouth), from the
// heightmap and skin alone; then the first two air cells above whatever the column grows (a tree,
// a cloud) so the player never spawns inside one.
static Vector fm_classicSpawn(const ClassicGen& g){
    const int H=T_HEIGHT,cx0=4096*CHUNK_SIZE+CHUNK_SIZE/2,cz0=cx0;
    int sx=cx0,sz=cz0,top=-1;
    for(int r=0;r<256&&top<0;r++){
        for(int dx=-r;dx<=r&&top<0;dx++){
            for(int dz=-r;dz<=r&&top<0;dz++){
                if(dx!=-r&&dx!=r&&dz!=-r&&dz!=r)continue;     // the ring only
                int t=classic_surface_top(g,cx0+dx,cz0+dz,H);
                if(t>0&&t==classic_height(g,cx0+dx,cz0+dz,H)-1){ sx=cx0+dx; sz=cz0+dz; top=t; }
            }
        }
    }
    if(top<0)top=classic_height(g,sx,sz,H)-1;
    static block8 types[CHUNK_SIZE*CHUNK_SIZE*T_HEIGHT_MAX];
    static color8 colors[CHUNK_SIZE*CHUNK_SIZE*T_HEIGHT_MAX];
    const int bx=sx-(sx&(CHUNK_SIZE-1)),bz=sz-(sz&(CHUNK_SIZE-1));
    classic_generate_window(g,bx,bz,CHUNK_SIZE,CHUNK_SIZE,H,types,colors,true);
    const block8* col=&types[((sx-bx)*CHUNK_SIZE+(sz-bz))*H];
    int y=top+1;
    while(y<H-2&&(col[y]!=TYPE_NONE||col[y+1]!=TYPE_NONE))y++;
    Vector v;
    v.x=sx; v.y=y; v.z=sz;
    return v;
}

bool FileManager::loadEmodWorld(const std::string& nameIn){
    Terrain* ter=World::getWorld->terrain;
    Player* player=World::getWorld->player;
    std::string path=docs+"/"+nameIn;
    emod::EdenWorldStore* st=new emod::EdenWorldStore();
    fm_damageNoticeShown=false;   // QA.17: once per load
    if(!st->open(path.c_str(),true)){
        std::string why="unreadable .emod world: "+st->error();
        delete st;
        eden_report_load_failure(nameIn.c_str(),why.c_str());
        return false;
    }
    // Housekeeping lives at open, never mid-play (plan §6): a log that is mostly dead bytes is
    // rewritten once, here. Refused by the store while damaged, and a failure just leaves the log.
    // stderr, not printg/NSLog: both compile out of Release, and damage is the one thing worth a line.
    if(st->damaged())fprintf(stderr,"emod: %s is damaged (%d issue(s), first: %s) -- playing what survives; compaction is off\n",
                           path.c_str(),(int)st->damage().size(),st->damage()[0].c_str());
    if(st->shouldCompact()&&!st->damaged()){
        if(!st->compact())fprintf(stderr,"emod: compaction skipped: %s\n",st->error().c_str());
    }
    std::vector<uint8_t> raw;
    if(st->readRecord(emod::T_WORLD_HEADER,raw)>emod::EdenWorldStore::READ_FALLBACK||raw.size()!=sizeof(WorldFileHeader)){
        delete st;
        eden_report_load_failure(nameIn.c_str(),"no readable world header in this .emod (corrupt save)");
        return false;
    }
    if(st->bands()!=CHUNKS_PER_COLUMN){
        // World::loadWorld probed the height from this same file; a mismatch is a caller bug, and
        // reading 4-band records into a 16-band window (or the reverse) would corrupt the world.
        delete st;
        eden_report_load_failure(nameIn.c_str(),"world height does not match the .emod's band count");
        return false;
    }
    std::vector<uint8_t> craw;
    if(st->hasRecord(emod::T_CREATURES))st->readRecord(emod::T_CREATURES,craw);
    // D.3a: an unreadable SIGN_TRAILER (READ_AIR_DAMAGED) loads as none -- and stays clean, so no save
    // writes an empty one over whatever an earlier version still holds.
    std::vector<uint8_t> traw;
    if(st->hasRecord(emod::T_SIGN_TRAILER)&&st->readRecord(emod::T_SIGN_TRAILER,traw)>emod::EdenWorldStore::READ_FALLBACK)traw.clear();
    // DN.2: the day/night clock. An unreadable or malformed record loads as none (t 0.30, unlocked).
    {
        std::vector<uint8_t> kraw;
        if(st->hasRecord(emod::T_SKY_CLOCK)&&st->readRecord(emod::T_SKY_CLOCK,kraw)<=emod::EdenWorldStore::READ_FALLBACK)
            SkyCycle::load(kraw.empty()?NULL:&kraw[0],kraw.size());
        else SkyCycle::reset();
    }
    // AT.1b: the weather (record 12). Unreadable, malformed or absent = none: clear, seeded from LEVEL_SEED.
    {
        std::vector<uint8_t> wraw;
        if(st->hasRecord(emod::T_WEATHER)&&st->readRecord(emod::T_WEATHER,wraw)<=emod::EdenWorldStore::READ_FALLBACK)
            Weather::load(wraw.empty()?NULL:&wraw[0],wraw.size());
        else Weather::reset();
    }
    // K.1-3: the palette (rows 128..255) resolves before the first column is read, so nothing ever
    // meshes a palette id with another world's rows. A flagged file without a readable record locks it.
    {
        std::vector<uint8_t> praw;
        const bool flagged=st->paletteFlag();
        emod::EdenWorldStore::ReadResult pr=emod::EdenWorldStore::READ_ABSENT;
        if(flagged&&st->hasRecord(emod::T_BLOCK_PALETTE))pr=st->readRecord(emod::T_BLOCK_PALETTE,praw);
        if(pr>emod::EdenWorldStore::READ_FALLBACK)praw.clear();
        // An unflagged file's palette is never read (the flag announces it; compaction drops a stray one).
        BlockRegistry::loadPalette(praw.empty()?NULL:&praw[0],praw.size(),flagged);
        // An older version decoded but the newest did not: the newest may have given out more ids.
        if(pr==emod::EdenWorldStore::READ_FALLBACK)BlockRegistry::lock();
        if(flagged)printg("registry: %d palette entr%s%s\n",BlockRegistry::count(),BlockRegistry::count()==1?"y":"ies",
                          BlockRegistry::locked()?" (LOCKED: unreadable)":"");
    }
    closeWorld();
    // The `.eden` directory maps belong to whatever world was open before: nothing below reads
    // them for an `.emod`, and leaving a previous world's rows in them is how one would one day.
    this->clearDirectory();
    this->clearColumnSpans();
    emodStore=st;
    emodHeader=(WorldFileHeader*)malloc(sizeof(WorldFileHeader));
    memcpy(emodHeader,&raw[0],sizeof(WorldFileHeader));
    sfh=emodHeader;
    fm_sanitizeHeader(sfh);   // QA.2
    // The header version is kept verbatim (never normalised): it is not a height marker.
    file_version=sfh->version;
    // The `.eden` loader's one-time v3 -> v4 upgrade, the same three fields (a converted v3 world
    // arrives here with its v3 header verbatim).
    if(file_version==3){
        file_version=4;
        sfh->version=4;
        sfh->goldencubes=10;
        for(int i=0;i<4;i++){
            for(int j=0;j<4;j++){
                sfh->skycolors[i*4+j]=COLOR_NORMAL_BLUE;
            }
        }
    }
    printg("FILE VERSION: %d (.emod, %d bands, %u columns)\n",file_version,st->bands(),(unsigned)st->columnCount());
    {
        int slots=(int)(craw.size()/sizeof(EntityData));
        if(craw.size()%sizeof(EntityData)!=0||slots>MAX_CREATURES_SAVED_MAX)slots=(g_world_height>=T_HEIGHT_MAX?400:200);
        eden_set_creature_slots(slots);
    }
    // G.2b-4: initWithCString:encoding: read to the NUL (nil, so "", for bytes with no UTF-8 form). Bounded
    // at the field: an unterminated 36 bytes overflows the 33-byte copy on save either way (empty hash).
    imgHash=fm_ns(std::string(sfh->hash,strnlen(sfh->hash,sizeof(sfh->hash))));
    ter->level_seed=sfh->level_seed;
    ter->tgen->LEVEL_SEED=ter->level_seed;
    fm_setupGenerator(ter->tgen,st,sfh);   // H.4
    cur_dir_offset=sfh->directory_offset;
    World::getWorld->hud->goldencubes=sfh->goldencubes;
    ter->home=sfh->home;
    player->pos=sfh->pos;
    worldUnbounded=fm_headerOutside(sfh);
    if(!worldUnbounded){
        std::vector<std::pair<int32_t,int32_t> > keys=st->columnKeys();
        for(size_t i=0;i<keys.size();i++)if(fm_columnOutsideSquare(keys[i].first,keys[i].second)){worldUnbounded=true;break;}
    }
    player->yaw=sfh->yaw;
    for(int i=0;i<4;i++){
        for(int j=0;j<4;j++){
            regionSkyColors[i][j]=(int)(sfh->skycolors[i*4+j]);
        }
    }
    oldOffsetX=chunkOffsetX;
    oldOffsetZ=chunkOffsetZ;
    // B.5b-1a: corner-centred, through the one helper the streamer uses, so the window this load
    // reads is exactly the one prepareAndLoadGeometry wants on its first frame (no row streams back).
    chunkOffsetX=windowOriginChunk(player->pos.x);
    chunkOffsetZ=windowOriginChunk(player->pos.z);
    player->pos=sfh->pos;
    int r=T_RADIUS;
    for(int x=chunkOffsetX;x<chunkOffsetX+2*r;x++){
        for(int z=chunkOffsetZ;z<chunkOffsetZ+2*r;z++){
            readColumn(x,z,NULL);
            World::getWorld->terrain->counter++;
        }
    }
    // LoadCreatures() for a record instead of a file region: start from "no creature in any slot"
    // (creatureData outlives the world), then copy what the record holds.
    for(int i=0;i<MAX_CREATURES_SAVED_MAX;i++)creatureData[i].type=-1;
    {
        size_t n=craw.size();
        if(n>sizeof(EntityData)*(size_t)MAX_CREATURES_SAVED)n=sizeof(EntityData)*(size_t)MAX_CREATURES_SAVED;
        if(n)memcpy(creatureData,&craw[0],n);
    }
    LoadModels2();
    this->loadTrailer(nameIn,traw.empty()?NULL:&traw[0],traw.size());
    return true;
}

// ---- MP.3a: the multiplayer session world (Classes/MpWorld.h; engine plan §2) -----------------
// The `.emod` loader's shape with nothing read: the header is MpWorld's (synthesised from SPAWN, then
// kept up to date by commitSession), seed 0 (the flat recipe = the server's base profile), always 256z
// (World::loadWorld set the height), unbounded, no creatures, no golden cubes.
void FileManager::loadSessionWorld(){
    Terrain* ter=World::getWorld->terrain;
    Player* player=World::getWorld->player;
    const WorldFileHeader* h=(const WorldFileHeader*)MpWorld::header();
    this->clearDirectory();
    this->clearColumnSpans();
    fm_resetGenerator();
    file_version=FILE_VERSION_256Z;
    eden_set_creature_slots(MAX_CREATURES_SAVED_MAX);
    imgHash.clear();
    ter->level_seed=0;
    ter->tgen->LEVEL_SEED=0;
    World::getWorld->hud->goldencubes=0;
    ter->home=h->home;
    player->pos=h->pos;
    player->yaw=h->yaw;
    worldUnbounded=true;
    for(int i=0;i<4;i++)
        for(int j=0;j<4;j++)
            regionSkyColors[i][j]=defaultRegionSkyColors[i][j];
    oldOffsetX=chunkOffsetX;
    oldOffsetZ=chunkOffsetZ;
    chunkOffsetX=windowOriginChunk(player->pos.x);
    chunkOffsetZ=windowOriginChunk(player->pos.z);
    int r=T_RADIUS;
    for(int x=chunkOffsetX;x<chunkOffsetX+2*r;x++){
        for(int z=chunkOffsetZ;z<chunkOffsetZ+2*r;z++){
            readColumn(x,z,NULL);   // seed 0, no directory: generateColumn's flat branch + the overlay hook
            World::getWorld->terrain->counter++;
        }
    }
    for(int i=0;i<MAX_CREATURES_SAVED_MAX;i++)creatureData[i].type=-1;
    LoadModels2();
}

// What saveWorld*/the evict-save do in a session: the header follows the player (a warp's target is its
// pos, as `.emod` stores it), then every modified resident column is diffed into the overlay.
void FileManager::commitSession(Vector warp){
    Terrain* ter=World::getWorld->terrain;
    WorldFileHeader* h=(WorldFileHeader*)MpWorld::header();
    h->pos=warp;
    h->home=ter->home;
    h->yaw=World::getWorld->player->yaw;
    MpWorld::commitModified();
    g_seconds_since_save=0;
}

// ---- Stage D / D.3a: the trailer model ------------------------------------------------------
WorldTrailer* FileManager::trailer(){return &worldTrailer;}

int FileManager::reportTrailerRefusal(int result){
    if(result!=WorldTrailer::TR_OK&&World::getWorld&&World::getWorld->hud&&World::getWorld->hud->sb)
        World::getWorld->hud->sb->setStatus(WorldTrailer::resultMessage(result),3);
    return result;
}

// The world's captured trailer bytes -> the model, then the 2026 game's sidecars. That game keeps a
// local world's signs and command blocks in `signs_<file>.dat` / `cmd_<file>.dat` next to it (one bare
// section each; `<file>` is the world's `.eden` name), and an exported `.eden` does not carry them --
// so a world copied over together with its sidecars would lose them. A sidecar is adopted only for a
// section the trailer does not already have (the trailer wins), which marks the model dirty: the
// first save writes it into the world, after which the sidecar is never read again.
void FileManager::loadTrailer(const std::string& nameIn,const unsigned char* bytes,unsigned long long len){
    adoptedSidecars.clear();
    if(!worldTrailer.load(bytes,(size_t)len))
        printg("trailer: %llu B kept verbatim, not editable: %s\n",len,worldTrailer.why().c_str());
    std::string stem=emod::WorldSource::stemOf(nameIn);
    static const char* const kinds[2]={"signs_","cmd_"};
    for(int k=0;k<2;k++){
        // K.0w: a folder world's sidecars sit in its folder as signs.eden.dat / cmd.eden.dat.
        std::string path=isFolderWorld(nameIn)?docs+"/"+sidecarPathOf(nameIn,k==0?"signs":"cmd")
                                              :docs+"/"+kinds[k]+stem+".eden.dat";
        FILE* f=emod::io::fopen_utf8(path.c_str(),"rb");
        if(!f)continue;
        std::vector<uint8_t> b;
        uint64_t n=emod::io::file_size(f);   // leaves the stream at EOF
        if(n>0&&n<=WorldTrailer::MAX_BYTES&&emod::io::seek_to(f,0)){
            b.resize((size_t)n);
            if(fread(&b[0],1,b.size(),f)!=b.size())b.clear();
        }
        fclose(f);
        int r=b.empty()?WorldTrailer::TR_BAD:worldTrailer.adoptSidecar(&b[0],b.size());
        if(r==WorldTrailer::TR_OK)adoptedSidecars.push_back(path);
        printg("trailer: sidecar %s (%llu B): %s\n",path.substr(docs.size()+1).c_str(),(unsigned long long)n,
               r==WorldTrailer::TR_OK?"adopted":r==WorldTrailer::TR_EXISTS?"ignored (the world has its own)":"not readable");
    }
    printg("trailer: %s\n",worldTrailer.describe().c_str());
}

// K.1-3: the flag compaction at the first allocation, not at the first save (plan §3 "Built in K.1-2":
// "allocate after the flag compaction"). The rewrite reads the file, not the window: every byte >= 128 a
// resident column holds came from that file (the generators and a session write stock ids only), so the
// latent bytes it reserves are all there are, and no id handed out afterwards can collide with one. The
// file is flagged from here on even if the player quits without saving (harmless: an empty or unused
// palette, D8). A one-time rewrite of the whole world on the main thread (K.1-6 measures it on the Air 2).
bool FileManager::paletteFlagged() const{ return emodStore&&emodStore->paletteFlag(); }

bool FileManager::preparePaletteAllocation(std::string* why){
    if(MpWorld::active()){ if(why)*why="Mod blocks are not available in a multiplayer world"; return false; }
    if(!emodStore){ if(why)*why="No saved world is open"; return false; }
    if(emodStore->paletteFlag())return true;
    if(emodStore->damaged()){ if(why)*why="This world's file is damaged, so it cannot take mod blocks (export it first)"; return false; }
    if(!emodStore->compact(true,NULL)){
        fprintf(stderr,"registry: flag compaction failed: %s\n",emodStore->error().c_str());
        if(why)*why="Could not prepare this world for mod blocks: "+emodStore->error();
        return false;
    }
    std::vector<uint8_t> praw;
    if(!emodStore->paletteFlag()||emodStore->readRecord(emod::T_BLOCK_PALETTE,praw)!=emod::EdenWorldStore::READ_OK){
        BlockRegistry::lock();
        if(why)*why="This world's block palette is unreadable: mod blocks are off";
        return false;
    }
    BlockRegistry::loadPalette(praw.empty()?NULL:&praw[0],praw.size(),true);
    printg("registry: flag compaction done (%d latent id(s) reserved)\n",(int)emodStore->latentIds().size());
    return !BlockRegistry::locked();
}

bool FileManager::saveEmodWorld(Vector warp){
    Terrain* ter=World::getWorld->terrain;
    const std::string& name=ter->world_name;
    std::string path=docs+"/"+name;

    if(!emodStore){
        // A brand-new world (loadWorld's generator branch saves it once, straight away): create the
        // file. It only becomes visible at the first commit, which carries header + creatures.
        emod::EdenWorldStore* st=new emod::EdenWorldStore();
        if(isFolderWorld(name))eden_fs_make_dirs(docs+"/"+worldDirOf(name));   // K.0w: the first save makes the folder
        bool ok=worldExists(name,false)
            ?st->open(path.c_str(),true)
            :st->create(path.c_str(),g_world_height>=T_HEIGHT_MAX?16:4,(uint64_t)time(NULL));
        if(!ok){
            std::string why="could not open "+path+": "+st->error();
            delete st;
            return saveFailed(why);
        }
        emodStore=st;
    }

    WorldFileHeader h;
    if(emodHeader)memcpy(&h,emodHeader,sizeof(h)); else memset(&h,0,sizeof(h));
    h.level_seed=ter->level_seed;
    h.goldencubes=World::getWorld->hud->goldencubes;
    h.home=ter->home;
    h.pos=warp;
    h.yaw=World::getWorld->player->yaw;
    for(int i=0;i<4;i++){
        for(int j=0;j<4;j++){
            h.skycolors[i*4+j]=regionSkyColors[i][j];
        }
    }
    WorldNode* sel=World::getWorld->menu->selected_world;
    // G.2b-4: getCString:maxLength: is eden_cstr_copy (D4: empty on overflow). A display name with no
    // UTF-8 form made a nil NSString, which left the header's name as it was.
    std::string selName=sel?std::string(sel->display_name.c_str()):std::string();
    if(sel&&utf8_valid(selName)){
        memset(h.name,0,sizeof(h.name));
        eden_cstr_copy(h.name,49,selName);
    }
    memset(h.hash,0,sizeof(h.hash));
    eden_cstr_copy(h.hash,33,imgHash);
    // Same version rule as the .eden writer: a 64z world is stamped 4, anything else (a >=5 file, or
    // a 256z world under a smaller version -- S.3b) keeps its own, verbatim.
    if(file_version<FILE_VERSION_256Z&&g_world_height<T_HEIGHT_MAX){
        file_version=FILE_VERSION;
    }
    h.version=file_version;

    emodStore->beginBatch();
    // Every resident column with a modified chunk. The flags are cleared only after the commit is
    // durable, so a failed save keeps them and the next trigger retries ("flags survive").
    std::vector<std::pair<int,int> > saved;
    std::vector<uint8_t> col((size_t)CHUNKS_PER_COLUMN*emod::BAND);
    for(int x=0;x<CHUNKS_PER_SIDE;x++){
        for(int z=0;z<CHUNKS_PER_SIDE;z++){
            TerrainChunk* chunk=ter->chunkTable[threeToOne(x,0,z)];
            if(chunk->pbounds[1]!=0){
                printg("trying to save column with unexpected chunk bound[1]: %d\n",chunk->pbounds[1]);
                continue;
            }
            int cx=chunk->pbounds[0]/CHUNK_SIZE,cz=chunk->pbounds[2]/CHUNK_SIZE;
            bool dirty=false;
            for(int cy=0;cy<CHUNKS_PER_COLUMN;cy++){
                if(ter->chunkTable[threeToOne(cx,cy,cz)]->modified){dirty=true;break;}
            }
            if(!dirty||twoToOneTest(cx,cz)==0)continue;
            for(int cy=0;cy<CHUNKS_PER_COLUMN;cy++){
                TerrainChunk* c=ter->chunkTable[threeToOne(cx,cy,cz)];
                memcpy(&col[(size_t)cy*emod::BAND],c->pblocks,CHUNK_SIZE3*sizeof(block8));
                memcpy(&col[(size_t)cy*emod::BAND+CHUNK_SIZE3*sizeof(block8)],c->pcolors,CHUNK_SIZE3*sizeof(color8));
            }
            if(!emodStore->putColumn(cx,cz,&col[0])){
                std::string why=strf("could not stage column (%d,%d): %s",cx,cz,emodStore->error().c_str());
                emodStore->abortBatch();
                return saveFailed(why);
            }
            saved.push_back(std::make_pair(cx,cz));
        }
    }
    SaveModels();
    // QA.5: these two were unchecked; a commit without them would be a silent half-save.
    if(!emodStore->putRecord(emod::T_CREATURES,creatureData,sizeof(EntityData)*(size_t)MAX_CREATURES_SAVED)||
       !emodStore->putRecord(emod::T_WORLD_HEADER,&h,sizeof(h))){
        std::string why="could not stage the header or creatures: "+emodStore->error();
        emodStore->abortBatch();
        return saveFailed(why);
    }
    // Stage D / D.3a: an edited trailer is a new SIGN_TRAILER record in this batch (an empty one
    // removes it). Untouched, nothing is written: the last committed record stays live.
    // H.4: the GENERATOR record, once: a new Classic world's first save, a legacy world's next.
    bool generatorSaving=fm_generatorPending&&ter->tgen->classicState==TerrainGenerator::CLASSIC_STATE_SET&&
        !emodStore->hasRecord(emod::T_GENERATOR);
    if(generatorSaving){
        unsigned char rec[CLASSIC_RECORD_BYTES];
        classic_encode_record(ter->tgen->classic,rec);
        if(!emodStore->putRecord(emod::T_GENERATOR,rec,sizeof(rec))){
            std::string why="could not stage the generator record: "+emodStore->error();
            emodStore->abortBatch();
            return saveFailed(why);
        }
    }
    // DN.2: the day/night clock, when it or its lock moved since the load or the last commit. Exit and
    // warp always commit a batch (header + creatures), so the time sticks with no edits; a world
    // nobody ran the cycle in never gets the record.
    bool clockSaving=SkyCycle::needsSave();
    if(clockSaving){
        uint8_t rec[SkyCycle::RECORD_BYTES];
        SkyCycle::encode(rec);
        if(!emodStore->putRecord(emod::T_SKY_CLOCK,rec,sizeof(rec))){
            std::string why="could not stage the sky clock: "+emodStore->error();
            emodStore->abortBatch();
            return saveFailed(why);
        }
    }
    // AT.1b (plan P7): the weather, the same way: only into a batch that commits anyway, only once it
    // moved past needsSave()'s coarse steps. Weather never marks the world dirty or wakes autosave, and a
    // world nobody ran weather in never gets the record.
    bool weatherSaving=Weather::needsSave();
    if(weatherSaving){
        uint8_t rec[Weather::RECORD_BYTES];
        Weather::encode(rec);
        if(!emodStore->putRecord(emod::T_WEATHER,rec,sizeof(rec))){
            std::string why="could not stage the weather: "+emodStore->error();
            emodStore->abortBatch();
            return saveFailed(why);
        }
    }
    // K.1-3: a palette that gained ids since the load or the last commit. The file is flagged already
    // (preparePaletteAllocation ran before the first id): a dirty palette in an unflagged file is a bug,
    // and the save is refused rather than put a byte >= 128 into a file that does not announce it (D14).
    bool paletteSaving=BlockRegistry::dirty();
    if(paletteSaving){
        std::vector<uint8_t> pb=BlockRegistry::encode();
        if(!emodStore->paletteFlag()||!emodStore->putRecord(emod::T_BLOCK_PALETTE,&pb[0],pb.size())){
            std::string why=std::string("could not stage the block palette: ")+
                    (emodStore->paletteFlag()?emodStore->error().c_str():"file not flagged");
            emodStore->abortBatch();
            return saveFailed(why);
        }
    }
    bool trailerSaving=worldTrailer.dirty();
    if(trailerSaving){
        std::vector<uint8_t> t=worldTrailer.encode();
        if(!emodStore->putRecord(emod::T_SIGN_TRAILER,t.empty()?NULL:&t[0],t.size())){
            std::string why="could not stage the sign trailer: "+emodStore->error();
            emodStore->abortBatch();
            return saveFailed(why);
        }
    }
    if(!emodStore->commitBatch()){
        // The store rolled back to the last commit; the `modified` flags below stay set ("flags survive").
        return saveFailed(emodStore->error());
    }
    saveCommitted();
    if(trailerSaving)trailerSaved();
    if(paletteSaving)BlockRegistry::markSaved();
    if(generatorSaving)fm_generatorPending=false;
    if(clockSaving)SkyCycle::saved();
    if(weatherSaving)Weather::saved();
    g_seconds_since_save=0;
    for(size_t i=0;i<saved.size();i++){
        for(int cy=0;cy<CHUNKS_PER_COLUMN;cy++)
            ter->chunkTable[threeToOne(saved[i].first,cy,saved[i].second)]->modified=false;
    }
    if(!emodHeader)emodHeader=(WorldFileHeader*)malloc(sizeof(WorldFileHeader));
    memcpy(emodHeader,&h,sizeof(h));
    sfh=emodHeader;
    return true;
}

// ---- converting a `.eden` (S.5) -----------------------------------------------------------------
// The player drops a `.eden`, a `.eden.gz`, a `.gz`, a `.zip` or a `.eden.zip` into the saves
// directory; all five are "source worlds". Each is listed, flagged, and converted when played;
// an archive converts WHILE IT INFLATES (emod::WorldSource -> EdenEmodConverter), so no inflated
// `.eden` is ever written. Since S.8 (2026-10-11) this is the only way a `.eden` is played, on every target.
static bool isSourceName(const std::string& name){
    return path_has_suffix(name,".eden")||emod::WorldSource::isArchiveName(name);
}
static unsigned long long fileSizeOf(const std::string& p){
    struct stat sb;
    if(stat(p.c_str(),&sb)!=0)return 0;
    return (unsigned long long)sb.st_size;
}
static long long fileMtimeOf(const std::string& p){
    struct stat sb;
    if(stat(p.c_str(),&sb)!=0)return 0;
    return (long long)sb.st_mtime;
}
static std::string stemOf(const std::string& name){return emod::WorldSource::stemOf(name);}

// X.9: "Convert to 64z" for an `.emod`. Export it as Legacy64z (the exporter restates the cut rules and
// reports every loss) to a scratch `.eden`, convert that back with the 256z upgrade OFF, then replace the
// world with one rename. The original is untouched until the new file is complete and verified.
static ConvertTo64Report convertEmodTo64(const std::string& name){
    ConvertTo64Report report; memset(&report,0,sizeof(report));
    auto fail=[&](const char* m){snprintf(report.error,sizeof(report.error),"%s",m);return report;};
    FileManager* fm=World::getWorld?World::getWorld->fm:NULL;
    if(!fm||!fm->worldExists(name,false))return fail("world does not exist");
    if(World::getWorld->doneLoading!=0&&World::getWorld->terrain&&!World::getWorld->terrain->world_name.empty()
       &&World::getWorld->terrain->world_name==name)
        return fail("cannot convert the world that is currently open -- return to the main menu first");
    const std::string src=docs+"/"+name;
    const std::string tmpEden=src+".64zsrc",tmpEmod=src+".64zconv";
    auto cleanup=[&](){
        remove(tmpEden.c_str());remove(tmpEmod.c_str());
        remove((tmpEmod+".spill").c_str());remove((tmpEmod+".converting").c_str());
    };
    cleanup();

    emod::ExportOptions opt; opt.target=emod::X_LEGACY64Z;
    opt.keepPalette=true;   // K.1-4: mod blocks stay raw; the scratch .eden's BPL1 gives the new world the palette + flag
    emod::ExportReport xr; std::string err;
    {
        emod::EdenWorldStore st;
        if(!st.open(src.c_str(),false))return fail("could not open the world");
        if(st.bands()!=16)return fail("world is already 64z");
    }
    if(!emod::export_world_file(src,tmpEden,opt,false,&xr,&err)){
        cleanup();
        snprintf(report.error,sizeof(report.error),"%s",err.empty()?"export failed":err.c_str());
        return report;
    }
    emod::ImportJob job;
    if(!job.begin(tmpEden,tmpEmod,name,(int64_t)fileMtimeOf(src),false)){
        std::string e=job.error();cleanup();
        snprintf(report.error,sizeof(report.error),"%s",e.c_str());
        return report;
    }
    int r;
    while((r=job.step(50))==emod::ImportJob::RUNNING){}
    if(r!=emod::ImportJob::DONE){
        std::string e=job.error();cleanup();
        snprintf(report.error,sizeof(report.error),"%s",e.empty()?"conversion failed":e.c_str());
        return report;
    }
    remove(tmpEden.c_str());
    if(!emod::io::replace_file(tmpEmod,src)){cleanup();return fail("could not replace the world with the converted file");}
    report.ok=true;
    report.columns=(int)xr.columns;
    report.blocksDiscarded=(int)xr.blocksAbove63;
    report.columnsAffected=(int)xr.columnsAffected;
    report.doorsOrphaned=(int)xr.doorsOrphaned;
    report.creaturesDropped=(int)xr.creaturesDropped;
    report.creaturesRelocated=(int)xr.creaturesRelocated;
    report.creaturesOverflow=(int)xr.creaturesOverflow;
    report.posClamped=xr.posClamped;
    report.homeClamped=xr.homeClamped;
    return report;
}
static unsigned long long le64(const uint8_t* p){
    unsigned long long v=0;
    for(int i=7;i>=0;i--)v=(v<<8)|p[i];
    return v;
}
// Does this `.emod` come from that source? PROVENANCE (docs/emod-file-format.md) records the
// source's size and mtime and its file name. A plain `.eden` pairs by size (the inflated size of an
// archive is unknowable without inflating it); an archive pairs by name + mtime. (Plan §4 also
// allows the header+directory SHA-256, which stays recorded in the file for a stricter check.)
static bool emodPairsWith(const std::string& emodPath,const std::string& srcName,const std::string& srcPath){
    emod::EdenWorldStore st;
    if(!st.open(emodPath.c_str(),false,true,true))return false;   // Q.1 F12: header-only (T_PROVENANCE)
    std::vector<uint8_t> p;
    if(st.readRecord(emod::T_PROVENANCE,p)>emod::EdenWorldStore::READ_FALLBACK||p.size()<80)return false;
    if(!path_has_suffix(srcName,".eden")){
        uint32_t nl=p[76]|(p[77]<<8)|(p[78]<<16)|((uint32_t)p[79]<<24);
        if(p.size()<80+(size_t)nl)return false;
        std::string nm((const char*)&p[80],nl);
        return nm==srcName&&(long long)le64(&p[16])==fileMtimeOf(srcPath);
    }
    return le64(&p[8])==fileSizeOf(srcPath);
}
static std::string pairedEmodPath(const std::string& dir,const std::string& srcName){
    std::string src=dir+"/"+srcName;
    if(fileSizeOf(src)==0)return std::string();
    std::string stem=stemOf(srcName);
    for(int k=1;k<=9;k++){
        std::string cand=dir+"/"+stem+(k==1?std::string():"-"+std::to_string(k))+".emod";
        if(fileSizeOf(cand)==0)continue;
        if(emodPairsWith(cand,srcName,src))return cand;
    }
    return std::string();
}

bool FileManager::edenHasPairedEmod(const std::string& edenNameIn){
    const std::string& edenName=edenNameIn;
    if(isEmodName(edenNameIn)||!isSourceName(edenName))return false;
    return !pairedEmodPath(docs,edenName).empty();
}

std::string FileManager::pairedEmodFor(const std::string& srcNameIn){
    const std::string& srcName=srcNameIn;
    if(isEmodName(srcNameIn)||!isSourceName(srcName))return std::string();
    std::string p=pairedEmodPath(docs,srcName);
    return p.empty()?std::string():fm_ns(p.substr(docs.size()+1));
}

std::string FileManager::originalOf(const std::string& emodName){
    if(isFolderWorld(emodName)){   // K.0w: the folder is the pairing (D4)
        const std::string dir=worldDirOf(emodName);
        std::vector<std::string> names=eden_fs_list(documents+"/"+dir);
        for(size_t i=0;i<names.size();i++)
            if(names[i].compare(0,9,"original.")==0&&isSourceName(names[i]))return dir+"/"+names[i];
        return std::string();
    }
    if(!isEmodName(emodName))return std::string();
    std::vector<std::string> names=eden_fs_list(documents);   // G.2b-4: sorted (Foundation's order was unspecified)
    for(size_t i=0;i<names.size();i++){
        const std::string& f=names[i];
        if(isEmodName(f)||!isSourceName(f))continue;
        std::string e=pairedEmodFor(f);
        if(!e.empty()&&e==emodName)return f;
    }
    return std::string();
}

bool FileManager::removeOriginal(const std::string& emodName){
    const std::string os=originalOf(emodName);
    if(os.empty())return false;
    std::string p=documents+"/"+os;
    bool ok=eden_fs_remove(p);
    printg("emod: %s the original %s\n",ok?"deleted":"could not delete",p.c_str());
    return ok;
}

// ---- K.0w-2: adoption of loose worlds into Worlds/<id>/ (world-folders-plan §4) --------------------
// A loose `<X>.emod` in the Documents root moves into `Worlds/<id>/world.emod` with its neighbours
// (photo, the PROVENANCE-paired original, the 2026 game's signs_/cmd_ sidecars). The world file moves
// by ONE no-replace rename on one volume (the commit point) and is never opened writable, copied or
// rewritten. A kill anywhere leaves either the world loose with some sidecars in a `.migrating` dir
// (rolled back by the next scan) or the world in that dir (rolled forward). No marker: every scan
// adopts whatever loose `.emod` it finds, so a world an older build made is picked up later.
static const char* const kAdoptJournal=".adopt";
static std::map<std::string,std::string> adoptedKeys;   // loose name -> key, this process (the harness maps names)

// EDEN_TEST_ADOPT_KILL_AFTER=<n>: _Exit(86) after the n-th mutating step of adoption or recovery
// (--folders-selftest loops n over every step). Counted across the whole process.
static void adoptStep(const char* what){
    static int limit=-2,count=0;
    if(limit==-2){const char* e=getenv("EDEN_TEST_ADOPT_KILL_AFTER");limit=(e&&*e)?atoi(e):-1;}
    if(limit<0)return;
    if(++count==limit){
        fprintf(stderr,"adopt: test kill after step %d (%s)\n",count,what);
        fflush(stderr);fflush(stdout);
        std::_Exit(86);
    }
}

static std::string fm_freeIdFrom(const std::string& base,bool tryBare){
    if(tryBare&&FileManager::validWorldId(base)&&fm_idFree(docs,base))return base;
    for(;;){
        std::string id=base+"-"+fm_randomLetters(4);
        if(fm_idFree(docs,id))return id;
    }
}

static bool fm_writeJournal(const std::string& dir,const std::vector<std::pair<std::string,std::string> >& moves){
    const std::string tmp=dir+"/"+kAdoptJournal+".tmp",fin=dir+"/"+kAdoptJournal;
    FILE* f=emod::io::fopen_utf8(tmp.c_str(),"wb");
    if(!f)return false;
    bool ok=true;
    for(size_t i=0;i<moves.size();i++)
        ok=fprintf(f,"%s\t%s\n",moves[i].first.c_str(),moves[i].second.c_str())>0&&ok;
    ok=emod::io::sync_file(f)&&ok;
    ok=fclose(f)==0&&ok;
    ok=ok&&eden_fs_move_new(tmp,fin);
    if(!ok)eden_fs_remove(tmp);
    return ok;
}

// The journal of a `.migrating` dir: root name -> folder name. false when absent or unreadable.
static bool fm_readJournal(const std::string& dir,std::vector<std::pair<std::string,std::string> >* moves){
    FILE* f=emod::io::fopen_utf8((dir+"/"+kAdoptJournal).c_str(),"rb");
    if(!f)return false;
    std::string all;
    char buf[4096];size_t n;
    while((n=fread(buf,1,sizeof buf,f))>0)all.append(buf,n);
    fclose(f);
    size_t at=0;
    while(at<all.size()){
        size_t nl=all.find('\n',at);
        if(nl==std::string::npos)return false;            // a torn last line: not ours
        std::string line=all.substr(at,nl-at);at=nl+1;
        size_t tab=line.find('\t');
        if(tab==std::string::npos||tab==0||tab+1>=line.size())return false;
        std::string r=line.substr(0,tab),d=line.substr(tab+1);
        if(r.find('/')!=std::string::npos||r.find('\\')!=std::string::npos||d.find('/')!=std::string::npos
           ||d.find('\\')!=std::string::npos||d==kWorldFile||r==".."||d=="..")return false;
        moves->push_back(std::make_pair(r,d));
    }
    return true;
}

// Rolls a `.migrating` dir without its world back: every journalled sidecar still in the dir moves back
// to its root name (no-replace). The dir goes only when nothing but the journal is left in it.
static bool fm_rollBack(const std::string& dir){
    std::vector<std::pair<std::string,std::string> > moves;
    const bool haveJournal=fm_readJournal(dir,&moves);
    if(haveJournal){
        for(size_t i=0;i<moves.size();i++){
            const std::string in=dir+"/"+moves[i].second,out=docs+"/"+moves[i].first;
            if(!eden_fs_exists(in))continue;
            if(!eden_fs_move_new(in,out)){
                fprintf(stderr,"adopt: cannot move %s back to %s (taken?); %s left as is\n",in.c_str(),out.c_str(),dir.c_str());
                return false;
            }
            adoptStep("roll back a sidecar");
        }
        eden_fs_sync_dir(docs);
    }
    // Only the journal (or its unrenamed .tmp) may be left; anything else is not ours to delete.
    std::vector<std::string> left=eden_fs_list(dir);
    for(size_t i=0;i<left.size();i++){
        if(left[i]==kAdoptJournal||left[i]==std::string(kAdoptJournal)+".tmp")continue;
        fprintf(stderr,"adopt: %s %s; left as is\n",dir.c_str(),haveJournal?"still holds files after the roll back":"has no readable journal");
        return false;
    }
    eden_fs_remove(dir+"/"+kAdoptJournal+".tmp");
    eden_fs_remove(dir+"/"+kAdoptJournal);
    adoptStep("roll back: journal removed");
    bool ok=eden_fs_remove(dir);
    adoptStep("roll back: dir removed");
    return ok;
}

// Rolls a `.migrating` dir that holds its world forward: drop the journal, then rename it to `<id>`
// (a fresh id when that is taken). Returns the new key, or "" when it stays (logged).
static std::string fm_rollForward(const std::string& dirName,const std::string& id){
    const std::string wroot=docs+"/"+kWorldsDir,dir=wroot+"/"+dirName;
    eden_fs_remove(dir+"/"+kAdoptJournal+".tmp");
    if(eden_fs_exists(dir+"/"+kAdoptJournal)){
        eden_fs_remove(dir+"/"+kAdoptJournal);
        adoptStep("journal removed");
    }
    std::string nid=id;
    for(int tries=0;tries<8;tries++){
        if(eden_fs_move_new(dir,wroot+"/"+nid)){
            adoptStep("dir renamed");
            eden_fs_sync_dir(wroot);
            return FileManager::worldKeyOfId(nid);
        }
        nid=fm_freeIdFrom(id,false);
    }
    fprintf(stderr,"adopt: cannot rename %s to a world folder; retried next scan\n",dir.c_str());
    return std::string();
}

static bool fm_adoptBusy(){
    if((single&&single->conversionActive()))return true;
    return WorldBrowser::anyBusy();   // not via World::getWorld->menu: Menu's constructor scans before it is set
}

int FileManager::adoptLooseWorlds(std::vector<std::pair<std::string,std::string> >* moved){
    if(fm_adoptBusy())return 0;
    const std::string wroot=docs+"/"+kWorldsDir;
    int adopted=0;

    // 1. Recover every `.<id>.migrating` a kill or a failure left.
    if(eden_fs_is_dir(wroot)){
        std::vector<std::string> ents=eden_fs_list(wroot);
        for(size_t i=0;i<ents.size();i++){
            const std::string& e=ents[i];
            static const std::string suf=".migrating";
            if(e.size()<=1+suf.size()||e[0]!='.'||!path_has_suffix(e,suf.c_str()))continue;
            const std::string id=e.substr(1,e.size()-1-suf.size());
            const std::string dir=wroot+"/"+e;
            if(!validWorldId(id)||!eden_fs_is_dir(dir))continue;
            if(eden_fs_exists(dir+"/"+kWorldFile)){
                std::string key=fm_rollForward(e,id);
                if(!key.empty()){
                    printg("adopt: recovered %s -> %s\n",e.c_str(),key.c_str());
                    adopted++;
                }
            }else if(fm_rollBack(dir)){
                printg("adopt: rolled back %s\n",e.c_str());
            }
        }
    }

    // 2. Adopt every loose `.emod` in the root, except the world that is open.
    std::string openPath=emodStore?emodStore->path():std::string();
    std::string openName;
    if(World::getWorld&&World::getWorld->terrain)openName=World::getWorld->terrain->world_name;
    std::vector<std::string> names=eden_fs_list(docs);
    for(size_t i=0;i<names.size();i++){
        const std::string X=names[i];
        if(!isEmodName(X)||X.size()<=strlen(EMOD_WORLD_EXTENSION)||X[0]=='.')continue;
        const std::string src=docs+"/"+X;
        if(X==openName||src==openPath||eden_fs_is_dir(src))continue;

        // §4.1 Probe: header only. A refused file stays loose (and still lists).
        uint64_t created=0;
        {
            emod::EdenWorldStore st;
            if(!st.open(src.c_str(),false,true,true)){
                fprintf(stderr,"adopt: %s is not a readable .emod (%s); left loose\n",X.c_str(),st.error().c_str());
                continue;
            }
            created=st.createdUnix();
        }   // closed here: Windows cannot move an open file

        // §4.2 The id.
        const std::string stem=X.substr(0,X.size()-strlen(EMOD_WORLD_EXTENSION));
        bool digits=!stem.empty()&&stem.size()<=20;
        for(size_t k=0;k<stem.size()&&digits;k++)digits=stem[k]>='0'&&stem[k]<='9';
        std::string id;
        if(digits)id=fm_freeIdFrom(stem,true);
        else{
            long long t=(long long)created,now=(long long)time(NULL);
            if(t<=0||t>now)t=fileMtimeOf(src);
            if(t<=0)t=now;
            id=fm_freeIdFrom(std::to_string(t),false);
        }

        // §4.3 The neighbours (root name -> folder name), and the stale temps deleteWorld would remove.
        std::vector<std::pair<std::string,std::string> > moves;
        auto plan=[&](const std::string& root,const std::string& in){
            if(root.find('\t')!=std::string::npos||root.find('\n')!=std::string::npos)return;
            if(eden_fs_exists(docs+"/"+root)&&!eden_fs_is_dir(docs+"/"+root))moves.push_back(std::make_pair(root,in));
        };
        plan(X+".png","photo.png");
        {
            const std::string orig=originalOf(X);   // PROVENANCE pairing, its last use for this world
            if(!orig.empty())plan(orig,"original"+orig.substr(stemOf(orig).size()));
        }
        static const char* const kKinds[2]={"signs","cmd"};
        for(int k=0;k<2;k++){
            const std::string a=std::string(kKinds[k])+"_"+stem+".eden.dat",b=std::string(kKinds[k])+".eden.dat";
            plan(a,b);
            plan(a+".adopted",b+".adopted");
        }
        static const char* const kStale[]={".savejrnl",".converting",".spill",".creating",".compact"};
        bool removedStale=false;
        for(size_t k=0;k<sizeof(kStale)/sizeof(kStale[0]);k++){
            const std::string t=src+kStale[k];
            if(eden_fs_exists(t)&&!eden_fs_is_dir(t)){eden_fs_remove(t);removedStale=true;}
        }
        if(removedStale)adoptStep("stale temps removed");

        // §4.4 The dir and its journal.
        const std::string mdirName="."+id+".migrating",mdir=wroot+"/"+mdirName;
        if(!eden_fs_make_dirs(wroot)||!eden_fs_make_dir(mdir)){
            fprintf(stderr,"adopt: cannot make %s; %s left loose\n",mdir.c_str(),X.c_str());
            continue;
        }
        adoptStep("dir made");
        if(!fm_writeJournal(mdir,moves)){
            fprintf(stderr,"adopt: cannot write the journal in %s; %s left loose\n",mdir.c_str(),X.c_str());
            fm_rollBack(mdir);
            continue;
        }
        eden_fs_sync_dir(mdir);
        eden_fs_sync_dir(wroot);
        adoptStep("journal written");

        // §4.5 Sidecars in (no-replace).
        bool ok=true;
        for(size_t k=0;k<moves.size()&&ok;k++){
            ok=eden_fs_move_new(docs+"/"+moves[k].first,mdir+"/"+moves[k].second);
            if(ok)adoptStep("sidecar moved");
            else fprintf(stderr,"adopt: cannot move %s into %s\n",moves[k].first.c_str(),mdirName.c_str());
        }
        // §4.6 The commit point: the world file, one rename.
        if(ok){
            ok=eden_fs_move_new(src,mdir+"/"+kWorldFile);
            if(!ok)fprintf(stderr,"adopt: cannot move %s into %s\n",X.c_str(),mdirName.c_str());
        }
        if(!ok){
            eden_fs_sync_dir(docs);
            if(!fm_rollBack(mdir))fprintf(stderr,"adopt: roll back of %s incomplete; retried next scan\n",mdirName.c_str());
            fprintf(stderr,"adopt: %s left loose\n",X.c_str());
            continue;
        }
        eden_fs_sync_dir(mdir);
        eden_fs_sync_dir(docs);
        adoptStep("world moved");

        // §4.7 Journal out, dir renamed to <id>.
        std::string key=fm_rollForward(mdirName,id);
        eden_fs_sync_dir(docs);
        if(key.empty())continue;   // the world is safe in the .migrating dir; the next scan finishes it
        adoptedKeys[X]=key;
        if(moved)moved->push_back(std::make_pair(X,key));
        printg("adopt: %s -> %s (%d neighbour%s)\n",X.c_str(),key.c_str(),(int)moves.size(),moves.size()==1?"":"s");
        adopted++;
    }
    return adopted;
}

std::string FileManager::adoptedKeyOf(const std::string& looseName){
    std::map<std::string,std::string>::const_iterator it=adoptedKeys.find(looseName);
    return it==adoptedKeys.end()?looseName:it->second;
}

bool FileManager::worldNeedsConversion(const std::string& nameIn){
    const std::string& name=nameIn;
    if(isEmodName(nameIn)||!isSourceName(name))return false;
    if(fileSizeOf(docs+"/"+nameIn)<sizeof(WorldFileHeader))return false;
    return pairedEmodPath(docs,name).empty();
}

// One conversion at a time, a time slice per convertStep() call. The work is emod::ImportJob
// (Classes/EdenWorldSource.h), the same job Get Worlds and the web import run: the pre-flight, the
// running free-space floor and the 500 MB temp cap live there, once.
struct EmodConvJob{
    emod::ImportJob job;
    std::string outPath,srcPath;
    std::string outName;   // G.2b-4: was a retained NSString*
    std::string id;        // K.0w-3: the staging id (Worlds/.<id>.partial)
};
static EmodConvJob* convJob=NULL;
static std::string convError;

bool FileManager::conversionActive(){return convJob!=NULL;}

// ---- S.5e: the height a conversion writes ---------------------------------------------------------
// Every `.emod` is 256z by default (the user, 2026-10-10; plan §10 Q9): the Settings toggle decides,
// and the prompt below lets the player keep one world 64z. One prompt at a time, for one source.
extern "C" int eden_get_upgrade_256z(void);   // port/src/seam/Settings.cpp
static bool heightAsked=false;        // G.2b-4: heightAskName set (it was a retained NSString*, nil when not)
static std::string heightAskName;     // the source the prompt is (or was) about
static int heightChoice=-3;           // -3 not asked, -2 waiting, -1 cancelled, 0 keep, 1 upgrade
static void clearHeightChoice(){heightAsked=false;heightAskName.clear();heightChoice=-3;}

bool FileManager::upgradeOnConvert(){return eden_get_upgrade_256z()!=0;}

// 4 or 16 from a plain `.eden`'s header + directory (the probeWorldHeight rule); 0 when it cannot
// tell without inflating (an archive) or reading (a short file).
static int sourceBands(const std::string& path){
    if(emod::WorldSource::isArchiveName(path))return 0;
    FILE* f=emod::io::fopen_utf8(path.c_str(),"rb");
    if(!f)return 0;
    uint8_t h[emod::EDEN_HEADER];
    int bands=0;
    unsigned long long size=emod::io::file_size(f);   // leaves the stream at EOF
    if(emod::io::seek_to(f,0)&&fread(h,1,sizeof h,f)==sizeof h){
        unsigned long long dirOff=le64(h+32);
        if(dirOff>=sizeof h&&dirOff<=size&&size-dirOff<=(256ull<<20)){
            std::vector<uint8_t> dir((size_t)(size-dirOff));
            if(emod::io::seek_to(f,dirOff)&&(dir.empty()||fread(&dir[0],1,dir.size(),f)==dir.size()))
                bands=emod::eden_detect_bands_raw(h,dir.empty()?NULL:&dir[0],dir.size());
        }
    }
    fclose(f);
    return bands;
}

int FileManager::convertHeightGate(const std::string& nameIn){
    const std::string& name=nameIn;
    if(convJob)return 1;
    if(heightAsked&&heightAskName!=name)clearHeightChoice();
    if(heightChoice>=0)return 1;
    if(heightChoice==-1){clearHeightChoice();return -1;}
    if(heightChoice==-2)return 0;
    if(!upgradeOnConvert())return 1;                     // the toggle is off: every world keeps its height
    const int bands=sourceBands(docs+"/"+name);
    if(bands==16)return 1;                               // nothing to choose
    const char* e=getenv("EDEN_CONVERT_PROMPT");
    if(e&&e[0]=='0')return 1;                            // a harness: the toggle decides
    heightAsked=true;
    heightAskName=name;
    heightChoice=-2;
    showAlertConvertHeight(nameIn.c_str(),bands==4);
    return 0;
}

void FileManager::answerConvertHeight(int choice){
    if(heightChoice==-2)heightChoice=choice<0?-1:(choice?1:0);
}
std::string FileManager::conversionError(){return convError;}

void FileManager::cleanConversionTemps(){
    if(convJob)return;
    // K.0w-3: a killed import's staging dir. Never while a convert / download / import is live (fm_adoptBusy),
    // and never a `.migrating` dir (adoption's recovery owns those).
    if(!fm_adoptBusy()){
        const std::string wroot=documents+"/"+kWorldsDir;
        std::vector<std::string> ents=eden_fs_list(wroot);
        for(size_t i=0;i<ents.size();i++){
            static const std::string suf=".partial";
            const std::string& e=ents[i];
            if(e.size()>1+suf.size()&&e[0]=='.'&&path_has_suffix(e,suf.c_str())&&eden_fs_is_dir(wroot+"/"+e)){
                eden_fs_remove_tree(wroot+"/"+e);
                printg("emod: removed the stale staging dir %s\n",e.c_str());
            }
        }
    }
    std::vector<std::string> names=eden_fs_list(documents);
    for(size_t i=0;i<names.size();i++){
        const std::string& f=names[i];
        // S.5: also a killed import's archive layer and a killed export/upload's partial output.
        if(path_has_suffix(f,".emod.converting")||path_has_suffix(f,".emod.spill")||path_has_suffix(f,".emod.creating")||path_has_suffix(f,".emod.compact")
           ||f.find(".emod.layer")!=std::string::npos||path_has_suffix(f,".exporting")||path_has_suffix(f,".upload-body")){
            eden_fs_remove(documents+"/"+f);
        }
    }
}

int FileManager::convertStep(const std::string& nameIn,int* percent,std::string* outName){
    const std::string& name=nameIn;
    if(percent)*percent=0;

    if(!convJob){
        convError.clear();
        if(!worldNeedsConversion(nameIn)){convError="nothing to convert";return -1;}
        EmodConvJob* j=new EmodConvJob();
        j->srcPath=docs+"/"+name;
        // A fresh stem unless this one is taken by a `.emod` of some OTHER source (a different
        // `.eden` re-imported under the same name lists separately and converts beside the first).
        // K.0w-3 (plan §5.2): the output goes to Worlds/.<id>.partial/world.emod and the dir is renamed to
        // Worlds/<id> when the job is done, so a source never needs a fresh stem any more.
        j->id=newWorldId(docs);
        const std::string stage=beginStaging(docs,j->id);
        if(stage.empty()){
            convError="cannot make the staging folder";
            delete j;
            return -1;
        }
        j->outPath=stage+"/"+kWorldFile;
        // S.5e: the prompt's answer for this world, else the Settings toggle. Consumed here.
        bool up=(heightAsked&&heightAskName==name&&heightChoice>=0)?(heightChoice==1):upgradeOnConvert();
        clearHeightChoice();
        if(!j->job.begin(j->srcPath,j->outPath,name,(int64_t)fileMtimeOf(j->srcPath),up)){
            convError=j->job.error();
            printg("emod: cannot convert %s: %s\n",j->srcPath.c_str(),convError.c_str());
            discardStaging(docs,j->id);
            delete j;
            return -1;
        }
        convJob=j;
        printg("emod: converting %s -> %s\n",j->srcPath.c_str(),j->outPath.c_str());
    }
    EmodConvJob* j=convJob;
    // ~12 ms of work, then back to the loading screen so it keeps drawing.
    const int r=j->job.step(12);
    if(r==emod::ImportJob::FAILED){
        convError=j->job.error();
        printg("emod: conversion of %s failed: %s\n",j->srcPath.c_str(),convError.c_str());
        discardStaging(docs,j->id);
        delete j;convJob=NULL;
        return -1;
    }
    if(r==emod::ImportJob::RUNNING){
        if(percent)*percent=j->job.percent();
        return 0;
    }
    // The converter committed Worlds/.<id>.partial/world.emod; the source was only ever read.
    stageStep("converted");
    const std::string key=commitStaging(docs,j->id);   // the commit point
    if(key.empty()){
        convError="cannot move the converted world into place";
        printg("emod: conversion of %s: %s\n",j->srcPath.c_str(),convError.c_str());
        discardStaging(docs,j->id);
        delete j;convJob=NULL;
        return -1;
    }
    j->outName=key;
    // Neighbours move in AFTER the commit, sidecars first and the source last: a kill in between leaves the
    // world listed and the source still loose (converts again as a second world; nothing is lost). All
    // no-replace; a failed move leaves that file where it was.
    const std::string dir=worldDirOf(key);
    const std::string stem=stemOf(name);
    auto into=[&](const std::string& rootName,const std::string& inName)->bool{
        const std::string from=docs+"/"+rootName;
        if(!eden_fs_exists(from)||eden_fs_is_dir(from))return false;
        bool ok=eden_fs_move_new(from,docs+"/"+dir+"/"+inName);
        if(ok)stageStep("neighbour moved");
        else printg("emod: could not move %s into %s\n",rootName.c_str(),dir.c_str());
        return ok;
    };
    static const char* const kKinds[2]={"signs","cmd"};
    for(int k=0;k<2;k++){
        const std::string a=std::string(kKinds[k])+"_"+stem+".eden.dat",b=std::string(kKinds[k])+".eden.dat";
        into(a,b);
        into(a+".adopted",b+".adopted");
    }
    into(name+".png","photo.png");
    const std::string origIn="original"+name.substr(stem.size());
    const bool movedSrc=into(name,origIn);
    if(outName)*outName=key;
    if(percent)*percent=100;
    if(movedSrc){
        pendingOriginalName=nameIn;                 // the prompt's label
        pendingOriginalPath=dir+"/"+origIn;         // what resolveOriginal(TRUE) deletes
    }
    printg("emod: converted %s (%llu B, %d bands) -> %s (%llu B, %d bands)\n",j->srcPath.c_str(),
           movedSrc?fileSizeOf(docs+"/"+pendingOriginalPath):fileSizeOf(j->srcPath),
           j->job.stats().bandsIn,key.c_str(),fileSizeOf(docs+"/"+key),j->job.stats().bandsOut);
    delete j;convJob=NULL;
    return 1;
}

std::string FileManager::pendingOriginal(){return pendingOriginalName;}

void FileManager::resolveOriginal(bool deleteIt){
    if(pendingOriginalName.empty())return;
    if(deleteIt){
        std::string p=documents+"/"+(pendingOriginalPath.empty()?pendingOriginalName:pendingOriginalPath);
        if(eden_fs_remove(p)){
            printg("emod: deleted the original %s\n",p.c_str());
        }else{
            printg("emod: could not delete the original %s\n",p.c_str());
        }
    }
    pendingOriginalName.clear();
    pendingOriginalPath.clear();
}

// ---- H.15: the world list's Properties screen ---------------------------------------------------
// world-properties-plan.md §1 and §3. Both run on a CLOSED world from the main menu (the screen is
// not offered in game), so nothing re-streams; the store is still shared when it happens to be the
// open one, as emodSetHeaderName does.

// §3.4: the H.4 spawn rule on one block column of the new terrain -- the first two air cells
// above its top dirt (a tree or a cloud over it pushes the feet up past it). A cave mouth at the
// surface drops to the cave's floor first, so the cell under the feet is never air.
static int fm_classicStandY(const ClassicGen& g,int wx,int wz,int H){
    static block8 types[CHUNK_SIZE*CHUNK_SIZE*T_HEIGHT_MAX];
    static color8 colors[CHUNK_SIZE*CHUNK_SIZE*T_HEIGHT_MAX];
    const int bx=wx-(wx&(CHUNK_SIZE-1)),bz=wz-(wz&(CHUNK_SIZE-1));
    classic_generate_window(g,bx,bz,CHUNK_SIZE,CHUNK_SIZE,H,types,colors,true);
    const block8* col=&types[((wx-bx)*CHUNK_SIZE+(wz-bz))*H];
    int top=classic_surface_top(g,wx,wz,H);
    if(top<0)top=classic_height(g,wx,wz,H)-1;
    int y=top+1;
    if(y<1)y=1;
    if(y>H-2)y=H-2;
    while(y>1&&col[y-1]==TYPE_NONE)y--;
    while(y<H-2&&(col[y]!=TYPE_NONE||col[y+1]!=TYPE_NONE))y++;
    return y;
}
// `home` is the cell the feet stand in; the player's `pos` is one block higher (the new-world
// branch of loadWorld: pos = home + (.5, 1, .5)). Saved land is left alone.
static bool fm_liftToSurface(const ClassicGen& g,emod::EdenWorldStore* st,Vector* v,int H,bool isPos){
    const int wx=(int)floorf(v->x/BLOCK_SIZE),wz=(int)floorf(v->z/BLOCK_SIZE);
    if(st->hasColumn(wx>>4,wz>>4))return false;
    const int y=fm_classicStandY(g,wx,wz,H);
    const float ny=isPos?BLOCK_SIZE*(y+1):(float)y;
    if(v->y==ny)return false;
    v->y=ny;
    return true;
}

bool FileManager::readWorldProperties(const std::string& file_name,WorldProps* out){
    const std::chrono::steady_clock::time_point t0=std::chrono::steady_clock::now();
    WorldProps p;
    p.kind=WP_KIND_EMOD; p.seed=0; p.height=T_HEIGHT_DEFAULT; p.version=0; p.type=WP_TYPE_NORMAL;
    p.genSource=WP_GEN_RECORD; memset(&p.gen,0,sizeof(p.gen));
    p.originalBytes=0; p.createdUnix=0; p.columns=-1; p.signs=-1; p.goldencubes=-1; p.convertedVersion=0;
    p.openMs=0;
    p.skyLocked=false; p.weatherLocked=false; p.skyT=SkyCycle::DEFAULT_TIME;
    const std::string fn=file_name,path=docs+"/"+fn;
    p.fileBytes=fileSizeOf(path);
    p.savedUnix=fileMtimeOf(path);
    WorldFileHeader h;
    memset(&h,0,sizeof(h));
    emod::EdenWorldStore st;
    bool haveStore=false;
    if(isEmodName(file_name)){
        if(!st.open(path.c_str(),false,true,true))return false;   // X.12: header-only scan (Quarry on the Air 2: ~755 ms before)
        haveStore=true;
        std::vector<uint8_t> raw;
        if(st.readRecord(emod::T_WORLD_HEADER,raw)>emod::EdenWorldStore::READ_FALLBACK||raw.size()!=sizeof(h))return false;
        memcpy(&h,&raw[0],sizeof(h));
        p.height=st.bands()*CHUNK_SIZE;
        p.columns=(int)st.columnCount();
        p.createdUnix=(long long)st.createdUnix();
        std::vector<uint8_t> kraw;   // DN.6: the clock, for Lock time of day
        if(st.hasRecord(emod::T_SKY_CLOCK)&&st.readRecord(emod::T_SKY_CLOCK,kraw)<=emod::EdenWorldStore::READ_FALLBACK&&!kraw.empty())
            SkyCycle::decode(&kraw[0],kraw.size(),&p.skyT,&p.skyLocked);
        std::vector<uint8_t> wraw;   // AT.1c: the weather, for Lock weather
        if(st.hasRecord(emod::T_WEATHER)&&st.readRecord(emod::T_WEATHER,wraw)<=emod::EdenWorldStore::READ_FALLBACK){
            Weather::State ws;
            if(Weather::decode(wraw.empty()?NULL:&wraw[0],wraw.size(),&ws))p.weatherLocked=(ws.flags&Weather::FLAG_LOCKED)!=0;
        }
        std::vector<uint8_t> traw;
        p.signs=0;
        if(st.hasRecord(emod::T_SIGN_TRAILER)&&st.readRecord(emod::T_SIGN_TRAILER,traw)<=emod::EdenWorldStore::READ_FALLBACK&&!traw.empty()){
            WorldTrailer t;
            if(t.load(&traw[0],traw.size()))p.signs=(int)t.signCount();
        }
        // PROVENANCE (docs/emod-file-format.md): source version @56, name_len @76, the name @80.
        std::vector<uint8_t> pr;
        if(st.hasRecord(emod::T_PROVENANCE)&&st.readRecord(emod::T_PROVENANCE,pr)<=emod::EdenWorldStore::READ_FALLBACK&&pr.size()>=80){
            const uint32_t nl=pr[76]|(pr[77]<<8)|(pr[78]<<16)|((uint32_t)pr[79]<<24);
            if(pr.size()>=80+(size_t)nl){
                p.convertedFrom.assign((const char*)&pr[80],nl);
                p.convertedVersion=(int)(pr[56]|(pr[57]<<8)|(pr[58]<<16)|((uint32_t)pr[59]<<24));
            }
        }
        const std::string o=originalOf(file_name);
        if(!o.empty())p.originalBytes=fileSizeOf(docs+"/"+o);
    }else if(isArchiveName(file_name)){
        p.kind=WP_KIND_ARCHIVE;
        if(!peekArchiveHeader(path,&h))return false;
        p.height=(h.version>=FILE_VERSION_256Z)?T_HEIGHT_MAX:T_HEIGHT_DEFAULT;   // the version rule only: no directory to probe
    }else{
        p.kind=WP_KIND_EDEN;
        FILE* f=fopen(path.c_str(),"rb");   // "rb": see getName
        if(!f)return false;
        const bool ok=fread(&h,sizeof(h),1,f)==1;
        fclose(f);
        if(!ok)return false;
        p.height=probeWorldHeight(file_name,false);
    }
    h.name[49]=0;
    p.name=h.name;
    p.seed=h.level_seed;
    p.version=h.version;
    p.goldencubes=(h.version==3)?10:h.goldencubes;   // the v3 -> v4 upgrade's value (loadEmodWorld)
    p.pos=h.pos;
    p.home=h.home;
    p.type=p.seed==0?WP_TYPE_FLAT:p.seed==DEFAULT_LEVEL_SEED?WP_TYPE_NORMAL:WP_TYPE_CLASSIC;
    if(p.type==WP_TYPE_CLASSIC)p.genSource=fm_resolveGenerator(haveStore?&st:NULL,&h,p.seed,&p.gen);
    p.openMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-t0).count();
    *out=p;
    return true;
}

// --props-selftest's mutation leg (gate 1): the GENERATOR append is skipped, which the round trip must catch.
bool g_wp_test_skip_generator=false;
int FileManager::applyWorldProperties(const std::string& file_name,const WorldProps& e){
    WorldProps cur;
    if(!readWorldProperties(file_name,&cur))return -1;
    const bool nameChanged=!e.name.empty()&&e.name!=cur.name;
    if(cur.kind!=WP_KIND_EMOD){
        // Only the name is editable on an unconverted world, and not inside an archive (§3.5).
        if(!nameChanged)return 0;
        if(cur.kind==WP_KIND_ARCHIVE)return -1;
        return renameWorld(file_name,e.name)?WP_WROTE_NAME:-1;
    }
    ClassicGen g=cur.gen;
    bool genChanged=false,writeGen=false;
    if(cur.type==WP_TYPE_CLASSIC&&cur.genSource!=WP_GEN_VOID){
        // The seed, the profile and (on a legacy or 64z world) the scale are not the screen's to
        // change; the detected flag stands only while the caves are the detected ones.
        g=e.gen;
        g.seed=cur.seed;
        g.profile=cur.gen.profile;
        if(cur.gen.profile==CLASSIC_PROFILE_LEGACY||cur.height<T_HEIGHT_MAX)g.scale4=cur.gen.scale4;
        g.flags=(g.caves==cur.gen.caves)?cur.gen.flags:0;
        unsigned char a[CLASSIC_RECORD_BYTES],b[CLASSIC_RECORD_BYTES];
        classic_encode_record(cur.gen,a);
        classic_encode_record(g,b);
        genChanged=memcmp(a,b,sizeof(a))!=0;
        writeGen=genChanged||cur.genSource!=WP_GEN_RECORD;
    }
    const bool lockChanged=e.skyLocked!=cur.skyLocked;   // DN.6: Lock time of day
    const bool wLockChanged=e.weatherLocked!=cur.weatherLocked;   // AT.1c: Lock weather
    if(!nameChanged&&!writeGen&&!lockChanged&&!wLockChanged)return 0;
    const std::string path=docs+"/"+file_name;
    emod::EdenWorldStore own;
    emod::EdenWorldStore* st=(emodStore&&emodStore->path()==path)?emodStore:&own;
    if(st==&own&&!own.open(path.c_str(),true))return -1;
    std::vector<uint8_t> raw;
    if(st->readRecord(emod::T_WORLD_HEADER,raw)>emod::EdenWorldStore::READ_FALLBACK||raw.size()!=sizeof(WorldFileHeader))return -1;
    WorldFileHeader h;
    memcpy(&h,&raw[0],sizeof(h));
    int res=0;
    if(nameChanged){
        memset(h.name,0,sizeof(h.name));
        strncpy(h.name,e.name.c_str(),sizeof(h.name)-1);
        res|=WP_WROTE_NAME;
    }
    if(genChanged){
        const int H=st->bands()*CHUNK_SIZE;
        if(fm_liftToSurface(g,st,&h.home,H,false))res|=WP_LIFTED_HOME;
        if(fm_liftToSurface(g,st,&h.pos,H,true))res|=WP_LIFTED_POS;
    }
    st->beginBatch();
    bool ok=true;
    if(res)ok=st->putRecord(emod::T_WORLD_HEADER,&h,sizeof(h));
    if(ok&&writeGen&&!g_wp_test_skip_generator){
        unsigned char rec[CLASSIC_RECORD_BYTES];
        classic_encode_record(g,rec);
        ok=st->putRecord(emod::T_GENERATOR,rec,sizeof(rec));
    }
    if(writeGen)res|=WP_WROTE_GENERATOR;
    if(ok&&lockChanged){
        // The stored time stands; only the lock bit moves. An open world's clock is the live one.
        uint8_t krec[SkyCycle::RECORD_BYTES];
        if(st==emodStore){ SkyCycle::setLocked(e.skyLocked); SkyCycle::encode(krec); }
        else{
            memset(krec,0,sizeof(krec));
            const float tf=(float)cur.skyT;
            memcpy(krec,&tf,4);
            krec[4]=e.skyLocked?SkyCycle::FLAG_LOCKED:0;
        }
        ok=st->putRecord(emod::T_SKY_CLOCK,krec,sizeof(krec));
        if(ok)res|=WP_WROTE_SKY;
    }
    if(ok&&wLockChanged){
        // Same shape as the sky lock: an open world's state is the live one; else the stored record with the bit moved.
        uint8_t wrec[Weather::RECORD_BYTES];
        bool have=false;
        if(st==emodStore){ Weather::setLocked(e.weatherLocked); Weather::encode(wrec); have=true; }
        else{
            std::vector<uint8_t> wr;
            if(st->hasRecord(emod::T_WEATHER)&&st->readRecord(emod::T_WEATHER,wr)<=emod::EdenWorldStore::READ_FALLBACK&&
               wr.size()==sizeof(wrec)){ memcpy(wrec,&wr[0],sizeof(wrec)); have=true; }
            else{   // no record yet: a clear spell, no time to speak of, locked -- decode() accepts it
                memset(wrec,0,sizeof(wrec));
                const float one=1.0f;
                memcpy(wrec+8,&one,4);
                have=true;
            }
            if(e.weatherLocked)wrec[1]|=Weather::FLAG_LOCKED; else wrec[1]&=~Weather::FLAG_LOCKED;
        }
        if(have){
            ok=st->putRecord(emod::T_WEATHER,wrec,sizeof(wrec));
            if(ok)res|=WP_WROTE_WEATHER;
        }
    }
    if(!ok){
        printg("applyWorldProperties: could not stage the records: %s\n",st->error().c_str());
        st->abortBatch();
        return -1;
    }
    if(!st->commitBatch()){
        printg("applyWorldProperties: commit failed (%s) -- nothing changed\n",st->error().c_str());
        return -1;
    }
    if(st==emodStore&&emodHeader)memcpy(emodHeader,&h,sizeof(h));
    if(st==emodStore&&(res&WP_WROTE_SKY))SkyCycle::saved();
    if(st==emodStore&&(res&WP_WROTE_WEATHER))Weather::saved();
    return res;
}
