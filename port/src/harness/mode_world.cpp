// G.4: split out of the old eden_main_native.cpp (see harness.h). Mode group "world".
#include "harness.h"

using namespace harness;

namespace harness {
namespace g_world {


// ---------------------------------------------------------------------------------------------
// --touch-selftest  (Phase N Stage 4.3, the SDL touch translator)
// ---------------------------------------------------------------------------------------------
// Touch is the one input path this port has always had and has never been able to TEST outside a
// browser: the engine half is the original 2010 code, the translator half was `public/eden-input.js`
// only, and a desktop leg has no fingers. SDL_PushEvent does, so this drives the same four events
// UIKit delivers on a device and asserts the effect at the far end.
//
// **IT USES THE ENGINE'S OWN HUD RECT, not a guessed coordinate.** eden_debug_display_state()
// reports `rmenu` (the in-game menu corner icon) in the same point space the touch core consumes,
// so this cannot pass by hitting the wrong thing or fail because a layout constant moved — which
// is exactly the failure mode a hard-coded "tap at 0.9, 0.1" would have.
//
// TWO COORDINATE FACTS, both of which are one-line bugs if they are wrong:
//   * SDL reports finger positions NORMALISED to the window (0..1); the mouse path reports window
//     points. The translator multiplies by the point space directly for that reason.
//   * HUD rects are BOTTOM-LEFT origin and the pointer API is TOP-LEFT (see
//     eden_tap_hud_button_begin, which flips with SCREEN_HEIGHT - cy). The flip belongs to the
//     caller, so this test does it too — and if the translator ever grows its own flip, this test
//     is what catches the double one.
// Defined further down, next to --input-selftest/--gamepad-selftest, which is where the rest of
// the movement harness lives. Declared here so the joystick checks below can reuse it rather than
// grow a second copy — the map-border clamp lesson (see teleport_for_movement) applies to any test
// that asks whether the player moved.

extern "C" void eden_set_fly_mode(int);
extern "C" void eden_test_force_touch_input(void);   // Settings.cpp (X.19)
extern "C" void eden_prefs_set_ephemeral(int on);    // Prefs_native.cpp (X.21)
// ---------------------------------------------------------------------------------------------
// --empty-bit-selftest  (Stage R / R.2, the per-chunk empty bit)
// ---------------------------------------------------------------------------------------------
// --stage1 proves the shortcut changes nothing on a world nobody edits. This covers the two
// transitions an edit makes, through the same Terrain::updateChunks entry point a build/mine
// reaches: a block placed in an all-air chunk must clear the bit BEFORE the next mesh decision
// (else rebuild2() skips the scan and the block is invisible), and the chunk must rejoin the
// shortcut once it is air again (rebuild2()'s scan refreshes the bit; setLand never sets it).
int run_empty_bit_selftest() {
    g_selftestTag = "eden-empty";
    // NOT --empty-selfcheck's mode: that runs the full scan on a chunk the bit calls empty, which
    // would mesh the block below even with a stale bit and make the test pass vacuously.
    const char* name = g_opt.world.empty() ? "empty-bit-selftest" : g_opt.world.c_str();
    if (!open_world(name, g_opt.height)) return 1;
    eden_console_teleport(g_opt.at[0], g_opt.at[1], g_opt.at[2]);
    tick(g_opt.frames / 2);

    // Top band of the window, a few blocks off the player: air on the bundled map at any height.
    // Off the player's REAL position -- the default --at is clamped to the map corner (see the
    // default-origin comment in eden_main_after_args), so --at itself is not where it lands.
    const PlayerState ps = player_state();
    const int x = (int)ps.x + 5, z = (int)ps.z + 5, y = g_opt.height - 3;
    char d[256];
    auto st = [&] { return eden_debug_chunk_empty(x, z, y); };
    long long s0 = st();
    std::snprintf(d, sizeof(d), "chunk at (%d,%d,%d): probe %lld", x, z, y, s0);
    check(s0 >= 0 && (s0 & 3) == 3, "target chunk is air and marked empty", d);
    if (s0 < 0 || (s0 & 3) != 3) {
        std::printf("[eden-empty] FAILURES (%d failure(s))\n", g_selftestFailures);
        return 1;
    }

    eden_console_setblock(x, z, y, 2 /* TYPE_STONE */);
    long long s1 = st();
    std::snprintf(d, sizeof(d), "probe %lld", s1);
    check((s1 & 3) == 0, "placing a block clears the bit at once", d);
    tick(30);
    long long s2 = st();
    std::snprintf(d, sizeof(d), "getblock %d, vertices %lld -> %lld",
                  eden_console_getblock(x, z, y), s0 >> 2, s2 >> 2);
    check(eden_console_getblock(x, z, y) == 2 && (s2 >> 2) > (s0 >> 2),
          "the placed block meshes (vertices grew)", d);

    eden_console_setblock(x, z, y, 0);
    tick(30);
    long long s3 = st();
    std::snprintf(d, sizeof(d), "probe %lld, vertices %lld", s3, s3 >> 2);
    check((s3 & 3) == 3 && (s3 >> 2) == (s0 >> 2), "removing it re-marks the chunk empty", d);

    const char* cs = eden_debug_chunk_state();
    const char* k1 = std::strstr(cs, "\"stale\":");
    const char* k2 = std::strstr(cs, "\"selfcheck_stale\":");
    check(k1 && atoi(k1 + 8) == 0 && k2 && atoi(k2 + 18) == 0,
          "window audit: no stale bits, self-check clean", cs);

    std::printf("[eden-empty] %s (%d failure(s))\n",
                g_selftestFailures ? "FAILURES" : "ALL PASS", g_selftestFailures);
    return g_selftestFailures ? 1 : 0;
}

// ---------------------------------------------------------------------------------------------
// --newblocks-selftest  (Stage D / D.2a, ids 112-127 in the engine's tables)
// ---------------------------------------------------------------------------------------------
// Until D.2a, NUM_BLOCKS was 111 and rebuild2()'s corrupt-byte repair turned every 2026 block it
// meshed into stone, in the world, kept by the next save (D.2c). This builds each new id through
// the real build path in air near the player, asserts it stays itself and meshes, saves, reloads
// and asserts every id comes back. Runs on either container (add --world-format=emod).
int run_newblocks_selftest() {
    g_selftestTag = "eden-newblocks";
    const char* name = g_opt.world.empty() ? "newblocks-selftest" : g_opt.world.c_str();
    if (!open_world(name, g_opt.height)) return 1;
    eden_console_teleport(g_opt.at[0], g_opt.at[1], g_opt.at[2]);
    tick(g_opt.frames / 2);

    // Top band, off the player's REAL position (see --empty-bit-selftest), two apart so every
    // block has all six faces exposed.
    const PlayerState ps = player_state();
    const int x0 = (int)ps.x + 5, z = (int)ps.z + 5, y = g_opt.height - 3;
    char d[256];
    for (int id = 112; id <= 127; id++) {
        const int x = x0 + (id - 112) * 2;
        const long long v0 = eden_debug_chunk_empty(x, z, y) >> 2;
        eden_debug_build_block(x, z, y, id);
        tick(30);
        const long long v1 = eden_debug_chunk_empty(x, z, y) >> 2;
        std::snprintf(d, sizeof(d), "id %d at (%d,%d,%d): getblock %d, vertices %lld -> %lld",
                      id, x, z, y, eden_console_getblock(x, z, y), v0, v1);
        check(eden_console_getblock(x, z, y) == id && v1 > v0, "a new id builds, stays itself and meshes", d);
    }
    save_world();
    if (!quit_to_menu()) return 1;
    tick(120);
    if (!open_world(name, g_opt.height)) return 1;
    eden_console_teleport(g_opt.at[0], g_opt.at[1], g_opt.at[2]);
    tick(g_opt.frames / 2);
    int back = 0;
    for (int id = 112; id <= 127; id++) {
        const int x = x0 + (id - 112) * 2;
        const int got = eden_console_getblock(x, z, y);
        const long long v = eden_debug_chunk_empty(x, z, y) >> 2;
        std::snprintf(d, sizeof(d), "id %d at (%d,%d,%d): getblock %d, chunk vertices %lld", id, x, z, y, got, v);
        check(got == id && v > 0, "after save + reload the id is still there and meshes", d);
        back += (got == id);
    }
    std::printf("[eden-newblocks] %d/16 ids back after reload\n", back);
    std::printf("[eden-newblocks] %s (%d failure(s))\n",
                g_selftestFailures ? "FAILURES" : "ALL PASS", g_selftestFailures);
    return g_selftestFailures ? 1 : 0;
}

// ---------------------------------------------------------------------------------------------
// --trailer-selftest  (Stage D / D.3a, the trailer model on both containers)
// ---------------------------------------------------------------------------------------------
namespace trailer_test {

bool is_emod_file(const std::string& path) {
    FILE* f = emod::io::fopen_utf8(path.c_str(), "rb");
    if (!f) return false;
    char m[8] = {0};
    const bool ok = std::fread(m, 1, 8, f) == 8 && std::memcmp(m, "EMODWLD", 8) == 0;
    std::fclose(f);
    return ok;
}

// The trailer exactly as the file holds it: a `.eden`'s gate-failing directory tail (the engine's own
// layout reader) or an `.emod`'s live SIGN_TRAILER record. `cols` gets one digest over every column.
bool read_world(const std::string& path, std::vector<uint8_t>& trailer, std::string* colDigest, std::string& err) {
    trailer.clear();
    emod::Sha256 h;
    if (is_emod_file(path)) {
        emod::EdenWorldStore st;
        if (!st.open(path.c_str(), false)) { err = st.error(); return false; }
        if (st.hasRecord(emod::T_SIGN_TRAILER) && st.readRecord(emod::T_SIGN_TRAILER, trailer) != emod::EdenWorldStore::READ_OK) {
            err = "SIGN_TRAILER does not decode"; return false;
        }
        if (colDigest) {
            std::vector<uint8_t> col((size_t)st.bands() * emod::BAND);
            for (const auto& k : st.columnKeys()) {
                st.readColumn(k.first, k.second, &col[0]);
                h.update(&k, sizeof(k));
                h.update(&col[0], col.size());
            }
        }
    } else {
        FILE* f = emod::io::fopen_utf8(path.c_str(), "rb");
        if (!f) { err = "cannot open"; return false; }
        const uint64_t size = emod::io::file_size(f);
        uint8_t hdr[emod::EDEN_HEADER];
        emod::io::seek_to(f, 0);
        if (std::fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) { std::fclose(f); err = "short header"; return false; }
        uint64_t dirOff; std::memcpy(&dirOff, hdr + 32, 8);
        if (dirOff < sizeof(hdr) || dirOff > size) { std::fclose(f); err = "bad directory offset"; return false; }
        std::vector<uint8_t> dir((size_t)(size - dirOff));
        emod::io::seek_to(f, dirOff);
        if (!dir.empty() && std::fread(&dir[0], 1, dir.size(), f) != dir.size()) { std::fclose(f); err = "short directory"; return false; }
        emod::EdenLayout L;
        if (!emod::eden_read_layout(hdr, size, dir, L)) { std::fclose(f); err = "layout"; return false; }
        trailer = L.trailer;
        if (colDigest) {
            std::vector<emod::EdenLayoutCol> cols = L.cols;
            std::sort(cols.begin(), cols.end(), [](const emod::EdenLayoutCol& a, const emod::EdenLayoutCol& b) {
                return a.x != b.x ? a.x < b.x : a.z < b.z; });
            std::vector<uint8_t> col((size_t)L.bands * emod::BAND);
            for (const auto& c : cols) {
                std::fill(col.begin(), col.end(), 0);
                emod::io::seek_to(f, c.off);
                std::fread(&col[0], 1, (size_t)std::min<uint64_t>(c.span, col.size()), f);
                int32_t k[2] = {c.x, c.z};
                h.update(k, sizeof(k));
                h.update(&col[0], col.size());
            }
        }
        std::fclose(f);
    }
    if (colDigest) {
        uint8_t d[32]; h.final(d);
        char hex[65];
        for (int i = 0; i < 32; i++) std::snprintf(hex + 2 * i, 3, "%02x", d[i]);
        *colDigest = hex;
    }
    return true;
}

std::vector<uint8_t> rows_of(const std::vector<uint8_t>& payload) {   // tag a payload into rows
    std::vector<uint8_t> p = payload, out;
    p.resize((p.size() + 11) / 12 * 12, 0);
    for (size_t i = 0; i < p.size(); i += 12) {
        static const uint8_t tag[4] = {0xff, 0xff, 0xff, 0xff};
        out.insert(out.end(), tag, tag + 4);
        out.insert(out.end(), p.begin() + i, p.begin() + i + 12);
    }
    return out;
}

void synthetic() {
    char d[256];
    // 1. a new world: CMB1 goes in front of SGN1 whatever order the edits come in; empty = no trailer
    WorldTrailer t;
    check(t.parsed() && !t.dirty() && t.encode().empty(), "a cleared trailer is empty, editable and clean", "");
    const TrailerPos p0 = TrailerPos::fromTerrainArgs(65600, 65610, 40);
    check(t.addSign(p0, 3, 45, 2, "hello") == WorldTrailer::TR_OK, "addSign on a new world", "");
    check(t.addCmd(p0, "@touch;say hi") == WorldTrailer::TR_OK, "addCmd on a new world", "");
    std::vector<uint8_t> e = t.encode();
    check(t.dirty() && e.size() == (size_t)(24 + 528 + 24 + 120) / 12 * 16 && e.size() % 16 == 0,
          "a new world's trailer is CMB1 + SGN1 rows", "");
    check(e.size() > 8 && std::memcmp(&e[4], "CMB1", 4) == 0, "CMB1 is written first (the 2026 game's order)", "");
    WorldTrailer r;
    check(r.load(e.data(), e.size()) && r.signCount() == 1 && r.cmdCount() == 1 && r.rebuild() == e,
          "it reads back, and re-serialises byte-identical", r.describe().c_str());
    // 2. the coordinate helper: file (x,y,z) = Terrain (x,z,y) = engine (x, y_up=z, z=y)
    int ex, ey, ez; p0.toEngine(&ex, &ey, &ez);
    TrailerPos back = TrailerPos::fromEngine(ex, ey, ez);
    std::snprintf(d, sizeof(d), "engine (%d,%d,%d)", ex, ey, ez);
    check(ex == 65600 && ey == 40 && ez == 65610 && back == p0, "file <-> engine coordinates", d);
    // 3. the index, and the anchor-removal hook
    check(t.hasAnchored(p0) && !t.hasAnchored(TrailerPos::fromTerrainArgs(65600, 65610, 41)), "hasAnchored is exact", "");
    check(t.addSign(p0, 3, 45, 2, "again") == WorldTrailer::TR_EXISTS && t.addSign(p0, 1, 45, 2, "side") == WorldTrailer::TR_OK,
          "one sign per (anchor, face)", "");
    check(t.addSign(p0, 6, 45, 2, "x") == WorldTrailer::TR_BAD && t.addSign(p0, 0, 0, 2, "x") == WorldTrailer::TR_BAD &&
          t.addSign(p0, 0, 45, 2, "tab\there") == WorldTrailer::TR_BAD &&
          t.addSign(p0, 0, 45, 2, std::string(96, 'x').c_str()) == WorldTrailer::TR_BAD &&
          t.addSign(p0, 0, 45, 2, std::string(95, 'x').c_str()) == WorldTrailer::TR_OK,
          "face/colour/text validated (95 printable ASCII fits, 96 does not)", "");
    std::snprintf(d, sizeof(d), "removed %d", t.removeAnchored(p0));
    check(t.signCount() == 0 && t.cmdCount() == 0 && t.encode().empty(), "removeAnchored takes every sign and the command block", d);
    // 4. an edit that empties a section drops it; the untouched section stays as read; markSaved holds
    WorldTrailer m;
    m.load(e.data(), e.size());
    m.removeCmd(0);
    std::vector<uint8_t> me = m.encode();
    check(me.size() == (size_t)(24 + 120) / 12 * 16 && std::memcmp(&me[4], "SGN1", 4) == 0, "an emptied CMB1 is left out", "");
    m.markSaved();
    check(!m.dirty() && m.rebuild() == me && m.encode() == me, "after markSaved the model rebuilds to what was written", "");
    // 5. opaque: kept verbatim, every edit refused
    std::vector<uint8_t> bad = e;
    bad[16] = 0;                                    // the second row's tag
    WorldTrailer o;
    check(!o.load(bad.data(), bad.size()) && o.rebuild() == bad && o.encode() == bad && !o.dirty(),
          "an unreadable trailer is opaque and kept byte for byte", o.why().c_str());
    check(o.addSign(p0, 3, 45, 0, "x") == WorldTrailer::TR_OPAQUE && o.removeAnchored(p0) == 0 && o.encode() == bad,
          "an opaque trailer refuses every edit", "");
    // an unknown section is carried verbatim in its slot through an edit
    std::vector<uint8_t> pay;
    const char unk[] = "XYZ1";
    pay.insert(pay.end(), unk, unk + 4);
    uint32_t w[2] = {12, 0}; pay.insert(pay.end(), (uint8_t*)w, (uint8_t*)w + 8);
    for (int i = 0; i < 12; i++) pay.push_back((uint8_t)(0xa0 + i));
    std::vector<uint8_t> ep;
    for (size_t i = 0; i < e.size(); i += 16) ep.insert(ep.end(), e.begin() + i + 4, e.begin() + i + 16);
    pay.insert(pay.end(), ep.begin(), ep.end());
    pay.insert(pay.end(), 24, 0);                   // a zero tail
    std::vector<uint8_t> mixed = rows_of(pay);
    WorldTrailer u;
    check(u.load(mixed.data(), mixed.size()) && u.rebuild() == mixed, "unknown section + zero tail: parse -> serialise identical",
          u.describe().c_str());
    u.setSignText(0, "edited");
    std::vector<uint8_t> ue = u.rebuild();
    size_t diff = 0;
    for (size_t i = 0; i < std::min(ue.size(), mixed.size()); i++) diff += ue[i] != mixed[i];
    std::snprintf(d, sizeof(d), "%zu B differ, sizes %zu/%zu", diff, ue.size(), mixed.size());
    check(ue.size() == mixed.size() && diff > 0 && diff <= 6, "an edit changes only that record's text bytes", d);
    // 6. sidecars: a bare section, adopted only when the trailer has none of that magic
    std::vector<uint8_t> side;
    const char sg[] = "SGN1"; side.insert(side.end(), sg, sg + 4);
    uint32_t vc[2] = {1, 2}; side.insert(side.end(), (uint8_t*)vc, (uint8_t*)vc + 8);
    TrailerSign s2[2]; std::memset(s2, 0, sizeof(s2));
    s2[0].x = 1; s2[0].a = 3; s2[0].b = 2; std::strcpy(s2[0].text, "one");
    s2[1].x = 2; s2[1].a = 5; s2[1].b = 2; std::strcpy(s2[1].text, "two");
    side.insert(side.end(), (uint8_t*)s2, (uint8_t*)s2 + sizeof(s2));
    WorldTrailer sc;
    check(sc.adoptSidecar(side.data(), side.size()) == WorldTrailer::TR_OK && sc.signCount() == 2 && sc.dirty(),
          "a sidecar is adopted into an empty trailer (dirty)", sc.describe().c_str());
    WorldTrailer sc2; sc2.load(e.data(), e.size());
    check(sc2.adoptSidecar(side.data(), side.size()) == WorldTrailer::TR_EXISTS && sc2.signCount() == 1 && !sc2.dirty(),
          "the world's own SGN1 wins over a sidecar", "");
    // 7. the 1 MiB refusal: (786,432 payload bytes - 24) / 120 = 6,553 signs fit, the next is refused
    WorldTrailer full;
    int n = 0;
    WorldTrailer::Result rr = WorldTrailer::TR_OK;
    for (; n < 10000; n++) {
        rr = full.addSign(TrailerPos::fromTerrainArgs(60000 + n % 100, 60000 + n / 100, 10), 3, 45, 0, "x");
        if (rr != WorldTrailer::TR_OK) break;
    }
    const std::vector<uint8_t> before = full.encode();
    const WorldTrailer::Result again = full.addSign(TrailerPos::fromTerrainArgs(1, 1, 1), 3, 45, 0, "x");
    const WorldTrailer::Result cmdFull = full.addCmd(TrailerPos::fromTerrainArgs(1, 1, 1), "@touch");
    std::snprintf(d, sizeof(d), "%d signs, %zu B, refused with %d/%d/%d", n, before.size(), (int)rr, (int)again, (int)cmdFull);
    check(n == 6553 && rr == WorldTrailer::TR_FULL && again == WorldTrailer::TR_FULL && cmdFull == WorldTrailer::TR_FULL &&
          before.size() <= WorldTrailer::MAX_BYTES && full.encode() == before && full.signCount() == 6553,
          "past 1 MiB a new sign / command block is refused, nothing truncated", d);
}

}  // namespace trailer_test

// --photo-selftest (H.16): round-trips a gradient through Util.mm's PNG encoder and a zlib decode.
int run_photo_selftest() {
    const int w = 301, h = 200;
    std::vector<unsigned char> rgb((size_t)w * h * 3);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        unsigned char* p = &rgb[((size_t)y * w + x) * 3];
        p[0] = (unsigned char)x; p[1] = (unsigned char)y; p[2] = (unsigned char)(x ^ y);
    }
    std::vector<unsigned char> png;
    int bad = 0;
    auto chk = [&](bool ok, const char* what) { std::printf("[eden-photo] %s: %s\n", ok ? "PASS" : "FAIL", what); if (!ok) bad++; };
    chk(photoEncodePNG(rgb.data(), w, h, png), "encode");
    auto be32 = [&](size_t o) { return ((unsigned)png[o] << 24) | (png[o+1] << 16) | (png[o+2] << 8) | png[o+3]; };
    static const unsigned char sig[8] = {0x89,'P','N','G',0x0d,0x0a,0x1a,0x0a};
    chk(png.size() > 33 && !std::memcmp(png.data(), sig, 8), "signature");
    chk(png.size() > 33 && be32(16) == (unsigned)w && be32(20) == (unsigned)h && png[24] == 8 && png[25] == 2, "IHDR 301x200 8-bit RGB");
    // Walk the chunks: check each CRC, gather IDAT.
    std::vector<unsigned char> idat; bool crcOk = true, iend = false;
    for (size_t o = 8; o + 12 <= png.size();) {
        unsigned n = be32(o);
        if (o + 12 + n > png.size()) { crcOk = false; break; }
        uLong c = crc32(crc32(0L, Z_NULL, 0), &png[o + 4], 4 + n);
        if ((unsigned)c != be32(o + 8 + n)) crcOk = false;
        if (!std::memcmp(&png[o + 4], "IDAT", 4)) idat.insert(idat.end(), &png[o + 8], &png[o + 8] + n);
        if (!std::memcmp(&png[o + 4], "IEND", 4)) iend = true;
        o += 12 + n;
    }
    chk(crcOk && iend, "chunk CRCs and IEND");
    std::vector<unsigned char> raw((size_t)(w * 3 + 1) * h);
    uLongf rn = (uLongf)raw.size();
    bool inf = uncompress(raw.data(), &rn, idat.data(), (uLong)idat.size()) == Z_OK && rn == raw.size();
    chk(inf, "IDAT inflates to h*(1+3w) bytes");
    bool same = inf;
    for (int y = 0; same && y < h; y++)
        same = raw[(size_t)y * (w * 3 + 1)] == 0 && !std::memcmp(&raw[(size_t)y * (w * 3 + 1) + 1], &rgb[(size_t)y * w * 3], (size_t)w * 3);
    chk(same, "pixels round-trip, not one flat colour");
    std::printf("[eden-photo] %s\n", bad ? "FAILED" : "ALL PASS");
    return bad ? 1 : 0;
}

int run_trailer_selftest() {
    using namespace trailer_test;
    g_selftestTag = "eden-trailer";
    char d[512];
    synthetic();

    // Gate 1: every real specimen parses and re-serialises byte-identical.
    for (const std::string& f : g_opt.trailerFiles) {
        std::vector<uint8_t> t;
        std::string err;
        const bool ok = read_world(f, t, nullptr, err);
        WorldTrailer w;
        const bool parsed = ok && w.load(t.data(), t.size());
        std::snprintf(d, sizeof(d), "%s: %zu B, %s", f.c_str(), t.size(), ok ? w.describe().c_str() : err.c_str());
        check(parsed && !t.empty() && w.rebuild() == t, "a real trailer: parse -> serialise byte-identical", d);
        std::printf("[eden-trailer] %s\n", d);
    }

    if (!g_opt.world.empty()) {
        // Gates 2 + 3: one sign's text edited in play, saved with zero dirty columns, reloaded.
        WorldTrailer::testNoDirtyMark = g_opt.trailerNoDirty;
        if (!open_world(g_opt.world.c_str(), g_opt.height)) return 1;
        tick(60);
        WorldTrailer* t = (WorldTrailer*)eden_debug_world_trailer();
        const std::string path = eden_debug_world_file();
        std::printf("[eden-trailer] %s: %s (%s)\n", path.c_str(), t ? t->describe().c_str() : "no trailer", eden_debug_world_format());
        if (!t || !t->parsed() || t->signCount() == 0) {
            check(false, "the world has a readable trailer with signs", path.c_str());
            std::printf("[eden-trailer] FAILURES (%d failure(s))\n", g_selftestFailures);
            return 1;
        }
        std::vector<uint8_t> disk0;
        std::string cols0, err;
        check(read_world(path, disk0, &cols0, err), "read the world back from disk", err.c_str());
        if (t->dirty()) {
            // Adopted sidecars: the first save must write them in.
            std::printf("[eden-trailer] sidecars adopted at load: %s\n", t->describe().c_str());
            const std::vector<uint8_t> want = t->encode();
            save_world();
            std::vector<uint8_t> disk;
            read_world(path, disk, nullptr, err);
            check(!t->dirty() && disk == want, "adopted sidecars are written into the world by the first save", "");
            disk0 = disk;
        }
        check(t->encode() == disk0, "the model holds what the file holds", "");
        const std::string old0 = t->sign(0).text;
        WorldTrailer scratch;
        scratch.load(disk0.data(), disk0.size());
        scratch.setSignText(0, "D3A edited sign");
        const std::vector<uint8_t> want = scratch.rebuild();
        size_t diff = 0;
        for (size_t i = 0; i < std::min(want.size(), disk0.size()); i++) diff += want[i] != disk0[i];

        const int dirtyChunks = eden_debug_modified_chunks();
        std::snprintf(d, sizeof(d), "%d modified chunks", dirtyChunks);
        check(dirtyChunks == 0, "no column is dirty before the save (the trailer alone must force it)", d);
        check(t->setSignText(0, "D3A edited sign") == WorldTrailer::TR_OK, "edit sign 0's text in play", old0.c_str());
        const std::string toast = eden_debug_trailer_refusal_toast(WorldTrailer::TR_FULL);
        check(toast == WorldTrailer::resultMessage(WorldTrailer::TR_FULL), "a refusal shows its toast", toast.c_str());
        save_world();
        std::vector<uint8_t> disk1;
        std::string cols1;
        read_world(path, disk1, &cols1, err);
        std::snprintf(d, sizeof(d), "disk %zu B (was %zu), %zu B of the text differ, dirty %d", disk1.size(), disk0.size(), diff,
                      t->dirty() ? 1 : 0);
        check(disk1 == want && want.size() == disk0.size() && diff > 0 && diff <= 96,
              "a zero-dirty-column save writes the edited trailer, only that sign's text changed", d);
        check(cols1 == cols0, "the save left every column byte-identical", cols1.c_str());

        if (!quit_to_menu()) return 1;
        tick(60);
        if (!open_world(g_opt.world.c_str(), g_opt.height)) return 1;
        tick(60);
        t = (WorldTrailer*)eden_debug_world_trailer();
        std::vector<uint8_t> disk2;
        std::string cols2;
        read_world(path, disk2, &cols2, err);
        check(t && t->encode() == want && !t->dirty() && std::strcmp(t->sign(0).text, "D3A edited sign") == 0 &&
              disk2 == want && cols2 == cols0, "after reload: the edit is there, nothing else changed", t ? t->describe().c_str() : "");

        // Remove every record: no trailer at all, on both containers.
        if (g_opt.trailerKeep) t = nullptr;
        while (t && t->signCount()) t->removeSign(t->signCount() - 1);
        while (t && t->cmdCount()) t->removeCmd(t->cmdCount() - 1);
        save_world();
        std::vector<uint8_t> disk3;
        read_world(path, disk3, nullptr, err);
        std::snprintf(d, sizeof(d), "%zu B on disk", disk3.size());
        if (!g_opt.trailerKeep) {
        check(t && t->encode().empty() && disk3.empty(), "removing every record leaves no trailer", d);
        if (!quit_to_menu()) return 1;
        tick(60);
        if (!open_world(g_opt.world.c_str(), g_opt.height)) return 1;
        tick(30);
        t = (WorldTrailer*)eden_debug_world_trailer();
        check(t && t->signCount() == 0 && t->cmdCount() == 0 && t->encode().empty(), "...and it reloads empty",
              t ? t->describe().c_str() : "");
        }
    }
    std::printf("[eden-trailer] %s (%d failure(s))\n", g_selftestFailures ? "FAILURES" : "ALL PASS", g_selftestFailures);
    return g_selftestFailures ? 1 : 0;
}

// ---------------------------------------------------------------------------------------------
// --classic-selftest  (Stage H / H.4, the seeded "Classic" generator)
// ---------------------------------------------------------------------------------------------
// generator-plan.md §7, gates 1, 2, 3 and 6. Pure-function legs first, then the engine's load
// and save path in --docs:
//   1. 1.x exactness: every column of --classic-fixture (the 159 unmodified columns of a Sept 2011
//      world, port/tools/fixtures/classic/) regenerates byte for byte. Trees, leaves, flowers and
//      clouds over predicted air, and the grass kinds, are wildcards (h3-tools/cmp.py's rule).
//      --classic-mutate=tables|trunc must FAIL it (an earlier random() caller; truncation).
//   2. Cross-target hashes: SHA-256 (first 16 hex) over 256 decorated columns per profile, pinned below and
//      identical on every target. A mismatch is a stop; fp contraction is the first suspect.
//   3. Seams: one 64x64 window equals the 16 columns it covers generated one at a time (the new
//      profile's cross-border trees; the 2x/4x lattice). --classic-mutate=noborder must FAIL it.
//   4. The GENERATOR record and its `.eden` mirror: round trip; unknown revisions refused.
//   5. Engine: a 1.x world loads legacy (caves detected off), its unsaved columns are the pure
//      function's, and it writes GENERATOR once; an exported `.eden` carries the mirror; a mirror
//      in the header is honoured; an unknown record loads void and saves nothing.
namespace classic_test {

// Recorded on macOS arm64 (2026-10-15). Every target must print the same: these are the bits of
// the baked tables, the 1.x float order and -ffp-contract=off. Re-pin only for a deliberate
// generator change (a new revision), in the same commit.
const char* const kPins[][2] = {
    {"legacy",       "b822c5c1cece9f0b"},
    {"new1x",        "63ba029b591b0787"},
    {"4x-2011",      "b189472315ffee9d"},
    {"2x-deep-wild", "b43e84546435f737"},
};


bool load_fixture(const std::string& path, Fixture& fx, std::string& err) {
    gzFile f = gzopen(path.c_str(), "rb");
    if (!f) { err = "cannot open " + path; return false; }
    std::vector<uint8_t> b;
    uint8_t buf[65536];
    int n;
    while ((n = gzread(f, buf, sizeof buf)) > 0) b.insert(b.end(), buf, buf + n);
    gzclose(f);
    if (b.size() < 20 || std::memcmp(&b[0], "EMCLFX1", 8) != 0) { err = "not a classic fixture"; return false; }
    int32_t h[3];
    std::memcpy(h, &b[8], 12);
    fx.seed = h[0]; fx.height = h[1];
    const size_t col = 256u * (size_t)fx.height;
    if (fx.height != 64 || b.size() != 20 + (size_t)h[2] * (8 + col)) { err = "fixture size mismatch"; return false; }
    for (int i = 0; i < h[2]; ++i) {
        const uint8_t* p = &b[20 + (size_t)i * (8 + col)];
        int32_t k[2];
        std::memcpy(k, p, 8);
        fx.keys.push_back(std::make_pair(k[0], k[1]));
        fx.types.insert(fx.types.end(), p + 8, p + 8 + col);
    }
    return true;
}

std::string hex16(emod::Sha256& h) {
    uint8_t d[32];
    h.final(d);
    char s[17];
    for (int i = 0; i < 8; ++i) std::snprintf(s + 2 * i, 3, "%02x", d[i]);
    return s;
}

struct Profile { const char* name; ClassicGen g; int height; };
std::vector<Profile> profiles() {
    std::vector<Profile> v;
    Profile p;
    p.name = "legacy"; classic_legacy_params(&p.g, 80866); p.height = 64; v.push_back(p);
    p.name = "new1x"; classic_default_params(&p.g, 1234567); p.g.caves = CLASSIC_CAVES_2011; p.g.trees = 3;
    p.height = 64; v.push_back(p);
    p.name = "4x-2011"; classic_default_params(&p.g, 777); p.g.scale4 = 16; p.g.caves = CLASSIC_CAVES_2011;
    p.height = 256; v.push_back(p);
    p.name = "2x-deep-wild"; classic_default_params(&p.g, 4242); p.g.scale4 = 8; p.g.hilliness = 4;
    p.g.caves = CLASSIC_CAVES_DEEP; p.g.trees = 1; p.height = 256; v.push_back(p);
    return v;
}

// Walks an .emod's framed records (docs/emod-file-format.md: 64-byte file header, then 32-byte
// record headers "EmR1", type @4, payload length @20, payloads padded to 8) and counts one type.
int count_records(const std::string& path, int type) {
    FILE* f = emod::io::fopen_utf8(path.c_str(), "rb");
    if (!f) return -1;
    std::vector<uint8_t> b;
    uint8_t buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) b.insert(b.end(), buf, buf + n);
    std::fclose(f);
    int count = 0;
    for (size_t off = emod::FILE_HEADER; off + emod::REC_HEADER <= b.size();) {
        if (std::memcmp(&b[off], "EmR1", 4) != 0) return -2;
        uint32_t plen;
        std::memcpy(&plen, &b[off + 20], 4);
        count += b[off + 4] == type;
        off += emod::REC_HEADER + ((plen + 7) & ~7u);
    }
    return count;
}

// A world file in --docs: header (seed, home/pos at block x,z), 200 empty creature slots, the given
// columns (64z, types (x*16+z)*64+y, paint 0) and optionally a GENERATOR record.
bool write_world(const std::string& path, const char* name, int seed, int hx, int hz,
                 const Fixture* fx, const uint8_t* genRecord, const uint8_t* mirror, std::string& err) {
    emod::io::remove_file(path);
    emod::EdenWorldStore st;
    if (!st.create(path.c_str(), 4, 0)) { err = st.error(); return false; }
    st.beginBatch();
    WorldFileHeader h;
    std::memset(&h, 0, sizeof h);
    h.level_seed = seed;
    h.pos.x = h.home.x = (float)hx; h.pos.z = h.home.z = (float)hz; h.pos.y = h.home.y = 60;
    std::snprintf(h.name, sizeof h.name, "%s", name);
    h.version = 4;
    h.goldencubes = 10;
    for (int i = 0; i < 16; ++i) h.skycolors[i] = COLOR_NORMAL_BLUE;
    if (mirror) std::memcpy(h.reserved, mirror, 36);
    std::vector<uint8_t> cr(200 * sizeof(EntityData), 0);
    for (size_t o = 0; o < cr.size(); o += sizeof(EntityData)) { int32_t t = -1; std::memcpy(&cr[o + 28], &t, 4); }
    st.putRecord(emod::T_WORLD_HEADER, &h, sizeof h);
    st.putRecord(emod::T_CREATURES, &cr[0], cr.size());
    if (genRecord && !st.putRecord(emod::T_GENERATOR, genRecord, CLASSIC_RECORD_BYTES)) { err = st.error(); return false; }
    if (fx) {
        std::vector<uint8_t> col(4 * emod::BAND, 0);
        for (size_t i = 0; i < fx->keys.size(); ++i) {
            std::fill(col.begin(), col.end(), 0);
            const uint8_t* t = &fx->types[i * 256 * 64];
            for (int xz = 0; xz < 256; ++xz)
                for (int y = 0; y < 64; ++y) col[(size_t)(y / 16) * emod::BAND + xz * 16 + y % 16] = t[xz * 64 + y];
            if (!st.putColumn(fx->keys[i].first, fx->keys[i].second, &col[0])) { err = st.error(); return false; }
        }
    }
    if (!st.commitBatch()) { err = st.error(); return false; }
    st.close();
    return true;
}

bool play_file(const char* file) {
    World::getWorld->menu->loadWorlds();
    int idx = -1;
    for (int i = 0; i < eden_menu_world_count(); ++i)
        if (std::strcmp(eden_menu_world_file(i), file) == 0) idx = i;
    if (idx < 0) { std::fprintf(stderr, "[eden-classic] %s is not listed\n", file); return false; }
    eden_menu_select(idx);
    if (eden_menu_play() != 1) { std::fprintf(stderr, "[eden-classic] menu_play refused %s\n", file); return false; }
    return tick_until([] { return game_mode() == 1; }, 60000, "game_mode == PLAY");
}

// Every cell of resident column (cx,cz) against the pure function; returns mismatches (-1: not resident).
int column_vs_pure(const ClassicGen& g, int cx, int cz, int height) {
    if (!isColumnResident(cx * 16, cz * 16)) return -1;
    std::vector<block8> t((size_t)256 * height);
    std::vector<color8> c((size_t)256 * height);
    classic_generate_window(g, cx * 16, cz * 16, 16, 16, height, &t[0], &c[0], true);
    int bad = 0;
    for (int x = 0; x < 16; ++x)
        for (int z = 0; z < 16; ++z)
            for (int y = 0; y < height; ++y)
                bad += getLandc(cx * 16 + x, cz * 16 + z, y) != t[(size_t)(x * 16 + z) * height + y];
    return bad;
}

}  // namespace classic_test

