// K.2-4: --packs-selftest, mod packs at run time (WORKING/data-driven-content-plan-2026-11-01.md §6 "Mod runtime").
// Headless; give it a fresh --docs. Mode group "packs".
//
//   1. the bundled example pack (packs/emod_example) copied into --docs/Packs, beside an `eden` impostor, a broken
//      pack.json, a folder without one and a second pack with the example's id: the example loads (6 blocks,
//      tiles 66..70 + atlas2 32, the oak twin), the others are refused with their lines; every trait reached
//      its row; both atlases were re-uploaded once and hold the PNGs' pixels
//   2. each block placed by the console (one painted): ids 128..133 in order, meshed; `packs` lists them
//   3. save -> reload: same bytes, paint, mesh; no atlas re-upload (the tile set did not change)
//   4. the pack folder moved away -> reload: placeholders, bytes + paint unchanged, stock atlases back;
//      an edit + save leaves the mod cells and the palette alone
//   5. restored -> reload: the original mesh is back
//   6. a pack with 60 own opaque tiles and 12 transparent ones after it: 57 fit (71..127) and 11 (8-slot strips
//      40..127), the rest draw TEX_MISSING, the example's tiles do not move, the next load allocates the same (D15)
//   7. a session loads none (loadMods(session)); the next load has them again
#include <algorithm>
#include "harness.h"
#include "GLDialog.h"
#include "eden_fs.h"
#include "eden_image.h"
#include "../../../Classes/BlockRegistry.h"
#include "../../../Classes/BlockTraits.h"
#include "../../../Classes/Console.h"
#include "../../../Classes/ContentPack.h"

using namespace harness;

