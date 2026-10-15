// Movement_web.mm — Phase 1 of the PC controls audit (see
// /Users/sam/.claude/plans/please-run-an-audit-lovely-dongarra.md, finding F1/F2).
//
// ROOT CAUSE (F1): Classes/Player.mm:940-958 damps velocity with a PER-FRAME multiplier
// (vel *= .90 on ground, etc.), not one scaled by etime. Acceleration IS etime-scaled
// (vel += accel*etime, accel = MOVE_SPEED*walk_force), so steady-state ground speed is
// v = 9*a*dt — proportional to frame time. Below ~56 fps Player::update's own substep
// (Classes/Player.mm:755-767) self-regulates to the iOS-era ~5.6 u/s; above that (this port
// runs ~75-144 Hz in Chrome) there is no compensation and top speed falls as low as -61%.
//
// FIX: `--wrap` Player::setSpeed (mangled name confirmed via
// `llvm-nm build-st/CMakeFiles/eden.dir/.../Classes/Player.mm.o`, NOT guessed — see
// web-port-final:web/docs/STATUS.md's "measure, never extrapolate" rule) and scale the walk_dir
// magnitude passed through by `k = clamp(DT_REF/dt_smoothed, 1, 4)`. Both callers of
// setSpeed — the keyboard path (Input_web.mm's eden_set_move_input) and the real touch
// joystick (Classes/Joystick.mm:70/83) — funnel through this one wrap, so both are fixed by
// one piece of code, and Player.mm itself is never touched.
//
// `max_walk_speed` (the ceiling, `walk_speed*SPEED_M*1.3`) is deliberately left alone — only
// the ACCELERATION term (walk_dir's magnitude) is scaled. That preserves the exponential
// "bouncy" ramp-up feel (only `a` changes, not the .90 damping) and leaves gravity/the jump
// arc untouched (they are already etime-scaled and don't go through setSpeed at all).
//
// F2 (Shift-sprint is a no-op): both callers pass a UNIT-length dir vector plus `walk_speed`
// as a separate ceiling-only parameter, so walk_force's magnitude (and therefore
// acceleration) never actually reflects `walk_speed` — only the clamp does. Since natural
// steady-state (after the fix above) sits at ~5.25, below every ceiling that matters, Shift
// changes nothing. Fixed by ALSO folding `walk_speed` into the scaled magnitude (target
// v = 5.25 * walk_speed), leaving the ceiling at walk_speed*5.85 so it never binds.
#import "../shim/foundation/uikit_stubs.h"
#import "../shim/foundation/NSArray.h"
#import "../shim/foundation/NSAutoreleasePool.h"
#import "../../../Classes/World.h"
#include "../shim/foundation/platform_shims.h"  // eden_platform_now_ms
#include <algorithm>
#include <cmath>

namespace {

constexpr double kDtRef = 1.0 / 60.0;   // the frame rate the engine's feel was tuned at
constexpr float  kMinK = 1.0f;          // never SLOW the player down — below ~56fps the
                                         // engine's own substep already regulates (F1 table)
constexpr float  kMaxK = 4.0f;          // cap so a stutter/240Hz panel can't spike accel

double g_lastTickTime = 0.0;
bool   g_haveLastTick = false;
double g_dtEMA = kDtRef;                // smoothed frame interval; seeded at the reference

} // namespace

// Port-owned setting (Settings_web.mm's kSettings[] "fps_normalize" row), default ON. Defined in
// Settings_web.mm exactly like eden_look_sensitivity/eden_fov_degrees; this file just reads it.
extern "C" float eden_fps_normalize;

extern "C" {

// Polled once per rAF frame from public/eden-st.html's existing trackCursorNeed loop, alongside
// eden_ui_tick()/EdenSettings.tick() — NOT derived from World::update's own etime (that's the
// raw, jittery, uncompensated delta this whole fix exists to counteract; see
// EdenViewController_web.cpp:57-59). A simple EMA smooths out a single dropped frame from
// spiking `k` for one tick.
EDEN_EXPORT
void eden_movement_tick(void) {
    double now = eden_platform_now_ms() / 1000.0;
    if (g_haveLastTick) {
        double dt = now - g_lastTickTime;
        // Guard against a backgrounded-tab wakeup (huge dt) or a zero/negative reading —
        // neither is a real frame interval, and folding either into the EMA would corrupt it
        // for many frames afterward.
        if (dt > 0.0 && dt < 0.5) {
            const double alpha = 0.15;
            g_dtEMA = g_dtEMA * (1.0 - alpha) + dt * alpha;
        }
    }
    g_lastTickTime = now;
    g_haveLastTick = true;
}

EDEN_EXPORT
float eden_movement_debug_k(void) {
    double k = kDtRef / g_dtEMA;
    return (float)std::clamp(k, (double)kMinK, (double)kMaxK);
}

// The scaling itself, kept as one shared implementation of
// it (Phase N Stage 2). One caller now:
//   * `eden_set_move_input` (Input_web.mm), the keyboard/gamepad path.
//
// THIS IS NOT POLISH, and Phase N Stage 1's notes described it as such — wrongly. Player::setSpeed
// treats `walk_dir`'s MAGNITUDE as the target velocity and `walk_speed` only as a separate ceiling
// (see this file's header, F1/F2), so a caller handing it a unit vector produces a player who does
// not move. Without this, keyboard movement on a target with no --wrap is silently dead.
EDEN_EXPORT
void eden_movement_scale_walk_dir(float* x, float* y, float* z, float walk_speed) {
    float k = 1.0f;
    if (eden_fps_normalize != 0.0f) {
        double raw_k = kDtRef / g_dtEMA;
        k = (float)std::clamp(raw_k, (double)kMinK, (double)kMaxK);
    }
    const float scale = k * walk_speed;
    if (x) *x *= scale;
    if (y) *y *= scale;
    if (z) *z *= scale;
}

} // extern "C"
