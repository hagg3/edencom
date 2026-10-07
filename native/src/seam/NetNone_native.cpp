// NetNone_native.cpp — NetBackend_native.h when the build found no HTTP stack (a Linux box without
// libcurl's headers). The browser asks eden_net_available() and does not offer "Get Worlds"; the
// offline fixture mode in Net_native.cpp still works, so the selftest runs regardless.
#include "NetBackend_native.h"

#include <cstdio>

extern "C" int eden_net_backend_available(void) { return 0; }

extern "C" int eden_net_backend_fetch(const char* url, const EdenNetCallbacks* cb, int* httpStatus,
                                      char* err, int errcap) {
    (void)url; (void)cb;
    *httpStatus = 0;
    std::snprintf(err, errcap, "this build has no network support");
    return 0;
}
