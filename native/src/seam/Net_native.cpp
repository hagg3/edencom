// Net_native.cpp — the eden_net_* seam on native (ROADMAP 5.9, the world browser).
//
// The engine asks for a URL and polls; nothing here blocks the frame. Each fetch is a JOB with its
// own worker thread running the platform's blocking GET (NetBackend_native.h). The contract the
// engine sees is declared in Classes/WorldBrowser.h:
//
//   eden_net_fetch(url, destPath)  -> job id (0 = could not start)
//   eden_net_poll(job, &got, &total) -> 0 running, 1 done, -1 failed (total -1 = unknown)
//   eden_net_body(job, &len)       -> the bytes, when destPath was NULL
//   eden_net_error(job)            -> a short sentence, when it failed
//   eden_net_release(job)          -> cancels if still running, forgets the job
//
// TWO RULES THAT ARE EASY TO BREAK:
//
//  * A file download is written to `<dest>.part` and renamed onto `dest` only after the backend
//    reported the WHOLE 2xx body. So a dest that exists is always a complete response — a killed
//    app, a dropped connection or a 404 leaves at most a .part behind, never a truncated world
//    that the menu would list.
//
//  * release() never joins. A worker blocked in a connect() can take the backend's whole connect
//    timeout to notice the cancel flag, and the main thread must not wait for that; so the job is
//    reference-counted (shared_ptr), the worker keeps its own reference, and release() detaches.
//
// OFFLINE FIXTURES. `eden_net_set_fixture_root(dir)` (the harness's --net-fixtures=DIR, or the
// EDEN_NET_FIXTURES environment variable) answers every URL from a file instead of the network:
// `scheme://host/path?query` -> `DIR/host/path@query`. A missing file is an HTTP 404, exactly as
// the real servers answer a missing preview. That is what lets --ui-selftest drive the browser end
// to end in CI, where the runners must not depend on edengame.net being up.
#include "NetBackend_native.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../../../web/src/shim/foundation/platform_shims.h"   // EDEN_EXPORT

namespace {

struct Job {
    std::string url, dest, part;
    std::string contentType, bodyPath;    // a POST (S.5c upload) when contentType is set
    std::atomic<int> state{0};            // 0 running, 1 done, -1 failed
    std::atomic<long long> got{0}, total{-1};
    std::atomic<int> cancel{0};
    std::string err;                      // written by the worker before state leaves 0
    std::vector<unsigned char> body;      // likewise
    FILE* f = nullptr;
};

std::mutex g_mu;
std::map<int, std::shared_ptr<Job>> g_jobs;
int g_next = 1;
std::string g_fixtureRoot;
bool g_fixtureInit = false;

const std::string& fixture_root() {
    if (!g_fixtureInit) {
        g_fixtureInit = true;
        if (const char* e = std::getenv("EDEN_NET_FIXTURES")) if (*e) g_fixtureRoot = e;
    }
    return g_fixtureRoot;
}

void on_total(void* ctx, long long t) { static_cast<Job*>(ctx)->total.store(t); }

int on_data(void* ctx, const void* data, size_t n) {
    Job* j = static_cast<Job*>(ctx);
    if (j->f) {
        if (std::fwrite(data, 1, n, j->f) != n) return 0;
    } else {
        const unsigned char* p = static_cast<const unsigned char*>(data);
        j->body.insert(j->body.end(), p, p + n);
    }
    j->got.fetch_add((long long)n);
    return 1;
}

int cancelled(void* ctx) { return static_cast<Job*>(ctx)->cancel.load(); }

// `scheme://host/path?query` -> `root/host/path@query`. '?' is the one URL character no Windows
// filename can carry; '&' and '=' are fine everywhere.
std::string fixture_path(const std::string& root, const std::string& url) {
    std::string rest = url;
    const size_t s = rest.find("://");
    if (s != std::string::npos) rest = rest.substr(s + 3);
    for (char& c : rest) if (c == '?') c = '@';
    return root + "/" + rest;
}

bool fetch_fixture(const std::string& root, Job* j, int* status, char* err, int errcap) {
    FILE* in = std::fopen(fixture_path(root, j->url).c_str(), "rb");
    if (!in) { *status = 404; std::snprintf(err, errcap, "HTTP 404"); return false; }
    std::fseek(in, 0, SEEK_END);
    on_total(j, (long long)std::ftell(in));
    std::fseek(in, 0, SEEK_SET);
    unsigned char buf[65536];
    size_t n;
    bool ok = true;
    while ((n = std::fread(buf, 1, sizeof(buf), in)) > 0) {
        if (j->cancel.load()) { std::snprintf(err, errcap, "cancelled"); ok = false; break; }
        if (!on_data(j, buf, n)) { std::snprintf(err, errcap, "could not write the download"); ok = false; break; }
    }
    std::fclose(in);
    *status = ok ? 200 : 0;
    return ok;
}

// A POST in fixture mode: the body is copied to `root/host/path.posted` (the query dropped, so a
// harness finds it whatever uuid was sent) and the reply is `root/host/path` -- `YES` for upload2.php.
bool post_fixture(const std::string& root, Job* j, int* status, char* err, int errcap) {
    std::string resp = fixture_path(root, j->url);
    const size_t q = resp.find('@', root.size());
    if (q != std::string::npos) resp.resize(q);
    FILE* in = std::fopen(j->bodyPath.c_str(), "rb");
    FILE* out = std::fopen((resp + ".posted").c_str(), "wb");
    bool ok = in && out;
    unsigned char buf[65536];
    size_t n;
    while (ok && (n = std::fread(buf, 1, sizeof(buf), in)) > 0) ok = std::fwrite(buf, 1, n, out) == n;
    if (in) std::fclose(in);
    if (out && std::fclose(out) != 0) ok = false;
    if (!ok) { *status = 0; std::snprintf(err, errcap, "could not read the upload"); return false; }
    const std::string saved = j->url;
    j->url = resp.substr(root.size() + 1);           // fetch_fixture maps it straight back to `resp`
    const bool r = fetch_fixture(root, j, status, err, errcap);
    j->url = saved;
    return r;
}

void run(std::shared_ptr<Job> j, std::string root) {
    char err[200] = {0};
    int status = 0;
    if (!j->dest.empty()) {
        j->f = std::fopen(j->part.c_str(), "wb");
        if (!j->f) {
            j->err = "could not create " + j->part;
            j->state.store(-1);
            return;
        }
    }
    bool ok;
    if (!root.empty()) {
        ok = j->contentType.empty() ? fetch_fixture(root, j.get(), &status, err, sizeof(err))
                                    : post_fixture(root, j.get(), &status, err, sizeof(err));
    } else {
        EdenNetCallbacks cb;
        cb.ctx = j.get();
        cb.on_total = on_total;
        cb.on_data = on_data;
        cb.cancelled = cancelled;
        ok = j->contentType.empty()
            ? eden_net_backend_fetch(j->url.c_str(), &cb, &status, err, sizeof(err)) != 0
            : eden_net_backend_post_file(j->url.c_str(), j->contentType.c_str(), j->bodyPath.c_str(), &cb, &status,
                                         err, sizeof(err)) != 0;
    }
    if (j->f) {
        if (std::fclose(j->f) != 0 && ok) { ok = false; std::snprintf(err, sizeof(err), "could not write the download"); }
        j->f = nullptr;
        if (ok) {
            std::remove(j->dest.c_str());            // Windows' rename() will not replace
            if (std::rename(j->part.c_str(), j->dest.c_str()) != 0) {
                ok = false;
                std::snprintf(err, sizeof(err), "could not move the download into place");
            }
        }
        if (!ok) std::remove(j->part.c_str());
    }
    if (!ok) j->err = err[0] ? err : "the download failed";
    j->state.store(ok ? 1 : -1);
}

std::shared_ptr<Job> find(int id) {
    std::lock_guard<std::mutex> lock(g_mu);
    auto it = g_jobs.find(id);
    return it == g_jobs.end() ? nullptr : it->second;
}

}  // namespace

