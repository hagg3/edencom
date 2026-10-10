//
//  EdenWorldSource.cpp — see EdenWorldSource.h
//
#include "EdenWorldSource.h"
#include "EdenWorldStore.h"   // emod::io: UTF-8 paths, 64-bit offsets (a > 2 GB archive on Windows)

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include "EdenWorldExport.h"   // emod::free_bytes
#include <cctype>
#include <cstring>
#include <vector>

namespace emod {

static unsigned u16le(const unsigned char* p) { return p[0] | (p[1] << 8); }
static uint32_t u32le(const unsigned char* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static std::string lower(std::string s) {
    for (size_t i = 0; i < s.size(); i++) s[i] = (char)std::tolower((unsigned char)s[i]);
    return s;
}
static bool endsWith(const std::string& s, const char* suffix) {
    const size_t n = std::strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

WorldSource::WorldSource() : in_(NULL), kind_(K_RAW), stored_(false), zon_(false), eof_(false), inLeft_(0), inTotal_(0) {
    std::memset(&zs_, 0, sizeof(zs_));
}
WorldSource::~WorldSource() { close(); }

bool WorldSource::isArchiveName(const std::string& name) {
    const std::string l = lower(name);
    return endsWith(l, ".gz") || endsWith(l, ".zip");
}

std::string WorldSource::stemOf(const std::string& name) {
    std::string n = name;
    const std::string l = lower(n);
    if (endsWith(l, ".gz")) n.resize(n.size() - 3);
    else if (endsWith(l, ".zip")) n.resize(n.size() - 4);
    if (endsWith(lower(n), ".eden")) n.resize(n.size() - 5);
    else if (n == name) {                       // a bare "w.ext" with an unknown extension: drop it
        const size_t dot = n.rfind('.');
        if (dot != std::string::npos && dot > 0) n.resize(dot);
    }
    return n;
}

void WorldSource::close() {
    if (zon_) { inflateEnd(&zs_); zon_ = false; }
    if (in_) { std::fclose(in_); in_ = NULL; }
}

bool WorldSource::open(const std::string& path) {
    close();
    error_.clear();
    eof_ = false;
    in_ = io::fopen_utf8(path.c_str(), "rb");
    if (!in_) return fail("could not read the file");
    const long long size = (long long)io::file_size(in_);
    io::seek_to(in_, 0);
    unsigned char magic[4] = {0, 0, 0, 0};
    const size_t got = std::fread(magic, 1, 4, in_);
    uint64_t dataStart = 0, compSize = (uint64_t)size;
    bool stored = false;
    if (got >= 2 && magic[0] == 0x1f && magic[1] == 0x8b) {
        kind_ = K_GZIP;
    } else if (got == 4 && u32le(magic) == 0x04034b50) {
        kind_ = K_ZIP;
        if (!zipEntry(&dataStart, &compSize, &stored)) return false;
    } else {
        kind_ = K_RAW;
        stored = true;
    }
    io::seek_to(in_, dataStart);
    inLeft_ = inTotal_ = compSize;
    stored_ = stored;
    if (!stored_) {
        std::memset(&zs_, 0, sizeof(zs_));
        // 15+32: zlib detects the gzip header itself. -15: a zip entry is a raw deflate stream.
        if (inflateInit2(&zs_, kind_ == K_GZIP ? 15 + 32 : -15) != Z_OK) return fail("could not start unpacking");
        zon_ = true;
    }
    return true;
}

size_t WorldSource::read(void* dstv, size_t n) {
    unsigned char* dst = (unsigned char*)dstv;
    size_t made = 0;
    if (!in_ || failed()) return 0;
    while (made < n && !eof_) {
        if (stored_) {
            if (inLeft_ == 0) { eof_ = true; break; }
            const size_t want = (size_t)std::min<uint64_t>(n - made, inLeft_);
            const size_t got = std::fread(dst + made, 1, want, in_);
            if (got == 0) { fail("the file is truncated"); break; }
            made += got;
            inLeft_ -= got;
            continue;
        }
        if (zs_.avail_in == 0 && inLeft_ > 0) {
            const size_t want = (size_t)std::min<uint64_t>(sizeof(hold_), inLeft_);
            const size_t got = std::fread(hold_, 1, want, in_);
            if (got == 0) { fail("the file is truncated"); break; }
            inLeft_ -= got;
            zs_.next_in = hold_;
            zs_.avail_in = (uInt)got;
        }
        zs_.next_out = dst + made;
        zs_.avail_out = (uInt)std::min<size_t>(n - made, (size_t)1 << 30);
        const size_t before = zs_.avail_out;
        const int rc = inflate(&zs_, Z_NO_FLUSH);
        made += before - zs_.avail_out;
        if (rc == Z_STREAM_END) {
            // A gzip file may be several members back to back; anything after a zip entry's stream
            // is the next record, not data.
            if (kind_ == K_GZIP && (zs_.avail_in > 0 || inLeft_ > 0) && nextIsGzip()) { inflateReset(&zs_); continue; }
            inLeft_ = 0;
            eof_ = true;
            break;
        }
        if (rc == Z_BUF_ERROR) {
            if (zs_.avail_in == 0 && inLeft_ == 0) { fail("the file is truncated"); break; }
            continue;
        }
        if (rc != Z_OK) { fail("the file is damaged"); break; }
    }
    return made;
}

bool WorldSource::nextIsGzip() {
    if (zs_.avail_in < 2) {
        unsigned char tmp[2];
        size_t have = zs_.avail_in;
        if (have) std::memcpy(tmp, zs_.next_in, have);
        const size_t got = std::fread(tmp + have, 1, 2 - have, in_);
        inLeft_ -= got;
        have += got;
        std::memcpy(hold_, tmp, have);
        zs_.next_in = hold_;
        zs_.avail_in = (uInt)have;
        if (have < 2) return false;
    }
    return zs_.next_in[0] == 0x1f && zs_.next_in[1] == 0x8b;
}

// The entry to extract: the first `.eden` in the central directory, else the first file. The central
// directory's sizes are authoritative -- the local header's are legitimately zero when the archiver
// streamed (general-purpose bit 3).
bool WorldSource::zipEntry(uint64_t* dataStart, uint64_t* compSize, bool* stored) {
    const long long size = (long long)io::file_size(in_);
    const long long tailLen = std::min<long long>(size, 65557);
    std::vector<unsigned char> tail((size_t)tailLen);
    io::seek_to(in_, (uint64_t)(size - tailLen));
    if (std::fread(tail.data(), 1, tail.size(), in_) != tail.size()) return fail("could not read the file");
    long long eocd = -1;
    for (long long i = tailLen - 22; i >= 0; i--)
        if (u32le(&tail[(size_t)i]) == 0x06054b50) { eocd = i; break; }
    if (eocd < 0) return fail("not a zip archive");
    const unsigned entries = u16le(&tail[(size_t)eocd + 10]);
    const unsigned cdSize = u32le(&tail[(size_t)eocd + 12]);
    const unsigned cdOff = u32le(&tail[(size_t)eocd + 16]);
    if (cdOff == 0xFFFFFFFFu || (long long)cdOff + cdSize > size) return fail("this zip format is not supported (zip64)");
    std::vector<unsigned char> cd(cdSize);
    io::seek_to(in_, cdOff);
    if (std::fread(cd.data(), 1, cd.size(), in_) != cd.size()) return fail("could not read the file");
    long long pickOff = -1, pickSize = 0;
    unsigned pickMethod = 0;
    size_t at = 0;
    for (unsigned k = 0; k < entries && at + 46 <= cd.size(); k++) {
        if (u32le(&cd[at]) != 0x02014b50) break;
        const unsigned method = u16le(&cd[at + 10]);
        const unsigned csize = u32le(&cd[at + 20]);
        const unsigned nlen = u16le(&cd[at + 28]), xlen = u16le(&cd[at + 30]), clen = u16le(&cd[at + 32]);
        const unsigned loff = u32le(&cd[at + 42]);
        const std::string name(cd.begin() + (long)(at + 46), cd.begin() + (long)std::min(cd.size(), at + 46 + nlen));
        const bool isDir = !name.empty() && name[name.size() - 1] == '/';
        const bool junk = name.compare(0, 9, "__MACOSX/") == 0;
        if (!isDir && !junk) {
            const bool isEden = endsWith(lower(name), ".eden");
            if (pickOff < 0 || isEden) { pickOff = loff; pickSize = csize; pickMethod = method; }
            if (isEden) break;
        }
        at += 46 + nlen + xlen + clen;
    }
    if (pickOff < 0) return fail("the zip archive is empty");
    if (pickSize == 0xFFFFFFFFLL || pickOff == 0xFFFFFFFFLL) return fail("this zip format is not supported (zip64)");
    if (pickMethod != 0 && pickMethod != 8) return fail("this zip compression is not supported");
    unsigned char lh[30];
    io::seek_to(in_, (uint64_t)pickOff);
    if (std::fread(lh, 1, 30, in_) != 30 || u32le(lh) != 0x04034b50) return fail("the zip archive is damaged");
    *dataStart = (uint64_t)pickOff + 30 + u16le(lh + 26) + u16le(lh + 28);
    *compSize = (uint64_t)pickSize;
    if (*dataStart + *compSize > (uint64_t)size) return fail("the file is truncated");
    *stored = (pickMethod == 0);
    return true;
}


// =============================================================================================
// ImportJob
// =============================================================================================
uint64_t ImportJob::tempCap() {
    if (const char* e = std::getenv("EDEN_TEST_TEMP_CAP")) if (*e) return std::strtoull(e, nullptr, 10);
    return 500ull << 20;
}

uint64_t ImportJob::needFor(uint64_t srcBytes, bool archive) {
    uint64_t need = archive ? (uint64_t)ARCHIVE_FACTOR * srcBytes : 2ull * (srcBytes / 32768ull) * 900ull;
    need += 64ull << 20;
    return std::min<uint64_t>(need, tempCap() + (64ull << 20));
}

ImportJob::ImportJob() : created_(0), upgrade_(false), layerF_(NULL), decide_(true), layering_(false), started_(false), lowSpace_(false),
                         depth_(0), need_(0), fed_(0), nextCheck_(0), layerBytes_(0), peakTemp_(0) {}
ImportJob::~ImportJob() { abort(); }

void ImportJob::abort() {
    in_.close();
    conv_.abort();
    if (layerF_) { std::fclose(layerF_); layerF_ = NULL; }
    if (!layer_.empty()) io::remove_file(layer_);
    if (!prevLayer_.empty()) io::remove_file(prevLayer_);
    layer_.clear(); prevLayer_.clear();
}

bool ImportJob::fail(const std::string& e, bool space) {
    if (error_.empty()) error_ = e;
    lowSpace_ = lowSpace_ || space;
    abort();
    return false;
}

bool ImportJob::begin(const std::string& srcPath, const std::string& outEmod, const std::string& sourceName, int64_t createdUnix,
                      bool upgradeTo256z) {
    abort();
    error_.clear(); lowSpace_ = false;
    src_ = srcPath; out_ = outEmod; name_ = sourceName; created_ = createdUnix; upgrade_ = upgradeTo256z;
    size_t sl = out_.find_last_of("/\\");
    dir_ = sl == std::string::npos ? "." : out_.substr(0, sl);
    decide_ = true; layering_ = false; started_ = false; depth_ = 0;
    fed_ = 0; nextCheck_ = 8ull << 20; layerBytes_ = 0; peakTemp_ = 0;
    if (!in_.open(srcPath)) return fail(in_.error());
    need_ = needFor(in_.inTotal(), in_.kind() != WorldSource::K_RAW);
    if (free_bytes(dir_) < need_) {
        char b[96];
        std::snprintf(b, sizeof b, "not enough free space (needs about %llu MB)", (unsigned long long)(need_ >> 20) + 1);
        return fail(b, true);
    }
    buf_.resize(1 << 20);
    return true;
}

int ImportJob::percent() const {
    // Progress through the INPUT of the current layer: the only honest measure (the inflated size is
    // unknown up front). A nested archive restarts it for its inner layer.
    return in_.inTotal() ? (int)(99.0 * (double)in_.inConsumed() / (double)in_.inTotal()) : 0;
}

int ImportJob::step(int budgetMs) {
    if (!error_.empty()) return FAILED;
    auto t0 = std::chrono::steady_clock::now();
    const uint64_t cap = tempCap();
    for (;;) {
        size_t got = in_.read(buf_.data(), buf_.size());
        if (in_.failed()) { fail(in_.error()); return FAILED; }
        if (got && decide_) {
            decide_ = false;
            const uint8_t* m = buf_.data();
            bool nested = got >= 4 && ((m[0] == 0x1f && m[1] == 0x8b) || (m[0] == 'P' && m[1] == 'K' && m[2] == 3 && m[3] == 4));
            if (nested) {
                if (++depth_ > 4) { fail("the archive is nested too deeply"); return FAILED; }
                layering_ = true;
                layer_ = out_ + ".layer" + std::to_string(depth_);
                layerF_ = io::fopen_utf8(layer_.c_str(), "wb");
                if (!layerF_) { fail("cannot create " + layer_); return FAILED; }
                layerBytes_ = 0;
            } else {
                layering_ = false;
                EdenEmodConverter::Options o;
                o.createdUnix = created_;
                o.sourceName = name_;
                o.maxTempBytes = cap;
                o.upgradeTo256z = upgrade_;
                if (!conv_.begin(out_.c_str(), o)) { fail(conv_.error()); return FAILED; }
                started_ = true;
            }
        }
        if (got) {
            if (layering_) {
                if (std::fwrite(buf_.data(), 1, got, layerF_) != got) { fail("write failed (disk full?)", true); return FAILED; }
                layerBytes_ += got;
                if (layerBytes_ > cap) { fail("the world is too big to convert (over the temp limit)", true); return FAILED; }
            } else if (!conv_.feed(buf_.data(), got)) {
                fail(conv_.error(), conv_.err() == EdenEmodConverter::E_CAP || conv_.err() == EdenEmodConverter::E_IO);
                return FAILED;
            }
            fed_ += got;
            const EdenEmodConverter::Stats& s = conv_.stats();
            peakTemp_ = std::max<uint64_t>(peakTemp_, s.spillBytes + s.outBytes + layerBytes_);
            if (fed_ >= nextCheck_) {                      // ~256 64z columns
                nextCheck_ = fed_ + (8ull << 20);
                if (free_bytes(dir_) < FREE_FLOOR) { fail("stopped: the disk is nearly full", true); return FAILED; }
            }
        }
        if (in_.eof()) {
            if (decide_) { fail("the file is empty"); return FAILED; }
            if (layering_) {
                if (std::fclose(layerF_) != 0) { layerF_ = NULL; fail("write failed (disk full?)", true); return FAILED; }
                layerF_ = NULL;
                in_.close();                                // before the remove: Windows will not delete an open file
                if (!prevLayer_.empty()) io::remove_file(prevLayer_);
                prevLayer_ = layer_;
                layer_.clear();
                if (!in_.open(prevLayer_)) { fail(in_.error()); return FAILED; }
                decide_ = true;
                continue;
            }
            if (!conv_.finish()) {
                fail(conv_.error(), conv_.err() == EdenEmodConverter::E_CAP || conv_.err() == EdenEmodConverter::E_IO);
                return FAILED;
            }
            const EdenEmodConverter::Stats& s = conv_.stats();
            peakTemp_ = std::max<uint64_t>(peakTemp_, s.spillBytes + s.outBytes + layerBytes_);
            in_.close();
            if (!prevLayer_.empty()) { io::remove_file(prevLayer_); prevLayer_.clear(); }
            return DONE;
        }
        if (std::chrono::steady_clock::now() - t0 > std::chrono::milliseconds(budgetMs)) return RUNNING;
    }
}

}  // namespace emod
