// NetFramer.h — MP.1: the `\n` line framer. Lines arrive coalesced or split arbitrarily; the
// server drops a connection past 8192 bytes without a newline, and `SNAPZ:` lines (one base64
// frame) are exempt up to 64 MB here. A `\r` is NOT stripped (protocol: part of the last field).
#pragma once
#include <cstddef>
#include <cstring>
#include <string>

namespace eden_net {

constexpr size_t kMaxLine = 8192;
constexpr size_t kMaxSnapzLine = size_t(64) << 20;

class LineFramer {
public:
    explicit LineFramer(size_t maxLine = kMaxLine, size_t maxSnapz = kMaxSnapzLine)
        : maxLine_(maxLine), maxSnapz_(maxSnapz) {}

    void feed(const char* p, size_t n) { if (!overflow_) buf_.append(p, n); }

    // Next complete line (without its `\n`). False when none is complete yet, or after overflow.
    bool next(std::string& out) {
        if (overflow_) return false;
        const size_t nl = buf_.find('\n', scan_);
        if (nl == std::string::npos) {
            if (buf_.size() - start_ > capFor() ) overflow_ = true;
            scan_ = buf_.size();
            return false;
        }
        if (nl - start_ > capFor()) { overflow_ = true; return false; }
        out.assign(buf_, start_, nl - start_);
        start_ = scan_ = nl + 1;
        if (start_ == buf_.size()) { buf_.clear(); start_ = scan_ = 0; }
        else if (start_ > (size_t(1) << 16)) { buf_.erase(0, start_); start_ = scan_ = 0; }
        return true;
    }

    bool overflowed() const { return overflow_; }
    // Bytes of the line being assembled (progress for a long SNAPZ line).
    size_t pending() const { return buf_.size() - start_; }

private:
    size_t capFor() const {
        return (buf_.size() - start_ >= 6 && !std::memcmp(buf_.data() + start_, "SNAPZ:", 6)) ? maxSnapz_ : maxLine_;
    }
    std::string buf_;
    size_t start_ = 0, scan_ = 0;
    size_t maxLine_, maxSnapz_;
    bool overflow_ = false;
};

}  // namespace eden_net
