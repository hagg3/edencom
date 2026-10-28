// StrUtil.h — G.2b-0 (WORKING/de-objc-plan-2026-10-24.md D2/D4): the plain C++ replacements for the
// Foundation string calls the engine uses (stringWithFormat, getCString:maxLength:, pathExtension, ...).
// No ObjC, no Foundation: compiles as plain C++ on every target. Behaviours were measured on Apple's
// Foundation (plan section 6.1).
#ifndef Eden_StrUtil_h
#define Eden_StrUtil_h

#include <cstddef>
#include <string>

// printf into a std::string. The format attribute makes clang reject a leftover `%@`.
std::string strf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// Apple's getCString:maxLength: semantics: if `src` + NUL fits in `max` bytes, copy it and return true;
// otherwise write dst[0] = 0, leave every other byte alone and return false. `max` includes the NUL.
bool eden_cstr_copy(char* dst, size_t max, const std::string& src);

// NSString path helpers, as measured: path_ext("x.eden.gz") = "gz", path_ext("noext") = "".
std::string path_ext(const std::string& p);                         // pathExtension
bool path_has_suffix(const std::string& s, const std::string& suf); // hasSuffix: (an empty suffix is NO)
std::string path_strip_ext(const std::string& p);                   // stringByDeletingPathExtension
std::string path_last(const std::string& p);                        // lastPathComponent

// G.2b-2: would [NSString stringWithUTF8String:] accept these bytes? Strict UTF-8 as measured on Apple
// (TESTERS/g2b2/probes/utf8.mm): no overlongs, no surrogates, nothing above U+10FFFF, no truncated or
// stray continuation bytes; noncharacters and a BOM are fine. For call sites that relied on a nil.
bool utf8_valid(const std::string& s);

#endif
