// MpServers_native.mm — MP.6: the saved server list and the probe behind Classes/MpServers.h.
// The model and the probe are port/src/net/ServerList (plain C++); this file owns the one list the
// screens see, a live status per row, and the prober's worker threads (which only make plain data
// on a locked queue, like the session's network thread: rule 8). WORKING/multiplayer-server-list-
// plan-2026-10-17.md §4-§6.
//
// servers.txt lives in the documents root (the plan said the app-data directory beside the prefs;
// the documents root is what --docs= points at, so every harness run gets its own list).
// mp_name / mp_character are NSUserDefaults port settings, so a selftest's ephemeral prefs cover them.
#include "../../../Classes/MpServers.h"
#include "../../../port/src/net/ServerList.h"
#import "../../../port/src/shim/foundation/NSUserDefaults.h"   // the system one on Apple
#import "../../../port/src/shim/foundation/NSString.h"
#import "../../../port/src/shim/foundation/NSNumber.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

extern "C" const char* eden_platform_documents_root(void);

namespace {

using namespace eden_net;

struct RowLive {
    unsigned uid = 0;
    int status = EDEN_MP_ST_CACHED;
    int pingMs = -1;
    bool fresh = false;        // this visit's probe answered with INFO
};

ServerFile g_file;
std::vector<RowLive> g_live;   // parallel to the shown rows
unsigned g_nextUid = 1;
bool g_loaded = false, g_dirty = false;
int g_mock = 0;                // 1 = the fixture, 2 = an empty list (--shot's empty state)
ServerProber* g_prober = nullptr;   // never deleted: a worker may sit in connect() at exit

ServerProber& prober() {
    if (!g_prober) g_prober = new ServerProber();
    return *g_prober;
}

std::string path() {
    const char* d = eden_platform_documents_root();
    return std::string(d ? d : ".") + "/servers.txt";
}

void copyStr(char* dst, size_t cap, const std::string& s) {
    if (!cap) return;
    size_t n = s.size() < cap - 1 ? s.size() : cap - 1;
    while (n > 0 && n < s.size() && ((unsigned char)s[n] & 0xC0) == 0x80) n--;   // never split a char
    std::memcpy(dst, s.data(), n);
    dst[n] = 0;
}

void syncLive() {
    const int n = g_file.count();
    while ((int)g_live.size() < n) { RowLive r; r.uid = g_nextUid++; g_live.push_back(r); }
    if ((int)g_live.size() > n) g_live.resize(n);
}

int indexOfUid(unsigned uid) {
    for (size_t i = 0; i < g_live.size(); ++i) if (g_live[i].uid == uid) return (int)i;
    return -1;
}

// The --mp-mock fixture (UI plan §9): one row per status, every one answered from the table.
void loadMock() {
    g_file = ServerFile();
    struct M { const char* label; const char* host; int port; const char* pw; int st; int ping; const char* name;
               int players, max, locked; const char* flags; const char* motd; };
    static const M kRows[] = {
        {"Castle Build", "castle.example", 27015, "pw", EDEN_MP_ST_ONLINE, 38, "Castle Build", 12, 64, 1, "tf",
         "Welcome! No griefing. /help for commands"},
        {"Friday LAN", "192.168.1.20", 27015, "", EDEN_MP_ST_ONLINE, 2, "Friday LAN", 0, 64, 0, "", ""},
        {"Big Server", "big.example", 27015, "", EDEN_MP_ST_BASIC, 120, "", -1, -1, -1, "", ""},
        {"Packed", "full.example", 27015, "", EDEN_MP_ST_FULL, 64, "", -1, -1, 0, "", ""},
        {"Home box", "home.example", 27016, "", EDEN_MP_ST_NOT_RUNNING, -1, "", -1, -1, -1, "", ""},
        {"old.box", "old.box", 27015, "", EDEN_MP_ST_NO_ANSWER, -1, "", -1, -1, -1, "", ""},
        {"Typo", "castel.example", 27015, "", EDEN_MP_ST_UNKNOWN_HOST, -1, "", -1, -1, -1, "", ""},
    };
    std::vector<std::pair<std::string, ProbeResult>> mock;
    for (const M& m : kRows) {
        ServerEntry e;
        e.label = m.label;
        e.host = m.host;
        e.port = m.port;
        e.password = m.pw;
        e.cache.locked = m.locked;
        if (m.st == EDEN_MP_ST_ONLINE) { e.cache.name = m.name; e.cache.players = m.players; e.cache.max = m.max; }
        g_file.add(e);
        ProbeResult r;
        r.pingMs = m.ping;
        r.status = m.st == EDEN_MP_ST_ONLINE ? ProbeStatus::Online : m.st == EDEN_MP_ST_BASIC ? ProbeStatus::OnlineBasic
                 : m.st == EDEN_MP_ST_FULL ? ProbeStatus::Full : m.st == EDEN_MP_ST_NOT_RUNNING ? ProbeStatus::NotRunning
                 : m.st == EDEN_MP_ST_NO_ANSWER ? ProbeStatus::NoAnswer : ProbeStatus::UnknownHost;
        if (m.st == EDEN_MP_ST_ONLINE) {
            r.hasInfo = true;
            r.info.name = m.name;
            r.info.players = m.players;
            r.info.max = m.max;
            r.info.locked = m.locked;
            r.info.flags = m.flags;
            r.info.motd = m.motd;
        }
        mock.push_back(std::make_pair(std::string(m.host), r));
    }
    prober().setMock(mock);
}

void ensureLoaded() {
    if (g_loaded) return;
    g_loaded = true;
    if (g_mock == 1) loadMock();
    else if (g_mock == 2) g_file = ServerFile();
    else loadServers(path(), g_file);
    g_live.clear();
    syncLive();
}

int statusOf(ProbeStatus s) {
    switch (s) {
        case ProbeStatus::Online: return EDEN_MP_ST_ONLINE;
        case ProbeStatus::OnlineBasic: return EDEN_MP_ST_BASIC;
        case ProbeStatus::Full: return EDEN_MP_ST_FULL;
        case ProbeStatus::NotRunning: return EDEN_MP_ST_NOT_RUNNING;
        case ProbeStatus::NoAnswer: return EDEN_MP_ST_NO_ANSWER;
        case ProbeStatus::UnknownHost: return EDEN_MP_ST_UNKNOWN_HOST;
        default: return EDEN_MP_ST_CACHED;
    }
}

void probeRow(int i) {
    const ServerEntry* e = g_file.at(i);
    if (!e || i >= (int)g_live.size()) return;
    g_live[i].status = EDEN_MP_ST_PROBING;
    prober().request((int)g_live[i].uid, e->host, e->port);
}

// ---- matchmakers (MP.8): the Experiments › Matchmakers addresses and their LIST results ------
struct MmRow { MatchRow r; std::string via; };
std::vector<std::pair<std::string, int>> g_mmAddrs;   // parsed setting (reloaded on open)
std::vector<MmRow> g_mmRows;
std::vector<std::string> g_mmFailed;                  // "host: cause"
int g_mmAsked = 0, g_mmAnswered = 0;
MatchFetcher* g_fetcher = nullptr;                    // never deleted (see ServerList.h)

MatchFetcher& fetcher() {
    if (!g_fetcher) g_fetcher = new MatchFetcher();
    return *g_fetcher;
}

std::string mmSetting() {
    NSString* s = [[NSUserDefaults standardUserDefaults] stringForKey:@"mp_matchmakers"];
    const char* c = s ? [s UTF8String] : nullptr;
    return c ? c : "";
}

void mmReload() {
    g_mmAddrs.clear();
    if (g_mock == 1) { g_mmAddrs = {{"match.example", 27020}, {"lan.example", 27020}}; return; }
    std::string e;
    if (!parseMatchmakers(mmSetting(), g_mmAddrs, &e)) g_mmAddrs.clear();   // a bad hand-edit: off
}

void mmSort() {
    std::stable_sort(g_mmRows.begin(), g_mmRows.end(), [](const MmRow& a, const MmRow& b) {
        if (a.r.players != b.r.players) return a.r.players > b.r.players;
        return a.r.name < b.r.name;
    });
}

void mmAddRows(const std::string& via, const std::vector<MatchRow>& rows) {
    for (const MatchRow& r : rows) {
        bool dup = false;
        for (const MmRow& m : g_mmRows) if (m.r.host == r.host && m.r.port == r.port) { dup = true; break; }
        if (!dup) g_mmRows.push_back({r, via});
    }
    mmSort();
}


}  // namespace

