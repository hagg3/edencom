//
//  EdenWorldStoreSelftest.cpp
//  Eden — Stage S / S.3's gates for EdenWorldStore + EdenEmodConverter, compiled into both builds:
//    native: `eden_native --emod-selftest [--emod-fixtures=DIR] [--emod-file=W.eden[.gz] --emod-ref=R.emod]`
//    web:    web-port-final:web/tools/headless-emod-store-test.js (calls eden_emod_selftest_run under node)
//  EDEN_DIAGNOSTICS-only, like the other self-tests, so a shipped build carries none of it.
//
//  What each section is for (WORKING/ROADMAP.md S.3; the plan's §7 S.3 row numbers the gates):
//    fixtures      gate 1 + 6 + 4: the checked-in pack (port/tools/fixtures/emod/, written by
//                  `emod.py fixtures-pack`) holds emod.py's fixtures as .eden.gz with the SHA-256 of
//                  emod.py's own conversion and its state digest. Every target must stream-convert
//                  each to the SAME bytes and read emod.py's multi-batch / zlib / raw `.emod` files
//                  to the same state. Same hash on every target == the cross-target byte gate.
//    synth         the converter against independently generated worlds (canonical, off-grid like
//                  Quarry, short span, dead/duplicate/bad rows, no columns): every feed chunking gives
//                  the same bytes, and the store reads back the engine's view of every column.
//    errors        refused inputs leave no temp files (legacy header, truncated stream, the cap).
//    writer        create / append / ords / SUMMARY / the first-batch rule / prev_commit chain;
//                  compaction (shrinks, same state, compact(convert(x)) == convert(x) byte for byte).
//    fallback      a committed record that frames but does not decode: the previous version, else
//                  air; damaged() set; compaction refused.
//    crash         gate 2: the real commitBatch() killed after every byte k of a batch (prefix, and
//                  zero-filled to length), plus each record of the batch lost while its COMMIT
//                  survived. Every reopen must be the old or the new commit, never a mix (K4).
//    palette       K.1-2: FileHeader flags bit 0 + BLOCK_PALETTE: putRecord's rules (flag first, D8
//                  superset, names), the flag compaction (D13 latent bytes, D14), crash injection at
//                  every byte of the flag compaction and of the first palette batch, and the refusal
//                  a build without acceptPaletteFlag (= a pre-K.1 build) gives, file untouched.
//    interchange   K.1-4 (needs the pack): a world with a block palette through every export target, the
//                  report's counts, BPL1 written/dropped, keepPalette (Convert to 64z), the upload body
//                  (no byte >= 128), and each output converted back (flag + palette, or none).
//    corrupt       gate 3: emod.py's corrupt gate, on this reader (its base carries the flag and a
//                  palette since K.1-2); then once with the CRC check off, which must FAIL.
//    opentime      gate 5: open() on a 30,000-column log + 20 append batches (decides INDEX).
//    file          optional: a real world (raw or gzip) against emod.py's output for it, with time,
//                  peak held bytes and (POSIX) peak RSS — Quarry's .gz is the gate-6 headline.
//
#include "EdenWorldStore.h"
#include "EdenWorldExport.h"


#include <zlib.h>
#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#if defined(_WIN32)
#  include <direct.h>
#else
#  include <sys/stat.h>
#  include <sys/resource.h>
#endif
#define EMOD_EXPORT

using namespace emod;

namespace {

int g_checks = 0, g_fails = 0;
bool g_quick = false;
std::string g_work;

void say(const char* f, ...) {
    va_list ap;
    va_start(ap, f);
    std::printf("[eden-emod] ");
    std::vprintf(f, ap);
    std::printf("\n");
    std::fflush(stdout);
    va_end(ap);
}
bool check(bool ok, const char* what, const std::string& detail = std::string()) {
    g_checks++;
    if (!ok) g_fails++;
    say("%s %s%s%s", ok ? "PASS" : "FAIL", what, detail.empty() ? "" : " -- ", detail.c_str());
    return ok;
}
std::string sfmt(const char* f, ...) {
    char b[1024];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(b, sizeof b, f, ap);
    va_end(ap);
    return b;
}
double now_ms() {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ---- files
std::string P(const std::string& name) { return g_work + "/" + name; }
void mkdir_p(const std::string& d) {
#if defined(_WIN32)
    _mkdir(d.c_str());
#else
    mkdir(d.c_str(), 0755);
#endif
}
bool exists(const std::string& p) { FILE* f = std::fopen(p.c_str(), "rb"); if (f) std::fclose(f); return f != nullptr; }
bool read_file(const std::string& p, std::vector<uint8_t>& out) {
    out.clear();
    FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) return false;
    uint8_t b[1 << 16];
    size_t n;
    while ((n = std::fread(b, 1, sizeof b, f)) > 0) out.insert(out.end(), b, b + n);
    std::fclose(f);
    return true;
}
bool write_file(const std::string& p, const uint8_t* d, size_t n) {
    FILE* f = std::fopen(p.c_str(), "wb");
    if (!f) return false;
    bool ok = std::fwrite(d, 1, n, f) == n;
    return std::fclose(f) == 0 && ok;
}
bool write_file(const std::string& p, const std::vector<uint8_t>& v) { return write_file(p, v.data(), v.size()); }
std::string hexs(const uint8_t* d, size_t n) {
    static const char* k = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; i++) { s += k[d[i] >> 4]; s += k[d[i] & 15]; }
    return s;
}
std::string sha_file(const std::string& p) {
    FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) return "missing";
    Sha256 s;
    uint8_t b[1 << 16];
    size_t n;
    while ((n = std::fread(b, 1, sizeof b, f)) > 0) s.update(b, n);
    std::fclose(f);
    uint8_t o[32];
    s.final(o);
    return hexs(o, 32);
}
std::string base_name(const std::string& p) {
    size_t s = p.find_last_of("/\\");
    return s == std::string::npos ? p : p.substr(s + 1);
}

inline uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
inline void wr32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
inline void wr64(uint8_t* p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }

// Feed a file (raw, or gzip — multi-member aware) to a converter in `chunk`-byte pieces.
bool feed_file(EdenEmodConverter& c, const std::string& path, size_t chunk) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::vector<uint8_t> in(std::max<size_t>(chunk, 1 << 16)), out(std::max<size_t>(chunk, 1));
    size_t n = std::fread(in.data(), 1, in.size(), f);
    bool gz = n >= 2 && in[0] == 0x1f && in[1] == 0x8b;
    bool ok = true;
    if (!gz) {
        while (ok && n) {
            for (size_t o = 0; ok && o < n; o += chunk) ok = c.feed(in.data() + o, std::min(chunk, n - o));
            n = std::fread(in.data(), 1, in.size(), f);
        }
        std::fclose(f);
        return ok;
    }
    z_stream z;
    std::memset(&z, 0, sizeof z);
    inflateInit2(&z, 16 + MAX_WBITS);
    z.next_in = in.data();
    z.avail_in = (uInt)n;
    while (ok) {
        if (z.avail_in == 0) {
            n = std::fread(in.data(), 1, in.size(), f);
            if (!n) break;
            z.next_in = in.data();
            z.avail_in = (uInt)n;
        }
        z.next_out = out.data();
        z.avail_out = (uInt)out.size();
        int r = inflate(&z, Z_NO_FLUSH);
        size_t got = out.size() - z.avail_out;
        if (got) ok = c.feed(out.data(), got);
        if (r == Z_STREAM_END) {
            // another member may follow (a concatenated gzip)
            inflateReset(&z);
        } else if (r != Z_OK && r != Z_BUF_ERROR) {
            ok = false;
        }
    }
    inflateEnd(&z);
    std::fclose(f);
    return ok;
}

bool convert_file(const std::string& src, const std::string& out, size_t chunk, const std::string& name,
                  EdenEmodConverter* keep = nullptr, int codec = C_ZSTD) {
    EdenEmodConverter local;
    EdenEmodConverter& c = keep ? *keep : local;
    EdenEmodConverter::Options o;
    o.codec = codec;
    o.createdUnix = 0;
    o.sourceName = name;
    if (!c.begin(out.c_str(), o)) return false;
    if (!feed_file(c, src, chunk)) { if (c.err() == EdenEmodConverter::OK) c.abort(); return false; }
    return c.finish();
}

// ---- synthetic worlds (independent of the converter: the expected engine view is built here)
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 1) {}
    uint32_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return (uint32_t)(s >> 16); }
};

// Terrain-ish: a height field, three strata, rare ore, sparse paint; `top` caps the height.
std::vector<uint8_t> make_column(Rng& r, int bands, int32_t cx, int32_t cz, int top, int paintOnlyBand = -1) {
    std::vector<uint8_t> c((size_t)bands * BAND, 0);
    for (int lx = 0; lx < 16; lx++)
        for (int lz = 0; lz < 16; lz++) {
            int h = std::min(top, 18 + (int)(((cx * 16 + lx) * 7 + (cz * 16 + lz) * 3) % 9) + (int)(r.next() % 3));
            for (int y = 0; y < h; y++) {
                uint8_t t = y < h - 3 ? 1 : (y < h - 1 ? 2 : 3);
                if (r.next() % 211 == 0) t = 14;
                int b = y / 16, ly = y % 16;
                size_t i = (size_t)b * BAND + lx * 256 + lz * 16 + ly;
                c[i] = t;
                if (y == h - 1 && r.next() % 40 == 0) c[i + 4096] = (uint8_t)(1 + r.next() % 53);
            }
        }
    if (paintOnlyBand >= 0) {                       // all-air types, non-zero paint: must be stored
        size_t b = (size_t)paintOnlyBand * BAND;
        std::memset(&c[b], 0, 4096);
        c[b + 4096 + 100] = 7;
    }
    return c;
}

std::vector<uint8_t> eden_header(int version, const char* name, uint64_t dirOff) {
    std::vector<uint8_t> h(EDEN_HEADER, 0);
    wr32(&h[0], 4242);
    float f[7] = {100.5f, 40.0f, 200.25f, 100.0f, 39.0f, 200.0f, 33.0f};
    std::memcpy(&h[4], f, sizeof f);
    wr64(&h[32], dirOff);
    std::strncpy((char*)&h[40], name, 49);
    wr32(&h[92], (uint32_t)version);
    for (int i = 0; i < 40; i++) h[152 + i] = (uint8_t)(i * 37);   // 2.1.1-era reserved[] garbage
    return h;
}

struct SynthWorld {
    std::string name;
    std::vector<uint8_t> bytes;
    std::map<std::pair<int32_t, int32_t>, std::vector<uint8_t> > view;   // expected engine view
    int bands;
};

