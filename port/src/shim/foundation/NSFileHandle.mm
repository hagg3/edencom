#include "platform_shims.h"  // eden_platform_now_ms — B1 read-path I/O timing (see below)
#include <unistd.h>   // ftruncate — used by -truncateFileAtOffset: (not on Windows; see below)
#if defined(_WIN32)
#include <io.h>       // _chsize_s — the mingw CRT has no ftruncate at all (Phase N Stage 3)
#endif
#include <cstdio>     // rename, remove — used by the backup-slot copy below
#include "save_backup.h"   // the portable rules, shared with the native target
#include <cstring>    // strdup — used by the deferred-backup path (_backupPathC)
#include <cstdlib>    // free — used by the deferred-backup path (_backupPathC)

#import "NSFileHandle.h"
#import "NSString.h"
#import "NSData.h"
#include "Constants.h"   // g_save_inplace_threshold — the shared save-strategy threshold

// B1 (ROADMAP Phase B): the column-read/RLE-decode burst characterization needs the file-I/O half
// of FileManager::readColumn measured separately from the RLE decode. Every default-world column
// read bottoms out in the fread() below (via NSData readDataOfLength:), whether it is served from
// the lazy Eden.eden node's block cache or triggers a synchronous XHR/readSync range fetch. Report
// each fread's wall time to MeshTiming_web.mm's accumulator; it is weak so a build that excludes
// that TU (none today) still links.
//
// B6: that comment used to end "two emscripten_get_now() calls per column read = noise", and it
// was wrong on both counts. At the time -readDataOfLength: was called EIGHT times per bundled-map
// column (a 2-byte length prefix plus a payload, for each of the 4 RLE bands), so a column cost 16
// emscripten_get_now() calls -- each one a wasm->JS boundary crossing -- not 2. (B6 also fixed the
// caller: fmh_readColumnRawFromDefault now reads the record in ONE call.) And it was not noise:
// this probe is deliberately not EDEN_DIAGNOSTICS-gated (see MeshTiming_web.mm's header), so
// build-rel, the build players actually run, paid it on every read of the world file. Worse for
// the row that found it, the overhead sat INSIDE the thing B6 had to measure, so an unmodified
// before/after would have credited the fix with removing its own instrumentation.
//
// So the timing is now opt-in at runtime and OFF by default: a probe that wants the B1 split calls
// eden_debug_set_io_timing(1) first and accepts the distortion knowingly. Everything else -- every
// shipped build, and every before/after that is not specifically about fread cost -- pays one
// predictable branch on a static bool. The read counters stay free either way.
extern "C" __attribute__((weak)) void eden_mt_note_io(double ms);

static bool g_io_timing = false;

// Exported so tools/headless-mesh-burst-probe.js --io-timing (and any future probe) can turn the
// B1 split back on for a window. KEEPALIVE, and not diagnostics-gated, for the same reason
// MeshTiming_web.mm's exports are not: the interesting measurement is of build-rel.
// EDEN_EXPORT, not EMSCRIPTEN_KEEPALIVE (Phase N Stage 3.2). This file is compiled by the Linux
// and Windows targets too now, and <emscripten/emscripten.h> does not exist there. platform_shims.h
// — already included at the top for eden_platform_now_ms — defines EDEN_EXPORT as the keepalive on
// Emscripten and as nothing elsewhere, which is the rule web/CLAUDE.md states for any seam or shim
// file both targets compile.
extern "C" EDEN_EXPORT void eden_debug_set_io_timing(int on) { g_io_timing = (on != 0); }

@implementation NSFileHandle

+ (NSFileHandle *)fileHandleForReadingAtPath:(NSString *)path {
    FILE *fp = fopen([path UTF8String], "rb");
    if (!fp) return nil;
    NSFileHandle *fh = [[NSFileHandle alloc] init];
    fh->_fp = fp;
    return [fh autorelease];
}

