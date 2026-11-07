#!/usr/bin/env python3
# SRC.1: clean-room reference model of Srclike (CS:S flavour) player movement for Emod's
# Experiments > Srclike movement (plan WORKING/srclike-movement-plan-2026-10-31.md, section 8). It is the
# spec SRC.4's C++ tick must match (--srclike-selftest=move, within 1 %), and its printout is what the
# user compares against community figures.
#
#   python3 -I port/tools/srclike-pm-ref.py            # human-readable vectors
#   python3 -I port/tools/srclike-pm-ref.py --json     # same vectors, machine-readable
#   python3 -I port/tools/srclike-pm-ref.py --check    # diff against fixtures/srclike-pm/vectors.txt
#   python3 -I port/tools/srclike-pm-ref.py --write    # regenerate that fixture (after a reviewed change)
#
# Clean room (plan D4): written from the published Quake-lineage formulas (accelerate, air-accelerate with
# a 30 u/s wish cap, friction with stopspeed, clip-velocity, slide move, step move) and from community
# write-ups. No Source SDK code was copied; the SDK was consulted only to read constant VALUES (cited in
# CONSTANTS below by commit). Units are Source units (u), z up, 1 block = 72/1.85 u (plan section 1).
#
# What the model is: one player hull (Eden's width 2/3 block, Eden's stand height 1.85 block = 72 u) moving
# through a world of convex brushes (axis boxes and 45-degree wedges = Eden ramps), traced by the
# classic expanded-plane brush clip. What it is not: bit-exact CS:S. Sub-tick orderings that CS:S's
# binary does not publish (exactly where the half-gravity steps fall on the jump tick, surface-friction
# memory, ground-entity velocity) are fixed here in the simplest documented form and marked "MODEL".
import json, math, os, sys

# ---------------------------------------------------------------------------------------------------
# Sources. SDK = github.com/ValveSoftware/source-sdk-2013 @ be57e3d01ac7d7aa6a2cacbfef187c0b46616db0
# (read for values only). Its CSTRIKE_DLL branches in movevars_shared.cpp hold the same numbers as the
# HL2 branches (stopspeed 100, accelerate 10), so that file is not trusted for CS:S-specific values.
SOURCES = {
    "SDK-movevars": "SDK src/game/shared/movevars_shared.cpp (ConVar defaults)",
    "SDK-gm.h":     "SDK src/game/shared/gamemovement.h (GetAirSpeedCap, GAMEMOVEMENT_* defines)",
    "SDK-gm.cpp":   "SDK src/game/shared/gamemovement.cpp (literals: jump 268.328..., NON_JUMP_VELOCITY, "
                    "ground offset 2, 4 bumps, 5 clip planes, STOP_EPSILON, water wish x0.8, swim-up 100, "
                    "water-jump 256, duck move x1/3)",
    "SDK-defs":     "SDK src/game/shared/shareddefs.h (TIME_TO_DUCK/UNDUCK, WATERJUMP_HEIGHT, MAX_CLIMB_SPEED)",
    "SDK-rules":    "SDK src/game/shared/gamerules.cpp g_DefaultViewVectors (HL2/base hull and eye)",
    "VDC-friction": "developer.valvesoftware.com/wiki/Sv_friction (default 4, CS and CS:S)",
    "VDC-hull":     "developer.valvesoftware.com/wiki/Player_Entity and /Dimensions_(Half-Life_2_and_"
                    "Counter-Strike:_Source) (32x32x72 stand, 36 crouch, eye 64 / 28)",
    "KZ":           "kz-rush.com/en/page/bhop-physics and /strafe-physics (GoldSrc CS 1.6: stopspeed 75, "
                    "accelerate 5, jump sqrt(2*800*45)=268.33, apex 45 u); CS:S inherited these server "
                    "defaults (community figure, not pinned to a CS:S binary)",
    "QW":           "quakeworld.nu/wiki/QW_physics_air (air-accelerate with the 30 u/s wish cap)",
    "TICK":         "CS:S tick interval 0.015 s (tickrate 66, i.e. 66.67 Hz); community/server-config fact",
    "PLAN":         "Emod decision in srclike-movement-plan-2026-10-31.md (U1-U5, D1-D8)",
    "EDEN":         "Classes/Player.cpp (STAND_BOX_HEIGHT 1.85, CROUCH_BOX_HEIGHT 0.925, boxbase 2/3, "
                    "SPEED_M 4.5 x 1.3, JUMP_SPEED 6.7), Classes/Constants.h GRAVITY 20",
}

U_PER_BLOCK = 72.0 / 1.85          # 38.9189... u per block (stand hull 72 u <-> 1.85 blocks)

