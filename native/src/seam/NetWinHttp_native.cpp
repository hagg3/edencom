// NetWinHttp_native.cpp — NetBackend_native.h on Windows, over WinHTTP (ROADMAP 5.9).
//
// WinHTTP rather than libcurl because it ships in the OS: libcurl from MSYS2 would add a dozen
// DLLs (OpenSSL, nghttp2, zstd, brotli, idn2 ...) to the release zip. It does HTTP and HTTPS with
// the system's certificate store and follows redirects for a GET by default.
//
// BUILT AND RUN ONLY IN GITHUB ACTIONS — there is no Windows box. Keep it boring.
#include "NetBackend_native.h"

#include <windows.h>
#include <winhttp.h>

#include <cstdio>
#include <cstdlib>       // _wtoi64
#include <string>
#include <vector>

namespace {

std::wstring widen(const char* s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], n);
    w.resize((size_t)n - 1);
    return w;
}

struct Handles {
    HINTERNET session = nullptr, connect = nullptr, request = nullptr;
    ~Handles() {
        if (request) WinHttpCloseHandle(request);
        if (connect) WinHttpCloseHandle(connect);
        if (session) WinHttpCloseHandle(session);
    }
};

}  // namespace

// GET when contentType is null; otherwise POST the file at bodyPath, its length declared up front
// (WinHttpSendRequest's dwTotalLength) and its bytes written with WinHttpWriteData.
static int run(const char* url, const char* contentType, const char* bodyPath, const EdenNetCallbacks* cb,
               int* httpStatus, char* err, int errcap) {
    *httpStatus = 0;
    const std::wstring wurl = widen(url);
    URL_COMPONENTS uc;
    ZeroMemory(&uc, sizeof(uc));
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256], path[2048], extra[2048];
    uc.lpszHostName = host;  uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;   uc.dwUrlPathLength = 2048;
    uc.lpszExtraInfo = extra; uc.dwExtraInfoLength = 2048;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) { std::snprintf(err, errcap, "bad address"); return 0; }
    const std::wstring object = std::wstring(path, uc.dwUrlPathLength) + std::wstring(extra, uc.dwExtraInfoLength);
    const bool secure = uc.nScheme == INTERNET_SCHEME_HTTPS;

    Handles h;
#ifdef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
    h.session = WinHttpOpen(L"Emod/2.1.1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                            WINHTTP_NO_PROXY_BYPASS, 0);
#endif
    if (!h.session)
        h.session = WinHttpOpen(L"Emod/2.1.1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                WINHTTP_NO_PROXY_BYPASS, 0);
    if (!h.session) { std::snprintf(err, errcap, "could not start a transfer"); return 0; }
    WinHttpSetTimeouts(h.session, 15000, 15000, 30000, 30000);   // resolve, connect, send, receive
    h.connect = WinHttpConnect(h.session, std::wstring(host, uc.dwHostNameLength).c_str(), uc.nPort, 0);
    if (!h.connect) { std::snprintf(err, errcap, "could not connect"); return 0; }
    h.request = WinHttpOpenRequest(h.connect, contentType ? L"POST" : L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER,
                                   WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0);
    if (!h.request) { std::snprintf(err, errcap, "could not connect"); return 0; }
    if (contentType) {
        const std::wstring wpath = widen(bodyPath);
        FILE* body = _wfopen(wpath.c_str(), L"rb");
        if (!body) { std::snprintf(err, errcap, "could not read the upload"); return 0; }
        _fseeki64(body, 0, SEEK_END);
        const long long len = _ftelli64(body);
        _fseeki64(body, 0, SEEK_SET);
        if (len < 0 || len > 0xFFFFFFFFLL) { std::fclose(body); std::snprintf(err, errcap, "the upload is too large"); return 0; }
        const std::wstring hdr = L"Content-Type: " + widen(contentType);
        bool ok = WinHttpSendRequest(h.request, hdr.c_str(), (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, (DWORD)len, 0) != 0;
        std::vector<char> chunk(65536);
        while (ok) {
            if (cb->cancelled && cb->cancelled(cb->ctx)) { std::fclose(body); std::snprintf(err, errcap, "cancelled"); return 0; }
            const size_t n = std::fread(chunk.data(), 1, chunk.size(), body);
            if (n == 0) break;
            DWORD wrote = 0;
            ok = WinHttpWriteData(h.request, chunk.data(), (DWORD)n, &wrote) != 0;
        }
        std::fclose(body);
        if (!ok || !WinHttpReceiveResponse(h.request, nullptr)) {
            std::snprintf(err, errcap, "could not send (error %lu)", (unsigned long)GetLastError());
            return 0;
        }
    } else if (!WinHttpSendRequest(h.request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(h.request, nullptr)) {
        std::snprintf(err, errcap, "could not connect (error %lu)", (unsigned long)GetLastError());
        return 0;
    }

    DWORD status = 0, sz = sizeof(status);
    WinHttpQueryHeaders(h.request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
    *httpStatus = (int)status;
    if (status < 200 || status >= 300) { std::snprintf(err, errcap, "HTTP %lu", (unsigned long)status); return 0; }
    // Content-Length as text: FLAG_NUMBER is a 32-bit DWORD, and a world can be over 4 GB.
    wchar_t lenText[32];
    DWORD lenSz = sizeof(lenText);
    if (cb->on_total && WinHttpQueryHeaders(h.request, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX,
                                            lenText, &lenSz, WINHTTP_NO_HEADER_INDEX))
        cb->on_total(cb->ctx, (long long)_wtoi64(lenText));

    std::vector<char> buf(65536);
    for (;;) {
        if (cb->cancelled && cb->cancelled(cb->ctx)) { std::snprintf(err, errcap, "cancelled"); return 0; }
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(h.request, &avail)) {
            std::snprintf(err, errcap, "the connection dropped (error %lu)", (unsigned long)GetLastError());
            return 0;
        }
        if (avail == 0) break;
        DWORD want = avail < buf.size() ? avail : (DWORD)buf.size(), got = 0;
        if (!WinHttpReadData(h.request, buf.data(), want, &got)) {
            std::snprintf(err, errcap, "the connection dropped (error %lu)", (unsigned long)GetLastError());
            return 0;
        }
        if (got == 0) break;
        if (!cb->on_data(cb->ctx, buf.data(), got)) { std::snprintf(err, errcap, "could not write the download"); return 0; }
    }
    return 1;
}

extern "C" int eden_net_backend_available(void) { return 1; }

extern "C" int eden_net_backend_fetch(const char* url, const EdenNetCallbacks* cb, int* httpStatus,
                                      char* err, int errcap) {
    return run(url, nullptr, nullptr, cb, httpStatus, err, errcap);
}

extern "C" int eden_net_backend_post_file(const char* url, const char* contentType, const char* bodyPath,
                                          const EdenNetCallbacks* cb, int* httpStatus, char* err, int errcap) {
    return run(url, contentType, bodyPath, cb, httpStatus, err, errcap);
}
