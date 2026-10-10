//
//  EdenWorldStore.h
//  Eden — Stage S / S.3: the `.emod` world container (docs/emod-file-format.md is normative).
//
//  Standalone on purpose: C++ only, no ObjC, no engine globals, no FileManager. S.4 routes
//  FileManager to it behind g_world_format; until then nothing in the game calls it, and the
//  only callers are its self-test (EdenWorldStoreSelftest.cpp: `eden_native --emod-selftest`,
//  web/tools/headless-emod-store-test.js). The reference it is checked against is
//  web/tools/emod.py, which shares no code with this file on purpose.
//
//  Two classes:
//    EdenWorldStore     open / scan / recover a `.emod`; read columns (with the spec's
//                       damaged-record fallback) and singletons; append a batch + COMMIT + fsync;
//                       compact; verify.
//    EdenEmodConverter  `.eden` -> `.emod` as a FORWARD-ONLY byte sink (plan §4 "Downloads and
//                       imports"): feed() it the bytes of a raw `.eden` in any chunking, in order,
//                       once, then finish(). Never seeks the source, so the same converter serves
//                       a local file, an inflating download and a browser import. Output is
//                       byte-identical to `emod.py convert` (and to `emod.py convert --stream`).
//
//  Threading: one owner at a time, like FileManager (plan §1). Nothing here is thread-safe and
//  nothing here needs to be.
//
#ifndef Eden_EdenWorldStore_h
#define Eden_EdenWorldStore_h

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

namespace emod {

// ---- format constants (docs/emod-file-format.md) ------------------------------------------
enum : uint32_t {
    FILE_HEADER = 64,
    REC_HEADER = 32,
    MAX_PAYLOAD = 16u << 20,
    BAND = 8192,                 // 4,096 type bytes + 4,096 paint bytes, CC(x,z,y) order
    EDEN_HEADER = 192,           // WorldFileHeader
    EDEN_DIR_ROW = 16,           // ColumnIndex
    EDEN_ENT = 60,               // EntityData
    EDEN_MAX_SLOTS = 400,
    TRAILER_MAX = 1u << 20,
};
enum RecordType : uint8_t {
    T_COLUMN = 1, T_WORLD_HEADER = 2, T_CREATURES = 3, T_SIGN_TRAILER = 4,
    T_PROVENANCE = 5, T_SUMMARY = 6, T_COMMIT = 7, T_INDEX = 8,
};
enum Codec : uint8_t { C_NONE = 0, C_ZSTD = 1, C_ZLIB = 2 };

uint32_t crc32_of(const void* p, size_t n, uint32_t crc = 0);

// SHA-256 (PROVENANCE's source hash; the self-test's state digest). Small and local: nothing in
// the tree links one on every target.
struct Sha256 {
    uint32_t h[8]; uint64_t len; uint8_t buf[64]; size_t fill;
    Sha256();
    void update(const void* p, size_t n);
    void final(uint8_t out[32]);
};

// Encoder/decoder with a reusable zstd context each (created lazily, freed in the destructor).
// Encoding is what conversion, compaction and saves share, so all three produce the same bytes.
class Codecs {
public:
    Codecs();
    ~Codecs();
    // (codec, stored bytes) for `n` decoded bytes. Empty input, `compress == false` or
    // `codec == C_NONE` -> C_NONE and the bytes themselves. zstd: level 3, content checksum on,
    // content size on, no dictionary id (the spec's byte-identical writer).
    bool encode(int codec, const uint8_t* p, size_t n, bool compress, uint8_t* outCodec,
                std::vector<uint8_t>& out);
    // Strict decode: exactly one frame/stream, no trailing bytes, decoded length == `expect`,
    // zstd frames must carry the content checksum and content size. False on any violation.
    bool decode(int codec, const uint8_t* p, size_t n, size_t expect, uint8_t* out);
private:
    void* cctx_; void* dctx_;
    Codecs(const Codecs&); Codecs& operator=(const Codecs&);
};

// (band_mask, payload) for one full-stride column: a band is stored iff any of its 8,192 bytes
// is non-zero (type plane OR paint plane — paint-only bands are real).
uint16_t split_bands(const uint8_t* col, int bands, std::vector<uint8_t>& payload);
void join_bands(uint16_t mask, const uint8_t* payload, int bands, uint8_t* col);

// ---- a `.eden`'s height (S.3b) ----------------------------------------------------------------
// 4 (64z) or 16 (256z) bands, decided from the FILE: the header `version` is not a height marker
// (the 2026 game stamps v2 on some 256z worlds). The sibling editor's rule, VuencEdit
// `detect_chunk_size_by_creature_gap`: version 5/6 -> 16; otherwise the stride (131,072 tried
// first, then 32,768) for which `directory_offset - (highest live offset + stride)` is 0 or a
// whole number of 60-byte slots <= 24,000; when neither is, the smallest gap between distinct
// live offsets (>= 131,072 -> 16); with fewer than two columns, 4. `offsets` are the LIVE rows'
// chunk_offsets (last row wins per key, unaddressable rows skipped), in any order.
// FileManager::probeWorldHeight, convertWorldTo64 and the converter all call this one function;
// emod.py and eden-convert.js restate it.
int eden_detect_bands(int32_t version, uint64_t dirOff, const uint64_t* offsets, size_t n);
// Same, over the raw header and directory region (`directory_offset`..EOF) of a file.
int eden_detect_bands_raw(const uint8_t header[EDEN_HEADER], const uint8_t* dir, size_t dirLen);

// ---- a `.eden`'s layout, as the engine reads it (S.5: shared with EdenWorldExport) ------------
// FileManager::readDirectory + deriveColumnSpans over the header, the file size and the directory
// region: the converter's own parser, exposed. `cols` are in OFFSET order (= an `.emod`'s ord),
// `dirOrder` is the live keys in directory-row order, `trailer` the post-directory sign/command-
// block rows verbatim (empty when none or over 1 MiB). False: the creature block would start
// inside the header.
struct EdenLayoutCol { int32_t x, z; uint64_t off, span; };
struct EdenLayout {
    int32_t version;
    int bands;
    uint32_t slots, flags;
    uint64_t dirOff, blockEnd;
    std::vector<EdenLayoutCol> cols;
    std::vector<std::pair<int32_t, int32_t> > dirOrder;
    std::vector<uint8_t> trailer;
};
bool eden_read_layout(const uint8_t header[EDEN_HEADER], uint64_t fileSize, const std::vector<uint8_t>& dirRegion,
                      EdenLayout& out);

// The store's portable file primitives (UTF-8 paths on Windows, 64-bit offsets, fsync, atomic
// replace), for the S.5 exporter so the two never disagree about durability.
namespace io {
FILE* fopen_utf8(const char* path, const char* mode);
bool seek_to(FILE* f, uint64_t off);
uint64_t file_size(FILE* f);
bool sync_file(FILE* f);
bool replace_file(const std::string& from, const std::string& to);
void remove_file(const std::string& path);
}

// ---- the store ------------------------------------------------------------------------------
class EdenWorldStore {
public:
    enum ReadResult {
        READ_OK = 0,          // the newest committed version decoded
        READ_FALLBACK,        // it did not; an earlier committed version did (damage)
        READ_AIR_DAMAGED,     // no version decodes: air, and the caller must NOT mark it modified
        READ_ABSENT,          // no such key
    };
    struct Summary { uint32_t liveColumns, nextOrd; uint16_t bandOr; };

