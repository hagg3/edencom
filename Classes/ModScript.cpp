//
//  ModScript.cpp
//  Emod -- K.4: sandboxed Lua mod scripts. See ModScript.h, docs/scripting.md and
//  WORKING/mod-scripting-plan-2026-11-03.md.
//
//  Lua is compiled as C++ (port/src/shim/vendor/lua/lua_onefile.cpp), so a Lua error raised in a C function
//  below is a C++ exception: locals (std::string, shared_ptr) are destroyed on the way out. Two rules keep the
//  host from ever hitting Lua's panic (an abort):
//    * every Lua call that can allocate runs inside lua_resume or protect() (a lua_pcall'd trampoline);
//    * what the host reads back after a resume (an error, a yield value, a console result) is read without
//      allocating or running a metamethod (lua_type + lua_tostring on a real string only).
//
#include "ModScript.h"
#include "Globals.h"
#include "World.h"
#include "Terrain.h"
#include "Hud.h"
#include "Player.h"
#include "Console.h"
#include "ContentPack.h"
#include "BlockRegistry.h"
#include "MpWorld.h"
#include "Weather.h"
#include "SignTool.h"
#include "GLDialog.h"
#include "EdenWorldStore.h"
#include "ModMenuScreen.h"
#include "UIIcons.h"
#include "Model.h"
#include "Util.h"

#include "lua/lua.h"
#include "lua/lauxlib.h"
#include "lua/lualib.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <new>
#include <set>