// rows: (x, z, offset) in directory order; `blocks` are written at their offsets first.
SynthWorld build_eden(const std::string& name, int version, const std::vector<std::pair<uint64_t, std::vector<uint8_t> > >& blocks,
                      uint64_t blockEnd, uint32_t slots, const std::vector<std::pair<std::pair<int32_t, int32_t>, uint64_t> >& rows,
                      const std::vector<uint8_t>& extraRows = std::vector<uint8_t>(), const std::vector<uint8_t>& trailer = std::vector<uint8_t>(),
                      int bands = 0) {
    SynthWorld w;
    w.name = name;
    w.bands = bands ? bands : version >= 5 ? 16 : 4;    // `bands` given: the version does not say (S.3b)
    const uint64_t col = (uint64_t)w.bands * BAND;
    uint64_t dirOff = blockEnd + (uint64_t)EDEN_ENT * slots;
    w.bytes.assign((size_t)dirOff, 0);
    std::vector<uint8_t> h = eden_header(version, name.c_str(), dirOff);
    std::memcpy(w.bytes.data(), h.data(), EDEN_HEADER);
    for (const auto& b : blocks) std::memcpy(&w.bytes[(size_t)b.first], b.second.data(), std::min<size_t>(b.second.size(), (size_t)(dirOff - b.first)));
    for (uint32_t s = 0; s < slots; s++) {                // creature slots: type -1, one real one
        uint8_t* e = &w.bytes[(size_t)(blockEnd + (uint64_t)s * EDEN_ENT)];
        float pos[3] = {(float)s, 40.0f, 7.5f};
        std::memcpy(e, pos, 12);
        wr32(e + 28, s == 0 ? 3u : 0xffffffffu);
    }
    for (const auto& r : rows) {
        uint8_t b[16];
        wr32(b, (uint32_t)r.first.first);
        wr32(b + 4, (uint32_t)r.first.second);
        wr64(b + 8, r.second);
        w.bytes.insert(w.bytes.end(), b, b + 16);
    }
    w.bytes.insert(w.bytes.end(), extraRows.begin(), extraRows.end());
    w.bytes.insert(w.bytes.end(), trailer.begin(), trailer.end());
    // expected view: last row wins; the creature slots are what the gap after the highest record
    // holds (else the version default), and a span runs to the next live offset or the creature
    // block, capped at the stride -- the engine's rules, written out independently
    std::map<std::pair<int32_t, int32_t>, uint64_t> live;
    for (const auto& r : rows) live[r.first] = r.second;
    std::vector<uint64_t> offs;
    for (const auto& kv : live) offs.push_back(kv.second);
    std::sort(offs.begin(), offs.end());
    uint64_t realEnd = blockEnd;
    if (!offs.empty()) {
        uint64_t lastEnd = offs.back() + col;
        uint32_t derived = w.bands == 16 ? 400 : 200;
        if (dirOff >= lastEnd && (dirOff - lastEnd) % EDEN_ENT == 0 && (dirOff - lastEnd) / EDEN_ENT <= EDEN_MAX_SLOTS)
            derived = (uint32_t)((dirOff - lastEnd) / EDEN_ENT);
        realEnd = dirOff - (uint64_t)derived * EDEN_ENT;
    }
    for (const auto& kv : live) {
        uint64_t o = kv.second;
        auto it = std::upper_bound(offs.begin(), offs.end(), o);
        uint64_t nxt = it == offs.end() ? realEnd : *it;
        uint64_t span = (nxt > o && nxt - o < col) ? nxt - o : col;
        std::vector<uint8_t> v((size_t)col, 0);
        for (uint64_t i = 0; i < span && o + i < w.bytes.size(); i++) v[(size_t)i] = w.bytes[(size_t)(o + i)];
        w.view[kv.first] = v;
    }
    return w;
}

std::vector<SynthWorld> synth_worlds() {
    std::vector<SynthWorld> out;
    Rng r(20261009);
    {   // 64z canonical: shuffled (hash-order) directory, an all-air column, a paint-only band
        const uint64_t col = 4 * BAND;
        std::vector<std::pair<uint64_t, std::vector<uint8_t> > > b;
        std::vector<std::pair<std::pair<int32_t, int32_t>, uint64_t> > rows;
        for (int i = 0; i < 12; i++) {
            std::vector<uint8_t> c = i == 4 ? std::vector<uint8_t>(col, 0) : make_column(r, 4, 4000 + i % 4, 4000 + i / 4, 40 + i, i == 2 ? 3 : -1);
            b.push_back(std::make_pair(EDEN_HEADER + i * col, c));
        }
        const int order[12] = {5, 1, 7, 0, 11, 8, 3, 6, 10, 2, 9, 4};
        for (int k : order) rows.push_back(std::make_pair(std::make_pair(4000 + k % 4, 4000 + k / 4), EDEN_HEADER + k * col));
        out.push_back(build_eden("s64-canonical", 4, b, EDEN_HEADER + 12 * col, 200, rows));
    }
    {   // 256z with a sign trailer (version 5)
        const uint64_t col = 16 * BAND;
        std::vector<std::pair<uint64_t, std::vector<uint8_t> > > b;
        std::vector<std::pair<std::pair<int32_t, int32_t>, uint64_t> > rows;
        for (int i = 0; i < 8; i++) {
            b.push_back(std::make_pair(EDEN_HEADER + i * col, make_column(r, 16, 4100 + i % 4, 3950 + i / 4, 30 + 25 * (i % 4), i == 1 ? 9 : -1)));
            rows.push_back(std::make_pair(std::make_pair(4100 + i % 4, 3950 + i / 4), EDEN_HEADER + i * col));
        }
        std::vector<uint8_t> tr;
        for (int i = 0; i < 12; i++) { uint8_t row[16]; std::memset(row, 0xA5 ^ i, 16); wr32(row, 0xffffffffu); tr.insert(tr.end(), row, row + 16); }
        out.push_back(build_eden("s256-trailer", 5, b, EDEN_HEADER + 8 * col, 400, rows, std::vector<uint8_t>(), tr));
    }
    {   // 256z off-grid, like Quarry: every record starts 24,000 B past its slot boundary
        const uint64_t col = 16 * BAND, base = EDEN_HEADER + 24000;
        std::vector<std::pair<uint64_t, std::vector<uint8_t> > > b;
        std::vector<std::pair<std::pair<int32_t, int32_t>, uint64_t> > rows;
        for (int i = 0; i < 6; i++) {
            b.push_back(std::make_pair(base + i * col, make_column(r, 16, 50 + i, 60, 70)));
            rows.push_back(std::make_pair(std::make_pair(50 + i, 60), base + i * col));
        }
        out.push_back(build_eden("s256-offgrid", 6, b, base + 6 * col, 400, rows));
    }
    {   // 256z short span: record 2 starts 24,000 B early, overwriting record 1's tail
        const uint64_t col = 16 * BAND;
        const uint64_t lay[4] = {EDEN_HEADER, EDEN_HEADER + col, EDEN_HEADER + 2 * col - 24000, EDEN_HEADER + 3 * col - 24000};
        std::vector<std::pair<uint64_t, std::vector<uint8_t> > > b;
        std::vector<std::pair<std::pair<int32_t, int32_t>, uint64_t> > rows;
        for (int i = 0; i < 4; i++) {
            b.push_back(std::make_pair(lay[i], make_column(r, 16, 90 + i, 60, 100)));
            rows.push_back(std::make_pair(std::make_pair(90 + i, 60), lay[i]));
        }
        out.push_back(build_eden("s256-short", 5, b, lay[3] + col, 400, rows));
    }
    {   // 64z: a dead slot, a duplicate key (the last row wins), an interior gate-failing row
        const uint64_t col = 4 * BAND;
        std::vector<std::pair<uint64_t, std::vector<uint8_t> > > b;
        std::vector<std::pair<std::pair<int32_t, int32_t>, uint64_t> > rows;
        for (int i = 0; i < 5; i++) b.push_back(std::make_pair(EDEN_HEADER + i * col, make_column(r, 4, 70 + i, 80, 30)));
        rows.push_back(std::make_pair(std::make_pair(70, 80), EDEN_HEADER + 0 * col));
        rows.push_back(std::make_pair(std::make_pair(72, 80), EDEN_HEADER + 2 * col));   // slot 1 is dead
        rows.push_back(std::make_pair(std::make_pair(73, 80), EDEN_HEADER + 3 * col));
        rows.push_back(std::make_pair(std::make_pair(70, 80), EDEN_HEADER + 4 * col));   // duplicate key
        std::vector<uint8_t> bad(16, 0);
        wr32(&bad[0], 0xffffffffu);
        wr32(&bad[4], 5);
        SynthWorld w = build_eden("s64-dead-dup", 4, b, EDEN_HEADER + 5 * col, 200, rows, bad);
        // move the bad row inside the directory: swap it with the last real row
        size_t n = w.bytes.size();
        std::vector<uint8_t> last(w.bytes.begin() + (n - 32), w.bytes.begin() + (n - 16));
        std::memcpy(&w.bytes[n - 32], &w.bytes[n - 16], 16);
        std::memcpy(&w.bytes[n - 16], last.data(), 16);
        out.push_back(w);
    }
    {   // S.3b: 256z stamped version 2 (the 2026 game; alpinecraft's shape): 400 slots, content to
        // band 6, a sign trailer. Only the creature gap says 256z -- `version >= 5` would read 64z.
        const uint64_t col = 16 * BAND;
        std::vector<std::pair<uint64_t, std::vector<uint8_t> > > b;
        std::vector<std::pair<std::pair<int32_t, int32_t>, uint64_t> > rows;
        for (int i = 0; i < 5; i++) {
            std::vector<uint8_t> c = make_column(r, 16, 600 + i % 3, 700 + i / 3, 60);
            c[(size_t)(2 + i) * BAND + 17 * 16 + 5] = 4;                  // a block in band 2..6
            c[(size_t)(2 + i) * BAND + 4096 + 17 * 16 + 5] = 9;
            b.push_back(std::make_pair(EDEN_HEADER + i * col, c));
            rows.push_back(std::make_pair(std::make_pair(600 + i % 3, 700 + i / 3), EDEN_HEADER + i * col));
        }
        std::vector<uint8_t> tr;
        for (int i = 0; i < 6; i++) { uint8_t row[16]; std::memset(row, 0x3C ^ i, 16); wr32(row, 0xffffffffu); tr.insert(tr.end(), row, row + 16); }
        out.push_back(build_eden("s256-v2", 2, b, EDEN_HEADER + 5 * col, 400, rows, std::vector<uint8_t>(), tr, 16));
    }
    {   // S.3b: ONE 256z column under version 2, no creature block -- no offset gap to measure, so
        // only the creature-gap test (0 at 131,072) can tell
        std::vector<std::pair<uint64_t, std::vector<uint8_t> > > b;
        std::vector<std::pair<std::pair<int32_t, int32_t>, uint64_t> > rows;
        std::vector<uint8_t> c = make_column(r, 16, 610, 710, 60);
        c[(size_t)9 * BAND + 3] = 4;                                        // band 9
        b.push_back(std::make_pair((uint64_t)EDEN_HEADER, c));
        rows.push_back(std::make_pair(std::make_pair(610, 710), (uint64_t)EDEN_HEADER));
        out.push_back(build_eden("s256-v2-single", 2, b, EDEN_HEADER + 16 * BAND, 0, rows, std::vector<uint8_t>(), std::vector<uint8_t>(), 16));
    }
    {   // a world with no columns at all (version 2: no creature block either)
        std::vector<std::pair<uint64_t, std::vector<uint8_t> > > b;
        std::vector<std::pair<std::pair<int32_t, int32_t>, uint64_t> > rows;
        out.push_back(build_eden("s64-empty", 2, b, EDEN_HEADER + 200 * EDEN_ENT, 0, rows));
    }
    return out;
}

// ---- record walking for the gates (the file is known-clean when these run)
struct RecPos { uint64_t off, poff; uint32_t plen; uint8_t type; uint32_t seq; };
std::vector<RecPos> walk(const std::vector<uint8_t>& f) {
    std::vector<RecPos> v;
    uint64_t p = FILE_HEADER;
    while (p + REC_HEADER <= f.size() && !std::memcmp(&f[(size_t)p], "EmR1", 4)) {
        RecPos r;
        r.off = p; r.poff = p + REC_HEADER; r.type = f[(size_t)p + 4];
        r.plen = rd32(&f[(size_t)p + 20]); r.seq = rd32(&f[(size_t)p + 24]);
        v.push_back(r);
        uint64_t n = REC_HEADER + (uint64_t)r.plen;
        p += n + ((8 - (n & 7)) & 7);
    }
    return v;
}
// Recompute a record's CRC after its payload was edited, then its batch's COMMIT (crc_of_crcs + CRC).
void reseal(std::vector<uint8_t>& f, const RecPos& r) {
    uint8_t* h = &f[(size_t)r.off];
    uint32_t c = crc32_of(h, 28);
    c = crc32_of(h + REC_HEADER, r.plen, c);
    wr32(h + 28, c);
    std::vector<RecPos> rs = walk(f);
    uint32_t cc = 0;
    for (const RecPos& q : rs) {
        if (q.seq != r.seq) continue;
        if (q.type == T_COMMIT) {
            uint8_t* ch = &f[(size_t)q.off];
            wr32(ch + REC_HEADER + 8, cc);
            uint32_t k = crc32_of(ch, 28);
            k = crc32_of(ch + REC_HEADER, q.plen, k);
            wr32(ch + 28, k);
            return;
        }
        uint8_t b[4];
        std::memcpy(b, &f[(size_t)q.off + 28], 4);
        cc = crc32_of(b, 4, cc);
    }
}

