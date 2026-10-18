// ServerList.cpp — see ServerList.h.
#include "ServerList.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "NetFramer.h"
#include "NetSock.h"

namespace eden_net {

// ---- the list ----------------------------------------------------------------------------

int ServerFile::lineOf(int shown) const {
    if (shown < 0) return -1;
    for (size_t i = 0; i < lines.size(); ++i)
        if (lines[i].parsed && shown-- == 0) return (int)i;
    return -1;
}
int ServerFile::count() const {
    int n = 0;
    for (const auto& l : lines) n += l.parsed;
    return n;
}
ServerEntry* ServerFile::at(int shown) { const int i = lineOf(shown); return i < 0 ? nullptr : &lines[i].e; }
const ServerEntry* ServerFile::at(int shown) const { const int i = lineOf(shown); return i < 0 ? nullptr : &lines[i].e; }
void ServerFile::add(const ServerEntry& e) {
    ServerLine l;
    l.parsed = true;
    l.e = e;
    lines.push_back(l);
}
void ServerFile::remove(int shown) {
    const int i = lineOf(shown);
    if (i >= 0) lines.erase(lines.begin() + i);
}
bool ServerFile::move(int shown, int delta) {
    const int a = lineOf(shown), b = lineOf(shown + delta);
    if (a < 0 || b < 0 || delta == 0) return false;
    std::swap(lines[a], lines[b]);   // unparsed lines between them keep their places
    return true;
}

// ---- validation --------------------------------------------------------------------------

bool parseAddress(const std::string& in0, std::string& host, int& port, std::string* err) {
    auto fail = [&](const char* m) { if (err) *err = m; return false; };
    std::string in = in0;
    const size_t a = in.find_first_not_of(" \t"), b = in.find_last_not_of(" \t");
    in = a == std::string::npos ? std::string() : in.substr(a, b - a + 1);
    if (in.empty()) return fail("Enter an address");
    if (in.find_first_of(" \t") != std::string::npos) return fail("No spaces in an address");
    for (unsigned char c : in) if (c < 0x21 || c == 0x7f) return fail("No spaces in an address");
    std::string h = in, p;
    if (in[0] == '[') {   // [v6]:port
        const size_t close = in.find(']');
        if (close == std::string::npos) return fail("Missing ] in the address");
        h = in.substr(1, close - 1);
        if (close + 1 < in.size()) {
            if (in[close + 1] != ':') return fail("Port must be 1-65535");
            p = in.substr(close + 2);
            if (p.empty()) return fail("Port must be 1-65535");
        }
    } else {
        const size_t c = in.rfind(':');
        if (c != std::string::npos) {
            if (in.find(':') != c) return fail("Put an IPv6 address in [ ]");
            h = in.substr(0, c);
            p = in.substr(c + 1);
            if (p.empty()) return fail("Port must be 1-65535");
        }
    }
    if (h.empty()) return fail("Enter an address");
    if (h.size() > 253) return fail("Address too long");
    int n = kDefaultPort;
    if (!p.empty()) {
        if (p.size() > 5) return fail("Port must be 1-65535");
        n = 0;
        for (char c : p) {
            if (c < '0' || c > '9') return fail("Port must be 1-65535");
            n = n * 10 + (c - '0');
        }
        if (n < 1 || n > 65535) return fail("Port must be 1-65535");
    }
    host = h;
    port = n;
    if (err) err->clear();
    return true;
}

std::string nameProblem(const std::string& n) {
    if (n.empty()) return "Enter a name";
    if (n.size() > 20) return "At most 20 letters";
    for (unsigned char c : n) {
        if (c < 0x20 || c > 0x7e) return "Letters, digits and plain symbols only";
        if (c == ':' || c == '[' || c == ']') return "No : [ or ]";
    }
    if (n.front() == ' ' || n.back() == ' ') return "No space at either end";
    std::string low = n;
    for (char& c : low) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    if (low == "server") return "\"server\" is reserved";
    return std::string();
}

// ---- servers.txt -------------------------------------------------------------------------

namespace {

const char kHeader[] = "emod-servers";

// A field never holds a tab or a line break (the file's separators).
std::string flat(const std::string& s) {
    std::string o;
    for (char c : s) o += (c == '\t' || c == '\n' || c == '\r') ? ' ' : c;
    return o;
}

// Cache values escape the cache's own separators.
std::string esc(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (c == '%' || c == ';' || c == '=' || c == '\t' || c == '\n' || c == '\r') {
            char b[4];
            std::snprintf(b, sizeof b, "%%%02X", c);
            o += b;
        } else o += (char)c;
    }
    return o;
}
std::string unesc(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            char h[3] = {s[i + 1], s[i + 2], 0};
            char* end = nullptr;
            const long v = std::strtol(h, &end, 16);
            if (end == h + 2) { o += (char)v; i += 2; continue; }
        }
        o += s[i];
    }
    return o;
}

