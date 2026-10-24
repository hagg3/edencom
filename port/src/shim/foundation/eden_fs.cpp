// eden_fs.cpp — G.2b-0. See eden_fs.h. The bodies of EdenFile are the fopen-backed NSFileHandle.mm
// ones (proven on Linux/Windows since Stage 3) minus the backups, with Apple's null-on-miss opens.
#include "eden_fs.h"
#include "platform_shims.h"   // eden_fseek64/eden_ftell64, eden_platform_bundle_root

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <unordered_map>
#if defined(_WIN32)
#include <io.h>      // _chsize_s, _fileno
#else
#include <unistd.h>  // ftruncate
#endif

namespace fs = std::filesystem;

static fs::path P(const std::string& s) { return fs::u8path(s); }

static FILE* open_existing(const std::string& path, const char* mode) {
    std::error_code ec;
    if (!fs::exists(P(path), ec) || fs::is_directory(P(path), ec)) return nullptr;   // fopen "r+b" on a dir can succeed on POSIX
#if defined(_WIN32)
    std::wstring wmode;
    for (const char* m = mode; *m; ++m) wmode.push_back((wchar_t)*m);
    return _wfopen(P(path).c_str(), wmode.c_str());
#else
    return fopen(path.c_str(), mode);
#endif
}

EdenFile* EdenFile::openRead(const std::string& path) {
    FILE* fp = open_existing(path, "rb");
    return fp ? new EdenFile(fp) : nullptr;
}
EdenFile* EdenFile::openUpdate(const std::string& path) {
    FILE* fp = open_existing(path, "r+b");
    return fp ? new EdenFile(fp) : nullptr;
}
EdenFile* EdenFile::openWrite(const std::string& path) { return openUpdate(path); }

EdenFile::~EdenFile() { close(); }
void EdenFile::close() { if (fp_) { fclose(fp_); fp_ = nullptr; } }

bool EdenFile::seek(long long offset) { return fp_ && eden_fseek64(fp_, offset, SEEK_SET) == 0; }
long long EdenFile::seekEnd() {
    if (!fp_ || eden_fseek64(fp_, 0, SEEK_END) != 0) return -1;
    return eden_ftell64(fp_);
}
long long EdenFile::tell() const { return fp_ ? eden_ftell64(fp_) : -1; }

size_t EdenFile::read(void* dst, size_t n) {
    return (fp_ && n) ? fread(dst, 1, n, fp_) : 0;
}
std::vector<uint8_t> EdenFile::read(size_t n) {
    std::vector<uint8_t> v(n);
    v.resize(read(v.data(), n));
    return v;
}
bool EdenFile::write(const void* src, size_t n) {
    if (!fp_) return false;
    if (n && fwrite(src, 1, n, fp_) != n) return false;
    return fflush(fp_) == 0;
}
bool EdenFile::truncate(long long length) {
    if (!fp_) return false;
    fflush(fp_);
#if defined(_WIN32)
    return _chsize_s(_fileno(fp_), length) == 0;      // _chsize's length is a 32-bit long: never that one
#else
    return ftruncate(fileno(fp_), (off_t)length) == 0;
#endif
}

// ---- eden_fs_* ----------------------------------------------------------------------------
bool eden_fs_exists(const std::string& path) { std::error_code ec; return fs::exists(P(path), ec); }
bool eden_fs_is_dir(const std::string& path) { std::error_code ec; return fs::is_directory(P(path), ec); }
bool eden_fs_remove(const std::string& path) { std::error_code ec; return fs::remove(P(path), ec) && !ec; }
bool eden_fs_move(const std::string& from, const std::string& to) {
    std::error_code ec;
    fs::rename(P(from), P(to), ec);                    // replaces an existing file on POSIX and (std) on Windows
    if (!ec) return true;
    ec.clear();
    fs::copy_file(P(from), P(to), fs::copy_options::overwrite_existing, ec);   // cross-device fallback
    if (ec) return false;
    fs::remove(P(from), ec);
    return true;
}
bool eden_fs_copy(const std::string& from, const std::string& to) {
    std::error_code ec;
    return fs::copy_file(P(from), P(to), fs::copy_options::overwrite_existing, ec) && !ec;
}
bool eden_fs_create_empty(const std::string& path) {
    FILE* fp;
#if defined(_WIN32)
    fp = _wfopen(P(path).c_str(), L"wb");
#else
    fp = fopen(path.c_str(), "wb");
#endif
    if (!fp) return false;
    fclose(fp);
    return true;
}
bool eden_fs_make_dir(const std::string& path) {
    std::error_code ec;
    fs::create_directory(P(path), ec);
    return fs::is_directory(P(path), ec);
}
long long eden_fs_size(const std::string& path) {
    std::error_code ec;
    auto n = fs::file_size(P(path), ec);
    return ec ? -1 : (long long)n;
}
std::vector<std::string> eden_fs_list(const std::string& dir) {
    std::vector<std::string> out;
    std::error_code ec;
    for (fs::directory_iterator it(P(dir), ec), end; !ec && it != end; it.increment(ec))
        out.push_back(it->path().filename().u8string());
    std::sort(out.begin(), out.end());
    return out;
}

// ---- eden_bundle_path ---------------------------------------------------------------------
static std::unordered_map<std::string, std::string>& media_index() {
    static std::unordered_map<std::string, std::string> index;
    static bool built = false;
    if (!built) {
        built = true;
        std::error_code ec;
        for (fs::recursive_directory_iterator it(P(std::string(eden_platform_bundle_root()) + "/media"), ec), end;
             !ec && it != end; it.increment(ec)) {
            std::error_code e2;
            if (!it->is_regular_file(e2)) continue;
            index.emplace(it->path().filename().u8string(), it->path().u8string());   // first hit wins
        }
    }
    return index;
}

std::string eden_bundle_path(const std::string& name) {
    if (name.empty()) return std::string();
    auto& idx = media_index();
    auto it = idx.find(name);
    if (it != idx.end()) return it->second;
    std::string root = std::string(eden_platform_bundle_root()) + "/" + name;
    std::error_code ec;
    if (fs::is_regular_file(P(root), ec)) return root;
    return std::string();
}