# name: (value, unit, source keys, note)
CONSTANTS = {
    "tick_interval":   (0.015, "s", "TICK PLAN", "D2: fixed 66.67 Hz tick"),
    "gravity":         (800.0, "u/s^2", "SDK-movevars", "DEFAULT_GRAVITY_STRING; = 20.56 b/s^2 (Eden 20)"),
    "accelerate":      (5.0, "1/s", "KZ PLAN", "CS:S/CS 1.6 server default 5; SDK file says 10 (HL2); cvar move.accelerate"),
    "airaccelerate":   (100.0, "1/s", "PLAN", "U1 surf/bhop value; CS:S default is 10 (SDK-movevars)"),
    "air_wish_cap":    (30.0, "u/s", "SDK-gm.h QW", "GetAirSpeedCap()"),
    "friction":        (4.0, "1/s", "VDC-friction SDK-movevars", ""),
    "stopspeed":       (75.0, "u/s", "KZ PLAN", "CS:S/CS 1.6 75; SDK file says 100 (HL2); cvar move.stopspeed"),
    "maxvelocity":     (3500.0, "u/s per axis", "SDK-movevars", ""),
    "stepsize":        (18.0, "u", "SDK-movevars", "= 0.4625 block"),
    "bounce":          (0.0, "", "SDK-movevars", "sv_bounce 0 -> overbounce 1.0"),
    "standable_nz":    (0.7, "", "SDK-gm.cpp", "ground if plane normal z >= 0.7 (Eden ramps 0.7071: see surf rule)"),
    "non_jump_vel":    (140.0, "u/s", "SDK-gm.cpp", "vz above this is never on ground"),
    "ground_probe":    (2.0, "u", "SDK-gm.cpp", "categorize-position trace distance"),
    "dist_epsilon":    (0.03125, "u", "SDK-gm.cpp", "trace back-off, Quake lineage"),
    "max_bumps":       (4, "", "SDK-gm.cpp", "slide-move iterations"),
    "max_clip_planes": (5, "", "SDK-gm.cpp", ""),
    "jump_impulse":    (math.sqrt(2 * 800.0 * 45.0), "u/s", "SDK-gm.cpp KZ", "268.328 = sqrt(2*800*45), 45 u apex"),
    "duck_time":       (0.4, "s", "SDK-defs", "TIME_TO_DUCK (non-TF)"),
    "unduck_time":     (0.2, "s", "SDK-defs", "TIME_TO_UNDUCK"),
    "duck_move_mult":  (1.0 / 3.0, "", "SDK-gm.cpp", "base 0.33333; CS:S community figure 0.34 (unpinned)"),
    "walk_mult":       (0.52, "", "PLAN", "shift-walk, CS:S community figure (unpinned); optional per SRC.2"),
    "water_wish_mult": (0.8, "", "SDK-gm.cpp", "wishspeed x0.8 in water"),
    "water_friction":  (4.0, "1/s", "SDK-gm.cpp", "water move uses sv_friction (no stopspeed); sv_waterfriction "
                                                  "is a surface scale, sv_wateraccelerate unused by base water move"),
    "swim_up_vel":     (100.0, "u/s", "SDK-gm.cpp", "jump held at water level >= 2 sets vz=100 each tick"),
    "swim_sink_wish":  (60.0, "u/s", "SDK-gm.cpp", "no input in water: wish z -= 60"),
    "waterjump_vz":    (256.0, "u/s", "SDK-gm.cpp", "water-jump push up, 2 s window"),
    "waterjump_probe": (8.0, "u", "SDK-defs", "WATERJUMP_HEIGHT"),
    "max_climb_speed": (200.0, "u/s", "SDK-defs", "ladder"),
    # Hulls and eyes. Source/HL2 base (SDK-rules, VDC-hull): stand 72, duck 36, eye 64 / 28, width 32.
    # CS:S ducked hull/eye differ (VDC Dimensions lists CS:S crouch clearance 48, unpinned); Emod uses Eden's.
    "hull_width":      (U_PER_BLOCK * 2.0 / 3.0, "u", "EDEN", "Eden boxbase 2/3 block = 25.95 u (Source 32 u = 0.82 b)"),
    "hull_stand":      (72.0, "u", "SDK-rules EDEN", "= 1.85 blocks"),
    "hull_duck":       (U_PER_BLOCK * 0.925, "u", "EDEN PLAN", "D5b: 36.0 u = 0.925 block (HL2 36, CS:S larger)"),
    "eye_stand":       (64.0, "u", "SDK-rules VDC-hull", "C2: 1.644 blocks (Eden today 0.8 x 1.85 = 1.48)"),
    "eye_duck":        (28.0 / 36.0 * U_PER_BLOCK * 0.925, "u", "SDK-rules PLAN",
                        "HL2 ratio 28/36 of the duck hull = 28.0 u = 0.719 block"),
    "maxspeed":        (5.85 * U_PER_BLOCK, "u/s", "EDEN PLAN", "U1: Eden walk SPEED_M*1.3 = 5.85 b/s = 227.7 u/s"),
}

