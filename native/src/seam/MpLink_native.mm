// MpLink_native.mm — MP.3b: the multiplayer join flow (WORKING/multiplayer-engine-plan-2026-10-17.md
// §7, §12) and the bridge between port/src/net's MpSession (the socket, the network thread, plain
// data on locked queues) and Classes/MpWorld (the session world). MpWorld owns the link and pumps it
// once a frame from World::update (MpWorld::tick), so this file needs no hook in the host's loop.
//
//   Connecting  MpSession connects and sends JOIN (no SIGNQ/REGION yet: joinBurst off).
//   Welcomed    the welcome line arrived; wait for CAPS:region, then up to 500 ms for SPAWN (a server
//               without CAPS gets 1 s, then the legacy path: no REGION, whatever it pushes).
//   Syncing     MpWorld::beginSession + setSpawn, SIGNQ, MpWorld::startRegions (the spawn's lattice
//               point); the world queue drains into the overlay until MpWorld::joinReady().
//   Loading     MpWorld::openSession: the menu loads the session world (flat + overlay).
//   Playing     MpSession::setPlaying; step 5c applies what queued during the load.
//
// A failure before Loading ends the join in EdenMpJoinState's FAILED (MP.6: the server list's
// Connecting window turns it into the error dialog or the password prompt); during play it shows the
// "Disconnected" dialog (Reconnect = a fresh join, Exit). Chat is MP.7; peers are MP.5.
#include "../../../Classes/MpWorld.h"
#include "../../../Classes/MpServers.h"
#include "../../../Classes/ChatFeed.h"   // MP.7
#include "../../../port/src/net/MpSession.h"
#import "World.h"
#import "Menu.h"
#import "Terrain.h"
#import "GLDialog.h"
#import "statusbar.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>

extern BOOL exit_to_menu;   // World.mm: the pause menu's Exit

