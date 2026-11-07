//
//  PackInstall.cpp
//  Emod — K.3. See PackInstall.h and docs/content-packs.md "Installing packs".
//
#include "PackInstall.h"
#include "ContentPack.h"
#include "EdenWorldStore.h"
#include "Json.h"
#include "eden_fs.h"

#include <zlib.h>
#include <stdio.h>
#include <string.h>
#include <map>
#include <set>

namespace PackInstall {

namespace {

std::string hex(const uint8_t* p, size_t n) {
    static const char* hx = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; ++i) { s += hx[p[i] >> 4]; s += hx[p[i] & 15]; }
    return s;
}

bool readAll(const std::string& path, size_t cap, std::string* out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    out->clear();
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
        out->append(buf, n);
        if (out->size() > cap) break;
    }
    const bool ok = !ferror(f);
    fclose(f);
    return ok;
}

bool writeAll(const std::string& path, const std::string& b) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    const bool ok = fwrite(b.data(), 1, b.size(), f) == b.size();
    return fclose(f) == 0 && ok;
}

unsigned u16(const std::string& b, size_t o) { return (unsigned)(uint8_t)b[o] | (unsigned)(uint8_t)b[o + 1] << 8; }
uint32_t u32(const std::string& b, size_t o) { return u16(b, o) | (uint32_t)u16(b, o + 2) << 16; }

// A zip entry name we will write: relative, `/`-separated, no empty/`.`/`..` component, no `\` `:` or control
// byte, <= 200 bytes. (The loader's relPathOk, plus the length.)
bool nameOk(const std::string& p) {
    if (p.empty() || p.size() > 200 || p[0] == '/') return false;
    size_t at = 0;
    while (at <= p.size()) {
        size_t e = p.find('/', at);
        if (e == std::string::npos) e = p.size();
        const std::string c = p.substr(at, e - at);
        if (c.empty() || c == "." || c == "..") return false;
        at = e + 1;
    }
    for (char c : p)
        if (c == '\\' || c == ':' || (unsigned char)c < 0x20) return false;
    return true;
}

struct Entry { std::string name; unsigned method, flags; uint32_t crc, csize, usize, local; };

// The central directory. False + `why` on anything we do not read (zip64, multi-disk, encryption).
bool readDirectory(const std::string& z, std::vector<Entry>* out, std::string* why) {
    if (z.size() < 22) { *why = "not a zip file"; return false; }
    size_t eocd = std::string::npos;
    const size_t lo = z.size() > 22 + 65535 ? z.size() - 22 - 65535 : 0;
    for (size_t i = z.size() - 22 + 1; i-- > lo;)
        if (u32(z, i) == 0x06054b50) { eocd = i; break; }
    if (eocd == std::string::npos) { *why = "not a zip file"; return false; }
    const unsigned count = u16(z, eocd + 10);
    const uint32_t size = u32(z, eocd + 12), off = u32(z, eocd + 16);
    if (u16(z, eocd + 4) != 0 || u16(z, eocd + 6) != 0 || count != u16(z, eocd + 8)) { *why = "a multi-part zip"; return false; }
    if (count == 0xffff || off == 0xffffffffu) { *why = "a zip64 file (too big for a pack)"; return false; }
    if (count > ZIP_MAX_ENTRIES) { *why = "too many files in the zip"; return false; }
    if ((uint64_t)off + size > eocd) { *why = "a damaged zip"; return false; }
    size_t at = off;
    for (unsigned i = 0; i < count; ++i) {
        if (at + 46 > eocd || u32(z, at) != 0x02014b50) { *why = "a damaged zip"; return false; }
        Entry e;
        e.flags = u16(z, at + 8);
        e.method = u16(z, at + 10);
        e.crc = u32(z, at + 16);
        e.csize = u32(z, at + 20);
        e.usize = u32(z, at + 24);
        const unsigned nl = u16(z, at + 28), xl = u16(z, at + 30), cl = u16(z, at + 32);
        e.local = u32(z, at + 42);
        if (at + 46 + nl + xl + cl > eocd) { *why = "a damaged zip"; return false; }
        e.name = z.substr(at + 46, nl);
        at += 46 + nl + xl + cl;
        out->push_back(e);
    }
    return true;
}

