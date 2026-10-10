// WorldTools_native.cpp — Stage S / S.5, S.5b, S.5c: the world-file verbs, as command-line modes of
// eden_native (no engine, no window, no save directory). They drive the SAME production code the menu
// does — emod::EdenExporter / export_world_file, emod::ImportJob, emod::build_upload_body and the
// eden_net_* seam — so the gates in WORKING/s5-results-2026-10-10.md test what ships.
//
//   --export=SRC --export-out=OUT [--export-target=own|Legacy64z|NewDawn256z|NewFormat256z]
//       [--export-signs=keep|prune] [--export-gz] [--export-dry]
//       prints `export-report {json}`, then `export ok <bytes>` or `export FAILED: <why>`.
//   --import=SRC --import-out=W.emod [--import-kill-after=BYTES] [--import-256z]
//       (--import-256z: S.5e's upgrade, a 64z source lands 256z; without it the height is the source's
//       -- a harness pins it rather than reading the Settings toggle)
//       prints `import ok need=… peak=… out=…` or `import FAILED: <why>`; kill-after _exit(137)s once
//       that many source bytes were consumed (gate (3)/(8): a process killed mid-convert).
//   --upload-src=SRC --upload-png=PNG --upload-body=FILE [--upload-server=current|legacy]
//       [--export-target=…] [--export-signs=…] [--upload-url=URL] [--upload-uuid=UUID]
//       builds the multipart body; with --upload-url (or --upload-post, which uses the real host for
//       the server) it POSTs it through eden_net_post_file and prints the reply. Fixture mode
//       (--net-fixtures / EDEN_NET_FIXTURES) answers it offline.
#include "../../../Classes/EdenWorldExport.h"
#include "../../../Classes/EdenWorldSource.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#if defined(_WIN32)
#  include <io.h>
#  define eden_exit _exit
#else
#  include <unistd.h>
#  define eden_exit _exit
#endif

extern "C" {
int eden_net_post_file(const char* url, const char* contentType, const char* bodyPath);
int eden_net_poll(int job, long long* got, long long* total);
const char* eden_net_error(int job);
const unsigned char* eden_net_body(int job, int* len);
void eden_net_release(int job);
void eden_net_set_fixture_root(const char* dir);
}

namespace {

bool starts(const char* a, const char* p) { return std::strncmp(a, p, std::strlen(p)) == 0; }

int run_export(const std::string& src, const std::string& out, const emod::ExportOptions& opt, bool gz, bool dry) {
    if (dry) {
        emod::EdenExporter ex;
        if (!ex.begin(src, opt)) { std::printf("export FAILED: %s\n", ex.error().c_str()); return 1; }
        std::printf("export-report %s\n", ex.report().json().c_str());
        std::printf("export-summary %s\n", ex.report().summary().c_str());
        return 0;
    }
    emod::ExportReport rep;
    std::string err;
    auto t0 = std::chrono::steady_clock::now();
    bool ok = emod::export_world_file(src, out, opt, gz, &rep, &err);
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (rep.bytes) std::printf("export-report %s\n", rep.json().c_str());
    if (!ok) { std::printf("export FAILED: %s\n", err.c_str()); return 1; }
    FILE* f = std::fopen(out.c_str(), "rb");
    unsigned long long size = 0;
    if (f) { size = emod::io::file_size(f); std::fclose(f); }
    std::printf("export ok %llu B (pre-flight raw %llu B) in %.0f ms\n", size, (unsigned long long)rep.bytes, ms);
    return 0;
}

int run_import(const std::string& src, const std::string& out, unsigned long long killAfter, bool up256) {
    emod::ImportJob job;
    std::string name = src.substr(src.find_last_of("/\\") == std::string::npos ? 0 : src.find_last_of("/\\") + 1);
    auto t0 = std::chrono::steady_clock::now();
    if (!job.begin(src, out, name, 0, up256)) {
        std::printf("import FAILED: %s%s\n", job.error().c_str(), job.lowSpace() ? " [space]" : "");
        return 1;
    }
    std::printf("import need %llu B\n", (unsigned long long)job.need());
    for (;;) {
        int r = job.step(12);
        if (killAfter && job.stats().bytesIn >= killAfter) {
            std::printf("import killed after %llu B in\n", (unsigned long long)job.stats().bytesIn);
            std::fflush(stdout);
            eden_exit(137);
        }
        if (r == emod::ImportJob::FAILED) {
            std::printf("import FAILED: %s%s\n", job.error().c_str(), job.lowSpace() ? " [space]" : "");
            return 1;
        }
        if (r == emod::ImportJob::DONE) break;
    }
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    const emod::EdenEmodConverter::Stats& s = job.stats();
    std::printf("import ok need=%llu peak=%llu out=%llu spill=%llu in=%llu columns=%llu bands=%d->%d in %.0f ms\n",
                (unsigned long long)job.need(), (unsigned long long)job.peakTempBytes(), (unsigned long long)s.outBytes,
                (unsigned long long)s.spillBytes, (unsigned long long)s.bytesIn, (unsigned long long)s.columns, s.bandsIn,
                s.bandsOut, ms);
    return 0;
}

int run_upload(int server, const std::string& src, const std::string& png, const std::string& body,
               const emod::ExportOptions& opt, const std::string& url) {
    emod::ExportReport rep;
    std::string err;
    if (!emod::build_upload_body(server, src, png, opt, body, &rep, &err)) { std::printf("upload FAILED: %s\n", err.c_str()); return 1; }
    std::printf("export-report %s\n", rep.json().c_str());
    std::printf("upload-body %s\n", body.c_str());
    if (url.empty()) return 0;
    int job = eden_net_post_file(url.c_str(), emod::upload_content_type().c_str(), body.c_str());
    if (!job) { std::printf("upload FAILED: no network\n"); return 1; }
    int st;
    long long got = 0, total = 0;
    while ((st = eden_net_poll(job, &got, &total)) == 0) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    if (st < 0) { std::printf("upload FAILED: %s\n", eden_net_error(job)); eden_net_release(job); return 1; }
    int len = 0;
    const unsigned char* b = eden_net_body(job, &len);
    std::string reply(b ? (const char*)b : "", (size_t)len);
    eden_net_release(job);
    std::printf("upload reply '%s'\n", reply.c_str());
    return reply == "YES" ? 0 : 1;
}

}  // namespace

