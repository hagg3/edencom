//
//  EdenWorldStore.cpp
//  Eden — Stage S / S.3. See EdenWorldStore.h; the format is docs/emod-file-format.md, and every
//  rule below cites the section it implements. The structure deliberately follows
//  web/tools/emod.py (Layout / StreamConverter / Store._scan / Store._evaluate / Store.verify)
//  so a reader can hold the two side by side; the code is not shared.
//
#define ZSTD_STATIC_LINKING_ONLY     // ZSTD_getFrameHeader: the strict single-frame checks
#include "zstd/zstd.h"               // the PINNED 1.5.7 next to this file, never a system copy
#include "EdenWorldStore.h"

#include <zlib.h>
#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstring>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <io.h>
#else
#  include <fcntl.h>
#  include <unistd.h>
#endif

namespace emod {

// =============================================================================================
// small helpers
// =============================================================================================
namespace {

const uint8_t kMagic[8] = {'E', 'M', 'O', 'D', 'W', 'L', 'D', 0};
const uint8_t kTag[4] = {'E', 'm', 'R', '1'};
const uint32_t COMMIT_LEN = 24, SUMMARY_LEN = 16;

inline uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
inline uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
inline uint64_t rd64(const uint8_t* p) { return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32); }
inline void wr16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
inline void wr32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
inline void wr64(uint8_t* p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }
inline void put32(std::vector<uint8_t>& v, uint32_t x) { uint8_t b[4]; wr32(b, x); v.insert(v.end(), b, b + 4); }
inline void put64(std::vector<uint8_t>& v, uint64_t x) { uint8_t b[8]; wr64(b, x); v.insert(v.end(), b, b + 8); }
inline uint32_t pad8(uint64_t n) { return (uint32_t)((8 - (n & 7)) & 7); }
inline int popcount16(uint16_t m) { int c = 0; while (m) { c += m & 1; m >>= 1; } return c; }
inline uint64_t recSize(uint32_t plen) { return REC_HEADER + (uint64_t)plen + pad8(REC_HEADER + (uint64_t)plen); }
inline bool addressable(int32_t x, int32_t z) {    // FileManager's twoToOne gate; key 0 is invalid
    return x >= 0 && x < 32768 && z >= 0 && z < 32768 && ((((int64_t)x) << 15) + z) != 0;
}

std::string fmt(const char* f, ...) __attribute__((format(printf, 1, 2)));
std::string fmt(const char* f, ...) {
    char b[512];
    va_list ap;
    va_start(ap, f);
    vsnprintf(b, sizeof b, f, ap);
    va_end(ap);
    return b;
}

FILE* fopen_utf8(const char* path, const char* mode) {
#if defined(_WIN32)
    wchar_t wp[1024], wm[16];
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, wp, 1024)) return nullptr;
    if (!MultiByteToWideChar(CP_UTF8, 0, mode, -1, wm, 16)) return nullptr;
    return _wfopen(wp, wm);
#else
    return std::fopen(path, mode);
#endif
}

bool seek_to(FILE* f, uint64_t off) {
#if defined(_WIN32)
    return _fseeki64(f, (long long)off, SEEK_SET) == 0;
#else
    return fseeko(f, (off_t)off, SEEK_SET) == 0;
#endif
}

uint64_t file_size(FILE* f) {
#if defined(_WIN32)
    if (_fseeki64(f, 0, SEEK_END) != 0) return 0;
    return (uint64_t)_ftelli64(f);
#else
    if (fseeko(f, 0, SEEK_END) != 0) return 0;
    return (uint64_t)ftello(f);
#endif
}

bool truncate_file(FILE* f, uint64_t n) {
    if (std::fflush(f) != 0) return false;
#if defined(_WIN32)
    return _chsize_s(_fileno(f), (long long)n) == 0;
#else
    return ftruncate(fileno(f), (off_t)n) == 0;
#endif
}

// The spec's fsync: plain fsync (FlushFileBuffers on Windows). On web this is MEMFS's no-op; the
// ordered OPFS mirror is the durability there (plan §1).
bool sync_file(FILE* f) {
    if (std::fflush(f) != 0) return false;
#if defined(_WIN32)
    return _commit(_fileno(f)) == 0;
#else
    return fsync(fileno(f)) == 0;
#endif
}

void sync_parent_dir(const std::string& path) {
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
    std::string d = path;
    size_t s = d.find_last_of('/');
    d = (s == std::string::npos) ? "." : (s == 0 ? "/" : d.substr(0, s));
    int fd = ::open(d.c_str(), O_RDONLY);
    if (fd >= 0) { (void)fsync(fd); ::close(fd); }
#else
    (void)path;
#endif
}

// Atomic replace. POSIX rename(2) replaces; Windows' does not, so MoveFileEx.
bool replace_file(const std::string& from, const std::string& to) {
#if defined(_WIN32)
    wchar_t wf[1024], wt[1024];
    if (!MultiByteToWideChar(CP_UTF8, 0, from.c_str(), -1, wf, 1024)) return false;
    if (!MultiByteToWideChar(CP_UTF8, 0, to.c_str(), -1, wt, 1024)) return false;
    return MoveFileExW(wf, wt, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    if (std::rename(from.c_str(), to.c_str()) != 0) return false;
    sync_parent_dir(to);
    return true;
#endif
}

void remove_file(const std::string& p) {
#if defined(_WIN32)
    wchar_t w[1024];
    if (MultiByteToWideChar(CP_UTF8, 0, p.c_str(), -1, w, 1024)) DeleteFileW(w);
#else
    std::remove(p.c_str());
#endif
}

// Appends one record (header + payload + zero padding) to `out`; returns its crc32.
uint32_t append_record(std::vector<uint8_t>& out, uint8_t type, uint8_t codec, uint16_t mask, int32_t x,
                       int32_t z, uint32_t aux, uint32_t seq, const uint8_t* data, size_t n) {
    uint8_t h[REC_HEADER];
    std::memcpy(h, kTag, 4);
    h[4] = type;
    h[5] = codec;
    wr16(h + 6, mask);
    wr32(h + 8, (uint32_t)x);
    wr32(h + 12, (uint32_t)z);
    wr32(h + 16, aux);
    wr32(h + 20, (uint32_t)n);
    wr32(h + 24, seq);
    uint32_t c = crc32_of(h, 28);
    c = crc32_of(data, n, c);
    wr32(h + 28, c);
    out.insert(out.end(), h, h + REC_HEADER);
    if (n) out.insert(out.end(), data, data + n);
    out.insert(out.end(), pad8(REC_HEADER + n), 0);
    return c;
}

void file_header(uint8_t out[FILE_HEADER], int bands, uint64_t created) {
    std::memset(out, 0, FILE_HEADER);
    std::memcpy(out, kMagic, 8);
    wr16(out + 8, 1);
    wr16(out + 10, (uint16_t)bands);
    wr32(out + 12, 0);
    wr64(out + 16, created);
    wr32(out + 60, crc32_of(out, 60));
}

void summary_payload(uint8_t out[SUMMARY_LEN], uint32_t live, uint32_t nextOrd, uint16_t bandOr) {
    std::memset(out, 0, SUMMARY_LEN);
    wr32(out, live);
    wr32(out + 4, nextOrd);
    wr16(out + 8, bandOr);
}

void commit_payload(uint8_t out[COMMIT_LEN], uint32_t seq, uint32_t count, uint32_t crcc, uint64_t prev) {
    std::memset(out, 0, COMMIT_LEN);
    wr32(out, seq);
    wr32(out + 4, count);
    wr32(out + 8, crcc);
    wr64(out + 16, prev);
}

bool all_zero(const uint8_t* p, size_t n) {
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t w;
        std::memcpy(&w, p + i, 8);
        if (w) return false;
    }
    for (; i < n; i++) if (p[i]) return false;
    return true;
}

std::string hex(const uint8_t* d, size_t n) {
    static const char* k = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; i++) { s += k[d[i] >> 4]; s += k[d[i] & 15]; }
    return s;
}

}  // namespace

uint32_t crc32_of(const void* p, size_t n, uint32_t crc) {
    // zlib's crc32 takes a uInt length; feed it in pieces so a >4 GiB call can't wrap.
    const Bytef* b = (const Bytef*)p;
    uLong c = crc;
    while (n) {
        uInt k = (uInt)std::min<size_t>(n, 1u << 30);
        c = ::crc32(c, b, k);
        b += k;
        n -= k;
    }
    return (uint32_t)c;
}

// =============================================================================================
// SHA-256 (FIPS 180-4)
// =============================================================================================
namespace {
const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
inline uint32_t ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
void sha_block(uint32_t h[8], const uint8_t* p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) w[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16) | ((uint32_t)p[4 * i + 2] << 8) | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = k + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        uint32_t t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        k = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += k;
}
}  // namespace

Sha256::Sha256() : len(0), fill(0) {
    static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::memcpy(h, iv, sizeof h);
}
void Sha256::update(const void* data, size_t n) {
    const uint8_t* p = (const uint8_t*)data;
    len += n;
    while (n) {
        if (fill == 0 && n >= 64) { sha_block(h, p); p += 64; n -= 64; continue; }
        size_t k = std::min(n, 64 - fill);
        std::memcpy(buf + fill, p, k);
        fill += k; p += k; n -= k;
        if (fill == 64) { sha_block(h, buf); fill = 0; }
    }
}
void Sha256::final(uint8_t out[32]) {
    uint64_t bits = len * 8;
    uint8_t one = 0x80, zero = 0;
    update(&one, 1);
    while (fill != 56) update(&zero, 1);
    uint8_t L[8];
    for (int i = 0; i < 8; i++) L[i] = (uint8_t)(bits >> (56 - 8 * i));
    update(L, 8);
    for (int i = 0; i < 8; i++) { out[4 * i] = (uint8_t)(h[i] >> 24); out[4 * i + 1] = (uint8_t)(h[i] >> 16); out[4 * i + 2] = (uint8_t)(h[i] >> 8); out[4 * i + 3] = (uint8_t)h[i]; }
}

