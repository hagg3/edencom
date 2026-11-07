// K.2-1: the content-pack JSON reader (Json.h).
#include "Json.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace Json {

namespace {

struct Reader {
    const char* p;
    size_t n, i = 0;
    Error* err;
    bool fail(const char* msg) {
        if (err) { err->offset = i; err->message = msg; }
        return false;
    }
    bool failAt(size_t at, const char* msg) { i = at; return fail(msg); }

    // Whitespace and `//` comments. A lone '/' is an error.
    bool skip() {
        for (;;) {
            while (i < n && (p[i] == ' ' || p[i] == '\t' || p[i] == '\n' || p[i] == '\r')) ++i;
            if (i < n && p[i] == '/') {
                if (i + 1 < n && p[i + 1] == '/') {
                    while (i < n && p[i] != '\n') ++i;
                    continue;
                }
                return fail("'/' that does not start a // comment");
            }
            return true;
        }
    }

    bool literal(const char* word, size_t len) {
        if (n - i < len || memcmp(p + i, word, len) != 0) return fail("unexpected character");
        i += len;
        return true;
    }

    static int hexval(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }
    bool hex4(unsigned* out) {
        if (n - i < 4) return fail("truncated \\u escape");
        unsigned v = 0;
        for (int k = 0; k < 4; ++k) {
            const int h = hexval(p[i + k]);
            if (h < 0) { i += k; return fail("bad hex digit in \\u escape"); }
            v = v * 16 + (unsigned)h;
        }
        i += 4;
        *out = v;
        return true;
    }
    static void putUtf8(std::string* s, unsigned cp) {
        if (cp < 0x80) s->push_back((char)cp);
        else if (cp < 0x800) { s->push_back((char)(0xC0 | (cp >> 6))); s->push_back((char)(0x80 | (cp & 0x3F))); }
        else if (cp < 0x10000) {
            s->push_back((char)(0xE0 | (cp >> 12))); s->push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            s->push_back((char)(0x80 | (cp & 0x3F)));
        } else {
            s->push_back((char)(0xF0 | (cp >> 18))); s->push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
            s->push_back((char)(0x80 | ((cp >> 6) & 0x3F))); s->push_back((char)(0x80 | (cp & 0x3F)));
        }
    }
    // One raw UTF-8 sequence starting at p[i] (>= 0x80): well-formed, shortest form, no surrogates.
    bool utf8(std::string* s) {
        const unsigned char c = (unsigned char)p[i];
        int len; unsigned cp, min;
        if (c >= 0xC2 && c <= 0xDF) { len = 2; cp = c & 0x1F; min = 0x80; }
        else if (c >= 0xE0 && c <= 0xEF) { len = 3; cp = c & 0x0F; min = 0x800; }
        else if (c >= 0xF0 && c <= 0xF4) { len = 4; cp = c & 0x07; min = 0x10000; }
        else return fail("invalid UTF-8");
        if (n - i < (size_t)len) return fail("invalid UTF-8");
        for (int k = 1; k < len; ++k) {
            const unsigned char d = (unsigned char)p[i + k];
            if ((d & 0xC0) != 0x80) return fail("invalid UTF-8");
            cp = (cp << 6) | (d & 0x3F);
        }
        if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return fail("invalid UTF-8");
        s->append(p + i, (size_t)len);
        i += (size_t)len;
        return true;
    }

    bool string(std::string* s) {
        ++i;   // the opening quote
        for (;;) {
            if (i >= n) return fail("unterminated string");
            const unsigned char c = (unsigned char)p[i];
            if (c == '"') { ++i; return true; }
            if (c < 0x20) return fail("control character in string");
            if (c >= 0x80) { if (!utf8(s)) return false; continue; }
            if (c != '\\') { s->push_back((char)c); ++i; continue; }
            const size_t esc = i;
            if (++i >= n) return fail("unterminated string");
            const char e = p[i++];
            switch (e) {
                case '"': s->push_back('"'); break;
                case '\\': s->push_back('\\'); break;
                case '/': s->push_back('/'); break;
                case 'b': s->push_back('\b'); break;
                case 'f': s->push_back('\f'); break;
                case 'n': s->push_back('\n'); break;
                case 'r': s->push_back('\r'); break;
                case 't': s->push_back('\t'); break;
                case 'u': {
                    unsigned cp;
                    if (!hex4(&cp)) return false;
                    if (cp >= 0xDC00 && cp <= 0xDFFF) return failAt(esc, "unpaired surrogate in \\u escape");
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        unsigned lo;
                        if (n - i < 2 || p[i] != '\\' || p[i + 1] != 'u') return failAt(esc, "unpaired surrogate in \\u escape");
                        i += 2;
                        if (!hex4(&lo)) return false;
                        if (lo < 0xDC00 || lo > 0xDFFF) return failAt(esc, "unpaired surrogate in \\u escape");
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    putUtf8(s, cp);
                    break;
                }
                default: return failAt(esc, "bad escape in string");
            }
        }
    }