// Perf-audit C4 ("no backup slot"): .eden is append-only with its ColumnIndex directory at the
// END, read to EOF — a save interrupted mid-write (tab discard, OOM, crash) leaves a file whose
// tail is not a valid index, and the engine has no fallback for that. Before this handle starts
// overwriting an EXISTING file, copy its last-known-good bytes to "<path>.bak" (best-effort,
// silently skipped on any error — a missing backup is the status quo, not a new failure mode).
// This does not make the write itself atomic (that needs temp+rename, which the incremental
// seek/write/truncate call pattern FileManager.mm uses does not map onto cleanly without editing
// Classes/), but it means a corrupted save no longer destroys the ONLY copy of the world.
// Phase N Stage 2: the BODY of this moved to save_backup.cpp, unchanged, so the native target can
// run the identical rules from its own hook (real Foundation's NSFileHandle is a class cluster
// this shim does not own — see save_backup.h). This wrapper stays because the shim's call sites
// pass NSString and because the deferred-until-first-write behaviour below is shim-specific.
static void eden_backup_before_overwrite(NSString *path) {
    eden_save_backup_before_overwrite([path UTF8String]);
}

+ (NSFileHandle *)fileHandleForWritingAtPath:(NSString *)path {
    eden_backup_before_overwrite(path);
    FILE *fp = fopen([path UTF8String], "r+b"); // matches original's real device fix (audit H1:
                                                  // "rw"->"r+") rather than the pre-fix "rw" mode.
    if (!fp) fp = fopen([path UTF8String], "w+b"); // create if missing
    if (!fp) return nil;
    NSFileHandle *fh = [[NSFileHandle alloc] init];
    fh->_fp = fp;
    return [fh autorelease];
}

// Found chasing the web port's load-failure recovery UI (LoadFailure_web.mm): FileManager::
// loadWorld()'s header/directory sanity check opens the REAL save via fileHandleForUpdatingAtPath:
// purely to READ it -- it never writes through this handle in the failure path. Before this fix,
// -fileHandleForUpdatingAtPath: was a bare alias for -fileHandleForWritingAtPath:, so that read-only
// open ALSO fired the eager backup-before-overwrite -- meaning the mere act of attempting to load a
// truncated/corrupt save copied the corrupt bytes over the last-known-good ".bak", destroying the
// only thing the recovery dialog's Restore button could recover, before the length check a few
// lines later even ran. (headless-confirmed: eden_load_restore_backup() returned success but the
// restored file was byte-identical to the corrupt 50-byte input, not the original.) Fix: defer the
// backup to the first actual WRITE through an "updating" handle (-writeData:/-truncateFileAtOffset:
// below) instead of firing it at open -- a pure read-then-close never triggers it, but the atomic-
// rename save path's real writes into ITS `.savetmp` scratch copy (opened via this same call) still
// get one, same as before.
+ (NSFileHandle *)fileHandleForUpdatingAtPath:(NSString *)path {
    FILE *fp = fopen([path UTF8String], "r+b");
    if (!fp) fp = fopen([path UTF8String], "w+b"); // create if missing
    if (!fp) return nil;
    NSFileHandle *fh = [[NSFileHandle alloc] init];
    fh->_fp = fp;
    fh->_backupPathC = strdup([path UTF8String]);
    return [fh autorelease];
}

// Fires the deferred backup (see -fileHandleForUpdatingAtPath: above) the first time this handle
// is actually used to modify the file, then clears _backupPathC so it never fires twice.
static void eden_fire_deferred_backup(NSFileHandle *fh) {
    if (!fh->_backupPathC) return;
    eden_backup_before_overwrite([NSString stringWithCString:fh->_backupPathC encoding:NSUTF8StringEncoding]);
    free(fh->_backupPathC);
    fh->_backupPathC = nullptr;
}

- (void)seekToFileOffset:(eden_offset_t)offset {
    if (_fp) eden_fseek64(_fp, offset, SEEK_SET);
}

