//
//  FileManager.m
//  prototype
//
//  Created by Ari Ronen on 10/25/10.
//  Copyright 2010 __MyCompanyName__. All rights reserved.
//

#import "FileManager.h"
#import "hashmap.h"
#import "Util.h"
#import "Terrain.h"
#import "Model.h"
#import "TerrainGen2.h"
#import "FileArchive.h"
#import "FileManagerHelper.h"
#import "MeshPool.h"
#import "World.h"
#include <cstddef>   // offsetof, renameWorld
#include "EdenWorldStore.h"   // emod::eden_detect_bands -- a .eden's height (S.3b)
#include "EdenWorldSource.h"  // the raw .eden bytes out of a .eden / .gz / .zip (S.5)
#import "Alert.h"            // showAlertConvertHeight (S.5e)

// See FileManager.h. Both NULL on web — its Foundation shim already answers both questions the
// way this port wants — and both installed by the native entry point.
void (*eden_save_backup_hook)(const char* worldFilePath) = NULL;
const char* (*eden_documents_root_hook)(void) = NULL;

#include <vector>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <chrono>
#include <ctime>
#include <sys/stat.h>
#ifndef _WIN32
#include <sys/statvfs.h>
#else
#include <windows.h>
#endif

//#import "TestFlight.h"

// Web port hook (web/src/seam/LoadFailure_web.mm): reports a corrupt/truncated save to the
// browser so it can offer a recovery dialog instead of silently reading garbage (perf-audit C4's
// still-open "load-failure recovery UI" item). Plain cross-TU C function call, NOT a --wrap --
// there is no existing call site to intercept, this is a new one loadWorld() makes directly. Only
// linked when the web seam sources are built; a from-scratch iOS build would need its own stub.
extern "C" void eden_report_load_failure(const char* world_file_name, const char* reason);

// Web port hook (web/src/seam/Menu_web.mm): the DOM New World screen's height picker parks its
// choice here the same way it already does for flat/normal (eden_menu_take_pending_world_type),
// so probeWorldHeight() below can answer something other than the 64z default for a world that
// doesn't exist yet. "Take" semantics: one-shot, -1 means nothing pending. Only linked when the
// web seam sources are built; a from-scratch iOS build would need its own stub.
extern "C" int eden_menu_take_pending_world_height(void);






static map_t indexes;
static unsigned long long cur_dir_offset;
static map_t indexes_hmm;
static FileManager* single;
static std::string docs;
static NSFileHandle* saveFile;
static WorldFileHeader* sfh;
static BOOL writeDirectory;
static NSString* imgHash;
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
static NSString* pendingOriginalName=nil;
// Default ON (the user, 2026-10-10): new worlds are `.emod`, `.eden` files convert when played. Settings >
// Experiments > "New .emod format" drives it; a harness pins it (g_world_format_pinned) so a setting cannot change a gate.
int g_world_format=1;
int g_world_format_pinned=0;

