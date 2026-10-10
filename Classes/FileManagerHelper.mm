//
//  FileManagerHelper.m
//  Eden
//
//  Created by Ari Ronen on 5/20/14.
//
//

#import "FileManagerHelper.h"
#import "FileManager.h"
#import "FileArchive.h"
#import "World.h"
#import "hashmap.h"
#import "zpipe.h"
#include "EdenWorldStore.h"   // S.6: the bundled map is Eden.emod
#include <vector>


FileManager* fm;

//This helper opens up the default world gen and keeps it open, then loads data from it when default world gen chunks are needed.
//All file handles refer to the default world gen, NOT the currently active world, even though the names are the same as FileManager
static NSFileHandle* saveFile;
static WorldFileHeader* sfh;
static map_t indexes;
// S.6: the bundled map is `Eden.emod` (docs/emod-file-format.md; baked by `emod.py bake-default`).
// When it is present this store answers every column read and the RLE handle/directory above stay
// empty; `Eden.eden` (the RLE source, "never regenerate" data) is only the fallback for a bundle
// that has no Eden.emod, and the source `emod.py bake-default` reads.
static emod::EdenWorldStore* s_store=NULL;

static void fmh_read_directory();
void fmh_init(FileManager* t_fm){
    if(JUST_TERRAIN_GEN)return;
    
    fm=t_fm;
     //Terrain* ter=[World::getWorld->terrain];
    
     //Player* player=[World::getWorld->player];
    printg("fmh init...\n");
   
    indexes=hashmap_new();
     
 //  NSString* file_name=[NSString stringWithFormat:@"%@/Eden.eden",fm.documents];

    // S.6: Eden.eden.gz and its 52.5 MiB inflated Documents/Eden.eden.cache are gone (Stage 4.4 added
    // them because the bundle is charged against install size; Eden.emod is 11.8 MB and is read in
    // place from the bundle, so there is nothing to inflate). Delete the cache an earlier build
    // left behind -- once, here, and silently when there is none.
    NSFileManager* nsfm=[NSFileManager defaultManager];
    {
        NSString* stale=[NSString stringWithFormat:@"%@/Eden.eden.cache",fm->documents];
        if([nsfm fileExistsAtPath:stale]){
            if([nsfm removeItemAtPath:stale error:NULL])printg("fmh init: removed the stale Eden.eden.cache\n");
        }
    }
    // NSBundle's web shim never returns nil (a miss synthesizes a non-existent path so untouched
    // callers can keep using -fileExistsAtPath:) so the presence check has to be
    // -fileExistsAtPath:, not a nil check against the path string.
    NSString* emod_name=[[NSBundle mainBundle] pathForResource:@"Eden.emod" ofType:nil];
    if(emod_name!=nil&&[nsfm fileExistsAtPath:emod_name]){
        emod::EdenWorldStore* st=new emod::EdenWorldStore();
        if(st->open([emod_name UTF8String],false)){
            s_store=st;
            printg("fmh init: bundled map Eden.emod, %d columns, %d bands, open %.1f ms\n",
                   (int)st->columnCount(),st->bands(),st->openMs());
            return;
        }
        printg("fmh init: Eden.emod refused (%s); trying Eden.eden\n",st->error().c_str());
        delete st;
    }
    NSString* file_name=[[NSBundle mainBundle] pathForResource:@"Eden.eden" ofType:nil];

     saveFile=[NSFileHandle fileHandleForReadingAtPath:file_name];
    [saveFile retain];
     sfh=(WorldFileHeader*)[[saveFile readDataOfLength:sizeof(WorldFileHeader)] bytes];
    //[sfh retain];
    
   
    
    fmh_read_directory();
    
    
    
}
static void fmh_read_directory(){
	
	[saveFile seekToFileOffset:sfh->directory_offset];
	while(TRUE){
		NSData* data=[saveFile readDataOfLength:sizeof(ColumnIndex)];
		if(data==NULL||[data length]<sizeof(ColumnIndex))break;
		
		ColumnIndex* colIdx=(ColumnIndex*)malloc(sizeof(ColumnIndex));
		[data getBytes:colIdx length:sizeof(ColumnIndex)];
		int n=twoToOne(colIdx->x, colIdx->z);
		if(n!=0){
            hashmap_put(indexes,n, (any_t)colIdx);
            // printg("reading dir\n");
        }else {
			free(colIdx);
		}
        
        
		
	}
}
// ---- B3 Stage 3: the column read, split into read / decode / publish -------------------------
// Same work, same order, same bytes as the single function this replaced -- see
// FileManagerHelper.h for why it is in three pieces and which piece can leave the main thread.
// There is deliberately only ONE copy of the decode, shared by the synchronous path below and by
// the worker path (Classes/MeshPool.mm), because two copies of an RLE reader is exactly the kind
// of thing that drifts.

