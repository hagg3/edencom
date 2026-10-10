//
//  EdenWorldExport.cpp
//  Eden — Stage S / S.5 + S.5b. See EdenWorldExport.h. The OWN target restates `emod.py export`
//  (web/tools/emod.py), and LEGACY64Z restates FileManager::convertWorldTo64's cut rules; the S.5b
//  gates hold the three side by side.
//
#include "EdenWorldExport.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <set>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <sys/statvfs.h>
#endif

namespace emod {

namespace {

// WorldFileHeader / EntityData field offsets (FileManager.h, Vector.h; docs/eden-file-format.md)
const size_t H_POS_Y = 8, H_HOME_Y = 20, H_DIR_OFF = 32, H_VERSION = 92, H_HASH = 96, HASH_LEN = 36;
const size_t E_POS_Y = 4, E_TYPE = 28;
const uint32_t SLOTS_64 = 200, SLOTS_256 = 400;
// Block ids (Classes/Constants.h)
const uint8_t T_STONE = 2, T_DOOR1 = 66, T_DOOR4 = 69, T_DOOR_TOP = 70, T_PORTAL1 = 75, T_PORTAL4 = 78, T_PORTAL_TOP = 79;
const size_t PLANE = 4096;               // one band's type bytes; its paint bytes follow

inline uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
inline uint64_t rd64(const uint8_t* p) { return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32); }
inline void wr32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
inline void wr64(uint8_t* p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }
inline float rdf(const uint8_t* p) { uint32_t u = rd32(p); float f; std::memcpy(&f, &u, 4); return f; }
inline void wrf(uint8_t* p, float f) { uint32_t u; std::memcpy(&u, &f, 4); wr32(p, u); }
inline int cc(int x, int z, int y) { return x * 256 + z * 16 + y; }   // CC(x,z,y), y fastest
inline int32_t floordiv16(int32_t v) { return v >= 0 ? v / 16 : -((-v + 15) / 16); }
inline uint64_t colKey(int32_t x, int32_t z) { return ((uint64_t)(uint32_t)x << 32) | (uint32_t)z; }

std::string fmt(const char* f, ...) {
    char b[1024];                          // json() is ~700 B
    va_list ap;
    va_start(ap, f);
    int n = vsnprintf(b, sizeof b, f, ap);
    va_end(ap);
    if (n < (int)sizeof b) return b;
    std::string big((size_t)n + 1, '\0');
    va_start(ap, f);
    vsnprintf(&big[0], big.size(), f, ap);
    va_end(ap);
    big.resize((size_t)n);
    return big;
}

bool ends_with(const std::string& s, const char* suf) {
    size_t n = std::strlen(suf);
    return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
}

// ---- the post-directory trailer (docs/eden-file-format.md "More than signs") ----------------
// Rows of `ff ff ff ff` + 12 payload bytes. The payload is a list of sections, each a 12-byte
// wrapper `MAGIC | u32 inner_len | u32 0`, then `MAGIC | u32 version | u32 count` and `count`
// fixed-size records starting `i32 x, i32 y, i32 z` (x, y horizontal; z the height). Zero padding
// fills the last row. Anything else: not understood, kept verbatim, never pruned.
struct Section { uint8_t magic[4]; uint32_t w3, ver, count, rs; size_t recs; };

bool parse_trailer(const std::vector<uint8_t>& t, std::vector<uint8_t>& pay, std::vector<Section>& secs) {
    pay.clear(); secs.clear();
    if (t.empty() || t.size() % EDEN_DIR_ROW) return false;
    for (size_t i = 0; i < t.size(); i += EDEN_DIR_ROW) {
        if (rd32(&t[i]) != 0xffffffffu) return false;
        pay.insert(pay.end(), t.begin() + i + 4, t.begin() + i + EDEN_DIR_ROW);
    }
    size_t i = 0;
    while (i + 24 <= pay.size()) {
        if (!rd32(&pay[i])) break;                                // zero padding from here on
        Section s;
        std::memcpy(s.magic, &pay[i], 4);
        uint32_t plen = rd32(&pay[i + 4]);
        s.w3 = rd32(&pay[i + 8]);
        if (plen < 12 || i + 12 + (uint64_t)plen > pay.size()) return false;
        if (std::memcmp(&pay[i + 12], s.magic, 4)) return false;
        s.ver = rd32(&pay[i + 16]);
        s.count = rd32(&pay[i + 20]);
        uint32_t body = plen - 12;
        if (s.count == 0) { if (body) return false; s.rs = 0; }
        else { if (body % s.count) return false; s.rs = body / s.count; if (s.rs < 12) return false; }
        s.recs = i + 24;
        secs.push_back(s);
        i += 12 + plen;
    }
    for (; i < pay.size(); i++) if (pay[i]) return false;
    return true;
}

bool is_magic(const Section& s, const char* m) { return std::memcmp(s.magic, m, 4) == 0; }

}  // namespace

const char* export_target_name(int t) {
    switch (t) {
        case X_LEGACY64Z: return "Legacy64z";
        case X_NEWDAWN256Z: return "NewDawn256z";
        case X_NEWFORMAT256Z: return "NewFormat256z";
        default: return "own";
    }
}