bool extract(const std::string& z, const Entry& e, std::string* out, std::string* why) {
    if (e.local + 30ull > z.size() || u32(z, e.local) != 0x04034b50) { *why = "a damaged zip"; return false; }
    const size_t data = (size_t)e.local + 30 + u16(z, e.local + 26) + u16(z, e.local + 28);
    if (data + e.csize > z.size()) { *why = "a damaged zip"; return false; }
    out->clear();
    if (e.method == 0) {
        if (e.csize != e.usize) { *why = "a damaged zip"; return false; }
        out->assign(z, data, e.csize);
    } else {
        out->resize(e.usize);
        z_stream s;
        memset(&s, 0, sizeof s);
        if (inflateInit2(&s, -15) != Z_OK) { *why = "cannot unpack"; return false; }
        s.next_in = (Bytef*)z.data() + data;
        s.avail_in = e.csize;
        s.next_out = (Bytef*)&(*out)[0];
        s.avail_out = e.usize;
        const int r = e.usize ? inflate(&s, Z_FINISH) : Z_STREAM_END;
        const bool ok = r == Z_STREAM_END && s.total_out == e.usize;
        inflateEnd(&s);
        if (!ok) { *why = "a damaged zip (" + e.name + " does not unpack)"; return false; }
    }
    if ((uint32_t)crc32(0, (const Bytef*)out->data(), (uInt)out->size()) != e.crc) {
        *why = "a damaged zip (" + e.name + " fails its checksum)";
        return false;
    }
    return true;
}

bool skippedName(const std::string& n) {
    // Finder's resource forks and dot files: never part of a pack.
    if (n.compare(0, 9, "__MACOSX/") == 0) return true;
    size_t at = 0;
    while (at < n.size()) {
        if (n[at] == '.') return true;
        const size_t s = n.find('/', at);
        if (s == std::string::npos) break;
        at = s + 1;
    }
    return false;
}

}  // namespace

std::string fileSha256(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return "";
    emod::Sha256 sha;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) sha.update(buf, n);
    const bool bad = ferror(f);
    fclose(f);
    if (bad) return "";
    uint8_t d[32];
    sha.final(d);
    return hex(d, 32);
}

Result installZip(const std::string& zipPath, const std::string& documents, const std::string& expectSha256) {
    Result r;
    const std::string packs = documents + "/Packs", stage = packs + "/.install.partial", old = packs + "/.install.old";
    std::string z;
    if (documents.empty() || !eden_fs_make_dirs(packs)) { r.error = "Cannot make the Packs folder"; return r; }
    if (!readAll(zipPath, ZIP_MAX_FILE_BYTES, &z)) { r.error = "Cannot read the file"; return r; }
    if (z.size() > ZIP_MAX_FILE_BYTES) { r.error = "The file is too big for a pack (32 MB at most)"; return r; }
    if (!expectSha256.empty()) {
        emod::Sha256 sha;
        sha.update(z.data(), z.size());
        uint8_t d[32];
        sha.final(d);
        if (hex(d, 32) != expectSha256) { r.error = "The download does not match the catalogue (checksum)"; return r; }
    }
    std::vector<Entry> all;
    std::string why;
    if (!readDirectory(z, &all, &why)) { r.error = "The file is " + why; return r; }
    // The files, minus folders and dot files; then the common top-level folder, if pack.json is not at the root.
    std::vector<Entry> files;
    for (const Entry& e : all) {
        if (!e.name.empty() && e.name.back() == '/') continue;
        if (!nameOk(e.name)) { r.error = "The zip holds a file with an unsafe name (" + e.name.substr(0, 60) + ")"; return r; }
        if (skippedName(e.name)) continue;
        if (e.flags & 1) { r.error = "The zip is encrypted"; return r; }
        if (e.method != 0 && e.method != 8) { r.error = "The zip uses a compression this game cannot read"; return r; }
        files.push_back(e);
    }
    bool rootPack = false;
    for (const Entry& e : files) rootPack |= e.name == "pack.json";
    if (!rootPack) {
        std::string top;
        bool same = !files.empty();
        for (const Entry& e : files) {
            const size_t s = e.name.find('/');
            const std::string t = s == std::string::npos ? "" : e.name.substr(0, s + 1);
            if (t.empty() || (!top.empty() && t != top)) { same = false; break; }
            top = t;
        }
        bool found = false;
        if (same) for (const Entry& e : files) found |= e.name == top + "pack.json";
        if (!found) { r.error = "The zip has no pack.json (at the top, or in one folder)"; return r; }
        for (Entry& e : files) e.name = e.name.substr(top.size());
    }
    uint64_t total = 0;
    for (const Entry& e : files) {
        if (e.usize > ZIP_MAX_ENTRY_BYTES) { r.error = "A file in the zip is too big (" + e.name.substr(0, 60) + ")"; return r; }
        total += e.usize;
    }
    if (total > ZIP_MAX_TOTAL_BYTES) { r.error = "The pack is too big (16 MB unpacked at most)"; return r; }

    // Unpack into the staging folder (a leftover from a crash is removed first).
    if (eden_fs_exists(stage)) eden_fs_remove_tree(stage);
    if (eden_fs_exists(old)) eden_fs_remove_tree(old);
    auto fail = [&](const std::string& e) { eden_fs_remove_tree(stage); r.error = e; return r; };
    if (!eden_fs_make_dirs(stage)) return fail("Cannot write to the Packs folder");
    std::set<std::string> written;
    for (const Entry& e : files) {
        std::string body;
        if (!extract(z, e, &body, &why)) return fail("The file is " + why);
        if (!written.insert(e.name).second) return fail("The zip holds " + e.name.substr(0, 60) + " twice");
        const size_t s = e.name.find_last_of('/');
        if (s != std::string::npos && !eden_fs_make_dirs(stage + "/" + e.name.substr(0, s))) return fail("Cannot write to the Packs folder");
        if (!writeAll(stage + "/" + e.name, body)) return fail("Cannot write to the Packs folder");
    }
    std::string text;
    if (!ContentPack::readPackFile(stage + "/pack.json", &text)) return fail("pack.json is unreadable or over 1 MB");
    ContentPack::ModPack pk;
    ContentPack::compileMod(text.data(), text.size(), stage, "pack.json", &pk);
    r.lines = pk.lines;
    r.id = pk.id;
    r.name = pk.name;
    r.version = pk.version;
    if (pk.refused) return fail("The pack has errors and was not installed");

    // Commit: replace the folder that holds this id, or make Packs/<id>.
    std::string target;
    for (const std::string& n : eden_fs_list(packs)) {
        if (n.empty() || n[0] == '.' || !eden_fs_is_dir(packs + "/" + n)) continue;
        std::string t;
        Json::Value v;
        Json::Error je;
        if (ContentPack::readPackFile(packs + "/" + n + "/pack.json", &t) && Json::parse(t.data(), t.size(), &v, &je) &&
            v.isObject() && v.get("id") && v.get("id")->isString() && v.get("id")->str == pk.id) {
            target = n;
            break;
        }
    }
    if (target.empty()) {
        target = pk.id;
        if (eden_fs_exists(packs + "/" + target))
            return fail("A folder named " + target + " is already in Packs (it holds another pack)");
    } else {
        if (!eden_fs_move_new(packs + "/" + target, old)) return fail("Cannot replace the installed pack");
        r.replaced = true;
    }
    if (!eden_fs_move_new(stage, packs + "/" + target)) {
        if (r.replaced) eden_fs_move_new(old, packs + "/" + target);   // put the old one back
        return fail("Cannot write to the Packs folder");
    }
    if (r.replaced) eden_fs_remove_tree(old);
    eden_fs_sync_dir(packs);
    r.folder = target;
    r.ok = true;
    return r;
}

