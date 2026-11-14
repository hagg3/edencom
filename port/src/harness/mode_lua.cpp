// K.4: --lua-selftest, sandboxed Lua mod scripts (WORKING/mod-scripting-plan-2026-11-03.md §5; docs/scripting.md).
// Headless; give it a fresh --docs. Mode group "packs". Writes four packs into --docs/Packs, opens a world:
//
//   1. load: two script packs run their files in list order and their load handler once; a pack whose script
//      does not compile is stopped (its line in `scripts`), the others run
//   2. the sandbox: no io/os/package/debug/require/dofile/loadfile/string.dump, no binary chunk, no
//      collectgarbage("stop"), no pattern match on a 70 KB subject; text `load` works
//   3. the API is the command-block language: eden.set == the console's `set` (type + paint), eden.fill counts,
//      a bedrock refusal, an unknown block and a ';' in an argument raise, `wait` in eden.cmd raises,
//      eden.cmd is anchored at the player's feet, eden.get reads back name + colour
//   4. events: the local player's build / paint / mine / burn (MpWorld::intent*) reach the handlers once each, in
//      order; a script's own edits are not events
//   5. eden.wait: a handler waits 0.5 s of game time before its edit
//   6. the shared WORK_PER_FRAME: 6 fills of 4096 cells finish over more than one frame, every cell set
//   7. CPU: a 300k-iteration loop is paused across frames (the gate the count-hook mutation fails); an endless
//      loop is stopped after maxPreempts frames; a non-yieldable endless comparator errors; ticks keep coming
//   8. memory: an allocation bomb gets "not enough memory" under the cap and the state lives on
//   9. errors: maxErrors errors stop that pack only
//  10. the pack hash covers the script bytes
//  12. C code can't hang (QA.3): setmetatable refuses __gc (a __gc added later never runs, at a collect or at close);
//      a backtracking pattern, string.rep("", 2^62), table.move/insert/remove over 2^62 elements each return or
//      error in < 2 s (they ran 20 s to forever before); ordinary patterns and table calls still work
//  11. a session starts no scripts; a closed world closes them; reopening starts them afresh (runs after 12)
// --lua-mutate=hook (count hook off: leg 7's paused loop must FAIL; the endless legs are skipped) and
// --lua-mutate=sandbox (base escapes left in: leg 2 must FAIL; leg 12's __gc cases are skipped, they would hang).
#include <algorithm>
#include <chrono>
#include "harness.h"
#include "eden_fs.h"
#include "../../../Classes/BlockRegistry.h"
#include "../../../Classes/Console.h"
#include "../../../Classes/ContentPack.h"
#include "../../../Classes/ModScript.h"
#include "../../../Classes/MpWorld.h"

using namespace harness;