namespace harness {
namespace g_packs {
namespace {

const char* kBlocks[] = {"oak_planks", "rope_ladder", "jelly", "polished_ice", "vault_stone", "stained_glass"};
enum { NBLOCKS = 6 };

std::string full(const char* id) { return std::string("emod_example:") + id; }

bool copyTree(const std::string& from, const std::string& to) {
    if (!eden_fs_make_dirs(to)) return false;
    for (const std::string& n : eden_fs_list(from)) {
        const std::string a = from + "/" + n, b = to + "/" + n;
        if (eden_fs_is_dir(a)) { if (!copyTree(a, b)) return false; }
        else if (!eden_fs_copy(a, b)) return false;
    }
    return true;
}

bool writeFile(const std::string& path, const std::string& text) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    return std::fclose(f) == 0 && ok;
}

// One console line (Console::run, so the console's own verbs work); what it printed.
std::vector<std::string> console(const std::string& line) {
    const unsigned before = Console::scrollbackSerial();
    Console::run(line.c_str());
    tick(2);
    const std::vector<Console::Line>& sb = Console::scrollback();
    const unsigned added = Console::scrollbackSerial() - before;
    std::vector<std::string> out;
    for (size_t i = sb.size() - std::min<size_t>(added, sb.size()); i < sb.size(); ++i) out.push_back(sb[i].text);
    return out;
}
std::string joined(const std::vector<std::string>& v) {
    std::string r;
    for (const std::string& s : v) r += (r.empty() ? "" : " | ") + s;
    return r;
}
bool hasLine(const std::vector<std::string>& v, const std::string& t) {
    for (const std::string& s : v) if (s.find(t) != std::string::npos) return true;
    return false;
}

std::string at(int x, int y, int z) {
    char b[64];
    std::snprintf(b, sizeof(b), "=%d =%d =%d", x, y, z);
    return b;
}

bool reload(const char* name) {
    save_world();
    if (!quit_to_menu()) return false;
    tick(60);
    if (!open_world(name, g_opt.height)) return false;
    eden_console_teleport(g_opt.at[0], g_opt.at[1], g_opt.at[2]);
    tick(g_opt.frames / 2);
    return true;
}

// The pack's lines and the loader's, all of them.
std::string reportLines() {
    const ContentPack::ModReport& r = ContentPack::modReport();
    std::string s;
    for (const ContentPack::ModPack& p : r.packs) for (const std::string& l : p.lines) s += l + "\n";
    for (const std::string& l : r.lines) s += l + "\n";
    return s;
}
const ContentPack::ModPack* packByFolder(const std::string& folder) {
    for (const ContentPack::ModPack& p : ContentPack::modReport().packs) if (p.folder == folder) return &p;
    return NULL;
}

// The bytes of a cell's column on disk (types + paints, full stride) and the palette record.
struct FileState { std::vector<uint8_t> column, palette; std::vector<std::string> verify; bool flagged = false; };
FileState file_state(int cx, int cz) {
    FileState s;
    emod::EdenWorldStore st;
    if (!st.open(eden_debug_world_file(), false)) { s.verify.push_back("open: " + st.error()); return s; }
    s.flagged = st.paletteFlag();
    if (st.hasRecord(emod::T_BLOCK_PALETTE)) st.readRecord(emod::T_BLOCK_PALETTE, s.palette);
    s.column.assign((size_t)st.bands() * emod::BAND, 0);
    if (st.readColumn(cx, cz, &s.column[0]) != emod::EdenWorldStore::READ_OK) s.column.clear();
    s.verify = st.verify();
    return s;
}
size_t cell_off(int x, int y, int z) {
    const int lx = x & (CHUNK_SIZE - 1), lz = z & (CHUNK_SIZE - 1), ly = y & (CHUNK_SIZE - 1);
    return (size_t)(y / CHUNK_SIZE) * emod::BAND + (size_t)(lx * 256 + lz * 16 + ly);
}

// The 32x32 tile `index` of a 32-wide RGBA strip equals `rgba` (RGB only when !alpha: atlas.png is RGB565).
bool tileIs(const std::vector<uint8_t>& strip, int index, const uint8_t* rgba, bool alpha) {
    const size_t o = (size_t)index * 32 * 32 * 4;
    if (strip.size() < o + 32 * 32 * 4) return false;
    for (size_t i = 0; i < 32 * 32; ++i)
        for (int c = 0; c < (alpha ? 4 : 3); ++c)
            if (strip[o + i * 4 + c] != rgba[i * 4 + c]) return false;
    return true;
}

}  // namespace

int run_packs_selftest() {
    g_selftestTag = "eden-packs";
    char d[1024];
    const std::string docs = eden_platform_documents_root();
    const std::string packs = docs + "/Packs", off = docs + "/Packs-off";
    std::string src = std::string(eden_platform_bundle_root()) + "/packs/emod_example";
    if (!eden_fs_exists(src + "/pack.json")) src = std::string(eden_platform_bundle_root()) + "/media/packs/emod_example";
    eden_fs_remove_tree(packs);
    eden_fs_remove_tree(off);
    const bool copied = copyTree(src, packs + "/emod_example");
    check(copied, "the bundled example pack copies into Documents/Packs", src.c_str());
    if (!copied) return 1;
    eden_fs_make_dirs(packs + "/aa_impostor");
    writeFile(packs + "/aa_impostor/pack.json", "{\"format\": 1, \"id\": \"eden\", \"blocks\": []}\n");
    eden_fs_make_dirs(packs + "/ab_broken");
    writeFile(packs + "/ab_broken/pack.json", "{\"format\": 1, \"id\": \"broken\",\n \"blocks\": [}\n");
    eden_fs_make_dirs(packs + "/ac_empty");
    eden_fs_make_dirs(packs + "/zz_dup");
    writeFile(packs + "/zz_dup/pack.json", "{\"format\": 1, \"id\": \"emod_example\", \"blocks\": []}\n");
    Resources* res = Resources::getResources;

    // ---- 1. the load
    const char* name = g_opt.world.empty() ? "packs-selftest" : g_opt.world.c_str();
    if (!open_world(name, g_opt.height)) return 1;
    eden_console_teleport(g_opt.at[0], g_opt.at[1], g_opt.at[2]);
    tick(g_opt.frames / 2);
    {
        const ContentPack::ModReport& r = ContentPack::modReport();
        std::string order;
        for (const ContentPack::ModPack& p : r.packs) order += p.folder + (p.refused ? "(x) " : " ");
        const std::string lines = reportLines();
        std::snprintf(d, sizeof(d), "order %s; blocks %zu; lines:\n%s", order.c_str(), r.blocks.size(), lines.c_str());
        check(order == "aa_impostor(x) ab_broken(x) emod_example zz_dup(x) " && r.blocks.size() == NBLOCKS &&
                  lines.find("aa_impostor@") != std::string::npos && lines.find("the id \"eden\" is reserved") != std::string::npos &&
                  lines.find("ab_broken@") != std::string::npos && lines.find("JSON:") != std::string::npos &&
                  lines.find("ac_empty: no pack.json (skipped)") != std::string::npos &&
                  lines.find("zz_dup: the pack id \"emod_example\" is already loaded from emod_example (skipped)") != std::string::npos &&
                  packByFolder("emod_example") && packByFolder("emod_example")->lines.empty(),
              "load order by folder; impostor, broken, empty and duplicate folders refused with their lines", d);
        std::string tiles;
        for (const ContentPack::LoadedTile& t : r.tiles) tiles += std::to_string(t.atlas) + ":" + std::to_string(t.index) + "=" + t.name + " ";
        std::snprintf(d, sizeof(d), "%s twin66=%d", tiles.c_str(), tileTwin[66]);
        check(tiles == "1:66=emod_example:oak 1:67=emod_example:oak_color 1:68=emod_example:rope 1:69=emod_example:jelly "
                       "1:70=emod_example:vault 2:32=emod_example:stained_glass " && tileTwin[66] == 67 && tileTwin[67] == 0,
              "tiles allocated in load order (D15), the oak twin recorded", d);
    }
    {
        int wa = -1, wi = -1, ia = -1, ii = -1;
        ContentPack::edenTile("wood", &wa, &wi);
        ContentPack::edenTile("ice", &ia, &ii);
        const BlockRegistry::BlockDef* oak = BlockRegistry::findDef(full("oak_planks"));
        const BlockRegistry::BlockDef* rope = BlockRegistry::findDef(full("rope_ladder"));
        const BlockRegistry::BlockDef* jelly = BlockRegistry::findDef(full("jelly"));
        const BlockRegistry::BlockDef* ice = BlockRegistry::findDef(full("polished_ice"));
        const BlockRegistry::BlockDef* vault = BlockRegistry::findDef(full("vault_stone"));
        const BlockRegistry::BlockDef* glass = BlockRegistry::findDef(full("stained_glass"));
        const bool all = oak && rope && jelly && ice && vault && glass;
        std::snprintf(d, sizeof(d), "defs %d; oak %#x art %#x faces %d; rope %#x faces %d,%d (wood %d); jelly %#x; ice %#x face %d (ice %d); "
                      "vault %#x fb %d; glass %#x face %d", all, all ? oak->info : 0, all ? oak->paintArt : 0, all ? oak->faces[0] : 0,
                      all ? rope->info : 0, all ? rope->faces[0] : 0, all ? rope->faces[5] : 0, wi, all ? jelly->info : 0,
                      all ? ice->info : 0, all ? ice->faces[0] : 0, ii, all ? vault->info : 0, all ? vault->fallbackType : 0,
                      all ? glass->info : 0, all ? glass->faces[0] : 0);
        check(all && oak->info == IS_FLAMMABLE && oak->paintArt == ART_ALL && oak->faces[0] == 66 && oak->faces[5] == 66 &&
                  rope->info == (IS_FLAMMABLE | IS_CLIMB) && rope->faces[0] == 68 && rope->faces[3] == 68 && rope->faces[4] == wi &&
                  rope->faces[5] == wi && rope->fallbackType == TYPE_LADDER && jelly->info == IS_BOUNCY && jelly->faces[2] == 69 &&
                  ice->info == (IS_HARD | IS_SLIPPERY) && ice->faces[1] == ii && ia == 1 &&
                  vault->info == (IS_HARD | IS_BLASTPROOF | IS_UNBREAKABLE) && vault->faces[0] == 70 && vault->fallbackType == TYPE_STEEL &&
                  glass->info == (IS_NOTSOLID | IS_HARD | IS_ATLAS2 | IS_NOSHADOW) && glass->faces[0] == 32 &&
                  blockSoundSets[oak->soundSet].name == std::string("eden:wood") &&
                  blockSoundSets[glass->soundSet].name == std::string("eden:glass") && oak->hudMap == -1,
              "every trait, face, sound set and fallback reached its definition row", d);
        // The atlases: uploaded once each, holding the PNGs (atlas.png drops alpha).
        EdenImage* oakPng = eden_image_load_file((packs + "/emod_example/textures/oak.png").c_str());
        EdenImage* glassPng = eden_image_load_file((packs + "/emod_example/textures/stained_glass.png").c_str());
        const bool px1 = res && oakPng && tileIs(res->atlasModPixels(1), 66, oakPng->rgba, false);
        bool px2 = res && glassPng;   // all 8 frames of the strip (Terrain::render2 animates atlas2)
        for (int f = 0; px2 && f < ContentPack::ATLAS2_FRAMES; ++f) px2 = tileIs(res->atlasModPixels(2), 32 + f, glassPng->rgba, true);
        std::snprintf(d, sizeof(d), "uploads %d/%d, oak px %d, glass px %d", res ? res->atlasUploads(1) : -1,
                      res ? res->atlasUploads(2) : -1, px1, px2);
        check(res && res->atlasUploads(1) == 1 && res->atlasUploads(2) == 1 && px1 && px2,
              "each atlas re-uploaded once, with the pack's pixels in its slots", d);
        eden_image_free(oakPng);
        eden_image_free(glassPng);
    }
    for (const char* f : {"aa_impostor", "ab_broken", "ac_empty", "zz_dup"}) eden_fs_remove_tree(packs + "/" + f);

    // ---- 2. place each block by the console
    const PlayerState ps = player_state();
    const int x = (((int)ps.x + 20) & ~(CHUNK_SIZE - 1)) + 1, z = (int)ps.z + 5, y = g_opt.height - 3;
    const int cx = x / CHUNK_SIZE, cz = z / CHUNK_SIZE;
    int ids[NBLOCKS];
    std::printf("[eden-packs] the blocks go at x %d..%d (every 2), y %d, z %d (for a --sign-shot of this world)\n", x, x + 10, y, z);
    {
        std::string why;
        bool ok = true;
        for (int i = 0; i < NBLOCKS; ++i) {
            const std::vector<std::string> out = console("set " + at(x + 2 * i, y, z) + " " + full(kBlocks[i]) + (i == 0 ? " 5" : ""));
            ids[i] = eden_console_getblock(x + 2 * i, z, y);
            if (ids[i] != 128 + i) { ok = false; why += std::string(kBlocks[i]) + ": " + std::to_string(ids[i]) + " (" + joined(out) + ") "; }
        }
        tick(30);
        const long long verts = eden_debug_chunk_empty(x, z, y) >> 2;
        std::snprintf(d, sizeof(d), "%s paint %d, verts %lld, climb %d bouncy %d slip %d blast %d unbreak %d noshadow %d art %d",
                      why.c_str(), World::getWorld->terrain->getColor(x, z, y), verts, trait_climbable(129), trait_bouncy(130),
                      trait_slippery(131), trait_blastproof(132), trait_unbreakable(132), trait_noShadow(133), trait_paintArtChunk(128));
        check(ok && World::getWorld->terrain->getColor(x, z, y) == 5 && verts > 0 && trait_climbable(129) && trait_bouncy(130) &&
                  trait_slippery(131) && trait_blastproof(132) && trait_unbreakable(132) && trait_noShadow(133) &&
                  trait_paintArtChunk(128) && !trait_paintArtChunk(129),
              "each block placed by its name (128..133), painted, meshed, its traits live", d);
        const std::vector<std::string> list = console("packs"), blocks = console("packs emod_example");
        std::snprintf(d, sizeof(d), "packs: %s || packs emod_example: %s", joined(list).c_str(), joined(blocks).c_str());
        check(hasLine(list, "emod_example \"Example blocks\" 1.0.0: 6 of 6 block(s)") && hasLine(list, "atlas tiles 5 of 62 opaque, 1 of 12 transparent") &&
                  blocks.size() == NBLOCKS && hasLine(blocks, "emod_example:jelly (130 in this world)"),
              "the console's packs verb lists the pack and its blocks", d);
    }
    tick(30);
    const std::string hashA = eden_debug_chunk_mesh_hash(x, z, y);

    // ---- 3. save + reload
    if (!reload(name)) return 1;
    const FileState saved = file_state(cx, cz);
    {
        bool same = true;
        for (int i = 0; i < NBLOCKS; ++i) same = same && eden_console_getblock(x + 2 * i, z, y) == 128 + i;
        const std::string h = eden_debug_chunk_mesh_hash(x, z, y);
        std::vector<emod::PaletteEntry> es;
        emod::palette_parse(saved.palette.data(), saved.palette.size(), &es, NULL);
        std::string pal;
        for (const auto& e : es) pal += std::to_string(e.id) + "=" + e.name + "/" + std::to_string(e.fallbackType) + " ";
        std::snprintf(d, sizeof(d), "same %d, paint %d, hash %s vs %s, uploads %d/%d, flagged %d, palette %s, verify %zu", same,
                      World::getWorld->terrain->getColor(x, z, y), h.c_str(), hashA.c_str(), res ? res->atlasUploads(1) : -1,
                      res ? res->atlasUploads(2) : -1, saved.flagged, pal.c_str(), saved.verify.size());
        check(same && World::getWorld->terrain->getColor(x, z, y) == 5 && h == hashA && res && res->atlasUploads(1) == 1 &&
                  res->atlasUploads(2) == 1 && saved.flagged && es.size() == NBLOCKS && es[0].name == full("oak_planks") &&
                  es[0].fallbackType == TYPE_WOOD && es[5].fallbackType == TYPE_GLASS && saved.verify.empty(),
              "after save + reload: same bytes, paint and mesh; no atlas re-upload; the palette names the blocks", d);
    }

    // ---- 4. the pack goes away
    eden_fs_make_dirs(off);
    const bool moved = eden_fs_move(packs + "/emod_example", off + "/emod_example");
    if (!reload(name)) return 1;
    {
        bool missing = true;
        for (int i = 0; i < NBLOCKS; ++i)
            missing = missing && BlockRegistry::missing(128 + i) && eden_console_getblock(x + 2 * i, z, y) == 128 + i;
        const std::string h = eden_debug_chunk_mesh_hash(x, z, y);
        std::snprintf(d, sizeof(d), "moved %d, missing %d, paint %d, info128 %#x, hash %s, uploads %d/%d, stock px %zu/%zu, twin66 %d, packs %zu",
                      moved, missing, World::getWorld->terrain->getColor(x, z, y), blockinfo[128], h.c_str(),
                      res ? res->atlasUploads(1) : -1, res ? res->atlasUploads(2) : -1, res ? res->atlasModPixels(1).size() : 0,
                      res ? res->atlasModPixels(2).size() : 0, tileTwin[66], ContentPack::modReport().packs.size());
        check(moved && missing && World::getWorld->terrain->getColor(x, z, y) == 5 && blockinfo[128] == (IS_HARD | IS_MISSING) &&
                  h != hashA && res && res->atlasUploads(1) == 2 && res->atlasUploads(2) == 2 && res->atlasModPixels(1).empty() &&
                  res->atlasModPixels(2).empty() && tileTwin[66] == 0 && ContentPack::modReport().packs.empty(),
              "pack removed: placeholders, bytes and paint kept, the stock atlases back", d);
        const int yd = y - CHUNK_SIZE;
        console("set " + at(x, yd, z) + " brick 9");
        if (!reload(name)) return 1;
        const FileState now = file_state(cx, cz);
        size_t diff = 0;
        const size_t o = cell_off(x, yd, z);
        if (now.column.size() == saved.column.size() && !now.column.empty()) {
            for (size_t i = 0; i < now.column.size(); ++i)
                if (i != o && i != o + 4096 && now.column[i] != saved.column[i]) diff++;
        } else diff = (size_t)-1;
        std::snprintf(d, sizeof(d), "%zu other bytes differ, palette same %d, edit %d, verify %zu", diff, now.palette == saved.palette,
                      now.column.empty() ? -1 : now.column[o], now.verify.size());
        check(diff == 0 && now.palette == saved.palette && !now.column.empty() && now.column[o] == TYPE_BRICK && now.verify.empty(),
              "an edit + save with the pack missing leaves the mod cells and the palette alone", d);
    }

    // ---- 5. the pack comes back
    eden_fs_move(off + "/emod_example", packs + "/emod_example");
    if (!reload(name)) return 1;
    {
        const std::string h = eden_debug_chunk_mesh_hash(x, z, y);
        std::snprintf(d, sizeof(d), "missing %d, hash %s vs %s, uploads %d/%d", BlockRegistry::missing(128), h.c_str(), hashA.c_str(),
                      res ? res->atlasUploads(1) : -1, res ? res->atlasUploads(2) : -1);
        check(!BlockRegistry::missing(128) && h == hashA && res && res->atlasUploads(1) == 3 && res->atlasUploads(2) == 3,
              "pack restored: the original look is back", d);
    }

    // ---- 6. the tile budget (D10): 60 more opaque tiles for 57 free slots, 12 transparent for 11 free strips
    {
        eden_fs_make_dirs(packs + "/zz_overflow/t");
        std::string json = "{\"format\": 1, \"id\": \"overflow\", \"tiles\": {";
        std::string blocks;
        for (int i = 0; i < 60; ++i) {
            std::vector<unsigned char> rgb(32 * 32 * 3, (unsigned char)(i * 4)), png;
            photoEncodePNG(rgb.data(), 32, 32, png);
            writeFile(packs + "/zz_overflow/t/" + std::to_string(i) + ".png", std::string(png.begin(), png.end()));
            json += std::string(i ? ", " : "") + "\"t" + std::to_string(i) + "\": {\"png\": \"t/" + std::to_string(i) + ".png\"}";
            blocks += std::string(i ? ", " : "") + "{\"id\": \"b" + std::to_string(i) + "\", \"faces\": {\"all\": \"t" + std::to_string(i) +
                      "\"}, \"color\": [1, 2, 3], \"traits\": [\"solid\"], \"fallback\": \"eden:stone\"}";
        }
        for (int i = 0; i < 12; ++i)   // transparent: 11 strips are free after the example's one
            blocks += ", {\"id\": \"g" + std::to_string(i) + "\", \"faces\": {\"all\": \"t" + std::to_string(i) +
                      "\"}, \"color\": [1, 2, 3], \"traits\": [\"transparent\"], \"fallback\": \"eden:glass\"}";
        json += "}, \"blocks\": [" + blocks + "]}\n";
        writeFile(packs + "/zz_overflow/pack.json", json);
        if (!reload(name)) return 1;
        const ContentPack::ModReport& r = ContentPack::modReport();
        auto describe = [&]() {
            std::string s;
            for (const ContentPack::LoadedTile& t : r.tiles) s += std::to_string(t.atlas) + ":" + std::to_string(t.index) + "=" + t.name + ";";
            for (const ContentPack::LoadedBlock& b : r.blocks) s += b.name + (b.missingTexture ? "!" : "") + ";";
            return s;
        };
        const std::string first = describe();
        int good = 0, missingTex = 0, wrong = 0;
        for (int i = 0; i < 60; ++i) {
            const BlockRegistry::BlockDef* def = BlockRegistry::findDef("overflow:b" + std::to_string(i));
            if (!def) { ++wrong; continue; }
            if (i < 57 && def->faces[0] == 71 + i && def->faces[5] == 71 + i) ++good;
            else if (i >= 57 && def->faces[0] == TEX_MISSING && def->faces[5] == TEX_MISSING) ++missingTex;
            else ++wrong;
        }
        int glassGood = 0, glassMissing = 0;
        for (int i = 0; i < 12; ++i) {
            const BlockRegistry::BlockDef* def = BlockRegistry::findDef("overflow:g" + std::to_string(i));
            if (!def) continue;
            if (i < 11 && def->faces[0] == 40 + 8 * i && (def->info & IS_ATLAS2)) ++glassGood;
            else if (i == 11 && def->faces[0] == TEX_MISSING && !(def->info & IS_ATLAS2)) ++glassMissing;
        }
        int budgetLines = 0;
        for (const std::string& l : r.lines) if (l.find("no free atlas tile") != std::string::npos) ++budgetLines;
        const std::string h = eden_debug_chunk_mesh_hash(x, z, y);
        std::snprintf(d, sizeof(d), "good %d, missing texture %d, wrong %d, transparent %d + %d missing, budget lines %d, oak %d, hash same %d, uploads %d/%d",
                      good, missingTex, wrong, glassGood, glassMissing, budgetLines, BlockRegistry::findDef(full("oak_planks")) ? BlockRegistry::findDef(full("oak_planks"))->faces[0] : -1,
                      h == hashA, res ? res->atlasUploads(1) : -1, res ? res->atlasUploads(2) : -1);
        check(good == 57 && missingTex == 3 && wrong == 0 && glassGood == 11 && glassMissing == 1 && budgetLines == 4 &&
                  BlockRegistry::findDef(full("oak_planks"))->faces[0] == 66 && h == hashA && res && res->atlasUploads(1) == 4 &&
                  res->atlasUploads(2) == 4,
              "over the tile budget only the overflowing blocks draw TEX_MISSING; the example's tiles do not move", d);
        if (!reload(name)) return 1;
        const std::string second = describe();
        check(first == second && res->atlasUploads(1) == 4 && res->atlasUploads(2) == 4,
              "the next load allocates identically (D15) and uploads nothing", "");
        eden_fs_remove_tree(packs + "/zz_overflow");
    }

    // ---- 6b. K.2-5: the Mods page, name-keyed selection, placement takes the id
    {
        Hud* hud = World::getWorld->hud;
        const int pages = Hud::pickerPageCount();
        hud->mode = MODE_PICK_BLOCK;
        hud->pickerPage = 2;
        tick(3);
        const int n = Hud::pickerCellCount(2);
        std::string c0 = n > 0 ? Hud::modCellName(Hud::pickerCell(2, 0)) : "";
        std::string c1 = n > 1 ? Hud::modCellName(Hud::pickerCell(2, 1)) : "";
        ui_tap_rect(hud->blockBounds[1]);
        tick(5);
        const bool picked = hud->mode == MODE_BUILD && hud_picker_is_mod(hud->blocktype) && Hud::modSelection() == c1 &&
                            BlockRegistry::findDef(c1) != NULL;
        const bool resolved = hud->resolveModBlock() && hud->blocktype == BlockRegistry::idOf(c1) && hud->blocktype > STOCK_BLOCK_MAX;
        // switching worlds re-points the selection by name (a stale sentinel/id must not survive)
        hud->blocktype = HUD_MOD_BASE + 99;
        hud->worldLoaded();
        const bool stale = hud->blocktype == HUD_MOD_BASE + (int)(std::find_if(ContentPack::modReport().blocks.begin(), ContentPack::modReport().blocks.end(),
                               [&](const ContentPack::LoadedBlock& b) { return b.name == c1; }) - ContentPack::modReport().blocks.begin());
        std::snprintf(d, sizeof(d), "pages %d cells %d [%s,%s] picked %d resolved %d re-pointed %d", pages, n, c0.c_str(), c1.c_str(), picked, resolved, stale);
        int vis = 0;
        for (const ContentPack::LoadedBlock& b : ContentPack::modReport().blocks) vis += b.picker;
        check(pages == 2 + (vis + 34) / 35 && n == std::min(35, vis) && vis > 0 && !c0.empty() && picked && resolved && stale,
              "Mods pages follow the picker blocks (35 a page); a tap selects by name; placement resolves the id; a new world re-points it", d);
        hud->blocktype = TYPE_BRICK;
        hud->mode = MODE_BUILD;
        // the page, as drawn (windowed runs only; headless capture is a no-op)
        tap_hud(1);   // the picker, opened by a real touch on the build button
        while (hud->pickerPage < 2 && hud->pickerPageStep(1)) tick(1);
        tick(10);
        g_shots::capture("picker-mods");
        tap_hud(1);
        tick(5);
        hud->mode = MODE_BUILD;
    }

    // ---- 6c. K.2-5: a fresh (unflagged) world asks before its first mod block; Cancel places nothing
    {
        const char* fresh = "packs-confirm";
        bool ok = quit_to_menu() && open_world(fresh, g_opt.height);
        Hud* hud = World::getWorld->hud;
        tick(10);
        std::string nm = Hud::modCellName(Hud::pickerCell(2, 0));
        const bool wasFlagged = World::getWorld->fm->paletteFlagged();
        hud->blocktype = HUD_MOD_BASE;
        const bool refused = !hud->resolveModBlock() && GLDialog::active() && hud_picker_is_mod(hud->blocktype);
        tick(5);
        g_shots::capture("mod-confirm");
        CGRect cancel, okr;
        const bool haveBtn = GLDialog::buttonRect(1, &cancel) && GLDialog::buttonRect(0, &okr);
        if (haveBtn) { ui_tap_rect(cancel); tick(5); }
        const bool cancelled = !GLDialog::active() && BlockRegistry::idOf(nm) < 0 && !World::getWorld->fm->paletteFlagged() && hud_picker_is_mod(hud->blocktype);
        hud->resolveModBlock();
        tick(3);
        if (haveBtn) { ui_tap_rect(okr); tick(5); }
        const bool placed = World::getWorld->fm->paletteFlagged() && BlockRegistry::idOf(nm) == 128 && hud->blocktype == 128;
        const bool again = hud->resolveModBlock();   // confirmed or have id: no second dialog
        std::snprintf(d, sizeof(d), "ok %d flagged-before %d dialog %d cancel %d placed %d again %d", ok, wasFlagged, refused, cancelled, placed, again);
        check(ok && !wasFlagged && refused && haveBtn && cancelled && placed && again && !GLDialog::active(),
              "first mod placement in an unflagged world asks; Cancel leaves the world unflagged; OK flags it and takes the id", d);
        hud->blocktype = TYPE_BRICK;
    }

    // ---- 7. a session loads none
    {
        ContentPack::loadMods(docs, true);
        const bool none = ContentPack::modReport().session && ContentPack::modReport().packs.empty() &&
                          !BlockRegistry::findDef(full("jelly")) && res && res->atlasModPixels(1).empty();
        const std::vector<std::string> out = console("packs");
        const bool noPage = Hud::pickerPageCount() == 2;
        ContentPack::loadMods(docs, false);
        std::snprintf(d, sizeof(d), "none %d, console %s, back %d", none, joined(out).c_str(), BlockRegistry::findDef(full("jelly")) != NULL);
        check(none && noPage && hasLine(out, "multiplayer") && BlockRegistry::findDef(full("jelly")) != NULL,
              "a session loads no pack (and the stock atlas); the next load has them again", d);
    }
    std::printf("[eden-packs] %s (%d failure(s))\n", g_selftestFailures ? "FAILURES" : "ALL PASS", g_selftestFailures);
    return g_selftestFailures ? 1 : 0;
}

}  // namespace g_packs
}  // namespace harness