    EdenWorldStore();
    ~EdenWorldStore();

    // Open and scan per the spec's reading rules. Returns false (and error()) when the file must
    // be refused: unreadable, bad file header, or no valid commit. `writable` opens r+b; nothing
    // is written until commitBatch(). `checkCrc = false` exists ONLY for the corrupt gate's
    // control run (it must then fail).
    bool open(const char* path, bool writable, bool checkCrc = true);
    // A brand-new world: writes a 64-byte file header to `path`.creating. The first
    // commitBatch() (which must carry WORLD_HEADER and CREATURES) fsyncs and renames it to
    // `path`, so a world never appears on disk without a committed batch.
    bool create(const char* path, int bands, uint64_t createdUnix);
    void close();
    bool isOpen() const { return f_ != nullptr; }

    const std::string& error() const { return error_; }
    const std::string& path() const { return path_; }
    int bands() const { return bands_; }
    uint64_t createdUnix() const { return created_; }
    uint64_t fileSize() const { return size_; }
    uint32_t lastSeq() const { return lastSeq_; }
    // The spec's per-session damaged flag: media damage inside committed data, or a live record
    // that failed to decode. Blocks compaction. A torn last batch / a tail is NOT damage.
    bool damaged() const { return !damage_.empty(); }
    bool torn() const { return torn_; }
    uint64_t tailBytes() const { return tailBytes_; }
    uint64_t truncationPoint() const { return truncateAt_; }
    const std::vector<std::string>& damage() const { return damage_; }
    const std::vector<std::string>& warnings() const { return warnings_; }
    double openMs() const { return openMs_; }

