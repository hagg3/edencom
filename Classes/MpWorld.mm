//
//  MpWorld.mm
//  Emod — Stage MP / MP.3a: the session world's overlay, landing hook and commit to RAM; MP.3b: the live
//  world (queue, step 5c apply, REGION pacer, reconcile); MP.4: edits out at the intent sites, peers' edits in. See MpWorld.h and
//  WORKING/multiplayer-engine-plan-2026-10-17.md §2-§7.
//
#import "MpWorld.h"
#import "Globals.h"
#import "World.h"
#import "Terrain.h"
#import "TerrainChunk.h"
#import "FileManager.h"
#import "Menu.h"
#import "Hud.h"
#import "Player.h"
#import "Util.h"
#import "Model.h"
#import "GLWidgets.h"
#import "ChatFeed.h"   // MP.7
#include "WorldTrailer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <deque>
#include <vector>

extern float eden_fov_degrees;   // Settings_web.mm: the vertical FOV setting (MP.5's name tags)
extern block8* blockarray;
extern int g_offcx, g_offcz;   // GBLOCK reads them (Terrain.mm)
extern BurnNode* burnList;      // Terrain.mm: the fires (MP.4 marks the player's own)
extern "C" void eden_reapply_fire_flags(void);   // Terrain.mm (H.8: forced off in a session)