std::string digest_of(const std::string& path, bool* opened = nullptr, bool* damaged = nullptr) {
    EdenWorldStore s;
    bool ok = s.open(path.c_str(), false);
    if (opened) *opened = ok;
    if (!ok) return "refused";
    std::string d = s.stateDigest();
    if (damaged) *damaged = s.damaged();
    return d;
}

// The second batch the crash and corrupt gates use: rewrite two columns, add two, new creatures
// and header (emod.py corrupt_gate's shape, one column more).
bool second_batch(EdenWorldStore& s, uint32_t salt, const std::vector<uint8_t>* palette = nullptr) {
    if (!s.isOpen() || s.columnCount() < 2) return false;
    std::vector<std::pair<int32_t, int32_t> > keys = s.columnKeys();
    std::vector<uint8_t> c((size_t)s.bands() * BAND);
    s.beginBatch();
    for (int i = 0; i < 2 && i < (int)keys.size(); i++) {
        s.readColumn(keys[i].first, keys[i].second, c.data());
        c[123 + i] ^= 0x5a;
        c[(size_t)BAND * 2 + 7] = (uint8_t)(salt + i);         // a new band in an old column
        if (!s.putColumn(keys[i].first, keys[i].second, c.data())) return false;
    }
    Rng r(salt);
    for (int i = 0; i < 2; i++) {
        std::vector<uint8_t> n = make_column(r, s.bands(), 5000 + i, 5000, 40);
        if (!s.putColumn(5000 + i, 5000, n.data())) return false;
    }
    std::vector<uint8_t> cr;
    s.readRecord(T_CREATURES, cr);
    if (cr.size() >= EDEN_ENT) cr[3] ^= 0x11;
    if (!s.putRecord(T_CREATURES, cr.data(), cr.size())) return false;
    std::vector<uint8_t> h;
    if (s.readRecord(T_WORLD_HEADER, h) != EdenWorldStore::READ_OK || h.size() != EDEN_HEADER) return false;
    h[4] ^= 1;
    if (!s.putRecord(T_WORLD_HEADER, h.data(), h.size())) return false;
    if (palette && !s.putRecord(T_BLOCK_PALETTE, palette->data(), palette->size())) return false;
    return s.commitBatch();
}

// K.1-2: emod.py's PALETTE_FIXTURE_V1/V2 (the corrupt gate's and the fixtures' palettes).
PaletteEntry pent(int id, int ft, int fp, const char* name) {
    PaletteEntry e;
    e.id = (uint8_t)id; e.fallbackType = (uint8_t)ft; e.fallbackPaint = (uint8_t)fp; e.flags = 0; e.name = name;
    return e;
}
std::vector<uint8_t> palette_bytes(const std::vector<PaletteEntry>& es) {
    std::vector<uint8_t> b;
    palette_build(es, b);
    return b;
}
std::vector<uint8_t> palette_v1() { return palette_bytes({pent(128, 1, 7, "emod:test_glow"), pent(129, 2, 0, "stairs:oak")}); }
std::vector<uint8_t> palette_v2() {
    return palette_bytes({pent(128, 1, 7, "emod:test_glow"), pent(129, 2, 0, "stairs:oak"), pent(130, 4, 12, "stairs:oak/r")});
}

// =============================================================================================
// sections
// =============================================================================================
void section_fixtures(const std::string& dir) {
    say("== fixtures (%s)", dir.c_str());
    std::vector<uint8_t> man;
    if (!check(read_file(dir + "/expected.txt", man), "fixture pack: expected.txt readable")) return;
    std::string text(man.begin(), man.end());
    size_t pos = 0;
    int n = 0;
    while (pos < text.size()) {
        size_t e = text.find('\n', pos);
        std::string line = text.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
        pos = e == std::string::npos ? text.size() : e + 1;
        if (line.empty() || line[0] == '#') continue;
        char kind[16], file[256], sha[80], dig[80];
        if (std::sscanf(line.c_str(), "%15s %255s %79s %79s", kind, file, sha, dig) != 4) { check(false, "fixture pack: line parses", line); continue; }
        n++;
        std::string src = dir + "/" + file;
        if (!std::strcmp(kind, "convert")) {
            // gates 1 + 6 + 4: stream-convert emod.py's fixture; bytes must equal emod.py's
            std::string name = file;
            if (name.size() > 3 && name.compare(name.size() - 3, 3, ".gz") == 0) name.resize(name.size() - 3);
            std::string out = P("fx-" + name + ".emod");
            bool ok = convert_file(src, out, 65536, name);
            std::string got = ok ? sha_file(out) : "convert-failed";
            check(got == sha, sfmt("%s: stream conversion byte-identical to emod.py", name.c_str()).c_str(), got == sha ? "" : got);
            bool opened = false, dmg = true;
            std::string d = digest_of(out, &opened, &dmg);
            check(opened && !dmg && d == dig, sfmt("%s: state digest == emod.py's", name.c_str()).c_str(), d);
            EdenWorldStore s;
            if (s.open(out.c_str(), false)) {
                std::vector<std::string> v = s.verify();
                check(v.empty(), sfmt("%s: verify clean", name.c_str()).c_str(), v.empty() ? "" : v[0]);
            }
            // compact(convert(x)) == convert(x)
            std::vector<uint8_t> a, b;
            read_file(out, a);
            std::string cp = P("fx-compact.emod");
            write_file(cp, a);
            EdenWorldStore w;
            bool cok = w.open(cp.c_str(), true) && w.compact();
            w.close();
            read_file(cp, b);
            check(cok && a == b, sfmt("%s: compacting a fresh conversion reproduces it byte for byte", name.c_str()).c_str());
            std::remove(cp.c_str());
            std::remove(out.c_str());
        } else if (!std::strcmp(kind, "read")) {
            // gate 1: read a .emod emod.py wrote (several batches; other codecs) to emod.py's state
            if (src.size() > 3 && src.compare(src.size() - 3, 3, ".gz") == 0) {
                std::string un = P(std::string("fx-") + file);
                un.resize(un.size() - 3);
                gzFile g = gzopen(src.c_str(), "rb");
                std::vector<uint8_t> d;
                uint8_t b[1 << 16];
                int n2;
                while (g && (n2 = gzread(g, b, sizeof b)) > 0) d.insert(d.end(), b, b + n2);
                if (g) gzclose(g);
                write_file(un, d);
                src = un;
            }
            bool opened = false, dmg = true;
            std::string d = digest_of(src, &opened, &dmg);
            check(opened && !dmg && d == dig, sfmt("%s: reads to emod.py's state", file).c_str(), d);
            check(sha_file(src) == sha, sfmt("%s: pack file intact", file).c_str());
            {
                EdenWorldStore s;
                if (s.open(src.c_str(), false)) {
                    std::vector<std::string> v = s.verify();
                    check(v.empty(), sfmt("%s: verify clean", file).c_str(), v.empty() ? "" : v[0]);
                }
            }
            if (src.compare(0, g_work.size(), g_work) == 0) std::remove(src.c_str());
        } else if (!std::strcmp(kind, "flagcompact")) {
            // K.1-2: the flag compaction of an emod.py file == emod.py `compact --set-palette-flag`, byte for byte
            std::vector<uint8_t> a;
            read_file(src, a);
            std::string cp = P("fx-flagcompact.emod");
            write_file(cp, a);
            EdenWorldStore w;
            bool cok = w.open(cp.c_str(), true) && w.compact(true);
            std::string why = cok ? "" : w.error();
            std::vector<std::string> v = cok ? w.verify() : std::vector<std::string>();
            w.close();
            std::string got = cok ? sha_file(cp) : "compact-failed: " + why;
            check(got == sha, sfmt("%s: flag compaction byte-identical to emod.py's", file).c_str(), got == sha ? "" : got);
            check(digest_of(cp) == dig, sfmt("%s: flag compaction state == emod.py's", file).c_str());
            check(cok && v.empty(), sfmt("%s: flag compaction verifies clean", file).c_str(), v.empty() ? "" : v[0]);
            std::remove(cp.c_str());
        } else if (!std::strcmp(kind, "export")) {
            // K.1-4: the own-format export (BPL1 from the record) == emod.py `export`, byte for byte
            std::string m = src;
            if (src.size() < 5 || src.compare(src.size() - 5, 5, ".emod") != 0) {
                std::string name = file;
                if (name.size() > 3 && name.compare(name.size() - 3, 3, ".gz") == 0) name.resize(name.size() - 3);
                m = P("fx-export.emod");
                if (!convert_file(src, m, 65536, name)) m.clear();
            }
            const std::string x = P("fx-export.eden");
            ExportOptions o;
            std::string err = m.empty() ? "convert failed" : "";
            const bool ok = !m.empty() && export_world_file(m, x, o, false, nullptr, &err);
            std::string got = ok ? sha_file(x) : "export-failed: " + err;
            check(got == sha, sfmt("%s: own-format export byte-identical to emod.py's", file).c_str(), got == sha ? "" : got);
            if (m != src) std::remove(m.c_str());
            std::remove(x.c_str());
        } else {
            check(false, "fixture pack: known line kind", kind);
        }
    }
    check(n >= 6, sfmt("fixture pack has entries (%d)", n).c_str());
}

void section_synth() {
    say("== synth: converter vs independently generated worlds");
    std::vector<SynthWorld> ws = synth_worlds();
    const size_t chunks[] = {1, 7, 4096, 1u << 20};
    for (const SynthWorld& w : ws) {
        std::string src = P(w.name + ".eden");
        write_file(src, w.bytes);
        std::string ref = P(w.name + ".emod");
        EdenEmodConverter c;
        bool ok = convert_file(src, ref, 1u << 20, w.name + ".eden", &c);
        check(ok, sfmt("%s: converts", w.name.c_str()).c_str(), ok ? "" : c.error());
        if (!ok) continue;
        std::vector<uint8_t> refb;
        read_file(ref, refb);
        bool same = true;
        for (size_t ch : chunks) {
            if (g_quick && ch == 1 && w.bytes.size() > (1u << 20)) continue;
            std::string o = P(w.name + ".chunk.emod");
            std::vector<uint8_t> b;
            same &= convert_file(src, o, ch, w.name + ".eden") && read_file(o, b) && b == refb;
            std::remove(o.c_str());
        }
        check(same, sfmt("%s: every feed chunking (1/7/4096/1M) gives the same bytes", w.name.c_str()).c_str());
        check(!exists(ref + ".spill") && !exists(ref + ".converting"), sfmt("%s: no temp files left", w.name.c_str()).c_str());
        EdenWorldStore s;
        if (!check(s.open(ref.c_str(), false), sfmt("%s: opens", w.name.c_str()).c_str(), s.error())) continue;
        check(s.bands() == w.bands, sfmt("%s: %d bands detected (want %d)", w.name.c_str(), s.bands(), w.bands).c_str());
        std::vector<uint8_t> buf((size_t)s.bands() * BAND);
        size_t bad = 0;
        for (const auto& kv : w.view)
            if (s.readColumn(kv.first.first, kv.first.second, buf.data()) != EdenWorldStore::READ_OK || buf != kv.second) bad++;
        check(bad == 0 && s.columnCount() == w.view.size(),
              sfmt("%s: all %zu columns read back as the engine's view (%zu wrong)", w.name.c_str(), w.view.size(), bad).c_str());
        std::vector<std::string> v = s.verify();
        check(v.empty(), sfmt("%s: verify clean", w.name.c_str()).c_str(), v.empty() ? "" : v[0]);
        std::vector<uint8_t> h;
        check(s.readRecord(T_WORLD_HEADER, h) == EdenWorldStore::READ_OK && h.size() == EDEN_HEADER &&
              !std::memcmp(h.data(), w.bytes.data(), EDEN_HEADER), sfmt("%s: WORLD_HEADER verbatim", w.name.c_str()).c_str());
        say("   %s: %llu B -> %llu B, %llu columns (%llu copied from the spill, %llu rebuilt), flags %#x", w.name.c_str(),
            (unsigned long long)w.bytes.size(), (unsigned long long)refb.size(), (unsigned long long)c.stats().columns,
            (unsigned long long)c.stats().copied, (unsigned long long)c.stats().rebuilt, c.stats().flags);
        if (w.name == "s256-offgrid") check(c.stats().rebuilt == w.view.size(), "s256-offgrid: every column takes the rebuild path");
        if (w.name == "s256-short") check(c.stats().rebuilt > 0 && c.stats().copied > 0, "s256-short: both paths, short span rebuilt");
        // S.3b: the spill grid is 131,072 B whatever the height, so a canonical 256z world copies
        // every column and a 64z one (a quarter-slot each) rebuilds every column
        if (w.name == "s256-trailer" || w.name == "s256-v2") check(c.stats().rebuilt == 0, sfmt("%s takes the copy path for every column", w.name.c_str()).c_str());
        if (w.name == "s64-canonical") check(c.stats().copied == 0, "s64-canonical takes the rebuild path for every column");
        std::remove(src.c_str());
        std::remove(ref.c_str());
    }
}

