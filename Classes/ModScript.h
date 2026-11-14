//
//  ModScript.h
//  Emod -- K.4: sandboxed Lua mod scripts (WORKING/mod-scripting-plan-2026-11-03.md; docs/scripting.md).
//
//  A mod pack lists Lua 5.4 sources in its pack.json (`"scripts"`, ContentPack::compileMod reads them whole).
//  Each loaded pack with scripts gets its OWN Lua state (globals, memory and errors never mix), created on the
//  first frame of a world and closed when the world closes or the next one loads. A multiplayer session loads
//  no packs, so no scripts (M8).
//
//  The sandbox (S4): base (no dofile/loadfile; load = text chunks only; collectgarbage count/collect/step;
//  setmetatable refuses __gc; print -> the console), coroutine, string (no dump; the pattern functions refuse a
//  subject over 64 KiB), table, math, utf8. C code can't be preempted, so the vendored Lua bounds its loops a script
//  sizes (QA.3: pattern steps, string.rep, table.move/insert/remove). io, os, package and debug are not compiled at all
//  (port/src/shim/vendor/lua). A script never opens a file: its sources were read at world entry.
//
//  The API is the command-block language (S8): every world edit (`eden.set`, `eden.fill`, `eden.cmd(line)`,
//  ...) is parsed by CmdScript::parseLine and run by CmdScript::runNow, so it has the same refusals, caps and
//  WORK_PER_FRAME budget as a command block; when the frame's work is spent, the call pauses the script to the
//  next frame. Coordinates are the engine's (x, height, z), absolute, like the console's `=N`.
//
//  Budgets (S5, S6): an allocator cap per state; a count hook every HOOK_EVERY instructions. Every handler
//  runs as a coroutine: past the per-pack or all-packs instruction budget of a frame it is PAUSED to the next
//  frame (preempted); preempted `maxPreempts` frames in a row without an eden.wait it is stopped with an
//  error. Code that can't yield (a table.sort comparator, a coroutine the script made) gets an error
//  past `hardPerResume` instructions instead. An error is reported (console, stderr, a rate-limited toast);
//  `maxErrors` stops the pack.
//
//  Events (S10) are queued where the local player edits (MpWorld::intent*, outside a session) and delivered in
//  World::update right after CmdScript::update (edit before mesh). Main thread only.
//
#ifndef Eden_ModScript_h
#define Eden_ModScript_h

#include "CmdScript.h"
#include <string>
#include <vector>

namespace ModScript {

enum { HOOK_EVERY = 1000, MAX_COROUTINES = 64, MAX_EVENTS = 256, MAX_HANDLERS = 32, MAX_PRINTS = 50,
       PATTERN_MAX = 64 * 1024, API_VERSION = 1 };

struct Limits {
    size_t memBytes = 8u << 20;          // per pack state
    long packPerFrame = 100000;          // instructions per pack per frame
    long allPerFrame = 250000;           // instructions per frame, every pack together
    long hardPerResume = 5000000;        // code that can't yield: an error past this in one resume
    int maxPreempts = 600;               // frames in a row paused by the budget before a handler is stopped
    int maxErrors = 100;                 // errors before a pack's scripts are stopped
};
Limits& limits();                         // the selftest lowers them; restoreLimits() puts the defaults back
void restoreLimits();

// ---- life cycle (World / Terrain)
void worldEntered(bool session);          // World::loadWorld, after ContentPack::loadMods: drop every state;
                                          // the packs' scripts start on the first update() (not in a session)
void update(float etime);                 // World::update, after CmdScript::update
void reset();                             // the world closed (Terrain::unloadTerrain(true))

// ---- events from the local player's edits (MpWorld::intent*, terrain order x, z, y like the call they wrap)
enum Event { EV_LOAD = 0, EV_TICK, EV_BUILD, EV_MINE, EV_PAINT, EV_BURN, EV_WEATHER, EV_COUNT };
// `before` = the cell's type before the edit. Queues the event when it changed something.
void noteEdit(int ev, int x, int z, int y, int before);
// AT.1c: Weather::update calls this when the spell's kind changed (Weather::Kind values); queues the `weather` event.
void noteWeather(int kind, int previous);

// ---- the console (Classes/Console.cpp: `lua`, `scripts`, `mod`)
// Runs a chunk in the console's own state (created on first use, closed with the world): an expression's
// values are printed. Output goes to `sink`. False: no world / a multiplayer session / a syntax error.
bool runConsole(const std::string& code, CmdScript::Sink* sink);
struct Info {
    std::string pack;                     // pack id ("console" for the console's state)
    size_t mem, memPeak;
    long instrLastFrame;
    long long instrTotal;
    int handlers, coroutines, errors, preempts, memRefusals;
    bool stopped;
    std::string why;                      // stopped: the reason; else the last error ("" none)
};
std::vector<Info> info();
void restart();                           // `scripts reload`: every pack state again (same sources)
// `mod` (no name): the commands; `mod NAME ARGS...`: runs one. False: no such command.
std::vector<std::string> commands();      // "name - help (pack)"
bool runCommand(const std::string& name, const std::vector<std::string>& args, CmdScript::Sink* sink);

// ---- tests
struct Stats { int started, finished, errors, preempted, stopped, events, eventsDropped, edits; };
Stats stats();
const char* describe();                   // one JSON line
extern bool testNoHook;                   // mutation: the count hook never pauses or stops anything
extern bool testNoSandbox;                // mutation: io/os-like globals are left in (the escape checks must fail)

}  // namespace ModScript

#endif
