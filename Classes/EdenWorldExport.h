//
//  EdenWorldExport.h
//  Eden — Stage S / S.5 + S.5b: a world as a vanilla `.eden` byte stream, in a chosen format era.
//
//  Plan: WORKING/emod-format-implementation-plan-2026-10-03.md §4 *Vanilla export* and *Format
//  eras, height detection, and target-format export*; spec: docs/emod-file-format.md § Export.
//
//  Standalone like EdenWorldStore: C++ and zlib only, no ObjC, no engine globals. The source is an
//  `.emod` (through EdenWorldStore) or a `.eden` (read through the converter's own layout parser,
//  so short spans and odd layouts read exactly as the engine reads them). The output is PULLED —
//  read() hands out the next bytes of the `.eden`, and GzipPump the next bytes of its gzip — so one
//  exporter serves a file (export_world_file), a browser download chunk by chunk (web Storage tab)
//  and an upload body (S.5c), and nothing ever materialises a raw `.eden` it does not need to.
//
//  The header is computable before the first byte, so begin() is the exact pre-flight: the report
//  gives the output's size to the byte and every loss the target implies, before anything is
//  written. For a target that changes blocks (Legacy64z, NewDawn256z) that costs one decode of
//  every column; the others decode nothing until read().
//
//  Targets (plan §4 table):
//    OWN            the world's own format; for an engine-written source with no short spans and
//                   no dead records the output is byte-identical to the original `.eden`
//    LEGACY64Z      bands 0-3, FileManager::convertWorldTo64's cut rules, version 4 (a 64z source
//                   keeps its own version); trailer dropped; 112-127 -> painted stone
//    NEWDAWN256Z    16 bands (a 64z source pads with air), version 5, trailer dropped, 112-127 -> painted stone
//    NEWFORMAT256Z  16 bands, ids kept, trailer kept (or pruned), version: the source's if it is
//                   256z, else 5
//  Signs: KEEP (what the game does) or PRUNE — drop sign / command-block records whose (x, y)
//  block position is in a column the world does not contain (a world captured from a hosted
//  server carries the whole server's).
//
#ifndef Eden_EdenWorldExport_h
#define Eden_EdenWorldExport_h

#include "EdenWorldStore.h"
#include <zlib.h>

namespace emod {

enum ExportTarget { X_OWN = 0, X_LEGACY64Z = 1, X_NEWDAWN256Z = 2, X_NEWFORMAT256Z = 3 };
enum SignPolicy { SIGNS_KEEP = 0, SIGNS_PRUNE = 1 };
const char* export_target_name(int target);   // "own", "Legacy64z", "NewDawn256z", "NewFormat256z"
int export_target_from_name(const char* s);    // -1 if unknown

// The one substitution table for ids a pre-2026 client does not know (ROADMAP D.2b refines it).
uint8_t legacy_block_substitute(uint8_t type);
uint8_t legacy_block_substitute_paint(uint8_t type);   // the paint index that goes with it (0 = n/a)

struct ExportOptions {
    int target;                // ExportTarget
    int signs;                 // SignPolicy
    bool overrideHash;         // S.5c: the preview's MD5 goes in WorldFileHeader::hash
    char hash[36];
    ExportOptions() : target(X_OWN), signs(SIGNS_KEEP), overrideHash(false) { hash[0] = 0; }
};

struct ExportReport {
    int target, bandsIn, bandsOut;
    int32_t versionIn, versionOut;
    uint64_t bytes;                         // the raw `.eden`'s exact size
    uint32_t columns, creatureSlotsIn, creatureSlotsOut;
    bool scanned;                           // the columns were decoded for the counts below
    uint64_t blocksAbove63;                 // non-air blocks cut by a 256z -> 64z export
    uint32_t columnsAffected, doorsOrphaned;
    uint64_t newBlocksReplaced;             // ids 112-127 written as their substitute
    uint32_t newBlockColumns;
    uint32_t trailerBytesIn, trailerBytesOut;
    bool trailerParsed;                     // the trailer's sections were understood (counts valid)
    uint32_t signsIn, signsOut, cmdsIn, cmdsOut, otherIn, otherOut;
    uint32_t creaturesDropped, creaturesRelocated, creaturesOverflow;
    bool posClamped, homeClamped;
    bool lossy() const;
    std::string json() const;
    std::string summary() const;            // one or two plain sentences for a dialog body
};

class EdenExporter {
public:
    EdenExporter();
    ~EdenExporter();
    // Opens `srcPath` (`.emod` by name, else a `.eden`) and runs the pre-flight. False: error().
    bool begin(const std::string& srcPath, const ExportOptions& opt);
    const ExportReport& report() const { return rep_; }
    // The next up-to-`n` bytes of the `.eden`; 0 at the end or on failure (failed()).
    size_t read(void* dst, size_t n);
    bool done() const { return phase_ == P_END; }
    bool failed() const { return !error_.empty(); }
    const std::string& error() const { return error_; }
    uint64_t produced() const { return produced_; }
    void close();

private:
    enum Phase { P_HEADER, P_COLUMNS, P_CREATURES, P_DIRECTORY, P_TRAILER, P_END };
    bool fail(const std::string& e) { if (error_.empty()) error_ = e; return false; }
    bool readSourceColumn(size_t ord, uint8_t* out);
    void transform(const uint8_t* in, uint8_t* out, ExportReport* count);
    bool fillPhase();

