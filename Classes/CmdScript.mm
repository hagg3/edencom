//
//  CmdScript.mm
//  Eden — Stage D / D.4c. See CmdScript.h.
//
#import "CmdScript.h"
#import "Globals.h"
#import "World.h"
#import "Terrain.h"
#import "TerrainGen2.h"
#import "FileManager.h"
#import "Hud.h"
#import "Util.h"
#import "Resources.h"
#import "GLDialog.h"
#import "Player.h"
#import "Liquids.h"
#import "Model.h"
#import "SignTool.h"
#include "WorldTrailer.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

void addlight(int xx, int zz, int yy, float brightness, Vector color);
extern Vector colorTable[256];
extern int fwc_result;

bool CmdScript::trace = false;
bool CmdScript::testNoDepthCap = false;

// ---- parsing ----------------------------------------------------------------------------------

namespace {

std::string lower(const std::string& s) {
    std::string r(s);
    for (char& c : r) c = (char)std::tolower((unsigned char)c);
    return r;
}
void trimInPlace(std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    s = s.substr(a, b - a);
}
std::vector<std::string> words(const std::string& s) {
    std::vector<std::string> w;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && std::isspace((unsigned char)s[i])) ++i;
        size_t j = i;
        while (j < s.size() && !std::isspace((unsigned char)s[j])) ++j;
        if (j > i) w.push_back(s.substr(i, j - i));
        i = j;
    }
    return w;
}
bool number(const std::string& t, double* out) {
    if (t.empty()) return false;
    char* end = NULL;
    const double v = std::strtod(t.c_str(), &end);
    if (!end || *end || !std::isfinite(v)) return false;
    *out = v;
    return true;
}
// A coordinate: a number, optionally after '~' ("~" alone is 0). Rounded to a cell.
bool coord(const std::string& t, int* out) {
    std::string s = t;
    if (!s.empty() && s[0] == '~') {
        s = s.substr(1);
        if (s.empty()) { *out = 0; return true; }
    }
    double v;
    if (!number(s, &v) || std::fabs(v) > 100000) return false;
    *out = (int)std::lround(v);
    return true;
}

// cmd.html's names. voidstone, sky, pillar, jungle, future, tech and lavafield are the 2026 game's
// names for blocks Emod cannot identify (no list maps them onto 112-127): unknown here, on purpose.
struct Name { const char* name; int id; };
const Name kBlocks[] = {
    {"air", TYPE_NONE}, {"stone", TYPE_STONE}, {"dirt", TYPE_DIRT}, {"grass", TYPE_GRASS},
    {"sand", TYPE_SAND}, {"wood", TYPE_WOOD}, {"tree", TYPE_TREE}, {"leaves", TYPE_LEAVES},
    {"glass", TYPE_GLASS}, {"water", TYPE_WATER}, {"lava", TYPE_LAVA}, {"tnt", TYPE_TNT},
    {"lamp", TYPE_LIGHTBOX}, {"brick", TYPE_BRICK}, {"cobble", TYPE_COBBLESTONE}, {"ice", TYPE_ICE},
    {"snow", TYPE_SNOW}, {"crystal", TYPE_CRYSTAL}, {"trampoline", TYPE_TRAMPOLINE},
    {"ladder", TYPE_LADDER}, {"cloud", TYPE_CLOUD}, {"weave", TYPE_WEAVE}, {"vine", TYPE_VINE},
    {"shingle", TYPE_SHINGLE}, {"gradient", TYPE_GRADIENT}, {"firework", TYPE_FIREWORK},
    {"gold", TYPE_GOLDEN_CUBE}, {"steel", TYPE_STEEL}, {"darkstone", TYPE_DARK_STONE},
    {"flower", TYPE_FLOWER},
    // Emod's own names for the 2026 blocks (docs/world-and-terrain.md), so they can be named at all.
    {"oresand", TYPE_ORE_SAND}, {"spacestone", TYPE_SPACE_STONE}, {"carpet", TYPE_CARPET},
    {"snakeskin", TYPE_SNAKESKIN}, {"obsidian", TYPE_OBSIDIAN}, {"cheese", TYPE_CHEESE},
    {"spacedirt", TYPE_SPACE_DIRT}, {"spacegrass", TYPE_SPACE_GRASS}, {"moss", TYPE_MOSS},
    {"darkmatter", TYPE_DARK_MATTER}, {"spacesand", TYPE_SPACE_SAND}, {"moonrock", TYPE_MOONROCK},
    {"basalt", TYPE_BASALT}, {"darktile", TYPE_DARK_TILE}, {"algae", TYPE_ALGAE},
};
const char* const kCreatures[NUM_CREATURES] = {"moof", "batty", "green", "nergle", "stumpy", "charger", "stalker"};