def C(name):
    return CONSTANTS[name][0]

# Eden's ramps are the only planes with 0.7 <= nz < 1 (45 degrees); the surf rule (plan 4.4, U2) decides
# whether such a plane is ground.
RAMP_NZ_MAX = 0.99

# ---------------------------------------------------------------------------------------------------
# Vectors
def add(a, b): return (a[0] + b[0], a[1] + b[1], a[2] + b[2])
def sub(a, b): return (a[0] - b[0], a[1] - b[1], a[2] - b[2])
def mul(a, s): return (a[0] * s, a[1] * s, a[2] * s)
def dot(a, b): return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
def cross(a, b): return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])
def length(a): return math.sqrt(dot(a, a))
def hlen(a): return math.hypot(a[0], a[1])
def norm(a):
    l = length(a)
    return (mul(a, 1.0 / l), l) if l > 0 else ((0.0, 0.0, 0.0), 0.0)

# ---------------------------------------------------------------------------------------------------
# World: convex brushes as outward planes (n, d) with n.p <= d inside. Every brush carries its six axial
# planes, so for a box hull the expanded-plane test is the exact Minkowski sum (a wedge's sloped edges
# run along an axis, so no extra edge bevels are needed).
class Brush:
    def __init__(self, planes, ramp=False):
        self.planes, self.ramp = planes, ramp

def box_brush(mins, maxs):
    return Brush([((1, 0, 0), maxs[0]), ((-1, 0, 0), -mins[0]), ((0, 1, 0), maxs[1]),
                  ((0, -1, 0), -mins[1]), ((0, 0, 1), maxs[2]), ((0, 0, -1), -mins[2])])

def ramp_brush_py(x0, x1, y0, z0, size):
    """45-degree wedge rising toward +y: occupies y in [y0, y0+size], top surface z = z0 + (y - y0)."""
    s = 1.0 / math.sqrt(2.0)
    n = (0.0, -s, s)
    planes = [(n, dot(n, (0.0, y0, z0)))]
    planes += box_brush((x0, y0, z0), (x1, y0 + size, z0 + size)).planes
    return Brush(planes, ramp=True)

PARALLEL_TOL = 1e-6

class Trace:
    __slots__ = ("fraction", "endpos", "normal", "ramp", "startsolid", "allsolid")

def trace_hull(world, start, end, mins, maxs):
    """Swept AABB (origin + mins/maxs) from start to end: Quake-lineage brush clip with DIST_EPSILON."""
    eps = C("dist_epsilon")
    tr = Trace()
    tr.fraction, tr.normal, tr.ramp, tr.startsolid, tr.allsolid = 1.0, (0.0, 0.0, 0.0), False, False, False
    for b in world:
        enter, leave, hitn = -1.0, 1.0, None
        startout = getout = False
        miss = False
        for n, d in b.planes:
            ofs = tuple(mins[i] if n[i] > 0 else maxs[i] for i in range(3))
            dist = d - dot(n, ofs)
            d1, d2 = dot(n, start) - dist, dot(n, end) - dist
            if d2 > 0: getout = True
            if d1 > 0: startout = True
            # d2 >= d1 - PARALLEL_TOL: a slide along a plane we rest one epsilon from must miss it; with an
            # exact compare, float noise in the clipped velocity reads as "entering" and stalls the move.
            # SRC.3 (C++ float) needs the same rule with a float-sized tolerance.
            if d1 > 0 and (d2 >= eps or d2 >= d1 - PARALLEL_TOL):
                miss = True
                break
            if d1 <= 0 and d2 <= 0:
                continue
            if d1 > d2:
                f = (d1 - eps) / (d1 - d2)
                if f > enter: enter, hitn = f, n
            else:
                f = (d1 + eps) / (d1 - d2)
                if f < leave: leave = f
        if miss:
            continue
        if not startout:
            tr.startsolid = True
            if not getout:
                tr.allsolid = True
                tr.fraction = 0.0
            continue
        if enter < leave and enter > -1.0 and enter < tr.fraction:
            tr.fraction, tr.normal, tr.ramp = max(enter, 0.0), hitn, b.ramp and hitn[2] < RAMP_NZ_MAX
    tr.endpos = add(start, mul(sub(end, start), tr.fraction))
    return tr

# ---------------------------------------------------------------------------------------------------
# Player and the tick
class Cmd:
    def __init__(self, fwd=0.0, side=0.0, up=0.0, yaw=0.0, pitch=0.0, jump=False):
        self.fwd, self.side, self.up, self.yaw, self.pitch, self.jump = fwd, side, up, yaw, pitch, jump

