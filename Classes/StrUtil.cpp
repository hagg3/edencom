// StrUtil.cpp — G.2b-0. See StrUtil.h.
#include "StrUtil.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

std::string strf(const char* fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    char small[256];
    int n = vsnprintf(small, sizeof(small), fmt, ap);
    va_end(ap);
    if (n < 0) { va_end(ap2); return std::string(); }
    if ((size_t)n < sizeof(small)) { va_end(ap2); return std::string(small, (size_t)n); }
    std::vector<char> big((size_t)n + 1);
    vsnprintf(big.data(), big.size(), fmt, ap2);
    va_end(ap2);
    return std::string(big.data(), (size_t)n);
}

bool eden_cstr_copy(char* dst, size_t max, const std::string& src) {
    if (max == 0) return false;
    if (src.size() + 1 > max) { dst[0] = 0; return false; }
    memcpy(dst, src.c_str(), src.size() + 1);
    return true;
}

std::string path_last(const std::string& p) {
    size_t end = p.size();
    while (end > 1 && p[end - 1] == '/') --end;          // trailing slashes do not count
    size_t slash = p.rfind('/', end ? end - 1 : 0);
    if (slash == std::string::npos || end <= 1) return p.substr(0, end);
    return p.substr(slash + 1, end - slash - 1);
}

std::string path_ext(const std::string& p) {
    std::string last = path_last(p);
    size_t dot = last.rfind('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 >= last.size()) return std::string();
    return last.substr(dot + 1);
}

bool path_has_suffix(const std::string& s, const std::string& suf) {
    return !suf.empty() && s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

std::string path_strip_ext(const std::string& p) {
    std::string ext = path_ext(p);
    if (ext.empty()) return p;
    return p.substr(0, p.size() - ext.size() - 1);
}
