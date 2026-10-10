//
//  WorldTrailer.cpp
//  Eden — Stage D / D.3a. See WorldTrailer.h; the format is docs/eden-file-format.md "The trailer model".
//
#include "WorldTrailer.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

bool WorldTrailer::testNoDirtyMark = false;

namespace {
const uint8_t kTag[4] = {0xff, 0xff, 0xff, 0xff};
uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
void put32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; i++) v.push_back((uint8_t)(x >> (8 * i)));
}
bool is(const uint8_t* m, const char* s) { return std::memcmp(m, s, 4) == 0; }
// Sign text: printable ASCII, at most 95 bytes (the record's 96 hold a NUL). Scripts: no NUL, at most 511.
bool okText(const char* t, size_t cap, bool printableOnly) {
    if (!t) return false;
    size_t n = std::strlen(t);
    if (n > cap) return false;
    if (printableOnly)
        for (size_t i = 0; i < n; i++) if ((unsigned char)t[i] < 0x20 || (unsigned char)t[i] > 0x7e) return false;
    return true;
}
}  // namespace

const char* WorldTrailer::resultMessage(int r) {
    switch (r) {
        case TR_OK: return "";
        case TR_FULL: return "No room for more signs in this world";
        case TR_OPAQUE: return "This world's signs can't be edited";
        case TR_EXISTS: return "There is already a sign there";
        case TR_NOT_FOUND: return "That sign is gone";
        default: return "That sign can't be written";
    }
}

WorldTrailer::WorldTrailer() : parsed_(true), dirty_(false) {}

void WorldTrailer::clear() {
    original_.clear();
    sections_.clear();
    tail_.clear();
    signs_.clear();
    cmds_.clear();
    signIndex_.clear();
    cmdIndex_.clear();
    parsed_ = true;
    dirty_ = false;
    why_.clear();
}

bool WorldTrailer::load(const uint8_t* p, size_t n) {
    clear();
    if (n) original_.assign(p, p + n);
    if (!parse(p, n)) {
        sections_.clear(); tail_.clear(); signs_.clear(); cmds_.clear();
        parsed_ = false;
    }
    reindex();
    return parsed_;
}

bool WorldTrailer::parse(const uint8_t* t, size_t n) {
    if (!n) return true;
    char b[160];
    if (n % ROW) { why_ = "not a whole number of 16-byte rows"; return false; }
    std::vector<uint8_t> p;
    p.reserve(n / ROW * ROW_PAYLOAD);
    for (size_t i = 0; i < n; i += ROW) {
        if (std::memcmp(t + i, kTag, 4)) {
            std::snprintf(b, sizeof(b), "row %zu is not tagged ff ff ff ff", i / ROW);
            why_ = b; return false;
        }
        p.insert(p.end(), t + i + 4, t + i + ROW);
    }
    size_t i = 0;
    while (i + 12 <= p.size()) {
        if (!rd32(&p[i]) ) break;                                  // zero tail from here on
        const uint32_t inner = rd32(&p[i + 4]), w3 = rd32(&p[i + 8]);
        if (i + 12 + (uint64_t)inner > p.size()) {
            std::snprintf(b, sizeof(b), "section at payload byte %zu runs past the end", i);
            why_ = b; return false;
        }
        Section s;
        std::memcpy(s.magic, &p[i], 4);
        s.w3 = w3; s.version = 0; s.present = true; s.edited = false;
        const bool sign = is(s.magic, "SGN1"), cmd = is(s.magic, "CMB1");
        if (sign || cmd) {
            s.kind = sign ? K_SIGN : K_CMD;
            const size_t rs = sign ? sizeof(TrailerSign) : sizeof(TrailerCmd);
            for (const Section& o : sections_)
                if (o.kind == s.kind) { why_ = std::string("a second ") + (sign ? "SGN1" : "CMB1") + " section"; return false; }
            if (inner < 12 || std::memcmp(&p[i + 12], s.magic, 4)) { why_ = "a known section without its inner header"; return false; }
            s.version = rd32(&p[i + 16]);
            const uint32_t count = rd32(&p[i + 20]);
            if ((uint64_t)inner - 12 != (uint64_t)count * rs) {
                std::snprintf(b, sizeof(b), "%.4s: %u body bytes for %u records of %zu", (const char*)s.magic, inner - 12, count, rs);
                why_ = b; return false;
            }
            const uint8_t* r = &p[i + 24];
            if (sign) { signs_.resize(count); if (count) std::memcpy(&signs_[0], r, (size_t)count * rs); }
            else { cmds_.resize(count); if (count) std::memcpy(&cmds_[0], r, (size_t)count * rs); }
        } else {
            s.kind = K_RAW;
            s.raw.assign(p.begin() + i, p.begin() + i + 12 + inner);
        }
        sections_.push_back(s);
        i += 12 + inner;
    }
    tail_.assign(p.begin() + i, p.end());
    for (uint8_t c : tail_) if (c) { why_ = "non-zero bytes after the last section"; return false; }
    return true;
}