class Player:
    def __init__(self, world, pos, vel=(0.0, 0.0, 0.0), airaccelerate=None):
        self.world, self.pos, self.vel = world, pos, vel
        w = C("hull_width") / 2.0
        self.mins, self.maxs = (-w, -w, 0.0), (w, w, C("hull_stand"))
        self.onground = False
        self.jump_held = False          # button state last tick (press-edge, U4)
        self.autobhop = False           # Experiments sub-toggle: held jump re-jumps
        self.maxspeed = C("maxspeed")
        self.airaccelerate = C("airaccelerate") if airaccelerate is None else airaccelerate
        self.water = 0                  # 0..3 water level (scenario-set; no liquid geometry in the model)
        self.jumped_this_tick = False

    def trace(self, a, b):
        return trace_hull(self.world, a, b, self.mins, self.maxs)

    # Plan 4.4 (U2): a ramp plane is ground only when we were grounded last tick, or slow onto it.
    def is_ground(self, tr, was_grounded):
        if tr.fraction >= 1.0 or tr.normal[2] < C("standable_nz"):
            return False
        if not tr.ramp:
            return True
        return was_grounded or (hlen(self.vel) <= 1.1 * self.maxspeed and abs(self.vel[2]) <= C("jump_impulse"))

    def categorize(self):
        was = self.onground
        if self.vel[2] > C("non_jump_vel"):
            self.onground = False
            return
        tr = self.trace(self.pos, sub(self.pos, (0.0, 0.0, C("ground_probe"))))
        self.onground = self.is_ground(tr, was)

    def wish(self, cmd, swim=False):
        cy, sy = math.cos(math.radians(cmd.yaw)), math.sin(math.radians(cmd.yaw))
        if swim:
            cp, sp = math.cos(math.radians(cmd.pitch)), math.sin(math.radians(cmd.pitch))
            fwd = (cy * cp, sy * cp, -sp)                      # pitch > 0 looks down (Source convention)
        else:
            fwd = (cy, sy, 0.0)
        right = (sy, -cy, 0.0)
        wv = add(mul(fwd, cmd.fwd), mul(right, cmd.side))
        if not swim:
            wv = (wv[0], wv[1], 0.0)
        return wv

    def accelerate(self, wishdir, wishspeed, accel, dt):
        add_speed = wishspeed - dot(self.vel, wishdir)
        if add_speed <= 0:
            return
        a = min(accel * dt * wishspeed, add_speed)
        self.vel = add(self.vel, mul(wishdir, a))

    def air_accelerate(self, wishdir, wishspeed, dt):
        cap = min(wishspeed, C("air_wish_cap"))
        add_speed = cap - dot(self.vel, wishdir)
        if add_speed <= 0:
            return
        a = min(self.airaccelerate * wishspeed * dt, add_speed)   # uncapped wishspeed scales the rate
        self.vel = add(self.vel, mul(wishdir, a))

    def friction(self, dt):
        speed = length(self.vel)
        if speed < 0.1:
            return
        drop = max(speed, C("stopspeed")) * C("friction") * dt
        ns = max(speed - drop, 0.0)
        self.vel = mul(self.vel, ns / speed)

    def clamp_velocity(self):
        m = C("maxvelocity")
        self.vel = tuple(max(-m, min(m, v)) for v in self.vel)

    @staticmethod
    def clip(v, n, overbounce):
        out = sub(v, mul(n, dot(v, n) * overbounce))
        adj = dot(out, n)
        if adj < 0:
            out = sub(out, mul(n, adj))
        return out

    def slide_move(self, dt):
        """Up to 4 bumps; clip against every plane touched this move; two planes -> slide along the crease."""
        ob = 1.0 + C("bounce")
        primal = original = self.vel
        planes = []
        time_left = dt
        for _ in range(C("max_bumps")):
            if length(self.vel) == 0:
                break
            tr = self.trace(self.pos, add(self.pos, mul(self.vel, time_left)))
            if tr.allsolid:
                self.vel = (0.0, 0.0, 0.0)
                return
            if tr.fraction > 0:
                self.pos = tr.endpos
                original = self.vel
                planes = []
            if tr.fraction == 1.0:
                break
            time_left -= time_left * tr.fraction
            if len(planes) >= C("max_clip_planes"):
                self.vel = (0.0, 0.0, 0.0)
                break
            if any(dot(tr.normal, q) > 0.99 for q in planes):
                # Same plane again (float noise on a slide): nudge off it instead of a degenerate
                # crease (Quake II/III lineage).
                self.vel = add(tr.normal, self.vel)
                continue
            planes.append(tr.normal)
            if len(planes) == 1 and not self.onground:
                self.vel = original = self.clip(original, planes[0], ob)
                continue
            for i, pi in enumerate(planes):
                v = self.clip(original, pi, ob)
                if all(dot(v, pj) >= 0 for j, pj in enumerate(planes) if j != i):
                    self.vel = v
                    break
            else:
                if len(planes) != 2:
                    self.vel = (0.0, 0.0, 0.0)
                    break
                d, _ = norm(cross(planes[0], planes[1]))
                self.vel = mul(d, dot(d, self.vel))
            if dot(self.vel, primal) <= 0:
                self.vel = (0.0, 0.0, 0.0)
                break

    def stay_on_ground(self):
        up = self.trace(self.pos, add(self.pos, (0.0, 0.0, 2.0)))
        start = up.endpos
        tr = self.trace(start, sub(self.pos, (0.0, 0.0, C("stepsize"))))
        if 0 < tr.fraction < 1 and not tr.startsolid and tr.normal[2] >= C("standable_nz"):
            self.pos = tr.endpos

    def step_move(self, dt):
        p0, v0 = self.pos, self.vel
        self.slide_move(dt)                                   # path 1: plain slide
        pd, vd = self.pos, self.vel
        self.pos, self.vel = p0, v0                           # path 2: up a step, slide, back down
        lift = C("stepsize") + C("dist_epsilon")
        tr = self.trace(self.pos, add(self.pos, (0.0, 0.0, lift)))
        if not tr.startsolid and not tr.allsolid:
            self.pos = tr.endpos
        self.slide_move(dt)
        tr = self.trace(self.pos, sub(self.pos, (0.0, 0.0, lift)))
        if not tr.startsolid and not tr.allsolid:
            self.pos = tr.endpos
        if tr.normal[2] < C("standable_nz"):                  # landed on nothing walkable: take path 1
            self.pos, self.vel = pd, vd
            return
        if hlen(sub(pd, p0)) > hlen(sub(self.pos, p0)):
            self.pos, self.vel = pd, vd
        else:
            self.vel = (self.vel[0], self.vel[1], vd[2])

    def walk_move(self, cmd, dt):
        wv = self.wish(cmd)
        wishdir, ws = norm(wv)
        ws = min(ws, self.maxspeed)
        self.vel = (self.vel[0], self.vel[1], 0.0)
        self.accelerate(wishdir, ws, C("accelerate"), dt)
        self.vel = (self.vel[0], self.vel[1], 0.0)
        if length(self.vel) < 1.0:
            self.vel = (0.0, 0.0, 0.0)
            return
        dest = add(self.pos, mul(self.vel, dt))
        tr = self.trace(self.pos, dest)
        if tr.fraction == 1.0:
            self.pos = tr.endpos
        else:
            self.step_move(dt)
        self.stay_on_ground()

    def air_move(self, cmd, dt):
        wishdir, ws = norm(self.wish(cmd))
        ws = min(ws, self.maxspeed)
        self.air_accelerate(wishdir, ws, dt)
        self.slide_move(dt)

    def water_move(self, cmd, dt):
        wv = self.wish(cmd, swim=True)
        if cmd.jump:
            wv = add(wv, (0.0, 0.0, self.maxspeed))
        elif cmd.fwd == 0 and cmd.side == 0 and cmd.up == 0:
            wv = add(wv, (0.0, 0.0, -C("swim_sink_wish")))
        else:
            wv = add(wv, (0.0, 0.0, cmd.up))
        wishdir, ws = norm(wv)
        ws = min(ws, self.maxspeed) * C("water_wish_mult")
        speed = length(self.vel)
        if speed > 0:
            ns = speed - dt * speed * C("water_friction")
            ns = 0.0 if ns < 0.1 else ns
            self.vel = mul(self.vel, ns / speed)
        else:
            ns = 0.0
        if ws >= 0.1:
            add_speed = ws - ns
            if add_speed > 0:
                a = min(C("accelerate") * ws * dt, add_speed)
                self.vel = add(self.vel, mul(wishdir, a))
        self.slide_move(dt)

    def try_jump(self, cmd):
        """Press edge only (U4) unless auto-bhop. MODEL: vz is SET to the impulse at the start of the
        jump tick, before the tick's first half-gravity step."""
        pressed = cmd.jump and (self.autobhop or not self.jump_held)
        if not (self.onground and pressed):
            return False
        self.onground = False
        self.vel = (self.vel[0], self.vel[1], C("jump_impulse"))
        return True

    def tick(self, cmd):
        dt = C("tick_interval")
        g = C("gravity")
        self.jumped_this_tick = False
        if self.water >= 2:
            if cmd.jump:
                self.onground = False
                self.vel = (self.vel[0], self.vel[1], C("swim_up_vel"))
            self.water_move(cmd, dt)
            self.categorize()
        else:
            self.jumped_this_tick = self.try_jump(cmd)
            # MODEL: leapfrog gravity, half before the move and half after (Quake lineage)
            if not self.onground:
                self.vel = (self.vel[0], self.vel[1], self.vel[2] - 0.5 * g * dt)
            if self.onground:
                self.vel = (self.vel[0], self.vel[1], 0.0)
                self.friction(dt)                             # skipped on the jump tick: bhop keeps speed
            self.clamp_velocity()
            if self.onground:
                self.walk_move(cmd, dt)
            else:
                self.air_move(cmd, dt)
            self.categorize()
            self.clamp_velocity()
            if not self.onground:
                self.vel = (self.vel[0], self.vel[1], self.vel[2] - 0.5 * g * dt)
            else:
                self.vel = (self.vel[0], self.vel[1], 0.0)
        self.jump_held = cmd.jump