int fmh_defaultBandCount(){
    // How many RLE bands the BUNDLED map actually stores per column -- derived from its own header,
    // not from the world being played. Eden.eden is a 64z (4-band) file and is deliberately NOT
    // regenerated for 256z: the offline TerrainGen2 bake would need ~4 GB and would produce a
    // differently-shaped world needing an art pass (see the 256z plan, Stage 2 item 6). A 256z
    // world seeded from the default map therefore gets those 4 bands and air above them.
    if(s_store){
        int b=s_store->bands();
        if(b>CHUNKS_PER_COLUMN)b=CHUNKS_PER_COLUMN;
        return b;
    }
    if(sfh==NULL)return 0;
    int bands=(sfh->version>=FILE_VERSION_256Z)?CHUNKS_PER_COLUMN_MAX:4;
    if(bands>CHUNKS_PER_COLUMN)bands=CHUNKS_PER_COLUMN;
    return bands;
}

// B6 (ROADMAP Phase B): how many bytes to try to pull in the ONE read this function now does.
// 4 KB, because that is what the bundled Eden.eden actually needs: measured over 576 columns
// (tools/headless-column-read-bench.js reports both figures) a record averages ~1.2 KB, and the
// handful that run past 4 KB just pay one top-up read. Deliberately NOT adaptive -- a hint that
// grew to the worst record seen would make every one of the many small columns pull 9 KB to suit
// the rare big one, which measured worse than the top-ups it avoids. Note this is always a 4-band
// record whatever the world's height: Eden.eden is a 64z file (see fmh_defaultBandCount).
#define FMH_RECORD_HINT 4096

// Pull `want` more bytes of the current record onto the end of what we already have. Only runs
// when the hint was too small, which on the bundled map is never after the first column.
static NSData* fmh_topUpRecord(NSData* have,int want){
    NSData* extra=[saveFile readDataOfLength:want];
    if(extra==NULL||[extra length]==0)return have;
    NSMutableData* grown=[NSMutableData dataWithCapacity:[have length]+[extra length]];
    [grown appendData:have];
    [grown appendData:extra];
    return grown;
}