    // RFC 8259 number grammar, then strtod on the validated span.
    bool number(double* out) {
        const size_t start = i;
        if (p[i] == '-') ++i;
        if (i >= n) return fail("bad number");
        if (p[i] == '0') ++i;
        else if (p[i] >= '1' && p[i] <= '9') { while (i < n && p[i] >= '0' && p[i] <= '9') ++i; }
        else return fail("bad number");
        if (i < n && p[i] == '.') {
            ++i;
            if (i >= n || p[i] < '0' || p[i] > '9') return fail("bad number: digit expected after '.'");
            while (i < n && p[i] >= '0' && p[i] <= '9') ++i;
        }
        if (i < n && (p[i] == 'e' || p[i] == 'E')) {
            ++i;
            if (i < n && (p[i] == '+' || p[i] == '-')) ++i;
            if (i >= n || p[i] < '0' || p[i] > '9') return fail("bad number: digit expected in exponent");
            while (i < n && p[i] >= '0' && p[i] <= '9') ++i;
        }
        const std::string text(p + start, i - start);
        const double v = strtod(text.c_str(), NULL);
        if (!isfinite(v)) return failAt(start, "number out of range");
        *out = v;
        return true;
    }

    bool value(Value* v, int depth) {
        if (!skip()) return false;
        if (i >= n) return fail("value expected");
        v->offset = i;
        const char c = p[i];
        if (c == '{' || c == '[') {
            if (depth >= kMaxDepth) return fail("nesting deeper than 32");
            ++i;
            const bool isObj = c == '{';
            const char close = isObj ? '}' : ']';
            v->type = isObj ? Object : Array;
            if (!skip()) return false;
            if (i < n && p[i] == close) { ++i; return true; }
            for (;;) {
                if (isObj) {
                    if (!skip()) return false;
                    if (i >= n || p[i] != '"') return fail("object key (a string) expected");
                    const size_t keyAt = i;
                    std::string key;
                    if (!string(&key)) return false;
                    for (const auto& m : v->obj)
                        if (m.first == key) return failAt(keyAt, "duplicate key");
                    if (!skip()) return false;
                    if (i >= n || p[i] != ':') return fail("':' expected");
                    ++i;
                    v->obj.emplace_back(std::move(key), Value());
                    if (!value(&v->obj.back().second, depth + 1)) return false;
                } else {
                    v->arr.emplace_back();
                    if (!value(&v->arr.back(), depth + 1)) return false;
                }
                if (!skip()) return false;
                if (i >= n) return fail(isObj ? "',' or '}' expected" : "',' or ']' expected");
                if (p[i] == close) { ++i; return true; }
                if (p[i] != ',') return fail(isObj ? "',' or '}' expected" : "',' or ']' expected");
                ++i;
                if (!skip()) return false;
                if (i < n && p[i] == close) return fail("trailing comma");
            }
        }
        if (c == '"') { v->type = String; return string(&v->str); }
        if (c == 't') { v->type = Bool; v->b = true; return literal("true", 4); }
        if (c == 'f') { v->type = Bool; v->b = false; return literal("false", 5); }
        if (c == 'n') { v->type = Null; return literal("null", 4); }
        if (c == '-' || (c >= '0' && c <= '9')) { v->type = Number; return number(&v->num); }
        return fail("unexpected character");
    }
};

void writeString(std::string* o, const std::string& s) {
    o->push_back('"');
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o->push_back('\\'); o->push_back((char)c); }
        else if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o->append(b); }
        else o->push_back((char)c);
    }
    o->push_back('"');
}

void writeValue(std::string* o, const Value& v) {
    switch (v.type) {
        case Null: o->append("null"); break;
        case Bool: o->append(v.b ? "true" : "false"); break;
        case Number: {
            char b[40];
            if (v.num == floor(v.num) && fabs(v.num) < 9007199254740992.0)
                snprintf(b, sizeof b, "%lld", (long long)v.num);
            else
                snprintf(b, sizeof b, "%.17g", v.num);
            o->append(b);
            break;
        }
        case String: writeString(o, v.str); break;
        case Array:
            o->push_back('[');
            for (size_t k = 0; k < v.arr.size(); ++k) { if (k) o->push_back(','); writeValue(o, v.arr[k]); }
            o->push_back(']');
            break;
        case Object:
            o->push_back('{');
            for (size_t k = 0; k < v.obj.size(); ++k) {
                if (k) o->push_back(',');
                writeString(o, v.obj[k].first);
                o->push_back(':');
                writeValue(o, v.obj[k].second);
            }
            o->push_back('}');
            break;
    }
}

}  // namespace

const Value* Value::get(const char* key) const {
    if (type != Object) return NULL;
    for (const auto& m : obj) if (m.first == key) return &m.second;
    return NULL;
}

bool Value::asInt(int64_t* out, int64_t lo, int64_t hi) const {
    if (type != Number || num != floor(num) || fabs(num) >= 9007199254740992.0) return false;
    const int64_t v = (int64_t)num;
    if (v < lo || v > hi) return false;
    if (out) *out = v;
    return true;
}

bool parse(const char* data, size_t len, Value* out, Error* err) {
    *out = Value();
    if (len > kMaxBytes) {
        if (err) { err->offset = kMaxBytes; err->message = "document larger than 1 MiB"; }
        return false;
    }
    Reader r{data, len, 0, err};
    // A UTF-8 BOM (EF BB BF), compared by value: tools/ui-string-gate.py refuses non-ASCII literals.
    const unsigned char* u = (const unsigned char*)data;
    if (len >= 3 && u[0] == 0xEF && u[1] == 0xBB && u[2] == 0xBF) r.i = 3;
    if (!r.value(out, 0)) { *out = Value(); return false; }
    if (!r.skip()) { *out = Value(); return false; }
    if (r.i != len) { r.fail("text after the value"); *out = Value(); return false; }
    return true;
}

std::string write(const Value& v) {
    std::string o;
    writeValue(&o, v);
    return o;
}

}  // namespace Json