namespace ModScript {

bool testNoHook = false;
bool testNoSandbox = false;
bool testLoadEarly = false;
bool testCapError = false;
bool testUseFilter = false;
bool testStoreNotDirty = false;
bool testLeaveAfterSave = false;
bool testToastFlood = false;
bool testDialogNoQueue = false;
bool testNoGen = false;
bool testNonFinite = false;
bool testLookBuild = false;
bool testNoReport = false;

namespace {

const char* const kEventNames[] = {"load", "tick", "build", "mine", "paint", "burn", "weather", "use", "sign", "leave", "setting", "menu", NULL};
const char* const kEditMeta = "emod.edit";
const char* const kCreatureMeta = "emod.creature";   // K.6-4a

struct Co {
    lua_State* T = NULL;
    int ref = LUA_NOREF;
    double wait = 0;
    int preempts = 0;
    int ticket = 0;                       // K.7-1b: waiting for the player's answer to this dialog request (0 = not)
    int ev = -1;                          // the event that started it (-1: a chunk, the console, a command)
    int handler = LUA_NOREF;              // the handler's registry ref (tick: one live call per handler)
    bool started = false;
    bool top = false;                     // a file's top level (the pack's `load` waits for all of them)
    int nargs = 0;                        // pushed on T before the first resume
    CmdScript::Sink* sink = NULL;         // console / `mod` output; NULL = the console scrollback
    bool printResults = false;            // the console's `lua`: print what the chunk returns
    bool done = false;
    std::string what;                     // "tick handler", "scripts/main.lua", ...
};
struct Cmd { std::string name, help; int fn; };
// K.7-3a: one eden.menu{...}: the validated spec (plain C++; the screen holds a copy), the two callbacks and, while it
// is open or a callback of it is queued, a registry ref to its handle (so the handle outlives the script's last
// reference until then). The handle's __gc drops the record.
struct MenuRec {
    int serial = 0;
    ModMenu::Spec spec;
    int onAction = LUA_NOREF, onClose = LUA_NOREF, self = LUA_NOREF;
    bool open = false;
    int tab = 0;
};
struct Pack {
    std::string id;
    bool console = false;
    lua_State* L = NULL;
    size_t mem = 0, memPeak = 0;
    int memRefusals = 0;
    long instrFrame = 0, instrLastFrame = 0, instrResume = 0;
    long long instrTotal = 0;
    lua_State* running = NULL;            // the coroutine the scheduler resumed (only it may be preempted)
    bool preempted = false;
    bool closing = false;
    bool stopped = false;
    std::string why;
    int errors = 0, preempts = 0;
    int skipped = 0;                      // K.8-2: ticks skipped / events dropped at the coroutine cap
    float capQuiet = 0;                   // seconds until the next cap console line
    int booting = 0;                      // K.8-1: top-level chunks not finished yet
    bool loadDue = false;                 // the last of them finished: dispatch `load` at the next safe point
    std::string lastError;
    int printsFrame = 0;
    float toastQuiet = 0;
    bool muted = false;                   // K.7-1b: the player chose "Stop dialogs from this mod" (until the world closes)
    bool dlgSeen = false;
    double dlgLast = 0, dlgNext = 0;      // game time of this pack's last dialog; before dlgNext its next one is held (0.5 s)
    double toastNext = 0;                 // K.7-1a: g_clock before which eden.toast returns false
    bool settingsRead = false;            // K.7-2b: settingVals holds this pack's values (read on the first eden.setting)
    std::vector<ContentPack::SettingValue> settingVals;
    std::vector<std::unique_ptr<MenuRec>> menus;   // K.7-3a
    CmdScript::Sink* sink = NULL;         // the running coroutine's sink
    std::vector<int> handlers[EV_COUNT];
    std::vector<Co> cos;
    std::vector<Cmd> cmds;
};
struct QEvent { int ev, x, y, z, a, b; std::string text; };

Limits g_lim;
}  // namespace
extern "C" int eden_device_class(void);   // port/src/seam/DisplayProfile.cpp: 2 = mobile_low (EDEN_DEVICE_MOBILE_LOW)
size_t defaultAllMemBytes() { return eden_device_class() == 2 ? (size_t)24 << 20 : (size_t)64 << 20; }
namespace {
Stats g_stats;
std::vector<std::unique_ptr<Pack>> g_packs;
std::vector<Info> g_lastReport;   // K.8-6
std::vector<QEvent> g_queue;
bool g_pending = false;    // start the packs' scripts on the next update
bool g_started = false;    // they were started (this world)
bool g_session = false;
bool g_resetPending = false;
bool g_leaving = false;    // K.6-2: leaving() is running the `leave` handlers (nothing may pause)
int g_depth = 0;           // > 0 while Lua runs (reset/restart wait for it)
long g_instrFrame = 0;
double g_clock = 0;

// K.7-1b: dialog requests (V9-V11). A request is queued by eden.ask/eden.input, shown by pumpDialogs() at the end of
// update(), and answered by a GLDialog callback that only RECORDS the result (rule 7: no Lua there); step 1 of the next
// update() resumes the waiting coroutine.
struct DlgReq {
    int ticket = 0;
    Pack* p = NULL;
    bool input = false;
    std::string title, body, def, ok, cancel;
    std::vector<std::string> buttons;
    int danger = 0, max = 64, lines = 1;
};
struct DlgRes { int ticket; int index; std::string text, why; };
std::vector<DlgReq> g_dlgQueue;
std::vector<DlgRes> g_dlgResults;
int g_ticket = 0;
int g_dlgShownTicket = 0;
Pack* g_dlgShownPack = NULL;

Pack* packOf(lua_State* L) { return *(Pack**)lua_getextraspace(L); }

bool worldOpen() {
    World* w = World::getWorld;
    return w && w->terrain && w->terrain->loaded && w->fm;
}

// ---- memory (S5) --------------------------------------------------------------------------------------
size_t g_allMem = 0;   // every state's bytes together (K.8-5: Limits::allMemBytes)

void* luaAlloc(void* ud, void* ptr, size_t osize, size_t nsize) {
    Pack* p = (Pack*)ud;
    const size_t old = ptr ? osize : 0;   // ptr NULL: osize is the object's type, not a size
    if (nsize == 0) {
        if (ptr) { p->mem -= old; g_allMem -= old; std::free(ptr); }
        return NULL;
    }
    // A shrink never fails. A refusal is Lua's "not enough memory" in that pack only: the pack that asked is the one
    // that hits the total, the others keep what they hold.
    if (nsize > old && (p->mem - old + nsize > g_lim.memBytes || g_allMem - old + nsize > g_lim.allMemBytes)) { p->memRefusals++; return NULL; }
    void* n = std::realloc(ptr, nsize);
    if (!n) return NULL;
    g_allMem = g_allMem - old + nsize;
    p->mem = p->mem - old + nsize;
    if (p->mem > p->memPeak) p->memPeak = p->mem;
    return n;
}

// ---- CPU (S6) -----------------------------------------------------------------------------------------
bool overBudget(const Pack* p) { return p->instrFrame >= g_lim.packPerFrame || g_instrFrame >= g_lim.allPerFrame; }

void countHook(lua_State* L, lua_Debug*) {
    Pack* p = packOf(L);
    p->instrFrame += HOOK_EVERY;
    p->instrResume += HOOK_EVERY;
    p->instrTotal += HOOK_EVERY;
    g_instrFrame += HOOK_EVERY;
    if (p->closing) luaL_error(L, "the world closed");   // (no script __gc exists to run at lua_close: safeSetmetatable)
    if (testNoHook) return;
    if (overBudget(p) && !g_leaving && L == p->running && lua_isyieldable(L)) {
        p->preempted = true;
        lua_yield(L, 0);   // a hook yields with no values; the scheduler resumes it next frame
        return;
    }
    // Code that can't pause (a callback inside C, a coroutine the script made): bounded instead. The hook never runs
    // inside the C loops themselves: the vendored Lua caps those (pattern steps, string.rep, table.move/insert/remove).
    if (p->instrResume > g_lim.hardPerResume || p->instrFrame > 2 * g_lim.hardPerResume)
        luaL_error(L, "used too much CPU without a pause (more than %d instructions)", (int)g_lim.hardPerResume);
}

void luaWarn(void* ud, const char* msg, int tocont) {
    (void)tocont;
    Pack* p = (Pack*)ud;
    if (msg[0] == '@') return;   // a control message ("@on"), not text
    if (p->printsFrame++ < MAX_PRINTS) Console::print(CmdScript::Sink::ERR, "[" + p->id + "] warning: " + msg);
}

// ---- reporting -----------------------------------------------------------------------------------------
void say(Pack* p, int kind, const std::string& text, CmdScript::Sink* sink) {
    if (sink) sink->line(kind, text);
    else Console::print(kind, "[" + p->id + "] " + text);
}

void stopPack(Pack* p, const std::string& why) {
    if (p->stopped) return;
    p->stopped = true;
    p->why = why;
    g_stats.stopped++;
    say(p, CmdScript::Sink::ERR, "scripts stopped: " + why, NULL);
    std::fprintf(stderr, "[lua %s] scripts stopped: %s\n", p->id.c_str(), why.c_str());
}

void scriptError(Pack* p, const std::string& msg, CmdScript::Sink* sink) {
    p->errors++;
    g_stats.errors++;
    p->lastError = msg;
    std::fprintf(stderr, "[lua %s] %s\n", p->id.c_str(), msg.c_str());
    say(p, CmdScript::Sink::ERR, msg, sink);
    if (!p->console && p->toastQuiet <= 0 && World::getWorld && World::getWorld->hud) {
        World::getWorld->hud->sb->setStatus("Mod " + p->id + ": script error (see the console)", 3);
        p->toastQuiet = 10;
    }
    if (!p->console && p->errors >= g_lim.maxErrors) stopPack(p, "too many errors (" + std::to_string(p->errors) + ")");
}

// The value at `idx` of T as text, never allocating (no conversion, no __tostring).
std::string plainText(lua_State* T, int idx) {
    char b[64];
    switch (lua_type(T, idx)) {
    case LUA_TSTRING: { size_t n; const char* s = lua_tolstring(T, idx, &n); return std::string(s, n); }
    case LUA_TNUMBER:
        if (lua_isinteger(T, idx)) std::snprintf(b, sizeof b, "%lld", (long long)lua_tointeger(T, idx));
        else std::snprintf(b, sizeof b, "%.14g", (double)lua_tonumber(T, idx));
        return b;
    case LUA_TBOOLEAN: return lua_toboolean(T, idx) ? "true" : "false";
    case LUA_TNIL: return "nil";
    default: return std::string("(a ") + lua_typename(T, lua_type(T, idx)) + ")";
    }
}

// Runs fn inside lua_pcall on the pack's main thread: an allocation failure or a Lua error there is an
// error string, never a panic.
struct Trampoline { const std::function<void(lua_State*)>* fn; };
int trampoline(lua_State* L) {
    const Trampoline* t = (const Trampoline*)lua_touserdata(L, 1);
    (*t->fn)(L);
    return 0;
}
bool protect(Pack* p, const std::function<void(lua_State*)>& fn, std::string* err) {
    lua_State* L = p->L;
    Trampoline t{&fn};
    p->instrResume = 0;
    lua_pushcfunction(L, trampoline);
    lua_pushlightuserdata(L, &t);
    g_depth++;
    const int st = lua_pcall(L, 1, 0, 0);
    g_depth--;
    if (st == LUA_OK) return true;
    if (err) *err = plainText(L, -1);
    lua_settop(L, 0);
    return false;
}

// ---- coroutines -----------------------------------------------------------------------------------------
int liveCos(const Pack* p) {
    int n = 0;
    for (const Co& c : p->cos) n += !c.done;
    return n;
}

void finishCo(Pack* p, Co& c) {
    c.done = true;
    if (c.top) { c.top = false; if (--p->booting == 0) p->loadDue = true; }
    if (p->L && c.ref != LUA_NOREF) luaL_unref(p->L, LUA_REGISTRYINDEX, c.ref);
    c.ref = LUA_NOREF;
    c.T = NULL;
}

// Resumes c. Afterwards it is either done, or waiting `wait` seconds (0 = next frame).
void resumeCo(Pack* p, size_t i) {
    Co* c = &p->cos[i];
    const int nargs = c->started ? 0 : c->nargs;
    c->started = true;
    p->running = c->T;
    p->preempted = false;
    p->instrResume = 0;
    p->sink = c->sink;
    int nres = 0;
    g_depth++;
    const int st = lua_resume(c->T, NULL, nargs, &nres);
    g_depth--;
    p->running = NULL;
    p->sink = NULL;
    c = &p->cos[i];   // (nothing adds a coroutine during a resume; re-taken anyway)
    if (st == LUA_YIELD) {
        double w = 0;
        if (nres >= 1 && lua_type(c->T, -nres) == LUA_TNUMBER) w = lua_tonumber(c->T, -nres);
        lua_pop(c->T, nres);   // only the yielded values: the rest is the paused C function's frame
        if (p->preempted) {
            g_stats.preempted++;
            p->preempts++;
            if (++c->preempts > g_lim.maxPreempts) {
                scriptError(p, c->what + ": ran " + std::to_string(c->preempts - 1) +
                                   " frames without eden.wait (stopped)", c->sink);
                finishCo(p, *c);
                return;
            }
            c->wait = 0;
            return;
        }
        c->preempts = 0;
        c->wait = w;
        return;
    }
    if (st == LUA_OK) {
        g_stats.finished++;
        if (c->printResults && nres > 0) {
            std::string line;
            for (int k = nres; k >= 1; --k) line += (line.empty() ? "" : "\t") + plainText(c->T, -k);
            say(p, CmdScript::Sink::OUT, line, c->sink);
        }
    } else {
        const std::string msg = lua_type(c->T, -1) == LUA_TSTRING ? plainText(c->T, -1)
                                : st == LUA_ERRMEM ? std::string("not enough memory")
                                                   : "an error (" + plainText(c->T, -1) + ")";
        scriptError(p, c->what + ": " + msg, c->sink);
    }
    finishCo(p, *c);
}

// A new coroutine: function `fnRef` (or the function `push` leaves first) with the arguments `push` pushes.
// Not resumed here. False: not started (too many waiting, or out of memory: reported).
bool spawn(Pack* p, int fnRef, const std::function<int(lua_State*)>& push, int ev, const std::string& what,
           CmdScript::Sink* sink, bool printResults) {
    if (p->stopped || !p->L) return false;
    if (liveCos(p) >= MAX_COROUTINES && ev != EV_LEAVE) {   // K.6-2: `leave` is bounded by MAX_HANDLERS instead
        const std::string msg = what + ": not started, " + std::to_string((int)MAX_COROUTINES) + " handlers are still waiting";
        if (testCapError) { scriptError(p, msg, sink); return false; }
        // K.8-2: the cap is a limit, not a fault: skipped / dropped, counted, never an error. A command or the console
        // answers its caller; an event says so on the console once per 10 s.
        p->skipped++;
        g_stats.skipped++;
        if (ev < 0) say(p, CmdScript::Sink::ERR, msg, sink);
        else if (p->capQuiet <= 0) {
            say(p, CmdScript::Sink::ERR, std::string(ev == EV_TICK ? "tick skipped" : "event dropped") + " (" + what + "): " +
                std::to_string((int)MAX_COROUTINES) + " handlers are still waiting (" + std::to_string(p->skipped) + " so far)", NULL);
            p->capQuiet = 10;
        }
        return false;
    }
    Co c;
    std::string err;
    const bool ok = protect(p, [&](lua_State* L) {
        lua_State* T = lua_newthread(L);
        c.ref = luaL_ref(L, LUA_REGISTRYINDEX);   // pops the thread
        c.T = T;
        const int base = lua_gettop(L);
        if (fnRef != LUA_NOREF) lua_rawgeti(L, LUA_REGISTRYINDEX, fnRef);
        if (push) push(L);
        const int moved = lua_gettop(L) - base;   // the function, then its arguments
        c.nargs = moved - 1;
        if (!lua_checkstack(T, moved + 1)) luaL_error(L, "not enough memory");
        lua_xmove(L, T, moved);
    }, &err);
    if (!ok) {
        if (c.ref != LUA_NOREF) luaL_unref(p->L, LUA_REGISTRYINDEX, c.ref);
        scriptError(p, what + ": not started (" + err + ")", sink);
        return false;
    }
    c.ev = ev;
    c.handler = fnRef;
    c.what = what;
    c.sink = sink;
    c.printResults = printResults;
    g_stats.started++;
    p->cos.push_back(c);
    return true;
}

// Resume what can run now: a new coroutine at once, if the budget allows.
void runNew(Pack* p) {
    const size_t i = p->cos.size() - 1;
    if (!overBudget(p)) resumeCo(p, i);
}

// ---- the sandbox (S4) ------------------------------------------------------------------------------------
int luaPrint(lua_State* L) {
    Pack* p = packOf(L);
    const int n = lua_gettop(L);
    std::string line;
    for (int i = 1; i <= n; ++i) {
        size_t len;
        const char* s = luaL_tolstring(L, i, &len);
        if (i > 1) line += '\t';
        line.append(s, len);
        lua_pop(L, 1);
    }
    if (p->printsFrame < MAX_PRINTS) say(p, CmdScript::Sink::OUT, line, p->sink);
    else if (p->printsFrame == MAX_PRINTS) say(p, CmdScript::Sink::ERR, "(more output this frame dropped)", p->sink);
    p->printsFrame++;
    return 0;
}

// load(chunk [, name [, mode [, env]]]): a string chunk, text only (no bytecode).
int safeLoad(lua_State* L) {
    size_t n;
    if (lua_type(L, 1) != LUA_TSTRING) return luaL_error(L, "load: only a string chunk loads in a mod script");
    const char* s = lua_tolstring(L, 1, &n);
    const char* name = luaL_optstring(L, 2, "=(load)");
    const int st = luaL_loadbufferx(L, s, n, name, "t");
    if (st == LUA_OK) {
        if (!lua_isnone(L, 4)) {
            lua_pushvalue(L, 4);
            if (!lua_setupvalue(L, -2, 1)) lua_pop(L, 1);
        }
        return 1;
    }
    lua_pushnil(L);
    lua_insert(L, -2);
    return 2;
}

int safeGC(lua_State* L) {
    const char* opt = luaL_optstring(L, 1, "collect");
    if (!std::strcmp(opt, "count")) {
        lua_pushnumber(L, (lua_Number)lua_gc(L, LUA_GCCOUNT) + (lua_Number)lua_gc(L, LUA_GCCOUNTB) / 1024.0);
        return 1;
    }
    if (!std::strcmp(opt, "collect")) { lua_gc(L, LUA_GCCOLLECT); lua_pushinteger(L, 0); return 1; }
    if (!std::strcmp(opt, "step")) { lua_pushboolean(L, lua_gc(L, LUA_GCSTEP, 0)); return 1; }
    return luaL_error(L, "collectgarbage(\"%s\") is not available to a mod script", opt);
}

// string.find/match/gmatch/gsub: the subject's length is capped (C code can't be preempted).
int guardedPattern(lua_State* L) {
    size_t n;
    luaL_checklstring(L, 1, &n);
    if (n > PATTERN_MAX) return luaL_error(L, "string too long for pattern matching in a mod script (%d bytes at most)", (int)PATTERN_MAX);
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    lua_call(L, lua_gettop(L) - 1, LUA_MULTRET);
    return lua_gettop(L);
}

// setmetatable without __gc (QA.3): Lua runs a finalizer with the count hook off (lgc.c GCTM), at a collect and
// inside lua_close, so a looping __gc could hang the game. Lua only marks an object for finalization when
// setmetatable sees a __gc field (any value, even false), so refusing that here means no script finalizer ever runs;
// adding __gc to the metatable later does nothing. (debug.setmetatable isn't in the sandbox.)
int safeSetmetatable(lua_State* L) {
    if (lua_type(L, 2) == LUA_TTABLE) {
        lua_pushliteral(L, "__gc");
        if (lua_rawget(L, 2) != LUA_TNIL) return luaL_error(L, "setmetatable: __gc is not available to a mod script");
        lua_pop(L, 1);
    }
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    lua_call(L, lua_gettop(L) - 1, LUA_MULTRET);
    return lua_gettop(L);
}

// ---- the API (S8): world edits are command-block statements -----------------------------------------------
enum { K_SET = 0, K_COUNT, K_NONE, K_SPAWN };
struct CreatureUD { int slot; uint32_t gen; };   // K.6-4a
enum { CREATURES_RADIUS_MAX = 64, CREATURES_RESULTS_MAX = 64 };
struct EditJob {
    std::shared_ptr<const CmdScript::Program> prog;
    size_t pc = 0;
    int cells = 0, refused = 0, kind = K_NONE, ax = 0, ay = 0, az = 0;
    int spawnSlot = -1;        // K_SPAWN: the slot SpawnCreatureAt filled, and its generation then
    uint32_t spawnGen = 0;
    std::string firstRefusal;
};
void pushCreature(lua_State* L, int slot, uint32_t gen);
struct CaptureSink : CmdScript::Sink {
    std::string* first;
    explicit CaptureSink(std::string* f) : first(f) {}
    void line(int kind, const std::string& t) override { if (kind == ERR && first->empty()) *first = t; }
};

int editGc(lua_State* L) {
    EditJob* j = (EditJob*)luaL_checkudata(L, 1, kEditMeta);
    j->~EditJob();
    return 0;
}

int editStep(lua_State* L, int status, lua_KContext ctx) {
    (void)status;
    EditJob* j = (EditJob*)lua_touserdata(L, (int)ctx);
    if (!worldOpen()) return luaL_error(L, "no world is open");
    CaptureSink sink(&j->firstRefusal);
    int lastSlot = -1;
    const uint32_t spawnsBefore = CreatureSpawns(&lastSlot);
    const bool done = CmdScript::runNow(j->prog, &j->pc, j->ax, j->ay, j->az, &sink, &j->cells, &j->refused);
    if (j->kind == K_SPAWN && CreatureSpawns(&lastSlot) != spawnsBefore) {   // (one statement: nothing else spawns in this call)
        j->spawnSlot = lastSlot;
        j->spawnGen = CreatureGen(lastSlot);
    }
    if (!done && g_leaving) {   // K.6-2: there is no next frame; what is left is refused
        if (j->firstRefusal.empty()) j->firstRefusal = "the world is closing: its world-edit budget is spent";
    } else if (!done) {   // this frame's WORK_PER_FRAME is spent: carry on next frame (a block pauses the same way)
        if (!lua_isyieldable(L)) return luaL_error(L, "this frame's world-edit budget is spent (an edit inside a callback that can't pause)");
        return lua_yieldk(L, 0, ctx, editStep);
    }
    g_stats.edits++;
    const char* why = j->firstRefusal.empty() ? NULL : j->firstRefusal.c_str();
    switch (j->kind) {
    case K_SET:
        lua_pushboolean(L, j->refused == 0 && !why);
        if (why) lua_pushstring(L, why); else lua_pushnil(L);
        return 2;
    case K_SPAWN:   // K.6-4a: a handle, or false, why (a spawn that did nothing used to return true)
        if (!why && j->spawnSlot >= 0) {
            pushCreature(L, j->spawnSlot, j->spawnGen);
            return 1;
        }
        lua_pushboolean(L, 0);
        lua_pushstring(L, why ? why : !CREATURES_ON ? "creatures are off" : !CreatureSlotFree() ? "every creature slot is in use" : "that spot isn't loaded");
        return 2;
    case K_COUNT:
        lua_pushinteger(L, j->cells);
        lua_pushinteger(L, j->refused);
        if (why) lua_pushstring(L, why); else lua_pushnil(L);
        return 3;
    default:
        lua_pushboolean(L, !why);
        return 1;
    }
}

// Parses `line` the console's way (no console verbs) and runs it at once, anchored at (ax, ay, az).
int edit(lua_State* L, const std::string& line, int ax, int ay, int az, bool single, int kind) {
    if (!worldOpen()) return luaL_error(L, "no world is open");
    if (MpWorld::active()) return luaL_error(L, "not in a multiplayer world");
    if (line.size() > CmdScript::MAX_LINE) return luaL_error(L, "a line holds up to 511 characters");
    std::shared_ptr<CmdScript::Program> prog = std::make_shared<CmdScript::Program>();
    CmdScript::parseLine(line.c_str(), prog.get(), CmdScript::consoleOptions(NULL));
    if (prog->stmts.empty()) return luaL_error(L, "nothing to run");
    if (prog->errors) return luaL_error(L, "%s", prog->firstError.c_str());
    if (single && prog->stmts.size() != 1) return luaL_error(L, "one statement only: an argument holds ';'");
    for (const CmdScript::Stmt& s : prog->stmts)
        if (s.op == CmdScript::OP_WAIT) return luaL_error(L, "use eden.wait(seconds) in a script, not `wait`");
    void* mem = lua_newuserdatauv(L, sizeof(EditJob), 0);
    EditJob* j = new (mem) EditJob();
    luaL_setmetatable(L, kEditMeta);
    j->prog = prog;
    j->kind = kind;
    j->ax = ax; j->ay = ay; j->az = az;
    return editStep(L, LUA_OK, lua_gettop(L));
}

int coordArg(lua_State* L, int i) {
    const lua_Integer v = luaL_checkinteger(L, i);
    if (v < -1000000000 || v > 1000000000) luaL_argerror(L, i, "coordinate out of range");
    return (int)v;
}
std::string at3(lua_State* L, int i) {
    char b[64];
    std::snprintf(b, sizeof b, "=%d =%d =%d", coordArg(L, i), coordArg(L, i + 1), coordArg(L, i + 2));
    return b;
}
// A block or creature: a name (one word) or a number.
std::string wordArg(lua_State* L, int i) {
    if (lua_type(L, i) == LUA_TNUMBER) return std::to_string((long long)luaL_checkinteger(L, i));
    size_t n;
    const char* s = luaL_checklstring(L, i, &n);
    if (n == 0 || n > 64) luaL_argerror(L, i, "a name of 1-64 characters");
    for (size_t k = 0; k < n; ++k)
        if (std::isspace((unsigned char)s[k]) || s[k] == ';' || (unsigned char)s[k] < 0x20) luaL_argerror(L, i, "a name has no spaces or ';'");
    return std::string(s, n);
}
std::string numArg(lua_State* L, int i) {
    char b[48];
    std::snprintf(b, sizeof b, "%.9g", (double)luaL_checknumber(L, i));
    return b;
}
std::string colourArg(lua_State* L, int i) {
    if (lua_isnoneornil(L, i)) return "";
    return " " + std::to_string((long long)luaL_checkinteger(L, i));
}

int apiCmd(lua_State* L) {
    const std::string line = luaL_checkstring(L, 1);
    int ax, ay, az;
    if (lua_isnoneornil(L, 2)) CmdScript::playerAnchor(&ax, &ay, &az);
    else { ax = coordArg(L, 2); ay = coordArg(L, 3); az = coordArg(L, 4); }
    return edit(L, line, ax, ay, az, false, K_COUNT);
}
int apiSet(lua_State* L) {
    return edit(L, "set " + at3(L, 1) + " " + wordArg(L, 4) + colourArg(L, 5), 0, 0, 0, true, K_SET);
}
int apiFill(lua_State* L) {
    return edit(L, "fill " + at3(L, 1) + " " + at3(L, 4) + " " + wordArg(L, 7) + colourArg(L, 8), 0, 0, 0, true, K_COUNT);
}
int apiPaint(lua_State* L) { return edit(L, "paint " + at3(L, 1) + colourArg(L, 4), 0, 0, 0, true, K_NONE); }
int apiExplode(lua_State* L) { return edit(L, "explode " + at3(L, 1), 0, 0, 0, true, K_NONE); }
int apiFire(lua_State* L) { return edit(L, "fire " + at3(L, 1), 0, 0, 0, true, K_NONE); }
int apiTp(lua_State* L) { return edit(L, "tp " + at3(L, 1), 0, 0, 0, true, K_NONE); }
int apiRun(lua_State* L) { return edit(L, "run " + at3(L, 1), 0, 0, 0, true, K_NONE); }
int apiSpawn(lua_State* L) {
    return edit(L, "spawn " + wordArg(L, 1) + (lua_isnoneornil(L, 2) ? std::string() : " " + at3(L, 2)), 0, 0, 0, true, K_SPAWN);
}

// ---- K.6-4a: creature handles (V5) ---------------------------------------------------------------------------
// A handle is (slot, generation): Model.cpp moves a slot's generation whenever it stops holding the same live
// creature (death, pick-up, removal, reuse, its cell leaving the loaded area), so a stale handle reads as gone instead
// of reading whatever took the slot. One userdata per (slot, generation) per state, kept in a weak registry table, so
// the same creature is the same value (== and table keys work). No __gc: nothing to free.
const char kCreatureCache = 0;   // its address keys the registry table
void pushCreature(lua_State* L, int slot, uint32_t gen) {
    luaL_checkstack(L, 4, "creature handle");
    if (lua_rawgetp(L, LUA_REGISTRYINDEX, &kCreatureCache) != LUA_TTABLE) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_createtable(L, 0, 1);
        lua_pushliteral(L, "v");
        lua_setfield(L, -2, "__mode");
        lua_setmetatable(L, -2);
        lua_pushvalue(L, -1);
        lua_rawsetp(L, LUA_REGISTRYINDEX, &kCreatureCache);
    }
    if (lua_rawgeti(L, -1, slot) == LUA_TUSERDATA) {
        const CreatureUD* u = (const CreatureUD*)lua_touserdata(L, -1);
        if (u->gen == gen) { lua_remove(L, -2); return; }
    }
    lua_pop(L, 1);
    CreatureUD* u = (CreatureUD*)lua_newuserdatauv(L, sizeof(CreatureUD), 0);
    u->slot = slot;
    u->gen = gen;
    luaL_setmetatable(L, kCreatureMeta);
    lua_pushvalue(L, -1);
    lua_rawseti(L, -3, slot);
    lua_remove(L, -2);
}
// The slot, or -1 when the handle is gone (or no world is open).
int creatureSlot(lua_State* L) {
    const CreatureUD* u = (const CreatureUD*)luaL_checkudata(L, 1, kCreatureMeta);
    if (!worldOpen() || g_session) return -1;
    return CreatureHandleLive(u->slot, testNoGen ? CreatureGen(u->slot) : u->gen) ? u->slot : -1;
}
std::string creatureKind(int type) {
    const std::vector<std::string> names = CmdScript::creatureNames();
    return type >= 0 && type < (int)names.size() ? names[type] : std::to_string(type);
}
// h:valid() -> boolean
int creatureValid(lua_State* L) {
    lua_pushboolean(L, creatureSlot(L) >= 0);
    return 1;
}
// h:pos() -> x, y, z (numbers: the point eden.spawn put at the cell's centre), or nil when gone
int creaturePos(lua_State* L) {
    Vector v;
    const int s = creatureSlot(L);
    if (s < 0 || !CreatureRead(s, NULL, &v)) return 0;
    lua_pushnumber(L, v.x);
    lua_pushnumber(L, v.y);
    lua_pushnumber(L, v.z);
    return 3;
}
// h:kind() -> the creature's name (eden.spawn's first argument), or nil when gone
int creatureKindApi(lua_State* L) {
    int type;
    const int s = creatureSlot(L);
    if (s < 0 || !CreatureRead(s, &type, NULL)) return 0;
    lua_pushstring(L, creatureKind(type).c_str());
    return 1;
}
// h:remove() -> true (the creature is gone, silently: no sound, no puff), or nil when it already was
int creatureRemove(lua_State* L) {
    const int s = creatureSlot(L);
    if (s < 0 || !RemoveCreature(s)) return 0;
    lua_pushboolean(L, 1);
    return 1;
}
// ---- K.6-4b: moving a creature against its AI ---------------------------------------------------------------
// Three numbers from stack index i on; a non-finite or huge one raises (it would reach the (int) casts in Move and
// CheckCollision, and the save's EntityData). --lua-mutate=nonfinite lets h:setvel take them.
Vector vec3Arg(lua_State* L, int i, bool check) {
    double c[3];
    for (int k = 0; k < 3; ++k) {
        c[k] = luaL_checknumber(L, i + k);
        if (check && (!std::isfinite(c[k]) || std::fabs(c[k]) > 1000000000.0)) luaL_argerror(L, i + k, "a finite number is needed");
    }
    return MakeVector((float)c[0], (float)c[1], (float)c[2]);
}
// h:vel() -> vx, vy, vz in blocks a second (the AI's own walking included), or nil when gone
int creatureVel(lua_State* L) {
    Vector v;
    const int s = creatureSlot(L);
    if (s < 0 || !CreatureVel(s, &v)) return 0;
    lua_pushnumber(L, v.x);
    lua_pushnumber(L, v.y);
    lua_pushnumber(L, v.z);
    return 3;
}
// h:setpos(x, y, z) -> true, or false, why (the cell holding the point is not loaded, or is solid: anything a creature
// collides with, so air, flowers and water are open); nil when gone. The creature lands there still, then falls.
int creatureSetPos(lua_State* L) {
    const Vector p = vec3Arg(L, 2, true);
    const int s = creatureSlot(L);
    if (s < 0) return 0;
    const int x = (int)std::floor(p.x), y = (int)std::floor(p.y), z = (int)std::floor(p.z);
    const char* why = NULL;
    if (!CmdScript::residentCell(x, y, z)) why = "that spot isn't loaded";
    else {
        const int t = World::getWorld->terrain->getLand(x, z, y);
        if (t < 0) why = "that spot isn't loaded";
        else if (t > 0 && t != TYPE_FLOWER && !(blockinfo[t] & IS_WATER)) why = "that spot is solid";
    }
    if (!why) {
        const int r = CreatureSetPos(s, p);
        if (r == 1) return 0;
        if (r == 2) why = "that spot isn't loaded";
    }
    lua_pushboolean(L, !why);
    if (!why) return 1;
    lua_pushstring(L, why);
    return 2;
}
// h:setvel(vx, vy, vz) -> true, or nil when gone. A launch, not a held state: the velocity is set once (its length
// clamped to 16 blocks/s), then gravity, friction, water and collisions act on it while the AI holds off walking and
// deciding; when the creature is down and nearly still (or after 3 s) the AI takes over again. h:vel() + h:setvel()
// adds to the current velocity.
int creatureSetVel(lua_State* L) {
    const Vector v = vec3Arg(L, 2, !testNonFinite);
    const int s = creatureSlot(L);
    if (s < 0 || !CreatureSetVel(s, v)) return 0;
    lua_pushboolean(L, 1);
    return 1;
}
int creatureToString(lua_State* L) {
    int type;
    const int s = creatureSlot(L);
    if (s >= 0 && CreatureRead(s, &type, NULL)) lua_pushfstring(L, "creature %s #%d", creatureKind(type).c_str(), s);
    else lua_pushliteral(L, "creature (gone)");
    return 1;
}
const luaL_Reg kCreatureMethods[] = {{"valid", creatureValid}, {"pos", creaturePos}, {"kind", creatureKindApi}, {"remove", creatureRemove},
                                     {"vel", creatureVel}, {"setpos", creatureSetPos}, {"setvel", creatureSetVel}, {NULL, NULL}};

// eden.creatures(x, y, z, r) -> an array of handles, nearest first: the live creatures within r (0 < r <= 64) of the
// point, at most 64. Empty when no world is open.
int apiCreatures(lua_State* L) {
    double c[3];
    for (int i = 0; i < 3; ++i) {
        c[i] = luaL_checknumber(L, i + 1);
        if (!std::isfinite(c[i]) || std::fabs(c[i]) > 1000000000.0) luaL_argerror(L, i + 1, "coordinate out of range");
    }
    const double r = luaL_checknumber(L, 4);
    if (!(r > 0 && r <= CREATURES_RADIUS_MAX)) luaL_argerror(L, 4, "a radius over 0 and up to 64");
    std::vector<std::pair<double, int> > found;
    if (worldOpen() && !g_session) {
        for (int s = 0, n = CreatureSlots(); s < n; ++s) {
            Vector v;
            if (!CreatureRead(s, NULL, &v)) continue;
            const double dx = v.x - c[0], dy = v.y - c[1], dz = v.z - c[2], d2 = dx * dx + dy * dy + dz * dz;
            if (d2 <= r * r) found.push_back(std::make_pair(d2, s));
        }
    }
    std::sort(found.begin(), found.end());
    if (found.size() > CREATURES_RESULTS_MAX) found.resize(CREATURES_RESULTS_MAX);
    lua_createtable(L, (int)found.size(), 0);
    for (size_t i = 0; i < found.size(); ++i) {
        pushCreature(L, found[i].second, CreatureGen(found[i].second));
        lua_rawseti(L, -2, (lua_Integer)i + 1);
    }
    return 1;
}
int apiSay(lua_State* L) {
    std::string t = luaL_checkstring(L, 1);
    for (char& ch : t) if ((unsigned char)ch < 0x20) ch = ' ';
    if (t.find(';') != std::string::npos) return luaL_argerror(L, 1, "the text can't hold ';'");
    return edit(L, "say " + t, 0, 0, 0, true, K_NONE);
}
int apiTime(lua_State* L) { return edit(L, "time " + numArg(L, 1), 0, 0, 0, true, K_NONE); }
int apiTimefreeze(lua_State* L) {
    luaL_checkany(L, 1);
    return edit(L, std::string("timefreeze ") + (lua_toboolean(L, 1) ? "on" : "off"), 0, 0, 0, true, K_NONE);
}
int apiHeal(lua_State* L) { return edit(L, "heal", 0, 0, 0, true, K_NONE); }
int apiHurt(lua_State* L) { return edit(L, "hurt " + numArg(L, 1), 0, 0, 0, true, K_NONE); }
int apiGold(lua_State* L) { return edit(L, "gold " + numArg(L, 1), 0, 0, 0, true, K_NONE); }
int apiSound(lua_State* L) { return edit(L, "sound " + numArg(L, 1), 0, 0, 0, true, K_NONE); }
int apiFlash(lua_State* L) { return edit(L, "flash", 0, 0, 0, true, K_NONE); }

// AT.1c: eden.weather([kind [, secs]]) -> kind, intensity, locked, precip; eden.weatherlock(on).
int apiWeather(lua_State* L) {
    if (!worldOpen()) return luaL_error(L, "no world is open");
    if (!lua_isnoneornil(L, 1)) {
        bool snow;
        const char* kn = luaL_checkstring(L, 1);
        const int k = Weather::kindByName(kn, &snow);
        if (k < 0) return luaL_argerror(L, 1, lua_pushfstring(L, "invalid option '%s'", kn));   // luaL_checkoption's wording
        const double secs = luaL_optnumber(L, 2, 0);
        if (!Weather::active()) return luaL_error(L, "weather is off (Settings > Gameplay > Weather)");
        if (secs < 0 || secs > 1e7) return luaL_argerror(L, 2, "seconds from 0 to 10000000");
        Weather::set((Weather::Kind)k, (float)secs, snow);
    }
    lua_pushstring(L, Weather::forceSnow() ? "snow" : Weather::name(Weather::kind()));
    lua_pushnumber(L, Weather::intensity());
    lua_pushboolean(L, Weather::locked());
    const Player* p = World::getWorld->player;
    static const char* const kPrecip[] = {"none", "rain", "snow", "dry"};
    lua_pushstring(L, kPrecip[Weather::precipAt(p->pos.x, p->pos.z, p->pos.y)]);
    return 4;
}
int apiWeatherlock(lua_State* L) {
    luaL_checkany(L, 1);
    if (!worldOpen()) return luaL_error(L, "no world is open");
    if (!Weather::active()) return luaL_error(L, "weather is off (Settings > Gameplay > Weather)");
    Weather::setLocked(lua_toboolean(L, 1) != 0);
    return 0;
}

const char* blockName(int t) {
    if (t <= STOCK_BLOCK_MAX) return BlockRegistry::stockName(t);
    return BlockRegistry::nameOf(t);
}
void pushBlock(lua_State* L, int t) {
    const char* n = t >= 0 ? blockName(t) : NULL;
    if (n) lua_pushstring(L, n);
    else lua_pushfstring(L, "%d", t);   // a palette byte with no name (the missing block)
}

// eden.get(x, y, z) -> name, colour; nil when the cell is not in the resident window.
int apiGet(lua_State* L) {
    const int x = coordArg(L, 1), y = coordArg(L, 2), z = coordArg(L, 3);
    if (!worldOpen() || !CmdScript::residentCell(x, y, z)) return 0;
    Terrain* t = World::getWorld->terrain;
    const int type = t->getLand(x, z, y);
    if (type < 0) return 0;
    pushBlock(L, type);
    lua_pushinteger(L, t->getColor(x, z, y));
    return 2;
}
// K.6-3: eden.sign(x, y, z [, face]) -> text, colour, face (nothing if no sign there; face omitted = the first one on
// the block); eden.setsign(x, y, z, face, text [, colour]) -> true, or false, why. Both go through SignTool.
// Lua's (x, y up, z) is the trailer's (x, z, y) of the Terrain argument order; faces are the record's: 0 -x, 1 +x,
// 3 top, 4 -z, 5 +z.
int apiSign(lua_State* L) {
    const int x = coordArg(L, 1), y = coordArg(L, 2), z = coordArg(L, 3);
    const int face = lua_isnoneornil(L, 4) ? -1 : coordArg(L, 4);
    if (!worldOpen() || MpWorld::active()) return 0;
    std::string text;
    int colour, f;
    if (!SignTool::get(x, z, y, face, &text, &colour, &f)) return 0;
    lua_pushlstring(L, text.data(), text.size());
    lua_pushinteger(L, colour);
    lua_pushinteger(L, f);
    return 3;
}
int apiSetSign(lua_State* L) {
    const int x = coordArg(L, 1), y = coordArg(L, 2), z = coordArg(L, 3), face = coordArg(L, 4);
    size_t n;
    const char* text = luaL_checklstring(L, 5, &n);
    const int colour = lua_isnoneornil(L, 6) ? 0 : coordArg(L, 6);
    if (!worldOpen()) return luaL_error(L, "no world is open");
    if (MpWorld::active()) return luaL_error(L, "not in a multiplayer world");
    std::string why;
    bool ok;
    if (std::strlen(text) != n) { why = "a sign holds printable ASCII only (32 to 126)"; ok = false; }   // an embedded NUL
    else if (!CmdScript::residentCell(x, y, z)) { why = "that cell is not loaded"; ok = false; }
    else ok = SignTool::set(x, z, y, face, text, colour, World::getWorld->player->yaw, &why);
    lua_pushboolean(L, ok);
    if (ok) return 1;
    lua_pushstring(L, why.c_str());
    return 2;
}
// K.7-1a: eden.toast(text [, seconds]) -> true, or false when this pack toasted less than 1 s ago (game time). text is
// at most 120 bytes (longer raises), seconds is clamped to 1..6 (default 3). It is the HUD status line, so an engine
// status replaces it and a later toast replaces an engine status.
int apiToast(lua_State* L) {
    size_t n;
    const char* text = luaL_checklstring(L, 1, &n);
    const double secs = lua_isnoneornil(L, 2) ? 3.0 : luaL_checknumber(L, 2);
    if (n > 120) return luaL_argerror(L, 1, "the text is longer than 120 bytes");
    if (!worldOpen() || !World::getWorld->hud) return luaL_error(L, "no world is open");
    Pack* p = packOf(L);
    if (!testToastFlood && g_clock < p->toastNext) { lua_pushboolean(L, 0); return 1; }
    p->toastNext = g_clock + 1.0;
    std::string t(text, n);
    for (char& ch : t) if ((unsigned char)ch < 0x20) ch = ' ';
    World::getWorld->hud->sb->setStatus(t, (float)(secs < 1.0 ? 1.0 : secs > 6.0 ? 6.0 : secs));
    lua_pushboolean(L, 1);
    return 1;
}
// ---- K.7-1b: eden.ask / eden.input ------------------------------------------------------------------------
std::string cleanText(const char* s, size_t n) {
    std::string t(s, n);
    for (char& ch : t) if ((unsigned char)ch < 0x20) ch = ' ';
    return t;
}
// Reads the string at stack index idx (absolute) into out; false + err if it is not one, or too long.
bool readStr(lua_State* L, int idx, const std::string& what, size_t maxB, bool allowEmpty, std::string& out, std::string& err) {
    if (lua_type(L, idx) != LUA_TSTRING) { err = what + " must be a string"; return false; }
    size_t n;
    const char* s = lua_tolstring(L, idx, &n);
    if (n > maxB) { err = what + " is longer than " + std::to_string(maxB) + " bytes"; return false; }
    if (n == 0 && !allowEmpty) { err = what + " is empty"; return false; }
    out = cleanText(s, n);
    return true;
}
bool readButtons(lua_State* L, int idx, std::vector<std::string>& out, std::string& err) {
    if (lua_type(L, idx) != LUA_TTABLE) { err = "buttons must be a table of 1 to 4 strings"; return false; }
    const lua_Integer n = (lua_Integer)lua_rawlen(L, idx);
    if (n < 1 || n > 4) { err = "buttons must have 1 to 4 entries"; return false; }
    for (lua_Integer i = 1; i <= n; ++i) {
        lua_rawgeti(L, idx, i);
        std::string b;
        const bool ok = readStr(L, lua_gettop(L), "buttons[" + std::to_string(i) + "]", 24, false, b, err);
        lua_pop(L, 1);
        if (!ok) return false;
        out.push_back(b);
    }
    return true;
}
bool readInt(lua_State* L, int idx, const char* what, lua_Integer lo, lua_Integer hi, int& out, std::string& err) {
    int isnum = 0;
    const lua_Integer v = lua_tointegerx(L, idx, &isnum);
    if (!isnum || v < lo || v > hi) { err = std::string(what) + " must be a whole number " + std::to_string(lo) + ".." + std::to_string(hi); return false; }
    out = (int)v;
    return true;
}
// eden.ask(title, body, buttons) / eden.ask{...} and eden.input(title, body [, default]) / eden.input{...}.
// Never raises (the caller's strings are alive): false + err.
bool parseDialog(lua_State* L, DlgReq& r, std::string& err) {
    const std::string fn = r.input ? "eden.input" : "eden.ask";
    r.ok = "OK"; r.cancel = "Cancel";
    if (lua_type(L, 1) == LUA_TTABLE) {
        lua_pushnil(L);
        while (lua_next(L, 1)) {
            const int v = lua_gettop(L);
            const std::string key = lua_type(L, v - 1) == LUA_TSTRING ? lua_tostring(L, v - 1) : "";
            bool ok = true;
            if (key == "title") ok = readStr(L, v, "title", 60, true, r.title, err);
            else if (key == "body") ok = readStr(L, v, "body", 400, true, r.body, err);
            else if (!r.input && key == "buttons") ok = readButtons(L, v, r.buttons, err);
            else if (!r.input && key == "danger") ok = readInt(L, v, "danger", 1, 4, r.danger, err);
            else if (r.input && key == "default") ok = readStr(L, v, "default", 256, true, r.def, err);
            else if (r.input && key == "max") ok = readInt(L, v, "max", 1, 256, r.max, err);
            else if (r.input && key == "lines") ok = readInt(L, v, "lines", 1, 4, r.lines, err);
            else if (r.input && key == "ok") ok = readStr(L, v, "ok", 24, false, r.ok, err);
            else if (r.input && key == "cancel") ok = readStr(L, v, "cancel", 24, false, r.cancel, err);
            else { err = "unknown key '" + key + "'"; ok = false; }
            if (!ok) { lua_settop(L, 1); err = fn + ": " + err; return false; }
            lua_pop(L, 1);
        }
    } else {
        bool ok = readStr(L, 1, "title", 60, true, r.title, err);
        if (ok && !lua_isnoneornil(L, 2)) ok = readStr(L, 2, "body", 400, true, r.body, err);
        if (ok && !r.input) ok = readButtons(L, 3, r.buttons, err);
        if (ok && r.input && !lua_isnoneornil(L, 3)) ok = readStr(L, 3, "default", 256, true, r.def, err);
        if (!ok) { err = fn + ": " + err; return false; }
    }
    if (!r.input && r.buttons.empty()) { err = fn + ": buttons is required"; return false; }
    if (!r.input && r.danger > (int)r.buttons.size()) { err = fn + ": danger is past the last button"; return false; }
    if (r.input && (int)r.def.size() > r.max) r.def.resize(r.max);
    return true;
}

void dlgClosed(int ticket, int index, const char* text, const char* why) {
    if (g_dlgShownPack) g_dlgShownPack->dlgNext = g_clock + 0.5;   // V11: the player gets frames to reach the pause menu
    g_dlgShownTicket = 0;
    g_dlgShownPack = NULL;
    g_dlgResults.push_back(DlgRes{ticket, index, text ? text : "", why ? why : ""});
}
void onAskChosen(int j) { dlgClosed(g_dlgShownTicket, j + 1, "", ""); }
void onInputChosen(int j, const char* text) {
    if (j == 0) dlgClosed(g_dlgShownTicket, 0, text, "");
    else dlgClosed(g_dlgShownTicket, 0, "", "dismissed");
}
void onDialogEnd(int token, int why) {   // GLDialog: replaced by an engine dialog, or the footer link was tapped
    if (why == GLDialog::END_LINK) {
        if (g_dlgShownPack) g_dlgShownPack->muted = true;
        dlgClosed(token, 0, "", "muted");
    } else dlgClosed(token, 0, "", "replaced");
}

void showDialog(const DlgReq& r) {
    Pack* p = r.p;
    const bool link = p->dlgSeen && g_clock - p->dlgLast < 60.0;   // V11: a second dialog within 60 s of game time
    p->dlgSeen = true;
    p->dlgLast = g_clock;
    const char* b[4];
    if (!r.input) {
        for (size_t i = 0; i < r.buttons.size(); ++i) b[i] = r.buttons[i].c_str();
        GLDialog::show(r.title.c_str(), r.body.c_str(), b, (int)r.buttons.size(), onAskChosen);
        if (r.danger) GLDialog::setDanger(r.danger - 1);
    } else {
        b[0] = r.ok.c_str(); b[1] = r.cancel.c_str();
        GLDialog::prompt(r.title.c_str(), r.body.c_str(), r.def.c_str(), r.max, b, 2, onInputChosen, r.lines);
    }
    const std::string footer = "Mod: " + p->id;
    GLDialog::setOwner(r.ticket, footer.c_str(), link ? "Stop dialogs from this mod" : NULL, onDialogEnd);
    g_dlgShownTicket = r.ticket;   // after show(): a show() over a mod dialog reports to the OLD owner first
    g_dlgShownPack = p;
}

Co* coOfTicket(int ticket) {
    for (std::unique_ptr<Pack>& p : g_packs)
        for (Co& c : p->cos) if (!c.done && c.ticket == ticket) return &c;
    return NULL;
}
bool hasResult(int ticket) {
    for (const DlgRes& r : g_dlgResults) if (r.ticket == ticket) return true;
    return false;
}

// End of update(): one mod dialog at a time, the first queued request whose pack is past its 0.5 s gap.
void pumpDialogs() {
    for (size_t i = 0; i < g_dlgQueue.size();) {
        DlgReq& r = g_dlgQueue[i];
        if (r.p->stopped || !coOfTicket(r.ticket)) { g_dlgQueue.erase(g_dlgQueue.begin() + i); continue; }
        if (r.p->muted) { g_dlgResults.push_back(DlgRes{r.ticket, 0, "", "muted"}); g_dlgQueue.erase(g_dlgQueue.begin() + i); continue; }
        ++i;
    }
    if (GLDialog::active()) return;
    for (size_t i = 0; i < g_dlgQueue.size(); ++i) {
        if (g_clock < g_dlgQueue[i].p->dlgNext) continue;
        const DlgReq r = g_dlgQueue[i];
        g_dlgQueue.erase(g_dlgQueue.begin() + i);
        showDialog(r);
        return;
    }
}

int uiStep(lua_State* L, int, lua_KContext ctx) {
    const int ticket = (int)ctx;
    for (size_t i = 0; i < g_dlgResults.size(); ++i) {
        const DlgRes& r = g_dlgResults[i];
        if (r.ticket != ticket) continue;
        int n;
        if (!r.why.empty()) { lua_pushnil(L); lua_pushstring(L, r.why.c_str()); n = 2; }
        else if (r.index > 0) { lua_pushinteger(L, r.index); n = 1; }
        else { lua_pushlstring(L, r.text.data(), r.text.size()); n = 1; }
        g_dlgResults.erase(g_dlgResults.begin() + i);
        return n;
    }
    lua_pushnil(L);
    lua_pushstring(L, "dismissed");
    return 2;
}

// The call pauses the handler like eden.wait until the player answers (WAIT_UI: `Co::ticket`; no timer, no work
// budget, one coroutine slot). Returns the button index (ask) or the text (input), or nil, why.
int apiDialog(lua_State* L, bool input) {
    static char msg[300];
    const char* nilWhy = NULL;
    int ticket = 0;
    msg[0] = 0;
    {   // (everything with a destructor ends here: luaL_error and lua_yieldk longjmp)
        DlgReq r;
        std::string err;
        r.input = input;
        const std::string fn = input ? "eden.input" : "eden.ask";
        Pack* p = packOf(L);
        if (!parseDialog(L, r, err)) {}
        else if (g_leaving) err = fn + ": can't pause while the world closes";
        else if (!lua_isyieldable(L)) err = fn + " can't pause here (inside a callback that can't pause)";
        else if (!worldOpen()) err = "no world is open";
        else if (p->muted) nilWhy = "muted";
        else if (input && !GLDialog::textEntryAvailable()) nilWhy = "unavailable";
        else {
            int pending = 0;
            for (const DlgReq& q : g_dlgQueue) pending += q.p == p;
            Co* c = NULL;
            for (Co& k : p->cos) if (k.T == L && !k.done) c = &k;
            if (pending >= 1 || g_dlgQueue.size() >= 4) nilWhy = "busy";
            else if (!c) err = fn + " can't pause here";
            else {
                r.p = p;
                r.ticket = c->ticket = ++g_ticket;
                ticket = r.ticket;
                if (testDialogNoQueue) showDialog(r);   // mutation: no queue, a second dialog replaces the first
                else g_dlgQueue.push_back(r);
            }
        }
        if (!err.empty()) std::snprintf(msg, sizeof msg, "%s", err.c_str());
    }
    if (msg[0]) return luaL_error(L, "%s", msg);
    if (nilWhy) { lua_pushnil(L); lua_pushstring(L, nilWhy); return 2; }
    return lua_yieldk(L, 0, ticket, uiStep);
}
int apiAsk(lua_State* L) { return apiDialog(L, false); }
int apiInput(lua_State* L) { return apiDialog(L, true); }
int apiBlockId(lua_State* L) {
    const int id = CmdScript::blockByName(luaL_checkstring(L, 1));
    if (id < 0) return 0;
    lua_pushinteger(L, id);
    return 1;
}
int apiPlayer(lua_State* L) {
    if (!worldOpen()) return 0;
    World* w = World::getWorld;
    const Player* p = w->player;
    lua_createtable(L, 0, 9);
    lua_pushnumber(L, p->pos.x); lua_setfield(L, -2, "x");
    lua_pushnumber(L, p->pos.y - p->boxheight / 2); lua_setfield(L, -2, "y");   // the feet
    lua_pushnumber(L, p->pos.z); lua_setfield(L, -2, "z");
    lua_pushnumber(L, p->yaw); lua_setfield(L, -2, "yaw");
    lua_pushnumber(L, p->pitch); lua_setfield(L, -2, "pitch");
    lua_pushnumber(L, p->life); lua_setfield(L, -2, "health");
    lua_pushboolean(L, p->dead); lua_setfield(L, -2, "dead");
    lua_pushinteger(L, w->hud->goldencubes); lua_setfield(L, -2, "gold");
    int ax, ay, az;
    CmdScript::playerAnchor(&ax, &ay, &az);
    lua_pushinteger(L, ay); lua_setfield(L, -2, "cell_y");   // the cell under the feet (eden.cmd's anchor)
    // K.6-7: what the hand holds. block = the picked block's name (absent for a tool or a mod block not yet placed),
    // colour = the paint colour, mode = the tool in use.
    static const char* const kModes[] = {"camera", "pick_block", "build", "mine", "burn", "paint", "pick_color"};
    const int m = w->hud->mode;
    if (m >= 0 && m <= MODE_PICK_COLOR) { lua_pushstring(L, kModes[m]); lua_setfield(L, -2, "mode"); }
    const int bt = w->hud->blocktype;
    if (bt > 0 && !hud_picker_is_tool(bt) && !hud_picker_is_mod(bt)) { pushBlock(L, bt); lua_setfield(L, -2, "block"); }
    lua_pushinteger(L, w->hud->block_paintcolor); lua_setfield(L, -2, "colour");
    return 1;
}
// K.6-7: eden.look() -> x, y, z, face, block for the cell under the crosshair (the hit cell, not the build point);
// nothing for the sky, a creature, or a menu. face is the record's (0 -x, 1 +x, 2 under, 3 top, 4 -z, 5 +z) = the side hit.
int apiLook(lua_State* L) {
    if (!worldOpen()) return 0;
    int x, y, z;
    if (!Console::lookCell(&x, &y, &z)) return 0;
    World* w = World::getWorld;
    const Point3D place = findWorldCoords((int)(SCREEN_HEIGHT / 2), (int)(SCREEN_WIDTH / 2), FC_PLACE);
    int face = 3;
    if (place.x != -1) {
        const int dx = place.x - x, dy = place.y - y, dz = place.z - z;
        if (dx == -1) face = 0; else if (dx == 1) face = 1;
        else if (dy == -1) face = 2; else if (dy == 1) face = 3;
        else if (dz == -1) face = 4; else if (dz == 1) face = 5;
    }
    if (testLookBuild && place.x != -1) { x = place.x; y = place.y; z = place.z; }   // mutation: the empty cell in front
    lua_pushinteger(L, x); lua_pushinteger(L, y); lua_pushinteger(L, z); lua_pushinteger(L, face);
    pushBlock(L, w->terrain->getLand(x, z, y));
    return 5;
}
int apiClock(lua_State* L) { lua_pushnumber(L, g_clock); return 1; }
int apiWait(lua_State* L) {
    double s = luaL_optnumber(L, 1, 0);
    if (!(s >= 0)) s = 0;
    if (s > CmdScript::MAX_WAIT) s = CmdScript::MAX_WAIT;
    if (g_leaving) return luaL_error(L, "eden.wait: can't pause while the world closes");
    if (!lua_isyieldable(L)) return luaL_error(L, "eden.wait can't pause here (inside a callback that can't pause)");
    lua_settop(L, 0);
    lua_pushnumber(L, s);
    return lua_yield(L, 1);
}
int apiOn(lua_State* L) {
    Pack* p = packOf(L);
    const int ev = luaL_checkoption(L, 1, NULL, kEventNames);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    if ((int)p->handlers[ev].size() >= MAX_HANDLERS) return luaL_error(L, "at most %d handlers for \"%s\"", (int)MAX_HANDLERS, kEventNames[ev]);
    lua_pushvalue(L, 2);
    p->handlers[ev].push_back(luaL_ref(L, LUA_REGISTRYINDEX));
    return 0;
}
bool cmdNameOk(const char* s, size_t n) {
    if (n == 0 || n > 24) return false;
    for (size_t i = 0; i < n; ++i)
        if (!(std::islower((unsigned char)s[i]) || std::isdigit((unsigned char)s[i]) || s[i] == '_')) return false;
    return true;
}
int apiCommand(lua_State* L) {
    Pack* p = packOf(L);
    size_t n;
    const char* name = luaL_checklstring(L, 1, &n);
    const char* help = luaL_optstring(L, 2, "");
    luaL_checktype(L, 3, LUA_TFUNCTION);
    if (!cmdNameOk(name, n)) return luaL_argerror(L, 1, "a command name matches [a-z0-9_]{1,24}");
    for (const Cmd& c : p->cmds)
        if (c.name == name) return luaL_error(L, "the mod command \"%s\" is registered twice in pack %s", name, p->id.c_str());
    // K.8-4: another pack holding the short name is a warning, not an error: this file runs to the end, `mod <pack>:<name>`
    // always reaches this pack's command and the short name stays with the first pack that registered it.
    for (const std::unique_ptr<Pack>& o : g_packs) {
        if (o.get() == p) continue;
        bool taken = false;
        for (const Cmd& c : o->cmds) taken |= c.name == name;
        if (taken) {
            Console::print(CmdScript::Sink::ERR, "[" + p->id + "] warning: the mod command \"" + name + "\" is also pack " + o->id +
                           "'s: `mod " + name + "` runs " + o->id + "'s, `mod " + p->id + ":" + name + "` runs this one");
            break;
        }
    }
    if (p->cmds.size() >= MAX_HANDLERS) return luaL_error(L, "at most %d commands a pack", (int)MAX_HANDLERS);
    lua_pushvalue(L, 3);
    const int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    p->cmds.push_back(Cmd{name, help, ref});
    return 0;
}

// ---- K.6-5: eden.store, the world's per-pack saved data (record 13 MOD_DATA; docs/scripting.md "Saved data") ----------
// Host-side, so it outlives a pack's Lua state (`scripts reload`) and a warp; loaded by FileManager::finishLoadOnMain,
// written into the next save's batch when dirty, dropped when the world closes (reset()). Each pack's entry is kept as
// the record's bytes until the pack first touches it, so a pack that is not loaded, or only reads, is written back
// verbatim. A blob (the runtime's encoding, below) is: u8 version 1, then per key in ascending byte order u8 klen
// (1..64), the key, a value. Value: tag 0 false | 1 true | 2 i64 | 3 f64 (finite) | 4 u32 len + bytes (<= 4 KiB)
// | 5 u16 n (<= 256) + n x (key value, value), a key value being tag 2 or tag 4 (1..64 bytes); integer keys
// ascending, then string keys in byte order. Tables nest <= 4 deep. Every number is exact: an integer stays one.
enum { STORE_KEY_MAX = 64, STORE_STRING_MAX = 4096, STORE_TABLE_MAX = 256, STORE_DEPTH_MAX = 4, STORE_BLOB_VERSION = 1 };
enum { SV_FALSE = 0, SV_TRUE, SV_INT, SV_FLOAT, SV_STRING, SV_TABLE };
struct StoreEntry {
    std::vector<uint8_t> raw;                       // the record's bytes for this pack (as loaded, or as last saved)
    bool decoded = false;                           // kv holds raw's keys (or raw did not decode: bad)
    bool bad = false;
    bool touched = false;                           // a set since the load / last save: raw is rebuilt from kv
    std::map<std::string, std::string> kv;          // key -> encoded value
    size_t size = 1;                                // the blob kv encodes to
};
std::map<std::string, StoreEntry> g_store;          // by pack id: ascending, the record's order
bool g_storeDirty = false;

void storePut(std::string& o, const void* p, size_t n) { o.append((const char*)p, n); }
void storePutU(std::string& o, uint64_t v, int bytes) { for (int i = 0; i < bytes; ++i) o.push_back((char)(uint8_t)(v >> (8 * i))); }

// A Lua value -> its encoding, or false with the why (never raises on a value: eden.store.set returns it).
bool storeEncodeKey(lua_State* L, int idx, std::string& out, std::string* why) {
    if (lua_type(L, idx) == LUA_TNUMBER && lua_isinteger(L, idx)) {
        out.push_back((char)SV_INT);
        storePutU(out, (uint64_t)lua_tointeger(L, idx), 8);
        return true;
    }
    if (lua_type(L, idx) == LUA_TSTRING) {
        size_t n;
        const char* k = lua_tolstring(L, idx, &n);
        if (n >= 1 && n <= STORE_KEY_MAX) {
            out.push_back((char)SV_STRING);
            storePutU(out, n, 4);
            storePut(out, k, n);
            return true;
        }
    }
    *why = "a table key must be a string of 1 to 64 bytes or an integer";
    return false;
}
bool storeEncodeValue(lua_State* L, int idx, int depth, std::string& out, std::string* why) {
    idx = lua_absindex(L, idx);
    switch (lua_type(L, idx)) {
    case LUA_TBOOLEAN: out.push_back((char)(lua_toboolean(L, idx) ? SV_TRUE : SV_FALSE)); return true;
    case LUA_TNUMBER:
        if (lua_isinteger(L, idx)) { out.push_back((char)SV_INT); storePutU(out, (uint64_t)lua_tointeger(L, idx), 8); return true; }
        else {
            const double d = lua_tonumber(L, idx);
            if (!std::isfinite(d)) { *why = "can't store a number that is not finite"; return false; }
            uint64_t b;
            std::memcpy(&b, &d, 8);
            out.push_back((char)SV_FLOAT);
            storePutU(out, b, 8);
            return true;
        }
    case LUA_TSTRING: {
        size_t n;
        const char* v = lua_tolstring(L, idx, &n);
        if (n > STORE_STRING_MAX) { *why = "can't store a string over 4096 bytes"; return false; }
        out.push_back((char)SV_STRING);
        storePutU(out, n, 4);
        storePut(out, v, n);
        return true;
    }
    case LUA_TTABLE: {
        if (depth >= STORE_DEPTH_MAX) { *why = "can't store tables nested more than 4 deep"; return false; }
        luaL_checkstack(L, 4, "eden.store");
        std::vector<std::pair<std::string, std::string> > ints, strs;   // (encoded key, encoded value)
        lua_pushnil(L);
        while (lua_next(L, idx)) {   // raw: metatables are not consulted
            if (ints.size() + strs.size() >= STORE_TABLE_MAX) { lua_pop(L, 2); *why = "can't store a table over 256 entries"; return false; }
            std::string k, v;
            if (!storeEncodeKey(L, -2, k, why) || !storeEncodeValue(L, -1, depth + 1, v, why)) { lua_pop(L, 2); return false; }
            (k[0] == SV_INT ? ints : strs).push_back(std::make_pair(k, v));
            lua_pop(L, 1);
        }
        auto ikey = [](const std::string& k) { int64_t v = 0; for (int i = 7; i >= 0; --i) v = (int64_t)(((uint64_t)v << 8) | (uint8_t)k[1 + i]); return v; };
        std::sort(ints.begin(), ints.end(), [&](const std::pair<std::string, std::string>& a, const std::pair<std::string, std::string>& b) { return ikey(a.first) < ikey(b.first); });
        // a string key is tag + u32 len + bytes: byte order of the bytes after the length
        std::sort(strs.begin(), strs.end(), [](const std::pair<std::string, std::string>& a, const std::pair<std::string, std::string>& b) { return a.first.compare(5, std::string::npos, b.first, 5, std::string::npos) < 0; });
        out.push_back((char)SV_TABLE);
        storePutU(out, ints.size() + strs.size(), 2);
        for (const auto& kv : ints) { out += kv.first; out += kv.second; }
        for (const auto& kv : strs) { out += kv.first; out += kv.second; }
        return true;
    }
    case LUA_TNIL: *why = "nil deletes a key: it is not stored"; return false;
    default:
        *why = luaL_testudata(L, idx, kCreatureMeta) ? std::string("can't store a creature handle") : std::string("can't store a ") + luaL_typename(L, idx);
        return false;
    }
}

// Checks one encoded value (bounds, tags, depth); `push` (with L) also builds it on L's stack.
bool storeDecodeValue(const uint8_t* p, size_t n, size_t* o, int depth, lua_State* L) {
    if (*o >= n) return false;
    const uint8_t tag = p[(*o)++];
    auto rdU = [&](int bytes, uint64_t* v) {
        if (n - *o < (size_t)bytes) return false;
        *v = 0;
        for (int i = bytes - 1; i >= 0; --i) *v = (*v << 8) | p[*o + i];
        *o += bytes;
        return true;
    };
    uint64_t v;
    switch (tag) {
    case SV_FALSE: case SV_TRUE: if (L) lua_pushboolean(L, tag == SV_TRUE); return true;
    case SV_INT: if (!rdU(8, &v)) return false; if (L) lua_pushinteger(L, (lua_Integer)(int64_t)v); return true;
    case SV_FLOAT: {
        if (!rdU(8, &v)) return false;
        double d;
        std::memcpy(&d, &v, 8);
        if (!std::isfinite(d)) return false;
        if (L) lua_pushnumber(L, d);
        return true;
    }
    case SV_STRING:
        if (!rdU(4, &v) || v > STORE_STRING_MAX || n - *o < v) return false;
        if (L) lua_pushlstring(L, (const char*)p + *o, (size_t)v);
        *o += (size_t)v;
        return true;
    case SV_TABLE: {
        if (depth >= STORE_DEPTH_MAX || !rdU(2, &v) || v > STORE_TABLE_MAX) return false;
        if (L) { luaL_checkstack(L, 4, "eden.store"); lua_createtable(L, 0, (int)v); }
        for (uint64_t i = 0; i < v; ++i) {
            const size_t k0 = *o;
            if (*o >= n || (p[*o] != SV_INT && p[*o] != SV_STRING)) return false;
            if (!storeDecodeValue(p, n, o, STORE_DEPTH_MAX, L)) return false;   // a key: no table, so any depth
            if (p[k0] == SV_STRING && (*o - k0 - 5 < 1 || *o - k0 - 5 > STORE_KEY_MAX)) return false;
            if (!storeDecodeValue(p, n, o, depth + 1, L)) return false;
            if (L) lua_rawset(L, -3);
        }
        return true;
    }
    }
    return false;
}

// raw -> kv (the keys' value bytes, unchecked by Lua). False: the blob breaks a rule (the entry starts empty).
bool storeDecodeBlob(const std::vector<uint8_t>& raw, std::map<std::string, std::string>& kv) {
    kv.clear();
    const uint8_t* p = raw.data();
    const size_t n = raw.size();
    if (n == 0) return true;   // no bytes: an empty store (this writer never writes one; another may)
    if (p[0] != STORE_BLOB_VERSION) return false;
    size_t o = 1;
    std::string last;
    while (o < n) {
        const size_t kl = p[o++];
        if (kl < 1 || kl > STORE_KEY_MAX || n - o < kl) return false;
        std::string k((const char*)p + o, kl);
        o += kl;
        if (!kv.empty() && !(last < k)) return false;
        const size_t v0 = o;
        if (!storeDecodeValue(p, n, &o, 0, NULL)) return false;
        kv[k] = std::string((const char*)p + v0, o - v0);
        last = k;
    }
    return true;
}
size_t storeBlobSize(const std::map<std::string, std::string>& kv) {
    size_t n = 1;
    for (const auto& e : kv) n += 1 + e.first.size() + e.second.size();
    return n;
}
// The bytes this entry goes out as at the next save (raw, or kv rebuilt).
size_t storeEntryBytes(const StoreEntry& e) { return e.touched ? (e.kv.empty() ? 0 : e.size) : e.raw.size(); }
bool storeEntryPresent(const StoreEntry& e) { return e.touched ? !e.kv.empty() : true; }
std::vector<uint8_t> storeBlob(const StoreEntry& e) {
    if (!e.touched) return e.raw;
    std::vector<uint8_t> b(1, (uint8_t)STORE_BLOB_VERSION);
    for (const auto& kv : e.kv) {
        b.push_back((uint8_t)kv.first.size());
        b.insert(b.end(), kv.first.begin(), kv.first.end());
        b.insert(b.end(), kv.second.begin(), kv.second.end());
    }
    return b;
}

StoreEntry* storeOf(Pack* p, bool create) {
    std::map<std::string, StoreEntry>::iterator it = g_store.find(p->id);
    if (it == g_store.end()) {
        if (!create) return NULL;
        it = g_store.insert(std::make_pair(p->id, StoreEntry())).first;
        it->second.decoded = true;
        it->second.touched = true;   // empty and touched: written only once it holds a key (a refused first set leaves nothing)
    }
    StoreEntry& e = it->second;
    if (!e.decoded) {
        e.decoded = true;
        if (!storeDecodeBlob(e.raw, e.kv)) {
            e.bad = true;
            e.kv.clear();
            Console::print(CmdScript::Sink::ERR, "[" + p->id + "] eden.store: this world's saved data for the pack does not decode (" +
                           std::to_string(e.raw.size()) + " B); it starts empty, and the old bytes are kept until the pack stores something");
        }
        e.size = storeBlobSize(e.kv);
    }
    return &e;
}

Pack* storePack(lua_State* L) {
    Pack* p = packOf(L);
    if (p->console) luaL_error(L, "eden.store belongs to a pack's scripts (the console has none)");
    if (!worldOpen() || g_session) luaL_error(L, "no world is open");
    return p;
}
const char* storeKeyArg(lua_State* L, size_t* n) {
    if (lua_type(L, 1) != LUA_TSTRING) luaL_typeerror(L, 1, "string");
    const char* k = lua_tolstring(L, 1, n);
    if (*n < 1 || *n > STORE_KEY_MAX) luaL_argerror(L, 1, "a store key is a string of 1 to 64 bytes");
    return k;
}
int apiStoreGet(lua_State* L) {
    Pack* p = storePack(L);
    size_t n;
    const std::string k(storeKeyArg(L, &n), n);
    StoreEntry* e = storeOf(p, false);
    std::map<std::string, std::string>::const_iterator it;
    size_t o = 0;
    if (!e || (it = e->kv.find(k)) == e->kv.end() ||
        !storeDecodeValue((const uint8_t*)it->second.data(), it->second.size(), &o, 0, L))   // checked at load
        lua_pushnil(L);
    return 1;
}
int apiStoreSet(lua_State* L) {
    Pack* p = storePack(L);
    size_t n;
    const std::string k(storeKeyArg(L, &n), n);
    luaL_checkany(L, 2);
    auto refuse = [&](const std::string& why) { lua_pushboolean(L, 0); lua_pushstring(L, why.c_str()); return 2; };
    std::string v, why;
    const bool del = lua_isnil(L, 2);
    if (!del && !storeEncodeValue(L, 2, 0, v, &why)) return refuse(why);
    StoreEntry* e = storeOf(p, !del);
    if (!e) { lua_pushboolean(L, 1); return 1; }     // deleting from a store that has nothing
    std::map<std::string, std::string>::iterator it = e->kv.find(k);
    const size_t old = it == e->kv.end() ? 0 : 1 + k.size() + it->second.size();
    if (del ? it == e->kv.end() : (it != e->kv.end() && it->second == v && !e->bad)) { lua_pushboolean(L, 1); return 1; }   // no change
    const size_t next = e->size - old + (del ? 0 : 1 + k.size() + v.size());
    if (!del) {
        if (next > emod::MOD_DATA_ENTRY_MAX) return refuse("this pack's store is full (64 KiB)");
        // the whole record: every pack's entry as it would go out, with this one at `next`
        size_t total = emod::MOD_DATA_HEAD, packs = 0;
        for (const auto& se : g_store) {
            const bool self = &se.second == e;
            if (!self && !storeEntryPresent(se.second)) continue;
            packs++;
            total += emod::MOD_DATA_ENTRY_HEAD + (self ? next : storeEntryBytes(se.second));
        }
        if (packs > emod::MOD_DATA_MAX_PACKS) return refuse("too many packs store data in this world (64)");
        if (total > emod::MOD_DATA_MAX) return refuse("this world's mod data is full (1 MiB)");
    }
    if (del) e->kv.erase(it);
    else e->kv[k] = v;
    e->size = next;
    e->touched = true;
    e->bad = false;
    if (!testStoreNotDirty) g_storeDirty = true;
    lua_pushboolean(L, 1);
    return 1;
}
int apiStoreKeys(lua_State* L) {
    Pack* p = storePack(L);
    StoreEntry* e = storeOf(p, false);
    lua_createtable(L, e ? (int)e->kv.size() : 0, 0);
    int i = 0;
    if (e)
        for (const auto& kv : e->kv) {
            lua_pushlstring(L, kv.first.data(), kv.first.size());
            lua_rawseti(L, -2, ++i);
        }
    return 1;
}
// K.7-2b (V15): eden.setting(id) -> the player's value for one of this pack's pack.json "settings": a boolean (toggle),
// number (slider), or string (choice: the option; text). Read from Packs/.settings/<pack id>.json on the first call and
// kept until the world closes (the page is on the Mods screen, which never opens in a world); a value that fails the
// pack's schema reads as the default. An id the pack does not declare raises.
int apiSetting(lua_State* L) {
    Pack* p = packOf(L);
    size_t n;
    const std::string id(luaL_checklstring(L, 1, &n), n);
    const ContentPack::ModPack* mp = NULL;
    for (const ContentPack::ModPack& q : ContentPack::modReport().packs)
        if (q.state == ContentPack::PACK_LOADED && !q.refused && q.id == p->id) { mp = &q; break; }
    if (!mp) return luaL_argerror(L, 1, "this pack declares no settings");
    if (!p->settingsRead) {
        p->settingVals = ContentPack::settingValues(World::getWorld->fm->documents, *mp);
        p->settingsRead = true;
    }
    for (size_t i = 0; i < mp->settings.size() && i < p->settingVals.size(); ++i) {
        const ContentPack::ModSetting& st = mp->settings[i];
        if (st.id.empty() || st.id != id) continue;
        const ContentPack::SettingValue& v = p->settingVals[i];
        if (st.type == "toggle") lua_pushboolean(L, v.b);
        else if (st.type == "slider") lua_pushnumber(L, v.n);
        else lua_pushlstring(L, v.s.data(), v.s.size());
        return 1;
    }
    return luaL_argerror(L, 1, "this pack declares no such setting");
}

// ---- K.7-3a: eden.menu{...} (V8, V12, V13) ---------------------------------------------------------------
// The spec is read once into ModMenu::Spec (plain C++: no Lua value is held but the two callbacks and the handle),
// with every cap of plan V8; a violation raises with its path ("tabs[2].rows[7].label: longer than 40 bytes"), an
// unknown row type or key raises too (silently dropping a row would hide the bug). ModMenuScreen draws it; what the
// player does comes back through menuInput / menuClosed and runs as an ordinary handler coroutine in step 2.
const char* const kMenuMeta = "emod.menu";
enum { MENU_TABS_MAX = 8, MENU_TAB_ROWS_MAX = 48, MENU_ROWS_MAX = 128, MENU_LABEL_MAX = 40, MENU_HINT_MAX = 120,
       MENU_TEXT_MAX = 400, MENU_ACTION_MAX = 16, MENU_OPTIONS_MAX = 8, MENU_OPTION_MAX = 24, MENU_STEPS_MAX = 1000 };
struct MenuUD { int serial; };
struct MenuEv { int serial; bool close; int tab, row; ModMenu::Row now; };
std::vector<MenuEv> g_menuEvents;
int g_menuSerial = 0;
std::set<std::string> g_iconWarned;   // unknown icon names already reported (once per name until the world closes)

bool menuIdOk(const std::string& s) { return cmdNameOk(s.data(), s.size()); }
bool readBool(lua_State* L, int idx, const std::string& what, bool& out, std::string& err) {
    if (lua_type(L, idx) != LUA_TBOOLEAN) { err = what + " must be true or false"; return false; }
    out = lua_toboolean(L, idx) != 0;
    return true;
}
bool readNum(lua_State* L, int idx, const std::string& what, double& out, std::string& err) {
    if (lua_type(L, idx) != LUA_TNUMBER || !std::isfinite((double)lua_tonumber(L, idx))) { err = what + " must be a number"; return false; }
    out = (double)lua_tonumber(L, idx);
    return true;
}
bool readIcon(lua_State* L, int idx, const std::string& what, Pack* p, int& out, std::string& err) {
    std::string name;
    if (!readStr(L, idx, what, 64, false, name, err)) return false;
    out = name.find(':') != std::string::npos ? ContentPack::packIconByName(name) : uiIconByName(name.c_str());   // K.7-3c: "<pack>:<icon>"
    if (out < 0) {   // V13: a pack written for a later atlas still opens; the fallback draws and the console says so once
        out = UIICON_FALLBACK;
        if (g_iconWarned.insert(name).second)
            Console::print(CmdScript::Sink::ERR, "[" + p->id + "] warning: " + what + ": no icon \"" + name + "\" in this build or world (drawn as \"settings\")");
    }
    return true;
}
const char* const kRowTypes[] = {"heading", "text", "row", "button", "toggle", "slider", "choice"};

// Reads one key of a row (or of m:set's fields, `set` = true: no id/type, a value checked against the row as it is).
bool menuRowKey(lua_State* L, int v, const std::string& key, const std::string& kp, Pack* p, ModMenu::Row& r,
                bool set, int* choiceValue, std::string& err) {
    const int t = r.type;
    if (key == "label") return readStr(L, v, kp, t == ModMenu::R_TEXT ? MENU_TEXT_MAX : MENU_LABEL_MAX, false, r.label, err);
    if (key == "hint" && t >= ModMenu::R_ROW) return readStr(L, v, kp, MENU_HINT_MAX, true, r.hint, err);
    if (key == "detail" && t == ModMenu::R_ROW) return readStr(L, v, kp, MENU_HINT_MAX, true, r.detail, err);
    if (key == "icon" && t == ModMenu::R_ROW) return readIcon(L, v, kp, p, r.icon, err);
    if (key == "action" && t == ModMenu::R_BUTTON) return readStr(L, v, kp, MENU_ACTION_MAX, false, r.action, err);
    if (key == "danger" && t == ModMenu::R_BUTTON) return readBool(L, v, kp, r.danger, err);
    if (key == "enabled" && t >= ModMenu::R_ROW) return readBool(L, v, kp, r.enabled, err);
    if (key == "value" && t == ModMenu::R_TOGGLE) return readBool(L, v, kp, r.on, err);
    if (key == "value" && t == ModMenu::R_SLIDER) {
        if (!readNum(L, v, kp, r.value, err)) return false;
        if (set && (r.value < r.min || r.value > r.max)) { err = kp + " is outside min..max"; return false; }
        return true;
    }
    if (key == "value" && t == ModMenu::R_CHOICE) {
        int n = 0;
        if (!readInt(L, v, kp.c_str(), 1, MENU_OPTIONS_MAX, n, err)) return false;
        if (set && n > (int)r.options.size()) { err = kp + " is past the last option"; return false; }
        *choiceValue = n;
        return true;
    }
    if (!set && t == ModMenu::R_SLIDER && (key == "min" || key == "max" || key == "step"))
        return readNum(L, v, kp, key == "min" ? r.min : key == "max" ? r.max : r.step, err);
    if (!set && key == "options" && t == ModMenu::R_CHOICE) {
        if (lua_type(L, v) != LUA_TTABLE) { err = kp + " must be a table of 2 to 8 strings"; return false; }
        const lua_Integer n = (lua_Integer)lua_rawlen(L, v);
        if (n < 2 || n > MENU_OPTIONS_MAX) { err = kp + " must have 2 to 8 entries"; return false; }
        r.options.clear();
        for (lua_Integer i = 1; i <= n; ++i) {
            lua_rawgeti(L, v, i);
            std::string o;
            const bool ok = readStr(L, lua_gettop(L), kp + "[" + std::to_string((long long)i) + "]", MENU_OPTION_MAX, false, o, err);
            lua_pop(L, 1);
            if (!ok) return false;
            r.options.push_back(o);
        }
        return true;
    }
    err = kp + ": unknown key (type '" + std::string(kRowTypes[t]) + "')";
    return false;
}

bool parseMenuRow(lua_State* L, int idx, const std::string& path, Pack* p, ModMenu::Row& r, std::string& err) {
    const int top = lua_gettop(L);
    idx = lua_absindex(L, idx);
    if (lua_type(L, idx) != LUA_TTABLE) { err = path + " must be a table"; return false; }
    lua_pushliteral(L, "type");
    lua_rawget(L, idx);
    std::string type;
    bool ok = readStr(L, lua_gettop(L), path + ".type", 16, false, type, err);
    lua_settop(L, top);
    if (!ok) return false;
    r = ModMenu::Row();
    r.type = -1;
    for (int k = 0; k < (int)(sizeof kRowTypes / sizeof kRowTypes[0]); ++k) if (type == kRowTypes[k]) r.type = k;
    if (r.type < 0) { err = path + ".type: unknown row type '" + type + "'"; return false; }
    bool hasLabel = false, hasId = false, hasAction = false, hasMin = false, hasMax = false, hasValue = false, hasOptions = false;
    int choiceValue = 1;
    lua_pushnil(L);
    while (lua_next(L, idx)) {
        const int v = lua_gettop(L);
        if (lua_type(L, v - 1) != LUA_TSTRING) { lua_settop(L, top); err = path + ": a key must be a name"; return false; }
        const std::string key = lua_tostring(L, v - 1), kp = path + "." + key;
        if (key == "type") ok = true;
        else if (key == "id") {
            hasId = true;
            ok = readStr(L, v, kp, 24, false, r.id, err);
            if (ok && !menuIdOk(r.id)) { err = kp + ": an id matches [a-z0-9_]{1,24}"; ok = false; }
        } else {
            ok = menuRowKey(L, v, key, kp, p, r, false, &choiceValue, err);
            hasLabel |= key == "label"; hasAction |= key == "action"; hasMin |= key == "min"; hasMax |= key == "max";
            hasValue |= key == "value"; hasOptions |= key == "options";
        }
        if (!ok) { lua_settop(L, top); return false; }
        lua_pop(L, 1);
    }
    lua_settop(L, top);
    if (!hasLabel) { err = path + ".label is required"; return false; }
    if (r.type >= ModMenu::R_ROW && !hasId) { err = path + ".id is required (on_action names the row by it)"; return false; }
    if (r.type == ModMenu::R_BUTTON && !hasAction) { err = path + ".action is required (the button's text)"; return false; }
    if (r.type == ModMenu::R_SLIDER) {
        if (!hasMin || !hasMax) { err = path + ": a slider needs min and max"; return false; }
        if (!(r.min < r.max)) { err = path + ": min must be less than max"; return false; }
        if (!(r.step > 0)) { err = path + ".step must be more than 0"; return false; }
        if ((r.max - r.min) / r.step > MENU_STEPS_MAX + 1e-9) { err = path + ": (max - min) / step is more than 1000"; return false; }
        if (!hasValue) r.value = r.min;
        if (r.value < r.min || r.value > r.max) { err = path + ".value is outside min..max"; return false; }
        r.value = std::min(r.max, r.min + std::floor((r.value - r.min) / r.step + 0.5) * r.step);
    }
    if (r.type == ModMenu::R_CHOICE) {
        if (!hasOptions) { err = path + ".options is required"; return false; }
        if (choiceValue > (int)r.options.size()) { err = path + ".value is past the last option"; return false; }
        r.choice = choiceValue - 1;
    }
    return true;
}

// rows = the table at idx; appends to tab.rows, counting toward *total.
bool parseMenuRows(lua_State* L, int idx, const std::string& path, Pack* p, ModMenu::Tab& tab, int* total,
                   std::set<std::string>& ids, std::string& err) {
    if (lua_type(L, idx) != LUA_TTABLE) { err = path + " must be a table of rows"; return false; }
    const lua_Integer n = (lua_Integer)lua_rawlen(L, idx);
    if (n > MENU_TAB_ROWS_MAX) { err = path + ": more than 48 rows in a tab"; return false; }
    for (lua_Integer i = 1; i <= n; ++i) {
        const std::string rp = path + "[" + std::to_string((long long)i) + "]";
        if (++*total > MENU_ROWS_MAX) { err = rp + ": more than 128 rows in the menu"; return false; }
        lua_rawgeti(L, idx, i);
        ModMenu::Row r;
        const bool ok = parseMenuRow(L, lua_gettop(L), rp, p, r, err);
        lua_pop(L, 1);
        if (!ok) return false;
        if (!r.id.empty() && !ids.insert(r.id).second) { err = rp + ".id: '" + r.id + "' is used twice in this menu"; return false; }
        tab.rows.push_back(r);
    }
    return true;
}

bool parseMenu(lua_State* L, int idx, Pack* p, ModMenu::Spec& s, std::string& err) {
    const int top = lua_gettop(L);
    bool hasTitle = false, hasTabs = false, hasRows = false;
    int total = 0;
    std::set<std::string> ids, tabIds;
    lua_pushnil(L);
    while (lua_next(L, idx)) {
        const int v = lua_gettop(L);
        if (lua_type(L, v - 1) != LUA_TSTRING) { lua_settop(L, top); err = "a key must be a name"; return false; }
        const std::string key = lua_tostring(L, v - 1);
        bool ok = true;
        if (key == "id") {
            ok = readStr(L, v, "id", 24, false, s.id, err);
            if (ok && !menuIdOk(s.id)) { err = "id: an id matches [a-z0-9_]{1,24}"; ok = false; }
        } else if (key == "title") { hasTitle = true; ok = readStr(L, v, "title", MENU_LABEL_MAX, false, s.title, err); }
        else if (key == "on_action" || key == "on_close") {
            if (lua_type(L, v) != LUA_TFUNCTION && lua_type(L, v) != LUA_TNIL) { err = key + " must be a function"; ok = false; }
        } else if (key == "rows") {
            hasRows = true;
            s.tabs.push_back(ModMenu::Tab());
            ok = parseMenuRows(L, v, "rows", p, s.tabs.back(), &total, ids, err);
        } else if (key == "tabs") {
            hasTabs = true;
            if (lua_type(L, v) != LUA_TTABLE) { err = "tabs must be a table of 1 to 8 tabs"; ok = false; }
            const lua_Integer n = ok ? (lua_Integer)lua_rawlen(L, v) : 0;
            if (ok && (n < 1 || n > MENU_TABS_MAX)) { err = "tabs must have 1 to 8 entries"; ok = false; }
            for (lua_Integer i = 1; ok && i <= n; ++i) {
                const std::string tp = "tabs[" + std::to_string((long long)i) + "]";
                lua_rawgeti(L, v, i);
                const int ti = lua_gettop(L);
                ModMenu::Tab tab;
                bool tabRows = false;
                if (lua_type(L, ti) != LUA_TTABLE) { err = tp + " must be a table"; ok = false; }
                if (ok) {
                    lua_pushnil(L);
                    while (ok && lua_next(L, ti)) {
                        const int tv = lua_gettop(L);
                        const std::string tk = lua_type(L, tv - 1) == LUA_TSTRING ? lua_tostring(L, tv - 1) : "", kp = tp + "." + tk;
                        if (tk == "id") {
                            ok = readStr(L, tv, kp, 24, false, tab.id, err);
                            if (ok && !menuIdOk(tab.id)) { err = kp + ": an id matches [a-z0-9_]{1,24}"; ok = false; }
                            if (ok && !tabIds.insert(tab.id).second) { err = kp + ": '" + tab.id + "' is used twice in this menu"; ok = false; }
                        } else if (tk == "label") ok = readStr(L, tv, kp, MENU_LABEL_MAX, false, tab.label, err);
                        else if (tk == "icon") ok = readIcon(L, tv, kp, p, tab.icon, err);
                        else if (tk == "rows") { tabRows = true; ok = parseMenuRows(L, tv, kp, p, tab, &total, ids, err); }
                        else { err = tk.empty() ? tp + ": a key must be a name" : kp + ": unknown key for a tab"; ok = false; }
                        lua_settop(L, ok ? tv - 1 : ti);
                    }
                }
                if (ok && !tabRows) { err = tp + ".rows is required"; ok = false; }
                if (ok && n > 1 && tab.label.empty()) { err = tp + ".label is required (the menu has more than one tab)"; ok = false; }
                if (ok && n > 1 && tab.id.empty()) { err = tp + ".id is required (the menu has more than one tab)"; ok = false; }
                lua_settop(L, v);
                if (ok) s.tabs.push_back(tab);
            }
        } else { err = key + ": unknown key"; ok = false; }
        if (!ok) { lua_settop(L, top); return false; }
        lua_pop(L, 1);
    }
    lua_settop(L, top);
    if (!hasTitle) { err = "title is required"; return false; }
    if (hasTabs == hasRows) { err = hasTabs ? "give tabs or rows, not both" : "tabs (or rows) is required"; return false; }
    s.pack = p->id;
    return true;
}

MenuRec* menuRec(Pack* p, int serial) {
    for (std::unique_ptr<MenuRec>& m : p->menus) if (m->serial == serial) return m.get();
    return NULL;
}
bool menuFind(int serial, Pack** pp, MenuRec** mm) {
    for (std::unique_ptr<Pack>& p : g_packs)
        if (MenuRec* m = menuRec(p.get(), serial)) { *pp = p.get(); *mm = m; return true; }
    return false;
}
// The record closed (the player, m:close): on_close is queued; the handle stays referenced until it has run.
void menuClose(MenuRec* m) {
    m->open = false;
    g_menuEvents.push_back(MenuEv{m->serial, true, 0, 0, ModMenu::Row()});
}
bool menuShow(Pack* p, MenuRec* m, lua_State* L, int handleIdx) {
    if (ModMenuScreen::isOpen() || p->muted) return false;
    if (m->self == LUA_NOREF) { lua_pushvalue(L, handleIdx); m->self = luaL_ref(L, LUA_REGISTRYINDEX); }
    m->open = true;
    ModMenuScreen::open(m->serial, m->spec, m->tab);
    return true;
}

// eden.menu{...} -> a handle, or nil, why ("busy": a mod menu is open; "muted": the player stopped this pack's dialogs).
int apiMenu(lua_State* L) {
    Pack* p = packOf(L);
    if (g_leaving) return luaL_error(L, "eden.menu: can't pause while the world closes");
    if (!worldOpen() || g_session) return luaL_error(L, "no world is open");
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_settop(L, 1);
    std::unique_ptr<MenuRec> m(new MenuRec());
    std::string err;
    if (!parseMenu(L, 1, p, m->spec, err)) return luaL_error(L, "eden.menu: %s", err.c_str());
    if (p->muted || ModMenuScreen::isOpen()) {
        lua_pushnil(L);
        lua_pushstring(L, p->muted ? "muted" : "busy");
        return 2;
    }
    MenuUD* u = (MenuUD*)lua_newuserdatauv(L, sizeof(MenuUD), 0);   // index 2
    u->serial = 0;
    luaL_setmetatable(L, kMenuMeta);
    for (const char* f : {"on_action", "on_close"}) {
        lua_pushstring(L, f);
        lua_rawget(L, 1);
        const int ref = lua_type(L, -1) == LUA_TFUNCTION ? luaL_ref(L, LUA_REGISTRYINDEX) : (lua_pop(L, 1), LUA_NOREF);
        (f[3] == 'a' ? m->onAction : m->onClose) = ref;
    }
    m->serial = u->serial = ++g_menuSerial;
    MenuRec* rec = m.get();
    p->menus.push_back(std::move(m));
    menuShow(p, rec, L, 2);
    lua_settop(L, 2);
    return 1;
}

MenuRec* checkMenu(lua_State* L, Pack** pp) {
    MenuUD* u = (MenuUD*)luaL_checkudata(L, 1, kMenuMeta);
    Pack* p = packOf(L);
    MenuRec* m = menuRec(p, u->serial);
    if (!m) luaL_error(L, "this menu is gone");
    *pp = p;
    return m;
}
// m:set(row, {label=, detail=, hint=, value=, enabled=, action=, danger=, icon=}) -> true
int menuSet(lua_State* L) {
    Pack* p;
    MenuRec* m = checkMenu(L, &p);
    size_t n;
    const std::string id(luaL_checklstring(L, 2, &n), n);
    luaL_checktype(L, 3, LUA_TTABLE);
    lua_settop(L, 3);
    int ti = -1, ri = -1;
    for (size_t t = 0; t < m->spec.tabs.size() && ti < 0; ++t)
        for (size_t r = 0; r < m->spec.tabs[t].rows.size(); ++r)
            if (m->spec.tabs[t].rows[r].id == id) { ti = (int)t; ri = (int)r; break; }
    if (ti < 0) return luaL_argerror(L, 2, "this menu has no row with that id");
    ModMenu::Row r = m->spec.tabs[(size_t)ti].rows[(size_t)ri];
    std::string err;
    int choiceValue = r.choice + 1;
    lua_pushnil(L);
    while (lua_next(L, 3)) {
        if (lua_type(L, -2) != LUA_TSTRING) return luaL_error(L, "m:set: a key must be a name");
        const std::string key = lua_tostring(L, -2);
        if (!menuRowKey(L, lua_gettop(L), key, key, p, r, true, &choiceValue, err)) return luaL_error(L, "m:set: %s", err.c_str());
        lua_pop(L, 1);
    }
    if (r.type == ModMenu::R_SLIDER) r.value = std::min(r.max, r.min + std::floor((r.value - r.min) / r.step + 0.5) * r.step);
    if (r.type == ModMenu::R_CHOICE) r.choice = choiceValue - 1;
    m->spec.tabs[(size_t)ti].rows[(size_t)ri] = r;
    if (m->open && ModMenuScreen::serial() == m->serial) ModMenuScreen::setRow(ti, ri, r);
    lua_pushboolean(L, 1);
    return 1;
}
// m:tab(id) -> true, or false when the menu has no such tab.
int menuTab(lua_State* L) {
    Pack* p;
    MenuRec* m = checkMenu(L, &p);
    const char* id = luaL_checkstring(L, 2);
    for (size_t t = 0; t < m->spec.tabs.size(); ++t)
        if (m->spec.tabs[t].id == id) {
            m->tab = (int)t;
            if (m->open && ModMenuScreen::serial() == m->serial) ModMenuScreen::showTab((int)t);
            lua_pushboolean(L, 1);
            return 1;
        }
    lua_pushboolean(L, 0);
    return 1;
}
// m:close() -> true, or false when it was not open. on_close runs (step 2 of the next update).
int menuCloseApi(lua_State* L) {
    Pack* p;
    MenuRec* m = checkMenu(L, &p);
    if (!m->open) { lua_pushboolean(L, 0); return 1; }
    if (ModMenuScreen::serial() == m->serial) ModMenuScreen::close();
    menuClose(m);
    lua_pushboolean(L, 1);
    return 1;
}
// m:open() -> true (it is open), or false while another mod menu is open or the pack is muted.
int menuOpenApi(lua_State* L) {
    Pack* p;
    MenuRec* m = checkMenu(L, &p);
    if (g_leaving) return luaL_error(L, "m:open: can't pause while the world closes");
    if (m->open) { lua_pushboolean(L, 1); return 1; }
    lua_pushboolean(L, menuShow(p, m, L, 1));
    return 1;
}
int menuGc(lua_State* L) {
    MenuUD* u = (MenuUD*)lua_touserdata(L, 1);
    Pack* p = packOf(L);
    for (size_t i = 0; i < p->menus.size(); ++i) {
        MenuRec* m = p->menus[i].get();
        if (m->serial != u->serial) continue;
        if (!p->closing) {   // (inside lua_close the whole registry goes anyway)
            if (m->onAction != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, m->onAction);
            if (m->onClose != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, m->onClose);
        }
        p->menus.erase(p->menus.begin() + (long)i);
        break;
    }
    return 0;
}
const luaL_Reg kMenuMethods[] = {{"set", menuSet}, {"tab", menuTab}, {"close", menuCloseApi}, {"open", menuOpenApi}, {NULL, NULL}};

// ---- K.7-3b: the pause menu's Mods page (U2) ----------------------------------------------------------------
struct PageTab { std::string pack; bool menu; };
std::vector<PageTab> g_pageTabs;
// A `setting` / `menu` event for one pack (not broadcast: the value belongs to that pack).
struct PackEv { std::string pack; int ev; std::string id; ContentPack::SettingValue now, old; std::string type; };
std::vector<PackEv> g_packEvents;
const ContentPack::ModPack* loadedPack(const std::string& id) {
    for (const ContentPack::ModPack& q : ContentPack::modReport().packs)
        if (q.state == ContentPack::PACK_LOADED && !q.refused && q.id == id) return &q;
    return NULL;
}
Pack* livePack(const std::string& id) {
    for (std::unique_ptr<Pack>& p : g_packs) if (p->id == id && !p->console) return p.get();
    return NULL;
}
bool packHandlesMenu(const std::string& id) {
    Pack* p = livePack(id);
    return p && !p->stopped && !p->handlers[EV_MENU].empty();
}
bool hasRealSettings(const ContentPack::ModPack& mp) {
    for (const ContentPack::ModSetting& st : mp.settings) if (!st.id.empty() && st.type != "heading") return true;
    return false;
}
void queueSettingEvent(const std::string& pack, int ev, const std::string& id, const std::string& type,
                       const ContentPack::SettingValue& now, const ContentPack::SettingValue& old) {
    if ((int)g_packEvents.size() >= MAX_EVENTS) { g_stats.eventsDropped++; return; }
    g_packEvents.push_back(PackEv{pack, ev, id, now, old, type});
}
std::string g_textPack;
int g_textIdx = -1;
void pageTextCb(int chosen, const char* text) {
    const std::string pid = g_textPack;
    const int i = g_textIdx;
    g_textPack.clear();
    g_textIdx = -1;
    const ContentPack::ModPack* mp = loadedPack(pid);
    if (chosen != 0 || !mp || !World::getWorld || !World::getWorld->fm || i < 0 || i >= (int)mp->settings.size()) return;
    const std::string& docs = World::getWorld->fm->documents;
    std::vector<ContentPack::SettingValue> vals = ContentPack::settingValues(docs, *mp);
    ContentPack::SettingValue v = vals[(size_t)i], old = v;
    std::string t = text ? text : "";
    for (char& c : t) if ((unsigned char)c < 0x20 || c == 0x7f) c = ' ';
    const int mx = mp->settings[(size_t)i].maxLen;
    if ((int)t.size() > mx) {
        t.resize((size_t)mx);
        while (!t.empty() && ((unsigned char)t[t.size() - 1] & 0xC0) == 0x80) t.erase(t.size() - 1);
        if (!t.empty() && (unsigned char)t[t.size() - 1] >= 0xC0) t.erase(t.size() - 1);
    }
    v.s = t;
    if (v.s == old.s || !ContentPack::saveSetting(docs, *mp, (size_t)i, v)) return;
    if (Pack* lp = livePack(pid)) if (lp->settingsRead && i < (int)lp->settingVals.size()) lp->settingVals[(size_t)i] = v;
    queueSettingEvent(pid, EV_SETTING, mp->settings[(size_t)i].id, "text", v, old);
    if (ModMenuScreen::serial() == MODS_PAGE) {   // the button shows the new text
        for (size_t t2 = 0; t2 < g_pageTabs.size(); ++t2)
            if (g_pageTabs[t2].pack == pid) {
                ModMenu::Row r;
                r.type = ModMenu::R_BUTTON; r.id = mp->settings[(size_t)i].id; r.label = mp->settings[(size_t)i].label;
                r.hint = mp->settings[(size_t)i].hint; r.action = v.s.size() > 24 ? v.s.substr(0, 22) + ".." : v.s;
                if (r.action.empty()) r.action = "Edit";
                ModMenuScreen::setRow((int)t2, i + (g_pageTabs[t2].menu ? 1 : 0), r);
            }
    }
}

void dispatch(Pack* p, int ev, const std::function<int(lua_State*)>& push);
// K.7-3b: `setting` (id, value, old) and `menu` () go to the one pack the Mods page named.
void dispatchPackEvents() {
    std::vector<PackEv> evs;
    evs.swap(g_packEvents);
    for (const PackEv& e : evs) {
        Pack* p = livePack(e.pack);
        if (!p || p->stopped || !p->L || p->handlers[e.ev].empty()) continue;
        g_stats.events++;
        dispatch(p, e.ev, [&](lua_State* L) {
            if (e.ev == EV_MENU) return 0;
            auto push = [&](const ContentPack::SettingValue& v) {
                if (e.type == "toggle") lua_pushboolean(L, v.b);
                else if (e.type == "slider") lua_pushnumber(L, v.n);
                else lua_pushlstring(L, v.s.data(), v.s.size());
            };
            lua_pushlstring(L, e.id.data(), e.id.size());
            push(e.now);
            push(e.old);
            return 3;
        });
    }
}

// Step 2 of update(): what the player did in the menu this frame, in order.
void dispatchMenus() {
    std::vector<MenuEv> evs;
    evs.swap(g_menuEvents);
    for (const MenuEv& e : evs) {
        Pack* p;
        MenuRec* m;
        if (!menuFind(e.serial, &p, &m) || !p->L || m->self == LUA_NOREF) continue;
        const int fn = e.close ? m->onClose : m->onAction;
        const int self = m->self;
        if (fn != LUA_NOREF && !p->stopped) {
            const ModMenu::Row now = e.now;
            const std::string rowId = e.close ? std::string() : m->spec.tabs[(size_t)e.tab].rows[(size_t)e.row].id;
            if (spawn(p, fn, [&](lua_State* L) {
                    lua_rawgeti(L, LUA_REGISTRYINDEX, self);
                    if (e.close) return 1;
                    lua_pushlstring(L, rowId.data(), rowId.size());
                    switch (now.type) {
                    case ModMenu::R_TOGGLE: lua_pushboolean(L, now.on); break;
                    case ModMenu::R_SLIDER: lua_pushnumber(L, now.value); break;
                    case ModMenu::R_CHOICE: lua_pushinteger(L, now.choice + 1); break;
                    default: lua_pushnil(L); break;
                    }
                    return 3;
                }, -1, e.close ? "menu on_close" : "menu on_action", NULL, false))
                runNew(p);
        }
        if (e.close && !m->open && m->self != LUA_NOREF && menuFind(e.serial, &p, &m)) {   // (the handler may have reopened it)
            luaL_unref(p->L, LUA_REGISTRYINDEX, m->self);
            m->self = LUA_NOREF;
        }
    }
    dispatchPackEvents();
}

const luaL_Reg kStoreApi[] = {{"get", apiStoreGet}, {"set", apiStoreSet}, {"keys", apiStoreKeys}, {NULL, NULL}};

const luaL_Reg kApi[] = {
    {"cmd", apiCmd}, {"set", apiSet}, {"fill", apiFill}, {"paint", apiPaint}, {"explode", apiExplode},
    {"fire", apiFire}, {"tp", apiTp}, {"run", apiRun}, {"spawn", apiSpawn}, {"creatures", apiCreatures}, {"say", apiSay},
    {"time", apiTime}, {"timefreeze", apiTimefreeze}, {"heal", apiHeal}, {"hurt", apiHurt}, {"gold", apiGold},
    {"sound", apiSound}, {"flash", apiFlash}, {"get", apiGet}, {"sign", apiSign}, {"setsign", apiSetSign}, {"toast", apiToast}, {"setting", apiSetting}, {"menu", apiMenu}, {"ask", apiAsk}, {"input", apiInput}, {"blockid", apiBlockId}, {"player", apiPlayer}, {"look", apiLook},
    {"weather", apiWeather}, {"weatherlock", apiWeatherlock}, {"clock", apiClock}, {"wait", apiWait}, {"on", apiOn}, {"command", apiCommand}, {NULL, NULL}};

// K.8-8: one line of help per eden.* function: what `--lua-api-dump` writes as docs/eden-api.lua (LuaLS stubs). Every kApi
// name needs a row here (apiDocsMissing(), checked in CI). args are the Lua parameter names, `?` suffix = optional.
struct ApiDoc { const char* name; const char* args; const char* doc; };
const ApiDoc kApiDocs[] = {
    {"cmd", "line, ax?, ay?, az?", "Runs any block-command line; returns cells, refused, why."},
    {"set", "x, y, z, block, colour?", "Sets one cell; returns ok, why (false when refused)."},
    {"fill", "x1, y1, z1, x2, y2, z2, block, colour?", "Fills a box (at most 4096 cells); returns cells, refused, why."},
    {"paint", "x, y, z, colour", "Paints one cell."},
    {"explode", "x, y, z", "Explodes at a cell."},
    {"fire", "x, y, z", "Sets a cell on fire."},
    {"tp", "x, y, z", "Teleports the player."},
    {"run", "x, y, z", "Runs the command block at a cell."},
    {"spawn", "creature, x?, y?, z?", "Spawns a creature; returns a handle, or false, why."},
    {"creatures", "x, y, z, r", "Returns an array of creature handles within r (up to 64), nearest first."},
    {"say", "text", "Shows a chat line."},
    {"time", "t", "Sets the world time."},
    {"timefreeze", "on", "Freezes or releases the world time."},
    {"heal", "", "Heals the player fully."},
    {"hurt", "n", "Hurts the player."},
    {"gold", "n", "Gives or takes golden cubes."},
    {"sound", "n", "Plays a sound by number."},
    {"flash", "", "Flashes the screen."},
    {"get", "x, y, z", "Returns the block name and colour at a cell, or nothing if it is not loaded."},
    {"sign", "x, y, z, face?", "Returns text, colour, face of a sign, or nothing."},
    {"setsign", "x, y, z, face, text, colour?", "Writes (or with empty text deletes) a sign; returns true, or false, why."},
    {"toast", "text, seconds?", "Shows a line in the HUD status bar; false if this pack toasted less than 1 s ago."},
    {"setting", "id", "Returns the player's value of one of this pack's settings."},
    {"menu", "spec", "Opens a full-screen menu over the world; returns a menu handle, or nil, why."},
    {"ask", "title, body, buttons", "Raises a dialog and waits; returns the button index 1..n, or nil, why."},
    {"input", "title, body, default?", "Raises a text dialog and waits; returns the text, or nil, why."},
    {"blockid", "name", "Returns the byte this world uses for a block name, or nothing."},
    {"player", "", "Returns {x, y, z, yaw, pitch, health, dead, gold, cell_y, block?, colour, mode}."},
    {"look", "", "Returns x, y, z, face, block of the cell under the crosshair, or nothing."},
    {"weather", "kind?, secs?", "Starts a weather spell; returns kind, intensity, locked, precip."},
    {"weatherlock", "on", "Locks the current weather spell so it never ends."},
    {"clock", "", "Returns seconds of game time since the scripts started."},
    {"wait", "seconds", "Pauses this handler for game seconds (0 = next frame)."},
    {"on", "event, fn", "Registers a handler for an event (load, tick, build, mine, paint, burn, weather, use, sign, leave, setting, menu)."},
    {"command", "name, help, fn", "Registers a console command run as `mod NAME ARGS...`."},
};
}  // namespace