# ---------------------------------------------------------------------------------------------------
# Scenarios
BIG = 100000.0
def floor_world():
    return [box_brush((-BIG, -BIG, -64.0), (BIG, BIG, 0.0))]

def on_floor(world, vel=(0.0, 0.0, 0.0), **kw):
    p = Player(world, (0.0, 0.0, C("dist_epsilon")), vel, **kw)   # resting = one epsilon above (trace rule)
    p.categorize()
    return p

def r(x, nd=3):
    return float(f"{x:.{nd}f}")

def ticks_to_s(n):
    return r(n * C("tick_interval"), 4)

def sc_ground_accel():
    p = on_floor(floor_world())
    ms = p.maxspeed
    cmd = Cmd(fwd=ms, yaw=0.0)
    marks, out, dist = {0.5: None, 0.9: None, 0.99: None, 1.0: None}, {}, 0.0
    samples = {}
    for t in range(1, 200):
        x0 = p.pos[0]
        p.tick(cmd)
        dist += p.pos[0] - x0
        s = hlen(p.vel)
        for k in marks:
            if marks[k] is None and s >= ms * k - 1e-6:
                marks[k] = t
        if t in (1, 2, 5, 10, 20, 33, 67):
            samples[f"speed_after_{t}_ticks"] = r(s)
        if t == 67:
            out["distance_after_67_ticks_u"] = r(dist)
    for k, t in marks.items():
        out[f"ticks_to_{int(k * 100)}pct"] = t
    out.update(samples)
    out["top_speed_u_s"] = r(hlen(p.vel))
    return out

