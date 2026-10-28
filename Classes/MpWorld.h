//
//  MpWorld.h
//  Emod — Stage MP / MP.3a, MP.3b, MP.4: the multiplayer session world (WORKING/multiplayer-engine-plan-2026-10-17.md
//  §2, §3, §5-§7, §11). docs/networking.md "The session world" is the reference.
//
//  A session has no file. The world is the flat recipe (seed 0, 256z) plus an in-memory OVERLAY: per
//  chunk column, the sorted list of cells that differ from flat. Columns land as flat + overlay
//  (TerrainGenerator's flat branch calls applyDelta before the GBLOCK copy and addChunk), and the
//  three save sites (evict, bulk reload/warp, autosave) "commit to RAM": commitModified() diffs each
//  modified resident column against flat into the overlay and clears `modified`. Leaving the session
//  drops it all.
//
//  MP.3b: a live session queues the server's world stream (enqueue*) and applies it in order at
//  World::update's step 5c (update()), resident cells through Terrain::setCellState under a 2 ms /
//  24-chunk budget; a REGION pacer asks the 128-block lattice point around the player, and every
//  complete reply is reconciled (the box's cells it doesn't name go back to flat).
//
//  MP.4: the player's edits leave at the intent sites (intent*: one ACTION per cell that changed,
//  burns only for flammables, the follow-up mine when the fuse runs out), signs at SignTool's answer;
//  a peer's edit applies through the engine's own primitives (destroyBlock, paintBlock, burnBlock),
//  which never send.
//
//  Everything here runs on the main thread, except applyDelta, which the load pthread also calls
//  during the initial load; the overlay is frozen then (nothing merges while World::doneLoading==1).
//  This file never includes port/src/net: MpRec has SnapzRec's layout, and the host converts.
//
#ifndef Eden_MpWorld_h
#define Eden_MpWorld_h

#include <cstddef>
#include <cstdint>
#include <string>

class TerrainChunk;

// One SNAPZ record, byte-for-byte eden_net::SnapzRec: x, y (height), z, flag (0 block, 1 air, 3 paint),
// type (the block, -1, or the paint index).
struct MpRec { int32_t x, y, z, flag, type; };