bool parseInt(const std::string& s, int& out) {
    if (s.empty() || s.size() > 10) return false;
    char* end = nullptr;
    const long v = std::strtol(s.c_str(), &end, 10);
    if (*end) return false;
    out = (int)v;
    return true;
}

void splitTabs(const std::string& l, std::vector<std::string>& f) {
    f.clear();
    size_t s = 0;
    for (;;) {
        const size_t t = l.find('\t', s);
        f.push_back(l.substr(s, t == std::string::npos ? std::string::npos : t - s));
        if (t == std::string::npos) break;
        s = t + 1;
    }
}

std::string formatCache(const ServerInfo& c) {
    std::string o;
    auto put = [&](const char* k, const std::string& v) {
        if (!o.empty()) o += ';';
        o += k;
        o += '=';
        o += esc(v);
    };
    if (!c.name.empty()) put("name", c.name);
    if (c.players >= 0) put("players", std::to_string(c.players));
    if (c.max >= 0) put("max", std::to_string(c.max));
    put("locked", c.locked < 0 ? "?" : std::to_string(c.locked));
    if (!c.flags.empty()) put("flags", c.flags);
    if (!c.motd.empty()) put("motd", c.motd);
    if (c.seen > 0) put("seen", std::to_string((long long)c.seen));
    for (const auto& kv : c.extra) put(kv.first.c_str(), kv.second);
    return o;
}

void parseCache(const std::string& s, ServerInfo& c) {
    size_t p = 0;
    while (p < s.size()) {
        size_t e = s.find(';', p);
        if (e == std::string::npos) e = s.size();
        const std::string kv = s.substr(p, e - p);
        p = e + 1;
        const size_t eq = kv.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        const std::string k = kv.substr(0, eq), v = unesc(kv.substr(eq + 1));
        int n = 0;
        if (k == "name") c.name = v;
        else if (k == "players") { if (parseInt(v, n)) c.players = n; }
        else if (k == "max") { if (parseInt(v, n)) c.max = n; }
        else if (k == "locked") c.locked = v == "1" ? 1 : (v == "0" ? 0 : -1);
        else if (k == "flags") c.flags = v;
        else if (k == "motd") c.motd = v;
        else if (k == "seen") c.seen = std::atoll(v.c_str());
        else c.extra.push_back(std::make_pair(k, v));
    }
}

}  // namespace

std::string formatServers(const ServerFile& f) {
    std::string o = std::string(kHeader) + " " + std::to_string(f.version < 1 ? 1 : f.version) + "\n";
    for (const auto& l : f.lines) {
        if (!l.parsed) { o += l.raw + "\n"; continue; }
        const ServerEntry& e = l.e;
        o += flat(e.label) + "\t" + flat(e.host) + "\t" + std::to_string(e.port) + "\t" + flat(e.password) + "\t" +
             formatCache(e.cache) + "\n";
    }
    return o;
}