def sc_stop(v0):
    p = on_floor(floor_world(), (v0, 0.0, 0.0))
    dist, t = 0.0, 0
    while hlen(p.vel) > 0 and t < 1000:
        x0 = p.pos[0]
        p.tick(Cmd())
        dist += p.pos[0] - x0
        t += 1
    return {"from_u_s": r(v0), "ticks_to_stop": t, "time_s": ticks_to_s(t),
            "distance_u": r(dist), "distance_blocks": r(dist / U_PER_BLOCK)}

def sc_jump():
    p = on_floor(floor_world())
    apex, t, landed = 0.0, 0, None
    jump = True
    while t < 200:
        p.tick(Cmd(jump=jump))
        jump = False
        t += 1
        apex = max(apex, p.pos[2])
        if p.onground:
            landed = t
            break
    j = C("jump_impulse")
    lift = C("hull_stand") - C("hull_duck")
    return {"impulse_u_s": r(j), "analytic_apex_u": r(j * j / (2 * C("gravity"))),
            "model_apex_u": r(apex), "model_apex_blocks": r(apex / U_PER_BLOCK),
            "airtime_ticks": landed, "airtime_s": ticks_to_s(landed),
            "duck_jump_reach_u": r(apex + lift), "two_block_ledge_u": r(2 * U_PER_BLOCK),
            "duck_jump_margin_u": r(apex + lift - 2 * U_PER_BLOCK),
            "stand_jump_clears_1_block": apex >= U_PER_BLOCK, "stand_jump_clears_2_blocks": apex >= 2 * U_PER_BLOCK}

def perfect_strafe_cmd(p, side_sign):
    """Pure side strafe whose wish direction is perpendicular to horizontal velocity (cos theta = 0):
    the per-tick optimum when airaccelerate*wishspeed*dt >= 30, giving |v'|^2 = |v|^2 + 30^2."""
    vyaw = math.degrees(math.atan2(p.vel[1], p.vel[0]))
    # right = (sin yaw, -cos yaw); side>0 pushes right. Want wishdir = velocity rotated by -/+90 deg.
    return Cmd(side=side_sign * p.maxspeed, yaw=vyaw)

def sc_air_strafe_open(aa):
    """Open air, 1 s (67 ticks) of perfect strafe from 250 u/s, no ground. Pure air-accelerate law."""
    p = Player([], (0.0, 0.0, 1000.0), (250.0, 0.0, 0.0), airaccelerate=aa)
    yaw_prev = 0.0
    turn = []
    for _ in range(67):
        cmd = perfect_strafe_cmd(p, 1.0)
        turn.append(abs(cmd.yaw - yaw_prev))
        yaw_prev = cmd.yaw
        p.tick(cmd)
    v = hlen(p.vel)
    return {"airaccelerate": aa, "v0_u_s": 250.0, "ticks": 67, "v_after_u_s": r(v),
            "closed_form_sqrt(v0^2+900n)": r(math.sqrt(250.0 ** 2 + 900 * 67)),
            "first_tick_turn_deg": r(math.degrees(math.atan2(30.0, 250.0))),
            "turn_rate_at_250_deg_s": r(math.degrees(math.atan2(30.0, 250.0)) / C("tick_interval"), 1)}