namespace harness {
namespace g_lua {
namespace {

bool writeFile(const std::string& path, const std::string& text) {
    eden_fs_make_dirs(path.substr(0, path.find_last_of('/')));
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    return std::fclose(f) == 0 && ok;
}

struct Capture : CmdScript::Sink {
    std::vector<std::string> lines;
    void line(int kind, const std::string& t) override { lines.push_back((kind == ERR ? "E:" : "") + t); }
    std::string all() const {
        std::string r;
        for (const std::string& s : lines) r += (r.empty() ? "" : " | ") + s;
        return r;
    }
    bool has(const std::string& t) const {
        for (const std::string& s : lines) if (s.find(t) != std::string::npos) return true;
        return false;
    }
};
Capture g_out;   // outlives every coroutine the test starts (a sink must)

// The console state's answer to one chunk (its printed values / errors).
std::string lua(const std::string& code) {
    g_out.lines.clear();
    ModScript::runConsole(code, &g_out);
    return g_out.all();
}
std::string mod(const std::string& name, const std::vector<std::string>& args = {}) {
    g_out.lines.clear();
    if (!ModScript::runCommand(name, args, &g_out)) return "(no command " + name + ")";
    return g_out.all();
}
const ModScript::Info* infoOf(const std::vector<ModScript::Info>& v, const std::string& id) {
    for (const ModScript::Info& i : v) if (i.pack == id) return &i;
    return NULL;
}
std::string infoText() {
    std::string s;
    for (const ModScript::Info& i : ModScript::info())
        s += i.pack + (i.stopped ? "(stopped: " + i.why + ")" : "") + " ";
    return s;
}
std::string n3(int x, int y, int z) { return std::to_string(x) + ", " + std::to_string(y) + ", " + std::to_string(z); }

const char* kTestLib = R"(lib = {}
function lib.fmt(...)
  local t = {}
  for i = 1, select("#", ...) do t[#t + 1] = tostring((select(i, ...))) end
  return table.concat(t, " ")
end
order = {"lib"}
)";
const char* kTestMain = R"(order[#order + 1] = "main"
local log, loads, ticks = {}, 0, 0
eden.on("load", function() loads = loads + 1 end)
eden.on("tick", function(dt) ticks = ticks + 1 end)
for _, ev in ipairs({"build", "mine", "paint", "burn"}) do
  eden.on(ev, function(...) log[#log + 1] = ev .. " " .. lib.fmt(...) end)
end
eden.command("probe", "test state", function(what)
  if what == "counts" then print(lib.fmt("order", table.concat(order, ","), "loads", loads))
  elseif what == "ticks" then print("ticks " .. ticks)
  elseif what == "log" then print("log " .. table.concat(log, "|")); log = {}
  end
end)
eden.command("waitset", "", function(x, y, z)
  eden.wait(0.5)
  eden.set(tonumber(x), tonumber(y), tonumber(z), "stone")
  print("waitset done")
end)
eden.command("bigfill", "", function(x, y, z)
  x, y, z = tonumber(x), tonumber(y), tonumber(z)
  local t0, cells, refused = eden.clock(), 0, 0
  for i = 0, 5 do
    local c, r = eden.fill(x + 16 * i, y, z, x + 16 * i + 15, y + 15, z + 15, "glass")
    cells, refused = cells + c, refused + r
  end
  print(string.format("bigfill %d %d %.4f", cells, refused, eden.clock() - t0))
end)
)";
const char* kSpin = R"(local n = 0
eden.on("tick", function() n = n + 1 end)
eden.command("ticks", "", function() print("ticks " .. n) end)
eden.command("bounded", "", function()
  local t0, s = eden.clock(), 0
  for i = 1, 300000 do s = s + i end
  print(string.format("bounded %d %.4f", s, eden.clock() - t0))
end)
eden.command("forever", "", function() while true do end end)
eden.command("nopause", "", function()
  local t = {}
  for i = 1, 50 do t[i] = 51 - i end
  local ok, e = pcall(table.sort, t, function(a, b) while true do end end)
  print("nopause " .. tostring(ok) .. " " .. tostring(e))
end)
)";
const char* kBomb = R"(eden.command("bomb", "", function()
  local ok, e = pcall(function()
    local t, s = {}, string.rep("x", 1000)
    for i = 1, 1e9 do t[i] = s .. i end
  end)
  print("bomb " .. tostring(ok) .. " " .. tostring(e))
end)
eden.command("alive", "", function() print("alive " .. math.floor(collectgarbage("count"))) end)
eden.command("boom", "", function() error("boom") end)
)";

}  // namespace

