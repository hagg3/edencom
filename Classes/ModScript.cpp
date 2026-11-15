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
#include "EdenWorldStore.h"

#include "lua/lua.h"
#include "lua/lauxlib.h"
#include "lua/lualib.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <new>

namespace ModScript {

bool testNoHook = false;
bool testNoSandbox = false;
bool testLoadEarly = false;
bool testCapError = false;
bool testUseFilter = false;
bool testStoreNotDirty = false;

namespace {

const char* const kEventNames[] = {"load", "tick", "build", "mine", "paint", "burn", "weather", "use", "sign", NULL};
const char* const kEditMeta = "emod.edit";

struct Co {
    lua_State* T = NULL;
    int ref = LUA_NOREF;
    double wait = 0;
    int preempts = 0;
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
    CmdScript::Sink* sink = NULL;         // the running coroutine's sink
    std::vector<int> handlers[EV_COUNT];
    std::vector<Co> cos;
    std::vector<Cmd> cmds;
};
struct QEvent { int ev, x, y, z, a, b; std::string text; };

Limits g_lim;
}  // namespace
extern "C" int eden_script_low_memory(void);   // port/src/seam/DisplayProfile.cpp
size_t defaultAllMemBytes() { return eden_script_low_memory() ? (size_t)24 << 20 : (size_t)64 << 20; }
namespace {
Stats g_stats;
std::vector<std::unique_ptr<Pack>> g_packs;
std::vector<QEvent> g_queue;
bool g_pending = false;    // start the packs' scripts on the next update
bool g_started = false;    // they were started (this world)
bool g_session = false;
bool g_resetPending = false;
int g_depth = 0;           // > 0 while Lua runs (reset/restart wait for it)
long g_instrFrame = 0;
double g_clock = 0;

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
    if (overBudget(p) && L == p->running && lua_isyieldable(L)) {
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
    if (liveCos(p) >= MAX_COROUTINES) {
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
enum { K_SET = 0, K_COUNT, K_NONE };
struct EditJob {
    std::shared_ptr<const CmdScript::Program> prog;
    size_t pc = 0;
    int cells = 0, refused = 0, kind = K_NONE, ax = 0, ay = 0, az = 0;
    std::string firstRefusal;
};
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
    const bool done = CmdScript::runNow(j->prog, &j->pc, j->ax, j->ay, j->az, &sink, &j->cells, &j->refused);
    if (!done) {   // this frame's WORK_PER_FRAME is spent: carry on next frame (a block pauses the same way)
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
    return edit(L, "spawn " + wordArg(L, 1) + (lua_isnoneornil(L, 2) ? std::string() : " " + at3(L, 2)), 0, 0, 0, true, K_NONE);
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
    return 1;
}
int apiClock(lua_State* L) { lua_pushnumber(L, g_clock); return 1; }
int apiWait(lua_State* L) {
    double s = luaL_optnumber(L, 1, 0);
    if (!(s >= 0)) s = 0;
    if (s > CmdScript::MAX_WAIT) s = CmdScript::MAX_WAIT;
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
        *why = std::string("can't store a ") + luaL_typename(L, idx);
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
const luaL_Reg kStoreApi[] = {{"get", apiStoreGet}, {"set", apiStoreSet}, {"keys", apiStoreKeys}, {NULL, NULL}};

const luaL_Reg kApi[] = {
    {"cmd", apiCmd}, {"set", apiSet}, {"fill", apiFill}, {"paint", apiPaint}, {"explode", apiExplode},
    {"fire", apiFire}, {"tp", apiTp}, {"run", apiRun}, {"spawn", apiSpawn}, {"say", apiSay},
    {"time", apiTime}, {"timefreeze", apiTimefreeze}, {"heal", apiHeal}, {"hurt", apiHurt}, {"gold", apiGold},
    {"sound", apiSound}, {"flash", apiFlash}, {"get", apiGet}, {"sign", apiSign}, {"setsign", apiSetSign}, {"blockid", apiBlockId}, {"player", apiPlayer},
    {"weather", apiWeather}, {"weatherlock", apiWeatherlock}, {"clock", apiClock}, {"wait", apiWait}, {"on", apiOn}, {"command", apiCommand}, {NULL, NULL}};

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
    p->closing = true;
    p->cos.clear();
    lua_close(p->L);
    p->L = NULL;
}

void closeAll() {
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
            p->cos[i].wait -= etime;
            if (p->cos[i].wait > 0 || overBudget(p)) continue;
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
                if (e.ev == EV_USE) { lua_pushstring(L, "fire"); return 5; }   // K.6-1a: how (K.6-1b adds "tap")
                return 4;
            });
        }
    }
    // 3. tick
    for (std::unique_ptr<Pack>& p : g_packs)
        if (!p->stopped && !p->handlers[EV_TICK].empty())
            dispatch(p.get(), EV_TICK, [&](lua_State* L) { lua_pushnumber(L, etime); return 1; });
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
    static char buf[360];
    int packs = 0, cos = 0;
    for (const std::unique_ptr<Pack>& p : g_packs) { packs += !p->console; cos += liveCos(p.get()); }
    std::snprintf(buf, sizeof buf,
                  "{\"packs\":%d,\"coroutines\":%d,\"started\":%d,\"finished\":%d,\"errors\":%d,\"preempted\":%d,"
                  "\"stopped\":%d,\"events\":%d,\"eventsDropped\":%d,\"edits\":%d,\"skipped\":%d}",
                  packs, cos, g_stats.started, g_stats.finished, g_stats.errors, g_stats.preempted, g_stats.stopped,
                  g_stats.events, g_stats.eventsDropped, g_stats.edits, g_stats.skipped);
    return buf;
}

}  // namespace ModScript
