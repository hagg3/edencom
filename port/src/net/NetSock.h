// NetSock.h — MP.1: the multiplayer client's TCP seam (POSIX + Winsock), plain C++.
// Non-blocking after connect; no engine includes (the network thread owns this, see
// WORKING/multiplayer-engine-plan-2026-10-17.md §4). Not the HTTP stack (Net_native.cpp).
#pragma once
#include <cstddef>
#include <cstdint>

namespace eden_net {

enum ConnErr { CONN_OK = 0, CONN_DNS, CONN_REFUSED, CONN_TIMEOUT, CONN_OTHER };

struct Sock {
    intptr_t h = -1;
    bool valid() const { return h != -1; }
};

// recv/send results besides a byte count
enum { IO_WOULDBLOCK = -1, IO_ERROR = -2 };

bool startup();   // WSAStartup on Windows; idempotent, true elsewhere

// Resolve and connect within `timeoutMs` (the DNS lookup itself cannot be interrupted). On
// success `out` is non-blocking with TCP_NODELAY and `*connectMs` (if given) is the connect time.
ConnErr tcpConnect(const char* host, int port, int timeoutMs, Sock& out, int* connectMs = nullptr);

int  sendSome(Sock& s, const void* p, size_t n);   // bytes sent, IO_WOULDBLOCK or IO_ERROR
int  recvSome(Sock& s, void* p, size_t n);         // bytes (>0), 0 = peer closed, IO_WOULDBLOCK, IO_ERROR
bool waitReadable(Sock& s, int ms);                // true when recv would not block (data or close)
bool sendAll(Sock& s, const void* p, size_t n, int timeoutMs);   // blocking-ish helper (tests)
void closeSock(Sock& s);

// Loopback listener for the selftests' mock server (127.0.0.1, port 0 = any).
bool tcpListen(int port, Sock& out, int* boundPort);
bool tcpAccept(Sock& listener, int timeoutMs, Sock& out);

int64_t nowMs();   // steady clock

}  // namespace eden_net