int eden_world_tool_main(int argc, char** argv) {
    std::string exportSrc, exportOut, importSrc, importOut, upSrc, upPng, upBody, upUrl, upUuid = "00000000-0000-4000-8000-000000000000";
    emod::ExportOptions opt;
    bool gz = false, dry = false, post = false, up256 = false;
    int server = emod::UPLOAD_CURRENT;
    unsigned long long killAfter = 0;
    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        if (starts(a, "--export-out=")) exportOut = a + 13;
        else if (starts(a, "--export-target=")) {
            opt.target = emod::export_target_from_name(a + 16);
            if (opt.target < 0) { std::fprintf(stderr, "unknown --export-target '%s'\n", a + 16); return 2; }
        }
        else if (starts(a, "--export-signs=")) opt.signs = !std::strcmp(a + 15, "prune") ? emod::SIGNS_PRUNE : emod::SIGNS_KEEP;
        else if (!std::strcmp(a, "--export-gz")) gz = true;
        else if (!std::strcmp(a, "--export-dry")) dry = true;
        else if (starts(a, "--export=")) exportSrc = a + 9;
        else if (starts(a, "--import-out=")) importOut = a + 13;
        else if (starts(a, "--import-kill-after=")) killAfter = std::strtoull(a + 20, nullptr, 10);
        else if (!std::strcmp(a, "--import-256z")) up256 = true;
        else if (starts(a, "--import=")) importSrc = a + 9;
        else if (starts(a, "--upload-src=")) upSrc = a + 13;
        else if (starts(a, "--upload-png=")) upPng = a + 13;
        else if (starts(a, "--upload-body=")) upBody = a + 14;
        else if (starts(a, "--upload-url=")) upUrl = a + 13;
        else if (starts(a, "--upload-uuid=")) upUuid = a + 14;
        else if (!std::strcmp(a, "--upload-post")) post = true;
        else if (starts(a, "--upload-server=")) server = !std::strcmp(a + 16, "legacy") ? emod::UPLOAD_LEGACY : emod::UPLOAD_CURRENT;
        else if (starts(a, "--net-fixtures=")) eden_net_set_fixture_root(a + 15);
    }
    if (!exportSrc.empty()) return run_export(exportSrc, exportOut, opt, gz, dry);
    if (!importSrc.empty()) return run_import(importSrc, importOut, killAfter, up256);
    if (!upSrc.empty()) {
        if (post && upUrl.empty()) upUrl = emod::upload_url(server, upUuid);
        return run_upload(server, upSrc, upPng, upBody, opt, upUrl);
    }
    std::fprintf(stderr, "world tool: nothing to do (see WorldTools_native.cpp)\n");
    return 2;
}