    // ---- live view
    size_t columnCount() const { return colIndex_.size(); }
    std::vector<std::pair<int32_t, int32_t> > columnKeys() const;   // ord order
    bool hasColumn(int32_t x, int32_t z) const;
    // Mask of the newest committed version (what SUMMARY counts); 0 if absent.
    uint16_t columnMask(int32_t x, int32_t z) const;
    // `out` receives bands() * 8192 bytes (cleared bands zero). `maskOut` (may be null) gets the
    // mask of the version actually used.
    ReadResult readColumn(int32_t x, int32_t z, uint8_t* out, uint16_t* maskOut = nullptr);
    bool hasRecord(int type) const { return type > 0 && type < 256 && latestT_[type] >= 0; }
    // Newest decodable committed version of a singleton type (WORLD_HEADER, CREATURES, ...).
    ReadResult readRecord(int type, std::vector<uint8_t>& out);
    Summary summary() const;
    // Bytes no live record uses (superseded versions + record overhead of dead ones); the
    // compaction policy's input.
    uint64_t deadBytes() const;
    // The plan's policy (§6): at open/close only, when dead > max(50% of the file, 4 MB).
    bool shouldCompact() const;

    // ---- writing (append-only; docs/emod-file-format.md "Batches and commits")
    // Columns are full-stride (bands() * 8192 bytes). A rewritten column keeps its ord; a new one
    // takes SUMMARY.next_ord. Singletons: WORLD_HEADER and SUMMARY are stored raw, the rest
    // compressed with the store's codec. Nothing reaches the file until commitBatch().
    void beginBatch();
    bool putColumn(int32_t x, int32_t z, const uint8_t* column);
    bool putRecord(int type, const void* data, size_t len);
    size_t batchRecords() const { return batchRecs_.size(); }
    // Truncates to the truncation point, appends the batch + SUMMARY + COMMIT in ONE write,
    // flushes and fsyncs. On failure the file is truncated back to the last commit and the
    // in-memory state is unchanged (the caller keeps its modified flags: today's "save skipped,
    // flags survive" contract).
    bool commitBatch();
    void abortBatch() { batch_.clear(); batchRecs_.clear(); }
    void setCodec(int codec) { codec_ = codec; }

    // Rewrites the live state as one batch into `path`.compact, fsyncs, renames over `path` and
    // reopens. Refused when damaged() (the bad bytes must never be the only copy destroyed),
    // when read-only, or mid-batch. zstd payloads are copied verbatim after a successful decode;
    // other codecs are re-encoded with the store's codec. Order: WORLD_HEADER, CREATURES,
    // SIGN_TRAILER (if non-empty), PROVENANCE, COLUMNs by ord, SUMMARY, COMMIT — conversion's
    // order, so compacting a fresh conversion reproduces it byte for byte.
    bool compact();

    // The full semantic check `emod.py verify` makes: every live record decodes, the invariants
    // hold, SUMMARY is exact. Returns damage + errors (empty == clean). Sets damaged() when it
    // finds a live record that does not decode.
    std::vector<std::string> verify();

    // SHA-256 of the live state, hex — identical to `emod.py digest` (state_digest_store).
    std::string stateDigest();

    // ---- test hooks (the crash-injection gate). Not for game code.
    // commitBatch() writes only the first `n` bytes of the batch, flushes, and returns false
    // WITHOUT truncating back — a process killed mid-write. SIZE_MAX disables.
    size_t testCrashAfter;
    // ... and, when true, extends the file with zeros to the batch's full length (a filesystem
    // that persisted the size but not the data).
    bool testCrashZeroFill;

private:
    struct Rec {               // a framed record
        uint64_t off, end;
        uint32_t aux, plen, seq, crc;
        int32_t x, z;
        uint16_t mask;
        uint8_t type, codec;
        bool padOk;
    };
    struct Entry {             // an applied record (history), newest last per key
        uint64_t off;          // record start
        uint32_t aux, plen;
        int32_t x, z;
        int32_t prev;          // previous applied version of the same key, -1 if none
        uint16_t mask;
        uint8_t type, codec;
    };
    static uint64_t colKey(int32_t x, int32_t z) { return ((uint64_t)(uint32_t)x << 32) | (uint32_t)z; }

    bool fail(const std::string& e) { error_ = e; return false; }
    bool readAt(uint64_t off, void* dst, size_t n);
    bool frame(uint64_t p, Rec* r, std::vector<uint8_t>& scratch);
    void scan();
    void evaluate();
    void apply(const Rec& r);
    bool decodeEntry(const Entry& e, std::vector<uint8_t>& out);
    int decodeNewest(int32_t idx, std::vector<uint8_t>& out, int* back);
    void addRecord(uint8_t type, uint8_t codec, uint16_t mask, int32_t x, int32_t z, uint32_t aux,
                   const uint8_t* data, size_t n);
    bool fsyncFile();
    bool truncateTo(uint64_t n);

