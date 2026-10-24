// BaseSelftest_native.cpp — G.2b-0: `eden_native --base-selftest --docs=<dir>`, the cross-target contract
// for the de-ObjC replacement layer (WORKING/de-objc-plan-2026-10-24.md section 4.1): strf, eden_cstr_copy
// and the path helpers against the Apple vectors measured in section 6.1, EdenFile's modes and a >4 GiB
// round trip, eden_fs_*, eden_bundle_path, and the flat prefs store. Exit 0 = all passed.
#include "../../../Classes/StrUtil.h"
#include "../../../port/src/shim/foundation/eden_base.h"
#include "../../../port/src/shim/foundation/eden_fs.h"
#include "../../../port/src/shim/foundation/eden_prefs.h"
#include "../../../port/src/shim/foundation/platform_shims.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {
int g_fail = 0, g_n = 0;
#define CHECK(c) do { ++g_n; if (!(c)) { ++g_fail; fprintf(stderr, "base-selftest FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

void test_strings() {
    CHECK(strf("%s/%s", "docs", "w") == "docs/w");
    CHECK(strf("%d-%05.1f", 7, 2.5) == "007-0002.5" || strf("%d-%05.1f", 7, 2.5) == "7-002.5");
    std::string big(1000, 'x');
    CHECK(strf("%s!", big.c_str()) == big + "!");               // past the 256-byte fast path
    CHECK(strf("%s", "(null)") == "(null)");

    char b[33];
    memset(b, 'Z', sizeof b);
    CHECK(eden_cstr_copy(b, 33, std::string(32, 'a')) && strlen(b) == 32);          // exactly 32: fits
    memset(b, 'Z', sizeof b);
    CHECK(!eden_cstr_copy(b, 33, std::string(33, 'a')) && b[0] == 0 && b[1] == 'Z'); // 33: empty, rest untouched
    memset(b, 'Z', sizeof b);
    CHECK(!eden_cstr_copy(b, 33, std::string(40, 'a')) && b[0] == 0);               // genhash-length
    char n[49];
    CHECK(!eden_cstr_copy(n, 49, std::string(60, 'n')) && n[0] == 0);               // 60-byte name
    char e[6];
    memset(e, 'Z', sizeof e);
    std::string eacute5;
    for (int i = 0; i < 5; ++i) eacute5 += "\xC3\xA9";
    CHECK(!eden_cstr_copy(e, 6, eacute5) && e[0] == 0 && e[1] == 'Z');              // 10 bytes in 6
    CHECK(!eden_cstr_copy(e, 0, "x"));

    CHECK(path_ext("x.eden.gz") == "gz");
    CHECK(path_ext("noext").empty());
    CHECK(path_ext("dir.d/noext").empty());
    CHECK(path_ext(".hidden").empty());
    CHECK(path_ext("w.emod") == "emod");
    CHECK(!path_has_suffix("abc", ""));
    CHECK(path_has_suffix("w.emod", ".emod") && !path_has_suffix("emod", ".emod"));
    CHECK(path_strip_ext("a/b/w.eden") == "a/b/w");
    CHECK(path_strip_ext("noext") == "noext");
    CHECK(path_last("a/b/w.eden") == "w.eden");
    CHECK(path_last("w") == "w");
    CHECK(path_last("a/b/") == "b");
}