int run_classic_selftest() {
    using namespace classic_test;
    g_selftestTag = "eden-classic";
    char d[512];
    const std::string& mut = g_opt.classicMutate;
    classic_set_mutation(mut == "tables" ? 1 : mut == "trunc" ? 2 : mut == "noborder" ? 3 : 0);

    // ---- 1. 1.x exactness
    Fixture fx;
    std::string err;
    const bool haveFx = load_fixture(g_opt.classicFixture, fx, err);
    check(haveFx, "the 1.x fixture loads", haveFx ? g_opt.classicFixture.c_str() : err.c_str());
    if (haveFx) {
        std::vector<block8> t(256 * 64);
        std::vector<color8> c(256 * 64);
        ClassicGen g;
        classic_legacy_params(&g, fx.seed);
        int exact = 0;
        long cells = 0, wrong = 0;
        for (size_t i = 0; i < fx.keys.size(); ++i) {
            classic_generate_window(g, fx.keys[i].first * 16, fx.keys[i].second * 16, 16, 16, 64, &t[0], &c[0], false);
            const uint8_t* a = &fx.types[i * 256 * 64];
            long w = 0;
            for (int k = 0; k < 256 * 64; ++k) {
                int av = (int8_t)a[k], ev = t[k];
                if (av == 8 || av == 11 || av == 12) av = 3;
                if (ev == 0 && (av == 0 || av == 5 || av == 6 || av == 19 || av == 73)) continue;
                ++cells;
                w += av != ev;
            }
            wrong += w;
            exact += w == 0;
        }
        std::snprintf(d, sizeof d, "seed %d: %d of %zu columns exact, %ld of %ld cells differ",
                      fx.seed, exact, fx.keys.size(), wrong, cells);
        check(exact == (int)fx.keys.size() && cells > 0, "1.x columns regenerate byte for byte", d);
    }

    // ---- 2. hashes
    std::vector<Profile> ps = profiles();
    for (size_t pi = 0; pi < ps.size(); ++pi) {
        const Profile& p = ps[pi];
        std::vector<block8> t((size_t)256 * p.height);
        std::vector<color8> c((size_t)256 * p.height);
        emod::Sha256 h;
        Uint64 ns = 0;
        for (int cx = 4088; cx < 4104; ++cx)
            for (int cz = 4088; cz < 4104; ++cz) {
                const Uint64 t0 = SDL_GetTicksNS();
                classic_generate_window(p.g, cx * 16, cz * 16, 16, 16, p.height, &t[0], &c[0], true);
                ns += SDL_GetTicksNS() - t0;
                h.update(&t[0], t.size());
                h.update(&c[0], c.size());
            }
        const double ms = ns / 1e6 / 256;
        const std::string got = hex16(h);
        std::printf("[eden-classic] hash %s %s (%.3f ms/column)\n", p.name, got.c_str(), ms);
        std::snprintf(d, sizeof d, "%s: got %s, pinned %s", p.name, got.c_str(), kPins[pi][1]);
        check(got == kPins[pi][1], "profile hash matches its pin", d);
    }

    // ---- 3. seams
    for (size_t pi = 1; pi < ps.size(); ++pi) {
        const Profile& p = ps[pi];
        const int H = p.height, X0 = 4096 * 16, Z0 = 4097 * 16;
        std::vector<block8> wt((size_t)64 * 64 * H), t((size_t)256 * H);
        std::vector<color8> wc((size_t)64 * 64 * H), c((size_t)256 * H);
        classic_generate_window(p.g, X0, Z0, 64, 64, H, &wt[0], &wc[0], true);
        long bad = 0, trees = 0;
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j) {
                classic_generate_window(p.g, X0 + i * 16, Z0 + j * 16, 16, 16, H, &t[0], &c[0], true);
                for (int x = 0; x < 16; ++x)
                    for (int z = 0; z < 16; ++z)
                        for (int y = 0; y < H; ++y) {
                            const size_t a = ((size_t)(i * 16 + x) * 64 + (j * 16 + z)) * H + y, b = ((size_t)x * 16 + z) * H + y;
                            bad += wt[a] != t[b] || wc[a] != c[b];
                            trees += t[b] == TYPE_TREE;
                        }
            }
        std::snprintf(d, sizeof d, "%s: %ld cells differ (%ld trunk cells)", p.name, bad, trees);
        check(bad == 0 && trees > 0, "a 64x64 window equals its 16 columns", d);
    }
    if (!mut.empty()) {
        std::printf("[eden-classic] %s (%d failure(s)) [mutation %s]\n", g_selftestFailures ? "FAILURES" : "ALL PASS",
                    g_selftestFailures, mut.c_str());
        return g_selftestFailures ? 1 : 0;
    }

    // ---- 4. the record
    {
        ClassicGen a = ps[2].g, b;
        a.flags = 1;
        uint8_t r[CLASSIC_RECORD_BYTES], m[36];
        classic_encode_record(a, r);
        bool ok = classic_decode_record(r, sizeof r, &b) && std::memcmp(&a, &b, sizeof a) == 0;
        classic_encode_mirror(a, m);
        ClassicGen mb;
        ok = ok && classic_decode_mirror(m, &mb) && std::memcmp(&a, &mb, sizeof a) == 0;
        check(ok, "GENERATOR record and mirror round-trip", "");
        uint8_t r2[CLASSIC_RECORD_BYTES];
        std::memcpy(r2, r, sizeof r); r2[2] = 2;
        bool refused = !classic_decode_record(r2, sizeof r2, &b);
        std::memcpy(r2, r, sizeof r); r2[27] = 1;
        refused = refused && !classic_decode_record(r2, sizeof r2, &b);
        std::memcpy(r2, r, sizeof r); r2[8] = 5;
        refused = refused && !classic_decode_record(r2, sizeof r2, &b) && !classic_decode_record(r, 27, &b);
        m[20] ^= 1;
        refused = refused && !classic_decode_mirror(m, &b);
        check(refused, "unknown revisions, bad fields and a bad mirror CRC are refused", "");
    }

    // ---- 5. engine: load, generate, save
    if (haveFx) {
        const std::string docs = eden_platform_documents_root();
        TerrainGenerator* tg = World::getWorld->terrain->tgen;
        const int hx = fx.keys[0].first * 16 + 8, hz = fx.keys[0].second * 16 + 8;
        // (a) a 1.x world
        const std::string lp = docs + "/classic-legacy.emod";
        bool ok = write_world(lp, "classic legacy", fx.seed, hx, hz, &fx, NULL, NULL, err) && play_file("classic-legacy.emod");
        check(ok, "a 1.x world (fixture columns, no record) plays", err.c_str());
        if (ok) {
            tick(30);
            const ClassicGen& g = tg->classic;
            std::snprintf(d, sizeof d, "state %d profile %d caves %d flags %d, votes %d/%d", tg->classicState, g.profile,
                          g.caves, g.flags, g_classic_cave_votes_air, g_classic_cave_votes_cells);
            check(tg->classicState == TerrainGenerator::CLASSIC_STATE_SET && g.profile == CLASSIC_PROFILE_LEGACY &&
                  g.caves == CLASSIC_CAVES_OFF && g.flags == 1 && g_classic_cave_votes_cells >= 2000,
                  "it loads the legacy profile, caves detected off", d);
            std::set<std::pair<int, int> > saved(fx.keys.begin(), fx.keys.end());
            int tested = 0, bad = 0;
            for (int dx = -4; dx <= 4 && tested < 6; ++dx)
                for (int dz = -4; dz <= 4 && tested < 6; ++dz) {
                    const int cx = fx.keys[0].first + dx, cz = fx.keys[0].second + dz;
                    if (saved.count(std::make_pair(cx, cz))) continue;
                    const int b = column_vs_pure(g, cx, cz, g_world_height);
                    if (b < 0) continue;
                    ++tested; bad += b;
                }
            std::snprintf(d, sizeof d, "%d unsaved columns, %d cells differ", tested, bad);
            check(tested == 6 && bad == 0, "its unsaved columns are the pure function's", d);
            save_world();
            save_world();
            const int n9 = count_records(lp, emod::T_GENERATOR);
            std::snprintf(d, sizeof d, "%d GENERATOR record(s) after two saves", n9);
            check(n9 == 1, "a legacy world writes GENERATOR exactly once", d);
            quit_to_menu();
            // the exported `.eden` carries the mirror
            const std::string ep = docs + "/classic-legacy-export.eden";
            emod::ExportOptions eo;
            std::string xerr;
            bool exported = emod::export_world_file(lp, ep, eo, false, NULL, &xerr);
            ClassicGen mg;
            bool mirrored = false;
            if (exported) {
                FILE* f = emod::io::fopen_utf8(ep.c_str(), "rb");
                WorldFileHeader eh;
                mirrored = f && std::fread(&eh, 1, sizeof eh, f) == sizeof eh &&
                           classic_decode_mirror((const unsigned char*)eh.reserved, &mg) &&
                           mg.seed == fx.seed && mg.profile == CLASSIC_PROFILE_LEGACY && mg.flags == 1;
                if (f) std::fclose(f);
                emod::io::remove_file(ep);
            }
            check(exported && mirrored, "the exported .eden carries the GENERATOR mirror", xerr.c_str());
        }
        // (b) a mirror in the header (an imported Classic `.eden`) is honoured and recorded
        ClassicGen mg;
        classic_default_params(&mg, 31337);
        mg.trees = 3; mg.hilliness = 3; mg.caves = CLASSIC_CAVES_DEEP;
        uint8_t mirror[36];
        classic_encode_mirror(mg, mirror);
        const std::string mp = docs + "/classic-mirror.emod";
        ok = write_world(mp, "classic mirror", 31337, 4096 * 16 + 8, 4096 * 16 + 8, NULL, NULL, mirror, err) &&
             play_file("classic-mirror.emod");
        check(ok, "a world whose header holds a mirror plays", err.c_str());
        if (ok) {
            tick(30);
            const bool same = tg->classicState == TerrainGenerator::CLASSIC_STATE_SET &&
                              std::memcmp(&tg->classic, &mg, sizeof mg) == 0;
            const int b = column_vs_pure(mg, 4096, 4096, g_world_height);
            std::snprintf(d, sizeof d, "state %d, trees %d caves %d, spawn column %d cells differ", tg->classicState,
                          tg->classic.trees, tg->classic.caves, b);
            check(same && b == 0, "the mirror's settings generate the world", d);
            save_world();
            const int n9 = count_records(mp, emod::T_GENERATOR);
            emod::EdenWorldStore st;
            std::vector<uint8_t> rec;
            ClassicGen back;
            const bool rd = st.open(mp.c_str(), false) && st.readRecord(emod::T_GENERATOR, rec) == emod::EdenWorldStore::READ_OK &&
                            classic_decode_record(&rec[0], rec.size(), &back) && std::memcmp(&back, &mg, sizeof mg) == 0;
            st.close();
            std::snprintf(d, sizeof d, "%d record(s), decodes to the mirror: %d", n9, rd ? 1 : 0);
            check(n9 == 1 && rd, "the next save records it", d);
            quit_to_menu();
        }
        // (c) a GENERATOR from a newer Emod: void, nothing saved
        uint8_t future[CLASSIC_RECORD_BYTES];
        classic_encode_record(mg, future);
        future[2] = 2;   // revision 2
        const std::string vp = docs + "/classic-void.emod";
        ok = write_world(vp, "classic void", 31337, 4096 * 16 + 8, 4096 * 16 + 8, NULL, future, NULL, err) &&
             play_file("classic-void.emod");
        check(ok, "a world with an unknown GENERATOR plays", err.c_str());
        if (ok) {
            tick(10);
            int solid = 0;
            for (int x = 0; x < 16; ++x)
                for (int z = 0; z < 16; ++z)
                    for (int y = 0; y < g_world_height; ++y) solid += getLandc(4096 * 16 + x, 4096 * 16 + z, y) != 0;
            std::snprintf(d, sizeof d, "state %d, %d solid cells at spawn", tg->classicState, solid);
            check(tg->classicState == TerrainGenerator::CLASSIC_STATE_VOID && solid == 0, "it loads void", d);
            save_world();
            emod::EdenWorldStore st;
            const bool opened = st.open(vp.c_str(), false);
            std::snprintf(d, sizeof d, "%zu columns, %d GENERATOR record(s)", opened ? st.columnCount() : (size_t)0,
                          count_records(vp, emod::T_GENERATOR));
            check(opened && st.columnCount() == 0 && count_records(vp, emod::T_GENERATOR) == 1,
                  "saving it stores no columns and keeps the record", d);
            st.close();
            quit_to_menu();
        }
    }

    std::printf("[eden-classic] %s (%d failure(s))\n", g_selftestFailures ? "FAILURES" : "ALL PASS", g_selftestFailures);
    return g_selftestFailures ? 1 : 0;
}

}  // namespace g_world
}  // namespace harness
