// DevConsole_web.mm — project-audit-2026-07-30 row F5 ("dev console: teleport, spawn, world
// stats"), requested from play rather than analysis (audit rows 31/33). Gated behind
// EDEN_DIAGNOSTICS (same CMakeLists.txt list as DebugState_web.mm) so it never ships in a build
// meant to be played — teleporting/spawning at will has no place outside a debugging session.
//
// Plain C exports, same "static buffer, single frame use" convention as DebugState_web.mm's
// probes — no _malloc/_free needed (Settings_web.mm's eden_settings_schema() established this
// pattern first). The console UI itself lives in public/eden-console.js, toggled by backtick;
// it feature-detects these exports (`typeof Module._eden_console_teleport === 'function'`)
// rather than checking a build flag from JS, so it silently doesn't appear on an
// EDEN_DIAGNOSTICS=OFF build without either side needing to know the other's config.
#import "../shim/foundation/uikit_stubs.h"
#include "../../../Classes/World.h"
#include "../../../Classes/Constants.h"
#include "../../../Classes/Model.h"
#include "../../../Classes/FileManagerHelper.h"   // B6 read-path benchmark, below
#include "../shim/foundation/platform_shims.h"   // EDEN_EXPORT (Phase N Stage 1)
#include <cstdio>

extern "C" {

// tp x y z — Vector convention (y UP, per CLAUDE.md #1 "Vector.y is up"), NOT Terrain's (x,z,y)
// argument order — this writes Player::pos directly, so it takes exactly what that field expects.
// No bounds/collision check: a console teleport is allowed to put the player somewhere the normal
// game never would (e.g. outside the resident toroidal window), same as it would in any game with
// a noclip-style teleport.
EDEN_EXPORT
int eden_console_teleport(float x, float y, float z) {
    if (!World::getWorld || !World::getWorld->player) return 0;
    Player* p = World::getWorld->player;
    p->pos.x = x;
    p->pos.y = y;
    p->pos.z = z;
    p->vel.x = p->vel.y = p->vel.z = 0;  // a stale velocity would immediately walk the player off
    return 1;
}

// spawn <type> — places a creature of the given TYPE_* / M_* model index at the player's current
// position via Model.mm's SpawnCreatureAt (Classes/ edit, same commit as this file — see
// web/docs/entities-and-creatures.md), which reuses the ambient spawner's own slot-scavenging and
// field setup rather than duplicating it here.
EDEN_EXPORT
int eden_console_spawn(int type) {
    if (!World::getWorld || !World::getWorld->player) return 0;
    return SpawnCreatureAt(type, World::getWorld->player->pos) ? 1 : 0;
}

// setblock x z y type — Terrain's own (x,z,y) argument order (CLAUDE.md #1), NOT Vector's.
// Calls Terrain::updateChunks, the same dirty-marking entry point Player::processInput's
// buildBlock eventually reaches (CLAUDE.md's "Trace a block edit"), skipping only buildBlock's
// own HUD-coupled side effects (golden-cube inventory, liquid sources, ramp/door orientation
// inference) that a console-driven edit has no HUD state to draw from. This is what makes a
// block edit possible from a script with no camera/raycast — added alongside the other three
// console commands to give tools/headless-save-roundtrip-test.js (audit row I6) something
// deterministic to edit before it saves, since a pristine unedited world has NO block data of
// its own to round-trip (unmodified terrain streams from the bundled Eden.eden by seed, per
// docs/eden-file-format.md — only touched columns are ever appended to a save file).
EDEN_EXPORT
int eden_console_setblock(int x, int z, int y, int type) {
    if (!World::getWorld || !World::getWorld->terrain) return 0;
    World::getWorld->terrain->updateChunks(x, z, y, type);
    return 1;
}

// stats — read-only snapshot for the console's "stats" command. Static buffer, JSON, same
// convention as DebugState_web.mm's probes and Settings_web.mm's schema export.
EDEN_EXPORT
const char* eden_console_world_stats(void) {
    static char buf[512];
    if (!World::getWorld || !World::getWorld->player || !World::getWorld->fm) {
        snprintf(buf, sizeof(buf), "{\"error\":\"no World yet\"}");
        return buf;
    }
    Player* p = World::getWorld->player;
    FileManager* fm = World::getWorld->fm;
    snprintf(buf, sizeof(buf),
        "{"
        "\"pos\":[%.2f,%.2f,%.2f],\"chunk_offset\":[%d,%d],"
        "\"active_creatures\":%d,\"game_mode\":%d"
        "}",
        p->pos.x, p->pos.y, p->pos.z, fm->chunkOffsetX, fm->chunkOffsetZ,
        CountActiveCreatures(), World::getWorld->game_mode);
    return buf;
}

// getblock x z y -- Terrain's own (x,z,y) order, the read counterpart of eden_console_setblock.
// Returns the block TYPE at that coordinate, or -1 for "outside the world" (which the engine also
// uses for out-of-range y, so a 256z world reporting -1 at y=200 is exactly the regression this
// exists to catch). Added for tools/headless-256z-test.js: reading a block back is the only way to
// prove a tall column was decoded into the right band from a script.
EDEN_EXPORT
int eden_console_getblock(int x, int z, int y) {
    if (!World::getWorld || !World::getWorld->terrain) return -1;
    return World::getWorld->terrain->getLand(x, z, y);
}

// ---- B6: an isolated, low-noise benchmark of the shim's file-read path ----------------------
// The chunk-reload burst probe (tools/headless-mesh-burst-probe.js) measures the read path inside
// a live teleport, where it shares a run with meshing, worker scheduling, lighting and the block
// cache's own warm-up. Its run-to-run spread on this number is +-20%, which is wider than the
// whole effect B6 is trying to move -- so it can say whether the burst got better, but it cannot
// say whether a change to NSFileHandle did anything.
//
// This runs the same fmh_readColumnRawFromDefault() the burst runs, over a fixed, spatially
// contiguous block of bundled-map columns, `iters` times, and returns the total wall time. Nothing
// else touches the file, no terrain state is written (this stops at the RAW read -- no decode, no
// publish), and after the first iteration the lazy Eden.eden node's block cache is warm, so what
// is left is exactly: seek + 8 x -readDataOfLength: per column, which is what B6 is about.
//
// `n` columns starting at (cx0,cz0) walking a square, in CHUNK-COLUMN coordinates (the same units
// FileManager's chunkOffsetX/Z are in). Returns milliseconds, or -1 if it could not allocate.
EDEN_EXPORT
double eden_debug_bench_column_read(int cx0, int cz0, int side, int iters) {
    const int bands = fmh_defaultBandCount();
    if (bands <= 0 || side <= 0 || iters <= 0) return -1.0;
    unsigned char* raw = (unsigned char*)malloc((size_t)bands * FMH_BAND_RAW_MAX);
    if (!raw) return -1.0;
    int lens[CHUNKS_PER_COLUMN_MAX];
    double t0 = eden_platform_now_ms();
    for (int it = 0; it < iters; it++)
        for (int dz = 0; dz < side; dz++)
            for (int dx = 0; dx < side; dx++)
                (void)fmh_readColumnRawFromDefault(cx0 + dx, cz0 + dz, raw, lens);
    double ms = eden_platform_now_ms() - t0;
    free(raw);
    return ms;
}

// How many of those columns the bundled map actually has, so the harness can report a per-column
// cost against real work rather than against misses (a miss returns before reading anything).
EDEN_EXPORT
int eden_debug_bench_column_hits(int cx0, int cz0, int side) {
    const int bands = fmh_defaultBandCount();
    if (bands <= 0 || side <= 0) return 0;
    unsigned char* raw = (unsigned char*)malloc((size_t)bands * FMH_BAND_RAW_MAX);
    if (!raw) return 0;
    int lens[CHUNKS_PER_COLUMN_MAX];
    int hits = 0;
    for (int dz = 0; dz < side; dz++)
        for (int dx = 0; dx < side; dx++)
            if (fmh_readColumnRawFromDefault(cx0 + dx, cz0 + dz, raw, lens)) hits++;
    free(raw);
    return hits;
}

// Total raw RLE bytes across those columns, i.e. what a "read the whole record in one call" scheme
// would have to size itself against. Reported by tools/headless-column-read-bench.js next to the
// timing, because the answer is what decides how big a single-call read may safely be: in a browser
// the file is the lazy Eden.eden node fetching 32 KB blocks over sync XHR, so read-ahead past what
// the caller wanted costs a real copy (B6 measured a fixed 16 KB read-ahead as a 60% REGRESSION,
// which is how this number came to be worth exporting).
static int g_bench_max_record = 0;

EDEN_EXPORT
int eden_debug_bench_column_bytes(int cx0, int cz0, int side) {
    const int bands = fmh_defaultBandCount();
    g_bench_max_record = 0;
    if (bands <= 0 || side <= 0) return 0;
    unsigned char* raw = (unsigned char*)malloc((size_t)bands * FMH_BAND_RAW_MAX);
    if (!raw) return 0;
    int lens[CHUNKS_PER_COLUMN_MAX];
    long long total = 0;
    int worst = 0;
    for (int dz = 0; dz < side; dz++)
        for (int dx = 0; dx < side; dx++)
            if (fmh_readColumnRawFromDefault(cx0 + dx, cz0 + dz, raw, lens)) {
                int rec = 0;
                for (int i = 0; i < bands; i++) rec += lens[i] + 2;   // +2 = the length prefix
                total += rec;
                if (rec > worst) worst = rec;
            }
    free(raw);
    g_bench_max_record = worst;
    return (int)total;
}

// The largest single record the last eden_debug_bench_column_bytes() call saw.
EDEN_EXPORT
int eden_debug_bench_column_maxbytes(void) { return g_bench_max_record; }

} // extern "C"
