//
//  SrclikeMove.cpp -- see SrclikeMove.h. SRC.2: settings state, usercmd, jump latch, tunables. SRC.4: the
//  core tick (pmTick) and the frame driver (frame) Player::update calls in the mode.
//
#include "SrclikeMove.h"
#include "Player.h"
#include "Liquids.h"
#include "BlockTraits.h"
#include "Terrain.h"
#include "Globals.h"
#include <math.h>

namespace SrclikeMove {

const Tunables kDefaults = {5.0f, 100.0f, 4.0f, 75.0f, 800.0f, 3500.0f, 66.67f};

static bool g_enabled = false;
static bool g_autoBhop = false;
static Tunables g_tun = kDefaults;
static float g_fwd = 0, g_side = 0;
static bool g_pulse = false;
static bool g_heldPrev = false;
static unsigned g_presses = 0;
static UserCmd g_last = {0, 0, false, false, 0, 0, false};

bool enabled() { return g_enabled; }
void setEnabled(bool on) {
    if (g_enabled == on) return;
    g_enabled = on;
    resetInput();
}
bool autoBhop() { return g_autoBhop; }
void setAutoBhop(bool on) { g_autoBhop = on; }

Tunables& tunables() { return g_tun; }
void resetTunables() { g_tun = kDefaults; }

static float clamp1(float v) { return v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v); }

void setKeyAxes(float forward, float side) {
    g_fwd = clamp1(forward);
    g_side = clamp1(side);
}

void postJumpPulse() { g_pulse = true; }

unsigned pressCount() { return g_presses; }

bool takeJump(bool held) {
    const bool pressed = (held && (!g_heldPrev || g_autoBhop)) || g_pulse;
    g_pulse = false;
    g_heldPrev = held;
    if (pressed) ++g_presses;
    return pressed;
}

UserCmd buildCmd(bool jumpHeld, bool duckHeld, float yaw, float pitch) {
    UserCmd c;
    c.forward = g_fwd;
    c.side = g_side;
    c.jump = takeJump(jumpHeld);
    c.duck = duckHeld;
    c.yaw = yaw;
    c.pitch = pitch;
    c.jumpHeld = jumpHeld;
    g_last = c;
    return c;
}

const UserCmd& lastCmd() { return g_last; }

void resetInput() {
    g_fwd = g_side = 0;
    g_pulse = false;
    g_heldPrev = false;
    g_last = UserCmd{0, 0, false, false, 0, 0, false};
}

}   // namespace SrclikeMove

