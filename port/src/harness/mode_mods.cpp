// K.3: --mods-selftest, the mod manager's model (WORKING/mod-manager-plan-2026-11-01.md §6). Headless, no world;
// give it a fresh --docs. Mode group "packs".
//
//   1. no mods.json: every Packs/ folder loads, by name; the mod-list hash is stable across loads
//   2. a pack switched off: mods.json written, the pack DISABLED, a pack that requires it NEEDS (not loaded),
//      its blocks unregistered, the hash moves; switched on again: the hash is back
//   3. order: movePack swaps neighbours (the hash follows load order), refuses past either end
//   4. mods.json: a broken one is ignored with a line; a new pack loads after the listed ones; a listed pack that
//      is not installed keeps its entry when the list is rewritten
//   5. compareVersions
//   6. worldMods: the palette's namespaces as loaded / disabled / missing
//   7. installZip: top-folder and root zips, deflate + stored, update in place (folder name kept), and every
//      refusal (unsafe name, no pack.json, refused pack, checksum, CRC, folder clash) leaves Packs/ untouched
//   8. parseCatalogue
#include <algorithm>
#include <zlib.h>
#include "harness.h"
#include "eden_fs.h"
#include "../../../Classes/BlockRegistry.h"
#include "../../../Classes/ContentPack.h"
#include "../../../Classes/PackInstall.h"

using namespace harness;