std::string apiStubs() {
    std::string o = "---@meta\n-- Generated by `eden_native --lua-api-dump` from Classes/ModScript.cpp (kApiDocs). Do not edit.\n"
                    "---Emod mod-script API (beta). docs/scripting.md has the full description of every call.\n---@class eden\n"
                    "eden = {}\n---The id of the pack this script belongs to.\n---@type string\neden.pack = \"\"\n"
                    "---The scripting API version.\n---@type integer\neden.version = 2\n"
                    "eden.store = {}\n---Returns the value saved under key (a copy), or nil.\nfunction eden.store.get(key) end\n"
                    "---Saves a value (nil deletes); returns true, or false, why.\nfunction eden.store.set(key, value) end\n"
                    "---Returns an array of this pack's keys.\nfunction eden.store.keys() end\n";
    for (const ApiDoc& d : kApiDocs) {
        o += std::string("---") + d.doc + "\n";
        std::string args = d.args, plain;
        for (char c : args) if (c != '?') plain += c;
        o += std::string("function eden.") + d.name + "(" + plain + ") end\n";
    }
    return o;
}

std::string apiDocsMissing() {
    std::string miss;
    for (const luaL_Reg* r = kApi; r->name; ++r) {
        bool found = false;
        for (const ApiDoc& d : kApiDocs) found |= !std::strcmp(d.name, r->name);
        if (!found) miss += std::string(miss.empty() ? "" : ", ") + r->name;
    }
    for (const ApiDoc& d : kApiDocs) {
        bool found = false;
        for (const luaL_Reg* r = kApi; r->name; ++r) found |= !std::strcmp(d.name, r->name);
        if (!found) miss += std::string(miss.empty() ? "" : ", ") + d.name + " (documented, not in the API)";
    }
    return miss;
}