namespace MpWorld {

void peersClear();   // MP.5, below

bool testNoEvictCommit = false;
bool testNoLandHook = false;
size_t overlayCapBytes = size_t(48) << 20;

namespace {

// A column's delta: entries `idx16 << 16 | type8 << 8 | colour8`, sorted (so by idx), where
// idx = x*4096 + z*256 + y inside the column -- CC order at 256z, exactly 16 bits. An entry equal to
// flat (the recipe's type, colour 0) is never stored.
typedef std::vector<uint32_t> ColumnDelta;
const int kColH = 256;       // a session is always 256z
const int kMaxCoord = 1 << 19;   // twoToOne packs 15 bits of chunk: cells at x or z >= 524,288 can't be addressed

std::unordered_map<uint32_t, ColumnDelta> g_overlay;
std::unordered_set<uint64_t> g_burstTouched;   // (colKey << 16 | idx) named by the current REGION reply
bool g_inBurst = false;
int g_dropped = 0;
WorldFileHeader g_header;
NSString* g_name = nil;
bool g_creaturesWanted = true, g_creaturesKnown = false;

inline uint32_t colKey(int cx, int cz) { return ((uint32_t)cx << 15) | (uint32_t)cz; }
inline uint32_t pack(int idx, int t, int c) { return ((uint32_t)idx << 16) | ((uint32_t)(t & 0xff) << 8) | (uint32_t)(c & 0xff); }
inline int entIdx(uint32_t e) { return (int)(e >> 16); }
inline int entType(uint32_t e) { return (int)(signed char)((e >> 8) & 0xff); }
inline int entColor(uint32_t e) { return (int)(e & 0xff); }

bool cellInRange(int x, int y, int z) { return x >= 0 && z >= 0 && x < kMaxCoord && z < kMaxCoord && y >= 0 && y < kColH; }

// The overlay's (type, colour) for a cell, else flat.
void cellState(int x, int y, int z, int* t, int* c) {
    *t = flatType(y); *c = 0;
    auto it = g_overlay.find(colKey(x >> 4, z >> 4));
    if (it == g_overlay.end()) return;
    const int idx = (x & 15) * 4096 + (z & 15) * 256 + y;
    const ColumnDelta& d = it->second;
    auto e = std::lower_bound(d.begin(), d.end(), (uint32_t)idx << 16);
    if (e != d.end() && entIdx(*e) == idx) { *t = entType(*e); *c = entColor(*e); }
}

void setCell(int x, int y, int z, int t, int c) {
    const uint32_t k = colKey(x >> 4, z >> 4);
    const int idx = (x & 15) * 4096 + (z & 15) * 256 + y;
    ColumnDelta& d = g_overlay[k];
    auto e = std::lower_bound(d.begin(), d.end(), (uint32_t)idx << 16);
    const bool have = e != d.end() && entIdx(*e) == idx;
    const bool flat = t == flatType(y) && c == 0;
    if (flat) { if (have) d.erase(e); }
    else if (have) *e = pack(idx, t, c);
    else d.insert(e, pack(idx, t, c));
    if (d.empty()) g_overlay.erase(k);
}

bool columnResident(int cx, int cz) {
    Terrain* ter = World::getWorld->terrain;
    if (!ter || !ter->chunkTable || !ter->loaded) return false;
    TerrainChunk* c = ter->chunkTable[threeToOne(cx, 0, cz)];
    return c && c->pbounds[0] == cx * CHUNK_SIZE && c->pbounds[2] == cz * CHUNK_SIZE;
}

// §3.1's cap: past it, the non-resident columns farthest from the player go whole (the next REGION
// that covers them refills them).
void enforceCap() {
    if (overlayBytes() <= overlayCapBytes) return;
    const Vector p = World::getWorld->terrain && World::getWorld->terrain->loaded ? World::getWorld->player->pos : g_header.pos;
    const int pcx = (int)p.x / CHUNK_SIZE, pcz = (int)p.z / CHUNK_SIZE;
    std::vector<std::pair<long long, uint32_t> > far;
    for (const auto& kv : g_overlay) {
        const int cx = (int)(kv.first >> 15), cz = (int)(kv.first & 0x7fff);
        if (columnResident(cx, cz)) continue;
        const long long dx = cx - pcx, dz = cz - pcz;
        far.push_back(std::make_pair(dx * dx + dz * dz, kv.first));
    }
    std::sort(far.begin(), far.end());
    const size_t target = overlayCapBytes - overlayCapBytes / 10;
    for (size_t i = far.size(); i-- > 0 && overlayBytes() > target;) g_overlay.erase(far[i].second);
}

void resetLive();   // MP.3b's queue, pacer and reconcile (below)

void resetHeader() {
    std::memset(&g_header, 0, sizeof(g_header));
    g_header.level_seed = 0;
    g_header.pos = g_header.home = MakeVector(65536.5f, 34.0f, 65536.5f);
    g_header.version = FILE_VERSION_256Z;
}

}  // namespace

int flatType(int y) {
    if (y == 0) return TYPE_BEDROCK;
    if (y < 0) return TYPE_NONE;
    if (y < T_HEIGHT_DEFAULT / 4) return TYPE_STONE;
    if (y < T_HEIGHT_DEFAULT / 2) return TYPE_DIRT;
    if (y == T_HEIGHT_DEFAULT / 2) return TYPE_GRASS;
    return TYPE_NONE;
}

bool active() {
    World* w = World::getWorld;
    return w && w->fm && w->fm->sessionWorld;
}

bool gateCreatures(bool wanted) {
    g_creaturesWanted = wanted;
    g_creaturesKnown = true;
    return wanted && !active();
}

void beginSession(const char* host, int port, const char* displayName) {
    World* w = World::getWorld;
    ChatFeed::reset();   // MP.7: a new session starts with an empty chat
    g_overlay.clear();
    g_burstTouched.clear();
    g_inBurst = false;
    g_dropped = 0;
    resetLive();
    resetHeader();
    if (displayName) std::snprintf(g_header.name, sizeof(g_header.name), "%s", displayName);
    [g_name release];
    g_name = [[NSString stringWithFormat:@"mp:%s:%d", host ? host : "", port] retain];
    w->fm->sessionWorld = TRUE;
    w->fm->trailer()->clear();
    // E11: no creatures (Model.mm paints blocks nobody else would see), no H.8 fire rules.
    if (!g_creaturesKnown) { g_creaturesWanted = CREATURES_ON; g_creaturesKnown = true; }
    CREATURES_ON = false;
    eden_reapply_fire_flags();
}

void setSpawn(float x, float y, float z) {
    g_header.pos = g_header.home = MakeVector(x, y, z);
}

bool openSession() {
    World* w = World::getWorld;
    if (!active() || !w->menu || w->menu->loading != 0) return false;
    [w->menu->session_world_name release];
    w->menu->session_world_name = [g_name retain];
    w->menu->loading = 1;
    w->menu->sbar->setStatus(@"Joining ", 9999);
    return true;
}

void endSession() {
    World* w = World::getWorld;
    if (!w || !w->fm || !w->fm->sessionWorld) return;
    w->fm->sessionWorld = FALSE;
    std::unordered_map<uint32_t, ColumnDelta>().swap(g_overlay);
    std::unordered_set<uint64_t>().swap(g_burstTouched);
    g_inBurst = false;
    resetLive();
    peersClear();   // MP.5
    dropLink();   // MP.3b: the host closes its socket
    w->fm->trailer()->clear();
    resetHeader();
    if (w->menu) { [w->menu->session_world_name release]; w->menu->session_world_name = nil; }
    [g_name release];
    g_name = nil;
    CREATURES_ON = g_creaturesWanted;
    eden_reapply_fire_flags();
}

void* worldName() { return g_name; }
void* header() { return &g_header; }

// ---- merging -----------------------------------------------------------------------------------

namespace {
bool recValid(const MpRec& r) {
    bool ok = cellInRange(r.x, r.y, r.z);
    if (ok && r.flag == 0) ok = r.type >= 1 && r.type <= NUM_BLOCKS;
    else if (ok && r.flag == 3) ok = r.type >= 0 && r.type <= 54;
    else if (ok) ok = r.flag == 1;
    return ok;
}

// The overlay half of a SNAPZ frame, inside the current burst (g_burstTouched); ending the burst is
// the caller's.
void mergeRecs(const MpRec* recs, size_t n) {
    g_inBurst = true;
    // Bucket the frame by column, keep arrival order inside a cell, then one sorted merge a column.
    struct Op { uint16_t idx; uint8_t flag; int16_t val; uint32_t seq; };
    std::unordered_map<uint32_t, std::vector<Op> > buckets;
    for (size_t i = 0; i < n; ++i) {
        const MpRec& r = recs[i];
        if (!recValid(r)) { g_dropped++; continue; }
        Op op = {(uint16_t)((r.x & 15) * 4096 + (r.z & 15) * 256 + r.y), (uint8_t)r.flag, (int16_t)r.type, (uint32_t)i};
        buckets[colKey(r.x >> 4, r.z >> 4)].push_back(op);
    }
    for (auto& kv : buckets) {
        std::vector<Op>& ops = kv.second;
        std::sort(ops.begin(), ops.end(), [](const Op& a, const Op& b) { return a.idx != b.idx ? a.idx < b.idx : a.seq < b.seq; });
        ColumnDelta& d = g_overlay[kv.first];
        ColumnDelta out;
        out.reserve(d.size() + ops.size());
        size_t i = 0;
        for (size_t j = 0; j < ops.size();) {
            const int idx = ops[j].idx, y = idx & 255;
            while (i < d.size() && entIdx(d[i]) < idx) out.push_back(d[i++]);
            const bool have = i < d.size() && entIdx(d[i]) == idx;
            int t = flatType(y), c = 0;
            // The first record of a cell in this reply replaces it; later ones (its flag 0/3 partner,
            // possibly in a later frame) refine it.
            const bool first = g_burstTouched.insert(((uint64_t)kv.first << 16) | (uint64_t)idx).second;
            if (have && !first) { t = entType(d[i]); c = entColor(d[i]); }
            if (have) i++;
            for (; j < ops.size() && ops[j].idx == idx; ++j) {
                if (ops[j].flag == 0) t = ops[j].val;
                else if (ops[j].flag == 1) { t = TYPE_NONE; c = 0; }
                else c = ops[j].val;
            }
            if (t != flatType(y) || c != 0) out.push_back(pack(idx, t, c));
        }
        while (i < d.size()) out.push_back(d[i++]);
        if (out.empty()) g_overlay.erase(kv.first);
        else d.swap(out);
    }
}

// Engine plan §8.2's rules at the state level, on (t, c) = the cell's current state. Returns 1 =
// write (t, c), 0 = nothing to do, -1 = a bad value (dropped).
int actionRule(bool server, int mode, int extra, int* t, int* c) {
    switch (mode) {
        case 0:
            if (extra < 0 || extra > NUM_BLOCKS) return -1;
            // A peer's bare build does not overwrite (protocol "legacy snapshot"); a restore does.
            if (!server && *t != TYPE_NONE && !(*t > 0 && (blockinfo[*t] & IS_LIQUID))) return 0;
            *t = extra; *c = 0;
            return 1;
        case 1:
            if (!server && *t == TYPE_NONE) return 0;
            *t = TYPE_NONE; *c = 0;
            return 1;
        case 3:
            if (extra < 0 || extra > 54) return -1;
            if (!server && *t == TYPE_NONE) return 0;
            *c = extra;
            return 1;
        default:   // 2 burn: the server applies it, the next REGION brings the result (§8.2; MP.4 runs it locally)
            return 0;
    }
}
}  // namespace

void mergeSnapz(const MpRec* recs, size_t n) {
    mergeRecs(recs, n);
    if (n < 3000) { g_inBurst = false; g_burstTouched.clear(); }
    enforceCap();
}

void applyAction(const char* who, int x, int y, int z, int mode, int extra) {
    if (!cellInRange(x, y, z)) { g_dropped++; return; }
    const bool server = who && !std::strcmp(who, "server");
    int t, c;
    cellState(x, y, z, &t, &c);
    const int r = actionRule(server, mode, extra, &t, &c);
    if (r < 0) g_dropped++;
    if (r > 0) setCell(x, y, z, t, c);
}

void applySign(int x, int y, int z, int a, int b, int c, const char* text) {
    if (!cellInRange(x, y, z)) { g_dropped++; return; }
    WorldTrailer* tr = World::getWorld->fm->trailer();
    // SGN1 holds 95 printable ASCII; the server keeps 256 bytes of anything but control characters
    // (§10), so a UTF-8 sequence becomes one '?' (the raster is ASCII-only) and the rest is cut at 95.
    // a/b/c go in as sent (servers carry b = 0 from old sidecars), as a loaded file's would.
    char t[96];
    size_t n = 0;
    for (const unsigned char* s = (const unsigned char*)(text ? text : ""); *s && n < sizeof(t) - 1; ++s) {
        if (*s >= 0x20 && *s <= 0x7e) t[n++] = (char)*s;
        else if (*s >= 0xc0) t[n++] = '?';   // a UTF-8 lead byte; its continuation bytes (0x80-0xbf) are skipped
    }
    t[n] = 0;
    const TrailerPos p = TrailerPos::fromEngine(x, y, z);
    const int i = tr->findSign(p, a);
    if (i >= 0) tr->removeSign((size_t)i);
    if (tr->addSign(p, a, b, c, t, true) != WorldTrailer::TR_OK) g_dropped++;
}

// ---- landing and commit --------------------------------------------------------------------------

void applyDelta(TerrainChunk** column, int cx, int cz) {
    if (testNoLandHook) return;
    auto it = g_overlay.find(colKey(cx, cz));
    if (it == g_overlay.end()) return;
    for (uint32_t e : it->second) {
        const int idx = entIdx(e), lx = idx >> 12, lz = (idx >> 8) & 15, y = idx & 255;
        if (y >= T_HEIGHT) continue;
        TerrainChunk* ch = column[y / CHUNK_SIZE];
        const int i = lx * (CHUNK_SIZE * CHUNK_SIZE) + lz * CHUNK_SIZE + (y % CHUNK_SIZE);
        ch->pblocks[i] = (block8)entType(e);
        ch->pcolors[i] = (color8)entColor(e);
    }
}

namespace {
// One resident column (cx, cz) to RAM if any of its chunks is modified (§3.2). TRUE = committed.
bool commitColumn(int cx, int cz) {
    Terrain* ter = World::getWorld->terrain;
    TerrainChunk* col[CHUNKS_PER_COLUMN_MAX];
    bool dirty = false;
    for (int cy = 0; cy < CHUNKS_PER_COLUMN; ++cy) {
        col[cy] = ter->chunkTable[threeToOne(cx, cy, cz)];
        dirty = dirty || col[cy]->modified;
    }
    if (!dirty) return false;
    // x, z, y order is idx order, so the diff comes out sorted.
    ColumnDelta d;
    for (int lx = 0; lx < CHUNK_SIZE; ++lx)
        for (int lz = 0; lz < CHUNK_SIZE; ++lz)
            for (int y = 0; y < kColH; ++y) {
                const TerrainChunk* ch = col[y / CHUNK_SIZE];
                const int i = lx * (CHUNK_SIZE * CHUNK_SIZE) + lz * CHUNK_SIZE + (y % CHUNK_SIZE);
                const int t = ch->pblocks[i], c = ch->pcolors[i];
                if (t != flatType(y) || c != 0) d.push_back(pack(lx * 4096 + lz * 256 + y, t, c));
            }
    if (d.empty()) g_overlay.erase(colKey(cx, cz));
    else g_overlay[colKey(cx, cz)].swap(d);
    for (int cy = 0; cy < CHUNKS_PER_COLUMN; ++cy) col[cy]->modified = FALSE;
    return true;
}
}  // namespace

int commitModified() {
    Terrain* ter = World::getWorld->terrain;
    if (!ter || !ter->chunkTable || T_HEIGHT != kColH) return 0;
    int n = 0;
    for (int sx = 0; sx < CHUNKS_PER_SIDE; ++sx) {
        for (int sz = 0; sz < CHUNKS_PER_SIDE; ++sz) {
            TerrainChunk* base = ter->chunkTable[threeToOne(sx, 0, sz)];
            if (!base || base->pbounds[1] != 0) continue;
            if (commitColumn(base->pbounds[0] / CHUNK_SIZE, base->pbounds[2] / CHUNK_SIZE)) n++;
        }
    }
    return n;
}

// ---- introspection ---------------------------------------------------------------------------------

size_t overlayCells() {
    size_t n = 0;
    for (const auto& kv : g_overlay) n += kv.second.size();
    return n;
}
size_t overlayColumns() { return g_overlay.size(); }
size_t overlayBytes() {
    size_t n = 0;
    for (const auto& kv : g_overlay) n += kv.second.capacity() * sizeof(uint32_t) + 64;
    return n;
}
int droppedRecords() { return g_dropped; }

bool overlayCell(int x, int y, int z, int* type, int* color) {
    if (!cellInRange(x, y, z)) return false;
    int t, c;
    cellState(x, y, z, &t, &c);
    if (type) *type = t;
    if (color) *color = c;
    return t != flatType(y) || c != 0;
}

WindowState windowState() {
    WindowState s = {1469598103934665603ULL, 0, 0, 0, CHUNKS_PER_SIDE, 0, 0};
    World* w = World::getWorld;
    Terrain* ter = w->terrain;
    s.ox = windowOriginChunk(w->player->pos.x);
    s.oz = windowOriginChunk(w->player->pos.z);
    auto feed = [&s](const void* p, size_t n) {
        const unsigned char* b = (const unsigned char*)p;
        for (size_t i = 0; i < n; ++i) { s.hash ^= b[i]; s.hash *= 1099511628211ULL; }
    };
    for (int cx = s.ox; cx < s.ox + s.side; ++cx) {
        for (int cz = s.oz; cz < s.oz + s.side; ++cz) {
            if (!columnResident(cx, cz)) { s.missing++; continue; }
            TerrainChunk* col[CHUNKS_PER_COLUMN_MAX];
            for (int cy = 0; cy < CHUNKS_PER_COLUMN; ++cy) col[cy] = ter->chunkTable[threeToOne(cx, cy, cz)];
            for (int lx = 0; lx < CHUNK_SIZE; ++lx)
                for (int lz = 0; lz < CHUNK_SIZE; ++lz)
                    for (int y = 0; y < T_HEIGHT; ++y) {
                        const TerrainChunk* ch = col[y / CHUNK_SIZE];
                        const int i = lx * (CHUNK_SIZE * CHUNK_SIZE) + lz * CHUNK_SIZE + (y % CHUNK_SIZE);
                        const int t = ch->pblocks[i], c = ch->pcolors[i];
                        const int32_t x = cx * CHUNK_SIZE + lx, z = cz * CHUNK_SIZE + lz;
                        if (GBLOCK(x, z, y) != ch->pblocks[i]) s.gblockMismatch++;
                        if (t == flatType(y) && c == 0) continue;
                        const int32_t yy = y;
                        const uint8_t tc[2] = {(uint8_t)t, (uint8_t)c};
                        feed(&x, 4); feed(&yy, 4); feed(&z, 4); feed(tc, 2);
                        s.cells++;
                    }
        }
    }
    return s;
}

// ==== MP.3b: the live world ========================================================================

bool testNoReconcile = false;
bool reconcileOn = true;
bool testEcho = false;

namespace {

int64_t steadyMs() {
    return (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}
double realMs() {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

const int kLattice = 128;          // §6.1: REGION points sit on a 128-block lattice
const int kSwitch = 64 + 8;        // ... switched with 8 blocks of hysteresis
const int kReconcileHalf = 208;    // §6.3: the server's R = 224 less one chunk
const int kFullFrame = 3000;
const int64_t kGapMs = 750, kQuietMs = 300, kUnansweredMs = 3000;
const int kMaxAsks = 3;
const double kPlayMs = 2.0, kJoinMs = 8.0;
const int kPlayChunks = 24;

inline uint64_t cellKey(int x, int y, int z) {
    return ((uint64_t)colKey(x >> 4, z >> 4) << 16) | (uint64_t)((x & 15) * 4096 + (z & 15) * 256 + y);
}
inline int keyX(uint64_t k) { return (int)((k >> 31) & 0x7fff) * 16 + (int)((k >> 12) & 15); }
inline int keyZ(uint64_t k) { return (int)((k >> 16) & 0x7fff) * 16 + (int)((k >> 8) & 15); }
inline int keyY(uint64_t k) { return (int)(k & 255); }

struct Ev {
    enum Kind { Snapz, Action, Sign, EndBurst } kind = Snapz;
    int64_t t = 0;                       // enqueue time (clockMs): the "Syncing..." age
    int x = 0, y = 0, z = 0, mode = 0, extra = 0, a = 0, b = 0, c = 0;
    bool solicited = false;              // EndBurst: answered our REGION at (x, z), sent at sentAt
    int64_t sentAt = 0;
    std::string who, text;
    std::vector<MpRec> recs;
    bool started = false;                // Snapz: the non-resident half is merged, `res` is what's left
    size_t cur = 0;
    std::vector<uint32_t> res;
};
std::deque<Ev> g_q;
std::unordered_set<uint64_t> g_burstRelayed;   // cells an ACTION changed inside the open burst (§6.3)
std::unordered_map<uint64_t, int64_t> g_localEdits;   // noteLocalEdit: cell -> when
Link* g_link = nullptr;
bool g_inPump = false, g_dropPending = false;
Stats g_stats;

struct Pacer {
    bool on = false, legacy = false;
    bool havePoint = false; int px = 0, pz = 0;   // the current lattice point
    bool want = false; int asks = 0; bool gaveUp = false;
    bool inFlight = false; int ax = 0, az = 0; int64_t sentAt = 0; int frames = 0;
    bool burstOpen = false; int64_t lastFrame = 0, gapUntil = 0, startedAt = 0;
    bool firstReplyApplied = false, legacyBurstApplied = false;
} g_pacer;

struct Recon {
    bool on = false;
    int px = 0, pz = 0; int64_t sentAt = 0;
    std::unordered_set<uint64_t> named;
    std::vector<uint32_t> cols; size_t ci = 0;
    bool colReady = false;
    std::vector<uint64_t> todo; size_t ti = 0;
} g_recon;

struct Budget {
    double t0, ms; int cap;
    std::unordered_set<uint64_t> chunks;
    bool worked = false;
    Budget(double m, int c) : t0(realMs()), ms(m), cap(c) {}
    bool over() const { return (cap && (int)chunks.size() >= cap) || realMs() - t0 >= ms; }
    // The chunks updateChunks queues for a rebuild: the cell's, and its 6 neighbours'.
    void dirty(int x, int y, int z) {
        worked = true;
        const int d[7][3] = {{0, 0, 0}, {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
        for (const auto& o : d) {
            const int yy = y + o[1];
            if (yy < 0 || yy >= T_HEIGHT) continue;
            chunks.insert(((uint64_t)(unsigned)((x + o[0]) >> 4) << 32) | ((uint64_t)(unsigned)((z + o[2]) >> 4) << 8) |
                          (uint64_t)(yy >> 4));
        }
    }
    void finish() {
        if (!worked) return;
        g_stats.applyFrames++;
        g_stats.worstFrameChunks = std::max(g_stats.worstFrameChunks, (int)chunks.size());
        g_stats.worstFrameMs = std::max(g_stats.worstFrameMs, realMs() - t0);
    }
};

int latticeOf(float p) { return kLattice * (int)std::floor(p / (float)kLattice + 0.5f); }

void resetLive() {
    g_q.clear();
    std::unordered_set<uint64_t>().swap(g_burstRelayed);
    std::unordered_map<uint64_t, int64_t>().swap(g_localEdits);
    g_pacer = Pacer();
    g_recon = Recon();
}

// A resident cell to (t, c), through the engine's write sequence. The chunk goes `modified`, so the
// next commit folds it into the overlay.
void writeResident(int x, int y, int z, int t, int c, Budget& b) {
    Terrain* ter = World::getWorld->terrain;
    if (ter->getLand(x, z, y) == t && ter->getColor(x, z, y) == c) return;
    ter->setCellState(x, z, y, t, t == TYPE_NONE && c == 0 ? -1 : c);
    g_stats.residentWrites++;
    b.dirty(x, y, z);
}

void sendLine(const char* l) { if (g_link) g_link->sendLine(l); }

void endBurst(int64_t now) {
    Ev e;
    e.kind = Ev::EndBurst;
    e.t = now;
    Pacer& p = g_pacer;
    e.solicited = p.inFlight && p.frames > 0;
    e.x = p.ax; e.z = p.az; e.sentAt = p.sentAt;
    if (p.inFlight) {
        p.inFlight = false;
        p.asks = 0;
        p.gapUntil = now + kGapMs;
        g_stats.repliesComplete++;
    }
    p.burstOpen = false;
    p.frames = 0;
    g_q.push_back(std::move(e));
}

void pacerTick() {
    Pacer& p = g_pacer;
    if (!p.on) return;
    const int64_t now = clockMs();
    if (p.burstOpen && now - p.lastFrame >= kQuietMs) endBurst(now);
    if (p.inFlight && p.frames == 0 && now - p.sentAt >= kUnansweredMs) {
        // Silence is "unanswered", not "empty": the server may have refused it for backpressure.
        p.inFlight = false;
        p.gapUntil = now + kGapMs;
        g_stats.unanswered++;
        if (++p.asks < kMaxAsks) p.want = true;
        else p.gaveUp = true;   // wait for the next point change
    }
    if (p.legacy) return;
    World* w = World::getWorld;
    const bool playing = w->terrain && w->terrain->loaded && w->game_mode == GAME_MODE_PLAY;
    const Vector pos = playing ? w->player->pos : g_header.pos;
    if (!p.havePoint || std::fabs(pos.x - (float)p.px) > kSwitch || std::fabs(pos.z - (float)p.pz) > kSwitch) {
        p.havePoint = true;
        p.px = latticeOf(pos.x);
        p.pz = latticeOf(pos.z);
        p.want = true;   // re-asked every time it becomes current, never deduplicated
        p.asks = 0;
    }
    if (p.want && !p.inFlight && now >= p.gapUntil) {
        p.want = false;
        if (p.px < 0 || p.pz < 0 || p.px > 0xFFFFFF || p.pz > 0xFFFFFF) return;   // never sent
        char l[48];
        std::snprintf(l, sizeof l, "REGION:%d:%d", p.px, p.pz);
        sendLine(l);
        p.inFlight = true;
        p.ax = p.px; p.az = p.pz;
        p.sentAt = now;
        p.frames = 0;
        g_stats.regionsSent++;
        g_stats.lastRegionX = p.px;
        g_stats.lastRegionZ = p.pz;
    }
}

// ---- applying one event ------------------------------------------------------------------------

// A SNAPZ frame: the columns not resident when it starts merge into the overlay at once (cheap); the
// resident ones go cell by cell under the budget. TRUE = done.
bool applySnapz(Ev& e, Budget& b) {
    if (!e.started) {
        e.started = true;
        std::vector<MpRec> rest;
        rest.reserve(e.recs.size());
        std::unordered_map<uint32_t, bool> res;
        for (size_t i = 0; i < e.recs.size(); ++i) {
            const MpRec& r = e.recs[i];
            bool resident = false;
            if (recValid(r)) {
                const uint32_t k = colKey(r.x >> 4, r.z >> 4);
                auto it = res.find(k);
                if (it == res.end()) it = res.insert(std::make_pair(k, columnResident(r.x >> 4, r.z >> 4))).first;
                resident = it->second;
            }
            if (resident) e.res.push_back((uint32_t)i);
            else rest.push_back(r);
        }
        mergeRecs(rest.data(), rest.size());
        g_inBurst = true;
        enforceCap();
    }
    while (e.cur < e.res.size()) {
        if (b.over()) return false;
        // One cell's run (its flag 0 and flag 3 records are adjacent in the server's order).
        const MpRec& r0 = e.recs[e.res[e.cur]];
        size_t j = e.cur;
        while (j < e.res.size()) {
            const MpRec& r = e.recs[e.res[j]];
            if (r.x != r0.x || r.y != r0.y || r.z != r0.z) break;
            j++;
        }
        if (!columnResident(r0.x >> 4, r0.z >> 4)) {   // evicted (committed) since the frame started
            std::vector<MpRec> run;
            for (size_t k = e.cur; k < j; ++k) run.push_back(e.recs[e.res[k]]);
            mergeRecs(run.data(), run.size());
        } else {
            Terrain* ter = World::getWorld->terrain;
            int t = flatType(r0.y), c = 0;
            if (!g_burstTouched.insert(cellKey(r0.x, r0.y, r0.z)).second) {
                t = ter->getLand(r0.x, r0.z, r0.y);
                c = ter->getColor(r0.x, r0.z, r0.y);
            }
            for (size_t k = e.cur; k < j; ++k) {
                const MpRec& r = e.recs[e.res[k]];
                if (r.flag == 0) t = r.type;
                else if (r.flag == 1) { t = TYPE_NONE; c = 0; }
                else c = r.type;
            }
            writeResident(r0.x, r0.y, r0.z, t, c, b);
        }
        e.cur = j;
    }
    return true;
}

void applyActionLive(const Ev& e, Budget& b) {
    if (!cellInRange(e.x, e.y, e.z)) { g_dropped++; return; }
    if (g_inBurst) g_burstRelayed.insert(cellKey(e.x, e.y, e.z));
    const bool server = e.who == "server";
    if (testEcho && !server) {   // --mp-mutate=echo: what the no-echo gate must catch
        char l[64];
        std::snprintf(l, sizeof l, e.mode == 0 || e.mode == 3 ? "ACTION:%d:%d:%d:%d:%d" : "ACTION:%d:%d:%d:%d", e.x, e.y, e.z,
                      e.mode, e.extra);
        sendLine(l);
    }
    // Not resident: the overlay's rules (a peer's burn is ignored; the server applied it and the
    // next REGION brings the result).
    if (!columnResident(e.x >> 4, e.z >> 4)) { applyAction(e.who.c_str(), e.x, e.y, e.z, e.mode, e.extra); return; }
    Terrain* ter = World::getWorld->terrain;
    int t = ter->getLand(e.x, e.z, e.y), c = ter->getColor(e.x, e.z, e.y);
    if (server || e.mode == 0) {   // restores raw; a peer's build only into air or liquid, unpainted
        const int r = actionRule(server, e.mode, e.extra, &t, &c);
        if (r < 0) g_dropped++;
        if (r > 0) writeResident(e.x, e.y, e.z, t, c, b);
        return;
    }
    // §8.2 (MP.4): a peer's mine, burn or paint through the engine's own primitives -- break particles,
    // door and portal pairs, light, liquid sources, the fire simulation -- never the intent* wrappers,
    // so nothing here sends. What they leave is committed like any local result.
    switch (e.mode) {
        case 1:
            if (t == TYPE_NONE) return;
            ter->destroyBlock(e.x, e.z, e.y);
            break;
        case 2:
            if (t <= 0 || !(blockinfo[t] & IS_FLAMMABLE)) return;
            ter->burnBlock(e.x, e.z, e.y, FALSE);
            break;
        case 3:
            if (e.extra < 0 || e.extra > 54) { g_dropped++; return; }
            if (t == TYPE_NONE) return;
            ter->paintBlock(e.x, e.z, e.y, e.extra);
            break;
        default: return;
    }
    g_stats.peerApplied++;
    g_stats.residentWrites++;
    b.dirty(e.x, e.y, e.z);
}

bool exempt(uint64_t key) {
    auto it = g_localEdits.find(key);
    return it != g_localEdits.end() && it->second >= g_recon.sentAt;
}

// §6.3: every cell of the box the reply didn't name goes back to flat. Column by column; a resident
// column is committed first, so local uncommitted results are part of what is reconciled. TRUE = done.
bool stepReconcile(Budget& b) {
    Recon& R = g_recon;
    while (R.ci < R.cols.size()) {
        if (b.over()) return false;
        const uint32_t k = R.cols[R.ci];
        const int cx = (int)(k >> 15), cz = (int)(k & 0x7fff);
        const bool resident = columnResident(cx, cz);
        if (!R.colReady) {
            if (resident) commitColumn(cx, cz);
            R.todo.clear();
            R.ti = 0;
            auto it = g_overlay.find(k);
            if (it != g_overlay.end()) {
                ColumnDelta& d = it->second;
                ColumnDelta keep;
                for (uint32_t ent : d) {
                    const uint64_t key = ((uint64_t)k << 16) | (uint64_t)entIdx(ent);
                    if (R.named.count(key) || exempt(key)) keep.push_back(ent);
                    else R.todo.push_back(key);
                }
                if (!resident) {   // overlay only: drop them in one pass (a resident one goes cell by cell below)
                    g_stats.reconciledCells += (int)R.todo.size();
                    if (keep.empty()) g_overlay.erase(it);
                    else d.swap(keep);
                    R.ci++;
                    continue;
                }
            }
            R.colReady = true;
        }
        while (R.ti < R.todo.size()) {
            if (b.over()) return false;
            const uint64_t key = R.todo[R.ti++];
            if (exempt(key)) continue;   // the player may have edited it since the column was listed
            const int x = keyX(key), y = keyY(key), z = keyZ(key);
            if (columnResident(cx, cz)) writeResident(x, y, z, flatType(y), 0, b);
            setCell(x, y, z, flatType(y), 0);
            g_stats.reconciledCells++;
        }
        R.colReady = false;
        R.ci++;
    }
    for (auto it = g_localEdits.begin(); it != g_localEdits.end();) {   // now part of the server's answer
        if (it->second < R.sentAt) it = g_localEdits.erase(it);
        else ++it;
    }
    R.on = false;
    std::unordered_set<uint64_t>().swap(R.named);
    g_stats.reconciles++;
    return true;
}

bool applyEndBurst(Ev& e, Budget& b) {
    if (!e.started) {
        e.started = true;
        g_inBurst = false;
        if (e.solicited && reconcileOn && !testNoReconcile) {   // resident columns are 256z by construction
            Recon& R = g_recon;
            R = Recon();
            R.on = true;
            R.px = e.x; R.pz = e.z; R.sentAt = e.sentAt;
            R.named.swap(g_burstTouched);
            R.named.insert(g_burstRelayed.begin(), g_burstRelayed.end());
            const int c0x = (e.x - kReconcileHalf) >> 4, c1x = (e.x + kReconcileHalf) >> 4;
            const int c0z = (e.z - kReconcileHalf) >> 4, c1z = (e.z + kReconcileHalf) >> 4;
            for (int cx = std::max(c0x, 0); cx <= c1x && cx < kMaxCoord / 16; ++cx)
                for (int cz = std::max(c0z, 0); cz <= c1z && cz < kMaxCoord / 16; ++cz) R.cols.push_back(colKey(cx, cz));
        }
        g_burstTouched.clear();
        g_burstRelayed.clear();
    }
    if (g_recon.on && !stepReconcile(b)) return false;
    if (e.solicited) g_pacer.firstReplyApplied = true;
    else g_pacer.legacyBurstApplied = true;
    return true;
}

void drain(Budget& b) {
    while (!g_q.empty()) {
        if (b.over()) break;
        Ev& e = g_q.front();
        bool done = true;
        switch (e.kind) {
            case Ev::Snapz: done = applySnapz(e, b); break;
            case Ev::Action: applyActionLive(e, b); break;
            case Ev::Sign: applySign(e.x, e.y, e.z, e.a, e.b, e.c, e.text.c_str()); break;
            case Ev::EndBurst: done = applyEndBurst(e, b); break;
        }
        if (!done) break;   // FIFO: a deferred event holds back everything behind it
        g_q.pop_front();
    }
    b.finish();
}

void releaseLink() {
    Link* l = g_link;
    g_link = nullptr;
    g_dropPending = false;
    if (l) l->release();
}

}  // namespace

int64_t (*clockMs)() = steadyMs;

void setLink(Link* l) {
    if (g_link && g_link != l) releaseLink();
    g_link = l;
}
Link* link() { return g_link; }
void dropLink() {
    if (g_inPump) g_dropPending = true;
    else releaseLink();
}

void enqueueSnapz(const MpRec* recs, size_t n) {
    const int64_t now = clockMs();
    Pacer& p = g_pacer;
    if (p.inFlight) p.frames++;
    p.burstOpen = true;
    p.lastFrame = now;
    Ev e;
    e.kind = Ev::Snapz;
    e.t = now;
    e.recs.assign(recs, recs + n);
    g_q.push_back(std::move(e));
    if (n < (size_t)kFullFrame) endBurst(now);
}

void enqueueAction(const char* who, int x, int y, int z, int mode, int extra) {
    Ev e;
    e.kind = Ev::Action;
    e.t = clockMs();
    e.who = who ? who : "";
    e.x = x; e.y = y; e.z = z; e.mode = mode; e.extra = extra;
    g_q.push_back(std::move(e));
}

void enqueueSign(int x, int y, int z, int a, int b, int c, const char* text) {
    Ev e;
    e.kind = Ev::Sign;
    e.t = clockMs();
    e.x = x; e.y = y; e.z = z; e.a = a; e.b = b; e.c = c;
    e.text = text ? text : "";
    g_q.push_back(std::move(e));
}

size_t queuedEvents() { return g_q.size(); }

void tick() {
    if (g_link) {
        g_inPump = true;
        g_link->pump();
        g_inPump = false;
        if (g_dropPending) releaseLink();
    }
    if (!active()) return;
    pacerTick();
    World* w = World::getWorld;
    // §7 step 2 (Syncing): before the window loads, everything goes into the overlay. Not while the
    // load pthread reads it (doneLoading), and not once the world is up (update() takes over).
    if (w->doneLoading == 0 && !(w->terrain && w->terrain->loaded)) {
        Budget b(kJoinMs, 0);
        drain(b);
    }
}


// ==== MP.5: remote players ==========================================================================

namespace {

const int kPeerSilenceMs = 30000;     // a peer that said nothing this long is gone (we send/expect 1 Hz at least)
const int kPosVelMovingMs = 250, kPosVelIdleMs = 1000;
const float kSnapDist = 12.0f;        // farther than this from where it should be: jump, don't glide
const float kMaxExtrapolate = 0.6f;   // seconds of dead reckoning past the last report
const float kTagRange = 64.0f;

struct Peer {
    bool used = false;
    std::string name;
    int type = 0;
    Vector pos, target, vel;   // pos: shown; target: last report; vel: last report (blocks/s)
    bool haveVel = false;
    int64_t lastMs = 0;        // last line from them
    float yaw = 0;
    int motion = 0;
    GLW::Label tag;
    bool haveTag = false;
    int lastTagType = -1;
};
Peer g_peers[MAX_REMOTE_BODIES];
int g_posVelSent = 0;
int64_t g_lastSendMs = 0;
float g_lastSent[6] = {0, 0, 0, 0, 0, 0};
bool g_sentOnce = false;

Peer* findPeer(const char* name) {
    for (auto& p : g_peers) if (p.used && p.name == name) return &p;
    return nullptr;
}

void freePeer(int i) {
    g_peers[i].used = false;
    g_peers[i].tag.clear();
    g_peers[i].haveTag = false;
    HideRemoteBody(i);
}

float angleLerp(float a, float b, float t) {   // degrees, the short way round
    float d = std::fmod(b - a + 540.0f, 360.0f) - 180.0f;
    return a + d * t;
}

}  // namespace

int avatarModel(int t) {
    // Vanilla's characterType picks one of 18 models, 7 of which Emod ships (the rest are closed assets).
    static const int kMap[11] = {M_GREEN, M_MOOF, M_BATTY, M_NERGLE, M_STUMPY, M_CHARGER, M_STALKER,
                                 M_GREEN, M_MOOF, M_BATTY, M_NERGLE};
    return kMap[t < 0 ? 0 : (t > 10 ? 10 : t)];
}

void peerSeen(const char* name, int characterType, int verb, const double v[6]) {
    if (!name || !*name || !active()) return;
    Peer* p = findPeer(name);
    const int64_t now = clockMs();
    if (!p) {
        int slot = -1;
        for (int i = 0; i < MAX_REMOTE_BODIES; ++i) if (!g_peers[i].used) { slot = i; break; }
        if (slot < 0) return;   // 16 others are plenty; the rest are not drawn
        p = &g_peers[slot];
        p->used = true;
        p->name = name;
        p->haveVel = false;
        p->vel = MakeVector(0, 0, 0);
        p->pos = p->target = MakeVector((float)v[0], (float)v[1], (float)v[2]);
        p->haveTag = false;
        p->motion = 0;
        p->yaw = 0;
    }
    p->type = characterType;
    if (verb != 1) p->target = MakeVector((float)v[0], (float)v[1], (float)v[2]);
    if (verb != 0) { p->vel = MakeVector((float)v[3], (float)v[4], (float)v[5]); p->haveVel = true; }
    else if (!p->haveVel) p->vel = MakeVector(0, 0, 0);
    p->lastMs = now;
    if (verb == 1 && !p->haveVel) p->haveVel = true;
}

void peerLeft(const char* name) {
    for (int i = 0; i < MAX_REMOTE_BODIES; ++i)
        if (g_peers[i].used && g_peers[i].name == name) freePeer(i);
}

void peersClear() {
    for (int i = 0; i < MAX_REMOTE_BODIES; ++i) if (g_peers[i].used) freePeer(i);
    g_sentOnce = false;
}

static void peersUpdate(float etime) {
    World* w = World::getWorld;
    const int64_t now = clockMs();
    const float bh = w->player ? w->player->boxheight : 6.0f;
    for (int i = 0; i < MAX_REMOTE_BODIES; ++i) {
        Peer& p = g_peers[i];
        if (!p.used) continue;
        if (now - p.lastMs > kPeerSilenceMs) { freePeer(i); continue; }
        // Dead reckoning: where the last report says they are now, then glide there.
        const float age = std::min((float)(now - p.lastMs) * 0.001f, kMaxExtrapolate);
        const Vector goal = MakeVector(p.target.x + p.vel.x * age, p.target.y + p.vel.y * age, p.target.z + p.vel.z * age);
        const float dx = goal.x - p.pos.x, dy = goal.y - p.pos.y, dz = goal.z - p.pos.z;
        if (dx * dx + dy * dy + dz * dz > kSnapDist * kSnapDist) {
            p.pos = goal;
        } else {
            const float k = std::min(1.0f, etime * 10.0f);
            p.pos.x += dx * k; p.pos.y += dy * k; p.pos.z += dz * k;
        }
        const float hs2 = p.vel.x * p.vel.x + p.vel.z * p.vel.z;
        if (hs2 > 0.25f) p.yaw = angleLerp(p.yaw, (float)(std::atan2(p.vel.z, p.vel.x) * 180.0 / M_PI), std::min(1.0f, etime * 12.0f));
        p.motion = std::fabs(p.vel.y) > 2.0f ? PB_JUMP : (hs2 > 0.25f ? PB_WALK : PB_IDLE);
        UpdateRemoteBody(i, avatarModel(p.type), MakeVector(p.pos.x, p.pos.y - bh * 0.5f, p.pos.z), p.yaw, p.motion, etime);
    }
    // POSVEL out: 4 Hz while anything about the player's motion changed, 1 Hz otherwise.
    Player* pl = w->player;
    if (g_link && pl && !pl->dead) {
        const float cur[6] = {pl->pos.x, pl->pos.y, pl->pos.z, pl->vel.x, pl->vel.y, pl->vel.z};
        bool changed = !g_sentOnce;
        for (int k = 0; k < 6; ++k) changed = changed || std::fabs(cur[k] - g_lastSent[k]) > 0.01f;
        const int64_t gap = now - g_lastSendMs;
        if (gap >= kPosVelIdleMs || (changed && gap >= kPosVelMovingMs)) {
            char b[160];
            std::snprintf(b, sizeof b, "POSVEL:%.2f:%.2f:%.2f:%.2f:%.2f:%.2f", cur[0], cur[1], cur[2], cur[3], cur[4], cur[5]);
            g_link->sendLine(b);
            std::memcpy(g_lastSent, cur, sizeof cur);
            g_lastSendMs = now;
            g_sentOnce = true;
            ++g_posVelSent;
        }
    }
}

void renderPeerTags() {
    World* w = World::getWorld;
    if (!active() || !w->cam) return;
    bool any = false;
    for (auto& p : g_peers) any = any || p.used;
    if (!any) return;
    Vector eye, at;
    w->cam->viewPoints(&eye, &at);
    Vector f = MakeVector(at.x - eye.x, at.y - eye.y, at.z - eye.z);
    const float fl = std::sqrt(f.x * f.x + f.y * f.y + f.z * f.z);
    if (fl < 1e-4f) return;
    f.x /= fl; f.y /= fl; f.z /= fl;
    Vector r = MakeVector(f.z, 0, -f.x);   // f x up(0,1,0)
    const float rl = std::sqrt(r.x * r.x + r.z * r.z);
    if (rl < 1e-4f) return;   // looking straight up or down
    r.x /= rl; r.z /= rl;
    const Vector u = MakeVector(r.y * f.z - r.z * f.y, r.z * f.x - r.x * f.z, r.x * f.y - r.y * f.x);   // r x f
    const float th = std::tan((eden_fov_degrees > 0.0f ? eden_fov_degrees : 80.0f) * (float)M_PI / 360.0f);
    const float bh = w->player ? w->player->boxheight : 6.0f;
    for (auto& p : g_peers) {
        if (!p.used) continue;
        const Vector d = MakeVector(p.pos.x - eye.x, p.pos.y + bh * 0.5f + 1.0f - eye.y, p.pos.z - eye.z);
        const float zc = d.x * f.x + d.y * f.y + d.z * f.z;
        if (zc < 0.5f || zc > kTagRange) continue;
        const float nx = (d.x * r.x + d.y * r.y + d.z * r.z) / (zc * th * P_ASPECT_RATIO);
        const float ny = (d.x * u.x + d.y * u.y + d.z * u.z) / (zc * th);
        if (std::fabs(nx) > 1.1f || std::fabs(ny) > 1.1f) continue;
        if (!p.haveTag) { p.tag.set(p.name.c_str(), GLW::du(18), UITextAlignmentCenter, 0, GLW::FACE_DISPLAY); p.haveTag = true; }
        const float sx = (nx * 0.5f + 0.5f) * SCREEN_WIDTH, sy = (ny * 0.5f + 0.5f) * SCREEN_HEIGHT;
        const float tw = p.tag.width(), lh = p.tag.lineHeight(), pad = GLW::du(5);
        const float a = zc > kTagRange * 0.7f ? 0.55f : 0.85f;
        GLW::fill(sx - tw * 0.5f - pad, sy - lh * 0.5f - pad * 0.5f, tw + 2 * pad, lh + pad, GLW::Color{0.17f, 0.17f, 0.17f, a});
        p.tag.drawCentered(sx, sy + lh * 0.5f, GLW::kWhite);
    }
}

int peerCount() {
    int n = 0;
    for (auto& p : g_peers) n += p.used ? 1 : 0;
    return n;
}

bool peerView(int i, PeerView* o) {
    int k = 0;
    for (int s = 0; s < MAX_REMOTE_BODIES; ++s) {
        if (!g_peers[s].used) continue;
        if (k++ != i) continue;
        const Peer& p = g_peers[s];
        std::snprintf(o->name, sizeof o->name, "%s", p.name.c_str());
        o->type = p.type; o->motion = p.motion; o->yaw = p.yaw; o->slot = s;
        o->x = p.pos.x; o->y = p.pos.y; o->z = p.pos.z;
        o->tx = p.target.x; o->ty = p.target.y; o->tz = p.target.z;
        return true;
    }
    return false;
}

int posVelSent() { return g_posVelSent; }

void update(float etime) {
    World* w = World::getWorld;
    if (!active() || w->doneLoading != 0 || !w->terrain || !w->terrain->loaded || w->game_mode != GAME_MODE_PLAY) return;
    ChatFeed::update(etime);   // MP.7: the HUD feed's ages
    peersUpdate(etime);   // MP.5: before the drain; the world events never touch a peer
    Budget b(kPlayMs, kPlayChunks);
    drain(b);
    if (!g_q.empty() && clockMs() - g_q.front().t > 2000 && w->hud && w->hud->sb)
        w->hud->sb->setStatus(@"Syncing...", 0.5f);
}

void startRegions(bool caps) {
    g_pacer = Pacer();
    g_pacer.on = true;
    g_pacer.legacy = !caps;
    g_pacer.startedAt = clockMs();
    pacerTick();
}

bool joinReady() {
    const Pacer& p = g_pacer;
    if (!active() || !p.on) return false;
    if (p.legacy) return p.legacyBurstApplied || (g_q.empty() && !p.burstOpen && clockMs() - p.startedAt > kUnansweredMs);
    return p.firstReplyApplied || (p.gaveUp && g_q.empty());
}

void serverSpawn(float x, float y, float z) {
    World* w = World::getWorld;
    setSpawn(x, y, z);
    if (w->terrain && w->terrain->loaded && w->game_mode == GAME_MODE_PLAY) w->terrain->warpToPoint(x, z, y);
    Pacer& p = g_pacer;
    p.havePoint = true;
    p.px = latticeOf(x);
    p.pz = latticeOf(z);
    p.want = true;
    p.asks = 0;
    p.gaveUp = false;
}

void reaskRegion() {
    g_pacer.want = true;
    g_pacer.asks = 0;
    g_pacer.gaveUp = false;
}

void noteLocalEdit(int x, int y, int z) {
    if (cellInRange(x, y, z)) g_localEdits[cellKey(x, y, z)] = clockMs();
}

// ==== MP.4: edits out (engine plan §8.1, E9, E10; signs §10) ======================================

namespace {

// The cells one intent may change, as they were before the engine call: the target and y +- 1.
struct Watch { int x, y, z, t, c; };
struct Intent {
    Watch w[3];
    int n = 0;
    Intent(int x, int z, int y) {
        Terrain* ter = World::getWorld->terrain;
        for (int yy = y - 1; yy <= y + 1; ++yy) {
            if (yy < 0 || yy >= T_HEIGHT) continue;
            const int t = ter->getLand(x, z, yy);
            if (t < 0) continue;   // not resident
            w[n++] = {x, yy, z, t, ter->getColor(x, z, yy)};
        }
    }
    // One line per cell that changed: went air -> mine; another type -> build (+ its paint);
    // the same type, another colour -> paint. Paint left on air (destroyBlock keeps it for TNT
    // colour) is not an edit.
    void send() {
        Terrain* ter = World::getWorld->terrain;
        for (int i = 0; i < n; ++i) {
            const Watch& o = w[i];
            const int t = ter->getLand(o.x, o.z, o.y), c = ter->getColor(o.x, o.z, o.y);
            if (t < 0 || (t == o.t && c == o.c) || (t == TYPE_NONE && o.t == TYPE_NONE)) continue;
            char l[64];
            if (t == TYPE_NONE) {
                std::snprintf(l, sizeof l, "ACTION:%d:%d:%d:1", o.x, o.y, o.z);
                sendLine(l);
                g_stats.actionsSent++;
            } else {
                if (t != o.t) {
                    std::snprintf(l, sizeof l, "ACTION:%d:%d:%d:0:%d", o.x, o.y, o.z, t);
                    sendLine(l);
                    g_stats.actionsSent++;
                }
                if (t != o.t ? c != 0 : c != o.c) {
                    std::snprintf(l, sizeof l, "ACTION:%d:%d:%d:3:%d", o.x, o.y, o.z, c);
                    sendLine(l);
                    g_stats.actionsSent++;
                }
            }
            noteLocalEdit(o.x, o.y, o.z);
        }
    }
};

}  // namespace

void intentBuild(int x, int z, int y) {
    if (!active()) { World::getWorld->terrain->buildBlock(x, z, y); return; }
    Intent in(x, z, y);
    World::getWorld->terrain->buildBlock(x, z, y);
    in.send();
}

void intentMine(int x, int z, int y) {
    if (!active()) { World::getWorld->terrain->destroyBlock(x, z, y); return; }
    Intent in(x, z, y);
    World::getWorld->terrain->destroyBlock(x, z, y);
    in.send();
}

void intentPaint(int x, int z, int y, int color) {
    if (!active()) { World::getWorld->terrain->paintBlock(x, z, y, color); return; }
    Intent in(x, z, y);
    World::getWorld->terrain->paintBlock(x, z, y, color);
    in.send();
}

void intentBurn(int x, int z, int y) {
    Terrain* ter = World::getWorld->terrain;
    const int type = ter->getLand(x, z, y);
    ter->burnBlock(x, z, y, FALSE);
    // E10: the server deletes whatever it is told to burn; the game burns only flammables.
    if (!active() || type <= 0 || !(blockinfo[type] & IS_FLAMMABLE)) return;
    bool lit = false;
    for (BurnNode* n = burnList; n; n = n->next)
        if (n->x == x && n->y == y && n->z == z) { n->mpLit = 1; lit = true; break; }
    if (!lit) return;   // past the window edge (burnBlock refused it)
    char l[64];
    std::snprintf(l, sizeof l, "ACTION:%d:%d:%d:2", x, y, z);
    sendLine(l);
    g_stats.actionsSent++;
    noteLocalEdit(x, y, z);
}

void burnFuseOut(int x, int y, int z) {
    if (!active()) return;
    char l[64];
    std::snprintf(l, sizeof l, "ACTION:%d:%d:%d:1", x, y, z);
    sendLine(l);
    g_stats.actionsSent++;
    noteLocalEdit(x, y, z);
}

void intentSign(int fx, int fy, int fz, int a, int b, int c, const char* text) {
    if (!active()) return;
    // The wire's x:y:z has y = height; the trailer's file order has z = height (§10).
    std::string l = "SIGNP:" + std::to_string(fx) + ":" + std::to_string(fz) + ":" + std::to_string(fy) + ":" +
                    std::to_string(a) + ":" + std::to_string(b) + ":" + std::to_string(c) + ":" + (text ? text : "");
    sendLine(l.c_str());
    g_stats.signsSent++;
}

Stats stats() { return g_stats; }
void resetStats() { g_stats = Stats(); }

}  // namespace MpWorld
