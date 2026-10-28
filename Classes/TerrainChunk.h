//
//  TerrainChunk.h
//  prototype
//
//  Created by Ari Ronen on 10/18/10.
//  Copyright 2010 __MyCompanyName__. All rights reserved.
//
#ifndef Eden_TerrainChunk_h
#define Eden_TerrainChunk_h


#include <stdint.h>

#include "Terrain.h"
#include "glu.h"
#include "Resources.h"
#include "Util.h"
#include "Globals.h"
#include "Renderer.h"   // G.5b-6: the chunk buffers are R::Buf

class Terrain;
typedef struct _static_object{
    Vector pos;
    int type;
    int ani;
    int dir;
    bool open;
    color8 color;
    float rot;
}StaticObject;

typedef struct _small_block{
    block8 blocks[8];
    color8 colors[8];
    
}SmallBlock;
#define CC(x,z,y) ((int)(x)*(CHUNK_SIZE*CHUNK_SIZE)+(int)(z)*(CHUNK_SIZE)+(int)(y))

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
    	int rcx,rcz;
   
    StaticObject* objects;   // the published door/portal/flower/golden-cube list, freed by clearMeshes()
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
    TreeNode* m_treenode;
	ListNode* m_listnode;
    
    bool needsVBO;
    // Phase N Stage 1 (WORKING/native-migration-plan-2026-09-04.md): a content fingerprint of the
    // vertex data this chunk last PUBLISHED, folded in prepareVBO() just before the CPU-side
    // arrays are freed. It has to live here because that is the only moment the data exists: the
    // mesher builds verticesbg/verticesbg2, prepareVBO() uploads them to the VBO and free()s them,
    // and from then on the only copy is inside a GL buffer a headless run cannot read back.
    //
    // `geomHash` covers position + texcoord (pure functions of block type and face visibility,
    // so identical between any two runs over the same world at the same origin, on any platform);
    // `fullHash` adds the baked vertex colours, which carry incrementally-propagated lighting
    // and therefore only converge after the same number of settled frames. Read together by
    // eden_debug_mesh_checksum(); see that export in port/src/seam/DebugState.cpp for why the
    // split is the useful part.
    //
    // OFF BY DEFAULT and off the hot path: computing them costs a pass over every vertex, so
    // prepareVBO() only does it when eden_debug_set_mesh_checksum(1) has been called.
    unsigned long long geomHash;
    unsigned long long fullHash;
    bool clearOldVerticesOnly;
    
    // Stage R / R.2: TRUE only if every pblocks byte is known to be 0 (air). Conservative: FALSE
    // means "not known", never "known non-empty". Types only -- paint on air does not mesh, and
    // rebuild2()'s own empty early-out has always looked at types alone. It lets an all-air chunk
    // skip rebuild2()'s scan and stay off the bulk-reload mesh budget. Who keeps it true:
    //   Terrain::addChunk  -- recomputed from the voxels, the moment a column lands (readColumn,
    //                         fmh_publishColumnFromDefault, every TerrainGenerator path);
    //   Terrain::setLand / TerrainChunk::setLand -- a non-zero write clears it at once;
    //   rebuild2()         -- a main-thread scan that finds nothing sets it (that is how a chunk
    //                         dug back to air becomes empty again; a set-to-0 never sets it).
    // Any NEW writer of pblocks owes one of those. eden_debug_chunk_state() audits it.
    bool typesEmpty;
    bool in_view;
    bool has_light;
    bool modified;
    int isTesting;
    uint32_t query
    ;
	R::Buf    vertexBuffer,vertexBuffer2,elementBuffer;   // G.5b-6: renderer buffers (0 = none)
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
    // B.5b-1a: forget the drawn mesh (CPU side and counts), keeping the GL buffer names for reuse
    // the way M3(a)'s prepareVBO does. For a chunk whose geometry belongs nowhere any more: a slot a
    // new column has just landed in, or one on the window's unmeshed outer ring. holdsGeometry()
    // is the "anything left to drop?" test, so a per-frame sweep can skip a dropped chunk cheaply.
    void dropGeometry();
    bool holdsGeometry() const;
    void prepareVBO();
};

typedef struct bnode{
	int x,y,z;
	float time;	
	float life;
	int pid;
	int sid;
	int type;
	int mpLit;   // MP.4: lit by this player in a session (its fuse running out sends the follow-up mine)
	struct bnode* next;
}BurnNode;


void tc_initGeometry();

// Stage R / R.2: is a chunk's type plane all air? Word-at-a-time over the 4 KB plane (~0.1 us),
// against the ~4 us (Mac) / ~11 us (A8X) rebuild2() scan it lets an empty chunk skip.
static inline bool tc_typesAllZero(const block8* blocks){
    const unsigned long long* w=(const unsigned long long*)blocks;
    unsigned long long acc=0;
    for(int i=0;i<(int)(CHUNK_SIZE*CHUNK_SIZE*CHUNK_SIZE*sizeof(block8)/8);i++)acc|=w[i];
    return acc==0;
}
// Recompute the bit from the voxels. Main thread only.
static inline void tc_refreshTypesEmpty(TerrainChunk* chunk){
    if(chunk)chunk->typesEmpty=tc_typesAllZero(chunk->pblocks);
}

#endif

