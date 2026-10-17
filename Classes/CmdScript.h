//
//  CmdScript.h
//  Eden — Stage D / D.4c: the command-block interpreter (the language of
//  https://db.edengame.net/cmd.html; a copy is in TESTERS/d4c/cmd.txt).
//
//  A script is one line of statements separated by ';'. An optional trigger header comes first:
//  @touch (the default: a build tap on any face of the block), @step (a player stands on it),
//  @near N (a player comes within N blocks; re-arms when they leave), @timer N (every N seconds,
//  only while a player is within 48 blocks). Coordinates are X HEIGHT Z, relative to the block that
//  owns the script -- so a script's (a, b, c) is the ENGINE's (x + a, y_up + b, z + c), i.e. Terrain
//  arguments (x + a, z + c, y + b). A leading '~' is accepted and means nothing.
//
//  What Emod decided where cmd.html is silent (WORKING/stage-d-plan-2026-10-10.md §4 D.4c; the 2026
//  game's own interpreter never ran a statement for us, D.0):
//    * `run` executes the target's script at once, inline, up to its first `wait`; the caller then
//      carries on. A chain is at most 8 runs deep (a trigger or a tap starts at depth 0); deeper is
//      refused with a toast. A `run` always starts a new instance, even of a block that is running.
//    * a trigger or a tap on a block that still has a live instance does nothing (no re-entry).
//    * a tap (or `run`) runs a script whatever its header: a @step block's button still works.
//    * at most 32 instances are alive at once; a 33rd is refused with a toast.
//    * `wait S` (0..600 s) always yields at least one frame, so `wait .01` = "next frame".
//    * `if`/`unless` skip the next statement when false; a skipped `if` takes its own gated
//      statement with it, so `if A; if B; X` is "A and B".
//    * COLOR is the stored colour byte, 0 (unpainted) .. 54 -- not cmd.html's "0-53": the adder world
//      writes `lamp 54` and its saved lamps hold 54.
//    * set/fill skip bedrock, a door or portal half, a cell inside the player and any non-resident
//      cell; overwriting a command block with a solid block is refused (air removes it, like mining).
//      They never place a door, a portal or TYPE_CUSTOM.
//    * Eden has no clock, only a sky colour per region: day/noon (time 0.25..0.75) put the region's
//      day sky back, night/midnight (the rest) paint it night (palette 54) -- what painting the sky
//      does.
//    * hurt does nothing with Health off; gold caps the purse at 999; sound N is 0..NUM_SOUNDS-1
//      (0..78 since 6ef81de added sounds; 0..72 when D.4c shipped).
//    * a statement that does not parse becomes a no-op that shows its message (e.g. "Unknown command:
//      foo") when it runs; the rest of the script still runs.
//    * at most WORK_PER_FRAME statements + filled cells a frame; past that every instance pauses to
//      the next frame where it stands.
//  Runtime state is never saved: a world that closes (or warps) stops every script.
//
//  C.1 (WORKING/dev-console-plan.md §2, docs/console.md): the same parser and interpreter run a typed
//  console line. parse() is the block path and is unchanged; parseLine() takes ParseOptions:
//    * triggers off: any '@' statement is "Triggers only work in a command block".
//    * absolute on: a coordinate written `=N` is the engine coordinate N (Stmt::abs); in a block
//      `=N` stays "Not a coordinate".
//    * ext: verbs the console registers (Console::, C.2) parse to OP_EXT with their words; a built-in
//      verb always wins, so a block's meaning can't change.
//  runLine() runs a line as a console JOB anchored where the caller says (the console: the cell under
//  the player's feet): a line with any bad statement runs nothing; at most MAX_JOBS jobs wait at once,
//  outside the 32 block instances; output goes to a Sink. Jobs are depth 0, so `run` chains 8 deep as
//  from a block. kVerbs (verbs()) holds every built-in verb's usage: the parse errors read it.
//
#ifndef Eden_CmdScript_h
#define Eden_CmdScript_h

#include "WorldTrailer.h"
#include <memory>
#include <string>
#include <vector>

class CmdScript {
public:
    enum { MAX_DEPTH = 8, MAX_RUNNING = 32, MAX_FILL = 4096, MAX_WAIT = 600, TIMER_RANGE = 48,
           MAX_GOLD = 999, WORK_PER_FRAME = 20000, MAX_JOBS = 4, MAX_LINE = 511 };
    enum Trigger { TRIG_TOUCH = 0, TRIG_STEP, TRIG_NEAR, TRIG_TIMER };
    enum Op { OP_BAD = 0, OP_SAY, OP_TP, OP_SET, OP_FILL, OP_PAINT, OP_EXPLODE, OP_FIRE, OP_SPAWN,
              OP_TIME, OP_HEAL, OP_HURT, OP_GOLD, OP_SOUND, OP_FLASH, OP_WAIT, OP_RUN, OP_IF,
              OP_UNLESS, OP_EXT };
    struct Stmt {
        int op = OP_BAD;
        int v[6] = {};     // coordinates in script order (X, height, Z [, X2, height2, Z2])
        bool abs[6] = {};  // console only: v[i] was written `=N`, an engine coordinate
        int type = 0;      // set/fill/if/unless: a block id; spawn: a creature
        int color = -1;    // set/fill/paint: the colour byte, -1 = none given
        bool hasPos = false;   // spawn: a spot was given
        float f = 0;       // wait/time/hurt/gold/sound
        std::string text;  // say: the text; OP_BAD: the message shown when it runs
        int ext = -1;      // OP_EXT: the registry's index for the verb
        std::vector<std::string> args;   // OP_EXT: the words after the verb
    };

