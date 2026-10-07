// NetBackend_native.h — the one blocking HTTP GET each native platform supplies (ROADMAP 5.9,
// the world browser). Plain C on purpose: the Apple backend is compiled in its own object library
// against the real SDK headers (src/seam/AppleNet_native.mm, the same wall AppleAudio_native.h
// draws, for the same reason), so nothing engine-shaped may cross this line.
//
// Three implementations, one per platform, and exactly one is linked:
//   Apple (macOS + iOS)  AppleNet_native.mm   NSURLSession — the only HTTP stack iOS offers, and
//                                             testing it on the Mac tests the code the iPad runs
//   Linux                NetCurl_native.cpp   libcurl (found by CMake; absent -> NetNone_native.cpp)
//   Windows              NetWinHttp_native.cpp WinHTTP — in the OS, no DLLs to ship in the zip
//
// The job table, the worker threads, the .part/rename discipline and the offline fixture mode all
// live ABOVE this, in Net_native.cpp, so they are written once.
#ifndef EDEN_NET_BACKEND_NATIVE_H
#define EDEN_NET_BACKEND_NATIVE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct EdenNetCallbacks {
    void* ctx;
    // The response's Content-Length, once, when the headers arrive — and only for a 2xx response.
    // Not called at all when the server does not say (chunked encoding).
    void (*on_total)(void* ctx, long long total);
    // Body bytes of a 2xx response, in order. Return 0 to abort the transfer (a write failed).
    int  (*on_data)(void* ctx, const void* data, size_t n);
    // Polled by the backend while it waits; non-zero aborts as soon as the stack allows.
    int  (*cancelled)(void* ctx);
} EdenNetCallbacks;

// 1 when this build has a working HTTP stack at all.
int eden_net_backend_available(void);

// Fetches `url` (http or https, redirects followed), streaming a 2xx body through cb->on_data.
// Returns 1 only when the status was 2xx AND the whole body was delivered. Otherwise 0, with
// *httpStatus set when a status line arrived (0 for a transport failure) and a short human
// sentence in err ("HTTP 404", "could not connect", "cancelled").
int eden_net_backend_fetch(const char* url, const EdenNetCallbacks* cb, int* httpStatus,
                           char* err, int errcap);

#ifdef __cplusplus
}
#endif

#endif