extern "C" {

EDEN_EXPORT void eden_net_set_fixture_root(const char* dir) {
    g_fixtureInit = true;
    g_fixtureRoot = dir ? dir : "";
}

EDEN_EXPORT int eden_net_available(void) {
    return (!fixture_root().empty() || eden_net_backend_available()) ? 1 : 0;
}

EDEN_EXPORT int eden_net_fetch(const char* url, const char* destPath) {
    if (!url || !*url || !eden_net_available()) return 0;
    auto j = std::make_shared<Job>();
    j->url = url;
    if (destPath && *destPath) { j->dest = destPath; j->part = j->dest + ".part"; }
    int id;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        id = g_next++;
        g_jobs[id] = j;
    }
    std::thread(run, j, fixture_root()).detach();
    return id;
}

// Stage S / S.5c: POST the file at `bodyPath` (the upload body WorldShare builds) and keep the reply
// in memory (eden_net_body). Same job contract as eden_net_fetch.
EDEN_EXPORT int eden_net_post_file(const char* url, const char* contentType, const char* bodyPath) {
    if (!url || !*url || !contentType || !bodyPath || !eden_net_available()) return 0;
    auto j = std::make_shared<Job>();
    j->url = url;
    j->contentType = contentType;
    j->bodyPath = bodyPath;
    int id;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        id = g_next++;
        g_jobs[id] = j;
    }
    std::thread(run, j, fixture_root()).detach();
    return id;
}

EDEN_EXPORT int eden_net_poll(int job, long long* got, long long* total) {
    auto j = find(job);
    if (!j) return -1;
    if (got) *got = j->got.load();
    if (total) *total = j->total.load();
    return j->state.load();
}

EDEN_EXPORT const char* eden_net_error(int job) {
    auto j = find(job);
    if (!j || j->state.load() != -1) return "";
    return j->err.c_str();       // the job outlives this call: the table holds it until release
}

EDEN_EXPORT const unsigned char* eden_net_body(int job, int* len) {
    auto j = find(job);
    if (!j || j->state.load() != 1) { if (len) *len = 0; return nullptr; }
    if (len) *len = (int)j->body.size();
    return j->body.empty() ? nullptr : j->body.data();
}

EDEN_EXPORT void eden_net_release(int job) {
    std::shared_ptr<Job> j;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        auto it = g_jobs.find(job);
        if (it == g_jobs.end()) return;
        j = it->second;
        g_jobs.erase(it);
    }
    j->cancel.store(1);          // the worker (if any) holds its own reference and cleans up .part
}

}  // extern "C"