def sc_strafe_hops(hops=10, aa=None):
    """Run-up to maxspeed, then perfect bhops (press on every landing tick, release in the air) with
    perfect air strafe on every tick, the jump tick included (it is an air tick after the jump)."""
    p = on_floor(floor_world(), (C("maxspeed"), 0.0, 0.0), airaccelerate=aa)
    out = {"airaccelerate": p.airaccelerate, "v0_u_s": r(hlen(p.vel)), "landing_speeds_u_s": []}
    side, h, t, air_ticks, friction_ticks = 1.0, 0, 0, 0, 0
    while h < hops and t < 5000:
        cmd = perfect_strafe_cmd(p, side)
        cmd.jump = p.onground
        p.tick(cmd)
        t += 1
        if not p.jumped_this_tick and cmd.jump:
            friction_ticks += 1
        if not p.onground:
            air_ticks += 1
        else:
            h += 1
            side = -side
            out["landing_speeds_u_s"].append(r(hlen(p.vel)))
    out["ticks_per_hop"] = r(t / hops, 2)
    out["ground_friction_ticks"] = friction_ticks
    out["closed_form_after_10_hops"] = r(math.sqrt(C("maxspeed") ** 2 + 900 * t))
    return out

def sc_bhop_retention(v0, late_ticks):
    """No input (pure momentum), jump `late_ticks` ground ticks after each landing; 5 hops."""
    p = on_floor(floor_world(), (v0, 0.0, 0.0))
    speeds, wait, t = [], 0, 0
    while len(speeds) < 5 and t < 5000:
        jump = p.onground and wait >= late_ticks
        was_air = not p.onground
        if p.onground:
            wait += 1
        p.tick(Cmd(jump=jump))
        t += 1
        if not p.onground:
            wait = 0
        elif was_air:
            speeds.append(r(hlen(p.vel)))
    return {"v0_u_s": v0, "late_ticks": late_ticks, "landing_speeds_u_s": speeds,
            "retention_per_hop": r(speeds[1] / speeds[0], 4),
            "closed_form_0.94^late": r((1 - C("friction") * C("tick_interval")) ** late_ticks, 4)}

def surf_world():
    # A long 45-degree ramp (Eden ramp stack) rising toward +y; floor far below.
    return [ramp_brush_py(-BIG, BIG, 0.0, 0.0, 4000.0), box_brush((-BIG, -BIG, -64.0), (BIG, BIG, -2000.0))]

def sc_surf(hold_into_ramp):
    w = surf_world()
    # Feet on the slope: the hull's bottom face meets the slope at its +y edge, where z = y + hw.
    hw = C("hull_width") / 2.0
    y = 2000.0
    p = Player(w, (0.0, y, (y + hw) + 0.1), (800.0, 0.0, 0.0))
    p.categorize()
    z0, ground_ticks = p.pos[2], 0
    for _ in range(67):
        cmd = Cmd(side=-p.maxspeed if hold_into_ramp else 0.0, yaw=0.0)   # side<0 = left = +y = into ramp
        p.tick(cmd)
        ground_ticks += p.onground
    return {"hold_into_ramp": hold_into_ramp, "v0_along_ramp_u_s": 800.0, "ticks": 67,
            "vx_after_u_s": r(p.vel[0]), "height_change_u": r(p.pos[2] - z0),
            "downslope_speed_u_s": r(math.hypot(p.vel[1], p.vel[2]) * (1 if p.vel[2] < 0 else -1)),
            "analytic_no_input_drop_u": r(0.5 * C("gravity") * 0.5 * (67 * C("tick_interval")) ** 2),
            "ticks_on_ground": ground_ticks}

def ramp_walk_world():
    return [box_brush((-BIG, -BIG, -64.0), (BIG, BIG, 0.0)), ramp_brush_py(-BIG, BIG, 100.0, 0.0, 4 * U_PER_BLOCK),
            box_brush((-BIG, 100.0 + 4 * U_PER_BLOCK, 0.0), (BIG, BIG, 4 * U_PER_BLOCK))]

