//
//  MeshPool.h
//  Eden — B3 Stage 2: the off-thread mesher's worker pool.
//
//  Design, hazard audit and staging: WORKING/b3-off-thread-meshing-plan.md (§2 is the audit that
//  says WHY only these things need protecting, §4 is this file's spec). Stage 1 —
//  Classes/TerrainChunk.{h,mm} — made rebuild2() re-entrant; this is what actually runs it on
//  another thread.
//
//  The handoff point, restated: a worker fills the chunk's NON-rt fields via rebuild2(); the main
//  thread calls prepareVBO() to publish them into the rt* fields and upload the VBOs. GL never
//  leaves the main thread (root CLAUDE.md convention #4), and it costs nothing to honour — the
//  upload is under 1% of a bulk-reload burst (measured every pass since pass 4).
//
//  Two kill switches, both deliberate and both to be kept:
//    1. EDEN_THREADED is not defined -> every function here is a no-op and mp_dispatch() always
//       answers FALSE, i.e. "mesh it inline yourself". That is exactly today's code path.
//    2. No free job slot -> mp_dispatch() answers FALSE the same way. Setting MP_JOB_SLOTS to 0
//       therefore disables the feature without removing a line of it.
//
#ifndef Eden_MeshPool_h
#define Eden_MeshPool_h

#include "TerrainChunk.h"

// Once per frame, before the frame's first mp_dispatch(). Snapshots what is burning so a worker
// never walks burnList (a linked list the main thread frees nodes from -- see the plan doc's §2).
void mp_beginFrame();

// Try to hand this chunk's mesh to a worker. TRUE = accepted, the caller must NOT touch the chunk
// again until the pool publishes it. FALSE = "not taken, mesh it inline right now", which is the
// unmodified single-threaded path and the reason this change is safe to leave enabled.
// idxn is the chunk's index in Terrain's chunkTable, remembered so the publish step can re-dirty it.
BOOL mp_dispatch(TerrainChunk* chunk,int idxn);

// B3 Stage 3. Try to hand column (cx,cz)'s RLE decode to a worker. The RAW READ happens inside
// this call, on the main thread, because it is FileManager singleton state (one open handle, a
// stateful seek); only the run-expansion and the band transpose -- B1's ~75% of the column-read
// cost -- go to the worker, and the publish (chunk voxels, blockarray, dirty lists) comes back to
// the main thread. TRUE = accepted: the caller must NOT mark the column loaded, and nothing may
// touch its chunks until mp_publishFinished() lands it. FALSE = "read it inline yourself", the
// stock synchronous path, which is again both the fallback and the kill switch.
BOOL mp_dispatchColumnDecode(int cx,int cz);

// Main thread: publish every job that has finished since the last call -- replay its deferred side
// effects, prepareVBO() it, hand the slot back. `redirty` is called for any chunk whose mesh went
// stale under it (an edit landed after the snapshot) or that rebuild2() refused, so the caller can
// set its dirty flags again. May be called as often as you like; publishing nothing is free.
void mp_publishFinished(void (*redirty)(int idxn));

// Does this chunk / any chunk of toroidal chunk-table column (cx,cz) have a job in flight? The
// invalidation rule the plan doc's §4.3 calls rule 1: a chunk a worker is reading must not be
// recycled -- no setBounds(), no resetForReuse(), no readColumn() that re-homes its slot.
BOOL mp_chunkBusy(const TerrainChunk* chunk);
BOOL mp_columnBusy(int cx,int cz);

// Wait until nothing is in flight. `publish` FALSE discards the finished meshes instead of
// uploading them, which is what teardown wants (the chunks are about to be deleted). Worker
// threads are NOT stopped -- they park on a condvar and are reused by the next world.
void mp_drain(void (*redirty)(int idxn),BOOL publish);

// Measurement, for tools/headless-mesh-burst-probe.js via src/seam/MeshTiming_web.mm. snapshotMs
// is the cost of the 8 KB pblocks+pcolors memcpy per dispatched chunk -- the one piece of work
// this change ADDS to the main thread, and unmeasured until now (10.6 MB per 64z burst).
extern "C" void mp_getStats(double* snapshotMs,unsigned* dispatched,unsigned* inlined,
                            unsigned* published,unsigned* stale);
// B3 Stage 3: readRawMs is the main-thread raw file read inside a decode dispatch -- what is LEFT
// of the column-read cost after the run-expansion and transpose move to a worker. `decoded` counts
// the columns that went off-thread.
extern "C" void mp_getDecodeStats(double* readRawMs,double* decodeMs,unsigned* decoded);
extern "C" void mp_resetStats();

#endif