bool syntaxCheck(const std::string& source, const std::string& chunkName, std::string* err) {
    lua_State* L = luaL_newstate();
    if (!L) { if (err) *err = "out of memory"; return false; }
    const int st = luaL_loadbufferx(L, source.data(), source.size(), chunkName.c_str(), "t");
    if (st != LUA_OK && err) *err = lua_tostring(L, -1) ? lua_tostring(L, -1) : "syntax error";
    lua_close(L);
    return st == LUA_OK;
}

namespace {

void openLib(lua_State* L, const char* name, lua_CFunction open) {
    luaL_requiref(L, name, open, 1);
    lua_pop(L, 1);
}

void setupState(lua_State* L) {
    Pack* p = packOf(L);
    openLib(L, LUA_GNAME, luaopen_base);
    openLib(L, LUA_COLIBNAME, luaopen_coroutine);
    openLib(L, LUA_STRLIBNAME, luaopen_string);
    openLib(L, LUA_TABLIBNAME, luaopen_table);
    openLib(L, LUA_MATHLIBNAME, luaopen_math);
    openLib(L, LUA_UTF8LIBNAME, luaopen_utf8);
    lua_pushcfunction(L, luaPrint);
    lua_setglobal(L, "print");
    if (!testNoSandbox) {
        for (const char* g : {"dofile", "loadfile"}) { lua_pushnil(L); lua_setglobal(L, g); }
        lua_pushcfunction(L, safeLoad);
        lua_setglobal(L, "load");
        lua_pushcfunction(L, safeGC);
        lua_setglobal(L, "collectgarbage");
        lua_getglobal(L, "setmetatable");
        lua_pushcclosure(L, safeSetmetatable, 1);
        lua_setglobal(L, "setmetatable");
        lua_getglobal(L, LUA_STRLIBNAME);
        lua_pushnil(L);
        lua_setfield(L, -2, "dump");
        for (const char* f : {"find", "match", "gmatch", "gsub"}) {
            lua_getfield(L, -1, f);
            lua_pushcclosure(L, guardedPattern, 1);
            lua_setfield(L, -2, f);
        }
        lua_pop(L, 1);
    }
    luaL_newmetatable(L, kEditMeta);
    lua_pushcfunction(L, editGc);
    lua_setfield(L, -2, "__gc");
    lua_pop(L, 1);
    luaL_newmetatable(L, kMenuMeta);   // K.7-3a: the eden.menu handle
    luaL_newlib(L, kMenuMethods);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, menuGc);
    lua_setfield(L, -2, "__gc");
    lua_pushliteral(L, "emod.menu");
    lua_setfield(L, -2, "__metatable");
    lua_pop(L, 1);
    luaL_newmetatable(L, kCreatureMeta);   // K.6-4a: the creature handle
    luaL_newlib(L, kCreatureMethods);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, creatureToString);
    lua_setfield(L, -2, "__tostring");
    lua_pushliteral(L, "emod.creature");
    lua_setfield(L, -2, "__metatable");
    lua_pop(L, 1);
    luaL_newlib(L, kApi);
    luaL_newlib(L, kStoreApi);
    lua_setfield(L, -2, "store");
    lua_pushstring(L, p->id.c_str());
    lua_setfield(L, -2, "pack");
    lua_pushinteger(L, API_VERSION);
    lua_setfield(L, -2, "version");
    lua_setglobal(L, "eden");
}

