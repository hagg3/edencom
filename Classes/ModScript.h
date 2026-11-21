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
#include <cstdint>
#include <string>
#include <vector>

namespace ModMenu { struct Row; }   // Classes/ModMenuScreen.h

namespace ModScript {

enum { HOOK_EVERY = 1000, MAX_COROUTINES = 64, MAX_EVENTS = 256, MAX_HANDLERS = 32, MAX_PRINTS = 50,
       PATTERN_MAX = 64 * 1024, API_VERSION = 2 };

size_t defaultAllMemBytes();              // K.8-5: 24 MiB when eden_device_class() is mobile_low (N.6-2b), else 64 MiB
struct Limits {
    size_t memBytes = 8u << 20;          // per pack state
    size_t allMemBytes = defaultAllMemBytes();   // every state together (the console's too): 24 MiB on a low-memory device, else 64
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
// K.6-2 (V7): the world is really closing (World::requestExit, a clean app quit), BEFORE the exit save. Every `leave`
// handler runs once to completion now: no pause (eden.wait / a yield raise "can't pause while the world closes"; the
// count hook never preempts, so hardPerResume bounds it), world edits get one fresh WORK_PER_FRAME and anything past
// it is refused with a `why`. A warp does not call it. Main thread, never from inside Lua.
void leaving();

// ---- events from the local player's edits (MpWorld::intent*, terrain order x, z, y like the call they wrap)
enum Event { EV_LOAD = 0, EV_TICK, EV_BUILD, EV_MINE, EV_PAINT, EV_BURN, EV_WEATHER, EV_USE, EV_SIGN, EV_LEAVE, EV_SETTING, EV_MENU, EV_COUNT };
// `before` = the cell's type before the edit. Queues the event when it changed something.
void noteEdit(int ev, int x, int z, int y, int before);
void noteUse(int x, int z, int y, int type);   // K.6-1b: `use` with how="tap" (a build tap on a usable block)
// AT.1c: Weather::update calls this when the spell's kind changed (Weather::Kind values); queues the `weather` event.
void noteWeather(int kind, int previous);
// K.6-3: SignTool::answer calls this when the player placed or edited a sign (engine order x, z, y; `face` the
// record's a); queues the `sign` event.
void noteSign(int x, int z, int y, int face, const char* text);

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
    int skipped;                          // ticks skipped / events dropped at the 64-coroutine cap (not errors)
    bool stopped;
    std::string why;                      // stopped: the reason; else the last error ("" none)
};
std::vector<Info> info();
// K.8-8: the LuaLS stub file (docs/eden-api.lua) generated from the API table's one-line docs; the names that have a
// kApi entry but no doc line (or the reverse), "" when complete; a syntax check of one chunk in a throwaway state.
std::string apiStubs();
std::string apiDocsMissing();
bool syntaxCheck(const std::string& source, const std::string& chunkName, std::string* err);
// K.8-6: the Mods screen's view of scripts: every pack's state while a world is open, else the last world's report as it
// was when that world closed (kept across reset(); the console's own state is not in it). Empty before any world ran scripts.
std::vector<Info> lastReport();
size_t totalMem();                        // bytes held by every state together (Limits::allMemBytes caps it)
void restart();                           // `scripts reload`: every pack state again (same sources)
// `mod` (no name): the commands; `mod NAME ARGS...`: runs one; `mod PACK:NAME` always runs that pack's, the short NAME
// the first pack that registered it (K.8-4). False: no such command.
std::vector<std::string> commands();      // "name - help (pack)"; a name another pack holds first reads "pack:name"
std::vector<std::string> commandNames();  // every `name` and `pack:name` (the console's completion)
bool runCommand(const std::string& name, const std::vector<std::string>& args, CmdScript::Sink* sink);

// ---- K.7-3a: eden.menu (V12/V13; docs/scripting.md "Menus"). ModMenuScreen calls these from its update (World::update's
// input slot); they only record what happened, and the menu's on_action / on_close run in step 2 of this frame's update().
void menuInput(int serial, int tab, int row, const ModMenu::Row& now);   // a value changed, or a row / button was tapped
void menuClosed(int serial);                                             // the player closed it (Back, Escape, the menu key)
size_t menuRecords();                                                    // menus held by every pack (tests)

// ---- K.7-3b (U2): the pause menu's Mods page. Offered only when a loaded pack declares settings or handles "menu".
// The page is a ModMenuScreen (serial MODS_PAGE) with one tab per such pack (at most 8): its "Open menu" button
// (when the pack has an eden.on("menu") handler) and its pack.json settings. A change is saved at once
// (ContentPack::saveSetting) and queues the pack's `setting` event (id, value, old); eden.setting reads the new value.
enum { MODS_PAGE = -1 };
bool modsPageAvailable();                                                // the pause menu shows its Mods button
bool openModsPage();                                                     // false: no world, a mod menu is open, or nothing to show
void modsPageInput(int tab, int row, const ModMenu::Row& now);           // ModMenuScreen: a row of that page changed / was tapped

// ---- K.6-5: eden.store, the world's per-pack saved data (`.emod` record 13 MOD_DATA; docs/scripting.md "Saved data").
// Held here, not in a Lua state, so it outlives `scripts reload` and a warp; reset() drops it with the world.
void storeLoad(const uint8_t* rec, size_t n); // FileManager::finishLoadOnMain: the world's record (NULL: none); a record
                                              // that breaks the format's rules is ignored whole (one console line)
bool storeNeedsSave();                        // a pack stored something since the load / last save (never in a session)
void storeEncode(std::vector<uint8_t>& out);  // the record: a pack that is not loaded, or only read, goes out verbatim
void storeSaved();                            // the batch holding storeEncode's bytes committed
size_t storeEntries();                        // packs with an entry (tests)

// ---- tests
struct Stats { int started, finished, errors, preempted, stopped, events, eventsDropped, edits, skipped, leaves; };
Stats stats();
const char* describe();                   // one JSON line
extern bool testNoHook;                   // mutation: the count hook never pauses or stops anything
extern bool testLoadEarly;                // mutation: `load` is dispatched at start, before the top levels finish (K.8-1)
extern bool testUseFilter;               // mutation: `use` keeps `burn`'s flammable test (K.6-1a)
extern bool testStoreNotDirty;            // mutation: eden.store.set never marks the store dirty (K.6-5)
extern bool testLeaveAfterSave;           // mutation: World::requestExit saves before `leave` runs (K.6-2)
extern bool testDialogNoQueue;             // mutation: mod dialogs are not queued, a second replaces the first (K.7-1b)
extern bool testToastFlood;              // mutation: eden.toast has no 1 s rate limit (K.7-1a)
extern bool testNoGen;                   // mutation: a creature handle ignores its generation (K.6-4a)
extern bool testLookBuild;               // mutation: eden.look returns the build point, not the hit cell (K.6-7)
extern bool testNoReport;                // mutation: the Mods screen gets no script report (K.8-6)
extern bool testNonFinite;               // mutation: h:setvel takes a NaN / inf (K.6-4b)
extern bool testCapError;                 // mutation: the coroutine cap is an error that counts toward maxErrors (K.8-2)
extern bool testNoSandbox;                // mutation: io/os-like globals are left in (the escape checks must fail)

}  // namespace ModScript

#endif