namespace {

int64_t nowMs() {
    return (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

// UI plan §7's bodies (the dialog itself and its Retry/Edit buttons are MP.6).
std::string errorText(const eden_net::MpSideEvent& e, const eden_net::MpConfig& c) {
    using eden_net::MpError;
    const std::string hp = c.host + ":" + std::to_string(c.port);
    switch (e.err) {
        case MpError::Dns: return "Unknown host " + c.host + ". Check the address.";
        case MpError::Refused: return "Nothing is running at " + hp + ".";
        case MpError::Timeout:
        case MpError::ConnectFailed: return "No answer from " + hp + ". It may be offline or blocked.";
        case MpError::ServerFull: return "The server is full.";
        case MpError::WrongPassword: return "Wrong password.";
        case MpError::InvalidName: return "The server refused the name " + c.username + ": " + e.text + ".";
        case MpError::NoWelcome: return "No reply from the server.";
        case MpError::ServerClosed: return e.text.empty() ? "The server closed the connection." : e.text;
        case MpError::Lost: return e.text.rfind("no reply", 0) == 0 ? "Connection lost (" + e.text + ")." : e.text + ".";
        case MpError::TooMuchData: return "Server sent more than this device can hold.";
        default: return e.text.empty() ? "Connection error." : e.text;
    }
}

eden_net::MpConfig g_rejoinCfg;
bool g_rejoinPending = false;
int startJoin(const eden_net::MpConfig& cfg);

// MP.6: what the Connecting window shows (UI plan §6), main thread only.
EdenMpJoinState g_js;
std::vector<std::string> g_jsLines;   // the welcome and the [Server] / MOTD lines before play, ≤ 8

void jsLine(const std::string& l) {
    if (g_js.stage >= EDEN_MP_JOIN_PLAYING) return;
    if (g_jsLines.size() >= 8) g_jsLines.erase(g_jsLines.begin());
    g_jsLines.push_back(l);
    g_js.lines = (int)g_jsLines.size();
}

class NativeLink;
NativeLink* g_cur = nullptr;   // the live link (MP.6's Cancel reaches it here)

class NativeLink : public MpWorld::Link {
public:
    enum St { Connecting, Welcomed, Syncing, Loading, Playing, Dead };

    explicit NativeLink(const eden_net::MpConfig& c) : cfg_(c) {}

    bool start() {
        t0_ = nowMs();
        eden_net::MpConfig c = cfg_;
        c.joinBurst = false;   // §7: SIGNQ and REGION go out after SPAWN, at its lattice point
        return s_.start(c);
    }

    void pump() override {
        std::vector<eden_net::MpSideEvent> side;
        s_.drainSide(side);
        for (const auto& e : side) {
            switch (e.kind) {
                case eden_net::MpSideEvent::Welcome:
                    if (st_ == Connecting) { st_ = Welcomed; tWelcome_ = nowMs(); }
                    std::printf("[mp] %s\n", e.text.c_str());
                    ChatFeed::addWire(true, "", e.text);   // MP.7: kept in the chat scrollback
                    jsLine(e.text);
                    break;
                case eden_net::MpSideEvent::Caps: caps_ = true; break;
                case eden_net::MpSideEvent::Spawn:
                    if (begun_) MpWorld::serverSpawn((float)e.v[0], (float)e.v[1], (float)e.v[2]);
                    else { spawn_ = true; for (int i = 0; i < 3; ++i) sp_[i] = (float)e.v[i]; }
                    break;
                case eden_net::MpSideEvent::Peer:   // MP.5: the other players
                    if (e.name != cfg_.username) MpWorld::peerSeen(e.name.c_str(), e.type, e.peerVerb, e.v);
                    break;
                case eden_net::MpSideEvent::Chat:   // MP.7: the panel and the HUD feed; the log has it too
                    ChatFeed::addWire(e.fromServer, e.name, e.text);
                    if (e.fromServer && e.text.size() > 10 && e.text.compare(e.text.size() - 10, 10, " has left.") == 0)
                        MpWorld::peerLeft(e.text.substr(0, e.text.size() - 10).c_str());
                    std::printf("[mp] chat %s%s%s\n", e.fromServer ? "[Server] " : "", e.name.empty() ? "" : (e.name + ": ").c_str(),
                                e.text.c_str());
                    if (st_ != Playing) jsLine(std::string(e.fromServer ? "[Server] " : "") + (e.name.empty() ? "" : e.name + ": ") + e.text);
                    break;
                case eden_net::MpSideEvent::Error: fail(errorText(e, cfg_), (int)e.err); break;
                default: break;
            }
            if (st_ == Dead) break;
        }
        std::fflush(stdout);
        if (st_ == Dead) return;
        const int64_t now = nowMs();
        if (st_ == Welcomed && ((caps_ && (spawn_ || now - tWelcome_ >= 500)) || (!caps_ && now - tWelcome_ >= 1000))) begin();
        if (begun_) {
            std::vector<eden_net::MpWorldEvent> w;
            s_.drainWorld(w);
            for (const auto& e : w) {
                if (e.kind == eden_net::MpWorldEvent::Snapz) {
                    records_ += e.recs.size();
                    MpWorld::enqueueSnapz(reinterpret_cast<const MpRec*>(e.recs.data()), e.recs.size());
                } else if (e.kind == eden_net::MpWorldEvent::SignP) {
                    MpWorld::enqueueSign(e.x, e.y, e.z, e.sa, e.sb, e.sc, e.text.c_str());
                } else {
                    MpWorld::enqueueAction(e.who.c_str(), e.x, e.y, e.z, e.mode, e.extra);
                }
            }
        }
        World* wd = World::getWorld;
        if (st_ == Syncing && MpWorld::joinReady()) {
            if (MpWorld::openSession()) { st_ = Loading; tLoad_ = now; }
        }
        // The Connecting window's stage line and bar (UI plan §6).
        if (st_ == Connecting) {
            g_js.stage = s_.stage() == eden_net::MpStage::Connecting || s_.stage() == eden_net::MpStage::Idle
                             ? EDEN_MP_JOIN_CONNECTING : EDEN_MP_JOIN_JOINING;
            g_js.progress = -1.0f;
        } else if (st_ == Welcomed || st_ == Syncing) {
            g_js.stage = EDEN_MP_JOIN_RECEIVING;
            g_js.progress = -1.0f;
        } else if (st_ == Loading) {
            g_js.stage = EDEN_MP_JOIN_LOADING;
            const float all = (float)(CHUNKS_PER_SIDE * CHUNKS_PER_SIDE);
            g_js.progress = wd->doneLoading >= 1 && all > 0 ? std::min(1.0f, (float)wd->terrain->counter / all) : 0.0f;
        }
        if (st_ == Loading && wd->game_mode == GAME_MODE_PLAY && wd->terrain->loaded && wd->doneLoading == 0) {
            st_ = Playing;
            g_js.stage = EDEN_MP_JOIN_PLAYING;
            g_js.progress = 1.0f;
            s_.setPlaying();
            std::printf("[mp] mp.join host=%s:%d connect=%d ms welcome=%lld ms sync=%lld ms load=%lld ms playable=%lld ms "
                        "records=%zu bytes=%zu caps=%d\n", cfg_.host.c_str(), cfg_.port, s_.connectMs(),
                        (long long)(tWelcome_ - t0_), (long long)(tLoad_ - tSync_), (long long)(now - tLoad_),
                        (long long)(now - t0_), records_, s_.bytesReceived(), (int)caps_);
            std::fflush(stdout);
            if (!lostText_.empty()) showDisconnected();
        }
    }

    void sendLine(const char* l) override { s_.sendLine(l); }

    // MP.6: the Connecting window's Cancel (before Loading only). Called outside pump, so the drop
    // below deletes this object at once: nothing may touch a member after it.
    bool cancel() {
        if (st_ == Loading || st_ == Playing || st_ == Dead) return false;
        st_ = Dead;
        g_js.stage = EDEN_MP_JOIN_IDLE;
        std::printf("[mp] join cancelled\n");
        std::fflush(stdout);
        if (begun_) MpWorld::endSession();
        else MpWorld::dropLink();
        return true;
    }

    void release() override {
        s_.stop();
        if (g_cur == this) g_cur = nullptr;
        delete this;
        // Reconnect (§12): a fresh join once the old session is gone (endSession is what released us).
        if (g_rejoinPending) {
            g_rejoinPending = false;
            startJoin(g_rejoinCfg);
        }
    }

private:
    void begin() {
        MpWorld::beginSession(cfg_.host.c_str(), cfg_.port, cfg_.host.c_str());   // the INFO name is MP.6's
        if (spawn_) MpWorld::setSpawn(sp_[0], sp_[1], sp_[2]);
        s_.sendSignQ();
        MpWorld::startRegions(caps_);
        begun_ = true;
        st_ = Syncing;
        tSync_ = nowMs();
        std::printf("[mp] syncing (%s)\n", caps_ ? "CAPS:region" : "no CAPS: legacy push");
    }

    void fail(const std::string& why, int err) {
        std::printf("[mp] disconnected: %s\n", why.c_str());
        const St was = st_;
        st_ = Dead;
        if (was == Playing) { lostText_ = why; showDisconnected(); return; }
        if (was == Loading) { lostText_ = why; st_ = Loading; return; }   // shown once the load lands
        // MP.6: the server list's Connecting window shows it (the error dialog or the password prompt).
        g_js.stage = EDEN_MP_JOIN_FAILED;
        g_js.err = err;
        std::snprintf(g_js.text, sizeof g_js.text, "%s", why.c_str());
        if (begun_) MpWorld::endSession();   // drops the link (deferred: we are inside pump)
        else MpWorld::dropLink();
    }

    void showDisconnected() {
        static const char* const kButtons[2] = {"Reconnect", "Exit"};
        g_rejoinCfg = cfg_;
        GLDialog::show("Disconnected", lostText_.c_str(), kButtons, 2, [](int chosen) {
            g_rejoinPending = chosen == 0;
            exit_to_menu = TRUE;   // World::renderFrame -> exitToMenu -> MpWorld::endSession -> release
        });
        st_ = Dead;
        g_js.stage = EDEN_MP_JOIN_IDLE;
    }

    eden_net::MpConfig cfg_;
    eden_net::MpSession s_;
    St st_ = Connecting;
    bool caps_ = false, spawn_ = false, begun_ = false;
    float sp_[3] = {0, 0, 0};
    int64_t t0_ = 0, tWelcome_ = 0, tSync_ = 0, tLoad_ = 0;
    size_t records_ = 0;
    std::string lostText_;
};

int startJoin(const eden_net::MpConfig& cfg) {
    if (MpWorld::active() || MpWorld::link()) return 0;
    NativeLink* l = new NativeLink(cfg);
    if (!l->start()) { delete l; return 0; }
    MpWorld::setLink(l);
    g_cur = l;
    std::memset(&g_js, 0, sizeof g_js);
    g_jsLines.clear();
    g_js.stage = EDEN_MP_JOIN_CONNECTING;
    g_js.progress = -1.0f;
    std::snprintf(g_js.host, sizeof g_js.host, "%s", cfg.host.c_str());
    g_js.port = cfg.port;
    g_js.usedPassword = !cfg.password.empty();
    std::printf("[mp] connecting to %s:%d as %s\n", cfg.host.c_str(), cfg.port, cfg.username.c_str());
    std::fflush(stdout);
    return 1;
}

}  // namespace

// Join a server from the main menu (the --mp-join dev path now; MP.6's server list later). 1 = the
// connection is under way (failures arrive as the menu status line); 0 = refused (a session or a
// join is already running, or bad input).
extern "C" int eden_mp_join(const char* host, int port, const char* name, int characterType, const char* password) {
    eden_net::MpConfig cfg;
    cfg.host = host ? host : "";
    cfg.port = port > 0 ? port : 27015;
    cfg.username = name && *name ? name : "Emod";
    cfg.characterType = characterType;
    cfg.password = password ? password : "";
    return startJoin(cfg);
}

// MP.6: the Connecting window's view of the join (Classes/MpServers.h).
extern "C" int eden_mp_join_state(EdenMpJoinState* out) {
    if (out) *out = g_js;
    return g_js.stage;
}

extern "C" int eden_mp_join_line(int i, char* buf, int cap) {
    if (i < 0 || i >= (int)g_jsLines.size() || !buf || cap <= 0) return 0;
    std::snprintf(buf, (size_t)cap, "%s", g_jsLines[i].c_str());
    return 1;
}

extern "C" void eden_mp_join_cancel(void) {
    if (g_cur && MpWorld::link() == g_cur) g_cur->cancel();
}

extern "C" void eden_mp_join_ack(void) {
    if (g_js.stage == EDEN_MP_JOIN_FAILED) g_js.stage = EDEN_MP_JOIN_IDLE;
}
