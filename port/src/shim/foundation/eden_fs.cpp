// eden_fs.cpp — G.2b-0. See eden_fs.h. The bodies of EdenFile are the fopen-backed NSFileHandle.mm
// ones (proven on Linux/Windows since Stage 3) minus the backups, with Apple's null-on-miss opens.
#include "eden_fs.h"
#include "platform_shims.h"   // eden_fseek64/eden_ftell64, eden_platform_bundle_root

#include <algorithm>
#include <cctype>    // eden_path_is_absolute (Windows drive letters)
#include <cstring>
#include <unordered_map>
#if defined(_WIN32)
#include <filesystem>
#include <system_error>
#include <io.h>      // _chsize_s, _fileno
#else
// POSIX, not std::filesystem: that is unavailable below iOS 13; kept POSIX because it is the engine's established idiom.
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>  // ftruncate
#endif

// ---- the few primitives everything below is built from ------------------------------------
#if defined(_WIN32)
namespace fs = std::filesystem;
static fs::path P(const std::string& s) { return fs::u8path(s); }

static bool x_exists(const std::string& p) { std::error_code ec; return fs::exists(P(p), ec); }
static bool x_is_dir(const std::string& p) { std::error_code ec; return fs::is_directory(P(p), ec); }
static bool x_is_file(const std::string& p) { std::error_code ec; return fs::is_regular_file(P(p), ec); }
static bool x_remove(const std::string& p) { std::error_code ec; return fs::remove(P(p), ec) && !ec; }
static bool x_rename(const std::string& a, const std::string& b) { std::error_code ec; fs::rename(P(a), P(b), ec); return !ec; }
static bool x_copy(const std::string& a, const std::string& b) {
    std::error_code ec;
    return fs::copy_file(P(a), P(b), fs::copy_options::overwrite_existing, ec) && !ec;
}
static bool x_mkdir(const std::string& p) { std::error_code ec; fs::create_directory(P(p), ec); return fs::is_directory(P(p), ec); }
static long long x_size(const std::string& p) { std::error_code ec; auto n = fs::file_size(P(p), ec); return ec ? -1 : (long long)n; }
static std::vector<std::string> x_list(const std::string& dir) {
    std::vector<std::string> out;
    std::error_code ec;
    for (fs::directory_iterator it(P(dir), ec), end; !ec && it != end; it.increment(ec))
        { auto u = it->path().filename().u8string(); out.emplace_back(u.begin(), u.end()); }   // C++20: u8string() is std::u8string
    return out;
}
#else
static bool x_exists(const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0; }
static bool x_is_dir(const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode); }
static bool x_is_file(const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode); }
static bool x_remove(const std::string& p) { return remove(p.c_str()) == 0; }   // file or empty directory, like fs::remove
static bool x_rename(const std::string& a, const std::string& b) { return rename(a.c_str(), b.c_str()) == 0; }
static bool x_copy(const std::string& a, const std::string& b) {
    FILE* in = fopen(a.c_str(), "rb");
    if (!in) return false;
    FILE* out = fopen(b.c_str(), "wb");
    if (!out) { fclose(in); return false; }
    char buf[64 * 1024];
    bool ok = true;
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0)
        if (fwrite(buf, 1, n, out) != n) { ok = false; break; }
    if (ferror(in)) ok = false;
    fclose(in);
    if (fclose(out) != 0) ok = false;
    return ok;
}
static bool x_mkdir(const std::string& p) { mkdir(p.c_str(), 0777); return x_is_dir(p); }
static long long x_size(const std::string& p) {
    struct stat st;
    return (stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode)) ? (long long)st.st_size : -1;
}
static std::vector<std::string> x_list(const std::string& dir) {
    std::vector<std::string> out;
    if (DIR* d = opendir(dir.c_str())) {
        while (const dirent* e = readdir(d))
            if (std::strcmp(e->d_name, ".") != 0 && std::strcmp(e->d_name, "..") != 0) out.push_back(e->d_name);
        closedir(d);
    }
    return out;
}
#endif

static FILE* open_existing(const std::string& path, const char* mode) {
    if (!x_exists(path) || x_is_dir(path)) return nullptr;   // fopen "r+b" on a dir can succeed on POSIX
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
bool eden_fs_exists(const std::string& path) { return x_exists(path); }
bool eden_fs_is_dir(const std::string& path) { return x_is_dir(path); }
bool eden_fs_remove(const std::string& path) { return x_remove(path); }
bool eden_fs_move(const std::string& from, const std::string& to) {
    if (x_rename(from, to)) return true;               // replaces an existing file on POSIX and (std) on Windows
    if (!x_copy(from, to)) return false;               // cross-device fallback
    x_remove(from);
    return true;
}
bool eden_fs_copy(const std::string& from, const std::string& to) { return x_copy(from, to); }
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
bool eden_fs_make_dir(const std::string& path) { return x_mkdir(path); }
long long eden_fs_size(const std::string& path) { return x_size(path); }
std::vector<std::string> eden_fs_list(const std::string& dir) {
    std::vector<std::string> out = x_list(dir);
    std::sort(out.begin(), out.end());
    return out;
}

// ---- eden_bundle_path ---------------------------------------------------------------------
static void index_media(const std::string& dir, std::unordered_map<std::string, std::string>& index) {
    std::vector<std::string> names = x_list(dir);
    std::sort(names.begin(), names.end());             // a stable "first hit wins" (directory order is not)
    for (const std::string& n : names) {
        const std::string full = dir + "/" + n;
        if (x_is_dir(full)) index_media(full, index);
        else if (x_is_file(full)) index.emplace(n, full);   // first hit wins
    }
}

static std::unordered_map<std::string, std::string>& media_index() {
    static std::unordered_map<std::string, std::string> index;
    static bool built = false;
    if (!built) {
        built = true;
        index_media(std::string(eden_platform_bundle_root()) + "/media", index);
    }
    return index;
}

std::string eden_bundle_path(const std::string& name) {
    if (name.empty()) return std::string();
    auto& idx = media_index();
    auto it = idx.find(name);
    if (it != idx.end()) return it->second;
    std::string root = std::string(eden_platform_bundle_root()) + "/" + name;
    if (x_is_file(root)) return root;
    return std::string();
}

bool eden_path_is_absolute(const std::string& s) {
#if defined(_WIN32)
    if (s.size() >= 3 && std::isalpha((unsigned char)s[0]) && s[1] == ':' && (s[2] == '\\' || s[2] == '/'))
        return true;
    if (!s.empty() && s[0] == '\\') return true;
#endif
    return !s.empty() && s[0] == '/';
}
