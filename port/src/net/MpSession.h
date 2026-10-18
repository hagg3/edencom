// MpSession.h — MP.2: one multiplayer connection. A network thread owns the socket and turns
// bytes into plain data on two locked queues; the main thread drains them (engine plan §4, E4).
// Never includes engine headers. Wire facts: ~/ewb-server/docs/protocol.md.
//
// `JOIN` tag: Emod sends `zr` verbatim (server-list plan §2). To the official server the tag is
// capability letters — `z` the client inflates SNAPZ, `r` the client sends REGION (server
// answers CAPS:region). Without `r` the whole world is pushed at join. ewb-server ignores it.
#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "NetSock.h"
#include "Snapz.h"

namespace eden_net {

enum class MpStage { Idle, Connecting, Joining, Receiving, Playing, Closed };

// Why a session ended (UI plan §7 maps these to text; `text` carries the server's own line).
enum class MpError {
    None, BadInput, Dns, Refused, Timeout, ConnectFailed,
    ServerFull, WrongPassword, InvalidName,   // text = the reason for InvalidName
    NoWelcome,                                // 10 s with no welcome after JOIN
    ServerClosed,                             // closed during Joining; text = last [Server] line, no prefix
    Lost,                                     // closed or PONG silence after the welcome
    Protocol,                                 // oversize line, corrupt SNAPZ
    TooMuchData,                              // world queue past its cap
};

struct MpSideEvent {
    enum Kind { Stage, Welcome, Renamed, Chat, Spawn, Caps, Peer, Pong, Error } kind = Stage;
    MpStage stage = MpStage::Idle;
    MpError err = MpError::None;
    std::string text;       // chat text, welcome line, error text, new name (Renamed)
    std::string name;       // chat sender / peer name / own name (Welcome)
    int type = 0;           // characterType (Welcome, Chat, Peer)
    bool fromServer = false;   // Chat: a `[Server] ...` line (prefix stripped)
    int peerVerb = 0;       // Peer: 0 POS, 1 VEL, 2 POSVEL
    double v[6] = {0, 0, 0, 0, 0, 0};   // Spawn: v[0..2]; Peer: position then velocity
    int rttMs = 0;          // Pong
};

struct MpWorldEvent {
    enum Kind { Action, SignP, Snapz } kind = Action;
    std::string who;        // Action: sender ("server" for restores)
    int type = 0;           // Action: sender's characterType
    int x = 0, y = 0, z = 0, mode = 0, extra = 0;   // Action (extra = type/colour, -1 if absent); SignP: x y z
    int sa = 0, sb = 0, sc = 0;   // SignP a b c
    std::string text;       // SignP
    std::vector<SnapzRec> recs;   // Snapz (MP.3b buckets by column and merges flag 0/3)
};

struct MpConfig {
    std::string host;
    int port = 27015;
    std::string username;
    int characterType = 0;      // clamped to 0..10
    std::string password;       // may not contain ':'
    int connectTimeoutMs = 3000;
    int welcomeTimeoutMs = 10000;
    int pingIntervalMs = 10000;
    int pongSilenceMs = 30000;
    bool joinBurst = true;      // send SIGNQ + REGION right behind JOIN, as retail does
    int regionX = 65536, regionZ = 65536;
    size_t worldQueueCapBytes = size_t(256) << 20;
};

class MpSession {
public:
    MpSession() = default;
    ~MpSession() { stop(); }
    MpSession(const MpSession&) = delete;
    MpSession& operator=(const MpSession&) = delete;

    bool start(const MpConfig& cfg);   // false (+ an Error event) on BadInput
    void stop();                       // closes the socket, joins the thread; no event
    MpStage stage() const { return stage_.load(); }
    void setPlaying() { setStage(MpStage::Playing); }   // MP.3b: after the join burst is applied

    bool drainSide(std::vector<MpSideEvent>& out);
    bool drainWorld(std::vector<MpWorldEvent>& out);

    // MP.3a --mp-replay: one recorded server line through the same parser, with no socket and no
    // thread (never call it on a started session). The events land in the queues as if received.
    void feedLine(const std::string& line) { handleLine(line); }

    // Thread-safe sends; each is one line (the `\n` is added). No-ops once closed.
    void sendLine(const std::string& line);
    void sendChat(const std::string& text);                 // MSG:text (text may hold ':')
    void sendRegion(int x, int z);
    void sendSignQ();
    void sendPosVel(double px, double py, double pz, double vx, double vy, double vz);
    void sendAction(int x, int y, int z, int mode, int extra = -1);

    size_t bytesReceived() const { return rxBytes_.load(); }
    size_t snapzRecords() const { return snapzRecs_.load(); }
    int connectMs() const { return connectMs_.load(); }
    const std::string& ownName() const { return cfg_.username; }

private:
    void run();
    void setStage(MpStage s);
    void pushSide(MpSideEvent&& e);
    void fail(MpError e, const std::string& text);
    void handleLine(const std::string& line);
    bool flushOut();

    MpConfig cfg_;
    std::thread th_;
    std::atomic<bool> stop_{false};
    std::atomic<MpStage> stage_{MpStage::Idle};
    std::atomic<size_t> rxBytes_{0}, snapzRecs_{0}, worldBytes_{0};
    std::atomic<int> connectMs_{0};

    std::mutex mu_;                          // guards sideQ_, worldQ_, outQ_
    std::deque<MpSideEvent> sideQ_;
    std::deque<MpWorldEvent> worldQ_;
    std::string outQ_;

    // net thread only
    Sock sock_;
    std::string pendingOut_;   // taken from outQ_, not yet accepted by the socket
    bool joined_ = false, terminal_ = false;
    std::string lastServerLine_;
    int64_t joinSentMs_ = 0, lastPingMs_ = 0, lastPongMs_ = 0, pingSentMs_ = 0;
};

}  // namespace eden_net
