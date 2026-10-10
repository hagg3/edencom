//
//  WorldBrowser.mm
//  Eden
//
//  See WorldBrowser.h for the sources, the protocol and the platform split. This file is four
//  things, in this order:
//    1. the two list parsers (the official servers' line pairs, the archive's JSON manifest);
//    2. the unpacker — gzip, single-entry zip, and the archive's zip-in-a-zip — run a few MB per
//       frame over zlib so a big world never stalls the menu;
//    3. the import: the unpacked file is checked, named, and handed to the menu as a WorldNode;
//    4. the screen on the kit.
//
//  NOTHING HERE TRUSTS THE NETWORK. Ids become file names and URL paths, so an id that is not
//  [A-Za-z0-9_-] drops its entry; names become textures, so invalid UTF-8 is replaced before it
//  reaches NSString; a "world" whose header does not describe its own file is refused, and the
//  menu never sees a partial file (the seam writes `.part` and renames on success).
//
#import "WorldBrowser.h"
#import "Graphics.h"
#import "Globals.h"
#import "Util.h"
#import "World.h"
#import "Menu.h"
#import "FileManager.h"

#include <zlib.h>
#include <ctime>
#include "EdenWorldSource.h"   // emod::ImportJob (S.5)
extern "C" int eden_get_upgrade_256z(void);   // web/src/seam/Settings_web.mm (S.5e)

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

extern float SCREEN_WIDTH;
extern float SCREEN_HEIGHT;

