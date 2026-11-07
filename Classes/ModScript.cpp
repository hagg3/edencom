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

#include "lua/lua.h"
#include "lua/lauxlib.h"
#include "lua/lualib.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <new>

namespace ModScript {

bool testNoHook = false;
bool testNoSandbox = false;

namespace {

const char* const kEventNames[] = {"load", "tick", "build", "mine", "paint", "burn", "weather", NULL};
const char* const kEditMeta = "emod.edit";

struct Co {
    lua_State* T = NULL;
    int ref = LUA_NOREF;
    double wait = 0;
    int preempts = 0;
    int ev = -1;                          // the event that started it (-1: a chunk, the console, a command)
    int handler = LUA_NOREF;              // the handler's registry ref (tick: one live call per handler)
    bool started = false;
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
    std::string lastError;
    int printsFrame = 0;
    float toastQuiet = 0;
    CmdScript::Sink* sink = NULL;         // the running coroutine's sink
    std::vector<int> handlers[EV_COUNT];
    std::vector<Co> cos;
    std::vector<Cmd> cmds;
};
struct QEvent { int ev, x, y, z, a, b; };

Limits g_lim;
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
void* luaAlloc(void* ud, void* ptr, size_t osize, size_t nsize) {
    Pack* p = (Pack*)ud;
    const size_t old = ptr ? osize : 0;   // ptr NULL: osize is the object's type, not a size
    if (nsize == 0) {
        if (ptr) { p->mem -= old; std::free(ptr); }
        return NULL;
    }
    if (nsize > old && p->mem - old + nsize > g_lim.memBytes) { p->memRefusals++; return NULL; }   // a shrink never fails
    void* n = std::realloc(ptr, nsize);
    if (!n) return NULL;
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
    if (p->closing) luaL_error(L, "the world closed");   // a __gc at lua_close runs no script code
    if (testNoHook) return;
    if (overBudget(p) && L == p->running && lua_isyieldable(L)) {
        p->preempted = true;
        lua_yield(L, 0);   // a hook yields with no values; the scheduler resumes it next frame
        return;
    }
    // Code that can't pause (a callback inside C, a __gc, a coroutine the script made): bounded instead.
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
        scriptError(p, what + ": not started, " + std::to_string((int)MAX_COROUTINES) + " handlers are still waiting", sink);
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
const char* const kWeatherKinds[] = {"clear", "overcast", "rain", "storm", "snow", NULL};
int apiWeather(lua_State* L) {
    if (!worldOpen()) return luaL_error(L, "no world is open");
    if (!lua_isnoneornil(L, 1)) {
        const int k = luaL_checkoption(L, 1, NULL, kWeatherKinds);
        const double secs = luaL_optnumber(L, 2, 0);
        if (!Weather::active()) return luaL_error(L, "weather is off (Settings > Gameplay > Weather)");
        if (secs < 0 || secs > 1e7) return luaL_argerror(L, 2, "seconds from 0 to 10000000");
        Weather::set(k == 4 ? Weather::RAIN : (Weather::Kind)k, (float)secs, k == 4);
    }
    lua_pushstring(L, Weather::forceSnow() ? "snow" : kWeatherKinds[Weather::kind()]);
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
    for (const std::unique_ptr<Pack>& o : g_packs)
        for (const Cmd& c : o->cmds)
            if (c.name == name) return luaL_error(L, "the mod command \"%s\" is taken (pack %s)", name, o->id.c_str());
    if (p->cmds.size() >= MAX_HANDLERS) return luaL_error(L, "at most %d commands a pack", (int)MAX_HANDLERS);
    lua_pushvalue(L, 3);
    const int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    p->cmds.push_back(Cmd{name, help, ref});
    return 0;
}

const luaL_Reg kApi[] = {
    {"cmd", apiCmd}, {"set", apiSet}, {"fill", apiFill}, {"paint", apiPaint}, {"explode", apiExplode},
    {"fire", apiFire}, {"tp", apiTp}, {"run", apiRun}, {"spawn", apiSpawn}, {"say", apiSay},
    {"time", apiTime}, {"timefreeze", apiTimefreeze}, {"heal", apiHeal}, {"hurt", apiHurt}, {"gold", apiGold},
    {"sound", apiSound}, {"flash", apiFlash}, {"get", apiGet}, {"blockid", apiBlockId}, {"player", apiPlayer},
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
            runNew(p);   // its top level runs now, in list order, up to its first eden.wait
        }
        if (!p->stopped) dispatch(p, EV_LOAD, nullptr);
    }
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
    g_pending = g_started = false;
    g_clock = 0;
}

void restart() {
    if (g_depth > 0) return;
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
    QEvent q{ev, x, y, z, 0, 0};
    switch (ev) {
    case EV_BUILD: if (after == before || after <= 0) return; q.a = after; break;
    case EV_MINE: if (after == before || before <= 0) return; q.a = before; break;
    case EV_PAINT: if (before <= 0) return; q.a = t->getColor(x, z, y); q.b = before; break;
    case EV_BURN: if (before <= 0 || !(blockinfo[before] & IS_FLAMMABLE)) return; q.a = before; break;
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
    g_queue.push_back(QEvent{EV_WEATHER, 0, 0, 0, kind, previous});
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
    // 2. the player's edits since the last frame
    std::vector<QEvent> q;
    q.swap(g_queue);
    for (const QEvent& e : q) {
        g_stats.events++;
        for (std::unique_ptr<Pack>& p : g_packs) {
            if (p->stopped || p->handlers[e.ev].empty()) continue;
            dispatch(p.get(), e.ev, [&](lua_State* L) {
                if (e.ev == EV_WEATHER) {   // AT.1c: (kind, previous); snow is a where, not a kind
                    lua_pushstring(L, kWeatherKinds[e.a]);
                    lua_pushstring(L, kWeatherKinds[e.b]);
                    return 2;
                }
                lua_pushinteger(L, e.x);
                lua_pushinteger(L, e.y);
                lua_pushinteger(L, e.z);
                if (e.ev == EV_PAINT) { lua_pushinteger(L, e.a); pushBlock(L, e.b); return 5; }
                pushBlock(L, e.a);
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
        i.memRefusals = p->memRefusals;
        i.stopped = p->stopped;
        i.why = p->stopped ? p->why : p->lastError;
        v.push_back(i);
    }
    return v;
}

std::vector<std::string> commands() {
    std::vector<std::string> v;
    for (const std::unique_ptr<Pack>& p : g_packs)
        if (!p->stopped)
            for (const Cmd& c : p->cmds) v.push_back(c.name + (c.help.empty() ? "" : " - " + c.help) + " (" + p->id + ")");
    return v;
}

bool runCommand(const std::string& name, const std::vector<std::string>& args, CmdScript::Sink* sink) {
    for (std::unique_ptr<Pack>& up : g_packs) {
        Pack* p = up.get();
        if (p->stopped) continue;
        for (const Cmd& c : p->cmds) {
            if (c.name != name) continue;
            if (spawn(p, c.fn, [&](lua_State* L) {
                    for (const std::string& a : args) lua_pushstring(L, a.c_str());
                    return (int)args.size();
                }, -1, "mod " + name, sink, false))
                resumeCo(p, p->cos.size() - 1);
            sweep();
            return true;
        }
    }
    return false;
}

Stats stats() { return g_stats; }

const char* describe() {
    static char buf[320];
    int packs = 0, cos = 0;
    for (const std::unique_ptr<Pack>& p : g_packs) { packs += !p->console; cos += liveCos(p.get()); }
    std::snprintf(buf, sizeof buf,
                  "{\"packs\":%d,\"coroutines\":%d,\"started\":%d,\"finished\":%d,\"errors\":%d,\"preempted\":%d,"
                  "\"stopped\":%d,\"events\":%d,\"eventsDropped\":%d,\"edits\":%d}",
                  packs, cos, g_stats.started, g_stats.finished, g_stats.errors, g_stats.preempted, g_stats.stopped,
                  g_stats.events, g_stats.eventsDropped, g_stats.edits);
    return buf;
}

}  // namespace ModScript