void section_errors() {
    say("== errors: refused inputs leave nothing behind");
    std::vector<SynthWorld> ws = synth_worlds();
    const SynthWorld& w = ws[0];
    std::string out = P("err.emod");
    auto clean = [&]() { return !exists(out) && !exists(out + ".spill") && !exists(out + ".converting"); };
    EdenEmodConverter::Options o;
    o.sourceName = "err.eden";
    {
        std::vector<uint8_t> b = w.bytes;
        wr32(&b[92], 136348415u);                        // a 1.x header's version field
        EdenEmodConverter c;
        bool ok = c.begin(out.c_str(), o) && c.feed(b.data(), b.size()) && c.finish();
        check(!ok && c.err() == EdenEmodConverter::E_LEGACY && clean(), "legacy 1.x header refused as E_LEGACY, no temps");
    }
    {
        EdenEmodConverter c;
        size_t half = w.bytes.size() / 2;
        bool ok = c.begin(out.c_str(), o) && c.feed(w.bytes.data(), half) && c.finish();
        check(!ok && c.err() == EdenEmodConverter::E_DIR_OFFSET && clean(), "truncated stream refused (E_DIR_OFFSET), no temps");
    }
    {
        EdenEmodConverter c;
        bool ok = c.begin(out.c_str(), o) && c.feed(w.bytes.data(), 100) && c.finish();
        check(!ok && c.err() == EdenEmodConverter::E_SHORT && clean(), "shorter than a header refused (E_SHORT), no temps");
    }
    {
        EdenEmodConverter c;
        EdenEmodConverter::Options oc = o;
        oc.maxTempBytes = 2000;
        bool ok = c.begin(out.c_str(), oc) && c.feed(w.bytes.data(), w.bytes.size()) && c.finish();
        check(!ok && c.err() == EdenEmodConverter::E_CAP && clean(), "temp-bytes cap aborts (E_CAP), no temps");
    }
    {
        EdenEmodConverter c;
        bool ok = c.begin(out.c_str(), o) && c.feed(w.bytes.data(), 4096);
        c.abort();
        check(ok && clean(), "abort() mid-stream deletes the spill");
    }
    {
        std::vector<uint8_t> junk(4096, 0x42);
        std::string p = P("junk.emod");
        write_file(p, junk);
        EdenWorldStore s;
        check(!s.open(p.c_str(), false), "a non-.emod file is refused", s.error());
        std::string p2 = P("hdronly.emod");
        EdenWorldStore c;
        c.create(p2.c_str(), 4, 0);
        check(!exists(p2) && exists(p2 + ".creating"), "create(): nothing at the final path before the first commit");
        c.close();
        check(!s.open((p2 + ".creating").c_str(), false), "a file header with no commit is refused (no world)", s.error());
        std::remove((p2 + ".creating").c_str());
        std::remove(p.c_str());
    }
}

void section_writer() {
    say("== writer: create, append, ords, SUMMARY, compaction");
    std::string p = P("w.emod");
    std::remove(p.c_str());
    Rng r(77);
    EdenWorldStore s;
    check(s.create(p.c_str(), 4, 1234567), "create()");
    std::vector<uint8_t> c0 = make_column(r, 4, 10, 10, 40), c1 = make_column(r, 4, 11, 10, 40), c2 = make_column(r, 4, 12, 10, 40);
    s.beginBatch();
    s.putColumn(10, 10, c0.data());
    check(!s.commitBatch(), "first batch without WORLD_HEADER/CREATURES refused");
    std::vector<uint8_t> hdr = eden_header(4, "writer", 0), cr(200 * EDEN_ENT, 0);
    s.beginBatch();
    s.putRecord(T_WORLD_HEADER, hdr.data(), hdr.size());
    s.putRecord(T_CREATURES, cr.data(), cr.size());
    s.putColumn(10, 10, c0.data());
    s.putColumn(11, 10, c1.data());
    s.putColumn(12, 10, c2.data());
    check(s.commitBatch(), "first batch commits", s.error());
    check(exists(p) && !exists(p + ".creating"), "the world appears at its path only after the first commit");
    check(!s.putColumn(0, 0, c0.data()), "key (0,0) refused (twoToOne key 0 is invalid)");
    s.close();
    check(s.open(p.c_str(), true), "reopens", s.error());
    std::vector<std::pair<int32_t, int32_t> > k = s.columnKeys();
    check(k.size() == 3 && k[0] == std::make_pair(10, 10) && k[2] == std::make_pair(12, 10), "ords 0,1,2 in put order");
    // a rewrite keeps its ord; a new column takes next_ord; SUMMARY exact
    std::vector<uint8_t> c1b = c1;
    c1b[5] ^= 1;
    std::vector<uint8_t> tall(4 * BAND, 0);
    tall[3 * BAND + 9] = 5;                               // only band 3
    s.beginBatch();
    s.putColumn(11, 10, c1b.data());
    s.putColumn(13, 10, tall.data());
    check(s.commitBatch(), "second batch commits", s.error());
    k = s.columnKeys();
    check(k.size() == 4 && k[1] == std::make_pair(11, 10) && k[3] == std::make_pair(13, 10), "rewrite keeps ord 1, new column gets ord 3");
    EdenWorldStore::Summary sm = s.summary();
    check(sm.liveColumns == 4 && sm.nextOrd == 4 && (sm.bandOr & 8), "SUMMARY counts the live state");
    check(s.columnMask(13, 10) == 8, "a band-3-only column stores mask 0x8");
    std::vector<uint8_t> got(4 * BAND);
    check(s.readColumn(11, 10, got.data()) == EdenWorldStore::READ_OK && got == c1b, "the newest version is read");
    check(s.lastSeq() == 2, "seq 1, 2");
    std::vector<std::string> v = s.verify();
    check(v.empty(), "verify clean after appends (SUMMARY exact, prev_commit chain)", v.empty() ? "" : v[0]);
    s.close();
    // many saves, then compaction
    s.open(p.c_str(), true);
    for (int i = 0; i < 40; i++) {
        s.beginBatch();
        c0[100 + i] ^= 0x33;
        s.putColumn(10, 10, c0.data());
        s.putRecord(T_CREATURES, cr.data(), cr.size());
        if (!s.commitBatch()) break;
    }
    check(s.lastSeq() == 42, "40 more saves");
    std::string before = s.stateDigest();
    uint64_t sizeBefore = s.fileSize(), dead = s.deadBytes();
    check(dead > 0, sfmt("dead bytes accounted (%llu of %llu)", (unsigned long long)dead, (unsigned long long)sizeBefore).c_str());
    check(s.compact(), "compact()", s.error());
    check(s.stateDigest() == before && s.fileSize() < sizeBefore && s.lastSeq() == 1 && s.deadBytes() == 0,
          sfmt("compaction keeps the state, %llu -> %llu B, one batch", (unsigned long long)sizeBefore, (unsigned long long)s.fileSize()).c_str());
    v = s.verify();
    check(v.empty(), "verify clean after compaction", v.empty() ? "" : v[0]);
    check(!exists(p + ".compact"), "no .compact left");
    // a save after compaction continues from seq 2
    s.beginBatch();
    s.putColumn(14, 10, c2.data());
    check(s.commitBatch() && s.lastSeq() == 2 && s.columnKeys().back() == std::make_pair(14, 10), "appends continue after compaction");
    s.close();
    // a read-only store refuses writes
    s.open(p.c_str(), false);
    check(!s.putColumn(15, 10, c2.data()) && !s.compact(), "a read-only open refuses writes and compaction");
    s.close();
    std::remove(p.c_str());
}

// DN.2: record 11 SKY_CLOCK (docs/emod-file-format.md "SKY_CLOCK"). Raw bytes on purpose: this file
// links no engine code, so the payload is built here, not by Classes/SkyCycle.
std::vector<uint8_t> sky_clock(float t, uint8_t flags) {
    std::vector<uint8_t> k(SKY_CLOCK_LEN, 0);
    std::memcpy(k.data(), &t, 4);
    k[4] = flags;
    return k;
}
bool has_warning(const EdenWorldStore& s, const char* what) {
    for (const std::string& w : s.warnings()) if (w.find(what) != std::string::npos) return true;
    return false;
}

void section_skyclock() {
    say("== SKY_CLOCK (DN.2): round trip, supersede, compaction, old readers, bad length");
    std::string p = P("sky.emod");
    std::remove(p.c_str());
    Rng r(11);
    EdenWorldStore s;
    check(s.create(p.c_str(), 4, 1234567), "create()");
    std::vector<uint8_t> hdr = eden_header(4, "sky", 0), cr(200 * EDEN_ENT, 0), c0 = make_column(r, 4, 10, 10, 40);
    s.beginBatch();
    s.putRecord(T_WORLD_HEADER, hdr.data(), hdr.size());
    s.putRecord(T_CREATURES, cr.data(), cr.size());
    s.putColumn(10, 10, c0.data());
    check(s.commitBatch(), "a world with no clock commits", s.error());
    const std::string d0 = s.stateDigest();
    check(!s.hasRecord(T_SKY_CLOCK), "no SKY_CLOCK until a save writes one");
    std::vector<uint8_t> k1 = sky_clock(0.30f, 0), k2 = sky_clock(0.7421875f, 1), got;
    s.beginBatch();
    check(!s.putRecord(T_SKY_CLOCK, k1.data(), 15), "putRecord refuses a 15-byte SKY_CLOCK");
    check(s.putRecord(T_SKY_CLOCK, k1.data(), k1.size()), "putRecord takes 16 bytes", s.error());
    s.putRecord(T_WORLD_HEADER, hdr.data(), hdr.size());
    check(s.commitBatch(), "clock + header batch commits (the exit/warp shape)", s.error());
    s.close();
    check(s.open(p.c_str(), true), "reopens", s.error());
    check(s.readRecord(T_SKY_CLOCK, got) == EdenWorldStore::READ_OK && got == k1, "round trip: the clock reads back byte for byte");
    check(!has_warning(s, "reserved type"), "type 11 is a known type (no reserved-type warning)");
    check(s.stateDigest() == d0, "the clock is not part of the state digest (like GENERATOR)");
    std::vector<std::string> v = s.verify();
    check(v.empty(), "verify clean with a SKY_CLOCK", v.empty() ? "" : v[0]);
    s.beginBatch();
    s.putRecord(T_SKY_CLOCK, k2.data(), k2.size());
    s.putRecord(T_WORLD_HEADER, hdr.data(), hdr.size());
    check(s.commitBatch(), "a newer clock commits", s.error());
    check(s.readRecord(T_SKY_CLOCK, got) == EdenWorldStore::READ_OK && got == k2, "the newest clock wins (flags bit 0 = locked kept)");
    for (int i = 0; i < 6; i++) {   // dead bytes, so the compaction below rewrites something
        s.beginBatch();
        s.putColumn(10, 10, c0.data());
        s.putRecord(T_CREATURES, cr.data(), cr.size());
        s.commitBatch();
    }
    check(s.compact(), "compact()", s.error());
    check(s.readRecord(T_SKY_CLOCK, got) == EdenWorldStore::READ_OK && got == k2 && s.lastSeq() == 1,
          "compaction keeps the newest clock");
    v = s.verify();
    check(v.empty(), "verify clean after compaction", v.empty() ? "" : v[0]);
    s.close();
    // An old reader: what a pre-DN build does with type 11 is what this build does with reserved 12 (10 was the example until K.1-2 made it BLOCK_PALETTE) --
    // a member of its batch, otherwise ignored; verify warns; compaction drops it.
    check(s.open(p.c_str(), true), "reopens", s.error());
    const std::string d1 = s.stateDigest();
    s.beginBatch();
    s.putRecord(12, k1.data(), k1.size());
    s.putRecord(T_WORLD_HEADER, hdr.data(), hdr.size());
    check(s.commitBatch(), "a batch with a reserved-type record commits", s.error());
    s.close();
    check(s.open(p.c_str(), true) && !s.damaged() && has_warning(s, "reserved type 12") && s.stateDigest() == d1 &&
          s.readRecord(T_SKY_CLOCK, got) == EdenWorldStore::READ_OK && got == k2,
          "an unknown singleton is ignored: opens undamaged, state and clock unchanged, a warning");
    check(s.compact() && !s.hasRecord(12) && s.hasRecord(T_SKY_CLOCK), "compaction drops the unknown type, keeps SKY_CLOCK", s.error());
    s.close();
    // A malformed clock (a buggy or foreign writer): the file still opens; verify names it; the engine
    // (SkyCycle::load) ignores a payload that is not 16 bytes.
    check(s.open(p.c_str(), true), "reopens", s.error());
    s.testUncheckedPut = true;
    s.beginBatch();
    s.putRecord(T_SKY_CLOCK, k1.data(), 15);
    s.putRecord(T_WORLD_HEADER, hdr.data(), hdr.size());
    check(s.commitBatch(), "a 15-byte SKY_CLOCK commits under the test hook", s.error());
    s.testUncheckedPut = false;
    s.close();
    check(s.open(p.c_str(), false) && !s.damaged(), "a bad-length clock does not make the world unreadable", s.error());
    check(s.readRecord(T_SKY_CLOCK, got) == EdenWorldStore::READ_OK && got.size() == 15, "it reads back as 15 bytes");
    v = s.verify();
    bool named = false;
    for (const std::string& e : v) named = named || e.find("SKY_CLOCK raw length 15") != std::string::npos;
    check(named, "verify reports the SKY_CLOCK length", v.empty() ? "(clean)" : v[0]);
    s.close();
    std::remove(p.c_str());
}