int atPanic(lua_State* L) {
    std::fprintf(stderr, "[lua] PANIC (an unprotected Lua call in ModScript.cpp): %s\n", lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "?");
    return 0;   // Lua aborts after this
}

Pack* newPack(const std::string& id, bool console) {
    std::unique_ptr<Pack> up(new Pack());
    Pack* p = up.get();
    p->id = id;
    p->console = console;
    g_packs.push_back(std::move(up));
    p->L = lua_newstate(luaAlloc, p);
    if (!p->L) { stopPack(p, "not enough memory for a Lua state"); return p; }
    *(Pack**)lua_getextraspace(p->L) = p;
    lua_atpanic(p->L, atPanic);
    lua_setwarnf(p->L, luaWarn, p);
    lua_sethook(p->L, countHook, LUA_MASKCOUNT, HOOK_EVERY);   // coroutines inherit it
    std::string err;
    if (!protect(p, setupState, &err)) stopPack(p, "could not set up the sandbox: " + err);
    return p;
}

void closePack(Pack* p) {
    if (!p->L) return;
    for (const std::unique_ptr<MenuRec>& m : p->menus)   // K.7-3a: its menu goes with its state, no on_close
        if (m->open && ModMenuScreen::serial() == m->serial) ModMenuScreen::close();
    p->closing = true;
    p->cos.clear();
    lua_close(p->L);
    p->L = NULL;
    p->menus.clear();
}