// =============================================================================================
// codecs (docs/emod-file-format.md "Codecs")
// =============================================================================================
Codecs::Codecs() : cctx_(nullptr), dctx_(nullptr) {}
Codecs::~Codecs() {
    if (cctx_) ZSTD_freeCCtx((ZSTD_CCtx*)cctx_);
    if (dctx_) ZSTD_freeDCtx((ZSTD_DCtx*)dctx_);
}

bool Codecs::encode(int codec, const uint8_t* p, size_t n, bool compress, uint8_t* outCodec, std::vector<uint8_t>& out) {
    out.clear();
    if (!n || !compress || codec == C_NONE) {
        *outCodec = C_NONE;
        out.assign(p, p + n);
        return true;
    }
    if (codec == C_ZSTD) {
        if (!cctx_) cctx_ = ZSTD_createCCtx();
        ZSTD_CCtx* c = (ZSTD_CCtx*)cctx_;
        if (!c) return false;
        ZSTD_CCtx_reset(c, ZSTD_reset_session_and_parameters);
        ZSTD_CCtx_setParameter(c, ZSTD_c_compressionLevel, 3);
        ZSTD_CCtx_setParameter(c, ZSTD_c_checksumFlag, 1);
        ZSTD_CCtx_setParameter(c, ZSTD_c_contentSizeFlag, 1);
        ZSTD_CCtx_setParameter(c, ZSTD_c_dictIDFlag, 0);
        out.resize(ZSTD_compressBound(n));
        size_t r = ZSTD_compress2(c, out.data(), out.size(), p, n);
        if (ZSTD_isError(r)) return false;
        out.resize(r);
        *outCodec = C_ZSTD;
        return true;
    }
    if (codec == C_ZLIB) {
        uLongf len = compressBound((uLong)n);
        out.resize(len);
        if (compress2(out.data(), &len, p, (uLong)n, 6) != Z_OK) return false;
        out.resize(len);
        *outCodec = C_ZLIB;
        return true;
    }
    return false;
}

bool Codecs::decode(int codec, const uint8_t* p, size_t n, size_t expect, uint8_t* out) {
    if (codec == C_NONE) {
        if (n != expect) return false;
        if (n) std::memcpy(out, p, n);
        return true;
    }
    if (expect == 0) return false;               // a compressed record with an empty decoded size
    if (codec == C_ZSTD) {
        ZSTD_FrameHeader zfh;
        if (ZSTD_getFrameHeader(&zfh, p, n) != 0) return false;
        if (zfh.frameType != ZSTD_frame || !zfh.checksumFlag || zfh.dictID != 0) return false;
        if (zfh.frameContentSize != (unsigned long long)expect) return false;
        size_t fs = ZSTD_findFrameCompressedSize(p, n);
        if (ZSTD_isError(fs) || fs != n) return false;     // exactly one frame, no trailing bytes
        if (!dctx_) dctx_ = ZSTD_createDCtx();
        if (!dctx_) return false;
        size_t r = ZSTD_decompressDCtx((ZSTD_DCtx*)dctx_, out, expect, p, n);
        return !ZSTD_isError(r) && r == expect;
    }
    if (codec == C_ZLIB) {
        z_stream s;
        std::memset(&s, 0, sizeof s);
        if (inflateInit(&s) != Z_OK) return false;
        s.next_in = (Bytef*)p;
        s.avail_in = (uInt)n;
        s.next_out = out;
        s.avail_out = (uInt)expect;
        int r = inflate(&s, Z_FINISH);
        bool ok = r == Z_STREAM_END && s.avail_in == 0 && s.total_out == expect;
        inflateEnd(&s);
        return ok;
    }
    return false;                                  // reserved codec
}

uint16_t split_bands(const uint8_t* col, int bands, std::vector<uint8_t>& payload) {
    payload.clear();
    uint16_t mask = 0;
    for (int b = 0; b < bands; b++) {
        const uint8_t* seg = col + (size_t)b * BAND;
        if (!all_zero(seg, BAND)) {
            mask |= (uint16_t)(1u << b);
            payload.insert(payload.end(), seg, seg + BAND);
        }
    }
    return mask;
}

void join_bands(uint16_t mask, const uint8_t* payload, int bands, uint8_t* col) {
    size_t p = 0;
    for (int b = 0; b < bands; b++) {
        uint8_t* dst = col + (size_t)b * BAND;
        if (mask >> b & 1) { std::memcpy(dst, payload + p, BAND); p += BAND; }
        else std::memset(dst, 0, BAND);
    }
}

// ---- a `.eden`'s height (S.3b; see the header) ----------------------------------------------
int eden_detect_bands(int32_t version, uint64_t dirOff, const uint64_t* offsets, size_t n) {
    if (version >= 5) return 16;
    if (!n) return 4;
    uint64_t mo = 0;
    for (size_t i = 0; i < n; i++) mo = std::max(mo, offsets[i]);
    static const int kBands[2] = {16, 4};
    for (int b : kBands) {
        uint64_t stride = (uint64_t)b * BAND;
        if (mo > dirOff || dirOff - mo < stride) continue;            // overflow-safe dir >= max + stride
        uint64_t gap = dirOff - mo - stride;
        if (gap % EDEN_ENT == 0 && gap <= (uint64_t)EDEN_ENT * EDEN_MAX_SLOTS) return b;
    }
    // Neither stride leaves a valid creature gap (the two candidates' gaps differ by 98,304 B, so
    // both can never pass): the min-offset-gap fallback, which needs two distinct offsets.
    std::vector<uint64_t> s(offsets, offsets + n);
    std::sort(s.begin(), s.end());
    uint64_t minGap = 0;
    for (size_t i = 1; i < s.size(); i++)
        if (s[i] != s[i - 1] && (!minGap || s[i] - s[i - 1] < minGap)) minGap = s[i] - s[i - 1];
    return minGap >= 16ull * BAND ? 16 : 4;
}

int eden_detect_bands_raw(const uint8_t header[EDEN_HEADER], const uint8_t* dir, size_t dirLen) {
    int32_t version = (int32_t)rd32(header + 92);
    if (version >= 5) return 16;
    std::unordered_map<uint64_t, uint64_t> live;                       // last row wins
    for (size_t i = 0; i + EDEN_DIR_ROW <= dirLen; i += EDEN_DIR_ROW) {
        int32_t x = (int32_t)rd32(dir + i), z = (int32_t)rd32(dir + i + 4);
        if (addressable(x, z)) live[((uint64_t)(uint32_t)x << 32) | (uint32_t)z] = rd64(dir + i + 8);
    }
    std::vector<uint64_t> offsets;
    offsets.reserve(live.size());
    for (const auto& kv : live) offsets.push_back(kv.second);
    return eden_detect_bands(version, rd64(header + 32), offsets.data(), offsets.size());
}

// =============================================================================================
// EdenWorldStore — opening (docs/emod-file-format.md "Reading (normative)")
// =============================================================================================
EdenWorldStore::EdenWorldStore()
    : testCrashAfter((size_t)-1), testCrashZeroFill(false), f_(nullptr), writable_(false), checkCrc_(true),
      creating_(false), bands_(0), created_(0), size_(0) {
    close();
}

EdenWorldStore::~EdenWorldStore() { close(); }

void EdenWorldStore::close() {
    if (f_) std::fclose(f_);
    f_ = nullptr;
    path_.clear(); finalPath_.clear();
    writable_ = false; creating_ = false;
    bands_ = 0; created_ = 0; size_ = 0;
    recs_.clear(); spans_.clear(); entries_.clear(); colIndex_.clear();
    for (int i = 0; i < 256; i++) latestT_[i] = -1;
    damage_.clear(); warnings_.clear(); errors_.clear();
    torn_ = false;
    tailBytes_ = 0; truncateAt_ = FILE_HEADER; lastCommitOff_ = 0; lastSeq_ = 0;
    maxOrd_ = 0; anyOrd_ = false;
    std::memset(bandCount_, 0, sizeof bandCount_);
    liveRecordBytes_ = 0;
    openMs_ = 0;
    codec_ = C_ZSTD;
    batch_.clear(); batchRecs_.clear();
}

bool EdenWorldStore::readAt(uint64_t off, void* dst, size_t n) {
    if (!n) return true;
    if (!seek_to(f_, off)) return false;
    return std::fread(dst, 1, n, f_) == n;
}

bool EdenWorldStore::open(const char* path, bool writable, bool checkCrc) {
    close();
    error_.clear();
    auto t0 = std::chrono::steady_clock::now();
    path_ = path;
    writable_ = writable;
    checkCrc_ = checkCrc;
    f_ = fopen_utf8(path, writable ? "r+b" : "rb");
    if (!f_) return fail(fmt("cannot open %s", path));
    size_ = file_size(f_);
    // 1. File header
    uint8_t h[FILE_HEADER];
    if (size_ < FILE_HEADER || !readAt(0, h, FILE_HEADER)) { close(); return fail("file shorter than the 64-byte file header"); }
    if (std::memcmp(h, kMagic, 8) != 0) { close(); return fail("bad magic"); }
    if (checkCrc_ && crc32_of(h, 60) != rd32(h + 60)) { close(); return fail("file header CRC mismatch"); }
    if (rd16(h + 8) != 1) { close(); return fail(fmt("fmt_version %u (this reader knows 1)", rd16(h + 8))); }
    bands_ = rd16(h + 10);
    if (bands_ != 4 && bands_ != 16) { close(); return fail(fmt("bands %d is neither 4 nor 16", bands_)); }
    if (rd32(h + 12) != 0 || !all_zero(h + 24, 36)) { close(); return fail("non-zero flags/reserved in the file header (written by a newer build)"); }
    created_ = rd64(h + 16);
    // 2-6. Scan, commits, apply, tail, truncation point
    scan();
    evaluate();
    recs_.clear(); recs_.shrink_to_fit();
    spans_.clear(); spans_.shrink_to_fit();
    // 7. No valid commit: no world
    if (!errors_.empty()) {
        std::string e = errors_[0];
        close();
        return fail(e);
    }
    openMs_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return true;
}

