// CodecBench_native.cpp — Stage S / S.2's `--codec-bench`: per-column zstd-1 / zstd-3 / zstd-3+checksum /
// zlib-6 encode and decode time over a sample of REAL columns from a raw (user-save) `.eden` file.
//
// No engine, no GL, no world load: it reads the header, walks the ColumnIndex directory to EOF
// (stopping at the first row that fails the coordinate/offset gate, which is also where a NewFormat256z
// sign trailer begins — docs/eden-file-format.md), takes every Nth column to reach ~N, and times each
// codec per column. Runs identically on the Mac and on the iPad (eden-args.txt), which is the point:
// the plan's A8X zstd-3 decode median is a number from THIS function on the device.
//
// Raw files only. The bundled Eden.eden is the RLE variant and has no 32 KB records to measure.
#include <zlib.h>
#include <zstd.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

struct Row { int32_t x, z; uint64_t off; };

double now_us() {
    using namespace std::chrono;
    return duration_cast<duration<double, std::micro>>(steady_clock::now().time_since_epoch()).count();
}

struct Stat {
    std::vector<double> enc, dec;
    uint64_t bytes = 0;
};

double pct(std::vector<double> v, double p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    size_t i = (size_t)(p * (v.size() - 1) + 0.5);
    return v[i];
}

void report(const char* name, const Stat& s, uint64_t raw, size_t n) {
    double e = 0, d = 0;
    for (double x : s.enc) e += x;
    for (double x : s.dec) d += x;
    std::printf("[eden-codec] %-12s %9llu B (%5.2f%% of raw, %6.1f B/col)  enc med %7.1f p95 %7.1f us  "
                "dec med %7.1f p95 %7.1f us  (sum enc %.1f ms dec %.1f ms)\n",
                name, (unsigned long long)s.bytes, 100.0 * s.bytes / raw, (double)s.bytes / n,
                pct(s.enc, 0.5), pct(s.enc, 0.95), pct(s.dec, 0.5), pct(s.dec, 0.95), e / 1000.0, d / 1000.0);
}

}  // namespace