bool parseServers(const std::string& text, ServerFile& f) {
    f = ServerFile();
    size_t p = 0;
    bool header = false;
    std::vector<std::string> fields;
    while (p <= text.size()) {
        size_t e = text.find('\n', p);
        if (e == std::string::npos) e = text.size();
        std::string l = text.substr(p, e - p);
        p = e + 1;
        if (!l.empty() && l.back() == '\r') l.pop_back();
        if (!header) {
            if (l.empty()) continue;
            if (l.compare(0, sizeof kHeader - 1, kHeader) != 0) return false;
            int v = 1;
            if (l.size() > sizeof kHeader && parseInt(l.substr(sizeof kHeader), v) && v > 0) f.version = v;
            header = true;
            continue;
        }
        if (l.empty()) { if (p > text.size()) break; continue; }
        ServerLine sl;
        sl.raw = l;
        splitTabs(l, fields);
        int port = 0;
        if (fields.size() >= 3 && !fields[1].empty() && parseInt(fields[2], port) && port >= 1 && port <= 65535 &&
            fields[1].find(' ') == std::string::npos) {
            sl.parsed = true;
            sl.e.label = fields[0];
            sl.e.host = fields[1];
            sl.e.port = port;
            if (fields.size() >= 4) sl.e.password = fields[3];
            if (fields.size() >= 5) parseCache(fields[4], sl.e.cache);
            sl.raw.clear();
        }
        f.lines.push_back(sl);
    }
    return header;
}

bool loadServers(const std::string& path, ServerFile& f) {
    f = ServerFile();
    FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) return false;
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, fp)) > 0 && text.size() < (size_t(1) << 20)) text.append(buf, n);
    std::fclose(fp);
    if (!parseServers(text, f)) { f = ServerFile(); return false; }
    return true;
}

bool saveServers(const std::string& path, const ServerFile& f) {
    const std::string tmp = path + ".tmp";
    FILE* fp = std::fopen(tmp.c_str(), "wb");
    if (!fp) return false;
    const std::string o = formatServers(f);
    const bool ok = std::fwrite(o.data(), 1, o.size(), fp) == o.size();
    if (std::fclose(fp) != 0 || !ok) { std::remove(tmp.c_str()); return false; }
#if defined(_WIN32)
    std::remove(path.c_str());   // Windows' rename fails onto an existing file
#endif
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}

// ---- INFO --------------------------------------------------------------------------------

namespace {

// Remote text: no control bytes, no broken UTF-8, at most `cap` bytes (never splitting a char).
std::string cleanRemote(const std::string& s, size_t cap) {
    std::string o;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = (unsigned char)s[i];
        int len = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
        if (!len || i + len > s.size()) { ++i; continue; }
        bool ok = true;
        for (int k = 1; k < len; ++k) ok = ok && ((unsigned char)s[i + k] & 0xC0) == 0x80;
        if (!ok) { ++i; continue; }
        if (len == 1 && (c < 0x20 || c == 0x7f)) { ++i; continue; }
        if (o.size() + len > cap) break;
        o.append(s, i, len);
        i += len;
    }
    return o;
}

}  // namespace

bool parseInfoLine(const std::string& line, ServerInfo& out) {
    if (line.compare(0, 5, "INFO:") != 0) return false;
    ServerInfo r;
    size_t p = 5;
    while (p < line.size()) {
        if (line.compare(p, 5, "motd=") == 0) { r.motd = cleanRemote(line.substr(p + 5), 128); break; }
        size_t e = line.find(':', p);
        if (e == std::string::npos) e = line.size();
        const std::string kv = line.substr(p, e - p);
        p = e + 1;
        const size_t eq = kv.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
        int n = 0;
        if (k == "name") r.name = cleanRemote(v, 48);
        else if (k == "players") { if (parseInt(v, n) && n >= 0) r.players = n; }
        else if (k == "max") { if (parseInt(v, n) && n >= 0) r.max = n; }
        else if (k == "locked") r.locked = v == "1" ? 1 : 0;
        else if (k == "flags") r.flags = cleanRemote(v, 16);
    }
    out = r;
    return true;
}

// ---- the probe ---------------------------------------------------------------------------