int export_target_from_name(const char* s) {
    if (!s) return -1;
    std::string v(s);
    for (char& c : v) c = (char)std::tolower((unsigned char)c);
    if (v == "own" || v == "") return X_OWN;
    if (v == "legacy64z" || v == "legacy") return X_LEGACY64Z;
    if (v == "newdawn256z" || v == "newdawn") return X_NEWDAWN256Z;
    if (v == "newformat256z" || v == "newformat") return X_NEWFORMAT256Z;
    return -1;
}

// D.2b: stone painted the colorTable swatch (index 1..54) nearest each id's placeholder colour
// (Globals.mm blockColor), by RGB distance against Hud::genColorTable's palette.
static const uint8_t kNewBlockPaint[16] = {38, 36, 32, 38, 45, 11, 38, 40, 45, 43, 29, 9, 18, 45, 36, 36};

uint8_t legacy_block_substitute(uint8_t type) {
    return (type >= 112 && type <= 127) ? T_STONE : type;
}

uint8_t legacy_block_substitute_paint(uint8_t type) {
    return (type >= 112 && type <= 127) ? kNewBlockPaint[type - 112] : 0;
}

bool ExportReport::lossy() const {
    return blocksAbove63 || doorsOrphaned || newBlocksReplaced || creaturesDropped || creaturesOverflow ||
           trailerBytesOut < trailerBytesIn || posClamped || homeClamped;
}

std::string ExportReport::json() const {
    return fmt("{\"target\":\"%s\",\"bandsIn\":%d,\"bandsOut\":%d,\"versionIn\":%d,\"versionOut\":%d,\"bytes\":%llu,"
               "\"columns\":%u,\"creatureSlotsIn\":%u,\"creatureSlotsOut\":%u,\"scanned\":%s,\"blocksAbove63\":%llu,"
               "\"columnsAffected\":%u,\"doorsOrphaned\":%u,\"newBlocksReplaced\":%llu,\"newBlockColumns\":%u,"
               "\"trailerBytesIn\":%u,\"trailerBytesOut\":%u,\"trailerParsed\":%s,\"signsIn\":%u,\"signsOut\":%u,"
               "\"cmdsIn\":%u,\"cmdsOut\":%u,\"otherIn\":%u,\"otherOut\":%u,\"creaturesDropped\":%u,"
               "\"creaturesRelocated\":%u,\"creaturesOverflow\":%u,\"posClamped\":%s,\"homeClamped\":%s,\"lossy\":%s}",
               export_target_name(target), bandsIn, bandsOut, versionIn, versionOut, (unsigned long long)bytes, columns,
               creatureSlotsIn, creatureSlotsOut, scanned ? "true" : "false", (unsigned long long)blocksAbove63,
               columnsAffected, doorsOrphaned, (unsigned long long)newBlocksReplaced, newBlockColumns, trailerBytesIn,
               trailerBytesOut, trailerParsed ? "true" : "false", signsIn, signsOut, cmdsIn, cmdsOut, otherIn, otherOut,
               creaturesDropped, creaturesRelocated, creaturesOverflow, posClamped ? "true" : "false",
               homeClamped ? "true" : "false", lossy() ? "true" : "false");
}

std::string ExportReport::summary() const {
    std::string s;
    auto add = [&](const std::string& p) { s += s.empty() ? p : ", " + p; };
    if (blocksAbove63) add(fmt("%llu blocks above height 63 removed", (unsigned long long)blocksAbove63));
    if (doorsOrphaned) add(fmt("%u cut doors cleared", doorsOrphaned));
    if (newBlocksReplaced) add(fmt("%llu new-block-type blocks become painted stone", (unsigned long long)newBlocksReplaced));
    uint32_t sDrop = signsIn - signsOut, cDrop = cmdsIn - cmdsOut;
    if (sDrop) add(fmt("%u of %u signs dropped", sDrop, signsIn));
    if (cDrop) add(fmt("%u of %u command blocks dropped", cDrop, cmdsIn));
    if (!trailerParsed && trailerBytesOut < trailerBytesIn) add("signs dropped");
    if (creaturesDropped + creaturesOverflow) add(fmt("%u creatures dropped", creaturesDropped + creaturesOverflow));
    if (posClamped || homeClamped) add("player/home moved below the 64-block ceiling");
    if (s.empty()) return "Nothing is lost.";
    s[0] = (char)std::toupper((unsigned char)s[0]);
    return s + ".";
}

// =============================================================================================
// EdenExporter
// =============================================================================================
EdenExporter::EdenExporter() : emod_(false), f_(nullptr), phase_(P_END), ord_(0), cur_(nullptr), curLeft_(0), produced_(0) {
    std::memset(&rep_, 0, sizeof rep_);
}
EdenExporter::~EdenExporter() { close(); }

void EdenExporter::close() {
    if (f_) { std::fclose(f_); f_ = nullptr; }
    store_.close();
}

bool EdenExporter::readSourceColumn(size_t ord, uint8_t* out) {
    const size_t len = (size_t)rep_.bandsIn * BAND;
    if (emod_) {
        EdenWorldStore::ReadResult r = store_.readColumn(keys_[ord].first, keys_[ord].second, out);
        if (r == EdenWorldStore::READ_AIR_DAMAGED) std::memset(out, 0, len);   // export what decodes
        return r != EdenWorldStore::READ_ABSENT;
    }
    std::memset(out, 0, len);              // a short span reads as air, never a neighbour's bytes
    const EdenLayoutCol& c = lay_.cols[ord];
    uint64_t take = std::min<uint64_t>(c.span, len);
    if (!take) return true;
    if (!io::seek_to(f_, c.off)) return false;
    size_t got = std::fread(out, 1, (size_t)take, f_);
    (void)got;                             // short at EOF: the rest stays air, as Eden.column reads it
    return true;
}

