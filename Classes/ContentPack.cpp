//
//  ContentPack.cpp
//  Emod — K.2-3. See ContentPack.h and docs/content-packs.md.
//
//  The validator below and port/tools/pack.py's compile_pack() check the same rules in the same order and
//  print the same lines; change both together (`pack.py fixtures` + `--pack-selftest` read one fixture set).
//
#include "ContentPack.h"
#include "BlockRegistry.h"
#include "BlockTraits.h"
#include "Constants.h"
#include "Globals.h"
#include "Hud.h"
#include "Json.h"
#include "Shadow.h"
#include "Resources.h"
#include "EdenWorldStore.h"
#include "eden_fs.h"
#include "eden_image.h"

#include <algorithm>
#include <ctype.h>
#include <map>
#include <set>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

extern "C" int hudBlocksMap[BLOCK_IDS];
extern "C" int blockTntMap[BLOCK_IDS];

namespace ContentPack {

enum { STOCK_IDS = STOCK_BLOCK_MAX + 1, BLOCKS_MAX = 512 };

struct Stock {
    int info[STOCK_IDS];
    uint8_t color[STOCK_IDS][3];
    int faces[STOCK_IDS][6];
    int hud[STOCK_IDS], tnt[STOCK_IDS];
    uint8_t soundSet[STOCK_IDS], paintArt[STOCK_IDS];
    std::string names[STOCK_IDS];
    std::vector<std::pair<std::string, int>> aliases;
    std::vector<int> pages[2];
    uint8_t twin[ATLAS_TILES], particleArt[ATLAS_TILES];
    std::vector<BlockSoundSet> sets;
    std::map<std::string, std::pair<int, int>> tileNames;   // K.2-4: eden: tiles for mod packs' faces
    Stock() {
        memset(info, 0, sizeof info); memset(color, 0, sizeof color); memset(faces, 0, sizeof faces);
        memset(hud, 0, sizeof hud); memset(tnt, 0, sizeof tnt); memset(soundSet, 0, sizeof soundSet);
        memset(paintArt, 0, sizeof paintArt); memset(twin, 0, sizeof twin); memset(particleArt, 0, sizeof particleArt);
    }
};

namespace {

// ---- The vocabulary (docs/content-packs.md; pack.py holds the same lists).
struct NameBits { const char* name; int bits; };
// "solid" is the absence of IS_NOTSOLID, "paint_art" sets every blockPaintArt site.
const NameBits kTraits[] = {
    {"solid", 0}, {"hard", IS_HARD}, {"flammable", IS_FLAMMABLE}, {"transparent", IS_ATLAS2},
    {"climbable", IS_CLIMB}, {"bouncy", IS_BOUNCY}, {"slippery", IS_SLIPPERY}, {"blastproof", IS_BLASTPROOF},
    {"unbreakable", IS_UNBREAKABLE}, {"no_shadow", IS_NOSHADOW}, {"paint_art", 0},
};
// Bits that are kinds: the stock pack only (D7).
const NameBits kFlags[] = {
    {"ramp", IS_RAMP}, {"side", IS_SIDE}, {"ramp_or_side", IS_RAMPORSIDE}, {"liquid", IS_LIQUID},
    {"water", IS_WATER}, {"lava", IS_LAVA}, {"colored_special", IS_COLOREDSPECIAL}, {"grass", IS_GRASS},
    {"ice", IS_ICE}, {"object", IS_OBJECT}, {"door", IS_DOOR}, {"portal", IS_PORTAL}, {"block_tnt", IS_BLOCKTNT},
};
const char* const kKinds[] = {"cube", "air", "grass", "tnt", "liquid", "ramp", "side", "firework", "door", "object",
                              "lightbox", "portal", "custom", "block_tnt", "bt"};
const char* const kSites[] = {"chunk", "batch", "cursor", "icon", "picker", "particle"};   // ART_* bit order
const char* const kTntGroups[] = {"other", "dirt", "stone", "glass"};                      // TRAIT_TNT_*
// Resources.h SOUND_TYPES a sound set may name: the enum's name minus S_, lowercased.
const NameBits kSounds[] = {
    {"land_soft", S_LAND_SOFT}, {"land_hard", S_LAND_HARD}, {"build_wood", S_BUILD_WOOD},
    {"build_water", S_BUILD_WATER}, {"build_stone", S_BUILD_STONE}, {"build_leaves", S_BUILD_LEAVES},
    {"build_lava", S_BUILD_LAVA}, {"build_glass", S_BUILD_GLASS}, {"build_dirt", S_BUILD_DIRT},
    {"build_generic", S_BUILD_GENERIC}, {"break_wood", S_BREAK_WOOD}, {"break_water", S_BREAK_WATER},
    {"break_stone", S_BREAK_STONE}, {"break_leaves", S_BREAK_LEAVES}, {"break_lava", S_BREAK_LAVA},
    {"break_dirt", S_BREAK_DIRT}, {"footsteps_hard", S_FOOTSTEPS_HARD}, {"footsteps_soft", S_FOOTSTEPS_SOFT},
    {"break_glass", S_BREAK_GLASS}, {"break_generic", S_BREAK_GENERIC}, {"build_metal", S_BUILD_METAL},
    {"build_light", S_BUILD_LIGHT}, {"metal_destroy", S_METAL_DESTROY},
};
const char* const kTopKeys[] = {"format", "id", "name", "version", "tiles", "sound_sets", "blocks", "aliases", "picker_pages"};
const char* const kTileKeys[] = {"atlas", "index", "unpainted", "particle_art"};
const char* const kSetKeys[] = {"build", "break", "land", "step", "tnt"};
const char* const kBlockKeys[] = {"id", "stock_id", "kind", "traits", "flags", "faces", "color", "sounds",
                                  "paint_art_sites", "bt", "tnt_map"};
const char* const kFaceKeys[] = {"front", "back", "left", "right", "bottom", "top"};   // blockTypeFaces 0..5
const int kPageCells[2] = {NUM_DISPLAY_BLOCKS, NUM_PICKER_PAGE2_CELLS};

template <size_t N>
int indexOf(const char* const (&list)[N], const std::string& s) {
    for (size_t i = 0; i < N; ++i) if (s == list[i]) return (int)i;
    return -1;
}
template <size_t N>
const NameBits* findBits(const NameBits (&list)[N], const std::string& s) {
    for (size_t i = 0; i < N; ++i) if (s == list[i].name) return &list[i];
    return NULL;
}

bool nameOk(const std::string& s, size_t maxLen, bool slash) {
    if (s.empty() || s.size() > maxLen) return false;
    for (char c : s)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || (slash && c == '/'))) return false;
    return true;
}
bool localOk(const std::string& s) { return nameOk(s, 24, false); }    // pack ids, tiles, sound sets, aliases
bool blockIdOk(const std::string& s) { return nameOk(s, 48, true); }  // the path half of a K.1 D15 name

const char* textOr(const Json::Value& v) { return v.isString() ? v.str.c_str() : "?"; }

struct Errors {
    const char* label;
    std::vector<std::string>* out;
    void operator()(size_t off, const char* fmt, ...) __attribute__((format(printf, 3, 4))) {
        va_list ap, ap2;
        va_start(ap, fmt);
        va_copy(ap2, ap);
        const int n = vsnprintf(NULL, 0, fmt, ap);
        va_end(ap);
        std::string msg(n > 0 ? (size_t)n : 0, '\0');
        if (n > 0) vsnprintf(&msg[0], (size_t)n + 1, fmt, ap2);
        va_end(ap2);
        char head[64];
        snprintf(head, sizeof head, "%s@%zu: ", label, off);
        out->push_back(head + msg);
    }
    size_t count() const { return out->size(); }
};

int64_t intIn(const Json::Value* v, int64_t lo, int64_t hi, bool* ok) {
    int64_t x = 0;
    *ok = v && v->asInt(&x, lo, hi);
    return x;
}

struct Tile { int atlas, index; const Json::Value* v; };

}  // namespace