bool EdenWorldStore::create(const char* path, int bands, uint64_t createdUnix) {
    close();
    error_.clear();
    if (bands != 4 && bands != 16) return fail("bands must be 4 or 16");
    finalPath_ = path;
    path_ = std::string(path) + ".creating";
    f_ = fopen_utf8(path_.c_str(), "w+b");
    if (!f_) return fail(fmt("cannot create %s", path_.c_str()));
    uint8_t h[FILE_HEADER];
    file_header(h, bands, createdUnix);
    if (std::fwrite(h, 1, FILE_HEADER, f_) != FILE_HEADER || std::fflush(f_) != 0) {
        close();
        remove_file(std::string(path) + ".creating");
        return fail("cannot write the file header");
    }
    writable_ = true; creating_ = true; checkCrc_ = true;
    bands_ = bands; created_ = createdUnix; size_ = FILE_HEADER;
    return true;
}

// Framing (step 2): tag, payload_len <= 16 MiB, the record + padding inside the file, CRC.
bool EdenWorldStore::frame(uint64_t p, Rec* r, std::vector<uint8_t>& buf) {
    if (p + REC_HEADER > size_) return false;
    uint8_t h[REC_HEADER];
    if (!readAt(p, h, REC_HEADER) || std::memcmp(h, kTag, 4) != 0) return false;
    uint32_t plen = rd32(h + 20);
    if (plen > MAX_PAYLOAD) return false;
    uint64_t end = p + recSize(plen);
    if (end > size_) return false;
    uint32_t padn = pad8(REC_HEADER + (uint64_t)plen);
    buf.resize((size_t)plen + padn);
    if (!readAt(p + REC_HEADER, buf.data(), buf.size())) return false;
    uint32_t crc = rd32(h + 28);
    if (checkCrc_) {
        uint32_t c = crc32_of(h, 28);
        c = crc32_of(buf.data(), plen, c);
        if (c != crc) return false;
    }
    r->off = p; r->end = end;
    r->type = h[4]; r->codec = h[5]; r->mask = rd16(h + 6);
    r->x = (int32_t)rd32(h + 8); r->z = (int32_t)rd32(h + 12);
    r->aux = rd32(h + 16); r->plen = plen; r->seq = rd32(h + 24); r->crc = crc;
    r->padOk = all_zero(buf.data() + plen, padn);
    return true;
}

void EdenWorldStore::scan() {
    std::vector<uint8_t>& buf = scratch_;
    std::vector<uint8_t> win;
    uint64_t p = FILE_HEADER;
    while (p < size_) {
        Rec r;
        if (frame(p, &r, buf)) {
            recs_.push_back(r);
            p = r.end;
            continue;
        }
        // resync: the next 8-aligned offset at which a record frames, or EOF
        uint64_t q = p + 8, found = size_;
        const size_t W = 1 << 16;
        while (q + 4 <= size_ && found == size_) {
            size_t n = (size_t)std::min<uint64_t>(W, size_ - q);
            win.resize(n);
            if (!readAt(q, win.data(), n)) break;
            size_t i = 0;
            for (; i + 4 <= n; i += 8) {
                if (std::memcmp(win.data() + i, kTag, 4) == 0) {
                    Rec t;
                    if (frame(q + i, &t, buf)) { found = q + i; break; }
                }
            }
            if (found == size_) q += i;
        }
        spans_.push_back(std::make_pair(p, found));
        p = found;
    }
}

namespace {
struct CommitF { uint32_t seq, count, crcc; uint64_t prev; };
}

// Steps 3-6, exactly as emod.py Store._evaluate (which is the spec's reading rules in code).
void EdenWorldStore::evaluate() {
    struct C { size_t idx; CommitF f; };
    std::vector<C> commits;
    uint8_t cp[COMMIT_LEN];
    for (size_t i = 0; i < recs_.size(); i++) {
        const Rec& r = recs_[i];
        if (r.type != T_COMMIT || r.codec != C_NONE || r.plen != COMMIT_LEN || r.aux != COMMIT_LEN || r.mask || r.x || r.z) continue;
        if (!readAt(r.off + REC_HEADER, cp, COMMIT_LEN)) continue;
        CommitF f = {rd32(cp), rd32(cp + 4), rd32(cp + 8), rd64(cp + 16)};
        if (f.seq != r.seq || rd32(cp + 12) != 0) continue;
        commits.push_back(C{i, f});
    }
    bool havePrev = false;
    uint64_t prevOff = 0, prevEnd = FILE_HEADER;
    uint32_t prevSeq = 0;
    size_t ri = 0, si = 0;
    std::vector<size_t> batch, orph;
    for (size_t k = 0; k < commits.size(); k++) {
        const Rec& c = recs_[commits[k].idx];
        const CommitF& f = commits[k].f;
        bool last = k + 1 == commits.size();
        batch.clear(); orph.clear();
        while (ri < recs_.size() && recs_[ri].off < prevEnd) ri++;
        for (; ri < recs_.size() && recs_[ri].off < c.off; ri++) {
            if (recs_[ri].seq == f.seq) batch.push_back(ri); else orph.push_back(ri);
        }
        size_t nspans = 0;
        while (si < spans_.size() && spans_[si].first < prevEnd) si++;
        for (; si < spans_.size() && spans_[si].first < c.off; si++) nspans++;
        bool prevOk = f.prev == (havePrev ? prevOff : 0) ||
                      (prevEnd <= f.prev && f.prev < c.off && (!orph.empty() || nspans));
        bool ok = batch.size() == f.count && prevOk;
        if (ok && checkCrc_) {
            uint32_t cc = 0;
            for (size_t i : batch) { uint8_t b[4]; wr32(b, recs_[i].crc); cc = crc32_of(b, 4, cc); }
            ok = cc == f.crcc;
        }
        bool gap = havePrev && f.seq != prevSeq + 1;
        if (!orph.empty() || (nspans && ok) || gap)
            damage_.push_back(fmt("batch seq %u (commit @%llu): %zu orphan records, %zu damaged spans%s", f.seq,
                                  (unsigned long long)c.off, orph.size(), nspans, gap ? ", seq gap" : ""));
        if (ok || !last) {
            if (!ok)
                damage_.push_back(fmt("batch seq %u (commit @%llu) fails its commit check; applying its %zu intact records",
                                      f.seq, (unsigned long long)c.off, batch.size()));
            // early orphans + the batch, in file order: both are subsets of the range, already ordered
            size_t a = 0, b = 0;
            while (a < orph.size() || b < batch.size()) {
                size_t pick;
                if (b >= batch.size() || (a < orph.size() && recs_[orph[a]].off < recs_[batch[b]].off)) {
                    pick = orph[a++];
                    if (!(recs_[pick].seq < f.seq)) continue;
                } else {
                    pick = batch[b++];
                }
                apply(recs_[pick]);
            }
            truncateAt_ = c.end;
            lastCommitOff_ = c.off;
            lastSeq_ = f.seq;
        } else {
            torn_ = true;
            uint64_t t = prevEnd;
            size_t present = batch.size();
            for (size_t i : orph) if (recs_[i].seq < f.seq) { apply(recs_[i]); t = std::max(t, recs_[i].end); }
            truncateAt_ = t;
            warnings_.push_back(fmt("last batch seq %u (commit @%llu) is torn: %zu of %u records present -- rolled back",
                                    f.seq, (unsigned long long)c.off, present, f.count));
        }
        havePrev = true;
        prevOff = c.off;
        prevEnd = c.end;
        prevSeq = f.seq;
    }
    uint64_t end = commits.empty() ? FILE_HEADER : recs_[commits.back().idx].end;
    tailBytes_ = size_ - end;
    if (commits.empty()) errors_.push_back("no committed batch");
    for (const Rec& r : recs_) {
        if (!r.padOk && r.off < end) damage_.push_back(fmt("non-zero padding after record @%llu", (unsigned long long)r.off));
        if ((r.type < 1 || r.type > 8) && r.off < truncateAt_)
            warnings_.push_back(fmt("record @%llu has reserved type %u", (unsigned long long)r.off, r.type));
    }
}

void EdenWorldStore::apply(const Rec& r) {
    Entry e;
    e.off = r.off; e.aux = r.aux; e.plen = r.plen; e.x = r.x; e.z = r.z; e.prev = -1;
    e.mask = r.mask; e.type = r.type; e.codec = r.codec;
    int32_t idx = (int32_t)entries_.size();
    if (r.type == T_COLUMN) {
        uint64_t k = colKey(r.x, r.z);
        auto it = colIndex_.find(k);
        if (it != colIndex_.end()) {
            e.prev = it->second;
            liveRecordBytes_ -= recSize(entries_[it->second].plen);
            it->second = idx;
        } else {
            colIndex_[k] = idx;
        }
    } else {
        e.prev = latestT_[r.type];
        if (e.prev >= 0) liveRecordBytes_ -= recSize(entries_[e.prev].plen);
        latestT_[r.type] = idx;
    }
    liveRecordBytes_ += recSize(r.plen);
    entries_.push_back(e);
}

// =============================================================================================
// live view
// =============================================================================================
bool EdenWorldStore::decodeEntry(const Entry& e, std::vector<uint8_t>& out) {
    size_t expect = e.type == T_COLUMN ? (size_t)popcount16(e.mask) * BAND : e.aux;
    if (expect > (size_t)MAX_PAYLOAD * 64) return false;
    scratch2_.resize(e.plen);
    if (!readAt(e.off + REC_HEADER, scratch2_.data(), e.plen)) return false;
    out.resize(expect);
    return codecs_.decode(e.codec, scratch2_.data(), e.plen, expect, out.data());
}

// Newest decodable version along the history chain; -1 when none decodes. `back` = how many
// versions back it had to go.
int EdenWorldStore::decodeNewest(int32_t idx, std::vector<uint8_t>& out, int* back) {
    int n = 0;
    for (int32_t i = idx; i >= 0; i = entries_[i].prev, n++) {
        if (decodeEntry(entries_[i], out)) { *back = n; return i; }
    }
    *back = n;
    return -1;
}

