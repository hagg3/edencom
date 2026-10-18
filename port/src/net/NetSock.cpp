// NetSock.cpp — see NetSock.h.
#include "NetSock.h"

#include <chrono>
#include <cstring>
#include <mutex>
#include <string>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define EN_INVALID INVALID_SOCKET
#define EN_CLOSE closesocket
#define EN_POLL WSAPoll
#define EN_ERRNO WSAGetLastError()
#define EN_WOULDBLOCK(e) ((e) == WSAEWOULDBLOCK)
#define EN_INPROGRESS(e) ((e) == WSAEWOULDBLOCK)
#define EN_REFUSED WSAECONNREFUSED
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int sock_t;
#define EN_INVALID (-1)
#define EN_CLOSE close
#define EN_POLL poll
#define EN_ERRNO errno
#define EN_WOULDBLOCK(e) ((e) == EAGAIN || (e) == EWOULDBLOCK || (e) == EINTR)
#define EN_INPROGRESS(e) ((e) == EINPROGRESS || (e) == EINTR)
#define EN_REFUSED ECONNREFUSED
#endif

namespace eden_net {

int64_t nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

bool startup() {
#ifdef _WIN32
    static std::once_flag once;
    static bool ok = false;
    std::call_once(once, [] { WSADATA d; ok = WSAStartup(MAKEWORD(2, 2), &d) == 0; });
    return ok;
#else
    return true;
#endif
}

static void setNonBlocking(sock_t s) {
#ifdef _WIN32
    u_long one = 1;
    ioctlsocket(s, FIONBIO, &one);
#else
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
}

static void tuneSocket(sock_t s) {
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof one);
#ifdef SO_NOSIGPIPE
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, (const char*)&one, sizeof one);
#endif
}

static bool pollOne(sock_t s, short events, int ms) {
    pollfd p;
    p.fd = s;
    p.events = events;
    p.revents = 0;
    int r = EN_POLL(&p, 1, ms < 0 ? 0 : ms);
    return r > 0;   // POLLERR/POLLHUP also wake: the following recv/send reports it
}

ConnErr tcpConnect(const char* host, int port, int timeoutMs, Sock& out, int* connectMs) {
    startup();
    out = Sock();
    const int64_t t0 = nowMs();
    addrinfo hints;
    std::memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    const std::string portStr = std::to_string(port);
    if (getaddrinfo(host, portStr.c_str(), &hints, &res) != 0 || !res) return CONN_DNS;

    ConnErr last = CONN_OTHER;
    for (addrinfo* ai = res; ai; ai = ai->ai_next) {
        const int64_t left = timeoutMs - (nowMs() - t0);
        if (left <= 0) { last = CONN_TIMEOUT; break; }
        sock_t s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == EN_INVALID) continue;
        setNonBlocking(s);
        int r = connect(s, ai->ai_addr, (int)ai->ai_addrlen);
        int err = r == 0 ? 0 : EN_ERRNO;
        if (r != 0 && EN_INPROGRESS(err)) {
            if (!pollOne(s, POLLOUT, (int)left)) {
                last = CONN_TIMEOUT;
                EN_CLOSE(s);
                continue;
            }
            socklen_t len = sizeof err;
            getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&err, &len);
        }
        if (err == 0) {
            tuneSocket(s);
            out.h = (intptr_t)s;
            if (connectMs) *connectMs = (int)(nowMs() - t0);
            freeaddrinfo(res);
            return CONN_OK;
        }
        last = err == EN_REFUSED ? CONN_REFUSED : CONN_OTHER;
#ifndef _WIN32
        if (err == ETIMEDOUT || err == EHOSTUNREACH || err == ENETUNREACH) last = CONN_TIMEOUT;
#else
        if (err == WSAETIMEDOUT || err == WSAEHOSTUNREACH || err == WSAENETUNREACH) last = CONN_TIMEOUT;
#endif
        EN_CLOSE(s);
    }
    freeaddrinfo(res);
    return last;
}

int sendSome(Sock& s, const void* p, size_t n) {
#ifdef MSG_NOSIGNAL
    const int flags = MSG_NOSIGNAL;
#else
    const int flags = 0;
#endif
    int r = (int)send((sock_t)s.h, (const char*)p, (int)n, flags);
    if (r >= 0) return r;
    return EN_WOULDBLOCK(EN_ERRNO) ? IO_WOULDBLOCK : IO_ERROR;
}

int recvSome(Sock& s, void* p, size_t n) {
    int r = (int)recv((sock_t)s.h, (char*)p, (int)n, 0);
    if (r >= 0) return r;
    return EN_WOULDBLOCK(EN_ERRNO) ? IO_WOULDBLOCK : IO_ERROR;
}

bool waitReadable(Sock& s, int ms) { return s.valid() && pollOne((sock_t)s.h, POLLIN, ms); }

bool sendAll(Sock& s, const void* p, size_t n, int timeoutMs) {
    const char* c = (const char*)p;
    const int64_t t0 = nowMs();
    while (n) {
        int r = sendSome(s, c, n);
        if (r > 0) { c += r; n -= (size_t)r; continue; }
        if (r == IO_ERROR || nowMs() - t0 > timeoutMs) return false;
        pollOne((sock_t)s.h, POLLOUT, 10);
    }
    return true;
}

void closeSock(Sock& s) {
    if (s.valid()) EN_CLOSE((sock_t)s.h);
    s.h = -1;
}

bool tcpListen(int port, Sock& out, int* boundPort) {
    startup();
    sock_t s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == EN_INVALID) return false;
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof one);
    sockaddr_in a;
    std::memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons((unsigned short)port);
    if (bind(s, (sockaddr*)&a, sizeof a) != 0 || listen(s, 8) != 0) { EN_CLOSE(s); return false; }
    socklen_t len = sizeof a;
    getsockname(s, (sockaddr*)&a, &len);
    if (boundPort) *boundPort = ntohs(a.sin_port);
    setNonBlocking(s);
    out.h = (intptr_t)s;
    return true;
}

bool tcpAccept(Sock& listener, int timeoutMs, Sock& out) {
    if (!pollOne((sock_t)listener.h, POLLIN, timeoutMs)) return false;
    sock_t c = accept((sock_t)listener.h, nullptr, nullptr);
    if (c == EN_INVALID) return false;
    setNonBlocking(c);
    tuneSocket(c);
    out.h = (intptr_t)c;
    return true;
}

}  // namespace eden_net