void closeAll() {
    if (GLDialog::ownerToken()) GLDialog::dismiss();   // a mod's dialog does not outlive its world
    if (ModMenuScreen::isOpen()) ModMenuScreen::close();   // nor its menu (no on_close: the states are closing)
    g_menuEvents.clear();
    g_packEvents.clear();
    g_dlgQueue.clear();
    g_dlgResults.clear();
    g_dlgShownTicket = 0;
    g_dlgShownPack = NULL;
    // K.8-6: keep what the Mods screen shows after the world (and its states) are gone; a world with no script packs keeps the older report
    {
        std::vector<Info> now = info();
        now.erase(std::remove_if(now.begin(), now.end(), [](const Info& i) { return i.pack == "console"; }), now.end());
        if (!now.empty()) g_lastReport = now;
    }
    for (std::unique_ptr<Pack>& p : g_packs) closePack(p.get());
    g_packs.clear();
    g_queue.clear();
}

// Loads a chunk (text only) as a new coroutine.
bool spawnChunk(Pack* p, const std::string& src, const std::string& chunkName, const std::string& what,
                CmdScript::Sink* sink, bool printResults, std::string* syntax) {
    int st = LUA_OK;
    std::string msg;
    bool ok = spawn(p, LUA_NOREF, [&](lua_State* L) {
        st = luaL_loadbufferx(L, src.data(), src.size(), chunkName.c_str(), "t");
        if (st != LUA_OK) {
            msg = plainText(L, -1);
            lua_pop(L, 1);
            lua_pushnil(L);   // a placeholder function slot; the coroutine is dropped below
        }
        return 0;
    }, -1, what, sink, printResults);
    if (ok && st != LUA_OK) {
        finishCo(p, p->cos.back());
        if (syntax) *syntax = msg;
        return false;
    }
    return ok;
}

