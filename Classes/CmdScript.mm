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
bool CmdScript::testAnchorFeet = false;

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
// A coordinate: a number, optionally after '~' ("~" alone is 0). Rounded to a cell. With `absOK`
// (the console), `=N` is the engine coordinate N and sets *isAbs; a block never gets here with it,
// so `=N` stays "Not a coordinate" there.
bool coord(const std::string& t, int* out, bool* isAbs, bool absOK) {
    std::string s = t;
    *isAbs = false;
    if (absOK && !s.empty() && s[0] == '=') {
        double v;
        if (!number(s.substr(1), &v) || std::fabs(v) > 10000000) return false;
        *out = (int)std::lround(v);
        *isAbs = true;
        return true;
    }
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

// The built-in verbs (CmdScript::verbs). The wording of every "X needs ..." error comes from here.
#define C_ CmdScript::ARG_COORD
const CmdScript::Verb kVerbs[] = {
    {"say", "TEXT", NULL, "Shows TEXT on screen as a status message.", {CmdScript::ARG_TEXT}},
    {"tp", "X Y Z", NULL, "Teleports the player, feet landing at that cell.", {C_, C_, C_}},
    {"set", "X Y Z TYPE [COLOR]", NULL, "Places or replaces one block; air removes it. Optional paint colour.",
     {C_, C_, C_, CmdScript::ARG_BLOCK, CmdScript::ARG_COLOR}},
    {"fill", "X Y Z X2 Y2 Z2 TYPE [COLOR]", NULL, "Fills the box between the two corners, up to 4096 blocks.",
     {C_, C_, C_, C_, C_, C_, CmdScript::ARG_BLOCK, CmdScript::ARG_COLOR}},
    {"paint", "X Y Z COLOR", NULL, "Repaints an existing block (colour 0-54).", {C_, C_, C_, CmdScript::ARG_COLOR}},
    {"explode", "X Y Z", NULL, "Drops a lit TNT block there: real fuse, real blast.", {C_, C_, C_}},
    {"fire", "X Y Z", NULL, "Ignites the block there, if it burns.", {C_, C_, C_}},
    {"spawn", "CREATURE [X Y Z]", "a creature", "Spawns a creature, on the anchor unless you give a spot.",
     {CmdScript::ARG_CREATURE, C_, C_, C_}},
    {"day", "", NULL, "Gives the region its day sky back (time 0.25).", {0}},
    {"noon", "", NULL, "The day sky (time 0.5).", {0}},
    {"night", "", NULL, "Paints the region's sky night (time 0.75).", {0}},
    {"midnight", "", NULL, "The night sky (time 0).", {0}},
    {"time", "T", "T from 0 to 1", "Sets the time, T from 0 to 1: 0.25..0.75 is day, the rest night.",
     {CmdScript::ARG_NUMBER}},
    {"heal", "", NULL, "Full health.", {0}},
    {"hurt", "N", "a number", "Deals N% damage. Only bites with Health on.", {CmdScript::ARG_NUMBER}},
    {"gold", "N", "a number", "Grants N golden cubes (the purse holds 999).", {CmdScript::ARG_NUMBER}},
    {"sound", "N", "a number", "Plays sound effect number N.", {CmdScript::ARG_NUMBER}},
    {"flash", "", NULL, "White screen flash.", {0}},
    {"wait", "S", "a number", "Pauses this script S seconds (up to 600). Everything else keeps running.",
     {CmdScript::ARG_NUMBER}},
    {"run", "X Y Z", NULL, "Pushes the command block at that spot. Chains up to 8 deep.", {C_, C_, C_}},
    {"if", "X Y Z TYPE", NULL, "The next statement runs only if that cell holds that block.",
     {C_, C_, C_, CmdScript::ARG_BLOCK}},
    {"unless", "X Y Z TYPE", NULL, "The next statement runs only if it doesn't.", {C_, C_, C_, CmdScript::ARG_BLOCK}},
};
#undef C_

// "fill needs X Y Z X2 Y2 Z2 TYPE [COLOR]": the row's usage, or its `needs` wording when `own`.
std::string needs(const char* verb, bool own) {
    const CmdScript::Verb* v = CmdScript::findVerb(verb);
    return std::string(verb) + " needs " + (own && v->needs ? v->needs : v->usage);
}

bool bad(CmdScript::Stmt* s, const std::string& msg) {
    s->op = CmdScript::OP_BAD;
    s->text = msg;
    return false;
}
bool coords(const std::vector<std::string>& w, size_t from, int n, CmdScript::Stmt* s, const std::string& usage,
            bool absOK) {
    if (w.size() < from + n) return bad(s, usage);
    for (int i = 0; i < n; ++i)
        if (!coord(w[from + i], &s->v[i], &s->abs[i], absOK)) return bad(s, "Not a coordinate: " + w[from + i]);
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
bool parseStmt(const std::string& src, CmdScript::Stmt* s, const CmdScript::ParseOptions& opts) {
    using CS = CmdScript;
    s->op = CS::OP_BAD;
    std::memset(s->v, 0, sizeof(s->v));
    std::memset(s->abs, 0, sizeof(s->abs));
    s->type = 0;
    s->color = -1;
    s->hasPos = false;
    s->f = 0;
    s->text.clear();
    s->ext = -1;
    s->args.clear();
    const bool A = opts.absolute;
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
        if (!coords(w, 1, 3, s, needs("tp", false), A)) return false;
        s->op = CS::OP_TP;
        return true;
    }
    if (c == "set") {
        if (n < 5) return bad(s, needs("set", true));
        if (!coords(w, 1, 3, s, "", A)) return false;
        if (!block(w[4], s) || (n > 5 && !colour(w[5], s))) return false;
        s->op = CS::OP_SET;
        return true;
    }
    if (c == "fill") {
        if (n < 8) return bad(s, needs("fill", true));
        if (!coords(w, 1, 6, s, "", A)) return false;
        if (!block(w[7], s) || (n > 8 && !colour(w[8], s))) return false;
        const long cells = (long)(std::abs(s->v[3] - s->v[0]) + 1) * (std::abs(s->v[4] - s->v[1]) + 1) *
                           (std::abs(s->v[5] - s->v[2]) + 1);
        if (cells > CS::MAX_FILL) return bad(s, "fill is over 4096 blocks");
        s->op = CS::OP_FILL;
        return true;
    }
    if (c == "paint") {
        if (n < 5) return bad(s, needs("paint", true));
        if (!coords(w, 1, 3, s, "", A)) return false;
        if (!colour(w[4], s)) return false;
        s->op = CS::OP_PAINT;
        return true;
    }
    if (c == "explode" || c == "fire") {
        if (!coords(w, 1, 3, s, needs(c.c_str(), false), A)) return false;
        s->op = c == "fire" ? CS::OP_FIRE : CS::OP_EXPLODE;
        return true;
    }
    if (c == "spawn") {
        if (n < 2) return bad(s, needs("spawn", true));
        s->type = CmdScript::creatureByName(w[1].c_str());
        if (s->type < 0) return bad(s, "Unknown creature: " + w[1]);
        if (n > 2) {
            if (!coords(w, 2, 3, s, needs("spawn", false), A)) return false;
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
        if (n < 2 || !number(w[1], &v) || v < 0 || v > 1) return bad(s, needs("time", true));
        s->op = CS::OP_TIME;
        s->f = (float)v;
        return true;
    }
    if (c == "heal") { s->op = CS::OP_HEAL; return true; }
    if (c == "flash") { s->op = CS::OP_FLASH; return true; }
    if (c == "hurt" || c == "gold" || c == "sound" || c == "wait") {
        if (n < 2 || !number(w[1], &v) || v < 0) return bad(s, needs(c.c_str(), true));
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
        if (!coords(w, 1, 3, s, needs("run", false), A)) return false;
        s->op = CS::OP_RUN;
        return true;
    }
    if (c == "if" || c == "unless") {
        if (n < 5) return bad(s, needs(c.c_str(), true));
        if (!coords(w, 1, 3, s, "", A)) return false;
        if (!block(w[4], s)) return false;
        s->op = c == "if" ? CS::OP_IF : CS::OP_UNLESS;
        return true;
    }
    // The console's own verbs come after every built-in one, so they can never change a block's meaning.
    if (opts.ext && opts.ext->find) {
        const int i = opts.ext->find(c.c_str());
        if (i >= 0) {
            s->op = CS::OP_EXT;
            s->ext = i;
            s->args.assign(w.begin() + 1, w.end());
            return true;
        }
    }
    if (c[0] == '@')
        return bad(s, opts.triggers ? "A trigger must be the first statement: " + w[0]
                                    : std::string("Triggers only work in a command block"));
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

std::vector<std::string> CmdScript::blockNames() {
    std::vector<std::string> r;
    for (const Name& b : kBlocks) r.push_back(b.name);
    return r;
}

std::vector<std::string> CmdScript::creatureNames() {
    return std::vector<std::string>(kCreatures, kCreatures + NUM_CREATURES);
}

CmdScript::ParseOptions CmdScript::consoleOptions(const Ext* ext) {
    ParseOptions o;
    o.triggers = false;
    o.absolute = true;
    o.ext = ext;
    return o;
}

const CmdScript::Verb* CmdScript::verbs(size_t* count) {
    if (count) *count = sizeof(kVerbs) / sizeof(kVerbs[0]);
    return kVerbs;
}

const CmdScript::Verb* CmdScript::findVerb(const char* name) {
    const std::string t = lower(name ? name : "");
    for (const Verb& v : kVerbs) if (t == v.name) return &v;
    return NULL;
}

void CmdScript::parse(const char* src, Program* out) { parseLine(src, out, blockOptions()); }

void CmdScript::parseLine(const char* src, Program* out, const ParseOptions& opts) {
    *out = Program();
    out->ext = opts.ext;
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
        if (first && c[0] == '@' && opts.triggers) {
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
        if (!parseStmt(piece, &s, opts)) {
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
    TrailerPos block;     // SRC_BLOCK: the owner (the console's jobs have none)
    std::shared_ptr<const CmdScript::Program> prog;
    size_t pc;
    float wait;
    int depth;
    bool live;
    int source = CmdScript::SRC_BLOCK;
    int ax = 0, ay = 0, az = 0;           // the anchor, engine order (a block's own cell)
    CmdScript::Sink* sink = NULL;         // console/harness output; NULL = toasts (a block)
    int job = 0;                          // console job id, 0 for a block
    bool announced = false;               // the console was told "[job N] waiting"
    int cells = 0, refused = 0;           // console: cells set/refused since the last report
    std::string src;                      // console: the line, for jobs()
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
int g_nextJob = 1;        // console job ids, reset with the world
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

// A refusal: a block toasts it (rate-limited), a console job prints it.
void refuse(CmdScript::Sink* sink, const std::string& msg) {
    if (sink) sink->line(CmdScript::Sink::ERR, msg);
    else toast(msg, true);
}

// A console job's "N cells set, M refused" since its last report.
void reportCells(Inst* in) {
    if (!in->sink || (!in->cells && !in->refused)) return;
    char b[96];
    std::snprintf(b, sizeof(b), "%d cell%s set, %d refused", in->cells, in->cells == 1 ? "" : "s", in->refused);
    in->sink->line(CmdScript::Sink::OUT, b);
    in->cells = in->refused = 0;
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

// Start the block's script at `depth`: run it to its first wait. Returns PRESS_*. `from` is the
// console job's sink when a job's `run` started it (refusals print there).
int start(const TrailerPos& block, int depth, CmdScript::Sink* from = NULL) {
    if (CmdScript::liveCount() >= CmdScript::MAX_RUNNING) {
        g_stats.refusedFull++;
        refuse(from, "Too many command blocks running (32)");
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
    block.toEngine(&in->ax, &in->ay, &in->az);
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
    g_stats.finished++;
    if (in->source != CmdScript::SRC_BLOCK) {
        reportCells(in);
        if (in->announced && in->sink) in->sink->line(CmdScript::Sink::JOB, "[job " + std::to_string(in->job) + "] done");
        return;
    }
    auto it = g_liveOn.find(key(in->block));
    if (it != g_liveOn.end() && --it->second <= 0) g_liveOn.erase(it);
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
    const int bx = in->ax, by = in->ay, bz = in->az;   // the anchor (a block: its own cell), engine order
    while (in->live && in->pc < st.size()) {
        if (g_work >= CS::WORK_PER_FRAME) { in->wait = 0; g_stats.suspended++; reportCells(in); return; }
        const CS::Stmt& s = st[in->pc];
        g_work++;
        g_stats.statements++;
        // A console `=N` coordinate is absolute; everything else counts from the anchor.
        const int x = s.abs[0] ? s.v[0] : bx + s.v[0], y = s.abs[1] ? s.v[1] : by + s.v[1],
                  z = s.abs[2] ? s.v[2] : bz + s.v[2];
        const int cells0 = g_stats.cells, refused0 = g_stats.refusedCells;
        if (CS::trace) {
            if (in->source == CS::SRC_BLOCK)
                std::printf("[cmd] %d,%d,%d d%d #%zu op%d %d %d %d t%d c%d %s\n", in->block.x, in->block.y, in->block.z,
                            in->depth, in->pc, s.op, s.v[0], s.v[1], s.v[2], s.type, s.color, s.text.c_str());
            else
                std::printf("[cmd] job%d @%d,%d,%d #%zu op%d %d %d %d t%d c%d %s\n", in->job, bx, by, bz, in->pc, s.op,
                            x, y, z, s.type, s.color, s.text.c_str());
        }
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
            reportCells(in);
            return;   // always yields, at least to the next frame
        case CS::OP_RUN:
            in->pc++;
            if (in->depth + 1 > CS::MAX_DEPTH && !CS::testNoDepthCap) {
                g_stats.refusedDepth++;
                refuse(in->sink, "Command blocks can chain 8 deep");
            } else if (start(TrailerPos::fromEngine(x, y, z), in->depth + 1, in->sink) == CS::PRESS_NONE && in->sink) {
                in->sink->line(CS::Sink::ERR, "No command block at " + std::to_string(x) + " " + std::to_string(y) + " " +
                                                  std::to_string(z));
            }
            continue;
        case CS::OP_BAD:
            g_stats.unknown++;
            if (in->sink) in->sink->line(CS::Sink::ERR, s.text);
            else toast(s.text, false);
            break;
        case CS::OP_SAY:
            w->hud->sb->setStatus([NSString stringWithUTF8String:s.text.c_str()], 3);
            if (in->sink) in->sink->line(CS::Sink::OUT, s.text);
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
            const int x2 = s.abs[3] ? s.v[3] : bx + s.v[3], y2 = s.abs[4] ? s.v[4] : by + s.v[4],
                      z2 = s.abs[5] ? s.v[5] : bz + s.v[5];
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
            CS::timeHook(s.f);
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
        case CS::OP_EXT:
            if (in->prog->ext && in->prog->ext->run) {
                const CS::ExtCall call{bx, by, bz, s.args, in->sink, in->source, in->job};
                in->prog->ext->run(s.ext, call);
            }
            break;
        }
        in->cells += g_stats.cells - cells0;
        in->refused += g_stats.refusedCells - refused0;
        in->pc++;
    }
    if (in->live && in->pc >= st.size()) finish(in);
}

void syncGreen() {
    WorldTrailer* t = World::getWorld->fm->trailer();
    std::unordered_map<uint64_t, TrailerPos> want;
    for (const Inst* in : g_insts)
        if (in->live && in->source == CmdScript::SRC_BLOCK) want[key(in->block)] = in->block;
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
    for (const Inst* in : g_insts) if (in->live && in->source == SRC_BLOCK) ++n;
    return n;
}

void (*CmdScript::timeHook)(float t) = setSky;

void CmdScript::playerAnchor(int* x, int* y, int* z) {
    const Player* p = World::getWorld->player;
    const float feet = p->pos.y - p->boxheight / 2;
    *x = (int)std::floor(p->pos.x);
    *y = (int)std::floor(feet + (testAnchorFeet ? 0.01f : -0.01f));
    *z = (int)std::floor(p->pos.z);
}

int CmdScript::runLine(const char* src, int ax, int ay, int az, int source, Sink* sink, const Ext* ext) {
    World* w = World::getWorld;
    const size_t len = src ? std::strlen(src) : 0;
    if (len > MAX_LINE) {
        if (sink) sink->line(Sink::ERR, "A line holds up to 511 characters (this one has " + std::to_string(len) + ")");
        return RUN_LONG;
    }
    std::shared_ptr<Program> prog = std::make_shared<Program>();
    parseLine(src, prog.get(), consoleOptions(ext));
    if (prog->stmts.empty()) return RUN_EMPTY;
    if (prog->errors) {
        // Typed input is all or nothing: a half-run line can't be undone (dev-console-plan.md §2.4).
        for (size_t i = 0; sink && i < prog->stmts.size(); ++i)
            if (prog->stmts[i].op == OP_BAD) sink->line(Sink::ERR, std::to_string(i + 1) + ": " + prog->stmts[i].text);
        if (sink) sink->line(Sink::ERR, "line not run");
        return RUN_BAD;
    }
    if (!w || !w->terrain || !w->terrain->loaded || !w->fm) {
        if (sink) sink->line(Sink::ERR, "No world is open");
        return RUN_NO_WORLD;
    }
    // The job pool caps lines that wait; a line with no `wait` always runs (a frame-budget pause
    // ends next frame).
    bool waits = false;
    for (const Stmt& s : prog->stmts) waits = waits || s.op == OP_WAIT;
    if (waits && (int)jobs().size() >= MAX_JOBS) {
        if (sink) sink->line(Sink::ERR, "4 console jobs are waiting: stop one first (jobs, stop N)");
        return RUN_FULL;
    }
    Inst* in = new Inst();
    in->prog = prog;
    in->pc = 0;
    in->wait = 0;
    in->depth = 0;   // so a `run` from the console chains 8 deep, as from a block
    in->live = true;
    in->source = source;
    in->ax = ax; in->ay = ay; in->az = az;
    in->sink = sink;
    in->job = g_nextJob++;
    in->src.assign(src, len);
    g_insts.push_back(in);
    g_stats.lines++;
    execute(in);
    if (!in->live) return RUN_DONE;
    in->announced = true;
    if (sink) {
        char b[64];
        std::snprintf(b, sizeof(b), "[job %d] waiting %.1f s", in->job, in->wait);
        sink->line(Sink::JOB, b);
    }
    return in->job;
}

std::vector<CmdScript::JobInfo> CmdScript::jobs() {
    std::vector<JobInfo> r;
    for (const Inst* in : g_insts)
        if (in->live && in->source != SRC_BLOCK)
            r.push_back(JobInfo{in->job, in->wait, in->pc, in->prog->stmts.size(), in->src});
    return r;
}

int CmdScript::stop(int job) {
    int n = 0;
    for (Inst* in : g_insts) {
        if (!in->live) continue;
        const bool console = in->source != SRC_BLOCK;
        if (!(job == STOP_ALL || (console && (job == STOP_JOBS || in->job == job)))) continue;
        in->announced = false;   // no "done" line: it was stopped
        finish(in);
        if (console && in->sink) in->sink->line(Sink::JOB, "[job " + std::to_string(in->job) + "] stopped");
        ++n;
    }
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
    g_nextJob = 1;
    CmdTool::clearRunning();
    g_stats.live = 0;
}

CmdScript::Stats CmdScript::stats() {
    g_stats.live = liveCount();
    g_stats.jobs = (int)jobs().size();
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