ProbeResult probeServer(const std::string& host, int port, int connectTimeoutMs, int readMs) {
    ProbeResult r;
    Sock s;
    int ms = 0;
    switch (tcpConnect(host.c_str(), port, connectTimeoutMs, s, &ms)) {
        case CONN_OK: break;
        case CONN_DNS: r.status = ProbeStatus::UnknownHost; return r;
        case CONN_REFUSED: r.status = ProbeStatus::NotRunning; return r;
        default: r.status = ProbeStatus::NoAnswer; return r;
    }
    r.pingMs = ms;
    r.status = ProbeStatus::OnlineBasic;
    static const char kInfo[] = "INFO\n";
    sendAll(s, kInfo, sizeof kInfo - 1, 500);
    LineFramer fr;
    std::string line;
    char buf[2048];
    const int64_t t0 = nowMs();
    bool got = false;
    while (!got && nowMs() - t0 < readMs) {
        if (fr.next(line)) { got = true; break; }
        if (!waitReadable(s, 20)) continue;
        const int n = recvSome(s, buf, sizeof buf);
        if (n > 0) fr.feed(buf, (size_t)n);
        else if (n == 0 || n == IO_ERROR) { got = fr.next(line); break; }
        if (fr.overflowed()) break;
    }
    closeSock(s);
    if (got) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line == "[Server] Server full.") r.status = ProbeStatus::Full;
        else if (parseInfoLine(line, r.info)) { r.status = ProbeStatus::Online; r.hasInfo = true; }
    }
    return r;
}

ServerProber::~ServerProber() {
    {
        std::lock_guard<std::mutex> l(mu_);
        stop_ = true;
        q_.clear();
    }
    cv_.notify_all();
    for (auto& t : th_) if (t.joinable()) t.join();
}

void ServerProber::request(int id, const std::string& host, int port) {
    std::lock_guard<std::mutex> l(mu_);
    q_.push_back(Job{id, host, port, gen_});
    if (th_.size() < 8 && (int)th_.size() < running_ + (int)q_.size()) th_.emplace_back([this] { worker(); });
    cv_.notify_one();
}

void ServerProber::cancel() {
    std::lock_guard<std::mutex> l(mu_);
    ++gen_;
    q_.clear();
    done_.clear();
}

bool ServerProber::drain(std::vector<ProbeResult>& out) {
    std::lock_guard<std::mutex> l(mu_);
    if (done_.empty()) return false;
    out.insert(out.end(), done_.begin(), done_.end());
    done_.clear();
    return true;
}

int ServerProber::pending() const {
    std::lock_guard<std::mutex> l(mu_);
    return (int)q_.size() + running_;
}

void ServerProber::setMock(std::vector<std::pair<std::string, ProbeResult>> m) {
    std::lock_guard<std::mutex> l(mu_);
    mock_ = std::move(m);
}

void ServerProber::worker() {
    std::unique_lock<std::mutex> l(mu_);
    for (;;) {
        cv_.wait(l, [this] { return stop_ || !q_.empty(); });
        if (stop_) return;
        Job j = q_.front();
        q_.pop_front();
        ++running_;
        bool mocked = false;
        ProbeResult r;
        for (const auto& m : mock_)
            if (m.first == j.host) { r = m.second; mocked = true; break; }
        l.unlock();
        if (!mocked) r = probeServer(j.host, j.port);
        else std::this_thread::sleep_for(std::chrono::milliseconds(20));
        r.id = j.id;
        l.lock();
        --running_;
        if (j.gen == gen_ && !stop_) done_.push_back(r);
    }
}

// ---- matchmakers (MP.8) ---------------------------------------------------------------------

bool parseMatchLine(const std::string& line, MatchRow& out) {
    static const char kTag[] = "SERVER:";
    if (line.compare(0, sizeof kTag - 1, kTag) != 0) return false;
    std::vector<std::string> f;
    size_t a = sizeof kTag - 1;
    for (;;) {
        const size_t b = line.find(':', a);
        if (b == std::string::npos) { f.push_back(line.substr(a)); break; }
        f.push_back(line.substr(a, b - a));
        a = b + 1;
    }
    if (f.size() < 4) return false;
    out = MatchRow();
    out.name = f[0];
    out.host = f[1];
    out.port = std::atoi(f[2].c_str());
    if (out.host.empty() || out.port < 1 || out.port > 65535) return false;
    out.locked = f[3] == "1" ? 1 : f[3] == "0" ? 0 : -1;
    if (f.size() > 4) { const int p = std::atoi(f[4].c_str()); out.players = p < 0 ? -1 : p; }
    return true;
}