// One column, bandsIn -> bandsOut, with the target's block rules. `count` non-null in the
// pre-flight only; the stream calls it with null, and the two must produce the same bytes.
void EdenExporter::transform(const uint8_t* in, uint8_t* out, ExportReport* count) {
    const int bi = rep_.bandsIn, bo = rep_.bandsOut;
    if (bi == 16 && bo == 4) {
        // FileManager::convertWorldTo64: count the non-air TYPE bytes cut, clear a door/portal
        // bottom at y = 63 whose top half was at y = 64, keep bands 0-3.
        std::memcpy(out, in, (size_t)4 * BAND);
        if (count) {
            uint64_t lost = 0;
            for (int b = 4; b < 16; b++) {
                const uint8_t* p = in + (size_t)b * BAND;
                for (size_t j = 0; j < PLANE; j++) lost += p[j] != 0;
            }
            if (lost) { count->blocksAbove63 += lost; count->columnsAffected++; }
        }
        uint8_t* top = out + (size_t)3 * BAND;
        const uint8_t* above = in + (size_t)4 * BAND;
        for (int lx = 0; lx < 16; lx++)
            for (int lz = 0; lz < 16; lz++) {
                int c = cc(lx, lz, 15);
                uint8_t bottom = top[c], up = above[cc(lx, lz, 0)];
                bool orphan = ((bottom >= T_DOOR1 && bottom <= T_DOOR4) && up == T_DOOR_TOP) ||
                              ((bottom >= T_PORTAL1 && bottom <= T_PORTAL4) && up == T_PORTAL_TOP);
                if (orphan) {
                    top[c] = 0;
                    top[PLANE + c] = 0;
                    if (count) count->doorsOrphaned++;
                }
            }
    } else if (bi == 4 && bo == 16) {
        std::memcpy(out, in, (size_t)4 * BAND);
        std::memset(out + (size_t)4 * BAND, 0, (size_t)12 * BAND);
    } else {
        std::memcpy(out, in, (size_t)bo * BAND);
    }
    if (rep_.target == X_LEGACY64Z || rep_.target == X_NEWDAWN256Z) {
        uint64_t n = 0;
        for (int b = 0; b < bo; b++) {
            uint8_t* p = out + (size_t)b * BAND;
            for (size_t j = 0; j < PLANE; j++) {
                uint8_t s = legacy_block_substitute(p[j]);
                if (s != p[j]) { p[PLANE + j] = legacy_block_substitute_paint(p[j]); p[j] = s; n++; }
            }
        }
        if (count && n) { count->newBlocksReplaced += n; count->newBlockColumns++; }
    }
}