    ExportOptions opt_;
    ExportReport rep_;
    std::string error_;
    bool emod_;
    EdenWorldStore store_;
    FILE* f_;
    EdenLayout lay_;
    std::vector<std::pair<int32_t, int32_t> > keys_;   // ord order
    std::vector<uint8_t> header_, creatures_, trailer_, dir_;
    std::vector<uint8_t> inCol_, outCol_;
    Phase phase_;
    size_t ord_;
    const uint8_t* cur_;
    size_t curLeft_;
    uint64_t produced_;
};

// The `.eden` as one gzip member (deflate level 6; header mtime 0, OS 255, so the same world
// gzips to the same bytes on every target). Stock 2.1.1's upload body and the web's "Compressed"
// export have this shape.
class GzipPump {
public:
    GzipPump();
    ~GzipPump();
    bool begin(EdenExporter* src, int level = 6);
    size_t read(void* dst, size_t n);
    bool done() const { return done_; }
    bool failed() const { return !error_.empty(); }
    const std::string& error() const { return error_; }
    uint64_t rawIn() const { return in_; }
private:
    EdenExporter* src_;
    z_stream zs_;
    bool on_, done_, srcEnd_;
    std::string error_;
    std::vector<uint8_t> buf_;
    uint64_t in_;
};

// Free bytes on the volume holding `dir` (statvfs / GetDiskFreeSpaceExW). UINT64_MAX when it
// cannot tell. EDEN_TEST_FREE_BYTES=<n> or <first>,<later> overrides it (the S.5 low-disk gates' test hook).
uint64_t free_bytes(const std::string& dir);

// The file-writing half, a time slice per step() (the menu's export and upload run it a frame at a
// time): `outPath`.exporting, then fsync and an atomic rename. `prefix`/`suffix` wrap the stream
// (the upload body's multipart framing). Pre-flight before the first byte: raw needs the exact size
// + 1% + 16 MB free, gzip 2 x the source file + 1/500 of the raw size + 16 MB; rechecked every 64 MB
// written (abort below 16 MB). On any failure the partial file is deleted.
class ExportFileJob {
public:
    enum { RUNNING = 0, DONE = 1, FAILED = -1 };
    ExportFileJob();
    ~ExportFileJob();
    bool begin(const std::string& srcPath, const std::string& outPath, const ExportOptions& opt, bool gzip,
               const std::string& prefix = std::string(), const std::string& suffix = std::string());
    int step(int budgetMs);
    int percent() const;
    const ExportReport& report() const { return ex_.report(); }
    const std::string& error() const { return error_; }
    uint64_t written() const { return written_; }
    void abort();
private:
    bool fail(const std::string& e);
    EdenExporter ex_;
    GzipPump gz_;
    bool gzip_;
    FILE* o_;
    std::string out_, tmp_, dir_, suffix_, error_;
    std::vector<uint8_t> buf_;
    uint64_t written_, nextCheck_;
};

// Exports `srcPath` to `outPath` (`.eden`, or gzip when `gzip`): `outPath`.exporting, fsync,
// atomic rename. Refuses before writing when the volume has too little room (raw: the exact size
// + 1% + 16 MB; gzip: 2x the source file + 1/500 of the raw size + 16 MB, rechecked while
// writing). On any failure the partial file is deleted. `report` (may be null) gets the pre-flight.
bool export_world_file(const std::string& srcPath, const std::string& outPath, const ExportOptions& opt, bool gzip,
                       ExportReport* report, std::string* err);


// ---- S.5c: upload to the Current / Legacy servers --------------------------------------------
// The contract, pinned 2026-10-10 from stock 2.1.1's Classes/FileUpload.mm + zpipe.c (what the Legacy
// server's client sends) and the sibling editor's capture of the 2026 client (what the Current server
// gets; ~/eden-world-editor DOCUMENTATION/10-features.md): POST `upload2.php?uuid=<UUID>`,
// `multipart/form-data; boundary=0xasdfasdfasdfasdfasdf`, exactly two parts and no per-part
// Content-Type — `uploaded` (filename `file.bin`): the world as one gzip member; `uploaded2`
// (filename `image.bin`): the preview PNG. A real Content-Length (no chunked encoding). Success is
// the 3-byte body `YES`. The world's header carries the preview's MD5 (lowercase hex) in `hash`.
enum UploadServer { UPLOAD_CURRENT = 0, UPLOAD_LEGACY = 1 };
extern const char* const UPLOAD_BOUNDARY;
std::string upload_url(int server, const std::string& uuid);
std::string upload_content_type();
std::string md5_file_hex(const std::string& path);   // "" if unreadable
// Writes the request body to `bodyPath` (a temp the caller deletes). The Legacy server takes
// Legacy64z only: `opt.target` is forced to it. The preview's MD5 overrides the header hash.
// The same as a time-sliced ExportFileJob (the menu's upload): false + err when it cannot start.
bool begin_upload_body(ExportFileJob& job, int server, const std::string& srcPath, const std::string& pngPath,
                       ExportOptions opt, const std::string& bodyPath, std::string* err);
bool build_upload_body(int server, const std::string& srcPath, const std::string& pngPath, ExportOptions opt,
                       const std::string& bodyPath, ExportReport* report, std::string* err);

}  // namespace emod

#endif