Stock* compileStock(const char* data, size_t len, const char* label, std::vector<std::string>* errors) {
    std::vector<std::string> local;
    std::vector<std::string>* out = errors ? errors : &local;
    out->clear();
    Errors err{label, out};
    Json::Value root;
    Json::Error je;
    if (!Json::parse(data, len, &root, &je)) {
        err(je.offset, "JSON: %s", je.message.c_str());
        return NULL;
    }
    if (!root.isObject()) {
        err(root.offset, "pack.json must be an object");
        return NULL;
    }
    for (const auto& m : root.obj)
        if (indexOf(kTopKeys, m.first) < 0) err(m.second.offset, "unknown key \"%s\"", m.first.c_str());
    Stock* st = new Stock();
    bool ok;

    const Json::Value* fmt = root.get("format");
    if (!fmt) err(root.offset, "missing \"format\"");
    else { intIn(fmt, 1, 1, &ok); if (!ok) err(fmt->offset, "\"format\" must be 1 (this build reads format 1)"); }
    const Json::Value* pid = root.get("id");
    if (!pid) err(root.offset, "missing \"id\"");
    else if (!pid->isString() || !localOk(pid->str)) err(pid->offset, "\"id\" must match [a-z0-9_]{1,24}");
    else if (pid->str != "eden") err(pid->offset, "the stock pack's \"id\" must be \"eden\"");
    for (const char* k : {"name", "version"}) {
        const Json::Value* v = root.get(k);
        if (v && !v->isString()) err(v->offset, "\"%s\" must be a string", k);
    }

    // ---- tiles
    std::map<std::string, Tile> tiles;
    const Json::Value* tv = root.get("tiles");
    if (!tv) err(root.offset, "missing \"tiles\"");
    else if (!tv->isObject()) err(tv->offset, "\"tiles\" must be an object");
    else {
        std::map<std::pair<int, int>, std::string> slots;
        for (const auto& m : tv->obj) {
            const std::string& name = m.first;
            const Json::Value& t = m.second;
            const char* nm = name.c_str();
            if (!localOk(name)) { err(t.offset, "tile \"%s\": the name must match [a-z0-9_]{1,24}", nm); continue; }
            if (!t.isObject()) { err(t.offset, "tile \"%s\": must be an object", nm); continue; }
            bool bad = false;
            for (const auto& k : t.obj)
                if (indexOf(kTileKeys, k.first) < 0) { err(k.second.offset, "tile \"%s\": unknown key \"%s\"", nm, k.first.c_str()); bad = true; }
            const Json::Value* a = t.get("atlas");
            const Json::Value* ix = t.get("index");
            const int atlas = (int)intIn(a, 1, 2, &ok);
            if (!ok) { err(a ? a->offset : t.offset, "tile \"%s\": \"atlas\" must be 1 or 2", nm); continue; }
            const int index = (int)intIn(ix, 0, ATLAS_TILES - 1, &ok);
            if (!ok) { err(ix ? ix->offset : t.offset, "tile \"%s\": \"index\" must be 0..127", nm); continue; }
            const std::pair<int, int> key(atlas, index);
            auto had = slots.find(key);
            if (had != slots.end()) {
                err(t.offset, "tile \"%s\": atlas %d index %d is already tile \"%s\"", nm, atlas, index, had->second.c_str());
                continue;
            }
            slots[key] = name;
            if (!bad) { tiles[name] = Tile{atlas, index, &t}; st->tileNames[name] = key; }
        }
        for (const auto& m : tv->obj) {
            auto it = tiles.find(m.first);
            if (it == tiles.end()) continue;
            const Tile& tile = it->second;
            const char* nm = m.first.c_str();
            const Json::Value* u = tile.v->get("unpainted");
            const Json::Value* pa = tile.v->get("particle_art");
            if (pa && !pa->isBool()) err(pa->offset, "tile \"%s\": \"particle_art\" must be true or false", nm);
            else if (pa && pa->b) {
                if (tile.atlas != 1) err(pa->offset, "tile \"%s\": particle_art is for atlas 1 tiles only", nm);
                else st->particleArt[tile.index] = 1;
            }
            if (u) {
                if (!u->isString()) err(u->offset, "tile \"%s\": \"unpainted\" must be a tile name", nm);
                else {
                    auto tw = tiles.find(u->str);
                    if (tw == tiles.end()) err(u->offset, "tile \"%s\": unknown unpainted tile \"%s\"", nm, u->str.c_str());
                    else if (tile.atlas != 1 || tw->second.atlas != 1 || tw->second.index == 0)
                        err(u->offset, "tile \"%s\": an unpainted twin pairs two atlas 1 tiles, and index 0 cannot be one", nm);
                    else st->twin[tile.index] = (uint8_t)tw->second.index;
                }
            }
        }
    }

    // ---- sound sets
    std::map<std::string, int> sets;
    const Json::Value* sv = root.get("sound_sets");
    if (!sv) err(root.offset, "missing \"sound_sets\"");
    else if (!sv->isObject()) err(sv->offset, "\"sound_sets\" must be an object");
    else if (sv->obj.empty()) err(sv->offset, "\"sound_sets\" is empty (the first set is every block's default)");
    else if (sv->obj.size() > SOUND_SETS_MAX)
        err(sv->offset, "\"sound_sets\" has %zu sets (at most %d)", sv->obj.size(), (int)SOUND_SETS_MAX);
    else {
        for (const auto& m : sv->obj) {
            const char* nm = m.first.c_str();
            const Json::Value& s = m.second;
            if (!localOk(m.first)) { err(s.offset, "sound set \"%s\": the name must match [a-z0-9_]{1,24}", nm); continue; }
            if (!s.isObject()) { err(s.offset, "sound set \"%s\": must be an object", nm); continue; }
            bool good = true;
            for (const auto& k : s.obj)
                if (indexOf(kSetKeys, k.first) < 0) { err(k.second.offset, "sound set \"%s\": unknown key \"%s\"", nm, k.first.c_str()); good = false; }
            int row[4] = {-1, -1, -1, -1};
            const char* const ev[4] = {"build", "break", "land", "step"};
            for (int e = 0; e < 4; ++e) {
                const bool required = e < 2;
                const Json::Value* v = s.get(ev[e]);
                if (!v && required) { err(s.offset, "sound set \"%s\": missing \"%s\"", nm, ev[e]); good = false; }
                else if (!v || v->isNull()) {
                    if (required) { err(v->offset, "sound set \"%s\": \"%s\" must name a sound", nm, ev[e]); good = false; }
                } else {
                    const NameBits* snd = v->isString() ? findBits(kSounds, v->str) : NULL;
                    if (!snd) { err(v->offset, "sound set \"%s\": unknown sound \"%s\"", nm, textOr(*v)); good = false; }
                    else row[e] = snd->bits;
                }
            }
            const Json::Value* g = s.get("tnt");
            int group = -1;
            if (!g) { err(s.offset, "sound set \"%s\": missing \"tnt\"", nm); good = false; }
            else if (!g->isString() || (group = indexOf(kTntGroups, g->str)) < 0) {
                err(g->offset, "sound set \"%s\": \"tnt\" must be one of other, dirt, stone, glass", nm);
                good = false;
            }
            if (good) {
                BlockSoundSet bs;
                memset(&bs, 0, sizeof bs);
                snprintf(bs.name, sizeof bs.name, "eden:%s", nm);
                bs.build = (int16_t)row[0]; bs.brk = (int16_t)row[1]; bs.land = (int16_t)row[2]; bs.step = (int16_t)row[3];
                bs.tnt = (uint8_t)group;
                sets[m.first] = (int)st->sets.size();
                st->sets.push_back(bs);
            }
        }
    }

    // ---- blocks
    auto tileRef = [&](const std::string& s) -> const Tile* {
        const std::string k = s.compare(0, 5, "eden:") == 0 ? s.substr(5) : s;
        auto it = tiles.find(k);
        return it == tiles.end() ? NULL : &it->second;
    };
    std::map<std::string, int> byName;   // "eden:<id>" -> stock id
    struct Deferred { int sid; std::string ctx; const char* key; const Json::Value* v; };
    std::vector<Deferred> deferred;
    bool named[STOCK_IDS] = {};
    const Json::Value* bv = root.get("blocks");
    if (!bv) err(root.offset, "missing \"blocks\"");
    else if (!bv->isArray()) err(bv->offset, "\"blocks\" must be an array");
    else if (bv->arr.size() > BLOCKS_MAX) err(bv->offset, "\"blocks\" has %zu blocks (at most %d)", bv->arr.size(), (int)BLOCKS_MAX);
    else {
        std::set<std::string> ids;
        for (size_t n = 0; n < bv->arr.size(); ++n) {
            const Json::Value& b = bv->arr[n];
            if (!b.isObject()) { err(b.offset, "block #%zu: must be an object", n); continue; }
            const Json::Value* idv = b.get("id");
            if (!(idv && idv->isString() && blockIdOk(idv->str))) {
                err(idv ? idv->offset : b.offset, "block #%zu: \"id\" must match [a-z0-9_/]{1,48}", n);
                continue;
            }
            const std::string ctxs = "block \"" + idv->str + "\": ";
            const char* ctx = ctxs.c_str();
            for (const auto& k : b.obj)
                if (indexOf(kBlockKeys, k.first) < 0) err(k.second.offset, "%sunknown key \"%s\"", ctx, k.first.c_str());
            if (!ids.insert(idv->str).second) { err(idv->offset, "%sduplicate block id", ctx); continue; }
            const Json::Value* sidv = b.get("stock_id");
            const int sid = (int)intIn(sidv, 0, STOCK_IDS - 1, &ok);
            if (!ok) { err(sidv ? sidv->offset : b.offset, "%s\"stock_id\" must be 0..127", ctx); continue; }
            if (named[sid]) { err(sidv->offset, "%sstock_id %d is already \"%s\"", ctx, sid, st->names[sid].c_str()); continue; }
            const Json::Value* kind = b.get("kind");
            if (kind && (!kind->isString() || indexOf(kKinds, kind->str) < 0)) err(kind->offset, "%sunknown kind \"%s\"", ctx, textOr(*kind));
            int info = IS_NOTSOLID, art = 0;
            std::set<std::string> seen;
            const Json::Value* tr = b.get("traits");
            if (tr) {
                if (!tr->isArray()) err(tr->offset, "%s\"traits\" must be an array of names", ctx);
                else for (const Json::Value& t : tr->arr) {
                    const NameBits* nb = t.isString() ? findBits(kTraits, t.str) : NULL;
                    if (!nb) err(t.offset, "%sunknown trait \"%s\"", ctx, textOr(t));
                    else if (!seen.insert(t.str).second) err(t.offset, "%strait \"%s\" listed twice", ctx, t.str.c_str());
                    else if (t.str == "solid") info &= ~IS_NOTSOLID;
                    else if (t.str == "paint_art") art = ART_ALL;
                    else info |= nb->bits;
                }
            }
            const Json::Value* fl = b.get("flags");
            if (fl) {
                if (!fl->isArray()) err(fl->offset, "%s\"flags\" must be an array of names", ctx);
                else for (const Json::Value& f : fl->arr) {
                    const NameBits* nb = f.isString() ? findBits(kFlags, f.str) : NULL;
                    if (!nb) err(f.offset, "%sunknown flag \"%s\"", ctx, textOr(f));
                    else if (info & nb->bits) err(f.offset, "%sflag \"%s\" listed twice", ctx, f.str.c_str());
                    else info |= nb->bits;
                }
            }
            const Json::Value* sites = b.get("paint_art_sites");
            if (sites) {
                if (seen.count("paint_art")) err(sites->offset, "%s\"paint_art_sites\" and the \"paint_art\" trait together", ctx);
                else if (!sites->isArray()) err(sites->offset, "%s\"paint_art_sites\" must be an array of site names", ctx);
                else for (const Json::Value& s : sites->arr) {
                    const int bit = s.isString() ? indexOf(kSites, s.str) : -1;
                    if (bit < 0) err(s.offset, "%sunknown paint art site \"%s\"", ctx, textOr(s));
                    else if (art & (1 << bit)) err(s.offset, "%spaint art site \"%s\" listed twice", ctx, s.str.c_str());
                    else art |= 1 << bit;
                }
            }
            const int atlas = (info & IS_ATLAS2) ? 2 : 1;
            int faces[6] = {0, 0, 0, 0, 0, 0};
            const Json::Value* fv = b.get("faces");
            if (fv) {
                std::vector<std::string> keys;
                if (fv->isObject()) for (const auto& k : fv->obj) keys.push_back(k.first);
                std::sort(keys.begin(), keys.end());
                std::vector<std::string> six(kFaceKeys, kFaceKeys + 6);
                std::sort(six.begin(), six.end());
                struct Want { const char* key; int first, count; };   // faces[first..first+count)
                std::vector<Want> want;
                if (fv->isObject() && keys == std::vector<std::string>{"all"}) want = {{"all", 0, 6}};
                else if (fv->isObject() && keys == std::vector<std::string>{"bottom", "sides", "top"})
                    want = {{"sides", 0, 4}, {"bottom", 4, 1}, {"top", 5, 1}};
                else if (fv->isObject() && keys == six)
                    for (int i = 0; i < 6; ++i) want.push_back({kFaceKeys[i], i, 1});
                else
                    err(fv->offset, "%s\"faces\" must be {\"all\"}, {\"sides\",\"top\",\"bottom\"} or the six "
                                    "\"front\",\"back\",\"left\",\"right\",\"bottom\",\"top\"", ctx);
                for (const Want& w : want) {
                    const Json::Value* v = fv->get(w.key);
                    const Tile* t = v->isString() ? tileRef(v->str) : NULL;
                    if (!t) { err(v->offset, "%sunknown tile \"%s\"", ctx, textOr(*v)); continue; }
                    if (t->atlas != atlas) {
                        err(v->offset, "%stile \"%s\" is in atlas %d but the block draws from atlas %d", ctx, v->str.c_str(), t->atlas, atlas);
                        continue;
                    }
                    for (int i = w.first; i < w.first + w.count; ++i) faces[i] = t->index;
                }
            }
            const Json::Value* cv = b.get("color");
            uint8_t rgb[3] = {0, 0, 0};
            if (!cv) err(b.offset, "%smissing \"color\"", ctx);
            else {
                bool good = cv->isArray() && cv->arr.size() == 3;
                for (int i = 0; good && i < 3; ++i) { rgb[i] = (uint8_t)intIn(&cv->arr[i], 0, 255, &ok); good = ok; }
                if (!good) { err(cv->offset, "%s\"color\" must be [r, g, b] with each 0..255", ctx); rgb[0] = rgb[1] = rgb[2] = 0; }
            }
            int ss = 0;
            const Json::Value* sn = b.get("sounds");
            if (sn) {
                auto it = (sn->isString() && sn->str.compare(0, 5, "eden:") == 0) ? sets.find(sn->str.substr(5)) : sets.end();
                if (it == sets.end()) err(sn->offset, "%sunknown sound set \"%s\"", ctx, textOr(*sn));
                else ss = it->second;
            }
            named[sid] = true;
            st->names[sid] = "eden:" + idv->str;
            byName[st->names[sid]] = sid;
            st->info[sid] = info;
            memcpy(st->faces[sid], faces, sizeof faces);
            memcpy(st->color[sid], rgb, 3);
            st->soundSet[sid] = (uint8_t)ss;
            st->paintArt[sid] = (uint8_t)art;
            for (const char* key : {"bt", "tnt_map"}) {
                const Json::Value* v = b.get(key);
                if (v) deferred.push_back(Deferred{sid, ctxs, key, v});
            }
        }
        for (const Deferred& d : deferred) {
            const bool bt = !strcmp(d.key, "bt");
            auto it = d.v->isString() ? byName.find(d.v->str) : byName.end();
            if (bt && d.v->isNull()) st->hud[d.sid] = -1;
            else if (it == byName.end()) err(d.v->offset, "%s\"%s\": unknown block \"%s\"", d.ctx.c_str(), d.key, textOr(*d.v));
            else if (bt) st->hud[d.sid] = it->second;
            else st->tnt[d.sid] = it->second;
        }
        std::string missing;
        for (int i = 0; i < STOCK_IDS; ++i)
            if (!named[i]) missing += (missing.empty() ? "" : ", ") + std::to_string(i);
        if (!missing.empty()) err(bv->offset, "stock ids without a block: %s", missing.c_str());
    }

    // ---- aliases
    const Json::Value* av = root.get("aliases");
    if (av) {
        if (!av->isObject()) err(av->offset, "\"aliases\" must be an object");
        else for (const auto& m : av->obj) {
            auto it = m.second.isString() ? byName.find(m.second.str) : byName.end();
            if (!localOk(m.first)) err(m.second.offset, "alias \"%s\": the name must match [a-z0-9_]{1,24}", m.first.c_str());
            else if (it == byName.end()) err(m.second.offset, "alias \"%s\": unknown block \"%s\"", m.first.c_str(), textOr(m.second));
            else st->aliases.push_back(std::make_pair(m.first, it->second));
        }
    }

    // ---- picker pages
    const Json::Value* pv = root.get("picker_pages");
    if (!pv) err(root.offset, "missing \"picker_pages\"");
    else if (!pv->isArray() || pv->arr.size() != 2) err(pv->offset, "\"picker_pages\" must be an array of the 2 stock pages");
    else for (int page = 0; page < 2; ++page) {
        const Json::Value& cells = pv->arr[page];
        if (!cells.isArray() || (int)cells.arr.size() != kPageCells[page]) {
            err(cells.offset, "picker page %d must have %d cells (the stock grid)", page, kPageCells[page]);
            continue;
        }
        for (const Json::Value& c : cells.arr) {
            auto it = c.isString() ? byName.find(c.str) : byName.end();
            if (c.isString() && c.str == "tool:sign") st->pages[page].push_back(HUD_TOOL_SIGN);
            else if (c.isString() && c.str == "tool:cmd") st->pages[page].push_back(HUD_TOOL_CMD);
            else if (it != byName.end()) st->pages[page].push_back(it->second);
            else err(c.offset, "picker page %d: unknown block \"%s\"", page, textOr(c));
        }
    }

    if (err.count()) { delete st; return NULL; }
    return st;
}