void section_fallback() {
    say("== fallback: a committed record that does not decode");
    std::vector<SynthWorld> ws = synth_worlds();
    std::string src = P("fb.eden"), p = P("fb.emod");
    write_file(src, ws[0].bytes);
    convert_file(src, p, 1u << 20, "fb.eden");
    std::vector<std::pair<int32_t, int32_t> > keys;
    std::vector<uint8_t> v1(4 * BAND), v2;
    {
        EdenWorldStore s;
        if (!check(s.open(p.c_str(), true) && s.columnCount() >= 2, "fallback fixture converted", s.error())) return;
        keys = s.columnKeys();
        s.readColumn(keys[0].first, keys[0].second, v1.data());
        v2 = v1;
        v2[77] ^= 0x40;
        s.beginBatch();
        s.putColumn(keys[0].first, keys[0].second, v2.data());
        s.commitBatch();
    }
    std::vector<uint8_t> f;
    read_file(p, f);
    std::vector<RecPos> rs = walk(f);
    // break the zstd frame of the NEWEST version of keys[0] (seq 2) and of keys[1] (only version);
    // reseal so both still frame and commit: only decoding can catch them
    for (const RecPos& r : rs) {
        if (r.type != T_COLUMN || r.plen < 16) continue;
        int32_t x = (int32_t)rd32(&f[(size_t)r.off + 8]), z = (int32_t)rd32(&f[(size_t)r.off + 12]);
        bool hit = (r.seq == 2 && std::make_pair(x, z) == keys[0]) || (r.seq == 1 && std::make_pair(x, z) == keys[1]);
        if (!hit) continue;
        f[(size_t)(r.poff + r.plen / 2)] ^= 0xff;
        reseal(f, r);
    }
    write_file(p, f);
    EdenWorldStore s;
    check(s.open(p.c_str(), true) && !s.damaged(), "the resealed file opens with no framing damage");
    std::vector<uint8_t> got(4 * BAND, 0xEE);
    uint16_t m = 0;
    check(s.readColumn(keys[0].first, keys[0].second, got.data(), &m) == EdenWorldStore::READ_FALLBACK && got == v1,
          "an undecodable newest version falls back to the previous committed one");
    check(s.readColumn(keys[1].first, keys[1].second, got.data(), &m) == EdenWorldStore::READ_AIR_DAMAGED &&
          std::all_of(got.begin(), got.end(), [](uint8_t b) { return b == 0; }) && m == 0,
          "an undecodable only version loads as air (never the default map)");
    check(s.damaged(), "both set the damaged flag");
    check(!s.compact() && exists(p) && !exists(p + ".compact"), "compaction is refused while damaged", s.error());
    std::vector<std::string> v = s.verify();
    check(!v.empty(), "verify reports it", v.empty() ? "" : v[0]);
    s.close();
    std::remove(src.c_str());
    std::remove(p.c_str());
}

// K.1-2: the block palette (WORKING/block-registry-plan-2026-10-31.md §3, §6 "Format").
bool palette_has(EdenWorldStore& s, int id, const char* name) {
    std::vector<uint8_t> b;
    std::vector<PaletteEntry> pe;
    if (s.readRecord(T_BLOCK_PALETTE, b) != EdenWorldStore::READ_OK || !palette_parse(b.data(), b.size(), &pe, nullptr)) return false;
    for (const PaletteEntry& e : pe) if (e.id == id) return !name || e.name == name;
    return false;
}
size_t palette_count(EdenWorldStore& s) {
    std::vector<uint8_t> b;
    std::vector<PaletteEntry> pe;
    if (s.readRecord(T_BLOCK_PALETTE, b) != EdenWorldStore::READ_OK) return (size_t)-1;
    palette_parse(b.data(), b.size(), &pe, nullptr);
    return pe.size();
}
// A world converted from synth world 0 (64z), optionally with latent type bytes 131 and 250 put by a
// plain (unflagged) batch -- what a pre-K.1 bug could have left.
bool palette_base(const std::string& p, bool latent) {
    std::vector<SynthWorld> ws = synth_worlds();
    std::string src = p + ".src.eden";
    write_file(src, ws[0].bytes);
    bool ok = convert_file(src, p, 1u << 20, "pal.eden");
    std::remove(src.c_str());
    if (!ok || !latent) return ok;
    EdenWorldStore s;
    if (!s.open(p.c_str(), true)) return false;
    std::vector<std::pair<int32_t, int32_t> > k = s.columnKeys();
    std::vector<uint8_t> c((size_t)s.bands() * BAND);
    s.readColumn(k[0].first, k[0].second, c.data());
    c[5] = 131; c[4000] = 250; c[(size_t)BAND + 6] = 131;
    s.beginBatch();
    return s.putColumn(k[0].first, k[0].second, c.data()) && s.commitBatch();
}
// The first save that uses mod blocks (after the flag compaction): bytes 128-130 + the live palette
// grown by `stairs:oak/r` at 130.
bool palette_batch(EdenWorldStore& s) {
    std::vector<std::pair<int32_t, int32_t> > k = s.columnKeys();
    if (k.size() < 2) return false;
    std::vector<uint8_t> c((size_t)s.bands() * BAND), v2, live;
    std::vector<PaletteEntry> pe;
    s.readRecord(T_BLOCK_PALETTE, live);
    palette_parse(live.data(), live.size(), &pe, nullptr);
    pe.push_back(pent(130, 4, 12, "stairs:oak/r"));
    palette_build(pe, v2);
    s.readColumn(k[1].first, k[1].second, c.data());
    c[10] = 128; c[11] = 129; c[12] = 130; c[4096 + 12] = 7;
    s.beginBatch();
    return s.putColumn(k[1].first, k[1].second, c.data()) && s.putRecord(T_BLOCK_PALETTE, v2.data(), v2.size()) && s.commitBatch();
}

