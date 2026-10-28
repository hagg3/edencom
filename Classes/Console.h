//
//  Console.h
//  Eden -- Stage C / C.2: the console's registry (WORKING/dev-console-plan.md §3, §5; docs/console.md).
//
//  A console line is a command-block script (CmdScript::runLine). This file adds what the block
//  language has no place for: VERBS a feature registers (`help`, `find`, `where`...) and CVARS, dotted
//  `group.key` variables (`video.fov`, `keys.jump`, `world.name`, `gen.caves`). Both reach the parser
//  through ONE CmdScript::Ext (Console::ext()); a built-in block verb always wins, so a registry name
//  can never change what a saved world's command block means (verbs have no '.', cvars always do).
//
//  Every kSettings[] row and every keybind registers itself from its table (init()), so a new Settings
//  row is a cvar with no console code. A feature registers from its own init path with a plain call to
//  addCommand()/addCvar() -- not from a static initialiser. Main thread only.
//
#ifndef Eden_Console_h
#define Eden_Console_h

#include "CmdScript.h"
#include <string>
#include <vector>

namespace Console {

// ---- registration -------------------------------------------------------------------------------
enum { CF_DIAG = 1,      // diagnostics builds only (absent, not refused, otherwise)
       CF_CHEAT = 2,     // refused unless Settings > Experiments > Allow cheats (no verb uses it in v1)
       CF_WORLD = 4,     // needs a world in play
       CF_EDITS = 8 };   // changes the world (a future multiplayer client refuses these in one place)

// More argument kinds than the block verbs have (CmdScript::ARG_* are 1..6).
enum { ARG_CVAR = 10, ARG_VALUE, ARG_KEY, ARG_VERB };

struct Ctx {                                  // what a command sees
    int ax, ay, az;                           // the anchor, engine order
    const std::vector<std::string>& args;     // the words after the verb
    CmdScript::Sink* sink;                    // NULL: no output wanted
    int source;                               // CmdScript::SRC_*
    int job;                                  // the console job, 0 = none
    void print(const char* fmt, ...);         // OUT line to the sink
    void error(const char* fmt, ...);         // ERR line to the sink
};
typedef void (*CommandFn)(Ctx&);
void addCommand(const char* name, const char* usage, const char* help, int flags,
                std::initializer_list<int> argKinds, CommandFn fn);

enum { KIND_TOGGLE = 0, KIND_RANGE = 1, KIND_ENUM = 2, KIND_TEXT = 4 };   // the first three are Settings'
enum { APPLY_LIVE = 0, APPLY_NEXT_WORLD, APPLY_NEXT_CONVERT, APPLY_READONLY };
struct Cvar {
    const char* name;        // "group.key": lower case, exactly one '.'-separated group
    const char* label;
    const char* help;        // may be NULL
    int kind;
    float min, max, step, def;
    const char* options;     // KIND_ENUM: comma-separated labels
    int apply;               // APPLY_*
    int flags;               // CF_*
    int tag;                 // handed back to the callbacks (the Settings row / keybind index)
    float (*get)(int tag);                   // numeric kinds
    void (*set)(int tag, float v);
    const char* (*getText)(int tag);         // KIND_TEXT (valid until the next call)
    bool (*setText)(int tag, const char* v);
    void (*reset)(int tag);                  // optional: back to the default (`reset`); NULL = not resettable
};
void addCvar(const Cvar&);

// ---- life cycle ---------------------------------------------------------------------------------
void init();                                  // idempotent: the verbs, the Settings/keybind/world bridges
void reset();                                 // a world closed: back to the default anchor (history stays)
const CmdScript::Ext* ext();                  // what ParseOptions::ext should be
extern bool testNoBridge;                     // --console-mutate=noreg: init() skips the Settings bridge

// ---- running a line -----------------------------------------------------------------------------
// The console's front door: records history, resolves the anchor, runs through CmdScript::runLine with
// the console's own sink (every line lands in scrollback() and in the mirror, below). Returns
// CmdScript::runLine's result. The echo of the input line is the SCREEN's job, not run()'s.
int run(const char* line);
// Every line printed (output of a job too, whenever it arrives) is also handed to this hook: the
// harness mirrors it to stdout. NULL = none.
void setMirror(void (*hook)(int kind, const char* text));

// ---- scrollback (what the screen draws; C.3) ------------------------------------------------------
struct Line { int kind; std::string text; };   // kind: CmdScript::Sink::OUT / ERR / JOB, or ECHO
enum { ECHO = 10 };
enum { MAX_SCROLLBACK = 200 };
const std::vector<Line>& scrollback();
// Lines ever printed, never reset (not by clear() either): scrollback()[i] is line number
// scrollbackSerial() - scrollback().size() + i, so a screen can cache one label per line.
unsigned scrollbackSerial();
void print(int kind, const std::string& text);   // append (also what the console sink does)
void clear();

// ---- history: the last 50 submitted lines, persisted ------------------------------------------------
enum { MAX_HISTORY = 50 };
size_t historyCount();
const std::string& historyGet(size_t i);        // 0 = oldest
void historyAdd(const std::string& line);       // skips an empty line and a repeat of the last
void historySave();                             // eden_prefs "console.history", 0x1F-joined
void historyLoad();
void historyClear();

// ---- completion (Tab) -------------------------------------------------------------------------------
// Completes the word at the END of `line` from what the statement's position allows. One match: the
// completed line. Several: `matches` lists them and the line is extended by their common prefix.
struct Completion { std::string line; std::vector<std::string> matches; };
Completion complete(const std::string& line);

// ---- anchor -----------------------------------------------------------------------------------------
bool anchorIsFixed();
void currentAnchor(int* x, int* y, int* z);      // fixed cell, else CmdScript::playerAnchor()
void setAnchorMe();
void setAnchorFixed(int x, int y, int z);
bool lookCell(int* x, int* y, int* z);           // the cell under the crosshair; false with nothing in front

// The display name stored in a world file's header ("" if unreadable): what a reload would show.
std::string storedWorldName(const std::string& file);

// ---- cvar access (tests, the Variables tab) ---------------------------------------------------------
size_t cvarCount();
const Cvar* cvarAt(size_t i);
int cvarIndex(const char* name);                 // case-insensitive; -1 if none
std::string cvarValue(int i);                    // the value as `video.fov 95` would print it
// Parse `text` for cvar i and set it. Returns "" on success, else the error. `note` (may be NULL)
// receives "applies when a world opens" style remarks.
std::string cvarSet(int i, const char* text, std::string* note);

// ---- verbs ---------------------------------------------------------------------------------------------
size_t commandCount();
struct CommandInfo { const char* name; const char* usage; const char* help; int flags; };
CommandInfo commandAt(size_t i);

// C.10: the generated tables of docs/console-reference.md, as markdown: "block-verbs", "console-verbs"
// or "variables" (one table per group). port/tools/console-reference.py splices them in between the
// <!-- BEGIN generated:NAME --> markers; --console-selftest checks every name is in the file.
std::string referenceMarkdown(const char* section);

}  // namespace Console

#endif
