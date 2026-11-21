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
//      K.6-1a: a fire tap on stone fires `use` once (how "fire") and no `burn`; on wood `use` then `burn`; eden.version is 2
//      (--lua-mutate=usefilter keeps the flammable test on `use`: must FAIL)
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
// K.8-B legs: 15 (a short command name two packs register: both files run to the end, `mod pack:name`), 16 (a total memory
// cap across all states), 17 (`scripts reload` re-reads the script files).
// K.8 legs: 13 (`load` once, after a top level that the budget paused / starved; --lua-mutate=loadearly must FAIL it)
// and 14 (the 64-coroutine cap skips a tick, never errors; --lua-mutate=caperror must FAIL it).
// K.6-5 leg 22: eden.store (record 13 MOD_DATA): every value type survives save + reopen exactly; the caps return
// false, why; a pack that is not loaded keeps its bytes through two saves; a blob that does not decode starts empty and
// is kept until its pack stores something; the 1 MiB / 64-pack record caps; a session world loads none
// (--lua-mutate=storenotdirty: set never marks the store dirty, so the reopen reads nothing: must FAIL).
// K.6-2 leg 23: `leave` sets a block and an eden.store key before the exit save (both back after reopen), fires again on a
// second close, eden.wait in it raises, edits past one WORK_PER_FRAME are refused, an endless handler ends at hardPerResume
// and the world still closes; a warp fires nothing (--lua-mutate=leaveaftersave saves first: must FAIL).
// K.7-1a leg 24: eden.toast shows in the HUD status line, a second within 1 s is false, 121 bytes raise (--lua-mutate=toastflood must FAIL).
// K.7-1b leg 25: eden.ask / eden.input: the world stops under the dialog and the answer resumes the handler; the 0.5 s gap and
// the mute link; two packs in one frame queue (--lua-mutate=dialognoqueue must FAIL); an engine show() over a mod dialog gives
// nil, "replaced"; "busy", "muted", "unavailable"; the caps raise; `leave` cannot pause; a huge body is clamped to the safe
// height (--lua-mutate=dialognoclamp must FAIL).
// K.7-2b leg 26: eden.setting returns the player's value from Packs/.settings/<id>.json (boolean, number, option string, text); a value
// that fails the pack's schema reads as the default; an undeclared id raises.
// K.6-4a leg 29: creature handles (slot, generation): eden.spawn returns one whose pos is the spawn point; kind, ==, tostring;
// eden.creatures(x, y, z, r) finds it nearest first and caps r; h:remove() and an engine kill make it invalid (every method
// nil); spawning until its slot is reused leaves the old handle invalid, never reading the new creature
// (--lua-mutate=nogen must FAIL); walking away until its cell leaves the loaded area makes it invalid for good;
// eden.store refuses a handle; sizeof(EntityData) is still 60.
// K.6-4b leg 30: h:setpos refuses a solid or unloaded cell (false, why) and raises for a non-finite coordinate; into open air
// it is exact and at rest; h:setvel raises for a NaN/inf (--lua-mutate=nonfinite must FAIL), clamps to 16 blocks/s, moves
// the creature a bounded distance over 30 frames with the AI held off, and the AI resumes once it settles.
// --lua-mutate=hook (count hook off: leg 7's paused loop must FAIL; the endless legs are skipped) and
// --lua-mutate=sandbox (base escapes left in: leg 2 must FAIL; leg 12's __gc cases are skipped, they would hang).
#include <algorithm>
#include <chrono>
#include "harness.h"
#include "eden_fs.h"
#include "../../../Classes/BlockRegistry.h"
#include "../../../Classes/Console.h"
#include "../../../Classes/ContentPack.h"
#include "../../../Classes/EdenWorldStore.h"
#include "../../../Classes/FileManager.h"
#include "../../../Classes/GLDialog.h"
#include "../../../Classes/ModMenuScreen.h"
#include "../../../Classes/ModScript.h"
#include "../../../Classes/MpWorld.h"
#include "../../../Classes/GLWidgets.h"
#include "../../../Classes/Hud.h"
#include "../../../Classes/Menu.h"
#include "../../../Classes/ModsMenu.h"
#include "../../../Classes/Model.h"
#include "../../../Classes/Globals.h"

extern "C" int eden_ui_focus_key(int key);
#include "../../../Classes/SignTool.h"
#include "../../../Classes/SignRenderer.h"

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
for _, ev in ipairs({"build", "mine", "paint", "burn", "use", "sign"}) do
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

// K.8-1: top levels of 3 x packPerFrame instructions, then a `load` handler. The three burners (ae, af, ag) use up
// allPerFrame (250000) in the first frame, so ah's top level is not even started then.
std::string loadPack(const char* id, long packPerFrame, bool burn) {
    std::string t = burn ? "for i = 1, " + std::to_string(3 * packPerFrame) + " do end\n" : "";
    t += "local loads, topEnd, loadAt = 0, eden.clock(), -1\n"
         "eden.on(\"load\", function() loads = loads + 1; loadAt = eden.clock() end)\n";
    t += std::string("eden.command(\"probe_") + id + "\", \"\", function() print(string.format(\"loads %d top %.4f load %.4f\", loads, topEnd, loadAt)) end)\n";
    return t;
}
// K.8-2: "hold" waits 6 s of game time; with 64 of them live, a tick must be skipped, not fail.
const char* kCap = R"(local n = 0
eden.on("tick", function() n = n + 1 end)
eden.command("hold", "", function() eden.wait(6) end)
eden.command("captick", "", function() print("ticks " .. n) end)
)";

// K.8-4: two packs register `go`; the second's file must run to the end (end_<pack> is registered after the clash).
const char* kGo = R"(eden.command("go", "", function() print("go from " .. eden.pack) end)
eden.command("end_" .. eden.pack, "", function() print("end " .. eden.pack) end)
)";
// K.8-5: a command that takes 6 MiB in one allocation, one that lets it go.
const char* kMem = R"(local keep
eden.command("grab", "", function()   -- 96 x 64 KiB (one 6 MiB string.rep would briefly need twice that)
  keep = {}
  for i = 1, 96 do keep[i] = string.rep("x", 65536) end
  print("grabbed " .. eden.pack)
end)
eden.command("drop", "", function() keep = nil; collectgarbage(); print("dropped") end)
eden.command("still", "", function() print("still " .. eden.pack) end)
)";
// K.6-5: eden.store. `dump` is exact: an integer prints as i<n>, a float as f<%a>, a string as s<len>:<hex>.
const char* kStore = R"(local S = eden.store
local function hex(s) return (s:gsub(".", function(c) return string.format("%02x", c:byte()) end)) end
local function order(a, b)
  if type(a) ~= type(b) then return type(a) == "number" end
  return a < b