std::vector<std::pair<int32_t, int32_t> > EdenWorldStore::columnKeys() const {
    std::vector<std::pair<uint32_t, std::pair<int32_t, int32_t> > > v;
    v.reserve(colIndex_.size());
    for (const auto& kv : colIndex_) {
        const Entry& e = entries_[kv.second];
        v.push_back(std::make_pair(e.aux, std::make_pair(e.x, e.z)));
    }
    std::sort(v.begin(), v.end());
    std::vector<std::pair<int32_t, int32_t> > out;
    out.reserve(v.size());
    for (const auto& p : v) out.push_back(p.second);
    return out;
}

bool EdenWorldStore::hasColumn(int32_t x, int32_t z) const { return colIndex_.count(colKey(x, z)) != 0; }

uint16_t EdenWorldStore::columnMask(int32_t x, int32_t z) const {
    auto it = colIndex_.find(colKey(x, z));
    return it == colIndex_.end() ? 0 : entries_[it->second].mask;
}

EdenWorldStore::ReadResult EdenWorldStore::readColumn(int32_t x, int32_t z, uint8_t* out, uint16_t* maskOut) {
    const size_t full = (size_t)bands_ * BAND;
    auto it = colIndex_.find(colKey(x, z));
    if (it == colIndex_.end()) {
        std::memset(out, 0, full);
        if (maskOut) *maskOut = 0;
        return READ_ABSENT;
    }
    std::vector<uint8_t> payload;
    int back = 0;
    int i = decodeNewest(it->second, payload, &back);
    if (i < 0) {
        // Spec: air, NOT marked modified, never the default map or the generator. Damage.
        damage_.push_back(fmt("COLUMN (%d,%d) does not decode and has no decodable earlier version: loaded as air", x, z));
        std::memset(out, 0, full);
        if (maskOut) *maskOut = 0;
        return READ_AIR_DAMAGED;
    }
    const Entry& e = entries_[i];
    if (e.mask >> bands_) {     // a damaged mask that still framed; never write past the buffer
        damage_.push_back(fmt("COLUMN (%d,%d) sets band bits >= %d", x, z, bands_));
        std::memset(out, 0, full);
        if (maskOut) *maskOut = 0;
        return READ_AIR_DAMAGED;
    }
    join_bands(e.mask, payload.data(), bands_, out);
    if (maskOut) *maskOut = e.mask;
    if (back) {
        damage_.push_back(fmt("COLUMN (%d,%d) newest version does not decode; used the one %d back", x, z, back));
        return READ_FALLBACK;
    }
    return READ_OK;
}

EdenWorldStore::ReadResult EdenWorldStore::readRecord(int type, std::vector<uint8_t>& out) {
    out.clear();
    if (type <= 0 || type > 255 || type == T_COLUMN || latestT_[type] < 0) return READ_ABSENT;
    int back = 0;
    int i = decodeNewest(latestT_[type], out, &back);
    if (i < 0) {
        out.clear();
        damage_.push_back(fmt("record type %d does not decode in any version", type));
        return READ_AIR_DAMAGED;
    }
    if (back) {
        damage_.push_back(fmt("record type %d newest version does not decode; used the one %d back", type, back));
        return READ_FALLBACK;
    }
    return READ_OK;
}

EdenWorldStore::Summary EdenWorldStore::summary() const {
    Summary s = {0, 0, 0};
    bool any = false;
    uint32_t mx = 0;
    for (const auto& kv : colIndex_) {
        const Entry& e = entries_[kv.second];
        s.bandOr |= e.mask;
        if (!any || e.aux > mx) mx = e.aux;
        any = true;
    }
    s.liveColumns = (uint32_t)colIndex_.size();
    s.nextOrd = any ? mx + 1 : 0;
    return s;
}

uint64_t EdenWorldStore::deadBytes() const {
    // the last COMMIT is live too: it is what makes everything else live
    uint64_t used = FILE_HEADER + liveRecordBytes_ + (lastSeq_ ? recSize(COMMIT_LEN) : 0);
    return truncateAt_ > used ? truncateAt_ - used : 0;
}

bool EdenWorldStore::shouldCompact() const {
    uint64_t d = deadBytes();
    return d > std::max<uint64_t>(truncateAt_ / 2, 4u << 20);
}

// =============================================================================================
// writing
// =============================================================================================
void EdenWorldStore::beginBatch() { abortBatch(); }

void EdenWorldStore::addRecord(uint8_t type, uint8_t codec, uint16_t mask, int32_t x, int32_t z, uint32_t aux,
                               const uint8_t* data, size_t n) {
    Pending p;
    p.rel = batch_.size();
    p.type = type; p.codec = codec; p.mask = mask; p.x = x; p.z = z; p.aux = aux; p.plen = (uint32_t)n;
    p.crc = append_record(batch_, type, codec, mask, x, z, aux, lastSeq_ + 1, data, n);
    batchRecs_.push_back(p);
}

bool EdenWorldStore::putColumn(int32_t x, int32_t z, const uint8_t* column) {
    error_.clear();
    if (!f_ || !writable_) return fail("store is not open for writing");
    if (!addressable(x, z)) return fail(fmt("column key (%d,%d) is not addressable", x, z));
    // ord: an existing column keeps its ord (also when rewritten earlier in this batch); a new
    // one takes next_ord.
    uint32_t ord;
    uint64_t k = colKey(x, z);
    auto it = colIndex_.find(k);
    bool found = false;
    if (it != colIndex_.end()) { ord = entries_[it->second].aux; found = true; }
    else {
        for (const Pending& p : batchRecs_) if (p.type == T_COLUMN && p.x == x && p.z == z) { ord = p.aux; found = true; }
    }
    if (!found) {
        Summary s = summary();
        ord = s.nextOrd;
        for (const Pending& p : batchRecs_) if (p.type == T_COLUMN && p.aux >= ord) ord = p.aux + 1;
    }
    uint16_t mask = split_bands(column, bands_, payloadBuf_);
    uint8_t codec;
    std::vector<uint8_t>& enc = scratch_;
    if (!codecs_.encode(codec_, payloadBuf_.data(), payloadBuf_.size(), true, &codec, enc))
        return fail("encode failed");
    if (enc.size() > MAX_PAYLOAD) return fail("column payload exceeds 16 MiB");
    addRecord(T_COLUMN, codec, mask, x, z, ord, enc.data(), enc.size());
    return true;
}

bool EdenWorldStore::putRecord(int type, const void* data, size_t len) {
    error_.clear();
    if (!f_ || !writable_) return fail("store is not open for writing");
    if (type <= 0 || type > 255 || type == T_COLUMN || type == T_COMMIT || type == T_SUMMARY)
        return fail("putRecord: COLUMN/COMMIT/SUMMARY are written by putColumn/commitBatch");
    if (type == T_WORLD_HEADER && len != EDEN_HEADER) return fail("WORLD_HEADER must be 192 bytes");
    if (type == T_CREATURES && (len % EDEN_ENT || len / EDEN_ENT > EDEN_MAX_SLOTS)) return fail("CREATURES must be 0-400 x 60 bytes");
    if (type == T_SIGN_TRAILER && (len % EDEN_DIR_ROW || len > TRAILER_MAX)) return fail("SIGN_TRAILER must be a multiple of 16, <= 1 MiB");
    uint8_t codec;
    std::vector<uint8_t>& enc = scratch_;
    if (!codecs_.encode(codec_, (const uint8_t*)data, len, type != T_WORLD_HEADER, &codec, enc)) return fail("encode failed");
    if (enc.size() > MAX_PAYLOAD) return fail("record payload exceeds 16 MiB");
    addRecord((uint8_t)type, codec, 0, 0, 0, (uint32_t)len, enc.data(), enc.size());
    return true;
}

bool EdenWorldStore::truncateTo(uint64_t n) { return truncate_file(f_, n); }
bool EdenWorldStore::fsyncFile() { return sync_file(f_); }

bool EdenWorldStore::commitBatch() {
    error_.clear();
    if (!f_ || !writable_) return fail("store is not open for writing");
    if (creating_) {
        bool wh = false, cr = false;
        for (const Pending& p : batchRecs_) { wh |= p.type == T_WORLD_HEADER; cr |= p.type == T_CREATURES; }
        if (!wh || !cr) return fail("a world's first batch must carry WORLD_HEADER and CREATURES");
    }
    // SUMMARY for the live state after this batch (exact; always written, so every batch has one)
    uint32_t live = (uint32_t)colIndex_.size();
    uint16_t bandOr = 0;
    bool any = false;
    uint32_t mx = 0;
    std::unordered_map<uint64_t, std::pair<uint32_t, uint16_t> > over;
    for (const Pending& p : batchRecs_) if (p.type == T_COLUMN) over[colKey(p.x, p.z)] = std::make_pair(p.aux, p.mask);
    for (const auto& kv : colIndex_) {
        if (over.count(kv.first)) continue;
        const Entry& e = entries_[kv.second];
        bandOr |= e.mask;
        if (!any || e.aux > mx) mx = e.aux;
        any = true;
    }
    for (const auto& kv : over) {
        if (!colIndex_.count(kv.first)) live++;
        bandOr |= kv.second.second;
        if (!any || kv.second.first > mx) mx = kv.second.first;
        any = true;
    }
    uint8_t sp[SUMMARY_LEN];
    summary_payload(sp, live, any ? mx + 1 : 0, bandOr);
    addRecord(T_SUMMARY, C_NONE, 0, 0, 0, SUMMARY_LEN, sp, SUMMARY_LEN);
    uint32_t seq = lastSeq_ + 1, cc = 0;
    for (const Pending& p : batchRecs_) { uint8_t b[4]; wr32(b, p.crc); cc = crc32_of(b, 4, cc); }
    uint8_t cp[COMMIT_LEN];
    commit_payload(cp, seq, (uint32_t)batchRecs_.size(), cc, lastCommitOff_);
    uint64_t commitRel = batch_.size();
    append_record(batch_, T_COMMIT, C_NONE, 0, 0, 0, COMMIT_LEN, seq, cp, COMMIT_LEN);

    const uint64_t base = truncateAt_;
    // Before appending, drop any uncommitted or torn tail (never below an applied commit).
    if (size_ != base && !truncateTo(base)) { abortBatch(); return fail("cannot truncate the uncommitted tail"); }
    size_ = base;
    if (!seek_to(f_, base)) { abortBatch(); return fail("seek failed"); }
    if (testCrashAfter != (size_t)-1 && testCrashAfter < batch_.size()) {
        // the crash-injection gate: the process dies after `testCrashAfter` bytes reached the file
        std::fwrite(batch_.data(), 1, testCrashAfter, f_);
        if (testCrashZeroFill) {
            std::vector<uint8_t> z(batch_.size() - testCrashAfter, 0);
            std::fwrite(z.data(), 1, z.size(), f_);
        }
        std::fflush(f_);
        std::fclose(f_);
        f_ = nullptr;
        abortBatch();
        return fail("test crash");
    }
    bool ok = std::fwrite(batch_.data(), 1, batch_.size(), f_) == batch_.size();
    ok = ok && fsyncFile();
    if (!ok) {
        // ENOSPC or an I/O error: back to the last commit, state unchanged, the caller retries
        truncateTo(base);
        size_ = file_size(f_);
        abortBatch();
        return fail("write failed (disk full?); the save was rolled back to the last commit");
    }
    size_ = base + batch_.size();
    for (const Pending& p : batchRecs_) {
        Rec r;
        r.off = base + p.rel; r.end = r.off + recSize(p.plen);
        r.aux = p.aux; r.plen = p.plen; r.seq = seq; r.crc = p.crc;
        r.x = p.x; r.z = p.z; r.mask = p.mask; r.type = p.type; r.codec = p.codec; r.padOk = true;
        apply(r);
    }
    lastCommitOff_ = base + commitRel;
    lastSeq_ = seq;
    truncateAt_ = size_;
    tailBytes_ = 0;
    torn_ = false;
    abortBatch();
    if (creating_) {
        std::fclose(f_);
        f_ = nullptr;
        if (!replace_file(path_, finalPath_)) return fail("cannot rename the new world into place");
        path_ = finalPath_;
        finalPath_.clear();
        creating_ = false;
        f_ = fopen_utf8(path_.c_str(), "r+b");
        if (!f_) return fail("cannot reopen the new world");
    }
    return true;
}

