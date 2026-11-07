// K.2-1: a small JSON DOM for content packs (WORKING/data-driven-content-plan-2026-11-01.md D2/D3).
//
// Dialect: RFC 8259 over UTF-8, plus `//` line comments wherever whitespace may stand (packs are
// hand-written). No trailing commas, no `/* */`, no NaN/Infinity, no duplicate keys in one object.
// A leading UTF-8 BOM is skipped. Caps: kMaxBytes per document, nesting depth kMaxDepth. Every error
// carries the byte offset where the reader stopped. Numbers go through strtod/snprintf: the engine
// never calls setlocale, so the C locale's '.' holds (`--json-selftest` checks a fraction).
//
// WorldBrowser's lenient manifest reader is separate on purpose (it reads a third party's quirks).
#ifndef EDEN_JSON_H
#define EDEN_JSON_H

#include <stddef.h>
#include <stdint.h>
#include <string>
#include <utility>
#include <vector>

namespace Json {

constexpr size_t kMaxBytes = 1u << 20;   // 1 MiB
constexpr int kMaxDepth = 32;

enum Type { Null, Bool, Number, String, Array, Object };

struct Member;   // an object member: {first = key, second = value}

struct Value {
    Type type = Null;
    bool b = false;
    double num = 0;
    std::string str;
    std::vector<Value> arr;
    // In document order. Not std::pair<std::string, Value>: a pair needs Value complete here, which libstdc++
    // enforces (the Linux legs) and libc++ does not; std::vector itself allows an incomplete element.
    std::vector<Member> obj;
    size_t offset = 0;                                 // byte offset of the value's first character

    bool isNull() const { return type == Null; }
    bool isBool() const { return type == Bool; }
    bool isNumber() const { return type == Number; }
    bool isString() const { return type == String; }
    bool isArray() const { return type == Array; }
    bool isObject() const { return type == Object; }
    // An object member, or NULL (also when this is not an object).
    const Value* get(const char* key) const;
    // A Number that is a whole value in [lo, hi].
    bool asInt(int64_t* out, int64_t lo = INT64_MIN, int64_t hi = INT64_MAX) const;
};

struct Member {
    std::string first;
    Value second;
    Member() = default;
    Member(std::string k, Value v) : first(std::move(k)), second(std::move(v)) {}
};

struct Error {
    size_t offset = 0;
    std::string message;
};

// Parses the whole buffer (one value, then only whitespace/comments). false: *err says why and where.
bool parse(const char* data, size_t len, Value* out, Error* err);
// Canonical compact text: no whitespace, members in stored order, integers without a fraction,
// other numbers %.17g, strings escaped minimally ("\\", "\"", control bytes as \u00XX).
std::string write(const Value& v);

}  // namespace Json

#endif
