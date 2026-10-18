// ServerList.h — MP.6: the saved server list (`servers.txt`) and the server probe
// (WORKING/multiplayer-server-list-plan-2026-10-17.md §4–§6). Plain C++, no engine includes; the
// UI in Classes/ reaches it through the C seam in native/src/seam/MpServers_native.mm.
//
// The probe never sends JOIN: one TCP connect (its time is the ping), `INFO\n`, one line read for
// 1.5 s, close. Official servers and ewb-server builds before MP.9 drop pre-JOIN lines, so they
// answer nothing and get the fallback row ("Online, basic").
#pragma once
#include <cstdint>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace eden_net {

// What an `INFO` reply (or the cache in servers.txt) says. -1 = unknown.
struct ServerInfo {
    std::string name, motd, flags;
    int players = -1, max = -1;
    int locked = -1;           // -1 "?", 0 open, 1 password
    int64_t seen = 0;          // unix seconds of the last good probe (cache only)
    std::vector<std::pair<std::string, std::string>> extra;   // unknown cache keys, kept on rewrite
};

struct ServerEntry {
    std::string label, host, password;
    int port = 27015;
    ServerInfo cache;
};

// One line of the file: a server, or a line this build cannot parse (kept verbatim, not shown).
struct ServerLine {
    bool parsed = false;
    ServerEntry e;
    std::string raw;
};

struct ServerFile {
    std::vector<ServerLine> lines;
    int version = 1;
    // Index into `lines` of the n-th shown server, or -1.
    int lineOf(int shown) const;
    int count() const;
    ServerEntry* at(int shown);
    const ServerEntry* at(int shown) const;
    void add(const ServerEntry& e);
    void remove(int shown);
    bool move(int shown, int delta);   // swaps with the neighbouring shown server
};

const int kServerCap = 100;
const int kDefaultPort = 27015;

// "host", "host:port", "[v6]:port" -> host + port (default 27015). `err` names the first problem
// ("Enter an address", "No spaces in an address", "Port must be 1-65535", "Address too long").
bool parseAddress(const std::string& in, std::string& host, int& port, std::string* err);
// The protocol's name rules: 1-20 printable ASCII, no ':' '[' ']', no edge spaces, not "server".
// "" when valid, else the first failing rule as a short sentence.
std::string nameProblem(const std::string& name);

std::string formatServers(const ServerFile& f);
bool parseServers(const std::string& text, ServerFile& f);   // false = no header (file ignored)
bool loadServers(const std::string& path, ServerFile& f);    // false = missing/unreadable (f empty)
bool saveServers(const std::string& path, const ServerFile& f);   // temp + rename

// `INFO:k=v:...:motd=...` (the reply line). False if it is not an INFO line.
bool parseInfoLine(const std::string& line, ServerInfo& out);

enum class ProbeStatus { None, Probing, Online, OnlineBasic, Full, NotRunning, NoAnswer, UnknownHost };

struct ProbeResult {
    int id = 0;
    ProbeStatus status = ProbeStatus::None;
    int pingMs = -1;
    bool hasInfo = false;
    ServerInfo info;
};

// Blocking: one probe (≤ ~4.5 s plus DNS). Runs on a prober thread, never on the main thread.
ProbeResult probeServer(const std::string& host, int port, int connectTimeoutMs = 3000, int readMs = 1500);

// At most 8 probes in flight on worker threads (list plan §4); results are plain data on a locked
// queue for the main thread. `cancel()` drops everything queued and every result still to come.
class ServerProber {
public:
    ServerProber() = default;
    ~ServerProber();
    ServerProber(const ServerProber&) = delete;
    ServerProber& operator=(const ServerProber&) = delete;

    void request(int id, const std::string& host, int port);
    void cancel();
    bool drain(std::vector<ProbeResult>& out);
    int pending() const;   // queued + running, this generation

    // Tests: a fixed result per host instead of a socket (the --mp-mock fixture).
    void setMock(std::vector<std::pair<std::string, ProbeResult>> m);

private:
    struct Job { int id; std::string host; int port; unsigned gen; };
    void worker();

    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Job> q_;
    std::vector<ProbeResult> done_;
    std::vector<std::thread> th_;
    std::vector<std::pair<std::string, ProbeResult>> mock_;
    unsigned gen_ = 0;
    int running_ = 0;
    bool stop_ = false;
};

// ---- matchmakers (MP.8) ---------------------------------------------------------------------
// A matchmaker (`edenmatch`, matchmaker.md) answers `LIST` with `SERVER:<name>:<ip>:<port>:<locked>
// [:<players>[:<flag6>]]` lines then `END`. One-shot: connect, ask, read, close.
struct MatchRow {
    std::string name, host;
    int port = 0, locked = -1, players = -1;
};

struct MatchResult {
    int id = 0;
    bool ok = false;               // got `END` (or a clean close) rather than failing
    std::string err;               // short cause when !ok
    std::vector<MatchRow> rows;
};

// One `SERVER:` line -> a row. False for anything else (the 4-, 6- and 7-field forms all parse).
bool parseMatchLine(const std::string& line, MatchRow& out);
// Blocking: ≤ ~3 s connect + ≤ 5 s reading. Runs on a fetch thread, never the main thread.
MatchResult fetchMatchList(const std::string& host, int port);

// One short-lived thread per request (≤ 8 addresses by the setting's cap); results are plain data on
// a locked queue. `cancel()` drops every result still to come.
class MatchFetcher {
public:
    MatchFetcher();
    void request(int id, const std::string& host, int port);
    void cancel();
    bool drain(std::vector<MatchResult>& out);
    int pending() const;
    void setMock(bool on);   // tests: instant canned results instead of sockets
private:
    struct State;
    State* s_;               // never freed: a thread may outlive the owner (process exit)
};

// The Experiments › Matchmakers string: comma-separated `host[:port]`, default port 27020, ≤ 8
// addresses, ≤ 512 bytes. Fills `out` with the cleaned entries; on a problem returns false with
// `err` naming it (the whole save is rejected).
const int kMatchDefaultPort = 27020;
bool parseMatchmakers(const std::string& text, std::vector<std::pair<std::string, int>>& out, std::string* err);

}  // namespace eden_net
