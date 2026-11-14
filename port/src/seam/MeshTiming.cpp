// MeshTiming.cpp — pure measurement, no behaviour change. Wraps TerrainChunk::rebuild2()
// (the CPU meshing pass) and TerrainChunk::prepareVBO() (the GL upload pass) to answer the
// question WORKING/c1-threaded-build-handoff.md's §5 item 1 asks before any off-thread-meshing
// design work: "nothing yet establishes what fraction of a frame meshing costs, or whether the
// bottleneck is meshing (CPU) or VBO upload (main-thread GL, which cannot move)."
//
// Both are exported cross-TU symbols — Classes/Terrain.mm calls chunk->rebuild2()/prepareVBO()
// on a TerrainChunk* whose methods are defined in Classes/TerrainChunk.mm, a different TU — so
// this needed no Classes/ edit. Confirmed via
// `emnm build-st/CMakeFiles/eden.dir/.../Classes/TerrainChunk.mm.o`, not guessed:
//   T _ZN12TerrainChunk8rebuild2Ev
//   T _ZN12TerrainChunk10prepareVBOEv
//
// Deliberately NOT gated behind EDEN_DIAGNOSTICS: the whole point is to measure build-rel, the
// build players actually run (same reasoning as tools/headless-load-timing.js and
// tools/headless-memory-probe.js, both of which avoid every diagnostics probe for the same
// reason). Cost is two eden_platform_now_ms() calls and a handful of double adds per chunk
// rebuild/upload — chunk rebuilds are at most a few hundred per frame even under heavy churn
// (Terrain.mm's list[] is sized CHUNKS_PER_SIDE^2*CHUNKS_PER_COLUMN_MAX), so this is noise next
// to the work being measured.
#include "../shim/foundation/platform_shims.h"  // eden_platform_now_ms
#include <cstdint>
#include <cstdio>

// The mesh accumulator is integer nanoseconds with __atomic_fetch_add and the max a compare-exchange
// loop: it was written for a worker-thread mesher (MeshPool, deleted in SM.8; see git history).
// Upload/read/io stay plain doubles: prepareVBO() and readColumn() are main-thread-only.
namespace {

uint64_t g_meshNs = 0;
uint64_t g_meshNsMax = 0;
unsigned g_meshCount = 0;

void note_mesh(double ms) {
    uint64_t ns = (uint64_t)(ms * 1e6);
    __atomic_fetch_add(&g_meshNs, ns, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_meshCount, 1u, __ATOMIC_RELAXED);
    uint64_t cur = __atomic_load_n(&g_meshNsMax, __ATOMIC_RELAXED);
    while (ns > cur &&
           !__atomic_compare_exchange_n(&g_meshNsMax, &cur, ns, true,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
}

double   g_uploadMs = 0.0;
double   g_uploadMsMax = 0.0;
unsigned g_uploadCount = 0;

// Added after the first burst measurement: mesh+upload alone didn't add up to the whole observed
// block (see tools/headless-mesh-burst-probe.js), so the column-read/decode step (the OTHER thing
// Terrain::prepareAndLoadGeometry's bulk-reload path does synchronously before meshing) needed its
// own number to close the gap.
double   g_readMs = 0.0;
double   g_readMsMax = 0.0;
unsigned g_readCount = 0;

// B1 (ROADMAP Phase B): split the g_readMs figure above one layer further. FileManager::readColumn
// is `NSFileHandle file I/O` + `RLE decode + band transpose` (see fmh_readColumnFromDefault). This
// counter is the file-I/O half only — every fread() the shim's NSFileHandle -readDataOfLength:
// serves during a read window, fed in via eden_mt_note_io() from src/shim/foundation/NSFileHandle.mm.
// g_readMs - g_ioMs is then the pure RLE-decode/transpose CPU cost. The JS side splits g_ioMs again:
// Module.EdenWorldFS.stats.fetchMs is the transport (sync XHR / fs.readSync) subset of it, leaving
// g_ioMs - fetchMs as the lazy node's block-cache/coalesce overhead.
double   g_ioMs = 0.0;
double   g_ioMsMax = 0.0;
unsigned g_ioCount = 0;

} // namespace


extern "C" {



} // extern "C"