bool bad(CmdScript::Stmt* s, const std::string& msg) {
    s->op = CmdScript::OP_BAD;
    s->text = msg;
    return false;
}
bool coords(const std::vector<std::string>& w, size_t from, int n, CmdScript::Stmt* s, const char* usage) {
    if (w.size() < from + n) return bad(s, std::string(usage));
    for (int i = 0; i < n; ++i)
        if (!coord(w[from + i], &s->v[i])) return bad(s, "Not a coordinate: " + w[from + i]);
    return true;
}
bool block(const std::string& t, CmdScript::Stmt* s) {
    s->type = CmdScript::blockByName(t.c_str());
    if (s->type < 0) return bad(s, "Unknown block: " + t);
    return true;
}
bool colour(const std::string& t, CmdScript::Stmt* s) {
    double v;
    if (!number(t, &v) || v != std::floor(v) || v < 0 || v > 54) return bad(s, "Colour must be 0-54: " + t);
    s->color = (int)v;
    return true;
}

// One statement (not a header). Returns false when it is OP_BAD.
bool parseStmt(const std::string& src, CmdScript::Stmt* s) {
    using CS = CmdScript;
    s->op = CS::OP_BAD;
    std::memset(s->v, 0, sizeof(s->v));
    s->type = 0;
    s->color = -1;
    s->hasPos = false;
    s->f = 0;
    s->text.clear();
    const std::vector<std::string> w = words(src);
    const std::string c = lower(w[0]);
    const size_t n = w.size();
    double v;
    if (c == "say") {
        s->op = CS::OP_SAY;
        s->text = src.substr(src.find_first_of(" \t") == std::string::npos ? src.size() : src.find_first_of(" \t"));
        trimInPlace(s->text);
        return true;
    }
    if (c == "tp") {
        if (!coords(w, 1, 3, s, "tp needs X Y Z")) return false;
        s->op = CS::OP_TP;
        return true;
    }
    if (c == "set") {
        if (n < 5) return bad(s, "set needs X Y Z TYPE [COLOR]");
        if (!coords(w, 1, 3, s, "")) return false;
        if (!block(w[4], s) || (n > 5 && !colour(w[5], s))) return false;
        s->op = CS::OP_SET;
        return true;
    }
    if (c == "fill") {
        if (n < 8) return bad(s, "fill needs X Y Z X2 Y2 Z2 TYPE [COLOR]");
        if (!coords(w, 1, 6, s, "")) return false;
        if (!block(w[7], s) || (n > 8 && !colour(w[8], s))) return false;
        const long cells = (long)(std::abs(s->v[3] - s->v[0]) + 1) * (std::abs(s->v[4] - s->v[1]) + 1) *
                           (std::abs(s->v[5] - s->v[2]) + 1);
        if (cells > CS::MAX_FILL) return bad(s, "fill is over 4096 blocks");
        s->op = CS::OP_FILL;
        return true;
    }
    if (c == "paint") {
        if (n < 5) return bad(s, "paint needs X Y Z COLOR");
        if (!coords(w, 1, 3, s, "")) return false;
        if (!colour(w[4], s)) return false;
        s->op = CS::OP_PAINT;
        return true;
    }
    if (c == "explode" || c == "fire") {
        if (!coords(w, 1, 3, s, c == "fire" ? "fire needs X Y Z" : "explode needs X Y Z")) return false;
        s->op = c == "fire" ? CS::OP_FIRE : CS::OP_EXPLODE;
        return true;
    }
    if (c == "spawn") {
        if (n < 2) return bad(s, "spawn needs a creature");
        s->type = CmdScript::creatureByName(w[1].c_str());
        if (s->type < 0) return bad(s, "Unknown creature: " + w[1]);
        if (n > 2) {
            if (!coords(w, 2, 3, s, "spawn needs CREATURE [X Y Z]")) return false;
            s->hasPos = true;
        }
        s->op = CS::OP_SPAWN;
        return true;
    }
    if (c == "day" || c == "noon" || c == "night" || c == "midnight") {
        s->op = CS::OP_TIME;
        s->f = c == "day" ? 0.25f : c == "noon" ? 0.5f : c == "night" ? 0.75f : 0.0f;
        return true;
    }
    if (c == "time") {
        if (n < 2 || !number(w[1], &v) || v < 0 || v > 1) return bad(s, "time needs T from 0 to 1");
        s->op = CS::OP_TIME;
        s->f = (float)v;
        return true;
    }
    if (c == "heal") { s->op = CS::OP_HEAL; return true; }
    if (c == "flash") { s->op = CS::OP_FLASH; return true; }
    if (c == "hurt" || c == "gold" || c == "sound" || c == "wait") {
        if (n < 2 || !number(w[1], &v) || v < 0) return bad(s, c + " needs a number");
        if (c == "hurt") { s->op = CS::OP_HURT; s->f = (float)std::min(v, 100.0); }
        else if (c == "gold") { s->op = CS::OP_GOLD; s->f = (float)std::min(std::floor(v), (double)CS::MAX_GOLD); }
        else if (c == "wait") { s->op = CS::OP_WAIT; s->f = (float)std::min(v, (double)CS::MAX_WAIT); }
        else {
            if (v != std::floor(v) || v >= NUM_SOUNDS) return bad(s, "No such sound: " + w[1]);
            s->op = CS::OP_SOUND;
            s->f = (float)v;
        }
        return true;
    }
    if (c == "run") {
        if (!coords(w, 1, 3, s, "run needs X Y Z")) return false;
        s->op = CS::OP_RUN;
        return true;
    }
    if (c == "if" || c == "unless") {
        if (n < 5) return bad(s, c + " needs X Y Z TYPE");
        if (!coords(w, 1, 3, s, "")) return false;
        if (!block(w[4], s)) return false;
        s->op = c == "if" ? CS::OP_IF : CS::OP_UNLESS;
        return true;
    }
    if (c[0] == '@') return bad(s, "A trigger must be the first statement: " + w[0]);
    return bad(s, "Unknown command: " + w[0]);
}

}  // namespace