bool EdenExporter::begin(const std::string& srcPath, const ExportOptions& opt) {
    close();
    error_.clear();
    std::memset(&rep_, 0, sizeof rep_);
    opt_ = opt;
    produced_ = 0;
    phase_ = P_END;
    keys_.clear(); header_.clear(); creatures_.clear(); trailer_.clear(); dir_.clear();
    rep_.target = opt.target;
    if (opt.target < X_OWN || opt.target > X_NEWFORMAT256Z) return fail("unknown export target");
    std::vector<std::pair<int32_t, int32_t> > dirOrder;
    emod_ = ends_with(srcPath, ".emod");
    if (emod_) {
        if (!store_.open(srcPath.c_str(), false)) return fail("cannot open " + srcPath + ": " + store_.error());
        if (store_.readRecord(T_WORLD_HEADER, header_) > EdenWorldStore::READ_FALLBACK || header_.size() != EDEN_HEADER)
            return fail("the world header does not decode");
        if (store_.readRecord(T_CREATURES, creatures_) > EdenWorldStore::READ_FALLBACK || creatures_.size() % EDEN_ENT)
            return fail("the creature block does not decode");
        if (store_.hasRecord(T_SIGN_TRAILER) && store_.readRecord(T_SIGN_TRAILER, trailer_) > EdenWorldStore::READ_FALLBACK)
            trailer_.clear();
        keys_ = store_.columnKeys();
        rep_.bandsIn = store_.bands();
        std::vector<uint8_t> p;
        if (store_.hasRecord(T_PROVENANCE) && store_.readRecord(T_PROVENANCE, p) <= EdenWorldStore::READ_FALLBACK && p.size() >= 80) {
            // PROVENANCE (spec): ... u32 name_len @76, name, u32 n_short, n_short x 12, u32 n_dir, n_dir x (x, z)
            size_t q = 80 + (size_t)rd32(&p[76]);
            if (q + 4 <= p.size()) {
                q += 4 + (size_t)rd32(&p[q]) * 12;
                if (q + 4 <= p.size()) {
                    size_t nd = rd32(&p[q]);
                    q += 4;
                    for (size_t k = 0; k < nd && q + 8 <= p.size(); k++, q += 8)
                        dirOrder.push_back(std::make_pair((int32_t)rd32(&p[q]), (int32_t)rd32(&p[q + 4])));
                }
            }
        }
    } else {
        f_ = io::fopen_utf8(srcPath.c_str(), "rb");
        if (!f_) return fail("cannot open " + srcPath);
        uint64_t size = io::file_size(f_);
        header_.resize(EDEN_HEADER);
        if (size < EDEN_HEADER || !io::seek_to(f_, 0) || std::fread(header_.data(), 1, EDEN_HEADER, f_) != EDEN_HEADER)
            return fail("shorter than a 192-byte header");
        // A `.gz`/`.zip` behind a `.eden` name (archive downloads often are): say so rather than
        // reading its bytes as a 1.x header. Importing or playing it converts it (ImportJob sniffs).
        if ((header_[0] == 0x1f && header_[1] == 0x8b) || (header_[0] == 'P' && header_[1] == 'K' && header_[2] == 3 && header_[3] == 4))
            return fail("a compressed archive, not a world file: play or import it first");
        int32_t v = (int32_t)rd32(&header_[H_VERSION]);
        if (v < 1 || v > 1000) return fail("a 1.x world: play it once so the game upgrades it, then export");
        if (v > 6) return fail(fmt("header version %d is newer than any known .eden", v));
        uint64_t dirOff = rd64(&header_[H_DIR_OFF]);
        if (dirOff < EDEN_HEADER || dirOff > size) return fail("directory offset outside the file (corrupt or truncated)");
        if (size - dirOff > (256u << 20)) return fail("directory region over 256 MiB: not a world");
        std::vector<uint8_t> dir((size_t)(size - dirOff));
        if (!dir.empty() && (!io::seek_to(f_, dirOff) || std::fread(dir.data(), 1, dir.size(), f_) != dir.size()))
            return fail("cannot read the directory");
        if (!eden_read_layout(header_.data(), size, dir, lay_)) return fail("the creature block would start inside the header");
        rep_.bandsIn = lay_.bands;
        for (const EdenLayoutCol& c : lay_.cols) keys_.push_back(std::make_pair(c.x, c.z));
        dirOrder = lay_.dirOrder;
        trailer_ = lay_.trailer;
        creatures_.resize((size_t)lay_.slots * EDEN_ENT);
        if (!creatures_.empty() && (!io::seek_to(f_, lay_.blockEnd) || std::fread(creatures_.data(), 1, creatures_.size(), f_) != creatures_.size()))
            return fail("cannot read the creature block");
    }

    // ---- shape of the output
    const int t = opt.target;
    rep_.versionIn = (int32_t)rd32(&header_[H_VERSION]);
    rep_.bandsOut = t == X_OWN ? rep_.bandsIn : (t == X_LEGACY64Z ? 4 : 16);
    if (t == X_OWN) rep_.versionOut = rep_.versionIn;
    else if (t == X_LEGACY64Z) rep_.versionOut = rep_.bandsIn == 16 ? 4 : rep_.versionIn;
    else if (t == X_NEWDAWN256Z) rep_.versionOut = 5;
    else rep_.versionOut = rep_.bandsIn == 16 ? rep_.versionIn : 5;
    rep_.columns = (uint32_t)keys_.size();

    // ---- trailer: kept (own, NewFormat), pruned, or dropped (the pre-2026 eras have none)
    rep_.trailerBytesIn = (uint32_t)trailer_.size();
    std::vector<uint8_t> pay;
    std::vector<Section> secs;
    rep_.trailerParsed = trailer_.empty() || parse_trailer(trailer_, pay, secs);
    std::set<uint64_t> have;
    if (opt.signs == SIGNS_PRUNE) for (const auto& k : keys_) have.insert(colKey(k.first, k.second));
    std::vector<uint8_t> outPay;
    for (const Section& s : secs) {
        uint32_t& in = is_magic(s, "SGN1") ? rep_.signsIn : is_magic(s, "CMB1") ? rep_.cmdsIn : rep_.otherIn;
        uint32_t& out = is_magic(s, "SGN1") ? rep_.signsOut : is_magic(s, "CMB1") ? rep_.cmdsOut : rep_.otherOut;
        in += s.count;
        std::vector<uint8_t> recs;
        uint32_t kept = 0;
        for (uint32_t k = 0; k < s.count; k++) {
            const uint8_t* r = &pay[s.recs + (size_t)k * s.rs];
            if (opt.signs == SIGNS_PRUNE &&
                !have.count(colKey(floordiv16((int32_t)rd32(r)), floordiv16((int32_t)rd32(r + 4))))) continue;
            recs.insert(recs.end(), r, r + s.rs);
            kept++;
        }
        bool keepsTrailer = t == X_OWN || t == X_NEWFORMAT256Z;
        if (keepsTrailer) out += kept;
        if (!kept) continue;
        uint8_t h[24];
        std::memcpy(h, s.magic, 4); wr32(h + 4, 12 + kept * s.rs); wr32(h + 8, s.w3);
        std::memcpy(h + 12, s.magic, 4); wr32(h + 16, s.ver); wr32(h + 20, kept);
        outPay.insert(outPay.end(), h, h + 24);
        outPay.insert(outPay.end(), recs.begin(), recs.end());
    }
    if (t == X_LEGACY64Z || t == X_NEWDAWN256Z) {
        trailer_.clear();
    } else if (opt.signs == SIGNS_PRUNE && rep_.trailerParsed && !secs.empty()) {
        // Rebuilt only when something was dropped, so "prune" on a world with nothing outside its
        // columns stays byte-identical.
        if (rep_.signsOut + rep_.cmdsOut + rep_.otherOut != rep_.signsIn + rep_.cmdsIn + rep_.otherIn) {
            outPay.resize((outPay.size() + 11) / 12 * 12, 0);
            trailer_.clear();
            for (size_t i = 0; i < outPay.size(); i += 12) {
                static const uint8_t tag[4] = {0xff, 0xff, 0xff, 0xff};
                trailer_.insert(trailer_.end(), tag, tag + 4);
                trailer_.insert(trailer_.end(), outPay.begin() + i, outPay.begin() + i + 12);
            }
        }
    } else {
        rep_.signsOut = rep_.signsIn; rep_.cmdsOut = rep_.cmdsIn; rep_.otherOut = rep_.otherIn;
    }
    if (t == X_LEGACY64Z || t == X_NEWDAWN256Z) { rep_.signsOut = rep_.cmdsOut = rep_.otherOut = 0; }
    rep_.trailerBytesOut = (uint32_t)trailer_.size();

    // ---- creatures
    const uint32_t srcSlots = (uint32_t)(creatures_.size() / EDEN_ENT);
    rep_.creatureSlotsIn = srcSlots;
    if (rep_.bandsIn == 16 && rep_.bandsOut == 4) {
        // convertWorldTo64: keep slot positions, drop y >= 64, relocate survivors from slots >= 200
        std::vector<uint8_t> out((size_t)SLOTS_64 * EDEN_ENT, 0);
        std::vector<uint32_t> freeSlots;
        for (uint32_t i = 0; i < SLOTS_64; i++) {
            uint8_t* d = &out[(size_t)i * EDEN_ENT];
            if (i < srcSlots) {
                std::memcpy(d, &creatures_[(size_t)i * EDEN_ENT], EDEN_ENT);
                int32_t type = (int32_t)rd32(d + E_TYPE);
                if (type != -1 && rdf(d + E_POS_Y) >= 64) { wr32(d + E_TYPE, 0xffffffffu); rep_.creaturesDropped++; freeSlots.push_back(i); }
                else if (type == -1) freeSlots.push_back(i);
            } else {
                wr32(d + E_TYPE, 0xffffffffu);
                freeSlots.push_back(i);
            }
        }
        for (uint32_t i = SLOTS_64; i < srcSlots; i++) {
            const uint8_t* s = &creatures_[(size_t)i * EDEN_ENT];
            if ((int32_t)rd32(s + E_TYPE) == -1) continue;
            if (rdf(s + E_POS_Y) >= 64) { rep_.creaturesDropped++; continue; }
            if (freeSlots.empty()) { rep_.creaturesOverflow++; continue; }
            uint32_t dst = freeSlots.back();
            freeSlots.pop_back();
            std::memcpy(&out[(size_t)dst * EDEN_ENT], s, EDEN_ENT);
            rep_.creaturesRelocated++;
        }
        creatures_.swap(out);
    } else if (rep_.bandsIn == 4 && rep_.bandsOut == 16 && srcSlots < SLOTS_256) {
        // eden-convert.js --to-256: the source's slots verbatim, then empty slots out to 400
        size_t was = creatures_.size();
        creatures_.resize((size_t)SLOTS_256 * EDEN_ENT, 0);
        for (size_t o = was; o < creatures_.size(); o += EDEN_ENT) wr32(&creatures_[o + E_TYPE], 0xffffffffu);
    }
    rep_.creatureSlotsOut = (uint32_t)(creatures_.size() / EDEN_ENT);

    // ---- header (directory_offset, version, the cut's clamps, the upload's preview hash)
    const uint64_t colOut = (uint64_t)rep_.bandsOut * BAND;
    const uint64_t n = keys_.size();
    const uint64_t dirOff = EDEN_HEADER + n * colOut + creatures_.size();
    wr64(&header_[H_DIR_OFF], dirOff);
    wr32(&header_[H_VERSION], (uint32_t)rep_.versionOut);
    if (rep_.bandsIn == 16 && rep_.bandsOut == 4) {
        float py = rdf(&header_[H_POS_Y]), hy = rdf(&header_[H_HOME_Y]);
        if (!(py < 63)) { wrf(&header_[H_POS_Y], 63); rep_.posClamped = true; }
        if (py < 0) { wrf(&header_[H_POS_Y], 0); rep_.posClamped = true; }
        if (!(hy < 63)) { wrf(&header_[H_HOME_Y], 63); rep_.homeClamped = true; }
        if (hy < 0) { wrf(&header_[H_HOME_Y], 0); rep_.homeClamped = true; }
    }
    if (opt.overrideHash) {
        std::memset(&header_[H_HASH], 0, HASH_LEN);
        std::memcpy(&header_[H_HASH], opt.hash, std::min<size_t>(std::strlen(opt.hash), 32));
    }

    // ---- directory: the source's row order (PROVENANCE / the `.eden`'s rows), then anything new
    std::unordered_map<uint64_t, uint32_t> ordOf;
    for (size_t i = 0; i < keys_.size(); i++) ordOf[colKey(keys_[i].first, keys_[i].second)] = (uint32_t)i;
    std::vector<uint32_t> rows;
    std::vector<bool> seen(keys_.size(), false);
    for (const auto& k : dirOrder) {
        auto it = ordOf.find(colKey(k.first, k.second));
        if (it == ordOf.end() || seen[it->second]) continue;
        seen[it->second] = true;
        rows.push_back(it->second);
    }
    for (uint32_t i = 0; i < keys_.size(); i++) if (!seen[i]) rows.push_back(i);
    dir_.resize(rows.size() * EDEN_DIR_ROW);
    for (size_t r = 0; r < rows.size(); r++) {
        uint8_t* p = &dir_[r * EDEN_DIR_ROW];
        wr32(p, (uint32_t)keys_[rows[r]].first);
        wr32(p + 4, (uint32_t)keys_[rows[r]].second);
        wr64(p + 8, EDEN_HEADER + (uint64_t)rows[r] * colOut);
    }
    rep_.bytes = dirOff + dir_.size() + trailer_.size();

    // ---- the pre-flight's block counts (only targets that change blocks need the decode)
    inCol_.resize((size_t)rep_.bandsIn * BAND);
    outCol_.resize((size_t)rep_.bandsOut * BAND);
    if (t == X_LEGACY64Z || t == X_NEWDAWN256Z) {
        rep_.scanned = true;
        for (size_t i = 0; i < keys_.size(); i++) {
            if (!readSourceColumn(i, inCol_.data())) return fail(fmt("cannot read column (%d,%d)", keys_[i].first, keys_[i].second));
            transform(inCol_.data(), outCol_.data(), &rep_);
        }
    }

    phase_ = P_HEADER;
    cur_ = header_.data();
    curLeft_ = header_.size();
    ord_ = 0;
    return true;
}