void section_palette() {
    say("== palette: FileHeader flag bit 0 + BLOCK_PALETTE (K.1-2)");
    const std::string p = P("pal.emod"), h = P("pal-latent.emod"), t = P("pal-case.emod");
    if (!check(palette_base(p, false) && palette_base(h, true), "palette bases converted")) return;
    std::vector<uint8_t> v1 = palette_v1(), v2 = palette_v2(), got, before, after;
    {
        EdenWorldStore s;
        s.open(p.c_str(), true);
        const std::string d0 = s.stateDigest();
        s.beginBatch();
        check(!s.putRecord(T_BLOCK_PALETTE, v1.data(), v1.size()), "putRecord refuses a BLOCK_PALETTE in an unflagged file (D14)", s.error());
        s.abortBatch();
        check(!s.compact(false, &v1), "a palette is only given to the flag compaction", s.error());
        check(s.compact(true) && s.paletteFlag() && s.fileFlags() == FH_FLAG_BLOCK_PALETTE && s.latentIds().empty(),
              "flag compaction of a clean world: flag set, no latent bytes", s.error());
        check(palette_count(s) == 0, "... with an empty BLOCK_PALETTE (the flag <=> a live record)");
        std::vector<std::string> v = s.verify();
        check(v.empty() && s.stateDigest() != d0, "verify clean; the palette is part of the state digest", v.empty() ? "" : v[0]);
        s.beginBatch();
        check(s.putRecord(T_BLOCK_PALETTE, v1.data(), v1.size()) && s.commitBatch(), "putRecord takes a palette once flagged", s.error());
        std::vector<uint8_t> drop = palette_bytes({pent(128, 1, 7, "emod:test_glow")});
        std::vector<uint8_t> changed = palette_bytes({pent(128, 1, 8, "emod:test_glow"), pent(129, 2, 0, "stairs:oak")});
        struct Bad { std::vector<uint8_t> b; const char* why; };
        std::vector<Bad> bads = {
            {drop, "drops id 129 (D8)"}, {changed, "changes id 128 (D8)"},
            {palette_bytes({pent(127, 2, 0, "a:b")}), "id 127"},
            {palette_bytes({pent(128, 2, 0, "eden:stone")}), "the stock namespace"},
            {palette_bytes({pent(128, 2, 0, "Mod:x")}), "an upper-case name"},
            {palette_bytes({pent(128, 69, 0, "a:b")}), "a door half as fallback"},
            {palette_bytes({pent(128, 2, 55, "a:b")}), "paint 55"},
            {palette_bytes({pent(128, 2, 0, "a:b"), pent(129, 2, 0, "a:b")}), "a repeated name"},
        };
        std::vector<uint8_t> big(PALETTE_HEAD + 129 * PALETTE_ENTRY, 0);
        wr32(&big[0], 1); wr32(&big[4], 129);
        bads.push_back(Bad{big, "count 129"});
        for (const Bad& b : bads) {
            s.beginBatch();
            check(!s.putRecord(T_BLOCK_PALETTE, b.b.data(), b.b.size()), sfmt("putRecord refuses a palette with %s", b.why).c_str(), s.error());
            s.abortBatch();
        }
        s.beginBatch();
        check(s.putRecord(T_BLOCK_PALETTE, v2.data(), v2.size()) && s.commitBatch() && palette_count(s) == 3 &&
              palette_has(s, 130, "stairs:oak/r"), "a superset commits; the newest version wins", s.error());
        s.close();
        check(s.open(p.c_str(), true) && s.paletteFlag() && palette_count(s) == 3, "reopens flagged with the newest palette", s.error());
        const std::string d1 = s.stateDigest();
        check(!s.compact(true, &v1), "compact(true, palette) on a flagged file is refused", s.error());
        check(s.compact(true) && s.paletteFlag() && s.stateDigest() == d1 && s.lastSeq() == 1, "compact(true) on a flagged file is a plain compaction", s.error());
        std::vector<uint8_t> c1, c2;
        read_file(p, c1);
        check(s.compact() && s.stateDigest() == d1, "plain compaction keeps the flag and the record", s.error());
        read_file(p, c2);
        check(c1 == c2, "and is idempotent, byte for byte");
        v = s.verify();
        check(v.empty(), "verify clean after compaction", v.empty() ? "" : v[0]);
        s.close();
    }
    // D3: a build without acceptPaletteFlag (a pre-K.1 build answers the same) refuses it and writes nothing
    {
        read_file(p, before);
        EdenWorldStore::acceptPaletteFlag = false;
        EdenWorldStore s;
        bool w = s.open(p.c_str(), true), r = s.open(p.c_str(), false), ho = s.open(p.c_str(), false, true, true);
        std::string e = s.error();
        EdenWorldStore::acceptPaletteFlag = true;
        read_file(p, after);
        check(!w && !r && !ho && e.find("newer build") != std::string::npos && before == after,
              "acceptPaletteFlag off: refused (writable, read-only, header-only) as 'written by a newer build'; file byte-identical", e);
        std::vector<uint8_t> f = before;
        f[12] = 3;                                              // bit 1: still a newer build's
        uint32_t c = crc32_of(f.data(), 60);
        wr32(&f[60], c);
        write_file(t, f);
        check(!s.open(t.c_str(), false), "any other flag bit is still refused", s.error());
    }
    // D13: latent bytes >= 128 in an unflagged file
    {
        EdenWorldStore s;
        s.open(h.c_str(), true);
        std::vector<std::string> v = s.verify();
        check(v.empty() && has_warning(s, "without the palette flag: [131, 250]"), "unflagged bytes >= 128: a verify warning, not an error");
        s.close();
        read_file(h, before);
        s.open(h.c_str(), true);
        std::vector<uint8_t> clash = palette_bytes({pent(131, 2, 0, "a:b")});
        check(!s.compact(true, &clash), "a supplied id that is a latent byte is refused (D13)", s.error());
        s.close();
        read_file(h, after);
        check(before == after && !exists(h + ".compact"), "... and nothing was replaced");
        std::vector<uint8_t> one = palette_bytes({pent(128, 2, 0, "a:b")});
        s.open(h.c_str(), true);
        check(s.compact(true, &one) && s.latentIds() == std::vector<uint8_t>({131, 250}), "the flag compaction finds 131, 250", s.error());
        check(palette_count(s) == 3 && palette_has(s, 128, "a:b") && palette_has(s, 131, "emod:unknown_131") && palette_has(s, 250, "emod:unknown_250"),
              "they are reserved as emod:unknown_<id> beside the supplied entry");
        v = s.verify();
        check(v.empty(), "verify clean", v.empty() ? "" : v[0]);
        std::vector<std::pair<int32_t, int32_t> > k = s.columnKeys();
        std::vector<uint8_t> c((size_t)s.bands() * BAND);
        s.readColumn(k[0].first, k[0].second, c.data());
        c[7] = 200;
        s.beginBatch();
        s.putColumn(k[0].first, k[0].second, c.data());
        s.commitBatch();
        v = s.verify();
        check(v.empty() && has_warning(s, "no BLOCK_PALETTE entry (drawn as the missing block): [200]"), "a flagged byte with no entry: a warning");
        s.close();
    }
    // Crash injection: the flag compaction killed after every byte of its `.compact` file. The path
    // always holds the old (unflagged) world byte for byte; the next try produces the uncrashed file.
    {
        palette_base(h, true);
        std::vector<uint8_t> b0, b1;
        read_file(h, b0);
        const std::string d0 = digest_of(h);
        std::string full = P("pal-full.emod");
        write_file(full, b0);
        {
            EdenWorldStore s;
            s.open(full.c_str(), true);
            s.compact(true, &v1);
        }
        read_file(full, b1);
        const std::string d1 = digest_of(full);
        size_t cases = 0, oldOk = 0, retryOk = 0;
        const size_t step = g_quick ? 13 : 1;
        for (size_t k = 0; k < b1.size(); k += step) {
            cases++;
            write_file(t, b0);
            std::remove((t + ".compact").c_str());
            {
                EdenWorldStore s;
                s.open(t.c_str(), true);
                s.testCompactCrashAfter = k;
                s.compact(true, &v1);
            }
            std::vector<uint8_t> now, part;
            read_file(t, now);
            read_file(t + ".compact", part);
            bool opened = false, dmg = true;
            std::string d = digest_of(t, &opened, &dmg);
            if (now == b0 && opened && !dmg && d == d0 && part.size() == k) oldOk++;
            {
                EdenWorldStore s;
                bool ok = s.open(t.c_str(), true) && !s.paletteFlag() && s.compact(true, &v1);
                s.close();
                read_file(t, now);
                if (ok && now == b1 && !exists(t + ".compact")) retryOk++;
            }
        }
        check(cases == b1.size() / step + (b1.size() % step ? 1 : 0) && oldOk == cases && d0 != d1,
              sfmt("flag compaction killed at %zu points of its %zu B file: the world is the old one byte for byte every time (%zu)",
                   cases, b1.size(), oldOk).c_str());
        check(retryOk == cases, sfmt("... and the next try writes exactly the uncrashed file over the stale .compact (%zu/%zu)", retryOk, cases).c_str());
        // the first palette batch on the flagged world, killed after every byte (prefix and zero-filled)
        std::vector<uint8_t> f1;
        {
            EdenWorldStore s;
            s.open(full.c_str(), true);
            check(palette_batch(s), "first palette batch commits", s.error());
        }
        read_file(full, f1);
        const std::string d2 = digest_of(full);
        const size_t L = f1.size() - b1.size();
        size_t bc = 0, bOld = 0, bNew = 0, bBad = 0;
        for (int zero = 0; zero < 2; zero++)
            for (size_t k = 0; k < L; k += step) {
                bc++;
                write_file(t, b1);
                {
                    EdenWorldStore s;
                    s.open(t.c_str(), true);
                    s.testCrashAfter = k;
                    s.testCrashZeroFill = zero != 0;
                    palette_batch(s);
                }
                EdenWorldStore s;
                bool ok = s.open(t.c_str(), false) && s.paletteFlag() && !s.damaged();
                std::string d = ok ? s.stateDigest() : "refused";
                size_t n = ok ? palette_count(s) : 0;
                if (ok && d == d1 && n == 4) bOld++;              // 128, 129 + unknown 131, 250
                else if (ok && d == d2 && n == 5) bNew++;
                else bBad++;
            }
        check(bBad == 0 && bOld + bNew == bc,
              sfmt("first palette batch killed at %zu points (%zu B, prefix + zero-filled): %zu old, %zu new, %zu wrong; always flagged with a record",
                   bc, L, bOld, bNew, bBad).c_str());
        std::remove(full.c_str());
    }
    std::remove(p.c_str());
    std::remove(h.c_str());
    std::remove(t.c_str());
    std::remove((t + ".compact").c_str());
}

// ---- K.1-4: interchange --------------------------------------------------------------------------
struct EdenView {                      // an exported `.eden`, converted back
    bool ok, flagged;
    std::vector<uint8_t> trailer, palette;
    std::vector<std::pair<int32_t, int32_t> > keys;
    std::vector<std::vector<uint8_t> > cols;
    int bands;
};
EdenView view_eden(const std::string& eden, const std::string& tag) {
    EdenView v;
    v.ok = v.flagged = false;
    v.bands = 0;
    const std::string m = P("ix-" + tag + ".emod");
    EdenEmodConverter c;
    if (!convert_file(eden, m, 65536, "ix.eden", &c)) return v;
    EdenWorldStore s;
    if (!s.open(m.c_str(), false)) return v;
    v.flagged = s.paletteFlag();
    if (s.hasRecord(T_SIGN_TRAILER)) s.readRecord(T_SIGN_TRAILER, v.trailer);
    if (s.hasRecord(T_BLOCK_PALETTE)) s.readRecord(T_BLOCK_PALETTE, v.palette);
    v.keys = s.columnKeys();
    v.bands = s.bands();
    for (const auto& k : v.keys) {
        std::vector<uint8_t> col((size_t)v.bands * BAND);
        s.readColumn(k.first, k.second, col.data());
        v.cols.push_back(col);
    }
    v.ok = s.verify().empty();
    s.close();
    std::remove(m.c_str());
    return v;
}
bool any_high(const EdenView& v) {
    bool seen[256] = {false};
    for (const auto& c : v.cols) mark_high_type_bytes(c.data(), v.bands, seen);
    for (int i = 128; i < 256; i++) if (seen[i]) return true;
    return false;
}
const std::vector<uint8_t>* col_of(const EdenView& v, int32_t x, int32_t z) {
    for (size_t i = 0; i < v.keys.size(); i++) if (v.keys[i].first == x && v.keys[i].second == z) return &v.cols[i];
    return nullptr;
}