int CmdScript::blockByName(const char* name) {
    const std::string t = lower(name ? name : "");
    if (t.empty()) return -1;
    double v;
    if (number(t, &v)) return (v == std::floor(v) && v >= 0 && v <= 127) ? (int)v : -1;
    for (const Name& b : kBlocks) if (t == b.name) return b.id;
    return -1;
}

int CmdScript::creatureByName(const char* name) {
    const std::string t = lower(name ? name : "");
    double v;
    if (number(t, &v)) return (v == std::floor(v) && v >= 0 && v < NUM_CREATURES) ? (int)v : -1;
    for (int i = 0; i < NUM_CREATURES; ++i) if (t == kCreatures[i]) return i;
    return -1;
}

void CmdScript::parse(const char* src, Program* out) {
    *out = Program();
    std::string all(src ? src : "");
    size_t at = 0;
    bool first = true;
    while (at <= all.size()) {
        size_t semi = all.find(';', at);
        if (semi == std::string::npos) semi = all.size();
        std::string piece = all.substr(at, semi - at);
        at = semi + 1;
        trimInPlace(piece);
        if (piece.empty()) continue;
        const std::vector<std::string> w = words(piece);
        const std::string c = lower(w[0]);
        if (first && c[0] == '@') {
            first = false;
            double v = 0;
            Stmt s;
            if (c == "@touch") { out->trigger = TRIG_TOUCH; continue; }
            if (c == "@step") { out->trigger = TRIG_STEP; continue; }
            if ((c == "@near" || c == "@timer") && w.size() >= 2 && number(w[1], &v) && v > 0) {
                out->trigger = c == "@near" ? TRIG_NEAR : TRIG_TIMER;
                // A timer faster than 20 Hz is a timer at 20 Hz (the engine's longest frame).
                out->triggerArg = c == "@near" ? (float)std::min(v, (double)TIMER_RANGE) : (float)std::max(v, 0.05);
                continue;
            }
            bad(&s, (c == "@near" || c == "@timer") ? c + " needs a number above 0" : "Unknown trigger: " + w[0]);
            out->stmts.push_back(s);
            out->errors++;
            if (out->firstError.empty()) out->firstError = s.text;
            continue;
        }
        first = false;
        Stmt s;
        if (!parseStmt(piece, &s)) {
            out->errors++;
            if (out->firstError.empty()) out->firstError = s.text;
        }
        out->stmts.push_back(s);
    }
}

