// Snapz.h — MP.1: the `SNAPZ:<count>:<b64>` codec. Standard-alphabet base64 without padding,
// raw DEFLATE (windowBits -15), inflated length exactly count*20, each record five little-endian
// int32 (x, y, z, flag, type); flag 0 = block, 1 = air (type -1), 3 = paint. Wire spec:
// ~/ewb-server/docs/protocol.md "REGION -> SNAPZ". Plain C++ over zlib.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace eden_net {

struct SnapzRec { int32_t x, y, z, flag, type; };

constexpr size_t kSnapzRecBytes = 20;
constexpr size_t kSnapzMaxRecords = size_t(1) << 22;   // ~84 MB inflated; real frames hold <= 3000

bool base64Decode(const char* p, size_t n, std::string& out);       // padding tolerated, junk rejected
std::string base64Encode(const void* p, size_t n, bool pad = false);

// Parse one whole `SNAPZ:` line (no `\n`). On failure returns false and sets *err (static text).
bool parseSnapzLine(const std::string& line, std::vector<SnapzRec>& recs, const char** err);

// Encoder (the selftests' mock server; the real server is ewb-server). Level 0-9.
std::string encodeSnapzLine(const SnapzRec* recs, size_t n, int level = 1);

}  // namespace eden_net