extern "C" {

int eden_mp_servers_load(void) {
    prober().cancel();
    g_loaded = false;
    ensureLoaded();
    return g_file.count();
}

int eden_mp_servers_count(void) { ensureLoaded(); return g_file.count(); }
int eden_mp_servers_cap(void) { return kServerCap; }

int eden_mp_servers_get(int i, EdenMpServerRow* out) {
    ensureLoaded();
    const ServerEntry* e = g_file.at(i);
    if (!e || !out || i >= (int)g_live.size()) return 0;
    std::memset(out, 0, sizeof *out);
    const RowLive& l = g_live[i];
    copyStr(out->label, sizeof out->label, e->label);
    copyStr(out->host, sizeof out->host, e->host);
    out->port = e->port;
    out->hasPassword = !e->password.empty();
    copyStr(out->name, sizeof out->name, e->cache.name);
    copyStr(out->motd, sizeof out->motd, e->cache.motd);
    copyStr(out->flags, sizeof out->flags, e->cache.flags);
    out->players = e->cache.players;
    out->max = e->cache.max;
    out->locked = e->cache.locked;
    out->seen = e->cache.seen;
    out->status = l.status;
    out->pingMs = l.pingMs;
    out->fromCache = !l.fresh;
    // The fallback row has no live count: a cached one from an old INFO would be stale (plan §5).
    if (l.status == EDEN_MP_ST_BASIC || l.status == EDEN_MP_ST_FULL) { out->players = -1; out->motd[0] = 0; }
    return 1;
}

int eden_mp_servers_password(int i, char* buf, int cap) {
    ensureLoaded();
    const ServerEntry* e = g_file.at(i);
    if (buf && cap > 0) buf[0] = 0;
    if (!e || e->password.empty()) return 0;
    if (buf && cap > 0) copyStr(buf, (size_t)cap, e->password);
    return 1;
}

int eden_mp_servers_add(const char* label, const char* host, int port, const char* password) {
    ensureLoaded();
    if (g_file.count() >= kServerCap || !host || !*host) return -1;
    ServerEntry e;
    e.label = label ? label : "";
    e.host = host;
    e.port = port > 0 ? port : kDefaultPort;
    e.password = password ? password : "";
    g_file.add(e);
    syncLive();
    g_dirty = true;
    return g_file.count() - 1;
}

int eden_mp_servers_update(int i, const char* label, const char* host, int port, const char* password) {
    ensureLoaded();
    ServerEntry* e = g_file.at(i);
    if (!e || !host || !*host) return 0;
    const bool moved = e->host != host || e->port != port;
    e->label = label ? label : "";
    e->host = host;
    e->port = port > 0 ? port : kDefaultPort;
    if (password) e->password = password;
    if (moved) {   // a different server: what we knew about the old one no longer applies
        e->cache = ServerInfo();
        g_live[i].status = EDEN_MP_ST_CACHED;
        g_live[i].pingMs = -1;
        g_live[i].fresh = false;
    }
    g_dirty = true;
    return 1;
}

void eden_mp_servers_remove(int i) {
    ensureLoaded();
    if (!g_file.at(i)) return;
    g_file.remove(i);
    g_live.erase(g_live.begin() + i);
    g_dirty = true;
}

int eden_mp_servers_move(int i, int delta) {
    ensureLoaded();
    if (!g_file.move(i, delta)) return 0;
    std::swap(g_live[i], g_live[i + delta]);
    g_dirty = true;
    return 1;
}

void eden_mp_servers_set_locked(int i, int locked) {
    ensureLoaded();
    ServerEntry* e = g_file.at(i);
    if (!e || e->cache.locked == locked) return;
    e->cache.locked = locked;
    g_dirty = true;
}

int eden_mp_servers_find(const char* host, int port, int except) {
    ensureLoaded();
    for (int i = 0; i < g_file.count(); ++i) {
        const ServerEntry* e = g_file.at(i);
        if (i != except && host && e->host == host && e->port == port) return i;
    }
    return -1;
}

int eden_mp_servers_save(void) {
    ensureLoaded();
    g_dirty = false;
    if (g_mock) return 1;   // the fixture never touches the disk
    return saveServers(path(), g_file) ? 1 : 0;
}

void eden_mp_servers_refresh(int i) {
    ensureLoaded();
    if (i >= 0) { probeRow(i); return; }
    for (int k = 0; k < g_file.count(); ++k) probeRow(k);
}

int eden_mp_servers_poll(void) {
    ensureLoaded();
    std::vector<ProbeResult> got;
    if (prober().drain(got)) {
        for (const ProbeResult& r : got) {
            const int i = indexOfUid((unsigned)r.id);
            ServerEntry* e = g_file.at(i);
            if (!e) continue;   // removed while it was probed
            RowLive& l = g_live[i];
            l.status = statusOf(r.status);
            l.pingMs = r.pingMs;
            if (r.hasInfo) {
                l.fresh = true;
                ServerInfo keep = e->cache;
                e->cache = r.info;
                e->cache.extra = keep.extra;
                e->cache.seen = (long long)time(nullptr);
                if (e->label.empty() && !r.info.name.empty()) e->label = r.info.name;   // list plan §6
                g_dirty = true;
            }
        }
    }
    const int pending = prober().pending();
    if (!pending && g_dirty) eden_mp_servers_save();
    return pending;
}

int eden_mp_parse_address(const char* in, char* host, int hostCap, int* port, char* err, int errCap) {
    std::string h, e;
    int p = 0;
    const bool ok = parseAddress(in ? in : "", h, p, &e);
    if (host && hostCap > 0) copyStr(host, (size_t)hostCap, ok ? h : std::string());
    if (port) *port = ok ? p : 0;
    if (err && errCap > 0) copyStr(err, (size_t)errCap, e);
    return ok ? 1 : 0;
}

int eden_mp_name_problem(const char* name, char* err, int errCap) {
    const std::string p = nameProblem(name ? name : "");
    if (err && errCap > 0) copyStr(err, (size_t)errCap, p);
    return p.empty() ? 0 : 1;
}

int eden_mp_profile_name(char* buf, int cap) {
    NSString* s = [[NSUserDefaults standardUserDefaults] stringForKey:@"mp_name"];
    const char* c = s ? [s UTF8String] : nullptr;
    if (buf && cap > 0) copyStr(buf, (size_t)cap, c ? c : "");
    return c && *c ? 1 : 0;
}

int eden_mp_profile_character(void) {
    id v = [[NSUserDefaults standardUserDefaults] objectForKey:@"mp_character"];
    const int t = v ? [(NSNumber*)v intValue] : 0;
    return t < 0 ? 0 : (t > 6 ? 6 : t);
}

void eden_mp_profile_set(const char* name, int character) {
    NSUserDefaults* d = [NSUserDefaults standardUserDefaults];
    NSString* s = [NSString stringWithUTF8String:name ? name : ""];
    [d setObject:(s ? s : @"") forKey:@"mp_name"];
    [d setObject:[NSNumber numberWithInt:character < 0 ? 0 : (character > 6 ? 6 : character)] forKey:@"mp_character"];
    [d synchronize];
}

// ---- matchmakers (MP.8) ----
void eden_mp_matchmakers_reload(void) { mmReload(); }
int eden_mp_matchmaker_count(void) { return (int)g_mmAddrs.size(); }

void eden_mp_matchmakers_fetch(void) {
    fetcher().cancel();
    g_mmRows.clear();
    g_mmFailed.clear();
    g_mmAnswered = 0;
    g_mmAsked = (int)g_mmAddrs.size();
    if (g_mock == 1) {   // canned, no sockets
        static const struct { const char* n; const char* h; int p; int l; int pl; } k[] = {
            {"Sky Kingdom", "203.0.113.5", 27015, 0, 9}, {"Moonlit Builders", "203.0.113.9", 27016, 1, 4},
            {"Survival Island", "198.51.100.2", 27015, 0, 2}, {"Empty Realm", "198.51.100.7", 27015, 0, 0},
        };
        std::vector<MatchRow> rows;
        for (const auto& m : k) { MatchRow r; r.name = m.n; r.host = m.h; r.port = m.p; r.locked = m.l; r.players = m.pl; rows.push_back(r); }
        mmAddRows("match.example", rows);
        g_mmAnswered = g_mmAsked;
        return;
    }
    for (int i = 0; i < g_mmAsked; ++i) fetcher().request(i, g_mmAddrs[i].first, g_mmAddrs[i].second);
}

int eden_mp_matchmakers_poll(void) {
    std::vector<MatchResult> got;
    if (fetcher().drain(got)) {
        for (const MatchResult& r : got) {
            if (r.id < 0 || r.id >= (int)g_mmAddrs.size()) continue;
            char addr[300];
            std::snprintf(addr, sizeof addr, "%s:%d", g_mmAddrs[r.id].first.c_str(), g_mmAddrs[r.id].second);
            if (r.ok || !r.rows.empty()) { ++g_mmAnswered; mmAddRows(addr, r.rows); }
            else g_mmFailed.push_back(std::string(addr) + ": " + r.err);
        }
    }
    return fetcher().pending();
}

int eden_mp_matchmaker_rows(void) { return (int)g_mmRows.size(); }

int eden_mp_matchmaker_get(int i, EdenMpServerRow* out) {
    if (i < 0 || i >= (int)g_mmRows.size() || !out) return 0;
    const MmRow& m = g_mmRows[i];
    std::memset(out, 0, sizeof *out);
    copyStr(out->label, sizeof out->label, m.r.name);
    copyStr(out->name, sizeof out->name, m.r.name);
    copyStr(out->host, sizeof out->host, m.r.host);
    copyStr(out->motd, sizeof out->motd, "via " + m.via);
    out->port = m.r.port;
    out->players = m.r.players;
    out->max = -1;
    out->locked = m.r.locked;
    out->status = EDEN_MP_ST_BASIC;
    out->pingMs = -1;
    return 1;
}

// "" when every matchmaker answered, else a one-line summary for the empty state / status.
int eden_mp_matchmaker_error(char* buf, int cap) {
    std::string s;
    if (!g_mmFailed.empty()) {
        char b[64];
        std::snprintf(b, sizeof b, "%d of %d matchmakers did not answer", (int)g_mmFailed.size(), g_mmAsked);
        s = b;
    }
    if (buf && cap > 0) copyStr(buf, (size_t)cap, s);
    return s.empty() ? 0 : 1;
}

// Copies a listed server into servers.txt (label = its name). The new row's index; the existing
// row's index for a duplicate; -1 when the list is full.
int eden_mp_matchmaker_save(int i) {
    EdenMpServerRow r;
    if (!eden_mp_matchmaker_get(i, &r)) return -1;
    const int dup = eden_mp_servers_find(r.host, r.port, -1);
    if (dup >= 0) return dup;
    const int idx = eden_mp_servers_add(r.label, r.host, r.port, "");
    if (idx < 0) return -1;
    eden_mp_servers_set_locked(idx, r.locked);
    eden_mp_servers_save();
    eden_mp_servers_refresh(idx);
    return idx;
}

// Experiments › Matchmakers' Save: "" in `err` and the cleaned text in `clean` when valid.
int eden_mp_matchmakers_validate(const char* text, char* clean, int cleanCap, char* err, int errCap) {
    std::vector<std::pair<std::string, int>> a;
    std::string e;
    const bool ok = parseMatchmakers(text ? text : "", a, &e);
    std::string c;
    if (ok) for (const auto& p : a) { if (!c.empty()) c += ", "; c += p.first + ":" + std::to_string(p.second); }
    if (clean && cleanCap > 0) copyStr(clean, (size_t)cleanCap, c);
    if (err && errCap > 0) copyStr(err, (size_t)errCap, e);
    return ok ? 1 : 0;
}

void eden_mp_servers_mock(int on) {
    g_mock = on == 1 || on == 2 ? on : 0;
    g_loaded = false;
}

}  // extern "C"