bool parseCatalogue(const char* data, size_t len, std::vector<CatalogueEntry>* out, int* dropped, std::string* error) {
    out->clear();
    if (dropped) *dropped = 0;
    Json::Value root;
    Json::Error je;
    if (!Json::parse(data, len, &root, &je)) { *error = "The catalogue is not JSON (byte " + std::to_string(je.offset) + ": " + je.message + ")"; return false; }
    const Json::Value* f = root.isObject() ? root.get("format") : NULL;
    if (!f || !f->isNumber() || f->num != 1) { *error = "The catalogue is not format 1"; return false; }
    const Json::Value* pv = root.get("packs");
    if (!pv || !pv->isArray()) { *error = "The catalogue has no \"packs\" list"; return false; }
    std::set<std::string> ids;
    for (const Json::Value& e : pv->arr) {
        auto str = [&](const char* k) -> std::string {
            const Json::Value* v = e.isObject() ? e.get(k) : NULL;
            return v && v->isString() ? v->str : std::string();
        };
        CatalogueEntry c;
        c.id = str("id");
        c.name = str("name");
        c.version = str("version");
        c.description = str("description");
        c.author = str("author");
        c.url = str("url");
        c.sha256 = str("sha256");
        const Json::Value* sz = e.isObject() ? e.get("size") : NULL;
        if (sz && sz->isNumber() && sz->num >= 0) c.size = (long long)sz->num;
        const Json::Value* rq = e.isObject() ? e.get("requires") : NULL;
        if (rq && rq->isArray()) for (const Json::Value& x : rq->arr) if (x.isString()) c.needs.push_back(x.str);
        bool ok = !c.id.empty() && c.id.size() <= 24 && c.sha256.size() == 64 &&
                  (c.url.compare(0, 8, "https://") == 0 || c.url.compare(0, 7, "http://") == 0);
        for (char ch : c.id) ok &= (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_';
        for (char ch : c.sha256) ok &= (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
        if (ok && c.id != "eden" && c.id != "emod" && ids.insert(c.id).second) out->push_back(c);
        else if (dropped) ++*dropped;
    }
    return true;
}

}  // namespace PackInstall
