// eden_fs.h — G.2b-0 (WORKING/de-objc-plan-2026-10-24.md D5/D6). Plain C++ file I/O for the engine
// once its Foundation is gone: EdenFile (the NSFileHandle replacement), eden_fs_* (the NSFileManager
// subset) and eden_bundle_path (NSBundle's pathForResource).
//
// Semantics follow APPLE's, which is what shipped: opening a MISSING file for reading, writing or
// updating returns null (create it first with eden_fs_create_empty); a bundle miss returns "". EdenFile
// makes NO backups. Every write is followed by fflush (the shim's long-standing behaviour). Offsets are 64-bit.
#ifndef EDEN_FS_H
#define EDEN_FS_H

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

class EdenFile {
public:
    static EdenFile* openRead(const std::string& path);     // "rb"; null if missing
    static EdenFile* openUpdate(const std::string& path);   // "r+b"; null if missing
    static EdenFile* openWrite(const std::string& path);    // "r+b" (no truncation, as NSFileHandle); null if missing
    ~EdenFile();                                            // closes

    bool seek(long long offset);                            // from start
    long long seekEnd();                                    // to EOF; returns the offset, -1 on error
    long long tell() const;                                 // -1 on error
    std::vector<uint8_t> read(size_t n);                    // shorter at EOF
    size_t read(void* dst, size_t n);                       // bytes read
    bool write(const void* src, size_t n);                  // + fflush
    bool truncate(long long length);                        // fflush, then ftruncate/_chsize_s
    void close();

private:
    explicit EdenFile(FILE* fp) : fp_(fp) {}
    EdenFile(const EdenFile&);
    EdenFile& operator=(const EdenFile&);
    FILE* fp_;
};

bool eden_fs_exists(const std::string& path);
bool eden_fs_is_dir(const std::string& path);
bool eden_fs_remove(const std::string& path);                       // file or empty directory
bool eden_fs_move(const std::string& from, const std::string& to);  // replaces an existing target
// K.0w-1 (world-folders-plan §5.1). Moves a file or directory and NEVER replaces: false if `to` exists,
// and no copy fallback (a world moves by one rename on one volume or not at all).
bool eden_fs_move_new(const std::string& from, const std::string& to);
bool eden_fs_make_dirs(const std::string& path);                    // mkdir -p; true if it now is a directory
bool eden_fs_remove_tree(const std::string& path);                  // rm -rf (callers guard the path: FileManager::removeWorldDir)
long long eden_fs_mtime(const std::string& path);                   // seconds since the epoch, -1 if missing
bool eden_fs_sync_dir(const std::string& dir);                      // fsync the directory entry (no-op true on Windows)
// QA.17 (A2-08): small-file durability. A file the app rewrites whole (prefs, servers.txt, mods.json, a
// finished download) goes through these, never fopen "wb" over the real name or remove-then-rename:
//   eden_fs_sync_file      fflush + fsync (_commit on Windows); false on either failing.
//   eden_fs_replace_atomic renames `from` over `to` in one step (POSIX rename + parent-dir fsync;
//                          Windows MoveFileExW REPLACE_EXISTING | WRITE_THROUGH: no remove-first gap).
//                          The body of emod::io::replace_file (EdenWorldStore.cpp), which stays the store's own.
//   eden_fs_write_atomic   writes `<path>.tmp`, checks every write + fsync + close, then replaces. On any
//                          failure the tmp is removed and `path` is left exactly as it was.
bool eden_fs_sync_file(FILE* f);
bool eden_fs_replace_atomic(const std::string& from, const std::string& to);
bool eden_fs_write_atomic(const std::string& path, const void* data, size_t n);
bool eden_fs_copy(const std::string& from, const std::string& to);
bool eden_fs_create_empty(const std::string& path);                 // truncates an existing file
bool eden_fs_make_dir(const std::string& path);                     // one level; true if it now exists
long long eden_fs_size(const std::string& path);                    // -1 if missing
std::vector<std::string> eden_fs_list(const std::string& dir);      // entry names, sorted, no . or ..

// A flat lookup under eden_platform_bundle_root(): "<root>/<name>", else "<root>/media/<name>" found by
// basename anywhere below media/ (first hit wins, as NSBundle's index does). "" on a miss.
std::string eden_bundle_path(const std::string& name);
// NSString's isAbsolutePath as the shim answers it: "/..." everywhere; on Windows also "C:\..." / "C:/..."
// and "\\..." (X.11: a Documents root is a drive path there). G.2b-1: Texture2D's path loads.
bool eden_path_is_absolute(const std::string& path);

#endif