namespace {

// ---------------------------------------------------------------------------------------------
// Sources
// ---------------------------------------------------------------------------------------------
const char* const kArchiveManifest = "https://hagg3.github.io/edenarchive/assets/data/worlds.json";
const char* const kArchiveFiles    = "https://hagg3.github.io/edenarchive/assets/worldfiles/";

struct Server { const char* app; const char* files; };
// Index = source - 1. The legacy pair is what ShareUtil.mm's commented-out history calls the
// service before the move to app2/files2; both still answer (2026-10-07).
const Server kServers[2] = {
    { "http://app2.edengame.net", "http://files2.edengame.net" },
    { "http://app.edengame.net",  "http://files.edengame.net"  },
};

const char* const kSourceTabs[WorldBrowser::SRC_COUNT] = { "Archive", "Current server", "Legacy server" };
const char* const kSourceNames[WorldBrowser::SRC_COUNT] = { "the world archive", "the current Eden server",
                                                            "the legacy Eden server" };
const char* const kModeTabs[2] = { "Featured", "Recent" };

struct Entry { std::string id, name, date, author, size, tags; };

// Ids become URL paths and file names: anything outside this set is not an id.
bool valid_id(const std::string& id) {
    if (id.empty() || id.size() > 64) return false;
    for (char c : id)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' || c == '_'))
            return false;
    return true;
}

// Replaces every byte that is not part of a well-formed UTF-8 sequence with '?', and control
// bytes with spaces. NSString's UTF-8 initialiser answers nil for a bad sequence, and a nil string
// is not something a label should ever be handed.
std::string clean_utf8(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = (unsigned char)s[i];
        int len = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
        bool ok = len > 0 && i + len <= s.size() && !(len == 2 && c < 0xC2);
        for (int k = 1; ok && k < len; k++) ok = ((unsigned char)s[i + k] & 0xC0) == 0x80;
        if (!ok) { out += '?'; i++; continue; }
        if (len == 1 && (c < 0x20 || c == 0x7f)) out += ' ';
        else out.append(s, i, (size_t)len);
        i += (size_t)len;
    }
    return out;
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return std::string();
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

bool ends_with(const std::string& s, const char* suf) {
    const size_t n = std::strlen(suf);
    return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
}

std::string lower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

// Official ids are the upload's unix time — the only date the protocol carries.
std::string date_of_id(const std::string& id) {
    if (id.size() < 9 || id.size() > 11) return std::string();
    for (char c : id) if (c < '0' || c > '9') return std::string();
    const time_t t = (time_t)std::strtoll(id.c_str(), NULL, 10);
    const struct tm* g = std::gmtime(&t);       // main thread only; the _r/_s variants differ per CRT
    if (!g) return std::string();
    char buf[16];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", g);
    return buf;
}

// ---------------------------------------------------------------------------------------------
// 1a. The official list: `<id>.eden` / `<name>.name` line pairs
// ---------------------------------------------------------------------------------------------
// Scanned for adjacency, not read at a stride of two: one stray blank line would otherwise
// desync every pair after it (VuencEdit learnt this against the same servers).
std::vector<Entry> parse_official(const unsigned char* p, int n) {
    std::vector<std::string> lines;
    std::string cur;
    for (int i = 0; i < n; i++) {
        if (p[i] == '\n') { lines.push_back(trim(cur)); cur.clear(); }
        else cur += (char)p[i];
    }
    if (!cur.empty()) lines.push_back(trim(cur));
    std::vector<Entry> out;
    for (size_t i = 0; i + 1 < lines.size();) {
        if (ends_with(lines[i], ".eden") && ends_with(lines[i + 1], ".name")) {
            Entry e;
            e.id = lines[i].substr(0, lines[i].size() - 5);
            e.name = clean_utf8(trim(lines[i + 1].substr(0, lines[i + 1].size() - 5)));
            if (e.name.empty()) e.name = "Untitled";
            e.date = date_of_id(e.id);
            if (valid_id(e.id)) out.push_back(e);
            i += 2;
        } else {
            i++;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// 1b. The archive manifest: a JSON array of flat objects
// ---------------------------------------------------------------------------------------------
// The smallest JSON reader that does the job: strings (with escapes and \u surrogates), and
// everything else skipped or kept as its literal text. Values the archive is known to get wrong —
// a tag that is `true` or a number instead of a string — are kept as text rather than failing the
// entry, as the web's reader learnt to (eden-worldbrowser.js, entryMatches).
struct Json {
    const char* p;
    const char* e;
    bool ok;
    Json(const char* b, const char* end) : p(b), e(end), ok(true) {}

    void ws() { while (p < e && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++; }
    bool lit(char c) { ws(); if (p < e && *p == c) { p++; return true; } return false; }

    static void utf8(std::string* o, unsigned cp) {
        if (cp < 0x80) *o += (char)cp;
        else if (cp < 0x800) { *o += (char)(0xC0 | (cp >> 6)); *o += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) {
            *o += (char)(0xE0 | (cp >> 12)); *o += (char)(0x80 | ((cp >> 6) & 0x3F)); *o += (char)(0x80 | (cp & 0x3F));
        } else {
            *o += (char)(0xF0 | (cp >> 18)); *o += (char)(0x80 | ((cp >> 12) & 0x3F));
            *o += (char)(0x80 | ((cp >> 6) & 0x3F)); *o += (char)(0x80 | (cp & 0x3F));
        }
    }
    bool hex4(unsigned* v) {
        if (e - p < 4) return false;
        *v = 0;
        for (int i = 0; i < 4; i++) {
            const char c = *p++;
            *v <<= 4;
            if (c >= '0' && c <= '9') *v |= (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') *v |= (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') *v |= (unsigned)(c - 'A' + 10);
            else return false;
        }
        return true;
    }
    bool str(std::string* o) {
        ws();
        if (p >= e || *p != '"') return false;
        p++;
        while (p < e && *p != '"') {
            if (*p != '\\') { *o += *p++; continue; }
            if (++p >= e) return false;
            const char c = *p++;
            switch (c) {
            case 'n': *o += '\n'; break;
            case 't': *o += '\t'; break;
            case 'r': *o += '\r'; break;
            case 'b': *o += '\b'; break;
            case 'f': *o += '\f'; break;
            case 'u': {
                unsigned cp;
                if (!hex4(&cp)) return false;
                if (cp >= 0xD800 && cp < 0xDC00 && e - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                    p += 2;
                    unsigned lo;
                    if (!hex4(&lo)) return false;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                utf8(o, cp);
                break;
            }
            default: *o += c; break;            // \" \\ \/
            }
        }
        if (p >= e) return false;
        p++;
        return true;
    }
    // Any value. A scalar is returned as text (null as ""); arrays/objects are skipped unless
    // `items` is given, in which case an array's scalar items are collected into it.
    bool value(std::string* text, std::vector<std::string>* items = NULL) {
        ws();
        if (p >= e) return false;
        if (*p == '"') { std::string s; if (!str(&s)) return false; if (text) *text = s; return true; }
        if (*p == '[') {
            p++;
            if (lit(']')) return true;
            do {
                std::string s;
                if (!value(&s)) return false;
                if (items && !s.empty()) items->push_back(s);
            } while (lit(','));
            return lit(']');
        }
        if (*p == '{') {
            p++;
            if (lit('}')) return true;
            do {
                std::string k;
                if (!str(&k) || !lit(':') || !value(NULL)) return false;
            } while (lit(','));
            return lit('}');
        }
        const char* b = p;
        while (p < e && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\n' && *p != '\r' && *p != '\t') p++;
        if (p == b) return false;
        std::string s(b, (size_t)(p - b));
        if (text) *text = (s == "null") ? std::string() : s;
        return true;
    }
};

bool parse_archive(const unsigned char* data, int n, std::vector<Entry>* out) {
    Json j((const char*)data, (const char*)data + n);
    if (!j.lit('[')) return false;
    if (j.lit(']')) return true;
    do {
        if (!j.lit('{')) return false;
        Entry en;
        std::string file;
        if (!j.lit('}')) {
            do {
                std::string k, v;
                std::vector<std::string> tags;
                if (!j.str(&k) || !j.lit(':')) return false;
                if (!j.value(&v, k == "tags" ? &tags : NULL)) return false;
                if (k == "filename") file = v;
                else if (k == "worldname") en.name = v;
                else if (k == "publishdate") en.date = v;
                else if (k == "author") en.author = v;
                else if (k == "filesize") en.size = v;
                else if (k == "tags") {
                    for (size_t t = 0; t < tags.size(); t++) en.tags += (t ? ", " : "") + tags[t];
                }
            } while (j.lit(','));
            if (!j.lit('}')) return false;
        }
        en.id = ends_with(file, ".eden") ? file.substr(0, file.size() - 5) : file;
        en.name = clean_utf8(trim(en.name));
        if (en.name.empty()) en.name = en.id;
        en.author = clean_utf8(en.author);
        en.tags = clean_utf8(en.tags);
        en.date = clean_utf8(en.date);
        en.size = clean_utf8(en.size);
        if (valid_id(en.id)) out->push_back(en);
    } while (j.lit(','));
    return j.lit(']');
}

// ---------------------------------------------------------------------------------------------
// 2. Unpacking: gzip / zip, nested, a budget per frame
// ---------------------------------------------------------------------------------------------
enum Kind { K_RAW = 0, K_GZIP, K_ZIP };

Kind sniff(const std::string& path) {
    unsigned char m[4] = {0, 0, 0, 0};
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return K_RAW;
    const size_t n = std::fread(m, 1, 4, f);
    std::fclose(f);
    if (n >= 2 && m[0] == 0x1f && m[1] == 0x8b) return K_GZIP;
    if (n == 4 && m[0] == 'P' && m[1] == 'K' && m[2] == 3 && m[3] == 4) return K_ZIP;
    return K_RAW;
}

unsigned u16le(const unsigned char* b) { return (unsigned)b[0] | ((unsigned)b[1] << 8); }
unsigned u32le(const unsigned char* b) { return u16le(b) | (u16le(b + 2) << 16); }

long long file_size(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return -1;
    std::fseek(f, 0, SEEK_END);
    const long long n = (long long)std::ftell(f);
    std::fclose(f);
    return n;
}

class Unpacker {
public:
    Unpacker() : m_in(NULL), m_out(NULL), m_zon(false), m_stored(false), m_gzip(false), m_left(0), m_total(0) {
        std::memset(&m_zs, 0, sizeof(m_zs));
    }
    ~Unpacker() { end(); }

    // Opens one layer: src (gzip or zip) -> dst. False with err() set when src is unreadable.
    bool begin(const std::string& src, const std::string& dst, Kind kind) {
        end();
        m_err.clear();
        m_in = std::fopen(src.c_str(), "rb");
        if (!m_in) return fail("could not read the download");
        long long dataStart = 0, compSize = file_size(src);
        m_gzip = (kind == K_GZIP);
        m_stored = false;
        if (kind == K_ZIP && !zipEntry(&dataStart, &compSize)) return false;
        std::fseek(m_in, (long)dataStart, SEEK_SET);
        m_left = m_total = compSize;
        m_out = std::fopen(dst.c_str(), "wb");
        if (!m_out) return fail("could not write the world (is the disk full?)");
        if (!m_stored) {
            std::memset(&m_zs, 0, sizeof(m_zs));
            // 15+32: zlib detects the gzip header itself. -15: a zip entry is a raw deflate stream.
            if (inflateInit2(&m_zs, m_gzip ? 15 + 32 : -15) != Z_OK) return fail("could not start unpacking");
            m_zon = true;
        }
        return true;
    }

    // 1 = layer finished, 0 = more to do, -1 = failed (err()). `budget` counts bytes read AND
    // written: voxel data inflates ~100:1, so an input-only budget could still write hundreds of
    // MB in one frame.
    int step(long long budget) {
        unsigned char outBuf[1 << 17];
        while (budget > 0) {
            if (m_stored) {
                if (m_left <= 0) return finishCheck();
                const size_t want = (size_t)std::min<long long>(sizeof(m_inHold), m_left);
                const size_t got = std::fread(m_inHold, 1, want, m_in);
                if (got == 0) return fail2("the download is truncated");
                if (std::fwrite(m_inHold, 1, got, m_out) != got) return fail2("could not write the world (is the disk full?)");
                m_left -= (long long)got;
                budget -= 2 * (long long)got;
                continue;
            }
            if (m_zs.avail_in == 0 && m_left > 0) {
                const size_t want = (size_t)std::min<long long>(sizeof(m_inHold), m_left);
                const size_t got = std::fread(m_inHold, 1, want, m_in);
                if (got == 0) return fail2("the download is truncated");
                m_left -= (long long)got;
                budget -= (long long)got;
                m_zs.next_in = m_inHold;
                m_zs.avail_in = (uInt)got;
            }
            m_zs.next_out = outBuf;
            m_zs.avail_out = sizeof(outBuf);
            const int rc = inflate(&m_zs, Z_NO_FLUSH);
            const size_t have = sizeof(outBuf) - m_zs.avail_out;
            if (have && std::fwrite(outBuf, 1, have, m_out) != have)
                return fail2("could not write the world (is the disk full?)");
            budget -= (long long)have + 1;
            if (rc == Z_STREAM_END) {
                // A gzip file may be several members back to back (`cat a.gz b.gz`); anything
                // after a zip entry's stream is the next record, not data.
                if (m_gzip && (m_zs.avail_in > 0 || m_left > 0) && nextIsGzip()) { inflateReset(&m_zs); continue; }
                m_left = 0;
                return finishCheck();
            }
            if (rc == Z_BUF_ERROR) {
                // No progress possible: out of input. With none left to read, the stream was cut short.
                if (m_zs.avail_in == 0 && m_left <= 0) return fail2("the download is truncated");
                continue;
            }
            if (rc != Z_OK) return fail2("the download is damaged");
        }
        return 0;
    }

    float progress() const { return m_total > 0 ? 1.0f - (float)m_left / (float)m_total : 0.0f; }
    const std::string& err() const { return m_err; }

    void end() {
        if (m_zon) { inflateEnd(&m_zs); m_zon = false; }
        if (m_in) { std::fclose(m_in); m_in = NULL; }
        if (m_out) { std::fclose(m_out); m_out = NULL; }
    }

private:
    bool fail(const char* m) { m_err = m; end(); return false; }
    int fail2(const char* m) { m_err = m; end(); return -1; }
    int finishCheck() {
        const bool closed = std::fclose(m_out) == 0;
        m_out = NULL;
        end();
        if (!closed) { m_err = "could not write the world (is the disk full?)"; return -1; }
        return 1;
    }

    bool nextIsGzip() {
        if (m_zs.avail_in < 2) {
            // Top the buffer up so the magic can be read; a member boundary rarely lands here.
            unsigned char tmp[2];
            size_t have = m_zs.avail_in;
            if (have) std::memcpy(tmp, m_zs.next_in, have);
            const size_t got = std::fread(tmp + have, 1, 2 - have, m_in);
            m_left -= (long long)got;
            have += got;
            std::memcpy(m_inHold, tmp, have);
            m_zs.next_in = m_inHold;
            m_zs.avail_in = (uInt)have;
            if (have < 2) return false;
        }
        return m_zs.next_in[0] == 0x1f && m_zs.next_in[1] == 0x8b;
    }

    // The entry to extract: the first `.eden` in the central directory, else the first file. The
    // central directory's sizes are authoritative — the local header's are legitimately zero when
    // the archiver streamed (general-purpose bit 3), which is why the web's reader starts there too.
    bool zipEntry(long long* dataStart, long long* compSize) {
        std::fseek(m_in, 0, SEEK_END);
        const long long size = (long long)std::ftell(m_in);
        const long long tailLen = std::min<long long>(size, 65557);
        std::vector<unsigned char> tail((size_t)tailLen);
        std::fseek(m_in, (long)(size - tailLen), SEEK_SET);
        if (std::fread(tail.data(), 1, tail.size(), m_in) != tail.size()) return fail("could not read the download");
        long long eocd = -1;
        for (long long i = tailLen - 22; i >= 0; i--)
            if (u32le(&tail[(size_t)i]) == 0x06054b50) { eocd = i; break; }
        if (eocd < 0) return fail("the download is not a zip archive");
        const unsigned entries = u16le(&tail[(size_t)eocd + 10]);
        const unsigned cdSize = u32le(&tail[(size_t)eocd + 12]);
        const unsigned cdOff = u32le(&tail[(size_t)eocd + 16]);
        if (cdOff == 0xFFFFFFFFu || (long long)cdOff + cdSize > size) return fail("this zip format is not supported (zip64)");
        std::vector<unsigned char> cd(cdSize);
        std::fseek(m_in, (long)cdOff, SEEK_SET);
        if (std::fread(cd.data(), 1, cd.size(), m_in) != cd.size()) return fail("could not read the download");
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
            if (!isDir && !junk && (pickOff < 0 || ends_with(lower(name), ".eden"))) {
                const bool better = pickOff < 0 || ends_with(lower(name), ".eden");
                if (better) { pickOff = loff; pickSize = csize; pickMethod = method; }
                if (ends_with(lower(name), ".eden")) break;
            }
            at += 46 + nlen + xlen + clen;
        }
        if (pickOff < 0) return fail("the zip archive is empty");
        if (pickSize == 0xFFFFFFFFLL || pickOff == 0xFFFFFFFFLL) return fail("this zip format is not supported (zip64)");
        if (pickMethod != 0 && pickMethod != 8) return fail("this zip compression is not supported");
        unsigned char lh[30];
        std::fseek(m_in, (long)pickOff, SEEK_SET);
        if (std::fread(lh, 1, 30, m_in) != 30 || u32le(lh) != 0x04034b50) return fail("the zip archive is damaged");
        *dataStart = pickOff + 30 + u16le(lh + 26) + u16le(lh + 28);
        *compSize = pickSize;
        if (*dataStart + *compSize > size) return fail("the download is truncated");
        m_stored = (pickMethod == 0);
        m_gzip = false;
        return true;
    }

    FILE* m_in;
    FILE* m_out;
    z_stream m_zs;
    bool m_zon, m_stored, m_gzip;
    long long m_left, m_total;
    std::string m_err;
    unsigned char m_inHold[1 << 16];
};

// ---------------------------------------------------------------------------------------------
// 3. Import
// ---------------------------------------------------------------------------------------------
// A world file describes itself: a 192-byte header whose directory_offset points inside the file
// (FileManager reads the column directory from there to EOF). Anything else — an HTML error page
// served with a 200, a preview PNG, a truncated transfer — is refused here, before the menu
// lists it.
bool looks_like_world(const std::string& path) {
    const long long size = file_size(path);
    if (size < (long long)sizeof(WorldFileHeader)) return false;
    WorldFileHeader h;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    const bool read = std::fread(&h, sizeof(h), 1, f) == 1;
    std::fclose(f);
    return read && h.directory_offset >= sizeof(WorldFileHeader) && (long long)h.directory_offset <= size;
}

std::string documents_dir() { return cpstring(World::getWorld->fm->documents); }

}  // namespace

// ---------------------------------------------------------------------------------------------
// 4. The screen
// ---------------------------------------------------------------------------------------------
#define WB_MAX_ROWS 24
#define WB_META 4

namespace {
struct SourceState {
    std::vector<Entry> all;
    std::vector<int>   view;             // indices into `all` that the list shows
    int  mode, listMode;                 // listMode: the last Featured/Recent, for a cleared search
    std::string query;                   // archive: live filter; servers: the search that is showing
    bool loaded, more;
    int  listJob;
    bool listAppend;
    std::string status;
    int  first, selected;                // into `view`
    SourceState() : mode(WorldBrowser::MODE_FEATURED), listMode(WorldBrowser::MODE_FEATURED), loaded(false),
                    more(false), listJob(0), listAppend(false), first(0), selected(-1) {}
};
enum DlState { DL_IDLE, DL_FETCH, DL_UNPACK, DL_CONVERT };
}

struct BrowserImpl {
    bool open;
    int  src;
    SourceState S[WorldBrowser::SRC_COUNT];

    GLW::Label  title, status, empty, emptyHint, name, noPreview, progressText;
    GLW::Label  meta[WB_META];
    GLW::Button back, download, more, go;
    GLW::TabRail sources, modes;
    GLW::TextField search;
    GLW::ListRow rows[WB_MAX_ROWS];
    std::string rowText[WB_MAX_ROWS];
    bool rowBuilt[WB_MAX_ROWS];
    GLW::ScrollView scroll;
    CGRect panel, list, detail, previewBox;
    float  pitch, btnH, titleY, fieldPt;
    bool   built, showTitle, shownBusy;

    int listTouch, barTouch, downRow, tabTouch, modeTouch;

    std::string detailKey, shownStatus, shownEmpty, shownProgress;

    int   previewJob;
    std::string previewKey, previewWant;
    Texture2D* previewTex;
    float previewAspect, previewDelay;
    bool  previewMissing;

    int   dlJob, dlSrc;
    DlState dl;
    Entry dlEntry;
    Unpacker unpack;
    emod::ImportJob convert;             // S.5: with g_world_format on, the download converts while it inflates
    std::string dlFile, layerIn, layerOut, dlStatus, convOut;
    int   dlDepth;
    float dlFrac;
    float clock;

    BrowserImpl() : open(false), src(WorldBrowser::SRC_ARCHIVE), pitch(30), btnH(30), titleY(0), fieldPt(0), built(false),
                    showTitle(true), shownBusy(false), listTouch(-1), barTouch(-1), downRow(-1), tabTouch(-1), modeTouch(-1),
                    previewJob(0), previewTex(NULL), previewAspect(0), previewDelay(0), previewMissing(false),
                    dlJob(0), dlSrc(0), dl(DL_IDLE), dlDepth(0), dlFrac(0), clock(0) {
        panel = list = detail = previewBox = CGRectMake(0, 0, 0, 0);
        for (int r = 0; r < WB_MAX_ROWS; r++) rowBuilt[r] = false;
    }

    bool official() const { return src != WorldBrowser::SRC_ARCHIVE; }
    SourceState& cur() { return S[src]; }

    const Entry* selectedEntry() {
        SourceState& s = cur();
        if (s.selected < 0 || s.selected >= (int)s.view.size()) return NULL;
        return &s.all[(size_t)s.view[(size_t)s.selected]];
    }

    // --- URLs -------------------------------------------------------------------------------
    std::string listUrl(int source, int mode, const std::string& q, int start) {
        if (source == WorldBrowser::SRC_ARCHIVE) return kArchiveManifest;
        const Server& sv = kServers[source - 1];
        if (mode == WorldBrowser::MODE_FEATURED) return std::string(sv.files) + "/popularlist.txt";
        if (mode == WorldBrowser::MODE_SEARCH) {
            std::string u = std::string(sv.app) + "/list2.php?search=";
            static const char* hexd = "0123456789ABCDEF";
            for (unsigned char c : q) {
                if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') u += (char)c;
                else if (c == ' ') u += '+';
                else { u += '%'; u += hexd[c >> 4]; u += hexd[c & 15]; }
            }
            return u;
        }
        char buf[200];
        std::snprintf(buf, sizeof(buf), "%s/list2.php?start=%d&sort=2", sv.app, start);
        return buf;
    }
    std::string worldUrl(int source, const std::string& id) {
        if (source == WorldBrowser::SRC_ARCHIVE) return std::string(kArchiveFiles) + id + "/" + id + ".eden.zip";
        return std::string(kServers[source - 1].files) + "/" + id + ".eden";
    }
    std::string previewUrl(int source, const std::string& id) {
        if (source == WorldBrowser::SRC_ARCHIVE) return std::string(kArchiveFiles) + id + "/" + id + ".eden.png";
        return std::string(kServers[source - 1].files) + "/" + id + ".eden.png";
    }

    // --- lists ------------------------------------------------------------------------------
    void refilter(int source) {
        SourceState& s = S[source];
        std::string keepId;
        if (s.selected >= 0 && s.selected < (int)s.view.size()) keepId = s.all[(size_t)s.view[(size_t)s.selected]].id;
        s.view.clear();
        const std::string q = lower(trim(s.query));
        for (size_t i = 0; i < s.all.size(); i++) {
            if (source == WorldBrowser::SRC_ARCHIVE && !q.empty()) {
                const Entry& e = s.all[i];
                if (lower(e.name).find(q) == std::string::npos && lower(e.author).find(q) == std::string::npos &&
                    lower(e.tags).find(q) == std::string::npos)
                    continue;
            }
            s.view.push_back((int)i);
        }
        s.selected = -1;
        for (size_t v = 0; v < s.view.size() && !keepId.empty(); v++)
            if (s.all[(size_t)s.view[v]].id == keepId) s.selected = (int)v;
        if (s.selected < 0) s.first = 0;
    }

    void startList(int source, bool append) {
        SourceState& s = S[source];
        eden_net_release(s.listJob);
        const int start = append ? (int)s.all.size() : 0;
        s.listJob = eden_net_fetch(listUrl(source, s.mode, s.query, start).c_str(), NULL);
        s.listAppend = append;
        if (!append) { s.all.clear(); s.view.clear(); s.selected = -1; s.first = 0; s.more = false; }
        s.status = s.listJob ? "" : "No network on this device";
    }

    void pollList(int source) {
        SourceState& s = S[source];
        if (!s.listJob) return;
        const int st = eden_net_poll(s.listJob, NULL, NULL);
        if (st == 0) return;
        if (st < 0) {
            char buf[240];
            std::snprintf(buf, sizeof(buf), "Couldn't reach %s (%s)", kSourceNames[source], eden_net_error(s.listJob));
            s.status = buf;
            s.more = false;
        } else {
            int len = 0;
            const unsigned char* body = eden_net_body(s.listJob, &len);
            std::vector<Entry> got;
            bool ok = true;
            if (source == WorldBrowser::SRC_ARCHIVE) ok = parse_archive(body, len, &got);
            else got = parse_official(body, len);
            if (!ok) {
                s.status = "The world archive's list is damaged";
            } else {
                size_t added = 0;
                for (size_t i = 0; i < got.size(); i++) {
                    if (s.listAppend) {
                        bool dup = false;
                        for (size_t k = 0; k < s.all.size() && !dup; k++) dup = s.all[k].id == got[i].id;
                        if (dup) continue;
                    }
                    s.all.push_back(got[i]);
                    added++;
                }
                s.loaded = true;
                s.more = (source != WorldBrowser::SRC_ARCHIVE && s.mode == WorldBrowser::MODE_RECENT && added > 0);
                refilter(source);
                s.status.clear();
            }
        }
        eden_net_release(s.listJob);
        s.listJob = 0;
    }

    // --- preview ----------------------------------------------------------------------------
    void dropPreview() {
        eden_net_release(previewJob);
        previewJob = 0;
        if (previewTex) { delete previewTex; previewTex = NULL; }
        previewKey.clear();
        previewMissing = false;
    }

    void pollPreview(float etime) {
        const Entry* e = selectedEntry();
        const std::string want = e ? (std::to_string(src) + ":" + e->id) : std::string();
        if (want != previewWant) {               // the selection moved: wait for it to settle
            previewWant = want;
            previewDelay = 0.25f;
            dropPreview();
        }
        if (want.empty() || previewKey == want) return;
        if (!previewJob) {
            previewDelay -= etime;
            if (previewDelay > 0.0f) return;
            previewJob = eden_net_fetch(previewUrl(src, e->id).c_str(), NULL);
            if (!previewJob) { previewKey = want; previewMissing = true; }
            return;
        }
        const int st = eden_net_poll(previewJob, NULL, NULL);
        if (st == 0) return;
        previewKey = want;
        previewMissing = true;
        if (st > 0) {
            int len = 0;
            const unsigned char* b = eden_net_body(previewJob, &len);
            // PNG only (that is all either host serves), sized from its IHDR for the aspect fit.
            if (b && len > 24 && b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G') {
                const unsigned w = (b[16] << 24) | (b[17] << 16) | (b[18] << 8) | b[19];
                const unsigned h = (b[20] << 24) | (b[21] << 16) | (b[22] << 8) | b[23];
                const std::string path = documents_dir() + "/.emod-preview.png";
                FILE* f = std::fopen(path.c_str(), "wb");
                if (f && w > 0 && h > 0) {
                    const bool wrote = std::fwrite(b, 1, (size_t)len, f) == (size_t)len;
                    std::fclose(f);
                    f = NULL;
                    if (wrote) {
                        Texture2D* t = new Texture2D(nsstring(path), FALSE);
                        if (t->name) { previewTex = t; previewAspect = (float)w / (float)h; previewMissing = false; }
                        else delete t;
                    }
                }
                if (f) std::fclose(f);
                std::remove(path.c_str());
            }
        }
        eden_net_release(previewJob);
        previewJob = 0;
    }

    // --- download + unpack + import ---------------------------------------------------------
    void tempPaths() {
        const std::string d = documents_dir();
        dlFile = d + "/.emod-download";
        layerIn = d + "/.emod-unpack-a";
        layerOut = d + "/.emod-unpack-b";
    }
    void cleanTemps() {
        tempPaths();
        std::remove(dlFile.c_str());
        std::remove((dlFile + ".part").c_str());
        std::remove(layerIn.c_str());
        std::remove(layerOut.c_str());
    }

    void startDownload() {
        const Entry* e = selectedEntry();
        if (!e || dl != DL_IDLE) return;
        cleanTemps();
        dlEntry = *e;
        dlSrc = src;
        dlFrac = 0;
        dlJob = eden_net_fetch(worldUrl(src, e->id).c_str(), dlFile.c_str());
        if (!dlJob) { dlStatus = "No network on this device"; return; }
        dl = DL_FETCH;
        dlStatus.clear();
    }

    void cancelDownload(const char* why) {
        eden_net_release(dlJob);
        dlJob = 0;
        unpack.end();
        convert.abort();                 // deletes its spill / .converting / layer
        dl = DL_IDLE;
        cleanTemps();
        dlStatus = why ? why : "";
    }

    void failDownload(const std::string& why) {
        cancelDownload(NULL);
        dlStatus = "Download failed: " + why;
    }

    void pollDownload() {
        if (dl == DL_FETCH) {
            long long got = 0, total = -1;
            const int st = eden_net_poll(dlJob, &got, &total);
            char buf[160];
            // Whole percent / whole MB: the text is a texture, and a per-frame-changing string
            // would be a raster (and on iOS a glFlush) every frame for the whole download.
            if (total > 0) {
                dlFrac = (float)got / (float)total;
                std::snprintf(buf, sizeof(buf), "Downloading %d%% of %.1f MB", (int)(dlFrac * 100.0f), total / 1048576.0);
            } else {
                dlFrac = -1.0f;
                std::snprintf(buf, sizeof(buf), "Downloading %d MB", (int)(got / 1048576));
            }
            dlStatus = buf;
            if (st == 0) return;
            if (st < 0) { failDownload(eden_net_error(dlJob)); return; }
            eden_net_release(dlJob);
            dlJob = 0;
            dlDepth = 0;
            if (FileManager::conversionEnabled()) {
                // Plan §4 *Downloads and imports*: no inflated `.eden` is ever written. The job
                // unpacks nested layers (still compressed) and converts the innermost stream.
                // An uncompressed payload gets the same "is it a world" check as the old path.
                if (sniff(dlFile) == K_RAW && !looks_like_world(dlFile)) {
                    failDownload("that is not an Eden world file");
                    return;
                }
                const std::string d = documents_dir();
                for (int k = 1; k < 1000; k++) {
                    convOut = d + "/" + dlEntry.id + (k == 1 ? std::string() : "-" + std::to_string(k)) + ".emod";
                    if (file_size(convOut) < 0) break;
                }
                // S.5e: Settings' "Upgrade 64z worlds to 256z when converting" (no per-world prompt here).
                if (!convert.begin(dlFile, convOut, dlEntry.id + ".eden", (int64_t)time(NULL), eden_get_upgrade_256z() != 0)) {
                    failDownload(convert.error());
                    return;
                }
                dl = DL_CONVERT;
                return;
            }
            layerIn = dlFile;                    // layers alternate between the two unpack temp names
            layerOut = documents_dir() + "/.emod-unpack-a";
            const std::string first = dlFile;
            const Kind k = sniff(first);
            if (k == K_RAW) { finishImport(first); return; }
            dlDepth = 1;
            if (!unpack.begin(first, layerOut, k)) { failDownload(unpack.err()); return; }
            dl = DL_UNPACK;
            return;
        }
        if (dl == DL_CONVERT) {
            const int r = convert.step(12);
            if (r == emod::ImportJob::FAILED) { failDownload(convert.error()); return; }
            char buf[64];
            std::snprintf(buf, sizeof(buf), "Converting %d%%", convert.percent());
            dlStatus = buf;
            dlFrac = convert.percent() / 100.0f;
            if (r == emod::ImportJob::RUNNING) return;
            std::remove(dlFile.c_str());        // the user's call (2026-10-09): the download is discarded
            dl = DL_IDLE;
            addImported(convOut.substr(documents_dir().size() + 1));
            return;
        }
        if (dl == DL_UNPACK) {
            const int r = unpack.step(6LL << 20);   // ~6 MB read + written a frame
            dlFrac = unpack.progress();
            dlStatus = "Unpacking";
            if (r < 0) { failDownload(unpack.err()); return; }
            if (r == 0) return;
            const std::string done = layerOut;
            std::remove(layerIn.c_str());
            // The next layer reads `done` and writes the other temp name.
            const std::string other = documents_dir() + (done == documents_dir() + "/.emod-unpack-a"
                                                         ? "/.emod-unpack-b" : "/.emod-unpack-a");
            const Kind k = sniff(done);
            if (k == K_RAW) { finishImport(done); return; }
            if (++dlDepth > 4) { failDownload("the archive is nested too deeply"); return; }
            if (!unpack.begin(done, other, k)) { failDownload(unpack.err()); return; }
            layerIn = done;
            layerOut = other;
        }
    }

    void finishImport(const std::string& path) {
        dl = DL_IDLE;
        if (!looks_like_world(path)) { failDownload("that is not an Eden world file"); return; }
        // `<id>.eden`, or `<id>-2.eden` ... when the player already has one: a re-download must
        // never replace a world they have since played and changed.
        const std::string d = documents_dir();
        std::string file;
        for (int k = 1; k < 1000; k++) {
            file = dlEntry.id + (k == 1 ? std::string() : "-" + std::to_string(k)) + ".eden";
            if (file_size(d + "/" + file) < 0) break;
        }
        if (std::rename(path.c_str(), (d + "/" + file).c_str()) != 0) { failDownload("could not save the world"); return; }
        cleanTemps();
        addImported(file);
    }

    // The downloaded world is on disk as `file` (a `.eden`, or since S.5 an `.emod`): list it, select it.
    void addImported(const std::string& file) {
        cleanTemps();

        Menu* m = World::getWorld->menu;
        NSString* nsFile = nsstring(file);
        NSString* shown = World::getWorld->fm->getName(nsFile);
        if (!shown || [shown isEqualToString:@"error~"] || [shown isEqualToString:@"error"])
            shown = nsstring(dlEntry.name);
        WorldNode* node = (WorldNode*)malloc(sizeof(WorldNode));
        memset(node, 0, sizeof(WorldNode));
        node->display_name = shown;
        node->file_name = nsFile;
        [node->display_name retain];
        [node->file_name retain];
        m->addWorld(node);
        m->selected_world = node;
        m->fnbar->setStatus(node->display_name, 9999);
        m->sbar->setStatus([NSString stringWithFormat:@"Downloaded %@", node->display_name], 4);
        dlStatus.clear();
        open = false;                        // back to the menu, the new world selected (the DOM does the same)
        search.blur();
        dropPreview();
    }
};

// ---------------------------------------------------------------------------------------------

WorldBrowser::WorldBrowser() : d(new BrowserImpl()) {}
WorldBrowser::~WorldBrowser() {
    for (int s = 0; s < SRC_COUNT; s++) eden_net_release(d->S[s].listJob);
    d->cancelDownload(NULL);
    d->dropPreview();
    delete d;
}

bool WorldBrowser::available() { return eden_net_available() != 0; }
bool WorldBrowser::isOpen() const { return d->open; }

void WorldBrowser::open() {
    d->open = true;
    d->cleanTemps();
    d->dlStatus.clear();
    SourceState& s = d->cur();
    if (!s.loaded && !s.listJob) d->startList(d->src, false);
}

void WorldBrowser::close() {
    if (d->dl != DL_IDLE) d->cancelDownload(NULL);
    d->search.blur();
    d->dropPreview();
    d->previewWant.clear();
    d->open = false;
}

static const int usage_id = 12;   // distinct from Menu (7), SettingsMenu (3), GLDialog (9), Keybinds (11)

// Pure rect arithmetic from SCREEN_* plus the labels' one-time rasters; run every frame like
// Menu::layoutKit(), so a display-profile switch re-flows on the next frame.
static void wb_layout(BrowserImpl* d) {
    using namespace GLW;
    if (!d->built) {
        d->built = true;
        d->title.set("Get Worlds", du(28), UITextAlignmentCenter);
        d->back.setLabel("Back", du(20));
        d->go.setLabel("Search", du(20));
        d->more.setLabel("More", du(20));
        d->download.setLabel("Download", du(20));
        d->download.setTone(GLW::Button::TONE_POSITIVE);
        d->sources.setTabs(kSourceTabs, WorldBrowser::SRC_COUNT, du(18));
        d->modes.setTabs(kModeTabs, 2, du(18));
        d->search.setMaxBytes(48);
        d->search.setPlaceholder(d->official() ? "Search world names" : "Search names, authors, tags");
        d->noPreview.set("No preview", du(16), UITextAlignmentCenter, 0, FACE_BODY);
    }
    const float tf = touchFloor();
    d->pitch = std::max(du(28), tf);
    d->btnH = std::max(du(30), tf);
    const float m = du(12), pad = du(12), gap = du(8);
    float pw = std::min(SCREEN_WIDTH - 2.0f * m, du(900));
    const float ph = SCREEN_HEIGHT - 2.0f * m;
    d->panel = CGRectMake((SCREEN_WIDTH - pw) * 0.5f, m, pw, ph);
    const float left = d->panel.origin.x + pad, cw = pw - 2.0f * pad, right = left + cw;
    const float top = d->panel.origin.y + ph - pad;
    const float actY = d->panel.origin.y + pad;

    // The titlebar carries the title only when the list still gets five rows without it — on a
    // short (touch-profile) screen Back sits beside the source tabs instead.
    const float fixedRows = 3.0f * d->btnH + 3.0f * gap;     // tabs, controls, action bar
    d->showTitle = (ph - 2.0f * pad - fixedRows - d->btnH - gap) >= 5.0f * d->pitch;
    float y = top - d->btnH;
    const float backW = du(90);
    d->back.setRect(CGRectMake(left, y, backW, d->btnH));
    if (d->showTitle) {
        d->titleY = y + (d->btnH + d->title.height()) * 0.5f;
        y -= gap + d->btnH;
        d->sources.setRect(CGRectMake(left, y, cw, d->btnH));
    } else {
        d->sources.setRect(CGRectMake(left + backW + gap, y, cw - backW - gap, d->btnH));
    }
    d->sources.setSelected(d->src);

    // Controls: [Featured|Recent] on a server tab, the search field, Search.
    y -= gap + d->btnH;
    float fx = left;
    if (d->official()) {
        const float mw = std::min(du(230), cw * 0.38f);
        d->modes.setRect(CGRectMake(left, y, mw, d->btnH));
        SourceState& s = d->cur();
        d->modes.setSelected(s.mode == WorldBrowser::MODE_SEARCH ? -1 : s.mode);
        fx = left + mw + gap;
    }
    const float goW = d->official() ? du(100) : 0.0f;
    d->search.setRect(CGRectMake(fx, y, right - fx - (goW > 0 ? goW + gap : 0.0f), d->btnH));
    if (d->fieldPt != du(17)) { d->fieldPt = du(17); d->search.setPointSize(d->fieldPt); }   // a raster
    d->go.setRect(CGRectMake(right - goW, y, goW, d->btnH));

    // Action bar.
    d->more.setRect(CGRectMake(left, actY, du(100), d->btnH));
    d->download.setRect(CGRectMake(right - du(150), actY, du(150), d->btnH));
    const bool busy = d->dl != DL_IDLE;
    if (busy != d->shownBusy) {               // setLabel is a raster: only on the flip
        d->shownBusy = busy;
        d->download.setLabel(busy ? "Cancel" : "Download", du(20));
        d->download.setTone(busy ? GLW::Button::TONE_DANGER : GLW::Button::TONE_POSITIVE);
    }
    SourceState& s = d->cur();
    d->download.setEnabled(busy || d->selectedEntry() != NULL);
    d->more.setEnabled(d->official() && s.more && !s.listJob && !busy);

    // Body: the list + scrollbar on the left, the detail pane on the right.
    const float bodyTop = y - gap, bodyBottom = actY + d->btnH + gap;
    const float bodyH = std::max(d->pitch, bodyTop - bodyBottom);
    int vis = (int)std::floor(bodyH / d->pitch);
    if (vis < 1) vis = 1;
    if (vis > WB_MAX_ROWS) vis = WB_MAX_ROWS;
    const float listH = vis * d->pitch, barW = du(20);
    const float listW = std::floor(cw * 0.56f);
    d->list = CGRectMake(left, bodyTop - listH, listW - barW - du(6), listH);
    d->scroll.setBarRect(CGRectMake(left + listW - barW, bodyTop - listH, barW, listH));
    d->scroll.setPitch(d->pitch);
    d->scroll.setRows((int)s.view.size(), vis);
    d->scroll.setFirst(s.first);
    s.first = d->scroll.first();
    if (s.selected >= (int)s.view.size()) s.selected = -1;
    d->detail = CGRectMake(left + listW + gap, bodyBottom, cw - listW - gap, bodyTop - bodyBottom);
    const float dp = du(10);
    const float pwid = d->detail.size.width - 2.0f * dp;
    const float phgt = std::min(pwid * 0.62f, (float)d->detail.size.height * 0.48f);
    d->previewBox = CGRectMake(d->detail.origin.x + dp, d->detail.origin.y + d->detail.size.height - dp - phgt, pwid, phgt);
    for (int r = 0; r < WB_MAX_ROWS; r++) {
        d->rows[r].setRect(CGRectMake(d->list.origin.x, d->list.origin.y + listH - (r + 1) * d->pitch,
                                      d->list.size.width, d->pitch));
        d->rows[r].setLast(r == vis - 1);
    }
    for (int r = 0; r < vis; r++) {
        const int v = s.first + r;
        const std::string t = v < (int)s.view.size() ? s.all[(size_t)s.view[(size_t)v]].name : std::string();
        if (!d->rowBuilt[r] || t != d->rowText[r]) {
            d->rowBuilt[r] = true;
            d->rowText[r] = t;
            d->rows[r].setTitle(t.c_str(), du(19));
        }
        d->rows[r].setSelected(v == s.selected && v < (int)s.view.size());
    }

    // The detail pane's text: rebuilt only when the selection (or the pane width) changes.
    const Entry* e = d->selectedEntry();
    char key[200];
    std::snprintf(key, sizeof(key), "%d:%s:%.0f", d->src, e ? e->id.c_str() : "", pwid);
    if (key != d->detailKey) {
        d->detailKey = key;
        d->name.clear();
        for (int k = 0; k < WB_META; k++) d->meta[k].clear();
        if (e) {
            d->name.set(e->name.c_str(), du(21), UITextAlignmentLeft, pwid);
            std::string lines[WB_META];
            int n = 0;
            if (!e->date.empty())   lines[n++] = (d->official() ? "Shared " : "Published ") + e->date;
            if (!e->author.empty()) lines[n++] = "By " + e->author;
            if (!e->size.empty())   lines[n++] = e->size;
            if (!e->tags.empty())   lines[n++] = "Tags: " + e->tags;
            for (int k = 0; k < n; k++) d->meta[k].set(lines[k].c_str(), du(14), UITextAlignmentLeft, pwid, FACE_BODY);
        }
    }
}

void WorldBrowser::update(float etime) {
    using namespace GLW;
    if (!d->open) return;
    d->clock += etime;
    for (int s = 0; s < SRC_COUNT; s++) d->pollList(s);
    d->pollDownload();
    if (!d->open) return;                     // the download finished and handed back to the menu
    d->pollPreview(etime);
    wb_layout(d);
    SourceState& s = d->cur();

    // The search field. The archive filters as you type (it holds the whole catalogue); a server
    // is asked on Return / Search, as the stock client did.
    // Compared by text, not by EV_CHANGED: typing and Return can arrive in one frame, and then the
    // field reports only the commit.
    const TextField::Event ev = d->search.update();
    if (!d->official() && d->search.text() != s.query) { s.query = d->search.text(); d->refilter(d->src); }
    bool runSearch = d->official() && ev == TextField::EV_COMMIT;

    if (int wheel = eden_ui_take_wheel()) { d->scroll.scrollBy(-wheel); s.first = d->scroll.first(); }

    Input* input = Input::getInput();
    itouch* touches = input->getTouches();
    for (int i = 0; i < MAX_TOUCHES; i++) {
        if (touches[i].inuse == 0 && touches[i].down == M_DOWN) {
            touches[i].inuse = usage_id;
            const float mx = touches[i].mx, my = touches[i].my;
            d->back.setPressed(d->back.hit(mx, my));
            d->download.setPressed(d->download.hit(mx, my));
            d->more.setPressed(d->more.hit(mx, my));
            if (d->official()) d->go.setPressed(d->go.hit(mx, my));
            const int t = d->sources.hit(mx, my);
            if (t >= 0) { d->tabTouch = i; d->sources.setHeld(t); }
            if (d->official()) {
                const int mo = d->modes.hit(mx, my);
                if (mo >= 0) { d->modeTouch = i; d->modes.setHeld(mo); }
            }
            if (d->barTouch < 0 && d->scroll.hitBar(mx, my)) {
                d->barTouch = i;
                d->scroll.barBegin(my);
                s.first = d->scroll.first();
            } else if (d->listTouch < 0 && inbox(mx, my, d->list)) {
                d->listTouch = i;
                d->scroll.beginContentDrag(my);
                d->downRow = -1;
                for (int r = 0; r < d->scroll.visible(); r++)
                    if (d->rows[r].hit(mx, my)) d->downRow = d->scroll.first() + r;
            }
        }
        if (touches[i].inuse == usage_id && touches[i].down == M_DOWN) {
            if (i == d->barTouch) { d->scroll.barDragTo(touches[i].my); s.first = d->scroll.first(); }
            if (i == d->listTouch && d->scroll.contentDragTo(touches[i].my)) { d->downRow = -1; s.first = d->scroll.first(); }
        }
        if (touches[i].inuse == usage_id && touches[i].down == M_RELEASE) {
            const float mx = touches[i].mx, my = touches[i].my;
            touches[i].inuse = 0;
            touches[i].down = M_NONE;
            const bool backHit = d->back.pressed() && d->back.hit(mx, my);
            const bool dlHit = d->download.pressed() && d->download.hit(mx, my);
            const bool moreHit = d->more.pressed() && d->more.hit(mx, my);
            const bool goHit = d->official() && d->go.pressed() && d->go.hit(mx, my);
            d->back.setPressed(false); d->download.setPressed(false); d->more.setPressed(false); d->go.setPressed(false);

            if (i == d->tabTouch) {
                d->tabTouch = -1;
                d->sources.setHeld(-1);
                const int t = d->sources.hit(mx, my);
                if (t >= 0 && t != d->src) {
                    d->search.blur();
                    if (d->dl == DL_IDLE) d->dlStatus.clear();   // the last outcome was another tab's
                    d->src = t;
                    SourceState& ns = d->cur();
                    d->search.setPlaceholder(d->official() ? "Search world names" : "Search names, authors, tags");
                    d->search.setText(ns.query.c_str());
                    if (!ns.loaded && !ns.listJob) d->startList(t, false);
                }
                return;                       // the layout changed under every other control
            }
            if (i == d->modeTouch) {
                d->modeTouch = -1;
                d->modes.setHeld(-1);
                const int mo = d->modes.hit(mx, my);
                if (d->official() && mo >= 0 && (mo != s.mode || !s.loaded)) {
                    d->search.blur();
                    d->search.setText("");
                    s.query.clear();
                    s.mode = s.listMode = mo;
                    d->startList(d->src, false);
                }
                continue;
            }
            if (i == d->barTouch) { d->scroll.endDrag(); d->barTouch = -1; continue; }
            if (i == d->listTouch) {
                const bool scrolled = d->scroll.scrolling();
                d->scroll.endDrag();
                d->listTouch = -1;
                const int row = d->downRow;
                d->downRow = -1;
                if (!scrolled && row >= 0 && row < (int)s.view.size()) {
                    const int r = row - d->scroll.first();
                    if (r >= 0 && r < d->scroll.visible() && d->rows[r].hit(mx, my)) {
                        // Tap selects; a tap on the selection downloads it (the menu's tap-again-to-play).
                        if (row == s.selected && d->dl == DL_IDLE) d->startDownload();
                        else if (d->dl == DL_IDLE) s.selected = row;
                    }
                }
                continue;
            }
            if (d->search.hit(mx, my)) { d->search.focus(); continue; }
            if (d->search.focused()) d->search.blur();
            if (backHit) { close(); return; }
            if (dlHit) {
                if (d->dl != DL_IDLE) d->cancelDownload("Download cancelled");
                else d->startDownload();
            }
            if (moreHit) d->startList(d->src, true);
            if (goHit) runSearch = true;
        }
    }

    if (runSearch) {
        const std::string q = trim(d->search.text());
        d->search.blur();
        s.query = q;
        s.mode = q.empty() ? s.listMode : (int)MODE_SEARCH;
        d->startList(d->src, false);
    }
}

void WorldBrowser::render() {
    using namespace GLW;
    if (!d->open) return;
    wb_layout(d);
    SourceState& s = d->cur();

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);

    fill(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, kScrim);
    bevel(d->panel, BEVEL_WINDOW);
    d->back.render();
    if (d->showTitle) d->title.drawCentered(d->panel.origin.x + d->panel.size.width * 0.5f, d->titleY, kText);
    d->sources.render();
    if (d->official()) { d->modes.render(); d->go.render(); }
    d->search.render();

    // The list, or what it is waiting for.
    bevel(d->list, BEVEL_CONTENT);
    std::string emptyText;
    if (s.view.empty()) {
        if (s.listJob) emptyText = "Loading worlds...";
        else if (!s.status.empty()) emptyText = "Couldn't load the list";
        else if (s.loaded) emptyText = "No worlds found";
    }
    if (emptyText != d->shownEmpty) {
        d->shownEmpty = emptyText;
        if (emptyText.empty()) { d->empty.clear(); d->emptyHint.clear(); }
        else {
            d->empty.set(emptyText.c_str(), du(22), UITextAlignmentCenter);
            const char* hint = s.listJob ? "" : !s.status.empty() ? "Check the connection, or try another tab."
                                                                  : "Try a different search.";
            d->emptyHint.set(hint, du(14), UITextAlignmentCenter, d->list.size.width - du(20), FACE_BODY);
        }
    }
    if (!d->empty.empty()) {
        const float cx = d->list.origin.x + d->list.size.width * 0.5f;
        const float h = d->empty.height() + du(6) + d->emptyHint.height();
        const float yTop = d->list.origin.y + (d->list.size.height + h) * 0.5f;
        d->empty.drawCentered(cx, yTop, kText);
        d->emptyHint.drawCentered(cx, yTop - d->empty.height() - du(6), kTextSecondary);
    } else {
        for (int r = 0; r < d->scroll.visible() && d->scroll.first() + r < (int)s.view.size(); r++) d->rows[r].render();
    }
    d->scroll.render();

    // The detail pane.
    bevel(d->detail, BEVEL_CONTENT);
    const Entry* e = d->selectedEntry();
    const float dp = du(10);
    float yTop = d->previewBox.origin.y - du(8);
    if (e) {
        CGRect pb = d->previewBox;
        fill(pb, rgb(0x2c2b2b));
        if (d->previewTex && d->previewAspect > 0.0f) {
            float w = pb.size.width, h = w / d->previewAspect;
            if (h > pb.size.height) { h = pb.size.height; w = h * d->previewAspect; }
            const CGRect r = CGRectMake(pb.origin.x + (pb.size.width - w) * 0.5f, pb.origin.y + (pb.size.height - h) * 0.5f, w, h);
            glColor4f(1, 1, 1, 1);
            d->previewTex->drawInRect(r);
        } else if (d->previewMissing) {
            d->noPreview.drawCentered(pb.origin.x + pb.size.width * 0.5f,
                                      pb.origin.y + (pb.size.height + d->noPreview.height()) * 0.5f, kTextPressed);
        } else {
            progressBar(CGRectMake(pb.origin.x + pb.size.width * 0.25f, pb.origin.y + pb.size.height * 0.5f - du(5),
                                   pb.size.width * 0.5f, du(10)), -1.0f, d->clock * 0.8f);
        }
        bevel(pb, BEVEL_SUNKEN);
        d->name.draw(d->detail.origin.x + dp, yTop, kText);
        yTop -= d->name.height() + du(4);
        for (int k = 0; k < WB_META; k++) {
            if (d->meta[k].empty()) continue;
            d->meta[k].draw(d->detail.origin.x + dp, yTop, kTextSecondary);
            yTop -= d->meta[k].height();
        }
    }

    // Download progress at the foot of the pane; the outcome of the last one when idle.
    std::string prog = d->dlStatus;
    if (d->dl != DL_IDLE && d->dlSrc == d->src && !d->dlEntry.name.empty()) prog = d->dlEntry.name + ": " + prog;
    if (prog != d->shownProgress) {
        d->shownProgress = prog;
        if (prog.empty()) d->progressText.clear();
        else d->progressText.set(prog.c_str(), du(14), UITextAlignmentLeft, d->detail.size.width - 2.0f * dp, FACE_BODY);
    }
    float foot = d->detail.origin.y + dp;
    if (d->dl != DL_IDLE) {
        progressBar(CGRectMake(d->detail.origin.x + dp, foot, d->detail.size.width - 2.0f * dp, du(18)),
                    d->dlFrac, d->clock * 0.8f);
        foot += du(18) + du(6);
    }
    if (!d->progressText.empty())
        d->progressText.draw(d->detail.origin.x + dp, foot + d->progressText.height(), kText);

    // The action bar and its status line.
    if (d->official()) d->more.render();
    d->download.render();
    std::string st = s.status;
    if (st.empty() && s.listJob) st = "Loading...";
    else if (st.empty() && s.loaded) {
        char buf[96];
        if (!d->official() && !trim(s.query).empty())
            std::snprintf(buf, sizeof(buf), "%d of %d worlds", (int)s.view.size(), (int)s.all.size());
        else std::snprintf(buf, sizeof(buf), "%d worlds", (int)s.view.size());
        st = buf;
    }
    if (st != d->shownStatus) {
        d->shownStatus = st;
        const float sx = (d->official() ? d->more.rect().origin.x + d->more.rect().size.width : d->more.rect().origin.x) + du(12);
        const float sw = d->download.rect().origin.x - du(12) - sx;
        if (st.empty()) d->status.clear();
        else d->status.set(st.c_str(), du(14), UITextAlignmentLeft, sw, FACE_BODY);
    }
    if (!d->status.empty()) {
        const float sx = (d->official() ? d->more.rect().origin.x + d->more.rect().size.width : d->more.rect().origin.x) + du(12);
        const CGRect a = d->download.rect();
        d->status.draw(sx, a.origin.y + (a.size.height + d->status.height()) * 0.5f, kTextSecondary);
    }
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
}

// --- harness surface -------------------------------------------------------------------------
CGRect WorldBrowser::controlRect(const char* which) {
    wb_layout(d);
    if (!std::strcmp(which, "back"))      return d->back.rect();
    if (!std::strcmp(which, "download"))  return d->download.rect();
    if (!std::strcmp(which, "more"))      return d->more.rect();
    if (!std::strcmp(which, "search"))    return d->search.rect();
    if (!std::strcmp(which, "go"))        return d->go.rect();
    if (!std::strcmp(which, "list"))      return d->list;
    if (!std::strcmp(which, "scrollbar")) return d->scroll.barRect();
    return CGRectMake(0, 0, 0, 0);
}
CGRect WorldBrowser::tabRect(int source) { wb_layout(d); return d->sources.tabRect(source); }
CGRect WorldBrowser::modeRect(int mode) { wb_layout(d); return d->modes.tabRect(mode); }
bool WorldBrowser::rowRect(int index, CGRect* r) {
    wb_layout(d);
    const int k = index - d->scroll.first();
    if (index < 0 || k < 0 || k >= d->scroll.visible() || index >= d->scroll.total()) return false;
    if (r) *r = d->rows[k].rect();
    return true;
}
void WorldBrowser::showRow(int index) {
    wb_layout(d);
    d->scroll.ensureVisible(index);
    d->cur().first = d->scroll.first();
}
int WorldBrowser::source() const { return d->src; }
int WorldBrowser::mode() const { return d->S[d->src].mode; }
int WorldBrowser::entryCount() const { return (int)d->S[d->src].view.size(); }
const char* WorldBrowser::entryName(int i) const {
    const SourceState& s = d->S[d->src];
    return (i >= 0 && i < (int)s.view.size()) ? s.all[(size_t)s.view[(size_t)i]].name.c_str() : "";
}
const char* WorldBrowser::entryId(int i) const {
    const SourceState& s = d->S[d->src];
    return (i >= 0 && i < (int)s.view.size()) ? s.all[(size_t)s.view[(size_t)i]].id.c_str() : "";
}
const char* WorldBrowser::entrySize(int i) const {
    const SourceState& s = d->S[d->src];
    return (i >= 0 && i < (int)s.view.size()) ? s.all[(size_t)s.view[(size_t)i]].size.c_str() : "";
}
int  WorldBrowser::selectedIndex() const { return d->S[d->src].selected; }
bool WorldBrowser::listLoading() const { return d->S[d->src].listJob != 0; }
bool WorldBrowser::busy() const { return d->dl != DL_IDLE; }
bool WorldBrowser::previewShown() const { return d->previewTex != NULL; }
const char* WorldBrowser::statusText() const {
    return !d->dlStatus.empty() ? d->dlStatus.c_str() : d->S[d->src].status.c_str();
}
