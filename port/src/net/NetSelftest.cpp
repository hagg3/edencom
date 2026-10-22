// NetSelftest.cpp — MP.1/MP.2 gates: `--net-selftest` (framer, base64, SNAPZ codec, socket) and
// `--mp-selftest` (the protocol session). Both run fully offline against an in-process mock
// server on loopback, so every CI leg runs them; with `--server=host:port` (a local
// `edenserver`, see port/tools/mp-live-gate.sh) they add a live leg. No engine, no window.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "MpSession.h"
#include "NetFramer.h"
#include "NetSock.h"
#include "ServerList.h"
#include "Snapz.h"

using namespace eden_net;

namespace {

int g_checks = 0, g_fail = 0;
const char* g_tag = "net-selftest";

void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) { ++g_fail; std::printf("[%s] FAIL %s\n", g_tag, what); }
}
#define CHECK(c) check((c), #c)

// ---- mock server --------------------------------------------------------------------------

bool readLine(Sock& s, LineFramer& fr, std::string& out, int ms) {
    const int64_t t0 = nowMs();
    char buf[4096];
    for (;;) {
        if (fr.next(out)) return true;
        if (nowMs() - t0 > ms) return false;
        if (!waitReadable(s, 20)) continue;
        const int r = recvSome(s, buf, sizeof buf);
        if (r > 0) fr.feed(buf, (size_t)r);
        else if (r == 0 || r == IO_ERROR) return fr.next(out);
    }
}