// =============================================================================================
// compaction (docs/emod-file-format.md "Batches and commits — writing")
// =============================================================================================
bool EdenWorldStore::compact() {
    error_.clear();
    if (!f_ || !writable_ || creating_) return fail("compaction needs a committed store open for writing");
    if (!batchRecs_.empty()) return fail("compaction mid-batch");
    if (damaged()) return fail("store is damaged: compaction is blocked (export first)");
    const std::string tmp = path_ + ".compact";
    FILE* o = fopen_utf8(tmp.c_str(), "wb");
    if (!o) return fail("cannot create " + tmp);
    std::vector<uint8_t> out, data, enc;
    std::vector<uint32_t> crcs;
    uint8_t fh[FILE_HEADER];
    file_header(fh, bands_, created_);
    out.insert(out.end(), fh, fh + FILE_HEADER);
    bool ok = true;
    auto flush = [&](bool force) {
        if (ok && (force || out.size() >= (1u << 20))) {
            ok = std::fwrite(out.data(), 1, out.size(), o) == out.size();
            out.clear();
        }
    };
    auto copy = [&](const Entry& e, bool rawType) -> bool {
        // decode to prove it is intact; copy zstd verbatim, re-encode anything else
        if (!decodeEntry(e, data)) {
            damage_.push_back(fmt("compaction: live record @%llu does not decode", (unsigned long long)e.off));
            return false;
        }
        uint8_t codec = e.codec;
        if (e.codec == C_ZSTD || (e.codec == C_NONE && (rawType || data.empty()))) {
            enc.resize(e.plen);
            if (!readAt(e.off + REC_HEADER, enc.data(), e.plen)) return false;
        } else if (!codecs_.encode(codec_, data.data(), data.size(), !rawType, &codec, enc)) {
            return false;
        }
        crcs.push_back(append_record(out, e.type, codec, e.mask, e.x, e.z, e.aux, 1, enc.data(), enc.size()));
        flush(false);
        return ok;
    };
    static const int order[] = {T_WORLD_HEADER, T_CREATURES, T_SIGN_TRAILER, T_PROVENANCE};
    for (int t : order) {
        if (!ok) break;
        int32_t i = latestT_[t];
        if (i < 0) continue;
        if (t == T_SIGN_TRAILER && entries_[i].aux == 0) continue;    // an empty trailer = none
        ok = copy(entries_[i], t == T_WORLD_HEADER);
    }
    std::vector<std::pair<uint32_t, int32_t> > cols;
    cols.reserve(colIndex_.size());
    for (const auto& kv : colIndex_) cols.push_back(std::make_pair(entries_[kv.second].aux, kv.second));
    std::sort(cols.begin(), cols.end(), [&](const std::pair<uint32_t, int32_t>& a, const std::pair<uint32_t, int32_t>& b) {
        if (a.first != b.first) return a.first < b.first;
        const Entry& ea = entries_[a.second]; const Entry& eb = entries_[b.second];
        return ea.x != eb.x ? ea.x < eb.x : ea.z < eb.z;
    });
    for (size_t i = 0; ok && i < cols.size(); i++) ok = copy(entries_[cols[i].second], false);
    if (ok) {
        Summary s = summary();
        uint8_t sp[SUMMARY_LEN];
        summary_payload(sp, s.liveColumns, s.nextOrd, s.bandOr);
        crcs.push_back(append_record(out, T_SUMMARY, C_NONE, 0, 0, 0, SUMMARY_LEN, 1, sp, SUMMARY_LEN));
        uint32_t cc = 0;
        for (uint32_t c : crcs) { uint8_t b[4]; wr32(b, c); cc = crc32_of(b, 4, cc); }
        uint8_t cp[COMMIT_LEN];
        commit_payload(cp, 1, (uint32_t)crcs.size(), cc, 0);
        append_record(out, T_COMMIT, C_NONE, 0, 0, 0, COMMIT_LEN, 1, cp, COMMIT_LEN);
        flush(true);
        ok = ok && sync_file(o);
    }
    std::fclose(o);
    if (!ok) {
        remove_file(tmp);
        return fail(damaged() ? "compaction found damage; nothing was replaced" : "compaction write failed; nothing was replaced");
    }
    std::string p = path_;
    int codec = codec_;
    std::fclose(f_);
    f_ = nullptr;
    if (!replace_file(tmp, p)) {
        remove_file(tmp);
        open(p.c_str(), true);
        return fail("cannot rename the compacted file into place");
    }
    bool r = open(p.c_str(), true);
    codec_ = codec;
    return r;
}

// =============================================================================================
// verify + state digest (emod.py Store.verify / state_digest_store)
// =============================================================================================
std::vector<std::string> EdenWorldStore::verify() {
    std::vector<std::string> errs;
    if (!f_) { errs.push_back("not open"); return errs; }
    std::vector<uint8_t> d;
    int back;
    int32_t wh = latestT_[T_WORLD_HEADER];
    if (wh < 0) errs.push_back("no WORLD_HEADER");
    else {
        const Entry& e = entries_[wh];
        if (!decodeEntry(e, d)) errs.push_back("WORLD_HEADER does not decode");
        else if (d.size() != EDEN_HEADER) errs.push_back(fmt("WORLD_HEADER is %zu B", d.size()));
        else {
            int32_t v = (int32_t)rd32(d.data() + 92);
            // S.3b: v5/6 => 16 bands, but not the converse (the 2026 game writes v2 256z worlds)
            if (v < 1 || v > 6 || (v >= 5 && bands_ != 16)) errs.push_back(fmt("WORLD_HEADER version %d does not match bands=%d", v, bands_));
        }
        if (e.codec != C_NONE) errs.push_back("WORLD_HEADER must be codec 0");
    }
    int32_t cr = latestT_[T_CREATURES];
    if (cr < 0) errs.push_back("no CREATURES");
    else if (entries_[cr].aux % EDEN_ENT || entries_[cr].aux / EDEN_ENT > EDEN_MAX_SLOTS)
        errs.push_back(fmt("CREATURES raw length %u is not 0..400 slots of 60 B", entries_[cr].aux));
    int32_t st = latestT_[T_SIGN_TRAILER];
    if (st >= 0 && (entries_[st].aux % EDEN_DIR_ROW || entries_[st].aux > TRAILER_MAX))
        errs.push_back(fmt("SIGN_TRAILER raw length %u is not a multiple of 16 up to 1 MiB", entries_[st].aux));
    static const int single[] = {T_CREATURES, T_SIGN_TRAILER, T_PROVENANCE, T_SUMMARY};
    for (int t : single) {
        int32_t i = latestT_[t];
        if (i < 0) continue;
        const Entry& e = entries_[i];
        if (e.mask || e.x || e.z) errs.push_back(fmt("record type %d: band_mask/x/z must be 0", t));
        int got = decodeNewest(i, d, &back);
        if (got < 0) { errs.push_back(fmt("record type %d does not decode", t)); damage_.push_back(errs.back()); }
        else if (back) { errs.push_back(fmt("record type %d does not decode; %d versions back does", t, back)); damage_.push_back(errs.back()); }
        if (t == T_PROVENANCE && got == i) {
            // parse_provenance: 76 fixed bytes, then three length-prefixed lists ending exactly at the end
            uint64_t o = 76;
            bool okp = d.size() >= o;
            auto need = [&](uint64_t k) { if (okp && o + k > d.size()) okp = false; return okp; };
            if (need(4)) { uint64_t nl = rd32(&d[o]); o += 4; if (need(nl)) o += nl; }
            if (need(4)) { uint64_t ns = rd32(&d[o]); o += 4; if (need(12 * ns)) o += 12 * ns; }
            if (need(4)) { uint64_t nd = rd32(&d[o]); o += 4; if (need(8 * nd)) o += 8 * nd; }
            if (!okp || o != d.size()) errs.push_back("PROVENANCE does not parse");
        }
    }
    std::unordered_map<uint32_t, uint64_t> ords;
    uint16_t bandOr = 0;
    bool any = false;
    uint32_t mx = 0;
    for (const auto& kv : colIndex_) {
        const Entry& e = entries_[kv.second];
        if (!addressable(e.x, e.z)) errs.push_back(fmt("COLUMN has an unaddressable key (%d,%d)", e.x, e.z));
        if (e.mask >> bands_) errs.push_back(fmt("COLUMN (%d,%d) sets band bits >= %d", e.x, e.z, bands_));
        if (e.mask == 0 && (e.codec != C_NONE || e.plen)) errs.push_back(fmt("COLUMN (%d,%d): empty mask needs codec 0 and no payload", e.x, e.z));
        if (ords.count(e.aux)) errs.push_back(fmt("COLUMN (%d,%d) shares ord %u", e.x, e.z, e.aux));
        ords[e.aux] = kv.first;
        bandOr |= e.mask;
        if (!any || e.aux > mx) mx = e.aux;
        any = true;
        int got = decodeNewest(kv.second, d, &back);
        if (got < 0) { errs.push_back(fmt("COLUMN (%d,%d) does not decode and has no earlier version", e.x, e.z)); damage_.push_back(errs.back()); }
        else if (back) { errs.push_back(fmt("COLUMN (%d,%d) does not decode; fell back %d version(s)", e.x, e.z, back)); damage_.push_back(errs.back()); }
        else {
            int bi = 0;
            for (int b = 0; b < bands_; b++) {
                if (!(e.mask >> b & 1)) continue;
                if (all_zero(d.data() + (size_t)bi * BAND, BAND))
                    warnings_.push_back(fmt("COLUMN (%d,%d): band %d stored but all zero (non-canonical)", e.x, e.z, b));
                bi++;
            }
        }
    }
    int32_t sm = latestT_[T_SUMMARY];
    if (!colIndex_.empty() && sm < 0) errs.push_back("no SUMMARY");
    else if (sm >= 0) {
        if (!decodeEntry(entries_[sm], d) || d.size() != SUMMARY_LEN) errs.push_back("SUMMARY does not decode");
        else {
            uint32_t lc = rd32(&d[0]), no = rd32(&d[4]);
            uint16_t bo = rd16(&d[8]);
            if (lc != colIndex_.size() || no != (any ? mx + 1 : 0) || bo != bandOr || rd16(&d[10]) || rd32(&d[12]))
                errs.push_back(fmt("SUMMARY (live=%u next_ord=%u band_or=%#x) != the live state", lc, no, bo));
        }
    }
    std::vector<std::string> all = errors_;
    all.insert(all.end(), damage_.begin(), damage_.end());
    // the decode failures above were pushed to damage_ too; don't list them twice
    for (const std::string& e : errs)
        if (std::find(all.begin(), all.end(), e) == all.end()) all.push_back(e);
    return all;
}

