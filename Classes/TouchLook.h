//
//  TouchLook.h
//  Emod — Stage L (WORKING/touch-camera-plan-2026-10-17.md): the touch drag camera.
//
//  Player::processInput's movecam branch used to turn the camera by the LAST event's int
//  delta in engine points (D1, D2), at a speed fixed per engine point (D3), after a 15-point
//  dead zone it re-tested every frame (D4, D5). This module is the replacement arithmetic; the
//  branch's arbitration (inuse, movecam, num>1, placeBlock) is unchanged. Docs:
//  docs/player-input-camera.md "Touch look".
//
#ifndef Eden_TouchLook_h
#define Eden_TouchLook_h

#include "Input.h"

// Parameters, written by the seam (the CAMERA_VIEW pattern). Window points are what the OS calls
// a point (an iOS pt); the engine's point space is derived (DisplayProfile.mm) and moves with
// the window aspect and UI scale, which is why speed and slop are in window points.
extern float TOUCH_PT_SCALE;          // window points per engine point (1 when no box is known)
extern float TOUCH_LOOK_DEG_PER_PT;   // degrees per window point (0.5 tablet, 0.65 phone)
extern float TOUCH_LOOK_SENS_X;       // multipliers, 1 = default
extern float TOUCH_LOOK_SENS_Y;
extern float TOUCH_LOOK_SMOOTH;       // 0 = raw exactly, 1 = heavy (One-Euro min cutoff 1 Hz)
extern float TOUCH_LOOK_GLIDE;      // glide half-life in seconds; 0 = off (keep turning after release, decaying)
extern float TOUCH_SLOP_PT;           // tap-vs-drag threshold, window points, latched once crossed

// --look-selftest's mutation legs: each puts one fixed bug back (port/src/harness/).
enum {
    TL_MUT_LASTONLY  = 1<<0,   // D1: keep only the last event's delta
    TL_MUT_INT       = 1<<1,   // D2: truncate positions to int engine points
    TL_MUT_ENGINEPTS = 1<<2,   // D3: speed per engine point (TOUCH_PT_SCALE ignored)
    TL_MUT_NOFLUSH   = 1<<3,   // smoothing remainder dropped on release
    TL_MUT_NOSLOP    = 1<<4,   // look starts below the tap threshold
    TL_MUT_NOCLEAR   = 1<<5,   // a drag past the slop still taps
    TL_MUT_NODECAY   = 1<<6,   // glide never stops
    TL_MUT_UNLATCHED = 1<<7,   // D5: slop re-tested every frame
    TL_MUT_MULTI     = 1<<8,   // two world fingers keep looking (num>1 rule off)
    TL_MUT_CLEARLATCH= 1<<9,   // L.7: a second finger's touch-down clears the latched look finger for good
};
extern int TOUCH_LOOK_MUTATE;
// --look-mutate=<name>: lastonly int enginepts noflush noslop noclear nodecay unlatched multi clearlatch.
// 0 for an unknown name.
int touchlook_mutation_bit(const char* name);

// Engine points per window point of slop, i.e. the threshold Player compares positions against.
float touchlook_slop_engine_pts(void);
// A movecam touch went down in `slot`: resets its filter and cancels any glide.
void touchlook_begin(int slot);
// Consumes t->ldx/ldy (engine points); returns this frame's yaw/pitch change in degrees, before
// invert (Player applies invertcam exactly as stock did). Call every frame the touch looks, with
// or without new events, so the filter settles when the finger stops.
void touchlook_step(int slot, itouch* t, float etime, float* dyaw, float* dpitch);
// The touch ended while looking: consumes what is left in t->ldx/ldy, flushes the filter's
// remainder (so a drag turns exactly its travel) and, with TOUCH_LOOK_GLIDE, starts the glide.
void touchlook_end(int slot, itouch* t, float* dyaw, float* dpitch);
// Once per frame: the glide's contribution (0 when none is running).
void touchlook_glide(float etime, float* dyaw, float* dpitch);
// Inside the slop: drops t->ldx/ldy (stock's discard, so crossing the slop does not jump).
void touchlook_discard(int slot, itouch* t);
// Per-touch totals since touchlook_begin, engine points, for --look-trace: what the look
// consumed and what the slop discarded on purpose (per-axis |dx|+|dy| per frame), and the
// consumed motion's signed sums. out[4] = consumed, discarded, sumx, sumy.
void touchlook_stats(int slot, double out[4]);

#endif
