// K.1-3: --registry-selftest, the block registry's runtime gate (WORKING/block-registry-plan-2026-10-31.md §6
// "Runtime"). Headless; give it a fresh --docs. Mode group "registry".
//
//   1. names: every stock id's canonical `eden:` name round-trips, aliases resolve, bad definitions refused
//   2. an unflagged world keeps a latent byte >= 128 through mesh + save + reload (rebuild2's repair is gone)
//   3. console `set`/`fill`/`if` with `emod:test_*`: the flag compaction reserves the latent byte (D13), the
//      first id is 128, paint kept; save -> reload: same bytes, same paint, same mesh hash
//   4. test pack unregistered -> reload: placeholders (IS_MISSING), bytes + paint unchanged; save -> the
//      column's other bytes and the palette unchanged; a placeholder placed by number breaks like stone
//   5. pack re-registered -> reload: the original mesh hash is back
//   6. refusals: an unassigned number, a mod name in a session (and world edits in a session with the
//      day/night cycle on), a full palette (D17)
//   6b. K.1-4 export: own format carries the palette as BPL1 (= the file's record), NewFormat256z writes fallbacks
//   7. Convert to 64z (a 256z run) keeps the flag and the palette; the next world starts with no palette (resetWorld)
// EdenWorldStore::verify() (= emod.py verify) is clean on every saved file.
#include "harness.h"
#include "../../../Classes/BlockRegistry.h"
#include "../../../Classes/SkyCycle.h"

using namespace harness;

