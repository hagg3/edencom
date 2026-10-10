//
//  SignTool.h
//  Eden — Stage D / D.3c: the picker's SIGN tool (place, edit, remove signs) and the anchor rule.
//
//  A build tap with the SIGN tool raycasts twice: FC_DESTROY gives the anchor (the block hit) and
//  FC_PLACE the empty cell in front of it; their difference is the face (WorldTrailer.h's `a`).
//    * the bottom face is refused with the toast "Can't place a sign under the block" (the 2026
//      game's behaviour); only an air anchor is otherwise refused (glass, leaves, water, doors all
//      take a sign in the game -- WORKING/stage-d-plan-2026-10-10.md §4 D.0);
//    * no sign on that (anchor, face) -> a prompt for the text, then a new record:
//        a = the face, b = the build paint (hud->block_paintcolor, a colorTable index) or 45
//        (charcoal) with none -- Emod's default, the game writes 2 -- and c = the face's own
//        direction for a wall sign, the quarter turn facing the player for a standing one;
//    * a sign already there -> the same prompt, prefilled: an edit. An EMPTY edit deletes it. An
//      edit keeps the board's colour unless a build paint is selected.
//  The prompt is GLDialog::prompt (printable ASCII, 95 bytes). On a host with no GL text input
//  (web, until ROADMAP 5.9) the tool is veiled in the picker instead: Hud::pickerToolEnabled.
//
//  The anchor rule (also D.4a's): Terrain::updateChunks -- the choke point every live edit goes
//  through -- calls anchorBecameAir() when it writes air, which removes every sign AND the CMB1
//  record on that block (mining, TNT, fire, liquids receding). Never on load or column streaming.
//
//  D.4b adds CmdTool below: the picker's CMD tool. A build tap on a command block (a block with a
//  CMB1 record) edits its script; on anything else it places one -- TYPE_STEEL in the cell in front
//  of the face, plus a record with an empty script.
//
//  Both tools ignore a tap while a GLDialog is up: the prompt is modal, and a tap that arrives
//  under it (a held build pulse, say) must not re-open it.
//
#ifndef Eden_SignTool_h
#define Eden_SignTool_h

#include "WorldTrailer.h"

class SignTool {
public:
    // A build tap at screen point (mx, my) with the SIGN tool armed. `yaw` is the player's.
    static void tap(int mx, int my, float yaw);
    // The same, from an already-raycast anchor (Terrain argument order x, z, y) and face delta --
    // the harness's entry point, which has no screen to tap. Returns what it did (enum below).
    static int use(int x, int z, int y, int dx, int dyUp, int dz, float yaw);
    enum { USE_NONE = 0, USE_PROMPT_NEW, USE_PROMPT_EDIT, USE_REFUSED_BOTTOM, USE_REFUSED_AIR,
           USE_REFUSED_OPAQUE };
    // The prompt's answer. GLDialog calls it; the harness calls it too, to answer the prompt.
    static void answer(int chosen, const char* text);
    // Terrain::updateChunks wrote air at (x, z, y): drop whatever hangs on that block.
    static void anchorBecameAir(int x, int z, int y);
    // The facing quarter turn (WorldTrailer.h's c) a standing sign gets when placed at `yaw`:
    // its text faces the player.
    static int facingForYaw(float yaw);
    // Mutation hooks for --signs-selftest (each must make its gate fail): the bottom face accepted,
    // and the anchor rule skipped.
    static bool testAllowBottom, testNoAnchorHook;
};

class CmdTool {
public:
    // cmd.html's limits: a script is at most 511 bytes (the record's 512 hold a NUL), and a world
    // holds at most 512 command blocks.
    enum { MAX_PER_WORLD = 512, MAX_SCRIPT = 511, PROMPT_LINES = 6 };
    // A build tap at screen point (mx, my) with the CMD tool armed.
    static void tap(int mx, int my);
    // The same from an already-raycast hit: the block hit (Terrain argument order x, z, y) and the
    // face's delta toward the cell in front. The harness's entry point. Returns what it did.
    static int use(int x, int z, int y, int dx, int dyUp, int dz);
    enum { USE_NONE = 0, USE_PLACED, USE_PROMPT_EDIT, USE_REFUSED_CELL, USE_REFUSED_PLAYER,
           USE_REFUSED_FULL, USE_REFUSED_OPAQUE };
    // The edit prompt's answer: OK writes the script (printable ASCII; EMPTY keeps the block and
    // clears its script, as cmd.html says), Cancel leaves it.
    static void answer(int chosen, const char* text);
    // Runtime-only, never saved: which command blocks are running their script, for the skin's
    // green button. CmdScript (D.4c) sets it every frame. A removed block is dropped.
    static bool running(const TrailerPos& block);
    static void setRunning(const TrailerPos& block, bool on);
    static void clearRunning();
};

#endif