// ===================================================================================================
// SRC.4: the core tick and the frame driver. Clean room (D4): a line-for-line port of
// port/tools/srclike-pm-ref.py's Player (the SRC.1 model) minus water and duck; that script's printout
// (fixtures/srclike-pm/vectors.txt) is what --srclike-selftest=move matches within 1 %.
// ===================================================================================================
namespace SrclikeMove {

const double kWalkSpeed = 5.85 * (72.0 / 1.85);
const double kJumpImpulse = 268.32815729997476;   // sqrt(2 * 800 * 45)
const double kStepSize = 18.0;

static const double kAirWishCap = 30.0;
static const double kStandableNz = 0.7;
static const double kNonJumpVel = 140.0;
static const double kGroundProbe = 2.0;
static const int    kMaxBumps = 4;
static const int    kMaxClipPlanes = 5;
static const double kOverbounce = 1.0;            // 1 + sv_bounce 0

static inline Vec3d vadd(const Vec3d& a, const Vec3d& b) { return Vec3d{a.x + b.x, a.y + b.y, a.z + b.z}; }
static inline Vec3d vsub(const Vec3d& a, const Vec3d& b) { return Vec3d{a.x - b.x, a.y - b.y, a.z - b.z}; }
static inline Vec3d vmul(const Vec3d& a, double s) { return Vec3d{a.x * s, a.y * s, a.z * s}; }
static inline double vdot(const Vec3d& a, const Vec3d& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline Vec3d vcross(const Vec3d& a, const Vec3d& b) {
    return Vec3d{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
static inline double vlen(const Vec3d& a) { return sqrt(vdot(a, a)); }
static inline double hlen(const Vec3d& a) { return hypot(a.x, a.y); }
static inline double vnorm(Vec3d& a) {   // normalises in place, returns the old length
    const double l = vlen(a);
    if (l > 0) a = vmul(a, 1.0 / l); else a = Vec3d{0, 0, 0};
    return l;
}

double tickInterval() {
    double r = g_tun.tickrate;
    if (!(r >= 10.0)) r = 10.0;          // also catches NaN
    if (r > 1000.0) r = 1000.0;
    return 1.0 / r;
}

Trace pmTrace(const PmState& s, const Vec3d& a, const Vec3d& b) {
    return traceHull(a, b, Vec3d{-s.halfWidth, -s.halfWidth, 0}, Vec3d{s.halfWidth, s.halfWidth, s.height});
}

// Plan §4.4 (U2): a ramp plane is ground only when we were grounded last tick, or slow onto it.
static bool isGround(const PmState& s, const Trace& t, bool was) {
    if (t.fraction >= 1.0 || t.normal.z < kStandableNz) return false;
    if (!t.ramp) return true;
    return was || (hlen(s.vel) <= 1.1 * kWalkSpeed && fabs(s.vel.z) <= kJumpImpulse);
}

void pmCategorize(PmState& s) {
    const bool was = s.onground;
    if (s.vel.z > kNonJumpVel) { s.onground = false; s.groundType = -1; return; }
    const Trace t = pmTrace(s, s.origin, vsub(s.origin, Vec3d{0, 0, kGroundProbe}));
    s.onground = isGround(s, t, was);
    s.groundType = s.onground ? t.type : -1;
}

static Vec3d wishVector(const UserCmd& c, const PmState& s) {
    const double y = c.yaw * (M_PI / 180.0), cy = cos(y), sy = sin(y);
    const double ws = kWalkSpeed * s.speedMul;
    // forward (cos, sin); Eden's strafe direction (-sin, cos) (Player::update's cosYaw90/sinYaw90).
    return Vec3d{(cy * c.forward - sy * c.side) * ws, (sy * c.forward + cy * c.side) * ws, 0};
}

static void accelerate(PmState& s, const Vec3d& dir, double wishspeed, double accel, double dt) {
    const double add = wishspeed - vdot(s.vel, dir);
    if (add <= 0) return;
    double a = accel * dt * wishspeed;
    if (a > add) a = add;
    s.vel = vadd(s.vel, vmul(dir, a));
}

static void airAccelerate(PmState& s, const Vec3d& dir, double wishspeed, double dt) {
    const double cap = wishspeed < kAirWishCap ? wishspeed : kAirWishCap;
    const double add = cap - vdot(s.vel, dir);
    if (add <= 0) return;
    double a = g_tun.airaccelerate * wishspeed * dt;   // the uncapped wishspeed scales the rate
    if (a > add) a = add;
    s.vel = vadd(s.vel, vmul(dir, a));
}

static void friction(PmState& s, double dt, double mul = 1.0) {
    const double speed = vlen(s.vel);
    if (speed < 0.1) return;
    const double drop = (speed > g_tun.stopspeed ? speed : g_tun.stopspeed) * g_tun.friction * mul * dt;
    double ns = speed - drop;
    if (ns < 0) ns = 0;
    s.vel = vmul(s.vel, ns / speed);
}

static void clampVelocity(PmState& s) {
    const double m = g_tun.maxvelocity;
    double* v[3] = {&s.vel.x, &s.vel.y, &s.vel.z};
    for (double* p : v) { if (*p > m) *p = m; else if (*p < -m) *p = -m; else if (*p != *p) *p = 0; }
}

static Vec3d clipVelocity(const Vec3d& v, const Vec3d& n, double overbounce) {
    Vec3d out = vsub(v, vmul(n, vdot(v, n) * overbounce));
    const double adj = vdot(out, n);
    if (adj < 0) out = vsub(out, vmul(n, adj));
    return out;
}

static void slideMove(PmState& s, double dt) {
    const Vec3d primal = s.vel;
    Vec3d original = s.vel;
    Vec3d planes[kMaxClipPlanes];
    int np = 0;
    double timeLeft = dt;
    for (int bump = 0; bump < kMaxBumps; ++bump) {
        if (vlen(s.vel) == 0) break;
        const Trace t = pmTrace(s, s.origin, vadd(s.origin, vmul(s.vel, timeLeft)));
        if (t.allsolid) { s.vel = Vec3d{0, 0, 0}; return; }
        if (t.fraction > 0) { s.origin = t.endpos; original = s.vel; np = 0; }
        if (t.fraction == 1.0) break;
        timeLeft -= timeLeft * t.fraction;
        if (np >= kMaxClipPlanes) { s.vel = Vec3d{0, 0, 0}; break; }
        bool again = false;   // §8.2.3a: the same plane again (float noise on a slide): nudge off it
        for (int i = 0; i < np; ++i) again = again || vdot(t.normal, planes[i]) > 0.99;
        if (again) { s.vel = vadd(t.normal, s.vel); continue; }
        planes[np++] = t.normal;
        if (np == 1 && !s.onground) {
            s.vel = original = clipVelocity(original, planes[0], kOverbounce);
            continue;
        }
        int i = 0;
        for (; i < np; ++i) {
            const Vec3d v = clipVelocity(original, planes[i], kOverbounce);
            bool ok = true;
            for (int j = 0; j < np && ok; ++j) if (j != i && vdot(v, planes[j]) < 0) ok = false;
            if (ok) { s.vel = v; break; }
        }
        if (i == np) {
            if (np != 2) { s.vel = Vec3d{0, 0, 0}; break; }
            Vec3d d = vcross(planes[0], planes[1]);
            vnorm(d);
            s.vel = vmul(d, vdot(d, s.vel));
        }
        if (vdot(s.vel, primal) <= 0) { s.vel = Vec3d{0, 0, 0}; break; }
    }
}

static void stayOnGround(PmState& s) {
    const Trace up = pmTrace(s, s.origin, vadd(s.origin, Vec3d{0, 0, 2.0}));
    const Trace t = pmTrace(s, up.endpos, vsub(s.origin, Vec3d{0, 0, kStepSize}));
    if (t.fraction > 0 && t.fraction < 1 && !t.startsolid && t.normal.z >= kStandableNz) s.origin = t.endpos;
}

static void stepMove(PmState& s, double dt) {
    const Vec3d p0 = s.origin, v0 = s.vel;
    slideMove(s, dt);                                   // path 1: plain slide
    const Vec3d pd = s.origin, vd = s.vel;
    s.origin = p0; s.vel = v0;                          // path 2: up a step, slide, back down
    const double lift = kStepSize + kDistEpsilon;
    Trace t = pmTrace(s, s.origin, vadd(s.origin, Vec3d{0, 0, lift}));
    if (!t.startsolid && !t.allsolid) s.origin = t.endpos;
    slideMove(s, dt);
    t = pmTrace(s, s.origin, vsub(s.origin, Vec3d{0, 0, lift}));
    if (!t.startsolid && !t.allsolid) s.origin = t.endpos;
    if (t.normal.z < kStandableNz) { s.origin = pd; s.vel = vd; return; }   // nothing walkable: path 1
    if (hlen(vsub(pd, p0)) > hlen(vsub(s.origin, p0))) { s.origin = pd; s.vel = vd; }
    else s.vel.z = vd.z;
}

static void walkMove(PmState& s, const UserCmd& c, double dt) {
    Vec3d dir = wishVector(c, s);
    double ws = vnorm(dir);
    const double cap = kWalkSpeed * s.speedMul;
    if (ws > cap) ws = cap;
    s.vel.z = 0;
    accelerate(s, dir, ws, g_tun.accelerate, dt);
    s.vel.z = 0;
    if (vlen(s.vel) < 1.0) { s.vel = Vec3d{0, 0, 0}; return; }
    const Trace t = pmTrace(s, s.origin, vadd(s.origin, vmul(s.vel, dt)));
    if (t.fraction == 1.0) s.origin = t.endpos;
    else stepMove(s, dt);
    stayOnGround(s);
}

static void airMove(PmState& s, const UserCmd& c, double dt) {
    Vec3d dir = wishVector(c, s);
    double ws = vnorm(dir);
    const double cap = kWalkSpeed * s.speedMul;
    if (ws > cap) ws = cap;
    airAccelerate(s, dir, ws, dt);
    slideMove(s, dt);
}

// ---- SRC.6: liquids, ladders, ice, trampolines, water-jump (plan D6). Eden-flavoured, Source-shaped. ----
static const double kIceFrictionMul = 0.1;       // stock: .99 vs .90 per frame = a tenth of the loss
static const double kLavaFrictionMul = 3.0;      // thick: lava drags harder than water
static const double kSwimUp = 100.0, kSwimSink = 60.0, kWaterWish = 0.8;
static const double kWaterJumpVz = 256.0, kWaterJumpFwd = 60.0;
static const double kWaterJumpProbe = 8.0, kWaterJumpClear = 38.0;   // solid at feet+8, empty at feet+38
static const double kLadderSpeed = 200.0, kLadderPad = 3.0, kLadderCool = 0.3;
static const double kBounceMinFrac = 0.6;        // a landing on a trampoline rebounds >= 0.6 of a jump
static const double kFlowAccel = 5.0;            // blocks/s^2 (Player.cpp FLOW_SPEED)

// 0 none, 1 water, 2 lava: is the point inside a liquid cell's filled part (fill level/4 of the cell)?
static int liquidAt(const Vec3d& p) {
    const double U = kUnitsPerBlock, bx = p.x / U, bz = p.y / U, by = p.z / U;
    if (bx < 0 || bz < 0) return 0;
    const int cx = (int)floor(bx), cz = (int)floor(bz), cy = (int)floor(by);
    const int t = getLandc2(cx, cz, cy);
    if (t <= 0 || t >= BLOCK_IDS || !(blockinfo[t] & IS_LIQUID)) return 0;
    if (by - cy >= getLevel(t) / 4.0) return 0;
    return (blockinfo[t] & IS_LAVA) ? 2 : 1;
}

static bool solidAt(const Vec3d& p) {
    const double U = kUnitsPerBlock;
    if (p.x < 0 || p.y < 0) return false;
    return blockShape(getLandc2((int)floor(p.x / U), (int)floor(p.y / U), (int)floor(p.z / U))) != SHAPE_EMPTY;
}

void pmWater(PmState& s) {
    s.waterlevel = 0;
    s.watertype = 0;
    const int f = liquidAt(vadd(s.origin, Vec3d{0, 0, 1.0}));
    if (!f) return;
    s.watertype = f;
    s.waterlevel = 1;
    if (!liquidAt(vadd(s.origin, Vec3d{0, 0, s.height * 0.5}))) return;
    s.waterlevel = 2;
    if (liquidAt(vadd(s.origin, Vec3d{0, 0, s.height * (64.0 / 72.0)}))) s.waterlevel = 3;
}

static bool touchingLadder(const PmState& s) {
    const double U = kUnitsPerBlock, r = s.halfWidth + kLadderPad;
    const int x0 = (int)floor((s.origin.x - r) / U), x1 = (int)floor((s.origin.x + r) / U);
    const int z0 = (int)floor((s.origin.y - r) / U), z1 = (int)floor((s.origin.y + r) / U);
    const int y0 = (int)floor((s.origin.z + 1.0) / U), y1 = (int)floor((s.origin.z + s.height - 1.0) / U);
    for (int x = x0; x <= x1; ++x) {
        if (x < 0) continue;
        for (int z = z0; z <= z1; ++z) {
            if (z < 0) continue;
            for (int y = y0; y <= y1; ++y) {
                const int t = getLandc2(x, z, y);
                if (t > 0 && t < BLOCK_IDS && trait_climbable(t)) return true;
            }
        }
    }
    return false;
}

// Flow push (stock: vel += flowdir * FLOW_SPEED * etime while in water), horizontal only.
static void flowPush(PmState& s, double dt) {
    float ex, ey, ez;
    toEden(s.origin, &ex, &ey, &ez);
    if (ex < 0 || ez < 0) return;
    const Vector fd = getFlowDirection((int)ex, (int)ez, (int)(ey + 0.01f));
    s.vel.x += fd.x * kFlowAccel * kUnitsPerBlock * dt;
    s.vel.y += fd.z * kFlowAccel * kUnitsPerBlock * dt;
}

static void waterMove(PmState& s, const UserCmd& c, double dt) {
    const double yaw = c.yaw * (M_PI / 180.0), pit = c.pitch * (M_PI / 180.0);   // Eden: pitch > 0 looks up
    const double cy = cos(yaw), sy = sin(yaw), cp = cos(pit), sp = sin(pit);
    const double ws0 = kWalkSpeed * s.speedMul;
    Vec3d wv{(cy * cp * c.forward - sy * c.side) * ws0, (sy * cp * c.forward + cy * c.side) * ws0, sp * c.forward * ws0};
    if (c.jumpHeld) wv.z += kWalkSpeed;
    else if (c.forward == 0 && c.side == 0) wv.z -= kSwimSink;
    double ws = vnorm(wv);
    const double cap = kWalkSpeed * s.speedMul;
    if (ws > cap) ws = cap;
    ws *= kWaterWish;
    double speed = vlen(s.vel), ns = 0;
    if (speed > 0) {
        ns = speed - dt * speed * g_tun.friction * (s.watertype == 2 ? kLavaFrictionMul : 1.0);
        if (ns < 0.1) ns = 0;
        s.vel = vmul(s.vel, ns / speed);
    }
    if (ws >= 0.1) {
        const double add = ws - ns;
        if (add > 0) {
            double a = g_tun.accelerate * ws * dt;
            if (a > add) a = add;
            s.vel = vadd(s.vel, vmul(wv, a));
        }
    }
    flowPush(s, dt);
    slideMove(s, dt);
}

// Source's CheckWaterJump, Eden-sized: at the waist, pushing forward, a wall at the feet and clear air above it.
static bool tryWaterJump(PmState& s, const UserCmd& c) {
    if ((s.waterlevel != 2 && s.waterlevel != 1) || c.forward <= 0.1f) return false;
    const double yaw = c.yaw * (M_PI / 180.0), dx = cos(yaw), dy = sin(yaw), reach = s.halfWidth + kWaterJumpProbe;
    const Vec3d low = vadd(s.origin, Vec3d{dx * reach, dy * reach, kWaterJumpProbe});
    const Vec3d high = vadd(s.origin, Vec3d{dx * reach, dy * reach, kWaterJumpClear});
    if (!solidAt(low) || solidAt(high) || liquidAt(high)) return false;
    s.waterJumpT = 2.0;
    s.onground = false;
    s.vel = Vec3d{dx * kWaterJumpFwd, dy * kWaterJumpFwd, kWaterJumpVz};
    return true;
}

static void ladderMove(PmState& s, const UserCmd& c, double dt) {
    Vec3d w = wishVector(c, s);
    s.vel = Vec3d{w.x * 0.35, w.y * 0.35, c.forward * kLadderSpeed * (c.pitch >= -30.0f ? 1.0 : -1.0)};
    s.onLadder = true;
    slideMove(s, dt);
}

void pmTick(PmState& s, const UserCmd& c, double dt) {
    const double g = g_tun.gravity;
    s.jumped = false;
    s.onLadder = false;
    s.bounced = false;
    if (s.ladderCool > 0) s.ladderCool -= dt;
    pmWater(s);

    if (s.waterJumpT > 0) {   // input locked: gravity and the slide until the rise ends or we land
        s.waterJumpT -= dt;
        if (s.vel.z < 0 || s.onground) s.waterJumpT = 0;
    }
    if (s.waterJumpT > 0) {
        s.onground = false;
        s.vel.z -= 0.5 * g * dt;
        slideMove(s, dt);
        pmCategorize(s);
        if (!s.onground) s.vel.z -= 0.5 * g * dt;
        pmWater(s);
        return;
    }
    if (s.waterlevel >= 2) {   // swim: no gravity, Source's water move
        if (!tryWaterJump(s, c)) {
            if (c.jumpHeld) { s.onground = false; s.vel.z = kSwimUp; }
            waterMove(s, c, dt);
            pmCategorize(s);
            clampVelocity(s);
            pmWater(s);
            return;
        }
        slideMove(s, dt);
        pmCategorize(s);
        pmWater(s);
        return;
    }
    if (s.waterlevel == 1 && !s.onground) tryWaterJump(s, c);   // near the surface: the swim-up may leave level 2 first
    if (s.waterJumpT > 0) { slideMove(s, dt); pmCategorize(s); pmWater(s); return; }
    const bool grab = s.ladderCool <= 0 && (c.forward != 0 || c.side != 0) && touchingLadder(s);
    if (grab && !c.jump) {
        ladderMove(s, c, dt);
        pmCategorize(s);
        clampVelocity(s);
        pmWater(s);
        return;
    }
    if (c.jump && !s.onground && touchingLadder(s)) {   // jump off a ladder: a half-strength kick, no instant re-grab
        s.vel.z = kJumpImpulse * 0.5;
        s.ladderCool = kLadderCool;
        s.jumped = true;
    }

    const bool wasGround = s.onground;
    if (s.onground && c.jump) {                 // §8.2.1: vz SET before the first half-gravity step
        s.onground = false;
        s.vel.z = kJumpImpulse;
        s.jumped = true;
    }
    if (!s.onground) s.vel.z -= 0.5 * g * dt;   // leapfrog gravity: half before the move...
    else {
        s.vel.z = 0;
        friction(s, dt, (s.groundType >= 0 && trait_slippery(s.groundType)) ? kIceFrictionMul : 1.0);   // ...and no friction on the jump tick: that is the bhop
    }
    clampVelocity(s);
    if (s.waterlevel == 1 && !s.onground) flowPush(s, dt);
    const double vzPre = s.vel.z;
    if (s.onground) walkMove(s, c, dt);
    else airMove(s, c, dt);
    pmCategorize(s);
    if (!wasGround && s.onground && !s.jumped && vzPre < 0 && s.groundType >= 0 && trait_bouncy(s.groundType)) {
        const double rb = -vzPre > kBounceMinFrac * kJumpImpulse ? -vzPre : kBounceMinFrac * kJumpImpulse;
        s.vel.z = rb;                           // elastic, with a floor so a step onto it still springs
        s.onground = false;
        s.groundType = -1;
        s.bounced = true;
    }
    clampVelocity(s);
    if (!s.onground) s.vel.z -= 0.5 * g * dt;   // ...half after
    else s.vel.z = 0;
    pmWater(s);
}

// ---------------------------------------------------------------------------------------------------
// The frame driver.
static PmState g_st;
static Vec3d g_prev;                 // origin before the last tick (interpolation)
static Vec3d g_render;               // the interpolated origin last written to Player::pos
static double g_acc = 0;             // seconds owed to the tick
static bool g_have = false;          // g_st is live (the mode's path ran last frame)
static Vector g_wrotePos, g_wroteVel;
static float g_wroteHeight = 0;
static unsigned g_ticks = 0;
static double g_stepOff = 0;         // C4: eye offset in Source units (negative after a step up), decays to 0

const PmState& state() { return g_st; }
const Vec3d& renderOrigin() { return g_render; }
unsigned tickCount() { return g_ticks; }
void invalidate() { g_have = false; g_stepOff = 0; }

// ---- SRC.7 camera ----------------------------------------------------------------------------------
const float kPitchLimit = 89.0f;
static const double kEyeStand = 64.0, kEyeDuck = 28.0;   // SDK g_DefaultViewVectors (SRC.1 pin)
static const double kStepEaseSeconds = 0.1;
float lookDegreesPerCount(float csSens) { return csSens * 0.022f; }
float eyeHeight(const Player* pl) {
    const double stand = 1.85, crouch = 0.925;   // blocks: Player.cpp STAND/CROUCH_BOX_HEIGHT (BLOCK_SIZE is 1)
    double t = (pl->boxheight - crouch) / (stand - crouch);
    t = t < 0 ? 0 : (t > 1 ? 1 : t);
    return (float)((kEyeDuck + (kEyeStand - kEyeDuck) * t) / kUnitsPerBlock);
}
float stepSmoothing() { return (float)(g_stepOff / kUnitsPerBlock); }
float eyeY(const Player* pl) { return pl->pos.y - pl->boxheight / 2 + eyeHeight(pl) + stepSmoothing(); }

static Vec3d velToSource(const Vector& v) { return Vec3d{v.x * kUnitsPerBlock, v.z * kUnitsPerBlock, v.y * kUnitsPerBlock}; }

// A seeded origin can sit inside something (stock's collider rests the feet exactly ON a floor, which the
// trace calls start-solid; a teleport can land in a block). Lift it the least that frees it.
static bool freeOrigin(PmState& s) {
    static const double lifts[] = {0, 1, 2, 4, 8, 16};   // x DIST_EPSILON, then whole units below
    for (double k : lifts) {
        const Vec3d o = vadd(s.origin, Vec3d{0, 0, k * kDistEpsilon});
        if (!pmTrace(s, o, o).startsolid) { s.origin = o; return true; }
    }
    for (double u = 1; u <= kStepSize; u += 1) {
        const Vec3d o = vadd(s.origin, Vec3d{0, 0, u});
        if (!pmTrace(s, o, o).startsolid) { s.origin = o; return true; }
    }
    return false;
}

static void seed(Player* pl, bool keepVel) {
    g_st.origin = toSource(pl->pos.x, pl->pos.y - pl->boxheight / 2, pl->pos.z);
    g_st.halfWidth = pl->boxbase / 2 * kUnitsPerBlock;
    g_st.height = pl->boxheight * kUnitsPerBlock;
    if (!keepVel) g_st.vel = velToSource(pl->vel);
    if (!freeOrigin(g_st)) {   // deep inside: stock's pop-up (zeroes vel when it moves the player)
        pl->unstick();
        g_st.origin = toSource(pl->pos.x, pl->pos.y - pl->boxheight / 2, pl->pos.z);
        g_st.vel = velToSource(pl->vel);
        freeOrigin(g_st);
    }
    g_st.onground = false;
    pmCategorize(g_st);
    g_prev = g_st.origin;
    g_acc = 0;
    g_stepOff = 0;
}

bool frame(Player* pl, float etime, bool jumpHeld, bool duckHeld, bool* jumpedOut) {
    const double dt = tickInterval();
    *jumpedOut = false;
    // Re-seed from Player when this path did not run last frame, or someone else moved the player. Crouch
    // (Player::preupdate) changes pos.y and boxheight together, keeping the feet: not a move.
    const bool posMoved = pl->pos.x != g_wrotePos.x || pl->pos.z != g_wrotePos.z ||
                          (pl->pos.y != g_wrotePos.y &&
                           fabsf((pl->pos.y - pl->boxheight / 2) - (g_wrotePos.y - g_wroteHeight / 2)) > 1e-3f);
    const bool velChanged = pl->vel.x != g_wroteVel.x || pl->vel.y != g_wroteVel.y || pl->vel.z != g_wroteVel.z;
    if (!g_have || posMoved) seed(pl, false);
    else if (velChanged) { g_st.vel = velToSource(pl->vel); }
    g_have = true;
    g_st.halfWidth = pl->boxbase / 2 * kUnitsPerBlock;
    g_st.height = pl->boxheight * kUnitsPerBlock;

    // Analog fallback (D8): the keyboard gives the raw axes; a stick/touch joystick only reaches
    // walk_force (a direction) and max_walk_speed (the magnitude, as Player::noclipUpdate reads it).
    float mul = 1.0f;
    if (pl->max_walk_speed > 0) {
        mul = pl->max_walk_speed / (4.5f * 1.3f);   // SPEED_M * 1.3
        if (mul > 1.3f) mul = 1.3f;
    }
    g_st.speedMul = mul;

    g_acc += etime > 0 ? etime : 0;
    if (g_acc > 8 * dt) g_acc = 8 * dt;   // a hitch: run at most 8 ticks, drop the rest (slow motion, not a burst)
    while (g_acc >= dt) {
        UserCmd c = buildCmd(jumpHeld, duckHeld, pl->yaw, pl->pitch);
        if (c.forward == 0 && c.side == 0) {
            const float fz = pl->walk_force.z, fx = pl->walk_force.x, fl = sqrtf(fz * fz + fx * fx);
            if (fl > 1e-4f && pl->max_walk_speed > 0) { c.forward = fz / fl; c.side = fx / fl; }
        }
        g_prev = g_st.origin;
        const bool wasGround = g_st.onground;
        pmTick(g_st, c, dt);
        // C4: a grounded tick that changed height by a step (not a slope's ~1 u creep) lags the eye behind.
        const double dz = g_st.origin.z - g_prev.z;
        if (wasGround && g_st.onground && fabs(dz) > 2.0 && fabs(dz) <= kStepSize + 1.0) g_stepOff -= dz;
        if (g_st.jumped) *jumpedOut = true;
        ++g_ticks;
        g_acc -= dt;
    }

    if (g_stepOff != 0) {   // ease to zero over kStepEaseSeconds for a full step
        const double d = kStepSize / kStepEaseSeconds * (etime > 0 ? etime : 0);
        g_stepOff = fabs(g_stepOff) <= d ? 0 : g_stepOff - (g_stepOff > 0 ? d : -d);
    }
    const double a = g_acc / dt;
    const Vec3d r = vadd(g_prev, vmul(vsub(g_st.origin, g_prev), a));
    g_render = r;
    float ex, ey, ez;
    toEden(r, &ex, &ey, &ez);
    pl->pos = MakeVector(ex, ey + pl->boxheight / 2, ez);
    pl->vel = MakeVector((float)(g_st.vel.x / kUnitsPerBlock), (float)(g_st.vel.z / kUnitsPerBlock),
                         (float)(g_st.vel.y / kUnitsPerBlock));
    g_wrotePos = pl->pos;
    g_wroteVel = pl->vel;
    g_wroteHeight = pl->boxheight;
    return g_st.onground;
}

}   // namespace SrclikeMove