BOOL fmh_readColumnRawFromDefault(int cx,int cz,unsigned char* raw,int* lens){
    ColumnIndex* colIndex=NULL;
    int n=twoToOne(cx,cz);
    if(n==0)return FALSE;
    if(s_store){
        // S.6: the zstd decode happens HERE (main thread: the store is single-owner, like the file
        // handle below); what reaches fmh_decodeColumnBands is already planar CC(x,z,y), flagged
        // with FMH_LEN_PLANAR, so the worker's share is a copy instead of an RLE expansion + transpose.
        if(!s_store->hasColumn(cx,cz))return FALSE;
        const int bands=fmh_defaultBandCount();
        std::vector<unsigned char> col((size_t)s_store->bands()*emod::BAND);
        emod::EdenWorldStore::ReadResult rr=s_store->readColumn(cx,cz,col.data());
        if(rr==emod::EdenWorldStore::READ_ABSENT)return FALSE;
        if(rr!=emod::EdenWorldStore::READ_OK)printg("fmh: bundled column %d,%d damaged (%d)\n",cx,cz,(int)rr);
        for(int cy=0;cy<bands;cy++){
            memcpy(raw+(size_t)cy*FMH_BAND_RAW_MAX,col.data()+(size_t)cy*emod::BAND,emod::BAND);
            lens[cy]=emod::BAND|FMH_LEN_PLANAR;
        }
        return TRUE;
    }
    hashmap_get(indexes,n,(any_t*)&colIndex);
    if(colIndex==NULL)return FALSE;

    [saveFile seekToFileOffset:colIndex->chunk_offset];
    const int bands=fmh_defaultBandCount();
    // B6: this used to be EIGHT reads per column -- a 2-byte length prefix and a payload, per band
    // -- and profiling the isolated read path (tools/headless-column-read-bench.js under
    // --cpu-prof) put ~43% of its cost in the per-read trip through musl stdio into Emscripten's FS
    // layer and another ~30% in the NSData object each read allocates, against a record that
    // averages 1203 bytes in total. So: read the record ONCE and slice the bands out of memory.
    // The bands are stored back to back from the column's directory offset, so a single read of
    // the whole record is exactly the same bytes in the same order -- and reading PAST the record
    // is harmless, because the next column re-seeks from the directory rather than continuing.
    // (The over-read is bounded on purpose: in a browser this file is the lazy Eden.eden node,
    // which services reads out of 32 KB blocks fetched over synchronous XHR, so read-ahead is only
    // free while it stays near what the caller wanted. A fixed 16 KB read-ahead in the shim was
    // measured as a 60% REGRESSION on this path for exactly that reason.)
    const int recordMax=bands*FMH_BAND_RAW_MAX;
    int want=FMH_RECORD_HINT; if(want>recordMax)want=recordMax;
    NSData* rec=[saveFile readDataOfLength:want];
    const unsigned char* p=(const unsigned char*)[rec bytes];
    int avail=(int)[rec length];
    int off=0;
    for(int cy=0;cy<bands;cy++){
        unsigned char* dst=raw+(size_t)cy*FMH_BAND_RAW_MAX;
        if(off+2>avail){
            rec=fmh_topUpRecord(rec,recordMax-avail>0?recordMax-avail:2);
            p=(const unsigned char*)[rec bytes]; avail=(int)[rec length];
            if(off+2>avail){lens[cy]=0;continue;}
        }
        color8 buft[2]={p[off],p[off+1]};
        off+=2;
        int chunk_data_length= buft[0]*256+buft[1]-2;
        // Clamp, which the original did not: the 2-byte prefix can encode up to 65533 and the
        // destination is FMH_BAND_RAW_MAX (12290), so a malformed record smashed the stack buffer
        // it was read into. Unreachable with a well-formed Eden.eden, hence never seen -- but the
        // read is of file bytes, so it is worth not trusting them.
        if(chunk_data_length<0)chunk_data_length=0;
        if(chunk_data_length>FMH_BAND_RAW_MAX)chunk_data_length=FMH_BAND_RAW_MAX;
        if(off+chunk_data_length>avail){
            rec=fmh_topUpRecord(rec,off+chunk_data_length-avail);
            p=(const unsigned char*)[rec bytes]; avail=(int)[rec length];
        }
        int got=chunk_data_length;
        if(off+got>avail)got=avail-off;
        if(got<0)got=0;
        if(got<chunk_data_length){
            printg("not enough file left, only read %d bytes\n",got);
        }
        if(got)memcpy(dst,p+off,got);
        off+=got;
        lens[cy]=got;
    }
    return TRUE;
}

void fmh_decodeColumnBands(const unsigned char* raw,const int* lens,int bands,
                           block8* outBlocks,color8* outColors,int* status){
    for(int cy=0;cy<bands;cy++){
        const unsigned char* buf=raw+(size_t)cy*FMH_BAND_RAW_MAX;
        if(lens[cy]&FMH_LEN_PLANAR){
            // S.6: an Eden.emod band -- 4,096 type bytes then 4,096 paint bytes, already CC(x,z,y).
            memcpy(outBlocks+(size_t)cy*CHUNK_SIZE3,buf,CHUNK_SIZE3*sizeof(block8));
            memcpy(outColors+(size_t)cy*CHUNK_SIZE3,buf+CHUNK_SIZE3,CHUNK_SIZE3*sizeof(color8));
            status[cy]=1;
            continue;
        }
        const int n=lens[cy];
        block8 tblocks[CHUNK_SIZE3];
        color8 tcolors[CHUNK_SIZE3];

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
        // The original signalled a short/long record with a bare putchar and then simply did not
        // write the chunk, leaving whatever was in it. Same outcome, reported to the caller
        // instead of printed from here, so this stays callable off the main thread.
        status[cy]=(idx2==CHUNK_SIZE3)?1:((idx2>CHUNK_SIZE3)?2:0);
        if(status[cy]!=1)continue;

        block8* ob=outBlocks+(size_t)cy*CHUNK_SIZE3;
        color8* oc=outColors+(size_t)cy*CHUNK_SIZE3;
        for(int z=0;z<CHUNK_SIZE;z++)
            for(int x=0;x<CHUNK_SIZE;x++)
                for(int y=0;y<CHUNK_SIZE;y++){
                    ob[CC(x,z,y)]=tblocks[CC(y,z,x)];
                    oc[CC(x,z,y)]=tcolors[CC(y,z,x)];
                }
    }
}

