// .eden / .gz / .zip -> .emod: WorldSource then the streaming converter, then reopen the result.
#include "EdenWorldSource.h"
#include "EdenWorldStore.h"
#include <cstdio>
#include <unistd.h>
using namespace emod;
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* d, size_t n) {
    std::string w = std::string(getenv("FZ_WORK") ? getenv("FZ_WORK") : ".") + "/eden-" + std::to_string(getpid());
    std::string in = w + ".src", out = w + ".emod";
    FILE* f = fopen(in.c_str(), "wb"); fwrite(d, 1, n, f); fclose(f);
    WorldSource src;
    if (!src.open(in)) return 0;
    EdenEmodConverter c; EdenEmodConverter::Options o; o.maxTempBytes = 64u << 20; o.upgradeTo256z = n & 1;
    if (!c.begin(out.c_str(), o)) return 0;
    static uint8_t buf[1 << 16]; size_t got, total = 0;
    while ((got = src.read(buf, sizeof buf)) > 0) { total += got; if (total > (256u << 20) || !c.feed(buf, got)) break; }
    if (!src.failed() && c.finish()) { EdenWorldStore st; if (st.open(out.c_str(), false)) st.verify(); }
    else c.abort();
    remove(out.c_str());
    return 0;
}
namespace emod { uint64_t free_bytes(const std::string&) { return 1ull << 40; } }   // stub: ImportJob is not fuzzed
