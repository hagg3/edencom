// G.4: split out of the old eden_main_native.cpp (see harness.h). Mode group "cmds".
#include "harness.h"

using namespace harness;

namespace harness {
namespace g_cmds {


// ---------------------------------------------------------------------------------------------
// --cmds-selftest  (Stage D / D.4c, the command-block interpreter)
// ---------------------------------------------------------------------------------------------
// With --world=NAME (the 3-bit adder, in --docs) and --at near its keypad: a parser leg on literal
// scripts; every CMB1 script in the world parses with no bad statement; the adder computes 3 + 4 = 7
// on its own seven-segment display when its buttons are pressed; then every command, the caps and
// the triggers on command blocks this test places in open air; a real screen tap on a command block
// with a block armed pushes it and builds nothing; save + reload keeps no runtime state. Fixed
// 1/60 s etime: waits, timers and fuses are engine time. --cmds-mutate=nodepth (run ignores the
// 8-deep cap) must fail the depth leg; --cmds-trace prints every statement.
namespace cmds_test {

Terrain* ter() { return World::getWorld->terrain; }
WorldTrailer* tr() { return (WorldTrailer*)eden_debug_world_trailer(); }
// Engine order (x, y_up, z) throughout this test.
int land(int x, int y, int z) { return ter()->getLand(x, z, y); }
int paint(int x, int y, int z) { return ter()->getColor(x, z, y); }
void put(int x, int y, int z, int type) { ter()->updateChunks(x, z, y, type); }
TrailerPos P(int x, int y, int z) { return TrailerPos::fromEngine(x, y, z); }
// A command block at (x, y, z) with this script (steel + record, as the CMD tool makes one).
TrailerPos make(int x, int y, int z, const char* script) {
    put(x, y, z, TYPE_STEEL);
    ter()->setColor(x, z, y, 0);
    const TrailerPos p = P(x, y, z);
    const int i = tr()->findCmd(p);
    if (i >= 0) tr()->setCmdScript((size_t)i, script);
    else tr()->addCmd(p, script);
    return p;
}
void secs(float s) { tick((int)(s * 60.0f + 0.5f)); }
std::string toast() {
    const std::string* t = World::getWorld->hud->sb->current();
    return t ? *t : std::string();
}
// An all-air box (w x h x d) starting 3 above the highest ground under it, near (px, pz). Engine order.
bool open_air(int px, int pz, int w, int h, int d, int* ox, int* oy, int* oz) {
    // East of the adder (it spans x 65512..65581), inside the resident window.
    for (int k = 0; k < 16; ++k) {
        const int x = px + 50 + (k % 4) * (w + 2), z = pz - 20 + (k / 4) * (d + 2);
        int top = -1;
        for (int a = 0; a < w; ++a)
            for (int c = 0; c < d; ++c)
                for (int y = T_HEIGHT - 1; y > top; --y)
                    if (land(x + a, y, z + c) > 0) { top = y; break; }
        const int y0 = top + 3;
        if (top < 0 || y0 + h + 1 >= T_HEIGHT) continue;
        *ox = x; *oy = y0; *oz = z;
        return true;
    }
    return false;
}

// The adder's units display: a seven-segment digit on the plane x = 65512 (engine order), each
// segment's cells read off the scripts at x = 65521 that light it ("fill -9 9 -1 -9 9 1 lamp 9"...).
struct Seg { const char* name; int z0, z1, y0, y1; };
const Seg kSegs[7] = {
    {"top", 65532, 65534, 42, 42}, {"top-left", 65535, 65535, 39, 41}, {"top-right", 65531, 65531, 39, 41},
    {"center", 65532, 65534, 38, 38}, {"bottom-left", 65535, 65535, 35, 37},
    {"bottom-right", 65531, 65531, 35, 37}, {"bottom", 65532, 65534, 34, 34}};
// Bit i = segment i lit (every cell lamp 9, the off colour is 54); -1 if a segment is mixed or not lamps.
int segments(std::string* why) {
    int bits = 0;
    bool bad = false;
    for (int i = 0; i < 7; ++i) {
        int lit = 0, off = 0, other = 0;
        for (int z = kSegs[i].z0; z <= kSegs[i].z1; ++z)
            for (int y = kSegs[i].y0; y <= kSegs[i].y1; ++y) {
                const int t = land(65512, y, z), c = paint(65512, y, z);
                if (t == TYPE_LIGHTBOX && c == 9) ++lit;
                else if (t == TYPE_LIGHTBOX && c == 54) ++off;
                else ++other;
            }
        if (why) *why += std::string(kSegs[i].name) + "=" + std::to_string(lit) + "/" + std::to_string(off) +
                         (other ? "/?" + std::to_string(other) : "") + " ";
        if (other || (lit && off)) bad = true;
        if (lit) bits |= 1 << i;
    }
    return bad ? -1 : bits;
}
int digitOf(int bits) {
    // bit order: top, top-left, top-right, center, bottom-left, bottom-right, bottom
    static const int kDigits[10] = {0x77, 0x24, 0x5D, 0x6D, 0x2E, 0x6B, 0x7B, 0x25, 0x7F, 0x6F};
    for (int d = 0; d < 10; ++d) if (kDigits[d] == bits) return d;
    return -1;
}

}  // namespace cmds_test

// The parser on literal scripts: one case table, run by --cmds-selftest and --console-selftest (C.1
// moved it out of the adder test so CI, which has no adder, checks the parser on every target).
void cmds_parser_cases() {
    char d[1024];
    {
        CmdScript::Program p;
        CmdScript::parse("say WELCOME TRAVELER;wait 1;set 0 3 0 lamp;gold 1", &p);
        check(p.errors == 0 && p.trigger == CmdScript::TRIG_TOUCH && p.stmts.size() == 4 &&
              p.stmts[0].op == CmdScript::OP_SAY && p.stmts[0].text == "WELCOME TRAVELER" &&
              p.stmts[1].op == CmdScript::OP_WAIT && p.stmts[1].f == 1.0f && p.stmts[2].op == CmdScript::OP_SET &&
              p.stmts[2].v[1] == 3 && p.stmts[2].type == TYPE_LIGHTBOX && p.stmts[3].op == CmdScript::OP_GOLD,
              "parse: cmd.html's first example", p.firstError.c_str());
        CmdScript::parse("@timer .1; if 0 -1 0 2; run 0 -2 0; set 0 -1 0 0", &p);
        check(p.errors == 0 && p.trigger == CmdScript::TRIG_TIMER && std::fabs(p.triggerArg - 0.1f) < 1e-6f &&
              p.stmts.size() == 3 && p.stmts[0].op == CmdScript::OP_IF && p.stmts[0].type == TYPE_STONE &&
              p.stmts[1].op == CmdScript::OP_RUN && p.stmts[1].v[1] == -2,
              "parse: a header, block numbers, negative heights", p.firstError.c_str());
        CmdScript::parse("@near 6;set ~ ~3 ~ glass;fill -1 0 -1 1 2 1 brick 19", &p);
        check(p.errors == 0 && p.trigger == CmdScript::TRIG_NEAR && p.triggerArg == 6 && p.stmts[0].v[0] == 0 &&
              p.stmts[0].v[1] == 3 && p.stmts[0].type == TYPE_GLASS && p.stmts[1].op == CmdScript::OP_FILL &&
              p.stmts[1].color == 19, "parse: tildes, fill with a colour", p.firstError.c_str());
        CmdScript::parse("frob 1 2; fill 0 0 0 16 16 16 stone; say ok; @step; set 0 0 0 voidstone; wait 9999; colour", &p);
        std::snprintf(d, sizeof(d), "errors %d, first '%s', wait %.0f", p.errors, p.firstError.c_str(),
                      p.stmts.size() > 5 ? p.stmts[5].f : -1.0f);
        check(p.errors == 5 && p.firstError == "Unknown command: frob" && p.stmts.size() == 7 &&
              p.stmts[2].op == CmdScript::OP_SAY && p.stmts[5].op == CmdScript::OP_WAIT && p.stmts[5].f == 600.0f,
              "parse: unknown command, fill > 4096, late trigger, unknown block; wait capped at 600", d);
        // The last sound and one past it (NUM_SOUNDS grew from 73 to 79 in 6ef81de; this leg pinned 72/73).
        std::snprintf(d, sizeof(d), "fill 0 0 0 15 15 15 stone; spawn moof; spawn charger 1 2 3; spawn dragon; sound %d; "
                      "sound %d; time 0.5; time 2; paint 0 0 0 54; paint 0 0 0 55", NUM_SOUNDS - 1, NUM_SOUNDS);
        CmdScript::parse(d, &p);
        std::snprintf(d, sizeof(d), "errors %d, first '%s'", p.errors, p.firstError.c_str());
        check(p.errors == 4 && p.stmts.size() == 10 && p.stmts[0].op == CmdScript::OP_FILL && p.stmts[2].hasPos &&
              p.stmts[2].type == M_CHARGER && p.stmts[3].op == CmdScript::OP_BAD && p.stmts[4].op == CmdScript::OP_SOUND &&
              p.stmts[5].op == CmdScript::OP_BAD && p.stmts[7].op == CmdScript::OP_BAD &&
              p.stmts[8].op == CmdScript::OP_PAINT && p.stmts[9].op == CmdScript::OP_BAD,
              "parse: fill at exactly 4096, creatures, sound/time/colour ranges", d);
    }
}

int run_cmds_selftest() {
    using namespace cmds_test;
    g_selftestTag = "eden-cmds";
    CmdScript::testNoDepthCap = g_opt.cmdsMutate == "nodepth";
    CmdScript::trace = g_opt.cmdsTrace;
    char d[1024];

    // ---- the parser, on literal scripts (shared with --console-selftest)
    cmds_parser_cases();

    if (g_opt.world.empty()) { check(false, "--world=NAME (the 3-bit adder) is given", ""); return 1; }
    if (!open_world(g_opt.world.c_str(), g_opt.height)) return 1;
    if (g_opt.haveAt) eden_console_teleport(g_opt.at[0], g_opt.at[1], g_opt.at[2]);
    tick(g_opt.frames / 4);
    if (GLDialog::active()) { GLDialog::dismiss(); tick(2); }   // --world-format=emod's "World converted"
    g_app->viewController.setFixedEtime(1.0f / 60.0f);
    WorldTrailer* t = tr();
    Hud* hud = World::getWorld->hud;
    Player* pl = World::getWorld->player;
    std::printf("[eden-cmds] world %s: %s\n", eden_debug_world_file(), t ? t->describe().c_str() : "no trailer");

    // ---- every script in the world parses
    {
        int bad = 0;
        std::string first;
        for (size_t i = 0; t && i < t->cmdCount(); ++i) {
            CmdScript::Program p;
            CmdScript::parse(t->cmd(i).script, &p);
            bad += p.errors;
            if (p.errors && first.empty()) first = std::string(t->cmd(i).script) + " -> " + p.firstError;
        }
        std::snprintf(d, sizeof(d), "%zu scripts, %d bad statements%s%s", t ? t->cmdCount() : 0, bad,
                      first.empty() ? "" : ": ", first.c_str());
        check(t && t->cmdCount() > 0 && bad == 0, "every CMB1 script parses (0 bad statements)", d);
    }

    // ---- the adder: Reset, 3, +, 4, =. The keypad, file order (x, y, z) -> engine (x, z, y).
    // The answer is the digit on the units display.
    {
        struct Key { const char* name; int x, y, z; };
        const Key keys[] = {{"Reset", 65531, 33, 65524}, {"3", 65531, 34, 65523}, {"+", 65531, 33, 65522},
                            {"4", 65531, 35, 65525}, {"=", 65531, 33, 65523}};
        std::string log;
        bool allPressed = true;
        for (const Key& k : keys) {
            const int r = CmdScript::press(P(k.x, k.y, k.z));
            allPressed = allPressed && r == CmdScript::PRESS_STARTED;
            secs(3.0f);
            std::string why;
            const int bits = segments(&why);
            log += std::string(k.name) + ":" + std::to_string(r) + "->" + std::to_string(digitOf(bits)) + " ";
            std::printf("[eden-cmds] adder after %-5s %s| %s\n", k.name, why.c_str(), CmdScript::describe());
        }
        secs(4.0f);
        std::string why;
        const int digit = digitOf(segments(&why));
        const CmdScript::Stats s = CmdScript::stats();
        std::snprintf(d, sizeof(d), "%s| final %d (%s) depth-refused %d unknown %d", log.c_str(), digit, why.c_str(),
                      s.refusedDepth, s.unknown);
        check(allPressed && digit == 7 && s.refusedDepth == 0 && s.unknown == 0 && s.refusedFull == 0,
              "the 3-bit adder: Reset, 3, +, 4, = shows 7", d);
        // ... and the keys light their own digit on the way (3 after "3", 4 after "4").
        check(log.find("3:1->3") != std::string::npos && log.find("4:1->4") != std::string::npos,
              "the adder's keys show their digit as they are pressed", log.c_str());
        std::printf("[eden-cmds] adder %s\n", CmdScript::describe());

        // More sums, so "7" is the adder adding and not a coincidence: the tens display is a "1" at
        // z = 65538 (the TEN script), lit or not.
        const Key digitKeys[8] = {{"0", 65531, 33, 65525}, {"1", 65531, 34, 65525}, {"2", 65531, 34, 65524},
                                  {"3", 65531, 34, 65523}, {"4", 65531, 35, 65525}, {"5", 65531, 35, 65524},
                                  {"6", 65531, 35, 65523}, {"7", 65531, 36, 65525}};
        auto tens = [&]() {
            int lit = 0, off = 0;
            for (int y = 35; y <= 41; ++y) {
                if (y == 38) continue;
                const int ty = land(65512, y, 65538), c = paint(65512, y, 65538);
                if (ty == TYPE_LIGHTBOX && c == 9) ++lit;
                else if (ty == TYPE_LIGHTBOX && c == 54) ++off;
            }
            return lit == 6 ? 1 : off == 6 ? 0 : -1;
        };
        const int sums[][2] = {{0, 0}, {1, 6}, {2, 3}, {5, 2}, {5, 5}, {7, 7}, {6, 7}, {7, 1}};
        std::string all;
        int wrong = 0;
        for (const auto& ab : sums) {
            const Key seq[] = {keys[0], digitKeys[ab[0]], keys[2], digitKeys[ab[1]], keys[4]};
            for (const Key& k : seq) { CmdScript::press(P(k.x, k.y, k.z)); secs(3.0f); }
            secs(3.0f);
            const int u = digitOf(segments(NULL)), tn = tens();
            const int got = (u < 0 || tn < 0) ? -1 : tn * 10 + u;
            all += std::to_string(ab[0]) + "+" + std::to_string(ab[1]) + "=" + std::to_string(got) + " ";
            if (got != ab[0] + ab[1]) ++wrong;
        }
        std::snprintf(d, sizeof(d), "%s| maxDepth %d, depth-refused %d", all.c_str(), CmdScript::stats().maxDepth,
                      CmdScript::stats().refusedDepth);
        check(wrong == 0 && CmdScript::stats().refusedDepth == 0, "the adder adds: eight more sums, 0..14", d);
    }

    // ---- the commands, on blocks placed in open air. The player is parked out of every written cell.
    const PlayerState ps = player_state();
    int ox, oy, oz;
    if (!open_air((int)ps.x, (int)ps.z, 16, 12, 16, &ox, &oy, &oz)) { check(false, "an open-air test site", ""); return 1; }
    auto park = [&]() {
        pl->pos = MakeVector(ox - 30.5f, (float)oy + 6.0f, oz - 30.5f);
        pl->vel = MakeVector(0, 0, 0);
    };
    park();
    tick(10);
    std::printf("[eden-cmds] site %d,%d,%d (engine order)\n", ox, oy, oz);
    const int X = ox + 2, Y = oy + 2, Z = oz + 2;   // the first block; legs use offsets from it

    // set / fill / paint / a lamp, and the skin's green button while it runs (min 0.25 s)
    {
        const TrailerPos A = make(X, Y, Z, "set 0 2 0 glass; fill 1 0 0 2 1 1 brick 19; paint 1 0 0 30; set 0 1 0 lamp 9");
        tick(2);
        const int r = CmdScript::press(A);
        tick(1);
        const bool green = CmdTool::running(A);
        std::snprintf(d, sizeof(d), "press %d; glass %d; brick %d/%d %d/%d; lamp %d/%d; green %d",
                      r, land(X, Y + 2, Z), land(X + 1, Y, Z), paint(X + 1, Y, Z), land(X + 2, Y + 1, Z + 1),
                      paint(X + 2, Y + 1, Z + 1), land(X, Y + 1, Z), paint(X, Y + 1, Z), green);
        check(r == CmdScript::PRESS_STARTED && land(X, Y + 2, Z) == TYPE_GLASS && land(X + 1, Y, Z) == TYPE_BRICK &&
              paint(X + 1, Y, Z) == 30 && land(X + 2, Y + 1, Z + 1) == TYPE_BRICK && paint(X + 2, Y + 1, Z + 1) == 19 &&
              land(X, Y + 1, Z) == TYPE_LIGHTBOX && paint(X, Y + 1, Z) == 9 && green,
              "set, fill (+colour), paint, a lamp; the button goes green", d);
        secs(0.5f);
        std::snprintf(d, sizeof(d), "green %d, busy %d", CmdTool::running(A), CmdScript::busy(A));
        check(!CmdTool::running(A) && !CmdScript::busy(A), "a script with no wait is done; green fades after 0.25 s", d);
        // clean up for the next legs: air through set (the record on A goes too, like mining)
        const TrailerPos C = make(X + 8, Y, Z, "fill -8 0 0 -6 2 1 air");
        CmdScript::press(C);
        tick(2);
        std::snprintf(d, sizeof(d), "A voxel %d record %d; brick %d", land(X, Y, Z), tr()->findCmd(A), land(X + 1, Y, Z));
        check(land(X, Y, Z) == 0 && tr()->findCmd(A) < 0 && land(X + 1, Y, Z) == 0 && land(X, Y + 1, Z) == 0,
              "fill air clears blocks and takes a command block's record with its steel", d);
    }

    // wait: non-blocking, the button stays green while it waits, other blocks keep running
    {
        const TrailerPos A = make(X, Y, Z, "set 0 2 0 stone; wait 1; set 0 2 0 air");
        const TrailerPos B = make(X + 2, Y, Z, "set 0 2 0 dirt");
        CmdScript::press(A);
        tick(3);
        const bool mid = land(X, Y + 2, Z) == TYPE_STONE && CmdScript::busy(A) && CmdTool::running(A);
        const int rb = CmdScript::press(A);   // busy: no re-entry
        CmdScript::press(B);
        tick(2);
        const bool other = land(X + 2, Y + 2, Z) == TYPE_DIRT && land(X, Y + 2, Z) == TYPE_STONE;
        secs(1.2f);
        std::snprintf(d, sizeof(d), "mid %d, re-press %d, other %d, after: %d busy %d green %d", mid, rb, other,
                      land(X, Y + 2, Z), CmdScript::busy(A), CmdTool::running(A));
        check(mid && rb == CmdScript::PRESS_BUSY && other && land(X, Y + 2, Z) == 0 && !CmdScript::busy(A) &&
              !CmdTool::running(A), "wait holds only its own script; a busy block ignores a press", d);
    }

    // if / unless gate the next statement; a skipped if takes its gated statement along
    {
        put(X, Y + 2, Z, 0);
        const TrailerPos A = make(X, Y, Z, "if 0 2 0 air; set 0 3 0 dirt; unless 0 2 0 air; set 0 4 0 dirt; "
                                           "if 0 2 0 stone; if 0 2 0 air; set 0 5 0 dirt; set 0 6 0 dirt; "
                                           "unless 0 2 0 stone; unless 0 3 0 stone; set 0 7 0 dirt");
        CmdScript::press(A);
        tick(2);
        std::snprintf(d, sizeof(d), "y+3 %d y+4 %d y+5 %d y+6 %d y+7 %d", land(X, Y + 3, Z), land(X, Y + 4, Z),
                      land(X, Y + 5, Z), land(X, Y + 6, Z), land(X, Y + 7, Z));
        check(land(X, Y + 3, Z) == TYPE_DIRT && land(X, Y + 4, Z) == 0 && land(X, Y + 5, Z) == 0 &&
              land(X, Y + 6, Z) == TYPE_DIRT && land(X, Y + 7, Z) == TYPE_DIRT, "if / unless, and chained (AND)", d);
        for (int y = 3; y <= 7; ++y) put(X, Y + y, Z, 0);
    }

    // run chains 8 deep: ten blocks in a row, each "run 1 0 0; set 0 1 0 stone" -- the first nine set
    // their stone (depth 0..8), the tenth is refused. The mutation runs all ten.
    {
        const CmdScript::Stats s0 = CmdScript::stats();
        for (int k = 0; k < 10; ++k) make(X + k, Y + 4, Z + 4, "run 1 0 0; set 0 1 0 stone");
        tick(2);
        CmdScript::press(P(X, Y + 4, Z + 4));
        tick(2);
        int n = 0;
        for (int k = 0; k < 10; ++k) n += land(X + k, Y + 5, Z + 4) == TYPE_STONE;
        const CmdScript::Stats s1 = CmdScript::stats();
        std::snprintf(d, sizeof(d), "%d of 10 ran, depth refusals %d, toast '%s'", n, s1.refusedDepth - s0.refusedDepth,
                      toast().c_str());
        check(n == 9 && land(X + 9, Y + 5, Z + 4) == 0 && s1.refusedDepth - s0.refusedDepth == 1,
              "run chains 8 deep, the 9th run is refused", d);
        CmdScript::press(make(X + 12, Y + 4, Z + 4, "fill -12 0 0 -2 1 0 air"));
        tick(2);
    }

    // 32 at once: 40 blocks with "wait 2", pressed in one frame
    {
        const CmdScript::Stats s0 = CmdScript::stats();
        std::vector<TrailerPos> v;
        for (int k = 0; k < 40; ++k) v.push_back(make(X + (k % 10), Y + 7, Z + 6 + k / 10, "wait 2"));
        tick(1);
        for (const TrailerPos& p : v) CmdScript::press(p);
        const int live = CmdScript::liveCount();
        const CmdScript::Stats s1 = CmdScript::stats();
        secs(2.3f);
        std::snprintf(d, sizeof(d), "live %d, refused %d, after %d", live, s1.refusedFull - s0.refusedFull, CmdScript::liveCount());
        check(live == 32 && s1.refusedFull - s0.refusedFull == 8 && CmdScript::liveCount() == 0,
              "at most 32 scripts run at once", d);
        CmdScript::press(make(X + 12, Y + 7, Z + 6, "fill -12 0 0 -3 0 3 air"));
        tick(2);
    }

    // the safety rules: bedrock, the player's cell, another command block, unresident cells
    {
        int by = -1;
        for (int y = 0; y < 4; ++y) if (land(X, y, Z) == TYPE_BEDROCK) { by = y; break; }
        const TrailerPos B = make(X + 3, Y, Z, "");
        char sc[200];
        std::snprintf(sc, sizeof(sc), "set 0 %d 0 air; set 3 0 0 stone; set 0 0 400 stone; fill 0 -1 0 0 -1 0 stone", by - Y);
        const TrailerPos A = make(X, Y, Z, sc);
        // the player stands in the cell below A (where the fill writes)
        pl->pos = MakeVector(X + 0.5f, (float)(Y - 1) + pl->boxheight / 2 + 0.001f, Z + 0.5f);
        pl->vel = MakeVector(0, 0, 0);
        const int below = land(X, Y - 1, Z);
        CmdScript::press(A);
        park();
        tick(2);
        std::snprintf(d, sizeof(d), "bedrock y=%d -> %d; cmd B %d/%d; far %d; player cell %d (was %d)", by,
                      by >= 0 ? land(X, by, Z) : -9, land(X + 3, Y, Z), tr()->findCmd(B), land(X, Y, Z + 400),
                      land(X, Y - 1, Z), below);
        check(by >= 0 && land(X, by, Z) == TYPE_BEDROCK && land(X + 3, Y, Z) == TYPE_STEEL && tr()->findCmd(B) >= 0 &&
              land(X, Y - 1, Z) == below, "set/fill never touch bedrock, a command block or the player's cell", d);
        CmdScript::press(make(X + 1, Y, Z, "set -1 0 0 air; set 2 0 0 air"));
        tick(2);
    }

    // an unknown command shows its message; the rest still runs
    {
        const TrailerPos A = make(X, Y, Z, "frob 1; set 0 2 0 dirt");
        const CmdScript::Stats s0 = CmdScript::stats();
        CmdScript::press(A);
        tick(1);
        const std::string msg = toast();
        std::snprintf(d, sizeof(d), "toast '%s', dirt %d", msg.c_str(), land(X, Y + 2, Z));
        check(msg == "Unknown command: frob" && land(X, Y + 2, Z) == TYPE_DIRT && CmdScript::stats().unknown == s0.unknown + 1,
              "an unknown command shows a message and the rest still runs", d);
        put(X, Y + 2, Z, 0);
    }

    // say, tp, gold, heal, hurt (only with Health on), flash, night / day, sound, spawn
    {
        const TrailerPos A = make(X, Y, Z, "say HELLO there; tp 0 3 0; gold 5; heal; flash; sound 8");
        const int g0 = hud->goldencubes;
        pl->life = 0.5f;
        CmdScript::press(A);
        tick(1);
        const PlayerState p1 = player_state();
        const float feet = pl->pos.y - pl->boxheight / 2;
        std::snprintf(d, sizeof(d), "toast '%s', player %.2f,%.2f,%.2f feet %.3f, gold %d->%d, life %.2f, flash %.2f",
                      toast().c_str(), p1.x, p1.y, p1.z, feet, g0, hud->goldencubes, pl->life, hud->flash);
        check(toast() == "HELLO there" && std::fabs(pl->pos.x - (X + 0.5f)) < 0.01f && std::fabs(pl->pos.z - (Z + 0.5f)) < 0.01f &&
              std::fabs(feet - (Y + 3)) < 0.05f && hud->goldencubes == std::min(999, g0 + 5) && pl->life == 1.0f && hud->flash > 0,
              "say, tp (feet at the cell), gold, heal, flash", d);
        park();
        const bool h0 = pl->health_option;
        pl->health_option = false;
        tr()->setCmdScript((size_t)tr()->findCmd(A), "hurt 25");
        CmdScript::press(A);
        tick(1);
        const float offLife = pl->life;
        pl->health_option = true;
        CmdScript::press(A);
        tick(1);
        const float onLife = pl->life;
        pl->health_option = h0;
        pl->life = 1;
        std::snprintf(d, sizeof(d), "health off %.2f, on %.2f", offLife, onLife);
        check(offLife == 1.0f && std::fabs(onLife - 0.75f) < 0.01f, "hurt bites only with Health on", d);
        tr()->setCmdScript((size_t)tr()->findCmd(A), "night");
        CmdScript::press(A);
        tick(2);
        const bool night = v_equals(ter()->final_skycolor, colorTable[54]);
        tr()->setCmdScript((size_t)tr()->findCmd(A), "time 0.5");
        CmdScript::press(A);
        tick(2);
        const bool day = !v_equals(ter()->final_skycolor, colorTable[54]);
        check(night && day, "night paints the sky night, time 0.5 gives the day back",
              night && day ? "" : night ? "day failed" : "night failed");
        tr()->setCmdScript((size_t)tr()->findCmd(A), "spawn moof; spawn stumpy 0 1 3");
        const int c0 = CountActiveCreatures();
        CmdScript::press(A);
        tick(2);
        std::snprintf(d, sizeof(d), "creatures %d -> %d", c0, CountActiveCreatures());
        check(!CREATURES_ON || CountActiveCreatures() >= c0 + 1, "spawn puts creatures in the world", d);
    }

    // fire and explode: a wood block burns; a lit TNT drops and blows (taking the command block)
    {
        put(X + 4, Y, Z, TYPE_WOOD);
        const TrailerPos A = make(X, Y, Z, "fire 4 0 0; explode 0 0 0");
        tick(1);
        CmdScript::press(A);
        tick(2);
        const bool burning = isOnFire(X + 4, Z, Y);
        const bool tnt = land(X, Y, Z) == TYPE_TNT && isOnFire(X, Z, Y);
        bool gone = false;
        for (int k = 0; k < 2400 && !gone; ++k) { tick(1); gone = land(X, Y, Z) == 0; }
        std::snprintf(d, sizeof(d), "wood burning %d, tnt lit %d, blown %d, record %d", burning, tnt, gone, tr()->findCmd(A));
        check(burning && tnt && gone && tr()->findCmd(A) < 0, "fire lights a burnable block; explode drops lit TNT", d);
        tick(240);
    }

    // triggers: @step, @near, @timer (only within 48 blocks)
    {
        const int sx = X + 6, sy = Y, sz = Z + 10;
        const TrailerPos S = make(sx, sy, sz, "@step; set 2 0 0 dirt");
        const TrailerPos N = make(sx + 8, sy, sz, "@near 3; set 0 2 0 dirt");
        tick(2);
        auto standOn = [&](int x, int y, int z) {
            pl->pos = MakeVector(x + 0.5f, (float)(y + 1) + pl->boxheight / 2 + 0.001f, z + 0.5f);
            pl->vel = MakeVector(0, 0, 0);
        };
        standOn(sx, sy, sz);
        tick(2);
        const bool s1 = land(sx + 2, sy, sz) == TYPE_DIRT;
        put(sx + 2, sy, sz, 0);
        standOn(sx, sy, sz);
        tick(5);
        const bool sStay = land(sx + 2, sy, sz) == 0;   // standing still does not re-fire
        park();
        tick(2);
        standOn(sx, sy, sz);
        tick(2);
        const bool s2 = land(sx + 2, sy, sz) == TYPE_DIRT;   // re-armed by stepping off
        park();
        std::snprintf(d, sizeof(d), "first %d, still %d, again %d", s1, sStay, s2);
        check(s1 && sStay && s2, "@step fires on stepping on, once, and re-arms off the block", d);

        pl->pos = MakeVector(sx + 8 + 0.5f + 2.0f, sy + 0.5f, sz + 0.5f);   // 2 blocks away
        pl->vel = MakeVector(0, 0, 0);
        tick(2);
        const bool n1 = land(sx + 8, sy + 2, sz) == TYPE_DIRT;
        put(sx + 8, sy + 2, sz, 0);
        tick(5);
        const bool nStay = land(sx + 8, sy + 2, sz) == 0;
        park();
        tick(2);
        pl->pos = MakeVector(sx + 8 + 0.5f, sy + 0.5f, sz + 0.5f - 2.5f);
        pl->vel = MakeVector(0, 0, 0);
        tick(2);
        const bool n2 = land(sx + 8, sy + 2, sz) == TYPE_DIRT;
        park();
        tick(2);
        std::snprintf(d, sizeof(d), "first %d, still %d, again %d", n1, nStay, n2);
        check(n1 && nStay && n2, "@near fires on approach, once, and re-arms when the player leaves", d);

        // Counted in golden cubes: the adder's own @timer .1 relays fire too while it is within 48.
        const TrailerPos T = make(sx + 4, sy, sz + 3, "@timer 0.5; gold 1");
        hud->goldencubes = 0;
        pl->pos = MakeVector(sx + 4.5f, sy + 0.5f, sz + 3.5f + 20.0f);   // 20 blocks: in range
        pl->vel = MakeVector(0, 0, 0);
        secs(3.0f);
        const int near = hud->goldencubes;
        pl->pos = MakeVector(sx + 4.5f, sy + 0.5f, sz + 3.5f + 60.0f);   // 60 blocks: asleep
        pl->vel = MakeVector(0, 0, 0);
        tick(1);
        const int g1 = hud->goldencubes;
        secs(3.0f);
        const int far = hud->goldencubes - g1;
        hud->goldencubes = 10;
        std::snprintf(d, sizeof(d), "in range %d fires in 3 s, at 60 blocks %d", near, far);
        check(near >= 5 && near <= 7 && far == 0, "@timer 0.5 fires every 0.5 s, only within 48 blocks", d);
        park();
        for (const TrailerPos& p : {S, N, T}) put(p.x, p.z, p.y, 0);
        put(sx + 2, sy, sz, 0);
        put(sx + 8, sy + 2, sz, 0);
        tick(2);
    }

    // a real screen tap with a block armed pushes a command block and builds nothing
    {
        const TrailerPos A = make(X, Y, Z, "set 0 2 0 dirt");
        tick(4);
        hud->mode = MODE_BUILD;
        hud->blocktype = TYPE_STONE;
        Camera* cam = World::getWorld->cam;
        const float fx = X + 0.5f, fy = Y + 0.5f, fz = (float)Z + 1.0f;   // its +z face
        for (int k = 0; k < 4; ++k) {
            pl->pos = MakeVector(fx, fy, fz + 3.0f);
            pl->vel = MakeVector(0, 0, 0);
            tick(1);
            const float dx = fx - cam->px, dy = fy - cam->py, dz = fz - cam->pz;
            pl->yaw = std::atan2(dz, dx) * 180.0f / (float)M_PI;
            pl->pitch = std::atan2(dy, std::sqrt(dx * dx + dz * dz)) * 180.0f / (float)M_PI;
        }
        tick(1);
        const int presses0 = CmdScript::stats().presses;
        ui_pointer(0, SCREEN_WIDTH * 0.5f, SCREEN_HEIGHT * 0.5f); tick(1);
        ui_pointer(2, SCREEN_WIDTH * 0.5f, SCREEN_HEIGHT * 0.5f); tick(2);
        std::snprintf(d, sizeof(d), "presses +%d, dirt %d, built in front %d", CmdScript::stats().presses - presses0,
                      land(X, Y + 2, Z), land(X, Y, Z + 1));
        check(CmdScript::stats().presses == presses0 + 1 && land(X, Y + 2, Z) == TYPE_DIRT && land(X, Y, Z + 1) == 0,
              "a build tap on a command block pushes it (and builds nothing)", d);
        park();
        put(X, Y + 2, Z, 0);
        tick(2);
    }

    // the frame budget: five 4096-cell fills in one script pause and finish on the next frames
    {
        const CmdScript::Stats s0 = CmdScript::stats();
        const TrailerPos A = make(X - 1, Y - 1, Z - 1, "fill 0 1 0 15 16 15 air; fill 0 1 0 15 16 15 air; "
                                                       "fill 0 1 0 15 16 15 air; fill 0 1 0 15 16 15 air; "
                                                       "fill 0 1 0 15 16 15 air; set 0 1 0 dirt");
        CmdScript::press(A);
        const bool paused = CmdScript::busy(A) && CmdScript::stats().suspended > s0.suspended;
        tick(4);
        std::snprintf(d, sizeof(d), "paused %d, done %d, dirt %d", paused, !CmdScript::busy(A), land(X - 1, Y, Z - 1));
        check(paused && !CmdScript::busy(A) && land(X - 1, Y, Z - 1) == TYPE_DIRT,
              "past 20,000 statements + cells a frame scripts pause and resume", d);
        put(X - 1, Y, Z - 1, 0);
        put(X - 1, Y - 1, Z - 1, 0);
    }

    // a script still waiting when the world closes: nothing of it survives the reload
    const TrailerPos W = make(X, Y, Z, "wait 30; set 0 2 0 dirt");
    CmdScript::press(W);
    tick(2);
    const bool wasGreen = CmdTool::running(W);
    g_app->viewController.setFixedEtime(0.0f);
    save_world();
    if (!quit_to_menu()) return 1;
    std::snprintf(d, sizeof(d), "green before %d; after quit: live %d, green %d", wasGreen, CmdScript::liveCount(), CmdTool::running(W));
    check(wasGreen && CmdScript::liveCount() == 0 && !CmdTool::running(W), "closing the world stops every script", d);
    tick(30);
    if (!open_world(g_opt.world.c_str(), g_opt.height)) return 1;
    if (g_opt.haveAt) eden_console_teleport(g_opt.at[0], g_opt.at[1], g_opt.at[2]);
    tick(30);
    t = tr();
    std::snprintf(d, sizeof(d), "%s; W record %d, live %d", t ? t->describe().c_str() : "no trailer",
                  t ? t->findCmd(W) : -1, CmdScript::liveCount());
    check(t && t->findCmd(W) >= 0 && CmdScript::liveCount() == 0 && !CmdTool::running(W),
          "after reload: the command block is kept, its run is not", d);
    std::printf("[eden-cmds] %s\n", CmdScript::describe());
    std::printf("[eden-cmds] %s (%d failure(s))\n", g_selftestFailures ? "FAILURES" : "ALL PASS", g_selftestFailures);
    return g_selftestFailures ? 1 : 0;
}

// ---------------------------------------------------------------------------------------------
// --console-selftest  (Stage C / C.1, the command-block language as a console line)
// ---------------------------------------------------------------------------------------------
// dev-console-plan.md §8. World-free first: the shared parser case table; gate 1, the corpus
// (--console-corpus=PATH, default ../port/tools/fixtures/cmds/corpus.txt) parsed with block and
// console options, classified and field-compared; the kVerbs table against the parser's own errors;
// runLine's refusals before a world. Then in a fresh world in --docs: gate 2 (execution equivalence:
// each script pressed on a command block P, then typed with the player standing on P -- the cells,
// player, purse, health, creatures and sky must come out identical), and runLine's jobs, `=`, sink,
// stop, the stand-in registry and the time hook. --console-mutate=anchor (the anchor is the feet cell,
// not the one under them) must FAIL it.
extern "C" {   // port/src/seam/Settings.cpp -- C.2's gates read the tables the cvars were built from
int         eden_settings_count(void);
const char* eden_settings_key(int i);
const char* eden_settings_label(int i);
const char* eden_settings_group(int i);
int         eden_settings_kind(int i);
float       eden_settings_min(int i);
float       eden_settings_max(int i);
float       eden_settings_def(int i);
float       eden_settings_get(int i);
void        eden_settings_set(int i, float v);
int         eden_settings_enum_count(int i);
const char* eden_settings_enum_label(int i, int j);
int         eden_settings_native_hidden(int i);
int         eden_settings_apply(int i);
int         eden_keybind_count(void);
int         eden_keybind_index(const char* action);
int         eden_keybind_get(int i);
int         eden_keybind_default(int i);
int         eden_keybind_native_hidden(int i);
const char* eden_keybind_action(int i);
int         eden_keybind_action_after(int prev, int code);
void        eden_keybind_set(int i, int code);
}

namespace console_test {

struct TestSink : CmdScript::Sink {
    std::vector<std::pair<int, std::string>> lines;
    void line(int kind, const std::string& text) override { lines.push_back(std::make_pair(kind, text)); }
    bool has(const std::string& t) const {
        for (const auto& l : lines) if (l.second == t) return true;
        return false;
    }
    std::string all() const {
        std::string r;
        for (const auto& l : lines) r += (r.empty() ? "" : " | ") + l.second;
        return r;
    }
};

// The stand-in for the C.2 registry: three verbs, one dotted like a cvar.
const char* const kTestVerbs[] = {"echo", "help", "video.fov"};
int g_extCalls = 0, g_extLast = -1, g_extAnchor[3] = {0, 0, 0};
std::vector<std::string> g_extArgs;
int test_ext_find(const char* v) {
    for (int i = 0; i < 3; ++i) if (!std::strcmp(v, kTestVerbs[i])) return i;
    return -1;
}
void test_ext_run(int i, const CmdScript::ExtCall& c) {
    g_extCalls++;
    g_extLast = i;
    g_extArgs = c.args;
    g_extAnchor[0] = c.ax; g_extAnchor[1] = c.ay; g_extAnchor[2] = c.az;
    if (c.sink) c.sink->line(CmdScript::Sink::OUT, std::string("ext ") + kTestVerbs[i]);
}
const CmdScript::Ext kTestExt = {test_ext_find, test_ext_run};

bool stmt_eq(const CmdScript::Stmt& a, const CmdScript::Stmt& b) {
    if (a.op != b.op || a.type != b.type || a.color != b.color || a.hasPos != b.hasPos || a.f != b.f ||
        a.text != b.text || a.ext != b.ext || a.args != b.args) return false;
    for (int i = 0; i < 6; ++i) if (a.v[i] != b.v[i] || a.abs[i] != b.abs[i]) return false;
    return true;
}
bool any_abs(const CmdScript::Stmt& s) {
    for (int i = 0; i < 6; ++i) if (s.abs[i]) return true;
    return false;
}

// Gate 1 on one corpus line; "" if it holds, else why not.
std::string corpus_line(char cls, const std::string& src) {
    CmdScript::Program blk, con;
    CmdScript::parse(src.c_str(), &blk);
    CmdScript::parseLine(src.c_str(), &con, CmdScript::consoleOptions(&kTestExt));
    const bool bok = blk.errors == 0, cok = con.errors == 0;
    const char got = bok && cok ? 'B' : bok ? 'b' : cok ? 'c' : 'x';
    if (got != cls)
        return std::string("is '") + got + "' (block: " + (bok ? "ok" : blk.firstError) + "; console: " +
               (cok ? "ok" : con.firstError) + ")";
    const auto& b = blk.stmts;
    const auto& c = con.stmts;
    if (cls == 'B') {
        if (b.size() != c.size()) return "statement counts differ";
        for (size_t i = 0; i < b.size(); ++i)
            if (!stmt_eq(b[i], c[i])) return "statement " + std::to_string(i + 1) + " differs";
    } else if (cls == 'b') {
        size_t k = src.find_first_not_of(" \t;");
        if (k == std::string::npos || src[k] != '@') return "a blocks-only line without a trigger header";
        if (con.errors != 1 || con.firstError != "Triggers only work in a command block")
            return "console error is '" + con.firstError + "'";
        if (c.size() != b.size() + 1) return "statement counts differ";
        for (size_t i = 0; i < b.size(); ++i)
            if (!stmt_eq(b[i], c[i + 1])) return "statement " + std::to_string(i + 2) + " differs";
    } else if (cls == 'c') {
        if (b.size() != c.size()) return "statement counts differ";
        for (size_t i = 0; i < b.size(); ++i) {
            if (b[i].op == CmdScript::OP_BAD) {
                if (c[i].op != CmdScript::OP_EXT && !any_abs(c[i]))
                    return "statement " + std::to_string(i + 1) + " is console-only without `=` or a registry verb";
            } else if (!stmt_eq(b[i], c[i])) {
                return "statement " + std::to_string(i + 1) + " differs";
            }
        }
    } else if (b.size() == c.size()) {
        for (size_t i = 0; i < b.size(); ++i)
            if (b[i].op != CmdScript::OP_BAD && c[i].op != CmdScript::OP_BAD && !stmt_eq(b[i], c[i]))
                return "statement " + std::to_string(i + 1) + " differs";
    }
    return "";
}

// One argument a verb accepts, by kind.
const char* sample(int kind) {
    switch (kind) {
    case CmdScript::ARG_COORD: return "1";
    case CmdScript::ARG_BLOCK: return "stone";
    case CmdScript::ARG_COLOR: return "7";
    case CmdScript::ARG_CREATURE: return "moof";
    case CmdScript::ARG_NUMBER: return "1";   // sound takes whole numbers only
    default: return "hi";
    }
}

// Gate 2's snapshot: the site's cells, the player, the purse, health, creatures, the sky, the toast.
struct Snap {
    std::vector<int> cells;
    float px, py, pz;
    int gold, creatures;
    float life;
    bool night;
    std::string toast;
};

}  // namespace console_test

int run_console_selftest() {
    using namespace cmds_test;
    using namespace console_test;
    using CS = CmdScript;
    g_selftestTag = "eden-console";
    CS::testAnchorFeet = g_opt.consoleMutate == "anchor";
    Console::testNoBridge = g_opt.consoleMutate == "noreg";
    CS::trace = g_opt.cmdsTrace;
    char d[1024];

    // ---- the shared literal-script table (block options)
    cmds_parser_cases();

    // ---- gate 1: the corpus
    {
        std::FILE* f = std::fopen(g_opt.consoleCorpus.c_str(), "rb");
        check(f != NULL, "gate 1: the corpus opens", g_opt.consoleCorpus.c_str());
        int lines = 0, bad = 0, counts[4] = {0, 0, 0, 0};
        std::string why;
        char buf[2048];
        while (f && std::fgets(buf, sizeof(buf), f)) {
            std::string l(buf);
            while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();   // CRLF checkouts
            if (l.empty() || l[0] == '#') continue;
            const char cls = l[0];
            const char* kinds = "Bbcx";
            const char* at = std::strchr(kinds, cls);
            if (!at || (l.size() > 1 && l[1] != ' ')) { ++bad; why += "unreadable: " + l + "; "; continue; }
            const std::string src = l.size() > 2 ? l.substr(2) : std::string();
            ++lines;
            counts[at - kinds]++;
            const std::string r = corpus_line(cls, src);
            if (!r.empty()) {
                ++bad;
                if (bad <= 6) why += "'" + src + "' " + r + "; ";
            }
        }
        if (f) std::fclose(f);
        std::snprintf(d, sizeof(d), "%d lines (B %d, b %d, c %d, x %d), %d wrong %s", lines, counts[0], counts[1],
                      counts[2], counts[3], bad, why.c_str());
        check(f && lines >= 100 && bad == 0 && counts[0] && counts[1] && counts[2] && counts[3],
              "gate 1: block and console parse the corpus alike", d);
    }

    // ---- the verb table is the parser's: every row parses, every "needs" error is the row's
    {
        size_t n = 0;
        const CS::Verb* vs = CS::verbs(&n);
        std::string why;
        for (size_t i = 0; i < n; ++i) {
            const CS::Verb& v = vs[i];
            std::string line = v.name;
            for (int k = 0; k < 9 && v.kinds[k]; ++k) line += std::string(" ") + sample(v.kinds[k]);
            CS::Program p;
            CS::parse(line.c_str(), &p);
            if (p.errors || p.stmts.size() != 1) why += line + " -> " + p.firstError + "; ";
            if (*v.usage && std::strcmp(v.name, "say")) {
                CS::parse(v.name, &p);
                const std::string want = std::string(v.name) + " needs " + (v.needs ? v.needs : v.usage);
                if (p.errors != 1 || p.firstError != want) why += std::string(v.name) + " -> '" + p.firstError + "' not '" + want + "'; ";
            }
            if (v.kinds[0] == CS::ARG_COORD || v.kinds[0] == CS::ARG_CREATURE) {
                line = std::string(v.name) + (v.kinds[0] == CS::ARG_CREATURE ? " moof 1" : " 1");
                CS::parse(line.c_str(), &p);
                const std::string want = std::string(v.name) + " needs " + v.usage;
                if (p.firstError != want) why += line + " -> '" + p.firstError + "' not '" + want + "'; ";
            }
        }
        std::snprintf(d, sizeof(d), "%zu verbs %s", n, why.c_str());
        check(n == 22 && why.empty(), "kVerbs: every verb parses; its errors are its usage", d);
        check(CS::findVerb("FILL") && CS::findVerb("Unless") && !CS::findVerb("video.fov") && !CS::findVerb("echo"),
              "findVerb: case-insensitive, built-ins only", "");
    }

    // ---- C.9 / C.10: the player's README and the commands reference name nothing that is gone
    {
        Console::init();
        // every `backticked` token whose first word starts with a letter must be a verb or a variable
        // (a ';' separates statements); the verbs the roadmap promises must be named
        const std::string text = eden_player_readme_text();
        std::string why;
        std::set<std::string> named;
        for (size_t at = text.find('`'); at != std::string::npos;) {
            const size_t end = text.find('`', at + 1);
            if (end == std::string::npos) { why += "unbalanced backtick; "; break; }
            std::string tok = text.substr(at + 1, end - at - 1);
            at = text.find('`', end + 1);
            size_t p = 0;
            while (p <= tok.size()) {
                size_t q = tok.find(';', p);
                if (q == std::string::npos) q = tok.size();
                std::string st = tok.substr(p, q - p);
                p = q + 1;
                size_t b = st.find_first_not_of(' ');
                if (b == std::string::npos || !std::isalpha((unsigned char)st[b])) continue;
                const std::string w = st.substr(b, st.find(' ', b) == std::string::npos ? std::string::npos : st.find(' ', b) - b);
                named.insert(w);
                if (!CS::findVerb(w.c_str()) && Console::cvarIndex(w.c_str()) < 0) {
                    bool reg = false;
                    for (size_t i = 0; i < Console::commandCount(); ++i)
                        if (w == Console::commandAt(i).name) reg = true;
                    if (!reg) why += "`" + w + "` ; ";
                }
            }
        }
        for (const char* must : {"help", "set", "fill", "tp", "day", "night", "spawn", "video.fov", "toggle", "reset", "where", "anchor"})
            if (!named.count(must)) why += std::string("missing ") + must + "; ";
        check(why.empty(), "README.txt: every command and variable it names exists, the promised ones are named", why.c_str());
        const std::string rd = std::string(eden_platform_documents_root()) + "/readme-test";
        mkdirs(rd);
        std::remove((rd + "/README.txt").c_str());
        const int w1 = eden_player_readme_write(rd.c_str()), w2 = eden_player_readme_write(rd.c_str());
        std::string back;
        if (std::FILE* f = std::fopen((rd + "/README.txt").c_str(), "rb")) {
            char b[512]; size_t n;
            while ((n = std::fread(b, 1, sizeof(b), f)) > 0) back.append(b, n);
            std::fclose(f);
        }
        {   // a stale version line is rewritten; a current file with edits is left alone
            if (std::FILE* f = std::fopen((rd + "/README.txt").c_str(), "wb")) { std::fputs("Emod README v0\nold\n", f); std::fclose(f); }
        }
        const int w3 = eden_player_readme_write(rd.c_str());
        std::snprintf(d, sizeof(d), "first %d, again %d, after a stale one %d, bytes %zu", w1, w2, w3, back.size());
        check(w1 == 1 && w2 == 0 && w3 == 1 && back == text, "README.txt: written once, idempotent, rewritten only when its version changes", d);

        // docs/console-reference.md: every verb, console verb and variable group appears in it
        std::string doc;
        if (std::FILE* f = std::fopen(g_opt.consoleRef.c_str(), "rb")) {
            char b[4096]; size_t n;
            while ((n = std::fread(b, 1, sizeof(b), f)) > 0) doc.append(b, n);
            std::fclose(f);
        }
        std::string miss;
        size_t nv = 0;
        const CS::Verb* vs = CS::verbs(&nv);
        for (size_t i = 0; i < nv; ++i)
            if (doc.find(std::string("`") + vs[i].name + "`") == std::string::npos) miss += std::string(vs[i].name) + " ";
        for (size_t i = 0; i < Console::commandCount(); ++i)
            if (doc.find(std::string("`") + Console::commandAt(i).name + "`") == std::string::npos) miss += std::string(Console::commandAt(i).name) + " ";
        std::set<std::string> groups;
        for (size_t i = 0; i < Console::cvarCount(); ++i) {
            const std::string n = Console::cvarAt(i)->name;
            groups.insert(n.substr(0, n.find('.') + 1));
            if (doc.find("`" + n + "`") == std::string::npos) miss += n + " ";
        }
        for (const std::string& g : groups)
            if (doc.find("`" + g + "`") == std::string::npos) miss += g + " ";
        std::snprintf(d, sizeof(d), "%s: %zu bytes; missing: %s (run port/tools/console-reference.py)", g_opt.consoleRef.c_str(), doc.size(), miss.c_str());
        check(!doc.empty() && miss.empty(), "console-reference.md names every verb, variable and group", d);
    }

    // ---- what only the options change
    {
        CS::Program b, c;
        CS::parse("tp =1 0 0", &b);
        CS::parseLine("tp =1 0 0", &c, CS::consoleOptions(&kTestExt));
        std::snprintf(d, sizeof(d), "block '%s', console errors %d", b.firstError.c_str(), c.errors);
        check(b.firstError == "Not a coordinate: =1" && c.errors == 0 && c.stmts[0].abs[0] && !c.stmts[0].abs[1],
              "`=N` is a console coordinate only", d);
        CS::parseLine("fill =1 2 =3 4 =5 6 stone", &c, CS::consoleOptions(&kTestExt));
        const CS::Stmt& s = c.stmts[0];
        check(c.errors == 0 && s.abs[0] && !s.abs[1] && s.abs[2] && !s.abs[3] && s.abs[4] && !s.abs[5] && s.v[0] == 1 &&
              s.v[5] == 6, "abs[] marks exactly the `=` coordinates", c.firstError.c_str());
        CS::parse("echo hi", &b);
        CS::parseLine("video.fov 95", &c, CS::consoleOptions(&kTestExt));
        std::snprintf(d, sizeof(d), "block '%s'; console op %d ext %d args %zu", b.firstError.c_str(), c.stmts[0].op,
                      c.stmts[0].ext, c.stmts[0].args.size());
        check(b.firstError == "Unknown command: echo" && c.stmts[0].op == CS::OP_EXT && c.stmts[0].ext == 2 &&
              c.stmts[0].args == std::vector<std::string>{"95"} && c.ext == &kTestExt,
              "registry verbs are OP_EXT in the console, unknown in a block", d);
        CS::parseLine("@step;say hi", &c, CS::consoleOptions(&kTestExt));
        check(c.errors == 1 && c.firstError == "Triggers only work in a command block", "the console refuses triggers",
              c.firstError.c_str());
    }

    // ---- runLine before a world: the parse refusals still come first, then "No world is open"
    {
        TestSink sk;
        const int rBad = CS::runLine("set 1 1 1 glass; fill 0 0 0 9 2 9 glas; frob", 0, 0, 0, CS::SRC_CONSOLE, &sk, &kTestExt);
        const bool msgs = sk.has("2: Unknown block: glas") && sk.has("3: Unknown command: frob") && sk.has("line not run");
        const std::string longLine(512, 'x');
        const int rLong = CS::runLine(longLine.c_str(), 0, 0, 0, CS::SRC_CONSOLE, &sk, &kTestExt);
        const int rEmpty = CS::runLine(" ;; ", 0, 0, 0, CS::SRC_CONSOLE, &sk, &kTestExt);
        const int rNone = CS::runLine("say hi", 0, 0, 0, CS::SRC_CONSOLE, &sk, &kTestExt);
        std::snprintf(d, sizeof(d), "bad %d long %d empty %d noworld %d | %s", rBad, rLong, rEmpty, rNone, sk.all().c_str());
        check(rBad == CS::RUN_BAD && msgs && rLong == CS::RUN_LONG && rEmpty == CS::RUN_EMPTY &&
              rNone == CS::RUN_NO_WORLD && sk.has("No world is open"),
              "runLine: a bad line lists every error and runs nothing", d);
    }

    // ---- a world
    const char* name = g_opt.world.empty() ? "console-selftest" : g_opt.world.c_str();
    if (!open_world(name, g_opt.height)) return 1;
    if (g_opt.haveAt) eden_console_teleport(g_opt.at[0], g_opt.at[1], g_opt.at[2]);
    tick(g_opt.frames / 4);
    if (GLDialog::active()) { GLDialog::dismiss(); tick(2); }
    g_app->viewController.setFixedEtime(1.0f / 60.0f);
    Hud* hud = World::getWorld->hud;
    Player* pl = World::getWorld->player;
    const PlayerState ps = player_state();
    int ox, oy, oz;
    if (!open_air((int)ps.x - 50, (int)ps.z, 16, 12, 16, &ox, &oy, &oz)) { check(false, "an open-air test site", ""); return 1; }
    const int PX = ox + 8, PY = oy + 3, PZ = oz + 8;   // the command block P, engine order
    std::printf("[eden-console] site %d,%d,%d, P %d,%d,%d (engine order)\n", ox, oy, oz, PX, PY, PZ);
    void (*const defaultTime)(float) = CS::timeHook;

    // Every gate-2 run starts from the same site: the box air, P holding `script`, stone at +2, grass at
    // +3, a command block at +5 (run's target), the player standing on P, purse 10, life 0.5, day sky.
    auto setup = [&](const char* script) {
        CS::stop(CS::STOP_ALL);
        tick(1);
        for (int a = 0; a < 16; ++a)
            for (int b = 0; b < 12; ++b)
                for (int c = 0; c < 16; ++c)
                    if (land(ox + a, oy + b, oz + c) != 0) put(ox + a, oy + b, oz + c, 0);
        put(PX + 2, PY, PZ, TYPE_STONE);
        ter()->setColor(PX + 2, PZ, PY, 0);
        put(PX + 3, PY, PZ, TYPE_GRASS);
        ter()->setColor(PX + 3, PZ, PY, 0);
        make(PX + 5, PY, PZ, "set 0 2 0 brick 7");
        const TrailerPos p = make(PX, PY, PZ, script);
        pl->pos = MakeVector(PX + 0.5f, PY + 1 + pl->boxheight / 2 + 0.001f, PZ + 0.5f);
        pl->vel = MakeVector(0, 0, 0);
        pl->life = 0.5f;
        hud->goldencubes = 10;
        defaultTime(0.5f);
        return p;
    };
    auto snap = [&](Snap* s, int creatures0) {
        s->cells.clear();
        for (int a = 0; a < 16; ++a)
            for (int b = 0; b < 12; ++b)
                for (int c = 0; c < 16; ++c)
                    s->cells.push_back(land(ox + a, oy + b, oz + c) * 256 + paint(ox + a, oy + b, oz + c));
        s->gold = hud->goldencubes;
        s->life = pl->life;
        (void)creatures0;
        s->night = v_equals(ter()->final_skycolor, colorTable[54]);
    };
    auto diff = [&](const Snap& a, const Snap& b) {
        std::string r;
        for (size_t i = 0; i < a.cells.size() && r.empty(); ++i)
            if (a.cells[i] != b.cells[i]) {
                const int a0 = (int)i / (12 * 16), b0 = ((int)i / 16) % 12, c0 = (int)i % 16;
                r = "cell P+" + std::to_string(ox + a0 - PX) + "," + std::to_string(oy + b0 - PY) + "," +
                    std::to_string(oz + c0 - PZ) + " " + std::to_string(a.cells[i] >> 8) + ":" + std::to_string(a.cells[i] & 255) +
                    " vs " + std::to_string(b.cells[i] >> 8) + ":" + std::to_string(b.cells[i] & 255);
            }
        if (std::fabs(a.px - b.px) > 0.001f || std::fabs(a.py - b.py) > 0.001f || std::fabs(a.pz - b.pz) > 0.001f) r += " player";
        if (a.gold != b.gold) r += " gold " + std::to_string(a.gold) + " vs " + std::to_string(b.gold);
        if (std::fabs(a.life - b.life) > 1e-4f) r += " life";
        if (a.creatures != b.creatures) r += " creatures " + std::to_string(a.creatures) + " vs " + std::to_string(b.creatures);
        if (a.night != b.night) r += " sky";
        if (a.toast != b.toast) r += " toast '" + a.toast + "' vs '" + b.toast + "'";
        return r;
    };

    // ---- gate 2: execution equivalence
    {
        const char* scripts[] = {
            "set 0 2 0 glass; fill 1 0 1 3 2 2 brick 19; paint 2 0 0 30",
            "set 0 0 0 glass; set 0 1 0 dirt; set 1 1 0 dirt; set 2 0 0 air",
            "if 2 0 0 stone; set -1 1 0 lamp 9; unless 3 0 0 grass; set -2 1 0 lamp 9",
            "if 2 0 0 grass; if 3 0 0 grass; set 0 3 0 brick; set 0 4 0 brick",
            "run 5 0 0",
            "gold 1; run 0 0 0",
            "tp 0 6 3",
            "gold 7; heal",
            "say HELLO there",
            "fill -3 0 -3 -1 2 -1 cobble 12",
            "set 1 0 -1 glass; wait 0.5; set 1 1 -1 glass; wait 0.2; fill -1 3 -1 1 3 1 ice",
            "spawn moof 4 1 4",
            "night",
            "set 4 0 4 lamp 12; set 4 1 4 lamp 3; set 4 0 4 air",
            "hurt 30; sound 8; flash",
            "fill -2 -1 -2 2 -1 2 stone; fill -1 -1 -1 1 -1 1 air",
        };
        int same = 0, total = 0, changed = 0;
        std::string why;
        bool anchorOk = true;
        for (const char* sc : scripts) {
            ++total;
            Snap A, B, base;
            const bool sayIt = std::strncmp(sc, "say", 3) == 0;
            // As a command block: press P.
            const TrailerPos P0 = setup(sc);
            int c0 = CountActiveCreatures();
            base.px = pl->pos.x; base.py = pl->pos.y; base.pz = pl->pos.z;
            snap(&base, c0);
            base.creatures = 0;
            CS::press(P0);
            // Creatures: right after the run, before a frame lets ambient spawning move the count.
            const int cA = CountActiveCreatures() - c0;
            A.px = pl->pos.x; A.py = pl->pos.y; A.pz = pl->pos.z;
            A.toast = sayIt ? toast() : "";
            secs(1.5f);
            snap(&A, c0);
            A.creatures = cA;
            if (!diff(base, A).empty()) ++changed;   // (a toast counts: base's is "")
            // Typed, standing on P.
            setup(sc);
            c0 = CountActiveCreatures();
            int ax, ay, az;
            CS::playerAnchor(&ax, &ay, &az);
            anchorOk = anchorOk && ax == PX && ay == PY && az == PZ;
            TestSink sk;
            const int r = CS::runLine(sc, ax, ay, az, CS::SRC_CONSOLE, &sk, NULL);
            const int cB = CountActiveCreatures() - c0;
            B.px = pl->pos.x; B.py = pl->pos.y; B.pz = pl->pos.z;
            B.toast = sayIt ? toast() : "";
            secs(1.5f);
            snap(&B, c0);
            B.creatures = cB;
            const std::string dd = r < CS::RUN_DONE ? "runLine " + std::to_string(r) + " " + sk.all() : diff(A, B);
            if (dd.empty()) ++same;
            else why += std::string("'") + sc + "': " + dd + "; ";
        }
        std::snprintf(d, sizeof(d), "%d/%d identical, %d changed the world %s", same, total, changed, why.c_str());
        // Only "hurt 30; sound 8; flash" leaves nothing to compare (Health is off in a new world).
        check(same == total && changed >= total - 1, "gate 2: a typed line = the same script pressed on a block", d);
        check(anchorOk, "the anchor is the cell under the player's feet", "");
    }

    // ---- runLine in play
    {
        // a line with a bad statement runs nothing, even its good ones
        setup("");
        TestSink sk;
        int r = CS::runLine("set 1 1 1 glass; fill 0 0 0 9 2 9 glas", PX, PY, PZ, CS::SRC_CONSOLE, &sk, NULL);
        std::snprintf(d, sizeof(d), "r %d cell %d | %s", r, land(PX + 1, PY + 1, PZ + 1), sk.all().c_str());
        check(r == CS::RUN_BAD && land(PX + 1, PY + 1, PZ + 1) == 0 && sk.has("2: Unknown block: glas") && sk.has("line not run"),
              "in play: a bad line changes nothing", d);

        // `=` absolute, mixed with relative, from an anchor far away
        sk.lines.clear();
        char line[160];
        std::snprintf(line, sizeof(line), "set =%d =%d =%d glass; set =%d 5 =%d brick", PX + 1, PY + 4, PZ, PX, PZ);
        r = CS::runLine(line, PX - 40, PY, PZ, CS::SRC_CONSOLE, &sk, NULL);
        std::snprintf(d, sizeof(d), "r %d glass %d brick %d | %s", r, land(PX + 1, PY + 4, PZ), land(PX, PY + 5, PZ), sk.all().c_str());
        check(r == CS::RUN_DONE && land(PX + 1, PY + 4, PZ) == TYPE_GLASS && land(PX, PY + 5, PZ) == TYPE_BRICK &&
              sk.has("2 cells set, 0 refused"), "`=` coordinates are absolute; the rest count from the anchor", d);

        // a job: prints its cells, waits, finishes, says so
        sk.lines.clear();
        r = CS::runLine("set 1 0 1 glass; wait 0.3; set 1 1 1 glass", PX, PY, PZ, CS::SRC_CONSOLE, &sk, NULL);
        const bool waiting = r > 0 && land(PX + 1, PY + 1, PZ + 1) == 0 && CS::jobs().size() == 1 && CS::liveCount() == 0;
        secs(0.6f);
        const std::string jn = "[job " + std::to_string(r) + "]";
        std::snprintf(d, sizeof(d), "r %d waiting %d cell %d jobs %zu | %s", r, waiting, land(PX + 1, PY + 1, PZ + 1),
                      CS::jobs().size(), sk.all().c_str());
        check(waiting && land(PX + 1, PY + 1, PZ + 1) == TYPE_GLASS && CS::jobs().empty() && sk.has("1 cell set, 0 refused") &&
              sk.has(jn + " waiting 0.3 s") && sk.has(jn + " done"),
              "a waiting line is a job outside the 32; it finishes and says so", d);

        // four jobs at most; a line with no wait still runs; stop N, stop jobs, stop all
        sk.lines.clear();
        int ids[4];
        for (int i = 0; i < 4; ++i) ids[i] = CS::runLine("wait 5; set 0 6 0 brick", PX, PY, PZ, CS::SRC_CONSOLE, &sk, NULL);
        const int r5 = CS::runLine("wait 1", PX, PY, PZ, CS::SRC_CONSOLE, &sk, NULL);
        const int rNo = CS::runLine("set 1 6 0 brick", PX, PY, PZ, CS::SRC_CONSOLE, &sk, NULL);
        const size_t j4 = CS::jobs().size();
        const int s1 = CS::stop(ids[0]);
        const size_t j3 = CS::jobs().size();
        const TrailerPos Q = make(PX - 3, PY, PZ, "wait 5; set 0 1 0 brick");
        CS::press(Q);
        const int sJobs = CS::stop(CS::STOP_JOBS);
        const bool qBusy = CS::busy(Q);
        CS::runLine("wait 5", PX, PY, PZ, CS::SRC_CONSOLE, &sk, NULL);
        const int sAll = CS::stop(CS::STOP_ALL);
        secs(6.0f);
        std::snprintf(d, sizeof(d), "ids %d %d %d %d, 5th %d, nowait %d, jobs %zu -> stop1 %d -> %zu, stopjobs %d (block busy %d), "
                      "stopall %d; cells %d %d %d", ids[0], ids[1], ids[2], ids[3], r5, rNo, j4, s1, j3, sJobs, qBusy, sAll,
                      land(PX, PY + 6, PZ), land(PX + 1, PY + 6, PZ), land(PX - 3, PY + 1, PZ));
        check(ids[0] > 0 && ids[3] == ids[0] + 3 && r5 == CS::RUN_FULL && rNo == CS::RUN_DONE && j4 == 4 && s1 == 1 &&
              j3 == 3 && sJobs == 3 && qBusy && sAll == 2 && !CS::busy(Q) && CS::jobs().empty() &&
              land(PX, PY + 6, PZ) == 0 && land(PX + 1, PY + 6, PZ) == TYPE_BRICK && land(PX - 3, PY + 1, PZ) == 0 &&
              sk.has("[job " + std::to_string(ids[0]) + "] stopped"),
              "jobs: 4 at most, stop N / jobs / all; blocks only stop with all", d);

        // say: a toast and a line; a `run` with no block there says so
        sk.lines.clear();
        CS::runLine("say ping; run 9 9 9", PX, PY, PZ, CS::SRC_CONSOLE, &sk, NULL);
        std::snprintf(d, sizeof(d), "toast '%s' | %s", toast().c_str(), sk.all().c_str());
        char none[64];
        std::snprintf(none, sizeof(none), "No command block at %d %d %d", PX + 9, PY + 9, PZ + 9);
        check(toast() == "ping" && sk.has("ping") && sk.has(none), "say toasts and prints; a missing run target is reported", d);

        // the registry hook: OP_EXT runs with the anchor and the words, and `if` gates it
        sk.lines.clear();
        const int calls0 = g_extCalls;
        r = CS::runLine("echo hi there; if 2 0 0 grass; echo no", PX, PY, PZ, CS::SRC_CONSOLE, &sk, &kTestExt);
        std::snprintf(d, sizeof(d), "r %d calls %d last %d args %zu anchor %d,%d,%d | %s", r, g_extCalls - calls0, g_extLast,
                      g_extArgs.size(), g_extAnchor[0], g_extAnchor[1], g_extAnchor[2], sk.all().c_str());
        check(r == CS::RUN_DONE && g_extCalls == calls0 + 1 && g_extLast == 0 &&
              g_extArgs == std::vector<std::string>{"hi", "there"} && g_extAnchor[0] == PX && g_extAnchor[1] == PY &&
              g_extAnchor[2] == PZ && sk.has("ext echo"), "OP_EXT calls the registry with the anchor and its words", d);

        // the time hook: blocks and the console go through it together
        static std::vector<float> times;
        times.clear();
        CS::timeHook = [](float t) { times.push_back(t); };
        CS::runLine("night; time 0.3", PX, PY, PZ, CS::SRC_CONSOLE, &sk, NULL);
        CS::press(make(PX - 3, PY, PZ + 3, "midnight"));
        tick(1);
        CS::timeHook = defaultTime;
        std::snprintf(d, sizeof(d), "%zu calls: %.2f %.2f %.2f", times.size(), times.size() > 0 ? times[0] : -1.0f,
                      times.size() > 1 ? times[1] : -1.0f, times.size() > 2 ? times[2] : -1.0f);
        check(times.size() == 3 && times[0] == 0.75f && std::fabs(times[1] - 0.3f) < 1e-6f && times[2] == 0.0f,
              "day/night/time call CmdScript::timeHook from a line and a block", d);
        setup("");
    }

    // ---- gates 3-5 (C.2): cvars, completion / help / history, gating
    {
        using namespace Console;
        init();
        auto lowerS = [](std::string t) { for (char& ch : t) ch = (char)std::tolower((unsigned char)ch); return t; };
        auto numS = [](double v) { char b[48]; std::snprintf(b, sizeof(b), "%g", v); return std::string(b); };
        // Run one console line; the output is the scrollback it left.
        auto con = [&](const char* line, std::string* out = NULL) {
            Console::clear();
            const int r = Console::run(line);
            if (out) {
                out->clear();
                for (const Line& l : scrollback()) *out += (out->empty() ? "" : " | ") + l.text;
            }
            return r;
        };
        auto has = [](const std::string& hay, const char* needle) { return hay.find(needle) != std::string::npos; };
        const bool diag = true;
        std::string out;
        // The player stands on P (setup), stone at +2 and grass at +3, so the anchor is P.
        setup("");

        // gate 3: every kSettings row is a cvar named group.key with the row's metadata
        {
            std::string why;
            int rows = 0, present = 0;
            for (int i = 0; i < eden_settings_count(); ++i) {
                if (eden_settings_kind(i) == 4) continue;   // MP.8: KIND_TEXT rows are not cvars
                const std::string name = lowerS(eden_settings_group(i)) + "." + eden_settings_key(i);
                const int ci = cvarIndex(name.c_str());
                const bool expect = diag || !eden_settings_native_hidden(i);
                ++rows;
                if (ci < 0) { if (expect) why += name + " missing; "; continue; }
                if (!expect) { why += name + " should be absent; "; continue; }
                ++present;
                const Cvar* c = cvarAt(ci);
                if (c->kind != eden_settings_kind(i) || c->min != eden_settings_min(i) || c->max != eden_settings_max(i) ||
                    c->def != eden_settings_def(i) || c->apply != eden_settings_apply(i)) why += name + " metadata; ";
                const float v = eden_settings_get(i);
                const std::string want = c->kind == KIND_TOGGLE ? (v != 0 ? "on" : "off")
                                       : c->kind == KIND_ENUM ? std::string(eden_settings_enum_label(i, (int)std::lround(v)))
                                       : numS(v);
                if (cvarValue(ci) != want) why += name + " value '" + cvarValue(ci) + "' not '" + want + "'; ";
            }
            std::snprintf(d, sizeof(d), "%d rows, %d cvars %s", rows, present, why.substr(0, 300).c_str());
            check(why.empty() && rows >= 40, "gate 3: every Settings row is a cvar group.key with its metadata", d);
        }

        // set / clamp / persist (through eden_settings_set), print, enum by label and by index
        {
            const int fov = cvarIndex("video.fov"), sh = cvarIndex("video.shadows");
            const int fovRow = [&] { for (int i = 0; i < eden_settings_count(); ++i) if (!std::strcmp(eden_settings_key(i), "fov")) return i; return -1; }();
            const int shRow = [&] { for (int i = 0; i < eden_settings_count(); ++i) if (!std::strcmp(eden_settings_key(i), "shadows")) return i; return -1; }();
            check(fov >= 0 && sh >= 0 && fovRow >= 0 && shRow >= 0, "video.fov and video.shadows exist", "");
            if (fov >= 0 && sh >= 0 && fovRow >= 0 && shRow >= 0) {
                std::string o1, o2, o3, o4, o5, o6;
                con("video.fov", &o1);
                con("video.fov 95", &o2);
                const float after95 = eden_settings_get(fovRow);
                con("video.fov 500", &o3);
                const float after500 = eden_settings_get(fovRow);
                con("video.fov abc", &o4);
                std::snprintf(d, sizeof(d), "'%s' | '%s' (%g) | '%s' (%g) | '%s'", o1.c_str(), o2.c_str(), after95, o3.c_str(), after500, o4.c_str());
                check(has(o1, "video.fov = 80") && has(o1, "60..110") && has(o1, "default 80") && after95 == 95 &&
                      has(o2, "video.fov = 95") && after500 == 110 && has(o3, "clamped to 110") && has(o4, "takes a number"),
                      "cvars: read, set, clamp like the slider, refuse text", d);
                con("reset video.fov", &o1);
                const bool fovBack = eden_settings_get(fovRow) == 80;

                con("video.shadows smooth", &o1);
                const float byLabel = eden_settings_get(shRow);
                con("video.shadows 1", &o2);
                const float byIndex = eden_settings_get(shRow);
                con("video.shadows CLASSIC", &o3);
                const float byCase = eden_settings_get(shRow);
                con("video.shadows smooth", &o4);
                con("toggle video.shadows", &o5);
                const float wrapped = eden_settings_get(shRow);
                con("video.shadows bogus", &o6);
                std::snprintf(d, sizeof(d), "label %g index %g case %g wrap %g | '%s' | '%s'", byLabel, byIndex, byCase, wrapped, o5.c_str(), o6.c_str());
                check(fovBack && byLabel == 2 && byIndex == 1 && byCase == 1 && wrapped == 0 && has(o6, "takes one of"),
                      "cvars: an enum by label (any case) or index; toggle steps and wraps; reset restores", d);
                con("reset video.shadows", &o1);
            }
            const int ap = cvarIndex("saves.autosave"), rd = cvarIndex("video.render_radius");
            if (ap >= 0 && rd >= 0) {
                std::string o1, o2;
                con("saves.autosave 2min", &o1);
                const bool two = cvarValue(ap) == "2 min";
                con("video.render_radius 12", &o2);
                const bool next = has(o2, "applies when a world opens") && cvarValue(rd) == "12";
                std::snprintf(d, sizeof(d), "autosave '%s' | '%s'", cvarValue(ap).c_str(), o2.c_str());
                check(two && next, "cvars: `2min` finds `2 min`; render_distance reports 'applies when a world opens'", d);
                con("reset saves.autosave", &o1);
                con("reset video.render_radius", &o1);
            } else check(false, "saves.autosave and video.render_radius exist", "");
            std::string o1, o2, o3;
            con("toggle gameplay.health", &o1);
            const bool flipped = cvarValue(cvarIndex("gameplay.health")) == "off";
            con("toggle gameplay.health", &o2);
            con("toggle video.fov", &o3);
            check(flipped && cvarValue(cvarIndex("gameplay.health")) == "on" && has(o3, "not an on/off"),
                  "toggle flips an on/off variable and refuses a range", o3.c_str());
        }

        // keybinds are cvars: rebind, and the old key stops (both directions)
        {
            const int jump = eden_keybind_index("jump");
            const int def = jump >= 0 ? eden_keybind_default(jump) : 0;
            std::string o1, o2, o3;
            con("keys.jump", &o1);
            con("keys.jump F9", &o2);
            const int nowF9 = jump >= 0 ? eden_keybind_get(jump) : -1;
            const bool oldStops = jump >= 0 && eden_keybind_action_after(-1, def) != jump;
            const bool newFires = jump >= 0 && eden_keybind_action_after(-1, nowF9) == jump;
            con("keys.jump nonsense", &o3);
            con("reset keys.jump", &o1);
            const bool back = jump >= 0 && eden_keybind_get(jump) == def && eden_keybind_action_after(-1, def) == jump;
            std::snprintf(d, sizeof(d), "now %d old stops %d new fires %d back %d | '%s' | '%s'", nowF9, oldStops, newFires, back, o2.c_str(), o3.c_str());
            check(jump >= 0 && nowF9 != def && oldStops && newFires && back && has(o2, "keys.jump = F9") && has(o3, "Not a key name"),
                  "keys.jump rebinds; the old key stops; reset restores", d);
        }

        // world.* / gen.*: read-only ones refuse; world.gold is live; the name is live
        {
            std::string o1, o2, o3;
            const int seed0 = ter()->level_seed;
            con("world.seed 5", &o1);
            con("gen.caves deep", &o2);
            std::snprintf(d, sizeof(d), "'%s' | '%s' | seed %d", o1.c_str(), o2.c_str(), ter()->level_seed);
            check(has(o1, "read-only") && has(o2, "World Properties") && ter()->level_seed == seed0,
                  "world.seed and gen.* are read-only in game and point at World Properties", d);
            con("world.gold 50", &o1);
            const int g50 = hud->goldencubes;
            con("world.gold 5000", &o2);
            const int g999 = hud->goldencubes;
            hud->goldencubes = 10;
            check(g50 == 50 && g999 == 999 && has(o2, "clamped to 999"), "world.gold is the purse, clamped to 0..999", o2.c_str());
            con("world.height", &o1);
            con("world.type", &o2);
            con("world.cmdblocks", &o3);
            std::snprintf(d, sizeof(d), "'%s' | '%s' | '%s'", o1.c_str(), o2.c_str(), o3.c_str());
            check(has(o1, std::to_string(g_world_height).c_str()) && has(o2, "world.type = ") && has(o3, "world.cmdblocks = 2"),
                  "world.height / type / cmdblocks read the open world", d);
        }

        // gate 4: completion, help, history
        {
            Completion f = complete("fi");
            std::string got;
            for (const std::string& m : f.matches) got += (got.empty() ? "" : " ") + m;
            check(got == "fill find fire", "completion: `fi` lists fill find fire", got.c_str());
            Completion v = complete("video.sha");
            check(v.line == "video.shadows ", "completion: `video.sha` -> video.shadows", v.line.c_str());
            Completion b = complete("set 0 0 0 gla");
            check(b.line == "set 0 0 0 glass ", "completion: block names by argument kind", b.line.c_str());
            Completion e = complete("gold 1; video.shadows sm");
            check(e.line == "gold 1; video.shadows Smooth ", "completion: after `;`, a cvar's enum labels", e.line.c_str());
            Completion k = complete("keys.jump F1");
            std::string km;
            for (const std::string& m : k.matches) km += m + " ";
            check(k.matches.size() >= 2 && k.line == "keys.jump F1", "completion: key labels for a keys.* variable (F1, F10, F11, F12)", km.c_str());

            std::string o;
            con("help set", &o);
            const CS::Verb* sv = CS::findVerb("set");
            check(sv && has(o, (std::string(sv->name) + " " + sv->usage).c_str()), "help set = the parser's usage string", o.c_str());
            con("help video.fov", &o);
            check(has(o, "video.fov = ") && has(o, "60..110"), "help on a cvar describes it", o.c_str());
            con("help nonsense", &o);
            check(has(o, "No verb or variable named nonsense"), "help on nothing says so", o.c_str());
            con("find shad", &o);
            check(has(o, "video.shadows") && has(o, "shadow"), "find searches names and descriptions", o.c_str());
            con("cvars video", &o);
            check(has(o, "video.fov = 80") && has(o, "video.shadows = "), "cvars GROUP lists the group", o.c_str());

            historyClear();
            std::string last;
            for (int i = 0; i < 60; ++i) { last = "line " + std::to_string(i); historyAdd(last); }
            historyAdd(last);   // a repeat of the last is not stored
            const bool capped = historyCount() == (size_t)MAX_HISTORY && historyGet(0) == "line 10" && historyGet(MAX_HISTORY - 1) == last;
            historySave();
            historyClear();
            historyLoad();
            check(capped && historyCount() == (size_t)MAX_HISTORY && historyGet(0) == "line 10" && historyGet(MAX_HISTORY - 1) == last,
                  "history keeps 50, skips repeats, survives a restart", numS((double)historyCount()).c_str());
            con("echo hello  world", &o);
            check(historyGet(historyCount() - 1) == "echo hello  world" && o == "hello world", "run() records history; echo prints only here", o.c_str());
        }

        // the verbs on the anchor, chaining with block verbs, jobs
        {
            std::string o;
            con("if 2 0 0 stone; echo yes; if 2 0 0 grass; echo no", &o);
            check(o == "yes", "registry verbs chain and gate like block verbs", o.c_str());
            con("anchor 1 0 0", &o);
            int ax, ay, az;
            currentAnchor(&ax, &ay, &az);
            const bool moved = anchorIsFixed() && ax == PX + 1 && ay == PY && az == PZ;
            con("if 1 0 0 stone; echo anchored", &o);
            const bool rel = o == "anchored";
            con("anchor me", &o);
            currentAnchor(&ax, &ay, &az);
            const bool me = !anchorIsFixed() && ax == PX && ay == PY && az == PZ;
            char ln[96];
            std::snprintf(ln, sizeof(ln), "anchor =%d =%d =%d", PX + 3, PY, PZ);
            con(ln, &o);
            currentAnchor(&ax, &ay, &az);
            const bool absolute = anchorIsFixed() && ax == PX + 3;
            con("where", &o);
            const bool where = has(o, "you stand on") && has(o, "anchor ");
            con("anchor me", &o);
            con("anchor look", &o);
            const bool lookRefusedOrSet = has(o, "Nothing in front") || has(o, "anchor ");
            con("anchor me", &o);
            std::snprintf(d, sizeof(d), "moved %d rel %d me %d abs %d where %d look %d", moved, rel, me, absolute, where, lookRefusedOrSet);
            check(moved && rel && me && absolute && where && lookRefusedOrSet, "anchor X Y Z / =N / me / look; where prints cells", d);

            con("wait 5; set 0 6 0 brick", &o);
            std::string jo, so;
            con("jobs", &jo);
            const size_t waiting = CS::jobs().size();
            con("stop all", &so);
            const bool stopped = CS::jobs().empty();
            con("jobs", &o);
            check(waiting == 1 && has(jo, "[job ") && stopped && has(o, "No jobs waiting"), "jobs lists and stop all stops console jobs", jo.c_str());
            con("set 0 3 0 glas", &o);
            check(has(o, "Unknown block: glas") && has(o, "line not run") && land(PX, PY + 3, PZ) == 0,
                  "a bad line through run() prints its errors and changes nothing", o.c_str());
            con("reset all", &o);
            con("video.fov 95", &o);
            con("reset all", &o);
            const int fovRow = cvarIndex("video.fov");
            const bool kept = fovRow >= 0 && cvarValue(fovRow) == "95";
            con("reset video", &o);
            check(kept && cvarValue(fovRow) == "80" && has(o, "reset"), "reset all asks first; reset GROUP resets the group", o.c_str());
        }

        // gate 5: gating and registration rules
        {
            static bool ran;
            ran = false;
            addCommand("cheattest", "", "test", CF_CHEAT, {}, [](Ctx&) { ran = true; });
            std::string o;
            con("cheattest", &o);
            check(!ran && has(o, "Allow cheats"), "a CF_CHEAT verb is refused without Allow cheats", o.c_str());
            bool gb = false;
            for (size_t i = 0; i < commandCount(); ++i) if (!std::strcmp(commandAt(i).name, "getblock")) gb = true;
            check(gb == diag, "a CF_DIAG verb exists only in a diagnostics build", diag ? "diagnostics on" : "diagnostics off");
            const size_t n0 = commandCount(), v0 = cvarCount();
            addCommand("fill", "", "dup of a block verb", 0, {}, [](Ctx&) {});
            addCommand("help", "", "dup", 0, {}, [](Ctx&) {});
            addCommand("dotted.verb", "", "dotted", 0, {}, [](Ctx&) {});
            Cvar nodot = {}; nodot.name = "nodot"; nodot.kind = KIND_TOGGLE;
            addCvar(nodot);
            Cvar dup = {}; dup.name = "world.gold"; dup.kind = KIND_RANGE;
            addCvar(dup);
            check(commandCount() == n0 && cvarCount() == v0, "duplicate, dotted and block-verb names are dropped", "");
            CS::Program p;
            CS::parse("video.fov 95", &p);
            CS::parse("help", &p);
            check(p.firstError == "Unknown command: help", "a command block cannot see the registry", p.firstError.c_str());
            con("video.fov 70; tp 0 1 0; video.fov", &o);
            check(has(o, "video.fov = 70"), "console verbs and block verbs share a line", o.c_str());
            con("reset video.fov", &o);
        }
        setup("");
    }

    // ---- world.name is live: it round-trips through a save and a reload (last: it closes the world)
    {
        Console::init();
        Console::clear();
        Console::run("world.name Console Renamed");
        const std::string nowName = Console::cvarValue(Console::cvarIndex("world.name"));
        const std::string file = Console::cvarValue(Console::cvarIndex("world.file"));
        Console::run("save");
        const bool reopened = quit_to_menu() && open_world(name, g_opt.height);
        const std::string onDisk = Console::storedWorldName(file);
        std::snprintf(d, sizeof(d), "now '%s', header of %s after save + reload '%s'", nowName.c_str(), file.c_str(), onDisk.c_str());
        check(reopened && nowName == "Console Renamed" && onDisk == "Console Renamed", "world.name round-trips through a save and a reload", d);
        Console::run("world.name Console selftest");   // leave the saved name as the file was made
        Console::run("save");
    }

    std::printf("[eden-console] %s\n", CS::describe());
    std::printf("[eden-console] %s (%d failure(s))\n", g_selftestFailures ? "FAILURES" : "ALL PASS", g_selftestFailures);
    return g_selftestFailures ? 1 : 0;
}


// ---- S.0(a) --save-bench=1,10,50,144 (dirty-column save cost; X.1: diagnostics-only) --------------

int run_save_bench() {
    g_selftestTag = "eden-savebench";
    const char* name = g_opt.world.empty() ? "S0 W64" : g_opt.world.c_str();
    if (!open_world(name, g_opt.height)) return 1;
    eden_console_teleport(g_opt.at[0], g_opt.at[1], g_opt.at[2]);
    tick(g_opt.frames / 2);
    if (const char* th = std::getenv("SAVEBENCH_THRESHOLD")) { eden_set_save_inplace_threshold(strtoull(th, 0, 10)); }
    const PlayerState ps = player_state();
    const int px = ((int)ps.x / 16) * 16 + 8, pz = ((int)ps.z / 16) * 16 + 8;
    std::printf("[eden-savebench] world=%s player=(%d,%d)\n", name, (int)ps.x, (int)ps.z);
    { const char* r = eden_bench_save(); std::printf("[eden-savebench] warmup(no dirt) %s\n", r); }
    int flip = 0;
    std::string list = g_saveBenchList;
    for (size_t pos = 0; pos < list.size();) {
        size_t c = list.find(',', pos); if (c == std::string::npos) c = list.size();
        int n = atoi(list.substr(pos, c - pos).c_str()); pos = c + 1;
        for (int rep = 0; rep < 3; ++rep) {
            ++flip;
            int side = 1; while (side * side < n) ++side;
            int placed = 0;
            for (int i = 0; i < side && placed < n; ++i)
                for (int j = 0; j < side && placed < n; ++j, ++placed)
                    eden_console_setblock(px + (i - side / 2) * 16, pz + (j - side / 2) * 16, 30, 10 + (flip % 20));
            tick(3);
            const char* r = eden_bench_save();
            std::printf("[eden-savebench] cols=%d rep=%d %s\n", n, rep, r);
        }
    }
    return 0;
}

}  // namespace g_cmds
}  // namespace harness
