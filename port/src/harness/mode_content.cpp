// K.2-1: the content pin and the pack JSON reader's gate (WORKING/data-driven-content-plan-2026-11-01.md §6).
// Mode group "content". Neither needs a world or a window.
//
//   --content-dump   every per-type table (blockinfo, blockColor, blockTypeFaces, hudBlocksMap, blockTntMap,
//                    g_shadow_caster, the stock names), the console aliases, the two picker pages, and the
//                    *oracle*: what each Classes/BlockTraits.h decision answers for all 256 type values (and the
//                    tile swaps for every atlas tile), with both fire toggles off. Canonical text between
//                    BEGIN/END lines, then `[content-dump] sha256=<hex>` of that text.
//                    .github/scripts/check-content-dump.py pins the SHA and diffs against
//                    port/tools/fixtures/content/stock-dump.txt. K.2-2 appended the sound sets and the
//                    per-type trait rows and made the oracle a view of the tables; the `## oracle` and
//                    `## tiles` sections stay byte-identical to K.2-1's (pinned separately). From K.2-3 on
//                    it is what the `eden` pack compiles to.
//   --json-selftest  Classes/Json: accepted documents and their canonical form, round trips, and every
//                    refusal with its byte offset.
//   --pack-selftest=DIR  K.2-3: the stock pack main() loaded equals a fresh compile of the same file; every
//                    DIR/<case>/pack.json is refused with exactly the lines of DIR/<case>/expect.txt, in
//                    order (`port/tools/pack.py fixtures DIR` checks the same: one fixture set, two validators); the
//                    1 MiB cap; and a refused pack leaves the live tables as they were. K.2-4: a `mod-*` case is a
//                    mod pack (ContentPack::compileMod): its lines, and its expect.txt's `= refused` / `= blocks N`.
#include "harness.h"
#include <cstdarg>
#include "eden_fs.h"
#include "../../../Classes/BlockRegistry.h"
#include "../../../Classes/ContentPack.h"
#include "../../../Classes/BlockTraits.h"
#include "../../../Classes/Hud.h"
#include "../../../Classes/Json.h"

extern "C" int hudBlocksMap[BLOCK_IDS];
extern "C" int blockTntMap[BLOCK_IDS];
extern "C" void eden_set_fire_melts_ice(int on);
extern "C" void eden_set_fire_chars_vines(int on);

using namespace harness;