size_t WorldTrailer::payloadSize() const {
    size_t n = 0;
    for (const Section& s : sections_) {
        if (s.kind == K_RAW) { n += s.raw.size(); continue; }
        const size_t count = s.kind == K_SIGN ? signs_.size() : cmds_.size();
        if (!s.present || (s.edited && !count)) continue;
        n += 24 + count * (s.kind == K_SIGN ? sizeof(TrailerSign) : sizeof(TrailerCmd));
    }
    return n;
}

size_t WorldTrailer::encodedSize() const {
    if (!parsed_ || !dirty_) return original_.size();
    const size_t n = payloadSize();
    if (!n) return 0;
    return (n + tail_.size() + ROW_PAYLOAD - 1) / ROW_PAYLOAD * ROW;
}

std::vector<uint8_t> WorldTrailer::rebuild() const {
    if (!parsed_) return original_;
    std::vector<uint8_t> p;
    for (const Section& s : sections_) {
        if (s.kind == K_RAW) { p.insert(p.end(), s.raw.begin(), s.raw.end()); continue; }
        const bool sign = s.kind == K_SIGN;
        const size_t count = sign ? signs_.size() : cmds_.size();
        if (!s.present || (s.edited && !count)) continue;
        const size_t body = count * (sign ? sizeof(TrailerSign) : sizeof(TrailerCmd));
        p.insert(p.end(), s.magic, s.magic + 4); put32(p, (uint32_t)(12 + body)); put32(p, s.w3);
        p.insert(p.end(), s.magic, s.magic + 4); put32(p, s.version); put32(p, (uint32_t)count);
        const uint8_t* r = count ? (sign ? (const uint8_t*)&signs_[0] : (const uint8_t*)&cmds_[0]) : nullptr;
        if (count) p.insert(p.end(), r, r + body);
    }
    std::vector<uint8_t> out;
    if (p.empty()) return out;                                       // nothing left: no trailer at all
    p.insert(p.end(), tail_.begin(), tail_.end());
    p.resize((p.size() + ROW_PAYLOAD - 1) / ROW_PAYLOAD * ROW_PAYLOAD, 0);
    out.reserve(p.size() / ROW_PAYLOAD * ROW);
    for (size_t i = 0; i < p.size(); i += ROW_PAYLOAD) {
        out.insert(out.end(), kTag, kTag + 4);
        out.insert(out.end(), p.begin() + i, p.begin() + i + ROW_PAYLOAD);
    }
    return out;
}

void WorldTrailer::markSaved() {
    if (!dirty_) return;
    original_ = rebuild();
    // What was just written no longer holds the sections an edit emptied; forget them so a rebuild
    // of this clean state is the written bytes again.
    for (Section& s : sections_) {
        if (s.kind == K_RAW) continue;
        const size_t count = s.kind == K_SIGN ? signs_.size() : cmds_.size();
        if (s.edited && !count) s.present = false;
        s.edited = false;
    }
    if (original_.empty()) tail_.clear();
    dirty_ = false;
}

uint64_t WorldTrailer::key(const TrailerPos& p) {
    // A bucket key, not an identity: lookups compare the full position, so wrap-around collisions
    // only cost a compare.
    return ((uint64_t)((uint32_t)p.x & 0xffffffu) << 40) | ((uint64_t)((uint32_t)p.y & 0xffffffu) << 16) |
           (uint64_t)((uint32_t)p.z & 0xffffu);
}

void WorldTrailer::reindex() {
    signIndex_.clear();
    cmdIndex_.clear();
    for (size_t i = 0; i < signs_.size(); i++) signIndex_[key(signs_[i].pos())].push_back((int)i);
    for (size_t i = 0; i < cmds_.size(); i++) cmdIndex_[key(cmds_[i].pos())].push_back((int)i);
}

int WorldTrailer::findSign(const TrailerPos& anchor, int face) const {
    auto it = signIndex_.find(key(anchor));
    if (it == signIndex_.end()) return -1;
    for (int i : it->second) if (signs_[i].pos() == anchor && signs_[i].a == face) return i;
    return -1;
}