void freeStock(Stock* s) { delete s; }

namespace {
std::map<std::string, std::pair<int, int>> g_edenTiles;
void setEdenTiles(const std::map<std::string, std::pair<int, int>>& t) { g_edenTiles = t; }
}  // namespace

bool edenTile(const std::string& name, int* atlas, int* index) {
    auto it = g_edenTiles.find(name);
    if (it == g_edenTiles.end()) return false;
    *atlas = it->second.first;
    *index = it->second.second;
    return true;
}

void commitStock(const Stock& s) {
    for (int t = 0; t < STOCK_IDS; ++t) {
        blockinfo[t] = s.info[t];
        memcpy(blockColor[t], s.color[t], 3);
        memcpy(blockTypeFaces[t], s.faces[t], sizeof s.faces[t]);
        hudBlocksMap[t] = s.hud[t];
        blockTntMap[t] = s.tnt[t];
        blockSoundSet[t] = s.soundSet[t];
        blockPaintArt[t] = s.paintArt[t];
    }
    memcpy(tileTwin, s.twin, sizeof tileTwin);
    memcpy(tileParticleArt, s.particleArt, sizeof tileParticleArt);
    memset(blockSoundSets, 0, sizeof blockSoundSets);
    for (size_t i = 0; i < s.sets.size(); ++i) blockSoundSets[i] = s.sets[i];
    blockSoundSetCount = (int)s.sets.size();
    std::vector<std::string> names(s.names, s.names + STOCK_IDS);
    BlockRegistry::setStock(names, s.aliases);
    setEdenTiles(s.tileNames);
    Hud::setPickerPages(s.pages[0], s.pages[1]);
    shadow_buildCasterTable();
}

