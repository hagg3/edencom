//
//  TerrainChunk.h
//  prototype
//
//  Created by Ari Ronen on 10/18/10.
//  Copyright 2010 __MyCompanyName__. All rights reserved.
//
#ifndef Eden_TerrainChunk_h
#define Eden_TerrainChunk_h


#import <OpenGLES/ES1/gl.h>
#import <OpenGLES/ES1/glext.h>

#import "Terrain.h"
#import "glu.h"
#import "Resources.h"
#import "Util.h"
#import "Globals.h"

class Terrain;
typedef struct _static_object{
    Vector pos;
    int type;
    int ani;
    int dir;
    BOOL open;
    color8 color;
    float rot;
}StaticObject;

typedef struct _small_block{
    block8 blocks[8];
    color8 colors[8];
    
}SmallBlock;
#define CC(x,z,y) ((int)(x)*(CHUNK_SIZE*CHUNK_SIZE)+(int)(z)*(CHUNK_SIZE)+(int)(y))

// ---- B3 / off-thread meshing: making rebuild2() re-entrant ------------------------------------
// The mesher is being moved onto a worker thread (WORKING/b3-off-thread-meshing-plan.md). Nothing
// here changes what a mesh contains; it removes the three reasons two threads could not both be
// inside rebuild2() at once, and every one of them is inert unless a caller opts in:
//
//   1. rebuild2()'s scratch (v_idx, face_visibility, face_size, hasBlocky, hasVisy) was file-scope
//      static, i.e. shared by every call. EDEN_MESH_TLS gives each thread its own copy, and is
//      EMPTY unless the build actually has threads -- so the single-threaded build (and the iOS
//      target, which never defines EDEN_THREADED) is byte-for-byte what it was.
//   2. rebuild2() reads the chunk's OWN pblocks/pcolors in both its counting pass and its fill
//      pass, and the main thread writes them from Terrain::setLand and FileManager::readColumn.
//      A write landing between the two passes breaks the counting/fill agreement the mesher
//      depends on -- a heap overrun, not a cosmetic glitch (see CLAUDE.md's warning). A worker
//      therefore meshes from a private snapshot; tc_meshSetSource() points rebuild2 at it.
//      (Its OTHER global reads -- blockarray/lightarray neighbours, getColorc -- feed
//      face_visibility, which is private scratch computed once BEFORE both passes, so staleness
//      there is cosmetic and does not break the invariant. burnList is the exception, see 3.)
//   3. Two things inside rebuild2() MUTATE global state: Portal::addPortal for a TYPE_PORTAL_TOP,
//      and a setLand() that repairs an out-of-range block type. Plus isOnFire(), which walks the
//      main thread's mutable burnList (a pointer chase = use-after-free, not just a stale read).
//      A worker passes a precomputed burn mask and a MeshSideEffect sink instead; the main thread
//      replays the sink when it publishes the mesh.
#ifdef EDEN_THREADED
#define EDEN_MESH_TLS thread_local
#else
#define EDEN_MESH_TLS
#endif

enum {
    MESH_SE_PORTAL=0,   // Portal::addPortal(x,y,z,dir,color)
    MESH_SE_REPAIR=1,   // Terrain::setLand(x,z,y,type,TRUE) -- out-of-range block type repair
};
typedef struct _mesh_side_effect{
    int kind;
    int x,y,z;      // world coords, as the deferred call wants them
    int a,b;        // portal: dir,color.  repair: type,unused.
}MeshSideEffect;

// Off-thread meshing hooks. Call before rebuild2() on a worker, tc_meshClearSource() after.
// blocks/colors are a private CHUNK_SIZE^3 snapshot of the chunk's pblocks/pcolors; burnMask is
// CHUNK_SIZE^3 bytes (nonzero = that block is in burnList) or NULL for "nothing is burning";
// sink/sink_max/sink_count receive the deferred side effects. Passing NULL for everything (the
// default state) is exactly the stock single-threaded behaviour.
void tc_meshSetSource(const block8* blocks,const color8* colors,const unsigned char* burnMask,
                      MeshSideEffect* sink,int sink_max,int* sink_count);
void tc_meshClearSource();
// Main thread: replay what a worker mesh deferred. Safe to call with count==0.
void tc_meshReplaySideEffects(const MeshSideEffect* sink,int count);

// ---- B3 Stage 2: per-chunk mesh-job state ------------------------------------------------------
// Lives on the chunk rather than in MeshPool so every writer of the chunk's voxels can invalidate
// an in-flight mesh with one branch, without knowing the pool exists (Classes/MeshPool.{h,mm} owns
// the pool itself). Only the MAIN thread writes IDLE and QUEUED; only a WORKER writes RUNNING and
// DONE. That asymmetry is what makes "busy == not IDLE" a stable question for the main thread to
// ask with a relaxed load: nothing but the main thread can ever put a chunk back to IDLE, so a
// chunk it believes busy stays busy until it says otherwise.
enum {
    MESH_JOB_IDLE=0,    // no worker has this chunk; the main thread may mesh, recycle or dispatch it
    MESH_JOB_QUEUED,    // snapshot taken, waiting for a worker
    MESH_JOB_RUNNING,   // a worker is inside rebuild2() for this chunk
    MESH_JOB_DONE,      // worker finished; the non-rt fields are filled, awaiting prepareVBO()
};