int WorldTrailer::findCmd(const TrailerPos& block) const {
    auto it = cmdIndex_.find(key(block));
    if (it == cmdIndex_.end()) return -1;
    for (int i : it->second) if (cmds_[i].pos() == block) return i;
    return -1;
}

std::vector<int> WorldTrailer::signsOn(const TrailerPos& block) const {
    std::vector<int> out;
    auto it = signIndex_.find(key(block));
    if (it != signIndex_.end())
        for (int i : it->second) if (signs_[i].pos() == block) out.push_back(i);
    return out;
}

bool WorldTrailer::hasAnchored(const TrailerPos& block) const {
    if (signIndex_.empty() && cmdIndex_.empty()) return false;
    return findCmd(block) >= 0 || !signsOn(block).empty();
}

WorldTrailer::Section* WorldTrailer::section(int kind, bool create) {
    for (Section& s : sections_) if (s.kind == kind) return &s;
    if (!create) return nullptr;
    Section s;
    std::memcpy(s.magic, kind == K_SIGN ? "SGN1" : "CMB1", 4);
    s.kind = kind; s.w3 = 0; s.version = 1; s.present = true; s.edited = true;
    // The 2026 game writes CMB1 first, then SGN1: a new CMB1 goes in front of an existing SGN1, a new
    // SGN1 right after an existing CMB1, anything else at the end.
    size_t at = sections_.size();
    for (size_t i = 0; i < sections_.size(); i++) {
        if (kind == K_CMD && sections_[i].kind == K_SIGN) { at = i; break; }
        if (kind == K_SIGN && sections_[i].kind == K_CMD) { at = i + 1; break; }
    }
    sections_.insert(sections_.begin() + at, s);
    return &sections_[at];
}

void WorldTrailer::touch(int kind) {
    Section* s = section(kind, true);
    s->present = true;
    s->edited = true;
    if (!testNoDirtyMark) dirty_ = true;
}

bool WorldTrailer::fits() const {
    const size_t n = payloadSize();
    return n == 0 || (n + tail_.size() + ROW_PAYLOAD - 1) / ROW_PAYLOAD * ROW <= MAX_BYTES;
}

WorldTrailer::Result WorldTrailer::addSign(const TrailerPos& anchor, int face, int colour, int facing, const char* text) {
    if (!parsed_) return TR_OPAQUE;
    if (face < 0 || face > 5 || colour < 1 || colour > 54 || facing < 0 || facing > 3 ||
        !okText(text, sizeof(((TrailerSign*)0)->text) - 1, true))
        return TR_BAD;
    if (findSign(anchor, face) >= 0) return TR_EXISTS;
    const std::vector<Section> before = sections_;
    const bool wasDirty = dirty_;
    TrailerSign s;
    std::memset(&s, 0, sizeof(s));
    s.x = anchor.x; s.y = anchor.y; s.z = anchor.z; s.a = face; s.b = colour; s.c = facing;
    std::memcpy(s.text, text, std::strlen(text));
    signs_.push_back(s);
    touch(K_SIGN);
    if (!fits()) { signs_.pop_back(); sections_ = before; dirty_ = wasDirty; return TR_FULL; }
    signIndex_[key(anchor)].push_back((int)signs_.size() - 1);
    return TR_OK;
}

WorldTrailer::Result WorldTrailer::setSignText(size_t i, const char* text) {
    if (!parsed_) return TR_OPAQUE;
    if (i >= signs_.size()) return TR_NOT_FOUND;
    if (!okText(text, sizeof(signs_[i].text) - 1, true)) return TR_BAD;
    std::memset(signs_[i].text, 0, sizeof(signs_[i].text));
    std::memcpy(signs_[i].text, text, std::strlen(text));
    touch(K_SIGN);
    return TR_OK;
}

WorldTrailer::Result WorldTrailer::setSignColour(size_t i, int colour) {
    if (!parsed_) return TR_OPAQUE;
    if (i >= signs_.size()) return TR_NOT_FOUND;
    if (colour < 1 || colour > 54) return TR_BAD;
    signs_[i].b = colour;
    touch(K_SIGN);
    return TR_OK;
}

WorldTrailer::Result WorldTrailer::removeSign(size_t i) {
    if (!parsed_) return TR_OPAQUE;
    if (i >= signs_.size()) return TR_NOT_FOUND;
    signs_.erase(signs_.begin() + i);
    touch(K_SIGN);
    reindex();
    return TR_OK;
}