void fmh_publishColumnFromDefault(int cx,int cz,const block8* blocks,const color8* colors,
                                  int bands,const int* status){
    Terrain* ter=World::getWorld->terrain;
    extern int g_offcx;
    extern int g_offcz;
    extern block8* blockarray;

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
          //  chunk=new TerrainChunk(bounds,cx,cz,ter,TRUE);
            printf("crittcler error re-allocating terrain chunk\n");
            continue;
        }

        if(cy>=bands){
            // Above the bundled map's top band: air, and no read at all (there is nothing there
            // to read -- the next bytes belong to the next column).
            memset(chunk->pblocks,0,CHUNK_SIZE3*sizeof(block8));
            memset(chunk->pcolors,0,CHUNK_SIZE3*sizeof(color8));
        }else if(status[cy]==1){
            memcpy(chunk->pblocks,blocks+(size_t)cy*CHUNK_SIZE3,CHUNK_SIZE3*sizeof(block8));
            memcpy(chunk->pcolors,colors+(size_t)cy*CHUNK_SIZE3,CHUNK_SIZE3*sizeof(color8));
        }else if(status[cy]==2)putchar('>');
        else putchar('<');

       /* for(int i=0;i<CHUNK_SIZE3;i++){
            if(i%255==0){
                chunk.pblocks[i]=TYPE_BRICK;
                chunk.pcolors[i]=0;
            }else{
                chunk.pblocks[i]=0;
                chunk.pcolors[i]=0;
            }
        }*/

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

// Scratch for the synchronous path. One shared set of buffers because this is main-thread-only and
// re-entered once per column; the worker path carries its own per-job copies (Classes/MeshPool.mm).
static unsigned char fmh_raw[(size_t)CHUNKS_PER_COLUMN_MAX*FMH_BAND_RAW_MAX];
static block8 fmh_blocks[(size_t)CHUNKS_PER_COLUMN_MAX*CHUNK_SIZE3];
static color8 fmh_colors[(size_t)CHUNKS_PER_COLUMN_MAX*CHUNK_SIZE3];

void fmh_readColumnFromDefault(int cx,int cz){
    Terrain* ter=World::getWorld->terrain;
    ColumnIndex* colIndex=NULL;
	int n= twoToOne(cx,cz);
	if(n==0){
		NSLog(@"mm");
		return;
	}

	hashmap_get(indexes,n, (any_t*)&colIndex);

	if(colIndex==NULL&&!(s_store&&s_store->hasColumn(cx,cz))){
        ter->tgen->generateEmptyColumn(cx,cz);
        return;
    }

    int lens[CHUNKS_PER_COLUMN_MAX];
    int status[CHUNKS_PER_COLUMN_MAX];
    const int bands=fmh_defaultBandCount();
    for(int i=0;i<CHUNKS_PER_COLUMN_MAX;i++){lens[i]=0;status[i]=0;}
    if(!fmh_readColumnRawFromDefault(cx,cz,fmh_raw,lens))return;
    fmh_decodeColumnBands(fmh_raw,lens,bands,fmh_blocks,fmh_colors,status);
    fmh_publishColumnFromDefault(cx,cz,fmh_blocks,fmh_colors,bands,status);
}

