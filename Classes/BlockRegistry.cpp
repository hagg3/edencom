//
//  BlockRegistry.cpp
//  Emod — K.1-3. See BlockRegistry.h and WORKING/block-registry-plan-2026-10-31.md §4.
//
#include "BlockRegistry.h"
#include "Globals.h"
#include "Constants.h"
#include "EdenWorldStore.h"   // emod::palette_* (the record's one codec)
#include "MpWorld.h"          // D7: no mod blocks in a session
#include "Shadow.h"
#include "World.h"
#include "FileManager.h"

#include <map>

namespace BlockRegistry {
namespace {

// D15: the enum minus TYPE_, lowercased. Never reordered (rule 4: the index is the saved byte).
const char* const kStock[128] = {
    "eden:none", "eden:bedrock", "eden:stone", "eden:dirt",  // 0-3
    "eden:sand", "eden:leaves", "eden:tree", "eden:wood",  // 4-7
    "eden:grass", "eden:tnt", "eden:dark_stone", "eden:grass2",  // 8-11
    "eden:grass3", "eden:brick", "eden:cobblestone", "eden:ice",  // 12-15
    "eden:crystal", "eden:trampoline", "eden:ladder", "eden:cloud",  // 16-19
    "eden:water", "eden:weave", "eden:vine", "eden:lava",  // 20-23
    "eden:stone_ramp1", "eden:stone_ramp2", "eden:stone_ramp3", "eden:stone_ramp4",  // 24-27
    "eden:wood_ramp1", "eden:wood_ramp2", "eden:wood_ramp3", "eden:wood_ramp4",  // 28-31
    "eden:shingle_ramp1", "eden:shingle_ramp2", "eden:shingle_ramp3", "eden:shingle_ramp4",  // 32-35
    "eden:ice_ramp1", "eden:ice_ramp2", "eden:ice_ramp3", "eden:ice_ramp4",  // 36-39
    "eden:stone_side1", "eden:stone_side2", "eden:stone_side3", "eden:stone_side4",  // 40-43
    "eden:wood_side1", "eden:wood_side2", "eden:wood_side3", "eden:wood_side4",  // 44-47
    "eden:shingle_side1", "eden:shingle_side2", "eden:shingle_side3", "eden:shingle_side4",  // 48-51
    "eden:ice_side1", "eden:ice_side2", "eden:ice_side3", "eden:ice_side4",  // 52-55
    "eden:shingle", "eden:gradient", "eden:glass", "eden:water3",  // 56-59
    "eden:water2", "eden:water1", "eden:lava3", "eden:lava2",  // 60-63
    "eden:lava1", "eden:firework", "eden:door1", "eden:door2",  // 64-67
    "eden:door3", "eden:door4", "eden:door_top", "eden:golden_cube",  // 68-71
    "eden:lightbox", "eden:flower", "eden:steel", "eden:portal1",  // 72-75
    "eden:portal2", "eden:portal3", "eden:portal4", "eden:portal_top",  // 76-79
    "eden:custom", "eden:block_tnt", "eden:btgrass", "eden:btdarkstone",  // 80-83
    "eden:btstone", "eden:btdirt", "eden:btsand", "eden:bttnt",  // 84-87
    "eden:btwood", "eden:btshingle", "eden:btglass", "eden:btgradient",  // 88-91
    "eden:bttree", "eden:btleaves", "eden:btbrick", "eden:btcobblestone",  // 92-95
    "eden:btvines", "eden:btladder", "eden:btice", "eden:btcrystal",  // 96-99
    "eden:bttrampoline", "eden:btcloud", "eden:btstoneside", "eden:btwoodside",  // 100-103
    "eden:bticeside", "eden:btshingleside", "eden:btfence", "eden:btwater",  // 104-107
    "eden:btlava", "eden:btfirework", "eden:btlightbox", "eden:btsteel",  // 108-111
    "eden:ore_sand", "eden:space_stone", "eden:carpet", "eden:snakeskin",  // 112-115
    "eden:obsidian", "eden:cheese", "eden:space_dirt", "eden:space_grass",  // 116-119
    "eden:moss", "eden:dark_matter", "eden:space_sand", "eden:snow",  // 120-123
    "eden:moonrock", "eden:basalt", "eden:dark_tile", "eden:algae",  // 124-127
};

// The console's short names (moved from CmdScript's kBlocks, K.1-3). cmd.html's names; voidstone, sky,
// pillar, jungle, future, tech and lavafield are the 2026 game's names for blocks Emod cannot identify (no
// list maps them onto 112-127): unknown here, on purpose.
struct Alias { const char* name; int id; };
const Alias kAliases[] = {
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

struct Slot { bool used; uint8_t fbType, fbPaint; std::string name; };
// Function-local statics: Globals.cpp's static init fills the tables before any of this exists.
struct State {
    Slot slots[LOCAL_IDS];
    std::map<std::string, int> ids;          // name -> 128..255
    std::map<std::string, BlockDef> defs;
    bool dirty = false, locked = false;
    int resolves = 0;
    State() { for (Slot& s : slots) { s.used = false; s.fbType = 0; s.fbPaint = 0; } }
};
State& st() { static State s; return s; }

// palette_parse's fallback rule: a stock block a stock game can hold alone (not air, a door/portal
// half or the custom block).
bool fallbackOk(int t) { return t >= 1 && t <= STOCK_BLOCK_MAX && !(t >= 66 && t <= 70) && !(t >= 75 && t <= 80); }

void clearSlots() {
    State& s = st();
    for (Slot& x : s.slots) { x.used = false; x.fbType = 0; x.fbPaint = 0; x.name.clear(); }
    s.ids.clear();
}

}  // namespace

const char* stockName(int id) { return id >= 0 && id <= STOCK_BLOCK_MAX ? kStock[id] : NULL; }

int stockByName(const std::string& t) {
    if (t.compare(0, 5, "eden:") == 0) {
        for (int i = 0; i <= STOCK_BLOCK_MAX; ++i) if (t == kStock[i]) return i;
        return -1;
    }
    for (const Alias& a : kAliases) if (t == a.name) return a.id;
    return -1;
}

std::vector<std::string> aliasNames() {
    std::vector<std::string> r;
    for (const Alias& a : kAliases) r.push_back(a.name);
    return r;
}

BlockDef defLike(const std::string& name, int like, int fallbackPaint) {
    BlockDef d;
    d.name = name;
    d.fallbackType = (uint8_t)like;
    d.fallbackPaint = (uint8_t)fallbackPaint;
    d.info = blockinfo[like];
    for (int i = 0; i < 6; ++i) d.faces[i] = blockTypeFaces[like][i];
    for (int i = 0; i < 3; ++i) d.color[i] = blockColor[like][i];
    d.hudMap = hudBlocksMap[like];
    d.tntMap = blockTntMap[like];
    return d;
}

bool registerDef(const BlockDef& def) {
    if (!emod::palette_name_ok(def.name) || !fallbackOk(def.fallbackType) || def.fallbackPaint > emod::PALETTE_PAINT_MAX)
        return false;
    BlockDef d = def;
    d.info &= ~IS_MISSING;   // the placeholder bit is the registry's, never a definition's
    st().defs[def.name] = d;
    return true;
}
bool unregisterDef(const std::string& name) { return st().defs.erase(name) > 0; }
void clearDefs() { st().defs.clear(); }
const BlockDef* findDef(const std::string& name) {
    std::map<std::string, BlockDef>::const_iterator it = st().defs.find(name);
    return it == st().defs.end() ? NULL : &it->second;
}

void resetWorld() {
    clearSlots();
    st().dirty = false;
    st().locked = false;
    resolve();
}

bool loadPalette(const uint8_t* rec, size_t len, bool flagged) {
    State& s = st();
    clearSlots();
    s.dirty = false;
    s.locked = false;
    bool ok = true;
    if (rec && len) {
        std::vector<emod::PaletteEntry> es;
        std::vector<std::string> errs;
        ok = emod::palette_parse(rec, len, &es, &errs) && errs.empty();
        if (ok) {
            for (const emod::PaletteEntry& e : es) {
                Slot& x = s.slots[e.id - FIRST_LOCAL];
                x.used = true; x.fbType = e.fallbackType; x.fbPaint = e.fallbackPaint; x.name = e.name;
                s.ids[e.name] = e.id;
            }
        } else {
            clearSlots();
            fprintf(stderr, "registry: the BLOCK_PALETTE does not parse (%s): mod blocks are off for this world\n",
                    errs.empty() ? "?" : errs[0].c_str());
        }
    } else if (flagged) {
        ok = false;   // a flagged world with no readable record: which id is whose is unknown
        fprintf(stderr, "registry: flagged world without a readable BLOCK_PALETTE: mod blocks are off for this world\n");
    }
    s.locked = !ok;
    resolve();
    return ok;
}

bool locked() { return st().locked; }
void lock() { st().locked = true; }
int count() { return (int)st().ids.size(); }

int idOf(const std::string& name) {
    std::map<std::string, int>::const_iterator it = st().ids.find(name);
    return it == st().ids.end() ? -1 : it->second;
}
const char* nameOf(int id) {
    if (id < FIRST_LOCAL || id >= FIRST_LOCAL + LOCAL_IDS) return NULL;
    const Slot& x = st().slots[id - FIRST_LOCAL];
    return x.used ? x.name.c_str() : NULL;
}
bool assigned(int id) { return nameOf(id) != NULL; }
bool missing(int id) {
    if (id < FIRST_LOCAL || id >= FIRST_LOCAL + LOCAL_IDS) return false;
    const char* n = nameOf(id);
    return !n || !findDef(n);
}

int allocate(const std::string& name, std::string* why) {
    State& s = st();
    const int have = idOf(name);
    if (have >= 0) return have;
    const BlockDef* d = findDef(name);
    if (!d) { if (why) *why = "Unknown block: " + name; return -1; }
    if (s.locked) { if (why) *why = "This world's block palette is unreadable: mod blocks are off"; return -1; }
    for (int i = 0; i < LOCAL_IDS; ++i) {
        Slot& x = s.slots[i];
        if (x.used) continue;
        x.used = true; x.fbType = d->fallbackType; x.fbPaint = d->fallbackPaint; x.name = name;
        s.ids[name] = FIRST_LOCAL + i;
        s.dirty = true;
        resolve();
        return FIRST_LOCAL + i;
    }
    if (why) *why = "This world has used all 128 mod block slots";   // D17: never evict
    return -1;
}

int acquire(const std::string& name, std::string* why) {
    const int have = idOf(name);
    if (have >= 0) return have;
    if (MpWorld::active()) { if (why) *why = "Mod blocks are not available in a multiplayer world"; return -1; }
    if (!findDef(name)) { if (why) *why = "Unknown block: " + name; return -1; }
    if (st().locked) { if (why) *why = "This world's block palette is unreadable: mod blocks are off"; return -1; }
    if (count() >= LOCAL_IDS) { if (why) *why = "This world has used all 128 mod block slots"; return -1; }
    World* w = World::getWorld;
    if (!w || !w->fm || !w->fm->preparePaletteAllocation(why)) return -1;
    return allocate(name, why);
}

std::vector<uint8_t> encode() {
    std::vector<emod::PaletteEntry> es;
    const State& s = st();
    for (int i = 0; i < LOCAL_IDS; ++i) {
        const Slot& x = s.slots[i];
        if (!x.used) continue;
        emod::PaletteEntry e;
        e.id = (uint8_t)(FIRST_LOCAL + i); e.fallbackType = x.fbType; e.fallbackPaint = x.fbPaint; e.flags = 0; e.name = x.name;
        es.push_back(e);
    }
    std::vector<uint8_t> out;
    emod::palette_build(es, out);
    return out;
}
bool dirty() { return st().dirty; }
void markSaved() { st().dirty = false; }

void resolve() {
    State& s = st();
    for (int i = 0; i < LOCAL_IDS; ++i) {
        const int t = FIRST_LOCAL + i;
        const BlockDef* d = s.slots[i].used ? findDef(s.slots[i].name) : NULL;
        if (!d) { eden_set_missing_block_row(t); continue; }
        blockinfo[t] = d->info;
        for (int k = 0; k < 6; ++k) blockTypeFaces[t][k] = d->faces[k];
        for (int k = 0; k < 3; ++k) blockColor[t][k] = d->color[k];
        hudBlocksMap[t] = d->hudMap;
        blockTntMap[t] = d->tntMap;
    }
    shadow_buildCasterTable();
    s.resolves++;
}

int resolveCount() { return st().resolves; }

}  // namespace BlockRegistry