MatchResult fetchMatchList(const std::string& host, int port) {
    MatchResult r;
    Sock s;
    switch (tcpConnect(host.c_str(), port, 3000, s, nullptr)) {
        case CONN_OK: break;
        case CONN_DNS: r.err = "unknown host"; return r;
        case CONN_REFUSED: r.err = "not running"; return r;
        default: r.err = "no answer"; return r;
    }
    static const char kList[] = "LIST\n";
    sendAll(s, kList, sizeof kList - 1, 1000);
    LineFramer fr;
    std::string line;
    char buf[2048];
    const int64_t t0 = nowMs();
    bool done = false;
    while (!done && nowMs() - t0 < 5000 && r.rows.size() < 512) {
        while (fr.next(line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line == "END") { r.ok = true; done = true; break; }
            MatchRow row;
            if (parseMatchLine(line, row)) r.rows.push_back(row);
        }
        if (done) break;
        if (!waitReadable(s, 20)) continue;
        const int n = recvSome(s, buf, sizeof buf);
        if (n > 0) fr.feed(buf, (size_t)n);
        else if (n == 0 || n == IO_ERROR) break;
        if (fr.overflowed()) break;
    }
    closeSock(s);
    if (!r.ok) r.err = r.rows.empty() ? "no list" : "list cut short";
    return r;
}

struct MatchFetcher::State {
    std::mutex mu;
    std::vector<MatchResult> done;
    unsigned gen = 0;
    int running = 0;
    bool mock = false;
};

MatchFetcher::MatchFetcher() : s_(new State) {}

void MatchFetcher::request(int id, const std::string& host, int port) {
    State* st = s_;
    unsigned gen;
    bool mock;
    {
        std::lock_guard<std::mutex> l(st->mu);
        gen = st->gen;
        mock = st->mock;
        ++st->running;
    }
    std::thread([st, id, host, port, gen, mock] {
        MatchResult r;
        if (!mock) r = fetchMatchList(host, port);
        else r.err = "mock";
        r.id = id;
        std::lock_guard<std::mutex> l(st->mu);
        --st->running;
        if (gen == st->gen) st->done.push_back(r);
    }).detach();
}

void MatchFetcher::cancel() {
    std::lock_guard<std::mutex> l(s_->mu);
    ++s_->gen;
    s_->done.clear();
}

bool MatchFetcher::drain(std::vector<MatchResult>& out) {
    std::lock_guard<std::mutex> l(s_->mu);
    if (s_->done.empty()) return false;
    out.insert(out.end(), s_->done.begin(), s_->done.end());
    s_->done.clear();
    return true;
}

int MatchFetcher::pending() const {
    std::lock_guard<std::mutex> l(s_->mu);
    return s_->running;
}

void MatchFetcher::setMock(bool on) {
    std::lock_guard<std::mutex> l(s_->mu);
    s_->mock = on;
}

bool parseMatchmakers(const std::string& text, std::vector<std::pair<std::string, int>>& out, std::string* err) {
    out.clear();
    auto fail = [&](const std::string& m) { if (err) *err = m; return false; };
    if (text.size() > 512) return fail("Too long (512 characters at most)");
    size_t a = 0;
    std::string bad;
    while (a <= text.size()) {
        size_t b = text.find(',', a);
        if (b == std::string::npos) b = text.size();
        std::string item = text.substr(a, b - a);
        const size_t f = item.find_first_not_of(" \t"), l = item.find_last_not_of(" \t");
        item = f == std::string::npos ? std::string() : item.substr(f, l - f + 1);
        a = b + 1;
        if (item.empty()) continue;
        std::string h;
        int p = kMatchDefaultPort;
        std::string e;
        const size_t rb = item.rfind(']');
        const bool hasPort = item.find(':', rb == std::string::npos ? 0 : rb) != std::string::npos;
        if (!parseAddress(item, h, p, &e)) { bad += (bad.empty() ? "" : ", ") + item; continue; }
        if (!hasPort) p = kMatchDefaultPort;   // the server list's default (27015) is not a matchmaker's
        out.push_back({h, p});
    }
    if (!bad.empty()) return fail("Not a valid address: " + bad);
    if (out.size() > 8) return fail("At most 8 addresses");
    return true;
}

}  // namespace eden_net