def sc_ramp_walk(up):
    w = ramp_walk_world()
    top = 100.0 + 4 * U_PER_BLOCK
    if up:
        p = Player(w, (0.0, 0.0, C("dist_epsilon")))
        yaw = 90.0
    else:
        p = Player(w, (0.0, top + 60.0, 4 * U_PER_BLOCK + C("dist_epsilon")))
        yaw = -90.0
    p.categorize()
    p.vel = (0.0, p.maxspeed * (1 if up else -1), 0.0)
    on_ramp_speeds, ground_ticks, ticks = [], 0, 0
    for _ in range(150):
        p.tick(Cmd(fwd=p.maxspeed, yaw=yaw))
        ticks += 1
        ground_ticks += p.onground
        if 100.0 + 20 < p.pos[1] < top - 20:
            on_ramp_speeds.append(hlen(p.vel))
    return {"direction": "up" if up else "down", "ticks": ticks, "ticks_on_ground": ground_ticks,
            "final_height_u": r(p.pos[2]), "ramp_top_u": r(4 * U_PER_BLOCK),
            "min_hspeed_on_ramp_u_s": r(min(on_ramp_speeds)) if on_ramp_speeds else None,
            "mean_hspeed_on_ramp_u_s": r(sum(on_ramp_speeds) / len(on_ramp_speeds)) if on_ramp_speeds else None}

def sc_land_on_ramp(v0):
    """Fall onto the ramp from the air: fast along it must stay a surf plane (U2); slow, or a clip that
    leaves you slow (landing uphill), makes it ground."""
    w = ramp_walk_world()
    p = Player(w, (0.0, 160.0, 120.0), v0)
    ground_ticks, first = 0, None
    for t in range(30):
        p.tick(Cmd())
        ground_ticks += p.onground
        if p.onground and first is None:
            first = t + 1
    return {"v0": list(v0), "ticks": 30, "ticks_on_ground": ground_ticks, "first_ground_tick": first}

def sc_swim_up():
    p = Player([], (0.0, 0.0, 0.0))
    p.water = 3
    speeds = []
    for _ in range(67):
        p.tick(Cmd(jump=True))
        speeds.append(p.vel[2])
    rise = speeds[-1]
    return {"steady_rise_u_s": r(rise), "steady_rise_blocks_s": r(rise / U_PER_BLOCK),
            "rise_3_blocks_s_at_steady": r(3 * U_PER_BLOCK / rise)}

def vectors():
    return {
        "units": {"u_per_block": r(U_PER_BLOCK, 4), "tick_hz": r(1.0 / C("tick_interval"), 3),
                  "maxspeed_u_s": r(C("maxspeed"))},
        "ground_accel": sc_ground_accel(),
        "stop_from_maxspeed": sc_stop(C("maxspeed")),
        "stop_from_400": sc_stop(400.0),
        "jump": sc_jump(),
        "air_strafe_open_aa100": sc_air_strafe_open(100.0),
        "air_strafe_open_aa10": sc_air_strafe_open(10.0),
        "strafe_hops_aa100": sc_strafe_hops(10),
        "bhop_retention_perfect": sc_bhop_retention(400.0, 0),
        "bhop_retention_1_late": sc_bhop_retention(400.0, 1),
        "bhop_retention_3_late": sc_bhop_retention(400.0, 3),
        "surf_no_input": sc_surf(False),
        "surf_hold_into_ramp": sc_surf(True),
        "ramp_walk_up": sc_ramp_walk(True),
        "ramp_walk_down": sc_ramp_walk(False),
        "land_on_ramp_fast_along": sc_land_on_ramp((600.0, 0.0, -100.0)),
        "land_on_ramp_slow": sc_land_on_ramp((100.0, 0.0, -100.0)),
        "land_on_ramp_fast_uphill": sc_land_on_ramp((0.0, 400.0, -100.0)),
        "swim_up": sc_swim_up(),
    }

def text(v):
    lines = ["# srclike-pm-ref.py vectors (SRC.1). Units: u = Source unit, 1 block = %.4f u, tick %.3f Hz."
             % (U_PER_BLOCK, 1.0 / C("tick_interval")), ""]
    lines.append("## constants")
    for k, (val, unit, src, note) in CONSTANTS.items():
        vs = f"{val:.6g}" if isinstance(val, float) else str(val)
        lines.append(f"{k:16s} {vs:>12s} {unit:12s} [{src}] {note}".rstrip())
    for sec, d in v.items():
        lines += ["", f"## {sec}"]
        for k, val in d.items():
            lines.append(f"{k} = {json.dumps(val)}")
    return "\n".join(lines) + "\n"

FIXTURE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fixtures", "srclike-pm", "vectors.txt")

def main(argv):
    v = vectors()
    if "--json" in argv:
        print(json.dumps({"constants": {k: c[0] for k, c in CONSTANTS.items()}, "vectors": v}, indent=1))
        return 0
    t = text(v)
    if "--write" in argv:
        os.makedirs(os.path.dirname(FIXTURE), exist_ok=True)
        with open(FIXTURE, "w") as f:
            f.write(t)
        print("wrote", FIXTURE)
        return 0
    if "--check" in argv:
        with open(FIXTURE) as f:
            want = f.read()
        if want != t:
            import difflib
            sys.stdout.writelines(difflib.unified_diff(want.splitlines(True), t.splitlines(True), "fixture", "now"))
            return 1
        print("srclike-pm-ref: vectors match fixture")
        return 0
    sys.stdout.write(t)
    return 0

if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