bool stockMatchesLive(const Stock& s, std::string* why) {
    char b[160];
    for (int t = 0; t < STOCK_IDS; ++t) {
        const bool same = blockinfo[t] == s.info[t] && !memcmp(blockColor[t], s.color[t], 3) &&
                          !memcmp(blockTypeFaces[t], s.faces[t], sizeof s.faces[t]) && hudBlocksMap[t] == s.hud[t] &&
                          blockTntMap[t] == s.tnt[t] && blockSoundSet[t] == s.soundSet[t] && blockPaintArt[t] == s.paintArt[t] &&
                          s.names[t] == (BlockRegistry::stockName(t) ? BlockRegistry::stockName(t) : "");
        if (!same) { snprintf(b, sizeof b, "row %d differs", t); if (why) *why = b; return false; }
    }
    if (memcmp(tileTwin, s.twin, sizeof tileTwin) || memcmp(tileParticleArt, s.particleArt, sizeof tileParticleArt)) {
        if (why) *why = "tile twins differ";
        return false;
    }
    if (blockSoundSetCount != (int)s.sets.size() ||
        (!s.sets.empty() && memcmp(blockSoundSets, s.sets.data(), s.sets.size() * sizeof(BlockSoundSet)))) {
        if (why) *why = "sound sets differ";
        return false;
    }
    for (int page = 0; page < 2; ++page)
        for (int i = 0; i < Hud::pickerCellCount(page); ++i)
            if (Hud::pickerCell(page, i) != s.pages[page][i]) { if (why) *why = "picker pages differ"; return false; }
    if (BlockRegistry::aliasNames().size() != s.aliases.size()) { if (why) *why = "aliases differ"; return false; }
    return true;
}

namespace {
std::string g_stockPath;
}

const std::string& stockPath() { return g_stockPath; }

bool readPackFile(const std::string& path, std::string* out) {
    out->clear();
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    char buf[65536];
    size_t n;
    // One byte past the cap is enough for Json::parse to refuse it with its own message.
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
        out->append(buf, n);
        if (out->size() > Json::kMaxBytes) { out->resize(Json::kMaxBytes + 1); break; }
    }
    const bool ok = !ferror(f);
    fclose(f);
    return ok;
}

bool loadStock(const std::string& bundleRoot, std::vector<std::string>* errors) {
    std::vector<std::string> local;
    std::vector<std::string>* out = errors ? errors : &local;
    out->clear();
    const std::string tries[2] = {bundleRoot + "/packs/eden/pack.json", bundleRoot + "/media/packs/eden/pack.json"};
    std::string text;
    bool read = false;
    for (const std::string& p : tries) {
        g_stockPath = p;
        if (readPackFile(p, &text)) { read = true; break; }
    }
    if (!read) {
        out->push_back("eden: the stock pack is missing (looked for " + tries[0] + ")");
        return false;
    }
    Stock* s = compileStock(text.data(), text.size(), "eden", out);
    if (!s) return false;
    commitStock(*s);
    delete s;
    return true;
}

// =============================================================================================
// K.2-4: mod packs. compileMod() and port/tools/pack.py's compile_mod() check the same rules in the same
// order and print the same lines (the `mod-*` fixtures in port/tools/fixtures/content/packs).

namespace {

const char* const kModTopKeys[] = {"format", "id", "name", "version", "requires", "conflicts", "tiles", "blocks", "scripts"};
const char* const kModTileKeys[] = {"png", "unpainted"};
const char* const kModBlockKeys[] = {"id", "kind", "traits", "faces", "color", "sounds", "fallback", "fallback_paint", "picker"};
const char* const kStockOnlyBlockKeys[] = {"stock_id", "flags", "paint_art_sites", "bt", "tnt_map"};
const char* const kReservedIds[] = {"eden", "emod"};
enum { PNG_MAX_BYTES = 1 << 20, DEPS_MAX = 32 };

// K.3 (mod-manager plan M3): a "requires" entry `<id>` or `<id>>=<version>` (version: digits and dots).
bool parseRequire(const std::string& s, ModRequire* out) {
    const size_t k = s.find(">=");
    out->id = k == std::string::npos ? s : s.substr(0, k);
    out->minVersion = k == std::string::npos ? "" : s.substr(k + 2);
    if (!localOk(out->id)) return false;
    if (k == std::string::npos) return true;
    const std::string& v = out->minVersion;
    if (v.empty() || v.size() > 24 || v.front() == '.' || v.back() == '.') return false;
    for (size_t i = 0; i < v.size(); ++i)
        if (!(isdigit((unsigned char)v[i]) || (v[i] == '.' && v[i - 1] != '.'))) return false;
    return true;
}

std::string hexOf(const uint8_t* p, size_t n) {
    static const char* hx = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; ++i) { s += hx[p[i] >> 4]; s += hx[p[i] & 15]; }
    return s;
}

// A pack-relative path: no root, drive, backslash, empty, `.` or `..` component.
bool relPathOk(const std::string& p) {
    if (p.empty() || p.size() > 200 || p[0] == '/') return false;
    size_t at = 0;
    while (at <= p.size()) {
        size_t e = p.find('/', at);
        if (e == std::string::npos) e = p.size();
        const std::string c = p.substr(at, e - at);
        if (c.empty() || c == "." || c == "..") return false;
        at = e + 1;
    }
    for (char c : p)
        if (c == '\\' || c == ':' || (unsigned char)c < 0x20) return false;
    return true;
}

// The PNG signature and IHDR (the first chunk): width and height. False: not a PNG.
bool pngSize(const std::string& b, int* w, int* h) {
    static const unsigned char sig[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    if (b.size() < 33 || memcmp(b.data(), sig, 8) || memcmp(b.data() + 12, "IHDR", 4)) return false;
    const unsigned char* u = (const unsigned char*)b.data();
    auto be32 = [&](size_t o) { return (uint32_t)u[o] << 24 | (uint32_t)u[o + 1] << 16 | (uint32_t)u[o + 2] << 8 | u[o + 3]; };
    if (be32(8) != 13) return false;
    const uint32_t ww = be32(16), hh = be32(20);
    *w = ww > 100000 ? 100000 : (int)ww;
    *h = hh > 100000 ? 100000 : (int)hh;
    return true;
}

}  // namespace