namespace harness {
namespace g_content {

namespace {

void line(std::string* o, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void line(std::string* o, const char* fmt, ...) {
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    o->append(b);
    o->push_back('\n');
}

std::string dumpText() {
    // The oracle is taken with the Experiments fire toggles off (plan D9): they flip IS_FLAMMABLE live.
    eden_set_fire_melts_ice(0);
    eden_set_fire_chars_vines(0);
    shadow_buildCasterTable();
    std::string o;
    line(&o, "# eden content dump v2 (K.2-2)");
    line(&o, "## tables");
    line(&o, "# t info color faces(6) hud tnt shadow name");
    for (int t = 0; t < BLOCK_IDS; ++t) {
        const int* f = blockTypeFaces[t];
        const char* name = BlockRegistry::stockName(t);
        line(&o, "%d %08x %d,%d,%d %d,%d,%d,%d,%d,%d %d %d %d %s", t, (unsigned)blockinfo[t],
             blockColor[t][0], blockColor[t][1], blockColor[t][2], f[0], f[1], f[2], f[3], f[4], f[5],
             hudBlocksMap[t], blockTntMap[t], g_shadow_caster[t], name ? name : "-");
    }
    line(&o, "## aliases");
    for (const std::string& a : BlockRegistry::aliasNames())
        line(&o, "%s %d", a.c_str(), BlockRegistry::stockByName(a));
    line(&o, "## picker");
    for (int page = 0; page < 2; ++page) {
        std::string cells;
        for (int i = 0; i < Hud::pickerCellCount(page); ++i) {
            if (i) cells.push_back(',');
            cells += std::to_string(Hud::pickerCell(page, i));
        }
        line(&o, "page%d %s", page, cells.c_str());
    }
    line(&o, "## oracle");
    line(&o, "# t build break land step tnt climb bounce slip blast unbreak noshadow art:chunk,batch,cursor,icon,picker,particle");
    for (int t = 0; t < BLOCK_IDS; ++t) {
        line(&o, "%d %d %d %d %d %d %d %d %d %d %d %d %d,%d,%d,%d,%d,%d", t, trait_buildSound(t), trait_breakSound(t),
             trait_landSound(t), trait_stepSound(t), trait_tntGroup(t), trait_climbable(t), trait_bouncy(t),
             trait_slippery(t), trait_blastproof(t), trait_unbreakable(t), trait_noShadow(t),
             trait_paintArtChunk(t), trait_paintArtBatch(t), trait_paintArtCursor(t), trait_paintArtIcon(t),
             trait_paintArtPicker(t), trait_paintArtParticle(t, -1));
    }
    line(&o, "## tiles");
    line(&o, "# tile twin? twin art:particle(type 0)");
    for (int bf = 0; bf < ATLAS_TILES; ++bf)
        line(&o, "%d %d %d %d", bf, trait_hasUnpaintedTwin(bf), trait_unpaintedTwin(bf), trait_paintArtParticle(TYPE_NONE, bf));
    // K.2-2: the tables the oracle above is now read from (it must not move: check-content-dump.py).
    line(&o, "## sound sets");
    line(&o, "# index name build break land step tnt");
    for (int i = 0; i < blockSoundSetCount; ++i) {
        const BlockSoundSet& ss = blockSoundSets[i];
        line(&o, "%d %s %d %d %d %d %d", i, ss.name, ss.build, ss.brk, ss.land, ss.step, ss.tnt);
    }
    line(&o, "## traits");
    line(&o, "# t sound_set paint_art(sites)");
    for (int t = 0; t < BLOCK_IDS; ++t)
        line(&o, "%d %s %02x", t, blockSoundSets[blockSoundSet[t]].name, blockPaintArt[t]);
    return o;
}

}  // namespace

// K.2-4: what --packs= dirs compile to (outside the pinned text): `pack.py compile <mod pack dirs>` prints
// the same section (CI's parity check on the example pack).
std::string modDumpText() {
    const ContentPack::ModReport& r = ContentPack::modReport();
    std::string o;
    line(&o, "## mods");
    line(&o, "# pack folder id blocks-loaded/blocks-in-file refused");
    for (size_t i = 0; i < r.packs.size(); ++i) {
        const ContentPack::ModPack& p = r.packs[i];
        int n = 0;
        for (const ContentPack::LoadedBlock& b : r.blocks) if (b.pack == (int)i) ++n;
        line(&o, "pack %s %s %d/%zu %d", p.folder.c_str(), p.id.empty() ? "-" : p.id.c_str(), n, p.blocksInFile, (int)p.refused);
        for (const std::string& l : p.lines) line(&o, "line %s", l.c_str());
    }
    for (const std::string& l : r.lines) line(&o, "loader %s", l.c_str());
    line(&o, "# block name info color faces(6) sound_set paint_art fallback paint picker");
    for (const ContentPack::LoadedBlock& b : r.blocks) {
        const BlockRegistry::BlockDef* d = BlockRegistry::findDef(b.name);
        if (!d) { line(&o, "block %s (not registered)", b.name.c_str()); continue; }
        const char* fb = BlockRegistry::stockName(d->fallbackType);
        line(&o, "block %s %08x %d,%d,%d %d,%d,%d,%d,%d,%d %s %02x %s %d %d", b.name.c_str(), (unsigned)d->info, d->color[0],
             d->color[1], d->color[2], d->faces[0], d->faces[1], d->faces[2], d->faces[3], d->faces[4], d->faces[5],
             blockSoundSets[d->soundSet].name, d->paintArt, fb ? fb : "?", d->fallbackPaint, (int)b.picker);
    }
    line(&o, "# tile atlas index name");
    for (const ContentPack::LoadedTile& t : r.tiles) line(&o, "tile %d %d %s", t.atlas, t.index, t.name.c_str());
    line(&o, "# twin tile unpainted-twin (atlas 1, mod tiles)");
    for (int i = ContentPack::ATLAS1_FREE_FIRST; i < ATLAS_TILES; ++i)
        if (tileTwin[i]) line(&o, "twin %d %d", i, tileTwin[i]);
    return o;
}

// D9 (K.2-2): the sound tables do not read IS_FLAMMABLE, so the fire toggles cannot move a sound.
// Outside the pinned text; a mismatch fails the run.
int fireToggleSoundDiffs() {
    int bad = 0;
    int was[BLOCK_IDS][5];
    for (int t = 0; t < BLOCK_IDS; ++t) {
        const int v[5] = {trait_buildSound(t), trait_breakSound(t), trait_landSound(t), trait_stepSound(t), trait_tntGroup(t)};
        std::memcpy(was[t], v, sizeof v);
    }
    eden_set_fire_melts_ice(1);
    eden_set_fire_chars_vines(1);
    for (int t = 0; t < BLOCK_IDS; ++t) {
        const int v[5] = {trait_buildSound(t), trait_breakSound(t), trait_landSound(t), trait_stepSound(t), trait_tntGroup(t)};
        if (std::memcmp(was[t], v, sizeof v)) ++bad;
    }
    eden_set_fire_melts_ice(0);
    eden_set_fire_chars_vines(0);
    return bad;
}

int run_content_dump() {
    const std::string text = dumpText();
    const int fireDiffs = fireToggleSoundDiffs();
    emod::Sha256 s;
    s.update(text.data(), text.size());
    uint8_t h[32];
    s.final(h);
    char hex[65];
    for (int i = 0; i < 32; ++i) std::snprintf(hex + 2 * i, 3, "%02x", h[i]);
    std::printf("[content-dump] BEGIN\n%s[content-dump] END\n", text.c_str());
    std::printf("[content-dump] sha256=%s bytes=%zu\n", hex, text.size());
    std::printf("[content-dump] fire toggles on: %d type(s) change a sound (D9: must be 0)\n", fireDiffs);
    if (!g_opt.packs.empty()) {
        std::fflush(stdout);   // the loader's stderr lines must not land inside the pinned text (2>&1 logs)
        ContentPack::loadMods("", false);
        std::fflush(stderr);
        std::printf("[content-dump] mods BEGIN\n%s[content-dump] mods END\n", modDumpText().c_str());
    }
    return fireDiffs ? 1 : 0;
}

// ---------------------------------------------------------------------------------------------
namespace {

int g_fail = 0;
void check(bool ok, const std::string& what) {
    if (!ok) { ++g_fail; std::printf("[json] FAIL %s\n", what.c_str()); }
}

// Accepted: the document and its canonical form.
struct Good { const char* in; const char* out; };
const Good kGood[] = {
    {"null", "null"}, {"true", "true"}, {" false ", "false"}, {"0", "0"}, {"-0", "0"}, {"42", "42"},
    {"-17", "-17"}, {"1.5", "1.5"}, {"0.25", "0.25"}, {"1e3", "1000"}, {"2E-1", "0.20000000000000001"},
    {"9007199254740993", "9007199254740992"}, {"1.5e300", "1.5000000000000001e+300"},
    {"\"\"", "\"\""}, {"\"a\\\"b\\\\c\\/d\"", "\"a\\\"b\\\\c/d\""}, {"\"\\n\\t\\r\\b\\f\"", "\"\\u000a\\u0009\\u000d\\u0008\\u000c\""},
    {"\"\\u0041\\u00e9\\u20ac\"", "\"A\xC3\xA9\xE2\x82\xAC\""}, {"\"\\ud83d\\ude00\"", "\"\xF0\x9F\x98\x80\""},
    {"\"\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80\"", "\"\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80\""},
    {"[]", "[]"}, {"{}", "{}"}, {"[1,[2,[3]],{}]", "[1,[2,[3]],{}]"},
    {"{\"b\":1,\"a\":[true,null]}", "{\"b\":1,\"a\":[true,null]}"},
    {"// a pack\n{ \"id\": \"stairs\", // the namespace\n  \"n\": [1, 2] }\n// end", "{\"id\":\"stairs\",\"n\":[1,2]}"},
    {"{\"url\":\"http://x//y\"}", "{\"url\":\"http://x//y\"}"},   // `//` inside a string is not a comment
    {"\xEF\xBB\xBF{\"a\":1}", "{\"a\":1}"},                       // leading BOM
    {"{\"a\":1}//no newline at the end", "{\"a\":1}"},
};

// Refused: the document, the byte offset of the error.
struct Bad { const char* in; size_t len; size_t off; };
#define BAD(s, off) {s, sizeof(s) - 1, off}
const Bad kBad[] = {
    BAD("", 0), BAD("   ", 3), BAD("// only a comment", 17), BAD("nul", 0), BAD("tru e", 0), BAD("x", 0),
    BAD("01", 1), BAD("-", 1), BAD("1.", 2), BAD(".5", 0), BAD("1e", 2), BAD("1e+", 3), BAD("+1", 0),
    BAD("1e999", 0), BAD("NaN", 0), BAD("Infinity", 0),
    BAD("\"abc", 4), BAD("\"a\nb\"", 2), BAD("\"\\x\"", 1), BAD("\"\\u12\"", 3), BAD("\"\\u12g4\"", 5),
    BAD("\"\\ud800\"", 1), BAD("\"\\udc00\"", 1), BAD("\"\\ud800\\u0041\"", 1),
    BAD("\"\xC0\xAF\"", 1), BAD("\"\xED\xA0\x80\"", 1), BAD("\"\xE2\x82\"", 1), BAD("\"\xFF\"", 1),
    BAD("[1,]", 3), BAD("{\"a\":1,}", 7), BAD("[1 2]", 3), BAD("{\"a\" 1}", 5), BAD("{\"a\":}", 5),
    BAD("{1:2}", 1), BAD("{\"a\":1,\"a\":2}", 7), BAD("[", 1), BAD("{", 1), BAD("[1", 2), BAD("{\"a\":1", 6),
    BAD("1 2", 2), BAD("{} x", 3), BAD("/* c */ 1", 0), BAD("1 / 2", 2), BAD("[1,/x]", 3),
};
#undef BAD

}  // namespace

int run_json_selftest() {
    g_fail = 0;
    int n = 0;
    for (const Good& g : kGood) {
        Json::Value v;
        Json::Error e;
        const bool ok = Json::parse(g.in, std::strlen(g.in), &v, &e);
        check(ok, strf("accept %s (%s at %zu)", g.in, e.message.c_str(), e.offset));
        if (!ok) continue;
        const std::string w = Json::write(v);
        check(w == g.out, strf("canonical %s -> %s (want %s)", g.in, w.c_str(), g.out));
        Json::Value v2;
        check(Json::parse(w.data(), w.size(), &v2, &e) && Json::write(v2) == w, strf("round trip %s", w.c_str()));
        ++n;
    }
    for (const Bad& b : kBad) {
        Json::Value v;
        Json::Error e;
        const bool ok = Json::parse(b.in, b.len, &v, &e);
        check(!ok, strf("refuse \"%s\"", b.in));
        if (ok) continue;
        check(e.offset == b.off, strf("offset of \"%s\": %zu (want %zu, %s)", b.in, e.offset, b.off, e.message.c_str()));
        check(v.isNull(), strf("a refused \"%s\" leaves a null value", b.in));
        ++n;
    }
    // Depth: 32 nested arrays parse, 33 do not (the offset is the 33rd '[').
    {
        std::string ok32(32, '['), bad33(33, '[');
        ok32 += std::string(32, ']');
        bad33 += std::string(33, ']');
        Json::Value v;
        Json::Error e;
        check(Json::parse(ok32.data(), ok32.size(), &v, &e), "depth 32 accepted");
        check(!Json::parse(bad33.data(), bad33.size(), &v, &e) && e.offset == 32, strf("depth 33 refused at 32 (%zu)", e.offset));
        n += 2;
    }
    // Size: exactly 1 MiB parses, one byte more does not.
    {
        std::string big = "\"" + std::string(Json::kMaxBytes - 2, 'a') + "\"";
        Json::Value v;
        Json::Error e;
        check(Json::parse(big.data(), big.size(), &v, &e) && v.str.size() == Json::kMaxBytes - 2, "1 MiB accepted");
        big.insert(big.begin(), ' ');
        check(!Json::parse(big.data(), big.size(), &v, &e) && e.offset == Json::kMaxBytes, "1 MiB + 1 refused");
        n += 2;
    }
    // An embedded NUL is a control byte inside a string and stray text outside one.
    {
        const char a[] = "\"a\0b\"", b[] = "1\0";
        Json::Value v;
        Json::Error e;
        check(!Json::parse(a, sizeof a - 1, &v, &e) && e.offset == 2, "NUL in a string refused at 2");
        check(!Json::parse(b, sizeof b - 1, &v, &e) && e.offset == 1, "NUL after a value refused at 1");
        n += 2;
    }
    // Accessors and offsets.
    {
        const char* doc = "{\"id\":\"eden\", \"n\":127, \"f\":1.5, \"neg\":-3, \"big\":1e20, \"arr\":[10,20]}";
        Json::Value v;
        Json::Error e;
        check(Json::parse(doc, std::strlen(doc), &v, &e), "accessor doc");
        const Json::Value* id = v.get("id");
        check(id && id->isString() && id->str == "eden" && id->offset == 6, "get(id) string at 6");
        int64_t x = 0;
        check(v.get("n") && v.get("n")->asInt(&x, 0, 127) && x == 127, "asInt 127 in [0,127]");
        check(v.get("n") && !v.get("n")->asInt(&x, 0, 126), "asInt 127 not in [0,126]");
        check(v.get("f") && !v.get("f")->asInt(&x), "asInt refuses 1.5");
        check(v.get("neg") && v.get("neg")->asInt(&x) && x == -3, "asInt -3");
        check(v.get("big") && !v.get("big")->asInt(&x), "asInt refuses 1e20");
        check(!v.get("missing") && !v.get("arr")->get("x"), "get on a missing key / a non-object");
        check(v.get("arr")->arr.size() == 2 && v.get("arr")->arr[1].offset == 64, "array element offset");
        n += 9;
    }
    std::printf("[json] %d checks, %d failure(s) -> %s\n", n, g_fail, g_fail ? "FAILURES" : "ALL PASS");
    return g_fail ? 1 : 0;
}

// ---------------------------------------------------------------------------------------------
int run_pack_selftest(const std::string& dir) {
    int fails = 0, n = 0;
    auto fail = [&](const std::string& what) { ++fails; std::printf("[pack] FAIL %s\n", what.c_str()); };
    // 1. What the boot loaded is what the file compiles to (and the loader read the bundle's copy).
    std::string text;
    std::vector<std::string> errs;
    ContentPack::Stock* stock = NULL;
    if (!ContentPack::readPackFile(ContentPack::stockPath(), &text)) fail("cannot re-read " + ContentPack::stockPath());
    else if (!(stock = ContentPack::compileStock(text.data(), text.size(), "eden", &errs))) fail("the stock pack does not compile: " + (errs.empty() ? std::string("?") : errs[0]));
    else {
        std::string why;
        if (!ContentPack::stockMatchesLive(*stock, &why)) fail("live tables != the stock pack: " + why);
    }
    std::printf("[pack] stock pack %s: %s\n", ContentPack::stockPath().c_str(), stock ? "compiles, equals the live tables" : "BAD");
    ++n;
    // 2. The corrupt-pack fixtures.
    int cases = 0;
    for (const std::string& c : eden_fs_list(dir)) {
        const std::string d = dir + "/" + c;
        std::string expect;
        if (!ContentPack::readPackFile(d + "/expect.txt", &expect)) continue;
        ++cases; ++n;
        std::vector<std::string> want;
        std::string verdict;   // K.2-4: a mod case's `= refused` / `= blocks N`
        size_t at = 0;
        while (at < expect.size()) {
            size_t e = expect.find('\n', at);
            if (e == std::string::npos) e = expect.size();
            std::string line = expect.substr(at, e - at);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.compare(0, 2, "= ") == 0) verdict = line.substr(2);
            else if (!line.empty() && line[0] != '#') want.push_back(line);
            at = e + 1;
        }
        std::string doc;
        if (!ContentPack::readPackFile(d + "/pack.json", &doc)) { fail(c + ": no pack.json"); continue; }
        if (c.compare(0, 4, "mod-") == 0) {
            // K.2-4: a mod pack, checked by compileMod (a bad block is skipped, not the pack).
            ContentPack::ModPack pk;
            ContentPack::compileMod(doc.data(), doc.size(), d, c, &pk);
            const std::string got = pk.refused ? "refused" : "blocks " + std::to_string(pk.blocks.size());
            if (pk.lines != want || got != verdict) {
                fail(c + ": " + (pk.lines != want ? "the lines differ from expect.txt" : "verdict " + got + ", expect.txt says " + verdict));
                for (const std::string& e : pk.lines) std::printf("[pack]     got:  %s\n", e.c_str());
                for (const std::string& w : want) std::printf("[pack]     want: %s\n", w.c_str());
            } else std::printf("[pack] ok   %s: %s\n", c.c_str(), want.empty() ? got.c_str() : want[0].c_str());
            continue;
        }
        ContentPack::Stock* s = ContentPack::compileStock(doc.data(), doc.size(), "eden", &errs);
        if (s) { ContentPack::freeStock(s); fail(c + ": accepted"); continue; }
        if (errs != want) {
            fail(c + ": the refusal differs from expect.txt");
            for (const std::string& e : errs) std::printf("[pack]     got:  %s\n", e.c_str());
            for (const std::string& w : want) std::printf("[pack]     want: %s\n", w.c_str());
        } else std::printf("[pack] ok   %s: %s\n", c.c_str(), want.empty() ? "(refused)" : want[0].c_str());
    }
    if (!cases) fail("no fixtures under " + dir);
    // 3. The size cap, without a 1 MiB file in the repo.
    {
        std::string big = "{\"format\": 1, \"id\": \"eden\", \"name\": \"" + std::string(Json::kMaxBytes, 'a') + "\"}";
        ContentPack::Stock* s = ContentPack::compileStock(big.data(), big.size(), "eden", &errs);
        const std::string want = "eden@1048576: JSON: document larger than 1 MiB";
        if (s || errs.size() != 1 || errs[0] != want) fail("1 MiB cap: " + (errs.empty() ? std::string("accepted") : errs[0]));
        ContentPack::freeStock(s);
        ++n;
    }
    // 4. Nothing above touched the live tables.
    if (stock) {
        std::string why;
        if (!ContentPack::stockMatchesLive(*stock, &why)) fail("a refused pack changed the live tables: " + why);
        ContentPack::freeStock(stock);
        ++n;
    }
    std::printf("[pack] %d checks (%d fixtures), %d failure(s) -> %s\n", n, cases, fails, fails ? "FAILURES" : "ALL PASS");
    return fails ? 1 : 0;
}

}  // namespace g_content
}  // namespace harness