    // Where a console job's output goes. Must outlive the job (CmdScript::reset/stop end jobs).
    struct Sink {
        enum { OUT = 0, ERR, JOB };
        virtual void line(int kind, const std::string& text) = 0;
        virtual ~Sink() {}
    };
    // What an OP_EXT statement sees when it runs.
    struct ExtCall {
        int ax, ay, az;                        // the anchor, engine order
        const std::vector<std::string>& args;
        Sink* sink;                            // NULL: no output wanted
        int source;                            // SRC_*
        int job;                               // the console job, 0 = none
    };
    // The console's verbs (registered by Console::, C.2). find() takes a lower-case verb and returns
    // an index or -1; run() executes one.
    struct Ext {
        int (*find)(const char* verb);
        void (*run)(int index, const ExtCall& call);
    };
    struct ParseOptions {
        bool triggers = true;      // an '@' header is allowed (blocks)
        bool absolute = false;     // `=N` coordinates (the console)
        const Ext* ext = nullptr;  // extra verbs (the console)
    };
    static ParseOptions blockOptions() { return ParseOptions(); }
    static ParseOptions consoleOptions(const Ext* ext);

    struct Program {
        int trigger = TRIG_TOUCH;
        float triggerArg = 0;
        std::vector<Stmt> stmts;   // the header is not a statement
        int errors = 0;            // statements that are OP_BAD
        std::string firstError;
        const Ext* ext = nullptr;  // what OP_EXT statements call (the parse options' ext)
    };
    static void parse(const char* src, Program* out);   // == parseLine(src, out, blockOptions())
    static void parseLine(const char* src, Program* out, const ParseOptions& opts);
    static int blockByName(const char* name);      // a name or a number 0..127; -1 if neither
    static int creatureByName(const char* name);   // moof..stalker or 0..6; -1 if neither
    // C.2 (completion): the names the two lookups above accept, in table order.
    static std::vector<std::string> blockNames();
    static std::vector<std::string> creatureNames();

    // The built-in verbs: one row each, in reference order. The parser's "X needs ..." errors are
    // name + " needs " + (needs ? needs : usage), so help and errors can't drift.
    enum { ARG_COORD = 1, ARG_BLOCK, ARG_COLOR, ARG_CREATURE, ARG_NUMBER, ARG_TEXT };
    struct Verb {
        const char* name;
        const char* usage;       // the arguments, e.g. "X Y Z TYPE [COLOR]"; "" = none
        const char* needs;       // the error's wording when it is not the usage; NULL = the usage
        const char* help;        // one line
        unsigned char kinds[9];  // ARG_* per argument, 0-terminated (optional ones included)
    };
    static const Verb* verbs(size_t* count);
    static const Verb* findVerb(const char* name);   // case-insensitive; NULL if not a verb

    // ---- runtime (main thread, in play)
    static void update(float etime);     // World::update, once a frame: resume waits, fire triggers
    // A build tap at (mx, my) with a block (not a tool) armed: if it lands on a command block, press
    // it and return true -- the tap is used up and builds nothing.
    static bool tap(int mx, int my);
    // @touch on this block (file-order anchor). The harness's entry point too.
    static int press(const TrailerPos& block);
    enum { PRESS_NONE = 0, PRESS_STARTED, PRESS_BUSY, PRESS_REFUSED };
    static void reset();                 // a world closed or warped: every instance stops
    static int liveCount();              // block instances (the 32); console jobs are not counted
    static bool busy(const TrailerPos& block);   // has a live instance

    // ---- console lines (C.1)
    enum Source { SRC_BLOCK = 0, SRC_CONSOLE, SRC_HARNESS };
    // Parse `src` with consoleOptions(ext) and, if every statement parses, run it at once as a job
    // anchored at (ax, ay, az) (engine order) up to its first wait. Errors go to `sink`, one line per
    // bad statement ("1: Unknown block: glas"), then "line not run". Returns the job id (> 0) if it
    // is still waiting, RUN_DONE if it finished, or a negative RUN_*.
    enum { RUN_DONE = 0, RUN_BAD = -1, RUN_FULL = -2, RUN_NO_WORLD = -3, RUN_EMPTY = -4, RUN_LONG = -5 };
    static int runLine(const char* src, int ax, int ay, int az, int source, Sink* sink, const Ext* ext);
    // The console's default anchor: the cell under the player's feet, engine order.
    static void playerAnchor(int* x, int* y, int* z);
    struct JobInfo { int id; float wait; size_t pc, count; std::string src; };
    static std::vector<JobInfo> jobs();  // console jobs still waiting, oldest first
    // job > 0: that job; STOP_JOBS: every console job; STOP_ALL: every job and every block instance.
    // Returns how many stopped.
    enum { STOP_JOBS = 0, STOP_ALL = -1 };
    static int stop(int job);
    // OP_TIME calls this (default: the region's sky repaint, see above). A day/night cycle replaces it,
    // and blocks and the console change together.
    static void (*timeHook)(float t);

    struct Stats {
        int live, started, finished, statements, cells, unknown, refusedDepth, refusedFull,
            refusedCells, triggers, presses, suspended, maxDepth;
        int lines, jobs;   // C.1: console lines run, console jobs waiting (not in describe())
    };
    static Stats stats();
    static const char* describe();       // one JSON line, for probes
    static bool trace;                   // print every statement as it runs (harness)
    // Mutation hook for --cmds-selftest: `run` stops checking the depth cap.
    static bool testNoDepthCap;
    // Mutation hook for --console-selftest: playerAnchor() answers the feet cell, not the one under.
    static bool testAnchorFeet;
};

#endif
