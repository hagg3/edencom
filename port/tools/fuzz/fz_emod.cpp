// Fuzz target: the .emod reader (open + every column + every record + verify). Built by port/tools/fuzz/build.sh.
#include "EdenWorldStore.h"
#include <cstdio>
#include <string>
#include <unistd.h>
using namespace emod;
static std::string path() { static std::string p = std::string(getenv("FZ_WORK") ? getenv("FZ_WORK") : ".") + "/emod-" + std::to_string(getpid()) + ".emod"; return p; }
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* d, size_t n) {
    FILE* f = fopen(path().c_str(), "wb"); fwrite(d, 1, n, f); fclose(f);
    for (int ho = 0; ho < 2; ho++) {
        EdenWorldStore st;
        if (!st.open(path().c_str(), false, true, ho == 1)) continue;
        if (ho) continue;
        std::vector<uint8_t> col((size_t)st.bands() * BAND);
        for (auto& k : st.columnKeys()) st.readColumn(k.first, k.second, col.data());
        std::vector<uint8_t> rec;
        for (int t = 1; t < 256; t++) if (st.hasRecord(t)) {
            st.readRecord(t, rec);
            if (t == T_BLOCK_PALETTE) palette_parse(rec.data(), rec.size(), nullptr, nullptr);
        }
        st.verify();
        st.stateDigest();
    }
    return 0;
}