int run_lua_selftest() {
    g_selftestTag = "eden-lua";
    char d[2048];
    const std::string docs = eden_platform_documents_root();
    const std::string packs = docs + "/Packs";
    if (!mutation_known("eden-lua", "--lua-mutate", g_opt.luaMutate, {"hook", "sandbox"})) return 2;
    const bool mutHook = g_opt.luaMutate == "hook", mutSandbox = g_opt.luaMutate == "sandbox";
    ModScript::testNoHook = mutHook;
    ModScript::testNoSandbox = mutSandbox;
    eden_fs_remove_tree(packs);
    bool wrote = writeFile(packs + "/aa_test/pack.json",
                           "{\"format\": 1, \"id\": \"aa_test\", \"version\": \"1\", \"scripts\": [\"scripts/lib.lua\", \"scripts/main.lua\"]}\n") &&
                 writeFile(packs + "/aa_test/scripts/lib.lua", kTestLib) && writeFile(packs + "/aa_test/scripts/main.lua", kTestMain) &&
                 writeFile(packs + "/ab_bomb/pack.json", "{\"format\": 1, \"id\": \"ab_bomb\", \"scripts\": [\"bomb.lua\"]}\n") &&
                 writeFile(packs + "/ab_bomb/bomb.lua", kBomb) &&
                 writeFile(packs + "/ac_spin/pack.json", "{\"format\": 1, \"id\": \"ac_spin\", \"scripts\": [\"spin.lua\"]}\n") &&
                 writeFile(packs + "/ac_spin/spin.lua", kSpin) &&
                 writeFile(packs + "/ad_bad/pack.json", "{\"format\": 1, \"id\": \"ad_bad\", \"scripts\": [\"bad.lua\"]}\n") &&
                 writeFile(packs + "/ad_bad/bad.lua", "this is not lua\n");
    check(wrote, "the four test packs are written to Documents/Packs", packs.c_str());
    if (!wrote) return 1;
    ModScript::Limits& lim = ModScript::limits();
    lim.memBytes = 2u << 20;
    lim.maxPreempts = 30;
    lim.hardPerResume = 200000;
    lim.maxErrors = 5;
    g_app->setFixedEtime(1.0f / 60.0f);   // game time = frames / 60 (eden.wait, eden.clock)

    // ---- 1. load
    const char* name = g_opt.world.empty() ? "lua-selftest" : g_opt.world.c_str();
    if (!open_world(name, g_opt.height)) return 1;
    eden_console_teleport(g_opt.at[0], g_opt.at[1], g_opt.at[2]);
    tick(g_opt.frames / 2);
    {
        const std::vector<ModScript::Info> v = ModScript::info();
        const ModScript::Info* bad = infoOf(v, "ad_bad");
        const std::string counts = mod("probe", {"counts"});
        std::snprintf(d, sizeof(d), "states: %s; probe: %s", infoText().c_str(), counts.c_str());
        check(infoOf(v, "aa_test") && !infoOf(v, "aa_test")->stopped && infoOf(v, "ab_bomb") && infoOf(v, "ac_spin") &&
                  bad && bad->stopped && bad->why.find("bad.lua does not compile") != std::string::npos &&
                  counts == "order lib,main loads 1",
              "packs' scripts run in list order, load fires once; a pack that does not compile is stopped alone", d);
    }

    // ---- 2. the sandbox (the console's state is set up by the same code as a pack's)
    {
        const std::string r = lua(R"(local r = {}
for _, n in ipairs({"io", "os", "package", "debug", "require", "dofile", "loadfile"}) do r[#r + 1] = n .. "=" .. type(_G[n]) end
r[#r + 1] = "dump=" .. type(string.dump)
local f, e = load("\27Lua\84\0\25\147\13\10\26\10", "x", "b")
r[#r + 1] = "binary=" .. tostring(f == nil and e:find("binary") ~= nil)
r[#r + 1] = "gcstop=" .. tostring(pcall(collectgarbage, "stop"))
r[#r + 1] = "pattern=" .. tostring(pcall(string.find, string.rep("a", 70000), "b"))
r[#r + 1] = "text=" .. tostring(load("return 7")())
return table.concat(r, ","))");
        check(r == "io=nil,os=nil,package=nil,debug=nil,require=nil,dofile=nil,loadfile=nil,dump=nil,binary=true,gcstop=false,pattern=false,text=7",
              "the sandbox: no io/os/package/debug/require/dofile/loadfile/dump, text-only load, no gc stop, capped patterns", r.c_str());
    }

    // ---- 3. the API is the command-block language
    const PlayerState ps = player_state();
    const int px = (int)ps.x, pz = (int)ps.z, Y = g_opt.height - 10;
    {
        const int x = px + 3, z = pz + 3;
        const std::string a = lua("return eden.set(" + n3(x, Y, z) + ", \"stone\", 5)");
        Console::run(("set =" + std::to_string(x + 1) + " =" + std::to_string(Y) + " =" + std::to_string(z) + " stone 5").c_str());
        tick(1);
        const int t1 = eden_console_getblock(x, z, Y), t2 = eden_console_getblock(x + 1, z, Y);
        const int c1 = World::getWorld->terrain->getColor(x, z, Y), c2 = World::getWorld->terrain->getColor(x + 1, z, Y);
        const std::string fill = lua("return eden.fill(" + n3(x, Y, z + 2) + ", " + n3(x + 2, Y + 2, z + 4) + ", \"brick\")");
        const std::string get = lua("return eden.get(" + n3(x, Y, z) + ")");
        const std::string bed = lua("return eden.set(" + n3(x, 0, z) + ", \"stone\")");
        const std::string unk = lua("return pcall(eden.set, " + n3(x, Y, z) + ", \"glas\")");
        const std::string semi = lua("return pcall(eden.set, " + n3(x, Y, z) + ", \"stone;tp\")");
        const std::string wt = lua("return pcall(eden.cmd, \"wait 1\")");
        int ax, ay, az;
        CmdScript::playerAnchor(&ax, &ay, &az);
        const std::string rel = lua("return eden.cmd(\"set 0 3 0 glass\")");
        const int trel = eden_console_getblock(ax, az, ay + 3);
        const std::string stone = BlockRegistry::stockName(TYPE_STONE) ? BlockRegistry::stockName(TYPE_STONE) : "?";
        std::snprintf(d, sizeof(d), "set %s -> %d:%d vs console %d:%d; fill %s; get %s; bedrock %s (%d); unknown %s; semi %s; wait %s; rel %s -> %d",
                      a.c_str(), t1, c1, t2, c2, fill.c_str(), get.c_str(), bed.c_str(), eden_console_getblock(x, z, 0), unk.c_str(),
                      semi.c_str(), wt.c_str(), rel.c_str(), trel);
        check(a == "true\tnil" && t1 == TYPE_STONE && c1 == 5 && t1 == t2 && c1 == c2 && fill == "27\t0\tnil" &&
                  get == stone + "\t5" && bed == "false\tnil" && eden_console_getblock(x, z, 0) == TYPE_BEDROCK &&
                  unk == "false\tUnknown block: glas" && semi.find("no spaces or ';'") != std::string::npos &&
                  wt.find("eden.wait") != std::string::npos && rel == "1\t0\tnil" && trel == TYPE_GLASS,
              "eden.set/fill/get/cmd behave as the block verbs: same cells, refusals and errors", d);
    }

    // ---- 4. events from the player's edits
    {
        const int x = px - 3, z = pz - 3;
        mod("probe", {"log"});   // drop what the earlier legs did (nothing: a script's edits are not events)
        Hud* hud = World::getWorld->hud;
        const int keep = hud->blocktype;
        hud->blocktype = TYPE_BRICK;
        MpWorld::intentBuild(x, z, Y);
        MpWorld::intentPaint(x, z, Y, 7);
        MpWorld::intentMine(x, z, Y);
        hud->blocktype = TYPE_LEAVES;
        MpWorld::intentBuild(x, z, Y);
        MpWorld::intentBurn(x, z, Y);
        hud->blocktype = keep;
        tick(2);
        const std::string got = mod("probe", {"log"});
        const std::string brick = BlockRegistry::stockName(TYPE_BRICK), leaves = BlockRegistry::stockName(TYPE_LEAVES);
        const std::string at = std::to_string(x) + " " + std::to_string(Y) + " " + std::to_string(z);
        const std::string want = "log build " + at + " " + brick + "|paint " + at + " 7 " + brick + "|mine " + at + " " + brick +
                                 "|build " + at + " " + leaves + "|burn " + at + " " + leaves;
        std::snprintf(d, sizeof(d), "got  %s\nwant %s", got.c_str(), want.c_str());
        check(got == want, "the player's build/paint/mine/burn reach the handlers once each, in order; script edits do not", d);
    }

    // ---- 5. eden.wait
    {
        const int x = px + 6, z = pz - 6;
        mod("waitset", {std::to_string(x), std::to_string(Y), std::to_string(z)});
        tick(15);
        const int early = eden_console_getblock(x, z, Y);
        const bool earlyDone = g_out.has("waitset done");
        tick(30);
        const int late = eden_console_getblock(x, z, Y);
        std::snprintf(d, sizeof(d), "after 0.25 s %d (done %d), after 0.75 s %d; out %s", early, earlyDone, late, g_out.all().c_str());
        check(early == TYPE_NONE && !earlyDone && late == TYPE_STONE && g_out.has("waitset done"),
              "eden.wait(0.5) pauses the handler in game time, then it carries on", d);
    }

    // ---- 6. the shared work budget
    {
        const int x = px - 48, z = pz + 12, y = g_opt.height - 22;
        mod("bigfill", {std::to_string(x), std::to_string(y), std::to_string(z)});
        const bool sameFrame = g_out.has("bigfill");
        tick(10);
        int glass = 0;
        for (int i = 0; i < 96; i += 5) glass += eden_console_getblock(x + i, z + 7, y + 7) == TYPE_GLASS;
        std::snprintf(d, sizeof(d), "same frame %d; %s; glass samples %d/20", sameFrame, g_out.all().c_str(), glass);
        check(!sameFrame && g_out.has("bigfill 24576 0 ") && glass == 20,
              "6 fills of 4096 cells share WORK_PER_FRAME: they finish over several frames, every cell set", d);
    }

    // ---- 7. CPU
    {
        const std::string t0 = mod("ticks");
        mod("bounded");
        const bool sameFrame = g_out.has("bounded ");
        tick(40);
        double dt = -1;
        long long sum = 0;
        for (const std::string& s : g_out.lines) if (s.compare(0, 8, "bounded ") == 0) std::sscanf(s.c_str() + 8, "%lld %lf", &sum, &dt);
        std::snprintf(d, sizeof(d), "same frame %d, sum %lld, %.4f s of game time", sameFrame, sum, dt);
        check(!sameFrame && sum == 45000150000LL && dt >= 1.5 / 60,
              "a 300k-iteration loop is paused by the instruction budget and finishes frames later", d);
        if (mutHook) {
            std::printf("[eden-lua] (mutation hook: the endless-loop legs are skipped)\n");
        } else {
            const ModScript::Stats s0 = ModScript::stats();
            mod("forever");
            tick(lim.maxPreempts + 10);
            const std::vector<ModScript::Info> v = ModScript::info();
            const ModScript::Info* spin = infoOf(v, "ac_spin");
            const std::string nop = mod("nopause");
            const std::string t1 = mod("ticks");
            int n0 = 0, n1 = 0;
            std::sscanf(t0.c_str(), "ticks %d", &n0);
            std::sscanf(t1.c_str(), "ticks %d", &n1);
            std::snprintf(d, sizeof(d), "preempted +%d, spin %s; nopause %s; ticks %d -> %d", ModScript::stats().preempted - s0.preempted,
                          spin ? spin->why.c_str() : "?", nop.c_str(), n0, n1);
            check(spin && !spin->stopped && spin->why.find("frames without eden.wait (stopped)") != std::string::npos &&
                      nop.find("nopause false") != std::string::npos && nop.find("too much CPU") != std::string::npos && n1 > n0 + 40,
                  "an endless loop is stopped after maxPreempts frames; a non-yieldable one errors; the tick handler kept running", d);
        }
    }

    // ---- 8. memory
    {
        mod("bomb");
        tick(120);
        const std::string after = g_out.all();
        const std::string alive = mod("alive");
        const std::vector<ModScript::Info> v = ModScript::info();
        const ModScript::Info* b = infoOf(v, "ab_bomb");
        std::snprintf(d, sizeof(d), "%s; %s; peak %zu KiB, refusals %d", after.c_str(), alive.c_str(), b ? b->memPeak / 1024 : 0,
                      b ? b->memRefusals : -1);
        check(after.find("bomb false not enough memory") != std::string::npos && alive.compare(0, 6, "alive ") == 0 && b &&
                  b->memPeak <= lim.memBytes && b->memRefusals > 0 && !b->stopped,
              "an allocation bomb hits the memory cap as a catchable error; the state lives on", d);
    }

    // ---- 9. errors
    {
        for (int i = 0; i < lim.maxErrors; ++i) mod("boom");
        tick(2);
        const std::vector<ModScript::Info> v = ModScript::info();
        const ModScript::Info* b = infoOf(v, "ab_bomb");
        const std::string probe = mod("probe", {"counts"});
        std::snprintf(d, sizeof(d), "bomb %s; probe %s", b ? b->why.c_str() : "?", probe.c_str());
        check(b && b->stopped && b->why.find("too many errors") != std::string::npos && probe.find("loads 1") != std::string::npos &&
                  mod("alive").find("no command") != std::string::npos,
              "maxErrors errors stop that pack only (its commands go with it)", d);
    }

    // ---- 10. the pack hash covers the scripts
    {
        std::vector<std::string> lines;
        auto hashOf = [&]() {
            for (const ContentPack::ModPack& p : ContentPack::scanPacks(docs, &lines)) if (p.id == "aa_test") return p.hash;
            return std::string();
        };
        const std::string h0 = hashOf();
        writeFile(packs + "/aa_test/scripts/main.lua", std::string(kTestMain) + "-- edited\n");
        const std::string h1 = hashOf();
        writeFile(packs + "/aa_test/scripts/main.lua", kTestMain);
        const std::string h2 = hashOf();
        std::snprintf(d, sizeof(d), "%.12s -> %.12s -> %.12s", h0.c_str(), h1.c_str(), h2.c_str());
        check(!h0.empty() && h0 != h1 && h0 == h2, "a script's bytes are in its pack's hash", d);
    }

    // ---- 12. C code a script calls can't hang the game (QA.3)
    {
        struct Case { const char* code; const char* want; bool gc; };
        const Case cases[] = {
            {"return pcall(setmetatable, {}, {__gc = function() while true do end end})", "false\t", true},
            {"return pcall(setmetatable, {}, {__gc = false})", "false\t", true},
            // __gc added after setmetatable: never marked, so neither this collect nor the close below runs it
            {"local mt = {}; qa3_keep = setmetatable({}, mt); do local t = setmetatable({}, mt) end; "
             "mt.__gc = function() while true do end end; collectgarbage('collect'); return 'collected'", "collected", true},
            {"return pcall(string.find, string.rep('a', 200), '.-.-.-.-b')", "false\t", false},
            {"return pcall(string.find, string.rep('a', 3000), 'a-a-b')", "false\t", false},
            {"return pcall(function() for m in string.gmatch(string.rep('a', 200), '.-.-.-.-b') do end end)", "false\t", false},
            {"return pcall(string.gsub, string.rep('(', 60000), '%b()', '')", "false\t", false},
            {"return #string.rep('', 1 << 62), #string.rep('', 1 << 62, '')", "0\t0", false},
            {"return pcall(table.move, {}, 1, 1 << 62, 2)", "false\t", false},
            {"local t = setmetatable({}, {__len = function() return 1 << 62 end}); return pcall(table.insert, t, 1, 'x')", "false\t", false},
            {"local t = setmetatable({}, {__len = function() return 1 << 62 end}); return pcall(table.remove, t, 1)", "false\t", false},
            {"local t = setmetatable({}, {__len = function() return -1 end}); return pcall(table.remove, t, math.mininteger)", "false\t", false},
        };
        std::string bad;
        double worst = 0;
        for (const Case& c : cases) {
            if (c.gc && mutSandbox) continue;
            tick(1);
            const auto t0 = std::chrono::steady_clock::now();
            const std::string r = lua(c.code);
            const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            worst = std::max(worst, s);
            std::printf("[eden-lua] leg 12: %.3f s  %s  ->  %s\n", s, c.code, r.c_str());
            if (r.compare(0, std::strlen(c.want), c.want) != 0 || s >= 2.0) bad += std::string(" [") + c.code + " -> " + r + "]";
        }
        // the errors say why, and ordinary calls are untouched
        const std::string why = lua("local _, a = pcall(setmetatable, {}, {__gc = 1}); local _, b = pcall(string.find, string.rep('a', 200), '.-.-.-.-b'); "
                                    "local _, c = pcall(table.move, {}, 1, 1 << 62, 2); return a, b, c");
        const std::string fine = lua("local t = {1, 2, 3}; table.insert(t, 1, 0); table.remove(t, 2); local m = table.move(t, 1, 3, 2, {}); "
                                     "return table.concat(t, ','), m[4], ('x(a(b)c)y'):match('%b()'), (('a b c'):gsub('%w', 'x')), "
                                     "string.rep('ab', 3, '-'), getmetatable(setmetatable({}, {__index = {k = 7}})).__index.k");
        const auto t0 = std::chrono::steady_clock::now();
        ModScript::restart();   // closes every state (the console's holds qa3_keep): a __gc would run here
        const double closeS = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        tick(3);
        std::snprintf(d, sizeof(d), "worst %.3f s, close %.3f s;%s why: %s; fine: %s", worst, closeS, bad.c_str(), why.c_str(), fine.c_str());
        check(bad.empty() && closeS < 2.0 &&
                  (mutSandbox || why.find("__gc is not available") != std::string::npos) && why.find("pattern too complex") != std::string::npos &&
                  why.find("too many elements to move") != std::string::npos && fine == "0,2,3\t3\t(a(b)c)\tx x x\tab-ab-ab\t7",
              "a script can't hang the game in C: no __gc, capped pattern steps, string.rep and table.move/insert/remove (< 2 s each)", d);
    }

    // ---- 11. session, close, reopen
    {
        ModScript::worldEntered(true);
        tick(3);
        const bool none = ModScript::info().empty() && lua("return 1").find("multiplayer") != std::string::npos;
        ModScript::worldEntered(false);
        tick(3);
        const bool back = infoOf(ModScript::info(), "aa_test") != NULL;
        save_world();
        const bool quit = quit_to_menu();
        tick(10);
        const bool closed = ModScript::info().empty();
        const bool reopened = open_world(name, g_opt.height);
        tick(g_opt.frames / 2);
        const std::string counts = mod("probe", {"counts"});
        std::snprintf(d, sizeof(d), "session none %d, back %d, quit %d closed %d, reopened %d: %s", none, back, quit, closed, reopened, counts.c_str());
        check(none && back && quit && closed && reopened && counts == "order lib,main loads 1",
              "a session starts no scripts; closing the world closes them; reopening starts them afresh", d);
    }
    g_app->setFixedEtime(0.0f);
    eden_fs_remove_tree(packs);   // the next selftest in the same Documents (the iOS simulator leg) loads no test pack
    ModScript::restoreLimits();
    ModScript::testNoHook = ModScript::testNoSandbox = false;
    std::printf("[eden-lua] %s (%d failure(s))\n", g_selftestFailures ? "FAILURES" : "ALL PASS", g_selftestFailures);
    return g_selftestFailures ? 1 : 0;
}

}  // namespace g_lua
}  // namespace harness
