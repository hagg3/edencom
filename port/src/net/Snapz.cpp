// Snapz.cpp — see Snapz.h.
#include "Snapz.h"

#include <cstring>
#include <zlib.h>

namespace eden_net {

static const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64Encode(const void* p, size_t n, bool pad) {
    const unsigned char* d = (const unsigned char*)p;
    std::string o;
    o.reserve((n + 2) / 3 * 4);
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)d[i] << 16;
        if (i + 1 < n) v |= (uint32_t)d[i + 1] << 8;
        if (i + 2 < n) v |= d[i + 2];
        o += kB64[(v >> 18) & 63];
        o += kB64[(v >> 12) & 63];
        if (i + 1 < n) o += kB64[(v >> 6) & 63]; else if (pad) o += '=';
        if (i + 2 < n) o += kB64[v & 63]; else if (pad) o += '=';
    }
    return o;
}

bool base64Decode(const char* p, size_t n, std::string& out) {
    out.clear();
    while (n && p[n - 1] == '=') --n;   // tolerate padding
    if (n % 4 == 1) return false;
    out.reserve(n / 4 * 3 + 3);
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < n; ++i) {
        const char c = p[i];
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+') v = 62;
        else if (c == '/') v = 63;
        else return false;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) { bits -= 8; out += (char)((acc >> bits) & 0xFF); }
    }
    return true;
}

static int32_t rdLE(const unsigned char* p) {
    return (int32_t)((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24);
}

bool parseSnapzLine(const std::string& line, std::vector<SnapzRec>& recs, const char** err) {
    const char* e = nullptr;
    if (!err) err = &e;
    recs.clear();
    if (line.compare(0, 6, "SNAPZ:") != 0) { *err = "not a SNAPZ line"; return false; }
    const size_t c2 = line.find(':', 6);
    if (c2 == std::string::npos || c2 == 6 || c2 - 6 > 9) { *err = "bad SNAPZ count"; return false; }
    size_t count = 0;
    for (size_t i = 6; i < c2; ++i) {
        if (line[i] < '0' || line[i] > '9') { *err = "bad SNAPZ count"; return false; }
        count = count * 10 + (size_t)(line[i] - '0');
    }
    if (count > kSnapzMaxRecords) { *err = "SNAPZ count too large"; return false; }
    std::string raw;
    if (!base64Decode(line.data() + c2 + 1, line.size() - c2 - 1, raw)) { *err = "bad SNAPZ base64"; return false; }
    const size_t expect = count * kSnapzRecBytes;
    if (expect == 0 && raw.empty()) return true;   // tolerate a bare empty body
    // deflate cannot beat ~1032:1, so a header claiming more than the payload can hold is a lie
    if (expect > raw.size() * 1040 + 64) { *err = "SNAPZ count exceeds payload"; return false; }

    std::vector<unsigned char> buf(expect);
    z_stream s;
    std::memset(&s, 0, sizeof s);
    if (inflateInit2(&s, -15) != Z_OK) { *err = "inflate init failed"; return false; }
    s.next_in = (Bytef*)raw.data();
    s.avail_in = (uInt)raw.size();
    s.next_out = buf.empty() ? (Bytef*)&e : (Bytef*)buf.data();
    s.avail_out = (uInt)expect;
    int rc = inflate(&s, Z_FINISH);
    const size_t produced = s.total_out;
    const bool trailing = s.avail_in != 0;
    inflateEnd(&s);
    if (rc != Z_STREAM_END) {
        *err = rc == Z_BUF_ERROR && produced == expect ? "SNAPZ longer than count" : "SNAPZ inflate failed";
        return false;
    }
    if (produced != expect) { *err = "SNAPZ shorter than count"; return false; }
    if (trailing) { *err = "SNAPZ trailing bytes"; return false; }
    recs.resize(count);
    for (size_t i = 0; i < count; ++i) {
        const unsigned char* q = buf.data() + i * kSnapzRecBytes;
        recs[i] = SnapzRec{rdLE(q), rdLE(q + 4), rdLE(q + 8), rdLE(q + 12), rdLE(q + 16)};
    }
    return true;
}

std::string encodeSnapzLine(const SnapzRec* recs, size_t n, int level) {
    std::vector<unsigned char> raw(n * kSnapzRecBytes);
    for (size_t i = 0; i < n; ++i) {
        const int32_t f[5] = {recs[i].x, recs[i].y, recs[i].z, recs[i].flag, recs[i].type};
        for (int k = 0; k < 5; ++k)
            for (int b = 0; b < 4; ++b) raw[i * 20 + (size_t)k * 4 + (size_t)b] = (unsigned char)((uint32_t)f[k] >> (8 * b));
    }
    z_stream s;
    std::memset(&s, 0, sizeof s);
    deflateInit2(&s, level, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY);
    std::vector<unsigned char> out(deflateBound(&s, (uLong)raw.size()) + 16);
    unsigned char dummy = 0;
    s.next_in = raw.empty() ? &dummy : raw.data();
    s.avail_in = (uInt)raw.size();
    s.next_out = out.data();
    s.avail_out = (uInt)out.size();
    deflate(&s, Z_FINISH);
    out.resize(out.size() - s.avail_out);
    deflateEnd(&s);
    return "SNAPZ:" + std::to_string(n) + ":" + base64Encode(out.data(), out.size());
}

}  // namespace eden_net