namespace MpWorld {

// ---- the session ------------------------------------------------------------------------------
// TRUE while a session world is set up (FileManager::sessionWorld): from beginSession() to endSession().
bool active();
// Set up an empty session: overlay and session trailer cleared, header synthesised (seed 0, pos = home
// = (65536.5, 34, 65536.5) until setSpawn), the E11 gates applied (creatures off, H.8 off). The world
// is named "mp:<host>:<port>". Call before merging anything and before openSession().
void beginSession(const char* host, int port, const char* displayName);
// SPAWN:x:y:z (engine Vector order: y is up). Sets the header's pos and home.
void setSpawn(float x, float y, float z);
// Ask the menu to load the session world, as its Play does for a file (Menu::session_world_name).
// FALSE if no session is set up or the menu is busy.
bool openSession();
// Free the overlay and the trailer, lift the gates. World::exitToMenu calls it; a no-op outside a session.
void endSession();
// "mp:<host>:<port>" ("" outside a session).
const std::string& worldName();
// The synthesised WorldFileHeader the session loads from and its saves update (pos, yaw).
void* header();      // WorldFileHeader*

// ---- what arrives (main thread) ---------------------------------------------------------------
// One SNAPZ frame, in arrival order. A cell's records within one REGION reply (a burst: frames up to
// and including the first with fewer than 3000 records) merge in any order; the first record of a
// cell in a burst replaces what the overlay held for it. Out-of-range cells are dropped and counted.
// Overlay only (nothing resident is touched): the offline path (--mp-replay's legs 1, 2, 5). A live
// session queues instead (enqueueSnapz below), which also reaches the resident window.
void mergeSnapz(const MpRec* recs, size_t n);
// ACTION relays and restores into the overlay (engine plan §8.2's rules, overlay half): a peer's
// build lands only in air or liquid, a mine makes air, a paint paints a non-air cell, a burn does
// nothing; `server` restores are raw. who == "server" for restores.
void applyAction(const char* who, int x, int y, int z, int mode, int extra);
// SIGNP: upsert into the session trailer (wire y = height). Text is cut to 95 characters.
void applySign(int x, int y, int z, int a, int b, int c, const char* text);

// ---- landing and commit -----------------------------------------------------------------------
// The flat branch's hook: write column (cx, cz)'s delta into its CHUNKS_PER_COLUMN chunks' pblocks/pcolors.
void applyDelta(TerrainChunk** column, int cx, int cz);
// Commit to RAM: every resident column with a modified chunk is diffed against flat into the overlay,
// and its chunks' `modified` cleared. Returns the columns committed.
int commitModified();

// ---- introspection (the --mp-replay gate, diagnostics) ----------------------------------------
int flatType(int y);                 // the flat recipe: 0 bedrock, 1-15 stone, 16-31 dirt, 32 grass
size_t overlayCells();
size_t overlayColumns();
size_t overlayBytes();
int droppedRecords();                // out-of-range cells and bad values, since beginSession
bool overlayCell(int x, int y, int z, int* type, int* color);   // FALSE = flat (not stored)
// The resident window's state, as `port/tools/mp-replay.py expect` computes it from a replay: FNV-1a
// 64 over every cell that differs from flat (type or colour), columns in (cx, cz) order, then x, z, y
// inside a column, each fed as int32 x, int32 y (height), int32 z, uint8 type, uint8 colour (LE).
struct WindowState {
    uint64_t hash;
    int cells;          // cells that differ from flat
    int ox, oz, side;   // window origin chunk and width in chunks
    int missing;        // window columns not resident (a slot holding another column)
    int gblockMismatch; // cells where blockarray disagrees with the chunk's pblocks
};
WindowState windowState();

// Test hooks (--mp-mutate=): the evict/autosave commit does nothing; the landing hook does nothing.
extern bool testNoEvictCommit;
extern bool testNoLandHook;
// --mp-overlay-mb (default 48): past it, the non-resident columns farthest from the player are dropped.
extern size_t overlayCapBytes;

// The Creatures setting as SettingsMenu applies it: remembers the player's choice, answers it minus
// the session gate (E11: no creatures in a session).
bool gateCreatures(bool wanted);

// ==== MP.3b: the live world (engine plan §5-§7) =================================================
// The host's half of a session (port/src/seam/MpLink_native.cpp: the socket, via port/src/net's
// MpSession). MpWorld owns it from setLink() on: tick() calls pump() once a frame (the host drains
// its queues into the enqueue* calls below and handles side events there), the REGION pacer calls
// sendLine(), and dropLink() / endSession() call release() exactly once, never from inside pump().
struct Link {
    virtual ~Link() {}
    virtual void pump() = 0;
    virtual void sendLine(const char* line) = 0;
    virtual void release() = 0;
};
void setLink(Link* l);   // releases any earlier link
Link* link();
void dropLink();         // inside pump(): deferred to the end of that tick

// The ordered world stream (ACTION, SIGNP, SNAPZ in the order the server decided them). Queued here
// and applied by tick() (before the window loads) or update() (in play), never out of order.
// enqueueSnapz also feeds the pacer: a frame of < 3000 records (or 300 ms of quiet) ends a burst,
// and a burst that answered our REGION is reconciled (§6.3) when it is applied.
void enqueueSnapz(const MpRec* recs, size_t n);
void enqueueAction(const char* who, int x, int y, int z, int mode, int extra);
void enqueueSign(int x, int y, int z, int a, int b, int c, const char* text);
size_t queuedEvents();
size_t heldBytes();   // Q.6: queued events + burst/reconcile sets, counted against the session's byte cap

// World::update's first call, every frame in every mode: the link's pump, the REGION pacer, and,
// while the session world has not loaded yet (the join's Syncing step, §7), the queue into the
// overlay at 8 ms a frame (nothing is resident). A no-op with no link and no session.
void tick();
// Step 5c (after CmdScript::update, before prepareAndLoadGeometry), and World::update's modal
// early-outs during play: the queue in order, into the overlay and the resident window
// (Terrain::setCellState), until 2 ms are spent or 24 chunks are dirtied. Play only, never while
// the load pthread runs.
void update(float etime);

// The 128-block REGION lattice and its pacer (§6.1-§6.2). After beginSession + setSpawn: caps =
// the server said CAPS:region (ask the spawn's point now); !caps = the legacy push (never ask).
void startRegions(bool caps);
// The join may load (§7 step 3): the first REGION reply has been applied (reconcile included), or
// its point went unanswered 3 times; legacy: a burst was applied or 3 s passed with none.
bool joinReady();
// A SPAWN mid-session (a server /tp): warp there and ask that point's REGION at once.
void serverSpawn(float x, float y, float z);
// Ask the current point again (a teleport the pacer can't see, diagnostics).
void reaskRegion();
// MP.4's intent sites: this client changed (x, y=height, z) now. A reconcile spares the cell if
// the change is newer than the REGION being reconciled (§6.3).
void noteLocalEdit(int x, int y, int z);

// ==== MP.4: edits out and in (engine plan §8-§10) ==============================================
// The player's intent sites (Player::processInput's build/mine/burn/paint, the double brush, jump-
// and-build, the portal exit) call these IN PLACE OF the Terrain call they wrap, so the arguments are
// in TERRAIN order (x, z, y) like that call. Each runs the engine call; in a session it then sends one
// ACTION per cell that actually changed (the target and y +- 1: both halves of a door or portal, a
// ramp's worked-out orientation, the paint) and stamps each with noteLocalEdit. Outside a session
// they are exactly the Terrain call. Remote edits never come through here (§8.2), so no echo.
void intentBuild(int x, int z, int y);
void intentMine(int x, int z, int y);
void intentPaint(int x, int z, int y, int color);
// E10: the burn goes out only for a flammable target, and marks its BurnNode (mpLit). When that node's
// fuse runs out on time, Terrain::update calls burnFuseOut (wire order, y = height) for the follow-up
// mine; a node ended by its cell going air first (a restore, a peer's mine) sends nothing.
void intentBurn(int x, int z, int y);
void burnFuseOut(int x, int y, int z);
// SignTool::answer: a sign placed or edited (its TrailerSign fields: file order, z = height). Nothing
// is sent for a removal (the server drops a block's signs when it goes air, as retail).
void intentSign(int fx, int fy, int fz, int a, int b, int c, const char* text);
// --mp-mutate=echo: the apply path re-sends every peer edit it applies (the no-echo gate must fail).
extern bool testEcho;


// ==== MP.5: remote players (WORKING/multiplayer-client-plan-2026-10-17.md) ======================
// Other players arrive as POS / VEL / POSVEL lines and are drawn as creature models (the H.12 body
// path, Model.mm's REMOTE_BODY slots) with a name tag. The local player's own POSVEL leaves from
// update() at 4 Hz (and at 1 Hz while standing still, which also refreshes the server's saved position).
// A peer is dropped by "<name> has left." or after kPeerSilenceMs without a line.
// verb 0 POS (v[0..2]), 1 VEL (v[3..5]), 2 POSVEL (v[0..5]); engine Vector order (y up), blocks/second.
void peerSeen(const char* name, int characterType, int verb, const double v[6]);
void peerLeft(const char* name);
// Hud::render: the name tags, projected from the camera (the HUD's point space, y up).
void renderPeerTags();
// characterType 0..10 -> one of Emod's 7 creature models (the other 11 vanilla models are closed assets).
int avatarModel(int characterType);
struct PeerView {
    char name[32];
    int type, motion;          // motion: PB_IDLE / PB_WALK / PB_JUMP
    float x, y, z;             // where the avatar is now
    float tx, ty, tz;          // the last reported position
    float yaw;
    int slot;                  // its Model.mm body slot
};
int peerCount();
bool peerView(int i, PeerView* out);
// Test hook (--mp-replay leg 7): how many POSVEL lines update() has sent.
int posVelSent();

struct Stats {
    int actionsSent, signsSent, peerApplied;   // MP.4
    int regionsSent, repliesComplete, unanswered, reconciles, reconciledCells, residentWrites;
    int applyFrames;          // frames update()/tick() had work in
    int worstFrameChunks;     // most chunks one apply frame dirtied (the 24 cap)
    double worstFrameMs;      // the longest apply frame (budget + one unit of overshoot)
    int lastRegionX, lastRegionZ;
};
Stats stats();
void resetStats();

// Test hooks: --mp-mutate=noreconcile; --mp-reconcile=0 (diagnosis: reconcile off); the pacer's
// clock (ms, monotonic; the replay harness simulates one).
extern bool testNoReconcile;
extern bool reconcileOn;
extern int64_t (*clockMs)();

}  // namespace MpWorld

#endif
