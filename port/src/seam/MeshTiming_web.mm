// MeshTiming_web.mm — pure measurement, no behaviour change. Wraps TerrainChunk::rebuild2()
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

// B3 Stage 2 made this file's rebuild2() wrapper multi-threaded. --wrap is a link-time symbol
// rename, so Classes/MeshPool.mm's `chunk->rebuild2()` on a WORKER lands in __wrap_ below just
// like the main thread's does — which is what keeps the mesh-CPU number honest once meshing moves
// off-thread, and which means these three counters are now written from up to three threads at
// once. Doubles cannot be atomically added, so the mesh accumulator is integer nanoseconds with
// __atomic_fetch_add and the max is a compare-exchange loop. Upload/read/io stay plain doubles:
// prepareVBO() and readColumn() are main-thread-only by construction (GL, and the non-reentrant
// FileManager singleton respectively) and that is not going to change.
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

// Classes/MeshPool.mm (B3 Stage 2). Declared rather than #included so this seam file keeps its
// no-engine-headers property — these are plain C entry points with no TerrainChunk in the signature.
extern "C" void mp_getStats(double* snapshotMs, unsigned* dispatched, unsigned* inlined,
                            unsigned* published, unsigned* stale);
extern "C" void mp_getDecodeStats(double* readRawMs, double* decodeMs, unsigned* decoded);
extern "C" void mp_resetStats();

// C linkage, called from the NSFileHandle shim (a different TU) around its fread(). A tiny
// accumulator function rather than an exported data symbol so there is no mangled-name coupling.
extern "C" void eden_mt_note_io(double ms) {
    g_ioMs += ms;
    g_ioCount++;
    if (ms > g_ioMsMax) g_ioMsMax = ms;
}

extern "C" {

// Zero every counter — call before a sampling window so eden_debug_mesh_timing() reports only
// that window rather than a cumulative-since-boot figure.
EDEN_EXPORT
void eden_debug_mesh_timing_reset(void) {
    g_uploadMs = g_uploadMsMax = g_readMs = g_readMsMax = 0.0;
    g_ioMs = g_ioMsMax = 0.0;
    g_uploadCount = g_readCount = g_ioCount = 0;
    __atomic_store_n(&g_meshNs, (uint64_t)0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_meshNsMax, (uint64_t)0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_meshCount, 0u, __ATOMIC_RELAXED);
    mp_resetStats();
}

// Static buffer, same shape as DebugState_web.mm's other JSON exports (that file is
// EDEN_DIAGNOSTICS-only; this export deliberately is not, so it stays reachable in build-rel).
EDEN_EXPORT
const char* eden_debug_mesh_timing(void) {
    static char buf[1024];
    // B3 Stage 2's own numbers. snapshotMs is the ONE piece of work the worker mesher adds to the
    // main thread — the 8 KB pblocks+pcolors memcpy per dispatched chunk, 10.6 MB per 64z burst —
    // and it was unmeasured when Stage 2 was specified. dispatched/inlined say how often the pool
    // actually took the work versus falling back to today's inline path (no threads, no free slot,
    // or fire in the chunk); stale says how often an edit landed under a running job.
    double   snapshotMs = 0.0;
    unsigned dispatched = 0, inlined = 0, published = 0, stale = 0;
    mp_getStats(&snapshotMs, &dispatched, &inlined, &published, &stale);
    // B3 Stage 3. When a column's decode is deferred it never enters FileManager::readColumn, so
    // readMs above stops seeing it — these three are where that time went. readRawMs is what is
    // LEFT on the main thread (the seek + fread of the raw RLE bytes); decodeMs is the
    // run-expansion and band transpose, now on a worker. readMs + readRawMs is the honest
    // main-thread column-read total to compare against a non-threaded build's readMs.
    double   readRawMs = 0.0, decodeMs = 0.0;
    unsigned decoded = 0;
    mp_getDecodeStats(&readRawMs, &decodeMs, &decoded);
    snprintf(buf, sizeof(buf),
        "{\"meshMs\":%.3f,\"meshCount\":%u,\"meshMsMax\":%.3f,"
        "\"uploadMs\":%.3f,\"uploadCount\":%u,\"uploadMsMax\":%.3f,"
        "\"readMs\":%.3f,\"readCount\":%u,\"readMsMax\":%.3f,"
        "\"ioMs\":%.3f,\"ioCount\":%u,\"ioMsMax\":%.3f,"
        "\"snapshotMs\":%.3f,\"dispatched\":%u,\"inlined\":%u,"
        "\"published\":%u,\"stale\":%u,"
        "\"readRawMs\":%.3f,\"decodeMs\":%.3f,\"decodedColumns\":%u}",
        (double)__atomic_load_n(&g_meshNs, __ATOMIC_RELAXED) / 1e6,
        __atomic_load_n(&g_meshCount, __ATOMIC_RELAXED),
        (double)__atomic_load_n(&g_meshNsMax, __ATOMIC_RELAXED) / 1e6,
        g_uploadMs, g_uploadCount, g_uploadMsMax,
        g_readMs, g_readCount, g_readMsMax,
        g_ioMs, g_ioCount, g_ioMsMax,
        snapshotMs, dispatched, inlined, published, stale,
        readRawMs, decodeMs, decoded);
    return buf;
}

} // extern "C"

// ---------------------------------------------------------------------------------------------
// --wrap=_ZN12TerrainChunk8rebuild2Ev — the meshing pass: counts+fills the per-face-direction
// vertex buckets from block data. CPU-only, no GL calls (root CLAUDE.md: its counting pass and
// fill pass must agree exactly, which is exactly the invariant an off-thread mesher would need
// to keep across two threads instead of one call).
// ---------------------------------------------------------------------------------------------
struct TerrainChunk;