void section_interchange(const std::string& dir) {
    say("== interchange (K.1-4)");
    // f256-palette.2batch.emod: flagged, palette V2 (128 emod:test_glow 1/7, 129 stairs:oak 2/0, 130 stairs:oak/r 4/12);
    // its second column carries 128, 129, 130 and the unassigned 200 at y 10..13 (emod.py fixtures_pack)
    const std::string src = dir + "/f256-palette.2batch.emod";
    EdenWorldStore s;
    if (!check(s.open(src.c_str(), false) && s.paletteFlag(), "interchange: the palette fixture opens flagged", s.error())) return;
    std::vector<uint8_t> rec;
    s.readRecord(T_BLOCK_PALETTE, rec);
    std::vector<std::pair<int32_t, int32_t> > keys = s.columnKeys();
    int32_t cx = 0, cz = 0;
    std::vector<uint8_t> scol(16 * BAND);
    bool found = false;
    for (const auto& k : keys) {
        s.readColumn(k.first, k.second, scol.data());
        if (scol[10] == 128 && scol[11] == 129 && scol[12] == 130 && scol[13] == 200) { cx = k.first; cz = k.second; found = true; break; }
    }
    s.close();
    if (!check(found, "interchange: the fixture's mod-block column is there")) return;
    const uint8_t p11 = scol[4096 + 11], p13 = scol[4096 + 13];

    // stock targets, and OWN with the upload's fallbacks: fallback type + paint, nothing >= 128, no BPL1
    struct T { int target; bool fallbacks; const char* name; };
    const T ts[] = {{X_LEGACY64Z, false, "Legacy64z"}, {X_NEWDAWN256Z, false, "NewDawn256z"},
                    {X_NEWFORMAT256Z, false, "NewFormat256z"}, {X_OWN, true, "own + paletteFallbacks"}};
    for (const T& t : ts) {
        ExportOptions o;
        o.target = t.target;
        o.paletteFallbacks = t.fallbacks;
        ExportReport r;
        std::string err;
        const std::string out = P("ix-out.eden");
        const bool ok = export_world_file(src, out, o, false, &r, &err);
        const std::string d = sfmt("%s; %s; %s", err.c_str(), r.json().c_str(), r.summary().c_str());
        check(ok && r.scanned && r.paletteEntries == 3 && r.paletteReplaced == 4 && r.paletteUnassigned == 1 && r.paletteColumns == 1 &&
                  r.paletteById[0] == 1 && r.paletteById[1] == 1 && r.paletteById[2] == 1 && r.paletteById[72] == 1 && !r.paletteWritten &&
                  r.lossy() && r.summary().find("4 mod blocks become their stock fallback") != std::string::npos &&
                  r.json().find("\"stairs:oak/r\":1") != std::string::npos && r.json().find("\"#200\":1") != std::string::npos,
              sfmt("%s: the report counts each mod block by name (4: 3 named, 1 unassigned)", t.name).c_str(), d);
        EdenView v = view_eden(out, "fb");
        const std::vector<uint8_t>* c = col_of(v, cx, cz);
        check(v.ok && !v.flagged && v.palette.empty() && !any_high(v) && !trailer_find_palette(v.trailer.data(), v.trailer.size(), nullptr),
              sfmt("%s: no byte >= 128, no BPL1, converts back unflagged", t.name).c_str());
        check(c && (*c)[10] == 1 && (*c)[4096 + 10] == 7 && (*c)[11] == 2 && (*c)[4096 + 11] == p11 && (*c)[12] == 4 &&
                  (*c)[4096 + 12] == 12 && (*c)[13] == 2 && (*c)[4096 + 13] == p13,
              sfmt("%s: fallback type + its paint (paint 0 keeps the voxel's; unassigned -> stone)", t.name).c_str());
        std::remove(out.c_str());
    }

    // OWN: raw bytes + BPL1 = the record; back in, the same palette plus the latent 200's reservation
    {
        ExportOptions o;
        ExportReport r;
        std::string err;
        const std::string out = P("ix-own.eden");
        const bool ok = export_world_file(src, out, o, false, &r, &err);
        check(ok && r.paletteWritten && r.paletteEntries == 3 && r.paletteReplaced == 0 && !r.scanned,
              "own: BPL1 written, nothing replaced, no scan", err + " " + r.json());
        EdenView v = view_eden(out, "own");
        std::vector<uint8_t> bpl1;
        std::vector<PaletteEntry> es;
        palette_parse(v.palette.data(), v.palette.size(), &es, nullptr);
        bool same = trailer_find_palette(v.trailer.data(), v.trailer.size(), &bpl1) && bpl1 == rec;
        const std::vector<uint8_t>* c = col_of(v, cx, cz);
        check(same, "own: the export's BPL1 is the record, byte for byte");
        check(v.ok && v.flagged && es.size() == 4 && es[3].id == 200 && es[3].name == "emod:unknown_200" && c && *c == scol,
              "own -> convert: flagged, the palette + emod:unknown_200 (D13), the column's bytes kept", sfmt("%zu entries", es.size()));
        std::remove(out.c_str());
    }

    // keepPalette (Convert to 64z): Legacy64z cut, raw bytes >= 128, a BPL1-only trailer -> back in flagged
    {
        ExportOptions o;
        o.target = X_LEGACY64Z;
        o.keepPalette = true;
        ExportReport r;
        std::string err;
        const std::string out = P("ix-64.eden");
        const bool ok = export_world_file(src, out, o, false, &r, &err);
        EdenView v = view_eden(out, "64");
        std::vector<uint8_t> bpl1;
        const std::vector<uint8_t>* c = col_of(v, cx, cz);
        check(ok && r.paletteWritten && r.paletteReplaced == 0 && v.ok && v.flagged && v.bands == 4 &&
                  trailer_find_palette(v.trailer.data(), v.trailer.size(), &bpl1) && bpl1 == rec && v.trailer.size() == 16 * ((16 + rec.size() + 11) / 12) &&
                  c && (*c)[10] == 128 && (*c)[11] == 129 && (*c)[12] == 130 && (*c)[13] == 200,
              "keepPalette Legacy64z: raw bytes, a BPL1-only trailer, converts back flagged at 64z", err + " " + r.json());
        ExportOptions both = o;
        both.paletteFallbacks = true;
        check(export_world_file(src, out, both, false, &r, &err) && r.paletteReplaced == 4 && !r.paletteWritten,
              "paletteFallbacks wins over keepPalette");
        std::remove(out.c_str());
    }

    // the upload body (D9): even the own format carries fallbacks
    {
        const std::string png = P("ix.png"), body = P("ix.body");
        const uint8_t fake[] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10, 1, 2, 3};
        write_file(png, fake, sizeof fake);
        ExportReport r;
        std::string err;
        const bool ok = build_upload_body(UPLOAD_CURRENT, src, png, ExportOptions(), body, &r, &err);
        std::vector<uint8_t> b;
        read_file(body, b);
        const std::string bs(b.begin(), b.end());
        const size_t a = bs.find("\r\n\r\n");
        const size_t e = bs.find(std::string("\r\n--") + UPLOAD_BOUNDARY + "\r\n");
        bool parsed = ok && a != std::string::npos && e != std::string::npos && e > a + 4;
        if (parsed) write_file(P("ix-up.eden.gz"), b.data() + a + 4, e - a - 4);
        EdenView v = parsed ? view_eden(P("ix-up.eden.gz"), "up") : EdenView();
        check(parsed && r.paletteReplaced == 4 && v.ok && !v.flagged && !any_high(v) && v.bands == 16,
              "upload (Current, own format): the body's world has no byte >= 128 and no palette", err + " " + r.json());
        std::remove(png.c_str()); std::remove(body.c_str()); std::remove(P("ix-up.eden.gz").c_str());
    }

    // an unflagged world: a stock target writes a stray byte >= 128 as stone
    {
        const std::string hb = dir + "/f64-highbytes.emod";      // unflagged, latent 131 and 250
        ExportOptions o;
        o.target = X_NEWDAWN256Z;
        ExportReport r;
        std::string err;
        const std::string out = P("ix-hb.eden");
        const bool ok = export_world_file(hb, out, o, false, &r, &err);
        EdenView v = view_eden(out, "hb");
        check(ok && r.paletteEntries == 0 && r.paletteReplaced == 3 && r.paletteUnassigned == 3 && v.ok && !any_high(v),
              "unflagged world, NewDawn256z: its 3 stray bytes >= 128 become stone (counted: this target always scans)",
              sfmt("ok %d err '%s' entries %u replaced %llu view ok %d high %d", ok, err.c_str(), r.paletteEntries,
                   (unsigned long long)r.paletteReplaced, v.ok, any_high(v)));
        std::remove(out.c_str());
    }
}

void section_crash() {
    say("== crash: kill commitBatch() after every byte of a batch (K4)");
    std::vector<SynthWorld> ws = synth_worlds();
    std::string src = P("cr.eden"), base = P("cr-base.emod"), full = P("cr-full.emod"), t = P("cr-case.emod");
    write_file(src, ws[1].bytes);                                 // 256z + trailer
    convert_file(src, base, 1u << 20, "cr.eden");
    std::vector<uint8_t> b0, b1;
    if (!check(read_file(base, b0) && b0.size() > FILE_HEADER, "crash base converted")) return;
    write_file(full, b0);
    {
        EdenWorldStore s;
        s.open(full.c_str(), true);
        check(second_batch(s, 99), "reference batch commits", s.error());
    }
    read_file(full, b1);
    const std::string d0 = digest_of(base), d1 = digest_of(full);
    check(d0 != d1 && b1.size() > b0.size(), "the batch changes the state");
    const size_t L = b1.size() - b0.size();
    std::vector<RecPos> rs = walk(b1);
    std::set<size_t> ks;
    size_t step = g_quick ? 23 : 1;
    for (size_t k = 0; k < L; k += step) ks.insert(k);
    for (const RecPos& r : rs)
        if (r.off >= b0.size())
            for (int d = -1; d <= 1; d++) { long k = (long)(r.off - b0.size()) + d; if (k >= 0 && (size_t)k < L) ks.insert((size_t)k); }
    size_t cases = 0, old = 0, neu = 0, mixed = 0, refused = 0, damagedN = 0, rewriteBad = 0;
    for (int zero = 0; zero < 2; zero++)
        for (size_t k : ks) {
            cases++;
            write_file(t, b0);
            {
                EdenWorldStore s;
                s.open(t.c_str(), true);
                s.testCrashAfter = k;
                s.testCrashZeroFill = zero != 0;
                second_batch(s, 99);
            }
            bool opened = false, dmg = false;
            std::string d = digest_of(t, &opened, &dmg);
            // a zero-filled kill point can leave a file byte-identical to the finished one (the
            // batch's last bytes were zero anyway): then, and only then, the new commit is right
            std::vector<uint8_t> persisted;
            read_file(t, persisted);
            const std::string& want = persisted == b1 ? d1 : d0;
            if (!opened) refused++;
            else if (d == want) { if (d == d1) neu++; else old++; }
            else mixed++;
            if (dmg) damagedN++;
            // ... and the next save truncates the torn tail and lands cleanly
            if (opened && d == d0) {
                EdenWorldStore s;
                s.open(t.c_str(), true);
                if (!second_batch(s, 99) || s.stateDigest() != d1) rewriteBad++;
                s.close();
                std::vector<uint8_t> after;
                read_file(t, after);
                if (after != b1) rewriteBad++;
            }
        }
    check(mixed == 0 && refused == 0 && old + neu == cases,
          sfmt("%zu kill points (%zu B batch, prefix + zero-filled): %zu old commit, %zu new (file already complete), %zu wrong/mixed, %zu refused",
               cases, L, old, neu, mixed, refused).c_str());
    check(damagedN == 0, sfmt("a torn tail is never reported as damage (%zu were)", damagedN).c_str());
    check(rewriteBad == 0, sfmt("the next save after every crash writes exactly the uncrashed file (%zu differ)", rewriteBad).c_str());
    // a reordering disk: the COMMIT reached the platter, one record of the batch did not
    size_t lost = 0, lostOk = 0;
    for (const RecPos& r : rs) {
        if (r.off < b0.size() || r.type == T_COMMIT) continue;
        lost++;
        std::vector<uint8_t> f = b1;
        uint64_t n = REC_HEADER + (uint64_t)r.plen;
        std::memset(&f[(size_t)r.off], 0, (size_t)(n + ((8 - (n & 7)) & 7)));
        write_file(t, f);
        bool opened = false, dmg = false;
        EdenWorldStore s;
        opened = s.open(t.c_str(), false);
        if (opened && s.stateDigest() == d0 && s.torn() && !s.damaged()) lostOk++;
    }
    check(lost > 0 && lostOk == lost, sfmt("a lost record under a surviving COMMIT rolls back to the old commit (%zu/%zu)", lostOk, lost).c_str());
    // a LONG torn tail, then a short save: the writer must cut the tail before appending, or the
    // stale records (and a stale COMMIT) would sit after the new commit
    {
        auto small = [](EdenWorldStore& s) {
            Rng r2(3);
            std::vector<uint8_t> c = make_column(r2, s.bands(), 5100, 5100, 30);
            s.beginBatch();
            return s.putColumn(5100, 5100, c.data()) && s.commitBatch();
        };
        std::vector<uint8_t> bS;
        write_file(t, b0);
        { EdenWorldStore s; s.open(t.c_str(), true); small(s); }
        read_file(t, bS);
        std::vector<std::vector<uint8_t> > variants;
        variants.push_back(std::vector<uint8_t>(b1.begin(), b1.end() - 1));       // killed one byte short
        std::vector<uint8_t> lostRec = b1;                                       // COMMIT survived, a record did not
        for (const RecPos& r : rs) if (r.off >= b0.size() && r.type == T_COLUMN) { std::memset(&lostRec[(size_t)r.off], 0, 32); break; }
        variants.push_back(lostRec);
        size_t good = 0;
        for (const auto& v : variants) {
            write_file(t, v);
            EdenWorldStore s;
            bool ok = s.open(t.c_str(), true) && small(s);
            s.close();
            std::vector<uint8_t> after;
            read_file(t, after);
            if (ok && after == bS) good++;
        }
        check(good == variants.size() && bS.size() < b1.size(),
              sfmt("a save after a long torn tail truncates it first: the file equals a clean save byte for byte (%zu/%zu)", good, variants.size()).c_str());
    }
    // the full batch with no crash
    write_file(t, b0);
    {
        EdenWorldStore s;
        s.open(t.c_str(), true);
        s.testCrashAfter = L;                                    // == length: not a crash
        second_batch(s, 99);
    }
    check(digest_of(t) == d1, "a kill point at the batch's last byte is the new commit");
    std::remove(t.c_str());
    std::remove(src.c_str());
    std::remove(base.c_str());
    std::remove(full.c_str());
}