    FILE* f_;
    std::string path_, error_, finalPath_;
    bool writable_, checkCrc_, creating_;
    int bands_;
    uint64_t created_, size_;
    std::vector<Rec> recs_;                                    // scan output (cleared after open)
    std::vector<std::pair<uint64_t, uint64_t> > spans_;        // damaged spans [p, q)
    std::vector<Entry> entries_;
    std::unordered_map<uint64_t, int32_t> colIndex_;           // key -> newest entry
    int32_t latestT_[256];
    std::vector<std::string> damage_, warnings_, errors_;
    bool torn_;
    uint64_t tailBytes_, truncateAt_, lastCommitOff_;
    uint32_t lastSeq_;
    uint32_t maxOrd_; bool anyOrd_;
    uint32_t bandCount_[16];
    uint64_t liveRecordBytes_;
    double openMs_;
    int codec_;
    Codecs codecs_;
    // the open batch
    struct Pending { uint64_t rel; uint8_t type, codec; uint16_t mask; int32_t x, z; uint32_t aux, plen, crc; };
    std::vector<uint8_t> batch_;
    std::vector<Pending> batchRecs_;
    std::vector<uint8_t> scratch_, scratch2_, payloadBuf_;
};

// ---- the streaming converter ------------------------------------------------------------------
class EdenEmodConverter {
public:
    enum Err {
        OK = 0,
        E_LEGACY,          // version field outside 1..1000: a 1.x file (the engine's own upgrade path)
        E_NEWER,           // version > 6
        E_DIR_OFFSET,      // directory_offset < 192, or past the end of the stream
        E_SHORT,           // fewer than 192 bytes
        E_LAYOUT,          // creature block would start inside the header
        E_DIRECTORY_SIZE,  // directory region over 256 MiB (a garbage header; not a world)
        E_IO,              // spill / output write failed (disk full lands here)
        E_CAP,             // the temp-bytes cap was exceeded (S.5's 500 MB ceiling)
        E_VERIFY,          // the written file did not read back as the source (a bug: report it)
        E_STATE,           // feed() after finish()/failure
    };
    struct Options {
        int codec;                 // C_ZSTD (the live default), C_ZLIB, C_NONE
        int64_t createdUnix;       // FileHeader.created_unix and PROVENANCE.source_mtime
        std::string sourceName;    // PROVENANCE's file name (UTF-8, no path)
        uint64_t maxTempBytes;     // spill + output cap; 0 = none
        // S.5e: a 64z source becomes a 256z `.emod` (bands 0-3 verbatim, 4-15 air; header version
        // < 5 -> 5, a v3 header getting the loader's v3 -> v4 fields first; creature block padded
        // with empty slots to 400; PROVENANCE flag 256). Policy, not format: off, the output is
        // emod.py's byte for byte. A 256z source is unaffected either way.
        bool upgradeTo256z;
        Options() : codec(C_ZSTD), createdUnix(0), maxTempBytes(0), upgradeTo256z(false) {}
    };
    struct Stats {
        uint64_t bytesIn, spillBytes, outBytes, columns, copied, rebuilt, slots;
        uint64_t peakHeldBytes;    // slot buffer + directory + slot index + decode cache (no zstd ctx)
        uint32_t creatureSlots, trailerBytes, flags;
        int bandsIn, bandsOut;     // the source's height and the `.emod`'s (S.5e: 4 -> 16 when upgraded)
    };

    EdenEmodConverter();
    ~EdenEmodConverter();
    // `outPath` is the final `.emod`; temps are `outPath`.spill and `outPath`.converting.
    bool begin(const char* outPath, const Options& opt);
    bool feed(const void* data, size_t n);
    // Parses the directory, writes `.converting`, re-reads and compares every column, fsyncs,
    // renames to `outPath`, deletes the spill. On failure every temp is deleted.
    bool finish();
    void abort();
    Err err() const { return err_; }
    const std::string& error() const { return error_; }
    const Stats& stats() const { return st_; }

private:
    struct Slot { uint64_t spillOff; uint32_t stored, len, crc; uint16_t mask; uint8_t codec; };
    bool fail(Err e, const std::string& msg);
    bool spillSlot();
    bool slotRaw(uint64_t k, const uint8_t** raw, uint32_t* len);
    bool readSource(uint64_t o, uint64_t n, std::vector<uint8_t>& out);
    void hold();

    Options opt_;
    std::string out_, spillPath_, convPath_;
    FILE* spill_;
    Err err_;
    std::string error_;
    bool begun_, done_;
    uint8_t hdr_[EDEN_HEADER];
    uint32_t hdrFill_;
    int bands_;
    uint64_t col_, dirOff_, pos_;
    std::vector<uint8_t> slot_, dir_;
    std::vector<Slot> slots_;
    std::vector<uint8_t> payload_, enc_;
    // two-entry decoded-slot cache for finish()
    uint64_t cacheK_[2]; std::vector<uint8_t> cache_[2]; int cacheNext_;
    Codecs codecs_;
    Stats st_;
};

}  // namespace emod

#endif