namespace harness {
namespace g_mods {
namespace {

bool writeFile(const std::string& path, const std::string& text) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    return std::fclose(f) == 0 && ok;
}
std::string readFile(const std::string& path) {
    std::string s;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return s;
    char b[65536];
    size_t n;
    while ((n = std::fread(b, 1, sizeof b, f)) > 0) s.append(b, n);
    std::fclose(f);
    return s;
}

void mkPack(const std::string& dir, const std::string& json) {
    eden_fs_make_dirs(dir);
    writeFile(dir + "/pack.json", json);
}
std::string packJson(const char* id, const char* version, const char* extra) {
    return std::string("{\"format\": 1, \"id\": \"") + id + "\", \"version\": \"" + version + "\"" + extra +
           ", \"blocks\": [{\"id\": \"cube\", \"faces\": {\"all\": \"eden:stone\"}, \"color\": [200, 200, 200],"
           " \"fallback\": \"eden:stone\"}]}\n";
}

std::string order(const std::vector<ContentPack::ModPack>& v) {
    static const char* st[] = {"", "(off)", "(x)", "(needs)", "(conflict)"};
    std::string o;
    for (const ContentPack::ModPack& p : v) o += p.folder + st[p.state] + " ";
    return o;
}
const ContentPack::ModPack* byId(const std::vector<ContentPack::ModPack>& v, const std::string& id) {
    for (const ContentPack::ModPack& p : v) if (p.id == id) return &p;
    return NULL;
}
std::vector<ContentPack::ModPack> scan(const std::string& docs, std::string* lines = NULL) {
    std::vector<std::string> l;
    std::vector<ContentPack::ModPack> v = ContentPack::scanPacks(docs, &l);
    if (lines) { lines->clear(); for (const std::string& s : l) *lines += s + "\n"; }
    return v;
}

// ---- a minimal zip writer (local headers + central directory; deflate or stored)
struct ZipFile { std::string name, body; bool deflate; };
void put16(std::string* o, unsigned v) { o->push_back((char)(v & 255)); o->push_back((char)(v >> 8 & 255)); }
void put32(std::string* o, uint32_t v) { put16(o, v & 0xffff); put16(o, v >> 16); }
std::string zipOf(const std::vector<ZipFile>& files, bool breakCrc = false) {
    std::string out, cd;
    for (const ZipFile& f : files) {
        std::string data = f.body;
        if (f.deflate) {
            z_stream s;
            std::memset(&s, 0, sizeof s);
            deflateInit2(&s, 6, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY);
            data.resize(deflateBound(&s, (uLong)f.body.size()) + 16);
            s.next_in = (Bytef*)f.body.data();
            s.avail_in = (uInt)f.body.size();
            s.next_out = (Bytef*)&data[0];
            s.avail_out = (uInt)data.size();
            deflate(&s, Z_FINISH);
            data.resize(s.total_out);
            deflateEnd(&s);
        }
        uint32_t crc = (uint32_t)crc32(0, (const Bytef*)f.body.data(), (uInt)f.body.size());
        if (breakCrc) crc ^= 1;
        const uint32_t local = (uint32_t)out.size();
        const unsigned method = f.deflate ? 8 : 0;
        put32(&out, 0x04034b50); put16(&out, 20); put16(&out, 0); put16(&out, method); put32(&out, 0);
        put32(&out, crc); put32(&out, (uint32_t)data.size()); put32(&out, (uint32_t)f.body.size());
        put16(&out, (unsigned)f.name.size()); put16(&out, 0);
        out += f.name;
        out += data;
        put32(&cd, 0x02014b50); put16(&cd, 20); put16(&cd, 20); put16(&cd, 0); put16(&cd, method); put32(&cd, 0);
        put32(&cd, crc); put32(&cd, (uint32_t)data.size()); put32(&cd, (uint32_t)f.body.size());
        put16(&cd, (unsigned)f.name.size()); put16(&cd, 0); put16(&cd, 0); put16(&cd, 0); put16(&cd, 0); put32(&cd, 0);
        put32(&cd, local);
        cd += f.name;
    }
    const uint32_t cdOff = (uint32_t)out.size();
    out += cd;
    put32(&out, 0x06054b50); put16(&out, 0); put16(&out, 0); put16(&out, (unsigned)files.size()); put16(&out, (unsigned)files.size());
    put32(&out, (uint32_t)cd.size()); put32(&out, cdOff); put16(&out, 0);
    return out;
}

// Only the expected folders in Packs/ (no staging leftovers).
std::string packsListing(const std::string& docs) {
    std::string s;
    for (const std::string& n : eden_fs_list(docs + "/Packs")) s += n + " ";
    return s;
}

}  // namespace

// K.3-5: a one-pack zip (`<id>/pack.json`, no blocks) for --ui-selftest's "Install from file" leg.
std::string ui_pack_zip(const std::string& id, const std::string& version) {
    return zipOf({{id + "/pack.json", "{\"format\": 1, \"id\": \"" + id + "\", \"name\": \"Zip " + id +
                   "\", \"version\": \"" + version + "\", \"blocks\": []}\n", true}});
}

int run_mods_selftest() {
    g_selftestTag = "eden-mods";
    char d[2048];
    const std::string docs = eden_platform_documents_root();
    const std::string packs = docs + "/Packs";
    eden_fs_remove_tree(packs);
    mkPack(packs + "/a_alpha", packJson("alpha", "1.0.0", ""));
    mkPack(packs + "/b_beta", packJson("beta", "2", ", \"requires\": [\"alpha>=1.0\"]"));
    mkPack(packs + "/c_gamma", packJson("gamma", "0.1", ""));

    // ---- 1. no list
    std::string lines;
    std::vector<ContentPack::ModPack> v = scan(docs, &lines);
    ContentPack::loadMods(docs, false);
    const std::string h0 = ContentPack::modsHash();
    ContentPack::loadMods(docs, false);
    std::snprintf(d, sizeof d, "order %s hash %s again %s lines %s", order(v).c_str(), h0.c_str(), ContentPack::modsHash().c_str(), lines.c_str());
    check(order(v) == "a_alpha b_beta c_gamma " && h0.size() == 64 && ContentPack::modsHash() == h0 && lines.empty() &&
              !eden_fs_exists(packs + "/mods.json") && BlockRegistry::findDef("beta:cube"),
          "no mods.json: every pack loads by folder name; the mod-list hash is stable", d);

    // ---- 2. off and on
    {
        const bool w = ContentPack::setPackEnabled(docs, "alpha", false);
        v = scan(docs, &lines);
        ContentPack::loadMods(docs, false);
        const std::string h1 = ContentPack::modsHash();
        const ContentPack::ModPack* b = byId(v, "beta");
        std::snprintf(d, sizeof d, "write %d order %s why %s hash %s lines %s", w, order(v).c_str(), b ? b->why.c_str() : "-", h1.c_str(), lines.c_str());
        check(w && order(v) == "a_alpha(off) b_beta(needs) c_gamma " && b && b->why == "needs \"alpha\" (not loaded)" &&
                  lines.find("b_beta: needs \"alpha\" (not loaded) (skipped)") != std::string::npos && h1.size() == 64 && h1 != h0 &&
                  !BlockRegistry::findDef("alpha:cube") && !BlockRegistry::findDef("beta:cube") && BlockRegistry::findDef("gamma:cube"),
              "a pack switched off: DISABLED, its dependant NEEDS, both unregistered, the hash moves", d);
        ContentPack::setPackEnabled(docs, "alpha", true);
        ContentPack::loadMods(docs, false);
        std::snprintf(d, sizeof d, "hash %s list %s", ContentPack::modsHash().c_str(), readFile(packs + "/mods.json").c_str());
        check(ContentPack::modsHash() == h0 && BlockRegistry::findDef("beta:cube"), "switched on again: the same hash", d);
    }

    // ---- 3. order
    {
        const bool up = ContentPack::movePack(docs, "gamma", -1);
        v = scan(docs);
        ContentPack::loadMods(docs, false);
        const std::string h2 = ContentPack::modsHash();
        const bool first = !ContentPack::movePack(docs, "alpha", -1), last = !ContentPack::movePack(docs, "beta", +1);
        const std::string o2 = order(v);
        const bool down = ContentPack::movePack(docs, "gamma", +1);
        ContentPack::loadMods(docs, false);
        std::snprintf(d, sizeof d, "up %d order %s ends %d %d down %d", up, o2.c_str(), first, last, down);
        check(up && o2 == "a_alpha c_gamma b_beta " && h2 != h0 && first && last && down && ContentPack::modsHash() == h0,
              "movePack swaps neighbours, the hash follows the order, the ends refuse", d);
    }

    // ---- 4. the list file
    {
        const std::string good = readFile(packs + "/mods.json");
        writeFile(packs + "/mods.json", "{\"packs\": [");
        v = scan(docs, &lines);
        const bool broken = order(v) == "a_alpha b_beta c_gamma " && lines.find("mods.json: JSON at byte") == 0;
        writeFile(packs + "/mods.json", good);
        ContentPack::movePack(docs, "beta", -1);   // beta before alpha: requirements do not constrain order
        mkPack(packs + "/0_new", packJson("newbie", "1", ""));
        v = scan(docs);
        const std::string o4 = order(v);
        ContentPack::removePackFolder(docs, "c_gamma");
        ContentPack::setPackEnabled(docs, "newbie", false);
        const std::string list = readFile(packs + "/mods.json");
        const bool kept = list.find("\"gamma\"") != std::string::npos && list.find("{\"id\": \"newbie\", \"enabled\": false}") != std::string::npos;
        mkPack(packs + "/c_gamma", packJson("gamma", "0.1", ""));
        std::snprintf(d, sizeof d, "broken %d (%s) order %s list %s", broken, lines.c_str(), o4.c_str(), list.c_str());
        check(broken && o4 == "b_beta a_alpha c_gamma 0_new " && kept && !ContentPack::removePackFolder(docs, "../Packs") &&
                  !ContentPack::removePackFolder(docs, "nope"),
              "a broken mods.json is ignored with a line; a new pack loads after the listed ones; an absent pack keeps its entry", d);
        ContentPack::removePackFolder(docs, "0_new");
        eden_fs_remove(packs + "/mods.json");
    }

    // ---- 5. versions
    {
        using ContentPack::compareVersions;
        const bool ok = compareVersions("1.2", "1.2.0") == 0 && compareVersions("1.10", "1.9") > 0 && compareVersions("", "0") == 0 &&
                        compareVersions("2.0-beta", "2") == 0 && compareVersions("1.0.1", "1.1") < 0 && compareVersions("v3", "1") < 0;
        check(ok, "compareVersions: dotted numbers, leading digits, missing parts 0", "");
    }

    // ---- 6. worldMods
    {
        BlockRegistry::resetWorld();
        ContentPack::loadMods(docs, false);
        std::string why;
        const int id = BlockRegistry::allocate("alpha:cube", &why);
        auto state = [&]() {
            std::string s;
            for (const ContentPack::WorldMod& m : ContentPack::worldMods(docs)) s += m.id + "=" + std::to_string(m.state) + "/" + std::to_string(m.blocks) + " ";
            return s;
        };
        const std::string s0 = state();
        ContentPack::setPackEnabled(docs, "alpha", false);
        ContentPack::loadMods(docs, false);
        const std::string s1 = state();
        eden_fs_move_new(packs + "/a_alpha", docs + "/a_alpha_away");
        ContentPack::loadMods(docs, false);
        const std::string s2 = state();
        eden_fs_move_new(docs + "/a_alpha_away", packs + "/a_alpha");
        eden_fs_remove(packs + "/mods.json");
        BlockRegistry::resetWorld();
        std::snprintf(d, sizeof d, "id %d (%s) loaded %s disabled %s missing %s", id, why.c_str(), s0.c_str(), s1.c_str(), s2.c_str());
        check(id >= 128 && s0 == "alpha=0/1 " && s1 == "alpha=1/1 " && s2 == "alpha=2/1 ",
              "worldMods: the palette's namespaces as loaded / disabled / missing", d);
    }

    // ---- 7. installZip
    {
        const std::string zdir = docs + "/zips";
        eden_fs_make_dirs(zdir);
        std::string png = readFile(std::string(eden_platform_bundle_root()) + "/packs/emod_example/textures/oak.png");
        if (png.empty()) png = readFile(std::string(eden_platform_bundle_root()) + "/media/packs/emod_example/textures/oak.png");
        auto deltaJson = [&](const char* ver, const char* id = "delta") {
            return std::string("{\"format\": 1, \"id\": \"") + id + "\", \"version\": \"" + ver +
                   "\", \"tiles\": {\"oak\": {\"png\": \"textures/oak.png\"}}, \"blocks\": [{\"id\": \"plank\", \"faces\": "
                   "{\"all\": \"oak\"}, \"color\": [255, 255, 255], \"fallback\": \"eden:wood\"}]}\n";
        };
        auto install = [&](const std::string& name, const std::string& zip, const std::string& sha = "") {
            writeFile(zdir + "/" + name, zip);
            return PackInstall::installZip(zdir + "/" + name, docs, sha);
        };
        const std::string before = packsListing(docs);
        PackInstall::Result r1 = install("d1.zip", zipOf({{"delta-1.0/pack.json", deltaJson("1.0"), true},
                                                          {"delta-1.0/textures/oak.png", png, false},
                                                          {"__MACOSX/delta-1.0/._pack.json", "junk", false},
                                                          {"delta-1.0/.DS_Store", "junk", false}}));
        const bool same = readFile(packs + "/delta/textures/oak.png") == png && readFile(packs + "/delta/pack.json") == deltaJson("1.0");
        PackInstall::Result r2 = install("d2.zip", zipOf({{"pack.json", deltaJson("1.1"), true}, {"textures/oak.png", png, true}}));
        eden_fs_move_new(packs + "/delta", packs + "/my_delta");
        const std::string zip3 = zipOf({{"pack.json", deltaJson("1.2"), false}, {"textures/oak.png", png, false}});
        const std::string sha3 = PackInstall::fileSha256((writeFile(zdir + "/d3.zip", zip3), zdir + "/d3.zip"));
        PackInstall::Result bad = install("d3.zip", zip3, std::string(64, '0'));
        PackInstall::Result r3 = install("d3.zip", zip3, sha3);
        v = scan(docs);
        const ContentPack::ModPack* dp = byId(v, "delta");
        std::snprintf(d, sizeof d, "r1 %d %s %s rep %d same %d | r2 %d %s rep %d | sha-bad %d %s | r3 %d %s %s rep %d | delta %s %s | listing %s",
                      r1.ok, r1.folder.c_str(), r1.error.c_str(), r1.replaced, same, r2.ok, r2.folder.c_str(), r2.replaced, bad.ok,
                      bad.error.c_str(), r3.ok, r3.folder.c_str(), r3.version.c_str(), r3.replaced, dp ? dp->folder.c_str() : "-",
                      dp ? dp->version.c_str() : "-", packsListing(docs).c_str());
        check(r1.ok && r1.folder == "delta" && !r1.replaced && same && r2.ok && r2.folder == "delta" && r2.replaced && !bad.ok &&
                  bad.error.find("checksum") != std::string::npos && r3.ok && r3.folder == "my_delta" && r3.replaced &&
                  dp && dp->folder == "my_delta" && dp->version == "1.2" && dp->state == ContentPack::PACK_LOADED &&
                  !eden_fs_exists(packs + "/delta") && !eden_fs_exists(packs + "/.install.partial") && !eden_fs_exists(packs + "/.install.old"),
              "installZip: a top-folder zip, a root zip, update in place keeping the folder name, the catalogue checksum", d);

        const std::string listing = packsListing(docs);
        struct Bad { const char* what; std::string zip; const char* expect; };
        mkPack(packs + "/zeta", packJson("other", "1", ""));
        const std::string listing2 = packsListing(docs);
        std::vector<Bad> bads = {
            {"unsafe", zipOf({{"pack.json", deltaJson("9"), false}, {"../evil.png", png, false}}), "unsafe name"},
            {"abs", zipOf({{"/pack.json", deltaJson("9"), false}}), "unsafe name"},
            {"none", zipOf({{"a/readme.txt", "hi", false}}), "no pack.json"},
            {"two-tops", zipOf({{"a/pack.json", deltaJson("9"), false}, {"b/x.png", png, false}}), "no pack.json"},
            {"refused", zipOf({{"pack.json", deltaJson("9", "eden"), false}, {"textures/oak.png", png, false}}), "has errors"},
            {"crc", zipOf({{"pack.json", deltaJson("9"), true}, {"textures/oak.png", png, false}}, true), "checksum"},
            {"clash", zipOf({{"pack.json", deltaJson("1", "zeta"), false}, {"textures/oak.png", png, false}}), "already in Packs"},
            {"junk", std::string("PK\x03\x04 not really"), "not a zip"},
        };
        std::string got;
        bool allOk = true;
        for (const Bad& b : bads) {
            PackInstall::Result r = install(std::string(b.what) + ".zip", b.zip);
            const bool ok = !r.ok && r.error.find(b.expect) != std::string::npos && packsListing(docs) == listing2;
            allOk &= ok;
            got += std::string(b.what) + (ok ? " ok" : " FAIL") + " (" + r.error + "); ";
        }
        PackInstall::Result refused = install("refused.zip", bads[4].zip);
        std::snprintf(d, sizeof d, "%s lines %zu; listing %s", got.c_str(), refused.lines.size(), packsListing(docs).c_str());
        check(allOk && !refused.lines.empty() && listing2 != listing,
              "installZip refusals (unsafe name, absolute, no pack.json, two tops, refused pack, CRC, folder clash, junk) leave Packs/ untouched", d);
        ContentPack::removePackFolder(docs, "zeta");
        eden_fs_remove_tree(zdir);
    }

    // ---- 8. the catalogue
    {
        const std::string sha(64, 'a');
        const std::string cat = "{\"format\": 1, \"packs\": [{\"id\": \"stairs\", \"name\": \"Stairs\", \"version\": \"1.0\", \"url\": "
                                "\"https://example.org/stairs.zip\", \"sha256\": \"" + sha + "\", \"size\": 1234, \"requires\": [\"base>=1\"]},"
                                "{\"id\": \"nosha\", \"url\": \"https://example.org/x.zip\"},"
                                "{\"id\": \"ftp\", \"url\": \"ftp://example.org/x.zip\", \"sha256\": \"" + sha + "\"},"
                                "{\"id\": \"stairs\", \"url\": \"https://example.org/dup.zip\", \"sha256\": \"" + sha + "\"},"
                                "{\"id\": \"eden\", \"url\": \"https://example.org/e.zip\", \"sha256\": \"" + sha + "\"}]}";
        std::vector<PackInstall::CatalogueEntry> out;
        int dropped = 0;
        std::string err;
        const bool ok = PackInstall::parseCatalogue(cat.data(), cat.size(), &out, &dropped, &err);
        const bool notJson = !PackInstall::parseCatalogue("<html>", 6, &out, &dropped, &err);
        PackInstall::parseCatalogue(cat.data(), cat.size(), &out, &dropped, &err);
        std::snprintf(d, sizeof d, "ok %d n %zu dropped %d notJson %d (%s)", ok, out.size(), dropped, notJson, err.c_str());
        check(ok && out.size() == 1 && out[0].id == "stairs" && out[0].size == 1234 && out[0].needs.size() == 1 && dropped == 4 && notJson,
              "parseCatalogue keeps valid entries, drops no-sha / non-http / duplicate / reserved ones, refuses non-JSON", d);
    }
    // ---- 9. the bundled example pack (WB): copied once, the marker remembers, a removal sticks, an id that
    // another folder already holds is not copied again
    {
        const std::string bundle = eden_platform_bundle_root();
        eden_fs_remove_tree(packs);
        std::vector<std::string> l1, l2, l3, l4;
        const int n1 = ContentPack::seedBundledPacks(docs, bundle, &l1);
        std::vector<ContentPack::ModPack> sv = scan(docs);
        const ContentPack::ModPack* w = byId(sv, "wonders");
        const bool loaded = w && w->state == ContentPack::PACK_LOADED && w->scripts.size() == 13 && w->blocks.size() == 23;
        const bool marked = readFile(packs + "/.bundled") == "wonders\n" && !eden_fs_exists(packs + "/.seed.partial");
        const int n2 = ContentPack::seedBundledPacks(docs, bundle, &l2);
        const bool removed = ContentPack::removePackFolder(docs, "wonders");
        const int n3 = ContentPack::seedBundledPacks(docs, bundle, &l3);
        const bool stayedGone = !eden_fs_exists(packs + "/wonders");
        eden_fs_remove(packs + "/.bundled");
        mkPack(packs + "/my_copy", packJson("wonders", "0.1", ""));
        const int n4 = ContentPack::seedBundledPacks(docs, bundle, &l4);
        const bool notDuplicated = !eden_fs_exists(packs + "/wonders") && readFile(packs + "/.bundled") == "wonders\n";
        std::snprintf(d, sizeof d, "n %d %d %d %d loaded %d marked %d removed %d gone %d dup %d first '%s' last '%s'", n1, n2, n3, n4,
                      loaded, marked, removed, stayedGone, notDuplicated, l1.empty() ? "" : l1[0].c_str(), l4.empty() ? "" : l4[0].c_str());
        check(n1 == 1 && loaded && marked && n2 == 0 && l2.empty() && removed && n3 == 0 && stayedGone && n4 == 0 && notDuplicated &&
                  !l4.empty() && l4[0] == "wonders: already in Packs",
              "seedBundledPacks: Wonder Blocks copied once and loads; a removal sticks; a folder holding its id is enough", d);
    }
    eden_fs_remove_tree(packs);
    ContentPack::loadMods(docs, false);
    std::printf("[eden-mods] %s (%d failure(s))\n", g_selftestFailures ? "FAILURES" : "ALL PASS", g_selftestFailures);
    return g_selftestFailures ? 1 : 0;
}

}  // namespace g_mods
}  // namespace harness