// The current chunk ran out: load the next one (a column, or the next region).
bool EdenExporter::fillPhase() {
    for (;;) {
        switch (phase_) {
            case P_HEADER: phase_ = P_COLUMNS; ord_ = 0; break;
            case P_COLUMNS: ord_++; break;
            case P_CREATURES: phase_ = P_DIRECTORY; cur_ = dir_.data(); curLeft_ = dir_.size(); break;
            case P_DIRECTORY: phase_ = P_TRAILER; cur_ = trailer_.data(); curLeft_ = trailer_.size(); break;
            case P_TRAILER: phase_ = P_END; break;
            case P_END: return false;
        }
        if (phase_ == P_COLUMNS) {
            if (ord_ >= keys_.size()) { phase_ = P_CREATURES; cur_ = creatures_.data(); curLeft_ = creatures_.size(); }
            else {
                if (!readSourceColumn(ord_, inCol_.data()))
                    return fail(fmt("cannot read column (%d,%d)", keys_[ord_].first, keys_[ord_].second));
                transform(inCol_.data(), outCol_.data(), nullptr);
                cur_ = outCol_.data();
                curLeft_ = outCol_.size();
            }
        }
        if (phase_ == P_END) {
            if (produced_ != rep_.bytes) return fail(fmt("exported %llu B, pre-flight said %llu B", (unsigned long long)produced_, (unsigned long long)rep_.bytes));
            return false;
        }
        if (curLeft_) return true;
    }
}