std::string EdenWorldStore::stateDigest() {
    Sha256 h;
    std::vector<uint8_t> d;
    static const int t3[] = {T_WORLD_HEADER, T_CREATURES, T_SIGN_TRAILER};
    for (int t : t3) {
        int32_t i = latestT_[t];
        if (i < 0) { h.update("-", 1); continue; }
        if (!decodeEntry(entries_[i], d)) return "undecodable";
        if (!d.empty()) h.update(d.data(), d.size());
    }
    std::vector<std::pair<std::pair<int32_t, int32_t>, int32_t> > cols;
    for (const auto& kv : colIndex_) cols.push_back(std::make_pair(std::make_pair(entries_[kv.second].x, entries_[kv.second].z), kv.second));
    std::sort(cols.begin(), cols.end());
    for (const auto& c : cols) {
        const Entry& e = entries_[c.second];
        uint8_t k[10];
        wr32(k, (uint32_t)e.x);
        wr32(k + 4, (uint32_t)e.z);
        wr16(k + 8, e.mask);
        h.update(k, 10);
        if (!decodeEntry(e, d)) return "undecodable";
        if (!d.empty()) h.update(d.data(), d.size());
    }
    uint8_t out[32];
    h.final(out);
    return hex(out, 32);
}

// =============================================================================================
// EdenEmodConverter — forward-only `.eden` -> `.emod` (emod.py StreamConverter; plan §4)
// =============================================================================================
namespace {

// The engine's reading of a `.eden` from header + size + directory region (emod.py Layout,
// itself FileManager::readDirectory + deriveColumnSpans). Never touches the column region.
struct LCol { int32_t x, z; uint64_t off, span; uint32_t row; };
struct Layout {
    int32_t version; int bands; uint64_t col, dirOff, size, blockEnd, deadBytes;
    uint32_t flags, dirRows, slots;
    std::vector<LCol> cols;                                   // offset order = ord order
    std::vector<std::pair<int32_t, int32_t> > dirOrder;
    std::vector<LCol> shortSpans;                            // row order
    std::vector<uint8_t> trailer;
    uint8_t sourceHash[32];
};

enum : uint32_t {
    PF_NONCANONICAL_LAYOUT = 1, PF_SHORT_SPANS = 2, PF_DROPPED_ROWS = 4, PF_DUPLICATE_KEYS = 8,
    PF_DUPLICATE_OFFSETS = 16, PF_PARTIAL_DIR_ROW = 32, PF_CREATURE_GAP_INVALID = 64, PF_TRAILER_DROPPED = 128,
    PF_UPGRADED_256Z = 256,
};

bool parse_layout(const uint8_t* h, uint64_t size, const std::vector<uint8_t>& raw, Layout& L) {
    L.version = (int32_t)rd32(h + 92);
    L.dirOff = rd64(h + 32);
    L.size = size;
    L.flags = 0;
    size_t nrows = raw.size() / EDEN_DIR_ROW;
    L.dirRows = (uint32_t)nrows;
    if (raw.size() % EDEN_DIR_ROW) L.flags |= PF_PARTIAL_DIR_ROW;
    std::unordered_map<uint64_t, LCol> live;
    size_t pendStart = 0, pendCount = 0, dropped = 0, dups = 0;
    for (size_t i = 0; i < nrows; i++) {
        const uint8_t* r = raw.data() + i * EDEN_DIR_ROW;
        int32_t x = (int32_t)rd32(r), z = (int32_t)rd32(r + 4);
        uint64_t o = rd64(r + 8);
        if (addressable(x, z)) {
            dropped += pendCount;
            pendCount = 0;
            uint64_t k = ((uint64_t)(uint32_t)x << 32) | (uint32_t)z;
            auto it = live.find(k);
            if (it != live.end()) dups++;
            live[k] = LCol{x, z, o, 0, (uint32_t)i};
        } else {
            if (!pendCount) pendStart = i;
            pendCount++;
        }
    }
    L.trailer.assign(raw.begin() + pendStart * EDEN_DIR_ROW, raw.begin() + (pendStart + pendCount) * EDEN_DIR_ROW);
    if (L.trailer.size() > TRAILER_MAX) { L.flags |= PF_TRAILER_DROPPED; L.trailer.clear(); }
    if (dropped) L.flags |= PF_DROPPED_ROWS;
    if (dups) L.flags |= PF_DUPLICATE_KEYS;
    std::vector<LCol> ents;
    ents.reserve(live.size());
    for (const auto& kv : live) ents.push_back(kv.second);
    std::sort(ents.begin(), ents.end(), [](const LCol& a, const LCol& b) { return a.row < b.row; });
    L.dirOrder.clear();
    for (const LCol& e : ents) L.dirOrder.push_back(std::make_pair(e.x, e.z));
    std::vector<uint64_t> offsets;
    offsets.reserve(ents.size());
    for (const LCol& e : ents) offsets.push_back(e.off);
    std::sort(offsets.begin(), offsets.end());
    for (size_t i = 1; i < offsets.size(); i++) if (offsets[i] == offsets[i - 1]) { L.flags |= PF_DUPLICATE_OFFSETS; break; }
    const size_t n = offsets.size();
    L.bands = eden_detect_bands(L.version, L.dirOff, offsets.data(), n);   // S.3b: not `version >= 5`
    L.col = (uint64_t)L.bands * BAND;
    uint32_t slots = L.bands == 16 ? 400 : 200;
    if (n) {
        uint64_t mo = offsets.back();
        bool gapOk = mo <= L.dirOff && L.dirOff - mo >= L.col;   // dir_off >= last_end, overflow-safe
        uint64_t gap = gapOk ? L.dirOff - mo - L.col : 0;
        if (gapOk && gap % EDEN_ENT == 0 && gap / EDEN_ENT <= EDEN_MAX_SLOTS) slots = (uint32_t)(gap / EDEN_ENT);
        else L.flags |= PF_CREATURE_GAP_INVALID;
    }
    L.slots = slots;
    if (L.dirOff < (uint64_t)EDEN_HEADER + (uint64_t)EDEN_ENT * slots) return false;
    L.blockEnd = L.dirOff - (uint64_t)EDEN_ENT * slots;
    L.cols.clear(); L.shortSpans.clear();
    for (LCol e : ents) {
        int64_t lo = 0, hi = (int64_t)n - 1, at = -1;
        while (lo <= hi) {                              // the exact binary search assignSpan does
            int64_t mid = (lo + hi) / 2;
            if (offsets[mid] == e.off) { at = mid; break; }
            if (offsets[mid] < e.off) lo = mid + 1; else hi = mid - 1;
        }
        uint64_t span = L.col;
        if (at >= 0) {
            uint64_t nxt = (uint64_t)(at + 1) < n ? offsets[at + 1] : L.blockEnd;
            if (nxt > e.off && nxt - e.off < L.col) span = nxt - e.off;
        }
        e.span = span;
        if (span < L.col) L.shortSpans.push_back(e);
        L.cols.push_back(e);
    }
    std::stable_sort(L.cols.begin(), L.cols.end(), [](const LCol& a, const LCol& b) { return a.off < b.off; });
    if (!L.shortSpans.empty()) L.flags |= PF_SHORT_SPANS;
    uint64_t referenced = 0;
    for (const LCol& c : L.cols) referenced += std::min<uint64_t>(c.span, size > c.off ? size - c.off : 0);
    uint64_t region = L.blockEnd - EDEN_HEADER;
    L.deadBytes = region > referenced ? region - referenced : 0;
    bool canon = L.dirOff == EDEN_HEADER + n * L.col + (uint64_t)EDEN_ENT * slots;
    for (size_t i = 0; canon && i < n; i++) canon = offsets[i] == EDEN_HEADER + i * L.col;
    if (!canon) L.flags |= PF_NONCANONICAL_LAYOUT;
    Sha256 s;
    s.update(h, EDEN_HEADER);
    if (!raw.empty()) s.update(raw.data(), raw.size());
    s.final(L.sourceHash);
    return true;
}

void provenance_payload(const Layout& L, int64_t mtime, const std::string& name, std::vector<uint8_t>& p) {
    p.clear();
    put32(p, 1);
    put32(p, L.flags);
    put64(p, L.size);
    put64(p, (uint64_t)mtime);
    p.insert(p.end(), L.sourceHash, L.sourceHash + 32);
    put32(p, (uint32_t)L.version);
    put32(p, L.dirRows);
    put64(p, L.deadBytes);
    put32(p, L.slots);
    put32(p, (uint32_t)name.size());
    p.insert(p.end(), name.begin(), name.end());
    put32(p, (uint32_t)L.shortSpans.size());
    for (const LCol& s : L.shortSpans) { put32(p, (uint32_t)s.x); put32(p, (uint32_t)s.z); put32(p, (uint32_t)s.span); }
    put32(p, (uint32_t)L.dirOrder.size());
    for (const auto& k : L.dirOrder) { put32(p, (uint32_t)k.first); put32(p, (uint32_t)k.second); }
}

}  // namespace