void dispatch(Pack* p, int ev, const std::function<int(lua_State*)>& push) {
    for (size_t h = 0; h < p->handlers[ev].size() && !p->stopped; ++h) {
        const int ref = p->handlers[ev][h];
        if (ev == EV_TICK) {   // one live call per tick handler: a slow one skips frames, never piles up
            bool alive = false;
            for (const Co& c : p->cos) alive |= !c.done && c.ev == EV_TICK && c.handler == ref;
            if (alive) continue;
        }
        if (spawn(p, ref, push, ev, std::string(kEventNames[ev]) + " handler", NULL, false)) runNew(p);
    }
}

// K.8-1: `load` once, when the pack's last top-level chunk has finished (it may be paused by the instruction budget
// for frames, or not yet started when earlier packs used the frame's share). Called where no coroutine is running.
void fireLoads() {
    for (size_t i = 0; i < g_packs.size(); ++i) {
        Pack* p = g_packs[i].get();
        if (!p->loadDue) continue;
        p->loadDue = false;
        if (!p->stopped) dispatch(p, EV_LOAD, nullptr);
    }
}

void startPacks() {
    g_started = true;
    g_clock = 0;
    const ContentPack::ModReport& r = ContentPack::modReport();
    for (const ContentPack::ModPack& mp : r.packs) {
        if (mp.state != ContentPack::PACK_LOADED || mp.refused || mp.scripts.empty()) continue;
        Pack* p = newPack(mp.id, false);
        for (const ContentPack::ModScriptSrc& s : mp.scripts) {
            if (p->stopped) break;
            std::string syntax;
            if (!spawnChunk(p, s.source, "@" + mp.id + "/" + s.path, s.path, NULL, false, &syntax)) {
                if (!syntax.empty()) { scriptError(p, syntax, NULL); stopPack(p, s.path + " does not compile"); }
                break;
            }
            p->cos.back().top = true;
            p->booting++;
            runNew(p);   // its top level runs now, in list order, up to its first eden.wait
        }
        if (testLoadEarly) { p->loadDue = false; p->booting = -1000000; if (!p->stopped) dispatch(p, EV_LOAD, nullptr); }   // mutation: the old dispatch at start
    }
    fireLoads();
}

void sweep() {
    for (size_t i = 0; i < g_packs.size();) {
        Pack* p = g_packs[i].get();
        if (p->stopped && p->L) closePack(p);
        size_t k = 0;
        for (size_t j = 0; j < p->cos.size(); ++j) if (!p->cos[j].done) p->cos[k++] = p->cos[j];
        p->cos.resize(k);
        ++i;
    }
}

Pack* consolePack(bool create) {
    for (std::unique_ptr<Pack>& p : g_packs) if (p->console) return p.get();
    return create ? newPack("console", true) : NULL;
}

}  // namespace

size_t totalMem() { return g_allMem; }
Limits& limits() { return g_lim; }
void restoreLimits() { g_lim = Limits(); }

void worldEntered(bool session) {
    reset();
    g_session = session;
    g_pending = !session;
}

void reset() {
    if (g_depth > 0) { g_resetPending = true; return; }
    closeAll();
    g_store.clear();   // K.6-5: the world closed (its exit save has been written) or the next one is loading
    g_storeDirty = false;
    g_iconWarned.clear();
    g_pending = g_started = false;
    g_clock = 0;
}

void restart() {
    if (g_depth > 0) return;
    // K.8-7: read the script files again first (a refused pack keeps its old sources and says why); then start over.
    if (!g_session)
        for (const ContentPack::ModPack& mp : ContentPack::modReport().packs) {
            if (mp.state != ContentPack::PACK_LOADED || mp.refused || mp.scripts.empty()) continue;
            std::vector<std::string> why;
            if (!ContentPack::rereadScripts(mp.id, &why))
                for (const std::string& w : why)
                    Console::print(CmdScript::Sink::ERR, "[" + mp.id + "] scripts reload: " + w + " (keeping the sources read when the world opened)");
        }
    closeAll();
    g_started = false;
    g_pending = !g_session;
}

void noteEdit(int ev, int x, int z, int y, int before) {
    if (!g_started || g_session || ev < EV_BUILD || ev >= EV_COUNT || !worldOpen()) return;
    bool any = false;
    for (const std::unique_ptr<Pack>& p : g_packs) any |= !p->stopped && !p->handlers[ev].empty();
    if (!any) return;
    Terrain* t = World::getWorld->terrain;
    const int after = t->getLand(x, z, y);
    QEvent q{ev, x, y, z, 0, 0, std::string()};
    switch (ev) {
    case EV_BUILD: if (after == before || after <= 0) return; q.a = after; break;
    case EV_MINE: if (after == before || before <= 0) return; q.a = before; break;
    case EV_PAINT: if (before <= 0) return; q.a = t->getColor(x, z, y); q.b = before; break;
    case EV_BURN: if (before <= 0 || !(blockinfo[before] & IS_FLAMMABLE)) return; q.a = before; break;
    case EV_USE: if (before <= 0 || (testUseFilter && !(blockinfo[before] & IS_FLAMMABLE))) return; q.a = before; break;   // K.6-1a: any non-air block
    }
    if ((int)g_queue.size() >= MAX_EVENTS) { g_stats.eventsDropped++; return; }
    g_queue.push_back(q);
}

void noteUse(int x, int z, int y, int type) {
    if (!g_started || g_session || !worldOpen() || type <= 0) return;
    bool any = false;
    for (const std::unique_ptr<Pack>& p : g_packs) any |= !p->stopped && !p->handlers[EV_USE].empty();
    if (!any) return;
    if ((int)g_queue.size() >= MAX_EVENTS) { g_stats.eventsDropped++; return; }
    g_queue.push_back(QEvent{EV_USE, x, y, z, type, 1, std::string()});   // b = 1: how "tap"
}

void noteWeather(int kind, int previous) {
    if (!g_started || g_session || !worldOpen()) return;
    bool any = false;
    for (const std::unique_ptr<Pack>& p : g_packs) any |= !p->stopped && !p->handlers[EV_WEATHER].empty();
    if (!any) return;
    if ((int)g_queue.size() >= MAX_EVENTS) { g_stats.eventsDropped++; return; }
    g_queue.push_back(QEvent{EV_WEATHER, 0, 0, 0, kind, previous, std::string()});
}