size_t EdenExporter::read(void* dst, size_t n) {
    uint8_t* o = (uint8_t*)dst;
    size_t got = 0;
    while (got < n && !failed()) {
        if (!curLeft_ && !fillPhase()) break;
        size_t take = std::min(n - got, curLeft_);
        std::memcpy(o + got, cur_, take);
        cur_ += take; curLeft_ -= take; got += take; produced_ += take;
    }
    return failed() ? 0 : got;
}

// =============================================================================================
// GzipPump
// =============================================================================================
GzipPump::GzipPump() : src_(nullptr), on_(false), done_(false), srcEnd_(false), in_(0) { std::memset(&zs_, 0, sizeof zs_); }
GzipPump::~GzipPump() { if (on_) deflateEnd(&zs_); }

bool GzipPump::begin(EdenExporter* src, int level) {
    if (on_) { deflateEnd(&zs_); on_ = false; }
    std::memset(&zs_, 0, sizeof zs_);
    src_ = src; done_ = false; srcEnd_ = false; in_ = 0; error_.clear();
    if (deflateInit2(&zs_, level, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) { error_ = "deflateInit2 failed"; return false; }
    on_ = true;
    static gz_header h;                    // read by deflate() lazily: must outlive begin(); all zero
    std::memset(&h, 0, sizeof h);
    h.os = 255;                            // "unknown": zlib's default OS_CODE differs per platform
    deflateSetHeader(&zs_, &h);
    buf_.resize(256 << 10);
    return true;
}

size_t GzipPump::read(void* dst, size_t n) {
    if (!on_ || done_ || failed()) return 0;
    zs_.next_out = (Bytef*)dst;
    zs_.avail_out = (uInt)n;
    while (zs_.avail_out && !done_) {
        if (!zs_.avail_in && !srcEnd_) {
            size_t got = src_->read(buf_.data(), buf_.size());
            if (src_->failed()) { error_ = src_->error(); return 0; }
            if (!got) srcEnd_ = true;
            in_ += got;
            zs_.next_in = buf_.data();
            zs_.avail_in = (uInt)got;
        }
        int r = deflate(&zs_, srcEnd_ ? Z_FINISH : Z_NO_FLUSH);
        if (r == Z_STREAM_END) done_ = true;
        else if (r != Z_OK && r != Z_BUF_ERROR) { error_ = "deflate failed"; return 0; }
    }
    return n - zs_.avail_out;
}

// =============================================================================================
// files
// =============================================================================================
uint64_t free_bytes(const std::string& dir) {
    // Test hook (S.5 gate (9)): "N" = always N; "A,B" = A for the first call (the pre-flight), B for
    // every later one (the running recheck), so a harness can let a job start and then starve it.
    if (const char* e = std::getenv("EDEN_TEST_FREE_BYTES")) if (*e) {
        static int calls = 0;
        char* comma = nullptr;
        const uint64_t a = std::strtoull(e, &comma, 10);
        return (comma && *comma == ',' && calls++ > 0) ? std::strtoull(comma + 1, nullptr, 10) : a;
    }
#if defined(_WIN32)
    wchar_t wp[1024];
    ULARGE_INTEGER avail;
    if (!MultiByteToWideChar(CP_UTF8, 0, dir.c_str(), -1, wp, 1024) || !GetDiskFreeSpaceExW(wp, &avail, NULL, NULL))
        return UINT64_MAX;
    return (uint64_t)avail.QuadPart;
#elif defined(__EMSCRIPTEN__)
    (void)dir;
    return UINT64_MAX;                     // MEMFS: the browser's quota is the page's question
#else
    struct statvfs sv;
    if (statvfs(dir.c_str(), &sv) != 0) return UINT64_MAX;
    return (uint64_t)sv.f_bavail * (uint64_t)sv.f_frsize;
#endif
}

ExportFileJob::ExportFileJob() : gzip_(false), o_(nullptr), written_(0), nextCheck_(0) {}
ExportFileJob::~ExportFileJob() { abort(); }

void ExportFileJob::abort() {
    if (o_) { std::fclose(o_); o_ = nullptr; io::remove_file(tmp_); }
    ex_.close();
}

bool ExportFileJob::fail(const std::string& e) {
    if (error_.empty()) error_ = e;
    abort();
    return false;
}

bool ExportFileJob::begin(const std::string& srcPath, const std::string& outPath, const ExportOptions& opt, bool gzip,
                          const std::string& prefix, const std::string& suffix) {
    abort();
    error_.clear();
    written_ = 0;
    nextCheck_ = 64ull << 20;
    gzip_ = gzip;
    out_ = outPath;
    tmp_ = outPath + ".exporting";
    suffix_ = suffix;
    if (!ex_.begin(srcPath, opt)) return fail(ex_.error());
    const uint64_t raw = ex_.report().bytes;
    uint64_t srcSize = 0;
    if (FILE* s = io::fopen_utf8(srcPath.c_str(), "rb")) { srcSize = io::file_size(s); std::fclose(s); }
    size_t sl = outPath.find_last_of("/\\");
    dir_ = sl == std::string::npos ? "." : outPath.substr(0, sl);
    const uint64_t need = (gzip ? 2 * srcSize + raw / 500 : raw + raw / 100) + prefix.size() + suffix.size() + (16ull << 20);
    if (free_bytes(dir_) < need) return fail(fmt("not enough free space (needs about %llu MB)", (unsigned long long)(need >> 20) + 1));
    o_ = io::fopen_utf8(tmp_.c_str(), "wb");
    if (!o_) return fail("cannot create " + tmp_);
    if (gzip && !gz_.begin(&ex_, Z_DEFAULT_COMPRESSION)) return fail(gz_.error());
    if (!prefix.empty() && std::fwrite(prefix.data(), 1, prefix.size(), o_) != prefix.size()) return fail("write failed (disk full?)");
    written_ = prefix.size();
    buf_.resize(1 << 20);
    return true;
}

int ExportFileJob::percent() const {
    const uint64_t raw = ex_.report().bytes;
    return raw ? (int)(99.0 * (double)ex_.produced() / (double)raw) : 0;
}

int ExportFileJob::step(int budgetMs) {
    if (!error_.empty() || !o_) return error_.empty() ? DONE : FAILED;
    auto t0 = std::chrono::steady_clock::now();
    for (;;) {
        size_t got = gzip_ ? gz_.read(buf_.data(), buf_.size()) : ex_.read(buf_.data(), buf_.size());
        if (gzip_ ? gz_.failed() : ex_.failed()) { fail(gzip_ ? gz_.error() : ex_.error()); return FAILED; }
        if (!got) break;
        if (std::fwrite(buf_.data(), 1, got, o_) != got) { fail("write failed (disk full?)"); return FAILED; }
        written_ += got;
        if (written_ >= nextCheck_) {
            nextCheck_ += 64ull << 20;
            if (free_bytes(dir_) < (16ull << 20)) { fail("stopped: the disk is nearly full"); return FAILED; }
        }
        if (std::chrono::steady_clock::now() - t0 > std::chrono::milliseconds(budgetMs)) return RUNNING;
    }
    if (!gzip_ && ex_.produced() != ex_.report().bytes) { fail("the export ended early"); return FAILED; }
    if (!suffix_.empty() && std::fwrite(suffix_.data(), 1, suffix_.size(), o_) != suffix_.size()) { fail("write failed (disk full?)"); return FAILED; }
    written_ += suffix_.size();
    if (!io::sync_file(o_)) { fail("flush failed (disk full?)"); return FAILED; }
    FILE* f = o_;
    o_ = nullptr;
    if (std::fclose(f) != 0) { io::remove_file(tmp_); fail("close failed (disk full?)"); return FAILED; }
    if (!io::replace_file(tmp_, out_)) { io::remove_file(tmp_); fail("cannot rename the export into place"); return FAILED; }
    ex_.close();
    return DONE;
}

bool export_world_file(const std::string& srcPath, const std::string& outPath, const ExportOptions& opt, bool gzip,
                       ExportReport* report, std::string* err) {
    ExportFileJob job;
    bool ok = job.begin(srcPath, outPath, opt, gzip);
    if (report) *report = job.report();
    int r = ok ? ExportFileJob::RUNNING : ExportFileJob::FAILED;
    while (r == ExportFileJob::RUNNING) r = job.step(1000);
    if (r != ExportFileJob::DONE) { if (err) *err = job.error(); return false; }
    return true;
}

// =============================================================================================
// S.5c upload body
// =============================================================================================
const char* const UPLOAD_BOUNDARY = "0xasdfasdfasdfasdfasdf";    // stock FileUpload.mm's BOUNDRY

std::string upload_url(int server, const std::string& uuid) {
    return std::string(server == UPLOAD_LEGACY ? "http://app.edengame.net" : "http://app2.edengame.net") +
           "/upload2.php?uuid=" + uuid;
}

std::string upload_content_type() { return std::string("multipart/form-data; boundary=") + UPLOAD_BOUNDARY; }

std::string md5_file_hex(const std::string& path) {
    static uint32_t K[64];
    static bool init = false;
    if (!init) { for (int i = 0; i < 64; i++) K[i] = (uint32_t)(std::fabs(std::sin((double)(i + 1))) * 4294967296.0); init = true; }
    static const int R[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14, 20,
                              5, 9, 14, 20, 5, 9, 14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                              6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    FILE* f = io::fopen_utf8(path.c_str(), "rb");
    if (!f) return std::string();
    std::vector<uint8_t> d;
    uint8_t b[65536];
    size_t n;
    while ((n = std::fread(b, 1, sizeof b, f)) > 0) d.insert(d.end(), b, b + n);
    std::fclose(f);
    uint64_t bits = (uint64_t)d.size() * 8;
    d.push_back(0x80);
    while (d.size() % 64 != 56) d.push_back(0);
    for (int i = 0; i < 8; i++) d.push_back((uint8_t)(bits >> (8 * i)));
    uint32_t h[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    for (size_t o = 0; o < d.size(); o += 64) {
        uint32_t m[16];
        for (int i = 0; i < 16; i++) m[i] = rd32(&d[o + 4 * i]);
        uint32_t a = h[0], bb = h[1], c = h[2], dd = h[3];
        for (int i = 0; i < 64; i++) {
            uint32_t fv; int g;
            if (i < 16) { fv = (bb & c) | (~bb & dd); g = i; }
            else if (i < 32) { fv = (dd & bb) | (~dd & c); g = (5 * i + 1) % 16; }
            else if (i < 48) { fv = bb ^ c ^ dd; g = (3 * i + 5) % 16; }
            else { fv = c ^ (bb | ~dd); g = (7 * i) % 16; }
            uint32_t t = dd; dd = c; c = bb;
            uint32_t x = a + fv + K[i] + m[g];
            bb = bb + ((x << R[i]) | (x >> (32 - R[i])));
            a = t;
        }
        h[0] += a; h[1] += bb; h[2] += c; h[3] += dd;
    }
    char hex[33];
    for (int i = 0; i < 16; i++) std::snprintf(hex + 2 * i, 3, "%02x", (h[i / 4] >> (8 * (i % 4))) & 0xff);
    return std::string(hex, 32);
}

std::string upload_prefix() {
    return std::string("--") + UPLOAD_BOUNDARY + "\r\n" +
           "Content-Disposition: form-data; name=\"uploaded\"; filename=\"file.bin\"\r\n\r\n";
}

std::string upload_suffix(const std::vector<uint8_t>& png) {
    return std::string("\r\n--") + UPLOAD_BOUNDARY + "\r\n" +
           "Content-Disposition: form-data; name=\"uploaded2\"; filename=\"image.bin\"\r\n\r\n" +
           std::string(png.begin(), png.end()) + "\r\n--" + UPLOAD_BOUNDARY + "--\r\n";
}

bool begin_upload_body(ExportFileJob& job, int server, const std::string& srcPath, const std::string& pngPath,
                       ExportOptions opt, const std::string& bodyPath, std::string* err) {
    if (server == UPLOAD_LEGACY) opt.target = X_LEGACY64Z;     // the Legacy server's clients read nothing else
    std::vector<uint8_t> png;
    if (FILE* p = io::fopen_utf8(pngPath.c_str(), "rb")) {
        uint8_t b[65536]; size_t n;
        while ((n = std::fread(b, 1, sizeof b, p)) > 0) png.insert(png.end(), b, b + n);
        std::fclose(p);
    }
    if (png.empty()) { if (err) *err = "this world has no preview picture yet: take one in camera mode first"; return false; }
    std::string md5 = md5_file_hex(pngPath);
    opt.overrideHash = true;
    std::snprintf(opt.hash, sizeof opt.hash, "%s", md5.c_str());
    // ExportFileJob gzips at Z_DEFAULT_COMPRESSION, windowBits 15+16: stock zpipe.c's compressFile.
    if (!job.begin(srcPath, bodyPath, opt, true, upload_prefix(), upload_suffix(png))) { if (err) *err = job.error(); return false; }
    return true;
}

bool build_upload_body(int server, const std::string& srcPath, const std::string& pngPath, ExportOptions opt,
                       const std::string& bodyPath, ExportReport* report, std::string* err) {
    ExportFileJob job;
    if (!begin_upload_body(job, server, srcPath, pngPath, opt, bodyPath, err)) return false;
    if (report) *report = job.report();
    int r;
    while ((r = job.step(1000)) == ExportFileJob::RUNNING) {}
    if (r != ExportFileJob::DONE) { if (err) *err = job.error(); return false; }
    return true;
}

}  // namespace emod