void putLine(Sock& s, const std::string& l, size_t chunk = 0) {
    const std::string d = l + "\n";
    if (!chunk) { sendAll(s, d.data(), d.size(), 3000); return; }
    for (size_t i = 0; i < d.size(); i += chunk) {   // dribble, to exercise reassembly
        sendAll(s, d.data() + i, std::min(chunk, d.size() - i), 3000);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

struct Mock {
    Sock ls;
    int port = 0;
    std::thread th;
    bool start(std::function<void(Sock&)> script) {
        if (!tcpListen(0, ls, &port)) return false;
        th = std::thread([this, script] {
            Sock c;
            if (tcpAccept(ls, 5000, c)) { script(c); closeSock(c); }
        });
        return true;
    }
    ~Mock() { if (th.joinable()) th.join(); closeSock(ls); }
};

// ---- event collection ---------------------------------------------------------------------

struct Got {
    std::vector<MpSideEvent> side;
    std::vector<MpWorldEvent> world;
    bool has(MpSideEvent::Kind k) const { for (auto& e : side) if (e.kind == k) return true; return false; }
    const MpSideEvent* find(MpSideEvent::Kind k) const { for (auto& e : side) if (e.kind == k) return &e; return nullptr; }
    const MpSideEvent* error() const { return find(MpSideEvent::Error); }
    int count(MpSideEvent::Kind k) const { int n = 0; for (auto& e : side) n += e.kind == k; return n; }
};

// Pump until `done(got)` or timeout.
bool pump(MpSession& s, Got& g, int ms, const std::function<bool(const Got&)>& done) {
    const int64_t t0 = nowMs();
    while (nowMs() - t0 < ms) {
        s.drainSide(g.side);
        s.drainWorld(g.world);
        if (done(g)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    s.drainSide(g.side);
    s.drainWorld(g.world);
    return done(g);
}

bool closedStage(const Got& g) {
    for (auto& e : g.side) if (e.kind == MpSideEvent::Stage && e.stage == MpStage::Closed) return true;
    return false;
}

MpConfig cfgFor(int port) {
    MpConfig c;
    c.host = "127.0.0.1";
    c.port = port;
    c.username = "Sam";
    c.characterType = 3;
    c.password = "pw";
    c.connectTimeoutMs = 2000;
    return c;
}

std::vector<SnapzRec> sampleRecs(int n, int seed) {
    std::vector<SnapzRec> r;
    for (int i = 0; i < n; ++i)
        r.push_back(SnapzRec{65536 + i * 3 - seed, (i * 7 + seed) % 256, 65500 + i, i % 3 == 0 ? 1 : (i % 3 == 1 ? 0 : 3),
                             i % 3 == 0 ? -1 : (i * 5 + seed) % 55});
    return r;
}

bool sameRecs(const std::vector<SnapzRec>& a, const std::vector<SnapzRec>& b) {
    return a.size() == b.size() && (a.empty() || !std::memcmp(a.data(), b.data(), a.size() * sizeof(SnapzRec)));
}

// ---- --net-selftest legs ------------------------------------------------------------------

void legFramer() {
    // Every split point of a three-line stream yields the same lines.
    const std::string stream = "alpha\nbe:ta\r\n\ngamma\n";
    for (size_t cut = 0; cut <= stream.size(); ++cut) {
        LineFramer f;
        std::vector<std::string> got;
        std::string l;
        f.feed(stream.data(), cut);
        while (f.next(l)) got.push_back(l);
        f.feed(stream.data() + cut, stream.size() - cut);
        while (f.next(l)) got.push_back(l);
        if (!(got.size() == 4 && got[0] == "alpha" && got[1] == "be:ta\r" && got[2].empty() && got[3] == "gamma")) {
            check(false, "framer reassembles at every split point");
            return;
        }
    }
    check(true, "framer reassembles at every split point");
    CHECK(kMaxLine == 8192 && kMaxSnapzLine == (size_t(64) << 20));

    LineFramer f(16, 64);   // tiny caps, same logic as 8 KB / 64 MB
    std::string l;
    f.feed("0123456789abcdef\n", 17);   // exactly the cap: accepted
    CHECK(f.next(l) && l.size() == 16);
    f.feed("0123456789abcdefX", 17);    // one past the cap with no newline
    CHECK(!f.next(l) && f.overflowed());
    LineFramer g(16, 64);
    g.feed("SNAPZ:3:", 8);
    std::string big(40, 'A');
    g.feed(big.data(), big.size());
    CHECK(!g.next(l) && !g.overflowed());   // a SNAPZ line may pass the short cap
    g.feed("\n", 1);
    CHECK(g.next(l) && l.size() == 48);
    LineFramer h(16, 64);
    h.feed("SNAPZ:", 6);
    std::string huge(70, 'A');
    h.feed(huge.data(), huge.size());
    CHECK(!h.next(l) && h.overflowed());    // ...but not the SNAPZ cap
    LineFramer w(16, 64);
    std::string whole(30, 'x');
    whole += "\n";
    w.feed(whole.data(), whole.size());
    CHECK(!w.next(l) && w.overflowed());    // an over-long line that arrived complete
}

void legBase64() {
    const char* vec[][2] = {{"", ""}, {"f", "Zg"}, {"fo", "Zm8"}, {"foo", "Zm9v"}, {"foob", "Zm9vYg"}, {"fooba", "Zm9vYmE"}, {"foobar", "Zm9vYmFy"}};
    bool ok = true;
    for (auto& v : vec) {
        std::string d;
        ok = ok && base64Encode(v[0], std::strlen(v[0])) == v[1] && base64Decode(v[1], std::strlen(v[1]), d) && d == v[0];
    }
    check(ok, "base64 RFC 4648 vectors (unpadded)");
    std::string d;
    CHECK(base64Decode("Zm9vYg==", 8, d) && d == "foob");   // padding tolerated
    CHECK(!base64Decode("Zm9v!g", 6, d));
    CHECK(!base64Decode("Zm9vY", 5, d));                     // length % 4 == 1
    std::string all;
    for (int i = 0; i < 256; ++i) all += (char)i;
    CHECK(base64Decode(base64Encode(all.data(), all.size()).c_str(), base64Encode(all.data(), all.size()).size(), d) && d == all);
}

void legSnapz() {
    std::vector<SnapzRec> out;
    const char* why = nullptr;
    for (int n : {0, 1, 7, 3000}) {
        const auto in = sampleRecs(n, n);
        const std::string line = encodeSnapzLine(in.data(), in.size(), n == 7 ? 6 : 1);
        CHECK(parseSnapzLine(line, out, &why) && sameRecs(in, out));
    }
    CHECK(parseSnapzLine("SNAPZ:0:", out, &why) && out.empty());   // the server's empty-region frame
    const auto in = sampleRecs(40, 1);
    std::string line = encodeSnapzLine(in.data(), in.size());
    std::string lie = "SNAPZ:41" + line.substr(line.find(':', 6));
    CHECK(!parseSnapzLine(lie, out, &why));                       // count longer than payload
    lie = "SNAPZ:39" + line.substr(line.find(':', 6));
    CHECK(!parseSnapzLine(lie, out, &why));                       // count shorter than payload
    CHECK(!parseSnapzLine(line.substr(0, line.size() - 6), out, &why));   // truncated stream
    CHECK(!parseSnapzLine("SNAPZ:x:AAAA", out, &why));
    CHECK(!parseSnapzLine("SNAPZ:99999999999:AAAA", out, &why));
    // A bomb: 4M records claimed, a tiny payload that cannot hold them.
    const std::vector<SnapzRec> zeros(1);
    const std::string tiny = encodeSnapzLine(zeros.data(), 1, 9);
    CHECK(!parseSnapzLine("SNAPZ:4000000" + tiny.substr(tiny.find(':', 6)), out, &why));
    // Reply order across cells is the server's; the codec must not reorder.
    const auto many = sampleRecs(500, 9);
    CHECK(parseSnapzLine(encodeSnapzLine(many.data(), many.size()), out, &why) && sameRecs(many, out));
}

void legSocket() {
    startup();
    // Loopback round trip with the server dribbling a long SNAPZ line in odd chunks.
    const auto recs = sampleRecs(3000, 4);
    const std::string snap = encodeSnapzLine(recs.data(), recs.size());
    Mock m;
    CHECK(m.start([&](Sock& c) {
        putLine(c, "hello:world", 3);
        std::string both = "second\n" + snap + "\nthird\n";
        for (size_t i = 0; i < both.size(); i += 1301) sendAll(c, both.data() + i, std::min<size_t>(1301, both.size() - i), 3000);
        LineFramer fr;
        std::string l;
        if (readLine(c, fr, l, 3000)) putLine(c, "echo:" + l);
    }));
    Sock s;
    int ms = -1;
    CHECK(tcpConnect("127.0.0.1", m.port, 2000, s, &ms) == CONN_OK && ms >= 0 && s.valid());
    LineFramer fr;
    std::string l;
    CHECK(readLine(s, fr, l, 3000) && l == "hello:world");
    CHECK(readLine(s, fr, l, 3000) && l == "second");
    std::vector<SnapzRec> got;
    const char* why = nullptr;
    CHECK(readLine(s, fr, l, 3000) && parseSnapzLine(l, got, &why) && sameRecs(recs, got));
    CHECK(readLine(s, fr, l, 3000) && l == "third");
    putLine(s, "ping");
    CHECK(readLine(s, fr, l, 3000) && l == "echo:ping");
    CHECK(!readLine(s, fr, l, 300));   // peer closed: no more lines
    CHECK(recvSome(s, &ms, 1) == 0 || recvSome(s, &ms, 1) == IO_ERROR);
    closeSock(s);

    // Connect failures map to their own codes.
    int deadPort = 0;
    { Sock t; tcpListen(0, t, &deadPort); closeSock(t); }
    Sock d;
    CHECK(tcpConnect("127.0.0.1", deadPort, 2000, d) == CONN_REFUSED);
    CHECK(tcpConnect("no-such-host.invalid", 27015, 2000, d) == CONN_DNS);
}

void legLiveNet(const std::string& server) {
    const size_t c = server.rfind(':');
    const std::string host = server.substr(0, c);
    const int port = c == std::string::npos ? 27015 : std::atoi(server.c_str() + c + 1);
    Sock s;
    CHECK(tcpConnect(host.c_str(), port, 3000, s) == CONN_OK);
    if (!s.valid()) return;
    LineFramer fr;
    std::string l;
    putLine(s, "PING");
    CHECK(readLine(s, fr, l, 3000) && l == "PONG");
    putLine(s, "JOIN:NetSelf:0::zr");
    putLine(s, "SIGNQ");
    putLine(s, "REGION:65536:65536");
    bool welcome = false, caps = false, snapz = false;
    for (int i = 0; i < 40 && !(welcome && caps && snapz); ++i) {
        if (!readLine(s, fr, l, 3000)) break;
        if (l.compare(0, 18, "[Server] Welcome, ") == 0) welcome = true;
        else if (l == "CAPS:region") caps = true;
        else if (l.compare(0, 6, "SNAPZ:") == 0) {
            std::vector<SnapzRec> r;
            const char* why = nullptr;
            check(parseSnapzLine(l, r, &why), "live SNAPZ frame decodes");
            snapz = true;
        }
    }
    CHECK(welcome);
    CHECK(caps);
    CHECK(snapz);
    closeSock(s);
}

// ---- --mp-selftest legs -------------------------------------------------------------------

void legJoin() {
    const auto r1 = sampleRecs(3000, 1), r2 = sampleRecs(120, 2);
    std::vector<std::string> seen;   // what the client sent, in order
    Mock m;
    CHECK(m.start([&](Sock& c) {
        LineFramer fr;
        std::string l;
        for (int i = 0; i < 3 && readLine(c, fr, l, 3000); ++i) seen.push_back(l);
        putLine(c, "[Server] Welcome, Sam! (Character Type: 3)");
        putLine(c, "[Server] Be nice.");
        putLine(c, "[Server] The name Sam is already in use; you are Sam-2.");
        putLine(c, "CAPS:region");
        putLine(c, "SPAWN:65536.00:34.00:65540.50");
        putLine(c, "SIGNP:server:65530:33:65531:1:0:2:Hello: a sign", 7);
        putLine(c, encodeSnapzLine(r1.data(), r1.size()), 997);
        putLine(c, encodeSnapzLine(r2.data(), r2.size()));
        putLine(c, "[Bob (T2)] hi: there");
        putLine(c, "POSVEL:Bob:2:100.00:34.00:200.50:1.00:0.00:-1.50");
        putLine(c, "POS:Bob:2:1.00:2.00:3.00");
        putLine(c, "VEL:Bob:2:4.00:5.00:6.00");
        putLine(c, "ACTION:Bob:2:65540:33:65541:3:17");
        putLine(c, "ACTION:server:0:65540:33:65541:1");
        putLine(c, "WHATEVER:ignored");
        while (readLine(c, fr, l, 4000)) {
            seen.push_back(l);
            if (l == "PING") putLine(c, "PONG");
            if (l == "MSG:last: line") break;
        }
    }));
    MpSession s;
    MpConfig cfg = cfgFor(m.port);
    cfg.pingIntervalMs = 100;
    CHECK(s.start(cfg));
    Got g;
    CHECK(pump(s, g, 5000, [](const Got& x) { return x.count(MpSideEvent::Pong) >= 1 && x.world.size() >= 5; }));
    const bool cleanSoFar = !g.error();
    s.sendChat("hello: world");
    s.sendAction(65540, 33, 65541, 0, 17);
    s.sendAction(65540, 33, 65541, 1, 5);   // mine carries no payload
    s.sendPosVel(1, 2, 3, 0, 0, 0);
    s.sendChat("last: line");
    pump(s, g, 800, [](const Got&) { return false; });
    s.stop();
    pump(s, g, 100, [](const Got&) { return false; });

    CHECK(seen.size() >= 3 && seen[0] == "JOIN:Sam:3:pw:zr" && seen[1] == "SIGNQ" && seen[2] == "REGION:65536:65536");
    auto sent = [&](const char* l) { for (auto& x : seen) if (x == l) return true; return false; };
    CHECK(sent("MSG:hello: world"));
    CHECK(sent("ACTION:65540:33:65541:0:17"));
    CHECK(sent("ACTION:65540:33:65541:1"));
    CHECK(sent("POSVEL:1.00:2.00:3.00:0.00:0.00:0.00"));
    CHECK(sent("PING"));

    // Stages go Connecting, Joining, Receiving in order.
    std::vector<MpStage> st;
    for (auto& e : g.side) if (e.kind == MpSideEvent::Stage) st.push_back(e.stage);
    CHECK(st.size() >= 3 && st[0] == MpStage::Connecting && st[1] == MpStage::Joining && st[2] == MpStage::Receiving);
    const MpSideEvent* w = g.find(MpSideEvent::Welcome);
    CHECK(w && w->name == "Sam" && w->type == 3);
    const MpSideEvent* rn = g.find(MpSideEvent::Renamed);
    CHECK(rn && rn->text == "Sam-2");
    CHECK(g.has(MpSideEvent::Caps));
    const MpSideEvent* sp = g.find(MpSideEvent::Spawn);
    CHECK(sp && sp->v[0] == 65536.0 && sp->v[1] == 34.0 && sp->v[2] == 65540.5);
    CHECK(g.count(MpSideEvent::Pong) >= 1);
    CHECK(cleanSoFar);

    bool motd = false, bob = false;
    for (auto& e : g.side)
        if (e.kind == MpSideEvent::Chat) {
            if (e.fromServer && e.text == "Be nice.") motd = true;
            if (!e.fromServer && e.name == "Bob" && e.type == 2 && e.text == "hi: there") bob = true;
        }
    CHECK(motd);
    CHECK(bob);
    int posvel = 0, pos = 0, vel = 0;
    for (auto& e : g.side)
        if (e.kind == MpSideEvent::Peer) {
            if (e.peerVerb == 2 && e.name == "Bob" && e.v[0] == 100 && e.v[2] == 200.5 && e.v[3] == 1 && e.v[5] == -1.5) ++posvel;
            if (e.peerVerb == 0 && e.v[2] == 3) ++pos;
            if (e.peerVerb == 1 && e.v[3] == 4 && e.v[5] == 6) ++vel;
        }
    CHECK(posvel == 1 && pos == 1 && vel == 1);

    // World stream: signp, two snapz frames, then two actions — in server order.
    std::vector<MpWorldEvent> sn;
    int signs = 0, acts = 0;
    bool order = true;
    int idx = 0;
    for (auto& e : g.world) {
        if (e.kind == MpWorldEvent::SignP) { ++signs; order = order && idx == 0; CHECK(e.who == "server" && e.x == 65530 && e.y == 33 && e.z == 65531 && e.sc == 2 && e.text == "Hello: a sign"); }
        else if (e.kind == MpWorldEvent::Snapz) { sn.push_back(e); order = order && idx >= 1 && idx <= 2; }
        else { ++acts; order = order && idx >= 3; }
        ++idx;
    }
    CHECK(order && signs == 1 && acts == 2 && sn.size() == 2);
    CHECK(sn.size() == 2 && sameRecs(r1, sn[0].recs) && sameRecs(r2, sn[1].recs));
    bool a1 = false, a2 = false;
    for (auto& e : g.world)
        if (e.kind == MpWorldEvent::Action) {
            if (e.who == "Bob" && e.type == 2 && e.x == 65540 && e.mode == 3 && e.extra == 17) a1 = true;
            if (e.who == "server" && e.mode == 1 && e.extra == -1) a2 = true;
        }
    CHECK(a1 && a2);
    CHECK(s.snapzRecords() == r1.size() + r2.size());
}

// One scripted refusal / failure; returns the session's Error (or null) once Closed.
struct Outcome { Got g; MpError err = MpError::None; std::string text; bool errored = false; };

Outcome runScript(const std::function<void(Sock&)>& script, const std::function<void(MpConfig&)>& tweak = nullptr) {
    Mock m;
    m.start(script);
    MpSession s;
    MpConfig cfg = cfgFor(m.port);
    if (tweak) tweak(cfg);
    s.start(cfg);
    Outcome o;
    pump(s, o.g, 6000, closedStage);
    if (const MpSideEvent* e = o.g.error()) { o.errored = true; o.err = e->err; o.text = e->text; }
    return o;
}

void drain(Sock& c) { LineFramer fr; std::string l; readLine(c, fr, l, 3000); }   // JOIN

void legErrors() {
    Outcome o = runScript([](Sock& c) { drain(c); putLine(c, "[Server] Server full."); });
    CHECK(o.errored && o.err == MpError::ServerFull);

    o = runScript([](Sock& c) { drain(c); putLine(c, "[Server] Wrong password."); });
    CHECK(o.errored && o.err == MpError::WrongPassword);

    o = runScript([](Sock& c) { drain(c); putLine(c, "[Server] Invalid name (too long)."); });
    CHECK(o.errored && o.err == MpError::InvalidName && o.text == "too long");

    o = runScript([](Sock& c) { drain(c); putLine(c, "[Server] Maintenance, back soon."); },
                  nullptr);
    CHECK(o.errored && o.err == MpError::ServerClosed && o.text == "Maintenance, back soon.");

    o = runScript([](Sock& c) { drain(c); });   // closes at once with nothing said
    CHECK(o.errored && o.err == MpError::ServerClosed && o.text.empty());

    o = runScript([](Sock& c) { drain(c); std::this_thread::sleep_for(std::chrono::milliseconds(1500)); },
                  [](MpConfig& c) { c.welcomeTimeoutMs = 400; });
    CHECK(o.errored && o.err == MpError::NoWelcome);

    // After the welcome a close is Lost, with one terminal Error only.
    o = runScript([](Sock& c) { drain(c); putLine(c, "[Server] Welcome, Sam! (Character Type: 3)"); });
    CHECK(o.errored && o.err == MpError::Lost && o.g.count(MpSideEvent::Error) == 1 && o.g.has(MpSideEvent::Welcome));

    // PONG silence past the limit is Lost too (the mock stays up and says nothing).
    o = runScript([](Sock& c) {
        drain(c);
        putLine(c, "[Server] Welcome, Sam! (Character Type: 3)");
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    }, [](MpConfig& c) { c.pingIntervalMs = 100; c.pongSilenceMs = 500; });
    CHECK(o.errored && o.err == MpError::Lost && o.text.find("no reply") != std::string::npos);

    // A corrupt SNAPZ and an oversize line are protocol errors, not crashes.
    o = runScript([](Sock& c) { drain(c); putLine(c, "[Server] Welcome, Sam! (Character Type: 3)"); putLine(c, "SNAPZ:5:AAAA"); });
    CHECK(o.errored && o.err == MpError::Protocol);
    o = runScript([](Sock& c) {
        drain(c);
        std::string junk(9000, 'j');
        putLine(c, junk);
    });
    CHECK(o.errored && o.err == MpError::Protocol);
    o = runScript([](Sock& c) { drain(c); putLine(c, "[Server] Welcome, Sam! (Character Type: 3)"); },
                  [](MpConfig& c) { c.worldQueueCapBytes = 100; });   // cap is only hit by world events
    CHECK(o.errored && o.err == MpError::Lost);
    {
        const auto big = sampleRecs(50, 3);
        o = runScript([&](Sock& c) {
            drain(c);
            putLine(c, "[Server] Welcome, Sam! (Character Type: 3)");
            putLine(c, encodeSnapzLine(big.data(), big.size()));
        }, [](MpConfig& c) { c.worldQueueCapBytes = 100; });
        CHECK(o.errored && o.err == MpError::TooMuchData);
    }

    // Connect failures.
    int deadPort = 0;
    { Sock t; tcpListen(0, t, &deadPort); closeSock(t); }
    {
        MpSession s;
        MpConfig cfg = cfgFor(deadPort);
        s.start(cfg);
        Got g;
        pump(s, g, 5000, closedStage);
        CHECK(g.error() && g.error()->err == MpError::Refused);
        MpSession s2;
        cfg.host = "no-such-host.invalid";
        s2.start(cfg);
        Got g2;
        pump(s2, g2, 8000, closedStage);
        CHECK(g2.error() && g2.error()->err == MpError::Dns);
    }
    // Bad input never reaches the network.
    {
        MpSession s;
        MpConfig cfg = cfgFor(1);
        cfg.username = "a:b";
        CHECK(!s.start(cfg));
        Got g;
        pump(s, g, 50, [](const Got&) { return false; });
        CHECK(g.error() && g.error()->err == MpError::BadInput);
        cfg.username = "ok";
        cfg.password = "x:y";
        CHECK(!s.start(cfg));
    }
    // characterType is clamped to 0..10 on the wire.
    std::string join;
    Mock m;
    m.start([&](Sock& c) { LineFramer fr; readLine(c, fr, join, 3000); });
    MpSession s;
    MpConfig cfg = cfgFor(m.port);
    cfg.characterType = 99;
    cfg.password.clear();
    s.start(cfg);
    Got g;
    pump(s, g, 3000, closedStage);
    CHECK(join == "JOIN:Sam:10::zr");
}

// ---- Q.6: memory caps and a non-blocking cancel -------------------------------------------

void legRobustness() {
    // The framer frees a big line's buffer once it is consumed (F21: it kept up to 64 MB).
    {
        LineFramer f;
        std::string big = "SNAPZ:1:" + std::string(size_t(3) << 20, 'A') + "\n";
        f.feed(big.data(), big.size());
        std::string l;
        CHECK(f.next(l) && l.size() == big.size() - 1);
        CHECK(f.capacity() <= kShrinkAbove);
    }
    // ACTION and SIGNP count against the cap too, not only SNAPZ.
    {
        Outcome o = runScript([](Sock& c) {
            drain(c);
            putLine(c, "[Server] Welcome, Sam! (Character Type: 3)");
            putLine(c, "ACTION:bob:1:10:20:30:0:5");
        }, [](MpConfig& c) { c.worldQueueCapBytes = 100; });
        CHECK(o.errored && o.err == MpError::TooMuchData);
    }
    // The cap spans drains: what the consumer still holds (setHeldBytes) counts. Two frames 300 ms
    // apart, each under the cap; drained between them, so only the held bytes can trip it.
    const auto recs = sampleRecs(200, 5);
    MpWorldEvent probe;
    probe.kind = MpWorldEvent::Snapz;
    probe.recs = recs;
    const size_t one = MpSession::eventBytes(probe);
    for (int hold = 0; hold < 2; ++hold) {
        Mock m;
        m.start([&](Sock& c) {
            drain(c);
            putLine(c, "[Server] Welcome, Sam! (Character Type: 3)");
            putLine(c, encodeSnapzLine(recs.data(), recs.size()));
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            putLine(c, encodeSnapzLine(recs.data(), recs.size()));
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        });
        MpSession s;
        MpConfig cfg = cfgFor(m.port);
        cfg.worldQueueCapBytes = one + one / 2;
        s.start(cfg);
        Got g;
        size_t held = 0;
        const int64_t t0 = nowMs();
        while (nowMs() - t0 < 3000 && !closedStage(g)) {
            s.drainSide(g.side);
            std::vector<MpWorldEvent> w;
            if (s.drainWorld(w)) {
                for (auto& e : w) held += MpSession::eventBytes(e);
                if (hold) s.setHeldBytes(held);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        s.drainSide(g.side);
        const bool tooMuch = g.error() && g.error()->err == MpError::TooMuchData;
        if (hold) check(tooMuch, "held bytes count against the cap across drains");
        else check(!tooMuch && held == 2 * one, "without held bytes two drained frames fit");
    }
    // F20: detach() returns at once, even mid-connect; the thread finishes alone and frees the session.
    {
        auto s = std::make_shared<MpSession>();
        MpConfig cfg = cfgFor(9);
        cfg.host = "10.255.255.1";   // unroutable: the connect hangs to its timeout (or fails fast)
        cfg.connectTimeoutMs = 1500;
        std::weak_ptr<MpSession> wk = s;
        s->start(cfg);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const int64_t t0 = nowMs();
        s->detach();
        s.reset();
        check(nowMs() - t0 < 100, "detach returns without waiting for the connect");
        const int64_t t1 = nowMs();
        while (!wk.expired() && nowMs() - t1 < 4000) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        check(wk.expired(), "a detached session frees itself when its thread ends");
    }
    // A stack session can't be kept alive by its thread: detach() falls back to a join.
    {
        int deadPort = 0;
        { Sock t; tcpListen(0, t, &deadPort); closeSock(t); }
        MpSession s;
        s.start(cfgFor(deadPort));
        s.detach();
        CHECK(s.stage() == MpStage::Closed);
    }
}

// ---- MP.6: servers.txt and the probe ------------------------------------------------------

void legServerList() {
    std::string h, err;
    int p = 0;
    CHECK(parseAddress("castle.example", h, p, &err) && h == "castle.example" && p == 27015);
    CHECK(parseAddress(" 10.0.0.2:27016 ", h, p, &err) && h == "10.0.0.2" && p == 27016);
    CHECK(parseAddress("[::1]:9", h, p, &err) && h == "::1" && p == 9);
    CHECK(parseAddress("[::1]", h, p, &err) && h == "::1" && p == 27015);
    CHECK(!parseAddress("", h, p, &err) && err == "Enter an address");
    CHECK(!parseAddress("a b", h, p, &err) && err == "No spaces in an address");
    CHECK(!parseAddress("host:0", h, p, &err) && err == "Port must be 1-65535");
    CHECK(!parseAddress("host:65536", h, p, &err) && err == "Port must be 1-65535");
    CHECK(!parseAddress("host:", h, p, &err));
    CHECK(!parseAddress("::1:5", h, p, &err));
    CHECK(!parseAddress(std::string(254, 'a'), h, p, &err));
    CHECK(nameProblem("Sam").empty());
    CHECK(!nameProblem("").empty() && !nameProblem(std::string(21, 'a')).empty());
    CHECK(!nameProblem("a:b").empty() && !nameProblem("[x]").empty() && !nameProblem(" a").empty());
    CHECK(!nameProblem("Server").empty() && !nameProblem("caf\xC3\xA9").empty());

    // Round trip: unknown cache keys and unparsable lines survive, order is kept, specials escape.
    const std::string text =
        "emod-servers 1\n"
        "Castle\tcastle.example\t27015\tpw\tname=Castle Build;players=12;max=64;locked=1;flags=tf;motd=Hi%3B there;seen=1700000000;zz=keep\n"
        "a line from a newer Emod\n"
        "LAN\t192.168.1.20\t27016\t\tlocked=?\n";
    ServerFile f;
    CHECK(parseServers(text, f));
    CHECK(f.count() == 2 && f.lines.size() == 3 && !f.lines[1].parsed);
    const ServerEntry* a = f.at(0);
    CHECK(a && a->label == "Castle" && a->password == "pw" && a->cache.name == "Castle Build" && a->cache.players == 12 &&
          a->cache.max == 64 && a->cache.locked == 1 && a->cache.flags == "tf" && a->cache.motd == "Hi; there" &&
          a->cache.seen == 1700000000 && a->cache.extra.size() == 1 && a->cache.extra[0].first == "zz");
    const ServerEntry* b = f.at(1);
    CHECK(b && b->port == 27016 && b->cache.locked == -1 && b->password.empty());
    CHECK(formatServers(f) == text);
    CHECK(f.move(1, -1) && f.at(0)->label == "LAN" && f.lines[1].raw == "a line from a newer Emod");
    f.remove(0);
    CHECK(f.count() == 1 && f.at(0)->label == "Castle" && f.lines.size() == 2);
    ServerEntry e;
    e.label = "Tab\there";
    e.host = "x";
    e.cache.motd = "a=b;c%";
    f.add(e);
    ServerFile g;
    CHECK(parseServers(formatServers(f), g) && g.count() == 2 && g.at(1)->label == "Tab here" && g.at(1)->cache.motd == "a=b;c%");
    CHECK(!parseServers("nope\n", g));

    ServerInfo i;
    CHECK(parseInfoLine("INFO:rev=1:name=Castle:players=3:max=64:locked=0:flags=t:new=x:motd=Hi: all\x01", i));
    CHECK(i.name == "Castle" && i.players == 3 && i.max == 64 && i.locked == 0 && i.flags == "t" && i.motd == "Hi: all");
    CHECK(!parseInfoLine("[Server] hi", i));
}

void legProbe() {
    // INFO answered: Online with every field; the first (only) line the server sees is INFO.
    std::string first;
    {
        Mock m;
        m.start([&](Sock& c) {
            LineFramer fr;
            readLine(c, fr, first, 2000);
            putLine(c, "INFO:rev=1:name=Mock:players=2:max=64:locked=1:flags=:motd=Welcome!");
        });
        ProbeResult r = probeServer("127.0.0.1", m.port);
        CHECK(r.status == ProbeStatus::Online && r.hasInfo && r.info.name == "Mock" && r.info.players == 2 &&
              r.info.locked == 1 && r.info.motd == "Welcome!" && r.pingMs >= 0);
    }
    CHECK(first == "INFO");
    // Silent (an official server, or ewb-server before MP.9 / --no-info): the fallback row, in ≤ 1.5 s.
    {
        Mock m;
        m.start([](Sock& c) { std::this_thread::sleep_for(std::chrono::milliseconds(2500)); (void)c; });
        const int64_t t0 = nowMs();
        ProbeResult r = probeServer("127.0.0.1", m.port, 3000, 600);
        CHECK(r.status == ProbeStatus::OnlineBasic && !r.hasInfo && nowMs() - t0 < 1500);
    }
    {
        Mock m;
        // Written at accept, before any line is read (the official server); reading our INFO before
        // the close keeps a loopback RST from racing the line.
        m.start([](Sock& c) { putLine(c, "[Server] Server full."); drain(c); });
        CHECK(probeServer("127.0.0.1", m.port).status == ProbeStatus::Full);
    }
    {
        Mock m;
        m.start([](Sock& c) { drain(c); });   // reads, closes, says nothing
        CHECK(probeServer("127.0.0.1", m.port).status == ProbeStatus::OnlineBasic);
    }
    int deadPort = 0;
    { Sock t; tcpListen(0, t, &deadPort); closeSock(t); }
    CHECK(probeServer("127.0.0.1", deadPort).status == ProbeStatus::NotRunning);
    CHECK(probeServer("no-such-host.invalid", 27015).status == ProbeStatus::UnknownHost);

    // The prober: results come back by id; a cancel drops what was still to come.
    ServerProber pr;
    ProbeResult fake;
    fake.status = ProbeStatus::Online;
    fake.pingMs = 7;
    pr.setMock({{"mock.a", fake}});
    for (int k = 0; k < 12; ++k) pr.request(k, k == 11 ? "127.0.0.1" : "mock.a", deadPort);
    std::vector<ProbeResult> got;
    const int64_t t0 = nowMs();
    while ((int)got.size() < 12 && nowMs() - t0 < 5000) {
        pr.drain(got);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    int ids = 0;
    for (auto& r : got) ids |= 1 << r.id;
    CHECK(got.size() == 12 && ids == 0xfff && pr.pending() == 0);
    for (auto& r : got) if (r.id == 11) CHECK(r.status == ProbeStatus::NotRunning);
    got.clear();
    for (int k = 0; k < 20; ++k) pr.request(100 + k, "mock.a", 1);
    pr.cancel();
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    pr.drain(got);
    CHECK(got.empty());
}

void legLiveMp(const std::string& server) {
    const size_t c = server.rfind(':');
    MpConfig cfg;
    cfg.host = server.substr(0, c);
    cfg.port = c == std::string::npos ? 27015 : std::atoi(server.c_str() + c + 1);
    cfg.username = "MpSelf";
    cfg.characterType = 2;
    cfg.pingIntervalMs = 200;
    // MP.6: the probe against a real server is Online (an INFO build, MP.9) or the fallback row;
    // never a JOIN, so the session below is the only player it ever sees.
    {
        ProbeResult r = probeServer(cfg.host, cfg.port);
        CHECK((r.status == ProbeStatus::Online || r.status == ProbeStatus::OnlineBasic) && r.pingMs >= 0);
        std::printf("[mp-selftest] live probe: %s ping=%d ms\n", r.hasInfo ? "INFO" : "no INFO (fallback row)", r.pingMs);
    }
    MpSession a, b;
    CHECK(a.start(cfg));
    Got ga, gb;
    CHECK(pump(a, ga, 8000, [](const Got& x) { return x.has(MpSideEvent::Caps) && x.has(MpSideEvent::Welcome) && x.count(MpSideEvent::Pong) >= 1; }));
    CHECK(pump(a, ga, 8000, [&](const Got&) { return a.snapzRecords() > 0 || !ga.world.empty(); }));
    CHECK(!ga.error());
    const MpSideEvent* w = ga.find(MpSideEvent::Welcome);
    CHECK(w && w->name == "MpSelf" && w->type == 2);

    // A second client under the same name is renamed, and sees the first one's chat.
    CHECK(b.start(cfg));
    CHECK(pump(b, gb, 8000, [](const Got& x) { return x.has(MpSideEvent::Welcome); }));
    CHECK(pump(b, gb, 3000, [](const Got& x) { return x.has(MpSideEvent::Renamed); }));
    a.sendChat("from a");
    CHECK(pump(b, gb, 5000, [](const Got& x) {
        for (auto& e : x.side) if (e.kind == MpSideEvent::Chat && !e.fromServer && e.text == "from a") return true;
        return false;
    }));
    a.sendPosVel(65536, 40, 65536, 0, 0, 0);
    CHECK(pump(b, gb, 5000, [](const Got& x) { return x.has(MpSideEvent::Peer); }));
    b.stop();
    a.stop();
}


}  // namespace

extern "C" int eden_net_selftest_run(const char* server) {
    g_tag = "net-selftest";
    g_checks = g_fail = 0;
    startup();
    legFramer();
    legBase64();
    legSnapz();
    legSocket();
    if (server && *server) legLiveNet(server);
    std::printf("[net-selftest] %d checks, %d failed%s\n", g_checks, g_fail, server && *server ? " (live leg ran)" : " (offline)");
    return g_fail ? 1 : 0;
}

extern "C" int eden_mp_selftest_run(const char* server) {
    g_tag = "mp-selftest";
    g_checks = g_fail = 0;
    startup();
    legJoin();
    legErrors();
    legRobustness();   // Q.6
    legServerList();   // MP.6
    legProbe();
    if (server && *server) legLiveMp(server);
    std::printf("[mp-selftest] %d checks, %d failed%s\n", g_checks, g_fail, server && *server ? " (live leg ran)" : " (offline)");
    return g_fail ? 1 : 0;
}
