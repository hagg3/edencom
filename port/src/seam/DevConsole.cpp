// DevConsole.cpp — harness accessors for the native session driver (port/src/harness/'s
// --stage1 / --live probes), NOT a console: the player's Console is Classes/Console.{h,mm}. Kept under
// this historical name because ~64 harness call sites use these exports. Gated behind EDEN_DIAGNOSTICS
// (CMakeLists.txt) so it never ships in a build meant to be played.
//
// Plain C exports, static buffer, single frame use (the DebugState.cpp convention).
#include "../shim/foundation/eden_base.h"
#include <unistd.h>
#include "../shim/foundation/eden_image.h"
#include "../../../Classes/World.h"
#include "../../../Classes/Constants.h"
#include "../../../Classes/Model.h"
#include "../shim/foundation/platform_shims.h"   // (Phase N Stage 1)
#include <cstdio>
#ifdef __APPLE__
#include <TargetConditionals.h>
#endif
#if defined(__APPLE__) && !TARGET_OS_IPHONE
#include <libproc.h>
#include <sys/resource.h>
#endif
#include <chrono>

extern "C" {

// tp x y z — Vector convention (y UP, per CLAUDE.md #1 "Vector.y is up"), NOT Terrain's (x,z,y)
// argument order — this writes Player::pos directly, so it takes exactly what that field expects.
// No bounds/collision check: a console teleport is allowed to put the player somewhere the normal
// game never would (e.g. outside the resident toroidal window), same as it would in any game with
// a noclip-style teleport.
int eden_console_teleport(float x, float y, float z) {
    if (!World::getWorld || !World::getWorld->player) return 0;
    Player* p = World::getWorld->player;
    p->pos.x = x;
    p->pos.y = y;
    p->pos.z = z;
    p->vel.x = p->vel.y = p->vel.z = 0;  // a stale velocity would immediately walk the player off
    return 1;
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
int eden_console_setblock(int x, int z, int y, int type) {
    if (!World::getWorld || !World::getWorld->terrain) return 0;
    World::getWorld->terrain->updateChunks(x, z, y, type);
    return 1;
}

// S.0(a) save bench (X.1): time one saveWorld() and report disk bytes moved by this process.
const char* eden_bench_save(void) {
    static char buf[160];
    unsigned long long r0 = 0, w0 = 0, r1 = 0, w1 = 0;
#if defined(__APPLE__) && !TARGET_OS_IPHONE
    rusage_info_current ri;
    if (proc_pid_rusage(getpid(), RUSAGE_INFO_CURRENT, (rusage_info_t*)&ri) == 0) { r0 = ri.ri_diskio_bytesread; w0 = ri.ri_diskio_byteswritten; }
#endif
    auto t0 = std::chrono::steady_clock::now();
    World::getWorld->fm->saveWorld();
    auto t1 = std::chrono::steady_clock::now();
#if defined(__APPLE__) && !TARGET_OS_IPHONE
    if (proc_pid_rusage(getpid(), RUSAGE_INFO_CURRENT, (rusage_info_t*)&ri) == 0) { r1 = ri.ri_diskio_bytesread; w1 = ri.ri_diskio_byteswritten; }
#endif
    snprintf(buf, sizeof(buf), "{\"ms\":%.3f,\"read\":%llu,\"written\":%llu}",
             std::chrono::duration<double, std::milli>(t1 - t0).count(), r1 - r0, w1 - w0);
    return buf;
}

// getblock x z y -- Terrain's own (x,z,y) order, the read counterpart of eden_console_setblock.
// Returns the block TYPE at that coordinate, or -1 for "outside the world" (which the engine also
// uses for out-of-range y, so a 256z world reporting -1 at y=200 is exactly the regression this
// exists to catch). Added for tools/headless-256z-test.js: reading a block back is the only way to
// prove a tall column was decoded into the right band from a script.
int eden_console_getblock(int x, int z, int y) {
    if (!World::getWorld || !World::getWorld->terrain) return -1;
    return World::getWorld->terrain->getLand(x, z, y);
}

} // extern "C"
