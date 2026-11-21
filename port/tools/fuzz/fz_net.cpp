#include "NetFramer.h"
#include "Snapz.h"
using namespace eden_net;
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* d, size_t n) {
    LineFramer fr(256, 1 << 16);
    size_t i = 0; std::string line;
    while (i < n) { size_t k = std::min<size_t>(n - i, (d[i] % 17) + 1); fr.feed((const char*)d + i, k); i += k;
        while (fr.next(line)) { std::vector<SnapzRec> r; const char* e; parseSnapzLine(line, r, &e); } }
    std::string s((const char*)d, n); std::vector<SnapzRec> r; const char* e; parseSnapzLine("SNAPZ:" + s, r, &e);
    return 0;
}