// S.3b: the source's stride is only known at EOF (the header `version` does not decide it), so the
// column region is spilled on one fixed grid -- 131,072 B slots, 16 bands -- whatever the world's
// height. A 256z column on that grid IS a slot (finish() copies its payload); a 64z column is a
// quarter of one and goes through the rebuild path, which cuts it at the detected stride.
const uint64_t SPILL_GRID = 16ull * BAND;
const int SPILL_BANDS = 16;

EdenEmodConverter::EdenEmodConverter() : spill_(nullptr), err_(OK), begun_(false), done_(false) { abort(); }
EdenEmodConverter::~EdenEmodConverter() { abort(); }

void EdenEmodConverter::abort() {
    if (spill_) { std::fclose(spill_); spill_ = nullptr; }
    if (begun_ && !done_) { remove_file(spillPath_); remove_file(convPath_); }
    begun_ = false; done_ = false;
    hdrFill_ = 0; bands_ = 0; col_ = 0; dirOff_ = 0; pos_ = 0;
    slot_.clear(); slot_.shrink_to_fit(); dir_.clear(); dir_.shrink_to_fit(); slots_.clear(); slots_.shrink_to_fit();
    cacheK_[0] = cacheK_[1] = (uint64_t)-1; cache_[0].clear(); cache_[1].clear(); cacheNext_ = 0;
    std::memset(&st_, 0, sizeof st_);
}

bool EdenEmodConverter::fail(Err e, const std::string& msg) {
    err_ = e;
    error_ = msg;
    if (spill_) { std::fclose(spill_); spill_ = nullptr; }
    if (begun_) { remove_file(spillPath_); remove_file(convPath_); }
    begun_ = false;
    done_ = true;
    return false;
}

bool EdenEmodConverter::begin(const char* outPath, const Options& opt) {
    abort();
    err_ = OK; error_.clear();
    opt_ = opt;
    out_ = outPath;
    spillPath_ = out_ + ".spill";
    convPath_ = out_ + ".converting";
    spill_ = fopen_utf8(spillPath_.c_str(), "w+b");
    if (!spill_) { err_ = E_IO; error_ = "cannot create " + spillPath_; return false; }
    begun_ = true;
    return true;
}

void EdenEmodConverter::hold() {
    uint64_t held = slot_.capacity() + dir_.capacity() + slots_.capacity() * sizeof(Slot) + cache_[0].capacity() + cache_[1].capacity()
                    + payload_.capacity() + enc_.capacity();
    st_.peakHeldBytes = std::max(st_.peakHeldBytes, held);
}

bool EdenEmodConverter::feed(const void* data, size_t n) {
    if (!begun_ || done_) return fail(E_STATE, "feed() without begin(), or after finish()/failure");
    const uint8_t* p = (const uint8_t*)data;
    while (n) {
        if (hdrFill_ < EDEN_HEADER) {
            size_t take = std::min<size_t>(n, EDEN_HEADER - hdrFill_);
            std::memcpy(hdr_ + hdrFill_, p, take);
            hdrFill_ += (uint32_t)take; p += take; n -= take; pos_ += take;
            if (hdrFill_ == EDEN_HEADER) {
                int32_t v = (int32_t)rd32(hdr_ + 92);
                if (v < 1 || v > 1000) return fail(E_LEGACY, fmt("legacy 1.x file (version field %d)", v));
                if (v > 6) return fail(E_NEWER, fmt("version %d is newer than any known .eden", v));
                dirOff_ = rd64(hdr_ + 32);
                if (dirOff_ < EDEN_HEADER) return fail(E_DIR_OFFSET, fmt("directory offset %llu inside the header", (unsigned long long)dirOff_));
                slot_.reserve((size_t)SPILL_GRID);
            }
            continue;
        }
        if (pos_ < dirOff_) {
            size_t take = (size_t)std::min<uint64_t>(std::min<uint64_t>(n, dirOff_ - pos_), SPILL_GRID - slot_.size());
            slot_.insert(slot_.end(), p, p + take);
            p += take; n -= take; pos_ += take;
            if (slot_.size() == SPILL_GRID || pos_ == dirOff_) { if (!spillSlot()) return false; }
            continue;
        }
        if (dir_.size() + n > (256u << 20)) return fail(E_DIRECTORY_SIZE, "directory region over 256 MiB: not a world");
        dir_.insert(dir_.end(), p, p + n);
        pos_ += n; p += n; n = 0;
    }
    st_.bytesIn = pos_;
    hold();
    return true;
}

bool EdenEmodConverter::spillSlot() {
    const uint32_t len = (uint32_t)slot_.size();
    slot_.resize((size_t)SPILL_GRID, 0);                // a partial last slot is zero-padded to split
    Slot s;
    s.crc = crc32_of(slot_.data(), len);
    s.len = len;
    s.mask = split_bands(slot_.data(), SPILL_BANDS, payload_);
    if (!codecs_.encode(opt_.codec, payload_.data(), payload_.size(), true, &s.codec, enc_)) return fail(E_IO, "encode failed");
    s.stored = (uint32_t)enc_.size();
    s.spillOff = st_.spillBytes;
    if (s.stored && std::fwrite(enc_.data(), 1, s.stored, spill_) != s.stored) return fail(E_IO, "spill write failed (disk full?)");
    st_.spillBytes += s.stored;
    if (opt_.maxTempBytes && st_.spillBytes > opt_.maxTempBytes) return fail(E_CAP, "temp-bytes cap exceeded while spilling");
    slots_.push_back(s);
    slot_.clear();
    return true;
}

bool EdenEmodConverter::slotRaw(uint64_t k, const uint8_t** raw, uint32_t* len) {
    for (int i = 0; i < 2; i++) if (cacheK_[i] == k) { *raw = cache_[i].data(); *len = slots_[k].len; return true; }
    const Slot& s = slots_[k];
    enc_.resize(s.stored);
    if (s.stored && (!seek_to(spill_, s.spillOff) || std::fread(enc_.data(), 1, s.stored, spill_) != s.stored)) return false;
    payload_.resize((size_t)popcount16(s.mask) * BAND);
    if (!codecs_.decode(s.codec, enc_.data(), s.stored, payload_.size(), payload_.data())) return false;
    int c = cacheNext_;
    cacheNext_ ^= 1;
    cache_[c].resize((size_t)SPILL_GRID);
    join_bands(s.mask, payload_.data(), SPILL_BANDS, cache_[c].data());
    // The rebuild path reads the source back out of the spill, so the read-back check in finish()
    // would be self-referential without this: the slot must reproduce the CRC of the bytes that
    // were actually fed.
    if (crc32_of(cache_[c].data(), s.len) != s.crc) { cacheK_[c] = (uint64_t)-1; return false; }
    cacheK_[c] = k;
    *raw = cache_[c].data();
    *len = s.len;
    hold();
    return true;
}

// Bytes [o, o+n) of the source as a file read sees them (short at EOF): header, spilled region,
// directory buffer.
bool EdenEmodConverter::readSource(uint64_t o, uint64_t n, std::vector<uint8_t>& out) {
    out.clear();
    if (o >= pos_) return true;
    uint64_t end = o + std::min<uint64_t>(n, pos_ - o);
    while (o < end) {
        uint64_t take;
        if (o < EDEN_HEADER) {
            take = std::min<uint64_t>(end, EDEN_HEADER) - o;
            out.insert(out.end(), hdr_ + o, hdr_ + o + take);
        } else if (o < dirOff_) {
            uint64_t k = (o - EDEN_HEADER) / SPILL_GRID, base = EDEN_HEADER + k * SPILL_GRID;
            take = std::min(std::min(end, dirOff_), base + SPILL_GRID) - o;
            const uint8_t* raw; uint32_t len;
            if (!slotRaw(k, &raw, &len)) return false;
            out.insert(out.end(), raw + (o - base), raw + (o - base) + take);
        } else {
            take = end - o;
            out.insert(out.end(), dir_.begin() + (size_t)(o - dirOff_), dir_.begin() + (size_t)(o - dirOff_ + take));
        }
        o += take;
    }
    return true;
}