void noteSign(int x, int z, int y, int face, const char* text) {
    if (!g_started || g_session || !worldOpen()) return;
    bool any = false;
    for (const std::unique_ptr<Pack>& p : g_packs) any |= !p->stopped && !p->handlers[EV_SIGN].empty();
    if (!any) return;
    if ((int)g_queue.size() >= MAX_EVENTS) { g_stats.eventsDropped++; return; }
    g_queue.push_back(QEvent{EV_SIGN, x, y, z, face, 0, text ? text : ""});
}

void update(float etime) {
    if (g_resetPending && g_depth == 0) { g_resetPending = false; reset(); return; }
    if (!worldOpen() || g_session || MpWorld::active()) return;
    g_clock += etime;
    g_instrFrame = 0;
    for (std::unique_ptr<Pack>& p : g_packs) {
        p->instrLastFrame = p->instrFrame;
        p->instrFrame = 0;
        p->printsFrame = 0;
        if (p->toastQuiet > 0) p->toastQuiet -= etime;
        if (p->capQuiet > 0) p->capQuiet -= etime;
    }
    if (g_pending) { g_pending = false; startPacks(); }

    // 1. what waits (wait elapsed, preempted, an edit paused by the work budget), oldest first
    for (std::unique_ptr<Pack>& up : g_packs) {
        Pack* p = up.get();
        const size_t n = p->cos.size();
        for (size_t i = 0; i < n && !p->stopped; ++i) {
            if (p->cos[i].done) continue;
            const bool ui = p->cos[i].ticket != 0;   // K.7-1b: WAIT_UI, resumed only when its answer is recorded
            if (ui && !hasResult(p->cos[i].ticket)) continue;
            if (!ui) p->cos[i].wait -= etime;
            if ((!ui && p->cos[i].wait > 0) || overBudget(p)) continue;
            p->cos[i].ticket = 0;   // (uiStep finds the answer by its own continuation context)
            resumeCo(p, i);
        }
    }
    fireLoads();   // a top level that finished above (K.8-1)
    // 2. the player's edits since the last frame
    std::vector<QEvent> q;
    q.swap(g_queue);
    for (const QEvent& e : q) {
        g_stats.events++;
        for (std::unique_ptr<Pack>& p : g_packs) {
            if (p->stopped || p->handlers[e.ev].empty()) continue;
            dispatch(p.get(), e.ev, [&](lua_State* L) {
                if (e.ev == EV_WEATHER) {   // AT.1c: (kind, previous); snow is a where, not a kind
                    lua_pushstring(L, Weather::name(e.a));
                    lua_pushstring(L, Weather::name(e.b));
                    return 2;
                }
                lua_pushinteger(L, e.x);
                lua_pushinteger(L, e.y);
                lua_pushinteger(L, e.z);
                if (e.ev == EV_PAINT) { lua_pushinteger(L, e.a); pushBlock(L, e.b); return 5; }
                if (e.ev == EV_SIGN) { lua_pushinteger(L, e.a); lua_pushlstring(L, e.text.data(), e.text.size()); return 5; }   // K.6-3: face, text
                pushBlock(L, e.a);
                if (e.ev == EV_USE) { lua_pushstring(L, e.b == 1 ? "tap" : "fire"); return 5; }   // K.6-1a/b: how
                return 4;
            });
        }
    }
    dispatchMenus();   // K.7-3a: the menu's on_action / on_close for what the player did this frame (V10)
    // 3. tick
    for (std::unique_ptr<Pack>& p : g_packs)
        if (!p->stopped && !p->handlers[EV_TICK].empty())
            dispatch(p.get(), EV_TICK, [&](lua_State* L) { lua_pushnumber(L, etime); return 1; });
    pumpDialogs();
    sweep();
}

void leaving() {
    if (!g_started || g_session || MpWorld::active() || !worldOpen() || g_depth > 0 || g_leaving) return;
    bool any = false;
    for (const std::unique_ptr<Pack>& p : g_packs) any |= !p->stopped && !p->handlers[EV_LEAVE].empty();
    if (!any) return;
    g_leaving = true;
    g_stats.leaves++;
    CmdScript::freshWork();   // one fresh WORK_PER_FRAME for every handler together, as one frame would have
    g_instrFrame = 0;
    for (std::unique_ptr<Pack>& up : g_packs) {
        Pack* p = up.get();
        p->instrFrame = 0;
        const size_t n = p->handlers[EV_LEAVE].size();   // (a handler that calls eden.on("leave") adds none to this close)
        for (size_t h = 0; h < n && !p->stopped; ++h) {
            if (!spawn(p, p->handlers[EV_LEAVE][h], nullptr, EV_LEAVE, "leave handler", NULL, false)) continue;
            const size_t i = p->cos.size() - 1;
            resumeCo(p, i);   // whatever the frame budget says: the count hook does not preempt while leaving
            if (!p->cos[i].done) {   // it yielded (coroutine.yield at its top): there is no next frame to resume it
                scriptError(p, "leave handler: can't pause while the world closes", NULL);
                if (!p->cos[i].done) finishCo(p, p->cos[i]);
            }
        }
    }
    g_leaving = false;
    sweep();
}

bool runConsole(const std::string& code, CmdScript::Sink* sink) {
    if (!worldOpen()) { if (sink) sink->line(CmdScript::Sink::ERR, "No world is open"); return false; }
    if (g_session || MpWorld::active()) { if (sink) sink->line(CmdScript::Sink::ERR, "Not in a multiplayer world"); return false; }
    Pack* p = consolePack(true);
    if (p->stopped) { if (sink) sink->line(CmdScript::Sink::ERR, "The console's Lua state stopped: " + p->why); return false; }
    std::string syntax;
    // An expression first (its values are printed), then a statement.
    if (!spawnChunk(p, "return " + code, "=console", "lua", sink, true, &syntax)) {
        if (syntax.empty()) return false;
        syntax.clear();
        if (!spawnChunk(p, code, "=console", "lua", sink, true, &syntax)) {
            if (!syntax.empty() && sink) sink->line(CmdScript::Sink::ERR, syntax);
            return false;
        }
    }
    resumeCo(p, p->cos.size() - 1);   // the console runs now, whatever the frame budget says
    sweep();
    return true;
}

std::vector<Info> info() {
    std::vector<Info> v;
    for (const std::unique_ptr<Pack>& p : g_packs) {
        Info i;
        i.pack = p->id;
        i.mem = p->mem;
        i.memPeak = p->memPeak;
        i.instrLastFrame = p->instrLastFrame;
        i.instrTotal = p->instrTotal;
        i.handlers = (int)p->cmds.size();
        for (int e = 0; e < EV_COUNT; ++e) i.handlers += (int)p->handlers[e].size();
        i.coroutines = liveCos(p.get());
        i.errors = p->errors;
        i.preempts = p->preempts;
        i.skipped = p->skipped;
        i.memRefusals = p->memRefusals;
        i.stopped = p->stopped;
        i.why = p->stopped ? p->why : p->lastError;
        v.push_back(i);
    }
    return v;
}

std::vector<Info> lastReport() {
    std::vector<Info> v;
    if (testNoReport) return v;   // mutation
    if (!g_packs.empty()) {
        v = info();
        v.erase(std::remove_if(v.begin(), v.end(), [](const Info& i) { return i.pack == "console"; }), v.end());
        if (!v.empty()) return v;
    }
    return g_lastReport;
}

// The pack that owns a short command name: the first (load order) running pack that registered it.
static const Cmd* shortOwner(const std::string& name, Pack** owner) {
    for (std::unique_ptr<Pack>& up : g_packs) {
        if (up->stopped) continue;
        for (const Cmd& c : up->cmds)
            if (c.name == name) { *owner = up.get(); return &c; }
    }
    return NULL;
}

std::vector<std::string> commands() {
    std::vector<std::string> v;
    for (const std::unique_ptr<Pack>& p : g_packs)
        if (!p->stopped)
            for (const Cmd& c : p->cmds) {
                Pack* own = NULL;
                const bool shortOk = shortOwner(c.name, &own) && own == p.get();
                v.push_back((shortOk ? c.name : p->id + ":" + c.name) + (c.help.empty() ? "" : " - " + c.help) + " (" + p->id + ")");
            }
    return v;
}

std::vector<std::string> commandNames() {
    std::vector<std::string> v;
    for (const std::unique_ptr<Pack>& p : g_packs)
        if (!p->stopped)
            for (const Cmd& c : p->cmds) { v.push_back(c.name); v.push_back(p->id + ":" + c.name); }
    return v;
}

bool runCommand(const std::string& name, const std::vector<std::string>& args, CmdScript::Sink* sink) {
    Pack* p = NULL;
    const Cmd* cmd = NULL;
    const size_t colon = name.find(':');
    if (colon != std::string::npos) {   // `mod <pack>:<name>`: that pack's command, whoever else has the short name
        const std::string pid = name.substr(0, colon), cn = name.substr(colon + 1);
        for (std::unique_ptr<Pack>& up : g_packs) {
            if (up->stopped || up->id != pid) continue;
            for (const Cmd& c : up->cmds)
                if (c.name == cn) { p = up.get(); cmd = &c; }
        }
    } else {
        cmd = shortOwner(name, &p);
    }
    if (!cmd) return false;
    if (spawn(p, cmd->fn, [&](lua_State* L) {
            for (const std::string& a : args) lua_pushstring(L, a.c_str());
            return (int)args.size();
        }, -1, "mod " + name, sink, false))
        resumeCo(p, p->cos.size() - 1);
    sweep();
    return true;
}

// ---- K.7-3a: the menu screen's reports (World::update's input slot; Lua runs in step 2 of update())
void menuInput(int serial, int tab, int row, const ModMenu::Row& now) {
    Pack* p;
    MenuRec* m;
    if (!menuFind(serial, &p, &m) || tab < 0 || tab >= (int)m->spec.tabs.size() || row < 0 ||
        row >= (int)m->spec.tabs[(size_t)tab].rows.size()) return;
    m->spec.tabs[(size_t)tab].rows[(size_t)row] = now;   // the value the player set is the menu's now (m:set, a reopen)
    if ((int)g_menuEvents.size() < MAX_EVENTS) g_menuEvents.push_back(MenuEv{serial, false, tab, row, now});
    else g_stats.eventsDropped++;
}
bool modsPageAvailable() {
    if (!worldOpen() || g_session || MpWorld::active() || !g_started) return false;
    for (const ContentPack::ModPack& q : ContentPack::modReport().packs) {
        if (q.state != ContentPack::PACK_LOADED || q.refused) continue;
        if (hasRealSettings(q) || packHandlesMenu(q.id)) return true;
    }
    return false;
}
bool openModsPage() {
    if (!modsPageAvailable() || ModMenuScreen::isOpen()) return false;
    const std::string& docs = World::getWorld->fm->documents;
    ModMenu::Spec spec;
    spec.id = "mods";
    spec.title = "Mods";
    g_pageTabs.clear();
    for (const ContentPack::ModPack& q : ContentPack::modReport().packs) {
        if (q.state != ContentPack::PACK_LOADED || q.refused || spec.tabs.size() >= 8) continue;
        const bool menu = packHandlesMenu(q.id);
        if (!hasRealSettings(q) && !menu) continue;
        ModMenu::Tab t;
        t.id = q.id;
        t.label = q.name.empty() ? q.id : q.name;
        if (t.label.size() > 24) t.label.resize(24);
        if (menu) {
            ModMenu::Row r;
            r.type = ModMenu::R_BUTTON; r.id = "open_menu"; r.label = "Menu"; r.action = "Open";
            t.rows.push_back(r);
        }
        const std::vector<ContentPack::SettingValue> vals = ContentPack::settingValues(docs, q);
        for (size_t i = 0; i < q.settings.size(); ++i) {
            const ContentPack::ModSetting& st = q.settings[i];
            ModMenu::Row r;
            r.id = st.id; r.label = st.label; r.hint = st.hint;
            if (st.type == "heading") r.type = ModMenu::R_HEADING;
            else if (st.type == "toggle") { r.type = ModMenu::R_TOGGLE; r.on = vals[i].b; }
            else if (st.type == "slider") { r.type = ModMenu::R_SLIDER; r.min = st.min; r.max = st.max; r.step = st.step; r.value = vals[i].n; }
            else if (st.type == "choice") {
                r.type = ModMenu::R_CHOICE; r.options = st.options;
                for (size_t k = 0; k < st.options.size(); ++k) if (st.options[k] == vals[i].s) r.choice = (int)k;
            } else {
                r.type = ModMenu::R_BUTTON;
                r.action = vals[i].s.size() > 24 ? vals[i].s.substr(0, 22) + ".." : vals[i].s;
                if (r.action.empty()) r.action = "Edit";
            }
            t.rows.push_back(r);
        }
        spec.tabs.push_back(t);
        g_pageTabs.push_back(PageTab{q.id, menu});
    }
    if (spec.tabs.empty()) return false;
    ModMenuScreen::open(MODS_PAGE, spec, 0);
    return true;
}
void modsPageInput(int tab, int row, const ModMenu::Row& now) {
    if (tab < 0 || tab >= (int)g_pageTabs.size() || !World::getWorld || !World::getWorld->fm) return;
    const PageTab& pt = g_pageTabs[(size_t)tab];
    const ContentPack::ModPack* mp = loadedPack(pt.pack);
    if (!mp) return;
    if (pt.menu && row == 0) {   // "Open": the page gives way so the pack's eden.menu can open (its handler runs in step 2)
        ModMenuScreen::close();
        queueSettingEvent(pt.pack, EV_MENU, "", "", ContentPack::SettingValue(), ContentPack::SettingValue());
        return;
    }
    const int i = row - (pt.menu ? 1 : 0);
    if (i < 0 || i >= (int)mp->settings.size()) return;
    const ContentPack::ModSetting& st = mp->settings[(size_t)i];
    const std::string& docs = World::getWorld->fm->documents;
    if (st.type == "text") {
        if (!GLDialog::textEntryAvailable()) return;
        const std::vector<ContentPack::SettingValue> vals = ContentPack::settingValues(docs, *mp);
        g_textPack = pt.pack;
        g_textIdx = i;
        static const char* const kBtn[] = { "Save", "Cancel" };
        GLDialog::prompt(st.label.c_str(), st.hint.empty() ? "Type the new text." : st.hint.c_str(), vals[(size_t)i].s.c_str(), st.maxLen, kBtn, 2, pageTextCb);
        return;
    }
    std::vector<ContentPack::SettingValue> vals = ContentPack::settingValues(docs, *mp);
    const ContentPack::SettingValue old = vals[(size_t)i];
    ContentPack::SettingValue v = old;
    if (st.type == "toggle") v.b = now.on;
    else if (st.type == "slider") v.n = now.value;
    else if (st.type == "choice" && now.choice >= 0 && now.choice < (int)st.options.size()) v.s = st.options[(size_t)now.choice];
    else return;
    if (!ContentPack::saveSetting(docs, *mp, (size_t)i, v)) return;
    v = ContentPack::settingValues(docs, *mp)[(size_t)i];   // what the file holds (a slider snapped to its step)
    if (v.b == old.b && v.n == old.n && v.s == old.s) return;   // a drag inside one step
    if (Pack* lp = livePack(pt.pack)) if (lp->settingsRead && i < (int)lp->settingVals.size()) lp->settingVals[(size_t)i] = v;
    queueSettingEvent(pt.pack, EV_SETTING, st.id, st.type, v, old);
}
void menuClosed(int serial) {
    Pack* p;
    MenuRec* m;
    if (!menuFind(serial, &p, &m) || !m->open) return;
    menuClose(m);
}
size_t menuRecords() {
    size_t n = 0;
    for (const std::unique_ptr<Pack>& p : g_packs) n += p->menus.size();
    return n;
}

// ---- K.6-5: the store's host side (FileManager)
void storeLoad(const uint8_t* rec, size_t n) {
    g_store.clear();
    g_storeDirty = false;
    if (!rec || g_session) return;   // a session world loads none (and writes none: storeNeedsSave)
    std::vector<emod::ModDataEntry> es;
    std::string why;
    if (!emod::mod_data_parse(rec, n, &es, &why)) {
        // The whole record is ignored and stays in the file until a pack stores something (then a valid one supersedes it).
        Console::print(CmdScript::Sink::ERR, "eden.store: this world's mod data record is ignored: " + why);
        std::fprintf(stderr, "emod: MOD_DATA ignored: %s\n", why.c_str());
        return;
    }
    for (emod::ModDataEntry& m : es) g_store[m.id].raw.swap(m.data);
}
bool storeNeedsSave() { return g_storeDirty && !g_session; }
void storeEncode(std::vector<uint8_t>& out) {
    std::vector<emod::ModDataEntry> es;
    for (const auto& se : g_store) {
        const StoreEntry& e = se.second;
        if (!storeEntryPresent(e)) continue;
        emod::ModDataEntry m;
        m.id = se.first;
        m.data = storeBlob(e);
        es.push_back(std::move(m));
    }
    emod::mod_data_build(es, out);
}
void storeSaved() {
    // the bytes just committed become each touched entry's raw (so the next save copies them, and an emptied one goes)
    for (std::map<std::string, StoreEntry>::iterator it = g_store.begin(); it != g_store.end();) {
        StoreEntry& e = it->second;
        if (e.touched) {
            if (e.kv.empty()) { it = g_store.erase(it); continue; }
            e.raw = storeBlob(e);
            e.touched = false;
        }
        ++it;
    }
    g_storeDirty = false;
}
size_t storeEntries() { return g_store.size(); }

Stats stats() { return g_stats; }

const char* describe() {
    static char buf[400];
    int packs = 0, cos = 0;
    for (const std::unique_ptr<Pack>& p : g_packs) { packs += !p->console; cos += liveCos(p.get()); }
    std::snprintf(buf, sizeof buf,
                  "{\"packs\":%d,\"coroutines\":%d,\"started\":%d,\"finished\":%d,\"errors\":%d,\"preempted\":%d,"
                  "\"stopped\":%d,\"events\":%d,\"eventsDropped\":%d,\"edits\":%d,\"skipped\":%d,\"leaves\":%d}",
                  packs, cos, g_stats.started, g_stats.finished, g_stats.errors, g_stats.preempted, g_stats.stopped,
                  g_stats.events, g_stats.eventsDropped, g_stats.edits, g_stats.skipped, g_stats.leaves);
    return buf;
}

}  // namespace ModScript
