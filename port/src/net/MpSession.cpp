// MpSession.cpp — see MpSession.h.
#include "MpSession.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "NetFramer.h"

namespace eden_net {

// ---- small parse helpers ------------------------------------------------------------------

static std::vector<std::string> splitN(const std::string& s, char sep, size_t maxFields) {
    std::vector<std::string> f;
    size_t pos = 0;
    while (f.size() + 1 < maxFields) {
        const size_t c = s.find(sep, pos);
        if (c == std::string::npos) break;
        f.emplace_back(s, pos, c - pos);
        pos = c + 1;
    }
    f.emplace_back(s, pos, std::string::npos);
    return f;
}

static bool parseInt(const std::string& s, int& v) {
    if (s.empty()) return false;
    char* e = nullptr;
    const long r = std::strtol(s.c_str(), &e, 10);
    if (*e || r < -2147483647L || r > 2147483647L) return false;
    v = (int)r;
    return true;
}

static bool parseNum(const std::string& s, double& v) {
    if (s.empty()) return false;
    char* e = nullptr;
    v = std::strtod(s.c_str(), &e);
    return !*e;
}

static bool startsWith(const std::string& s, const char* p) { return s.compare(0, std::strlen(p), p) == 0; }

// ---- control ------------------------------------------------------------------------------

void MpSession::setStage(MpStage s) {
    if (stage_.exchange(s) == s) return;
    MpSideEvent e;
    e.kind = MpSideEvent::Stage;
    e.stage = s;
    pushSide(std::move(e));
}

void MpSession::pushSide(MpSideEvent&& e) {
    std::lock_guard<std::mutex> g(mu_);
    sideQ_.push_back(std::move(e));
}

void MpSession::fail(MpError err, const std::string& text) {
    if (terminal_) return;
    terminal_ = true;
    MpSideEvent e;
    e.kind = MpSideEvent::Error;
    e.err = err;
    e.text = text;
    pushSide(std::move(e));
}

bool MpSession::start(const MpConfig& cfg) {
    stop();
    cfg_ = cfg;
    stop_ = false;
    terminal_ = joined_ = false;
    lastServerLine_.clear();
    rxBytes_ = snapzRecs_ = worldBytes_ = 0;
    {
        std::lock_guard<std::mutex> g(mu_);
        sideQ_.clear();
        worldQ_.clear();
        outQ_.clear();
    }
    pendingOut_.clear();
    if (cfg_.characterType < 0) cfg_.characterType = 0;
    if (cfg_.characterType > 10) cfg_.characterType = 10;
    auto bad = [](const std::string& s) { return s.find_first_of(":\n\r") != std::string::npos; };
    if (cfg_.host.empty() || cfg_.username.empty() || bad(cfg_.username) || bad(cfg_.password)) {
        stage_ = MpStage::Closed;
        fail(MpError::BadInput, "name and password may not contain ':' and the host may not be empty");
        return false;
    }
    stage_ = MpStage::Idle;
    th_ = std::thread([this] { run(); });
    return true;
}

void MpSession::stop() {
    stop_ = true;
    if (th_.joinable()) th_.join();
}

bool MpSession::drainSide(std::vector<MpSideEvent>& out) {
    std::lock_guard<std::mutex> g(mu_);
    if (sideQ_.empty()) return false;
    for (auto& e : sideQ_) out.push_back(std::move(e));
    sideQ_.clear();
    return true;
}

bool MpSession::drainWorld(std::vector<MpWorldEvent>& out) {
    std::lock_guard<std::mutex> g(mu_);
    if (worldQ_.empty()) return false;
    for (auto& e : worldQ_) out.push_back(std::move(e));
    worldQ_.clear();
    worldBytes_ = 0;
    return true;
}

void MpSession::sendLine(const std::string& line) {
    if (stage_.load() == MpStage::Closed) return;
    std::lock_guard<std::mutex> g(mu_);
    outQ_ += line;
    outQ_ += '\n';
}
void MpSession::sendChat(const std::string& t) { sendLine("MSG:" + t); }
void MpSession::sendRegion(int x, int z) { sendLine("REGION:" + std::to_string(x) + ":" + std::to_string(z)); }
void MpSession::sendSignQ() { sendLine("SIGNQ"); }
void MpSession::sendPosVel(double px, double py, double pz, double vx, double vy, double vz) {
    char b[200];
    std::snprintf(b, sizeof b, "POSVEL:%.2f:%.2f:%.2f:%.2f:%.2f:%.2f", px, py, pz, vx, vy, vz);
    sendLine(b);
}
void MpSession::sendAction(int x, int y, int z, int mode, int extra) {
    std::string l = "ACTION:" + std::to_string(x) + ":" + std::to_string(y) + ":" + std::to_string(z) + ":" + std::to_string(mode);
    if (extra >= 0 && (mode == 0 || mode == 3)) l += ":" + std::to_string(extra);
    sendLine(l);
}

bool MpSession::flushOut() {
    std::string& pending = pendingOut_;
    if (pending.empty()) {
        std::lock_guard<std::mutex> g(mu_);
        pending.swap(outQ_);
    }
    while (!pending.empty()) {
        const int r = sendSome(sock_, pending.data(), pending.size());
        if (r > 0) { pending.erase(0, (size_t)r); continue; }
        if (r == IO_WOULDBLOCK) return true;
        pending.clear();
        return false;
    }
    return true;
}

// ---- the network thread -------------------------------------------------------------------

void MpSession::run() {
    setStage(MpStage::Connecting);
    int cms = 0;
    switch (tcpConnect(cfg_.host.c_str(), cfg_.port, cfg_.connectTimeoutMs, sock_, &cms)) {
        case CONN_OK: break;
        case CONN_DNS: fail(MpError::Dns, cfg_.host); goto done;
        case CONN_REFUSED: fail(MpError::Refused, cfg_.host + ":" + std::to_string(cfg_.port)); goto done;
        case CONN_TIMEOUT: fail(MpError::Timeout, cfg_.host + ":" + std::to_string(cfg_.port)); goto done;
        default: fail(MpError::ConnectFailed, cfg_.host + ":" + std::to_string(cfg_.port)); goto done;
    }
    connectMs_ = cms;
    setStage(MpStage::Joining);
    {
        sendLine("JOIN:" + cfg_.username + ":" + std::to_string(cfg_.characterType) + ":" + cfg_.password + ":zr");
        if (cfg_.joinBurst) { sendSignQ(); sendRegion(cfg_.regionX, cfg_.regionZ); }
        joinSentMs_ = nowMs();

        LineFramer fr;
        std::string line;
        std::vector<char> rb(64 * 1024);
        while (!stop_ && !terminal_) {
            if (!flushOut()) { if (!terminal_) fail(joined_ ? MpError::Lost : MpError::ServerClosed, lastServerLine_); break; }
            bool closed = false;
            if (waitReadable(sock_, 15)) {
                for (;;) {
                    const int r = recvSome(sock_, rb.data(), rb.size());
                    if (r > 0) {
                        rxBytes_ += (size_t)r;
                        fr.feed(rb.data(), (size_t)r);
                        while (!terminal_ && fr.next(line)) handleLine(line);
                        if (fr.overflowed()) { fail(MpError::Protocol, "line too long"); break; }
                        continue;
                    }
                    if (r == IO_WOULDBLOCK) break;
                    closed = true;   // 0 = orderly close, IO_ERROR = reset
                    break;
                }
            }
            if (terminal_) break;
            if (closed) {
                if (!joined_) fail(MpError::ServerClosed, lastServerLine_);
                else fail(MpError::Lost, "The connection closed");
                break;
            }
            const int64_t now = nowMs();
            if (!joined_) {
                if (now - joinSentMs_ > cfg_.welcomeTimeoutMs) { fail(MpError::NoWelcome, ""); break; }
            } else {
                if (now - lastPongMs_ > cfg_.pongSilenceMs) {
                    fail(MpError::Lost, "no reply for " + std::to_string(cfg_.pongSilenceMs / 1000) + " s");
                    break;
                }
                if (now - lastPingMs_ >= cfg_.pingIntervalMs) {
                    lastPingMs_ = pingSentMs_ = now;
                    sendLine("PING");
                }
            }
        }
        flushOut();   // e.g. a last chat line queued before stop()
    }
done:
    closeSock(sock_);
    setStage(MpStage::Closed);
}

void MpSession::handleLine(const std::string& line) {
    if (line.empty()) return;

    if (startsWith(line, "[Server] ")) {
        const std::string body = line.substr(9);
        if (!joined_) {
            if (startsWith(body, "Welcome, ")) {
                // "Welcome, <name>! (Character Type: <n>)"
                MpSideEvent e;
                e.kind = MpSideEvent::Welcome;
                e.text = body;
                const size_t bang = body.find("! (Character Type: ");
                if (bang != std::string::npos) {
                    e.name = body.substr(9, bang - 9);
                    parseInt(body.substr(bang + 19, body.size() - bang - 20), e.type);
                } else {
                    e.name = cfg_.username;
                }
                joined_ = true;
                lastPongMs_ = lastPingMs_ = nowMs();
                pushSide(std::move(e));
                setStage(MpStage::Receiving);
                return;
            }
            if (body == "Server full.") { fail(MpError::ServerFull, body); return; }
            if (body == "Wrong password.") { fail(MpError::WrongPassword, body); return; }
            if (startsWith(body, "Invalid name (")) {
                std::string why = body.substr(14);
                if (!why.empty() && why.back() == '.') why.pop_back();
                if (!why.empty() && why.back() == ')') why.pop_back();
                fail(MpError::InvalidName, why);
                return;
            }
            lastServerLine_ = body;   // reported if the server then closes (UI plan §7)
        }
        if (body.compare(0, 9, "The name ") == 0) {
            const size_t p = body.find("; you are ");
            if (p != std::string::npos) {
                MpSideEvent e;
                e.kind = MpSideEvent::Renamed;
                e.text = body.substr(p + 10);
                if (!e.text.empty() && e.text.back() == '.') e.text.pop_back();
                pushSide(std::move(e));
            }
        }
        MpSideEvent c;
        c.kind = MpSideEvent::Chat;
        c.fromServer = true;
        c.text = body;
        pushSide(std::move(c));
        return;
    }

    if (line[0] == '[') {   // "[name (T3)] text"
        const size_t close = line.find("] ");
        const size_t paren = line.find(" (T");
        if (close != std::string::npos && paren != std::string::npos && paren < close) {
            MpSideEvent c;
            c.kind = MpSideEvent::Chat;
            c.name = line.substr(1, paren - 1);
            parseInt(line.substr(paren + 3, close - paren - 4), c.type);
            c.text = line.substr(close + 2);
            pushSide(std::move(c));
            return;
        }
        MpSideEvent c;
        c.kind = MpSideEvent::Chat;
        c.text = line;
        pushSide(std::move(c));
        return;
    }

    if (line == "PONG") {
        MpSideEvent e;
        e.kind = MpSideEvent::Pong;
        const int64_t now = nowMs();
        e.rttMs = pingSentMs_ ? (int)(now - pingSentMs_) : 0;
        pingSentMs_ = 0;
        lastPongMs_ = now;
        pushSide(std::move(e));
        return;
    }
    if (line == "CAPS:region") {
        MpSideEvent e;
        e.kind = MpSideEvent::Caps;
        e.text = "region";
        pushSide(std::move(e));
        return;
    }
    if (startsWith(line, "SPAWN:")) {
        const auto f = splitN(line, ':', 4);
        MpSideEvent e;
        e.kind = MpSideEvent::Spawn;
        if (f.size() == 4 && parseNum(f[1], e.v[0]) && parseNum(f[2], e.v[1]) && parseNum(f[3], e.v[2])) pushSide(std::move(e));
        return;
    }
    if (startsWith(line, "SNAPZ:")) {
        MpWorldEvent w;
        w.kind = MpWorldEvent::Snapz;
        const char* why = nullptr;
        if (!parseSnapzLine(line, w.recs, &why)) { fail(MpError::Protocol, why ? why : "bad SNAPZ"); return; }
        snapzRecs_ += w.recs.size();
        const size_t bytes = w.recs.size() * sizeof(SnapzRec) + 64;
        if (worldBytes_ + bytes > cfg_.worldQueueCapBytes) { fail(MpError::TooMuchData, "Server sent more than this device can hold"); return; }
        worldBytes_ += bytes;
        std::lock_guard<std::mutex> g(mu_);
        worldQ_.push_back(std::move(w));
        return;
    }
    if (startsWith(line, "SIGNP:")) {   // SIGNP:server:x:y:z:a:b:c:text — text may hold ':'
        const auto f = splitN(line, ':', 9);
        MpWorldEvent w;
        w.kind = MpWorldEvent::SignP;
        if (f.size() == 9 && parseInt(f[2], w.x) && parseInt(f[3], w.y) && parseInt(f[4], w.z) &&
            parseInt(f[5], w.sa) && parseInt(f[6], w.sb) && parseInt(f[7], w.sc)) {
            w.who = f[1];
            w.text = f[8];
            std::lock_guard<std::mutex> g(mu_);
            worldQ_.push_back(std::move(w));
        }
        return;
    }
    if (startsWith(line, "ACTION:")) {   // ACTION:name:type:x:y:z:mode[:extra]
        const auto f = splitN(line, ':', 9);
        MpWorldEvent w;
        w.kind = MpWorldEvent::Action;
        w.extra = -1;
        if (f.size() >= 7 && parseInt(f[2], w.type) && parseInt(f[3], w.x) && parseInt(f[4], w.y) && parseInt(f[5], w.z) &&
            parseInt(f[6], w.mode) && (f.size() < 8 || parseInt(f[7], w.extra))) {
            w.who = f[1];
            std::lock_guard<std::mutex> g(mu_);
            worldQ_.push_back(std::move(w));
        }
        return;
    }
    const bool pos = startsWith(line, "POS:"), vel = startsWith(line, "VEL:"), pv = startsWith(line, "POSVEL:");
    if (pos || vel || pv) {   // VERB:name:type:numbers
        const size_t n = pv ? 6 : 3;
        const auto f = splitN(line, ':', 3 + n);
        MpSideEvent e;
        e.kind = MpSideEvent::Peer;
        e.peerVerb = pv ? 2 : (vel ? 1 : 0);
        bool ok = f.size() == 3 + n && parseInt(f[2], e.type);
        for (size_t i = 0; ok && i < n; ++i) ok = parseNum(f[3 + i], e.v[(vel ? 3 : 0) + i]);
        if (ok) {
            e.name = f[1];
            pushSide(std::move(e));
        }
        return;
    }
    // Unknown verbs are ignored (protocol: "A verb the server does not know is ignored").
}

}  // namespace eden_net
