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
#include "BlockTraits.h"   // K.2-2: blockSoundSet, blockPaintArt
#include "World.h"
#include "FileManager.h"

#include <map>

namespace BlockRegistry {
namespace {

// The stock half (K.2-3): names 0..127 and the console aliases come from the `eden` pack
// (media/packs/eden/pack.json, ContentPack::loadStock at boot). The names are the enum minus TYPE_,
// lowercased (D15), and never change meaning (rule 4: the index is the saved byte). The aliases are the
// console's short names (moved from CmdScript's kBlocks in K.1-3), in the pack's order.
struct StockNames {
    std::string names[STOCK_BLOCK_MAX + 1];
    std::vector<std::pair<std::string, int>> aliases;
};
StockNames& stock() { static StockNames s; return s; }

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

void setStock(const std::vector<std::string>& names, const std::vector<std::pair<std::string, int>>& aliases) {
    StockNames& s = stock();
    for (int i = 0; i <= STOCK_BLOCK_MAX; ++i) s.names[i] = i < (int)names.size() ? names[i] : std::string();
    s.aliases = aliases;
}

const char* stockName(int id) {
    if (id < 0 || id > STOCK_BLOCK_MAX || stock().names[id].empty()) return NULL;
    return stock().names[id].c_str();
}

int stockByName(const std::string& t) {
    const StockNames& s = stock();
    if (t.compare(0, 5, "eden:") == 0) {
        for (int i = 0; i <= STOCK_BLOCK_MAX; ++i) if (t == s.names[i]) return i;
        return -1;
    }
    for (const auto& a : s.aliases) if (t == a.first) return a.second;
    return -1;
}

std::vector<std::string> aliasNames() {
    std::vector<std::string> r;
    for (const auto& a : stock().aliases) r.push_back(a.first);
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
    d.soundSet = blockSoundSet[like];
    d.paintArt = blockPaintArt[like];
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
        blockSoundSet[t] = d->soundSet < blockSoundSetCount ? d->soundSet : 0;
        blockPaintArt[t] = d->paintArt;
    }
    shadow_buildCasterTable();
    s.resolves++;
}

int resolveCount() { return st().resolves; }

}  // namespace BlockRegistry