// ---- S.6 gate -----------------------------------------------------------------------------------
// Both backends through the engine's own read + decode steps, column by column. The statics above are
// swapped (and restored) rather than duplicated, so what is compared is exactly the code the game runs.
int fmh_selftestCompare(const char* edenPath,const char* emodPath,int* compared){
    if(compared)*compared=0;
    emod::EdenWorldStore st;
    if(!st.open(emodPath,false)){printg("selftest: %s: %s\n",emodPath,st.error().c_str());return -1;}
    NSFileHandle* rleFile=[NSFileHandle fileHandleForReadingAtPath:[NSString stringWithUTF8String:edenPath]];
    if(rleFile==nil){printg("selftest: cannot open %s\n",edenPath);return -1;}

    NSFileHandle* o_file=saveFile; WorldFileHeader* o_sfh=sfh; map_t o_idx=indexes; emod::EdenWorldStore* o_store=s_store;
    saveFile=rleFile;
    sfh=(WorldFileHeader*)[[saveFile readDataOfLength:sizeof(WorldFileHeader)] bytes];
    indexes=hashmap_new();
    fmh_read_directory();
    NSData* hdrKeep=[NSData dataWithBytes:sfh length:sizeof(WorldFileHeader)];   // sfh points into a temporary NSData
    sfh=(WorldFileHeader*)[hdrKeep bytes];

    int rleCols=hashmap_length(indexes);
    int bad=0,n=0;
    if(rleCols!=(int)st.columnCount()){printg("selftest: %d RLE columns vs %d in the .emod\n",rleCols,(int)st.columnCount());bad++;}
    static unsigned char raw[(size_t)CHUNKS_PER_COLUMN_MAX*FMH_BAND_RAW_MAX];
    static block8 bA[(size_t)CHUNKS_PER_COLUMN_MAX*CHUNK_SIZE3],bB[(size_t)CHUNKS_PER_COLUMN_MAX*CHUNK_SIZE3];
    static color8 cA[(size_t)CHUNKS_PER_COLUMN_MAX*CHUNK_SIZE3],cB[(size_t)CHUNKS_PER_COLUMN_MAX*CHUNK_SIZE3];
    std::vector<std::pair<int32_t,int32_t> > keys=st.columnKeys();
    for(size_t i=0;i<keys.size();i++){
        const int cx=keys[i].first,cz=keys[i].second;
        int lensA[CHUNKS_PER_COLUMN_MAX],lensB[CHUNKS_PER_COLUMN_MAX],stA[CHUNKS_PER_COLUMN_MAX],stB[CHUNKS_PER_COLUMN_MAX];
        for(int k=0;k<CHUNKS_PER_COLUMN_MAX;k++){lensA[k]=lensB[k]=0;stA[k]=stB[k]=0;}
        s_store=NULL;
        const int bandsA=fmh_defaultBandCount();
        BOOL okA=fmh_readColumnRawFromDefault(cx,cz,raw,lensA);
        if(okA)fmh_decodeColumnBands(raw,lensA,bandsA,bA,cA,stA);
        s_store=&st;
        const int bandsB=fmh_defaultBandCount();
        BOOL okB=fmh_readColumnRawFromDefault(cx,cz,raw,lensB);
        if(okB)fmh_decodeColumnBands(raw,lensB,bandsB,bB,cB,stB);
        n++;
        bool same=okA&&okB&&bandsA==bandsB;
        for(int cy=0;same&&cy<bandsA;cy++){
            if(stA[cy]!=1||stB[cy]!=1||
               memcmp(bA+(size_t)cy*CHUNK_SIZE3,bB+(size_t)cy*CHUNK_SIZE3,CHUNK_SIZE3*sizeof(block8))||
               memcmp(cA+(size_t)cy*CHUNK_SIZE3,cB+(size_t)cy*CHUNK_SIZE3,CHUNK_SIZE3*sizeof(color8)))same=false;
        }
        if(!same){if(bad<10)printg("selftest: column %d,%d differs (ok %d/%d)\n",cx,cz,(int)okA,(int)okB);bad++;}
    }
    saveFile=o_file; sfh=o_sfh; indexes=o_idx; s_store=o_store;   // (the RLE directory leaks: a one-shot gate)
    if(compared)*compared=n;
    return bad;
}
