// K.0w-2: --folders-selftest, the world-folder adoption gate (WORKING/world-folders-plan-2026-10-30.md §4, §6).
// Mode group "folders".
//
//   --folders-selftest --docs=D                    fixture, adopt, verify, then the unit legs (delete, rename,
//                                                  photo, rule-4 guard, the open world is skipped)
//   --folders-selftest --folders-phase=fixture     the crash-loop fixture + D/folders-manifest.txt, exit
//   --folders-selftest --folders-phase=adopt       one adoption scan (EDEN_TEST_ADOPT_KILL_AFTER=<n> _Exits 86)
//   --folders-selftest --folders-phase=verify      one more scan (recovery), then: every world present exactly
//                                                  once with its bytes unchanged, no sidecar lost, no dot-dir
//                                                  left, and a further scan is a no-op
// port/tools/folders-crash-gate.sh loops n over every step: fixture, kill at n, verify.
#include "harness.h"
#include "../shim/foundation/eden_fs.h"
#include <sys/stat.h>
#include <map>
#include <set>
#if defined(_WIN32)
#include <sys/utime.h>
#else
#include <utime.h>
#endif

using namespace harness;

namespace harness {
namespace g_folders {

static std::string D() { return World::getWorld->fm->documents; }

static std::string sha_of(const std::string& path) {
    FILE* f = emod::io::fopen_utf8(path.c_str(), "rb");
    if (!f) return std::string();
    emod::Sha256 h;
    uint8_t buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) h.update(buf, n);
    std::fclose(f);
    uint8_t out[32];
    h.final(out);
    char hex[65];
    for (int i = 0; i < 32; ++i) std::snprintf(hex + i * 2, 3, "%02x", out[i]);
    return std::string(hex, 64);
}

static bool put_file(const std::string& path, const std::string& bytes) {
    FILE* f = emod::io::fopen_utf8(path.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    return std::fclose(f) == 0 && ok;
}

static void set_mtime(const std::string& path, long long t) {
#if defined(_WIN32)
    struct _utimbuf u; u.actime = u.modtime = (time_t)t;
    _utime(path.c_str(), &u);   // fixture paths are ASCII
#else
    struct utimbuf u; u.actime = u.modtime = (time_t)t;
    utime(path.c_str(), &u);
#endif
}

// A minimal valid `.emod`: header, empty creatures, and (srcBytes >= 0) a PROVENANCE record that pairs it
// with a plain `.eden` of that size (FileManager's emodPairsWith reads the source size at payload offset 8).
static bool write_emod(const std::string& path, const char* name, uint64_t created, long long srcBytes,
                       const std::string& srcName) {
    emod::io::remove_file(path);
    emod::EdenWorldStore st;
    if (!st.create(path.c_str(), 4, created)) return false;
    st.beginBatch();
    WorldFileHeader h;
    std::memset(&h, 0, sizeof h);
    h.level_seed = 4242;
    h.pos.x = h.home.x = h.pos.z = h.home.z = 4096 * 16 + 8; h.pos.y = h.home.y = 60;
    std::snprintf(h.name, sizeof h.name, "%s", name);
    h.version = 4;
    for (int i = 0; i < 16; ++i) h.skycolors[i] = COLOR_NORMAL_BLUE;
    std::vector<uint8_t> cr(200 * sizeof(EntityData), 0);
    for (size_t o = 0; o < cr.size(); o += sizeof(EntityData)) { int32_t t = -1; std::memcpy(&cr[o + 28], &t, 4); }
    st.putRecord(emod::T_WORLD_HEADER, &h, sizeof h);
    st.putRecord(emod::T_CREATURES, &cr[0], cr.size());
    if (srcBytes >= 0) {
        std::vector<uint8_t> p(80 + srcName.size(), 0);
        for (int i = 0; i < 8; ++i) p[8 + i] = (uint8_t)((uint64_t)srcBytes >> (8 * i));
        const uint32_t nl = (uint32_t)srcName.size();
        for (int i = 0; i < 4; ++i) p[76 + i] = (uint8_t)(nl >> (8 * i));
        std::memcpy(&p[80], srcName.data(), srcName.size());
        st.putRecord(emod::T_PROVENANCE, &p[0], p.size());
    }
    const bool ok = st.commitBatch();
    st.close();
    return ok;
}

// What the crash loop must find again: "world <sha> <label>" and "side <sha> <label>" lines.
struct Manifest { std::vector<std::pair<std::string, std::string> > worlds, sides; };

static void manifest_write(const Manifest& m) {
    std::string s;
    for (auto& w : m.worlds) s += "world " + w.first + " " + w.second + "\n";
    for (auto& w : m.sides) s += "side " + w.first + " " + w.second + "\n";
    put_file(D() + "/folders-manifest.txt", s);
}
static bool manifest_read(Manifest* m) {
    FILE* f = emod::io::fopen_utf8((D() + "/folders-manifest.txt").c_str(), "rb");
    if (!f) return false;
    char line[512];
    while (std::fgets(line, sizeof line, f)) {
        char kind[16], sha[80], label[256];
        if (std::sscanf(line, "%15s %79s %255s", kind, sha, label) != 3) continue;
        (std::strcmp(kind, "world") ? m->sides : m->worlds).push_back(std::make_pair(sha, label));
    }
    std::fclose(f);
    return true;
}

// The fixture of plan §6. `withUnknown` adds a `.migrating` dir no journal explains (it must be left alone;
// the crash loop leaves it out so "no dot-dir left" is exact).
static bool build_fixture(Manifest* m, bool withUnknown) {
    const std::string d = D(), w = d + "/Worlds";
    bool ok = true;
    auto side = [&](const std::string& path, const std::string& bytes, const char* label) {
        ok = put_file(path, bytes) && ok;
        m->sides.push_back(std::make_pair(sha_of(path), label));
    };
    auto world = [&](const std::string& path, const char* label) { m->worlds.push_back(std::make_pair(sha_of(path), label)); };

    // A hash-named world (created_unix 0 -> the id comes from the mtime) with its photo and stale temps.
    const std::string hashName = "0123456789abcdef0123456789abcdef01234567.emod";
    ok = write_emod(d + "/" + hashName, "hash world", 0, -1, "") && ok;
    set_mtime(d + "/" + hashName, 1600000000);
    world(d + "/" + hashName, "hash");
    side(d + "/" + hashName + ".png", "photo of the hash world", "hash-photo");
    ok = put_file(d + "/" + hashName + ".converting", "stale") && put_file(d + "/" + hashName + ".spill", "stale") &&
         put_file(d + "/" + hashName + ".savejrnl", "stale") && ok;
    // A converted Get Worlds download: digits keep their name.
    ok = write_emod(d + "/1316045874.emod", "download", 1316045874, -1, "") && ok;
    world(d + "/1316045874.emod", "digits");
    // A converted world, its PROVENANCE-paired original and the 2026 game's sidecars (one already adopted).
    const std::string orig(1024, 'E');
    side(d + "/paired.eden", orig, "original");
    ok = write_emod(d + "/paired.emod", "paired", 1700000100, (long long)orig.size(), "paired.eden") && ok;
    world(d + "/paired.emod", "paired");
    side(d + "/signs_paired.eden.dat", "signs sidecar", "signs");
    side(d + "/cmd_paired.eden.dat.adopted", "cmd sidecar, adopted", "cmd-adopted");
    // An unpaired `.eden` (stays a root source), a backup slot (not a world) and a damaged `.emod`.
    side(d + "/lonely.eden", std::string(512, 'L'), "lonely");
    side(d + "/old.eden.bak.zip", "a backup slot", "bak");
    side(d + "/broken.emod", "this is not an emod file at all", "broken");

    // Crash states a previous scan could have left.
    ok = eden_fs_make_dirs(w) && ok;
    // a) the world already moved: roll forward to 1700000000-rfwd.
    const std::string fwd = w + "/.1700000000-rfwd.migrating";
    ok = eden_fs_make_dir(fwd) && write_emod(fwd + "/world.emod", "rolled forward", 1700000000, -1, "") && ok;
    world(fwd + "/world.emod", "rfwd");
    side(fwd + "/photo.png", "photo of rfwd", "rfwd-photo");
    ok = put_file(fwd + "/.adopt", "rfwd.emod.png\tphoto.png\n") && ok;
    // b) the world never moved: the sidecar goes back to the root.
    const std::string bak = w + "/.1700000000-rbak.migrating";
    ok = eden_fs_make_dir(bak) && ok;
    side(bak + "/photo.png", "photo of rbak", "rbak-photo");
    ok = put_file(bak + "/.adopt", "rbak.emod.png\tphoto.png\n") && ok;
    // c) killed right after mkdir, d) killed before the journal's rename.
    ok = eden_fs_make_dir(w + "/.1700000000-empt.migrating") && ok;
    ok = eden_fs_make_dir(w + "/.1700000000-tmpj.migrating") && put_file(w + "/.1700000000-tmpj.migrating/.adopt.tmp", "x\tphoto.png\n") && ok;
    // e) not ours to judge: no journal, an unexplained file.
    if (withUnknown) ok = eden_fs_make_dir(w + "/.1700000000-unkn.migrating") && put_file(w + "/.1700000000-unkn.migrating/note.txt", "?") && ok;
    // A staging dir (K.0w-3/4 own it; adoption must not touch it).
    ok = eden_fs_make_dir(w + "/.1700000000-part.partial") && put_file(w + "/.1700000000-part.partial/download", "partial") && ok;
    return ok;
}

// Every regular file of the root and of each Worlds/<entry>/ (one level), with its SHA: path -> sha.
static std::map<std::string, std::string> tree() {
    std::map<std::string, std::string> t;
    const std::string d = D();
    for (const std::string& n : eden_fs_list(d)) {
        if (eden_fs_is_dir(d + "/" + n)) continue;
        if (n == "folders-manifest.txt") continue;
        if (n == "emod-last-run.log" || n == "eden-args.last.txt") continue;   // iOS logs into Documents and grows during the scan
        t[n] = sha_of(d + "/" + n);
    }
    for (const std::string& e : eden_fs_list(d + "/Worlds")) {
        const std::string dir = d + "/Worlds/" + e;
        if (!eden_fs_is_dir(dir)) { t["Worlds/" + e] = sha_of(dir); continue; }
        t["Worlds/" + e + "/"] = "dir";
        for (const std::string& n : eden_fs_list(dir)) t["Worlds/" + e + "/" + n] = sha_of(dir + "/" + n);
    }
    return t;
}

static std::string where_is(const std::map<std::string, std::string>& t, const std::string& sha, int* count) {
    std::string at;
    *count = 0;
    for (auto& e : t) if (e.second == sha) { ++*count; at = e.first; }
    return at;
}

// After any number of interrupted scans plus one clean one: the invariants of plan §4.
static void verify_invariants(const Manifest& m, bool expectUnknown) {
    char d[512];
    const auto t = tree();
    for (auto& w : m.worlds) {
        int n;
        const std::string at = where_is(t, w.first, &n);
        const bool inFolder = FileManager::isFolderWorld(at);
        std::snprintf(d, sizeof d, "%s: %d cop%s, at %s", w.second.c_str(), n, n == 1 ? "y" : "ies", at.c_str());
        check(n == 1 && inFolder, "each world: once, bytes unchanged, in Worlds/<id>/", d);
    }
    for (auto& s : m.sides) {
        int n;
        const std::string at = where_is(t, s.first, &n);
        std::snprintf(d, sizeof d, "%s: %d cop%s, at %s", s.second.c_str(), n, n == 1 ? "y" : "ies", at.c_str());
        check(n == 1, "each sidecar/source: present exactly once", d);
    }
    int dots = 0;
    std::string which;
    for (auto& e : t) {
        if (e.second != "dir" || e.first.compare(0, 8, "Worlds/.") != 0) continue;
        if (e.first.find(".migrating/") == std::string::npos) continue;
        if (expectUnknown && e.first == "Worlds/.1700000000-unkn.migrating/") continue;
        ++dots; which += e.first + " ";
    }
    check(dots == 0, "no .migrating dir left (but the unexplained one)", which.c_str());
    const auto before = t;
    const int again = World::getWorld->fm->adoptLooseWorlds();
    std::snprintf(d, sizeof d, "%d adopted, tree %s", again, tree() == before ? "unchanged" : "CHANGED");
    check(again == 0 && tree() == before, "a further scan is a no-op", d);
}

static int finish() {
    std::printf("[eden-folders] %s (%d failure(s))\n", g_selftestFailures ? "FAILURES" : "ALL PASS", g_selftestFailures);
    return g_selftestFailures ? 1 : 0;
}

int run_folders_selftest() {
    g_selftestTag = "eden-folders";
    FileManager* fm = World::getWorld->fm;
    const std::string phase = g_opt.foldersPhase, dd = D();
    char d[512];
    if (phase == "fixture") {
        Manifest m;
        const bool ok = build_fixture(&m, false);
        manifest_write(m);
        check(ok, "the crash-loop fixture is written", dd.c_str());
        return finish();
    }
    if (phase == "adopt") {
        const int n = fm->adoptLooseWorlds();
        std::printf("[eden-folders] scan adopted %d\n", n);
        return 0;
    }
    if (phase == "verify") {
        Manifest m;
        check(manifest_read(&m) && !m.worlds.empty(), "the manifest reads", dd.c_str());
        fm->adoptLooseWorlds();
        verify_invariants(m, false);
        return finish();
    }

    // ---- the full run ----
    Manifest m;
    check(build_fixture(&m, true), "the fixture is written", dd.c_str());
    // The open world is never adopted (Windows could not move it; the store holds the path).
    write_emod(dd + "/open.emod", "open", 0, -1, "");
    const std::string openSha = sha_of(dd + "/open.emod");
    std::string savedName = World::getWorld->terrain->world_name;
    World::getWorld->terrain->world_name = "open.emod";
    std::vector<std::pair<std::string, std::string> > moved;
    const int n = fm->adoptLooseWorlds(&moved);
    check(sha_of(dd + "/open.emod") == openSha, "the open world stays loose", "");
    for (auto& mv : moved) std::printf("[eden-folders] adopted %s -> %s\n", mv.first.c_str(), mv.second.c_str());
    std::snprintf(d, sizeof d, "%d (3 loose + 1 rolled forward)", n);
    check(n == 4 && moved.size() == 3, "the scan adopts the readable loose worlds", d);

    // The layout of plan §3.
    const std::string hashKey = FileManager::adoptedKeyOf("0123456789abcdef0123456789abcdef01234567.emod");
    const std::string pairKey = FileManager::adoptedKeyOf("paired.emod");
    check(FileManager::adoptedKeyOf("1316045874.emod") == "Worlds/1316045874/world.emod", "digits keep their name",
          FileManager::adoptedKeyOf("1316045874.emod").c_str());
    check(hashKey.compare(0, 18, "Worlds/1600000000-") == 0 && FileManager::isFolderWorld(hashKey),
          "a hash name gets <mtime>-<4 a-z> (created_unix 0)", hashKey.c_str());
    check(pairKey.compare(0, 18, "Worlds/1700000100-") == 0, "otherwise <created_unix>-<4 a-z>", pairKey.c_str());
    const std::string hdir = dd + "/" + fm->worldDirOf(hashKey), pdir = dd + "/" + fm->worldDirOf(pairKey);
    check(eden_fs_exists(hdir + "/photo.png") && !eden_fs_exists(dd + "/" + "0123456789abcdef0123456789abcdef01234567.emod.png"),
          "the photo moves in as photo.png", hdir.c_str());
    check(!eden_fs_exists(dd + "/0123456789abcdef0123456789abcdef01234567.emod.converting") &&
          !eden_fs_exists(dd + "/0123456789abcdef0123456789abcdef01234567.emod.spill") &&
          !eden_fs_exists(dd + "/0123456789abcdef0123456789abcdef01234567.emod.savejrnl"), "stale temps are deleted", "");
    check(eden_fs_exists(pdir + "/original.eden") && eden_fs_exists(pdir + "/signs.eden.dat") &&
          eden_fs_exists(pdir + "/cmd.eden.dat.adopted"), "the paired original and the sidecars move in", pdir.c_str());
    check(fm->originalOf(pairKey) == fm->worldDirOf(pairKey) + "/original.eden", "originalOf finds the folder's original",
          fm->originalOf(pairKey).c_str());
    check(eden_fs_exists(dd + "/lonely.eden") && eden_fs_exists(dd + "/old.eden.bak.zip"), "an unpaired .eden and a .bak stay in the root", "");
    check(eden_fs_exists(dd + "/broken.emod"), "a damaged .emod stays loose", "");
    check(eden_fs_exists(dd + "/Worlds/1700000000-rfwd/world.emod") && eden_fs_exists(dd + "/Worlds/1700000000-rfwd/photo.png"),
          "recovery rolls a moved world forward", "");
    check(eden_fs_exists(dd + "/rbak.emod.png") && !eden_fs_exists(dd + "/Worlds/.1700000000-rbak.migrating"),
          "recovery rolls an unmoved one back", "");
    check(!eden_fs_exists(dd + "/Worlds/.1700000000-empt.migrating") && !eden_fs_exists(dd + "/Worlds/.1700000000-tmpj.migrating"),
          "an empty / journal-less dir is removed", "");
    check(eden_fs_exists(dd + "/Worlds/.1700000000-unkn.migrating/note.txt"), "an unexplained dir is left alone", "");
    check(eden_fs_exists(dd + "/Worlds/.1700000000-part.partial/download"), "a staging dir is not adoption's", "");
    verify_invariants(m, true);

    // The list: folder keys, the damaged loose file, no dot-dir.
    World::getWorld->menu->loadWorlds();
    std::set<std::string> listed;
    for (int i = 0; i < eden_menu_world_count(); ++i) listed.insert(eden_menu_world_file(i));
    std::string ls;
    for (auto& k : listed) ls += k + " ";
    check(listed.count(hashKey) && listed.count(pairKey) && listed.count("Worlds/1316045874/world.emod") &&
          listed.count("Worlds/1700000000-rfwd/world.emod") && listed.count("open.emod") && listed.count("lonely.eden") &&
          !listed.count("broken.emod"),   // an unreadable .emod lists as before K.0w: not at all (getName "error~")
          "the list shows the folder worlds and the loose ones", ls.c_str());
    {
        long long ob = -1;
        for (WorldNode* nd = World::getWorld->menu->world_list; nd; nd = nd->next) if (nd->file_name == pairKey) ob = nd->original_bytes;
        std::snprintf(d, sizeof d, "%lld B", ob);
        check(ob == 1024, "the list row counts the folder's kept original", d);
    }
    // "Closed" now: the next scan adopts it.
    World::getWorld->terrain->world_name = savedName;
    fm->adoptLooseWorlds();
    check(FileManager::isFolderWorld(FileManager::adoptedKeyOf("open.emod")), "a closed world is adopted by the next scan",
          FileManager::adoptedKeyOf("open.emod").c_str());

    // Rename is a header record; the folder keeps its name.
    const std::string before = sha_of(dd + "/" + hashKey);
    const bool renamed = fm->renameWorld(hashKey, "Renamed World");
    check(renamed && eden_fs_exists(dd + "/" + hashKey) && fm->getName(hashKey) == "Renamed World" && sha_of(dd + "/" + hashKey) != before,
          "rename leaves the folder name alone", fm->getName(hashKey).c_str());
    // The photo path.
    check(FileManager::photoPathOf(hashKey) == fm->worldDirOf(hashKey) + "/photo.png" &&
          FileManager::photoPathOf("x.emod") == "x.emod.png", "photoPathOf: folder and loose", FileManager::photoPathOf(hashKey).c_str());
    // Rule 4: the recursive remove refuses anything but Worlds/<valid id>.
    eden_fs_make_dirs(dd + "/Exports");
    const char* bad[] = {"Worlds", "Worlds/", "Worlds/..", "Worlds/.1700000000-unkn.migrating", "Exports", "/", "..",
                         "Worlds/1316045874/..", "Worlds/a/b", ""};
    int refused = 0;
    for (const char* b : bad) refused += !fm->removeWorldDir(b);
    std::snprintf(d, sizeof d, "%d of %d refused", refused, (int)(sizeof bad / sizeof bad[0]));
    check(refused == (int)(sizeof bad / sizeof bad[0]) && eden_fs_exists(dd + "/Worlds/1316045874/world.emod") &&
          eden_fs_exists(dd + "/Exports") && eden_fs_exists(dd + "/Worlds/.1700000000-unkn.migrating/note.txt"),
          "removeWorldDir refuses every other path", d);
    // Delete removes exactly one folder.
    auto worlds = [&]() { std::vector<std::string> v = fm->folderWorldKeys(); return std::set<std::string>(v.begin(), v.end()); };
    std::set<std::string> w0 = worlds();
    const bool del = fm->deleteWorld(pairKey);
    std::set<std::string> w1 = worlds();
    w0.erase(pairKey);
    check(del && w1 == w0 && !eden_fs_exists(pdir), "delete removes exactly that folder", pdir.c_str());
    return finish();
}

}  // namespace g_folders
}  // namespace harness