WorldTrailer::Result WorldTrailer::addCmd(const TrailerPos& block, const char* script) {
    if (!parsed_) return TR_OPAQUE;
    if (!okText(script, sizeof(((TrailerCmd*)0)->script) - 1, false)) return TR_BAD;
    if (findCmd(block) >= 0) return TR_EXISTS;
    const std::vector<Section> before = sections_;
    const bool wasDirty = dirty_;
    TrailerCmd c;
    std::memset(&c, 0, sizeof(c));
    c.x = block.x; c.y = block.y; c.z = block.z;
    std::memcpy(c.script, script, std::strlen(script));
    cmds_.push_back(c);
    touch(K_CMD);
    if (!fits()) { cmds_.pop_back(); sections_ = before; dirty_ = wasDirty; return TR_FULL; }
    cmdIndex_[key(block)].push_back((int)cmds_.size() - 1);
    return TR_OK;
}

WorldTrailer::Result WorldTrailer::setCmdScript(size_t i, const char* script) {
    if (!parsed_) return TR_OPAQUE;
    if (i >= cmds_.size()) return TR_NOT_FOUND;
    if (!okText(script, sizeof(cmds_[i].script) - 1, false)) return TR_BAD;
    std::memset(cmds_[i].script, 0, sizeof(cmds_[i].script));
    std::memcpy(cmds_[i].script, script, std::strlen(script));
    touch(K_CMD);
    return TR_OK;
}

WorldTrailer::Result WorldTrailer::removeCmd(size_t i) {
    if (!parsed_) return TR_OPAQUE;
    if (i >= cmds_.size()) return TR_NOT_FOUND;
    cmds_.erase(cmds_.begin() + i);
    touch(K_CMD);
    reindex();
    return TR_OK;
}

int WorldTrailer::removeAnchored(const TrailerPos& block) {
    if (!parsed_ || !hasAnchored(block)) return 0;
    const size_t ns = signs_.size(), nc = cmds_.size();
    signs_.erase(std::remove_if(signs_.begin(), signs_.end(), [&](const TrailerSign& s) { return s.pos() == block; }), signs_.end());
    cmds_.erase(std::remove_if(cmds_.begin(), cmds_.end(), [&](const TrailerCmd& c) { return c.pos() == block; }), cmds_.end());
    if (signs_.size() != ns) touch(K_SIGN);
    if (cmds_.size() != nc) touch(K_CMD);
    reindex();
    return (int)((ns - signs_.size()) + (nc - cmds_.size()));
}

WorldTrailer::Result WorldTrailer::adoptSidecar(const uint8_t* p, size_t n) {
    if (!parsed_) return TR_OPAQUE;
    if (!p || n < 12) return TR_BAD;
    const bool sign = is(p, "SGN1"), cmd = is(p, "CMB1");
    if (!sign && !cmd) return TR_BAD;
    const size_t rs = sign ? sizeof(TrailerSign) : sizeof(TrailerCmd);
    const uint32_t version = rd32(p + 4), count = rd32(p + 8);
    if ((uint64_t)n != 12 + (uint64_t)count * rs) return TR_BAD;
    const int kind = sign ? K_SIGN : K_CMD;
    Section* have = section(kind, false);
    if (have && have->present && (sign ? signs_.size() : cmds_.size())) return TR_EXISTS;
    if (!count) return TR_OK;
    const std::vector<Section> before = sections_;
    const bool wasDirty = dirty_;
    if (sign) { signs_.resize(count); std::memcpy(&signs_[0], p + 12, (size_t)count * rs); }
    else { cmds_.resize(count); std::memcpy(&cmds_[0], p + 12, (size_t)count * rs); }
    touch(kind);
    section(kind, false)->version = version;
    if (!fits()) {
        if (sign) signs_.clear(); else cmds_.clear();
        sections_ = before; dirty_ = wasDirty;
        reindex();
        return TR_FULL;
    }
    reindex();
    return TR_OK;
}

std::string WorldTrailer::describe() const {
    std::string secs;
    for (const Section& s : sections_) {
        if (!secs.empty()) secs += ",";
        secs.append((const char*)s.magic, 4);
        if (s.kind != K_RAW && !s.present) secs += "(gone)";
    }
    char b[512];
    std::snprintf(b, sizeof(b), "{\"parsed\":%d,\"dirty\":%d,\"bytes\":%zu,\"signs\":%zu,\"cmds\":%zu,\"sections\":\"%s\",\"why\":\"%s\"}",
                  parsed_ ? 1 : 0, dirty_ ? 1 : 0, encodedSize(), signs_.size(), cmds_.size(), secs.c_str(), why_.c_str());
    return b;
}