end
local function dump(v)
  local t = type(v)
  if t == "number" then return math.type(v) == "integer" and "i" .. tostring(v) or "f" .. string.format("%a", v) end
  if t == "string" then return "s" .. #v .. ":" .. hex(v) end
  if t == "table" then
    local ks, o = {}, {}
    for k in pairs(v) do ks[#ks + 1] = k end
    table.sort(ks, order)
    for _, k in ipairs(ks) do o[#o + 1] = dump(k) .. "=" .. dump(v[k]) end
    return "{" .. table.concat(o, ",") .. "}"
  end
  return tostring(v)
end
local V = {
  int = 42, big = math.maxinteger, small = math.mininteger, odd = 9007199254740993,
  flt = 0.1, third = 1 / 3, negz = -0.0, tiny = 4.9e-324, whole = 2.0,
  yes = true, no = false,
  bin = "\0\1\2\255" .. string.rep("\128", 10) .. "\0",
  empty = "",
  max4k = string.rep("z", 4095) .. "\0",
  nest = {1, 2.5, "three", {deep = {deeper = {"four"}}}, x = true, [-3] = "neg", [10] = 10},
}
local function fill(prefix)
  local i, ok, why = 0, true, nil
  while ok do ok, why = S.set(prefix .. i, string.rep("f", 4000)); i = i + 1 end
  for j = 0, i - 1 do S.set(prefix .. j, nil) end
  return "fill " .. (i - 1) .. " " .. tostring(why)
end
eden.command("sput", "", function()
  for k, v in pairs(V) do
    local ok, why = S.set(k, v)
    if not ok then print("sput fail " .. k .. " " .. tostring(why)) return end
  end
  print("sput ok")
end)
eden.command("vdump", "", function()
  local ks, o = {}, {}
  for k in pairs(V) do ks[#ks + 1] = k end
  table.sort(ks)
  for _, k in ipairs(ks) do o[#o + 1] = k .. "=" .. dump(V[k]) end
  print(table.concat(o, ";"))
end)
eden.command("sget", "", function()
  local o = {}
  for _, k in ipairs(S.keys()) do if k ~= "step" then o[#o + 1] = k .. "=" .. dump(S.get(k)) end end
  print(table.concat(o, ";"))
end)
eden.command("sone", "", function(k, v) local ok, why = S.set(k, v); print(tostring(ok) .. " " .. tostring(why)) end)
eden.command("sstep", "", function() print("step " .. tostring(S.get("step"))) end)
eden.command("sfill", "", function(prefix) print(fill(prefix)) end)
eden.command("scaps", "", function()
  local r = {}
  local function try(k, v) local ok, why = S.set(k, v); r[#r + 1] = tostring(ok) .. ":" .. tostring(why) end
  try("fn", function() end)
  try("nan", 0 / 0)
  try("inf", math.huge)
  try("s4097", string.rep("x", 4097))
  try("deep", {{{{{1}}}}})
  local cyc = {}; cyc.me = cyc; try("cyc", cyc)
  local wide = {}; for i = 1, 257 do wide[i] = i end; try("wide", wide)
  try("bkey", {[true] = 1})
  try("fkey", {[1.5] = 1})
  try("co", coroutine.create(function() end))
  r[#r + 1] = "keyerr " .. tostring(pcall(S.set, "", 1)) .. tostring(pcall(S.set, string.rep("k", 65), 1)) ..
              tostring(pcall(S.set, 5, 1)) .. tostring(pcall(S.get, {}))
  r[#r + 1] = "after " .. tostring(S.get("fn")) .. tostring(S.get("deep"))
  S.set("tmp", 5); local had = S.get("tmp"); S.set("tmp", nil)
  r[#r + 1] = "del " .. tostring(had) .. " " .. tostring(S.get("tmp"))
  local t = S.get("nest"); t.x = false
  r[#r + 1] = "copy " .. tostring(S.get("nest").x)
  r[#r + 1] = fill("fill")
  r[#r + 1] = "keys " .. #S.keys()
  print(table.concat(r, "|"))
end)
)";
// K.6-5: a pack whose saved blob the test replaces with bytes that do not decode.
const char* kStoreBad = R"(eden.command("bkeys", "", function() print("bkeys " .. #eden.store.keys()) end)
eden.command("bset", "", function() print("bset " .. tostring(eden.store.set("fixed", 1))) end)
)";

// K.6-2: `leave` handlers, idle until `larm` arms them (every earlier leg's quit fires them too). One sets a block and
// counts its closes in eden.store, one tries eden.wait, one spends more than WORK_PER_FRAME, one never returns.
const char* kLeave = R"(local arm
eden.command("larm", "", function(x, y, z, fx, fy, fz, spin)
  arm = {x = tonumber(x), y = tonumber(y), z = tonumber(z), fx = tonumber(fx), fy = tonumber(fy), fz = tonumber(fz), spin = spin == "1"}
  print("armed")
end)
eden.command("lget", "", function()
  print("left " .. tostring(eden.store.get("left")) .. " | " .. tostring(eden.store.get("wait")) .. " | " .. tostring(eden.store.get("budget")))
end)
eden.on("leave", function()
  if not arm then return end
  eden.set(arm.x, arm.y, arm.z, "brick")
  eden.store.set("left", (eden.store.get("left") or 0) + 1)
  local ok, e = pcall(eden.wait, 1)
  eden.store.set("wait", tostring(ok) .. " " .. tostring(e))
end)
eden.on("leave", function()
  if not arm then return end
  local cells, why, z = 0, nil, arm.fz + 20 * (eden.store.get("left") or 0)   -- fresh air each close
  for i = 0, 5 do
    local c, r, w = eden.fill(arm.fx + 16 * i, arm.fy, z, arm.fx + 16 * i + 15, arm.fy + 15, z + 15, "glass")
    cells = cells + c
    why = why or w
  end
  eden.store.set("budget", cells .. " " .. tostring(why))
end)
eden.on("leave", function()
  if arm and arm.spin then while true do end end
end)
-- K.7-1a: eden.toast
eden.command("toast", "", function(text, secs)
  local ok, r = pcall(eden.toast, text, tonumber(secs))
  print("toast " .. tostring(ok) .. " " .. tostring(r))
end)
eden.command("toastlong", "", function(n)
  local ok, e = pcall(eden.toast, string.rep("x", tonumber(n)))
  print("toastlong " .. tostring(ok) .. " " .. tostring(e))
end)
)";

// K.7-1b: eden.ask / eden.input. Two copies of this pack run (au_dlg, av_dlg); commands are suffixed with the pack id.
// K.7-2b: eden.setting.
const char* kSet = R"(eden.command("setting", "", function(id)
  local ok, v = pcall(eden.setting, id)
  print("setting " .. tostring(ok) .. " " .. type(v) .. " " .. tostring(v))
end)
local slog = {}   -- K.7-3b: what the pause menu's Mods page reported
eden.on("setting", function(id, v, old) slog[#slog + 1] = id .. "=" .. tostring(v) .. "/" .. tostring(old) end)
eden.on("menu", function() slog[#slog + 1] = "menu" end)
eden.command("setlog", "", function() print("setlog " .. table.concat(slog, ";")) end)
)";

const char* kDlg = R"(local P = eden.pack
local res = {}
local function show(a, b) return tostring(a) .. "/" .. tostring(b) end
eden.command("ask_" .. P, "", function(tag, danger)
  local a, b = eden.ask{title = "Hello " .. tag, body = "pick one", buttons = {"A", "B", "C"}, danger = tonumber(danger)}
  res[tag] = show(a, b)
end)
eden.command("long_" .. P, "", function(tag)
  local a, b = eden.ask("Long", string.rep("lorem ipsum ", 33), {"Yes", "No"})
  res[tag] = show(a, b)
end)
eden.command("inp_" .. P, "", function(tag, def)
  local a, b = eden.input{title = "Name", body = "type it", default = def, max = 8}
  res[tag] = show(a, b)
end)
eden.command("res_" .. P, "", function(tag) print(tostring(res[tag])) end)
eden.command("bad_" .. P, "", function()
  local r = {}
  local function try(...) local ok, e = pcall(eden.ask, ...); r[#r + 1] = tostring(ok) .. ":" .. tostring(e):gsub("^.-: ", "") end
  try("t", "b", {})
  try(string.rep("x", 61), "b", {"ok"})
  try("t", string.rep("x", 401), {"ok"})
  try("t", "b", {"a", "b", "c", "d", "e"})
  try("t", "b", {string.rep("x", 25)})
  try{title = "t", buttons = {"a"}, colour = 1}
  try{title = "t", buttons = {"a", "b"}, danger = 3}
  print(table.concat(r, "|"))
end)
eden.on("leave", function()
  local ok, e = pcall(eden.ask, "t", "b", {"x"})
  eden.store.set("dlgleave", tostring(ok) .. " " .. tostring(e))
end)
eden.command("dget_" .. P, "", function() print(tostring(eden.store.get("dlgleave"))) end)
)";

// K.7-3a: eden.menu. `uimenu x y z` opens a two-tab menu (icon rail) whose `place` row sets stone at x y z; `last` prints
// what on_action / on_close saw; `relabel` is m:set; `again` asks for a second menu while one is open; `big` a 129-row
// spec; `closeit` m:close(); `flat` a one-tab menu (no rail); `tabbed` a labelled two-tab menu (a tab strip).
const char* kMenu = R"(local m, at
local log = {}
local closes = 0
local function act(h, row, value)
  log[#log + 1] = row .. "=" .. tostring(value)
  if row == "place" and at then eden.set(at[1], at[2], at[3], "stone") end
end
local function cls(h) closes = closes + 1; log[#log + 1] = "closed" end
local rows = {
  {type = "heading", label = "Power"},
  {type = "text", label = "Taps run the mod's on_action; the world keeps running behind this menu."},
  {type = "row", id = "place", label = "Place stone", detail = "here", icon = "box", hint = "Sets one block"},
  {type = "toggle", id = "glow", label = "Glow", value = false},
  {type = "slider", id = "speed", label = "Speed", min = 0, max = 10, step = 0.5, value = 2},
  {type = "choice", id = "mode", label = "Mode", options = {"Low", "Mid", "High"}, value = 1},
  {type = "button", id = "wipe", label = "Clear all", action = "Clear", danger = true},
}
eden.command("uimenu", "", function(x, y, z)
  at = {tonumber(x), tonumber(y), tonumber(z)}
  local why
  m, why = eden.menu{id = "tool", title = "Tool box", on_action = act, on_close = cls, tabs = {
    {id = "main", label = "Main", icon = "zap", rows = rows},
    {id = "more", label = "More", icon = "cable", rows = {{type = "text", label = "The second tab."}}}}}
  print("open " .. tostring(m ~= nil) .. " " .. tostring(why))
end)
eden.command("flat", "", function()
  local why
  m, why = eden.menu{title = "One page", rows = rows, on_action = act, on_close = cls}
  print("open " .. tostring(m ~= nil) .. " " .. tostring(why))
end)
eden.command("tabbed", "", function()
  local why
  m, why = eden.menu{title = "Tabs", on_action = act, on_close = cls, tabs = {
    {id = "one", label = "One", rows = rows}, {id = "two", label = "Two", rows = {{type = "heading", label = "Two"}}},
    {id = "three", label = "Three", icon = "nope_icon", rows = {{type = "heading", label = "Three"}}}}}
  print("open " .. tostring(m ~= nil) .. " " .. tostring(why))
end)
eden.command("last", "", function() print(table.concat(log, ",") .. " closes " .. closes); log = {} end)
eden.command("relabel", "", function(s) print("set " .. tostring(m:set("place", {label = s, detail = "there"}))) end)
eden.command("again", "", function()
  local a, b = eden.menu{title = "Second", rows = {{type = "heading", label = "x"}}}
  print("again " .. tostring(a) .. " " .. tostring(b))
end)
eden.command("big", "", function()
  local t = {}
  for i = 1, 3 do
    local r = {}
    for j = 1, (i == 3 and 33 or 48) do r[j] = {type = "row", id = "r" .. i .. "_" .. j, label = "Row " .. j} end
    t[i] = {id = "t" .. i, label = "T" .. i, rows = r}
  end
  local ok, e = pcall(eden.menu, {title = "Big", tabs = t})
  local ok2, e2 = pcall(eden.menu, {title = "Bad", rows = {{type = "row", id = "a", label = string.rep("x", 41)}}})
  local ok3, e3 = pcall(eden.menu, {title = "Bad", rows = {{type = "spinner", id = "a", label = "x"}}})
  local ok4, e4 = pcall(eden.menu, {title = "Bad", rows = {{type = "row", id = "a", label = "x", colour = 1}}})
  local ok5, e5 = pcall(eden.menu, {title = "Bad", rows = {{type = "row", id = "a", label = "x"}, {type = "toggle", id = "a", label = "y"}}})
  print(table.concat({tostring(ok) .. ":" .. tostring(e), tostring(ok2) .. ":" .. tostring(e2), tostring(ok3) .. ":" .. tostring(e3),
                      tostring(ok4) .. ":" .. tostring(e4), tostring(ok5) .. ":" .. tostring(e5)}, "|"))
end)
eden.command("closeit", "", function() print("close " .. tostring(m:close())) end)
eden.on("leave", function()
  local ok, e = pcall(eden.menu, {title = "Late", rows = {{type = "heading", label = "x"}}})
  eden.store.set("menuleave", tostring(ok) .. " " .. tostring(e))
end)
eden.command("mget", "", function() print(tostring(eden.store.get("menuleave"))) end)
eden.command("storeh", "", function(x, y, z)   -- K.6-4a leg 29: eden.store refuses a creature handle, also inside a table
  local h = eden.spawn("moof", tonumber(x), tonumber(y), tonumber(z))
  local ok, why = eden.store.set("h", h)
  local ok2, why2 = eden.store.set("t", {pet = h})
  if h then h:remove() end
  print(table.concat({type(h), tostring(ok), tostring(why), tostring(ok2), tostring(why2), tostring(eden.store.get("h"))}, "|"))
end)
)";

// K.8-7: a one-command pack whose answer the test edits on disk.
std::string verPack(const char* v) { return std::string("eden.command(\"ver\", \"\", function() print(\"") + v + "\") end)\n"; }

}  // namespace

int run_lua_selftest() {
    g_selftestTag = "eden-lua";
    char d[2048];
    const std::string docs = eden_platform_documents_root();
    const std::string packs = docs + "/Packs";
    if (!mutation_known("eden-lua", "--lua-mutate", g_opt.luaMutate, {"hook", "sandbox", "loadearly", "caperror", "usefilter", "signnocache", "storenotdirty", "leaveaftersave", "toastflood", "dialognoqueue", "dialognoclamp", "nogen", "nonfinite", "lookbuild", "noreport"})) return 2;
    const bool mutHook = g_opt.luaMutate == "hook", mutSandbox = g_opt.luaMutate == "sandbox";
    ModScript::testLoadEarly = g_opt.luaMutate == "loadearly";
    ModScript::testCapError = g_opt.luaMutate == "caperror";
    ModScript::testUseFilter = g_opt.luaMutate == "usefilter";
    ModScript::testStoreNotDirty = g_opt.luaMutate == "storenotdirty";
    ModScript::testLeaveAfterSave = g_opt.luaMutate == "leaveaftersave";
    ModScript::testToastFlood = g_opt.luaMutate == "toastflood";
    ModScript::testDialogNoQueue = g_opt.luaMutate == "dialognoqueue";
    ModScript::testNoGen = g_opt.luaMutate == "nogen";
    ModScript::testNonFinite = g_opt.luaMutate == "nonfinite";
    ModScript::testLookBuild = g_opt.luaMutate == "lookbuild";
    ModScript::testNoReport = g_opt.luaMutate == "noreport";
    GLDialog::testNoClamp = g_opt.luaMutate == "dialognoclamp";
    ModScript::testNoHook = mutHook;
    SignTool::testNoCache = g_opt.luaMutate == "signnocache";
    ModScript::testNoSandbox = mutSandbox;
    eden_fs_remove_tree(packs);
    bool wrote = writeFile(packs + "/aa_test/pack.json",
                           "{\"format\": 1, \"id\": \"aa_test\", \"version\": \"1\", \"scripts\": [\"scripts/lib.lua\", \"scripts/main.lua\"], \"blocks\": ["
                           "{\"id\": \"lever\", \"faces\": {\"all\": \"eden:stone\"}, \"color\": [200, 200, 200], \"traits\": [\"solid\", \"usable\"], "
                           "\"sounds\": \"eden:stone\", \"fallback\": \"eden:stone\"}]}\n") &&
                 writeFile(packs + "/aa_test/scripts/lib.lua", kTestLib) && writeFile(packs + "/aa_test/scripts/main.lua", kTestMain) &&
                 writeFile(packs + "/ab_bomb/pack.json", "{\"format\": 1, \"id\": \"ab_bomb\", \"scripts\": [\"bomb.lua\"]}\n") &&
                 writeFile(packs + "/ab_bomb/bomb.lua", kBomb) &&
                 writeFile(packs + "/ac_spin/pack.json", "{\"format\": 1, \"id\": \"ac_spin\", \"scripts\": [\"spin.lua\"]}\n") &&
                 writeFile(packs + "/ac_spin/spin.lua", kSpin) &&
                 writeFile(packs + "/ad_bad/pack.json", "{\"format\": 1, \"id\": \"ad_bad\", \"scripts\": [\"bad.lua\"]}\n") &&
                 writeFile(packs + "/ad_bad/bad.lua", "this is not lua\n") &&
                 writeFile(packs + "/ae_load/pack.json", "{\"format\": 1, \"id\": \"ae_load\", \"scripts\": [\"a.lua\"]}\n") &&
                 writeFile(packs + "/ae_load/a.lua", loadPack("ae", ModScript::limits().packPerFrame, true)) &&
                 writeFile(packs + "/af_load/pack.json", "{\"format\": 1, \"id\": \"af_load\", \"scripts\": [\"a.lua\"]}\n") &&
                 writeFile(packs + "/af_load/a.lua", loadPack("af", ModScript::limits().packPerFrame, true)) &&
                 writeFile(packs + "/ag_load/pack.json", "{\"format\": 1, \"id\": \"ag_load\", \"scripts\": [\"a.lua\"]}\n") &&
                 writeFile(packs + "/ag_load/a.lua", loadPack("ag", ModScript::limits().packPerFrame, true)) &&
                 writeFile(packs + "/ah_load/pack.json", "{\"format\": 1, \"id\": \"ah_load\", \"scripts\": [\"a.lua\"]}\n") &&
                 writeFile(packs + "/ah_load/a.lua", loadPack("ah", ModScript::limits().packPerFrame, false)) &&
                 writeFile(packs + "/ai_cap/pack.json", "{\"format\": 1, \"id\": \"ai_cap\", \"scripts\": [\"a.lua\"]}\n") &&
                 writeFile(packs + "/ai_cap/a.lua", kCap) &&
                 writeFile(packs + "/aj_one/pack.json", "{\"format\": 1, \"id\": \"aj_one\", \"scripts\": [\"a.lua\"]}\n") &&
                 writeFile(packs + "/aj_one/a.lua", kGo) &&
                 writeFile(packs + "/ak_two/pack.json", "{\"format\": 1, \"id\": \"ak_two\", \"scripts\": [\"a.lua\"]}\n") &&
                 writeFile(packs + "/ak_two/a.lua", kGo) &&
                 writeFile(packs + "/al_mem/pack.json", "{\"format\": 1, \"id\": \"al_mem\", \"scripts\": [\"a.lua\"]}\n") &&
                 writeFile(packs + "/al_mem/a.lua", kMem) &&
                 writeFile(packs + "/am_mem/pack.json", "{\"format\": 1, \"id\": \"am_mem\", \"scripts\": [\"a.lua\"]}\n") &&
                 writeFile(packs + "/am_mem/a.lua", kMem) &&
                 writeFile(packs + "/an_mem/pack.json", "{\"format\": 1, \"id\": \"an_mem\", \"scripts\": [\"a.lua\"]}\n") &&
                 writeFile(packs + "/an_mem/a.lua", kMem) &&
                 writeFile(packs + "/ao_rel/pack.json", "{\"format\": 1, \"id\": \"ao_rel\", \"scripts\": [\"a.lua\"]}\n") &&
                 writeFile(packs + "/ao_rel/a.lua", verPack("v1")) &&
                 writeFile(packs + "/ap_rel/pack.json", "{\"format\": 1, \"id\": \"ap_rel\", \"scripts\": [\"a.lua\"]}\n") &&
                 writeFile(packs + "/ap_rel/a.lua", verPack("v1")) &&
                 writeFile(packs + "/aq_rel/pack.json", "{\"format\": 1, \"id\": \"aq_rel\", \"scripts\": [\"a.lua\"]}\n") &&
                 writeFile(packs + "/aq_rel/a.lua", verPack("v1")) &&
                 writeFile(packs + "/ar_store/pack.json", "{\"format\": 1, \"id\": \"ar_store\", \"scripts\": [\"a.lua\"]}\n") &&
                 writeFile(packs + "/ar_store/a.lua", kStore) &&
                 writeFile(packs + "/as_bad/pack.json", "{\"format\": 1, \"id\": \"as_bad\", \"scripts\": [\"a.lua\"]}\n") &&
                 writeFile(packs + "/as_bad/a.lua", kStoreBad) &&
                 writeFile(packs + "/at_leave/pack.json", "{\"format\": 1, \"id\": \"at_leave\", \"scripts\": [\"a.lua\"]}\n") &&
                 writeFile(packs + "/at_leave/a.lua", kLeave) &&
                 writeFile(packs + "/au_dlg/pack.json", "{\"format\": 1, \"id\": \"au_dlg\", \"scripts\": [\"a.lua\"]}\n") &&
                 writeFile(packs + "/au_dlg/a.lua", kDlg) &&
                 writeFile(packs + "/av_dlg/pack.json", "{\"format\": 1, \"id\": \"av_dlg\", \"scripts\": [\"a.lua\"]}\n") &&
                 writeFile(packs + "/av_dlg/a.lua", kDlg) &&
                 writeFile(packs + "/ax_menu/pack.json", "{\"format\": 1, \"id\": \"ax_menu\", \"scripts\": [\"a.lua\"]}\n") &&
                 writeFile(packs + "/ax_menu/a.lua", kMenu) &&
                 writeFile(packs + "/aw_set/pack.json",
                           "{\"format\": 1, \"id\": \"aw_set\", \"scripts\": [\"a.lua\"], \"settings\": ["
                           "{\"id\": \"speed\", \"type\": \"slider\", \"label\": \"Speed\", \"min\": 0.5, \"max\": 4, \"step\": 0.5, \"default\": 1},"
                           "{\"id\": \"glow\", \"type\": \"toggle\", \"label\": \"Glow\", \"default\": true},"
                           "{\"id\": \"mode\", \"type\": \"choice\", \"label\": \"Mode\", \"options\": [\"Simple\", \"Full\"], \"default\": \"Simple\"},"
                           "{\"id\": \"sign\", \"type\": \"text\", \"label\": \"Greeting\", \"max\": 8, \"default\": \"Hello\"},"
                           "{\"id\": \"broken\", \"type\": \"slider\", \"label\": \"Broken\", \"min\": 0, \"max\": 4, \"step\": 1, \"default\": 2}]}\n") &&
                 writeFile(packs + "/aw_set/a.lua", kSet) &&
                 eden_fs_make_dirs(packs + "/.settings") &&
                 writeFile(packs + "/.settings/aw_set.json",
                           "{\"format\": 1, \"values\": {\"speed\": 2, \"glow\": false, \"mode\": \"Full\", \"sign\": \"far too long\", \"broken\": 2.5, \"gone\": 1}}\n");
    check(wrote, "the test packs are written to Documents/Packs", packs.c_str());
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

    // ---- 13. K.8-1: `load` fires once, after a top level the budget paused (ae, af, ag) or starved (ah)
    {
        std::string all, bad;
        for (const char* id : {"ae", "af", "ag", "ah"}) {
            const std::string out = mod(std::string("probe_") + id);
            int loads = -1;
            double top = -1, load = -1;
            std::sscanf(out.c_str(), "loads %d top %lf load %lf", &loads, &top, &load);
            all += std::string(id) + ": " + out + "; ";
            if (loads != 1 || load < top) bad += id;
            if (std::string(id) != "ah" && top < 1.5 / 60) bad += std::string(id) + "(not paused)";   // 3 x packPerFrame: >= 3 frames
            if (std::string(id) == "ah" && top < 0.5 / 60) bad += "ah(not starved)";                 // its top level began a frame or more late
        }
        check(bad.empty(), "`load` fires exactly once, after a top level paused by packPerFrame or starved by allPerFrame", (all + "bad: " + bad).c_str());
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
                                 "|build " + at + " " + leaves + "|use " + at + " " + leaves + " fire|burn " + at + " " + leaves;
        std::snprintf(d, sizeof(d), "got  %s\nwant %s", got.c_str(), want.c_str());
        check(got == want, "the player's build/paint/mine/burn reach the handlers once each, in order; script edits do not", d);

        // K.6-1a: a fire tap fires `use` (how = "fire") on any non-air block; `burn` still only on a flammable one, after `use`
        mod("probe", {"log"});
        hud->blocktype = TYPE_STONE;
        MpWorld::intentBuild(x, z, Y);
        MpWorld::intentBurn(x, z, Y);
        tick(2);
        const std::string stoneLog = mod("probe", {"log"});
        mod("probe", {"log"});
        hud->blocktype = TYPE_WOOD;
        MpWorld::intentBuild(x, z, Y);
        MpWorld::intentBurn(x, z, Y);
        hud->blocktype = keep;
        tick(2);
        const std::string woodLog = mod("probe", {"log"});
        const std::string stone = BlockRegistry::stockName(TYPE_STONE), wood = BlockRegistry::stockName(TYPE_WOOD);
        std::snprintf(d, sizeof(d), "stone: %s\nwood: %s", stoneLog.c_str(), woodLog.c_str());
        check(stoneLog == "log build " + at + " " + stone + "|use " + at + " " + stone + " fire" &&
                  woodLog == "log build " + at + " " + wood + "|use " + at + " " + wood + " fire|burn " + at + " " + wood,
              "a fire tap fires `use` (how=\"fire\") on stone (no burn) and on wood (use, then burn), once each", d);
        // K.6-1b: a build tap on a block with the `usable` trait fires `use` (how="tap") once and changes no cell
        {
            const std::string setL = lua("return eden.set(" + n3(x, Y, z) + ", 'aa_test:lever')");
            const std::string lid = lua("return eden.blockid('aa_test:lever')");
            const int lt = std::atoi(lid.c_str());
            const bool usable = lt >= STOCK_BLOCK_MAX + 1 && (blockinfo[lt] & IS_USABLE) && !(blockinfo[TYPE_STONE] & IS_USABLE);
            const int before = World::getWorld->terrain->getLand(x, z, Y);
            mod("probe", {"log"});
            MpWorld::intentUse(x, z, Y);
            tick(2);
            const std::string tapLog = mod("probe", {"log"});
            const bool same = World::getWorld->terrain->getLand(x, z, Y) == before && before == lt;
            std::snprintf(d, sizeof(d), "set '%s' id '%s' usable %d same %d: %s", setL.c_str(), lid.c_str(), usable, same, tapLog.c_str());
            check(usable && same && tapLog == "log use " + at + " aa_test:lever tap",
                  "a tap on a usable mod block fires `use` once with how=\"tap\" and changes no cell; stone has no such trait", d);
        }
        const std::string ver = lua("return eden.version");
        check(ver == "2", "eden.version == 2 (K.6-6)", ver.c_str());
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

    // ---- 14. K.8-2: 64 waiting handlers + a tick: the tick is skipped, not an error; it runs again once slots free
    {
        const std::string t0s = mod("captick");
        int t0 = 0, t1 = 0;
        std::sscanf(t0s.c_str(), "ticks %d", &t0);
        for (int i = 0; i < ModScript::MAX_COROUTINES; ++i) mod("hold");
        std::vector<ModScript::Info> iv = ModScript::info();
        const ModScript::Info* c = infoOf(iv, "ai_cap");
        const int live = c ? c->coroutines : -1;
        tick(300);   // the holds wait 6 s = 360 frames
        iv = ModScript::info();
        c = infoOf(iv, "ai_cap");
        const int skipped = c ? c->skipped : -1, errors = c ? c->errors : -1;
        const bool stopped = c ? c->stopped : true;
        tick(200);
        const std::string t1s = mod("captick");
        std::sscanf(t1s.c_str(), "ticks %d", &t1);
        iv = ModScript::info();
        c = infoOf(iv, "ai_cap");
        std::snprintf(d, sizeof(d), "live %d; after 300 frames skipped %d errors %d stopped %d; ticks %d -> %d; why %s", live, skipped, errors,
                      stopped, t0, t1, c ? c->why.c_str() : "?");
        check(live == ModScript::MAX_COROUTINES && !stopped && errors == 0 && skipped > 100 && c && !c->stopped && t1 - t0 >= 100,
              "reaching the 64-coroutine cap skips ticks (counted), never an error; the tick runs again once slots free", d);
    }

    // ---- 15. K.8-4: two packs register `go`: both files run to the end; `mod go` is the first's, `mod ak_two:go` the second's
    {
        const std::vector<ModScript::Info> iv = ModScript::info();
        const ModScript::Info* a = infoOf(iv, "aj_one");
        const ModScript::Info* b = infoOf(iv, "ak_two");
        const std::string shortGo = mod("go"), second = mod("ak_two:go"), first = mod("aj_one:go"), endA = mod("aj_one:end_aj_one"), endB = mod("ak_two:end_ak_two");
        const std::string nope = mod("ak_two:end_aj_one"), none = mod("zz_none:go");
        std::snprintf(d, sizeof(d), "go -> %s; ak_two:go -> %s; aj_one:go -> %s; ends %s / %s; wrong pack %s, no pack %s; errors %d/%d; %s", shortGo.c_str(),
                      second.c_str(), first.c_str(), endA.c_str(), endB.c_str(), nope.c_str(), none.c_str(), a ? a->errors : -1, b ? b->errors : -1, infoText().c_str());
        check(a && b && !a->stopped && !b->stopped && a->errors == 0 && b->errors == 0 && shortGo == "go from aj_one" && second == "go from ak_two" &&
                  first == "go from aj_one" && endA == "end aj_one" && endB == "end ak_two" && nope.find("no command") != std::string::npos &&
                  none.find("no command") != std::string::npos,
              "a short command name two packs register: both files run to the end; `mod go` runs the first's, `mod pack:go` always that pack's", d);
    }

    // ---- 16. K.8-5: three packs take 6 MiB each under a 12 MiB total: the third gets "not enough memory" and lives on
    {
        const size_t keepPer = lim.memBytes, keepAll = lim.allMemBytes, base = ModScript::totalMem();
        lim.memBytes = 8u << 20;
        lim.allMemBytes = base + (13u << 20);   // two 6 MiB strings fit, a third does not
        const std::string r1 = mod("al_mem:grab"), r2 = mod("am_mem:grab"), r3 = mod("an_mem:grab");
        const size_t held = ModScript::totalMem();
        std::vector<ModScript::Info> iv = ModScript::info();
        const ModScript::Info *a = infoOf(iv, "al_mem"), *b = infoOf(iv, "am_mem"), *c3 = infoOf(iv, "an_mem");
        const bool shape = a && b && c3 && a->mem >= (6u << 20) && b->mem >= (6u << 20) && c3->mem < (3u << 20) && c3->memRefusals > 0 &&
                           a->memRefusals == 0 && b->memRefusals == 0 && !a->stopped && !b->stopped && !c3->stopped;
        const std::string still = mod("an_mem:still"), still1 = mod("al_mem:still");
        mod("al_mem:drop");
        const std::string again = mod("an_mem:grab");   // al_mem let go of 6 MiB: now the third fits
        mod("am_mem:drop");
        mod("an_mem:drop");
        std::snprintf(d, sizeof(d), "%s | %s | %s; total %zu KiB of %zu KiB; shape %d; still %s / %s; after a drop: %s", r1.c_str(), r2.c_str(), r3.c_str(), held / 1024,
                      lim.allMemBytes / 1024, shape, still.c_str(), still1.c_str(), again.c_str());
        check(r1 == "grabbed al_mem" && r2 == "grabbed am_mem" && r3.find("not enough memory") != std::string::npos && shape &&
                  held <= lim.allMemBytes && still == "still an_mem" && still1 == "still al_mem" && again == "grabbed an_mem",
              "a total memory cap across all states: the third pack gets \"not enough memory\" and lives on, the others are untouched", d);
        lim.memBytes = keepPer;
        lim.allMemBytes = keepAll;
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

    // ---- 17. K.8-7: `scripts reload` reads the script files again: new behaviour, a syntax error stops that pack with its line,
    // an oversized file is refused and the old sources are kept; the other packs restart
    {
        tick(30);   // the restart in leg 12 left the burner packs' top levels spread over a few frames
        std::string hashBefore;
        for (const ContentPack::ModPack& p : ContentPack::modReport().packs) if (p.id == "ao_rel") hashBefore = p.hash;
        const std::string v0 = mod("ao_rel:ver");
        writeFile(packs + "/ao_rel/a.lua", verPack("v2"));
        writeFile(packs + "/ap_rel/a.lua", "eden.command(\"ver\", \"\", function() print(\"v2\")\n");   // no `end)`
        writeFile(packs + "/aq_rel/a.lua", verPack("v2") + "-- " + std::string(300 * 1024, 'x') + "\n");
        const std::string stale = mod("ao_rel:ver");   // not until the reload
        Console::run("scripts reload");
        tick(30);
        const std::string v1 = mod("ao_rel:ver"), vq = mod("aq_rel:ver"), vp = mod("ap_rel:ver");
        const std::vector<ModScript::Info> iv = ModScript::info();
        const ModScript::Info* p = infoOf(iv, "ap_rel");
        const std::string counts = mod("probe", {"counts"});
        std::string hashAfter;
        for (const ContentPack::ModPack& mp : ContentPack::modReport().packs) if (mp.id == "ao_rel") hashAfter = mp.hash;
        std::snprintf(d, sizeof(d), "before %s, stale %s; after: ao_rel %s, aq_rel %s, ap_rel %s; ap_rel %s; probe %s; hash same %d", v0.c_str(), stale.c_str(), v1.c_str(), vq.c_str(),
                      vp.c_str(), p ? (p->stopped ? ("stopped: " + p->why).c_str() : "running") : "?", counts.c_str(), hashBefore == hashAfter);
        check(v0 == "v1" && stale == "v1" && v1 == "v2" && vq == "v1" && vp.find("no command") != std::string::npos && p && p->stopped &&
                  p->why.find("does not compile") != std::string::npos && counts == "order lib,main loads 1" && !hashBefore.empty() && hashBefore == hashAfter,
              "`scripts reload` re-reads the script files: new behaviour; a syntax error stops that pack; an oversized file is refused (old sources kept); the hash stays", d);
    }

    // ---- 19. K.6-3: eden.sign / eden.setsign / the `sign` event, through SignTool's own commit
    {
        World* w = World::getWorld;
        WorldTrailer* tr = w->fm->trailer();
        const int Y = (int)w->player->pos.y + 2;   // beside the player's own height: in the sign radius (shadows leg 3's Y)
        const int ax = px + 8, az = pz - 8, bx = px + 10;   // A: set from Lua; B: typed by hand (same text, same paint)
        const std::string A = n3(ax, Y, az), B = n3(bx, Y, az);
        lua("eden.set(" + A + ", \"stone\"); eden.set(" + B + ", \"stone\")");
        const size_t n0 = tr->signCount();
        const float yaw = w->player->yaw;
        const std::string s1 = lua("return eden.setsign(" + A + ", 3, \"Hello Lua\", 7)");
        const std::string r1 = lua("return eden.sign(" + A + ")");
        const std::string wall = lua("return eden.setsign(" + A + ", 1, \"wall\"), eden.sign(" + A + ", 1)");
        const std::string first = lua("return eden.sign(" + A + ")");   // no face: the first record on the block
        // refusals, each with a why, none changing anything
        const std::string c95 = lua("return eden.setsign(" + A + ", 4, string.rep(\"x\", 95))");
        const std::string r95 = lua("return #eden.sign(" + A + ", 4)");
        const std::string c96 = lua("return eden.setsign(" + A + ", 4, string.rep(\"x\", 96))");
        const std::string ctl = lua("return eden.setsign(" + A + ", 4, \"a\\1b\")");
        const std::string nul = lua("return eden.setsign(" + A + ", 4, \"a\\0b\")");
        const std::string hi = lua("return eden.setsign(" + A + ", 4, \"caf\\195\\169\")");
        const std::string under = lua("return eden.setsign(" + A + ", 2, \"x\")");
        const std::string badc = lua("return eden.setsign(" + A + ", 4, \"x\", 99)");
        const std::string air = lua("return eden.setsign(" + n3(ax, Y + 5, az) + ", 3, \"floating\")");
        const std::string airGet = lua("return eden.sign(" + n3(ax, Y + 5, az) + ")");
        const std::string still = lua("return eden.sign(" + A + ", 4)");
        // an empty text deletes (and deleting nothing is not an error)
        const std::string del = lua("return eden.setsign(" + A + ", 4, \"\"), eden.sign(" + A + ", 4)");
        const std::string del2 = lua("return eden.setsign(" + A + ", 5, \"\")");
        const size_t nAfterDel = tr->signCount();
        // the same sign typed by hand at B, with the same paint, yaw and text
        mod("probe", {"log"});
        Hud* hud = w->hud;
        hud->block_paintcolor = 7;
        SignTool::use(bx, az, Y, 0, 1, 0, yaw);
        SignTool::answer(0, "Hello Lua");
        if (GLDialog::active()) GLDialog::dismiss();
        hud->block_paintcolor = 0;
        SignTool::use(bx, az, Y, 0, 1, 0, yaw);
        SignTool::answer(0, "Edited by hand");
        if (GLDialog::active()) GLDialog::dismiss();
        tick(2);
        const std::string evLog = mod("probe", {"log"});
        const int ia = tr->findSign(TrailerPos::fromTerrainArgs(ax, az, Y), 3), ib = tr->findSign(TrailerPos::fromTerrainArgs(bx, az, Y), 3);
        std::snprintf(d, sizeof(d), "set %s; read %s; wall %s; first %s; 95: %s len %s; 96: %s; ctl: %s; nul: %s; high: %s; bottom: %s; colour: %s; no anchor: %s / get '%s'; refused kept %s; "
                      "delete %s, again %s; signs %zu -> %zu; hand %s; log %s",
                      s1.c_str(), r1.c_str(), wall.c_str(), first.c_str(), c95.c_str(), r95.c_str(), c96.c_str(), ctl.c_str(), nul.c_str(), hi.c_str(),
                      under.c_str(), badc.c_str(), air.c_str(), airGet.c_str(), still.c_str(), del.c_str(), del2.c_str(), n0, nAfterDel,
                      ib >= 0 ? "ok" : "none", evLog.c_str());
        auto has = [](const std::string& hay, const char* n) { return hay.find(n) != std::string::npos; };
        check(s1 == "true" && r1 == "Hello Lua\t7\t3" && has(wall, "true") && first == "Hello Lua\t7\t3" && wall.find("wall\t45\t1") != std::string::npos &&
                  c95 == "true" && r95 == "95" && c96.compare(0, 5, "false") == 0 && has(c96, "95 bytes") &&
                  ctl.compare(0, 5, "false") == 0 && has(ctl, "printable") && nul.compare(0, 5, "false") == 0 && hi.compare(0, 5, "false") == 0 &&
                  under.compare(0, 5, "false") == 0 && badc.compare(0, 5, "false") == 0 && has(badc, "colour") &&
                  air.compare(0, 5, "false") == 0 && has(air, "no block there") && airGet.empty() && still.compare(0, 3, "xxx") == 0 &&
                  del == "true" && del2 == "true" && nAfterDel == n0 + 2 && ia >= 0 && ib >= 0 &&
                  evLog == "log sign " + std::to_string(bx) + " " + std::to_string(Y) + " " + std::to_string(az) + " 3 Hello Lua|sign " +
                      std::to_string(bx) + " " + std::to_string(Y) + " " + std::to_string(az) + " 3 Edited by hand",
              "eden.sign/setsign: set and read, faces, 95 bytes ok, 96 / control / NUL / high bytes / bottom / bad colour / no anchor refused with a why, empty deletes; the player's place and edit fire `sign`, a script's own does not", d);
        // Lua vs hand: the same record (text, face, paint, facing)
        tr->setSignText((size_t)ib, "Hello Lua");
        const TrailerSign &sa = tr->sign((size_t)ia), &sb = tr->sign((size_t)ib);
        std::snprintf(d, sizeof(d), "Lua a=%d b=%d c=%d '%s'; hand a=%d b=%d c=%d '%s'", sa.a, sa.b, sa.c, sa.text, sb.a, sb.b, sb.c, sb.text);
        check(sa.a == sb.a && sa.b == sb.b && sa.c == sb.c && !std::strcmp(sa.text, sb.text) && sb.b == 7,
              "a sign set from Lua is the record the same sign typed by hand makes", d);

        // The renderer: the new text is what it shows, and an edit drops the old text's texture
        tick(6);
        const bool shownOld = SignRenderer::cached("Hello Lua");
        const int withText = SignRenderer::stats().withText;
        lua("eden.setsign(" + A + ", 3, \"Second text\")");
        lua("eden.setsign(" + B + ", 3, \"\")");   // B's "Hello Lua" goes too: nothing shows the old text now
        const bool oldGone = !SignRenderer::cached("Hello Lua");   // before any frame: the edit itself dropped it
        tick(6);
        const bool newShown = SignRenderer::cached("Second text") && SignRenderer::stats().withText >= 1;
        std::snprintf(d, sizeof(d), "old text cached before %d (withText %d), dropped by the edit %d, new text drawn %d", shownOld, withText, oldGone, newShown);
        check(shownOld && oldGone && newShown, "SignRenderer shows a Lua-set sign's text; editing it drops the old text's cached texture (--lua-mutate=signnocache must FAIL)", d);

        // save -> reopen -> read
        lua("eden.setsign(" + A + ", 5, \"kept\", 12)");
        save_world();
        const bool quit = quit_to_menu();
        tick(10);
        const bool reopened = open_world(name, g_opt.height);
        tick(g_opt.frames / 2);
        const std::string back = lua("return eden.sign(" + A + ", 3)"), back5 = lua("return eden.sign(" + A + ", 5)"), gone = lua("return eden.sign(" + B + ")");
        std::snprintf(d, sizeof(d), "quit %d reopened %d; top '%s'; +z '%s'; deleted '%s'", quit, reopened, back.c_str(), back5.c_str(), gone.c_str());
        check(quit && reopened && back == "Second text\t7\t3" && back5 == "kept\t12\t5" && gone.empty(),
              "set, save, reopen: the signs read back (text, colour, face); the deleted one stays gone", d);
    }

    // ---- 22. K.6-5: eden.store, saved in the world's MOD_DATA record (13)
    {
        World* w = World::getWorld;
        const std::string path = w->fm->documents + "/" + w->terrain->world_name;
        auto readRec = [&](std::vector<emod::ModDataEntry>* es) {   // the file's live MOD_DATA, parsed (world closed)
            emod::EdenWorldStore st;
            std::vector<uint8_t> rec;
            es->clear();
            return st.open(path.c_str(), false) && st.hasRecord(emod::T_MOD_DATA) &&
                   st.readRecord(emod::T_MOD_DATA, rec) == emod::EdenWorldStore::READ_OK && emod::mod_data_parse(rec.data(), rec.size(), es, NULL);
        };
        auto entryOf = [](const std::vector<emod::ModDataEntry>& es, const char* id) -> const emod::ModDataEntry* {
            for (const emod::ModDataEntry& e : es) if (e.id == id) return &e;
            return NULL;
        };
        const std::string put = mod("sput"), want = mod("vdump"), now = mod("sget");
        const std::string caps = mod("scaps");
        const std::string afterCaps = mod("sget");
        std::snprintf(d, sizeof(d), "sput '%s'; set then get %s (%zu chars); caps: %s; unchanged by the refusals %d", put.c_str(),
                      now == want ? "same" : "DIFFERS", want.size(), caps.c_str(), afterCaps == want);
        auto has = [](const std::string& hay, const std::string& n) { return hay.find(n) != std::string::npos; };
        check(put == "sput ok" && now == want && afterCaps == want && has(want, "big=i9223372036854775807") && has(want, "odd=i9007199254740993") &&
                  has(want, "whole=f0x1p+1") && has(want, "negz=f-0x0p+0") && has(want, "bin=s15:000102ff80808080808080808080") &&
                  has(want, "nest={i-3=s3:6e6567,i1=i1,i2=f0x1.4p+1,i3=s5:7468726565,i4={s4:64656570={s6:646565706572={i1=s4:666f7572}}},i10=i10,s1:78=true}"),
              "eden.store.set/get: every value type reads back exactly (integers stay integers, floats bit for bit, binary strings, nested tables 4 deep)", d);
        check(caps.compare(0, 1, "f") == 0 &&
                  has(caps, "false:can't store a function|false:can't store a number that is not finite|false:can't store a number that is not finite|"
                            "false:can't store a string over 4096 bytes|false:can't store tables nested more than 4 deep|false:can't store tables nested more than 4 deep|"
                            "false:can't store a table over 256 entries|false:a table key must be a string of 1 to 64 bytes or an integer|"
                            "false:a table key must be a string of 1 to 64 bytes or an integer|false:can't store a thread|keyerr falsefalsefalsefalse|after nilnil|"
                            "del 5 nil|copy true|fill ") &&
                  has(caps, " this pack's store is full (64 KiB)|keys 15"),
              "eden.store refusals: a function, a non-finite number, a 4097-byte string, 5-deep or cyclic tables, 257 entries, a bad table key, "
              "a thread each return false, why; a bad store key raises; nil deletes; get returns a copy; the 64 KiB cap returns false, why", d);

        // save -> reopen: the values are the world's
        save_world();
        const bool quit1 = quit_to_menu();
        tick(10);
        std::vector<emod::ModDataEntry> es;
        const bool rec1 = readRec(&es);
        const emod::ModDataEntry* ar = entryOf(es, "ar_store");
        // a pack that is not installed (zz_gone) and a blob as_bad's runtime cannot decode (blob version 9)
        std::vector<uint8_t> gone(300), garbage = {9, 9, 9};
        for (size_t i = 0; i < gone.size(); ++i) gone[i] = (uint8_t)(i * 7 + 1);
        bool injected = false;
        {
            std::vector<emod::ModDataEntry> next = es;
            emod::ModDataEntry g, b;
            g.id = "zz_gone"; g.data = gone; b.id = "as_bad"; b.data = garbage;
            next.push_back(g);
            next.push_back(b);
            std::vector<uint8_t> out;
            emod::mod_data_build(next, out);
            emod::EdenWorldStore st;
            if (st.open(path.c_str(), true)) {
                st.beginBatch();
                injected = st.putRecord(emod::T_MOD_DATA, out.data(), out.size()) && st.commitBatch();
                st.close();
            }
        }
        const bool re1 = open_world(name, g_opt.height);
        tick(g_opt.frames / 2);
        const std::string back = mod("sget"), badKeys = mod("bkeys");
        std::snprintf(d, sizeof(d), "quit %d; record %d with ar_store %d (%zu B); injected %d; reopened %d; read back %s; as_bad '%s'", quit1, rec1,
                      ar != NULL, ar ? ar->data.size() : (size_t)0, injected, re1, back == want ? "same" : back.substr(0, 200).c_str(), badKeys.c_str());
        check(quit1 && rec1 && ar && injected && re1 && back == want && badKeys == "bkeys 0",
              "save, reopen: every stored value is back exactly; a blob that does not decode starts empty (--lua-mutate=storenotdirty must FAIL)", d);

        // two more saves (each rewrites the record): the absent pack's entry and the undecodable blob go out verbatim
        const std::string s1 = mod("sone", {"step", "1"});
        save_world();
        const std::string s2 = mod("sone", {"step", "2"});
        save_world();
        const bool quit2 = quit_to_menu();
        tick(10);
        const bool rec2 = readRec(&es);
        const emod::ModDataEntry *g2 = entryOf(es, "zz_gone"), *b2 = entryOf(es, "as_bad"), *a2 = entryOf(es, "ar_store");
        std::snprintf(d, sizeof(d), "sets '%s' '%s'; quit %d; record %d: zz_gone %s, as_bad %s, ar_store %zu B", s1.c_str(), s2.c_str(), quit2, rec2,
                      !g2 ? "missing" : g2->data == gone ? "same" : "DIFFERS", !b2 ? "missing" : b2->data == garbage ? "same" : "DIFFERS",
                      a2 ? a2->data.size() : (size_t)0);
        check(s1 == "true nil" && s2 == "true nil" && quit2 && rec2 && g2 && g2->data == gone && b2 && b2->data == garbage && a2 && ar &&
                  a2->data.size() > ar->data.size(),
              "two saves later: a pack that is not loaded keeps its bytes, an undecodable blob is kept until its pack stores something", d);

        // as_bad stores something: its blob is replaced by a valid one
        const bool re2 = open_world(name, g_opt.height);
        tick(g_opt.frames / 2);
        const std::string bset = mod("bset"), step = mod("sstep");
        save_world();
        const bool quit3 = quit_to_menu();
        tick(10);
        const bool rec3 = readRec(&es);
        const emod::ModDataEntry* b3 = entryOf(es, "as_bad");
        const bool re3 = open_world(name, g_opt.height);
        tick(g_opt.frames / 2);
        const std::string bkeys = mod("bkeys");
        std::snprintf(d, sizeof(d), "reopened %d; bset '%s'; %s; quit %d record %d as_bad %s; reopened %d '%s'", re2, bset.c_str(), step.c_str(), quit3, rec3,
                      !b3 ? "missing" : b3->data == garbage ? "still the old bytes" : "rewritten", re3, bkeys.c_str());
        check(re2 && bset == "bset true" && step == "step 2" && quit3 && rec3 && b3 && b3->data != garbage && b3->data.size() > 0 && b3->data[0] == 1 &&
                  re3 && bkeys == "bkeys 1",
              "once the pack stores something its blob is rewritten; the last of several saves wins", d);

        // the whole record's caps: 1 MiB (16 other packs, nearly full) and 64 packs (as a world from elsewhere could hold)
        {
            std::vector<emod::ModDataEntry> big;
            for (int i = 0; i < 16; ++i) {   // 15 x 64 KiB + 60,000 B: 5,052 B left for ar_store's entry, far under its own 64 KiB
                emod::ModDataEntry e;
                e.id = "zz_" + std::to_string(10 + i);
                e.data.assign(i < 15 ? emod::MOD_DATA_ENTRY_MAX : 60000u, 'm');
                big.push_back(e);
            }
            std::vector<uint8_t> r;
            emod::mod_data_build(big, r);
            ModScript::storeLoad(r.data(), r.size());
            const std::string fill = mod("sfill", {"g"});
            std::vector<emod::ModDataEntry> many;
            for (int i = 0; i < 64; ++i) { emod::ModDataEntry e; e.id = "p" + std::to_string(10 + i); many.push_back(e); }
            emod::mod_data_build(many, r);
            ModScript::storeLoad(r.data(), r.size());
            const std::string one = mod("sone", {"x", "1"});
            const bool clean = !ModScript::storeNeedsSave();
            // a session world loads none
            ModScript::worldEntered(true);
            ModScript::storeLoad(r.data(), r.size());
            const bool sessionNone = ModScript::storeEntries() == 0 && !ModScript::storeNeedsSave();
            ModScript::worldEntered(false);
            ModScript::storeLoad(NULL, 0);
            tick(3);
            std::snprintf(d, sizeof(d), "16 others filling 1 MiB: '%s'; 64 others: '%s'; nothing to save %d; session loads none %d", fill.c_str(), one.c_str(), clean, sessionNone);
            check(fill == "fill 1 this world's mod data is full (1 MiB)" && one == "false too many packs store data in this world (64)" && clean && sessionNone,
                  "the record's caps (1 MiB, 64 packs) return false, why and leave nothing to save; a session world loads no mod data", d);
        }
    }

    // ---- 23. K.6-2: `leave` runs before the exit save, once per real close; a warp fires nothing
    {
        const int leaves0 = ModScript::stats().leaves;
        const bool warped = World::getWorld->terrain->warpToHome();
        const bool back = tick_until([] { return World::getWorld->terrain->loaded; }, 5000, "the world back after a warp");
        tick(30);
        const int afterWarp = ModScript::stats().leaves;
        const PlayerState lp = player_state();
        const int bx = (int)lp.x + 4, bz = (int)lp.z - 6, by = g_opt.height - 12;
        const int fx = (int)lp.x - 48, fz = (int)lp.z + 32, fy = g_opt.height - 22;
        auto a = [](int v) { return std::to_string(v); };
        const std::string armed = mod("larm", {a(bx), a(by), a(bz), a(fx), a(fy), a(fz), mutHook ? "0" : "1"});   // (no hook: it would never end)
        const int before = eden_console_getblock(bx, bz, by);
        // a Save & Quit whose save failed (QA.5) keeps the world open, and the retry must fire `leave` again: the first
        // attempt is this direct call, the retry is the real Save & Quit below
        const int errs0 = ModScript::stats().errors;
        ModScript::leaving();
        const auto t0 = std::chrono::steady_clock::now();
        const bool quit = quit_to_menu();
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const int leaves1 = ModScript::stats().leaves, errs1 = ModScript::stats().errors;
        tick(10);
        const bool reopened = open_world(name, g_opt.height);
        tick(g_opt.frames / 2);
        const int brick = eden_console_getblock(bx, bz, by);
        const std::string got = mod("lget");
        std::snprintf(d, sizeof(d), "armed '%s'; before %d; warp %d back %d leaves %d -> %d; quit %d in %.2f s, leaves -> %d, errors +%d; reopened %d; block %d; '%s'",
                      armed.c_str(), before, warped, back, leaves0, afterWarp, quit, secs, leaves1, errs1 - errs0, reopened, brick, got.c_str());
        check(armed == "armed" && warped && back && afterWarp == leaves0, "a warp does not fire `leave` (scripts outlive it)", d);
        check(quit && reopened && leaves1 == leaves0 + 2 && before != TYPE_BRICK && brick == TYPE_BRICK && got.compare(0, 7, "left 2 ") == 0,
              "a `leave` handler's block and eden.store key are in the exit save; a second close fires it again (--lua-mutate=leaveaftersave must FAIL)", d);
        check(got.find("| false ") != std::string::npos && got.find("can't pause while the world closes") != std::string::npos,
              "eden.wait inside `leave` raises \"can't pause while the world closes\"", d);
        int cells = -1;
        std::sscanf(got.c_str() + (got.rfind("| ") == std::string::npos ? 0 : got.rfind("| ") + 2), "%d", &cells);
        check(cells > 0 && cells < 6 * 4096 && got.find("the world is closing") != std::string::npos,
              "edits in `leave` get one WORK_PER_FRAME; past it they are refused with a why, not carried over", d);
        check((mutHook || errs1 - errs0 >= 2) && secs < 20, "an endless `leave` handler is stopped at hardPerResume; the world still closes and saves", d);
    }

    // ---- 24. K.7-1a: eden.toast is the HUD status line, one a second a pack, 120 bytes
    {
        statusbar* sb = World::getWorld->hud->sb;
        sb->clear();
        const std::string first = mod("toast", {"hello"}), second = mod("toast", {"again"});
        const std::string* cur = sb->current();
        const std::string shown = cur ? *cur : "(none)";
        sb->setStatus("engine says", 3);   // an engine status replaces it
        const std::string* cur2 = sb->current();
        const bool replaced = cur2 && *cur2 == "engine says";
        tick(g_opt.frames);   // game time passes: the limit lifts
        const std::string l120 = mod("toastlong", {"120"}), l121 = mod("toastlong", {"121"});
        tick(g_opt.frames);
        const std::string later = mod("toast", {"later", "99"});
        const std::string* cur3 = sb->current();
        const std::string shown3 = cur3 ? *cur3 : "(none)";
        std::snprintf(d, sizeof(d), "first '%s', second '%s', shown '%s'; engine replaced %d; 120: '%s'; 121: '%s'; later '%s' shown '%s'",
                      first.c_str(), second.c_str(), shown.c_str(), replaced, l120.c_str(), l121.c_str(), later.c_str(), shown3.c_str());
        check(first == "toast true true" && shown == "hello" && replaced && later == "toast true true" && shown3 == "later",
              "eden.toast puts its text in the HUD status line; an engine status replaces it and a later toast replaces that", d);
        check(second == "toast true false" && l121.find("toastlong false") == 0 && l121.find("120 bytes") != std::string::npos && l120 == "toastlong true true",
              "a second toast within 1 s returns false (--lua-mutate=toastflood must FAIL); 120 bytes pass, 121 raises", d);
    }

    // ---- 25. K.7-1b: eden.ask / eden.input, GLDialog ownership, queue, gap, mute, clamp
    {
        const bool keepInput = g_tickInput;
        g_tickInput = true;
        auto tapBtn = [&](int j) { CGRect r; if (!GLDialog::buttonRect(j, &r)) return false; ui_tap_rect(r); tick(2); return true; };
        auto res = [&](const char* pk, const char* tag) { return mod(std::string("res_") + pk, {tag}); };
        auto clock = [&]() { return lua("return eden.clock()"); };
        char dd[1200];
        // a. ask pauses the handler and the world; button 2 answers it; no link on a pack's first dialog
        mod("ask_au_dlg", {"a", "2"});
        tick(1);
        const bool shown = GLDialog::active() && GLDialog::ownerToken() > 0;
        CGRect lr;
        const bool noLink = !GLDialog::footerLinkRect(&lr);
        const std::string c0 = clock();
        tick(30);
        const bool frozen = clock() == c0 && res("au_dlg", "a") == "nil";
        const bool tapped = tapBtn(1);
        tick(2);
        const std::string ra = res("au_dlg", "a");
        const bool closed = !GLDialog::active();
        std::snprintf(dd, sizeof dd, "shown %d noLink %d frozen %d (clock %s) tapped %d answer %s closed %d", shown, noLink, frozen, c0.c_str(), tapped, ra.c_str(), closed);
        check(shown && noLink && frozen && tapped && ra == "2/nil" && closed && clock() != c0,
              "eden.ask raises a GLDialog and the world stops (eden.clock holds); tapping button 2 resumes the handler with 2", dd);

        // b. the 0.5 s gap: the pack's next dialog is held, then shows with the mute link (second within 60 s)
        mod("ask_au_dlg", {"b"});
        tick(3);
        const bool held = !GLDialog::active();
        tick(40);
        const bool gapShown = GLDialog::active() && GLDialog::footerLinkRect(&lr);
        push_tap_key(SDL_SCANCODE_ESCAPE);
        tick(3);
        const std::string rb = res("au_dlg", "b");
        std::snprintf(dd, sizeof dd, "held %d shown+link %d escape answer %s", held, gapShown, rb.c_str());
        check(held && gapShown && rb == "3/nil",
              "a pack's next dialog waits 0.5 s of game time, then shows with the mute link; Escape answers with the last button", dd);

        // c. two packs in one frame: one at a time, the second shows after the first closes (dialognoqueue must FAIL)
        tick(40);
        mod("ask_au_dlg", {"c1"});
        mod("ask_av_dlg", {"c2"});
        tick(1);
        const int t1 = GLDialog::ownerToken();
        tick(5);
        const bool still = GLDialog::ownerToken() == t1 && t1 > 0;
        tapBtn(0);
        tick(2);
        const int t2 = GLDialog::ownerToken();
        const std::string r1 = res("au_dlg", "c1");
        tapBtn(2);
        tick(2);
        const std::string r2 = res("av_dlg", "c2");
        std::snprintf(dd, sizeof dd, "first token %d still %d, second token %d; answers %s / %s", t1, still, t2, r1.c_str(), r2.c_str());
        check(still && t2 > 0 && t2 != t1 && r1 == "1/nil" && r2 == "3/nil",
              "two packs asking in one frame: the second waits and shows after the first (--lua-mutate=dialognoqueue must FAIL)", dd);

        // d. an engine show() over a mod dialog resolves the waiter with nil, "replaced"
        tick(40);
        mod("ask_au_dlg", {"d"});
        tick(1);
        const bool upD = GLDialog::ownerToken() > 0;
        const char* eb[1] = {"OK"};
        GLDialog::show("Engine", "an engine dialog", eb, 1, NULL);
        const bool engineUp = GLDialog::active() && GLDialog::ownerToken() == 0;
        tick(2);   // (the world is frozen under the engine dialog: the waiter resumes once it is gone)
        GLDialog::dismiss();
        tick(2);
        const std::string rd = res("au_dlg", "d");
        std::snprintf(dd, sizeof dd, "mod up %d, engine up %d, answer %s", upD, engineUp, rd.c_str());
        check(upD && engineUp && rd == "nil/replaced", "an engine dialog over a mod dialog: the waiting handler gets nil, \"replaced\"", dd);

        // e. one pending per pack: a second call while one is queued is "busy"
        tick(40);
        mod("ask_au_dlg", {"e1"});
        mod("ask_au_dlg", {"e2"});
        const std::string re2 = res("au_dlg", "e2");
        tick(1);
        tapBtn(0);
        tick(2);
        std::snprintf(dd, sizeof dd, "second %s first %s", re2.c_str(), res("au_dlg", "e1").c_str());
        check(re2 == "nil/busy" && res("au_dlg", "e1") == "1/nil", "a pack with a request queued gets nil, \"busy\" for a second", dd);

        // f. eden.input: the answer is the text, Cancel is nil, "dismissed"; with no text entry nil, "unavailable"
        tick(40);
        mod("inp_au_dlg", {"f", "hi"});
        tick(1);
        const bool avail = GLDialog::textEntryAvailable();
        std::string rf;
        if (avail) {
            const bool up = GLDialog::promptField() != NULL;
            tapBtn(0);
            tick(2);
            rf = res("au_dlg", "f") + (up ? "" : " (no field)");
            tick(40);
            mod("inp_au_dlg", {"f2", "xy"});
            tick(1);
            tapBtn(1);
            tick(2);
            rf += " cancel " + res("au_dlg", "f2");
            check(rf == "hi/nil cancel nil/dismissed", "eden.input shows a prompt with the default; OK answers its text, Cancel nil, \"dismissed\"", rf.c_str());
        } else {
            rf = res("au_dlg", "f");
            check(rf == "nil/unavailable" && !GLDialog::active(), "eden.input on a host with no text entry returns nil, \"unavailable\" and shows nothing", rf.c_str());
        }

        // g. the mute link: nil, "muted" now and for every later call of that pack; another pack is not muted
        tick(40);
        mod("ask_au_dlg", {"g1"});
        tick(1);
        const bool link = GLDialog::footerLinkRect(&lr);
        if (link) { ui_tap_rect(lr); tick(2); }
        const std::string rg1 = res("au_dlg", "g1");
        mod("ask_au_dlg", {"g2"});
        tick(2);
        const std::string rg2 = res("au_dlg", "g2");
        const bool none = !GLDialog::active();
        mod("ask_av_dlg", {"g3"});
        tick(1);
        const bool otherUp = GLDialog::active();
        tapBtn(0);
        tick(2);
        std::snprintf(dd, sizeof dd, "link %d first %s, after %s, nothing shown %d; other pack shown %d", link, rg1.c_str(), rg2.c_str(), none, otherUp);
        check(link && rg1 == "nil/muted" && rg2 == "nil/muted" && none && otherUp,
              "the footer link mutes the pack for the rest of the world (nil, \"muted\"); other packs still ask", dd);

        // h. argument caps raise with a reason; a leave handler cannot pause
        const std::string bad = mod("bad_av_dlg");
        std::string dleave;
        ModScript::leaving();
        dleave = mod("dget_av_dlg");
        std::snprintf(dd, sizeof dd, "bad: %s ; leave: %s", bad.c_str(), dleave.c_str());
        check(bad.find("false:buttons must have 1 to 4 entries|false:title is longer than 60 bytes|false:body is longer than 400 bytes|"
                       "false:buttons must have 1 to 4 entries|false:buttons[1] is longer than 24 bytes|false:unknown key 'colour'|"
                       "false:danger is past the last button") != std::string::npos &&
              dleave.find("false") == 0 && dleave.find("can't pause while the world closes") != std::string::npos,
              "ask's caps raise with the path; inside `leave` it raises \"can't pause while the world closes\"", dd);

        // i. a body longer than the screen is cut to it (C-8): the last button stays on screen
        GLDialog::dismiss();
        const char* two[2] = {"Yes", "No"};
        std::string huge;
        while (huge.size() < 6000) huge += "lorem ipsum dolor ";
        GLDialog::show("A long one", huge.c_str(), two, 2, NULL);
        CGRect b0, b1;
        GLDialog::buttonRect(0, &b0);
        GLDialog::buttonRect(1, &b1);
        const CGRect safe = GLW::safeRect();
        const bool inside = b0.origin.y >= safe.origin.y && b1.origin.y >= safe.origin.y &&
                            b0.origin.y + b0.size.height <= safe.origin.y + safe.size.height;
        GLDialog::dismiss();
        std::snprintf(dd, sizeof dd, "buttons y %.0f / %.0f, safe y %.0f..%.0f", b0.origin.y, b1.origin.y, safe.origin.y, safe.origin.y + safe.size.height);
        check(inside, "a 6000-byte dialog body is cut with an ellipsis to the safe height: the buttons stay on screen (--lua-mutate=dialognoclamp must FAIL)", dd);
        g_tickInput = keepInput;
    }

    // ---- 26. K.7-2b: eden.setting reads Packs/.settings/<id>.json; a value that fails the schema is the default
    {
        const std::string sp = mod("setting", {"speed"}), gl = mod("setting", {"glow"}), md = mod("setting", {"mode"});
        const std::string sg = mod("setting", {"sign"}), br = mod("setting", {"broken"}), no = mod("setting", {"nope"});
        std::snprintf(d, sizeof(d), "speed '%s' glow '%s' mode '%s' sign '%s' broken '%s' nope '%s'", sp.c_str(), gl.c_str(), md.c_str(), sg.c_str(), br.c_str(), no.c_str());
        check(sp == "setting true number 2.0" && gl == "setting true boolean false" && md == "setting true string Full" &&
                  sg == "setting true string Hello" && br == "setting true number 2.0" && no.find("setting false") == 0 &&
                  no.find("no such setting") != std::string::npos,
              "eden.setting returns the stored value of each type; a value off the schema reads as the default; an undeclared id raises", d);
    }

    // ---- 27. K.7-3a: eden.menu, a mod's menu screen over the running world
    {
        const bool keepInput = g_tickInput;
        g_tickInput = true;
        char dd[1600];
        auto clock = [&]() { return lua("return eden.clock()"); };
        auto key = [&](SDL_Scancode sc) { push_tap_key(sc); tick(7); };
        auto rowRect = [&](int r) { CGRect c = CGRectMake(0, 0, 0, 0); ModMenuScreen::showRow(r); ModMenuScreen::rowControlRect(r, &c); return c; };
        auto inside = [](CGRect outer, CGRect in) {
            const float cx = in.origin.x + in.size.width * 0.5f, cy = in.origin.y + in.size.height * 0.5f;
            return cx >= outer.origin.x && cx <= outer.origin.x + outer.size.width && cy >= outer.origin.y && cy <= outer.origin.y + outer.size.height;
        };
        const PlayerState here = player_state();   // (earlier legs warp: leg 3's px/pz are stale)
        const int bx = (int)here.x + 5, bz = (int)here.z - 4, by = Y - 2;
        Console::run(("set =" + std::to_string(bx) + " =" + std::to_string(by) + " =" + std::to_string(bz) + " air").c_str());
        tick(2);
        const int before = eden_console_getblock(bx, bz, by);

        // a. the mod command opens it; the world keeps running (eden.clock advances), the rail shows two tiles
        const std::string o = mod("uimenu", {std::to_string(bx), std::to_string(by), std::to_string(bz)});
        tick(2);
        const bool open = ModMenuScreen::isOpen() && ModMenuScreen::railShown() && ModMenuScreen::rowCount() == 7;
        const std::string c0 = clock();
        tick(30);
        const std::string c1 = clock();
        std::snprintf(dd, sizeof dd, "'%s' open %d rail %d rows %d; clock %s -> %s", o.c_str(), ModMenuScreen::isOpen(), ModMenuScreen::railShown(),
                      ModMenuScreen::rowCount(), c0.c_str(), c1.c_str());
        check(o == "open true nil" && open && c1 != c0, "`mod uimenu` opens the menu (icon rail, 7 rows); eden.clock keeps advancing under it", dd);

        // b. tapping a row runs on_action, whose edit is in the world when the tap's frame ends
        ui_tap_rect(rowRect(2));
        tick(1);
        const int got = eden_console_getblock(bx, bz, by);
        tick(1);
        const std::string l1 = mod("last");
        std::snprintf(dd, sizeof dd, "block %d -> %d, a frame later %d (stone %d) log '%s' err '%s'", before, got, eden_console_getblock(bx, bz, by), TYPE_STONE, l1.c_str(),
                      lua("return eden.get(" + n3(bx, by, bz) + ")").c_str());
        check(before != TYPE_STONE && got == TYPE_STONE && l1 == "place=nil closes 0", "tapping a row runs on_action (row id, nil) and its eden.set lands the same frame", dd);

        // c. m:set changes the label on screen
        const std::string sr = mod("relabel", {"Moved"});
        tick(2);
        std::snprintf(dd, sizeof dd, "'%s' row 2 '%s'", sr.c_str(), ModMenuScreen::rowTitle(2).c_str());
        check(sr == "set true" && ModMenuScreen::rowTitle(2) == "Moved", "m:set relabels the row on screen", dd);

        // d. toggle, slider and choice report their values
        ui_tap_rect(rowRect(3));
        tick(2);
        CGRect sl = rowRect(4);
        sl.origin.x += sl.size.width * 0.4f;   // (right of the readout: on the track)
        sl.size.width *= 0.6f;
        ui_tap_rect(sl);
        tick(2);
        CGRect ch = rowRect(5);   // the stepper's right arrow (its right end)
        const float aw = std::min((float)ch.size.height, (float)ch.size.width * 0.3f);
        ch.origin.x += ch.size.width - aw;
        ch.size.width = aw;
        ui_tap_rect(ch);
        tick(2);
        const std::string l2 = mod("last");
        const bool tog = l2.find("glow=true") != std::string::npos, spd = l2.find("speed=") != std::string::npos && l2.find("speed=2.0") == std::string::npos;
        const size_t mp = l2.find("mode=");
        const bool cho = mp != std::string::npos && l2.size() > mp + 5 && l2[mp + 5] >= '1' && l2[mp + 5] <= '3';
        std::snprintf(dd, sizeof dd, "log '%s'", l2.c_str());
        check(tog && spd && cho && l2.find("closes 0") != std::string::npos, "the toggle (true), slider (a new number) and choice (a 1-based index) report their values", dd);

        // e. a second eden.menu while one is open: nil, "busy"
        const std::string ag = mod("again");
        check(ag == "again nil busy", "a second eden.menu while one is open returns nil, \"busy\"", ag.c_str());

        // f. the caps raise with the path
        const std::string bg = mod("big");
        check(bg.find("false:") == 0 && bg.find("tabs[3].rows[33]: more than 128 rows in the menu") != std::string::npos &&
                  bg.find("rows[1].label is longer than 40 bytes") != std::string::npos && bg.find("rows[1].type: unknown row type 'spinner'") != std::string::npos &&
                  bg.find("rows[1].colour: unknown key (type 'row')") != std::string::npos && bg.find("rows[2].id: 'a' is used twice") != std::string::npos,
              "a 129-row spec, a long label, an unknown type or key and a repeated id raise with the path", bg.c_str());

        // g. pointer: Tab walks the rows in order, Return activates; touch: no ring, no tint
        const bool touch = GLW::touchFloor() > 0.0f;
        if (!touch) {
            eden_ui_focus_key(GLW::Focus::K_POINTER);
            tick(2);
            std::vector<int> order;
            int placeAt = -1;
            const int n = GLW::Focus::stops();
            for (int i = 0; i < n + 1; ++i) {
                key(SDL_SCANCODE_TAB);
                CGRect f;
                if (!GLW::Focus::focusedRect(&f)) continue;
                for (int r = 2; r <= 6; ++r) {
                    CGRect c = CGRectMake(0, 0, 0, 0);
                    ModMenuScreen::rowControlRect(r, &c);
                    if (inside(c, f)) { if (order.empty() || order.back() != r) order.push_back(r); break; }
                }
            }
            const bool ordered = order.size() >= 5 && std::is_sorted(order.begin(), order.begin() + 5) &&
                                 std::adjacent_find(order.begin(), order.begin() + 5) == order.begin() + 5 && order[0] == 2;
            const bool ring = GLW::Focus::ringShown();
            // Tab to the `place` row and press Return
            eden_ui_focus_key(GLW::Focus::K_POINTER);
            tick(2);
            for (int i = 0; i < n + 1 && placeAt < 0; ++i) {
                key(SDL_SCANCODE_TAB);
                CGRect f;
                if (GLW::Focus::focusedRect(&f) && inside(rowRect(2), f)) placeAt = i;
            }
            mod("last");
            key(SDL_SCANCODE_RETURN);
            tick(2);
            const std::string l3 = mod("last");
            std::string os;
            for (int r : order) os += std::to_string(r) + " ";
            std::snprintf(dd, sizeof dd, "%d stops, row order %s, ring %d; place at Tab %d, Return -> '%s'", n, os.c_str(), ring, placeAt, l3.c_str());
            check(ordered && ring && placeAt >= 0 && l3 == "place=nil closes 0", "pointer: Tab visits the rows in row order with a ring; Return on a row runs on_action", dd);
            eden_ui_focus_key(GLW::Focus::K_POINTER);   // (a touch begins: the ring goes)
            set_setting_by_key("input_mode", 1);   // touch, for the next check
            tick(3);
        }
        {
            int rings = 0, hovers = 0;
            GLW::Focus::debugCounts(NULL, NULL, true);
            key(SDL_SCANCODE_TAB);
            tick(3);
            GLW::Focus::debugCounts(&rings, &hovers, true);
            std::snprintf(dd, sizeof dd, "touch %d: ring %d rings %d hovers %d live %d", GLW::touchFloor() > 0.0f, GLW::Focus::ringShown(), rings, hovers, GLW::Focus::live());
            check(GLW::touchFloor() > 0.0f && !GLW::Focus::ringShown() && rings == 0 && hovers == 0, "touch: no focus ring and no hover tint on the menu", dd);
            if (!touch) { set_setting_by_key("input_mode", 0); tick(3); }
        }

        // h. Back and Escape close it; on_close runs once each time; m:close is true once, then false
        ui_tap_rect(ModMenuScreen::controlRect("back"));
        tick(3);
        const bool backClosed = !ModMenuScreen::isOpen();
        const std::string l4 = mod("last");
        mod("flat");
        tick(2);
        const bool flatNoRail = ModMenuScreen::isOpen() && !ModMenuScreen::railShown() && !ModMenuScreen::tabsShown();
        key(SDL_SCANCODE_ESCAPE);
        tick(2);
        const bool escClosed = !ModMenuScreen::isOpen() && !World::getWorld->hud->inmenu;
        const std::string l5 = mod("last");
        mod("tabbed");
        tick(2);
        const bool strip = ModMenuScreen::isOpen() && ModMenuScreen::tabsShown();
        const std::string c2 = mod("closeit"), c3 = mod("closeit");
        tick(2);
        const std::string l6 = mod("last");
        std::snprintf(dd, sizeof dd, "back closed %d '%s'; flat no rail %d; escape closed %d '%s'; tab strip %d; close '%s' then '%s', '%s'", backClosed, l4.c_str(),
                      flatNoRail, escClosed, l5.c_str(), strip, c2.c_str(), c3.c_str(), l6.c_str());
        check(backClosed && l4 == "closed closes 1" && flatNoRail && escClosed && l5 == "closed closes 2" && strip && c2 == "close true" &&
                  c3 == "close false" && l6 == "closed closes 3",
              "Back, Escape and m:close each close the menu and run on_close once (a one-tab menu has no rail, labelled tabs a strip)", dd);

        // i. leaving the world closes it, with no on_close; eden.menu inside `leave` raises
        mod("uimenu", {std::to_string(bx), std::to_string(by), std::to_string(bz)});
        tick(2);
        const bool upAgain = ModMenuScreen::isOpen();
        // (Save & Quit's own call: the pause menu cannot be reached by a tap while the menu claims every touch)
        const bool quit = World::getWorld->requestExit() && tick_until([] { return game_mode() == 0; }, 20000, "game_mode back to MENU");
        tick(5);
        const bool gone = !ModMenuScreen::isOpen();
        const bool reopened = open_world(name, g_opt.height);
        tick(g_opt.frames / 2);
        const std::string ml = mod("mget"), l7 = mod("last");
        std::snprintf(dd, sizeof dd, "up %d quit %d gone %d reopened %d; leave '%s'; after '%s'", upAgain, quit, gone, reopened, ml.c_str(), l7.c_str());
        check(upAgain && quit && gone && reopened && ml.find("false") == 0 && ml.find("can't pause while the world closes") != std::string::npos &&
                  l7 == " closes 0",
              "leaving the world closes the menu (no on_close); eden.menu inside `leave` raises", dd);
        g_tickInput = keepInput;
    }

    // ---- 28. K.7-3b (U2): the pause menu's Mods button -> the Mods page -> a changed setting fires `setting` once
    {
        const bool keepInput = g_tickInput;
        g_tickInput = true;
        char dd[1200];
        Hud* hud = World::getWorld->hud;
        const bool avail = ModScript::modsPageAvailable();
        if (!eden_hud_in_menu()) tap_hud(0);
        tick(10);
        const CGRect mb = CGRectMake(hud->rmods.origin.x, hud->rmods.origin.y, hud->rmods.size.width, hud->rmods.size.height);
        std::snprintf(dd, sizeof dd, "available %d in_menu %d button w=%.0f", avail, eden_hud_in_menu(), mb.size.width);
        check(avail && eden_hud_in_menu() == 1 && mb.size.width > 0, "a loaded pack with settings puts a Mods button in the pause menu", dd);
        ui_tap_rect(mb);
        tick(3);
        const bool page = ModMenuScreen::isOpen() && ModMenuScreen::serial() == ModScript::MODS_PAGE;
        std::snprintf(dd, sizeof dd, "open %d serial %d rows %d in_menu %d", ModMenuScreen::isOpen(), ModMenuScreen::serial(), ModMenuScreen::rowCount(), eden_hud_in_menu());
        check(page && ModMenuScreen::rowCount() == 6 && eden_hud_in_menu() == 0, "the button opens the Mods page (menu button + the pack's 5 settings)", dd);
        CGRect tg = CGRectMake(0, 0, 0, 0);
        ModMenuScreen::showRow(2);
        ModMenuScreen::rowControlRect(2, &tg);
        ui_tap_rect(tg);
        tick(3);
        const std::string lg = mod("setlog"), gl = mod("setting", {"glow"});
        bool fileGlow = false;
        for (const ContentPack::ModPack& q : ContentPack::modReport().packs)
            if (q.id == "aw_set") fileGlow = ContentPack::settingValues(World::getWorld->fm->documents, q)[1].b;
        std::snprintf(dd, sizeof dd, "log '%s' glow '%s' file %d", lg.c_str(), gl.c_str(), fileGlow);
        check(lg == "setlog true setlog glow=true/false" || lg.find("glow=true/false") != std::string::npos,
              "flipping a toggle on the page fires `setting` (id, value, old) once", dd);
        check(lg.find("glow=true/false") == lg.rfind("glow=true/false") && gl.find("boolean true") != std::string::npos && fileGlow,
              "eden.setting reads the new value and the file holds it", dd);
        CGRect ob = CGRectMake(0, 0, 0, 0);
        ModMenuScreen::showRow(0);
        ModMenuScreen::rowControlRect(0, &ob);
        ui_tap_rect(ob);
        tick(3);
        const std::string lg2 = mod("setlog");
        std::snprintf(dd, sizeof dd, "open %d log '%s'", ModMenuScreen::isOpen(), lg2.c_str());
        check(!ModMenuScreen::isOpen() && lg2.find(";menu") != std::string::npos, "the page's Open button closes it and fires the pack's `menu` handler", dd);
        g_tickInput = keepInput;
    }

    // ---- 29. K.6-4a (V5): creature handles, read-mostly
    {
        char dd[1600];
        const bool keepCreatures = CREATURES_ON;
        CREATURES_ON = true;
        const PlayerState here = player_state();
        const int cx = (int)here.x + 4, cy = (int)here.y + 2, cz = (int)here.z + 4;
        const std::string C = n3(cx, cy, cz);
        auto slotOf = [](const std::string& t) { const size_t h = t.rfind('#'); return h == std::string::npos ? -1 : std::atoi(t.c_str() + h + 1); };

        // a. spawn -> a handle whose pos is the spawn point (the cell's centre), kind, tostring, ==, eden.creatures
        const std::string a = lua("H = eden.spawn('moof', " + C + "); local x, y, z = H:pos(); local near = eden.creatures(" + C + ", 4) "
                                  "local seen = false; for _, h in ipairs(near) do if h == H then seen = true end end "
                                  "return table.concat({type(H), tostring(x == " + std::to_string(cx) + ".5 and y == " + std::to_string(cy) + ".5 and z == " +
                                  std::to_string(cz) + ".5), H:kind(), tostring(H:valid()), tostring(seen), tostring(near[1] == H), tostring(H)}, ' ')");
        const int S = slotOf(a);
        std::snprintf(dd, sizeof dd, "'%s' slot %d", a.c_str(), S);
        check(a.find("userdata true moof true true true creature moof #") == 0 && S >= 0,
              "eden.spawn returns a handle: pos is the spawn point, kind 'moof', valid; eden.creatures finds the same value nearest first", dd);

        // b. the caps and refusals: r over 64 raises, eden.store refuses a handle, a handle is not a table
        const std::string b = lua("local r1 = pcall(eden.creatures, " + C + ", 65); local r0 = pcall(eden.creatures, " + C + ", 0) "
                                  "local mt = getmetatable(H) "
                                  "return table.concat({tostring(r1), tostring(r0), tostring(mt), tostring(#eden.creatures(" + C + ", 64) <= 64)}, '|')");
        const std::string b2 = mod("storeh", {std::to_string(cx), std::to_string(cy), std::to_string(cz)});
        std::snprintf(dd, sizeof dd, "'%s' store '%s'", b.c_str(), b2.c_str());
        check(b == "false|false|emod.creature|true" &&
                  b2 == "userdata|false|can't store a creature handle|false|can't store a creature handle|nil",
              "eden.creatures raises for r outside (0, 64]; eden.store refuses a handle (also in a table); the metatable is locked", dd);

        // c. h:remove() -> true, then the handle is gone: valid false, every other method nil, remove nil
        const std::string c = lua("local G = eden.spawn('green', " + C + "); local r = G:remove() "
                                  "return table.concat({tostring(r), tostring(G:valid()), tostring((G:pos())), tostring((G:kind())), tostring((G:remove())), tostring(G)}, ' ')");
        check(c == "true false nil nil nil creature (gone)", "h:remove() removes it once; the handle then reads valid false and nil from every method", c.c_str());

        // d. an engine kill (the player's sword, fire, TNT: killCreature) makes the handle invalid
        killCreature(S);
        const std::string d1 = lua("return tostring(H:valid()) .. ' ' .. tostring((H:pos()))");
        check(d1 == "false nil", "a creature the engine kills reads as gone", d1.c_str());

        // e. spawn until the slot is reused: the old handle stays invalid and never reads the new creature
        int reused = -1;
        for (int i = 0; i < CreatureSlots() && reused < 0; ++i) {
            if (!SpawnCreatureAt(1 /* batty */, MakeVector(cx + 0.5f, cy + 3.5f, cz + 0.5f))) break;
            int last = -1;
            CreatureSpawns(&last);
            if (last == S) reused = S;
        }
        const std::string e = lua("local x, y, z = H:pos() "
                                  "return table.concat({tostring(H:valid()), tostring((H:kind())), tostring(y), tostring(H)}, ' ')");
        std::snprintf(dd, sizeof dd, "slot %d reused %d: '%s'", S, reused, e.c_str());
        check(reused == S && e == "false nil nil creature (gone)",
              "a stale handle to a reused slot stays invalid and never reads the new creature (--lua-mutate=nogen must FAIL)", dd);

        // f. walk away until the creature's cell leaves the loaded area: invalid, and still invalid back home
        const std::string f0 = lua("W = eden.spawn('stumpy', " + C + "); return tostring(W)");
        const int WS = slotOf(f0);
        eden_console_teleport(here.x + 1200, here.y + 40, here.z);
        const bool left = tick_until([WS] { return !CreatureRead(WS, NULL, NULL); }, 3000, "the creature's cell out of the window");
        const std::string f1 = lua("return tostring(W:valid())");
        eden_console_teleport(here.x, here.y + 40, here.z);
        tick_until([cx, cy, cz] { return CmdScript::residentCell(cx, cy, cz); }, 3000, "the creature's cell loaded again");
        tick(5);
        const std::string f2 = lua("return tostring(W:valid()) .. ' ' .. tostring((W:pos()))");
        std::snprintf(dd, sizeof dd, "'%s' slot %d left %d: away '%s' back '%s'", f0.c_str(), WS, left, f1.c_str(), f2.c_str());
        check(WS >= 0 && left && f1 == "false" && f2 == "false nil", "a creature whose cell leaves the loaded area is gone for good to its handle", dd);

        // g. EntityData (raw-memcpy'd to disk) did not grow: handles live script-side
        check(sizeof(EntityData) == 60, "sizeof(EntityData) is still 60", std::to_string(sizeof(EntityData)).c_str());
        for (int i = 0; i < CreatureSlots(); ++i) RemoveCreature(i);   // (the batty flock)
        lua("H, W = nil, nil");
        eden_console_teleport(here.x, here.y, here.z);
        tick(5);
        CREATURES_ON = keepCreatures;
    }

    // ---- 30. K.6-4b: h:vel / h:setpos / h:setvel against the AI
    {
        char dd[1600];
        const bool keepCreatures = CREATURES_ON;
        CREATURES_ON = true;
        const PlayerState here = player_state();
        const int cx = (int)here.x + 4, cy = (int)here.y + 2, cz = (int)here.z + 4;
        const std::string C = n3(cx, cy, cz);
        Terrain* ter = World::getWorld->terrain;
        int sy = -1;   // a solid cell under the spawn spot (the ground)
        for (int y = cy; y >= 0 && sy < 0; --y) {
            const int t = ter->getLand(cx, cz, y);
            if (t > 0 && t != TYPE_FLOWER && !(blockinfo[t] & IS_WATER)) sy = y;
        }
        const std::string a0 = lua("V = eden.spawn('moof', " + C + "); return tostring(V)");
        const int VS = a0.rfind('#') == std::string::npos ? -1 : std::atoi(a0.c_str() + a0.rfind('#') + 1);
        tick(60);   // let it land

        // a. setpos: a solid or unloaded cell is refused and nothing moves; a non-finite or huge coordinate raises
        const std::string a = lua("local x0, y0, z0 = V:pos() "
                                  "local s1, w1 = V:setpos(" + std::to_string(cx) + ".5, " + std::to_string(sy) + ".5, " + std::to_string(cz) + ".5) "
                                  "local s2, w2 = V:setpos(x0 + 5000, y0, z0) "
                                  "local s3, w3 = V:setpos(x0, -3, z0) "
                                  "local r1 = pcall(V.setpos, V, 0/0, y0, z0); local r2 = pcall(V.setpos, V, x0, math.huge, z0) "
                                  "local r3 = pcall(V.setpos, V, x0, y0, 1e12) "
                                  "local x1, y1, z1 = V:pos() "
                                  "return table.concat({tostring(s1), w1, tostring(s2), w2, tostring(s3), w3, tostring(r1), tostring(r2), tostring(r3), "
                                  "tostring(x0 == x1 and y0 == y1 and z0 == z1)}, '|')");
        std::snprintf(dd, sizeof dd, "'%s' slot %d ground y %d: '%s'", a0.c_str(), VS, sy, a.c_str());
        check(VS >= 0 && sy >= 0 &&
                  a == "false|that spot is solid|false|that spot isn't loaded|false|that spot isn't loaded|false|false|false|true",
              "h:setpos refuses a solid or unloaded cell (false, why) and raises for a non-finite coordinate; the creature stays put", dd);

        // b. setpos into open air: the creature is exactly there, still
        const std::string T = std::to_string(cx) + ".5, " + std::to_string(cy + 1) + ".25, " + std::to_string(cz) + ".5";
        const std::string b = lua("local ok = V:setpos(" + T + "); local x, y, z = V:pos(); local vx, vy, vz = V:vel() "
                                  "return table.concat({tostring(ok), tostring(x == " + std::to_string(cx) + ".5 and y == " + std::to_string(cy + 1) +
                                  ".25 and z == " + std::to_string(cz) + ".5), tostring(vx == 0 and vy == 0 and vz == 0)}, ' ')");
        check(b == "true true true", "h:setpos into open air puts the creature exactly there, at rest (h:vel is 0, 0, 0)", b.c_str());

        // c. setvel with a NaN raises and the velocity stays finite (--lua-mutate=nonfinite must FAIL; the state is put
        //    back in the same chunk, before any frame could cast it)
        const std::string c = lua("local r = pcall(V.setvel, V, 0/0, 0, 0); local r2 = pcall(V.setvel, V, 1, -math.huge, 0) "
                                  "local vx, vy, vz = V:vel(); local fin = vx == vx and vy == vy and vz == vz and math.abs(vy) < math.huge "
                                  "if not fin then V:setvel(0, 0, 0); V:setpos(" + T + ") end "
                                  "return tostring(r) .. ' ' .. tostring(r2) .. ' ' .. tostring(fin)");
        check(c == "false false true", "h:setvel raises for a NaN or infinite component and the creature's velocity stays finite (--lua-mutate=nonfinite must FAIL)", c.c_str());

        // d. setvel launches: clamped to 16 blocks/s; over 30 frames the physics moves it a bounded distance while the AI
        //    holds off; then it settles and the AI takes over again
        // the launch starts from a spot with 10 open cells (4 rows) along one axis, so the test is about the launch, not the terrain
        int dirX = 1, dirZ = 0, ly = -1;
        for (int y = sy + 2; y < T_HEIGHT - 4 && ly < 0; ++y) {
            for (int k = 0; k < 4 && ly < 0; ++k) {
                const int ux = k == 0 ? 1 : k == 1 ? -1 : 0, uz = k == 2 ? 1 : k == 3 ? -1 : 0;
                bool clear = true;
                for (int i = 0; i <= 10 && clear; ++i)
                    for (int h = -1; h <= 2 && clear; ++h) {
                        const int t = ter->getLand(cx + ux * i, cz + uz * i, y + h);
                        clear = t == 0 || t == TYPE_FLOWER;
                    }
                if (clear) { ly = y; dirX = ux; dirZ = uz; }
            }
        }
        const std::string L0 = lua("return tostring(V:setpos(" + std::to_string(cx) + ".5, " + std::to_string(ly) + ".5, " + std::to_string(cz) + ".5))");
        Vector p0;
        CreatureRead(VS, NULL, &p0);
        const std::string d = lua("local ok = V:setvel(" + std::to_string(dirX * 100) + ", 30, " + std::to_string(dirZ * 100) + "); local vx, vy, vz = V:vel() "
                                  "return tostring(ok) .. ' ' .. string.format('%.3f', math.sqrt(vx * vx + vy * vy + vz * vz))");
        const bool knocked0 = CreatureKnocked(VS);
        float maxDist = 0;
        bool heldOff = true;
        for (int f = 0; f < 30; ++f) {
            tick(1);
            Vector p;
            if (!CreatureRead(VS, NULL, &p)) break;
            const float dx = p.x - p0.x, dz = p.z - p0.z, dist = std::sqrt(dx * dx + dz * dz);
            maxDist = std::max(maxDist, dist);
            if (f < 5 && !CreatureKnocked(VS)) heldOff = false;   // still in the air: the AI must not have taken over
        }
        Vector p1;
        CreatureRead(VS, NULL, &p1);
        const float along = (p1.x - p0.x) * dirX + (p1.z - p0.z) * dirZ;
        const bool resumed = tick_until([VS] { return !CreatureKnocked(VS); }, 240, "the launch to settle");
        const std::string d2 = lua("return tostring(V:valid())");
        std::snprintf(dd, sizeof dd, "'%s' knocked %d held %d: from y %d (%s) along (%d,%d), moved %.2f along it, max %.2f blocks in 30 frames; resumed %d valid %s",
                      d.c_str(), knocked0, heldOff, ly, L0.c_str(), dirX, dirZ, along, maxDist, resumed, d2.c_str());
        check(ly > 0 && L0 == "true" && d == "true 16.000" && knocked0 && heldOff && along > 3.0f && maxDist <= CREATURE_SPEED_MAX * 0.5f + 0.5f &&
                  resumed && d2 == "true",
              "h:setvel launches the creature (clamped to 16 blocks/s) a bounded distance over 30 frames, then the AI resumes", dd);

        // e. a gone handle: every new method is nil
        const std::string e = lua("local G = eden.spawn('green', " + C + "); G:remove() "
                                  "return table.concat({tostring((G:vel())), tostring((G:setpos(" + T + "))), tostring((G:setvel(1, 0, 0)))}, ' ')");
        check(e == "nil nil nil", "h:vel, h:setpos and h:setvel return nil for a gone handle", e.c_str());

        RemoveCreature(VS);
        lua("V = nil");
        tick(5);
        CREATURES_ON = keepCreatures;
    }

    // ---- 31. K.6-7: eden.player() block/colour/mode and eden.look()
    {
        World* w = World::getWorld;
        Player* pl = w->player;
        char dd[600];
        const float keepYaw = pl->yaw, keepPitch = pl->pitch;
        const int keepMode = w->hud->mode, keepBlock = w->hud->blocktype, keepColour = w->hud->block_paintcolor;
        w->hud->mode = MODE_BUILD; w->hud->blocktype = TYPE_STONE; w->hud->block_paintcolor = 7;
        const std::string a = lua("local p = eden.player(); return tostring(p.mode) .. ' ' .. tostring(p.block) .. ' ' .. tostring(p.colour)");
        check(a == "build eden:stone 7", "eden.player() reports the held block, paint colour and tool mode", a.c_str());
        w->hud->mode = MODE_MINE;
        const std::string a2 = lua("return eden.player().mode");
        check(a2 == "mine", "eden.player().mode follows the tool in use", a2.c_str());
        // a stone cell straight below the player: looking down names it (the hit cell, face 3), not the air above it
        const int gx = (int)pl->pos.x, gz = (int)pl->pos.z;
        bool down = false;
        std::string b;
        for (float pitch : {-90.0f, 90.0f, -89.0f, 89.0f}) {
            pl->pitch = pitch;
            tick(3);
            b = lua("local x, y, z, f, b = eden.look(); return table.concat({tostring(x), tostring(y), tostring(z), tostring(f), tostring(b)}, ' ')");
            if (b.find("nil") == std::string::npos) { down = true; break; }
        }
        std::snprintf(dd, sizeof dd, "pitch %.0f: '%s', expected a cell under (%d, %d)", pl->pitch, b.c_str(), gx, gz);
        int lx = 0, ly = 0, lz = 0, lf = -1;
        char lb[64] = "";
        const bool parsed = std::sscanf(b.c_str(), "%d %d %d %d %63s", &lx, &ly, &lz, &lf, lb) == 5;
        check(down && parsed && lx == gx && lz == gz && lf == 3 && std::string(lb) != "eden:air" &&
                  World::getWorld->terrain->getLand(lx, lz, ly) > 0 && World::getWorld->terrain->getLand(lx, lz, ly + 1) <= 0,
              "eden.look() looking straight down names the cell hit and its top face (--lua-mutate=lookbuild must FAIL)", dd);
        pl->pitch = -pl->pitch;   // the other way: the sky
        tick(3);
        const std::string c = lua("return tostring((eden.look()))");
        check(c == "nil", "eden.look() returns nothing when looking at the sky", c.c_str());
        pl->yaw = keepYaw; pl->pitch = keepPitch;
        w->hud->mode = keepMode; w->hud->blocktype = keepBlock; w->hud->block_paintcolor = keepColour;
        tick(3);
    }

    // ---- 11. session, close, reopen
    {
        ModScript::worldEntered(true);
        tick(3);
        const bool none = ModScript::info().empty() && lua("return 1").find("multiplayer") != std::string::npos;
        ModScript::worldEntered(false);
        tick(3);
        const bool back = infoOf(ModScript::info(), "aa_test") != NULL;
        for (int i = 0; i < lim.maxErrors; ++i) mod("boom");   // K.8-6: stop ab_bomb again in this fresh world, then see it in the Mods pane
        tick(2);
        save_world();
        const bool quit = quit_to_menu();
        tick(10);
        const bool closed = ModScript::info().empty();
        // K.8-6: the world is gone, but the Mods pane still shows what its scripts did (ab_bomb was stopped by its own errors just above)
        {
            Menu* mn = World::getWorld->menu;
            std::string stoppedPane = "(no menu)", okPane = "(no menu)";
            if (mn) {
                mn->showHome(true); tick(2);
                mn->openMods(); tick(3);
                ModsMenu* mm = mn->mods;
                auto paneOf = [&](const char* id) {
                    for (int i = 0; i < mm->entryCount(); ++i)
                        if (!std::strcmp(mm->entryId(i), id)) { mm->select(i); tick(3); return std::string(mm->detailText()); }
                    return std::string("(no such pack)");
                };
                stoppedPane = paneOf("ab_bomb");
                okPane = paneOf("aa_test");
                mm->back(); mn->showmods = false; mn->showHome(false); tick(2);
            }
            std::snprintf(d, sizeof(d), "ab_bomb pane: '%s' / aa_test pane: '%s'", stoppedPane.c_str(), okPane.c_str());
            check(stoppedPane.find("Runs scripts") != std::string::npos && stoppedPane.find("Scripts stopped: ") != std::string::npos &&
                      stoppedPane.find("too many errors") != std::string::npos && okPane.find("Runs scripts") != std::string::npos &&
                      okPane.find("Scripts stopped") == std::string::npos,
                  "after the world closes, the Mods detail pane still names a stopped pack's reason and tags script packs (--lua-mutate=noreport must FAIL)", d);
        }
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
    ModScript::testNoHook = ModScript::testNoSandbox = ModScript::testLoadEarly = ModScript::testCapError = ModScript::testStoreNotDirty = ModScript::testLeaveAfterSave = ModScript::testToastFlood = ModScript::testNoGen = ModScript::testNonFinite = ModScript::testLookBuild = ModScript::testNoReport = false;
    SignTool::testNoCache = false;
    std::printf("[eden-lua] %s (%d failure(s))\n", g_selftestFailures ? "FAILURES" : "ALL PASS", g_selftestFailures);
    return g_selftestFailures ? 1 : 0;
}

}  // namespace g_lua
}  // namespace harness