// ---- runtime ----------------------------------------------------------------------------------

namespace {

uint64_t key(const TrailerPos& p) {
    return ((uint64_t)(uint32_t)p.x << 40) ^ ((uint64_t)(uint32_t)p.y << 16) ^ (uint64_t)(uint32_t)p.z;
}

struct Cached { std::string src; std::shared_ptr<const CmdScript::Program> prog; };
struct Inst {
    TrailerPos block;
    std::shared_ptr<const CmdScript::Program> prog;
    size_t pc;
    float wait;
    int depth;
    bool live;
};
struct TrigState { bool armed; float acc; };

std::unordered_map<uint64_t, Cached> g_cache;
std::unordered_map<uint64_t, TrigState> g_trig;
struct Green { TrailerPos pos; double until; };
std::unordered_map<uint64_t, Green> g_greenUntil;     // the button stays green at least this long
std::unordered_map<uint64_t, TrailerPos> g_green;      // what CmdTool::setRunning was last told
std::unordered_map<uint64_t, int> g_liveOn;            // live instances per block
std::vector<Inst*> g_insts;
CmdScript::Stats g_stats;
double g_now = 0;
int g_work = 0;
float g_toastQuiet = 0;   // refusal toasts at most once a second
float g_daySky = -1;      // the region's sky before a script made it night

const float kGreenMin = 0.25f;

void toast(const std::string& msg, bool rateLimited) {
    if (rateLimited) {
        if (g_toastQuiet > 0) return;
        g_toastQuiet = 1.0f;
    }
    World::getWorld->hud->sb->setStatus([NSString stringWithUTF8String:msg.c_str()], 3);
}

std::shared_ptr<const CmdScript::Program> programFor(const TrailerPos& p, int* index) {
    WorldTrailer* t = World::getWorld->fm->trailer();
    const int i = t->findCmd(p);
    if (index) *index = i;
    if (i < 0) return std::shared_ptr<const CmdScript::Program>();
    const TrailerCmd& rec = t->cmd((size_t)i);
    const size_t len = strnlen(rec.script, sizeof(rec.script));
    Cached& c = g_cache[key(p)];
    if (!c.prog || c.src.size() != len || std::memcmp(c.src.data(), rec.script, len) != 0) {
        c.src.assign(rec.script, len);
        std::shared_ptr<CmdScript::Program> pr = std::make_shared<CmdScript::Program>();
        CmdScript::parse(c.src.c_str(), pr.get());
        c.prog = pr;
    }
    return c.prog;
}

// The cell is in the resident window (its chunk's bounds say this very spot).
bool resident(int ex, int ey, int ez) {
    if (ey < 0 || ey >= T_HEIGHT) return false;
    Terrain* t = World::getWorld->terrain;
    const int cx = (int)std::floor((double)ex / CHUNK_SIZE), cy = ey / CHUNK_SIZE,
              cz = (int)std::floor((double)ez / CHUNK_SIZE);
    TerrainChunk* c = t->chunkTable[threeToOne(cx, cy, cz)];
    return c && c->pbounds[0] == cx * CHUNK_SIZE && c->pbounds[1] == cy * CHUNK_SIZE &&
           c->pbounds[2] == cz * CHUNK_SIZE;
}

bool inPlayer(int ex, int ey, int ez) {
    Player* p = World::getWorld->player;
    const float e = 0.01f;
    return p->pos.x + p->boxbase / 2 > ex + e && p->pos.x - p->boxbase / 2 < ex + 1 - e &&
           p->pos.z + p->boxbase / 2 > ez + e && p->pos.z - p->boxbase / 2 < ez + 1 - e &&
           p->pos.y + p->boxheight / 2 > ey + e && p->pos.y - p->boxheight / 2 < ey + 1 - e;
}

bool cmdAt(int ex, int ey, int ez) {
    WorldTrailer* t = World::getWorld->fm->trailer();
    return t->cmdCount() && t->findCmd(TrailerPos::fromEngine(ex, ey, ez)) >= 0;
}

// One cell of a set/fill: engine order (x, y_up, z). Returns whether it was written.
bool setCell(int ex, int ey, int ez, int type, int color) {
    Terrain* t = World::getWorld->terrain;
    g_work++;
    if (!resident(ex, ey, ez)) { g_stats.refusedCells++; return false; }
    const int cur = t->getLand(ex, ez, ey);
    if (cur < 0) { g_stats.refusedCells++; return false; }
    const int paint = color < 0 ? 0 : color;
    if (cur == type && (type == TYPE_NONE || t->getColor(ex, ez, ey) == paint)) return true;   // already so
    if (cur == TYPE_BEDROCK || (blockinfo[cur] & (IS_DOOR | IS_PORTAL))) { g_stats.refusedCells++; return false; }
    if (type != TYPE_NONE && (inPlayer(ex, ey, ez) || cmdAt(ex, ey, ez))) { g_stats.refusedCells++; return false; }
    if (blockinfo[cur] & IS_LIQUID) t->liquids->removeSource(ex, ez, ey, cur);
    if (cur == TYPE_LIGHTBOX) addlight(ex, ez, ey, -1.0f, colorTable[t->getColor(ex, ez, ey) & 255]);
    if (type == TYPE_LIGHTBOX) addlight(ex, ez, ey, 1.0f, colorTable[paint & 255]);
    t->updateChunks(ex, ez, ey, type);                    // air also drops what hangs on the block
    if (type != TYPE_NONE) t->setColor(ex, ez, ey, (color8)paint);
    if (type == TYPE_WATER || type == TYPE_LAVA) t->liquids->addSource(ex, ez, ey);
    if (cur == TYPE_LIGHTBOX || type == TYPE_LIGHTBOX) t->refreshChunksInRadius(ex, ez, ey, LIGHT_RADIUS);
    g_stats.cells++;
    return true;
}

bool placeable(int type) {
    return type >= 0 && type < NUM_BLOCKS && !(blockinfo[type] & (IS_DOOR | IS_PORTAL)) && type != TYPE_CUSTOM;
}

void setSky(float t) {
    Terrain* ter = World::getWorld->terrain;
    const bool night = t < 0.25f || t >= 0.75f;
    const bool isNight = v_equals(ter->final_skycolor, colorTable[54]);
    if (night == isNight) return;
    if (night) {
        // Remember the day sky this region had, to give it back on `day`.
        for (int c = 0; c < 256; ++c)
            if (v_equals(ter->final_skycolor, colorTable[c])) { g_daySky = (float)c; break; }
        paintSky(54);
    } else {
        paintSky(g_daySky >= 0 && (int)g_daySky != 54 ? (int)g_daySky : COLOR_NORMAL_BLUE);
    }
}

void execute(Inst* in);

// Start the block's script at `depth`: run it to its first wait. Returns PRESS_*.
int start(const TrailerPos& block, int depth) {
    if (CmdScript::liveCount() >= CmdScript::MAX_RUNNING) {
        g_stats.refusedFull++;
        toast("Too many command blocks running (32)", true);
        return CmdScript::PRESS_REFUSED;
    }
    std::shared_ptr<const CmdScript::Program> prog = programFor(block, NULL);
    if (!prog) return CmdScript::PRESS_NONE;
    Inst* in = new Inst();
    in->block = block;
    in->prog = prog;
    in->pc = 0;
    in->wait = 0;
    in->depth = depth;
    in->live = true;
    g_insts.push_back(in);
    g_liveOn[key(block)]++;
    g_greenUntil[key(block)] = Green{block, g_now + kGreenMin};
    g_stats.started++;
    if (depth > g_stats.maxDepth) g_stats.maxDepth = depth;
    execute(in);
    return CmdScript::PRESS_STARTED;
}

void finish(Inst* in) {
    in->live = false;
    auto it = g_liveOn.find(key(in->block));
    if (it != g_liveOn.end() && --it->second <= 0) g_liveOn.erase(it);
    g_stats.finished++;
}

// The index after statement i, a gated statement included: skipping an `if` skips what it gates.
size_t skipFrom(const std::vector<CmdScript::Stmt>& s, size_t i) {
    while (i < s.size() && (s[i].op == CmdScript::OP_IF || s[i].op == CmdScript::OP_UNLESS)) ++i;
    return i < s.size() ? i + 1 : i;
}

void execute(Inst* in) {
    using CS = CmdScript;
    World* w = World::getWorld;
    Terrain* ter = w->terrain;
    Player* pl = w->player;
    const std::vector<CS::Stmt>& st = in->prog->stmts;
    int bx, by, bz;   // the owner, engine order
    in->block.toEngine(&bx, &by, &bz);
    while (in->live && in->pc < st.size()) {
        if (g_work >= CS::WORK_PER_FRAME) { in->wait = 0; g_stats.suspended++; return; }
        const CS::Stmt& s = st[in->pc];
        g_work++;
        g_stats.statements++;
        const int x = bx + s.v[0], y = by + s.v[1], z = bz + s.v[2];
        if (CS::trace)
            std::printf("[cmd] %d,%d,%d d%d #%zu op%d %d %d %d t%d c%d %s\n", in->block.x, in->block.y, in->block.z,
                        in->depth, in->pc, s.op, s.v[0], s.v[1], s.v[2], s.type, s.color, s.text.c_str());
        switch (s.op) {
        case CS::OP_IF:
        case CS::OP_UNLESS: {
            const bool holds = resident(x, y, z) && ter->getLand(x, z, y) == s.type;
            in->pc = (holds == (s.op == CS::OP_IF)) ? in->pc + 1 : skipFrom(st, in->pc + 1);
            continue;
        }
        case CS::OP_WAIT:
            in->pc++;
            in->wait = s.f;
            return;   // always yields, at least to the next frame
        case CS::OP_RUN:
            in->pc++;
            if (in->depth + 1 > CS::MAX_DEPTH && !CS::testNoDepthCap) {
                g_stats.refusedDepth++;
                toast("Command blocks can chain 8 deep", true);
            } else {
                start(TrailerPos::fromEngine(x, y, z), in->depth + 1);
            }
            continue;
        case CS::OP_BAD:
            g_stats.unknown++;
            toast(s.text, false);
            break;
        case CS::OP_SAY:
            w->hud->sb->setStatus([NSString stringWithUTF8String:s.text.c_str()], 3);
            break;
        case CS::OP_TP:
            if (y >= 0 && y < T_HEIGHT + 32) {
                pl->pos = MakeVector(x + 0.5f, y + pl->boxheight / 2 + 0.001f, z + 0.5f);
                pl->vel = MakeVector(0, 0, 0);
            }
            break;
        case CS::OP_SET:
            if (placeable(s.type)) setCell(x, y, z, s.type, s.color);
            else g_stats.refusedCells++;
            break;
        case CS::OP_FILL: {
            if (!placeable(s.type)) { g_stats.refusedCells++; break; }
            const int x2 = bx + s.v[3], y2 = by + s.v[4], z2 = bz + s.v[5];
            for (int a = std::min(x, x2); a <= std::max(x, x2); ++a)
                for (int b = std::min(y, y2); b <= std::max(y, y2); ++b)
                    for (int c = std::min(z, z2); c <= std::max(z, z2); ++c)
                        setCell(a, b, c, s.type, s.color);
            break;
        }
        case CS::OP_PAINT:
            if (resident(x, y, z) && ter->getLand(x, z, y) > 0) ter->paintBlock(x, z, y, s.color);
            break;
        case CS::OP_EXPLODE:
            if (resident(x, y, z)) {
                const int cur = ter->getLand(x, z, y);
                if (cur >= 0 && cur != TYPE_BEDROCK && cur != TYPE_GOLDEN_CUBE && !(blockinfo[cur] & (IS_DOOR | IS_PORTAL))) {
                    if (cur != TYPE_TNT) {
                        if (blockinfo[cur] & IS_LIQUID) ter->liquids->removeSource(x, z, y, cur);
                        if (cur == TYPE_LIGHTBOX) {
                            addlight(x, z, y, -1.0f, colorTable[ter->getColor(x, z, y) & 255]);
                            ter->refreshChunksInRadius(x, z, y, LIGHT_RADIUS);
                        }
                        ter->updateChunks(x, z, y, TYPE_TNT);   // a command block keeps its record until the blast
                        ter->setColor(x, z, y, 0);
                    }
                    ter->burnBlock(x, z, y, FALSE);            // lit: TNT's own 4 s fuse
                }
            }
            break;
        case CS::OP_FIRE:
            if (resident(x, y, z)) ter->burnBlock(x, z, y, FALSE);   // only if it burns
            break;
        case CS::OP_SPAWN: {
            const int sx = s.hasPos ? x : bx, sy = s.hasPos ? y : by + 1, sz = s.hasPos ? z : bz;
            if (CREATURES_ON && resident(sx, std::max(0, std::min(sy, T_HEIGHT - 1)), sz))
                SpawnCreatureAt(s.type, MakeVector(sx + 0.5f, sy + 0.5f, sz + 0.5f));
            break;
        }
        case CS::OP_TIME:
            setSky(s.f);
            break;
        case CS::OP_HEAL:
            pl->life = 1;
            break;
        case CS::OP_HURT:
            if (pl->health_option && !pl->dead && s.f > 0) pl->takeDamage(s.f / 100.0f);
            break;
        case CS::OP_GOLD:
            w->hud->goldencubes = std::min((int)CS::MAX_GOLD, w->hud->goldencubes + (int)s.f);
            if (s.f > 0) Resources::getResources->playSound(S_TREASURE_PICKUP);
            break;
        case CS::OP_SOUND:
            Resources::getResources->playSound((int)s.f);
            break;
        case CS::OP_FLASH:
            w->hud->flash = 1.0f;
            w->hud->flashcolor = MakeVector(1.0f, 1.0f, 1.0f);
            break;
        }
        in->pc++;
    }
    if (in->live && in->pc >= st.size()) finish(in);
}

void syncGreen() {
    WorldTrailer* t = World::getWorld->fm->trailer();
    std::unordered_map<uint64_t, TrailerPos> want;
    for (const Inst* in : g_insts)
        if (in->live) want[key(in->block)] = in->block;
    // A block still inside its minimum green time stays green with no live instance.
    for (auto it = g_greenUntil.begin(); it != g_greenUntil.end();) {
        if (it->second.until <= g_now) { it = g_greenUntil.erase(it); continue; }
        want[it->first] = it->second.pos;
        ++it;
    }
    for (const auto& kv : g_green)
        if (!want.count(kv.first)) CmdTool::setRunning(kv.second, false);
    for (const auto& kv : want)
        if (t->findCmd(kv.second) >= 0) CmdTool::setRunning(kv.second, true);
    g_green.swap(want);
}

}  // namespace

