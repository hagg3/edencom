// eden_fs.h — G.2b-0 (WORKING/de-objc-plan-2026-10-24.md D5/D6). Plain C++ file I/O for the engine
// once its Foundation is gone: EdenFile (the NSFileHandle replacement), eden_fs_* (the NSFileManager
// subset) and eden_bundle_path (NSBundle's pathForResource).
//
// Semantics follow APPLE's, which is what shipped: opening a MISSING file for reading, writing or
// updating returns null (create it first with eden_fs_create_empty); a bundle miss returns "". EdenFile
// makes NO backups: the engine's eden_save_backup_hook (top of FileManager::saveWorld) is the only .bak
// mechanism. Every write is followed by fflush (the shim's long-standing behaviour). Offsets are 64-bit.
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
