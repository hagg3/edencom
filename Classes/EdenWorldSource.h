//
//  EdenWorldSource.h
//  Eden — Stage S / S.5: the bytes of a `.eden`, however the player left them on disk.
//
//  A forward-only reader that yields the RAW `.eden` byte stream from a plain `.eden`, a gzip
//  (`.eden.gz`, `.gz`) or a single-entry-or-more zip (`.zip`, `.eden.zip`): the first `.eden` entry,
//  else the first file. It is what feeds emod::EdenEmodConverter, so an archive is converted WHILE
//  it inflates and no inflated `.eden` is ever written (plan §4 "Downloads and imports").
//
//  Plain C++ and zlib only, like EdenWorldStore. Not thread-safe; one owner.
//
#ifndef Eden_EdenWorldSource_h
#define Eden_EdenWorldSource_h

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include <zlib.h>
#include "EdenWorldStore.h"

namespace emod {

class WorldSource {
public:
    enum Kind { K_RAW = 0, K_GZIP, K_ZIP };
    WorldSource();
    ~WorldSource();

    // A file name the world list should treat as a possible archive (case-insensitive `.gz`/`.zip`).
    static bool isArchiveName(const std::string& name);
    // The `.emod` stem for a source name: "w.eden.gz" / "w.eden.zip" / "w.gz" / "w.zip" / "w.eden" -> "w".
    static std::string stemOf(const std::string& name);

    // Detects the kind from the file's magic (not its name) and positions at the first byte.
    bool open(const std::string& path);
    void close();
    // Up to `n` bytes of the raw `.eden`. Returns the number produced; fewer than `n` only at the
    // end of the stream (check failed() to tell a clean end from an error).
    size_t read(void* dst, size_t n);
    bool eof() const { return eof_; }
    bool failed() const { return !error_.empty(); }
    const std::string& error() const { return error_; }
    Kind kind() const { return kind_; }
    // Progress through the INPUT (compressed) bytes: the only honest measure, since the inflated size
    // is unknown up front (gzip's ISIZE wraps at 4 GiB).
    uint64_t inConsumed() const { return inTotal_ - inLeft_; }
    uint64_t inTotal() const { return inTotal_; }

private:
    bool fail(const char* m) { if (error_.empty()) error_ = m; return false; }
    bool zipEntry(uint64_t* dataStart, uint64_t* compSize, bool* stored);
    bool nextIsGzip();

    FILE* in_;
    Kind kind_;
    bool stored_, zon_, eof_;
    z_stream zs_;
    uint64_t inLeft_, inTotal_;
    std::string error_;
    unsigned char hold_[1 << 16];
};


// One `.eden` / `.gz` / `.zip` -> `.emod` conversion, a time slice per step(): the single path every
// caller takes (FileManager's convert-on-play, native Get Worlds, the web Storage tab's import), so
// the pre-flight, the running free-space floor and the temp cap are the same everywhere (plan §4
// *Downloads and imports*). A nested archive (the community archive's zip-in-a-zip) is unpacked one
// layer at a time to `<out>.layer` — small: it is still compressed — and the innermost stream is
// converted while it inflates, so no inflated `.eden` is ever written.
class ImportJob {
public:
    enum { RUNNING = 0, DONE = 1, FAILED = -1 };
    // Free space the pre-flight asks for: a plain `.eden` ~2 x its columns at the worst measured
    // rate; an archive ARCHIVE_FACTOR x its compressed size; + 64 MB, at most the temp cap + 64 MB.
    // Measured over the 752 convertible worlds of the community archive (s5-results-2026-10-10.md):
    // the `.emod` is 1.43x the zip (median; 11.4x max) and the peak temp (spill + output) reaches
    // 6.33x the zip for worlds over 1 MB, so the plan's 4x was too LOW, not too high.
    static const unsigned ARCHIVE_FACTOR = 6;
    static uint64_t needFor(uint64_t srcBytes, bool archive);
    // The 500 MB ceiling on spill + output (+ any layer); EDEN_TEST_TEMP_CAP=<bytes> overrides it.
    static uint64_t tempCap();
    static const uint64_t FREE_FLOOR = 32ull << 20;   // abort when the volume drops below this

    ImportJob();
    ~ImportJob();
    // False with error() when it cannot start: unreadable, or the pre-flight refused (nothing written).
    // `upgradeTo256z` (S.5e): a 64z source lands as a 256z `.emod` (EdenEmodConverter::Options).
    bool begin(const std::string& srcPath, const std::string& outEmod, const std::string& sourceName, int64_t createdUnix,
               bool upgradeTo256z = false);
    int step(int budgetMs);
    int percent() const;
    const std::string& error() const { return error_; }
    bool lowSpace() const { return lowSpace_; }        // the failure was the pre-flight / the floor / the cap
    uint64_t need() const { return need_; }
    uint64_t peakTempBytes() const { return peakTemp_; }
    const EdenEmodConverter::Stats& stats() const { return conv_.stats(); }
    void abort();

private:
    bool fail(const std::string& e, bool space = false);
    std::string src_, out_, name_, layer_, prevLayer_, dir_;
    int64_t created_;
    bool upgrade_;
    WorldSource in_;
    EdenEmodConverter conv_;
    FILE* layerF_;
    bool decide_, layering_, started_, lowSpace_;
    int depth_;
    uint64_t need_, fed_, nextCheck_, layerBytes_, peakTemp_;
    std::string error_;
    std::vector<uint8_t> buf_;
};

}  // namespace emod

#endif