int CmdScript::liveCount() {
    int n = 0;
    for (const Inst* in : g_insts) if (in->live) ++n;
    return n;
}

bool CmdScript::busy(const TrailerPos& block) { return g_liveOn.count(key(block)) > 0; }

int CmdScript::press(const TrailerPos& block) {
    World* w = World::getWorld;
    if (!w || !w->fm || w->fm->trailer()->findCmd(block) < 0) return PRESS_NONE;
    g_stats.presses++;
    if (busy(block)) return PRESS_BUSY;
    Resources::getResources->playSound(S_DOOR_CLOSED);   // the button clunks in (the 2026 game's sound)
    const int r = start(block, 0);
    syncGreen();
    return r;
}

bool CmdScript::tap(int mx, int my) {
    World* w = World::getWorld;
    if (GLDialog::active()) return false;
    WorldTrailer* t = w->fm->trailer();
    if (!t->cmdCount()) return false;
    const Point3D hit = findWorldCoords(my, mx, FC_DESTROY);
    if (hit.x == -1 || fwc_result != -1) return false;   // nothing, or a creature in front
    if (w->terrain->getLand(hit.x, hit.z, hit.y) <= 0) return false;
    const TrailerPos pos = TrailerPos::fromTerrainArgs(hit.x, hit.z, hit.y);
    if (t->findCmd(pos) < 0) return false;
    press(pos);
    return true;   // a tap on a command block pushes its button and never builds
}

