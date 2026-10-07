// NetCurl_native.cpp — NetBackend_native.h on Linux, over libcurl (ROADMAP 5.9).
//
// Linked only when CMake finds libcurl (native/CMakeLists.txt); without it NetNone_native.cpp is
// linked instead and the browser's "Get Worlds" button simply is not offered. libcurl.so.4 is on
// every desktop distribution, and CI installs libcurl4-openssl-dev to build against it.
//
// BUILT AND RUN ONLY IN GITHUB ACTIONS — there is no Linux box. Keep it boring.
#include "NetBackend_native.h"

#include <curl/curl.h>

#include <cstdio>
#include <mutex>

namespace {

struct Ctx {
    const EdenNetCallbacks* cb;
    CURL* h;
    bool started, badStatus, writeFailed;
    long status;
};

size_t on_write(char* p, size_t size, size_t n, void* user) {
    Ctx* c = static_cast<Ctx*>(user);
    const size_t bytes = size * n;
    if (!c->started) {
        // The first body byte is the first moment the status and Content-Length are both known.
        c->started = true;
        curl_easy_getinfo(c->h, CURLINFO_RESPONSE_CODE, &c->status);
        if (c->status < 200 || c->status >= 300) { c->badStatus = true; return 0; }
        curl_off_t len = -1;
        if (curl_easy_getinfo(c->h, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &len) == CURLE_OK && len >= 0 &&
            c->cb->on_total)
            c->cb->on_total(c->cb->ctx, (long long)len);
    }
    if (!c->cb->on_data(c->cb->ctx, p, bytes)) { c->writeFailed = true; return 0; }
    return bytes;
}

int on_progress(void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    Ctx* c = static_cast<Ctx*>(user);
    return (c->cb->cancelled && c->cb->cancelled(c->cb->ctx)) ? 1 : 0;
}

std::once_flag g_init;

}  // namespace

extern "C" int eden_net_backend_available(void) { return 1; }

extern "C" int eden_net_backend_fetch(const char* url, const EdenNetCallbacks* cb, int* httpStatus,
                                      char* err, int errcap) {
    std::call_once(g_init, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
    *httpStatus = 0;
    CURL* h = curl_easy_init();
    if (!h) { std::snprintf(err, errcap, "could not start a transfer"); return 0; }
    Ctx c = { cb, h, false, false, false, 0 };
    curl_easy_setopt(h, CURLOPT_URL, url);
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(h, CURLOPT_USERAGENT, "Emod/2.1.1");
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(h, CURLOPT_LOW_SPEED_LIMIT, 1L);       // 30 s of silence is a dead link
    curl_easy_setopt(h, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);              // worker threads: no SIGALRM
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, on_write);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &c);
    curl_easy_setopt(h, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(h, CURLOPT_XFERINFOFUNCTION, on_progress);
    curl_easy_setopt(h, CURLOPT_XFERINFODATA, &c);
    const CURLcode rc = curl_easy_perform(h);
    long status = c.status;
    if (!c.started) curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);   // an empty body
    curl_easy_cleanup(h);
    *httpStatus = (int)status;
    if (c.badStatus || (rc == CURLE_OK && (status < 200 || status >= 300))) {
        std::snprintf(err, errcap, "HTTP %ld", status);
        return 0;
    }
    if (c.writeFailed) { std::snprintf(err, errcap, "could not write the download"); return 0; }
    if (rc == CURLE_ABORTED_BY_CALLBACK) { std::snprintf(err, errcap, "cancelled"); return 0; }
    if (rc != CURLE_OK) { std::snprintf(err, errcap, "%s", curl_easy_strerror(rc)); return 0; }
    return 1;
}
