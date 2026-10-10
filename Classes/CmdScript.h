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
//    * hurt does nothing with Health off; gold caps the purse at 999; sound N is 0..72.
//    * a statement that does not parse becomes a no-op that shows its message (e.g. "Unknown command:
//      foo") when it runs; the rest of the script still runs.
//    * at most WORK_PER_FRAME statements + filled cells a frame; past that every instance pauses to
//      the next frame where it stands.
//  Runtime state is never saved: a world that closes (or warps) stops every script.
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
           MAX_GOLD = 999, WORK_PER_FRAME = 20000 };
    enum Trigger { TRIG_TOUCH = 0, TRIG_STEP, TRIG_NEAR, TRIG_TIMER };
    enum Op { OP_BAD = 0, OP_SAY, OP_TP, OP_SET, OP_FILL, OP_PAINT, OP_EXPLODE, OP_FIRE, OP_SPAWN,
              OP_TIME, OP_HEAL, OP_HURT, OP_GOLD, OP_SOUND, OP_FLASH, OP_WAIT, OP_RUN, OP_IF,
              OP_UNLESS };
    struct Stmt {
        int op;
        int v[6];          // coordinates in script order (X, height, Z [, X2, height2, Z2])
        int type;          // set/fill/if/unless: a block id; spawn: a creature
        int color;         // set/fill/paint: the colour byte, -1 = none given
        bool hasPos;       // spawn: a spot was given
        float f;           // wait/time/hurt/gold/sound
        std::string text;  // say: the text; OP_BAD: the message shown when it runs
    };
    struct Program {
        int trigger = TRIG_TOUCH;
        float triggerArg = 0;
        std::vector<Stmt> stmts;   // the header is not a statement
        int errors = 0;            // statements that are OP_BAD
        std::string firstError;
    };
    static void parse(const char* src, Program* out);
    static int blockByName(const char* name);      // a name or a number 0..127; -1 if neither
    static int creatureByName(const char* name);   // moof..stalker or 0..6; -1 if neither

    // ---- runtime (main thread, in play)
    static void update(float etime);     // World::update, once a frame: resume waits, fire triggers
    // A build tap at (mx, my) with a block (not a tool) armed: if it lands on a command block, press
    // it and return true -- the tap is used up and builds nothing.
    static bool tap(int mx, int my);
    // @touch on this block (file-order anchor). The harness's entry point too.
    static int press(const TrailerPos& block);
    enum { PRESS_NONE = 0, PRESS_STARTED, PRESS_BUSY, PRESS_REFUSED };
    static void reset();                 // a world closed or warped: every instance stops
    static int liveCount();
    static bool busy(const TrailerPos& block);   // has a live instance

    struct Stats {
        int live, started, finished, statements, cells, unknown, refusedDepth, refusedFull,
            refusedCells, triggers, presses, suspended, maxDepth;
    };
    static Stats stats();
    static const char* describe();       // one JSON line, for probes
    static bool trace;                   // print every statement as it runs (harness)
    // Mutation hook for --cmds-selftest: `run` stops checking the depth cap.
    static bool testNoDepthCap;
};

#endif