class TerrainChunk {
	
public:
	
	//block8 blocks[CHUNK_SIZE*CHUNK_SIZE*CHUNK_SIZE];
    
   
    block8 pblocks[CHUNK_SIZE*CHUNK_SIZE*CHUNK_SIZE];
    //block8 blocks2[CHUNK_SIZE*CHUNK_SIZE*CHUNK_SIZE];
    color8 pcolors[CHUNK_SIZE*CHUNK_SIZE*CHUNK_SIZE];
    //float lightsf[CHUNK_SIZE*CHUNK_SIZE*CHUNK_SIZE];
    
   
  //  block8* pblocks2;
   
   // SmallBlock* sblocks[CHUNK_SIZE*CHUNK_SIZE*CHUNK_SIZE];
   // SmallBlock** psblocks;
   // float test[100];
    
    vertexStructSmall* verticesbg;
    vertexStructSmall* verticesbg2;
    // float test2[100];
    unsigned short* indices;
    unsigned short* rtindices;
    	int rcx,rcz;
   
    StaticObject* objects;
     StaticObject* rtobjects;
     int num_objects;
	int n_vertices,n_vertices2;
    int num_vertices[7];
    int face_idx[7];
    int num_vertices2[7];
    int face_idx2[7];
    bool visibleFaces[7];
    int vis_vertices;
    int rebuildCounter;
    
    int idxn;
    int rtnum_objects;
	int rtn_vertices,rtn_vertices2;
    int rtnum_vertices[7];
    int rtface_idx[7];
    int rtnum_vertices2[7];
    int rtface_idx2[7];
    bool rtvisibleFaces[7];
    int rtvis_vertices;
    TreeNode* m_treenode;
	ListNode* m_listnode;
    
    BOOL needsVBO;
    // Phase N Stage 1 (WORKING/native-migration-plan-2026-09-04.md): a content fingerprint of the
    // vertex data this chunk last PUBLISHED, folded in prepareVBO() just before the CPU-side
    // arrays are freed. It has to live here because that is the only moment the data exists: the
    // mesher builds verticesbg/verticesbg2, prepareVBO() uploads them to the VBO and free()s them,
    // and from then on the only copy is inside a GL buffer a headless run cannot read back.
    //
    // `rtGeomHash` covers position + texcoord (pure functions of block type and face visibility,
    // so identical between any two runs over the same world at the same origin, on any platform);
    // `rtFullHash` adds the baked vertex colours, which carry incrementally-propagated lighting
    // and therefore only converge after the same number of settled frames. Read together by
    // eden_debug_mesh_checksum(); see that export in web/src/seam/DebugState_web.mm for why the
    // split is the useful part.
    //
    // OFF BY DEFAULT and off the hot path: computing them costs a pass over every vertex, so
    // prepareVBO() only does it when eden_debug_set_mesh_checksum(1) has been called.
    unsigned long long rtGeomHash;
    unsigned long long rtFullHash;
    BOOL clearOldVerticesOnly;
    // B3 Stage 2. meshJobState is one of the MESH_JOB_* values above and is touched from two
    // threads (atomically, via MeshPool.mm). meshJobStale is main-thread-only: it records that the
    // chunk's voxels changed AFTER the worker took its snapshot, so the mesh about to be published
    // is a frame out of date. The rule is publish it anyway and re-dirty the chunk -- discarding it
    // would leave the chunk with no geometry at all until something else dirtied it, which reads as
    // a hole in the world.
    int meshJobState;
    BOOL meshJobStale;
    
    BOOL in_view;
    BOOL has_light;
    BOOL modified;
    int isTesting;
    GLuint query
    ;
	GLuint    vertexBuffer,vertexBuffer2,elementBuffer;
    // M3(a) (ROADMAP Phase M): byte capacity each GL buffer name is currently sized for, so
    // prepareVBO() can re-upload into a persistent name instead of delete/recreating it every
    // re-mesh. Never shrinks; reset to 0 wherever the name is actually deleted.
    int       vbCapacity,vb2Capacity;
   // BOOL needsRebuild;

	int pbounds[6];
    
	float rbounds[6];	
    
   
    
   
    TerrainChunk(const int* boundz,Terrain* terrain);
    ~TerrainChunk();
    int getLand(int x,int z,int y);
    void setLand(int x,int z,int y,int type);
    void resetForReuse();
    int rebuild2();
    void setBounds(int* boundz);
    void clearMeshes();
    int render();
    void render2();
    void unbuild();
    void prepareVBO();
};

typedef struct bnode{
	int x,y,z;
	float time;	
	float life;
	int pid;
	int sid;
	int type;
	struct bnode* next;
}BurnNode;


// A chunk's voxels were just written by the main thread. If a worker is meshing it right now, the
// mesh it is building no longer matches the world -- flag it so the publish step re-dirties the
// chunk. One predictable branch on a field that is permanently IDLE in a single-threaded build.
static inline void tc_noteChunkWritten(TerrainChunk* chunk){
    if(chunk&&chunk->meshJobState!=MESH_JOB_IDLE)chunk->meshJobStale=TRUE;
}

void tc_initGeometry();

#endif

