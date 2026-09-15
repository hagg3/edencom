//
//  FileManagerHelper.h
//  Eden
//
//  Created by Ari Ronen on 5/20/14.
//
//
#ifndef Eden_FileManagerHelper_h
#define Eden_FileManagerHelper_h



#import "FileManager.h"


void fmh_init(FileManager* tfm);
void fmh_readColumnFromDefault(int cx,int cz);

// ---- B3 Stage 3: the same column read, split into its three layers ---------------------------
// fmh_readColumnFromDefault() above is now exactly these three called in a row, and stays the
// only entry point anything outside this file needs for the synchronous case. They are separate
// because B1 measured the middle one as ~75% of a bulk reload's column-read cost (RLE run-
// expansion plus the CC(x,z,y)<->CC(y,z,x) band transpose) and it is the only one of the three
// that can move to a worker thread:
//   - the READ is FileManager singleton state (one open handle, a stateful seek) and stays put;
//   - the DECODE is pure -- no globals, no file, no engine state;
//   - the PUBLISH writes chunk voxels, blockarray and the dirty lists, so it is main-thread by
//     the same rules everything else in Terrain is.
// See WORKING/b3-off-thread-meshing-plan.md §5.

// Worst case raw bytes for one band's record: a 2-byte length prefix plus one 3-byte run per voxel.
#define FMH_BAND_RAW_MAX (2+CHUNK_SIZE3*3)

// How many RLE bands the BUNDLED map stores per column, clamped to this world's height. Callers
// size their buffers from this.
int fmh_defaultBandCount();

// MAIN THREAD. Seek to (cx,cz)'s record and read its `bands` raw band payloads into `raw`
// (band i at raw + i*FMH_BAND_RAW_MAX, its length in lens[i]). FALSE = the bundled map has no
// such column, and the caller should fall back to fmh_readColumnFromDefault's own handling.
BOOL fmh_readColumnRawFromDefault(int cx,int cz,unsigned char* raw,int* lens);

// PURE -- safe to call on any thread. Expands and transposes `bands` bands into outBlocks/
// outColors (band i at out* + i*CHUNK_SIZE3). status[i] is 1 if the band decoded to exactly
// CHUNK_SIZE3 voxels and 0 if it did not, in which case the publish step must leave that chunk's
// existing voxels alone -- which is what the original did, by simply not writing them.
void fmh_decodeColumnBands(const unsigned char* raw,const int* lens,int bands,
                           block8* outBlocks,color8* outColors,int* status);

// MAIN THREAD. Land a decoded column: re-home each chunk (setBounds), copy the decoded voxels in,
// mirror them into blockarray, and addChunk() so the dirty lists pick them up. Bands the bundled
// map does not have become air, exactly as before.
void fmh_publishColumnFromDefault(int cx,int cz,const block8* blocks,const color8* colors,
                                  int bands,const int* status);


#endif