bool EdenEmodConverter::finish() {
    if (!begun_ || done_) return fail(E_STATE, "finish() without begin(), or twice");
    if (hdrFill_ < EDEN_HEADER) return fail(E_SHORT, "shorter than a 192-byte header");
    if (pos_ < dirOff_) return fail(E_DIR_OFFSET, fmt("directory offset %llu outside the file (%llu B)", (unsigned long long)dirOff_, (unsigned long long)pos_));
    if (!slot_.empty() && !spillSlot()) return false;
    if (std::fflush(spill_) != 0) return fail(E_IO, "spill flush failed");
    Layout L;
    if (!parse_layout(hdr_, pos_, dir_, L)) return fail(E_LAYOUT, "creature block would start inside the header");
    bands_ = L.bands;                                   // the detected height, from here on
    col_ = L.col;
    std::vector<uint8_t> creatures, data, rec, payload, enc;
    if (!readSource(L.blockEnd, (uint64_t)EDEN_ENT * L.slots, creatures) || creatures.size() != (size_t)EDEN_ENT * L.slots)
        return fail(E_IO, "cannot read the creature block back from the spill");
    // S.5e: promote a 64z source to 256z. The bands the source has are its bytes; the twelve above
    // are absent from every band_mask (air), so each COLUMN payload is the one a 64z conversion
    // writes -- only the stride it is read at changes. The header and creature block get what a
    // 256z `.eden` carries (the exporter's NewDawn256z rules, EdenWorldExport.cpp).
    const bool up = opt_.upgradeTo256z && L.bands == 4;
    const int outBands = up ? 16 : L.bands;
    const uint64_t outCol = (uint64_t)outBands * BAND;
    uint8_t hdrOut[EDEN_HEADER];
    std::memcpy(hdrOut, hdr_, EDEN_HEADER);
    if (up) {
        int32_t v = (int32_t)rd32(hdrOut + 92);
        if (v == 3) {
            // FileManager's one-time v3 -> v4 upgrade (goldencubes 10, sky COLOR_NORMAL_BLUE = 14):
            // it keys off version 3, which is about to stop being the version.
            wr32(hdrOut + 148, 10);
            std::memset(hdrOut + 132, 14, 16);
        }
        if (v < 5) wr32(hdrOut + 92, 5);
        if (L.slots < EDEN_MAX_SLOTS) {
            size_t was = creatures.size();
            creatures.resize((size_t)EDEN_MAX_SLOTS * EDEN_ENT, 0);
            for (size_t e = was; e < creatures.size(); e += EDEN_ENT) wr32(&creatures[e + 28], 0xffffffffu);   // type = -1
        }
        L.flags |= PF_UPGRADED_256Z;
    }
    FILE* o = fopen_utf8(convPath_.c_str(), "wb");
    if (!o) return fail(E_IO, "cannot create " + convPath_);
    bool ok = true;
    std::vector<uint32_t> crcs;
    auto flush = [&](bool force) {
        if (ok && (force || rec.size() >= (1u << 20))) {
            ok = std::fwrite(rec.data(), 1, rec.size(), o) == rec.size();
            st_.outBytes += rec.size();
            rec.clear();
            if (ok && opt_.maxTempBytes && st_.spillBytes + st_.outBytes > opt_.maxTempBytes) ok = false, err_ = E_CAP;
        }
    };
    auto put = [&](uint8_t type, const uint8_t* d, size_t n, bool compress) {
        uint8_t codec;
        if (!codecs_.encode(opt_.codec, d, n, compress, &codec, enc)) { ok = false; return; }
        crcs.push_back(append_record(rec, type, codec, 0, 0, 0, (uint32_t)n, 1, enc.data(), enc.size()));
        flush(false);
    };
    uint8_t fh[FILE_HEADER];
    file_header(fh, outBands, (uint64_t)opt_.createdUnix);
    rec.insert(rec.end(), fh, fh + FILE_HEADER);
    put(T_WORLD_HEADER, hdrOut, EDEN_HEADER, false);
    put(T_CREATURES, creatures.data(), creatures.size(), true);
    if (!L.trailer.empty()) put(T_SIGN_TRAILER, L.trailer.data(), L.trailer.size(), true);
    std::vector<uint8_t> prov;
    provenance_payload(L, opt_.createdUnix, opt_.sourceName, prov);
    put(T_PROVENANCE, prov.data(), prov.size(), true);
    std::vector<uint32_t> want(L.cols.size());
    uint16_t bandOr = 0;
    for (size_t ord = 0; ok && ord < L.cols.size(); ord++) {
        const LCol& c = L.cols[ord];
        uint64_t k = c.off >= EDEN_HEADER ? (c.off - EDEN_HEADER) / SPILL_GRID : 0;
        bool fast = col_ == SPILL_GRID && c.off >= EDEN_HEADER && (c.off - EDEN_HEADER) % SPILL_GRID == 0 &&
                    c.span == col_ && k < slots_.size() && slots_[k].len == SPILL_GRID;
        uint16_t mask;
        uint8_t codec;
        if (fast) {
            // a full-stride column on a slot boundary IS that slot: copy its spilled payload
            const Slot& s = slots_[k];
            enc.resize(s.stored);
            if (s.stored && (!seek_to(spill_, s.spillOff) || std::fread(enc.data(), 1, s.stored, spill_) != s.stored)) { ok = false; break; }
            mask = s.mask; codec = s.codec; want[ord] = s.crc;
            st_.copied++;
        } else {
            // short span, off-grid offset, an offset outside the region: rebuild as Eden.column reads it
            if (!readSource(c.off, c.span, data)) { ok = false; break; }
            data.resize((size_t)outCol, 0);             // S.5e: an upgraded column's bands 4-15 are air
            want[ord] = crc32_of(data.data(), data.size());
            mask = split_bands(data.data(), outBands, payload);
            if (!codecs_.encode(opt_.codec, payload.data(), payload.size(), true, &codec, enc)) { ok = false; break; }
            st_.rebuilt++;
        }
        bandOr |= mask;
        crcs.push_back(append_record(rec, T_COLUMN, codec, mask, c.x, c.z, (uint32_t)ord, 1, enc.data(), enc.size()));
        flush(false);
    }
    if (ok) {
        uint8_t sp[SUMMARY_LEN];
        summary_payload(sp, (uint32_t)L.cols.size(), (uint32_t)L.cols.size(), bandOr);
        put(T_SUMMARY, sp, SUMMARY_LEN, false);
        uint32_t cc = 0;
        for (uint32_t c : crcs) { uint8_t b[4]; wr32(b, c); cc = crc32_of(b, 4, cc); }
        uint8_t cp[COMMIT_LEN];
        commit_payload(cp, 1, (uint32_t)crcs.size(), cc, 0);
        append_record(rec, T_COMMIT, C_NONE, 0, 0, 0, COMMIT_LEN, 1, cp, COMMIT_LEN);
        flush(true);
        ok = ok && sync_file(o);
    }
    std::fclose(o);
    if (!ok) return fail(err_ == E_CAP ? E_CAP : E_IO, err_ == E_CAP ? "temp-bytes cap exceeded" : "output write failed (disk full?)");
    st_.columns = L.cols.size();
    st_.slots = slots_.size();
    st_.creatureSlots = L.slots;
    st_.trailerBytes = (uint32_t)L.trailer.size();
    st_.flags = L.flags;
    st_.bandsIn = L.bands;
    st_.bandsOut = outBands;
    // convert-on-open step 3: re-read every record and compare with the source before the rename
    {
        EdenWorldStore s;
        if (!s.open(convPath_.c_str(), false)) return fail(E_VERIFY, "written file does not open: " + s.error());
        if (s.damaged() || s.torn() || s.tailBytes()) return fail(E_VERIFY, "written file is damaged/torn");
        if (s.columnCount() != L.cols.size()) return fail(E_VERIFY, "written file has the wrong column count");
        if (s.bands() != outBands) return fail(E_VERIFY, "written file has the wrong height");
        std::vector<uint8_t> buf((size_t)outCol);
        for (size_t ord = 0; ord < L.cols.size(); ord++) {
            if (s.readColumn(L.cols[ord].x, L.cols[ord].z, buf.data()) != EdenWorldStore::READ_OK ||
                crc32_of(buf.data(), buf.size()) != want[ord])
                return fail(E_VERIFY, fmt("column (%d,%d) does not read back as the source", L.cols[ord].x, L.cols[ord].z));
        }
        std::vector<uint8_t> h;
        if (s.readRecord(T_WORLD_HEADER, h) != EdenWorldStore::READ_OK || h.size() != EDEN_HEADER || std::memcmp(h.data(), hdrOut, EDEN_HEADER))
            return fail(E_VERIFY, "WORLD_HEADER does not read back");
    }
    std::fclose(spill_);
    spill_ = nullptr;
    if (!replace_file(convPath_, out_)) return fail(E_IO, "cannot rename the converted file into place");
    remove_file(spillPath_);
    done_ = true;
    begun_ = false;
    return true;
}

// ---- S.5: the parser and the file primitives, exposed (EdenWorldStore.h) ----------------------
bool eden_read_layout(const uint8_t header[EDEN_HEADER], uint64_t fileSize, const std::vector<uint8_t>& dirRegion,
                      EdenLayout& out) {
    Layout L;
    if (!parse_layout(header, fileSize, dirRegion, L)) return false;
    out.version = L.version;
    out.bands = L.bands;
    out.slots = L.slots;
    out.flags = L.flags;
    out.dirOff = L.dirOff;
    out.blockEnd = L.blockEnd;
    out.cols.clear();
    out.cols.reserve(L.cols.size());
    for (const LCol& c : L.cols) out.cols.push_back(EdenLayoutCol{c.x, c.z, c.off, c.span});
    out.dirOrder = L.dirOrder;
    out.trailer = L.trailer;
    return true;
}

namespace io {
FILE* fopen_utf8(const char* path, const char* mode) { return ::emod::fopen_utf8(path, mode); }
bool seek_to(FILE* f, uint64_t off) { return ::emod::seek_to(f, off); }
uint64_t file_size(FILE* f) { return ::emod::file_size(f); }
bool sync_file(FILE* f) { return ::emod::sync_file(f); }
bool replace_file(const std::string& from, const std::string& to) { return ::emod::replace_file(from, to); }
void remove_file(const std::string& path) { ::emod::remove_file(path); }
}

}  // namespace emod