void test_files(const std::string& dir) {
    const std::string f = dir + "/f.bin";
    CHECK(EdenFile::openRead(dir + "/missing") == nullptr);          // Apple: null on a miss
    CHECK(EdenFile::openUpdate(dir + "/missing") == nullptr);
    CHECK(EdenFile::openWrite(dir + "/missing") == nullptr);
    CHECK(!eden_fs_exists(dir + "/missing"));
    CHECK(eden_fs_create_empty(f) && eden_fs_size(f) == 0);

    EdenFile* w = EdenFile::openWrite(f);
    CHECK(w != nullptr);
    if (w) {
        const char a[] = "hello world";
        CHECK(w->write(a, 11));
        CHECK(w->tell() == 11 && w->seek(6));
        std::vector<uint8_t> v = w->read(5);
        CHECK(v.size() == 5 && memcmp(v.data(), "world", 5) == 0);
        CHECK(w->read(5).empty());                                   // at EOF
        CHECK(w->seekEnd() == 11);
        CHECK(w->truncate(5) && eden_fs_size(f) == 5);
        delete w;
    }
    EdenFile* r = EdenFile::openRead(f);
    CHECK(r != nullptr);
    if (r) { char b[8] = {0}; CHECK(r->read(b, 8) == 5 && memcmp(b, "hello", 5) == 0); delete r; }

    // A sparse file past 4 GiB (where the filesystem allows): write 4 bytes at 5 GiB, read them back.
    const long long off = 5LL * 1024 * 1024 * 1024;
    EdenFile* u = EdenFile::openUpdate(f);
    if (u && u->seek(off) && u->write("tail", 4)) {
        CHECK(u->seekEnd() == off + 4);
        CHECK(u->seek(off));
        char b[4] = {0};
        CHECK(u->read(b, 4) == 4 && memcmp(b, "tail", 4) == 0);
        CHECK(u->truncate(5) && eden_fs_size(f) == 5);
    } else {
        fprintf(stderr, "base-selftest: note: no sparse >4 GiB file here, leg skipped\n");
    }
    delete u;

    // eden_fs_*
    const std::string sub = dir + "/sub";
    CHECK(eden_fs_make_dir(sub) && eden_fs_is_dir(sub) && !eden_fs_is_dir(f));
    CHECK(eden_fs_copy(f, sub + "/b") && eden_fs_size(sub + "/b") == 5);
    CHECK(eden_fs_create_empty(sub + "/a") && eden_fs_create_empty(sub + "/c"));
    std::vector<std::string> l = eden_fs_list(sub);
    CHECK(l.size() == 3 && l[0] == "a" && l[1] == "b" && l[2] == "c");
    CHECK(eden_fs_move(sub + "/b", sub + "/c") && eden_fs_size(sub + "/c") == 5 && !eden_fs_exists(sub + "/b"));
    CHECK(eden_fs_remove(sub + "/a") && eden_fs_remove(sub + "/c") && eden_fs_remove(sub) && !eden_fs_exists(sub));
    CHECK(!eden_fs_remove(sub));
    CHECK(eden_fs_size(sub) == -1);
    CHECK(eden_fs_remove(f));
}

void test_bundle() {
    CHECK(eden_bundle_path("no-such-asset.zzz").empty());           // Apple: nil on a miss, not a guess
    CHECK(eden_bundle_path("").empty());
    const std::string root = eden_platform_bundle_root();
    for (const std::string& name : eden_fs_list(root)) {            // any regular file at the bundle root hits
        if (eden_fs_is_dir(root + "/" + name) || name[0] == '.') continue;
        CHECK(eden_bundle_path(name) == root + "/" + name);
        return;
    }
    fprintf(stderr, "base-selftest: note: bundle root has no plain file, hit leg skipped\n");
}

void test_prefs(const std::string& dir) {
    const std::string p = dir + "/prefs";
    eden_prefs_reset_for_test(p.c_str());
    CHECK(!eden_prefs_has("a") && eden_prefs_get_int("a", 42) == 42 && eden_prefs_get_str("a") == nullptr);
    eden_prefs_set_int("a", -7);
    eden_prefs_set_str("s", "hello");
    std::string huge(64 * 1024, 'q');
    eden_prefs_set_str("huge", huge.c_str());
    std::string bytes;
    for (int c = 1; c < 256; ++c) if (c != '\t' && c != '\n' && c != '\r') bytes.push_back((char)c);
    eden_prefs_set_str("bytes", bytes.c_str());                     // c < 256 incl. 0x1F (console history's join)
    CHECK(eden_prefs_type("a") == 1 && eden_prefs_type("s") == 2 && eden_prefs_get_int("s", 9) == 9);

    eden_prefs_reset_for_test(p.c_str());                           // forget memory, re-read the file
    CHECK(eden_prefs_get_int("a", 0) == -7);
    CHECK(eden_prefs_get_str("s") && std::string(eden_prefs_get_str("s")) == "hello");
    CHECK(eden_prefs_get_str("huge") && std::string(eden_prefs_get_str("huge")) == huge);   // not split at 1 KB
    CHECK(eden_prefs_get_str("bytes") && std::string(eden_prefs_get_str("bytes")) == bytes);
    eden_prefs_set_int("a", 1);
    eden_prefs_reset_for_test(p.c_str());
    CHECK(eden_prefs_get_int("a", 0) == 1);
    eden_prefs_reset_for_test(nullptr);
    eden_fs_remove(p);
    eden_fs_remove(p + ".tmp");
}
}  // namespace

extern "C" int eden_base_selftest_run(const char* workDir) {
    if (!workDir || !*workDir) { fprintf(stderr, "base-selftest: needs --docs=<absolute dir>\n"); return 1; }
    eden_fs_make_dir(workDir);
    const std::string dir = std::string(workDir) + "/base-selftest";
    eden_fs_make_dir(dir);
    test_strings();
    test_files(dir);
    test_bundle();
    test_prefs(dir);
    eden_fs_remove(dir);
    printf("base-selftest: %d checks, %d failed\n", g_n, g_fail);
    return g_fail ? 1 : 0;
}
