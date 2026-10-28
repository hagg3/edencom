// G.4: split out of the old eden_main_native.cpp (see harness.h). Mode group "mp".
#include "harness.h"

using namespace harness;

namespace harness {
namespace g_mp {


// ---------------------------------------------------------------------------------------------
// --mp-replay=FILE  (MP.3a: the session world offline; engine plan §15 legs 1, 2, 5)
// ---------------------------------------------------------------------------------------------
// FILE is a server's line stream as one client received it (port/tools/mp-replay.py record, against a
// local edenserver); FILE's `.expect` holds what mp-replay.py computes from the same lines with its own
// decoder (base64 + raw inflate + the burst merge) over flat. The lines go through MpSession's own
// parser (feedLine, no socket), the world events into the overlay before the window loads (§7 step 2),
// then the session world is opened through the menu as a join would.
namespace {
struct MpReplayData {
    bool haveSpawn = false;
    float spawn[3] = {65536.5f, 34.0f, 65536.5f};
    std::vector<eden_net::MpWorldEvent> world;
    int lines = 0;
};
static_assert(sizeof(MpRec) == sizeof(eden_net::SnapzRec), "MpRec is SnapzRec's layout");
extern "C" void eden_set_fire_melts_ice(int on);   // Classes/Terrain.mm (H.8)

bool mp_replay_load(const std::string& path, MpReplayData& r) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { std::fprintf(stderr, "[eden-mp-replay] cannot open %s\n", path.c_str()); return false; }
    eden_net::MpSession s;
    std::string line;
    for (int ch; (ch = std::fgetc(f)) != EOF;) {
        if (ch == '\n') {
            if (!line.empty() && line.back() == '\r') line.pop_back();   // a CRLF checkout (Windows)
            s.feedLine(line);
            r.lines++;
            line.clear();
        } else {
            line += (char)ch;
        }
    }
    std::fclose(f);
    std::vector<eden_net::MpSideEvent> side;
    s.drainSide(side);
    for (const auto& e : side) {
        if (e.kind == eden_net::MpSideEvent::Error) {
            std::fprintf(stderr, "[eden-mp-replay] the replay does not parse: %s\n", e.text.c_str());
            return false;
        }
        if (e.kind == eden_net::MpSideEvent::Spawn) {
            r.haveSpawn = true;
            for (int i = 0; i < 3; ++i) r.spawn[i] = (float)e.v[i];
        }
    }
    s.drainWorld(r.world);
    return true;
}

// beginSession, the replay's world events into the overlay, then the menu's load, as a join's
// Syncing -> Loading steps (§7) would do it.
bool mp_replay_open(const MpReplayData& r, const float at[3]) {
    MpWorld::beginSession("replay", 0, "Replay");
    for (const auto& e : r.world) {
        if (e.kind == eden_net::MpWorldEvent::Snapz)
            MpWorld::mergeSnapz(reinterpret_cast<const MpRec*>(e.recs.data()), e.recs.size());
        else if (e.kind == eden_net::MpWorldEvent::SignP)
            MpWorld::applySign(e.x, e.y, e.z, e.sa, e.sb, e.sc, e.text.c_str());
        else
            MpWorld::applyAction(e.who.c_str(), e.x, e.y, e.z, e.mode, e.extra);
    }
    MpWorld::setSpawn(at[0], at[1], at[2]);
    if (!MpWorld::openSession()) { std::fprintf(stderr, "[eden-mp-replay] openSession refused\n"); return false; }
    return tick_until([] { return game_mode() == 1; }, 60000, "the session world to load");
}

// MP.3b: a socket-free MpWorld::Link. The pacer's clock is simulated (17 ms a frame, advanced in pump),
// so every wait is a frame count and the run is deterministic. A REGION it was told the answer to is
// answered on the next frame (one frame of "latency"), anything else is never answered.
int64_t g_mpFakeMs = 0;
int64_t mp_fake_clock() { return g_mpFakeMs; }
long long mp_chat_clock() { return (long long)g_mpFakeMs; }
inline int mp_lattice(float p) { return 128 * (int)std::floor(p / 128.0f + 0.5f); }

struct ReplayLink : MpWorld::Link {
    std::map<std::pair<int, int>, std::vector<std::vector<eden_net::SnapzRec> > > answers;
    std::vector<std::pair<int, int> > pending;
    std::vector<std::string> sent;
    std::function<void(int, int)> onAsk;   // the leg's hook, right after a REGION line goes out
    bool joinFlow = false;                 // open the session once joinReady(), as MpLink_native's does
    bool released = false;
    void pump() override {
        g_mpFakeMs += 17;
        for (const auto& p : pending) {
            auto it = answers.find(p);
            if (it == answers.end()) continue;
            for (const auto& f : it->second) MpWorld::enqueueSnapz(reinterpret_cast<const MpRec*>(f.data()), f.size());
        }
        pending.clear();
        if (joinFlow && MpWorld::joinReady() && MpWorld::openSession()) joinFlow = false;
    }
    void sendLine(const char* l) override {
        sent.push_back(l);
        int x = 0, z = 0;
        if (std::sscanf(l, "REGION:%d:%d", &x, &z) == 2) {
            pending.push_back(std::make_pair(x, z));
            if (onAsk) onAsk(x, z);
        }
    }
    void release() override { released = true; }   // the leg owns it
    int count(const std::string& l) const { return (int)std::count(sent.begin(), sent.end(), l); }
};

// The replay's SNAPZ frames split into REGION replies (a reply ends at a frame of < 3000 records).
std::vector<std::vector<std::vector<eden_net::SnapzRec> > > mp_bursts(const MpReplayData& r) {
    std::vector<std::vector<std::vector<eden_net::SnapzRec> > > out(1);
    for (const auto& e : r.world) {
        if (e.kind != eden_net::MpWorldEvent::Snapz) continue;
        out.back().push_back(e.recs);
        if (e.recs.size() < 3000) out.emplace_back();
    }
    if (out.back().empty()) out.pop_back();
    return out;
}

std::set<std::string> mp_docs_listing() {
    std::set<std::string> names;
    if (DIR* d = opendir(eden_platform_documents_root())) {
        while (dirent* e = readdir(d)) names.insert(e->d_name);
        closedir(d);
    }
    return names;
}
}  // namespace

int run_mp_replay() {
    g_selftestTag = "eden-mp-replay";
    MpWorld::testNoEvictCommit = g_opt.mpMutate == "noevictcommit";
    MpWorld::testNoLandHook = g_opt.mpMutate == "nolandhook";
    MpWorld::testNoReconcile = g_opt.mpMutate == "noreconcile";
    MpWorld::testEcho = g_opt.mpMutate == "echo";   // MP.4: fails leg 6
    if (g_opt.mpOverlayMb > 0) MpWorld::overlayCapBytes = (size_t)g_opt.mpOverlayMb << 20;
    MpReplayData rep;
    if (!mp_replay_load(g_opt.mpReplay, rep)) return 1;
    std::string ex = g_opt.mpReplay;
    if (ex.size() > 6 && ex.compare(ex.size() - 6, 6, ".lines") == 0) ex.resize(ex.size() - 6);
    ex += ".expect";
    std::map<std::string, std::string> want;
    if (FILE* f = std::fopen(ex.c_str(), "rb")) {
        char b[512];
        while (std::fgets(b, sizeof b, f)) {
            std::string l(b);
            while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
            const size_t eq = l.find('=');
            if (!l.empty() && l[0] != '#' && eq != std::string::npos) want[l.substr(0, eq)] = l.substr(eq + 1);
        }
        std::fclose(f);
    } else {
        std::fprintf(stderr, "[eden-mp-replay] no %s (port/tools/mp-replay.py expect --write)\n", ex.c_str());
        return 1;
    }
    size_t snapz = 0;
    for (const auto& e : rep.world) snapz += e.kind == eden_net::MpWorldEvent::Snapz ? e.recs.size() : 0;
    std::printf("[eden-mp-replay] %s: %d lines, %zu world events, %zu SNAPZ records, spawn %.2f %.2f %.2f\n",
                g_opt.mpReplay.c_str(), rep.lines, rep.world.size(), snapz, rep.spawn[0], rep.spawn[1], rep.spawn[2]);
    g_app->viewController.setFixedEtime(1.0f / 60.0f);
    g_autosave_seconds = 0;
    char d[320];
    auto hex = [](uint64_t h) { char b[24]; std::snprintf(b, sizeof b, "%016llx", (unsigned long long)h); return std::string(b); };
    auto report_ws = [&](const char* leg, const MpWorld::WindowState& ws) {
        std::printf("[eden-mp-replay] mp.replay leg=%s hash=%s cells=%d ox=%d oz=%d side=%d missing=%d gblock=%d "
                    "overlay=%zu cells/%zu cols/%zu B dropped=%d\n", leg, hex(ws.hash).c_str(), ws.cells, ws.ox, ws.oz,
                    ws.side, ws.missing, ws.gblockMismatch, MpWorld::overlayCells(), MpWorld::overlayColumns(),
                    MpWorld::overlayBytes(), MpWorld::droppedRecords());
        std::fflush(stdout);
    };
    auto settle = [&]() {   // until the window around the player is resident (a teleport's bulk reload)
        for (int i = 0; i < 1200 && MpWorld::windowState().missing; ++i) tick(1);
        tick(g_opt.frames);
    };
    const std::set<std::string> docs0 = mp_docs_listing();

    // ---- leg 1: the join burst lands as flat + overlay -----------------------------------------
    if (!mp_replay_open(rep, rep.spawn)) return 1;
    settle();
    const MpWorld::WindowState w1 = MpWorld::windowState();
    report_ws("1", w1);
    std::snprintf(d, sizeof d, "hash %s cells %d window (%d,%d) side %d; expect %s cells %s window (%s,%s)",
                  hex(w1.hash).c_str(), w1.cells, w1.ox, w1.oz, w1.side, want["spawn.hash"].c_str(),
                  want["spawn.cells"].c_str(), want["spawn.ox"].c_str(), want["spawn.oz"].c_str());
    check(w1.missing == 0 && w1.gblockMismatch == 0, "leg 1: the whole window is resident and blockarray agrees", d);
    check(hex(w1.hash) == want["spawn.hash"] && std::to_string(w1.cells) == want["spawn.cells"] &&
          std::to_string(w1.ox) == want["spawn.ox"] && std::to_string(w1.oz) == want["spawn.oz"] && w1.side == 18,
          "leg 1: the window equals mp-replay.py's flat + replay", d);
    check(w1.cells > 0, "leg 1: the replay has edits in the window", d);

    // ---- the E11 gates, while in the session ---------------------------------------------------
    {
        Terrain* t = World::getWorld->terrain;
        std::snprintf(d, sizeof d, "height %d radius %d seed %d creatures %d gold %d world_name %s", T_HEIGHT, T_RADIUS,
                      t->level_seed, (int)CREATURES_ON, World::getWorld->hud->goldencubes, t->world_name.c_str());
        check(MpWorld::active() && T_HEIGHT == 256 && T_RADIUS <= 9 && t->level_seed == 0 && !CREATURES_ON &&
              World::getWorld->hud->goldencubes == 0, "gates: 256z, radius <= 9, seed 0, no creatures, no gold", d);
        const int rc = CmdScript::runLine("set 0 0 0 stone", 0, 40, 0, CmdScript::SRC_HARNESS, nullptr, nullptr);
        const int rc2 = CmdScript::runLine("tp 0 5 0", 0, 40, 0, CmdScript::SRC_HARNESS, nullptr, nullptr);
        std::snprintf(d, sizeof d, "set -> %d, tp -> %d", rc, rc2);
        check(rc == CmdScript::RUN_BAD && rc2 == CmdScript::RUN_BAD, "gates: console world commands are refused", d);
        eden_set_fire_melts_ice(1);
        const bool iceBurns = (blockinfo[TYPE_ICE] & IS_FLAMMABLE) != 0;
        eden_set_fire_melts_ice(0);
        check(!iceBurns, "gates: H.8 fire-melts-ice stays off in a session", "");
        World::getWorld->fm->saveWorld();   // the pause menu's Save, the bulk reload's, a warp's: to RAM only
    }

    // ---- leg 2: walk out of the window and back -------------------------------------------------
    {
        Terrain* t = World::getWorld->terrain;
        const float X0 = rep.spawn[0], Z0 = rep.spawn[2], Y0 = 60.0f;
        eden_console_teleport(X0, Y0, Z0);
        tick(4);
        const int pcx = (int)X0 / CHUNK_SIZE, pcz = (int)Z0 / CHUNK_SIZE;
        // Local results nobody sent (what fire, liquids and blasts leave): only the evict-commit keeps them.
        struct Col { int cx, cz; };
        const Col E[3] = {{pcx, pcz}, {pcx - 6, pcz + 2}, {pcx + 4, pcz - 5}};
        for (const Col& c : E) {
            const int bx = c.cx * CHUNK_SIZE, bz = c.cz * CHUNK_SIZE;
            for (int k = 0; k < 5; ++k) t->updateChunks(bx + 2 + k, bz + 3 + 2 * k, 200 - 7 * k, TYPE_BRICK);
            t->updateChunks(bx + 9, bz + 9, 32, TYPE_NONE);
            t->setColor(bx + 2, bz + 3, 200, 17);
        }
        tick(4);
        const MpWorld::WindowState w2a = MpWorld::windowState();
        report_ws("2-before", w2a);
        const int bulk0 = g_bulk_reloads, ring0 = g_ring_columns_landed;
        float wx = X0;
        auto walk = [&](float dx) {
            const int steps = (int)std::ceil(std::fabs(dx) / 0.5f);
            for (int k = 1; k <= steps; ++k) { eden_console_teleport(wx + dx * (float)k / (float)steps, Y0, Z0); tick(1); }
            wx += dx;
        };
        const float W = (float)std::max(320, CHUNK_SIZE * (T_RADIUS + 6));
        walk(W);
        tick(120);
        bool gone = true;
        for (const Col& c : E) gone = gone && !isColumnResident(c.cx * CHUNK_SIZE, c.cz * CHUNK_SIZE);
        int keptType = 0, keptColor = 0;
        MpWorld::overlayCell(E[0].cx * CHUNK_SIZE + 2, 200, E[0].cz * CHUNK_SIZE + 3, &keptType, &keptColor);
        walk(-W);
        settle();
        const MpWorld::WindowState w2b = MpWorld::windowState();
        report_ws("2-after", w2b);
        std::snprintf(d, sizeof d, "edited columns streamed out %d; the overlay holds an edit as %d:%d; ring +%d, bulk %d->%d; "
                      "hash %s -> %s", gone, keptType, keptColor, g_ring_columns_landed - ring0, bulk0, g_bulk_reloads,
                      hex(w2a.hash).c_str(), hex(w2b.hash).c_str());
        check(gone && g_ring_columns_landed > ring0 && g_bulk_reloads == bulk0, "leg 2: the walk streams the edited columns out", d);
        check(w2b.hash == w2a.hash && w2b.cells == w2a.cells && w2b.missing == 0 && w2b.gblockMismatch == 0,
              "leg 2: walking out and back keeps every cell (evict-commit to RAM)", d);
    }

    // ---- leg 3 (MP.3b): the live world -- step 5c, reconcile, the pacer ---------------------------
    {
        Terrain* t = World::getWorld->terrain;
        const float X0 = rep.spawn[0], Z0 = rep.spawn[2], Y0 = 60.0f;
        const auto bursts = mp_bursts(rep);
        int64_t (*clock0)() = MpWorld::clockMs;
        MpWorld::clockMs = mp_fake_clock;
        g_mpFakeMs = 1000000;
        MpWorld::resetStats();
        ReplayLink* L = new ReplayLink;
        const int px = mp_lattice(X0), pz = mp_lattice(Z0);
        if (!bursts.empty()) L->answers[std::make_pair(px, pz)] = bursts[0];
        char want0[48];
        std::snprintf(want0, sizeof want0, "REGION:%d:%d", px, pz);

        // 3a: step 5c applies to the resident window, in the frame it drains.
        const int ax = (int)X0 + 6, az = (int)Z0 - 4;
        MpWorld::enqueueAction("server", ax, 45, az, 0, TYPE_BRICK);   // a restore: raw
        MpWorld::enqueueAction("peer", ax, 32, az, 0, TYPE_STONE);     // a peer's build onto grass: no overwrite
        MpWorld::enqueueAction("peer", ax, 46, az, 0, TYPE_COBBLESTONE);
        MpWorld::enqueueAction("peer", ax, 46, az, 3, 21);             // ... and its paint
        tick(1);
        std::snprintf(d, sizeof d, "queued %zu; (%d,45,%d) %d, (%d,32,%d) %d, (%d,46,%d) %d:%d", MpWorld::queuedEvents(), ax, az,
                      t->getLand(ax, az, 45), ax, az, t->getLand(ax, az, 32), ax, az, t->getLand(ax, az, 46), t->getColor(ax, az, 46));
        check(MpWorld::queuedEvents() == 0 && t->getLand(ax, az, 45) == TYPE_BRICK && t->getLand(ax, az, 32) == TYPE_GRASS &&
              t->getLand(ax, az, 46) == TYPE_COBBLESTONE && t->getColor(ax, az, 46) == 21,
              "leg 3: step 5c applies ACTIONs to the resident window the frame they drain (8.2's rules)", d);

        // 3b: a refused edit (a local mine nobody's server accepted) and a re-REGION -> back to flat.
        const int rx = (int)X0 + 12, rz = (int)Z0 - 9;
        t->updateChunks(rx, rz, 32, TYPE_NONE);
        int kx = 0, ky = 0, kz = 0, kt = 0;
        std::sscanf(want["reconcile.keep"].c_str(), "%d:%d:%d:%d", &kx, &ky, &kz, &kt);
        bool kept = false;
        L->onAsk = [&](int, int) {   // changed after the REGION went out: its reply may not know it
            if (kept) return;
            kept = true;
            t->updateChunks(kx, kz, ky, kt);
            MpWorld::noteLocalEdit(kx, ky, kz);
        };
        MpWorld::setLink(L);
        MpWorld::startRegions(true);
        tick_until([] { return MpWorld::stats().reconciles >= 1 && MpWorld::queuedEvents() == 0; }, 600, "the reconcile");
        const MpWorld::Stats s1 = MpWorld::stats();
        const MpWorld::WindowState w3 = MpWorld::windowState();
        report_ws("3-reconcile", w3);
        std::printf("[eden-mp-replay] mp.reconcile sent=%d replies=%d reconciles=%d cells=%d writes=%d frames=%d worst_chunks=%d "
                    "worst_ms=%.2f\n", s1.regionsSent, s1.repliesComplete, s1.reconciles, s1.reconciledCells, s1.residentWrites,
                    s1.applyFrames, s1.worstFrameChunks, s1.worstFrameMs);
        std::snprintf(d, sizeof d, "first line %s (want %s); refused (%d,32,%d) = %d; kept (%d,%d,%d) = %d; restore = %d",
                      L->sent.empty() ? "-" : L->sent[0].c_str(), want0, rx, rz, t->getLand(rx, rz, 32), kx, ky, kz,
                      t->getLand(kx, kz, ky), t->getLand(ax, az, 45));
        check(!L->sent.empty() && L->sent[0] == want0, "leg 3: the first REGION asks the spawn's 128-block lattice point", d);
        check(t->getLand(rx, rz, 32) == TYPE_GRASS && t->getLand(ax, az, 45) == TYPE_NONE,
              "leg 3: a refused edit is back to flat after a re-REGION (reconcile)", d);
        check(t->getLand(kx, kz, ky) == kt, "leg 3: a cell changed after the REGION went out is spared", d);
        std::snprintf(d, sizeof d, "hash %s cells %d; expect %s cells %s; missing %d gblock %d", hex(w3.hash).c_str(), w3.cells,
                      want["reconcile.hash"].c_str(), want["reconcile.cells"].c_str(), w3.missing, w3.gblockMismatch);
        check(hex(w3.hash) == want["reconcile.hash"] && std::to_string(w3.cells) == want["reconcile.cells"] && w3.missing == 0 &&
              w3.gblockMismatch == 0, "leg 3: the window equals mp-replay.py's reconcile model", d);
        std::snprintf(d, sizeof d, "worst frame dirtied %d chunks over %d frames", s1.worstFrameChunks, s1.applyFrames);
        check(s1.worstFrameChunks <= 24 + 6, "leg 3: the play budget caps a frame at 24 chunks (+1 cell)", d);

        // 3c: the lattice switch (> 72 blocks from the point), unanswered asks, the re-ask on return.
        float wx = X0;
        auto walk = [&](float dx) {
            const int steps = (int)std::ceil(std::fabs(dx) / 0.5f);
            for (int k = 1; k <= steps; ++k) { eden_console_teleport(wx + dx * (float)k / (float)steps, Y0, Z0); tick(1); }
            wx += dx;
        };
        char want1[48];
        std::snprintf(want1, sizeof want1, "REGION:%d:%d", mp_lattice(X0 + 80.0f), pz);
        walk(80.0f);
        tick_until([] { return MpWorld::stats().unanswered >= 3; }, 2000, "three unanswered REGIONs");
        const int asked = L->count(want1);
        tick(300);   // 5 s more: the point is given up until it changes
        std::snprintf(d, sizeof d, "%s sent %d then %d times; unanswered %d", want1, asked, L->count(want1), MpWorld::stats().unanswered);
        check(asked == 3 && L->count(want1) == 3 && MpWorld::stats().unanswered == 3,
              "leg 3: walking 80 blocks asks the next lattice point; unanswered 3 times, then it waits", d);
        walk(-80.0f);
        tick_until([] { return MpWorld::stats().reconciles >= 2 && MpWorld::queuedEvents() == 0; }, 600, "the second reconcile");
        settle();
        const MpWorld::WindowState w3b = MpWorld::windowState();
        report_ws("3-return", w3b);
        std::snprintf(d, sizeof d, "%s sent %d times; reconciles %d; kept cell %d; hash %s cells %d, expect %s cells %s", want0,
                      L->count(want0), MpWorld::stats().reconciles, t->getLand(kx, kz, ky), hex(w3b.hash).c_str(), w3b.cells,
                      want["reconcile2.hash"].c_str(), want["reconcile2.cells"].c_str());
        check(L->count(want0) == 2 && MpWorld::stats().reconciles == 2, "leg 3: returning re-asks the spawn's point (never deduplicated)", d);
        // The kept cell was spared only by the REGION it postdated; this reply (which in life would name it,
        // had the server accepted it) doesn't, so it goes.
        check(hex(w3b.hash) == want["reconcile2.hash"] && std::to_string(w3b.cells) == want["reconcile2.cells"] &&
              t->getLand(kx, kz, ky) == TYPE_NONE, "leg 3: a later reply reconciles the spared cell too", d);
        MpWorld::dropLink();
        check(L->released, "leg 3: dropLink releases the link", "");
        delete L;
        MpWorld::clockMs = clock0;
    }

    // ---- leg 6 (MP.4): edits out at the intent sites, a peer's edits in, no echo -----------------
    {
        Terrain* t = World::getWorld->terrain;
        Hud* hud = World::getWorld->hud;
        int64_t (*clock0)() = MpWorld::clockMs;
        MpWorld::clockMs = mp_fake_clock;
        ReplayLink* L = new ReplayLink;   // answers nothing: what it was sent is the evidence
        MpWorld::setLink(L);
        MpWorld::resetStats();
        auto edits = [&](size_t from) {   // the ACTION / SIGNP lines sent since `from` (the pacer's REGIONs aside)
            std::vector<std::string> v;
            for (size_t i = from; i < L->sent.size(); ++i)
                if (L->sent[i].rfind("ACTION:", 0) == 0 || L->sent[i].rfind("SIGNP:", 0) == 0) v.push_back(L->sent[i]);
            return v;
        };
        auto joined = [](const std::vector<std::string>& v) {
            std::string s;
            for (const auto& l : v) s += (s.empty() ? "" : " ") + l;
            return s.empty() ? std::string("(none)") : s;
        };
        auto line = [](const char* fmt, int x, int y, int z, int v) {
            char b[64];
            std::snprintf(b, sizeof b, fmt, x, y, z, v);
            return std::string(b);
        };
        const int bx = (int)rep.spawn[0] + 20, bz = (int)rep.spawn[2] - 30, by = 50;
        bool clear = true;
        for (int dx = -6; dx <= 40; ++dx)
            for (int dz = -6; dz <= 16; ++dz)
                for (int y = by - 2; y <= by + 2; ++y) clear = clear && t->getLand(bx + dx, bz + dz, y) == TYPE_NONE;
        check(clear, "leg 6: the test area is air", "");
        const int savedType = hud->blocktype, savedPaint = hud->block_paintcolor;
        size_t n0 = L->sent.size();

        // 6a: a painted build, then a repaint: the cell as built, one line each.
        hud->blocktype = TYPE_BRICK;
        hud->block_paintcolor = 12;
        MpWorld::intentBuild(bx, bz, by);
        MpWorld::intentPaint(bx, bz, by, 30);
        std::vector<std::string> got = edits(n0);
        std::vector<std::string> want6 = {line("ACTION:%d:%d:%d:0:%d", bx, by, bz, TYPE_BRICK),
                                          line("ACTION:%d:%d:%d:3:%d", bx, by, bz, 12), line("ACTION:%d:%d:%d:3:%d", bx, by, bz, 30)};
        check(got == want6, "leg 6: a painted build and a repaint go out as build + paint, paint", joined(got).c_str());

        // 6b: a door: both halves out on the build and on the mine (U1: retail unknown).
        n0 = L->sent.size();
        hud->blocktype = TYPE_DOOR_TOP;
        hud->block_paintcolor = 0;
        MpWorld::intentBuild(bx + 2, bz, by);
        const int bottom = t->getLand(bx + 2, bz, by - 1);
        MpWorld::intentMine(bx + 2, bz, by);
        got = edits(n0);
        want6 = {line("ACTION:%d:%d:%d:0:%d", bx + 2, by - 1, bz, bottom), line("ACTION:%d:%d:%d:0:%d", bx + 2, by, bz, TYPE_DOOR_TOP),
                 line("ACTION:%d:%d:%d:1%.0d", bx + 2, by - 1, bz, 0), line("ACTION:%d:%d:%d:1%.0d", bx + 2, by, bz, 0)};
        check(bottom >= TYPE_DOOR1 && bottom < TYPE_DOOR_TOP && got == want6, "leg 6: a door goes out as both halves, built and mined",
              joined(got).c_str());

        // 6c: E10 -- a burn on a non-flammable block sends nothing (the server would delete it).
        n0 = L->sent.size();
        MpWorld::intentBurn(bx, bz, by);
        got = edits(n0);
        check(got.empty() && !isOnFire(bx, bz, by), "leg 6: burning brick sends nothing", joined(got).c_str());

        // 6d: TNT: the burn out, then the follow-up mine once its fuse runs out on time.
        const int tx = bx + 30;
        n0 = L->sent.size();
        hud->blocktype = TYPE_TNT;
        MpWorld::intentBuild(tx, bz, by);
        MpWorld::intentBurn(tx, bz, by);
        const bool litTnt = isOnFire(tx, bz, by);
        for (int i = 0; i < 600 && isOnFire(tx, bz, by); ++i) tick(1);
        tick(2);
        got = edits(n0);
        want6 = {line("ACTION:%d:%d:%d:0:%d", tx, by, bz, TYPE_TNT), line("ACTION:%d:%d:%d:2%.0d", tx, by, bz, 0),
                 line("ACTION:%d:%d:%d:1%.0d", tx, by, bz, 0)};
        check(litTnt && got == want6 && t->getLand(tx, bz, by) == TYPE_NONE,
              "leg 6: TNT goes out as build, burn, then one mine when the fuse runs out (the blast is not sent)", joined(got).c_str());

        // 6e: a burn whose cell a server restore mines first: no follow-up mine.
        const int wx = bx + 12;
        n0 = L->sent.size();
        hud->blocktype = TYPE_WOOD;
        MpWorld::intentBuild(wx, bz, by);
        MpWorld::intentBurn(wx, bz, by);
        const bool litWood = isOnFire(wx, bz, by);
        MpWorld::enqueueAction("server", wx, by, bz, 1, -1);
        for (int i = 0; i < 600 && isOnFire(wx, bz, by); ++i) tick(1);
        tick(2);
        got = edits(n0);
        want6 = {line("ACTION:%d:%d:%d:0:%d", wx, by, bz, TYPE_WOOD), line("ACTION:%d:%d:%d:2%.0d", wx, by, bz, 0)};
        check(litWood && got == want6 && t->getLand(wx, bz, by) == TYPE_NONE,
              "leg 6: a burn ended by a restore's mine sends no mine of its own", joined(got).c_str());

        // 6f: a sign on the brick: SIGNP with the wire's y = height.
        n0 = L->sent.size();
        const int su = SignTool::use(bx, bz, by, 0, 1, 0, 0.0f);
        SignTool::answer(0, "Hello: MP.4");
        if (GLDialog::active()) GLDialog::dismiss();
        tick(2);
        got = edits(n0);
        char sw[96];
        std::snprintf(sw, sizeof sw, "SIGNP:%d:%d:%d:3:45:%d:Hello: MP.4", bx, by, bz, SignTool::facingForYaw(0.0f));
        check(su == SignTool::USE_PROMPT_NEW && got == std::vector<std::string>{sw}, "leg 6: a placed sign goes out as SIGNP",
              joined(got).c_str());
        hud->blocktype = savedType;
        hud->block_paintcolor = savedPaint;

        // 6f2: a server's signs land as sent: b = 0 (old sidecars carry it; the editor's range check refused
        // them, 2026-10-17), UTF-8 text as '?', text past SGN1's 95 cut, not dropped.
        {
            WorldTrailer* tr = World::getWorld->fm->trailer();
            const int d0 = MpWorld::droppedRecords();
            MpWorld::enqueueSign(bx, by, bz, 0, 0, 0, "Caf\xc3\xa9 \xe2\x86\x92 north");
            MpWorld::enqueueSign(bx, by, bz, 1, 2, 7, std::string(256, 'y').c_str());
            tick(1);
            const int i0 = tr->findSign(TrailerPos::fromEngine(bx, by, bz), 0);
            const int i1 = tr->findSign(TrailerPos::fromEngine(bx, by, bz), 1);
            const bool ok = i0 >= 0 && i1 >= 0 && !std::strcmp(tr->sign((size_t)i0).text, "Caf? ? north") &&
                            tr->sign((size_t)i0).b == 0 && tr->sign((size_t)i1).c == 7 &&
                            std::strlen(tr->sign((size_t)i1).text) == 95 && MpWorld::droppedRecords() == d0;
            check(ok, "leg 6: a server's b = 0 / UTF-8 / 256-byte signs land, cleaned, none dropped",
                  i0 >= 0 ? tr->sign((size_t)i0).text : "(no sign)");
            if (i1 >= 0) tr->removeSign((size_t)i1);
            const int j0 = tr->findSign(TrailerPos::fromEngine(bx, by, bz), 0);
            if (j0 >= 0) tr->removeSign((size_t)j0);
        }

        // 6g: a peer's edits in, through the engine's own primitives (doors in pairs, fire runs here),
        // and nothing goes back out (--mp-mutate=echo fails this).
        const int px = bx, pz = bz + 10;
        n0 = L->sent.size();
        const MpWorld::Stats s0 = MpWorld::stats();
        MpWorld::enqueueAction("peer", px, by - 1, pz, 0, TYPE_DOOR1);
        MpWorld::enqueueAction("peer", px, by, pz, 0, TYPE_DOOR_TOP);
        MpWorld::enqueueAction("peer", px, by - 1, pz, 3, 21);   // paintBlock paints the top half too
        tick(1);
        const int topPaint = t->getColor(px, pz, by);
        MpWorld::enqueueAction("peer", px, by, pz, 1, -1);        // destroyBlock takes both halves
        tick(1);
        const int doorAfter = t->getLand(px, pz, by - 1);
        MpWorld::enqueueAction("peer", px + 3, by, pz, 0, TYPE_WOOD);
        MpWorld::enqueueAction("peer", px + 3, by, pz, 2, -1);    // burnBlock(FALSE): the fire runs here
        tick(1);
        const bool peerFire = isOnFire(px + 3, pz, by);
        for (int i = 0; i < 600 && isOnFire(px + 3, pz, by); ++i) tick(1);
        tick(2);
        got = edits(n0);
        std::snprintf(d, sizeof d, "top paint %d, bottom after mine %d, peer fire %d, wood after %d, peer-applied %d; sent %s", topPaint,
                      doorAfter, (int)peerFire, t->getLand(px + 3, pz, by), MpWorld::stats().peerApplied - s0.peerApplied,
                      joined(got).c_str());
        check(topPaint == 21 && doorAfter == TYPE_NONE && peerFire && t->getLand(px + 3, pz, by) == TYPE_NONE,
              "leg 6: a peer's paint, mine and burn apply as the game's (door pairs, the fire runs locally)", d);
        check(got.empty(), "leg 6: applying a peer's edits sends nothing (no echo)", d);
        const MpWorld::Stats s6 = MpWorld::stats();
        std::printf("[eden-mp-replay] mp.edits actions=%d signs=%d peer_applied=%d\n", s6.actionsSent, s6.signsSent, s6.peerApplied);
        MpWorld::dropLink();
        delete L;
        MpWorld::clockMs = clock0;
    }

    // ---- leg 7 (MP.5): remote players: avatars, dead reckoning, POSVEL out, leaving ----------------
    {
        int64_t (*clock0)() = MpWorld::clockMs;
        MpWorld::clockMs = mp_fake_clock;
        ReplayLink* L = new ReplayLink;
        MpWorld::setLink(L);
        Player* pl = World::getWorld->player;
        const int sent0 = MpWorld::posVelSent();
        const double a[6] = {rep.spawn[0] + 10.0, rep.spawn[1] + 2.0, rep.spawn[2] + 10.0, 0, 0, 0};
        MpWorld::peerSeen("Zed", 3, 2, a);
        const double b[6] = {a[0] + 4.0, a[1], a[2], 4.0, 0, 0};   // 4 blocks east, walking east at 4/s
        MpWorld::peerSeen("Zed", 3, 2, b);
        MpWorld::peerSeen("Amy", 9, 0, a);
        tick(1);
        MpWorld::PeerView v;
        bool have = MpWorld::peerCount() == 2 && MpWorld::peerView(0, &v);
        std::snprintf(d, sizeof d, "peers %d, bodies shown %d, available %d", MpWorld::peerCount(), RemoteBodiesShown(),
                      (int)PlayerBodyAvailable());
        check(have && RemoteBodiesShown() == 2 && PlayerBodyAvailable(), "leg 7: two peers are drawn as bodies in a session", d);
        // Glide: monotone toward (and never past) the extrapolated goal, within the cap.
        float prev = -1e9f;
        bool mono = true, within = true;
        for (int i = 0; i < 40; ++i) {
            tick(1);
            for (int k = 0; k < MpWorld::peerCount(); ++k) {
                if (!MpWorld::peerView(k, &v) || std::strcmp(v.name, "Zed") != 0) continue;
                mono = mono && v.x >= prev - 1e-3f;
                prev = v.x;
                within = within && v.x <= v.tx + 0.6f * 4.0f + 1e-2f;
            }
        }
        MpWorld::peerView(0, &v);
        std::snprintf(d, sizeof d, "Zed x %.2f, last report %.2f, yaw %.1f, motion %d, avatar model %d", v.x, v.tx, v.yaw, v.motion,
                      MpWorld::avatarModel(v.type));
        check(mono && within && v.x > a[0] + 3.0f && v.motion == PB_WALK && std::fabs(v.yaw) < 5.0f,
              "leg 7: a walking peer glides to its report and faces its heading", d);
        check(MpWorld::avatarModel(0) == M_GREEN && MpWorld::avatarModel(10) >= 0 && MpWorld::avatarModel(99) == MpWorld::avatarModel(10) &&
                  MpWorld::avatarModel(-1) == M_GREEN,
              "leg 7: characterType 0..10 maps to a shipped model, out-of-range clamps", "");
        // A far jump snaps.
        const double far_[6] = {a[0] + 200.0, a[1], a[2], 0, 0, 0};
        MpWorld::peerSeen("Amy", 9, 0, far_);
        tick(1);
        for (int k = 0; k < MpWorld::peerCount(); ++k)
            if (MpWorld::peerView(k, &v) && !std::strcmp(v.name, "Amy")) break;
        check(std::fabs(v.x - far_[0]) < 0.5f, "leg 7: a peer reported far away snaps instead of gliding", "");
        // POSVEL out: ~4 Hz while the player moves, then at most 1 Hz still.
        const int64_t t0 = mp_fake_clock();
        const int n0 = MpWorld::posVelSent();
        size_t l0 = L->sent.size();
        for (int i = 0; i < 120; ++i) {   // ~2 s of fake time
            pl->pos.x += 0.2f;
            pl->vel.x = 6.0f;
            tick(1);
        }
        const int moving = MpWorld::posVelSent() - n0;
        const int64_t dt = mp_fake_clock() - t0;
        pl->vel.x = 0;
        const int n1 = MpWorld::posVelSent();
        for (int i = 0; i < 120; ++i) tick(1);
        const int idle = MpWorld::posVelSent() - n1;
        int posvels = 0;
        bool fmt = true;
        for (size_t i = l0; i < L->sent.size(); ++i)
            if (L->sent[i].rfind("POSVEL:", 0) == 0) {
                ++posvels;
                int c = 0;
                for (char ch : L->sent[i]) c += ch == ':';
                fmt = fmt && c == 6;
            }
        std::snprintf(d, sizeof d, "moving: %d POSVEL in %lld ms, idle: %d in the next 120 frames; lines %d, 7-field %d", moving,
                      (long long)dt, idle, posvels, (int)fmt);
        check(moving >= 1 && moving <= (int)(dt / 250) + 1 && idle >= 1 && idle <= (int)(dt / 1000) + 2 && fmt && posvels == moving + idle,
              "leg 7: POSVEL leaves at <= 4 Hz moving and <= 1 Hz still, in the wire format", d);
        (void)sent0;
        // ---- leg 8 (MP.7): chat in, chat out, the rate guard, /login, the panel, the HUD feed ---------
        {
            ChatFeed::reset();
            ChatFeed::clockMs = mp_chat_clock;
            ChatFeed::addWire(false, "Alex", "anyone near spawn?");
            ChatFeed::addWire(true, "", "TNT is disabled on this server.");
            const std::vector<Console::Line>& cl = ChatFeed::lines();
            check(cl.size() == 2 && cl[0].text == "Alex: anyone near spawn?" && cl[0].kind == ChatFeed::K_CHAT &&
                      cl[1].text == "[Server] TNT is disabled on this server." && cl[1].kind == ChatFeed::K_SERVER,
                  "leg 8: relayed chat and [Server] lines land in the scrollback, styled by kind", "");
            const size_t s0 = L->sent.size();
            check(ChatFeed::send("hello") == ChatFeed::SENT && L->sent.size() == s0 + 1 && L->sent.back() == "MSG:hello" &&
                      cl.back().text == "You: hello" && cl.back().kind == ChatFeed::K_SELF,
                  "leg 8: a line goes out as MSG:text with a local echo", L->sent.back().c_str());
            check(ChatFeed::send("a:b:c") == ChatFeed::SENT && L->sent.back() == "MSG:a:b:c", "leg 8: a ':' in the text is safe", L->sent.back().c_str());
            int ok = 2;
            while (ChatFeed::send("burst") == ChatFeed::SENT && ok < 20) ++ok;
            std::snprintf(d, sizeof d, "%d lines passed the 0.5/s burst-5 guard", ok);
            check(ok == 5, "leg 8: the client guard stops the sixth line in a burst (burst 5)", d);
            const size_t sBefore = L->sent.size();
            check(ChatFeed::send("held") == ChatFeed::TOO_FAST && L->sent.size() == sBefore, "leg 8: a refused line is not sent", "");
            check(ChatFeed::send("/msg Alex psst") == ChatFeed::SENT && L->sent.back() == "MSG:/msg Alex psst",
                  "leg 8: a / command is exempt from the guard and goes out verbatim", L->sent.back().c_str());
            g_mpFakeMs += 2500;   // 1.25 tokens
            check(ChatFeed::send("later") == ChatFeed::SENT, "leg 8: the guard refills over time", "");
            const size_t hist0 = ChatFeed::historyCount();
            ChatFeed::send("/login 1234");
            check(L->sent.back() == "MSG:/login 1234" && cl.back().text == "> /login ****" && ChatFeed::historyCount() == hist0,
                  "leg 8: /login is masked in the scrollback and never kept in history", cl.back().text.c_str());
            check(ChatFeed::send("   ") == ChatFeed::EMPTY, "leg 8: an empty line is not sent", "");
            // The panel: the Console screen on its Chat tile, with the session tiles in front.
            ConsoleMenu* con = World::getWorld->menu->console;
            check(eden_chat_open(0) == 1 && con->isOpen() && con->chatTab() && con->railTabs() == 5,
                  "leg 8: T opens the Console screen on Chat with five tiles in a session", "");
            const double pa[6] = {rep.spawn[0] + 6.0, rep.spawn[1] + 2.0, rep.spawn[2], 0, 0, 0};
            MpWorld::peerSeen("Kim", 2, 0, pa);
            con->setCollapsed(false);   // MP.14: T opens the collapsed bar; the rail (and so Players) is on the full screen
            con->showTab(ConsoleMenu::TAB_PLAYERS);
            tick(1);
            std::snprintf(d, sizeof d, "rows %d peers %d", con->playerRows(), MpWorld::peerCount());
            check(con->playerRows() == MpWorld::peerCount() + 1, "leg 8: Players lists you and every player seen", d);
            con->showTab(ConsoleMenu::TAB_CHAT);
            con->setInput("sent from the panel");
            const size_t s1 = L->sent.size();
            g_mpFakeMs += 3000;
            con->submit();
            check(L->sent.size() == s1 + 1 && L->sent.back() == "MSG:sent from the panel" && con->input().empty(),
                  "leg 8: Send from the panel sends and clears the field", L->sent.back().c_str());
            con->close();
            check(eden_chat_open(1) == 1 && con->input() == "/", "leg 8: / opens Chat with the slash typed", con->input().c_str());
            con->close();
            // The HUD feed: the newest lines (cap 5), gone after 10 s.
            ChatFeed::update(0.0f);
            ChatFeed::renderHud(false);
            const int shown = ChatFeed::feedLines();
            ChatFeed::update(11.0f);
            ChatFeed::renderHud(false);
            std::snprintf(d, sizeof d, "feed %d lines, then %d after 11 s", shown, ChatFeed::feedLines());
            check(shown >= 1 && shown <= 5 && ChatFeed::feedLines() == 0, "leg 8: the HUD feed shows at most 5 lines and they fade out after 10 s", d);
            ChatFeed::clockMs = nullptr;
            ChatFeed::reset();
            MpWorld::peerLeft("Kim");
        }
        // Leaving.
        MpWorld::peerLeft("Zed");
        tick(1);
        std::snprintf(d, sizeof d, "peers %d, bodies shown %d", MpWorld::peerCount(), RemoteBodiesShown());
        check(MpWorld::peerCount() == 1 && RemoteBodiesShown() == 1, "leg 7: a peer that left is gone with its body", d);
        MpWorld::dropLink();
        delete L;
        MpWorld::clockMs = clock0;
    }

    // ---- leg 5: a teleport's bulk reload equals a fresh join there ------------------------------
    const float dest[3] = {(float)std::atof(want["dest.x"].c_str()), (float)std::atof(want["dest.y"].c_str()),
                           (float)std::atof(want["dest.z"].c_str())};
    {
        const int bulk0 = g_bulk_reloads;
        eden_console_teleport(dest[0], dest[1], dest[2]);
        tick(2);
        settle();
        const MpWorld::WindowState w5 = MpWorld::windowState();
        report_ws("5-teleport", w5);
        std::snprintf(d, sizeof d, "bulk reloads %d->%d; hash %s cells %d window (%d,%d); expect %s cells %s window (%s,%s)",
                      bulk0, g_bulk_reloads, hex(w5.hash).c_str(), w5.cells, w5.ox, w5.oz, want["dest.hash"].c_str(),
                      want["dest.cells"].c_str(), want["dest.ox"].c_str(), want["dest.oz"].c_str());
        check(g_bulk_reloads > bulk0 && w5.missing == 0 && w5.gblockMismatch == 0, "leg 5: the teleport bulk-reloads the window", d);
        check(hex(w5.hash) == want["dest.hash"] && std::to_string(w5.cells) == want["dest.cells"] && w5.cells > 0,
              "leg 5: the reloaded window equals mp-replay.py's at the destination", d);
        if (!quit_to_menu()) return 1;
        std::snprintf(d, sizeof d, "active %d, overlay %zu cells, creatures %d", (int)MpWorld::active(), MpWorld::overlayCells(),
                      (int)CREATURES_ON);
        check(!MpWorld::active() && MpWorld::overlayCells() == 0, "exit: leaving the session drops the overlay", d);
        if (!mp_replay_open(rep, dest)) return 1;
        settle();
        const MpWorld::WindowState wf = MpWorld::windowState();
        report_ws("5-fresh", wf);
        std::snprintf(d, sizeof d, "teleport %s/%d, fresh join %s/%d", hex(w5.hash).c_str(), w5.cells, hex(wf.hash).c_str(), wf.cells);
        check(wf.hash == w5.hash && wf.cells == w5.cells && wf.missing == 0, "leg 5: a fresh join at the destination matches", d);
        if (!quit_to_menu()) return 1;
    }

    // ---- leg 4 (MP.3b): a 600k-record join through the whole flow, then a relay storm ----------
    {
        const int P = 65536, H = 80;   // a 160x160 area, y 33-44, every cell a painted block: 614,400 records
        std::vector<eden_net::SnapzRec> all;
        for (int cx = (P - H) >> 4; cx < (P + H) >> 4; ++cx)          // the server's order: chunk by chunk,
            for (int cz = (P - H) >> 4; cz < (P + H) >> 4; ++cz)      // then x, z, y, flag inside one
                for (int cy = 2; cy <= 2; ++cy)
                    for (int x = cx * 16; x < cx * 16 + 16; ++x)
                        for (int z = cz * 16; z < cz * 16 + 16; ++z)
                            for (int y = 33; y < 45; ++y) {
                                const int ty = (x + z + y) % 3 == 0 ? TYPE_BRICK : (x + z + y) % 3 == 1 ? TYPE_COBBLESTONE : TYPE_STONE;
                                all.push_back({x, y, z, 0, ty});
                                all.push_back({x, y, z, 3, (x * 7 + z * 3 + y) % 54 + 1});
                            }
        std::vector<std::string> lines;
        for (size_t i = 0; i < all.size(); i += 3000)
            lines.push_back(eden_net::encodeSnapzLine(all.data() + i, std::min<size_t>(3000, all.size() - i), 1));
        if (all.size() % 3000 == 0) lines.push_back(eden_net::encodeSnapzLine(nullptr, 0, 1));
        size_t wire = 0;
        for (const auto& l : lines) wire += l.size() + 1;
        // The net thread's work (base64 + inflate + parse), timed here on the main thread.
        const Uint64 td0 = SDL_GetPerformanceCounter();
        eden_net::MpSession dec;
        for (const auto& l : lines) dec.feedLine(l);
        std::vector<eden_net::MpWorldEvent> frames;
        dec.drainWorld(frames);
        const double decodeMs = (double)(SDL_GetPerformanceCounter() - td0) * 1000.0 / (double)SDL_GetPerformanceFrequency();
        int64_t (*clock0)() = MpWorld::clockMs;
        MpWorld::clockMs = mp_fake_clock;
        g_mpFakeMs = 2000000;
        ReplayLink* L = new ReplayLink;
        for (const auto& f : frames) L->answers[std::make_pair(P, P)].push_back(f.recs);
        L->joinFlow = true;
        MpWorld::resetStats();
        MpWorld::beginSession("replay", 0, "Leg 4");
        MpWorld::setSpawn((float)P + 0.5f, 60.0f, (float)P + 0.5f);
        MpWorld::setLink(L);
        size_t heldPeak = 0;   // Q.6: MpWorld::heldBytes(), what the session's byte cap sees
        auto frameMs = [&heldPeak] {
            const Uint64 a = SDL_GetPerformanceCounter();
            tick(1);
            heldPeak = std::max(heldPeak, MpWorld::heldBytes());
            return (double)(SDL_GetPerformanceCounter() - a) * 1000.0 / (double)SDL_GetPerformanceFrequency();
        };
        const Uint64 tj0 = SDL_GetPerformanceCounter();
        auto since = [tj0] { return (double)(SDL_GetPerformanceCounter() - tj0) * 1000.0 / (double)SDL_GetPerformanceFrequency(); };
        MpWorld::startRegions(true);
        double syncWorst = 0, loadWorst = 0, syncMs = 0, loadMs = 0;
        int syncFrames = 0, loadFrames = 0;
        for (int i = 0; i < 20000 && L->joinFlow; ++i) { syncWorst = std::max(syncWorst, frameMs()); syncFrames++; }
        syncMs = since();
        for (int i = 0; i < 20000 && game_mode() != 1; ++i) {   // the load lands the whole window before PLAY
            loadWorst = std::max(loadWorst, frameMs());
            loadFrames++;
        }
        loadMs = since() - syncMs;
        const MpWorld::WindowState w4 = MpWorld::windowState();
        report_ws("4-join", w4);
        std::printf("[eden-mp-replay] mp.join records=%zu frames=%zu wire=%zu B decode=%.1f ms sync=%.1f ms/%d frames worst=%.2f ms "
                    "load=%.1f ms/%d frames worst=%.2f ms playable=%.1f ms overlay=%zu B\n", all.size(), frames.size(), wire,
                    decodeMs, syncMs, syncFrames, syncWorst, loadMs, loadFrames, loadWorst, since(), MpWorld::overlayBytes());
        std::snprintf(d, sizeof d, "game_mode %d, window cells %d (want %d), missing %d, gblock %d, sent %zu", game_mode(), w4.cells,
                      2 * H * 2 * H * 12, w4.missing, w4.gblockMismatch, L->sent.size());
        check(game_mode() == 1 && w4.cells == 2 * H * 2 * H * 12 && w4.missing == 0 && w4.gblockMismatch == 0,
              "leg 4: a 600k-record join lands whole through Syncing -> Loading -> Play", d);
        std::snprintf(d, sizeof d, "%d sync frames for %zu SNAPZ frames, worst %.2f ms", syncFrames, frames.size(), syncWorst);
        check(syncFrames >= 2, "leg 4: the join drain is spread over frames (8 ms budget)", d);
        std::snprintf(d, sizeof d, "peak %zu B for %zu records, %zu B after", heldPeak, all.size(), MpWorld::heldBytes());
        check(heldPeak >= all.size() * sizeof(eden_net::SnapzRec) && MpWorld::heldBytes() == 0,
              "leg 4: held bytes count the queued burst, 0 once applied", d);

        // A 2,000-cell relay storm in play: peers building into air, all over the window.
        Terrain* t = World::getWorld->terrain;
        tick(30);
        MpWorld::resetStats();
        for (int i = 0; i < 2000; ++i)
            MpWorld::enqueueAction("peer", P - 100 + (i % 50) * 4, 50 + (i / 500) * 20, P - 100 + ((i / 50) % 10) * 20, 0, TYPE_BRICK);
        double stormWorst = 0;
        int stormFrames = 0;
        for (int i = 0; i < 2000 && MpWorld::queuedEvents(); ++i) { stormWorst = std::max(stormWorst, frameMs()); stormFrames++; }
        int landed = 0;
        for (int i = 0; i < 2000; ++i)
            landed += t->getLand(P - 100 + (i % 50) * 4, P - 100 + ((i / 50) % 10) * 20, 50 + (i / 500) * 20) == TYPE_BRICK;
        const MpWorld::Stats s4 = MpWorld::stats();
        std::printf("[eden-mp-replay] mp.storm cells=2000 landed=%d frames=%d worst_frame=%.2f ms worst_apply=%.2f ms worst_chunks=%d\n",
                    landed, stormFrames, stormWorst, s4.worstFrameMs, s4.worstFrameChunks);
        std::snprintf(d, sizeof d, "landed %d/2000 over %d frames, worst %d chunks", landed, stormFrames, s4.worstFrameChunks);
        check(landed == 2000 && stormFrames >= 2 && s4.worstFrameChunks <= 24 + 6,
              "leg 4: a 2,000-cell relay storm lands whole, at most 24 chunks (+1 cell) a frame", d);
        std::snprintf(d, sizeof d, "%zu B after the storm", MpWorld::heldBytes());
        check(MpWorld::heldBytes() == 0, "leg 4: held bytes return to 0 after the storm", d);
        if (!quit_to_menu()) return 1;
        check(L->released && !MpWorld::active(), "leg 4: leaving the session releases the link", "");
        delete L;
        MpWorld::clockMs = clock0;
    }

    // Nothing a session did reached the save directory (no world file, no thumbnail, no backup slot).
    {
        const std::set<std::string> docs1 = mp_docs_listing();
        std::string extra;
        for (const auto& n : docs1) if (!docs0.count(n)) extra += " " + n;
        check(extra.empty(), "a session writes nothing to the save directory", extra.empty() ? "none" : extra.c_str());
    }
    g_app->viewController.setFixedEtime(0.0f);
    std::printf("[eden-mp-replay] %s (%d failure(s))%s\n", g_selftestFailures ? "FAILURES" : "ALL PASS", g_selftestFailures,
                g_opt.mpMutate.empty() ? "" : (" [mutation " + g_opt.mpMutate + "]").c_str());
    return g_selftestFailures ? 1 : 0;
}

// --mp-join=HOST[:PORT]: the MP.3b join flow (port/src/seam/MpLink_native.cpp). Interactive: straight from
// the main menu. Headless (run_mp_join): a live gate -- join, wait for play, print the window state, run
// --frames, leave; exit 0 only if the session became playable and left cleanly.
extern "C" int eden_mp_join(const char* host, int port, const char* name, int characterType, const char* password);
int mp_join_from_args() {
    std::string host = g_opt.mpJoin;
    int port = 27015;
    const size_t c = host.rfind(':');
    if (c != std::string::npos) { port = std::atoi(host.c_str() + c + 1); host.resize(c); }
    return eden_mp_join(host.c_str(), port, g_opt.mpName.c_str(), g_opt.mpType, g_opt.mpPassword.c_str());
}

// MP.4's two-client leg (port/tools/mp-replay.py edits). --mp-edits (the builder): edits near the spawn
// through the intent sites -- a painted build and repaint, a door, a burnt wood block, a TNT blast, a
// mined grass cell, a sign -- then waits for its fires and sends. --mp-hold=FILE (the watcher): plays
// until FILE exists. Both then re-ask their REGION, wait for its reconcile, and print the window hash,
// the session's signs and what they sent; the script compares them with each other and the server.
void mp_wait_ms(Uint32 ms) {
    const Uint64 t0 = SDL_GetTicks();
    while (SDL_GetTicks() - t0 < ms) tick(1);
}
void mp_live_edits(const MpWorld::WindowState& w0) {
    Terrain* t = World::getWorld->terrain;
    Hud* hud = World::getWorld->hud;
    if (g_opt.mpEdits) {
        const Vector p = World::getWorld->player->pos;
        const int bx = (int)p.x + 20, bz = (int)p.z - 30, by = 50, tx = bx + 30, wx = bx + 12;
        hud->blocktype = TYPE_BRICK;
        hud->block_paintcolor = 12;
        MpWorld::intentBuild(bx, bz, by);
        MpWorld::intentPaint(bx, bz, by, 30);
        hud->block_paintcolor = 0;
        hud->blocktype = TYPE_DOOR_TOP;
        MpWorld::intentBuild(bx + 4, bz, by);
        hud->blocktype = TYPE_WOOD;
        MpWorld::intentBuild(wx, bz, by);
        MpWorld::intentBurn(wx, bz, by);
        hud->blocktype = TYPE_BRICK;
        for (int k = -2; k <= 2; k += 4) MpWorld::intentBuild(tx + k, bz, by);   // in the blast
        hud->blocktype = TYPE_TNT;
        MpWorld::intentBuild(tx, bz, by);
        MpWorld::intentBurn(tx, bz, by);
        MpWorld::intentMine(bx, bz + 5, 32);   // natural grass
        SignTool::use(bx, bz, by, 0, 1, 0, 0.0f);
        SignTool::answer(0, "Hello: MP.4");
        if (GLDialog::active()) GLDialog::dismiss();
        tick_until([&] { return !isOnFire(tx, bz, by) && !isOnFire(wx, bz, by); }, 3000, "the builder's fires to burn out");
        check(t->getLand(tx, bz, by) == TYPE_NONE && t->getLand(wx, bz, by) == TYPE_NONE && t->getLand(bx, bz, by) == TYPE_BRICK,
              "edits: the builder's edits landed locally", "");
        mp_wait_ms(1500);   // the net thread writes what is queued
    } else {
        const Uint64 t0 = SDL_GetTicks();
        while (SDL_GetTicks() - t0 < 180000) {
            if (FILE* f = std::fopen(g_opt.mpHold.c_str(), "rb")) { std::fclose(f); break; }
            tick(1);
        }
        mp_wait_ms(1000);
    }
    const MpWorld::WindowState wp = MpWorld::windowState();   // what the relays alone made
    const int r0 = MpWorld::stats().reconciles;
    MpWorld::reaskRegion();
    const bool rec = tick_until([r0] { return MpWorld::stats().reconciles > r0 && MpWorld::queuedEvents() == 0; }, 3000,
                                "the closing reconcile");
    check(rec, "edits: the closing REGION is answered and reconciled", "");
    const MpWorld::WindowState w = MpWorld::windowState();
    const MpWorld::Stats s = MpWorld::stats();
    std::printf("[eden-mp-join] edits role=%s final hash=%016llx cells=%d pre=%016llx/%d (joined %016llx/%d) sent actions=%d "
                "signs=%d peer_applied=%d\n", g_opt.mpEdits ? "builder" : "watcher", (unsigned long long)w.hash, w.cells,
                (unsigned long long)wp.hash, wp.cells, (unsigned long long)w0.hash, w0.cells, s.actionsSent, s.signsSent, s.peerApplied);
    WorldTrailer* tr = World::getWorld->fm->trailer();
    for (size_t i = 0; i < tr->signCount(); ++i) {
        const TrailerSign& sg = tr->sign(i);
        char text[96];
        std::memcpy(text, sg.text, 95);
        text[95] = 0;
        std::printf("[eden-mp-join] sign %d:%d:%d:%d:%d:%d:%s\n", sg.x, sg.z, sg.y, sg.a, sg.b, sg.c, text);   // wire order
    }
    std::fflush(stdout);
}

int run_mp_join() {
    g_selftestTag = "eden-mp-join";
    g_autosave_seconds = 0;
    MpWorld::testEcho = g_opt.mpMutate == "echo";   // MP.4: mp-replay.py edits --mutate echo must fail
    tick(10);
    check(mp_join_from_args() == 1, "the join starts", g_opt.mpJoin.c_str());
    const bool ok = tick_until([] { return game_mode() == 1 && MpWorld::active(); }, 3000,
                               "the session world to be playable");
    check(ok, "the session becomes playable", g_opt.mpJoin.c_str());
    if (ok) {
        const MpWorld::WindowState w = MpWorld::windowState();
        const MpWorld::Stats s = MpWorld::stats();
        std::printf("[eden-mp-join] window hash=%016llx cells=%d ox=%d oz=%d overlay=%zu cells regions=%d replies=%d reconciles=%d\n",
                    (unsigned long long)w.hash, w.cells, w.ox, w.oz, MpWorld::overlayCells(), s.regionsSent, s.repliesComplete,
                    s.reconciles);
        if (g_opt.mpEdits || !g_opt.mpHold.empty()) mp_live_edits(w);
        tick(g_opt.frames);
        check(quit_to_menu() && !MpWorld::active() && !MpWorld::link(), "leaving the session closes it", "");
    }
    std::printf("[eden-mp-join] %s (%d failure(s))\n", g_selftestFailures ? "FAILURES" : "ALL PASS", g_selftestFailures);
    return g_selftestFailures ? 1 : 0;
}

}  // namespace g_mp
}  // namespace harness