bool compileMod(const char* data, size_t len, const std::string& dir, const std::string& label, ModPack* pk) {
    pk->lines.clear();
    pk->tiles.clear();
    pk->blocks.clear();
    pk->needs.clear();
    pk->conflicts.clear();
    pk->scripts.clear();
    pk->blocksInFile = 0;
    pk->refused = true;
    // K.3 (plan M5): the content hash: pack.json's bytes, then each readable tile PNG's bytes in file order.
    emod::Sha256 sha;
    sha.update(data, len);
    struct HashOut {
        emod::Sha256& s; std::string& out;
        ~HashOut() { uint8_t d[32]; s.final(d); out = hexOf(d, 32); }
    } hashOut{sha, pk->hash};
    Errors err{label.c_str(), &pk->lines};
    Json::Value root;
    Json::Error je;
    if (!Json::parse(data, len, &root, &je)) { err(je.offset, "JSON: %s", je.message.c_str()); return false; }
    if (!root.isObject()) { err(root.offset, "pack.json must be an object"); return false; }
    bool top = true;   // no top-level error so far
    for (const auto& m : root.obj)
        if (indexOf(kModTopKeys, m.first) < 0) err(m.second.offset, "warning: unknown key \"%s\" (ignored)", m.first.c_str());
    bool ok;
    const Json::Value* fmt = root.get("format");
    if (!fmt) { err(root.offset, "missing \"format\""); top = false; }
    else { intIn(fmt, 1, 1, &ok); if (!ok) { err(fmt->offset, "\"format\" must be 1 (this build reads format 1)"); top = false; } }
    const Json::Value* pid = root.get("id");
    if (!pid) { err(root.offset, "missing \"id\""); top = false; }
    else if (!pid->isString() || !localOk(pid->str)) { err(pid->offset, "\"id\" must match [a-z0-9_]{1,24}"); top = false; }
    else if (indexOf(kReservedIds, pid->str) >= 0) { err(pid->offset, "the id \"%s\" is reserved", pid->str.c_str()); top = false; }
    else pk->id = pid->str;
    for (const char* k : {"name", "version"}) {
        const Json::Value* v = root.get(k);
        if (v && !v->isString()) { err(v->offset, "\"%s\" must be a string", k); top = false; }
        else if (v) (k[0] == 'n' ? pk->name : pk->version) = v->str;
    }
    // K.3: dependencies. A bad list refuses the pack (a top-level error): loading it without its
    // dependency would be the failure the list exists to prevent.
    for (const char* k : {"requires", "conflicts"}) {
        const Json::Value* v = root.get(k);
        if (!v) continue;
        if (!v->isArray()) { err(v->offset, "\"%s\" must be an array of pack ids", k); top = false; continue; }
        if (v->arr.size() > DEPS_MAX) { err(v->offset, "\"%s\" has %zu entries (at most %d)", k, v->arr.size(), (int)DEPS_MAX); top = false; continue; }
        for (const Json::Value& e : v->arr) {
            ModRequire r;
            const bool req = k[0] == 'r';
            if (!e.isString() || !(req ? parseRequire(e.str, &r) : localOk(e.str))) {
                err(e.offset, req ? "\"requires\" entry \"%s\": must be <id> or <id>>=<version> (digits and dots)"
                                  : "\"conflicts\" entry \"%s\": must be a pack id", textOr(e));
                top = false;
                continue;
            }
            if (!req) r.id = e.str;
            if (!pk->id.empty() && r.id == pk->id) { err(e.offset, "\"%s\" names the pack itself", k); top = false; continue; }
            (req ? pk->needs : pk->conflicts).push_back(r);
        }
    }
    const Json::Value* tv = root.get("tiles");
    if (tv && !tv->isObject()) { err(tv->offset, "\"tiles\" must be an object"); top = false; }
    // K.4 (mod-scripting plan S2): Lua sources, read whole now. A bad entry refuses the pack (its other
    // scripts would run without it). A pack with scripts may have no "blocks".
    const Json::Value* sv = root.get("scripts");
    if (sv) {
        size_t total = 0;
        if (!sv->isArray()) { err(sv->offset, "\"scripts\" must be an array of paths"); top = false; }
        else if (sv->arr.size() > SCRIPTS_MAX) { err(sv->offset, "\"scripts\" has %zu entries (at most %d)", sv->arr.size(), (int)SCRIPTS_MAX); top = false; }
        else for (const Json::Value& e : sv->arr) {
            const std::string& p = e.str;
            if (!e.isString() || !relPathOk(p) || p.size() < 5 || p.compare(p.size() - 4, 4, ".lua") != 0) {
                err(e.offset, "script \"%s\": must be a relative path inside the pack ending in .lua", textOr(e)); top = false; continue;
            }
            bool twice = false;
            for (const ModScriptSrc& o : pk->scripts) twice |= o.path == p;
            if (twice) { err(e.offset, "script \"%s\": listed twice", p.c_str()); top = false; continue; }
            std::string bytes;
            FILE* f = fopen((dir + "/" + p).c_str(), "rb");
            bool readOk = f != NULL;
            if (f) {
                char buf[65536];
                size_t n;
                while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
                    bytes.append(buf, n);
                    if (bytes.size() > SCRIPT_MAX_BYTES) break;
                }
                readOk = !ferror(f);
                fclose(f);
            }
            if (!readOk) { err(e.offset, "script \"%s\": cannot read it", p.c_str()); top = false; continue; }
            if (bytes.size() > SCRIPT_MAX_BYTES) { err(e.offset, "script \"%s\": larger than 256 KiB", p.c_str()); top = false; continue; }
            if (!bytes.empty() && bytes[0] == 0x1b) { err(e.offset, "script \"%s\": precompiled Lua (only source scripts load)", p.c_str()); top = false; continue; }
            total += bytes.size();
            pk->scripts.push_back(ModScriptSrc{p, bytes});
        }
        if (total > SCRIPTS_MAX_BYTES) { err(sv->offset, "\"scripts\" add up to more than 1 MiB"); top = false; }
    }
    const Json::Value* bv = root.get("blocks");
    if (!bv) { if (!sv) { err(root.offset, "missing \"blocks\""); top = false; } }
    else if (!bv->isArray()) { err(bv->offset, "\"blocks\" must be an array"); top = false; }
    else if (bv->arr.size() > BLOCKS_MAX) { err(bv->offset, "\"blocks\" has %zu blocks (at most %d)", bv->arr.size(), (int)BLOCKS_MAX); top = false; }
    if (!top) return false;

    // ---- tiles: a 32x32 PNG each, and an optional unpainted twin (another tile of this pack)
    std::map<std::string, int> tiles;   // name -> pack->tiles index
    if (tv) {
        for (const auto& m : tv->obj) {
            const std::string& name = m.first;
            const Json::Value& t = m.second;
            const char* nm = name.c_str();
            if (!localOk(name)) { err(t.offset, "tile \"%s\": the name must match [a-z0-9_]{1,24}", nm); continue; }
            if (!t.isObject()) { err(t.offset, "tile \"%s\": must be an object", nm); continue; }
            for (const auto& k : t.obj)
                if (indexOf(kModTileKeys, k.first) < 0) err(k.second.offset, "warning: tile \"%s\": unknown key \"%s\" (ignored)", nm, k.first.c_str());
            const Json::Value* png = t.get("png");
            if (!png) { err(t.offset, "tile \"%s\": missing \"png\"", nm); continue; }
            if (!png->isString() || !relPathOk(png->str)) { err(png->offset, "tile \"%s\": \"png\" must be a relative path inside the pack", nm); continue; }
            const char* pp = png->str.c_str();
            std::string bytes;
            FILE* f = fopen((dir + "/" + png->str).c_str(), "rb");
            bool readOk = f != NULL;
            if (f) {
                char buf[65536];
                size_t n;
                while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
                    bytes.append(buf, n);
                    if (bytes.size() > PNG_MAX_BYTES) break;
                }
                readOk = !ferror(f);
                fclose(f);
            }
            if (!readOk) { err(png->offset, "tile \"%s\": cannot read \"%s\"", nm, pp); continue; }
            sha.update(png->str.data(), png->str.size() + 1);   // the path and its NUL, then the bytes
            sha.update(bytes.data(), bytes.size());
            if (bytes.size() > PNG_MAX_BYTES) { err(png->offset, "tile \"%s\": \"%s\" is larger than 1 MiB", nm, pp); continue; }
            int w = 0, h = 0;
            if (!pngSize(bytes, &w, &h)) { err(png->offset, "tile \"%s\": \"%s\" is not a PNG", nm, pp); continue; }
            if (w != TILE_PX || h != TILE_PX) { err(png->offset, "tile \"%s\": \"%s\" is %dx%d (a tile is 32x32)", nm, pp, w, h); continue; }
            EdenImage* img = eden_image_decode(bytes.data(), bytes.size());
            if (!img || img->width != TILE_PX || img->height != TILE_PX) {
                eden_image_free(img);
                err(png->offset, "tile \"%s\": \"%s\" does not decode", nm, pp);
                continue;
            }
            ModTileSrc src;
            src.name = name;
            src.rgba.assign(img->rgba, img->rgba + TILE_PX * TILE_PX * 4);
            src.twin = -1;
            eden_image_free(img);
            tiles[name] = (int)pk->tiles.size();
            pk->tiles.push_back(src);
        }
        std::set<std::string> badTwin;
        for (const auto& m : tv->obj) {
            auto it = tiles.find(m.first);
            if (it == tiles.end()) continue;
            const Json::Value* u = m.second.get("unpainted");
            if (!u) continue;
            auto tw = u->isString() ? tiles.find(u->str) : tiles.end();
            if (tw == tiles.end()) { err(u->offset, "tile \"%s\": unknown unpainted tile \"%s\"", m.first.c_str(), textOr(*u)); badTwin.insert(m.first); }
            else pk->tiles[it->second].twin = tw->second;
        }
        for (const std::string& b : badTwin) tiles.erase(b);   // the tile is unusable: its blocks say "unknown tile"
    }

    // ---- blocks
    const std::string own = pk->id + ":";
    std::set<std::string> ids;
    // K.4: the scripts join the hash after the tiles (path + NUL + bytes, list order).
    for (const ModScriptSrc& sc : pk->scripts) {
        sha.update(sc.path.data(), sc.path.size() + 1);
        sha.update(sc.source.data(), sc.source.size());
    }
    pk->blocksInFile = bv ? bv->arr.size() : 0;
    for (size_t n = 0; n < pk->blocksInFile; ++n) {
        const Json::Value& b = bv->arr[n];
        if (!b.isObject()) { err(b.offset, "block #%zu: must be an object", n); continue; }
        const Json::Value* idv = b.get("id");
        if (!(idv && idv->isString() && blockIdOk(idv->str))) {
            err(idv ? idv->offset : b.offset, "block #%zu: \"id\" must match [a-z0-9_/]{1,48}", n);
            continue;
        }
        const std::string ctxs = "block \"" + idv->str + "\": ";
        const char* ctx = ctxs.c_str();
        const size_t before = err.count();
        size_t warnings = 0;
        for (const auto& k : b.obj) {
            if (indexOf(kStockOnlyBlockKeys, k.first) >= 0) err(k.second.offset, "%s\"%s\" is for the stock pack only", ctx, k.first.c_str());
            else if (indexOf(kModBlockKeys, k.first) < 0) { err(k.second.offset, "warning: %sunknown key \"%s\" (ignored)", ctx, k.first.c_str()); ++warnings; }
        }
        if (!ids.insert(idv->str).second) { err(idv->offset, "%sduplicate block id", ctx); continue; }
        const Json::Value* kind = b.get("kind");
        if (kind && (!kind->isString() || indexOf(kKinds, kind->str) < 0)) err(kind->offset, "%sunknown kind \"%s\"", ctx, textOr(*kind));
        else if (kind && kind->str != "cube") err(kind->offset, "%skind \"%s\" is for the stock pack only (a mod block is \"cube\")", ctx, kind->str.c_str());
        ModBlockSrc mb;
        mb.id = idv->str;
        int info = IS_NOTSOLID, art = 0;
        std::set<std::string> seen;
        const Json::Value* tr = b.get("traits");
        if (tr) {
            if (!tr->isArray()) err(tr->offset, "%s\"traits\" must be an array of names", ctx);
            else for (const Json::Value& t : tr->arr) {
                const NameBits* nb = t.isString() ? findBits(kTraits, t.str) : NULL;
                if (!nb) err(t.offset, "%sunknown trait \"%s\"", ctx, textOr(t));
                else if (!seen.insert(t.str).second) err(t.offset, "%strait \"%s\" listed twice", ctx, t.str.c_str());
                else if (t.str == "solid") info &= ~IS_NOTSOLID;
                else if (t.str == "paint_art") art = ART_ALL;
                else info |= nb->bits;
            }
        }
        const int atlas = (info & IS_ATLAS2) ? 2 : 1;
        for (ModFace& f : mb.faces) { f.eden = 0; f.local = -1; }
        const Json::Value* fv = b.get("faces");
        if (!fv) err(b.offset, "%smissing \"faces\"", ctx);
        else {
            std::vector<std::string> keys;
            if (fv->isObject()) for (const auto& k : fv->obj) keys.push_back(k.first);
            std::sort(keys.begin(), keys.end());
            std::vector<std::string> six(kFaceKeys, kFaceKeys + 6);
            std::sort(six.begin(), six.end());
            struct Want { const char* key; int first, count; };
            std::vector<Want> want;
            if (fv->isObject() && keys == std::vector<std::string>{"all"}) want = {{"all", 0, 6}};
            else if (fv->isObject() && keys == std::vector<std::string>{"bottom", "sides", "top"})
                want = {{"sides", 0, 4}, {"bottom", 4, 1}, {"top", 5, 1}};
            else if (fv->isObject() && keys == six)
                for (int i = 0; i < 6; ++i) want.push_back({kFaceKeys[i], i, 1});
            else
                err(fv->offset, "%s\"faces\" must be {\"all\"}, {\"sides\",\"top\",\"bottom\"} or the six "
                                "\"front\",\"back\",\"left\",\"right\",\"bottom\",\"top\"", ctx);
            for (const Want& w : want) {
                const Json::Value* v = fv->get(w.key);
                ModFace face = {0, -1};
                bool found = false;
                if (v->isString() && v->str.compare(0, 5, "eden:") == 0) {
                    int ta = 0, ti = 0;
                    if ((found = edenTile(v->str.substr(5), &ta, &ti))) {
                        if (ta != atlas) {
                            err(v->offset, "%stile \"%s\" is in atlas %d but the block draws from atlas %d", ctx, v->str.c_str(), ta, atlas);
                            continue;
                        }
                        face.eden = ti;
                    }
                } else if (v->isString()) {
                    const std::string local = v->str.compare(0, own.size(), own) == 0 ? v->str.substr(own.size()) : v->str;
                    auto it = tiles.find(local);
                    if ((found = it != tiles.end())) face.local = it->second;
                }
                if (!found) { err(v->offset, "%sunknown tile \"%s\"", ctx, textOr(*v)); continue; }
                for (int i = w.first; i < w.first + w.count; ++i) mb.faces[i] = face;
            }
        }
        const Json::Value* cv = b.get("color");
        if (!cv) err(b.offset, "%smissing \"color\"", ctx);
        else {
            bool good = cv->isArray() && cv->arr.size() == 3;
            for (int i = 0; good && i < 3; ++i) { mb.color[i] = (uint8_t)intIn(&cv->arr[i], 0, 255, &ok); good = ok; }
            if (!good) err(cv->offset, "%s\"color\" must be [r, g, b] with each 0..255", ctx);
        }
        mb.soundSet = 0;
        const Json::Value* sn = b.get("sounds");
        if (sn) {
            int found = -1;
            if (sn->isString())
                for (int i = 0; i < blockSoundSetCount; ++i) if (sn->str == blockSoundSets[i].name) { found = i; break; }
            if (found < 0) err(sn->offset, "%sunknown sound set \"%s\"", ctx, textOr(*sn));
            else mb.soundSet = (uint8_t)found;
        }
        const Json::Value* fb = b.get("fallback");
        if (!fb) err(b.offset, "%smissing \"fallback\"", ctx);
        else {
            const int t = fb->isString() && fb->str.compare(0, 5, "eden:") == 0 ? BlockRegistry::stockByName(fb->str) : -1;
            if (!(t >= 1 && t <= STOCK_BLOCK_MAX && !(t >= 66 && t <= 70) && !(t >= 75 && t <= 80)))
                err(fb->offset, "%s\"fallback\" must name a placeable stock block (\"eden:...\"; not air, a door or portal half, or custom)", ctx);
            else mb.fallbackType = (uint8_t)t;
        }
        mb.fallbackPaint = 0;
        const Json::Value* fp = b.get("fallback_paint");
        if (fp) {
            const int64_t p = intIn(fp, 0, 54, &ok);
            if (!ok) err(fp->offset, "%s\"fallback_paint\" must be 0..54", ctx);
            else mb.fallbackPaint = (uint8_t)p;
        }
        mb.picker = true;
        const Json::Value* pv = b.get("picker");
        if (pv) {
            if (!pv->isBool()) err(pv->offset, "%s\"picker\" must be true or false", ctx);
            else mb.picker = pv->b;
        }
        if (err.count() - before != warnings) continue;   // an error: the block is skipped
        mb.info = info;
        mb.paintArt = (uint8_t)art;
        pk->blocks.push_back(mb);
    }
    pk->refused = false;
    return true;
}