// emod.py corrupt_gate, on this reader. Returns (undetected, mixed).
std::pair<int, int> corrupt_gate(bool checkCrc, int* casesOut) {
    std::vector<SynthWorld> ws = synth_worlds();
    std::string src = P("cg.eden"), base = P("cg-base.emod"), t = P("cg-case.emod");
    write_file(src, ws[1].bytes);
    convert_file(src, base, 1u << 20, "cg.eden");
    {   // K.1-2: the flag and a BLOCK_PALETTE in batch 1, a grown one in batch 2 (emod.py corrupt_gate's base)
        std::vector<uint8_t> v1 = palette_v1();
        EdenWorldStore s;
        if (!s.open(base.c_str(), true) || !s.compact(true, &v1)) { *casesOut = 0; return std::make_pair(1000, 0); }
    }
    const std::string s1 = digest_of(base);
    {
        std::vector<uint8_t> v2 = palette_v2();
        EdenWorldStore s;
        s.open(base.c_str(), true);
        second_batch(s, 5, &v2);
    }
    const std::string s2 = digest_of(base);
    std::vector<uint8_t> blob;
    read_file(base, blob);
    std::vector<RecPos> rs = walk(blob);
    if (rs.size() < 8) { *casesOut = 0; return std::make_pair(1000, 0); }    // the base did not build
    struct Case { std::string name; bool flip; uint64_t at; };
    std::vector<Case> cases;
    std::set<uint64_t> seen;
    for (const RecPos& r : rs) {
        for (int which = 0; which < 2; which++) {
            const RecPos* q = nullptr;
            for (const RecPos& x : rs) if (x.type == r.type) { if (which == 0 && !q) q = &x; if (which == 1) q = &x; }
            if (seen.count(q->off)) continue;
            seen.insert(q->off);
            cases.push_back(Case{sfmt("flip %s type-%d header byte", which ? "last" : "first", q->type), true, q->off + 9});
            if (q->plen) cases.push_back(Case{sfmt("flip %s type-%d payload byte", which ? "last" : "first", q->type), true, q->poff + q->plen / 2});
        }
    }
    cases.push_back(Case{"flip file header byte (bands)", true, 10});
    cases.push_back(Case{"flip file header byte (created_unix)", true, 20});
    cases.push_back(Case{"flip file header byte (flags)", true, 12});
    const RecPos* lastCol = nullptr;
    for (const RecPos& r : rs) if (r.type == T_COLUMN) lastCol = &r;
    cases.push_back(Case{"truncate mid-COLUMN in the last batch", false, lastCol->poff + lastCol->plen / 2});
    cases.push_back(Case{"truncate mid-COMMIT of the last batch", false, rs.back().off + 20});
    cases.push_back(Case{"truncate mid-record in the first batch", false, rs[3].off + 10});
    int undetected = 0, mixed = 0;
    for (const Case& c : cases) {
        std::vector<uint8_t> bad = blob;
        if (c.flip) bad[(size_t)c.at] ^= 0xff; else bad.resize((size_t)c.at);
        write_file(t, bad);
        EdenWorldStore s;
        bool opened = s.open(t.c_str(), false, checkCrc);
        bool reported = !opened;
        std::string sd;
        std::vector<std::string> probs;
        if (opened) {
            bool dmgAtOpen = s.damaged();
            probs = s.verify();
            reported = !probs.empty() || s.tailBytes() || s.torn();
            if (!dmgAtOpen) sd = s.stateDigest();
        }
        bool mix = !sd.empty() && sd != s1 && sd != s2 && probs.empty();
        if (!reported) undetected++;
        if (mix) mixed++;
        if (!reported || mix || !checkCrc)
            say("   %-44s %s%s", c.name.c_str(), reported ? "reported" : "NOT DETECTED", mix ? " + MIXED STATE" : "");
    }
    *casesOut = (int)cases.size();
    std::remove(t.c_str());
    std::remove(src.c_str());
    std::remove(base.c_str());
    return std::make_pair(undetected, mixed);
}

void section_corrupt() {
    say("== corrupt gate (emod.py's cases, on this reader)");
    int n = 0;
    std::pair<int, int> on = corrupt_gate(true, &n);
    check(on.first == 0 && on.second == 0, sfmt("CRC on: %d cases, %d not detected, %d mixed", n, on.first, on.second).c_str());
    std::pair<int, int> off = corrupt_gate(false, &n);
    check(off.first + off.second > 0,
          sfmt("control: CRC check DISABLED, the gate must fail -- %d not detected, %d mixed", off.first, off.second).c_str());
}

// A synthetic world streamed straight into the converter (no source file): `ncols` columns.
bool stream_synthetic(const std::string& out, int ncols, int bands, EdenEmodConverter& c) {
    EdenEmodConverter::Options o;
    o.sourceName = "synthetic.eden";
    const uint64_t col = (uint64_t)bands * BAND;
    const uint32_t slots = bands == 16 ? 400 : 200;
    uint64_t dirOff = EDEN_HEADER + (uint64_t)ncols * col + (uint64_t)slots * EDEN_ENT;
    if (!c.begin(out.c_str(), o)) return false;
    std::vector<uint8_t> h = eden_header(bands == 16 ? 5 : 4, "synthetic", dirOff);
    if (!c.feed(h.data(), h.size())) return false;
    Rng r(4711);
    int side = 1;
    while (side * side < ncols) side++;
    for (int i = 0; i < ncols; i++) {
        std::vector<uint8_t> cdata = make_column(r, bands, 1000 + i % side, 1000 + i / side, 60);
        if (!c.feed(cdata.data(), cdata.size())) return false;
    }
    std::vector<uint8_t> cr((size_t)slots * EDEN_ENT, 0);
    if (!c.feed(cr.data(), cr.size())) return false;
    std::vector<uint8_t> dir;
    for (int i = 0; i < ncols; i++) {
        uint8_t b[16];
        wr32(b, (uint32_t)(1000 + i % side));
        wr32(b + 4, (uint32_t)(1000 + i / side));
        wr64(b + 8, EDEN_HEADER + (uint64_t)i * col);
        dir.insert(dir.end(), b, b + 16);
    }
    return c.feed(dir.data(), dir.size()) && c.finish();
}

void section_opentime() {
    say("== open time on a 30,000-column log (decides INDEX)");
    const int N = g_quick ? 3000 : 30000;
    std::string p = P("big.emod");
    EdenEmodConverter c;
    double t0 = now_ms();
    bool ok = stream_synthetic(p, N, 4, c);
    double tconv = now_ms() - t0;
    if (!check(ok, sfmt("%d-column 64z world streamed (%.0f ms, %.1f MB in -> %.2f MB)", N, tconv,
                        c.stats().bytesIn / 1048576.0, c.stats().outBytes / 1048576.0).c_str(), c.error())) return;
    // 20 autosave-sized batches on top (144 dirty columns each)
    {
        EdenWorldStore s;
        s.open(p.c_str(), true);
        std::vector<std::pair<int32_t, int32_t> > k = s.columnKeys();
        std::vector<uint8_t> buf(4 * BAND);
        for (int b = 0; b < 20 && !k.empty(); b++) {
            s.beginBatch();
            for (int i = 0; i < 144; i++) {
                const auto& key = k[(size_t)(b * 997 + i * 13) % k.size()];
                s.readColumn(key.first, key.second, buf.data());
                buf[(size_t)(b + i) % buf.size()] ^= 1;
                s.putColumn(key.first, key.second, buf.data());
            }
            s.commitBatch();
        }
    }
    std::vector<double> ms;
    size_t cols = 0;
    uint64_t sz = 0;
    for (int i = 0; i < 5; i++) {
        EdenWorldStore s;
        if (!s.open(p.c_str(), false)) break;
        ms.push_back(s.openMs());
        cols = s.columnCount();
        sz = s.fileSize();
    }
    std::sort(ms.begin(), ms.end());
    double med = ms.empty() ? -1 : ms[ms.size() / 2];
    check(cols == (size_t)N, sfmt("open(): %zu columns, %.2f MB, 21 batches: median %.1f ms (min %.1f, max %.1f) over 5 opens; "
                                  "x3 (A8X proxy) = %.0f ms vs the 100 ms INDEX threshold -> %s",
                                  cols, sz / 1048576.0, med, ms.empty() ? 0 : ms.front(), ms.empty() ? 0 : ms.back(), med * 3,
                                  med * 3 > 100 ? "INDEX WANTED" : "no INDEX").c_str());
    // RAM: what the converter held, 1/10 of the columns vs all of them
    EdenEmodConverter small;
    std::string p2 = P("small.emod");
    stream_synthetic(p2, N / 10, 4, small);
    uint64_t a = small.stats().peakHeldBytes, b = c.stats().peakHeldBytes;
    double perCol = (double)(b - a) / (N - N / 10);
    check(perCol < 64, sfmt("converter RAM: %llu B held at %d columns, %llu B at %d -- %.1f B per extra column (directory row + slot index), "
                            "nothing per byte of world", (unsigned long long)a, N / 10, (unsigned long long)b, N, perCol).c_str());
    std::remove(p.c_str());
    std::remove(p2.c_str());
}

long peak_rss_kb() {
#if defined(_WIN32)
    return -1;
#else
    struct rusage u;
    getrusage(RUSAGE_SELF, &u);
#  if defined(__APPLE__)
    return u.ru_maxrss / 1024;
#  else
    return u.ru_maxrss;
#  endif
#endif
}

void section_file(const std::string& file, const std::string& ref) {
    say("== file: %s", file.c_str());
    std::string name = base_name(file);
    if (name.size() > 3 && name.compare(name.size() - 3, 3, ".gz") == 0) name.resize(name.size() - 3);
    std::string out = P("file-" + name + ".emod");
    long rss0 = peak_rss_kb();
    EdenEmodConverter c;
    double t0 = now_ms();
    bool ok = convert_file(file, out, 1u << 20, name, &c);
    double ms = now_ms() - t0;
    long rss1 = peak_rss_kb();
    const EdenEmodConverter::Stats& s = c.stats();
    check(ok, sfmt("%s: streamed in %.0f ms: %.1f MB in, %llu columns (%llu copied, %llu rebuilt), spill %.2f MB, out %.2f MB, "
                   "peak held %.0f KB, peak RSS %ld -> %ld MB", name.c_str(), ms, s.bytesIn / 1048576.0,
                   (unsigned long long)s.columns, (unsigned long long)s.copied, (unsigned long long)s.rebuilt, s.spillBytes / 1048576.0,
                   s.outBytes / 1048576.0, s.peakHeldBytes / 1024.0, rss0 / 1024, rss1 / 1024).c_str(), c.error());
    if (!ok) return;
    if (!ref.empty()) {
        std::string a = sha_file(out), b = sha_file(ref);
        check(a == b, sfmt("%s: byte-identical to emod.py's conversion", name.c_str()).c_str(), a == b ? "" : a + " vs " + b);
    }
    EdenWorldStore st;
    if (check(st.open(out.c_str(), false), sfmt("%s: opens", name.c_str()).c_str(), st.error())) {
        say("   open %.1f ms, %zu columns", st.openMs(), st.columnCount());
        std::vector<std::string> v = st.verify();
        check(v.empty(), sfmt("%s: verify clean", name.c_str()).c_str(), v.empty() ? "" : v[0]);
    }
    std::remove(out.c_str());
}

}  // namespace

// flags: 1 = quick (stride the crash points, smaller open-time log: the wasm Debug build),
//        2 = keep the work directory's last files (none are kept today; reserved)
extern "C" EMOD_EXPORT int eden_emod_selftest_run(const char* fixtureDir, const char* workDir, int flags,
                                                  const char* file, const char* ref) {
    g_checks = g_fails = 0;
    g_quick = (flags & 1) != 0;
    // K.1-2: the gates read flagged files; the game keeps this off until K.1-3 (EdenWorldStore.h)
    const bool acceptBefore = EdenWorldStore::acceptPaletteFlag;
    EdenWorldStore::acceptPaletteFlag = true;
    g_work = workDir && *workDir ? workDir : "emod-work";
    mkdir_p(g_work);
    double t0 = now_ms();
    if (file && *file) {
        section_file(file, ref ? ref : "");
    } else {
        if (fixtureDir && *fixtureDir) section_fixtures(fixtureDir);
        else say("== fixtures: no --emod-fixtures=DIR given; the emod.py byte comparison did NOT run");
        section_synth();
        section_errors();
        section_writer();
        section_skyclock();
        section_fallback();
        section_palette();
        if (fixtureDir && *fixtureDir) section_interchange(fixtureDir);
        section_crash();
        section_corrupt();
        section_opentime();
    }
    EdenWorldStore::acceptPaletteFlag = acceptBefore;
    say("%d checks, %d failed (%.1f s)%s", g_checks, g_fails, (now_ms() - t0) / 1000.0, g_fails ? "" : " -- ALL PASS");
    return g_fails == 0 && g_checks > 0 ? 0 : 1;
}

// web-port-final:web/tools/headless-emod-store-test.js: the build exports no string helpers, so the paths are
// fixed — the test copies the pack into MEMFS /emod-fixtures first.
extern "C" EMOD_EXPORT int eden_emod_selftest_web(int flags) {
    return eden_emod_selftest_run("/emod-fixtures", "/emod-work", flags, "", "");
}