namespace harness {
namespace g_registry {

int setting_index(const char* key) {
    for (int i = 0; i < eden_settings_count(); ++i) if (!std::strcmp(eden_settings_key(i), key)) return i;
    return -1;
}

struct Sink : CmdScript::Sink {
    std::vector<std::string> lines;
    void line(int, const std::string& text) override { lines.push_back(text); }
    bool has(const std::string& t) const {
        for (const std::string& l : lines) if (l.find(t) != std::string::npos) return true;
        return false;
    }
    std::string all() const {
        std::string r;
        for (const std::string& l : lines) r += (r.empty() ? "" : " | ") + l;
        return r;
    }
};

// One console line at the player's anchor; `out` gets what it printed.
int con(const std::string& line, Sink* out) {
    out->lines.clear();
    int ax, ay, az;
    CmdScript::playerAnchor(&ax, &ay, &az);
    const int r = CmdScript::runLine(line.c_str(), ax, ay, az, CmdScript::SRC_HARNESS, out, NULL);
    tick(2);
    return r;
}
std::string at(int x, int y, int z) {
    char b[64];
    std::snprintf(b, sizeof(b), "=%d =%d =%d", x, y, z);
    return b;
}
int color_at(int x, int y, int z) { return World::getWorld->terrain->getColor(x, z, y); }
long long verts(int x, int y, int z) { return eden_debug_chunk_empty(x, z, y) >> 2; }
std::string mesh_hash(int x, int y, int z) { return eden_debug_chunk_mesh_hash(x, z, y); }

void register_pack() {
    for (int i = 0; i <= 128; ++i) {
        const int like = i == 1 ? TYPE_GLASS : i == 0 ? TYPE_BRICK : TYPE_STEEL;
        BlockRegistry::registerDef(BlockRegistry::defLike("emod:test_" + std::to_string(i), like, i == 0 ? 7 : 0));
    }
}

// The file's state: flag, palette record bytes, verify() findings, one column's bytes.
struct FileState {
    bool ok = false, flagged = false;
    std::vector<uint8_t> palette, column;
    std::vector<std::string> verify;
};
FileState file_state(int cx, int cz) {
    FileState s;
    const std::string path = eden_debug_world_file();
    emod::EdenWorldStore st;
    if (!st.open(path.c_str(), false)) { s.verify.push_back("open: " + st.error()); return s; }
    s.ok = true;
    s.flagged = st.paletteFlag();
    if (st.hasRecord(emod::T_BLOCK_PALETTE)) st.readRecord(emod::T_BLOCK_PALETTE, s.palette);
    s.column.assign((size_t)st.bands() * emod::BAND, 0);
    if (st.readColumn(cx, cz, &s.column[0]) != emod::EdenWorldStore::READ_OK) s.column.clear();
    s.verify = st.verify();
    return s;
}
// A cell's type and paint byte offsets in its column (full stride: band = 4096 types + 4096 paints).
size_t cell_off(int x, int y, int z) {
    const int lx = x & (CHUNK_SIZE - 1), lz = z & (CHUNK_SIZE - 1), ly = y & (CHUNK_SIZE - 1);
    return (size_t)(y / CHUNK_SIZE) * emod::BAND + (size_t)(lx * 256 + lz * 16 + ly);
}

// The trailer of a `.eden` on disk (empty if it cannot be read).
std::vector<uint8_t> eden_trailer(const std::string& path) {
    std::vector<uint8_t> f;
    if (FILE* fp = std::fopen(path.c_str(), "rb")) {
        uint8_t b[65536];
        size_t n;
        while ((n = std::fread(b, 1, sizeof b, fp)) > 0) f.insert(f.end(), b, b + n);
        std::fclose(fp);
    }
    if (f.size() < emod::EDEN_HEADER) return std::vector<uint8_t>();
    uint64_t dirOff = 0;
    for (int i = 7; i >= 0; i--) dirOff = (dirOff << 8) | f[32 + i];
    if (dirOff > f.size()) return std::vector<uint8_t>();
    emod::EdenLayout lay;
    if (!emod::eden_read_layout(f.data(), f.size(), std::vector<uint8_t>(f.begin() + (size_t)dirOff, f.end()), lay)) return std::vector<uint8_t>();
    return lay.trailer;
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

int run_registry_selftest() {
    g_selftestTag = "eden-registry";
    char d[512];

    // ---- 1. names
    {
        int bad = 0;
        std::string why;
        for (int i = 0; i <= STOCK_BLOCK_MAX; ++i) {
            const char* n = BlockRegistry::stockName(i);
            if (!n || BlockRegistry::stockByName(n) != i || CmdScript::blockByName(n) != i) { bad++; why += std::to_string(i) + " "; }
        }
        std::snprintf(d, sizeof(d), "%d bad: %s; ramp1=%s btgrass=%d", bad, why.c_str(),
                      BlockRegistry::stockName(TYPE_STONE_RAMP1), BlockRegistry::stockByName("eden:btgrass"));
        check(bad == 0 && !std::strcmp(BlockRegistry::stockName(TYPE_STONE_RAMP1), "eden:stone_ramp1") &&
                  BlockRegistry::stockByName("eden:btgrass") == TYPE_BTGRASS && BlockRegistry::stockName(128) == NULL,
              "the 128 canonical eden: names round-trip", d);
        std::snprintf(d, sizeof(d), "lamp %d, LAMP %d, eden:lamp %d, stone_ramp1 %d, 127 %d, 128 %d",
                      CmdScript::blockByName("lamp"), CmdScript::blockByName("LAMP"), CmdScript::blockByName("eden:lamp"),
                      CmdScript::blockByName("stone_ramp1"), CmdScript::blockByName("127"), CmdScript::blockByName("128"));
        check(CmdScript::blockByName("lamp") == TYPE_LIGHTBOX && CmdScript::blockByName("LAMP") == TYPE_LIGHTBOX &&
                  CmdScript::blockByName("eden:lamp") == -1 && CmdScript::blockByName("stone_ramp1") == -1 &&
                  CmdScript::blockByName("127") == 127 && CmdScript::blockByName("128") == -1,
              "aliases stay unqualified, canonical names need eden:, 128 is unassigned", d);
        BlockRegistry::BlockDef g = BlockRegistry::defLike("emod:ok", TYPE_STONE);
        BlockRegistry::BlockDef e = BlockRegistry::defLike("eden:stone2", TYPE_STONE);
        BlockRegistry::BlockDef u = BlockRegistry::defLike("Emod:Upper", TYPE_STONE);
        BlockRegistry::BlockDef f = BlockRegistry::defLike("emod:door", TYPE_DOOR1);
        const bool rg = BlockRegistry::registerDef(g), re = BlockRegistry::registerDef(e), ru = BlockRegistry::registerDef(u),
                   rf = BlockRegistry::registerDef(f);
        BlockRegistry::unregisterDef("emod:ok");
        std::snprintf(d, sizeof(d), "ok %d eden: %d upper %d door-fallback %d", rg, re, ru, rf);
        check(rg && !re && !ru && !rf, "registerDef refuses eden:, bad names and a door fallback", d);
    }
    register_pack();

    const char* name = g_opt.world.empty() ? "registry-selftest" : g_opt.world.c_str();
    if (!open_world(name, g_opt.height)) return 1;
    eden_console_teleport(g_opt.at[0], g_opt.at[1], g_opt.at[2]);
    tick(g_opt.frames / 2);
    const PlayerState ps = player_state();
    // Top band, off the player's real position, two apart (all six faces exposed). One column: x, z fixed.
    // x..x+9 stay inside one 16-wide column (A..F+1 and the break test at x+8).
    const int x = (((int)ps.x + 20) & ~(CHUNK_SIZE - 1)) + 1, z = (int)ps.z + 5, y = g_opt.height - 3;
    const int xa = x, xb = x + 2, xf = x + 4;   // A: test_0 (+paint), B: latent 130, F/F+1: fill test_1
    const int cx = x / CHUNK_SIZE, cz = z / CHUNK_SIZE;
    const bool oneColumn = (xf + 1) / CHUNK_SIZE == cx;
    Sink sk;

    // ---- 2. a latent byte in an unflagged world survives
    check(BlockRegistry::count() == 0 && !BlockRegistry::locked(), "a new world has an empty palette", "");
    eden_debug_build_block(xb, z, y, 130);
    tick(30);
    std::snprintf(d, sizeof(d), "getblock %d, verts %lld, info %#x", eden_console_getblock(xb, z, y), verts(xb, y, z),
                  blockinfo[130]);
    check(eden_console_getblock(xb, z, y) == 130 && verts(xb, y, z) > 0 && (blockinfo[130] & IS_MISSING),
          "byte 130 with no palette meshes as the missing block and is not rewritten", d);
    if (!reload(name)) return 1;
    {
        FileState fs = file_state(cx, cz);
        std::snprintf(d, sizeof(d), "getblock %d, flagged %d, verify %zu", eden_console_getblock(xb, z, y), fs.flagged,
                      fs.verify.size());
        check(eden_console_getblock(xb, z, y) == 130 && !fs.flagged, "after save + reload byte 130 is still 130 (unflagged file)", d);
    }

    // ---- 3. console placement, flag compaction, save + reload
    int r = con("set " + at(xa, y, z) + " 200", &sk);
    std::snprintf(d, sizeof(d), "r %d: %s", r, sk.all().c_str());
    check(r == CmdScript::RUN_BAD && sk.has("Unknown block: 200"), "an unassigned number is refused", d);
    r = con("set " + at(xa, y, z) + " emod:test_0 5", &sk);
    std::string hashA;
    {
        FileState fs = file_state(cx, cz);
        const char* n130 = BlockRegistry::nameOf(130);
        std::snprintf(d, sizeof(d), "r %d (%s), getblock %d, paint %d, 130=%s, count %d, flagged %d, verts %lld", r,
                      sk.all().c_str(), eden_console_getblock(xa, z, y), color_at(xa, y, z), n130 ? n130 : "-",
                      BlockRegistry::count(), fs.flagged, verts(xa, y, z));
        check(r == CmdScript::RUN_DONE && eden_console_getblock(xa, z, y) == 128 && color_at(xa, y, z) == 5 &&
                  n130 && !std::strcmp(n130, "emod:unknown_130") && BlockRegistry::count() == 2 && fs.flagged &&
                  !BlockRegistry::missing(128) && BlockRegistry::missing(130) && verts(xa, y, z) > 0,
              "set emod:test_0: flag compaction reserved latent 130, test_0 got 128, painted, meshed", d);
    }
    r = con("fill " + at(xf, y, z) + " " + at(xf + 1, y, z) + " emod:test_1", &sk);
    std::snprintf(d, sizeof(d), "r %d (%s), %d %d, test_1=%d", r, sk.all().c_str(), eden_console_getblock(xf, z, y),
                  eden_console_getblock(xf + 1, z, y), BlockRegistry::idOf("emod:test_1"));
    check(r == CmdScript::RUN_DONE && eden_console_getblock(xf, z, y) == 129 && eden_console_getblock(xf + 1, z, y) == 129,
          "fill emod:test_1 takes 129", d);
    r = con("if " + at(xa, y, z) + " emod:test_0; say holds; unless " + at(xa, y, z) + " emod:test_1; say differs", &sk);
    std::snprintf(d, sizeof(d), "r %d: %s", r, sk.all().c_str());
    check(r == CmdScript::RUN_DONE && sk.has("holds") && sk.has("differs"), "if/unless compare mod names", d);
    r = con("if " + at(xa, y, z) + " emod:test_9; say wrong", &sk);
    check(r == CmdScript::RUN_DONE && !sk.has("wrong") && BlockRegistry::idOf("emod:test_9") < 0,
          "if with an unallocated mod name is false and allocates nothing", sk.all().c_str());
    tick(30);
    hashA = mesh_hash(xa, y, z);
    if (!reload(name)) return 1;
    FileState saved = file_state(cx, cz);
    {
        std::vector<emod::PaletteEntry> es;
        emod::palette_parse(saved.palette.data(), saved.palette.size(), &es, NULL);
        std::string list;
        for (const auto& e : es) list += std::to_string(e.id) + "=" + e.name + "/" + std::to_string(e.fallbackType) + " ";
        std::snprintf(d, sizeof(d), "flagged %d, palette %s, verify %zu%s%s", saved.flagged, list.c_str(), saved.verify.size(),
                      saved.verify.empty() ? "" : ": ", saved.verify.empty() ? "" : saved.verify[0].c_str());
        check(saved.ok && saved.flagged && es.size() == 3 && es[0].id == 128 && es[0].name == "emod:test_0" &&
                  es[0].fallbackType == TYPE_BRICK && es[0].fallbackPaint == 7 && es[1].id == 129 && es[2].id == 130 &&
                  saved.verify.empty(),
              "the saved file: flagged, palette 128/129/130 with fallbacks, verify clean", d);
        const std::string h = mesh_hash(xa, y, z);
        std::snprintf(d, sizeof(d), "getblock %d %d %d, paint %d, hash %s vs %s", eden_console_getblock(xa, z, y),
                      eden_console_getblock(xf, z, y), eden_console_getblock(xb, z, y), color_at(xa, y, z), h.c_str(), hashA.c_str());
        check(eden_console_getblock(xa, z, y) == 128 && eden_console_getblock(xf, z, y) == 129 &&
                  eden_console_getblock(xb, z, y) == 130 && color_at(xa, y, z) == 5 && h == hashA && !BlockRegistry::dirty(),
              "after reload: same bytes, same paint, same mesh", d);
    }

    // ---- 4. the pack goes away: placeholders, nothing rewritten
    BlockRegistry::clearDefs();
    if (!reload(name)) return 1;
    {
        const std::string h = mesh_hash(xa, y, z);
        std::snprintf(d, sizeof(d), "getblock %d, paint %d, missing %d, info %#x, verts %lld, hash %s",
                      eden_console_getblock(xa, z, y), color_at(xa, y, z), BlockRegistry::missing(128), blockinfo[128],
                      verts(xa, y, z), h.c_str());
        check(eden_console_getblock(xa, z, y) == 128 && color_at(xa, y, z) == 5 && BlockRegistry::missing(128) &&
                  blockinfo[128] == (IS_HARD | IS_MISSING) && blockTypeFaces[128][0] == TEX_MISSING && verts(xa, y, z) > 0 &&
                  h != hashA && BlockRegistry::count() == 3,
              "unregistered: the bytes draw as the missing block, paint kept", d);
        r = con("set " + at(xa, y, z) + " emod:test_2", &sk);
        check(r == CmdScript::RUN_BAD && sk.has("Unknown block: emod:test_2"), "a mod name with no definition is unknown", sk.all().c_str());
        // dirty the column away from A's chunk, save, and compare every other byte
        const int yd = y - CHUNK_SIZE;
        r = con("set " + at(xa, yd, z) + " brick 9", &sk);
        if (!reload(name)) return 1;
        FileState now = file_state(cx, cz);
        size_t diff = 0;
        const size_t o = cell_off(xa, yd, z);
        if (now.column.size() == saved.column.size() && !now.column.empty()) {
            for (size_t i = 0; i < now.column.size(); ++i)
                if (i != o && i != o + 4096 && now.column[i] != saved.column[i]) diff++;
        } else diff = (size_t)-1;
        std::snprintf(d, sizeof(d), "edit r %d, one column %d, %zu other bytes differ, palette same %d, verify %zu, edit landed %d",
                      r, oneColumn, diff, now.palette == saved.palette, now.verify.size(),
                      now.column.empty() ? -1 : now.column[o] == TYPE_BRICK);
        check(oneColumn && diff == 0 && now.palette == saved.palette && now.verify.empty() && !now.column.empty() &&
                  now.column[o] == TYPE_BRICK && now.column[cell_off(xa, y, z)] == 128 && now.column[cell_off(xa, y, z) + 4096] == 5,
              "save with the pack missing: placeholder bytes, paint and palette unchanged", d);
        // a placeholder placed by its number, then broken
        const int xp = x + 8;
        r = con("set " + at(xp, y, z) + " 128", &sk);
        const int placed = eden_console_getblock(xp, z, y);
        World::getWorld->terrain->destroyBlock(xp, z, y);
        tick(5);
        std::snprintf(d, sizeof(d), "r %d (%s), placed %d, after break %d", r, sk.all().c_str(), placed, eden_console_getblock(xp, z, y));
        check(r == CmdScript::RUN_DONE && placed == 128 && eden_console_getblock(xp, z, y) == 0,
              "an assigned id places by number and the placeholder breaks", d);
    }

    // ---- 5. the pack comes back
    register_pack();
    if (!reload(name)) return 1;
    {
        const std::string h = mesh_hash(xa, y, z);
        std::snprintf(d, sizeof(d), "missing %d, hash %s vs %s", BlockRegistry::missing(128), h.c_str(), hashA.c_str());
        check(!BlockRegistry::missing(128) && BlockRegistry::missing(130) && h == hashA, "re-registered: the original look is back", d);
    }

    // ---- 6. refusals
    {
        // a session: no mod names, no world edits even with the day/night cycle on
        FileManager* fm = World::getWorld->fm;
        const int dn = setting_index("day_night_cycle");
        if (dn >= 0) eden_settings_set(dn, 1);
        tick(2);
        const bool cycleOn = SkyCycle::enabled();
        fm->sessionWorld = true;
        CmdScript::Program p;
        CmdScript::parseLine("set 0 0 0 emod:test_5", &p, CmdScript::consoleOptions(NULL));
        std::string why;
        const int got = BlockRegistry::acquire("emod:test_5", &why);
        Sink s2;
        int ax, ay, az;
        CmdScript::playerAnchor(&ax, &ay, &az);
        const int rs = CmdScript::runLine("set ~ ~3 ~ stone", ax, ay, az, CmdScript::SRC_HARNESS, &s2, NULL);
        fm->sessionWorld = false;
        if (dn >= 0) eden_settings_set(dn, 0);
        std::snprintf(d, sizeof(d), "parse errors %d (%s), acquire %d (%s), set stone r %d (%s), cycle %d",
                      p.errors, p.firstError.c_str(), got, why.c_str(), rs, s2.all().c_str(), (int)cycleOn);
        check(cycleOn && p.errors == 1 && p.firstError.find("multiplayer") != std::string::npos && got < 0 &&
                  why.find("multiplayer") != std::string::npos && rs == CmdScript::RUN_BAD &&
                  s2.has("Not in a multiplayer world: that changes the world"),
              "a session refuses mod names and world edits (day/night cycle on)", d);
        check(BlockRegistry::idOf("emod:test_5") < 0, "the refused session line allocated nothing", "");

        // K.1-4: own-format export = raw bytes + BPL1 (the file's record); NewFormat256z = fallbacks, no BPL1
        save_world();
        emod::ExportOptions xo;
        emod::ExportReport xr;
        std::string err;
        const std::string out = World::getWorld->fm->documents + "/registry-export.eden";
        bool xok = emod::export_world_file(eden_debug_world_file(), out, xo, false, &xr, &err);
        FileState xs = file_state(cx, cz);
        std::vector<uint8_t> bpl1;
        const std::vector<uint8_t> tr = eden_trailer(out);
        const bool bplOk = emod::trailer_find_palette(tr.data(), tr.size(), &bpl1) && bpl1 == xs.palette;
        std::snprintf(d, sizeof(d), "ok %d (%s), written %d, entries %u, replaced %llu, BPL1 == record %d", xok, err.c_str(),
                      xr.paletteWritten, xr.paletteEntries, (unsigned long long)xr.paletteReplaced, bplOk);
        check(xok && xr.paletteWritten && xr.paletteEntries == (uint32_t)BlockRegistry::count() && xr.paletteReplaced == 0 && bplOk,
              "own-format export: the raw bytes + a BPL1 that is the file's BLOCK_PALETTE", d);
        xo.target = emod::X_NEWFORMAT256Z;
        xok = emod::export_world_file(eden_debug_world_file(), out, xo, false, &xr, &err);
        const std::vector<uint8_t> tr2 = eden_trailer(out);
        std::snprintf(d, sizeof(d), "ok %d (%s), written %d, replaced %llu, unassigned %llu, %s", xok, err.c_str(), xr.paletteWritten,
                      (unsigned long long)xr.paletteReplaced, (unsigned long long)xr.paletteUnassigned, xr.summary().c_str());
        check(xok && !xr.paletteWritten && xr.paletteReplaced > 0 && !emod::trailer_find_palette(tr2.data(), tr2.size(), NULL),
              "NewFormat256z export: the mod blocks become their fallbacks, no BPL1", d);
        std::remove(out.c_str());

        // palette full (D17)
        int last = -1;
        for (int i = 2; i <= 127 && BlockRegistry::count() < 128; ++i) last = BlockRegistry::acquire("emod:test_" + std::to_string(i), &why);
        const int full = BlockRegistry::acquire("emod:test_128", &why);
        r = con("set " + at(xa, y + 1, z) + " emod:test_128", &sk);
        std::snprintf(d, sizeof(d), "count %d, last id %d, 129th %d (%s), console r %d (%s), cell %d", BlockRegistry::count(), last,
                      full, why.c_str(), r, sk.all().c_str(), eden_console_getblock(xa, z, y + 1));
        check(BlockRegistry::count() == 128 && last == 255 && full < 0 && why.find("all 128") != std::string::npos &&
                  sk.has("all 128 mod block slots") && eden_console_getblock(xa, z, y + 1) == 0,
              "the 129th mod block is refused, nothing evicted or placed", d);
        if (!reload(name)) return 1;
        FileState fs = file_state(cx, cz);
        std::vector<emod::PaletteEntry> es;
        emod::palette_parse(fs.palette.data(), fs.palette.size(), &es, NULL);
        std::snprintf(d, sizeof(d), "entries %zu, verify %zu%s%s", es.size(), fs.verify.size(), fs.verify.empty() ? "" : ": ",
                      fs.verify.empty() ? "" : fs.verify[0].c_str());
        check(es.size() == 128 && fs.verify.empty() && BlockRegistry::count() == 128, "a full palette saves and verifies", d);
    }

    // ---- 7. Convert to 64z keeps the palette (K.1-4); then the next world has no palette
    const std::string docs = World::getWorld->fm->documents;
    std::string key = eden_debug_world_file();
    if (key.compare(0, docs.size() + 1, docs + "/") == 0) key = key.substr(docs.size() + 1);
    FileState before = file_state(0, 0);
    if (!quit_to_menu()) return 1;
    tick(60);
    if (g_opt.height == 256) {
        ConvertTo64Report rep = World::getWorld->fm->convertWorldTo64(key);
        emod::EdenWorldStore st;
        const bool opened = st.open((docs + "/" + key).c_str(), false);
        std::vector<uint8_t> pal;
        if (opened && st.hasRecord(emod::T_BLOCK_PALETTE)) st.readRecord(emod::T_BLOCK_PALETTE, pal);
        const std::vector<std::string> v = opened ? st.verify() : std::vector<std::string>(1, st.error());
        std::snprintf(d, sizeof(d), "ok %d (%s), opened %d, bands %d, flagged %d, palette %zu B (was %zu), verify %zu %s", rep.ok,
                      rep.error, opened, opened ? st.bands() : 0, opened && st.paletteFlag(), pal.size(), before.palette.size(), v.size(),
                      v.empty() ? "" : v[0].c_str());
        check(rep.ok && opened && st.bands() == 4 && st.paletteFlag() && !pal.empty() && pal == before.palette && v.empty(),
              "Convert to 64z: the new world is flagged and keeps the palette byte for byte", d);
    } else {
        std::printf("[eden-registry] (Convert to 64z not checked: a %d-high run)\n", g_opt.height);
    }
    const std::string other = std::string(name) + "-2";
    if (!open_world(other.c_str(), g_opt.height)) return 1;
    tick(10);
    {
        FileState fs = file_state(0, 0);
        std::snprintf(d, sizeof(d), "count %d, info128 %#x, flagged %d", BlockRegistry::count(), blockinfo[128], fs.flagged);
        check(BlockRegistry::count() == 0 && blockinfo[128] == (IS_HARD | IS_MISSING) && fs.ok && !fs.flagged,
              "the next world starts with an empty palette and the missing rows", d);
    }
    std::printf("[eden-registry] %s (%d failure(s))\n", g_selftestFailures ? "FAILURES" : "ALL PASS", g_selftestFailures);
    return g_selftestFailures ? 1 : 0;
}

}  // namespace g_registry
}  // namespace harness