// ---- the loader ------------------------------------------------------------------------------
namespace {

std::vector<std::string> g_extraDirs;
ModReport g_report;
std::vector<std::string> g_registered;   // the defs the last loadMods registered (others are left alone)

void loaderLine(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void loaderLine(const char* fmt, ...) {
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    g_report.lines.push_back(b);
}

std::string baseName(const std::string& p) {
    std::string s = p;
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    const size_t k = s.find_last_of('/');
    return k == std::string::npos ? s : s.substr(k + 1);
}

// The pack folders in scan order (D15): each extra dir (a pack, or a directory of packs) then
// Documents/Packs, folders by name, bytewise (scanPacks then orders the Packs/ ones by the enable list).
struct PackDir { std::string dir; bool extra; };
std::vector<PackDir> packDirs(const std::string& documents) {
    std::vector<PackDir> out;
    auto addRoot = [&](const std::string& root, bool extra) {
        if (extra && eden_fs_exists(root + "/pack.json")) { out.push_back(PackDir{root, true}); return; }
        if (!eden_fs_is_dir(root)) return;
        std::vector<std::string> names = eden_fs_list(root);
        std::sort(names.begin(), names.end());
        for (const std::string& n : names) {
            if (n.empty() || n[0] == '.') continue;
            const std::string d = root + "/" + n;
            if (eden_fs_is_dir(d)) out.push_back(PackDir{d, extra});
        }
    };
    for (const std::string& d : g_extraDirs) addRoot(d, true);
    if (!documents.empty()) addRoot(documents + "/Packs", false);
    return out;
}

std::string modListPath(const std::string& documents) { return documents + "/Packs/mods.json"; }

int findLoaded(const std::vector<ModPack>& v, const std::string& id) {
    for (size_t i = 0; i < v.size(); ++i) if (v[i].state == PACK_LOADED && v[i].id == id) return (int)i;
    return -1;
}

// M3: requirements to a fixpoint (a skipped pack can break the packs that need it). port/tools/pack.py
// resolve_mods() is the same walk with the same lines.
void resolveRequires(std::vector<ModPack>& v, std::vector<std::string>* lines) {
    for (bool changed = true; changed;) {
        changed = false;
        for (ModPack& pk : v) {
            if (pk.state != PACK_LOADED) continue;
            for (const ModRequire& r : pk.needs) {
                const int q = findLoaded(v, r.id);
                std::string why;
                if (q < 0) {
                    bool there = false;
                    for (const ModPack& o : v) there |= o.id == r.id;
                    why = "needs \"" + r.id + "\" (" + (there ? "not loaded" : "not installed") + ")";
                } else if (!r.minVersion.empty() && compareVersions(v[q].version, r.minVersion) < 0) {
                    why = "needs \"" + r.id + "\" " + r.minVersion + " or newer (found \"" + v[q].version + "\")";
                }
                if (why.empty()) continue;
                pk.state = PACK_NEEDS;
                pk.refused = true;
                pk.why = why;
                lines->push_back(pk.folder + ": " + why + " (skipped)");
                changed = true;
                break;
            }
        }
    }
}

}  // namespace

int compareVersions(const std::string& a, const std::string& b) {
    size_t i = 0, j = 0;
    while (i < a.size() || j < b.size()) {
        auto part = [](const std::string& s, size_t* at) {
            long long n = 0;
            size_t k = *at;
            while (k < s.size() && isdigit((unsigned char)s[k]) && n < 1000000000LL) n = n * 10 + (s[k++] - '0');
            while (k < s.size() && s[k] != '.') ++k;
            *at = k < s.size() ? k + 1 : k;
            return n;
        };
        const long long x = part(a, &i), y = part(b, &j);
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;
}

std::vector<ModListEntry> readModList(const std::string& documents, std::string* problem) {
    std::vector<ModListEntry> out;
    if (problem) problem->clear();
    const std::string path = modListPath(documents);
    if (documents.empty() || !eden_fs_exists(path)) return out;
    std::string text;
    Json::Value root;
    Json::Error je;
    auto bad = [&](const std::string& why) { if (problem) *problem = why; out.clear(); return out; };
    if (!readPackFile(path, &text)) return bad("cannot read it");
    if (!Json::parse(text.data(), text.size(), &root, &je)) return bad("JSON at byte " + std::to_string(je.offset) + ": " + je.message);
    const Json::Value* pv = root.isObject() ? root.get("packs") : NULL;
    if (!pv || !pv->isArray()) return bad("no \"packs\" array");
    std::set<std::string> seen;
    for (const Json::Value& e : pv->arr) {
        const Json::Value* id = e.isObject() ? e.get("id") : NULL;
        const Json::Value* en = e.isObject() ? e.get("enabled") : NULL;
        if (!id || !id->isString() || !localOk(id->str) || (en && !en->isBool())) continue;   // a bad entry is dropped
        if (!seen.insert(id->str).second) continue;
        out.push_back(ModListEntry{id->str, en ? en->b : true});
    }
    return out;
}

bool writeModList(const std::string& documents, const std::vector<ModListEntry>& list) {
    if (documents.empty() || !eden_fs_make_dirs(documents + "/Packs")) return false;
    std::string t = "{\"format\": 1, \"packs\": [";
    for (size_t i = 0; i < list.size(); ++i)
        t += std::string(i ? ",\n  " : "\n  ") + "{\"id\": \"" + list[i].id + "\", \"enabled\": " + (list[i].enabled ? "true" : "false") + "}";
    t += "\n]}\n";
    return eden_fs_write_atomic(modListPath(documents), t.data(), t.size());   // QA.17: + fsync, no copy fallback
}

std::vector<ModPack> scanPacks(const std::string& documents, std::vector<std::string>* lines) {
    std::vector<ModPack> v;
    std::string problem;
    const std::vector<ModListEntry> list = readModList(documents, &problem);
    if (!problem.empty()) lines->push_back("mods.json: " + problem + " (ignored: every pack is enabled)");
    for (const PackDir& pd : packDirs(documents)) {
        ModPack pk;
        pk.dir = pd.dir;
        pk.folder = baseName(pd.dir);
        pk.fromExtraDir = pd.extra;
        std::string text;
        if (!readPackFile(pd.dir + "/pack.json", &text)) {
            lines->push_back(pk.folder + ": no pack.json (skipped)");
            continue;
        }
        compileMod(text.data(), text.size(), pd.dir, pk.folder, &pk);
        pk.state = pk.refused ? PACK_REFUSED : PACK_LOADED;
        v.push_back(std::move(pk));
    }
    // The enable list orders the Packs/ folders (listed ones in list order, then the rest by name; stable)
    // and switches packs off. --packs= folders stay first.
    auto rank = [&](const ModPack& pk) -> size_t {
        if (pk.fromExtraDir) return 0;
        for (size_t i = 0; i < list.size(); ++i) if (!pk.id.empty() && list[i].id == pk.id) return i + 1;
        return list.size() + 1;
    };
    std::stable_sort(v.begin(), v.end(), [&](const ModPack& a, const ModPack& b) { return rank(a) < rank(b); });
    for (ModPack& pk : v) {
        const size_t r = rank(pk);
        if (pk.state == PACK_LOADED && r >= 1 && r <= list.size() && !list[r - 1].enabled) {
            pk.state = PACK_DISABLED;
            pk.refused = true;
        }
    }
    std::map<std::string, std::string> idFolder;   // pack id -> the folder that loaded it
    for (ModPack& pk : v) {
        if (pk.state != PACK_LOADED) continue;
        if (idFolder.count(pk.id)) {
            lines->push_back(pk.folder + ": the pack id \"" + pk.id + "\" is already loaded from " + idFolder[pk.id] + " (skipped)");
            pk.refused = true;
            pk.state = PACK_REFUSED;
        } else idFolder[pk.id] = pk.folder;
    }
    resolveRequires(v, lines);
    // Conflicts (either side may declare one): the pack earlier in load order wins.
    for (size_t i = 0; i < v.size(); ++i) {
        ModPack& pk = v[i];
        if (pk.state != PACK_LOADED) continue;
        for (size_t j = 0; j < i; ++j) {
            const ModPack& q = v[j];
            if (q.state != PACK_LOADED) continue;
            bool c = false;
            for (const ModRequire& r : pk.conflicts) c |= r.id == q.id;
            for (const ModRequire& r : q.conflicts) c |= r.id == pk.id;
            if (!c) continue;
            pk.state = PACK_CONFLICT;
            pk.refused = true;
            pk.why = "conflicts with \"" + q.id + "\"";
            lines->push_back(pk.folder + ": " + pk.why + " (skipped)");
            break;
        }
    }
    resolveRequires(v, lines);
    return v;
}

namespace {
// The installed (Packs/) packs in scan order, merged with the list's absent entries (kept at the end).
std::vector<ModListEntry> currentList(const std::string& documents, const std::vector<ModPack>& v) {
    std::vector<ModListEntry> out;
    std::set<std::string> have;
    for (const ModPack& pk : v)
        if (!pk.fromExtraDir && !pk.id.empty() && have.insert(pk.id).second)
            out.push_back(ModListEntry{pk.id, pk.state != PACK_DISABLED});
    for (const ModListEntry& e : readModList(documents)) if (have.insert(e.id).second) out.push_back(e);
    return out;
}
}  // namespace

bool setPackEnabled(const std::string& documents, const std::string& id, bool enabled) {
    std::vector<std::string> lines;
    std::vector<ModListEntry> list = currentList(documents, scanPacks(documents, &lines));
    for (ModListEntry& e : list)
        if (e.id == id) { e.enabled = enabled; return writeModList(documents, list); }
    return false;
}

bool movePack(const std::string& documents, const std::string& id, int delta) {
    std::vector<std::string> lines;
    const std::vector<ModPack> v = scanPacks(documents, &lines);
    std::vector<ModListEntry> list = currentList(documents, v);
    size_t installed = 0;   // only installed packs move, among themselves
    std::set<std::string> ids;
    for (const ModPack& pk : v) if (!pk.fromExtraDir && !pk.id.empty() && ids.insert(pk.id).second) ++installed;
    for (size_t i = 0; i < installed; ++i) {
        if (list[i].id != id) continue;
        const long j = (long)i + (delta < 0 ? -1 : 1);
        if (j < 0 || j >= (long)installed) return false;
        std::swap(list[i], list[(size_t)j]);
        return writeModList(documents, list);
    }
    return false;
}

bool removePackFolder(const std::string& documents, const std::string& folder) {
    if (documents.empty() || folder.empty() || folder[0] == '.' || folder.find('/') != std::string::npos ||
        folder.find('\\') != std::string::npos)
        return false;
    const std::string d = documents + "/Packs/" + folder;
    if (!eden_fs_is_dir(d) || !eden_fs_exists(d + "/pack.json")) return false;
    return eden_fs_remove_tree(d);
}

void setExtraPackDirs(const std::vector<std::string>& dirs) { g_extraDirs = dirs; }
const ModReport& modReport() { return g_report; }

std::string modsHash() {
    std::string text;
    for (const ModPack& pk : g_report.packs)
        if (pk.state == PACK_LOADED) text += pk.id + " " + pk.version + " " + pk.hash + "\n";
    if (text.empty()) return "";
    emod::Sha256 sha;
    sha.update(text.data(), text.size());
    uint8_t d[32];
    sha.final(d);
    return hexOf(d, 32);
}

std::vector<WorldMod> worldMods(const std::string& documents) {
    std::map<std::string, int> count;
    for (int id = STOCK_BLOCK_MAX + 1; id < BLOCK_IDS; ++id) {
        const char* n = BlockRegistry::nameOf(id);
        const char* c = n ? strchr(n, ':') : NULL;
        if (c) count[std::string(n, c - n)]++;
    }
    std::vector<WorldMod> out;
    std::vector<std::string> lines;
    std::vector<ModPack> installed;
    bool scanned = false;
    for (const auto& kv : count) {
        WorldMod m{kv.first, WMOD_MISSING, kv.second};
        for (const ModPack& pk : g_report.packs) if (pk.state == PACK_LOADED && pk.id == kv.first) m.state = WMOD_LOADED;
        if (m.state == WMOD_MISSING) {
            if (!scanned) { installed = scanPacks(documents, &lines); scanned = true; }
            for (const ModPack& pk : installed) if (pk.id == kv.first) m.state = WMOD_DISABLED;
        }
        out.push_back(m);
    }
    return out;
}

void loadMods(const std::string& documents, bool session) {
    for (const std::string& n : g_registered) BlockRegistry::unregisterDef(n);
    g_registered.clear();
    g_report = ModReport();
    g_report.ran = true;
    g_report.session = session;
    for (int i = ATLAS1_FREE_FIRST; i < ATLAS_TILES; ++i) { tileTwin[i] = 0; tileParticleArt[i] = 0; }
    struct Placed { int atlas, index, pack, tile; };
    std::vector<Placed> placed;   // resolved to pixels once g_report.packs stops moving
    if (!session) {
        g_report.packs = scanPacks(documents, &g_report.lines);
        // Tile slots: (pack, local tile, atlas) -> index. Allocation is first use, block by block (D15).
        std::map<std::pair<std::pair<int, int>, int>, int> slot;
        int next[3] = {0, ATLAS1_FREE_FIRST, ATLAS2_FREE_FIRST};
        for (int pi = 0; pi < (int)g_report.packs.size(); ++pi) {
            const ModPack& pk = g_report.packs[pi];
            if (pk.state == PACK_LOADED) {
                for (const ModBlockSrc& mb : pk.blocks) {
                    const int atlas = (mb.info & IS_ATLAS2) ? 2 : 1;
                    // The new slots this block needs (its local tiles, and in atlas 1 their twins).
                    std::vector<std::pair<int, int>> need;   // (local tile, atlas)
                    auto want = [&](int local, int a) {
                        const std::pair<int, int> k(local, a);
                        if (!slot.count(std::make_pair(std::make_pair(pi, local), a)) && std::find(need.begin(), need.end(), k) == need.end())
                            need.push_back(k);
                    };
                    for (const ModFace& f : mb.faces) {
                        if (f.local < 0) continue;
                        want(f.local, atlas);
                        if (atlas == 1 && pk.tiles[f.local].twin >= 0) want(pk.tiles[f.local].twin, 1);
                    }
                    int count[3] = {0, 0, 0};   // slots: an atlas2 tile is an 8-frame strip
                    for (const auto& k : need) count[k.second] += k.second == 2 ? ATLAS2_FRAMES : 1;
                    BlockRegistry::BlockDef def;
                    def.name = pk.id + ":" + mb.id;
                    def.fallbackType = mb.fallbackType;
                    def.fallbackPaint = mb.fallbackPaint;
                    def.info = mb.info;
                    memcpy(def.color, mb.color, 3);
                    def.hudMap = -1;
                    def.tntMap = 0;
                    def.soundSet = mb.soundSet;
                    def.paintArt = mb.paintArt;
                    bool missingTex = false;
                    if (next[1] + count[1] > ATLAS_TILES || next[2] + count[2] > ATLAS_TILES) {
                        missingTex = true;   // D10: over budget -> the missing texture, drawn from atlas 1
                        def.info &= ~IS_ATLAS2;
                        for (int i = 0; i < 6; ++i) def.faces[i] = TEX_MISSING;
                        loaderLine("%s: block \"%s\": no free atlas tile (%d opaque and %d transparent are taken): "
                                   "drawn as the missing texture", pk.folder.c_str(), mb.id.c_str(),
                                   ATLAS_TILES - ATLAS1_FREE_FIRST, (ATLAS_TILES - ATLAS2_FREE_FIRST) / ATLAS2_FRAMES);
                    } else {
                        for (const auto& k : need) {
                            const int at = next[k.second];
                            next[k.second] += k.second == 2 ? ATLAS2_FRAMES : 1;
                            slot[std::make_pair(std::make_pair(pi, k.first), k.second)] = at;
                            placed.push_back(Placed{k.second, at, pi, k.first});
                            g_report.tiles.push_back(LoadedTile{k.second, at, pk.id + ":" + pk.tiles[k.first].name});
                        }
                        for (int i = 0; i < 6; ++i) {
                            const ModFace& f = mb.faces[i];
                            def.faces[i] = f.local < 0 ? f.eden : slot[std::make_pair(std::make_pair(pi, f.local), atlas)];
                        }
                        if (atlas == 1)
                            for (const ModFace& f : mb.faces)
                                if (f.local >= 0 && pk.tiles[f.local].twin >= 0)
                                    tileTwin[slot[std::make_pair(std::make_pair(pi, f.local), 1)]] =
                                        (uint8_t)slot[std::make_pair(std::make_pair(pi, pk.tiles[f.local].twin), 1)];
                    }
                    if (BlockRegistry::registerDef(def)) {
                        g_registered.push_back(def.name);
                        g_report.blocks.push_back(LoadedBlock{def.name, pi, mb.picker, missingTex});
                    }
                }
            }
        }
    }
    std::vector<Resources::AtlasTile> upload;
    for (const Placed& p : placed)
        for (int f = 0; f < (p.atlas == 2 ? (int)ATLAS2_FRAMES : 1); ++f)   // the 8 frames of an atlas2 strip
            upload.push_back(Resources::AtlasTile{p.atlas, p.index + f, g_report.packs[p.pack].tiles[p.tile].rgba.data()});
    // Resources copies the pixels; the report keeps only the tile names.
    if (Resources::getResources) Resources::getResources->setModTiles(upload);
    for (ModPack& pk : g_report.packs) for (ModTileSrc& t : pk.tiles) std::vector<uint8_t>().swap(t.rgba);
    for (const ModPack& pk : g_report.packs)
        for (const std::string& l : pk.lines) fprintf(stderr, "[pack] %s\n", l.c_str());
    for (const std::string& l : g_report.lines) fprintf(stderr, "[pack] %s\n", l.c_str());
    if (!g_report.packs.empty())
        fprintf(stderr, "[pack] mod packs: %zu folder(s), %zu block(s), %zu tile(s)\n", g_report.packs.size(),
                g_report.blocks.size(), g_report.tiles.size());
}

}  // namespace ContentPack