// Returns 0 on success. `height` is the world height (64 or 256): the record is 16*16*16*(height/16)*2 bytes.
int eden_codec_bench(const char* path, int height, int want) {
    const size_t colBytes = (size_t)16 * 16 * height * 2;
    FILE* f = std::fopen(path, "rb");
    if (!f) { std::printf("[eden-codec] FAIL cannot open %s\n", path); return 1; }
    std::fseek(f, 0, SEEK_END);
    const uint64_t fsize = (uint64_t)std::ftell(f);
    uint64_t dir = 0;
    std::fseek(f, 32, SEEK_SET);
    if (std::fread(&dir, 8, 1, f) != 1 || dir < 192 || dir >= fsize) {
        std::printf("[eden-codec] FAIL bad directory_offset %llu (file %llu B) — is this a raw .eden?\n",
                    (unsigned long long)dir, (unsigned long long)fsize);
        std::fclose(f);
        return 1;
    }
    std::fseek(f, (long)dir, SEEK_SET);
    std::vector<Row> rows;
    for (;;) {
        unsigned char b[16];
        if (std::fread(b, 16, 1, f) != 1) break;
        Row r; std::memcpy(&r.x, b, 4); std::memcpy(&r.z, b + 4, 4); std::memcpy(&r.off, b + 8, 8);
        if (r.off < 192 || r.off + colBytes > dir || (r.off & 7) || r.x < 0 || r.x > 65536 || r.z < 0 || r.z > 65536) break;
        rows.push_back(r);
    }
    if (rows.empty()) { std::printf("[eden-codec] FAIL no directory rows\n"); std::fclose(f); return 1; }
    const size_t stride = std::max<size_t>(1, rows.size() / (size_t)std::max(1, want));
    std::vector<Row> pick;
    for (size_t i = 0; i < rows.size(); i += stride) pick.push_back(rows[i]);
    std::printf("[eden-codec] %s: %zu directory rows, sampling %zu (stride %zu), %zu B/column raw\n",
                path, rows.size(), pick.size(), stride, colBytes);

    std::vector<unsigned char> in(colBytes), back(colBytes);
    const size_t zcap = ZSTD_compressBound(colBytes) > compressBound((uLong)colBytes) ? ZSTD_compressBound(colBytes) : compressBound((uLong)colBytes);
    std::vector<unsigned char> out(zcap);
    ZSTD_CCtx* cc = ZSTD_createCCtx();
    ZSTD_DCtx* dc = ZSTD_createDCtx();
    Stat z1, z3, z3c, zl;
    int bad = 0;
    for (const Row& r : pick) {
        std::fseek(f, (long)r.off, SEEK_SET);
        if (std::fread(in.data(), 1, colBytes, f) != colBytes) { bad++; continue; }
        // One warm-up of each so the first column's allocation cost is not charged to the median.
        for (int rep = 0; rep < 1; rep++) {
            double t0 = now_us();
            size_t n = ZSTD_compressCCtx(cc, out.data(), out.size(), in.data(), colBytes, 1);
            double t1 = now_us();
            size_t m = ZSTD_decompressDCtx(dc, back.data(), colBytes, out.data(), n);
            double t2 = now_us();
            if (ZSTD_isError(n) || m != colBytes || std::memcmp(in.data(), back.data(), colBytes)) bad++;
            z1.enc.push_back(t1 - t0); z1.dec.push_back(t2 - t1); z1.bytes += n;

            t0 = now_us();
            n = ZSTD_compressCCtx(cc, out.data(), out.size(), in.data(), colBytes, 3);
            t1 = now_us();
            m = ZSTD_decompressDCtx(dc, back.data(), colBytes, out.data(), n);
            t2 = now_us();
            if (ZSTD_isError(n) || m != colBytes || std::memcmp(in.data(), back.data(), colBytes)) bad++;
            z3.enc.push_back(t1 - t0); z3.dec.push_back(t2 - t1); z3.bytes += n;

            ZSTD_CCtx_reset(cc, ZSTD_reset_session_and_parameters);
            ZSTD_CCtx_setParameter(cc, ZSTD_c_compressionLevel, 3);
            ZSTD_CCtx_setParameter(cc, ZSTD_c_checksumFlag, 1);
            t0 = now_us();
            n = ZSTD_compress2(cc, out.data(), out.size(), in.data(), colBytes);
            t1 = now_us();
            m = ZSTD_decompressDCtx(dc, back.data(), colBytes, out.data(), n);
            t2 = now_us();
            if (ZSTD_isError(n) || m != colBytes || std::memcmp(in.data(), back.data(), colBytes)) bad++;
            z3c.enc.push_back(t1 - t0); z3c.dec.push_back(t2 - t1); z3c.bytes += n;
            ZSTD_CCtx_reset(cc, ZSTD_reset_session_and_parameters);

            uLongf zn = (uLongf)out.size();
            t0 = now_us();
            int rc = compress2(out.data(), &zn, in.data(), (uLong)colBytes, 6);
            t1 = now_us();
            uLongf zm = (uLongf)colBytes;
            int rc2 = uncompress(back.data(), &zm, out.data(), zn);
            t2 = now_us();
            if (rc != Z_OK || rc2 != Z_OK || zm != colBytes || std::memcmp(in.data(), back.data(), colBytes)) bad++;
            zl.enc.push_back(t1 - t0); zl.dec.push_back(t2 - t1); zl.bytes += zn;
        }
    }
    std::fclose(f);
    ZSTD_freeCCtx(cc); ZSTD_freeDCtx(dc);
    const size_t n = pick.size();
    const uint64_t raw = (uint64_t)n * colBytes;
    std::printf("[eden-codec] zstd %s, zlib %s\n", ZSTD_versionString(), zlibVersion());
    report("zstd-1", z1, raw, n);
    report("zstd-3", z3, raw, n);
    report("zstd-3+ck", z3c, raw, n);
    report("zlib-6", zl, raw, n);
    std::printf("[eden-codec] %s (%d round-trip failure(s))\n", bad ? "FAIL" : "PASS", bad);
    return bad ? 1 : 0;
}
