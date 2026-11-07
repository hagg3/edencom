//
//  SrclikeMove.h -- Stage SRC (WORKING/srclike-movement-plan-2026-10-31.md): the opt-in Srclike
//  movement mode (Experiments > Srclike movement). SRC.2 is the input half: the two settings, the raw
//  key axes, the usercmd, the jump edge latch (keyboard edge, wheel pulse, Auto-bhop) and the move.*
//  tunables. SRC.3 adds the swept hull trace (SrclikeTrace.cpp). SRC.4 adds the core tick and the frame
//  driver (fixed tick + interpolation) that Player::update hands the player to in the mode.
//
#ifndef SRCLIKEMOVE_H
#define SRCLIKEMOVE_H

class Player;

namespace SrclikeMove {

// What one tick samples (plan §4.2). forward/side are RAW key axes in [-1,1] (no frame-rate factor);
// vertical keys are not here: duck is its own button, and jump is the latched press.
struct UserCmd {
    float forward, side;     // +forward = along yaw, +side = right
    bool  jump;              // a fresh press this sample (edge, wheel pulse, or held with Auto-bhop)
    bool  duck;              // crouch key / HUD crouch held
    float yaw, pitch;        // degrees, as Player holds them (yaw 0 faces +x)
    bool  jumpHeld;          // SRC.6: jump key held (swim up), not just the press edge in `jump`
};

// The tunables (plan D7; Source units; the SRC.1 defaults, §4.1). Console variables `move.*`.
struct Tunables {
    float accelerate, airaccelerate, friction, stopspeed, gravity, maxvelocity, tickrate;
};

bool enabled();                   // Experiments > Srclike movement
void setEnabled(bool on);
bool autoBhop();                  // Experiments > Auto-bhop (hold jump); only meaningful while enabled()
void setAutoBhop(bool on);

Tunables& tunables();
void resetTunables();
extern const Tunables kDefaults;

// Keyboard/stick axes, raw (written by eden_set_move_input; a zero vector clears them).
void setKeyAxes(float forward, float side);

// Jump. postJumpPulse() is the mouse wheel (either direction); any number of pulses before the next
// takeJump() are ONE press. takeJump(held) is called once per tick (SRC.4): it
// returns true for a fresh press (rising edge of `held`, or `held` with Auto-bhop, or a pulse) and
// consumes the pulse either way, so a pulse made while airborne is spent, never banked for landing.
void postJumpPulse();
bool takeJump(bool held);
unsigned pressCount();            // presses takeJump has returned true for (the harness counts attempts)

// Builds the sample and remembers it (lastCmd) for the harness.
UserCmd buildCmd(bool jumpHeld, bool duckHeld, float yaw, float pitch);
const UserCmd& lastCmd();

void resetInput();                // world load / mode toggle: axes, latch, edge memory

// ---------------------------------------------------------------------------------------------------
// SRC.3: the swept hull trace (plan D5; SrclikeTrace.cpp). Everything here is in SOURCE UNITS with Z UP
// and in double (D3): S = (E.x, E.z, E.y) * kUnitsPerBlock, so Eden's (x, z, y) terrain order (rule 1)
// is Source's (x, y, z). Doubles because world coordinates reach ~112 000 u, where a float's step
// (~0.008 u) is a quarter of DIST_EPSILON. SRC.4 should keep the player's state in double too.
// The clip is port/tools/srclike-pm-ref.py's trace_hull (the SRC.1 oracle): expanded-plane brush clip,
// DIST_EPSILON back-off, "moving away" = d2 >= d1 - kParallelTol (plan §8.2.3b), touching = start-solid.
struct Vec3d { double x, y, z; };
extern const double kUnitsPerBlock;   // 72 / 1.85 = 38.9189 u
extern const double kDistEpsilon;     // 0.03125 u
extern const double kParallelTol;     // 1e-6 u

Vec3d toSource(float ex, float ey, float ez);   // Eden block coords (y up) -> Source units (z up)
void  toEden(const Vec3d& s, float* ex, float* ey, float* ez);

struct Trace {
    double fraction;      // 0..1 of start->end travelled before contact (1 = no hit)
    Vec3d  endpos;        // start + (end-start)*fraction
    Vec3d  normal;        // outward plane normal of the hit (0 if none)
    bool   startsolid;    // the start overlaps (or exactly touches) a solid brush
    bool   allsolid;      // ...and so does the end: no move possible (fraction 0)
    bool   ramp;          // the hit plane is a ramp's slope (0.7 <= nz < 0.99): the surf rule (§4.4) applies
    int    type;          // block type hit, -1 if none
    int    bx, bz, by;    // its cell, Eden (x, z, y)
};

// Swept axis-aligned hull [origin+mins, origin+maxs] from start to end against the resident terrain.
Trace traceHull(const Vec3d& start, const Vec3d& end, const Vec3d& mins, const Vec3d& maxs);

// The collision shape of a block, as the trace sees it. Mirrors Player::vertc's choices, with liquids
// non-solid (plan D5; SRC.6 owns water/lava). Walk-through as in stock: doors, flowers, portals and golden
// cubes (portal one-sidedness and pickups are SRC.8). Everything else with type > 0 is a full cube.
enum Shape { SHAPE_EMPTY = 0, SHAPE_CUBE, SHAPE_RAMP, SHAPE_SIDE };
Shape blockShape(int type);

// A block's convex brush: outward planes n.p <= d (Source units). Returns the plane count (0 if empty,
// 6 for a cube, 7 for a ramp/side: the six cube planes plus the slope or diagonal, which passes through
// the cell centre). Exposed for the selftest, which checks it against the stock makeRamp/makeSide.
struct Plane { Vec3d n; double d; };
int blockBrush(int type, int bx, int bz, int by, Plane out[7]);

// ---------------------------------------------------------------------------------------------------
// SRC.4: the core tick (plan §4.2, D1-D3; SrclikeMove.cpp). A clean-room port of srclike-pm-ref.py's
// Player.tick minus water (SRC.6) and duck (SRC.5): friction, accelerate, air-accelerate with the 30 u/s
// wish cap, slide move (4 bumps, 5 clip planes, the §8.2.3a same-plane nudge), step move, stay-on-ground,
// jump (vz SET on the press tick, no friction that tick, §8.2.1), leapfrog gravity, maxvelocity clamp,
// categorize with the surf rule (§4.4, on the post-move velocity: §8.2.2). Source units, z up, double.
extern const double kWalkSpeed;       // 227.68 u/s = Eden's walk SPEED_M*1.3 = 5.85 b/s (U1)
extern const double kJumpImpulse;     // 268.33 u/s = sqrt(2*800*45): a 45 u apex
extern const double kStepSize;        // 18 u

struct PmState {
    Vec3d  origin;        // feet centre
    Vec3d  vel;           // u/s
    double halfWidth;     // u (Eden boxbase/2)
    double height;        // u (Eden boxheight)
    double speedMul;      // input magnitude: 1 walk, 1.3 sprint, analog stick < 1 (wish cap = kWalkSpeed * this)
    bool   onground;
    bool   jumped;        // this tick left the ground by a jump
    // SRC.6 (plan D6). Liquid level as Source counts it: 0 dry, 1 feet, 2 waist, 3 eyes; type 1 water, 2 lava.
    int    waterlevel = 0;
    int    watertype = 0;
    int    groundType = -1;       // block type under the feet while onground, else -1
    bool   onLadder = false;      // this tick used the ladder move
    double waterJumpT = 0;        // seconds left of a water-jump (input locked); 0 = none
    double ladderCool = 0;        // seconds before a ladder can be grabbed again (after jumping off)
    bool   bounced = false;       // this tick rebounded off a trampoline
};

// One tick of dt seconds. cmd.forward/side in [-1,1], side in Eden's strafe sense (Player::walk_force.x:
// world direction (-sin yaw, cos yaw) in Source x/y); cmd.jump is a press (takeJump already did the edge).
void pmTick(PmState& s, const UserCmd& cmd, double dt);
void pmCategorize(PmState& s);        // ground probe + surf rule; also call after placing a state
Trace pmTrace(const PmState& s, const Vec3d& start, const Vec3d& end);
void pmWater(PmState& s);             // SRC.6: refresh waterlevel/watertype from the terrain at s.origin
double tickInterval();                // 1 / move.tickrate, clamped to a sane range

// The frame driver (D2): Player::update calls this in the mode instead of the stock path. It runs the
// fixed ticks the frame's etime owes (sampling the usercmd per tick), then writes Player::pos/vel as the
// interpolation between the last two ticks. Player::pos/vel changed by anyone else (teleport, portal,
// respawn, world edge, crouch is not one: it keeps the feet) re-seeds the state from them.
// Returns whether the player stands on ground; *jumpedOut = a jump tick ran this frame.
bool frame(Player* pl, float etime, bool jumpHeld, bool duckHeld, bool* jumpedOut);
// SRC.7 camera (plan §5). C2: eye 64/72 of the stand hull, 28 u ducked, lerped by the hull's own ease;
// C4: a step-up/down tick eases the eye over ~0.1 s; C5: pitch limit in the mode.
extern const float kPitchLimit;       // 89 degrees (stock 80)
float eyeHeight(const Player* pl);    // eye above the feet, blocks (hull-ease lerp, no step smoothing)
float eyeY(const Player* pl);         // world y of the eye: feet + eyeHeight + step smoothing
float stepSmoothing();                // the eye's current step offset, blocks (harness)
float lookDegreesPerCount(float csSens);   // C6: csSens * 0.022 (m_yaw / m_pitch)
void invalidate();                    // the mode's path did not run this frame: re-seed next time
const PmState& state();               // the authoritative tick state (harness)
const Vec3d& renderOrigin();          // the interpolated origin Player::pos was last written from (double)
unsigned tickCount();                 // ticks run since boot (harness)

}   // namespace SrclikeMove

#endif