// The 192-byte header of a dropped-in archive (`.eden.gz`, `.zip`), inflated from its first bytes.
// True only if it reads like a world a conversion could take: a plausible directory_offset and a
// version this build knows (1.x files and unknown versions have no converter and are not listed).
static BOOL peekArchiveHeader(const std::string& path,WorldFileHeader* out){
    emod::WorldSource src;
    if(!src.open(path)||src.kind()==emod::WorldSource::K_RAW)return FALSE;
    if(src.read(out,sizeof(*out))!=sizeof(*out)||src.failed())return FALSE;
    return out->directory_offset>=sizeof(WorldFileHeader)&&out->version>=1&&out->version<=FILE_VERSION_256Z_MAX;
}
// Stage S / S.4: rename an `.emod` world on disk -- its display name lives in the WORLD_HEADER
// record, which is never rewritten in place, so a new one is appended in its own committed batch.
// Shared by renameWorld (the world list) and setName (the share screen). If that world is the one
// open, its store is used, so the two handles never disagree about the file's end.
static BOOL emodSetHeaderName(const std::string& path,const std::string& displayName){
    emod::EdenWorldStore own;
    emod::EdenWorldStore* st=(emodStore&&emodStore->path()==path)?emodStore:&own;
    if(st==&own&&!own.open(path.c_str(),true))return FALSE;
    std::vector<uint8_t> raw;
    if(st->readRecord(emod::T_WORLD_HEADER,raw)>emod::EdenWorldStore::READ_FALLBACK||raw.size()!=sizeof(WorldFileHeader))return FALSE;
    WorldFileHeader h; memcpy(&h,&raw[0],sizeof(h));
    memset(h.name,0,sizeof(h.name));
    strncpy(h.name,displayName.c_str(),sizeof(h.name)-1);
    if(st==emodStore&&emodHeader)memcpy(emodHeader->name,h.name,sizeof(h.name));
    st->beginBatch();
    st->putRecord(emod::T_WORLD_HEADER,&h,sizeof(h));
    return st->commitBatch()?TRUE:FALSE;
}
// S.3b: whether the open file already has its creature block between the last column and the
// directory. Stock code asked `version>=3` (a v1/v2 file had none until its first save appended
// one and stamped 4) -- but the 2026 game writes version 2 on 256z worlds WITH a 400-slot block,
// which that test would append a second block after. Set by deriveColumnSpans() from the file.
static BOOL creature_block_on_disk;

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
    genflat=FALSE;
    imgHash=NULL;
    convertingWorld=FALSE;
	// PORT SEAM (Phase N Stage 2). Was NSSearchPathForDirectoriesInDomains(NSDocumentDirectory,
	// NSUserDomainMask, YES) + [paths objectAtIndex:0]. On WEB that is byte-identical — the
	// Foundation shim's NSSearchPathForDirectoriesInDomains does nothing but return
	// eden_platform_documents_root() in a one-element array. On NATIVE it is the difference
	// between "wherever real Foundation says Documents is" and a directory the host chooses:
	// ~/Library/Application Support/Eden, or whatever --docs= names. Asking the seam directly is
	// also what makes the two targets agree about where saves are, which the save-interchange
	// criterion cares about. See docs/save-load.md.
	// PORT SEAM (Phase N Stage 2). Stock behaviour is the `else` branch, byte for byte — including
	// on web, where the Foundation shim's NSSearchPathForDirectoriesInDomains already answers with
	// eden_platform_documents_root() and hands back an immortal string it owns.
	//
	// THE HOOK IS NOT COSMETIC AND THE ELSE BRANCH MUST STAY. Replacing this whole thing with
	// `[[NSString stringWithUTF8String:seam] retain]` was tried and it BROKE THE WEB BUILD — six
	// headless suites died inside a later FileManager::readDirectory() as a bare wasm "function
	// signature mismatch" with nothing naming NSString, i.e. `documents` had been freed under it.
	// The shim's array is deliberately immortal for exactly this pointer (see the comment in
	// web/src/shim/foundation/NSFileManager.mm); a shim-autoreleased string plus a retain is not
	// the same thing. So web keeps the path it already had, and only a host that installs the hook
	// — native, where this is real Foundation and real refcounting — takes the other branch.
	if (eden_documents_root_hook) {
		documents = [[NSString stringWithUTF8String:eden_documents_root_hook()] retain];
	} else {
		NSArray *paths = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES);
		documents = [paths objectAtIndex:0];
		[documents retain];
	}
    docs=cpstring(documents);
    printg("!!!!!! %s\n",[documents cStringUsingEncoding:NSUTF8StringEncoding]);
	oldOffsetX=oldOffsetZ=chunkOffsetX=chunkOffsetZ=-1;
	indexes=hashmap_new();
    indexes_hmm=indexes;
    shortSpans=hashmap_new();

    fmh_init(this);
	
	
}
BOOL FileManager::worldExists(std::string na,BOOL appendArchive){
    
   
    
    std::string file_name=docs + "/" +na;
    
    FILE* f;
    // "rb", not "r" — Phase N Stage 3. Existence is all this asks, so the mode is cosmetic here,
    // but every fopen in this file names a .eden save and the Windows CRT's text mode is not a
    // no-op on one (see getName below, where it was not cosmetic at all).
    if((f=fopen(file_name.c_str(),"rb"))){
        fclose(f);
        return TRUE;
    }
    return FALSE;

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

BOOL FileManager::deleteWorld(NSString* name){
    NSFileManager* fm=[NSFileManager defaultManager];
    if(isEmodName(name)){
        // The open store holds the file; Windows will not delete an open one. Its temp names go too.
        this->closeWorld();
        NSString* base=[NSString stringWithFormat:@"%@/%@",documents,name];
        // A C array, not an @[...] literal: web's Foundation shim has no arrayWithObjects:count:.
        static const char* const kTemps[]={".converting",".spill",".creating",".compact"};
        for(int i=0;i<4;i++)
            [fm removeItemAtPath:[NSString stringWithFormat:@"%@%s",base,kTemps[i]] error:NULL];
    }
    NSString* img_name=[NSString stringWithFormat:@"%@/%@.png",documents,name];
    if([fm fileExistsAtPath:img_name]){
        [fm removeItemAtPath:img_name error:NULL];
    }
   // removeFromIndex(name);
	NSString* file_name=[NSString stringWithFormat:@"%@/%@",documents,name];
	// B5: a leftover rollback journal must not outlive the world it belongs to -- a new world
	// created under the same file name would otherwise be "recovered" back into that stale tail
	// on its first load. Same reason the scratch/backup slots go here.
	[fm removeItemAtPath:[file_name stringByAppendingString:@".savejrnl"] error:NULL];
	
	
	if([fm fileExistsAtPath:file_name]){
		if([fm removeItemAtPath:file_name error:NULL])
			return TRUE;
		else 
			return FALSE;

	}
	return FALSE;
	
	
}
void FileManager::LoadCreatures(){
    printg("start load:%d\n",1);

    // Start from "no creature in any slot" every time: creatureData is a file-scope array that
    // outlives the world, MAX_CREATURES_SAVED is now per-file (and legitimately 0 for a save whose
    // writer emitted no creature block at all), so anything not overwritten below would otherwise
    // be the PREVIOUS world's creatures.
    for(int i=0;i<MAX_CREATURES_SAVED_MAX;i++){
        creatureData[i].type=-1;
    }
    if(!creature_block_on_disk){
    }else{
        [saveFile seekToFileOffset:sfh->directory_offset-sizeof(EntityData)*MAX_CREATURES_SAVED];
        for(int i=0;i<MAX_CREATURES_SAVED;i++){
            NSData* data=[saveFile readDataOfLength:sizeof(EntityData)];
            
            [data getBytes:&creatureData[i] length:sizeof(EntityData)];
          //  creatureData[i].pos.x-=CHUNK_SIZE*chunkOffsetX;
           // creatureData[i].pos.z-=CHUNK_SIZE*chunkOffsetZ;
            //  printg("type: %d\n  pos(%f,%f,%f)",creatureData[i].type,creatureData[i].pos.x,creatureData[i].pos.z,creatureData[i].pos.y);
        }
    }
    

    LoadModels2();
     printg("end load:%d\n",2);
}
void FileManager::saveCreatures(){
  //  printg("start save:%d\n",sfh->version);
    if(!creature_block_on_disk){
    [saveFile seekToFileOffset:sfh->directory_offset];
        sfh->directory_offset+=sizeof(EntityData)*MAX_CREATURES_SAVED;
        writeDirectory=TRUE;
        creature_block_on_disk=TRUE;
    }
    else
      [saveFile seekToFileOffset:sfh->directory_offset-sizeof(EntityData)*MAX_CREATURES_SAVED];  
    SaveModels();
    for(int i=0;i<MAX_CREATURES_SAVED;i++){
        EntityData data=creatureData[i];
        //data.pos.x+=CHUNK_SIZE*chunkOffsetX;
       // data.pos.z+=CHUNK_SIZE*chunkOffsetZ;
        NSData* dh=[NSData dataWithBytesNoCopy:&data length:sizeof(EntityData)
                                  freeWhenDone:FALSE];
        [saveFile writeData:dh];

    }
     
//	 printg("end save:%d\n",sfh->version);
}

void FileManager::saveWorld(){
    this->saveWorld(World::getWorld->player->pos);
    
    
}
void FileManager::compressLastPlayed(){
  //  NSString* name=World::getWorld->terrain.world_name;
	//NSString* file_name=[NSString stringWithFormat:@"%@/%@",documents,name];
   // CompressWorld([name cStringUsingEncoding:NSUTF8StringEncoding]);
    
}
void FileManager::loadGenFromDisk(){
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
       
        extern block8* biomez;
        biomez=(block8*)malloc(height*width*sizeof(block8));
        memset(biomez,0,height*width*sizeof(block8));
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
    
    
}
void FileManager::writeGenToDisk(){
    printg("writing gen to disk\n");
    NSString* name=@"Eden.eden";
	NSString* file_name=[NSString stringWithFormat:@"%@/%@",documents,name];
    
    sfh=(WorldFileHeader*)malloc(sizeof(WorldFileHeader));
    // Zero it: see saveWorld's copy of this note. Only some of the 192 bytes are assigned below,
    // and the rest go to disk as-is.
    memset(sfh,0,sizeof(WorldFileHeader));
    
    NSFileManager* fm=[NSFileManager defaultManager];
	if([fm fileExistsAtPath:file_name]){
       

        BOOL success = [fm removeItemAtPath:file_name error:NULL];
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
    
    [fm createFileAtPath:file_name
                contents:[NSData dataWithBytesNoCopy:sfh
                 length:sizeof(WorldFileHeader) freeWhenDone:FALSE]
              attributes:nil];
    
    saveFile=[NSFileHandle fileHandleForUpdatingAtPath:file_name];
    
    
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
    
   
    NSData* dh=[NSData dataWithBytesNoCopy:sfh length:sizeof(WorldFileHeader) freeWhenDone:FALSE];
    
    
	[saveFile seekToFileOffset:0];
    [saveFile writeData:dh];
	if(writeDirectory){
		
		
		count=0;
		this->fwriteDirectory();
		
	}
	
	this->readDirectory();
	free(sfh);
	[saveFile closeFile];
    printg("finished writing gen to disk\n");

}

// ---- B5: saving a large world without duplicating it ----------------------------------------
// Below g_save_inplace_threshold nothing here changes: the save runs on a whole-file scratch copy
// and is committed by one rename, which is fully atomic (pass 37). That copy is O(file size) in
// time AND in peak memory, so above the threshold the save runs directly on the real file and a
// small ROLLBACK JOURNAL stands in for the scratch copy.
//
// What the journal has to cover. A save writes: the 192-byte header; the dirty columns, each at
// its own already-allocated offset; then -- only when a column is NEW -- an appended column record
// starting at (directory_offset - creature block), which overwrites the old creature block and the
// front of the old directory, followed by the creature block and the directory at their new,
// higher offsets. Everything destructive is therefore at or above (directory_offset - creature
// block); every in-place column write is strictly below it. So journalling the header plus the
// file's tail from that point is enough to put the file back exactly as the last successful save
// left it -- and it is O(number of columns), not O(file size): ~410 KB for a 3.97 GB specimen.
//
// C3 (2026-09-02) closed the residual this comment used to end with. It read: "a crash between the
// journal and the commit CAN leave an individual dirty column half-old/half-new... journalling the
// dirty columns too would restore full atomicity at the cost of re-reading and re-writing every
// dirty column each save (up to ~42 MB at 256z), which is most of the cost this row exists to
// remove." Two things made that trade worth reversing:
//
//  * The ~42 MB was the WORST case (every column in the resident window dirty at 256z), and the
//    typical case is nothing like it -- a save only writes columns whose chunks are `modified`,
//    and a column the directory does not yet have is APPENDED, i.e. already covered by the region
//    below and needing no pre-image at all. A steady-state autosave journals zero columns.
//  * Under IDBFS the cost was unmeasurable anyway, because the persistence layer re-wrote the
//    whole world file to IndexedDB on every save regardless (ROADMAP C1). Since C2 put an OPFS
//    backend underneath that mirrors only the byte ranges actually written, the extra journal
//    write is a real, visible number for the first time -- and it is one extra copy of exactly the
//    bytes the save was already writing, i.e. the in-place path stays O(dirty columns) and never
//    goes back to O(file size).
//
// So the journal now has TWO phases and the in-place path is fully atomic again:
//   phase 1  the world header + the file's tail from (directory_offset - creature block) to EOF,
//            written before saveWorld touches anything -- the region an APPEND destroys.
//   phase 2  one record per column this save will overwrite AT ITS EXISTING OFFSET, holding that
//            column's original bytes. Written after readDirectory(), because "does this column
//            already have a directory row" is what separates an overwrite from an append, and
//            still before the first destructive byte (readDirectory only reads).
// Both are O(number of columns), never O(file size). `g_save_journal_columns` (Constants.h) turns
// phase 2 off, which reproduces B5's behaviour exactly -- it exists to A/B the cost.
#define SAVE_JOURNAL_VERSION 2
static const char kSaveJournalMagic[8]="EDNJRNL";
// A phase-2 record, repeated to the end of the journal: this header, then `length` original bytes.
static const char kSaveJournalColumnMagic[8]="EDNJCOL";
// The largest a column record can be at any world height, for bounds-checking a journal read back
// before the world's height is known (recoverInterruptedSave runs from probeWorldHeight).
#define SAVE_JOURNAL_MAX_COLUMN \
	((unsigned long long)CHUNK_SIZE*CHUNK_SIZE*CHUNK_SIZE*2*CHUNKS_PER_COLUMN_MAX)
typedef struct{
	char magic[8];
	unsigned int version;
	unsigned int reserved;
	unsigned long long orig_length;
	unsigned long long region_offset;
	unsigned long long region_length;
	unsigned char world_header[sizeof(WorldFileHeader)];
}SaveJournalHeader;
typedef struct{
	char magic[8];
	unsigned long long offset;
	unsigned long long length;
}SaveJournalColumn;

// The journal handle stays open across the whole in-place save so phase 2 can append to it; see
// beginSaveJournal()/journalDirtyColumns()/endSaveJournal().
static NSFileHandle* saveJournalFile=NULL;
static unsigned long long saveJournalRegionOffset=0;
static unsigned long long saveJournalColumnBytes=0;
// How many bytes the journal SHOULD contain so far. -writeData: cannot report a short write (the
// shim's fwrite return is unchecked, and was equally unchecked behind the createFileAtPath: this
// replaced), so each phase compares this against the file's real length before letting the save
// proceed. A journal that did not fit is the exact failure the whole mechanism exists for.
static unsigned long long saveJournalExpected=0;

static NSString* journalPathFor(NSString* file_name){
	return [file_name stringByAppendingString:@".savejrnl"];
}
static unsigned long long fileByteLength(NSString* path){
	NSFileHandle* fh=[NSFileHandle fileHandleForReadingAtPath:path];
	if(fh==NULL)return 0;
	unsigned long long len=[fh seekToEndOfFile];
	[fh closeFile];
	return len;
}
// Phase 1. Must complete before saveWorld touches the world file. Answers FALSE if anything went
// wrong, in which case the caller skips the save rather than writing unprotected. The handle is
// left OPEN, positioned at EOF, for journalDirtyColumns() to append phase 2 to.
//
// This used to be one createFileAtPath: "so the journal is never observed half-built through a
// handle, and so it never trips the shim's own backup-before-overwrite". The first property is
// gone by construction now -- phase 2 is not knowable this early -- but it was never the thing
// keeping recovery honest: recoverInterruptedSave() discards whatever it cannot read to the end,
// because a torn record proves the crash happened before the bytes that record protects were
// written. The second property still holds, and is why the handle is opened on a path that was
// just removed: the shim only backs up a file that already has bytes in it.
static BOOL beginSaveJournal(NSString* file_name,unsigned long long orig_length,
							 unsigned long long dir_offset){
	NSFileManager* fm=[NSFileManager defaultManager];
	NSString* jrnl=journalPathFor(file_name);
	[fm removeItemAtPath:jrnl error:NULL];
	saveJournalRegionOffset=0;
	saveJournalColumnBytes=0;
	unsigned long long creatures=(unsigned long long)sizeof(EntityData)*MAX_CREATURES_SAVED;
	unsigned long long region_off=(dir_offset>creatures)?(dir_offset-creatures):0;
	if(region_off>orig_length)return FALSE;   // header disagrees with the file; don't guess
	unsigned long long region_len=orig_length-region_off;
	NSFileHandle* src=[NSFileHandle fileHandleForReadingAtPath:file_name];
	if(src==NULL)return FALSE;
	NSData* hdr=[src readDataOfLength:sizeof(WorldFileHeader)];
	[src seekToFileOffset:region_off];
	NSData* region=[src readDataOfLength:(NSUInteger)region_len];
	[src closeFile];
	if([hdr length]<sizeof(WorldFileHeader)||[region length]<region_len)return FALSE;
	SaveJournalHeader jh;
	memset(&jh,0,sizeof(jh));
	memcpy(jh.magic,kSaveJournalMagic,sizeof(jh.magic));
	jh.version=SAVE_JOURNAL_VERSION;
	jh.orig_length=orig_length;
	jh.region_offset=region_off;
	jh.region_length=region_len;
	memcpy(jh.world_header,[hdr bytes],sizeof(WorldFileHeader));
	NSMutableData* out=[NSMutableData dataWithCapacity:(NSUInteger)(sizeof(jh)+region_len)];
	[out appendBytes:&jh length:sizeof(jh)];
	[out appendData:region];
	// N.8: Apple Foundation's fileHandleForUpdatingAtPath: returns nil for a missing file (the journal was
	// just removed), so create it first -- otherwise every in-place save (worlds >= 16 MB) was skipped.
	[fm createFileAtPath:jrnl contents:nil attributes:nil];
	NSFileHandle* jf=[NSFileHandle fileHandleForUpdatingAtPath:jrnl];
	if(jf==NULL)return FALSE;
	[jf writeData:out];
	saveJournalExpected=(unsigned long long)sizeof(jh)+region_len;
	if([jf seekToEndOfFile]!=saveJournalExpected){[jf closeFile];return FALSE;}
	saveJournalFile=[jf retain];
	saveJournalRegionOffset=region_off;
	printg("save: journalled %llu B of tail (offset %llu) before writing %s in place\n",
		   region_len,region_off,[file_name UTF8String]);
	return TRUE;
}
// Phase 2 (C3). Appends the ORIGINAL bytes of every column this save is about to overwrite at its
// existing offset. Runs after readDirectory() and before the first destructive write.
//
// Three things it must get right:
//  * It must NOT clear `modified`. saveColumn() owns that flag, and if this fails the save is
//    skipped, so the flags have to survive for the next one.
//  * A column with no directory row is APPENDED, at or above saveJournalRegionOffset, so phase 1
//    already holds its pre-image; skipping it here is what keeps a first save of a big new area
//    from journalling twice.
//  * Each (cx,cz) is visited once, so no offset is ever journalled twice and replay order does
//    not matter.
// A short column record (deriveColumnSpans' case) is journalled at the full stride the save will
// write, clamped to the phase-1 region -- the pre-image has to cover the bytes that get destroyed,
// not the bytes the directory claims are the column's.
static BOOL journalDirtyColumns(NSFileHandle* world){
	if(saveJournalFile==NULL)return FALSE;
	if(!g_save_journal_columns)return TRUE;
	Terrain* ter=World::getWorld->terrain;
	int columns=0;
	for(int x=0;x<CHUNKS_PER_SIDE;x++){
		for(int z=0;z<CHUNKS_PER_SIDE;z++){
			TerrainChunk* col=ter->chunkTable[threeToOne(x,0,z)];
			if(col==NULL||col->pbounds[1]!=0)continue;   // same guard the save loop uses
			int cx=col->pbounds[0]/CHUNK_SIZE;
			int cz=col->pbounds[2]/CHUNK_SIZE;
			BOOL dirty=FALSE;
			for(int cy=0;cy<CHUNKS_PER_COLUMN;cy++){
				TerrainChunk* chunk=ter->chunkTable[threeToOne(cx,cy,cz)];
				if(chunk&&chunk->modified){dirty=TRUE;break;}
			}
			if(!dirty)continue;
			int n=twoToOneTest(cx,cz);
			if(n==0)continue;                            // saveColumn drops these too
			ColumnIndex* colIndex=NULL;
			hashmap_get(indexes,n,(any_t*)&colIndex);
			if(colIndex==NULL)continue;                  // new column -> appended, phase 1 covers it
			unsigned long long off=colIndex->chunk_offset;
			if(off>=saveJournalRegionOffset)continue;    // already inside phase 1
			unsigned long long len=SIZEOF_COLUMN;
			if(off+len>saveJournalRegionOffset)len=saveJournalRegionOffset-off;
			[world seekToFileOffset:off];
			NSData* pre=[world readDataOfLength:(NSUInteger)len];
			if(pre==NULL||[pre length]<len)return FALSE;
			SaveJournalColumn rec;
			memset(&rec,0,sizeof(rec));
			memcpy(rec.magic,kSaveJournalColumnMagic,sizeof(rec.magic));
			rec.offset=off;
			rec.length=len;
			// Two writes rather than one concatenated buffer, so a 128 KB column is never held
			// twice at once -- this runs on the same heap Phase M is trying to keep under a cap.
			[saveJournalFile writeData:[NSData dataWithBytesNoCopy:&rec length:sizeof(rec)
													 freeWhenDone:FALSE]];
			[saveJournalFile writeData:pre];
			saveJournalExpected+=sizeof(rec)+len;
			saveJournalColumnBytes+=len;
			columns++;
		}
	}
	if([saveJournalFile seekToEndOfFile]!=saveJournalExpected)return FALSE;
	if(columns)printg("save: journalled %d dirty column(s), %llu B, before overwriting them\n",
					  columns,saveJournalColumnBytes);
	return TRUE;
}
// Closes the journal handle, and on `remove_it` deletes the journal -- which is the commit: the
// point after which recoverInterruptedSave() will no longer roll this save back.
static void endSaveJournal(NSString* file_name,BOOL remove_it){
	if(saveJournalFile){
		[saveJournalFile closeFile];
		[saveJournalFile release];
		saveJournalFile=NULL;
	}
	if(remove_it)[[NSFileManager defaultManager] removeItemAtPath:journalPathFor(file_name)
														   error:NULL];
	saveJournalRegionOffset=0;
	saveJournalExpected=0;
}
// Roll a world file back to the last successful save if one was interrupted. Idempotent: the
// journal is only removed once the restore has fully landed, so a crash DURING recovery just
// means recovery runs again next time. A journal that is itself short or malformed means the
// crash happened while the journal was being written -- i.e. before the world file had been
// touched at all -- so it is discarded and the file is left alone.
void FileManager::recoverInterruptedSave(NSString* file_name){
	NSFileManager* fm=[NSFileManager defaultManager];
	NSString* jrnl=journalPathFor(file_name);
	if(![fm fileExistsAtPath:jrnl])return;
	NSFileHandle* jf=[NSFileHandle fileHandleForReadingAtPath:jrnl];
	if(jf==NULL){[fm removeItemAtPath:jrnl error:NULL];return;}
	NSData* jhd=[jf readDataOfLength:sizeof(SaveJournalHeader)];
	SaveJournalHeader jh;
	BOOL ok=([jhd length]==sizeof(SaveJournalHeader));
	if(ok){
		memcpy(&jh,[jhd bytes],sizeof(jh));
		// v1 journals (B5, phase 1 only) are still replayable and mean exactly what they meant
		// then; v2 adds the phase-2 column records after the region.
		ok=(memcmp(jh.magic,kSaveJournalMagic,sizeof(jh.magic))==0
			&&jh.version>=1&&jh.version<=SAVE_JOURNAL_VERSION
			&&jh.region_offset+jh.region_length==jh.orig_length);
	}
	NSData* region=ok?[jf readDataOfLength:(NSUInteger)jh.region_length]:NULL;
	if(ok&&[region length]!=jh.region_length)ok=FALSE;
	if(!ok){
		[jf closeFile];
		printg("save: discarding an incomplete journal for %s (world file untouched)\n",[file_name UTF8String]);
		[fm removeItemAtPath:jrnl error:NULL];
		return;
	}
	NSFileHandle* wf=[NSFileHandle fileHandleForUpdatingAtPath:file_name];
	if(wf==NULL){[jf closeFile];[fm removeItemAtPath:jrnl error:NULL];return;}
	[wf seekToFileOffset:jh.region_offset];
	[wf writeData:region];
	[wf truncateFileAtOffset:jh.orig_length];
	[wf seekToFileOffset:0];
	[wf writeData:[NSData dataWithBytes:jh.world_header length:sizeof(WorldFileHeader)]];
	// C3: then put back every column the interrupted save had started overwriting in place. The
	// scan stops at the first record that is short, mis-magicked or out of range -- a torn record
	// proves the crash happened WHILE that pre-image was being written, i.e. before its column had
	// been touched, so there is nothing to undo past that point. Records are disjoint by
	// construction (one per column per save), so replay order is irrelevant.
	int restored=0;
	if(jh.version>=2){
		while(TRUE){
			NSData* rd=[jf readDataOfLength:sizeof(SaveJournalColumn)];
			if(rd==NULL||[rd length]<sizeof(SaveJournalColumn))break;
			SaveJournalColumn rec;
			memcpy(&rec,[rd bytes],sizeof(rec));
			if(memcmp(rec.magic,kSaveJournalColumnMagic,sizeof(rec.magic))!=0)break;
			if(rec.length==0||rec.length>SAVE_JOURNAL_MAX_COLUMN)break;
			if(rec.offset+rec.length>jh.orig_length)break;
			NSData* pre=[jf readDataOfLength:(NSUInteger)rec.length];
			if(pre==NULL||[pre length]<rec.length)break;
			[wf seekToFileOffset:rec.offset];
			[wf writeData:pre];
			restored++;
		}
	}
	[wf closeFile];
	[jf closeFile];
	[fm removeItemAtPath:jrnl error:NULL];
	printg("save: rolled %s back to its last complete save (%llu B, %d column(s) restored) after an interrupted one\n",
		   [file_name UTF8String],jh.orig_length,restored);
}
void FileManager::saveWorld(Vector warp){
    //[TestFlight passCheckpoint:[NSString stringWithFormat:@"header_size:%d",(int)sizeof(WorldFileHeader)]];
    printf("sizeof(WFH)=%d",(int)sizeof(WorldFileHeader));
	World::getWorld->terrain->endDynamics(TRUE);
	//[World::getWorld->terrain updateAllImportantChunks];
	writeDirectory=FALSE;
	Terrain* ter=World::getWorld->terrain;
	NSString* name=ter->world_name;
	NSString* file_name=[NSString stringWithFormat:@"%@/%@",documents,name];
	// Stage S / S.4: an `.emod` world appends a committed batch instead of rewriting a file -- no
	// scratch copy, journal, backup slot or threshold apply (docs/save-load.md, "`.emod` worlds").
	if(isEmodName(name)){
		this->saveEmodWorld(warp);
		return;
	}

	// PORT SEAM (Phase N Stage 2) — the "previous save" slot, for hosts whose file layer cannot
	// provide it. See FileManager.h. Deliberately here, before ANY write to file_name and after
	// the path is known, so what it copies is the last successful save.
	if (eden_save_backup_hook) eden_save_backup_hook([file_name UTF8String]);

	sfh=(WorldFileHeader*)malloc(sizeof(WorldFileHeader));
	// ZERO THE WHOLE 192 BYTES BEFORE FILLING ANY OF THEM. This block assigns eleven fields and
	// then memcpy's the struct to disk, and -getCString:maxLength: NUL-TERMINATES rather than
	// pads — so everything after the terminator in name[50] and hash[36], all of reserved[], and
	// the struct's own padding went to the file as whatever malloc last handed back. Three
	// consequences, in increasing order of how much they matter: two identical saves are not
	// byte-identical; a world file (which the sharing feature UPLOADS) carries a few dozen bytes
	// of this process's heap; and the "reserved" bytes future versions are supposed to grow into
	// are not actually zero in any file written since 2.1.1.
	// Found by Phase N Stage 3's cross-platform byte comparison, which is exactly the check a
	// non-reproducible header defeats. Nothing reads these bytes, so zeroing them cannot change
	// how any existing world loads.
	memset(sfh,0,sizeof(WorldFileHeader));
	//NSLog(@"saving level_seed: %d",ter.level_seed);
	sfh->level_seed=ter->level_seed;
    sfh->goldencubes=World::getWorld->hud->goldencubes;
	sfh->directory_offset=cur_dir_offset;
	sfh->home=ter->home;
	sfh->pos=World::getWorld->player->pos;
	//sfh->pos.x/=BLOCK_SIZE;
	//sfh->pos.z/=BLOCK_SIZE;
	//sfh->pos.x+=CHUNK_SIZE*chunkOffsetX;
	//sfh->pos.z+=CHUNK_SIZE*chunkOffsetZ;
    printg("saving at player pos: %f,%f   co: %d,%d wfh_size:%d\n",sfh->pos.x,sfh->pos.z,chunkOffsetX,chunkOffsetZ,(int)sizeof(WorldFileHeader));
	sfh->yaw=World::getWorld->player->yaw;
    sfh->version=file_version;
    
    for(int i=0;i<4;i++){
        for(int j=0;j<4;j++){
            sfh->skycolors[i*4+j]=regionSkyColors[i][j];
        }
    }
    
	[World::getWorld->menu->selected_world->display_name getCString:sfh->name
														 maxLength:49
														  encoding:NSUTF8StringEncoding];
    if(imgHash==NULL)imgHash=@""; 
    [imgHash getCString:sfh->hash
        maxLength:33
        encoding:NSUTF8StringEncoding];
    
	// Perf-audit C4 ("no atomicity"): every save used to seek/write/truncate file_name IN PLACE, so
	// a save interrupted mid-write (tab discard, OOM, crash) left the ONLY copy of the world in a
	// state whose tail is not a valid ColumnIndex directory — unreadable. Now this whole function
	// operates on a scratch copy (temp_name) and the real file is only ever replaced by one atomic
	// rename at the very end, after temp_name is fully written and closed — the exact temp+rename
	// pattern convertFile() already uses for format migration (:1302-1339), just applied to the
	// ordinary save path now that Classes/ is editable (was blocked on this before 2026-07-25).
	// Any crash before the rename leaves file_name byte-identical to the last successful save.
	//
	// B5 (2026-08-25): that scratch copy is O(file size) in time and in peak memory, which does not
	// survive contact with a 256z world — so it now only runs BELOW g_save_inplace_threshold. At or
	// above it the save writes straight into file_name, protected by writeSaveJournal()'s small
	// rollback journal instead (see the block comment above this function). Anything that makes the
	// journal unwritable falls back to the copy path rather than writing unprotected.
	NSString* temp_name=[file_name stringByAppendingString:@".savetmp"];
	NSFileManager* fm=[NSFileManager defaultManager];
	BOOL existed=[fm fileExistsAtPath:file_name];
	BOOL inPlace=FALSE;
	unsigned long long existing=existed?fileByteLength(file_name):0;
	if(existed&&existing>=g_save_inplace_threshold){
		if(!beginSaveJournal(file_name,existing,sfh->directory_offset)){
			// Bail rather than fall through to the copy path: a file this big is exactly the one
			// whose whole-file copy cannot be relied on to succeed, and finishing the save with
			// neither a scratch copy nor a journal is the one outcome that can leave the world
			// unloadable. The last complete save stays on disk untouched, every chunk keeps its
			// `modified` flag, and the next save retries. (Chunks are re-marked by endDynamics
			// only; nothing above this point has cleared them.)
			NSLog(@"saveWorld: could not journal %@ -- SKIPPING this save, last one left intact",file_name);
			endSaveJournal(file_name,TRUE);
			free(sfh);
			return;
		}
		inPlace=TRUE;
	}
	[fm removeItemAtPath:temp_name error:NULL]; // drop any orphan from a previous crashed save
	if(inPlace){
		// Nothing to seed: the file we are about to write IS the file that already holds every
		// column this save won't touch.
		//
		// Do reclaim the whole-file backup slots, though. A world that grew past the threshold
		// leaves behind whatever the LAST below-threshold save wrote (NSFileHandle.mm's
		// "<path>.bak" / ".savetmp.bak" copy), and nothing above the threshold ever refreshes it:
		// it is a full second copy of the world, permanently, of exactly the worlds least able to
		// afford one -- and LoadFailure_web.mm's Restore button would offer it as if it were the
		// previous save when it can in fact be arbitrarily old. Above the threshold the durability
		// story is the rollback journal, not a copy slot, so the stale slot goes. (Caught by the
		// live-Safari run of this change, not by any headless test -- a world only crosses the
		// threshold with real play behind it.)
		[fm removeItemAtPath:[temp_name stringByAppendingString:@".bak"] error:NULL];
		[fm removeItemAtPath:[file_name stringByAppendingString:@".bak"] error:NULL];
	}else if(!existed){
        // Historically an unconditional 2 (every brand-new world starts there and gets bumped to
        // FILE_VERSION by the "file_version<FILE_VERSION_256Z" stamp below) -- but readDirectory()
        // a few lines down calls deriveColumnSpans(), which for an EMPTY directory falls back to
        // deciding the creature-block size from sfh->version right here, before that later stamp
        // ever runs. A brand-new 256z world (FileManager::loadWorld already set file_version to
        // FILE_VERSION_256Z for it) needs THAT decision to see 256z too, or its first save silently
        // gets a 200-slot 64z-shaped creature block under a version-5 header.
        sfh->version=(file_version>=FILE_VERSION_256Z)?file_version:2;
        // saveCreatures() below has two paths: version<3 treats directory_offset as NOT yet
        // accounting for the creature block and bumps it there (the historical "brand new world"
        // case, always true before this file's version could BE anything but 2 on a first save);
        // version>=3 trusts directory_offset ALREADY points past the creature block, which is only
        // true after that one-time bump has happened once. A version stamped 256z straight out of
        // the gate skips the version<3 branch entirely, so it has to arrive with directory_offset
        // already correct instead -- otherwise saveCreatures() seeks to a negative (wrapped-huge)
        // offset, the seek silently fails, and the creature block lands at byte 192 with
        // directory_offset left pointing AT it instead of past it (readDirectory then reads the
        // creature block as 1500+ garbage "columns").
        sfh->directory_offset=sizeof(WorldFileHeader)+
            (sfh->version>=FILE_VERSION_256Z?(unsigned long long)sizeof(EntityData)*MAX_CREATURES_SAVED_MAX:0);

		[fm createFileAtPath:temp_name
					contents:[NSData dataWithBytesNoCopy:sfh
						length:sizeof(WorldFileHeader) freeWhenDone:FALSE]
			attributes:nil];
        writeDirectory=TRUE;

	}else{
        // Seed the scratch copy with the CURRENT on-disk file so every column this save doesn't
        // touch keeps its valid data at its existing offset — this save only rewrites what changed.
        // If that copy fails (no space for a second whole world, most likely) the scratch is empty
        // or absent, and carrying on would write a save containing ONLY this session's dirty
        // columns and then rename it over the real world. That was reachable before B5 too; it is
        // checked now because the fix costs one BOOL.
        if(![fm copyItemAtPath:file_name toPath:temp_name error:NULL]){
            NSLog(@"saveWorld: could not stage a scratch copy of %@ -- SKIPPING this save, last one left intact",file_name);
            [fm removeItemAtPath:temp_name error:NULL];
            free(sfh);
            return;
        }
    }


	saveFile=[NSFileHandle fileHandleForUpdatingAtPath:inPlace?file_name:temp_name];

    
	count=0;
	this->readDirectory();
	NSLog(@"read %d colidx's",count);

	// C3: phase 2 of the rollback journal, here because this is the first point at which the
	// directory is known -- and still before the first destructive byte (readDirectory only reads,
	// and the column loop below is what starts writing). Same bail rule as phase 1: an in-place
	// save with an incomplete journal is the one outcome the journal exists to prevent, so skip it
	// and leave the last complete save alone. Every chunk keeps its `modified` flag (nothing has
	// cleared them yet -- journalDirtyColumns deliberately does not) and the next save retries.
	if(inPlace&&!journalDirtyColumns(saveFile)){
		NSLog(@"saveWorld: could not journal the dirty columns of %@ -- SKIPPING this save, last one left intact",file_name);
		endSaveJournal(file_name,TRUE);
		[saveFile closeFile];
		free(sfh);
		return;
	}
	// Stage D / D.3a: an edited trailer (signs, command blocks) has one home in a `.eden`, the
	// directory region, so it forces the directory rewrite even when no column changed.
	// readDirectory() above just re-captured what is on disk; the model's bytes replace it. (Phase 1
	// of the journal already holds the whole old directory + trailer, so an in-place rewrite of it is
	// covered.) The dirty mark is cleared only once the save has landed, below.
	BOOL trailerSaving=worldTrailer.dirty();
	if(trailerSaving){
		std::vector<uint8_t> t=worldTrailer.encode();
		if(dir_trailer){free(dir_trailer);dir_trailer=NULL;}
		dir_trailer_len=0;
		if(!t.empty()){
			dir_trailer=(unsigned char*)malloc(t.size());
			memcpy(dir_trailer,&t[0],t.size());
			dir_trailer_len=t.size();
		}
		writeDirectory=TRUE;
		printg("save: trailer edited -> rewriting the directory with a %llu B trailer\n",dir_trailer_len);
	}
 //   Player* player=World::getWorld->player;
  //  int scox=player.pos.x/CHUNK_SIZE-T_RADIUS;
   // int scoz=player.pos.z/CHUNK_SIZE-T_RADIUS;
   
    sfh->pos=warp;
    
    
    //NSLog(@"player pos load: %f %f %f",player.pos.x,player.pos.y,player.pos.z);
    //int r=T_RADIUS;
	//	int asdf=0;
   // printg("saving at co(%d,%d)",scox,scoz);
   // printg("save player pos(%d,%d)\n",(int)warp.x,(int)warp.z);
 //   for(int x=scox;x<scox+2*r;x++){
  //      for(int z=scoz;z<scoz+2*r;z++){
			//	NSLog(@"lch:%d",asdf++);
        //   [World::getWorld->fm saveColumn:x:z];
//        }
 //   }
    
    for(int x=0;x<CHUNKS_PER_SIDE;x++){
        for(int z=0;z<CHUNKS_PER_SIDE;z++)
        {
            TerrainChunk* chunk=ter->chunkTable[threeToOne(x,0,z)];
            
            if(chunk->pbounds[1]==0){
                this->saveColumn(chunk->pbounds[0]/CHUNK_SIZE
                                  ,chunk->pbounds[2]/CHUNK_SIZE);
                
            }else{
                printg("trying to save column with unexpected chunk bound[1]: %d\n",chunk->pbounds[1]);
            
            }
        }
    }
   

  
	//hashmap_iterate(ter.chunkMap, saveChunk, NULL);
	saveCreatures();

    // B4: this used to stamp FILE_VERSION (4) unconditionally, which would have re-labelled a 256z
    // world as 64z while its columns were still being written at the 256z stride -- and would have
    // normalised a v6 file to v5's meaning without knowing what v6 changes. A file that arrived
    // >=5 keeps its own version; everything else is stamped 4 exactly as before.
    // S.3b: ...unless the world is 256z under a smaller version (the 2026 game writes v2 256z
    // worlds): its version is kept verbatim too -- the height comes from the file, not from this
    // field, and 4 would mean nothing more to that game than 2 does.
    if(file_version<FILE_VERSION_256Z&&g_world_height<T_HEIGHT_MAX){
        sfh->version=FILE_VERSION;
        file_version=FILE_VERSION;
    }else{
        sfh->version=file_version;
    }
    NSData* dh=[NSData dataWithBytesNoCopy:sfh length:sizeof(WorldFileHeader) freeWhenDone:FALSE];
     
    
	if(writeDirectory){
		
		
		count=0;
		fwriteDirectory();
		NSLog(@"wrote %d colidx's",count);
	}
	// The header is written LAST because it is the only thing that says where the directory is:
	// on the in-place path it is the nearest thing this format has to a commit record, and on the
	// copy path the ordering costs nothing. (It used to be written before fwriteDirectory().)
	[saveFile seekToFileOffset:0];
    [saveFile writeData:dh];
	cur_dir_offset=sfh->directory_offset;
	readDirectory();
	free(sfh);
	[saveFile closeFile];

	if(inPlace){
		// Commit. Everything is written, flushed and closed; removing the journal is the point
		// after which recoverInterruptedSave() will no longer roll this save back.
		endSaveJournal(file_name,TRUE);
		if(trailerSaving)trailerSaved();
	}else{
	// The atomic swap: temp_name is now a fully-written, closed, valid save. Replace file_name with
	// it in one rename() — the point at which a crash can no longer produce a half-written world.
	// (removeItemAtPath first, then moveItemAtPath, matching convertFile()'s existing pattern above
	// rather than relying on rename()'s overwrite-destination behavior alone.)
	[fm removeItemAtPath:file_name error:NULL];
	if(![fm moveItemAtPath:temp_name toPath:file_name error:NULL]){
		NSLog(@"saveWorld: FAILED to swap in %@ from %@ -- previous save left untouched",file_name,temp_name);
	}else if(trailerSaving){
		trailerSaved();
	}
	}

	//[file writeData:[[NSData

}
// Rows actually WRITTEN by the pass below -- `count` is incremented for every row considered,
// including the ones the offset check rejects, so it cannot be used to find the directory's end.
static int dir_rows_written=0;
int saveColIdx(any_t passedIn,any_t colToSave){
	count++;
	ColumnIndex* colIndex=(ColumnIndex*)colToSave;
	if(colIndex&&colIndex->chunk_offset<sfh->directory_offset){
		int n=twoToOne(colIndex->x, colIndex->z);
		if(n==0){
		//	NSLog(@"corrupted col:%d",colIndex->chunk_offset);
		}
		
	NSData* dh=[NSData dataWithBytesNoCopy:colIndex length:sizeof(ColumnIndex)
				freeWhenDone:FALSE];
	[saveFile writeData:dh];
	dir_rows_written++;
	}else{
		NSLog(@"WTF MATE");
	}
	return MAP_OK;
}
void FileManager::fwriteDirectory(){
	[saveFile seekToFileOffset:sfh->directory_offset];
	dir_rows_written=0;
	hashmap_iterate(indexes, saveColIdx, NULL);
	// Re-emit whatever readDirectory captured past the real entries (sign records on a
	// NewFormat256z world; nothing at all on every other world) -- see dir_trailer's comment.
	if(dir_trailer&&dir_trailer_len){
		NSData* dt=[NSData dataWithBytesNoCopy:dir_trailer length:(NSUInteger)dir_trailer_len
					freeWhenDone:FALSE];
		[saveFile writeData:dt];
		printg("directory: re-emitted %llu B sign trailer after %d column rows\n",
			dir_trailer_len,dir_rows_written);
	}
	// The directory is read TO EOF, so any byte left past what we just wrote is parsed as another
	// directory row next time. Nothing shrinks the directory today, but the in-place save path
	// (B5) writes into the real file rather than into a fresh scratch copy, where a stale tail is
	// no longer impossible -- make the file end exactly where the directory does.
	[saveFile truncateFileAtOffset:sfh->directory_offset
		+(unsigned long long)dir_rows_written*sizeof(ColumnIndex)+dir_trailer_len];
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
	BOOL pending_over=FALSE;
	[saveFile seekToFileOffset:sfh->directory_offset];
	while(TRUE){
		NSData* data=[saveFile readDataOfLength:sizeof(ColumnIndex)];
		if(data==NULL||[data length]<sizeof(ColumnIndex))break;
		count++;
		ColumnIndex* colIdx=(ColumnIndex*)malloc(sizeof(ColumnIndex));
		[data getBytes:colIdx length:sizeof(ColumnIndex)];
		int n=twoToOne(colIdx->x, colIdx->z);
		if(n!=0){
		pending_len=0; pending_over=FALSE;
		hashmap_put(indexes,n, (any_t)colIdx);
           // printg("reading dir\n");
        }else {
			if(!pending_over){
				if(pending_len+sizeof(ColumnIndex)<=DIR_TRAILER_MAX){
					if(!pending)pending=(unsigned char*)malloc(DIR_TRAILER_MAX);
					if(pending){
						memcpy(pending+pending_len,colIdx,sizeof(ColumnIndex));
						pending_len+=sizeof(ColumnIndex);
					}else pending_over=TRUE;
				}else pending_over=TRUE;
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
    hashmap_remove_all(shortSpans,TRUE);
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
            if(gap>0)creature_block_on_disk=TRUE;
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
	hashmap_remove_all(indexes,TRUE);
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
    
    writeDirectory=TRUE;
    colIndex->x=cx;
    colIndex->z=cz;
    int n=twoToOneTest(cx,cz);
	hashmap_put(indexes, n, colIndex);
    
	
	[saveFile seekToFileOffset:colIndex->chunk_offset];
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
        
        NSData* data=[NSData dataWithBytesNoCopy:rledata
                                          length:(dataidx*sizeof(color8))
                                    freeWhenDone:FALSE];
        [saveFile writeData:data];
               //[saveFile writeData:data];
		
	}
	
}
void FileManager::saveColumn(int cx,int cz){
	Terrain* ter=World::getWorld->terrain;
	ColumnIndex* colIndex=NULL;
	
    BOOL needsSave=FALSE;
    for(int cy=0;cy<CHUNKS_PER_COLUMN ;cy++){
		TerrainChunk* chunk;
        //issue #3 continued
        chunk=ter->chunkTable[threeToOne(cx, cy, cz)];
        if(chunk->modified){needsSave=TRUE; chunk->modified=FALSE;}
    }
    if(!needsSave)return;
    
    //printg("saving column: %d,%d\n",cx,cz);
	int n=twoToOneTest(cx,cz);
	if(n==0){
		return;
	}
	hashmap_get(indexes, n, (any_t*)&colIndex);
	if(colIndex==NULL){
		colIndex=(ColumnIndex*)malloc(sizeof(ColumnIndex));
        if(creature_block_on_disk){
		colIndex->chunk_offset=sfh->directory_offset-sizeof(EntityData)*MAX_CREATURES_SAVED;
		
		
        }else{
            colIndex->chunk_offset=sfh->directory_offset;
        }
        sfh->directory_offset+=SIZEOF_COLUMN;
		writeDirectory=TRUE;
		colIndex->x=cx;
		colIndex->z=cz;
		hashmap_put(indexes, n, colIndex);
       
	}
	if((colIndex->chunk_offset-192)%SIZEOF_COLUMN!=0||colIndex->chunk_offset>=sfh->directory_offset){
        if((colIndex->chunk_offset-192)%SIZEOF_COLUMN!=0){
		printg("BAD BAD OFFSET!! %d\n",(int)sizeof(WorldFileHeader));
        }else if(colIndex->chunk_offset>=sfh->directory_offset)
        NSLog(@"OFFSET OVERFLOWS DIRECTORY!");
	}
	[saveFile seekToFileOffset:colIndex->chunk_offset];
    
	for(int cy=0;cy<CHUNKS_PER_COLUMN ;cy++){
		TerrainChunk* chunk;
         //issue #3 continued
        chunk=ter->chunkTable[threeToOne(cx, cy, cz)];
		//hashmap_get(ter.chunkMap, threeToOne(cx-chunkOffsetX, cy, cz-chunkOffsetZ), (any_t)&chunk);
        //co(16316,16395),co(16316,16395)
		if(chunk!=NULL){
			/*EdenChunkHeader ch;
			ch.n_vertices=chunk.n_vertices;
			NSData* data=[NSData dataWithBytesNoCopy:&ch
											  length:sizeof(EdenChunkHeader)
										freeWhenDone:FALSE];
			[saveFile writeData:data];
			
			int mesh_bytes=ch.n_vertices*sizeof(vertexStruct);
			sfh->directory_offset+=sizeof(EdenChunkHeader)+mesh_bytes;
			NSLog(@"vertices: %d",chunk.n_vertices);
			data=[NSData dataWithBytesNoCopy:chunk.vertices
									  length:mesh_bytes
								freeWhenDone:FALSE];
			[saveFile writeData:data];*/
            
			NSData* data=[NSData dataWithBytesNoCopy:chunk->pblocks
											  length:(CHUNK_SIZE*CHUNK_SIZE*CHUNK_SIZE*sizeof(block8))
										freeWhenDone:FALSE];
			[saveFile writeData:data];
            data=[NSData dataWithBytesNoCopy:chunk->pcolors
                                      length:(CHUNK_SIZE*CHUNK_SIZE*CHUNK_SIZE*sizeof(color8))
                                freeWhenDone:FALSE];
			[saveFile writeData:data];
		}else{
			printg("NULL CHUNK O SHIT\n");
		}
	}
	
}
extern block8* blockarray;
extern int g_offcx;
extern int g_offcz;
BOOL FileManager::readColumnDeferred(int cx,int cz,NSFileHandle* rcfile){
    // Only ONE case can be deferred: a column the CURRENT world's directory does not have, on a
    // default-seed world, which readColumn() below would satisfy from the bundled map via
    // fmh_readColumnFromDefault(). Everything else -- a column this world has saved (a straight
    // 32 KB read, no decode to move), a non-default seed (procedural generation, which touches
    // TerrainGen state) -- falls through to the unchanged synchronous path. The duplicated lookup
    // is one hashmap_get; duplicating readColumn() itself is what this avoids.
    Terrain* ter=World::getWorld->terrain;
    int n=twoToOne(cx,cz);
    if(n!=0&&ter->tgen->LEVEL_SEED==DEFAULT_LEVEL_SEED){
        ColumnIndex* colIndex=NULL;
        hashmap_get(indexes,n,(any_t*)&colIndex);
        BOOL absent=emodStore?!emodStore->hasColumn(cx,cz):(colIndex==NULL);
        if(absent&&mp_dispatchColumnDecode(cx,cz))return FALSE;
    }
    readColumn(cx,cz,rcfile);
    return TRUE;
}

void FileManager::readColumn(int cx,int cz,NSFileHandle* rcfile){
	Terrain* ter=World::getWorld->terrain;
	ColumnIndex* colIndex=NULL;
	int n= twoToOne(cx,cz);
	if(n==0){
		NSLog(@"mm");
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
         ter->tgen->generateColumn(cx,cz,FALSE);
      		return;
     }
	}
	//NSLog(@"reading col: %d, %d, %d",cx,cz,colIndex->chunk_offset);
		
	//cx-=chunkOffsetX;
	//cz-=chunkOffsetZ;
	
	 NSAutoreleasePool * pool = [[NSAutoreleasePool alloc] init];   
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
		}else
		[rcfile seekToFileOffset:colIndex->chunk_offset];
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
            
            TerrainChunk* chunk;
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
            
           
             BOOL rle=false;
             if(rle){
                 
                 block8 tblocks[CHUNK_SIZE3];
                 color8 tcolors[CHUNK_SIZE3];
                 color8 buf[CHUNK_SIZE3*3];
                 //too much read
                 NSData* datat=[rcfile readDataOfLength:2];
                 color8 buft[2];
                 [datat getBytes:buft length:2];
                 int chunk_data_length= buft[0]*256+buft[1]-2;
                 NSData* data=[rcfile readDataOfLength:chunk_data_length];
                 int n=(int)[data length];
                 if(n<chunk_data_length){
                     printg("not enough file left, only read %d bytes\n",n);
                 }//else
                  //   printg("all good %d, %d  sizeofcolor8:%d\n",(int)n,(int)chunk_data_length,(int)sizeof(color8));
                 [data getBytes:buf length:n];
                 
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
                 NSData* data=[rcfile readDataOfLength:(CHUNK_SIZE*CHUNK_SIZE*CHUNK_SIZE*sizeof(block8))];
                 [data getBytes:chunk->pblocks length:(CHUNK_SIZE*CHUNK_SIZE*CHUNK_SIZE*sizeof(block8))];

                 NSData* data2=[rcfile readDataOfLength:(CHUNK_SIZE*CHUNK_SIZE*CHUNK_SIZE*sizeof(color8))];
                 [data2 getBytes:chunk->pcolors length:(CHUNK_SIZE*CHUNK_SIZE*CHUNK_SIZE*sizeof(color8))];
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
                
                  
                ter->addChunk(chunk,cx,cy,cz,TRUE);
             
        }
        
	}
    
    [pool release];
    
	
}
std::string fullPathForFilename(const char* fn){
    return cpstring([[NSBundle mainBundle] pathForResource:nsstring(fn) ofType:nil]);
    //return docs+"/"+std::string(fn);
}
void FileManager::setName(std::string fn,std::string dn){
    
   
   //file_name=[file_name stringByDeletingPathExtension];
   // NSLog(@"set name request on:%@",file_name);
   // NSString* nofp=file_name;
    
    // Stage S / S.4: an `.emod` keeps its name in the WORLD_HEADER record -- append a new one.
    if(fn.size()>5&&fn.compare(fn.size()-5,5,".emod")==0){
        if(!emodSetHeaderName(docs+"/"+fn,dn))NSLog(@"file to rename not found\n");
        return;
    }
    fn=docs+"/"+fn;
    FILE* f;
    if(!(f=fopen(fn.c_str(),"rw"))){
        NSLog(@"file to rename not found\n");
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
void FileManager::setImageHash(NSString* hash){
    NSString* name=World::getWorld->terrain->world_name;
	NSString* file_name=[NSString stringWithFormat:@"%@/%@",documents,name];
    if(imgHash!=NULL){
        [imgHash release];
        imgHash=NULL;
    }
    imgHash=hash;
   
    if(isEmodName(name)){
        // The hash lives in the WORLD_HEADER record: append a new one in its own committed batch.
        emod::EdenWorldStore own;
        emod::EdenWorldStore* st=emodStore;
        if(!st){
            if(!own.open([file_name UTF8String],true))return;
            st=&own;
        }
        std::vector<uint8_t> raw;
        if(st->readRecord(emod::T_WORLD_HEADER,raw)>emod::EdenWorldStore::READ_FALLBACK||raw.size()!=sizeof(WorldFileHeader))return;
        WorldFileHeader h; memcpy(&h,&raw[0],sizeof(h));
        memset(h.hash,0,sizeof(h.hash));
        [hash getCString:h.hash maxLength:33 encoding:NSUTF8StringEncoding];
        if(emodHeader&&st==emodStore)memcpy(emodHeader->hash,h.hash,sizeof(h.hash));
        st->beginBatch();
        st->putRecord(emod::T_WORLD_HEADER,&h,sizeof(h));
        if(!st->commitBatch())printg("emod: could not record the image hash\n");
        return;
    }
    saveFile=[NSFileHandle fileHandleForUpdatingAtPath:file_name];
    if(!saveFile){
        printg("err gettin save file: %s\n",[file_name cStringUsingEncoding:NSUTF8StringEncoding]);
        return;
    }
	WorldFileHeader* fh=(WorldFileHeader*)[[saveFile readDataOfLength:sizeof(WorldFileHeader)] bytes];
    if(fh==NULL){
        printg("err reading has from file\n");
        return;
    }
	WorldFileHeader* fh2=(WorldFileHeader*)malloc(sizeof(WorldFileHeader));
	memcpy(fh2,fh,sizeof(WorldFileHeader));
	[hash getCString:fh2->hash
                   maxLength:33
                    encoding:NSUTF8StringEncoding];
    NSLog(@"MD5 hash of file  \"%@\": %s", 
          hash, fh2->hash);
	NSData* dh=[NSData dataWithBytesNoCopy:fh2 length:sizeof(WorldFileHeader) freeWhenDone:TRUE];
	[saveFile seekToFileOffset:0];
	[saveFile writeData:dh];
	
	[saveFile closeFile];	
   
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
BOOL FileManager::renameWorld(NSString* file_name, NSString* display_name){
    std::string n=cpstring(file_name);
    if(!worldExists(n,FALSE)) return TRUE;
    std::string fn=docs+"/"+n;
    if(isEmodName(file_name)){
        // The name is inside the WORLD_HEADER record, which is never rewritten in place: append a
        // new one. (Called from the world list, where no world -- so no open store -- exists.)
        return emodSetHeaderName(fn,cpstring(display_name));
    }
    FILE* f=fopen(fn.c_str(),"r+b");
    if(!f) return FALSE;
    WorldFileHeader fh;
    if(fread(&fh,sizeof(WorldFileHeader),1,f)!=1){ fclose(f); return FALSE; }
    char name[sizeof(fh.name)];
    memset(name,0,sizeof(name));
    std::string dn=cpstring(display_name);
    strncpy(name,dn.c_str(),sizeof(name)-1);
    BOOL ok=fseek(f,(long)offsetof(WorldFileHeader,name),SEEK_SET)==0
          &&fwrite(name,1,sizeof(name),f)==sizeof(name);
    if(fclose(f)!=0) ok=FALSE;
    return ok;
}

NSString* FileManager::getName(NSString* name){
    std::string n=cpstring(name);
	if(!worldExists(n,FALSE)) return @"error~";
    std::string fn=docs+"/"+n;
    if(emod::WorldSource::isArchiveName(n)){
        // A dropped-in `.eden.gz` / `.zip`: its name is in the first 192 inflated bytes.
        WorldFileHeader h;
        if(!peekArchiveHeader(fn,&h))return @"error~";
        h.name[49]=0;
        if(!h.name[0])return @"error";
        return nsstring(std::string(h.name));
    }
    if(isEmodName(name)){
        emod::EdenWorldStore st;
        if(!st.open(fn.c_str(),false))return @"error~";
        std::vector<uint8_t> raw;
        if(st.readRecord(emod::T_WORLD_HEADER,raw)>emod::EdenWorldStore::READ_FALLBACK||raw.size()!=sizeof(WorldFileHeader))return @"error~";
        WorldFileHeader h; memcpy(&h,&raw[0],sizeof(h));
        h.name[49]=0;
        if(!h.name[0])return @"error";
        return nsstring(std::string(h.name));
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
        return @"error~";
    }
    fh->name[49]=NULL;
    std::string res_name(fh->name);
    
    if(res_name.length()==0){
        fclose(sf);
         free(fh);
        return @"error";
    }
    
    
    fclose(sf);
    free(fh);
    NSString * nss=nsstring(res_name);
    //printf("file_name: %s",res_name.c_str());
    return nss;
    
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
static unsigned long long convert_offset;
static NSFileHandle* oldFile;
static NSFileHandle* newFile;
#define SIZEOF_OLDCOLUMN CHUNK_SIZE*CHUNK_SIZE*CHUNK_SIZE*CHUNKS_PER_COLUMN*(sizeof(block8))
#define SIZEOF_OLDCHUNK CHUNK_SIZE*CHUNK_SIZE*CHUNK_SIZE*sizeof(block8)
enum OLD_BLOCK_TYPES{
    oTYPE_NONE=0,
    oTYPE_BEDROCK=1,
    oTYPE_STONE=2,
    oTYPE_DIRT=3,
    oTYPE_SAND=4,
    oTYPE_GREEN_LEAVES=5,
    oTYPE_TREE=6,
    oTYPE_WOOD=7,
    oTYPE_GRASS=8,
    oTYPE_TNT=9,
    oTYPE_DARK_WOOD=10,
    oTYPE_ORANGE_LEAVES=11,
    oTYPE_YELLOW_LEAVES=12,
    oTYPE_DARK_STONE=13,
    oTYPE_GRASS2=14,
    oTYPE_GRASS3=15,
    oTYPE_BRICK=16,
    oTYPE_COBBLESTONE=17,
    oTYPE_GLASS=18,
    oTYPE_GREEN_CRYSTAL=19,
    oTYPE_PINK_CRYSTAL=20,
    oTYPE_PURPLE_CRYSTAL=21,
    oTYPE_WHITE_CRYSTAL=22,
    oTYPE_RED_LEAVES=23,
    oTYPE_BLANK_RED=24,
    oTYPE_BLANK_ORANGE=25,
    oTYPE_BLANK_YELLOW=26,
    oTYPE_BLANK_GREEN=27,
    oTYPE_BLANK_BLUE=28,
    oTYPE_BLANK_PURPLE=29,
    oTYPE_BLANK_PINK=30
};
int convertType[31]={
    [oTYPE_NONE]=TYPE_NONE,
    [oTYPE_BEDROCK]=TYPE_BEDROCK,
    [oTYPE_STONE]=TYPE_STONE,
    [oTYPE_DIRT]=TYPE_DIRT,
    [oTYPE_SAND]=TYPE_SAND,
    [oTYPE_GREEN_LEAVES]=TYPE_LEAVES,
    [oTYPE_TREE]=TYPE_TREE,
    [oTYPE_WOOD]=TYPE_WOOD,
    [oTYPE_GRASS]=TYPE_GRASS,
    [oTYPE_TNT]=TYPE_TNT,
    [oTYPE_DARK_WOOD]=TYPE_WOOD,
    [oTYPE_ORANGE_LEAVES]=TYPE_LEAVES,
    [oTYPE_YELLOW_LEAVES]=TYPE_LEAVES,
    [oTYPE_DARK_STONE]=TYPE_DARK_STONE,
    [oTYPE_GRASS2]=TYPE_GRASS2,
    [oTYPE_GRASS3]=TYPE_GRASS3,
    [oTYPE_BRICK]=TYPE_BRICK,
    [oTYPE_COBBLESTONE]=TYPE_COBBLESTONE,
    [oTYPE_GLASS]=TYPE_GLASS,
    [oTYPE_GREEN_CRYSTAL]=TYPE_CRYSTAL,
    [oTYPE_PINK_CRYSTAL]=TYPE_CRYSTAL,
    [oTYPE_PURPLE_CRYSTAL]=TYPE_CRYSTAL,
    [oTYPE_WHITE_CRYSTAL]=TYPE_CRYSTAL,
    [oTYPE_RED_LEAVES]=TYPE_LEAVES,
    [oTYPE_BLANK_RED]=TYPE_SAND,
    [oTYPE_BLANK_ORANGE]=TYPE_SAND,
    [oTYPE_BLANK_YELLOW]=TYPE_SAND,
    [oTYPE_BLANK_GREEN]=TYPE_SAND,
    [oTYPE_BLANK_BLUE]=TYPE_SAND,
    [oTYPE_BLANK_PURPLE]=TYPE_SAND,
    [oTYPE_BLANK_PINK]=TYPE_SAND
};
int convertColor[31]={
    [oTYPE_NONE]=0,
    [oTYPE_BEDROCK]=0,
    [oTYPE_STONE]=0,
    [oTYPE_DIRT]=0,
    [oTYPE_SAND]=0,
    [oTYPE_GREEN_LEAVES]=0,
    [oTYPE_TREE]=0,
    [oTYPE_WOOD]=0,
    [oTYPE_GRASS]=0,
    [oTYPE_TNT]=0,
    [oTYPE_DARK_WOOD]=38,
    [oTYPE_ORANGE_LEAVES]=20,
    [oTYPE_YELLOW_LEAVES]=21,
    [oTYPE_DARK_STONE]=0,
    [oTYPE_GRASS2]=0,
    [oTYPE_GRASS3]=0,
    [oTYPE_BRICK]=0,
    [oTYPE_COBBLESTONE]=0,
    [oTYPE_GLASS]=0,
    [oTYPE_GREEN_CRYSTAL]=22,
    [oTYPE_PINK_CRYSTAL]=26,
    [oTYPE_PURPLE_CRYSTAL]=25,
    [oTYPE_WHITE_CRYSTAL]=0,
    [oTYPE_RED_LEAVES]=19,
    [oTYPE_BLANK_RED]=19,
    [oTYPE_BLANK_ORANGE]=20,
    [oTYPE_BLANK_YELLOW]=21,
    [oTYPE_BLANK_GREEN]=22,
    [oTYPE_BLANK_BLUE]=24,
    [oTYPE_BLANK_PURPLE]=25,
    [oTYPE_BLANK_PINK]=26
};

int convertColumnIdx(any_t passedIn,any_t colToConvert){
	
    NSAutoreleasePool * pool = [[NSAutoreleasePool alloc] init];   
   
	ColumnIndex* colIndex=(ColumnIndex*)colToConvert;
	if(colIndex&&colIndex->chunk_offset+SIZEOF_OLDCOLUMN<=sfh->directory_offset){
        [oldFile seekToFileOffset:colIndex->chunk_offset];
        colIndex->chunk_offset=convert_offset;
        convert_offset+=SIZEOF_COLUMN;
        
        
        for(int cy=0;cy<CHUNKS_PER_COLUMN ;cy++){  	
            block8* blocks=(block8*)malloc(SIZEOF_OLDCHUNK);
            color8* colors=(color8*)malloc(SIZEOF_OLDCHUNK);
            memset(colors,0,SIZEOF_OLDCHUNK);
        
            NSData* data=[oldFile readDataOfLength:SIZEOF_OLDCHUNK];
            [data getBytes:blocks length:SIZEOF_OLDCHUNK];
            for(int i=0;i<SIZEOF_OLDCHUNK;i++){
                int type=blocks[i];
                if(type>30)type=oTYPE_STONE;
                blocks[i]=convertType[type];
                colors[i]=convertColor[type];
            }
            
			data=[NSData dataWithBytesNoCopy:blocks length:SIZEOF_OLDCHUNK freeWhenDone:FALSE];
			[newFile writeData:data];
            data=[NSData dataWithBytesNoCopy:colors length:SIZEOF_OLDCHUNK freeWhenDone:FALSE];
			[newFile writeData:data];        
            free(blocks);
            free(colors);
        }	
	}
     [pool release];
	return MAP_OK;
}
void FileManager::convertFile(NSString* file_name){
    NSFileManager* fm=[NSFileManager defaultManager];
    oldFile=[NSFileHandle fileHandleForReadingAtPath:file_name];    
    NSString* temp_name=[NSString stringWithFormat:@"%@/temp.map",documents];
    [fm removeItemAtPath:temp_name error:NULL];
    [fm createFileAtPath:temp_name contents:nil attributes:nil];
    newFile=[NSFileHandle fileHandleForWritingAtPath:temp_name];
    
    sfh=(WorldFileHeader*)[[oldFile readDataOfLength:sizeof(WorldFileHeader)] bytes];
    sfh->version=2;
    file_version=2;
    saveFile=oldFile;
    count=0;
	this->readDirectory();
	NSLog(@"read %d old colidx's newfile: %@",count,newFile);  
    
    
    convert_offset=sizeof(WorldFileHeader);
    [newFile seekToFileOffset:convert_offset];
	hashmap_iterate(indexes, convertColumnIdx, NULL);
    
    sfh->directory_offset=convert_offset;    
    saveFile=newFile;
    this->fwriteDirectory();
    
    [newFile seekToFileOffset:0];
    NSData* dh=[NSData dataWithBytesNoCopy:sfh length:sizeof(WorldFileHeader) freeWhenDone:FALSE];
    [newFile writeData:dh];
    
    [oldFile closeFile];
    [newFile closeFile];    
    
    [fm removeItemAtPath:file_name error:NULL];
    NSError* err=nil;
    [fm moveItemAtPath:temp_name toPath:file_name error:&err];
    
    NSLog(@"err:%@",[err localizedDescription]);

    
}
extern bool SUPPORTS_OGL2;
extern float P_ZFAR;
  static int last_spawn_location=-1;

int FileManager::probeWorldHeight(NSString* name,BOOL fromArchive){
    if(emod::WorldSource::isArchiveName(cpstring(name)))return T_HEIGHT_DEFAULT;   // never opened as a world
    if(!worldExists(cpstring(name),fromArchive)){
        // A world that doesn't exist yet is 64z by default -- unless the New World screen parked
        // an explicit 256z choice for it (see eden_menu_take_pending_world_height's header). This
        // is what makes "new worlds stay 64z unless the player explicitly picks 256z" true by
        // construction rather than by every caller remembering to ask.
        return (eden_menu_take_pending_world_height()==T_HEIGHT_MAX)?T_HEIGHT_MAX:T_HEIGHT_DEFAULT;
    }
    NSString* file_name=[NSString stringWithFormat:@"%@/%@",documents,name];
    if(isEmodName(name)){
        // No probe needed beyond the file header: an `.emod` states its height (bands 4 | 16).
        emod::EdenWorldStore st;
        if(!st.open([file_name UTF8String],false))return T_HEIGHT_DEFAULT;
        return st.bands()==16?T_HEIGHT_MAX:T_HEIGHT_DEFAULT;
    }
    // Earliest point on the load path that touches this specific file (World::loadWorld calls this
    // before allocateMemory), so it is where an interrupted in-place save gets rolled back --
    // before anything reads the header it would otherwise trust. Idempotent and free when there is
    // no journal, which is the normal case.
    this->recoverInterruptedSave(file_name);
    NSFileHandle* fh=[NSFileHandle fileHandleForReadingAtPath:file_name];
    if(fh==NULL)return T_HEIGHT_DEFAULT;
    NSData* headerData=[fh readDataOfLength:sizeof(WorldFileHeader)];
    int height=T_HEIGHT_DEFAULT;
    if([headerData length]==sizeof(WorldFileHeader)){
        const WorldFileHeader* h=(const WorldFileHeader*)[headerData bytes];
        if(h->version>=FILE_VERSION_256Z&&h->version<=FILE_VERSION_256Z_MAX)height=T_HEIGHT_MAX;
        else if(h->version>=1&&h->version<FILE_VERSION_256Z){
            // S.3b: the version is not a height marker -- the 2026 game stamps v2 on 256z worlds
            // too. Read the directory (directory_offset..EOF; the load reads it next anyway) and
            // let the creature gap decide, the sibling editor's rule (emod::eden_detect_bands).
            // Anything outside 1..6 keeps the 64z answer: loadWorld refuses or legacy-converts it.
            unsigned long long fileEnd=[fh seekToEndOfFile];
            if(h->directory_offset>=sizeof(WorldFileHeader)&&h->directory_offset<=fileEnd
               &&fileEnd-h->directory_offset<=(256ull<<20)){
                [fh seekToFileOffset:h->directory_offset];
                NSData* dir=[fh readDataOfLength:(NSUInteger)(fileEnd-h->directory_offset)];
                if(emod::eden_detect_bands_raw((const uint8_t*)h,(const uint8_t*)[dir bytes],[dir length])==16)
                    height=T_HEIGHT_MAX;
            }
        }
    }
    [fh closeFile];
    return height;
}

// ---- 256z Stage 3 item 5: in-app "Convert to 64z" (Settings -> Storage tab) ----
//
// A from-scratch C++ port of web/tools/eden-convert.js's `analyseAndConvertTo64` (Stage 1), not a
// call into it -- that tool is a standalone Node script (fs.readSync/writeSync over a plain fd)
// with no wasm/engine dependency, which is exactly what makes it independently trustworthy as a
// recovery/authoring tool outside the browser. This function is the SAME algorithm restated over
// NSFileHandle so the Storage tab can do it without shelling out to Node, which doesn't exist in a
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
ConvertTo64Report FileManager::convertWorldTo64(NSString* name){
    ConvertTo64Report report; memset(&report,0,sizeof(report));
    if(isEmodName(name)){
        snprintf(report.error,sizeof(report.error),"not supported for .emod worlds yet");
        return report;
    }

    if(!worldExists(cpstring(name),FALSE)){
        snprintf(report.error,sizeof(report.error),"world does not exist");
        return report;
    }
    // This does raw file surgery behind the engine's back; the currently-open world has its own
    // live NSFileHandle and in-memory directory/creature state that this function knows nothing
    // about. Refuse rather than race it -- the player can convert after returning to the menu.
    if(World::getWorld&&World::getWorld->doneLoading!=0&&World::getWorld->terrain
       &&World::getWorld->terrain->world_name
       &&[World::getWorld->terrain->world_name isEqualToString:name]){
        snprintf(report.error,sizeof(report.error),
                 "cannot convert the world that is currently open -- return to the main menu first");
        return report;
    }

    NSString* file_name=[NSString stringWithFormat:@"%@/%@",documents,name];
    NSFileHandle* fh=[NSFileHandle fileHandleForReadingAtPath:file_name];
    if(fh==NULL){
        snprintf(report.error,sizeof(report.error),"could not open %s",[file_name UTF8String]);
        return report;
    }
    NSData* headerData=[fh readDataOfLength:sizeof(WorldFileHeader)];
    if([headerData length]!=sizeof(WorldFileHeader)){
        [fh closeFile];
        snprintf(report.error,sizeof(report.error),"truncated header (save file too short)");
        return report;
    }
    WorldFileHeader header; memcpy(&header,[headerData bytes],sizeof(WorldFileHeader));
    if(header.version>FILE_VERSION_256Z_MAX){
        [fh closeFile];
        snprintf(report.error,sizeof(report.error),
                 "unsupported world format (header version %d, newer than this build knows how to read)",header.version);
        return report;
    }
    unsigned long long fileSize=[fh seekToEndOfFile];
    if(header.directory_offset<sizeof(WorldFileHeader)||header.directory_offset>fileSize){
        [fh closeFile];
        snprintf(report.error,sizeof(report.error),
                 "directory offset outside file bounds (corrupt or truncated save)");
        return report;
    }
    // S.3b: below version 5 the header does not say the height (the 2026 game stamps v2 on 256z
    // worlds), so ask the file -- the same rule probeWorldHeight uses.
    if(header.version<FILE_VERSION_256Z){
        [fh seekToFileOffset:header.directory_offset];
        NSData* dir=[fh readDataOfLength:(NSUInteger)(fileSize-header.directory_offset)];
        if(header.version<1||emod::eden_detect_bands_raw((const uint8_t*)&header,(const uint8_t*)[dir bytes],[dir length])!=16){
            [fh closeFile];
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
    [fh seekToFileOffset:header.directory_offset];
    for(long i=0;i<dirCount;i++){
        NSData* d=[fh readDataOfLength:DIR_ENTRY_SIZE];
        if([d length]!=DIR_ENTRY_SIZE)break;
        ColumnIndex ci; memcpy(&ci,[d bytes],sizeof(ci));
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
        [fh closeFile];
        snprintf(report.error,sizeof(report.error),
                 "%d of %d columns are not a clean 131072 B record -- looks like a damaged file, not a user save",
                 odd,(int)sorted.size());
        return report;
    }

    report.columns=(int)sorted.size();

    // ---- write the scratch output ----
    NSString* temp_name=[file_name stringByAppendingString:@".64zconv"];
    NSFileManager* nsfm=[NSFileManager defaultManager];
    [nsfm removeItemAtPath:temp_name error:NULL]; // drop any orphan from a previous failed attempt
    // fileHandleForWritingAtPath: (not ...ForUpdatingAtPath:) deliberately: temp_name does not exist
    // yet (just removed above), so its eager backup-before-overwrite is a no-op, whereas the
    // Updating variant's DEFERRED backup would fire on our first write and leave a stray
    // "<temp_name>.bak" of the empty scratch file behind.
    NSFileHandle* wh=[NSFileHandle fileHandleForWritingAtPath:temp_name];
    if(wh==NULL){
        [fh closeFile];
        snprintf(report.error,sizeof(report.error),"could not create scratch file %s",[temp_name UTF8String]);
        return report;
    }
    [wh seekToFileOffset:sizeof(WorldFileHeader)]; // header is written last, once directory_offset is known

    std::vector<unsigned char> col(COL_256);
    for(size_t i=0;i<sorted.size();i++){
        Entry* e=sorted[i];
        std::fill(col.begin(),col.end(),0); // short spans read as air, never a neighbour's bytes
        if(e->span>0){
            [fh seekToFileOffset:e->offset];
            NSData* d=[fh readDataOfLength:(NSUInteger)e->span];
            memcpy(col.data(),[d bytes],[d length]);
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
                BOOL orphan=((bottom>=TYPE_DOOR1&&bottom<=TYPE_DOOR4)&&above==TYPE_DOOR_TOP)||
                            ((bottom>=TYPE_PORTAL1&&bottom<=TYPE_PORTAL4)&&above==TYPE_PORTAL_TOP);
                if(orphan){
                    col[topBand+cc]=0;
                    col[topBand+4096+cc]=0; // paint byte for the same voxel
                    report.doorsOrphaned++;
                }
            }
        }
        e->newOffset=[wh offsetInFile];
        NSData* outCol=[NSData dataWithBytesNoCopy:col.data() length:(NSUInteger)COL_64 freeWhenDone:FALSE];
        [wh writeData:outCol];
    }

    // ---- creatures: keep slot positions, relocate survivors from slots >= 200, drop y >= 64 ----
    std::vector<unsigned char> srcCre(creatureBytes);
    if(creatureBytes>0){
        unsigned long long start=header.directory_offset-creatureBytes;
        if(start>=sizeof(WorldFileHeader)){
            [fh seekToFileOffset:start];
            NSData* d=[fh readDataOfLength:(NSUInteger)creatureBytes];
            memcpy(srcCre.data(),[d bytes],std::min((NSUInteger)creatureBytes,[d length]));
        }
    }
    [fh closeFile];
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
    [wh writeData:[NSData dataWithBytesNoCopy:outCre.data() length:outCre.size() freeWhenDone:FALSE]];

    // ---- directory: original slot order, patched offsets ----
    unsigned long long directoryOffset=[wh offsetInFile];
    std::vector<unsigned char> dirBuf(entries.size()*DIR_ENTRY_SIZE);
    for(size_t i=0;i<entries.size();i++){
        ColumnIndex ci; ci.x=entries[i].x; ci.z=entries[i].z; ci.chunk_offset=entries[i].newOffset;
        memcpy(dirBuf.data()+i*DIR_ENTRY_SIZE,&ci,sizeof(ci));
    }
    if(!dirBuf.empty())[wh writeData:[NSData dataWithBytesNoCopy:dirBuf.data() length:dirBuf.size() freeWhenDone:FALSE]];

    // ---- header: patched copy of the original, player/home clamped into the 64z ceiling ----
    WorldFileHeader outHeader=header;
    outHeader.directory_offset=directoryOffset;
    outHeader.version=FILE_VERSION;
    if(!(outHeader.pos.y<63)){outHeader.pos.y=63; report.posClamped=TRUE;}
    if(outHeader.pos.y<0){outHeader.pos.y=0; report.posClamped=TRUE;}
    if(!(outHeader.home.y<63)){outHeader.home.y=63; report.homeClamped=TRUE;}
    if(outHeader.home.y<0){outHeader.home.y=0; report.homeClamped=TRUE;}
    [wh seekToFileOffset:0];
    [wh writeData:[NSData dataWithBytesNoCopy:&outHeader length:sizeof(outHeader) freeWhenDone:FALSE]];
    [wh closeFile];

    // ---- commit: only now does the original get replaced ----
    [nsfm removeItemAtPath:file_name error:NULL];
    if(![nsfm moveItemAtPath:temp_name toPath:file_name error:NULL]){
        snprintf(report.error,sizeof(report.error),"converted successfully but could not replace the original file");
        return report;
    }
    report.ok=TRUE;
    return report;
}

void FileManager::loadWorld(NSString* name,BOOL fromArchive){
   
    
    
    
	Terrain* ter=World::getWorld->terrain;
		ter->clearBlocks();
	Player* player=World::getWorld->player;
    this->closeWorld();   // a previous session's `.emod` store (warp, re-load); no-op for `.eden`
    worldTrailer.clear(); // D.3a: a brand-new world has none; the two load branches below read theirs
    if(emod::WorldSource::isArchiveName(cpstring(name))){
        // An archive that was not converted can't be played, and must never reach the .eden branch
        // below: its "header" is compressed bytes, which that branch would try to "upgrade" in place.
        eden_report_load_failure([name UTF8String],"this archive could not be converted to a world");
        return;
    }
    if(imgHash!=NULL){
        [imgHash release];
        imgHash=NULL;
    }
    World::getWorld->player->reset();
    // Belt and braces: probeWorldHeight() normally gets here first, but nothing structurally
    // guarantees every caller went through it, and rolling back twice is a no-op.
    if(worldExists(cpstring(name),fromArchive))
        this->recoverInterruptedSave([NSString stringWithFormat:@"%@/%@",documents,name]);
	if(!worldExists(cpstring(name),fromArchive)){
     
        
        extern int g_terrain_type;
        
        printg("making new world : %d\n",g_terrain_type);
        
      //  clear();
        BOOL gen_default=FALSE;
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
            genflat=TRUE;
        }else if(g_terrain_type==9){
            gen_default=TRUE;
        }

		this->clearDirectory();
        if(genflat)ter->tgen->LEVEL_SEED= 0;
        else if(gen_default){
           
            
            ter->tgen->LEVEL_SEED=DEFAULT_LEVEL_SEED;
            
        }else{
             ter->tgen->LEVEL_SEED=arc4random()%300000;
            
            
        }
		int centerChunk=4096;
        int r=T_SIZE/CHUNK_SIZE/2;

		ter->level_seed=ter->tgen->LEVEL_SEED;
		
        
        
        
	
		
		Vector temp;
		
        int tempyaw=90;
      
        if(gen_default){
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
            chunkOffsetX=temp.x/CHUNK_SIZE-T_RADIUS;
            chunkOffsetZ=temp.z/CHUNK_SIZE-T_RADIUS;
            
        }else{
            chunkOffsetX=centerChunk-r;
            chunkOffsetZ=centerChunk-r;
            temp.x=centerChunk*CHUNK_SIZE+CHUNK_SIZE/2;
            temp.z=centerChunk*CHUNK_SIZE+CHUNK_SIZE/2;
            temp.y=T_HEIGHT-10;
        }
        
        
        
        for(int x=centerChunk-r;x<centerChunk+r;x++){
            
            for(int z=centerChunk-r;z<centerChunk+r;z++){
                
                readColumn(x,z,saveFile);
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
		this->saveWorld();
		//[ter unloadTerrain:FALSE];
		//[self loadWorld:name];
	}else if(isEmodName(name)){
        if(!this->loadEmodWorld(name))return;
    }else{
              
		NSString* file_name=[NSString stringWithFormat:@"%@/%@",documents,name];
        
        if(fromArchive){
          //  DecompressWorld([file_name cStringUsingEncoding:NSUTF8StringEncoding]);
        }

       
		saveFile=[NSFileHandle fileHandleForUpdatingAtPath:file_name];
        // Perf-audit C4 ("no validation, no recovery"): a save interrupted before its header's own
        // sizeof(WorldFileHeader) bytes were (re)written arrives here short -- the temp+rename
        // atomicity in saveWorld() above stops this port's OWN writes from ever leaving file_name
        // in that state, but this guards against any `.eden` that arrives short some other way (a
        // hand-edited file, a backup restored mid-write, a bug elsewhere). Report and bail instead
        // of casting a too-short buffer to WorldFileHeader* and reading past its end.
        NSData* headerData=[saveFile readDataOfLength:sizeof(WorldFileHeader)];
        if([headerData length]!=sizeof(WorldFileHeader)){
            [saveFile closeFile];
            eden_report_load_failure([name UTF8String],"truncated header (save file too short)");
            return;
        }
		sfh=(WorldFileHeader*)[headerData bytes];
        file_version=sfh->version;
        printg("FILE VERSION: %d\n",file_version);
        // 256z ("New Dawn") worlds stamp version 5 or 6 -- same header layout, but 16 chunk-bands
        // per column (131072-byte stride) instead of 4 (32768). Those two are READ and PLAYED as of
        // 2026-08-06 (Stage 2): World::loadWorld has already probed this header and called
        // eden_set_world_height(256), so SIZEOF_COLUMN/CHUNKS_PER_COLUMN below are already the 256z
        // values. Anything ABOVE 6 is a format nobody here has seen; it is inside the 1..1000 range
        // so it would sail past the legacy-convert branch below, read at some stride we guessed,
        // and be overwritten by the first autosave. Refuse it instead -- that is Stage 0's whole
        // point and it stays, just narrowed from ">=5" to "above what we know".
        if(sfh->version>FILE_VERSION_256Z_MAX&&sfh->version<=1000){
            [saveFile closeFile];
            eden_report_load_failure([name UTF8String],"unsupported world format (newer than any .eden version this build knows how to read)");
            return;
        }
        if(sfh->version<1||sfh->version>1000){  //old legacy convert code, no longer really supported
            [saveFile closeFile];
           
            NSLog(@"converting file");
            convertingWorld=TRUE;
            convertFile(file_name);
            
            NSLog(@"done converting file");
          
            saveFile=[NSFileHandle fileHandleForUpdatingAtPath:file_name];		
            sfh=(WorldFileHeader*)[[saveFile readDataOfLength:sizeof(WorldFileHeader)] bytes];
            convertingWorld=FALSE;
        }
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
        if(sfh->hash[32]==0)
            NSLog(@"image hash is %s",sfh->hash);
        if(imgHash!=NULL){
            [imgHash release];
            imgHash=NULL;
        }
        imgHash=[[NSString alloc] initWithCString:sfh->hash encoding:NSUTF8StringEncoding];
		ter->level_seed=sfh->level_seed;
		ter->tgen->LEVEL_SEED=ter->level_seed;
		cur_dir_offset=sfh->directory_offset;
       World::getWorld->hud->goldencubes= sfh->goldencubes;
		ter->home=sfh->home;
		player->pos=sfh->pos;
		player->yaw=sfh->yaw;
         extern Vector colorTable[256];
       /* if(sfh->skycolor<=0||sfh->skycolor>NUM_COLORS){
           World::getWorld->terrain.final_skycolor=colorTable[14];
            printg("skycolor oob setting sky color to beautiful blue\n");
        }else{
             printg("skycolor setting sky color to : %d\n",sfh->skycolor);
        */
       // World::getWorld->terrain.final_skycolor=colorTable[sfh->skycolor];
        
        for(int i=0;i<4;i++){
            for(int j=0;j<4;j++){
                regionSkyColors[i][j]=(int)(sfh->skycolors[i*4+j]);
            }
        }
        

        
        // Perf-audit C4: the ColumnIndex directory lives at directory_offset and is read to EOF
        // (docs/eden-file-format.md) -- a save interrupted mid-write is exactly a file whose
        // directory_offset points past the real end of the file, or before the header even ends.
        // Catch that here rather than letting readDirectory()/readColumn() walk off the end of a
        // truncated file.
        {
            unsigned long long fileEnd=[saveFile seekToEndOfFile];
            if(sfh->directory_offset<sizeof(WorldFileHeader)||(unsigned long long)sfh->directory_offset>fileEnd){
                [saveFile closeFile];
                eden_report_load_failure([name UTF8String],"directory offset outside file bounds (corrupt or truncated save)");
                return;
            }
        }
		this->readDirectory();
		this->loadTrailer(name,dir_trailer,dir_trailer_len);
		//NSLog(@"indexes: %d",hashmap_length(indexes));
		//NSLog(@"loading level_seed: %d",ter.level_seed);
		//NSLog(@"directory offset: %d entries: %d",(int)sfh->directory_offset,hashmap_length(indexes));
		oldOffsetX=chunkOffsetX;
		oldOffsetZ=chunkOffsetZ;
		
		chunkOffsetX=player->pos.x/CHUNK_SIZE-T_RADIUS;
		chunkOffsetZ=player->pos.z/CHUNK_SIZE-T_RADIUS;
		//NSLog(@"chunkOffsets: %d %d",chunkOffsetX,chunkOffsetZ);
		/*sfh->pos.x-=chunkOffsetX*CHUNK_SIZE;
		sfh->pos.z-=chunkOffsetZ*CHUNK_SIZE;
		sfh->pos.x*=BLOCK_SIZE; 
		sfh->pos.z*=BLOCK_SIZE;
          
		*/player->pos=sfh->pos;
       
        printg("reading at co %d, %d    player pos %d, %d)\n",chunkOffsetX,chunkOffsetZ,(int)player->pos.x,(int)player->pos.z);
        		//NSLog(@"player pos load: %f %f %f",player.pos.x,player.pos.y,player.pos.z);
		int r=T_RADIUS;
	//	int asdf=0;
        
		for(int x=chunkOffsetX;x<chunkOffsetX+2*r;x++){
			for(int z=chunkOffsetZ;z<chunkOffsetZ+2*r;z++){
			//	NSLog(@"lch:%d",asdf++);
				readColumn(x,z,saveFile);
                World::getWorld->terrain->counter++;
			}
		}
        //if(CREATURES_ON)
        this->LoadCreatures();
		//[ter updateAllImportantChunks];
		NSLog(@"done");
		[saveFile closeFile];
        
		
		
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

    Input::getInput()->clearAll();
    World::getWorld->effects->clearAllEffects();
    World::getWorld->hud->worldLoaded();
	updateSkyColor1(World::getWorld->player,TRUE);
    extern BOOL loaded_new_terrain;
    loaded_new_terrain=TRUE;

}


// =============================================================================================
// Stage S / S.4 + S.5: `.emod` worlds -- docs/emod-file-format.md, Classes/EdenWorldStore.h
// =============================================================================================
// A world is an `.emod` iff its file name ends in ".emod". Everything above routes on that one
// test (isEmodName), so an `.eden` world takes exactly the code it always did, and a name that
// ends in ".emod" never reaches a NSFileHandle or the directory hashmap. The container is
// append-only: a save appends the dirty columns + header + creatures + a COMMIT in one write and
// fsyncs; nothing a previous commit wrote is ever overwritten (so no scratch copy, no journal, no
// threshold -- docs/save-load.md).

BOOL FileManager::isEmodName(NSString* name){
    return name!=nil&&[name hasSuffix:@EMOD_WORLD_EXTENSION];
}

BOOL FileManager::isArchiveName(NSString* name){
    // Never a backup slot or an export the engine itself left beside a world (`.eden.bak.zip`,
    // stock's CompressWorld `.eden.zip` of a world that is still listed): those are not new worlds.
    if(name==nil||[name hasSuffix:@".bak.zip"]||[name hasSuffix:@".bak.gz"])return FALSE;
    return emod::WorldSource::isArchiveName(cpstring(name));
}

NSString* FileManager::newWorldFileName(){
    return [NSString stringWithFormat:(g_world_format==1?@"%@.emod":@"%@.eden"),genhash()];
}

void FileManager::closeWorld(){
    if(emodStore){delete emodStore;emodStore=NULL;}
    if(emodHeader){free(emodHeader);emodHeader=NULL;}
}

BOOL FileManager::emodOpen(){return emodStore!=NULL;}

BOOL FileManager::loadEmodWorld(NSString* name){
    Terrain* ter=World::getWorld->terrain;
    Player* player=World::getWorld->player;
    std::string path=docs+"/"+cpstring(name);
    emod::EdenWorldStore* st=new emod::EdenWorldStore();
    if(!st->open(path.c_str(),true)){
        std::string why="unreadable .emod world: "+st->error();
        delete st;
        eden_report_load_failure([name UTF8String],why.c_str());
        return FALSE;
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
        eden_report_load_failure([name UTF8String],"no readable world header in this .emod (corrupt save)");
        return FALSE;
    }
    if(st->bands()!=CHUNKS_PER_COLUMN){
        // World::loadWorld probed the height from this same file; a mismatch is a caller bug, and
        // reading 4-band records into a 16-band window (or the reverse) would corrupt the world.
        delete st;
        eden_report_load_failure([name UTF8String],"world height does not match the .emod's band count");
        return FALSE;
    }
    std::vector<uint8_t> craw;
    if(st->hasRecord(emod::T_CREATURES))st->readRecord(emod::T_CREATURES,craw);
    // D.3a: an unreadable SIGN_TRAILER (READ_AIR_DAMAGED) loads as none -- and stays clean, so no save
    // writes an empty one over whatever an earlier version still holds.
    std::vector<uint8_t> traw;
    if(st->hasRecord(emod::T_SIGN_TRAILER)&&st->readRecord(emod::T_SIGN_TRAILER,traw)>emod::EdenWorldStore::READ_FALLBACK)traw.clear();
    closeWorld();
    // The `.eden` directory maps belong to whatever world was open before: nothing below reads
    // them for an `.emod`, and leaving a previous world's rows in them is how one would one day.
    this->clearDirectory();
    this->clearColumnSpans();
    emodStore=st;
    emodHeader=(WorldFileHeader*)malloc(sizeof(WorldFileHeader));
    memcpy(emodHeader,&raw[0],sizeof(WorldFileHeader));
    sfh=emodHeader;
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
    if(imgHash!=NULL){[imgHash release];imgHash=NULL;}
    imgHash=[[NSString alloc] initWithCString:sfh->hash encoding:NSUTF8StringEncoding];
    ter->level_seed=sfh->level_seed;
    ter->tgen->LEVEL_SEED=ter->level_seed;
    cur_dir_offset=sfh->directory_offset;
    World::getWorld->hud->goldencubes=sfh->goldencubes;
    ter->home=sfh->home;
    player->pos=sfh->pos;
    player->yaw=sfh->yaw;
    for(int i=0;i<4;i++){
        for(int j=0;j<4;j++){
            regionSkyColors[i][j]=(int)(sfh->skycolors[i*4+j]);
        }
    }
    oldOffsetX=chunkOffsetX;
    oldOffsetZ=chunkOffsetZ;
    chunkOffsetX=player->pos.x/CHUNK_SIZE-T_RADIUS;
    chunkOffsetZ=player->pos.z/CHUNK_SIZE-T_RADIUS;
    player->pos=sfh->pos;
    int r=T_RADIUS;
    for(int x=chunkOffsetX;x<chunkOffsetX+2*r;x++){
        for(int z=chunkOffsetZ;z<chunkOffsetZ+2*r;z++){
            readColumn(x,z,nil);
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
    this->loadTrailer(name,traw.empty()?NULL:&traw[0],traw.size());
    return TRUE;
}

// ---- Stage D / D.3a: the trailer model ------------------------------------------------------
WorldTrailer* FileManager::trailer(){return &worldTrailer;}

int FileManager::reportTrailerRefusal(int result){
    if(result!=WorldTrailer::TR_OK&&World::getWorld&&World::getWorld->hud&&World::getWorld->hud->sb)
        World::getWorld->hud->sb->setStatus([NSString stringWithUTF8String:WorldTrailer::resultMessage(result)],3);
    return result;
}

// The world's captured trailer bytes -> the model, then the 2026 game's sidecars. That game keeps a
// local world's signs and command blocks in `signs_<file>.dat` / `cmd_<file>.dat` next to it (one bare
// section each; `<file>` is the world's `.eden` name), and an exported `.eden` does not carry them --
// so a world copied over together with its sidecars would lose them. A sidecar is adopted only for a
// section the trailer does not already have (the trailer wins), which marks the model dirty: the
// first save writes it into the world, after which the sidecar is never read again.
void FileManager::loadTrailer(NSString* name,const unsigned char* bytes,unsigned long long len){
    adoptedSidecars.clear();
    if(!worldTrailer.load(bytes,(size_t)len))
        printg("trailer: %llu B kept verbatim, not editable: %s\n",len,worldTrailer.why().c_str());
    std::string stem=emod::WorldSource::stemOf(cpstring(name));
    static const char* const kinds[2]={"signs_","cmd_"};
    for(int k=0;k<2;k++){
        std::string path=docs+"/"+kinds[k]+stem+".eden.dat";
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
        printg("trailer: sidecar %s%s.eden.dat (%llu B): %s\n",kinds[k],stem.c_str(),(unsigned long long)n,
               r==WorldTrailer::TR_OK?"adopted":r==WorldTrailer::TR_EXISTS?"ignored (the world has its own)":"not readable");
    }
    printg("trailer: %s\n",worldTrailer.describe().c_str());
}

void FileManager::saveEmodWorld(Vector warp){
    Terrain* ter=World::getWorld->terrain;
    NSString* name=ter->world_name;
    std::string path=docs+"/"+cpstring(name);

    if(!emodStore){
        // A brand-new world (loadWorld's generator branch saves it once, straight away): create the
        // file. It only becomes visible at the first commit, which carries header + creatures.
        emod::EdenWorldStore* st=new emod::EdenWorldStore();
        BOOL ok=worldExists(cpstring(name),FALSE)
            ?st->open(path.c_str(),true)
            :st->create(path.c_str(),g_world_height>=T_HEIGHT_MAX?16:4,(uint64_t)time(NULL));
        if(!ok){
            printg("saveWorld: could not open %s: %s -- SKIPPING this save\n",path.c_str(),st->error().c_str());
            delete st;
            return;
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
    if(sel&&sel->display_name){
        memset(h.name,0,sizeof(h.name));
        [sel->display_name getCString:h.name maxLength:49 encoding:NSUTF8StringEncoding];
    }
    if(imgHash==NULL)imgHash=@"";
    memset(h.hash,0,sizeof(h.hash));
    [imgHash getCString:h.hash maxLength:33 encoding:NSUTF8StringEncoding];
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
            BOOL dirty=FALSE;
            for(int cy=0;cy<CHUNKS_PER_COLUMN;cy++){
                if(ter->chunkTable[threeToOne(cx,cy,cz)]->modified){dirty=TRUE;break;}
            }
            if(!dirty||twoToOneTest(cx,cz)==0)continue;
            for(int cy=0;cy<CHUNKS_PER_COLUMN;cy++){
                TerrainChunk* c=ter->chunkTable[threeToOne(cx,cy,cz)];
                memcpy(&col[(size_t)cy*emod::BAND],c->pblocks,CHUNK_SIZE3*sizeof(block8));
                memcpy(&col[(size_t)cy*emod::BAND+CHUNK_SIZE3*sizeof(block8)],c->pcolors,CHUNK_SIZE3*sizeof(color8));
            }
            if(!emodStore->putColumn(cx,cz,&col[0])){
                printg("saveWorld: could not stage column (%d,%d): %s\n",cx,cz,emodStore->error().c_str());
                emodStore->abortBatch();
                return;
            }
            saved.push_back(std::make_pair(cx,cz));
        }
    }
    SaveModels();
    emodStore->putRecord(emod::T_CREATURES,creatureData,sizeof(EntityData)*(size_t)MAX_CREATURES_SAVED);
    emodStore->putRecord(emod::T_WORLD_HEADER,&h,sizeof(h));
    // Stage D / D.3a: an edited trailer is a new SIGN_TRAILER record in this batch (an empty one
    // removes it). Untouched, nothing is written: the last committed record stays live.
    BOOL trailerSaving=worldTrailer.dirty();
    if(trailerSaving){
        std::vector<uint8_t> t=worldTrailer.encode();
        if(!emodStore->putRecord(emod::T_SIGN_TRAILER,t.empty()?NULL:&t[0],t.size())){
            printg("saveWorld: could not stage the sign trailer: %s\n",emodStore->error().c_str());
            emodStore->abortBatch();
            return;
        }
    }
    if(!emodStore->commitBatch()){
        NSLog(@"saveWorld: .emod commit failed (%s) -- save skipped, last commit intact",emodStore->error().c_str());
        return;
    }
    if(trailerSaving)trailerSaved();
    for(size_t i=0;i<saved.size();i++){
        for(int cy=0;cy<CHUNKS_PER_COLUMN;cy++)
            ter->chunkTable[threeToOne(saved[i].first,cy,saved[i].second)]->modified=FALSE;
    }
    if(!emodHeader)emodHeader=(WorldFileHeader*)malloc(sizeof(WorldFileHeader));
    memcpy(emodHeader,&h,sizeof(h));
    sfh=emodHeader;
}

// ---- converting a `.eden` (S.5) -----------------------------------------------------------------
// The player drops a `.eden`, a `.eden.gz`, a `.gz`, a `.zip` or a `.eden.zip` into the saves
// directory; all five are "source worlds". Each is listed, flagged, and converted when played;
// an archive converts WHILE IT INFLATES (emod::WorldSource -> EdenEmodConverter), so no inflated
// `.eden` is ever written. Only with g_world_format == 1 (S.4: flag off is today's behaviour, byte
// for byte), and only where the Storage/import/export UI knows `.emod` -- i.e. not on web yet (S.5's
// remaining rows): there a `.eden` keeps playing in place exactly as before. EDEN_NO_CONVERT=1
// switches it off for a harness that wants the `.eden` path with the flag on.
BOOL FileManager::conversionEnabled(){
#ifdef __EMSCRIPTEN__
    return FALSE;
#else
    if(g_world_format!=1)return FALSE;
    const char* e=getenv("EDEN_NO_CONVERT");
    return !(e&&e[0]&&e[0]!='0');
#endif
}
static BOOL isSourceName(NSString* name){
    return [name hasSuffix:@".eden"]||emod::WorldSource::isArchiveName(cpstring(name));
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
static std::string stemOf(NSString* name){return emod::WorldSource::stemOf(cpstring(name));}
static unsigned long long le64(const uint8_t* p){
    unsigned long long v=0;
    for(int i=7;i>=0;i--)v=(v<<8)|p[i];
    return v;
}
// Does this `.emod` come from that source? PROVENANCE (docs/emod-file-format.md) records the
// source's size and mtime and its file name. A plain `.eden` pairs by size (the inflated size of an
// archive is unknowable without inflating it); an archive pairs by name + mtime. (Plan §4 also
// allows the header+directory SHA-256, which stays recorded in the file for a stricter check.)
static BOOL emodPairsWith(const std::string& emodPath,NSString* srcName,const std::string& srcPath){
    emod::EdenWorldStore st;
    if(!st.open(emodPath.c_str(),false))return FALSE;
    std::vector<uint8_t> p;
    if(st.readRecord(emod::T_PROVENANCE,p)>emod::EdenWorldStore::READ_FALLBACK||p.size()<80)return FALSE;
    if(![srcName hasSuffix:@".eden"]){
        uint32_t nl=p[76]|(p[77]<<8)|(p[78]<<16)|((uint32_t)p[79]<<24);
        if(p.size()<80+(size_t)nl)return FALSE;
        std::string nm((const char*)&p[80],nl);
        return nm==cpstring(srcName)&&(long long)le64(&p[16])==fileMtimeOf(srcPath);
    }
    return le64(&p[8])==fileSizeOf(srcPath);
}
static std::string pairedEmodPath(const std::string& dir,NSString* srcName){
    std::string src=dir+"/"+cpstring(srcName);
    if(fileSizeOf(src)==0)return std::string();
    std::string stem=stemOf(srcName);
    for(int k=1;k<=9;k++){
        std::string cand=dir+"/"+stem+(k==1?std::string():"-"+std::to_string(k))+".emod";
        if(fileSizeOf(cand)==0)continue;
        if(emodPairsWith(cand,srcName,src))return cand;
    }
    return std::string();
}

BOOL FileManager::edenHasPairedEmod(NSString* edenName){
    if(isEmodName(edenName)||!isSourceName(edenName))return FALSE;
    return !pairedEmodPath(docs,edenName).empty();
}

NSString* FileManager::pairedEmodFor(NSString* srcName){
    if(isEmodName(srcName)||!isSourceName(srcName))return nil;
    std::string p=pairedEmodPath(docs,srcName);
    return p.empty()?nil:nsstring(p.substr(docs.size()+1));
}

NSString* FileManager::originalOf(NSString* emodName){
    if(!isEmodName(emodName))return nil;
    NSArray* names=[[NSFileManager defaultManager] contentsOfDirectoryAtPath:documents error:NULL];
    for(int i=0;i<(int)[names count];i++){
        NSString* f=[names objectAtIndex:i];
        if(isEmodName(f)||!isSourceName(f))continue;
        NSString* e=pairedEmodFor(f);
        if(e&&[e isEqualToString:emodName])return f;
    }
    return nil;
}

BOOL FileManager::removeOriginal(NSString* emodName){
    NSString* o=originalOf(emodName);
    if(!o)return FALSE;
    NSString* p=[NSString stringWithFormat:@"%@/%@",documents,o];
    BOOL ok=[[NSFileManager defaultManager] removeItemAtPath:p error:NULL];
    printg("emod: %s the original %s\n",ok?"deleted":"could not delete",[p UTF8String]);
    return ok;
}

BOOL FileManager::worldNeedsConversion(NSString* name){
    if(!conversionEnabled()||isEmodName(name)||!isSourceName(name))return FALSE;
    if(fileSizeOf(docs+"/"+cpstring(name))<sizeof(WorldFileHeader))return FALSE;
    return pairedEmodPath(docs,name).empty();
}

// One conversion at a time, a time slice per convertStep() call. The work is emod::ImportJob
// (Classes/EdenWorldSource.h), the same job Get Worlds and the web import run: the pre-flight, the
// running free-space floor and the 500 MB temp cap live there, once.
struct EmodConvJob{
    emod::ImportJob job;
    std::string outPath,srcPath;
    NSString* outName;
    EmodConvJob():outName(nil){}
    ~EmodConvJob(){[outName release];}
};
static EmodConvJob* convJob=NULL;
static std::string convError;

BOOL FileManager::conversionActive(){return convJob!=NULL;}

// ---- S.5e: the height a conversion writes ---------------------------------------------------------
// Every `.emod` is 256z by default (the user, 2026-10-10; plan §10 Q9): the Settings toggle decides,
// and the prompt below lets the player keep one world 64z. One prompt at a time, for one source.
extern "C" int eden_get_upgrade_256z(void);   // web/src/seam/Settings_web.mm
static NSString* heightAskName=nil;   // the source the prompt is (or was) about
static int heightChoice=-3;           // -3 not asked, -2 waiting, -1 cancelled, 0 keep, 1 upgrade
static void clearHeightChoice(){[heightAskName release];heightAskName=nil;heightChoice=-3;}

BOOL FileManager::upgradeOnConvert(){return eden_get_upgrade_256z()!=0;}

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

int FileManager::convertHeightGate(NSString* name){
    if(convJob)return 1;
    if(heightAskName&&![heightAskName isEqualToString:name])clearHeightChoice();
    if(heightChoice>=0)return 1;
    if(heightChoice==-1){clearHeightChoice();return -1;}
    if(heightChoice==-2)return 0;
    if(!upgradeOnConvert())return 1;                     // the toggle is off: every world keeps its height
    const int bands=sourceBands(docs+"/"+cpstring(name));
    if(bands==16)return 1;                               // nothing to choose
    const char* e=getenv("EDEN_CONVERT_PROMPT");
    if(e&&e[0]=='0')return 1;                            // a harness: the toggle decides
    heightAskName=[name retain];
    heightChoice=-2;
    showAlertConvertHeight(name,bands==4);
    return 0;
}

void FileManager::answerConvertHeight(int choice){
    if(heightChoice==-2)heightChoice=choice<0?-1:(choice?1:0);
}
NSString* FileManager::conversionError(){return convError.empty()?nil:nsstring(convError);}

void FileManager::cleanConversionTemps(){
    if(convJob)return;
    NSFileManager* nfm=[NSFileManager defaultManager];
    NSArray* names=[nfm contentsOfDirectoryAtPath:documents error:NULL];
    for(int i=0;i<(int)[names count];i++){
        NSString* f=[names objectAtIndex:i];
        // S.5: also a killed import's archive layer and a killed export/upload's partial output.
        if([f hasSuffix:@".emod.converting"]||[f hasSuffix:@".emod.spill"]||[f hasSuffix:@".emod.creating"]||[f hasSuffix:@".emod.compact"]
           ||[f rangeOfString:@".emod.layer"].location!=NSNotFound||[f hasSuffix:@".exporting"]||[f hasSuffix:@".upload-body"]){
            [nfm removeItemAtPath:[NSString stringWithFormat:@"%@/%@",documents,f] error:NULL];
        }
    }
}

int FileManager::convertStep(NSString* name,int* percent,NSString** outName){
    if(percent)*percent=0;

    if(!convJob){
        convError.clear();
        if(!worldNeedsConversion(name)){convError="nothing to convert";return -1;}
        EmodConvJob* j=new EmodConvJob();
        j->srcPath=docs+"/"+cpstring(name);
        // A fresh stem unless this one is taken by a `.emod` of some OTHER source (a different
        // `.eden` re-imported under the same name lists separately and converts beside the first).
        std::string stem=stemOf(name);
        for(int k=1;k<=99;k++){
            j->outPath=docs+"/"+stem+(k==1?std::string():"-"+std::to_string(k))+".emod";
            if(fileSizeOf(j->outPath)==0)break;
        }
        j->outName=[[NSString stringWithUTF8String:j->outPath.substr(docs.size()+1).c_str()] retain];
        // S.5e: the prompt's answer for this world, else the Settings toggle. Consumed here.
        BOOL up=(heightAskName&&[heightAskName isEqualToString:name]&&heightChoice>=0)?(heightChoice==1):upgradeOnConvert();
        clearHeightChoice();
        if(!j->job.begin(j->srcPath,j->outPath,cpstring(name),(int64_t)fileMtimeOf(j->srcPath),up)){
            convError=j->job.error();
            printg("emod: cannot convert %s: %s\n",j->srcPath.c_str(),convError.c_str());
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
        delete j;convJob=NULL;
        return -1;
    }
    if(r==emod::ImportJob::RUNNING){
        if(percent)*percent=j->job.percent();
        return 0;
    }
    // Committed and renamed by the converter; the source was only ever read.
    NSFileManager* nfm=[NSFileManager defaultManager];
    NSString* oldPng=[NSString stringWithFormat:@"%@/%@.png",documents,name];
    if([nfm fileExistsAtPath:oldPng]){
        [nfm moveItemAtPath:oldPng toPath:[NSString stringWithFormat:@"%@/%@.png",documents,j->outName] error:NULL];
    }
    if(outName)*outName=[[j->outName retain] autorelease];
    if(percent)*percent=100;
    [pendingOriginalName release];
    pendingOriginalName=[name retain];
    printg("emod: converted %s (%llu B, %d bands) -> %s (%llu B, %d bands)\n",j->srcPath.c_str(),fileSizeOf(j->srcPath),
           j->job.stats().bandsIn,j->outPath.c_str(),fileSizeOf(j->outPath),j->job.stats().bandsOut);
    delete j;convJob=NULL;
    return 1;
}

NSString* FileManager::pendingOriginal(){return pendingOriginalName;}

void FileManager::resolveOriginal(BOOL deleteIt){
    if(!pendingOriginalName)return;
    if(deleteIt){
        NSString* p=[NSString stringWithFormat:@"%@/%@",documents,pendingOriginalName];
        if([[NSFileManager defaultManager] removeItemAtPath:p error:NULL]){
            printg("emod: deleted the original %s\n",[p UTF8String]);
        }else{
            printg("emod: could not delete the original %s\n",[p UTF8String]);
        }
    }
    [pendingOriginalName release];
    pendingOriginalName=nil;
}
