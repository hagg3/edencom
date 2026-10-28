// G.4: split out of the old eden_main_native.cpp (see harness.h). Mode group "browser".
#include "harness.h"

using namespace harness;

namespace harness {
namespace g_browser {


// ---------------------------------------------------------------------------------------------
// --browser-selftest  (ROADMAP 5.9: Get Worlds — the archive, the current and the legacy server)
// ---------------------------------------------------------------------------------------------
// OFFLINE, and that is the point: the CI runners must not depend on edengame.net being up, so
// Net_native.cpp's fixture mode answers every URL from files this test writes first —
// `<docs>/.net-fixtures/<host>/<path>@<query>`. Everything above the HTTP stack is the real thing:
// the menu's button, the browser's tabs/rows/field/buttons through pointer and SDL text events,
// the list parsers, the per-frame unpacker (a gzip, a deflate zip, a stored zip around a deflate
// zip), the import, and finally loading a downloaded world. The payload is a world this test plays
// and saves first, so "the download is a world" is checked byte-for-byte and by loading it.
//
// The legacy server gets NO fixtures on purpose: that is the "server unreachable" path.
// --net-live-selftest below runs the same screen against the real servers (not a CI gate).
extern "C" void eden_net_set_fixture_root(const char* dir);

namespace {
std::vector<std::string> g_fixtureFiles;

bool write_bytes(const std::string& path, const std::vector<unsigned char>& b) {
    const size_t slash = path.find_last_of('/');
    if (slash != std::string::npos) mkdirs(path.substr(0, slash));
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const bool ok = b.empty() || std::fwrite(b.data(), 1, b.size(), f) == b.size();
    std::fclose(f);
    g_fixtureFiles.push_back(path);
    return ok;
}
std::vector<unsigned char> bytes_of(const std::string& s) { return std::vector<unsigned char>(s.begin(), s.end()); }

std::vector<unsigned char> deflate_bytes(const std::vector<unsigned char>& in, int windowBits) {
    z_stream z;
    std::memset(&z, 0, sizeof(z));
    deflateInit2(&z, 6, Z_DEFLATED, windowBits, 8, Z_DEFAULT_STRATEGY);
    std::vector<unsigned char> out(deflateBound(&z, (uLong)in.size()) + 64);
    z.next_in = (Bytef*)in.data();
    z.avail_in = (uInt)in.size();
    z.next_out = out.data();
    z.avail_out = (uInt)out.size();
    deflate(&z, Z_FINISH);
    out.resize(z.total_out);
    deflateEnd(&z);
    return out;
}
std::vector<unsigned char> gzip_bytes(const std::vector<unsigned char>& in) { return deflate_bytes(in, 15 + 16); }

// A single-entry PK zip, the shape the archive serves: local header, data, central directory, EOCD.
// `streamed` sets general-purpose bit 3 and ZEROES the local header's sizes, as a streaming
// archiver does — the case that makes a reader trust the central directory.
std::vector<unsigned char> zip_bytes(const char* name, const std::vector<unsigned char>& data, bool deflate,
                                     bool streamed) {
    const std::vector<unsigned char> body = deflate ? deflate_bytes(data, -15) : data;
    const unsigned crc = (unsigned)crc32(0, data.data(), (uInt)data.size());
    const unsigned nlen = (unsigned)std::strlen(name);
    std::vector<unsigned char> z;
    auto u16 = [&](unsigned v) { z.push_back(v & 0xff); z.push_back((v >> 8) & 0xff); };
    auto u32 = [&](unsigned v) { u16(v & 0xffff); u16(v >> 16); };
    const unsigned method = deflate ? 8 : 0, flags = streamed ? 8 : 0;
    u32(0x04034b50); u16(20); u16(flags); u16(method); u16(0); u16(0);
    u32(streamed ? 0 : crc); u32(streamed ? 0 : (unsigned)body.size()); u32(streamed ? 0 : (unsigned)data.size());
    u16(nlen); u16(0);
    z.insert(z.end(), name, name + nlen);
    z.insert(z.end(), body.begin(), body.end());
    if (streamed) { u32(0x08074b50); u32(crc); u32((unsigned)body.size()); u32((unsigned)data.size()); }
    const unsigned cdOff = (unsigned)z.size();
    u32(0x02014b50); u16(20); u16(20); u16(flags); u16(method); u16(0); u16(0);
    u32(crc); u32((unsigned)body.size()); u32((unsigned)data.size());
    u16(nlen); u16(0); u16(0); u16(0); u16(0); u32(0); u32(0);
    z.insert(z.end(), name, name + nlen);
    const unsigned cdSize = (unsigned)z.size() - cdOff;
    u32(0x06054b50); u16(0); u16(0); u16(1); u16(1); u32(cdSize); u32(cdOff); u16(0);
    return z;
}

int menu_index_of_file(const char* file) {
    for (int i = 0; i < eden_menu_world_count(); ++i) if (!std::strcmp(eden_menu_world_file(i), file)) return i;
    return -1;
}

// Stage S / S.5: with --world-format=emod a download lands as `<id>.emod` (converted while it
// inflated) — so "the same world" means its `.eden` export equals what was served.
const char* world_ext() { return ".emod"; }
bool world_bytes(const std::string& path, std::vector<unsigned char>* out) {
    if (path.size() < 5 || path.compare(path.size() - 5, 5, ".emod") != 0) return read_file(path, out);
    emod::EdenExporter ex;
    if (!ex.begin(path, emod::ExportOptions())) return false;
    out->clear();
    unsigned char buf[1 << 16];
    for (size_t n; (n = ex.read(buf, sizeof buf)) > 0;) out->insert(out->end(), buf, buf + n);
    return !ex.failed();
}
std::vector<unsigned char> gunzip_bytes(const std::vector<unsigned char>& in) {
    z_stream z;
    std::memset(&z, 0, sizeof(z));
    inflateInit2(&z, 15 + 32);
    std::vector<unsigned char> out;
    unsigned char buf[1 << 16];
    z.next_in = (Bytef*)in.data();
    z.avail_in = (uInt)in.size();
    int rc;
    do {
        z.next_out = buf;
        z.avail_out = sizeof buf;
        rc = inflate(&z, Z_NO_FLUSH);
        out.insert(out.end(), buf, buf + (sizeof buf - z.avail_out));
    } while (rc == Z_OK);
    inflateEnd(&z);
    return rc == Z_STREAM_END ? out : std::vector<unsigned char>();
}

bool browser_wait_list(WorldBrowser* b) {
    return tick_until([b] { return !b->listLoading(); }, 3000, "the browser's list");
}

// Selects the row whose id is `id` (scrolled into view first) and taps
// Download, then waits for the browser to hand back to the menu or to report a failure.
bool browser_download(WorldBrowser* b, Menu* m, const char* id) {
    int at = -1;
    for (int i = 0; i < b->entryCount(); ++i) if (!std::strcmp(b->entryId(i), id)) at = i;
    CGRect r;
    if (at >= 0) b->showRow(at);
    if (at < 0 || !b->rowRect(at, &r)) return false;
    ui_tap_rect(r);
    tick(2);
    if (b->selectedIndex() != at) return false;
    ui_tap_rect(b->controlRect("download"));
    tick(2);
    return tick_until([b, m] { return !m->showbrowser || !b->busy(); }, 6000, "the download");
}
}  // namespace

int run_browser_selftest_body(bool live) {
    char detail[300];
    Menu* m = World::getWorld->menu;
    WorldBrowser* b = m->browser;
    const std::string docs = eden_platform_documents_root();

    if (live) {
        check(WorldBrowser::available(), "this build has a network backend", "");
        if (!WorldBrowser::available()) return 1;
        ui_menu_to(true); ui_tap_rect(m->controlRect("getworlds"));
        tick(3);
        check(m->showbrowser && b->isOpen(), "Get Worlds opens the browser", "");
        const char* names[3] = { "archive", "current server", "legacy server" };
        for (int src = 0; src < 3; ++src) {
            ui_tap_rect(b->tabRect(src));
            tick(3);
            browser_wait_list(b);
            std::snprintf(detail, sizeof(detail), "%d worlds; status \"%s\"; first \"%s\"", b->entryCount(),
                          b->statusText(), b->entryName(0));
            char what[80];
            std::snprintf(what, sizeof(what), "the %s lists worlds", names[src]);
            check(b->entryCount() > 5, what, detail);
            if (src >= 1) {                   // Recent, the paged list, on the servers
                ui_tap_rect(b->modeRect(WorldBrowser::MODE_RECENT));
                tick(3);
                browser_wait_list(b);
                std::snprintf(detail, sizeof(detail), "%d worlds; first \"%s\" (%s)", b->entryCount(), b->entryName(0), b->entryId(0));
                std::snprintf(what, sizeof(what), "the %s's Recent list", names[src]);
                check(b->entryCount() > 5, what, detail);
            }
            if (b->entryCount() > 0) {
                CGRect r;
                if (b->rowRect(0, &r)) ui_tap_rect(r);
                tick_until([b] { return b->previewShown(); }, 4000, "a preview");
                std::printf("[eden-browser] %s preview for \"%s\": %s\n", names[src], b->entryName(0),
                            b->previewShown() ? "shown" : "none (many worlds have none)");
            }
        }
        // One real download, chosen to be SMALL: the archive is the only source that publishes
        // sizes, and the first entry of a server list can be gigabytes (on 2026-10-07 the current
        // server's first Featured world inflated to 1.88 GB). HTTPS + a zip, end to end.
        ui_tap_rect(b->tabRect(WorldBrowser::SRC_ARCHIVE));
        tick(3);
        browser_wait_list(b);
        int pick = -1;
        double best = 1e18;
        for (int i = 0; i < b->entryCount(); ++i) {
            double v = 0;
            char unit[8] = {0};
            if (std::sscanf(b->entrySize(i), "%lf %7s", &v, unit) != 2 || v <= 0) continue;
            const double bytes = v * (!std::strcmp(unit, "GB") ? 1e9 : !std::strcmp(unit, "MB") ? 1e6 : !std::strcmp(unit, "KB") ? 1e3 : 1.0);
            if (bytes < best) { best = bytes; pick = i; }
        }
        const std::string id = pick >= 0 ? b->entryId(pick) : "";
        const std::string size = pick >= 0 ? b->entrySize(pick) : "";
        const int before = eden_menu_world_count();
        const bool ok = !id.empty() && browser_download(b, m, id.c_str());
        const int at = menu_index_of_file((id + world_ext()).c_str());
        std::snprintf(detail, sizeof(detail), "id %s (%s), worlds %d -> %d, status \"%s\", listed as \"%s\"", id.c_str(),
                      size.c_str(), before, eden_menu_world_count(), b->statusText(), at >= 0 ? eden_menu_world_name(at) : "-");
        check(ok && at >= 0 && !m->showbrowser, "the smallest archive world downloads, unpacks and is listed", detail);
        check(!m->showhome, "a download opened from the home screen lands on My Worlds", "");   // I.7
        if (at >= 0) {
            eden_menu_select(at);
            tick(2);
            ui_tap_rect(m->controlRect("play"));
            const bool played = tick_until([] { return game_mode() == 1; }, 60000, "game_mode == PLAY");
            check(played, "the downloaded world loads", "");
            if (played) { tick(60); quit_to_menu(); tick(10); }
            const int again = menu_index_of_file((id + world_ext()).c_str());
            if (again >= 0) { eden_menu_select(again); m->a_deleteConfirm(); }
        }
        if (m->showbrowser) { ui_tap_rect(b->controlRect("back")); tick(3); }
        return 0;
    }

    // --- a world to serve: play one, save it, read its bytes --------------------------------
    const char* name = g_opt.world.empty() ? "browser-selftest" : g_opt.world.c_str();
    if (!open_world(name, g_opt.height)) return 1;
    tick(60);
    if (!quit_to_menu()) return 1;
    tick(20);
    int src = -1;
    for (int i = 0; i < eden_menu_world_count(); ++i) if (!std::strcmp(eden_menu_world_name(i), name)) src = i;
    std::vector<unsigned char> world;
    const bool haveWorld = src >= 0 && world_bytes(docs + "/" + eden_menu_world_file(src), &world);
    check(haveWorld && world.size() > 192, "a saved world to serve", "");
    if (!haveWorld) return 1;

    // --- fixtures -----------------------------------------------------------------------------
    const std::string fx = docs + "/.net-fixtures";
    const std::string arch = fx + "/hagg3.github.io/edenarchive/assets";
    write_bytes(arch + "/data/worlds.json", bytes_of(
        "[\n"
        " {\"filename\": \"1600000001.eden\", \"worldname\": \"Selftest Alpha\", \"publishdate\": \"2020-09-13\",\n"
        "  \"archivedate\": null, \"filesize\": \"1.0 MB\", \"author\": \"Tester\", \"tags\": [\"castle\", true, 7],\n"
        "  \"url\": \"/worlds/alpha/\"},\n"
        " {\"filename\": \"1600000002.eden\", \"worldname\": \"Selftest Beta \\u00dc\", \"publishdate\": null,\n"
        "  \"archivedate\": null, \"filesize\": null, \"author\": null, \"tags\": [], \"url\": \"/worlds/beta/\"},\n"
        " {\"filename\": \"../evil.eden\", \"worldname\": \"Path traversal\", \"tags\": []}\n"
        "]\n"));
    // Alpha: a deflate zip written the streaming way (bit 3, zero sizes in the local header).
    write_bytes(arch + "/worldfiles/1600000001/1600000001.eden.zip", zip_bytes("1600000001.eden", world, true, true));
    // Beta: the "double-compressed" case the archive's own site warns about — a stored zip
    // around a deflate zip around the world.
    write_bytes(arch + "/worldfiles/1600000002/1600000002.eden.zip",
                zip_bytes("1600000002.eden.zip", zip_bytes("1600000002.eden", world, true, false), false, false));
    std::vector<unsigned char> png;
    char exe[1024] = {0};
    const char* base = SDL_GetBasePath();
    std::snprintf(exe, sizeof(exe), "%sreport_flag.png", base ? base : "");
    const bool havePng = read_file(exe, &png);
    if (havePng) write_bytes(arch + "/worldfiles/1600000001/1600000001.eden.png", png);

    const std::string app2 = fx + "/app2.edengame.net", files2 = fx + "/files2.edengame.net";
    write_bytes(files2 + "/popularlist.txt", bytes_of("1600000101.eden\nServer Alpha.name\n"));
    write_bytes(app2 + "/list2.php@start=0&sort=2", bytes_of(
        "1600000101.eden\nServer Alpha.name\n\n1600000102.eden\nBroken World.name\n"));   // a stray blank line
    write_bytes(app2 + "/list2.php@start=2&sort=2", bytes_of(
        "1600000102.eden\nBroken World.name\n1600000103.eden\nServer Gamma.name\n"));    // one repeat, one new
    write_bytes(app2 + "/list2.php@start=3&sort=2", bytes_of(""));
    write_bytes(app2 + "/list2.php@search=gamma", bytes_of("1600000103.eden\nServer Gamma.name\n"));
    write_bytes(files2 + "/1600000101.eden", gzip_bytes(world));
    write_bytes(files2 + "/1600000102.eden", bytes_of("<html><body>Not found</body></html>\n"));
    // A gzip file of two members (what `cat a.gz b.gz` makes) — still one world once inflated.
    {
        const size_t half = world.size() / 2;
        std::vector<unsigned char> a(world.begin(), world.begin() + (long)half), c(world.begin() + (long)half, world.end());
        std::vector<unsigned char> two = gzip_bytes(a), tail = gzip_bytes(c);
        two.insert(two.end(), tail.begin(), tail.end());
        write_bytes(files2 + "/1600000103.eden", two);
    }
    eden_net_set_fixture_root(fx.c_str());

    // --- the menu offers it, and it opens on the archive --------------------------------------
    tick(3);
    check(m->browserOffered(), "the menu offers Get Worlds", "");
    ui_menu_to(true); ui_tap_rect(m->controlRect("getworlds"));
    tick(3);
    check(m->showbrowser && b->isOpen() && b->source() == WorldBrowser::SRC_ARCHIVE,
          "Get Worlds opens the browser on the archive tab", "");
    browser_wait_list(b);
    std::snprintf(detail, sizeof(detail), "%d entries: \"%s\", \"%s\"", b->entryCount(), b->entryName(0), b->entryName(1));
    check(b->entryCount() == 2 && !std::strcmp(b->entryName(0), "Selftest Alpha") &&
          !std::strcmp(b->entryName(1), "Selftest Beta \xC3\x9C"),
          "the manifest parses (\\u escape, odd tags) and drops a bad id", detail);

    // The archive filters as you type: by name, then by a tag.
    ui_tap_rect(b->controlRect("search"));
    tick(2);
    push_text("beta");
    tick(3);
    std::snprintf(detail, sizeof(detail), "%d: \"%s\"", b->entryCount(), b->entryName(0));
    check(b->entryCount() == 1 && !std::strcmp(b->entryId(0), "1600000002"), "typing filters the archive by name", detail);
    for (int k = 0; k < 4; ++k) push_tap_key(SDL_SCANCODE_BACKSPACE);
    tick(2);
    push_text("castle");
    tick(3);
    std::snprintf(detail, sizeof(detail), "%d: \"%s\"", b->entryCount(), b->entryName(0));
    check(b->entryCount() == 1 && !std::strcmp(b->entryId(0), "1600000001"), "...and by tag", detail);
    for (int k = 0; k < 6; ++k) push_tap_key(SDL_SCANCODE_BACKSPACE);
    push_tap_key(SDL_SCANCODE_RETURN);
    tick(3);
    check(b->entryCount() == 2, "clearing the field shows the whole list again", "");

    // A selection fetches its preview.
    CGRect r;
    if (b->rowRect(0, &r)) ui_tap_rect(r);
    tick_until([b] { return b->previewShown(); }, 1500, "the preview");
    check(!havePng || b->previewShown(), "selecting a world shows its preview", havePng ? "" : "(no PNG to serve)");

    // --- downloads: deflate zip, nested zip, no overwrite --------------------------------------
    struct Want { const char* id; const char* file; const char* what; int source; };
    const std::string X = world_ext();
    const std::string f1 = "1600000001" + X, f2 = "1600000002" + X, f1b = "1600000001-2" + X, f3 = "1600000103" + X;
    const Want wants[] = {
        { "1600000001", f1.c_str(),  "a streamed deflate zip imports as a world", WorldBrowser::SRC_ARCHIVE },
        { "1600000002", f2.c_str(),  "a zip inside a zip imports as a world", WorldBrowser::SRC_ARCHIVE },
        { "1600000001", f1b.c_str(), "a second download never replaces the first", WorldBrowser::SRC_ARCHIVE },
    };
    std::vector<std::string> made;
    for (const Want& w : wants) {
        if (!m->showbrowser) { ui_menu_to(true); ui_tap_rect(m->controlRect("getworlds")); tick(3); browser_wait_list(b); }
        const bool done = browser_download(b, m, w.id);
        // The converted case (S.5) runs a time slice per frame; wait for it to hand back.
        tick_until([m, b] { return !m->showbrowser || !b->busy(); }, 6000, "the conversion");
        const int at = menu_index_of_file(w.file);
        std::vector<unsigned char> got;
        const bool same = world_bytes(docs + "/" + w.file, &got) && got == world;
        std::snprintf(detail, sizeof(detail), "%s: listed %d selected %d same-bytes %d browser %d status \"%s\"", w.file,
                      at, eden_menu_selected_index(), same ? 1 : 0, m->showbrowser ? 1 : 0, b->statusText());
        check(done && at >= 0 && eden_menu_selected_index() == at && same && !m->showbrowser, w.what, detail);
        if (at >= 0) made.push_back(w.file);
    }
    {
        // S.5 plan §4: the download is converted while it inflates and is gone after the commit.
        bool stray = false;
        for (const char* f : { "1600000001.eden", "1600000002.eden", "1600000001-2.eden", "1600000001.eden.zip", "1600000002.eden.zip" })
            if (FILE* t = std::fopen((docs + "/" + f).c_str(), "rb")) { std::fclose(t); stray = true; }
        check(!stray, "converted downloads leave no .eden and no download behind", "");
    }
    {
        const int at = menu_index_of_file(f1.c_str());
        std::snprintf(detail, sizeof(detail), "\"%s\"", at >= 0 ? eden_menu_world_name(at) : "-");
        check(at >= 0 && !std::strcmp(eden_menu_world_name(at), name), "a downloaded world is named from its own header", detail);
    }

    // --- the current server: Featured, Recent + More, Search, gzip, a bad payload -------------
    ui_menu_to(true); ui_tap_rect(m->controlRect("getworlds"));
    tick(3);
    ui_tap_rect(b->tabRect(WorldBrowser::SRC_CURRENT));
    tick(3);
    browser_wait_list(b);
    std::snprintf(detail, sizeof(detail), "source %d mode %d, %d entries", b->source(), b->mode(), b->entryCount());
    check(b->source() == WorldBrowser::SRC_CURRENT && b->mode() == WorldBrowser::MODE_FEATURED && b->entryCount() == 1,
          "the current server's tab opens on Featured", detail);
    ui_tap_rect(b->modeRect(WorldBrowser::MODE_RECENT));
    tick(3);
    browser_wait_list(b);
    std::snprintf(detail, sizeof(detail), "%d entries", b->entryCount());
    check(b->mode() == WorldBrowser::MODE_RECENT && b->entryCount() == 2, "Recent lists pairs past a stray blank line", detail);
    ui_tap_rect(b->controlRect("more"));
    tick(3);
    browser_wait_list(b);
    std::snprintf(detail, sizeof(detail), "%d entries", b->entryCount());
    check(b->entryCount() == 3, "More appends the next page and skips the repeat", detail);
    ui_tap_rect(b->controlRect("search"));
    tick(2);
    push_text("gamma");
    tick(2);
    push_tap_key(SDL_SCANCODE_RETURN);
    tick(3);
    browser_wait_list(b);
    std::snprintf(detail, sizeof(detail), "mode %d, %d: \"%s\"", b->mode(), b->entryCount(), b->entryName(0));
    check(b->mode() == WorldBrowser::MODE_SEARCH && b->entryCount() == 1 && !std::strcmp(b->entryId(0), "1600000103"),
          "Return runs a server search", detail);
    {
        const bool done = browser_download(b, m, "1600000103");
        tick_until([m, b] { return !m->showbrowser || !b->busy(); }, 6000, "the conversion");
        std::vector<unsigned char> got;
        const bool same = world_bytes(docs + "/" + f3, &got) && got == world;
        std::snprintf(detail, sizeof(detail), "listed %d same-bytes %d status \"%s\"", menu_index_of_file(f3.c_str()),
                      same ? 1 : 0, b->statusText());
        check(done && same && !m->showbrowser, "a two-member gzip from the server imports as a world", detail);
        if (same) made.push_back(f3);
    }
    ui_menu_to(true); ui_tap_rect(m->controlRect("getworlds"));
    tick(3);
    check(b->source() == WorldBrowser::SRC_CURRENT && b->mode() == WorldBrowser::MODE_SEARCH,
          "the browser reopens where it was", "");
    ui_tap_rect(b->modeRect(WorldBrowser::MODE_RECENT));
    tick(3);
    browser_wait_list(b);
    {
        const int before = eden_menu_world_count();
        browser_download(b, m, "1600000102");
        std::vector<unsigned char> got;
        const bool none = !read_file(docs + "/1600000102.eden", &got);
        std::snprintf(detail, sizeof(detail), "status \"%s\", worlds %d -> %d, browser %d", b->statusText(), before,
                      eden_menu_world_count(), m->showbrowser ? 1 : 0);
        check(std::strstr(b->statusText(), "not an Eden world") && none && eden_menu_world_count() == before && m->showbrowser,
              "an HTML page served as a world is refused", detail);
    }

    // --- the legacy server: nothing answers -----------------------------------------------------
    ui_tap_rect(b->tabRect(WorldBrowser::SRC_LEGACY));
    tick(3);
    browser_wait_list(b);
    std::snprintf(detail, sizeof(detail), "%d entries, status \"%s\"", b->entryCount(), b->statusText());
    check(b->source() == WorldBrowser::SRC_LEGACY && b->entryCount() == 0 && std::strstr(b->statusText(), "Couldn't reach"),
          "an unreachable server says so", detail);
    ui_tap_rect(b->controlRect("back"));
    tick(3);
    check(!m->showbrowser, "Back returns to the menu", "");

    // --- a downloaded world plays -------------------------------------------------------------
    {
        const int at = menu_index_of_file(f2.c_str());
        if (at >= 0) eden_menu_select(at);
        check(m->showhome, "Get Worlds' Back returns to the home screen it was opened from", "");   // I.7
        ui_menu_to(false);
        tick(2);
        ui_tap_rect(m->controlRect("play"));
        const bool played = at >= 0 && tick_until([] { return game_mode() == 1; }, 60000, "game_mode == PLAY");
        check(played, "a downloaded world loads and plays", "");
        if (played) { tick(60); quit_to_menu(); tick(20); }
    }

    // --- Share (S.5 / S.5c): export to Exports/ as .eden.gz, and the upload, offline -----------
    {
        const std::string srcFile = docs + "/" + eden_menu_world_file(src);
        eden_menu_select(src);
        tick(2);
        WorldNode* node = m->selected_world;
        check(node && m->shareShown(), "the menu offers Share on a selected world", "");
        const bool started = node && WorldShare::startExport(node, emod::X_OWN, emod::SIGNS_KEEP);
        tick_until([] { return !WorldShare::busy(); }, 6000, "the export");
        std::string out = WorldShare::lastResult();
        const size_t at = out.find("Exports/");
        std::string path = at == std::string::npos ? std::string() : docs + "/" + out.substr(at, out.find(" (", at) - at);
        std::vector<unsigned char> gz;
        const bool same = !path.empty() && read_file(path, &gz) && gunzip_bytes(gz) == world;
        std::snprintf(detail, sizeof(detail), "\"%s\"", out.c_str());
        check(started && out.compare(0, 4, "ok: ") == 0 && same, "Share > Export writes the world as Exports/<name>.eden.gz", detail);
        if (!path.empty()) { std::remove(path.c_str()); SDL_RemovePath((docs + "/Exports").c_str()); }

        // Upload: refused with no preview, then the multipart POST to the fixture upload2.php.
        write_bytes(app2 + "/upload2.php", bytes_of("YES"));
        const bool refused = node && !WorldShare::startUpload(node, emod::UPLOAD_CURRENT, emod::X_OWN, emod::SIGNS_KEEP) &&
                             std::strstr(WorldShare::lastResult(), "failed");
        check(refused, "an upload without a preview PNG is refused", WorldShare::lastResult());
        if (havePng) write_bytes(srcFile + ".png", png);
        const bool up = node && WorldShare::startUpload(node, emod::UPLOAD_CURRENT, emod::X_OWN, emod::SIGNS_KEEP);
        tick_until([] { return !WorldShare::busy(); }, 6000, "the upload");
        std::vector<unsigned char> posted, ignored;
        const bool gotPost = read_file(app2 + "/upload2.php.posted", &posted);
        g_fixtureFiles.push_back(app2 + "/upload2.php.posted");
        const std::string head = std::string("--") + emod::UPLOAD_BOUNDARY + "\r\nContent-Disposition: form-data; name=\"uploaded\"; filename=\"file.bin\"\r\n\r\n";
        const bool shape = gotPost && posted.size() > head.size() && std::memcmp(posted.data(), head.data(), head.size()) == 0 &&
                           posted[head.size()] == 0x1f && posted[head.size() + 1] == 0x8b;
        const bool bodyGone = !read_file(srcFile + ".upload-body", &ignored);
        std::snprintf(detail, sizeof(detail), "\"%s\", posted %zu B, multipart+gzip %d, temp body gone %d", WorldShare::lastResult(),
                      posted.size(), shape ? 1 : 0, bodyGone ? 1 : 0);
        check(!havePng || (up && !std::strcmp(WorldShare::lastResult(), "ok: Uploaded") && shape && bodyGone),
              "Share > Upload POSTs the pinned upload2.php multipart and reads YES", detail);
        std::remove((srcFile + ".png").c_str());
    }

    // --- clean up: the downloads (through the menu's own delete), the fixtures, the world -------
    eden_net_set_fixture_root("");
    made.push_back(eden_menu_world_file(src));
    for (const std::string& f : made) {
        const int at = menu_index_of_file(f.c_str());
        if (at < 0) continue;
        eden_menu_select(at);
        m->a_deleteConfirm();
    }
    // Files, then their now-empty directories, deepest first (SDL_RemovePath only takes an empty one).
    std::vector<std::string> dirs;
    for (const std::string& f : g_fixtureFiles) {
        std::remove(f.c_str());
        for (std::string d = f.substr(0, f.find_last_of('/')); d.size() > docs.size(); d = d.substr(0, d.find_last_of('/')))
            if (std::find(dirs.begin(), dirs.end(), d) == dirs.end()) dirs.push_back(d);
    }
    std::sort(dirs.begin(), dirs.end(), [](const std::string& a, const std::string& b) { return a.size() > b.size(); });
    for (const std::string& d : dirs) SDL_RemovePath(d.c_str());
    return 0;
}

int run_browser_selftest(bool live) {
    g_selftestTag = "eden-browser";
    g_tickInput = true;
    tick(120);
    const int rc = run_browser_selftest_body(live);
    std::printf("[eden-browser] %s (%d failure(s))\n", (rc || g_selftestFailures) ? "FAILURES" : "ALL PASS", g_selftestFailures);
    return (rc || g_selftestFailures) ? 1 : 0;
}

int run_keybind_selftest() {
    g_selftestTag = "eden-keybind";
    g_tickInput = true;
    const char* name = g_opt.world.empty() ? "keybind-selftest" : g_opt.world.c_str();
    if (!open_world(name, g_opt.height)) return 1;
    teleport_for_movement();
    tick(240);

    char detail[220];

    // --- the model resolves, and its codes are the scancodes this platform reports -------------
    const int miForward = eden_keybind_index("moveForward");
    const int miFlyDown = eden_keybind_index("flyDown");
    const int miCrouch  = eden_keybind_index("crouch");
    std::snprintf(detail, sizeof(detail), "%d rows; moveForward=%d flyDown=%d crouch=%d",
                  eden_keybind_count(), miForward, miFlyDown, miCrouch);
    check(miForward >= 0 && miFlyDown >= 0 && miCrouch >= 0,
          "the model resolves the actions by name", detail);
    if (miForward < 0) { std::printf("[eden-keybind] FAILURES (%d)\n", ++g_selftestFailures); return 1; }

    eden_keybind_reset_all();
    std::snprintf(detail, sizeof(detail), "moveForward default %d (%s), SDL_SCANCODE_W is %d",
                  eden_keybind_default(miForward), eden_keybind_code_name(eden_keybind_default(miForward)),
                  (int)SDL_SCANCODE_W);
    check(eden_keybind_default(miForward) == (int)SDL_SCANCODE_W,
          "a binding IS the SDL scancode, not a parallel numbering", detail);

    // --- a default double-binding fires both of its actions ------------------------------------
    {
        int hits = 0, seen[4] = {-1, -1, -1, -1};
        for (int mi = eden_keybind_action_after(-1, (int)SDL_SCANCODE_LCTRL);
             mi >= 0 && hits < 4;
             mi = eden_keybind_action_after(mi, (int)SDL_SCANCODE_LCTRL))
            seen[hits++] = mi;
        std::snprintf(detail, sizeof(detail), "Ctrl -> %d action(s): %d,%d", hits, seen[0], seen[1]);
        check(hits == 2 && ((seen[0] == miFlyDown && seen[1] == miCrouch) ||
                            (seen[0] == miCrouch  && seen[1] == miFlyDown)),
              "one key still drives both flyDown and crouch", detail);
    }

    // --- the rebind actually moves the behaviour ----------------------------------------------
    // Same measurement --input-selftest uses (rise clear, walk, measure XZ), reused verbatim so
    // "did the player move" means the same thing in both — including the retry, since the flake
    // it retires (spawning face-first into a hill) is a property of the world, not of the test.
    auto walk_with = [&](SDL_Scancode sc, float* outDist) {
        rise_until_clear(1800);
        // SETTLE FIRST. The player is flying by this point (rise_until_clear turns fly on), and
        // flying has almost no drag — so the velocity left over from the PREVIOUS measurement
        // carries into the next one. The first version of this test measured 17 blocks of pure
        // coast and read it as "W still works", which is the wrong conclusion drawn from a real
        // number: the key was already unbound. 120 idle ticks is where the drift stops changing.
        tick(120);
        PlayerState before = player_state();
        push_key(sc, true);
        tick(300);
        push_key(sc, false);
        tick(10);
        PlayerState after = player_state();
        const float dx = after.x - before.x, dz = after.z - before.z;
        *outDist = std::sqrt(dx * dx + dz * dz);
    };

    // Establish that the DEFAULT key moves the player at all before rebinding — otherwise a
    // "T moves, W does not" result below could just as well mean the world is in the way.
    float distW = 0.0f;
    walk_with(SDL_SCANCODE_W, &distW);
    if (distW <= 0.5f) { push_mouse_motion(1800.0f, 0.0f); tick(20); walk_with(SDL_SCANCODE_W, &distW); }
    std::snprintf(detail, sizeof(detail), "moved %.2f blocks on the default binding", distW);
    check(distW > 0.5f, "W moves the player before any rebind", detail);

    eden_keybind_set(miForward, (int)SDL_SCANCODE_T);
    std::snprintf(detail, sizeof(detail), "moveForward -> %d (%s)",
                  eden_keybind_get(miForward), eden_keybind_code_name(eden_keybind_get(miForward)));
    check(eden_keybind_get(miForward) == (int)SDL_SCANCODE_T, "the rebind is stored", detail);

    float distT = 0.0f;
    walk_with(SDL_SCANCODE_T, &distT);
    std::snprintf(detail, sizeof(detail), "moved %.2f blocks on the NEW binding", distT);
    check(distT > 0.5f, "T moves the player after the rebind", detail);

    // A CONTROL, not an absolute threshold. "Did not move" is not a number this harness can
    // assert directly — even settled, a flying player drifts a little, and how much depends on the
    // world. So measure a key that is bound to NOTHING (P, in no row of the table) and require
    // the now-unbound W to be no better than it. That is the claim the test actually wants, and
    // unlike a fixed epsilon it cannot be tuned into passing.
    float distControl = 0.0f, distWAfter = 0.0f;
    walk_with(SDL_SCANCODE_P, &distControl);
    walk_with(SDL_SCANCODE_W, &distWAfter);
    std::snprintf(detail, sizeof(detail), "W %.2f blocks vs %.2f for an unbound key",
                  distWAfter, distControl);
    check(distWAfter <= distControl + 1.0f && distWAfter < distT * 0.25f,
          "W is no more effective than an unbound key after the rebind", detail);

    // --- capture swallows the key instead of playing it ---------------------------------------
    // The keybinds screen arms a capture; the very next key must rebind and must NOT also be
    // dispatched as gameplay. Driven through the model rather than the screen because the screen
    // needs a window and this mode is headless — the ordering it protects (capture ahead of every
    // dispatch, Input_native.cpp's KEY_DOWN) is the part that can silently regress.
    {
        eden_keybind_capture_begin(miForward);
        check(eden_keybind_capture_active() == miForward, "capture arms", "");
        const int consumed = eden_keybind_capture_feed((int)SDL_SCANCODE_Y);
        std::snprintf(detail, sizeof(detail), "consumed=%d, moveForward now %d (%s)",
                      consumed, eden_keybind_get(miForward),
                      eden_keybind_code_name(eden_keybind_get(miForward)));
        check(consumed != 0 && eden_keybind_get(miForward) == (int)SDL_SCANCODE_Y &&
              eden_keybind_capture_active() < 0,
              "a captured key rebinds and is consumed", detail);

        // Escape CANCELS — and must leave the binding alone. This is the one key the screen
        // deliberately cannot bind to, and getting it wrong strands a player in a modal.
        eden_keybind_capture_begin(miForward);
        eden_keybind_capture_feed((int)SDL_SCANCODE_ESCAPE);
        std::snprintf(detail, sizeof(detail), "moveForward still %d", eden_keybind_get(miForward));
        check(eden_keybind_get(miForward) == (int)SDL_SCANCODE_Y && eden_keybind_capture_active() < 0,
              "Escape cancels a capture without rebinding", detail);
    }

    // --- reset puts it back -------------------------------------------------------------------
    eden_keybind_reset_all();
    std::snprintf(detail, sizeof(detail), "moveForward back to %d (%s)",
                  eden_keybind_get(miForward), eden_keybind_code_name(eden_keybind_get(miForward)));
    check(eden_keybind_get(miForward) == (int)SDL_SCANCODE_W, "reset restores the defaults", detail);

    std::printf("[eden-keybind] %s (%d failure(s))\n",
                g_selftestFailures ? "FAILURES" : "ALL PASS", g_selftestFailures);
    return g_selftestFailures ? 1 : 0;
}

}  // namespace g_browser
}  // namespace harness