void CmdScript::update(float etime) {
    World* w = World::getWorld;
    if (!w || !w->terrain || !w->terrain->loaded || !w->fm) return;
    WorldTrailer* t = w->fm->trailer();
    g_now += etime;
    g_work = 0;
    if (g_toastQuiet > 0) g_toastQuiet -= etime;

    // 1. Resume what was waiting (snapshot: anything started below waits for the next frame).
    const std::vector<Inst*> snap = g_insts;
    for (Inst* in : snap) {
        if (!in->live) continue;
        in->wait -= etime;
        if (in->wait <= 0) execute(in);
    }

    // 2. Triggers. Only scripts that start with '@' can have one; the rest are never parsed here.
    if (t->cmdCount()) {
        Player* p = w->player;
        const float feet = p->pos.y - p->boxheight / 2;
        for (size_t i = 0; i < t->cmdCount(); ++i) {
            const TrailerCmd& rec = t->cmd(i);
            size_t k = 0;
            while (k < sizeof(rec.script) && rec.script[k] == ' ') ++k;
            if (k >= sizeof(rec.script) || rec.script[k] != '@') continue;
            const TrailerPos pos = rec.pos();
            std::shared_ptr<const Program> prog = programFor(pos, NULL);
            if (!prog || prog->trigger == TRIG_TOUCH) continue;
            int ex, ey, ez;
            pos.toEngine(&ex, &ey, &ez);
            if (!resident(ex, ey, ez) || w->terrain->getLand(ex, ez, ey) <= 0) continue;
            const float dx = ex + 0.5f - p->pos.x, dy = ey + 0.5f - p->pos.y, dz = ez + 0.5f - p->pos.z;
            const float d = sqrtf(dx * dx + dy * dy + dz * dz);
            TrigState& ts = g_trig.emplace(key(pos), TrigState{true, 0}).first->second;
            bool fire = false;
            if (prog->trigger == TRIG_TIMER) {
                if (d <= TIMER_RANGE) {
                    ts.acc += etime;
                    if (ts.acc >= prog->triggerArg) {
                        ts.acc -= prog->triggerArg;
                        if (ts.acc >= prog->triggerArg) ts.acc = 0;   // never a burst to catch up
                        fire = true;
                    }
                } else {
                    ts.acc = 0;
                }
            } else {
                const bool on = prog->trigger == TRIG_NEAR
                    ? d <= prog->triggerArg
                    : ((int)floorf(p->pos.x) == ex && (int)floorf(p->pos.z) == ez && fabsf(feet - (ey + 1)) < 0.25f);
                if (on && ts.armed) { fire = true; ts.armed = false; }
                else if (!on) ts.armed = true;
            }
            if (fire && !busy(pos)) {
                g_stats.triggers++;
                start(pos, 0);
            }
        }
    }

    // 3. Drop what finished; tell the skin which buttons are green.
    for (size_t i = 0; i < g_insts.size();) {
        if (!g_insts[i]->live) { delete g_insts[i]; g_insts.erase(g_insts.begin() + (long)i); }
        else ++i;
    }
    syncGreen();
    g_stats.live = (int)g_insts.size();
}

void CmdScript::reset() {
    for (Inst* in : g_insts) delete in;
    g_insts.clear();
    g_cache.clear();
    g_trig.clear();
    g_greenUntil.clear();
    g_green.clear();
    g_liveOn.clear();
    g_daySky = -1;
    CmdTool::clearRunning();
    g_stats.live = 0;
}

CmdScript::Stats CmdScript::stats() {
    g_stats.live = liveCount();
    return g_stats;
}

const char* CmdScript::describe() {
    static char buf[320];
    const Stats s = stats();
    std::snprintf(buf, sizeof(buf),
                  "{\"live\":%d,\"started\":%d,\"finished\":%d,\"statements\":%d,\"cells\":%d,\"unknown\":%d,"
                  "\"refusedDepth\":%d,\"refusedFull\":%d,\"refusedCells\":%d,\"triggers\":%d,\"presses\":%d,"
                  "\"suspended\":%d,\"maxDepth\":%d}",
                  s.live, s.started, s.finished, s.statements, s.cells, s.unknown, s.refusedDepth, s.refusedFull,
                  s.refusedCells, s.triggers, s.presses, s.suspended, s.maxDepth);
    return buf;
}