- (eden_offset_t)seekToEndOfFile {
    if (_fp) { eden_fseek64(_fp, 0, SEEK_END); return (eden_offset_t)eden_ftell64(_fp); }
    return 0;
}

- (eden_offset_t)offsetInFile {
    return _fp ? (eden_offset_t)eden_ftell64(_fp) : 0;
}

- (NSData *)readDataOfLength:(NSUInteger)length {
    if (!_fp || length == 0) return [NSData data];
    NSMutableData *d = [NSMutableData dataWithCapacity:length];
    // B6: -setLength: has to zero what it grows into, because that is what real Foundation does
    // and callers elsewhere may lean on it. Here the very next statement overwrites every one of
    // those bytes with fread(), so the fill is pure waste -- and this is the world file's read
    // path, called for every column of every chunk-streaming burst. -setLengthUninitialized: is
    // the shim-only escape hatch for exactly this shape: grow, then immediately fill.
    [d setLengthUninitialized:length];
    size_t got;
    if (g_io_timing && eden_mt_note_io) {
        double _t0 = eden_platform_now_ms();
        got = fread(d->_bytes.data(), 1, length, _fp);
        eden_mt_note_io(eden_platform_now_ms() - _t0);
    } else {
        got = fread(d->_bytes.data(), 1, length, _fp);
    }
    [d setLength:(NSUInteger)got];   // shrink only; nothing to initialise
    return d;
}

- (NSData *)readDataToEndOfFile {
    if (!_fp) return [NSData data];
    // long long, not long: this is a file offset and `long` is 32 bits on Windows.
    long long cur = eden_ftell64(_fp);
    eden_fseek64(_fp, 0, SEEK_END);
    long long end = eden_ftell64(_fp);
    eden_fseek64(_fp, cur, SEEK_SET);
    return [self readDataOfLength:(NSUInteger)(end - cur)];
}

- (void)writeData:(NSData *)data {
    // Perf-audit C4: this is stdio-BUFFERED, and eden-storage.js's flushNow() (FS.syncfs on
    // visibilitychange/pagehide) reads through MEMFS, not through this FILE*'s userspace buffer —
    // so without an explicit flush, a save interrupted before -closeFile leaves MEMFS (and
    // therefore whatever IndexedDB sync follows) holding a TRUNCATED file, even though the
    // in-process bytes were "written" from FileManager.mm's point of view. Flushing after every
    // write makes MEMFS's view of the file match what the engine believes it just wrote, at the
    // cost of a write() syscall per NSFileHandle -writeData: call (already the granularity
    // FileManager.mm calls this at — one per 32 KB column record — so this is not a new
    // per-byte cost).
    if (_fp && data) {
        eden_fire_deferred_backup(self);
        fwrite([data bytes], 1, [data length], _fp); fflush(_fp);
    }
}

// SHRINKING THE FILE IS PART OF THE SAVE, not housekeeping: the in-place strategy rewrites a world
// that may be shorter than the one on disk, and a tail left behind is trailing garbage after the
// ColumnIndex directory that the loader reads to EOF.
//
// The mingw CRT has no ftruncate — not a differently-spelled one, none — so Windows uses the MS
// CRT's _chsize_s, which takes the same fd and a 64-bit length and is the documented equivalent.
// (_chsize is the other spelling and its length is a 32-bit `long`, i.e. it would silently
// mis-truncate any world over 2 GB. This port has 256z worlds; use the _s form.)
- (void)truncateFileAtOffset:(eden_offset_t)offset {
    if (!_fp) return;
    eden_fire_deferred_backup(self);
    fflush(_fp);
#if defined(_WIN32)
    _chsize_s(_fileno(_fp), (long long)offset);
#else
    ftruncate(fileno(_fp), (off_t)offset);
#endif
}

- (void)closeFile {
    if (_fp) { fclose(_fp); _fp = nullptr; }
}

- (void)dealloc {
    [self closeFile];
    free(_backupPathC);
    [super dealloc];
}

@end
